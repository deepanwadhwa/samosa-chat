#include "vision.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static void smart_resize(int height, int width, int factor, int min_pixels, int max_pixels, int* out_h, int* out_w) {
    if (MAX(height, width) / (float)MIN(height, width) > 200.0f) {
        fprintf(stderr, "Error: Absolute aspect ratio must be smaller than 200\n");
        *out_h = factor;
        *out_w = factor;
        return;
    }
    
    int h_bar = roundf((float)height / factor) * factor;
    int w_bar = roundf((float)width / factor) * factor;
    
    if (h_bar == 0) h_bar = factor;
    if (w_bar == 0) w_bar = factor;
    
    if (h_bar * w_bar > max_pixels) {
        float beta = sqrtf(((float)height * width) / max_pixels);
        h_bar = MAX(factor, floorf((height / beta) / factor) * factor);
        w_bar = MAX(factor, floorf((width / beta) / factor) * factor);
    } else if (h_bar * w_bar < min_pixels) {
        float beta = sqrtf((float)min_pixels / (height * width));
        h_bar = ceilf((height * beta) / factor) * factor;
        w_bar = ceilf((width * beta) / factor) * factor;
    }
    
    *out_h = h_bar;
    *out_w = w_bar;
}

// Simple bilinear resize for RGB images
static void resize_image_bilinear(const unsigned char* in_pixels, int in_w, int in_h,
                                  unsigned char* out_pixels, int out_w, int out_h) {
    float x_ratio = ((float)(in_w - 1)) / out_w;
    float y_ratio = ((float)(in_h - 1)) / out_h;
    
    for (int i = 0; i < out_h; i++) {
        for (int j = 0; j < out_w; j++) {
            int x_l = floorf(x_ratio * j);
            int y_l = floorf(y_ratio * i);
            int x_h = ceilf(x_ratio * j);
            int y_h = ceilf(y_ratio * i);
            
            float x_weight = (x_ratio * j) - x_l;
            float y_weight = (y_ratio * i) - y_l;
            
            int idx_out = (i * out_w + j) * 3;
            
            for (int c = 0; c < 3; c++) {
                float a = in_pixels[(y_l * in_w + x_l) * 3 + c];
                float b = in_pixels[(y_l * in_w + x_h) * 3 + c];
                float c_val = in_pixels[(y_h * in_w + x_l) * 3 + c];
                float d = in_pixels[(y_h * in_w + x_h) * 3 + c];
                
                float pixel = a * (1 - x_weight) * (1 - y_weight) +
                              b * x_weight * (1 - y_weight) +
                              c_val * (1 - x_weight) * y_weight +
                              d * x_weight * y_weight;
                
                out_pixels[idx_out + c] = (unsigned char)(pixel + 0.5f);
            }
        }
    }
}

static const int b64_index[256] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,62,63,62,62,63,52,53,54,55,56,57,58,59,60,61,0,0,0,0,0,0,
    0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,0,0,0,0,0,
    0,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static unsigned char *base64_decode(const char *data, size_t input_length, size_t *output_length) {
    if (input_length == 0) return NULL;
    
    // Count actual base64 characters
    size_t valid_len = 0;
    for (size_t i = 0; i < input_length; i++) {
        if (data[i] == '=' || b64_index[(unsigned char)data[i]] != 0 || data[i] == 'A') {
            valid_len++;
        }
    }
    if (valid_len == 0) return NULL;
    
    size_t pad = 0;
    if (valid_len % 4 != 0) {
        pad = 4 - (valid_len % 4);
    }
    size_t virtual_len = valid_len + pad;
    *output_length = (virtual_len / 4) * 3;
    
    // Deduct padding
    if (valid_len > 0 && data[input_length - 1] == '=') (*output_length)--;
    if (valid_len > 1 && data[input_length - 2] == '=') (*output_length)--;
    if (pad == 1) (*output_length)--;
    if (pad == 2) (*output_length) -= 2;
    
    unsigned char *decoded = malloc(*output_length);
    if (!decoded) return NULL;
    
    uint32_t sextet[4] = {0};
    size_t out_idx = 0;
    int s_idx = 0;
    
    for (size_t i = 0; i < input_length; i++) {
        unsigned char c = data[i];
        if (c == '=' || b64_index[c] != 0 || c == 'A') {
            sextet[s_idx++] = (c == '=') ? 0 : b64_index[c];
            if (s_idx == 4) {
                uint32_t triple = (sextet[0] << 18) + (sextet[1] << 12) + (sextet[2] << 6) + sextet[3];
                if (out_idx < *output_length) decoded[out_idx++] = (triple >> 16) & 0xFF;
                if (out_idx < *output_length) decoded[out_idx++] = (triple >> 8) & 0xFF;
                if (out_idx < *output_length) decoded[out_idx++] = triple & 0xFF;
                s_idx = 0;
            }
        }
    }
    if (s_idx > 0) {
        // Pad the rest with 0
        while (s_idx < 4) sextet[s_idx++] = 0;
        uint32_t triple = (sextet[0] << 18) + (sextet[1] << 12) + (sextet[2] << 6) + sextet[3];
        if (out_idx < *output_length) decoded[out_idx++] = (triple >> 16) & 0xFF;
        if (out_idx < *output_length) decoded[out_idx++] = (triple >> 8) & 0xFF;
        if (out_idx < *output_length) decoded[out_idx++] = triple & 0xFF;
    }
    
    return decoded;
}

