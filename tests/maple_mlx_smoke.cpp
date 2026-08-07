#include "mlx/mlx.h"
#include "mlx/fast.h"
#include <iostream>

using namespace mlx::core;

int main() {
    // 1. Create two MLX arrays
    auto a = array({1.0f, 2.0f, 3.0f, 4.0f}, {2, 2});
    auto b = array({1.0f, 0.0f, 0.0f, 1.0f}, {2, 2});

    // 2. Perform matrix multiplication
    auto c = matmul(a, b);

    // 3. Execute on the Metal backend (by evaluating)
    eval(c);

    // 4. Call a trivial custom Metal kernel
    std::string source = R"(
        uint elem = thread_position_in_grid.x;
        out[elem] = inp[elem] * 2.0;
    )";

    auto custom_k = mlx::core::fast::metal_kernel(
        "double_it",
        {"inp"},
        {"out"},
        source
    );

    auto out_shapes = std::vector<Shape>{c.shape()};
    auto out_dtypes = std::vector<Dtype>{c.dtype()};
    auto grid = std::make_tuple((int)c.size(), 1, 1);
    auto threadgroup = std::make_tuple(1, 1, 1);
    
    // custom_k returns a vector of arrays
    auto res = custom_k(
        {c}, out_shapes, out_dtypes, grid, threadgroup,
        {}, std::nullopt, false, default_stream(default_device())
    )[0];

    // 5. Evaluate the result
    eval(res);

    std::cout << "Smoke test passed. Matrix multiplication and Metal custom kernel executed." << std::endl;

    // 6. Exit normally
    return 0;
}
