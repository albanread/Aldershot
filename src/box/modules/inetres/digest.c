/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* digest.c -- the message digests that *MD5 and its family compute: MD5,
 * SHA-1, SHA-224, SHA-256, SHA-384, SHA-512, SHA-512/256, RIPEMD-160 and
 * Skein (256, 512 and 1024), as FreeBSD's md5(1) offers them.
 *
 * Written from the standards (RFC 1321, FIPS 180-4, the RIPEMD-160 paper,
 * Skein 1.3). FreeBSD's test suite (md5.c, -x) checks each of them. It is
 * portable C with no host library, because the box has none and the
 * digests must be the same hosted and in the box.
 */
#include <string.h>

#include "digest.h"

static uint32_t rol32(uint32_t x, unsigned n)
{
    return x << n | x >> (32 - n);
}

static uint32_t ror32(uint32_t x, unsigned n)
{
    return x >> n | x << (32 - n);
}

static uint64_t rol64(uint64_t x, unsigned n)
{
    return x << n | x >> (64 - n);
}

static uint64_t ror64(uint64_t x, unsigned n)
{
    return x >> n | x << (64 - n);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static uint64_t be64(const uint8_t *p)
{
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> 8 * i);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (24 - 8 * i));
}

static void put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> 8 * i);
}

static void put_be64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (56 - 8 * i));
}

/* ---- the Merkle-Damgard digests: a block buffer and a byte count ----------- */

typedef void block_fn(union digest_ctx *c, const uint8_t *block);

static void md_update(union digest_ctx *c, const uint8_t *p, size_t n, size_t size, block_fn *block)
{
    struct md *m = &c->md;
    m->count += n;
    while (n) {
        size_t take = size - m->used < n ? size - m->used : n;
        memcpy(m->buf + m->used, p, take);
        m->used += take, p += take, n -= take;
        if (m->used == size) {
            block(c, m->buf);
            m->used = 0;
        }
    }
}

/* 0x80, zeros, and the length in bits: little- or big-endian, in a field of
 * 8 bytes (64-byte blocks) or 16 (128-byte blocks). */
static void md_pad(union digest_ctx *c, size_t size, int big, block_fn *block)
{
    struct md *m = &c->md;
    uint64_t bits = m->count << 3, high = m->count >> 61;
    size_t field = size == 128 ? 16 : 8;
    m->buf[m->used++] = 0x80;
    if (m->used > size - field) {
        memset(m->buf + m->used, 0, size - m->used);
        block(c, m->buf);
        m->used = 0;
    }
    memset(m->buf + m->used, 0, size - m->used);
    if (big) {
        put_be64(m->buf + size - 8, bits);
        if (field == 16)
            put_be64(m->buf + size - 16, high);
    } else {
        put_le64(m->buf + size - 8, bits);
    }
    block(c, m->buf);
}

/* ---- MD5 (RFC 1321) ------------------------------------------------------ */

static const uint32_t md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const uint8_t md5_s[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };

static void md5_block(union digest_ctx *c, const uint8_t *p)
{
    uint32_t x[16], *h = c->md.h32;
    for (int i = 0; i < 16; i++)
        x[i] = le32(p + 4 * i);
    uint32_t a = h[0], b = h[1], cc = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        switch (i >> 4) {
        case 0: f = (b & cc) | (~b & d); g = i; break;
        case 1: f = (d & b) | (~d & cc); g = (5 * i + 1) & 15; break;
        case 2: f = b ^ cc ^ d; g = (3 * i + 5) & 15; break;
        default: f = cc ^ (b | ~d); g = (7 * i) & 15; break;
        }
        uint32_t t = d;
        d = cc, cc = b;
        b = b + rol32(a + f + md5_k[i] + x[g], md5_s[(i >> 4) * 4 + (i & 3)]);
        a = t;
    }
    h[0] += a, h[1] += b, h[2] += cc, h[3] += d;
}

static void md5_init(union digest_ctx *c)
{
    memset(c, 0, sizeof *c);
    c->md.h32[0] = 0x67452301, c->md.h32[1] = 0xefcdab89, c->md.h32[2] = 0x98badcfe, c->md.h32[3] = 0x10325476;
}

static void md5_update(union digest_ctx *c, const uint8_t *p, size_t n)
{
    md_update(c, p, n, 64, md5_block);
}

static void md5_final(union digest_ctx *c, uint8_t *out)
{
    md_pad(c, 64, 0, md5_block);
    for (int i = 0; i < 4; i++)
        put_le32(out + 4 * i, c->md.h32[i]);
}

/* ---- SHA-1 (FIPS 180-4) ---------------------------------------------------- */

