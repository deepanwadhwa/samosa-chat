/* Focused regression checks for the document subprocess boundary and OCR
 * response contract.  This includes the gateway so the checks exercise the
 * actual static helpers rather than a second implementation of the rules. */
#define main samosa_gateway_test_main
#include "../src/samosa_gateway.c"
#undef main

typedef struct {
    Gateway *gateway;
    const char *ready_path;
} CancelChildContext;

static void *cancel_child(void *opaque) {
    CancelChildContext *context = opaque;
    for (int i = 0; i < 200 && access(context->ready_path, F_OK) != 0; ++i)
        usleep(10000);
    atomic_store(&context->gateway->document_cancel_requested, 1);
    document_child_stop(context->gateway, 0);
    return NULL;
}

static int cancel_at_ocr_complete(void *opaque, const char *filename,
                                  const char *stage, const char *message) {
    (void)filename;
    (void)message;
    if (!strcmp(stage, "ocr_complete"))
        atomic_store(&((Gateway *)opaque)->document_cancel_requested, 1);
    return 1;
}

static int write_contract_fixture(const char *path) {
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int wrote = fputs("contract fixture\n", file) >= 0;
    int closed = fclose(file) == 0;
    return wrote && closed;
}

static void cleanup_contract_cache(const char *root, const char *key) {
    char entry[PATH_MAX + 80], shard[PATH_MAX + 8];
    rc_entry_path(root, key, entry, sizeof(entry));
    snprintf(shard, sizeof(shard), "%s/%c%c", root, key[0], key[1]);
    (void)unlink(entry);
    char lock_path[PATH_MAX + 24];
    snprintf(lock_path, sizeof(lock_path), "%s/.lock", shard);
    (void)unlink(lock_path);
    (void)rmdir(shard);
    (void)rmdir(root);
}

static int check_capture_terminator(Gateway *gateway) {
    int status = 0;
    char *argv[] = {"/usr/bin/printf", "abc", NULL};
    char *output = run_capture(gateway, argv[0], argv, 16, &status);
    int ok = output && !memcmp(output, "abc", 3) && output[3] == '\0' &&
             !memchr(output, '\0', 3);
    free(output);
    return ok;
}

static int check_invalid_ocr_rejected(void) {
    static const char *const invalid[] = {
        "{\"ok\":true,\"lines\":[]} trailing}",
        "{\"ok\":true,\"lines\":[{\"text\":\"INVALID NUMBER\",\"conf\":.3}]}",
        "{\"ok\":true,\"lines\":[{\"text\":\"INVALID BOX\",\"conf\":0.9,\"bbox\":[10,10,0,0]}]}"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        char *arena = NULL;
        jval *root = json_parse(invalid[i], &arena), *lines = NULL;
        int accepted = ocr_json_text_complete(invalid[i]) &&
                       ocr_json_validate(root, &lines);
        json_free(root);
        free(arena);
        if (accepted) return 0;
    }
    return 1;
}

static int check_deep_ocr_rejected(void) {
    const size_t depth = 100000;
    char *raw = malloc(depth * 2 + 64);
    if (!raw) return 0;
    size_t at = 0;
    at += (size_t)snprintf(raw + at, depth * 2 + 64 - at,
                           "{\"ok\":true,\"lines\":[");
    for (size_t i = 0; i < depth; ++i) raw[at++] = '[';
    raw[at++] = '0';
    for (size_t i = 0; i < depth; ++i) raw[at++] = ']';
    raw[at++] = ']', raw[at++] = '}', raw[at] = 0;
    int accepted = ocr_json_text_complete(raw);
    free(raw);
    return !accepted;
}

static int check_empty_ocr_requires_review(Gateway *gateway, const char *self) {
    char input[PATH_MAX], cache_root[PATH_MAX], key[65];
    snprintf(input, sizeof(input), "/tmp/samosa-document-contract-empty-%d.png",
             (int)getpid());
    snprintf(cache_root, sizeof(cache_root), "/tmp/samosa-document-contract-empty-cache-%d",
             (int)getpid());
    if (!write_contract_fixture(input) || read_cache_key_file(input, key) != 0)
        return 0;
    setenv("SAMOSA_DOCUMENT_CONTRACT_READER_MODE", "empty", 1);
    setenv("SAMOSA_READ_CACHE_DIR", cache_root, 1);
    path_copy(gateway->samosa_ocr, sizeof(gateway->samosa_ocr), self);
    path_copy(gateway->reader_fingerprint, sizeof(gateway->reader_fingerprint),
              "contract-empty-v1");
    atomic_store(&gateway->document_processing, 1);
    atomic_store(&gateway->document_cancel_requested, 0);
    char *result = doc_read_handler(gateway, input, NULL);
    char *arena = NULL;
    jval *root = result ? json_parse(result, &arena) : NULL;
    jval *review = root && root->t == J_OBJ ? json_get(root, "needs_review") : NULL;
    jval *text = root && root->t == J_OBJ ? json_get(root, "text") : NULL;
    jval *pages = root && root->t == J_OBJ ? json_get(root, "pages") : NULL;
    jval *page_review = pages && pages->t == J_ARR && pages->len
        ? json_get(pages->kids[0], "needs_review") : NULL;
    int ok = review && review->t == J_BOOL && review->boolean &&
             page_review && page_review->t == J_BOOL && page_review->boolean &&
             text && text->t == J_STR &&
             strstr(text->str, "OCR recognized no readable text") != NULL;
    json_free(root); free(arena); free(result);
    atomic_store(&gateway->document_processing, 0);
    unsetenv("SAMOSA_DOCUMENT_CONTRACT_READER_MODE");
    unsetenv("SAMOSA_READ_CACHE_DIR");
    cleanup_contract_cache(cache_root, key);
    (void)unlink(input);
    return ok;
}

