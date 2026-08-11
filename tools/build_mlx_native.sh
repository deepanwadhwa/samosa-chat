#!/bin/sh
set -e

MLX_VERSION=$(cat vendor/mlx.version)
BUILD_DIR="${BUILD_DIR:-build}"
MLX_BUILD_DIR="$BUILD_DIR/mlx-build"

[ "$(uname -s):$(uname -m)" = "Darwin:arm64" ] || {
    echo "Maple's native MLX runtime requires Apple Silicon" >&2
    exit 2
}
[ -f vendor/mlx/CMakeLists.txt ] || {
    echo "missing MLX submodule; run: git submodule update --init vendor/mlx" >&2
    exit 2
}

MLX_ACTUAL=$(git -C vendor/mlx rev-parse HEAD)
[ "$MLX_ACTUAL" = "$MLX_VERSION" ] || {
    echo "MLX submodule is $MLX_ACTUAL, expected pinned revision $MLX_VERSION" >&2
    exit 2
}

echo "Building native MLX (Python bindings OFF)"
cmake -S vendor/mlx -B "$MLX_BUILD_DIR" \
    -DMLX_BUILD_PYTHON_BINDINGS=OFF \
    -DMLX_BUILD_METAL=ON \
    -DMLX_BUILD_SAFETENSORS=ON \
    -DMLX_BUILD_GGUF=OFF \
    -DMLX_BUILD_TESTS=OFF \
    -DCMAKE_BUILD_TYPE=Release

cmake --build "$MLX_BUILD_DIR" --parallel "$(sysctl -n hw.ncpu)"

echo "MLX Native Build Complete."
