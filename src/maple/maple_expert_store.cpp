#include "maple_expert_store.h"

#include "../json.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>

namespace samosa {
namespace maple {

namespace {

constexpr uint64_t kSafetensorHeaderLimit = 128ull * 1024ull * 1024ull;
constexpr int kPartCount = 6;
/* Four real Maple experts per MoE layer is ~73.5 MiB total.  Keep the
 * production default close to the guarded 64 MiB parity configuration; users
 * can opt into a larger cache explicitly. */
constexpr uint64_t kDefaultCacheEntriesPerLayer = 4;
constexpr uint64_t kMaxCacheBudgetBytes = 8ull * 1024ull * 1024ull * 1024ull;

struct TensorInfo {
    std::string dtype;
    std::vector<uint64_t> shape;
    uint64_t data_begin = 0;
    uint64_t data_end = 0;
};

static void throw_errno(const std::string& what, const std::string& path) {
    throw std::runtime_error(what + " " + path + ": " + std::strerror(errno));
}

static uint64_t checked_mul(uint64_t a, uint64_t b, const char* label) {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) {
        throw std::runtime_error(std::string("integer overflow while sizing ") + label);
    }
    return a * b;
}

static uint64_t checked_add(uint64_t a, uint64_t b, const char* label) {
    if (b > std::numeric_limits<uint64_t>::max() - a) {
        throw std::runtime_error(std::string("integer overflow while sizing ") + label);
    }
    return a + b;
}

static uint64_t dtype_size(const std::string& dtype) {
    if (dtype == "U32") return 4;
    if (dtype == "BF16") return 2;
    throw std::runtime_error("unsupported Maple expert tensor dtype: " + dtype);
}

static uint64_t tensor_element_count(const std::vector<uint64_t>& shape) {
    uint64_t n = 1;
    for (uint64_t dim : shape) {
        n = checked_mul(n, dim, "tensor elements");
    }
    return n;
}

static std::string slurp_text(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw_errno("could not open", path);
    if (std::fseek(f, 0, SEEK_END) != 0) {
        int saved = errno;
        std::fclose(f);
        errno = saved;
        throw_errno("could not seek", path);
    }
    long size = std::ftell(f);
    if (size < 0) {
        int saved = errno;
        std::fclose(f);
        errno = saved;
        throw_errno("could not size", path);
    }
    if (std::fseek(f, 0, SEEK_SET) != 0) {
        int saved = errno;
        std::fclose(f);
        errno = saved;
        throw_errno("could not rewind", path);
    }
    std::string text((size_t)size, '\0');
    if (!text.empty() && std::fread(&text[0], 1, text.size(), f) != text.size()) {
        int saved = ferror(f) ? errno : EIO;
        std::fclose(f);
        errno = saved;
        throw_errno("could not read", path);
    }
    std::fclose(f);
    return text;
}

static uint64_t read_u64_le(const uint8_t bytes[8]) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | bytes[i];
    }
    return v;
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
        if (n == 0) {
            throw std::runtime_error("short pread while reading " + label);
        }
        done += (uint64_t)n;
    }
}

static jval* require_obj(jval* root, const std::string& key, const std::string& path) {
    jval* v = json_get(root, key.c_str());
    if (!v || v->t != J_OBJ) {
        throw std::runtime_error("missing object `" + key + "` in " + path);
    }
    return v;
}

static std::string require_weight_map_entry(jval* weight_map, const std::string& tensor) {
    jval* v = json_get(weight_map, tensor.c_str());
    if (!v || v->t != J_STR) {
        throw std::runtime_error("missing Maple expert tensor in weight_map: " + tensor);
    }
    return v->str;
}

static TensorInfo parse_tensor_info(jval* header, const std::string& tensor,
                                    const std::string& shard_path) {
    jval* entry = json_get(header, tensor.c_str());
    if (!entry || entry->t != J_OBJ) {
        throw std::runtime_error("tensor `" + tensor + "` not found in " + shard_path);
    }
    jval* dtype = json_get(entry, "dtype");
    if (!dtype || dtype->t != J_STR) {
        throw std::runtime_error("tensor `" + tensor + "` has no dtype in " + shard_path);
    }
    jval* shape = json_get(entry, "shape");
    if (!shape || shape->t != J_ARR) {
        throw std::runtime_error("tensor `" + tensor + "` has no shape in " + shard_path);
    }
    jval* offsets = json_get(entry, "data_offsets");
    if (!offsets || offsets->t != J_ARR || offsets->len != 2) {
        throw std::runtime_error("tensor `" + tensor + "` has invalid data_offsets in " + shard_path);
    }

    TensorInfo info;
    info.dtype = dtype->str;
    for (int i = 0; i < shape->len; ++i) {
        if (!shape->kids[i] || shape->kids[i]->t != J_NUM || shape->kids[i]->num < 0) {
            throw std::runtime_error("tensor `" + tensor + "` has invalid shape in " + shard_path);
        }
        info.shape.push_back((uint64_t)shape->kids[i]->num);
    }
    if (offsets->kids[0]->t != J_NUM || offsets->kids[1]->t != J_NUM ||
        offsets->kids[0]->num < 0 || offsets->kids[1]->num < offsets->kids[0]->num) {
        throw std::runtime_error("tensor `" + tensor + "` has invalid data_offsets in " + shard_path);
    }
    info.data_begin = (uint64_t)offsets->kids[0]->num;
    info.data_end = (uint64_t)offsets->kids[1]->num;
    return info;
}

