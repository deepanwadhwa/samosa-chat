#pragma once

#include "mlx/mlx.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <iostream>

namespace samosa {
namespace maple {

// 1. Maple Add + RMSNorm
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
    int max_position_embeddings = 4096;
    int sliding_window = 4096;
    std::vector<std::string> layer_types;
};

class KVCache {
public:
    mlx::core::array keys;
    mlx::core::array values;
    int offset = 0;

    KVCache() : keys(mlx::core::array(0.0f)), values(mlx::core::array(0.0f)) {}
    virtual ~KVCache() = default;

    virtual void update_and_fetch(mlx::core::array& k, mlx::core::array& v) {
        if (keys.ndim() == 0) {
            keys = k;
            values = v;
        } else {
            keys = mlx::core::concatenate({keys, k}, 2);
            values = mlx::core::concatenate({values, v}, 2);
        }
        offset += k.shape(2);
        k = keys;
        v = values;
    }
};

class RotatingKVCache : public KVCache {
public:
    int max_size;
    RotatingKVCache(int max_size) : max_size(max_size) {}

    void update_and_fetch(mlx::core::array& k, mlx::core::array& v) override {
        if (keys.ndim() == 0) {
            keys = k;
            values = v;
        } else {
            keys = mlx::core::concatenate({keys, k}, 2);
            values = mlx::core::concatenate({values, v}, 2);
        }

        if (keys.shape(2) > max_size) {
            keys = mlx::core::slice(keys, {0, 0, keys.shape(2) - max_size, 0}, {keys.shape(0), keys.shape(1), keys.shape(2), keys.shape(3)});
            values = mlx::core::slice(values, {0, 0, values.shape(2) - max_size, 0}, {values.shape(0), values.shape(1), values.shape(2), values.shape(3)});
        }
        offset += k.shape(2);
        k = keys;
        v = values;
    }
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
            scales = mlx::core::broadcast_to(mlx::core::expand_dims(alpha, -1), mlx::core::Shape(out_shape.begin(), out_shape.end()));
            biases = mlx::core::negative(scales);
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
    
    // (B, L, hidden_size) -> (B, L, hidden_size)
    mlx::core::array operator()(const mlx::core::array& x, const std::optional<mlx::core::array>& mask = std::nullopt, KVCache* cache = nullptr);
    
public:
    ModelArgs args_;
    bool use_rope_;
    QLinear q_proj_;
    QLinear k_proj_;
    QLinear v_proj_;
    QLinear o_proj_;
    mlx::core::array q_norm_weight_;
    mlx::core::array k_norm_weight_;
    int sliding_window_ = 0;
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
    MapleSparseMoeBlock(const ModelArgs& args);
    void load_weights(const std::unordered_map<std::string, mlx::core::array>& weights, const std::string& prefix);
    mlx::core::array operator()(const mlx::core::array& x);
    
private:
    int num_experts_;
    mlx::core::array gate_weight_;
    QLinear switch_up_proj_;
    QLinear switch_gate_proj_;
    QLinear switch_down_proj_;
    mlx::core::array router_ctr_; // initialized to zeros
};

class MapleDecoderLayer {
public:
    MapleDecoderLayer(const ModelArgs& args, int layer_idx);
    void load_weights(const std::unordered_map<std::string, mlx::core::array>& weights, const std::string& prefix);
    mlx::core::array operator()(const mlx::core::array& x, const std::optional<mlx::core::array>& mask = std::nullopt, KVCache* cache = nullptr);
    
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
    mlx::core::array operator()(const mlx::core::array& inputs, std::vector<KVCache*>* caches = nullptr);
    
    const ModelArgs& args() const { return args_; }
    
public:
    ModelArgs args_;
    mlx::core::array word_embeddings_weight_;
    std::optional<mlx::core::array> word_embeddings_scales_;
    std::optional<mlx::core::array> word_embeddings_biases_;
    bool word_embeddings_quantized_{false};
    mlx::core::array norm_weight_;
    QLinear lm_head_proj_;
    std::vector<MapleDecoderLayer> layers_;
};

MapleModel load_maple_model(const std::string& model_dir);

} // namespace maple
} // namespace samosa
