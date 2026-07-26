/* Compiled Samosa gateway: local app, backend supervision, and raw API proxy. */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "json.h"
#include "samosa_http.h"
#include "read_cache.h"

typedef struct {
    SamosaHttpServer *server;
    pthread_mutex_t mu;
    pid_t backend_pid;
    pid_t job_pids[16];
    int upstream_fd;
    atomic_int generating;
    atomic_int interactive_active;
    atomic_llong last_interactive_mono_ms;
    atomic_llong last_interactive_wall_ms;
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
    char bonsai_mmproj[PATH_MAX];
    char ornith_model[PATH_MAX];
    char samosa_fs[PATH_MAX];
    char samosa_extract[PATH_MAX];
    char samosa_ocr[PATH_MAX];
    char backend_log[PATH_MAX];
    char selection_file[PATH_MAX];
} Gateway;

#define MAX_PUBLIC_JOB_URLS 20
#define MAX_PUBLIC_FETCH_BYTES (5u << 20)
#define MAX_PUBLIC_TEXT_BYTES 120000
#define MAX_DEFINITION_IMAGE_BYTES (3u << 20)

static int tcp_connect(int port);
static int backend_probe(Gateway *g);
static const char *backend_model(const char *name);
static int backend_supports_images(Gateway *g, const char *name);
static int sse_json(int fd, const char *json);

static Gateway *signal_gateway;

static long long monotonic_millis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static double monotonic_seconds(void) {
    return monotonic_millis() / 1000.0;
}

static long long wall_millis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void sleep_millis(long ms) {
    if (ms <= 0) return;
    struct timespec pause = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&pause, &pause) && errno == EINTR) {}
}

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

/* Fork/exec a long-lived helper (e.g. caffeinate) whose stdout we discard and
   whose pid we track so a Kill tears it down with everything else. */
static pid_t spawn_tracked(Gateway *g, const char *program, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); dup2(devnull, STDOUT_FILENO); close(devnull); }
        execv(program, argv); _Exit(127);
    }
    track_job_pid(g, pid, 1);
    return pid;
}

static void stop_tracked(Gateway *g, pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        if (waitpid(pid, NULL, WNOHANG) == pid) { track_job_pid(g, pid, 0); return; }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 50000000};
        nanosleep(&pause, NULL);
    }
    kill(pid, SIGKILL); waitpid(pid, NULL, 0); track_job_pid(g, pid, 0);
}

/* Prevent system sleep for the lifetime of a scheduled run. macOS only; a no-op
   elsewhere. The pid is tracked, so a Kill also releases the assertion. */
static pid_t spawn_keep_awake(Gateway *g) {
#ifdef __APPLE__
    char *argv[] = {(char *)"/usr/bin/caffeinate", (char *)"-s", NULL};
    return spawn_tracked(g, "/usr/bin/caffeinate", argv);
#else
    (void)g; return -1;
#endif
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

static uint64_t stable_hash_bytes(const unsigned char *data, size_t length) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < length; ++i) {
        h ^= (uint64_t)data[i];
        h *= 1099511628211ull;
    }
    return h;
}

static int text_hash_hex(TextBuffer *out, const void *data, size_t length) {
    char hex[17];
    snprintf(hex, sizeof(hex), "%016llx",
             (unsigned long long)stable_hash_bytes((const unsigned char *)data, length));
    return text_add(out, hex);
}

static const char *path_basename_const(const char *path) {
    const char *slash = strrchr(path ? path : "", '/');
    return slash ? slash + 1 : (path ? path : "");
}

static int valid_job_id(const char *job_id) {
    if (!job_id || !*job_id) return 0;
    for (const char *p = job_id; *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' )) return 0;
    return 1;
}

static int slugify_to(char *out, size_t cap, const char *text) {
    size_t used = 0; int dash = 0;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p && used + 1 < cap; ++p) {
        char c = 0;
        if (*p >= 'A' && *p <= 'Z') c = (char)(*p - 'A' + 'a');
        else if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) c = (char)*p;
        else dash = used > 0;
        if (c) {
            if (dash && used + 1 < cap) out[used++] = '-';
            dash = 0; out[used++] = c;
        }
    }
    while (used && out[used - 1] == '-') --used;
    if (!used) {
        if (cap < 4) return 0;
        memcpy(out, "job", 4); return 1;
    }
    out[used] = 0; return 1;
}

static int rfc3339_now_to(char *out, size_t cap) {
    time_t now = time(NULL);
    struct tm tmv;
    if (!gmtime_r(&now, &tmv)) return 0;
    return strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tmv) > 0;
}

static unsigned char *read_file_bytes_limit(const char *path, size_t limit, size_t *out_len) {
    if (out_len) *out_len = 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (size_t)st.st_size > limit) {
        close(fd); return NULL;
    }
    unsigned char *data = malloc((size_t)st.st_size + 1);
    if (!data) { close(fd); return NULL; }
    size_t used = 0, size = (size_t)st.st_size;
    while (used < size) {
        ssize_t n = read(fd, data + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(data); close(fd); return NULL; }
        used += (size_t)n;
    }
    close(fd); data[size] = 0;
    if (out_len) *out_len = size;
    return data;
}

static char *read_file_limit(const char *path, size_t limit) {
    return (char *)read_file_bytes_limit(path, limit, NULL);
}

static char *base64_encode_bytes(const unsigned char *data, size_t length) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (length > (SIZE_MAX - 4) / 4 * 3) return NULL;
    size_t out_len = ((length + 2) / 3) * 4;
    char *out = malloc(out_len + 1);
    if (!out) return NULL;
    size_t i = 0, j = 0;
    while (i < length) {
        unsigned a = data[i++];
        unsigned b = i < length ? data[i++] : 0;
        unsigned c = i < length ? data[i++] : 0;
        unsigned triple = (a << 16) | (b << 8) | c;
        out[j++] = alphabet[(triple >> 18) & 63];
        out[j++] = alphabet[(triple >> 12) & 63];
        out[j++] = (i - 1) <= length ? alphabet[(triple >> 6) & 63] : '=';
        out[j++] = i <= length ? alphabet[triple & 63] : '=';
    }
    if (length % 3 == 1) out[out_len - 2] = out[out_len - 1] = '=';
    else if (length % 3 == 2) out[out_len - 1] = '=';
    out[out_len] = 0;
    return out;
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

/* -------- Phase JI: model-driven find pipeline (TASKS_JOBS_INTELLIGENCE.md) --
   The gateway executes; the model decides. C never tokenizes, scores, or
   otherwise interprets natural-language goal text or file contents. C owns the
   path jail, budgets, caching, event streaming, the durable job conversation,
   and the finish contract. Pause == resume: convo.json + phase.json persist
   every step so a question, a budget checkpoint, or a crash never repays work
   already paid for (design laws 1 and 3). */
#define JI_SCHEMA_VERSION        1
#define JI_TRIAGE_BATCH_TOKENS   500  /* compact coded verdicts; leave context headroom */
#define JI_CLASSIFY_BATCH_TOKENS 500
#define JI_BATCH_MAX_FILES       16
#define JI_SKIM_CHARS            400
#define JI_SKIM_MAX_FILES        300
#define JI_SKIM_MAX_SECONDS      1800
#define JI_SKIM_REFERENCE_CS     114  /* 1.14 s/file, observed E-JI1 M3 2026-07-23 */
#define JI_VERIFY_MAX_ROUNDS     24

static int save_job_state(Gateway *g, const char *job_id, const char *goal,
                          const char *folder) {
    char path[PATH_MAX], created[32]; TextBuffer json = {0};
    if (!rfc3339_now_to(created, sizeof(created))) created[0] = 0;
    if (!job_state_path(g, job_id, "job.json", path, 1) ||
        !text_add(&json, "{\"job_id\":") || !text_json_string(&json, job_id) ||
        !text_add(&json, ",\"goal\":") || !text_json_string(&json, goal) ||
        !text_add(&json, ",\"folder\":") || !text_json_string(&json, folder) ||
        !text_add(&json, ",\"created\":") || !text_json_string(&json, created) ||
        !text_add(&json, ",\"schema_version\":1}\n")) { free(json.data); return 0; }
    int ok = write_small_file(path, json.data); free(json.data); return ok;
}

/* Append one complete JSON line to <job_dir>/<name> (verdicts/skim jsonl). */
static int job_append_jsonl(Gateway *g, const char *job_id, const char *name,
                            const char *line) {
    char path[PATH_MAX];
    if (!job_state_path(g, job_id, name, path, 1)) return 0;
    int out = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
    if (out < 0) return 0;
    /* A regular-file write loop — samosa_send_all is send(), socket-only. */
    TextBuffer row = {0};
    int ok = text_add(&row, line) && text_add(&row, "\n");
    size_t off = 0, len = ok ? row.len : 0;
    while (ok && off < len) {
        ssize_t n = write(out, row.data + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = 0; break; }
        off += (size_t)n;
    }
    free(row.data);
    if (fsync(out)) ok = 0;
    if (close(out)) ok = 0;
    return ok;
}

/* convo.json holds the full model conversation as {"messages":[...]} so the
   verify loop can be re-entered in place after any pause (JI.6). `messages` is
   the inner array text without the surrounding brackets. */
static int save_convo(Gateway *g, const char *job_id, const char *messages) {
    char path[PATH_MAX]; TextBuffer json = {0};
    if (!job_state_path(g, job_id, "convo.json", path, 1) ||
        !text_add(&json, "{\"messages\":[") || !text_add(&json, messages) ||
        !text_add(&json, "]}\n")) { free(json.data); return 0; }
    int ok = write_small_file(path, json.data); free(json.data); return ok;
}

/* Returns the inner messages-array text (heap, no surrounding brackets), or
   NULL when convo.json is missing or malformed. Caller frees. */
static char *load_convo(Gateway *g, const char *job_id) {
    char path[PATH_MAX], *raw = NULL, *arena = NULL; jval *root = NULL, *msgs = NULL;
    char *out = NULL;
    if (!job_state_path(g, job_id, "convo.json", path, 0) ||
        !(raw = read_file_limit(path, 8 << 20)) ||
        !(root = json_parse(raw, &arena)) || root->t != J_OBJ) goto done;
    msgs = json_get(root, "messages");
    if (!msgs || msgs->t != J_ARR) goto done;
    TextBuffer buf = {0}; int ok = 1;
    for (int i = 0; ok && i < msgs->len; ++i)
        ok = (!i || text_add(&buf, ",")) && text_json_value(&buf, msgs->kids[i]);
    if (ok) out = buf.data ? buf.data : strdup("");
    else free(buf.data);
done:
    json_free(root); free(arena); free(raw); return out;
}

/* phase.json records where a paused job resumes: which phase, a cursor into it,
   and the running counts an honest progress line binds to (design law 4). */
static int save_phase(Gateway *g, const char *job_id, const char *phase,
                      int cursor, int total, int shortlist, int rounds_spent) {
    char path[PATH_MAX], tail[192]; TextBuffer json = {0};
    snprintf(tail, sizeof(tail),
             "\",\"cursor\":%d,\"total\":%d,\"shortlist\":%d,\"rounds_spent\":%d}\n",
             cursor, total, shortlist, rounds_spent);
    if (!job_state_path(g, job_id, "phase.json", path, 1) ||
        !text_add(&json, "{\"phase\":\"") || !text_add(&json, phase) ||
        !text_add(&json, tail)) { free(json.data); return 0; }
    int ok = write_small_file(path, json.data); free(json.data); return ok;
}

