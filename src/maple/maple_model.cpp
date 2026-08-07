#include "maple_model.h"
#include "../json.h"
#include "mlx/fast.h"
#include <sstream>
#include <iomanip>
#include <iostream>
#include <unordered_set>
#include <filesystem>

namespace samosa {
namespace maple {

using namespace mlx::core;

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

array add_rms_norm(const array& h, const array& r, const array& w, float eps) {
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
    
    return out[1]; // We return the normalized h, wait, the signature in python returns both.
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
    
    auto out = kernel(
        {reshape(x, {-1}), w, ctr_in},
        {{8}, {8}, {num_experts}},
        {int32, float32, float32},
        std::make_tuple((num_experts / 32) * 256, 1, 1),
        std::make_tuple(256, 1, 1),
        {{"T_", w.dtype()}, {"NEXP", num_experts}, {"DIM", x.shape().back()}},
        std::nullopt,
        false,
        default_stream(default_device())
    );
    
    // out_indices is out[0], out_scores is out[1]
    auto shape = x.shape();
    shape.back() = 8;
    return {reshape(out[0], shape), reshape(out[1], shape)};
}

array clamped_swiglu(const array& gate, const array& x, float mlp_clamp) {
    auto clamped_gate = minimum(gate, array(mlp_clamp, gate.dtype()));
    auto act = multiply(clamped_gate, sigmoid(clamped_gate));
    auto clamped_x = clip(x, array(-mlp_clamp, x.dtype()), array(mlp_clamp, x.dtype()));
    return multiply(act, clamped_x);
}

MapleAttention::MapleAttention(const ModelArgs& args, int layer_idx)
    : args_(args), use_rope_(false),
      qkv_proj_weight_(array(0.0f)), o_proj_weight_(array(0.0f)),
      q_norm_weight_(array(0.0f)), k_norm_weight_(array(0.0f)) {
    if (layer_idx < args.layer_types.size() && args.layer_types[layer_idx] == "sliding_attention") {
        use_rope_ = true;
    }
}

void MapleAttention::load_weights(const std::unordered_map<std::string, array>& weights, const std::string& prefix) {
    qkv_proj_weight_ = weights.at(prefix + ".self_attn.qkv_proj.weight");
    o_proj_weight_ = weights.at(prefix + ".self_attn.o_proj.weight");
    q_norm_weight_ = weights.at(prefix + ".self_attn.q_norm.weight");
    k_norm_weight_ = weights.at(prefix + ".self_attn.k_norm.weight");
}

array MapleAttention::operator()(const array& x) {
    // Stage C check: just verify shapes for now. Actual execution will be in Stage E.
    return x; 
}

MapleMLP::MapleMLP(const ModelArgs& args) 
    : gate_proj_weight_(array(0.0f)), up_proj_weight_(array(0.0f)), down_proj_weight_(array(0.0f)) {}

void MapleMLP::load_weights(const std::unordered_map<std::string, array>& weights, const std::string& prefix) {
    gate_proj_weight_ = weights.at(prefix + ".mlp.gate_proj.weight");
    up_proj_weight_ = weights.at(prefix + ".mlp.up_proj.weight");
    down_proj_weight_ = weights.at(prefix + ".mlp.down_proj.weight");
}

array MapleMLP::operator()(const array& x) { return x; }

MapleSparseMoeBlock::MapleSparseMoeBlock(const ModelArgs& args) 
    : num_experts_(args.num_experts),
      gate_weight_(array(0.0f)), switch_up_gate_proj_weight_(array(0.0f)),
      switch_down_proj_weight_(array(0.0f)), router_ctr_(array(0.0f)) {
    router_ctr_ = zeros({8}, uint32);
}

void MapleSparseMoeBlock::load_weights(const std::unordered_map<std::string, array>& weights, const std::string& prefix) {
    gate_weight_ = weights.at(prefix + ".mlp.gate.weight");
    switch_up_gate_proj_weight_ = weights.at(prefix + ".mlp.switch_mlp.up_gate_proj.weight");
    switch_down_proj_weight_ = weights.at(prefix + ".mlp.switch_mlp.down_proj.weight");
}

array MapleSparseMoeBlock::operator()(const array& x) { return x; }

MapleDecoderLayer::MapleDecoderLayer(const ModelArgs& args, int layer_idx)
    : args_(args), is_moe_(layer_idx >= args.first_k_dense_replace), self_attn_(args, layer_idx),
      input_layernorm_weight_(array(0.0f)), post_attention_layernorm_weight_(array(0.0f)) {
    if (is_moe_) {
        moe_mlp_ = std::make_unique<MapleSparseMoeBlock>(args);
    } else {
        mlp_ = std::make_unique<MapleMLP>(args);
    }
}

void MapleDecoderLayer::load_weights(const std::unordered_map<std::string, array>& weights, const std::string& prefix) {
    input_layernorm_weight_ = weights.at(prefix + ".input_layernorm.weight");
    post_attention_layernorm_weight_ = weights.at(prefix + ".post_attention_layernorm.weight");
    self_attn_.load_weights(weights, prefix);
    if (is_moe_) {
        moe_mlp_->load_weights(weights, prefix);
    } else {
        mlp_->load_weights(weights, prefix);
    }
}

array MapleDecoderLayer::operator()(const array& x) { return x; }

MapleModel::MapleModel(const ModelArgs& args) 
    : args_(args), word_embeddings_weight_(array(0.0f)), norm_weight_(array(0.0f)), lm_head_weight_(array(0.0f)) {
    for (int i = 0; i < args.num_hidden_layers; ++i) {
        layers_.emplace_back(args, i);
    }
}

void MapleModel::load_weights(const std::unordered_map<std::string, array>& weights) {
    word_embeddings_weight_ = weights.at("model.embed_tokens.weight");
    norm_weight_ = weights.at("model.norm.weight");
    lm_head_weight_ = weights.at("lm_head.weight");
    for (int i = 0; i < args_.num_hidden_layers; ++i) {
        layers_[i].load_weights(weights, "model.layers." + std::to_string(i));
    }
}

array MapleModel::operator()(const array& inputs) { return inputs; }

MapleModel load_maple_model(const std::string& model_dir) {
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
    
    if (jval* v = json_get(config, "layer_types")) {
        args.layer_types.clear();
        for (int i = 0; i < v->len; ++i) {
            args.layer_types.push_back(v->kids[i]->str);
        }
    }
    
    json_free(config);
    
    MapleModel model(args);
    std::unordered_map<std::string, array> all_weights;
    
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
            shards_to_load.insert(val->str);
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
    return model;
}

} // namespace maple
} // namespace samosa

