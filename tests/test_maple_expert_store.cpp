#include "maple/maple_expert_store.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using samosa::maple::MapleExpertStore;
using samosa::maple::MapleExpertCacheConfig;
using samosa::maple::MapleExpertStoreConfig;

namespace {

struct TensorSpec {
    std::string name;
    std::string dtype;
    std::vector<int> shape;
    uint64_t begin = 0;
    uint64_t end = 0;
    std::vector<uint8_t> bytes;
};

static void die_errno(const char* what) {
    throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}

static void write_all(int fd, const void* data, size_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = ::write(fd, p + done, bytes - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            die_errno("write");
        }
        if (n == 0) throw std::runtime_error("short write");
        done += (size_t)n;
    }
}

static void write_text_file(const std::string& path, const std::string& text) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) die_errno("open text fixture");
    write_all(fd, text.data(), text.size());
    if (::close(fd) != 0) die_errno("close text fixture");
}

static std::string make_temp_dir() {
    const char* base = std::getenv("TMPDIR");
    std::string tmpl = std::string(base ? base : "/tmp") + "/maple-expert-store.XXXXXX";
    std::vector<char> mutable_path(tmpl.begin(), tmpl.end());
    mutable_path.push_back('\0');
    char* made = ::mkdtemp(mutable_path.data());
    if (!made) die_errno("mkdtemp");
    return made;
}

static void put_u64_le(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back((uint8_t)(v & 0xffu));
        v >>= 8;
    }
}

static uint64_t dtype_size(const std::string& dtype) {
    if (dtype == "U32") return 4;
    if (dtype == "BF16") return 2;
    throw std::runtime_error("bad dtype in test");
}

static uint64_t elements(const std::vector<int>& shape) {
    uint64_t n = 1;
    for (int dim : shape) n *= (uint64_t)dim;
    return n;
}

static std::string shape_json(const std::vector<int>& shape) {
    std::string out = "[";
    for (size_t i = 0; i < shape.size(); ++i) {
        if (i) out += ",";
        out += std::to_string(shape[i]);
    }
    out += "]";
    return out;
}

static uint8_t pattern_byte(int layer, int part, int expert, uint64_t byte_index) {
    return (uint8_t)(17 + layer * 31 + part * 19 + expert * 7 + (int)(byte_index % 251));
}

static std::string tensor_name(int layer, const char* suffix) {
    return "model.layers." + std::to_string(layer) + ".mlp.switch_mlp." + suffix;
}

static std::vector<TensorSpec> make_specs(int layers, int experts, int hidden, int moe,
                                          bool omit_last_tensor) {
    struct Part {
        const char* suffix;
        const char* dtype;
        int rows;
        int cols;
        int dims;
    };
    const Part parts[] = {
        {"gate_proj.weight", "U32", moe, hidden / 16, 3},
        {"gate_proj.row_alpha", "BF16", moe, 0, 2},
        {"up_proj.weight", "U32", moe, hidden / 16, 3},
        {"up_proj.row_alpha", "BF16", moe, 0, 2},
        {"down_proj.weight", "U32", hidden, moe / 16, 3},
        {"down_proj.row_alpha", "BF16", hidden, 0, 2},
    };

    std::vector<TensorSpec> specs;
    for (int layer = 0; layer < layers; ++layer) {
        for (int part = 0; part < 6; ++part) {
            if (omit_last_tensor && layer == layers - 1 && part == 5) continue;
            TensorSpec spec;
            spec.name = tensor_name(layer, parts[part].suffix);
            spec.dtype = parts[part].dtype;
            if (parts[part].dims == 3) {
                spec.shape = {experts, parts[part].rows, parts[part].cols};
            } else {
                spec.shape = {experts, parts[part].rows};
            }
            uint64_t bytes = elements(spec.shape) * dtype_size(spec.dtype);
            spec.bytes.resize((size_t)bytes);
            uint64_t per_expert = bytes / (uint64_t)experts;
            for (int expert = 0; expert < experts; ++expert) {
                for (uint64_t i = 0; i < per_expert; ++i) {
                    spec.bytes[(size_t)((uint64_t)expert * per_expert + i)] =
                        pattern_byte(layer, part, expert, i);
                }
            }
            specs.push_back(std::move(spec));
        }
    }
    return specs;
}

