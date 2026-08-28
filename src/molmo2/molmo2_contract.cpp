#include "molmo2_contract.h"

#include "../json.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sys/stat.h>

namespace samosa::molmo2 {
namespace {

struct Sha256 {
    std::uint32_t h[8];
    std::uint64_t bits = 0;
    std::array<unsigned char, 64> block {};
    std::size_t used = 0;
};

std::uint32_t rotate_right(std::uint32_t value, int count) {
    return (value >> count) | (value << (32 - count));
}

void sha_compress(Sha256* context, const unsigned char bytes[64]) {
    static constexpr std::uint32_t constants[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
    std::uint32_t words[64];
    for (int i = 0; i < 16; ++i) {
        const unsigned char* p = bytes + i * 4;
        words[i] = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
                   (std::uint32_t(p[2]) << 8) | p[3];
    }
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = rotate_right(words[i - 15], 7) ^ rotate_right(words[i - 15], 18) ^ (words[i - 15] >> 3);
        std::uint32_t s1 = rotate_right(words[i - 2], 17) ^ rotate_right(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    std::uint32_t a=context->h[0], b=context->h[1], c=context->h[2], d=context->h[3];
    std::uint32_t e=context->h[4], f=context->h[5], g=context->h[6], h=context->h[7];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t s1=rotate_right(e,6)^rotate_right(e,11)^rotate_right(e,25);
        std::uint32_t choose=(e&f)^((~e)&g), t1=h+s1+choose+constants[i]+words[i];
        std::uint32_t s0=rotate_right(a,2)^rotate_right(a,13)^rotate_right(a,22);
        std::uint32_t majority=(a&b)^(a&c)^(b&c), t2=s0+majority;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    context->h[0]+=a; context->h[1]+=b; context->h[2]+=c; context->h[3]+=d;
    context->h[4]+=e; context->h[5]+=f; context->h[6]+=g; context->h[7]+=h;
}

void sha_init(Sha256* context) {
    static constexpr std::uint32_t initial[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    std::copy(std::begin(initial), std::end(initial), context->h);
    context->bits = 0; context->used = 0;
}

void sha_update(Sha256* context, const unsigned char* bytes, std::size_t length) {
    context->bits += static_cast<std::uint64_t>(length) * 8;
    while (length) {
        std::size_t take = std::min(length, context->block.size() - context->used);
        memcpy(context->block.data() + context->used, bytes, take);
        context->used += take; bytes += take; length -= take;
        if (context->used == context->block.size()) {
            sha_compress(context, context->block.data()); context->used = 0;
        }
    }
}

std::string sha_file_impl(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    Sha256 context; sha_init(&context);
    std::array<unsigned char, 65536> buffer {};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        std::streamsize got = input.gcount();
        if (got > 0) sha_update(&context, buffer.data(), static_cast<std::size_t>(got));
    }
    if (!input.eof()) return {};
    const std::uint64_t bits = context.bits;
    context.block[context.used++] = 0x80;
    if (context.used > 56) {
        while (context.used < 64) context.block[context.used++] = 0;
        sha_compress(&context, context.block.data()); context.used = 0;
    }
    while (context.used < 56) context.block[context.used++] = 0;
    for (int i = 7; i >= 0; --i) context.block[context.used++] = static_cast<unsigned char>(bits >> (i * 8));
    sha_compress(&context, context.block.data());
    static constexpr char hex[] = "0123456789abcdef";
    std::string result(64, '0');
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 4; ++j) {
            unsigned char byte = static_cast<unsigned char>(context.h[i] >> (24 - j * 8));
            result[(i * 4 + j) * 2] = hex[byte >> 4];
            result[(i * 4 + j) * 2 + 1] = hex[byte & 15];
        }
    }
    return result;
}

bool safe_leaf_name(const char* name) {
    if (!name || !*name || strlen(name) > 180 || strstr(name, "..") || strchr(name, '/'))
        return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p)
        if (!std::isalnum(*p) && *p != '.' && *p != '_' && *p != '-') return false;
    return true;
}

bool regular_no_symlink(const std::string& path, std::uint64_t* bytes) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) return false;
    if (bytes) *bytes = static_cast<std::uint64_t>(st.st_size);
    return true;
}

bool get_string(jval* object, const char* key, std::string* out) {
    jval* value = object ? json_get(object, key) : nullptr;
    if (!value || value->t != J_STR || !value->str[0]) return false;
    *out = value->str;
    return true;
}

