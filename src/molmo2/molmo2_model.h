#ifndef SAMOSA_MOLMO2_MODEL_H
#define SAMOSA_MOLMO2_MODEL_H

#include "molmo2_processor.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace samosa::molmo2 {

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
