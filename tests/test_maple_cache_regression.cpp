#include "mlx/mlx.h"
#include "maple_model.h"
#include "tokenizer.h"
#include <iostream>
#include <vector>

using namespace mlx::core;
using namespace samosa::maple;

int main() {
    auto model = load_maple_model("models/maple");
    MapleTokenizer tokenizer("models/maple");

    std::string prompt = "List three benefits of regular exercise.";
    std::vector<Message> messages = {{"user", prompt}};
    auto formatted = tokenizer.apply_chat_template(messages, true);
    auto input_tokens = tokenizer.encode(formatted);

    std::vector<KVCache*> caches;
    for (int layer = 0; layer < model.args().num_hidden_layers; layer++) {
        caches.push_back(new RotatingKVCache(model.args().sliding_window));
    }

    // Incremental prefill: just the first token
    array x = array({input_tokens[0]}, {1, 1}, int32);
    auto logits = model(x, &caches);
    eval(logits);
    for (auto c : caches) { auto rc = (RotatingKVCache*)c; eval(rc->keys, rc->values); }

    std::vector<int> prefix = {input_tokens[0]};

    for (size_t step = 1; step < input_tokens.size() + 10; step++) {
        int next_token = -1;
        if (step < input_tokens.size()) {
            next_token = input_tokens[step];
        } else {
            auto next_logits = slice(logits, {0, 0, 0}, {1, 1, logits.shape(-1)});
            auto next_t_arr = argmax(next_logits, -1);
            eval(next_t_arr);
            next_token = next_t_arr.item<int>();
        }

        prefix.push_back(next_token);

        // Cached path
        array x_cached = array({next_token}, {1, 1}, int32);
        logits = model(x_cached, &caches);
        auto cached_logits = slice(logits, {0, 0, 0}, {1, 1, logits.shape(-1)});
        eval(cached_logits);
        for (auto c : caches) { auto rc = (RotatingKVCache*)c; eval(rc->keys, rc->values); }

        // Cacheless path
        array x_cacheless = array(prefix.data(), {1, (int)prefix.size()}, int32);
        auto logits_cl = model(x_cacheless, nullptr);
        auto cacheless_logits = slice(logits_cl, {0, logits_cl.shape(1)-1, 0}, {1, logits_cl.shape(1), logits_cl.shape(-1)});
        eval(cacheless_logits);

        auto diff = abs(cached_logits - cacheless_logits);
        auto max_diff = max(diff).item<float>();

        int argmax_c = argmax(cached_logits, -1).item<int>();
        int argmax_cl = argmax(cacheless_logits, -1).item<int>();

        std::cout << "Step " << step << " | prefix size " << prefix.size()
                  << " | max diff: " << max_diff
                  << " | argmax (cached): " << argmax_c
                  << " | argmax (cacheless): " << argmax_cl << "\n";

        if (argmax_c != argmax_cl) {
            std::cout << "MISMATCH FOUND at step " << step << "\n";
            break;
        }
    }

    for (auto c : caches) delete c;
    return 0;
}
