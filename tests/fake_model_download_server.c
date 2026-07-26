#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

/* Deterministic fake remote artifact host for T0.1/T2.2 (docs/TASKS_UI_CHUTNI.md).
   Ordinary tests must never fetch a multi-gigabyte model artifact or exercise a
   real download over the network; this stands in for the trusted catalog host
   and serves one small file (SAMOSA_FAKE_DOWNLOAD_FILE) with a selectable,
   deliberately imperfect behavior (SAMOSA_FAKE_DOWNLOAD_MODE) so the future
   resumable downloader (T2.2) can be tested against every documented failure
   mode without a real network artifact. */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "samosa_http.h"

static SamosaHttpServer *active_server;

static void stop_server(int number) {
    (void)number;
    if (active_server) samosa_http_server_stop(active_server);
}

static char *read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(buf); return NULL; }
    buf[size] = 0;
    *out_len = (size_t)size;
    return buf;
}

/* Range: bytes=START-END or bytes=START- . Returns 1 and fills start/end
   (inclusive, end==-1 means "to EOF") on success, 0 if absent/unparseable. */
static int parse_range(const char *header, long long total, long long *start, long long *end) {
    if (!header || !*header) return 0;
    long long s = -1, e = -1;
    if (sscanf(header, "bytes=%lld-%lld", &s, &e) == 2) {
        *start = s; *end = e; return s >= 0 && e >= s && e < total;
    }
    if (sscanf(header, "bytes=%lld-", &s) == 1) {
        *start = s; *end = total - 1; return s >= 0 && s < total;
    }
    return 0;
}

static int send_body_partial(int fd, const char *body, size_t full_len, size_t send_len) {
    /* Sends a Content-Length matching full_len but only writes send_len bytes,
       then the caller closes the connection — simulates a dropped transfer. */
    if (!samosa_http_headers(fd, 200, "application/octet-stream", full_len, NULL)) return 0;
    if (send_len) return samosa_send_all(fd, body, send_len);
    return 1;
}

static int handler(SamosaHttpServer *server, int fd,
                   const SamosaHttpRequest *request, void *opaque) {
    (void)opaque;
    if (!strcmp(request->method, "GET") && !strcmp(request->path, "/health"))
        return samosa_http_response(fd, 200, "application/json", "{\"status\":\"ok\"}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/shutdown")) {
        samosa_http_response(fd, 200, "application/json", "{}", NULL);
        samosa_http_server_stop(server); return 1;
    }
    if (strcmp(request->method, "GET") || strcmp(request->path, "/artifact"))
        return samosa_http_json_error(fd, 404, "not_found", "Not found.");

    const char *file = getenv("SAMOSA_FAKE_DOWNLOAD_FILE");
    if (!file || !*file)
        return samosa_http_json_error(fd, 500, "server_misconfigured", "No fixture file set.");
    size_t len = 0;
    char *body = read_whole_file(file, &len);
    if (!body)
        return samosa_http_json_error(fd, 500, "server_misconfigured", "Fixture file unreadable.");

    const char *mode = getenv("SAMOSA_FAKE_DOWNLOAD_MODE");
    if (!mode) mode = "normal";
    int ok = 1;

    if (!strcmp(mode, "redirect")) {
        /* RFC 5737 TEST-NET-1: guaranteed non-routable, never reaches a real host. */
        ok = samosa_http_response(fd, 302, "text/plain", "redirect",
            "Location: http://192.0.2.1:1/artifact\r\n");
    } else if (!strcmp(mode, "ignore_range")) {
        ok = samosa_http_headers(fd, 200, "application/octet-stream", len, NULL) &&
             samosa_send_all(fd, body, len);
    } else if (!strcmp(mode, "truncate")) {
        /* Drop the connection after 40% of the promised bytes. */
        ok = send_body_partial(fd, body, len, len * 2 / 5);
    } else if (!strcmp(mode, "corrupt")) {
        if (len) body[len / 2] ^= 0xFF;
        long long start = 0, end = (long long)len - 1;
        if (parse_range(request->range, (long long)len, &start, &end)) {
            size_t slice = (size_t)(end - start + 1);
            char header[128];
            snprintf(header, sizeof(header), "Content-Range: bytes %lld-%lld/%zu\r\n", start, end, len);
            ok = samosa_http_headers(fd, 206, "application/octet-stream", slice, header) &&
                 samosa_send_all(fd, body + start, slice);
        } else {
            ok = samosa_http_headers(fd, 200, "application/octet-stream", len, NULL) &&
                 samosa_send_all(fd, body, len);
        }
    } else if (!strcmp(mode, "bad_content_range")) {
        long long start = 0, end = (long long)len - 1;
        parse_range(request->range, (long long)len, &start, &end);
        size_t slice = (size_t)(end - start + 1);
        /* Deliberately wrong: claims the whole file's size range regardless of
           what was actually requested/sent. */
        char header[128];
        snprintf(header, sizeof(header), "Content-Range: bytes 0-0/0\r\n");
        ok = samosa_http_headers(fd, 206, "application/octet-stream", slice, header) &&
             samosa_send_all(fd, body + start, slice);
    } else if (!strcmp(mode, "oversize")) {
        /* Byte-correct HTTP framing; the catalog's declared size is simply
           smaller than what this fixture file actually contains — the
           mismatch the downloader must catch is set up by the test picking a
           SAMOSA_FAKE_DOWNLOAD_FILE bigger than the catalog manifest bytes. */
        ok = samosa_http_headers(fd, 200, "application/octet-stream", len, NULL) &&
             samosa_send_all(fd, body, len);
    } else {
        /* normal: honor Range with a correct 206, else a correct 200. */
        long long start = 0, end = (long long)len - 1;
        if (parse_range(request->range, (long long)len, &start, &end)) {
            size_t slice = (size_t)(end - start + 1);
            char header[128];
            snprintf(header, sizeof(header), "Content-Range: bytes %lld-%lld/%zu\r\n", start, end, len);
            ok = samosa_http_headers(fd, 206, "application/octet-stream", slice, header) &&
                 samosa_send_all(fd, body + start, slice);
        } else {
            ok = samosa_http_headers(fd, 200, "application/octet-stream", len, NULL) &&
                 samosa_send_all(fd, body, len);
        }
    }
    free(body);
    return ok;
}

int main(int argc, char **argv) {
    int port = 0;
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--port")) port = atoi(argv[i + 1]);
    if (port < 1) return 2;
    const char *pid_file = getenv("SAMOSA_FAKE_PID_FILE");
    if (pid_file && *pid_file) {
        int pidfd = open(pid_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (pidfd >= 0) {
            char text[32]; int n = snprintf(text, sizeof(text), "%ld\n", (long)getpid());
            if (n > 0) (void)write(pidfd, text, (size_t)n);
            close(pidfd);
        }
    }
    SamosaHttpServer server;
    if (!samosa_http_server_init(&server, port, handler, NULL)) return 2;
    active_server = &server;
    signal(SIGINT, stop_server); signal(SIGTERM, stop_server);
    int ok = samosa_http_server_run(&server);
    samosa_http_server_destroy(&server);
    active_server = NULL;
    return ok ? 0 : 2;
}
