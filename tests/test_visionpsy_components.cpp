#include "visionpsy/visionpsy_model.h"
#include "mlx/mlx.h"

#include <iostream>
#include <cassert>
#include <cmath>
#include <vector>

using namespace samosa::visionpsy;
using namespace mlx::core;

void test_pixel_shuffle_shapes() {
    std::cout << "Testing pixel-shuffle projector tensor shapes..." << std::endl;
    // Input features: [1, 1024, 768] representing 32x32 grid
    int grid_size = 32;
    int factor = 4;
    int hidden_size = 768;
    int out_grid = grid_size / factor; // 8

    auto x = zeros({1, grid_size * grid_size, hidden_size}, float32);
    auto feat = reshape(x, {1, out_grid, factor, out_grid, factor, hidden_size});
    feat = transpose(feat, {0, 1, 3, 2, 4, 5});
    feat = reshape(feat, {1, out_grid * out_grid, factor * factor * hidden_size});

    assert(feat.shape(0) == 1);
    assert(feat.shape(1) == 64);
    assert(feat.shape(2) == 12288);

    // Linear projection weight: [960, 12288]
    auto proj_w = zeros({960, 12288}, float32);
    auto visual_embeds = matmul(feat, transpose(proj_w, {1, 0}));
    assert(visual_embeds.shape(0) == 1);
    assert(visual_embeds.shape(1) == 64);
    assert(visual_embeds.shape(2) == 960);

    std::cout << "Pixel-shuffle test: PASS" << std::endl;
}

void test_dynamic_resize_and_tiling() {
    std::cout << "Testing aspect-preserving DynamicResize and tile generation..." << std::endl;
    VisionPsyModel model;

    // 1. Create a 16:9 synthetic PPM image (1920x1080)
    int orig_w = 1920, orig_h = 1080;
    std::vector<unsigned char> ppm_16_9;
    std::string header = "P6\n" + std::to_string(orig_w) + " " + std::to_string(orig_h) + "\n255\n";
    ppm_16_9.insert(ppm_16_9.end(), header.begin(), header.end());
    ppm_16_9.resize(ppm_16_9.size() + (size_t)orig_w * orig_h * 3, 200);

    // Test header inspection
    int insp_w = 0, insp_h = 0;
    assert(model.inspect_image_header(ppm_16_9.data(), ppm_16_9.size(), &insp_w, &insp_h));
    assert(insp_w == 1920);
    assert(insp_h == 1080);

    // Test standard budget (max_side_len = 2048) -> long=2048, short=1536 (n_w=4, n_h=3)
    ImageTiles tiles_std;
    assert(model.preprocess_image_tiles(ppm_16_9.data(), ppm_16_9.size(), 2048, &tiles_std));
    assert(tiles_std.n_w == 4);
    assert(tiles_std.n_h == 3);
    assert(tiles_std.has_global == true);
    // 1 global tile + 12 positional tiles = 13 tiles
    assert(tiles_std.tiles.size() == 13);
    for (const auto& tile : tiles_std.tiles) {
        assert(tile.shape(0) == 1);
        assert(tile.shape(1) == 512);
        assert(tile.shape(2) == 512);
        assert(tile.shape(3) == 3);
    }

    // Verify pixel values in [0.0, 1.0]
    auto first_tile = tiles_std.tiles[0];
    eval(first_tile);
    float val = first_tile.data<float>()[0];
    assert(val >= 0.0f && val <= 1.0f);
    assert(std::abs(val - (200.0f / 255.0f)) < 0.02f);

    // Test memory-pressured budget (max_side_len = 1024) -> long=1024, short=1024 (n_w=2, n_h=2)
    ImageTiles tiles_med;
    assert(model.preprocess_image_tiles(ppm_16_9.data(), ppm_16_9.size(), 1024, &tiles_med));
    assert(tiles_med.n_w == 2);
    assert(tiles_med.has_global == true);
    // 1 global tile + 4 positional tiles = 5 tiles
    assert(tiles_med.tiles.size() == 5);

    // Test single-tile budget (max_side_len = 512)
    ImageTiles tiles_min;
    assert(model.preprocess_image_tiles(ppm_16_9.data(), ppm_16_9.size(), 512, &tiles_min));
    assert(tiles_min.n_w == 1);
    assert(tiles_min.n_h == 1);
    assert(tiles_min.has_global == false);
    assert(tiles_min.tiles.size() == 1);

    std::cout << "DynamicResize and tiling test: PASS" << std::endl;
}

void test_decompression_safety() {
    std::cout << "Testing decompression safety limits..." << std::endl;
    VisionPsyModel model;

    // Header indicating impossible or dangerous dimensions
    std::string bad_header = "P6\n20000 20000\n255\n"; // 400 megapixels > 64 megapixels limit
    std::vector<unsigned char> bad_data(bad_header.begin(), bad_header.end());
    int w = 0, h = 0;
    assert(!model.inspect_image_header(bad_data.data(), bad_data.size(), &w, &h));

    ImageTiles bad_tiles;
    assert(!model.preprocess_image_tiles(bad_data.data(), bad_data.size(), 2048, &bad_tiles));

    std::cout << "Decompression safety test: PASS" << std::endl;
}

int main() {
    test_pixel_shuffle_shapes();
    test_dynamic_resize_and_tiling();
    test_decompression_safety();
    std::cout << "All VisionPsy component tests passed!" << std::endl;
    return 0;
}
