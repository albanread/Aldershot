/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_sysvars.c: system variables and GSTrans, against the kernel's
 * own (Kernel/s/Arthur2). They are used through the SWIs, as programs call
 * them.
 */
#include <string.h>
#include <time.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "selftest.h"

#define check ros_check

static uint32_t name_at, value_at, buf;         /* arena scratch */

static int swi(uint32_t n, uint32_t r[5])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 5 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 5 * sizeof r[0]);
    return s.v;
}

static uint32_t errnum(const uint32_t r[5])
{
    return ((os_error *)ros_ptr(r[0]))->errnum;
}

static const char *errmess(const uint32_t r[5])
{
    return ((os_error *)ros_ptr(r[0]))->errmess;
}

/* OS_SetVarVal: value is len bytes (a string's terminator included). */
static int set(const char *name, const void *value, uint32_t len, uint32_t type, uint32_t r[5])
{
    strcpy(ros_ptr(name_at), name);
    memcpy(ros_ptr(value_at), value, len);
    r[0] = name_at, r[1] = value_at, r[2] = len, r[3] = 0, r[4] = type;
    return swi(XOS_SetVarVal, r);
}

static int set_str(const char *name, const char *value, uint32_t type)
{
    uint32_t r[5];
    return set(name, value, (uint32_t)strlen(value) + 1, type, r);
}

static int unset(const char *name, uint32_t ctx, uint32_t type, uint32_t r[5])
{
    strcpy(ros_ptr(name_at), name);
    r[0] = name_at, r[1] = 0, r[2] = (uint32_t)-1, r[3] = ctx, r[4] = type;
    return swi(XOS_SetVarVal, r);
}

/* OS_ReadVarVal into buf, 0-terminated here for comparing. */
static int get(const char *name, uint32_t size, uint32_t ctx, uint32_t want, uint32_t r[5])
{
    strcpy(ros_ptr(name_at), name);
    memset(ros_ptr(buf), 0, 256);
    r[0] = name_at, r[1] = buf, r[2] = size, r[3] = ctx, r[4] = want;
    int v = swi(XOS_ReadVarVal, r);
    if (!v && (int32_t)r[2] >= 0 && r[2] < 256)
        ros_st8(buf + r[2], 0);
    return v;
}

static int reads(const char *name, uint32_t want, const char *expect, uint32_t type)
{
    uint32_t r[5];
    int v = get(name, 255, 0, want, r);
    int ok = !v && r[2] == strlen(expect) && strcmp(ros_ptr(buf), expect) == 0 && r[4] == type;
    if (!ok)
        ros_console_printf("        %s: V%d \"%s\" R2 %d R4 %u; want \"%s\" type %u\n", name, v,
                           v ? errmess(r) : (const char *)ros_ptr(buf), (int)r[2], r[4], expect,
                           type);
    return ok;
}

/* GSTrans of text into buf; the count, C and V. */
static int gstrans(const char *text, uint32_t size_flags, uint32_t r[5], int *c)
{
    strcpy(ros_ptr(value_at), text);
    memset(ros_ptr(buf), 0, 256);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = value_at, s.r[1] = buf, s.r[2] = size_flags;
    ros_swi(&s, XOS_GSTrans);
    memcpy(r, s.r, 5 * sizeof r[0]);
    *c = (int)s.c;
    return s.v;
}

static int trans(const char *text, const char *expect, uint32_t n)
{
    uint32_t r[5];
    int c, v = gstrans(text, 255, r, &c);
    int ok = !v && !c && r[2] == n && memcmp(ros_ptr(buf), expect, n) == 0;
    if (!ok)
        ros_console_printf("        GSTrans \"%s\": V%d C%d R2 %u \"%.*s\"\n", text, v, c, r[2],
                           (int)(r[2] < 256 ? r[2] : 0), (const char *)ros_ptr(buf));
    return ok;
}

/* A client's code variable: its code begins with the kernel's form,
 * LDR PC, [PC, #0], through native entries standing for its routines. */
static char written[64];
static void code_write(struct ros_cpu *s)
{
    uint32_t n = s->r[2] < sizeof written - 1 ? s->r[2] : sizeof written - 1;
    memcpy(written, ros_ptr(s->r[1]), n);
    written[n] = 0;
    s->v = 0;
    s->r[15] = s->r[14];
}

static void code_read(struct ros_cpu *s)
{
    strcpy(ros_ptr(value_at + 128), "from code");
    s->r[0] = value_at + 128;
    s->r[2] = 9;
    s->v = 0;
    s->r[15] = s->r[14];
}

