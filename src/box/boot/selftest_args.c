/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_args.c: OS_EvaluateExpression and OS_ReadArgs, against the
 * kernel's own (Kernel/s/Arthur3, MoreSWIs): through the SWIs.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "selftest.h"

#define check ros_check

static uint32_t text, buf, keys;    /* arena scratch */

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

static int eval(const char *expr, uint32_t size, uint32_t r[5])
{
    strcpy(ros_ptr(text), expr);
    memset(ros_ptr(buf), 0, 256);
    r[0] = text, r[1] = buf, r[2] = size, r[3] = r[4] = 0;
    return swi(XOS_EvaluateExpression, r);
}

static int is_int(const char *expr, int32_t want)
{
    uint32_t r[5];
    int v = eval(expr, 255, r);
    int ok = !v && r[1] == 0 && (int32_t)r[2] == want;
    if (!ok)
        ros_console_printf("        %s: V%d R1 &%X R2 %d, want %d%s%s\n", expr, v, r[1],
                           (int)r[2], (int)want, v ? " -- " : "",
                           v ? ((os_error *)ros_ptr(r[0]))->errmess : "");
    return ok;
}

static int is_str(const char *expr, const char *want)
{
    uint32_t r[5];
    int v = eval(expr, 255, r);
    int ok = !v && r[1] == buf && r[2] == strlen(want) && memcmp(ros_ptr(buf), want, r[2]) == 0;
    if (!ok)
        ros_console_printf("        %s: V%d R2 %d \"%.*s\", want \"%s\"\n", expr, v, (int)r[2],
                           v ? 0 : (int)r[2], (const char *)ros_ptr(buf), want);
    return ok;
}

static int is_err(const char *expr, uint32_t want)
{
    uint32_t r[5];
    int v = eval(expr, 255, r);
    int ok = v && errnum(r) == want && r[1] == 0;
    if (!ok)
        ros_console_printf("        %s: V%d &%X R1 &%X, want error &%X\n", expr, v,
                           v ? errnum(r) : 0, r[1], want);
    return ok;
}

static void set_var(const char *name, const void *value, uint32_t len, uint32_t type)
{
    uint32_t r[5];
    strcpy(ros_ptr(keys), name);
    memcpy(ros_ptr(text), value, len);
    r[0] = keys, r[1] = text, r[2] = len, r[3] = 0, r[4] = type;
    swi(XOS_SetVarVal, r);
}

/* OS_ReadArgs of cmd against k into buf, size bytes. */
static int args(const char *k, const char *cmd, uint32_t size, uint32_t r[5])
{
    strcpy(ros_ptr(keys), k);
    strcpy(ros_ptr(text), cmd);
    memset(ros_ptr(buf), 0xEE, 256);
    r[0] = keys, r[1] = text, r[2] = buf, r[3] = size, r[4] = 0;
    return swi(XOS_ReadArgs, r);
}

static uint32_t word(unsigned i)
{
    return ros_ld32(buf + 4 * i);
}

static int str_at(unsigned i, const char *want)
{
    uint32_t w = word(i);
    return w >= buf && w < buf + 256 && strcmp(ros_ptr(w), want) == 0;
}

