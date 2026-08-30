#ifndef SAMOSA_VISIONPSY_MODEL_H
#define SAMOSA_VISIONPSY_MODEL_H

#include "mlx/mlx.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <atomic>

namespace samosa {
namespace visionpsy {

struct VisionPsyConfig {
    // Text Config (SmolLM2-360M)
    int vocab_size = 49218;
    int hidden_size = 960;
    int intermediate_size = 2560;
    int num_hidden_layers = 32;
    int num_attention_heads = 15;
    int num_key_value_heads = 5;
    int head_dim = 64;
    float rms_norm_eps = 1e-5f;
    float rope_theta = 100000.0f;
    int max_position_embeddings = 8192;

    // Vision Config (SigLIP2-Base-512)
    int vit_hidden_size = 768;
    int vit_intermediate_size = 3072;
    int vit_num_hidden_layers = 12;
    int vit_num_attention_heads = 12;
    int vit_head_dim = 64;
    int vit_image_size = 512;
    int vit_patch_size = 16;
    float vit_layer_norm_eps = 1e-6f;

    // Projector Config
    int pixel_shuffle_factor = 4;
    int mp_image_token_length = 64; // (512/16/4)^2 = 8*8 = 64
    int max_img_size = 2048;
};

struct ImageTiles {
    std::vector<mlx::core::array> tiles; // Each tile is [1, 512, 512, 3] in [0, 1]
    int n_w = 1;
    int n_h = 1;
    bool has_global = false;
    int orig_w = 0;
    int orig_h = 0;
};

struct KVCache {
    std::vector<mlx::core::array> k;
    std::vector<mlx::core::array> v;
    int offset = 0;

    void reset() {
        k.clear();
        v.clear();
        offset = 0;
    }
};

struct InferenceResult {
    std::string text;
    int prompt_tokens = 0;
    int generated_tokens = 0;
    long long prefill_ms = 0;
    long long decode_ms = 0;
    std::string error;
};

class VisionPsyModel {
public:
    VisionPsyModel();
    ~VisionPsyModel();

    // Loads weights and tokenizer from model_dir
    bool load(const std::string& model_dir);
    bool is_loaded() const { return is_loaded_; }
    const std::string& last_error() const { return last_error_; }

    // Validates image bytes against decompression bombs and extracts dimension info
    bool inspect_image_header(const unsigned char* data, size_t len, int* out_w, int* out_h) const;

    // Preprocesses an image with aspect-preserving DynamicResize and standard tiling
    // max_side_len: 2048 (standard), 1024/1536 (under memory pressure), 512 (single-tile)
    bool preprocess_image_tiles(const unsigned char* data, size_t len,
                                int max_side_len, ImageTiles* out_tiles) const;

    // Forward pass for a single 512x512 tile through SigLIP2 + Pixel-Shuffle Projector -> [64, 960]
    mlx::core::array encode_tile(const mlx::core::array& tile_pixels);

    // Formats and tokenizes prompt according to upstream chat template and dynamic tiles
    std::vector<int> build_prompt_tokens(const std::string& user_prompt,
                                         const ImageTiles& tiles) const;

    // End-to-end inference from ImageTiles and prompt
    InferenceResult generate_from_tiles(const ImageTiles& tiles,
                                        const std::string& user_prompt,
                                        int max_new_tokens = 512,
                                        const std::atomic<bool>* cancel_flag = nullptr);

    // Direct text decode / tokenization helpers
    std::vector<int> encode_text(const std::string& text) const;
    std::string decode_tokens(const std::vector<int>& tokens) const;

    int image_token_id() const { return image_token_id_; }
    int global_image_token_id() const { return global_image_token_id_; }
    int eos_token_id() const { return eos_token_id_; }

private:
    std::unordered_map<std::string, mlx::core::array> weights_;
    VisionPsyConfig config_;
    bool is_loaded_ = false;
    std::string last_error_;

    int image_token_id_ = 49152;        // <|image|>
    int global_image_token_id_ = 49153; // <|global_image|>
    int eos_token_id_ = 2;              // <|im_end|>

    void* tokenizer_state_ = nullptr;

    bool validate_config(const std::string& model_dir);
    bool validate_checkpoint();
};

} // namespace visionpsy
} // namespace samosa

#endif // SAMOSA_VISIONPSY_MODEL_H
