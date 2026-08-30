#include "molmo2_media.h"
#include "molmo2_model.h"
#include "molmo2_processor.h"
#include "../json.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

using namespace samosa::molmo2;

namespace {
std::atomic_bool g_cancelled {false};
std::atomic_bool g_quit {false};
constexpr std::size_t kMaxFrame = 1024 * 1024;
constexpr std::size_t kMaxImages = 2;

void signal_handler(int) { g_cancelled.store(true); g_quit.store(true); }

bool read_all(void* bytes_, std::size_t length) {
    auto* bytes = static_cast<unsigned char*>(bytes_);
    while (length) {
        const ssize_t got = ::read(STDIN_FILENO, bytes, length);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        bytes += got; length -= static_cast<std::size_t>(got);
    }
    return true;
}

bool write_all(const void* bytes_, std::size_t length) {
    const auto* bytes = static_cast<const unsigned char*>(bytes_);
    while (length) {
        const ssize_t wrote = ::write(STDOUT_FILENO, bytes, length);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) return false;
        bytes += wrote; length -= static_cast<std::size_t>(wrote);
    }
    return true;
}

bool receive(std::string* output) {
    std::uint32_t encoded = 0;
    if (!read_all(&encoded, sizeof(encoded))) return false;
    const std::size_t length = ntohl(encoded);
    if (!length || length > kMaxFrame) return false;
    output->resize(length);
    return read_all(output->data(), length);
}

bool send(const std::string& message) {
    if (message.empty() || message.size() > kMaxFrame) return false;
    const std::uint32_t encoded = htonl(static_cast<std::uint32_t>(message.size()));
    return write_all(&encoded, sizeof(encoded)) && write_all(message.data(), message.size());
}

std::string escape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char c : value) {
        switch (c) {
            case '"': out << "\\\""; break; case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break; case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) { char code[8]; std::snprintf(code, sizeof(code), "\\u%04x", c); out << code; }
                else out << c;
        }
    }
    return out.str();
}

std::string string_value(jval* root, const char* key) {
    jval* value = root ? json_get(root, key) : nullptr;
    return value && value->t == J_STR ? value->str : "";
}

double number_value(jval* root, const char* key, double fallback) {
    jval* value = root ? json_get(root, key) : nullptr;
    return value && value->t == J_NUM && std::isfinite(value->num) ? value->num : fallback;
}

bool integer_value(jval* root, const char* key, int fallback,
                   int minimum, int maximum, int* output) {
    jval* value = root ? json_get(root, key) : nullptr;
    if (!value) { *output = fallback; return true; }
    if (value->t != J_NUM || !std::isfinite(value->num) ||
        std::floor(value->num) != value->num ||
        value->num < minimum || value->num > maximum)
        return false;
    *output = static_cast<int>(value->num);
    return true;
}

bool string_array_value(jval* root, const char* key,
                        std::size_t minimum, std::size_t maximum,
                        std::vector<std::string>* output) {
    jval* value = root ? json_get(root, key) : nullptr;
    if (!value || value->t != J_ARR || value->len < static_cast<int>(minimum) ||
        value->len > static_cast<int>(maximum) || !output)
        return false;
    output->clear();
    output->reserve(static_cast<std::size_t>(value->len));
    for (int index = 0; index < value->len; ++index) {
        jval* item = value->kids[index];
        if (!item || item->t != J_STR || !item->str[0] || item->str[0] != '/') {
            output->clear();
            return false;
        }
        output->emplace_back(item->str);
    }
    return true;
}

std::string error_reply(const std::string& id, const std::string& code, const std::string& message) {
    return "{\"status\":\"error\",\"id\":\"" + escape(id) + "\",\"code\":\"" +
           escape(code) + "\",\"message\":\"" + escape(message) + "\"}";
}
}  // namespace