void ros_selftest_sysvars(void)
{
    uint32_t r[5];
    name_at = ros_addr(ros_rma_alloc(256));
    value_at = ros_addr(ros_rma_alloc(256));
    buf = ros_addr(ros_rma_alloc(256));

    /* ---- the kernel's own ---- */
    char year[8];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(year, sizeof year, "%Y", &tm);
    int k1 = reads("Sys$RCLimit", 3, "256", 0) && reads("Alias$.", 0, "Cat ", 0) &&
             reads("Sys$Year", 0, year, 0);
    get("Sys$Time", 255, 0, 0, r);
    const char *t = ros_ptr(buf);
    int k2 = r[2] == 8 && t[2] == ':' && t[5] == ':';
    check(k1 && k2, "OS_ReadVarVal -- the kernel's own: Sys$RCLimit, Alias$., Sys$Year, Sys$Time",
          "Sys$Time \"%s\"", t);

    /* ---- strings are GSTransed as they are set ---- */
    set_str("Test$Str", "hello <Alias$.>world\r", 0);
    int s1 = reads("Test$Str", 0, "hello Cat world", 0);
    get("TEST$str", 255, 0, 0, r);
    int s2 = strcmp(ros_ptr(r[3]), "Test$Str") == 0;
    set_str("TEST$STR", "x\r", 0);
    get("test$str", 255, 0, 0, r);
    int s3 = strcmp(ros_ptr(r[3]), "Test$Str") == 0 && strcmp(ros_ptr(buf), "x") == 0;
    check(s1 && s2 && s3,
          "OS_SetVarVal, string -- expanded as it is set; names without case, the first spelling "
          "kept", NULL);

    /* ---- numbers, macros, literals ---- */
    int32_t minus = -123;
    set("Test$Num", &minus, 4, 1, r);
    get("Test$Num", 255, 0, 0, r);
    int n1 = r[2] == 4 && r[4] == 1 && (int32_t)ros_ld32(buf) == -123;
    int n2 = reads("Test$Num", 3, "-123", 0);
    check(n1 && n2, "OS_SetVarVal, number -- four bytes as it is; expanded, in decimal", NULL);

    set_str("Test$Mac", "<Test$Num>!\r", 2);
    int m1 = reads("Test$Mac", 0, "<Test$Num>!", 2) && reads("Test$Mac", 3, "-123!", 2);
    int32_t seven = 7;
    set("Test$Num", &seven, 4, 1, r);
    int m2 = reads("Test$Mac", 3, "7!", 2);
    set("Test$Lit", "a<b>", 4, 4, r);
    int m3 = reads("Test$Lit", 0, "a<b>", 0);
    check(m1 && m2 && m3,
          "OS_SetVarVal, macro and literal -- a macro expands as it is read, a literal never", NULL);

    /* ---- the buffer ---- */
    set_str("Test$Str", "hello world\r", 0);
    int v = get("Test$Str", (uint32_t)-1, 0, 0, r);
    int b1 = v && errnum(r) == 0x1E4 && r[2] == ~11u;
    v = get("Test$Str", 3, 0, 0, r);
    int b2 = v && errnum(r) == 0x1E4 && r[2] == 3 && memcmp(ros_ptr(buf), "hel", 3) == 0;
    check(b1 && b2,
          "OS_ReadVarVal -- R2 < 0: NOT the length; too small: as many as fit, and &1E4", NULL);

    v = get("Nope$X", 255, 0, 0, r);
    check(v && errnum(r) == 0x124 && r[2] == 0 &&
              strcmp(errmess(r), "System variable 'Nope$X' not found") == 0,
          "OS_ReadVarVal -- not found: &124 naming it, R2 = 0", "%s", v ? errmess(r) : "");

    /* ---- wildcards: enumeration and deletion ---- */
    set_str("Test$C", "3\r", 0);
    set_str("Test$A", "1\r", 0);
    set_str("Test$B", "2\r", 0);
    char seen[8] = "";
    uint32_t ctx = 0;
    for (int i = 0; i < 6; i++) {
        if (get("Test$#", 255, ctx, 0, r))
            break;
        ctx = r[3];
        strcat(seen, ros_ptr(buf));
    }
    int w1 = strcmp(seen, "123") == 0;
    int w2 = reads("t*$c", 0, "3", 0);
    int deleted = 0;
    for (ctx = 0; deleted < 6 && !unset("Test$#", ctx, 0, r); deleted++)
        ctx = r[3];
    int w3 = deleted == 3 && errnum(r) == 0x124 && get("Test$B", 255, 0, 0, r);
    check(w1 && w2 && w3,
          "Wildcards -- # and * without case; R3 enumerates in order and deletes them all",
          "seen \"%s\", deleted %d", seen, deleted);

    /* ---- what is refused ---- */
    set("Bad*Name", "x\r", 3, 0, r);
    int e1 = errnum(r) == 0x121;
    set("", "x\r", 3, 0, r);
    int e2 = errnum(r) == 0x121;
    set("Test$T", "x\r", 3, 5, r);
    int e3 = errnum(r) == 0x122;
    set("Test$M", "ab\x01", 3, 2, r);
    int e4 = errnum(r) == 0x120;
    check(e1 && e2 && e3 && e4,
          "OS_SetVarVal -- a wildcard or empty name &121, a bad type &122, a bad macro &120", NULL);

    /* ---- code variables ---- */
    set_str("Sys$ReturnCode", "5\r", 0);
    int c1 = reads("Sys$ReturnCode", 0, "5", 0);
    set("Sys$ReturnCode", "300\r", 5, 0, r);
    int c2 = errnum(r) == 0x1E2 && strcmp(errmess(r), "Return code limit exceeded") == 0 &&
             reads("Sys$ReturnCode", 0, "300", 0);
    set("Sys$ReturnCode", "-1\r", 4, 0, r);
    int c3 = errnum(r) == 0x1E2 && strcmp(errmess(r), "Negative return code") == 0;
    set_str("Sys$ReturnCode", "0\r", 0);
    unset("Sys$Time", 0, 0, r);
    int c4 = !get("Sys$Time", 255, 0, 0, r);
    check(c1 && c2 && c3 && c4,
          "Code variables -- Sys$ReturnCode against Sys$RCLimit; not deleted but by R4 = 16", NULL);

    uint32_t code[4] = { 0xE59FF000u, 0xE59FF000u, ros_native_entry(code_write, "test:write"),
                         ros_native_entry(code_read, "test:read") };
    set("Test$Code", code, sizeof code, 16, r);
    int d1 = reads("Test$Code", 0, "from code", 0);
    set_str("Test$Code", "set <Test$Lit>\r", 0);
    int d2 = strcmp(written, "set a<b>") == 0;
    unset("Test$Code", 0, 16, r);
    int d3 = get("Test$Code", 255, 0, 0, r) != 0;
    check(d1 && d2 && d3,
          "Code variables -- a client's, through its LDR PC: read, written expanded, deleted",
          "\"%s\"", written);

    /* The Wimp's Wimp$State: a block whose entries are its routines
     * (MOV PC,LR; then the read), compiled in the ROM where it gave them */
    uint32_t w[5] = { 3, 0, 0, 0, 0 };
    int ws1 = !swi(XWimp_ReadSysInfo, w);
    const char *state = w[0] ? "desktop" : "commands";
    int ws2 = ws1 && reads("Wimp$State", 0, state, 0) && trans("<Wimp$State>", state, strlen(state));
    check(ws2, "Code variables -- the Wimp's Wimp$State, its entries run where the Wimp gave them",
          "want \"%s\"", state);

    /* ---- GSTrans ---- */
    int g1 = trans("|<A|>|!B|G|?<65>", "<A>\xC2\x07\x7F" "A", 7) &&
             trans("<Nope$X>z", "z", 1) && trans("<>", "<>", 2) &&
             trans("<Test$Lit>", "a<b>", 4);
    set_str("Test$Mac", "[<Test$Num>]\r", 2);
    set_str("Test$Mac2", "<Test$Mac><Test$Mac>\r", 2);
    int g2 = trans("<Test$Mac2>.", "[7][7].", 7);
    check(g1 && g2,
          "OS_GSTrans -- |-escapes, <number>, unknown and empty <>, variables, nested macros",
          NULL);

    int c;
    v = gstrans("\"a b\"  rest", 255, r, &c);
    int q1 = !v && r[2] == 3 && memcmp(ros_ptr(buf), "a b", 3) == 0 &&
             strcmp(ros_ptr(r[0]), "rest") == 0;
    v = gstrans("ab cd", 255 | 1u << 29, r, &c);
    int q2 = !v && r[2] == 2;
    v = gstrans("ab cd", 255, r, &c);
    int q3 = !v && r[2] == 5;
    v = gstrans("\"abc", 255, r, &c);
    int q4 = v && errnum(r) == 0xFD;
    v = gstrans("abcdef", 3, r, &c);
    int q5 = !v && c && r[2] == 3;
    check(q1 && q2 && q3 && q4 && q5,
          "OS_GSTrans -- quotes, space ending with bit 29, an unclosed quote &FD, overflow in C",
          NULL);

    /* ---- GSInit and GSRead, one character at a time ---- */
    strcpy(ros_ptr(value_at), "  x<Test$Lit>");
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = value_at, s.r[2] = 0;
    ros_swi(&s, XOS_GSInit);
    int i1 = s.r[1] == 'x' && !s.z;
    char got[16];
    int n = 0;
    for (;;) {
        ros_swi(&s, XOS_GSRead);
        if (s.v || s.c || n == 15)
            break;
        got[n++] = (char)s.r[1];
    }
    got[n] = 0;
    strcpy(ros_ptr(value_at), "   \r");
    ros_cpu_enter(&s);
    s.r[0] = value_at, s.r[2] = 0;
    ros_swi(&s, XOS_GSInit);
    check(i1 && strcmp(got, "xa<b>") == 0 && s.z,
          "OS_GSInit / OS_GSRead -- the first character, Z for empty, then one at a time",
          "\"%s\"", got);

    unset("Test$*", 0, 0, r);
    while (!unset("Test$*", 0, 0, r))
        ;
    ros_rma_free(ros_ptr(buf));
    ros_rma_free(ros_ptr(value_at));
    ros_rma_free(ros_ptr(name_at));
}
