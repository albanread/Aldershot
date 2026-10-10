/* Derived from:
 *
 * MDDRIVER.C - test driver for MD2, MD4 and MD5
 */

/*
 *  Copyright (C) 1990-2, RSA Data Security, Inc. Created 1990. All
 *  rights reserved.
 *
 *  RSA Data Security, Inc. makes no representations concerning either
 *  the merchantability of this software or the suitability of this
 *  software for any particular purpose. It is provided "as is"
 *  without express or implied warranty of any kind.
 *
 *  These notices must be retained in any copies of any part of this
 *  documentation and/or software.
 *
 * This file is a reimplementation for BOX, over Linux, of FreeBSD's md5(1) as
 * ported to RISC OS in RISC OS Open's InetRes
 * (Sources/SystemRes/InetRes/Sources/md5: c.md5).
 */

/* md5.c -- *MD5 and its family (InetRes/Sources/md5, FreeBSD's md5(1)).
 *
 * RISC OS's MD5 is FreeBSD's md5 with every digest but MD5 left out
 * (RISCOS_TWEAK). ROSGD has them all back, each a command as FreeBSD links
 * them: *MD5, *SHA1, *SHA224, *SHA256, *SHA384, *SHA512, *SHA512t256,
 * *RMD160, *Skein256, *Skein512, *Skein1024, and the *...Sum forms, which
 * print as GNU's coreutils do and check a file of digests with -c.
 *
 * The output is FreeBSD's: "SHA256 (file) = <hex>", "<hex> file" with -r,
 * and the digest alone with -q. -c compares with an expected digest (or,
 * for the Sum forms, with a file of them). -s digests a string, -x runs the
 * test suite, and -t runs a time trial. Files are RISC OS names, read
 * through FileSwitch. There is no standard input to digest, as on RISC OS,
 * so with nothing to do the usage is printed. Sys$ReturnCode is 1 if a
 * file could not be read, and 2 if a check failed.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "digest.h"
#include "inetres.h"

#define TEST_BLOCK_LEN   10000
#define TEST_BLOCK_COUNT 100000
#define HEX_MAX          257

/* One command's state, as md5.c's statics are */
struct run {
    const struct digest *d;
    char prog[16];                  /* "sha256", or "sha256sum" */
    int gnu, bflag, cflag, qflag, rflag;
    const char *check;              /* -c's digest, or a record's */
    int failed, checks_failed;
};

/* -c's file, for the Sum forms: a digest a file */
struct record {
    char *file, *sum;
};

static os_error *output(struct run *r, const char *hex, const char *name)
{
    int bad = 0;
    os_error *e = NULL;
    if (r->cflag && r->gnu) {
        bad = strcasecmp(r->check, hex) != 0;
        if (!r->qflag || bad)
            e = inet_printf("%s: %s\n", name, bad ? "FAILED" : "OK");
    } else if (r->qflag) {
        e = inet_printf("%s\n", hex);
    } else {
        if (r->rflag)
            e = r->gnu ? inet_printf("%s %c%s", hex, r->bflag ? '*' : ' ', name)
                       : inet_printf("%s %s", hex, name);
        else
            e = inet_printf("%s (%s) = %s", r->d->name, name, hex);
        if (!e && r->check) {
            bad = strcasecmp(r->check, hex) != 0;
            if (bad)
                e = inet_printf(" [ Failed ]");
        }
        if (!e)
            e = inet_printf("\n");
    }
    r->checks_failed += bad;
    return e;
}

/* A file's digest: 0, -1 if it could not be read, 1 on Escape */
static int digest_file(struct run *r, const char *name, char *hex)
{
    struct inet_file f;
    if (inet_fopen(&f, name, 0))
        return -1;
    union digest_ctx c;
    r->d->init(&c);
    static uint8_t buf[65536];
    long n;
    int esc = 0;
    while (!esc && (n = inet_fread(&f, buf, sizeof buf)) > 0) {
        r->d->update(&c, buf, (size_t)n);
        esc = inet_escape();
    }
    inet_fclose(&f);
    if (esc)
        return 1;
    digest_hex(r->d, &c, hex);
    return 0;
}

