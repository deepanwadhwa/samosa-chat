#include "mlx/mlx.h"
#include "maple_model.h"
#include "tokenizer.h"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <iomanip>

using namespace mlx::core;
using namespace samosa::maple;

std::vector<std::pair<std::string, std::vector<int>>> parse_json(const std::string& path) {
    std::vector<std::pair<std::string, std::vector<int>>> refs;
    std::ifstream f(path);
    std::string line;
    std::string current_prompt;
    std::vector<int> current_tokens;

    while (std::getline(f, line)) {
        if (line.find("\"prompt\":") != std::string::npos) {
            size_t start = line.find(":") + 3;
            size_t end = line.rfind("\"");
            current_prompt = line.substr(start, end - start);
        } else if (line.find("\"generated_tokens\":") != std::string::npos) {
            current_tokens.clear();
            std::string arr_line;
            std::getline(f, arr_line);
            while (arr_line.find("]") == std::string::npos) {
                size_t num_start = arr_line.find_first_of("0123456789");
                if (num_start != std::string::npos) {
                    size_t num_end = arr_line.find_first_not_of("0123456789", num_start);
                    int val = std::stoi(arr_line.substr(num_start, num_end - num_start));
                    current_tokens.push_back(val);
                }
                std::getline(f, arr_line);
            }
            refs.push_back({current_prompt, current_tokens});
        }
    }
    return refs;
}

void print_top5(const array& logits, const std::string& prefix) {
    auto sorted_indices = argsort(logits, -1);
    eval(sorted_indices);
    eval(logits);

    std::cout << prefix << " Top-5: ";
    int v_size = logits.shape(-1);
    const int* idx_ptr = sorted_indices.data<int>();
    const float* val_ptr = logits.data<float>();
    for (int i = 1; i <= 5; i++) {
        int idx = idx_ptr[v_size - i];
        float val = val_ptr[idx];
        std::cout << "[" << idx << ": " << std::fixed << std::setprecision(4) << val << "] ";
    }
    std::cout << "\n";
}

int main() {
    auto model = load_maple_model("models/maple");
    MapleTokenizer tokenizer("models/maple");
    auto refs = parse_json("tests/fixtures/maple/greedy_parity_refs.json");

    for (size_t i = 0; i < refs.size(); i++) {
        std::string prompt = refs[i].first;
        auto expected = refs[i].second;

        std::vector<Message> messages = {{"user", prompt}};
        auto formatted = tokenizer.apply_chat_template(messages, true);
        auto input_tokens = tokenizer.encode(formatted);

        // Find first mismatch by running incremental decode
        std::vector<KVCache*> caches;
        for (int layer = 0; layer < model.args().num_hidden_layers; layer++) {
            caches.push_back(new RotatingKVCache(model.args().sliding_window));
        }

        array x = array(input_tokens.data(), {1, (int)input_tokens.size()}, int32);
        auto logits = model(x, &caches);
        auto next_logits = squeeze(slice(logits, {0, logits.shape(1)-1, 0}, {logits.shape(0), logits.shape(1), logits.shape(2)}), 1);
        auto next_t_arr = argmax(next_logits, -1);
        eval(next_t_arr);
        for (auto c : caches) {
            auto rc = (RotatingKVCache*)c;
            eval(rc->keys, rc->values);
        }
        int next_t = next_t_arr.item<int>();

        std::vector<int> generated = {next_t};
        int first_mismatch = -1;

        if (expected[0] != next_t) {
            first_mismatch = 0;
        } else {
            for (int step = 1; step < 32; step++) {
                array t = array({next_t}, {1, 1}, int32);
                logits = model(t, &caches);
                next_t_arr = argmax(logits, -1);
                eval(next_t_arr);
                for (auto c : caches) {
                    auto rc = (RotatingKVCache*)c;
                    eval(rc->keys, rc->values);
                }
                next_t = next_t_arr.item<int>();
                generated.push_back(next_t);

                if (expected[step] != next_t) {
                    first_mismatch = step;
                    break;
                }
            }
        }

        for (auto c : caches) delete c;

        if (first_mismatch != -1) {
            std::cout << "----------------------------------------\n";
            std::cout << "PROMPT: " << prompt << "\n";
            std::cout << "DECODE POS: " << first_mismatch << "\n";
            std::cout << "GOLDEN EXPECTED: " << expected[first_mismatch] << "\n";

            // Build the exact prefix up to the mismatch
            std::vector<int> prefix = input_tokens;
            for (int j = 0; j < first_mismatch; j++) {
                prefix.push_back(expected[j]);
            }

            // PATH A: Incremental Cached Decode
            std::vector<KVCache*> caches_a;
            for (int layer = 0; layer < model.args().num_hidden_layers; layer++) {
                caches_a.push_back(new RotatingKVCache(model.args().sliding_window));
            }

            array x_a(0);
            array logits_a(0);
            if (prefix.size() > 1) {
                // Prefill up to last token
                std::vector<int> pre_prefix(prefix.begin(), prefix.end() - 1);
                x_a = array(pre_prefix.data(), {1, (int)pre_prefix.size()}, int32);
                auto pre_logits = model(x_a, &caches_a);
                eval(pre_logits);
                for (auto c : caches_a) {
                    auto rc = (RotatingKVCache*)c;
                    eval(rc->keys, rc->values);
                }
            }

            // Decode last token
            x_a = array({prefix.back()}, {1, 1}, int32);
            logits_a = model(x_a, &caches_a);
            auto next_logits_a = slice(logits_a, {0, 0, 0}, {1, 1, logits_a.shape(-1)});
            auto next_t_arr_a = argmax(next_logits_a, -1);
            eval(next_t_arr_a);
            int next_t_a = next_t_arr_a.item<int>();

            // PATH B: Full-Prefix Cacheless
            array x_b = array(prefix.data(), {1, (int)prefix.size()}, int32);
            auto logits_b = model(x_b, nullptr);
            auto next_logits_b = slice(logits_b, {0, logits_b.shape(1)-1, 0}, {1, logits_b.shape(1), logits_b.shape(-1)});
            auto next_t_arr_b = argmax(next_logits_b, -1);
            eval(next_t_arr_b);
            int next_t_b = next_t_arr_b.item<int>();

            std::cout << "CACHED NATIVE: " << next_t_a << "\n";
            std::cout << "CACHELESS NATIVE: " << next_t_b << "\n";

            auto diff = abs(next_logits_a - next_logits_b);
            auto max_diff = max(diff).item<float>();
            std::cout << "MAX LOGIT DIFF: " << std::fixed << std::setprecision(6) << max_diff << "\n";

            print_top5(next_logits_a, "CACHED NATIVE");
            print_top5(next_logits_b, "CACHELESS NATIVE");

            for (auto c : caches_a) delete c;
        }
    }

    return 0;
}
