/* Offline test for src/read_cache.h (R4, decision 5): the content-addressed
 * read cache. Proves the "read once per file content, ever" property, the
 * fingerprint/contract guard, and (T0.3, docs/TASKS_UI_CHUTNI.md) one-writer
 * locking, atomic durable publication, and bounded pruning. No network, no
 * model, no gateway. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "../src/read_cache.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)

static void write_file(const char *p, const char *s) { FILE *f = fopen(p, "wb"); fwrite(s, 1, strlen(s), f); fclose(f); }

#define RACE_THREADS 8

typedef struct { const char *root; const char *key; int id; } RcRaceArg;

/* Each thread publishes a large (~9000 byte), fully distinguishable payload
 * to the SAME key. Before the per-shard flock in read_cache_put(), two
 * threads sharing one pid computed the same ".tmp.<pid>" name and could
 * truncate/interleave each other's writes; whichever rename() landed last
 * then published a corrupted file. With the lock, the result must be
 * byte-identical to exactly one writer's complete payload. */
static void *rc_race_worker(void *opaque) {
    RcRaceArg *a = (RcRaceArg *)opaque;
    char payload[9000];
    int o = snprintf(payload, sizeof payload, "{\"writer\":%d,\"filler\":\"", a->id);
    while (o < 8900) payload[o++] = (char)('A' + (a->id % 26));
    o += snprintf(payload + o, sizeof payload - o, "\"}");
    read_cache_put(a->root, a->key, "reader-v0", "fp_v1", payload);
    return NULL;
}

