/* Compiled Samosa gateway: local app, backend supervision, and raw API proxy. */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "json.h"
#include "samosa_http.h"

typedef struct {
    SamosaHttpServer *server;
    pthread_mutex_t mu;
    pid_t backend_pid;
    pid_t job_pids[16];
    int upstream_fd;
    atomic_int generating;
    atomic_int stopping;
    int public_port;
    int backend_port;
    char home[PATH_MAX];
    char jobs_root[PATH_MAX];
    char backend[16];
    char app_html[PATH_MAX];
    char app_logo[PATH_MAX];
    char qwen_engine[PATH_MAX];
    char qwen_model[PATH_MAX];
    char tokenizer[PATH_MAX];
    char llama_server[PATH_MAX];
    char bonsai_model[PATH_MAX];
    char ornith_model[PATH_MAX];
    char samosa_fs[PATH_MAX];
    char samosa_extract[PATH_MAX];
    char backend_log[PATH_MAX];
    char selection_file[PATH_MAX];
} Gateway;

static int tcp_connect(int port);
static int backend_probe(Gateway *g);
static const char *backend_model(const char *name);

static Gateway *signal_gateway;

static int path_copy(char *out, size_t cap, const char *value) {
    int n = snprintf(out, cap, "%s", value ? value : "");
    return n >= 0 && (size_t)n < cap;
}

static int path_join(char *out, size_t cap, const char *left, const char *right) {
    int n = snprintf(out, cap, "%s/%s", left, right);
    return n >= 0 && (size_t)n < cap;
}

static int regular_file(const char *path, int executable) {
    struct stat st;
    return path && !stat(path, &st) && S_ISREG(st.st_mode) &&
           (!executable || !access(path, X_OK));
}

static int mkdirs(const char *path) {
    char copy[PATH_MAX];
    if (!path_copy(copy, sizeof(copy), path)) return 0;
    for (char *p = copy + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(copy, 0700) && errno != EEXIST) return 0;
        *p = '/';
    }
    return !mkdir(copy, 0700) || errno == EEXIST;
}

static int read_small_file(const char *path, char *out, size_t cap) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return 0;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0) return 0;
    out[n] = 0;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
        out[--n] = 0;
    return n > 0;
}

static int write_small_file(const char *path, const char *text) {
    char temp[PATH_MAX];
    if (snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid()) >=
        (int)sizeof(temp)) return 0;
    int out = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (out < 0) return 0;
    size_t length = strlen(text), written = 0;
    int ok = 1;
    while (written < length) {
        ssize_t n = write(out, text + written, length - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = 0; break; }
        written += (size_t)n;
    }
    if (ok) ok = fsync(out) == 0;
    if (close(out)) ok = 0;
    if (ok) ok = rename(temp, path) == 0;
    if (!ok) unlink(temp);
    return ok;
}

static void track_job_pid(Gateway *g, pid_t pid, int add) {
    pthread_mutex_lock(&g->mu);
    if (add) {
        for (size_t i = 0; i < sizeof(g->job_pids) / sizeof(g->job_pids[0]); ++i)
            if (!g->job_pids[i]) { g->job_pids[i] = pid; break; }
    } else {
        for (size_t i = 0; i < sizeof(g->job_pids) / sizeof(g->job_pids[0]); ++i)
            if (g->job_pids[i] == pid) { g->job_pids[i] = 0; break; }
    }
    pthread_mutex_unlock(&g->mu);
}

static char *run_capture(Gateway *g, const char *program, char *const argv[], size_t limit, int *status) {
    int pipefd[2];
    if (pipe(pipefd)) return NULL;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return NULL; }
    if (pid == 0) {
        close(pipefd[0]); dup2(pipefd[1], STDOUT_FILENO); close(pipefd[1]);
        execv(program, argv); _Exit(127);
    }
    close(pipefd[1]); track_job_pid(g, pid, 1);
    char *output = malloc(limit + 1); size_t used = 0;
    if (!output) { close(pipefd[0]); kill(pid, SIGKILL); waitpid(pid, NULL, 0); track_job_pid(g, pid, 0); return NULL; }
    while (used < limit) {
        ssize_t n = read(pipefd[0], output + used, limit - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(pipefd[0]); waitpid(pid, status, 0); track_job_pid(g, pid, 0); output[used] = 0;
    if (used == limit) { free(output); return NULL; }
    return output;
}

static int json_escape_to(char *out, size_t cap, size_t *used, const char *text) {
    static const char hex[] = "0123456789abcdef";
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        char encoded[7]; const char *part = encoded; size_t length;
        if (*p == '"' || *p == '\\') { encoded[0] = '\\'; encoded[1] = (char)*p; length = 2; }
        else if (*p == '\n') { part = "\\n"; length = 2; }
        else if (*p == '\r') { part = "\\r"; length = 2; }
        else if (*p == '\t') { part = "\\t"; length = 2; }
        else if (*p < 0x20) { memcpy(encoded, "\\u00", 4); encoded[4] = hex[*p >> 4]; encoded[5] = hex[*p & 15]; length = 6; }
        else { encoded[0] = (char)*p; length = 1; }
        if (*used + length >= cap) return 0;
        memcpy(out + *used, part, length); *used += length;
    }
    out[*used] = 0; return 1;
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} TextBuffer;

static int text_reserve(TextBuffer *buffer, size_t extra) {
    if (extra > SIZE_MAX - buffer->len - 1) return 0;
    size_t needed = buffer->len + extra + 1;
    if (needed <= buffer->cap) return 1;
    size_t cap = buffer->cap ? buffer->cap : 4096;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2) { cap = needed; break; }
        cap *= 2;
    }
    char *next = realloc(buffer->data, cap);
    if (!next) return 0;
    buffer->data = next; buffer->cap = cap;
    return 1;
}

static int text_add_n(TextBuffer *buffer, const char *text, size_t length) {
    if (!text_reserve(buffer, length)) return 0;
    memcpy(buffer->data + buffer->len, text, length);
    buffer->len += length; buffer->data[buffer->len] = 0;
    return 1;
}

static int text_add(TextBuffer *buffer, const char *text) {
    return text_add_n(buffer, text, strlen(text));
}

static int text_json_string(TextBuffer *buffer, const char *value) {
    if (!text_add(buffer, "\"")) return 0;
    size_t source_len = strlen(value);
    size_t cap = source_len * 6 + 1;
    char *escaped = malloc(cap);
    size_t used = 0;
    if (!escaped || !json_escape_to(escaped, cap, &used, value)) {
        free(escaped); return 0;
    }
    int ok = text_add_n(buffer, escaped, used) && text_add(buffer, "\"");
    free(escaped); return ok;
}

static int text_json_value(TextBuffer *out, jval *value) {
    if (!value) return text_add(out, "null");
    char number[64];
    switch (value->t) {
        case J_NULL: return text_add(out, "null");
        case J_BOOL: return text_add(out, value->boolean ? "true" : "false");
        case J_NUM:
            snprintf(number, sizeof(number), "%.17g", value->num);
            return text_add(out, number);
        case J_STR: return text_json_string(out, value->str);
        case J_ARR:
            if (!text_add(out, "[")) return 0;
            for (int i = 0; i < value->len; ++i)
                if ((i && !text_add(out, ",")) || !text_json_value(out, value->kids[i])) return 0;
            return text_add(out, "]");
        case J_OBJ:
            if (!text_add(out, "{")) return 0;
            for (int i = 0; i < value->len; ++i)
                if ((i && !text_add(out, ",")) || !text_json_string(out, value->keys[i]) ||
                    !text_add(out, ":") || !text_json_value(out, value->kids[i])) return 0;
            return text_add(out, "}");
    }
    return 0;
}