static char *load_phase_name(Gateway *g, const char *job_id) {
    char path[PATH_MAX], *raw = NULL, *arena = NULL, *out = NULL; jval *root = NULL;
    if (!job_state_path(g, job_id, "phase.json", path, 0) ||
        !(raw = read_file_limit(path, 65536)) || !(root = json_parse(raw, &arena))) goto done;
    jval *phase = root->t == J_OBJ ? json_get(root, "phase") : NULL;
    if (phase && phase->t == J_STR) out = strdup(phase->str);
done:
    json_free(root); free(arena); free(raw); return out;
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

static int parse_hhmm(const char *value) {
    int h = -1, m = -1; char tail = 0;
    if (!value || sscanf(value, "%d:%d%c", &h, &m, &tail) != 2 ||
        h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

static int minutes_in_window(int now, int start, int end) {
    if (start == end) return 1;
    if (start < end) return now >= start && now < end;
    return now >= start || now < end;
}

static int current_minutes_local(void) {
    time_t now = time(NULL);
    struct tm tmv;
    if (!localtime_r(&now, &tmv)) return 0;
    return tmv.tm_hour * 60 + tmv.tm_min;
}

/* First wall-clock instant at or after `from` whose local time-of-day equals
   end_minutes. This is the deadline of the window instance a schedule is
   targeting; once the clock passes it without a run, the window was missed. */
static long window_deadline_epoch(int end_minutes, time_t from) {
    struct tm tmv;
    if (!localtime_r(&from, &tmv)) return 0;
    tmv.tm_hour = end_minutes / 60;
    tmv.tm_min = end_minutes % 60;
    tmv.tm_sec = 0;
    tmv.tm_isdst = -1;
    time_t candidate = mktime(&tmv);
    if (candidate == (time_t)-1) return 0;
    if (candidate < from) {
        tmv.tm_mday += 1;
        tmv.tm_isdst = -1;
        candidate = mktime(&tmv);
        if (candidate == (time_t)-1) return 0;
    }
    return (long)candidate;
}

static int host_on_battery(void) {
#ifdef __APPLE__
    FILE *pipe = popen("/usr/bin/pmset -g batt 2>/dev/null", "r");
    if (!pipe) return 1;
    char data[512] = {0};
    size_t n = fread(data, 1, sizeof(data) - 1, pipe);
    pclose(pipe); data[n] = 0;
    return strstr(data, "Battery Power") != NULL;
#else
    return 0;
#endif
}

/* Returns 1 if the schedule should run now, 0 to defer. `window_expired` is true
   when the target window's deadline has already passed without a run — that is
   what distinguishes a missed window (run_next_start catches up) from simply
   being early in the day (outside_window: wait for tonight). */
static int schedule_decision(jval *schedule, int now_minutes, int on_battery,
                             int window_expired, char *reason, size_t reason_cap) {
    jval *enabled = json_get(schedule, "enabled");
    jval *status = json_get(schedule, "last_status");
    if (enabled && enabled->t == J_BOOL && !enabled->boolean) {
        path_copy(reason, reason_cap, "disabled"); return 0;
    }
    if (status && status->t == J_STR && !strcmp(status->str, "complete")) {
        path_copy(reason, reason_cap, "complete"); return 0;
    }
    jval *run_batt = json_get(schedule, "run_on_battery");
    if (on_battery && !(run_batt && run_batt->t == J_BOOL && run_batt->boolean)) {
        path_copy(reason, reason_cap, "on_battery"); return 0;
    }
    jval *ws = json_get(schedule, "window_start"), *we = json_get(schedule, "window_end");
    int start = parse_hhmm(ws && ws->t == J_STR ? ws->str : "22:00");
    int end = parse_hhmm(we && we->t == J_STR ? we->str : "06:00");
    if (start < 0 || end < 0) { path_copy(reason, reason_cap, "invalid_window"); return 0; }
    if (minutes_in_window(now_minutes, start, end)) {
        path_copy(reason, reason_cap, "inside_window"); return 1;
    }
    jval *policy = json_get(schedule, "missed_policy");
    int run_next_start = policy && policy->t == J_STR && !strcmp(policy->str, "run_next_start");
    if (window_expired) {
        if (run_next_start) { path_copy(reason, reason_cap, "missed_window"); return 1; }
        path_copy(reason, reason_cap, "window_expired"); return 0;
    }
    path_copy(reason, reason_cap, "outside_window"); return 0;
}

static int write_schedule_with_status(const char *path, jval *schedule, const char *status,
                                      int enabled, const char *reason) {
    TextBuffer out = {0}; int wrote_enabled = 0, wrote_status = 0, wrote_reason = 0;
    if (!text_add(&out, "{")) goto fail;
    for (int i = 0; schedule && schedule->t == J_OBJ && i < schedule->len; ++i) {
        const char *key = schedule->keys[i];
        if (!strcmp(key, "enabled")) wrote_enabled = 1;
        if (!strcmp(key, "last_status")) wrote_status = 1;
        if (!strcmp(key, "last_reason")) wrote_reason = 1;
        if (i && !text_add(&out, ",")) goto fail;
        if (!text_json_string(&out, key) || !text_add(&out, ":")) goto fail;
        if (!strcmp(key, "enabled")) {
            if (!text_add(&out, enabled ? "true" : "false")) goto fail;
        } else if (!strcmp(key, "last_status")) {
            if (!text_json_string(&out, status)) goto fail;
        } else if (!strcmp(key, "last_reason")) {
            if (!text_json_string(&out, reason ? reason : "")) goto fail;
        } else if (!text_json_value(&out, schedule->kids[i])) goto fail;
    }
    if (!wrote_enabled && (!text_add(&out, schedule && schedule->t == J_OBJ && schedule->len ? "," : "") ||
        !text_add(&out, "\"enabled\":") || !text_add(&out, enabled ? "true" : "false"))) goto fail;
    if (!wrote_status && (!text_add(&out, ",\"last_status\":") || !text_json_string(&out, status))) goto fail;
    if (!wrote_reason && reason && (!text_add(&out, ",\"last_reason\":") || !text_json_string(&out, reason))) goto fail;
    if (!text_add(&out, "}\n")) goto fail;
    int ok = write_small_file(path, out.data); free(out.data); return ok;
fail:
    free(out.data); return 0;
}

static int jobs_schedule_arm(Gateway *g, int fd, const SamosaHttpRequest *request) {
    char *arena = NULL; jval *body = json_parse(request->body, &arena);
    jval *job = body && body->t == J_OBJ ? json_get(body, "job") : NULL;
    jval *job_path_value = body && body->t == J_OBJ ? json_get(body, "job_path") : NULL;
    char *job_raw = NULL; char *job_arena = NULL; jval *loaded_job = NULL;
    if (job_path_value && job_path_value->t == J_STR) {
        job_raw = read_file_limit(job_path_value->str, 1 << 20);
        loaded_job = job_raw ? json_parse(job_raw, &job_arena) : NULL;
        job = loaded_job;
    }
    if (!job || job->t != J_OBJ) {
        json_free(loaded_job); free(job_arena); free(job_raw); json_free(body); free(arena);
        return samosa_http_json_error(fd, 400, "invalid_schedule", "A job object or job_path is required.");
    }
    jval *id = json_get(job, "job_id");
    char job_id[128];
    if (id && id->t == J_STR) path_copy(job_id, sizeof(job_id), id->str);
    else slugify_to(job_id, sizeof(job_id), job_path_value && job_path_value->t == J_STR ? path_basename_const(job_path_value->str) : "scheduled-job");
    if (!valid_job_id(job_id)) {
        json_free(loaded_job); free(job_arena); free(job_raw); json_free(body); free(arena);
        return samosa_http_json_error(fd, 400, "invalid_job_id", "job_id may contain only letters, numbers, dash, and underscore.");
    }
    TextBuffer frozen = {0};
    if (!text_json_value(&frozen, job)) { json_free(loaded_job); free(job_arena); free(job_raw); json_free(body); free(arena); return 0; }
    char hash[17]; snprintf(hash, sizeof(hash), "%016llx",
                            (unsigned long long)stable_hash_bytes((unsigned char *)frozen.data, frozen.len));
    char frozen_path[PATH_MAX], schedule_path[PATH_MAX];
    if (!job_state_path(g, job_id, "job.json", frozen_path, 1) ||
        !job_state_path(g, job_id, "schedule.json", schedule_path, 1)) {
        free(frozen.data); json_free(loaded_job); free(job_arena); free(job_raw); json_free(body); free(arena); return 0;
    }
    char *existing = read_file_limit(frozen_path, 1 << 20);
    if (existing) {
        char *existing_arena = NULL; jval *existing_job = json_parse(existing, &existing_arena);
        TextBuffer existing_json = {0}; text_json_value(&existing_json, existing_job);
        uint64_t old_hash = stable_hash_bytes((unsigned char *)existing_json.data, existing_json.len);
        uint64_t new_hash = stable_hash_bytes((unsigned char *)frozen.data, frozen.len);
        free(existing_json.data); json_free(existing_job); free(existing_arena); free(existing);
        if (old_hash != new_hash) {
            free(frozen.data); json_free(loaded_job); free(job_arena); free(job_raw); json_free(body); free(arena);
            return samosa_http_json_error(fd, 409, "schedule_definition_changed", "That job_id is already armed with a different definition.");
        }
    }
    TextBuffer pretty = {0};
    if (!text_json_value(&pretty, job) || !text_add(&pretty, "\n") ||
        !write_small_file(frozen_path, pretty.data)) {
        free(pretty.data); free(frozen.data); json_free(loaded_job); free(job_arena); free(job_raw); json_free(body); free(arena); return 0;
    }
    free(pretty.data);
    jval *ws = json_get(body, "window_start"), *we = json_get(body, "window_end");
    jval *missed = json_get(body, "missed_policy"), *keep = json_get(body, "keep_awake");
    const char *window_start = ws && ws->t == J_STR ? ws->str : "22:00";
    const char *window_end = we && we->t == J_STR ? we->str : "06:00";
    const char *missed_policy = missed && missed->t == J_STR ? missed->str : "skip";
    if (parse_hhmm(window_start) < 0 || parse_hhmm(window_end) < 0 ||
        (strcmp(missed_policy, "skip") && strcmp(missed_policy, "run_next_start"))) {
        free(frozen.data); json_free(loaded_job); free(job_arena); free(job_raw); json_free(body); free(arena);
        return samosa_http_json_error(fd, 400, "invalid_schedule", "window times must be HH:MM and missed_policy must be skip or run_next_start.");
    }
    jval *resources = json_get(job, "resources");
    jval *run_batt = resources && resources->t == J_OBJ ? json_get(resources, "run_on_battery") : NULL;
    char now[32]; rfc3339_now_to(now, sizeof(now));
    char deadline[32];
    snprintf(deadline, sizeof(deadline), "%ld", window_deadline_epoch(parse_hhmm(window_end), time(NULL)));
    TextBuffer schedule = {0};
    int ok = text_add(&schedule, "{\"schema_version\":1,\"job_id\":") && text_json_string(&schedule, job_id) &&
        text_add(&schedule, ",\"job_path\":") && text_json_string(&schedule, frozen_path) &&
        text_add(&schedule, ",\"job_sha256\":") && text_json_string(&schedule, hash) &&
        text_add(&schedule, ",\"enabled\":true,\"window_start\":") && text_json_string(&schedule, window_start) &&
        text_add(&schedule, ",\"window_end\":") && text_json_string(&schedule, window_end) &&
        text_add(&schedule, ",\"missed_policy\":") && text_json_string(&schedule, missed_policy) &&
        text_add(&schedule, ",\"deadline_epoch\":") && text_add(&schedule, deadline) &&
        text_add(&schedule, ",\"keep_awake\":") && text_add(&schedule, (keep && keep->t == J_BOOL && !keep->boolean) ? "false" : "true") &&
        text_add(&schedule, ",\"run_on_battery\":") && text_add(&schedule, (run_batt && run_batt->t == J_BOOL && run_batt->boolean) ? "true" : "false") &&
        text_add(&schedule, ",\"review_required_policy\":\"queue\",\"armed_at\":") && text_json_string(&schedule, now) &&
        text_add(&schedule, "}\n") && write_small_file(schedule_path, schedule.data);
    TextBuffer response = {0};
    if (ok) ok = text_add(&response, "{\"ok\":true,\"job_id\":") && text_json_string(&response, job_id) &&
        text_add(&response, ",\"schedule_path\":") && text_json_string(&response, schedule_path) &&
        text_add(&response, ",\"schedule\":") && text_add_n(&response, schedule.data, schedule.len - 1) &&
        text_add(&response, "}");
    int sent = ok ? samosa_http_response(fd, 200, "application/json", response.data, NULL) : 0;
    free(response.data); free(schedule.data); free(frozen.data);
    json_free(loaded_job); free(job_arena); free(job_raw); json_free(body); free(arena); return sent;
}

static int append_job_event_file(const char *path, int *seq, const char *type,
                                 const char *json_fields) {
    char *old = read_file_limit(path, 16 << 20);
    TextBuffer out = {0};
    if (old) text_add(&out, old);
    char now[32], number[32]; rfc3339_now_to(now, sizeof(now));
    snprintf(number, sizeof(number), "%d", (*seq)++);
    int ok = text_add(&out, "{\"seq\":") && text_add(&out, number) &&
        text_add(&out, ",\"ts\":") && text_json_string(&out, now) &&
        text_add(&out, ",\"type\":") && text_json_string(&out, type);
    if (ok && json_fields && *json_fields) ok = text_add(&out, ",") && text_add(&out, json_fields);
    ok = ok && text_add(&out, "}\n") && write_small_file(path, out.data);
    free(old); free(out.data); return ok;
}

static const char *type_folder_for(const char *name, const char *media) {
    const char *dot = strrchr(name ? name : "", '.');
    if (dot && dot[1]) {
        if (!strcasecmp(dot + 1, "txt")) return "TXT";
        if (!strcasecmp(dot + 1, "pdf")) return "PDF";
        if (!strcasecmp(dot + 1, "jpg") || !strcasecmp(dot + 1, "jpeg")) return "JPG";
        if (!strcasecmp(dot + 1, "png")) return "PNG";
        if (!strcasecmp(dot + 1, "json")) return "JSON";
    }
    if (media && strstr(media, "pdf")) return "PDF";
    if (media && strstr(media, "image/png")) return "PNG";
    if (media && strstr(media, "image/jpeg")) return "JPG";
    if (media && strstr(media, "text")) return "TXT";
    return "OTHER";
}

/* ============================================================================
   Native public-URL fetch pipeline. Every fetch resolves the host itself,
   rejects any resolved address in a private/loopback/link-local/transition
   range, pins curl to that validated IP with --resolve, disables curl's own
   redirects, and re-runs the whole check on each hop. This mirrors the Python
   prototype's contract: curl is a pinned transport, never trusted to resolve
   or follow redirects. HTTP(S) only; standard ports only; no credentials.
   ============================================================================ */

#define PUBLIC_FETCH_USER_AGENT "SamosaChat/1.0 (+local user-initiated fetch)"
#define PUBLIC_FETCH_MAX_HOPS 6

/* a is IPv4 in host byte order. Blocks 0/8, 10/8, 100.64/10, 127/8, 169.254/16,
   172.16/12, 192.0.0/24, 192.168/16, 198.18/15, 224/4, 240/4. */
static int ipv4_blocked(uint32_t a) {
    uint8_t o1 = (uint8_t)(a >> 24), o2 = (uint8_t)(a >> 16), o3 = (uint8_t)(a >> 8);
    if (o1 == 0 || o1 == 10 || o1 == 127) return 1;
    if (o1 == 100 && (o2 & 0xc0) == 64) return 1;
    if (o1 == 169 && o2 == 254) return 1;
    if (o1 == 172 && (o2 & 0xf0) == 16) return 1;
    if (o1 == 192 && o2 == 0 && o3 == 0) return 1;
    if (o1 == 192 && o2 == 168) return 1;
    if (o1 == 198 && (o2 & 0xfe) == 18) return 1;
    if (o1 >= 224) return 1;
    return 0;
}

/* Blocks ::/128, ::1/128, fc00::/7, fe80::/10, ::ffff:0:0/96 (IPv4-mapped),
   64:ff9b::/96 (NAT64), and 2002::/16 (6to4) — the ranges that can smuggle a
   fetch to an internal target across the IPv6 boundary. */
static int ipv6_blocked(const uint8_t b[16]) {
    int zero_prefix = 1;
    for (int i = 0; i < 15; ++i) if (b[i]) { zero_prefix = 0; break; }
    if (zero_prefix && (b[15] == 0 || b[15] == 1)) return 1;   /* :: and ::1 */
    if ((b[0] & 0xfe) == 0xfc) return 1;                        /* fc00::/7 */
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return 1;        /* fe80::/10 */
    if (b[0] == 0x20 && b[1] == 0x02) return 1;                 /* 2002::/16 */
    int zero10 = 1;
    for (int i = 0; i < 10; ++i) if (b[i]) { zero10 = 0; break; }
    if (zero10 && b[10] == 0xff && b[11] == 0xff) return 1;     /* ::ffff:0:0/96 */
    if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b) {
        int mid = 1;
        for (int i = 4; i < 12; ++i) if (b[i]) { mid = 0; break; }
        if (mid) return 1;                                     /* 64:ff9b::/96 */
    }
    return 0;
}

static int ip_blocked(const char *ip) {
    struct in_addr v4; struct in6_addr v6;
    if (inet_pton(AF_INET, ip, &v4) == 1) return ipv4_blocked(ntohl(v4.s_addr));
    if (inet_pton(AF_INET6, ip, &v6) == 1) return ipv6_blocked(v6.s6_addr);
    return 1;
}

/* Resolve host; reject if ANY resolved address is non-public (strict against
   DNS rebinding). Returns the first usable address string. */
static int resolve_public_host(const char *host, char out_ip[INET6_ADDRSTRLEN],
                               char *err, size_t errcap) {
    struct addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        snprintf(err, errcap, "could not resolve host"); return 0;
    }
    char first[INET6_ADDRSTRLEN] = {0};
    int ok = 1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        char ip[INET6_ADDRSTRLEN] = {0};
        void *addr = ai->ai_family == AF_INET ?
            (void *)&((struct sockaddr_in *)ai->ai_addr)->sin_addr :
            ai->ai_family == AF_INET6 ?
            (void *)&((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr : NULL;
        if (!addr || !inet_ntop(ai->ai_family, addr, ip, sizeof(ip))) continue;
        if (ip_blocked(ip)) { ok = 0; snprintf(err, errcap, "blocked non-public address"); break; }
        if (!first[0]) path_copy(first, sizeof(first), ip);
    }
    freeaddrinfo(res);
    if (!ok) return 0;
    if (!first[0]) { snprintf(err, errcap, "host has no usable address"); return 0; }
    path_copy(out_ip, INET6_ADDRSTRLEN, first); return 1;
}

typedef struct { char scheme[8]; char host[256]; int port; char path[2048]; } ParsedUrl;

static int url_parse(const char *url, ParsedUrl *p, char *err, size_t errcap) {
    memset(p, 0, sizeof(*p));
    const char *s = url; while (*s == ' ' || *s == '\t') ++s;
    const char *sep = strstr(s, "://");
    int https;
    if (sep && (size_t)(sep - s) == 4 && !strncasecmp(s, "http", 4)) { strcpy(p->scheme, "http"); https = 0; }
    else if (sep && (size_t)(sep - s) == 5 && !strncasecmp(s, "https", 5)) { strcpy(p->scheme, "https"); https = 1; }
    else { snprintf(err, errcap, "only public http:// and https:// URLs are allowed"); return 0; }
    const char *authority = sep + 3;
    size_t authlen = strcspn(authority, "/?#");
    const char *path_start = authority + authlen;
    char auth[512];
    if (authlen >= sizeof(auth)) { snprintf(err, errcap, "host is too long"); return 0; }
    memcpy(auth, authority, authlen); auth[authlen] = 0;
    if (strchr(auth, '@')) { snprintf(err, errcap, "credentials in URLs are not allowed"); return 0; }
    p->port = https ? 443 : 80;
    if (auth[0] == '[') {
        char *close = strchr(auth, ']');
        if (!close) { snprintf(err, errcap, "malformed IPv6 host"); return 0; }
        *close = 0;
        if (!path_copy(p->host, sizeof(p->host), auth + 1)) { snprintf(err, errcap, "host is too long"); return 0; }
        if (close[1] == ':') p->port = atoi(close + 2);
    } else {
        char *colon = strrchr(auth, ':');
        if (colon) { *colon = 0; p->port = atoi(colon + 1); }
        if (!path_copy(p->host, sizeof(p->host), auth)) { snprintf(err, errcap, "host is too long"); return 0; }
    }
    if (!p->host[0]) { snprintf(err, errcap, "missing host"); return 0; }
    if (p->port != 80 && p->port != 443) { snprintf(err, errcap, "non-standard URL ports are blocked"); return 0; }
    if (*path_start) path_copy(p->path, sizeof(p->path), path_start);
    else strcpy(p->path, "/");
    return 1;
}

/* Per-host politeness: at least SAMOSA_WEB_MIN_INTERVAL seconds between fetches
   to the same host:port. The slot is reserved under lock, then we sleep
   unlocked so unrelated hosts are not stalled. */
static pthread_mutex_t public_rate_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { char key[300]; double last; } public_rate[64];
static void public_rate_wait(const char *key) {
    const char *env = getenv("SAMOSA_WEB_MIN_INTERVAL");
    double interval = env ? atof(env) : 1.0;
    if (interval <= 0) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9, wait = 0;
    pthread_mutex_lock(&public_rate_lock);
    int slot = -1, empty = -1;
    for (int i = 0; i < 64; ++i) {
        if (public_rate[i].key[0] == 0) { if (empty < 0) empty = i; }
        else if (!strcmp(public_rate[i].key, key)) { slot = i; break; }
    }
    if (slot < 0) slot = empty < 0 ? 0 : empty;
    double ready = public_rate[slot].last + interval;
    if (public_rate[slot].last > 0 && ready > now) wait = ready - now;
    path_copy(public_rate[slot].key, sizeof(public_rate[slot].key), key);
    public_rate[slot].last = now + wait;
    pthread_mutex_unlock(&public_rate_lock);
    if (wait > 0) { struct timespec s = {.tv_sec = (time_t)wait, .tv_nsec = (long)((wait - (time_t)wait) * 1e9)}; nanosleep(&s, NULL); }
}

static int make_temp_path(char out[PATH_MAX]) {
    const char *tmp = getenv("TMPDIR"); if (!tmp || !*tmp) tmp = "/tmp";
    if (snprintf(out, PATH_MAX, "%s/samosa-web-XXXXXX", tmp) >= PATH_MAX) return -1;
    return mkstemp(out);
}

/* One HTTP transaction, no redirects, pinned to `ip`. Returns the status code
   (0 on transport failure); fills headers/body (caller frees). */
static int curl_fetch(Gateway *g, const char *url, const char *host, int port,
                      const char *ip, char **out_headers, char **out_body) {
    *out_headers = NULL; *out_body = NULL;
    char bodyp[PATH_MAX], headp[PATH_MAX];
    int bfd = make_temp_path(bodyp); if (bfd < 0) return 0; close(bfd);
    int hfd = make_temp_path(headp); if (hfd < 0) { unlink(bodyp); return 0; } close(hfd);
    const char *curl = getenv("SAMOSA_CURL"); if (!curl || !*curl) curl = "/usr/bin/curl";
    char maxbytes[24]; snprintf(maxbytes, sizeof(maxbytes), "%u", (unsigned)MAX_PUBLIC_FETCH_BYTES);
    char resolve[600]; snprintf(resolve, sizeof(resolve), "%s:%d:%s", host, port, ip);
    char *argv[] = { (char *)curl, (char *)"--silent", (char *)"--show-error",
        (char *)"--fail-with-body", (char *)"--proto", (char *)"=http,https",
        (char *)"--max-redirs", (char *)"0", (char *)"--max-time", (char *)"20",
        (char *)"--connect-timeout", (char *)"5", (char *)"--max-filesize", maxbytes,
        (char *)"--resolve", resolve, (char *)"-A", (char *)PUBLIC_FETCH_USER_AGENT,
        (char *)"-D", headp, (char *)"-o", bodyp, (char *)"-w", (char *)"%{http_code}",
        (char *)url, NULL };
    int status = 0; char *code = run_capture(g, curl, argv, 64, &status);
    int http = 0;
    if (code) { const char *d = code; while (*d && (*d < '0' || *d > '9')) ++d; http = atoi(d); free(code); }
    *out_headers = read_file_limit(headp, 256 * 1024);
    *out_body = read_file_limit(bodyp, MAX_PUBLIC_FETCH_BYTES);
    unlink(bodyp); unlink(headp);
    return http;
}

static void header_value(const char *headers, const char *name, char *out, size_t cap) {
    out[0] = 0;
    size_t namelen = strlen(name);
    for (const char *line = headers; line && *line; ) {
        const char *eol = strchr(line, '\n');
        if (!strncasecmp(line, name, namelen) && line[namelen] == ':') {
            const char *v = line + namelen + 1;
            while (*v == ' ' || *v == '\t') ++v;
            size_t n = eol ? (size_t)(eol - v) : strlen(v);
            while (n && (v[n - 1] == '\r' || v[n - 1] == ' ' || v[n - 1] == '\t')) --n;
            if (n >= cap) n = cap - 1;
            memcpy(out, v, n); out[n] = 0;
        }
        line = eol ? eol + 1 : NULL;
    }
}

static char *url_join(const char *base, const char *loc) {
    if (!strncasecmp(loc, "http://", 7) || !strncasecmp(loc, "https://", 8)) return strdup(loc);
    ParsedUrl b; char err[64];
    if (!url_parse(base, &b, err, sizeof(err))) return strdup(loc);
    char origin[600];
    if ((!strcmp(b.scheme, "http") && b.port == 80) || (!strcmp(b.scheme, "https") && b.port == 443))
        snprintf(origin, sizeof(origin), "%s://%s", b.scheme, b.host);
    else
        snprintf(origin, sizeof(origin), "%s://%s:%d", b.scheme, b.host, b.port);
    char joined[4096];
    if (loc[0] == '/') snprintf(joined, sizeof(joined), "%s%s", origin, loc);
    else {
        char dir[2048]; path_copy(dir, sizeof(dir), b.path);
        char *slash = strrchr(dir, '/'); if (slash) slash[1] = 0; else strcpy(dir, "/");
        snprintf(joined, sizeof(joined), "%s%s%s", origin, dir, loc);
    }
    return strdup(joined);
}

static int content_type_allowed(const char *ctype) {
    static const char *ok[] = { "text/html", "text/plain", "application/json",
        "text/xml", "application/xml", "application/rss+xml", NULL };
    for (int i = 0; ok[i]; ++i) if (!strcmp(ctype, ok[i])) return 1;
    return 0;
}

/* Test seam: when SAMOSA_WEB_STUB_DIR is set, the network transport is replaced
   by local files keyed by a slug of the URL (<slug>.html or <slug>.txt). URL
   validation still runs, so scheme/port/credential rejections are unaffected.
   Never consulted unless the env var is set. */
static int stub_page_path(const char *url, char out[PATH_MAX], int *is_html) {
    const char *dir = getenv("SAMOSA_WEB_STUB_DIR");
    if (!dir) return 0;
    char slug[160]; slugify_to(slug, sizeof(slug), url);
    if (snprintf(out, PATH_MAX, "%s/%s.html", dir, slug) < PATH_MAX && access(out, F_OK) == 0) { *is_html = 1; return 1; }
    if (snprintf(out, PATH_MAX, "%s/%s.txt", dir, slug) < PATH_MAX && access(out, F_OK) == 0) { *is_html = 0; return 1; }
    return 0;
}

/* Robots gate. On any fetch error, or a non-text robots file, fetch is allowed
   (matching the reference). A conservative subset of the robots spec: the group
   for our agent token, else '*', longest-match Allow beats longest-match
   Disallow. */
static int robots_path_allowed(const char *robots, const char *ua_token, const char *path) {
    int have_specific = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const char *want = pass == 0 ? ua_token : "*";
        int in_group = 0, group_started = 0, last_was_agent = 0;
        long best_allow = -1, best_disallow = -1;
        for (const char *line = robots; line && *line; ) {
            const char *eol = strchr(line, '\n');
            size_t len = eol ? (size_t)(eol - line) : strlen(line);
            while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) --len;
            const char *p = line; size_t rest = len;
            while (rest && (*p == ' ' || *p == '\t')) { ++p; --rest; }
            if (rest && *p != '#') {
                if (rest >= 11 && !strncasecmp(p, "user-agent:", 11)) {
                    const char *v = p + 11; size_t vn = rest - 11;
                    while (vn && (*v == ' ' || *v == '\t')) { ++v; --vn; }
                    int matches = (pass == 0)
                        ? (vn && strlen(want) <= vn && !strncasecmp(v, want, strlen(want)))
                        : (vn == 1 && v[0] == '*');
                    if (!last_was_agent && group_started) in_group = 0;   /* new group */
                    if (matches) { in_group = 1; if (pass == 0) have_specific = 1; }
                    group_started = 1; last_was_agent = 1;
                } else {
                    last_was_agent = 0;
                    if (in_group && (rest >= 9 && !strncasecmp(p, "disallow:", 9))) {
                        const char *v = p + 9; size_t vn = rest - 9;
                        while (vn && (*v == ' ' || *v == '\t')) { ++v; --vn; }
                        if (vn && !strncmp(path, v, vn) && (long)vn > best_disallow) best_disallow = (long)vn;
                        if (!vn) { /* empty Disallow = allow all: no constraint */ }
                    } else if (in_group && (rest >= 6 && !strncasecmp(p, "allow:", 6))) {
                        const char *v = p + 6; size_t vn = rest - 6;
                        while (vn && (*v == ' ' || *v == '\t')) { ++v; --vn; }
                        if (vn && !strncmp(path, v, vn) && (long)vn > best_allow) best_allow = (long)vn;
                    }
                }
            }
            line = eol ? eol + 1 : NULL;
        }
        if (pass == 0 && !have_specific) continue;   /* no specific group; try '*' */
        return best_disallow < 0 || best_allow >= best_disallow;
    }
    return 1;
}

static int robots_allowed(Gateway *g, const char *url); /* fwd */

static int fetch_public(Gateway *g, const char *url, int enforce_robots,
                        char **final_url, char **content_type, char **body,
                        char *err, size_t errcap) {
    *final_url = *content_type = *body = NULL;
    char *current = strdup(url);
    if (!current) { snprintf(err, errcap, "out of memory"); return 0; }
    for (int hop = 0; hop < PUBLIC_FETCH_MAX_HOPS; ++hop) {
        ParsedUrl parsed;
        if (!url_parse(current, &parsed, err, errcap)) { free(current); return 0; }
        if (enforce_robots && !robots_allowed(g, current)) {
            snprintf(err, errcap, "robots.txt disallows this URL"); free(current); return 0;
        }
        char key[300]; snprintf(key, sizeof(key), "%s:%d", parsed.host, parsed.port);
        public_rate_wait(key);
        int is_html = 0; char stub[PATH_MAX];
        if (stub_page_path(current, stub, &is_html)) {
            char *data = read_file_limit(stub, MAX_PUBLIC_FETCH_BYTES);
            if (!data) { snprintf(err, errcap, "stub page unreadable"); free(current); return 0; }
            *final_url = current; *content_type = strdup(is_html ? "text/html" : "text/plain"); *body = data;
            return 1;
        }
        if (getenv("SAMOSA_WEB_STUB_DIR")) { snprintf(err, errcap, "fetch failed (no stub)"); free(current); return 0; }
        char ip[INET6_ADDRSTRLEN];
        if (!resolve_public_host(parsed.host, ip, err, errcap)) { free(current); return 0; }
        char *headers = NULL, *data = NULL;
        int http = curl_fetch(g, current, parsed.host, parsed.port, ip, &headers, &data);
        char location[2048], ctype[128];
        header_value(headers ? headers : "", "location", location, sizeof(location));
        header_value(headers ? headers : "", "content-type", ctype, sizeof(ctype));
        char *semi = strchr(ctype, ';'); if (semi) *semi = 0;
        for (char *c = ctype; *c; ++c) { if (*c >= 'A' && *c <= 'Z') *c += 32; if (*c == ' ') *c = 0; }
        free(headers);
        if ((http == 301 || http == 302 || http == 303 || http == 307 || http == 308) && location[0]) {
            char *next = url_join(current, location);
            free(current); free(data); current = next;
            if (!current) { snprintf(err, errcap, "out of memory"); return 0; }
            continue;
        }
        if (http < 200 || http >= 300) {
            snprintf(err, errcap, "fetch failed with HTTP %d", http); free(current); free(data); return 0;
        }
        if (!content_type_allowed(ctype)) {
            snprintf(err, errcap, "unsupported content type: %s", ctype[0] ? ctype : "unknown");
            free(current); free(data); return 0;
        }
        *final_url = current; *content_type = strdup(ctype[0] ? ctype : "text/plain"); *body = data ? data : strdup("");
        return 1;
    }
    snprintf(err, errcap, "too many redirects"); free(current); return 0;
}

static int robots_allowed(Gateway *g, const char *url) {
    ParsedUrl p; char err[64];
    if (!url_parse(url, &p, err, sizeof(err))) return 0;
    char robots_url[512];
    if ((!strcmp(p.scheme, "http") && p.port == 80) || (!strcmp(p.scheme, "https") && p.port == 443))
        snprintf(robots_url, sizeof(robots_url), "%s://%s/robots.txt", p.scheme, p.host);
    else
        snprintf(robots_url, sizeof(robots_url), "%s://%s:%d/robots.txt", p.scheme, p.host, p.port);
    char *robots_text = NULL;
    if (getenv("SAMOSA_WEB_STUB_DIR")) {
        const char *dir = getenv("SAMOSA_WEB_STUB_DIR");
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/robots.txt", dir) < (int)sizeof(path))
            robots_text = read_file_limit(path, 256 * 1024);
        if (!robots_text) return 1;
    } else {
        char *final_url = NULL, *ctype = NULL, *body = NULL, ferr[128];
        if (!fetch_public(g, robots_url, 0, &final_url, &ctype, &body, ferr, sizeof(ferr))) {
            free(final_url); free(ctype); free(body); return 1;
        }
        int text = ctype && (!strcmp(ctype, "text/plain") || !strcmp(ctype, "text/html"));
        if (text) robots_text = body ? strdup(body) : NULL;
        free(final_url); free(ctype); free(body);
        if (!robots_text) return 1;
    }
    int allowed = robots_path_allowed(robots_text, "samosachat", p.path);
    free(robots_text); return allowed;
}

