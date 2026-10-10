/* Copyright 1996 Acorn Computers Ltd
 * Copyright 1998 Acorn Computers Ltd
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
 * This file is a reimplementation in C of RISC OS Open's International Keyboard
 * module (Sources/Internat/IntKey: Source.IntKeyBody, Source.KeyStruct, c.keygen).
 */

/* intkey.c: InternationalKeyboard, the key handler, as a native module.
 *
 * This is written from the International Keyboard module (Internat/IntKey,
 * Source/IntKeyBody). Its layout tables are generated from RISC OS's own
 * sources by its own keygen (tools/keylayout.py). So the layouts are RISC
 * OS's, byte for byte, and this file holds the code that reads them.
 * The module installs its key handler with the kernel (runtime/keyboard.c)
 * for a keyboard it recognises. It chooses the layout by the keyboard
 * number (OS_Byte 71). The handler does the following.
 *
 *   - It counts Shift, Ctrl and Alt, the left and right keys of each. It
 *     also handles FN. Mouse buttons are passed back to the kernel. Break
 *     goes to the kernel's BREAKvector action.
 *   - It handles the keypad according to Num Lock, using OS_Byte 238 for
 *     the base and OS_Byte 254 for the modifier. Alt with keypad digits
 *     enters a character by its number. The number is decimal, or hex after
 *     a leading 0. In hex the keypad's operators and A-F count as digits.
 *   - It handles Caps, Num, Scroll, Tab (OS_Byte 219) and Delete.
 *   - Every other key goes through the layout's Unicode table. The table
 *     has variants for Shift, Ctrl and Alt, a map for Caps Lock, and dead
 *     accents. The result is returned in the current alphabet. For UTF-8
 *     that is NUL-prefixed bytes. For other alphabets it is the code whose
 *     UCS character it is in the alphabet's table, which the International
 *     module supplies (Service_International 8). With no table it is the
 *     Latin1 code.
 *
 * Only the UK layout is carried so far. Each other layout needs one more
 * table.
 */
#include <string.h>

#include "keylayout.h"
#include "rosgd/api.h"
#include "rosgd/cpu.h"
#include "rosgd/international.h"
#include "rosgd/keyboard.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"

#define SERVICE_RESET          0x27u
#define SERVICE_KEYHANDLER     0x44u
#define ALPHABET_UTF8          111u
#define KBSTAT_NO_KANA_LOCK    KBSTAT_NO_SHIFT_LOCK        /* reassigned */

enum {                                  /* PendingAltType's bits */
    ALT_ACCENT_MASK = 31, ALT_DOWN = 1 << 5, ALT_DIGITS = 1 << 6, ALT_SELECT_KEYBOARD = 1 << 7,
};

/* The OS_Byte variables it reads */
enum { V_TABCH = 0xDB, V_KEYPAD_BASE = 0xEE, V_KEYPAD_MODIFIER = 0xFE };

/* ---- the workspace (UserKeyWorkSpace, and the module's) --------------------- */

static struct {
    uint8_t shift_count, ctrl_count, alt_count, alt_left_down, fn_down, mouse;
    uint8_t temp_action;
    uint8_t key_return[2];              /* 1, the character */
    uint8_t key_nul_return[3];          /* 2, NUL, the character */
    uint8_t key_utf[13];                /* up to 12: NUL and a byte, six times */
    uint32_t alt_digits;                /* AltDigitValue */
    int32_t fallback;                   /* FallbackCode: the unaccented form */
    uint32_t current_alphabet;          /* CurrentAlphabet: -1 not yet read */
    uint32_t alphabet_table;            /* AlphabetTable: its UCS codes, 0 none */
    uint8_t hex_digits;
    const struct ros_keylayout *layout;
    const struct ros_keyhandler *old;   /* OldKeyHandler */
} W;

static struct ros_keyhandler handler;

static const uint8_t nowt[1] = { 0 };   /* NowtReturn */

