#include "kv_cache.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t checks;

#define CHECK(condition) do {                                                   \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1);                                                               \
    }                                                                          \
} while (0)

#define CHECK_STATUS(expression, expected) do {                                \
    kv_cache_status status_ = (expression);                                    \
    ++checks;                                                                  \
    if (status_ != (expected)) {                                               \
        fprintf(stderr, "FAIL %s:%d: %s returned %s, expected %s\n",         \
                __FILE__, __LINE__, #expression,                               \
                kv_cache_status_string(status_),                               \
                kv_cache_status_string(expected));                             \
        exit(1);                                                               \
    }                                                                          \
} while (0)

static uint64_t rng_state = UINT64_C(0x6a09e667f3bcc909);

static uint32_t random_u32(void) {
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return (uint32_t)((x * UINT64_C(0x2545f4914f6cdd1d)) >> 32);
}

static float random_signed(void) {
    return ((float)(random_u32() >> 8) * (1.0f / 8388608.0f)) - 1.0f;
}

static float scalar_dot(const float *a, const float *b, uint32_t dim) {
    float sum = 0.0f;
    uint32_t i;
    for (i = 0; i < dim; ++i) sum += a[i] * b[i];
    return sum;
}

static void fill_vector(float *vector, uint32_t dim, float amplitude) {
    uint32_t i;
    for (i = 0; i < dim; ++i) {
        float shaped = random_signed() + 0.2f * sinf((float)i * 0.31f);
        vector[i] = amplitude * shaped;
    }
}

static void test_status_and_sizing(void) {
    kv_cache_geometry f32 = {10, 2, 256, 32768, KV_CACHE_F32, KV_CACHE_F32};
    kv_cache_geometry i8 = {10, 2, 256, 32768, KV_CACHE_I8, KV_CACHE_I8};
    kv_cache_geometry bad;
    size_t f32_bytes = 0, i8_bytes = 0, row = 0;
    uint64_t payload = 0;
    unsigned status;

    CHECK_STATUS(kv_cache_row_bytes(KV_CACHE_F32, 256, &row), KV_CACHE_OK);
    CHECK(row == 1024);
    CHECK_STATUS(kv_cache_row_bytes(KV_CACHE_I8, 256, &row), KV_CACHE_OK);
    CHECK(row == 260);
    CHECK_STATUS(kv_cache_row_bytes((kv_cache_encoding)0, 256, &row),
                 KV_CACHE_INVALID_ARGUMENT);
    CHECK_STATUS(kv_cache_row_bytes(KV_CACHE_I8, 0, &row),
                 KV_CACHE_INVALID_ARGUMENT);
    CHECK_STATUS(kv_cache_row_bytes(KV_CACHE_I8, 1, NULL),
                 KV_CACHE_INVALID_ARGUMENT);

    CHECK_STATUS(kv_cache_geometry_storage_bytes(&f32, &f32_bytes),
                 KV_CACHE_OK);
    CHECK_STATUS(kv_cache_geometry_storage_bytes(&i8, &i8_bytes),
                 KV_CACHE_OK);
    CHECK(f32_bytes == (size_t)10 * 2 * 32768 * 256 * 2 * sizeof(float));
    CHECK(i8_bytes == (size_t)10 * 2 * 32768 * (256 + 4) * 2);
    CHECK((double)i8_bytes / (double)f32_bytes <= 0.30);
    printf("Qwen GQA KV target: f32=%zu bytes, int8=%zu bytes (%.3f%%)\n",
           f32_bytes, i8_bytes, 100.0 * (double)i8_bytes / (double)f32_bytes);

    bad = f32;
    bad.layers = 0;
    CHECK_STATUS(kv_cache_geometry_storage_bytes(&bad, &row),
                 KV_CACHE_INVALID_ARGUMENT);
    bad = f32;
    bad.kv_heads = 0;
    CHECK_STATUS(kv_cache_geometry_storage_bytes(&bad, &row),
                 KV_CACHE_INVALID_ARGUMENT);
    bad = f32;
    bad.head_dim = 0;
    CHECK_STATUS(kv_cache_geometry_storage_bytes(&bad, &row),
                 KV_CACHE_INVALID_ARGUMENT);
    bad = f32;
    bad.capacity_tokens = 0;
    CHECK_STATUS(kv_cache_geometry_storage_bytes(&bad, &row),
                 KV_CACHE_INVALID_ARGUMENT);
    bad.layers = UINT32_MAX;
    bad.kv_heads = UINT32_MAX;
    bad.head_dim = UINT32_MAX;
    bad.capacity_tokens = UINT64_MAX;
    CHECK_STATUS(kv_cache_geometry_storage_bytes(&bad, &row),
                 KV_CACHE_OVERFLOW);

    CHECK_STATUS(kv_cache_persist_payload_bytes(&i8, 1234, &payload),
                 KV_CACHE_OK);
    CHECK(payload == UINT64_C(10) * 2 * 1234 * 520);
    CHECK_STATUS(kv_cache_persist_payload_bytes(&i8, 32769, &payload),
                 KV_CACHE_INVALID_ARGUMENT);
    CHECK_STATUS(kv_cache_persist_payload_bytes(NULL, 0, &payload),
                 KV_CACHE_INVALID_ARGUMENT);

    for (status = KV_CACHE_OK; status <= KV_CACHE_INCOMPATIBLE; ++status)
        CHECK(kv_cache_status_string((kv_cache_status)status) != NULL);
    CHECK(kv_cache_status_string((kv_cache_status)999) != NULL);
}

