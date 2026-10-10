/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* capp.c -- C applications: the loader and the gate (capp.h).
 *
 * The loader is FileSwitch's Run_Absolute8000File for an x32 ELF image.
 * The gate is how that image's code makes SWIs. The program is user code
 * in the arena, and the kernel is reached only through one page of the
 * arena, &FEEFF000. The page moves the thread onto its native stack above
 * 4 GB and calls ros_swi in user mode, with the program's registers and
 * with R13 as its stack pointer. From there the call takes every user-mode
 * SWI's path. The SWI banks R13 and runs on the task's SVC stack (swi.c),
 * and its exit is the outermost one, where callbacks run.
 *
 * The page holds trampolines into the runtime. The runtime is a static
 * PIE, far above 4 GB, so the page is how a 32-bit address reaches it.
 * Each trampoline is `movabs $target, %r11; jmp *%r11`. The page is mapped
 * read-only and executable once it is written:
 *
 *   &FEEFF000  ros_capp_gate            the SWI gate
 *   &FEEFF010  ros_capp_returned        a return from the program's entry
 *   &FEEFF020  ros_capp_nested_return   a return from a nested entry
 *   &FEEFF030  ros_capp_ended           a return from a delivered handler
 *   &FEEFF040  ros_capp_gate_psr        the SWI gate, entered with flags
 *
 * so x32 code's stacks hold only arena addresses.
 *
 * The native stack used is the thread's own, below the frame that entered
 * the x32 code running now. That frame is the program's entry or the
 * innermost nested entry. The gate state that the code uses says which
 * (ros_capp_user, per thread). It is the task's own, in struct ros_task,
 * while the program's code runs, and a nested entry's own while one runs.
 * Everything above that frame stays live while the program runs. That
 * includes the *Run that loaded the program, and its setjmp. So the
 * program's exit can longjmp there (module.c), and delivery can flatten
 * to run_program's frame.
 *
 * Entries into x32 code (capp.h) are the dispatcher's fallback for x32
 * ranges (ros_capp_call, nested), and the error and exit handlers
 * (ros_capp_deliver, delivered). The static base %gs is set on each.
 * What the runtime knows of a task's application is kept per task (capp.h)
 * in the task. Application code belongs to the task whose memory is mapped
 * (slot_owner).
 */
#include <errno.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/auxv.h>
#include <sys/syscall.h>
#endif

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/armbox.h"
#include "rosgd/background.h"
#include "rosgd/capp.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/error.h"
#include "rosgd/heap.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/switrace.h"
#include "rosgd/task.h"
#if ROS_CAPP_NATIVE
#include "fileswitch.h"                 /* ros_hostfs_linux_path: the image's file, for symbols */
#endif

/* ---- the image ----------------------------------------------------------------- */

struct elf32_ehdr {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct elf32_phdr {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

#define ET_EXEC   2
#define EM_X86_64 62
#define PT_LOAD   1
#define PT_NOTE   4
#ifndef PF_X                    /* <elf.h>, through <sys/auxv.h> in the box, has it */
#define PF_X      1
#endif
#define ELFOSABI_ROSGD 255      /* ELFOSABI_STANDALONE: not a Linux program */
#define NT_ROSGD_IMAGE 1
#define ROSGD_IMAGE_VERSION 1
#define MAX_PHDRS 16

int ros_capp_is_image(const void *bytes, uint32_t size)
{
    const struct elf32_ehdr *h = bytes;
#if ROS_CAPP_A64
    /* x32 images are accepted too. read_headers refuses them as the other
     * machine's, so that they are not taken for ARM code. */
    return size >= sizeof *h && !memcmp(h->ident, "\177ELF", 4) && h->ident[4] == 1 /* 32 */ &&
           h->ident[5] == 1 /* LSB */ && (h->machine == ROS_CAPP_EM || h->machine == EM_X86_64);
#else
    return size >= sizeof *h && !memcmp(h->ident, "\177ELF", 4) && h->ident[4] == 1 /* 32 */ &&
           h->ident[5] == 1 /* LSB */ && h->machine == ROS_CAPP_EM;
#endif
}

static uint32_t rma_string(const char *s, size_t n)
{
    char *p = ros_rma_alloc((uint32_t)n + 1);
    if (!p)
        return 0;
    memcpy(p, s, n);
    p[n] = 0;
    return ros_addr(p);
}

static os_error *bad_image(const char *name, const char *why)
{
    return ros_error(ROS_ERR_UNIMPLEMENTED, "'%s' is not a C application ROSGD can run: %s", name,
                     why);
}

/* ---- reading the file: only what is needed, where it is needed ------------------------ */

/* The file, open for reading, and a small buffer in the RMA for its
 * headers. The image itself is read straight to its addresses, as
 * FileSwitch loads an Absolute to &8000. It is never read whole into the
 * RMA. So an ARM Absolute bigger than the free RMA is still told it is ARM
 * code, and an image is limited by application space alone. */
struct file {
    uint32_t handle, len;
    unsigned char *buf;         /* BUF bytes in the RMA */
};
#define BUF 512u

static void file_close(struct file *f)
{
    if (f->handle) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        c.r[0] = 0, c.r[1] = f->handle;
        ros_swi(&c, XOS_Find);
        f->handle = 0;
    }
    if (f->buf)
        ros_rma_free(f->buf);
    f->buf = NULL;
}

/* Reads n bytes from pos to the arena address to. *got is how many there
 * were. */
static os_error *file_read(const struct file *f, uint32_t to, uint32_t pos, uint32_t n,
                           uint32_t *got)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 3, c.r[1] = f->handle, c.r[2] = to, c.r[3] = n, c.r[4] = pos;
    ros_swi(&c, XOS_GBPB);
    if (c.v)
        return ros_ptr(c.r[0]);
    *got = n - c.r[3];
    return NULL;
}

