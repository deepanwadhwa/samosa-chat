/* Contract and coverage regressions. The live suite separately checks the
 * decision model and generated answers; these checks reject unsafe plans. */
#define main samosa_gateway_program_main
#include "../src/samosa_gateway.c"
#undef main
#include <assert.h>

static int valid(const char *raw, int count) {
    char *arena = NULL; jval *plan = json_parse(raw, &arena);
    int selected[MEMORY_SOURCE_LIMIT] = {0}, overview, inventory, metadata, content;
    int result = memory_plan_valid(plan, count, selected, &overview, &inventory, &metadata, &content);
    json_free(plan); free(arena); return result;
}

typedef struct { Gateway *gateway; int observed_child; } MemoryCancel;
static void *cancel_memory(void *opaque) {
    MemoryCancel *context = opaque;
    Gateway *gateway = context->gateway;
    for (int i = 0; i < 200; ++i) {
        pthread_mutex_lock(&gateway->mu);
        int running = gateway->document_child_pid > 0;
        pthread_mutex_unlock(&gateway->mu);
        if (running) { context->observed_child = 1; break; }
        usleep(10000);
    }
    atomic_store(&gateway->document_cancel_requested, 1);
    return NULL;
}

int main(int argc, char **argv) {
    /* Reader excerpts and filename/identity probes use byte ceilings, but
       their boundary must remain a complete UTF-8 character. */
    const char *widths = "Aé漢😀";
    assert(text_utf8_prefix(widths, strlen(widths), 2) == 1);
    assert(text_utf8_prefix(widths, strlen(widths), 4) == 3);
    assert(text_utf8_prefix(widths, strlen(widths), 7) == 6);
    char *utf_arena = NULL;
    jval *utf_context = json_parse("{\"artifacts\":[{\"artifact_kind\":\"extracted_text\",\"freshness\":\"current\",\"status\":\"active\",\"content\":\"Aé漢😀\"}]}", &utf_arena);
    MemorySource utf_source = {.context = utf_context}; TextBuffer excerpt = {0};
    memory_append_content(&excerpt, &utf_source, 4);
    assert(strstr(excerpt.data, "Aé\n") && utf8_scalar_count((const unsigned char *)excerpt.data, excerpt.len) >= 0);
    assert(strstr(excerpt.data, "truncated"));
    free(excerpt.data); json_free(utf_context); free(utf_arena);
    if (argc > 2 && !strcmp(argv[1], "--mode") && !strcmp(argv[2], "memory")) {
        sleep(10); return 0;
    }
    char *kind_arena = NULL;
    jval *kind_case = json_parse("{\"form\":\"form\",\"guide\":\"instructions\",\"invalid\":\"delete\",\"topic\":\"topic\"}", &kind_arena);
    assert(!strcmp(jobs_search_kind(json_get(kind_case, "form"), json_get(kind_case, "form")), "document_type"));
    assert(!strcmp(jobs_search_kind(json_get(kind_case, "guide"), json_get(kind_case, "guide")), "document_type"));
    assert(!jobs_search_kind(json_get(kind_case, "form"), json_get(kind_case, "guide")));
    assert(!jobs_search_kind(json_get(kind_case, "invalid"), json_get(kind_case, "invalid")));
    assert(!strcmp(jobs_search_kind(json_get(kind_case, "topic"), NULL), "topic"));
    json_free(kind_case); free(kind_arena);
    char search_span[257];
    assert(jobs_search_subject("Find AB123 forms", "AB-123", search_span));
    assert(!strcmp(search_span, "AB123"));
    assert(jobs_search_subject("Find records for Mira Lee", "Mira Lee", search_span));
    assert(!jobs_search_subject("Find records for Mira Leena", "Mira Lee", search_span));
    assert(!jobs_search_subject("Find AB1234 forms", "AB123", search_span));
    assert(!jobs_search_subject("Find invoices", "tax forms", search_span));
    char *search_arena = NULL;
    jval *search_items = json_parse("[{\"excerpt\":\"Actual document title with enough literal supporting text.\"}]", &search_arena);
    int search_candidates[1] = {0};
    const char *search_verdicts[] = {
        "[{\"number\":0,\"status\":\"match\",\"quote\":\"Actual document title with enough literal supporting text.\"}]",
        "[{\"number\":0,\"status\":\"uncertain\",\"quote\":\"\"}]",
        "[{\"number\":0,\"status\":\"match\",\"quote\":\"Invented document title with unsupported text.\"}]",
        "[{\"number\":1,\"status\":\"match\",\"quote\":\"Actual document title with enough literal supporting text.\"}]",
        "[{\"number\":0,\"status\":\"match\",\"quote\":\"title\"}]",
        "[]"};
    for (int i = 0; i < 6; ++i) {
        char *verdict_arena = NULL; jval *verdicts = json_parse(search_verdicts[i], &verdict_arena);
        assert(jobs_search_verdicts_valid(search_items, search_candidates, 1, verdicts) == (i < 2));
        json_free(verdicts); free(verdict_arena);
    }
    json_free(search_items); free(search_arena);
    MemorySource paths[3] = {{.path="/fixture/alpha/notes.txt"}, {.path="/fixture/beta/notes.txt"}, {.path="/fixture/other.txt"}};
    int named[MEMORY_SOURCE_LIMIT] = {0};
    assert(memory_named_sources("/fixture", "Read beta/notes.txt", paths, 3, named) == 1);
    assert(!named[0] && named[1] && !named[2]);
    assert(memory_named_sources("/fixture", "Read notes.txt", paths, 3, named) == 2);
    assert(named[0] && named[1] && !named[2]);
    assert(memory_named_sources("/fixture", "Read other.txt.bak", paths, 3, named) == 0);
    assert(memory_named_sources("/fixture", "Read other.txt?", paths, 3, named) == 1 && named[2]);
    assert(memory_contains_identity("Mira Lee appointment.txt", "Mira Lee"));
    assert(!memory_contains_identity("Mira Leena diary.txt", "Mira Lee"));
    assert(!memory_contains_identity("Person Xavier notes", "Person X"));
    assert(memory_contains_identity("Reference: WX-482.", "WX-482"));
    MemorySource proof = {.path="/fixture/Mira Lee.pdf", .preview_literal=1};
    text_add(&proof.preview, "Mira Lee project notes. Reference CE-405.");
    assert(memory_proof_grounded("/fixture", "Mira Lee", 1, &proof, "Mira Lee"));
    assert(!memory_proof_grounded("/fixture", "Mira Wen", 1, &proof, "Mira Lee"));
    assert(!memory_proof_grounded("/fixture", "Mira Lee", 1, &proof, "Mira Lee invented appointment"));
    assert(memory_proof_grounded("/fixture", "", 0, &proof, "project notes"));
    assert(memory_named_filename_proof("/fixture", "Mira Lee", &proof));
    proof.preview_literal = 0;
    assert(!memory_proof_grounded("/fixture", "", 0, &proof, "project notes"));
    free(proof.preview.data);
    MemorySource prefix = {.path="/fixture/Person Xavier.txt", .preview_literal=1};
    text_add(&prefix.preview, "Mira Leena gardening diary.");
    assert(!memory_proof_grounded("/fixture", "Person X", 1, &prefix, "Person X"));
    assert(!memory_proof_grounded("/fixture", "Mira Lee", 1, &prefix, "Mira Lee"));
    assert(!memory_named_filename_proof("/fixture", "Person X", &prefix));
    assert(memory_proof_grounded("/fixture", "Mira Leena", 1, &prefix, "Mira Leena"));
    prefix.preview_complete = 1;
    TextBuffer topic_probe = {0};
    assert(memory_semantic_probe("/fixture", &prefix, &topic_probe));
    assert(strstr(topic_probe.data, "Mira Leena gardening diary."));
    free(topic_probe.data); topic_probe = (TextBuffer){0};
    prefix.preview_literal = 0;
    assert(!memory_semantic_probe("/fixture", &prefix, &topic_probe));
    assert(!strstr(topic_probe.data, "Mira Leena gardening diary."));
    free(topic_probe.data);
    free(prefix.preview.data);
    assert(valid("{\"version\":1,\"scope\":\"folder\",\"actions\":[\"overview\"],\"sources\":[]}", 0));
    assert(valid("{\"version\":1,\"scope\":\"files\",\"actions\":[\"inventory\",\"metadata\",\"content\"],\"sources\":[{\"number\":0,\"choice\":\"plausible\",\"score\":0.7}]}", 1));
    assert(!valid("{\"version\":1,\"scope\":\"folder\",\"actions\":[\"delete\"],\"sources\":[]}", 0));
    assert(!valid("{\"version\":1,\"scope\":\"folder\",\"actions\":[\"content\",\"content\"],\"sources\":[]}", 0));
    assert(!valid("{\"version\":1,\"scope\":\"files\",\"actions\":[\"overview\"],\"sources\":[]}", 0));
    assert(!valid("{\"version\":1,\"scope\":\"files\",\"actions\":[\"metadata\"],\"sources\":[{\"number\":1,\"choice\":\"plausible\",\"score\":1}]}", 1));
    assert(!valid("{\"version\":1,\"scope\":\"files\",\"actions\":[\"content\"],\"sources\":[{\"number\":0,\"choice\":\"plausible\",\"score\":2}]}", 1));
    assert(!valid("{\"version\":1,\"scope\":\"files\",\"actions\":[\"content\"],\"sources\":[{\"number\":0,\"choice\":\"plausible\",\"score\":0.9},{\"number\":0,\"choice\":\"plausible\",\"score\":0.9}]}", 2));
    char *member_arena = NULL;
    jval *members = json_parse("{\"match\":[{\"number\":0,\"quote\":\"proof A\"},{\"number\":2,\"quote\":\"proof B\"}],\"uncertain\":[1]}", &member_arena);
    int membership[MEMORY_SOURCE_LIMIT] = {0};
    assert(memory_membership_valid(members, 3, membership));
    assert(membership[0] == 1 && membership[1] == 2 && membership[2] == 1);
    json_free(members); free(member_arena);
    members = json_parse("{\"match\":[{\"number\":0,\"quote\":\"proof\"},{\"number\":0,\"quote\":\"proof\"}],\"uncertain\":[]}", &member_arena);
    assert(!memory_membership_valid(members, 2, membership));
    json_free(members); free(member_arena);
    members = json_parse("{\"match\":[{\"number\":0,\"quote\":\"proof\"}],\"uncertain\":[0]}", &member_arena);
    assert(!memory_membership_valid(members, 2, membership));
    json_free(members); free(member_arena);
    char *arena = NULL;
    MemorySource source = {0};
    source.context = json_parse("{\"artifacts\":[{\"artifact_kind\":\"page_text\",\"content\":\"Current evidence\",\"freshness\":\"current\",\"status\":\"active\",\"selector\":{\"start\":37}},{\"artifact_kind\":\"page_text\",\"content\":\"Old secret\",\"freshness\":\"stale\",\"status\":\"active\"},{\"artifact_kind\":\"page_text\",\"content\":\"Another current page\",\"freshness\":\"current\",\"status\":\"active\"}]}", &arena);
    TextBuffer evidence = {0}; memory_append_content(&evidence, &source, 20);
    assert(strstr(evidence.data, "PDF page 37"));
    assert(strstr(evidence.data, "1 of 2"));
    assert(strstr(evidence.data, "not read in this turn"));
    assert(!strstr(evidence.data, "Old secret"));
    assert(!strstr(evidence.data, "Another current page"));
    free(evidence.data); json_free(source.context); free(arena);
    source.context = json_parse("{\"artifacts\":[{\"artifact_kind\":\"summary_short\",\"content\":\"Invented secret\",\"freshness\":\"current\",\"status\":\"active\"},{\"artifact_kind\":\"extracted_text\",\"content\":\"Literal evidence\",\"freshness\":\"current\",\"status\":\"active\"}]}", &arena);
    evidence = (TextBuffer){0}; memory_append_content(&evidence, &source, 1000);
    assert(strstr(evidence.data, "Literal evidence") && !strstr(evidence.data, "Invented secret"));
    assert(strstr(evidence.data, "1 of 1"));
    free(evidence.data); json_free(source.context); free(arena);
    source.context = json_parse("{\"artifacts\":[{\"artifact_kind\":\"extracted_text\",\"content\":\"One original line\",\"freshness\":\"current\",\"status\":\"active\"},{\"artifact_kind\":\"extracted_text\",\"content\":\"One original line\",\"freshness\":\"current\",\"status\":\"active\"}]}", &arena);
    evidence = (TextBuffer){0}; memory_append_content(&evidence, &source, 1000);
    assert(strstr(evidence.data, "1 of 1"));
    assert(!strstr(strstr(evidence.data, "One original line") + 1, "One original line"));
    free(evidence.data); json_free(source.context); free(arena);
    /* Exercise the actual memory invocation path, including temp request
       cleanup and prompt cancellation of its owned decision subprocess. */
    Gateway *gateway = calloc(1, sizeof(*gateway)); assert(gateway);
    pthread_mutex_init(&gateway->mu, NULL);
    char task_home[] = "/tmp/samosa-memory-contract-XXXXXX";
    assert(mkdtemp(task_home));
    path_copy(gateway->home, sizeof(gateway->home), task_home);
    snprintf(gateway->jobs_root, sizeof(gateway->jobs_root), "%s/jobs", task_home);
    assert(realpath(argv[0], gateway->samosa_decision));
    MemoryCancel context = {gateway, 0};
    pthread_t canceller; assert(!pthread_create(&canceller, NULL, cancel_memory, &context));
    char error[256]; long long started = monotonic_millis();
    char *reply = jobs_decision_invoke(gateway, "memory", "{\"question\":\"Inspect files\"}", NULL, NULL, error, sizeof(error));
    pthread_join(canceller, NULL);
    assert(context.observed_child && !reply && atomic_load(&gateway->document_cancel_requested));
    assert(monotonic_millis() - started < 4000 && !gateway->document_child_pid);
    /* A controlled restart must leave a pending handoff untouched. Clearing
       the flag exercises the same queued action and its unavailable-source
       result, proving that the fixture is actually runnable. */
    pthread_mutex_init(&gateway->chutni_mu, NULL);
    char missing_folder[PATH_MAX], pending_path[PATH_MAX];
    snprintf(missing_folder, sizeof(missing_folder), "%s/missing-source", task_home);
    assert(save_job_state(gateway, "deferred-memory-fixture", "Generated privacy fixture", missing_folder));
    assert(job_state_path(gateway, "deferred-memory-fixture", "memory.json", pending_path, 1));
    assert(write_small_file(pending_path, "{\"state\":\"busy\"}"));
    assert(!setenv("SAMOSA_DEFER_PENDING_MEMORY", "1", 1));
    jobs_folder_memory_drain(gateway);
    char *pending = read_file_limit(pending_path, 8192);
    assert(pending && !strcmp(pending, "{\"state\":\"busy\"}")); free(pending);
    assert(!unsetenv("SAMOSA_DEFER_PENDING_MEMORY"));
    jobs_folder_memory_drain(gateway);
    pending = read_file_limit(pending_path, 8192);
    assert(pending && strstr(pending, "unavailable")); free(pending);
    pthread_mutex_destroy(&gateway->chutni_mu);
    assert(!unlink(pending_path));
    assert(job_state_path(gateway, "deferred-memory-fixture", "job.json", pending_path, 0)); assert(!unlink(pending_path));
    assert(job_state_path(gateway, "deferred-memory-fixture", "events.jsonl", pending_path, 0)); assert(!unlink(pending_path));
    snprintf(pending_path, sizeof(pending_path), "%s/jobs/deferred-memory-fixture", task_home); assert(!rmdir(pending_path));
    char jobs[PATH_MAX]; snprintf(jobs, sizeof(jobs), "%s/jobs", task_home);
    assert(!rmdir(jobs)); assert(!rmdir(task_home));
    pthread_mutex_destroy(&gateway->mu); free(gateway);
    char *scope_arena = NULL;
    jval *scope = json_parse("{\"focus\":[\"selected.pdf\"],\"selected\":{\"path\":\"selected.pdf\",\"condition_evidence\":[]},\"other\":{\"path\":\"other.pdf\",\"condition_evidence\":[]},\"filtered\":{\"path\":\"selected.pdf\",\"condition_evidence\":[{\"requirement\":\"signed\"}]}}", &scope_arena);
    assert(scope);
    assert(jobs_search_candidate_in_scope(json_get(scope, "focus"), json_get(scope, "selected")));
    assert(!jobs_search_candidate_in_scope(json_get(scope, "focus"), json_get(scope, "other")));
    assert(!jobs_search_candidate_in_scope(json_get(scope, "focus"), json_get(scope, "filtered")));
    char *item_intent_arena = NULL;
    jval *item_intent = json_parse("{\"parent\":{\"target\":\"a form\"},\"same\":{\"search_intent\":{\"target\":\"a form\"}},\"refined\":{\"search_intent\":{\"target\":\"an instruction guide\"}}}", &item_intent_arena);
    assert(jobs_search_item_intent_matches(json_get(item_intent, "parent"), json_get(item_intent, "same")));
    assert(!jobs_search_item_intent_matches(json_get(item_intent, "parent"), json_get(item_intent, "refined")));
    json_free(item_intent); free(item_intent_arena);
    json_free(scope); free(scope_arena);
    puts("test_memory_harness: PASS"); return 0;
}
