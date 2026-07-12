#include "kv_cache.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define KV_CACHE_HAS_NEON 1
#else
#define KV_CACHE_HAS_NEON 0
#endif

_Static_assert(sizeof(float) == 4, "kv_cache requires 32-bit float");
_Static_assert(sizeof(uint32_t) == 4, "kv_cache requires 32-bit uint32_t");

struct kv_cache {
    kv_cache_geometry geometry;
    size_t key_row_bytes;
    size_t value_row_bytes;
    size_t key_storage_bytes;
    size_t value_offset;
    size_t storage_bytes;
    uint8_t *storage;
    uint8_t *keys;
    uint8_t *values;
    uint64_t *layer_tokens;
    uint32_t *next_head;
};

static const uint8_t KV_CACHE_MAGIC[8] = {
    'C', 'O', 'L', 'I', 'K', 'V', 'C', '\0'
};

enum {
    HDR_VERSION = 8,
    HDR_SIZE = 10,
    HDR_FLAGS = 12,
    HDR_LAYERS = 16,
    HDR_HEADS = 20,
    HDR_DIM = 24,
    HDR_KEY_ENCODING = 28,
    HDR_VALUE_ENCODING = 29,
    HDR_SCALE_TYPE = 30,
    HDR_QUANT_SCHEME = 31,
    HDR_CAPACITY = 32,
    HDR_COMMITTED = 40,
    HDR_KEY_ROW_BYTES = 48,
    HDR_VALUE_ROW_BYTES = 56,
    HDR_PAYLOAD_BYTES = 64,
    HDR_MODEL_ID = 72,
    HDR_PAYLOAD_CRC = 104,
    HDR_HEADER_CRC = 108,
    HDR_RESERVED = 112
};

static int encoding_valid(kv_cache_encoding encoding) {
    return encoding == KV_CACHE_F32 || encoding == KV_CACHE_I8;
}

static int mul_size(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) return 0;
    *out = a * b;
    return 1;
}

static int add_size(size_t a, size_t b, size_t *out) {
    if (b > SIZE_MAX - a) return 0;
    *out = a + b;
    return 1;
}

static int align_size(size_t value, size_t alignment, size_t *out) {
    size_t remainder, padding;
    if (alignment == 0) return 0;
    remainder = value % alignment;
    padding = remainder == 0 ? 0 : alignment - remainder;
    return add_size(value, padding, out);
}

static int mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a != 0 && b > UINT64_MAX / a) return 0;
    *out = a * b;
    return 1;
}

static int add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (b > UINT64_MAX - a) return 0;
    *out = a + b;
    return 1;
}