static char *read_file_limit(const char *path, size_t limit) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (size_t)st.st_size > limit) {
        close(fd); return NULL;
    }
    char *data = malloc((size_t)st.st_size + 1);
    if (!data) { close(fd); return NULL; }
    size_t used = 0, size = (size_t)st.st_size;
    while (used < size) {
        ssize_t n = read(fd, data + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(data); close(fd); return NULL; }
        used += (size_t)n;
    }
    close(fd); data[size] = 0; return data;
}

static int job_state_path(Gateway *g, const char *job_id, const char *name,
                          char out[PATH_MAX], int create) {
    if (!job_id || !*job_id) return 0;
    for (const char *p = job_id; *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' )) return 0;
    char directory[PATH_MAX];
    if (!path_join(directory, sizeof(directory), g->jobs_root, job_id) ||
        (create && !mkdirs(directory)) || !path_join(out, PATH_MAX, directory, name)) return 0;
    return 1;
}

static int save_job_state(Gateway *g, const char *job_id, const char *goal,
                          const char *folder) {
    char path[PATH_MAX]; TextBuffer json = {0};
    if (!job_state_path(g, job_id, "job.json", path, 1) ||
        !text_add(&json, "{\"job_id\":") || !text_json_string(&json, job_id) ||
        !text_add(&json, ",\"goal\":") || !text_json_string(&json, goal) ||
        !text_add(&json, ",\"folder\":") || !text_json_string(&json, folder) ||
        !text_add(&json, "}\n")) { free(json.data); return 0; }
    int ok = write_small_file(path, json.data); free(json.data); return ok;
}

static int load_job_state(Gateway *g, const char *job_id, char **goal, char **folder) {
    char path[PATH_MAX], *raw = NULL, *arena = NULL; jval *root = NULL;
    if (!job_state_path(g, job_id, "job.json", path, 0) || !(raw = read_file_limit(path, 65536)) ||
        !(root = json_parse(raw, &arena)) || root->t != J_OBJ) goto fail;
    jval *gval = json_get(root, "goal"), *fval = json_get(root, "folder");
    if (!gval || gval->t != J_STR || !fval || fval->t != J_STR) goto fail;
    *goal = strdup(gval->str); *folder = strdup(fval->str);
    json_free(root); free(arena); free(raw); return *goal && *folder;
fail:
    json_free(root); free(arena); free(raw); return 0;
}

static char *backend_json(Gateway *g, const char *payload) {
    if (!backend_probe(g)) return NULL;
    int fd = tcp_connect(g->backend_port);
    if (fd < 0) return NULL;
    pthread_mutex_lock(&g->mu); g->upstream_fd = fd; pthread_mutex_unlock(&g->mu);
    atomic_store(&g->generating, 1);
    char header[512];
    int n = snprintf(header, sizeof(header),
        "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
        "Content-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        g->backend_port, strlen(payload));
    if (n <= 0 || (size_t)n >= sizeof(header) ||
        !samosa_send_all(fd, header, (size_t)n) ||
        !samosa_send_all(fd, payload, strlen(payload))) {
        pthread_mutex_lock(&g->mu); if (g->upstream_fd == fd) g->upstream_fd = -1; pthread_mutex_unlock(&g->mu);
        atomic_store(&g->generating, 0); close(fd); return NULL;
    }
    TextBuffer response = {0}; char chunk[65536];
    while (response.len < SAMOSA_HTTP_MAX_BODY + SAMOSA_HTTP_MAX_HEADER) {
        ssize_t got = recv(fd, chunk, sizeof(chunk), 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        if (!text_add_n(&response, chunk, (size_t)got)) break;
    }
    pthread_mutex_lock(&g->mu); if (g->upstream_fd == fd) g->upstream_fd = -1; pthread_mutex_unlock(&g->mu);
    atomic_store(&g->generating, 0); close(fd);
    if (!response.data || !strstr(response.data, " 200 ")) {
        free(response.data); return NULL;
    }
    char *body = strstr(response.data, "\r\n\r\n");
    if (!body) { free(response.data); return NULL; }
    body += 4;
    char *copy = strdup(body);
    free(response.data); return copy;
}

static int sse_json(int fd, const char *json) {
    return samosa_send_all(fd, "data: ", 6) && samosa_send_all(fd, json, strlen(json)) &&
           samosa_send_all(fd, "\n\n", 2);
}

static int contains_case(const char *text, const char *word) {
    size_t length = strlen(word);
    if (!length) return 0;
    for (; *text; ++text)
        if (!strncasecmp(text, word, length)) return 1;
    return 0;
}

static int find_intent(const char *goal) {
    static const char *terms[] = {"find", "locate", "search", "look for", "where is", "which file"};
    for (size_t i = 0; i < sizeof(terms) / sizeof(terms[0]); ++i)
        if (contains_case(goal, terms[i])) return 1;
    return 0;
}

static int safe_job_path(const char *folder, const char *relative, char out[PATH_MAX]) {
    if (!relative || !*relative || relative[0] == '/') return 0;
    char root[PATH_MAX], joined[PATH_MAX], resolved[PATH_MAX];
    if (!realpath(folder, root) ||
        snprintf(joined, sizeof(joined), "%s/%s", root, relative) >= (int)sizeof(joined) ||
        !realpath(joined, resolved)) return 0;
    size_t root_len = strlen(root);
    if (strncmp(root, resolved, root_len) ||
        (resolved[root_len] && resolved[root_len] != '/')) return 0;
    struct stat st;
    if (lstat(joined, &st) || S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) return 0;
    return path_copy(out, PATH_MAX, resolved);
}

static char *read_bounded_text(const char *path) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return strdup("The selected file could not be opened.");
    char *result = malloc(8193);
    if (!result) { close(fd); return NULL; }
    ssize_t n = read(fd, result, 8192);
    close(fd);
    if (n < 0) { free(result); return strdup("The selected file could not be read."); }
    result[n] = 0;
    return result;
}

static int candidate_score(const char *goal, const char *name) {
    char copy[1024];
    if (!path_copy(copy, sizeof(copy), goal)) return 0;
    static const char *stop[] = {"find","locate","search","look","file","files","folder",
        "record","records","please","could","would","should","this","that","with","from","your","my"};
    int score = 0;
    for (char *save = NULL, *word = strtok_r(copy, " \\t.,?!:;/\\\"'()[]{}", &save);
         word; word = strtok_r(NULL, " \\t.,?!:;/\\\"'()[]{}", &save)) {
        int ignored = strlen(word) < 3;
        for (size_t i = 0; !ignored && i < sizeof(stop) / sizeof(stop[0]); ++i)
            ignored = !strcasecmp(word, stop[i]);
        if (!ignored && contains_case(name, word)) score += 4;
    }
    if ((contains_case(goal, "cat") || contains_case(goal, "pet")) &&
        (contains_case(name, "vet") || contains_case(name, "medical") ||
         contains_case(name, "vaccin") || contains_case(name, "rabies") ||
         contains_case(name, "clinic") || contains_case(name, "health"))) score += 3;
    if (contains_case(goal, "medical") &&
        (contains_case(name, "medical") || contains_case(name, "health") ||
         contains_case(name, "clinic") || contains_case(name, "lab") ||
         contains_case(name, "prescription") || contains_case(name, "vet"))) score += 2;
    return score;
}

typedef struct { const char *name; int score; } FindCandidate;

static int build_candidates(const char *goal, jval *items, TextBuffer *out, int *count) {
    FindCandidate best[40] = {{0}};
    *count = 0;
    for (int i = 0; items && items->t == J_ARR && i < items->len; ++i) {
        jval *name = json_get(items->kids[i], "name");
        if (!name || name->t != J_STR) continue;
        int score = candidate_score(goal, name->str);
        if (score <= 0) continue;
        if (*count == 40 && (score < best[39].score ||
            (score == best[39].score && strcasecmp(name->str, best[39].name) >= 0))) continue;
        int at = *count < 40 ? (*count)++ : 39;
        best[at].name = name->str; best[at].score = score;
        while (at > 0 && (best[at].score > best[at - 1].score ||
               (best[at].score == best[at - 1].score &&
                strcasecmp(best[at].name, best[at - 1].name) < 0))) {
            FindCandidate swap = best[at - 1]; best[at - 1] = best[at]; best[at] = swap; --at;
        }
    }
    if (!*count) return text_add(out, "No filename was a clear match. Ask for a distinguishing name, date, or phrase.");
    if (!text_add(out, "Likely candidates selected from the complete filename index:\n")) return 0;
    for (int i = 0; i < *count; ++i)
        if (!text_add(out, "- ") || !text_add(out, best[i].name) || !text_add(out, "\n")) return 0;
    return 1;
}

static char *tool_result(Gateway *g, const char *folder, const char *name, jval *args) {
    jval *path = args && args->t == J_OBJ ? json_get(args, "path") : NULL;
    char absolute[PATH_MAX];
    if (strcmp(name, "ask_user") && (!path || path->t != J_STR ||
        !safe_job_path(folder, path->str, absolute)))
        return strdup("That path is not a regular file inside the selected folder.");
    if (!strcmp(name, "fs_metadata")) {
        char *argv[] = {g->samosa_fs, "metadata", "--max-file-bytes", "104857600", absolute, NULL};
        int status = 0; char *raw = run_capture(g, g->samosa_fs, argv, 1 << 20, &status);
        if (!raw || !WIFEXITED(status) || WEXITSTATUS(status)) { free(raw); return strdup("File details could not be read."); }
        return raw;
    }
    if (!strcmp(name, "fs_read_text")) return read_bounded_text(absolute);
    if (!strcmp(name, "fs_read_pages")) {
        jval *start_value = json_get(args, "start"), *count_value = json_get(args, "count");
        int start = start_value && start_value->t == J_NUM ? (int)start_value->num : 1;
        int pages = count_value && count_value->t == J_NUM ? (int)count_value->num : 5;
        if (start < 1 || pages < 1 || pages > 5)
            return strdup("Page reads require a start page of 1 or greater and a count from 1 to 5.");
        char start_text[24], count_text[24];
        snprintf(start_text, sizeof(start_text), "%d", start);
        snprintf(count_text, sizeof(count_text), "%d", pages);
        char *argv[] = {g->samosa_extract, "--json-pages", absolute, start_text, count_text, NULL};
        int status = 0; char *raw = run_capture(g, g->samosa_extract, argv, 1 << 20, &status);
        if (!raw || !WIFEXITED(status) || WEXITSTATUS(status)) { free(raw); return strdup("Those document pages could not be extracted."); }
        return raw;
    }
    return strdup("Unknown tool request.");
}

static const char *find_tools_json =
    "[{\"type\":\"function\",\"function\":{\"name\":\"fs_metadata\",\"description\":\"Check one candidate file's type, size and metadata without reading its content\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"fs_read_text\",\"description\":\"Read at most 8192 characters from one selected plain text candidate\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"fs_read_pages\",\"description\":\"Read 1 to 5 consecutive pages from one selected PDF; request another range only if needed\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"start\":{\"type\":\"integer\",\"minimum\":1},\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":5}},\"required\":[\"path\",\"start\",\"count\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"ask_user\",\"description\":\"Ask one clarifying question when the indexed candidates are not reliable\",\"parameters\":{\"type\":\"object\",\"properties\":{\"question\":{\"type\":\"string\"}},\"required\":[\"question\"]}}}]";

