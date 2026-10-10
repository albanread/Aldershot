/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_program.c: what a program starts with and asks of the kernel:
 * OS_GetEnv and OS_WriteEnv, FileSwitch's StartApplication, OS_Module 2,
 * OS_UpCall, OS_EnterOS, OS_PrettyPrint, the Escape condition's OS_Bytes,
 * and VFPSupport. These are the calls BASIC makes as it starts
 * (RUNTIME-BUGS 1-4, 6, 8).
 */
#include <math.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/task.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

#define WRCHV 0x03u
#define UPCALLV 0x1Du

static char out[1024];
static unsigned outn;

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (outn < sizeof out - 1)
        out[outn++] = (char)s->r[0];
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

static int swi(uint32_t n, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return (int)s.v;
}

static unsigned upcalls;
static uint32_t upcall_r0;
static int refuse;

static int upcallv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    upcalls++;
    upcall_r0 = s->r[0];
    if (refuse && s->r[0] == 256) {
        s->r[0] = 0;                    /* UpCall_Claimed */
        return ROS_VECTOR_CLAIM;
    }
    return ROS_VECTOR_PASS;
}

/* Two tasks, each with a VFP context of its own: the active context is
 * the task's, so each saves and loads only its own registers */
static struct ros_task *home;
static struct {
    uint32_t ctx[2];
    uint32_t active_seen[2];
    int step;
} two;

static void vfp_task(void *arg)
{
    int k = (int)(uintptr_t)arg;
    uint32_t cr[8] = { 0x80000003u, 32, 0, 0 };
    swi(XVFPSupport_CreateContext, cr);
    two.ctx[k] = cr[0];
    ros_fp_current->vfp.d[0] = 1.0 + k;         /* task 0 has 1.0, task 1 2.0 */
    ros_task_switch(home);
    uint32_t ac[8] = { 0 };
    swi(XVFPSupport_ActiveContext, ac);
    two.active_seen[k] = ac[0];
    uint32_t off[8] = { 0, 0 };                 /* deactivate: its registers saved */
    swi(XVFPSupport_ChangeContext, off);
    ros_task_switch(home);
}