static void write_safetensor(const std::string& path, std::vector<TensorSpec>& specs) {
    uint64_t offset = 0;
    for (TensorSpec& spec : specs) {
        spec.begin = offset;
        spec.end = offset + spec.bytes.size();
        offset = spec.end;
    }

    std::string header = "{";
    for (size_t i = 0; i < specs.size(); ++i) {
        const TensorSpec& spec = specs[i];
        if (i) header += ",";
        header += "\"" + spec.name + "\":{\"dtype\":\"" + spec.dtype + "\",\"shape\":";
        header += shape_json(spec.shape);
        header += ",\"data_offsets\":[" + std::to_string(spec.begin) + ",";
        header += std::to_string(spec.end) + "]}";
    }
    header += "}";

    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) die_errno("open safetensor fixture");
    std::vector<uint8_t> prefix;
    put_u64_le(prefix, header.size());
    write_all(fd, prefix.data(), prefix.size());
    write_all(fd, header.data(), header.size());
    for (const TensorSpec& spec : specs) {
        write_all(fd, spec.bytes.data(), spec.bytes.size());
    }
    if (::close(fd) != 0) die_errno("close safetensor fixture");
}

static void write_index(const std::string& path, const std::vector<TensorSpec>& specs) {
    std::string text = "{\"weight_map\":{";
    for (size_t i = 0; i < specs.size(); ++i) {
        if (i) text += ",";
        text += "\"" + specs[i].name + "\":\"model-00001-of-00001.safetensors\"";
    }
    text += "}}";
    write_text_file(path, text);
}

static std::vector<uint8_t> expected_slab(const std::vector<TensorSpec>& specs,
                                          int layers, int expert) {
    (void)layers;
    std::vector<uint8_t> out;
    const int experts = 3;
    const int target_layer = 1;
    for (const TensorSpec& spec : specs) {
        if (spec.name.find("model.layers." + std::to_string(target_layer) + ".") == std::string::npos) {
            continue;
        }
        uint64_t per_expert = spec.bytes.size() / experts;
        const uint8_t* begin = spec.bytes.data() + (uint64_t)expert * per_expert;
        out.insert(out.end(), begin, begin + per_expert);
    }
    return out;
}

static void test_direct_safetensor_index_and_pread() {
    const std::string dir = make_temp_dir();
    std::vector<TensorSpec> specs = make_specs(2, 3, 32, 16, false);
    write_safetensor(dir + "/model-00001-of-00001.safetensors", specs);
    write_index(dir + "/model.safetensors.index.json", specs);

    MapleExpertStoreConfig cfg;
    cfg.model_dir = dir;
    cfg.num_layers = 2;
    cfg.first_moe_layer = 0;
    cfg.num_experts = 3;
    cfg.hidden_size = 32;
    cfg.moe_intermediate_size = 16;

    MapleExpertStore store = MapleExpertStore::open_direct_safetensors(cfg);
    auto slab = store.read_expert(1, 2);
    std::vector<uint8_t> want = expected_slab(specs, 2, 2);
    if (slab.bytes != want.size()) {
        throw std::runtime_error("unexpected slab byte count");
    }
    if (std::memcmp(slab.data.get(), want.data(), want.size()) != 0) {
        throw std::runtime_error("expert slab bytes do not match source slices");
    }
}

