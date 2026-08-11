#include "json.h"
#include "maple/maple_expert_store.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using samosa::maple::MapleExpertStore;
using samosa::maple::MapleExpertStoreConfig;

namespace {

constexpr uint64_t kHeaderLimit = 128ull * 1024ull * 1024ull;
constexpr uint64_t kRecordAlignment = 16ull * 1024ull;
constexpr uint64_t kCopyBufferBytes = 8ull * 1024ull * 1024ull;

struct TensorInfo {
    std::string name;
    std::string shard_name;
    std::string dtype;
    std::vector<uint64_t> shape;
    uint64_t data_begin = 0;
    uint64_t data_end = 0;
    uint64_t output_begin = 0;
    uint64_t output_end = 0;
};

struct Shard {
    std::string name;
    std::string path;
    int fd = -1;
    uint64_t file_size = 0;
    uint64_t data_offset = 0;
    uint64_t data_bytes = 0;
    jval* header = nullptr;

    ~Shard() {
        if (fd >= 0) ::close(fd);
        if (header) json_free(header);
    }
};

static void fail_errno(const std::string& what, const std::string& path) {
    throw std::runtime_error(what + " " + path + ": " + std::strerror(errno));
}

static uint64_t checked_add(uint64_t a, uint64_t b, const char* label) {
    if (b > std::numeric_limits<uint64_t>::max() - a) {
        throw std::runtime_error(std::string("integer overflow while computing ") + label);
    }
    return a + b;
}

static uint64_t checked_mul(uint64_t a, uint64_t b, const char* label) {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) {
        throw std::runtime_error(std::string("integer overflow while computing ") + label);
    }
    return a * b;
}

static uint64_t align_up(uint64_t n, uint64_t alignment) {
    uint64_t r = n % alignment;
    return r == 0 ? n : checked_add(n, alignment - r, "aligned size");
}

static std::string read_text(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) fail_errno("could not open", path);
    if (std::fseek(f, 0, SEEK_END) != 0) {
        int saved = errno;
        std::fclose(f);
        errno = saved;
        fail_errno("could not seek", path);
    }
    long n = std::ftell(f);
    if (n < 0) {
        int saved = errno;
        std::fclose(f);
        errno = saved;
        fail_errno("could not size", path);
    }
    if (std::fseek(f, 0, SEEK_SET) != 0) {
        int saved = errno;
        std::fclose(f);
        errno = saved;
        fail_errno("could not rewind", path);
    }
    std::string out((size_t)n, '\0');
    if (!out.empty() && std::fread(&out[0], 1, out.size(), f) != out.size()) {
        int saved = ferror(f) ? errno : EIO;
        std::fclose(f);
        errno = saved;
        fail_errno("could not read", path);
    }
    std::fclose(f);
    return out;
}

static uint64_t read_u64_le(const uint8_t bytes[8]) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | bytes[i];
    return v;
}

static void put_u64_le(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back((uint8_t)(v & 0xffu));
        v >>= 8;
    }
}

static void pread_exact(int fd, void* dst, uint64_t bytes, uint64_t offset,
                        const std::string& label) {
    uint8_t* out = static_cast<uint8_t*>(dst);
    uint64_t done = 0;
    while (done < bytes) {
        uint64_t remaining = bytes - done;
        size_t chunk = remaining > (uint64_t)std::numeric_limits<size_t>::max()
                           ? std::numeric_limits<size_t>::max()
                           : (size_t)remaining;
        ssize_t n = ::pread(fd, out + done, chunk, (off_t)(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("pread failed for " + label + ": " + std::strerror(errno));
        }
        if (n == 0) throw std::runtime_error("short pread for " + label);
        done += (uint64_t)n;
    }
}

static void write_exact(int fd, const void* src, uint64_t bytes, const std::string& label) {
    const uint8_t* p = static_cast<const uint8_t*>(src);
    uint64_t done = 0;
    while (done < bytes) {
        uint64_t remaining = bytes - done;
        size_t chunk = remaining > (uint64_t)std::numeric_limits<size_t>::max()
                           ? std::numeric_limits<size_t>::max()
                           : (size_t)remaining;
        ssize_t n = ::write(fd, p + done, chunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("write failed for " + label + ": " + std::strerror(errno));
        }
        if (n == 0) throw std::runtime_error("short write for " + label);
        done += (uint64_t)n;
    }
}

static void write_zeros(int fd, uint64_t bytes) {
    std::vector<uint8_t> zeros(4096, 0);
    while (bytes > 0) {
        uint64_t n = bytes < zeros.size() ? bytes : zeros.size();
        write_exact(fd, zeros.data(), n, "padding");
        bytes -= n;
    }
}

static int open_readonly(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) fail_errno("could not open", path);
    return fd;
}

