/* Bounded macOS audio probe/decoder for Samosa Auto file chat.
 *
 * This process deliberately owns AVFoundation parsing rather than linking
 * media codecs into the gateway. Each invocation handles one private local
 * file, writes at most one bounded mono/16 kHz/PCM16 WAV window, and exits.
 */
#define _DARWIN_C_SOURCE

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void put_le16(unsigned char *out, uint16_t value) {
    out[0] = (unsigned char)value;
    out[1] = (unsigned char)(value >> 8);
}

static void put_le32(unsigned char *out, uint32_t value) {
    out[0] = (unsigned char)value;
    out[1] = (unsigned char)(value >> 8);
    out[2] = (unsigned char)(value >> 16);
    out[3] = (unsigned char)(value >> 24);
}

static int write_all(int fd, const void *bytes, size_t length) {
    const unsigned char *at = (const unsigned char *)bytes;
    while (length) {
        ssize_t wrote = write(fd, at, length);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) return 0;
        at += wrote;
        length -= (size_t)wrote;
    }
    return 1;
}

static void format_id_text(AudioFormatID value, char out[5]) {
    out[0] = (char)(value >> 24);
    out[1] = (char)(value >> 16);
    out[2] = (char)(value >> 8);
    out[3] = (char)value;
    out[4] = 0;
    for (int i = 0; i < 4; ++i)
        if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e ||
            out[i] == '"' || out[i] == '\\')
            out[i] = '?';
}

static AVURLAsset *open_asset(const char *path) {
    if (!path || !*path) return nil;
    NSString *value = [[NSString alloc] initWithBytes:path
                                               length:strlen(path)
                                             encoding:NSUTF8StringEncoding];
    if (!value) return nil;
    NSURL *url = [NSURL fileURLWithPath:value isDirectory:NO];
    return [AVURLAsset URLAssetWithURL:url
                               options:@{AVURLAssetPreferPreciseDurationAndTimingKey:@YES}];
}

static int probe(const char *path) {
    AVURLAsset *asset = open_asset(path);
    if (!asset) return 0;
    NSArray<AVAssetTrack *> *audio = [asset tracksWithMediaType:AVMediaTypeAudio];
    NSArray<AVAssetTrack *> *video = [asset tracksWithMediaType:AVMediaTypeVideo];
    double duration = CMTimeGetSeconds(asset.duration);
    if (audio.count < 1 || !isfinite(duration) || duration <= 0.0 ||
        duration > 4.0 * 60.0 * 60.0) return 0;

    AudioFormatID format = 0;
    AVAssetTrack *selected = audio[0];
    double audio_start = CMTimeGetSeconds(selected.timeRange.start);
    double audio_duration = CMTimeGetSeconds(selected.timeRange.duration);
    double audio_end = audio_start + audio_duration;
    if (!isfinite(audio_start) || !isfinite(audio_duration) ||
        !isfinite(audio_end) || audio_start < 0 || audio_duration <= 0 ||
        audio_end > 4.0 * 60.0 * 60.0 + 1.0) return 0;
    if (selected.formatDescriptions.count) {
        CMAudioFormatDescriptionRef description =
            (__bridge CMAudioFormatDescriptionRef)selected.formatDescriptions[0];
        const AudioStreamBasicDescription *stream =
            CMAudioFormatDescriptionGetStreamBasicDescription(description);
        if (stream) format = stream->mFormatID;
    }
    char codec[5]; format_id_text(format, codec);
    printf("{\"ok\":true,\"duration_seconds\":%.6f,"
           "\"audio_start_seconds\":%.6f,\"audio_end_seconds\":%.6f,"
           "\"audio_streams\":%lu,\"video_streams\":%lu,"
           "\"selected_stream\":0,\"codec_fourcc\":\"%s\"}\n",
           duration, audio_start, audio_end, (unsigned long)audio.count,
           (unsigned long)video.count, codec);
    return 1;
}

static int write_wav_header(int fd, uint32_t data_bytes) {
    unsigned char header[44] = {0};
    memcpy(header, "RIFF", 4); put_le32(header + 4, 36u + data_bytes);
    memcpy(header + 8, "WAVEfmt ", 8); put_le32(header + 16, 16);
    put_le16(header + 20, 1); put_le16(header + 22, 1);
    put_le32(header + 24, 16000); put_le32(header + 28, 32000);
    put_le16(header + 32, 2); put_le16(header + 34, 16);
    memcpy(header + 36, "data", 4); put_le32(header + 40, data_bytes);
    return lseek(fd, 0, SEEK_SET) == 0 && write_all(fd, header, sizeof(header));
}