static void sha1_block(union digest_ctx *c, const uint8_t *p)
{
    uint32_t w[80], *h = c->md.h32;
    for (int i = 0; i < 16; i++)
        w[i] = be32(p + 4 * i);
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], cc = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)
            f = (b & cc) | (~b & d), k = 0x5a827999;
        else if (i < 40)
            f = b ^ cc ^ d, k = 0x6ed9eba1;
        else if (i < 60)
            f = (b & cc) | (b & d) | (cc & d), k = 0x8f1bbcdc;
        else
            f = b ^ cc ^ d, k = 0xca62c1d6;
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d, d = cc, cc = rol32(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += cc, h[3] += d, h[4] += e;
}

static void sha1_init(union digest_ctx *c)
{
    static const uint32_t iv[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    memset(c, 0, sizeof *c);
    memcpy(c->md.h32, iv, sizeof iv);
}

static void sha1_update(union digest_ctx *c, const uint8_t *p, size_t n)
{
    md_update(c, p, n, 64, sha1_block);
}

static void sha1_final(union digest_ctx *c, uint8_t *out)
{
    md_pad(c, 64, 1, sha1_block);
    for (int i = 0; i < 5; i++)
        put_be32(out + 4 * i, c->md.h32[i]);
}

/* ---- SHA-224 and SHA-256 ----------------------------------------------------- */

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_block(union digest_ctx *c, const uint8_t *p)
{
    uint32_t w[64], v[8], *h = c->md.h32;
    for (int i = 0; i < 16; i++)
        w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(v, h, sizeof v);
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = ror32(v[4], 6) ^ ror32(v[4], 11) ^ ror32(v[4], 25);
        uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint32_t t1 = v[7] + s1 + ch + sha256_k[i] + w[i];
        uint32_t s0 = ror32(v[0], 2) ^ ror32(v[0], 13) ^ ror32(v[0], 22);
        uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        memmove(v + 1, v, 7 * sizeof v[0]);
        v[4] += t1;
        v[0] = t1 + s0 + maj;
    }
    for (int i = 0; i < 8; i++)
        h[i] += v[i];
}

static void sha256_init(union digest_ctx *c)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memset(c, 0, sizeof *c);
    memcpy(c->md.h32, iv, sizeof iv);
}

static void sha224_init(union digest_ctx *c)
{
    static const uint32_t iv[8] = { 0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939,
                                    0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4 };
    memset(c, 0, sizeof *c);
    memcpy(c->md.h32, iv, sizeof iv);
}

static void sha256_update(union digest_ctx *c, const uint8_t *p, size_t n)
{
    md_update(c, p, n, 64, sha256_block);
}

static void sha256_final(union digest_ctx *c, uint8_t *out)
{
    md_pad(c, 64, 1, sha256_block);
    for (int i = 0; i < 8; i++)
        put_be32(out + 4 * i, c->md.h32[i]);
}

static void sha224_final(union digest_ctx *c, uint8_t *out)
{
    md_pad(c, 64, 1, sha256_block);
    for (int i = 0; i < 7; i++)
        put_be32(out + 4 * i, c->md.h32[i]);
}

/* ---- SHA-384, SHA-512 and SHA-512/256 ------------------------------------------ */

static const uint64_t sha512_k[80] = {
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538,
    0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242, 0x12835b0145706fbe,
    0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2, 0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235,
    0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65,
    0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5, 0x983e5152ee66dfab,
    0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725,
    0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed,
    0x53380d139d95b3df, 0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
    0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218,
    0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8, 0x19a4c116b8d2d0c8, 0x1e376c085141ab53,
    0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373,
    0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b, 0xca273eceea26619c,
    0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6,
    0x113f9804bef90dae, 0x1b710b35131c471b, 0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc,
    0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817,
};

static void sha512_block(union digest_ctx *c, const uint8_t *p)
{
    uint64_t w[80], v[8], *h = c->md.h64;
    for (int i = 0; i < 16; i++)
        w[i] = be64(p + 8 * i);
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(v, h, sizeof v);
    for (int i = 0; i < 80; i++) {
        uint64_t s1 = ror64(v[4], 14) ^ ror64(v[4], 18) ^ ror64(v[4], 41);
        uint64_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint64_t t1 = v[7] + s1 + ch + sha512_k[i] + w[i];
        uint64_t s0 = ror64(v[0], 28) ^ ror64(v[0], 34) ^ ror64(v[0], 39);
        uint64_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        memmove(v + 1, v, 7 * sizeof v[0]);
        v[4] += t1;
        v[0] = t1 + s0 + maj;
    }
    for (int i = 0; i < 8; i++)
        h[i] += v[i];
}

static void sha512_iv(union digest_ctx *c, const uint64_t iv[8])
{
    memset(c, 0, sizeof *c);
    memcpy(c->md.h64, iv, 8 * sizeof iv[0]);
}

