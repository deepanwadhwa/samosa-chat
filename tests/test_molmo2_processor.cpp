#include "molmo2/molmo2_processor.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace samosa::molmo2;

static void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << "\n"; std::exit(1); }
}

static RgbImage image(int width, int height) {
    RgbImage result;
    result.width = width; result.height = height;
    result.pixels.resize(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const auto at = (static_cast<std::size_t>(y) * width + x) * 3;
        result.pixels[at] = static_cast<unsigned char>(x % 256);
        result.pixels[at + 1] = static_cast<unsigned char>(y % 256);
        result.pixels[at + 2] = 127;
    }
    return result;
}

int main() {
    std::string error;
    VisualInput square;
    check(preprocess_image(image(640, 640), &square, &error), error.c_str());
    check(!square.is_video && square.crop_count >= 2 && square.crop_count <= 9, "image crop bound");
    check(square.pooling_width == 4, "2x2 image pooling");
    check(square.pooling.size() == static_cast<std::size_t>(square.patch_token_count * 4),
          "image token/pool parity");
    check(square.patches.size() == static_cast<std::size_t>(square.crop_count) * 729 * 588,
          "image patch shape");
    check(square.token_text.find("<low_res_im_start>") == 0 &&
          square.token_text.find("<im_col>") != std::string::npos, "image token contract");
    const auto low_end = square.token_text.find("<im_end>");
    check(low_end != std::string::npos &&
          square.token_text.substr(0, low_end).find("<im_col>") == std::string::npos,
          "global low-resolution crop omits column tokens");
    check(square.token_text.find("<im_col>", low_end) != std::string::npos,
          "high-resolution grid keeps column tokens");
    for (float value : square.patches) check(std::isfinite(value) && value >= -1 && value <= 1,
                                              "normalized image range");

    VisualInput wide;
    check(preprocess_image(image(1920, 320), &wide, &error), error.c_str());
    check(wide.crop_count <= 9, "wide image crop bound");

    VisualInput joint;
    check(preprocess_images({image(1920, 320), image(320, 1920)},
                            &joint, &error), error.c_str());
    check(!joint.is_video && joint.crop_count == 4 &&
          joint.patch_token_count == 2 * 392,
          "two-image batch shares four-crop budget");
    check(joint.pooling_width == 4 &&
          joint.pooling.size() == static_cast<std::size_t>(joint.patch_token_count * 4),
          "joint image token/pool parity");
    check(joint.segment_crop_counts.size() == 2 &&
          joint.segment_patch_token_counts.size() == 2 &&
          joint.segment_crop_counts[0] == 2 && joint.segment_crop_counts[1] == 2,
          "joint image segments preserve independent vision batches");
    check(joint.token_text.find("Image 1<low_res_im_start>") == 0 &&
          joint.token_text.find("Image 2<low_res_im_start>") != std::string::npos,
          "joint image labels match upstream chat template");
    const std::size_t second_pooling = (joint.patch_token_count / 2) * 4;
    for (std::size_t i = second_pooling; i < joint.pooling.size(); ++i)
        check(joint.pooling[i] < 0 || joint.pooling[i] >= 2 * 729,
              "second image pooling indices are crop-offset");

    VisualInput rejected;
    check(!preprocess_images({image(1, 1)}, &rejected, &error),
          "joint processor rejects one image");
    check(!preprocess_images({image(1, 1), image(1, 1), image(1, 1)},
                             &rejected, &error),
          "joint processor rejects unsafe third image");

    VisualInput video;
    check(preprocess_video_frames({image(320, 180), image(180, 320)}, {0.0, 1.25},
                                  &video, &error), error.c_str());
    check(video.is_video && video.crop_count == 2 && video.patch_token_count == 162,
          "video frame/token counts");
    check(video.pooling_width == 9 && video.pooling.size() == 162u * 9u, "3x3 video pooling");
    check(video.token_text.find("0.0 <im_start>") == 0 &&
          video.token_text.find(" 1.2 <im_start>") != std::string::npos,
          "uniform video timestamp format");
    check(video.token_text.find("<frame_start>") == std::string::npos &&
          video.token_text.find("<im_col>") == std::string::npos,
          "final video special-token policy");

    std::vector<RgbImage> excessive_frames;
    std::vector<double> excessive_timestamps;
    for (int i = 0; i < 17; ++i) {
        excessive_frames.push_back(image(1, 1));
        excessive_timestamps.push_back(static_cast<double>(i));
    }
    check(!preprocess_video_frames(excessive_frames, excessive_timestamps,
                                   &video, &error),
          "video processor rejects more than 16 retained frames");

    std::cout << "molmo2 processor: PASS\n";
    return 0;
}
