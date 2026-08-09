#include "mlx/mlx.h"
#include "maple_model.h"
#include <iostream>

using namespace mlx::core;
using namespace samosa::maple;

int main() {
    std::cout << "[Gate D] Greedy Generation Parity Test\n";
    auto model = load_maple_model("models/maple");
    
    auto fixture = load_safetensors("tests/fixtures/maple/gate_d_fixture.safetensors").first;
    auto x = fixture.at("x");
    auto expected_tokens = fixture.at("expected_tokens");
    
    std::vector<KVCache*> caches;
    for (int i = 0; i < model.args().num_hidden_layers; i++) {
        caches.push_back(new KVCache());
    }
    
    std::vector<int> generated_tokens;
    std::cout << "Starting generation..." << std::endl;
    for (int step = 0; step < 5; ++step) {
        auto logits = model(x, &caches);
        auto last_token_logits = slice(logits, {0, logits.shape(1) - 1, 0}, {logits.shape(0), logits.shape(1), logits.shape(2)});
        last_token_logits = squeeze(last_token_logits, 1);
        
        auto token_arr = argmax(last_token_logits, -1);
        eval(token_arr);
        int token = token_arr.item<int>();
        generated_tokens.push_back(token);
        
        std::cout << "Step " << step << ": token " << token << std::endl;
        x = reshape(array(token), {1, 1});
    }
    
    auto gen_arr = array(generated_tokens.data(), { (int)generated_tokens.size() }, int32);
    eval(gen_arr);
    
    auto expected_f32 = astype(expected_tokens, float32);
    auto gen_f32 = astype(gen_arr, float32);
    auto diff = abs(subtract(expected_f32, gen_f32));
    auto cmp = equal(expected_f32, gen_f32);
    
    eval({cmp, diff});
    auto max_diff = max(diff).item<float>();
    
    std::cout << "  - Max Diff in Tokens: " << max_diff << "\n";
    if (!all(cmp).item<bool>()) {
        std::cout << "FAIL: Gate D failed!\n";
        exit(1);
    }
    std::cout << "PASS: Gate D passed perfectly!\n";
    
    for (auto c : caches) delete c;
    return 0;
}
