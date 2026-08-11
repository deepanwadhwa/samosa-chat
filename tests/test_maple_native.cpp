#include "mlx/mlx.h"
#include "../src/maple/maple_model.h"
#include "../src/maple/tokenizer.h"
#include <iostream>
#include <cassert>

using namespace mlx::core;
using namespace samosa::maple;

#include <mach/mach.h>

size_t get_footprint_mb() {
    task_vm_info_data_t vm_info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm_info, &count) == KERN_SUCCESS) {
        return vm_info.phys_footprint / (1024 * 1024);
    }
    return 0;
}

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
    try {
    auto dict = load_safetensors("tests/maple_parity_fixtures.safetensors").first;

    std::cout << "Testing add_rms_norm..." << std::endl;
    {
        auto x = dict.at("add_rms_norm_x");
        auto r = dict.at("add_rms_norm_r");
        auto w = dict.at("add_rms_norm_w");
        auto ref_h_out = dict.at("add_rms_norm_h_out");
        auto ref_hn_out = dict.at("add_rms_norm_hn_out");
        float eps = 1e-6;

        auto pair = add_rms_norm_pair(x, r, w, eps);
        eval(pair.first, pair.second);
        assert_allclose(pair.first, ref_h_out, 1e-2, 1e-2);
        assert_allclose(pair.second, ref_hn_out, 1e-2, 1e-2);
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

    std::cout << "Testing load_maple_model and tiny generation..." << std::endl;
    {
        const char* model_dir = std::getenv("MAPLE_MODEL_DIR");
        if (!model_dir) {
            model_dir = "tests/fake_model";
            /* The production loader correctly fails closed without packed
             * streaming artifacts.  This deliberately tiny synthetic model
             * is the only full-resident exception used by this smoke test. */
            setenv("SAMOSA_MAPLE_ALLOW_FULL_RESIDENT", "1", 0);
        }
        std::cout << "[Mem] Before loading model: " << get_footprint_mb() << " MB" << std::endl;
        auto model = load_maple_model(model_dir);
        std::cout << "[Mem] After loading model: " << get_footprint_mb() << " MB" << std::endl;

        samosa::maple::MapleTokenizer tokenizer(model_dir);
        std::vector<samosa::maple::Message> msgs = {{"user", "Hello."}};
        auto prompt = tokenizer.apply_chat_template(msgs, true);
        auto tokens = tokenizer.encode(prompt);
        if (tokens.size() > 16) tokens.resize(16);

        std::cout << "Encoded " << tokens.size() << " tokens." << std::endl;
        std::vector<KVCache*> caches;
        for (const auto& layer_type : model.args().layer_types) {
            if (layer_type == "sliding_attention") {
                caches.push_back(new RotatingKVCache(model.args().sliding_window));
            } else {
                caches.push_back(new KVCache());
            }
        }

        auto input = array(tokens.data(), {1, (int)tokens.size()});
        std::cout << "Running prefill..." << std::endl;
        auto logits = model.streaming_enabled()
                          ? model.run_token_sequence(input, &caches)
                          : model(input, &caches);
        eval(logits);
        std::cout << "[Mem] Peak during prefill: " << get_footprint_mb() << " MB" << std::endl;
        std::cout << "Running decode..." << std::endl;
        for (int i = 0; i < 2; i++) {
            auto last_logits = model.streaming_enabled()
                                   ? squeeze(logits, {0, 1})
                                   : squeeze(slice(logits, {0, logits.shape(1)-1, 0},
                                                  {logits.shape(0), logits.shape(1), logits.shape(2)}),
                                             {0, 1});
            auto next_tok = argmax(last_logits, -1);
            eval(next_tok);
            auto next_input = array({next_tok.item<int>()}, {1, 1});
            logits = model(next_input, &caches);
            eval(logits);
        }
        std::cout << "[Mem] Peak during decode: " << get_footprint_mb() << " MB" << std::endl;
        for (auto* cache : caches) delete cache;
    }

    std::cout << "All Maple tensor ops, model load, and tokenizer passed!" << std::endl;
    return 0;
    } catch (const std::exception& e) {
        std::cerr << "Maple native test error: " << e.what() << "\n";
        return 1;
    }
}
