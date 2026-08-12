#include "maple_model.h"
#include "tokenizer.h"
#include "openai_message_content.h"
#include "../samosa_http.h"
#include "../json.h"
#include "mlx/utils.h"

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstring>

using namespace samosa::maple;

struct ServerContext {
    MapleModel* model;
    MapleTokenizer* tokenizer;
};

static std::string visible_answer(std::string text, bool started_thinking) {
    const auto think_start = text.find("<think>");
    if (started_thinking || think_start != std::string::npos) {
        const auto think_end = text.find("</think>", think_start == std::string::npos ? 0 : think_start + 7);
        if (think_end == std::string::npos) return {};
        text.erase(0, think_end + 8);
    } else if (text.rfind("</think>", 0) == 0) {
        text.erase(0, 8);
    }
    for (const char* marker : {"<|im_end|>", "<|endoftext|>"}) {
        const auto end = text.find(marker);
        if (end != std::string::npos) text.erase(end);
    }
    const auto first = text.find_first_not_of(" \t\r\n");
    const auto last = text.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
}

// Very basic argmax sampling
int sample_argmax(const mlx::core::array& logits) {
    auto max_idx = mlx::core::argmax(logits, -1);
    mlx::core::eval({max_idx});
    return max_idx.item<int>();
}

// Handler for HTTP requests
int samosa_maple_handler(SamosaHttpServer *server, int fd, const SamosaHttpRequest *req, void *ctx) {
    (void)server;
    ServerContext* server_ctx = static_cast<ServerContext*>(ctx);
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/healthz") == 0) {
        ecache_stats stats{};
        server_ctx->model->cache_stats(&stats);
        char body[2048];
        std::snprintf(
            body, sizeof(body),
            "{\"ready\":true,\"backend\":\"maple\",\"streaming\":%s,"
            "\"cache\":{\"budget_bytes\":%llu,\"payload_bytes\":%llu,"
            "\"peak_payload_bytes\":%llu,\"entries\":%u,"
            "\"hits\":%llu,\"misses\":%llu,\"bytes_read\":%llu,"
            "\"bytes_avoided\":%llu,\"evictions\":%llu,"
            "\"failed_admissions\":%llu,\"pressure_warn\":%llu,"
            "\"pressure_critical\":%llu}}",
            server_ctx->model->streaming_enabled() ? "true" : "false",
            (unsigned long long)stats.budget_bytes,
            (unsigned long long)stats.payload_bytes,
            (unsigned long long)stats.peak_payload_bytes,
            stats.entries,
            (unsigned long long)stats.base_hits,
            (unsigned long long)stats.base_misses,
            (unsigned long long)stats.base_bytes_read,
            (unsigned long long)stats.base_bytes_avoided,
            (unsigned long long)stats.evictions,
            (unsigned long long)stats.failed_admissions,
            (unsigned long long)stats.pressure_warn_events,
            (unsigned long long)stats.pressure_critical_events);
        return samosa_http_response(fd, 200, "application/json",
                                    body, nullptr);
    }
    // The HTTP server dispatches each connection on a fresh pthread.  MLX's
    // default streams are thread-local, so initialize one before touching any
    // lazy arrays on this worker thread.
    auto worker_stream = mlx::core::new_thread_unsafe_stream(mlx::core::default_device());
    mlx::core::set_default_stream(worker_stream);
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/v1/chat/completions") == 0) {
        char *arena = nullptr;
        jval *root = json_parse(req->body, &arena);

        if (!root || root->t != J_OBJ) {
            json_free(root);
            return samosa_http_json_error(fd, 400, "invalid_json", "A JSON object is required.");
        }

        jval* messages = json_get(root, "messages");
        if (!messages || messages->t != J_ARR) {
            json_free(root); free(arena);
            return samosa_http_json_error(fd, 400, "invalid_request", "messages array required.");
        }

        std::vector<Message> msgs;
        for (int i = 0; i < messages->len; i++) {
            jval* m = messages->kids[i];
            if (m->t == J_OBJ) {
                jval* role = json_get(m, "role");
                jval* content = json_get(m, "content");
                std::string text;
                if (role && role->t == J_STR && openai_message_text(content, text)) {
                    msgs.push_back({role->str, std::move(text)});
                }
            }
        }

        if (msgs.empty()) {
            json_free(root); free(arena);
            return samosa_http_json_error(fd, 400, "invalid_request", "messages must contain text content.");
        }

        bool stream = false;
        jval* stream_val = json_get(root, "stream");
        if (stream_val && stream_val->t == J_BOOL) {
            stream = stream_val->boolean;
        }

        bool enable_thinking = true;
        jval* template_kwargs = json_get(root, "chat_template_kwargs");
        if (template_kwargs && template_kwargs->t == J_OBJ) {
            jval* thinking = json_get(template_kwargs, "enable_thinking");
            if (thinking && thinking->t == J_BOOL) enable_thinking = thinking->boolean;
        }

        int max_tokens = 50;
        jval* max_tokens_val = json_get(root, "max_tokens");
        if (max_tokens_val && max_tokens_val->t == J_NUM) {
            max_tokens = (int)max_tokens_val->num;
        }
        if (max_tokens < 0 || max_tokens > 4096) {
            json_free(root); free(arena);
            return samosa_http_json_error(fd, 400, "invalid_request", "max_tokens must be between 0 and 4096.");
        }

        json_free(root);
        free(arena);

        std::string prompt = server_ctx->tokenizer->apply_chat_template(msgs, true, enable_thinking);
        auto input_ids = server_ctx->tokenizer->encode(prompt);

        int eos_token = server_ctx->tokenizer->get_eos_token();

        std::vector<int> generated_tokens;
        std::vector<KVCache*> caches;
        for (const auto& layer_type : server_ctx->model->args().layer_types) {
            if (layer_type == "sliding_attention") {
                caches.push_back(new RotatingKVCache(server_ctx->model->args().sliding_window));
            } else {
                caches.push_back(new KVCache());
            }
        }

        bool client_connected = true;
        std::string streamed_text;
        std::string text;
        std::string generation_error;
        const char* finish_reason = max_tokens == 0 ? "length" : "stop";
        if (stream) {
            client_connected = samosa_http_stream_headers(fd) &&
                samosa_send_all(fd, ": maple generation started\n\n", 28);
        }

        auto stream_progress = [&]() {
            if (!stream || !client_connected) return;
            std::string visible = visible_answer(
                server_ctx->tokenizer->decode(generated_tokens), enable_thinking);
            if (visible.size() > streamed_text.size() &&
                visible.compare(0, streamed_text.size(), streamed_text) == 0) {
                std::string delta = visible.substr(streamed_text.size());
                char escaped[8192];
                samosa_json_escape(escaped, sizeof(escaped), delta.c_str());
                char chunk[8704];
                snprintf(chunk, sizeof(chunk),
                    "data: {\"choices\": [{\"delta\": {\"content\": \"%s\"}}]}\n\n",
                    escaped);
                client_connected = samosa_send_all(fd, chunk, strlen(chunk));
                streamed_text = std::move(visible);
            } else {
                client_connected = samosa_send_all(fd, ": keepalive\n\n", 13);
            }
        };

        try {
            if (max_tokens > 0 && client_connected) {
                // Prefill once, then feed one token at a time through the caches.
                mlx::core::array x(input_ids.begin(), {(int)input_ids.size()}, mlx::core::int32);
                x = mlx::core::reshape(x, {1, (int)input_ids.size()});
                auto logits = server_ctx->model->streaming_enabled()
                                  ? server_ctx->model->run_token_sequence(x, &caches)
                                  : (*server_ctx->model)(x, &caches);
                auto last_logits = server_ctx->model->streaming_enabled()
                                       ? logits
                                       : mlx::core::slice(logits, {0, (int)input_ids.size() - 1, 0},
                                                          {1, (int)input_ids.size(), logits.shape(-1)});
                int next_token = sample_argmax(last_logits);

                for (int i = 0; i < max_tokens; i++) {
                    generated_tokens.push_back(next_token);
                    input_ids.push_back(next_token);
                    stream_progress();

                    if (!client_connected) break;
                    if (next_token == eos_token) {
                        finish_reason = "stop";
                        break;
                    }
                    if (i + 1 == max_tokens) {
                        finish_reason = "length";
                        break;
                    }

                    x = mlx::core::array({next_token}, {1, 1});
                    logits = (*server_ctx->model)(x, &caches);
                    last_logits = mlx::core::slice(logits, {0, 0, 0}, {1, 1, logits.shape(-1)});
                    next_token = sample_argmax(last_logits);
                }
            }
            text = visible_answer(server_ctx->tokenizer->decode(generated_tokens), enable_thinking);
            stream_progress();
        } catch (const std::exception& error) {
            generation_error = error.what();
        } catch (...) {
            generation_error = "unknown exception";
        }

        for (auto* cache : caches) delete cache;

        if (!generation_error.empty()) {
            std::cerr << "Maple generation failed: " << generation_error << std::endl;
            if (stream) {
                if (client_connected) {
                    const char* error_chunk =
                        "data: {\"error\":{\"type\":\"generation_error\",\"message\":\"Maple generation failed. Please retry this turn.\"}}\n\n"
                        "data: [DONE]\n\n";
                    samosa_send_all(fd, error_chunk, strlen(error_chunk));
                }
            } else {
                samosa_http_json_error(fd, 500, "generation_error",
                                       "Maple generation failed. Please retry this turn.");
            }
            return 1;
        }

        if (stream) {
            if (client_connected) {
                char finish_chunk[192];
                snprintf(finish_chunk, sizeof(finish_chunk),
                    "data: {\"choices\": [{\"delta\": {}, \"finish_reason\": \"%s\"}]}\n\n",
                    finish_reason);
                client_connected = samosa_send_all(fd, finish_chunk, strlen(finish_chunk));
                const char* end_chunk = "data: [DONE]\n\n";
                if (client_connected) samosa_send_all(fd, end_chunk, strlen(end_chunk));
            }
        } else {
            // Construct JSON response
            char body[12288];
            char escaped_text[8192];
            samosa_json_escape(escaped_text, sizeof(escaped_text), text.c_str());
            snprintf(body, sizeof(body),
                "{\"choices\": [{\"index\": 0, \"message\": {\"role\": \"assistant\", \"content\": \"%s\"}, \"finish_reason\": \"%s\"}]}",
                escaped_text, finish_reason);

            samosa_http_response(fd, 200, "application/json", body, nullptr);
        }

        return 1;
    }

    return samosa_http_json_error(fd, 404, "not_found", "Endpoint not found.");
}

