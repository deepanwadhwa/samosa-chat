#include "visionpsy_model.h"
#include "mlx/fast.h"
#include "mlx/utils.h"

#define STB_IMAGE_IMPLEMENTATION
#include "../stb_image.h"
#include "../tok.h"

#include <iostream>
#include <fstream>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace samosa {
namespace visionpsy {

using namespace mlx::core;

static array layer_norm_f32(const array& x, const array& weight, const array& bias, float eps) {
    auto mean_val = mean(x, -1, true);
    auto var_val = var(x, -1, true);
    auto normalized = (x - mean_val) / sqrt(var_val + eps);
    return normalized * weight + bias;
}

static array rms_norm_f32(const array& x, const array& weight, float eps) {
    auto x_f32 = astype(x, float32);
    auto w_f32 = astype(weight, float32);
    auto norm = fast::rms_norm(x_f32, w_f32, eps);
    return astype(norm, x.dtype());
}

static array gelu_approx(const array& x) {
    float sqrt_2_over_pi = 0.7978845608f;
    auto x_cubed = x * x * x;
    auto inner = sqrt_2_over_pi * (x + 0.044715f * x_cubed);
    return 0.5f * x * (1.0f + tanh(inner));
}

static float cubic_weight(float x) {
    // PIL's BICUBIC path uses a=-0.5 (Keys/Catmull-Rom cubic convolution).
    constexpr float a = -0.5f;
    x = std::fabs(x);
    if (x < 1.0f) return (a + 2.0f) * x * x * x - (a + 3.0f) * x * x + 1.0f;
    if (x < 2.0f) return a * x * x * x - 5.0f * a * x * x + 8.0f * a * x - 4.0f * a;
    return 0.0f;
}

// Standard VisionPsy preprocessing uses bicubic for the high-resolution
// canvas. Values remain in [0,1]; there is no mean/std or [-1,1] transform.
static std::vector<float> resize_bicubic_rgb(const unsigned char* src, int src_w, int src_h,
                                             int dst_w, int dst_h) {
    std::vector<float> dst(dst_w * dst_h * 3);
    float x_ratio = (float)src_w / (float)dst_w;
    float y_ratio = (float)src_h / (float)dst_h;

    for (int dy = 0; dy < dst_h; ++dy) {
        float src_y = (dy + 0.5f) * y_ratio - 0.5f;
        int y_base = (int)std::floor(src_y);

        for (int dx = 0; dx < dst_w; ++dx) {
            float src_x = (dx + 0.5f) * x_ratio - 0.5f;
            int x_base = (int)std::floor(src_x);

            for (int c = 0; c < 3; ++c) {
                float value = 0.0f, total = 0.0f;
                for (int ky = -1; ky <= 2; ++ky) {
                    int sy = std::clamp(y_base + ky, 0, src_h - 1);
                    float wy = cubic_weight(src_y - (float)(y_base + ky));
                    for (int kx = -1; kx <= 2; ++kx) {
                        int sx = std::clamp(x_base + kx, 0, src_w - 1);
                        float w = wy * cubic_weight(src_x - (float)(x_base + kx));
                        value += w * (float)src[(sy * src_w + sx) * 3 + c];
                        total += w;
                    }
                }
                if (total != 0.0f) value /= total;
                dst[(dy * dst_w + dx) * 3 + c] = std::clamp(value, 0.0f, 255.0f) / 255.0f;
            }
        }
    }
    return dst;
}

// The global overview tile is bilinear in the reference processor, and is
// produced from the already-bicubic high-resolution canvas.
static std::vector<float> resize_bilinear_float_rgb(const std::vector<float>& src,
                                                    int src_w, int src_h,
                                                    int dst_w, int dst_h) {
    std::vector<float> dst((size_t)dst_w * dst_h * 3);
    const float xr = (float)src_w / (float)dst_w;
    const float yr = (float)src_h / (float)dst_h;
    for (int y = 0; y < dst_h; ++y) {
        float fy = (y + 0.5f) * yr - 0.5f;
        int y0 = std::clamp((int)std::floor(fy), 0, src_h - 1);
        int y1 = std::min(src_h - 1, y0 + 1);
        float wy = std::clamp(fy - std::floor(fy), 0.0f, 1.0f);
        for (int x = 0; x < dst_w; ++x) {
            float fx = (x + 0.5f) * xr - 0.5f;
            int x0 = std::clamp((int)std::floor(fx), 0, src_w - 1);
            int x1 = std::min(src_w - 1, x0 + 1);
            float wx = std::clamp(fx - std::floor(fx), 0.0f, 1.0f);
            for (int c = 0; c < 3; ++c) {
                float a = src[((size_t)y0 * src_w + x0) * 3 + c];
                float b = src[((size_t)y0 * src_w + x1) * 3 + c];
                float d = src[((size_t)y1 * src_w + x0) * 3 + c];
                float e = src[((size_t)y1 * src_w + x1) * 3 + c];
                dst[((size_t)y * dst_w + x) * 3 + c] =
                    (a + wx * (b - a)) + wy * ((d + wx * (e - d)) - (a + wx * (b - a)));
            }
        }
    }
    return dst;
}

// Aspect-preserving DynamicResize computation
static void compute_dynamic_resize(int w, int h, int max_side_len,
                                   int* out_w, int* out_h, int* out_nw, int* out_nh) {
    const int p = 512;
    int m = max_side_len;
    if (m < 512) m = 512;

    int long_side = (w >= h) ? w : h;
    int short_side = (w >= h) ? h : w;

    int target_long = m;
    double scale = (double)target_long / (double)long_side;
    int target_short = (int)(std::ceil((short_side * scale) / (double)p) * (double)p);
    if (target_short < p) target_short = p;
    if (target_short > m) target_short = m;

    int new_w = (w >= h) ? target_long : target_short;
    int new_h = (w >= h) ? target_short : target_long;

    *out_w = new_w;
    *out_h = new_h;
    *out_nw = new_w / p;
    *out_nh = new_h / p;
}

VisionPsyModel::VisionPsyModel() = default;

VisionPsyModel::~VisionPsyModel() {
    if (tokenizer_state_) {
        Tok* t = static_cast<Tok*>(tokenizer_state_);
        tok_free(t);
        delete t;
        tokenizer_state_ = nullptr;
    }
}

static bool array_shape_is(const array& value, std::initializer_list<int> expected) {
    if (value.ndim() != expected.size()) return false;
    int i = 0;
    for (int dim : expected) if (value.shape(i++) != dim) return false;
    return true;
}

bool VisionPsyModel::validate_config(const std::string& model_dir) {
    std::ifstream in(model_dir + "/config.json", std::ios::binary);
    if (!in) { last_error_ = "missing config.json"; return false; }
    std::ostringstream contents; contents << in.rdbuf();
    std::string bytes = contents.str();
    char* arena = nullptr;
    jval* root = json_parse(bytes.c_str(), &arena);
    if (!root || root->t != J_OBJ) {
        if (root) json_free(root); free(arena);
        last_error_ = "invalid config.json";
        return false;
    }
    auto number_is = [](jval* obj, const char* key, int expected) {
        jval* v = obj && obj->t == J_OBJ ? json_get(obj, key) : nullptr;
        return v && v->t == J_NUM && v->num == expected;
    };
    jval* model_type = json_get(root, "model_type");
    jval* flash = json_get(root, "is_flash");
    jval* text = json_get(root, "text_config");
    jval* vision = json_get(root, "vision_config");
    bool ok = model_type && model_type->t == J_STR && !strcmp(model_type->str, "visionpsy_nano") &&
              flash && flash->t == J_BOOL && !flash->boolean &&
              number_is(root, "mp_image_token_length", 64) &&
              number_is(root, "mp_pixel_shuffle_factor", 4) &&
              number_is(text, "hidden_size", 960) && number_is(text, "intermediate_size", 2560) &&
              number_is(text, "num_hidden_layers", 32) && number_is(text, "num_attention_heads", 15) &&
              number_is(text, "num_key_value_heads", 5) && number_is(text, "vocab_size", 49218) &&
              number_is(vision, "hidden_size", 768) && number_is(vision, "intermediate_size", 3072) &&
              number_is(vision, "num_hidden_layers", 12) && number_is(vision, "num_attention_heads", 12) &&
              number_is(vision, "image_size", 512) && number_is(vision, "patch_size", 16) &&
              number_is(vision, "max_img_size", 2048);
    json_free(root); free(arena);
    if (!ok) last_error_ = "unsupported VisionPsy config; expected pinned Standard 460M architecture";
    return ok;
}

bool VisionPsyModel::validate_checkpoint() {
    auto require = [&](const std::string& name, std::initializer_list<int> shape) {
        auto it = weights_.find(name);
        if (it == weights_.end()) { last_error_ = "checkpoint tensor missing: " + name; return false; }
        if (!array_shape_is(it->second, shape)) { last_error_ = "checkpoint tensor has wrong shape: " + name; return false; }
        return true;
    };
    if (!require("vision_tower.patch_embedding.conv.weight", {768, 3, 16, 16}) ||
        !require("vision_tower.patch_embedding.conv.bias", {768}) ||
        !require("vision_tower.patch_embedding.position_embedding", {1, 1024, 768}) ||
        !require("vision_tower.layer_norm.weight", {768}) ||
        !require("vision_tower.layer_norm.bias", {768}) ||
        !require("multi_modal_projector.proj.weight", {960, 12288}) ||
        !require("language_model.token_embedding.weight", {49218, 960}) ||
        !require("language_model.norm.weight", {960}) ||
        !require("language_model.head.weight", {49218, 960})) return false;
    for (int l = 0; l < config_.vit_num_hidden_layers; ++l) {
        std::string p = "vision_tower.blocks." + std::to_string(l) + ".";
        if (!require(p + "ln1.weight", {768}) || !require(p + "ln1.bias", {768}) ||
            !require(p + "attn.qkv_proj.weight", {2304, 768}) || !require(p + "attn.qkv_proj.bias", {2304}) ||
            !require(p + "attn.out_proj.weight", {768, 768}) || !require(p + "attn.out_proj.bias", {768}) ||
            !require(p + "ln2.weight", {768}) || !require(p + "ln2.bias", {768}) ||
            !require(p + "mlp.fc1.weight", {3072, 768}) || !require(p + "mlp.fc1.bias", {3072}) ||
            !require(p + "mlp.fc2.weight", {768, 3072}) || !require(p + "mlp.fc2.bias", {768})) return false;
    }
    for (int l = 0; l < config_.num_hidden_layers; ++l) {
        std::string p = "language_model.blocks." + std::to_string(l) + ".";
        if (!require(p + "norm1.weight", {960}) || !require(p + "attn.q_proj.weight", {960, 960}) ||
            !require(p + "attn.k_proj.weight", {320, 960}) || !require(p + "attn.v_proj.weight", {320, 960}) ||
            !require(p + "attn.out_proj.weight", {960, 960}) || !require(p + "norm2.weight", {960}) ||
            !require(p + "mlp.gate_up_proj.weight", {5120, 960}) ||
            !require(p + "mlp.down_proj.weight", {960, 2560})) return false;
    }
    return true;
}

bool VisionPsyModel::load(const std::string& model_dir) {
    last_error_.clear();
    is_loaded_ = false;
    try {
        if (!validate_config(model_dir)) return false;
        std::string weights_path = model_dir + "/model.safetensors";
        auto loaded = load_safetensors(weights_path).first;
        weights_ = std::move(loaded);
        if (!validate_checkpoint()) { weights_.clear(); return false; }

        std::string tok_path = model_dir + "/tokenizer.json";
        Tok* t = new Tok();
        tok_load(t, tok_path.c_str());
        tokenizer_state_ = t;
        is_loaded_ = true;
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        weights_.clear();
        std::cerr << "Failed to load VisionPsy model: " << last_error_ << std::endl;
        return false;
    }
}

bool VisionPsyModel::inspect_image_header(const unsigned char* data, size_t len, int* out_w, int* out_h) const {
    if (!data || len == 0) return false;
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(data, (int)len, &w, &h, &comp)) return false;
    if (w <= 0 || h <= 0) return false;
    // Bound decoded allocation, not model resolution. Large but legitimate
    // camera/scanner images are decoded and reduced to the adaptive tile budget.
    if (w > 32768 || h > 32768 || ((uint64_t)w * (uint64_t)h) > 134217728ULL) return false;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return true;
}