static uint32_t os_byte(uint32_t n, uint32_t r1, uint32_t r2)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = n, c.r[1] = r1, c.r[2] = r2;
    ros_swi(&c, XOS_Byte);
    return c.r[1];
}

/* ---- returning characters -------------------------------------------------- */

static void one_char(struct ros_keyctx *c, uint32_t ch)      /* ReturnOneChar */
{
    W.key_return[0] = 1;
    W.key_return[1] = (uint8_t)ch;
    c->out = W.key_return;
}

static void nul_char(struct ros_keyctx *c, uint32_t ch)      /* ReturnNULChar */
{
    W.key_nul_return[0] = 2, W.key_nul_return[1] = 0;
    W.key_nul_return[2] = (uint8_t)ch;
    c->out = W.key_nul_return;
}

/* GetAlphabetTable: reads the current alphabet. If it has changed, this
 * also fetches its table of UCS codes from the International module.
 * UTF-8 has no table. */
static uint32_t get_alphabet_table(void)
{
    uint32_t alphabet = os_byte(71, 0x7F, 0);
    if (alphabet == W.current_alphabet)
        return alphabet;
    W.current_alphabet = alphabet;
    if (alphabet == ALPHABET_UTF8)
        return alphabet;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[1] = SERVICE_INTERNATIONAL, s.r[2] = INTER_UCS_TABLE, s.r[3] = alphabet, s.r[4] = 0;
    ros_service_call(&s);
    W.alphabet_table = s.r[4];
    return alphabet;
}

/* ReturnUCS: a Unicode character in the current alphabet */
static void return_ucs(struct ros_keyctx *c, uint32_t ucs)
{
    for (;;) {
        if (ucs < 0x80) {                               /* ReturnOneOrNUL */
            if (ucs == 0)
                nul_char(c, 0);
            else
                one_char(c, ucs);
            return;
        }
        uint32_t alphabet = get_alphabet_table();
        if (alphabet == ALPHABET_UTF8) {
            uint8_t *p = W.key_utf + sizeof W.key_utf;
            uint32_t n = 2, lim = 0x20;
            for (;;) {
                *--p = (uint8_t)((ucs & 0x3F) | 0x80);
                *--p = 0;
                ucs >>= 6;
                if (ucs < lim)
                    break;
                lim >>= 1;
                n++;
            }
            *--p = (uint8_t)(ucs | (0xFF00u >> n));
            *--p = 0;
            *--p = (uint8_t)(n * 2);
            c->out = p;
            return;
        }
        if (W.alphabet_table) {                         /* ReturnUCSForAlphabet */
            for (uint32_t k = 0x80; k <= 0xFF; k++)
                if (ros_ld32(W.alphabet_table + k * 4) == ucs) {
                    nul_char(c, k);
                    return;
                }
        } else if (ucs <= 0xFF) {                       /* ReturnUCSForNoAlphabet: Latin 1 */
            nul_char(c, ucs);
            return;
        }
        if (W.fallback == -1) {
            c->out = nowt;
            return;
        }
        ucs = (uint32_t)W.fallback;
        W.fallback = -1;
    }
}

static void clear_accent(struct ros_keyctx *c)
{
    c->pending &= ~ALT_ACCENT_MASK;
    if (!c->pending)
        c->status &= ~KBSTAT_PENDING_ALT;
}

/* ReturnUCSAlt: with a dead accent pending, the accented form */
static void return_ucs_alt(struct ros_keyctx *c, uint32_t ucs)
{
    uint32_t acc = c->pending & ALT_ACCENT_MASK;
    if (!acc) {
        W.fallback = -1;
        return_ucs(c, ucs);
        return;
    }
    const uint32_t *list = ros_intkey_accents[acc];
    clear_accent(c);
    for (;; list += 2) {
        if (list[0] > ucs)
            break;                                      /* not in the list */
        if (list[0] == ucs) {
            W.fallback = (int32_t)ucs;
            ucs = list[1];
            break;
        }
    }
    return_ucs(c, ucs);
}

/* ---- the special keys ---------------------------------------------------------- */

