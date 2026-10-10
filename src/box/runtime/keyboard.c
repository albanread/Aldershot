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
 * This file is a reimplementation in C of RISC OS Open's kernel source
 * (Sources/Kernel: s.PMF.key, s.PMF.Buffer, s.PMF.osbyte, s.PMF.osinit,
 * hdr.KeyWS, hdr.KernelWS).
 */

/* keyboard.c: the kernel's keyboard: KeyV's owner, the kernel's buffers,
 * OS_ReadC and INKEY, and the OS_Byte variables.
 *
 * Written from the kernel's source (Kernel/s/PMF/key, Buffer, osbyte,
 * osinit; hdr/KeyWS), in its order and with its quirks:
 *
 *   - KeyV 0 (KeyboardPresent) records the keyboard's id and offers
 *     Service_KeyHandler, which the International Keyboard module answers
 *     by installing its key handler. KeyV 1 and 2 (up, down) keep a bitmap
 *     of the keys down and raise Event 11. They hand shifting keys (Shift,
 *     Ctrl, Alt, the mouse buttons, Break) to the handler's code, both ways.
 *     Other keys become the current key, two at most (rollover). They are
 *     repeated on the centisecond tick after OS_Byte 196's delay, at 197's
 *     rate. With the driver's "NoKd" there is no debounce. The first
 *     character comes at once, as the Input module asks;
 *   - a key becomes characters through the handler's table (Shift, Ctrl,
 *     Caps Lock) or its code for special keys, and goes into the keyboard
 *     buffer. The Escape character raises Event 6 or the Escape condition;
 *   - the kernel's ten buffers (keyboard, serial in and out, printer, the
 *     sound and speech ones, mouse) at the foot of InsV, RemV and CnpV:
 *     single bytes only. The kernel does not do block transfers on them, so
 *     the mouse's block insert goes unanswered, as it does on RISC OS 5;
 *   - OS_ReadC (RdchV's default, after *Exec and redirection in streams.c)
 *     and INKEY wait for the buffer, expanding function keys (Key$n) and
 *     applying the key bases (OS_Byte 221-228) and cursor-key mode (OS_Byte 4);
 *   - the OS_Byte variables &A6-&FF, in zero page, and the keyboard's
 *     OS_Bytes: 4, 11, 12, 15, 18, 21, 70, 71, 118, 120-122, 128, 129,
 *     138, 145, 152, 153, 166, 167, 202;
 *   - the country, the alphabet and the keyboard, which OS_Byte 70 and 71
 *     read and set through Service_International: the International
 *     module (modules/international) says which alphabet a country has and
 *     draws an alphabet in the system font. The keyboard's handler is told
 *     of a new keyboard.
 *
 * The serial console is a keyboard too: what is typed there is inserted as
 * a keyboard's characters are, the Escape character included.
 *
 * Not here: the serial port as an input stream (OS_Byte 2), and block
 * transfers on the kernel's buffers. Cursor editing (OS_Byte 4,0's copy key)
 * is done by the VDU (ros_vdu_cursor_edit), which OS_ReadC calls.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cmos.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/international.h"
#include "rosgd/keyboard.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/streams.h"
#include "rosgd/swi.h"
#include "rosgd/vdu.h"
#include "rosgd/vector.h"

#define KEYV    0x13u
#define INSV    0x14u
#define REMV    0x15u
#define CNPV    0x16u
#define UPCALLV 0x1Du

#define SERVICE_KEYHANDLER 0x44u
#define COUNTRY_CMOS 0xBAu
#define UPCALL_KEYBOARD_STATUS 20u
enum { EVENT_OUTPUT_EMPTY = 0, EVENT_INPUT_FULL = 1, EVENT_CHAR_INPUT = 2, EVENT_ESCAPE = 6,
       EVENT_KEYBOARD = 11 };
#define NO_KBD_MAGIC 0x4E6F4B64u        /* "NoKd": the driver debounces */
#define KBID_NONE 0xFFu
#define NO_KEY 0xFFu

#define ESC_STATUS (ROS_ZEROPAGE + 0x104)
#define ESC_CONDITION 0x40u
#define OSBYTE_VARS (ROS_ZEROPAGE + 0x1400)     /* OsbyteVars, from &A6 */
#define OSVERSION_ID 0xAAu                      /* INKEY(-256): RISC OS 5 */

/* The variables used here (hdr/KernelWS's names) */
enum {
    V_KEYREPDELAY = 0xC4, V_KEYREPRATE = 0xC5, V_ESCBREAK = 0xC8, V_KEYBDDISABLE = 0xC9,
    V_KEYBDSTATUS = 0xCA, V_SOFTKEYLEN = 0xD8, V_ESCCH = 0xDC, V_IPBUFFERCH = 0xDD,
    V_ESCACTION = 0xE5, V_ESCEFFECT = 0xE6, V_WRCHDEST = 0xEC, V_CUREDIT = 0xED,
    V_COUNTRY = 0xF0, V_BREAKVECTOR = 0xF7,
};

/* ---- the state (KeyWorkSpace) ---------------------------------------------- */

static const struct ros_keyhandler *keyvec;     /* KeyVec: NULL none */
static uint32_t curr_key = NO_KEY, old_key = NO_KEY;
static uint8_t kbid = KBID_NONE, last_kbid = KBID_NONE;
static uint8_t autorepeat, debouncing, no_debounce, pending_alt_type, last_led = 0xFF;
static uint32_t inkey_counter;
static uint32_t keys_down[0x300 / 32];
static uint8_t soft_key[256];                   /* SoftKeyExpand */
static uint32_t soft_key_end;                   /* SoftKeyPtr: one past the expansion */
static uint8_t keyboard_no = 1, alphabet = 101, key_alphabet = 101;     /* UK, Latin1 */

static const uint8_t null_list[1] = { 0 };
static const uint8_t esc_list[2] = { 1, 0x1B };
static const uint8_t nulnul_list[3] = { 2, 0, 0 };

/* Waking a reader: anything inserted into the keyboard buffer, an Escape */
static pthread_mutex_t input_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t input_cv = PTHREAD_COND_INITIALIZER;

static void wake_readers(void)
{
    pthread_mutex_lock(&input_mu);
    pthread_cond_broadcast(&input_cv);
    pthread_mutex_unlock(&input_mu);
}

/* ---- the OS_Byte variables -------------------------------------------------- */