static void put_u16le(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32le(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void put_u64le(uint8_t *p, uint64_t value) {
    unsigned i;
    for (i = 0; i < 8; ++i) p[i] = (uint8_t)(value >> (8u * i));
}

static uint16_t get_u16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32le(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64le(const uint8_t *p) {
    uint64_t value = 0;
    unsigned i;
    for (i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (8u * i);
    return value;
}

const char *kv_cache_status_string(kv_cache_status status) {
    switch (status) {
        case KV_CACHE_OK: return "ok";
        case KV_CACHE_INVALID_ARGUMENT: return "invalid argument";
        case KV_CACHE_OVERFLOW: return "integer overflow";
        case KV_CACHE_NO_MEMORY: return "out of memory";
        case KV_CACHE_BOUNDS: return "index out of bounds";
        case KV_CACHE_BAD_STATE: return "invalid cache state or append order";
        case KV_CACHE_NONFINITE: return "non-finite vector value";
        case KV_CACHE_BAD_FORMAT: return "malformed persistence format";
        case KV_CACHE_CORRUPT: return "corrupt header or payload";
        case KV_CACHE_INCOMPATIBLE: return "incompatible cache version or geometry";
        default: return "unknown kv-cache status";
    }
}

kv_cache_status kv_cache_row_bytes(kv_cache_encoding encoding,
                                   uint32_t head_dim,
                                   size_t *bytes_out) {
    size_t bytes;
    if (bytes_out == NULL || head_dim == 0 || !encoding_valid(encoding))
        return KV_CACHE_INVALID_ARGUMENT;
    if (encoding == KV_CACHE_F32) {
        if (!mul_size((size_t)head_dim, sizeof(float), &bytes))
            return KV_CACHE_OVERFLOW;
    } else {
        if (!add_size(sizeof(float), (size_t)head_dim, &bytes))
            return KV_CACHE_OVERFLOW;
    }
    *bytes_out = bytes;
    return KV_CACHE_OK;
}

kv_cache_status kv_cache_geometry_storage_bytes(const kv_cache_geometry *geometry,
                                                size_t *bytes_out) {
    size_t key_row, value_row, rows, key_bytes, value_bytes, value_offset, bytes;
    kv_cache_status status;
    if (geometry == NULL || bytes_out == NULL || geometry->layers == 0
        || geometry->kv_heads == 0 || geometry->head_dim == 0
        || geometry->capacity_tokens == 0
        || geometry->capacity_tokens > (uint64_t)SIZE_MAX)
        return KV_CACHE_INVALID_ARGUMENT;
    status = kv_cache_row_bytes(geometry->key_encoding, geometry->head_dim,
                                &key_row);
    if (status != KV_CACHE_OK) return status;
    status = kv_cache_row_bytes(geometry->value_encoding, geometry->head_dim,
                                &value_row);
    if (status != KV_CACHE_OK) return status;
    if (!mul_size((size_t)geometry->layers,
                     (size_t)geometry->kv_heads, &rows)
        || !mul_size(rows, (size_t)geometry->capacity_tokens, &rows)
        || !mul_size(rows, key_row, &key_bytes)
        || !mul_size(rows, value_row, &value_bytes)
        || !align_size(key_bytes, _Alignof(float), &value_offset)
        || !add_size(value_offset, value_bytes, &bytes))
        return KV_CACHE_OVERFLOW;
    *bytes_out = bytes;
    return KV_CACHE_OK;
}

kv_cache *kv_cache_create(const kv_cache_geometry *geometry,
                          kv_cache_status *status_out) {
    kv_cache *cache = NULL;
    kv_cache_status status;
    size_t rows, metadata_bytes;

    if (status_out != NULL) *status_out = KV_CACHE_INVALID_ARGUMENT;
    if (geometry == NULL) return NULL;

    cache = (kv_cache *)calloc(1, sizeof(*cache));
    if (cache == NULL) {
        if (status_out != NULL) *status_out = KV_CACHE_NO_MEMORY;
        return NULL;
    }
    status = kv_cache_geometry_storage_bytes(geometry, &cache->storage_bytes);
    if (status != KV_CACHE_OK) goto fail;
    status = kv_cache_row_bytes(geometry->key_encoding, geometry->head_dim,
                                &cache->key_row_bytes);
    if (status != KV_CACHE_OK) goto fail;
    status = kv_cache_row_bytes(geometry->value_encoding, geometry->head_dim,
                                &cache->value_row_bytes);
    if (status != KV_CACHE_OK) goto fail;
    if (!mul_size((size_t)geometry->layers, (size_t)geometry->kv_heads, &rows)
        || !mul_size(rows, (size_t)geometry->capacity_tokens, &rows)
        || !mul_size(rows, cache->key_row_bytes, &cache->key_storage_bytes)
        || !mul_size((size_t)geometry->layers, sizeof(uint64_t),
                     &metadata_bytes)) {
        status = KV_CACHE_OVERFLOW;
        goto fail;
    }

    cache->storage = (uint8_t *)malloc(cache->storage_bytes);
    cache->layer_tokens = (uint64_t *)calloc(1, metadata_bytes);
    cache->next_head = (uint32_t *)calloc((size_t)geometry->layers,
                                          sizeof(*cache->next_head));
    if (cache->storage == NULL || cache->layer_tokens == NULL
        || cache->next_head == NULL) {
        status = KV_CACHE_NO_MEMORY;
        goto fail;
    }
    memset(cache->storage, 0, cache->storage_bytes);
    cache->geometry = *geometry;
    cache->keys = cache->storage;
    if (!align_size(cache->key_storage_bytes, _Alignof(float),
                    &cache->value_offset)) {
        status = KV_CACHE_OVERFLOW;
        goto fail;
    }
    cache->values = cache->storage + cache->value_offset;
    if (status_out != NULL) *status_out = KV_CACHE_OK;
    return cache;

fail:
    if (cache != NULL) {
        free(cache->next_head);
        free(cache->layer_tokens);
        free(cache->storage);
        free(cache);
    }
    if (status_out != NULL) *status_out = status;
    return NULL;
}

void kv_cache_destroy(kv_cache *cache) {
    if (cache == NULL) return;
    free(cache->next_head);
    free(cache->layer_tokens);
    free(cache->storage);
    free(cache);
}

void kv_cache_clear(kv_cache *cache) {
    if (cache == NULL) return;
    memset(cache->storage, 0, cache->storage_bytes);
    memset(cache->layer_tokens, 0,
           (size_t)cache->geometry.layers * sizeof(*cache->layer_tokens));
    memset(cache->next_head, 0,
           (size_t)cache->geometry.layers * sizeof(*cache->next_head));
}

const kv_cache_geometry *kv_cache_get_geometry(const kv_cache *cache) {
    return cache == NULL ? NULL : &cache->geometry;
}

size_t kv_cache_storage_bytes(const kv_cache *cache) {
    return cache == NULL ? 0 : cache->storage_bytes;
}

uint64_t kv_cache_layer_tokens(const kv_cache *cache, uint32_t layer) {
    if (cache == NULL || layer >= cache->geometry.layers) return 0;
    return cache->layer_tokens[layer];
}

static size_t row_index(const kv_cache *cache, uint32_t layer,
                        uint64_t token, uint32_t head) {
    return (((size_t)layer * (size_t)cache->geometry.capacity_tokens
             + (size_t)token) * (size_t)cache->geometry.kv_heads)
           + (size_t)head;
}

static uint8_t *mutable_row(kv_cache *cache, kv_cache_kind kind,
                            size_t index) {
    if (kind == KV_CACHE_KEY)
        return cache->keys + index * cache->key_row_bytes;
    return cache->values + index * cache->value_row_bytes;
}

static const uint8_t *const_row(const kv_cache *cache, kv_cache_kind kind,
                                size_t index) {
    if (kind == KV_CACHE_KEY)
        return cache->keys + index * cache->key_row_bytes;
    return cache->values + index * cache->value_row_bytes;
}

static kv_cache_encoding kind_encoding(const kv_cache *cache,
                                       kv_cache_kind kind) {
    return kind == KV_CACHE_KEY ? cache->geometry.key_encoding
                                : cache->geometry.value_encoding;
}

static int vector_is_finite(const float *vector, uint32_t dim) {
    uint32_t i;
    for (i = 0; i < dim; ++i) {
        if (!isfinite(vector[i])) return 0;
    }
    return 1;
}

static void encode_i8(uint8_t *row, const float *vector, uint32_t dim) {
    float max_abs = 0.0f;
    float scale;
    int8_t *quantized = (int8_t *)(void *)(row + sizeof(float));
    uint32_t i;

    for (i = 0; i < dim; ++i) {
        float magnitude = fabsf(vector[i]);
        if (magnitude > max_abs) max_abs = magnitude;
    }
    if (max_abs == 0.0f) {
        scale = 0.0f;
        memcpy(row, &scale, sizeof(scale));
        memset(quantized, 0, dim);
        return;
    }

    scale = max_abs / 127.0f;
    /* Preserve non-zero subnormals when max_abs/127 underflows. */
    if (scale == 0.0f) scale = nextafterf(0.0f, 1.0f);
    /* A rounded-up FLT_MAX/127 scale can overflow again at q=127. */
    if (!isfinite(scale * 127.0f)) scale = nextafterf(scale, 0.0f);
    memcpy(row, &scale, sizeof(scale));
    for (i = 0; i < dim; ++i) {
        double normalized = (double)vector[i] / (double)scale;
        long q = normalized >= 0.0
               ? (long)floor(normalized + 0.5)
               : (long)ceil(normalized - 0.5);
        if (q > 127) q = 127;
        if (q < -127) q = -127;
        quantized[i] = (int8_t)q;
    }
}

static void encode_row(kv_cache_encoding encoding, uint8_t *row,
                       const float *vector, uint32_t dim) {
    if (encoding == KV_CACHE_F32) {
        memcpy(row, vector, (size_t)dim * sizeof(float));
    } else {
        encode_i8(row, vector, dim);
    }
}

static float row_scale(kv_cache_encoding encoding, const uint8_t *row) {
    float scale = 1.0f;
    if (encoding == KV_CACHE_I8) memcpy(&scale, row, sizeof(scale));
    return scale;
}

static void decode_row(kv_cache_encoding encoding, const uint8_t *row,
                       uint32_t dim, float *out) {
    uint32_t i = 0;
    if (encoding == KV_CACHE_F32) {
        memcpy(out, row, (size_t)dim * sizeof(float));
        return;
    }
    {
        float scale;
        const int8_t *quantized = (const int8_t *)(const void *)(row + sizeof(float));
        memcpy(&scale, row, sizeof(scale));
#if KV_CACHE_HAS_NEON
        {
            float32x4_t scale4 = vdupq_n_f32(scale);
            for (; i + 16 <= dim; i += 16) {
                int8x16_t q8 = vld1q_s8(quantized + i);
                int16x8_t q16lo = vmovl_s8(vget_low_s8(q8));
                int16x8_t q16hi = vmovl_s8(vget_high_s8(q8));
                int32x4_t q0 = vmovl_s16(vget_low_s16(q16lo));
                int32x4_t q1 = vmovl_s16(vget_high_s16(q16lo));
                int32x4_t q2 = vmovl_s16(vget_low_s16(q16hi));
                int32x4_t q3 = vmovl_s16(vget_high_s16(q16hi));
                vst1q_f32(out + i, vmulq_f32(vcvtq_f32_s32(q0), scale4));
                vst1q_f32(out + i + 4, vmulq_f32(vcvtq_f32_s32(q1), scale4));
                vst1q_f32(out + i + 8, vmulq_f32(vcvtq_f32_s32(q2), scale4));
                vst1q_f32(out + i + 12, vmulq_f32(vcvtq_f32_s32(q3), scale4));
            }
        }
#endif
        for (; i < dim; ++i) out[i] = (float)quantized[i] * scale;
    }
}

kv_cache_status kv_cache_append_head(kv_cache *cache,
                                     uint32_t layer,
                                     uint64_t token,
                                     uint32_t head,
                                     const float *key,
                                     const float *value) {
    size_t index;
    uint32_t dim;
    if (cache == NULL || key == NULL || value == NULL)
        return KV_CACHE_INVALID_ARGUMENT;
    if (layer >= cache->geometry.layers || head >= cache->geometry.kv_heads
        || token >= cache->geometry.capacity_tokens)
        return KV_CACHE_BOUNDS;
    if (token != cache->layer_tokens[layer]
        || head != cache->next_head[layer])
        return KV_CACHE_BAD_STATE;

    dim = cache->geometry.head_dim;
    /* Validate both vectors before changing either shelf. */
    if (!vector_is_finite(key, dim) || !vector_is_finite(value, dim))
        return KV_CACHE_NONFINITE;

    index = row_index(cache, layer, token, head);
    encode_row(cache->geometry.key_encoding,
               mutable_row(cache, KV_CACHE_KEY, index), key, dim);
    encode_row(cache->geometry.value_encoding,
               mutable_row(cache, KV_CACHE_VALUE, index), value, dim);

    ++cache->next_head[layer];
    if (cache->next_head[layer] == cache->geometry.kv_heads) {
        cache->next_head[layer] = 0;
        ++cache->layer_tokens[layer];
    }
    return KV_CACHE_OK;
}

static kv_cache_status checked_committed_row(const kv_cache *cache,
                                             kv_cache_kind kind,
                                             uint32_t layer,
                                             uint64_t token,
                                             uint32_t head,
                                             const uint8_t **row_out) {
    if (cache == NULL || row_out == NULL
        || (kind != KV_CACHE_KEY && kind != KV_CACHE_VALUE))
        return KV_CACHE_INVALID_ARGUMENT;
    if (layer >= cache->geometry.layers || head >= cache->geometry.kv_heads)
        return KV_CACHE_BOUNDS;
    if (token >= cache->layer_tokens[layer]) return KV_CACHE_BAD_STATE;
    *row_out = const_row(cache, kind, row_index(cache, layer, token, head));
    return KV_CACHE_OK;
}

kv_cache_status kv_cache_read_head(const kv_cache *cache,
                                   uint32_t layer,
                                   uint64_t token,
                                   uint32_t head,
                                   float *key_out,
                                   float *value_out) {
    const uint8_t *key_row, *value_row;
    kv_cache_status status;
    if (key_out == NULL || value_out == NULL)
        return KV_CACHE_INVALID_ARGUMENT;
    status = checked_committed_row(cache, KV_CACHE_KEY, layer, token, head,
                                   &key_row);
    if (status != KV_CACHE_OK) return status;
    status = checked_committed_row(cache, KV_CACHE_VALUE, layer, token, head,
                                   &value_row);
    if (status != KV_CACHE_OK) return status;
    decode_row(cache->geometry.key_encoding, key_row,
               cache->geometry.head_dim, key_out);
    decode_row(cache->geometry.value_encoding, value_row,
               cache->geometry.head_dim, value_out);
    return KV_CACHE_OK;
}

kv_cache_status kv_cache_get_scale(const kv_cache *cache,
                                   kv_cache_kind kind,
                                   uint32_t layer,
                                   uint64_t token,
                                   uint32_t head,
                                   float *scale_out) {
    const uint8_t *row;
    kv_cache_status status;
    if (scale_out == NULL) return KV_CACHE_INVALID_ARGUMENT;
    status = checked_committed_row(cache, kind, layer, token, head, &row);
    if (status != KV_CACHE_OK) return status;
    *scale_out = row_scale(kind_encoding(cache, kind), row);
    return KV_CACHE_OK;
}

static float dot_f32_scalar(const float *a, const float *b, uint32_t dim) {
    float sum = 0.0f;
    uint32_t i;
    for (i = 0; i < dim; ++i) sum += a[i] * b[i];
    return sum;
}

static float dot_f32(const float *a, const float *b, uint32_t dim) {
    uint32_t i = 0;
    float sum = 0.0f;
#if KV_CACHE_HAS_NEON
    float32x4_t accum = vdupq_n_f32(0.0f);
    for (; i + 4 <= dim; i += 4)
        accum = vmlaq_f32(accum, vld1q_f32(a + i), vld1q_f32(b + i));
    sum += vaddvq_f32(accum);
#endif
    if (i == 0) return dot_f32_scalar(a, b, dim);
    for (; i < dim; ++i) sum += a[i] * b[i];
    return sum;
}

static float dot_i8(const uint8_t *row, const float *query, uint32_t dim) {
    const int8_t *quantized = (const int8_t *)(const void *)(row + sizeof(float));
    float scale, sum = 0.0f;
    uint32_t i = 0;
    memcpy(&scale, row, sizeof(scale));
#if KV_CACHE_HAS_NEON
    {
        float32x4_t accum = vdupq_n_f32(0.0f);
        for (; i + 16 <= dim; i += 16) {
            int8x16_t q8 = vld1q_s8(quantized + i);
            int16x8_t q16lo = vmovl_s8(vget_low_s8(q8));
            int16x8_t q16hi = vmovl_s8(vget_high_s8(q8));
            int32x4_t q0 = vmovl_s16(vget_low_s16(q16lo));
            int32x4_t q1 = vmovl_s16(vget_high_s16(q16lo));
            int32x4_t q2 = vmovl_s16(vget_low_s16(q16hi));
            int32x4_t q3 = vmovl_s16(vget_high_s16(q16hi));
            accum = vmlaq_f32(accum, vld1q_f32(query + i),
                              vcvtq_f32_s32(q0));
            accum = vmlaq_f32(accum, vld1q_f32(query + i + 4),
                              vcvtq_f32_s32(q1));
            accum = vmlaq_f32(accum, vld1q_f32(query + i + 8),
                              vcvtq_f32_s32(q2));
            accum = vmlaq_f32(accum, vld1q_f32(query + i + 12),
                              vcvtq_f32_s32(q3));
        }
        sum = vaddvq_f32(accum);
    }
#endif
    for (; i < dim; ++i) sum += query[i] * (float)quantized[i];
    return sum * scale;
}

kv_cache_status kv_cache_dot_key(const kv_cache *cache,
                                 uint32_t layer,
                                 uint64_t token,
                                 uint32_t head,
                                 const float *query,
                                 float *dot_out) {
    const uint8_t *row;
    kv_cache_status status;
    if (query == NULL || dot_out == NULL) return KV_CACHE_INVALID_ARGUMENT;
    if (!vector_is_finite(query, cache == NULL ? 0 : cache->geometry.head_dim))
        return cache == NULL ? KV_CACHE_INVALID_ARGUMENT : KV_CACHE_NONFINITE;
    status = checked_committed_row(cache, KV_CACHE_KEY, layer, token, head,
                                   &row);
    if (status != KV_CACHE_OK) return status;
    if (cache->geometry.key_encoding == KV_CACHE_F32) {
        *dot_out = dot_f32((const float *)(const void *)row, query,
                           cache->geometry.head_dim);
    } else {
        *dot_out = dot_i8(row, query, cache->geometry.head_dim);
    }
    return KV_CACHE_OK;
}

static void axpy_f32(float *accumulator, const float *row, float weight,
                     uint32_t dim) {
    uint32_t i = 0;
#if KV_CACHE_HAS_NEON
    float32x4_t w = vdupq_n_f32(weight);
    for (; i + 4 <= dim; i += 4) {
        float32x4_t a = vld1q_f32(accumulator + i);
        a = vmlaq_f32(a, vld1q_f32(row + i), w);
        vst1q_f32(accumulator + i, a);
    }
#endif
    for (; i < dim; ++i) accumulator[i] += weight * row[i];
}

static void axpy_i8(float *accumulator, const uint8_t *row, float weight,
                    uint32_t dim) {
    const int8_t *quantized = (const int8_t *)(const void *)(row + sizeof(float));
    float scale;
    uint32_t i = 0;
    memcpy(&scale, row, sizeof(scale));
#if KV_CACHE_HAS_NEON
    {
        float32x4_t ws = vdupq_n_f32(weight * scale);
        for (; i + 16 <= dim; i += 16) {
            int8x16_t q8 = vld1q_s8(quantized + i);
            int16x8_t q16lo = vmovl_s8(vget_low_s8(q8));
            int16x8_t q16hi = vmovl_s8(vget_high_s8(q8));
            int32x4_t q0 = vmovl_s16(vget_low_s16(q16lo));
            int32x4_t q1 = vmovl_s16(vget_high_s16(q16lo));
            int32x4_t q2 = vmovl_s16(vget_low_s16(q16hi));
            int32x4_t q3 = vmovl_s16(vget_high_s16(q16hi));
            float32x4_t a0 = vld1q_f32(accumulator + i);
            float32x4_t a1 = vld1q_f32(accumulator + i + 4);
            float32x4_t a2 = vld1q_f32(accumulator + i + 8);
            float32x4_t a3 = vld1q_f32(accumulator + i + 12);
            vst1q_f32(accumulator + i,
                      vmlaq_f32(a0, vcvtq_f32_s32(q0), ws));
            vst1q_f32(accumulator + i + 4,
                      vmlaq_f32(a1, vcvtq_f32_s32(q1), ws));
            vst1q_f32(accumulator + i + 8,
                      vmlaq_f32(a2, vcvtq_f32_s32(q2), ws));
            vst1q_f32(accumulator + i + 12,
                      vmlaq_f32(a3, vcvtq_f32_s32(q3), ws));
        }
    }
#endif
    for (; i < dim; ++i)
        accumulator[i] += weight * scale * (float)quantized[i];
}

kv_cache_status kv_cache_accumulate_value(const kv_cache *cache,
                                          uint32_t layer,
                                          uint64_t token,
                                          uint32_t head,
                                          float weight,
                                          float *accumulator) {
    const uint8_t *row;
    kv_cache_status status;
    if (accumulator == NULL || !isfinite(weight))
        return KV_CACHE_INVALID_ARGUMENT;
    status = checked_committed_row(cache, KV_CACHE_VALUE, layer, token, head,
                                   &row);
    if (status != KV_CACHE_OK) return status;
    if (cache->geometry.value_encoding == KV_CACHE_F32) {
        axpy_f32(accumulator, (const float *)(const void *)row, weight,
                 cache->geometry.head_dim);
    } else {
        axpy_i8(accumulator, row, weight, cache->geometry.head_dim);
    }
    return KV_CACHE_OK;
}

uint32_t kv_cache_crc32c(const void *data, size_t bytes) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = UINT32_MAX;
    size_t i;
    if (data == NULL && bytes != 0) return 0;
    for (i = 0; i < bytes; ++i) {
        unsigned bit;
        crc ^= p[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0x82f63b78u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

kv_cache_status kv_cache_persist_payload_bytes(const kv_cache_geometry *geometry,
                                               uint64_t committed_tokens,
                                               uint64_t *bytes_out) {
    size_t key_row_size, value_row_size;
    uint64_t row_pair, rows, bytes;
    kv_cache_status status;
    if (geometry == NULL || bytes_out == NULL || geometry->layers == 0
        || geometry->kv_heads == 0 || geometry->head_dim == 0
        || geometry->capacity_tokens == 0
        || committed_tokens > geometry->capacity_tokens)
        return KV_CACHE_INVALID_ARGUMENT;
    status = kv_cache_row_bytes(geometry->key_encoding, geometry->head_dim,
                                &key_row_size);
    if (status != KV_CACHE_OK) return status;
    status = kv_cache_row_bytes(geometry->value_encoding, geometry->head_dim,
                                &value_row_size);
    if (status != KV_CACHE_OK) return status;
    if ((uint64_t)key_row_size != key_row_size
        || (uint64_t)value_row_size != value_row_size
        || !add_u64((uint64_t)key_row_size, (uint64_t)value_row_size,
                    &row_pair)
        || !mul_u64((uint64_t)geometry->layers,
                    (uint64_t)geometry->kv_heads, &rows)
        || !mul_u64(rows, committed_tokens, &rows)
        || !mul_u64(rows, row_pair, &bytes))
        return KV_CACHE_OVERFLOW;
    *bytes_out = bytes;
    return KV_CACHE_OK;
}

static int geometry_equal(const kv_cache_geometry *a,
                          const kv_cache_geometry *b) {
    return a->layers == b->layers
        && a->kv_heads == b->kv_heads
        && a->head_dim == b->head_dim
        && a->capacity_tokens == b->capacity_tokens
        && a->key_encoding == b->key_encoding
        && a->value_encoding == b->value_encoding;
}

static kv_cache_status persist_info_validate(const kv_cache_persist_info *info,
                                             uint64_t *payload_bytes_out,
                                             size_t *key_row_out,
                                             size_t *value_row_out) {
    kv_cache_status status;
    if (info == NULL || payload_bytes_out == NULL || key_row_out == NULL
        || value_row_out == NULL)
        return KV_CACHE_INVALID_ARGUMENT;
    if (info->format_version != KV_CACHE_FORMAT_V1
        && info->format_version != KV_CACHE_FORMAT_V2)
        return KV_CACHE_INCOMPATIBLE;
    if (info->format_version == KV_CACHE_FORMAT_V1
        && (info->geometry.key_encoding != KV_CACHE_F32
            || info->geometry.value_encoding != KV_CACHE_F32))
        return KV_CACHE_INCOMPATIBLE;
    status = kv_cache_geometry_storage_bytes(&info->geometry, key_row_out);
    if (status != KV_CACHE_OK) return status;
    status = kv_cache_row_bytes(info->geometry.key_encoding,
                                info->geometry.head_dim, key_row_out);
    if (status != KV_CACHE_OK) return status;
    status = kv_cache_row_bytes(info->geometry.value_encoding,
                                info->geometry.head_dim, value_row_out);
    if (status != KV_CACHE_OK) return status;
    return kv_cache_persist_payload_bytes(&info->geometry,
                                          info->committed_tokens,
                                          payload_bytes_out);
}

kv_cache_status kv_cache_header_encode(const kv_cache_persist_info *info,
                                       uint8_t out[KV_CACHE_HEADER_BYTES]) {
    uint64_t payload_bytes;
    size_t key_row, value_row;
    kv_cache_status status;
    uint32_t header_crc;
    if (out == NULL) return KV_CACHE_INVALID_ARGUMENT;
    status = persist_info_validate(info, &payload_bytes, &key_row, &value_row);
    if (status != KV_CACHE_OK) return status;

    memset(out, 0, KV_CACHE_HEADER_BYTES);
    memcpy(out, KV_CACHE_MAGIC, sizeof(KV_CACHE_MAGIC));
    put_u16le(out + HDR_VERSION, info->format_version);
    put_u16le(out + HDR_SIZE, KV_CACHE_HEADER_BYTES);
    put_u32le(out + HDR_FLAGS, 0);
    put_u32le(out + HDR_LAYERS, info->geometry.layers);
    put_u32le(out + HDR_HEADS, info->geometry.kv_heads);
    put_u32le(out + HDR_DIM, info->geometry.head_dim);
    out[HDR_KEY_ENCODING] = (uint8_t)info->geometry.key_encoding;
    out[HDR_VALUE_ENCODING] = (uint8_t)info->geometry.value_encoding;
    out[HDR_SCALE_TYPE] = 1;   /* IEEE-754 f32 */
    out[HDR_QUANT_SCHEME] = 1; /* signed symmetric [-127,127] */
    put_u64le(out + HDR_CAPACITY, info->geometry.capacity_tokens);
    put_u64le(out + HDR_COMMITTED, info->committed_tokens);
    put_u64le(out + HDR_KEY_ROW_BYTES, (uint64_t)key_row);
    put_u64le(out + HDR_VALUE_ROW_BYTES, (uint64_t)value_row);
    put_u64le(out + HDR_PAYLOAD_BYTES, payload_bytes);
    memcpy(out + HDR_MODEL_ID, info->model_id, KV_CACHE_MODEL_ID_BYTES);
    put_u32le(out + HDR_PAYLOAD_CRC, info->payload_crc32c);
    header_crc = kv_cache_crc32c(out, KV_CACHE_HEADER_BYTES);
    put_u32le(out + HDR_HEADER_CRC, header_crc);
    return KV_CACHE_OK;
}

kv_cache_status kv_cache_header_decode(
    const uint8_t *header,
    size_t header_bytes,
    const kv_cache_geometry *expected_geometry,
    const uint8_t expected_model_id[KV_CACHE_MODEL_ID_BYTES],
    kv_cache_persist_info *info_out) {
    uint8_t copy[KV_CACHE_HEADER_BYTES];
    kv_cache_persist_info info;
    uint32_t recorded_crc, actual_crc;
    uint64_t payload_bytes, encoded_payload_bytes;
    size_t key_row, value_row;
    kv_cache_status status;
    size_t i;

    if (header == NULL || info_out == NULL)
        return KV_CACHE_INVALID_ARGUMENT;
    if (header_bytes < KV_CACHE_HEADER_BYTES) return KV_CACHE_BAD_FORMAT;
    if (memcmp(header, KV_CACHE_MAGIC, sizeof(KV_CACHE_MAGIC)) != 0)
        return KV_CACHE_BAD_FORMAT;
    if (get_u16le(header + HDR_SIZE) != KV_CACHE_HEADER_BYTES)
        return KV_CACHE_BAD_FORMAT;

    memcpy(copy, header, sizeof(copy));
    recorded_crc = get_u32le(copy + HDR_HEADER_CRC);
    memset(copy + HDR_HEADER_CRC, 0, sizeof(uint32_t));
    actual_crc = kv_cache_crc32c(copy, sizeof(copy));
    if (recorded_crc != actual_crc) return KV_CACHE_CORRUPT;
    if (get_u32le(header + HDR_FLAGS) != 0) return KV_CACHE_BAD_FORMAT;
    for (i = HDR_RESERVED; i < KV_CACHE_HEADER_BYTES; ++i) {
        if (header[i] != 0) return KV_CACHE_BAD_FORMAT;
    }
    if (header[HDR_SCALE_TYPE] != 1 || header[HDR_QUANT_SCHEME] != 1)
        return KV_CACHE_INCOMPATIBLE;

    memset(&info, 0, sizeof(info));
    info.format_version = get_u16le(header + HDR_VERSION);
    info.geometry.layers = get_u32le(header + HDR_LAYERS);
    info.geometry.kv_heads = get_u32le(header + HDR_HEADS);
    info.geometry.head_dim = get_u32le(header + HDR_DIM);
    info.geometry.key_encoding = (kv_cache_encoding)header[HDR_KEY_ENCODING];
    info.geometry.value_encoding = (kv_cache_encoding)header[HDR_VALUE_ENCODING];
    info.geometry.capacity_tokens = get_u64le(header + HDR_CAPACITY);
    info.committed_tokens = get_u64le(header + HDR_COMMITTED);
    memcpy(info.model_id, header + HDR_MODEL_ID, KV_CACHE_MODEL_ID_BYTES);
    info.payload_crc32c = get_u32le(header + HDR_PAYLOAD_CRC);

    status = persist_info_validate(&info, &payload_bytes, &key_row, &value_row);
    if (status != KV_CACHE_OK) return status;
    encoded_payload_bytes = get_u64le(header + HDR_PAYLOAD_BYTES);
    if (get_u64le(header + HDR_KEY_ROW_BYTES) != (uint64_t)key_row
        || get_u64le(header + HDR_VALUE_ROW_BYTES) != (uint64_t)value_row
        || encoded_payload_bytes != payload_bytes)
        return KV_CACHE_BAD_FORMAT;
    if (expected_geometry != NULL
        && !geometry_equal(&info.geometry, expected_geometry))
        return KV_CACHE_INCOMPATIBLE;
    if (expected_model_id != NULL
        && memcmp(info.model_id, expected_model_id,
                  KV_CACHE_MODEL_ID_BYTES) != 0)
        return KV_CACHE_INCOMPATIBLE;
    *info_out = info;
    return KV_CACHE_OK;
}

kv_cache_status kv_cache_validate_payload(const kv_cache_persist_info *info,
                                          const void *payload,
                                          size_t payload_bytes) {
    uint64_t expected_bytes;
    kv_cache_status status;
    if (info == NULL || (payload == NULL && payload_bytes != 0))
        return KV_CACHE_INVALID_ARGUMENT;
    status = kv_cache_persist_payload_bytes(&info->geometry,
                                            info->committed_tokens,
                                            &expected_bytes);
    if (status != KV_CACHE_OK) return status;
    if (expected_bytes > (uint64_t)SIZE_MAX
        || payload_bytes != (size_t)expected_bytes)
        return KV_CACHE_BAD_FORMAT;
    return kv_cache_crc32c(payload, payload_bytes) == info->payload_crc32c
         ? KV_CACHE_OK : KV_CACHE_CORRUPT;
}
