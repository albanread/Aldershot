/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* fault.c -- a processor's exceptions as RISC OS's (fault.h).
 *
 * The signal handler runs on the thread's own signal stack (task.h) and
 * does only what cannot wait. It decides whether the fault is the
 * program's, and what it is, from the signal, the trap number and the
 * registers. It reads at most the instruction at the pc, which was just
 * fetched. It records them. For a division that only overflowed, it
 * finishes the instruction as ARM would and returns to the code.
 * Otherwise it returns into ros_fault_landing and not into the code. The
 * landing runs on the thread's native stack, below the frame that entered
 * the x32 code (or below the runtime's own frame, for a fault in a SWI),
 * and with the runtime's floating point. There, in plain C, the register
 * block is made, the report is printed, and the exception is delivered,
 * either to the program's own handler for it or as the error. That never
 * comes back.
 *
 * A fault while one is being delivered, or one that is not the program's,
 * goes to the fatal report (boot/main.c), which powers off.
 */
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/capp.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/error.h"
#include "rosgd/fault.h"
#include "rosgd/heap.h"
#include "rosgd/platform.h"
#include "rosgd/swi.h"
#include "rosgd/switrace.h"

#if ROS_CAPP_NATIVE

#include <elf.h>
#include <fcntl.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#define ARENA_TOP 0x100000000ull
#define ZP(off) (ROS_ZEROPAGE + (off))
#define EXCEPTION_DUMP ZP(0x958)        /* the ExceptionDump word (Kernel/hdr/KernelWS) */
#define DUMPER         ZP(0xAE8)        /* the kernel's own block (environment.c) */

enum { X32 = 1, RUNTIME };

/* The machine's register set as a signal has it (fault_regs), and what
 * else of the machine the classifier names. */
#if ROS_CAPP_A64
#include "arch/aarch64/fault.h.inc"
#else
#include "arch/x86_64/fault.h.inc"
#endif

/* What the handler found, for the landing. */
struct fault {
    int sig, code, where;
    uint64_t addr;                      /* si_addr */
    fault_regs g;                       /* the registers as the signal had them */
    uint32_t fpc;                       /* the code's floating point control */
    uint16_t fpc2;                      /* capp.h's: MXCSR and x87's on x86-64, FPCR and 0 on AArch64 */
    uint32_t pc;                        /* the address the error gives, which is the instruction's */
    uint32_t errnum;
    const char *kind;
    int overflow;                       /* in the stack's guard */
    int in_gate;                        /* the gate was copying a register block */
    uint32_t gate_sp, gate_ret;
};

static _Thread_local struct fault fault;
static _Thread_local int delivering;
static ros_fault_fatal_fn *fatal;
static unsigned count;
static char last_report[4096];

/* ---- the signal handler's half ------------------------------------------------------- */

/* Whether the program running has an error handler of its own. If it has,
 * a data abort in the ROM's compiled code outside a SWI is RISC OS's
 * "abort on data transfer", raised to that handler as the processor's
 * abort would be. An example is BASIC running a program, which reads past
 * the end of an empty one (#82). With the default handler still in place
 * (the box starting, or a probe), the abort is the fatal report. */
static int program_takes_errors(void)
{
    uint32_t code, r12, buffer;
    ros_env_read(ROS_ENV_ERROR, &code, &r12, &buffer);
    return code && !ros_env_is_default(ROS_ENV_ERROR, code);
}

/* A bus error on a page of application space that is there to be had. The
 * slot pool is used up (arena.c), so the page could not be given. RISC OS
 * has no such fault, because its slot's pages were taken when it grew. So
 * it is the program's data abort, named for what it is, and the box lives
 * on. */
#define OUT_OF_MEMORY "data abort (no memory for the page: the box's memory is used up)"

static int out_of_memory(const struct fault *f)
{
    return f->sig == SIGBUS && f->code == BUS_ADRERR && f->addr >= ROS_APP_BASE &&
           f->addr < ROS_APP_LIMIT && ros_slot_current &&
           f->addr - ROS_APP_BASE < ros_slot_current->size;
}

#if ROS_CAPP_A64
#include "arch/aarch64/fault.c.inc"
#else
#include "arch/x86_64/fault.c.inc"
#endif

/* ---- symbols ---------------------------------------------------------------------------- */

/* /init's functions, from its own symbol table. The table is read once,
 * when first wanted. It gives the function holding a pc, and how far into
 * it the pc is. */
struct nsym {
    uint64_t value, size;
    uint32_t name;
};
static struct nsym *nsyms;
static size_t n_nsyms;
static char *nstrings;
static size_t nstrings_size;
static int native_loaded;
extern const char __ehdr_start[] __attribute__((weak));

static int by_value(const void *a, const void *b)
{
    uint64_t x = ((const struct nsym *)a)->value, y = ((const struct nsym *)b)->value;
    return x < y ? -1 : x > y;
}

static void *read_at(int fd, uint64_t off, uint64_t size)
{
    void *p = size ? malloc(size) : NULL;
    if (p && pread(fd, p, size, (off_t)off) != (ssize_t)size) {
        free(p);
        p = NULL;
    }
    return p;
}

static void load_native(void)
{
    native_loaded = 1;
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    Elf64_Ehdr h;
    Elf64_Shdr *sh = NULL;
    Elf64_Sym *sym = NULL;
    if (pread(fd, &h, sizeof h, 0) != sizeof h || memcmp(h.e_ident, ELFMAG, SELFMAG) ||
        h.e_ident[EI_CLASS] != ELFCLASS64 || h.e_shentsize != sizeof(Elf64_Shdr) || !h.e_shnum)
        goto out;
    sh = read_at(fd, h.e_shoff, (uint64_t)h.e_shnum * sizeof *sh);
    for (unsigned i = 0; sh && i < h.e_shnum && !nsyms; i++) {
        if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_link >= h.e_shnum)
            continue;
        const Elf64_Shdr *st = &sh[sh[i].sh_link];
        size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
        sym = read_at(fd, sh[i].sh_offset, n * sizeof *sym);
        nstrings = read_at(fd, st->sh_offset, st->sh_size);
        nstrings_size = st->sh_size;
        nsyms = sym && nstrings ? malloc(n * sizeof *nsyms) : NULL;
        for (size_t k = 0; nsyms && k < n; k++)
            if (ELF64_ST_TYPE(sym[k].st_info) == STT_FUNC && sym[k].st_value &&
                sym[k].st_name < nstrings_size)
                nsyms[n_nsyms++] = (struct nsym){ sym[k].st_value, sym[k].st_size, sym[k].st_name };
        if (nsyms)
            qsort(nsyms, n_nsyms, sizeof *nsyms, by_value);
    }
out:
    free(sh);
    free(sym);
    close(fd);
}