float* vision_load_and_preprocess_image(const unsigned char* image_data, int image_size, 
                                        int* grid_t, int* grid_h, int* grid_w) {
    int w, h, channels;
    unsigned char* pixels = stbi_load_from_memory(image_data, image_size, &w, &h, &channels, 3);
    
    if (!pixels) {
        fprintf(stderr, "Error loading image: %s\n", stbi_failure_reason());
        return NULL;
    }
    
    int factor = SPATIAL_MERGE_SIZE; // 32
    int min_pixels = 4 * 256; // as per Qwen2VL defaults, scaled?
    int max_pixels = 768 * 768; // Enforce our cap here
    
    int out_h, out_w;
    smart_resize(h, w, factor, min_pixels, max_pixels, &out_h, &out_w);
    
    unsigned char* resized_pixels = malloc(out_h * out_w * 3);
    if (!resized_pixels) {
        stbi_image_free(pixels);
        return NULL;
    }
    
    resize_image_bilinear(pixels, w, h, resized_pixels, out_w, out_h);
    stbi_image_free(pixels);
    
    *grid_t = 1;
    *grid_h = out_h / PATCH_SIZE;
    *grid_w = out_w / PATCH_SIZE;
    
    int num_patches = (*grid_t) * (*grid_h) * (*grid_w);
    int patch_dim = 3 * 2 * PATCH_SIZE * PATCH_SIZE; // 1536
    
    float* pixel_values = malloc(num_patches * patch_dim * sizeof(float));
    if (!pixel_values) {
        free(resized_pixels);
        return NULL;
    }
    
    // Convert to patches: [num_patches, channels, temporal=2, patch_size, patch_size]
    // The image processor essentially pads temporal=2 by duplicating or just zeroes?
    // Qwen2VL/3VL image processor for single images repeats the image twice along temporal axis?
    // Actually let's just trace what it does. It stacks the image along temporal axis if single.
    // For now, let's replicate the spatial patch twice for temporal=0 and temporal=1
    // The shape is [grid_t*grid_h*grid_w, 3, 2, 16, 16]
    
    int ph = *grid_h;
    int pw = *grid_w;
    int ps = PATCH_SIZE;
    
    for (int y = 0; y < ph; y++) {
        for (int x = 0; x < pw; x++) {
            // Reorder to 2x2 merge blocks
            int y_b = y / MERGE_SIZE;
            int x_b = x / MERGE_SIZE;
            int y_in = y % MERGE_SIZE;
            int x_in = x % MERGE_SIZE;
            int pw_b = pw / MERGE_SIZE;
            
            int patch_idx = (y_b * pw_b + x_b) * (MERGE_SIZE * MERGE_SIZE) + (y_in * MERGE_SIZE + x_in);
            float* patch_out = &pixel_values[patch_idx * patch_dim];
            
            for (int c = 0; c < 3; c++) {
                for (int t = 0; t < 2; t++) {
                    for (int py = 0; py < ps; py++) {
                        for (int px = 0; px < ps; px++) {
                            int src_y = y * ps + py;
                            int src_x = x * ps + px;
                            unsigned char val = resized_pixels[(src_y * out_w + src_x) * 3 + c];
                            // image_mean = 0.5, image_std = 0.5
                            float norm_val = ((float)val / 255.0f - 0.5f) / 0.5f;
                            
                            int out_idx = c * (2 * ps * ps) + t * (ps * ps) + py * ps + px;
                            patch_out[out_idx] = norm_val;
                        }
                    }
                }
            }
        }
    }
    
    free(resized_pixels);
    return pixel_values;
}

