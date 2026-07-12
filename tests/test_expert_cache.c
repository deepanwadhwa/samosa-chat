#include "expert_cache.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            return 1;                                                          \
        }                                                                      \
    } while (0)

#define CHECK_STATUS(expression, expected)                                    \
    do {                                                                       \
        ecache_status got_ = (expression);                                     \
        if (got_ != (expected)) {                                              \
            fprintf(stderr, "%s:%d: %s returned %s, expected %s\n",          \
                    __FILE__, __LINE__, #expression,                           \
                    ecache_status_string(got_),                                \
                    ecache_status_string(expected));                           \
            return 1;                                                          \
        }                                                                      \
    } while (0)

typedef struct model_cell {
    uint8_t state;
    void *base;
    void *residual;
} model_cell;

typedef struct release_log {
    ecache_release_event events[128];
    size_t event_count;
    uint64_t released_charge;
    uint64_t release_calls;
    model_cell (*model)[64];
    uint32_t model_layers;
    int callback_errors;
} release_log;

typedef struct fixture {
    void *workspace;
    size_t workspace_bytes;
    expert_cache *cache;
    release_log releases;
    unsigned char *handles;
    size_t handle_capacity;
    size_t next_handle;
    uint64_t admitted_charge;
} fixture;

static void release_callback(void *context,
                             const ecache_release_event *event) {
    release_log *log = (release_log *)context;
    if (!event || !event->payload || !event->logical_bytes ||
        !event->charged_bytes) {
        ++log->callback_errors;
        return;
    }
    if (log->event_count < ARRAY_COUNT(log->events))
        log->events[log->event_count] = *event;
    ++log->event_count;
    ++log->release_calls;
    log->released_charge += event->charged_bytes;

    if (log->model && event->key.layer < log->model_layers &&
        event->key.expert < 64u) {
        model_cell *cell = &log->model[event->key.layer][event->key.expert];
        if (event->plane == ECACHE_PLANE_RESIDUAL) {
            if (cell->state != ECACHE_BASE_AND_RESIDUAL ||
                cell->residual != event->payload)
                ++log->callback_errors;
            cell->state = ECACHE_BASE_ONLY;
            cell->residual = NULL;
        } else if (event->plane == ECACHE_PLANE_BASE) {
            if (cell->state != ECACHE_BASE_ONLY ||
                cell->base != event->payload)
                ++log->callback_errors;
            memset(cell, 0, sizeof(*cell));
        } else {
            ++log->callback_errors;
        }
    }
}

static int fixture_init(fixture *f, const ecache_config *config,
                        const ecache_layer_floor *floors,
                        size_t handle_capacity) {
    ecache_callbacks callbacks;
    ecache_status status;
    memset(f, 0, sizeof(*f));
    status = ecache_workspace_size(config, &f->workspace_bytes);
    if (status != ECACHE_OK) {
        fprintf(stderr, "workspace size: %s\n", ecache_status_string(status));
        return 0;
    }
    f->workspace = calloc(1, f->workspace_bytes);
    f->handles = (unsigned char *)malloc(handle_capacity ? handle_capacity : 1);
    f->handle_capacity = handle_capacity;
    if (!f->workspace || !f->handles) return 0;
    callbacks.release = release_callback;
    callbacks.context = &f->releases;
    status = ecache_init(f->workspace, f->workspace_bytes, config, floors,
                         &callbacks, &f->cache);
    if (status != ECACHE_OK) {
        fprintf(stderr, "cache init: %s\n", ecache_status_string(status));
        return 0;
    }
    return 1;
}

static void fixture_free(fixture *f) {
    free(f->handles);
    free(f->workspace);
    memset(f, 0, sizeof(*f));
}

static void *new_handle(fixture *f) {
    if (f->next_handle >= f->handle_capacity) return NULL;
    return &f->handles[f->next_handle++];
}

static ecache_status insert_base(fixture *f, ecache_key key,
                                 uint64_t logical, uint64_t source_read,
                                 ecache_admission admission,
                                 void **payload_out) {
    ecache_view view;
    void *payload = new_handle(f);
    ecache_status status;
    if (!payload) return ECACHE_ERR_SIZE;
    status = ecache_insert_base(f->cache, key, payload, logical, source_read,
                                admission, &view);
    if (status == ECACHE_OK) {
        f->admitted_charge += view.base_charged_bytes;
        if (payload_out) *payload_out = payload;
    }
    return status;
}

