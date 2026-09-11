#include "samosa_evidence.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    SamosaEvidenceOperations bit;
    const char *name;
} OperationName;

static const OperationName operation_names[] = {
    {SAMOSA_EVIDENCE_EXTRACT_TEXT, "extract_text"},
    {SAMOSA_EVIDENCE_OCR_TEXT, "ocr_text"},
    {SAMOSA_EVIDENCE_INSPECT_VISUAL, "inspect_visual"},
    {SAMOSA_EVIDENCE_ANALYZE_VIDEO, "analyze_video"},
    {SAMOSA_EVIDENCE_TRANSCRIBE_AUDIO, "transcribe_audio"},
    {SAMOSA_EVIDENCE_EXTRACT_SUBTITLES, "extract_subtitles"},
    {SAMOSA_EVIDENCE_INSPECT_METADATA, "inspect_metadata"},
    {SAMOSA_EVIDENCE_RETRIEVE, "retrieve_evidence"}
};

static void evidence_error(char *out, size_t cap, const char *value) {
    if (!out || !cap) return;
    snprintf(out, cap, "%s", value ? value : "evidence_contract_invalid");
}

static int attachment_id_valid(const char *id) {
    if (!id || strlen(id) != 64) return 0;
    for (size_t i = 0; i < 64; ++i) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

static void copy_text(char *out, size_t cap, const char *value) {
    if (!out || !cap) return;
    snprintf(out, cap, "%s", value ? value : "");
}

const char *samosa_source_kind_name(SamosaSourceKind kind) {
    switch (kind) {
    case SAMOSA_SOURCE_TEXT: return "text";
    case SAMOSA_SOURCE_DOCUMENT: return "document";
    case SAMOSA_SOURCE_IMAGE: return "image";
    case SAMOSA_SOURCE_AUDIO: return "audio";
    case SAMOSA_SOURCE_VIDEO: return "video";
    default: return "unknown";
    }
}

int samosa_source_kind_parse(const char *name, SamosaSourceKind *kind) {
    if (!name || !kind) return 0;
    for (int value = SAMOSA_SOURCE_UNKNOWN; value <= SAMOSA_SOURCE_VIDEO; ++value) {
        if (!strcmp(name, samosa_source_kind_name((SamosaSourceKind)value))) {
            *kind = (SamosaSourceKind)value;
            return 1;
        }
    }
    return 0;
}

const char *samosa_evidence_operation_name(SamosaEvidenceOperations operation) {
    for (size_t i = 0; i < sizeof(operation_names) / sizeof(operation_names[0]); ++i)
        if (operation_names[i].bit == operation) return operation_names[i].name;
    return NULL;
}

int samosa_evidence_operation_parse(const char *name,
                                    SamosaEvidenceOperations *operation) {
    if (!name || !operation) return 0;
    for (size_t i = 0; i < sizeof(operation_names) / sizeof(operation_names[0]); ++i) {
        if (!strcmp(name, operation_names[i].name)) {
            *operation = operation_names[i].bit;
            return 1;
        }
    }
    return 0;
}

const char *samosa_evidence_coverage_name(SamosaEvidenceCoverage coverage) {
    switch (coverage) {
    case SAMOSA_COVERAGE_EXPLICIT: return "explicit";
    case SAMOSA_COVERAGE_RELEVANCE_FIRST: return "relevance_first";
    case SAMOSA_COVERAGE_OVERVIEW: return "overview";
    case SAMOSA_COVERAGE_EXHAUSTIVE: return "exhaustive";
    default: return NULL;
    }
}

const char *samosa_evidence_quality_name(SamosaEvidenceQuality quality) {
    switch (quality) {
    case SAMOSA_QUALITY_FAST: return "fast";
    case SAMOSA_QUALITY_BALANCED: return "balanced";
    case SAMOSA_QUALITY_QUALITY: return "quality";
    default: return NULL;
    }
}

SamosaEvidenceOperations samosa_source_default_operations(
    SamosaSourceKind kind, const char *media_type) {
    switch (kind) {
    case SAMOSA_SOURCE_TEXT:
        return SAMOSA_EVIDENCE_EXTRACT_TEXT | SAMOSA_EVIDENCE_INSPECT_METADATA |
               SAMOSA_EVIDENCE_RETRIEVE;
    case SAMOSA_SOURCE_DOCUMENT:
        /* PDF already has exact text, page rendering, native OCR, and visual
         * escalation. Other document containers keep only operations their
         * extractor actually implements when they are admitted later. */
        if (media_type && !strcmp(media_type, "application/pdf"))
            return SAMOSA_EVIDENCE_EXTRACT_TEXT | SAMOSA_EVIDENCE_OCR_TEXT |
                   SAMOSA_EVIDENCE_INSPECT_VISUAL |
                   SAMOSA_EVIDENCE_INSPECT_METADATA | SAMOSA_EVIDENCE_RETRIEVE;
        return SAMOSA_EVIDENCE_EXTRACT_TEXT | SAMOSA_EVIDENCE_INSPECT_METADATA |
               SAMOSA_EVIDENCE_RETRIEVE;
    case SAMOSA_SOURCE_IMAGE:
        return SAMOSA_EVIDENCE_OCR_TEXT | SAMOSA_EVIDENCE_INSPECT_VISUAL |
               SAMOSA_EVIDENCE_INSPECT_METADATA;
    case SAMOSA_SOURCE_AUDIO:
        return SAMOSA_EVIDENCE_TRANSCRIBE_AUDIO |
               SAMOSA_EVIDENCE_INSPECT_METADATA | SAMOSA_EVIDENCE_RETRIEVE;
    case SAMOSA_SOURCE_VIDEO:
        /* Audio/subtitle operations are added only after the media inventory
         * has proved those streams and a decoder is available. */
        return SAMOSA_EVIDENCE_ANALYZE_VIDEO |
               SAMOSA_EVIDENCE_INSPECT_METADATA;
    default:
        return 0;
    }
}

static const char *container_for_media_type(const char *media_type) {
    if (!media_type) return "";
    if (!strcmp(media_type, "image/png")) return "png";
    if (!strcmp(media_type, "image/jpeg")) return "jpeg";
    if (!strcmp(media_type, "image/webp")) return "webp";
    if (!strcmp(media_type, "image/gif")) return "gif";
    if (!strcmp(media_type, "application/pdf")) return "pdf";
    if (!strcmp(media_type, "application/vnd.openxmlformats-officedocument.wordprocessingml.document"))
        return "docx";
    if (!strcmp(media_type, "video/mp4")) return "mp4";
    if (!strcmp(media_type, "video/quicktime")) return "quicktime";
    if (!strcmp(media_type, "audio/wav")) return "wav";
    if (!strcmp(media_type, "audio/mpeg")) return "mp3";
    if (!strcmp(media_type, "audio/mp4")) return "mp4";
    if (!strcmp(media_type, "text/html")) return "html";
    return "utf-8";
}

int samosa_source_inventory_from_attachment(
    SamosaSourceInventory *inventory, const char *attachment_id,
    const char *media_type, uint64_t bytes, int image_cap, int video_cap,
    int audio_cap, int document_cap) {
    if (!inventory) return 0;
    memset(inventory, 0, sizeof(*inventory));
    if (image_cap + video_cap + audio_cap + document_cap != 1) return 0;
    if (!attachment_id_valid(attachment_id) || !media_type || !*media_type || !bytes)
        return 0;

    copy_text(inventory->attachment_id, sizeof(inventory->attachment_id), attachment_id);
    copy_text(inventory->media_type, sizeof(inventory->media_type), media_type);
    copy_text(inventory->container, sizeof(inventory->container),
              container_for_media_type(media_type));
    inventory->bytes = bytes;
    if (image_cap) inventory->kind = SAMOSA_SOURCE_IMAGE;
    else if (video_cap) inventory->kind = SAMOSA_SOURCE_VIDEO;
    else if (audio_cap) inventory->kind = SAMOSA_SOURCE_AUDIO;
    else if (!strcmp(media_type, "application/pdf") ||
             !strcmp(media_type, "application/vnd.openxmlformats-officedocument.wordprocessingml.document"))
        inventory->kind = SAMOSA_SOURCE_DOCUMENT;
    else inventory->kind = SAMOSA_SOURCE_TEXT;
    inventory->available_operations = samosa_source_default_operations(
        inventory->kind, media_type);
    return inventory->available_operations != 0;
}

int samosa_source_inventory_valid(const SamosaSourceInventory *inventory,
                                  char *error, size_t error_cap) {
    if (!inventory) {
        evidence_error(error, error_cap, "source_inventory_missing");
        return 0;
    }
    if (!attachment_id_valid(inventory->attachment_id)) {
        evidence_error(error, error_cap, "source_attachment_id_invalid");
        return 0;
    }
    if (inventory->kind <= SAMOSA_SOURCE_UNKNOWN ||
        inventory->kind > SAMOSA_SOURCE_VIDEO) {
        evidence_error(error, error_cap, "source_kind_invalid");
        return 0;
    }
    if (!inventory->media_type[0] || !inventory->bytes) {
        evidence_error(error, error_cap, "source_identity_incomplete");
        return 0;
    }
    if (!inventory->available_operations ||
        (inventory->available_operations & ~((1u << SAMOSA_EVIDENCE_OPERATION_COUNT) - 1u))) {
        evidence_error(error, error_cap, "source_operations_invalid");
        return 0;
    }
    if (inventory->duration_seconds < 0 || inventory->page_count < 0 ||
        inventory->width < 0 || inventory->height < 0) {
        evidence_error(error, error_cap, "source_bounds_invalid");
        return 0;
    }
    if ((inventory->width == 0) != (inventory->height == 0)) {
        evidence_error(error, error_cap, "source_dimensions_incomplete");
        return 0;
    }
    if (error && error_cap) error[0] = 0;
    return 1;
}

int samosa_evidence_plan_safe_fallback(const SamosaSourceInventory *inventory,
                                       SamosaEvidencePlanItem *item) {
    char error[64];
    if (!item || !samosa_source_inventory_valid(inventory, error, sizeof(error)))
        return 0;
    memset(item, 0, sizeof(*item));
    copy_text(item->attachment_id, sizeof(item->attachment_id),
              inventory->attachment_id);
    item->quality = SAMOSA_QUALITY_BALANCED;
    switch (inventory->kind) {
    case SAMOSA_SOURCE_TEXT:
    case SAMOSA_SOURCE_DOCUMENT:
        item->operations = SAMOSA_EVIDENCE_EXTRACT_TEXT;
        item->coverage = SAMOSA_COVERAGE_RELEVANCE_FIRST;
        break;
    case SAMOSA_SOURCE_IMAGE:
        item->operations = SAMOSA_EVIDENCE_INSPECT_VISUAL;
        item->coverage = SAMOSA_COVERAGE_OVERVIEW;
        break;
    case SAMOSA_SOURCE_AUDIO:
        item->operations = SAMOSA_EVIDENCE_TRANSCRIBE_AUDIO;
        item->coverage = SAMOSA_COVERAGE_RELEVANCE_FIRST;
        break;
    case SAMOSA_SOURCE_VIDEO:
        item->operations = SAMOSA_EVIDENCE_ANALYZE_VIDEO;
        item->coverage = SAMOSA_COVERAGE_OVERVIEW;
        break;
    default:
        return 0;
    }
    return samosa_evidence_plan_item_valid(inventory, item, error, sizeof(error));
}

int samosa_evidence_plan_item_valid(const SamosaSourceInventory *inventory,
                                    const SamosaEvidencePlanItem *item,
                                    char *error, size_t error_cap) {
    if (!samosa_source_inventory_valid(inventory, error, error_cap)) return 0;
    if (!item || strcmp(inventory->attachment_id, item->attachment_id)) {
        evidence_error(error, error_cap, "plan_attachment_mismatch");
        return 0;
    }
    if (!item->operations ||
        (item->operations & ~inventory->available_operations)) {
        evidence_error(error, error_cap, "plan_operation_unavailable");
        return 0;
    }
    if (!samosa_evidence_coverage_name(item->coverage)) {
        evidence_error(error, error_cap, "plan_coverage_invalid");
        return 0;
    }
    if (!samosa_evidence_quality_name(item->quality)) {
        evidence_error(error, error_cap, "plan_quality_invalid");
        return 0;
    }
    if ((item->page_start == 0) != (item->page_end == 0) ||
        item->page_start < 0 || item->page_end < item->page_start) {
        evidence_error(error, error_cap, "plan_page_range_invalid");
        return 0;
    }
    if (item->page_start) {
        if (inventory->kind != SAMOSA_SOURCE_DOCUMENT ||
            (inventory->page_count && item->page_end > inventory->page_count)) {
            evidence_error(error, error_cap, "plan_page_range_unavailable");
            return 0;
        }
    }
    if (item->has_time_range) {
        if ((inventory->kind != SAMOSA_SOURCE_AUDIO &&
             inventory->kind != SAMOSA_SOURCE_VIDEO) ||
            item->time_start < 0 || item->time_end <= item->time_start ||
            (inventory->duration_seconds > 0 &&
             item->time_end > inventory->duration_seconds)) {
            evidence_error(error, error_cap, "plan_time_range_invalid");
            return 0;
        }
    } else if (item->time_start != 0 || item->time_end != 0) {
        evidence_error(error, error_cap, "plan_time_range_incomplete");
        return 0;
    }
    if (error && error_cap) error[0] = 0;
    return 1;
}
