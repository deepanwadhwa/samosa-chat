#pragma once

#include "mlx/mlx.h"
#include <string>

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
    float rms_norm_eps = 1e-6f;
    int vocab_size = 151936;
    std::vector<std::string> layer_types;
};

class MapleAttention {
public:
    MapleAttention(const ModelArgs& args, int layer_idx);
    void load_weights(const std::unordered_map<std::string, mlx::core::array>& weights, const std::string& prefix);
    
    // (B, L, hidden_size) -> (B, L, hidden_size)
    mlx::core::array operator()(const mlx::core::array& x);
    
private:
    ModelArgs args_;
    bool use_rope_;
    mlx::core::array qkv_proj_weight_;
    mlx::core::array o_proj_weight_;
    mlx::core::array q_norm_weight_;
    mlx::core::array k_norm_weight_;
};

class MapleMLP {
public:
    MapleMLP(const ModelArgs& args);
    void load_weights(const std::unordered_map<std::string, mlx::core::array>& weights, const std::string& prefix);
    mlx::core::array operator()(const mlx::core::array& x);
    
private:
    mlx::core::array gate_proj_weight_;
    mlx::core::array up_proj_weight_;
    mlx::core::array down_proj_weight_;
};

class MapleSparseMoeBlock {
public:
    MapleSparseMoeBlock(const ModelArgs& args);
    void load_weights(const std::unordered_map<std::string, mlx::core::array>& weights, const std::string& prefix);
    mlx::core::array operator()(const mlx::core::array& x);
    
private:
    int num_experts_;
    mlx::core::array gate_weight_;
    mlx::core::array switch_up_gate_proj_weight_;
    mlx::core::array switch_down_proj_weight_;
    mlx::core::array router_ctr_; // initialized to zeros
};

class MapleDecoderLayer {
public:
    MapleDecoderLayer(const ModelArgs& args, int layer_idx);
    void load_weights(const std::unordered_map<std::string, mlx::core::array>& weights, const std::string& prefix);
    mlx::core::array operator()(const mlx::core::array& x);
    
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
    mlx::core::array operator()(const mlx::core::array& inputs);
    
private:
    ModelArgs args_;
    mlx::core::array word_embeddings_weight_;
    mlx::core::array norm_weight_;
    mlx::core::array lm_head_weight_;
    std::vector<MapleDecoderLayer> layers_;
};

MapleModel load_maple_model(const std::string& model_dir);

} // namespace maple
} // namespace samosa
