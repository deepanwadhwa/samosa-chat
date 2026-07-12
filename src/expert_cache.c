#include "expert_cache.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ECACHE_MAGIC UINT64_C(0x4543414348453031) /* "ECACHE01" */
#define HASH_EMPTY UINT32_MAX
#define HASH_TOMBSTONE (UINT32_MAX - 1u)

enum { QUEUE_COLD = 1, QUEUE_HOT = 2 };

typedef struct cache_entry {
    ecache_key key;
    void *base_payload;
    void *residual_payload;
    uint64_t base_logical;
    uint64_t base_charge;
    uint64_t base_read;
    uint64_t residual_logical;
    uint64_t residual_charge;
    uint64_t residual_read;
    uint64_t last_access;
    uint64_t admission_order;
    uint8_t occupied;
    uint8_t has_residual;
    uint8_t queue;
    uint8_t base_prefetched_unused;
    uint8_t residual_prefetched_unused;
    uint8_t planned_base_evict;
} cache_entry;

typedef struct layer_state {
    ecache_layer_floor floor;
    uint64_t base_bytes;
    uint64_t residual_bytes;
    uint64_t plan_base_bytes;
    uint32_t base_entries;
    uint32_t residual_entries;
    uint32_t plan_base_entries;
} layer_state;

struct expert_cache {
    uint64_t magic;
    ecache_config config;
    ecache_callbacks callbacks;
    size_t metadata_bytes;
    uint32_t bucket_count;
    uint32_t tombstones;
    uint32_t entry_count;
    uint64_t clock;
    cache_entry *entries;
    uint32_t *buckets;
    layer_state *layers;
    ecache_stats stats;
};

static int key_equal(ecache_key a, ecache_key b) {
    return a.layer == b.layer && a.expert == b.expert;
}

static void sat_inc(uint64_t *value) {
    if (*value != UINT64_MAX) ++*value;
}

static void sat_add(uint64_t *value, uint64_t addend) {
    if (UINT64_MAX - *value < addend) *value = UINT64_MAX;
    else *value += addend;
}

static int add_size(size_t a, size_t b, size_t *result) {
    if (SIZE_MAX - a < b) return 0;
    *result = a + b;
    return 1;
}

static int mul_size(size_t a, size_t b, size_t *result) {
    if (a && b > SIZE_MAX / a) return 0;
    *result = a * b;
    return 1;
}

static int align_size(size_t value, size_t alignment, size_t *result) {
    size_t remainder;
    if (!alignment) return 0;
    remainder = value % alignment;
    if (!remainder) {
        *result = value;
        return 1;
    }
    return add_size(value, alignment - remainder, result);
}

static ecache_status payload_charge(const expert_cache *cache,
                                    uint64_t logical, uint64_t *charge) {
    uint64_t alignment, remainder, extra;
    if (!cache || !logical || !charge) return ECACHE_ERR_SIZE;
    alignment = cache->config.payload_alignment;
    if (!alignment) return ECACHE_ERR_CORRUPT;
    remainder = logical % alignment;
    extra = remainder ? alignment - remainder : 0;
    if (UINT64_MAX - logical < extra) return ECACHE_ERR_OVERFLOW;
    *charge = logical + extra;
    return ECACHE_OK;
}

static ecache_status validate_config(const ecache_config *config,
                                     uint32_t *bucket_count) {
    uint64_t wanted, buckets;
    if (!config || !bucket_count) return ECACHE_ERR_ARGUMENT;
    if (!config->payload_alignment || !config->max_entries ||
        !config->layer_count)
        return ECACHE_ERR_SIZE;
    if (config->policy != ECACHE_POLICY_LRU &&
        config->policy != ECACHE_POLICY_2Q)
        return ECACHE_ERR_POLICY;
    if (config->max_entries >= HASH_TOMBSTONE) return ECACHE_ERR_SIZE;

    wanted = (uint64_t)config->max_entries * 2u;
    buckets = 1;
    while (buckets < wanted) {
        if (buckets > UINT32_MAX / 2u) return ECACHE_ERR_OVERFLOW;
        buckets <<= 1;
    }
    if (buckets > UINT32_MAX) return ECACHE_ERR_OVERFLOW;
    *bucket_count = (uint32_t)buckets;
    return ECACHE_OK;
}

const char *ecache_status_string(ecache_status status) {
    switch (status) {
        case ECACHE_OK: return "ok";
        case ECACHE_PARTIAL: return "partial";
        case ECACHE_ERR_ARGUMENT: return "invalid argument";
        case ECACHE_ERR_SIZE: return "invalid size";
        case ECACHE_ERR_OVERFLOW: return "integer overflow";
        case ECACHE_ERR_ALIGNMENT: return "invalid alignment";
        case ECACHE_ERR_POLICY: return "invalid policy";
        case ECACHE_ERR_NOT_FOUND: return "not found";
        case ECACHE_ERR_EXISTS: return "already exists";
        case ECACHE_ERR_NO_SPACE: return "no policy-eligible space";
        case ECACHE_ERR_CORRUPT: return "cache metadata corrupt";
        default: return "unknown cache status";
    }
}

