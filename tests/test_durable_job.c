/* Offline test for src/durable_job.h (T0.4, docs/TASKS_UI_CHUTNI.md): the
 * generalized durable background-operation primitive shared by future model
 * downloads (T2.2) and Chutni builds (T4.x) -- path-jailed job directories,
 * atomic state, sequenced append-only events matching the frozen §5.7
 * contract, and one-writer locks with stale-lock recovery based on
 * verifiable process identity, not PID alone. No network, no gateway. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../src/durable_job.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)

int main(void) {
    char base[512], tmpl[] = "/tmp/durable_job_testXXXXXX";
    char *root = mkdtemp(tmpl);
    snprintf(base, sizeof(base), "%s", root);

    /* --- path jail: durable_job_dir / durable_id_valid --- */
    CHECK(!durable_id_valid(""), "empty id rejected");
    CHECK(!durable_id_valid("../etc"), "dotdot id rejected");
    CHECK(!durable_id_valid("a/b"), "slash id rejected");
    CHECK(durable_id_valid("job-123_ABC"), "alnum-dash-underscore id accepted");

    char job_dir[PATH_MAX];
    CHECK(durable_job_dir(base, "job-one", job_dir, 1), "durable_job_dir creates directory");
    struct stat st;
    CHECK(stat(job_dir, &st) == 0 && S_ISDIR(st.st_mode), "job directory actually exists");
    CHECK(!durable_job_dir(base, "../escape", job_dir, 1), "durable_job_dir rejects escaping id");

    /* Same primitive, different caller root -- proves it's not hardcoded to
       one gateway-wide jobs tree the way job_state_path() is. */
    char alt_base[600]; snprintf(alt_base, sizeof(alt_base), "%s/models-root", base);
    char alt_dir[PATH_MAX];
    CHECK(durable_job_dir(alt_base, "install-job-1", alt_dir, 1), "same primitive works under a different base root");
    CHECK(strstr(alt_dir, "models-root") != NULL, "job dir lives under the caller's chosen root");

    /* --- atomic state --- */
    char state_path[PATH_MAX + 16];
    snprintf(state_path, sizeof(state_path), "%s/job.json", job_dir);
    CHECK(durable_state_put(state_path, "{\"state\":\"queued\"}\n"), "durable_state_put succeeds");
    char *state = durable_state_get(state_path, 4096);
    CHECK(state && strstr(state, "\"state\":\"queued\"") != NULL, "durable_state_get round-trips");
    free(state);
    CHECK(stat(state_path, &st) == 0 && (st.st_mode & 0777) == 0600, "state file mode 0600");
    /* oversized read is a hard failure, never a silent truncation */
    CHECK(durable_state_get(state_path, 4) == NULL, "durable_state_get refuses to silently truncate");

    /* --- durable events: frozen §5.7 shape --- */
    char events_path[PATH_MAX + 16];
    snprintf(events_path, sizeof(events_path), "%s/events.jsonl", job_dir);
    long completed = 10, total = 100;
    CHECK(durable_event_append(events_path, 1, "job-one", "chutni_build", "running",
                               "extracting", &completed, &total, "files", "a/b.pdf", "Reading documents"),
         "durable_event_append (known total)");
    CHECK(durable_event_append(events_path, 2, "job-one", "chutni_build", "running",
                               "extracting", NULL, NULL, "files", "c.pdf", "Reading documents"),
         "durable_event_append (unknown total -> null)");
    char *events = durable_state_get(events_path, 1 << 16);
    CHECK(events && strstr(events, "\"seq\":1") != NULL, "event 1 present");
    CHECK(events && strstr(events, "\"seq\":2") != NULL, "event 2 present");
    CHECK(events && strstr(events, "\"total\":100") != NULL, "known total serialized as a number");
    CHECK(events && strstr(events, "\"total\":null") != NULL, "unknown total serialized as null, not fabricated");
    CHECK(events && strstr(events, "\"kind\":\"chutni_build\"") != NULL, "kind field present");
    CHECK(events && strstr(events, "\"job_id\":\"job-one\"") != NULL, "job_id field present");
    /* both lines complete and present -- append-only, no line lost or torn */
    int newline_count = 0;
    for (char *p = events; *p; p++) if (*p == '\n') newline_count++;
    CHECK(newline_count == 2, "exactly two complete event lines");
    free(events);

    /* --- one-writer lock: real mutual exclusion via flock --- */
    char lock_path[PATH_MAX + 16];
    snprintf(lock_path, sizeof(lock_path), "%s/writer.lock", job_dir);
    int fd1 = durable_scope_lock_acquire(lock_path);
    CHECK(fd1 >= 0, "first acquire succeeds");
    int fd2 = durable_scope_lock_acquire(lock_path);
    CHECK(fd2 < 0, "second concurrent acquire is rejected while the first is held");
    durable_scope_lock_release(fd1);
    int fd3 = durable_scope_lock_acquire(lock_path);
    CHECK(fd3 >= 0, "acquire succeeds again after release");
    durable_scope_lock_release(fd3);

    /* --- stale-lock recovery based on verifiable identity, not PID alone --- */
    pid_t child = fork();
    if (child == 0) {
        /* Write our own (soon-to-be-stale) lock record, then exit immediately. */
        int64_t self_identity = 0;
        durable_process_identity(getpid(), &self_identity);
        durable_lock_record(lock_path, getpid(), self_identity);
        _exit(0);
    }
    int wstatus = 0;
    waitpid(child, &wstatus, 0);
    CHECK(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0, "helper child recorded its lock and exited");

    pid_t dead_pid; int64_t dead_identity;
    CHECK(durable_lock_read(lock_path, &dead_pid, &dead_identity), "lock record readable after child exit");
    CHECK(durable_lock_is_stale(dead_pid, dead_identity), "a lock held by a now-exited process is detected as stale");

    /* The critical case: same pid as us (definitely alive), but the WRONG
       identity -- simulates the original holder having crashed and this
       exact pid number later being reused by an unrelated live process.
       A PID-only check ("is this pid alive?") would wrongly say "still
       held"; identity verification must say "stale" instead. */
    int64_t my_identity = 0;
    CHECK(durable_process_identity(getpid(), &my_identity), "durable_process_identity resolves our own pid");
    CHECK(!durable_lock_is_stale(getpid(), my_identity), "our own live, correctly-identified lock is NOT stale");
    CHECK(durable_lock_is_stale(getpid(), my_identity ^ 0x1), "same live pid but mismatched identity IS stale (not PID alone)");

    printf(fails ? "durable-job-test: FAIL (%d)\n" : "durable-job-test: PASS\n", fails);
    return fails ? 1 : 0;
}