static void shift_or_ctrl(struct ros_keyctx *c, uint8_t *count, uint8_t bit)
{
    uint32_t n = *count;
    if (c->action) {
        *count = (uint8_t)(n + 1);
        c->status |= bit;
        return;
    }
    n -= 1;                                             /* SUBS on the word */
    *count = (uint8_t)n;
    if (n == 0)
        c->status &= ~bit;
    else
        c->status |= bit;
}

static void k_shift(struct ros_keyctx *c)
{
    if (W.alt_left_down == 1 && W.shift_count == 0 && c->action == 1)
        c->status ^= KBSTAT_NO_KANA_LOCK;
    shift_or_ctrl(c, &W.shift_count, KBSTAT_SHIFT_ENGAGED);
}

static void k_mouse(struct ros_keyctx *c, uint8_t bit)
{
    if (c->action)
        W.mouse |= bit;
    else
        W.mouse &= (uint8_t)~bit;
    ros_key_mouse_buttons(W.mouse);
}

static const struct { uint32_t idd, country; } idd_table[] = {
    { 44, 1 }, { 39, 4 }, { 34, 5 }, { 33, 6 }, { 49, 7 }, { 351, 8 }, { 1100, 9 }, { 30, 10 },
    { 46, 11 }, { 358, 12 }, { 45, 14 }, { 47, 15 }, { 354, 16 }, { 90, 20 }, { 353, 22 },
    { 852, 23 }, { 7, 24 }, { 972, 26 }, { 52, 27 }, { 61, 29 }, { 43, 30 }, { 32, 31 },
    { 81, 32 }, { 31, 34 }, { 41, 35 }, { 1222, 36 }, { 1, 48 }, { 2222, 49 }, { 27, 52 },
    { 9944, 70 }, { 991, 71 }, { 19244, 72 }, { 1001, 80 }, { 1002, 81 }, { 1003, 82 },
    { 1004, 83 }, { 1005, 84 }, { 1006, 85 }, { 1007, 86 }, { 1008, 87 }, { 1009, 88 },
};

/* SelectKeyboard: calls OS_Byte 71, as the original does. The original does
 * it on a callback. */
static void select_keyboard(uint32_t kb)
{
    os_byte(71, kb, 0);
}

static void k_alt(struct ros_keyctx *c)
{
    if (W.alt_count == 0)
        W.alt_digits = 0, W.hex_digits = 0;
    uint32_t n = W.alt_count;
    if (c->action) {
        W.alt_count = (uint8_t)(n + 1);
        c->pending |= ALT_DOWN;
        c->status |= KBSTAT_PENDING_ALT;
        return;
    }
    n -= 1;
    W.alt_count = (uint8_t)n;
    if (n != 0) {
        c->pending |= ALT_DOWN;
        c->status |= KBSTAT_PENDING_ALT;
        return;
    }
    if (c->pending & ALT_DIGITS) {
        uint32_t v = W.alt_digits;
        if (c->pending & ALT_SELECT_KEYBOARD) {
            for (size_t i = 0; i < sizeof idd_table / sizeof idd_table[0]; i++)
                if (idd_table[i].idd == v) {
                    select_keyboard(idd_table[i].country | 0x80);
                    break;
                }
        } else if (os_byte(71, 0x7F, 0) == ALPHABET_UTF8) {
            c->pending &= ~(ALT_DOWN | ALT_DIGITS);
            if (!c->pending)
                c->status &= ~KBSTAT_PENDING_ALT;
            return_ucs(c, v & ~0x80000000u);
            return;
        } else {
            nul_char(c, v);
        }
    }
    c->pending &= ~(ALT_DOWN | ALT_DIGITS | ALT_SELECT_KEYBOARD);
    if (c->pending)
        c->status |= KBSTAT_PENDING_ALT;
    else
        c->status &= ~KBSTAT_PENDING_ALT;
}

