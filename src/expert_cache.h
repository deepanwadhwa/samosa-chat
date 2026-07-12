#ifndef COLIBRI_EXPERT_CACHE_H
#define COLIBRI_EXPERT_CACHE_H

/*
 * Global byte-budget cache policy for independently fetchable expert planes.
 *
 * The cache owns no model-specific allocation and performs no I/O.  Callers
 * provide one aligned metadata workspace and opaque payload handles.  A
 * successful admission transfers a handle to the cache; the release callback
 * returns it on demotion, eviction, removal, or destruction.  This makes all
 * payload allocation visible in exact, alignment-rounded byte counters while
 * keeping mmap, malloc, Metal, and other storage choices outside this module.
 *
 * The API is not thread-safe.  Serialize calls externally.  Release callbacks
 * must not re-enter the cache.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct expert_cache expert_cache;

typedef enum ecache_status {
    ECACHE_OK = 0,
    /* A pressure request reclaimed everything permitted by its policy/floors
     * but could not reach the requested target.  The cache remains valid. */
    ECACHE_PARTIAL,
    ECACHE_ERR_ARGUMENT,
    ECACHE_ERR_SIZE,
    ECACHE_ERR_OVERFLOW,
    ECACHE_ERR_ALIGNMENT,
    ECACHE_ERR_POLICY,
    ECACHE_ERR_NOT_FOUND,
    ECACHE_ERR_EXISTS,
    ECACHE_ERR_NO_SPACE,
    ECACHE_ERR_CORRUPT
} ecache_status;

typedef struct ecache_key {
    uint32_t layer;
    uint32_t expert;
} ecache_key;

typedef enum ecache_state {
    ECACHE_BASE_ONLY = 1,
    ECACHE_BASE_AND_RESIDUAL = 2
} ecache_state;

typedef enum ecache_requirement {
    ECACHE_REQUIRE_BASE = 1,
    ECACHE_REQUIRE_FULL = 2
} ecache_requirement;

typedef enum ecache_lookup_result {
    ECACHE_LOOKUP_HIT = 1,
    ECACHE_LOOKUP_BASE_MISS,
    ECACHE_LOOKUP_RESIDUAL_MISS
} ecache_lookup_result;

typedef enum ecache_policy {
    /* Exact recency, with stable slot-index tie breaking. */
    ECACHE_POLICY_LRU = 1,
    /* New entries enter a FIFO cold queue.  Their first subsequent demand
     * hit promotes them to a hot LRU queue.  Cold entries are reclaimed first. */
    ECACHE_POLICY_2Q = 2
} ecache_policy;

typedef enum ecache_admission {
    ECACHE_ADMIT_DEMAND = 1,
    ECACHE_ADMIT_PREFETCH = 2
} ecache_admission;

typedef enum ecache_plane {
    ECACHE_PLANE_BASE = 1,
    ECACHE_PLANE_RESIDUAL = 2
} ecache_plane;

typedef enum ecache_release_reason {
    ECACHE_RELEASE_DEMOTION = 1,
    ECACHE_RELEASE_EVICTION,
    ECACHE_RELEASE_EXPLICIT,
    ECACHE_RELEASE_PRESSURE_WARN,
    ECACHE_RELEASE_PRESSURE_CRITICAL,
    ECACHE_RELEASE_DESTROY
} ecache_release_reason;

typedef enum ecache_pressure {
    /* NORMAL is a no-op and exists so pressure controllers can pass through
     * their state without conditionals. */
    ECACHE_PRESSURE_NORMAL = 0,
    /* WARN releases residual planes only, never base planes. */
    ECACHE_PRESSURE_WARN = 1,
    /* CRITICAL releases all residuals first, then floor-eligible bases. */
    ECACHE_PRESSURE_CRITICAL = 2
} ecache_pressure;

typedef struct ecache_config {
    uint64_t budget_bytes;
    uint64_t payload_alignment;
    uint32_t max_entries;
    uint32_t layer_count;
    ecache_policy policy;
} ecache_config;

/* Both constraints are enforced when automatic policy wants to evict a base.
 * An explicit remove/destroy is deliberately allowed to cross these floors. */
typedef struct ecache_layer_floor {
    uint64_t min_base_bytes;
    uint32_t min_base_entries;
} ecache_layer_floor;

typedef struct ecache_release_event {
    ecache_key key;
    ecache_plane plane;
    ecache_release_reason reason;
    void *payload;
    uint64_t logical_bytes;
    uint64_t charged_bytes;
    uint64_t source_bytes_read;
    int prefetched_unused;
} ecache_release_event;

typedef void (*ecache_release_fn)(void *context,
                                  const ecache_release_event *event);

typedef struct ecache_callbacks {
    ecache_release_fn release;
    void *context;
} ecache_callbacks;