/* Writes "name+0xoff" for a pc in /init into out. It returns 0 if no
 * function holds the pc. */
static int native_symbol(uint64_t pc, char *out, size_t n)
{
    if (!native_loaded)
        load_native();
    uint64_t base = (uint64_t)(uintptr_t)__ehdr_start, off = pc - base;
    if (!base || !n_nsyms || pc < base)
        return 0;
    size_t lo = 0, hi = n_nsyms;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (nsyms[mid].value <= off)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return 0;
    const struct nsym *s = &nsyms[lo - 1];
    if (s->size && off >= s->value + s->size)
        return 0;
    snprintf(out, n, "%s+0x%llx", nstrings + s->name, (unsigned long long)(off - s->value));
    return 1;
}

/* Writes "name+&off" for addr in an x32 image's file, from its symbol
 * table. The name is the function holding the address, or else the nearest
 * symbol below it. It returns 0 if there is none. */
static int x32_symbol(const char *file, uint32_t addr, char *out, size_t n)
{
    int fd = open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    int found = 0;
    Elf32_Ehdr h;
    Elf32_Shdr *sh = NULL;
    if (pread(fd, &h, sizeof h, 0) != sizeof h || memcmp(h.e_ident, ELFMAG, SELFMAG) ||
        h.e_ident[EI_CLASS] != ELFCLASS32 || h.e_shentsize != sizeof(Elf32_Shdr) || !h.e_shnum ||
        h.e_shnum > 512)
        goto out;
    sh = read_at(fd, h.e_shoff, (uint64_t)h.e_shnum * sizeof *sh);
    for (unsigned i = 0; sh && i < h.e_shnum && !found; i++) {
        if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_link >= h.e_shnum)
            continue;
        /* Find the symbol holding addr, else the nearest one below it. */
        uint32_t in_name = 0, in_value = 0, near_name = 0, near_value = 0;
        int have_in = 0, have_near = 0;
        Elf32_Sym chunk[256];
        uint32_t total = sh[i].sh_size / sizeof(Elf32_Sym);
        for (uint32_t k = 0; k < total; k += 256) {
            uint32_t m = total - k < 256 ? total - k : 256;
            if (pread(fd, chunk, m * sizeof *chunk, (off_t)(sh[i].sh_offset + k * sizeof *chunk)) !=
                (ssize_t)(m * sizeof *chunk))
                break;
            for (uint32_t j = 0; j < m; j++) {
                const Elf32_Sym *s = &chunk[j];
                unsigned t = ELF32_ST_TYPE(s->st_info);
                if (!s->st_name || (t != STT_FUNC && t != STT_NOTYPE) || s->st_value > addr ||
                    s->st_shndx == SHN_UNDEF || s->st_shndx == SHN_ABS)
                    continue;
                if (s->st_size && addr < s->st_value + s->st_size) {
                    if (!have_in || s->st_value > in_value)
                        have_in = 1, in_name = s->st_name, in_value = s->st_value;
                } else if (!have_near || s->st_value > near_value) {
                    have_near = 1, near_name = s->st_name, near_value = s->st_value;
                }
            }
        }
        int have = have_in || have_near;
        uint32_t best_name = have_in ? in_name : near_name;
        uint32_t best_value = have_in ? in_value : near_value;
        if (have) {
            char name[96] = "";
            const Elf32_Shdr *st = &sh[sh[i].sh_link];
            if (best_name < st->sh_size &&
                pread(fd, name, sizeof name - 1, (off_t)(st->sh_offset + best_name)) > 0) {
                name[sizeof name - 1] = 0;
                snprintf(out, n, "%s+&%X", name, addr - best_value);
                found = 1;
            }
        }
    }