static void sha512_init(union digest_ctx *c)
{
    static const uint64_t iv[8] = { 0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b,
                                    0xa54ff53a5f1d36f1, 0x510e527fade682d1, 0x9b05688c2b3e6c1f,
                                    0x1f83d9abfb41bd6b, 0x5be0cd19137e2179 };
    sha512_iv(c, iv);
}

static void sha384_init(union digest_ctx *c)
{
    static const uint64_t iv[8] = { 0xcbbb9d5dc1059ed8, 0x629a292a367cd507, 0x9159015a3070dd17,
                                    0x152fecd8f70e5939, 0x67332667ffc00b31, 0x8eb44a8768581511,
                                    0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4 };
    sha512_iv(c, iv);
}

static void sha512t256_init(union digest_ctx *c)
{
    static const uint64_t iv[8] = { 0x22312194fc2bf72c, 0x9f555fa3c84c64c2, 0x2393b86b6f53b151,
                                    0x963877195940eabd, 0x96283ee2a88effe3, 0xbe5e1e2553863992,
                                    0x2b0199fc2c85b8aa, 0x0eb72ddc81c52ca2 };
    sha512_iv(c, iv);
}

static void sha512_update(union digest_ctx *c, const uint8_t *p, size_t n)
{
    md_update(c, p, n, 128, sha512_block);
}

static void sha512_out(union digest_ctx *c, uint8_t *out, unsigned bytes)
{
    uint8_t all[64];
    md_pad(c, 128, 1, sha512_block);
    for (int i = 0; i < 8; i++)
        put_be64(all + 8 * i, c->md.h64[i]);
    memcpy(out, all, bytes);
}

static void sha512_final(union digest_ctx *c, uint8_t *out)
{
    sha512_out(c, out, 64);
}

static void sha384_final(union digest_ctx *c, uint8_t *out)
{
    sha512_out(c, out, 48);
}

static void sha512t256_final(union digest_ctx *c, uint8_t *out)
{
    sha512_out(c, out, 32);
}

/* ---- RIPEMD-160 ------------------------------------------------------------------ */

static const uint8_t rmd_r[80] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    7, 4, 13, 1, 10, 6, 15, 3, 12, 0, 9, 5, 2, 14, 11, 8,
    3, 10, 14, 4, 9, 15, 8, 1, 2, 7, 0, 6, 13, 11, 5, 12,
    1, 9, 11, 10, 0, 8, 12, 4, 13, 3, 7, 15, 14, 5, 6, 2,
    4, 0, 5, 9, 7, 12, 2, 10, 14, 1, 3, 8, 11, 6, 15, 13,
};
static const uint8_t rmd_rr[80] = {
    5, 14, 7, 0, 9, 2, 11, 4, 13, 6, 15, 8, 1, 10, 3, 12,
    6, 11, 3, 7, 0, 13, 5, 10, 14, 15, 8, 12, 4, 9, 1, 2,
    15, 5, 1, 3, 7, 14, 6, 9, 11, 8, 12, 2, 10, 0, 4, 13,
    8, 6, 4, 1, 3, 11, 15, 0, 5, 12, 2, 13, 9, 7, 10, 14,
    12, 15, 10, 4, 1, 5, 8, 7, 6, 2, 13, 14, 0, 3, 9, 11,
};
static const uint8_t rmd_s[80] = {
    11, 14, 15, 12, 5, 8, 7, 9, 11, 13, 14, 15, 6, 7, 9, 8,
    7, 6, 8, 13, 11, 9, 7, 15, 7, 12, 15, 9, 11, 7, 13, 12,
    11, 13, 6, 7, 14, 9, 13, 15, 14, 8, 13, 6, 5, 12, 7, 5,
    11, 12, 14, 15, 14, 15, 9, 8, 9, 14, 5, 6, 8, 6, 5, 12,
    9, 15, 5, 11, 6, 8, 13, 12, 5, 12, 13, 14, 11, 8, 5, 6,
};
static const uint8_t rmd_ss[80] = {
    8, 9, 9, 11, 13, 15, 15, 5, 7, 7, 8, 11, 14, 14, 12, 6,
    9, 13, 15, 7, 12, 8, 9, 11, 7, 7, 12, 7, 6, 15, 13, 11,
    9, 7, 15, 11, 8, 6, 6, 14, 12, 13, 5, 14, 13, 13, 7, 5,
    15, 5, 8, 11, 14, 14, 6, 14, 6, 9, 12, 9, 12, 5, 15, 8,
    8, 5, 12, 9, 12, 5, 14, 6, 8, 13, 6, 5, 15, 13, 11, 11,
};

