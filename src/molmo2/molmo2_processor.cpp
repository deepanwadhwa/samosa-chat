#include "molmo2_processor.h"

#define STB_IMAGE_IMPLEMENTATION
#include "../stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <utility>

namespace samosa::molmo2 {
namespace {

constexpr int kSide = 378;
constexpr int kPatch = 14;
constexpr int kPatches = 27;
constexpr int kPixelsPerPatch = 14 * 14 * 3;
constexpr int kOverlap = 4;
constexpr int kWindowPatches = 19;
constexpr int kWindowPixels = kWindowPatches * kPatch;
constexpr int kMaxCrops = 8;
constexpr int kMaxDimension = 16384;
constexpr std::uint64_t kMaxDecodedPixels = 64ULL * 1024 * 1024;

bool valid_image(const RgbImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.width > kMaxDimension ||
        image.height > kMaxDimension) return false;
    const std::uint64_t count = static_cast<std::uint64_t>(image.width) * image.height;
    return count <= kMaxDecodedPixels && image.pixels.size() == count * 3;
}

std::vector<float> resize_bilinear(const RgbImage& image, int out_w, int out_h) {
    std::vector<float> result(static_cast<std::size_t>(out_w) * out_h * 3);
    const float xr = static_cast<float>(image.width) / out_w;
    const float yr = static_cast<float>(image.height) / out_h;
    for (int y = 0; y < out_h; ++y) {
        const float fy = (y + 0.5f) * yr - 0.5f;
        const int raw_y0 = static_cast<int>(std::floor(fy));
        const int y0 = std::clamp(raw_y0, 0, image.height - 1);
        const int y1 = std::clamp(raw_y0 + 1, 0, image.height - 1);
        const float wy = std::clamp(fy - std::floor(fy), 0.0f, 1.0f);
        for (int x = 0; x < out_w; ++x) {
            const float fx = (x + 0.5f) * xr - 0.5f;
            const int raw_x0 = static_cast<int>(std::floor(fx));
            const int x0 = std::clamp(raw_x0, 0, image.width - 1);
            const int x1 = std::clamp(raw_x0 + 1, 0, image.width - 1);
            const float wx = std::clamp(fx - std::floor(fx), 0.0f, 1.0f);
            for (int c = 0; c < 3; ++c) {
                const float a = image.pixels[(static_cast<std::size_t>(y0) * image.width + x0) * 3 + c];
                const float b = image.pixels[(static_cast<std::size_t>(y0) * image.width + x1) * 3 + c];
                const float d = image.pixels[(static_cast<std::size_t>(y1) * image.width + x0) * 3 + c];
                const float e = image.pixels[(static_cast<std::size_t>(y1) * image.width + x1) * 3 + c];
                const float top = a + wx * (b - a);
                const float bottom = d + wx * (e - d);
                // mean=.5/std=.5 after [0,255] -> [0,1].
                result[(static_cast<std::size_t>(y) * out_w + x) * 3 + c] =
                    2.0f * ((top + wy * (bottom - top)) / 255.0f) - 1.0f;
            }
        }
    }
    return result;
}

std::pair<int, int> select_tiling(int height, int width, int max_crops) {
    const float h = static_cast<float>(std::max(1, height));
    const float w = static_cast<float>(std::max(1, width));
    std::vector<std::pair<int, int>> candidates;
    for (int rows = 1; rows <= max_crops; ++rows)
        for (int cols = 1; cols <= max_crops; ++cols)
            if (rows * cols <= max_crops) candidates.emplace_back(rows, cols);
    std::sort(candidates.begin(), candidates.end(), [](auto a, auto b) {
        if (a.first * a.second != b.first * b.second)
            return a.first * a.second < b.first * b.second;
        return a.first < b.first;
    });
    std::vector<float> scales;
    bool all_downscale = true;
    for (auto [rows, cols] : candidates) {
        const float scale = std::min(rows * kWindowPixels / h, cols * kWindowPixels / w);
        scales.push_back(scale);
        if (scale >= 1.0f) all_downscale = false;
    }
    std::size_t best = 0;
    if (all_downscale) {
        for (std::size_t i = 1; i < scales.size(); ++i) if (scales[i] > scales[best]) best = i;
    } else {
        float score = std::numeric_limits<float>::infinity();
        for (std::size_t i = 0; i < scales.size(); ++i) {
            if (scales[i] >= 1.0f && scales[i] < score) { score = scales[i]; best = i; }
        }
    }
    return candidates[best];
}

void append_patches(const std::vector<float>& image, int width, int height,
                    int x0, int y0, std::vector<float>* output) {
    if (x0 < 0 || y0 < 0 || x0 + kSide > width || y0 + kSide > height)
        throw std::runtime_error("internal Molmo2 crop bounds error");
    output->reserve(output->size() + static_cast<std::size_t>(kPatches * kPatches * kPixelsPerPatch));
    for (int py = 0; py < kPatches; ++py) {
        for (int px = 0; px < kPatches; ++px) {
            for (int iy = 0; iy < kPatch; ++iy) {
                const int y = y0 + py * kPatch + iy;
                for (int ix = 0; ix < kPatch; ++ix) {
                    const int x = x0 + px * kPatch + ix;
                    const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 3;
                    output->push_back(image[at]);
                    output->push_back(image[at + 1]);
                    output->push_back(image[at + 2]);
                }
            }
        }
    }
}

std::vector<std::int32_t> pool_grid(const std::vector<std::int32_t>& grid,
                                    int height, int width, int pool) {
    const int padded_h = ((height + pool - 1) / pool) * pool;
    const int padded_w = ((width + pool - 1) / pool) * pool;
    const int top = (padded_h - height) / 2;
    const int left = (padded_w - width) / 2;
    std::vector<std::int32_t> result;
    result.reserve(static_cast<std::size_t>(padded_h / pool) * (padded_w / pool) * pool * pool);
    for (int y = 0; y < padded_h; y += pool) {
        for (int x = 0; x < padded_w; x += pool) {
            for (int dy = 0; dy < pool; ++dy) {
                for (int dx = 0; dx < pool; ++dx) {
                    const int sy = y + dy - top, sx = x + dx - left;
                    result.push_back(sy >= 0 && sy < height && sx >= 0 && sx < width
                                         ? grid[static_cast<std::size_t>(sy) * width + sx] : -1);
                }
            }
        }
    }
    return result;
}

void append_image_token_grid(std::string* text, const char* start,
                             int height, int width, bool columns) {
    *text += start;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) *text += "<im_patch>";
        if (columns) *text += "<im_col>";
    }
    *text += "<im_end>";
}

}  // namespace

