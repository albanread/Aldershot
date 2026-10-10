/* Copyright 1996 Acorn Computers Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is a reimplementation in C of RISC OS Open's ARM assembler source
 * (Sources/Video/Render/Colours: s.Calibrate, s.DevicePal, s.DevicePal2, s.NewModels, s.FileCal).
 */

/* models.c: ColourTrans's calibration, colour models and files (s/Calibrate,
 * DevicePal, DevicePal2, NewModels, FileCal).
 *
 *   - Calibration. By default there is none, and then nothing changes. A
 *     calibration table changes the colour that is looked up in palette
 *     modes. An old-style table has three lists of device levels and the
 *     colours they give, and the code interpolates between them. A
 *     new-style table has an ideal black and white, gamma tables and a
 *     post-processing SWI.
 *   - Conversion of RGB to and from CIE XYZ, HSV and CMYK, in 16.16 fixed
 *     point.
 *   - The desktop save: *ColourTransMapSize, *ColourTransMap and
 *     *ColourTransLoadings. The files are written as the original writes
 *     them.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "ct.h"

enum { CAL_BLACK = 4, CAL_WHITE = 8, CAL_POSTSWI = 12, CAL_COUNT = 16, CAL_GAMMA = 20 };

/* ---- the old format -------------------------------------------------------------------------- */

/* interpolate_device_colour. L is a list of n entries and v is the device
 * level. */
static void interpolate(uint32_t L, uint32_t n, uint32_t v, int32_t acc[3])
{
    uint32_t prev = 0, j = L;
    for (; n; n--, j += 4) {
        if ((int32_t)ros_ld8(j) >= (int32_t)v)
            break;
        prev = j;
    }
    if (!n)
        return;                             /* the original jumps to 0 */
    if (ros_ld8(j) == v)
        prev = j;
    if (!prev)
        return;
    int32_t c0[3] = { (int32_t)ros_ld8(prev + 1), (int32_t)ros_ld8(prev + 2), (int32_t)ros_ld8(prev + 3) };
    for (int k = 0; k < 3; k++)
        acc[k] += c0[k] << 16;
    if (prev == j)
        return;
    uint32_t dd = ros_ld8(j) - ros_ld8(prev);
    uint32_t f = dd ? ((v - ros_ld8(prev)) << 16) / dd : 0;
    for (int k = 0; k < 3; k++)
        acc[k] += (int32_t)(f * (uint32_t)((int32_t)ros_ld8(j + 1 + (uint32_t)k) - c0[k]));
}

static uint32_t old_convert(uint32_t c, uint32_t t)
{
    int32_t acc[3] = { 0, 0, 0 };
    uint32_t L = t + 12;
    for (uint32_t ch = 0; ch < 3; ch++) {
        uint32_t n = ros_ld32(t + 4 * ch);
        interpolate(L, n, (c >> (8 * (ch + 1))) & 255, acc);
        L += 4 * n;
    }
    uint32_t out = 0;
    for (int k = 0; k < 3; k++) {
        int32_t v = acc[k] >> 16;
        v = v >= 256 ? 255 : v < 0 ? 0 : v;
        out |= (uint32_t)v << (8 * (k + 1));
    }
    return out;
}

/* ---- the new format -------------------------------------------------------------------------- */

/* do_grey_shift. k is the complemented ideal white for the white pass, and
 * the ideal black for the black pass. */
static void grey_shift(uint32_t k, uint32_t x[3])
{
    uint32_t mx = x[0], mn = x[0];
    for (int i = 1; i < 3; i++) {
        if (mx < x[i]) mx = x[i];
        if (mn > x[i]) mn = x[i];
    }
    if (mx == 0)
        return;
    uint32_t q = mx == mn ? 256 : (mn << 8) / mx;
    for (int i = 0; i < 3; i++) {
        uint32_t kb = (k >> (8 * (i + 1))) & 255;
        if (!kb)
            continue;
        uint32_t v = (x[i] * (0xFF00 - q * kb)) >> 16;
        if (v)
            v++;
        x[i] = v > 255 ? 255 : v;
    }
}

