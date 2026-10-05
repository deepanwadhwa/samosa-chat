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
#include <algorithm>

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

/* Find the largest complete UTF-8 prefix that fits this model's actual
   encoder limit. Character heuristics otherwise create unnecessary map calls
   for English and can overflow the encoder for other writing systems. */
static std::vector<std::string> split_prompt(llama_model *model, const std::string &prompt) {
    const llama_vocab *vocab = llama_model_get_vocab(model);
    const std::string prefix = "summarize: ";
    std::string source = prompt.compare(0, prefix.size(), prefix) ? prompt : prompt.substr(prefix.size());
    std::vector<std::string> parts;
    size_t offset = 0;
    while (offset < source.size()) {
        size_t low = 1, high = source.size() - offset, best = 0;
        while (low <= high) {
            size_t middle = low + (high - low) / 2;
            size_t end = middle;
            while (end && end < source.size() - offset &&
                (static_cast<unsigned char>(source[offset + end]) & 0xc0) == 0x80) end--;
            std::string candidate = prefix + source.substr(offset, end);
            int tokens = -llama_tokenize(vocab, candidate.data(), candidate.size(), nullptr, 0, true, true);
            if (end && tokens > 0 && tokens <= 512) { best = end; low = middle + 1; }
            else high = middle - 1;
        }
        if (!best) return {};
        if (best < source.size() - offset) {
            for (size_t at = best; at > best * 3 / 4; at--) {
                char c = source[offset + at - 1];
                if (c == '\n' || c == '.' || c == '!' || c == '?') { best = at; break; }
            }
        }
        parts.push_back(prefix + source.substr(offset, best)); offset += best;
    }
    return parts;
}

/* T5 can loop on boilerplate. Once it emits the same complete substantial
   sentence twice, retain the first occurrence and finish that sequence.
   This is output repetition detection, independent of question/file names. */
static bool stop_repeated_sentence(std::string &output) {
    size_t end = output.find_last_not_of(" \t\r\n");
    if (end == std::string::npos || output[end] != '.') return false;
    size_t previous = output.rfind('.', end ? end - 1 : 0);
    if (previous == std::string::npos || previous == end) return false;
    size_t begin = output.find_first_not_of(" \t\r\n", previous + 1);
    if (begin == std::string::npos || end - begin < 24) return false;
    std::string sentence = output.substr(begin, end - begin + 1);
    size_t at = 0;
    while (at < previous) {
        size_t finish = output.find('.', at);
        if (finish == std::string::npos || finish > previous) break;
        size_t start = output.find_first_not_of(" \t\r\n", at);
        if (start != std::string::npos && output.substr(start, finish - start + 1) == sentence) {
            output.resize(previous + 1); return true;
        }
        at = finish + 1;
    }
    return false;
}

static std::string summarize(llama_model *model, llama_context *ctx, const std::string &prompt,
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

    /* Keep context/Metal allocations resident across chunks. Clear decoder
       history before encoding a new source so files cannot contaminate one another. */
    llama_memory_clear(llama_get_memory(ctx), true);

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
        if (stop_repeated_sentence(output)) break;
        batch = llama_batch_get_one(&token, 1);
    }

    llama_sampler_free(sampler);
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

/* Encode independent sources with distinct sequence IDs, then decode one
   token per live sequence in a shared GPU batch. Attention masks keep both
   encoder text and decoder history isolated for every file. */
