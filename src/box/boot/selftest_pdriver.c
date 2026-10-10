/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_pdriver.c: PDriver, the printer driver whose pages are PDF
 * (modules/pdriver).
 *
 * What must always hold is this. The driver says what it is. The page size
 * is A4 with its margins. A whole print job (select, give a rectangle, draw
 * it band by band, end) leaves a PDF in the file the job was opened on, with
 * the colours the job plotted in it. The pixels are checked by reading the
 * PDF's image stream back and inflating it. So the test covers the band
 * capture and the writer together, and does more than check that a file
 * appeared.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "fileswitch.h"
#include "pdriver.h"
#include "selftest.h"

#define check ros_check

static int swi(uint32_t n, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return s.v;
}

static const char *stage = "";
static char why[160];

/* a step: note where we are, and keep the first error's text */
static int step(const char *what, int failed, const uint32_t r[8])
{
    if (!failed)
        return 1;
    if (!why[0])
        snprintf(why, sizeof why, "%s: %s", what,
                 r ? ((const os_error *)ros_ptr(r[0]))->errmess : "failed");
    stage = what;
    return 0;
}

static uint32_t str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = ros_rma_alloc((uint32_t)n);
    memcpy(p, s, n);
    return ros_addr(p);
}

static int scratch_disc(const char *name, char *dir, size_t max)
{
    const char *tmp = getenv("TMPDIR");
    if (!(tmp && *tmp))
        tmp = "/tmp";
    mkdir(tmp, 0777);
    snprintf(dir, max, "%s/rosgd-%s-XXXXXX", tmp, name);
    if (!mkdtemp(dir))
        return 0;
    return ros_hostfs_mount(name, dir) == 0;
}

static void scratch_gone(const char *name, const char *dir)
{
    ros_hostfs_unmount(name);
    DIR *d = opendir(dir);
    struct dirent *e;
    char p[700];
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.') {
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            unlink(p);
        }
    if (d)
        closedir(d);
    rmdir(dir);
}

/* The one image stream in the PDF, inflated: its bytes, or NULL */
static uint8_t *image_pixels(const uint8_t *pdf, size_t n, size_t want)
{
    const char *marker = "/Subtype /Image";
    const uint8_t *p = NULL;
    for (size_t i = 0; i + strlen(marker) < n; i++)
        if (!memcmp(pdf + i, marker, strlen(marker))) {
            p = pdf + i;
            break;
        }
    if (!p)
        return NULL;
    const uint8_t *s = NULL;
    for (const uint8_t *q = p; q + 7 < pdf + n; q++)
        if (!memcmp(q, "stream", 6)) {
            s = q + 6;
            while (s < pdf + n && (*s == '\r' || *s == '\n'))
                s++;
            break;
        }
    if (!s)
        return NULL;
    uint8_t *out = malloc(want);
    if (!out)
        return NULL;
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit(&z) != Z_OK) {
        free(out);
        return NULL;
    }
    z.next_in = (Bytef *)s, z.avail_in = (uInt)(pdf + n - s);
    z.next_out = out, z.avail_out = (uInt)want;
    int r = inflate(&z, Z_FINISH);
    size_t got = want - z.avail_out;
    inflateEnd(&z);
    if ((r != Z_STREAM_END && r != Z_OK && r != Z_BUF_ERROR) || got != want) {
        free(out);
        return NULL;
    }
    return out;
}