static const uint8_t byte_var_init[0x100 - 0xA6] = {
    /* &A6 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* &B0 */ 0, 0, 0xFF, 0, 0x80, 1, 0, 0, 0, 0, 0, 0xFF, 4, 4, 0, 0xFF,
    /* &C0 */ 0x42, 0x19, 0x19, 0x19, 0x32, 0x08, 0, 0, 0, 0, 0x34, 0x11, 0, 0, 0, 0,
    /* &D0 */ 0, 0, 0, 1, 0x90, 0x64, 6, 0x81, 0, 0, 0, 9, 0x1B, 1, 0xD0, 0xE0,
    /* &E0 */ 0xF0, 1, 0x80, 0x90, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x30, 1,
    /* &F0 */ 0, 0, 0x64, 5, 0xFF, 1, 0x0A, 1, 0, 0, 0, 0, 0xFF, 1, 0x0F, 8,
};

uint8_t ros_byte_var(uint32_t n)
{
    return (uint8_t)ros_ld8(OSBYTE_VARS + n - 0xA6);
}

void ros_byte_var_set(uint32_t n, uint8_t v)
{
    ros_st8(OSBYTE_VARS + n - 0xA6, v);
}

#define VAR(n) ros_byte_var(n)

/* ---- calling out -------------------------------------------------------------- */

static void call_vector(uint32_t v, struct ros_cpu *c)
{
    ros_vector_call(v, c);
}

/* OSEVEN: 1 if the event was disabled (C set) */
static int event(uint32_t n, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t r4)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = n, c.r[1] = r1, c.r[2] = r2, c.r[3] = r3, c.r[4] = r4;
    ros_event_generate(&c);
    return (int)c.c;
}

static void escape_condition(void)
{
    ros_env_escape(1);                  /* the flag, and the program's handler told */
    wake_readers();
}

/* ---- the kernel's buffers (PMF/Buffer) --------------------------------------- */

#define NBUFFERS 10
enum { BUFF_KEY = 0, BUFF_RS423OUT = 2, BUFF_MOUSE = 9 };
static const uint32_t buff_size[NBUFFERS] = { 0x100, 0x100, 0xC0, 0x400, 4, 4, 4, 4, 4, 0x40 };
static uint8_t buff_key[0x100], buff_in[0x100], buff_out[0xC0], buff_print[0x400],
               buff_sound[4][4], buff_speech[4], buff_mouse[0x40];
static uint8_t *const buff_mem[NBUFFERS] = {
    buff_key, buff_in, buff_out, buff_print, buff_sound[0], buff_sound[1], buff_sound[2],
    buff_sound[3], buff_speech, buff_mouse,
};
static uint32_t buff_in_ptr[NBUFFERS], buff_out_ptr[NBUFFERS];

static void console_resume(void);
static int console_ended;       /* the console (a file or a pipe) has no more */

/* NewInsV: R0 the byte, R1 the buffer; C set if it would not go in */
static int insv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    uint32_t b = s->r[1];
    if (b >= NBUFFERS)
        return ROS_VECTOR_PASS;
    uint32_t in = buff_in_ptr[b];
    buff_mem[b][in] = (uint8_t)s->r[0];                 /* stored anyway */
    if (++in == buff_size[b])
        in = 0;
    if (in != buff_out_ptr[b]) {
        buff_in_ptr[b] = in;
        s->c = 0;
        if (b == BUFF_KEY)
            wake_readers();
        return ROS_VECTOR_CLAIM;
    }
    if (b == BUFF_MOUSE || b < BUFF_RS423OUT)           /* an input buffer: Event 1 */
        event(EVENT_INPUT_FULL, b, s->r[0], 0, 0);
    s->c = 1;                                           /* full */
    return ROS_VECTOR_CLAIM;
}

/* NewRemV: V clear remove, set examine; R0 = R2 the byte, C set if empty */
static int remv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    uint32_t b = s->r[1];
    if (b >= NBUFFERS)
        return ROS_VECTOR_PASS;
    uint32_t out = buff_out_ptr[b];
    if (out == buff_in_ptr[b]) {
        s->c = 1;
        return ROS_VECTOR_CLAIM;
    }
    uint8_t ch = buff_mem[b][out];
    s->r[0] = s->r[2] = ch;
    s->c = 0;
    if (s->v)                                           /* examine only */
        return ROS_VECTOR_CLAIM;
    if (++out == buff_size[b])
        out = 0;
    buff_out_ptr[b] = out;
    if (b == BUFF_KEY)
        console_resume();
    if (b != BUFF_MOUSE && b >= BUFF_RS423OUT && out == buff_in_ptr[b])
        event(EVENT_OUTPUT_EMPTY, b, 0, 0, 0);
    s->r[0] = s->r[2] = ch;
    s->c = 0;
    return ROS_VECTOR_CLAIM;
}

/* NewCnpV: V set purge; else count, C clear entries, C set spaces, in R1
 * (low byte) and R2 (high byte) */
static int cnpv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    uint32_t b = s->r[1];
    if (b >= NBUFFERS)
        return ROS_VECTOR_PASS;
    if (s->v) {
        buff_in_ptr[b] = buff_out_ptr[b];
        if (b == BUFF_KEY)
            console_resume();
        return ROS_VECTOR_CLAIM;
    }
    int32_t n = (int32_t)buff_in_ptr[b] - (int32_t)buff_out_ptr[b];
    if (n < 0)
        n += (int32_t)buff_size[b];
    if (s->c)
        n = (int32_t)buff_size[b] - n - 1;
    s->r[1] = (uint32_t)n & 0xFF, s->r[2] = (uint32_t)n >> 8;
    return ROS_VECTOR_CLAIM;
}

/* Through the vectors, as the kernel's INSERT, REMOVE and CnpEntry go */
static int insert(uint32_t b, uint32_t ch)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ch, c.r[1] = b;
    call_vector(INSV, &c);
    return (int)c.c;
}

static int remove_byte(uint32_t b, uint8_t *ch)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = b;
    call_vector(REMV, &c);
    *ch = (uint8_t)c.r[0];
    return (int)c.c;                                    /* 1: empty */
}

static uint32_t count(uint32_t b, int spaces)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = b, c.c = (uint32_t)spaces;
    call_vector(CNPV, &c);
    return (c.r[1] & 0xFF) | c.r[2] << 8;
}