void ros_selftest_program(void)
{
    uint32_t text = ros_addr(ros_rma_alloc(512));

    /* OS_WriteEnv, OS_GetEnv */
    strcpy(ros_ptr(text), "Prog -a b\rjunk");
    uint8_t *t5 = ros_ptr(text + 64);
    memcpy(t5, "\x01\x02\x03\x04\x05", 5);
    uint32_t w[8] = { text, text + 64 };
    swi(XOS_WriteEnv, w);
    uint32_t g[8] = { 0 };
    swi(XOS_GetEnv, g);
    check(!strcmp(ros_ptr(g[0]), "Prog -a b") && !memcmp(ros_ptr(g[2]), "\x01\x02\x03\x04\x05", 5) &&
              g[1] == ros_ld32(ROS_ZEROPAGE + 0x11C),
          "OS_GetEnv, OS_WriteEnv -- the command line, the RAM limit, the start time", "\"%s\"",
          (char *)ros_ptr(g[0]));

    /* StartApplication: the command line, and a refusal */
    ros_vector_claim_native(UPCALLV, upcallv, 0);
    strcpy(ros_ptr(text), "tail words");
    strcpy(ros_ptr(text + 32), "Name");
    uint32_t sa[8] = { 2, text, 0, text + 32 };
    int v = swi(XOS_FSControl, sa);
    swi(XOS_GetEnv, g);
    int s1 = !v && !strcmp(ros_ptr(g[0]), "Name tail words") && upcall_r0 == 256;
    refuse = 1;
    uint32_t sb[8] = { 2, text, 0, text + 32 };
    v = swi(XOS_FSControl, sb);
    refuse = 0;
    ros_vector_release_native(UPCALLV, upcallv, 0);
    check(s1 && v && ((os_error *)ros_ptr(sb[0]))->errnum == 0x600,
          "OS_FSControl 2 -- the command line set, UpCall 256 offered; refused, &600", NULL);

    /* OS_Module 2 of a module with no start entry returns */
    strcpy(ros_ptr(text), "MessageTrans");
    uint32_t en[8] = { 2, text, 0 };
    check(swi(XOS_Module, en) == 0, "OS_Module 2 -- a module with no start entry returns", NULL);

    /* OS_EnterOS, OS_UpCall's default */
    uint32_t eo[8] = { 0 };
    uint32_t up[8] = { 1, 0 };
    check(swi(XOS_EnterOS, eo) == 0 && swi(XOS_UpCall, up) == 0,
          "OS_EnterOS; OS_UpCall with nothing on UpCallV", NULL);

    /* The Escape condition: OS_Byte 125 sets, 126 acknowledges, 124 clears */
    uint32_t b125[8] = { 125 }, b126[8] = { 126 }, b126b[8] = { 126 };
    swi(XOS_Byte, b125);
    int esc = 0;
    xos_read_escape_state(&esc);
    swi(XOS_Byte, b126);
    int esc2 = 1;
    xos_read_escape_state(&esc2);
    swi(XOS_Byte, b126b);
    uint32_t b218[8] = { 218, 0, 0 };
    int v218 = swi(XOS_Byte, b218);
    check(esc && b126[1] == 0xFF && !esc2 && b126b[1] == 0 && !v218 && b218[1] == 0,
          "OS_Byte 124-126, 218 -- Escape set, acknowledged once; the VDU queue empty", NULL);

    /* OS_PrettyPrint: words wrapped to the text window's width (WindowWidth,
     * the characters after a new line), tokens, hard spaces and CR. It is
     * checked in a window 80 wide, and the window before is put back after. */
    uint32_t tw = text + 460;
    ros_st32(tw, 128), ros_st32(tw + 4, 129), ros_st32(tw + 8, 130), ros_st32(tw + 12, 131);
    ros_st32(tw + 16, (uint32_t)-1);
    uint32_t rv[8] = { tw, tw + 20 };
    swi(XOS_ReadVduVariables, rv);                  /* ScrLCol, ScrBRow, ScrRCol, ScrTRow */
    uint32_t was[4] = { ros_ld32(tw + 20), ros_ld32(tw + 24), ros_ld32(tw + 28), ros_ld32(tw + 32) };
    static const uint8_t w80[] = { 28, 0, 24, 79, 0 };
    for (unsigned k = 0; k < sizeof w80; k++) {
        uint32_t wc[8] = { w80[k] };
        swi(XOS_WriteC, wc);
    }
    ros_vector_claim_native(WRCHV, wrch, 0);
    char *p = ros_ptr(text);
    memset(p, 0, 512);
    for (int k = 0; k < 20; k++)
        strcat(p, "word ");
    static const char tail[] = "\x1B\x00 a\x1F" "b\rnext";  /* 27 0 is token 0 */
    memcpy(p + strlen(p), tail, sizeof tail);
    uint32_t dict = text + 300, special = text + 400;
    strcpy(ros_ptr(special), "SPECIAL");
    outn = 0, out[0] = 0;
    uint32_t pp[8] = { text, dict, special };
    ros_st8(dict, 0);
    v = swi(XOS_PrettyPrint, pp);
    ros_vector_release_native(WRCHV, wrch, 0);
    const uint8_t back[] = { 28, (uint8_t)was[0], (uint8_t)was[1], (uint8_t)was[2], (uint8_t)was[3] };
    for (unsigned k = 0; k < sizeof back; k++) {
        uint32_t wc[8] = { back[k] };
        swi(XOS_WriteC, wc);
    }
    const char *want = "word word word word word word word word word word word word word word word "
                       "word\n\rword word word word SPECIAL a b\n\rnext";
    check(!v && !strcmp(out, want),
          "OS_PrettyPrint -- wrapped to the window, 80 wide; token 0, a hard space, CR", "\"%s\"", out);
    ros_rma_free(ros_ptr(text));

    /* VFPSupport: contexts save and restore the VFP registers */
    struct ros_fp *fp = ros_fp_current;
    uint32_t cc[8] = { 3, 16 }, bad[8] = { 0x100, 16 };
    int c1 = swi(XVFPSupport_CheckContext, cc) == 0 && cc[0] == 24 + 16 * 8 &&
             swi(XVFPSupport_CheckContext, bad) == 1 &&
             ((os_error *)ros_ptr(bad[0]))->errnum == 0x81F102;
    fp->vfp.d[0] = 9.0;
    uint32_t ca[8] = { 0x80000003u, 32, 0, 0x03000000 };  /* activate; VFPSupport's memory */
    swi(XVFPSupport_CreateContext, ca);
    uint32_t a = ca[0];
    int c2 = a && ca[1] == 0 && fp->vfp.d[0] == 0.0 && fp->fpscr == 0x03000000;
    fp->vfp.d[0] = 2.5;
    uint32_t cb[8] = { 3, 32, 0, 0 };
    swi(XVFPSupport_CreateContext, cb);
    uint32_t b = cb[0];
    uint32_t ch[8] = { b, 0 };
    swi(XVFPSupport_ChangeContext, ch);
    c2 = c2 && ch[0] == a && fp->vfp.d[0] == 0.0 && fp->fpscr == 0;
    uint32_t ch2[8] = { a, 0 };
    swi(XVFPSupport_ChangeContext, ch2);
    c2 = c2 && ch2[0] == b && fp->vfp.d[0] == 2.5 && fp->fpscr == 0x03000000;
    uint32_t ex[8] = { a, 0 };
    swi(XVFPSupport_ExamineContext, ex);
    c2 = c2 && ex[1] == 32 && ex[4] == 24 + 256 && (ex[0] & 0x80000000u) && (ex[0] & 0x40000000u);
    uint32_t da[8] = { a, b }, db[8] = { b, 0 }, ac[8] = { 0 };
    swi(XVFPSupport_DestroyContext, da);
    c2 = c2 && da[0] == b;
    swi(XVFPSupport_DestroyContext, db);
    swi(XVFPSupport_ActiveContext, ac);
    c2 = c2 && db[0] == 0 && ac[0] == 0;
    check(c1 && c2, "VFPSupport -- contexts: sized, created, swapped, examined, destroyed",
          "%d %d", c1, c2);
    uint32_t f0[8] = { 0 }, f2[8] = { 2 }, f9[8] = { 9 }, el[8] = { 0 };
    int fv = swi(XVFPSupport_Features, f0) == 0 && (f0[2] & 0xF000) == 0 &&
             swi(XVFPSupport_Features, f2) == 0 && f2[0] == 4 &&
             swi(XVFPSupport_Features, f9) == 1 && swi(XVFPSupport_ElementaryFunctions, el) == 0 &&
             el[0] == 12;
    struct ros_cpu fc;
    ros_cpu_enter(&fc);
    fp->vfp.d[0] = 0.5;
    ros_call(&fc, el[2] + 4 * 0);          /* sin */
    double sn = fp->vfp.d[0];
    fp->vfp.d[0] = 2.0, fp->vfp.d[1] = 10.0;
    ros_call(&fc, el[2] + 4 * 10);         /* pow */
    home = ros_task_current();
    uint32_t mine_before[8] = { 0 };
    swi(XVFPSupport_ActiveContext, mine_before);
    struct ros_task *ta = ros_task_create(0x10000, vfp_task, (void *)(uintptr_t)0);
    struct ros_task *tb = ros_task_create(0x10000, vfp_task, (void *)(uintptr_t)1);
    ros_task_switch(ta);                        /* A: context X, d0 1.0 */
    ros_task_switch(tb);                        /* B: context Y, d0 2.0 */
    ros_task_switch(ta);                        /* A: X still its; saved */
    ros_task_switch(tb);                        /* B: Y still its; saved */
    uint32_t mine_after[8] = { 0 };
    swi(XVFPSupport_ActiveContext, mine_after);
    double xd0, yd0;
    memcpy(&xd0, ros_ptr(two.ctx[0] + 24), 8);
    memcpy(&yd0, ros_ptr(two.ctx[1] + 24), 8);
    check(two.active_seen[0] == two.ctx[0] && two.active_seen[1] == two.ctx[1] && xd0 == 1.0 &&
              yd0 == 2.0 && mine_after[0] == mine_before[0],
          "VFPSupport -- the active context is the task's: each saves its own registers",
          "X %g Y %g", xd0, yd0);
    ros_task_destroy(ta);
    ros_task_destroy(tb);
    ros_rma_free(ros_ptr(two.ctx[0]));
    ros_rma_free(ros_ptr(two.ctx[1]));

    check(fv && sn == sin(0.5) && fp->vfp.d[0] == 1024.0,
          "VFPSupport -- features (no NEON); the elementary functions by table, BLX-able",
          "%d %g %g", fv, sn, fp->vfp.d[0]);
}
