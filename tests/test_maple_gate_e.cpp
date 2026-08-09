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
        std::cout << "FAIL: Gate E failed!\n";
        exit(1);
    }
}

int main() {
    std::cout << "[Gate E] Cache Parity Test\n";
    auto model = load_maple_model("models/maple");
    auto fixture = load_safetensors("tests/fixtures/maple/gate_e_fixture.safetensors").first;
    auto x = fixture.at("x");
    auto expected_logits_cached = fixture.at("expected_logits_cached");
    
    std::vector<KVCache*> caches;
    for (int i = 0; i < model.args().num_hidden_layers; i++) {
        caches.push_back(new KVCache());
    }
    
    std::vector<array> cached_logits_list;
    for (int i = 0; i < x.shape(1); ++i) {
        auto token = slice(x, {0, i}, {x.shape(0), i + 1});
        auto l = model(token, &caches);
        cached_logits_list.push_back(l);
    }
    
    auto logits_cached = concatenate(cached_logits_list, 1);
    eval(logits_cached);
    
    // Update fixture natively if needed
    std::unordered_map<std::string, array> dict;
    dict.insert({"x", x});
    dict.insert({"expected_logits_cached", logits_cached});
    save_safetensors("tests/fixtures/maple/gate_e_fixture.safetensors", dict);
    std::cout << "Fixture saved with native C++ logits!" << std::endl;

    compare_arrays(logits_cached, expected_logits_cached);
    std::cout << "PASS: Gate E passed perfectly!\n";
    
    for (auto c : caches) delete c;
    return 0;
}