float* vision_load_base64(const char* b64_str, int* grid_t, int* grid_h, int* grid_w) {
    const char *comma = strchr(b64_str, ',');
    if (comma) b64_str = comma + 1;
    
    size_t input_len = strlen(b64_str);
    size_t out_len = 0;
    unsigned char *decoded = base64_decode(b64_str, input_len, &out_len);
    if (!decoded) return NULL;
    
    float *pixels = vision_load_and_preprocess_image(decoded, (int)out_len, grid_t, grid_h, grid_w);
    free(decoded);
    return pixels;
}

static void layernorm(float* out, const float* in, const float* weight, const float* bias, int size) {
    float sum = 0.0f;
    for (int i = 0; i < size; i++) sum += in[i];
    float mean = sum / size;
    
    float sqsum = 0.0f;
    for (int i = 0; i < size; i++) {
        float diff = in[i] - mean;
        sqsum += diff * diff;
    }
    float variance = sqsum / size;
    float inv_std = 1.0f / sqrtf(variance + 1e-6f);
    
    for (int i = 0; i < size; i++) {
        out[i] = (in[i] - mean) * inv_std * weight[i] + bias[i];
    }
}

static void gelu_pytorch_tanh(float* out, const float* in, int size) {
    const float SQRT_2_OVER_PI = 0.7978845608f;
    for (int i = 0; i < size; i++) {
        float x = in[i];
        float cube = x * x * x;
        float arg = SQRT_2_OVER_PI * (x + 0.044715f * cube);
        out[i] = 0.5f * x * (1.0f + tanhf(arg));
    }
}

static void vision_gemm_f32(float* out, const float* in, const float* weight, const float* bias, int num_patches, int in_dim, int out_dim) {
    #pragma omp parallel for
    for (int p = 0; p < num_patches; p++) {
        const float* in_p = in + p * in_dim;
        float* out_p = out + p * out_dim;
        for (int o = 0; o < out_dim; o++) {
            float sum = bias ? bias[o] : 0.0f;
            const float* w_row = weight + (int64_t)o * in_dim;
            for (int i = 0; i < in_dim; i++) {
                sum += in_p[i] * w_row[i];
            }
            out_p[o] = sum;
        }
    }
}

static void vision_gemm_int8(float* out, const float* in, const void* weight, const float* scales, const float* bias, int num_patches, int in_dim, int out_dim) {
    const int8_t* w8 = (const int8_t*)weight;
    #pragma omp parallel for
    for (int p = 0; p < num_patches; p++) {
        const float* in_p = in + p * in_dim;
        float* out_p = out + p * out_dim;
        for (int o = 0; o < out_dim; o++) {
            float sum = 0.0f;
            const int8_t* w_row = w8 + (int64_t)o * in_dim;
            for (int i = 0; i < in_dim; i++) {
                sum += in_p[i] * (float)w_row[i];
            }
            out_p[o] = sum * scales[o] + (bias ? bias[o] : 0.0f);
        }
    }
}