size_t ecache_workspace_alignment(void) {
    return _Alignof(max_align_t);
}

ecache_status ecache_workspace_size(const ecache_config *config,
                                    size_t *workspace_size) {
    uint32_t bucket_count;
    size_t total, bytes;
    ecache_status status;
    if (!workspace_size) return ECACHE_ERR_ARGUMENT;
    *workspace_size = 0;
    status = validate_config(config, &bucket_count);
    if (status != ECACHE_OK) return status;

    total = sizeof(expert_cache);
    if (!align_size(total, _Alignof(cache_entry), &total) ||
        !mul_size(config->max_entries, sizeof(cache_entry), &bytes) ||
        !add_size(total, bytes, &total) ||
        !align_size(total, _Alignof(uint32_t), &total) ||
        !mul_size(bucket_count, sizeof(uint32_t), &bytes) ||
        !add_size(total, bytes, &total) ||
        !align_size(total, _Alignof(layer_state), &total) ||
        !mul_size(config->layer_count, sizeof(layer_state), &bytes) ||
        !add_size(total, bytes, &total) ||
        !align_size(total, _Alignof(max_align_t), &total))
        return ECACHE_ERR_OVERFLOW;
    *workspace_size = total;
    return ECACHE_OK;
}

ecache_status ecache_init(void *workspace, size_t workspace_size,
                          const ecache_config *config,
                          const ecache_layer_floor *layer_floors,
                          const ecache_callbacks *callbacks,
                          expert_cache **cache_out) {
    expert_cache *cache;
    uint32_t bucket_count;
    size_t needed, offset;
    ecache_status status;
    uint32_t i;

    if (!cache_out) return ECACHE_ERR_ARGUMENT;
    *cache_out = NULL;
    if (!workspace || !callbacks || !callbacks->release)
        return ECACHE_ERR_ARGUMENT;
    if ((uintptr_t)workspace % ecache_workspace_alignment())
        return ECACHE_ERR_ALIGNMENT;
    status = validate_config(config, &bucket_count);
    if (status != ECACHE_OK) return status;
    status = ecache_workspace_size(config, &needed);
    if (status != ECACHE_OK) return status;
    if (workspace_size < needed) return ECACHE_ERR_SIZE;

    memset(workspace, 0, needed);
    cache = (expert_cache *)workspace;
    offset = sizeof(*cache);
    if (!align_size(offset, _Alignof(cache_entry), &offset))
        return ECACHE_ERR_OVERFLOW;
    cache->entries = (cache_entry *)((uint8_t *)workspace + offset);
    offset += (size_t)config->max_entries * sizeof(cache_entry);
    if (!align_size(offset, _Alignof(uint32_t), &offset))
        return ECACHE_ERR_OVERFLOW;
    cache->buckets = (uint32_t *)((uint8_t *)workspace + offset);
    offset += (size_t)bucket_count * sizeof(uint32_t);
    if (!align_size(offset, _Alignof(layer_state), &offset))
        return ECACHE_ERR_OVERFLOW;
    cache->layers = (layer_state *)((uint8_t *)workspace + offset);

    cache->magic = ECACHE_MAGIC;
    cache->config = *config;
    cache->callbacks = *callbacks;
    cache->metadata_bytes = needed;
    cache->bucket_count = bucket_count;
    cache->stats.budget_bytes = config->budget_bytes;
    cache->stats.metadata_bytes = needed;
    for (i = 0; i < bucket_count; ++i) cache->buckets[i] = HASH_EMPTY;
    if (layer_floors) {
        for (i = 0; i < config->layer_count; ++i)
            cache->layers[i].floor = layer_floors[i];
    }
    *cache_out = cache;
    return ECACHE_OK;
}

static int cache_valid(const expert_cache *cache) {
    return cache && cache->magic == ECACHE_MAGIC;
}

