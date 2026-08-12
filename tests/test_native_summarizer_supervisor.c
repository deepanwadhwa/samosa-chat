#define main samosa_gateway_program_main
#include "../src/samosa_gateway.c"
#undef main

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    Gateway gateway;
    memset(&gateway, 0, sizeof(gateway));
    pthread_mutex_init(&gateway.summarizer_mu, NULL);
    gateway.summarizer_write_fd = -1;
    gateway.summarizer_read_fd = -1;
    if (!path_copy(gateway.summarizer_engine, sizeof(gateway.summarizer_engine), argv[1]) ||
        !path_copy(gateway.summarizer_model, sizeof(gateway.summarizer_model), argv[2]))
        return 3;
    char log_path[] = "/tmp/samosa-summary-supervisor.XXXXXX";
    int log_fd = mkstemp(log_path);
    if (log_fd < 0) return 4;
    close(log_fd);
    path_copy(gateway.summarizer_log, sizeof(gateway.summarizer_log), log_path);
    setenv("SAMOSA_FAKE_SUMMARIZER_LOG", log_path, 1);

    const char *source =
        "South Carolina DPH confirms 30 cases in 2026 and provides produce washing and symptom guidance.";
    pid_t resident_pid = 0;
    for (int i = 0; i < 8; ++i) {
        char *summary = native_summarize_once(&gateway, source, strlen(source));
        if (!summary || !strstr(summary, "30") || !strstr(summary, "cases")) {
            fprintf(stderr, "summary request %d failed: %s (pid=%ld, log=%s)\n",
                    i + 1, summary ? summary : "<none>",
                    (long)gateway.summarizer_pid, log_path);
            free(summary); summarizer_stop(&gateway); return 5;
        }
        free(summary);
        if (!resident_pid) resident_pid = gateway.summarizer_pid;
        if (gateway.summarizer_pid != resident_pid) {
            summarizer_stop(&gateway); unlink(log_path); return 6;
        }
    }

    char long_source[3900] = {0};
    const char *sentence =
        "A bounded article segment contains factual source material for the local summary. ";
    while (strlen(long_source) + strlen(sentence) + 1 < sizeof(long_source))
        strcat(long_source, sentence);
    TextBuffer reduced = {0};
    int mapped = native_summarize_text(
        &gateway, long_source, strlen(long_source), 3, 300, &reduced);
    if (mapped != 3 || !reduced.data || !reduced.len || reduced.len > 300 ||
        gateway.summarizer_pid != resident_pid) {
        fprintf(stderr, "native map-reduce failed: mapped=%d bytes=%zu\n",
                mapped, reduced.len);
        free(reduced.data); summarizer_stop(&gateway); unlink(log_path); return 7;
    }
    free(reduced.data);
    summarizer_stop(&gateway);
    pthread_mutex_destroy(&gateway.summarizer_mu);

    FILE *log = fopen(log_path, "r");
    long first = 0, value = 0;
    int lines = 0, one_process = 1;
    while (log && fscanf(log, "%ld", &value) == 1) {
        if (!first) first = value;
        if (value != first) one_process = 0;
        lines++;
    }
    if (log) fclose(log);
    unlink(log_path);
    unsetenv("SAMOSA_FAKE_SUMMARIZER_LOG");
    /* Eight direct requests, three map chunks, and one local reduce pass all
       use the same resident sidecar process. */
    if (lines != 12 || !one_process) return 8;
    puts("test_native_summarizer_supervisor: PASS");
    return 0;
}
