#include "mlx/mlx.h"
#include "maple_model.h"
#include <iostream>

using namespace mlx::core;
using namespace samosa::maple;

void compare_arrays(const array& a, const array& b, float rtol = 1e-3, float atol = 1e-3) {
    auto a_f32 = astype(a, float32);
    auto b_f32 = astype(b, float32);
    auto diff = abs(subtract(a_f32, b_f32));
    auto tol = add(array(atol), multiply(array(rtol), abs(b_f32)));
    auto cmp = less_equal(diff, tol);
    auto max_diff_arr = max(diff);
    eval({cmp, max_diff_arr});
    auto max_diff = max_diff_arr.item<float>();
    std::cout << "  - Max Abs Error: " << max_diff << "\n";
    std::cout << "  - A shape: " << a.shape() << ", B shape: " << b.shape() << "\n";
    if (!all(cmp).item<bool>()) {
        std::cout << "FAIL: Gate C failed!\n";
        exit(1);
    }
}

int main() {
    std::cout << "[Gate C] Real First-Token Logits Parity Test\n";
    auto model = load_maple_model("models/maple");
    auto fixture = load_safetensors("tests/fixtures/maple/gate_c_fixture.safetensors").first;
    auto x = fixture.at("x");
    std::vector<KVCache*> caches;
    for (int i = 0; i < model.args().num_hidden_layers; i++) {
        caches.push_back(new KVCache());
    }
    
    auto logits = model(x, &caches);
    auto first_token_logits = slice(logits, {0, logits.shape(1) - 1, 0}, {logits.shape(0), logits.shape(1), logits.shape(2)});
    first_token_logits = squeeze(first_token_logits, 1);
    eval(first_token_logits);

    auto expected_logits = fixture.at("expected_logits");
    compare_arrays(first_token_logits, expected_logits);
    std::cout << "PASS: Gate C passed perfectly!\n";
    
    for (auto c : caches) delete c;
    return 0;
}