bool VisionPsyModel::preprocess_image_tiles(const unsigned char* data, size_t len,
                                           int max_side_len, ImageTiles* out_tiles) const {
    if (!out_tiles) return false;
    int orig_w = 0, orig_h = 0, comp = 0;
    if (!inspect_image_header(data, len, &orig_w, &orig_h)) return false;

    unsigned char* rgb = stbi_load_from_memory(data, (int)len, &orig_w, &orig_h, &comp, 3);
    if (!rgb) return false;

    int new_w = 512, new_h = 512, n_w = 1, n_h = 1;
    max_side_len = std::clamp(max_side_len, 512, config_.max_img_size);
    // The model prompt only defines row/column tokens through 8x8.
    compute_dynamic_resize(orig_w, orig_h, max_side_len, &new_w, &new_h, &n_w, &n_h);

    out_tiles->orig_w = orig_w;
    out_tiles->orig_h = orig_h;
    out_tiles->n_w = n_w;
    out_tiles->n_h = n_h;
    out_tiles->tiles.clear();

    const int p = 512;

    if (n_w == 1 && n_h == 1) {
        // Single 512x512 tile
        auto normalized_pixels = resize_bicubic_rgb(rgb, orig_w, orig_h, p, p);
        stbi_image_free(rgb);
        array tile_arr(normalized_pixels.data(), {1, p, p, 3}, float32);
        out_tiles->tiles.push_back(tile_arr);
        out_tiles->has_global = false;
        return true;
    }

    // Multi-tile: 1 global overview tile + (n_h * n_w) spatial tiles
    out_tiles->has_global = true;

    // High-resolution canvas: bicubic, as in the pinned processor.
    auto canvas = resize_bicubic_rgb(rgb, orig_w, orig_h, new_w, new_h);
    stbi_image_free(rgb);

    // Global overview: bilinear from the resized canvas.
    auto global_pixels = resize_bilinear_float_rgb(canvas, new_w, new_h, p, p);
    array global_arr(global_pixels.data(), {1, p, p, 3}, float32);
    out_tiles->tiles.push_back(global_arr);

    // 3. Extract spatial tiles (512x512 each)
    for (int r = 0; r < n_h; ++r) {
        for (int c = 0; c < n_w; ++c) {
            std::vector<float> tile_buf(p * p * 3);
            int start_y = r * p;
            int start_x = c * p;
            for (int y = 0; y < p; ++y) {
                int src_y = start_y + y;
                for (int x = 0; x < p; ++x) {
                    int src_x = start_x + x;
                    int src_idx = (src_y * new_w + src_x) * 3;
                    int dst_idx = (y * p + x) * 3;
                    tile_buf[dst_idx] = canvas[src_idx];
                    tile_buf[dst_idx + 1] = canvas[src_idx + 1];
                    tile_buf[dst_idx + 2] = canvas[src_idx + 2];
                }
            }
            array tile_arr(tile_buf.data(), {1, p, p, 3}, float32);
            out_tiles->tiles.push_back(tile_arr);
        }
    }
    return true;
}