static os_error *file_open(const char *path, struct file *f)
{
    *f = (struct file){ 0 };
    uint32_t name = rma_string(path, strlen(path));
    f->buf = ros_rma_alloc(BUF);
    if (!name || !f->buf) {
        if (name)
            ros_rma_free(ros_ptr(name));
        file_close(f);
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    }
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 0x4F, c.r[1] = name;       /* OpenIn: it must exist, not be a directory, and use no path */
    ros_swi(&c, XOS_Find);
    ros_rma_free(ros_ptr(name));
    os_error *e = c.v ? ros_ptr(c.r[0]) : NULL;
    if (!e) {
        f->handle = c.r[0];
        ros_cpu_enter(&c);
        c.r[0] = 2, c.r[1] = f->handle;     /* its extent */
        ros_swi(&c, XOS_Args);
        e = c.v ? ros_ptr(c.r[0]) : NULL;
        f->len = c.r[2];
    }
    if (e)
        file_close(f);
    return e;
}

/* What the headers say, checked: the ELF header, the program headers and
 * the ROSGD note (roscc's src/x32.rs: "ROSGD\0", NT_ROSGD_IMAGE, then the
 * image ABI version, flags, stack, reserved). An image is refused, and
 * nothing of it runs, if it is for another OS/ABI (such as a Linux x32
 * program filed as &FF8), if it is another version, or if it has flags
 * that a version 1 loader does not know. */
struct image {
    struct elf32_ehdr h;
    struct elf32_phdr ph[MAX_PHDRS];
    uint32_t stack;             /* the note's stack size. 0 means the runtime's default */
};

/* Finds the ROSGD note among the notes in f->buf[0, len). It returns the
 * note's descriptor, or NULL. */
static const unsigned char *rosgd_note(const unsigned char *b, uint32_t len)
{
    for (uint32_t at = 0; len >= 12 && at <= len - 12;) {
        uint32_t w[3];
        memcpy(w, b + at, sizeof w);
        uint32_t nlen = (w[0] + 3) & ~3u, dlen = (w[1] + 3) & ~3u;
        if (w[0] > len || w[1] > len || nlen + dlen > len - at - 12)
            return NULL;
        if (w[0] == 6 && w[2] == NT_ROSGD_IMAGE && !memcmp(b + at + 12, "ROSGD", 6) && w[1] >= 12)
            return b + at + 12 + nlen;
        at += 12 + nlen + dlen;
    }
    return NULL;
}

static os_error *read_headers(const char *name, const struct file *f, struct image *im)
{
    char why[160];
    uint32_t got;
    os_error *e = file_read(f, ros_addr(f->buf), 0, sizeof im->h, &got);
    if (e)
        return e;
    memcpy(&im->h, f->buf, sizeof im->h);
    const struct elf32_ehdr *h = &im->h;
    if (got < sizeof im->h || !ros_capp_is_image(h, got))
        return bad_image(name, "not an ELF32 " ROS_CAPP_MACHINE " image");
#if ROS_CAPP_A64
    if (h->machine != ROS_CAPP_EM)
        return bad_image(name, "it is an x32 image, for the x86-64 box; this box runs A64X32");
#endif
    if (h->ident[7] != ELFOSABI_ROSGD) {
        snprintf(why, sizeof why, "its OS/ABI is %u, not %u (ROSGD): it is not a RISC OS program",
                 h->ident[7], ELFOSABI_ROSGD);
        return bad_image(name, why);
    }
    if (h->type != ET_EXEC)
        return bad_image(name, "not an executable");
    if (h->phentsize != sizeof(struct elf32_phdr) || h->phnum == 0 || h->phnum > MAX_PHDRS ||
        h->phoff > f->len || (uint64_t)h->phnum * sizeof(struct elf32_phdr) > f->len - h->phoff)
        return bad_image(name, "its program headers are not in the file");
    uint32_t n = h->phnum * (uint32_t)sizeof(struct elf32_phdr);
    if ((e = file_read(f, ros_addr(f->buf), h->phoff, n, &got)))
        return e;
    memcpy(im->ph, f->buf, n);
    const unsigned char *d = NULL;
    for (unsigned i = 0; i < h->phnum && !d; i++) {
        const struct elf32_phdr *p = &im->ph[i];
        if (p->type != PT_NOTE)
            continue;
        if (p->offset > f->len || p->filesz > f->len - p->offset)
            return bad_image(name, "its note is not in the file");
        uint32_t len = p->filesz < BUF ? p->filesz : BUF;
        if ((e = file_read(f, ros_addr(f->buf), p->offset, len, &got)))
            return e;
        d = rosgd_note(f->buf, got);
    }
    if (!d)
        return bad_image(name, "it has no ROSGD note (link it with roscc)");
    uint32_t v[3];
    memcpy(v, d, sizeof v);
    if (v[0] != ROSGD_IMAGE_VERSION) {
        snprintf(why, sizeof why, "it is a ROSGD image of version %u; this loader runs version %u",
                 v[0], ROSGD_IMAGE_VERSION);
        return bad_image(name, why);
    }
    if (v[1] != 0) {
        snprintf(why, sizeof why,
                 "its ROSGD note has flags &%X, which a version %u loader does not know", v[1],
                 ROSGD_IMAGE_VERSION);
        return bad_image(name, why);
    }
    im->stack = v[2];
    return NULL;
}

/* ---- *Run -------------------------------------------------------------------------- */

/* Checks the memory the image needs, as FileSwitch's
 * ValidateR2R5_WriteToCoreCodeLoad and CheckAIFMemoryLimit want it. Once
 * the Wimp has application space to give (Wimp_ReadSysInfo 11, which gives
 * 0 before the Wimp starts), [&8000, end) must lie inside that limit, and
 * the slot is grown by Wimp_SlotSize if it is smaller. Then, always, all of
 * it must be writable (OS_ValidateAddress) and below the memory limit. The
 * task gets application space first if it has none (the Wimp_SlotSize
 * path, task.c). */