static uint32_t rmd_f(int j, uint32_t x, uint32_t y, uint32_t z)
{
    switch (j >> 4) {
    case 0: return x ^ y ^ z;
    case 1: return (x & y) | (~x & z);
    case 2: return (x | ~y) ^ z;
    case 3: return (x & z) | (y & ~z);
    default: return x ^ (y | ~z);
    }
}

static void rmd160_block(union digest_ctx *c, const uint8_t *p)
{
    static const uint32_t k[5] = { 0, 0x5a827999, 0x6ed9eba1, 0x8f1bbcdc, 0xa953fd4e };
    static const uint32_t kk[5] = { 0x50a28be6, 0x5c4dd124, 0x6d703ef3, 0x7a6d76e9, 0 };
    uint32_t x[16], *h = c->md.h32;
    for (int i = 0; i < 16; i++)
        x[i] = le32(p + 4 * i);
    uint32_t a = h[0], b = h[1], cc = h[2], d = h[3], e = h[4];
    uint32_t aa = a, bb = b, ccc = cc, dd = d, ee = e;
    for (int j = 0; j < 80; j++) {
        uint32_t t = rol32(a + rmd_f(j, b, cc, d) + x[rmd_r[j]] + k[j >> 4], rmd_s[j]) + e;
        a = e, e = d, d = rol32(cc, 10), cc = b, b = t;
        t = rol32(aa + rmd_f(79 - j, bb, ccc, dd) + x[rmd_rr[j]] + kk[j >> 4], rmd_ss[j]) + ee;
        aa = ee, ee = dd, dd = rol32(ccc, 10), ccc = bb, bb = t;
    }
    uint32_t t = h[1] + cc + dd;
    h[1] = h[2] + d + ee;
    h[2] = h[3] + e + aa;
    h[3] = h[4] + a + bb;
    h[4] = h[0] + b + ccc;
    h[0] = t;
}

static void rmd160_init(union digest_ctx *c)
{
    sha1_init(c);                                   /* the same five words */
}

static void rmd160_update(union digest_ctx *c, const uint8_t *p, size_t n)
{
    md_update(c, p, n, 64, rmd160_block);
}

static void rmd160_final(union digest_ctx *c, uint8_t *out)
{
    md_pad(c, 64, 0, rmd160_block);
    for (int i = 0; i < 5; i++)
        put_le32(out + 4 * i, c->md.h32[i]);
}

/* ---- Skein 1.3: Threefish in UBI chaining ------------------------------------------ */

#define SKEIN_C240 0x1bd11bdaa9fc1a22ull
#define T_FIRST    (1ull << 62)
#define T_FINAL    (1ull << 63)
#define T_CFG      (4ull << 56)
#define T_MSG      (48ull << 56)
#define T_OUT      (63ull << 56)

static const uint8_t rot256[8][2] = { { 14, 16 }, { 52, 57 }, { 23, 40 }, { 5, 37 },
                                      { 25, 33 }, { 46, 12 }, { 58, 22 }, { 32, 32 } };
static const uint8_t rot512[8][4] = { { 46, 36, 19, 37 }, { 33, 27, 14, 42 }, { 17, 49, 36, 39 },
                                      { 44, 9, 54, 56 },  { 39, 30, 34, 24 }, { 13, 50, 10, 17 },
                                      { 25, 29, 39, 43 }, { 8, 35, 56, 22 } };
static const uint8_t rot1024[8][8] = {
    { 24, 13, 8, 47, 8, 17, 22, 37 },   { 38, 19, 10, 55, 49, 18, 23, 52 },
    { 33, 4, 51, 13, 34, 41, 59, 17 },  { 5, 20, 48, 41, 47, 28, 16, 25 },
    { 41, 9, 37, 31, 12, 47, 44, 30 },  { 16, 34, 56, 51, 4, 53, 42, 41 },
    { 31, 44, 47, 46, 19, 42, 44, 25 }, { 9, 48, 35, 52, 23, 31, 37, 20 },
};
static const uint8_t perm256[4] = { 0, 3, 2, 1 };
static const uint8_t perm512[8] = { 2, 1, 4, 7, 6, 5, 0, 3 };
static const uint8_t perm1024[16] = { 0, 9, 2, 13, 6, 11, 4, 15, 10, 7, 12, 3, 14, 5, 8, 1 };