static int open_output(const std::string& path) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) fail_errno("could not create", path);
    return fd;
}

static bool is_expert_tensor(const std::string& name) {
    const std::string marker = ".mlp.switch_mlp.";
    if (name.find(marker) == std::string::npos) return false;
    return name.rfind(".gate_proj.weight") != std::string::npos ||
           name.rfind(".gate_proj.row_alpha") != std::string::npos ||
           name.rfind(".up_proj.weight") != std::string::npos ||
           name.rfind(".up_proj.row_alpha") != std::string::npos ||
           name.rfind(".down_proj.weight") != std::string::npos ||
           name.rfind(".down_proj.row_alpha") != std::string::npos;
}

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if ((unsigned char)c < 0x20) {
            throw std::runtime_error("control character in JSON string");
        } else {
            out.push_back(c);
        }
    }
    return out;
}

static std::string shape_json(const std::vector<uint64_t>& shape) {
    std::string out = "[";
    for (size_t i = 0; i < shape.size(); ++i) {
        if (i) out += ",";
        out += std::to_string(shape[i]);
    }
    out += "]";
    return out;
}

static TensorInfo parse_tensor(jval* header, const std::string& name,
                               const std::string& shard_name,
                               const std::string& shard_path,
                               uint64_t data_bytes) {
    jval* entry = json_get(header, name.c_str());
    if (!entry || entry->t != J_OBJ) {
        throw std::runtime_error("tensor `" + name + "` missing from " + shard_path);
    }
    jval* dtype = json_get(entry, "dtype");
    jval* shape = json_get(entry, "shape");
    jval* offsets = json_get(entry, "data_offsets");
    if (!dtype || dtype->t != J_STR || !shape || shape->t != J_ARR ||
        !offsets || offsets->t != J_ARR || offsets->len != 2 ||
        offsets->kids[0]->t != J_NUM || offsets->kids[1]->t != J_NUM) {
        throw std::runtime_error("invalid tensor metadata for `" + name + "`");
    }

    TensorInfo info;
    info.name = name;
    info.shard_name = shard_name;
    info.dtype = dtype->str;
    for (int i = 0; i < shape->len; ++i) {
        if (!shape->kids[i] || shape->kids[i]->t != J_NUM || shape->kids[i]->num < 0) {
            throw std::runtime_error("invalid shape for `" + name + "`");
        }
        info.shape.push_back((uint64_t)shape->kids[i]->num);
    }
    if (offsets->kids[0]->num < 0 || offsets->kids[1]->num < offsets->kids[0]->num) {
        throw std::runtime_error("invalid offsets for `" + name + "`");
    }
    info.data_begin = (uint64_t)offsets->kids[0]->num;
    info.data_end = (uint64_t)offsets->kids[1]->num;
    if (info.data_end > data_bytes) {
        throw std::runtime_error("tensor `" + name + "` extends beyond shard payload");
    }
    return info;
}

