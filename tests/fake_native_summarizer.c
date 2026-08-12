#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int exact_read(void *buffer, size_t length) {
    unsigned char *cursor = buffer;
    while (length) {
        ssize_t count = read(STDIN_FILENO, cursor, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return 0;
        cursor += count; length -= (size_t)count;
    }
    return 1;
}

static int exact_write(const void *buffer, size_t length) {
    const unsigned char *cursor = buffer;
    while (length) {
        ssize_t count = write(STDOUT_FILENO, cursor, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return 0;
        cursor += count; length -= (size_t)count;
    }
    return 1;
}

int main(void) {
    for (;;) {
        uint32_t encoded = 0;
        if (!exact_read(&encoded, sizeof(encoded))) return 0;
        uint32_t length = ntohl(encoded);
        if (!length || length > 65536) return 2;
        char *prompt = malloc((size_t)length + 1);
        if (!prompt || !exact_read(prompt, length)) return 3;
        prompt[length] = 0;
        const char *fixed =
            "South Carolina DPH confirms 30 cyclosporiasis cases in 2026 and provides produce-washing and symptom guidance.";
        const char *summary = strstr(prompt, "South Carolina") ? fixed : prompt;
        if (!strncmp(summary, "summarize: ", 11)) summary += 11;
        size_t output_length = strlen(summary);
        if (output_length > 260) output_length = 260;
        const char *log_path = getenv("SAMOSA_FAKE_SUMMARIZER_LOG");
        if (log_path) {
            FILE *log = fopen(log_path, "a");
            if (log) { fprintf(log, "%ld\n", (long)getpid()); fclose(log); }
        }
        uint32_t output_encoded = htonl((uint32_t)output_length);
        int ok = exact_write(&output_encoded, sizeof(output_encoded)) &&
                 exact_write(summary, output_length);
        free(prompt);
        if (!ok) return 4;
    }
}