out:
    free(sh);
    close(fd);
    return found;
}

/* Says where an x32 address is, as " (faults+&26C3: load+&3)". The text is
 * empty if the address is nowhere. */
static void x32_where(uint32_t addr, char *out, size_t n)
{
    uint32_t lo;
    const char *file, *name;
    out[0] = 0;
    if (!ros_capp_where(addr, &lo, &file, &name))
        return;
    char sym[128] = "";
    if (file && x32_symbol(file, addr, sym, sizeof sym))
        snprintf(out, n, " (%s+&%X: %s)", name, addr - lo, sym);
    else
        snprintf(out, n, " (%s+&%X)", name, addr - lo);
}

/* ---- the landing: the error, the block, the report ------------------------------------------ */

static size_t report_n;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    int k = snprintf(last_report + report_n, sizeof last_report - report_n, "rosgd: exception: %s\n",
                     line);
    if (k > 0)
        report_n = report_n + (size_t)k < sizeof last_report ? report_n + (size_t)k
                                                             : sizeof last_report - 1;
    fprintf(stderr, "rosgd: exception: %s\n", line);
}

static int say_swi(const char *line, void *ctx)
{
    (void)ctx;
    say("    %s", line);
    return 0;
}

static const char *mode_name(uint32_t psr)
{
    switch (psr & 0x1F) {
    case ROS_MODE_USR:
        return "USR";
    case ROS_MODE_SVC:
        return "SVC";
    default:
        return "?";
    }
}

/* The register block (fault.h). This defines block(), the machine's names
 * for its registers (dump_name) and its line in the report (SAY_MACHINE). */
#if ROS_CAPP_A64
#include "arch/aarch64/faultblock.c.inc"
#else
#include "arch/x86_64/faultblock.c.inc"
#endif

/* Writes the register block into the block that OS_ChangeEnvironment 13
 * names. If that is not memory it may write, the block goes into the
 * kernel's own, which then becomes the one named. */