static void test_append_order_f32_and_clear(void) {
    kv_cache_geometry geometry = {2, 3, 5, 3, KV_CACHE_F32, KV_CACHE_F32};
    kv_cache_status status = KV_CACHE_CORRUPT;
    kv_cache *cache = kv_cache_create(&geometry, &status);
    float key[5] = {1.0f, -2.0f, 3.5f, -0.0f, FLT_MIN};
    float value[5] = {-4.0f, 5.0f, -6.0f, 7.0f, FLT_MAX};
    float key_out[5], value_out[5], dot, expected, accum[5] = {0};
    size_t expected_storage;
    uint32_t head, i;

    CHECK(cache != NULL);
    CHECK(status == KV_CACHE_OK);
    CHECK(kv_cache_get_geometry(cache) != NULL);
    CHECK(kv_cache_get_geometry(cache)->head_dim == 5);
    CHECK_STATUS(kv_cache_geometry_storage_bytes(&geometry, &expected_storage),
                 KV_CACHE_OK);
    CHECK(kv_cache_storage_bytes(cache) == expected_storage);
    CHECK(kv_cache_layer_tokens(cache, 0) == 0);
    CHECK(kv_cache_layer_tokens(cache, 99) == 0);

    CHECK_STATUS(kv_cache_append_head(cache, 0, 1, 0, key, value),
                 KV_CACHE_BAD_STATE);
    CHECK_STATUS(kv_cache_append_head(cache, 0, 0, 1, key, value),
                 KV_CACHE_BAD_STATE);
    CHECK_STATUS(kv_cache_append_head(cache, 2, 0, 0, key, value),
                 KV_CACHE_BOUNDS);
    CHECK_STATUS(kv_cache_append_head(cache, 0, 0, 3, key, value),
                 KV_CACHE_BOUNDS);
    CHECK_STATUS(kv_cache_append_head(cache, 0, 3, 0, key, value),
                 KV_CACHE_BOUNDS);
    CHECK_STATUS(kv_cache_append_head(cache, 0, 0, 0, NULL, value),
                 KV_CACHE_INVALID_ARGUMENT);

    for (head = 0; head < geometry.kv_heads; ++head) {
        for (i = 0; i < geometry.head_dim; ++i) {
            key[i] = (float)(10 * head + i) - 4.0f;
            value[i] = (float)(-7 * (int)head + (int)i) + 0.25f;
        }
        CHECK_STATUS(kv_cache_append_head(cache, 0, 0, head, key, value),
                     KV_CACHE_OK);
        if (head + 1 < geometry.kv_heads) {
            CHECK(kv_cache_layer_tokens(cache, 0) == 0);
            CHECK_STATUS(kv_cache_read_head(cache, 0, 0, 0,
                                            key_out, value_out),
                         KV_CACHE_BAD_STATE);
        }
    }
    CHECK(kv_cache_layer_tokens(cache, 0) == 1);

    /* Re-create the expected final head and require byte-exact f32 control. */
    head = geometry.kv_heads - 1;
    for (i = 0; i < geometry.head_dim; ++i) {
        key[i] = (float)(10 * head + i) - 4.0f;
        value[i] = (float)(-7 * (int)head + (int)i) + 0.25f;
    }
    CHECK_STATUS(kv_cache_read_head(cache, 0, 0, head, key_out, value_out),
                 KV_CACHE_OK);
    CHECK(memcmp(key, key_out, sizeof(key)) == 0);
    CHECK(memcmp(value, value_out, sizeof(value)) == 0);

    expected = scalar_dot(key, value, geometry.head_dim);
    CHECK_STATUS(kv_cache_dot_key(cache, 0, 0, head, value, &dot),
                 KV_CACHE_OK);
    CHECK(fabsf(dot - expected) <= 2e-5f * (1.0f + fabsf(expected)));
    CHECK_STATUS(kv_cache_accumulate_value(cache, 0, 0, head, 0.25f, accum),
                 KV_CACHE_OK);
    for (i = 0; i < geometry.head_dim; ++i)
        CHECK(accum[i] == 0.25f * value[i]);

    CHECK_STATUS(kv_cache_read_head(cache, 0, 1, 0, key_out, value_out),
                 KV_CACHE_BAD_STATE);
    CHECK_STATUS(kv_cache_dot_key(cache, 0, 0, 0, NULL, &dot),
                 KV_CACHE_INVALID_ARGUMENT);
    CHECK_STATUS(kv_cache_accumulate_value(cache, 0, 0, 0, NAN, accum),
                 KV_CACHE_INVALID_ARGUMENT);

    kv_cache_clear(cache);
    CHECK(kv_cache_layer_tokens(cache, 0) == 0);
    CHECK(kv_cache_layer_tokens(cache, 1) == 0);
    CHECK_STATUS(kv_cache_read_head(cache, 0, 0, 0, key_out, value_out),
                 KV_CACHE_BAD_STATE);
    kv_cache_destroy(cache);
    kv_cache_destroy(NULL);
}