static void purge(uint32_t b)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = b, c.v = 1;
    call_vector(CNPV, &c);
}

/* DoInsertESC: into a buffer, the keyboard's and the serial one's
 * checked for the Escape character */
static int insert_esc(uint32_t b, uint32_t ch)
{
    if (b >= 2 || (VAR(0xB5) & b))                      /* RS423mode: raw serial input */
        return insert(b, ch);
    if (ch == VAR(V_ESCCH) && VAR(V_ESCACTION) == 0) {
        if (!(VAR(V_ESCBREAK) & 1) && event(EVENT_ESCAPE, 0, 0, 0, 0))
            escape_condition();                         /* the event disabled: the condition */
        return 0;
    }
    event(EVENT_CHAR_INPUT, b, ch, 0, 0);
    return insert(b, ch);
}

/* InsertKeyZCOE: a key's character; the Escape character stops the repeat */
static void insert_key(uint32_t ch)
{
    if (VAR(V_KEYBDDISABLE))
        return;
    if (ch == VAR(V_ESCCH) && VAR(V_ESCACTION) == 0)
        autorepeat = 0;
    insert_esc(BUFF_KEY, ch);
}

void ros_keyboard_flush_all(void)
{
    for (int b = NBUFFERS - 1; b >= 0; b--) {
        if (b < BUFF_RS423OUT)
            ros_byte_var_set(V_SOFTKEYLEN, 0);
        purge((uint32_t)b);
    }
}

/* ---- the serial console as a keyboard ----------------------------------------- */

static int console_paused, console_watching;

static void console_ready(void *arg, uint32_t revents)
{
    (void)arg, (void)revents;
    console_watching = 0;
    for (;;) {
        if (count(BUFF_KEY, 1) == 0) {                  /* full: wait for room */
            console_paused = 1;
            return;
        }
        struct pollfd p = { .fd = 0, .events = POLLIN };
        if (poll(&p, 1, 0) <= 0)
            break;
        uint8_t c;
        ssize_t n = read(0, &c, 1);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            console_ended = 1;                          /* no more: OS_ReadC's Ctrl-D */
            wake_readers();
            return;
        }
        if (n != 1)
            continue;
        /* The console is a terminal. Its cursor keys come as ESC [ A-D (or
         * ESC O A-D), and the rest of its keys as other sequences. Unless
         * Escape is a character (OS_Byte 229, which a PTY session sets
         * because it wants the terminal's own sequences), the cursor keys go
         * in as the key handler puts them, &8C-&8F. The other sequences are
         * dropped. Only an ESC on its own is Escape. */
        struct pollfd more = { .fd = 0, .events = POLLIN };
        if (c == 27 && VAR(V_ESCACTION) == 0 && poll(&more, 1, 0) > 0) {
            uint8_t b = 0;
            if (read(0, &b, 1) == 1 && (b == '[' || b == 'O')) {
                while (read(0, &b, 1) == 1 && !(b >= 0x40 && b <= 0x7E))
                    ;
                if (b >= 'A' && b <= 'D')
                    insert(BUFF_KEY, (uint8_t)"\x8F\x8E\x8D\x8C"[b - 'A']);   /* up down right left */
                continue;
            }
            insert_esc(BUFF_KEY, c);
            if (b)
                insert_esc(BUFF_KEY, b);
            continue;
        }
        insert_esc(BUFF_KEY, c);
    }
    console_watching = ros_watch(0, POLLIN, console_ready, NULL) == 0;
}

static void console_resume(void)
{
    if (console_paused && !console_watching) {
        console_paused = 0;
        console_watching = ros_watch(0, POLLIN, console_ready, NULL) == 0;
    }
}

/* ---- keys ---------------------------------------------------------------------- */

static int is_down(uint32_t k)
{
    return k < 0x300 && (keys_down[k >> 5] >> (31 - (k & 31)) & 1);
}

/* CheckForShiftingKey: with no handler, every key is (and nothing happens) */
static int shifting(uint32_t k)
{
    const struct ros_keyhandler *h = keyvec;
    if (!h)
        return 1;
    if (k < h->keytran_size) {
        const uint8_t *t = h->keytran + 4 * k;
        if (!(t[0] == 0xFF && t[1] == 0xFF && t[2] == 0xFF && t[3] == 0xFF))
            return 0;
    }
    for (uint32_t i = h->shifting[0]; i > 0; i--)
        if (h->shifting[i] == k)
            return 1;
    return 0;
}

/* ScanKeys: the highest key down but ignore and the shifting keys, or -1 */
static int32_t scan_keys(uint32_t ignore)
{
    for (int32_t w = 0x300 / 32 - 1; w >= 0; w--) {
        if (w == 0x200 / 32)
            w = 4;                                      /* past the &100 range */
        uint32_t bits = keys_down[w];
        for (int32_t k = w * 32 + 31; bits && k >= w * 32; k--, bits >>= 1)
            if ((bits & 1) && (uint32_t)k != ignore && !shifting((uint32_t)k))
                return k;
    }
    return -1;
}

static void update_leds(void);

/* OfferKeyStatusUpCall: before and after; a claimant may change it */
static uint8_t offer_status(uint8_t old, uint8_t now, int pre)
{
    struct ros_cpu c;
    if (pre) {
        ros_cpu_enter(&c);
        c.r[0] = UPCALL_KEYBOARD_STATUS, c.r[1] = 0, c.r[2] = old, c.r[3] = now;
        call_vector(UPCALLV, &c);
        now = (uint8_t)c.r[3];
    }
    if (now != old) {
        ros_cpu_enter(&c);
        c.r[0] = UPCALL_KEYBOARD_STATUS, c.r[1] = 1, c.r[2] = old, c.r[3] = now;
        call_vector(UPCALLV, &c);
    }
    return now;
}

/* CallUserKeyCode: the handler's code, with KeyBdStatus and PendingAltType */
static void call_user(void (*fn)(struct ros_keyctx *), struct ros_keyctx *c)
{
    uint8_t old = VAR(V_KEYBDSTATUS);
    c->status = old;
    c->pending = pending_alt_type;
    fn(c);
    pending_alt_type = c->pending;
    uint8_t now = c->status;
    if (now != old)
        now = offer_status(old, now, 1);
    ros_byte_var_set(V_KEYBDSTATUS, now);
    update_leds();
}

