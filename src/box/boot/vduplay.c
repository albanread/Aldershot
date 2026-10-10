/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* vduplay.c: play a VDU byte stream and keep what it drew. This is the ROSGD
 * half of the screendump comparison with the 32-bit system (tests/vdu).
 *
 * It does the same as the farm's BASIC program (tests/vdu/Play). The mode
 * selector in <dir>/Sel, if there is one, goes through OS_ScreenMode 0. Then
 * the soft font is reset (OS_Byte 25), and the bytes of <dir>/In go through
 * OS_WriteN. Then <dir>/Vars is written: the mode and VDU variables, every
 * palette entry, POS and VPOS, and OS_Byte 117, one to a line as BASIC's
 * BPUT# writes them. Then <dir>/Screen is written: ScreenSize bytes of
 * screen memory from ScreenStart. The files are the host's.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "selftest.h"

static void call(uint32_t n, struct ros_cpu *s)
{
    ros_swi(s, n);
}

int ros_vdu_play(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/In", dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ros_console_printf("vduplay: no %s\n", path);
        return 1;
    }
    uint8_t *in = ros_rma_alloc(1 << 20);
    size_t n = fread(in, 1, 1 << 20, f);
    fclose(f);

    /* A mode selector first, when <dir>/Sel gives one; and the font the
     * hard font, as the farm's desktop may have left it otherwise */
    struct ros_cpu s;
    snprintf(path, sizeof path, "%s/Sel", dir);
    uint32_t *sel = ros_rma_alloc(256);
    if ((f = fopen(path, "rb")) != NULL) {
        size_t words = fread(sel, 4, 60, f);
        fclose(f);
        sel[words] = 0xFFFFFFFFu;
        ros_cpu_enter(&s);
        s.r[0] = 0, s.r[1] = ros_addr(sel);
        call(XOS_ScreenMode, &s);
        if (s.v)
            ros_console_printf("vduplay: %s\n", ((os_error *)ros_ptr(s.r[0]))->errmess);
    }
    ros_cpu_enter(&s);
    s.r[0] = 25, s.r[1] = 0;
    call(XOS_Byte, &s);

    ros_cpu_enter(&s);
    s.r[0] = ros_addr(in), s.r[1] = (uint32_t)n;
    call(XOS_WriteN, &s);
    if (s.v)
        ros_console_printf("vduplay: %s\n", ((os_error *)ros_ptr(s.r[0]))->errmess);

    /* The op script, as Play runs it: SWIs (registers relocated into the
     * buffer by a mask), writes to the buffer, VDU bytes, dumps */
    snprintf(path, sizeof path, "%s/Ops", dir);
    FILE *of = fopen(path, "rb");
    snprintf(path, sizeof path, "%s/Res", dir);
    FILE *res = fopen(path, "w");
    snprintf(path, sizeof path, "%s/Buf", dir);
    FILE *buf = fopen(path, "wb");
    if (of && res && buf) {
        uint8_t *ops = malloc(1 << 20);
        size_t on = fread(ops, 1, 1 << 20, of);
        uint8_t *ub = ros_rma_alloc(0x100000);
        memset(ub, 0, 0x100000);
        uint32_t u = ros_addr(ub), q = 0;
        uint32_t slot[16] = { 0 }, nslots = 0;  /* handles, which differ between systems */
        uint32_t last[8] = { 0 };               /* the last SWI's results */
        fprintf(res, "U %X\n", u);
        uint32_t *w;
        while (q + 4 <= on) {
            w = (uint32_t *)(ops + q);
            if (w[0] == 1) {
                /* w[10]: bits 0-7 registers relative to the buffer, 8-15
                 * registers that name a slot, 16-23 results printed as one,
                 * 24-31 results not compared (pointers into the system) */
                ros_cpu_enter(&s);
                for (int i = 0; i < 8; i++) {
                    s.r[i] = w[2 + i] + ((w[10] >> i) & 1 ? u : 0);
                    if ((w[10] >> (8 + i)) & 1)
                        s.r[i] = slot[w[2 + i] & 15];
                }
                call(w[1] | 0x20000, &s);
                memcpy(last, s.r, sizeof last);
                if (s.v) {
                    os_error *e = ros_ptr(s.r[0]);
                    fprintf(res, "V %X %.200s\n", e->errnum, e->errmess);
                } else {
                    fprintf(res, "R");
                    for (int i = 0; i < 8; i++) {
                        uint32_t r = s.r[i];
                        if ((w[10] >> (24 + i)) & 1) {
                            fprintf(res, " x");             /* not compared */
                        } else if ((w[10] >> (16 + i)) & 1 && r) {
                            uint32_t k = 0;
                            while (k < nslots && slot[k] != r)
                                k++;
                            if (k == nslots && nslots < 16)
                                slot[nslots++] = r;
                            fprintf(res, " h%X", k);
                        } else if (r >= u && r < u + 0x100000)
                            fprintf(res, " b%X", r - u);
                        else
                            fprintf(res, " %X", r);
                    }
                    fprintf(res, "\n");
                }
                q += 44;
            } else if (w[0] == 2) {
                memcpy(ub + w[1], ops + q + 12, w[2]);
                q += 12 + ((w[2] + 3) & ~3u);
            } else if (w[0] == 3) {
                uint8_t *t = ros_rma_alloc(w[1] + 4);
                memcpy(t, ops + q + 8, w[1]);
                ros_cpu_enter(&s);
                s.r[0] = ros_addr(t), s.r[1] = w[1];
                call(XOS_WriteN, &s);
                ros_rma_free(t);
                q += 8 + ((w[1] + 3) & ~3u);
            } else if (w[0] == 4) {
                fwrite(ub + w[1], 1, w[2], buf);
                q += 12;
            } else if (w[0] == 6) {             /* w[2] bytes at the address stored at w[1] */
                uint32_t a;
                memcpy(&a, ub + w[1], 4);
                fwrite(ros_ptr(a), 1, w[2], buf);
                q += 12;
            } else if (w[0] == 8) {             /* vector w[1] with R0-R3 w[2..5] */
                ros_cpu_enter(&s);
                s.r[0] = w[2], s.r[1] = w[3], s.r[2] = w[4], s.r[3] = w[5], s.r[9] = w[1];
                call(0x34 | 0x20000, &s);       /* OS_CallAVector */
                q += 24;
            } else if (w[0] == 7) {             /* the last SWI's R(w[2]), at w[1] */
                memcpy(ub + w[1], &last[w[2] & 7], 4);
                q += 12;
            } else if (w[0] == 5) {             /* the buffer's address + w[2], at w[1] */
                uint32_t a = u + w[2];
                memcpy(ub + w[1], &a, 4);
                q += 12;
            } else {
                break;
            }
        }
        free(ops);
    }
    if (of)
        fclose(of);
    if (res)
        fclose(res);
    if (buf)
        fclose(buf);

    uint32_t *v = ros_rma_alloc(1024);
    for (uint32_t i = 0; i <= 12; i++)
        v[i] = i;
    for (uint32_t i = 128; i <= 177; i++)
        v[i - 115] = i;
    v[63] = 256, v[64] = 257, v[65] = 0xFFFFFFFFu;
    ros_cpu_enter(&s);
    s.r[0] = ros_addr(v), s.r[1] = ros_addr(v) + 512;
    call(XOS_ReadVduVariables, &s);

    snprintf(path, sizeof path, "%s/Vars", dir);
    FILE *o = fopen(path, "w");
    if (!o)
        return 1;
    for (int i = 0; i <= 64; i++)
        fprintf(o, "%X %X\n", v[i], v[128 + i]);
    for (uint32_t i = 0; i < 256; i++) {
        ros_cpu_enter(&s);
        s.r[0] = i, s.r[1] = 16;
        call(XOS_ReadPalette, &s);
        fprintf(o, "P%u %X %X\n", i, s.r[2], s.r[3]);
    }
    ros_cpu_enter(&s);
    s.r[0] = 134;
    call(XOS_Byte, &s);
    fprintf(o, "POS %u %u\n", s.r[1], s.r[2]);
    ros_cpu_enter(&s);
    s.r[0] = 117;
    call(XOS_Byte, &s);
    fprintf(o, "STATUS %X\n", s.r[1]);
    fclose(o);

    uint32_t start = v[128 + 33], size = v[128 + 7];
    if (!start)
        return 1;                       /* no screen */
    snprintf(path, sizeof path, "%s/Screen", dir);
    o = fopen(path, "wb");
    if (!o)
        return 1;
    fwrite(ros_ptr(start), 1, size, o);
    fclose(o);
    ros_rma_free(v);
    ros_rma_free(sel);
    ros_rma_free(in);
    return 0;
}