static void k_caps(struct ros_keyctx *c)
{
    if (c->action != 2)
        return;
    if (c->status & KBSTAT_SHIFT_ENGAGED) {
        c->status &= ~KBSTAT_NO_CAPS_LOCK;
        c->status |= KBSTAT_SHIFT_ENABLE;
    } else {
        c->status ^= KBSTAT_NO_CAPS_LOCK;
        c->status &= ~KBSTAT_SHIFT_ENABLE;
    }
}

static void pending_alt(struct ros_keyctx *c);

static void k_tab(struct ros_keyctx *c)
{
    uint32_t ch = ros_byte_var(V_TABCH);
    if (ch & 0x80) {
        if (c->status & KBSTAT_SHIFT_ENGAGED)
            ch ^= 0x10;
        if (c->status & KBSTAT_CTRL_ENGAGED)
            ch ^= 0x20;
    }
    if (!(c->status & KBSTAT_PENDING_ALT) || !(c->pending & ALT_ACCENT_MASK)) {
        one_char(c, ch);
        return;
    }
    c->chr = ch;                                        /* ReturnOneCharAlt */
    pending_alt(c);
}

static void k_toggle(struct ros_keyctx *c, uint8_t bit)
{
    if (c->action == 2)
        c->status ^= bit;
}

static void k_shift_caps(struct ros_keyctx *c)
{
    if (c->action != 2)
        return;
    c->status ^= KBSTAT_NO_CAPS_LOCK;
    if (c->status & KBSTAT_NO_CAPS_LOCK)
        c->status &= ~KBSTAT_SHIFT_ENABLE;
    else
        c->status |= KBSTAT_SHIFT_ENABLE;
}

static const uint8_t *pad_num(void)
{
    return W.layout->pad_num ? W.layout->pad_num : ros_intkey_pad_num;
}

static const uint8_t *pad_cur(void)
{
    return W.layout->pad_cur ? W.layout->pad_cur : ros_intkey_pad_cur;
}

static const uint8_t hex_pad[][2] = {       /* HexPadTable: the keypad's physical keys */
    { 0x23, 0xA }, { 0x24, 0xB }, { 0x25, 0xC }, { 0x3A, 0xC }, { 0x4B, 0xD }, { 0x66, 0xE },
    { 0x67, 0xF },
};

static void got_hex(uint32_t d)
{
    W.alt_digits = (W.alt_digits << 4) + d;
}

static void alt_keypad(struct ros_keyctx *c)
{
    uint32_t i = c->special - W.layout->pad_first;
    if (!W.hex_digits) {
        uint32_t d = pad_num()[i] - (uint32_t)'0';
        if (d >= 10) {
            c->pending &= ~(ALT_DIGITS | ALT_SELECT_KEYBOARD);
            W.alt_digits = 0;
            return;
        }
        c->pending |= ALT_DIGITS;
        uint32_t v = W.alt_digits * 10 + d;
        if (v == 0 && !(c->pending & ALT_SELECT_KEYBOARD))
            W.hex_digits = 10;                          /* a leading 0: hex */
        W.alt_digits = v;
        return;
    }
    uint32_t d = pad_num()[i] - (uint32_t)'0';
    if (d >= 10) {
        d = 0;
        for (size_t k = 0; k < sizeof hex_pad / sizeof hex_pad[0]; k++)
            if (hex_pad[k][0] == c->key) {
                d = hex_pad[k][1];
                break;
            }
    }
    got_hex(d);
}

static void k_pad(struct ros_keyctx *c)
{
    if (c->pending & ALT_DOWN) {
        alt_keypad(c);
        return;
    }
    clear_accent(c);
    const uint8_t *t = c->status & KBSTAT_NO_NUM_LOCK ? pad_cur() : pad_num();
    uint32_t ch = t[c->special - W.layout->pad_first];
    if (ch == 0xFF)
        return;
    ch = ch - '0' + ros_byte_var(V_KEYPAD_BASE);
    if (ros_byte_var(V_KEYPAD_MODIFIER) == 0 && (ch & 0x80)) {
        if (c->status & KBSTAT_SHIFT_ENGAGED)
            ch ^= 0x10;
        if (c->status & KBSTAT_CTRL_ENGAGED)
            ch ^= 0x20;
    }
    one_char(c, ch);
}

