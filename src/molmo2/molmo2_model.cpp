#include "molmo2_model.h"

#include "molmo2_contract.h"
#include "../tok.h"

#include "mlx/fast.h"
#include "mlx/io.h"
#include "mlx/memory.h"
#include "mlx/ops.h"
#include "mlx/transforms.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace samosa::molmo2 {
namespace {

using namespace mlx::core;
using Weights = std::unordered_map<std::string, array>;
constexpr std::size_t kSafeSequenceTokens = 2048;

array required(const Weights& weights, const std::string& name) {
    auto found = weights.find(name);
    if (found == weights.end()) throw std::runtime_error("checkpoint tensor missing: " + name);
    return found->second;
}

void require_shape(const Weights& weights, const std::string& name,
                   std::initializer_list<int> expected) {
    auto value = required(weights, name);
    if (value.ndim() != expected.size() ||
        !std::equal(value.shape().begin(), value.shape().end(), expected.begin(), expected.end()))
        throw std::runtime_error("checkpoint tensor has incompatible shape: " + name);
}

struct QLinear {
    array weight {0.0f}, scales {0.0f}, biases {0.0f}, output_bias {0.0f};
    bool quantized = false;
    bool has_output_bias = false;

    void load(const Weights& weights, const std::string& prefix) {
        weight = required(weights, prefix + ".weight");
        auto scale = weights.find(prefix + ".scales");
        if (scale != weights.end()) {
            scales = scale->second;
            biases = required(weights, prefix + ".biases");
            quantized = true;
        }
        auto bias = weights.find(prefix + ".bias");
        if (bias != weights.end()) { output_bias = bias->second; has_output_bias = true; }
    }

    array operator()(const array& x) const {
        auto result = quantized
            ? quantized_matmul(x, weight, scales, biases, true, 64, 4, "affine")
            : matmul(x, swapaxes(weight, -1, -2));
        return has_output_bias ? result + output_bias : result;
    }
};

array rms_norm(const array& x, const array& weight) {
    auto result = fast::rms_norm(astype(x, float32), astype(weight, float32), 1e-6f);
    return astype(result, x.dtype());
}

array layer_norm(const array& x, const array& weight, const array& bias) {
    auto xf = astype(x, float32);
    auto result = (xf - mean(xf, -1, true)) / sqrt(var(xf, -1, true) + 1e-6f);
    return astype(result, x.dtype()) * weight + bias;
}

array gelu_tanh(const array& x) {
    return 0.5f * x * (1.0f + tanh(0.7978845608f * (x + 0.044715f * x * x * x)));
}

array silu(const array& x) { return x * sigmoid(x); }

struct Cache {
    array keys {0.0f};
    array values {0.0f};
    int offset = 0;

    void update(array* key, array* value) {
        if (offset == 0) { keys = *key; values = *value; }
        else { keys = concatenate({keys, *key}, 2); values = concatenate({values, *value}, 2); }
        offset += key->shape(2);
        *key = keys; *value = values;
    }
};

struct TextLayer {
    QLinear att_proj, attn_out, ff_proj, ff_out;
    array q_norm {0.0f}, k_norm {0.0f}, attn_norm {0.0f}, ff_norm {0.0f};

    void load(const Weights& weights, int layer) {
        const std::string p = "language_model.model.blocks." + std::to_string(layer) + ".";
        att_proj.load(weights, p + "self_attn.att_proj");
        attn_out.load(weights, p + "self_attn.attn_out");
        ff_proj.load(weights, p + "mlp.ff_proj");
        ff_out.load(weights, p + "mlp.ff_out");
        q_norm = required(weights, p + "self_attn.q_norm.weight");
        k_norm = required(weights, p + "self_attn.k_norm.weight");
        attn_norm = required(weights, p + "attn_norm.weight");
        ff_norm = required(weights, p + "ff_norm.weight");
    }

