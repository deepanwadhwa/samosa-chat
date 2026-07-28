/* Gateway-owned supervisor for the private GTK1/v2 adapter protocol.
 *
 * This header is intentionally independent of the HTTP gateway so it can be
 * tested as a small process boundary before T4.4 wires it into Chutni.  The
 * gateway, never the browser, supplies the immutable model/tokenizer policy.
 */
#ifndef SAMOSA_GIGATOKEN_H
#define SAMOSA_GIGATOKEN_H

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define SAMOSA_GIGATOKEN_MAGIC 0x31544B47u
#define SAMOSA_GIGATOKEN_VERSION 2u
#define SAMOSA_GIGATOKEN_MAX_FRAME (8u * 1024u * 1024u)
#define SAMOSA_GIGATOKEN_MAX_TEXT (2u * 1024u * 1024u)

enum {
    SAMOSA_GIGATOKEN_HEALTH = 1,
    SAMOSA_GIGATOKEN_ENCODE_BATCH = 2,
    SAMOSA_GIGATOKEN_ENCODE_PROMPT = 3,
    SAMOSA_GIGATOKEN_CANCEL = 4,
    SAMOSA_GIGATOKEN_SHUTDOWN = 5,
    SAMOSA_GIGATOKEN_RESULT = 0x8001,
    SAMOSA_GIGATOKEN_ERROR = 0x8002
};

typedef struct {
    pid_t pid;
    int input_fd;
    int output_fd;
    pthread_mutex_t write_mu;
    pthread_mutex_t read_mu;
    int running;
    uint64_t next_request_id;
    char adapter_path[PATH_MAX];
    char tokenizer_path[PATH_MAX];
    char build_commit[129];
    char model_id[129];
    char model_version[129];
    unsigned char tokenizer_sha256[32];
    uint32_t vocab_size;
    unsigned char policy_fingerprint[1024];
    uint16_t policy_len;
    uint16_t last_error_code;
    int last_error_retryable;
    uint64_t last_error_request_id;
    char last_error[256];
} SamosaGigatoken;

static void samosa_gigatoken_error(SamosaGigatoken *g, uint16_t code, int retryable,
                                   uint64_t request_id, const char *message) {
    g->last_error_code = code;
    g->last_error_retryable = retryable;
    g->last_error_request_id = request_id;
    snprintf(g->last_error, sizeof(g->last_error), "%s", message ? message : "adapter error");
}

static int samosa_gigatoken_write_all(int fd, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n; len -= (size_t)n;
    }
    return 1;
}

static int samosa_gigatoken_poll_read(int fd, void *data, size_t len, int timeout_ms) {
    unsigned char *p = (unsigned char *)data;
    while (len) {
        struct pollfd item = {.fd = fd, .events = POLLIN};
        int ready = poll(&item, 1, timeout_ms);
        if (ready <= 0 || !(item.revents & (POLLIN | POLLHUP))) return 0;
        ssize_t n = read(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n; len -= (size_t)n;
    }
    return 1;
}

static void samosa_gigatoken_close_fds(SamosaGigatoken *g) {
    if (g->input_fd >= 0) close(g->input_fd);
    if (g->output_fd >= 0) close(g->output_fd);
    g->input_fd = g->output_fd = -1;
}

static void samosa_gigatoken_kill_child(SamosaGigatoken *g) {
    if (g->pid > 0) {
        kill(g->pid, SIGTERM);
        for (int i = 0; i < 20; ++i) {
            int status = 0;
            pid_t done = waitpid(g->pid, &status, WNOHANG);
            if (done == g->pid) { g->pid = 0; break; }
            if (done < 0 && errno == ECHILD) { g->pid = 0; break; }
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 50000000L};
            nanosleep(&pause, NULL);
        }
        if (g->pid > 0) { kill(g->pid, SIGKILL); waitpid(g->pid, NULL, 0); g->pid = 0; }
    }
    samosa_gigatoken_close_fds(g);
    g->running = 0;
}

