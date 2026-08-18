/* A tiny native Sherpa-ONNX C-ABI stand-in for the gateway contract test.
 * It proves that Samosa dlopens a C runtime, configures legacy Kokoro or
 * streaming Pocket, and forwards float PCM without Python or a voice server. */
#include <string.h>

#include "samosa_kokoro.h"

struct SamosaSherpaOfflineTts { int pocket; };
static struct SamosaSherpaOfflineTts tts;
static const float samples[] = {0.0f, 0.30f, -0.30f, 0.0f};
static const SamosaSherpaGeneratedAudio audio = {samples, 4, 24000};

const SamosaSherpaOfflineTts *SherpaOnnxCreateOfflineTts(const SamosaSherpaTtsConfig *config) {
    if (!config || !config->model.provider || strcmp(config->model.provider, "cpu")) return NULL;
    if (config->model.pocket.lm_flow) {
        if (!config->model.pocket.lm_main || !config->model.pocket.encoder ||
            !config->model.pocket.decoder || !config->model.pocket.text_conditioner ||
            !config->model.pocket.vocab_json || !config->model.pocket.token_scores_json ||
            config->model.pocket.voice_embedding_cache_capacity != 4 ||
            config->model.num_threads != 2)
            return NULL;
        tts.pocket = 1;
    } else {
        if (!config->model.kokoro.model || !config->model.kokoro.voices ||
            !config->model.kokoro.tokens || !config->model.kokoro.data_dir ||
            config->model.num_threads != 6 || config->max_num_sentences != 1)
            return NULL;
        tts.pocket = 0;
    }
    return &tts;
}

void SherpaOnnxDestroyOfflineTts(const SamosaSherpaOfflineTts *value) { (void)value; }

int32_t SherpaOnnxOfflineTtsSampleRate(const SamosaSherpaOfflineTts *value) {
    return value == &tts ? 24000 : 0;
}

const SamosaSherpaGeneratedAudio *SherpaOnnxOfflineTtsGenerateWithConfig(
    const SamosaSherpaOfflineTts *value, const char *text,
    const SamosaSherpaGenerationConfig *config,
    SamosaSherpaGeneratedAudioProgressCallbackWithArg callback, void *arg) {
    if (value != &tts || !text || !text[0] || !config)
        return NULL;
    if (tts.pocket ? (!config->reference_audio || config->reference_audio_len <= 0 ||
                   config->reference_sample_rate != 16000 || config->num_steps != 5 ||
                      config->speed != 1.0f)
                   : (config->sid != 3 || config->speed != 1.15f))
        return NULL;
    if (callback && (!callback(samples, 2, 0.5f, arg) || !callback(samples + 2, 2, 1.0f, arg)))
        return NULL;
    return &audio;
}

void SherpaOnnxDestroyOfflineTtsGeneratedAudio(const SamosaSherpaGeneratedAudio *value) { (void)value; }
