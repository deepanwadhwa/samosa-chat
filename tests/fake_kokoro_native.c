/* A tiny native Sherpa-ONNX C-ABI stand-in for the gateway contract test.
 * It proves that Samosa dlopens a C runtime, configures Kokoro, and turns its
 * float PCM into a WAV without a Python process or a loopback voice server. */
#include <string.h>

#include "samosa_kokoro.h"

struct SamosaSherpaOfflineTts { int valid; };
static const struct SamosaSherpaOfflineTts tts = {1};
static const float samples[] = {0.0f, 0.30f, -0.30f, 0.0f};
static const SamosaSherpaGeneratedAudio audio = {samples, 4, 24000};

const SamosaSherpaOfflineTts *SherpaOnnxCreateOfflineTts(const SamosaSherpaTtsConfig *config) {
    if (!config || !config->model.kokoro.model || !config->model.kokoro.voices ||
        !config->model.kokoro.tokens || !config->model.kokoro.data_dir ||
        config->model.num_threads != 2 || !config->model.provider ||
        strcmp(config->model.provider, "cpu") || config->max_num_sentences != 1)
        return NULL;
    return &tts;
}

void SherpaOnnxDestroyOfflineTts(const SamosaSherpaOfflineTts *value) { (void)value; }

const SamosaSherpaGeneratedAudio *SherpaOnnxOfflineTtsGenerateWithConfig(
    const SamosaSherpaOfflineTts *value, const char *text,
    const SamosaSherpaGenerationConfig *config,
    SamosaSherpaGeneratedAudioProgressCallbackWithArg callback, void *arg) {
    (void)callback; (void)arg;
    if (value != &tts || !text || !text[0] || !config || config->sid != 3 ||
        config->speed != 1.0f)
        return NULL;
    return &audio;
}

void SherpaOnnxDestroyOfflineTtsGeneratedAudio(const SamosaSherpaGeneratedAudio *value) { (void)value; }
