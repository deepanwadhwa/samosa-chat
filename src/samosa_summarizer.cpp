/*
 * Persistent native text-summarization sidecar for Samosa.
 *
 * The inference loop is adapted from llama.cpp's MIT-licensed `simple`
 * encoder/decoder example.  Keeping it here matters: llama-server and the
 * generic completion front end currently terminate T5 generation at its
 * decoder-start token, while the encoder/decoder API produces the expected
 * Falconsai summaries.
 *
 * Protocol (stdin/stdout): repeated big-endian uint32 length + UTF-8 bytes.
 * A zero-length reply reports a failed request.  The model stays resident so
 * eight web articles do not pay model load / Metal graph setup eight times.
 */

#include "llama.h"

#include <arpa/inet.h>
#include <cerrno>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

static bool read_exact(void *buffer, size_t length) {
    unsigned char *cursor = static_cast<unsigned char *>(buffer);
    while (length) {
        ssize_t count = read(STDIN_FILENO, cursor, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        cursor += count;
        length -= static_cast<size_t>(count);
    }
    return true;
}

static bool write_exact(const void *buffer, size_t length) {
    const unsigned char *cursor = static_cast<const unsigned char *>(buffer);
    while (length) {
        ssize_t count = write(STDOUT_FILENO, cursor, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        cursor += count;
        length -= static_cast<size_t>(count);
    }
    return true;
}

static bool write_reply(const std::string &text) {
    if (text.size() > UINT32_MAX) return false;
    uint32_t length = htonl(static_cast<uint32_t>(text.size()));
    return write_exact(&length, sizeof(length)) &&
           (text.empty() || write_exact(text.data(), text.size()));
}

static std::string summarize(llama_model *model, const std::string &prompt,
                             int max_tokens) {
    const llama_vocab *vocab = llama_model_get_vocab(model);
    int count = llama_tokenize(vocab, prompt.data(), prompt.size(), nullptr, 0,
                               true, true);
    if (count >= 0) return {};
    count = -count;
    if (count <= 0 || count > 512) return {};

    std::vector<llama_token> prompt_tokens(static_cast<size_t>(count));
    if (llama_tokenize(vocab, prompt.data(), prompt.size(), prompt_tokens.data(),
                       prompt_tokens.size(), true, true) < 0)
        return {};

    llama_context_params params = llama_context_default_params();
    params.n_ctx = static_cast<uint32_t>(count + max_tokens);
    params.n_batch = static_cast<uint32_t>(count);
    params.no_perf = true;
    llama_context *ctx = llama_init_from_model(model, params);
    if (!ctx) return {};

    llama_sampler_chain_params sampler_params =
        llama_sampler_chain_default_params();
    sampler_params.no_perf = true;
    llama_sampler *sampler = llama_sampler_chain_init(sampler_params);
    llama_sampler_chain_add(sampler, llama_sampler_init_greedy());

    llama_batch batch =
        llama_batch_get_one(prompt_tokens.data(), prompt_tokens.size());
    bool failed = false;
    if (!llama_model_has_encoder(model) || llama_encode(ctx, batch)) {
        failed = true;
    } else {
        llama_token start = llama_model_decoder_start_token(model);
        if (start == LLAMA_TOKEN_NULL) start = llama_vocab_bos(vocab);
        if (start == LLAMA_TOKEN_NULL) {
            failed = true;
        } else {
            batch = llama_batch_get_one(&start, 1);
        }
    }

    std::string output;
    for (int generated = 0; !failed && generated < max_tokens; ++generated) {
        if (llama_decode(ctx, batch)) {
            failed = true;
            break;
        }
        llama_token token = llama_sampler_sample(sampler, ctx, -1);
        if (llama_vocab_is_eog(vocab, token)) break;
        char piece[256];
        int bytes = llama_token_to_piece(vocab, token, piece, sizeof(piece), 0,
                                         true);
        if (bytes < 0) {
            failed = true;
            break;
        }
        output.append(piece, static_cast<size_t>(bytes));
        batch = llama_batch_get_one(&token, 1);
    }

    llama_sampler_free(sampler);
    llama_free(ctx);
    if (failed) return {};
    while (!output.empty() &&
           (output.back() == '\n' || output.back() == '\r' ||
            output.back() == ' ' || output.back() == '\t'))
        output.pop_back();
    size_t first = output.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    if (first) output.erase(0, first);
    return output;
}

int main(int argc, char **argv) {
    std::setlocale(LC_NUMERIC, "C");
    const char *model_path = nullptr;
    int gpu_layers = 99;
    int max_tokens = 128;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc)
            model_path = argv[++i];
        else if (!strcmp(argv[i], "--gpu-layers") && i + 1 < argc)
            gpu_layers = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-tokens") && i + 1 < argc)
            max_tokens = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: %s --model MODEL [--gpu-layers N] [--max-tokens N]\n",
                    argv[0]);
            return 2;
        }
    }
    if (!model_path || max_tokens < 16 || max_tokens > 256) return 2;

    ggml_backend_load_all();
    llama_log_set([](enum ggml_log_level, const char *, void *) {}, nullptr);
    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = gpu_layers;
    ggml_backend_dev_t cpu_devices[2] = {nullptr, nullptr};
    if (gpu_layers <= 0) {
        cpu_devices[0] =
            ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
        model_params.devices = cpu_devices;
    }
    llama_model *model = llama_model_load_from_file(model_path, model_params);
    if (!model) return 3;

    for (;;) {
        uint32_t encoded = 0;
        if (!read_exact(&encoded, sizeof(encoded))) break;
        uint32_t length = ntohl(encoded);
        if (!length || length > 65536) break;
        std::string prompt(length, '\0');
        if (!read_exact(prompt.data(), prompt.size())) break;
        if (!write_reply(summarize(model, prompt, max_tokens))) break;
    }
    llama_model_free(model);
    return 0;
}