/* Threefish-(64 nw): out = E(key, tweak, in) */
static void threefish(int nw, const uint64_t *key, const uint64_t tweak[2], const uint64_t *in, uint64_t *out)
{
    uint64_t k[17], t[3] = { tweak[0], tweak[1], tweak[0] ^ tweak[1] }, v[16], tmp[16];
    const uint8_t *perm = nw == 4 ? perm256 : nw == 8 ? perm512 : perm1024;
    int rounds = nw == 16 ? 80 : 72;
    k[nw] = SKEIN_C240;
    for (int i = 0; i < nw; i++)
        k[i] = key[i], k[nw] ^= key[i];
    memcpy(v, in, (size_t)nw * 8);
    for (int d = 0; d <= rounds; d++) {
        if (d % 4 == 0) {                           /* the subkey d/4 */
            int s = d / 4;
            for (int i = 0; i < nw; i++)
                v[i] += k[(s + i) % (nw + 1)];
            v[nw - 3] += t[s % 3];
            v[nw - 2] += t[(s + 1) % 3];
            v[nw - 1] += (uint64_t)s;
        }
        if (d == rounds)
            break;
        for (int j = 0; j < nw / 2; j++) {
            unsigned r = nw == 4 ? rot256[d % 8][j] : nw == 8 ? rot512[d % 8][j] : rot1024[d % 8][j];
            v[2 * j] += v[2 * j + 1];
            v[2 * j + 1] = rol64(v[2 * j + 1], r) ^ v[2 * j];
        }
        for (int i = 0; i < nw; i++)
            tmp[i] = v[perm[i]];
        memcpy(v, tmp, (size_t)nw * 8);
    }
    memcpy(out, v, (size_t)nw * 8);
}

/* One UBI block: g = E(g, tweak, m) ^ m, the block's bytes little-endian */
static void ubi_block(struct skein *s, const uint8_t *block, uint64_t pos, uint64_t flags)
{
    uint64_t m[16], out[16], tweak[2] = { pos, flags };
    for (int i = 0; i < s->nw; i++)
        m[i] = le64(block + 8 * i);
    threefish(s->nw, s->g, tweak, m, out);
    for (int i = 0; i < s->nw; i++)
        s->g[i] = out[i] ^ m[i];
}

static void skein_init(union digest_ctx *c, int nw, unsigned bits)
{
    struct skein *s = &c->skein;
    memset(c, 0, sizeof *c);
    s->nw = nw, s->bits = bits;
    uint8_t cfg[128] = { 'S', 'H', 'A', '3', 1, 0, 0, 0 };
    put_le64(cfg + 8, bits);
    ubi_block(s, cfg, 32, T_CFG | T_FIRST | T_FINAL);
    s->first = 1;
}

static void skein256_init(union digest_ctx *c)
{
    skein_init(c, 4, 256);
}

static void skein512_init(union digest_ctx *c)
{
    skein_init(c, 8, 512);
}

static void skein1024_init(union digest_ctx *c)
{
    skein_init(c, 16, 1024);
}

/* A full buffer waits for more: the last block is processed as final. */
static void skein_update(union digest_ctx *c, const uint8_t *p, size_t n)
{
    struct skein *s = &c->skein;
    size_t size = (size_t)s->nw * 8;
    while (n) {
        if (s->used == size) {
            s->pos += size;
            ubi_block(s, s->buf, s->pos, T_MSG | (s->first ? T_FIRST : 0));
            s->first = 0, s->used = 0;
        }
        size_t take = size - s->used < n ? size - s->used : n;
        memcpy(s->buf + s->used, p, take);
        s->used += take, p += take, n -= take;
    }
}

static void skein_final(union digest_ctx *c, uint8_t *out)
{
    struct skein *s = &c->skein;
    size_t size = (size_t)s->nw * 8;
    memset(s->buf + s->used, 0, size - s->used);
    s->pos += s->used;
    ubi_block(s, s->buf, s->pos, T_MSG | T_FINAL | (s->first ? T_FIRST : 0));
    uint8_t counter[128] = { 0 };                   /* output block 0 */
    ubi_block(s, counter, 8, T_OUT | T_FIRST | T_FINAL);
    uint8_t all[128];
    for (int i = 0; i < s->nw; i++)
        put_le64(all + 8 * i, s->g[i]);
    memcpy(out, all, s->bits / 8);
}

/* ---- the table, and FreeBSD's test suite's answers ---------------------------------------- */