bool exact_uint(jval* object, const char* key, std::uint64_t* out) {
    jval* value = object ? json_get(object, key) : nullptr;
    if (!value || value->t != J_NUM || !std::isfinite(value->num) || value->num < 0 ||
        value->num > static_cast<double>(std::numeric_limits<std::uint64_t>::max()) ||
        std::floor(value->num) != value->num) return false;
    *out = static_cast<std::uint64_t>(value->num);
    return true;
}

bool sha256_string(const std::string& value) {
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

}  // namespace

std::string sha256_file(const std::string& path) {
    return sha_file_impl(path);
}

std::string canonical_source_tensor_name(const std::string& source_name) {
    if (source_name == "lm_head.weight") return "language_model.lm_head.weight";
    constexpr const char* text_source = "model.transformer.";
    constexpr const char* text_target = "language_model.model.";
    if (source_name.rfind(text_source, 0) == 0)
        return text_target + source_name.substr(strlen(text_source));

    constexpr const char* vision_source = "model.vision_backbone.";
    if (source_name.rfind(vision_source, 0) == 0) {
        std::string result = "vision_tower." + source_name.substr(strlen(vision_source));
        const std::string saved_blocks = "image_vit.transformer.resblocks.";
        const std::string runtime_blocks = "image_vit.transformer.";
        const std::size_t at = result.find(saved_blocks);
        if (at != std::string::npos) result.replace(at, saved_blocks.size(), runtime_blocks);
        return result;
    }
    return {};
}

namespace {
const std::map<std::string, std::vector<int>>& pinned_tensor_shapes() {
    static const std::map<std::string, std::vector<int>> shapes = [] {
        std::map<std::string, std::vector<int>> result;
        auto add = [&](const std::string& name, std::initializer_list<int> shape) {
            result.emplace(name, std::vector<int>(shape));
        };
        add("language_model.model.wte.embedding", {151936, 2560});
        add("language_model.model.wte.new_embedding", {128, 2560});
        add("language_model.model.ln_f.weight", {2560});
        add("language_model.lm_head.weight", {151936, 2560});
        for (int layer = 0; layer < 36; ++layer) {
            const std::string prefix =
                "language_model.model.blocks." + std::to_string(layer) + ".";
            add(prefix + "attn_norm.weight", {2560});
            add(prefix + "ff_norm.weight", {2560});
            add(prefix + "self_attn.att_proj.weight", {6144, 2560});
            add(prefix + "self_attn.attn_out.weight", {2560, 4096});
            add(prefix + "self_attn.q_norm.weight", {128});
            add(prefix + "self_attn.k_norm.weight", {128});
            add(prefix + "mlp.ff_proj.weight", {19456, 2560});
            add(prefix + "mlp.ff_out.weight", {2560, 9728});
        }

        add("vision_tower.image_vit.patch_embedding.weight", {1152, 588});
        add("vision_tower.image_vit.patch_embedding.bias", {1152});
        add("vision_tower.image_vit.positional_embedding", {729, 1152});
        for (int layer = 0; layer < 25; ++layer) {
            const std::string prefix =
                "vision_tower.image_vit.transformer." + std::to_string(layer) + ".";
            for (const char* projection : {"wq", "wk", "wv", "wo"}) {
                add(prefix + "attention." + projection + ".weight", {1152, 1152});
                add(prefix + "attention." + projection + ".bias", {1152});
            }
            add(prefix + "attention_norm.weight", {1152});
            add(prefix + "attention_norm.bias", {1152});
            add(prefix + "feed_forward.w1.weight", {4304, 1152});
            add(prefix + "feed_forward.w1.bias", {4304});
            add(prefix + "feed_forward.w2.weight", {1152, 4304});
            add(prefix + "feed_forward.w2.bias", {1152});
            add(prefix + "ffn_norm.weight", {1152});
            add(prefix + "ffn_norm.bias", {1152});
        }
        for (const char* projection : {"wq", "wk", "wv"}) {
            add(std::string("vision_tower.image_pooling_2d.") + projection + ".weight",
                {1152, 2304});
            add(std::string("vision_tower.image_pooling_2d.") + projection + ".bias", {1152});
        }
        add("vision_tower.image_pooling_2d.wo.weight", {1152, 1152});
        add("vision_tower.image_pooling_2d.wo.bias", {1152});
        add("vision_tower.image_projector.w1.weight", {9728, 1152});
        add("vision_tower.image_projector.w2.weight", {2560, 9728});
        add("vision_tower.image_projector.w3.weight", {9728, 1152});
        return result;
    }();
    return shapes;
}
}  // namespace

bool expected_tensor_shape(const std::string& canonical_name,
                           std::vector<int>* shape) {
    const auto& shapes = pinned_tensor_shapes();
    const auto found = shapes.find(canonical_name);
    if (found == shapes.end()) return false;
    if (shape) *shape = found->second;
    return true;
}