static ecache_status promote(fixture *f, ecache_key key, uint64_t logical,
                             uint64_t source_read,
                             ecache_admission admission,
                             void **payload_out) {
    ecache_view view;
    void *payload = new_handle(f);
    ecache_status status;
    if (!payload) return ECACHE_ERR_SIZE;
    status = ecache_promote(f->cache, key, payload, logical, source_read,
                            admission, &view);
    if (status == ECACHE_OK) {
        f->admitted_charge += view.residual_charged_bytes;
        if (payload_out) *payload_out = payload;
    }
    return status;
}

static int check_conservation(const fixture *f) {
    ecache_stats stats;
    ecache_get_stats(f->cache, &stats);
    if (f->releases.callback_errors ||
        f->admitted_charge < f->releases.released_charge ||
        f->admitted_charge - f->releases.released_charge !=
            stats.payload_bytes) {
        fprintf(stderr,
                "ownership mismatch admitted=%" PRIu64 " released=%" PRIu64
                " resident=%" PRIu64 " callback-errors=%d\n",
                f->admitted_charge, f->releases.released_charge,
                stats.payload_bytes, f->releases.callback_errors);
        return 0;
    }
    return 1;
}

static int destroy_and_check(fixture *f) {
    CHECK(check_conservation(f));
    CHECK_STATUS(ecache_destroy(f->cache), ECACHE_OK);
    CHECK(f->releases.callback_errors == 0);
    CHECK(f->admitted_charge == f->releases.released_charge);
    return 0;
}

static int test_basic_accounting_and_promotion(void) {
    const ecache_config config = {512, 64, 4, 2, ECACHE_POLICY_LRU};
    fixture f;
    ecache_key key = {1, 7};
    ecache_lookup_result result;
    ecache_view view;
    ecache_stats stats;
    void *base, *residual;
    CHECK(fixture_init(&f, &config, NULL, 16));

    CHECK_STATUS(ecache_get(f.cache, key, ECACHE_REQUIRE_FULL, &result, &view),
                 ECACHE_OK);
    CHECK(result == ECACHE_LOOKUP_BASE_MISS);
    CHECK(view.base_payload == NULL);
    CHECK_STATUS(insert_base(&f, key, 65, 70, ECACHE_ADMIT_DEMAND, &base),
                 ECACHE_OK);
    CHECK_STATUS(ecache_peek(f.cache, key, &view), ECACHE_OK);
    CHECK(view.state == ECACHE_BASE_ONLY && view.base_payload == base);
    CHECK(view.base_logical_bytes == 65 && view.base_charged_bytes == 128);

    CHECK_STATUS(promote(&f, key, 1, 5, ECACHE_ADMIT_DEMAND, &residual),
                 ECACHE_OK);
    CHECK_STATUS(ecache_peek(f.cache, key, &view), ECACHE_OK);
    CHECK(view.state == ECACHE_BASE_AND_RESIDUAL);
    CHECK(view.base_payload == base && view.residual_payload == residual);
    CHECK(view.residual_charged_bytes == 64);

    CHECK_STATUS(ecache_get(f.cache, key, ECACHE_REQUIRE_FULL, &result, &view),
                 ECACHE_OK);
    CHECK(result == ECACHE_LOOKUP_HIT);
    CHECK_STATUS(ecache_get(f.cache, key, ECACHE_REQUIRE_BASE, &result, &view),
                 ECACHE_OK);
    CHECK(result == ECACHE_LOOKUP_HIT);
    ecache_get_stats(f.cache, &stats);
    CHECK(stats.metadata_bytes == f.workspace_bytes);
    CHECK(stats.payload_bytes == 192 && stats.base_bytes == 128);
    CHECK(stats.residual_bytes == 64 && stats.peak_payload_bytes == 192);
    CHECK(stats.base_hits == 2 && stats.base_misses == 1);
    CHECK(stats.residual_hits == 1 && stats.residual_misses == 1);
    CHECK(stats.base_bytes_read == 70 && stats.residual_bytes_read == 5);
    CHECK(stats.base_bytes_avoided == 140);
    CHECK(stats.residual_bytes_avoided == 5);
    CHECK(stats.promotions == 1);
    CHECK_STATUS(ecache_validate(f.cache), ECACHE_OK);

    f.releases.event_count = 0;
    CHECK_STATUS(ecache_remove(f.cache, key), ECACHE_OK);
    CHECK(f.releases.event_count == 2);
    CHECK(f.releases.events[0].plane == ECACHE_PLANE_RESIDUAL);
    CHECK(f.releases.events[1].plane == ECACHE_PLANE_BASE);
    CHECK(f.releases.events[0].reason == ECACHE_RELEASE_EXPLICIT);
    CHECK(f.releases.events[1].reason == ECACHE_RELEASE_EXPLICIT);
    ecache_get_stats(f.cache, &stats);
    CHECK(stats.demotions == 0 && stats.evictions == 0);
    CHECK(stats.payload_bytes == 0 && stats.entries == 0);
    CHECK_STATUS(ecache_validate(f.cache), ECACHE_OK);
    CHECK(destroy_and_check(&f) == 0);
    fixture_free(&f);
    return 0;
}

