/* Generated data only: exercise the real reader's stopping, cache isolation,
   Unicode budget, OCR and progress boundaries with a deterministic sidecar. */
#define main samosa_gateway_test_main
#include "../src/samosa_gateway.c"
#undef main

static int sample_sidecar(int argc, char **argv) {
    const char *mode = getenv("SAMOSA_SAMPLE_TEST_MODE");
    if (argc > 1 && !strcmp(argv[1], "--version")) { puts("sample-test;pdfium"); return 0; }
    if (!mode || argc < 2) return -1;
    const char *log_name = getenv("SAMOSA_SAMPLE_TEST_LOG");
    FILE *log = log_name ? fopen(log_name, "a") : NULL;
    if (log) { fprintf(log, "%s %s %s\n", argv[1], argc > 3 ? argv[3] : "", argc > 4 ? argv[4] : ""); fclose(log); }
    if (!strcmp(argv[1], "--render-ocr-ppm")) {
        FILE *f = fopen(argv[4], "w"); if (!f) return 2;
        fprintf(f, "%s", argv[3]); fclose(f); puts("{\"ok\":true}"); return 0;
    }
    if (!strcmp(argv[1], "read")) {
        int page = 1; FILE *f = fopen(argv[2], "r");
        if (f) { if (fscanf(f, "%d", &page) != 1) page = 1; fclose(f); }
        int lengths[] = {500, 900, 1700};
        int length = page <= 3 ? lengths[page - 1] : 3500;
        printf("{\"ok\":true,\"lines\":[{\"text\":\"");
        for (int i = 0; i < length; i++) putchar('o');
        puts("\",\"conf\":0.99,\"bbox\":[0,0,1,1]}]}"); return 0;
    }
    if (strcmp(argv[1], "--json-pages")) return -1;
    const char *delay = getenv("SAMOSA_SAMPLE_TEST_DELAY_MS");
    if (delay) sleep_millis(atoi(delay));
    int start = atoi(argv[3]), count = atoi(argv[4]);
    printf("{\"ok\":true,\"page_count\":20,\"pages\":[");
    for (int page = start; page < start + count && page <= 20; page++) {
        int lengths[] = {500, 900, 1700};
        int length = page <= 3 ? lengths[page - 1] : 3500;
        if (!strcmp(mode, "dense")) length = 6000;
        if (!strcmp(mode, "unicode")) length = 1200;
        if (!strcmp(mode, "blank") || !strcmp(mode, "ocr")) length = 0;
        if (page > start) putchar(',');
        printf("{\"index\":%d,\"text_chars\":%d,\"text\":\"", page, length);
        for (int i = 0; i < length; i++) fputs(!strcmp(mode, "unicode") ? "é" : "a", stdout);
        printf("\",\"inspection\":{\"needs_ocr\":%s,\"blank\":%s}}",
            !strcmp(mode, "ocr") ? "true" : "false", !strcmp(mode, "blank") ? "true" : "false");
    }
    puts("]}"); return 0;
}

typedef struct { int pages, starts; size_t characters; } SampleEvents;
static int sample_progress(void *opaque, const char *filename, const char *stage, const char *message) {
    (void)filename;
    SampleEvents *events = opaque;
    if (!strcmp(stage, "page_started")) events->starts++;
    if (!strcmp(stage, "page_complete")) {
        char *arena = NULL; jval *event = json_parse(message, &arena);
        jval *chars = event ? json_get(event, "characters") : NULL;
        if (chars && chars->t == J_NUM) events->characters += (size_t)chars->num;
        events->pages++; json_free(event); free(arena);
    }
    return 1;
}

