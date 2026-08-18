/* Benchmark the exact native Kokoro runtime installed by Samosa.
 *
 * Usage:
 *   cc -O2 -Isrc tools/benchmark_kokoro_native.c -o /tmp/samosa-kokoro-bench
 *   /tmp/samosa-kokoro-bench LIB MODEL VOICES TOKENS DATA PROVIDER THREADS [TEXT] [SPEED]
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "samosa_kokoro.h"

typedef struct {
    long long started_ms;
    long long first_ms;
    int callbacks;
} Progress;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int32_t progress_callback(const float *samples, int32_t n,
                                 float progress, void *opaque) {
    (void)samples;
    (void)progress;
    Progress *state = opaque;
    if (n > 0 && !state->first_ms) state->first_ms = now_ms();
    if (n > 0) state->callbacks++;
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 8) {
        fprintf(stderr, "usage: %s LIB MODEL VOICES TOKENS DATA PROVIDER THREADS [TEXT] [SPEED]\n", argv[0]);
        return 2;
    }
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    SamosaSherpaCreateTts create = (SamosaSherpaCreateTts)dlsym(library, "SherpaOnnxCreateOfflineTts");
    SamosaSherpaDestroyTts destroy = (SamosaSherpaDestroyTts)dlsym(library, "SherpaOnnxDestroyOfflineTts");
    SamosaSherpaGenerateTts generate = (SamosaSherpaGenerateTts)dlsym(library, "SherpaOnnxOfflineTtsGenerateWithConfig");
    SamosaSherpaDestroyAudio destroy_audio = (SamosaSherpaDestroyAudio)dlsym(library, "SherpaOnnxDestroyOfflineTtsGeneratedAudio");
    if (!create || !destroy || !generate || !destroy_audio) {
        fprintf(stderr, "native Kokoro symbols are incomplete\n");
        dlclose(library);
        return 1;
    }

    SamosaSherpaTtsConfig config;
    memset(&config, 0, sizeof(config));
    config.model.kokoro.model = argv[2];
    config.model.kokoro.voices = argv[3];
    config.model.kokoro.tokens = argv[4];
    config.model.kokoro.data_dir = argv[5];
    config.model.provider = argv[6];
    config.model.num_threads = atoi(argv[7]);
    config.max_num_sentences = 1;

    long long init_started = now_ms();
    const SamosaSherpaOfflineTts *tts = create(&config);
    long long init_done = now_ms();
    if (!tts) {
        fprintf(stderr, "Kokoro initialization failed for provider=%s threads=%s\n", argv[6], argv[7]);
        dlclose(library);
        return 1;
    }

    const char *text = argc > 8 ? argv[8] :
        "The total land area of the United States is approximately 3.5 million square miles.";
    SamosaSherpaGenerationConfig generation;
    memset(&generation, 0, sizeof(generation));
    generation.silence_scale = 0.12f;
    generation.speed = argc > 9 ? strtof(argv[9], NULL) : 1.0f;
    generation.sid = 1;
    Progress progress = {.started_ms = now_ms()};
    const SamosaSherpaGeneratedAudio *audio = generate(
        tts, text, &generation, progress_callback, &progress);
    long long generation_done = now_ms();
    if (!audio || !audio->samples || audio->n <= 0 || audio->sample_rate <= 0) {
        fprintf(stderr, "Kokoro generation failed\n");
        if (audio) destroy_audio(audio);
        destroy(tts);
        dlclose(library);
        return 1;
    }
    double audio_ms = (double)audio->n * 1000.0 / audio->sample_rate;
    long long generation_ms = generation_done - progress.started_ms;
    printf("provider=%s threads=%d speed=%.2f init_ms=%lld first_pcm_ms=%lld generation_ms=%lld audio_ms=%.1f rtf=%.3f callbacks=%d\n",
           argv[6], config.model.num_threads, generation.speed, init_done - init_started,
           progress.first_ms ? progress.first_ms - progress.started_ms : -1,
           generation_ms, audio_ms, generation_ms / audio_ms, progress.callbacks);
    destroy_audio(audio);
    destroy(tts);
    dlclose(library);
    return 0;
}