static os_error *new_convert(uint32_t c, uint32_t t, uint32_t *out)
{
    uint32_t x[3] = { (c >> 8) & 255, (c >> 16) & 255, c >> 24 };
    uint32_t white = ros_ld32(t + CAL_WHITE), black = ros_ld32(t + CAL_BLACK);
    if (white != 0xFFFFFF00u)
        grey_shift(~white, x);
    if (black != 0) {
        for (int i = 0; i < 3; i++)
            x[i] ^= 255;
        grey_shift(black, x);
        for (int i = 0; i < 3; i++)
            x[i] ^= 255;
    }
    uint32_t three = ros_ld32(t + CAL_COUNT) == 3, g = t + CAL_GAMMA;
    uint32_t r = ros_ld8(g + x[0]), gr = ros_ld8(g + (three ? 256 : 0) + x[1]);
    uint32_t b = ros_ld8(g + (three ? 512 : 0) + x[2]);
    *out = b << 24 | gr << 16 | r << 8;
    uint32_t swi = ros_ld32(t + CAL_POSTSWI);
    if (swi) {
        uint32_t reg[10] = { swi, *out, *out };
        os_error *e;
        ct_swi(swi | 0x20000, reg, &e);
        if (e)
            return e;
        *out = reg[2];
    }
    return NULL;
}

/* Converts a colour through a table. A table of 0 means the current
 * calibration. */
static os_error *convert(uint32_t c, uint32_t t, uint32_t *out)
{
    if (!t) {
        if (!ct.calib) {
            *out = c;
            return NULL;
        }
        t = ct.calib;
        if (ct.calib_new)
            return new_convert(c, t, out);
        *out = old_convert(c, t);
        return NULL;
    }
    if (ros_ld32(t) == 0)
        return new_convert(c, t, out);
    *out = old_convert(c, t);
    return NULL;
}

uint32_t ct_convert_screen_colour(uint32_t c)
{
    uint32_t out = c;
    convert(c, 0, &out);
    return out;
}

/* ---- SetCalibration, ReadCalibration ----------------------------------------------------------- */

static void calibration_changed(void)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[1] = 0x5B;                          /* Service_CalibrationChanged */
    ros_service_call(&s);
}

static void drop_calibration(void)
{
    if (ct.calib)
        ros_rma_free(ros_ptr(ct.calib));
    ct.calib = 0;
}

static os_error *bad_calibration(void)
{
    drop_calibration();
    calibration_changed();
    return ct_err(CT_BADCALIB);
}

static os_error *set_calibration(uint32_t t)
{
    ct_init_cache();
    drop_calibration();
    if (t == 0 || ros_ld32(t) == 0) {       /* the new format, or no table */
        if (!t)
            return NULL;
        uint32_t n = ros_ld32(t + CAL_COUNT);
        if (n != 1 && n != 3)
            return bad_calibration();
        uint32_t size = CAL_GAMMA + 256 * n;
        void *block = ros_rma_alloc(size);
        if (!block)
            return ros_error(0x101, "No room in RMA");
        ct.calib = ros_addr(block);
        memcpy(ros_ptr(ct.calib), ros_ptr(t), size);
        ct.calib_new = 0xFFFFFFFFu;
        calibration_changed();
        return NULL;
    }
    int32_t n[3] = { (int32_t)ros_ld32(t), (int32_t)ros_ld32(t + 4), (int32_t)ros_ld32(t + 8) };
    for (int i = 0; i < 3; i++)
        if (n[i] < 2)
            return bad_calibration();
    /* The counts come from the caller. Three counts of 2 GiB would wrap in 32
     * bits, so the sum is made in 64 bits. */
    int64_t size64 = 12 + 4 * ((int64_t)n[0] + n[1] + n[2]);
    if (size64 > 1 << 20)
        return bad_calibration();
    uint32_t size = (uint32_t)size64;
    void *block = ros_rma_alloc(size);
    if (!block)
        return ros_error(0x101, "No room in RMA");
    ct.calib = ros_addr(block);
    memcpy(ros_ptr(ct.calib), ros_ptr(t), size);
    uint32_t p = t + 12;
    for (int i = 0; i < 3; i++) {
        if (ros_ld32(p) & 255)
            return bad_calibration();
        uint32_t last = 0;
        for (int32_t k = 0; k < n[i]; k++, p += 4) {
            uint32_t d = ros_ld32(p) & 255;
            if (d < last)
                return bad_calibration();
            last = d;
        }
        if (last != 255)
            return bad_calibration();
    }
    ct.calib_new = 0;
    calibration_changed();
    return NULL;
}

static uint32_t calibration_size(void)
{
    if (!ct.calib)
        return 0;
    if (ct.calib_new)
        return CAL_GAMMA + 256 * ros_ld32(ct.calib + CAL_COUNT);
    return 12 + 4 * (ros_ld32(ct.calib) + ros_ld32(ct.calib + 4) + ros_ld32(ct.calib + 8));
}

/* ---- CIE ------------------------------------------------------------------------------------- */

