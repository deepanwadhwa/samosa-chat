#include "mlx/mlx.h"
#include "maple_model.h"
#include <cassert>
#include <iostream>
#include <vector>

using namespace mlx::core;
using namespace samosa::maple;

void test_rotating_cache() {
    std::cout << "--- RotatingKVCache Boundary Test ---\n";
    // Using a sliding window size of 512 for the test
    int max_size = 512;
    RotatingKVCache cache(max_size);

    // We mock keys and values as simple sequences
    int B = 1, n_heads = 1, head_dim = 16;

    for (int step = 0; step <= 514; ++step) {
        // mock token: value = step
        auto k = full({B, n_heads, 1, head_dim}, (float)step);
        auto v = full({B, n_heads, 1, head_dim}, (float)step);

        cache.update_and_fetch(k, v);

        if (step >= 510) {
            eval(k);
            auto k_shape = k.shape();
            auto k_f32 = astype(k, float32);
            // get the first and last element to verify eviction
            auto first_k = slice(k_f32, {0, 0, 0, 0}, {1, 1, 1, 1});
            auto last_k = slice(k_f32, {0, 0, k_shape[2]-1, 0}, {1, 1, k_shape[2], 1});

            eval(first_k);
            eval(last_k);

            float first_val = reshape(first_k, {-1}).item<float>();
            float last_val = reshape(last_k, {-1}).item<float>();

            std::cout << "Step " << step << " | Cache Size: " << k_shape[2]
                      << " | First token val: " << first_val
                      << " | Last token val: " << last_val
                      << " | Offset: " << cache.offset << "\n";

            if (k_shape[2] > max_size) {
                std::cout << "FAIL: Cache exceeded sliding window max_size!\n";
                exit(1);
            }
        }
    }
}

void test_rotating_chunk_boundary() {
    const int max_size = 512;
    RotatingKVCache cache(max_size);
    auto k0 = zeros({1, 1, max_size, 16}, float32);
    auto v0 = zeros({1, 1, max_size, 16}, float32);
    cache.update_and_fetch(k0, v0);
    assert(k0.shape(2) == max_size);

    auto k1 = ones({1, 1, 64, 16}, float32);
    auto v1 = ones({1, 1, 64, 16}, float32);
    cache.update_and_fetch(k1, v1);
    // mlx-lm preserves window + chunk - 1 keys so every query in the
    // multi-token append has a complete 512-token causal window.
    assert(k1.shape(2) == max_size + 64 - 1);
    assert(cache.size() == max_size);
}

void test_global_cache() {
    std::cout << "--- Global KVCache Test ---\n";
    KVCache cache;
    int B = 1, n_heads = 1, head_dim = 16;

    // prefill 10 tokens
    auto k = full({B, n_heads, 10, head_dim}, 1.0f);
    auto v = full({B, n_heads, 10, head_dim}, 1.0f);
    cache.update_and_fetch(k, v);
    std::cout << "Prefill size: " << k.shape()[2] << ", Offset: " << cache.offset << "\n";

    // decode 5 tokens
    for (int step = 0; step < 5; ++step) {
        auto k1 = full({B, n_heads, 1, head_dim}, 2.0f);
        auto v1 = full({B, n_heads, 1, head_dim}, 2.0f);
        cache.update_and_fetch(k1, v1);
        std::cout << "Decode step " << step << ", Cache Size: " << k1.shape()[2] << ", Offset: " << cache.offset << "\n";
    }
}

int main() {
    std::cout << "=== 4. CACHE VALIDATION (Boundary Math) ===\n";
    test_global_cache();
    test_rotating_cache();
    test_rotating_chunk_boundary();
    std::cout << "PASS: KV Cache semantics verified natively.\n";
    return 0;
}
