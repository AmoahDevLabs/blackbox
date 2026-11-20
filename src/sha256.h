#ifndef BLACKBOX_SHA256_H
#define BLACKBOX_SHA256_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    unsigned char block[64];
    size_t blocklen;
} bb_sha256_ctx;

void bb_sha256_init(bb_sha256_ctx *ctx);
void bb_sha256_update(bb_sha256_ctx *ctx, const unsigned char *data, size_t len);
void bb_sha256_final(bb_sha256_ctx *ctx, unsigned char digest[32]);
void bb_sha256_hex(const unsigned char digest[32], char hex[65]);
bool bb_sha256_file(const char *path, unsigned char digest[32]);
bool bb_sha256_file_hex(const char *path, char hex[65]);

#endif /* BLACKBOX_SHA256_H */