static std::string part_suffix(MapleExpertPart part) {
    switch (part) {
        case MapleExpertPart::GateWeight: return "gate_proj.weight";
        case MapleExpertPart::GateRowAlpha: return "gate_proj.row_alpha";
        case MapleExpertPart::UpWeight: return "up_proj.weight";
        case MapleExpertPart::UpRowAlpha: return "up_proj.row_alpha";
        case MapleExpertPart::DownWeight: return "down_proj.weight";
        case MapleExpertPart::DownRowAlpha: return "down_proj.row_alpha";
    }
    throw std::runtime_error("unknown Maple expert part");
}

static std::string tensor_name(int layer, MapleExpertPart part) {
    return "model.layers." + std::to_string(layer) + ".mlp.switch_mlp." + part_suffix(part);
}

static std::vector<uint64_t> expected_shape(const MapleExpertStoreConfig& cfg,
                                            MapleExpertPart part) {
    const uint64_t e = (uint64_t)cfg.num_experts;
    const uint64_t h = (uint64_t)cfg.hidden_size;
    const uint64_t m = (uint64_t)cfg.moe_intermediate_size;
    switch (part) {
        case MapleExpertPart::GateWeight:
        case MapleExpertPart::UpWeight:
            return {e, m, h / 16};
        case MapleExpertPart::DownWeight:
            return {e, h, m / 16};
        case MapleExpertPart::GateRowAlpha:
        case MapleExpertPart::UpRowAlpha:
            return {e, m};
        case MapleExpertPart::DownRowAlpha:
            return {e, h};
    }
    throw std::runtime_error("unknown Maple expert part");
}

static const char* expected_dtype(MapleExpertPart part) {
    switch (part) {
        case MapleExpertPart::GateWeight:
        case MapleExpertPart::UpWeight:
        case MapleExpertPart::DownWeight:
            return "U32";
        case MapleExpertPart::GateRowAlpha:
        case MapleExpertPart::UpRowAlpha:
        case MapleExpertPart::DownRowAlpha:
            return "BF16";
    }
    return "";
}

static uint64_t expected_part_bytes(const MapleExpertStoreConfig& cfg,
                                    MapleExpertPart part) {
    std::vector<uint64_t> shape = expected_shape(cfg, part);
    return checked_mul(tensor_element_count(shape), dtype_size(expected_dtype(part)),
                       "Maple expert part bytes") / (uint64_t)cfg.num_experts;
}

static void validate_tensor(const MapleExpertStoreConfig& cfg, MapleExpertPart part,
                            const TensorInfo& info, const std::string& tensor,
                            const std::string& shard_path, uint64_t shard_data_bytes) {
    if (info.dtype != expected_dtype(part)) {
        throw std::runtime_error("tensor `" + tensor + "` has dtype " + info.dtype +
                                 ", expected " + expected_dtype(part));
    }
    std::vector<uint64_t> expected = expected_shape(cfg, part);
    if (info.shape != expected) {
        throw std::runtime_error("tensor `" + tensor + "` has unexpected shape in " + shard_path);
    }
    if (info.data_end > shard_data_bytes) {
        throw std::runtime_error("tensor `" + tensor + "` exceeds safetensor payload in " + shard_path);
    }
    uint64_t expected_bytes = checked_mul(tensor_element_count(info.shape), dtype_size(info.dtype),
                                          "tensor bytes");
    if (info.data_end - info.data_begin != expected_bytes) {
        throw std::runtime_error("tensor `" + tensor + "` byte length does not match dtype and shape");
    }
    if (cfg.num_experts <= 0 || expected_bytes % (uint64_t)cfg.num_experts != 0) {
        throw std::runtime_error("tensor `" + tensor + "` is not evenly divisible by expert count");
    }
}