std::size_t expected_tensor_count() { return pinned_tensor_shapes().size(); }

bool package_tensor_is_q4(const std::string& canonical_name) {
    std::vector<int> shape;
    if (canonical_name.rfind("language_model.", 0) != 0 ||
        !expected_tensor_shape(canonical_name, &shape) || shape.size() != 2)
        return false;
    if (shape[1] < 64 || shape[1] % 64 != 0) return false;
    const std::uint64_t elements =
        static_cast<std::uint64_t>(shape[0]) * static_cast<std::uint64_t>(shape[1]);
    return elements >= 1024ULL * 1024;
}

std::uint64_t estimated_tensor_payload_bytes() {
    std::uint64_t total = 0;
    for (const auto& [name, shape] : pinned_tensor_shapes()) {
        std::uint64_t elements = 1;
        for (int dimension : shape) elements *= static_cast<std::uint64_t>(dimension);
        if (package_tensor_is_q4(name)) {
            /* packed Q4 values plus BF16 scale and bias for every group64 */
            total += elements / 2 + elements / 64 * 4;
        } else {
            total += elements * 2;
        }
    }
    return total;
}

bool validate_package(const std::string& model_dir, bool verify_hashes,
                      PackageManifest* out, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (model_dir.empty() || model_dir[0] != '/') return fail("model directory must be absolute");
    struct stat root_st {};
    if (lstat(model_dir.c_str(), &root_st) != 0 || !S_ISDIR(root_st.st_mode))
        return fail("model directory is missing or is a symlink");

    const std::string manifest_path = model_dir + "/manifest.json";
    std::uint64_t manifest_bytes = 0;
    if (!regular_no_symlink(manifest_path, &manifest_bytes) || manifest_bytes > (1u << 20))
        return fail("manifest.json is missing, unsafe, or too large");
    std::ifstream input(manifest_path, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (raw.size() != manifest_bytes) return fail("manifest.json could not be read completely");

    char* arena = nullptr;
    jval* root = json_parse(raw.c_str(), &arena);
    if (!root || root->t != J_OBJ) {
        json_free(root); free(arena);
        return fail("manifest.json is malformed");
    }
    PackageManifest result;
    std::string format, model_id, quant_mode;
    bool ok = get_string(root, "format", &format) && format == kPackageFormat &&
              get_string(root, "package_id", &result.package_id) && result.package_id == kPackageId &&
              get_string(root, "model_id", &model_id) && model_id == kUpstreamModel &&
              get_string(root, "upstream_revision", &result.upstream_revision) &&
              result.upstream_revision == kUpstreamRevision &&
              get_string(root, "processor_fingerprint", &result.processor_fingerprint) &&
              result.processor_fingerprint == kProcessorFingerprint;
    jval* quant = json_get(root, "quantization");
    std::uint64_t bits = 0, group = 0;
    ok = ok && quant && quant->t == J_OBJ && get_string(quant, "mode", &quant_mode) &&
         quant_mode == "affine" && exact_uint(quant, "bits", &bits) && bits == 4 &&
         exact_uint(quant, "group_size", &group) && group == 64;
    result.bits = static_cast<int>(bits);
    result.group_size = static_cast<int>(group);
    ok = ok && exact_uint(root, "estimated_resident_bytes", &result.estimated_resident_bytes) &&
         result.estimated_resident_bytes > 0 &&
         result.estimated_resident_bytes <= kMaxEstimatedResidentBytes;
    jval* files = json_get(root, "files");
    ok = ok && files && files->t == J_ARR && files->len >= 4 && files->len <= 64;
    if (!ok) {
        json_free(root); free(arena);
        return fail("manifest contract, revision, processor, or Q4 budget is incompatible");
    }

    std::set<std::string> names;
    bool has_weights = false, has_tokenizer = false, has_config = false,
         has_processor = false;
    for (int i = 0; i < files->len && ok; ++i) {
        jval* item = files->kids[i];
        PackageFile file;
        ok = item && item->t == J_OBJ && get_string(item, "name", &file.name) &&
             safe_leaf_name(file.name.c_str()) && get_string(item, "role", &file.role) &&
             exact_uint(item, "bytes", &file.bytes) && file.bytes > 0 &&
             get_string(item, "sha256", &file.sha256) && sha256_string(file.sha256) &&
             names.insert(file.name).second;
        if (!ok) break;
        std::uint64_t actual = 0;
        const std::string path = model_dir + "/" + file.name;
        if (!regular_no_symlink(path, &actual) || actual != file.bytes) { ok = false; break; }
        if (verify_hashes) {
            const std::string digest = sha256_file(path);
            if (digest.empty() || file.sha256 != digest) {
                ok = false; break;
            }
        }
        const bool safetensors = file.name.size() > strlen(".safetensors") &&
            file.name.compare(file.name.size() - strlen(".safetensors"),
                              strlen(".safetensors"), ".safetensors") == 0;
        if ((file.role != "weights" && file.role != "tokenizer" &&
             file.role != "metadata") || safetensors != (file.role == "weights")) {
            ok = false; break;
        }
        if (file.name == "tokenizer.json") {
            if (file.role != "tokenizer" || file.sha256 != kPinnedTokenizerSha256) {
                ok = false; break;
            }
            has_tokenizer = true;
        } else if (file.role == "tokenizer") {
            ok = false; break;
        }
        if (file.name == "config.json") {
            if (file.role != "metadata" || file.sha256 != kPinnedConfigSha256) {
                ok = false; break;
            }
            has_config = true;
        }
        if (file.name == "processor.json") {
            if (file.role != "metadata" || file.sha256 != kProcessorFingerprint) {
                ok = false; break;
            }
            has_processor = true;
        }
        if (result.total_bytes > kMaxPackageBytes - file.bytes) { ok = false; break; }
        result.total_bytes += file.bytes;
        has_weights |= file.role == "weights";
        result.files.push_back(std::move(file));
    }
    json_free(root); free(arena);
    if (!ok || !has_weights || !has_tokenizer || !has_config || !has_processor ||
        result.total_bytes > kMaxPackageBytes)
        return fail("package files are incomplete, changed, or exceed the safe size ceiling");
    if (out) *out = std::move(result);
    if (error) error->clear();
    return true;
}

std::vector<float> build_prefill_attention_mask(
    const std::vector<std::uint8_t>& visual_token_types) {
    const std::size_t n = visual_token_types.size();
    std::vector<float> mask(n * n, -1.0e9f);
    for (std::size_t query = 0; query < n; ++query) {
        for (std::size_t key = 0; key < n; ++key) {
            if (key <= query || (visual_token_types[query] && visual_token_types[key]))
                mask[query * n + key] = 0.0f;
        }
    }
    return mask;
}

std::vector<double> uniform_last_frame_times(double start_seconds,
                                             double end_seconds,
                                             int max_frames,
                                             double max_fps) {
    std::vector<double> result;
    if (!std::isfinite(start_seconds) || !std::isfinite(end_seconds) ||
        !std::isfinite(max_fps) || start_seconds < 0 || end_seconds < start_seconds ||
        max_frames <= 0 || max_frames > 384 || max_fps <= 0 || max_fps > 2.0)
        return result;
    if (end_seconds == start_seconds || max_frames == 1) {
        result.push_back(start_seconds); return result;
    }
    const double duration = end_seconds - start_seconds;
    const double dense_count = std::floor(duration * max_fps) + 1.0;
    int count = std::min(max_frames, std::max(2, static_cast<int>(dense_count)));
    if ((max_frames - 1) / max_fps < duration) count = max_frames;
    result.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double ratio = count == 1 ? 0.0 : static_cast<double>(i) / (count - 1);
        result.push_back(start_seconds + duration * ratio);
    }
    result.back() = end_seconds;
    return result;
}