static void test_nonfinite_zero_extremes(void) {
    kv_cache_geometry geometry = {1, 1, 7, 5, KV_CACHE_I8, KV_CACHE_I8};
    kv_cache_status status;
    kv_cache *cache = kv_cache_create(&geometry, &status);
    float zero[7] = {0};
    float key[7] = {0}, value[7] = {0};
    float key_out[7], value_out[7], scale;
    uint32_t i;

    CHECK(cache != NULL && status == KV_CACHE_OK);
    CHECK_STATUS(kv_cache_append_head(cache, 0, 0, 0, zero, zero),
                 KV_CACHE_OK);
    CHECK_STATUS(kv_cache_get_scale(cache, KV_CACHE_KEY, 0, 0, 0, &scale),
                 KV_CACHE_OK);
    CHECK(scale == 0.0f);
    CHECK_STATUS(kv_cache_get_scale(cache, KV_CACHE_VALUE, 0, 0, 0, &scale),
                 KV_CACHE_OK);
    CHECK(scale == 0.0f);
    CHECK_STATUS(kv_cache_read_head(cache, 0, 0, 0, key_out, value_out),
                 KV_CACHE_OK);
    for (i = 0; i < 7; ++i) CHECK(key_out[i] == 0.0f && value_out[i] == 0.0f);

    key[0] = NAN;
    CHECK_STATUS(kv_cache_append_head(cache, 0, 1, 0, key, zero),
                 KV_CACHE_NONFINITE);
    CHECK(kv_cache_layer_tokens(cache, 0) == 1);
    key[0] = 0.0f;
    value[3] = INFINITY;
    CHECK_STATUS(kv_cache_append_head(cache, 0, 1, 0, key, value),
                 KV_CACHE_NONFINITE);
    CHECK(kv_cache_layer_tokens(cache, 0) == 1);
    value[3] = -INFINITY;
    CHECK_STATUS(kv_cache_append_head(cache, 0, 1, 0, key, value),
                 KV_CACHE_NONFINITE);
    value[3] = 0.0f;

    for (i = 0; i < 7; ++i) {
        key[i] = (i & 1u) ? -FLT_MAX : FLT_MAX;
        value[i] = (i & 1u) ? FLT_MAX : -FLT_MAX;
    }
    CHECK_STATUS(kv_cache_append_head(cache, 0, 1, 0, key, value),
                 KV_CACHE_OK);
    CHECK_STATUS(kv_cache_read_head(cache, 0, 1, 0, key_out, value_out),
                 KV_CACHE_OK);
    for (i = 0; i < 7; ++i) {
        CHECK(isfinite(key_out[i]));
        CHECK(isfinite(value_out[i]));
        CHECK(signbit(key_out[i]) == signbit(key[i]));
        CHECK(signbit(value_out[i]) == signbit(value[i]));
    }

    for (i = 0; i < 7; ++i) key[i] = value[i] = 0.0f;
    key[2] = nextafterf(0.0f, 1.0f);
    value[4] = nextafterf(0.0f, -1.0f);
    CHECK_STATUS(kv_cache_append_head(cache, 0, 2, 0, key, value),
                 KV_CACHE_OK);
    CHECK_STATUS(kv_cache_read_head(cache, 0, 2, 0, key_out, value_out),
                 KV_CACHE_OK);
    CHECK(key_out[2] == key[2]);
    CHECK(value_out[4] == value[4]);

    CHECK_STATUS(kv_cache_get_scale(cache, (kv_cache_kind)9, 0, 0, 0, &scale),
                 KV_CACHE_INVALID_ARGUMENT);
    CHECK_STATUS(kv_cache_get_scale(cache, KV_CACHE_KEY, 0, 0, 0, NULL),
                 KV_CACHE_INVALID_ARGUMENT);
    kv_cache_destroy(cache);
}

