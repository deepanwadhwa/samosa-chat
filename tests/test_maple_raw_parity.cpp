#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>
#include "../src/maple/maple_model.h"
#include "../src/maple/tokenizer.h"
#include <mlx/mlx.h>

using namespace mlx::core;
using json = nlohmann::json;
using namespace samosa::maple;

int main() {
    std::cout << "=== 2. RAW FIRST-LOGIT PARITY ===\n";

    // Load fixtures
    std::ifstream f("tests/fixtures/maple/raw_parity_refs_3.json");
    if (!f.is_open()) {
        std::cerr << "Failed to open tests/fixtures/maple/raw_parity_refs_3.json\n";
        return 1;
    }
    json refs;
    f >> refs;

    // Initialize native model
    std::string model_dir = "models/maple";
    auto model = MapleModel(model_dir);
    MapleTokenizer tokenizer(model_dir);

    for (const auto& ref : refs) {
        std::string prompt = ref["prompt"];
        std::vector<int> expected_shape = ref["logits_shape"];
        int expected_argmax = ref["argmax_id"];
        std::vector<int> expected_top20 = ref["top20_ids"];
        std::vector<float> expected_logits_data = ref["logits"];

        std::vector<Message> messages = {{"user", prompt}};
        auto formatted = tokenizer.apply_chat_template(messages, true);
        auto input_tokens = tokenizer.encode(formatted);

        array x = array(input_tokens.data(), {1, (int)input_tokens.size()}, int32);

        // Evaluate
        auto logits = model(x);
        eval(logits);

        auto last_logits = squeeze(slice(logits, {0, logits.shape(1)-1, 0}, {logits.shape(0), logits.shape(1), logits.shape(2)}), {0, 1});
        eval(last_logits);

        int native_argmax = argmax(last_logits, -1).item<int>();

        auto top20_idx = argpartition(negative(last_logits), 20);
        auto top20_idx_slice = slice(top20_idx, {0}, {20});

        auto top20_vals = take(negative(last_logits), top20_idx_slice);
        auto sort_idx = argsort(top20_vals);
        auto sorted_top20 = take(top20_idx_slice, sort_idx);
        eval(sorted_top20);

        std::vector<int> native_top20;
        for (int i=0; i<20; i++) {
            native_top20.push_back(sorted_top20.data<uint32_t>()[i]);
        }

        array exp_logits = array(expected_logits_data.data(), last_logits.shape(), float32);
        auto diff = abs(subtract(last_logits, exp_logits));
        float max_err = max(diff).item<float>();

        std::cout << "Prompt: " << prompt << "\n";
        std::cout << "Tokens: ";
        for (auto t : input_tokens) std::cout << t << " ";
        std::cout << "\n";

        std::cout << "Logits Tensor Shape: (" << logits.shape(0) << ", " << logits.shape(1) << ", " << logits.shape(2) << ")\n";
        std::cout << "Reference argmax ID: " << expected_argmax << "\n";
        std::cout << "Native argmax ID: " << native_argmax << "\n";

        std::cout << "Reference top-20: ";
        for (int t : expected_top20) std::cout << t << " ";
        std::cout << "\nNative top-20: ";
        for (int t : native_top20) std::cout << t << " ";
        std::cout << "\nMax absolute logit error: " << max_err << "\n\n";
    }

    return 0;
}