static int samosa_gigatoken_start(SamosaGigatoken *g, const char *adapter_path,
                                  const char *tokenizer_path, const char *build_commit,
                                  const char *model_id, const char *model_version,
                                  const unsigned char tokenizer_sha256[32], uint32_t vocab_size,
                                  const unsigned char *policy, size_t policy_len) {
    int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1};
    memset(g, 0, sizeof(*g)); g->input_fd = g->output_fd = -1;
    if (!adapter_path || !tokenizer_path || !build_commit || !model_id || !model_version ||
        strlen(adapter_path) >= sizeof(g->adapter_path) || strlen(tokenizer_path) >= sizeof(g->tokenizer_path) ||
        strlen(build_commit) >= sizeof(g->build_commit) || strlen(model_id) >= sizeof(g->model_id) ||
        strlen(model_version) >= sizeof(g->model_version) || policy_len > sizeof(g->policy_fingerprint)) return 0;
    snprintf(g->adapter_path, sizeof(g->adapter_path), "%s", adapter_path);
    snprintf(g->tokenizer_path, sizeof(g->tokenizer_path), "%s", tokenizer_path);
    snprintf(g->build_commit, sizeof(g->build_commit), "%s", build_commit);
    snprintf(g->model_id, sizeof(g->model_id), "%s", model_id);
    snprintf(g->model_version, sizeof(g->model_version), "%s", model_version);
    memcpy(g->tokenizer_sha256, tokenizer_sha256, 32); g->vocab_size = vocab_size;
    memcpy(g->policy_fingerprint, policy, policy_len); g->policy_len = (uint16_t)policy_len;
    pthread_mutex_init(&g->write_mu, NULL); pthread_mutex_init(&g->read_mu, NULL);
    if (pipe(in_pipe) || pipe(out_pipe)) { if (in_pipe[0] >= 0) close(in_pipe[0]); if (in_pipe[1] >= 0) close(in_pipe[1]); return 0; }
    g->pid = fork();
    if (g->pid < 0) { close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]); return 0; }
    if (!g->pid) {
        close(in_pipe[1]); close(out_pipe[0]);
        dup2(in_pipe[0], STDIN_FILENO); dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]); close(out_pipe[1]);
        setpriority(PRIO_PROCESS, 0, 10);
        char *const argv[] = {(char *)adapter_path, (char *)tokenizer_path, NULL};
        execv(adapter_path, argv); _Exit(127);
    }
    close(in_pipe[0]); close(out_pipe[1]);
    g->input_fd = in_pipe[1]; g->output_fd = out_pipe[0]; g->running = 1; g->next_request_id = 1;
    return 1;
}

static int samosa_gigatoken_meta(SamosaGigatoken *g, unsigned char *out, size_t cap,
                                  uint64_t request_id, const unsigned char source_sha256[32],
                                  uint32_t item_count, uint64_t input_bytes,
                                  uint64_t max_tokens, uint64_t max_output_bytes, size_t *used) {
    size_t p = 0; size_t build_len = strlen(g->build_commit), model_len = strlen(g->model_id), version_len = strlen(g->model_version);
#define PUT_BYTES(ptr, n) do { if ((n) > cap - p) return 0; memcpy(out + p, (ptr), (n)); p += (n); } while (0)
#define PUT_U16(value) do { uint16_t v = (uint16_t)(value); PUT_BYTES(&v, sizeof(v)); } while (0)
#define PUT_U32(value) do { uint32_t v = (uint32_t)(value); PUT_BYTES(&v, sizeof(v)); } while (0)
#define PUT_U64(value) do { uint64_t v = (uint64_t)(value); PUT_BYTES(&v, sizeof(v)); } while (0)
    if (build_len > UINT16_MAX || model_len > UINT16_MAX || version_len > UINT16_MAX) return 0;
    PUT_U64(request_id); PUT_U16(build_len); PUT_BYTES(g->build_commit, build_len);
    PUT_U16(model_len); PUT_BYTES(g->model_id, model_len); PUT_U16(version_len); PUT_BYTES(g->model_version, version_len);
    PUT_BYTES(g->tokenizer_sha256, 32); PUT_U32(g->vocab_size); PUT_U16(g->policy_len); PUT_BYTES(g->policy_fingerprint, g->policy_len);
    PUT_BYTES(source_sha256, 32); PUT_U32(item_count); PUT_U64(input_bytes); PUT_U64(max_tokens); PUT_U64(max_output_bytes);
    *used = p;
#undef PUT_BYTES
#undef PUT_U16
#undef PUT_U32
#undef PUT_U64
    return 1;
}

static int samosa_gigatoken_send(SamosaGigatoken *g, uint16_t op, const unsigned char *payload, size_t len) {
    if (!g->running || len > SAMOSA_GIGATOKEN_MAX_FRAME) return 0;
    unsigned char header[12]; uint32_t magic = SAMOSA_GIGATOKEN_MAGIC, version = SAMOSA_GIGATOKEN_VERSION, length = (uint32_t)len;
    memcpy(header, &magic, 4); memcpy(header + 4, &version, 2); memcpy(header + 6, &op, 2); memcpy(header + 8, &length, 4);
    pthread_mutex_lock(&g->write_mu);
    int ok = samosa_gigatoken_write_all(g->input_fd, header, sizeof(header)) && samosa_gigatoken_write_all(g->input_fd, payload, len);
    pthread_mutex_unlock(&g->write_mu);
    return ok;
}

/* Reads the response belonging to request_id. A concurrent cancel response is
 * skipped by the active request reader; only one thread may read responses. */
