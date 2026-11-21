/* Mechanical SHA-256 primitive copied from legacy samosa_fs.c, not its policy.
 * Owned locally; no private source/include dependency on the old gateway. */
#ifndef SOURCES_SHA256_H
#define SOURCES_SHA256_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef struct { uint32_t h[8]; uint64_t bits; unsigned char block[64]; size_t used; } Sha256;
static uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_compress(Sha256 *ctx, const unsigned char block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; ++i) {
        const unsigned char *p = block + i * 4;
        w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }
    for (i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
    e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];
    for (i = 0; i < 64; ++i) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;
        h = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

static void sha256_init(Sha256 *ctx) {
    static const uint32_t init[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    memcpy(ctx->h, init, sizeof(init));
    ctx->bits = 0;
    ctx->used = 0;
}

static void sha256_update(Sha256 *ctx, const void *data_, size_t len) {
    const unsigned char *data = (const unsigned char *)data_;
    ctx->bits += (uint64_t)len * 8u;
    while (len) {
        size_t n = 64u - ctx->used;
        if (n > len) n = len;
        memcpy(ctx->block + ctx->used, data, n);
        ctx->used += n;
        data += n;
        len -= n;
        if (ctx->used == 64u) {
            sha256_compress(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

static void sha256_final(Sha256 *ctx, unsigned char out[32]) {
    uint64_t bits = ctx->bits;
    int i;
    ctx->block[ctx->used++] = 0x80u;
    if (ctx->used > 56u) {
        while (ctx->used < 64u) ctx->block[ctx->used++] = 0;
        sha256_compress(ctx, ctx->block);
        ctx->used = 0;
    }
    while (ctx->used < 56u) ctx->block[ctx->used++] = 0;
    for (i = 7; i >= 0; --i)
        ctx->block[ctx->used++] = (unsigned char)(bits >> (i * 8));
    sha256_compress(ctx, ctx->block);
    for (i = 0; i < 8; ++i) {
        out[i * 4] = (unsigned char)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)ctx->h[i];
    }
}

static void sha256_hex(const unsigned char digest[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[64] = '\0';
}

static void hash_bytes(const void *bytes, size_t length, char output[65]) {
    Sha256 ctx; unsigned char digest[32];
    sha256_init(&ctx); sha256_update(&ctx, bytes, length);
    sha256_final(&ctx, digest); sha256_hex(digest, output);
}
#endif
