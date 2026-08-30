#ifndef SAMOSA_MOLMO2_PROCESSOR_H
#define SAMOSA_MOLMO2_PROCESSOR_H

#include <cstdint>
#include <string>
#include <vector>

namespace samosa::molmo2 {

struct RgbImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

struct VisualInput {
    /* [crop, 729, 588], patch-major RGB, normalized to [-1, 1]. */
    std::vector<float> patches;
    int crop_count = 0;
    /* One row per <im_patch>; image rows have 4 source patches and video rows
       have 9. Indices address [crop, 729] after flattening. */
    std::vector<std::int32_t> pooling;
    int pooling_width = 0;
    std::string token_text;
    int patch_token_count = 0;
    bool is_video = false;
    /* Multiple images share one decoder sequence but are vision-encoded one
       at a time to keep Metal memory at the single-image peak. */
    std::vector<int> segment_crop_counts;
    std::vector<int> segment_patch_token_counts;
};

bool decode_image(const std::string& path, RgbImage* output, std::string* error);
bool preprocess_image(const RgbImage& image, VisualInput* output, std::string* error);
/* Molmo2's native chat template labels multiple inputs as Image 1, Image 2,
   and so on before expanding each visual placeholder.  Keep the local 2,048
   token safety envelope by sharing the high-resolution crop budget across
   exactly two images; every image still retains both its global and tiled
   visual streams. */
bool preprocess_images(const std::vector<RgbImage>& images,
                       VisualInput* output,
                       std::string* error);
bool preprocess_video_frames(const std::vector<RgbImage>& frames,
                             const std::vector<double>& timestamps,
                             VisualInput* output,
                             std::string* error);

}  // namespace samosa::molmo2

#endif