static int samosa_gigatoken_request(SamosaGigatoken *g, uint16_t op, const unsigned char *body,
                                    size_t body_len, uint64_t request_id, const unsigned char source_sha256[32],
                                    uint32_t item_count, uint64_t input_bytes, uint64_t max_tokens,
                                    int timeout_ms, unsigned char **response, size_t *response_len, uint16_t *response_op) {
    unsigned char *payload = malloc(SAMOSA_GIGATOKEN_MAX_FRAME); size_t meta_len;
    if (!payload || body_len > SAMOSA_GIGATOKEN_MAX_FRAME || !samosa_gigatoken_meta(g, payload, SAMOSA_GIGATOKEN_MAX_FRAME, request_id, source_sha256,
                                                               item_count, input_bytes, max_tokens,
                                                               SAMOSA_GIGATOKEN_MAX_FRAME - 24, &meta_len) ||
        body_len > SAMOSA_GIGATOKEN_MAX_FRAME - meta_len) { free(payload); return 0; }
    memcpy(payload + meta_len, body, body_len);
    pthread_mutex_lock(&g->read_mu);
    int ok = samosa_gigatoken_send(g, op, payload, meta_len + body_len);
    free(payload);
    while (ok) {
        unsigned char header[12];
        if (!samosa_gigatoken_poll_read(g->output_fd, header, sizeof(header), timeout_ms)) { ok = 0; break; }
        uint32_t magic, version, length; uint16_t got_op;
        memcpy(&magic, header, 4); memcpy(&version, header + 4, 2); memcpy(&got_op, header + 6, 2); memcpy(&length, header + 8, 4);
        if (magic != SAMOSA_GIGATOKEN_MAGIC || version != SAMOSA_GIGATOKEN_VERSION || length > SAMOSA_GIGATOKEN_MAX_FRAME) { ok = 0; break; }
        unsigned char *got = malloc(length ? length : 1);
        if (!got || !samosa_gigatoken_poll_read(g->output_fd, got, length, timeout_ms)) { free(got); ok = 0; break; }
        uint64_t got_request = length >= 8 ? *(uint64_t *)got : 0;
        if (got_request != request_id) { free(got); continue; }
        *response = got; *response_len = length; *response_op = got_op;
        if (got_op == SAMOSA_GIGATOKEN_ERROR && length >= 16) {
            uint16_t code = *(uint16_t *)(got + 8); uint16_t retryable = *(uint16_t *)(got + 10); uint32_t detail_len = *(uint32_t *)(got + 12);
            if (detail_len > length - 16) detail_len = (uint32_t)(length - 16);
            char detail[256]; size_t n = detail_len < sizeof(detail) - 1 ? detail_len : sizeof(detail) - 1;
            memcpy(detail, got + 16, n); detail[n] = 0; samosa_gigatoken_error(g, code, retryable != 0, request_id, detail);
        }
        break;
    }
    pthread_mutex_unlock(&g->read_mu);
    if (!ok) { samosa_gigatoken_error(g, 0, 1, request_id, "adapter timeout, pipe failure, or protocol error"); samosa_gigatoken_kill_child(g); }
    return ok;
}

static int samosa_gigatoken_cancel(SamosaGigatoken *g, uint64_t target_request_id) {
    unsigned char payload[512]; size_t meta_len; unsigned char zero_sha[32] = {0};
    if (!samosa_gigatoken_meta(g, payload, sizeof(payload), g->next_request_id++, zero_sha, 0, 0, 0, 0, &meta_len)) return 0;
    if (meta_len + 8 > sizeof(payload)) return 0; memcpy(payload + meta_len, &target_request_id, 8);
    return samosa_gigatoken_send(g, SAMOSA_GIGATOKEN_CANCEL, payload, meta_len + 8);
}

static int samosa_gigatoken_shutdown(SamosaGigatoken *g, int timeout_ms) {
    unsigned char zero_sha[32] = {0}, *response = NULL; size_t len = 0; uint16_t op = 0;
    uint64_t id = g->next_request_id++;
    int ok = samosa_gigatoken_request(g, SAMOSA_GIGATOKEN_SHUTDOWN, NULL, 0, id, zero_sha, 0, 0, 0, timeout_ms, &response, &len, &op);
    free(response); if (!ok) samosa_gigatoken_kill_child(g); else samosa_gigatoken_kill_child(g);
    pthread_mutex_destroy(&g->write_mu); pthread_mutex_destroy(&g->read_mu); return ok && op == SAMOSA_GIGATOKEN_RESULT;
}

static int samosa_gigatoken_health(SamosaGigatoken *g, int timeout_ms) {
    unsigned char zero_sha[32] = {0}, *response = NULL; size_t len = 0; uint16_t op = 0;
    uint64_t id = g->next_request_id++;
    int ok = samosa_gigatoken_request(g, SAMOSA_GIGATOKEN_HEALTH, NULL, 0, id, zero_sha, 0, 0, 0, timeout_ms, &response, &len, &op);
    free(response); return ok && op == SAMOSA_GIGATOKEN_RESULT;
}

#endif
