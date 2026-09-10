#include "samosa_evidence.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *ID =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static void inventory_contract(void) {
    SamosaSourceInventory source;
    char error[96];

    assert(samosa_source_inventory_from_attachment(
        &source, ID, "image/png", 120, 1, 0, 0, 0));
    assert(source.kind == SAMOSA_SOURCE_IMAGE);
    assert(!strcmp(source.container, "png"));
    assert(source.available_operations & SAMOSA_EVIDENCE_OCR_TEXT);
    assert(source.available_operations & SAMOSA_EVIDENCE_INSPECT_VISUAL);
    assert(!(source.available_operations & SAMOSA_EVIDENCE_TRANSCRIBE_AUDIO));
    assert(samosa_source_inventory_valid(&source, error, sizeof(error)));

    assert(samosa_source_inventory_from_attachment(
        &source, ID, "application/pdf", 500, 0, 0, 0, 1));
    assert(source.kind == SAMOSA_SOURCE_DOCUMENT);
    assert(source.available_operations & SAMOSA_EVIDENCE_EXTRACT_TEXT);
    assert(source.available_operations & SAMOSA_EVIDENCE_OCR_TEXT);
    assert(source.available_operations & SAMOSA_EVIDENCE_INSPECT_VISUAL);
    assert(source.available_operations & SAMOSA_EVIDENCE_RETRIEVE);

    assert(samosa_source_inventory_from_attachment(
        &source, ID, "text/x-python", 80, 0, 0, 0, 1));
    assert(source.kind == SAMOSA_SOURCE_TEXT);
    assert(source.available_operations & SAMOSA_EVIDENCE_EXTRACT_TEXT);
    assert(!(source.available_operations & SAMOSA_EVIDENCE_INSPECT_VISUAL));

    assert(samosa_source_inventory_from_attachment(
        &source, ID, "audio/wav", 64000, 0, 0, 1, 0));
    assert(source.kind == SAMOSA_SOURCE_AUDIO);
    assert(source.available_operations & SAMOSA_EVIDENCE_TRANSCRIBE_AUDIO);
    assert(source.available_operations & SAMOSA_EVIDENCE_RETRIEVE);

    assert(samosa_source_inventory_from_attachment(
        &source, ID, "video/mp4", 1000, 0, 1, 0, 0));
    assert(source.kind == SAMOSA_SOURCE_VIDEO);
    assert(source.available_operations & SAMOSA_EVIDENCE_ANALYZE_VIDEO);
    /* Audio is not inferred merely because the MP4 container could hold it. */
    assert(!(source.available_operations & SAMOSA_EVIDENCE_TRANSCRIBE_AUDIO));

    assert(!samosa_source_inventory_from_attachment(
        &source, ID, "image/png", 120, 1, 1, 0, 0));
    assert(!samosa_source_inventory_from_attachment(
        &source, "not-a-hash", "image/png", 120, 1, 0, 0, 0));
    assert(!samosa_source_inventory_from_attachment(
        &source, ID, "image/png", 0, 1, 0, 0, 0));
}

static void names_are_stable(void) {
    SamosaSourceKind kind = SAMOSA_SOURCE_UNKNOWN;
    SamosaEvidenceOperations operation = 0;
    assert(!strcmp(SAMOSA_SOURCE_SCHEMA, "samosa.source.v1"));
    assert(!strcmp(SAMOSA_EVIDENCE_PLAN_SCHEMA, "samosa.evidence-plan.v1"));
    assert(samosa_source_kind_parse("audio", &kind));
    assert(kind == SAMOSA_SOURCE_AUDIO);
    assert(!samosa_source_kind_parse("podcast", &kind));
    assert(samosa_evidence_operation_parse("transcribe_audio", &operation));
    assert(operation == SAMOSA_EVIDENCE_TRANSCRIBE_AUDIO);
    assert(!strcmp(samosa_evidence_operation_name(operation),
                   "transcribe_audio"));
    assert(!samosa_evidence_operation_parse("run_command", &operation));
    assert(samosa_evidence_operation_name(3u) == NULL);
    assert(samosa_source_default_operations(SAMOSA_SOURCE_AUDIO, "audio/wav") &
           SAMOSA_EVIDENCE_TRANSCRIBE_AUDIO);
}

static void plan_contract(void) {
    SamosaSourceInventory source;
    SamosaEvidencePlanItem item;
    char error[96];

    assert(samosa_source_inventory_from_attachment(
        &source, ID, "application/pdf", 500, 0, 0, 0, 1));
    source.page_count = 12;
    assert(samosa_evidence_plan_safe_fallback(&source, &item));
    assert(item.operations == SAMOSA_EVIDENCE_EXTRACT_TEXT);
    assert(item.coverage == SAMOSA_COVERAGE_RELEVANCE_FIRST);
    assert(item.quality == SAMOSA_QUALITY_BALANCED);

    item.operations |= SAMOSA_EVIDENCE_INSPECT_VISUAL;
    item.coverage = SAMOSA_COVERAGE_EXPLICIT;
    item.page_start = 4;
    item.page_end = 6;
    assert(samosa_evidence_plan_item_valid(&source, &item, error, sizeof(error)));

    item.operations |= SAMOSA_EVIDENCE_TRANSCRIBE_AUDIO;
    assert(!samosa_evidence_plan_item_valid(&source, &item, error, sizeof(error)));
    assert(!strcmp(error, "plan_operation_unavailable"));
    item.operations &= ~SAMOSA_EVIDENCE_TRANSCRIBE_AUDIO;

    item.page_end = 13;
    assert(!samosa_evidence_plan_item_valid(&source, &item, error, sizeof(error)));
    assert(!strcmp(error, "plan_page_range_unavailable"));

    assert(samosa_source_inventory_from_attachment(
        &source, ID, "audio/wav", 64000, 0, 0, 1, 0));
    source.duration_seconds = 60.0;
    assert(samosa_evidence_plan_safe_fallback(&source, &item));
    item.coverage = SAMOSA_COVERAGE_EXPLICIT;
    item.has_time_range = 1;
    item.time_start = 10.0;
    item.time_end = 20.0;
    assert(samosa_evidence_plan_item_valid(&source, &item, error, sizeof(error)));
    item.time_end = 61.0;
    assert(!samosa_evidence_plan_item_valid(&source, &item, error, sizeof(error)));
    assert(!strcmp(error, "plan_time_range_invalid"));

    item.time_end = 20.0;
    item.attachment_id[0] = 'f';
    assert(!samosa_evidence_plan_item_valid(&source, &item, error, sizeof(error)));
    assert(!strcmp(error, "plan_attachment_mismatch"));
}

int main(void) {
    inventory_contract();
    names_are_stable();
    plan_contract();
    puts("samosa evidence contracts: PASS");
    return 0;
}