static os_error *memory_for(uint32_t end)
{
    os_error *e = ros_task_give_slot();
    if (e)
        return e;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 11;
    ros_swi(&c, XWimp_ReadSysInfo);
    if (!c.v && c.r[0] >= ROS_APP_BASE) {
        if (end > c.r[0])
            return ros_error(ROS_ERR_CORE_NOT_WRITABLE, "No writable memory at this address");
        ros_cpu_enter(&c);
        c.r[0] = 0xFFFFFFFFu, c.r[1] = 0xFFFFFFFFu;
        ros_swi(&c, XWimp_SlotSize);
        if (!c.v && end - ROS_APP_BASE > c.r[0]) {
            uint32_t want = end - ROS_APP_BASE;
            ros_cpu_enter(&c);
            c.r[0] = want, c.r[1] = 0xFFFFFFFFu;
            ros_swi(&c, XWimp_SlotSize);
        }
    }
    int invalid = 1;
    xos_validate_address(ROS_APP_BASE, end, &invalid);
    ros_cpu_enter(&c);
    c.r[0] = 0, c.r[1] = 0, c.r[2] = 0, c.r[3] = 0;     /* check it is below the memory limit */
    ros_swi(&c, XOS_ChangeEnvironment);
    if (invalid || c.v || end > c.r[1])
        return ros_error(ROS_ERR_CORE_NOT_WRITABLE, "No writable memory at this address");
    return NULL;
}

os_error *ros_capp_memory_for(uint32_t end)
{
    return memory_for(end);
}

#if ROS_CAPP_NATIVE

/* What the image asks for: its segments' extent, and where it starts.
 * The image is refused as FileSwitch refuses an Absolute that runs too low
 * or starts outside its code. */
struct layout {
    uint32_t lo, hi;            /* every PT_LOAD: [lo, hi) */
    uint32_t code_lo, code_hi;  /* the executable ones */
    uint32_t entry;
};

static os_error *image_layout(const struct image *im, uint32_t len, struct layout *l)
{
    *l = (struct layout){ ~0u, 0, ~0u, 0, im->h.entry };
    for (unsigned i = 0; i < im->h.phnum; i++) {
        struct elf32_phdr p = im->ph[i];
        if (p.type != PT_LOAD || p.memsz == 0)
            continue;
        if (p.filesz > p.memsz || p.offset > len || p.filesz > len - p.offset ||
            (uint64_t)p.vaddr + p.memsz + 0x2000u > 0xFFFFFFFFu)
            return ros_error(ROS_ERR_UNIMPLEMENTED, "A segment of the image is not in the file, "
                             "or not below 4 GB");
        if (p.vaddr < ROS_APP_BASE)
            return ros_error(ROS_ERR_CODE_TOO_LOW, "Code runs too low");
        if (p.vaddr < l->lo) l->lo = p.vaddr;
        if (p.vaddr + p.memsz > l->hi) l->hi = p.vaddr + p.memsz;
        if (p.flags & PF_X) {
            if (p.vaddr < l->code_lo) l->code_lo = p.vaddr;
            if (p.vaddr + p.memsz > l->code_hi) l->code_hi = p.vaddr + p.memsz;
        }
    }
    if (l->entry < l->code_lo || l->entry >= l->code_hi)
        return ros_error(ROS_ERR_EXEC_NOT_IN_CODE, "Execution address not within code");
    return NULL;
}

/* ---- the gate, and the ways in and out of the program (the box only) ---------------- */

#if ROS_CAPP_A64
#include "arch/aarch64/capp.c.inc"
#else
#include "arch/x86_64/capp.c.inc"
#endif

/* ---- which code is x32 --------------------------------------------------------------- */

/* The application running on this thread, innermost. It holds the task it
 * runs in, whose struct ros_task holds what the runtime knows of it
 * (capp.h). It also holds the frame that delivery flattens to, which is
 * run_program's. The native frames belong to this thread. */
struct app {
    struct ros_task *task;
    jmp_buf flat;                       /* delivery longjmps here */
    const void *frame;
    struct entry *entries;              /* the nested entries there were when it started */
    struct ros_swi_frame *trace;
    uint32_t entry;                     /* its ELF entry point */
    uint32_t deliver_code, deliver_r12, deliver_base;
    uint32_t deliver_r[3];              /* R0-R2, as the kernel enters the handler */
    int deliver_status;
    uint32_t deliver_fpc;               /* the program's floating point control, */
    uint16_t deliver_fpc2;              /* which the handler gets */
    int deliver_full;                   /* set for an exception handler, which gets deliver_block whole */
    uint32_t deliver_block[17];
    uint32_t limit;                     /* its RAM limit: the top of its stack */
    uint32_t guard_lo, guard_hi, emergency_sp;      /* set by ros_capp_stack */
};
static _Thread_local struct app *app;

/* A nested entry in progress, on the native stack. It holds its own gate
 * state and what it replaced. */
struct entry {
    struct entry *prev;
    struct ros_capp_gate_state gate;    /* its code's: its gate calls' native and saved sp */
    struct ros_capp_gate_state *user;
    uint64_t ret_sp;
    uint32_t base, mode;
    uint32_t fpc;                       /* the runtime's floating point control */
    uint16_t fpc2;
};
static _Thread_local struct entry *entry_top;
static _Thread_local uint32_t mode_now = ROS_MODE_USR;   /* the mode of the gate's callers */

#define RANGES 64
static struct {
    uint32_t lo, hi, base;
    int kind;
} ranges[RANGES];

os_error *ros_capp_code_add(uint32_t lo, uint32_t hi, int kind, uint32_t base)
{
    for (unsigned i = 0; i < RANGES; i++)
        if (!ranges[i].kind) {
            ranges[i].lo = lo, ranges[i].hi = hi, ranges[i].base = base, ranges[i].kind = kind;
            ros_armrun_code_native(lo, hi);     /* ARM code's branches there are native now (#146) */
            return NULL;
        }
    return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room for another C code range");
}

void ros_capp_code_remove(uint32_t lo)
{
    for (unsigned i = 0; i < RANGES; i++)
        if (ranges[i].kind && ranges[i].lo == lo)
            ranges[i].kind = 0;
}

os_error *ros_capp_code_assembled(uint32_t lo, uint32_t hi)
{
    SYNC_ASSEMBLED(lo, hi);             /* the code was just written, so synchronise the instruction cache (AArch64) */
    /* One range per block. It is grown over any range it overlaps or
     * touches. */
    for (unsigned i = 0; i < RANGES; i++)
        if (ranges[i].kind == ROS_CAPP_ASSEMBLED && lo <= ranges[i].hi && ranges[i].lo <= hi) {
            if (ranges[i].lo < lo)
                lo = ranges[i].lo;
            if (ranges[i].hi > hi)
                hi = ranges[i].hi;
            ranges[i].kind = 0;
        }
    return ros_capp_code_add(lo, hi, ROS_CAPP_ASSEMBLED, 0);
}

