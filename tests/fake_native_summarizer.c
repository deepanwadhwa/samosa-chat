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
        int count = (length & 0x80000000U) ? (int)(length & 0x3fffffffU) : 1;
        int batch = (length & 0x80000000U) != 0;
        if (count < 1 || count > 64) return 2;
        char *prompts[64] = {0};
        /* Drain the complete request before writing replies, just like the
           native engine. Large batches must not deadlock two full pipes. */
        for (int i = 0; i < count; i++) {
            if (batch) {
                if (!exact_read(&encoded, sizeof(encoded))) return 2;
                length = ntohl(encoded);
            }
            if (!length || length > 65536) return 2;
            prompts[i] = malloc((size_t)length + 1);
            if (!prompts[i] || !exact_read(prompts[i], length)) return 3;
            prompts[i][length] = 0;
        }
        for (int i = 0; i < count; i++) {
            const char *fixed =
                "South Carolina DPH confirms 30 cyclosporiasis cases in 2026 and provides produce-washing and symptom guidance.";
            const char *summary = strstr(prompts[i], "South Carolina") ? fixed : prompts[i];
            if (!strncmp(summary, "summarize: ", 11)) summary += 11;
            size_t output_length = strlen(summary);
            if (output_length > 260) output_length = 260;
            const char *log_path = getenv("SAMOSA_FAKE_SUMMARIZER_LOG");
            if (log_path) {
                FILE *log = fopen(log_path, "a");
                if (log) { fprintf(log, "%ld\n", (long)getpid()); fclose(log); }
            }
            uint32_t output_encoded = htonl((uint32_t)output_length);
            int ok = exact_write(&output_encoded, sizeof(output_encoded)) && exact_write(summary, output_length);
            free(prompts[i]);
            if (!ok) return 4;
        }
    }
}