static int open_readonly(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw_errno("could not open", path);
    return fd;
}

static uint64_t align_u64(uint64_t value, uint64_t alignment, const char* label) {
    if (!alignment) throw std::runtime_error("zero alignment while sizing Maple cache");
    uint64_t rem = value % alignment;
    if (!rem) return value;
    return checked_add(value, alignment - rem, label);
}

static uint64_t parse_cache_budget_env(uint64_t fallback) {
    const char* env = std::getenv("SAMOSA_MAPLE_EXPERT_BUDGET_MB");
    if (!env || !*env) return fallback;
    char* end = nullptr;
    errno = 0;
    unsigned long long mb = std::strtoull(env, &end, 10);
    if (errno || !end || *end || mb == 0) {
        throw std::runtime_error("invalid SAMOSA_MAPLE_EXPERT_BUDGET_MB");
    }
    if (mb > std::numeric_limits<uint64_t>::max() / (1024ull * 1024ull)) {
        throw std::runtime_error("SAMOSA_MAPLE_EXPERT_BUDGET_MB overflows bytes");
    }
    return (uint64_t)mb * 1024ull * 1024ull;
}

} // namespace

struct MapleExpertStore::Shard {
    std::string name;
    std::string path;
    int fd = -1;
    uint64_t file_size = 0;
    uint64_t data_offset = 0;
    uint64_t data_bytes = 0;
    jval* header = nullptr;

    Shard() = default;
    Shard(Shard&& other) noexcept {
        *this = std::move(other);
    }
    Shard& operator=(Shard&& other) noexcept {
        if (this == &other) return *this;
        close();
        name = std::move(other.name);
        path = std::move(other.path);
        fd = other.fd;
        file_size = other.file_size;
        data_offset = other.data_offset;
        data_bytes = other.data_bytes;
        header = other.header;
        other.fd = -1;
        other.header = nullptr;
        return *this;
    }
    Shard(const Shard&) = delete;
    Shard& operator=(const Shard&) = delete;
    ~Shard() { close(); }

    void close() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        if (header) {
            json_free(header);
            header = nullptr;
        }
    }
};

struct MapleExpertStore::CachedExpert {
    MapleExpertSlab slab;
    int layer = -1;
    int expert = -1;
};

struct MapleExpertStore::CacheState {
    std::vector<uint8_t> workspace;
    expert_cache* cache = nullptr;
    mutable std::mutex mu;
};

MapleExpertSlab::MapleExpertSlab() : data(nullptr, std::free) {}

MapleExpertLease::MapleExpertLease() = default;

MapleExpertLease::MapleExpertLease(MapleExpertStore* store, int layer, int expert,
                                   void* cached_payload, uint64_t bytes)
    : store_(store),
      layer_(layer),
      expert_(expert),
      cached_payload_(cached_payload),
      bytes_(bytes),
      cached_(true) {}

MapleExpertLease::MapleExpertLease(int layer, int expert, MapleExpertSlab&& slab)
    : layer_(layer),
      expert_(expert),
      uncached_(std::move(slab)),
      bytes_(uncached_.bytes),
      cached_(false) {}

MapleExpertLease::MapleExpertLease(MapleExpertLease&& other) noexcept
    : store_(other.store_),
      layer_(other.layer_),
      expert_(other.expert_),
      cached_payload_(other.cached_payload_),
      uncached_(std::move(other.uncached_)),
      bytes_(other.bytes_),
      cached_(other.cached_) {
    other.store_ = nullptr;
    other.layer_ = -1;
    other.expert_ = -1;
    other.cached_payload_ = nullptr;
    other.bytes_ = 0;
    other.cached_ = false;
}

MapleExpertLease& MapleExpertLease::operator=(MapleExpertLease&& other) noexcept {
    if (this == &other) return *this;
    release();
    store_ = other.store_;
    layer_ = other.layer_;
    expert_ = other.expert_;
    cached_payload_ = other.cached_payload_;
    uncached_ = std::move(other.uncached_);
    bytes_ = other.bytes_;
    cached_ = other.cached_;
    other.store_ = nullptr;
    other.layer_ = -1;
    other.expert_ = -1;
    other.cached_payload_ = nullptr;
    other.bytes_ = 0;
    other.cached_ = false;
    return *this;
}

MapleExpertLease::~MapleExpertLease() {
    release();
}

const uint8_t* MapleExpertLease::data() const {
    if (cached_) {
        const MapleExpertStore::CachedExpert* payload =
            static_cast<const MapleExpertStore::CachedExpert*>(cached_payload_);
        return payload ? payload->slab.data.get() : nullptr;
    }
    return uncached_.data.get();
}