void ros_capp_code_forget_assembled(uint32_t lo, uint32_t hi)
{
    for (unsigned i = 0; i < RANGES; i++)
        if (ranges[i].kind == ROS_CAPP_ASSEMBLED && ranges[i].lo < hi && lo < ranges[i].hi)
            ranges[i].kind = 0;
}

/* Finds the task whose application's code is at addr in application space.
 * That is the task whose memory is mapped at &8000 now (capp.h), however
 * the thread asking came to be here. It may be the task's own thread, the
 * Wimp's switch on another task's, or the background thread. The memory is
 * told by its memfd, which is the same file however AMB holds or resizes
 * it. That holds whether it is the task's own slot or, once adopted, the
 * Wimp's node. When the descriptor matches, the file's identity is checked
 * too, because a descriptor can be reused. The result is NULL if there is
 * no memory mapped, or it holds no running x32 application, or addr is
 * outside that application's code. */
static struct ros_task *slot_owner(uint32_t addr)
{
    const struct ros_slot *m = ros_slot_current;
    if (addr < ROS_APP_BASE || addr >= ROS_APP_LIMIT || !m || m->fd < 0)
        return NULL;
    struct stat st;
    int known = 0;
    for (struct ros_task *t = ros_task_next(NULL); t; t = ros_task_next(t)) {
        const struct ros_capp_task *c = ros_task_capp(t);
        if (addr < c->code_lo || addr >= c->code_hi || c->mem_fd != m->fd || ros_task_ended(t))
            continue;
        if (!known && fstat(m->fd, &st) != 0)
            return NULL;
        known = 1;
        if ((uint64_t)st.st_dev == c->mem_dev && (uint64_t)st.st_ino == c->mem_ino)
            return t;
    }
    return NULL;
}

/* Gives the kind of code at addr and the base it runs with, given its R12.
 * If the code is an application's, it also gives the task. */
static int code_base(uint32_t addr, uint32_t r12, uint32_t *base, struct ros_task **owner)
{
    struct ros_task *t = slot_owner(addr);
    if (t) {
        *base = ros_task_capp(t)->base;
        if (owner)
            *owner = t;
        return ROS_CAPP_TASK;
    }
    for (unsigned i = 0; i < RANGES; i++)
        if (ranges[i].kind && addr >= ranges[i].lo && addr < ranges[i].hi) {
            *base = ranges[i].kind == ROS_CAPP_LIBRARY ? r12 : ranges[i].base;
            return ranges[i].kind;
        }
    return 0;
}

int ros_capp_code(uint32_t addr)
{
    uint32_t base;
    return code_base(addr, 0, &base, NULL);
}

void ros_capp_task_base(uint32_t base)
{
    if (app)
        ros_task_capp(app->task)->base = base;
    set_gs(base);
}

int ros_capp_module_base(uint32_t addr, uint32_t base)
{
    for (unsigned i = 0; i < RANGES; i++)
        if (ranges[i].kind == ROS_CAPP_MODULE && addr >= ranges[i].lo && addr < ranges[i].hi) {
            ranges[i].base = base;
            set_gs(base);
            return 1;
        }
    return 0;
}

/* ---- nested entries ------------------------------------------------------------------ */

/* The x32 stack a nested entry runs on. It is the SVC stack, below where
 * the caller stands. That is the caller's sp if it is on that stack, and
 * otherwise where the SVC stack's live frames end. RISC OS runs UpCall,
 * event and vector code there. */
static uint32_t entry_stack(const struct ros_cpu *s)
{
    uint32_t sp = s->r[13];
    if (sp - ROS_SVCSTACK_BASE <= ROS_SVCSTACK_SIZE && sp < ros_svc_sp)
        return sp;
    return ros_svc_sp;
}

int ros_capp_call(struct ros_cpu *s, uint32_t addr)
{
    uint32_t base;
    int kind = code_base(addr, s->r[12], &base, NULL);
    if (!kind)
        return 0;
#if ROS_CAPP_A64
    /* BASIC's AArch64 code is a nested entry with x1 as its SWI entry,
     * which is the gate. */
    ENTRY_X1(kind == ROS_CAPP_ASSEMBLED ? ROS_CAPP_GATE_PAGE : 0);
#endif
    /* The block below a red zone's worth, then the code's stack below it */
    uint32_t block = (entry_stack(s) - 128 - 17 * 4) & ~15u;
    for (unsigned i = 0; i < 15; i++)
        ros_st32(block + 4 * i, s->r[i]);
    ros_st32(block + 60, s->r[14]);
    ros_st32(block + 64, ros_cpsr(s));
    struct entry e = { .prev = entry_top, .gate = { 0, block }, .user = ros_capp_user,
                       .ret_sp = ros_capp_ret_sp, .base = gs_in_force(), .mode = mode_now,
                       .fpc = FP_CTL_NOW() };
    FP_CTL2_STORE(e.fpc2);
    uint32_t svc = ros_svc_sp;
    if (block - ROS_SVCSTACK_BASE <= ROS_SVCSTACK_SIZE && block < ros_svc_sp)
        ros_svc_sp = block;
    entry_top = &e;
    mode_now = s->mode;
    ros_capp_user = &e.gate;
    set_gs(base);
    ros_capp_call_x32(addr, block, block);
#if ROS_CAPP_A64
    ENTRY_X1(0);
#endif
    FP_LOAD(e.fpc, e.fpc2);
    set_gs(e.base);
    mode_now = e.mode;
    ros_capp_user = e.user;
    entry_top = e.prev;
    ros_svc_sp = svc;
    for (unsigned i = 0; i < 16; i++)
        s->r[i] = ros_ld32(block + 4 * i);
    ros_msr_f(s, ros_ld32(block + 64));
    return 1;
}

