#ifndef SAMOSA_MOLMO2_MEDIA_H
#define SAMOSA_MOLMO2_MEDIA_H

#include "molmo2_processor.h"

#include <string>
#include <vector>

namespace samosa::molmo2 {

struct DecodedVideo {
    double duration_seconds = 0;
    std::vector<double> timestamps;
    std::vector<RgbImage> frames;
};

/* Native platform decoder. On macOS this is AVFoundation/CoreGraphics; it
   seeks directly to at most 16 bounded sample times and stores only 378x378
   RGB frames, never full-resolution frames or an entire video. */
bool decode_video_window(const std::string& path,
                         double start_seconds,
                         double end_seconds,
                         int max_frames,
                         double max_fps,
                         DecodedVideo* output,
                         std::string* error);

}  // namespace samosa::molmo2

#endif