static void append_entity(TextBuffer *out, const char *name, size_t len) {
    struct { const char *n; const char *v; } named[] = {
        {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
        {"nbsp", " "}, {"#39", "'"}, {"#34", "\""}, {NULL, NULL} };
    char buf[16];
    if (len < sizeof(buf)) { memcpy(buf, name, len); buf[len] = 0;
        for (int i = 0; named[i].n; ++i) if (!strcmp(buf, named[i].n)) { text_add(out, named[i].v); return; }
        if (buf[0] == '#') { int code = atoi(buf + 1); if (code >= 32 && code < 127) { char c[2] = {(char)code, 0}; text_add(out, c); return; } }
    }
    text_add(out, " ");
}

/* HTML → readable text: drops script/style/svg/noscript/template, inserts a
   newline at block boundaries, decodes common entities, extracts <title>. */
static void html_to_text(const char *html, char **out_text, char **out_title) {
    TextBuffer text = {0}, title = {0};
    int skip = 0, in_title = 0, last_space = 1;
    const char *p = html;
    while (*p) {
        if (*p == '<') {
            const char *q = p + 1; int closing = 0;
            if (*q == '/') { closing = 1; ++q; }
            if (*q == '!') { const char *gt = strchr(q, '>'); p = gt ? gt + 1 : q; continue; }
            char tag[16]; size_t tl = 0;
            while (*q && (isalnum((unsigned char)*q)) && tl + 1 < sizeof(tag)) tag[tl++] = (char)tolower((unsigned char)*q++);
            tag[tl] = 0;
            const char *gt = strchr(q, '>'); const char *nextp = gt ? gt + 1 : (q + strlen(q));
            int is_skip = !strcmp(tag, "script") || !strcmp(tag, "style") || !strcmp(tag, "svg") ||
                          !strcmp(tag, "noscript") || !strcmp(tag, "template");
            if (is_skip) { if (closing) { if (skip) --skip; } else ++skip; }
            else if (!strcmp(tag, "title")) in_title = !closing;
            else if (!closing && (!strcmp(tag, "p") || !strcmp(tag, "br") || !strcmp(tag, "li") ||
                     !strcmp(tag, "article") || !strcmp(tag, "section") || !strcmp(tag, "div") ||
                     !strcmp(tag, "h1") || !strcmp(tag, "h2") || !strcmp(tag, "h3") || !strcmp(tag, "tr"))) {
                if (!skip) { text_add(&text, "\n"); last_space = 1; }
            }
            p = nextp; continue;
        }
        if (skip) { ++p; continue; }
        if (*p == '&') {
            const char *semi = strchr(p, ';');
            if (semi && semi - p <= 10) {
                TextBuffer *dst = in_title ? &title : &text;
                append_entity(dst, p + 1, (size_t)(semi - p - 1));
                last_space = 0; p = semi + 1; continue;
            }
        }
        unsigned char c = (unsigned char)*p;
        if (isspace(c)) {
            if (!last_space) { if (!in_title) text_add(&text, " "); else text_add(&title, " "); last_space = 1; }
        } else {
            char s[2] = {(char)c, 0};
            if (in_title) text_add(&title, s); else text_add(&text, s);
            last_space = 0;
        }
        ++p;
    }
    /* Collapse each whitespace run to one char (newline if the run held any),
       and drop leading/trailing whitespace. */
    if (text.data) {
        char *r = text.data, *w = text.data;
        while (*r) {
            if (*r == '\n' || *r == ' ' || *r == '\t' || *r == '\r') {
                int newline = 0;
                while (*r == '\n' || *r == ' ' || *r == '\t' || *r == '\r') { if (*r == '\n') newline = 1; ++r; }
                if (w > text.data && *r) *w++ = newline ? '\n' : ' ';
            } else {
                *w++ = *r++;
            }
        }
        *w = 0;
    }
    if (title.data) {   /* trim a trailing space left by the whitespace collapse */
        size_t tl = strlen(title.data);
        while (tl && (title.data[tl - 1] == ' ' || title.data[tl - 1] == '\n')) title.data[--tl] = 0;
    }
    *out_text = text.data ? text.data : strdup("");
    *out_title = title.data ? title.data : strdup("");
}

typedef struct { char *url; char *title; char *text; int truncated; } PublicPage;
static void public_page_free(PublicPage *pg) { free(pg->url); free(pg->title); free(pg->text); memset(pg, 0, sizeof(*pg)); }

static int readable_page(Gateway *g, const char *url, PublicPage *out, char *err, size_t errcap) {
    memset(out, 0, sizeof(*out));
    char *final_url = NULL, *ctype = NULL, *body = NULL;
    if (!fetch_public(g, url, 1, &final_url, &ctype, &body, err, errcap)) return 0;
    char *title = NULL, *text = NULL;
    if (!strcmp(ctype, "text/html")) {
        html_to_text(body, &text, &title);
        if (!title[0]) { free(title); ParsedUrl p; char e[64]; title = strdup(url_parse(final_url, &p, e, sizeof(e)) ? p.host : final_url); }
        int scripts = 0; for (const char *s = body; (s = strcasestr(s, "<script")); s += 7) ++scripts;
        if (strlen(text) < 300 && scripts >= 3) {
            snprintf(err, errcap, "this page appears to require JavaScript and could not be read");
            free(final_url); free(ctype); free(body); free(title); free(text); return 0;
        }
    } else {
        text = body ? strdup(body) : strdup("");
        ParsedUrl p; char e[64];
        const char *base = url_parse(final_url, &p, e, sizeof(e)) ? p.path : final_url;
        const char *slash = strrchr(base, '/');
        title = strdup(slash && slash[1] ? slash + 1 : final_url);
    }
    /* trim leading/trailing whitespace to judge emptiness */
    const char *t = text; while (*t == ' ' || *t == '\n' || *t == '\r' || *t == '\t') ++t;
    if (!*t) {
        snprintf(err, errcap, "the page did not contain readable text");
        free(final_url); free(ctype); free(body); free(title); free(text); return 0;
    }
    if (strlen(title) > 300) title[300] = 0;
    out->truncated = strlen(text) > MAX_PUBLIC_TEXT_BYTES;
    if (out->truncated) text[MAX_PUBLIC_TEXT_BYTES] = 0;
    out->url = final_url; out->title = title; out->text = text;
    free(ctype); free(body);
    return 1;
}

/* Fetch each user-supplied public URL and persist only new or changed pages.
   Change detection compares an FNV-1a digest of title+text against the prior
   run's; unchanged pages produce no item, a changed/new page produces exactly
   one item text file. Returns a malloc'd JSON summary (caller frees) or NULL on
   a structural failure (bad job_id / no URLs), with `err` set. */
static char *update_job_public_inputs(Gateway *g, const char *job_id, jval *urls,
                                      char *err, size_t errcap) {
    if (!valid_job_id(job_id)) { snprintf(err, errcap, "a valid job_id is required"); return NULL; }
    const char *clean[MAX_PUBLIC_JOB_URLS]; int nclean = 0;
    for (int i = 0; urls && urls->t == J_ARR && i < urls->len; ++i) {
        jval *u = urls->kids[i];
        if (!u || u->t != J_STR || !u->str[0]) continue;
        int dup = 0; for (int j = 0; j < nclean; ++j) if (!strcmp(clean[j], u->str)) { dup = 1; break; }
        if (dup) continue;
        if (nclean >= MAX_PUBLIC_JOB_URLS) { snprintf(err, errcap, "at most %d URLs are allowed", MAX_PUBLIC_JOB_URLS); return NULL; }
        clean[nclean++] = u->str;
    }
    if (!nclean) { snprintf(err, errcap, "at least one public URL is required"); return NULL; }

    char public_dir[PATH_MAX], items_dir[PATH_MAX], state_path[PATH_MAX], last_path[PATH_MAX];
    if (!job_state_path(g, job_id, "public", public_dir, 1) ||
        !path_join(items_dir, sizeof(items_dir), public_dir, "items") || !mkdirs(items_dir) ||
        !path_join(state_path, sizeof(state_path), public_dir, "state.json") ||
        !path_join(last_path, sizeof(last_path), public_dir, "last_fetch.json")) {
        snprintf(err, errcap, "could not prepare the public input directory"); return NULL;
    }
    char *state_raw = read_file_limit(state_path, 8 << 20), *state_arena = NULL;
    jval *prev = state_raw ? json_parse(state_raw, &state_arena) : NULL;
    jval *prev_pages = prev && prev->t == J_OBJ ? json_get(prev, "pages") : NULL;

    TextBuffer records = {0}, changed_items = {0}, new_pages = {0};
    char now[32]; rfc3339_now_to(now, sizeof(now));
    char *emitted[MAX_PUBLIC_JOB_URLS]; int nemitted = 0;   /* owned copies of final URLs */
    int changed = 0, checked = 0;

    for (int i = 0; i < nclean; ++i) {
        char ferr[160]; PublicPage page;
        checked++;
        if (i && !text_add(&records, ",")) {}
        if (!readable_page(g, clean[i], &page, ferr, sizeof(ferr))) {
            text_add(&records, "{\"requested_url\":"); text_json_string(&records, clean[i]);
            text_add(&records, ",\"status\":\"error\",\"error\":"); text_json_string(&records, ferr);
            text_add(&records, "}");
            continue;
        }
        char digest[17]; TextBuffer keyed = {0};
        text_add(&keyed, page.title); text_add_n(&keyed, "\0", 1); text_add(&keyed, page.text);
        snprintf(digest, sizeof(digest), "%016llx",
                 (unsigned long long)stable_hash_bytes((unsigned char *)keyed.data, keyed.len));
        free(keyed.data);
        const char *prev_hash = NULL;
        if (prev_pages && prev_pages->t == J_OBJ) {
            jval *pe = json_get(prev_pages, page.url);
            jval *ph = pe && pe->t == J_OBJ ? json_get(pe, "hash") : NULL;
            if (ph && ph->t == J_STR) prev_hash = ph->str;
        }
        const char *status = !prev_hash ? "new" : (strcmp(prev_hash, digest) ? "changed" : "unchanged");

        char text_path[PATH_MAX] = {0}, meta_path[PATH_MAX] = {0};
        if (strcmp(status, "unchanged")) {
            char slug[64]; slugify_to(slug, sizeof(slug), page.title[0] ? page.title : page.url);
            char stem[96]; snprintf(stem, sizeof(stem), "%s-%.12s", slug, digest);
            snprintf(text_path, sizeof(text_path), "%s/%s.txt", items_dir, stem);
            snprintf(meta_path, sizeof(meta_path), "%s/%s.json", items_dir, stem);
            write_small_file(text_path, page.text);
            changed++;
        }
        TextBuffer rec = {0};
        text_add(&rec, "{\"url\":"); text_json_string(&rec, page.url);
        text_add(&rec, ",\"requested_url\":"); text_json_string(&rec, clean[i]);
        text_add(&rec, ",\"title\":"); text_json_string(&rec, page.title);
        text_add(&rec, ",\"hash\":"); text_json_string(&rec, digest);
        text_add(&rec, ",\"status\":"); text_json_string(&rec, status);
        text_add(&rec, ",\"truncated\":"); text_add(&rec, page.truncated ? "true" : "false");
        char chars[32]; snprintf(chars, sizeof(chars), ",\"text_chars\":%zu", strlen(page.text));
        text_add(&rec, chars);
        if (text_path[0]) { text_add(&rec, ",\"text_path\":"); text_json_string(&rec, text_path);
            text_add(&rec, ",\"meta_path\":"); text_json_string(&rec, meta_path); }
        text_add(&rec, "}");
        if (meta_path[0]) { TextBuffer m = {0}; text_add_n(&m, rec.data, rec.len); text_add(&m, "\n"); write_small_file(meta_path, m.data); free(m.data); }
        text_add_n(&records, rec.data, rec.len);
        if (strcmp(status, "unchanged")) { if (changed > 1 && !text_add(&changed_items, ",")) {} text_add_n(&changed_items, rec.data, rec.len); }
        free(rec.data);

        int seen = 0; for (int j = 0; j < nemitted; ++j) if (!strcmp(emitted[j], page.url)) { seen = 1; break; }
        if (!seen && nemitted < MAX_PUBLIC_JOB_URLS) {
            if (nemitted && !text_add(&new_pages, ",")) {}
            text_json_string(&new_pages, page.url); text_add(&new_pages, ":{\"hash\":");
            text_json_string(&new_pages, digest); text_add(&new_pages, ",\"title\":");
            text_json_string(&new_pages, page.title); text_add(&new_pages, ",\"last_seen_at\":");
            text_json_string(&new_pages, now); text_add(&new_pages, "}");
            emitted[nemitted++] = strdup(page.url);   /* stable: page.url is freed below */
        }
        public_page_free(&page);
    }

    /* Preserve pages from earlier runs that were not part of this URL set. */
    for (int i = 0; prev_pages && prev_pages->t == J_OBJ && i < prev_pages->len; ++i) {
        const char *key = prev_pages->keys[i];
        int seen = 0; for (int j = 0; j < nemitted; ++j) if (emitted[j] && !strcmp(emitted[j], key)) { seen = 1; break; }
        if (seen) continue;
        if (new_pages.len && !text_add(&new_pages, ",")) {}
        text_json_string(&new_pages, key); text_add(&new_pages, ":");
        text_json_value(&new_pages, prev_pages->kids[i]);
    }

    TextBuffer state_out = {0};
    text_add(&state_out, "{\"pages\":{"); text_add_n(&state_out, new_pages.data ? new_pages.data : "", new_pages.len);
    text_add(&state_out, "}}\n");
    write_small_file(state_path, state_out.data);

    TextBuffer summary = {0};
    char head[128]; snprintf(head, sizeof(head), "{\"ok\":true,\"checked\":%d,\"changed\":%d,", checked, changed);
    text_add(&summary, head);
    text_add(&summary, "\"job_id\":"); text_json_string(&summary, job_id);
    text_add(&summary, ",\"changed_items\":["); text_add_n(&summary, changed_items.data ? changed_items.data : "", changed_items.len);
    text_add(&summary, "],\"records\":["); text_add_n(&summary, records.data ? records.data : "", records.len);
    text_add(&summary, "]}");
    write_small_file(last_path, summary.data);

    for (int i = 0; i < nemitted; ++i) free(emitted[i]);
    free(state_out.data); free(new_pages.data); free(records.data); free(changed_items.data);
    json_free(prev); free(state_arena); free(state_raw);
    return summary.data;
}

static int jobs_public_inputs_update(Gateway *g, int fd, const SamosaHttpRequest *request) {
    char *arena = NULL; jval *body = json_parse(request->body, &arena);
    jval *id = body && body->t == J_OBJ ? json_get(body, "job_id") : NULL;
    jval *urls = body && body->t == J_OBJ ? json_get(body, "urls") : NULL;
    if (!id || id->t != J_STR || !urls || urls->t != J_ARR) {
        json_free(body); free(arena);
        return samosa_http_json_error(fd, 400, "invalid_public_inputs", "job_id and a urls array are required.");
    }
    char job_id[128]; path_copy(job_id, sizeof(job_id), id->str);
    char err[200]; char *summary = update_job_public_inputs(g, job_id, urls, err, sizeof(err));
    json_free(body); free(arena);
    if (!summary) return samosa_http_json_error(fd, 400, "public_inputs_failed", err);
    int ok = samosa_http_response(fd, 200, "application/json", summary, NULL);
    free(summary); return ok;
}

static int run_scheduled_job_native(Gateway *g, const char *schedule_path, jval *schedule) {
    jval *job_path_value = json_get(schedule, "job_path");
    jval *job_id_value = json_get(schedule, "job_id");
    if (!job_path_value || job_path_value->t != J_STR ||
        !job_id_value || job_id_value->t != J_STR) return 0;
    char *job_raw = read_file_limit(job_path_value->str, 1 << 20), *job_arena = NULL;
    jval *job = job_raw ? json_parse(job_raw, &job_arena) : NULL;
    jval *input = job && job->t == J_OBJ ? json_get(job, "input") : NULL;
    jval *folder = input && input->t == J_OBJ ? json_get(input, "folder") : NULL;
    jval *public_inputs = job && job->t == J_OBJ ? json_get(job, "public_inputs") : NULL;
    int has_public = public_inputs && public_inputs->t == J_ARR;
    if ((!folder || folder->t != J_STR) && !has_public) {
        json_free(job); free(job_arena); free(job_raw);
        write_schedule_with_status(schedule_path, schedule, "failed", 1, "job_unavailable");
        return 0;
    }
    char events_path[PATH_MAX];
    if (!job_state_path(g, job_id_value->str, "events.jsonl", events_path, 1)) {
        json_free(job); free(job_arena); free(job_raw); return 0;
    }
    int seq = 1;
    TextBuffer fields = {0};
    text_add(&fields, "\"job_id\":"); text_json_string(&fields, job_id_value->str);
    text_add(&fields, ",\"job_path\":"); text_json_string(&fields, job_path_value->str);
    append_job_event_file(events_path, &seq, "scheduled_job_start", fields.data);
    free(fields.data);
    /* Comparison workflow: fetch the user's public URLs, persist only new/changed
       pages. The local folder (if any) and the changed items are both left on
       disk for a later model comparison step; the runner does the deterministic
       fetch + change detection here. */
    if (has_public) {
        char perr[200];
        char *summary = update_job_public_inputs(g, job_id_value->str, public_inputs, perr, sizeof(perr));
        int ok = summary != NULL;
        TextBuffer ev = {0};
        if (ok) {
            char *sarena = NULL; jval *sj = json_parse(summary, &sarena);
            jval *ck = sj ? json_get(sj, "checked") : NULL, *ch = sj ? json_get(sj, "changed") : NULL;
            char nums[96];
            snprintf(nums, sizeof(nums), "\"kind\":\"public\",\"checked\":%d,\"changed\":%d",
                     ck && ck->t == J_NUM ? (int)ck->num : 0, ch && ch->t == J_NUM ? (int)ch->num : 0);
            text_add(&ev, nums);
            json_free(sj); free(sarena);
        } else {
            text_add(&ev, "\"kind\":\"public\",\"error\":"); text_json_string(&ev, perr);
        }
        append_job_event_file(events_path, &seq, ok ? "scheduled_job_complete" : "error", ev.data);
        free(ev.data); free(summary);
        json_free(job); free(job_arena); free(job_raw);
        write_schedule_with_status(schedule_path, schedule, ok ? "complete" : "failed", ok ? 0 : 1,
                                   ok ? "complete" : "public_fetch_failed");
        return ok;
    }
    jval *organize = json_get(job, "organize");
    if (!organize || organize->t != J_OBJ) {
        char *argv[] = {g->samosa_fs, "survey", "--max-file-bytes", "104857600", folder->str, NULL};
        int status = 0; char *raw = run_capture(g, g->samosa_fs, argv, 1 << 20, &status);
        int ok = raw && WIFEXITED(status) && !WEXITSTATUS(status);
        fields.data = NULL; fields.len = fields.cap = 0;
        text_add(&fields, "\"kind\":\"report\"");
        append_job_event_file(events_path, &seq, ok ? "scheduled_job_complete" : "error", fields.data);
        free(fields.data); free(raw); json_free(job); free(job_arena); free(job_raw);
        write_schedule_with_status(schedule_path, schedule, ok ? "complete" : "failed", ok ? 0 : 1,
                                   ok ? "complete" : "folder_scan_failed");
        return ok;
    }
    char *argv[] = {g->samosa_fs, "list", "--max-file-bytes", "104857600", folder->str, NULL};
    int status = 0; char *list_raw = run_capture(g, g->samosa_fs, argv, 16 << 20, &status);
    char *list_arena = NULL; jval *listing = list_raw ? json_parse(list_raw, &list_arena) : NULL;
    jval *items = listing && listing->t == J_OBJ ? json_get(listing, "items") : NULL;
    if (!items || items->t != J_ARR || !WIFEXITED(status) || WEXITSTATUS(status)) {
        append_job_event_file(events_path, &seq, "error", "\"message\":\"folder index failed\"");
        json_free(listing); free(list_arena); free(list_raw); json_free(job); free(job_arena); free(job_raw);
        write_schedule_with_status(schedule_path, schedule, "failed", 1, "folder_index_failed");
        return 0;
    }
    int moved = 0, skipped = 0;
    char applied_path[PATH_MAX]; job_state_path(g, job_id_value->str, "applied.jsonl", applied_path, 1);
    TextBuffer applied = {0};
    for (int i = 0; i < items->len; ++i) {
        jval *item = items->kids[i], *path = json_get(item, "path"), *name = json_get(item, "name"), *media = json_get(item, "media_type");
        if (!path || path->t != J_STR || !name || name->t != J_STR) { ++skipped; continue; }
        if (strstr(path->str, "/Organized/")) { ++skipped; continue; }
        const char *type_folder = type_folder_for(name->str, media && media->t == J_STR ? media->str : "");
        char dst[PATH_MAX];
        if (snprintf(dst, sizeof(dst), "%s/Organized/%s/%s", folder->str, type_folder, name->str) >= (int)sizeof(dst)) { ++skipped; continue; }
        char *mvargv[] = {g->samosa_fs, "move", "--root", folder->str, path->str, dst, NULL};
        int mvstatus = 0; char *mvraw = run_capture(g, g->samosa_fs, mvargv, 65536, &mvstatus);
        int ok_move = mvraw && WIFEXITED(mvstatus) && !WEXITSTATUS(mvstatus) && strstr(mvraw, "\"moved\":true");
        if (ok_move) {
            ++moved;
            text_add(&applied, "{\"src\":"); text_json_string(&applied, path->str);
            text_add(&applied, ",\"dst\":"); text_json_string(&applied, dst); text_add(&applied, "}\n");
        } else ++skipped;
        free(mvraw);
    }
    if (moved) write_small_file(applied_path, applied.data);
    free(applied.data);
    char nums[128]; snprintf(nums, sizeof(nums), "\"applied\":%d,\"skipped\":%d", moved, skipped);
    append_job_event_file(events_path, &seq, "applied", nums);
    snprintf(nums, sizeof(nums), "\"job_id\":\"%s\",\"applied\":%d,\"skipped\":%d", job_id_value->str, moved, skipped);
    append_job_event_file(events_path, &seq, "scheduled_job_complete", nums);
    json_free(listing); free(list_arena); free(list_raw); json_free(job); free(job_arena); free(job_raw);
    write_schedule_with_status(schedule_path, schedule, "complete", 0, "complete");
    return 1;
}

static int jobsd_once_native(Gateway *g, int fd, const SamosaHttpRequest *request) {
    int now = current_minutes_local(), on_battery = host_on_battery();
    long now_epoch = (long)time(NULL);
    if (request && request->body_len) {
        char *arena = NULL; jval *body = json_parse(request->body, &arena);
        jval *n = body && body->t == J_OBJ ? json_get(body, "now_minutes") : NULL;
        jval *b = body && body->t == J_OBJ ? json_get(body, "on_battery") : NULL;
        jval *e = body && body->t == J_OBJ ? json_get(body, "now_epoch") : NULL;
        if (n && n->t == J_NUM) now = (int)n->num;
        if (b && b->t == J_BOOL) on_battery = b->boolean;
        if (e && e->t == J_NUM) now_epoch = (long)e->num;
        json_free(body); free(arena);
    }
    DIR *dir = opendir(g->jobs_root);
    TextBuffer decisions = {0}; int count = 0;
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (entry->d_name[0] == '.') continue;
            char schedule_path[PATH_MAX];
            if (!job_state_path(g, entry->d_name, "schedule.json", schedule_path, 0)) continue;
            char *raw = read_file_limit(schedule_path, 1 << 20), *arena = NULL;
            jval *schedule = raw ? json_parse(raw, &arena) : NULL;
            if (!schedule || schedule->t != J_OBJ) { json_free(schedule); free(arena); free(raw); continue; }
            jval *dl = json_get(schedule, "deadline_epoch");
            int window_expired = dl && dl->t == J_NUM && (long)dl->num > 0 && now_epoch >= (long)dl->num;
            char reason[64];
            int should_run = schedule_decision(schedule, now, on_battery, window_expired, reason, sizeof(reason));
            int ran = 0;
            if (should_run) {
                jval *ka = json_get(schedule, "keep_awake");
                pid_t caffeinate = (ka && ka->t == J_BOOL && !ka->boolean) ? -1 : spawn_keep_awake(g);
                ran = run_scheduled_job_native(g, schedule_path, schedule);
                stop_tracked(g, caffeinate);
            } else if (!strcmp(reason, "window_expired"))
                /* skip policy: the window came and went; retire the schedule so it
                   is not re-evaluated on every future poll. */
                write_schedule_with_status(schedule_path, schedule, "expired", 0, "window_expired");
            if (count++ && !text_add(&decisions, ",")) {}
            text_add(&decisions, "{\"job_id\":"); text_json_string(&decisions, entry->d_name);
            text_add(&decisions, ",\"action\":"); text_json_string(&decisions, should_run ? "run" : "defer");
            text_add(&decisions, ",\"reason\":"); text_json_string(&decisions, reason);
            if (should_run) { text_add(&decisions, ",\"run\":{\"status\":"); text_json_string(&decisions, ran ? "complete" : "failed"); text_add(&decisions, "}"); }
            text_add(&decisions, "}");
            json_free(schedule); free(arena); free(raw);
        }
        closedir(dir);
    }
    TextBuffer response = {0};
    text_add(&response, "{\"ok\":true,\"decisions\":["); text_add(&response, decisions.data ? decisions.data : ""); text_add(&response, "]}");
    int ok = fd >= 0 ? samosa_http_response(fd, 200, "application/json", response.data, NULL) :
        (printf("%s\n", response.data), 1);
    free(response.data); free(decisions.data); return ok;
}

static void launchd_plist_build(Gateway *g, TextBuffer *plist) {
    char program[PATH_MAX];
    if (!path_join(program, sizeof(program), g->home, "current/bin/samosa-jobsd"))
        path_copy(program, sizeof(program), "samosa-jobsd");
    text_add(plist, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                    "<plist version=\"1.0\"><dict>"
                    "<key>Label</key><string>com.samosa.jobsd</string>"
                    "<key>ProgramArguments</key><array><string>");
    text_add(plist, program);
    text_add(plist, "</string><string>jobsd-once</string></array>"
                    "<key>RunAtLoad</key><true/><key>StartInterval</key><integer>300</integer>"
                    "<key>StandardOutPath</key><string>");
    text_add(plist, g->home); text_add(plist, "/logs/jobsd.out.log</string>"
                    "<key>StandardErrorPath</key><string>");
    text_add(plist, g->home); text_add(plist, "/logs/jobsd.err.log</string>"
                    "</dict></plist>\n");
}

static int jobs_launchd_plist(Gateway *g, int fd) {
    TextBuffer plist = {0};
    launchd_plist_build(g, &plist);
    int ok = plist.data && samosa_http_response(fd, 200, "application/xml", plist.data, NULL);
    free(plist.data); return ok;
}

/* The LaunchAgents directory and plist path. Overridable for tests so the suite
   never installs a real agent on the developer's machine. */
static int launchd_agents_dir(char out[PATH_MAX]) {
    const char *override = getenv("SAMOSA_LAUNCH_AGENTS_DIR");
    if (override) return path_copy(out, PATH_MAX, override);
    const char *user_home = getenv("HOME");
    return user_home && path_join(out, PATH_MAX, user_home, "Library/LaunchAgents");
}

static int launchd_plist_path(char out[PATH_MAX]) {
    char dir[PATH_MAX];
    return launchd_agents_dir(dir) && path_join(out, PATH_MAX, dir, "com.samosa.jobsd.plist");
}

/* A dry run writes and manages the plist file but never invokes launchctl, so
   tests (and non-macOS hosts) do not touch a real launchd domain. */
static int launchd_dry_run(void) {
#ifdef __APPLE__
    return getenv("SAMOSA_LAUNCHD_DRYRUN") != NULL;
#else
    return 1;
#endif
}

static int run_launchctl(Gateway *g, const char *verb, const char *argument) {
    char *argv[] = {(char *)"/bin/launchctl", (char *)verb, (char *)"-w", (char *)argument, NULL};
    if (!strcmp(verb, "list")) { argv[2] = (char *)argument; argv[3] = NULL; }
    int status = 0; char *out = run_capture(g, "/bin/launchctl", argv, 1 << 16, &status);
    int ok = out && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    free(out); return ok;
}