void ros_capp_unwind(const void *to)
{
    struct entry *e = entry_top, *outermost = NULL;
    while (e && (uintptr_t)e < (uintptr_t)to) {
        outermost = e;
        e = e->prev;
    }
    if (!outermost)
        return;
    entry_top = e;
    FP_LOAD(outermost->fpc, outermost->fpc2);
    set_gs(outermost->base);
    mode_now = outermost->mode;
    ros_capp_user = outermost->user;
    ros_capp_ret_sp = outermost->ret_sp;
}

/* ---- the gate's C half, and the program's ways out ------------------------------------ */

/* This runs on the native stack, with the runtime's floating point in
 * force. R14 and R15 are the caller's return address, for anyone who
 * reports where a SWI came from. The mode is the one the calling code was
 * entered in. */
uint64_t ros_capp_swi(uint32_t number, uint32_t in, uint32_t out, uint64_t sp, uint32_t psr,
                      uint64_t lr)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.mode = mode_now;
    ros_msr_f(&s, psr & 0xF0000000u);
    uint32_t ret = GATE_RET(sp, lr);
    s.r[13] = (uint32_t)sp;
    s.r[14] = s.r[15] = ret;
    /* The blocks are the program's, so a fault copying them is its fault
     * (fault.h). */
    gate_sp = (uint32_t)sp, gate_ret = ret, gate_copying = 1;
    for (unsigned i = 0; i < 10; i++)
        s.r[i] = ros_ld32(in + 4 * i);
    gate_copying = 0;
    /* Use the SVC stack below the caller's frames and its red zone. A
     * nested entry's stack is the SVC stack, below where ros_capp_call put
     * it. */
    uint32_t svc = ros_svc_sp, below = ((uint32_t)sp - 128) & ~15u;
    if (below - ROS_SVCSTACK_BASE <= ROS_SVCSTACK_SIZE && below < ros_svc_sp)
        ros_svc_sp = below;
    ros_swi(&s, number);
    ros_svc_sp = svc;
    gate_sp = (uint32_t)sp, gate_ret = ret, gate_copying = 1;
    for (unsigned i = 0; i < 10; i++)
        ros_st32(out + 4 * i, s.r[i]);
    gate_copying = 0;
    return (uint64_t)ros_cpsr(&s) << 32 | (s.v ? s.r[0] : 0);
}

__attribute__((noreturn)) void ros_capp_returned_c(void)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.mode = ROS_MODE_USR;
    ros_swi(&s, OS_Exit);
    ros_module_app_exit();                      /* OS_Exit does not return */
    __builtin_unreachable();
}

__attribute__((noreturn)) void ros_capp_ended_c(void)
{
    ros_env_end_program(app ? app->deliver_status : 1);
}

int ros_capp_gate_init(void)
{
    GATE_INIT_MACHINE();
    void *page = ros_ptr(ROS_CAPP_GATE_PAGE);
    if (mmap(page, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) !=
        page)
        return -errno;
    memset(page, GATE_FILL, 4096);
    uint64_t to[5] = { (uint64_t)(uintptr_t)ros_capp_gate, (uint64_t)(uintptr_t)ros_capp_returned,
                       (uint64_t)(uintptr_t)ros_capp_nested_return,
                       (uint64_t)(uintptr_t)ros_capp_ended, (uint64_t)(uintptr_t)ros_capp_gate_psr };
    for (unsigned i = 0; i < 5; i++) {
        unsigned char *p = (unsigned char *)page + 16 * i;
        memcpy(p, gate_tramp, sizeof gate_tramp);
        memcpy(p + GATE_TRAMP_TARGET, &to[i], 8);
    }
    __builtin___clear_cache((char *)page, (char *)page + 4096);
    return mprotect(page, 4096, PROT_READ | PROT_EXEC) ? -errno : 0;
}

/* ---- delivery ------------------------------------------------------------------------- */

/* Whether the memory mapped at &8000 is the one the task's application was
 * loaded into (its record in the task). It may instead be another task's,
 * paged in by the Wimp's switch on this thread (Wimp07 ExitPoll). The
 * handlers there are that task's, as RISC OS's mapslotin makes them.
 * Memory that ROSGD cannot tell (there was no slot when the application
 * started) counts as the task's. */
static int mapped_is(const struct ros_capp_task *c)
{
    if (c->mem_fd < 0)
        return 1;
    const struct ros_slot *m = ros_slot_current;
    struct stat st;
    return m && m->fd == c->mem_fd && fstat(m->fd, &st) == 0 && (uint64_t)st.st_dev == c->mem_dev &&
           (uint64_t)st.st_ino == c->mem_ino;
}

/* Gives the kind of x32 code at code, and its base given r12, if a handler
 * there can be delivered to this thread's application. The code may be the
 * application's own or anyone's (the library's, a module's). The task's
 * memory must be at &8000, because delivery flattens this thread's frames
 * into that task's program. It returns 0 if the handler cannot be
 * delivered, and environment.c then calls it as it calls compiled code. */
static int deliverable(const struct app *a, uint32_t code, uint32_t r12, uint32_t *base)
{
    struct ros_task *owner = NULL;
    int kind = a ? code_base(code, r12, base, &owner) : 0;
    if (!kind || (kind == ROS_CAPP_TASK && owner != a->task) || !mapped_is(ros_task_capp(a->task)))
        return 0;
    return kind;
}

/* Delivers a handler. It records where the program stood when it last
 * called the OS itself. That is its task's saved sp, which nested entries
 * leave alone. It also records the program's floating point control from
 * then, with the exception flags clear (capp.h). Then it flattens to
 * run_program, which enters the handler. */
__attribute__((noreturn)) static void deliver(struct app *a, uint32_t code, uint32_t base,
                                              int status)
{
    const struct ros_capp_task *t = ros_task_capp(a->task);
    a->deliver_fpc = FP_NO_FLAGS(t->gate.fpc);
    a->deliver_fpc2 = t->gate.fpc2;
    a->deliver_code = code;
    a->deliver_base = base;
    a->deliver_status = status;
    longjmp(a->flat, 1);
}