static uint64_t hash_key(ecache_key key) {
    uint64_t x = ((uint64_t)key.layer << 32) | key.expert;
    x += UINT64_C(0x9e3779b97f4a7c15);
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

/* Returns 1 if found.  On miss, bucket receives the first reusable bucket. */
static int hash_find(const expert_cache *cache, ecache_key key,
                     uint32_t *bucket, uint32_t *slot) {
    uint32_t mask = cache->bucket_count - 1u;
    uint32_t start = (uint32_t)hash_key(key) & mask;
    uint32_t first_tombstone = HASH_EMPTY;
    uint32_t probe;
    for (probe = 0; probe < cache->bucket_count; ++probe) {
        uint32_t b = (start + probe) & mask;
        uint32_t value = cache->buckets[b];
        if (value == HASH_EMPTY) {
            if (bucket)
                *bucket = first_tombstone != HASH_EMPTY ? first_tombstone : b;
            return 0;
        }
        if (value == HASH_TOMBSTONE) {
            if (first_tombstone == HASH_EMPTY) first_tombstone = b;
            continue;
        }
        if (value < cache->config.max_entries &&
            cache->entries[value].occupied &&
            key_equal(cache->entries[value].key, key)) {
            if (bucket) *bucket = b;
            if (slot) *slot = value;
            return 1;
        }
    }
    if (bucket) *bucket = first_tombstone;
    return 0;
}

static void hash_rebuild(expert_cache *cache) {
    uint32_t i;
    for (i = 0; i < cache->bucket_count; ++i) cache->buckets[i] = HASH_EMPTY;
    cache->tombstones = 0;
    for (i = 0; i < cache->config.max_entries; ++i) {
        uint32_t bucket;
        if (!cache->entries[i].occupied) continue;
        (void)hash_find(cache, cache->entries[i].key, &bucket, NULL);
        cache->buckets[bucket] = i;
    }
}

static void hash_erase(expert_cache *cache, ecache_key key) {
    uint32_t bucket;
    if (!hash_find(cache, key, &bucket, NULL)) return;
    cache->buckets[bucket] = HASH_TOMBSTONE;
    ++cache->tombstones;
}

static uint64_t next_clock(expert_cache *cache) {
    uint32_t i;
    if (cache->clock == UINT64_MAX) {
        for (i = 0; i < cache->config.max_entries; ++i) {
            cache_entry *entry = &cache->entries[i];
            if (!entry->occupied) continue;
            entry->last_access = (entry->last_access >> 1) + 1u;
            entry->admission_order = (entry->admission_order >> 1) + 1u;
        }
        cache->clock = (cache->clock >> 1) + 1u;
    }
    return ++cache->clock;
}

static void fill_view(const cache_entry *entry, ecache_view *view) {
    if (!view) return;
    memset(view, 0, sizeof(*view));
    view->key = entry->key;
    view->state = entry->has_residual ? ECACHE_BASE_AND_RESIDUAL
                                      : ECACHE_BASE_ONLY;
    view->base_payload = entry->base_payload;
    view->residual_payload = entry->residual_payload;
    view->base_logical_bytes = entry->base_logical;
    view->base_charged_bytes = entry->base_charge;
    view->residual_logical_bytes = entry->residual_logical;
    view->residual_charged_bytes = entry->residual_charge;
    view->hot_queue = entry->queue == QUEUE_HOT;
}

static int payload_in_use(const expert_cache *cache, const void *payload) {
    uint32_t i;
    for (i = 0; i < cache->config.max_entries; ++i) {
        const cache_entry *entry = &cache->entries[i];
        if (!entry->occupied) continue;
        if (entry->base_payload == payload ||
            (entry->has_residual && entry->residual_payload == payload))
            return 1;
    }
    return 0;
}

static void note_prefetch_waste(expert_cache *cache, int unused,
                                uint64_t source_bytes) {
    if (!unused) return;
    sat_inc(&cache->stats.wasted_prefetch_planes);
    sat_add(&cache->stats.wasted_prefetch_bytes, source_bytes);
}

static void release_residual(expert_cache *cache, uint32_t slot,
                             ecache_release_reason reason,
                             int count_demotion) {
    cache_entry *entry = &cache->entries[slot];
    layer_state *layer = &cache->layers[entry->key.layer];
    ecache_release_event event;
    if (!entry->occupied || !entry->has_residual) return;

    event.key = entry->key;
    event.plane = ECACHE_PLANE_RESIDUAL;
    event.reason = reason;
    event.payload = entry->residual_payload;
    event.logical_bytes = entry->residual_logical;
    event.charged_bytes = entry->residual_charge;
    event.source_bytes_read = entry->residual_read;
    event.prefetched_unused = entry->residual_prefetched_unused != 0;

    note_prefetch_waste(cache, event.prefetched_unused,
                        event.source_bytes_read);
    cache->stats.payload_bytes -= entry->residual_charge;
    cache->stats.residual_bytes -= entry->residual_charge;
    layer->residual_bytes -= entry->residual_charge;
    --layer->residual_entries;
    if (count_demotion) sat_inc(&cache->stats.demotions);

    entry->residual_payload = NULL;
    entry->residual_logical = 0;
    entry->residual_charge = 0;
    entry->residual_read = 0;
    entry->residual_prefetched_unused = 0;
    entry->has_residual = 0;
    cache->callbacks.release(cache->callbacks.context, &event);
}

static void release_base(expert_cache *cache, uint32_t slot,
                         ecache_release_reason reason,
                         int count_eviction) {
    cache_entry *entry = &cache->entries[slot];
    layer_state *layer;
    ecache_release_event event;
    ecache_key key;
    if (!entry->occupied) return;
    if (entry->has_residual)
        release_residual(cache, slot, reason, 0);
    layer = &cache->layers[entry->key.layer];
    key = entry->key;

    event.key = entry->key;
    event.plane = ECACHE_PLANE_BASE;
    event.reason = reason;
    event.payload = entry->base_payload;
    event.logical_bytes = entry->base_logical;
    event.charged_bytes = entry->base_charge;
    event.source_bytes_read = entry->base_read;
    event.prefetched_unused = entry->base_prefetched_unused != 0;

    note_prefetch_waste(cache, event.prefetched_unused,
                        event.source_bytes_read);
    cache->stats.payload_bytes -= entry->base_charge;
    cache->stats.base_bytes -= entry->base_charge;
    layer->base_bytes -= entry->base_charge;
    --layer->base_entries;
    --cache->entry_count;
    cache->stats.entries = cache->entry_count;
    if (count_eviction) sat_inc(&cache->stats.evictions);

    hash_erase(cache, key);
    memset(entry, 0, sizeof(*entry));
    if (cache->tombstones > cache->bucket_count / 4u) hash_rebuild(cache);
    cache->callbacks.release(cache->callbacks.context, &event);
}

static int base_floor_allows(const expert_cache *cache, uint32_t slot,
                             int planning) {
    const cache_entry *entry = &cache->entries[slot];
    const layer_state *layer;
    uint64_t bytes;
    uint32_t entries;
    if (!entry->occupied || entry->planned_base_evict) return 0;
    layer = &cache->layers[entry->key.layer];
    bytes = planning ? layer->plan_base_bytes : layer->base_bytes;
    entries = planning ? layer->plan_base_entries : layer->base_entries;
    if (!entries || bytes < entry->base_charge) return 0;
    return entries - 1u >= layer->floor.min_base_entries &&
           bytes - entry->base_charge >= layer->floor.min_base_bytes;
}

static int candidate_precedes(const expert_cache *cache,
                              const cache_entry *a, uint32_t a_slot,
                              const cache_entry *b, uint32_t b_slot) {
    if (cache->config.policy == ECACHE_POLICY_2Q) {
        if (a->queue != b->queue) return a->queue == QUEUE_COLD;
        if (a->queue == QUEUE_COLD) {
            if (a->admission_order != b->admission_order)
                return a->admission_order < b->admission_order;
        } else if (a->last_access != b->last_access) {
            return a->last_access < b->last_access;
        }
    } else if (a->last_access != b->last_access) {
        return a->last_access < b->last_access;
    }
    return a_slot < b_slot;
}

static uint32_t choose_residual(const expert_cache *cache) {
    uint32_t best = UINT32_MAX, i;
    for (i = 0; i < cache->config.max_entries; ++i) {
        const cache_entry *entry = &cache->entries[i];
        if (!entry->occupied || !entry->has_residual) continue;
        if (best == UINT32_MAX ||
            candidate_precedes(cache, entry, i, &cache->entries[best], best))
            best = i;
    }
    return best;
}

static uint32_t choose_base(const expert_cache *cache,
                            uint32_t protected_slot, int planning) {
    uint32_t best = UINT32_MAX, i;
    for (i = 0; i < cache->config.max_entries; ++i) {
        const cache_entry *entry = &cache->entries[i];
        if (i == protected_slot || !base_floor_allows(cache, i, planning))
            continue;
        if (best == UINT32_MAX ||
            candidate_precedes(cache, entry, i, &cache->entries[best], best))
            best = i;
    }
    return best;
}

static uint64_t required_reclaim(const expert_cache *cache,
                                 uint64_t incoming) {
    uint64_t available = cache->config.budget_bytes -
                         cache->stats.payload_bytes;
    return incoming > available ? incoming - available : 0;
}

/* Non-mutating admission proof.  Temporary planning fields are internal and
 * cleared before return, including every failure path. */
static int can_make_room(expert_cache *cache, uint64_t incoming,
                         int need_entry_slot, uint32_t protected_slot) {
    uint64_t needed, freed;
    uint32_t bases = 0, i;
    int require_base;

    if (incoming > cache->config.budget_bytes) return 0;
    needed = required_reclaim(cache, incoming);
    require_base = (need_entry_slot &&
                    cache->entry_count >= cache->config.max_entries) ||
                   cache->stats.residual_bytes < needed;
    if (!require_base) return 1;

    /* Once any base must leave, the runtime policy drops every residual first. */
    freed = cache->stats.residual_bytes;
    for (i = 0; i < cache->config.layer_count; ++i) {
        cache->layers[i].plan_base_bytes = cache->layers[i].base_bytes;
        cache->layers[i].plan_base_entries = cache->layers[i].base_entries;
    }
    for (;;) {
        uint32_t candidate;
        if (freed >= needed &&
            (!need_entry_slot || bases >= 1u)) break;
        candidate = choose_base(cache, protected_slot, 1);
        if (candidate == UINT32_MAX) {
            for (i = 0; i < cache->config.max_entries; ++i)
                cache->entries[i].planned_base_evict = 0;
            for (i = 0; i < cache->config.layer_count; ++i) {
                cache->layers[i].plan_base_bytes = cache->layers[i].base_bytes;
                cache->layers[i].plan_base_entries =
                    cache->layers[i].base_entries;
            }
            return 0;
        }
        cache->entries[candidate].planned_base_evict = 1;
        cache->layers[cache->entries[candidate].key.layer].plan_base_bytes -=
            cache->entries[candidate].base_charge;
        --cache->layers[cache->entries[candidate].key.layer].plan_base_entries;
        freed += cache->entries[candidate].base_charge;
        ++bases;
    }
    for (i = 0; i < cache->config.max_entries; ++i)
        cache->entries[i].planned_base_evict = 0;
    for (i = 0; i < cache->config.layer_count; ++i) {
        cache->layers[i].plan_base_bytes = cache->layers[i].base_bytes;
        cache->layers[i].plan_base_entries = cache->layers[i].base_entries;
    }
    return 1;
}

static void make_room(expert_cache *cache, uint64_t incoming,
                      int need_entry_slot, uint32_t protected_slot) {
    for (;;) {
        uint64_t needed = required_reclaim(cache, incoming);
        int slot_needed = need_entry_slot &&
                          cache->entry_count >= cache->config.max_entries;
        uint32_t candidate;
        if (!needed && !slot_needed) return;
        candidate = choose_residual(cache);
        if (candidate != UINT32_MAX) {
            release_residual(cache, candidate, ECACHE_RELEASE_DEMOTION, 1);
            continue;
        }
        candidate = choose_base(cache, protected_slot, 0);
        /* can_make_room proved this cannot happen. */
        if (candidate == UINT32_MAX) return;
        release_base(cache, candidate, ECACHE_RELEASE_EVICTION, 1);
    }
}

static uint32_t free_slot(const expert_cache *cache) {
    uint32_t i;
    for (i = 0; i < cache->config.max_entries; ++i)
        if (!cache->entries[i].occupied) return i;
    return UINT32_MAX;
}

ecache_status ecache_get(expert_cache *cache, ecache_key key,
                         ecache_requirement requirement,
                         ecache_lookup_result *result,
                         ecache_view *view) {
    uint32_t slot;
    cache_entry *entry;
    if (!cache_valid(cache) || !result || !view)
        return ECACHE_ERR_ARGUMENT;
    memset(view, 0, sizeof(*view));
    if (key.layer >= cache->config.layer_count)
        return ECACHE_ERR_ARGUMENT;
    if (requirement != ECACHE_REQUIRE_BASE &&
        requirement != ECACHE_REQUIRE_FULL)
        return ECACHE_ERR_ARGUMENT;

    if (!hash_find(cache, key, NULL, &slot)) {
        sat_inc(&cache->stats.base_misses);
        if (requirement == ECACHE_REQUIRE_FULL)
            sat_inc(&cache->stats.residual_misses);
        *result = ECACHE_LOOKUP_BASE_MISS;
        return ECACHE_OK;
    }

    entry = &cache->entries[slot];
    sat_inc(&cache->stats.base_hits);
    sat_add(&cache->stats.base_bytes_avoided, entry->base_read);
    entry->base_prefetched_unused = 0;
    entry->last_access = next_clock(cache);
    if (cache->config.policy == ECACHE_POLICY_2Q) entry->queue = QUEUE_HOT;

    if (requirement == ECACHE_REQUIRE_FULL) {
        if (!entry->has_residual) {
            sat_inc(&cache->stats.residual_misses);
            *result = ECACHE_LOOKUP_RESIDUAL_MISS;
        } else {
            sat_inc(&cache->stats.residual_hits);
            sat_add(&cache->stats.residual_bytes_avoided,
                    entry->residual_read);
            entry->residual_prefetched_unused = 0;
            *result = ECACHE_LOOKUP_HIT;
        }
    } else {
        *result = ECACHE_LOOKUP_HIT;
    }
    fill_view(entry, view);
    return ECACHE_OK;
}

ecache_status ecache_peek(const expert_cache *cache, ecache_key key,
                          ecache_view *view) {
    uint32_t slot;
    if (!cache_valid(cache) || !view) return ECACHE_ERR_ARGUMENT;
    memset(view, 0, sizeof(*view));
    if (key.layer >= cache->config.layer_count)
        return ECACHE_ERR_ARGUMENT;
    if (!hash_find(cache, key, NULL, &slot)) return ECACHE_ERR_NOT_FOUND;
    fill_view(&cache->entries[slot], view);
    return ECACHE_OK;
}

ecache_status ecache_insert_base(expert_cache *cache, ecache_key key,
                                 void *payload, uint64_t logical_bytes,
                                 uint64_t source_bytes_read,
                                 ecache_admission admission,
                                 ecache_view *view) {
    uint64_t charge, order;
    uint32_t slot, bucket;
    cache_entry *entry;
    layer_state *layer;
    ecache_status status;
    if (!cache_valid(cache) || !payload) return ECACHE_ERR_ARGUMENT;
    if (key.layer >= cache->config.layer_count)
        return ECACHE_ERR_ARGUMENT;
    if (admission != ECACHE_ADMIT_DEMAND &&
        admission != ECACHE_ADMIT_PREFETCH)
        return ECACHE_ERR_ARGUMENT;
    status = payload_charge(cache, logical_bytes, &charge);
    if (status != ECACHE_OK) return status;
    if (hash_find(cache, key, NULL, NULL)) return ECACHE_ERR_EXISTS;
    if (payload_in_use(cache, payload)) return ECACHE_ERR_EXISTS;
    if (!can_make_room(cache, charge, 1, UINT32_MAX)) {
        sat_inc(&cache->stats.failed_admissions);
        return ECACHE_ERR_NO_SPACE;
    }
    make_room(cache, charge, 1, UINT32_MAX);
    slot = free_slot(cache);
    if (slot == UINT32_MAX || hash_find(cache, key, &bucket, NULL) ||
        bucket == HASH_EMPTY)
        return ECACHE_ERR_CORRUPT;

    order = next_clock(cache);
    entry = &cache->entries[slot];
    memset(entry, 0, sizeof(*entry));
    entry->key = key;
    entry->base_payload = payload;
    entry->base_logical = logical_bytes;
    entry->base_charge = charge;
    entry->base_read = source_bytes_read;
    entry->last_access = order;
    entry->admission_order = order;
    entry->occupied = 1;
    entry->queue = QUEUE_COLD;
    entry->base_prefetched_unused = admission == ECACHE_ADMIT_PREFETCH;
    if (cache->buckets[bucket] == HASH_TOMBSTONE) --cache->tombstones;
    cache->buckets[bucket] = slot;
    ++cache->entry_count;

    layer = &cache->layers[key.layer];
    layer->base_bytes += charge;
    ++layer->base_entries;
    cache->stats.payload_bytes += charge;
    cache->stats.base_bytes += charge;
    cache->stats.entries = cache->entry_count;
    if (cache->stats.payload_bytes > cache->stats.peak_payload_bytes)
        cache->stats.peak_payload_bytes = cache->stats.payload_bytes;
    sat_add(&cache->stats.base_bytes_read, source_bytes_read);
    fill_view(entry, view);
    return ECACHE_OK;
}

ecache_status ecache_promote(expert_cache *cache, ecache_key key,
                             void *residual_payload,
                             uint64_t logical_bytes,
                             uint64_t source_bytes_read,
                             ecache_admission admission,
                             ecache_view *view) {
    uint64_t charge;
    uint32_t slot;
    cache_entry *entry;
    layer_state *layer;
    ecache_status status;
    if (!cache_valid(cache) || !residual_payload)
        return ECACHE_ERR_ARGUMENT;
    if (key.layer >= cache->config.layer_count)
        return ECACHE_ERR_ARGUMENT;
    if (admission != ECACHE_ADMIT_DEMAND &&
        admission != ECACHE_ADMIT_PREFETCH)
        return ECACHE_ERR_ARGUMENT;
    if (!hash_find(cache, key, NULL, &slot)) return ECACHE_ERR_NOT_FOUND;
    entry = &cache->entries[slot];
    if (entry->has_residual) return ECACHE_ERR_EXISTS;
    if (payload_in_use(cache, residual_payload)) return ECACHE_ERR_EXISTS;
    status = payload_charge(cache, logical_bytes, &charge);
    if (status != ECACHE_OK) return status;
    if (!can_make_room(cache, charge, 0, slot)) {
        sat_inc(&cache->stats.failed_admissions);
        return ECACHE_ERR_NO_SPACE;
    }
    make_room(cache, charge, 0, slot);

    /* The protected slot and base payload survived make_room unchanged. */
    entry = &cache->entries[slot];
    entry->residual_payload = residual_payload;
    entry->residual_logical = logical_bytes;
    entry->residual_charge = charge;
    entry->residual_read = source_bytes_read;
    entry->residual_prefetched_unused = admission == ECACHE_ADMIT_PREFETCH;
    entry->has_residual = 1;
    layer = &cache->layers[key.layer];
    layer->residual_bytes += charge;
    ++layer->residual_entries;
    cache->stats.payload_bytes += charge;
    cache->stats.residual_bytes += charge;
    if (cache->stats.payload_bytes > cache->stats.peak_payload_bytes)
        cache->stats.peak_payload_bytes = cache->stats.payload_bytes;
    sat_add(&cache->stats.residual_bytes_read, source_bytes_read);
    sat_inc(&cache->stats.promotions);
    fill_view(entry, view);
    return ECACHE_OK;
}

ecache_status ecache_remove(expert_cache *cache, ecache_key key) {
    uint32_t slot;
    if (!cache_valid(cache)) return ECACHE_ERR_ARGUMENT;
    if (key.layer >= cache->config.layer_count)
        return ECACHE_ERR_ARGUMENT;
    if (!hash_find(cache, key, NULL, &slot)) return ECACHE_ERR_NOT_FOUND;
    if (cache->entries[slot].has_residual)
        release_residual(cache, slot, ECACHE_RELEASE_EXPLICIT, 0);
    release_base(cache, slot, ECACHE_RELEASE_EXPLICIT, 0);
    return ECACHE_OK;
}

ecache_status ecache_apply_pressure(expert_cache *cache,
                                    ecache_pressure pressure,
                                    uint64_t target_bytes,
                                    uint64_t *reclaimed_bytes) {
    uint64_t before;
    if (!cache_valid(cache) || !reclaimed_bytes)
        return ECACHE_ERR_ARGUMENT;
    *reclaimed_bytes = 0;
    if (target_bytes > cache->config.budget_bytes)
        return ECACHE_ERR_SIZE;
    if (pressure != ECACHE_PRESSURE_NORMAL &&
        pressure != ECACHE_PRESSURE_WARN &&
        pressure != ECACHE_PRESSURE_CRITICAL)
        return ECACHE_ERR_ARGUMENT;
    before = cache->stats.payload_bytes;
    if (pressure == ECACHE_PRESSURE_NORMAL) return ECACHE_OK;
    if (pressure == ECACHE_PRESSURE_WARN)
        sat_inc(&cache->stats.pressure_warn_events);
    else
        sat_inc(&cache->stats.pressure_critical_events);

    while (cache->stats.payload_bytes > target_bytes) {
        uint32_t slot = choose_residual(cache);
        if (slot == UINT32_MAX) break;
        release_residual(cache, slot,
                         pressure == ECACHE_PRESSURE_WARN
                             ? ECACHE_RELEASE_PRESSURE_WARN
                             : ECACHE_RELEASE_PRESSURE_CRITICAL,
                         1);
    }
    if (pressure == ECACHE_PRESSURE_CRITICAL) {
        while (cache->stats.payload_bytes > target_bytes) {
            uint32_t slot = choose_base(cache, UINT32_MAX, 0);
            if (slot == UINT32_MAX) break;
            release_base(cache, slot, ECACHE_RELEASE_PRESSURE_CRITICAL, 1);
        }
    }
    *reclaimed_bytes = before - cache->stats.payload_bytes;
    return cache->stats.payload_bytes <= target_bytes ? ECACHE_OK
                                                       : ECACHE_PARTIAL;
}

void ecache_get_stats(const expert_cache *cache, ecache_stats *stats) {
    if (!stats) return;
    if (!cache_valid(cache)) {
        memset(stats, 0, sizeof(*stats));
        return;
    }
    *stats = cache->stats;
}

ecache_status ecache_get_layer_usage(const expert_cache *cache,
                                     uint32_t layer,
                                     ecache_layer_usage *usage) {
    const layer_state *state;
    if (!cache_valid(cache) || !usage) return ECACHE_ERR_ARGUMENT;
    if (layer >= cache->config.layer_count) return ECACHE_ERR_ARGUMENT;
    state = &cache->layers[layer];
    usage->base_bytes = state->base_bytes;
    usage->residual_bytes = state->residual_bytes;
    usage->base_entries = state->base_entries;
    usage->residual_entries = state->residual_entries;
    usage->floor = state->floor;
    return ECACHE_OK;
}

ecache_status ecache_validate(const expert_cache *cache) {
    uint64_t base_bytes = 0, residual_bytes = 0;
    uint32_t occupied = 0, i, j;
    if (!cache_valid(cache)) return ECACHE_ERR_ARGUMENT;
    if (!cache->entries || !cache->buckets || !cache->layers ||
        !cache->callbacks.release)
        return ECACHE_ERR_CORRUPT;
    if (!cache->bucket_count ||
        (cache->bucket_count & (cache->bucket_count - 1u)) ||
        cache->bucket_count < cache->config.max_entries * 2u)
        return ECACHE_ERR_CORRUPT;
    if (cache->stats.budget_bytes != cache->config.budget_bytes ||
        cache->stats.metadata_bytes != cache->metadata_bytes)
        return ECACHE_ERR_CORRUPT;

    for (i = 0; i < cache->config.max_entries; ++i) {
        const cache_entry *entry = &cache->entries[i];
        uint32_t slot, references = 0;
        uint64_t charge;
        if (!entry->occupied) continue;
        ++occupied;
        if (entry->key.layer >= cache->config.layer_count ||
            !entry->base_payload || !entry->base_logical ||
            (entry->queue != QUEUE_COLD && entry->queue != QUEUE_HOT) ||
            !entry->last_access || !entry->admission_order ||
            entry->planned_base_evict)
            return ECACHE_ERR_CORRUPT;
        if (payload_charge(cache, entry->base_logical, &charge) != ECACHE_OK ||
            charge != entry->base_charge ||
            UINT64_MAX - base_bytes < charge)
            return ECACHE_ERR_CORRUPT;
        base_bytes += charge;
        if (entry->has_residual) {
            if (!entry->residual_payload || !entry->residual_logical ||
                entry->residual_payload == entry->base_payload ||
                payload_charge(cache, entry->residual_logical, &charge) !=
                    ECACHE_OK ||
                charge != entry->residual_charge ||
                UINT64_MAX - residual_bytes < charge)
                return ECACHE_ERR_CORRUPT;
            residual_bytes += charge;
        } else if (entry->residual_payload || entry->residual_logical ||
                   entry->residual_charge || entry->residual_read ||
                   entry->residual_prefetched_unused) {
            return ECACHE_ERR_CORRUPT;
        }
        if (!hash_find(cache, entry->key, NULL, &slot) || slot != i)
            return ECACHE_ERR_CORRUPT;
        for (j = 0; j < cache->bucket_count; ++j)
            if (cache->buckets[j] == i) ++references;
        if (references != 1u) return ECACHE_ERR_CORRUPT;
        for (j = i + 1u; j < cache->config.max_entries; ++j) {
            const cache_entry *other = &cache->entries[j];
            if (!other->occupied) continue;
            if (key_equal(entry->key, other->key) ||
                entry->base_payload == other->base_payload ||
                (other->has_residual &&
                 entry->base_payload == other->residual_payload) ||
                (entry->has_residual &&
                 (entry->residual_payload == other->base_payload ||
                  (other->has_residual &&
                   entry->residual_payload == other->residual_payload))))
                return ECACHE_ERR_CORRUPT;
        }
    }

    for (i = 0; i < cache->bucket_count; ++i) {
        uint32_t slot = cache->buckets[i];
        if (slot == HASH_EMPTY || slot == HASH_TOMBSTONE) continue;
        if (slot >= cache->config.max_entries ||
            !cache->entries[slot].occupied)
            return ECACHE_ERR_CORRUPT;
    }

    if (occupied != cache->entry_count || occupied != cache->stats.entries ||
        base_bytes != cache->stats.base_bytes ||
        residual_bytes != cache->stats.residual_bytes ||
        UINT64_MAX - base_bytes < residual_bytes ||
        base_bytes + residual_bytes != cache->stats.payload_bytes ||
        cache->stats.payload_bytes > cache->config.budget_bytes ||
        cache->stats.peak_payload_bytes < cache->stats.payload_bytes ||
        cache->stats.peak_payload_bytes > cache->config.budget_bytes)
        return ECACHE_ERR_CORRUPT;

    for (i = 0; i < cache->config.layer_count; ++i) {
        uint64_t layer_base = 0, layer_residual = 0;
        uint32_t layer_entries = 0, layer_residual_entries = 0;
        for (j = 0; j < cache->config.max_entries; ++j) {
            const cache_entry *entry = &cache->entries[j];
            if (!entry->occupied || entry->key.layer != i) continue;
            layer_base += entry->base_charge;
            ++layer_entries;
            if (entry->has_residual) {
                layer_residual += entry->residual_charge;
                ++layer_residual_entries;
            }
        }
        if (layer_base != cache->layers[i].base_bytes ||
            layer_residual != cache->layers[i].residual_bytes ||
            layer_entries != cache->layers[i].base_entries ||
            layer_residual_entries != cache->layers[i].residual_entries)
            return ECACHE_ERR_CORRUPT;
    }
    return ECACHE_OK;
}

ecache_status ecache_destroy(expert_cache *cache) {
    uint32_t i;
    if (!cache_valid(cache)) return ECACHE_ERR_ARGUMENT;
    for (i = 0; i < cache->config.max_entries; ++i) {
        if (!cache->entries[i].occupied) continue;
        if (cache->entries[i].has_residual)
            release_residual(cache, i, ECACHE_RELEASE_DESTROY, 0);
        release_base(cache, i, ECACHE_RELEASE_DESTROY, 0);
    }
    cache->magic = 0;
    return ECACHE_OK;
}
