#define main samosa_gateway_test_main
#define PROMPT_BUDGET_TRACE(...) do { if (getenv("SAMOSA_PROMPT_BUDGET_TRACE")) fprintf(stderr, __VA_ARGS__); } while (0)
#include "../src/samosa_gateway.c"
#undef main
#include <assert.h>

static void *cancel_review(void *opaque) {
    Gateway *g = opaque;
    for (int i = 0; i < 5000; i++) {
        if (atomic_load(&g->generating)) {
            atomic_store(&g->document_cancel_requested, 1);
            pthread_mutex_lock(&g->mu);
            if (g->upstream_fd >= 0) shutdown(g->upstream_fd, SHUT_RDWR);
            pthread_mutex_unlock(&g->mu);
            return NULL;
        }
        usleep(1000);
    }
    assert(0 && "summary inference was not observed"); return NULL;
}

int main(int argc, char **argv) {
    assert(argc == 4);
    Gateway *g = calloc(1, sizeof(*g)); assert(g);
    path_copy(g->backend, sizeof(g->backend), "ornith");
    path_copy(g->home, sizeof(g->home), argv[2]); g->backend_port = atoi(argv[1]);
    int lifetime[2]; assert(!pipe(lifetime));
    pid_t child = fork(); assert(child >= 0);
    if (!child) { close(lifetime[1]); char byte; (void)read(lifetime[0], &byte, 1); _exit(0); }
    close(lifetime[0]);
    g->backend_pid = child; g->upstream_fd = -1;
    pthread_mutex_init(&g->mu, NULL); pthread_mutex_init(&g->generation_gate_mu, NULL);
    pthread_mutex_init(&g->developer_trace_mu, NULL);
    TextBuffer evidence = {0}, payload = {0}, question = {0};
    text_add(&question, "Describe the categories represented in these generated records.");
    if (!strcmp(argv[3], "oversized-question"))
        for (int i = 0; i < 2000; i++) text_add(&question, " Extra generated question text.");
    text_add(&evidence, "\n--- Folder/file action evidence ---\nIndexed source records: 900; inventory records supplied: 80.\n"
        "INCOMPLETE INVENTORY: files exist outside this decision batch. No exhaustive absence claim is allowed.\n"
        "Content coverage: opening samples only. Later pages may be unread.\n");
    int files = !strcmp(argv[3], "small") ? 1 : 80;
    int repeats = !strcmp(argv[3], "small") ? 1 : !strcmp(argv[3], "real") ? 10 : !strcmp(argv[3], "reduce") ? 100 : 45;
    for (int i = 0; i < files; i++) {
        char label[128]; snprintf(label, sizeof(label), "[File: generated-record-%03d.txt] type=text/plain\n", i);
        text_add(&evidence, label);
        for (int j = 0; j < repeats; j++) text_add(&evidence,
            "Generated material about orchard irrigation, rainfall measurements and water use. é😀测试\n");
        char fact[96]; snprintf(fact, sizeof(fact), "FACT_%03d: measured value %d.\n", i, i + 700);
        text_add(&evidence, fact);
        if (!strcmp(argv[3], "sanitized-evidence")) text_add(&evidence, "Legacy incomplete character: \xc3\n");
    }
    text_add(&evidence, "--- end folder/file action evidence ---\n");
    text_add(&payload, "{\"model\":\"ornith\",\"chat_template_kwargs\":{\"enable_thinking\":false},\"thinking\":\"off\",\"messages\":[{\"role\":\"system\",\"content\":\"Answer using labelled evidence and preserve uncertainty. Inventory and coverage facts are authoritative. Derived notes are paraphrases, never exact quotations. Give only the final answer.\"},"
        "{\"role\":\"user\",\"content\":");
    TextBuffer user = {0}; text_add(&user, question.data); text_add(&user, evidence.data);
    text_json_string(&payload, user.data); text_add(&payload, "}],\"stream\":false,\"max_tokens\":8192}");
    if (!strcmp(argv[3], "cancel")) atomic_store(&g->document_cancel_requested, 1);
    pthread_t cancellation;
    if (!strcmp(argv[3], "cancelmid")) assert(!pthread_create(&cancellation, NULL, cancel_review, g));
    WebProgress progress = {0};
    const char *failure = NULL;
    int ok = folder_prompt_budget(g, &payload, evidence.data, question.data, &progress, &failure);
    if (!ok) fprintf(stderr, "failure=%s\n", failure);
    if (!strcmp(argv[3], "cancelmid")) pthread_join(cancellation, NULL);
    if (!strcmp(argv[3], "fail") || !strcmp(argv[3], "truncated") || !strcmp(argv[3], "cancel") ||
        !strcmp(argv[3], "cancelmid") || !strcmp(argv[3], "oversized-question")) assert(!ok);
    else {
        assert(ok);
        char *arena = NULL; jval *root = json_parse(payload.data, &arena); int exact = 0;
        size_t count = prompt_token_count(g, root, &exact);
        int context = prompt_live_context(g);
        jval *max = json_get(root, "max_tokens");
        assert(max && max->t == J_NUM && count + max->num + 256 <= context);
        assert(strstr(payload.data, "Indexed source records: 900") && strstr(payload.data, "INCOMPLETE INVENTORY"));
        assert(strstr(payload.data, "opening samples only") && strstr(payload.data, question.data));
        printf("%s\n", payload.data);
        json_free(root); free(arena);
    }
    free(evidence.data); free(payload.data); free(question.data); free(user.data); free(progress.buffered.data);
    close(lifetime[1]); waitpid(child, NULL, 0);
    pthread_mutex_destroy(&g->developer_trace_mu);
    pthread_mutex_destroy(&g->mu); pthread_mutex_destroy(&g->generation_gate_mu); free(g);
    return 0;
}