static void test_randomized_odd_dimensions(void) {
    static const uint32_t dims[] = {1, 3, 7, 15, 17, 31, 33, 65, 127, 129, 257};
    size_t d;
    double total_squared_error = 0.0, total_signal = 0.0;
    uint64_t values = 0;

    for (d = 0; d < sizeof(dims) / sizeof(dims[0]); ++d) {
        uint32_t dim = dims[d];
        kv_cache_geometry geometry = {1, 1, dim, 24, KV_CACHE_I8, KV_CACHE_I8};
        kv_cache_status status;
        kv_cache *cache = kv_cache_create(&geometry, &status);
        float *key = (float *)malloc((size_t)dim * sizeof(float));
        float *value = (float *)malloc((size_t)dim * sizeof(float));
        float *key_out = (float *)malloc((size_t)dim * sizeof(float));
        float *value_out = (float *)malloc((size_t)dim * sizeof(float));
        float *query = (float *)malloc((size_t)dim * sizeof(float));
        float *accum = (float *)calloc(dim, sizeof(float));
        uint64_t token;
        uint32_t i;

        CHECK(cache != NULL && status == KV_CACHE_OK);
        CHECK(key != NULL && value != NULL && key_out != NULL
              && value_out != NULL && query != NULL && accum != NULL);
        for (token = 0; token < geometry.capacity_tokens; ++token) {
            float amplitude = exp2f((float)((int)(token % 13) - 6));
            float key_scale, value_scale, dot_direct, dot_dequant;
            fill_vector(key, dim, amplitude);
            fill_vector(value, dim, 0.75f * amplitude);
            fill_vector(query, dim, 0.2f);
            if (dim > 4 && token % 7 == 0) key[dim - 1] *= 7.0f;
            CHECK_STATUS(kv_cache_append_head(cache, 0, token, 0, key, value),
                         KV_CACHE_OK);
            CHECK_STATUS(kv_cache_read_head(cache, 0, token, 0,
                                            key_out, value_out),
                         KV_CACHE_OK);
            CHECK_STATUS(kv_cache_get_scale(cache, KV_CACHE_KEY,
                                            0, token, 0, &key_scale),
                         KV_CACHE_OK);
            CHECK_STATUS(kv_cache_get_scale(cache, KV_CACHE_VALUE,
                                            0, token, 0, &value_scale),
                         KV_CACHE_OK);
            for (i = 0; i < dim; ++i) {
                float key_error = fabsf(key_out[i] - key[i]);
                float value_error = fabsf(value_out[i] - value[i]);
                float key_bound = 0.5001f * key_scale
                                + 4.0f * FLT_EPSILON * fabsf(key[i]);
                float value_bound = 0.5001f * value_scale
                                  + 4.0f * FLT_EPSILON * fabsf(value[i]);
                CHECK(key_error <= key_bound + FLT_TRUE_MIN);
                CHECK(value_error <= value_bound + FLT_TRUE_MIN);
                total_squared_error += (double)key_error * key_error
                                     + (double)value_error * value_error;
                total_signal += (double)key[i] * key[i]
                              + (double)value[i] * value[i];
                values += 2;
            }
            CHECK_STATUS(kv_cache_dot_key(cache, 0, token, 0, query,
                                          &dot_direct), KV_CACHE_OK);
            dot_dequant = scalar_dot(query, key_out, dim);
            CHECK(fabsf(dot_direct - dot_dequant)
                  <= 3e-5f * (1.0f + fabsf(dot_dequant)));
            memset(accum, 0, (size_t)dim * sizeof(float));
            CHECK_STATUS(kv_cache_accumulate_value(cache, 0, token, 0,
                                                   -0.375f, accum),
                         KV_CACHE_OK);
            for (i = 0; i < dim; ++i) {
                float expected = -0.375f * value_out[i];
                CHECK(fabsf(accum[i] - expected)
                      <= 3e-6f * (1.0f + fabsf(expected)));
            }
        }
        free(accum);
        free(query);
        free(value_out);
        free(key_out);
        free(value);
        free(key);
        kv_cache_destroy(cache);
    }
    printf("Randomized int8 rows: %llu values, normalized RMSE %.6g\n",
           (unsigned long long)values,
           sqrt(total_squared_error / (total_signal + DBL_MIN)));
    CHECK(sqrt(total_squared_error / (total_signal + DBL_MIN)) < 0.02);
}

