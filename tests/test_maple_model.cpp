#include "mlx/mlx.h"
#include "../src/maple/maple_model.h"
#include <iostream>
#include <cassert>
#include <cmath>
#include <vector>

using namespace mlx::core;
using namespace samosa::maple;

void assert_allclose(const array& a, const array& b, float rtol = 1e-3, float atol = 1e-4) {
    if (a.shape() != b.shape()) {
        std::cerr << "Shape mismatch!\n";
        exit(1);
    }
    auto diff = abs(a - b);
    auto max_diff = max(diff).item<float>();
    std::cout << "  - Shape: (";
    for (int i = 0; i < a.shape().size(); ++i) {
        std::cout << a.shape()[i] << (i == a.shape().size() - 1 ? "" : ", ");
    }
    std::cout << ")\n";
    std::cout << "  - Max Abs Error: " << max_diff << "\n";
    std::cout << "  - Tolerance: rtol=" << rtol << ", atol=" << atol << "\n";
    if (max_diff > atol) {
        std::cout << "  - Status: FAIL\n";
        std::cerr << "Assertion failed: outputs not close.\n";
        exit(1);
    }
    std::cout << "  - Status: PASS\n";
}

int main() {
    std::cout << "Running Maple C++ Model Test\n";
    auto data_pair = load_safetensors("tests/fixtures/maple/components/model.safetensors");
    auto data = data_pair.first;
    
    ModelArgs args;
    args.hidden_size = 128;
    args.intermediate_size = 256;
    args.num_attention_heads = 16;
    args.num_key_value_heads = 4;
    args.num_hidden_layers = 2;
    args.layer_types = {"sliding_attention", "attention"};
    args.vocab_size = 512;
    args.rms_norm_eps = 1e-5f;
    args.max_position_embeddings = 128;
    args.rope_theta = 10000.0f;
    args.num_experts = 32;
    args.num_experts_per_tok = 8;
    args.first_k_dense_replace = 0;
    
    MapleModel model(args);
    model.load_weights(data);

    std::vector<KVCache*> caches;
    for (const auto& lt : args.layer_types) {
        if (lt == "sliding_attention") {
            caches.push_back(new RotatingKVCache(64));
        } else {
            caches.push_back(new KVCache());
        }
    }

    auto x_seq = data.at("x_seq");
    auto out_prefill = model(x_seq, &caches);
    eval(out_prefill);
    
    std::cout << "[Prefill Test]\n";
    assert_allclose(out_prefill, data.at("out_prefill"), 1e-2f, 1e-2f);
    
    auto x_dec1 = data.at("x_dec1");
    auto out_dec1 = model(x_dec1, &caches);
    eval(out_dec1);
    
    std::cout << "[Decode 1 Test]\n";
    assert_allclose(out_dec1, data.at("out_dec1"), 1e-2f, 1e-2f);

    auto x_dec2 = data.at("x_dec2");
    auto out_dec2 = model(x_dec2, &caches);
    eval(out_dec2);
    
    std::cout << "[Decode 2 Test]\n";
    assert_allclose(out_dec2, data.at("out_dec2"), 1e-2f, 1e-2f);

    for (auto c : caches) delete c;
    std::cout << "Model tests passed!\n";
    return 0;
}
