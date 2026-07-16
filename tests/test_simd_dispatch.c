#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "kernels.h"

// Define a simple reference scalar dot_i8i8
static int32_t ref_dot_i8i8(const int8_t *w, const int8_t *x, int I) {
    int32_t sum = 0;
    for (int i = 0; i < I; i++) {
        sum += (int32_t)w[i] * x[i];
    }
    return sum;
}

// Define a simple reference scalar dot_i4i8
static int32_t ref_dot_i4i8(const uint8_t *w4, const int8_t *x, int I) {
    int32_t sum = 0;
    for (int i = 0; i < I; i++) {
        uint8_t b = w4[i >> 1];
        int v = (i & 1) ? (int)(b >> 4) - 8 : (int)(b & 0x0F) - 8;
        sum += v * x[i];
    }
    return sum;
}

int main(void) {
    printf("Starting SIMD dispatch test...\n");
    simd_init();

    // Verify dot_i8i8 correctness
    int8_t w8[64], x8[64];
    for (int i = 0; i < 64; i++) {
        w8[i] = (int8_t)(i - 32);
        x8[i] = (int8_t)(32 - i);
    }
    int32_t got_i8 = dot_i8i8(w8, x8, 64);
    int32_t ref_i8 = ref_dot_i8i8(w8, x8, 64);
    if (got_i8 != ref_i8) {
        fprintf(stderr, "ERROR: dot_i8i8 mismatch: got %d, expected %d\n", got_i8, ref_i8);
        return 1;
    }
    printf("dot_i8i8 test: PASS\n");

    // Verify dot_i4i8 correctness
    uint8_t w4[32];
    for (int i = 0; i < 32; i++) {
        w4[i] = (uint8_t)(i * 7);
    }
    int32_t got_i4 = dot_i4i8(w4, x8, 64);
    int32_t ref_i4 = ref_dot_i4i8(w4, x8, 64);
    if (got_i4 != ref_i4) {
        fprintf(stderr, "ERROR: dot_i4i8 mismatch: got %d, expected %d\n", got_i4, ref_i4);
        return 1;
    }
    printf("dot_i4i8 test: PASS\n");

    // Verify matmul_q correctness
    float y_q[4], xs[32];
    int8_t q[32];
    float scales[4] = {0.5f, 1.5f, 2.0f, -1.0f};
    for (int i = 0; i < 32; i++) {
        xs[i] = (float)i * 0.1f;
        q[i] = (int8_t)(i - 16);
    }
    // We run it for S=1, I=8, O=4
    matmul_q(y_q, xs, q, scales, 1, 8, 4);
    // Reference check
    for (int o = 0; o < 4; o++) {
        float sum = 0;
        for (int i = 0; i < 8; i++) {
            sum += xs[i] * q[o * 8 + i];
        }
        float expected = sum * scales[o];
        if (fabsf(y_q[o] - expected) > 1e-4f) {
            fprintf(stderr, "ERROR: matmul_q mismatch at %d: got %f, expected %f\n", o, y_q[o], expected);
            return 1;
        }
    }
    printf("matmul_q test: PASS\n");

    // Verify matmul_i4 correctness
    float y_i4[4];
    uint8_t q4[16];
    for (int i = 0; i < 16; i++) {
        q4[i] = (uint8_t)(i * 11);
    }
    matmul_i4(y_i4, xs, q4, scales, 1, 8, 4);
    for (int o = 0; o < 4; o++) {
        float sum = 0;
        for (int i = 0; i < 8; i++) {
            uint8_t byte = q4[o * 4 + (i >> 1)];
            int v = (i & 1) ? (int)(byte >> 4) - 8 : (int)(byte & 0x0F) - 8;
            sum += xs[i] * v;
        }
        float expected = sum * scales[o];
        if (fabsf(y_i4[o] - expected) > 1e-4f) {
            fprintf(stderr, "ERROR: matmul_i4 mismatch at %d: got %f, expected %f\n", o, y_i4[o], expected);
            return 1;
        }
    }
    printf("matmul_i4 test: PASS\n");

    printf("All SIMD dispatch tests passed successfully.\n");
    return 0;
}