std::vector<FrameWindow> plan_frame_windows(double start_seconds,
                                            double end_seconds,
                                            int max_frames_per_window,
                                            double max_fps,
                                            double overlap_seconds) {
    std::vector<FrameWindow> windows;
    if (!std::isfinite(overlap_seconds) || overlap_seconds < 0 ||
        max_frames_per_window < 2 || max_frames_per_window > 64 ||
        max_fps <= 0 || max_fps > 2.0 || end_seconds < start_seconds)
        return windows;
    const double span = (max_frames_per_window - 1) / max_fps;
    if (span <= 0 || overlap_seconds >= span) return windows;
    double cursor = start_seconds;
    while (cursor < end_seconds || windows.empty()) {
        FrameWindow window;
        window.start_seconds = cursor;
        window.end_seconds = std::min(end_seconds, cursor + span);
        window.sample_times = uniform_last_frame_times(
            window.start_seconds, window.end_seconds, max_frames_per_window, max_fps);
        if (window.sample_times.empty()) return {};
        windows.push_back(std::move(window));
        if (windows.back().end_seconds >= end_seconds) break;
        cursor = windows.back().end_seconds - overlap_seconds;
        if (windows.size() > 100000) return {};
    }
    return windows;
}

}  // namespace samosa::molmo2