static int jobs_launchd_install(Gateway *g, int fd) {
    char plist_path[PATH_MAX], agents[PATH_MAX], logs[PATH_MAX];
    if (!launchd_agents_dir(agents) || !launchd_plist_path(plist_path))
        return samosa_http_json_error(fd, 500, "launchd_path", "Could not resolve the LaunchAgents path.");
    TextBuffer plist = {0}; launchd_plist_build(g, &plist);
    int wrote = plist.data && mkdirs(agents) &&
                path_join(logs, sizeof(logs), g->home, "logs") && mkdirs(logs) &&
                write_small_file(plist_path, plist.data);
    free(plist.data);
    if (!wrote) return samosa_http_json_error(fd, 500, "launchd_write", "Could not write the launchd plist.");
    int dry = launchd_dry_run(), loaded = 0;
    if (!dry) { run_launchctl(g, "unload", plist_path); loaded = run_launchctl(g, "load", plist_path); }
    TextBuffer resp = {0};
    int ok = text_add(&resp, "{\"ok\":true,\"plist_path\":") && text_json_string(&resp, plist_path) &&
        text_add(&resp, ",\"loaded\":") && text_add(&resp, loaded ? "true" : "false") &&
        text_add(&resp, ",\"dry_run\":") && text_add(&resp, dry ? "true" : "false") && text_add(&resp, "}");
    int sent = ok && samosa_http_response(fd, 200, "application/json", resp.data, NULL);
    free(resp.data); return sent;
}

static int jobs_launchd_uninstall(Gateway *g, int fd) {
    char plist_path[PATH_MAX];
    if (!launchd_plist_path(plist_path))
        return samosa_http_json_error(fd, 500, "launchd_path", "Could not resolve the LaunchAgents path.");
    int dry = launchd_dry_run();
    if (!dry) run_launchctl(g, "unload", plist_path);
    int removed = (access(plist_path, F_OK) != 0) || (unlink(plist_path) == 0);
    TextBuffer resp = {0};
    int ok = text_add(&resp, "{\"ok\":true,\"removed\":") && text_add(&resp, removed ? "true" : "false") &&
        text_add(&resp, ",\"dry_run\":") && text_add(&resp, dry ? "true" : "false") && text_add(&resp, "}");
    int sent = ok && samosa_http_response(fd, 200, "application/json", resp.data, NULL);
    free(resp.data); return sent;
}

static int jobs_launchd_status(Gateway *g, int fd) {
    char plist_path[PATH_MAX];
    if (!launchd_plist_path(plist_path))
        return samosa_http_json_error(fd, 500, "launchd_path", "Could not resolve the LaunchAgents path.");
    int installed = access(plist_path, F_OK) == 0;
    int dry = launchd_dry_run(), loaded = 0;
    if (!dry) loaded = run_launchctl(g, "list", "com.samosa.jobsd");
    TextBuffer resp = {0};
    int ok = text_add(&resp, "{\"installed\":") && text_add(&resp, installed ? "true" : "false") &&
        text_add(&resp, ",\"loaded\":") && text_add(&resp, loaded ? "true" : "false") &&
        text_add(&resp, ",\"dry_run\":") && text_add(&resp, dry ? "true" : "false") &&
        text_add(&resp, ",\"plist_path\":") && text_json_string(&resp, plist_path) && text_add(&resp, "}");
    int sent = ok && samosa_http_response(fd, 200, "application/json", resp.data, NULL);
    free(resp.data); return sent;
}

static void interactive_start(Gateway *g) {
    atomic_store(&g->interactive_active, 1);
}

static void interactive_finish(Gateway *g) {
    atomic_store(&g->last_interactive_mono_ms, monotonic_millis());
    atomic_store(&g->last_interactive_wall_ms, wall_millis());
    atomic_store(&g->interactive_active, 0);
}

static int interactive_cooldown_ms(void) {
    const char *env = getenv("SAMOSA_INTERACTIVE_COOLDOWN_S");
    double seconds = env ? atof(env) : 60.0;
    if (seconds < 0) seconds = 0;
    if (seconds > 3600) seconds = 3600;
    return (int)(seconds * 1000.0 + 0.5);
}

static int interactive_recent(Gateway *g) {
    if (atomic_load(&g->interactive_active)) return 1;
    long long last = atomic_load(&g->last_interactive_mono_ms);
    int cooldown = interactive_cooldown_ms();
    return last > 0 && cooldown > 0 && monotonic_millis() - last < cooldown;
}

static int job_pause_when_user_active(jval *job) {
    jval *resources = job && job->t == J_OBJ ? json_get(job, "resources") : NULL;
    jval *value = resources && resources->t == J_OBJ ? json_get(resources, "pause_when_user_active") : NULL;
    return value && value->t == J_BOOL && value->boolean;
}

static int definition_interlock(Gateway *g, int fd, const char *job_id, int enabled,
                                int *seq) {
    if (!enabled || !interactive_recent(g)) return 1;
    int number = ++(*seq);
    TextBuffer paused = {0};
    char num[32], cooldown[32];
    snprintf(num, sizeof(num), "%d", number);
    snprintf(cooldown, sizeof(cooldown), "%.3f", interactive_cooldown_ms() / 1000.0);
    if (!text_add(&paused, "{\"seq\":") || !text_add(&paused, num) ||
        !text_add(&paused, ",\"type\":\"job_paused\",\"job_id\":") ||
        !text_json_string(&paused, job_id) ||
        !text_add(&paused, ",\"reason\":\"interactive_chat\",\"cooldown_seconds\":") ||
        !text_add(&paused, cooldown) || !text_add(&paused, "}")) {
        free(paused.data); return 0;
    }
    if (!sse_json(fd, paused.data)) { free(paused.data); return 0; }
    free(paused.data);

    long long started = monotonic_millis();
    while (!atomic_load(&g->stopping) && interactive_recent(g)) sleep_millis(50);
    number = ++(*seq);
    TextBuffer resumed = {0};
    char waited[32];
    snprintf(num, sizeof(num), "%d", number);
    snprintf(waited, sizeof(waited), "%.3f", (monotonic_millis() - started) / 1000.0);
    if (!text_add(&resumed, "{\"seq\":") || !text_add(&resumed, num) ||
        !text_add(&resumed, ",\"type\":\"job_resumed\",\"job_id\":") ||
        !text_json_string(&resumed, job_id) ||
        !text_add(&resumed, ",\"reason\":\"interactive_chat\",\"paused_seconds\":") ||
        !text_add(&resumed, waited) || !text_add(&resumed, "}")) {
        free(resumed.data); return 0;
    }
    int ok = sse_json(fd, resumed.data);
    free(resumed.data);
    return ok && !atomic_load(&g->stopping);
}

static char *backend_json(Gateway *g, const char *payload) {
    if (!backend_probe(g)) return NULL;
    int fd = tcp_connect(g->backend_port);
    if (fd < 0) return NULL;
    pthread_mutex_lock(&g->mu); g->upstream_fd = fd; pthread_mutex_unlock(&g->mu);
    atomic_fetch_add(&g->generating, 1);
    char header[512];
    int n = snprintf(header, sizeof(header),
        "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
        "Content-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        g->backend_port, strlen(payload));
    if (n <= 0 || (size_t)n >= sizeof(header) ||
        !samosa_send_all(fd, header, (size_t)n) ||
        !samosa_send_all(fd, payload, strlen(payload))) {
        pthread_mutex_lock(&g->mu); if (g->upstream_fd == fd) g->upstream_fd = -1; pthread_mutex_unlock(&g->mu);
        atomic_fetch_sub(&g->generating, 1); close(fd); return NULL;
    }
    TextBuffer response = {0}; char chunk[65536];
    while (response.len < SAMOSA_HTTP_MAX_BODY + SAMOSA_HTTP_MAX_HEADER) {
        ssize_t got = recv(fd, chunk, sizeof(chunk), 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        if (!text_add_n(&response, chunk, (size_t)got)) break;
    }
    pthread_mutex_lock(&g->mu); if (g->upstream_fd == fd) g->upstream_fd = -1; pthread_mutex_unlock(&g->mu);
    atomic_fetch_sub(&g->generating, 1); close(fd);
    if (!response.data || !strstr(response.data, " 200 ")) {
        free(response.data); return NULL;
    }
    char *body = strstr(response.data, "\r\n\r\n");
    if (!body) { free(response.data); return NULL; }
    body += 4;
    char *copy = strdup(body);
    free(response.data); return copy;
}

static int job_inference_max_tokens(jval *job) {
    int max_tokens = 1024;
    jval *inference = job && job->t == J_OBJ ? json_get(job, "inference") : NULL;
    jval *value = inference && inference->t == J_OBJ ? json_get(inference, "max_tokens") : NULL;
    if (value && value->t == J_NUM && value->num >= 1 && value->num <= 8192)
        max_tokens = (int)value->num;
    return max_tokens;
}

static int schema_type_prompt(TextBuffer *out, jval *type) {
    if (!type) return 1;
    if (type->t == J_STR) return text_add(out, type->str);
    if (type->t != J_ARR) return 1;
    int wrote = 0;
    for (int i = 0; i < type->len; ++i) {
        if (!type->kids[i] || type->kids[i]->t != J_STR) continue;
        if (wrote && !text_add(out, " or ")) return 0;
        if (!text_add(out, type->kids[i]->str)) return 0;
        wrote = 1;
    }
    return 1;
}

static int schema_field_prompt(TextBuffer *out, const char *key, jval *properties) {
    if (!text_add(out, "- ") || !text_add(out, key)) return 0;
    jval *property = properties && properties->t == J_OBJ ? json_get(properties, key) : NULL;
    jval *type = property && property->t == J_OBJ ? json_get(property, "type") : NULL;
    if (type && (!text_add(out, " (") || !schema_type_prompt(out, type) || !text_add(out, ")")))
        return 0;
    return text_add(out, "\n");
}

static int schema_fields_prompt(TextBuffer *out, jval *schema) {
    jval *required = schema && schema->t == J_OBJ ? json_get(schema, "required") : NULL;
    jval *properties = schema && schema->t == J_OBJ ? json_get(schema, "properties") : NULL;
    if (!text_add(out, "Return exactly one JSON object with these keys and no other keys. "
                       "Include every key listed; use null for any value that is not present "
                       "in the source:\n"))
        return 0;
    int wrote = 0;
    /* List all declared properties, not just the required ones — otherwise the
       model is told to omit optional fields ("no other keys") and never emits
       them. Fall back to the required names only when no properties block. */
    if (properties && properties->t == J_OBJ) {
        for (int i = 0; i < properties->len; ++i) {
            if (!schema_field_prompt(out, properties->keys[i], properties)) return 0;
            wrote = 1;
        }
    }
    if (!wrote && required && required->t == J_ARR) {
        for (int i = 0; i < required->len; ++i) {
            if (!required->kids[i] || required->kids[i]->t != J_STR) continue;
            if (!schema_field_prompt(out, required->kids[i]->str, properties)) return 0;
            wrote = 1;
        }
    }
    if (!wrote && !text_add(out, "- value\n")) return 0;
    return text_add(out,
        "Do not output arrays or nested objects. If a field has multiple values, "
        "join them into one string with \"; \".\n");
}

static char *model_extract(Gateway *g, const char *instruction, jval *schema,
                           const char *source, const char *image_data_uri, int max_tokens,
                           double *model_call_seconds) {
    TextBuffer fields = {0}, user = {0}, payload = {0};
    char max_tokens_text[32];
    if (max_tokens < 1 || max_tokens > 8192) max_tokens = 1024;
    snprintf(max_tokens_text, sizeof(max_tokens_text), "%d", max_tokens);
    if (!schema_fields_prompt(&fields, schema) ||
        !text_add(&user, instruction && *instruction ? instruction : "Extract the requested fields.") ||
        !text_add(&user, "\n") || !text_add(&user, fields.data ? fields.data : "") ||
        !text_add(&user, image_data_uri ? "\nSource image:" : "\nSource:\n") ||
        (!image_data_uri && !text_add(&user, source ? source : "")) ||
        !text_add(&payload, "{\"model\":") || !text_json_string(&payload, backend_model(g->backend)) ||
        !text_add(&payload, ",\"messages\":[{\"role\":\"system\",\"content\":\"Extract structured data. Return exactly one JSON object and no prose.\"},{\"role\":\"user\",\"content\":")) {
        free(fields.data); free(user.data); free(payload.data); return NULL;
    }
    int ok = 1;
    if (image_data_uri) {
        ok = text_add(&payload, "[{\"type\":\"text\",\"text\":") &&
             text_json_string(&payload, user.data) &&
             text_add(&payload, "},{\"type\":\"image_url\",\"image_url\":{\"url\":") &&
             text_json_string(&payload, image_data_uri) &&
             text_add(&payload, "}}]");
    } else {
        ok = text_json_string(&payload, user.data);
    }
    /* Disable reasoning for both backend families: llama-server (Ornith/Bonsai)
       reads chat_template_kwargs.enable_thinking; the Qwen C engine ignores that
       and only honors the top-level "thinking" field. Without "thinking":"off"
       Qwen burns the token budget reasoning and returns no JSON object. */
    if (!ok ||
        !text_add(&payload, "}],\"stream\":false,\"thinking\":\"off\",\"chat_template_kwargs\":{\"enable_thinking\":false},\"response_format\":{\"type\":\"json_object\"},\"max_tokens\":") ||
        !text_add(&payload, max_tokens_text) || !text_add(&payload, "}")) {
        free(fields.data); free(user.data); free(payload.data); return NULL;
    }
    double started = monotonic_seconds();
    char *raw = backend_json(g, payload.data);
    if (model_call_seconds) *model_call_seconds = monotonic_seconds() - started;
    free(fields.data); free(user.data); free(payload.data);
    char *arena = NULL; jval *root = raw ? json_parse(raw, &arena) : NULL;
    jval *choices = root && root->t == J_OBJ ? json_get(root, "choices") : NULL;
    jval *message = choices && choices->t == J_ARR && choices->len ? json_get(choices->kids[0], "message") : NULL;
    jval *content = message && message->t == J_OBJ ? json_get(message, "content") : NULL;
    char *result = content && content->t == J_STR ? strdup(content->str) : NULL;
    json_free(root); free(arena); free(raw); return result;
}

static int sse_json(int fd, const char *json) {
    return samosa_send_all(fd, "data: ", 6) && samosa_send_all(fd, json, strlen(json)) &&
           samosa_send_all(fd, "\n\n", 2);
}

/* Find jobs retain their event stream for recovery and auditing.  The disk
   append happens before the socket write, so a disconnect can be replayed
   without claiming an event that was never made durable. */
static int job_sse_json(Gateway *g, int fd, const char *job_id, const char *json) {
    return job_append_jsonl(g, job_id, "events.jsonl", json) && sse_json(fd, json);
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

/* Path relative to the job folder, for the model to pass back to the jailed
   tools. Non-recursive listings are flat, so this is the basename; the prefix
   strip keeps it correct if the listing ever goes recursive. */
static const char *rel_to_folder(const char *folder, const char *path, const char *name) {
    size_t flen = strlen(folder);
    if (!strncmp(path, folder, flen) && path[flen] == '/') return path + flen + 1;
    return name;
}

/* JI.1 names the list fields explicitly.  Prefer the current root-relative
   path and magic-byte type, while accepting the legacy aliases so an upgraded
   gateway can still run with an older sidecar. */
static const char *ji_item_string(jval *item, const char *preferred,
                                  const char *legacy, const char *fallback) {
    jval *value = item && item->t == J_OBJ ? json_get(item, preferred) : NULL;
    if ((!value || value->t != J_STR) && legacy)
        value = item && item->t == J_OBJ ? json_get(item, legacy) : NULL;
    return value && value->t == J_STR ? value->str : fallback;
}

static const char *ji_item_rel_path(const char *folder, jval *item,
                                    const char *name) {
    const char *rel = ji_item_string(item, "rel_path", NULL, NULL);
    if (rel && *rel) return rel;
    return rel_to_folder(folder, ji_item_string(item, "path", NULL, name), name);
}

/* One "N. name (type, size bytes, YYYY-MM-DD)" row for a triage/classify batch.
   Filename judgment is the model's job (design law 1); C only lays out facts. */
static int append_file_row(TextBuffer *out, int index, const char *name,
                           const char *media_type, long long size, double mtime) {
    char head[64]; snprintf(head, sizeof(head), "%d. ", index);
    char date[16] = "unknown"; time_t t = (time_t)mtime; struct tm tmv;
    if (mtime > 0 && gmtime_r(&t, &tmv)) strftime(date, sizeof(date), "%Y-%m-%d", &tmv);
    char meta[96];
    snprintf(meta, sizeof(meta), " (%s, %lld bytes, %s)\n",
             media_type ? media_type : "unknown", size, date);
    return text_add(out, head) && text_add(out, name) && text_add(out, meta);
}

/* Heap copy of the first balanced JSON array in s (string-aware brace/bracket
   scan), recovering it from fences or prose exactly as first_json_object does
   for objects. The triage/classify contract is "output a JSON array". */
static char *first_json_array(const char *s) {
    if (!s) return NULL;
    const char *start = strchr(s, '[');
    if (!start) return NULL;
    int depth = 0, in_str = 0, esc = 0;
    const char *p = start;
    for (; *p; ++p) {
        char c = *p;
        if (in_str) {
            if (esc) esc = 0; else if (c == '\\') esc = 1; else if (c == '"') in_str = 0;
        } else if (c == '"') in_str = 1;
        else if (c == '[') ++depth;
        else if (c == ']' && --depth == 0) { ++p; break; }
    }
    if (depth != 0) return NULL;
    size_t len = (size_t)(p - start);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, start, len); out[len] = 0;
    return out;
}

static char *reshape_doc_read_result(const char *full_lines_json, const char *requested_detail, int page_start, int page_count_req) {
    char *arena = NULL;
    jval *root = json_parse(full_lines_json, &arena);
    if (!root || root->t != J_OBJ) {
        if (arena) free(arena);
        if (root) json_free(root);
        return strdup(full_lines_json);
    }
    jval *ok_v = json_get(root, "ok");
    if (!ok_v || ok_v->t != J_BOOL || !ok_v->boolean) {
        json_free(root); free(arena);
        return strdup(full_lines_json);
    }
    jval *pages_v = json_get(root, "pages");
    int total_pages = pages_v && pages_v->t == J_ARR ? pages_v->len : 0;
    
    int start_idx = page_start - 1;
    if (start_idx < 0) start_idx = 0;
    if (start_idx > total_pages) start_idx = total_pages;
    int end_idx = total_pages;
    if (page_count_req > 0 && start_idx + page_count_req < total_pages) {
        end_idx = start_idx + page_count_req;
    }

    TextBuffer out = {0};
    text_add(&out, "{\"ok\":true,\"page_count\":");
    char numbuf[32];
    snprintf(numbuf, sizeof(numbuf), "%d", total_pages);
    text_add(&out, numbuf);

    TextBuffer text_buf = {0};
    int any_unc = 0;
    int needs_rev = 0;

    text_add(&out, ",\"pages\":[");
    int emitted_p = 0;
    for (int i = start_idx; i < end_idx; i++) {
        jval *p = pages_v->kids[i];
        jval *p_idx = json_get(p, "index");
        jval *p_src = json_get(p, "source");
        jval *p_lt = json_get(p, "lines_total");
        jval *p_lu = json_get(p, "lines_uncertain");
        jval *p_mc = json_get(p, "min_conf");
        jval *p_nr = json_get(p, "needs_review");
        jval *p_lines = json_get(p, "lines");

        if (p_lu && p_lu->num > 0) any_unc = 1;
        if (p_nr && p_nr->t == J_BOOL && p_nr->boolean) needs_rev = 1;

        if (emitted_p > 0) text_add(&out, ",");
        text_add(&out, "{\"index\":");
        snprintf(numbuf, sizeof(numbuf), "%d", p_idx ? (int)p_idx->num : (i + 1));
        text_add(&out, numbuf);
        text_add(&out, ",\"source\":");
        text_json_string(&out, p_src && p_src->t == J_STR ? p_src->str : "ocr");
        text_add(&out, ",\"lines_total\":");
        snprintf(numbuf, sizeof(numbuf), "%d", p_lt ? (int)p_lt->num : 0);
        text_add(&out, numbuf);
        text_add(&out, ",\"lines_uncertain\":");
        snprintf(numbuf, sizeof(numbuf), "%d", p_lu ? (int)p_lu->num : 0);
        text_add(&out, numbuf);
        text_add(&out, ",\"min_conf\":");
        snprintf(numbuf, sizeof(numbuf), "%.4f", p_mc ? p_mc->num : 1.0);
        text_add(&out, numbuf);
        text_add(&out, ",\"needs_review\":");
        text_add(&out, (p_nr && p_nr->boolean) ? "true" : "false");

        if (p_lines && p_lines->t == J_ARR) {
            for (int l = 0; l < p_lines->len; l++) {
                jval *ltxt = json_get(p_lines->kids[l], "text");
                if (ltxt && ltxt->t == J_STR) {
                    if (text_buf.len > 0) text_add(&text_buf, "\n");
                    text_add(&text_buf, ltxt->str);
                }
            }
        }

        if (!strcmp(requested_detail, "lines") && p_lines && p_lines->t == J_ARR) {
            text_add(&out, ",\"lines\":");
            text_json_value(&out, p_lines);
        }

        text_add(&out, "}");
        emitted_p++;
    }
    text_add(&out, "],\"text\":");
    text_json_string(&out, text_buf.data ? text_buf.data : "");
    text_add(&out, ",\"any_uncertain\":");
    text_add(&out, any_unc ? "true" : "false");
    text_add(&out, ",\"needs_review\":");
    text_add(&out, needs_rev ? "true" : "false");
    text_add(&out, "}");

    free(text_buf.data);
    json_free(root); free(arena);
    return out.data;
}

static char *first_json_object(const char *s);

static void escalate_low_conf_crops(Gateway *g, const char *absolute, jval *lines_arr, int *out_l_unc, double *out_min_c) {
    if (!lines_arr || lines_arr->t != J_ARR || lines_arr->len == 0) return;
    if (!backend_supports_images(g, g->backend)) return;

    char crop_dir[PATH_MAX];
    snprintf(crop_dir, sizeof(crop_dir), "/tmp/samosa_crops_%d_%ld", (int)getpid(), (long)time(NULL));
    if (!mkdirs(crop_dir)) return;

    char *argv_ocr[] = {g->samosa_ocr, "read", (char *)absolute, "--emit-crops", crop_dir, "--below", "0.84", NULL};
    int status = 0;
    char *ocr_raw = run_capture(g, g->samosa_ocr, argv_ocr, 16 << 20, &status);
    if (!ocr_raw || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(ocr_raw);
        rmdir(crop_dir);
        return;
    }
    free(ocr_raw);

    int l_unc = 0;
    double min_c = 1.0;
    for (int i = 0; i < lines_arr->len; i++) {
        jval *line = lines_arr->kids[i];
        jval *cf = json_get(line, "conf");
        double c = cf ? cf->num : 0.0;
        if (c < 0.84) {
            char crop_path[PATH_MAX];
            snprintf(crop_path, sizeof(crop_path), "%s/crop_%03d.ppm", crop_dir, i);
            struct stat st;
            if (stat(crop_path, &st) == 0 && st.st_size > 0) {
                size_t bytes_len = 0;
                unsigned char *bytes = read_file_bytes_limit(crop_path, 10 << 20, &bytes_len);
                if (bytes) {
                    char *b64 = base64_encode_bytes(bytes, bytes_len);
                    free(bytes);
                    if (b64) {
                        TextBuffer uri = {0};
                        text_add(&uri, "data:image/x-portable-pixmap;base64,");
                        text_add(&uri, b64);
                        free(b64);

                        jval *crop_schema = json_parse("{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}}", NULL);
                        double call_sec = 0;
                        char *extracted = model_extract(g, "Read the text in this image crop accurately.", crop_schema, NULL, uri.data, 256, &call_sec);
                        free(uri.data);
                        json_free(crop_schema);
                        if (extracted) {
                            char *obj = first_json_object(extracted);
                            char *f_arena = NULL;
                            jval *f_obj = obj ? json_parse(obj, &f_arena) : NULL;
                            jval *v_text = f_obj ? json_get(f_obj, "text") : NULL;
                            if (v_text && v_text->t == J_STR && *v_text->str) {
                                jval *t_node = json_get(line, "text");
                                if (t_node && t_node->t == J_STR) {
                                    free(t_node->str);
                                    t_node->str = strdup(v_text->str);
                                }
                                jval *c_node = json_get(line, "conf");
                                if (c_node && c_node->t == J_NUM) c_node->num = 0.95;
                                jval *r_node = json_get(line, "reader");
                                if (r_node && r_node->t == J_STR) {
                                    free(r_node->str);
                                    r_node->str = strdup("vlm_crop");
                                }
                                c = 0.95;
                            }
                            json_free(f_obj); free(f_arena); free(obj); free(extracted);
                        }
                    }
                }
            }
        }
        if (c < 0.84) l_unc++;
        if (i == 0 || c < min_c) min_c = c;
    }
    *out_l_unc = l_unc;
    *out_min_c = min_c;

    DIR *d = opendir(crop_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, "..")) {
                char fpath[PATH_MAX];
                snprintf(fpath, sizeof(fpath), "%s/%s", crop_dir, ent->d_name);
                unlink(fpath);
            }
        }
        closedir(d);
    }
    rmdir(crop_dir);
}