/* CallSpecialCode: the handler's code for a special key, or no characters */
static void call_special(struct ros_keyctx *c)
{
    const struct ros_keyhandler *h = keyvec;
    c->out = null_list;
    for (uint32_t i = h->nspecial; i > 0; i--)
        if (h->special[i - 1] == c->key) {
            c->special = i;
            call_user(h->code, c);
            return;
        }
}

/* ReturnNChars: more than one character only if they all fit */
static void return_chars(const uint8_t *list)
{
    uint32_t n = list[0];
    if (n == 0)
        return;
    if (n > 1 && n > count(BUFF_KEY, 1))
        return;
    for (uint32_t i = 1; i <= n; i++)
        insert_key(list[i]);
}

static void special_chars(uint32_t action, uint32_t key)
{
    if (!keyvec)
        return;
    struct ros_keyctx c = { .action = action, .key = key };
    call_special(&c);
    return_chars(c.out);
}

/* GenerateChar: action 2 first, 3 repeat */
static void generate_char(uint32_t action, uint32_t key)
{
    const struct ros_keyhandler *h = keyvec;
    if (!h)
        return;
    if (key >= h->keytran_size) {
        special_chars(action, key);
        return;
    }
    uint8_t st = VAR(V_KEYBDSTATUS);
    uint32_t code = h->keytran[4 * key + (st & KBSTAT_CTRL_ENGAGED ? 2 : 0) +
                               (st & KBSTAT_SHIFT_ENGAGED ? 1 : 0)];
    uint32_t up = code & ~0x20u;
    if (up >= 'A' && up <= 'Z') {                       /* Caps Lock */
        if (st & KBSTAT_SHIFT_ENABLE)
            code ^= 0x20;
        else if (!(st & KBSTAT_NO_CAPS_LOCK))
            code &= ~0x20u;
    }
    if (code == 0xFF) {
        special_chars(action, key);
        return;
    }
    if (code == VAR(V_ESCCH) && VAR(V_ESCACTION) == 0 && !(VAR(V_ESCBREAK) & 1) &&
        (st & KBSTAT_PENDING_ALT)) {                    /* Escape cancels a pending Alt */
        uint8_t now = offer_status(st, st & ~KBSTAT_PENDING_ALT, 0);
        ros_byte_var_set(V_KEYBDSTATUS, now);
        st = now;
    }
    if (st & KBSTAT_PENDING_ALT) {                      /* ProcessPendingAlt */
        struct ros_keyctx c = { .action = action, .key = key, .chr = code, .out = null_list };
        call_user(h->pending_alt, &c);
        return_chars(c.out);
        return;
    }
    if (code == 0) {
        return_chars(nulnul_list);
        return;
    }
    insert_key(code);
}

/* ---- the LEDs -------------------------------------------------------------- */

static void set_leds(uint32_t leds)
{
    if (leds == last_led)
        return;
    last_led = (uint8_t)leds;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 3, c.r[1] = leds;                          /* KeyV NotifyLEDState */
    call_vector(KEYV, &c);
}

static void update_leds(void)
{
    if (kbid == KBID_NONE)
        return;
    uint8_t st = VAR(V_KEYBDSTATUS);
    set_leds((st & KBSTAT_NO_CAPS_LOCK ? 0 : 1) | (st & KBSTAT_NO_NUM_LOCK ? 0 : 2) |
             (st & KBSTAT_SCROLL_LOCK ? 4 : 0));
}

/* ---- KeyV: the kernel's claimant ----------------------------------------------- */

static void clear_kbd(void)
{
    curr_key = old_key = NO_KEY;
    last_led = 0xFF;
    memset(keys_down, 0, sizeof keys_down);
}

/* KeyboardEnable: the handler initialised, and the drivers told */
static void keyboard_enable(void)
{
    clear_kbd();
    if (!keyvec || kbid == KBID_NONE)
        return;
    last_kbid = kbid;
    struct ros_keyctx c = { 0 };
    uint8_t old = VAR(V_KEYBDSTATUS);
    c.status = old, c.pending = pending_alt_type;
    keyvec->init(&c, kbid);
    pending_alt_type = c.pending;
    uint8_t now = c.status != old ? offer_status(old, c.status, 1) : old;
    ros_byte_var_set(V_KEYBDSTATUS, now);
    update_leds();
    struct ros_cpu v;
    ros_cpu_enter(&v);
    v.r[0] = 4;                                         /* KeyV EnableDrivers */
    call_vector(KEYV, &v);
}

/* GotKbId */
static void got_kbid(uint32_t id, uint32_t magic)
{
    kbid = (uint8_t)id;
    const struct ros_keyhandler *was = keyvec;
    no_debounce = magic == NO_KBD_MAGIC;
    if (last_kbid != kbid) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[1] = SERVICE_KEYHANDLER, c.r[2] = kbid;
        ros_service_call(&c);
    }
    if (keyvec == was)
        keyboard_enable();
}

static int keyv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    uint32_t reason = s->r[0];
    if (reason >= 3)
        return ROS_VECTOR_CLAIM;
    if (reason == 0) {
        got_kbid(s->r[1] & 0xFF, s->r[2]);
        return ROS_VECTOR_CLAIM;
    }
    uint32_t key = s->r[1], down = reason - 1;
    if (key < 0x300) {
        uint32_t *w = &keys_down[key >> 5], bit = 1u << (31 - (key & 31));
        if (down == (*w & bit ? 1u : 0u))
            return ROS_VECTOR_CLAIM;                    /* already so */
        *w ^= bit;
    }
    event(EVENT_KEYBOARD, down, key, kbid, 0);
    if (shifting(key)) {
        special_chars(down, key);
        return ROS_VECTOR_CLAIM;
    }
    if (down) {
        if (curr_key != NO_KEY) {
            if (old_key != NO_KEY)
                return ROS_VECTOR_CLAIM;                /* two down already */
            old_key = curr_key;
        }
    new_current:
        curr_key = key;
        debouncing = no_debounce ? 0 : 2;
        autorepeat = no_debounce ? VAR(V_KEYREPDELAY) : 2;
        if (no_debounce)
            generate_char(2, key);
        return ROS_VECTOR_CLAIM;
    }
    if (key == old_key) {                               /* the old key up */
        int32_t k = scan_keys(curr_key);
        if (k >= 0) {
            old_key = curr_key;
            key = (uint32_t)k;
            goto new_current;
        }
        old_key = NO_KEY;
    } else if (key == curr_key) {                       /* the current key up */
        int32_t k = scan_keys(old_key);
        if (k >= 0) {
            key = (uint32_t)k;
            goto new_current;
        }
        curr_key = NO_KEY;
    }
    return ROS_VECTOR_CLAIM;
}