uint8_t* MapleExpertLease::data() {
    if (cached_) {
        MapleExpertStore::CachedExpert* payload =
            static_cast<MapleExpertStore::CachedExpert*>(cached_payload_);
        return payload ? payload->slab.data.get() : nullptr;
    }
    return uncached_.data.get();
}

void MapleExpertLease::release() noexcept {
    if (cached_ && store_) {
        store_->unpin_cached(layer_, expert_);
    }
    store_ = nullptr;
    layer_ = -1;
    expert_ = -1;
    cached_payload_ = nullptr;
    bytes_ = 0;
    cached_ = false;
}

MapleExpertStore::MapleExpertStore() = default;
MapleExpertStore::MapleExpertStore(MapleExpertStore&& other) noexcept
    : cfg_(std::move(other.cfg_)),
      shards_(std::move(other.shards_)),
      records_(std::move(other.records_)),
      expert_logical_bytes_(other.expert_logical_bytes_),
      cache_(std::move(other.cache_)) {
    other.expert_logical_bytes_ = 0;
}

MapleExpertStore& MapleExpertStore::operator=(MapleExpertStore&& other) noexcept {
    if (this == &other) return *this;
    if (cache_) {
        try {
            disable_cache();
        } catch (...) {
            cache_.release();
        }
    }
    cfg_ = std::move(other.cfg_);
    shards_ = std::move(other.shards_);
    records_ = std::move(other.records_);
    expert_logical_bytes_ = other.expert_logical_bytes_;
    cache_ = std::move(other.cache_);
    other.expert_logical_bytes_ = 0;
    return *this;
}
MapleExpertStore::~MapleExpertStore() {
    if (!cache_) return;
    try {
        disable_cache();
    } catch (...) {
    }
}

void MapleExpertStore::release_cached_payload(void*, const ecache_release_event* event) {
    delete static_cast<CachedExpert*>(event ? event->payload : nullptr);
}

bool MapleExpertStore::is_moe_layer(int layer) const {
    return layer >= cfg_.first_moe_layer && layer < cfg_.num_layers;
}

size_t MapleExpertStore::record_index(int layer, int expert) const {
    if (!is_moe_layer(layer) || expert < 0 || expert >= cfg_.num_experts) {
        throw std::runtime_error("Maple expert key out of range");
    }
    return (size_t)(layer - cfg_.first_moe_layer) * (size_t)cfg_.num_experts + (size_t)expert;
}

const MapleExpertRecord& MapleExpertStore::record(int layer, int expert) const {
    return records_.at(record_index(layer, expert));
}

void MapleExpertStore::read_expert(int layer, int expert, void* dst, uint64_t dst_bytes) const {
    const MapleExpertRecord& rec = record(layer, expert);
    if (!dst || dst_bytes < rec.logical_bytes) {
        throw std::runtime_error("Maple expert destination buffer is too small");
    }
    for (const MapleExpertRegion& region : rec.regions) {
        if (region.bytes == 0) continue;
        if (region.shard_index < 0 || (size_t)region.shard_index >= shards_.size()) {
            throw std::runtime_error("Maple expert region references invalid shard");
        }
        const Shard& shard = shards_[(size_t)region.shard_index];
        pread_exact(shard.fd, static_cast<uint8_t*>(dst) + region.slab_offset, region.bytes,
                    region.source_offset, shard.name + " expert region");
    }
}

MapleExpertSlab MapleExpertStore::read_expert(int layer, int expert) const {
    const MapleExpertRecord& rec = record(layer, expert);
    MapleExpertSlab slab;
    void* ptr = nullptr;
    if (posix_memalign(&ptr, 16384, (size_t)rec.logical_bytes) != 0) {
        throw std::runtime_error("could not allocate aligned Maple expert slab");
    }
    slab.data.reset(static_cast<uint8_t*>(ptr));
    slab.bytes = rec.logical_bytes;
    read_expert(layer, expert, slab.data.get(), slab.bytes);
    return slab;
}

