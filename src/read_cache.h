/* read_cache.h — content-addressed cache for doc.read results (R4, decision 5).
 *
 * TASKS_READER.md: "Read results are cached centrally, content-addressed. Never
 * as companion files in user folders." The key is the SHA-256 of the file
 * bytes, so a moved/renamed file still hits and an edited file misses. The
 * cached payload is the full detail:"lines" result; a pack_fingerprint +
 * contract_version guard makes a threshold/pack/charset change a miss.
 *
 *   path:  <root>/<sha[0:2]>/<sha>.json     root default ~/.samosa/cache/read
 *   dirs 0700, files 0600, atomic write via temp + rename.
 *   entry: {"contract_version","pack_fingerprint","created",<int>,"result":<obj>}
 *
 * Header-only, self-contained (own SHA-256), reuses json.h for reads. No
 * network, nothing leaves ~/.samosa (same local trust boundary as chats).
 */
#ifndef READ_CACHE_H
#define READ_CACHE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "json.h"

/* ------------------------------- SHA-256 ---------------------------------- */
typedef struct { uint32_t h[8]; uint64_t bits; unsigned char block[64]; size_t used; } RcSha;
static uint32_t rc_rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void rc_sha_compress(RcSha *c, const unsigned char b[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u };
    uint32_t w[64], a,bb,cc,d,e,f,g,h; int i;
    for (i = 0; i < 16; i++) { const unsigned char *p = b + i*4; w[i] = ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
    for (i = 16; i < 64; i++) { uint32_t s0=rc_rotr(w[i-15],7)^rc_rotr(w[i-15],18)^(w[i-15]>>3), s1=rc_rotr(w[i-2],17)^rc_rotr(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1; }
    a=c->h[0];bb=c->h[1];cc=c->h[2];d=c->h[3];e=c->h[4];f=c->h[5];g=c->h[6];h=c->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t s1=rc_rotr(e,6)^rc_rotr(e,11)^rc_rotr(e,25), ch=(e&f)^((~e)&g), t1=h+s1+ch+k[i]+w[i];
        uint32_t s0=rc_rotr(a,2)^rc_rotr(a,13)^rc_rotr(a,22), mj=(a&bb)^(a&cc)^(bb&cc), t2=s0+mj;
        h=g;g=f;f=e;e=d+t1;d=cc;cc=bb;bb=a;a=t1+t2;
    }
    c->h[0]+=a;c->h[1]+=bb;c->h[2]+=cc;c->h[3]+=d;c->h[4]+=e;c->h[5]+=f;c->h[6]+=g;c->h[7]+=h;
}
static void rc_sha_init(RcSha *c) {
    static const uint32_t iv[8]={0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    memcpy(c->h, iv, sizeof iv); c->bits=0; c->used=0;
}
static void rc_sha_update(RcSha *c, const void *d_, size_t len) {
    const unsigned char *d=d_; c->bits+=(uint64_t)len*8;
    while (len) { size_t n=64-c->used; if (n>len) n=len; memcpy(c->block+c->used,d,n); c->used+=n; d+=n; len-=n; if (c->used==64){rc_sha_compress(c,c->block);c->used=0;} }
}
static void rc_sha_final(RcSha *c, unsigned char out[32]) {
    uint64_t bits=c->bits; int i; c->block[c->used++]=0x80;
    if (c->used>56){ while(c->used<64)c->block[c->used++]=0; rc_sha_compress(c,c->block); c->used=0; }
    while (c->used<56) c->block[c->used++]=0;
    for (i=7;i>=0;i--) c->block[c->used++]=(unsigned char)(bits>>(i*8));
    rc_sha_compress(c,c->block);
    for (i=0;i<8;i++){ out[i*4]=c->h[i]>>24; out[i*4+1]=c->h[i]>>16; out[i*4+2]=c->h[i]>>8; out[i*4+3]=(unsigned char)c->h[i]; }
}

/* SHA-256 of the file bytes -> lowercase hex[65]. Returns 0 on success. */
static int read_cache_key_file(const char *path, char hex[65]) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    RcSha c; rc_sha_init(&c);
    unsigned char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) rc_sha_update(&c, buf, n);
    fclose(f);
    unsigned char dig[32]; rc_sha_final(&c, dig);
    static const char *hx = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { hex[i*2]=hx[dig[i]>>4]; hex[i*2+1]=hx[dig[i]&15]; }
    hex[64]=0; return 0;
}

