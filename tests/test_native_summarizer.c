#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_all(int fd, const void *buffer, size_t length) {
    const unsigned char *cursor = buffer;
    while (length) {
        ssize_t count = write(fd, cursor, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return 0;
        cursor += count; length -= (size_t)count;
    }
    return 1;
}

static int read_all(int fd, void *buffer, size_t length) {
    unsigned char *cursor = buffer;
    while (length) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int ready = poll(&p, 1, 60000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return 0;
        ssize_t count = read(fd, cursor, length);
        if (count <= 0) return 0;
        cursor += count; length -= (size_t)count;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s ENGINE MODEL\n", argv[0]);
        return 2;
    }
    int in[2], out[2];
    if (pipe(in) || pipe(out)) return 3;
    pid_t pid = fork();
    if (pid < 0) return 4;
    if (!pid) {
        dup2(in[0], STDIN_FILENO); dup2(out[1], STDOUT_FILENO);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execl(argv[1], argv[1], "--model", argv[2], "--gpu-layers", "0",
              "--max-tokens", "100", (char *)NULL);
        _Exit(127);
    }
    close(in[0]); close(out[1]);
    const char *prompt =
        "summarize: South Carolina officials reported 30 cases of cyclosporiasis in 2026. "
        "None were linked to the national outbreak. Eleven cases had been reported by July 8, "
        "but the total increased to 30 by July 24. The illness can cause watery diarrhea.";
    uint32_t request = htonl((uint32_t)strlen(prompt));
    int ok = write_all(in[1], &request, sizeof(request)) &&
             write_all(in[1], prompt, strlen(prompt));
    uint32_t encoded = 0;
    ok = ok && read_all(out[0], &encoded, sizeof(encoded));
    uint32_t length = ok ? ntohl(encoded) : 0;
    char *summary = length && length < 8192 ? malloc((size_t)length + 1) : NULL;
    ok = summary && read_all(out[0], summary, length);
    if (summary) summary[length] = 0;
    close(in[1]); close(out[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!ok || !summary || !strstr(summary, "30 cases") || strstr(summary, "29")) {
        fprintf(stderr, "native summarizer produced an unsafe result: %s\n",
                summary ? summary : "<none>");
        free(summary); return 1;
    }
    printf("test_native_summarizer: PASS (%s)\n", summary);
    free(summary);
    return 0;
}
