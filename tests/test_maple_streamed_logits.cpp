#include "json.h"
#include "maple/maple_model.h"
#include "maple/tokenizer.h"
#include "mlx/mlx.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace mlx::core;
using namespace samosa::maple;

int main() {
    try {
        std::ifstream input("tests/fixtures/maple/raw_parity_refs_3.json");
        std::ostringstream text;
        text << input.rdbuf();
        jval* root = json_parse(text.str().c_str(), nullptr);
        if (!root || root->t != J_ARR || root->len < 2) {
            throw std::runtime_error("invalid raw Maple parity fixture");
        }
        jval* ref = root->kids[1];
        jval* prompt_value = json_get(ref, "prompt");
        jval* argmax_value = json_get(ref, "argmax_id");
        jval* logits_value = json_get(ref, "logits");
        if (!prompt_value || prompt_value->t != J_STR ||
            !argmax_value || argmax_value->t != J_NUM ||
            !logits_value || logits_value->t != J_ARR) {
            json_free(root);
            throw std::runtime_error("incomplete raw Maple parity fixture");
        }

        auto model = load_maple_model("models/maple");
        MapleTokenizer tokenizer("models/maple");
        std::vector<Message> messages = {{"user", prompt_value->str}};
        auto formatted = tokenizer.apply_chat_template(messages, true);
        auto tokens = tokenizer.encode(formatted);
        array token_array(tokens.data(), {1, static_cast<int>(tokens.size())}, int32);

        std::vector<KVCache*> caches;
        for (int layer = 0; layer < model.args().num_hidden_layers; ++layer) {
            if (model.args().layer_types[layer] == "sliding_attention") {
                caches.push_back(new RotatingKVCache(model.args().sliding_window));
            } else {
                caches.push_back(new KVCache());
            }
        }
        auto logits = reshape(
            astype(model.run_token_sequence(token_array, &caches), float32), {-1});
        eval(logits);
        if (logits.size() != static_cast<size_t>(logits_value->len)) {
            throw std::runtime_error("raw Maple parity vocabulary mismatch");
        }

        const float* actual = logits.data<float>();
        double sum_abs = 0.0;
        float max_abs = 0.0f;
        int max_index = -1;
        for (int i = 0; i < logits_value->len; ++i) {
            const float expected = static_cast<float>(logits_value->kids[i]->num);
            const float error = std::fabs(actual[i] - expected);
            sum_abs += error;
            if (error > max_abs) {
                max_abs = error;
                max_index = i;
            }
        }
        const int expected_id = static_cast<int>(argmax_value->num);
        const int actual_id = argmax(logits).item<int>();
        std::cout << "prompt_tokens=" << tokens.size() << "\n"
                  << "expected_argmax=" << expected_id << "\n"
                  << "actual_argmax=" << actual_id << "\n"
                  << "expected_score=" << logits_value->kids[expected_id]->num << "\n"
                  << "actual_score=" << actual[expected_id] << "\n"
                  << "max_abs_error=" << max_abs << " at " << max_index << "\n"
                  << "mean_abs_error=" << (sum_abs / logits.size()) << "\n";

        for (auto* cache : caches) delete cache;
        json_free(root);
        return actual_id == expected_id && max_abs == 0.0f ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Maple streamed-logit diagnostic: " << error.what() << "\n";
        return 1;
    }
}
