/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
// armscan.cpp: the ARM container's corpus tool. It runs on the Mac with the
// engine (armrun.h over dynarmic).
//
//   armscan unsqueeze FILE OUT
//       An AIF image as it is in memory once it has started: a squeezed
//       one (word 0 a BL to its decompressor, not a NOP) is loaded at &8000
//       and its own decompressor run under the engine until it returns to
//       &8004. The image (header to the end of its read-write data) is
//       written to OUT. One line of JSON goes to stdout: what was found, the
//       SWIs the decompressor called, and the instructions it ran. An image
//       that is not squeezed is copied as it is.
//
//   armscan exec run|step
//       One block of A32 code run as tests/armrun/idioms.py frames it. It is
//       read from stdin as "BASE <hex>", then "WORDS <hex> ..." (the whole
//       block, code and data, at BASE), then "ENTRY <hex>". It is entered
//       with r13 a stack of its own and r14 a return address that halts. It
//       is run as blocks (run) or one instruction at a time (step). The
//       output is "HALT <why>" and "BLOCK <hex> ..." (the block's words
//       afterwards). A memory access outside the block and the stack halts it
//       ("abort &addr"), as a data abort would end the case on RISC OS.
//
//   armscan probe
//       Instruction words are read from stdin, one hex word a line. For each,
//       a line goes to stdout with the word and what the engine makes of it
//       when run as one instruction. That is "ok", or the exception it raises
//       (undefined, unpredictable, decode-error, fallback, ...). An assertion
//       in the engine ends the process. tools/corpus_scan.py restarts it after
//       the word it was given, and records that word as "abort".
//
// Guest memory is a 4 GB reservation. Guest address a is base + a, as in
// the box. Nothing here is the box's runtime. The SWIs that a decompressor
// makes are answered here (OS_SynchroniseCodeAreas, OS_GetEnv).

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "armrun.h"

namespace {

uint8_t *base;

const char *kind_name(int k);

constexpr uint32_t LOAD = 0x8000;
constexpr uint32_t ROOM = 64u << 20;       // mapped above &8000: image, its expansion, a stack
constexpr uint32_t PROBE_PC = 0x10000000;  // probe slots: one word each, from here
constexpr uint32_t MARK = 0xEFFFFFFFu;     // SVC &FFFFFF: the decompressor has returned

uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, base + a, 4); return v; }

bool mapped(uint32_t a, unsigned size) { return a >= LOAD && a + size <= LOAD + ROOM; }

// ---- unsqueeze ------------------------------------------------------------

struct Unsqueeze {
    std::vector<uint32_t> svcs;
    bool returned = false, failed = false, synced = false;
    std::string why;
} uq;

void uq_svc(void *, armrun *a, uint32_t n)
{
    uint32_t *r = armrun_regs(a);
    uint32_t swi = n & ~0x20000u;
    if (uq.svcs.size() < 64)
        uq.svcs.push_back(n);
    if (n == (MARK & 0xFFFFFF)) {
        uq.returned = true;
        armrun_halt(a);
    } else if (swi == 0x6E) {                       // OS_SynchroniseCodeAreas
        uq.synced = true;
        armrun_clear(a);
    } else if (swi == 0x10) {                       // OS_GetEnv
        r[0] = LOAD + ROOM - 256, r[1] = LOAD + ROOM, r[2] = LOAD + ROOM - 16;
    } else {
        uq.failed = true;
        char b[64];
        snprintf(b, sizeof b, "SWI &%X", n);
        uq.why = b;
        armrun_halt(a);
    }
}

void uq_exception(void *, armrun *a, uint32_t pc, int kind)
{
    uq.failed = true;
    char b[64];
    snprintf(b, sizeof b, "exception %d at &%X", kind, pc);
    uq.why = b;
    armrun_halt(a);
}

uint64_t uq_read(void *, armrun *a, uint32_t addr, unsigned size)
{
    uint64_t v = 0;
    if (mapped(addr, size)) {
        memcpy(&v, base + addr, size);
        return v;
    }
    uq.failed = true;
    char b[64];
    snprintf(b, sizeof b, "read &%X", addr);
    uq.why = b;
    armrun_halt(a);
    return 0;
}

void uq_write(void *, armrun *a, uint32_t addr, unsigned size, uint64_t v)
{
    if (mapped(addr, size)) {
        memcpy(base + addr, &v, size);
        return;
    }
    uq.failed = true;
    char b[64];
    snprintf(b, sizeof b, "write &%X", addr);
    uq.why = b;
    armrun_halt(a);
}

int uq_fetch(void *, armrun *, uint32_t addr, uint32_t *w)
{
    if (addr == LOAD + 4) {                         // the BL at &8000 has returned
        *w = MARK;
        return 1;
    }
    if (!mapped(addr, 4))
        return 0;
    *w = rd32(addr);
    return 1;
}