static int check_cancel_prevents_cache(Gateway *gateway, const char *self) {
    char input[PATH_MAX], cache_root[PATH_MAX], key[65];
    snprintf(input, sizeof(input), "/tmp/samosa-document-contract-cancel-%d.pdf",
             (int)getpid());
    snprintf(cache_root, sizeof(cache_root), "/tmp/samosa-document-contract-cancel-cache-%d",
             (int)getpid());
    if (!write_contract_fixture(input) || read_cache_key_file(input, key) != 0)
        return 0;
    setenv("SAMOSA_DOCUMENT_CONTRACT_READER_MODE", "cancel_cache", 1);
    setenv("SAMOSA_READ_CACHE_DIR", cache_root, 1);
    path_copy(gateway->home, sizeof(gateway->home), "/tmp");
    path_copy(gateway->samosa_extract, sizeof(gateway->samosa_extract), self);
    path_copy(gateway->samosa_ocr, sizeof(gateway->samosa_ocr), self);
    path_copy(gateway->reader_fingerprint, sizeof(gateway->reader_fingerprint),
              "contract-cancel-v1");
    atomic_store(&gateway->document_processing, 1);
    atomic_store(&gateway->document_cancel_requested, 0);
    DocumentReadProgress progress = {cancel_at_ocr_complete, gateway, "fixture.pdf"};
    char *result = doc_read_with_progress(gateway, input, NULL, &progress);
    char *cached = read_cache_get(cache_root, key, "reader-v3",
                                  gateway->reader_fingerprint);
    int ok = result && strstr(result, "\"error\":\"document_cancelled\"") &&
             cached == NULL;
    if (!ok)
        fprintf(stderr, "cancel-before-cache result=%s cached=%s cancel=%d\n",
                result ? result : "(null)", cached ? cached : "(null)",
                atomic_load(&gateway->document_cancel_requested));
    free(cached); free(result);
    atomic_store(&gateway->document_processing, 0);
    unsetenv("SAMOSA_DOCUMENT_CONTRACT_READER_MODE");
    unsetenv("SAMOSA_READ_CACHE_DIR");
    cleanup_contract_cache(cache_root, key);
    (void)unlink(input);
    return ok;
}

static int check_cancellation_reaps_child(Gateway *gateway, const char *self) {
    int status = 0;
    char *argv[] = {(char *)self, (char *)"child", NULL};
    pthread_t thread;
    char pid_path[PATH_MAX];
    snprintf(pid_path, sizeof(pid_path), "/tmp/samosa-document-contract-grandchild-%d.pid", (int)getpid());
    unlink(pid_path);
    setenv("SAMOSA_DOCUMENT_CONTRACT_GRANDCHILD_PID", pid_path, 1);
    atomic_store(&gateway->document_cancel_requested, 0);
    CancelChildContext context = {gateway, pid_path};
    if (pthread_create(&thread, NULL, cancel_child, &context)) return 0;
    long long started = monotonic_millis();
    char *output = run_capture_document(gateway, self, argv, 64, &status);
    long long elapsed = monotonic_millis() - started;
    pthread_join(thread, NULL);
    int grandchild = 0;
    FILE *pid_file = fopen(pid_path, "r");
    if (pid_file) {
        if (fscanf(pid_file, "%d", &grandchild) != 1) grandchild = 0;
        fclose(pid_file);
    }
    unlink(pid_path);
    unsetenv("SAMOSA_DOCUMENT_CONTRACT_GRANDCHILD_PID");
    int alive = 0;
    if (grandchild > 0) {
        for (int i = 0; i < 100 && !kill(grandchild, 0); ++i) usleep(10000);
        alive = !kill(grandchild, 0) || errno != ESRCH;
        if (alive) (void)kill(grandchild, SIGKILL);
    }
    free(output);
    return elapsed < 2000 && grandchild > 0 && !alive;
}