int main(int argc, char** argv) {
    const bool single_self_test = argc == 5 && !std::strcmp(argv[1], "--self-test") &&
                                  (!std::strcmp(argv[2], "image") || !std::strcmp(argv[2], "video"));
    const bool multi_self_test = argc == 6 &&
                                 !std::strcmp(argv[1], "--self-test") &&
                                 !std::strcmp(argv[2], "images");
    const bool self_test = single_self_test || multi_self_test;
    const char* model_path = self_test ? argv[3] : argc == 2 ? argv[1] : nullptr;
    if (!model_path || model_path[0] != '/' ||
        (self_test && argv[4][0] != '/') || (multi_self_test && argv[5][0] != '/')) {
        std::cerr << "usage: samosa-molmo2 ABSOLUTE_MODEL_PACKAGE\n"
                     "       samosa-molmo2 --self-test image|video ABSOLUTE_MODEL_PACKAGE ABSOLUTE_MEDIA\n"
                     "       samosa-molmo2 --self-test images ABSOLUTE_MODEL_PACKAGE ABSOLUTE_IMAGE_1 ABSOLUTE_IMAGE_2\n";
        return 2;
    }
    std::signal(SIGINT, signal_handler);
    /* Leave SIGTERM at its default disposition. The supervisor uses it for
       prompt cancellation even when MLX is inside a long, non-interruptible
       prefill; graceful shutdown still arrives as the framed quit command. */
    std::signal(SIGPIPE, signal_handler);
    Model model;
    std::string load_error;
    if (!model.load(model_path, &load_error)) {
        std::cerr << "samosa-molmo2: " << load_error << "\n"; return 78;
    }
    if (self_test) {
        VisualInput visual; std::string media_error; bool prepared = false;
        if (!std::strcmp(argv[2], "image")) {
            RgbImage image;
            prepared = decode_image(argv[4], &image, &media_error) &&
                       preprocess_image(image, &visual, &media_error);
        } else if (!std::strcmp(argv[2], "images")) {
            std::vector<RgbImage> images(static_cast<std::size_t>(argc - 4));
            prepared = true;
            for (int index = 4; index < argc; ++index) {
                if (!decode_image(argv[index], &images[static_cast<std::size_t>(index - 4)],
                                  &media_error)) {
                    prepared = false;
                    break;
                }
            }
            prepared = prepared && preprocess_images(images, &visual, &media_error);
        } else {
            DecodedVideo video;
            prepared = decode_video_window(argv[4], 0, 0, 8, 2.0, &video, &media_error) &&
                       preprocess_video_frames(video.frames, video.timestamps, &visual, &media_error);
        }
        if (!prepared) { std::cerr << "samosa-molmo2: " << media_error << "\n"; return 65; }
        GenerateOptions options; options.max_new_tokens = 96;
        auto result = model.generate(
            multi_self_test
                ? "Compare all supplied Images directly. Describe what is common and what differs."
                : "First describe the visible medium, outline, composition, colors, shapes, "
                  "and repeated motifs without naming the subject. Then identify the most "
                  "likely visual type and subject. Distinguish a literal object from an "
                  "illustration or decorative pattern that merely resembles one. If "
                  "ambiguous, give plausible interpretations and the visible reason.",
                                     visual, options, &g_cancelled);
        model.unload();
        if (!result.ok || result.text.empty()) {
            std::cerr << "samosa-molmo2: " << (result.error.empty() ? "empty generation" : result.error) << "\n";
            return 70;
        }
        std::cout << result.text << "\n";
        return 0;
    }
    while (!g_quit.load()) {
        std::string request;
        if (!receive(&request)) break;
        char* arena = nullptr;
        jval* root = json_parse(request.c_str(), &arena);
        if (!root || root->t != J_OBJ) {
            if (!send(error_reply("", "invalid_json", "malformed framed JSON command"))) break;
            json_free(root); free(arena); continue;
        }
        const std::string command = string_value(root, "command");
        const std::string id = string_value(root, "id");
        if (command == "hello") {
            send("{\"status\":\"ok\",\"protocol\":\"samosa.multimodal.v1\","
                 "\"provider\":\"molmo2-4b\",\"capabilities\":[\"image\",\"multi_image\",\"video\"]}");
        } else if (command == "quit") {
            send("{\"status\":\"ok\"}"); json_free(root); free(arena); break;
        } else if (command == "cancel") {
            g_cancelled.store(true); send("{\"status\":\"ok\",\"cancelled\":true}");
        } else if (command == "analyze") {
            const std::string path = string_value(root, "media_path");
            const std::string kind = string_value(root, "media_kind");
            const std::string prompt = string_value(root, "prompt");
            int max_tokens = 256;
            VisualInput visual;
            double media_duration = 0;
            int decoded_frames = 0;
            int decoded_images = 0;
            std::string media_error;
            bool prepared = false;
            if (!integer_value(root, "max_tokens", 256, 1, 1024, &max_tokens)) {
                media_error = "max_tokens must be a whole number from 1 through 1024";
            } else if (kind == "image") {
                RgbImage image;
                prepared = decode_image(path, &image, &media_error) &&
                           preprocess_image(image, &visual, &media_error);
                if (prepared) decoded_images = 1;
            } else if (kind == "images") {
                std::vector<std::string> paths;
                if (!string_array_value(root, "media_paths", 2, kMaxImages, &paths)) {
                    media_error = "media_paths must contain exactly two absolute image paths";
                } else {
                    std::vector<RgbImage> images(paths.size());
                    prepared = true;
                    for (std::size_t index = 0; index < paths.size(); ++index) {
                        if (!decode_image(paths[index], &images[index], &media_error)) {
                            prepared = false;
                            break;
                        }
                    }
                    prepared = prepared && preprocess_images(images, &visual, &media_error);
                    if (prepared) decoded_images = static_cast<int>(images.size());
                }
            } else if (kind == "video") {
                DecodedVideo video;
                const double start = number_value(root, "start_seconds", 0);
                const double end = number_value(root, "end_seconds", 0);
                int max_frames = 16;
                if (!integer_value(root, "max_frames", 16, 1, 16, &max_frames)) {
                    media_error = "max_frames must be a whole number from 1 through 16";
                } else {
                    prepared = decode_video_window(path, start, end, max_frames, 2.0,
                                                   &video, &media_error) &&
                               preprocess_video_frames(video.frames, video.timestamps,
                                                       &visual, &media_error);
                }
                if (prepared) {
                    media_duration = video.duration_seconds;
                    decoded_frames = static_cast<int>(video.frames.size());
                }
            } else media_error = "media_kind must be image, images, or video";
            if (!prepared) {
                send(error_reply(id, "molmo2_media_invalid", media_error));
            } else {
                g_cancelled.store(false);
                GenerateOptions options; options.max_new_tokens = max_tokens;
                auto result = model.generate(prompt, visual, options, &g_cancelled);
                if (!result.ok || result.text.empty()) {
                    send(error_reply(id, result.cancelled ? "molmo2_cancelled" : "molmo2_inference_failed",
                                     result.cancelled ? "request cancelled" :
                                     result.error.empty() ? "model produced no visual observation"
                                                          : result.error));
                } else {
                    std::ostringstream reply;
                    reply << "{\"status\":\"ok\",\"id\":\"" << escape(id)
                          << "\",\"observation\":\"" << escape(result.text)
                          << "\",\"prompt_tokens\":" << result.prompt_tokens
                          << ",\"generated_tokens\":" << result.generated_tokens
                          << ",\"images\":" << decoded_images
                          << ",\"frames\":" << decoded_frames
                          << ",\"duration_seconds\":" << media_duration << "}";
                    send(reply.str());
                }
            }
        } else send(error_reply(id, "unknown_command", "unrecognized Molmo2 command"));
        json_free(root); free(arena);
    }
    model.unload();
    return 0;
}
