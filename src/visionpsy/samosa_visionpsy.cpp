#include "visionpsy_model.h"
#include "../json.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <atomic>
#include <csignal>
#include <unistd.h>

using namespace samosa::visionpsy;

static std::atomic<bool> g_quit{false};
static std::atomic<bool> g_cancel{false};

static void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM || sig == SIGPIPE) {
        g_quit.store(true);
        g_cancel.store(true);
    }
}

static std::string json_escape_string(const std::string& str) {
    std::ostringstream ss;
    for (char c : str) {
        switch (c) {
            case '"': ss << "\\\""; break;
            case '\\': ss << "\\\\"; break;
            case '\b': ss << "\\b"; break;
            case '\f': ss << "\\f"; break;
            case '\n': ss << "\\n"; break;
            case '\r': ss << "\\r"; break;
            case '\t': ss << "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    ss << buf;
                } else {
                    ss << c;
                }
                break;
        }
    }
    return ss.str();
}

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, signal_handler);

    VisionPsyModel model;

    // If model directory was provided via command line as initial preload
    if (argc > 1 && argv[1]) {
        std::string model_dir = argv[1];
        if (!model.load(model_dir)) {
            std::cerr << "[samosa-visionpsy] Failed to load initial model from: " << model_dir
                      << ": " << model.last_error() << std::endl;
            return 78;
        }
    }

    std::string line;
    while (!g_quit.load() && std::getline(std::cin, line)) {
        if (line.empty()) continue;

        char* arena = nullptr;
        jval* root = json_parse(line.c_str(), &arena);
        if (!root || root->t != J_OBJ) {
            std::cout << "{\"status\":\"error\",\"code\":\"invalid_json\",\"message\":\"Malformed JSON command\"}\n" << std::flush;
            if (root) json_free(root);
            if (arena) free(arena);
            continue;
        }

        jval* cmd_val = json_get(root, "command");
        std::string command = (cmd_val && cmd_val->t == J_STR) ? cmd_val->str : "";

        if (command == "ping") {
            std::cout << "{\"status\":\"ok\",\"pong\":true}\n" << std::flush;
        } else if (command == "load") {
            jval* dir_val = json_get(root, "model_dir");
            if (!dir_val || dir_val->t != J_STR) {
                std::cout << "{\"status\":\"error\",\"code\":\"invalid_args\",\"message\":\"Missing model_dir\"}\n" << std::flush;
            } else {
                bool ok = model.load(dir_val->str);
                if (ok) {
                    std::cout << "{\"status\":\"ok\"}\n" << std::flush;
                } else {
                    std::cout << "{\"status\":\"error\",\"code\":\"vision_load_failed\",\"message\":\"Could not load VisionPsy model weights\"}\n" << std::flush;
                }
            }
        } else if (command == "inspect") {
            jval* id_val = json_get(root, "id");
            std::string correlation_id = (id_val && id_val->t == J_STR) ? id_val->str : "";

            jval* img_val = json_get(root, "image_path");
            jval* prompt_val = json_get(root, "prompt");
            jval* max_tokens_val = json_get(root, "max_tokens");
            jval* max_side_len_val = json_get(root, "max_side_len");

            if (!model.is_loaded()) {
                std::cout << "{\"status\":\"error\",\"code\":\"vision_not_loaded\",\"message\":\"VisionPsy model is not loaded\"}\n" << std::flush;
            } else if (!img_val || img_val->t != J_STR || !prompt_val || prompt_val->t != J_STR) {
                std::cout << "{\"status\":\"error\",\"code\":\"invalid_args\",\"message\":\"Missing image_path or prompt\"}\n" << std::flush;
            } else {
                std::string image_path = img_val->str;
                std::string prompt_text = prompt_val->str;
                int max_tokens = (max_tokens_val && max_tokens_val->t == J_NUM) ? (int)max_tokens_val->num : 512;
                if (max_tokens <= 0) max_tokens = 512;

                int max_side_len = (max_side_len_val && max_side_len_val->t == J_NUM) ? (int)max_side_len_val->num : 2048;
                if (max_side_len < 512) max_side_len = 512;
                if (max_side_len > 2048) max_side_len = 2048;

                // Read image file into memory
                std::ifstream file(image_path, std::ios::binary | std::ios::ate);
                if (!file.is_open()) {
                    std::cout << "{\"status\":\"error\",\"code\":\"vision_invalid_image\",\"id\":\""
                              << json_escape_string(correlation_id) << "\",\"message\":\"Image file not found or unreadable\"}\n" << std::flush;
                } else {
                    std::streamsize file_size = file.tellg();
                    file.seekg(0, std::ios::beg);
                    if (file_size <= 0 || file_size > (64LL << 20)) {
                        std::cout << "{\"status\":\"error\",\"code\":\"vision_invalid_image\",\"id\":\""
                                  << json_escape_string(correlation_id) << "\",\"message\":\"Image file size exceeds the safe local limit\"}\n" << std::flush;
                    } else {
                      std::vector<unsigned char> img_bytes((size_t)file_size);
                      if (!file.read((char*)img_bytes.data(), file_size)) {
                        std::cout << "{\"status\":\"error\",\"code\":\"vision_invalid_image\",\"id\":\""
                                  << json_escape_string(correlation_id) << "\",\"message\":\"Failed to read image bytes\"}\n" << std::flush;
                      } else {
                        ImageTiles tiles;
                        if (!model.preprocess_image_tiles(img_bytes.data(), img_bytes.size(), max_side_len, &tiles)) {
                            std::cout << "{\"status\":\"error\",\"code\":\"vision_invalid_image\",\"id\":\""
                                      << json_escape_string(correlation_id) << "\",\"message\":\"Image format is invalid or exceeds safety dimensions\"}\n" << std::flush;
                        } else {
                            try {
                                g_cancel.store(false);
                                auto result = model.generate_from_tiles(tiles, prompt_text, max_tokens, &g_cancel);

                                if (!result.error.empty()) {
                                    std::string err_code = (result.error == "Cancelled") ? "vision_cancelled" : "vision_inference_failed";
                                    std::cout << "{\"status\":\"error\",\"code\":\"" << err_code << "\",\"id\":\""
                                              << json_escape_string(correlation_id) << "\",\"message\":\""
                                              << json_escape_string(result.error) << "\"}\n" << std::flush;
                                } else {
                                    std::cout << "{\"status\":\"ok\",\"id\":\"" << json_escape_string(correlation_id)
                                              << "\",\"observation\":\"" << json_escape_string(result.text)
                                              << "\",\"n_w\":" << tiles.n_w
                                              << ",\"n_h\":" << tiles.n_h
                                              << ",\"has_global\":" << (tiles.has_global ? "true" : "false")
                                              << ",\"prompt_tokens\":" << result.prompt_tokens
                                              << ",\"generated_tokens\":" << result.generated_tokens
                                              << ",\"prefill_ms\":" << result.prefill_ms
                                              << ",\"decode_ms\":" << result.decode_ms << "}\n" << std::flush;
                                }
                            } catch (const std::exception& e) {
                                std::cout << "{\"status\":\"error\",\"code\":\"vision_process_failed\",\"id\":\""
                                          << json_escape_string(correlation_id) << "\",\"message\":\""
                                          << json_escape_string(e.what()) << "\"}\n" << std::flush;
                            }
                        }
                      }
                    }
                }
            }
        } else if (command == "cancel") {
            g_cancel.store(true);
            std::cout << "{\"status\":\"ok\",\"cancelled\":true}\n" << std::flush;
        } else if (command == "quit" || command == "exit") {
            std::cout << "{\"status\":\"ok\",\"message\":\"bye\"}\n" << std::flush;
            break;
        } else {
            std::cout << "{\"status\":\"error\",\"code\":\"unknown_command\",\"message\":\"Unrecognized command\"}\n" << std::flush;
        }

        json_free(root);
        free(arena);
    }

    return 0;
}