static Shard* ensure_shard(std::vector<std::unique_ptr<Shard>>& shards,
                           std::unordered_map<std::string, Shard*>& by_name,
                           const std::string& src_dir,
                           const std::string& shard_name) {
    auto it = by_name.find(shard_name);
    if (it != by_name.end()) return it->second;

    std::unique_ptr<Shard> shard(new Shard());
    shard->name = shard_name;
    shard->path = src_dir + "/" + shard_name;
    shard->fd = open_readonly(shard->path);

    struct stat st;
    if (::fstat(shard->fd, &st) != 0) fail_errno("could not stat", shard->path);
    if (st.st_size < 8) throw std::runtime_error("safetensor shard too small: " + shard->path);
    shard->file_size = (uint64_t)st.st_size;

    uint8_t len_bytes[8];
    pread_exact(shard->fd, len_bytes, sizeof(len_bytes), 0, shard->path + " header length");
    uint64_t header_len = read_u64_le(len_bytes);
    if (header_len == 0 || header_len > kHeaderLimit) {
        throw std::runtime_error("invalid safetensor header length in " + shard->path);
    }
    shard->data_offset = checked_add(8, header_len, "safetensor data offset");
    if (shard->data_offset > shard->file_size) {
        throw std::runtime_error("safetensor header extends past EOF: " + shard->path);
    }
    shard->data_bytes = shard->file_size - shard->data_offset;

    std::string header_text((size_t)header_len, '\0');
    pread_exact(shard->fd, &header_text[0], header_len, 8, shard->path + " header");
    shard->header = json_parse(header_text.c_str(), nullptr);
    if (!shard->header || shard->header->t != J_OBJ) {
        throw std::runtime_error("failed to parse safetensor header: " + shard->path);
    }

    Shard* raw = shard.get();
    shards.push_back(std::move(shard));
    by_name[shard_name] = raw;
    return raw;
}

static int config_int(jval* config, const char* key, int fallback) {
    jval* v = json_get(config, key);
    return (v && v->t == J_NUM) ? (int)v->num : fallback;
}

static MapleExpertStoreConfig read_config(const std::string& src_dir) {
    std::string text = read_text(src_dir + "/config.json");
    jval* config = json_parse(text.c_str(), nullptr);
    if (!config || config->t != J_OBJ) {
        if (config) json_free(config);
        throw std::runtime_error("failed to parse config.json");
    }
    std::unique_ptr<jval, void (*)(jval*)> guard(config, json_free);

    MapleExpertStoreConfig cfg;
    cfg.model_dir = src_dir;
    cfg.num_layers = config_int(config, "num_hidden_layers", cfg.num_layers);
    cfg.first_moe_layer = config_int(config, "first_k_dense_replace", cfg.first_moe_layer);
    cfg.num_experts = config_int(config, "num_experts", cfg.num_experts);
    cfg.hidden_size = config_int(config, "hidden_size", cfg.hidden_size);
    cfg.moe_intermediate_size = config_int(config, "moe_intermediate_size",
                                           cfg.moe_intermediate_size);
    return cfg;
}

