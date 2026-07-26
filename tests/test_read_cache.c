/* Offline test for src/read_cache.h (R4, decision 5): the content-addressed
 * read cache. Proves the "read once per file content, ever" property and the
 * fingerprint/contract guard. No network, no model, no gateway. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../src/read_cache.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)

static void write_file(const char *p, const char *s) { FILE *f = fopen(p, "wb"); fwrite(s, 1, strlen(s), f); fclose(f); }

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

    printf(fails ? "read-cache-test: FAIL (%d)\n" : "read-cache-test: PASS\n", fails);
    return fails ? 1 : 0;
}