void MapleExpertStore::enable_cache(const MapleExpertCacheConfig& user_config) {
    if (records_.empty() || !expert_logical_bytes_) {
        throw std::runtime_error("cannot enable Maple expert cache before opening a store");
    }
    if (!user_config.payload_alignment ||
        (user_config.payload_alignment & (user_config.payload_alignment - 1ull))) {
        throw std::runtime_error("Maple expert cache alignment must be a power of two");
    }
    const uint64_t charged_expert_bytes =
        align_u64(expert_logical_bytes_, user_config.payload_alignment,
                  "Maple expert cache charge");
    const uint64_t moe_layers = (uint64_t)(cfg_.num_layers - cfg_.first_moe_layer);
    const uint64_t total_records = checked_mul(moe_layers, (uint64_t)cfg_.num_experts,
                                               "Maple expert record count");
    uint64_t default_entries = checked_mul(moe_layers, kDefaultCacheEntriesPerLayer,
                                           "Maple default cache entries");
    if (default_entries > total_records) default_entries = total_records;
    if (!default_entries) {
        throw std::runtime_error("Maple expert cache has no MoE records to cache");
    }

    uint64_t budget_bytes = user_config.budget_bytes;
    if (!budget_bytes) {
        budget_bytes = parse_cache_budget_env(
            checked_mul(default_entries, charged_expert_bytes,
                        "Maple default cache budget"));
    }
    if (budget_bytes < charged_expert_bytes) {
        throw std::runtime_error("Maple expert cache budget is smaller than one expert");
    }
    if (budget_bytes > kMaxCacheBudgetBytes) {
        throw std::runtime_error("Maple expert cache budget exceeds hard maximum");
    }

    uint64_t budget_entries = budget_bytes / charged_expert_bytes;
    if (!budget_entries) {
        throw std::runtime_error("Maple expert cache budget admits no experts");
    }
    if (budget_entries > total_records) budget_entries = total_records;
    uint64_t max_entries = user_config.max_entries ? user_config.max_entries
                                                   : budget_entries;
    if (max_entries > budget_entries) max_entries = budget_entries;
    if (max_entries > total_records) max_entries = total_records;
    if (!max_entries || max_entries > UINT32_MAX) {
        throw std::runtime_error("Maple expert cache entry count is invalid");
    }

    ecache_config config;
    std::memset(&config, 0, sizeof(config));
    config.budget_bytes = budget_bytes;
    config.payload_alignment = user_config.payload_alignment;
    config.max_entries = (uint32_t)max_entries;
    config.layer_count = (uint32_t)cfg_.num_layers;
    config.policy = ECACHE_POLICY_LRU;

    std::vector<ecache_layer_floor> floors((size_t)cfg_.num_layers);
    if (user_config.min_entries_per_layer) {
        if (user_config.min_entries_per_layer > (uint32_t)cfg_.num_experts) {
            throw std::runtime_error("Maple expert cache layer floor exceeds expert count");
        }
        for (int layer = cfg_.first_moe_layer; layer < cfg_.num_layers; ++layer) {
            floors[(size_t)layer].min_base_entries = user_config.min_entries_per_layer;
        }
    }

    size_t workspace_bytes = 0;
    ecache_status status = ecache_workspace_size(&config, &workspace_bytes);
    if (status != ECACHE_OK) {
        throw std::runtime_error(std::string("Maple expert cache workspace sizing failed: ") +
                                 ecache_status_string(status));
    }

    std::unique_ptr<CacheState> next(new CacheState());
    next->workspace.resize(workspace_bytes);
    ecache_callbacks callbacks;
    callbacks.release = &MapleExpertStore::release_cached_payload;
    callbacks.context = this;
    status = ecache_init(next->workspace.data(), next->workspace.size(), &config,
                         floors.empty() ? nullptr : floors.data(), &callbacks,
                         &next->cache);
    if (status != ECACHE_OK) {
        throw std::runtime_error(std::string("Maple expert cache init failed: ") +
                                 ecache_status_string(status));
    }

    disable_cache();
    cache_ = std::move(next);
}

void MapleExpertStore::disable_cache() {
    if (!cache_) return;
    std::unique_ptr<CacheState> old = std::move(cache_);
    {
        std::lock_guard<std::mutex> lock(old->mu);
        ecache_status status = ecache_destroy(old->cache);
        if (status != ECACHE_OK) {
            cache_ = std::move(old);
            throw std::runtime_error(std::string("Maple expert cache destroy failed: ") +
                                     ecache_status_string(status));
        }
    }
}

bool MapleExpertStore::cache_enabled() const {
    return cache_ && cache_->cache;
}

