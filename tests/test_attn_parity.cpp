#include <iostream>
#include "mlx/mlx.h"
#include "maple/maple_model.h"
#include <cmath>
#include <cassert>

using namespace mlx::core;
using namespace samosa::maple;

int main() {
    ModelArgs args;
    args.hidden_size = 2048;
    args.num_hidden_layers = 1;
    args.vocab_size = 151936;
    args.num_attention_heads = 16;
    args.num_key_value_heads = 4;
    args.rms_norm_eps = 1e-5;
    args.rope_theta = 10000.0f;
    args.head_dim = 128;
    args.first_k_dense_replace = 1;
    args.layer_types = {"sliding_attention"};

    MapleAttention attn(args, 0);

    auto npz = load_safetensors("tests/attn_parity.safetensors").first;

    // Load weights into C++ attention (exclude test inputs/outputs)
    std::unordered_map<std::string, array> weights;
    for (const auto& kv : npz) {
        if (kv.first != "x" && kv.first != "out" && kv.first != "q_out") {
            weights.insert({kv.first, kv.second});
        }
    }
    attn.load_weights(weights, "model.layers.0");

    auto x = npz.at("x");
    auto expected_out = npz.at("out");

    // This legacy fixture calls Python attention directly with mask=None,
    // which is non-causal.  The C++ attention API reserves nullopt for the
    // production model's optimized causal mode, so pass an explicit all-zero
    // additive mask to exercise the same non-causal math here.
    std::optional<array> direct_mask =
        zeros({x.shape(1), x.shape(1)}, x.dtype());
    auto out = attn(x, direct_mask, nullptr);
    eval(out);

    auto diff = abs(out - expected_out);
    auto max_err = max(diff).item<float>();
    float tolerance = 1e-3;

    std::cout << "=== MapleAttention Parity Test ===" << std::endl;
    std::cout << "Tensor shape:       [" << out.shape(0) << ", " << out.shape(1) << ", " << out.shape(2) << "]" << std::endl;
    std::cout << "Expected shape:     [" << expected_out.shape(0) << ", " << expected_out.shape(1) << ", " << expected_out.shape(2) << "]" << std::endl;
    std::cout << "Max absolute error: " << max_err << std::endl;
    std::cout << "Tolerance:          " << tolerance << std::endl;

    // Also report q_proj parity if available
    if (npz.count("q_out")) {
        auto q_out_cpp = attn.q_proj_(x);
        eval(q_out_cpp);
        auto expected_q = npz.at("q_out");
        auto q_err = max(abs(q_out_cpp - expected_q)).item<float>();
        std::cout << "q_proj max error:   " << q_err << std::endl;
    }

    // The production SSD path uses the checkpoint's fused QKV + fused QK
    // norm/RoPE graph for single-token decode.  Exercise it independently of
    // the full resident model and compare it to the portable one-token path.
    MapleAttention fused_attn(args, 0);
    fused_attn.load_weights(weights, "model.layers.0");
    fused_attn.enable_streaming_decode();
    auto x_one = astype(
        slice(x, {0, 0, 0}, {1, 1, x.shape(2)}), bfloat16);
    std::optional<array> decode_mask = std::nullopt;
    auto portable_one = attn(x_one, decode_mask, nullptr);
    auto fused_one = fused_attn(x_one, decode_mask, nullptr);
    eval(portable_one, fused_one);
    auto fused_err = max(abs(fused_one - portable_one)).item<float>();
    std::cout << "fused decode error:  " << fused_err << std::endl;
    if (fused_err >= tolerance) {
        std::cout << "STATUS: FAIL" << std::endl;
        return 1;
    }

    if (max_err < tolerance) {
        std::cout << "STATUS: PASS" << std::endl;
        return 0;
    } else {
        std::cout << "STATUS: FAIL" << std::endl;
        return 1;
    }
}
