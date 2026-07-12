#ifndef COLIBRI_KV_CACHE_H
#define COLIBRI_KV_CACHE_H

/*
 * Versioned GQA KV-cache storage.
 *
 * This module deliberately has no dependency on the inference engine.  It is
 * the precision/storage core that can be tested before changing model output.
 * A cache contains only full-attention (GQA) layers; recurrent/DeltaNet state
 * is outside its scope and remains f32.
 *
 * INT8 rows use symmetric per-token/per-head quantization.  Their byte layout
 * is exactly
 *
 *     [IEEE-754 f32 scale][head_dim signed int8 values]
 *
 * with no padding.  Values are in [-127, 127], q=-128 is never emitted, and
 * dequantization is q*scale with f32 accumulation.  An all-zero row has scale
 * zero.  Non-finite input is rejected before either K or V is modified.
 * Finite values have a scalar reconstruction error bounded by scale/2, apart
 * from the final f32 rounding.  This bound is local; model perplexity and
 * downstream quality still require the integration gates in tasks.md.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KV_CACHE_OK = 0,
    KV_CACHE_INVALID_ARGUMENT,
    KV_CACHE_OVERFLOW,
    KV_CACHE_NO_MEMORY,
    KV_CACHE_BOUNDS,
    KV_CACHE_BAD_STATE,
    KV_CACHE_NONFINITE,
    KV_CACHE_BAD_FORMAT,
    KV_CACHE_CORRUPT,
    KV_CACHE_INCOMPATIBLE
} kv_cache_status;

typedef enum {
    KV_CACHE_F32 = 1,
    KV_CACHE_I8 = 2
} kv_cache_encoding;

typedef enum {
    KV_CACHE_KEY = 0,
    KV_CACHE_VALUE = 1
} kv_cache_kind;

typedef struct {
    uint32_t layers;          /* Number of GQA/full-attention layers. */
    uint32_t kv_heads;        /* Key/value heads per GQA layer. */
    uint32_t head_dim;
    uint64_t capacity_tokens; /* Exact, fixed token capacity per layer. */
    kv_cache_encoding key_encoding;
    kv_cache_encoding value_encoding;
} kv_cache_geometry;

typedef struct kv_cache kv_cache;

const char *kv_cache_status_string(kv_cache_status status);

/* Checked sizing helpers; no allocation is performed. */
kv_cache_status kv_cache_row_bytes(kv_cache_encoding encoding,
                                   uint32_t head_dim,
                                   size_t *bytes_out);
kv_cache_status kv_cache_geometry_storage_bytes(const kv_cache_geometry *geometry,
                                                size_t *bytes_out);

/* Allocates exactly geometry_storage_bytes() bytes for K/V row storage. */
kv_cache *kv_cache_create(const kv_cache_geometry *geometry,
                          kv_cache_status *status_out);
void kv_cache_destroy(kv_cache *cache);
void kv_cache_clear(kv_cache *cache);

const kv_cache_geometry *kv_cache_get_geometry(const kv_cache *cache);
size_t kv_cache_storage_bytes(const kv_cache *cache);
uint64_t kv_cache_layer_tokens(const kv_cache *cache, uint32_t layer);

/*
 * Append one K/V head pair.  Calls for a layer must be ordered by token, then
 * head: (token=0, head=0..H-1), (token=1, head=0..H-1), ... .  A token becomes
 * readable only after its final head is committed.  This prevents partially
 * written tokens from entering attention or persistence.
 */
kv_cache_status kv_cache_append_head(kv_cache *cache,
                                     uint32_t layer,
                                     uint64_t token,
                                     uint32_t head,
                                     const float *key,
                                     const float *value);

/* Read/dequantize a committed row into caller-owned f32 vectors. */
kv_cache_status kv_cache_read_head(const kv_cache *cache,
                                   uint32_t layer,
                                   uint64_t token,
                                   uint32_t head,
                                   float *key_out,
                                   float *value_out);

/* Return the stored row scale (1.0 for an f32 row). */
kv_cache_status kv_cache_get_scale(const kv_cache *cache,
                                   kv_cache_kind kind,
                                   uint32_t layer,
                                   uint64_t token,
                                   uint32_t head,
                                   float *scale_out);

/* Attention primitives.  Both paths accumulate in f32. */
kv_cache_status kv_cache_dot_key(const kv_cache *cache,
                                 uint32_t layer,
                                 uint64_t token,
                                 uint32_t head,
                                 const float *query,
                                 float *dot_out);
kv_cache_status kv_cache_accumulate_value(const kv_cache *cache,
                                          uint32_t layer,
                                          uint64_t token,
                                          uint32_t head,
                                          float weight,
                                          float *accumulator);

/*
 * Portable persistence header.  Payloads are canonical rows ordered by
 * layer, token, head, with each K row immediately followed by its V row.
 * Header and f32 payload words are little-endian IEEE-754.  Payload encoding
 * itself is intentionally left to the eventual engine integration.
 *
 * Format v1 accepts only f32 K + f32 V.  Format v2 accepts f32 or int8 for K
 * and V independently, making mixed precision explicit rather than inferred.
 */
#define KV_CACHE_FORMAT_V1 1u
#define KV_CACHE_FORMAT_V2 2u
#define KV_CACHE_FORMAT_CURRENT KV_CACHE_FORMAT_V2
#define KV_CACHE_HEADER_BYTES 128u
#define KV_CACHE_MODEL_ID_BYTES 32u

typedef struct {
    uint16_t format_version;
    kv_cache_geometry geometry;
    uint64_t committed_tokens;
    uint8_t model_id[KV_CACHE_MODEL_ID_BYTES];
    uint32_t payload_crc32c;
} kv_cache_persist_info;

kv_cache_status kv_cache_persist_payload_bytes(const kv_cache_geometry *geometry,
                                               uint64_t committed_tokens,
                                               uint64_t *bytes_out);

kv_cache_status kv_cache_header_encode(const kv_cache_persist_info *info,
                                       uint8_t out[KV_CACHE_HEADER_BYTES]);

/*
 * expected_geometry and expected_model_id are optional.  When present they
 * must match exactly, including capacity and independent K/V encodings.
 */
kv_cache_status kv_cache_header_decode(
    const uint8_t *header,
    size_t header_bytes,
    const kv_cache_geometry *expected_geometry,
    const uint8_t expected_model_id[KV_CACHE_MODEL_ID_BYTES],
    kv_cache_persist_info *info_out);

kv_cache_status kv_cache_validate_payload(const kv_cache_persist_info *info,
                                          const void *payload,
                                          size_t payload_bytes);

/* Castagnoli CRC-32C used by both the header and payload. */
uint32_t kv_cache_crc32c(const void *data, size_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* COLIBRI_KV_CACHE_H */