static const int32_t to_cie[3][3] = {
    { 0x7A6BC1, 0x4C6E67, 0x2CB2AD }, { 0x4372CE, 0xA7B146, 0x14DBEA }, { 0x051181, 0x29114A, 0xE86DEE },
};
static const int32_t from_cie[3][3] = {
    { 0x02BD4872, (int32_t)0xFEDAF45B, (int32_t)0xFF936FD5 },
    { (int32_t)0xFEE18A27, 0x02074BD3, 0x00087C2A },
    { 0x00235269, (int32_t)0xFFAAA2FC, 0x011AD439 },
};

static os_error *cie(struct ros_cpu *s, const int32_t m[3][3])
{
    int32_t out[3];
    for (int i = 0; i < 3; i++) {
        int64_t sum = 0;
        for (int j = 0; j < 3; j++)
            sum += (int64_t)(int32_t)s->r[j] * m[i][j];
        int64_t top = sum >> 55;
        if (top != 0 && top != -1)
            return ct_err(CT_CONVOVER);
        out[i] = (int32_t)(sum >> 24);
    }
    for (int i = 0; i < 3; i++)
        s->r[i] = (uint32_t)out[i];
    return NULL;
}

/* ---- HSV and CMYK ---------------------------------------------------------------------------- */

/* DSdiv. It shifts num left by 16 into a 64-bit value and divides that by
 * den. The division is unsigned and truncating. */
static uint32_t dsdiv(int32_t num, uint32_t den)
{
    if (!den)
        return 0;
    uint64_t n = ((uint64_t)(uint32_t)(num >> 16) << 32) | (uint32_t)((uint32_t)num << 16);
    return (uint32_t)(n / den);
}

static int32_t mul16(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b) >> 16);
}

static void rgb_to_hsv(struct ros_cpu *s)
{
    int32_t r = (int32_t)s->r[0], g = (int32_t)s->r[1], b = (int32_t)s->r[2];
    int32_t mx = r, mn = r;
    if (g > mx) mx = g;
    if (b > mx) mx = b;
    if (g < mn) mn = g;
    if (b < mn) mn = b;
    int32_t d = mx - mn, H;
    uint32_t S = mx == 0 ? 0 : dsdiv(d, (uint32_t)mx);
    if (S == 0) {
        H = -1;
    } else {
        int32_t cr = (int32_t)dsdiv(mx - r, (uint32_t)d), cg = (int32_t)dsdiv(mx - g, (uint32_t)d);
        int32_t cb = (int32_t)dsdiv(mx - b, (uint32_t)d);
        H = 0;
        if (r == mx) H = cb - cg;
        if (g == mx) H = cr - cb + 0x20000;
        if (b == mx) H = cg - cr + 0x40000;
        H = (int32_t)((uint32_t)H * 60);
        if (H < 0)
            H += 360 << 16;
    }
    s->r[0] = (uint32_t)H, s->r[1] = S, s->r[2] = (uint32_t)mx;
}

static os_error *hsv_to_rgb(struct ros_cpu *s)
{
    int32_t H = (int32_t)s->r[0], S = (int32_t)s->r[1], V = (int32_t)s->r[2];
    if (S == 0) {
        if (H == 0)
            return ct_err(CT_BADHSV);
        s->r[0] = s->r[1] = s->r[2] = (uint32_t)V;
        return NULL;
    }
    if (H >= 360 << 16)
        H -= 360 << 16;
    uint32_t q = dsdiv(H, 60 << 16);
    int32_t i = (int32_t)q >> 16, f = (int32_t)(q & 0xFFFF);
    int32_t m = mul16(V, 0x10000 - S), n = mul16(0x10000 - mul16(S, f), V);
    int32_t k = mul16(0x10000 - mul16(S, 0x10000 - f), V);
    int32_t out[6][3] = { { V, k, m }, { n, V, m }, { m, V, k }, { m, n, V }, { k, m, V }, { V, m, n } };
    if (i < 0 || i > 5)
        i = 0;                              /* the original jumps past its table */
    for (int c = 0; c < 3; c++)
        s->r[c] = (uint32_t)out[i][c];
    return NULL;
}

static void rgb_to_cmyk(struct ros_cpu *s)
{
    int32_t c = 0x10000 - (int32_t)s->r[0], m = 0x10000 - (int32_t)s->r[1];
    int32_t y = 0x10000 - (int32_t)s->r[2], k = c;
    if (m < k) k = m;
    if (y < k) k = y;
    s->r[0] = (uint32_t)(c - k), s->r[1] = (uint32_t)(m - k), s->r[2] = (uint32_t)(y - k);
    s->r[3] = (uint32_t)k;
}

