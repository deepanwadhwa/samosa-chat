#define _POSIX_C_SOURCE 200809L

#include "samosa_multimodal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void mm_error(char *out, size_t cap, const char *code) {
    if (!out || !cap) return;
    snprintf(out, cap, "%s", code ? code : "multimodal_error");
}

static long long mm_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int mm_regular_executable(const char *path) {
    struct stat st;
    return path && path[0] == '/' && lstat(path, &st) == 0 &&
           S_ISREG(st.st_mode) && !(st.st_mode & S_IWOTH) &&
           access(path, X_OK) == 0;
}

static int mm_directory_no_symlink(const char *path) {
    struct stat st;
    return path && path[0] == '/' && lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int mm_set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static int mm_wait_fd(int fd, short events, long long deadline) {
    for (;;) {
        long long left = deadline - mm_now_ms();
        if (left <= 0) return 0;
        struct pollfd pfd = {.fd = fd, .events = events};
        int wait_ms = left > INT_MAX ? INT_MAX : (int)left;
        int rc = poll(&pfd, 1, wait_ms);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) return 0;
        if (pfd.revents & (POLLERR | POLLNVAL)) return 0;
        if (pfd.revents & events) return 1;
        if ((events & POLLIN) && (pfd.revents & POLLHUP)) return 1;
    }
}

static int mm_write_deadline(int fd, const void *bytes_, size_t len,
                             long long deadline) {
    const unsigned char *bytes = (const unsigned char *)bytes_;
    size_t at = 0;
    while (at < len) {
        if (!mm_wait_fd(fd, POLLOUT, deadline)) return 0;
        ssize_t n = write(fd, bytes + at, len - at);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return 0;
        at += (size_t)n;
    }
    return 1;
}

static int mm_read_exact_deadline(int fd, void *bytes_, size_t len,
                                  long long deadline) {
    unsigned char *bytes = (unsigned char *)bytes_;
    size_t at = 0;
    while (at < len) {
        if (!mm_wait_fd(fd, POLLIN, deadline)) return 0;
        ssize_t n = read(fd, bytes + at, len - at);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return 0;
        at += (size_t)n;
    }
    return 1;
}

static int mm_send(SamosaMmSession *session, const char *json, size_t len,
                   int timeout_ms) {
    long long deadline = mm_now_ms() + timeout_ms;
    if (session->protocol == SAMOSA_MM_PROTOCOL_JSON_FRAME_V1) {
        if (len == 0 || len > UINT32_MAX || len > session->max_frame_bytes) return 0;
        uint32_t encoded = htonl((uint32_t)len);
        return mm_write_deadline(session->in_fd, &encoded, sizeof(encoded), deadline) &&
               mm_write_deadline(session->in_fd, json, len, deadline);
    }
    if (!len || len + 1 > session->max_frame_bytes || memchr(json, '\n', len)) return 0;
    return mm_write_deadline(session->in_fd, json, len, deadline) &&
           mm_write_deadline(session->in_fd, "\n", 1, deadline);
}

static int mm_receive(SamosaMmSession *session, char **reply, size_t *reply_len,
                      int timeout_ms) {
    long long deadline = mm_now_ms() + timeout_ms;
    size_t len = 0;
    char *result = NULL;
    if (session->protocol == SAMOSA_MM_PROTOCOL_JSON_FRAME_V1) {
        uint32_t encoded = 0;
        if (!mm_read_exact_deadline(session->out_fd, &encoded, sizeof(encoded), deadline)) return 0;
        len = ntohl(encoded);
        if (!len || len > session->max_frame_bytes) return 0;
        result = (char *)malloc(len + 1);
        if (!result) return 0;
        if (!mm_read_exact_deadline(session->out_fd, result, len, deadline)) {
            free(result);
            return 0;
        }
    } else {
        size_t cap = session->max_frame_bytes;
        result = (char *)malloc(cap + 1);
        if (!result) return 0;
        while (len < cap) {
            if (!mm_wait_fd(session->out_fd, POLLIN, deadline)) {
                free(result);
                return 0;
            }
            unsigned char c = 0;
            ssize_t n = read(session->out_fd, &c, 1);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (n <= 0) { free(result); return 0; }
            if (c == '\n') break;
            result[len++] = (char)c;
        }
        if (len == cap) { free(result); return 0; }
    }
    result[len] = 0;
    *reply = result;
    if (reply_len) *reply_len = len;
    return 1;
}

static int mm_reply_ok(const char *json, size_t len) {
    /* Handshake replies are helper-owned, bounded JSON.  Full request replies
       are parsed by the gateway.  Keep this check deliberately strict enough
       not to accept an error containing the word "ok" in a message. */
    static const char needle[] = "\"status\":\"ok\"";
    if (!json || len < sizeof(needle) - 1) return 0;
    for (size_t i = 0; i + sizeof(needle) - 1 <= len; ++i)
        if (!memcmp(json + i, needle, sizeof(needle) - 1)) return 1;
    return 0;
}