static std::vector<TensorInfo> collect_resident_tensors(const std::string& src_dir) {
    std::string index_text = read_text(src_dir + "/model.safetensors.index.json");
    jval* index = json_parse(index_text.c_str(), nullptr);
    if (!index || index->t != J_OBJ) {
        if (index) json_free(index);
        throw std::runtime_error("failed to parse model.safetensors.index.json");
    }
    std::unique_ptr<jval, void (*)(jval*)> index_guard(index, json_free);
    jval* weight_map = json_get(index, "weight_map");
    if (!weight_map || weight_map->t != J_OBJ) {
        throw std::runtime_error("model.safetensors.index.json has no weight_map");
    }

    std::vector<std::unique_ptr<Shard>> shards;
    std::unordered_map<std::string, Shard*> by_name;
    std::vector<TensorInfo> resident;
    uint64_t output_offset = 0;

    for (int i = 0; i < weight_map->len; ++i) {
        std::string name = weight_map->keys[i];
        jval* shard_value = weight_map->kids[i];
        if (!shard_value || shard_value->t != J_STR) {
            throw std::runtime_error("invalid shard entry for `" + name + "`");
        }
        if (is_expert_tensor(name)) continue;
        /* FlashHead is an optional approximate output head.  The native
         * Maple runtime uses the exact lm_head tensors and the checkpoint
         * may legitimately omit this derived shard, so it is not part of
         * the resident streaming artifact. */
        if (std::string(shard_value->str).find("flashhead") != std::string::npos) continue;

        Shard* shard = ensure_shard(shards, by_name, src_dir, shard_value->str);
        TensorInfo info = parse_tensor(shard->header, name, shard->name, shard->path,
                                       shard->data_bytes);
        info.output_begin = output_offset;
        output_offset = checked_add(output_offset, info.data_end - info.data_begin,
                                    "resident safetensor payload");
        info.output_end = output_offset;
        resident.push_back(std::move(info));
    }
    return resident;
}

static void stream_copy(int out_fd, Shard* shard, const TensorInfo& tensor,
                        std::vector<uint8_t>& buffer) {
    uint64_t remaining = tensor.data_end - tensor.data_begin;
    uint64_t offset = shard->data_offset + tensor.data_begin;
    while (remaining > 0) {
        uint64_t n = remaining < buffer.size() ? remaining : buffer.size();
        pread_exact(shard->fd, buffer.data(), n, offset, tensor.name);
        write_exact(out_fd, buffer.data(), n, tensor.name);
        offset += n;
        remaining -= n;
    }
}

static void write_resident_safetensors(const std::string& src_dir, const std::string& out_path,
                                       const std::vector<TensorInfo>& resident) {
    std::string header = "{";
    for (size_t i = 0; i < resident.size(); ++i) {
        const TensorInfo& t = resident[i];
        if (i) header += ",";
        header += "\"" + json_escape(t.name) + "\":{\"dtype\":\"" + json_escape(t.dtype) + "\",";
        header += "\"shape\":" + shape_json(t.shape) + ",";
        header += "\"data_offsets\":[" + std::to_string(t.output_begin) + ",";
        header += std::to_string(t.output_end) + "]}";
    }
    header += "}";

    std::vector<std::unique_ptr<Shard>> shards;
    std::unordered_map<std::string, Shard*> by_name;
    int fd = open_output(out_path);
    std::vector<uint8_t> prefix;
    put_u64_le(prefix, header.size());
    write_exact(fd, prefix.data(), prefix.size(), "resident header length");
    write_exact(fd, header.data(), header.size(), "resident header");

    std::vector<uint8_t> buffer((size_t)kCopyBufferBytes);
    for (const TensorInfo& tensor : resident) {
        Shard* shard = ensure_shard(shards, by_name, src_dir, tensor.shard_name);
        stream_copy(fd, shard, tensor, buffer);
    }
    if (::fsync(fd) != 0) {
        int saved = errno;
        ::close(fd);
        errno = saved;
        fail_errno("could not fsync", out_path);
    }
    if (::close(fd) != 0) fail_errno("could not close", out_path);
}

static void rename_into_place(const std::string& partial, const std::string& final_path) {
    if (::rename(partial.c_str(), final_path.c_str()) != 0) {
        fail_errno("could not rename", partial + " to " + final_path);
    }
}