void ros_capp_deliver(uint32_t code, uint32_t r12, const uint32_t r[3], int status)
{
    struct app *a = app;
    uint32_t base;
    if (!deliverable(a, code, r12, &base))
        return;
    a->deliver_full = 0;
    a->deliver_r12 = r12;
    memcpy(a->deliver_r, r, sizeof a->deliver_r);
    deliver(a, code, base, status);
}

int ros_capp_exception_handler(uint32_t code)
{
    uint32_t base;
    int kind = deliverable(app, code, 0, &base);
    return kind == ROS_CAPP_TASK || kind == ROS_CAPP_LIBRARY;
}

void ros_capp_deliver_exception(uint32_t code, const uint32_t block[17])
{
    struct app *a = app;
    if (!ros_capp_exception_handler(code))
        return;
    a->deliver_full = 1;
    memcpy(a->deliver_block, block, sizeof a->deliver_block);
    deliver(a, code, ros_task_capp(a->task)->base, 1);     /* the task's base, because the handler has no R12 */
}

/* The program, run as the application (module.c). Its entry is entered
 * with the command line, on a stack at the top of its memory. Once
 * delivery has flattened everything back to here, its error or exit
 * handler is entered instead. Its gate state is its task's. */
static void run_program(void *arg)
{
    struct app *a = arg;
    struct ros_capp_task *t = ros_task_capp(a->task);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    ros_swi(&c, XOS_GetEnv);
    uint32_t line = c.r[0], limit = c.r[1], tail = line;
    while (ros_ld8(tail) > ' ')
        tail++;
    while (ros_ld8(tail) == ' ')
        tail++;
    a->frame = __builtin_frame_address(0);
    a->entries = entry_top;
    a->trace = ros_switrace_top();
    a->limit = limit;
    if (setjmp(a->flat) == 0) {
        ros_capp_user = &t->gate;
        t->gate.saved_sp = (limit & ~15u) - ENTRY_SP_BIAS;
        t->gate.fpc = FP_CLEAN_CTL, t->gate.fpc2 = FP_CLEAN_CTL2;
        ros_capp_entry_fp = 0;
        mode_now = ROS_MODE_USR;
        set_gs(t->base);
        ros_capp_enter(a->entry, limit & ~15u, line, tail, ROS_CAPP_EXIT);
    }
    /* Delivered. The handler is entered as the kernel enters these
     * handlers. The SVC stack is flat, no SWI is in progress, and the C
     * handlers and nested entries of the frames left behind are gone. The
     * handler runs on the program's stack below where it last called the OS
     * itself, which is the task's saved sp, and nested entries leave that
     * alone. */
    ros_svc_sp = ROS_SVCSTACK_BASE + ROS_SVCSTACK_SIZE;
    ros_call_depth = 0;
    ros_handler_chain(NULL);
    ros_resume_unwind(a->frame);
    ros_switrace_unwind(a->trace);
    entry_top = a->entries;
    ros_capp_user = &t->gate;
    mode_now = ROS_MODE_USR;
    set_gs(a->deliver_base);
    /* A stack that the program cannot write, such as a runaway sp or a
     * fault's, is no place to enter the handler. The program's emergency
     * stack is used instead (ros_capp_stack), or else the top of its
     * memory. */
    uint32_t sp = (uint32_t)t->gate.saved_sp;
    if (sp < ROS_APP_BASE + 1024 || !ros_arena_valid(sp - 1024, sp))
        sp = a->emergency_sp ? a->emergency_sp : a->limit & ~15u;
    uint32_t block = (sp - 128 - 17 * 4) & ~15u;
    if (a->deliver_full) {              /* an exception handler gets the register block */
        for (unsigned i = 0; i < 17; i++)
            ros_st32(block + 4 * i, a->deliver_block[i]);
    } else {
        for (unsigned i = 0; i < 17; i++)
            ros_st32(block + 4 * i, 0);
        for (unsigned i = 0; i < 3; i++)
            ros_st32(block + 4 * i, a->deliver_r[i]);
        ros_st32(block + 48, a->deliver_r12);
        ros_st32(block + 64, ROS_MODE_USR);
    }
    t->gate.saved_sp = block;
    t->gate.fpc = a->deliver_fpc, t->gate.fpc2 = a->deliver_fpc2;
    ros_capp_entry_fp = 1;
    ENTRY_FPC = a->deliver_fpc;
    ENTRY_FPC2 = a->deliver_fpc2;
    ros_capp_enter(a->deliver_code, block, block, 0, ROS_CAPP_ENDED);
}

/* The program's errors are its own. No C handler of the command that ran
 * it catches them (error.h), so they go to its error handler, as a raise
 * with none does. What the task knows of the program is the task's while
 * it runs (capp.h). That is its code, the memory it was loaded into, its
 * base and stacks, and its name. The handler chain, the task's record of an
 * application it ran inside, and this thread's x32 state come back when the
 * program ends. */
static os_error *start(const struct layout *l, const char *path)
{
    struct ros_task *task = ros_task_current();
    if (!task)
        return ros_error(ROS_ERR_UNIMPLEMENTED, "A C application runs in a task: there is none");
    struct ros_capp_task *t = ros_task_capp(task), outer_task = *t;
    struct ros_handler *chain = ros_handler_chain(NULL);
    struct ros_capp_gate_state *user = ros_capp_user;
    uint64_t ret = ros_capp_ret_sp;
    struct app *outer = app;
    struct entry *entries = entry_top;
    uint32_t mode = mode_now, base = gs_in_force();
    unsigned fpc = FP_CTL_NOW();
    unsigned short fpc2;
    FP_CTL2_STORE(fpc2);
    *t = (struct ros_capp_task){ .code_lo = l->code_lo, .code_hi = l->code_hi, .mem_fd = -1 };
    struct stat st;
    if (ros_slot_current && fstat(ros_slot_current->fd, &st) == 0) {
        t->mem_fd = ros_slot_current->fd;
        t->mem_dev = (uint64_t)st.st_dev;
        t->mem_ino = (uint64_t)st.st_ino;
    }
    /* Record its name and file, for a fault's report (fault.c). The name
     * is the leaf of the name that *Run had. The file is the Linux file, if
     * the program is on HostFS. */
    const char *leaf = strrchr(path, '.');
    leaf = leaf ? leaf + 1 : path;
    snprintf(t->name, sizeof t->name, "%s", leaf[0] ? leaf : path);
    if (ros_hostfs_linux_path(path, t->file, sizeof t->file))
        t->file[0] = 0;
    uint32_t entry_fp = ros_capp_entry_fp, entry_fpc = ENTRY_FPC;
    uint16_t entry_fpc2 = ENTRY_FPC2;
    struct app a = { .task = task, .entry = l->entry };
    app = &a;
    ros_module_run_as_application(run_program, &a);
    app = outer;
    /* Its stack guard goes with it, if its memory is still there (capp.h). */
    if (a.guard_hi > a.guard_lo && ros_slot_in_guard(a.guard_lo))
        ros_slot_guard(0, 0);
    *t = outer_task;
    entry_top = entries;
    mode_now = mode;
    ros_capp_user = user;
    set_gs(base);
    FP_LOAD(fpc, fpc2);
    ros_capp_ret_sp = ret;
    ros_capp_entry_fp = entry_fp, ENTRY_FPC = entry_fpc, ENTRY_FPC2 = entry_fpc2;
    ros_handler_chain(chain);
    return NULL;
}

