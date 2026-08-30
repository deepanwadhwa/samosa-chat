#ifndef SAMOSA_MOLMO2_CONTRACT_H
#define SAMOSA_MOLMO2_CONTRACT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace samosa::molmo2 {

inline constexpr const char* kPackageFormat = "samosa.molmo2.mlx.v1";
inline constexpr const char* kPackageId = "molmo2-4b-mlx-q4-v1";
inline constexpr const char* kUpstreamModel = "allenai/Molmo2-4B";
inline constexpr const char* kUpstreamRevision =
    "042abfa7a38879a376cec03d949eff0aefaa0600";
/* SHA-256 of assets/molmo2/processor.json.  Kept in the native binary so a
   checkpoint produced against a different crop/timestamp contract fails
   before any multi-gigabyte tensor is mapped. */
inline constexpr const char* kProcessorFingerprint =
    "808de9add76144a557348c5f5180a8408b12ca83592c7a6a257ae69c968e51df";
inline constexpr const char* kPinnedConfigSha256 =
    "17e072e3c3b29d9be7a348c74b88658e67ccce094c31b21f87646f6cecd2a76f";
inline constexpr const char* kPinnedTokenizerSha256 =
    "95e80901c901584f416b8fd4349fd60022774b89ba4377626511f0562cc599f7";

inline constexpr std::uint64_t kMaxPackageBytes = 4ULL * 1024 * 1024 * 1024;
inline constexpr std::uint64_t kMaxEstimatedResidentBytes =
    3750ULL * 1024 * 1024;

struct Config {
    int vocab_size = 151936;
    int additional_vocab_size = 128;
    int hidden_size = 2560;
    int intermediate_size = 9728;
    int text_layers = 36;
    int attention_heads = 32;
    int kv_heads = 8;
    int head_dim = 128;
    float norm_eps = 1e-6f;
    float rope_theta = 5000000.0f;
    int max_positions = 36864;

    int vision_hidden = 1152;
    int vision_intermediate = 4304;
    int vision_declared_layers = 27;
    int vision_required_layers = 25;
    int vision_heads = 16;
    int vision_head_dim = 72;
    int image_size = 378;
    int patch_size = 14;
    int image_positions = 729;

    int adapter_intermediate = 9728;
    int adapter_output = 2560;
    int adapter_heads = 16;
    int adapter_head_dim = 72;
    int selected_vision_layers[2] = {24, 18};

    int image_start = 151936;
    int image_end = 151937;
    int image_patch = 151938;
    int image_col = 151939;
    int low_res_image_start = 151940;
    int image_low_res = 151942;
    int frame_start = 151943;
    int frame_end = 151944;
    int eos[2] = {151645, 151643};
};

struct PackageFile {
    std::string name;
    std::string role;
    std::uint64_t bytes = 0;
    std::string sha256;
};

struct PackageManifest {
    std::string package_id;
    std::string upstream_revision;
    std::string processor_fingerprint;
    int bits = 0;
    int group_size = 0;
    std::uint64_t estimated_resident_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::vector<PackageFile> files;
};

/* Streaming native SHA-256 used by both the installer/validator and the
   offline package builder.  Returning an empty string means the file could
   not be read completely. */
std::string sha256_file(const std::string& path);

/* The pinned Hugging Face checkpoint uses model.transformer.*,
   model.vision_backbone.*, and lm_head.*. Packages use stable Samosa names so
   the runtime is insulated from Python module aliases. An empty result means
   the source name is not part of the pinned 706-tensor checkpoint. */
std::string canonical_source_tensor_name(const std::string& source_name);

/* Exact canonical shape contract for every tensor in the pinned checkpoint.
   The package builder rejects unknown, missing, or reshaped inputs before
   quantizing anything. */
bool expected_tensor_shape(const std::string& canonical_name,
                           std::vector<int>* shape);
std::size_t expected_tensor_count();

/* True only for matrices stored as affine Q4 in the v1 package. Vision and
   connector tensors deliberately remain BF16. */
bool package_tensor_is_q4(const std::string& canonical_name);

/* Exact safetensor payload bytes implied by the pinned shapes and Q4/BF16
   policy (excluding small per-shard JSON headers). */
std::uint64_t estimated_tensor_payload_bytes();

/* `verify_hashes=false` is available for cheap catalog readiness checks. The
   native helper uses true before mapping any tensor, and then independently
   validates every tensor name, shape, and dtype. */
bool validate_package(const std::string& model_dir,
                      bool verify_hashes,
                      PackageManifest* out,
                      std::string* error);

/* Additive attention mask: zero when a key is visible and a large negative
   value when it is not.  Text remains causal; every visual query/key pair is
   bidirectional, matching Molmo2's token_type_ids mask. */
std::vector<float> build_prefill_attention_mask(
    const std::vector<std::uint8_t>& visual_token_types);

std::vector<double> uniform_last_frame_times(double start_seconds,
                                             double end_seconds,
                                             int max_frames,
                                             double max_fps);

struct FrameWindow {
    double start_seconds = 0;
    double end_seconds = 0;
    std::vector<double> sample_times;
};

/* Sequential, overlapping windows for exhaustive coverage.  No interval is
   silently dropped; the caller records how many completed before cancellation
   or pressure. */
std::vector<FrameWindow> plan_frame_windows(double start_seconds,
                                            double end_seconds,
                                            int max_frames_per_window,
                                            double max_fps,
                                            double overlap_seconds);

}  // namespace samosa::molmo2

#endif