static void ensure_dir(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) == 0) return;
    if (errno == EEXIST) return;
    fail_errno("could not create directory", path);
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            std::fprintf(stderr, "usage: maple-pack SOURCE_MAPLE_DIR OUTPUT_DIR\n");
            return 2;
        }
        const std::string src_dir = argv[1];
        const std::string out_dir = argv[2];
        ensure_dir(out_dir);

        MapleExpertStoreConfig cfg = read_config(src_dir);
        MapleExpertStore source = MapleExpertStore::open_direct_safetensors(cfg);
        const uint64_t logical_bytes = source.expert_logical_bytes();
        const uint64_t record_bytes = align_up(logical_bytes, kRecordAlignment);
        const int moe_layers = cfg.num_layers - cfg.first_moe_layer;
        const uint64_t expert_count = checked_mul((uint64_t)moe_layers,
                                                  (uint64_t)cfg.num_experts,
                                                  "expert count");

        const std::string experts_partial = out_dir + "/maple-experts.bin.partial";
        const std::string resident_partial = out_dir + "/maple-resident.safetensors.partial";
        const std::string manifest_partial = out_dir + "/maple-manifest.json.partial";

        int experts_fd = open_output(experts_partial);
        for (int layer = cfg.first_moe_layer; layer < cfg.num_layers; ++layer) {
            for (int expert = 0; expert < cfg.num_experts; ++expert) {
                auto slab = source.read_expert(layer, expert);
                write_exact(experts_fd, slab.data.get(), slab.bytes, "packed Maple expert");
                write_zeros(experts_fd, record_bytes - slab.bytes);
            }
        }
        if (::fsync(experts_fd) != 0) {
            int saved = errno;
            ::close(experts_fd);
            errno = saved;
            fail_errno("could not fsync", experts_partial);
        }
        if (::close(experts_fd) != 0) fail_errno("could not close", experts_partial);

        std::vector<TensorInfo> resident = collect_resident_tensors(src_dir);
        write_resident_safetensors(src_dir, resident_partial, resident);

        std::string manifest = "{\n";
        manifest += "  \"schema\":\"maple-ssd-streaming-v1\",\n";
        manifest += "  \"experts_file\":\"maple-experts.bin\",\n";
        manifest += "  \"resident_file\":\"maple-resident.safetensors\",\n";
        manifest += "  \"num_layers\":" + std::to_string(cfg.num_layers) + ",\n";
        manifest += "  \"first_moe_layer\":" + std::to_string(cfg.first_moe_layer) + ",\n";
        manifest += "  \"num_experts\":" + std::to_string(cfg.num_experts) + ",\n";
        manifest += "  \"hidden_size\":" + std::to_string(cfg.hidden_size) + ",\n";
        manifest += "  \"moe_intermediate_size\":" + std::to_string(cfg.moe_intermediate_size) + ",\n";
        manifest += "  \"expert_logical_bytes\":" + std::to_string(logical_bytes) + ",\n";
        manifest += "  \"expert_record_bytes\":" + std::to_string(record_bytes) + ",\n";
        manifest += "  \"expert_count\":" + std::to_string(expert_count) + ",\n";
        manifest += "  \"record_alignment\":" + std::to_string(kRecordAlignment) + ",\n";
        manifest += "  \"resident_tensor_count\":" + std::to_string(resident.size()) + "\n";
        manifest += "}\n";
        int manifest_fd = open_output(manifest_partial);
        write_exact(manifest_fd, manifest.data(), manifest.size(), "manifest");
        if (::fsync(manifest_fd) != 0) {
            int saved = errno;
            ::close(manifest_fd);
            errno = saved;
            fail_errno("could not fsync", manifest_partial);
        }
        if (::close(manifest_fd) != 0) fail_errno("could not close", manifest_partial);

        rename_into_place(experts_partial, out_dir + "/maple-experts.bin");
        rename_into_place(resident_partial, out_dir + "/maple-resident.safetensors");
        rename_into_place(manifest_partial, out_dir + "/maple-manifest.json");

        std::printf("wrote %llu Maple expert records (%llu logical bytes each, %llu padded bytes each)\n",
                    (unsigned long long)expert_count,
                    (unsigned long long)logical_bytes,
                    (unsigned long long)record_bytes);
        std::printf("wrote %zu resident tensors\n", resident.size());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "maple-pack: %s\n", e.what());
        return 1;
    }
}