static int test_residual_first_and_fairness(void) {
    const ecache_config config = {256, 64, 4, 2, ECACHE_POLICY_LRU};
    const ecache_layer_floor floors[2] = {{0, 1}, {0, 1}};
    const ecache_key a = {0, 0}, b = {0, 1}, c = {1, 0};
    const ecache_key d = {1, 1}, e = {1, 2};
    fixture f;
    ecache_stats stats;
    ecache_layer_usage usage;
    CHECK(fixture_init(&f, &config, floors, 32));
    CHECK_STATUS(insert_base(&f, a, 10, 10, ECACHE_ADMIT_DEMAND, NULL),
                 ECACHE_OK);
    CHECK_STATUS(insert_base(&f, b, 10, 10, ECACHE_ADMIT_DEMAND, NULL),
                 ECACHE_OK);
    CHECK_STATUS(insert_base(&f, c, 10, 10, ECACHE_ADMIT_DEMAND, NULL),
                 ECACHE_OK);
    CHECK_STATUS(promote(&f, a, 10, 10, ECACHE_ADMIT_DEMAND, NULL),
                 ECACHE_OK);

    f.releases.event_count = 0;
    CHECK_STATUS(insert_base(&f, d, 10, 10, ECACHE_ADMIT_DEMAND, NULL),
                 ECACHE_OK);
    CHECK(f.releases.event_count == 1);
    CHECK(f.releases.events[0].plane == ECACHE_PLANE_RESIDUAL);
    CHECK(f.releases.events[0].key.layer == a.layer &&
          f.releases.events[0].key.expert == a.expert);
    CHECK(f.releases.events[0].reason == ECACHE_RELEASE_DEMOTION);
    CHECK_STATUS(ecache_peek(f.cache, a, &(ecache_view){0}), ECACHE_OK);

    f.releases.event_count = 0;
    CHECK_STATUS(insert_base(&f, e, 10, 10, ECACHE_ADMIT_DEMAND, NULL),
                 ECACHE_OK);
    CHECK(f.releases.event_count == 1);
    CHECK(f.releases.events[0].plane == ECACHE_PLANE_BASE);
    CHECK(f.releases.events[0].key.layer == 0);
    CHECK_STATUS(ecache_get_layer_usage(f.cache, 0, &usage), ECACHE_OK);
    CHECK(usage.base_entries == 1);
    CHECK_STATUS(ecache_get_layer_usage(f.cache, 1, &usage), ECACHE_OK);
    CHECK(usage.base_entries == 3);
    ecache_get_stats(f.cache, &stats);
    CHECK(stats.demotions == 1 && stats.evictions == 1);
    CHECK(stats.payload_bytes == config.budget_bytes);
    CHECK_STATUS(ecache_validate(f.cache), ECACHE_OK);
    CHECK(destroy_and_check(&f) == 0);
    fixture_free(&f);
    return 0;
}

static int test_failed_admission_is_transactional(void) {
    const ecache_config config = {128, 64, 2, 2, ECACHE_POLICY_LRU};
    const ecache_layer_floor floors[2] = {{0, 1}, {0, 1}};
    fixture f;
    ecache_stats before, after;
    uint64_t releases;
    CHECK(fixture_init(&f, &config, floors, 8));
    CHECK_STATUS(insert_base(&f, (ecache_key){0, 0}, 1, 1,
                             ECACHE_ADMIT_DEMAND, NULL), ECACHE_OK);
    CHECK_STATUS(insert_base(&f, (ecache_key){1, 0}, 1, 1,
                             ECACHE_ADMIT_DEMAND, NULL), ECACHE_OK);
    ecache_get_stats(f.cache, &before);
    releases = f.releases.release_calls;
    CHECK_STATUS(insert_base(&f, (ecache_key){0, 1}, 1, 1,
                             ECACHE_ADMIT_DEMAND, NULL),
                 ECACHE_ERR_NO_SPACE);
    ecache_get_stats(f.cache, &after);
    CHECK(after.payload_bytes == before.payload_bytes);
    CHECK(after.entries == before.entries);
    CHECK(after.failed_admissions == before.failed_admissions + 1);
    CHECK(f.releases.release_calls == releases);
    CHECK_STATUS(promote(&f, (ecache_key){0, 0}, 1, 1,
                         ECACHE_ADMIT_DEMAND, NULL),
                 ECACHE_ERR_NO_SPACE);
    ecache_get_stats(f.cache, &after);
    CHECK(after.payload_bytes == before.payload_bytes);
    CHECK(after.entries == before.entries);
    CHECK(after.failed_admissions == before.failed_admissions + 2);
    CHECK(f.releases.release_calls == releases);
    CHECK_STATUS(ecache_peek(f.cache, (ecache_key){0, 0}, &(ecache_view){0}),
                 ECACHE_OK);
    CHECK_STATUS(ecache_peek(f.cache, (ecache_key){1, 0}, &(ecache_view){0}),
                 ECACHE_OK);
    CHECK_STATUS(ecache_validate(f.cache), ECACHE_OK);
    CHECK(destroy_and_check(&f) == 0);
    fixture_free(&f);
    return 0;
}