mlx::core::array VisionPsyModel::encode_tile(const mlx::core::array& image_pixels) {
    // SigLIP2 encoder. The pinned checkpoint keeps the convolution in PyTorch
    // OIHW layout; transpose to MLX/matrix-friendly OHWI before flattening.
    auto patch_weight = transpose(weights_.at("vision_tower.patch_embedding.conv.weight"), {0, 2, 3, 1});
    patch_weight = reshape(patch_weight, {config_.vit_hidden_size, -1});
    auto patch_bias = weights_.at("vision_tower.patch_embedding.conv.bias");
    int P = config_.vit_patch_size; // 16
    int H_p = config_.vit_image_size / P; // 32
    int W_p = config_.vit_image_size / P; // 32
    int D_v = config_.vit_hidden_size;    // 768

    auto patches = reshape(image_pixels, {1, H_p, P, W_p, P, 3});
    patches = transpose(patches, {0, 1, 3, 2, 4, 5});
    patches = reshape(patches, {1, H_p * W_p, P * P * 3});

    auto x = matmul(patches, transpose(patch_weight, {1, 0})) + patch_bias;
    auto pos_embed = weights_.at("vision_tower.patch_embedding.position_embedding");
    x = x + pos_embed;

    int num_heads = config_.vit_num_attention_heads;
    int head_dim = config_.vit_head_dim;
    float scale = 1.0f / std::sqrt((float)head_dim);

    for (int l = 0; l < config_.vit_num_hidden_layers; ++l) {
        std::string pfx = "vision_tower.blocks." + std::to_string(l) + ".";

        auto norm1_w = weights_.at(pfx + "ln1.weight");
        auto norm1_b = weights_.at(pfx + "ln1.bias");
        auto norm1 = layer_norm_f32(x, norm1_w, norm1_b, config_.vit_layer_norm_eps);

        auto qkv = matmul(norm1, transpose(weights_.at(pfx + "attn.qkv_proj.weight"), {1, 0})) +
                   weights_.at(pfx + "attn.qkv_proj.bias");
        auto q = slice(qkv, {0, 0, 0}, {1, 1024, D_v});
        auto k = slice(qkv, {0, 0, D_v}, {1, 1024, 2 * D_v});
        auto v = slice(qkv, {0, 0, 2 * D_v}, {1, 1024, 3 * D_v});

        q = reshape(q, {1, 1024, num_heads, head_dim});
        k = reshape(k, {1, 1024, num_heads, head_dim});
        v = reshape(v, {1, 1024, num_heads, head_dim});

        q = transpose(q, {0, 2, 1, 3});
        k = transpose(k, {0, 2, 1, 3});
        v = transpose(v, {0, 2, 1, 3});

        auto attn_out = fast::scaled_dot_product_attention(q, k, v, scale, "");

        attn_out = transpose(attn_out, {0, 2, 1, 3});
        attn_out = reshape(attn_out, {1, 1024, D_v});
        auto attn_proj = matmul(attn_out, transpose(weights_.at(pfx + "attn.out_proj.weight"), {1, 0})) +
                         weights_.at(pfx + "attn.out_proj.bias");
        x = x + attn_proj;

        auto norm2_w = weights_.at(pfx + "ln2.weight");
        auto norm2_b = weights_.at(pfx + "ln2.bias");
        auto norm2 = layer_norm_f32(x, norm2_w, norm2_b, config_.vit_layer_norm_eps);

        auto fc1_w = weights_.at(pfx + "mlp.fc1.weight");
        auto fc1_b = weights_.at(pfx + "mlp.fc1.bias");
        auto fc2_w = weights_.at(pfx + "mlp.fc2.weight");
        auto fc2_b = weights_.at(pfx + "mlp.fc2.bias");

        auto h = matmul(norm2, transpose(fc1_w, {1, 0})) + fc1_b;
        h = gelu_approx(h);
        auto mlp_out = matmul(h, transpose(fc2_w, {1, 0})) + fc2_b;
        x = x + mlp_out;
    }

    x = layer_norm_f32(x, weights_.at("vision_tower.layer_norm.weight"),
                       weights_.at("vision_tower.layer_norm.bias"), config_.vit_layer_norm_eps);

    // Pixel-Shuffle Projector
    int factor = config_.pixel_shuffle_factor; // 4
    int H_feat = 32, W_feat = 32;
    auto feat_grid = reshape(x, {1, H_feat, W_feat, D_v});
    int H_out = H_feat / factor; // 8
    int W_out = W_feat / factor; // 8
    auto shuf = reshape(feat_grid, {1, H_out, factor, W_out, factor, D_v});
    shuf = transpose(shuf, {0, 1, 3, 2, 4, 5});
    auto flattened_tiles = reshape(shuf, {1, H_out * W_out, factor * factor * D_v}); // [1, 64, 12288]

    auto proj_out = matmul(flattened_tiles,
                           transpose(weights_.at("multi_modal_projector.proj.weight"), {1, 0}));

    return reshape(proj_out, {64, config_.hidden_size});
}

