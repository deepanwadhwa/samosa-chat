#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "kernels.h"

static void require_close(float actual, float expected, const char *what) {
    float tolerance = 1e-5f * fmaxf(1.f, fabsf(expected));
    if (fabsf(actual - expected) > tolerance) {
        fprintf(stderr, "groupwise q4: %s: got %.9g expected %.9g\n",
                what, actual, expected);
        exit(1);
    }
}

static int code_at(const uint8_t *packed, int index) {
    uint8_t byte = packed[index >> 1];
    return (index & 1) ? (int)(byte >> 4) - 8 : (int)(byte & 15) - 8;
}

int main(void) {
    enum { O = 2, I = 64, S = 2, GROUP = 32 };
    uint8_t packed[O * I / 2] = {0};
    float scales[O * (I / GROUP)] = {0.125f, 0.75f, 1.5f, 0.0625f};
    float input[S * I];

    for (int o = 0; o < O; ++o) {
        for (int i = 0; i < I; i += 2) {
            int lo = ((i + 3 * o) % 16) - 8;
            int hi = ((i + 1 + 3 * o) % 16) - 8;
            packed[o * (I / 2) + i / 2] =
                (uint8_t)((lo + 8) | ((hi + 8) << 4));
        }
    }
    for (int s = 0; s < S; ++s)
        for (int i = 0; i < I; ++i)
            input[s * I + i] = (float)(((i * 7 + s * 5) % 23) - 11) / 8.f;

    float actual[S * O];
    matmul_i4_grouped(actual, input, packed, scales, GROUP, S, I, O);
    for (int s = 0; s < S; ++s) {
        for (int o = 0; o < O; ++o) {
            float expected = 0.f;
            for (int i = 0; i < I; ++i)
                expected += input[s * I + i] *
                            (float)code_at(packed + o * (I / 2), i) *
                            scales[o * (I / GROUP) + i / GROUP];
            require_close(actual[s * O + o], expected, "float activation");
        }
    }

    int8_t input_q[S * I];
    float input_scales[S];
    for (int s = 0; s < S; ++s)
        input_scales[s] = qrow_i8(input + s * I, input_q + s * I, I);
    matmul_i4_grouped_idot(actual, input_q, input_scales, packed, scales,
                           GROUP, S, I, O);
    for (int s = 0; s < S; ++s) {
        for (int o = 0; o < O; ++o) {
            float expected = 0.f;
            for (int i = 0; i < I; ++i)
                expected += (float)input_q[s * I + i] *
                            (float)code_at(packed + o * (I / 2), i) *
                            scales[o * (I / GROUP) + i / GROUP] * input_scales[s];
            require_close(actual[s * O + o], expected, "int8 activation");
        }
    }

    QT tensor = {.fmt = 4, .O = O, .I = I, .qgroup = GROUP};
    require_close((float)qt_bytes(&tensor),
                  (float)(sizeof(packed) + sizeof(scales)), "storage bytes");

    /* Mixed expert candidate: its down projection is ordinary row-q8 and
     * therefore reuses the established float and integer-dot kernels. */
    enum { DOWN_O = 3, DOWN_I = 16 };
    int8_t down_q[DOWN_O * DOWN_I];
    float down_scales[DOWN_O] = {0.03125f, 0.125f, 0.5f};
    float down_input[DOWN_I], down_actual[DOWN_O];
    for (int i = 0; i < DOWN_I; ++i) down_input[i] = (float)(i - 7) / 9.f;
    for (int o = 0; o < DOWN_O; ++o)
        for (int i = 0; i < DOWN_I; ++i)
            down_q[o * DOWN_I + i] = (int8_t)(((o * 13 + i * 7) % 41) - 20);
    matmul_q(down_actual, down_input, down_q, down_scales, 1, DOWN_I, DOWN_O);
    for (int o = 0; o < DOWN_O; ++o) {
        float expected = 0.f;
        for (int i = 0; i < DOWN_I; ++i)
            expected += down_input[i] * (float)down_q[o * DOWN_I + i] * down_scales[o];
        require_close(down_actual[o], expected, "mixed q8-down float activation");
    }
    int8_t down_input_q[DOWN_I]; float down_input_scale;
    down_input_scale = qrow_i8(down_input, down_input_q, DOWN_I);
    matmul_q_idot(down_actual, down_input_q, &down_input_scale,
                  down_q, down_scales, 1, DOWN_I, DOWN_O);
    for (int o = 0; o < DOWN_O; ++o) {
        int expected_dot = 0;
        for (int i = 0; i < DOWN_I; ++i)
            expected_dot += (int)down_input_q[i] * (int)down_q[o * DOWN_I + i];
        require_close(down_actual[o], (float)expected_dot * down_input_scale * down_scales[o],
                      "mixed q8-down int8 activation");
    }
    QT down_tensor = {.fmt = 1, .O = DOWN_O, .I = DOWN_I};
    require_close((float)qt_bytes(&down_tensor),
                  (float)(sizeof(down_q) + sizeof(down_scales)),
                  "mixed q8-down storage bytes");
    puts("groupwise q4 and mixed q8-down tests: ok");
    return 0;
}