static const char *const md5_tests[8] = {
    "d41d8cd98f00b204e9800998ecf8427e", "0cc175b9c0f1b6a831c399e269772661",
    "900150983cd24fb0d6963f7d28e17f72", "f96b697d7cb7938d525a2f31aaf161d0",
    "c3fcd3d76192e4007dfb496cca67e13b", "d174ab98d277d9f5a5611c2c9f419d9f",
    "57edf4a22be3c955ac49da2e2107b67a", "b50663f41d44d92171cb9976bc118538",
};
static const char *const sha1_tests[8] = {
    "da39a3ee5e6b4b0d3255bfef95601890afd80709", "86f7e437faa5a7fce15d1ddcb9eaeaea377667b8",
    "a9993e364706816aba3e25717850c26c9cd0d89d", "c12252ceda8be8994d5fa0290a47231c1d16aae3",
    "32d10c7b8cf96570ca04ce37f2a19d84240d3a89", "761c457bf73b14d27e9e9265c46f4b4dda11f940",
    "50abf5706a150990a08b2c5ea40fa0e585554732", "18eca4333979c4181199b7b4fab8786d16cf2846",
};
static const char *const sha224_tests[8] = {
    "d14a028c2a3a2bc9476102bb288234c415a2b01f828ea62ac5b3e42f",
    "abd37534c7d9a2efb9465de931cd7055ffdb8879563ae98078d6d6d5",
    "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7",
    "2cb21c83ae2f004de7e81c3c7019cbcb65b71ab656b22d6d0c39b8eb",
    "45a5f72c39c5cff2522eb3429799e49e5f44b356ef926bcf390dccc2",
    "bff72b4fcb7d75e5632900ac5f90d219e05e97a7bde72e740db393d9",
    "b50aecbe4e9bb0b57bc5f3ae760a8e01db24f203fb3cdcd13148046e",
    "5ae55f3779c8a1204210d7ed7689f661fbe140f96f272ab79e19d470",
};
static const char *const sha256_tests[8] = {
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb",
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    "f7846f55cf23e14eebeab5b4e1550cad5b509e3348fbc4efa3a1413d393cb650",
    "71c480df93d6ae2f1efad1447c66c9525e316218cf51fc8d9ed832f2daf18b73",
    "db4bfcbd4da0cd85a60c3c37d3fbd8805c77f15fc6b1fdfe614ee0a7c8fdb4c0",
    "f371bc4a311f2b009eef952dd83ca80e2b60026c8e935592d0f9c308453c813e",
    "e6eae09f10ad4122a0e2a4075761d185a272ebd9f5aa489e998ff2f09cbfdd9f",
};
static const char *const sha384_tests[8] = {
    "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b",
    "54a59b9f22b0b80880d8427e548b7c23abd873486e1f035dce9cd697e85175033caa88e6d57bc35efae0b5afd3145f31",
    "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7",
    "473ed35167ec1f5d8e550368a3db39be54639f828868e9454c239fc8b52e3c61dbd0d8b4de1390c256dcbb5d5fd99cd5",
    "feb67349df3db6f5924815d6c3dc133f091809213731fe5c7b5f4999e463479ff2877f5f2936fa63bb43784b12f3ebb4",
    "1761336e3f7cbfe51deb137f026f89e01a448e3b1fafa64039c1464ee8732f11a5341a6f41e0c202294736ed64db1a84",
    "b12932b0627d1c060942f5447764155655bd4da0c9afa6dd9b9ef53129af1b8fb0195996d2de9ca0df9d821ffee67026",
    "99428d401bf4abcd4ee0695248c9858b7503853acfae21a9cffa7855f46d1395ef38596fcd06d5a8c32d41a839cc5dfb",
};
static const char *const sha512_tests[8] = {
    "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd4"
    "7417a81a538327af927da3e",
    "1f40fc92da241694750979ee6cf582f2d5d7d28e18335de05abc54d0560e0f5302860c652bf08d560252aa5e74210546f369fbbbc"
    "e8c12cfc7957b2652fe9a75",
    "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d44236"
    "43ce80e2a9ac94fa54ca49f",
    "107dbf389d9e9f71a3a95f6c055b9251bc5268c2be16d6c13492ea45b0199f3309e16455ab1e96118e8a905d5597b72038ddb372a"
    "89826046de66687bb420e7c",
    "4dbff86cc2ca1bae1e16468a05cb9881c97f1753bce3619034898faa1aabe429955a1bf8ec483d7421fe3c1646613a59ed5441fb0"
    "f321389f77f48a879c7b1f1",
    "1e07be23c26a86ea37ea810c8ec7809352515a970e9253c26f536cfc7a9996c45c8370583e0a78fa4a90041d71a4ceab7423f19c7"
    "1b9d5a3e01249f0bebd5894",
    "72ec1ef1124a45b047e8b7c75a932195135bb61de24ec0d1914042246e0aec3a2354e093d76f3048b456764346900cb130d2a4fd5"
    "dd16abb5e30bcb850dee843",
    "e8a835195e039708b13d9131e025f4441dbdc521ce625f245a436dcd762f54bf5cb298d96235e6c6a304e087ec8189b9512cbdf64"
    "27737ea82793460c367b9c3",
};
static const char *const sha512t256_tests[8] = {
    "c672b8d1ef56ed28ab87c3622c5114069bdd3ad7b8f9737498d0c01ecef0967a",
    "455e518824bc0601f9fb858ff5c37d417d67c2f8e0df2babe4808858aea830f8",
    "53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23",
    "0cf471fd17ed69d990daf3433c89b16d63dec1bb9cb42a6094604ee5d7b4e9fb",
    "fc3189443f9c268f626aea08a756abe7b726b05f701cb08222312ccfd6710a26",
    "cdf1cc0effe26ecc0c13758f7b4a48e000615df241284185c39eb05d355bb9c8",
    "2c9fdbc0c90bdd87612ee8455474f9044850241dc105b1e8b94b8ddf5fac9148",
    "dd095fc859b336c30a52548b3dc59fcc0d1be8616ebcf3368fad23107db2d736",
};
static const char *const rmd160_tests[8] = {
    "9c1185a5c5e9fc54612808977ee8f548b2258d31", "0bdc9d2d256b3ee9daae347be6f4dc835a467ffe",
    "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc", "5d0689ef49d2fae572b881b123a85ffa21595f36",
    "f71c27109c692c1b56bbdceb5b9d2865b3708dbc", "b0e20b6e3116640286ed3a87a5713079b21f5189",
    "9b752e45573d4b39f4dbd3323cab82bf63326bfb", "5feb69c6bf7c29d95715ad55f57d8ac5b2b7dd32",
};
static const char *const skein256_tests[8] = {
    "c8877087da56e072870daa843f176e9453115929094c3a40c463a196c29bf7ba",
    "7fba44ff1a31d71a0c1f82e6e82fb5e9ac6c92a39c9185b9951fed82d82fe635",
    "258bdec343b9fde1639221a5ae0144a96e552e5288753c5fec76c05fc2fc1870",
    "4d2ce0062b5eb3a4db95bc1117dd8aa014f6cd50fdc8e64f31f7d41f9231e488",
    "46d8440685461b00e3ddb891b2ecc6855287d2bd8834a95fb1c1708b00ea5e82",
    "7c5eb606389556b33d34eb2536459528dc0af97adbcd0ce273aeb650f598d4b2",
    "4def7a7e5464a140ae9c3a80279fbebce4bd00f9faad819ab7e001512f67a10d",
    "d9c017dbe355f318d036469eb9b5fbe129fc2b5786a9dc6746a516eab6fe0126",
};
static const char *const skein512_tests[8] = {
    "bc5b4c50925519c290cc634277ae3d6257212395cba733bbad37a4af0fa06af41fca7903d06564fea7a2d3730dbdb80c1f85562df"
    "cc070334ea4d1d9e72cba7a",
    "b1cd8d33f61b3737adfd59bb13ad82f4a9548e92f22956a8976cca3fdb7fee4fe91698146c4197cec85d38b83c5d93bdba92c01fd"
    "9a53870d0c7f967bc62bdce",
    "8f5dd9ec798152668e35129496b029a960c9a9b88662f7f9482f110b31f9f93893ecfb25c009baad9e46737197d5630379816a886"
    "aa05526d3a70df272d96e75",
    "15b73c158ffb875fed4d72801ded0794c720b121c0c78edf45f900937e6933d9e21a3a984206933d504b5dbb2368000411477ee1b"
    "204c986068df77886542fcc",
    "23793ad900ef12f9165c8080da6fdfd2c8354a2929b8aadf83aa82a3c6470342f57cf8c035ec0d97429b626c4d94f28632c8f5134"
    "fd367dca5cf293d2ec13f8c",
    "0c6bed927e022f5ddcf81877d42e5f75798a9f8fd3ede3d83baac0a2f364b082e036c11af35fe478745459dd8f5c0b73efe3c56ba"
    "5bb2009208d5a29cc6e469c",
    "2ca9fcffb3456f297d1b5f407014ecb856f0baac8eb540f534b1f187196f21e88f31103128c2f03fcc9857d7a58eb66f9525e2302"
    "d88833ee069295537a434ce",
    "1131f2aaa0e97126c9314f9f968cc827259bbfabced2943bb8c9274448998fb3b78738b4580dd500c76105fd3c03e465e1414f2c2"
    "9664286b1f79d3e51128125",
};
static const char *const skein1024_tests[8] = {
    "0fff9563bb3279289227ac77d319b6fff8d7e9f09da1247b72a0a265cd6d2a62645ad547ed8193db48cff847c06494a03f55666d3"
    "b47eb4c20456c9373c86297d630d5578ebd34cb40991578f9f52b18003efa35d3da6553ff35db91b81ab890bec1b189b7f52cb2a7"
    "83ebb7d823d725b0b4a71f6824e88f68f982eefc6d19c6",
    "6ab4c4ba9814a3d976ec8bffa7fcc638ceba0544a97b3c98411323ffd2dc936315d13dc93c13c4e88cda6f5bac6f2558b2d8694d3"
    "b6143e40d644ae43ca940685cb37f809d3d0550c56cba8036dee729a4f8fb960732e59e64d57f7f7710f8670963cdcdc95b41daab"
    "4855fcf8b6762a64b173ee61343a2c7689af1d293eba97",
    "35a599a0f91abcdb4cb73c19b8cb8d947742d82c309137a7caed29e8e0a2ca7a9ff9a90c34c1908cc7e7fd99bb15032fb86e76df2"
    "1b72628399b5f7c3cc209d7bb31c99cd4e19465622a049afbb87c03b5ce3888d17e6e667279ec0aa9b3e2712624c01b5f5bbe1a5"
    "64220bdcf6990af0c2539019f313fdd7406cca3892a1f1f",
    "ea891f5268acd0fac97467fc1aa89d1ce8681a9992a42540e53babee861483110c2d16f49e73bac27653ff173003e40cfb08516cd"
    "34262e6af95a5d8645c9c1abb3e813604d508b8511b30f9a5c1b352aa0791c7d2f27b2706dccea54bc7de6555b5202351751c329"
    "9f97c09cf89c40f67187e2521c0fad82b30edbb224f0458",
    "f23d95c2a25fbcd0e797cd058fec39d3c52d2b5afd7a9af1df934e63257d1d3dcf3246e7329c0f1104c1e51e3d22e300507b0c3b9"
    "f985bb1f645ef49835080536becf83788e17fed09c9982ba65c3cb7ffe6a5f745b911c506962adf226e435c42f6f6bc08d288f9c8"
    "10e807e3216ef444f3db22744441deefa4900982a1371f",
    "cf3889e8a8d11bfd3938055d7d061437962bc5eac8ae83b1b71c94be201b8cf657fdbfc38674997a008c0c903f56a23feb3ae30e0"
    "12377f1cfa080a9ca7fe8b96138662653fb3335c7d06595bf8baf65e215307532094cfdfa056bd8052ab792a3944a2adaa47b303"
    "35b8badb8fe9eb94fe329cdca04e58bbc530f0af709f469",
    "cf21a613620e6c119eca31fdfaad449a8e02f95ca256c21d2a105f8e4157048f9fe1e897893ea18b64e0e37cb07d5ac947f27ba54"
    "4caf7cbc1ad094e675aed77a366270f7eb7f46543bccfa61c526fd628408058ed00ed566ac35a9761d002e629c4fb0d430b2f4ad0"
    "16fcc49c44d2981c4002da0eecc42144160e2eaea4855a",
    "e6799b78db54085a2be7ff4c8007f147fa88d326abab30be0560b953396d8802feee9a15419b48a467574e9283be15685ca8a079e"
    "e52b27166b64dd70b124b1d4e4f6aca37224c3f2685e67e67baef9f94b905698adc794a09672aba977a61b20966912acdb08c21a2"
    "c37001785355dc884751a21f848ab36e590331ff938138",
};

