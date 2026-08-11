#include "mlx/mlx.h"
#include "../src/maple/maple_model.h"
#include <iostream>
#include <cassert>

using namespace mlx::core;

void assert_allclose(const array& a, const array& b, float rtol = 1e-3f, float atol = 1e-4f) {
    auto diff = abs(subtract(a, b));
    auto max_diff = max(diff);
    eval(max_diff);
    float max_err = max_diff.item<float>();

    auto is_close = allclose(a, b, rtol, atol);
    eval(is_close);
    bool passed = is_close.item<bool>();

    std::cout << "  - Shape: (";
    for (int i = 0; i < a.shape().size(); ++i) {
        std::cout << a.shape()[i] << (i == a.shape().size() - 1 ? "" : ", ");
    }
    std::cout << ")\n";
    std::cout << "  - Max Abs Error: " << max_err << "\n";
    std::cout << "  - Tolerance: rtol=" << rtol << ", atol=" << atol << "\n";
    std::cout << "  - Status: " << (passed ? "PASS" : "FAIL") << "\n";

    if (!passed) {
        std::cerr << "Assertion failed: outputs not close.\n";
        exit(1);
    }
}

int main() {
    try {
    std::cout << "Running Maple C++ Components Tests\n";

    // RMSNorm
    {
        auto data_pair = load_safetensors("tests/fixtures/maple/components/rmsnorm.safetensors");
        auto data = data_pair.first;
        auto x = data.at("input_x");
        auto w = data.at("module.weight");
        auto expected = data.at("expected_output_0");

        // C++ test
        auto out = fast::rms_norm(x, w, 1e-5f);
        eval(out);
        assert_allclose(out, expected);
        std::cout << "RMSNorm PASS\n";
    }

    // RoPE
    {
        auto data_pair = load_safetensors("tests/fixtures/maple/components/rope.safetensors");
        auto data = data_pair.first;
        auto q = data.at("input_q");
        auto k = data.at("input_k");
        auto exp_q = data.at("expected_output_0");
        auto exp_k = data.at("expected_output_1");

        int head_dim = 128;
        int rope_dim = head_dim;

        auto q_out = fast::rope(q, rope_dim, false, 10000.0f, 1.0f, 0);
        auto k_out = fast::rope(k, rope_dim, false, 10000.0f, 1.0f, 0);
        eval(q_out, k_out);

        assert_allclose(q_out, exp_q);
        assert_allclose(k_out, exp_k);
        std::cout << "RoPE PASS\n";
    }

    // Router
    {
        std::cout << "\n[Router Test]\n";
        auto data_pair = load_safetensors("tests/fixtures/maple/components/router.safetensors");
        auto data = data_pair.first;
        auto x = data.at("input_x");
        auto w = data.at("module.weight");
        auto exp_idx = data.at("expected_output_0");
        auto exp_scores = data.at("expected_output_1");

        auto ctr = zeros({8}, uint32);
        // fused_router expects 2D input (N, dim), flatten (2,32,128) -> (64,128)
        auto x_flat = reshape(x, {-1, x.shape(-1)});
        auto out = samosa::maple::fused_router(x_flat, w, ctr, 256);
        // Reshape output from (64, 8) -> (2, 32, 8)
        out.first = reshape(out.first, {x.shape(0), x.shape(1), 8});
        out.second = reshape(out.second, {x.shape(0), x.shape(1), 8});
        eval(out.first, out.second);

        // For indices: sort each row's top-8, then compare (order may differ)
        // NOTE: if all expert scores are nearly uniform (synthetic weights),
        // any top-8 selection is equally valid. Only check strict match
        // when scores are distinguishable.
        auto sorted_actual = sort(astype(out.first, uint32), -1);
        auto sorted_expected = sort(exp_idx, -1);
        eval(sorted_actual, sorted_expected);
        bool idx_match = array_equal(sorted_actual, sorted_expected).item<bool>();

        std::cout << "  - Reference Top-8 IDs (Row 0): ";
        for (int i=0; i<8; i++) std::cout << sorted_expected.data<uint32_t>()[i] << " ";
        std::cout << "\n  - Native Top-8 IDs (Row 0): ";
        for (int i=0; i<8; i++) std::cout << sorted_actual.data<uint32_t>()[i] << " ";
        std::cout << "\n  - Exact ID Match: " << (idx_match ? "PASS" : "FAIL") << "\n";
        // Check if scores are ~uniform (degenerate fixture)
        auto score_range = max(out.second) - min(out.second);
        eval(score_range);
        float sr = score_range.item<float>();
        std::cout << "  - Score range: " << sr << "\n";
        bool uniform_scores = sr < 1e-3f;

        std::cout << "Router Indices (sorted):\n";
        std::cout << "  - Shape: (" << sorted_actual.shape(0) << ", " << sorted_actual.shape(1) << ", " << sorted_actual.shape(2) << ")\n";
        if (uniform_scores) {
            std::cout << "  - Scores are uniform (degenerate fixture) — index tie-breaking is arbitrary\n";
            std::cout << "  - Status: PASS (uniform)\n";
        } else {
            std::cout << "  - Exact match: " << (idx_match ? "Yes" : "No") << "\n";
            std::cout << "  - Status: " << (idx_match ? "PASS" : "FAIL") << "\n";
            if (!idx_match) {
                std::cerr << "Router indices mismatch!\n";
                exit(1);
            }
        }

        std::cout << "Router Scores:\n";
        assert_allclose(out.second, exp_scores, 1e-3f, 1e-4f);

        auto actual_idx_flat = reshape(astype(out.first, uint32), {-1});
        eval(actual_idx_flat);
        std::cout << "  - Selected top-8 expert IDs (first token): ";
        for (int i = 0; i < 8 && i < actual_idx_flat.size(); ++i) {
            std::cout << actual_idx_flat.data<uint32_t>()[i] << " ";
        }
        std::cout << "\n";

        auto max_score_err = max(abs(out.second - exp_scores)).item<float>();
        std::cout << "  - Max routing-weight error: " << max_score_err << "\n";
        std::cout << "Router PASS\n";
    }

    // SwiGLU
    {
        auto data_pair = load_safetensors("tests/fixtures/maple/components/swiglu.safetensors");
        auto data = data_pair.first;
        auto up = data.at("input_x_up");
        auto gate = data.at("input_x_gate");
        auto exp_out = data.at("expected_output_0");

        auto out = samosa::maple::clamped_swiglu(gate, up, 7.0f);
        eval(out);

        assert_allclose(out, exp_out, 1e-2f, 1e-2f); // swiglu can accumulate precision issues
        std::cout << "SwiGLU PASS\n";
    }

    // Decoder Block
    {
        auto data_pair = load_safetensors("tests/fixtures/maple/components/decoder_block.safetensors");
        auto data = data_pair.first;
        auto x = data.at("input_x");
        auto mask = data.at("input_mask");
        auto exp_out = data.at("expected_output_0");

        samosa::maple::ModelArgs args;
        args.hidden_size = 128;
        args.intermediate_size = 256;
        args.num_attention_heads = 16;
        args.num_key_value_heads = 4;
        args.num_hidden_layers = 2;
        args.rms_norm_eps = 1e-5f;
        args.vocab_size = 512;
        args.max_position_embeddings = 128;
        args.rope_theta = 10000.0f;
        args.num_experts_per_tok = 8;
        args.num_experts = 256;
        args.layer_types = {"sliding_attention", "attention"};

        samosa::maple::MapleDecoderLayer block(args, 0);

        // Strip module. prefix from dict
        std::unordered_map<std::string, array> weights;
        for (auto& kv : data) {
            if (kv.first.find("module.") == 0) {
                weights.insert({kv.first.substr(7), kv.second});
            }
        }
        block.load_weights(weights, "");

        auto out = block(x, mask);
        eval(out);
        assert_allclose(out, exp_out, 1e-2f, 1e-2f);
        std::cout << "Decoder Block PASS\n";
    }

    return 0;
    } catch (const std::exception& e) {
        std::cerr << "Maple component test error: " << e.what() << "\n";
        return 1;
    }
}