MapleExpertLease MapleExpertStore::acquire_expert(int layer, int expert) {
    (void)record(layer, expert);
    if (!cache_) {
        return MapleExpertLease(layer, expert, read_expert(layer, expert));
    }

    const ecache_key key = {(uint32_t)layer, (uint32_t)expert};
    {
        std::lock_guard<std::mutex> lock(cache_->mu);
        ecache_lookup_result result;
        ecache_view view;
        ecache_status status =
            ecache_get(cache_->cache, key, ECACHE_REQUIRE_BASE, &result, &view);
        if (status != ECACHE_OK) {
            throw std::runtime_error(std::string("Maple expert cache lookup failed: ") +
                                     ecache_status_string(status));
        }
        if (result == ECACHE_LOOKUP_HIT) {
            status = ecache_pin(cache_->cache, key, &view);
            if (status != ECACHE_OK) {
                throw std::runtime_error(std::string("Maple expert cache pin failed: ") +
                                         ecache_status_string(status));
            }
            return MapleExpertLease(this, layer, expert, view.base_payload,
                                    view.base_logical_bytes);
        }
    }

    MapleExpertSlab slab = read_expert(layer, expert);
    std::unique_ptr<CachedExpert> payload(new CachedExpert());
    payload->layer = layer;
    payload->expert = expert;
    payload->slab = std::move(slab);
    const uint64_t payload_bytes = payload->slab.bytes;

    std::lock_guard<std::mutex> lock(cache_->mu);
    ecache_view view;
    ecache_status status =
        ecache_insert_base(cache_->cache, key, payload.get(), payload_bytes,
                           payload_bytes, ECACHE_ADMIT_DEMAND, &view);
    if (status == ECACHE_OK) {
        CachedExpert* admitted = payload.release();
        status = ecache_pin(cache_->cache, key, &view);
        if (status != ECACHE_OK) {
            (void)ecache_remove(cache_->cache, key);
            throw std::runtime_error(std::string("Maple expert cache pin after insert failed: ") +
                                     ecache_status_string(status));
        }
        return MapleExpertLease(this, layer, expert, admitted, payload_bytes);
    }
    if (status == ECACHE_ERR_NO_SPACE) {
        MapleExpertSlab uncached = std::move(payload->slab);
        return MapleExpertLease(layer, expert, std::move(uncached));
    }
    throw std::runtime_error(std::string("Maple expert cache insert failed: ") +
                             ecache_status_string(status));
}

ecache_status MapleExpertStore::apply_cache_pressure(ecache_pressure pressure,
                                                     uint64_t target_bytes,
                                                     uint64_t* reclaimed_bytes) {
    if (!cache_ || !reclaimed_bytes) return ECACHE_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(cache_->mu);
    return ecache_apply_pressure(cache_->cache, pressure, target_bytes, reclaimed_bytes);
}

void MapleExpertStore::cache_stats(ecache_stats* stats) const {
    if (!stats) return;
    if (!cache_) {
        std::memset(stats, 0, sizeof(*stats));
        return;
    }
    std::lock_guard<std::mutex> lock(cache_->mu);
    ecache_get_stats(cache_->cache, stats);
}

void MapleExpertStore::unpin_cached(int layer, int expert) noexcept {
    if (!cache_) return;
    std::lock_guard<std::mutex> lock(cache_->mu);
    ecache_view view;
    ecache_key key;
    key.layer = (uint32_t)layer;
    key.expert = (uint32_t)expert;
    (void)ecache_unpin(cache_->cache, key, &view);
}