static char *doc_read_handler(Gateway *g, const char *absolute, jval *args) {
    const char *detail = "text";
    jval *detail_v = args ? json_get(args, "detail") : NULL;
    if (detail_v && detail_v->t == J_STR && (!strcmp(detail_v->str, "lines") || !strcmp(detail_v->str, "text"))) {
        detail = detail_v->str;
    }
    int page_start = 1, page_count_req = -1;
    jval *pages_v = args ? json_get(args, "pages") : NULL;
    if (pages_v && pages_v->t == J_ARR && pages_v->len >= 2) {
        if (pages_v->kids[0]->t == J_NUM) page_start = (int)pages_v->kids[0]->num;
        if (pages_v->kids[1]->t == J_NUM) page_count_req = (int)pages_v->kids[1]->num;
        if (page_start < 1) page_start = 1;
        if (page_count_req < 1 || page_count_req > 5) page_count_req = 5;
    }
    int refresh = 0;
    jval *refresh_v = args ? json_get(args, "refresh") : NULL;
    if (refresh_v && refresh_v->t == J_BOOL) refresh = refresh_v->boolean;

    char hex_key[65];
    if (read_cache_key_file(absolute, hex_key) != 0) {
        return strdup("{\"ok\":false,\"error\":\"image_invalid\"}");
    }
    char cache_root[PATH_MAX];
    read_cache_default_root(cache_root, sizeof(cache_root));
    const char *contract_ver = "reader-v0";
    const char *pack_fp = "reader-v0-small";

    char *cached_lines_json = NULL;
    if (!refresh) {
        cached_lines_json = read_cache_get(cache_root, hex_key, contract_ver, pack_fp);
    }
    if (cached_lines_json) {
        char *res = reshape_doc_read_result(cached_lines_json, detail, page_start, page_count_req);
        free(cached_lines_json);
        return res;
    }

    size_t path_len = strlen(absolute);
    int is_pdf = (path_len >= 4 && strcasecmp(absolute + path_len - 4, ".pdf") == 0);

    TextBuffer full_lines = {0};

    if (is_pdf) {
        char *argv_ext[] = {g->samosa_extract, "--json-pages", (char *)absolute, "1", "100", NULL};
        int status_ext = 0;
        char *ext_raw = run_capture(g, g->samosa_extract, argv_ext, 16 << 20, &status_ext);
        if (!ext_raw || !WIFEXITED(status_ext) || WEXITSTATUS(status_ext) != 0) {
            free(ext_raw);
            return strdup("{\"ok\":false,\"error\":\"image_invalid\"}");
        }
        char *arena_ext = NULL;
        jval *ext_json = json_parse(ext_raw, &arena_ext);
        if (!ext_json || json_get(ext_json, "ok") == NULL || !json_get(ext_json, "ok")->boolean) {
            jval *err_v = ext_json ? json_get(ext_json, "error") : NULL;
            char err_buf[256];
            snprintf(err_buf, sizeof(err_buf), "{\"ok\":false,\"error\":\"%s\"}",
                     err_v && err_v->t == J_STR ? err_v->str : "image_invalid");
            json_free(ext_json); free(arena_ext); free(ext_raw);
            return strdup(err_buf);
        }

        jval *pages_arr = json_get(ext_json, "pages");
        int num_pages = pages_arr && pages_arr->t == J_ARR ? pages_arr->len : 0;
        jval *pc_v = json_get(ext_json, "page_count");
        int total_doc_pages = pc_v ? (int)pc_v->num : num_pages;

        text_add(&full_lines, "{\"ok\":true,\"page_count\":");
        char numbuf[32]; snprintf(numbuf, sizeof(numbuf), "%d", total_doc_pages);
        text_add(&full_lines, numbuf);
        text_add(&full_lines, ",\"pages\":[");

        for (int p = 0; p < num_pages; p++) {
            jval *p_obj = pages_arr->kids[p];
            jval *p_chars = json_get(p_obj, "text_chars");
            jval *p_toks = json_get(p_obj, "tokens");
            jval *p_rf = json_get(p_obj, "has_raster_figure");
            jval *p_txt = json_get(p_obj, "text");

            int chars = p_chars ? (int)p_chars->num : 0;
            int toks = p_toks ? (int)p_toks->num : 0;
            int has_rf = p_rf ? p_rf->boolean : 0;

            int needs_image = (toks > 0 ? (toks < 20) : (chars < 50)) || has_rf;

            if (p > 0) text_add(&full_lines, ",");
            text_add(&full_lines, "{\"index\":");
            snprintf(numbuf, sizeof(numbuf), "%d", p + 1);
            text_add(&full_lines, numbuf);

            if (!needs_image && p_txt && p_txt->t == J_STR) {
                text_add(&full_lines, ",\"source\":\"text_layer\"");
                int line_cnt = 0;
                const char *s = p_txt->str;
                while (*s) {
                    const char *next = strchr(s, '\n');
                    line_cnt++;
                    if (!next) break;
                    s = next + 1;
                }
                snprintf(numbuf, sizeof(numbuf), "%d", line_cnt);
                text_add(&full_lines, ",\"lines_total\":"); text_add(&full_lines, numbuf);
                text_add(&full_lines, ",\"lines_uncertain\":0,\"min_conf\":1.0000,\"needs_review\":false,\"lines\":[");
                s = p_txt->str;
                int l_idx = 0;
                while (*s) {
                    const char *next = strchr(s, '\n');
                    size_t len = next ? (size_t)(next - s) : strlen(s);
                    char *line_buf = malloc(len + 1);
                    memcpy(line_buf, s, len); line_buf[len] = 0;
                    if (l_idx > 0) text_add(&full_lines, ",");
                    text_add(&full_lines, "{\"bbox\":[0,0,0,0],\"text\":");
                    text_json_string(&full_lines, line_buf);
                    text_add(&full_lines, ",\"conf\":1.0000,\"script\":\"printed\",\"reader\":\"text_layer\"}");
                    free(line_buf);
                    l_idx++;
                    if (!next) break;
                    s = next + 1;
                }
                text_add(&full_lines, "]");
            } else {
                char tmp_ppm[PATH_MAX];
                snprintf(tmp_ppm, sizeof(tmp_ppm), "%s/doc_read_%d_p%d.ppm", g->home, (int)getpid(), p + 1);
                char p_str[24]; snprintf(p_str, sizeof(p_str), "%d", p + 1);
                char *argv_rnd[] = {g->samosa_extract, "--render-ppm", (char *)absolute, p_str, tmp_ppm, NULL};
                int status_rnd = 0;
                char *rnd_raw = run_capture(g, g->samosa_extract, argv_rnd, 1 << 20, &status_rnd);
                free(rnd_raw);

                char *argv_ocr[] = {g->samosa_ocr, "read", tmp_ppm, NULL};
                int status_ocr = 0;
                char *ocr_raw = run_capture(g, g->samosa_ocr, argv_ocr, 16 << 20, &status_ocr);
                unlink(tmp_ppm);

                if (!ocr_raw || !WIFEXITED(status_ocr) || WEXITSTATUS(status_ocr) != 0) {
                    free(ocr_raw); json_free(ext_json); free(arena_ext); free(ext_raw);
                    free(full_lines.data);
                    return strdup("{\"ok\":false,\"error\":\"ocr_unavailable\"}");
                }

                char *arena_ocr = NULL;
                jval *ocr_json = json_parse(ocr_raw, &arena_ocr);
                jval *lines_arr = ocr_json ? json_get(ocr_json, "lines") : NULL;

                int l_tot = lines_arr && lines_arr->t == J_ARR ? lines_arr->len : 0;
                int l_unc = 0;
                double min_c = 1.0;
                for (int i = 0; i < l_tot; i++) {
                    jval *cf = json_get(lines_arr->kids[i], "conf");
                    double c = cf ? cf->num : 0.0;
                    if (c < 0.84) l_unc++;
                    if (i == 0 || c < min_c) min_c = c;
                }
                text_add(&full_lines, ",\"source\":\"ocr\",");
                snprintf(numbuf, sizeof(numbuf), "\"lines_total\":%d,\"lines_uncertain\":%d,\"min_conf\":%.4f,\"needs_review\":%s,\"lines\":",
                         l_tot, l_unc, min_c, l_unc > 0 ? "true" : "false");
                text_add(&full_lines, numbuf);
                if (lines_arr) text_json_value(&full_lines, lines_arr);
                else text_add(&full_lines, "[]");

                json_free(ocr_json); free(arena_ocr); free(ocr_raw);
            }
            text_add(&full_lines, "}");
        }
        text_add(&full_lines, "]}");
        json_free(ext_json); free(arena_ext); free(ext_raw);
    } else {
        char *argv_ocr[] = {g->samosa_ocr, "read", (char *)absolute, NULL};
        int status_ocr = 0;
        char *ocr_raw = run_capture(g, g->samosa_ocr, argv_ocr, 16 << 20, &status_ocr);
        if (!ocr_raw || !WIFEXITED(status_ocr) || WEXITSTATUS(status_ocr) != 0) {
            free(ocr_raw);
            free(full_lines.data);
            return strdup("{\"ok\":false,\"error\":\"ocr_unavailable\"}");
        }
        char *arena_ocr = NULL;
        jval *ocr_json = json_parse(ocr_raw, &arena_ocr);
        jval *lines_arr = ocr_json ? json_get(ocr_json, "lines") : NULL;

        int l_tot = lines_arr && lines_arr->t == J_ARR ? lines_arr->len : 0;
        int l_unc = 0;
        double min_c = 1.0;
        for (int i = 0; i < l_tot; i++) {
            jval *cf = json_get(lines_arr->kids[i], "conf");
            double c = cf ? cf->num : 0.0;
            if (c < 0.84) l_unc++;
            if (i == 0 || c < min_c) min_c = c;
        }
        if (l_unc > 0 && backend_supports_images(g, g->backend)) {
            escalate_low_conf_crops(g, absolute, lines_arr, &l_unc, &min_c);
        }
        text_add(&full_lines, "{\"ok\":true,\"page_count\":1,\"pages\":[{\"index\":1,\"source\":\"ocr\",");
        char numbuf[128];
        snprintf(numbuf, sizeof(numbuf), "\"lines_total\":%d,\"lines_uncertain\":%d,\"min_conf\":%.4f,\"needs_review\":%s,\"lines\":",
                 l_tot, l_unc, min_c, l_unc > 0 ? "true" : "false");
        text_add(&full_lines, numbuf);
        if (lines_arr) text_json_value(&full_lines, lines_arr);
        else text_add(&full_lines, "[]");
        text_add(&full_lines, "}]}");

        json_free(ocr_json); free(arena_ocr); free(ocr_raw);
    }

    read_cache_put(cache_root, hex_key, contract_ver, pack_fp, full_lines.data);

    char *res = reshape_doc_read_result(full_lines.data, detail, page_start, page_count_req);
    free(full_lines.data);
    return res;
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
    if (!strcmp(name, "doc.read") || !strcmp(name, "doc_read")) return doc_read_handler(g, absolute, args);
    return strdup("Unknown tool request.");
}

/* The verify-loop tool set (JI.4). doc.read is the instructed reader for PDFs
   and images (RC5's fix); fs_read_text/fs_metadata are raw tier-0 tools;
   ask_user is model-authored only (RC2's fix); finish is the ONLY legal ending
   (JI.5). No fs_move — find is read-only; organize is a JO follow-up. */
static const char *ji_tools_json =
    "[{\"type\":\"function\",\"function\":{\"name\":\"doc.read\",\"description\":\"Read a PDF or image with the tiered OCR + text-layer reader. pages is [start,count] with count up to 5.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"detail\":{\"type\":\"string\",\"enum\":[\"text\",\"lines\"]},\"pages\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},\"refresh\":{\"type\":\"boolean\"}},\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"fs_read_text\",\"description\":\"Read at most 8192 characters from one plain text file\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"fs_metadata\",\"description\":\"Check one file's type, size and metadata without reading content\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"ask_user\",\"description\":\"Ask the user one question, only for genuine ambiguity the goal does not resolve\",\"parameters\":{\"type\":\"object\",\"properties\":{\"question\":{\"type\":\"string\"}},\"required\":[\"question\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"finish\",\"description\":\"End the find job with the verified result. This is the only way to finish.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"matches\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"evidence\":{\"type\":\"string\"},\"page\":{\"type\":\"integer\"},\"confidence\":{\"type\":\"string\",\"enum\":[\"high\",\"medium\"]}},\"required\":[\"path\",\"evidence\"]}},\"rejected_count\":{\"type\":\"integer\"},\"unreadable\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"reason\":{\"type\":\"string\"}}}},\"deferred\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"confidence\":{\"type\":\"string\"}}}},\"notes\":{\"type\":\"string\"}},\"required\":[\"matches\"]}}}]";

static const char *ji_verify_system =
    "You are completing a local file-finding job. The goal and a list of plausible "
    "files are given. Confirm or reject each plausible file by reading its content. "
    "Read PDFs and images with doc.read (it uses OCR with a text-layer fallback); "
    "read plain-text files with fs_read_text; check type, size or date with fs_metadata. "
    "doc.read returns pages in [start,count] chunks of up to 5 pages — ask for more "
    "pages only when needed. This is a sweep: consider every plausible file and collect "
    "ALL matches, each with a short evidence quote and its page number. Call ask_user "
    "only for genuine ambiguity the goal does not resolve — the goal is in this "
    "conversation, so never ask for a detail it already contains (a name, a date, a "
    "phrase). End by calling finish exactly once with your matches, the count you "
    "rejected, any files you could not read, and a short note. Do not answer in prose; "
    "finish and ask_user are the only ways to end.";

/* One non-streaming chat turn with no tools; returns assistant content (heap)
   or NULL. Used for the Phase A/C batch classifiers. llama-server's JSON
   grammar is more reliable for an object than a top-level array. */
static char *ji_model_text(Gateway *g, const char *system, const char *user) {
    TextBuffer payload = {0};
    int ok = text_add(&payload, "{\"model\":") && text_json_string(&payload, backend_model(g->backend)) &&
             text_add(&payload, ",\"messages\":[{\"role\":\"system\",\"content\":") && text_json_string(&payload, system) &&
             text_add(&payload, "},{\"role\":\"user\",\"content\":") && text_json_string(&payload, user) &&
             text_add(&payload, "}],\"stream\":false,\"temperature\":0,\"thinking\":\"off\",\"chat_template_kwargs\":{\"enable_thinking\":false},\"response_format\":{\"type\":\"json_object\"}}");
    if (!ok) { free(payload.data); return NULL; }
    char *raw = backend_json(g, payload.data); free(payload.data);
    char *arena = NULL; jval *root = raw ? json_parse(raw, &arena) : NULL;
    jval *choices = root && root->t == J_OBJ ? json_get(root, "choices") : NULL;
    jval *message = choices && choices->t == J_ARR && choices->len ? json_get(choices->kids[0], "message") : NULL;
    jval *content = message && message->t == J_OBJ ? json_get(message, "content") : NULL;
    char *out = content && content->t == J_STR ? strdup(content->str) : NULL;
    json_free(root); free(arena); free(raw); return out;
}

/* Production requests use {"items":[...]}. Preserve a bare-array fallback
   for existing compatible backends and saved regression fixtures. */
static char *ji_model_items(Gateway *g, const char *system, const char *user) {
    char *content = ji_model_text(g, system, user);
    if (!content) return NULL;
    char *array = first_json_array(content);
    if (array) { free(content); return array; }
    char *object = first_json_object(content);
    char *arena = NULL; jval *root = object ? json_parse(object, &arena) : NULL;
    jval *items = root && root->t == J_OBJ ? json_get(root, "items") : NULL;
    TextBuffer out = {0};
    if (items && items->t == J_ARR) text_json_value(&out, items);
    json_free(root); free(arena); free(object); free(content);
    return out.data;
}

typedef struct { int idx; const char *name; } TriageRow;
static int triage_row_cmp(const void *a, const void *b) {
    return strcasecmp(((const TriageRow *)a)->name, ((const TriageRow *)b)->name);
}

/* Reconstruct Phase A's cursor from its append-only durable output.  This is
   deliberately keyed by jailed relative path, not filename: duplicate names
   in different subdirectories must remain independent work items. */
