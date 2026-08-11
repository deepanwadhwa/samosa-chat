#pragma once

#include "mlx/mlx.h"
#include "maple_expert_store.h"
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <iostream>
#include <optional>

namespace samosa {
namespace maple {

// 1. Maple Add + RMSNorm
std::pair<mlx::core::array, mlx::core::array> add_rms_norm_pair(
                              const mlx::core::array& h,
                              const mlx::core::array& r,
                              const mlx::core::array& w,
                              float eps);
mlx::core::array add_rms_norm(const mlx::core::array& h,
                              const mlx::core::array& r,
                              const mlx::core::array& w,
                              float eps);

// 2. Maple QK Norm + RoPE
mlx::core::array qk_norm_rope(const mlx::core::array& qk,
                              const mlx::core::array& w,
                              const mlx::core::array& inv_freq,
                              int head_dim,
                              int rope_dim,
                              float offset,
                              float eps);

// 3. Maple Fused Router
std::pair<mlx::core::array, mlx::core::array> fused_router(const mlx::core::array& x,
                                                           const mlx::core::array& w,
                                                           mlx::core::array& ctr_in,
                                                           int num_experts);

// 4. Clamped SwiGLU
mlx::core::array clamped_swiglu(const mlx::core::array& gate,
                                const mlx::core::array& x,
                                float mlp_clamp = 7.0f);

struct ModelArgs {
    int hidden_size = 2048;
    int intermediate_size = 5120;
    int moe_intermediate_size = 512;
    int num_hidden_layers = 24;
    int num_attention_heads = 16;
    int num_key_value_heads = 4;
    int head_dim = 128;
    int num_experts = 256;
    int num_experts_per_tok = 8;
    int first_k_dense_replace = 0;
    float rms_norm_eps = 1e-5f;
    int vocab_size = 32000;
    float rope_theta = 10000.0f;
    float partial_rotary_factor = 0.5f;
    int max_position_embeddings = 4096;
    int sliding_window = 4096;
    std::vector<std::string> layer_types;
};

class KVCache {
public:
    static constexpr int step = 256;
    mlx::core::array keys;
    mlx::core::array values;
    int offset = 0;

    KVCache() : keys(mlx::core::array(0.0f)), values(mlx::core::array(0.0f)) {}
    virtual ~KVCache() = default;

    virtual void update_and_fetch(mlx::core::array& k, mlx::core::array& v) {
        const int batch = k.shape(0);
        const int heads = k.shape(1);
        const int count = k.shape(2);
        const int key_dim = k.shape(3);
        const int value_dim = v.shape(3);
        const int previous = offset;
        if (keys.ndim() == 0 || previous + count > keys.shape(2)) {
            const int blocks = (step + count - 1) / step;
            auto new_keys = mlx::core::zeros(
                {batch, heads, blocks * step, key_dim}, k.dtype());
            auto new_values = mlx::core::zeros(
                {batch, heads, blocks * step, value_dim}, v.dtype());
            if (keys.ndim() > 0) {
                if (previous % step != 0) {
                    keys = mlx::core::slice(
                        keys, {0, 0, 0, 0},
                        {batch, heads, previous, key_dim});
                    values = mlx::core::slice(
                        values, {0, 0, 0, 0},
                        {batch, heads, previous, value_dim});
                }
                keys = mlx::core::concatenate({keys, new_keys}, 2);
                values = mlx::core::concatenate({values, new_values}, 2);
            } else {
                keys = new_keys;
                values = new_values;
            }
        }
        keys = mlx::core::slice_update(
            keys, k, {0, 0, previous, 0},
            {batch, heads, previous + count, key_dim});
        values = mlx::core::slice_update(
            values, v, {0, 0, previous, 0},
            {batch, heads, previous + count, value_dim});
        offset += count;
        k = mlx::core::slice(
            keys, {0, 0, 0, 0}, {batch, heads, offset, key_dim});
        v = mlx::core::slice(
            values, {0, 0, 0, 0}, {batch, heads, offset, value_dim});
    }

    virtual int size() const { return offset; }
};

class RotatingKVCache : public KVCache {
public:
    int max_size;
    int keep = 0;
    int index = 0;
    RotatingKVCache(int max_size) : max_size(max_size) {}

