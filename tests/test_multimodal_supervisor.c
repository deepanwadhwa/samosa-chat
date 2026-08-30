#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "samosa_multimodal.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    SamosaMmSession *session;
    int result;
    char error[96];
} HangingRequest;

static void *request_hang(void *opaque) {
    HangingRequest *request = opaque;
    char *reply = NULL; size_t reply_len = 0;
    request->result = samosa_mm_session_request(
        request->session, "{\"command\":\"hang\"}", 18,
        &reply, &reply_len, request->error, sizeof(request->error));
    free(reply);
    return NULL;
}

static SamosaMmProvider provider(const char *engine, const char *model,
                                 SamosaMmProtocol protocol) {
    SamosaMmProvider value = {
        .provider = protocol == SAMOSA_MM_PROTOCOL_JSON_LINE ? "fixture_line" : "fixture_frame",
        .executable = engine,
        .model_dir = model,
        .protocol = protocol,
        .ready_timeout_ms = 2000,
        .request_timeout_ms = 2000,
        .shutdown_grace_ms = 100,
        .max_frame_bytes = 4096
    };
    return value;
}

int main(int argc, char **argv) {
    assert(argc == 2 && argv[1][0] == '/');
    char model_template[] = "/tmp/samosa-mm-model-XXXXXX";
    char *model_dir = mkdtemp(model_template);
    assert(model_dir);

    SamosaMmSupervisor supervisor;
    assert(samosa_mm_supervisor_init(&supervisor));
    char error[96] = {0};

    unsetenv("SAMOSA_FAKE_MM_FRAMED");
    SamosaMmProvider line_provider = provider(argv[1], model_dir, SAMOSA_MM_PROTOCOL_JSON_LINE);
    SamosaMmSession line = {0};
    assert(samosa_mm_session_start(&supervisor, &line_provider, &line, error, sizeof(error)));
    assert(samosa_mm_supervisor_is_active(&supervisor, NULL, 0));

    SamosaMmSession refused = {0};
    assert(!samosa_mm_session_start(&supervisor, &line_provider, &refused, error, sizeof(error)));
    assert(!strcmp(error, "multimodal_specialist_busy"));

    char *reply = NULL; size_t reply_len = 0;
    const char *echo = "{\"command\":\"inspect\"}";
    assert(samosa_mm_session_request(&line, echo, strlen(echo), &reply, &reply_len,
                                     error, sizeof(error)));
    assert(reply_len == strlen(reply));
    assert(strstr(reply, "\"observation\":\"fixture"));
    free(reply);
    samosa_mm_session_close(&line);
    assert(!samosa_mm_supervisor_is_active(&supervisor, NULL, 0));

    setenv("SAMOSA_FAKE_MM_FRAMED", "1", 1);
    SamosaMmProvider frame_provider = provider(argv[1], model_dir, SAMOSA_MM_PROTOCOL_JSON_FRAME_V1);
    SamosaMmSession frame = {0};
    assert(samosa_mm_session_start(&supervisor, &frame_provider, &frame, error, sizeof(error)));
    const char *with_newline = "{\"command\":\"inspect\",\"prompt\":\"a\\nb\"}";
    assert(samosa_mm_session_request(&frame, with_newline, strlen(with_newline), &reply, &reply_len,
                                     error, sizeof(error)));
    free(reply);
    char oversized[5000]; memset(oversized, 'x', sizeof(oversized));
    assert(!samosa_mm_session_request(&frame, oversized, sizeof(oversized), &reply, &reply_len,
                                      error, sizeof(error)));
    assert(!strcmp(error, "multimodal_request_too_large"));
    samosa_mm_session_close(&frame);

    /* Cancellation must break a wedged request without waiting for its
       ordinary request timeout, after which close owns reap/lease release. */
    unsetenv("SAMOSA_FAKE_MM_FRAMED");
    SamosaMmSession hanging = {0};
    assert(samosa_mm_session_start(&supervisor, &line_provider, &hanging, error, sizeof(error)));
    HangingRequest request = {.session = &hanging};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, request_hang, &request) == 0);
    usleep(100000);
    assert(samosa_mm_supervisor_cancel(&supervisor));
    pthread_join(thread, NULL);
    assert(!request.result);
    samosa_mm_session_close(&hanging);
    assert(!samosa_mm_supervisor_is_active(&supervisor, NULL, 0));

    samosa_mm_supervisor_destroy(&supervisor);
    assert(rmdir(model_dir) == 0);
    puts("multimodal supervisor: PASS");
    return 0;
}
