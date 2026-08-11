#include "maple_model.h"
#include "maple_expert_views.h"
#include "mlx/compile.h"
#include "../json.h"
#include "mlx/fast.h"
#include <sstream>
#include <iomanip>
#include <iostream>
#include <unordered_set>
#include <filesystem>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace samosa {
namespace maple {

using namespace mlx::core;

array maple_rms_norm(const array& x, const array& weight, float eps) {
    auto x_f32 = astype(x, float32);
    auto weight_f32 = astype(weight, float32);
    auto norm = fast::rms_norm(x_f32, weight_f32, eps);
    return astype(norm, x.dtype());
}

std::string format_eps(float eps) {
    std::ostringstream out;
    out << std::scientific << std::setprecision(10) << eps << "f";
    return out.str();
}

std::string tag_eps(float eps) {
    std::ostringstream out;
    out << std::scientific << std::setprecision(3) << eps;
    std::string tag = out.str();
    for (char &c : tag) {
        if (c == '.') c = '_';
        if (c == '-') c = 'm';
        if (c == '+') c = 'p';
    }
    return tag;
}



std::pair<array, array> add_rms_norm_pair(const array& h, const array& r,
                                         const array& w, float eps) {
    std::string source = R"(
        uint tid = thread_position_in_threadgroup.x;
        constexpr uint N = DIM;
        constexpr uint PT = N / 256u;
        float hb[PT];
        float ss = 0.0f;
        for (uint i = 0; i < PT; ++i) {
            uint j = tid * PT + i;
            float v = (float)x[j] + (float)r_in[j];
            T_ vb = (T_)v;
            h_out[j] = vb;
            hb[i] = (float)vb;
            ss += hb[i] * hb[i];
        }
        ss = simd_sum(ss);
        threadgroup float sums[8];
        uint sg = tid / 32u;
        uint lane = tid % 32u;
        if (lane == 0u) sums[sg] = ss;
        threadgroup_barrier(mem_flags::mem_threadgroup);
        float tot = 0.0f;
        for (uint i = 0; i < 8u; ++i) tot += sums[i];
        float scale = metal::rsqrt(tot / (float)N + EPS_);
        for (uint i = 0; i < PT; ++i) {
            uint j = tid * PT + i;
            hn_out[j] = (T_)(hb[i] * scale * (float)w[j]);
        }
    )";

    std::string eps_str = format_eps(eps);
    size_t pos;
    while ((pos = source.find("EPS_")) != std::string::npos) {
        source.replace(pos, 4, eps_str);
    }

    auto kernel = fast::metal_kernel(
        "maple_add_rms_norm_" + tag_eps(eps),
        {"x", "r_in", "w"},
        {"h_out", "hn_out"},
        source
    );

    auto h_flat = reshape(h, {-1});
    auto r_flat = reshape(r, {-1});

    auto out = kernel(
        {h_flat, r_flat, w},
        {h.shape(), h.shape()},
        {h.dtype(), h.dtype()},
        std::make_tuple(256, 1, 1),
        std::make_tuple(256, 1, 1),
        {{"T_", h.dtype()}, {"DIM", h.shape().back()}},
        std::nullopt,
        false,
        default_stream(default_device())
    );

    return {out[0], out[1]};
}

array add_rms_norm(const array& h, const array& r, const array& w, float eps) {
    return add_rms_norm_pair(h, r, w, eps).second;
}

array qk_norm_rope(const array& qk, const array& w, const array& inv_freq, int head_dim, int rope_dim, float offset, float eps) {
    std::string source = R"(
        uint head = thread_position_in_grid.y;
        uint lane = thread_position_in_grid.x;

        constexpr int per_lane = HEAD_DIM / 32;
        const device T_* xh = x + head * HEAD_DIM;
        const device T_* wh = w + head * HEAD_DIM;
        device T_* oh = out + head * HEAD_DIM;

        float ss = 0.0f;
        for (int i = 0; i < per_lane; ++i) {
            float v = (float)xh[lane * per_lane + i];
            ss += v * v;
        }
        ss = simd_sum(ss);
        float pos = pos_eps[0];
        float eps = pos_eps[1];
        float scale = metal::rsqrt(ss / HEAD_DIM + eps);

        for (int i = 0; i < per_lane; ++i) {
            int j = lane * per_lane + i;
            float v = (float)xh[j] * scale * (float)wh[j];
            if (ROPE_DIM > 0 && j < ROPE_DIM) {
                constexpr int rhalf = ROPE_DIM > 0 ? ROPE_DIM / 2 : 1;
                int p = j < rhalf ? j : j - rhalf;
                float theta = pos * inv_freq[p];
                float c = metal::cos(theta);
                float s = metal::sin(theta);
                int j2 = j < rhalf ? j + rhalf : j - rhalf;
                float u = (float)xh[j2] * scale * (float)wh[j2];
                v = j < rhalf ? (v * c - u * s) : (v * c + u * s);
            }
            oh[j] = (T_)v;
        }
    )";

    auto kernel = fast::metal_kernel(
        "maple_qk_norm_rope",
        {"x", "w", "inv_freq", "pos_eps"},
        {"out"},
        source
    );

    auto pos_eps = array({offset, eps}, float32);

    auto out = kernel(
        {qk, w, inv_freq, pos_eps},
        {qk.shape()},
        {qk.dtype()},
        std::make_tuple(32, qk.shape()[0], 1),
        std::make_tuple(32, 1, 1),
        {{"T_", qk.dtype()}, {"HEAD_DIM", head_dim}, {"ROPE_DIM", rope_dim}},
        std::nullopt,
        false,
        default_stream(default_device())
    );

    return out[0];
}

