#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "samosa_http.h"

static SamosaHttpServer *active_server;

static void sleep_ms(long ms) {
    struct timespec pause = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&pause, &pause) && errno == EINTR) {}
}

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
        strstr(request->body, "slow interactive probe")) {
        sleep_ms(1200);
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":\"slow interactive reply\"}}]}", NULL);
    }
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "No filename was a clear match") &&
        !strstr(request->body, "Additional detail from the user"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_compiled_ask\",\"type\":\"function\",\"function\":{"
            "\"name\":\"ask_user\",\"arguments\":\"{\\\"question\\\":\\\"What name should I search for?\\\"}\"}}]}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "Additional detail from the user: Miso") &&
        !strstr(request->body, "\"role\":\"tool\""))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_compiled_resume\",\"type\":\"function\",\"function\":{"
            "\"name\":\"fs_read_text\",\"arguments\":\"{\\\"path\\\":\\\"miso-record.txt\\\"}\"}}]}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "\"role\":\"tool\"") && strstr(request->body, "Miso vaccination"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"Found Miso's record at miso-record.txt.\"}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "find cat image document with doc.read") && !strstr(request->body, "\"role\":\"tool\""))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_compiled_doc_read\",\"type\":\"function\",\"function\":{"
            "\"name\":\"doc.read\",\"arguments\":\"{\\\"path\\\":\\\"cat-medical-note.png\\\",\\\"detail\\\":\\\"lines\\\"}\"}}]}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "local file-finding job") && !strstr(request->body, "\"role\":\"tool\""))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_compiled_find\",\"type\":\"function\",\"function\":{"
            "\"name\":\"fs_read_text\",\"arguments\":\"{\\\"path\\\":\\\"cat-medical-note.txt\\\"}\"}}]}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "move it to Archive") && strstr(request->body, "\"role\":\"tool\""))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\","
            "\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{"
            "\"id\":\"call_compiled_move\",\"type\":\"function\",\"function\":{"
            "\"name\":\"fs_move\",\"arguments\":\"{\\\"src\\\":\\\"cat-medical-note.txt\\\",\\\"dst\\\":\\\"Archive/cat-medical-note.txt\\\"}\"}}]}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "\"role\":\"tool\""))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"Found the matching record at cat-medical-note.txt. It contains Titli's vaccination record.\"}}]}", NULL);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "Extract structured data")) {
        if (strstr(request->body, "Interlock definition probe")) {
            sleep_ms(800);
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"merchant\\\":\\\"Interlock\\\",\\\"total\\\":7}\"}}]}", NULL);
        }
        if (strstr(request->body, "Image definition probe")) {
            if (!strstr(request->body, "\"content\":[") ||
                !strstr(request->body, "\"type\":\"image_url\"") ||
                !strstr(request->body, "\"url\":\"data:image/png;base64,"))
                return samosa_http_response(fd, 200, "application/json",
                    "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                    "\"message\":{\"role\":\"assistant\",\"content\":\"not json\"}}]}", NULL);
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"people\\\":2}\"}}]}", NULL);
        }
        if (strstr(request->body, "PDF first-final page probe")) {
            if (!strstr(request->body, "Page 1:") ||
                !strstr(request->body, "FIRST PAGE TITLE") ||
                !strstr(request->body, "Final page:") ||
                !strstr(request->body, "FINAL AFFILIATION") ||
                strstr(request->body, "MIDDLE PAGE BODY"))
                return samosa_http_response(fd, 200, "application/json",
                    "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                    "\"message\":{\"role\":\"assistant\",\"content\":\"not json\"}}]}", NULL);
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"merchant\\\":\\\"PdfPages\\\",\\\"total\\\":12}\"}}]}", NULL);
        }
        if (strstr(request->body, "Require budget probe")) {
            if (!strstr(request->body, "\"max_tokens\":1536") ||
                !strstr(request->body, "\"chat_template_kwargs\":{\"enable_thinking\":false}") ||
                !strstr(request->body, "\"response_format\":{\"type\":\"json_object\"}"))
                return samosa_http_response(fd, 200, "application/json",
                    "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                    "\"message\":{\"role\":\"assistant\",\"content\":\"not json\"}}]}", NULL);
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"merchant\\\":\\\"Budget\\\",\\\"total\\\":10}\"}}]}", NULL);
        }
        if (strstr(request->body, "Fenced JSON probe")) {
            /* Reproduce Qwen vision's habit of wrapping the object in a ```json
               markdown fence; the gateway must still recover the object. */
            return samosa_http_response(fd, 200, "application/json",
                "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"```json\\n{\\\"merchant\\\":\\\"Fenced\\\",\\\"total\\\":3}\\n```\"}}]}", NULL);
        }
    }
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/chat/completions") &&
        strstr(request->body, "Extract structured data"))
        return samosa_http_response(fd, 200, "application/json",
            "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\","
            "\"message\":{\"role\":\"assistant\",\"content\":"
            "\"{\\\"merchant\\\":\\\"Cafe\\\",\\\"total\\\":4.5}\"}}]}", NULL);
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