/* The key structure as RISC OS lays it out (hdr/Keyboard's KeyHandler_*),
 * for OS_InstallKeyHandler to hand out: the tables copied into the RMA, the
 * code offsets 0. The code is C, which nothing outside may call. */
static uint32_t key_struct;

static void build_key_struct(const struct ros_keyhandler *h)
{
    if (key_struct)
        ros_rma_free(ros_ptr(key_struct));
    key_struct = 0;
    if (!h)
        return;
    uint32_t head = 9 * 4, kt = 4 * h->keytran_size, it = 128 * 4, sh = h->shifting[0] + 1u,
             sp = h->nspecial + 1;
    uint32_t size = head + kt + it + ((sh + 3) & ~3u) + ((sp + 3) & ~3u);
    uint8_t *b = ros_rma_alloc(size);
    if (!b)
        return;
    memset(b, 0, size);
    uint32_t o = head;
    uint32_t w[9] = { o, h->keytran_size };
    memcpy(b + o, h->keytran, kt), o += kt;
    w[2] = o;
    memcpy(b + o, h->inkeytran, it), o += it;
    w[3] = o;
    memcpy(b + o, h->shifting, sh), o += (sh + 3) & ~3u;
    w[4] = o;
    b[o] = (uint8_t)h->nspecial;
    for (uint32_t i = 0; i < h->nspecial; i++)
        b[o + 1 + i] = (uint8_t)h->special[i];
    memcpy(b, w, sizeof w);
    key_struct = ros_addr(b);
}

const struct ros_keyhandler *ros_key_install(const struct ros_keyhandler *h)
{
    const struct ros_keyhandler *old = keyvec;
    keyvec = h;
    build_key_struct(h);
    keyboard_enable();
    return old;
}

const struct ros_keyhandler *ros_key_handler(void)
{
    return keyvec;
}

uint32_t ros_key_kbid(void)
{
    return kbid;
}

/* OS_InstallKeyHandler: 0 read, 1 the keyboard id; a handler of ARM code
 * cannot be run, so it is refused */
void ros_thunk_OS_InstallKeyHandler(struct ros_cpu *s)
{
    s->v = 0;
    if (s->r[0] == 1) {
        s->r[0] = kbid;
        return;
    }
    if (s->r[0] != 0) {
        ros_swi_fail(s, ros_error(0x1E6, "Key handlers in ARM code are not supported"));
        return;
    }
    s->r[0] = keyvec ? key_struct : 0xFFFFFFFFu;
}

void ros_thunk_OS_Reset(struct ros_cpu *s);  /* runtime/sysinfo.c */

/* DoBreakKey: BREAKvector's two bits for this Shift/Ctrl combination. They
 * mean ignore it, or Escape at the key's press, or a reset at its release.
 * The reset is PerformReset, which OS_Reset shares: Service_PreReset, then
 * the machine restarts. */
void ros_key_break(struct ros_keyctx *c)
{
    uint32_t shift = c->status & KBSTAT_SHIFT_ENGAGED ? 29 : 31;
    if (c->status & KBSTAT_CTRL_ENGAGED)
        shift &= ~4u;
    uint32_t v = VAR(V_BREAKVECTOR), carry = v >> (32 - shift) & 1, neg = v >> (31 - shift) & 1;
    if (carry)
        return;                                         /* 2 or 3: ignore */
    if (neg) {
        if (c->action == 1)
            c->out = esc_list;                          /* 1: Escape */
        return;
    }
    if (c->action == 0) {                               /* 0: a reset, at the release */
        struct ros_cpu r;
        ros_cpu_enter(&r);
        r.r[0] = 0;
        ros_thunk_OS_Reset(&r);
    }
}

/* ---- the tick ------------------------------------------------------------------- */

void ros_keyboard_tick(void)
{
    if (inkey_counter)
        inkey_counter--;
    if (curr_key == NO_KEY)
        return;
    update_leds();
    if (autorepeat == 0)
        return;                                         /* frozen */
    if (--autorepeat)
        return;
    uint32_t action;
    if (debouncing) {
        debouncing = 0;
        autorepeat = VAR(V_KEYREPDELAY);
        action = 2;
    } else {
        autorepeat = VAR(V_KEYREPRATE);
        action = 3;
    }
    generate_char(action, curr_key);
}

/* ---- reading -------------------------------------------------------------------- */

static int soft_key_next(uint8_t *ch)
{
    uint32_t len = VAR(V_SOFTKEYLEN);
    if (!len)
        return 0;
    *ch = soft_key[soft_key_end - len];
    ros_byte_var_set(V_SOFTKEYLEN, (uint8_t)(len - 1));
    return 1;
}

/* ReturnNULR0: NUL now, code next */
static void nul_then(uint8_t code)
{
    soft_key[0] = code;
    soft_key_end = 1;
    ros_byte_var_set(V_SOFTKEYLEN, 1);
}

/* ExpandSoftKey: Key$n, expanded; 0 if there is none */
static int expand_soft_key(uint32_t k)
{
    char name[8];
    snprintf(name, sizeof name, "Key$%u", k);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    uint8_t *buf = ros_rma_alloc(256 + 8);
    if (!buf)
        return 0;
    strcpy((char *)buf + 256, name);
    c.r[0] = ros_addr(buf) + 256, c.r[1] = ros_addr(buf), c.r[2] = 255, c.r[3] = 0, c.r[4] = 3;
    ros_swi(&c, XOS_ReadVarVal);
    int ok = !c.v;
    if (ok) {
        memcpy(soft_key, buf, c.r[2]);
        soft_key_end = c.r[2];
        ros_byte_var_set(V_SOFTKEYLEN, (uint8_t)c.r[2]);
    }
    ros_rma_free(buf);
    return ok;
}

/* RDCHG: the next character from the keyboard buffer, as the program is to
 * see it; 0 if there is none */
