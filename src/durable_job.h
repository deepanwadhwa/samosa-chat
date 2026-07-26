/* durable_job.h — the generalized durable background-operation primitive
 * (T0.4, docs/TASKS_UI_CHUTNI.md). Model downloads (T2.2) and Chutni builds
 * (T4.x) both need: an atomically-written small state file, an append-only
 * sequenced event log matching the §5.7 event contract, and a one-writer
 * lock that survives a crash without being fooled by PID reuse. This header
 * factors those three primitives out so neither future caller reinvents
 * them, without touching the already-tested Phase-JI job code in
 * src/samosa_gateway.c (job_state_path/save_job_state/append_job_event_file
 * remain as they are; append_job_event_file's on-disk event shape is
 * compatible with what's here, so a future migration is possible but not
 * forced by this task).
 *
 * Header-only, self-contained, no network. Every write is temp-file +
 * fsync + rename, matching the project's existing atomic-write convention
 * (src/samosa_gateway.c's write_small_file, src/read_cache.h's
 * read_cache_put).
 */
#ifndef DURABLE_JOB_H
#define DURABLE_JOB_H

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__linux__)
/* /proc/<pid>/stat is read directly; no extra header needed. */
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ---------------------------------------------------------------------- */
/* Path jail: a job/scope id must be a plain token (matches the existing
 * job_state_path()'s validation exactly) so it can never be a path escape,
 * and the directory is created under an arbitrary caller-supplied root --
 * unlike job_state_path(), which is hardcoded to one gateway-wide jobs
 * root. This is what lets a model install (~/.samosa/models/<id>/.partial/
 * <job-id>/) and a Chutni build (~/.samosa/chutni/scopes/<scope-id>/) both
 * use the same primitive without sharing a directory tree. */
static int durable_id_valid(const char *id) {
    if (!id || !*id) return 0;
    for (const char *p = id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return 0;
    return 1;
}

static int durable_mkdirs(const char *dir) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s", dir) >= (int)sizeof(tmp)) return 0;
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0700); *p = '/'; }
    }
    return mkdir(tmp, 0700) == 0 || errno == EEXIST;
}

/* base_root/id, created if `create`. Returns 0 on an invalid id or a
 * directory that couldn't be created. */
static int durable_job_dir(const char *base_root, const char *id, char out[PATH_MAX], int create) {
    if (!durable_id_valid(id)) return 0;
    int n = snprintf(out, PATH_MAX, "%s/%s", base_root, id);
    if (n < 0 || n >= PATH_MAX) return 0;
    return !create || durable_mkdirs(out);
}

/* ---------------------------------------------------------------------- */
/* Atomic small-file state: temp + fsync + rename, matching this project's
 * established convention. */
static int durable_state_put(const char *path, const char *text) {
    char temp[PATH_MAX];
    if (snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(temp))
        return 0;
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return 0;
    size_t len = strlen(text), off = 0;
    int ok = 1;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = 0; break; }
        off += (size_t)n;
    }
    if (ok) ok = fsync(fd) == 0;
    if (close(fd)) ok = 0;
    if (ok) ok = rename(temp, path) == 0;
    if (!ok) unlink(temp);
    else chmod(path, 0600);
    return ok;
}

/* Reads at most max_bytes (malloc'd, NUL-terminated). Caller frees. NULL on
 * any failure, including a file at or over max_bytes (never a silent
 * truncation). */
static char *durable_state_get(const char *path, size_t max_bytes) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(max_bytes + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, max_bytes, f);
    int truncated = !feof(f);
    fclose(f);
    if (truncated) { free(buf); return NULL; }
    buf[n] = 0;
    return buf;
}

/* ---------------------------------------------------------------------- */
/* Durable events: the §5.7 shape exactly --
 *   {"seq","time","job_id","kind","state","phase","completed","total","unit",
 *    "current_item","message"}
 * `total` may be omitted (NULL) for "unknown" per the spec ("the UI uses an
 * indeterminate state"). Appends one complete line under an flock on the
 * events file itself, so concurrent appenders (a worker thread and a status
 * poll) can't interleave two events into one torn line, and fsyncs before
 * returning -- "the event is persisted before it is sent over SSE" is the
 * caller's job (this returns only after the persist half is durable). */
