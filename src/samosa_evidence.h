#ifndef SAMOSA_EVIDENCE_H
#define SAMOSA_EVIDENCE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* These strings are persisted and cross the HTTP boundary. Change them only
 * with an explicit compatibility path for older attachment/evidence records. */
#define SAMOSA_SOURCE_SCHEMA "samosa.source.v1"
#define SAMOSA_SOURCE_CAPABILITIES_SCHEMA "samosa.source-capabilities.v1"
#define SAMOSA_EVIDENCE_PLAN_SCHEMA "samosa.evidence-plan.v1"
#define SAMOSA_EVIDENCE_SCHEMA "samosa.evidence.v1"

typedef enum {
    SAMOSA_SOURCE_UNKNOWN = 0,
    SAMOSA_SOURCE_TEXT,
    SAMOSA_SOURCE_DOCUMENT,
    SAMOSA_SOURCE_IMAGE,
    SAMOSA_SOURCE_AUDIO,
    SAMOSA_SOURCE_VIDEO
} SamosaSourceKind;

/* Operation values are stable bit positions. Evidence plans and provider
 * descriptors use these bits internally and the matching names on wire. */
typedef uint32_t SamosaEvidenceOperations;
enum {
    SAMOSA_EVIDENCE_EXTRACT_TEXT      = 1u << 0,
    SAMOSA_EVIDENCE_OCR_TEXT          = 1u << 1,
    SAMOSA_EVIDENCE_INSPECT_VISUAL    = 1u << 2,
    SAMOSA_EVIDENCE_ANALYZE_VIDEO     = 1u << 3,
    SAMOSA_EVIDENCE_TRANSCRIBE_AUDIO  = 1u << 4,
    SAMOSA_EVIDENCE_EXTRACT_SUBTITLES = 1u << 5,
    SAMOSA_EVIDENCE_INSPECT_METADATA  = 1u << 6,
    SAMOSA_EVIDENCE_RETRIEVE          = 1u << 7,
    SAMOSA_EVIDENCE_OPERATION_COUNT   = 8
};

typedef enum {
    SAMOSA_COVERAGE_EXPLICIT = 0,
    SAMOSA_COVERAGE_RELEVANCE_FIRST,
    SAMOSA_COVERAGE_OVERVIEW,
    SAMOSA_COVERAGE_EXHAUSTIVE
} SamosaEvidenceCoverage;

typedef enum {
    SAMOSA_QUALITY_FAST = 0,
    SAMOSA_QUALITY_BALANCED,
    SAMOSA_QUALITY_QUALITY
} SamosaEvidenceQuality;

typedef struct {
    char attachment_id[65];
    SamosaSourceKind kind;
    char media_type[128];
    char container[32];
    char codec[32];
    uint64_t bytes;
    double duration_seconds; /* 0 when the source has no probed timeline. */
    int page_count;          /* 0 until a document inventory has counted it. */
    int width;
    int height;
    SamosaEvidenceOperations available_operations;
} SamosaSourceInventory;

typedef struct {
    char attachment_id[65];
    SamosaEvidenceOperations operations;
    SamosaEvidenceCoverage coverage;
    SamosaEvidenceQuality quality;
    int page_start;
    int page_end;
    int has_time_range;
    double time_start;
    double time_end;
} SamosaEvidencePlanItem;

const char *samosa_source_kind_name(SamosaSourceKind kind);
int samosa_source_kind_parse(const char *name, SamosaSourceKind *kind);

const char *samosa_evidence_operation_name(SamosaEvidenceOperations operation);
int samosa_evidence_operation_parse(const char *name,
                                    SamosaEvidenceOperations *operation);

SamosaEvidenceOperations samosa_source_default_operations(
    SamosaSourceKind kind, const char *media_type);

const char *samosa_evidence_coverage_name(SamosaEvidenceCoverage coverage);
const char *samosa_evidence_quality_name(SamosaEvidenceQuality quality);

/* Build the first deterministic inventory from the gateway's byte-sniffed
 * attachment result. Rich media probing later fills duration/streams/codec
 * without changing this contract. Capability arguments are mutually
 * exclusive except that document sources may later also expose visual/OCR
 * operations through their media type. */
int samosa_source_inventory_from_attachment(
    SamosaSourceInventory *inventory, const char *attachment_id,
    const char *media_type, uint64_t bytes, int image_cap, int video_cap,
    int audio_cap, int document_cap);

int samosa_source_inventory_valid(const SamosaSourceInventory *inventory,
                                  char *error, size_t error_cap);

/* A safe fallback is used only when task planning is unavailable or invalid.
 * It selects the source's canonical evidence operation; task-aware planning
 * may add operations but still has to pass the validator below. */
int samosa_evidence_plan_safe_fallback(const SamosaSourceInventory *inventory,
                                       SamosaEvidencePlanItem *item);

int samosa_evidence_plan_item_valid(const SamosaSourceInventory *inventory,
                                    const SamosaEvidencePlanItem *item,
                                    char *error, size_t error_cap);

#ifdef __cplusplus
}
#endif

#endif