static void test_packed_manifest_reader() {
    const std::string dir = make_temp_dir();
    const int layers = 2;
    const int experts = 3;
    std::vector<TensorSpec> specs = make_specs(layers, experts, 32, 16, false);
    const std::vector<uint8_t> want = expected_slab(specs, layers, 2);
    const uint64_t logical_bytes = want.size();
    const uint64_t record_bytes = 1024;

    int fd = ::open((dir + "/maple-experts.bin").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) die_errno("open packed experts fixture");
    std::vector<uint8_t> zero((size_t)record_bytes, 0);
    for (int layer = 0; layer < layers; ++layer) {
        for (int expert = 0; expert < experts; ++expert) {
            std::vector<uint8_t> slab;
            for (const TensorSpec& spec : specs) {
                if (spec.name.find("model.layers." + std::to_string(layer) + ".") == std::string::npos) {
                    continue;
                }
                uint64_t per_expert = spec.bytes.size() / experts;
                const uint8_t* begin = spec.bytes.data() + (uint64_t)expert * per_expert;
                slab.insert(slab.end(), begin, begin + per_expert);
            }
            write_all(fd, slab.data(), slab.size());
            write_all(fd, zero.data(), (size_t)(record_bytes - slab.size()));
        }
    }
    if (::close(fd) != 0) die_errno("close packed experts fixture");

    std::string manifest = "{";
    manifest += "\"schema\":\"maple-ssd-streaming-v1\",";
    manifest += "\"experts_file\":\"maple-experts.bin\",";
    manifest += "\"num_layers\":2,";
    manifest += "\"first_moe_layer\":0,";
    manifest += "\"num_experts\":3,";
    manifest += "\"hidden_size\":32,";
    manifest += "\"moe_intermediate_size\":16,";
    manifest += "\"expert_logical_bytes\":" + std::to_string(logical_bytes) + ",";
    manifest += "\"expert_record_bytes\":" + std::to_string(record_bytes);
    manifest += "}";
    write_text_file(dir + "/maple-manifest.json", manifest);

    MapleExpertStore store = MapleExpertStore::open_packed(dir);
    auto slab = store.read_expert(1, 2);
    if (slab.bytes != want.size()) {
        throw std::runtime_error("unexpected packed slab byte count");
    }
    if (std::memcmp(slab.data.get(), want.data(), want.size()) != 0) {
        throw std::runtime_error("packed expert slab bytes do not match");
    }
}

static void test_rejects_missing_expert_tensor() {
    const std::string dir = make_temp_dir();
    std::vector<TensorSpec> specs = make_specs(2, 3, 32, 16, true);
    write_safetensor(dir + "/model-00001-of-00001.safetensors", specs);
    write_index(dir + "/model.safetensors.index.json", specs);

    MapleExpertStoreConfig cfg;
    cfg.model_dir = dir;
    cfg.num_layers = 2;
    cfg.first_moe_layer = 0;
    cfg.num_experts = 3;
    cfg.hidden_size = 32;
    cfg.moe_intermediate_size = 16;

    try {
        (void)MapleExpertStore::open_direct_safetensors(cfg);
    } catch (const std::runtime_error& e) {
        if (std::string(e.what()).find("missing Maple expert tensor") != std::string::npos) {
            return;
        }
        throw;
    }
    throw std::runtime_error("missing expert tensor was accepted");
}

static MapleExpertStore make_direct_store_fixture(const std::string& dir,
                                                  std::vector<TensorSpec>* specs_out) {
    std::vector<TensorSpec> specs = make_specs(2, 3, 32, 16, false);
    write_safetensor(dir + "/model-00001-of-00001.safetensors", specs);
    write_index(dir + "/model.safetensors.index.json", specs);

    MapleExpertStoreConfig cfg;
    cfg.model_dir = dir;
    cfg.num_layers = 2;
    cfg.first_moe_layer = 0;
    cfg.num_experts = 3;
    cfg.hidden_size = 32;
    cfg.moe_intermediate_size = 16;

    if (specs_out) *specs_out = specs;
    return MapleExpertStore::open_direct_safetensors(cfg);
}