static void read_cache_default_root(char *out, size_t n) {
    const char *e = getenv("SAMOSA_READ_CACHE_DIR");
    if (e) { snprintf(out, n, "%s", e); return; }
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/.samosa/cache/read", home ? home : ".");
}

static void rc_entry_path(const char *root, const char *key, char *out, size_t n) {
    snprintf(out, n, "%s/%c%c/%s.json", root, key[0], key[1], key);
}

/* JSON-escape src into dst (bounded). */
static void rc_escape(const char *s, char *dst, size_t cap) {
    size_t o = 0;
    for (; *s && o + 8 < cap; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') { dst[o++]='\\'; dst[o++]=c; }
        else if (c == '\n') { dst[o++]='\\'; dst[o++]='n'; }
        else if (c == '\r') { dst[o++]='\\'; dst[o++]='r'; }
        else if (c == '\t') { dst[o++]='\\'; dst[o++]='t'; }
        else if (c < 0x20) { o += snprintf(dst+o, cap-o, "\\u%04x", c); }
        else dst[o++] = c;
    }
    dst[o] = 0;
}

/* Write an entry atomically. result_json is the raw detail:"lines" JSON object.
 * Returns 0 on success. */
static int read_cache_put(const char *root, const char *key, const char *contract,
                          const char *fingerprint, const char *result_json) {
    char shard[1200]; snprintf(shard, sizeof shard, "%s/%c%c", root, key[0], key[1]);
    char base[1024]; snprintf(base, sizeof base, "%s", root);
    /* mkdir -p root then shard, 0700 */
    for (char *p = base + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(base, 0700); *p = '/'; }
    mkdir(base, 0700); mkdir(shard, 0700);
    char path[1400], tmp[1500];
    rc_entry_path(root, key, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "wb"); if (!f) { close(fd); return -1; }
    char *esc = malloc(strlen(result_json) * 6 + 16);
    rc_escape(result_json, esc, strlen(result_json) * 6 + 16);
    fprintf(f, "{\"contract_version\":\"%s\",\"pack_fingerprint\":\"%s\",\"created\":%ld,\"result\":\"%s\"}",
            contract, fingerprint, (long)time(NULL), esc);
    free(esc); fclose(f);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    chmod(path, 0600);
    return 0;
}

/* Return the cached result JSON (malloc'd) on a fingerprint+contract match,
 * else NULL (miss). Caller frees. */
static char *read_cache_get(const char *root, const char *key, const char *contract,
                            const char *fingerprint) {
    char path[1400]; rc_entry_path(root, key, path, sizeof path);
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *txt = malloc(n + 1); if (!txt) { fclose(f); return NULL; }
    if (fread(txt, 1, n, f) != (size_t)n) { fclose(f); free(txt); return NULL; }
    txt[n] = 0; fclose(f);
    char *arena = NULL; jval *o = json_parse(txt, &arena); free(txt);
    if (!o || o->t != J_OBJ) { json_free(o); free(arena); return NULL; }
    jval *cv = json_get(o, "contract_version"), *fp = json_get(o, "pack_fingerprint"), *rs = json_get(o, "result");
    char *result = NULL;
    if (cv && cv->t == J_STR && fp && fp->t == J_STR && rs && rs->t == J_STR &&
        !strcmp(cv->str, contract) && !strcmp(fp->str, fingerprint))
        result = strdup(rs->str);
    json_free(o); free(arena);
    return result;
}

#endif /* READ_CACHE_H */