/* CheckHexAtoF: a key while digits are being entered, as a hex digit */
static void check_hex_a_to_f(uint32_t sym)
{
    if (!W.hex_digits)
        return;
    sym &= ~0x20u;
    if (sym >= 'A' && sym <= 'F')
        got_hex(sym - 'A' + 10);
}

static void special_key(struct ros_keyctx *c, uint32_t n)
{
    c->action = W.temp_action;
    switch (n) {
    case 0: k_toggle(c, KBSTAT_SCROLL_LOCK); break;
    case 1: k_toggle(c, KBSTAT_NO_NUM_LOCK); break;
    case 2: k_tab(c); break;
    case 3: k_caps(c); break;
    case 4: k_toggle(c, KBSTAT_NO_KANA_LOCK); break;
    case 5: k_shift_caps(c); break;
    case 6: select_keyboard(0x81); c->out = nowt; break;     /* the UK keyboard */
    case 7: select_keyboard(0x80); c->out = nowt; break;     /* the configured one */
    case 8:
        if (c->pending & ALT_DOWN)
            c->pending |= ALT_SELECT_KEYBOARD;
        break;
    case 9: one_char(c, 0x7F); break;
    default: break;                                     /* nothing */
    }
}

static void k_ucs(struct ros_keyctx *c)
{
    const uint32_t *t = c->status & KBSTAT_NO_KANA_LOCK ? W.layout->ucs0 : W.layout->ucs1;
    const uint32_t *e = t + 9 * (c->special - W.layout->ucs_first);
    if (c->pending & ALT_DIGITS) {
        check_hex_a_to_f(e[0]);
        return;
    }
    W.temp_action = (uint8_t)c->action;
    uint32_t idx = (c->status & KBSTAT_SHIFT_ENGAGED ? 1 : 0) |
                   (c->status & KBSTAT_CTRL_ENGAGED ? 2 : 0) | (c->pending & ALT_DOWN ? 4 : 0);
    if (!(c->status & KBSTAT_NO_CAPS_LOCK)) {           /* Caps Lock's map */
        uint32_t alt = e[8] >> (4 * idx) & 0xF;
        if (!(alt & 8))
            idx = alt;
        else if (c->status & KBSTAT_SHIFT_ENABLE)
            idx = alt & 7;
    }
    uint32_t ucs = e[idx];
    if (!(ucs & 0x80000000u)) {
        return_ucs_alt(c, ucs);
        return;
    }
    if (ucs == 0xFFFFFFFFu) {                           /* FunnyUCS: nothing */
        c->out = nowt;
        return;
    }
    ucs &= ~0x80000000u;
    if (ucs < 0x100)
        one_char(c, ucs);                               /* a raw buffer code */
    else if (ucs < 0x10000)
        ;                                               /* daft: nothing */
    else if (ucs < 0x20000)
        special_key(c, ucs - 0x10000);
    else if (ucs < 0x30000) {                           /* a dead key */
        c->pending = (uint8_t)((c->pending & ~ALT_ACCENT_MASK) | (ucs - 0x20000));
        c->status |= KBSTAT_PENDING_ALT;
    } else {
        c->out = nowt;
    }
}