static void write_block(const uint32_t r[17])
{
    uint32_t at = ros_ld32(EXCEPTION_DUMP);
    if ((at & 3) || !ros_arena_valid(at, at + 17 * 4)) {
        at = DUMPER;
        ros_st32(EXCEPTION_DUMP, at);
    }
    for (unsigned i = 0; i < 17; i++)
        ros_st32(at + 4 * i, r[i]);
}

static const char *const env_name[] = { [ROS_ENV_UNDEFINED] = "undefined instruction",
                                        [ROS_ENV_PREFETCH_ABORT] = "prefetch abort",
                                        [ROS_ENV_DATA_ABORT] = "data abort" };

/* own is 1 if the program's handler takes the fault, -1 if the program has
 * a handler that cannot, and 0 if it has none. */
static void report(const struct fault *f, const uint32_t r[17], const os_error *e, int env,
                   uint32_t handler, int own)
{
    report_n = 0;
    last_report[0] = 0;
    uint32_t task = ros_ld32(ROS_ZP_DOMAINID);
    char where[200], addr[40] = "";
    if (f->sig == SIGSEGV || (f->sig == SIGBUS && f->code != BUS_ADRALN)) {
        if (f->addr < ARENA_TOP && f->code != SI_KERNEL)
            snprintf(addr, sizeof addr, ", address &%08X", (unsigned)f->addr);
        else if (f->code != SI_KERNEL)
            snprintf(addr, sizeof addr, ", address %#llx", (unsigned long long)f->addr);
    }
    if (f->where == X32) {
        x32_where(f->pc, where, sizeof where);
        uint32_t lo;
        const char *file, *name;
        int kind = ros_capp_where(f->pc, &lo, &file, &name);
        say("%s in %s at &%08X%s%s, task &%X", f->kind,
            kind == ROS_CAPP_TASK      ? "the application"
            : kind == ROS_CAPP_LIBRARY ? "the library"
            : kind == ROS_CAPP_MODULE  ? "a module"
                                       : ROS_CAPP_ABI " code",
            f->pc, where, addr, task);
    } else {
        uint64_t rip = FAULT_NATIVE_PC(f), base = (uint64_t)(uintptr_t)__ehdr_start;
        char sym[160] = "", in[64] = "";
        if (base && rip >= base)
            snprintf(in, sizeof in, "/init+%#llx", (unsigned long long)(rip - base));
        if (native_symbol(rip, sym, sizeof sym))
            snprintf(where, sizeof where, " (%s: %s)", in, sym);
        else
            snprintf(where, sizeof where, " (%s)", in);
        say("%s in the runtime at %#llx%s%s, task &%X", f->kind, (unsigned long long)rip, where,
            addr, task);
        char from[200];
        x32_where(r[14], from, sizeof from);
        if (f->in_gate) {
            say("  the SWI gate, copying the program's registers; its call returns to &%08X%s",
                r[14], from);
        } else {
            /* The innermost SWI traced. There is none if tracing is off. */
            struct ros_swi_frame *in_swi = ros_switrace_top();
            const char *n1 = in_swi ? ros_swi_name(in_swi->number) : NULL;
            if (in_swi)
                say("  in SWI %s (&%X); the program called the OS with R14 &%08X%s",
                    n1 ? n1 : "?", in_swi->number, r[14], from);
            else
                say("  in a SWI (not traced); the program called the OS with R14 &%08X%s", r[14],
                    from);
        }
    }
    say("  %s (error &%X)", e->errmess, e->errnum);
    if (own > 0)
        say("  to the program's %s handler at &%08X", env_name[env], handler);
    else if (own < 0)
        say("  its %s handler at &%08X is not the program's x32 code: raised as the error",
            env_name[env], handler);
    else
        say("  raised as the error");
    const char *label = f->where == X32 ? "" : "  (the program's, as it called the OS)";
    for (unsigned row = 0; row < 4; row++) {
        char line[200];
        int k = snprintf(line, sizeof line, " ");
        for (unsigned c = 0; c < 4; c++) {
            unsigned i = row * 4 + c;
            k += snprintf(line + k, sizeof line - (size_t)k, " R%-2u &%08X", i, r[i]);
        }
        if (f->where == X32)
            snprintf(line + k, sizeof line - (size_t)k, "   %s %s %s %s", dump_name[row * 4],
                     dump_name[row * 4 + 1], dump_name[row * 4 + 2], dump_name[row * 4 + 3]);
        else if (row == 0)
            snprintf(line + k, sizeof line - (size_t)k, "%s", label);
        say("%s", line);
    }
    if (f->where == X32)
        SAY_MACHINE(f, r);
    else {
        say("  PSR &%08X (%s); the last SWIs:", r[16], mode_name(r[16]));
        ros_switrace_recent(8, say_swi, NULL);
    }
    fflush(stderr);
}