std::pair<array, array> fused_router(const array& x, const array& w, array& ctr_in, int num_experts) {
    std::string source = R"(
    constexpr uint NE = NEXP;
    constexpr uint D = DIM;
    constexpr uint NTG = NE / 32u;
    constexpr uint TM = 4u;
    constexpr uint TN = 4u;
    constexpr uint BLOCKN = 32u * TN;
    constexpr uint NITER = D / BLOCKN;

    uint tid = thread_position_in_threadgroup.x;
    uint tgid = threadgroup_position_in_grid.x;
    uint n_threads = 256u;
    uint sg_id = tid / 32u;
    uint lane = tid % 32u;
    uint n_sg = n_threads / 32u;

    uint row0 = tgid * (n_sg * TM) + sg_id * TM;
    float result[TM] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint bn = lane * TN;
    for (uint i = 0u; i < NITER; ++i) {
        float v[TN];
        for (uint tn = 0u; tn < TN; ++tn) v[tn] = float(x[bn + tn]);
        for (uint tm = 0u; tm < TM; ++tm) {
            const device T_* wrow = w + (ulong)(row0 + tm) * D;
            T_ inter[TN];
            for (uint tn = 0u; tn < TN; ++tn) inter[tn] = wrow[bn + tn];
            for (uint tn = 0u; tn < TN; ++tn) result[tm] += inter[tn] * v[tn];
        }
        bn += BLOCKN;
    }
    for (uint tm = 0u; tm < TM; ++tm) {
        for (ushort sn = 16; sn >= 1; sn >>= 1) {
            result[tm] += simd_shuffle_down(result[tm], sn);
        }
    }
    device atomic_float* ls = (device atomic_float*)logits_scratch;
    if (lane == 0u) {
        for (uint tm = 0u; tm < TM; ++tm) {
            atomic_store_explicit(&ls[row0 + tm], result[tm],
                                  memory_order_relaxed);
        }
    }

    threadgroup_barrier(mem_flags::mem_device);
    threadgroup uint last_flag;
    if (tid == 0u) {
        device atomic_uint* ctr = (device atomic_uint*)ctr_in;
        uint prev = atomic_fetch_add_explicit(ctr, 1u, memory_order_relaxed);
        uint last = (prev == NTG - 1u) ? 1u : 0u;
        if (last == 1u) atomic_store_explicit(ctr, 0u, memory_order_relaxed);
        last_flag = last;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (last_flag == 0u) return;
    threadgroup_barrier(mem_flags::mem_device);

    float my_max = -1e30f;
    for (uint e = tid; e < NE; e += n_threads) {
        float v = atomic_load_explicit(&ls[e], memory_order_relaxed);
        if (v > my_max) my_max = v;
    }
    for (int off = 16; off > 0; off >>= 1) {
        float other = simd_shuffle_down(my_max, off);
        if (other > my_max) my_max = other;
    }
    threadgroup float sg_red[16];
    if (lane == 0u) sg_red[sg_id] = my_max;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (tid == 0u) {
        float m = sg_red[0];
        for (uint s = 1u; s < n_sg; s++) if (sg_red[s] > m) m = sg_red[s];
        sg_red[0] = m;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float lmax = sg_red[0];

    threadgroup float scores[NE];
    float my_sum = 0.0f;
    for (uint e = tid; e < NE; e += n_threads) {
        float lv = atomic_load_explicit(&ls[e], memory_order_relaxed);
        float v = metal::exp(lv - lmax);
        scores[e] = v;
        my_sum += v;
    }
    for (int off = 16; off > 0; off >>= 1) {
        my_sum += simd_shuffle_down(my_sum, off);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lane == 0u) sg_red[sg_id] = my_sum;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (tid == 0u) {
        float ssum = sg_red[0];
        for (uint i = 1u; i < n_sg; i++) ssum += sg_red[i];
        sg_red[0] = ssum;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float inv_total = 1.0f / (sg_red[0] + 1e-20f);
    for (uint e = tid; e < NE; e += n_threads) {
        scores[e] = scores[e] * inv_total;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    threadgroup int topk_idx[8];
    threadgroup float topk_val[8];
    threadgroup uint8_t used[NE];
    for (uint e = tid; e < NE; e += n_threads) used[e] = 0;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    for (int k = 0; k < 8; k++) {
        float my_best = -1e30f;
        int my_idx = 0;
        for (int e = int(tid); e < int(NE); e += int(n_threads)) {
            if (!used[e] && scores[e] > my_best) {
                my_best = scores[e];
                my_idx = e;
            }
        }
        for (int off = 16; off > 0; off >>= 1) {
            float other_v = simd_shuffle_down(my_best, off);
            int other_i = simd_shuffle_down(my_idx, off);
            if (other_v > my_best) { my_best = other_v; my_idx = other_i; }
        }
        threadgroup float sg_vals[16];
        threadgroup int sg_idxs[16];
        if (lane == 0u) { sg_vals[sg_id] = my_best; sg_idxs[sg_id] = my_idx; }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (tid == 0u) {
            float bv = sg_vals[0]; int bi = sg_idxs[0];
            for (uint s = 1u; s < n_sg; s++) {
                if (sg_vals[s] > bv) { bv = sg_vals[s]; bi = sg_idxs[s]; }
            }
            topk_val[k] = bv; topk_idx[k] = bi;
            used[bi] = 1;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    if (tid < 8u) {
        float sel_sum = 0.0f;
        for (int i = 0; i < 8; i++) sel_sum += topk_val[i];
        out_indices[tid] = topk_idx[tid];
        out_scores[tid] = float(topk_val[tid] / (sel_sum + 1e-20f));
    }
    )";

    auto kernel = fast::metal_kernel(
        "maple_fused_router",
        {"x", "w", "ctr_in"},
        {"out_indices", "out_scores", "logits_scratch"},
        source
    );

    int seq_len = x.shape(0);
    int dim = x.shape(1);
    int ntg = num_experts / 32;

    // The Metal kernel processes ONE token at a time. Loop over tokens.
    std::vector<array> all_indices, all_scores;
    for (int t = 0; t < seq_len; ++t) {
        auto x_t = reshape(slice(x, {t, 0}, {t + 1, dim}), {dim});
        auto out_t = kernel(
            {x_t, w, ctr_in},
            {{1, 8}, {1, 8}, {num_experts}},
            {int32, float32, float32},
            std::make_tuple(ntg * 256, 1, 1),
            std::make_tuple(256, 1, 1),
            {{"T_", w.dtype()}, {"NEXP", num_experts}, {"DIM", dim}},
            std::nullopt,
            false,
            default_stream(default_device())
        );
        all_indices.push_back(reshape(out_t[0], {1, 8}));
        all_scores.push_back(reshape(out_t[1], {1, 8}));
    }

    auto indices = concatenate(all_indices, 0);
    auto scores = concatenate(all_scores, 0);

    return {indices, scores};
}


array safe_at2(const std::unordered_map<std::string, array>& weights, const std::string& key) {
    auto it = weights.find(key);
    if (it == weights.end()) {
        throw std::runtime_error("Key not found: " + key);
    }
    return it->second;
}

array clamped_swiglu(const array& gate, const array& x, float mlp_clamp) {
    if (mlp_clamp != 7.0f) {
        throw std::runtime_error(
            "compiled Maple SwiGLU requires the checkpoint clamp of 7");
    }
    static auto compiled = mlx::core::compile(
        [](const std::vector<array>& inputs) {
            const auto& gate_in = inputs[0];
            const auto& up_in = inputs[1];
            auto clipped_gate = minimum(
                gate_in, array(7.0f, gate_in.dtype()));
            auto silu = multiply(clipped_gate, sigmoid(clipped_gate));
            auto clipped_up = clip(
                up_in, array(-7.0f, up_in.dtype()),
                array(7.0f, up_in.dtype()));
            return std::vector<array>{multiply(silu, clipped_up)};
        },
        true);
    return compiled({gate, x})[0];
}

MapleAttention::MapleAttention(const ModelArgs& args, int layer_idx)
    : args_(args), use_rope_(false),
      q_norm_weight_(array(0.0f)), k_norm_weight_(array(0.0f)) {
    if (layer_idx >= 0 && static_cast<size_t>(layer_idx) < args.layer_types.size() &&
        args.layer_types[layer_idx] == "sliding_attention") {
        use_rope_ = true;
    }
}

void MapleAttention::enable_streaming_decode() {
    streaming_decode_ = true;
}

void MapleAttention::ensure_streaming_decode_weights() {
    if (decode_aux_ready_) return;
    if (!streaming_decode_) {
        throw std::runtime_error("Maple fused QKV requested outside streaming decode");
    }
    if (!qkv_fused_ready_) {
        if (q_proj_.is_quantized != k_proj_.is_quantized ||
            q_proj_.is_quantized != v_proj_.is_quantized ||
            q_proj_.group_size != k_proj_.group_size ||
            q_proj_.group_size != v_proj_.group_size ||
            q_proj_.bits != k_proj_.bits || q_proj_.bits != v_proj_.bits) {
            throw std::runtime_error("Maple QKV projections have incompatible quantization");
        }

        /* The Python checkpoint implementation concatenates these tensors
         * before inference and executes one quantized matmul.  Materialize
         * one layer at a time on first use, then release the split tensors. */
        qkv_proj_.weight = contiguous(concatenate(
            {q_proj_.weight, k_proj_.weight, v_proj_.weight}, 0));
        qkv_proj_.is_quantized = q_proj_.is_quantized;
        qkv_proj_.group_size = q_proj_.group_size;
        qkv_proj_.bits = q_proj_.bits;
        if (qkv_proj_.is_quantized) {
            qkv_proj_.scales = contiguous(concatenate(
                {q_proj_.scales, k_proj_.scales, v_proj_.scales}, 0));
            qkv_proj_.biases = contiguous(concatenate(
                {q_proj_.biases, k_proj_.biases, v_proj_.biases}, 0));
            eval(qkv_proj_.weight, qkv_proj_.scales, qkv_proj_.biases);
        } else {
            eval(qkv_proj_.weight);
        }
        q_proj_ = QLinear();
        k_proj_ = QLinear();
        v_proj_ = QLinear();
        qkv_fused_ready_ = true;
    }

    const int n_q = args_.num_attention_heads;
    const int n_kv = args_.num_key_value_heads;
    auto q_weight = broadcast_to(expand_dims(q_norm_weight_, 0),
                                 {n_q, args_.head_dim});
    auto k_weight = broadcast_to(expand_dims(k_norm_weight_, 0),
                                 {n_kv, args_.head_dim});
    qk_norm_weight_ = contiguous(concatenate({q_weight, k_weight}, 0));
    if (use_rope_) {
        const int rope_dim = static_cast<int>(
            args_.head_dim * args_.partial_rotary_factor);
        const int half = rope_dim / 2;
        inv_freq_ = power(
            array(args_.rope_theta, float32),
            divide(negative(arange(half, float32)), array((float)half, float32)));
    } else {
        inv_freq_ = ones({1}, float32);
    }
    eval(qk_norm_weight_, inv_freq_);

    decode_aux_ready_ = true;
}

void MapleAttention::load_weights(const std::unordered_map<std::string, array>& weights, const std::string& prefix) {
    std::string p = prefix.empty() ? "" : prefix + ".";
    if (weights.count(p + "self_attn.qkv_proj.weight")) {
        qkv_proj_.load(weights, p + "self_attn.qkv_proj");
        qkv_fused_ready_ = true;
    } else {
        q_proj_.load(weights, p + "self_attn.q_proj");
        k_proj_.load(weights, p + "self_attn.k_proj");
        v_proj_.load(weights, p + "self_attn.v_proj");
    }
    o_proj_.load(weights, p + "self_attn.o_proj");
    q_norm_weight_ = safe_at2(weights, p + "self_attn.q_norm.weight");
    k_norm_weight_ = safe_at2(weights, p + "self_attn.k_norm.weight");
}

array MapleAttention::operator()(const array& x, const std::optional<array>& mask, KVCache* cache) {
    auto B = x.shape(0);
    auto L = x.shape(1);

    array queries(0.0f), keys(0.0f), values(0.0f);
    if (streaming_decode_) {
        /* Batch prefill uses the fused QKV projection but the portable QK
         * norm/RoPE path, exactly like the checkpoint implementation. */
        ensure_streaming_decode_weights();
    }
    if (streaming_decode_ && B == 1 && L == 1) {
        auto qkv = qkv_proj_(x);
        const int n_q = args_.num_attention_heads;
        const int n_kv = args_.num_key_value_heads;
        const int qk_rows = n_q + n_kv;
        const int qk_elements = qk_rows * args_.head_dim;
        auto qk = reshape(slice(reshape(qkv, {-1}), {0}, {qk_elements}),
                          {qk_rows, args_.head_dim});
        const int rope_dim = use_rope_
            ? static_cast<int>(args_.head_dim * args_.partial_rotary_factor)
            : 0;
        const int offset = cache ? cache->offset : 0;
        auto qk_out = qk_norm_rope(
            qk, qk_norm_weight_, inv_freq_, args_.head_dim, rope_dim,
            static_cast<float>(offset), args_.rms_norm_eps);
        queries = reshape(slice(qk_out, {0, 0}, {n_q, args_.head_dim}),
                          {1, n_q, 1, args_.head_dim});
        keys = reshape(slice(qk_out, {n_q, 0}, {qk_rows, args_.head_dim}),
                       {1, n_kv, 1, args_.head_dim});
        values = reshape(
            slice(reshape(qkv, {-1}), {qk_elements},
                  {static_cast<int>(qkv.size())}),
            {1, n_kv, 1, args_.head_dim});
    } else {
        array q(0.0f), k(0.0f), v(0.0f);
        if (qkv_fused_ready_) {
            auto qkv = qkv_proj_(x);
            const int q_size = args_.num_attention_heads * args_.head_dim;
            const int kv_size = args_.num_key_value_heads * args_.head_dim;
            q = slice(qkv, {0, 0, 0}, {B, L, q_size});
            k = slice(qkv, {0, 0, q_size}, {B, L, q_size + kv_size});
            v = slice(qkv, {0, 0, q_size + kv_size},
                      {B, L, q_size + 2 * kv_size});
        } else {
            q = q_proj_(x);
            k = k_proj_(x);
            v = v_proj_(x);
        }

        queries = reshape(q, {B, L, args_.num_attention_heads, args_.head_dim});
        keys = reshape(k, {B, L, args_.num_key_value_heads, args_.head_dim});
        values = reshape(v, {B, L, args_.num_key_value_heads, args_.head_dim});

        queries = maple_rms_norm(queries, q_norm_weight_, args_.rms_norm_eps);
        keys = maple_rms_norm(keys, k_norm_weight_, args_.rms_norm_eps);

        queries = transpose(queries, {0, 2, 1, 3});
        keys = transpose(keys, {0, 2, 1, 3});
        values = transpose(values, {0, 2, 1, 3});

        if (use_rope_) {
            int rope_dim = static_cast<int>(args_.head_dim * args_.partial_rotary_factor);
            int offset = cache ? cache->offset : 0;
            queries = fast::rope(queries, rope_dim, /*traditional=*/false, args_.rope_theta, /*scale=*/1.0f, offset);
            keys = fast::rope(keys, rope_dim, /*traditional=*/false, args_.rope_theta, /*scale=*/1.0f, offset);
        }
    }

    if (cache) {
        cache->update_and_fetch(keys, values);
    }

    float scale = 1.0f / std::sqrt((float)args_.head_dim);
    const std::string mask_mode = (!mask && L > 1) ? "causal" : "";
    auto output = fast::scaled_dot_product_attention(
        queries, keys, values, scale, mask_mode, mask);

    output = reshape(transpose(output, {0, 2, 1, 3}), {B, L, -1});
    return o_proj_(output);
}

MapleMLP::MapleMLP(const ModelArgs& /*args*/) {}

void MapleMLP::load_weights(const std::unordered_map<std::string, array>& weights, const std::string& prefix) {
    std::string p = prefix.empty() ? "" : prefix + ".";
    gate_proj_.load(weights, p + "mlp.gate_proj");
    up_proj_.load(weights, p + "mlp.up_proj");
    down_proj_.load(weights, p + "mlp.down_proj");
}

array MapleMLP::operator()(const array& x) {
    auto gate = gate_proj_(x);
    auto up = up_proj_(x);
    // The clamp is part of Maple's sparse expert path only.  Dense layers
    // use the ordinary SwiGLU activation from the reference model.
    auto act = multiply(multiply(gate, sigmoid(gate)), up);
    return down_proj_(act);
}

MapleSparseMoeBlock::MapleSparseMoeBlock(const ModelArgs& args, int layer_idx)
    : num_experts_(args.num_experts),
      num_experts_per_tok_(args.num_experts_per_tok),
      hidden_size_(args.hidden_size),
      moe_intermediate_size_(args.moe_intermediate_size),
      layer_idx_(layer_idx),
      gate_weight_(array(0.0f)), router_ctr_(array(0.0f)) {
    router_ctr_ = zeros({8}, uint32);
}

void MapleSparseMoeBlock::load_weights(const std::unordered_map<std::string, array>& weights, const std::string& prefix) {
    std::string p = prefix.empty() ? "" : prefix + ".";
    gate_weight_ = safe_at2(weights, p + "mlp.gate.weight");
    expert_store_ = nullptr;
    if (weights.count(p + "mlp.switch_mlp.up_gate_proj.weight")) {
        switch_up_gate_proj_.load(weights, p + "mlp.switch_mlp.up_gate_proj");
        switch_up_gate_fused_ = true;
    } else {
        switch_up_proj_.load(weights, p + "mlp.switch_mlp.up_proj");
        switch_gate_proj_.load(weights, p + "mlp.switch_mlp.gate_proj");
        switch_up_gate_fused_ = false;
    }
    switch_down_proj_.load(weights, p + "mlp.switch_mlp.down_proj");
}

void MapleSparseMoeBlock::load_streaming_weights(
    const std::unordered_map<std::string, array>& weights,
    const std::string& prefix,
    MapleExpertStore* expert_store) {
    if (!expert_store) {
        throw std::runtime_error("Maple streamed MoE requires an expert store");
    }
    std::string p = prefix.empty() ? "" : prefix + ".";
    gate_weight_ = safe_at2(weights, p + "mlp.gate.weight");
    if (layer_idx_ < 0 || layer_idx_ >= expert_store->config().num_layers ||
        layer_idx_ < expert_store->config().first_moe_layer) {
        throw std::runtime_error("Maple streamed MoE layer is outside the expert store");
    }
    if (expert_store->config().num_experts != num_experts_ ||
        expert_store->config().hidden_size != hidden_size_ ||
        expert_store->config().moe_intermediate_size != moe_intermediate_size_) {
        throw std::runtime_error("Maple streamed MoE dimensions do not match the model");
    }
    expert_store_ = expert_store;
}

array MapleSparseMoeBlock::operator()(const array& x) {
    auto b = x.shape(0);
    auto l = x.shape(1);
    auto h = x.shape(2);
    auto x_flat = reshape(x, {-1, h});

    array topk_idx(0.0f), topk_val(0.0f);
    const bool fused_router_supported =
        b == 1 && l == 1 && num_experts_per_tok_ == 8 &&
        num_experts_ % 32 == 0 && hidden_size_ % 128 == 0;
    if (fused_router_supported) {
        auto router_res = fused_router(x_flat, gate_weight_, router_ctr_, num_experts_);
        topk_idx = reshape(router_res.first, {b, l, -1});
        topk_val = reshape(router_res.second, {b, l, -1});
    } else {
        auto x_f32 = astype(x, float32);
        auto gate_weight_f32 = astype(gate_weight_, float32);
        auto gates = matmul(x_f32, swapaxes(gate_weight_f32, -1, -2));
        auto scores = softmax(gates, -1);
        topk_idx = argpartition(scores, -num_experts_per_tok_, -1);
        topk_idx = slice(topk_idx, {0, 0, scores.shape(-1) - num_experts_per_tok_},
                        {b, l, scores.shape(-1)});
        topk_val = take_along_axis(scores, topk_idx, -1);
        auto sums = sum(topk_val, -1, /* keepdims */ true);
        topk_val = divide(topk_val, add(sums, array(1e-20f)));
    }

    if (expert_store_) {
        if (b != 1 || layer_idx_ < 0) {
            throw std::runtime_error(
                "Maple streamed MoE requires a single sequence");
        }
        eval(topk_idx, topk_val);
        const int route_count = l * num_experts_per_tok_;
        if (route_count >= 64) {
            /* Match Maple's reference prefill dispatch without materializing
             * a 256-expert layer.  Routes are sorted by the real expert ID,
             * remapped onto a compact ascending RHS, evaluated in one
             * gather_qmm, then scattered back to token/top-k order. */
            auto ids_flat = reshape(astype(topk_idx, int32), {route_count});
            /* Use MLX's argsort, not a host stable sort.  Equal-expert rows
             * can be assigned different gather_qmm tiles depending on their
             * tie order; matching the checkpoint graph avoids a one-BF16-step
             * drift observed first at layer 21. */
            auto mlx_order = astype(argsort(ids_flat), int32);
            eval(ids_flat, mlx_order);
            const int32_t* ids = ids_flat.data<int32_t>();
            std::vector<int> order(static_cast<size_t>(route_count));
            for (int slot = 0; slot < route_count; ++slot) {
                order[static_cast<size_t>(slot)] =
                    static_cast<int>(mlx_order.data<int32_t>()[slot]);
            }

            std::vector<int> rhs_experts;
            std::vector<int32_t> compact_ids(static_cast<size_t>(route_count));
            std::vector<int32_t> token_rows(static_cast<size_t>(route_count));
            std::vector<int32_t> inverse(static_cast<size_t>(route_count));
            int previous = -1;
            int compact = -1;
            for (int sorted = 0; sorted < route_count; ++sorted) {
                const int slot = order[static_cast<size_t>(sorted)];
                const int expert = ids[slot];
                if (expert < 0 || expert >= num_experts_) {
                    throw std::runtime_error(
                        "Maple router selected an invalid expert");
                }
                if (expert != previous) {
                    rhs_experts.push_back(expert);
                    previous = expert;
                    ++compact;
                }
                compact_ids[static_cast<size_t>(sorted)] = compact;
                token_rows[static_cast<size_t>(sorted)] =
                    slot / num_experts_per_tok_;
                inverse[static_cast<size_t>(slot)] = sorted;
            }

            auto row_indices = array(
                token_rows.data(), {route_count}, int32);
            auto compact_indices = array(
                compact_ids.data(), {route_count}, int32);
            auto inverse_indices = array(
                inverse.data(), {route_count}, int32);
            auto rows = expand_dims(take(
                reshape(x, {l, hidden_size_}), row_indices, 0), -2);
            auto sorted_outputs = streamed_expert_batch(
                rows, *expert_store_, layer_idx_, rhs_experts,
                compact_indices, true);
            auto outputs = reshape(
                take(sorted_outputs, inverse_indices, 0),
                {1, l, num_experts_per_tok_, hidden_size_});
            auto output = sum(
                multiply(astype(outputs, float32),
                         expand_dims(topk_val, -1)), -2);
            output = astype(output, x.dtype());
            eval(output);
            return output;
        }

        std::vector<array> token_outputs;
        token_outputs.reserve(static_cast<size_t>(l));
        for (int token = 0; token < l; ++token) {
            auto token_ids = reshape(astype(
                slice(topk_idx, {0, token, 0},
                      {1, token + 1, num_experts_per_tok_}), int32),
                {num_experts_per_tok_});
            auto token_scores = reshape(
                slice(topk_val, {0, token, 0},
                      {1, token + 1, num_experts_per_tok_}),
                {1, num_experts_per_tok_, 1});
            auto token_x = reshape(
                slice(x, {0, token, 0}, {1, token + 1, hidden_size_}),
                {1, hidden_size_});
            eval(token_ids, token_scores, token_x);
            const int32_t* expert_ids = token_ids.data<int32_t>();

            std::vector<int> selected_experts;
            selected_experts.reserve(
                static_cast<size_t>(num_experts_per_tok_));
            for (int i = 0; i < num_experts_per_tok_; ++i) {
                selected_experts.push_back(static_cast<int>(expert_ids[i]));
            }
            auto outputs = streamed_expert_outputs(
                token_x, *expert_store_, layer_idx_, selected_experts);
            auto output = sum(
                multiply(astype(outputs, float32), token_scores), 1);
            output = astype(output, x.dtype());
            eval(output);
            token_outputs.push_back(output);
        }
        return reshape(stack(token_outputs, 1), {1, l, hidden_size_});
    }

    // 3. Dispatch to experts
    auto x_exp = reshape(x, {b, l, 1, 1, h});
    array up(0.0f), gate(0.0f);
    if (switch_up_gate_fused_) {
        auto up_gate_w = take(switch_up_gate_proj_.weight, topk_idx, 0);
        array up_gate(0.0f);
        if (switch_up_gate_proj_.is_quantized) {
            auto scales = take(switch_up_gate_proj_.scales, topk_idx, 0);
            auto biases = take(switch_up_gate_proj_.biases, topk_idx, 0);
            up_gate = quantized_matmul(
                x_exp, up_gate_w, scales, biases, true,
                switch_up_gate_proj_.group_size, switch_up_gate_proj_.bits);
        } else {
            up_gate = matmul(x_exp, swapaxes(up_gate_w, -1, -2));
        }
        up_gate = squeeze(up_gate, -2);
        up = slice(up_gate, {0, 0, 0, 0},
                   {b, l, num_experts_per_tok_, moe_intermediate_size_});
        gate = slice(up_gate, {0, 0, 0, moe_intermediate_size_},
                     {b, l, num_experts_per_tok_, 2 * moe_intermediate_size_});
    } else {
        auto up_w = take(switch_up_proj_.weight, topk_idx, 0);
        auto gate_w = take(switch_gate_proj_.weight, topk_idx, 0);
        if (switch_up_proj_.is_quantized) {
            auto up_scales = take(switch_up_proj_.scales, topk_idx, 0);
            auto up_biases = take(switch_up_proj_.biases, topk_idx, 0);
            up = quantized_matmul(x_exp, up_w, up_scales, up_biases, true, switch_up_proj_.group_size, switch_up_proj_.bits);

            auto gate_scales = take(switch_gate_proj_.scales, topk_idx, 0);
            auto gate_biases = take(switch_gate_proj_.biases, topk_idx, 0);
            gate = quantized_matmul(x_exp, gate_w, gate_scales, gate_biases, true, switch_gate_proj_.group_size, switch_gate_proj_.bits);
        } else {
            up = matmul(x_exp, swapaxes(up_w, -1, -2));
            gate = matmul(x_exp, swapaxes(gate_w, -1, -2));
        }
        up = squeeze(up, -2);
        gate = squeeze(gate, -2);
    }

    auto act = clamped_swiglu(gate, up, 7.0f);
    act = expand_dims(act, -2);

    auto down_w = take(switch_down_proj_.weight, topk_idx, 0);
    array down(0.0f);
    if (switch_down_proj_.is_quantized) {
        auto scales = take(switch_down_proj_.scales, topk_idx, 0);
        auto biases = take(switch_down_proj_.biases, topk_idx, 0);
        down = quantized_matmul(act, down_w, scales, biases, true, switch_down_proj_.group_size, switch_down_proj_.bits);
    } else {
        down = matmul(act, swapaxes(down_w, -1, -2));
    }
    down = squeeze(down, -2);

    auto scores = expand_dims(topk_val, -1);
    auto out = sum(multiply(astype(down, float32), scores), -2);
    return astype(out, x.dtype());
}

MapleDecoderLayer::MapleDecoderLayer(const ModelArgs& args, int layer_idx)
    : args_(args), is_moe_(layer_idx >= args.first_k_dense_replace), self_attn_(args, layer_idx),
      input_layernorm_weight_(array(0.0f)), post_attention_layernorm_weight_(array(0.0f)) {
    if (is_moe_) {
        moe_mlp_ = std::make_unique<MapleSparseMoeBlock>(args, layer_idx);
    } else {
        mlp_ = std::make_unique<MapleMLP>(args);
    }
}

void MapleDecoderLayer::load_weights(const std::unordered_map<std::string, array>& weights, const std::string& prefix) {
    std::string p = prefix.empty() ? "" : prefix + ".";
    input_layernorm_weight_ = safe_at2(weights, p + "input_layernorm.weight");
    post_attention_layernorm_weight_ = safe_at2(weights, p + "post_attention_layernorm.weight");
    self_attn_.load_weights(weights, prefix);
    if (is_moe_) {
        moe_mlp_->load_weights(weights, prefix);
    } else {
        mlp_->load_weights(weights, prefix);
    }
}

void MapleDecoderLayer::load_streaming_weights(
    const std::unordered_map<std::string, array>& weights,
    const std::string& prefix,
    MapleExpertStore* expert_store) {
    std::string p = prefix.empty() ? "" : prefix + ".";
    input_layernorm_weight_ = safe_at2(weights, p + "input_layernorm.weight");
    post_attention_layernorm_weight_ = safe_at2(weights, p + "post_attention_layernorm.weight");
    self_attn_.load_weights(weights, prefix);
    self_attn_.enable_streaming_decode();
    if (is_moe_) {
        moe_mlp_->load_streaming_weights(weights, prefix, expert_store);
    } else {
        mlp_->load_weights(weights, prefix);
    }
}

array MapleDecoderLayer::operator()(const array& x, const std::optional<array>& mask, KVCache* cache) {
    auto norm1 = maple_rms_norm(x, input_layernorm_weight_, args_.rms_norm_eps);
    auto h_attn = self_attn_(norm1, mask, cache);

    auto h_mid = add(x, h_attn);

    auto norm2 = maple_rms_norm(h_mid, post_attention_layernorm_weight_, args_.rms_norm_eps);
    auto r = is_moe_ ? (*moe_mlp_)(norm2) : (*mlp_)(norm2);
    auto h_out = add(h_mid, r);
    return h_out;
}

std::pair<array, array> MapleDecoderLayer::decode_fused(
    const array& h, const array& r, const std::optional<array>& mask,
    KVCache* cache) {
    auto first = add_rms_norm_pair(
        h, r, input_layernorm_weight_, args_.rms_norm_eps);
    auto attn = self_attn_(first.second, mask, cache);
    auto second = add_rms_norm_pair(
        first.first, attn, post_attention_layernorm_weight_, args_.rms_norm_eps);
    auto mlp = is_moe_ ? (*moe_mlp_)(second.second) : (*mlp_)(second.second);
    return {second.first, mlp};
}

MapleModel::MapleModel(const ModelArgs& args)
    : args_(args), word_embeddings_weight_(array(0.0f)), norm_weight_(array(0.0f)) {
    for (int i = 0; i < args.num_hidden_layers; ++i) {
        layers_.emplace_back(args, i);
    }
}

void MapleModel::load_weights(const std::unordered_map<std::string, array>& weights) {
    if (weights.count("model.word_embeddings.scales")) {
        if (weights.count("model.word_embeddings.weight")) {
            word_embeddings_weight_ = safe_at2(weights, "model.word_embeddings.weight");
        } else {
            word_embeddings_weight_ = safe_at2(weights, "model.embed_tokens.weight");
        }
        word_embeddings_scales_ = safe_at2(weights, "model.word_embeddings.scales");
        word_embeddings_biases_ = safe_at2(weights, "model.word_embeddings.biases");
        word_embeddings_quantized_ = true;
    } else {
        if (weights.count("model.word_embeddings.weight")) {
            word_embeddings_weight_ = safe_at2(weights, "model.word_embeddings.weight");
        } else {
            word_embeddings_weight_ = safe_at2(weights, "model.embed_tokens.weight");
        }
        word_embeddings_quantized_ = false;
    }

    norm_weight_ = safe_at2(weights, "model.norm.weight");
    lm_head_proj_.load(weights, "lm_head");
    for (int i = 0; i < args_.num_hidden_layers; ++i) {
        layers_[i].load_weights(weights, "model.layers." + std::to_string(i));
    }
}

void MapleModel::load_streaming_weights(
    const std::unordered_map<std::string, array>& weights,
    MapleExpertStore&& expert_store) {
    if (!expert_store.cache_enabled()) {
        throw std::runtime_error("Maple streamed model requires an enabled expert cache");
    }
    if (expert_store.config().num_layers != args_.num_hidden_layers ||
        expert_store.config().num_experts != args_.num_experts ||
        expert_store.config().hidden_size != args_.hidden_size ||
        expert_store.config().moe_intermediate_size != args_.moe_intermediate_size) {
        throw std::runtime_error("Maple streamed store dimensions do not match the model");
    }

    if (weights.count("model.word_embeddings.scales")) {
        if (weights.count("model.word_embeddings.weight")) {
            word_embeddings_weight_ = safe_at2(weights, "model.word_embeddings.weight");
        } else {
            word_embeddings_weight_ = safe_at2(weights, "model.embed_tokens.weight");
        }
        word_embeddings_scales_ = safe_at2(weights, "model.word_embeddings.scales");
        word_embeddings_biases_ = safe_at2(weights, "model.word_embeddings.biases");
        word_embeddings_quantized_ = true;
    } else {
        if (weights.count("model.word_embeddings.weight")) {
            word_embeddings_weight_ = safe_at2(weights, "model.word_embeddings.weight");
        } else {
            word_embeddings_weight_ = safe_at2(weights, "model.embed_tokens.weight");
        }
        word_embeddings_quantized_ = false;
    }

    norm_weight_ = safe_at2(weights, "model.norm.weight");
    lm_head_proj_.load(weights, "lm_head");
    expert_store_ = std::make_unique<MapleExpertStore>(std::move(expert_store));
    for (int i = 0; i < args_.num_hidden_layers; ++i) {
        layers_[i].load_streaming_weights(
            weights, "model.layers." + std::to_string(i), expert_store_.get());
    }
}

array MapleModel::operator()(const array& inputs, std::vector<KVCache*>* caches) {


    array h(0.0f);
    if (word_embeddings_quantized_) {
        auto w_taken = take(word_embeddings_weight_, inputs, 0);
        auto s_taken = take(*word_embeddings_scales_, inputs, 0);
        auto b_taken = take(*word_embeddings_biases_, inputs, 0);
        h = dequantize(w_taken, s_taken, b_taken, 64, 4, "affine", std::nullopt, bfloat16);
    } else {
        h = take(word_embeddings_weight_, inputs, 0);
    }


    const bool fused_streaming_decode =
        expert_store_ && inputs.shape(0) == 1 && inputs.shape(1) == 1 &&
        h.size() == static_cast<size_t>(h.shape(-1)) &&
        h.shape(-1) % 256 == 0;
    array residual = fused_streaming_decode ? zeros(h.shape(), h.dtype())
                                            : array(0.0f);
    if (fused_streaming_decode) {
        /* Match the checkpoint implementation's dispatch boundary and keep
         * the serial token graph from retaining the embedding gather. */
        eval(h, residual);
    }

    for (size_t i = 0; i < layers_.size(); ++i) {
        KVCache* cache = (caches && i < caches->size()) ? (*caches)[i] : nullptr;
        std::optional<array> mask = std::nullopt;
        /* mlx-lm's cache mask contract returns None for a single query token:
         * the cache contains only past/current keys, so causality is already
         * guaranteed.  Passing an all-zero mask is mathematically redundant
         * but selects a different SDPA kernel and changes BF16 rounding. */
        if (inputs.shape(1) > 1) {
            const int query_len = inputs.shape(1);
            const int old_offset = cache ? cache->offset : 0;
            const int old_len = cache ? cache->size() : 0;
            int total_len = old_len + query_len;
            const bool sliding = i < args_.layer_types.size() &&
                                  args_.layer_types[i] == "sliding_attention";
            if (sliding && args_.sliding_window > 0) {
                total_len = std::min(total_len, args_.sliding_window);
            }

            /* mlx-lm uses the optimized causal mode unless a sliding batch
             * actually crosses the window boundary. */
            const int mask_offset = sliding && args_.sliding_window > 0
                ? std::min(args_.sliding_window - 1, old_offset)
                : old_offset;
            const bool needs_array_mask =
                sliding && args_.sliding_window > 0 &&
                mask_offset + query_len > args_.sliding_window;
            if (needs_array_mask) {
                // Keys are appended before attention.  For a rotating cache
                // they represent the final total_len absolute positions.
                const int first_key_pos = old_offset + query_len - total_len;
                auto qpos = reshape(arange(query_len), {query_len, 1}) + array(old_offset);
                auto kpos = reshape(arange(total_len), {1, total_len}) + array(first_key_pos);
                auto blocked = greater(kpos, qpos);
                blocked = logical_or(
                    blocked,
                    less(kpos, qpos - array(args_.sliding_window - 1)));
                mask = astype(
                    multiply(astype(blocked, float32), array(-3.4028235e38f)),
                    bfloat16);
            }
        }
        if (fused_streaming_decode) {
            auto state = layers_[i].decode_fused(h, residual, mask, cache);
            h = state.first;
            residual = state.second;
        } else {
            h = layers_[i](h, mask, cache);
        }
    }

    h = fused_streaming_decode
        ? add_rms_norm_pair(h, residual, norm_weight_, args_.rms_norm_eps).second
        : maple_rms_norm(h, norm_weight_, args_.rms_norm_eps);
    /* Keep the reference lm_head row shape.  Projecting only the final prompt
     * row selects a different quantized Metal kernel and changes BF16 logits.
     * run_token_sequence caps this allocation at 64 rows (~19 MiB). */
    auto logits = lm_head_proj_(h);
    return logits;
}

array MapleModel::run_token_sequence(const array& inputs,
                                     std::vector<KVCache*>* caches) {
    if (inputs.ndim() != 2 || inputs.shape(0) != 1 || inputs.shape(1) <= 0) {
        throw std::runtime_error("Maple token sequence must be [1, tokens]");
    }
    if (!caches || caches->size() < layers_.size()) {
        throw std::runtime_error("Maple streamed token sequence requires layer caches");
    }

    constexpr int kPrefillChunkTokens = 64;
    array last(0.0f);
    for (int start = 0; start < inputs.shape(1);
         start += kPrefillChunkTokens) {
        const int end = std::min(inputs.shape(1), start + kPrefillChunkTokens);
        auto chunk = slice(inputs, {0, start}, {1, end});
        auto projected = (*this)(chunk, caches);
        /* Hidden activations are capped at 64 tokens, the vocabulary head is
         * capped at the same bounded chunk, and every routed expert is still
         * evaluated and released before the next layer.  Evaluate the full
         * projection before slicing so MLX preserves the reference kernel. */
        eval(projected);
        last = slice(
            projected, {0, projected.shape(1) - 1, 0},
            {1, projected.shape(1), projected.shape(2)});
        eval(last);
    }
    return last;
}

void MapleModel::cache_stats(ecache_stats* stats) const {
    if (!stats) return;
    if (expert_store_) {
        expert_store_->cache_stats(stats);
    } else {
        std::memset(stats, 0, sizeof(*stats));
    }
}

MapleModel load_maple_model(const std::string& model_dir) {
    // The HTTP backend evaluates the model from detached worker threads.
    // Load tensors on a stream usable from those threads so their lazy MLX
    // graphs do not retain a main-thread-only CPU stream.
    auto load_stream = new_thread_unsafe_stream(Device::cpu);
    set_default_device(Device::cpu);
    set_default_stream(load_stream);

    std::string config_path = model_dir + "/config.json";
    FILE* f = fopen(config_path.c_str(), "r");
    if (!f) {
        throw std::runtime_error("Could not open " + config_path);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string config_text(size, '\0');
    fread(&config_text[0], 1, size, f);
    fclose(f);

    jval* config = json_parse(config_text.c_str(), nullptr);
    if (!config) {
        throw std::runtime_error("Failed to parse config.json");
    }

    ModelArgs args;
    if (jval* v = json_get(config, "hidden_size")) args.hidden_size = (int)v->num;
    if (jval* v = json_get(config, "intermediate_size")) args.intermediate_size = (int)v->num;
    if (jval* v = json_get(config, "moe_intermediate_size")) args.moe_intermediate_size = (int)v->num;
    if (jval* v = json_get(config, "num_hidden_layers")) args.num_hidden_layers = (int)v->num;
    if (jval* v = json_get(config, "num_attention_heads")) args.num_attention_heads = (int)v->num;
    if (jval* v = json_get(config, "num_key_value_heads")) args.num_key_value_heads = (int)v->num;
    if (jval* v = json_get(config, "head_dim")) args.head_dim = (int)v->num;
    if (jval* v = json_get(config, "num_experts")) args.num_experts = (int)v->num;
    if (jval* v = json_get(config, "num_experts_per_tok")) args.num_experts_per_tok = (int)v->num;
    if (jval* v = json_get(config, "first_k_dense_replace")) args.first_k_dense_replace = (int)v->num;
    if (jval* v = json_get(config, "rms_norm_eps")) args.rms_norm_eps = (float)v->num;
    if (jval* v = json_get(config, "vocab_size")) args.vocab_size = (int)v->num;
    if (jval* v = json_get(config, "rope_theta")) args.rope_theta = (float)v->num;
    if (jval* v = json_get(config, "partial_rotary_factor")) args.partial_rotary_factor = (float)v->num;
    if (jval* v = json_get(config, "sliding_window")) args.sliding_window = (int)v->num;

    if (jval* v = json_get(config, "layer_types")) {
        args.layer_types.clear();
        for (int i = 0; i < v->len; ++i) {
            args.layer_types.push_back(v->kids[i]->str);
        }
    }

    json_free(config);

    MapleModel model(args);
    std::unordered_map<std::string, array> all_weights;

    const bool has_streaming_artifacts =
        std::filesystem::exists(model_dir + "/maple-experts.bin") &&
        std::filesystem::exists(model_dir + "/maple-resident.safetensors") &&
        std::filesystem::exists(model_dir + "/maple-manifest.json");
    const char* allow_full = std::getenv("SAMOSA_MAPLE_ALLOW_FULL_RESIDENT");
    const bool allow_full_resident = allow_full && std::string(allow_full) == "1";

    if (!has_streaming_artifacts && !allow_full_resident) {
        throw std::runtime_error(
            "Maple full-resident safetensor loading is disabled because it can "
            "consume around 10GB of unified memory. Build streaming artifacts "
            "with maple-pack, or set SAMOSA_MAPLE_ALLOW_FULL_RESIDENT=1 only "
            "for a guarded developer parity run.");
    }

    if (has_streaming_artifacts && !allow_full_resident) {
        /* The released path loads only the resident safetensor container.  The
         * six routed expert tensors are indexed by MapleExpertStore and are
         * fetched after routing; do not even open the source shard index here.
         * Any malformed or dimension-incompatible artifact is a startup
         * error, never a reason to fall back to the full-resident loader. */
        try {
            auto resident_weights =
                load_safetensors(model_dir + "/maple-resident.safetensors").first;
            MapleExpertStore expert_store = MapleExpertStore::open_packed(model_dir);
            expert_store.enable_cache();
            model.load_streaming_weights(resident_weights, std::move(expert_store));
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("Maple SSD streaming load failed closed: ") +
                                     e.what());
        }
        set_default_device(Device::gpu);
        set_default_stream(new_thread_unsafe_stream(Device::gpu));
        return model;
    }

    // Parse model.safetensors.index.json
    std::string index_path = model_dir + "/model.safetensors.index.json";
    FILE* f_index = fopen(index_path.c_str(), "r");
    if (!f_index) {
        throw std::runtime_error("Could not open " + index_path);
    }
    fseek(f_index, 0, SEEK_END);
    long index_size = ftell(f_index);
    fseek(f_index, 0, SEEK_SET);
    std::string index_text(index_size, '\0');
    fread(&index_text[0], 1, index_size, f_index);
    fclose(f_index);

    jval* index_json = json_parse(index_text.c_str(), nullptr);
    if (!index_json) {
        throw std::runtime_error("Failed to parse model.safetensors.index.json");
    }

    jval* weight_map = json_get(index_json, "weight_map");
    if (!weight_map || weight_map->t != J_OBJ) {
        throw std::runtime_error("Invalid weight_map in index.json");
    }

    std::unordered_set<std::string> shards_to_load;
    for (int i = 0; i < weight_map->len; ++i) {
        jval* val = weight_map->kids[i];
        if (val && val->t == J_STR) {
            std::string shard_name = val->str;
            if (shard_name.find("flashhead") == std::string::npos) {
                shards_to_load.insert(shard_name);
            }
        }
    }

    for (const auto& shard : shards_to_load) {
        auto dict = load_safetensors(model_dir + "/" + shard).first;
        for (auto& kv : dict) {
            all_weights.insert({kv.first, kv.second});
        }
    }

    json_free(index_json);

    model.load_weights(all_weights);
    set_default_device(Device::gpu);
    set_default_stream(new_thread_unsafe_stream(Device::gpu));
    return model;
}

} // namespace maple
} // namespace samosa