static int jobs_find(Gateway *g, int fd, const char *goal, const char *folder,
                     jval *survey, const char *job_id, int start_seq) {
    char *argv[] = {g->samosa_fs, "list", "--max-file-bytes", "104857600", (char *)folder, NULL};
    int status = 0; char *list_raw = run_capture(g, g->samosa_fs, argv, 16 << 20, &status);
    char *arena = NULL; jval *listing = list_raw ? json_parse(list_raw, &arena) : NULL;
    jval *items = listing && listing->t == J_OBJ ? json_get(listing, "items") : NULL;
    if (!items || items->t != J_ARR || !WIFEXITED(status) || WEXITSTATUS(status)) {
        json_free(listing); free(arena); free(list_raw);
        return sse_json(fd, "{\"type\":\"error\",\"message\":\"The folder index could not be built.\"}");
    }
    char event[1024];
    snprintf(event, sizeof(event), "{\"seq\":%d,\"type\":\"indexing\",\"total\":%d}", start_seq, items->len);
    if (!sse_json(fd, event)) goto fail;
    TextBuffer candidates = {0}; int candidate_count = 0;
    if (!build_candidates(goal, items, &candidates, &candidate_count)) goto fail;
    snprintf(event, sizeof(event), "{\"seq\":%d,\"type\":\"index_complete\",\"total\":%d,\"candidates\":%d}",
             start_seq + 1, items->len, candidate_count);
    if (!sse_json(fd, event)) { free(candidates.data); goto fail; }

    TextBuffer messages = {0};
    const char *system = "You are completing a local file-finding job. The gateway has checked every filename and supplied the strongest candidates. Inspect only plausible candidates. Use metadata before content. Read plain text only with fs_read_text. Read PDFs only with fs_read_pages in chunks of no more than 5 pages, beginning with the most relevant range and continuing only when required. Never ask to read an entire document. Return a plain-language answer naming the matching relative path and the evidence. If no candidate is reliable, call ask_user. Never print tool JSON or tool names in the answer.";
    TextBuffer user = {0};
    if (!text_add(&user, "Goal: ") || !text_add(&user, goal) || !text_add(&user, "\n") ||
        !text_add(&user, candidates.data)) { free(candidates.data); free(user.data); goto fail; }
    free(candidates.data);
    if (!text_add(&messages, "{\"role\":\"system\",\"content\":") ||
        !text_json_string(&messages, system) || !text_add(&messages, "},{\"role\":\"user\",\"content\":") ||
        !text_json_string(&messages, user.data) || !text_add(&messages, "}")) {
        free(user.data); free(messages.data); goto fail;
    }
    free(user.data);

    int seq = start_seq + 2;
    for (int round = 0; round < 8; ++round) {
        TextBuffer payload = {0};
        if (!text_add(&payload, "{\"model\":") || !text_json_string(&payload, backend_model(g->backend)) ||
            !text_add(&payload, ",\"messages\":[") || !text_add(&payload, messages.data) ||
            !text_add(&payload, "],\"tools\":") || !text_add(&payload, find_tools_json) ||
            !text_add(&payload, ",\"tool_choice\":\"auto\",\"parallel_tool_calls\":false,\"stream\":false,\"max_tokens\":1024}")) {
            free(payload.data); free(messages.data); goto fail;
        }
        char *reply_raw = backend_json(g, payload.data); free(payload.data);
        char *reply_arena = NULL; jval *reply = reply_raw ? json_parse(reply_raw, &reply_arena) : NULL;
        jval *choices = reply && reply->t == J_OBJ ? json_get(reply, "choices") : NULL;
        jval *message = choices && choices->t == J_ARR && choices->len ? json_get(choices->kids[0], "message") : NULL;
        jval *calls = message && message->t == J_OBJ ? json_get(message, "tool_calls") : NULL;
        if (!message || message->t != J_OBJ) {
            json_free(reply); free(reply_arena); free(reply_raw); free(messages.data); goto model_fail;
        }
        if (!calls || calls->t != J_ARR || !calls->len) {
            jval *content = json_get(message, "content");
            if (!content || content->t != J_STR || !*content->str || strstr(content->str, "samosa_tool") || strstr(content->str, "fs_")) {
                json_free(reply); free(reply_arena); free(reply_raw); free(messages.data); goto ask;
            }
            TextBuffer done = {0};
            if (!text_add(&done, "{\"seq\":") ) { free(done.data); goto final_fail; }
            char number[32]; snprintf(number, sizeof(number), "%d", seq);
            if (!text_add(&done, number) || !text_add(&done, ",\"type\":\"done\",\"job_id\":") ||
                !text_json_string(&done, job_id) || !text_add(&done, ",\"summary\":") ||
                !text_json_string(&done, content->str) || !text_add(&done, "}")) { free(done.data); goto final_fail; }
            int ok = sse_json(fd, done.data) && samosa_send_all(fd, "data: [DONE]\n\n", 14);
            free(done.data); json_free(reply); free(reply_arena); free(reply_raw); free(messages.data);
            json_free(listing); free(arena); free(list_raw); (void)survey; return ok;
final_fail:
            json_free(reply); free(reply_arena); free(reply_raw); free(messages.data); goto fail;
        }
        jval *call = calls->kids[0], *function = call && call->t == J_OBJ ? json_get(call, "function") : NULL;
        jval *id = call && call->t == J_OBJ ? json_get(call, "id") : NULL;
        jval *name = function && function->t == J_OBJ ? json_get(function, "name") : NULL;
        jval *arguments = function && function->t == J_OBJ ? json_get(function, "arguments") : NULL;
        if (!id || id->t != J_STR || !name || name->t != J_STR || !arguments || arguments->t != J_STR) {
            json_free(reply); free(reply_arena); free(reply_raw); free(messages.data); goto model_fail;
        }
        char *args_arena = NULL; jval *args = json_parse(arguments->str, &args_arena);
        if (!args || args->t != J_OBJ) { json_free(args); free(args_arena); json_free(reply); free(reply_arena); free(reply_raw); free(messages.data); goto model_fail; }
        if (!strcmp(name->str, "ask_user")) {
            jval *question = json_get(args, "question");
            if (!question || question->t != J_STR || !*question->str) { json_free(args); free(args_arena); json_free(reply); free(reply_arena); free(reply_raw); free(messages.data); goto ask; }
            TextBuffer paused = {0}; char number[32]; snprintf(number, sizeof(number), "%d", seq);
            text_add(&paused, "{\"seq\":"); text_add(&paused, number); text_add(&paused, ",\"type\":\"await_user\",\"job_id\":");
            text_json_string(&paused, job_id); text_add(&paused, ",\"question\":"); text_json_string(&paused, question->str); text_add(&paused, "}");
            int ok = sse_json(fd, paused.data) && samosa_send_all(fd, "data: [DONE]\n\n", 14);
            free(paused.data); json_free(args); free(args_arena); json_free(reply); free(reply_arena); free(reply_raw); free(messages.data);
            json_free(listing); free(arena); free(list_raw); return ok;
        }
        jval *path = json_get(args, "path");
        TextBuffer call_event = {0}; char number[32]; snprintf(number, sizeof(number), "%d", seq++);
        text_add(&call_event, "{\"seq\":"); text_add(&call_event, number); text_add(&call_event, ",\"type\":\"tool_call\",\"tool\":");
        text_json_string(&call_event, name->str); text_add(&call_event, ",\"args\":"); text_add(&call_event, arguments->str); text_add(&call_event, "}");
        if (!sse_json(fd, call_event.data)) { free(call_event.data); json_free(args); free(args_arena); json_free(reply); free(reply_arena); free(reply_raw); free(messages.data); goto fail; }
        free(call_event.data);
        char *result = tool_result(g, folder, name->str, args);
        snprintf(number, sizeof(number), "%d", seq++);
        TextBuffer result_event = {0}; text_add(&result_event, "{\"seq\":"); text_add(&result_event, number);
        text_add(&result_event, ",\"type\":\"tool_result\",\"tool\":"); text_json_string(&result_event, name->str);
        text_add(&result_event, ",\"path\":"); text_json_string(&result_event, path && path->t == J_STR ? path->str : ""); text_add(&result_event, "}");
        if (!result || !sse_json(fd, result_event.data)) { free(result_event.data); free(result); json_free(args); free(args_arena); json_free(reply); free(reply_arena); free(reply_raw); free(messages.data); goto fail; }
        free(result_event.data);
        if (!text_add(&messages, ",")) { free(result); json_free(args); free(args_arena); json_free(reply); free(reply_arena); free(reply_raw); free(messages.data); goto fail; }
        TextBuffer assistant = {0};
        text_add(&assistant, "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":"); text_json_string(&assistant, id->str);
        text_add(&assistant, ",\"type\":\"function\",\"function\":{\"name\":"); text_json_string(&assistant, name->str);
        text_add(&assistant, ",\"arguments\":"); text_json_string(&assistant, arguments->str); text_add(&assistant, "}}]}");
        text_add(&messages, assistant.data); text_add(&messages, ",{\"role\":\"tool\",\"tool_call_id\":"); text_json_string(&messages, id->str);
        text_add(&messages, ",\"name\":"); text_json_string(&messages, name->str); text_add(&messages, ",\"content\":"); text_json_string(&messages, result); text_add(&messages, "}");
        free(assistant.data); free(result); json_free(args); free(args_arena); json_free(reply); free(reply_arena); free(reply_raw);
    }
    free(messages.data);
ask:
    {
        TextBuffer paused = {0}; char number[32]; snprintf(number, sizeof(number), "%d", seq);
        const char *question = (contains_case(goal, "cat") || contains_case(goal, "pet")) ?
            "I could not identify the right record yet. What is your pet's name?" :
            "What filename, name, date, or phrase should I use to narrow the search?";
        text_add(&paused, "{\"seq\":"); text_add(&paused, number); text_add(&paused, ",\"type\":\"await_user\",\"job_id\":");
        text_json_string(&paused, job_id); text_add(&paused, ",\"question\":"); text_json_string(&paused, question); text_add(&paused, "}");
        int ok = sse_json(fd, paused.data) && samosa_send_all(fd, "data: [DONE]\n\n", 14);
        free(paused.data); json_free(listing); free(arena); free(list_raw); return ok;
    }
model_fail:
    sse_json(fd, "{\"type\":\"error\",\"message\":\"The model could not complete this file search.\"}");
    samosa_send_all(fd, "data: [DONE]\n\n", 14);
fail:
    json_free(listing); free(arena); free(list_raw); return 0;
}

static int jobs_report(Gateway *g, int fd, const char *goal, const char *folder,
                       const char *existing_job_id) {
    char *argv[] = {g->samosa_fs, "survey", "--max-file-bytes", "104857600",
                    (char *)folder, NULL};
    int status = 0;
    char *raw = run_capture(g, g->samosa_fs, argv, 1 << 20, &status);
    if (!raw || !WIFEXITED(status) || WEXITSTATUS(status)) {
        free(raw); return samosa_http_json_error(fd, 400, "folder_scan_failed", "The folder could not be inspected.");
    }
    char *arena = NULL; jval *survey = json_parse(raw, &arena);
    jval *total = json_get(survey, "total"), *skipped = json_get(survey, "skipped_count");
    jval *types = json_get(survey, "by_type");
    if (!survey || survey->t != J_OBJ || !total || total->t != J_NUM ||
        !types || types->t != J_OBJ) {
        json_free(survey); free(arena); free(raw);
        return samosa_http_json_error(fd, 500, "invalid_sidecar_response", "The filesystem tool returned invalid data.");
    }
    if (!samosa_http_stream_headers(fd)) { json_free(survey); free(arena); free(raw); return 0; }
    char event[16384], job_id[64]; size_t used = 0;
    if (existing_job_id) path_copy(job_id, sizeof(job_id), existing_job_id);
    else snprintf(job_id, sizeof(job_id), "job-%ld-%ld", (long)time(NULL), (long)getpid());
    if (!save_job_state(g, job_id, goal, folder)) {
        json_free(survey); free(arena); free(raw);
        return samosa_http_json_error(fd, 500, "job_state_failed", "The job state could not be saved.");
    }
    used += (size_t)snprintf(event + used, sizeof(event) - used,
        "{\"seq\":1,\"type\":\"decode_intent\",\"job_id\":\"%s\",\"goal\":\"", job_id);
    if (!json_escape_to(event, sizeof(event), &used, goal)) goto fail;
    used += (size_t)snprintf(event + used, sizeof(event) - used, "\",\"folder\":\"");
    if (!json_escape_to(event, sizeof(event), &used, folder)) goto fail;
    used += (size_t)snprintf(event + used, sizeof(event) - used, "\"}");
    if (!sse_json(fd, event)) goto fail;
    int is_find = find_intent(goal);
    if (!sse_json(fd, is_find ?
        "{\"seq\":2,\"type\":\"intent\",\"kind\":\"find\",\"rule\":null,\"explain\":\"Search the complete filename index, then inspect likely matches with bounded reads.\"}" :
        "{\"seq\":2,\"type\":\"intent\",\"kind\":\"report\",\"rule\":null,\"explain\":\"Look through the folder and report what is there, by file type.\"}")) goto fail;
    used = (size_t)snprintf(event, sizeof(event),
        "{\"seq\":3,\"type\":\"counting\",\"total\":%d,\"skipped\":%d,\"by_type\":{",
        (int)total->num, skipped && skipped->t == J_NUM ? (int)skipped->num : 0);
    for (int i = 0; i < types->len; ++i) {
        jval *count = json_get(types->kids[i], "count");
        if (i) event[used++] = ',';
        event[used++] = '"';
        if (!json_escape_to(event, sizeof(event), &used, types->keys[i])) goto fail;
        used += (size_t)snprintf(event + used, sizeof(event) - used, "\":%d",
                                count && count->t == J_NUM ? (int)count->num : 0);
    }
    used += (size_t)snprintf(event + used, sizeof(event) - used, "}}");
    if (!sse_json(fd, event)) goto fail;
    if (is_find) {
        int ok = jobs_find(g, fd, goal, folder, survey, job_id, 4);
        json_free(survey); free(arena); free(raw); return ok;
    }
    event[0] = 0; used = (size_t)snprintf(event, sizeof(event),
        "{\"seq\":4,\"type\":\"report\",\"total\":%d,\"by_type\":{", (int)total->num);
    for (int i = 0; i < types->len; ++i) {
        jval *count = json_get(types->kids[i], "count");
        if (i) event[used++] = ',';
        event[used++] = '"';
        if (!json_escape_to(event, sizeof(event), &used, types->keys[i])) goto fail;
        used += (size_t)snprintf(event + used, sizeof(event) - used, "\":%d",
                                count && count->t == J_NUM ? (int)count->num : 0);
    }
    used += (size_t)snprintf(event + used, sizeof(event) - used, "}}");
    if (!sse_json(fd, event)) goto fail;
    used = (size_t)snprintf(event, sizeof(event),
        "{\"seq\":5,\"type\":\"done\",\"summary\":\"%d file%s inspected.\"}",
        (int)total->num, (int)total->num == 1 ? "" : "s");
    if (!sse_json(fd, event) || !samosa_send_all(fd, "data: [DONE]\n\n", 14)) goto fail;
    json_free(survey); free(arena); free(raw); return 1;
fail:
    json_free(survey); free(arena); free(raw); return 0;
}

static int jobs_run(Gateway *g, int fd, const SamosaHttpRequest *request) {
    char *arena = NULL; jval *root = json_parse(request->body, &arena);
    jval *goal = root && root->t == J_OBJ ? json_get(root, "goal") : NULL;
    jval *folder = root && root->t == J_OBJ ? json_get(root, "folder") : NULL;
    if (!goal || goal->t != J_STR || !folder || folder->t != J_STR) {
        json_free(root); free(arena);
        return samosa_http_json_error(fd, 400, "invalid_job", "goal and folder are required.");
    }
    char *goal_copy = strdup(goal->str), *folder_copy = strdup(folder->str);
    json_free(root); free(arena);
    if (!goal_copy || !folder_copy) { free(goal_copy); free(folder_copy); return 0; }
    int result = jobs_report(g, fd, goal_copy, folder_copy, NULL);
    free(goal_copy); free(folder_copy); return result;
}

static int jobs_answer(Gateway *g, int fd, const SamosaHttpRequest *request) {
    char *arena = NULL; jval *root = json_parse(request->body, &arena);
    jval *id = root && root->t == J_OBJ ? json_get(root, "job_id") : NULL;
    jval *answer = root && root->t == J_OBJ ? json_get(root, "answer") : NULL;
    if (!id || id->t != J_STR || !answer || answer->t != J_STR || !*answer->str) {
        json_free(root); free(arena);
        return samosa_http_json_error(fd, 400, "invalid_answer", "job_id and answer are required.");
    }
    char job_id[128]; path_copy(job_id, sizeof(job_id), id->str);
    char *answer_copy = strdup(answer->str), *goal = NULL, *folder = NULL;
    json_free(root); free(arena);
    if (!answer_copy || !load_job_state(g, job_id, &goal, &folder)) {
        free(answer_copy); free(goal); free(folder);
        return samosa_http_json_error(fd, 404, "job_not_found", "That paused job is unavailable.");
    }
    TextBuffer expanded = {0};
    int built = text_add(&expanded, goal) && text_add(&expanded, "\nAdditional detail from the user: ") &&
                text_add(&expanded, answer_copy);
    free(answer_copy); free(goal);
    if (!built) { free(expanded.data); free(folder); return 0; }
    int ok = jobs_report(g, fd, expanded.data, folder, job_id);
    free(expanded.data); free(folder); return ok;
}

static int review_pending(jval *record) {
    jval *status = record && record->t == J_OBJ ? json_get(record, "status") : NULL;
    return status && status->t == J_STR &&
           (!strcmp(status->str, "review_required") || !strcmp(status->str, "needs_review"));
}

static char *source_preview(jval *record) {
    jval *path = record && record->t == J_OBJ ? json_get(record, "input_path") : NULL;
    if (!path || path->t != J_STR) return strdup("");
    int fd = open(path->str, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return strdup("");
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return strdup(""); }
    char *text = malloc(4001); if (!text) { close(fd); return NULL; }
    ssize_t n = read(fd, text, 4000); close(fd);
    if (n < 0) { free(text); return strdup(""); }
    text[n] = 0; return text;
}

static int jobs_review(Gateway *g, int fd, const SamosaHttpRequest *request) {
    char *arena = NULL; jval *body = json_parse(request->body, &arena);
    jval *id = body && body->t == J_OBJ ? json_get(body, "job_id") : NULL;
    char path[PATH_MAX];
    if (!id || id->t != J_STR || !job_state_path(g, id->str, "results/output.jsonl", path, 0)) {
        json_free(body); free(arena);
        return samosa_http_json_error(fd, 400, "invalid_review", "A valid job_id is required.");
    }
    char *raw = read_file_limit(path, 16 << 20);
    if (!raw) { json_free(body); free(arena); return samosa_http_json_error(fd, 404, "review_not_found", "No review output exists for that job."); }
    TextBuffer items = {0}; int pending = 0, index = 0;
    char *save = NULL;
    for (char *line = strtok_r(raw, "\n", &save); line; line = strtok_r(NULL, "\n", &save), ++index) {
        char *line_arena = NULL; jval *record = json_parse(line, &line_arena);
        if (!review_pending(record)) { json_free(record); free(line_arena); continue; }
        jval *unit = json_get(record, "unit_id"), *input = json_get(record, "input_path");
        jval *fields = json_get(record, "extracted"), *reasons = json_get(record, "reasons");
        char *source = source_preview(record); char number[32]; snprintf(number, sizeof(number), "%d", index);
        if ((pending && !text_add(&items, ",")) || !text_add(&items, "{\"index\":") ||
            !text_add(&items, number) || !text_add(&items, ",\"unit_id\":") ||
            !text_json_string(&items, unit && unit->t == J_STR ? unit->str : "") ||
            !text_add(&items, ",\"input_path\":") || !text_json_string(&items, input && input->t == J_STR ? input->str : "") ||
            !text_add(&items, ",\"fields\":") || !text_json_value(&items, fields) ||
            !text_add(&items, ",\"reasons\":") || !text_json_value(&items, reasons) ||
            !text_add(&items, ",\"source\":") || !text_json_string(&items, source ? source : "") ||
            !text_add(&items, ",\"done\":false}")) {
            free(source); json_free(record); free(line_arena); free(items.data); free(raw); json_free(body); free(arena); return 0;
        }
        ++pending; free(source); json_free(record); free(line_arena);
    }
    TextBuffer response = {0}; char number[32]; snprintf(number, sizeof(number), "%d", pending);
    text_add(&response, "{\"ok\":true,\"pending\":"); text_add(&response, number);
    text_add(&response, ",\"items\":["); text_add(&response, items.data ? items.data : ""); text_add(&response, "]}");
    int ok = samosa_http_response(fd, 200, "application/json", response.data, NULL);
    free(response.data); free(items.data); free(raw); json_free(body); free(arena); return ok;
}

static int field_name(jval *fields, const char *name) {
    if (!fields || fields->t != J_OBJ) return 0;
    for (int i = 0; i < fields->len; ++i) if (!strcmp(fields->keys[i], name)) return 1;
    return 0;
}

static int write_corrected_record(TextBuffer *out, jval *record, jval *fields) {
    jval *existing = json_get(record, "extracted");
    jval *chosen = fields && fields->t == J_OBJ ? fields : existing;
    int first = 1;
    if (!text_add(out, "{")) return 0;
    for (int i = 0; i < record->len; ++i) {
        const char *key = record->keys[i];
        if (!strcmp(key, "status") || !strcmp(key, "reviewed") || !strcmp(key, "extracted") || field_name(chosen, key)) continue;
        if ((!first && !text_add(out, ",")) || !text_json_string(out, key) || !text_add(out, ":") ||
            !text_json_value(out, record->kids[i])) return 0;
        first = 0;
    }
    if (!first && !text_add(out, ",")) return 0;
    if (!text_add(out, "\"status\":\"passed\",\"reviewed\":true,\"extracted\":") ||
        !text_json_value(out, chosen)) return 0;
    if (chosen && chosen->t == J_OBJ) for (int i = 0; i < chosen->len; ++i)
        if (!text_add(out, ",") || !text_json_string(out, chosen->keys[i]) || !text_add(out, ":") ||
            !text_json_value(out, chosen->kids[i])) return 0;
    return text_add(out, "}");
}

static int jobs_review_correct(Gateway *g, int fd, const SamosaHttpRequest *request) {
    char *arena = NULL; jval *body = json_parse(request->body, &arena);
    jval *id = body && body->t == J_OBJ ? json_get(body, "job_id") : NULL;
    jval *wanted = body && body->t == J_OBJ ? json_get(body, "item") : NULL;
    jval *fields = body && body->t == J_OBJ ? json_get(body, "fields") : NULL;
    char path[PATH_MAX];
    if (!id || id->t != J_STR || !wanted || wanted->t != J_OBJ ||
        !job_state_path(g, id->str, "results/output.jsonl", path, 0)) {
        json_free(body); free(arena); return samosa_http_json_error(fd, 400, "invalid_correction", "job_id and item are required.");
    }
    char *raw = read_file_limit(path, 16 << 20); if (!raw) { json_free(body); free(arena); return samosa_http_json_error(fd, 404, "review_not_found", "No review output exists for that job."); }
    jval *wanted_unit = json_get(wanted, "unit_id"), *wanted_index = json_get(wanted, "index");
    TextBuffer output = {0}; int index = 0, found = 0, pending = 0; char *save = NULL;
    for (char *line = strtok_r(raw, "\n", &save); line; line = strtok_r(NULL, "\n", &save), ++index) {
        char *line_arena = NULL; jval *record = json_parse(line, &line_arena); jval *unit = json_get(record, "unit_id");
        int match = !found && ((wanted_unit && wanted_unit->t == J_STR && unit && unit->t == J_STR && !strcmp(wanted_unit->str, unit->str)) ||
                    (wanted_index && wanted_index->t == J_NUM && (int)wanted_index->num == index));
        int ok = match ? write_corrected_record(&output, record, fields) : text_json_value(&output, record);
        if (match) found = 1; else if (review_pending(record)) ++pending;
        if (!ok || !text_add(&output, "\n")) { json_free(record); free(line_arena); free(output.data); free(raw); json_free(body); free(arena); return 0; }
        json_free(record); free(line_arena);
    }
    if (!found) { free(output.data); free(raw); json_free(body); free(arena); return samosa_http_json_error(fd, 404, "review_item_not_found", "That review item is unavailable."); }
    int saved = write_small_file(path, output.data); free(output.data); free(raw);
    if (!saved) { json_free(body); free(arena); return samosa_http_json_error(fd, 500, "review_save_failed", "The correction could not be saved."); }
    char response[160]; snprintf(response, sizeof(response), "{\"ok\":true,\"pending\":%d,\"item\":{\"done\":true}}", pending);
    json_free(body); free(arena); return samosa_http_response(fd, 200, "application/json", response, NULL);
}

static int backend_available(Gateway *g, const char *name) {
    if (!strcmp(name, "qwen")) {
        char experts[PATH_MAX];
        return path_join(experts, sizeof(experts), g->qwen_model, "experts.bin") &&
               regular_file(g->qwen_engine, 1) && regular_file(experts, 0);
    }
    if (!strcmp(name, "bonsai"))
        return regular_file(g->llama_server, 1) && regular_file(g->bonsai_model, 0);
    if (!strcmp(name, "ornith"))
        return regular_file(g->llama_server, 1) && regular_file(g->ornith_model, 0);
    return 0;
}

static int tcp_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address))) {
        close(fd);
        return -1;
    }
    return fd;
}