void ros_fault_land_c(void)
{
    struct fault *f = &fault;
    uint32_t r[17];
    block(f, r);
    if (f->where == RUNTIME)
        f->pc = r[15];
    const os_error *e;
    switch (f->errnum) {
    case ROS_ERR_UNDEFINED_INSTRUCTION:
        e = ros_error(f->errnum, "Internal error: undefined instruction at &%08X", f->pc);
        break;
    case ROS_ERR_INSTRUCTION_ABORT:
        e = ros_error(f->errnum, "Internal error: abort on instruction fetch at &%08X", f->pc);
        break;
    case ROS_ERR_DATA_ABORT:
        e = ros_error(f->errnum, "Internal error: abort on data transfer at &%08X", f->pc);
        break;
    case ROS_ERR_DIVIDE_BY_ZERO:
        e = ros_error(f->errnum, "Divide by zero");
        break;
    case ROS_ERR_STACK_OVERFLOW:
        e = ros_error(f->errnum, "Not enough memory, stack overflow");
        break;
    default:
        e = ros_error(f->errnum, "Floating point exception: %s",
                      fp_kind[f->errnum - ROS_ERR_FP_BASE < 5 ? f->errnum - ROS_ERR_FP_BASE : 0]);
        break;
    }
    /* The three aborts go to the environment's handler for them, if the
     * program has one of its own that can take it (fault.h). */
    int env = f->errnum == ROS_ERR_UNDEFINED_INSTRUCTION ? ROS_ENV_UNDEFINED
              : f->errnum == ROS_ERR_INSTRUCTION_ABORT   ? ROS_ENV_PREFETCH_ABORT
              : f->errnum == ROS_ERR_DATA_ABORT          ? ROS_ENV_DATA_ABORT
                                                         : -1;
    uint32_t handler = 0, unused1, unused2;
    int own = 0;
    if (env >= 0) {
        ros_env_read((uint32_t)env, &handler, &unused1, &unused2);
        if (handler && !ros_env_is_default((uint32_t)env, handler))
            own = ros_capp_exception_handler(handler) ? 1 : -1;
    }
    report(f, r, e, env, handler, own);
    if (f->where == X32 && FAULT_TO_PROGRAM(f))
        ros_capp_fault((uint32_t)FAULT_SP(f), f->fpc, f->fpc2, f->overflow);
    count++;
    delivering = 0;
    if (own > 0)
        ros_capp_deliver_exception(handler, r);     /* does not return */
    write_block(r);
    ros_raise_at(e, f->pc);
}

void ros_fault_init(ros_fault_fatal_fn *fn)
{
    fatal = fn;
    struct sigaction sa = { .sa_sigaction = ros_fault_signal, .sa_flags = SA_SIGINFO | SA_ONSTACK };
    sigemptyset(&sa.sa_mask);
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP };
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        sigaction(sigs[i], &sa, NULL);
}

#else

/* Hosted: the host's signals are left as they are. */
void ros_fault_init(ros_fault_fatal_fn *fn)
{
    (void)fn;
}

static unsigned count;
static char last_report[1];

#endif

_Thread_local struct ros_fault_escape *ros_fault_escape;
int (*ros_fault_jit)(int sig, siginfo_t *si, void *context);

unsigned ros_fault_count(void)
{
    return count;
}

const char *ros_fault_last_report(void)
{
    return last_report;
}
