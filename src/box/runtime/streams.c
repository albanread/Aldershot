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
 * This file is a reimplementation in C of RISC OS Open's Kernel source
 * (Sources/Kernel: s.PMF.oswrch, s.PMF.key, s.PMF.osbyte, s.Oscli,
 * s.Middle).
 */
/* streams.c -- where characters go and come from: spooling, *Exec, OS_CLI's
 * redirection, and reading a line.
 *
 * This follows the kernel's Kernel/s/PMF/oswrch, PMF/key, PMF/osbyte,
 * Oscli and Middle. It provides:
 *
 *   - Output, which is WrchV's default owner. Characters go to the VDU
 *     drivers (runtime/vdu) unless bit 1 of WrchDest (OS_Byte 3) says not.
 *     Characters that are text also go to the serial console. They then go
 *     to the *Spool file unless bit 4 says not. A VDU error (a mode
 *     change's) is returned. An error writing the spool file stops
 *     spooling and is returned. The handle is cleared first and then the
 *     file is closed.
 *   - Input, which is RdchV's default owner. Characters come from the
 *     redirection file, then the *Exec file, then the keyboard. A file at
 *     its end is closed and the next source is read. An error reading a
 *     file closes it.
 *   - OS_Byte 129, INKEY. With R2 < &80 it waits up to R1 + R2 * 256
 *     centiseconds for a character. The character comes from the
 *     redirection file, the *Exec file or the keyboard, as OS_ReadC reads
 *     it. On success R2 is 0 and R1 is the character. If none came in time,
 *     R2 is &FF and C is set. INKEY(-256) returns R1 0 and R2 &FF, the
 *     OS's identity, &AA for RISC OS 5. Any other negative INKEY asks
 *     whether a key is down. The answer is always no, because the console
 *     has characters, not keys.
 *   - OS_Byte 198 and 199, the *Exec and *Spool handles, as OS_Byte
 *     variables. R1 returns the old value, the new value is
 *     (old AND R2) EOR R1, and R2 returns the next variable's value. OS_Byte
 *     3, WrchDest, returns the old value in R1, and 236 is the same as a
 *     variable. Bit 5 sends what would reach the VDU drivers to VDUXV
 *     instead. The Font Manager uses this to collect a VDU 25 string.
 *   - Redirection: "{ > file }", "{ >> file }" and "{ < file }". The files
 *     are opened as the kernel opens them, and must open and must not be
 *     directories. Output is sent to the file instead of the screen by a
 *     WrchV claim. All of it is undone after the command, or when an error
 *     reaches the error handler (OscliTidy). OS_ChangeRedirection reads and
 *     sets the handles.
 *   - Streams for a second command line. An SSH session (modules/sshd) is
 *     a task of its own. Its hooks take that task's characters before the
 *     VDU or the keyboard see them. While the console waits for a key, the
 *     hooks' idle function runs the sessions that have something to do.
 *   - OS_ReadLine and OS_ReadLine32, through ReadLineV. Characters come
 *     from OS_ReadC and are echoed through OS_WriteC. Delete and backspace
 *     take one character back, and Ctrl-U takes back the line. CR or LF
 *     ends the line and is stored as CR. A full buffer beeps. Characters
 *     outside R2-R3 are echoed but not kept. With flag bit 31 they are not
 *     echoed. With flag bit 30 every echo is R4's low byte instead of the
 *     character.
 *
 * There is one of each stream for the whole box, as in the kernel.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/keyboard.h"
#include "rosgd/platform.h"
#include "rosgd/streams.h"
#include "rosgd/swi.h"
#include "rosgd/vdu.h"
#include "rosgd/vector.h"

#define WRCHV 0x03u
#define READLINEV 0x0Eu
#define VDUXV 0x1Bu

#define ERR_REDIRECT_FAIL 0x140u

const struct ros_stream_hooks *ros_stream_hooks;

void ros_streams_idle(void)
{
    if (ros_stream_hooks && ros_stream_hooks->idle)
        ros_stream_hooks->idle();
}

int ros_streams_own(void)
{
    return ros_stream_hooks && ros_stream_hooks->owns && ros_stream_hooks->owns();
}

static uint8_t wrch_dest;               /* OS_Byte 3 */
static uint8_t exec_h, spool_h;         /* OS_Byte 198, 199 */
static uint8_t redirect_in, redirect_out;
static int redirect_claimed;