static std::vector<std::string> summarize_many(llama_model *model, llama_context *ctx,
        const std::vector<std::string> &prompts, int max_tokens) {
    const llama_vocab *vocab = llama_model_get_vocab(model);
    std::vector<std::string> outputs(prompts.size());
    std::vector<std::vector<llama_token>> tokens(prompts.size());
    int total = 0;
    for (size_t i = 0; i < prompts.size(); i++) {
        int count = -llama_tokenize(vocab, prompts[i].data(), prompts[i].size(), nullptr, 0, true, true);
        if (count <= 0 || count > 512) return {};
        tokens[i].resize(count);
        if (llama_tokenize(vocab, prompts[i].data(), prompts[i].size(), tokens[i].data(), count, true, true) < 0) return {};
        total += count;
    }
    llama_memory_clear(llama_get_memory(ctx), true);
    llama_batch encoder = llama_batch_init(total, 0, 1);
    encoder.n_tokens = 0;
    for (size_t sequence = 0; sequence < tokens.size(); sequence++) {
        for (size_t position = 0; position < tokens[sequence].size(); position++) {
            int at = encoder.n_tokens++;
            encoder.token[at] = tokens[sequence][position]; encoder.pos[at] = position;
            encoder.n_seq_id[at] = 1; encoder.seq_id[at][0] = sequence; encoder.logits[at] = 0;
        }
    }
    int error = llama_encode(ctx, encoder); llama_batch_free(encoder);
    if (error) return {};
    llama_token start = llama_model_decoder_start_token(model);
    if (start == LLAMA_TOKEN_NULL) start = llama_vocab_bos(vocab);
    if (start == LLAMA_TOKEN_NULL) return {};
    llama_batch decoder = llama_batch_init(prompts.size(), 0, 1);
    std::vector<size_t> active;
    for (size_t i = 0; i < prompts.size(); i++) {
        active.push_back(i); decoder.token[i] = start; decoder.pos[i] = 0;
        decoder.n_seq_id[i] = 1; decoder.seq_id[i][0] = i; decoder.logits[i] = 1;
    }
    decoder.n_tokens = prompts.size();
    for (int generated = 0; generated < max_tokens && !active.empty(); generated++) {
        if (llama_decode(ctx, decoder)) { error = 1; break; }
        std::vector<size_t> next;
        std::vector<llama_token> chosen;
        for (size_t row = 0; row < active.size(); row++) {
            const float *logits = llama_get_logits_ith(ctx, row);
            if (!logits) { error = 1; break; }
            llama_token best = 0;
            for (int token = 1; token < llama_vocab_n_tokens(vocab); token++) if (logits[token] > logits[best]) best = token;
            if (llama_vocab_is_eog(vocab, best)) continue;
            char piece[256]; int bytes = llama_token_to_piece(vocab, best, piece, sizeof(piece), 0, true);
            if (bytes < 0) { error = 1; break; }
            outputs[active[row]].append(piece, bytes);
            if (stop_repeated_sentence(outputs[active[row]])) continue;
            next.push_back(active[row]); chosen.push_back(best);
        }
        if (error) break;
        active = next; decoder.n_tokens = active.size();
        for (size_t row = 0; row < active.size(); row++) {
            decoder.token[row] = chosen[row]; decoder.pos[row] = generated + 1;
            decoder.n_seq_id[row] = 1; decoder.seq_id[row][0] = active[row]; decoder.logits[row] = 1;
        }
    }
    llama_batch_free(decoder);
    if (error) return {};
    for (auto &output : outputs) {
        auto first = output.find_first_not_of(" \t\r\n"), last = output.find_last_not_of(" \t\r\n");
        output = first == std::string::npos ? "" : output.substr(first, last - first + 1);
    }
    return outputs;
}