    array forward(const array& input, const std::optional<array>& mask, Cache* cache) const {
        const int batch = input.shape(0), length = input.shape(1);
        auto qkv = att_proj(rms_norm(input, attn_norm));
        auto q = slice(qkv, {0, 0, 0}, {batch, length, 4096});
        auto k = slice(qkv, {0, 0, 4096}, {batch, length, 5120});
        auto v = slice(qkv, {0, 0, 5120}, {batch, length, 6144});
        q = rms_norm(reshape(q, {batch, length, 32, 128}), q_norm);
        k = rms_norm(reshape(k, {batch, length, 8, 128}), k_norm);
        q = transpose(q, {0, 2, 1, 3});
        k = transpose(k, {0, 2, 1, 3});
        v = transpose(reshape(v, {batch, length, 8, 128}), {0, 2, 1, 3});
        const int offset = cache ? cache->offset : 0;
        q = fast::rope(q, 128, false, 5000000.0f, 1.0f, offset);
        k = fast::rope(k, 128, false, 5000000.0f, 1.0f, offset);
        if (cache) cache->update(&k, &v);
        const std::string mask_mode = !mask && length > 1 ? "causal" : "";
        auto attention = fast::scaled_dot_product_attention(
            q, k, v, 1.0f / std::sqrt(128.0f), mask_mode, mask);
        auto hidden = input + attn_out(reshape(transpose(attention, {0, 2, 1, 3}),
                                                 {batch, length, 4096}));
        auto projected = ff_proj(rms_norm(hidden, ff_norm));
        auto value = slice(projected, {0, 0, 0}, {batch, length, 9728});
        auto gate = slice(projected, {0, 0, 9728}, {batch, length, 19456});
        return hidden + ff_out(silu(gate) * value);
    }
};

struct VisionAttention {
    QLinear wq, wk, wv, wo;
    void load(const Weights& weights, const std::string& prefix) {
        wq.load(weights, prefix + ".wq"); wk.load(weights, prefix + ".wk");
        wv.load(weights, prefix + ".wv"); wo.load(weights, prefix + ".wo");
    }
    array forward(const array& queries, const array& key_values,
                  const std::optional<array>& mask = std::nullopt) const {
        const int batch = queries.shape(0), qlen = queries.shape(1), klen = key_values.shape(1);
        auto q = transpose(reshape(wq(queries), {batch, qlen, 16, 72}), {0, 2, 1, 3});
        auto k = transpose(reshape(wk(key_values), {batch, klen, 16, 72}), {0, 2, 1, 3});
        auto v = transpose(reshape(wv(key_values), {batch, klen, 16, 72}), {0, 2, 1, 3});
        // The pinned adapter and ViT require float32 attention accumulation.
        auto attended = fast::scaled_dot_product_attention(
            astype(q, float32), astype(k, float32), astype(v, float32),
            1.0f / std::sqrt(72.0f), "", mask);
        return wo(astype(reshape(transpose(attended, {0, 2, 1, 3}),
                                 {batch, qlen, 1152}), queries.dtype()));
    }
};

struct VisionLayer {
    VisionAttention attention;
    QLinear w1, w2;
    array attention_norm_w {0.0f}, attention_norm_b {0.0f};
    array ffn_norm_w {0.0f}, ffn_norm_b {0.0f};
    void load(const Weights& weights, int layer) {
        const std::string p = "vision_tower.image_vit.transformer." + std::to_string(layer) + ".";
        attention.load(weights, p + "attention");
        w1.load(weights, p + "feed_forward.w1"); w2.load(weights, p + "feed_forward.w2");
        attention_norm_w = required(weights, p + "attention_norm.weight");
        attention_norm_b = required(weights, p + "attention_norm.bias");
        ffn_norm_w = required(weights, p + "ffn_norm.weight");
        ffn_norm_b = required(weights, p + "ffn_norm.bias");
    }
    array forward(const array& input) const {
        auto hidden = input + attention.forward(
            layer_norm(input, attention_norm_w, attention_norm_b),
            layer_norm(input, attention_norm_w, attention_norm_b));
        return hidden + w2(gelu_tanh(w1(layer_norm(hidden, ffn_norm_w, ffn_norm_b))));
    }
};

struct VisionBackbone {
    QLinear patch_embedding;
    array position {0.0f};
    std::vector<VisionLayer> layers;
    VisionAttention pooling;
    QLinear projector_w1, projector_w2, projector_w3;

    void load(const Weights& weights) {
        patch_embedding.load(weights, "vision_tower.image_vit.patch_embedding");
        position = required(weights, "vision_tower.image_vit.positional_embedding");
        layers.resize(25);
        for (int i = 0; i < 25; ++i) layers[i].load(weights, i);
        pooling.load(weights, "vision_tower.image_pooling_2d");
        projector_w1.load(weights, "vision_tower.image_projector.w1");
        projector_w2.load(weights, "vision_tower.image_projector.w2");
        projector_w3.load(weights, "vision_tower.image_projector.w3");
    }