static void mm_supervisor_release(SamosaMmSession *session) {
    if (!session || !session->supervisor) return;
    SamosaMmSupervisor *supervisor = session->supervisor;
    pthread_mutex_lock(&supervisor->mutex);
    if (supervisor->generation == session->generation &&
        (supervisor->active_pid == session->pid || supervisor->reserved)) {
        supervisor->active_pid = 0;
        supervisor->active_pgid = 0;
        supervisor->reserved = 0;
        supervisor->active_provider[0] = 0;
    }
    pthread_mutex_unlock(&supervisor->mutex);
}

static void mm_kill_and_reap(SamosaMmSession *session, int graceful) {
    if (!session || session->pid <= 0) return;
    if (graceful && session->in_fd >= 0) {
        const char *quit = "{\"command\":\"quit\"}";
        (void)mm_send(session, quit, strlen(quit), 250);
    }
    if (session->in_fd >= 0) close(session->in_fd);
    if (session->out_fd >= 0) close(session->out_fd);
    session->in_fd = session->out_fd = -1;

    int status = 0;
    long long deadline = mm_now_ms() + (graceful ? session->shutdown_grace_ms : 0);
    while (graceful && mm_now_ms() < deadline) {
        pid_t rc = waitpid(session->pid, &status, WNOHANG);
        if (rc == session->pid || (rc < 0 && errno == ECHILD)) return;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 20000000};
        nanosleep(&pause, NULL);
    }
    if (session->pgid > 1 && session->pgid != getpgrp()) (void)kill(-session->pgid, SIGTERM);
    else (void)kill(session->pid, SIGTERM);
    deadline = mm_now_ms() + 750;
    while (mm_now_ms() < deadline) {
        pid_t rc = waitpid(session->pid, &status, WNOHANG);
        if (rc == session->pid || (rc < 0 && errno == ECHILD)) return;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 20000000};
        nanosleep(&pause, NULL);
    }
    if (session->pgid > 1 && session->pgid != getpgrp()) (void)kill(-session->pgid, SIGKILL);
    else (void)kill(session->pid, SIGKILL);
    while (waitpid(session->pid, &status, 0) < 0 && errno == EINTR) {}
}

int samosa_mm_supervisor_init(SamosaMmSupervisor *supervisor) {
    if (!supervisor) return 0;
    memset(supervisor, 0, sizeof(*supervisor));
    return pthread_mutex_init(&supervisor->mutex, NULL) == 0;
}

void samosa_mm_supervisor_destroy(SamosaMmSupervisor *supervisor) {
    if (!supervisor) return;
    (void)samosa_mm_supervisor_cancel(supervisor);
    pthread_mutex_destroy(&supervisor->mutex);
}

