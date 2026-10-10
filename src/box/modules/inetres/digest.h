/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* digest.h -- the message digests of *MD5 and its family (digest.c). */
#ifndef INETRES_DIGEST_H
#define INETRES_DIGEST_H

#include <stddef.h>
#include <stdint.h>

/* The Merkle-Damgard digests: a block buffer, a byte count, and the chaining
 * words, 32 or 64 bits wide. */
struct md {
    union {
        uint32_t h32[8];
        uint64_t h64[8];
    };
    uint64_t count;
    uint8_t buf[128];
    size_t used;
};

/* Skein: Threefish's chaining words, the message's position, and a block
 * held back until it is known whether it is the last. */
struct skein {
    uint64_t g[16];
    uint64_t pos;
    uint8_t buf[128];
    size_t used;
    int nw, first;
    unsigned bits;
};

union digest_ctx {
    struct md md;
    struct skein skein;
};

struct digest {
    const char *prog;               /* "sha256": the command's name */
    const char *name;               /* "SHA256": the output's */
    unsigned len;                   /* bytes */
    const char *const *tests;       /* FreeBSD's test suite's answers, 8 */
    void (*init)(union digest_ctx *c);
    void (*update)(union digest_ctx *c, const uint8_t *p, size_t n);
    void (*final)(union digest_ctx *c, uint8_t *out);
};

/* Every digest, ending with one whose prog is NULL */
extern const struct digest digests[];
extern const char *const digest_test_input[8];

/* The digest, finished, as lower-case hex: 2 len + 1 bytes */
void digest_hex(const struct digest *d, union digest_ctx *c, char *hex);

#endif