    array forward(const VisualInput& input) const {
        if (input.crop_count <= 0 || input.pooling_width <= 0 ||
            input.patches.size() != static_cast<std::size_t>(input.crop_count) * 729 * 588 ||
            input.pooling.size() != static_cast<std::size_t>(input.patch_token_count * input.pooling_width))
            throw std::runtime_error("visual input violates the frozen Molmo2 shape contract");
        auto pixels = array(input.patches.data(), {input.crop_count, 729, 588}, float32);
        auto hidden = astype(patch_embedding(pixels), bfloat16) + position;
        array selected18(0.0f), selected24(0.0f);
        for (int layer = 0; layer < 25; ++layer) {
            hidden = layers[layer].forward(hidden);
            if (layer == 18) selected18 = hidden;
            if (layer == 24) selected24 = hidden;
        }
        // adapter vit_layers=[-3,-9] -> [24,18], in that order.
        auto features = concatenate({selected24, selected18}, -1);
        features = reshape(features, {input.crop_count * 729, 2304});
        std::vector<int> safe_indices(input.pooling.size());
        std::vector<float> valid(input.pooling.size());
        for (std::size_t i = 0; i < input.pooling.size(); ++i) {
            safe_indices[i] = std::max<std::int32_t>(0, input.pooling[i]);
            valid[i] = input.pooling[i] >= 0 ? 1.0f : 0.0f;
        }
        auto indices = array(safe_indices.data(),
                             {input.patch_token_count, input.pooling_width}, int32);
        auto mask = array(valid.data(), {input.patch_token_count, input.pooling_width, 1}, float32);
        auto to_pool = take(features, indices, 0) * astype(mask, features.dtype());
        auto denominator = maximum(sum(mask, 1, true), array(1.0f));
        auto query = sum(astype(to_pool, float32), 1, true) / denominator;
        query = astype(query, to_pool.dtype());
        auto additive_mask = reshape((1.0f - mask) * -1.0e9f,
                                     {input.patch_token_count, 1, 1, input.pooling_width});
        auto pooled = pooling.forward(query, to_pool, additive_mask);
        auto projected = projector_w2(silu(projector_w1(pooled)) * projector_w3(pooled));
        return reshape(projected, {input.patch_token_count, 2560});
    }
};

bool stop_token(int token) { return token == 151645 || token == 151643; }

bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string q4_prefix(const std::string& base) {
    constexpr const char* embedding = "language_model.model.wte.embedding";
    if (base == embedding) return base;
    constexpr const char* weight = ".weight";
    return ends_with(base, weight) ? base.substr(0, base.size() - strlen(weight)) : std::string();
}

void validate_runtime_weights(const Weights& weights) {
    std::size_t base_count = 0, q4_count = 0;
    for (const auto& [name, value] : weights) {
        std::vector<int> expected;
        if (!expected_tensor_shape(name, &expected)) continue;
        ++base_count;
        if (package_tensor_is_q4(name)) {
            ++q4_count;
            const std::string prefix = q4_prefix(name);
            const auto scale = weights.find(prefix + ".scales");
            const auto bias = weights.find(prefix + ".biases");
            const std::vector<int> packed_shape = {expected[0], expected[1] / 8};
            const std::vector<int> sidecar_shape = {expected[0], expected[1] / 64};
            if (prefix.empty() || value.dtype() != uint32 ||
                std::vector<int>(value.shape().begin(), value.shape().end()) != packed_shape ||
                scale == weights.end() || bias == weights.end() ||
                scale->second.dtype() != bfloat16 || bias->second.dtype() != bfloat16 ||
                std::vector<int>(scale->second.shape().begin(), scale->second.shape().end()) != sidecar_shape ||
                std::vector<int>(bias->second.shape().begin(), bias->second.shape().end()) != sidecar_shape)
                throw std::runtime_error("invalid Q4 tensor layout: " + name);
        } else if (value.dtype() != bfloat16 ||
                   std::vector<int>(value.shape().begin(), value.shape().end()) != expected) {
            throw std::runtime_error("invalid BF16 tensor layout: " + name);
        }
    }
    if (base_count != expected_tensor_count() || weights.size() != base_count + q4_count * 2)
        throw std::runtime_error("checkpoint tensors do not exactly match the Molmo2 package contract");
    for (const auto& [name, _] : weights) {
        if (expected_tensor_shape(name, nullptr)) continue;
        std::string base;
        if (ends_with(name, ".scales"))
            base = name.substr(0, name.size() - strlen(".scales"));
        else if (ends_with(name, ".biases"))
            base = name.substr(0, name.size() - strlen(".biases"));
        if (base != "language_model.model.wte.embedding") base += ".weight";
        if (!package_tensor_is_q4(base))
            throw std::runtime_error("unknown checkpoint tensor: " + name);
    }
}

}  // namespace