static int backend_probe(Gateway *g) {
    int fd = tcp_connect(g->backend_port);
    if (fd < 0) return 0;
    const char *path = !strcmp(g->backend, "qwen") ? "/healthz" : "/health";
    char request[256];
    int n = snprintf(request, sizeof(request),
                     "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\n\r\n",
                     path, g->backend_port);
    char response[64] = {0};
    int ok = n > 0 && samosa_send_all(fd, request, (size_t)n) &&
             recv(fd, response, sizeof(response) - 1, 0) > 0 &&
             strstr(response, " 200 ") != NULL;
    close(fd);
    return ok;
}

static void backend_stop(Gateway *g) {
    pthread_mutex_lock(&g->mu);
    pid_t pid = g->backend_pid;
    int upstream = g->upstream_fd;
    g->backend_pid = 0;
    g->upstream_fd = -1;
    pthread_mutex_unlock(&g->mu);
    if (upstream >= 0) shutdown(upstream, SHUT_RDWR);
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 80; ++i) {
        if (waitpid(pid, NULL, WNOHANG) == pid) return;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
        nanosleep(&pause, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static void jobs_stop(Gateway *g) {
    pid_t pids[16] = {0};
    pthread_mutex_lock(&g->mu);
    memcpy(pids, g->job_pids, sizeof(pids));
    memset(g->job_pids, 0, sizeof(g->job_pids));
    pthread_mutex_unlock(&g->mu);
    for (size_t i = 0; i < sizeof(pids) / sizeof(pids[0]); ++i)
        if (pids[i] > 0) kill(pids[i], SIGKILL);
}

static int backend_start(Gateway *g) {
    if (!backend_available(g, g->backend)) return 0;
    char chats[PATH_MAX];
    if (!path_join(chats, sizeof(chats), g->home, "chats") || !mkdirs(chats)) return 0;
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        int log = open(g->backend_log, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (log >= 0) { dup2(log, STDOUT_FILENO); dup2(log, STDERR_FILENO); close(log); }
        char port[16];
        snprintf(port, sizeof(port), "%d", g->backend_port);
        if (!strcmp(g->backend, "qwen")) {
            setenv("SNAP", g->qwen_model, 1);
            setenv("TOKENIZER", g->tokenizer, 1);
            setenv("SAMOSA_CHATS_DIR", chats, 1);
            execl(g->qwen_engine, g->qwen_engine, "--serve", "--port", port,
                  "--tokenizer", g->tokenizer, (char *)NULL);
        } else {
            const char *model = !strcmp(g->backend, "ornith") ?
                                g->ornith_model : g->bonsai_model;
            const char *alias = !strcmp(g->backend, "ornith") ?
                                "ornith-1.0-9b" : "bonsai-27b-1bit";
            execl(g->llama_server, g->llama_server, "-m", model, "-ngl", "99",
                  "-c", "8192", "-np", "1", "--cache-ram", "0", "--host",
                  "127.0.0.1", "--port", port, "--no-ui", "--alias", alias,
                  (char *)NULL);
        }
        _Exit(127);
    }
    pthread_mutex_lock(&g->mu);
    g->backend_pid = pid;
    pthread_mutex_unlock(&g->mu);
    return 1;
}

static int static_file(int fd, const char *path, const char *type, const char *extra) {
    int file = open(path, O_RDONLY | O_NOFOLLOW);
    if (file < 0) return 0;
    struct stat st;
    if (fstat(file, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > (4 << 20)) {
        close(file); return 0;
    }
    size_t size = (size_t)st.st_size;
    char *data = malloc(size ? size : 1);
    if (!data) { close(file); return 0; }
    size_t used = 0;
    while (used < size) {
        ssize_t n = read(file, data + used, size - used);
        if (n <= 0) { free(data); close(file); return 0; }
        used += (size_t)n;
    }
    close(file);
    int ok = samosa_http_headers(fd, 200, type, size, extra) &&
             (!size || samosa_send_all(fd, data, size));
    free(data);
    return ok;
}

static int proxy_request(Gateway *g, int client, const SamosaHttpRequest *request) {
    if (!backend_probe(g))
        return samosa_http_json_error(client, 503, "backend_loading", "The model is still loading.");
    int upstream = tcp_connect(g->backend_port);
    if (upstream < 0)
        return samosa_http_json_error(client, 503, "backend_unavailable", "The model backend is unavailable.");
    pthread_mutex_lock(&g->mu); g->upstream_fd = upstream; pthread_mutex_unlock(&g->mu);
    atomic_store(&g->generating, 1);
    char header[1024];
    int n = snprintf(header, sizeof(header),
        "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        request->method, request->path, g->backend_port, request->body_len);
    int ok = n > 0 && (size_t)n < sizeof(header) &&
             samosa_send_all(upstream, header, (size_t)n) &&
             (!request->body_len || samosa_send_all(upstream, request->body, request->body_len));
    char buffer[65536];
    while (ok) {
        ssize_t got = recv(upstream, buffer, sizeof(buffer), 0);
        if (got == 0) break;
        if (got < 0) { if (errno == EINTR) continue; ok = 0; break; }
        if (!samosa_send_all(client, buffer, (size_t)got)) { ok = 0; break; }
    }
    pthread_mutex_lock(&g->mu);
    if (g->upstream_fd == upstream) g->upstream_fd = -1;
    pthread_mutex_unlock(&g->mu);
    close(upstream);
    atomic_store(&g->generating, 0);
    return ok;
}

static const char *backend_label(const char *name) {
    if (!strcmp(name, "ornith")) return "Ornith 9B";
    if (!strcmp(name, "bonsai")) return "Bonsai 27B 1-bit";
    return "Qwen3.6 35B A3B";
}

static const char *backend_model(const char *name) {
    if (!strcmp(name, "ornith")) return "ornith-1.0-9b";
    if (!strcmp(name, "bonsai")) return "bonsai-27b-1bit";
    return "qwen3.6-35b-a3b";
}

static int gateway_handler(SamosaHttpServer *server, int fd,
                           const SamosaHttpRequest *request, void *opaque) {
    Gateway *g = opaque;
    if (!strcmp(request->method, "GET") &&
        (!strcmp(request->path, "/") || !strcmp(request->path, "/index.html"))) {
        const char *policy = "Content-Security-Policy: default-src 'self'; img-src 'self' data: blob:; "
            "style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; "
            "object-src 'none'; base-uri 'none'; frame-ancestors 'none'\r\n";
        if (static_file(fd, g->app_html, "text/html; charset=utf-8", policy)) return 1;
        return samosa_http_json_error(fd, 404, "app_missing", "The app asset is missing.");
    }
    if (!strcmp(request->method, "GET") && !strcmp(request->path, "/assets/samosa-chat.png")) {
        if (static_file(fd, g->app_logo, "image/png", NULL)) return 1;
        return samosa_http_json_error(fd, 404, "logo_missing", "The app logo is missing.");
    }
    if (!strcmp(request->method, "GET") && !strcmp(request->path, "/healthz")) {
        char body[768];
        pthread_mutex_lock(&g->mu); pid_t pid = g->backend_pid; pthread_mutex_unlock(&g->mu);
        int ready = backend_probe(g);
        snprintf(body, sizeof(body),
            "{\"gateway\":true,\"compiled\":true,\"backend\":\"%s\","
            "\"label\":\"%s\",\"model\":\"%s\",\"supports_images\":%s,"
            "\"ready\":%s,\"loading\":%s,\"generating\":%s,\"pid\":%ld}",
            g->backend, backend_label(g->backend), backend_model(g->backend),
            !strcmp(g->backend, "qwen") ? "true" : "false",
            ready ? "true" : "false", (!ready && pid > 0) ? "true" : "false",
            atomic_load(&g->generating) ? "true" : "false", (long)pid);
        return samosa_http_response(fd, 200, "application/json", body, NULL);
    }
    if (!strcmp(request->method, "GET") && !strcmp(request->path, "/v1/backends")) {
        char body[1536];
        snprintf(body, sizeof(body),
            "{\"active\":\"%s\",\"backends\":["
            "{\"id\":\"bonsai\",\"label\":\"Bonsai 27B 1-bit\",\"model\":\"bonsai-27b-1bit\",\"supports_images\":false,\"available\":%s},"
            "{\"id\":\"ornith\",\"label\":\"Ornith 9B\",\"model\":\"ornith-1.0-9b\",\"supports_images\":false,\"available\":%s},"
            "{\"id\":\"qwen\",\"label\":\"Qwen3.6 35B A3B\",\"model\":\"qwen3.6-35b-a3b\",\"supports_images\":true,\"available\":%s}]}",
            g->backend, backend_available(g, "bonsai") ? "true" : "false",
            backend_available(g, "ornith") ? "true" : "false",
            backend_available(g, "qwen") ? "true" : "false");
        return samosa_http_response(fd, 200, "application/json", body, NULL);
    }
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/backends/select")) {
        char *arena = NULL;
        jval *root = json_parse(request->body, &arena);
        jval *selected = root && root->t == J_OBJ ? json_get(root, "backend") : NULL;
        if (!selected || selected->t != J_STR ||
            (strcmp(selected->str, "qwen") && strcmp(selected->str, "bonsai") &&
             strcmp(selected->str, "ornith"))) {
            json_free(root); free(arena);
            return samosa_http_json_error(fd, 400, "invalid_backend", "Unknown model backend.");
        }
        if (!backend_available(g, selected->str)) {
            json_free(root); free(arena);
            return samosa_http_json_error(fd, 409, "backend_unavailable", "That model backend is not installed.");
        }
        char name[16]; path_copy(name, sizeof(name), selected->str);
        json_free(root); free(arena);
        if (atomic_load(&g->generating))
            return samosa_http_json_error(fd, 409, "generation_active", "Stop the current response before switching models.");
        if (strcmp(name, g->backend)) {
            backend_stop(g);
            path_copy(g->backend, sizeof(g->backend), name);
            char persisted[32]; snprintf(persisted, sizeof(persisted), "%s\n", name);
            if (!write_small_file(g->selection_file, persisted) || !backend_start(g))
                return samosa_http_json_error(fd, 500, "backend_start_failed", "The selected model could not be started.");
        }
        return samosa_http_response(fd, 202, "application/json", "{\"accepted\":true}", NULL);
    }
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/cancel")) {
        pthread_mutex_lock(&g->mu); int upstream = g->upstream_fd; pthread_mutex_unlock(&g->mu);
        if (upstream >= 0) shutdown(upstream, SHUT_RDWR);
        return samosa_http_response(fd, 200, "application/json",
                                    upstream >= 0 ? "{\"cancelled\":true}" : "{\"cancelled\":false}", NULL);
    }
    if (!strcmp(request->method, "POST") &&
        (!strcmp(request->path, "/v1/shutdown") || !strcmp(request->path, "/v1/kill"))) {
        atomic_store(&g->stopping, 1);
        samosa_http_response(fd, 200, "application/json", "{\"stopping\":true}", NULL);
        jobs_stop(g);
        backend_stop(g);
        samosa_http_server_stop(server);
        return 1;
    }
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/jobs/run"))
        return jobs_run(g, fd, request);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/jobs/answer"))
        return jobs_answer(g, fd, request);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/jobs/review"))
        return jobs_review(g, fd, request);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/jobs/review/correct"))
        return jobs_review_correct(g, fd, request);
    if (!strcmp(request->path, "/v1/chat/completions") ||
        !strcmp(request->path, "/v1/models"))
        return proxy_request(g, fd, request);
    if (!strncmp(request->path, "/v1/jobs/", 9))
        return samosa_http_json_error(fd, 503, "jobs_port_in_progress",
                                      "The compiled Jobs controller is not available yet.");
    return samosa_http_json_error(fd, 404, "not_found", "Endpoint not found.");
}

static void on_signal(int number) {
    (void)number;
    if (!signal_gateway) return;
    atomic_store(&signal_gateway->stopping, 1);
    if (signal_gateway->server) samosa_http_server_stop(signal_gateway->server);
}

static int load_config(Gateway *g) {
    memset(g, 0, sizeof(*g));
    g->backend_pid = 0; g->upstream_fd = -1;
    pthread_mutex_init(&g->mu, NULL);
    atomic_init(&g->generating, 0); atomic_init(&g->stopping, 0);
    const char *home = getenv("SAMOSA_HOME");
    const char *user_home = getenv("HOME");
    if (!home) {
        if (!user_home || snprintf(g->home, sizeof(g->home), "%s/.samosa", user_home) >=
                          (int)sizeof(g->home)) return 0;
    } else if (!path_copy(g->home, sizeof(g->home), home)) return 0;
    g->public_port = getenv("SAMOSA_PORT") ? atoi(getenv("SAMOSA_PORT")) : 8642;
    g->backend_port = getenv("SAMOSA_BACKEND_PORT") ? atoi(getenv("SAMOSA_BACKEND_PORT")) : g->public_port + 1;
#define ENV_PATH(field, name, fallback) do { const char *v = getenv(name); \
    if (v) { if (!path_copy(g->field, sizeof(g->field), v)) return 0; } \
    else { if (!path_join(g->field, sizeof(g->field), g->home, fallback)) return 0; } } while (0)
    ENV_PATH(app_html, "SAMOSA_APP_HTML", "current/app.html");
    ENV_PATH(app_logo, "SAMOSA_APP_LOGO", "current/samosa-chat.png");
    ENV_PATH(qwen_engine, "SAMOSA_QWEN_ENGINE", "current/bin/qwen36b");
    ENV_PATH(qwen_model, "SAMOSA_QWEN_MODEL", "current/model");
    ENV_PATH(tokenizer, "SAMOSA_TOKENIZER", "current/tokenizer_qwen36.json");
    ENV_PATH(llama_server, "SAMOSA_BONSAI_SERVER", "backends/prism-llama.cpp/build/bin/llama-server");
    ENV_PATH(bonsai_model, "SAMOSA_BONSAI_MODEL", "models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf");
    ENV_PATH(ornith_model, "SAMOSA_ORNITH_MODEL", "models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf");
    ENV_PATH(samosa_fs, "SAMOSA_FS", "current/bin/samosa-fs");
    ENV_PATH(samosa_extract, "SAMOSA_EXTRACT", "current/bin/samosa-extract");
#undef ENV_PATH
    const char *jobs_root = getenv("SAMOSA_JOBS_ROOT");
    if (jobs_root ? !path_copy(g->jobs_root, sizeof(g->jobs_root), jobs_root) :
                    !path_join(g->jobs_root, sizeof(g->jobs_root), g->home, "jobs")) return 0;
    if (!path_join(g->backend_log, sizeof(g->backend_log), g->home, "backend.log") ||
        !path_join(g->selection_file, sizeof(g->selection_file), g->home, "model-backend") ||
        !mkdirs(g->home)) return 0;
    char selected[32] = {0};
    if (read_small_file(g->selection_file, selected, sizeof(selected)) &&
        backend_available(g, selected)) path_copy(g->backend, sizeof(g->backend), selected);
    else if (backend_available(g, "ornith")) path_copy(g->backend, sizeof(g->backend), "ornith");
    else if (backend_available(g, "bonsai")) path_copy(g->backend, sizeof(g->backend), "bonsai");
    else path_copy(g->backend, sizeof(g->backend), "qwen");
    return g->public_port > 0 && g->public_port < 65536 &&
           g->backend_port > 0 && g->backend_port < 65536;
}

int main(void) {
    Gateway gateway;
    if (!load_config(&gateway)) {
        fprintf(stderr, "samosa-gateway: invalid configuration\n"); return 2;
    }
    if (!backend_start(&gateway)) {
        fprintf(stderr, "samosa-gateway: backend %s is not installed\n", gateway.backend); return 2;
    }
    SamosaHttpServer server;
    if (!samosa_http_server_init(&server, gateway.public_port, gateway_handler, &gateway)) {
        fprintf(stderr, "samosa-gateway: cannot bind 127.0.0.1:%d: %s\n",
                gateway.public_port, strerror(errno)); backend_stop(&gateway); return 2;
    }
    gateway.server = &server; signal_gateway = &gateway;
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    fprintf(stderr, "[gateway] compiled ready http://127.0.0.1:%d backend=%s\n",
            server.port, gateway.backend); fflush(stderr);
    int ok = samosa_http_server_run(&server);
    jobs_stop(&gateway);
    backend_stop(&gateway);
    samosa_http_server_destroy(&server);
    pthread_mutex_destroy(&gateway.mu);
    signal_gateway = NULL;
    return ok ? 0 : 2;
}