int main(int argc, char **argv) {
    std::setlocale(LC_NUMERIC, "C");
    const char *model_path = nullptr;
    int gpu_layers = 99;
    int max_tokens = 128;
    int batch_size = 2, threads = 2, memory_max_tokens = 64;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc)
            model_path = argv[++i];
        else if (!strcmp(argv[i], "--gpu-layers") && i + 1 < argc)
            gpu_layers = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-tokens") && i + 1 < argc)
            max_tokens = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--batch-size") && i + 1 < argc) batch_size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--memory-max-tokens") && i + 1 < argc) memory_max_tokens = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: %s --model MODEL [--gpu-layers N] [--max-tokens N]\n",
                    argv[0]);
            return 2;
        }
    }
    if (!model_path || max_tokens < 16 || max_tokens > 256) return 2;
    if (batch_size < 1 || batch_size > 4 || threads < 1 || threads > 8 || memory_max_tokens < 32 || memory_max_tokens > 128) return 2;

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
    llama_context_params params = llama_context_default_params();
    params.n_ctx = static_cast<uint32_t>(512 + max_tokens);
    params.n_batch = 512;
    params.n_threads = params.n_threads_batch = threads;
    params.no_perf = true;
    /* One small context handles legacy scalar requests; one context batches
       isolated sequences. Both share one resident model. Two encoder sequences
       measured faster than four on 3,000-character samples (less masked work). */
    llama_context *scalar = llama_init_from_model(model, params);
    params.n_ctx = (512 + max_tokens) * batch_size;
    params.n_batch = params.n_ubatch = 512 * batch_size;
    params.n_seq_max = batch_size;
    params.kv_unified = true;
    llama_context *batched = llama_init_from_model(model, params);
    if (!scalar || !batched) {
        if (scalar) llama_free(scalar);
        if (batched) llama_free(batched);
        llama_model_free(model); return 3;
    }
    for (;;) {
        uint32_t encoded = 0;
        if (!read_exact(&encoded, sizeof(encoded))) break;
        uint32_t length = ntohl(encoded);
        if (length & 0x80000000U) {
            int batch_max_tokens = (length & 0x40000000U) ? std::min(max_tokens, memory_max_tokens) : max_tokens;
            uint32_t count = length & 0x3fffffffU;
            if (!count || count > 64) break;
            std::vector<std::string> prompts(count), replies(count);
            bool valid = true;
            for (auto &prompt : prompts) {
                if (!read_exact(&encoded, sizeof(encoded))) { valid = false; break; }
                uint32_t bytes = ntohl(encoded);
                if (!bytes || bytes > 65536) { valid = false; break; }
                prompt.resize(bytes);
                if (!read_exact(prompt.data(), bytes)) { valid = false; break; }
            }
            if (!valid) break;
            std::vector<std::string> chunks, chunk_replies;
            std::vector<size_t> owner;
            for (size_t i = 0; i < prompts.size(); i++) {
                auto parts = split_prompt(model, prompts[i]);
                if (parts.empty()) { valid = false; break; }
                for (auto &part : parts) { owner.push_back(i); chunks.push_back(std::move(part)); }
            }
            chunk_replies.resize(chunks.size());
            for (size_t at = 0; at < chunks.size(); at += batch_size) {
                size_t stop = std::min(chunks.size(), at + batch_size);
                std::vector<std::string> group(chunks.begin() + at, chunks.begin() + stop);
                auto generated = summarize_many(model, batched, group, batch_max_tokens);
                if (generated.size() != group.size()) { valid = false; break; }
                for (size_t i = 0; i < generated.size(); i++) chunk_replies[at + i] = std::move(generated[i]);
            }
            std::vector<bool> failed(count, !valid);
            for (size_t i = 0; i < chunks.size(); i++) {
                if (chunk_replies[i].empty()) failed[owner[i]] = true;
                else { if (!replies[owner[i]].empty()) replies[owner[i]] += "\n"; replies[owner[i]] += chunk_replies[i]; }
            }
            for (size_t i = 0; i < replies.size(); i++) if (failed[i]) replies[i].clear();
            for (const auto &reply : replies) if (!write_reply(reply)) { valid = false; break; }
            if (!valid) break;
            continue;
        }
        if (!length || length > 65536) break;
        std::string prompt(length, '\0');
        if (!read_exact(prompt.data(), prompt.size())) break;
        if (!write_reply(summarize(model, scalar, prompt, max_tokens))) break;
    }
    llama_free(scalar); llama_free(batched);
    llama_model_free(model);
    return 0;
}