    void update_and_fetch(mlx::core::array& k, mlx::core::array& v) override {
        const int batch = k.shape(0);
        const int heads = k.shape(1);
        const int count = k.shape(2);
        const int key_dim = k.shape(3);
        const int value_dim = v.shape(3);

        if (count > 1) {
            if (keys.ndim() == 0) {
                keys = k;
                values = v;
            } else {
                /* Restore temporal order before a chunk append.  This is the
                 * same compact path mlx-lm uses for multi-token prefill. */
                if (index != keys.shape(2)) {
                    if (index < offset) {
                        keys = mlx::core::concatenate(
                            {mlx::core::slice(
                                 keys, {0, 0, index, 0},
                                 {batch, heads, keys.shape(2), key_dim}),
                             mlx::core::slice(
                                 keys, {0, 0, keep, 0},
                                 {batch, heads, index, key_dim})},
                            2);
                        values = mlx::core::concatenate(
                            {mlx::core::slice(
                                 values, {0, 0, index, 0},
                                 {batch, heads, values.shape(2), value_dim}),
                             mlx::core::slice(
                                 values, {0, 0, keep, 0},
                                 {batch, heads, index, value_dim})},
                            2);
                    } else {
                        keys = mlx::core::slice(
                            keys, {0, 0, 0, 0},
                            {batch, heads, index, key_dim});
                        values = mlx::core::slice(
                            values, {0, 0, 0, 0},
                            {batch, heads, index, value_dim});
                    }
                }
                index = keys.shape(2);
                const int trim = index - max_size + 1;
                if (trim > 0) {
                    keys = mlx::core::slice(
                        keys, {0, 0, trim + keep, 0},
                        {batch, heads, keys.shape(2), key_dim});
                    values = mlx::core::slice(
                        values, {0, 0, trim + keep, 0},
                        {batch, heads, values.shape(2), value_dim});
                }
                keys = mlx::core::concatenate({keys, k}, 2);
                values = mlx::core::concatenate({values, v}, 2);
            }
            offset += count;
            index = keys.shape(2);
            k = keys;
            v = values;
            return;
        }

        const int previous = offset;
        if (keys.ndim() == 0 ||
            (previous >= keys.shape(2) && keys.shape(2) < max_size)) {
            const int new_size = std::min(step, max_size - previous);
            auto new_keys = mlx::core::zeros(
                {batch, heads, new_size, key_dim}, k.dtype());
            auto new_values = mlx::core::zeros(
                {batch, heads, new_size, value_dim}, v.dtype());
            if (keys.ndim() > 0) {
                keys = mlx::core::concatenate({keys, new_keys}, 2);
                values = mlx::core::concatenate({values, new_values}, 2);
            } else {
                keys = new_keys;
                values = new_values;
            }
            index = previous;
        }

        const int trim = keys.shape(2) - max_size;
        if (trim > 0) {
            keys = mlx::core::slice(
                keys, {0, 0, trim + keep, 0},
                {batch, heads, keys.shape(2), key_dim});
            values = mlx::core::slice(
                values, {0, 0, trim + keep, 0},
                {batch, heads, values.shape(2), value_dim});
            index = max_size;
        }
        if (index == max_size) index = keep;

        keys = mlx::core::slice_update(
            keys, k, {0, 0, index, 0},
            {batch, heads, index + count, key_dim});
        values = mlx::core::slice_update(
            values, v, {0, 0, index, 0},
            {batch, heads, index + count, value_dim});
        offset += count;
        index += count;
        if (offset < max_size) {
            k = mlx::core::slice(
                keys, {0, 0, 0, 0}, {batch, heads, offset, key_dim});
            v = mlx::core::slice(
                values, {0, 0, 0, 0}, {batch, heads, offset, value_dim});
        } else {
            k = keys;
            v = values;
        }
    }

    int size() const override { return std::min(offset, max_size); }
};

struct QLinear {
    mlx::core::array weight;
    mlx::core::array scales;
    mlx::core::array biases;
    bool is_quantized = false;
    int group_size = 32;
    int bits = 2;

    QLinear() : weight(mlx::core::array(0.0f)), scales(mlx::core::array(0.0f)), biases(mlx::core::array(0.0f)) {}

    void load(const std::unordered_map<std::string, mlx::core::array>& weights, const std::string& prefix) {
        if (!weights.count(prefix + ".weight")) throw std::runtime_error("Key not found: " + prefix + ".weight");
        weight = weights.at(prefix + ".weight");
        if (weights.count(prefix + ".scales")) {
            scales = weights.at(prefix + ".scales");
            biases = weights.at(prefix + ".biases");
            is_quantized = true;
            group_size = 64;
            bits = 4;
        } else if (weights.count(prefix + ".row_alpha")) {
            auto alpha = weights.at(prefix + ".row_alpha");
            int n_groups = (weight.shape().back() * 16) / 128;
            std::vector<int> out_shape(alpha.shape().begin(), alpha.shape().end());
            out_shape.push_back(n_groups);
            scales = mlx::core::contiguous(mlx::core::broadcast_to(
                mlx::core::expand_dims(alpha, -1),
                mlx::core::Shape(out_shape.begin(), out_shape.end())));
            biases = mlx::core::contiguous(mlx::core::negative(scales));
            is_quantized = true;
            group_size = 128;
            bits = 2;
        } else {
            std::cout << "Warning: " << prefix << " is NOT quantized! No .scales or .row_alpha found!" << std::endl;
            is_quantized = false;
        }
    }

    mlx::core::array operator()(const mlx::core::array& x) const {
        if (is_quantized) {
            return mlx::core::quantized_matmul(x, weight, scales, biases, true, group_size, bits);
        } else {
            return mlx::core::matmul(x, mlx::core::swapaxes(weight, -1, -2));
        }
    }
};

class MapleAttention {
public:
    MapleAttention(const ModelArgs& args, int layer_idx);
    void load_weights(const std::unordered_map<std::string, mlx::core::array>& weights, const std::string& prefix);
    void enable_streaming_decode();