static int rdchg(uint8_t *out)
{
    uint8_t ch;
    for (;;) {
        if (soft_key_next(out))
            return 1;
        if (remove_byte(BUFF_KEY, &ch))
            return 0;
        if (ch == 0) {                                  /* NUL: its partner */
            if (remove_byte(BUFF_KEY, &ch))
                return 0;
            if (ch != 0) {
                *out = ch;
                return 1;
            }
            for (int i = 0; i < 8; i++)                 /* NUL NUL: a key base of 2? */
                if (VAR(V_IPBUFFERCH + (uint32_t)i) == 2) {
                    nul_then(0);
                    break;
                }
            *out = 0;
            return 1;
        }
        if (!(ch & 0x80)) {
            *out = ch;
            return 1;
        }
        uint32_t low = ch & 0x0F;
        if (low >= 0x0B && !(ch & 0x40)) {              /* a cursor key, or COPY */
            uint32_t mode = VAR(V_CUREDIT);
            uint8_t code = (uint8_t)(low + 0x87 - 0x0B);
            if (mode == 0) {                            /* 0: the OS edits with them */
                if (VAR(V_WRCHDEST) & 0x02)             /* output not to the screen: */
                    continue;                           /* ItsCursorEdit ignores them */
                if (ros_vdu_cursor_edit(code, out))
                    return 1;                           /* COPY: the character it read */
                continue;                               /* the rest move the cursor */
            }
            if (mode == 1) {
                *out = code;
                return 1;
            }
        }
        uint32_t idx = (uint32_t)(ch >> 4) ^ 0x0C;
        uint32_t base = VAR(V_IPBUFFERCH + idx);
        if (base == 0)
            continue;                                   /* ignored */
        if (base == 1) {
            expand_soft_key(low);
            continue;
        }
        if (base == 2) {
            nul_then(ch);
            *out = 0;
            return 1;
        }
        *out = (uint8_t)(base + low);
        return 1;
    }
}

static volatile int read_depth;     /* >0: a read is waiting for a key */

/* Whether the interpreter sits at a line read (a language's > prompt, or
 * an INPUT), as against a program running. A host has no other signal for
 * it. BBC BASIC V for Mac saves the program only then. */
int ros_keyboard_read_waiting(void)
{
    return read_depth;
}

/* Waits until a character, an Escape, or (for INKEY) the count running out.
 * Returns 0 for a character, 1 for Escape and 2 for timed out. */
static int read_wait_inner(int inkey, uint8_t *ch)
{
    for (;;) {
        ros_callbacks_run();
        if (ros_ld8(ESC_STATUS) & ESC_CONDITION) {
            *ch = 27;
            return 1;
        }
        if (rdchg(ch))
            return 0;
        if (!inkey && console_ended) {                  /* a piped console's end: Ctrl-D, */
            *ch = 4;                                    /* which the prompt takes as the end */
            return 0;
        }
        if (inkey && inkey_counter == 0) {
            *ch = 0xFF;
            return 2;
        }
        unsigned d = ros_blocking_begin();
        pthread_mutex_lock(&input_mu);
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_nsec += 10 * 1000 * 1000;
        if (t.tv_nsec >= 1000000000)
            t.tv_sec++, t.tv_nsec -= 1000000000;
        pthread_cond_timedwait(&input_cv, &input_mu, &t);
        pthread_mutex_unlock(&input_mu);
        ros_blocking_end(d);
        ros_streams_idle();                             /* other command lines run */
    }
}

/* read_wait: the inner read with the waiting depth held up around it */
static int read_wait(int inkey, uint8_t *ch)
{
    read_depth++;
    int r = read_wait_inner(inkey, ch);
    read_depth--;
    return r;
}

os_error *ros_keyboard_rdch(uint8_t *ch, int *escape)
{
    *escape = read_wait(0, ch) == 1;
    return NULL;
}

/* OS_Byte 129 with R2 < &80, after *Exec and redirection: INKEY(n) */
void ros_keyboard_inkey(struct ros_cpu *s)
{
    inkey_counter = (s->r[1] & 0xFF) | (s->r[2] & 0xFF) << 8;
    uint8_t ch;
    int r = read_wait(1, &ch);
    s->r[1] = ch;
    s->r[2] = r ? ch : 0;
    s->c = r != 0;
}

/* BBCScanKeys: R1 &80-&FF tests internal key R1 EOR &80 (C set, R1 = R2 =
 * &FF if down); below &80 scans from it (C clear and the key found, or C
 * set and &FF) */
static void bbc_scan_keys(struct ros_cpu *s)
{
    uint32_t r1 = s->r[1] & 0xFF, ext = s->r[2] & 0x7F;
    const struct ros_keyhandler *h = keyvec;
    if (r1 & 0x80) {
        if (!h) {
            s->r[1] = 0, s->c = 0;
            return;
        }
        int down;
        if (r1 == 0x80 + 13 || r1 == 0x80 + 15) {
            down = is_down(ext + (r1 & 2 ? 0x200 : 0x100));
        } else {
            uint32_t w = h->inkeytran[0xFF - r1];
            down = 0;
            while (w != 0xFFFFFFFFu && !down) {
                down = is_down(w & 0xFF);
                w = 0xFF000000u | w >> 8;
            }
        }
        s->r[1] = s->r[2] = down ? 0xFF : 0;
        s->c = (uint32_t)down;
        return;
    }
    if (!h) {
        s->r[1] = 0xFF, s->c = 1;
        return;
    }
    for (; r1 < 0x80; r1++) {
        if ((r1 & (0x7F - 2)) == 13) {                 /* the extension ranges */
            uint32_t base = r1 & 2 ? 0x200 : 0x100;
            for (uint32_t k = base + ext; k < base + 0x100; k++)
                if (is_down(k)) {
                    s->r[1] = (r1 == 13 ? 13u : 15u) | (k & 0xFF) << 8;
                    s->r[2] = 0, s->c = 0;
                    return;
                }
        }
        uint32_t w = h->inkeytran[0x7F - r1];
        while (w != 0xFFFFFFFFu) {
            if (is_down(w & 0xFF)) {
                s->r[1] = r1, s->r[2] = 0, s->c = 0;
                return;
            }
            w = 0xFF000000u | w >> 8;
        }
    }
    s->r[1] = 0xFF, s->r[2] = 0x20000000u, s->c = 1;    /* R2: the C bit, as the kernel leaves it */
}