/* The special code table, indexed by special number (SpecialCodeTable) */
static void code(struct ros_keyctx *c)
{
    uint32_t s = c->special;
    if (s >= W.layout->ucs_first) {
        k_ucs(c);
        return;
    }
    if (s >= W.layout->pad_first && s < W.layout->pad_first + 17) {
        k_pad(c);
        return;
    }
    switch (s) {
    case 1: case 2: k_shift(c); break;
    case 3: case 4: shift_or_ctrl(c, &W.ctrl_count, KBSTAT_CTRL_ENGAGED); break;
    case 5: case 6: k_alt(c); break;
    case 7: W.fn_down = (uint8_t)c->action; break;
    case 8: k_mouse(c, 4); break;                       /* left: Select */
    case 9: k_mouse(c, 2); break;                       /* centre: Menu */
    case 10: k_mouse(c, 1); break;                      /* right: Adjust */
    case 11: ros_key_break(c); break;
    default:
        s -= W.layout->pad_first + 17;                  /* after the keypad */
        if (s == 0)
            k_toggle(c, KBSTAT_SCROLL_LOCK);
        else if (s == 1)
            k_toggle(c, KBSTAT_NO_NUM_LOCK);
        else if (s == 2)
            k_tab(c);
        else if (s == 3)
            k_caps(c);
        break;
    }
}

/* PendingAltCode: a simple key while Alt, or an accent, is pending */
static void pending_alt(struct ros_keyctx *c)
{
    if (c->pending & ALT_DIGITS) {
        if (c->key < W.layout->keytran_size)
            check_hex_a_to_f(W.layout->keytran[4 * c->key]);
        return;
    }
    if (!(c->pending & ALT_ACCENT_MASK))
        return;
    if (c->chr < 0x80) {
        return_ucs_alt(c, c->chr);
        return;
    }
    clear_accent(c);
    one_char(c, c->chr);
}

/* KeyStructInit */
static void init(struct ros_keyctx *c, uint32_t kbid)
{
    (void)kbid;
    W.shift_count = W.ctrl_count = W.alt_count = W.alt_left_down = W.fn_down = W.mouse = 0;
    c->status &= ~(KBSTAT_SHIFT_ENGAGED | KBSTAT_CTRL_ENGAGED | KBSTAT_PENDING_ALT);
    c->status |= KBSTAT_NO_KANA_LOCK;
    c->pending = 0;
}

/* ---- the module ---------------------------------------------------------------- */

/* SetUpKeyStructureAndHandlerIfUs: 1 if the handler is ours now */
static int set_up(void)
{
    uint32_t id = ros_key_kbid();
    if (id != 1 && id != 2 && id != 3 && id != 4 && id != 0xFF) {  /* Archimedes, PC, RCMM,
                                                                      Pandora, none */
        if (ros_key_handler() == &handler)
            ros_key_install(W.old);
        return 0;
    }
    uint32_t kb = os_byte(71, 0xFF, 0);                 /* the keyboard number */
    (void)kb;                                           /* only the UK layout is carried, so every keyboard uses it */
    W.layout = &ros_keylayout_uk;
    W.current_alphabet = 0xFFFFFFFFu;
    W.fallback = -1;
    get_alphabet_table();
    handler = (struct ros_keyhandler){
        .keytran = W.layout->keytran, .keytran_size = W.layout->keytran_size,
        .inkeytran = ros_intkey_inkeytran, .shifting = ros_intkey_shifting,
        .special = W.layout->special, .nspecial = W.layout->nspecial,
        .init = init, .code = code, .pending_alt = pending_alt,
    };
    if (ros_key_handler() != &handler)
        W.old = ros_key_install(&handler);
    return 1;
}

static os_error *mod_init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    uint32_t kb = os_byte(71, 0xFF, 0);                 /* the current keyboard, reset */
    os_byte(71, kb | 0x80, 0);
    set_up();
    return NULL;
}

static os_error *mod_final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    if (ros_key_handler() == &handler)
        ros_key_install(W.old);
    return NULL;
}

static void service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    switch (s->r[1]) {
    case SERVICE_KEYHANDLER:
        if (set_up())
            s->r[1] = 0;
        break;
    case SERVICE_RESET:
        set_up();
        break;
    case SERVICE_INTERNATIONAL:
        if (s->r[2] == INTER_KEYBOARD)
            set_up();
        break;
    default:
        break;
    }
}

struct ros_module intkey_module = {
    .title = "InternationalKeyboard",
    .help = "Int'l Keyboard\t1.00 (26 Sep 2026) ROSGD native",
    .init = mod_init,
    .final = mod_final,
    .service = service,
};