MapleExpertStore MapleExpertStore::open_packed(const std::string& model_dir) {
    if (model_dir.empty()) throw std::runtime_error("Maple packed model_dir is empty");
    const std::string manifest_path = model_dir + "/maple-manifest.json";
    std::string manifest_text = slurp_text(manifest_path);
    jval* manifest = json_parse(manifest_text.c_str(), nullptr);
    if (!manifest || manifest->t != J_OBJ) {
        if (manifest) json_free(manifest);
        throw std::runtime_error("failed to parse " + manifest_path);
    }
    std::unique_ptr<jval, void (*)(jval*)> manifest_guard(manifest, json_free);

    auto number_field = [&](const char* key) -> int {
        jval* v = json_get(manifest, key);
        if (!v || v->t != J_NUM) {
            throw std::runtime_error(std::string("missing numeric field in maple-manifest.json: ") + key);
        }
        return (int)v->num;
    };
    auto u64_field = [&](const char* key) -> uint64_t {
        jval* v = json_get(manifest, key);
        if (!v || v->t != J_NUM || v->num < 0) {
            throw std::runtime_error(std::string("missing uint64 field in maple-manifest.json: ") + key);
        }
        return (uint64_t)v->num;
    };
    auto string_field = [&](const char* key, const char* fallback) -> std::string {
        jval* v = json_get(manifest, key);
        if (!v) return fallback;
        if (v->t != J_STR) {
            throw std::runtime_error(std::string("manifest field must be a string: ") + key);
        }
        return v->str;
    };

    MapleExpertStore store;
    store.cfg_.model_dir = model_dir;
    store.cfg_.num_layers = number_field("num_layers");
    store.cfg_.first_moe_layer = number_field("first_moe_layer");
    store.cfg_.num_experts = number_field("num_experts");
    store.cfg_.hidden_size = number_field("hidden_size");
    store.cfg_.moe_intermediate_size = number_field("moe_intermediate_size");
    store.expert_logical_bytes_ = u64_field("expert_logical_bytes");
    const uint64_t record_bytes = u64_field("expert_record_bytes");
    if (record_bytes < store.expert_logical_bytes_) {
        throw std::runtime_error("packed Maple expert record is smaller than logical expert bytes");
    }
    if (store.cfg_.num_layers <= 0 || store.cfg_.num_experts <= 0 ||
        store.cfg_.first_moe_layer < 0 || store.cfg_.first_moe_layer > store.cfg_.num_layers) {
        throw std::runtime_error("packed Maple manifest has invalid dimensions");
    }

    Shard shard;
    shard.name = string_field("experts_file", "maple-experts.bin");
    shard.path = model_dir + "/" + shard.name;
    shard.fd = open_readonly(shard.path);
    struct stat st;
    if (::fstat(shard.fd, &st) != 0) throw_errno("could not stat", shard.path);
    if (st.st_size < 0) throw std::runtime_error("packed Maple experts file has invalid size");
    shard.file_size = (uint64_t)st.st_size;
    shard.data_offset = 0;
    shard.data_bytes = shard.file_size;
    store.shards_.push_back(std::move(shard));

    const int moe_layers = store.cfg_.num_layers - store.cfg_.first_moe_layer;
    const uint64_t record_count = checked_mul((uint64_t)moe_layers,
                                              (uint64_t)store.cfg_.num_experts,
                                              "packed Maple expert count");
    const uint64_t required_bytes = checked_mul(record_count, record_bytes,
                                                "packed Maple experts file size");
    if (store.shards_[0].file_size < required_bytes) {
        throw std::runtime_error("packed Maple experts file is truncated");
    }

    store.records_.resize((size_t)record_count);
    for (int layer = store.cfg_.first_moe_layer; layer < store.cfg_.num_layers; ++layer) {
        for (int expert = 0; expert < store.cfg_.num_experts; ++expert) {
            MapleExpertRecord& rec = store.records_[store.record_index(layer, expert)];
            const uint64_t packed_index =
                (uint64_t)(layer - store.cfg_.first_moe_layer) *
                    (uint64_t)store.cfg_.num_experts +
                (uint64_t)expert;
            rec.layer = layer;
            rec.expert = expert;
            rec.logical_bytes = store.expert_logical_bytes_;
            uint64_t slab_offset = 0;
            const uint64_t record_base = checked_mul(packed_index, record_bytes,
                                                     "packed Maple expert offset");
            for (int i = 0; i < kPartCount; ++i) {
                MapleExpertPart part = static_cast<MapleExpertPart>(i);
                uint64_t bytes = expected_part_bytes(store.cfg_, part);
                rec.regions[i].part = part;
                rec.regions[i].shard_index = 0;
                rec.regions[i].source_offset =
                    checked_add(record_base, slab_offset, "packed Maple expert part offset");
                rec.regions[i].bytes = bytes;
                rec.regions[i].slab_offset = slab_offset;
                slab_offset = checked_add(slab_offset, bytes, "packed Maple expert slab offset");
            }
            if (slab_offset != store.expert_logical_bytes_) {
                throw std::runtime_error("packed Maple manifest logical byte size does not match dimensions");
            }
        }
    }

    return store;
}