int unsqueeze(const char *in, const char *out)
{
    FILE *f = fopen(in, "rb");
    if (!f) {
        perror(in);
        return 2;
    }
    std::vector<uint8_t> file;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        file.insert(file.end(), buf, buf + n);
    fclose(f);
    if (file.size() < 0x80 || file.size() > ROOM / 2) {
        printf("{\"file\":\"%s\",\"error\":\"size %zu\"}\n", in, file.size());
        return 1;
    }
    memcpy(base + LOAD, file.data(), file.size());
    const bool aif = rd32(LOAD + 0x10) == 0xEF000011u;
    const bool squeezed = aif && rd32(LOAD) != 0xE1A00000u;
    if (squeezed) {
        static const armrun_hooks hooks = { nullptr, uq_svc, uq_exception, uq_read, uq_write, uq_fetch };
        armrun *a = armrun_create((uintptr_t)base, 0, &hooks);
        uint32_t *r = armrun_regs(a);
        for (int i = 0; i < 16; i++)
            r[i] = 0;
        r[13] = LOAD + ROOM - 0x1000;               // a stack at the top
        r[15] = LOAD;
        armrun_set_cpsr(a, 0x10);
        // in slices, so a decompressor that never returns is stopped
        for (int slice = 0; slice < 400 && !uq.returned && !uq.failed; slice++) {
            armrun_run(a, 1000000);
        }
        armrun_destroy(a);
        if (!uq.returned && !uq.failed)
            uq.failed = true, uq.why = "did not return";
    }
    // the image as it now stands: header to the end of read-write data
    uint32_t ro = rd32(LOAD + 0x14), rw = rd32(LOAD + 0x18), zi = rd32(LOAD + 0x20);
    uint32_t size = aif ? ro + rw : (uint32_t)file.size();
    if (size > ROOM / 2 || size < 0x80)
        size = (uint32_t)file.size();
    if (!uq.failed) {
        FILE *o = fopen(out, "wb");
        if (!o) {
            perror(out);
            return 2;
        }
        fwrite(base + LOAD, 1, size, o);
        fclose(o);
    }
    printf("{\"file\":\"%s\",\"aif\":%s,\"squeezed\":%s,\"ok\":%s,\"why\":\"%s\",\"ro\":%u,\"rw\":%u,"
           "\"zi\":%u,\"mode\":%u,\"entry\":%u,\"size\":%u,\"filesize\":%zu,\"synced\":%s,\"svcs\":[",
           in, aif ? "true" : "false", squeezed ? "true" : "false", uq.failed ? "false" : "true",
           uq.why.c_str(), ro, rw, zi, rd32(LOAD + 0x30), rd32(LOAD + 0x0C), size, file.size(),
           uq.synced ? "true" : "false");
    for (size_t i = 0; i < uq.svcs.size(); i++)
        printf("%s%u", i ? "," : "", uq.svcs[i]);
    printf("]}\n");
    return uq.failed ? 1 : 0;
}

// ---- exec -----------------------------------------------------------------

constexpr uint32_t EXEC_RETURN = 0x7FFF0000;    // r14 on entry: fetching it halts
constexpr uint32_t EXEC_STACK = 0x7FF00000;     // a 64 KB stack below this
uint32_t ex_base, ex_len;
std::string ex_why;
bool ex_done;

bool ex_ours(uint32_t a, unsigned size)
{
    return (a >= ex_base && a + size <= ex_base + ex_len) ||
           (a >= EXEC_STACK - 0x10000 && a + size <= EXEC_STACK);
}

void ex_svc(void *, armrun *a, uint32_t n)
{
    if (n == (MARK & 0xFFFFFF)) {
        ex_why = "returned";
    } else {
        char b[32];
        snprintf(b, sizeof b, "svc &%X", n);
        ex_why = b;
    }
    ex_done = true;
    armrun_halt(a);
}

void ex_exception(void *, armrun *a, uint32_t pc, int kind)
{
    char b[64];
    snprintf(b, sizeof b, "%s &%X", kind_name(kind), pc);
    ex_why = b;
    ex_done = true;
    armrun_halt(a);
}

uint64_t ex_read(void *, armrun *a, uint32_t addr, unsigned size)
{
    uint64_t v = 0;
    if (ex_ours(addr, size)) {
        memcpy(&v, base + addr, size);
        return v;
    }
    char b[32];
    snprintf(b, sizeof b, "abort &%X", addr);
    ex_why = b;
    ex_done = true;
    armrun_halt(a);
    return 0;
}

void ex_write(void *, armrun *a, uint32_t addr, unsigned size, uint64_t v)
{
    if (ex_ours(addr, size)) {
        memcpy(base + addr, &v, size);
        return;
    }
    char b[32];
    snprintf(b, sizeof b, "abort &%X", addr);
    ex_why = b;
    ex_done = true;
    armrun_halt(a);
}

int ex_fetch(void *, armrun *, uint32_t addr, uint32_t *w)
{
    if (addr == EXEC_RETURN) {
        *w = MARK;
        return 1;
    }
    if (!ex_ours(addr, 4))
        return 0;
    memcpy(w, base + addr, 4);
    return 1;
}

