#define _POSIX_C_SOURCE 200809L

#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr(uint32_t x, unsigned int n)
{
    return (x >> n) | (x << (32u - n));
}

static uint32_t big_endian32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24u) | ((uint32_t)p[1] << 16u) |
           ((uint32_t)p[2] << 8u) | (uint32_t)p[3];
}

static void compress(bb_sha256_ctx *ctx, const unsigned char block[64])
{
    uint32_t w[64];
    for (size_t i = 0u; i < 16u; i++) {
        w[i] = big_endian32(block + i * 4u);
    }
    for (size_t i = 16u; i < 64u; i++) {
        uint32_t s0 = rotr(w[i - 15u], 7u) ^ rotr(w[i - 15u], 18u) ^ (w[i - 15u] >> 3u);
        uint32_t s1 = rotr(w[i - 2u], 17u) ^ rotr(w[i - 2u], 19u) ^ (w[i - 2u] >> 10u);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (size_t i = 0u; i < 64u; i++) {
        uint32_t s1 = rotr(e, 6u) ^ rotr(e, 11u) ^ rotr(e, 25u);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + K[i] + w[i];
        uint32_t s0 = rotr(a, 2u) ^ rotr(a, 13u) ^ rotr(a, 22u);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void bb_sha256_init(bb_sha256_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0u;
    ctx->blocklen = 0u;
}

void bb_sha256_update(bb_sha256_ctx *ctx, const unsigned char *data, size_t len)
{
    if (ctx == NULL || data == NULL) {
        return;
    }
    ctx->bitlen += (uint64_t)len * 8u;

    while (len > 0u) {
        size_t room = 64u - ctx->blocklen;
        size_t take = len < room ? len : room;
        memcpy(ctx->block + ctx->blocklen, data, take);
        ctx->blocklen += take;
        data += take;
        len -= take;
        if (ctx->blocklen == 64u) {
            compress(ctx, ctx->block);
            ctx->blocklen = 0u;
        }
    }
}

void bb_sha256_final(bb_sha256_ctx *ctx, unsigned char digest[32])
{
    if (ctx == NULL || digest == NULL) {
        return;
    }
    ctx->block[ctx->blocklen++] = 0x80u;

    if (ctx->blocklen > 56u) {
        memset(ctx->block + ctx->blocklen, 0, 64u - ctx->blocklen);
        compress(ctx, ctx->block);
        ctx->blocklen = 0u;
    }
    memset(ctx->block + ctx->blocklen, 0, 56u - ctx->blocklen);

    uint64_t bits = ctx->bitlen;
    for (size_t i = 0u; i < 8u; i++) {
        ctx->block[63u - i] = (unsigned char)(bits >> (8u * i));
    }
    compress(ctx, ctx->block);

    for (size_t i = 0u; i < 8u; i++) {
        digest[i * 4u] = (unsigned char)(ctx->state[i] >> 24u);
        digest[i * 4u + 1u] = (unsigned char)(ctx->state[i] >> 16u);
        digest[i * 4u + 2u] = (unsigned char)(ctx->state[i] >> 8u);
        digest[i * 4u + 3u] = (unsigned char)ctx->state[i];
    }
}

void bb_sha256_hex(const unsigned char digest[32], char hex[65])
{
    if (digest == NULL || hex == NULL) {
        return;
    }
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0u; i < 32u; i++) {
        hex[i * 2u] = digits[digest[i] >> 4u];
        hex[i * 2u + 1u] = digits[digest[i] & 0x0fu];
    }
    hex[64] = '\0';
}

bool bb_sha256_file(const char *path, unsigned char digest[32])
{
    if (path == NULL || digest == NULL) {
        return false;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    bb_sha256_ctx ctx;
    bb_sha256_init(&ctx);
    unsigned char buf[65536];
    bool ok = true;

    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        bb_sha256_update(&ctx, buf, (size_t)n);
    }
    if (close(fd) != 0) {
        ok = false;
    }
    if (!ok) {
        return false;
    }
    bb_sha256_final(&ctx, digest);
    return true;
}

bool bb_sha256_file_hex(const char *path, char hex[65])
{
    unsigned char digest[32];
    if (!bb_sha256_file(path, digest)) {
        return false;
    }
    bb_sha256_hex(digest, hex);
    return true;
}