static int test_byte_floor(void) {
    const ecache_config config = {192, 64, 3, 1, ECACHE_POLICY_LRU};
    const ecache_layer_floor floor = {100, 0};
    fixture f;
    uint64_t reclaimed;
    ecache_stats stats;
    CHECK(fixture_init(&f, &config, &floor, 8));
    /* Oldest 128-byte entry cannot leave because 64 would remain.  The newer
     * 64-byte entry can leave because 128 then remains above the byte floor. */
    CHECK_STATUS(insert_base(&f, (ecache_key){0, 0}, 65, 65,
                             ECACHE_ADMIT_DEMAND, NULL), ECACHE_OK);
    CHECK_STATUS(insert_base(&f, (ecache_key){0, 1}, 1, 1,
                             ECACHE_ADMIT_DEMAND, NULL), ECACHE_OK);
    f.releases.event_count = 0;
    CHECK_STATUS(ecache_apply_pressure(f.cache, ECACHE_PRESSURE_CRITICAL, 64,
                                       &reclaimed), ECACHE_PARTIAL);
    CHECK(reclaimed == 64 && f.releases.event_count == 1);
    CHECK(f.releases.events[0].key.expert == 1);
    ecache_get_stats(f.cache, &stats);
    CHECK(stats.payload_bytes == 128);
    CHECK_STATUS(ecache_validate(f.cache), ECACHE_OK);
    CHECK(destroy_and_check(&f) == 0);
    fixture_free(&f);
    return 0;
}

static int test_pressure_order_and_warn_guarantee(void) {
    const ecache_config config = {256, 64, 4, 1, ECACHE_POLICY_LRU};
    const ecache_layer_floor floor = {0, 1};
    fixture f;
    ecache_stats stats;
    uint64_t reclaimed;
    CHECK(fixture_init(&f, &config, &floor, 16));
    CHECK_STATUS(insert_base(&f, (ecache_key){0, 0}, 1, 1,
                             ECACHE_ADMIT_DEMAND, NULL), ECACHE_OK);
    CHECK_STATUS(promote(&f, (ecache_key){0, 0}, 1, 1,
                         ECACHE_ADMIT_DEMAND, NULL), ECACHE_OK);
    CHECK_STATUS(insert_base(&f, (ecache_key){0, 1}, 1, 1,
                             ECACHE_ADMIT_DEMAND, NULL), ECACHE_OK);
    CHECK_STATUS(promote(&f, (ecache_key){0, 1}, 1, 1,
                         ECACHE_ADMIT_DEMAND, NULL), ECACHE_OK);

    f.releases.event_count = 0;
    CHECK_STATUS(ecache_apply_pressure(f.cache, ECACHE_PRESSURE_WARN, 64,
                                       &reclaimed), ECACHE_PARTIAL);
    CHECK(reclaimed == 128 && f.releases.event_count == 2);
    CHECK(f.releases.events[0].plane == ECACHE_PLANE_RESIDUAL &&
          f.releases.events[1].plane == ECACHE_PLANE_RESIDUAL);
    ecache_get_stats(f.cache, &stats);
    CHECK(stats.payload_bytes == 128 && stats.entries == 2);
    CHECK(stats.pressure_warn_events == 1 && stats.demotions == 2);

    /* Rebuild residuals and prove CRITICAL also releases every needed
     * residual before its first base plane. */
    CHECK_STATUS(promote(&f, (ecache_key){0, 0}, 1, 1,
                         ECACHE_ADMIT_DEMAND, NULL), ECACHE_OK);
    CHECK_STATUS(promote(&f, (ecache_key){0, 1}, 1, 1,
                         ECACHE_ADMIT_DEMAND, NULL), ECACHE_OK);
    f.releases.event_count = 0;
    CHECK_STATUS(ecache_apply_pressure(f.cache, ECACHE_PRESSURE_CRITICAL, 64,
                                       &reclaimed), ECACHE_OK);
    CHECK(reclaimed == 192 && f.releases.event_count == 3);
    CHECK(f.releases.events[0].plane == ECACHE_PLANE_RESIDUAL);
    CHECK(f.releases.events[1].plane == ECACHE_PLANE_RESIDUAL);
    CHECK(f.releases.events[2].plane == ECACHE_PLANE_BASE);
    ecache_get_stats(f.cache, &stats);
    CHECK(stats.payload_bytes == 64 && stats.entries == 1);
    CHECK(stats.pressure_critical_events == 1);
    CHECK_STATUS(ecache_validate(f.cache), ECACHE_OK);
    CHECK(destroy_and_check(&f) == 0);
    fixture_free(&f);
    return 0;
}

