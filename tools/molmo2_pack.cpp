#include "molmo2/molmo2_contract.h"

#include "mlx/io.h"
#include "mlx/memory.h"
#include "mlx/ops.h"
#include "mlx/transforms.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using mlx::core::array;

namespace {

constexpr std::uint64_t kShardTarget = 192ULL * 1024 * 1024;
constexpr std::uint64_t kPackerMemoryLimit = 2300ULL * 1024 * 1024;
constexpr std::uint64_t kRequiredOutputFreeBytes = 6ULL * 1024 * 1024 * 1024;

struct PinnedSourceFile {
    const char* name;
    std::uint64_t bytes;
    const char* sha256;
};

constexpr PinnedSourceFile kPinnedShards[] = {
    {"model-00001-of-00004.safetensors", 4891799000ULL,
     "065ed844edb74cc3f3992415dc412cfae4c2f6e324de76dc84fffe3a652f6fd3"},
    {"model-00002-of-00004.safetensors", 4844690992ULL,
     "ac1506897468e69d5af2f46127d30274eecb71c60745e7d27ea0319a27addbb4"},
    {"model-00003-of-00004.safetensors", 4844691024ULL,
     "da71a15c961f92361b64c3b45f5465d370c9992f032f0f4adf544d0b208ed573"},
    {"model-00004-of-00004.safetensors", 4822393416ULL,
     "519e694862a7d6d82e539424733f6f55fd8118c3c37e328c2dd942a6d4a16120"},
};
struct PublishedFile {
    std::string name;
    std::string role;
    std::uint64_t bytes = 0;
    std::string sha256;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

bool safe_leaf(const std::string& name) {
    if (name.empty() || name.size() > 180 || name.find("..") != std::string::npos ||
        name.find('/') != std::string::npos) return false;
    for (unsigned char c : name) {
        if (!std::isalnum(c) && c != '.' && c != '_' && c != '-') return false;
    }
    return true;
}

std::string json_escape(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result.push_back('\\'); result.push_back(c); }
        else if (c < 0x20) fail("control character in JSON value");
        else result.push_back(c);
    }
    return result;
}

void write_all(int fd, const char* bytes, std::size_t length) {
    while (length) {
        ssize_t wrote = ::write(fd, bytes, length);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) fail("write failed: " + std::string(std::strerror(errno)));
        bytes += wrote;
        length -= static_cast<std::size_t>(wrote);
    }
}

void durable_write(const fs::path& path, const std::string& contents) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) fail("cannot create " + path.string() + ": " + std::strerror(errno));
    try {
        write_all(fd, contents.data(), contents.size());
        if (::fsync(fd) != 0) fail("cannot fsync " + path.string());
    } catch (...) {
        ::close(fd);
        throw;
    }
    if (::close(fd) != 0) fail("cannot close " + path.string());
}

void fsync_file(const fs::path& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0 || ::fsync(fd) != 0) {
        if (fd >= 0) ::close(fd);
        fail("cannot durably publish " + path.string());
    }
    ::close(fd);
}

void fsync_dir(const fs::path& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0 || ::fsync(fd) != 0) {
        if (fd >= 0) ::close(fd);
        fail("cannot fsync directory " + path.string());
    }
    ::close(fd);
}

std::uint64_t regular_size(const fs::path& path) {
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0)
        fail("unsafe or missing regular file: " + path.string());
    return static_cast<std::uint64_t>(st.st_size);
}