static os_error *swi(uint32_t n, struct ros_cpu *c)
{
    ros_swi(c, n);
    return c->v ? ros_ptr(c->r[0]) : NULL;
}

static void close_h(uint32_t h)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = h;
    swi(XOS_Find, &c);
}

static os_error *bput(uint32_t h, uint32_t ch)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ch, c.r[1] = h;
    return swi(XOS_BPut, &c);
}

/* ---- output ------------------------------------------------------------------- */

os_error *ros_wrch_default(uint8_t ch)
{
    os_error *vdu_e = NULL;
    if (ros_stream_hooks && ros_stream_hooks->wrch && ros_stream_hooks->wrch(ch)) {
        /* a session's: to its terminal */
    } else if (wrch_dest & 0x02) {
        /* not to the VDU at all */
    } else if (wrch_dest & 0x20) {      /* to VDUXV instead: the Font Manager's */
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = ch;
        ros_vector_call(VDUXV, &c);
        if (c.v)
            vdu_e = (os_error *)ros_ptr(c.r[0]);
    } else {
        int plain;
        vdu_e = ros_vdu_write(ch, &plain);
        if (plain)                      /* the serial console: the text */
            ros_console_putc(ch);
    }
    if (spool_h && !(wrch_dest & 0x10)) {
        uint32_t h = spool_h;
        os_error *e = bput(h, ch);
        if (e) {
            spool_h = 0;                /* stop spooling first */
            os_error keep = *e;
            close_h(h);
            return ros_error(keep.errnum, "%s", keep.errmess);
        }
    }
    return vdu_e;
}

/* ---- input -------------------------------------------------------------------- */

/* Gets a byte from h. It returns 1 if a byte was got and 0 at the end of
 * the file. *e is set to an error. */
static int bget(uint32_t h, uint8_t *ch, os_error **e)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = h;
    *e = swi(XOS_BGet, &c);
    *ch = (uint8_t)c.r[0];
    return !*e && !c.c;
}

/* Gets a byte from one of the files. It returns 1 for a byte, or 0 to go
 * on to the next source. */
static int from_file(uint8_t *handle, uint8_t *ch, os_error **e)
{
    if (!*handle)
        return 0;
    uint32_t h = *handle;
    if (bget(h, ch, e))
        return 1;
    *handle = 0;                        /* stop reading it, then close it */
    if (*e) {
        os_error keep = **e;
        close_h(h);
        *e = ros_error(keep.errnum, "%s", keep.errmess);
        return 1;
    }
    close_h(h);
    return 0;
}

os_error *ros_rdch_default(uint8_t *ch, int *escape)
{
    os_error *e = NULL;
    *escape = 0;
    if (from_file(&redirect_in, ch, &e) || from_file(&exec_h, ch, &e))
        return e;
    if (ros_stream_hooks && ros_stream_hooks->rdch && ros_stream_hooks->rdch(ch, escape))
        return NULL;
    return ros_keyboard_rdch(ch, escape);          /* the keyboard buffer, the console's too */
}

/* OS_Byte 129: INKEY. It runs a safe point first, even inside a SWI. On
 * RISC OS the keyboard's and the mouse's interrupts break into whatever
 * calls it. Code that polls it from inside a SWI waits for input that only
 * those interrupts bring. Two examples are Wimp_ReportError's loop
 * (INKEY(0), then OS_Mouse) and Wimp_CommandWindow's "Press SPACE" loop,
 * which no outermost SWI's exit ever ends. INKEY with a time limit also
 * turns interrupts on for as long as it lasts, whatever its caller had, as
 * the kernel's RdchInkey does (CLI, Kernel s/PMF/key). A negative INKEY,
 * which is a scan of the keys, leaves them as they are. */
static void inkey_positive(struct ros_cpu *s);