MapleExpertStore MapleExpertStore::open_direct_safetensors(const MapleExpertStoreConfig& cfg) {
    if (cfg.model_dir.empty()) throw std::runtime_error("Maple expert model_dir is empty");
    if (cfg.num_layers <= 0 || cfg.num_experts <= 0 || cfg.hidden_size <= 0 ||
        cfg.moe_intermediate_size <= 0) {
        throw std::runtime_error("Maple expert store has invalid model dimensions");
    }
    if ((cfg.hidden_size % 16) != 0 || (cfg.moe_intermediate_size % 16) != 0) {
        throw std::runtime_error("Maple 2-bit expert dimensions must be multiples of 16");
    }
    if (cfg.first_moe_layer < 0 || cfg.first_moe_layer > cfg.num_layers) {
        throw std::runtime_error("Maple first_moe_layer is out of range");
    }

    MapleExpertStore store;
    store.cfg_ = cfg;

    const std::string index_path = cfg.model_dir + "/model.safetensors.index.json";
    std::string index_text = slurp_text(index_path);
    jval* index = json_parse(index_text.c_str(), nullptr);
    if (!index) throw std::runtime_error("failed to parse " + index_path);
    std::unique_ptr<jval, void (*)(jval*)> index_guard(index, json_free);
    jval* weight_map = require_obj(index, "weight_map", index_path);

    std::unordered_map<std::string, int> shard_by_name;
    auto ensure_shard = [&](const std::string& shard_name) -> int {
        auto it = shard_by_name.find(shard_name);
        if (it != shard_by_name.end()) return it->second;

        Shard shard;
        shard.name = shard_name;
        shard.path = cfg.model_dir + "/" + shard_name;
        shard.fd = open_readonly(shard.path);

        struct stat st;
        if (::fstat(shard.fd, &st) != 0) throw_errno("could not stat", shard.path);
        if (st.st_size < 8) {
            throw std::runtime_error("safetensor shard is too small: " + shard.path);
        }
        shard.file_size = (uint64_t)st.st_size;

        uint8_t len_bytes[8];
        pread_exact(shard.fd, len_bytes, sizeof(len_bytes), 0, shard.path + " header length");
        const uint64_t header_len = read_u64_le(len_bytes);
        if (header_len == 0 || header_len > kSafetensorHeaderLimit) {
            throw std::runtime_error("invalid safetensor header length in " + shard.path);
        }
        shard.data_offset = checked_add(8, header_len, "safetensor data offset");
        if (shard.data_offset > shard.file_size) {
            throw std::runtime_error("safetensor header extends past EOF in " + shard.path);
        }
        shard.data_bytes = shard.file_size - shard.data_offset;

        std::string header_text((size_t)header_len, '\0');
        pread_exact(shard.fd, &header_text[0], header_len, 8, shard.path + " header");
        shard.header = json_parse(header_text.c_str(), nullptr);
        if (!shard.header || shard.header->t != J_OBJ) {
            throw std::runtime_error("failed to parse safetensor header in " + shard.path);
        }

        int index = (int)store.shards_.size();
        store.shards_.push_back(std::move(shard));
        shard_by_name[shard_name] = index;
        return index;
    };

    const int moe_layers = cfg.num_layers - cfg.first_moe_layer;
    store.records_.resize((size_t)moe_layers * (size_t)cfg.num_experts);

    for (int layer = cfg.first_moe_layer; layer < cfg.num_layers; ++layer) {
        for (int expert = 0; expert < cfg.num_experts; ++expert) {
            MapleExpertRecord& rec = store.records_[store.record_index(layer, expert)];
            rec.layer = layer;
            rec.expert = expert;
            rec.logical_bytes = 0;
        }

        for (int part_i = 0; part_i < kPartCount; ++part_i) {
            MapleExpertPart part = static_cast<MapleExpertPart>(part_i);
            std::string name = tensor_name(layer, part);
            std::string shard_name = require_weight_map_entry(weight_map, name);
            int shard_index = ensure_shard(shard_name);
            const Shard& shard = store.shards_[(size_t)shard_index];
            TensorInfo info = parse_tensor_info(shard.header, name, shard.path);
            validate_tensor(cfg, part, info, name, shard.path, shard.data_bytes);

            const uint64_t tensor_bytes = info.data_end - info.data_begin;
            const uint64_t per_expert_bytes = tensor_bytes / (uint64_t)cfg.num_experts;
            const uint64_t abs_begin = checked_add(shard.data_offset, info.data_begin,
                                                   "safetensor tensor offset");

            for (int expert = 0; expert < cfg.num_experts; ++expert) {
                MapleExpertRecord& rec = store.records_[store.record_index(layer, expert)];
                MapleExpertRegion& region = rec.regions[part_i];
                region.part = part;
                region.shard_index = shard_index;
                region.source_offset = checked_add(abs_begin,
                                                   checked_mul((uint64_t)expert, per_expert_bytes,
                                                               "expert tensor offset"),
                                                   "expert source offset");
                region.bytes = per_expert_bytes;
                region.slab_offset = rec.logical_bytes;
                rec.logical_bytes = checked_add(rec.logical_bytes, per_expert_bytes,
                                                "expert slab size");
            }
        }
    }

    if (!store.records_.empty()) {
        store.expert_logical_bytes_ = store.records_[0].logical_bytes;
        for (const MapleExpertRecord& rec : store.records_) {
            if (rec.logical_bytes != store.expert_logical_bytes_) {
                throw std::runtime_error("Maple expert records have inconsistent byte sizes");
            }
        }
    }

    return store;
}

} // namespace maple
} // namespace samosa