    // (B, L, hidden_size) -> (B, L, hidden_size)
    mlx::core::array operator()(const mlx::core::array& x, const std::optional<mlx::core::array>& mask = std::nullopt, KVCache* cache = nullptr);

public:
    ModelArgs args_;
    bool use_rope_;
    QLinear q_proj_;
    QLinear k_proj_;
    QLinear v_proj_;
    QLinear qkv_proj_;
    QLinear o_proj_;
    mlx::core::array q_norm_weight_;
    mlx::core::array k_norm_weight_;
    mlx::core::array qk_norm_weight_{0.0f};
    mlx::core::array inv_freq_{0.0f};
    bool streaming_decode_ = false;
    bool qkv_fused_ready_ = false;
    bool decode_aux_ready_ = false;
    int sliding_window_ = 0;

private:
    void ensure_streaming_decode_weights();
};

class MapleMLP {
public:
    MapleMLP(const ModelArgs& args);
    void load_weights(const std::unordered_map<std::string, mlx::core::array>& weights, const std::string& prefix);
    mlx::core::array operator()(const mlx::core::array& x);

private:
    QLinear gate_proj_;
    QLinear up_proj_;
    QLinear down_proj_;
};

class MapleSparseMoeBlock {
public:
    MapleSparseMoeBlock(const ModelArgs& args, int layer_idx = -1);
    void load_weights(const std::unordered_map<std::string, mlx::core::array>& weights, const std::string& prefix);
    void load_streaming_weights(const std::unordered_map<std::string, mlx::core::array>& weights,
                                const std::string& prefix,
                                MapleExpertStore* expert_store);
    mlx::core::array operator()(const mlx::core::array& x);

private:
    int num_experts_;
    int num_experts_per_tok_;
    int hidden_size_;
    int moe_intermediate_size_;
    int layer_idx_;
    mlx::core::array gate_weight_;
    QLinear switch_up_proj_;
    QLinear switch_gate_proj_;
    QLinear switch_up_gate_proj_;
    QLinear switch_down_proj_;
    bool switch_up_gate_fused_ = false;
    mlx::core::array router_ctr_; // initialized to zeros
    MapleExpertStore* expert_store_ = nullptr;
};

class MapleDecoderLayer {
public:
    MapleDecoderLayer(const ModelArgs& args, int layer_idx);
    void load_weights(const std::unordered_map<std::string, mlx::core::array>& weights, const std::string& prefix);
    void load_streaming_weights(const std::unordered_map<std::string, mlx::core::array>& weights,
                                const std::string& prefix,
                                MapleExpertStore* expert_store);
    mlx::core::array operator()(const mlx::core::array& x, const std::optional<mlx::core::array>& mask = std::nullopt, KVCache* cache = nullptr);
    std::pair<mlx::core::array, mlx::core::array> decode_fused(
        const mlx::core::array& h,
        const mlx::core::array& r,
        const std::optional<mlx::core::array>& mask = std::nullopt,
        KVCache* cache = nullptr);

private:
    ModelArgs args_;
    bool is_moe_;
    MapleAttention self_attn_;
    std::unique_ptr<MapleMLP> mlp_;
    std::unique_ptr<MapleSparseMoeBlock> moe_mlp_;

    mlx::core::array input_layernorm_weight_;
    mlx::core::array post_attention_layernorm_weight_;
};

class MapleModel {
public:
    MapleModel(const ModelArgs& args);
    void load_weights(const std::unordered_map<std::string, mlx::core::array>& weights);
    void load_streaming_weights(const std::unordered_map<std::string, mlx::core::array>& weights,
                                MapleExpertStore&& expert_store);
    mlx::core::array operator()(const mlx::core::array& inputs, std::vector<KVCache*>* caches = nullptr);
    /* Bounded chunked prefill for the streamed model.  Each chunk and its
     * selected-expert buffer are evaluated before the next chunk begins. */
    mlx::core::array run_token_sequence(const mlx::core::array& inputs,
                                        std::vector<KVCache*>* caches);
    bool streaming_enabled() const { return expert_store_ != nullptr; }
    void cache_stats(ecache_stats* stats) const;

    const ModelArgs& args() const { return args_; }

public:
    ModelArgs args_;
    mlx::core::array word_embeddings_weight_;
    std::optional<mlx::core::array> word_embeddings_scales_;
    std::optional<mlx::core::array> word_embeddings_biases_;
    bool word_embeddings_quantized_{false};
    mlx::core::array norm_weight_;
    QLinear lm_head_proj_;
    std::unique_ptr<MapleExpertStore> expert_store_;
    std::vector<MapleDecoderLayer> layers_;
};

MapleModel load_maple_model(const std::string& model_dir);

} // namespace maple
} // namespace samosa