int main(void) {
    char root[512], tmpl[] = "/tmp/rc_testXXXXXX";
    char *base = mkdtemp(tmpl);
    snprintf(root, sizeof root, "%s/cache", base);

    char fa[600], fb[600];
    snprintf(fa, sizeof fa, "%s/a.png", base);
    snprintf(fb, sizeof fb, "%s/b.png", base);
    write_file(fa, "PIXELS-AAAA");
    write_file(fb, "PIXELS-BBBB");

    char ka[65], kb[65], ka2[65];
    CHECK(read_cache_key_file(fa, ka) == 0, "key of a");
    CHECK(read_cache_key_file(fb, kb) == 0, "key of b");
    CHECK(strcmp(ka, kb) != 0, "different bytes -> different keys");

    /* content-addressing: same bytes at a different path -> same key */
    char fc[600]; snprintf(fc, sizeof fc, "%s/moved_a.png", base); write_file(fc, "PIXELS-AAAA");
    CHECK(read_cache_key_file(fc, ka2) == 0 && strcmp(ka, ka2) == 0, "moved/renamed file hits same key");

    /* known SHA-256 vector: sha256("abc") */
    char fh[600]; snprintf(fh, sizeof fh, "%s/abc.txt", base); write_file(fh, "abc");
    char kh[65]; read_cache_key_file(fh, kh);
    CHECK(strcmp(kh, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "sha256(\"abc\") vector");

    const char *FP = "fp_v1", *CT = "reader-v0";
    const char *result = "{\"page_count\":1,\"text\":\"hello\",\"needs_review\":false}";

    CHECK(read_cache_get(root, ka, CT, FP) == NULL, "cold miss");
    CHECK(read_cache_put(root, ka, CT, FP, result) == 0, "put");
    char *got = read_cache_get(root, ka, CT, FP);
    CHECK(got && strcmp(got, result) == 0, "hit returns exact result"); free(got);

    /* fingerprint mismatch = miss (pack/threshold change) */
    CHECK(read_cache_get(root, ka, CT, "fp_v2") == NULL, "fingerprint mismatch -> miss");
    /* contract mismatch = miss */
    CHECK(read_cache_get(root, ka, "reader-v1", FP) == NULL, "contract mismatch -> miss");
    /* a stale fingerprint doesn't just miss -- re-putting under a new
       fingerprint (simulating re-extraction) makes the new one hit. */
    CHECK(read_cache_put(root, ka, CT, "fp_v2", result) == 0, "re-put under new fingerprint");
    char *got_new = read_cache_get(root, ka, CT, "fp_v2");
    CHECK(got_new && strcmp(got_new, result) == 0, "stale parser fingerprint triggers re-extraction (new fp hits)");
    free(got_new);
    CHECK(read_cache_get(root, ka, CT, FP) == NULL, "old fingerprint is now a miss after re-put");

    /* result with quotes/newlines round-trips through escaping */
    const char *tricky = "{\"text\":\"a \\\"quote\\\" and\\nnewline\",\"conf\":0.9}";
    CHECK(read_cache_put(root, kb, CT, FP, tricky) == 0, "put tricky");
    char *g2 = read_cache_get(root, kb, CT, FP);
    CHECK(g2 && strcmp(g2, tricky) == 0, "tricky result round-trips"); free(g2);

    /* perms: entry file 0600, shard dir 0700 */
    char entry[700], shard[700];
    rc_entry_path(root, ka, entry, sizeof entry);
    snprintf(shard, sizeof shard, "%s/%c%c", root, ka[0], ka[1]);
    struct stat st;
    CHECK(stat(entry, &st) == 0 && (st.st_mode & 0777) == 0600, "entry file mode 0600");
    CHECK(stat(shard, &st) == 0 && (st.st_mode & 0777) == 0700, "shard dir mode 0700");

    /* entry lives under the cache root, not beside the user's file */
    CHECK(strstr(entry, base) == entry && strstr(entry, "/a.png") == NULL, "cache entry not a companion file");

    /* --- T0.3: concurrent same-key writers must never corrupt the entry --- */
    {
        char race_root[600]; snprintf(race_root, sizeof race_root, "%s/race_cache", base);
        RcRaceArg args[RACE_THREADS];
        pthread_t threads[RACE_THREADS];
        char *candidates[RACE_THREADS];

        for (int i = 0; i < RACE_THREADS; i++) {
            candidates[i] = malloc(9000);
            int o = snprintf(candidates[i], 9000, "{\"writer\":%d,\"filler\":\"", i);
            while (o < 8900) candidates[i][o++] = (char)('A' + (i % 26));
            o += snprintf(candidates[i] + o, 9000 - o, "\"}");
            args[i].root = race_root; args[i].key = ka; args[i].id = i;
        }
        for (int i = 0; i < RACE_THREADS; i++)
            pthread_create(&threads[i], NULL, rc_race_worker, &args[i]);
        for (int i = 0; i < RACE_THREADS; i++)
            pthread_join(threads[i], NULL);

        char *raced = read_cache_get(race_root, ka, "reader-v0", "fp_v1");
        int matched = 0;
        if (raced) for (int i = 0; i < RACE_THREADS; i++) if (!strcmp(raced, candidates[i])) matched = 1;
        CHECK(raced != NULL, "concurrent writers: cache has a published entry");
        CHECK(matched, "concurrent writers: published entry exactly matches one writer, not a corrupted mix");
        free(raced);
        for (int i = 0; i < RACE_THREADS; i++) free(candidates[i]);
    }

    /* --- T0.3: size accounting + bounded pruning, respecting .pin --- */
    {
        char prune_root[600]; snprintf(prune_root, sizeof prune_root, "%s/prune_cache", base);
        const char *keys[3] = {
            "1111111111111111111111111111111111111111111111111111111111111a",
            "2222222222222222222222222222222222222222222222222222222222222b",
            "3333333333333333333333333333333333333333333333333333333333333c",
        };
        char payload[2000];
        memset(payload, 'x', sizeof payload - 1); payload[sizeof payload - 1] = 0;
        char body[2100]; snprintf(body, sizeof body, "{\"pad\":\"%s\"}", payload);

        for (int i = 0; i < 3; i++)
            CHECK(read_cache_put(prune_root, keys[i], CT, FP, body) == 0, "prune fixture: put entry");

        /* Force a deterministic age ordering: key[0] oldest, key[2] newest. */
        for (int i = 0; i < 3; i++) {
            char entry_path[700];
            rc_entry_path(prune_root, keys[i], entry_path, sizeof entry_path);
            struct timeval tv[2];
            time_t when = 1700000000 + i * 1000;
            tv[0].tv_sec = tv[1].tv_sec = when; tv[0].tv_usec = tv[1].tv_usec = 0;
            utimes(entry_path, tv);
        }
        /* Pin the OLDEST entry -- pruning must skip it despite its age. */
        {
            char pin_path[700];
            rc_entry_path(prune_root, keys[0], pin_path, sizeof pin_path);
            size_t n = strlen(pin_path);
            pin_path[n - 5] = 0; /* strip ".json" */
            strcat(pin_path, ".pin");
            FILE *pf = fopen(pin_path, "wb"); if (pf) fclose(pf);
        }

        long long before = read_cache_total_bytes(prune_root);
        CHECK(before > 0, "read_cache_total_bytes: nonzero before pruning");

        /* Budget forces at least one removal, but leaves room for the pinned
           oldest entry to legitimately survive. */
        int removed = read_cache_prune(prune_root, before / 3);
        CHECK(removed >= 1, "read_cache_prune: removed at least one entry");

        char entry0[700], entry1[700];
        rc_entry_path(prune_root, keys[0], entry0, sizeof entry0);
        rc_entry_path(prune_root, keys[1], entry1, sizeof entry1);
        struct stat st0, st1;
        CHECK(stat(entry0, &st0) == 0, "read_cache_prune: pinned oldest entry survives");
        CHECK(stat(entry1, &st1) != 0, "read_cache_prune: unpinned older entry was removed first");

        long long after = read_cache_total_bytes(prune_root);
        CHECK(after < before, "read_cache_prune: total bytes decreased");
    }

    printf(fails ? "read-cache-test: FAIL (%d)\n" : "read-cache-test: PASS\n", fails);
    return fails ? 1 : 0;
}