bool decode_image(const std::string& path, RgbImage* output, std::string* error) {
    if (!output || path.empty() || path[0] != '/') {
        if (error) *error = "invalid image request";
        return false;
    }
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        static_cast<std::uint64_t>(st.st_size) > 4ULL * 1024 * 1024 * 1024) {
        if (error) *error = "image path is missing, not regular, is a symlink, or exceeds 4 GiB";
        return false;
    }
    int width = 0, height = 0, channels = 0;
    if (!stbi_info(path.c_str(), &width, &height, &channels) || width <= 0 || height <= 0 ||
        width > kMaxDimension || height > kMaxDimension ||
        static_cast<std::uint64_t>(width) * height > kMaxDecodedPixels) {
        if (error) *error = "image header is invalid or exceeds the 64-megapixel safety limit";
        return false;
    }
    unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, 3);
    if (!pixels) { if (error) *error = "image decode failed"; return false; }
    output->width = width;
    output->height = height;
    output->pixels.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 3);
    stbi_image_free(pixels);
    if (error) error->clear();
    return true;
}

static bool preprocess_image_with_crop_limit(const RgbImage& image, int max_crops,
                                             VisualInput* output, std::string* error) {
    if (max_crops < 1 || max_crops > kMaxCrops) {
        if (error) *error = "invalid Molmo2 image crop limit";
        return false;
    }
    if (!output || !valid_image(image)) { if (error) *error = "invalid RGB image"; return false; }
    try {
        VisualInput result;
        result.pooling_width = 4;
        auto global = resize_bilinear(image, kSide, kSide);
        append_patches(global, kSide, kSide, 0, 0, &result.patches);

        const auto [rows, cols] = select_tiling(image.height - 2 * kOverlap * kPatch,
                                                image.width - 2 * kOverlap * kPatch,
                                                max_crops);
        const int resized_h = rows * kWindowPixels + 2 * kOverlap * kPatch;
        const int resized_w = cols * kWindowPixels + 2 * kOverlap * kPatch;
        auto high = resize_bilinear(image, resized_w, resized_h);
        for (int row = 0; row < rows; ++row)
            for (int col = 0; col < cols; ++col)
                append_patches(high, resized_w, resized_h,
                               col * kWindowPixels, row * kWindowPixels, &result.patches);
        result.crop_count = 1 + rows * cols;

        std::vector<std::int32_t> low(kPatches * kPatches);
        for (int i = 0; i < kPatches * kPatches; ++i) low[i] = i;
        auto low_pool = pool_grid(low, kPatches, kPatches, 2);

        const int high_h = rows * kWindowPatches + 2 * kOverlap;
        const int high_w = cols * kWindowPatches + 2 * kOverlap;
        std::vector<std::int32_t> high_map(static_cast<std::size_t>(high_h) * high_w, -1);
        for (int row = 0; row < rows; ++row) {
            for (int col = 0; col < cols; ++col) {
                const int crop = row * cols + col;
                for (int py = 0; py < kPatches; ++py) {
                    for (int px = 0; px < kPatches; ++px) {
                        if ((row && py < kOverlap) || (col && px < kOverlap) ||
                            (row + 1 < rows && py >= kPatches - kOverlap) ||
                            (col + 1 < cols && px >= kPatches - kOverlap)) continue;
                        const int gy = row * kWindowPatches + py;
                        const int gx = col * kWindowPatches + px;
                        high_map[static_cast<std::size_t>(gy) * high_w + gx] =
                            kPatches * kPatches + crop * kPatches * kPatches + py * kPatches + px;
                    }
                }
            }
        }
        if (std::find(high_map.begin(), high_map.end(), -1) != high_map.end())
            throw std::runtime_error("Molmo2 overlap map left an uncovered patch");
        auto high_pool = pool_grid(high_map, high_h, high_w, 2);
        result.pooling = std::move(low_pool);
        result.pooling.insert(result.pooling.end(), high_pool.begin(), high_pool.end());
        const int low_h = (kPatches + 1) / 2, low_w = (kPatches + 1) / 2;
        const int pooled_h = (high_h + 1) / 2, pooled_w = (high_w + 1) / 2;
        /* Pinned processor_config.json: image_use_col_tokens=true applies to
           the high-resolution grid, while use_single_crop_col_tokens=false
           means the leading global/low-resolution crop has no row markers. */
        append_image_token_grid(&result.token_text, "<low_res_im_start>", low_h, low_w, false);
        append_image_token_grid(&result.token_text, "<im_start>", pooled_h, pooled_w, true);
        result.patch_token_count = low_h * low_w + pooled_h * pooled_w;
        if (static_cast<int>(result.pooling.size()) != result.patch_token_count * 4)
            throw std::runtime_error("Molmo2 image token/pooling mismatch");
        result.segment_crop_counts.push_back(result.crop_count);
        result.segment_patch_token_counts.push_back(result.patch_token_count);
        *output = std::move(result);
        if (error) error->clear();
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}

bool preprocess_image(const RgbImage& image, VisualInput* output, std::string* error) {
    return preprocess_image_with_crop_limit(image, kMaxCrops, output, error);
}

bool preprocess_images(const std::vector<RgbImage>& images,
                       VisualInput* output, std::string* error) {
    if (!output || images.size() != 2) {
        if (error) *error = "Molmo2 joint image batches require exactly two images";
        return false;
    }
    try {
        VisualInput result;
        result.pooling_width = 4;
        /* Each image gets one detailed crop in addition to its global crop.
           Four ViT crops total preserve both native visual streams while
           keeping joint prefill below the host-pressure transition observed
           with six crops on the qualified 16 GiB machine. */
        const int high_resolution_crops = 1;
        for (std::size_t image_index = 0; image_index < images.size(); ++image_index) {
            VisualInput current;
            std::string current_error;
            if (!preprocess_image_with_crop_limit(images[image_index],
                                                  high_resolution_crops,
                                                  &current, &current_error))
                throw std::runtime_error(current_error.empty()
                                             ? "Molmo2 joint image preprocessing failed"
                                             : current_error);
            const std::int32_t patch_offset = result.crop_count * kPatches * kPatches;
            result.patches.insert(result.patches.end(),
                                  current.patches.begin(), current.patches.end());
            for (std::int32_t index : current.pooling)
                result.pooling.push_back(index < 0 ? -1 : index + patch_offset);
            result.token_text += "Image " + std::to_string(image_index + 1);
            result.token_text += current.token_text;
            result.crop_count += current.crop_count;
            result.patch_token_count += current.patch_token_count;
            result.segment_crop_counts.push_back(current.crop_count);
            result.segment_patch_token_counts.push_back(current.patch_token_count);
        }
        if (result.crop_count > 9 ||
            result.patches.size() != static_cast<std::size_t>(result.crop_count) *
                                         kPatches * kPatches * kPixelsPerPatch ||
            result.pooling.size() != static_cast<std::size_t>(result.patch_token_count * 4))
            throw std::runtime_error("Molmo2 joint image token/pooling mismatch");
        *output = std::move(result);
        if (error) error->clear();
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}

bool preprocess_video_frames(const std::vector<RgbImage>& frames,
                             const std::vector<double>& timestamps,
                             VisualInput* output, std::string* error) {
    if (!output || frames.empty() || frames.size() != timestamps.size() || frames.size() > 16) {
        if (error) *error = "video frame batch is invalid"; return false;
    }
    try {
        VisualInput result;
        result.is_video = true;
        result.pooling_width = 9;
        std::vector<std::int32_t> local(kPatches * kPatches);
        for (int i = 0; i < kPatches * kPatches; ++i) local[i] = i;
        const auto local_pool = pool_grid(local, kPatches, kPatches, 3);
        for (std::size_t frame = 0; frame < frames.size(); ++frame) {
            if (!valid_image(frames[frame]) || !std::isfinite(timestamps[frame]) || timestamps[frame] < 0)
                throw std::runtime_error("video contains an invalid frame or timestamp");
            auto resized = resize_bilinear(frames[frame], kSide, kSide);
            append_patches(resized, kSide, kSide, 0, 0, &result.patches);
            for (std::int32_t index : local_pool)
                result.pooling.push_back(index < 0 ? -1 : index + static_cast<int>(frame) * kPatches * kPatches);
            char timestamp[64];
            std::snprintf(timestamp, sizeof(timestamp), frame ? " %.1f " : "%.1f ", timestamps[frame]);
            result.token_text += timestamp;
            append_image_token_grid(&result.token_text, "<im_start>", 9, 9, false);
        }
        result.crop_count = static_cast<int>(frames.size());
        result.patch_token_count = static_cast<int>(frames.size()) * 81;
        if (static_cast<int>(result.pooling.size()) != result.patch_token_count * 9)
            throw std::runtime_error("Molmo2 video token/pooling mismatch");
        result.segment_crop_counts.push_back(result.crop_count);
        result.segment_patch_token_counts.push_back(result.patch_token_count);
        *output = std::move(result);
        if (error) error->clear();
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}

}  // namespace samosa::molmo2