static void softmax(float *values, size_t n) {
    float max_value = values[0], sum = 0.0f;
    size_t i;
    for (i = 1; i < n; ++i) if (values[i] > max_value) max_value = values[i];
    for (i = 0; i < n; ++i) {
        values[i] = expf(values[i] - max_value);
        sum += values[i];
    }
    for (i = 0; i < n; ++i) values[i] /= sum;
}

static void test_long_context_attention_drift(void) {
    enum { SEQUENCE = 2048, DIM = 65 };
    kv_cache_geometry geometry = {1, 1, DIM, SEQUENCE,
                                  KV_CACHE_I8, KV_CACHE_I8};
    kv_cache_status status;
    kv_cache *cache = kv_cache_create(&geometry, &status);
    float *keys = (float *)malloc((size_t)SEQUENCE * DIM * sizeof(float));
    float *values = (float *)malloc((size_t)SEQUENCE * DIM * sizeof(float));
    float *query = (float *)malloc(DIM * sizeof(float));
    float *score_ref = (float *)malloc(SEQUENCE * sizeof(float));
    float *score_i8 = (float *)malloc(SEQUENCE * sizeof(float));
    float *ctx_ref = (float *)calloc(DIM, sizeof(float));
    float *ctx_i8 = (float *)calloc(DIM, sizeof(float));
    double score_error = 0.0, score_signal = 0.0;
    double early_error = 0.0, late_error = 0.0;
    double ctx_error = 0.0, ctx_signal = 0.0;
    uint64_t token;
    uint32_t i;
    float inv_sqrt_dim = 1.0f / sqrtf((float)DIM);

    CHECK(cache != NULL && status == KV_CACHE_OK);
    CHECK(keys != NULL && values != NULL && query != NULL
          && score_ref != NULL && score_i8 != NULL
          && ctx_ref != NULL && ctx_i8 != NULL);
    fill_vector(query, DIM, 0.7f);
    for (token = 0; token < SEQUENCE; ++token) {
        float *key = keys + (size_t)token * DIM;
        float *value = values + (size_t)token * DIM;
        float phase = (float)token * 0.0017f;
        for (i = 0; i < DIM; ++i) {
            key[i] = 0.8f * random_signed()
                   + 0.3f * sinf(phase + 0.07f * (float)i);
            value[i] = 0.6f * random_signed()
                     + 0.2f * cosf(phase * 0.7f + 0.11f * (float)i);
        }
        if (token % 127 == 0) {
            key[token % DIM] *= 4.0f;
            value[(token * 3) % DIM] *= 3.0f;
        }
        CHECK_STATUS(kv_cache_append_head(cache, 0, token, 0, key, value),
                     KV_CACHE_OK);
    }

    for (token = 0; token < SEQUENCE; ++token) {
        float error;
        score_ref[token] = scalar_dot(query, keys + (size_t)token * DIM, DIM)
                         * inv_sqrt_dim;
        CHECK_STATUS(kv_cache_dot_key(cache, 0, token, 0, query,
                                      &score_i8[token]), KV_CACHE_OK);
        score_i8[token] *= inv_sqrt_dim;
        error = score_i8[token] - score_ref[token];
        score_error += (double)error * error;
        score_signal += (double)score_ref[token] * score_ref[token];
        if (token < SEQUENCE / 4) early_error += (double)error * error;
        if (token >= 3 * SEQUENCE / 4) late_error += (double)error * error;
    }
    softmax(score_ref, SEQUENCE);
    softmax(score_i8, SEQUENCE);
    for (token = 0; token < SEQUENCE; ++token) {
        const float *value = values + (size_t)token * DIM;
        for (i = 0; i < DIM; ++i)
            ctx_ref[i] += score_ref[token] * value[i];
        CHECK_STATUS(kv_cache_accumulate_value(cache, 0, token, 0,
                                               score_i8[token], ctx_i8),
                     KV_CACHE_OK);
    }
    for (i = 0; i < DIM; ++i) {
        double error = (double)ctx_i8[i] - ctx_ref[i];
        ctx_error += error * error;
        ctx_signal += (double)ctx_ref[i] * ctx_ref[i];
    }
    printf("Long-context int8: score NRMSE %.6g, context NRMSE %.6g, "
           "late/early score MSE %.3f\n",
           sqrt(score_error / (score_signal + DBL_MIN)),
           sqrt(ctx_error / (ctx_signal + DBL_MIN)),
           late_error / (early_error + DBL_MIN));
    CHECK(sqrt(score_error / (score_signal + DBL_MIN)) < 0.025);
    CHECK(sqrt(ctx_error / (ctx_signal + DBL_MIN)) < 0.025);
    CHECK(late_error / (early_error + DBL_MIN) < 2.0);

    free(ctx_i8);
    free(ctx_ref);
    free(score_i8);
    free(score_ref);
    free(query);
    free(values);
    free(keys);
    kv_cache_destroy(cache);
}

