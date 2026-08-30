#include "molmo2_media.h"

namespace samosa::molmo2 {
bool decode_video_window(const std::string&, double, double, int, double,
                         DecodedVideo*, std::string* error) {
    if (error) *error = "native video decoding is supported on macOS only";
    return false;
}
}  // namespace samosa::molmo2