static os_error *test_suite(struct run *r)
{
    os_error *e = inet_printf("%s test suite:\n", r->d->name);
    for (int i = 0; i < 8 && !e; i++) {
        union digest_ctx c;
        char hex[HEX_MAX];
        r->d->init(&c);
        r->d->update(&c, (const uint8_t *)digest_test_input[i], strlen(digest_test_input[i]));
        digest_hex(r->d, &c, hex);
        int ok = !strcmp(hex, r->d->tests[i]);
        e = inet_printf("%s (\"%s\") = %s - %s\n", r->d->name, digest_test_input[i], hex,
                        ok ? "verified correct" : "INCORRECT RESULT!");
        r->failed += !ok;
    }
    return e;
}

static os_error *time_trial(struct run *r)
{
    static uint8_t block[TEST_BLOCK_LEN];
    for (int i = 0; i < TEST_BLOCK_LEN; i++)
        block[i] = (uint8_t)i;
    os_error *e = inet_printf("%s time trial. Digesting %d %d-byte blocks ...", r->d->name, TEST_BLOCK_COUNT,
                              TEST_BLOCK_LEN);
    union digest_ctx c;
    char hex[HEX_MAX];
    clock_t t0 = clock();
    r->d->init(&c);
    for (int i = 0; i < TEST_BLOCK_COUNT && !e; i++) {
        r->d->update(&c, block, TEST_BLOCK_LEN);
        if (i % 1000 == 0 && inet_escape())
            return inet_printf("\n");
    }
    digest_hex(r->d, &c, hex);
    float seconds = (float)(clock() - t0) / CLOCKS_PER_SEC;
    if (!e)
        e = inet_printf(" done\nDigest = %s\nTime = %f seconds\nSpeed = %f MiB/second\n", hex, (double)seconds,
                        (double)((float)TEST_BLOCK_LEN * (float)TEST_BLOCK_COUNT / seconds / (1 << 20)));
    return e;
}

/* -c's file for the Sum forms: "<Name> (<file>) = <hex>" (BSD) or
 * "<hex> [ *]<file>" (GNU) a line; the records, or -1 with e set */
static int read_checks(struct run *r, const struct inet_tool *t, const char *name, struct record **out,
                       os_error **e)
{
    struct inet_file f;
    os_error *err = inet_fopen(&f, name, 0);
    if (err) {
        *e = inet_fail(t, 1, "%s: %s", name, err->errmess);
        return -1;
    }
    size_t size = 0, cap = 4096;
    char *text = malloc(cap);
    long n;
    while (text && (n = inet_fread(&f, text + size, cap - size - 1)) > 0) {
        size += (size_t)n;
        if (size + 1 == cap) {
            char *bigger = realloc(text, cap * 2);
            if (!bigger) {
                free(text);
                text = NULL;
                break;
            }
            text = bigger, cap *= 2;
        }
    }
    inet_fclose(&f);
    if (!text) {
        *e = inet_fail(t, 1, "malloc failed");
        return -1;
    }
    text[size] = 0;

    const char *dname = r->d->name;
    size_t dlen = strlen(dname), hlen = 2 * r->d->len;
    int count = 0, malformed = 0;
    struct record *recs = NULL;
    for (char *line = text, *nl; *line; line = nl + 1) {
        nl = strchr(line, '\n');
        if (!nl) {
            *e = inet_fail(t, 1, "malformed input line %d (len=%d)", count + 1, (int)strlen(line));
            free(text), free(recs);
            return -1;
        }
        *nl = 0;
        size_t len = strlen(line);
        if (!len)
            break;
        char *file, *sum;
        if (len >= dlen + hlen + 6 && !strncmp(line, dname, dlen) && !strncmp(line + dlen, " (", 2) &&
            !strncmp(line + len - hlen - 4, ") = ", 4)) {
            line[len - hlen - 4] = 0;
            file = line + dlen + 2, sum = line + len - hlen;
        } else if (len >= hlen + 3 && line[hlen] == ' ') {
            line[hlen] = 0;
            sum = line, file = line + hlen + 1;
            if (*file == ' ' || *file == '*')
                file++;
        } else {
            malformed++;
            continue;
        }
        struct record *more = realloc(recs, (size_t)(count + 1) * sizeof *recs);
        if (!more)
            break;
        recs = more;
        recs[count].file = strdup(file);
        recs[count].sum = strdup(sum);
        count++;
    }
    free(text);
    *out = recs;
    return malformed << 16 | count;
}