static void test_cache_acquire_pins_hits_and_pressure_reclaims_after_release() {
    const std::string dir = make_temp_dir();
    std::vector<TensorSpec> specs;
    MapleExpertStore store = make_direct_store_fixture(dir, &specs);
    const std::vector<uint8_t> want = expected_slab(specs, 2, 2);

    MapleExpertCacheConfig cache_cfg;
    cache_cfg.budget_bytes = 2 * store.expert_logical_bytes();
    cache_cfg.payload_alignment = 64;
    cache_cfg.max_entries = 2;
    store.enable_cache(cache_cfg);

    {
        auto first = store.acquire_expert(1, 2);
        if (!first.cached()) throw std::runtime_error("first expert was not admitted");
        if (first.bytes() != want.size() ||
            std::memcmp(first.data(), want.data(), want.size()) != 0) {
            throw std::runtime_error("cached expert bytes do not match source");
        }

        auto second = store.acquire_expert(1, 2);
        if (!second.cached()) throw std::runtime_error("cache hit returned uncached lease");
        ecache_stats stats;
        store.cache_stats(&stats);
        if (stats.base_misses != 1 || stats.base_hits != 1 || stats.entries != 1) {
            throw std::runtime_error("unexpected Maple cache hit/miss stats");
        }

        uint64_t reclaimed = 999;
        ecache_status status =
            store.apply_cache_pressure(ECACHE_PRESSURE_CRITICAL, 0, &reclaimed);
        if (status != ECACHE_PARTIAL || reclaimed != 0) {
            throw std::runtime_error("pressure reclaimed a pinned Maple expert");
        }
    }

    uint64_t reclaimed = 0;
    ecache_status status = store.apply_cache_pressure(ECACHE_PRESSURE_CRITICAL, 0, &reclaimed);
    if (status != ECACHE_OK || reclaimed == 0) {
        throw std::runtime_error("pressure did not reclaim released Maple expert");
    }
    ecache_stats stats;
    store.cache_stats(&stats);
    if (stats.entries != 0 || stats.payload_bytes != 0 || stats.evictions == 0) {
        throw std::runtime_error("released Maple expert remained in cache");
    }
}

static void test_cache_admission_failure_returns_uncached_lease() {
    const std::string dir = make_temp_dir();
    std::vector<TensorSpec> specs;
    MapleExpertStore store = make_direct_store_fixture(dir, &specs);
    MapleExpertCacheConfig cache_cfg;
    cache_cfg.budget_bytes = store.expert_logical_bytes();
    cache_cfg.payload_alignment = 64;
    cache_cfg.max_entries = 1;
    store.enable_cache(cache_cfg);

    auto pinned = store.acquire_expert(1, 1);
    if (!pinned.cached()) throw std::runtime_error("first singleton-cache expert was not cached");

    auto fallback = store.acquire_expert(1, 2);
    if (fallback.cached()) {
        throw std::runtime_error("pinned full cache did not use uncached fallback");
    }
    std::vector<uint8_t> want = expected_slab(specs, 2, 2);
    if (fallback.bytes() != want.size() ||
        std::memcmp(fallback.data(), want.data(), want.size()) != 0) {
        throw std::runtime_error("uncached fallback bytes do not match source");
    }
    ecache_stats stats;
    store.cache_stats(&stats);
    if (stats.entries != 1 || stats.failed_admissions != 1) {
        throw std::runtime_error("Maple cache failed-admission telemetry is wrong");
    }
}

} // namespace

int main() {
    test_direct_safetensor_index_and_pread();
    test_packed_manifest_reader();
    test_rejects_missing_expert_tensor();
    test_cache_acquire_pins_hits_and_pressure_reclaims_after_release();
    test_cache_admission_failure_returns_uncached_lease();
    std::puts("PASS: maple expert store indexes safetensor slices and reads exact slabs");
    return 0;
}