/* Header offsets are part of the documented 128-byte persistence protocol. */
enum { TEST_HDR_VERSION = 8, TEST_HDR_FLAGS = 12, TEST_HDR_KEY_ROW = 48,
       TEST_HDR_SCALE_TYPE = 30, TEST_HDR_CRC = 108, TEST_HDR_RESERVED = 112 };

static void put_u16le_test(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32le_test(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void resign_header(uint8_t header[KV_CACHE_HEADER_BYTES]) {
    uint32_t crc;
    memset(header + TEST_HDR_CRC, 0, sizeof(uint32_t));
    crc = kv_cache_crc32c(header, KV_CACHE_HEADER_BYTES);
    put_u32le_test(header + TEST_HDR_CRC, crc);
}

static void test_persistence(void) {
    kv_cache_geometry mixed = {2, 3, 7, 11, KV_CACHE_I8, KV_CACHE_F32};
    kv_cache_geometry wrong_geometry = mixed;
    kv_cache_persist_info info, decoded;
    uint8_t header[KV_CACHE_HEADER_BYTES], changed[KV_CACHE_HEADER_BYTES];
    uint8_t model_id[KV_CACHE_MODEL_ID_BYTES];
    uint8_t wrong_model_id[KV_CACHE_MODEL_ID_BYTES];
    uint8_t *payload;
    uint64_t payload_u64;
    size_t payload_bytes, i;

    CHECK(kv_cache_crc32c("123456789", 9) == UINT32_C(0xe3069283));
    CHECK(kv_cache_crc32c(NULL, 0) == 0);

    for (i = 0; i < sizeof(model_id); ++i) model_id[i] = (uint8_t)(3 * i + 1);
    memcpy(wrong_model_id, model_id, sizeof(model_id));
    wrong_model_id[17] ^= 0x80;
    memset(&info, 0, sizeof(info));
    info.format_version = KV_CACHE_FORMAT_V2;
    info.geometry = mixed;
    info.committed_tokens = 5;
    memcpy(info.model_id, model_id, sizeof(model_id));
    CHECK_STATUS(kv_cache_persist_payload_bytes(&mixed, info.committed_tokens,
                                                &payload_u64), KV_CACHE_OK);
    CHECK(payload_u64 == UINT64_C(2) * 3 * 5 * ((4 + 7) + (4 * 7)));
    CHECK(payload_u64 <= SIZE_MAX);
    payload_bytes = (size_t)payload_u64;
    payload = (uint8_t *)malloc(payload_bytes);
    CHECK(payload != NULL);
    for (i = 0; i < payload_bytes; ++i) payload[i] = (uint8_t)random_u32();
    info.payload_crc32c = kv_cache_crc32c(payload, payload_bytes);

    CHECK_STATUS(kv_cache_header_encode(&info, header), KV_CACHE_OK);
    CHECK_STATUS(kv_cache_header_decode(header, sizeof(header), &mixed,
                                        model_id, &decoded), KV_CACHE_OK);
    CHECK(decoded.format_version == KV_CACHE_FORMAT_V2);
    CHECK(decoded.committed_tokens == info.committed_tokens);
    CHECK(decoded.payload_crc32c == info.payload_crc32c);
    CHECK(memcmp(decoded.model_id, model_id, sizeof(model_id)) == 0);
    CHECK_STATUS(kv_cache_validate_payload(&decoded, payload, payload_bytes),
                 KV_CACHE_OK);
    payload[payload_bytes / 2] ^= 1;
    CHECK_STATUS(kv_cache_validate_payload(&decoded, payload, payload_bytes),
                 KV_CACHE_CORRUPT);
    payload[payload_bytes / 2] ^= 1;
    CHECK_STATUS(kv_cache_validate_payload(&decoded, payload, payload_bytes - 1),
                 KV_CACHE_BAD_FORMAT);
    CHECK_STATUS(kv_cache_validate_payload(&decoded, NULL, payload_bytes),
                 KV_CACHE_INVALID_ARGUMENT);

    CHECK_STATUS(kv_cache_header_decode(header, sizeof(header) - 1, NULL,
                                        NULL, &decoded), KV_CACHE_BAD_FORMAT);
    CHECK_STATUS(kv_cache_header_decode(NULL, sizeof(header), NULL,
                                        NULL, &decoded),
                 KV_CACHE_INVALID_ARGUMENT);
    CHECK_STATUS(kv_cache_header_decode(header, sizeof(header), NULL,
                                        NULL, NULL),
                 KV_CACHE_INVALID_ARGUMENT);
    wrong_geometry.head_dim++;
    CHECK_STATUS(kv_cache_header_decode(header, sizeof(header), &wrong_geometry,
                                        model_id, &decoded),
                 KV_CACHE_INCOMPATIBLE);
    CHECK_STATUS(kv_cache_header_decode(header, sizeof(header), &mixed,
                                        wrong_model_id, &decoded),
                 KV_CACHE_INCOMPATIBLE);

    memcpy(changed, header, sizeof(changed));
    changed[65] ^= 1;
    CHECK_STATUS(kv_cache_header_decode(changed, sizeof(changed), NULL,
                                        NULL, &decoded), KV_CACHE_CORRUPT);
    memcpy(changed, header, sizeof(changed));
    changed[0] ^= 1;
    CHECK_STATUS(kv_cache_header_decode(changed, sizeof(changed), NULL,
                                        NULL, &decoded), KV_CACHE_BAD_FORMAT);

    /* Semantically invalid fields remain invalid even with a valid checksum. */
    memcpy(changed, header, sizeof(changed));
    changed[TEST_HDR_FLAGS] = 1;
    resign_header(changed);
    CHECK_STATUS(kv_cache_header_decode(changed, sizeof(changed), NULL,
                                        NULL, &decoded), KV_CACHE_BAD_FORMAT);
    memcpy(changed, header, sizeof(changed));
    changed[TEST_HDR_RESERVED] = 1;
    resign_header(changed);
    CHECK_STATUS(kv_cache_header_decode(changed, sizeof(changed), NULL,
                                        NULL, &decoded), KV_CACHE_BAD_FORMAT);
    memcpy(changed, header, sizeof(changed));
    changed[TEST_HDR_SCALE_TYPE] = 9;
    resign_header(changed);
    CHECK_STATUS(kv_cache_header_decode(changed, sizeof(changed), NULL,
                                        NULL, &decoded),
                 KV_CACHE_INCOMPATIBLE);
    memcpy(changed, header, sizeof(changed));
    put_u16le_test(changed + TEST_HDR_VERSION, 99);
    resign_header(changed);
    CHECK_STATUS(kv_cache_header_decode(changed, sizeof(changed), NULL,
                                        NULL, &decoded),
                 KV_CACHE_INCOMPATIBLE);
    memcpy(changed, header, sizeof(changed));
    changed[TEST_HDR_KEY_ROW] ^= 1;
    resign_header(changed);
    CHECK_STATUS(kv_cache_header_decode(changed, sizeof(changed), NULL,
                                        NULL, &decoded), KV_CACHE_BAD_FORMAT);

    /* V1 round-trip and its explicit all-f32 compatibility restriction. */
    info.format_version = KV_CACHE_FORMAT_V1;
    CHECK_STATUS(kv_cache_header_encode(&info, changed),
                 KV_CACHE_INCOMPATIBLE);
    info.geometry.key_encoding = KV_CACHE_F32;
    info.geometry.value_encoding = KV_CACHE_F32;
    CHECK_STATUS(kv_cache_header_encode(&info, changed), KV_CACHE_OK);
    CHECK_STATUS(kv_cache_header_decode(changed, sizeof(changed), NULL,
                                        model_id, &decoded), KV_CACHE_OK);
    CHECK(decoded.format_version == KV_CACHE_FORMAT_V1);
    CHECK(decoded.geometry.key_encoding == KV_CACHE_F32);
    CHECK(decoded.geometry.value_encoding == KV_CACHE_F32);

    /* Every independent V2 K/V encoding combination is representable. */
    info.format_version = KV_CACHE_FORMAT_V2;
    info.committed_tokens = 0;
    info.payload_crc32c = 0;
    for (i = 0; i < 4; ++i) {
        info.geometry.key_encoding = (i & 1u) ? KV_CACHE_I8 : KV_CACHE_F32;
        info.geometry.value_encoding = (i & 2u) ? KV_CACHE_I8 : KV_CACHE_F32;
        CHECK_STATUS(kv_cache_header_encode(&info, changed), KV_CACHE_OK);
        CHECK_STATUS(kv_cache_header_decode(changed, sizeof(changed), NULL,
                                            model_id, &decoded), KV_CACHE_OK);
        CHECK(decoded.geometry.key_encoding == info.geometry.key_encoding);
        CHECK(decoded.geometry.value_encoding == info.geometry.value_encoding);
    }

    info.committed_tokens = info.geometry.capacity_tokens + 1;
    CHECK_STATUS(kv_cache_header_encode(&info, changed),
                 KV_CACHE_INVALID_ARGUMENT);
    free(payload);
}

static void test_mixed_unaligned_shelves(void) {
    /* 7-byte int8 row + 4-byte scale makes the following f32 shelf unaligned. */
    kv_cache_geometry geometry = {1, 1, 7, 1, KV_CACHE_I8, KV_CACHE_F32};
    kv_cache_status status;
    kv_cache *cache = kv_cache_create(&geometry, &status);
    float key[7], value[7], key_out[7], value_out[7], accum[7] = {0};
    uint32_t i;
    CHECK(cache != NULL && status == KV_CACHE_OK);
    for (i = 0; i < 7; ++i) {
        key[i] = (float)i - 3.25f;
        value[i] = 0.125f * (float)(i + 1);
    }
    CHECK_STATUS(kv_cache_append_head(cache, 0, 0, 0, key, value),
                 KV_CACHE_OK);
    CHECK_STATUS(kv_cache_read_head(cache, 0, 0, 0, key_out, value_out),
                 KV_CACHE_OK);
    CHECK(memcmp(value, value_out, sizeof(value)) == 0);
    CHECK_STATUS(kv_cache_accumulate_value(cache, 0, 0, 0, 2.0f, accum),
                 KV_CACHE_OK);
    for (i = 0; i < 7; ++i) CHECK(accum[i] == 2.0f * value[i]);
    kv_cache_destroy(cache);
}

int main(void) {
    test_status_and_sizing();
    test_append_order_f32_and_clear();
    test_nonfinite_zero_extremes();
    test_randomized_odd_dimensions();
    test_long_context_attention_drift();
    test_persistence();
    test_mixed_unaligned_shelves();
    printf("kv_cache: %llu checks passed\n", (unsigned long long)checks);
    return 0;
}