void ros_selftest_args(void)
{
    uint32_t r[5];
    text = ros_addr(ros_rma_alloc(256));
    buf = ros_addr(ros_rma_alloc(256));
    keys = ros_addr(ros_rma_alloc(256));

    /* ---- OS_EvaluateExpression: integers ---- */
    int a1 = is_int("1+2*3", 7) && is_int("(1+2)*3", 9) && is_int("-5+2", -3) &&
             is_int("7/2", 3) && is_int("-7/2", -3) && is_int("7 MOD -3", 1) &&
             is_int("-7 MOD 3", -1) && is_int("&FF AND 15", 15) && is_int("2_101", 5) &&
             is_int("1 OR 2 EOR 3", 0) && is_int("NOT 0", -1);
    check(a1, "OS_EvaluateExpression -- arithmetic and logic, with the kernel's precedence",
          NULL);
    int a2 = is_int("1<<4", 16) && is_int("-16>>2", -4) && is_int("-16>>>28", 15) &&
             is_int("8>>-1", 16) && is_int("1<<40", 0) && is_int("-1>>40", -1) &&
             is_int("3=3", -1) && is_int("3<>3", 0) && is_int("2>=3", 0) &&
             is_int("\"abc\"<\"abd\"", -1) && is_int("\"ab\"<\"abc\"", -1) &&
             is_int("\"b\">\"abc\"", -1);
    check(a2, "OS_EvaluateExpression -- shifts, either way; comparisons of integers and strings",
          NULL);

    /* ---- strings, and conversions both ways ---- */
    int s1 = is_str("\"ab\"+\"cd\"", "abcd") && is_str("\"hello\" RIGHT 3", "llo") &&
             is_str("\"hello\" LEFT 2", "he") && is_str("\"hello\" RIGHT 9", "hello") &&
             is_str("STR 42", "42") && is_str("STR (6*7) + \"!\"", "42!") &&
             is_int("LEN \"hello\"", 5) && is_int("VAL \"12\"+1", 13) &&
             is_int("\"12\"+1", 13) && is_int("\"5\"*\" 6 \"", 30) && is_int("\"\"+1", 1);
    check(s1, "OS_EvaluateExpression -- strings: +, RIGHT, LEFT, STR, LEN, VAL, converted "
              "where integers are wanted", NULL);

    int32_t n42 = 42;
    set_var("Test$N", &n42, 4, 1);
    set_var("Test$S", "abc\r", 4, 0);
    set_var("Test$T", "10\r", 3, 0);
    int v1 = is_int("Test$N+1", 43) && is_str("Test$S+\"d\"", "abcd") &&
             is_int("<Test$T>*2", 20) && is_str("\"<Test$T>\"", "10") &&
             is_int("Test$T*2", 20);
    check(v1, "OS_EvaluateExpression -- variables by name and in <>, inside quotes too", NULL);

    int e1 = is_err("1/0", 0x169) && is_err("(1", 0x162) && is_err("1)", 0x162) &&
             is_err("1+", 0x164) && is_err("1 2", 0x164) && is_err("(1 2)", 0x165) &&
             is_err("Nope$Var", 0x168) && is_err("\"x\"+1", 0x166);
    int v = eval("\"abcd\"", 2, r);
    int e2 = v && errnum(r) == 0x1E4 && r[1] == buf && r[2] == 2 &&
             memcmp(ros_ptr(buf), "ab", 2) == 0;
    check(e1 && e2,
          "OS_EvaluateExpression -- the kernel's errors, &162-&169; a short buffer, &1E4", NULL);

    /* ---- OS_SetVarVal's expression type, now there is one ---- */
    set_var("Test$E", "1+2\r", 4, 3);
    strcpy(ros_ptr(keys), "Test$E");
    r[0] = keys, r[1] = buf, r[2] = 255, r[3] = 0, r[4] = 0;
    v = swi(XOS_ReadVarVal, r);
    int x1 = !v && r[4] == 1 && ros_ld32(buf) == 3;
    set_var("Test$E", "\"a\"+\"b\"\r", 8, 3);
    r[0] = keys, r[1] = buf, r[2] = 255, r[3] = 0, r[4] = 0;
    v = swi(XOS_ReadVarVal, r);
    int x2 = !v && r[4] == 0 && r[2] == 2 && memcmp(ros_ptr(buf), "ab", 2) == 0;
    check(x1 && x2, "OS_SetVarVal, type 3 -- evaluated: a number, or a string", NULL);

    /* ---- OS_ReadArgs ---- */
    v = args("a,b", "x  y", 64, r);
    int r1 = !v && str_at(0, "x") && str_at(1, "y") && r[3] == 64 - 8 - 4;
    v = args("from/a,to,verbose=v/s,count/k/e", "src -v -count 3+4", 128, r);
    int r2 = !v && str_at(0, "src") && word(1) == 0 && word(2) != 0 && word(3) != 0 &&
             ros_ld8(word(3)) == 0 && ros_ld32(word(3) + 1) == 7;
    check(r1 && r2,
          "OS_ReadArgs -- items in order, R3 what is left; an alias, a switch, a keyword "
          "evaluated", NULL);

    v = args("all/s,brief/s,cat/s", "-ab", 64, r);
    int r3 = !v && word(0) && word(1) && word(2) == 0;
    v = args("output/k", "-o file", 64, r);
    int r4 = !v && str_at(0, "file");
    v = args("output/k", "-ofile", 64, r);
    int r5 = !v && str_at(0, "file");
    v = args("n", "-5", 64, r);
    int r6 = !v && str_at(0, "-5");
    check(r3 && r4 && r5 && r6,
          "OS_ReadArgs -- switches run together, a keyword by its letter, \"-5\" an item", NULL);

    v = args("x,y", "\"a \"\"b\" c", 64, r);
    int q1 = !v && str_at(0, "a \"b") && str_at(1, "c");
    v = args("s/g", "\"<Test$S>|M\"", 64, r);
    uint32_t g = word(0);
    int q2 = !v && ros_ld8(g) == 4 && ros_ld8(g + 1) == 0 &&
             memcmp(ros_ptr(g + 2), "abc\r", 4) == 0;
    v = args("e/e", "Test$S", 64, r);
    uint32_t e = word(0);
    int q3 = !v && ros_ld8(e) == 1 && ros_ld8(e + 1) == 3 && memcmp(ros_ptr(e + 3), "abc", 3) == 0;
    check(q1 && q2 && q3,
          "OS_ReadArgs -- quotes and \"\"; /G GSTransed with its length; /E a string", NULL);

    int f1 = args("from/a", "", 64, r) && errnum(r) == 0x1EA;
    int f2 = args("v/s", "-v -v", 64, r) && errnum(r) == 0x1EB;
    int f3 = args("a", "x y", 64, r) && errnum(r) == 0x1EA;
    int f4 = args("a", "abcdefgh", 8, r) && errnum(r) == 0x1E4;
    int f5 = args("a", "\"abc", 64, r) && errnum(r) == 0xFD;
    check(f1 && f2 && f3 && f4 && f5,
          "OS_ReadArgs -- /A missing or too many &1EA, repeated &1EB, overflow &1E4, "
          "an unclosed quote &FD", NULL);

    uint32_t d[5];
    strcpy(ros_ptr(keys), "Test$*");
    for (int i = 0; i < 16; i++) {
        d[0] = keys, d[1] = 0, d[2] = (uint32_t)-1, d[3] = 0, d[4] = 0;
        if (swi(XOS_SetVarVal, d))
            break;
    }
    ros_rma_free(ros_ptr(keys));
    ros_rma_free(ros_ptr(buf));
    ros_rma_free(ros_ptr(text));
}