int main(int argc, char** argv) {
    std::string model_dir;
    int port = 8080;
    bool self_test = false;
    std::string test_prompt;
    int max_tokens = 32;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--model-dir" && i + 1 < argc) {
            model_dir = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--self-test") {
            self_test = true;
        } else if (arg == "--prompt" && i + 1 < argc) {
            test_prompt = argv[++i];
        } else if (arg == "--max-tokens" && i + 1 < argc) {
            max_tokens = std::stoi(argv[++i]);
        }
    }

    if (model_dir.empty()) {
        std::cerr << "Usage: " << argv[0] << " --model-dir <path> [--port <port>] [--self-test] [--prompt <text>] [--max-tokens <N>]" << std::endl;
        return 1;
    }

    if (!test_prompt.empty()) {
        // Fast parity testing mode (no HTTP server)
        MapleModel model = load_maple_model(model_dir);
        MapleTokenizer tokenizer(model_dir);

        std::vector<Message> msgs = {{"user", test_prompt}};
        std::string prompt = tokenizer.apply_chat_template(msgs, true);
        auto input_ids = tokenizer.encode(prompt);

        // Always output the formatted prompt for chat-template parity
        std::cout << "PROMPT: " << prompt << std::endl;
        std::cout << "PROMPT_IDS: ";
        for (size_t i = 0; i < input_ids.size(); i++) {
            std::cout << input_ids[i] << (i + 1 == input_ids.size() ? "" : ",");
        }
        std::cout << std::endl;

        if (max_tokens == 0) {
            // Tokenizer/template-only mode
            return 0;
        }

        std::vector<KVCache*> caches;
        for (const auto& ltype : model.args().layer_types) {
            if (ltype == "sliding_attention") {
                caches.push_back(new RotatingKVCache(model.args().sliding_window));
            } else {
                caches.push_back(new KVCache());
            }
        }

        std::vector<int> out_tokens;

        // Prefill
        mlx::core::array x(input_ids.begin(), {(int)input_ids.size()}, mlx::core::int32);
        x = mlx::core::reshape(x, {1, (int)input_ids.size()});
        auto logits = model.streaming_enabled()
                          ? model.run_token_sequence(x, &caches)
                          : model(x, &caches);
        auto last_logits = model.streaming_enabled()
                               ? logits
                               : mlx::core::slice(logits, {0, (int)input_ids.size() - 1, 0},
                                                  {1, (int)input_ids.size(), logits.shape(-1)});
        int next_token = sample_argmax(last_logits);
        out_tokens.push_back(next_token);

        // Decode
        for (int i = 1; i < max_tokens; i++) {
            x = mlx::core::array({next_token});
            x = mlx::core::reshape(x, {1, 1});
            logits = model(x, &caches);
            last_logits = mlx::core::slice(logits, {0, 0, 0}, {1, 1, logits.shape(-1)});
            next_token = sample_argmax(last_logits);
            out_tokens.push_back(next_token);
        }

        for (auto c : caches) delete c;

        std::cout << "TOKENS: ";
        for (size_t i = 0; i < out_tokens.size(); i++) {
            std::cout << out_tokens[i] << (i + 1 == out_tokens.size() ? "" : ",");
        }
        std::cout << std::endl;
        return 0;
    }

    std::cout << "Loading model from " << model_dir << "..." << std::endl;
    MapleModel model = load_maple_model(model_dir);

    std::cout << "Loading tokenizer from " << model_dir << "..." << std::endl;
    MapleTokenizer tokenizer(model_dir);

    if (self_test) {
        std::cout << "Running self-test..." << std::endl;
        std::vector<Message> msgs = {{"user", "Hello!"}};
        std::string prompt = tokenizer.apply_chat_template(msgs, true);
        auto input_ids = tokenizer.encode(prompt);

        std::cout << "Prompt: " << prompt << std::endl;

        std::vector<KVCache*> caches;
        for (int layer = 0; layer < model.args().num_hidden_layers; ++layer) {
            const bool sliding = layer < static_cast<int>(model.args().layer_types.size()) &&
                                  model.args().layer_types[layer] == "sliding_attention";
            if (sliding) {
                caches.push_back(new RotatingKVCache(model.args().sliding_window));
            } else {
                caches.push_back(new KVCache());
            }
        }

        mlx::core::array x(input_ids.begin(), {(int)input_ids.size()}, mlx::core::int32);
        x = mlx::core::reshape(x, {1, (int)input_ids.size()});
        auto logits = model.streaming_enabled()
                          ? model.run_token_sequence(x, &caches)
                          : model(x, &caches);
        auto last_logits = model.streaming_enabled()
                               ? logits
                               : mlx::core::slice(logits, {0, (int)input_ids.size() - 1, 0},
                                                  {1, (int)input_ids.size(), logits.shape(-1)});
        int next_token = sample_argmax(last_logits);
        input_ids.push_back(next_token);

        for (int i = 1; i < 5; ++i) {
            x = mlx::core::array({next_token});
            x = mlx::core::reshape(x, {1, 1});
            logits = model(x, &caches);
            last_logits = mlx::core::slice(logits, {0, 0, 0}, {1, 1, logits.shape(-1)});
            next_token = sample_argmax(last_logits);
            input_ids.push_back(next_token);
        }

        for (auto* cache : caches) delete cache;

        std::cout << "Self-test completed successfully. " << input_ids.size() << " tokens generated." << std::endl;
        return 0;
    }

    ServerContext ctx = {&model, &tokenizer};

    SamosaHttpServer server;
    if (!samosa_http_server_init(&server, port, samosa_maple_handler, &ctx)) {
        std::cerr << "Failed to bind to port " << port << std::endl;
        return 1;
    }

    std::cout << "Maple HTTP Server running on http://127.0.0.1:" << port << std::endl;
    samosa_http_server_run(&server);
    samosa_http_server_destroy(&server);

    return 0;
}