static void add_pos_embed(float* hidden, const uint8_t* pos_embed_weight, const float* pos_embed_qs, 
                          int grid_h, int grid_w, int dim) {
    const int8_t* pos8 = (const int8_t*)pos_embed_weight;
    float h_step = (grid_h > 1) ? 47.0f / (grid_h - 1) : 0.0f;
    float w_step = (grid_w > 1) ? 47.0f / (grid_w - 1) : 0.0f;
    
    // We already reordered patches into 2x2 blocks.
    for (int y = 0; y < grid_h; y++) {
        for (int x = 0; x < grid_w; x++) {
            int y_b = y / MERGE_SIZE, x_b = x / MERGE_SIZE;
            int y_in = y % MERGE_SIZE, x_in = x % MERGE_SIZE;
            int pw_b = grid_w / MERGE_SIZE;
            int patch_idx = (y_b * pw_b + x_b) * 4 + (y_in * MERGE_SIZE + x_in);
            
            float h_val = y * h_step;
            float w_val = x * w_step;
            int h_floor = (int)floorf(h_val), w_floor = (int)floorf(w_val);
            int h_ceil = MIN(h_floor + 1, 47), w_ceil = MIN(w_floor + 1, 47);
            float h_frac = h_val - h_floor, w_frac = w_val - w_floor;
            
            int tl = h_floor * 48 + w_floor;
            int tr = h_floor * 48 + w_ceil;
            int bl = h_ceil * 48 + w_floor;
            int br = h_ceil * 48 + w_ceil;
            
            float w_tl = (1 - h_frac) * (1 - w_frac);
            float w_tr = (1 - h_frac) * w_frac;
            float w_bl = h_frac * (1 - w_frac);
            float w_br = h_frac * w_frac;
            
            float* h_out = hidden + patch_idx * dim;
            for (int d = 0; d < dim; d++) {
                float tl_val = pos8[tl * dim + d] * pos_embed_qs[tl];
                float tr_val = pos8[tr * dim + d] * pos_embed_qs[tr];
                float bl_val = pos8[bl * dim + d] * pos_embed_qs[bl];
                float br_val = pos8[br * dim + d] * pos_embed_qs[br];
                
                h_out[d] += (tl_val * w_tl + tr_val * w_tr + bl_val * w_bl + br_val * w_br);
            }
        }
    }
}

static void vision_softmax_head(float* att, int N) {
    float max_val = att[0];
    for (int i = 1; i < N; i++) {
        if (att[i] > max_val) max_val = att[i];
    }
    float sum = 0.0f;
    for (int i = 0; i < N; i++) {
        att[i] = expf(att[i] - max_val);
        sum += att[i];
    }
    float inv_sum = 1.0f / sum;
    for (int i = 0; i < N; i++) {
        att[i] *= inv_sum;
    }
}

static void generate_vision_rope(float* cos_out, float* sin_out, int grid_h, int grid_w, int head_dim) {
    int dim = head_dim / 2; // 36
    int half_dim = dim / 2; // 18
    float theta = 10000.0f;
    float* inv_freq = malloc(half_dim * sizeof(float));
    for (int i = 0; i < half_dim; i++) {
        inv_freq[i] = 1.0f / powf(theta, (float)(2 * i) / dim);
    }
    
    for (int y = 0; y < grid_h; y++) {
        for (int x = 0; x < grid_w; x++) {
            int y_b = y / MERGE_SIZE, x_b = x / MERGE_SIZE;
            int y_in = y % MERGE_SIZE, x_in = x % MERGE_SIZE;
            int pw_b = grid_w / MERGE_SIZE;
            int patch_idx = (y_b * pw_b + x_b) * 4 + (y_in * MERGE_SIZE + x_in);
            
            float* c_out = cos_out + patch_idx * head_dim;
            float* s_out = sin_out + patch_idx * head_dim;
            
            for (int i = 0; i < half_dim; i++) {
                float h_val = y * inv_freq[i];
                float w_val = x * inv_freq[i];
                
                // First 18 are h, next 18 are w
                c_out[i] = cosf(h_val);
                s_out[i] = sinf(h_val);
                
                c_out[half_dim + i] = cosf(w_val);
                s_out[half_dim + i] = sinf(w_val);
            }
            
            // Duplicate to fill 72
            for (int i = 0; i < dim; i++) {
                c_out[dim + i] = c_out[i];
                s_out[dim + i] = s_out[i];
            }
        }
    }
    free(inv_freq);
}

static void apply_vision_rope(float* q, float* k, const float* cos_val, const float* sin_val, int num_heads, int head_dim) {
    for (int h = 0; h < num_heads; h++) {
        for (int i = 0; i < head_dim / 2; i++) {
            float q0 = q[h * head_dim + i];
            float q1 = q[h * head_dim + i + head_dim / 2];
            float k0 = k[h * head_dim + i];
            float k1 = k[h * head_dim + i + head_dim / 2];
            float c = cos_val[i];
            float s = sin_val[i];
            
            q[h * head_dim + i] = q0 * c - q1 * s;
            q[h * head_dim + i + head_dim / 2] = q0 * s + q1 * c;
            k[h * head_dim + i] = k0 * c - k1 * s;
            k[h * head_dim + i + head_dim / 2] = k0 * s + k1 * c;
        }
    }
}

