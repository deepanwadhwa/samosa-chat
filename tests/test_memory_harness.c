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
    if (argc > 2 && !strcmp(argv[1], "--mode") && !strcmp(argv[2], "memory")) {
        sleep(10); return 0;
    }
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
    MemorySource proof = {.path="/fixture/Mira Lee.pdf"};
    text_add(&proof.preview, "Mira Lee project notes. Reference CE-405.");
    assert(memory_proof_grounded("/fixture", "Mira Lee", 1, &proof, "Mira Lee"));
    assert(!memory_proof_grounded("/fixture", "Mira Wen", 1, &proof, "Mira Lee"));
    assert(!memory_proof_grounded("/fixture", "Mira Lee", 1, &proof, "Mira Lee invented appointment"));
    assert(memory_proof_grounded("/fixture", "", 0, &proof, "project notes"));
    free(proof.preview.data);
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
    char jobs[PATH_MAX]; snprintf(jobs, sizeof(jobs), "%s/jobs", task_home);
    assert(!rmdir(jobs)); assert(!rmdir(task_home));
    pthread_mutex_destroy(&gateway->mu); free(gateway);
    puts("test_memory_harness: PASS"); return 0;
}