static int check_sample(Gateway *g, const char *directory, const char *mode, int expected_pages, size_t expected_chars) {
    char input[PATH_MAX], log[PATH_MAX], cache[PATH_MAX], key[65];
    snprintf(input, sizeof(input), "%s/%s.pdf", directory, mode);
    snprintf(log, sizeof(log), "%s/%s.calls", directory, mode);
    snprintf(cache, sizeof(cache), "%s/cache-%s", directory, mode);
    FILE *f = fopen(input, "w"); if (!f) return 0;
    fputs("generated fixture", f); fclose(f);
    setenv("SAMOSA_READ_CACHE_DIR", cache, 1);
    setenv("SAMOSA_SAMPLE_TEST_MODE", mode, 1);
    setenv("SAMOSA_SAMPLE_TEST_LOG", log, 1);
    SampleEvents events = {0}; DocumentReadProgress progress = {sample_progress, &events, input};
    char *arena = NULL; jval *args = json_parse("{\"detail\":\"lines\",\"sample_characters\":3000}", &arena);
    char *raw = doc_read_with_progress(g, input, args, &progress);
    char *result_arena = NULL; jval *result = raw ? json_parse(raw, &result_arena) : NULL;
    jval *pages = result ? json_get(result, "pages") : NULL;
    jval *chars = result ? json_get(result, "characters_read") : NULL;
    jval *sampled = result ? json_get(result, "sampled") : NULL;
    jval *reason = result ? json_get(result, "stop_reason") : NULL;
    int ok = pages && pages->t == J_ARR && pages->len == expected_pages &&
        chars && chars->t == J_NUM && chars->num == expected_chars &&
        sampled && sampled->t == J_BOOL && sampled->boolean &&
        reason && reason->t == J_STR && !strcmp(reason->str, !strcmp(mode, "blank") ? "page_guard" : "character_target") &&
        events.pages == expected_pages && events.starts == expected_pages && events.characters == expected_chars;
    size_t content = 0;
    for (int i = 0; pages && pages->t == J_ARR && i < pages->len; i++) content += document_page_character_count(pages->kids[i]);
    ok = ok && content == expected_chars;
    if (read_cache_key_file(input, key)) ok = 0;
    char *full = read_cache_get(cache, key, "reader-v3", reader_fingerprint(g));
    ok = ok && !full; free(full);
    char *calls = read_file_limit(log, 65536);
    int extraction_calls = 0, ocr_calls = 0;
    for (const char *p = calls; p && (p = strstr(p, "--json-pages")); p++) extraction_calls++;
    for (const char *p = calls; p && (p = strstr(p, "read ")); p++) ocr_calls++;
    ok = ok && extraction_calls == expected_pages && ocr_calls == (!strcmp(mode, "ocr") ? expected_pages : 0);
    free(calls);
    /* Warm sampling must reuse exactly the sampled cache, with the same
       progress units, and never publish a sample under the complete contract. */
    events = (SampleEvents){0};
    char *warm = doc_read_with_progress(g, input, args, &progress);
    ok = ok && warm && !strcmp(warm, raw) && events.pages == expected_pages && events.characters == expected_chars;
    if (!ok) fprintf(stderr, "sample %s failed: %s\n", mode, raw ? raw : "null");
    free(warm); free(raw); json_free(result); free(result_arena); json_free(args); free(arena);
    return ok;
}

static int check_package_read_guard(Gateway *g) {
    const char *excluded[] = {
        "/generated/Editor.app/Contents/Resources/file.json",
        "/generated/UPPER.APP/file.pdf", "/generated/Resources.bundle/a.txt",
        "/generated/Runtime.framework/a.txt", "/generated/Extension.plugin/a.txt",
        "/generated/Project.xcodeproj/a.txt", "/generated/Workspace.xcworkspace/a.txt",
        "/generated/Pictures.photoslibrary/a.txt", NULL
    };
    for (const char **path = excluded; *path; path++) {
        if (!chutni_package_path_excluded(*path)) return 0;
        ChutniEnrichmentCounts counts = {0};
        /* These paths do not exist. A reader would fail; the guard instead
           records exclusion without touching a reader or source identity. */
        chutni_enrich_source(g, "/unused", *path, "application/pdf", "test", 500, &counts, NULL);
        if (counts.metadata_files != 1 || counts.failed || counts.derived || counts.model || counts.summaries) return 0;
    }
    return !chutni_package_path_excluded("/generated/ordinary/package.json") &&
        !chutni_package_path_excluded("/generated/project.app.notes/report.txt") &&
        !chutni_package_path_excluded("/generated/application/report.txt");
}

