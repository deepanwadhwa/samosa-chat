#include "molmo2_media.h"
#include "molmo2_contract.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include <cmath>
#include <sys/stat.h>

namespace samosa::molmo2 {

namespace {
constexpr std::size_t kVideoSide = 378;
}

bool decode_video_window(const std::string& path, double start_seconds,
                         double end_seconds, int max_frames, double max_fps,
                         DecodedVideo* output, std::string* error) {
    if (!output || path.empty() || path[0] != '/' || max_frames < 1 || max_frames > 16 ||
        !std::isfinite(start_seconds) || !std::isfinite(end_seconds) ||
        start_seconds < 0 || end_seconds < start_seconds || max_fps <= 0 || max_fps > 2.0) {
        if (error) *error = "invalid video window request";
        return false;
    }
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        static_cast<std::uint64_t>(st.st_size) > 4ULL * 1024 * 1024 * 1024) {
        if (error) *error = "video path is missing, not regular, is a symlink, or exceeds 4 GiB";
        return false;
    }
    @autoreleasepool {
        NSString* file = [NSString stringWithUTF8String:path.c_str()];
        if (!file) { if (error) *error = "video path is not valid UTF-8"; return false; }
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:file] options:nil];
        const double duration = CMTimeGetSeconds(asset.duration);
        if (!std::isfinite(duration) || duration <= 0) {
            if (error) *error = "AVFoundation could not determine video duration";
            return false;
        }
        const double bounded_start = std::min(start_seconds, duration);
        const double bounded_end = std::min(end_seconds <= 0 ? duration : end_seconds, duration);
        auto times = uniform_last_frame_times(bounded_start, bounded_end, max_frames, max_fps);
        if (times.empty()) { if (error) *error = "video window produced no sample times"; return false; }

        AVAssetImageGenerator* generator = [[AVAssetImageGenerator alloc] initWithAsset:asset];
        generator.appliesPreferredTrackTransform = YES;
        generator.requestedTimeToleranceBefore = CMTimeMakeWithSeconds(0.05, 600);
        generator.requestedTimeToleranceAfter = CMTimeMakeWithSeconds(0.05, 600);
        DecodedVideo result;
        result.duration_seconds = duration;
        for (double seconds : times) {
            /* AVFoundation can reject an exact request at the asset's terminal
               timestamp. Keep the last sample inside the presentation range. */
            seconds = std::min(seconds, std::max(0.0, duration - 1.0 / 600.0));
            NSError* ns_error = nil;
            CMTime actual = kCMTimeZero;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            CGImageRef image = [generator copyCGImageAtTime:CMTimeMakeWithSeconds(seconds, 600)
                                                  actualTime:&actual error:&ns_error];
#pragma clang diagnostic pop
            if (!image) {
                if (error) *error = ns_error ? [[ns_error localizedDescription] UTF8String]
                                             : "AVFoundation frame decode failed";
                return false;
            }
            const std::size_t source_width = CGImageGetWidth(image);
            const std::size_t source_height = CGImageGetHeight(image);
            if (!source_width || !source_height || source_width > 16384 || source_height > 16384 ||
                source_width * source_height > 64ULL * 1024 * 1024) {
                CGImageRelease(image);
                if (error) *error = "decoded video frame exceeds safety dimensions";
                return false;
            }
            RgbImage frame;
            frame.width = static_cast<int>(kVideoSide);
            frame.height = static_cast<int>(kVideoSide);
            frame.pixels.resize(kVideoSide * kVideoSide * 3);
            std::vector<unsigned char> rgba(kVideoSide * kVideoSide * 4);
            CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
            /* CoreGraphics does not consistently support three-byte RGB bitmap
               contexts. Decode and resize directly into a bounded canonical
               RGBA layout, then discard alpha. No full-resolution frame is
               retained after AVFoundation returns its CGImage. */
            const CGBitmapInfo bitmap_info = static_cast<CGBitmapInfo>(
                static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast) |
                static_cast<std::uint32_t>(kCGBitmapByteOrder32Big));
            CGContextRef context = CGBitmapContextCreate(rgba.data(), kVideoSide, kVideoSide, 8,
                kVideoSide * 4, color, bitmap_info);
            CGColorSpaceRelease(color);
            if (!context) { CGImageRelease(image); if (error) *error = "RGB frame allocation failed"; return false; }
            CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
            CGContextTranslateCTM(context, 0, static_cast<CGFloat>(kVideoSide));
            CGContextScaleCTM(context, 1, -1);
            CGContextDrawImage(context, CGRectMake(0, 0, kVideoSide, kVideoSide), image);
            CGContextRelease(context); CGImageRelease(image);
            for (std::size_t pixel = 0; pixel < kVideoSide * kVideoSide; ++pixel) {
                frame.pixels[pixel * 3 + 0] = rgba[pixel * 4 + 0];
                frame.pixels[pixel * 3 + 1] = rgba[pixel * 4 + 1];
                frame.pixels[pixel * 3 + 2] = rgba[pixel * 4 + 2];
            }
            /* Molmo's timestamp tokens describe positions in the original
               asset, so retain absolute media time across sequential windows. */
            result.timestamps.push_back(std::max(0.0, CMTimeGetSeconds(actual)));
            result.frames.push_back(std::move(frame));
        }
        *output = std::move(result);
        if (error) error->clear();
        return true;
    }
}

}  // namespace samosa::molmo2