std::vector<int> VisionPsyModel::build_prompt_tokens(const std::string& user_prompt,
                                                     const ImageTiles& tiles) const {
    std::string image_string;
    if (tiles.has_global) {
        image_string += "<|global_image|>";
        for (int i = 0; i < config_.mp_image_token_length; ++i) {
            image_string += "<|image|>";
        }
    }
    for (int r = 0; r < tiles.n_h; ++r) {
        for (int c = 0; c < tiles.n_w; ++c) {
            image_string += "<row_" + std::to_string(r + 1) + "_col_" + std::to_string(c + 1) + ">";
            for (int i = 0; i < config_.mp_image_token_length; ++i) {
                image_string += "<|image|>";
            }
        }
    }

    std::string full_prompt = "<|im_start|>user\n" + image_string + "\n" + user_prompt + "<|im_end|>\n<|im_start|>assistant\n";
    return encode_text(full_prompt);
}

std::vector<int> VisionPsyModel::encode_text(const std::string& text) const {
    if (!tokenizer_state_) return {};
    Tok* t = static_cast<Tok*>(tokenizer_state_);
    int capacity = (int)text.size() * 2 + 512;
    std::vector<int> tokens(capacity);
    int num_tokens = tok_encode(t, text.c_str(), (int)text.size(), tokens.data(), capacity);
    if (num_tokens > 0) {
        tokens.resize(num_tokens);
    } else {
        tokens.clear();
    }
    return tokens;
}