void copy_regular(const fs::path& source, const fs::path& target,
                  std::uint64_t max_bytes) {
    const std::uint64_t expected = regular_size(source);
    if (!expected || expected > max_bytes) fail("metadata file has unsafe size: " + source.string());
    int in = ::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int out = ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (in < 0 || out < 0) {
        if (in >= 0) ::close(in);
        if (out >= 0) ::close(out);
        fail("cannot copy " + source.string());
    }
    std::uint64_t copied = 0;
    char buffer[65536];
    while (copied < expected) {
        ssize_t got = ::read(in, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { ::close(in); ::close(out); fail("short read from " + source.string()); }
        write_all(out, buffer, static_cast<std::size_t>(got));
        copied += static_cast<std::uint64_t>(got);
    }
    if (::fsync(out) != 0 || ::close(out) != 0 || ::close(in) != 0)
        fail("cannot durably copy " + source.string());
}

std::vector<fs::path> input_shards(const fs::path& source) {
    std::vector<fs::path> result;
    std::set<std::string> expected_names;
    for (const auto& expected : kPinnedShards) {
        expected_names.insert(expected.name);
        const fs::path path = source / expected.name;
        if (regular_size(path) != expected.bytes)
            fail("pinned source shard has the wrong byte count: " + path.string());
        const std::string digest = samosa::molmo2::sha256_file(path.string());
        if (digest != expected.sha256)
            fail("pinned source shard failed SHA-256 verification: " + path.string());
        result.push_back(path);
    }
    for (const auto& entry : fs::directory_iterator(source)) {
        if (entry.path().extension() == ".safetensors" &&
            !expected_names.count(entry.path().filename().string()))
            fail("unexpected safetensors shard in pinned source: " + entry.path().string());
    }
    return result;
}

bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string remove_suffix(const std::string& value, const std::string& suffix) {
    return value.substr(0, value.size() - suffix.size());
}

bool quantizable(const std::string& name, const array& value) {
    if (!samosa::molmo2::package_tensor_is_q4(name) || value.ndim() != 2)
        return false;
    // Keep the complete vision tower and connector in BF16. Q4 is limited to
    // the large text matrices and base token table, preserving visual quality.
    return value.size() >= 1024 * 1024;
}

class ShardWriter {
public:
    explicit ShardWriter(fs::path directory) : directory_(std::move(directory)) {}

    void add(const std::string& name, array value) {
        if (!names_.insert(name).second) fail("duplicate output tensor: " + name);
        const std::uint64_t bytes = value.nbytes();
        if (!pending_.empty() && pending_bytes_ + bytes > kShardTarget) flush();
        pending_bytes_ += bytes;
        pending_.emplace(name, std::move(value));
        if (pending_bytes_ >= kShardTarget) flush();
    }

    void flush() {
        if (pending_.empty()) return;
        ++shard_number_;
        char leaf[64];
        std::snprintf(leaf, sizeof(leaf), "weights-%05d.safetensors", shard_number_);
        const fs::path path = directory_ / leaf;
        std::vector<array> outputs;
        outputs.reserve(pending_.size());
        for (const auto& [_, value] : pending_) outputs.push_back(value);
        mlx::core::eval(std::move(outputs));
        mlx::core::save_safetensors(path.string(), pending_, {{"format", "mlx"}});
        fsync_file(path);
        for (const auto& [name, _] : pending_) tensor_to_file_[name] = leaf;
        pending_.clear();
        pending_bytes_ = 0;
        mlx::core::clear_cache();
    }

    void finish() { flush(); }
    const std::map<std::string, std::string>& index() const { return tensor_to_file_; }

private:
    fs::path directory_;
    int shard_number_ = 0;
    std::uint64_t pending_bytes_ = 0;
    std::unordered_map<std::string, array> pending_;
    std::set<std::string> names_;
    std::map<std::string, std::string> tensor_to_file_;
};

std::string index_json(const std::map<std::string, std::string>& index) {
    std::ostringstream out;
    out << "{\n  \"metadata\": {\"format\": \"samosa.molmo2.mlx.v1\"},\n  \"weight_map\": {\n";
    std::size_t position = 0;
    for (const auto& [tensor, file] : index) {
        out << "    \"" << json_escape(tensor) << "\": \"" << file << "\"";
        out << (++position == index.size() ? "\n" : ",\n");
    }
    out << "  }\n}\n";
    return out.str();
}

std::vector<PublishedFile> inventory(const fs::path& directory) {
    std::vector<PublishedFile> result;
    for (const auto& entry : fs::directory_iterator(directory)) {
        const std::string name = entry.path().filename().string();
        if (name == "manifest.json") continue;
        if (!safe_leaf(name) || entry.is_symlink() || !entry.is_regular_file())
            fail("unsafe output artifact: " + entry.path().string());
        PublishedFile item;
        item.name = name;
        item.role = entry.path().extension() == ".safetensors" ? "weights" :
                    name == "tokenizer.json" ? "tokenizer" : "metadata";
        item.bytes = regular_size(entry.path());
        item.sha256 = samosa::molmo2::sha256_file(entry.path().string());
        if (item.sha256.empty()) fail("cannot hash " + entry.path().string());
        result.push_back(std::move(item));
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return result;
}

std::string manifest_json(const std::vector<PublishedFile>& files, std::uint64_t resident) {
    std::ostringstream out;
    out << "{\n"
        << "  \"format\": \"" << samosa::molmo2::kPackageFormat << "\",\n"
        << "  \"package_id\": \"" << samosa::molmo2::kPackageId << "\",\n"
        << "  \"model_id\": \"" << samosa::molmo2::kUpstreamModel << "\",\n"
        << "  \"upstream_revision\": \"" << samosa::molmo2::kUpstreamRevision << "\",\n"
        << "  \"processor_fingerprint\": \"" << samosa::molmo2::kProcessorFingerprint << "\",\n"
        << "  \"quantization\": {\"mode\": \"affine\", \"bits\": 4, \"group_size\": 64},\n"
        << "  \"estimated_resident_bytes\": " << resident << ",\n"
        << "  \"files\": [\n";
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& file = files[i];
        out << "    {\"name\": \"" << file.name << "\", \"role\": \"" << file.role
            << "\", \"bytes\": " << file.bytes << ", \"sha256\": \"" << file.sha256 << "\"}"
            << (i + 1 == files.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    return out.str();
}

void transform(const fs::path& source, const fs::path& processor, const fs::path& output) {
    if (!source.is_absolute() || !processor.is_absolute() || !output.is_absolute())
        fail("source, processor contract, and output paths must be absolute");
    if (!fs::is_directory(source) || fs::is_symlink(source)) fail("source directory is unsafe");
    if (fs::exists(output)) fail("output already exists; refusing to overwrite it");
    std::error_code space_error;
    const auto disk = fs::space(output.parent_path(), space_error);
    if (space_error || disk.available < kRequiredOutputFreeBytes)
        fail("output volume needs at least 6 GiB free before native conversion");
    if (samosa::molmo2::sha256_file(processor.string()) != samosa::molmo2::kProcessorFingerprint)
        fail("processor contract does not match the pinned Molmo2 implementation");
    if (samosa::molmo2::sha256_file((source / "config.json").string()) !=
            samosa::molmo2::kPinnedConfigSha256 ||
        samosa::molmo2::sha256_file((source / "tokenizer.json").string()) !=
            samosa::molmo2::kPinnedTokenizerSha256)
        fail("config.json or tokenizer.json is not from the pinned Molmo2-4B revision");

    fs::path staging = output;
    staging += ".partial-" + std::to_string(static_cast<long long>(::getpid()));
    if (fs::exists(staging) || !fs::create_directory(staging)) fail("cannot create staging directory");
    try {
        mlx::core::set_memory_limit(kPackerMemoryLimit);
        mlx::core::set_cache_limit(64ULL * 1024 * 1024);
        ShardWriter writer(staging);
        const auto shards = input_shards(source);
        std::set<std::string> all_source;
        // Inventory every official tensor before writing output. This makes a
        // renamed, missing, extra, retyped, or reshaped checkpoint fail closed.
        for (const fs::path& shard_path : shards) {
            auto tensors = mlx::core::load_safetensors(shard_path.string()).first;
            for (const auto& [source_name, value] : tensors) {
                const std::string name = samosa::molmo2::canonical_source_tensor_name(source_name);
                std::vector<int> expected;
                if (name.empty() || !samosa::molmo2::expected_tensor_shape(name, &expected))
                    fail("unknown tensor in pinned source: " + source_name);
                const std::vector<int> actual(value.shape().begin(), value.shape().end());
                if (actual != expected || value.dtype() != mlx::core::float32)
                    fail("source tensor shape/dtype mismatch: " + source_name);
                if (!all_source.insert(name).second) fail("duplicate source tensor: " + source_name);
            }
        }
        if (all_source.size() != samosa::molmo2::expected_tensor_count())
            fail("source checkpoint is missing one or more pinned Molmo2-4B tensors");

        for (const fs::path& shard_path : shards) {
            auto tensors = mlx::core::load_safetensors(shard_path.string()).first;
            std::vector<std::string> names;
            names.reserve(tensors.size());
            for (const auto& [name, _] : tensors) names.push_back(name);
            std::sort(names.begin(), names.end());
            for (const std::string& source_name : names) {
                const std::string name =
                    samosa::molmo2::canonical_source_tensor_name(source_name);
                const array& value = tensors.at(source_name);
                const std::string prefix = ends_with(name, ".weight")
                    ? remove_suffix(name, ".weight") : name;
                if (quantizable(name, value)) {
                    auto quantized = mlx::core::quantize(
                        mlx::core::astype(value, mlx::core::bfloat16), 64, 4, "affine");
                    if (quantized.size() != 3) fail("MLX affine quantizer returned an invalid tensor set");
                    writer.add(name, std::move(quantized[0]));
                    writer.add(prefix + ".scales", std::move(quantized[1]));
                    writer.add(prefix + ".biases", std::move(quantized[2]));
                } else {
                    writer.add(name, mlx::core::astype(value, mlx::core::bfloat16));
                }
            }
            tensors.clear();
            mlx::core::clear_cache();
        }
        writer.finish();
        durable_write(staging / "model.safetensors.index.json", index_json(writer.index()));

        const std::vector<std::string> required = {"config.json", "tokenizer.json"};
        for (const std::string& name : required) copy_regular(source / name, staging / name, 256ULL << 20);
        copy_regular(processor, staging / "processor.json", 1ULL << 20);
        durable_write(staging / "PROVENANCE.md",
            "# Molmo2 4B package provenance\n\n"
            "Converted by Samosa's native molmo2-pack tool from "
            "allenai/Molmo2-4B revision "
            "042abfa7a38879a376cec03d949eff0aefaa0600.\n\n"
            "The upstream model is licensed under Apache-2.0. This package "
            "uses affine Q4/group-64 for large language matrices and BF16 "
            "for the vision tower, connector, and remaining tensors.\n\n"
            "Upstream: https://huggingface.co/allenai/Molmo2-4B\n");
        const std::vector<std::string> optional = {
            "tokenizer_config.json", "added_tokens.json", "special_tokens_map.json",
            "chat_template.jinja", "merges.txt", "vocab.json"};
        for (const std::string& name : optional) {
            if (fs::exists(source / name) && !fs::is_symlink(source / name))
                copy_regular(source / name, staging / name, 256ULL << 20);
        }

        auto files = inventory(staging);
        if (files.size() < 4 || files.size() > 63) fail("output package file count is outside contract");
        std::uint64_t total = 0, weights = 0;
        for (const auto& file : files) {
            if (total > UINT64_MAX - file.bytes) fail("package size overflow");
            total += file.bytes;
            if (file.role == "weights") weights += file.bytes;
        }
        const std::uint64_t expected_payload =
            samosa::molmo2::estimated_tensor_payload_bytes();
        if (weights < expected_payload || weights > expected_payload + 16ULL * 1024 * 1024)
            fail("weight shards do not match the exact Q4/BF16 payload budget");
        // Weight bytes plus a deliberately conservative 384 MiB activation/
        // KV allowance is the package's admission-control number.
        const std::uint64_t resident = weights + 384ULL * 1024 * 1024;
        if (total > samosa::molmo2::kMaxPackageBytes ||
            resident > samosa::molmo2::kMaxEstimatedResidentBytes)
            fail("fully quantized package exceeds Samosa's 16-GiB-machine safety budget");
        durable_write(staging / "manifest.json", manifest_json(files, resident));
        fsync_dir(staging);
        if (::rename(staging.c_str(), output.c_str()) != 0)
            fail("atomic package publish failed: " + std::string(std::strerror(errno)));
        fsync_dir(output.parent_path());

        samosa::molmo2::PackageManifest verified;
        std::string error;
        if (!samosa::molmo2::validate_package(output.string(), true, &verified, &error))
            fail("published package failed its own verifier: " + error);
        std::cout << "published " << output << " (" << verified.total_bytes
                  << " bytes on disk, " << verified.estimated_resident_bytes
                  << " admitted resident bytes)\n";
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
        throw;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: molmo2-pack ABSOLUTE_SOURCE_DIR ABSOLUTE_PROCESSOR_JSON ABSOLUTE_OUTPUT_DIR\n";
        return 2;
    }
    try {
        transform(fs::path(argv[1]), fs::path(argv[2]), fs::path(argv[3]));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "molmo2-pack: " << error.what() << "\n";
        return 1;
    }
}