int exec(bool step)
{
    char tag[16];
    uint32_t entry = 0;
    std::vector<uint32_t> block;
    while (scanf("%15s", tag) == 1) {
        if (!strcmp(tag, "BASE")) {
            scanf("%x", &ex_base);
        } else if (!strcmp(tag, "ENTRY")) {
            scanf("%x", &entry);
        } else if (!strcmp(tag, "WORDS")) {
            uint32_t w;
            while (scanf("%x", &w) == 1)
                block.push_back(w);
            clearerr(stdin);
        }
    }
    ex_len = 4 * (uint32_t)block.size();
    const uint32_t pg = (uint32_t)getpagesize();     // 16 KB on Apple silicon
    if (mprotect(base + (ex_base & ~(pg - 1)), ((ex_base & (pg - 1)) + ex_len + pg - 1) & ~(pg - 1), PROT_READ | PROT_WRITE) ||
        mprotect(base + EXEC_STACK - 0x10000, 0x10000, PROT_READ | PROT_WRITE)) {
        perror("armscan exec: memory");
        return 2;
    }
    memcpy(base + ex_base, block.data(), ex_len);
    static const armrun_hooks hooks = { nullptr, ex_svc, ex_exception, ex_read, ex_write, ex_fetch };
    armrun *a = armrun_create((uintptr_t)base, 0, &hooks);
    uint32_t *r = armrun_regs(a);
    for (int i = 0; i < 16; i++)
        r[i] = 0;
    r[13] = EXEC_STACK, r[14] = EXEC_RETURN, r[15] = entry;
    armrun_set_cpsr(a, 0x110);                      // USR, A set: RISC OS 5.30's user mode reads &110
    for (int i = 0; i < 100000 && !ex_done; i++) {
        if (step)
            armrun_step(a);
        else
            armrun_run(a, 100000);
    }
    printf("HALT %s\n", ex_done ? ex_why.c_str() : "ran on");
    printf("BLOCK");
    for (uint32_t i = 0; i < ex_len / 4; i++) {
        uint32_t w;
        memcpy(&w, base + ex_base + 4 * i, 4);
        printf(" %08X", w);
    }
    printf("\n");
    armrun_destroy(a);
    return 0;
}

// ---- probe ----------------------------------------------------------------

std::vector<uint32_t> words;
int raised;

void pr_svc(void *, armrun *, uint32_t) {}
void pr_exception(void *, armrun *, uint32_t, int kind) { raised = kind; }
uint64_t pr_read(void *, armrun *, uint32_t, unsigned) { return 0; }
void pr_write(void *, armrun *, uint32_t, unsigned, uint64_t) {}
int pr_fetch(void *, armrun *, uint32_t addr, uint32_t *w)
{
    uint32_t i = (addr - PROBE_PC) / 4;
    *w = addr >= PROBE_PC && i < words.size() ? words[i] : 0xE1A00000u;   // else NOP
    return 1;
}

const char *kind_name(int k)
{
    static const char *names[] = { "undefined", "unpredictable", "decode-error", "sev", "sevl", "wfi", "wfe",
                                   "yield", "breakpoint", "pld", "pldw", "pli", "no-execute", "interpret" };
    return k >= 0 && k <= ARMRUN_INTERPRET ? names[k] : "exception";
}

int probe()
{
    static const armrun_hooks hooks = { nullptr, pr_svc, pr_exception, pr_read, pr_write, pr_fetch };
    armrun *a = armrun_create((uintptr_t)base, 64u << 20, &hooks);
    char line[64];
    while (fgets(line, sizeof line, stdin)) {
        uint32_t w = (uint32_t)strtoul(line, nullptr, 16);
        words.push_back(w);
        printf("%08X ", w);
        fflush(stdout);                             // the word is named before the engine sees it
        uint32_t *r = armrun_regs(a);
        for (int i = 0; i < 15; i++)
            r[i] = 0x20000000u + 0x1000u * i;       // pointers to nowhere: every access a hook
        r[15] = PROBE_PC + 4 * (uint32_t)(words.size() - 1);
        armrun_set_cpsr(a, 0x10);
        raised = -1;
        armrun_step(a);
        printf("%s\n", raised < 0 ? "ok" : kind_name(raised));
        fflush(stdout);
    }
    armrun_destroy(a);
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    base = (uint8_t *)mmap(nullptr, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED || mprotect(base + LOAD, ROOM, PROT_READ | PROT_WRITE)) {
        perror("armscan: guest memory");
        return 2;
    }
    if (argc == 4 && !strcmp(argv[1], "unsqueeze"))
        return unsqueeze(argv[2], argv[3]);
    if (argc == 2 && !strcmp(argv[1], "probe"))
        return probe();
    if (argc == 3 && !strcmp(argv[1], "exec"))
        return exec(!strcmp(argv[2], "step"));
    fprintf(stderr, "usage: armscan unsqueeze FILE OUT | armscan probe | armscan exec run|step\n");
    return 2;
}