static int test_2q_protects_reused_entries(void) {
    const ecache_config config = {192, 64, 3, 1, ECACHE_POLICY_2Q};
    fixture f;
    ecache_lookup_result result;
    ecache_view view;
    CHECK(fixture_init(&f, &config, NULL, 16));
    CHECK_STATUS(insert_base(&f, (ecache_key){0, 0}, 1, 1,
                             ECACHE_ADMIT_DEMAND, NULL), ECACHE_OK);
    CHECK_STATUS(insert_base(&f, (ecache_key){0, 1}, 1, 1,
                             ECACHE_ADMIT_DEMAND, NULL), ECACHE_OK);
    CHECK_STATUS(insert_base(&f, (ecache_key){0, 2}, 1, 1,
                             ECACHE_ADMIT_DEMAND, NULL), ECACHE_OK);
    CHECK_STATUS(ecache_get(f.cache, (ecache_key){0, 0}, ECACHE_REQUIRE_BASE,
                            &result, &view), ECACHE_OK);
    CHECK(view.hot_queue && result == ECACHE_LOOKUP_HIT);
    CHECK_STATUS(ecache_get(f.cache, (ecache_key){0, 1}, ECACHE_REQUIRE_BASE,
                            &result, &view), ECACHE_OK);
    CHECK(view.hot_queue);

    f.releases.event_count = 0;
    CHECK_STATUS(insert_base(&f, (ecache_key){0, 3}, 1, 1,
                             ECACHE_ADMIT_DEMAND, NULL), ECACHE_OK);
    CHECK(f.releases.event_count == 1);
    CHECK(f.releases.events[0].key.expert == 2); /* only cold candidate */
    CHECK_STATUS(ecache_peek(f.cache, (ecache_key){0, 0}, &view), ECACHE_OK);
    CHECK(view.hot_queue);
    CHECK_STATUS(ecache_peek(f.cache, (ecache_key){0, 1}, &view), ECACHE_OK);
    CHECK(view.hot_queue);
    CHECK_STATUS(ecache_validate(f.cache), ECACHE_OK);
    CHECK(destroy_and_check(&f) == 0);
    fixture_free(&f);
    return 0;
}

static int test_prefetch_waste_and_io_telemetry(void) {
    const ecache_config config = {128, 64, 2, 1, ECACHE_POLICY_LRU};
    fixture f;
    ecache_lookup_result result;
    ecache_view view;
    ecache_stats stats;
    uint64_t reclaimed;
    CHECK(fixture_init(&f, &config, NULL, 16));
    CHECK_STATUS(insert_base(&f, (ecache_key){0, 0}, 7, 101,
                             ECACHE_ADMIT_PREFETCH, NULL), ECACHE_OK);
    CHECK_STATUS(promote(&f, (ecache_key){0, 0}, 5, 50,
                         ECACHE_ADMIT_PREFETCH, NULL), ECACHE_OK);
    CHECK_STATUS(ecache_get(f.cache, (ecache_key){0, 0}, ECACHE_REQUIRE_BASE,
                            &result, &view), ECACHE_OK);
    /* Base was demanded, residual was not. */
    CHECK_STATUS(ecache_apply_pressure(f.cache, ECACHE_PRESSURE_WARN, 64,
                                       &reclaimed), ECACHE_OK);
    CHECK(reclaimed == 64);
    CHECK_STATUS(ecache_remove(f.cache, (ecache_key){0, 0}), ECACHE_OK);
    CHECK_STATUS(insert_base(&f, (ecache_key){0, 1}, 3, 77,
                             ECACHE_ADMIT_PREFETCH, NULL), ECACHE_OK);
    CHECK_STATUS(ecache_remove(f.cache, (ecache_key){0, 1}), ECACHE_OK);
    ecache_get_stats(f.cache, &stats);
    CHECK(stats.base_bytes_read == 178 && stats.residual_bytes_read == 50);
    CHECK(stats.wasted_prefetch_planes == 2);
    CHECK(stats.wasted_prefetch_bytes == 127);
    CHECK_STATUS(ecache_validate(f.cache), ECACHE_OK);
    CHECK(destroy_and_check(&f) == 0);
    fixture_free(&f);
    return 0;
}