typedef struct ecache_view {
    ecache_key key;
    ecache_state state;
    void *base_payload;
    void *residual_payload;
    uint64_t base_logical_bytes;
    uint64_t base_charged_bytes;
    uint64_t residual_logical_bytes;
    uint64_t residual_charged_bytes;
    int hot_queue;
} ecache_view;

typedef struct ecache_layer_usage {
    uint64_t base_bytes;
    uint64_t residual_bytes;
    uint32_t base_entries;
    uint32_t residual_entries;
    ecache_layer_floor floor;
} ecache_layer_usage;

typedef struct ecache_stats {
    uint64_t budget_bytes;
    uint64_t metadata_bytes;
    uint64_t payload_bytes;
    uint64_t base_bytes;
    uint64_t residual_bytes;
    uint64_t peak_payload_bytes;
    uint32_t entries;

    uint64_t base_hits;
    uint64_t base_misses;
    uint64_t residual_hits;
    uint64_t residual_misses;
    uint64_t base_bytes_read;
    uint64_t residual_bytes_read;
    /* Hits avoid the recorded source_bytes_read for that resident plane. */
    uint64_t base_bytes_avoided;
    uint64_t residual_bytes_avoided;

    uint64_t promotions;
    uint64_t demotions;
    uint64_t evictions;
    uint64_t failed_admissions;
    uint64_t pressure_warn_events;
    uint64_t pressure_critical_events;
    uint64_t wasted_prefetch_planes;
    uint64_t wasted_prefetch_bytes;
} ecache_stats;

const char *ecache_status_string(ecache_status status);

/* The module allocates nothing.  Allocate workspace_size bytes at this
 * alignment (malloc/calloc already satisfy it), then pass it to init.
 * workspace_size includes all entry, hash-index, and per-layer metadata. */
size_t ecache_workspace_alignment(void);
ecache_status ecache_workspace_size(const ecache_config *config,
                                    size_t *workspace_size);
ecache_status ecache_init(void *workspace, size_t workspace_size,
                          const ecache_config *config,
                          const ecache_layer_floor *layer_floors,
                          const ecache_callbacks *callbacks,
                          expert_cache **cache);

/* Releases every owned plane and invalidates the object. */
ecache_status ecache_destroy(expert_cache *cache);

/* Demand lookup updates policy recency and hit/miss/avoided-I/O counters.
 * A base-only result for REQUIRE_FULL is RESIDUAL_MISS and still returns the
 * base view.  A missing key returns BASE_MISS and a zeroed view. */
ecache_status ecache_get(expert_cache *cache, ecache_key key,
                         ecache_requirement requirement,
                         ecache_lookup_result *result,
                         ecache_view *view);

/* Inspection without recency or telemetry changes. */
ecache_status ecache_peek(const expert_cache *cache, ecache_key key,
                          ecache_view *view);

/* On success, ownership of payload transfers to the cache.  logical_bytes is
 * rounded up to payload_alignment for budget accounting; source_bytes_read is
 * the actual I/O amount and is used only for telemetry.  Failed admissions do
 * not release or take ownership of payload and do not mutate resident state. */
ecache_status ecache_insert_base(expert_cache *cache, ecache_key key,
                                 void *payload, uint64_t logical_bytes,
                                 uint64_t source_bytes_read,
                                 ecache_admission admission,
                                 ecache_view *view);

/* Adds only the residual plane to an existing base.  The base handle and its
 * recency identity remain unchanged, so promotion cannot reread the base. */
ecache_status ecache_promote(expert_cache *cache, ecache_key key,
                             void *residual_payload,
                             uint64_t logical_bytes,
                             uint64_t source_bytes_read,
                             ecache_admission admission,
                             ecache_view *view);

/* Explicit removal ignores fairness floors and releases residual before base. */
ecache_status ecache_remove(expert_cache *cache, ecache_key key);

/* Reclaim until payload_bytes <= target_bytes.  target must not exceed the
 * configured budget.  WARN is guaranteed not to release a base; CRITICAL
 * honors per-layer floors.  reclaimed_bytes receives alignment-charged bytes. */
ecache_status ecache_apply_pressure(expert_cache *cache,
                                    ecache_pressure pressure,
                                    uint64_t target_bytes,
                                    uint64_t *reclaimed_bytes);

void ecache_get_stats(const expert_cache *cache, ecache_stats *stats);
ecache_status ecache_get_layer_usage(const expert_cache *cache,
                                     uint32_t layer,
                                     ecache_layer_usage *usage);

/* O(n^2) diagnostic verifier intended for tests, debug builds, and integration
 * bring-up.  It checks keys, planes, hashes, floors, and every byte total. */
ecache_status ecache_validate(const expert_cache *cache);

#ifdef __cplusplus
}
#endif

#endif /* COLIBRI_EXPERT_CACHE_H */
