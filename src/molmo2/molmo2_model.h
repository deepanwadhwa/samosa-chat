#ifndef SAMOSA_MOLMO2_MODEL_H
#define SAMOSA_MOLMO2_MODEL_H

#include "molmo2_processor.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace samosa::molmo2 {

inline constexpr std::size_t kSafeSequenceTokens = 2048;

/* max_new_tokens is a ceiling, not a reservation. Visual prompts vary with
   crop/frame count, so trim generation to the context that remains instead
   of rejecting an otherwise valid image or video before its first token. */
inline int bounded_generation_tokens(std::size_t prompt_tokens,
                                     int requested_tokens) {
    if (requested_tokens < 1 || prompt_tokens >= kSafeSequenceTokens) return 0;
    const std::size_t available = kSafeSequenceTokens - prompt_tokens;
    return available < static_cast<std::size_t>(requested_tokens)
        ? static_cast<int>(available) : requested_tokens;
}

struct GenerateOptions {
    int max_new_tokens = 256;
};

struct GenerateResult {
    bool ok = false;
    bool cancelled = false;
    std::string text;
    std::string error;
    int prompt_tokens = 0;
    int generated_tokens = 0;
};

class Model {
public:
    Model();
    ~Model();
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    bool load(const std::string& package_dir, std::string* error);
    bool loaded() const;
    GenerateResult generate(const std::string& question,
                            const VisualInput& visual,
                            const GenerateOptions& options,
                            const std::atomic_bool* cancelled,
                            const std::function<void(const std::string&)>& on_delta = {});
    void unload();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace samosa::molmo2

#endif
