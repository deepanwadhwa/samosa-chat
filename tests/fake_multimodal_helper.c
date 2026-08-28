#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int write_all(const void *bytes_, size_t len) {
    const unsigned char *bytes = bytes_;
    size_t at = 0;
    while (at < len) {
        ssize_t n = write(STDOUT_FILENO, bytes + at, len - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        at += (size_t)n;
    }
    return 1;
}

static int read_all(void *bytes_, size_t len) {
    unsigned char *bytes = bytes_;
    size_t at = 0;
    while (at < len) {
        ssize_t n = read(STDIN_FILENO, bytes + at, len - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        at += (size_t)n;
    }
    return 1;
}

static char *read_message(int framed, size_t *out_len) {
    if (framed) {
        uint32_t encoded = 0;
        if (!read_all(&encoded, sizeof(encoded))) return NULL;
        size_t len = ntohl(encoded);
        if (!len || len > (1u << 20)) return NULL;
        char *message = malloc(len + 1);
        if (!message || !read_all(message, len)) { free(message); return NULL; }
        message[len] = 0; *out_len = len; return message;
    }
    size_t cap = 4096, len = 0;
    char *message = malloc(cap);
    if (!message) return NULL;
    for (;;) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(message); return NULL; }
        if (c == '\n') break;
        if (len + 1 >= cap) {
            cap *= 2;
            if (cap > (1u << 20)) { free(message); return NULL; }
            char *grown = realloc(message, cap);
            if (!grown) { free(message); return NULL; }
            message = grown;
        }
        message[len++] = c;
    }
    message[len] = 0; *out_len = len; return message;
}

static int send_message(int framed, const char *message) {
    size_t len = strlen(message);
    if (framed) {
        uint32_t encoded = htonl((uint32_t)len);
        return write_all(&encoded, sizeof(encoded)) && write_all(message, len);
    }
    return write_all(message, len) && write_all("\n", 1);
}

int main(void) {
    int framed = getenv("SAMOSA_FAKE_MM_FRAMED") &&
                 !strcmp(getenv("SAMOSA_FAKE_MM_FRAMED"), "1");
    for (;;) {
        size_t len = 0;
        char *message = read_message(framed, &len);
        if (!message) return 0;
        const char *log_path = getenv("SAMOSA_FAKE_MM_LOG");
        if (log_path && *log_path) {
            FILE *log = fopen(log_path, "a");
            if (log) { fwrite(message, 1, len, log); fputc('\n', log); fclose(log); }
        }
        if (strstr(message, "\"command\":\"quit\"")) {
            free(message);
            (void)send_message(framed, "{\"status\":\"ok\"}");
            return 0;
        }
        if (strstr(message, "\"command\":\"hang\"")) {
            struct timespec pause = {.tv_sec = 30, .tv_nsec = 0};
            free(message);
            nanosleep(&pause, NULL);
            continue;
        }
        if (strstr(message, "\"command\":\"crash\"")) {
            free(message);
            return 73;
        }
        int image = strstr(message, "\"media_kind\":\"image\"") != NULL;
        int poisoned_vague_prompt = image && strstr(message, "what is this?") &&
            (strstr(message, "For charts") ||
             strstr(message, "Transcribe every visible title"));
        free(message);
        const char *response = poisoned_vague_prompt
            ? "{\"status\":\"ok\",\"id\":\"gateway\",\"observation\":\"POISONED_VAGUE_IMAGE_PROMPT\","
              "\"prompt_tokens\":3,\"generated_tokens\":4,\"frames\":0,\"duration_seconds\":0}"
            : image
            ? "{\"status\":\"ok\",\"id\":\"gateway\",\"observation\":\"fixture image evidence: a 3 by 4 grid containing 12 charts <points coords=\\\"1 1 100 100 2 300 100 3 500 100 4 700 100 5 100 400 6 300 400 7 500 400 8 700 400 9 100 700 10 300 700 11 500 700 12 700 700\\\">charts</points>\","
              "\"prompt_tokens\":3,\"generated_tokens\":4,\"frames\":0,\"duration_seconds\":0}"
            : "{\"status\":\"ok\",\"id\":\"gateway\",\"observation\":\"fixture timestamped video evidence\","
              "\"prompt_tokens\":3,\"generated_tokens\":4,\"frames\":8,\"duration_seconds\":30}";
        if (!send_message(framed, response))
            return 1;
    }
}