std::string VisionPsyModel::decode_tokens(const std::vector<int>& tokens) const {
    if (!tokenizer_state_ || tokens.empty()) return "";
    Tok* t = static_cast<Tok*>(tokenizer_state_);
    size_t out_cap = tokens.size() * 16 + 256;
    std::string out_str(out_cap, '\0');
    size_t out_len = tok_decode(t, tokens.data(), (int)tokens.size(), &out_str[0], (int)out_cap);
    out_str.resize(out_len);
    return out_str;
}

InferenceResult VisionPsyModel::generate_from_tiles(const ImageTiles& tiles,
                                                    const std::string& user_prompt,
                                                    int max_new_tokens,
                                                    const std::atomic<bool>* cancel_flag) {
    InferenceResult res;
    if (!is_loaded_) {
        res.error = "Model is not loaded";
        return res;
    }

    auto prompt_tokens = build_prompt_tokens(user_prompt, tiles);
    if (prompt_tokens.empty()) {
        res.error = "Failed to tokenize prompt";
        return res;
    }
    res.prompt_tokens = (int)prompt_tokens.size();

    auto t_start = std::chrono::steady_clock::now();

    // 1. Text embedding table
    auto wte = weights_.at("language_model.token_embedding.weight"); // [vocab_size, 960]

    // 2. Encode visual tiles incrementally
    std::vector<array> tile_embeds;
    for (size_t i = 0; i < tiles.tiles.size(); ++i) {
        if (cancel_flag && cancel_flag->load()) {
            res.error = "Cancelled";
            return res;
        }
        auto encoded = encode_tile(tiles.tiles[i]);
        eval(encoded); // bound the lazy graph and peak memory to one tile
        tile_embeds.push_back(encoded);
    }

    // 3. Build sequence embeddings replacing <|image|> tokens
    std::vector<float> seq_embeds_buf((size_t)prompt_tokens.size() * (size_t)config_.hidden_size);
    size_t visual_tile_idx = 0;
    size_t visual_token_offset = 0;

    for (size_t i = 0; i < prompt_tokens.size(); ++i) {
        int token_id = prompt_tokens[i];
        if (token_id == image_token_id_ && visual_tile_idx < tile_embeds.size()) {
            auto tile_data = tile_embeds[visual_tile_idx];
            // Copy 960 float values for this image token
            auto row_slice = slice(tile_data, {(int)visual_token_offset, 0}, {(int)visual_token_offset + 1, config_.hidden_size});
            row_slice = astype(row_slice, float32);
            eval(row_slice);
            std::memcpy(&seq_embeds_buf[i * config_.hidden_size], row_slice.data<float>(), config_.hidden_size * sizeof(float));

            visual_token_offset++;
            if (visual_token_offset >= (size_t)config_.mp_image_token_length) {
                visual_token_offset = 0;
                visual_tile_idx++;
            }
        } else {
            auto token_arr = array(&token_id, {1}, int32);
            auto embed = take(wte, token_arr, 0);
            embed = astype(embed, float32);
            eval(embed);
            std::memcpy(&seq_embeds_buf[i * config_.hidden_size], embed.data<float>(), config_.hidden_size * sizeof(float));
        }
    }

    array h = array(seq_embeds_buf.data(), {1, (int)prompt_tokens.size(), config_.hidden_size}, float32);

    KVCache kv_cache;
    int num_q_heads = config_.num_attention_heads;     // 15
    int num_kv_heads = config_.num_key_value_heads;    // 5
    int head_dim = config_.head_dim;                   // 64
    float rope_theta = config_.rope_theta;

    auto forward_layer = [&](int l, const array& x, int offset) -> array {
        std::string pfx = "language_model.blocks." + std::to_string(l) + ".";

        auto input_norm_w = weights_.at(pfx + "norm1.weight");
        auto norm_x = rms_norm_f32(x, input_norm_w, config_.rms_norm_eps);

        auto q_w = weights_.at(pfx + "attn.q_proj.weight");
        auto k_w = weights_.at(pfx + "attn.k_proj.weight");
        auto v_w = weights_.at(pfx + "attn.v_proj.weight");
        auto o_w = weights_.at(pfx + "attn.out_proj.weight");

        auto q = matmul(norm_x, transpose(q_w, {1, 0}));
        auto k = matmul(norm_x, transpose(k_w, {1, 0}));
        auto v = matmul(norm_x, transpose(v_w, {1, 0}));

        int B = x.shape(0);
        int L_seq = x.shape(1);

        q = reshape(q, {B, L_seq, num_q_heads, head_dim});
        k = reshape(k, {B, L_seq, num_kv_heads, head_dim});
        v = reshape(v, {B, L_seq, num_kv_heads, head_dim});

        q = fast::rope(q, head_dim, false, rope_theta, 1.0f, offset);
        k = fast::rope(k, head_dim, false, rope_theta, 1.0f, offset);

        q = transpose(q, {0, 2, 1, 3});
        k = transpose(k, {0, 2, 1, 3});
        v = transpose(v, {0, 2, 1, 3});

        if ((int)kv_cache.k.size() <= l) {
            kv_cache.k.push_back(k);
            kv_cache.v.push_back(v);
        } else {
            k = concatenate({kv_cache.k[l], k}, 2);
            v = concatenate({kv_cache.v[l], v}, 2);
            kv_cache.k[l] = k;
            kv_cache.v[l] = v;
        }

        // Scaled dot-product attention with GQA
        float scale = 1.0f / std::sqrt((float)head_dim);
        auto attn_out = fast::scaled_dot_product_attention(q, k, v, scale, offset == 0 ? "causal" : "");
        attn_out = transpose(attn_out, {0, 2, 1, 3});
        attn_out = reshape(attn_out, {B, L_seq, num_q_heads * head_dim});

        auto attn_proj = matmul(attn_out, transpose(o_w, {1, 0}));
        auto x_post_attn = x + attn_proj;

        auto post_norm_w = weights_.at(pfx + "norm2.weight");
        auto norm_x2 = rms_norm_f32(x_post_attn, post_norm_w, config_.rms_norm_eps);

        auto gate_up_w = weights_.at(pfx + "mlp.gate_up_proj.weight");
        auto down_w = weights_.at(pfx + "mlp.down_proj.weight");

        auto gate_up = matmul(norm_x2, transpose(gate_up_w, {1, 0}));
        auto gate = slice(gate_up, {0, 0, 0}, {B, L_seq, config_.intermediate_size});
        auto up = slice(gate_up, {0, 0, config_.intermediate_size},
                        {B, L_seq, 2 * config_.intermediate_size});
        auto mlp_act = (gate * sigmoid(gate)) * up;
        auto mlp_out = matmul(mlp_act, transpose(down_w, {1, 0}));

        return x_post_attn + mlp_out;
    };

    // Prefill pass
    for (int l = 0; l < config_.num_hidden_layers; ++l) {
        if (cancel_flag && cancel_flag->load()) {
            res.error = "Cancelled";
            return res;
        }
        h = forward_layer(l, h, 0);
    }
    kv_cache.offset = (int)prompt_tokens.size();

    auto norm_final_w = weights_.at("language_model.norm.weight");
    h = rms_norm_f32(h, norm_final_w, config_.rms_norm_eps);

    auto last_h = slice(h, {0, (int)prompt_tokens.size() - 1, 0}, {1, (int)prompt_tokens.size(), config_.hidden_size});
    last_h = reshape(last_h, {1, config_.hidden_size});

    auto lm_head = weights_.at("language_model.head.weight");

    auto logits = matmul(last_h, transpose(lm_head, {1, 0}));
    auto next_token_arr = argmax(logits, -1);
    eval(next_token_arr);
    int next_token = next_token_arr.item<int>();

    auto t_prefill = std::chrono::steady_clock::now();
    res.prefill_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_prefill - t_start).count();

    std::vector<int> gen_tokens;

    // Decode loop
    for (int step = 0; step < max_new_tokens; ++step) {
        if (cancel_flag && cancel_flag->load()) break;
        if (next_token == eos_token_id_) break;

        gen_tokens.push_back(next_token);

        auto tok_arr = array(&next_token, {1}, int32);
        auto tok_embed = take(wte, tok_arr, 0);
        h = reshape(tok_embed, {1, 1, config_.hidden_size});

        for (int l = 0; l < config_.num_hidden_layers; ++l) {
            h = forward_layer(l, h, kv_cache.offset);
        }
        kv_cache.offset++;

        h = rms_norm_f32(h, norm_final_w, config_.rms_norm_eps);
        h = reshape(h, {1, config_.hidden_size});

        logits = matmul(h, transpose(lm_head, {1, 0}));
        next_token_arr = argmax(logits, -1);
        eval(next_token_arr);
        next_token = next_token_arr.item<int>();
    }

    auto t_decode = std::chrono::steady_clock::now();
    res.decode_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_decode - t_prefill).count();
    res.generated_tokens = (int)gen_tokens.size();
    res.text = decode_tokens(gen_tokens);

    return res;
}

} // namespace visionpsy
} // namespace samosa