/* ConvertInternalKey: INKEY code EOR &FF to a key number, &FF none */
static uint32_t convert_internal(uint32_t r)
{
    if (!(r & 0x80) || !keyvec)
        return NO_KEY;
    return keyvec->inkeytran[(r ^ 0x7F) - 0x80] & 0xFF;
}

/* ---- country, alphabet and keyboard (Kernel/s/PMF/osbyte) ------------------------------ */

/* OfferInternationalService: R2 the reason, R3-R5 its parameters; 1 if
 * claimed, R3 and R4 as the service left them */
static int offer_international(uint32_t reason, uint32_t *r3, uint32_t *r4, uint32_t r5)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[1] = SERVICE_INTERNATIONAL, c.r[2] = reason, c.r[3] = *r3, c.r[4] = *r4, c.r[5] = r5;
    ros_service_call(&c);
    *r3 = c.r[3], *r4 = c.r[4];
    return c.r[1] == 0;
}

/* GetCountry: 0 is the configured country; if that is Default too, the
 * keyboard's ID, if below &20 (else 0 still) */
static uint32_t get_country(uint32_t r1)
{
    if (r1 != 0)
        return r1;
    r1 = ros_cmos_read(COUNTRY_CMOS);
    if (r1 != 0)
        return r1;
    return last_kbid < 0x20 ? last_kbid : 0;
}

/* NewKeyboard: the keyboard (a country) and its alphabet, and the key
 * handler told */
static void new_keyboard(uint32_t r3, uint32_t r4, uint32_t r5)
{
    keyboard_no = (uint8_t)r3, key_alphabet = (uint8_t)r4;
    offer_international(INTER_KEYBOARD, &r3, &r4, r5);
}

/* SetAlphabet: the alphabet's characters 32-255 defined in the system
 * font; 1 if the alphabet is known.  *r3 = the alphabet. */
static int set_alphabet(uint32_t *r3, uint32_t r4)
{
    uint32_t first = 32;
    *r3 = r4;
    return offer_international(INTER_DEFINE, r3, &first, 255);
}

/* OS_Byte 70: the country, 127 to read it; setting it sets its alphabet
 * and keyboard.  R1 = 0 back for a country no one knows.  R2 is left as
 * the kernel leaves it: the last reason it issued. */
static void byte_country(struct ros_cpu *s)
{
    uint32_t r1 = s->r[1], r3, r4 = 0;
    if (r1 == 0x7F) {
        s->r[1] = VAR(V_COUNTRY), s->r[2] = 0;
        return;
    }
    r3 = get_country(r1);
    if (!offer_international(INTER_CNO_TO_ANO, &r3, &r4, s->r[5])) {
        s->r[1] = 0, s->r[2] = INTER_CNO_TO_ANO;
        return;
    }
    uint32_t old = VAR(V_COUNTRY);
    ros_byte_var_set(V_COUNTRY, (uint8_t)r3);
    alphabet = (uint8_t)r4;
    new_keyboard(r3, r4, s->r[5]);
    set_alphabet(&r3, r4);
    s->r[1] = old, s->r[2] = INTER_DEFINE;
}

/* OS_Byte 71: R1 < &80 the alphabet (a country's, or itself), &80 + n the
 * keyboard (country n's); 127 and 255 read them.  R1 = 0 back for one no
 * one knows. */
static void byte_alphabet(struct ros_cpu *s)
{
    uint32_t r1 = s->r[1], r3, r4 = 0;
    if (!(r1 & 0x80)) {
        if (r1 == 0x7F) {
            s->r[1] = alphabet, s->r[2] = 0;
            return;
        }
        r3 = get_country(r1);
        if (!offer_international(INTER_CNO_TO_ANO, &r3, &r4, s->r[5]))
            r4 = r3;                                    /* not a country: an alphabet */
        s->r[2] = INTER_DEFINE;
        if (!set_alphabet(&r3, r4)) {
            s->r[1] = 0;
            return;
        }
        s->r[1] = alphabet;
        alphabet = (uint8_t)r3;
        return;
    }
    r1 &= 0x7F;
    if (r1 == 0x7F) {
        s->r[1] = keyboard_no, s->r[2] = 0;
        return;
    }
    r3 = get_country(r1);
    if (!offer_international(INTER_CNO_TO_ANO, &r3, &r4, s->r[5])) {
        s->r[1] = 0, s->r[2] = INTER_CNO_TO_ANO;
        return;
    }
    s->r[1] = keyboard_no, s->r[2] = INTER_KEYBOARD;
    new_keyboard(r3, r4, s->r[5]);
}

uint32_t ros_keyboard_alphabet(void)
{
    return alphabet;
}

/* ---- the OS_Bytes -------------------------------------------------------------- */

/* DoOsbyteVar: new = (old AND R2) EOR R1; R1 the old, R2 the next */
static void byte_var(struct ros_cpu *s, uint32_t n)
{
    uint8_t old = VAR(n), now = (uint8_t)((old & s->r[2]) ^ s->r[1]);
    s->r[1] = old;
    if (n == V_KEYBDSTATUS) {                           /* DoOsbyteKeyStatus */
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = UPCALL_KEYBOARD_STATUS, c.r[1] = 0, c.r[2] = old, c.r[3] = now;
        call_vector(UPCALLV, &c);
        now = (uint8_t)c.r[3];
        ros_byte_var_set(n, now);
        if (now != old) {
            ros_cpu_enter(&c);
            c.r[0] = UPCALL_KEYBOARD_STATUS, c.r[1] = 1, c.r[2] = old, c.r[3] = now;
            call_vector(UPCALLV, &c);
        }
    } else {
        ros_byte_var_set(n, now);
    }
    s->r[2] = n < 0xFF ? VAR(n + 1) : 0;
}

static void read_key_defaults(void)
{
    ros_byte_var_set(V_KEYREPDELAY, ros_cmos_read(0x0C));
    ros_byte_var_set(V_KEYREPRATE, ros_cmos_read(0x0D));
}

static void flush_this(uint32_t b)
{
    if (b < BUFF_RS423OUT)
        ros_byte_var_set(V_SOFTKEYLEN, 0);
    purge(b);
}