static int check_cancel_recovery(Gateway *g, const char *directory) {
    const char *scope = "33333333333333333333333333333333";
    const char *job = "44444444444444444444444444444444";
    char scopes[PATH_MAX], scope_dir[PATH_MAX], recovered_job[96], state[32];
    unsigned long long generation = 0;
    if (!path_join(g->chutni_root, sizeof(g->chutni_root), directory, "cancel-recovery") ||
        !path_join(scopes, sizeof(scopes), g->chutni_root, "scopes") ||
        !path_join(scope_dir, sizeof(scope_dir), scopes, scope) || !mkdirs(scope_dir) ||
        !chutni_job_write(g, scope, job, "canceling", "extract", 1, "Generated cancel request")) return 0;
    chutni_repair_after_restart(g);
    if (!chutni_job_load(g, scope, recovered_job, state, &generation) || strcmp(state, "canceled")) return 0;
    if (!chutni_job_write(g, scope, job, "running", "extract", 2, "Generated interrupted run")) return 0;
    chutni_repair_after_restart(g);
    return chutni_job_load(g, scope, recovered_job, state, &generation) && !strcmp(state, "paused_user");
}

static int check_text_samples(const char *directory) {
    char input[PATH_MAX]; snprintf(input, sizeof(input), "%s/character-samples.txt", directory);
    const char *units[] = {"a", "é", "😀"};
    for (int test = 0; test < 3; test++) {
        FILE *f = fopen(input, "w"); if (!f) return 0;
        for (int i = 0; i < 4000; i++) fputs(units[test], f);
        fputs("UNREAD_TAIL", f); fclose(f);
        char *sample = chutni_read_character_sample(input, 3000);
        int ok = sample && document_character_count(sample) == 3000 &&
            strlen(sample) == 3000 * strlen(units[test]) && !strstr(sample, "UNREAD_TAIL");
        free(sample); if (!ok) return 0;
    }
    FILE *f = fopen(input, "w"); if (!f) return 0;
    for (int i = 0; i < 2999; i++) fputc('a', f);
    fputs("😀UNREAD_TAIL", f); fclose(f);
    char *sample = chutni_read_character_sample(input, 3000);
    int ok = sample && strlen(sample) == 3003 && !strstr(sample, "UNREAD_TAIL"); free(sample);
    /* Invalid continuation-only data must not overflow the sample buffer. */
    f = fopen(input, "w"); if (!f) return 0;
    for (int i = 0; i < 20000; i++) fputc(0x80, f);
    fclose(f);
    sample = chutni_read_character_sample(input, 3000); ok = ok && !sample; free(sample);
    return ok;
}