static int test_malformed_and_overflow_inputs(void) {
    ecache_config config = {128, 64, 2, 1, ECACHE_POLICY_LRU};
    ecache_callbacks callbacks = {release_callback, NULL};
    release_log log;
    expert_cache *cache = NULL;
    size_t bytes = 123;
    void *workspace;
    unsigned char *unaligned;
    fixture f;
    ecache_stats stats;
    ecache_view view;
    memset(&log, 0, sizeof(log));
    callbacks.context = &log;

    CHECK_STATUS(ecache_workspace_size(NULL, &bytes), ECACHE_ERR_ARGUMENT);
    config.payload_alignment = 0;
    CHECK_STATUS(ecache_workspace_size(&config, &bytes), ECACHE_ERR_SIZE);
    config.payload_alignment = 64;
    config.max_entries = 0;
    CHECK_STATUS(ecache_workspace_size(&config, &bytes), ECACHE_ERR_SIZE);
    config.max_entries = 2;
    config.policy = (ecache_policy)99;
    CHECK_STATUS(ecache_workspace_size(&config, &bytes), ECACHE_ERR_POLICY);
    config.policy = ECACHE_POLICY_LRU;
    CHECK_STATUS(ecache_workspace_size(&config, &bytes), ECACHE_OK);
    workspace = malloc(bytes);
    unaligned = (unsigned char *)malloc(bytes + ecache_workspace_alignment());
    CHECK(workspace && unaligned);
    CHECK_STATUS(ecache_init((void *)(unaligned + 1), bytes, &config, NULL,
                             &callbacks, &cache), ECACHE_ERR_ALIGNMENT);
    CHECK_STATUS(ecache_init(workspace, bytes - 1, &config, NULL, &callbacks,
                             &cache), ECACHE_ERR_SIZE);
    CHECK_STATUS(ecache_init(workspace, bytes, &config, NULL, &callbacks,
                             &cache), ECACHE_OK);
    CHECK_STATUS(ecache_insert_base(cache, (ecache_key){1, 0}, workspace, 1, 1,
                                    ECACHE_ADMIT_DEMAND, NULL),
                 ECACHE_ERR_ARGUMENT);
    CHECK_STATUS(ecache_insert_base(cache, (ecache_key){0, 0}, workspace, 0, 1,
                                    ECACHE_ADMIT_DEMAND, NULL), ECACHE_ERR_SIZE);
    CHECK_STATUS(ecache_insert_base(cache, (ecache_key){0, 0}, workspace,
                                    UINT64_MAX, 1, ECACHE_ADMIT_DEMAND, NULL),
                 ECACHE_ERR_OVERFLOW);
    CHECK_STATUS(ecache_get(cache, (ecache_key){0, 0},
                            (ecache_requirement)99,
                            &(ecache_lookup_result){0}, &view),
                 ECACHE_ERR_ARGUMENT);
    CHECK_STATUS(ecache_apply_pressure(cache, ECACHE_PRESSURE_WARN, 129,
                                       &(uint64_t){0}), ECACHE_ERR_SIZE);
    CHECK_STATUS(ecache_validate(cache), ECACHE_OK);
    CHECK_STATUS(ecache_destroy(cache), ECACHE_OK);
    free(unaligned);
    free(workspace);

    /* Duplicate handles are rejected, and source-byte counters saturate. */
    config.budget_bytes = 256;
    CHECK(fixture_init(&f, &config, NULL, 8));
    {
        void *payload;
        CHECK_STATUS(insert_base(&f, (ecache_key){0, 0}, 1, UINT64_MAX,
                                 ECACHE_ADMIT_DEMAND, &payload), ECACHE_OK);
        CHECK_STATUS(ecache_insert_base(f.cache, (ecache_key){0, 1}, payload,
                                        1, 1, ECACHE_ADMIT_DEMAND, NULL),
                     ECACHE_ERR_EXISTS);
        CHECK_STATUS(insert_base(&f, (ecache_key){0, 1}, 1, 1,
                                 ECACHE_ADMIT_DEMAND, NULL), ECACHE_OK);
    }
    ecache_get_stats(f.cache, &stats);
    CHECK(stats.base_bytes_read == UINT64_MAX);
    CHECK_STATUS(ecache_validate(f.cache), ECACHE_OK);
    CHECK(destroy_and_check(&f) == 0);
    fixture_free(&f);
    return 0;
}

