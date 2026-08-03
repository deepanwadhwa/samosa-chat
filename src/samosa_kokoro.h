/* Minimal, pinned subset of Sherpa-ONNX's public C ABI used by Samosa.
 *
 * This is deliberately a C header: the gateway loads the official native
 * library at runtime after the user downloads it, without Python, pip, a
 * virtualenv, CMake, or a linker-time dependency.  Keep the layouts in lock
 * step with Sherpa-ONNX v1.13.4's c-api.h.
 */
#ifndef SAMOSA_KOKORO_H
#define SAMOSA_KOKORO_H

#include <stdint.h>

typedef struct { const char *model, *lexicon, *tokens, *data_dir; float noise_scale, noise_scale_w, length_scale; const char *dict_dir; } SamosaSherpaVitsConfig;
typedef struct { const char *acoustic_model, *vocoder, *lexicon, *tokens, *data_dir; float noise_scale, length_scale; const char *dict_dir; } SamosaSherpaMatchaConfig;
typedef struct { const char *model, *voices, *tokens, *data_dir; float length_scale; const char *dict_dir, *lexicon, *lang; } SamosaSherpaKokoroConfig;
typedef struct { const char *model, *voices, *tokens, *data_dir; float length_scale; } SamosaSherpaKittenConfig;
typedef struct { const char *tokens, *encoder, *decoder, *vocoder, *data_dir, *lexicon; float feat_scale, t_shift, target_rms, guidance_scale; } SamosaSherpaZipVoiceConfig;
typedef struct { const char *lm_flow, *lm_main, *encoder, *decoder, *text_conditioner, *vocab_json, *token_scores_json; int32_t voice_embedding_cache_capacity; } SamosaSherpaPocketConfig;
typedef struct { const char *duration_predictor, *text_encoder, *vector_estimator, *vocoder, *tts_json, *unicode_indexer, *voice_style; } SamosaSherpaSupertonicConfig;
typedef struct {
    SamosaSherpaVitsConfig vits;
    int32_t num_threads, debug;
    const char *provider;
    SamosaSherpaMatchaConfig matcha;
    SamosaSherpaKokoroConfig kokoro;
    SamosaSherpaKittenConfig kitten;
    SamosaSherpaZipVoiceConfig zipvoice;
    SamosaSherpaPocketConfig pocket;
    SamosaSherpaSupertonicConfig supertonic;
} SamosaSherpaTtsModelConfig;
typedef struct { SamosaSherpaTtsModelConfig model; const char *rule_fsts; int32_t max_num_sentences; const char *rule_fars; float silence_scale; } SamosaSherpaTtsConfig;
typedef struct { const float *samples; int32_t n, sample_rate; } SamosaSherpaGeneratedAudio;
typedef struct { float silence_scale, speed; int32_t sid; const float *reference_audio; int32_t reference_audio_len, reference_sample_rate; const char *reference_text; int32_t num_steps; const char *extra; } SamosaSherpaGenerationConfig;
typedef struct SamosaSherpaOfflineTts SamosaSherpaOfflineTts;

typedef int32_t (*SamosaSherpaGeneratedAudioProgressCallbackWithArg)(
    const float *samples, int32_t n, float progress, void *arg);

typedef const SamosaSherpaOfflineTts *(*SamosaSherpaCreateTts)(const SamosaSherpaTtsConfig *);
typedef void (*SamosaSherpaDestroyTts)(const SamosaSherpaOfflineTts *);
typedef const SamosaSherpaGeneratedAudio *(*SamosaSherpaGenerateTts)(const SamosaSherpaOfflineTts *, const char *, const SamosaSherpaGenerationConfig *, SamosaSherpaGeneratedAudioProgressCallbackWithArg, void *);
typedef void (*SamosaSherpaDestroyAudio)(const SamosaSherpaGeneratedAudio *);

#endif