static void cmyk_to_rgb(struct ros_cpu *s)
{
    int32_t k = (int32_t)s->r[3];
    for (int i = 0; i < 3; i++) {
        int32_t v = (int32_t)s->r[i] + k;
        if (v > 0x10000)
            v = 0x10000;
        s->r[i] = (uint32_t)(0x10000 - v);
    }
}

/* ---- the files ------------------------------------------------------------------------------- */

static os_error *bput(uint32_t h, uint32_t c)
{
    uint32_t r[10] = { c, h };
    os_error *e;
    ct_swi(XOS_BPut, r, &e);
    return e;
}

static os_error *bput_s(uint32_t h, const char *s, size_t n)
{
    os_error *e = NULL;
    for (size_t i = 0; i < n && !e; i++)
        e = bput(h, (uint8_t)s[i]);
    return e;
}

/* output_number_r3. It writes " &" and then the hex digits, with leading
 * zeros stripped. */
static os_error *bput_n(uint32_t h, uint32_t v)
{
    char t[9];
    for (int i = 0; i < 8; i++)
        t[i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15];
    int k = 0;
    while (k < 7 && t[k] == '0')
        k++;
    os_error *e = bput_s(h, " &", 2);
    return e ? e : bput_s(h, t + k, (size_t)(8 - k));
}

static const uint32_t default_calibration[9] = { 2, 2, 2, 0, 0xFFFF, 0, 0xFF00FF, 0, 0xFF0000FF };

static os_error *write_calibration(uint32_t flags, uint32_t h)
{
    if (!ct.calib && !(flags & 1))
        return NULL;
    if (ct.calib && ct.calib_new)
        return NULL;
    uint32_t tab[64], words;
    if (ct.calib) {
        words = calibration_size() / 4;
        for (uint32_t i = 0; i < words && i < 64; i++)
            tab[i] = ros_ld32(ct.calib + 4 * i);
    } else {
        words = 9;
        memcpy(tab, default_calibration, sizeof default_calibration);
    }
    /* The first line is 18 bytes from "ColourTransMap". 18 is the length
     * for the size command, but this is the wrong string, so the line
     * ends with "Colo" from the string that follows. */
    os_error *e = bput_s(h, "ColourTransMapColo", 18);
    uint32_t sum = 0;
    for (int i = 0; i < 3 && !e; i++)
        sum += tab[i], e = bput_n(h, tab[i]);
    if (e || (e = bput(h, 10)) || (e = bput_s(h, "ColourTransMap", 14)))
        return e;
    int32_t top = (int32_t)(sum * 4 + 8), stop = top - 64;
    for (int32_t o = top; o >= 12; o -= 4) {
        if (o <= stop)
            break;                          /* the original's second line runs away */
        if ((uint32_t)o / 4 < 64 && (e = bput_n(h, tab[o / 4])))
            return e;
    }
    return bput(h, 10);
}

static os_error *write_loadings(uint32_t h)
{
    os_error *e = bput_s(h, "ColourTransLoadings", 19);
    uint32_t L = ct.loading;
    for (int i = 0; i < 3 && !e; i++)
        L >>= 8, e = bput_n(h, L & 255);
    return e ? e : bput(h, 10);
}

/* ---- the SWIs -------------------------------------------------------------------------------- */

os_error *ct_models_swi(struct ros_cpu *s, uint32_t n)
{
    uint32_t v;
    os_error *e;
    switch (n) {
    case 17: return set_calibration(s->r[0]);
    case 18:
        s->r[1] = calibration_size();
        if (s->r[0] && s->r[1])
            memcpy(ros_ptr(s->r[0]), ros_ptr(ct.calib), s->r[1]);
        return NULL;
    case 19:
        if ((e = convert(s->r[1], s->r[3], &v)))
            return e;
        s->r[2] = v;
        return NULL;
    case 20:
        /* The original returns to the caller's R5. Here it simply returns. */
        for (uint32_t i = 0; i < s->r[0]; i++) {
            if ((e = convert(ros_ld32(s->r[1] + 4 * i), s->r[3], &v)))
                return e;
            ros_st32(s->r[2] + 4 * i, v);
        }
        return NULL;
    case 21: return cie(s, to_cie);
    case 22: return cie(s, from_cie);
    case 23: return write_calibration(s->r[0], s->r[1]);
    case 24: rgb_to_hsv(s); return NULL;
    case 25: return hsv_to_rgb(s);
    case 26: rgb_to_cmyk(s); return NULL;
    case 27: cmyk_to_rgb(s); return NULL;
    case 32: return write_loadings(s->r[1]);
    default: return ct_err(CT_BADSWI);
    }
}
