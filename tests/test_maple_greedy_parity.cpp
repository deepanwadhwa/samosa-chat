#include "mlx/mlx.h"
#include "maple_model.h"
#include "tokenizer.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>

// Since we don't have a JSON parser natively imported in tests, we'll just write a quick string parser for the simple JSON we generated.
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
                // simple number extraction
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

using namespace mlx::core;
using namespace samosa::maple;

int main() {
    std::cout << "=== 3. GREEDY PARITY GATE ===\n";
    auto model = load_maple_model("models/maple");
    MapleTokenizer tokenizer("models/maple");

    auto refs = parse_json("tests/fixtures/maple/greedy_parity_refs.json");
    if (refs.empty()) {
        std::cout << "FAIL: Could not load golden fixtures.\n";
        return 1;
    }

    std::cout << "Prompt | Matching tokens | First mismatch | PASS/FAIL\n";
    std::cout << "---------------------------------------------------------\n";

    const char* reeval_env = std::getenv("MAPLE_REEVAL_RESIDENT");
    const bool reeval_resident = reeval_env && std::string(reeval_env) == "1";
    if (reeval_resident && model.streaming_enabled()) {
        std::cerr << "MAPLE_REEVAL_RESIDENT=1 requires "
                     "SAMOSA_MAPLE_ALLOW_FULL_RESIDENT=1\n";
        return 2;
    }

    size_t prompt_start = 0;
    if (const char* start = std::getenv("MAPLE_PARITY_START")) {
        prompt_start = std::min(refs.size(), static_cast<size_t>(std::strtoul(start, nullptr, 10)));
    }
    size_t prompt_limit = refs.size() - prompt_start;
    if (const char* limit = std::getenv("MAPLE_PARITY_LIMIT")) {
        prompt_limit = std::min(prompt_limit, static_cast<size_t>(std::strtoul(limit, nullptr, 10)));
    }
    int all_passed = 0;
    int total_tokens = 0;

    for (size_t run = 0; run < prompt_limit; run++) {
        const size_t i = prompt_start + run;
        std::string prompt = refs[i].first;
        auto expected = refs[i].second;

        std::vector<Message> messages = {{"user", prompt}};
        auto formatted = tokenizer.apply_chat_template(messages, true);
        auto input_tokens = tokenizer.encode(formatted);
        array x = array(input_tokens.data(), {1, (int)input_tokens.size()}, int32);

        std::vector<KVCache*> caches;
        if (!reeval_resident) {
            for (int layer = 0; layer < model.args().num_hidden_layers; layer++) {
                if (layer < static_cast<int>(model.args().layer_types.size()) &&
                    model.args().layer_types[layer] == "sliding_attention") {
                    caches.push_back(new RotatingKVCache(model.args().sliding_window));
                } else {
                    caches.push_back(new KVCache());
                }
            }
        }

        std::vector<int> generated;
        std::vector<int> sequence = input_tokens;

        // Prefill
        auto logits = model.streaming_enabled()
                          ? model.run_token_sequence(x, &caches)
                          : model(x, &caches);
        auto next_logits = reeval_resident
                               ? squeeze(slice(logits, {0, logits.shape(1) - 1, 0},
                                              {1, logits.shape(1), logits.shape(2)}), 1)
                           : model.streaming_enabled()
                               ? squeeze(logits, {0, 1})
                               : squeeze(slice(logits, {0, logits.shape(1)-1, 0},
                                              {logits.shape(0), logits.shape(1), logits.shape(2)}),
                                         1);
        auto next_t_arr = argmax(next_logits, -1);
        eval(next_t_arr);
        for (auto c : caches) {
            eval(c->keys, c->values);
        }
        int next_t = next_t_arr.item<int>();
        generated.push_back(next_t);

        for (int step = 0; step < 31; step++) {
            if (reeval_resident) {
                sequence.push_back(next_t);
                array full(sequence.data(), {1, (int)sequence.size()}, int32);
                logits = model(full);
                next_logits = squeeze(slice(logits, {0, logits.shape(1) - 1, 0},
                                             {1, logits.shape(1), logits.shape(2)}), 1);
                next_t_arr = argmax(next_logits, -1);
            } else {
                array t = array({next_t}, {1, 1}, int32);
                logits = model(t, &caches);
                next_t_arr = argmax(logits, -1);
            }
            eval(next_t_arr);
            for (auto* c : caches) {
                eval(c->keys, c->values);
            }
            next_t = next_t_arr.item<int>();
            generated.push_back(next_t);
        }

        int matching = 0;
        int first_mismatch = -1;
        for (size_t j = 0; j < expected.size(); j++) {
            if (expected[j] == generated[j]) {
                matching++;
                total_tokens++;
            } else {
                first_mismatch = j;
                break;
            }
        }

        std::string prompt_trunc = prompt.length() > 20 ? prompt.substr(0, 17) + "..." : prompt;
        std::cout << prompt_trunc << " | " << matching << "/" << expected.size() << " | ";
        if (first_mismatch == -1) {
            std::cout << "None | PASS\n";
            all_passed++;
        } else {
            std::cout << expected[first_mismatch] << " != " << generated[first_mismatch] << " | FAIL\n";
        }

        for (auto c : caches) delete c;
    }

    std::cout << "\n" << all_passed << "/" << prompt_limit << " PASS\n";
    std::cout << total_tokens << "/" << (prompt_limit * 32)
              << " generated tokens identical\n";

    if (all_passed != static_cast<int>(prompt_limit)) return 1;
    return 0;
}
