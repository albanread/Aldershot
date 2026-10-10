/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* keyboard.h -- the kernel's keyboard: keys to characters, the kernel's
 * buffers, reading characters, and the OS_Byte variables they live in.
 *
 * Written from the kernel's (Kernel/s/PMF/key, Buffer, osbyte, osinit;
 * hdr/KeyWS). Keys arrive as RISC OS's low-level key numbers on KeyV,
 * from the Input module, which gets them from Linux. The kernel's
 * KeyV owner, here, keeps which keys are down, repeats the current one, and
 * turns keys into characters in the keyboard buffer through the key handler
 * that OS_InstallKeyHandler installs: the International Keyboard module's
 * (modules/intkey), which knows the layout.  OS_ReadC and INKEY read the
 * buffer.  The serial console is a second keyboard: what is typed there goes
 * into the same buffer.
 */
#ifndef ROSGD_KEYBOARD_H
#define ROSGD_KEYBOARD_H

#include <stdint.h>

#include "rosgd/cpu.h"
#include "rosgd/error.h"

/* ---- key handlers (hdr/Keyboard's KeyHandler_*) ---------------------------- */

/* KeyBdStatus, OS_Byte 202 */
#define KBSTAT_PENDING_ALT   0x01u
#define KBSTAT_SCROLL_LOCK   0x02u
#define KBSTAT_NO_NUM_LOCK   0x04u
#define KBSTAT_SHIFT_ENGAGED 0x08u
#define KBSTAT_NO_CAPS_LOCK  0x10u
#define KBSTAT_NO_SHIFT_LOCK 0x20u
#define KBSTAT_CTRL_ENGAGED  0x40u
#define KBSTAT_SHIFT_ENABLE  0x80u

/* What the kernel passes a handler's code, and what it gets back: the
 * registers of the kernel's CallSpecialCode, CallUserKeyCode and
 * ProcessPendingAlt, by name. */
struct ros_keyctx {
    uint32_t action;            /* R1: 0 up, 1 down (shifting keys); 2 first, 3 repeat */
    uint32_t key;               /* R2: the low-level key number */
    uint32_t chr;               /* R3: for the pending-Alt code, the character */
    uint32_t special;           /* R4: the special key's number, from 1 */
    uint8_t status;             /* R5: KeyBdStatus, updated */
    uint8_t pending;            /* R7: PendingAltType, updated */
    const uint8_t *out;         /* R6: what to insert: a length, then the bytes */
};

/* A key handler written in C: RISC OS's key structure, with functions for
 * its code offsets.  The tables are the structure's, as the kernel reads
 * them. */
struct ros_keyhandler {
    const uint8_t *keytran;     /* 4 bytes a key: plain, Shift, Ctrl, Ctrl-Shift;
                                   &FF special */
    uint32_t keytran_size;      /* keys in it */
    const uint32_t *inkeytran;  /* 128 words: INKEY -128..-1's keys, &FF none */
    const uint8_t *shifting;    /* the shifting keys: a count, then keys */
    const uint16_t *special;    /* the special keys: special number = index + 1 */
    uint32_t nspecial;
    void (*init)(struct ros_keyctx *c, uint32_t kbid);  /* KeyStructInit */
    void (*code)(struct ros_keyctx *c);                 /* the special key code */
    void (*pending_alt)(struct ros_keyctx *c);          /* PendingAltCode */
};

/* OS_InstallKeyHandler for handlers in C: the old handler back; and the
 * handler installed now */
const struct ros_keyhandler *ros_key_install(const struct ros_keyhandler *h);
const struct ros_keyhandler *ros_key_handler(void);
/* The kernel's ReturnVector: the handler's way back in */
void ros_key_mouse_buttons(uint32_t lcr);   /* MouseButtonChange: bit 0 R, 1 C, 2 L */
void ros_key_break(struct ros_keyctx *c);   /* DoBreakKey */
/* KbId: &FF until a keyboard says what it is */
uint32_t ros_key_kbid(void);

/* ---- the OS_Byte variables (&A6-&FF) -------------------------------------- */

/* The kernel's OsbyteVars, indexed by OS_Byte number */
uint8_t ros_byte_var(uint32_t n);
void ros_byte_var_set(uint32_t n, uint8_t v);

/* ---- the runtime's calls ---------------------------------------------------- */

/* Before the ROM's modules start: the vectors' kernel owners (KeyV, InsV,
 * RemV, CnpV, at the chains' feet), the variables from CMOS; after them,
 * KeyPostInit. */
void ros_keyboard_init(void);
void ros_keyboard_post_init(void);
/* Each centisecond tick: autorepeat, INKEY's count */
void ros_keyboard_tick(void);
/* RdchV's default owner and OS_Byte's: 1 if the OS_Byte was the keyboard's */
os_error *ros_keyboard_rdch(uint8_t *ch, int *escape);
/* >0 while a read is blocked waiting for a key: a language's > prompt or
 * an INPUT, as against a program running (a host's signal) */
int ros_keyboard_read_waiting(void);
int ros_keyboard_byte(struct ros_cpu *s);
/* The current alphabet (OS_Byte 71's), for the VDU's font resets */
uint32_t ros_keyboard_alphabet(void);
/* OS_Byte 129 after *Exec and redirection: INKEY(n) with R2 < &80, and
 * INKEY(-n) (and -256, the OS version) with R2 >= &80 */
void ros_keyboard_inkey(struct ros_cpu *s);
void ros_keyboard_inkey_neg(struct ros_cpu *s);
/* Escape acknowledged (OS_Byte 126): the buffers flushed, as the kernel does */
void ros_keyboard_flush_all(void);

#endif