static uint32_t prng_next(uint64_t *state) {
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return (uint32_t)((x * UINT64_C(2685821657736338717)) >> 32);
}

static int check_random_model(fixture *f, model_cell model[8][64]) {
    uint32_t layer, expert;
    for (layer = 0; layer < 8; ++layer) {
        for (expert = 0; expert < 64; ++expert) {
            ecache_view view;
            ecache_status status =
                ecache_peek(f->cache, (ecache_key){layer, expert}, &view);
            model_cell *cell = &model[layer][expert];
            if (!cell->state) {
                if (status != ECACHE_ERR_NOT_FOUND) return 0;
            } else {
                if (status != ECACHE_OK || view.state != cell->state ||
                    view.base_payload != cell->base ||
                    view.residual_payload != cell->residual)
                    return 0;
            }
        }
    }
    return 1;
}

static int test_randomized_state_machine(void) {
    const ecache_config config = {4096, 64, 32, 8, ECACHE_POLICY_2Q};
    ecache_layer_floor floors[8];
    model_cell model[8][64];
    fixture f;
    uint64_t rng = UINT64_C(0xd1b54a32d192ed03);
    uint32_t iteration;
    memset(floors, 0, sizeof(floors));
    memset(model, 0, sizeof(model));
    for (iteration = 0; iteration < 8; ++iteration)
        floors[iteration].min_base_entries = 1;
    CHECK(fixture_init(&f, &config, floors, 70000));
    f.releases.model = model;
    f.releases.model_layers = 8;

    for (iteration = 0; iteration < 50000; ++iteration) {
        uint32_t random = prng_next(&rng);
        uint32_t layer = random & 7u;
        uint32_t expert = (random >> 3) & 63u;
        uint32_t operation = (random >> 9) % 8u;
        ecache_key key = {layer, expert};
        model_cell *cell = &model[layer][expert];
        ecache_status status;
        ecache_view view;

        if (operation == 0 || operation == 1) {
            ecache_lookup_result result;
            status = ecache_get(f.cache, key,
                                operation ? ECACHE_REQUIRE_FULL
                                          : ECACHE_REQUIRE_BASE,
                                &result, &view);
            CHECK(status == ECACHE_OK);
            if (!cell->state)
                CHECK(result == ECACHE_LOOKUP_BASE_MISS);
            else if (operation && cell->state == ECACHE_BASE_ONLY)
                CHECK(result == ECACHE_LOOKUP_RESIDUAL_MISS);
            else
                CHECK(result == ECACHE_LOOKUP_HIT);
        } else if (operation == 2 && !cell->state) {
            void *payload;
            status = insert_base(&f, key, (random % 191u) + 1u,
                                 (random % 223u) + 1u,
                                 (random & 0x10000u) ? ECACHE_ADMIT_PREFETCH
                                                    : ECACHE_ADMIT_DEMAND,
                                 &payload);
            CHECK(status == ECACHE_OK || status == ECACHE_ERR_NO_SPACE);
            if (status == ECACHE_OK) {
                cell->state = ECACHE_BASE_ONLY;
                cell->base = payload;
            }
        } else if (operation == 3 && cell->state == ECACHE_BASE_ONLY) {
            void *payload;
            status = promote(&f, key, (random % 127u) + 1u,
                             (random % 149u) + 1u,
                             (random & 0x20000u) ? ECACHE_ADMIT_PREFETCH
                                                : ECACHE_ADMIT_DEMAND,
                             &payload);
            CHECK(status == ECACHE_OK || status == ECACHE_ERR_NO_SPACE);
            if (status == ECACHE_OK) {
                cell->state = ECACHE_BASE_AND_RESIDUAL;
                cell->residual = payload;
            }
        } else if (operation == 4 && cell->state) {
            CHECK_STATUS(ecache_remove(f.cache, key), ECACHE_OK);
        } else if (operation == 5) {
            uint64_t reclaimed;
            uint64_t target = (uint64_t)(random % (config.budget_bytes + 1u));
            status = ecache_apply_pressure(f.cache, ECACHE_PRESSURE_WARN,
                                           target, &reclaimed);
            CHECK(status == ECACHE_OK || status == ECACHE_PARTIAL);
        } else if (operation == 6) {
            uint64_t reclaimed;
            uint64_t target = (uint64_t)(random % (config.budget_bytes + 1u));
            status = ecache_apply_pressure(f.cache, ECACHE_PRESSURE_CRITICAL,
                                           target, &reclaimed);
            CHECK(status == ECACHE_OK || status == ECACHE_PARTIAL);
        } else {
            /* Valid no-op inspection path, including absent keys. */
            status = ecache_peek(f.cache, key, &view);
            CHECK(status == (cell->state ? ECACHE_OK : ECACHE_ERR_NOT_FOUND));
        }

        CHECK(f.releases.callback_errors == 0);
        CHECK_STATUS(ecache_validate(f.cache), ECACHE_OK);
        CHECK(check_conservation(&f));
        if ((iteration & 255u) == 0) CHECK(check_random_model(&f, model));
    }
    CHECK(check_random_model(&f, model));
    CHECK(destroy_and_check(&f) == 0);
    fixture_free(&f);
    return 0;
}