const struct digest digests[] = {
    { "md5", "MD5", 16, md5_tests, md5_init, md5_update, md5_final },
    { "sha1", "SHA1", 20, sha1_tests, sha1_init, sha1_update, sha1_final },
    { "sha224", "SHA224", 28, sha224_tests, sha224_init, sha256_update, sha224_final },
    { "sha256", "SHA256", 32, sha256_tests, sha256_init, sha256_update, sha256_final },
    { "sha384", "SHA384", 48, sha384_tests, sha384_init, sha512_update, sha384_final },
    { "sha512", "SHA512", 64, sha512_tests, sha512_init, sha512_update, sha512_final },
    { "sha512t256", "SHA512t256", 32, sha512t256_tests, sha512t256_init, sha512_update, sha512t256_final },
    { "rmd160", "RMD160", 20, rmd160_tests, rmd160_init, rmd160_update, rmd160_final },
    { "skein256", "Skein256", 32, skein256_tests, skein256_init, skein_update, skein_final },
    { "skein512", "Skein512", 64, skein512_tests, skein512_init, skein_update, skein_final },
    { "skein1024", "Skein1024", 128, skein1024_tests, skein1024_init, skein_update, skein_final },
    { 0 },
};

const char *const digest_test_input[8] = {
    "",
    "a",
    "abc",
    "message digest",
    "abcdefghijklmnopqrstuvwxyz",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
    "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
    ("MD5 has not yet (2001-09-03) been broken, but sufficient attacks have been made "
     "that its security is in some doubt"),
};

void digest_hex(const struct digest *d, union digest_ctx *c, char *hex)
{
    uint8_t out[128];
    d->final(c, out);
    for (unsigned i = 0; i < d->len; i++) {
        hex[2 * i] = "0123456789abcdef"[out[i] >> 4];
        hex[2 * i + 1] = "0123456789abcdef"[out[i] & 15];
    }
    hex[2 * d->len] = 0;
}