static void inkey(struct ros_cpu *s)
{
    if ((s->r[2] & 0xFF) >= 0x80) {
        ros_background_run();
        ros_keyboard_inkey_neg(s);     /* a key tested or scanned, or the OS version */
        return;
    }
    unsigned irq = ros_irq_suspend();
    ros_background_run();
    inkey_positive(s);
    ros_irq_resume(irq);
}

static void inkey_positive(struct ros_cpu *s)
{
    uint32_t r1 = s->r[1] & 0xFF, r2 = s->r[2] & 0xFF;
    uint8_t ch;
    os_error *e = NULL;
    if (from_file(&redirect_in, &ch, &e) || from_file(&exec_h, &ch, &e)) {
        if (e) {
            ros_swi_fail(s, e);
            return;
        }
        s->r[1] = ch, s->r[2] = 0, s->c = 0;
        return;
    }
    int cs = (int)(r1 | r2 << 8), c = -2;
    if (ros_stream_hooks && ros_stream_hooks->inkey)
        c = ros_stream_hooks->inkey(cs);
    if (c == -2) {                      /* the keyboard buffer's, the console's too */
        ros_keyboard_inkey(s);
        return;
    }
    if (c < 0) {
        s->r[2] = 0xFF, s->c = 1;
    } else {
        s->r[1] = (uint32_t)c, s->r[2] = 0, s->c = 0;
    }
}

/* ---- the OS_Byte variables ------------------------------------------------------- */

int ros_streams_byte(struct ros_cpu *s)
{
    uint8_t *var;
    switch (s->r[0]) {
    case 129:
        inkey(s);
        return 1;
    case 3: {
        uint32_t old = wrch_dest;
        wrch_dest = (uint8_t)s->r[1];
        s->r[1] = old;
        return 1;
    }
    case 198: var = &exec_h; break;
    case 199: var = &spool_h; break;
    case 236: var = &wrch_dest; break;  /* WrchDest as a variable */
    default: return 0;
    }
    uint32_t old = *var;
    *var = (uint8_t)((old & s->r[2]) ^ s->r[1]);
    s->r[1] = old;
    s->r[2] = var == &exec_h ? spool_h : 0;
    return 1;
}

/* ---- redirection ----------------------------------------------------------------- */

/* The claim on WrchV while output is redirected: to the file instead */
static int redirect_wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    os_error *e = bput(redirect_out, s->r[0]);
    if (e) {
        os_error keep = *e;
        ros_redirect_tidy();
        ros_swi_fail(s, ros_error(keep.errnum, "%s", keep.errmess));
    }
    return ROS_VECTOR_CLAIM;
}

static void claim_wrch(int on)
{
    if (on && !redirect_claimed)
        ros_vector_claim_native(WRCHV, redirect_wrch, 0);
    else if (!on && redirect_claimed)
        ros_vector_release_native(WRCHV, redirect_wrch, 0);
    redirect_claimed = on;
}

os_error *ros_redirect(uint32_t mode, uint32_t name)
{
    uint8_t *h = mode == 0x40 ? &redirect_in : &redirect_out;
    if (*h) {                           /* any previous one closed */
        uint32_t old = *h;
        *h = 0;
        close_h(old);
    }
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = mode | 0x08 | 0x04, c.r[1] = name;
    os_error *e = swi(XOS_Find, &c);
    if (e)
        return e;
    if (c.r[0] == 0)
        return ros_error(ERR_REDIRECT_FAIL, "Redirection fails");
    *h = (uint8_t)c.r[0];
    if (mode == 0x40)
        return NULL;
    claim_wrch(1);
    if (mode == 0xC0) {                 /* >>: at the end */
        ros_cpu_enter(&c);
        c.r[0] = 2, c.r[1] = *h;
        if ((e = swi(XOS_Args, &c)) != NULL)
            return e;
        uint32_t ext = c.r[2];
        ros_cpu_enter(&c);
        c.r[0] = 1, c.r[1] = *h, c.r[2] = ext;
        return swi(XOS_Args, &c);
    }
    return NULL;
}

void ros_redirect_tidy(void)
{
    claim_wrch(0);                      /* before the files close */
    if (redirect_in) {
        uint32_t h = redirect_in;
        redirect_in = 0;
        close_h(h);
    }
    if (redirect_out) {
        uint32_t h = redirect_out;
        redirect_out = 0;
        close_h(h);
    }
}