/* ---- faults (fault.c) ------------------------------------------------------------- */

int ros_capp_where(uint32_t addr, uint32_t *lo, const char **file, const char **name)
{
    const struct ros_task *t = slot_owner(addr);         /* the task whose memory is paged in */
    if (t) {
        const struct ros_capp_task *c = ros_task_capp(t);
        *lo = c->code_lo;
        *file = c->file[0] ? c->file : NULL;
        *name = c->name;
        return ROS_CAPP_TASK;
    }
    for (unsigned i = 0; i < RANGES; i++)
        if (ranges[i].kind && addr >= ranges[i].lo && addr < ranges[i].hi) {
            *lo = ranges[i].lo;
            *file = NULL;
            *name = ranges[i].kind == ROS_CAPP_LIBRARY     ? "the library"
                    : ranges[i].kind == ROS_CAPP_ASSEMBLED ? "BASIC's assembled code"
                                                           : "a module";
            return ranges[i].kind;
        }
    return 0;
}

uint32_t ros_capp_mode(void)
{
    return mode_now;
}

uint64_t ros_capp_native_stack(void)
{
    return ros_capp_user ? ros_capp_user->native_sp : 0;
}

os_error *ros_capp_stack(uint32_t guard_lo, uint32_t guard_hi, uint32_t emergency_sp)
{
    struct app *a = app;
    if (!a)
        return ros_error(ROS_ERR_BAD_ADDRESS, "No C application is running to have a stack guard");
    int e = ros_slot_guard(guard_lo, guard_hi);
    if (e)
        return ros_error(ROS_ERR_BAD_ADDRESS, "A stack guard at &%08X-&%08X cannot be made: %s",
                         guard_lo, guard_hi, strerror(-e));
    a->guard_lo = guard_lo;
    a->guard_hi = guard_hi;
    a->emergency_sp = emergency_sp & ~15u;
    return NULL;
}

int ros_capp_stack_overflow(uint32_t addr)
{
    const struct app *a = app;
    return a && a->guard_hi > a->guard_lo && addr - a->guard_lo < a->guard_hi - a->guard_lo &&
           ros_slot_in_guard(addr);
}

void ros_capp_fault(uint32_t sp, uint32_t fpc, uint16_t fpc2, int overflow)
{
    struct app *a = app;
    if (!a || entry_top != a->entries)
        return;                         /* nested, so delivery has the program's from before it */
    if (overflow && a->emergency_sp)
        sp = a->emergency_sp;
    struct ros_capp_task *t = ros_task_capp(a->task);
    t->gate.saved_sp = sp;              /* run_program checks that it can be written */
    t->gate.fpc = fpc;
    t->gate.fpc2 = fpc2;
}

int ros_capp_gate_fault(uint32_t *sp, uint32_t *ret)
{
    if (!gate_copying)
        return 0;
    gate_copying = 0;
    *sp = gate_sp;
    *ret = gate_ret;
    return 1;
}

#else

/* Hosted: no 32-bit-pointer code runs here, and there is no gate. On an
 * Apple Silicon Mac that defines ROS_CAPP_A64_JIT (BBC BASIC V for Mac,
 * capp.h), BASIC's AArch64 code does run, from copies in a MAP_JIT arena. */

int ros_capp_gate_init(void)
{
    return 0;
}

os_error *ros_capp_code_add(uint32_t lo, uint32_t hi, int kind, uint32_t base)
{
    (void)lo, (void)hi, (void)kind, (void)base;
    return ros_error(ROS_ERR_UNIMPLEMENTED, "The hosted build has no 32-bit-pointer code");
}

void ros_capp_code_remove(uint32_t lo)
{
    (void)lo;
}

#ifdef ROS_CAPP_A64_JIT
#include "arch/aarch64/capp_jit.c.inc"
#else
os_error *ros_capp_code_assembled(uint32_t lo, uint32_t hi)
{
    (void)lo, (void)hi;
    return NULL;                    /* it is assembled here, but run only in the box */
}

void ros_capp_code_forget_assembled(uint32_t lo, uint32_t hi)
{
    (void)lo, (void)hi;
}

int ros_capp_call(struct ros_cpu *s, uint32_t addr)
{
    (void)s, (void)addr;
    return 0;
}
#endif

int ros_capp_code(uint32_t addr)
{
    (void)addr;
    return 0;
}

void ros_capp_deliver(uint32_t code, uint32_t r12, const uint32_t r[3], int status)
{
    (void)code, (void)r12, (void)r, (void)status;
}

void ros_capp_task_base(uint32_t base)
{
    (void)base;
}

int ros_capp_module_base(uint32_t addr, uint32_t base)
{
    (void)addr, (void)base;
    return 0;
}

uint64_t ros_capp_gs(void)
{
    return 0;
}

void ros_capp_unwind(const void *to)
{
    (void)to;
}

int ros_capp_where(uint32_t addr, uint32_t *lo, const char **file, const char **name)
{
    (void)addr, (void)lo, (void)file, (void)name;
    return 0;
}