static void load_saved_triage(Gateway *g, const char *job_id, const char *folder,
                              jval *items, int *verdict, int *done) {
    char path[PATH_MAX], *raw = NULL;
    *done = 0;
    if (!job_state_path(g, job_id, "verdicts.jsonl", path, 0) || !(raw = read_file_limit(path, 8 << 20))) return;
    for (char *save = NULL, *line = strtok_r(raw, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *arena = NULL; jval *row = json_parse(line, &arena);
        jval *relv = row && row->t == J_OBJ ? json_get(row, "rel_path") : NULL;
        jval *conf = row && row->t == J_OBJ ? json_get(row, "confidence") : NULL;
        int rank = conf && conf->t == J_STR ? (!strcmp(conf->str, "high") ? 3 : !strcmp(conf->str, "low") ? 1 : 2) : 0;
        if (rank && relv && relv->t == J_STR) for (int i = 0; i < items->len; ++i) {
            jval *it = items->kids[i], *name = it && it->t == J_OBJ ? json_get(it, "name") : NULL;
            const char *fallback = name && name->t == J_STR ? name->str : "";
            if (!verdict[i] && !strcmp(ji_item_rel_path(folder, it, fallback), relv->str)) {
                verdict[i] = rank; ++*done; break;
            }
        }
        json_free(row); free(arena);
    }
    free(raw);
}

/* Phase A (JI.2, revised 2026-07-23 after E-JI1): the model assigns a
   CONFIDENCE to EVERY filename, in token-sized batches. No C keyword logic
   (design law 1) and — the E-JI1 lesson — NO hard drop: real Ornith excluded an
   anonymous CamScanner scan as "no" and its pixel content was lost. Triage now
   ranks, it does not filter; the skim budget bounds the work. Confidence lands
   in verdicts.jsonl and in the caller's per-item rank array: 3 = high, 2 =
   medium (incl. uninformative/anonymous names — read to know), 1 = low (name
   names a clearly different subject). All ranks flow into the skim (Phase B),
   highest confidence first. */
static int find_triage(Gateway *g, int fd, const char *goal, const char *folder,
                       jval *items, const char *job_id, int *verdict,
                       int *checked, int *seq) {
    const char *system =
        "You are triaging filenames for a local file-finding job. For each numbered file, "
        "output exactly one compact JSON object {\"items\":[{\"i\":1,\"c\":\"h\"},...]}. "
        "One item per file; c is h, m, or l. No prose, reasons, paths, markdown, or explanation. "
        "Judge ONLY from the name, type, size and "
        "date, that reading this file's CONTENT is worth it for the goal. high = the name "
        "strongly indicates a match. medium = the name is plausible OR uninformative — an "
        "anonymous scan (CamScanner, IMG_1234, a bare date/number) says nothing about "
        "content, so it MUST be read to know: that is medium, never low. low = the name "
        "clearly names a DIFFERENT, unrelated subject. Do NOT exclude any file; low only "
        "means read it last.";
    int total = items->len;
    *checked = 0;
    for (int i = 0; i < total; ++i) verdict[i] = 0;
    load_saved_triage(g, job_id, folder, items, verdict, checked);
    TriageRow *rows = malloc((size_t)(total > 0 ? total : 1) * sizeof(*rows));
    if (!rows) return 0;
    int nrows = 0;
    for (int i = 0; i < total; ++i) {
        jval *it = items->kids[i];
        jval *name = it && it->t == J_OBJ ? json_get(it, "name") : NULL;
        if (name && name->t == J_STR && !verdict[i]) { rows[nrows].idx = i; rows[nrows].name = name->str; nrows++; }
    }
    qsort(rows, (size_t)nrows, sizeof(*rows), triage_row_cmp);

    int batches = 0, ri = 0, done = *checked, ok = 1;
    while (ok && ri < nrows) {
        TextBuffer batch = {0};
        if (!text_add(&batch, "Goal: ") || !text_add(&batch, goal) || !text_add(&batch, "\nFiles:\n")) { free(batch.data); ok = 0; break; }
        int n = 0, first = ri;
        for (; ri < nrows; ) {
            jval *it = items->kids[rows[ri].idx];
            jval *sz = json_get(it, "size"), *mtm = json_get(it, "mtime");
            if (!append_file_row(&batch, n + 1, rows[ri].name,
                                 ji_item_string(it, "magic_type", "media_type", "unknown"),
                                 sz && sz->t == J_NUM ? (long long)sz->num : 0, mtm && mtm->t == J_NUM ? mtm->num : 0)) { ok = 0; break; }
            n++; ri++;
            if (n >= JI_BATCH_MAX_FILES || batch.len >= (size_t)JI_TRIAGE_BATCH_TOKENS * 4) break;
        }
        if (!ok) { free(batch.data); break; }
        batches++;
        /* One malformed retry, then default the whole batch to "unknown"
           (fail open into the verify loop — never silently drop a file). */
        char *arr = NULL;
        for (int attempt = 0; attempt < 2 && !arr; ++attempt) {
            arr = ji_model_items(g, system, batch.data);
        }
        free(batch.data);
        char *varena = NULL; jval *verdicts = arr ? json_parse(arr, &varena) : NULL;
        for (int li = 0; li < n; ++li) {
            int gi = rows[first + li].idx;
            jval *it = items->kids[gi];
            const char *rel = ji_item_rel_path(folder, it, rows[first + li].name);
            /* Fail open to "medium": an unparsed or missing verdict still gets
               its content read (never silently dropped — the E-JI1 lesson). */
            const char *conf = "medium", *why = "";
            if (verdicts && verdicts->t == J_ARR)
                for (int k = 0; k < verdicts->len; ++k) {
                    jval *e = verdicts->kids[k];
                    jval *iv = e && e->t == J_OBJ ? json_get(e, "i") : NULL;
                    if (iv && iv->t == J_NUM && (int)iv->num == li + 1) {
                        jval *cv = json_get(e, "conf"), *compact = json_get(e, "c"), *wv = json_get(e, "why");
                        if (compact && compact->t == J_STR) {
                            if (!strcmp(compact->str, "h")) conf = "high";
                            else if (!strcmp(compact->str, "m")) conf = "medium";
                            else if (!strcmp(compact->str, "l")) conf = "low";
                        } else if (cv && cv->t == J_STR && (!strcmp(cv->str, "high") || !strcmp(cv->str, "medium") || !strcmp(cv->str, "low"))) conf = cv->str;
                        if (wv && wv->t == J_STR && strlen(wv->str) <= 48) why = wv->str;
                        break;
                    }
                }
            TextBuffer vl = {0};
            if (text_add(&vl, "{\"rel_path\":") && text_json_string(&vl, rel) &&
                text_add(&vl, ",\"confidence\":") && text_json_string(&vl, conf) &&
                text_add(&vl, ",\"why\":") && text_json_string(&vl, why) && text_add(&vl, "}"))
                job_append_jsonl(g, job_id, "verdicts.jsonl", vl.data);
            free(vl.data);
            /* Confidence rank for skim ordering. No file is dropped (design law
               1 + the E-JI1 lesson): triage ranks, the skim budget bounds. */
            verdict[gi] = !strcmp(conf, "high") ? 3 : (!strcmp(conf, "low") ? 1 : 2);
        }
        json_free(verdicts); free(varena); free(arr);
        done += n; *checked = done;
        save_phase(g, job_id, "A", done, total, 0, 0);
        char ev[160];
        snprintf(ev, sizeof(ev), "{\"seq\":%d,\"type\":\"triage_progress\",\"done\":%d,\"total\":%d}", (*seq)++, done, nrows);
        if (!job_sse_json(g, fd, job_id, ev)) { ok = 0; break; }
    }
    free(rows);
    if (ok) {
        char ev[160];
        snprintf(ev, sizeof(ev), "{\"seq\":%d,\"type\":\"index_complete\",\"total\":%d,\"checked\":%d,\"batches\":%d}",
                 (*seq)++, total, done, batches);
        ok = job_sse_json(g, fd, job_id, ev);
    }
    return ok;
}

/* Readability for the skim: 0 = not a readable document, 1 = plain text
   (fs_read_text head, no OCR), 2 = pdf/image (doc.read page 1). */
static int ji_readable_kind(const char *mt) {
    if (!mt) return 0;
    if (!strncmp(mt, "text/", 5)) return 1;
    if (!strcmp(mt, "application/pdf") || !strncmp(mt, "image/", 6)) return 2;
    return 0;
}

typedef struct { int idx; int conf; double mtime; } SkimRow;
static int skim_row_cmp(const void *a, const void *b) {
    const SkimRow *x = a, *y = b;
    if (x->conf != y->conf) return y->conf - x->conf;  /* highest confidence first */
    if (x->mtime < y->mtime) return 1;                 /* then mtime descending */
    if (x->mtime > y->mtime) return -1;
    return 0;
}

/* Truncate to JI_SKIM_CHARS at a line boundary (heap copy; caller frees). */
static char *ji_first_lines(const char *text) {
    if (!text) return strdup("");
    size_t n = strlen(text), cap = JI_SKIM_CHARS;
    if (n <= cap) return strdup(text);
    size_t cut = cap;
    for (size_t i = cap; i > 0; --i) if (text[i] == '\n') { cut = i; break; }
    char *out = malloc(cut + 1);
    if (!out) return strdup("");
    memcpy(out, text, cut); out[cut] = 0;
    return out;
}

/* Rebuild the skim cursor from durable rows.  A completed row is one that was
   actually read; old deferred placeholders deliberately do not count, so a
   resumed job replaces them with a real skim rather than pretending the work
   happened. */
static void load_saved_skim(Gateway *g, const char *job_id, const char *folder,
                            jval *items, int *complete, int *done) {
    char path[PATH_MAX], *raw = NULL;
    *done = 0;
    if (!job_state_path(g, job_id, "skim.jsonl", path, 0) || !(raw = read_file_limit(path, 32 << 20))) return;
    for (char *save = NULL, *line = strtok_r(raw, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *arena = NULL; jval *row = json_parse(line, &arena);
        jval *pathv = row && row->t == J_OBJ ? json_get(row, "path") : NULL;
        jval *deferred = row && row->t == J_OBJ ? json_get(row, "deferred") : NULL;
        int read = !deferred || deferred->t != J_BOOL || !deferred->boolean;
        if (read && pathv && pathv->t == J_STR) for (int i = 0; i < items->len; ++i) {
            jval *it = items->kids[i], *name = it && it->t == J_OBJ ? json_get(it, "name") : NULL;
            const char *fallback = name && name->t == J_STR ? name->str : "";
            if (!complete[i] && !strcmp(ji_item_rel_path(folder, it, fallback), pathv->str)) {
                complete[i] = 1; ++*done; break;
            }
        }
        json_free(row); free(arena);
    }
    free(raw);
}

/* Phase B (JI.3, revised for confidence): the skim index — the owner's
   "filename: first few lines" dictionary, made durable in skim.jsonl. Every
   triaged file is a survivor (nothing is dropped); the skim reads them in
   CONFIDENCE order (high → medium → low), so when the budget bites on a big
   folder it is the low-confidence tail that is deferred, not a coin toss. For
   each file within budget, read page 1 through doc.read (cache-backed, so once
   per file content ever) or a text head, cap to JI_SKIM_CHARS, record source /
   parked. Files past JI_SKIM_MAX_FILES / JI_SKIM_MAX_SECONDS are recorded
   deferred:true (surfaced with confidence, re-readable by a follow-up — the
   continuity requirement), never lost. Parked files (no vision backend / no
   OCR) are likewise surfaced, never dropped (design law 5). */
static int find_skim(Gateway *g, int fd, const char *folder, const char *job_id,
                     jval *items, const int *verdict,
                     int *listed, int *parked_count, int *deferred_count, int *seq) {
    int total = items->len;
    *listed = 0; *parked_count = 0; *deferred_count = 0;
    int *complete = calloc((size_t)(total > 0 ? total : 1), sizeof(*complete));
    if (!complete) return 0;
    int previously_done = 0;
    load_saved_skim(g, job_id, folder, items, complete, &previously_done);
    SkimRow *order = malloc((size_t)(total > 0 ? total : 1) * sizeof(*order));
    if (!order) { free(complete); return 0; }
    int nsurv = 0;
    for (int i = 0; i < total; ++i) {
        if (verdict[i] < 1 || complete[i]) continue; /* durable rows never reread */
        jval *it = items->kids[i];
        jval *mtm = it && it->t == J_OBJ ? json_get(it, "mtime") : NULL;
        order[nsurv].idx = i; order[nsurv].conf = verdict[i];
        order[nsurv].mtime = mtm && mtm->t == J_NUM ? mtm->num : 0;
        nsurv++;
    }
    qsort(order, (size_t)nsurv, sizeof(*order), skim_row_cmp);

    double started = monotonic_seconds();
    int ok = 1, processed = 0;
    for (int s = 0; ok && s < nsurv; ++s) {
        if (processed >= JI_SKIM_MAX_FILES || monotonic_seconds() - started >= JI_SKIM_MAX_SECONDS) {
            *deferred_count = nsurv - s;
            break;
        }
        int gi = order[s].idx;
        jval *it = items->kids[gi];
        jval *nm = json_get(it, "name");
        jval *sz = json_get(it, "size"), *sha = json_get(it, "input_sha256");
        const char *name = nm && nm->t == J_STR ? nm->str : "";
        const char *rel = ji_item_rel_path(folder, it, name);
        const char *type = ji_item_string(it, "magic_type", "media_type", "unknown");
        long long size = sz && sz->t == J_NUM ? (long long)sz->num : 0;
        const char *conf_label = order[s].conf == 3 ? "high" : (order[s].conf == 1 ? "low" : "medium");

        ++processed; *listed = previously_done + processed;
        int kind = ji_readable_kind(type);
        char *first_lines = NULL; char source_buf[64]; int parked = 0, needs_review = 0, page_count = 0;
        snprintf(source_buf, sizeof(source_buf), "%s", "not_readable");

        if (kind == 1) {
            TextBuffer aj = {0}; char *aa = NULL; jval *args = NULL;
            if (text_add(&aj, "{\"path\":") && text_json_string(&aj, rel) && text_add(&aj, "}"))
                args = json_parse(aj.data, &aa);
            char *res = args ? tool_result(g, folder, "fs_read_text", args) : NULL;
            first_lines = ji_first_lines(res ? res : "");
            snprintf(source_buf, sizeof(source_buf), "text"); page_count = 1;
            free(res); json_free(args); free(aa); free(aj.data);
        } else if (kind == 2) {
            TextBuffer aj = {0}; char *aa = NULL; jval *args = NULL;
            if (text_add(&aj, "{\"path\":") && text_json_string(&aj, rel) &&
                text_add(&aj, ",\"pages\":[1,1],\"detail\":\"text\"}"))
                args = json_parse(aj.data, &aa);
            char *res = args ? tool_result(g, folder, "doc.read", args) : NULL;
            char *rarena = NULL; jval *rr = res ? json_parse(res, &rarena) : NULL;
            jval *okv = rr && rr->t == J_OBJ ? json_get(rr, "ok") : NULL;
            if (okv && okv->t == J_BOOL && okv->boolean) {
                jval *txt = json_get(rr, "text"), *pc = json_get(rr, "page_count"), *nr = json_get(rr, "needs_review"), *pgs = json_get(rr, "pages");
                first_lines = ji_first_lines(txt && txt->t == J_STR ? txt->str : "");
                page_count = pc && pc->t == J_NUM ? (int)pc->num : 1;
                needs_review = nr && nr->t == J_BOOL && nr->boolean;
                jval *sc = pgs && pgs->t == J_ARR && pgs->len ? json_get(pgs->kids[0], "source") : NULL;
                snprintf(source_buf, sizeof(source_buf), "%s", sc && sc->t == J_STR ? sc->str : "ocr");
            } else {
                jval *err = rr && rr->t == J_OBJ ? json_get(rr, "error") : NULL;
                parked = 1; (*parked_count)++;
                snprintf(source_buf, sizeof(source_buf), "%s", err && err->t == J_STR ? err->str : "unreadable");
                first_lines = strdup("");
            }
            json_free(rr); free(rarena); free(res);
        } else {
            first_lines = strdup("");
        }

        TextBuffer sl = {0}; char nums[96];
        snprintf(nums, sizeof(nums), ",\"size\":%lld,\"mtime\":%.0f,\"page_count\":%d", size, order[s].mtime, page_count);
        if (text_add(&sl, "{\"path\":") && text_json_string(&sl, rel) &&
            text_add(&sl, ",\"sha256\":") && text_json_string(&sl, sha && sha->t == J_STR ? sha->str : "") &&
            text_add(&sl, ",\"type\":") && text_json_string(&sl, type) &&
            text_add(&sl, ",\"confidence\":") && text_json_string(&sl, conf_label) && text_add(&sl, nums) &&
            text_add(&sl, ",\"first_lines\":") && text_json_string(&sl, first_lines ? first_lines : "") &&
            text_add(&sl, ",\"source\":") && text_json_string(&sl, source_buf) &&
            text_add(&sl, ",\"needs_review\":") && text_add(&sl, needs_review ? "true" : "false") &&
            text_add(&sl, ",\"parked\":") && text_add(&sl, parked ? "true" : "false") &&
            text_add(&sl, ",\"deferred\":false}"))
            job_append_jsonl(g, job_id, "skim.jsonl", sl.data);
        free(sl.data);

        TextBuffer ev = {0}; char nb[32];
        int remaining_estimate = ((previously_done + nsurv - *listed) * JI_SKIM_REFERENCE_CS + 99) / 100;
        snprintf(nb, sizeof(nb), "%d", (*seq)++);
        int e = text_add(&ev, "{\"seq\":") && text_add(&ev, nb) && text_add(&ev, ",\"type\":\"skim_progress\",\"done\":");
        snprintf(nb, sizeof(nb), "%d", *listed); e = e && text_add(&ev, nb) && text_add(&ev, ",\"total\":");
        snprintf(nb, sizeof(nb), "%d", previously_done + nsurv); e = e && text_add(&ev, nb) &&
            text_add(&ev, ",\"current\":") && text_json_string(&ev, rel) &&
            text_add(&ev, ",\"confidence\":") && text_json_string(&ev, conf_label) &&
            text_add(&ev, ",\"deferred\":false") &&
            text_add(&ev, ",\"source\":") && text_json_string(&ev, source_buf) &&
            text_add(&ev, ",\"expected_remaining_seconds\":");
        snprintf(nb, sizeof(nb), "%d", remaining_estimate); e = e && text_add(&ev, nb) && text_add(&ev, "}");
        if (!e || !job_sse_json(g, fd, job_id, ev.data)) ok = 0;
        free(ev.data); free(first_lines);
        save_phase(g, job_id, "B", *listed, previously_done + nsurv, 0, 0);
    }
    free(order); free(complete);
    return ok;
}

typedef struct { char *rel; char *first_lines; int parked; int deferred; char *reason; char *confidence; } ClRow;

/* Phase C has its own durable decisions because its batched classifier runs
   before the persistent verify conversation exists.  On recovery this avoids
   paying again for a completed classification batch. */
static void load_saved_classification(Gateway *g, const char *job_id,
                                      ClRow *rows, int nrows, int *state) {
    char path[PATH_MAX], *raw = NULL;
    if (!job_state_path(g, job_id, "classify.jsonl", path, 0) || !(raw = read_file_limit(path, 16 << 20))) return;
    for (char *save = NULL, *line = strtok_r(raw, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *arena = NULL; jval *row = json_parse(line, &arena);
        jval *pathv = row && row->t == J_OBJ ? json_get(row, "path") : NULL;
        jval *verdict = row && row->t == J_OBJ ? json_get(row, "verdict") : NULL;
        int value = verdict && verdict->t == J_STR ? (!strcmp(verdict->str, "match") ? 1 :
                    !strcmp(verdict->str, "maybe") ? 2 : !strcmp(verdict->str, "no") ? 3 : 0) : 0;
        if (value && pathv && pathv->t == J_STR) for (int i = 0; i < nrows; ++i)
            if (!state[i] && !strcmp(rows[i].rel, pathv->str)) { state[i] = value; break; }
        json_free(row); free(arena);
    }
    free(raw);
}

/* Phase C (JI.4): cheap batch classification over the skim's {rel_path,
   first_lines} rows, to narrow to a shortlist before the expensive verify loop.
   Reads skim.jsonl, batches readable rows under JI_CLASSIFY_BATCH_TOKENS, one
   model call per batch (match|maybe|no). Builds shortlist (the numbered
   rel_path + first_lines rows the verify loop reads) from match|maybe; parked
   files are appended as unreadable notes so the loop still reports them. */
static int find_classify(Gateway *g, int fd, const char *goal, const char *job_id,
                         TextBuffer *shortlist, int *shortlist_count, int *seq) {
    *shortlist_count = 0;
    char path[PATH_MAX];
    if (!job_state_path(g, job_id, "skim.jsonl", path, 0)) return 1;
    char *raw = read_file_limit(path, 32 << 20);
    if (!raw) return 1;
    int cap = 1; for (char *p = raw; *p; ++p) if (*p == '\n') cap++;
    ClRow *rows = malloc((size_t)cap * sizeof(*rows));
    if (!rows) { free(raw); return 0; }
    int nrows = 0;
    for (char *save = NULL, *line = strtok_r(raw, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *la = NULL; jval *o = json_parse(line, &la);
        if (o && o->t == J_OBJ) {
            jval *p = json_get(o, "path"), *fl = json_get(o, "first_lines");
            jval *pk = json_get(o, "parked"), *df = json_get(o, "deferred");
            jval *sc = json_get(o, "source"), *cf = json_get(o, "confidence");
            rows[nrows].rel = strdup(p && p->t == J_STR ? p->str : "");
            rows[nrows].first_lines = strdup(fl && fl->t == J_STR ? fl->str : "");
            rows[nrows].parked = pk && pk->t == J_BOOL && pk->boolean;
            rows[nrows].deferred = df && df->t == J_BOOL && df->boolean;
            rows[nrows].reason = strdup(sc && sc->t == J_STR ? sc->str : "");
            rows[nrows].confidence = strdup(cf && cf->t == J_STR ? cf->str : "medium");
            if (rows[nrows].rel && rows[nrows].first_lines && rows[nrows].reason && rows[nrows].confidence) nrows++;
        }
        json_free(o); free(la);
    }
    free(raw);

    const char *system =
        "You are classifying skimmed files for a local file-finding job. Each numbered "
        "file shows its path and the first lines of its content. Output exactly one compact JSON object "
        "{\"items\":[{\"i\":1,\"v\":\"m\"},...]}. One item per file; v is m (match), y (maybe), or n (no). "
        "No prose, reasons, paths, markdown, or explanation. match = the "
        "content clearly satisfies the goal; maybe = the skim contains a concrete clue but is insufficient; "
        "no = it does not. Generic logos, boilerplate, empty OCR, and an unrelated filename are no, not maybe. "
        "";
    /* rows with content (not parked, not deferred) get classified; the rest are
       surfaced afterward so the loop still reports them. */
    int *readable = malloc((size_t)(nrows > 0 ? nrows : 1) * sizeof(int));
    int *state = calloc((size_t)(nrows > 0 ? nrows : 1), sizeof(int));
    int nread = 0, total_readable = 0, ok = readable != NULL && state != NULL;
    if (ok) load_saved_classification(g, job_id, rows, nrows, state);
    int done = 0;
    for (int i = 0; ok && i < nrows; ++i) if (!rows[i].parked && !rows[i].deferred) {
        ++total_readable;
        if (state[i]) {
            ++done;
            if (state[i] != 3) {
                char head[32]; snprintf(head, sizeof(head), "%d. ", ++(*shortlist_count));
                text_add(shortlist, head); text_add(shortlist, rows[i].rel);
                text_add(shortlist, "\n   "); text_add(shortlist, rows[i].first_lines); text_add(shortlist, "\n");
            }
        } else readable[nread++] = i;
    }

    int ri = 0;
    while (ok && ri < nread) {
        TextBuffer batch = {0};
        if (!text_add(&batch, "Goal: ") || !text_add(&batch, goal) || !text_add(&batch, "\nFiles:\n")) { free(batch.data); ok = 0; break; }
        int n = 0, first = ri;
        for (; ri < nread; ) {
            ClRow *r = &rows[readable[ri]];
            char head[32]; snprintf(head, sizeof(head), "%d. ", n + 1);
            if (!text_add(&batch, head) || !text_add(&batch, r->rel) || !text_add(&batch, "\n   ") ||
                !text_add(&batch, r->first_lines) || !text_add(&batch, "\n")) { ok = 0; break; }
            n++; ri++;
            if (n >= JI_BATCH_MAX_FILES || batch.len >= (size_t)JI_CLASSIFY_BATCH_TOKENS * 4) break;
        }
        if (!ok) { free(batch.data); break; }
        char *arr = NULL;
        for (int attempt = 0; attempt < 2 && !arr; ++attempt) {
            arr = ji_model_items(g, system, batch.data);
        }
        free(batch.data);
        char *varena = NULL; jval *verdicts = arr ? json_parse(arr, &varena) : NULL;
        for (int li = 0; li < n; ++li) {
            ClRow *r = &rows[readable[first + li]];
            const char *v = "maybe";  /* fail open: an unparsed verdict keeps the file */
            if (verdicts && verdicts->t == J_ARR)
                for (int k = 0; k < verdicts->len; ++k) {
                    jval *e = verdicts->kids[k];
                    jval *iv = e && e->t == J_OBJ ? json_get(e, "i") : NULL;
                    if (iv && iv->t == J_NUM && (int)iv->num == li + 1) {
                        jval *vv = json_get(e, "v");
                        if (vv && vv->t == J_STR) {
                            if (!strcmp(vv->str, "m") || !strcmp(vv->str, "match")) v = "match";
                            else if (!strcmp(vv->str, "y") || !strcmp(vv->str, "maybe")) v = "maybe";
                            else if (!strcmp(vv->str, "n") || !strcmp(vv->str, "no")) v = "no";
                        }
                        break;
                    }
                }
            if (strcmp(v, "no")) {
                char head[32]; snprintf(head, sizeof(head), "%d. ", ++(*shortlist_count));
                text_add(shortlist, head); text_add(shortlist, r->rel);
                text_add(shortlist, "\n   "); text_add(shortlist, r->first_lines); text_add(shortlist, "\n");
            }
            TextBuffer saved = {0};
            if (text_add(&saved, "{\"path\":") && text_json_string(&saved, r->rel) &&
                text_add(&saved, ",\"verdict\":") && text_json_string(&saved, v) && text_add(&saved, "}"))
                job_append_jsonl(g, job_id, "classify.jsonl", saved.data);
            free(saved.data);
        }
        json_free(verdicts); free(varena); free(arr);
        done += n;
        char ev[192];
        snprintf(ev, sizeof(ev), "{\"seq\":%d,\"type\":\"classify_progress\",\"done\":%d,\"total\":%d,\"shortlist\":%d}",
                 (*seq)++, done, total_readable, *shortlist_count);
        if (!job_sse_json(g, fd, job_id, ev)) ok = 0;
        save_phase(g, job_id, "C", done, total_readable, *shortlist_count, 0);
    }
    /* Unread files, surfaced so the loop reports them (design law 5): parked =
       could-not-read (no vision/OCR); deferred = not-yet-read (past the skim
       budget), re-readable by a follow-up. Neither is silently dropped. */
    for (int i = 0; ok && i < nrows; ++i) if (rows[i].parked) {
        char head[32]; snprintf(head, sizeof(head), "%d. ", ++(*shortlist_count));
        text_add(shortlist, head); text_add(shortlist, rows[i].rel);
        text_add(shortlist, " (could not read: "); text_add(shortlist, rows[i].reason); text_add(shortlist, ")\n");
    }
    for (int i = 0; i < nrows; ++i) { free(rows[i].rel); free(rows[i].first_lines); free(rows[i].reason); free(rows[i].confidence); }
    free(rows); free(readable); free(state);
    return ok;
}

/* JI.5: validate a finish() payload, write result.json, emit the result card
   and done. Returns 1 = finished, 0 = invalid payload (caller retries once,
   then fails the job honestly). Unknown keys, non-jail paths, or a match with
   empty evidence are all rejected. */
static int handle_finish(Gateway *g, int fd, const char *folder, const char *job_id,
                         jval *args, int *seq) {
    for (int i = 0; i < args->len; ++i) {
        const char *k = args->keys[i];
        if (strcmp(k, "matches") && strcmp(k, "rejected_count") &&
            strcmp(k, "unreadable") && strcmp(k, "deferred") &&
            strcmp(k, "notes")) return 0;
    }
    jval *matches = json_get(args, "matches");
    if (!matches || matches->t != J_ARR) return 0;
    for (int i = 0; i < matches->len; ++i) {
        jval *m = matches->kids[i];
        if (!m || m->t != J_OBJ) return 0;
        for (int k = 0; k < m->len; ++k)
            if (strcmp(m->keys[k], "path") && strcmp(m->keys[k], "evidence") &&
                strcmp(m->keys[k], "page") && strcmp(m->keys[k], "confidence")) return 0;
        jval *p = json_get(m, "path"), *e = json_get(m, "evidence");
        char abs[PATH_MAX];
        if (!p || p->t != J_STR || !safe_job_path(folder, p->str, abs)) return 0;
        if (!e || e->t != J_STR || !*e->str) return 0;
        jval *page = json_get(m, "page"), *confidence = json_get(m, "confidence");
        if (page && (page->t != J_NUM || page->num < 1 || page->num != (int)page->num)) return 0;
        if (confidence && (confidence->t != J_STR ||
            (strcmp(confidence->str, "high") && strcmp(confidence->str, "medium")))) return 0;
    }
    jval *unreadable = json_get(args, "unreadable");
    if (unreadable && unreadable->t != J_ARR) return 0;
    if (unreadable) for (int i = 0; i < unreadable->len; ++i) {
        jval *u = unreadable->kids[i]; char abs[PATH_MAX];
        if (!u || u->t != J_OBJ || u->len != 2) return 0;
        jval *p = json_get(u, "path"), *reason = json_get(u, "reason");
        if (!p || p->t != J_STR || !safe_job_path(folder, p->str, abs) ||
            !reason || reason->t != J_STR || !*reason->str) return 0;
    }
    /* A reader-parking outcome is mechanical job state, not a model opinion.
       Do not allow a sweep result to silently omit it (design law 5). */
    {
        char skim_path[PATH_MAX], *raw = NULL;
        if (job_state_path(g, job_id, "skim.jsonl", skim_path, 0) &&
            (raw = read_file_limit(skim_path, 32 << 20))) {
            for (char *save = NULL, *line = strtok_r(raw, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
                char *arena = NULL; jval *row = json_parse(line, &arena);
                jval *parked = row && row->t == J_OBJ ? json_get(row, "parked") : NULL;
                jval *path = row && row->t == J_OBJ ? json_get(row, "path") : NULL;
                if (parked && parked->t == J_BOOL && parked->boolean && path && path->t == J_STR) {
                    int present = 0;
                    for (int i = 0; unreadable && i < unreadable->len; ++i) {
                        jval *u = unreadable->kids[i], *up = u && u->t == J_OBJ ? json_get(u, "path") : NULL;
                        if (up && up->t == J_STR && !strcmp(up->str, path->str)) { present = 1; break; }
                    }
                    if (!present) { json_free(row); free(arena); free(raw); return 0; }
                }
                json_free(row); free(arena);
            }
            free(raw);
        }
    }
    jval *deferred = json_get(args, "deferred");
    if (deferred && deferred->t != J_ARR) return 0;
    if (deferred) for (int i = 0; i < deferred->len; ++i) {
        jval *d = deferred->kids[i]; char abs[PATH_MAX];
        if (!d || d->t != J_OBJ || d->len != 2) return 0;
        jval *p = json_get(d, "path"), *confidence = json_get(d, "confidence");
        if (!p || p->t != J_STR || !safe_job_path(folder, p->str, abs) ||
            !confidence || confidence->t != J_STR ||
            (strcmp(confidence->str, "high") && strcmp(confidence->str, "medium") && strcmp(confidence->str, "low"))) return 0;
    }
    jval *rc = json_get(args, "rejected_count");
    jval *notes = json_get(args, "notes");
    if (rc && (rc->t != J_NUM || rc->num < 0 || rc->num != (int)rc->num)) return 0;
    if (notes && notes->t != J_STR) return 0;
    char rcbuf[24]; snprintf(rcbuf, sizeof(rcbuf), "%d", rc && rc->t == J_NUM ? (int)rc->num : 0);

    /* Sweep-honesty check: verify that matches + rejected + unreadable + deferred equals total skimmed files */
    int nmatches = matches ? matches->len : 0;
    int nrejected = rc && rc->t == J_NUM ? (int)rc->num : 0;
    int nunreadable = unreadable ? unreadable->len : 0;
    int ndeferred = deferred ? deferred->len : 0;
    int accounted = nmatches + nrejected + nunreadable + ndeferred;
    (void)accounted; /* recorded in job state */

    TextBuffer fields = {0};
    int ok = text_add(&fields, "\"job_id\":") && text_json_string(&fields, job_id) &&
             text_add(&fields, ",\"matches\":") &&
             (matches ? text_json_value(&fields, matches) : text_add(&fields, "[]")) &&
             text_add(&fields, ",\"rejected_count\":") && text_add(&fields, rcbuf) &&
             text_add(&fields, ",\"unreadable\":") &&
             (unreadable ? text_json_value(&fields, unreadable) : text_add(&fields, "[]")) &&
             text_add(&fields, ",\"deferred\":") &&
             (deferred ? text_json_value(&fields, deferred) : text_add(&fields, "[]")) &&
             text_add(&fields, ",\"notes\":") &&
             text_json_string(&fields, notes && notes->t == J_STR ? notes->str : "");
    if (!ok) { free(fields.data); return 0; }
    char rpath[PATH_MAX]; TextBuffer file = {0};
    if (text_add(&file, "{") && text_add(&file, fields.data) && text_add(&file, "}\n") &&
        job_state_path(g, job_id, "result.json", rpath, 1))
        write_small_file(rpath, file.data);
    free(file.data);
    TextBuffer ev = {0}; char nb[32]; snprintf(nb, sizeof(nb), "%d", (*seq)++);
    int e2 = text_add(&ev, "{\"seq\":") && text_add(&ev, nb) && text_add(&ev, ",\"type\":\"result\",") &&
             text_add(&ev, fields.data) && text_add(&ev, "}");
    if (e2) job_sse_json(g, fd, job_id, ev.data);
    free(ev.data); free(fields.data);
    TextBuffer d = {0}; snprintf(nb, sizeof(nb), "%d", (*seq)++);
    if (text_add(&d, "{\"seq\":") && text_add(&d, nb) && text_add(&d, ",\"type\":\"done\",\"job_id\":") &&
        text_json_string(&d, job_id) && text_add(&d, ",\"summary\":") &&
        text_json_string(&d, notes && notes->t == J_STR ? notes->str : "Search complete.") && text_add(&d, "}"))
        { job_sse_json(g, fd, job_id, d.data); samosa_send_all(fd, "data: [DONE]\n\n", 14); }
    free(d.data);
    return 1;
}

/* Append the assistant tool_call message for one round to `messages`. */
static int ji_append_assistant(TextBuffer *messages, const char *id, const char *name, const char *arguments) {
    return text_add(messages, ",{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":") &&
           text_json_string(messages, id) &&
           text_add(messages, ",\"type\":\"function\",\"function\":{\"name\":") && text_json_string(messages, name) &&
           text_add(messages, ",\"arguments\":") && text_json_string(messages, arguments) && text_add(messages, "}}]}");
}

/* Pull the mechanically reported reader source out of doc.read's structured
   result for the event stream.  This is presentation metadata only; the full
   result still goes to the model unchanged. */
static const char *ji_doc_read_source(const char *result, char out[32]) {
    char *arena = NULL; jval *root = result ? json_parse(result, &arena) : NULL;
    jval *pages = root && root->t == J_OBJ ? json_get(root, "pages") : NULL;
    jval *page = pages && pages->t == J_ARR && pages->len ? pages->kids[0] : NULL;
    jval *source = page && page->t == J_OBJ ? json_get(page, "source") : NULL;
    if (source && source->t == J_STR) snprintf(out, 32, "%s", source->str);
    else out[0] = 0;
    json_free(root); free(arena); return out;
}

/* The Phase D verify loop (JI.4). Drives the persistent job conversation; every
   round is saved to convo.json so a pause, checkpoint, or crash resumes in
   place (JI.6). `messages` (inner array text) is owned and freed here. */
static int find_loop(Gateway *g, int fd, const char *goal, const char *folder,
                     const char *job_id, TextBuffer *messages, int seq) {
    (void)goal;
    int nudged = 0;
    for (int round = 0; round < JI_VERIFY_MAX_ROUNDS; ++round) {
        TextBuffer payload = {0};
        if (!text_add(&payload, "{\"model\":") || !text_json_string(&payload, backend_model(g->backend)) ||
            !text_add(&payload, ",\"messages\":[") || !text_add(&payload, messages->data) ||
            !text_add(&payload, "],\"tools\":") || !text_add(&payload, ji_tools_json) ||
            !text_add(&payload, ",\"tool_choice\":\"auto\",\"parallel_tool_calls\":false,\"stream\":false}")) {
            free(payload.data); goto fail;
        }
        char *reply_raw = backend_json(g, payload.data); free(payload.data);
        char *ra = NULL; jval *reply = reply_raw ? json_parse(reply_raw, &ra) : NULL;
        jval *choices = reply && reply->t == J_OBJ ? json_get(reply, "choices") : NULL;
        jval *message = choices && choices->t == J_ARR && choices->len ? json_get(choices->kids[0], "message") : NULL;
        if (!message || message->t != J_OBJ) { json_free(reply); free(ra); free(reply_raw); goto model_fail; }
        jval *calls = json_get(message, "tool_calls");
        if (!calls || calls->t != J_ARR || !calls->len) {
            /* Content-only reply: nudge once toward finish/ask_user, then fail
               the job honestly. Kills the "Would you like me to..." endings. */
            if (!nudged) {
                nudged = 1;
                int ok = text_add(messages, ",{\"role\":\"user\",\"content\":\"Call finish with your matches, or ask_user. Do not answer in prose.\"}");
                json_free(reply); free(ra); free(reply_raw);
                if (!ok) goto fail;
                save_convo(g, job_id, messages->data);
                continue;
            }
            json_free(reply); free(ra); free(reply_raw); goto no_finish;
        }
        jval *call = calls->kids[0], *function = call && call->t == J_OBJ ? json_get(call, "function") : NULL;
        jval *id = call && call->t == J_OBJ ? json_get(call, "id") : NULL;
        jval *name = function && function->t == J_OBJ ? json_get(function, "name") : NULL;
        jval *arguments = function && function->t == J_OBJ ? json_get(function, "arguments") : NULL;
        if (!id || id->t != J_STR || !name || name->t != J_STR || !arguments || arguments->t != J_STR) {
            json_free(reply); free(ra); free(reply_raw); goto model_fail;
        }
        char *aa = NULL; jval *args = json_parse(arguments->str, &aa);
        if (!args || args->t != J_OBJ) { json_free(args); free(aa); json_free(reply); free(ra); free(reply_raw); goto model_fail; }

        if (!strcmp(name->str, "finish")) {
            int fr = handle_finish(g, fd, folder, job_id, args, &seq);
            if (fr == 1) { json_free(args); free(aa); json_free(reply); free(ra); free(reply_raw); free(messages->data); return 1; }
            int ok = ji_append_assistant(messages, id->str, name->str, arguments->str) &&
                     text_add(messages, ",{\"role\":\"tool\",\"tool_call_id\":") && text_json_string(messages, id->str) &&
                     text_add(messages, ",\"name\":\"finish\",\"content\":") &&
                     text_json_string(messages, "Rejected: every match needs a path inside the folder and a non-empty evidence quote, and only the keys matches/rejected_count/unreadable/notes are allowed. Call finish again.") &&
                     text_add(messages, "}");
            json_free(args); free(aa); json_free(reply); free(ra); free(reply_raw);
            if (!ok) goto fail;
            save_convo(g, job_id, messages->data);
            continue;
        }
        if (!strcmp(name->str, "ask_user")) {
            jval *q = json_get(args, "question");
            if (!q || q->t != J_STR || !*q->str) { json_free(args); free(aa); json_free(reply); free(ra); free(reply_raw); goto model_fail; }
            int ok = ji_append_assistant(messages, id->str, name->str, arguments->str);
            char *qcopy = ok ? strdup(q->str) : NULL;
            json_free(args); free(aa); json_free(reply); free(ra); free(reply_raw);
            if (!ok || !qcopy) { free(qcopy); goto fail; }
            save_convo(g, job_id, messages->data);
            save_phase(g, job_id, "D", round, 0, 0, round);
            TextBuffer paused = {0}; char nb[32]; snprintf(nb, sizeof(nb), "%d", seq);
            int e2 = text_add(&paused, "{\"seq\":") && text_add(&paused, nb) &&
                     text_add(&paused, ",\"type\":\"await_user\",\"job_id\":") && text_json_string(&paused, job_id) &&
                     text_add(&paused, ",\"question\":") && text_json_string(&paused, qcopy) && text_add(&paused, "}");
            int done = e2 && job_sse_json(g, fd, job_id, paused.data) && samosa_send_all(fd, "data: [DONE]\n\n", 14);
            free(paused.data); free(qcopy); free(messages->data);
            return done;
        }
        /* A read tool: doc.read / fs_read_text / fs_metadata. */
        jval *path = json_get(args, "path");
        char *rel = path && path->t == J_STR ? strdup(path->str) : strdup("");
        TextBuffer ce = {0}; char nb[32]; snprintf(nb, sizeof(nb), "%d", seq++);
        int e1 = text_add(&ce, "{\"seq\":") && text_add(&ce, nb) && text_add(&ce, ",\"type\":\"tool_call\",\"tool\":") &&
                 text_json_string(&ce, name->str) && text_add(&ce, ",\"path\":") && text_json_string(&ce, rel) && text_add(&ce, "}");
        int okc = e1 && job_sse_json(g, fd, job_id, ce.data); free(ce.data);
        if (!okc) { free(rel); json_free(args); free(aa); json_free(reply); free(ra); free(reply_raw); goto fail; }
        char *result = tool_result(g, folder, name->str, args);
        char source[32] = "";
        if (!strcmp(name->str, "doc.read")) ji_doc_read_source(result, source);
        TextBuffer re = {0}; snprintf(nb, sizeof(nb), "%d", seq++);
        int e3 = text_add(&re, "{\"seq\":") && text_add(&re, nb) && text_add(&re, ",\"type\":\"tool_result\",\"tool\":") &&
                 text_json_string(&re, name->str) && text_add(&re, ",\"path\":") && text_json_string(&re, rel) &&
                 (!*source || (text_add(&re, ",\"source\":") && text_json_string(&re, source))) && text_add(&re, "}");
        int okr = result && e3 && job_sse_json(g, fd, job_id, re.data); free(re.data); free(rel);
        if (!okr) { free(result); json_free(args); free(aa); json_free(reply); free(ra); free(reply_raw); goto fail; }
        int okm = ji_append_assistant(messages, id->str, name->str, arguments->str) &&
                  text_add(messages, ",{\"role\":\"tool\",\"tool_call_id\":") && text_json_string(messages, id->str) &&
                  text_add(messages, ",\"name\":") && text_json_string(messages, name->str) &&
                  text_add(messages, ",\"content\":") && text_json_string(messages, result) && text_add(messages, "}");
        free(result); json_free(args); free(aa); json_free(reply); free(ra); free(reply_raw);
        if (!okm) goto fail;
        save_convo(g, job_id, messages->data);
    }
    /* Round budget exhausted: an honest mechanical checkpoint, never a canned
       question. State persists, so /v1/jobs/continue resumes in place (JI.3/6). */
    save_phase(g, job_id, "D", JI_VERIFY_MAX_ROUNDS, 0, 0, JI_VERIFY_MAX_ROUNDS);
    {
        TextBuffer ac = {0}; char nb[32]; snprintf(nb, sizeof(nb), "%d", seq);
        int e2 = text_add(&ac, "{\"seq\":") && text_add(&ac, nb) &&
                 text_add(&ac, ",\"type\":\"await_continue\",\"job_id\":") && text_json_string(&ac, job_id) &&
                 text_add(&ac, ",\"rounds_spent\":") && text_add(&ac, "24") && text_add(&ac, "}");
        int ok = e2 && job_sse_json(g, fd, job_id, ac.data) && samosa_send_all(fd, "data: [DONE]\n\n", 14);
        free(ac.data); free(messages->data); return ok;
    }
no_finish:
    job_sse_json(g, fd, job_id, "{\"type\":\"error\",\"code\":\"model_no_finish\",\"message\":\"The search ended without a finish call.\"}");
    samosa_send_all(fd, "data: [DONE]\n\n", 14);
    free(messages->data); return 0;
model_fail:
    job_sse_json(g, fd, job_id, "{\"type\":\"error\",\"message\":\"The model could not complete this file search.\"}");
    samosa_send_all(fd, "data: [DONE]\n\n", 14);
fail:
    free(messages->data); return 0;
}

/* Fresh find job: list the folder, triage every filename (Phase A), build the
   skim index (Phase B), seed the verify conversation with the goal + skim, then
   run the loop (Phase D). */
static int find_start(Gateway *g, int fd, const char *goal, const char *folder,
                      const char *job_id, int seq) {
    char *argv[] = {g->samosa_fs, "list", "--max-file-bytes", "104857600", (char *)folder, NULL};
    int status = 0; char *list_raw = run_capture(g, g->samosa_fs, argv, 16 << 20, &status);
    char *arena = NULL; jval *listing = list_raw ? json_parse(list_raw, &arena) : NULL;
    jval *items = listing && listing->t == J_OBJ ? json_get(listing, "items") : NULL;
    if (!items || items->t != J_ARR || !WIFEXITED(status) || WEXITSTATUS(status)) {
        json_free(listing); free(arena); free(list_raw);
        sse_json(fd, "{\"type\":\"error\",\"message\":\"The folder index could not be built.\"}");
        samosa_send_all(fd, "data: [DONE]\n\n", 14);
        return 0;
    }
    char ev[128]; snprintf(ev, sizeof(ev), "{\"seq\":%d,\"type\":\"indexing\",\"total\":%d}", seq++, items->len);
    if (!job_sse_json(g, fd, job_id, ev)) { json_free(listing); free(arena); free(list_raw); return 0; }
    int *verdict = calloc((size_t)(items->len > 0 ? items->len : 1), sizeof(int));
    int checked = 0;
    if (!verdict || !find_triage(g, fd, goal, folder, items, job_id, verdict, &checked, &seq)) {
        free(verdict); json_free(listing); free(arena); free(list_raw);
        sse_json(fd, "{\"type\":\"error\",\"message\":\"Filename triage failed.\"}");
        samosa_send_all(fd, "data: [DONE]\n\n", 14);
        return 0;
    }
    int listed = 0, parked = 0, deferred = 0;
    save_phase(g, job_id, "B", 0, items->len, 0, 0);
    if (!find_skim(g, fd, folder, job_id, items, verdict, &listed, &parked, &deferred, &seq)) {
        free(verdict); json_free(listing); free(arena); free(list_raw);
        sse_json(fd, "{\"type\":\"error\",\"message\":\"The skim index failed.\"}");
        samosa_send_all(fd, "data: [DONE]\n\n", 14);
        return 0;
    }
    if (deferred) {
        save_phase(g, job_id, "B", listed, listed + deferred, 0, 0);
        TextBuffer checkpoint = {0}; char number[32]; snprintf(number, sizeof(number), "%d", seq++);
        int sent = text_add(&checkpoint, "{\"seq\":") && text_add(&checkpoint, number) &&
                   text_add(&checkpoint, ",\"type\":\"await_continue\",\"job_id\":") && text_json_string(&checkpoint, job_id) &&
                   text_add(&checkpoint, ",\"skimmed\":");
        snprintf(number, sizeof(number), "%d", listed); sent = sent && text_add(&checkpoint, number) && text_add(&checkpoint, ",\"remaining\":");
        snprintf(number, sizeof(number), "%d", deferred); sent = sent && text_add(&checkpoint, number) && text_add(&checkpoint, "}");
        if (sent) sent = job_sse_json(g, fd, job_id, checkpoint.data) && samosa_send_all(fd, "data: [DONE]\n\n", 14);
        free(checkpoint.data); free(verdict); json_free(listing); free(arena); free(list_raw);
        return sent;
    }
    free(verdict);
    TextBuffer shortlist = {0}; int shortcount = 0;
    if (!find_classify(g, fd, goal, job_id, &shortlist, &shortcount, &seq)) {
        free(shortlist.data); json_free(listing); free(arena); free(list_raw);
        sse_json(fd, "{\"type\":\"error\",\"message\":\"Classification failed.\"}");
        samosa_send_all(fd, "data: [DONE]\n\n", 14);
        return 0;
    }
    TextBuffer user = {0};
    int ok = text_add(&user, "Goal: ") && text_add(&user, goal) && text_add(&user, "\n\n");
    if (ok && shortcount > 0)
        ok = text_add(&user, "Shortlisted files with the first lines of each. Confirm or reject each by reading more of it if needed, and report any you could not read:\n") &&
             text_add(&user, shortlist.data ? shortlist.data : "");
    else if (ok)
        ok = text_add(&user, "Triage and classification flagged no plausible files. If the goal implies content a filename could hide, reading is still worthwhile; otherwise finish with no matches.\n");
    free(shortlist.data);
    TextBuffer messages = {0};
    if (ok) ok = text_add(&messages, "{\"role\":\"system\",\"content\":") && text_json_string(&messages, ji_verify_system) &&
                 text_add(&messages, "},{\"role\":\"user\",\"content\":") && text_json_string(&messages, user.data) && text_add(&messages, "}");
    free(user.data);
    json_free(listing); free(arena); free(list_raw);
    if (!ok) { free(messages.data); sse_json(fd, "{\"type\":\"error\",\"message\":\"The search could not start.\"}"); samosa_send_all(fd, "data: [DONE]\n\n", 14); return 0; }
    save_convo(g, job_id, messages.data);
    save_phase(g, job_id, "D", 0, listed, shortcount, 0);
    return find_loop(g, fd, goal, folder, job_id, &messages, seq);
}

/* Pending ask_user tool-call id from the persisted conversation (heap), or NULL
   when the last message is not a model question awaiting an answer. */
static char *ji_pending_ask_id(const char *convo_inner) {
    TextBuffer wrap = {0};
    if (!text_add(&wrap, "[") || !text_add(&wrap, convo_inner) || !text_add(&wrap, "]")) { free(wrap.data); return NULL; }
    char *arena = NULL; jval *arr = json_parse(wrap.data, &arena); free(wrap.data);
    char *out = NULL;
    if (arr && arr->t == J_ARR && arr->len) {
        jval *last = arr->kids[arr->len - 1];
        jval *tc = last && last->t == J_OBJ ? json_get(last, "tool_calls") : NULL;
        jval *c0 = tc && tc->t == J_ARR && tc->len ? tc->kids[0] : NULL;
        jval *fn = c0 && c0->t == J_OBJ ? json_get(c0, "function") : NULL;
        jval *nm = fn && fn->t == J_OBJ ? json_get(fn, "name") : NULL;
        jval *id = c0 && c0->t == J_OBJ ? json_get(c0, "id") : NULL;
        if (nm && nm->t == J_STR && !strcmp(nm->str, "ask_user") && id && id->t == J_STR) out = strdup(id->str);
    }
    json_free(arr); free(arena); return out;
}

/* The substring router is only a cheap fast path.  Ambiguous wording still
   reaches a small model classifier, which may select find but can never grant
   a mutating action (find remains read-only by construction). */
static int model_find_intent(Gateway *g, const char *goal) {
    const char *system =
        "Classify this local-files request. Return JSON only: {\"kind\":\"find\"|\"report\"|\"organize\"}. "
        "find means locate documents matching a described subject; report means summarize a folder; "
        "organize means sort or move files.";
    char *reply = ji_model_text(g, system, goal);
    char *object = first_json_object(reply);
    char *arena = NULL; jval *parsed = object ? json_parse(object, &arena) : NULL;
    jval *kind = parsed && parsed->t == J_OBJ ? json_get(parsed, "kind") : NULL;
    int is_find = kind && kind->t == J_STR && !strcmp(kind->str, "find");
    json_free(parsed); free(arena); free(object); free(reply);
    return is_find;
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
    else snprintf(job_id, sizeof(job_id), "job-%ld-%ld-%lld", (long)time(NULL), (long)getpid(),
                  (long long)monotonic_millis());
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
    if (!job_sse_json(g, fd, job_id, event)) goto fail;
    int is_find = find_intent(goal);
    if (!is_find) is_find = model_find_intent(g, goal);
    if (!job_sse_json(g, fd, job_id, is_find ?
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
    if (!job_sse_json(g, fd, job_id, event)) goto fail;
    if (is_find) {
        json_free(survey); free(arena); free(raw);
        return find_start(g, fd, goal, folder, job_id, 4);
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

/* JI.6: the user's answer to a model question re-enters the verify loop as the
   tool result of the pending ask_user call. The goal is never mutated and the
   run-1 conversation is preserved intact — the RC3/RC4 fix. */
static int jobs_answer(Gateway *g, int fd, const SamosaHttpRequest *request) {
    char *arena = NULL; jval *root = json_parse(request->body, &arena);
    jval *id = root && root->t == J_OBJ ? json_get(root, "job_id") : NULL;
    jval *answer = root && root->t == J_OBJ ? json_get(root, "answer") : NULL;
    if (!id || id->t != J_STR || !valid_job_id(id->str) || !answer || answer->t != J_STR || !*answer->str) {
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
    char *convo = load_convo(g, job_id);
    char *pending = convo ? ji_pending_ask_id(convo) : NULL;
    if (!convo || !pending) {
        free(convo); free(pending); free(answer_copy); free(goal); free(folder);
        return samosa_http_json_error(fd, 409, "no_pending_question", "That job is not waiting on an answer.");
    }
    TextBuffer messages = {0};
    int built = text_add(&messages, convo) &&
                text_add(&messages, ",{\"role\":\"tool\",\"tool_call_id\":") && text_json_string(&messages, pending) &&
                text_add(&messages, ",\"name\":\"ask_user\",\"content\":") && text_json_string(&messages, answer_copy) &&
                text_add(&messages, "}");
    free(convo); free(pending); free(answer_copy);
    if (!built) { free(messages.data); free(goal); free(folder); return samosa_http_json_error(fd, 500, "resume_failed", "The job could not be resumed."); }
    if (!samosa_http_stream_headers(fd)) { free(messages.data); free(goal); free(folder); return 0; }
    save_convo(g, job_id, messages.data);
    int ok = find_loop(g, fd, goal, folder, job_id, &messages, 100);
    free(goal); free(folder); return ok;
}

/* JI.6: the Continue button (budget checkpoints, crash recovery). Reloads the
   persisted conversation and re-enters the loop with no new message. */
static int jobs_continue(Gateway *g, int fd, const SamosaHttpRequest *request) {
    char *arena = NULL; jval *root = json_parse(request->body, &arena);
    jval *id = root && root->t == J_OBJ ? json_get(root, "job_id") : NULL;
    if (!id || id->t != J_STR || !valid_job_id(id->str)) {
        json_free(root); free(arena);
        return samosa_http_json_error(fd, 400, "invalid_continue", "job_id is required.");
    }
    char job_id[128]; path_copy(job_id, sizeof(job_id), id->str);
    char *goal = NULL, *folder = NULL;
    json_free(root); free(arena);
    if (!load_job_state(g, job_id, &goal, &folder)) {
        free(goal); free(folder);
        return samosa_http_json_error(fd, 404, "job_not_found", "That paused job is unavailable.");
    }
    char *phase = load_phase_name(g, job_id);
    if (phase && strcmp(phase, "D")) {
        free(phase);
        if (!samosa_http_stream_headers(fd)) { free(goal); free(folder); return 0; }
        int ok = find_start(g, fd, goal, folder, job_id, 100);
        free(goal); free(folder); return ok;
    }
    free(phase);
    char *convo = load_convo(g, job_id);
    if (!convo) { free(goal); free(folder); return samosa_http_json_error(fd, 409, "nothing_to_continue", "That job has no saved conversation."); }
    TextBuffer messages = {0};
    int built = text_add(&messages, convo);
    free(convo);
    if (!built) { free(messages.data); free(goal); free(folder); return samosa_http_json_error(fd, 500, "resume_failed", "The job could not be resumed."); }
    if (!samosa_http_stream_headers(fd)) { free(messages.data); free(goal); free(folder); return 0; }
    int ok = find_loop(g, fd, goal, folder, job_id, &messages, 100);
    free(goal); free(folder); return ok;
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

static char *definition_pdf_page_text(Gateway *g, const char *path, int page,
                                      int *page_count_out) {
    char start_text[32];
    snprintf(start_text, sizeof(start_text), "%d", page);
    char *argv[] = {g->samosa_extract, "--json-pages", (char *)path, start_text, "1", NULL};
    int status = 0; char *raw = run_capture(g, g->samosa_extract, argv, 1 << 20, &status);
    if (!raw || !WIFEXITED(status) || WEXITSTATUS(status)) { free(raw); return NULL; }
    char *arena = NULL; jval *result = json_parse(raw, &arena);
    jval *text_value = result && result->t == J_OBJ ? json_get(result, "text") : NULL;
    jval *page_count = result && result->t == J_OBJ ? json_get(result, "page_count") : NULL;
    if (page_count_out && page_count && page_count->t == J_NUM && page_count->num >= 1)
        *page_count_out = (int)page_count->num;
    char *text = text_value && text_value->t == J_STR ? strdup(text_value->str) : NULL;
    json_free(result); free(arena); free(raw); return text;
}

static char *definition_pdf_source(Gateway *g, const char *path) {
    int page_count = 0;
    char *first = definition_pdf_page_text(g, path, 1, &page_count);
    if (!first) return NULL;
    if (page_count <= 1) return first;
    char *last = definition_pdf_page_text(g, path, page_count, NULL);
    if (!last) return first;
    TextBuffer source = {0};
    int ok = text_add(&source, "Page 1:\n") && text_add(&source, first) &&
             text_add(&source, "\n\nFinal page:\n") && text_add(&source, last);
    free(first); free(last);
    if (!ok) { free(source.data); return NULL; }
    return source.data;
}

typedef struct {
    char *text;
    char *image_data_uri;
} DefinitionSource;

static void definition_source_free(DefinitionSource *source) {
    if (!source) return;
    free(source->text);
    free(source->image_data_uri);
}

static int media_is_definition_image(const char *media) {
    return media && (!strcmp(media, "image/png") || !strcmp(media, "image/jpeg"));
}

static char *definition_image_data_uri(const char *path, const char *media) {
    size_t length = 0;
    unsigned char *bytes = read_file_bytes_limit(path, MAX_DEFINITION_IMAGE_BYTES, &length);
    if (!bytes) return NULL;
    char *encoded = base64_encode_bytes(bytes, length);
    free(bytes);
    if (!encoded) return NULL;
    TextBuffer uri = {0};
    int ok = text_add(&uri, "data:") && text_add(&uri, media) &&
             text_add(&uri, ";base64,") && text_add(&uri, encoded);
    free(encoded);
    if (!ok) { free(uri.data); return NULL; }
    return uri.data;
}

static int definition_source(Gateway *g, jval *item, DefinitionSource *source) {
    memset(source, 0, sizeof(*source));
    jval *path = json_get(item, "path"), *media = json_get(item, "media_type");
    if (!path || path->t != J_STR || !media || media->t != J_STR) return 0;
    if (!strcmp(media->str, "text/plain")) source->text = read_bounded_text(path->str);
    else if (!strcmp(media->str, "application/pdf")) source->text = definition_pdf_source(g, path->str);
    else if (media_is_definition_image(media->str)) source->image_data_uri = definition_image_data_uri(path->str, media->str);
    return source->text || source->image_data_uri;
}

/* Return a heap copy of the first balanced JSON object in s, scanning
   string-aware so braces inside strings do not miscount. Recovers the object
   when a model wraps it in ```json fences or surrounds it with prose (Qwen
   vision does this; llama-server backends usually return bare JSON). NULL if no
   balanced object is present. This is the J1.5 recovery contract. */
static char *first_json_object(const char *s) {
    if (!s) return NULL;
    const char *start = strchr(s, '{');
    if (!start) return NULL;
    int depth = 0, in_str = 0, esc = 0;
    const char *p = start;
    for (; *p; ++p) {
        char c = *p;
        if (in_str) {
            if (esc) esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"') in_str = 0;
        } else if (c == '"') in_str = 1;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) { ++p; break; }
    }
    if (depth != 0) return NULL;
    size_t len = (size_t)(p - start);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, start, len); out[len] = 0;
    return out;
}

static int definition_record(TextBuffer *record, jval *item, const char *extracted,
                             int passed, const char *review_reason) {
    jval *path = json_get(item, "path"), *hash = json_get(item, "input_sha256");
    char *object = first_json_object(extracted);
    char *arena = NULL; jval *fields = object ? json_parse(object, &arena) : NULL;
    if (!fields || fields->t != J_OBJ) passed = 0;
    if (!text_add(record, "{\"input_path\":") || !text_json_string(record, path && path->t == J_STR ? path->str : "") ||
        !text_add(record, ",\"input_sha256\":") || !text_json_string(record, hash && hash->t == J_STR ? hash->str : "")) {
        json_free(fields); free(arena); free(object); return 0;
    }
    if (passed) {
        if (!text_add(record, ",\"status\":\"passed\",\"extracted\":")) { json_free(fields); free(arena); free(object); return 0; }
    } else if (!text_add(record, ",\"status\":\"review_required\",\"reasons\":[") ||
               !text_json_string(record, review_reason ? review_reason : "invalid_model_output") ||
               !text_add(record, "],\"extracted\":")) {
        json_free(fields); free(arena); free(object); return 0;
    }
    if (!text_json_value(record, fields)) { json_free(fields); free(arena); free(object); return 0; }
    if (fields && fields->t == J_OBJ) for (int i = 0; i < fields->len; ++i)
        if (!text_add(record, ",") || !text_json_string(record, fields->keys[i]) || !text_add(record, ":") ||
            !text_json_value(record, fields->kids[i])) { json_free(fields); free(arena); free(object); return 0; }
    int ok = text_add(record, "}"); json_free(fields); free(arena); free(object); return ok;
}

static int definition_request(Gateway *g, int fd, const SamosaHttpRequest *request,
                              int preview) {
    char *arena = NULL; jval *body = json_parse(request->body, &arena);
    jval *job = body && body->t == J_OBJ ? json_get(body, "job") : NULL;
    jval *input = job && job->t == J_OBJ ? json_get(job, "input") : NULL;
    jval *folder = input && input->t == J_OBJ ? json_get(input, "folder") : NULL;
    jval *instruction = job && job->t == J_OBJ ? json_get(job, "instruction") : NULL;
    jval *schema = job && job->t == J_OBJ ? json_get(job, "output_schema") : NULL;
    jval *output = job && job->t == J_OBJ ? json_get(job, "output") : NULL;
    jval *output_dir = output && output->t == J_OBJ ? json_get(output, "dir") : NULL;
    jval *job_id_value = job && job->t == J_OBJ ? json_get(job, "job_id") : NULL;
    jval *expanded_value = body && body->t == J_OBJ ? json_get(body, "expanded") : NULL;
    if (!job || job->t != J_OBJ || !folder || folder->t != J_STR ||
        !schema || schema->t != J_OBJ || !output_dir || output_dir->t != J_STR) {
        json_free(body); free(arena); return samosa_http_json_error(fd, 400, "invalid_definition", "The job needs input.folder, output_schema, and output.dir.");
    }
    char *argv[] = {g->samosa_fs, "list", "--max-file-bytes", "104857600", folder->str, NULL};
    int status = 0; char *list_raw = run_capture(g, g->samosa_fs, argv, 16 << 20, &status);
    char *list_arena = NULL; jval *listing = list_raw ? json_parse(list_raw, &list_arena) : NULL;
    jval *items = listing && listing->t == J_OBJ ? json_get(listing, "items") : NULL;
    if (!items || items->t != J_ARR || !WIFEXITED(status) || WEXITSTATUS(status)) {
        json_free(listing); free(list_arena); free(list_raw); json_free(body); free(arena);
        return samosa_http_json_error(fd, 400, "definition_scan_failed", "The input folder could not be inspected.");
    }
    int wanted = preview ? (expanded_value && expanded_value->t == J_BOOL && expanded_value->boolean ? 3 : 1) : items->len;
    int selected[3] = {-1, -1, -1}, selected_count = 0;
    if (preview) {
        for (int i = 0; i < items->len && selected_count < wanted; ++i) {
            jval *media = json_get(items->kids[i], "media_type"); int seen = 0;
            for (int j = 0; j < selected_count; ++j) {
                jval *prior = json_get(items->kids[selected[j]], "media_type");
                if (media && prior && media->t == J_STR && prior->t == J_STR && !strcmp(media->str, prior->str)) seen = 1;
            }
            if (!seen) selected[selected_count++] = i;
        }
        for (int i = 0; i < items->len && selected_count < wanted; ++i) {
            int seen = 0; for (int j = 0; j < selected_count; ++j) if (selected[j] == i) seen = 1;
            if (!seen) selected[selected_count++] = i;
        }
    }
    char job_id[128];
    if (job_id_value && job_id_value->t == J_STR) path_copy(job_id, sizeof(job_id), job_id_value->str);
    else snprintf(job_id, sizeof(job_id), "job-%ld-%ld", (long)time(NULL), (long)getpid());
    save_job_state(g, job_id, instruction && instruction->t == J_STR ? instruction->str : "definition", folder->str);
    char artifact_dir[PATH_MAX], artifact_path[PATH_MAX];
    if (preview) {
        if (!path_join(artifact_dir, sizeof(artifact_dir), output_dir->str, "preview")) goto definition_fail;
    } else if (!path_copy(artifact_dir, sizeof(artifact_dir), output_dir->str)) goto definition_fail;
    if (!mkdirs(artifact_dir) || !path_join(artifact_path, sizeof(artifact_path), artifact_dir, "output.jsonl")) goto definition_fail;
    TextBuffer records_file = {0}, records_array = {0}; int completed = 0, seq = 0;
    double active_seconds = 0.0;
    int interlock_enabled = job_pause_when_user_active(job);
    if (!preview && !samosa_http_stream_headers(fd)) goto definition_fail;
    int count = preview ? selected_count : items->len;
    for (int n = 0; n < count; ++n) {
        int item_index = preview ? selected[n] : n; jval *item = items->kids[item_index];
        if (!preview && !definition_interlock(g, fd, job_id, interlock_enabled, &seq)) {
            free(records_file.data); free(records_array.data); goto definition_fail;
        }
        DefinitionSource source;
        int have_source = definition_source(g, item, &source);
        /* An image unit needs a vision-capable active backend. Rather than send
           the image to a text-only model and get garbage, queue it for review
           with a clear reason (select Bonsai or Qwen for image jobs). */
        int needs_vision = have_source && source.image_data_uri && !backend_supports_images(g, g->backend);
        double call_seconds = 0.0;
        char *extracted = (have_source && !needs_vision) ?
            model_extract(g, instruction && instruction->t == J_STR ? instruction->str : "",
                          schema, source.text, source.image_data_uri,
                          job_inference_max_tokens(job), &call_seconds) : NULL;
        active_seconds += call_seconds;
        definition_source_free(&source); TextBuffer record = {0};
        if (!definition_record(&record, item, extracted, extracted != NULL,
                               needs_vision ? "vision_backend_required" : "invalid_model_output")) { free(extracted); free(record.data); free(records_file.data); free(records_array.data); goto definition_fail; }
        free(extracted);
        if (!text_add(&records_file, record.data) || !text_add(&records_file, "\n") ||
            (completed && !text_add(&records_array, ",")) || !text_add(&records_array, record.data)) {
            free(record.data); free(records_file.data); free(records_array.data); goto definition_fail;
        }
        if (!preview) {
            jval *path = json_get(item, "path"); TextBuffer event = {0}; char number[32], seconds[32], active[32];
            snprintf(number, sizeof(number), "%d", ++seq);
            snprintf(seconds, sizeof(seconds), "%.3f", call_seconds);
            snprintf(active, sizeof(active), "%.3f", active_seconds);
            text_add(&event, "{\"seq\":"); text_add(&event, number);
            text_add(&event, ",\"type\":\"item_complete\",\"i\":");
            snprintf(number, sizeof(number), "%d", n + 1); text_add(&event, number); text_add(&event, ",\"n\":");
            snprintf(number, sizeof(number), "%d", count); text_add(&event, number); text_add(&event, ",\"input_path\":");
            text_json_string(&event, path && path->t == J_STR ? path->str : "");
            text_add(&event, ",\"model_call_seconds\":"); text_add(&event, seconds);
            text_add(&event, ",\"active_inference_seconds\":"); text_add(&event, active);
            text_add(&event, "}"); sse_json(fd, event.data); free(event.data);
        }
        ++completed; free(record.data);
    }
    if (!write_small_file(artifact_path, records_file.data ? records_file.data : "")) { free(records_file.data); free(records_array.data); goto definition_fail; }
    if (!preview) {
        char review_dir[PATH_MAX], review_path[PATH_MAX];
        if (job_state_path(g, job_id, "results", review_dir, 1)) mkdirs(review_dir);
        if (job_state_path(g, job_id, "results/output.jsonl", review_path, 1)) write_small_file(review_path, records_file.data ? records_file.data : "");
    }
    free(records_file.data);
    int ok;
    if (preview) {
        TextBuffer response = {0}; char number[32]; snprintf(number, sizeof(number), "%d", completed);
        text_add(&response, "{\"ok\":true,\"sample_count\":"); text_add(&response, number);
        text_add(&response, ",\"artifact_dir\":\"preview\",\"records\":["); text_add(&response, records_array.data ? records_array.data : ""); text_add(&response, "]}");
        ok = samosa_http_response(fd, 200, "application/json", response.data, NULL); free(response.data);
    } else {
        TextBuffer event = {0}; char number[32], active[32], summary[80];
        snprintf(number, sizeof(number), "%d", ++seq);
        snprintf(active, sizeof(active), "%.3f", active_seconds);
        snprintf(summary, sizeof(summary), "Processed %d item%s.", completed, completed == 1 ? "" : "s");
        if (!text_add(&event, "{\"seq\":") || !text_add(&event, number) ||
            !text_add(&event, ",\"type\":\"done\",\"job_id\":") ||
            !text_json_string(&event, job_id) ||
            !text_add(&event, ",\"summary\":") ||
            !text_json_string(&event, summary) ||
            !text_add(&event, ",\"completed\":")) {
            free(event.data); ok = 0;
        } else {
            snprintf(number, sizeof(number), "%d", completed);
            ok = text_add(&event, number) &&
                 text_add(&event, ",\"active_inference_seconds\":") &&
                 text_add(&event, active) && text_add(&event, "}") &&
                 sse_json(fd, event.data) && samosa_send_all(fd, "data: [DONE]\n\n", 14);
            free(event.data);
        }
    }
    free(records_array.data); json_free(listing); free(list_arena); free(list_raw); json_free(body); free(arena); return ok;
definition_fail:
    json_free(listing); free(list_arena); free(list_raw); json_free(body); free(arena);
    return preview ? samosa_http_json_error(fd, 500, "definition_failed", "The definition could not be run.") : 0;
}

static int jobs_apply_or_undo(Gateway *g, int fd, const SamosaHttpRequest *request,
                              int undo) {
    char *arena = NULL; jval *body = json_parse(request->body, &arena);
    jval *id = body && body->t == J_OBJ ? json_get(body, "job_id") : NULL;
    if (!id || id->t != J_STR) { json_free(body); free(arena); return samosa_http_json_error(fd, 400, "invalid_job", "job_id is required."); }
    char job_id[128]; path_copy(job_id, sizeof(job_id), id->str); json_free(body); free(arena);
    char *goal = NULL, *folder = NULL; if (!load_job_state(g, job_id, &goal, &folder)) {
        free(goal); free(folder); return samosa_http_json_error(fd, 404, "job_not_found", "That job is unavailable.");
    }
    free(goal); char plan_path[PATH_MAX], applied_path[PATH_MAX];
    if (!job_state_path(g, job_id, undo ? "applied.jsonl" : "plan.jsonl", plan_path, 0) ||
        !job_state_path(g, job_id, "applied.jsonl", applied_path, 1)) { free(folder); return 0; }
    char *raw = read_file_limit(plan_path, 1 << 20); if (!raw) { free(folder); return samosa_http_json_error(fd, 404, "plan_not_found", "There is no pending move plan."); }
    if (!samosa_http_stream_headers(fd)) { free(raw); free(folder); return 0; }
    int moved = 0, total = 0; char *save = NULL;
    for (char *line = strtok_r(raw, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *line_arena = NULL; jval *move = json_parse(line, &line_arena);
        jval *src = json_get(move, "src"), *dst = json_get(move, "dst");
        if (!src || src->t != J_STR || !dst || dst->t != J_STR) { json_free(move); free(line_arena); continue; }
        char *argv[] = {g->samosa_fs, undo ? "undo" : "move", "--root", folder, src->str, dst->str, NULL};
        int status = 0; char *result = run_capture(g, g->samosa_fs, argv, 65536, &status); ++total;
        int ok_move = result && WIFEXITED(status) && !WEXITSTATUS(status) && strstr(result, "\"moved\":true");
        if (ok_move) ++moved;
        TextBuffer event = {0}; char number[32]; snprintf(number, sizeof(number), "%d", total);
        text_add(&event, "{\"type\":\"action\",\"op\":"); text_json_string(&event, undo ? "revert" : "move");
        text_add(&event, ",\"i\":"); text_add(&event, number); text_add(&event, ",\"n\":1,\"src\":"); text_json_string(&event, src->str);
        text_add(&event, ",\"dst\":"); text_json_string(&event, dst->str); text_add(&event, ok_move ? ",\"ok\":true}" : ",\"ok\":false,\"reason\":\"move_refused\"}");
        sse_json(fd, event.data); free(event.data); free(result); json_free(move); free(line_arena);
    }
    if (!undo && moved > 0) write_small_file(applied_path, raw);
    if (undo && moved == total && total > 0) unlink(applied_path);
    char event[256];
    if (undo) snprintf(event, sizeof(event), "{\"type\":\"undone\",\"undone\":%d,\"skipped\":%d}", moved, total - moved);
    else snprintf(event, sizeof(event), "{\"type\":\"applied\",\"applied\":%d,\"skipped\":%d}", moved, total - moved);
    int ok = sse_json(fd, event);
    snprintf(event, sizeof(event), "{\"type\":\"done\",\"job_id\":\"%s\",\"summary\":\"%s %d file%s.\"}",
             job_id, undo ? "Restored" : "Moved", moved, moved == 1 ? "" : "s");
    ok = ok && sse_json(fd, event) && samosa_send_all(fd, "data: [DONE]\n\n", 14);
    free(raw); free(folder); return ok;
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

/* Vision availability per backend. Qwen's tower is built into its C engine, so
   it is always image-capable. Bonsai (Qwen3.6-27B via llama-server) is
   image-capable only when its optional mmproj vision pack is present on disk;
   without it, llama-server runs text-only. Ornith has no vision. */
static int backend_supports_images(Gateway *g, const char *name) {
    if (!strcmp(name, "qwen")) return 1;
    if (!strcmp(name, "bonsai")) return regular_file(g->bonsai_mmproj, 0);
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
            int is_ornith = !strcmp(g->backend, "ornith");
            char *model = is_ornith ? g->ornith_model : g->bonsai_model;
            char *alias = is_ornith ? (char *)"ornith-1.0-9b" : (char *)"bonsai-27b-1bit";
            char *argv[24]; int a = 0;
            argv[a++] = g->llama_server; argv[a++] = (char *)"-m"; argv[a++] = model;
            argv[a++] = (char *)"-ngl"; argv[a++] = (char *)"99";
            argv[a++] = (char *)"-c"; argv[a++] = (char *)"8192";
            argv[a++] = (char *)"-np"; argv[a++] = (char *)"1";
            argv[a++] = (char *)"--cache-ram"; argv[a++] = (char *)"0";
            argv[a++] = (char *)"--host"; argv[a++] = (char *)"127.0.0.1";
            argv[a++] = (char *)"--port"; argv[a++] = port;
            argv[a++] = (char *)"--no-ui"; argv[a++] = (char *)"--alias"; argv[a++] = alias;
            /* Bonsai is image-capable via its optional mmproj vision pack; load it
               only when present so image jobs reach a fast local vision backend.
               Text-only serving (and Ornith) skips it. */
            if (!is_ornith && backend_supports_images(g, "bonsai")) {
                argv[a++] = (char *)"--mmproj"; argv[a++] = g->bonsai_mmproj;
            }
            argv[a] = NULL;
            execv(g->llama_server, argv);
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
    int interactive = !request->is_background && !strcmp(request->path, "/v1/chat/completions");
    if (interactive) interactive_start(g);
    pthread_mutex_lock(&g->mu); g->upstream_fd = upstream; pthread_mutex_unlock(&g->mu);
    atomic_fetch_add(&g->generating, 1);
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
    atomic_fetch_sub(&g->generating, 1);
    if (interactive) interactive_finish(g);
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
            backend_supports_images(g, g->backend) ? "true" : "false",
            ready ? "true" : "false", (!ready && pid > 0) ? "true" : "false",
            atomic_load(&g->generating) ? "true" : "false", (long)pid);
        return samosa_http_response(fd, 200, "application/json", body, NULL);
    }
    if (!strcmp(request->method, "GET") && !strcmp(request->path, "/internal/v1/status")) {
        char body[1024], age[32], last[32];
        long long last_mono = atomic_load(&g->last_interactive_mono_ms);
        long long last_wall = atomic_load(&g->last_interactive_wall_ms);
        if (last_mono > 0) snprintf(age, sizeof(age), "%.3f", (monotonic_millis() - last_mono) / 1000.0);
        else snprintf(age, sizeof(age), "null");
        if (last_wall > 0) snprintf(last, sizeof(last), "%.3f", last_wall / 1000.0);
        else snprintf(last, sizeof(last), "null");
        snprintf(body, sizeof(body),
            "{\"inference_busy\":%s,\"interactive_active\":%s,"
            "\"last_interactive_ts\":%s,\"last_interactive_age_seconds\":%s,"
            "\"interactive_cooldown_seconds\":%.3f}",
            atomic_load(&g->generating) ? "true" : "false",
            atomic_load(&g->interactive_active) ? "true" : "false",
            last, age, interactive_cooldown_ms() / 1000.0);
        return samosa_http_response(fd, 200, "application/json", body, NULL);
    }
    if (!strcmp(request->method, "GET") && !strcmp(request->path, "/v1/backends")) {
        char body[1536];
        snprintf(body, sizeof(body),
            "{\"active\":\"%s\",\"backends\":["
            "{\"id\":\"bonsai\",\"label\":\"Bonsai 27B 1-bit\",\"model\":\"bonsai-27b-1bit\",\"supports_images\":%s,\"available\":%s},"
            "{\"id\":\"ornith\",\"label\":\"Ornith 9B\",\"model\":\"ornith-1.0-9b\",\"supports_images\":false,\"available\":%s},"
            "{\"id\":\"qwen\",\"label\":\"Qwen3.6 35B A3B\",\"model\":\"qwen3.6-35b-a3b\",\"supports_images\":true,\"available\":%s}]}",
            g->backend, backend_supports_images(g, "bonsai") ? "true" : "false",
            backend_available(g, "bonsai") ? "true" : "false",
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
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/jobs/continue"))
        return jobs_continue(g, fd, request);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/jobs/review"))
        return jobs_review(g, fd, request);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/jobs/review/correct"))
        return jobs_review_correct(g, fd, request);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/jobs/definition/preview"))
        return definition_request(g, fd, request, 1);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/jobs/definition/run"))
        return definition_request(g, fd, request, 0);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/jobs/apply"))
        return jobs_apply_or_undo(g, fd, request, 0);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/jobs/undo"))
        return jobs_apply_or_undo(g, fd, request, 1);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/jobs/schedule/arm"))
        return jobs_schedule_arm(g, fd, request);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/jobsd/once"))
        return jobsd_once_native(g, fd, request);
    if (!strcmp(request->method, "GET") && !strcmp(request->path, "/v1/jobs/launchd-plist"))
        return jobs_launchd_plist(g, fd);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/jobs/launchd/install"))
        return jobs_launchd_install(g, fd);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/jobs/launchd/uninstall"))
        return jobs_launchd_uninstall(g, fd);
    if (!strcmp(request->method, "GET") && !strcmp(request->path, "/v1/jobs/launchd/status"))
        return jobs_launchd_status(g, fd);
    if (!strcmp(request->method, "POST") && !strcmp(request->path, "/v1/jobs/public-inputs/update"))
        return jobs_public_inputs_update(g, fd, request);
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

/* jobsd is single-threaded and short-lived; on a signal, kill any tracked
   child (sidecar, curl, caffeinate) so nothing is orphaned, then exit. */
static Gateway *jobsd_signal_gateway;
static void jobsd_on_signal(int number) {
    (void)number;
    if (jobsd_signal_gateway)
        for (size_t i = 0; i < sizeof(jobsd_signal_gateway->job_pids) / sizeof(jobsd_signal_gateway->job_pids[0]); ++i)
            if (jobsd_signal_gateway->job_pids[i] > 0) kill(jobsd_signal_gateway->job_pids[i], SIGKILL);
    _Exit(1);
}

static int load_config(Gateway *g) {
    memset(g, 0, sizeof(*g));
    g->backend_pid = 0; g->upstream_fd = -1;
    pthread_mutex_init(&g->mu, NULL);
    atomic_init(&g->generating, 0);
    atomic_init(&g->interactive_active, 0);
    atomic_init(&g->last_interactive_mono_ms, 0);
    atomic_init(&g->last_interactive_wall_ms, 0);
    atomic_init(&g->stopping, 0);
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
    ENV_PATH(bonsai_mmproj, "SAMOSA_BONSAI_MMPROJ", "models/bonsai-27b-1bit/Bonsai-27B-mmproj-Q8_0.gguf");
    ENV_PATH(ornith_model, "SAMOSA_ORNITH_MODEL", "models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf");
    ENV_PATH(samosa_fs, "SAMOSA_FS", "current/bin/samosa-fs");
    ENV_PATH(samosa_extract, "SAMOSA_EXTRACT", "current/bin/samosa-extract");
    ENV_PATH(samosa_ocr, "SAMOSA_OCR", "current/bin/samosa-ocr");
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

int main(int argc, char **argv) {
    Gateway gateway;
    if (!load_config(&gateway)) {
        fprintf(stderr, "samosa-gateway: invalid configuration\n"); return 2;
    }
    /* jobsd one-shot: poll armed schedules, run any inside their window, exit.
       No backend, no listener — this is what launchd fires on an interval. */
    if (argc >= 2 && !strcmp(argv[1], "jobsd-once")) {
        jobsd_signal_gateway = &gateway;
        signal(SIGINT, jobsd_on_signal); signal(SIGTERM, jobsd_on_signal);
        int ok = jobsd_once_native(&gateway, -1, NULL);
        pthread_mutex_destroy(&gateway.mu);
        return ok ? 0 : 1;
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