int main(int argc, char **argv) {
    int sidecar = sample_sidecar(argc, argv); if (sidecar >= 0) return sidecar;
    if (argc == 4 && !strcmp(argv[1], "--real-enrich")) {
        Gateway *g = calloc(1, sizeof(*g)); if (!g || !load_config(g)) return 1;
        char progress_dir[PATH_MAX];
        const char *scope = "11111111111111111111111111111111";
        snprintf(progress_dir, sizeof(progress_dir), "%s/scopes/%s", g->chutni_root, scope);
        if (!mkdirs(progress_dir)) return 1;
        ChutniBuildProgress progress = {0};
        progress.started_ms = progress.enrichment_started_ms = wall_millis();
        path_copy(progress.phase, sizeof(progress.phase), "extract");
        ChutniEnrichmentCounts counts = chutni_enrich_store(g, argv[3], argv[2], "real-sampling-test", scope, "generated-test", &progress, 500);
        progress.enrichment = counts;
        path_copy(progress.phase, sizeof(progress.phase), "complete");
        progress.current_file[0] = 0;
        chutni_progress_write(g, scope, "generated-test", &progress, 1);
        printf("{\"files\":%llu,\"summaries\":%llu,\"sampled_files\":%llu,\"full_files\":%llu,\"failures\":%llu,\"seconds\":%.3f,\"extraction_ms\":%llu,\"ocr_ms\":%llu,\"summary_ms\":%llu,\"storage_ms\":%llu,\"verification_ms\":%llu,\"commit_ms\":%llu}\n",
            counts.files_processed, counts.summaries, counts.sampled_files, counts.complete_files, counts.failed,
            (wall_millis() - progress.started_ms) / 1000.0,
            counts.extraction_ms, counts.ocr_ms, counts.summary_ms, counts.storage_ms, counts.verification_ms, counts.commit_ms);
        summarizer_stop(g); free(g);
        return counts.failed ? 1 : 0;
    }
    if (argc == 6 && !strcmp(argv[1], "--real-sample")) {
        Gateway *g = calloc(1, sizeof(*g)); if (!g) return 1;
        pthread_mutex_init(&g->mu, NULL);
        path_copy(g->home, sizeof(g->home), argv[5]);
        path_copy(g->samosa_extract, sizeof(g->samosa_extract), argv[3]);
        path_copy(g->samosa_ocr, sizeof(g->samosa_ocr), argv[4]);
        atomic_store(&g->document_processing, 1);
        char *arena = NULL; jval *args = json_parse("{\"detail\":\"lines\",\"sample_characters\":3000}", &arena);
        SampleEvents events = {0}; DocumentReadProgress progress = {sample_progress, &events, argv[2]};
        char *raw = doc_read_with_progress(g, argv[2], args, &progress);
        if (raw) puts(raw);
        int ok = raw && strstr(raw, "\"ok\":true");
        free(raw); json_free(args); free(arena); pthread_mutex_destroy(&g->mu); free(g);
        return ok ? 0 : 1;
    }
    char directory[] = "/tmp/samosa-opening-sample.XXXXXX";
    if (!mkdtemp(directory)) return 1;
    Gateway *g = calloc(1, sizeof(*g)); if (!g) return 1;
    pthread_mutex_init(&g->mu, NULL);
    path_copy(g->home, sizeof(g->home), directory);
    path_copy(g->samosa_extract, sizeof(g->samosa_extract), argv[0]);
    path_copy(g->samosa_ocr, sizeof(g->samosa_ocr), argv[0]);
    path_copy(g->reader_fingerprint, sizeof(g->reader_fingerprint), "opening-sample-test-v1");
    atomic_store(&g->document_processing, 1);
    int ok = check_sample(g, directory, "short", 3, 3100);
    ok = check_package_read_guard(g) && ok;
    ok = check_cancel_recovery(g, directory) && ok;
    ok = check_text_samples(directory) && ok;
    ok = check_sample(g, directory, "dense", 1, 6000) && ok;
    ok = check_sample(g, directory, "unicode", 3, 3600) && ok;
    ok = check_sample(g, directory, "ocr", 3, 3100) && ok;
    ok = check_sample(g, directory, "blank", 12, 0) && ok;
    unsetenv("SAMOSA_SAMPLE_TEST_MODE"); unsetenv("SAMOSA_SAMPLE_TEST_LOG"); unsetenv("SAMOSA_READ_CACHE_DIR");
    pthread_mutex_destroy(&g->mu); free(g);
    printf("test_chutni_sampling: %s (generated fixtures: %s)\n", ok ? "PASS" : "FAIL", directory);
    return ok ? 0 : 1;
}