struct Model::Impl {
    Weights weights;
    array embedding {0.0f}, new_embedding {0.0f};
    std::optional<array> embedding_scales, embedding_biases;
    std::vector<TextLayer> text_layers;
    array final_norm {0.0f};
    QLinear lm_head;
    VisionBackbone vision;
    Tok tokenizer {};
    bool tokenizer_loaded = false;
    bool is_loaded = false;
    std::string model_dir;

    ~Impl() { if (tokenizer_loaded) tok_free(&tokenizer); }

    array embed(const array& ids) const {
        // Additional multimodal tokens occupy a separate 128-row table.
        auto base_ids = minimum(ids, array(151935, int32));
        auto extra_ids = maximum(ids - 151936, array(0, int32));
        array base(0.0f);
        if (embedding_scales) {
            base = dequantize(take(embedding, base_ids, 0),
                              take(*embedding_scales, base_ids, 0),
                              take(*embedding_biases, base_ids, 0), 64, 4,
                              "affine", std::nullopt, bfloat16);
        } else base = take(embedding, base_ids, 0);
        auto extra = take(new_embedding, extra_ids, 0);
        auto use_extra = expand_dims(greater_equal(ids, array(151936, int32)), -1);
        return where(use_extra, extra, base);
    }

    array decode(array hidden, const std::optional<array>& mask,
                 std::vector<Cache>* caches) const {
        for (int layer = 0; layer < 36; ++layer)
            hidden = text_layers[layer].forward(hidden, mask, &(*caches)[layer]);
        return lm_head(rms_norm(hidden, final_norm));
    }

    array encode_visual(const VisualInput& input) const {
        if (input.segment_crop_counts.size() <= 1)
            return vision.forward(input);
        if (input.segment_crop_counts.size() !=
            input.segment_patch_token_counts.size())
            throw std::runtime_error("Molmo2 visual segment metadata mismatch");
        constexpr std::size_t values_per_crop = 729 * 588;
        std::vector<array> encoded;
        encoded.reserve(input.segment_crop_counts.size());
        int crop_offset = 0, token_offset = 0;
        for (std::size_t segment = 0;
             segment < input.segment_crop_counts.size(); ++segment) {
            const int crops = input.segment_crop_counts[segment];
            const int patch_tokens = input.segment_patch_token_counts[segment];
            if (crops <= 0 || patch_tokens <= 0 ||
                crop_offset + crops > input.crop_count ||
                token_offset + patch_tokens > input.patch_token_count)
                throw std::runtime_error("Molmo2 visual segment exceeds input bounds");
            VisualInput current;
            current.crop_count = crops;
            current.patch_token_count = patch_tokens;
            current.pooling_width = input.pooling_width;
            const std::size_t patch_begin =
                static_cast<std::size_t>(crop_offset) * values_per_crop;
            const std::size_t patch_end = patch_begin +
                static_cast<std::size_t>(crops) * values_per_crop;
            current.patches.assign(input.patches.begin() + patch_begin,
                                   input.patches.begin() + patch_end);
            const std::size_t pool_begin =
                static_cast<std::size_t>(token_offset) * input.pooling_width;
            const std::size_t pool_end = pool_begin +
                static_cast<std::size_t>(patch_tokens) * input.pooling_width;
            current.pooling.reserve(pool_end - pool_begin);
            const std::int32_t index_offset = crop_offset * 729;
            for (std::size_t index = pool_begin; index < pool_end; ++index)
                current.pooling.push_back(input.pooling[index] < 0
                    ? -1 : input.pooling[index] - index_offset);
            auto features = vision.forward(current);
            eval(features);
            encoded.push_back(std::move(features));
            mlx::core::clear_cache();
            crop_offset += crops;
            token_offset += patch_tokens;
        }
        if (crop_offset != input.crop_count ||
            token_offset != input.patch_token_count)
            throw std::runtime_error("Molmo2 visual segments do not cover the input");
        return concatenate(encoded, 0);
    }
};