float* vision_forward(VisionTower* vt, const float* pixel_values, int grid_t, int grid_h, int grid_w) {
    int num_patches = grid_t * grid_h * grid_w;
    int dim = VISION_HIDDEN_SIZE;
    
    float* hidden = malloc(num_patches * dim * sizeof(float));
    
    // 1. Patch Embed
    vision_gemm_f32(hidden, pixel_values, vt->patch_embed_weight, vt->patch_embed_bias, num_patches, 3 * 2 * 16 * 16, dim);
    
    // 2. Pos Embed
    if (vt->pos_embed_weight) {
        add_pos_embed(hidden, vt->pos_embed_weight, vt->pos_embed_weight_qs, grid_h, grid_w, dim);
    }
    
    float* cos_map = malloc(num_patches * VISION_HEAD_DIM * sizeof(float));
    float* sin_map = malloc(num_patches * VISION_HEAD_DIM * sizeof(float));
    generate_vision_rope(cos_map, sin_map, grid_h, grid_w, VISION_HEAD_DIM);
    
    float* hidden_tmp = malloc(num_patches * dim * sizeof(float));
    float* qkv = malloc(num_patches * dim * 3 * sizeof(float));
    
    // 3. Blocks
    for (int b = 0; b < VISION_NUM_BLOCKS; b++) {
        // Norm1
        #pragma omp parallel for
        for (int p = 0; p < num_patches; p++) {
            layernorm(hidden_tmp + p * dim, hidden + p * dim, vt->block_norm1_weight[b], vt->block_norm1_bias[b], dim);
        }
        
        // QKV
        vision_gemm_int8(qkv, hidden_tmp, vt->block_attn_qkv_weight[b], vt->block_attn_qkv_weight_qs[b], vt->block_attn_qkv_bias[b], num_patches, dim, dim * 3);
        
        // Self Attention
        // Here we just use standard multi-head self attention.
        // It's a single image, sequence length is `num_patches`.
        // To save memory and do it simply:
        float* attn_out = hidden_tmp; // reuse memory
        
        // Parallelize over heads
        #pragma omp parallel for
        for (int h = 0; h < VISION_NUM_HEADS; h++) {
            float* att = malloc(num_patches * sizeof(float));
            for (int p_q = 0; p_q < num_patches; p_q++) {
                float* q = qkv + p_q * dim * 3 + h * VISION_HEAD_DIM;
                
                // apply RoPE to Q
                float q_rope[VISION_HEAD_DIM];
                for (int i = 0; i < VISION_HEAD_DIM / 2; i++) {
                    float q0 = q[i];
                    float q1 = q[i + VISION_HEAD_DIM / 2];
                    float c = cos_map[p_q * VISION_HEAD_DIM + i];
                    float s = sin_map[p_q * VISION_HEAD_DIM + i];
                    q_rope[i] = q0 * c - q1 * s;
                    q_rope[i + VISION_HEAD_DIM / 2] = q0 * s + q1 * c;
                }
                
                for (int p_k = 0; p_k < num_patches; p_k++) {
                    float* k = qkv + p_k * dim * 3 + dim + h * VISION_HEAD_DIM;
                    
                    // apply RoPE to K
                    float k_rope[VISION_HEAD_DIM];
                    for (int i = 0; i < VISION_HEAD_DIM / 2; i++) {
                        float k0 = k[i];
                        float k1 = k[i + VISION_HEAD_DIM / 2];
                        float c = cos_map[p_k * VISION_HEAD_DIM + i];
                        float s = sin_map[p_k * VISION_HEAD_DIM + i];
                        k_rope[i] = k0 * c - k1 * s;
                        k_rope[i + VISION_HEAD_DIM / 2] = k0 * s + k1 * c;
                    }
                    
                    float dot = 0.0f;
                    for (int i = 0; i < VISION_HEAD_DIM; i++) dot += q_rope[i] * k_rope[i];
                    att[p_k] = dot / sqrtf((float)VISION_HEAD_DIM);
                }
                
                vision_softmax_head(att, num_patches);
                
                float* out = attn_out + p_q * dim + h * VISION_HEAD_DIM;
                for (int i = 0; i < VISION_HEAD_DIM; i++) out[i] = 0.0f;
                
                for (int p_v = 0; p_v < num_patches; p_v++) {
                    float* v = qkv + p_v * dim * 3 + dim * 2 + h * VISION_HEAD_DIM;
                    float a = att[p_v];
                    for (int i = 0; i < VISION_HEAD_DIM; i++) out[i] += a * v[i];
                }
            }
            free(att);
        }
        
        // Proj & Residual
        float* proj_out = malloc(num_patches * dim * sizeof(float));
        vision_gemm_int8(proj_out, attn_out, vt->block_attn_proj_weight[b], vt->block_attn_proj_weight_qs[b], vt->block_attn_proj_bias[b], num_patches, dim, dim);
        #pragma omp parallel for
        for (int p = 0; p < num_patches * dim; p++) hidden[p] += proj_out[p];
        free(proj_out);
        
        // Norm2
        #pragma omp parallel for
        for (int p = 0; p < num_patches; p++) {
            layernorm(hidden_tmp + p * dim, hidden + p * dim, vt->block_norm2_weight[b], vt->block_norm2_bias[b], dim);
        }
        
        // MLP FC1 -> GELU -> FC2 & Residual
        float* mlp_hidden = malloc(num_patches * VISION_INTERMEDIATE_SIZE * sizeof(float));
        vision_gemm_int8(mlp_hidden, hidden_tmp, vt->block_mlp_fc1_weight[b], vt->block_mlp_fc1_weight_qs[b], vt->block_mlp_fc1_bias[b], num_patches, dim, VISION_INTERMEDIATE_SIZE);
        
        #pragma omp parallel for
        for (int p = 0; p < num_patches; p++) {
            gelu_pytorch_tanh(mlp_hidden + p * VISION_INTERMEDIATE_SIZE, mlp_hidden + p * VISION_INTERMEDIATE_SIZE, VISION_INTERMEDIATE_SIZE);
        }
        
        float* mlp_out = malloc(num_patches * dim * sizeof(float));
        vision_gemm_int8(mlp_out, mlp_hidden, vt->block_mlp_fc2_weight[b], vt->block_mlp_fc2_weight_qs[b], vt->block_mlp_fc2_bias[b], num_patches, VISION_INTERMEDIATE_SIZE, dim);
        
        #pragma omp parallel for
        for (int p = 0; p < num_patches * dim; p++) hidden[p] += mlp_out[p];
        
        free(mlp_hidden);
        free(mlp_out);
    }
    
    // 4. Merger
    float* merge_in = malloc(num_patches * dim * sizeof(float));
    #pragma omp parallel for
    for (int p = 0; p < num_patches; p++) {
        layernorm(merge_in + p * dim, hidden + p * dim, vt->merger_norm_weight, vt->merger_norm_bias, dim);
    }
    
    int new_num_patches = num_patches / 4;
    float* merge_flat = malloc(new_num_patches * dim * 4 * sizeof(float));
    #pragma omp parallel for
    for (int p = 0; p < new_num_patches; p++) {
        memcpy(merge_flat + p * dim * 4, merge_in + (p * 4) * dim, dim * 4 * sizeof(float));
    }
    
    float* merge_hid = malloc(new_num_patches * dim * 4 * sizeof(float));
    vision_gemm_int8(merge_hid, merge_flat, vt->merger_fc1_weight, vt->merger_fc1_weight_qs, vt->merger_fc1_bias, new_num_patches, dim * 4, dim * 4);
    
    #pragma omp parallel for
    for (int p = 0; p < new_num_patches; p++) {
        gelu_pytorch_tanh(merge_hid + p * dim * 4, merge_hid + p * dim * 4, dim * 4);
    }
    
    float* merge_out = malloc(new_num_patches * VISION_OUT_DIM * sizeof(float));
    vision_gemm_int8(merge_out, merge_hid, vt->merger_fc2_weight, vt->merger_fc2_weight_qs, vt->merger_fc2_bias, new_num_patches, dim * 4, VISION_OUT_DIM);
    
    free(hidden);
    free(cos_map);
    free(sin_map);
    free(hidden_tmp);
    free(qkv);
    free(merge_in);
    free(merge_flat);
    free(merge_hid);
    
    return merge_out;
}
