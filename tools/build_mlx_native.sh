#!/bin/sh
set -e

MLX_VERSION=$(cat vendor/mlx.version)
BUILD_DIR="${BUILD_DIR:-build}"
MLX_BUILD_DIR="$BUILD_DIR/mlx-build"

if [ ! -d "vendor/mlx" ]; then
    echo "Cloning MLX..."
    git clone https://github.com/ml-explore/mlx.git vendor/mlx
fi

echo "Checking out pinned MLX version $MLX_VERSION"
cd vendor/mlx
git fetch --tags
git checkout "$MLX_VERSION"
cd ../..

echo "Building native MLX (Python bindings OFF)"
mkdir -p "$MLX_BUILD_DIR"
cd "$MLX_BUILD_DIR"

cmake ../../vendor/mlx \
    -DMLX_BUILD_PYTHON_BINDINGS=OFF \
    -DMLX_BUILD_METAL=ON \
    -DMLX_BUILD_SAFETENSORS=ON \
    -DMLX_BUILD_GGUF=OFF \
    -DMLX_BUILD_TESTS=OFF \
    -DCMAKE_BUILD_TYPE=Release

make -j$(sysctl -n hw.ncpu)
cd ../..

echo "MLX Native Build Complete."