static int durable_event_append(const char *events_path, long seq, const char *job_id,
                                const char *kind, const char *state, const char *phase,
                                const long *completed, const long *total, const char *unit,
                                const char *current_item, const char *message) {
    char now[40];
    {
        time_t t = time(NULL);
        struct tm tm_utc;
        gmtime_r(&t, &tm_utc);
        strftime(now, sizeof(now), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    }
    char line[2048];
    int n = snprintf(line, sizeof(line),
        "{\"seq\":%ld,\"time\":\"%s\",\"job_id\":\"%s\",\"kind\":\"%s\",\"state\":\"%s\","
        "\"phase\":\"%s\",\"completed\":", seq, now, job_id, kind, state, phase);
    if (n < 0 || (size_t)n >= sizeof(line)) return 0;
    size_t off = (size_t)n;
    if (completed) off += (size_t)snprintf(line + off, sizeof(line) - off, "%ld", *completed);
    else off += (size_t)snprintf(line + off, sizeof(line) - off, "null");
    if (off >= sizeof(line)) return 0;
    off += (size_t)snprintf(line + off, sizeof(line) - off, ",\"total\":");
    if (off >= sizeof(line)) return 0;
    if (total) off += (size_t)snprintf(line + off, sizeof(line) - off, "%ld", *total);
    else off += (size_t)snprintf(line + off, sizeof(line) - off, "null");
    if (off >= sizeof(line)) return 0;
    off += (size_t)snprintf(line + off, sizeof(line) - off,
        ",\"unit\":\"%s\",\"current_item\":\"%s\",\"message\":\"%s\"}\n",
        unit ? unit : "", current_item ? current_item : "", message ? message : "");
    if (off >= sizeof(line)) return 0;

    int fd = open(events_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return 0;
    if (flock(fd, LOCK_EX) != 0) { close(fd); return 0; }
    size_t written = 0, len = strlen(line);
    int ok = 1;
    while (written < len) {
        ssize_t w = write(fd, line + written, len - written);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { ok = 0; break; }
        written += (size_t)w;
    }
    if (ok) ok = fsync(fd) == 0;
    flock(fd, LOCK_UN);
    close(fd);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* Verifiable-process-identity locking: a lock file records not just a pid
 * but that pid's OS-reported start time, so a stale lock left by a crashed
 * process is never confused with a live, unrelated process that later
 * reused the same pid. Comparing pid alone cannot tell these apart; pid
 * reuse is routine on a long-running machine. */

/* Returns 1 and fills *out_identity with a value that changes if-and-only-if
 * that exact pid's process is replaced by a different one (start time, in
 * platform-native units), 0 if the pid is not currently running or the
 * identity cannot be determined (treated as "cannot verify liveness" by
 * callers, not as "definitely stale"). */
static int durable_process_identity(pid_t pid, int64_t *out_identity) {
#if defined(__APPLE__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid};
    struct kinfo_proc info;
    size_t len = sizeof(info);
    if (sysctl(mib, 4, &info, &len, NULL, 0) != 0 || len == 0) return 0;
    *out_identity = (int64_t)info.kp_proc.p_starttime.tv_sec * 1000000LL +
                    (int64_t)info.kp_proc.p_starttime.tv_usec;
    return 1;
#elif defined(__linux__)
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (!n) return 0;
    buf[n] = 0;
    /* Field 22 (starttime) follows the ")" that closes the process name,
       which may itself contain spaces/parens -- skip to the last ')'. */
    char *close_paren = strrchr(buf, ')');
    if (!close_paren) return 0;
    char *cursor = close_paren + 1;
    long long value = 0;
    int field = 2; /* state is field 2, right after the name */
    while (*cursor) {
        while (*cursor == ' ') cursor++;
        char *end = cursor;
        while (*end && *end != ' ') end++;
        if (field == 22) {
            char saved = *end; *end = 0;
            value = atoll(cursor);
            *end = saved;
            *out_identity = (int64_t)value;
            return 1;
        }
        cursor = end; field++;
    }
    return 0;
#else
    (void)pid; (void)out_identity;
    return 0; /* unsupported platform: caller falls back to liveness-only */
#endif
}

/* A lock is stale (safe to break) when either:
 *   - the recorded pid is not running at all, or
 *   - the recorded pid IS running, but this platform can verify identity
 *     and the running process's start time no longer matches what was
 *     recorded -- the original holder is gone and something else reused
 *     its pid.
 * If the platform cannot verify identity (unsupported OS) and the pid is
 * alive, this conservatively reports "not stale" -- pid-alone liveness is
 * still safer than always breaking the lock. */
static int durable_lock_is_stale(pid_t recorded_pid, int64_t recorded_identity) {
    if (kill(recorded_pid, 0) != 0 && errno == ESRCH) return 1; /* not running at all */
    int64_t current_identity = 0;
    if (durable_process_identity(recorded_pid, &current_identity))
        return current_identity != recorded_identity;
    return 0; /* alive, identity unverifiable on this platform: assume held */
}

/* Writes {"pid":N,"identity":M,"acquired_at":"..."} to lock_path via the
 * atomic-write helper above. Does not itself provide mutual exclusion --
 * pair with an flock() on the same fd for that (see durable_scope_lock_*
 * below), this only records WHO holds it for stale-recovery purposes. */
static int durable_lock_record(const char *lock_path, pid_t pid, int64_t identity) {
    char now[40], text[256];
    time_t t = time(NULL);
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    strftime(now, sizeof(now), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    snprintf(text, sizeof(text), "{\"pid\":%ld,\"identity\":%lld,\"acquired_at\":\"%s\"}\n",
             (long)pid, (long long)identity, now);
    return durable_state_put(lock_path, text);
}

/* Parses a lock file written by durable_lock_record(). Returns 1 on a
 * parseable record. This is a tiny hand-rolled reader (three fixed
 * integer/string fields) rather than a json.h dependency, so this header
 * stays includable from a standalone test binary with zero other project
 * headers. */
static int durable_lock_read(const char *lock_path, pid_t *out_pid, int64_t *out_identity) {
    char *text = durable_state_get(lock_path, 4096);
    if (!text) return 0;
    long pid_val = 0; long long identity_val = 0;
    int ok = sscanf(text, "{\"pid\":%ld,\"identity\":%lld,", &pid_val, &identity_val) == 2;
    free(text);
    if (!ok) return 0;
    *out_pid = (pid_t)pid_val;
    *out_identity = (int64_t)identity_val;
    return 1;
}

/* Attempts to acquire a one-writer lock at lock_path (a plain file path,
 * typically "<job_dir>/writer.lock"). On success returns an open fd the
 * caller must keep open for the lock's duration and close (which releases
 * the flock) when done; the fd's flock is the actual mutual-exclusion
 * mechanism, robust across threads and processes. If another live,
 * verified-identity holder has it, returns -1 with errno left as EWOULDBLOCK.
 * If the existing lock file's holder is stale (per durable_lock_is_stale),
 * this recovers automatically: it still acquires the flock (which a truly
 * dead process cannot be holding) and overwrites the identity record. */
static int durable_scope_lock_acquire(const char *lock_path) {
    int fd = open(lock_path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        errno = EWOULDBLOCK;
        return -1;
    }
    /* We hold the flock, so any previous holder is provably gone (flock is
       released on process exit/crash by the kernel) -- record our own
       identity for the next stale-check regardless of what was there.
       Written directly to the already-locked fd (ftruncate + write), NOT
       via durable_lock_record()'s temp-file-plus-rename: renaming a new
       inode onto lock_path would silently swap out the file this fd's
       flock protects, leaving the lock path pointing at an unlocked inode
       and defeating mutual exclusion entirely for the next acquirer. */
    int64_t identity = 0;
    durable_process_identity(getpid(), &identity);
    char now[40], text[256];
    time_t t = time(NULL);
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    strftime(now, sizeof(now), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    int n = snprintf(text, sizeof(text), "{\"pid\":%ld,\"identity\":%lld,\"acquired_at\":\"%s\"}\n",
                     (long)getpid(), (long long)identity, now);
    if (n > 0 && ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0) {
        size_t len = (size_t)n, off = 0;
        while (off < len) {
            ssize_t w = write(fd, text + off, len - off);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) break;
            off += (size_t)w;
        }
        fsync(fd);
    }
    return fd;
}

static void durable_scope_lock_release(int fd) {
    if (fd >= 0) { flock(fd, LOCK_UN); close(fd); }
}

#endif /* DURABLE_JOB_H */
