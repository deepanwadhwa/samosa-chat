#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "samosa_kokoro.h"

typedef struct {
    long long started_ms;
    long long first_ms;
    long long last_ms;
    long long max_gap_ms;
    long long samples;
    int callbacks;
} Progress;

static long long now_ms(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (long long)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static uint16_t le16(const unsigned char *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_pcm16_mono(const char *path, float **samples_out,
                           int32_t *count_out, int32_t *rate_out) {
    FILE *stream = fopen(path, "rb");
    if (!stream) return 0;
    if (fseek(stream, 0, SEEK_END)) { fclose(stream); return 0; }
    long size = ftell(stream);
    if (size < 44 || fseek(stream, 0, SEEK_SET)) { fclose(stream); return 0; }
    unsigned char *bytes = malloc((size_t)size);
    if (!bytes || fread(bytes, 1, (size_t)size, stream) != (size_t)size) {
        free(bytes); fclose(stream); return 0;
    }
    fclose(stream);
    if (memcmp(bytes, "RIFF", 4) || memcmp(bytes + 8, "WAVE", 4)) {
        free(bytes); return 0;
    }
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0, data_size = 0;
    const unsigned char *data = NULL;
    for (size_t at = 12; at + 8 <= (size_t)size;) {
        uint32_t chunk_size = le32(bytes + at + 4);
        size_t next = at + 8u + chunk_size + (chunk_size & 1u);
        if (next > (size_t)size) break;
        if (!memcmp(bytes + at, "fmt ", 4) && chunk_size >= 16) {
            format = le16(bytes + at + 8);
            channels = le16(bytes + at + 10);
            rate = le32(bytes + at + 12);
            bits = le16(bytes + at + 22);
        } else if (!memcmp(bytes + at, "data", 4)) {
            data = bytes + at + 8;
            data_size = chunk_size;
        }
        at = next;
    }
    if (format != 1 || channels != 1 || bits != 16 || !rate || !data ||
        data_size / 2u > INT32_MAX) { free(bytes); return 0; }
    int32_t count = (int32_t)(data_size / 2u);
    float *samples = malloc((size_t)count * sizeof(*samples));
    if (!samples) { free(bytes); return 0; }
    for (int32_t i = 0; i < count; ++i)
        samples[i] = (float)(int16_t)le16(data + (size_t)i * 2u) / 32768.0f;
    free(bytes);
    *samples_out = samples; *count_out = count; *rate_out = (int32_t)rate;
    return 1;
}

static int32_t progress_callback(const float *samples, int32_t count,
                                 float progress, void *opaque) {
    (void)samples; (void)progress;
    Progress *timing = opaque;
    long long now = now_ms();
    if (!timing->callbacks) timing->first_ms = now;
    else if (now - timing->last_ms > timing->max_gap_ms)
        timing->max_gap_ms = now - timing->last_ms;
    timing->last_ms = now;
    timing->callbacks++;
    timing->samples += count;
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s LIB MODEL_DIR REFERENCE_WAV THREADS TEXT [STEPS] [RUNS]\n", argv[0]);
        return 2;
    }
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    SamosaSherpaCreateTts create = (SamosaSherpaCreateTts)dlsym(library, "SherpaOnnxCreateOfflineTts");
    SamosaSherpaDestroyTts destroy = (SamosaSherpaDestroyTts)dlsym(library, "SherpaOnnxDestroyOfflineTts");
    SamosaSherpaTtsSampleRate sample_rate = (SamosaSherpaTtsSampleRate)dlsym(library, "SherpaOnnxOfflineTtsSampleRate");
    SamosaSherpaGenerateTts generate = (SamosaSherpaGenerateTts)dlsym(library, "SherpaOnnxOfflineTtsGenerateWithConfig");
    SamosaSherpaDestroyAudio destroy_audio = (SamosaSherpaDestroyAudio)dlsym(library, "SherpaOnnxDestroyOfflineTtsGeneratedAudio");
    if (!create || !destroy || !sample_rate || !generate || !destroy_audio) {
        fprintf(stderr, "sherpa-onnx TTS symbols are incomplete\n"); dlclose(library); return 1;
    }
    char lm_flow[4096], lm_main[4096], encoder[4096], decoder[4096];
    char conditioner[4096], vocab[4096], scores[4096];
#define MODEL_PATH(buffer, filename) do { \
    if (snprintf((buffer), sizeof(buffer), "%s/%s", argv[2], (filename)) >= (int)sizeof(buffer)) { \
        fprintf(stderr, "model path too long\n"); dlclose(library); return 1; \
    } \
} while (0)
    MODEL_PATH(lm_flow, "lm_flow.int8.onnx");
    MODEL_PATH(lm_main, "lm_main.int8.onnx");
    MODEL_PATH(encoder, "encoder.onnx");
    MODEL_PATH(decoder, "decoder.int8.onnx");
    MODEL_PATH(conditioner, "text_conditioner.onnx");
    MODEL_PATH(vocab, "vocab.json");
    MODEL_PATH(scores, "token_scores.json");
#undef MODEL_PATH

    SamosaSherpaTtsConfig config = {0};
    config.model.pocket.lm_flow = lm_flow;
    config.model.pocket.lm_main = lm_main;
    config.model.pocket.encoder = encoder;
    config.model.pocket.decoder = decoder;
    config.model.pocket.text_conditioner = conditioner;
    config.model.pocket.vocab_json = vocab;
    config.model.pocket.token_scores_json = scores;
    config.model.pocket.voice_embedding_cache_capacity = 4;
    config.model.num_threads = atoi(argv[4]);
    config.model.provider = "cpu";
    long long create_started = now_ms();
    const SamosaSherpaOfflineTts *tts = create(&config);
    long long create_ms = now_ms() - create_started;
    if (!tts) { fprintf(stderr, "could not create Pocket TTS\n"); dlclose(library); return 1; }

    float *reference = NULL; int32_t reference_count = 0, reference_rate = 0;
    if (!read_pcm16_mono(argv[3], &reference, &reference_count, &reference_rate)) {
        fprintf(stderr, "could not read reference WAV\n"); destroy(tts); dlclose(library); return 1;
    }
    SamosaSherpaGenerationConfig generation = {0};
    generation.reference_audio = reference;
    generation.reference_audio_len = reference_count;
    generation.reference_sample_rate = reference_rate;
    generation.num_steps = argc > 6 ? atoi(argv[6]) : 1;
    generation.extra = "{\"max_reference_audio_len\":10.0,\"seed\":42}";
    int32_t rate = sample_rate(tts);
    int runs = argc > 7 ? atoi(argv[7]) : 1;
    int all_ok = 1;
    for (int run = 1; run <= runs; ++run) {
        Progress timing = {.started_ms = now_ms()};
        const SamosaSherpaGeneratedAudio *audio = generate(
            tts, argv[5], &generation, progress_callback, &timing);
        long long total_ms = now_ms() - timing.started_ms;
        long long output_samples = audio ? audio->n : timing.samples;
        printf("run=%d create_ms=%lld first_pcm_ms=%lld total_ms=%lld callbacks=%d max_callback_gap_ms=%lld callback_samples=%lld output_samples=%lld sample_rate=%d audio_ms=%.0f rtf=%.3f\n",
               run, create_ms, timing.first_ms ? timing.first_ms - timing.started_ms : -1,
               total_ms, timing.callbacks, timing.max_gap_ms, timing.samples, output_samples, rate,
               rate > 0 ? (double)output_samples * 1000.0 / rate : 0.0,
               output_samples > 0 ? (double)total_ms * rate / (double)output_samples : 0.0);
        if (audio) destroy_audio(audio); else all_ok = 0;
    }
    free(reference); destroy(tts); dlclose(library);
    return all_ok ? 0 : 1;
}