/* Enumerate all length-five traces over seven operation classes.  This is a
 * bounded exhaustive transition test, separate from the longer random walk. */
static int test_exhaustive_short_traces(void) {
    const ecache_config config = {192, 64, 3, 2, ECACHE_POLICY_LRU};
    const uint32_t operation_count = 7;
    const uint32_t depth = 5;
    uint32_t traces = 1, trace;
    uint32_t i;
    for (i = 0; i < depth; ++i) traces *= operation_count;

    for (trace = 0; trace < traces; ++trace) {
        fixture f;
        uint32_t code = trace;
        CHECK(fixture_init(&f, &config, NULL, 32));
        for (i = 0; i < depth; ++i) {
            uint32_t operation = code % operation_count;
            ecache_key key = {(code >> 3) & 1u, (code >> 4) & 1u};
            ecache_view view;
            ecache_status present = ecache_peek(f.cache, key, &view);
            code /= operation_count;
            if (operation == 0) {
                ecache_lookup_result result;
                CHECK_STATUS(ecache_get(f.cache, key, ECACHE_REQUIRE_BASE,
                                        &result, &view), ECACHE_OK);
            } else if (operation == 1) {
                ecache_lookup_result result;
                CHECK_STATUS(ecache_get(f.cache, key, ECACHE_REQUIRE_FULL,
                                        &result, &view), ECACHE_OK);
            } else if (operation == 2 && present == ECACHE_ERR_NOT_FOUND) {
                ecache_status status = insert_base(
                    &f, key, i + 1u, i + 3u,
                    (i & 1u) ? ECACHE_ADMIT_PREFETCH : ECACHE_ADMIT_DEMAND,
                    NULL);
                CHECK(status == ECACHE_OK || status == ECACHE_ERR_NO_SPACE);
            } else if (operation == 3 && present == ECACHE_OK &&
                       view.state == ECACHE_BASE_ONLY) {
                ecache_status status = promote(
                    &f, key, i + 1u, i + 2u,
                    (i & 1u) ? ECACHE_ADMIT_PREFETCH : ECACHE_ADMIT_DEMAND,
                    NULL);
                CHECK(status == ECACHE_OK || status == ECACHE_ERR_NO_SPACE);
            } else if (operation == 4 && present == ECACHE_OK) {
                CHECK_STATUS(ecache_remove(f.cache, key), ECACHE_OK);
            } else if (operation == 5) {
                uint64_t reclaimed;
                ecache_status status = ecache_apply_pressure(
                    f.cache, ECACHE_PRESSURE_WARN, 64, &reclaimed);
                CHECK(status == ECACHE_OK || status == ECACHE_PARTIAL);
            } else if (operation == 6) {
                uint64_t reclaimed;
                ecache_status status = ecache_apply_pressure(
                    f.cache, ECACHE_PRESSURE_CRITICAL, 64, &reclaimed);
                CHECK(status == ECACHE_OK || status == ECACHE_PARTIAL);
            }
            CHECK_STATUS(ecache_validate(f.cache), ECACHE_OK);
            CHECK(check_conservation(&f));
        }
        CHECK(destroy_and_check(&f) == 0);
        fixture_free(&f);
    }
    return 0;
}

int main(void) {
    CHECK(test_basic_accounting_and_promotion() == 0);
    CHECK(test_residual_first_and_fairness() == 0);
    CHECK(test_failed_admission_is_transactional() == 0);
    CHECK(test_byte_floor() == 0);
    CHECK(test_pressure_order_and_warn_guarantee() == 0);
    CHECK(test_2q_protects_reused_entries() == 0);
    CHECK(test_prefetch_waste_and_io_telemetry() == 0);
    CHECK(test_malformed_and_overflow_inputs() == 0);
    CHECK(test_exhaustive_short_traces() == 0);
    CHECK(test_randomized_state_machine() == 0);
    puts("expert cache tests: ok (16,807 exhaustive traces + 50,000 random transitions)");
    return 0;
}