void ros_selftest_pdriver(void)
{
    /* what the driver says it is */
    uint32_t info[8] = { 0 };
    int ok = !swi(XPDriver_Info, info);
    ok = ok && info[0] == ((PDRIVER_TYPE << 16) | PDRIVER_VERSION) &&
         info[1] == PDRIVER_DPI && info[2] == PDRIVER_DPI &&
         (info[3] & (1u << 29)) && (info[3] & (1u << 25)) &&
         !strcmp(ros_ptr(info[4]), "PDF");
    uint32_t page[8] = { 0 };
    ok = ok && !swi(XPDriver_PageSize, page) && page[1] == 595276 && page[2] == 841890 &&
         page[3] == 28346 && page[5] == 595276 - 28346;
    uint32_t none[8] = { 0 };
    int nojob = swi(XPDriver_GiveRectangle, none) != 0;
    check(ok && nojob, "PDriver: Info gives type 64, version 1.00, 180 dpi, DeclareFont and "
          "arbitrary transformations, the printer \"PDF\"; PageSize gives A4 and its 10 mm "
          "margins; GiveRectangle with no job selected is an error", NULL);

    char dir[700];
    if (!scratch_disc("PTest", dir, sizeof dir)) {
        check(0, "PDriver: a scratch disc for the print job", NULL);
        return;
    }

    /* a job: one rectangle, 200 x 100 OS units, plotted red with a green
     * square in its bottom left corner */
    uint32_t open[8] = { 0x8F, str("HostFS::PTest.$.Job") };
    int job = step("OS_Find", swi(XOS_Find, open), open) && open[0] != 0;
    uint32_t handle = open[0];
    uint32_t sel[8] = { handle, str("Self test") };
    job = job && step("SelectJob", swi(XPDriver_SelectJob, sel), sel);

    uint32_t *blk = ros_rma_alloc(16 * 4);
    uint32_t rect = ros_addr(blk), trans = rect + 16, pos = rect + 32, out = rect + 48;
    ros_st32(rect + 0, 0), ros_st32(rect + 4, 0);
    ros_st32(rect + 8, 200), ros_st32(rect + 12, 100);
    ros_st32(trans + 0, 0x10000), ros_st32(trans + 4, 0);
    ros_st32(trans + 8, 0), ros_st32(trans + 12, 0x10000);
    ros_st32(pos + 0, 100000), ros_st32(pos + 4, 200000);
    uint32_t give[8] = { 42, rect, trans, pos, 0xFFFFFF00u };
    job = job && step("GiveRectangle", swi(XPDriver_GiveRectangle, give), give);

    uint32_t draw[8] = { 1, out, 1, 0 };
    job = job && step("DrawPage", swi(XPDriver_DrawPage, draw), draw);
    int rects = 0;
    while (job && draw[0]) {
        rects++;
        if (draw[2] != 42)
            job = 0;
        /* deselected and selected again before the band is drawn, as
         * OvationPro does between bands: its drawing must still reach the
         * page, which the pixels below check */
        uint32_t off[8] = { 0, 0 }, on[8] = { handle, 0 };
        job = job && step("SelectJob 0", swi(XPDriver_SelectJob, off), off) &&
              step("SelectJob again", swi(XPDriver_SelectJob, on), on);
        /* the rectangle the driver wants, in the application's own units */
        int32_t x0 = (int32_t)ros_ld32(out), y0 = (int32_t)ros_ld32(out + 4);
        int32_t x1 = (int32_t)ros_ld32(out + 8), y1 = (int32_t)ros_ld32(out + 12);
        uint32_t red[8] = { 0x0000FF00u, 0, 0, 0, 0 };          /* &BBGGRR00: red */
        job = job && !swi(XColourTrans_SetGCOL, red);
        uint32_t mv[8] = { 4, (uint32_t)x0, (uint32_t)y0 };
        uint32_t fill[8] = { 101, (uint32_t)(x1 - 1), (uint32_t)(y1 - 1) };
        job = job && !swi(XOS_Plot, mv) && !swi(XOS_Plot, fill);
        uint32_t green[8] = { 0x00FF0000u, 0, 0, 0, 0 };        /* &BBGGRR00: green */
        uint32_t mv2[8] = { 4, 0, 0 };
        uint32_t fill2[8] = { 101, 9, 9 };
        job = job && !swi(XColourTrans_SetGCOL, green) &&
              !swi(XOS_Plot, mv2) && !swi(XOS_Plot, fill2);
        uint32_t next[8] = { 0, out };
        job = job && step("GetRectangle", swi(XPDriver_GetRectangle, next), next);
        draw[0] = next[0], draw[2] = next[2];
    }
    uint32_t end[8] = { handle };
    job = job && step("EndJob", swi(XPDriver_EndJob, end), end);
    uint32_t close[8] = { 0, handle };
    job = job && step("close", swi(XOS_Find, close), close);
    /* the file: a PDF, with the band's pixels in it */
    size_t n = 0;
    uint8_t *pdf = NULL;
    /* HostFS gives the file the host name its type asks for ("Job,ffd"),
     * so the leaf is found rather than assumed */
    char path[780];
    path[0] = 0;
    DIR *d = opendir(dir);
    struct dirent *de;
    while (d && (de = readdir(d)))
        if (!strncmp(de->d_name, "Job", 3)) {
            snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
            break;
        }
    if (d)
        closedir(d);
    FILE *f = path[0] ? fopen(path, "rb") : NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        n = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        pdf = malloc(n ? n : 1);
        if (pdf && fread(pdf, 1, n, f) != n)
            free(pdf), pdf = NULL;
        fclose(f);
    }
    int isdf = pdf && n > 400 && !memcmp(pdf, "%PDF-", 5) &&
               memmem(pdf, n, "%%EOF", 5) != NULL;
    check(job && rects == 1 && isdf,
          "PDriver: a print job of one 200 x 100 rectangle gives one band, and the file it was "
          "opened on holds a PDF", "%s%s%zu bytes, %d band(s)", why[0] ? why : "",
          why[0] ? "; " : "", n, rects);

    if (isdf && getenv("ROSGD_PDRIVER_KEEP")) {       /* debugging */
        FILE *k = fopen(getenv("ROSGD_PDRIVER_KEEP"), "wb");
        if (k)
            fwrite(pdf, 1, n, k), fclose(k);
    }
    int pixels = 0;
    if (isdf) {
        /* 200 x 100 pixels, RGB, the top row first */
        uint8_t *px = image_pixels(pdf, n, 200u * 100u * 3u);
        if (px) {
            const uint8_t *top = px;                    /* y = 99: red */
            const uint8_t *bottom = px + 99u * 200u * 3u;   /* y = 0: green at the left */
            pixels = top[0] == 255 && top[1] == 0 && top[2] == 0 &&
                     bottom[0] == 0 && bottom[1] == 255 && bottom[2] == 0 &&
                     bottom[60] == 255 && bottom[61] == 0 && bottom[62] == 0;
            free(px);
        }
    }
    check(pixels, "PDriver: the PDF's image is the band the application plotted -- red, with the "
          "green square it drew in the bottom left corner", NULL);

    free(pdf);

    /* ---- printer:, and where a job goes ---- */

    /* Printer$Out is the scratch disc, so a job's dated name lands there */
    uint32_t setvar[8] = { str("Printer$Out"), str("HostFS::PTest.$.Out"), 19, 0, 0 };
    int p2 = !swi(XOS_SetVarVal, setvar);

    /* a job written straight to printer: */
    const char *body = "%PDF-1.7\n% not a real one\n";
    uint32_t po[8] = { 0x80, str("printer:") };          /* OpenOut */
    p2 = p2 && !swi(XOS_Find, po) && po[0] != 0;
    uint32_t ph = po[0];
    uint32_t bytes = (uint32_t)strlen(body);
    void *buf = ros_rma_alloc(bytes);
    memcpy(buf, body, bytes);
    uint32_t pw[8] = { 2, ph, ros_addr(buf), bytes };
    p2 = p2 && !swi(XOS_GBPB, pw);
    uint32_t pc[8] = { 0, ph };
    p2 = p2 && !swi(XOS_Find, pc);
    ros_rma_free(buf);

    /* it is in Out, under a dated name, typed &ADF because it is a PDF */
    char outdir[760];
    snprintf(outdir, sizeof outdir, "%s/Out", dir);
    char found[300] = "";
    size_t found_len = 0;
    DIR *od = opendir(outdir);
    struct dirent *oe;
    while (od && (oe = readdir(od)))
        if (!strncmp(oe->d_name, "Print-", 6)) {
            snprintf(found, sizeof found, "%s", oe->d_name);
            char fp[1100];
            snprintf(fp, sizeof fp, "%s/%s", outdir, oe->d_name);
            struct stat sb;
            if (!stat(fp, &sb))
                found_len = (size_t)sb.st_size;
            break;
        }
    if (od)
        closedir(od);
    int dated = found[0] && strstr(found, ",adf") && found_len == strlen(body);
    check(p2 && dated, "printer: a job opened on it, written and closed, is a file in "
          "Printer$Out under a dated name, typed &ADF because it is a PDF",
          "\"%s\", %zu bytes", found[0] ? found : "(none)", found_len);

    /* *PrintTo file sends the next job to a name of its own */
    uint32_t cmd[8] = { str("PrintTo file HostFS::PTest.$.Out.Named") };
    int named = !swi(XOS_CLI, cmd);
    uint32_t po2[8] = { 0x80, str("printer:") };
    named = named && !swi(XOS_Find, po2) && po2[0] != 0;
    uint32_t pw2[8] = { 2, po2[0], ros_addr(blk), 4 };   /* any four bytes */
    named = named && !swi(XOS_GBPB, pw2);
    uint32_t pc2[8] = { 0, po2[0] };
    named = named && !swi(XOS_Find, pc2);
    char namedpath[800];
    snprintf(namedpath, sizeof namedpath, "%s/Named,ffd", outdir);
    struct stat nb;
    int there = stat(namedpath, &nb) == 0 && nb.st_size == 4;
    uint32_t back[8] = { str("PrintTo") };
    named = named && !swi(XOS_CLI, back);
    check(named && there, "printer: *PrintTo file names where the job goes, and what is not a "
          "PDF is left as data; *PrintTo with nothing goes back to the dated name", NULL);

    /* the whole way an application prints: a job on printer:, drawn, ended,
     * and a PDF in Printer$Out with nothing else said */
    uint32_t wo[8] = { 0x80, str("printer:Whole") };
    int whole = !swi(XOS_Find, wo) && wo[0] != 0;
    uint32_t wsel[8] = { wo[0], str("Whole job") };
    whole = whole && !swi(XPDriver_SelectJob, wsel);
    uint32_t wgive[8] = { 7, rect, trans, pos, 0xFFFFFF00u };
    whole = whole && !swi(XPDriver_GiveRectangle, wgive);
    uint32_t wdraw[8] = { 1, out, 1, 0 };
    whole = whole && !swi(XPDriver_DrawPage, wdraw);
    while (whole && wdraw[0]) {
        uint32_t c[8] = { 0x00FF00FFu, 0, 0, 0, 0 };
        uint32_t mv[8] = { 4, 0, 0 }, fl[8] = { 101, 199, 99 };
        whole = whole && !swi(XColourTrans_SetGCOL, c) && !swi(XOS_Plot, mv) && !swi(XOS_Plot, fl);
        uint32_t nx[8] = { 0, out };
        whole = whole && !swi(XPDriver_GetRectangle, nx);
        wdraw[0] = nx[0];
    }
    uint32_t wend[8] = { wo[0] };
    whole = whole && !swi(XPDriver_EndJob, wend);
    uint32_t wclose[8] = { 0, wo[0] };
    whole = whole && !swi(XOS_Find, wclose);
    char wpath[800];
    snprintf(wpath, sizeof wpath, "%s/Whole,adf", outdir);
    struct stat wb;
    int landed = stat(wpath, &wb) == 0 && wb.st_size > 400;
    char wh[8] = { 0 };
    FILE *wf = fopen(wpath, "rb");
    if (wf) {
        if (fread(wh, 1, 5, wf) != 5)
            wh[0] = 0;
        fclose(wf);
    }
    check(whole && landed && !memcmp(wh, "%PDF-", 5),
          "printing, the whole way: a job opened on printer:, selected, drawn and ended is a "
          "PDF in Printer$Out, with nothing else told to the application", "%lld bytes",
          landed ? (long long)wb.st_size : -1);

    /* ---- a real printer, when one is named ----
     *
     * ROSGD_PRINTER=ipp://printer.local:631/ipp/print prints a page on it.
     * The box asks the printer what it takes, rasters the page itself if it
     * takes no PDF, and sends it. Without the variable nothing is printed,
     * so an ordinary run never uses anybody's paper. */
    const char *uri = getenv("ROSGD_PRINTER");
    if (uri && *uri) {
        char cmdline[400];
        snprintf(cmdline, sizeof cmdline, "PrintTo %s", uri);
        uint32_t to[8] = { str(cmdline) };
        int live = !swi(XOS_CLI, to);

        uint32_t lo[8] = { 0x80, str("printer:BoxTest") };
        live = live && !swi(XOS_Find, lo) && lo[0] != 0;
        uint32_t lsel[8] = { lo[0], str("ROSGD test page") };
        live = live && !swi(XPDriver_SelectJob, lsel);
        /* a page: a red panel with a white square in its corner, and rules
         * across it, 400 x 300 OS units placed 100 pt in and 400 pt up */
        uint32_t *lb = ros_rma_alloc(16 * 4);
        uint32_t lrect = ros_addr(lb), ltrans = lrect + 16, lpos = lrect + 32, lout = lrect + 48;
        ros_st32(lrect + 0, 0), ros_st32(lrect + 4, 0);
        ros_st32(lrect + 8, 400), ros_st32(lrect + 12, 300);
        ros_st32(ltrans + 0, 0x10000), ros_st32(ltrans + 4, 0);
        ros_st32(ltrans + 8, 0), ros_st32(ltrans + 12, 0x10000);
        ros_st32(lpos + 0, 100000), ros_st32(lpos + 4, 400000);
        uint32_t lgive[8] = { 1, lrect, ltrans, lpos, 0xFFFFFF00u };
        live = live && !swi(XPDriver_GiveRectangle, lgive);
        uint32_t ldraw[8] = { 1, lout, 1, 0 };
        live = live && !swi(XPDriver_DrawPage, ldraw);
        while (live && ldraw[0]) {
            uint32_t red[8] = { 0x0000C000u, 0, 0, 0, 0 };
            uint32_t m1[8] = { 4, 0, 0 }, f1[8] = { 101, 399, 299 };
            live = live && !swi(XColourTrans_SetGCOL, red) && !swi(XOS_Plot, m1) &&
                   !swi(XOS_Plot, f1);
            uint32_t white[8] = { 0xFFFFFF00u, 0, 0, 0, 0 };
            uint32_t m2[8] = { 4, 20, 20 }, f2[8] = { 101, 99, 99 };
            live = live && !swi(XColourTrans_SetGCOL, white) && !swi(XOS_Plot, m2) &&
                   !swi(XOS_Plot, f2);
            for (int k = 0; live && k < 8; k++) {   /* rules, so the scale shows */
                uint32_t mv[8] = { 4, 0, (uint32_t)(40 + 30 * k) };
                uint32_t ln[8] = { 5, 399, (uint32_t)(40 + 30 * k) };
                live = !swi(XOS_Plot, mv) && !swi(XOS_Plot, ln);
            }
            uint32_t nx[8] = { 0, lout };
            live = live && !swi(XPDriver_GetRectangle, nx);
            ldraw[0] = nx[0];
        }
        uint32_t lend[8] = { lo[0] };
        live = live && !swi(XPDriver_EndJob, lend);
        uint32_t lclose[8] = { 0, lo[0] };
        live = live && !swi(XOS_Find, lclose);
        ros_rma_free(lb);
        uint32_t lback[8] = { str("PrintTo") };
        swi(XOS_CLI, lback);
        check(live, "PDriver: the printer ROSGD_PRINTER names took a page from the box",
              "%s", uri);
    }

    ros_rma_free(blk);
    scratch_gone("PTest", dir);
}
