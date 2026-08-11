#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "expert_cache.h"

namespace samosa {
namespace maple {

struct MapleExpertStoreConfig {
    std::string model_dir;
    int num_layers = 24;
    int first_moe_layer = 0;
    int num_experts = 256;
    int hidden_size = 2048;
    int moe_intermediate_size = 512;
};

enum class MapleExpertPart {
    GateWeight = 0,
    GateRowAlpha = 1,
    UpWeight = 2,
    UpRowAlpha = 3,
    DownWeight = 4,
    DownRowAlpha = 5,
};

struct MapleExpertRegion {
    MapleExpertPart part;
    int shard_index = -1;
    uint64_t source_offset = 0;
    uint64_t bytes = 0;
    uint64_t slab_offset = 0;
};

struct MapleExpertRecord {
    int layer = 0;
    int expert = 0;
    uint64_t logical_bytes = 0;
    MapleExpertRegion regions[6];
};

struct MapleExpertSlab {
    std::unique_ptr<uint8_t, void (*)(void*)> data;
    uint64_t bytes = 0;

    MapleExpertSlab();
};

struct MapleExpertCacheConfig {
    uint64_t budget_bytes = 0;
    uint64_t payload_alignment = 16384;
    uint32_t max_entries = 0;
    uint32_t min_entries_per_layer = 0;
};

class MapleExpertStore;

class MapleExpertLease {
public:
    MapleExpertLease();
    MapleExpertLease(MapleExpertLease&& other) noexcept;
    MapleExpertLease& operator=(MapleExpertLease&& other) noexcept;
    ~MapleExpertLease();

    MapleExpertLease(const MapleExpertLease&) = delete;
    MapleExpertLease& operator=(const MapleExpertLease&) = delete;

    const uint8_t* data() const;
    uint8_t* data();
    uint64_t bytes() const { return bytes_; }
    int layer() const { return layer_; }
    int expert() const { return expert_; }
    bool cached() const { return cached_; }
    explicit operator bool() const { return data() != nullptr; }

private:
    friend class MapleExpertStore;

    MapleExpertLease(MapleExpertStore* store, int layer, int expert,
                     void* cached_payload, uint64_t bytes);
    MapleExpertLease(int layer, int expert, MapleExpertSlab&& slab);

    void release() noexcept;

    MapleExpertStore* store_ = nullptr;
    int layer_ = -1;
    int expert_ = -1;
    void* cached_payload_ = nullptr;
    MapleExpertSlab uncached_;
    uint64_t bytes_ = 0;
    bool cached_ = false;
};

class MapleExpertStore {
public:
    MapleExpertStore();
    MapleExpertStore(MapleExpertStore&& other) noexcept;
    MapleExpertStore& operator=(MapleExpertStore&& other) noexcept;
    ~MapleExpertStore();

    MapleExpertStore(const MapleExpertStore&) = delete;
    MapleExpertStore& operator=(const MapleExpertStore&) = delete;

    static MapleExpertStore open_direct_safetensors(const MapleExpertStoreConfig& cfg);
    static MapleExpertStore open_packed(const std::string& model_dir);

    const MapleExpertStoreConfig& config() const { return cfg_; }
    uint64_t expert_logical_bytes() const { return expert_logical_bytes_; }
    const MapleExpertRecord& record(int layer, int expert) const;

    void read_expert(int layer, int expert, void* dst, uint64_t dst_bytes) const;
    MapleExpertSlab read_expert(int layer, int expert) const;

    void enable_cache(const MapleExpertCacheConfig& config = MapleExpertCacheConfig());
    void disable_cache();
    bool cache_enabled() const;
    MapleExpertLease acquire_expert(int layer, int expert);
    ecache_status apply_cache_pressure(ecache_pressure pressure,
                                       uint64_t target_bytes,
                                       uint64_t* reclaimed_bytes);
    void cache_stats(ecache_stats* stats) const;

private:
    friend class MapleExpertLease;

    struct Shard;
    struct CachedExpert;
    struct CacheState;

    MapleExpertStoreConfig cfg_;
    std::vector<Shard> shards_;
    std::vector<MapleExpertRecord> records_;
    uint64_t expert_logical_bytes_ = 0;
    std::unique_ptr<CacheState> cache_;

    bool is_moe_layer(int layer) const;
    size_t record_index(int layer, int expert) const;
    void unpin_cached(int layer, int expert) noexcept;
    static void release_cached_payload(void* context,
                                       const ecache_release_event* event);
};

} // namespace maple
} // namespace samosa