int samosa_mm_session_start(SamosaMmSupervisor *supervisor,
                            const SamosaMmProvider *provider,
                            SamosaMmSession *session,
                            char *error_code, size_t error_cap) {
    if (!supervisor || !provider || !session || !provider->provider ||
        !provider->executable || !provider->model_dir) {
        mm_error(error_code, error_cap, "multimodal_invalid_provider");
        return 0;
    }
    memset(session, 0, sizeof(*session));
    session->in_fd = session->out_fd = -1;
    if (!mm_regular_executable(provider->executable)) {
        mm_error(error_code, error_cap, "multimodal_engine_unavailable");
        return 0;
    }
    if (!mm_directory_no_symlink(provider->model_dir)) {
        mm_error(error_code, error_cap, "multimodal_model_unavailable");
        return 0;
    }

    pthread_mutex_lock(&supervisor->mutex);
    if (supervisor->reserved || supervisor->active_pid > 0) {
        pthread_mutex_unlock(&supervisor->mutex);
        mm_error(error_code, error_cap, "multimodal_specialist_busy");
        return 0;
    }
    supervisor->reserved = 1;
    supervisor->generation++;
    session->generation = supervisor->generation;
    snprintf(supervisor->active_provider, sizeof(supervisor->active_provider),
             "%s", provider->provider);
    pthread_mutex_unlock(&supervisor->mutex);

    session->supervisor = supervisor;
    session->protocol = provider->protocol;
    session->request_timeout_ms = provider->request_timeout_ms > 0
                                ? provider->request_timeout_ms : 120000;
    session->shutdown_grace_ms = provider->shutdown_grace_ms > 0
                               ? provider->shutdown_grace_ms : 2000;
    session->max_frame_bytes = provider->max_frame_bytes > 0
                             ? provider->max_frame_bytes : SAMOSA_MM_DEFAULT_MAX_FRAME;
    snprintf(session->provider, sizeof(session->provider), "%s", provider->provider);

    int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1};
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        if (in_pipe[0] >= 0) { close(in_pipe[0]); close(in_pipe[1]); }
        if (out_pipe[0] >= 0) { close(out_pipe[0]); close(out_pipe[1]); }
        mm_error(error_code, error_cap, "multimodal_pipe_failed");
        mm_supervisor_release(session);
        return 0;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
        mm_error(error_code, error_cap, "multimodal_fork_failed");
        mm_supervisor_release(session);
        return 0;
    }
    if (pid == 0) {
        (void)setpgid(0, 0);
        if (dup2(in_pipe[0], STDIN_FILENO) < 0 ||
            dup2(out_pipe[1], STDOUT_FILENO) < 0) _Exit(126);
        close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
        execl(provider->executable, provider->executable, provider->model_dir, (char *)NULL);
        _Exit(127);
    }
    (void)setpgid(pid, pid);
    close(in_pipe[0]); close(out_pipe[1]);
    session->pid = pid;
    session->pgid = pid;
    session->in_fd = in_pipe[1];
    session->out_fd = out_pipe[0];
    (void)mm_set_nonblock(session->in_fd);
    (void)mm_set_nonblock(session->out_fd);

    pthread_mutex_lock(&supervisor->mutex);
    if (supervisor->generation == session->generation) {
        supervisor->active_pid = pid;
        supervisor->active_pgid = pid;
        supervisor->reserved = 0;
    }
    pthread_mutex_unlock(&supervisor->mutex);

    const char *hello = provider->protocol == SAMOSA_MM_PROTOCOL_JSON_FRAME_V1
        ? "{\"command\":\"hello\",\"protocol\":\"samosa.multimodal.v1\"}"
        : "{\"command\":\"ping\"}";
    int ready_timeout = provider->ready_timeout_ms > 0 ? provider->ready_timeout_ms : 20000;
    char *reply = NULL; size_t reply_len = 0;
    if (!mm_send(session, hello, strlen(hello), ready_timeout) ||
        !mm_receive(session, &reply, &reply_len, ready_timeout) ||
        !mm_reply_ok(reply, reply_len)) {
        free(reply);
        mm_error(error_code, error_cap, "multimodal_readiness_failed");
        mm_kill_and_reap(session, 0);
        mm_supervisor_release(session);
        session->pid = session->pgid = 0;
        return 0;
    }
    free(reply);
    session->active = 1;
    mm_error(error_code, error_cap, "");
    return 1;
}

int samosa_mm_session_request(SamosaMmSession *session,
                              const char *json, size_t json_len,
                              char **reply, size_t *reply_len,
                              char *error_code, size_t error_cap) {
    if (reply) *reply = NULL;
    if (reply_len) *reply_len = 0;
    if (!session || !session->active || !reply || !json || !json_len) {
        mm_error(error_code, error_cap, "multimodal_not_ready");
        return 0;
    }
    if (json_len > session->max_frame_bytes) {
        mm_error(error_code, error_cap, "multimodal_request_too_large");
        return 0;
    }
    if (!mm_send(session, json, json_len, session->request_timeout_ms)) {
        mm_error(error_code, error_cap, "multimodal_pipe_error");
        return 0;
    }
    if (!mm_receive(session, reply, reply_len, session->request_timeout_ms)) {
        mm_error(error_code, error_cap, "multimodal_timeout_or_protocol_error");
        return 0;
    }
    mm_error(error_code, error_cap, "");
    return 1;
}

void samosa_mm_session_close(SamosaMmSession *session) {
    if (!session) return;
    if (session->pid > 0) mm_kill_and_reap(session, session->active);
    mm_supervisor_release(session);
    session->active = 0;
    session->pid = session->pgid = 0;
    session->supervisor = NULL;
}

int samosa_mm_supervisor_cancel(SamosaMmSupervisor *supervisor) {
    if (!supervisor) return 0;
    pthread_mutex_lock(&supervisor->mutex);
    pid_t pid = supervisor->active_pid;
    pid_t pgid = supervisor->active_pgid;
    pthread_mutex_unlock(&supervisor->mutex);
    if (pid <= 0) return 0;
    if (pgid > 1 && pgid != getpgrp()) (void)kill(-pgid, SIGTERM);
    else (void)kill(pid, SIGTERM);
    return 1;
}

pid_t samosa_mm_supervisor_active_pgid(SamosaMmSupervisor *supervisor) {
    if (!supervisor) return 0;
    pthread_mutex_lock(&supervisor->mutex);
    pid_t pgid = supervisor->active_pgid;
    pthread_mutex_unlock(&supervisor->mutex);
    return pgid;
}

int samosa_mm_supervisor_is_active(SamosaMmSupervisor *supervisor,
                                   char *provider, size_t provider_cap) {
    if (!supervisor) return 0;
    pthread_mutex_lock(&supervisor->mutex);
    int active = supervisor->reserved || supervisor->active_pid > 0;
    if (provider && provider_cap)
        snprintf(provider, provider_cap, "%s", supervisor->active_provider);
    pthread_mutex_unlock(&supervisor->mutex);
    return active;
}