Model::Model() : impl_(std::make_unique<Impl>()) {}
Model::~Model() = default;

bool Model::load(const std::string& package_dir, std::string* error) {
    unload();
    try {
        PackageManifest manifest;
        std::string validation;
        if (!validate_package(package_dir, true, &manifest, &validation))
            throw std::runtime_error("Molmo2 package rejected: " + validation);
        // Limit MLX itself as a second line of defense. The supervisor performs
        // the pre-launch host-pressure gate before reaching this point.
        mlx::core::set_memory_limit(kMaxEstimatedResidentBytes);
        mlx::core::set_cache_limit(96ULL * 1024 * 1024);
        for (const auto& file : manifest.files) {
            if (file.role != "weights") continue;
            auto shard = load_safetensors(package_dir + "/" + file.name).first;
            for (auto& [name, value] : shard) {
                if (!impl_->weights.emplace(name, std::move(value)).second)
                    throw std::runtime_error("duplicate checkpoint tensor: " + name);
            }
        }
        if (impl_->weights.empty()) throw std::runtime_error("package contains no tensors");
        validate_runtime_weights(impl_->weights);
        require_shape(impl_->weights, "language_model.model.wte.new_embedding", {128, 2560});
        require_shape(impl_->weights, "language_model.model.ln_f.weight", {2560});
        require_shape(impl_->weights, "vision_tower.image_vit.positional_embedding", {729, 1152});
        impl_->embedding = required(impl_->weights, "language_model.model.wte.embedding");
        auto scale = impl_->weights.find("language_model.model.wte.scales");
        // The packer names sidecars after the full tensor prefix.
        if (scale == impl_->weights.end())
            scale = impl_->weights.find("language_model.model.wte.embedding.scales");
        if (scale != impl_->weights.end()) {
            impl_->embedding_scales = scale->second;
            const std::string bias_name = impl_->weights.count("language_model.model.wte.embedding.biases")
                                            ? "language_model.model.wte.embedding.biases"
                                            : "language_model.model.wte.biases";
            impl_->embedding_biases = required(impl_->weights, bias_name);
        }
        impl_->new_embedding = required(impl_->weights, "language_model.model.wte.new_embedding");
        impl_->text_layers.resize(36);
        for (int i = 0; i < 36; ++i) impl_->text_layers[i].load(impl_->weights, i);
        impl_->final_norm = required(impl_->weights, "language_model.model.ln_f.weight");
        impl_->lm_head.load(impl_->weights, "language_model.lm_head");
        impl_->vision.load(impl_->weights);
        tok_load(&impl_->tokenizer, (package_dir + "/tokenizer.json").c_str());
        impl_->tokenizer_loaded = true;
        impl_->model_dir = package_dir;
        impl_->is_loaded = true;
        if (error) error->clear();
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        unload();
        return false;
    }
}

bool Model::loaded() const { return impl_->is_loaded; }