uint32_t ros_capp_mode(void)
{
    return ROS_MODE_USR;
}

uint64_t ros_capp_native_stack(void)
{
    return 0;
}

os_error *ros_capp_stack(uint32_t guard_lo, uint32_t guard_hi, uint32_t emergency_sp)
{
    (void)guard_lo, (void)guard_hi, (void)emergency_sp;
    return ros_error(ROS_ERR_UNIMPLEMENTED, "The hosted build has no 32-bit-pointer code");
}

int ros_capp_exception_handler(uint32_t code)
{
    (void)code;
    return 0;
}

void ros_capp_deliver_exception(uint32_t code, const uint32_t block[17])
{
    (void)code, (void)block;
}

int ros_capp_stack_overflow(uint32_t addr)
{
    (void)addr;
    return 0;
}

void ros_capp_fault(uint32_t sp, uint32_t fpc, uint16_t fpc2, int overflow)
{
    (void)sp, (void)fpc, (void)fpc2, (void)overflow;
}

int ros_capp_gate_fault(uint32_t *sp, uint32_t *ret)
{
    (void)sp, (void)ret;
    return 0;
}

#endif

os_error *ros_capp_run(const char *path, uint32_t line, int *is_capp)
{
    *is_capp = 0;
    struct file f;
    os_error *e = file_open(path, &f);
    if (e)
        return e;
    uint32_t got;
    if ((e = file_read(&f, ros_addr(f.buf), 0, sizeof(struct elf32_ehdr), &got)) ||
        !ros_capp_is_image(f.buf, got)) {
        file_close(&f);
        return e;                               /* not an image, so it is ARM code, as before */
    }
    *is_capp = 1;
    struct image im;
    if ((e = read_headers(path, &f, &im))) {
        file_close(&f);
        return e;
    }
#if !ROS_CAPP_NATIVE
    (void)line;
    file_close(&f);
    return ros_error(ROS_ERR_UNIMPLEMENTED,
                     "'%s' is a C application (a 32-bit x86-64 ELF image), which runs in the box: "
                     "the hosted build has no 32-bit-pointer code", path);
#else
    struct layout l;
    if ((e = image_layout(&im, f.len, &l)) || (e = memory_for(l.hi + 0x2000u))) {
        file_close(&f);
        return e;
    }

    /* StartApplication. The command line is as *Run had it, and the CAO
     * is &8000. */
    uint32_t n = 0;
    while (ros_ld8(line + n) >= ' ' && n < 1000)
        n++;
    uint32_t cmd = rma_string(ros_ptr(line), n), none = rma_string("", 0);
    if (!cmd || !none) {
        file_close(&f);
        if (cmd)
            ros_rma_free(ros_ptr(cmd));
        if (none)
            ros_rma_free(ros_ptr(none));
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    }
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 2, c.r[1] = none, c.r[2] = ROS_APP_BASE, c.r[3] = cmd;
    ros_swi(&c, XOS_FSControl);
    ros_rma_free(ros_ptr(cmd));
    ros_rma_free(ros_ptr(none));
    if (c.v) {
        file_close(&f);
        return ros_ptr(c.r[0]);
    }

    /* The image is read straight into the memory that the application now
     * owns. The file is closed before it runs. */
    for (unsigned i = 0; i < im.h.phnum && !e; i++) {
        const struct elf32_phdr *p = &im.ph[i];
        if (p->type != PT_LOAD || p->memsz == 0)
            continue;
        e = file_read(&f, p->vaddr, p->offset, p->filesz, &got);
        if (!e && got != p->filesz)
            e = ros_error(ROS_ERR_UNIMPLEMENTED, "'%s' is shorter than its program headers say",
                          path);
        if (!e)
            memset(ros_ptr(p->vaddr + p->filesz), 0, p->memsz - p->filesz);
    }
    file_close(&f);
    if (e)
        return e;
    __builtin___clear_cache((char *)ros_ptr(l.lo), (char *)ros_ptr(l.hi));
    return start(&l, path);
#endif
}

/* ---- OS_SynchroniseCodeAreas --------------------------------------------------------- */

/* R0 bit 0 clear means every code area. R0 bit 0 set means [R1, R2],
 * inclusive. Code that the arena holds, such as an application's or the
 * RMA's, is made coherent after it is written. There is nothing to do on
 * x86-64, and on AArch64 the instruction cache is synchronised. The loader
 * does the same for the image it copies. */
static void sync_code(uint32_t lo, uint32_t hi)
{
#ifdef SYNC_CODE_RANGE
    SYNC_CODE_RANGE(lo, hi);            /* the machine's own method (runtime/arch) */
#elif defined(__aarch64__)
    /* The hosted build on AArch64 (an Apple Silicon Mac). The arena's pages
     * that are not there, being reserved with PROT_NONE, fault the
     * instruction cache's maintenance (a bus error on macOS, #68). So, as
     * the box's sync_resident does, only the pages that are there are
     * synchronised. */
    for (uint32_t page = lo & ~0xFFFu; page < hi && page >= (lo & ~0xFFFu); page += 0x1000) {
        char there = 0;
        if (mincore(ros_ptr(page), 4096, (void *)&there) != 0 || !(there & 1))
            continue;
        uint32_t a = page > lo ? page : lo, b = hi - page > 0x1000 ? page + 0x1000 : hi;
        __builtin___clear_cache((char *)ros_ptr(a), (char *)ros_ptr(b));
    }
#else
    if (hi > lo)
        __builtin___clear_cache((char *)ros_ptr(lo), (char *)ros_ptr(hi));
#endif
}

void ros_thunk_OS_SynchroniseCodeAreas(struct ros_cpu *s)
{
    if (s->r[0] & 1) {
        if (s->r[2] >= s->r[1])
            sync_code(s->r[1], s->r[2] + 1);
    } else {
        sync_code(ROS_APP_BASE, ROS_APP_BASE + (ros_slot_current ? ros_slot_current->size : 0));
        sync_code(ROS_RMA_BASE, ROS_RMA_BASE + ROS_RMA_SIZE);
    }
    s->v = 0;
}
