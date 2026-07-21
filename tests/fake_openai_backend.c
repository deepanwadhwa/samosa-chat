#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "samosa_http.h"

static SamosaHttpServer *active_server;

static void stop_server(int number) {
    (void)number;
    if (active_server) samosa_http_server_stop(active_server);
}

static int handler(SamosaHttpServer *server, int fd,
                   const SamosaHttpRequest *request, void *opaque) {
    (void)opaque;
    if (!strcmp(request->method, "GET") && !strcmp(request->path, "/health"))
        return samosa_http_response(fd, 200, "application/json", "{\"status\":\"ok\"}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "local file-finding job") && !strstr(request->body, "\"role\":\"tool\""))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_compiled_find\",\"type\":\"function\",\"function\":{"
            "\"name\":\"fs_read_text\",\"arguments\":\"{\\\"path\\\":\\\"cat-medical-note.txt\\\"}\"}}]}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "\"role\":\"tool\""))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"Found the matching record at cat-medical-note.txt. It contains Titli's vaccination record.\"}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":\"compiled reply\"}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/shutdown")) {
        samosa_http_response(fd, 200, "application/json", "{}", NULL);
        samosa_http_server_stop(server); return 1;
    }
    return samosa_http_json_error(fd, 404, "not_found", "Not found.");
}

int main(int argc, char **argv) {
    int port = 0;
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--port")) port = atoi(argv[i + 1]);
    if (port < 1) return 2;
    SamosaHttpServer server;
    if (!samosa_http_server_init(&server, port, handler, NULL)) return 2;
    active_server = &server;
    signal(SIGINT, stop_server); signal(SIGTERM, stop_server);
    int ok = samosa_http_server_run(&server);
    samosa_http_server_destroy(&server);
    active_server = NULL;
    return ok ? 0 : 2;
}