static os_error *usage(const struct run *r)
{
    os_error *e;
    if (r->gnu)
        e = inet_printf("Usage:   %s [-bqrx] [-c file] [-s string] [files ...]\n"
                        "Options: -b  binary mode: '*' before each file's name\n"
                        "         -q  quiet mode\n"
                        "         -r  reverse the order of the output (the default)\n"
                        "         -c  check the digests listed in file\n"
                        "         -s  digest string\n"
                        "         -x  run the test suite\n", r->prog);
    else
        e = inet_printf("Usage:   %s [-qrtx] [-c expected] [-s string] [files ...]\n"
                        "Options: -q  quiet mode\n"
                        "         -r  reverse the order of the output\n"
                        "         -c  compare computed digests with expected value\n"
                        "         -s  digest string\n"
                        "         -t  run a time trial\n"
                        "         -x  run the test suite\n", r->prog);
    inet_exit(1);
    return e;
}

os_error *inet_md5(const struct inet_args *a)
{
    struct run r = { 0 };
    snprintf(r.prog, sizeof r.prog, "%s", a->argv[0]);
    size_t len = strlen(r.prog);
    if (len > 3 && !strcmp(r.prog + len - 3, "sum"))
        r.gnu = 1, r.rflag = 1, len -= 3;
    for (r.d = digests; r.d->prog; r.d++)
        if (strlen(r.d->prog) == len && !strncasecmp(r.d->prog, r.prog, len))
            break;
    if (!r.d->prog)
        r.d = digests;
    struct inet_tool t = { r.prog, 0 };

    struct inet_opt o = { 0 };
    const char *string = NULL, *checks = NULL;
    int sflag = 0, skip = 0, c;
    os_error *e = NULL;
    while ((c = inet_getopt(&o, a, "bc:qrs:tx")) != -1 && !e) {
        switch (c) {
        case 'b':
            r.bflag = 1;
            break;
        case 'c':
            r.cflag = 1;
            if (r.gnu)
                checks = o.arg;
            else
                r.check = o.arg;
            break;
        case 'q':
            r.qflag = 1;
            break;
        case 'r':
            r.rflag = 1;
            break;
        case 's':
            sflag = 1;
            string = o.arg;
            break;
        case 't':
            if (!r.gnu) {
                e = time_trial(&r);
                skip = 1;
            }                                       /* a Sum form's -t is text mode: nothing */
            break;
        case 'x':
            e = test_suite(&r);
            skip = 1;
            break;
        default:
            return usage(&r);
        }
    }
    if (e)
        return e;

    int nfiles = a->argc - o.ind;
    const char *const *files = (const char *const *)a->argv + o.ind;
    struct record *recs = NULL;
    int nrecs = 0, malformed = 0;
    if (r.cflag && r.gnu) {
        int got = read_checks(&r, &t, checks, &recs, &e);
        if (got < 0)
            return e;
        nrecs = got & 0xFFFF, malformed = got >> 16;
        nfiles = 0;
    }

    char hex[HEX_MAX];
    if (nrecs) {
        for (int i = 0; i < nrecs && !e; i++) {
            r.check = recs[i].sum;
            int rc = digest_file(&r, recs[i].file, hex);
            if (rc < 0) {
                e = inet_printf("%s: unable to open %s for reading\n", r.prog, recs[i].file);
                r.failed++;
            } else if (rc > 0) {
                e = ros_error(0x11, "Escape");
            } else {
                e = output(&r, hex, recs[i].file);
            }
        }
    } else if (nfiles) {
        for (int i = 0; i < nfiles && !e; i++) {
            int rc = digest_file(&r, files[i], hex);
            if (rc < 0) {
                e = inet_printf("%s: unable to open %s for reading\n", r.prog, files[i]);
                r.failed++;
            } else if (rc > 0) {
                e = ros_error(0x11, "Escape");
            } else {
                e = output(&r, hex, files[i]);
            }
        }
    } else if (!r.cflag && !sflag && !skip) {
        return usage(&r);                           /* no standard input to digest */
    } else if (sflag) {
        union digest_ctx ctx;
        r.d->init(&ctx);
        r.d->update(&ctx, (const uint8_t *)string, strlen(string));
        digest_hex(r.d, &ctx, hex);
        e = output(&r, hex, string);
    }
    for (int i = 0; i < nrecs; i++)
        free(recs[i].file), free(recs[i].sum);
    free(recs);
    if (e)
        return e;
    if (r.gnu && malformed)
        e = inet_printf("%s: WARNING: %d lines are improperly formatted\n", r.prog, malformed);
    if (!e && r.gnu && r.checks_failed)
        e = inet_printf("%s: WARNING: %d computed checksums did NOT match\n", r.prog, r.checks_failed);
    inet_exit(r.failed ? 1 : r.checks_failed ? 2 : 0);
    return e;
}
