#include "maple_model.h"
#include "tokenizer.h"
#include "../samosa_http.h"
#include "../json.h"

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <chrono>

using namespace samosa::maple;

struct ServerContext {
    MapleModel* model;
    MapleTokenizer* tokenizer;
};

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
                if (role && content && role->t == J_STR && content->t == J_STR) {
                    msgs.push_back({role->str, content->str});
                }
            }
        }
        
        bool stream = false;
        jval* stream_val = json_get(root, "stream");
        if (stream_val && stream_val->t == J_BOOL) {
            stream = stream_val->boolean;
        }
        
        json_free(root);
        free(arena);
        
        std::string prompt = server_ctx->tokenizer->apply_chat_template(msgs, true);
        auto input_ids = server_ctx->tokenizer->encode(prompt);
        
        // Very naive generation loop (no KV cache for now, just to test server structure)
        // A production implementation would maintain KV cache and step through it.
        int max_tokens = 50;
        int eos_token = server_ctx->tokenizer->get_eos_token();
        
        std::vector<int> generated_tokens;
        
        if (stream) {
            samosa_http_stream_headers(fd);
        }
        
        for (int i = 0; i < max_tokens; i++) {
            // Forward pass
            mlx::core::array x(input_ids.begin(), {(int)input_ids.size()}, mlx::core::int32);
            
            // Model expects shape (B, L)
            x = mlx::core::reshape(x, {1, (int)input_ids.size()});
            auto logits = (*server_ctx->model)(x);
            
            // Get last token logits: shape is (1, L, V), we want (1, 1, V) -> V
            auto last_logits = mlx::core::slice(logits, {0, (int)input_ids.size() - 1, 0}, {1, (int)input_ids.size(), logits.shape(-1)});
            int next_token = sample_argmax(last_logits);
            
            generated_tokens.push_back(next_token);
            input_ids.push_back(next_token);
            
            if (stream) {
                std::string text = server_ctx->tokenizer->decode({next_token});
                
                // Escape newlines for SSE
                std::string sse_text;
                for (char c : text) {
                    if (c == '\n') sse_text += "\\n";
                    else if (c == '"') sse_text += "\\\"";
                    else sse_text += c;
                }
                
                char chunk[1024];
                snprintf(chunk, sizeof(chunk), "data: {\"choices\": [{\"delta\": {\"content\": \"%s\"}}]}\n\n", sse_text.c_str());
                samosa_send_all(fd, chunk, strlen(chunk));
            }
            
            if (next_token == eos_token) {
                break;
            }
        }
        
        if (stream) {
            const char* end_chunk = "data: [DONE]\n\n";
            samosa_send_all(fd, end_chunk, strlen(end_chunk));
        } else {
            std::string text = server_ctx->tokenizer->decode(generated_tokens);
            // Construct JSON response
            char body[8192];
            char escaped_text[4096];
            samosa_json_escape(escaped_text, sizeof(escaped_text), text.c_str());
            snprintf(body, sizeof(body), 
                "{\"choices\": [{\"message\": {\"role\": \"assistant\", \"content\": \"%s\"}}]}", 
                escaped_text);
                
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
        
        std::vector<int> out_tokens;
        for (int i = 0; i < max_tokens; i++) {
            mlx::core::array x(input_ids.begin(), {(int)input_ids.size()}, mlx::core::int32);
            x = mlx::core::reshape(x, {1, (int)input_ids.size()});
            auto logits = model(x);
            auto last_logits = mlx::core::slice(logits, {0, (int)input_ids.size() - 1, 0}, {1, (int)input_ids.size(), logits.shape(-1)});
            int next_token = sample_argmax(last_logits);
            input_ids.push_back(next_token);
            out_tokens.push_back(next_token);
        }
        
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
        
        for (int i = 0; i < 5; i++) {
            mlx::core::array x(input_ids.begin(), {(int)input_ids.size()}, mlx::core::int32);
            x = mlx::core::reshape(x, {1, (int)input_ids.size()});
            auto logits = model(x);
            auto last_logits = mlx::core::slice(logits, {0, (int)input_ids.size() - 1, 0}, {1, (int)input_ids.size(), logits.shape(-1)});
            int next_token = sample_argmax(last_logits);
            input_ids.push_back(next_token);
        }
        
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