static int check_stale_render_path(Gateway *gateway, const char *self) {
    char input[PATH_MAX], legacy[PATH_MAX], cache[PATH_MAX], key[65];
    snprintf(input, sizeof(input), "/tmp/samosa-stale-render-%d.pdf", (int)getpid());
    snprintf(legacy, sizeof(legacy), "/tmp/doc_read_%d_p1.ppm", (int)getpid());
    snprintf(cache, sizeof(cache), "/tmp/samosa-stale-render-cache-%d", (int)getpid());
    if (!write_contract_fixture(input) || !write_contract_fixture(legacy) ||
        read_cache_key_file(input, key)) return 0;
    setenv("SAMOSA_DOCUMENT_CONTRACT_READER_MODE", "stale", 1);
    setenv("SAMOSA_READ_CACHE_DIR", cache, 1);
    path_copy(gateway->home, sizeof(gateway->home), "/tmp");
    path_copy(gateway->samosa_extract, sizeof(gateway->samosa_extract), self);
    path_copy(gateway->samosa_ocr, sizeof(gateway->samosa_ocr), self);
    atomic_store(&gateway->document_processing, 1);
    atomic_store(&gateway->document_cancel_requested, 0);
    char *result = doc_read_handler(gateway, input, NULL);
    int ok = result && strstr(result, "OCR SENTINEL") && access(legacy, F_OK) == 0;
    free(result);
    atomic_store(&gateway->document_processing, 0);
    unsetenv("SAMOSA_DOCUMENT_CONTRACT_READER_MODE");
    unsetenv("SAMOSA_READ_CACHE_DIR");
    cleanup_contract_cache(cache, key);
    unlink(input); unlink(legacy);
    return ok;
}

static int check_render_cancel_cleanup(Gateway *gateway, const char *self) {
    char input[PATH_MAX], marker[PATH_MAX], cache[PATH_MAX], key[65];
    snprintf(input, sizeof(input), "/tmp/samosa-render-cancel-%d.pdf", (int)getpid());
    snprintf(marker, sizeof(marker), "/tmp/samosa-render-cancel-%d.ready", (int)getpid());
    snprintf(cache, sizeof(cache), "/tmp/samosa-render-cancel-cache-%d", (int)getpid());
    if (!write_contract_fixture(input) || read_cache_key_file(input, key)) return 0;
    unlink(marker);
    setenv("SAMOSA_DOCUMENT_CONTRACT_READER_MODE", "cancel_render", 1);
    setenv("SAMOSA_RENDER_CANCEL_MARKER", marker, 1);
    setenv("SAMOSA_READ_CACHE_DIR", cache, 1);
    path_copy(gateway->home, sizeof(gateway->home), "/tmp");
    path_copy(gateway->samosa_extract, sizeof(gateway->samosa_extract), self);
    path_copy(gateway->samosa_ocr, sizeof(gateway->samosa_ocr), self);
    atomic_store(&gateway->document_processing, 1);
    atomic_store(&gateway->document_cancel_requested, 0);
    CancelChildContext context = {gateway, marker};
    pthread_t thread;
    if (pthread_create(&thread, NULL, cancel_child, &context)) return 0;
    char *result = doc_read_handler(gateway, input, NULL);
    pthread_join(thread, NULL);
    char rendered[PATH_MAX + 64] = {0};
    FILE *ready = fopen(marker, "r");
    if (ready) {
        if (!fgets(rendered, sizeof(rendered), ready)) rendered[0] = '\0';
        fclose(ready);
    }
    int ok = result && strstr(result, "document_cancelled") && rendered[0] &&
             access(rendered, F_OK) != 0;
    char *slash = strrchr(rendered, '/');
    if (slash) { *slash = 0; ok = ok && access(rendered, F_OK) != 0; }
    free(result);
    atomic_store(&gateway->document_processing, 0);
    atomic_store(&gateway->document_cancel_requested, 0);
    unsetenv("SAMOSA_DOCUMENT_CONTRACT_READER_MODE");
    unsetenv("SAMOSA_RENDER_CANCEL_MARKER");
    unsetenv("SAMOSA_READ_CACHE_DIR");
    cleanup_contract_cache(cache, key);
    unlink(input); unlink(marker);
    return ok;
}