/* OS_ChangeRedirection: R0 is the new input handle and R1 the new output
 * handle (0 for none, or out of range to leave it alone). The old handles
 * come back in R0 and R1. */
void ros_thunk_OS_ChangeRedirection(struct ros_cpu *s)
{
    uint32_t in = redirect_in, out = redirect_out;
    if (s->r[0] < 0x100)
        redirect_in = (uint8_t)s->r[0];
    if (s->r[1] < 0x100) {
        redirect_out = (uint8_t)s->r[1];
        claim_wrch(redirect_out != 0);
    }
    s->r[0] = in, s->r[1] = out;
    s->v = 0;
}

/* ---- reading a line -------------------------------------------------------------- */

static os_error *write_c(uint32_t ch)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ch;
    return swi(XOS_WriteC, &c);
}

/* ReadLineV's default owner (the kernel's VecRdLine). */
static void read_line(struct ros_cpu *s)
{
    uint32_t buf = s->r[0], max = s->r[1], lo = s->r[2], hi = s->r[3], flags = s->r[4];
    uint32_t n = 0;
    os_error *e = NULL;
    int escape = 0;
    for (;;) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        if ((e = swi(XOS_ReadC, &c)) != NULL)
            break;
        if (c.c) {
            escape = 1;
            break;
        }
        uint32_t ch = c.r[0] & 0xFF;
        if (ch == 127 || ch == 8) {
            if (n) {
                n--;
                e = write_c(127);
            }
        } else if (ch == 21) {
            for (; n && !e; n--)
                e = write_c(127);
        } else if (ch == 13 || ch == 10) {
            ros_st8(buf + n, 13);
            ros_cpu_enter(&c);
            e = swi(XOS_NewLine, &c);
            break;
        } else if (n >= max) {
            e = write_c(7);
        } else if (ch >= lo && ch <= hi) {
            ros_st8(buf + n++, ch);
            e = write_c(flags & 0x40000000u ? (flags & 0xFF) : ch);
        } else if (!(flags & 0x80000000u)) {
            e = write_c(flags & 0x40000000u ? (flags & 0xFF) : ch);
        }
        if (e)
            break;
    }
    s->r[1] = n;
    s->c = (uint32_t)escape;
    if (e) {
        ros_swi_fail(s, e);
    } else {
        s->r[0] = buf;
        s->v = 0;
    }
}

static void through_readline(struct ros_cpu *s)
{
    uint32_t outer = ros_svc_sp_enter(s);
    s->v = 0;
    if (!ros_vector_call(READLINEV, s))
        read_line(s);
    ros_svc_sp = outer;
}

/* OS_ReadLine32: R0 the buffer, R1 its size less the CR, R2-R3 the range
 * kept, R4 the flags */
void ros_thunk_OS_ReadLine32(struct ros_cpu *s)
{
    through_readline(s);
}

/* OS_ReadLine: the flags are in R0's top two bits and the echo byte is in
 * R4. The exception is a buffer in the RMA, the system heap or the SVC
 * stack (and the C ROM between them and the dynamic areas). ROSGD's map
 * puts these at &60000000 and above (arena.h). For such a buffer, bit 30
 * is part of the address and not a flag. This is as on RISC OS 5, whose
 * RMA is below &40000000 and whose modules pass their buffers to
 * OS_ReadLine as they are. There is one ambiguity. It arises with bit 30
 * (R4's character echoed in place of what is typed) and a buffer in
 * application space from &20000000 to &38000000. That needs a slot of more
 * than 512 MB, which no RISC OS 5 program had to pair with the flag. */
void ros_thunk_OS_ReadLine(struct ros_cpu *s)
{
    uint32_t r4 = s->r[4];
    if (s->r[0] >= ROS_RMA_BASE && s->r[0] < ROS_DA_BASE) {
        s->r[4] = r4 & 0xFF;
    } else {
        s->r[4] = (s->r[0] & 0xC0000000u) | (r4 & 0xFF);
        s->r[0] &= 0x3FFFFFFFu;
    }
    through_readline(s);
    s->r[4] = r4;
}