static int decode_window(const char *input_path, double start, double end,
                         const char *output_path) {
    AVURLAsset *asset = open_asset(input_path);
    if (!asset || !isfinite(start) || !isfinite(end) || start < 0 ||
        end <= start || end - start > 3600.0) return 0;
    NSArray<AVAssetTrack *> *tracks = [asset tracksWithMediaType:AVMediaTypeAudio];
    double asset_duration = CMTimeGetSeconds(asset.duration);
    if (tracks.count < 1 || !isfinite(asset_duration) ||
        asset_duration <= 0 || start >= asset_duration + 0.001 ||
        end > asset_duration + 0.050) return 0;
    if (end > asset_duration) end = asset_duration;

    NSError *error = nil;
    AVAssetReader *reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
    if (!reader || error) return 0;
    reader.timeRange = CMTimeRangeFromTimeToTime(
        CMTimeMakeWithSeconds(start, 60000),
        CMTimeMakeWithSeconds(end, 60000));
    NSDictionary *settings = @{
        AVFormatIDKey: @(kAudioFormatLinearPCM),
        AVSampleRateKey: @16000,
        AVNumberOfChannelsKey: @1,
        AVLinearPCMBitDepthKey: @16,
        AVLinearPCMIsFloatKey: @NO,
        AVLinearPCMIsBigEndianKey: @NO,
        AVLinearPCMIsNonInterleaved: @NO
    };
    AVAssetReaderTrackOutput *track_output =
        [[AVAssetReaderTrackOutput alloc] initWithTrack:tracks[0]
                                         outputSettings:settings];
    track_output.alwaysCopiesSampleData = NO;
    if (![reader canAddOutput:track_output]) return 0;
    [reader addOutput:track_output];

    int output = open(output_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (output < 0) return 0;
    unsigned char placeholder[44] = {0};
    int ok = write_all(output, placeholder, sizeof(placeholder)) &&
             [reader startReading];
    uint64_t written = 0;
    uint64_t maximum = (uint64_t)ceil((end - start + 1.0) * 32000.0);
    unsigned char buffer[64 * 1024];
    while (ok) {
        CMSampleBufferRef sample = [track_output copyNextSampleBuffer];
        if (!sample) break;
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
        size_t length = block ? CMBlockBufferGetDataLength(block) : 0;
        for (size_t offset = 0; ok && offset < length;) {
            size_t amount = length - offset < sizeof(buffer)
                          ? length - offset : sizeof(buffer);
            ok = written + amount <= maximum &&
                 CMBlockBufferCopyDataBytes(block, offset, amount, buffer) == kCMBlockBufferNoErr &&
                 write_all(output, buffer, amount);
            offset += amount;
            written += amount;
        }
        CFRelease(sample);
    }
    ok = ok && reader.status == AVAssetReaderStatusCompleted && written > 0 &&
         written <= UINT32_MAX && !(written & 1u) &&
         write_wav_header(output, (uint32_t)written) && fsync(output) == 0;
    if (close(output)) ok = 0;
    if (!ok) unlink(output_path);
    return ok;
}

static int parse_seconds(const char *text, double *out) {
    char *end = NULL; errno = 0;
    double value = strtod(text, &end);
    if (errno || !end || *end || !isfinite(value)) return 0;
    *out = value;
    return 1;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc == 3 && !strcmp(argv[1], "--probe")) {
            if (probe(argv[2])) return 0;
            fputs("{\"ok\":false,\"error\":\"unsupported_or_invalid_audio\"}\n",
                  stdout);
            return 2;
        }
        if (argc == 7 && !strcmp(argv[1], "--decode-window")) {
            double start = 0, end = 0;
            if (parse_seconds(argv[3], &start) && parse_seconds(argv[4], &end) &&
                !strcmp(argv[5], "--output") &&
                decode_window(argv[2], start, end, argv[6])) return 0;
            fputs("audio window decode failed\n", stderr);
            return 2;
        }
        fputs("usage: samosa-audio-decode --probe FILE\n"
              "       samosa-audio-decode --decode-window FILE START END --output WAV\n",
              stderr);
        return 64;
    }
}
