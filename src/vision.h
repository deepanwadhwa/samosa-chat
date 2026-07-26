#ifndef VISION_H
#define VISION_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#define PATCH_SIZE 16
#define MERGE_SIZE 2
#define SPATIAL_MERGE_SIZE (PATCH_SIZE * MERGE_SIZE)
#define VISION_HIDDEN_SIZE 1152
#define VISION_INTERMEDIATE_SIZE 4304
#define VISION_NUM_HEADS 16
#define VISION_HEAD_DIM (VISION_HIDDEN_SIZE / VISION_NUM_HEADS)
#define VISION_NUM_BLOCKS 27
#define VISION_OUT_DIM 2048

// Tensors required for the vision tower (all point directly into resident.safetensors)
typedef struct {
    // Patch Embedding
    float* patch_embed_weight; // [VISION_HIDDEN_SIZE, 3, 2, 16, 16] - note: float32
    float* patch_embed_bias;   // [VISION_HIDDEN_SIZE] - float32
    
    // Position Embedding
    uint8_t* pos_embed_weight; // [2304, VISION_HIDDEN_SIZE] - int8
    float* pos_embed_weight_qs; // [2304] - int8 scales
    
    // Blocks
    float* block_norm1_weight[VISION_NUM_BLOCKS]; // [VISION_HIDDEN_SIZE] - float32
    float* block_norm1_bias[VISION_NUM_BLOCKS];   // [VISION_HIDDEN_SIZE] - float32
    
    float* block_norm2_weight[VISION_NUM_BLOCKS]; // [VISION_HIDDEN_SIZE] - float32
    float* block_norm2_bias[VISION_NUM_BLOCKS];   // [VISION_HIDDEN_SIZE] - float32
    
    uint8_t* block_attn_qkv_weight[VISION_NUM_BLOCKS]; // [VISION_HIDDEN_SIZE*3, VISION_HIDDEN_SIZE] - int8
    float* block_attn_qkv_weight_qs[VISION_NUM_BLOCKS];
    float* block_attn_qkv_bias[VISION_NUM_BLOCKS];
    
    uint8_t* block_attn_proj_weight[VISION_NUM_BLOCKS]; // [VISION_HIDDEN_SIZE, VISION_HIDDEN_SIZE] - int8
    float* block_attn_proj_weight_qs[VISION_NUM_BLOCKS];
    float* block_attn_proj_bias[VISION_NUM_BLOCKS];
    
    uint8_t* block_mlp_fc1_weight[VISION_NUM_BLOCKS]; // [VISION_INTERMEDIATE_SIZE, VISION_HIDDEN_SIZE] - int8
    float* block_mlp_fc1_weight_qs[VISION_NUM_BLOCKS];
    float* block_mlp_fc1_bias[VISION_NUM_BLOCKS];
    
    uint8_t* block_mlp_fc2_weight[VISION_NUM_BLOCKS]; // [VISION_HIDDEN_SIZE, VISION_INTERMEDIATE_SIZE] - int8
    float* block_mlp_fc2_weight_qs[VISION_NUM_BLOCKS];
    float* block_mlp_fc2_bias[VISION_NUM_BLOCKS];
    
    // Merger
    float* merger_norm_weight; // [VISION_HIDDEN_SIZE] - float32
    float* merger_norm_bias;   // [VISION_HIDDEN_SIZE] - float32
    
    uint8_t* merger_fc1_weight; // [VISION_HIDDEN_SIZE*4, VISION_HIDDEN_SIZE*4] - int8 (in: 4608, out: 4608)
    float* merger_fc1_weight_qs;
    float* merger_fc1_bias;
    
    uint8_t* merger_fc2_weight; // [VISION_OUT_DIM, VISION_HIDDEN_SIZE*4] - int8
    float* merger_fc2_weight_qs;
    float* merger_fc2_bias;
    
    bool is_loaded;
} VisionTower;

// Load an image from memory, resize it, and preprocess it to RGB patch values.
// Returns a dynamically allocated array of size grid_t * grid_h * grid_w * (3 * 2 * 16 * 16) floats.
// Sets grid_t, grid_h, grid_w accordingly.
float* vision_load_and_preprocess_image(const unsigned char* image_data, int image_size, 
                                        int* grid_t, int* grid_h, int* grid_w);

float* vision_load_base64(const char* b64_str, int* grid_t, int* grid_h, int* grid_w);

// Forward pass the vision tower on preprocessed patches.
// Returns dynamically allocated embeddings of size (grid_t * grid_h * grid_w / 4) * VISION_OUT_DIM.
// The returned number of tokens is (grid_t * grid_h * grid_w / 4).
float* vision_forward(VisionTower* vt, const float* pixel_values, int grid_t, int grid_h, int grid_w,
                      const atomic_int* cancel_flag);

#endif