int ros_keyboard_byte(struct ros_cpu *s)
{
    uint32_t n = s->r[0] & 0xFF;
    switch (n) {
    case 4:                                             /* cursor keys: &ED */
        s->r[2] = 0;
        byte_var(s, V_CUREDIT);
        return 1;
    case 11:                                            /* repeat delay: &C4 */
        s->r[2] = 0;
        byte_var(s, V_KEYREPDELAY);
        return 1;
    case 12:                                            /* repeat rate: &C5, 0 the defaults */
        if ((s->r[1] & 0xFF) == 0) {
            read_key_defaults();
        } else {
            s->r[2] = 0;
            byte_var(s, V_KEYREPRATE);
        }
        return 1;
    case 15:                                            /* flush all, or the input buffer */
        if (s->r[1] == 0)
            ros_keyboard_flush_all();
        else
            flush_this(VAR(0xB1));
        return 1;
    case 18:                                            /* the function keys cleared */
        ros_byte_var_set(V_SOFTKEYLEN, 0);
        for (int k = 15; k >= 0; k--) {
            char name[8];
            snprintf(name, sizeof name, "Key$%d", k);
            uint8_t *b = ros_rma_alloc(8);
            if (!b)
                continue;
            strcpy((char *)b, name);
            struct ros_cpu c;
            ros_cpu_enter(&c);
            c.r[0] = ros_addr(b), c.r[2] = 0xFFFFFFFFu;
            ros_swi(&c, XOS_SetVarVal);
            ros_rma_free(b);
        }
        return 1;
    case 21:                                            /* flush one buffer */
        flush_this(s->r[1]);
        return 1;
    case 70:                                            /* country: 127 reads it */
        byte_country(s);
        return 1;
    case 71:                                            /* alphabet, keyboard (&80+) */
        byte_alphabet(s);
        return 1;
    case 118:
        update_leds();
        return 1;
    case 120:                                           /* WriteKeysDown */
        curr_key = convert_internal(s->r[1] & 0xFF);
        old_key = convert_internal(s->r[2] & 0xFF);
        return 1;
    case 122:
        s->r[1] = 0x0F;
        /* fall through */
    case 121:
        bbc_scan_keys(s);
        return 1;
    case 128: {                                         /* ADVAL */
        uint32_t x = s->r[1] & 0xFF;
        if (x & 0x80) {
            uint32_t b = x ^ 0xFF;
            uint32_t v = count(b, b != BUFF_MOUSE && b >= BUFF_RS423OUT);
            s->r[1] = v & 0xFF, s->r[2] = v >> 8;
            return 1;
        }
        if (x != 7 && x != 8)
            return 0;
        struct ros_cpu c;
        ros_cpu_enter(&c);
        ros_swi(&c, XOS_Mouse);
        uint32_t v = x == 7 ? c.r[0] : c.r[1];
        s->r[1] = v & 0xFF, s->r[2] = v >> 8 & 0xFF;
        return 1;
    }
    case 138:                                           /* insert */
        s->c = (uint32_t)insert(s->r[1], s->r[2]);
        return 1;
    case 145:                                           /* get */
    case 152: {                                         /* examine */
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[1] = s->r[1], c.v = n == 152;
        call_vector(REMV, &c);
        s->r[2] = c.r[2], s->c = c.c;
        return 1;
    }
    case 153:                                           /* insert, checking for Escape */
        s->c = (uint32_t)insert_esc(s->r[1], s->r[2]);
        return 1;
    case 166:                                           /* where the variables are */
        s->r[1] = (OSBYTE_VARS - 0xA6) & 0xFF, s->r[2] = (OSBYTE_VARS - 0xA6) >> 8 & 0xFF;
        return 1;
    case 167:
        s->r[1] = (OSBYTE_VARS - 0xA6) >> 8 & 0xFF, s->r[2] = VAR(0xA8);
        return 1;
    default:
        break;
    }
    if (n >= 0xA8) {
        byte_var(s, n);
        return 1;
    }
    return 0;
}

/* OS_Byte 129: negative INKEY, or the OS version */
void ros_keyboard_inkey_neg(struct ros_cpu *s)
{
    if ((s->r[1] & 0xFF) == 0) {
        s->r[1] = OSVERSION_ID, s->r[2] = 0;
        return;
    }
    if (ros_vdu_keys_blocked()) {
        /* A virtual display whose window has not got the input focus:
         * nothing is pressed. */
        s->r[1] = s->r[2] = 0;
        return;
    }
    s->r[1] ^= 0x7F, s->r[2] ^= 0x7F;
    bbc_scan_keys(s);
}

/* ---- starting ------------------------------------------------------------------- */

void ros_keyboard_init(void)
{
    for (uint32_t i = 0; i < sizeof byte_var_init; i++)
        ros_st8(OSBYTE_VARS + i, byte_var_init[i]);
    /* ReadCMOSDefaults: Caps Lock and Num Lock, the repeat delay and rate */
    uint8_t start = ros_cmos_read(0x0B), st;
    if (start & 0x20)
        st = KBSTAT_NO_SHIFT_LOCK;                      /* CAPS */
    else if (start & 0x10)
        st = KBSTAT_NO_SHIFT_LOCK | KBSTAT_NO_CAPS_LOCK;        /* NOCAPS */
    else
        st = KBSTAT_NO_SHIFT_LOCK | KBSTAT_SHIFT_ENABLE;        /* SHCAPS */
    if (start & 0x80)
        st |= KBSTAT_NO_NUM_LOCK;
    ros_byte_var_set(V_KEYBDSTATUS, st);
    read_key_defaults();
    ros_byte_var_set(V_COUNTRY, ros_cmos_read(0xBA));
    clear_kbd();
    ros_vector_claim_native(INSV, insv, 0);
    ros_vector_claim_native(REMV, remv, 0);
    ros_vector_claim_native(CNPV, cnpv, 0);
    ros_vector_claim_native(KEYV, keyv, 0);
    insert(BUFF_KEY, 0xCA);                             /* "insert soft key 10" (osinit) */
}

void ros_keyboard_post_init(void)
{
    keyboard_no = 1, alphabet = key_alphabet = 101;     /* KeyPostInit: UK, Latin1 */
    if (last_kbid == KBID_NONE && kbid != KBID_NONE)
        got_kbid(kbid, no_debounce ? NO_KBD_MAGIC : 0);
    struct ros_cpu c;                                   /* the configured country */
    ros_cpu_enter(&c);
    c.r[0] = 70, c.r[1] = VAR(V_COUNTRY);
    ros_swi(&c, XOS_Byte);
    console_watching = ros_watch(0, POLLIN, console_ready, NULL) == 0;
}
