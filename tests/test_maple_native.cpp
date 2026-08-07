#include "mlx/mlx.h"
#include "../src/maple/maple_model.h"
#include "../src/maple/tokenizer.h"
#include <iostream>
#include <cassert>

using namespace mlx::core;
using namespace samosa::maple;

void assert_allclose(const array& a, const array& b, float atol=1e-5, float rtol=1e-5) {
    if (a.shape() != b.shape()) {
        std::cerr << "Shape mismatch: " << a.shape() << " != " << b.shape() << std::endl;
        exit(1);
    }
    auto diff = abs(subtract(a, b));
    auto tol = add(array(atol), multiply(array(rtol), abs(b)));
    auto cmp = less_equal(diff, tol);
    eval(cmp);
    if (!all(cmp).item<bool>()) {
        std::cerr << "Value mismatch!" << std::endl;
        exit(1);
    }
}

int main() {
    auto dict = load_safetensors("tests/maple_parity_fixtures.safetensors").first;

    std::cout << "Testing add_rms_norm..." << std::endl;
    {
        auto x = dict.at("add_rms_norm_x");
        auto r = dict.at("add_rms_norm_r");
        auto w = dict.at("add_rms_norm_w");
        auto ref_h_out = dict.at("add_rms_norm_h_out");
        auto ref_hn_out = dict.at("add_rms_norm_hn_out");
        float eps = 1e-6;
        
        auto hn_out = add_rms_norm(x, r, w, eps);
        eval(hn_out);
        assert_allclose(hn_out, ref_hn_out, 1e-2, 1e-2);
        std::cout << "add_rms_norm OK!" << std::endl;
    }

    std::cout << "Testing qk_norm_rope..." << std::endl;
    {
        auto qk = dict.at("qk_norm_rope_qk");
        auto w = dict.at("qk_norm_rope_w");
        auto inv_freq = dict.at("qk_norm_rope_inv_freq");
        auto pos_eps = dict.at("qk_norm_rope_pos_eps");
        auto ref_out = dict.at("qk_norm_rope_out");
        
        eval(pos_eps);
        float offset = pos_eps.data<float>()[0];
        float eps = pos_eps.data<float>()[1];
        int head_dim = 128;
        int rope_dim = 64;
        
        auto out = qk_norm_rope(qk, w, inv_freq, head_dim, rope_dim, offset, eps);
        eval(out);
        assert_allclose(out, ref_out, 1e-2, 1e-2);
        std::cout << "qk_norm_rope OK!" << std::endl;
    }

    std::cout << "Testing fused_router..." << std::endl;
    {
        auto x = dict.at("fused_router_x");
        auto w = dict.at("fused_router_w");
        auto ref_inds = reshape(dict.at("fused_router_inds"), {1, 8});
        auto ref_scores = reshape(dict.at("fused_router_scores"), {1, 8});
        
        int num_experts = w.shape(0);
        auto ctr_in = zeros({8}, uint32);
        
        auto [inds, scores] = fused_router(x, w, ctr_in, num_experts);
        eval(inds, scores);
        
        assert_allclose(inds, ref_inds);
        assert_allclose(scores, ref_scores, 1e-5, 1e-5);
        std::cout << "fused_router OK!" << std::endl;
    }

    std::cout << "Testing swiglu..." << std::endl;
    {
        auto gate = dict.at("swiglu_gate");
        auto x = dict.at("swiglu_x");
        auto ref_out = dict.at("swiglu_out");
        
        auto out = clamped_swiglu(gate, x, 7.0f);
        eval(out);
        
        assert_allclose(out, ref_out, 1e-2, 1e-2);
        std::cout << "swiglu OK!" << std::endl;
    }

    std::cout << "Testing load_maple_model..." << std::endl;
    {
        auto model = load_maple_model("tests/fake_model");
        std::cout << "Model loaded successfully!" << std::endl;
    }

    std::cout << "Testing MapleTokenizer..." << std::endl;
    {
        samosa::maple::MapleTokenizer tokenizer("tests/fake_model");
        std::vector<samosa::maple::Message> msgs = {
            {"user", "Hello!"}
        };
        std::string prompt = tokenizer.apply_chat_template(msgs, true);
        std::cout << "Formatted prompt: " << prompt << std::endl;
        auto tokens = tokenizer.encode(prompt);
        std::cout << "Encoded " << tokens.size() << " tokens." << std::endl;
        std::string decoded = tokenizer.decode(tokens);
        std::cout << "Decoded text: " << decoded << std::endl;
    }

    std::cout << "All Maple tensor ops, model load, and tokenizer passed!" << std::endl;
    return 0;
}