int main(int argc, char **argv) {
    const char *reader_mode = getenv("SAMOSA_DOCUMENT_CONTRACT_READER_MODE");
    if (argc > 1 && reader_mode) {
        if (!strcmp(argv[1], "--json-pages")) {
            puts("{\"ok\":true,\"page_count\":1,\"pages\":[{\"index\":1,\"text_chars\":6,\"tokens\":1,\"has_raster_figure\":true,\"text\":\"native\",\"inspection\":{\"needs_ocr\":true,\"incomplete\":false,\"blank\":false,\"ocr_region\":false}}]}");
            return 0;
        }
        if (!strcmp(argv[1], "--render-ocr-ppm")) {
            int fd = open(argv[4], O_WRONLY | O_CREAT | O_EXCL, 0600);
            FILE *output = fd < 0 ? NULL : fdopen(fd, "wb");
            if (!output) return 2;
            fputs("P6\n1 1\n255\nxxx", output);
            if (fclose(output)) return 2;
            if (!strcmp(reader_mode, "cancel_render")) {
                const char *marker = getenv("SAMOSA_RENDER_CANCEL_MARKER");
                FILE *ready = marker ? fopen(marker, "w") : NULL;
                if (!ready) return 2;
                fputs(argv[4], ready); fclose(ready);
                usleep(3000000);
            }
            return 0;
        }
        if (!strcmp(argv[1], "read")) {
            puts(!strcmp(reader_mode, "empty")
                ? "{\"ok\":true,\"lines\":[]}"
                : "{\"ok\":true,\"lines\":[{\"text\":\"OCR SENTINEL\",\"conf\":0.99,\"bbox\":[0,0,1,1]}]}");
            return 0;
        }
    }
    if (argc > 1 && !strcmp(argv[1], "child")) {
        const char *pid_path = getenv("SAMOSA_DOCUMENT_CONTRACT_GRANDCHILD_PID");
        pid_t grandchild = fork();
        if (grandchild == 0) {
            signal(SIGTERM, SIG_IGN);
            if (pid_path) {
                FILE *pid_file = fopen(pid_path, "w");
                if (pid_file) { fprintf(pid_file, "%d\n", (int)getpid()); fclose(pid_file); }
            }
            close(STDOUT_FILENO);
            usleep(3000000);
            _Exit(0);
        }
        close(STDOUT_FILENO);
        usleep(3000000);
        return 0;
    }
    Gateway *gateway = calloc(1, sizeof(*gateway));
    if (!gateway) return 1;
    pthread_mutex_init(&gateway->mu, NULL);
    atomic_init(&gateway->document_cancel_requested, 0);
    if (argc == 6 && !strcmp(argv[1], "--real-pdf")) {
        path_copy(gateway->home, sizeof(gateway->home), argv[5]);
        path_copy(gateway->samosa_extract, sizeof(gateway->samosa_extract), argv[3]);
        path_copy(gateway->samosa_ocr, sizeof(gateway->samosa_ocr), argv[4]);
        atomic_store(&gateway->document_processing, 1);
        char *result = doc_read_handler(gateway, argv[2], NULL);
        if (result) puts(result);
        int success = result && strstr(result, "\"ok\":true");
        free(result); free(gateway);
        return success ? 0 : 1;
    }
    int ok = 1;
#define CHECK_CONTRACT(name, expression) do { \
        if (!(expression)) { fprintf(stderr, "test_document_reader_contract: %s failed\n", name); ok = 0; } \
    } while (0)
    CHECK_CONTRACT("capture terminator", check_capture_terminator(gateway));
    CHECK_CONTRACT("invalid OCR rejection", check_invalid_ocr_rejected());
    CHECK_CONTRACT("deep OCR rejection", check_deep_ocr_rejected());
    CHECK_CONTRACT("empty OCR review", check_empty_ocr_requires_review(gateway, argv[0]));
    CHECK_CONTRACT("cancel before cache", check_cancel_prevents_cache(gateway, argv[0]));
    CHECK_CONTRACT("descendant cancellation", check_cancellation_reaps_child(gateway, argv[0]));
    CHECK_CONTRACT("stale render path", check_stale_render_path(gateway, argv[0]));
    CHECK_CONTRACT("render cancellation cleanup", check_render_cancel_cleanup(gateway, argv[0]));
    char *failure = document_ocr_failure("{\"ok\":false,\"error\":\"image_invalid\"}", 65 << 8);
    CHECK_CONTRACT("OCR error preserved", failure && strstr(failure, "image_invalid"));
    free(failure);
    failure = document_ocr_failure(NULL, 23 << 8);
    CHECK_CONTRACT("OCR process failure distinguished", failure && strstr(failure, "ocr_process_failed"));
    free(failure);
    failure = document_ocr_failure(NULL, SIGXCPU);
    CHECK_CONTRACT("OCR timeout remains retryable", failure && strstr(failure, "ocr_timeout") && strstr(failure, "retryable"));
    free(failure);
#undef CHECK_CONTRACT
    pthread_mutex_destroy(&gateway->mu);
    free(gateway);
    if (!ok) {
        fprintf(stderr, "test_document_reader_contract: FAIL\n");
        return 1;
    }
    puts("test_document_reader_contract: PASS");
    return 0;
}