GenerateResult Model::generate(const std::string& question, const VisualInput& visual,
                               const GenerateOptions& options,
                               const std::atomic_bool* cancelled,
                               const std::function<void(const std::string&)>& on_delta) {
    GenerateResult result;
    if (!loaded()) { result.error = "Molmo2 is not loaded"; return result; }
    if (question.empty() || question.size() > 32768 || options.max_new_tokens < 1 ||
        options.max_new_tokens > 1024) { result.error = "generation request exceeds safe bounds"; return result; }
    try {
        const std::string prompt = visual.token_text +
            "<|im_start|>user\n" + question + "<|im_end|>\n<|im_start|>assistant\n";
        std::vector<int> tokens(prompt.size() + 16);
        const int count = tok_encode(&impl_->tokenizer, prompt.data(),
                                     static_cast<int>(prompt.size()), tokens.data(), tokens.size());
        if (count <= 0 || static_cast<std::size_t>(count) == tokens.size())
            throw std::runtime_error("Molmo2 tokenizer failed or exhausted its exact output buffer");
        tokens.resize(count);
        /* Molmo2Processor.insert_bos() adds the tokenizer BOS after expanding
           the image/video placeholder. For this pinned Qwen tokenizer BOS is
           <|im_end|> (151645); it is not part of the visible chat template. */
        tokens.insert(tokens.begin(), 151645);
        if (tokens.empty() || tokens.size() + options.max_new_tokens > kSafeSequenceTokens)
            throw std::runtime_error(
                "Molmo2 request exceeds Samosa's 2,048-token memory-safe sequence envelope");
        int image_patch_tokens = 0;
        for (int token : tokens) if (token == 151938) ++image_patch_tokens;
        if (image_patch_tokens != visual.patch_token_count)
            throw std::runtime_error("tokenizer and visual feature counts disagree");
        result.prompt_tokens = static_cast<int>(tokens.size());

        auto ids = array(tokens.data(), {1, static_cast<int>(tokens.size())}, int32);
        auto hidden = impl_->embed(ids);
        auto visual_features = impl_->encode_visual(visual);
        std::vector<int> patch_positions;
        for (std::size_t i = 0; i < tokens.size(); ++i)
            if (tokens[i] == 151938) patch_positions.push_back(static_cast<int>(i));
        auto position_array = array(patch_positions.data(), {static_cast<int>(patch_positions.size())}, int32);
        /* scatter() is a slice-scatter primitive: its updates rank must equal
           input_rank + index_rank.  Visual token injection is instead an
           elementwise axis update.  Broadcast one sequence index across the
           hidden width and add the projected visual features in place. */
        auto position_indices = reshape(
            position_array, {1, static_cast<int>(patch_positions.size()), 1});
        hidden = scatter_add_axis(hidden, position_indices,
                                  expand_dims(visual_features, 0), 1);

        std::vector<std::uint8_t> token_types(tokens.size());
        for (std::size_t i = 0; i < tokens.size(); ++i)
            token_types[i] = tokens[i] >= 151936 && tokens[i] <= 151944;
        auto mask_values = build_prefill_attention_mask(token_types);
        auto mask = astype(array(mask_values.data(),
                                 {1, 1, static_cast<int>(tokens.size()), static_cast<int>(tokens.size())},
                                 float32), bfloat16);
        std::vector<Cache> caches(36);
        auto logits = impl_->decode(hidden, mask, &caches);
        int next = argmax(slice(logits, {0, static_cast<int>(tokens.size()) - 1, 0},
                                {1, static_cast<int>(tokens.size()), 151936}), -1).item<int>();
        std::string emitted;
        for (int step = 0; step < options.max_new_tokens && !stop_token(next); ++step) {
            if (cancelled && cancelled->load(std::memory_order_relaxed)) {
                result.cancelled = true; break;
            }
            tokens.push_back(next);
            ++result.generated_tokens;
            char piece[4096];
            const int bytes = tok_decode(&impl_->tokenizer, &next, 1, piece, sizeof(piece) - 1);
            if (bytes > 0) {
                emitted.append(piece, bytes);
                if (on_delta) on_delta(std::string(piece, bytes));
            }
            auto one = array(&next, {1, 1}, int32);
            logits = impl_->decode(impl_->embed(one), std::nullopt, &caches);
            next = argmax(reshape(logits, {151936}), -1).item<int>();
            // Bound graph retention and return inactive buffers to the OS/Metal heap.
            eval(logits);
            if ((step & 15) == 15) mlx::core::clear_cache();
        }
        result.text = std::move(emitted);
        result.ok = !result.cancelled;
        mlx::core::clear_cache();
        return result;
    } catch (const std::exception& exception) {
        result.error = exception.what();
        mlx::core::clear_cache();
        return result;
    }
}

void Model::unload() {
    if (impl_->tokenizer_loaded) { tok_free(&impl_->tokenizer); impl_->tokenizer_loaded = false; }
    impl_->weights.clear();
    impl_->text_layers.clear();
    impl_->embedding_scales.reset(); impl_->embedding_biases.reset();
    impl_->is_loaded = false; impl_->model_dir.clear();
    mlx::core::clear_cache();
}

}  // namespace samosa::molmo2
