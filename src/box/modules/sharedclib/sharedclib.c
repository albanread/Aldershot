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
 * This file is a reimplementation in C of RISC OS Open's SharedCLibrary module
 * (Sources/Lib/RISC_OSLib: clib/s/cl_rmhdr, s/initmodule, s/h_brazil).
 */

/* sharedclib.c: SharedCLibrary (RISC_OSLib 6.23, 15 May 2024, the version in
 * RISC OS 5.30), reimplemented natively. This is the module that sits in
 * front of the ROM C library.
 *
 * The library itself is one x32 image. It is linked by `roscc link
 * --clib-image` at &72200000 in the C ROM (include/rosgd/clibimage.h). It
 * is carried in /init as the ROM carries it. In the box that is
 * $(GEN)/clib_rom.h, which is the library that `make capps` builds. The
 * module's first initialisation maps the image there, read-only and
 * executable. It checks the image's header and registers the image as x32
 * library code (ros_capp_code_add). A handler in that code runs with its R12
 * as the static base. The image stays mapped, as ROM does, whatever becomes
 * of the module. The hosted build has no 32-bit-pointer code, so it has no
 * image.
 *
 * The module is RISC_OSLib's (clib/s/cl_rmhdr and s/initmodule). It matches
 * RISC OS 5.30 to the letter wherever a client can see it. That covers its
 * title and help, its SWI chunk and names, and its errors' numbers and
 * texts. The error texts are the CLib messages (Resources:$.Resources.CLib).
 * They are looked up through SharedCLibrary$Path, as _kernel_copyerror looks
 * them up, with %0 as the title. The module also sets two path variables
 * when they are unset. tests/capps/farm/sclib.txt records all of this on
 * 5.30. The SWIs are:
 *
 *   &80680 LibInitAPCS_A         refused: C71 (&800E86)
 *   &80681 LibInitAPCS_R         refused: C72 (&800E86)
 *   &80682 LibInitModule         refused: C72, as 5.30's 32-bit build does
 *   &80683 LibInitAPCS_32        registers an application
 *   &80684 LibInitModuleAPCS_32  registers a module
 *   &80685-&806BF                BadSWI (&800E85), %0 SharedCLibrary
 *
 * Registration follows _Shared_Lib_Module_SWI_Code step for step and error
 * for error. The ARM code patches the client, and ROSGD does that its own
 * way instead. The steps are these.
 *
 *   - The workspace. For an application, R1 plus the client's statics
 *     (R5 - R4, or none if that is negative) is the base of the root stack.
 *     The stack is R6 >> 16 K above that base, or 16K if that is 0. For a
 *     module the stack is the SVC stack. Its base is R13 rounded down to
 *     1 MB, and the statics end the workspace used. If the workspace
 *     passes R2 (a signed comparison, as the ARM's GT), the error is C01.
 *   - The client's own statics. With R5 >= R4, the bytes [R4, R3) are
 *     copied to R1 and the client offset is R1 - R4. With R5 < R4 there is
 *     nothing to copy and the offset is 0. This is cl_stub's "no copying"
 *     case, where R4 = 0 and R5 = -1. Then R5 - R3 bytes are zeroed after
 *     what was copied, if that is positive.
 *   - Each stub chunk is {id, entries, entries end, statics, statics end}.
 *     The list ends at an id below 0. The chunk's id must be one that the
 *     library has, or the error is C02. A chunk that the library holds for
 *     ROM clients only also gives C02, and ROSGD's library has no such
 *     chunk. The chunk may have no more entries than the library's, or
 *     the error is C63 (OldSharedLibrary, raised at registration as RISC OS
 *     raises it). Then the chunk's address table is filled. A slot is 8
 *     bytes of fixed code, `jmp *(size-6)(%rip)`. It jumps through the
 *     table that follows the slots. That table has 8 bytes an entry, the
 *     entry's address zero-extended (roscc's roclib_x32.s). The slots are
 *     never written, so no code needs synchronising. A chunk that is not
 *     a whole number of slots gives C02, as RISC OS gives C02 for a table
 *     it cannot reach. Then the chunk's statics are handled. They must be
 *     the same size as the library's template, or the error is C04. They
 *     are copied from the template to the stub's statics plus the client
 *     offset. The offset from template to copy must be the same for every
 *     chunk, or the error is C05.
 *   - Then comes ROSGD's counterpart of setting [sl, #SL_Lib_Offset]. The
 *     client's block has a thread pointer (TP) at its top. The TP is the
 *     templates' start plus that offset plus the image's TP offset. The
 *     block has a TP only if the client registered the chunk whose template
 *     ends the block. Otherwise the client has no TP and its base is left
 *     alone. The block holds its own address (the self word) at TP+0 and the
 *     stack limit, which is the stack's base, at TP+4. TP then becomes the
 *     static base (%gs) that the library reaches its statics through. For
 *     an application this is the task's base (ros_capp_task_base), which
 *     is recorded for this thread and for the program. For a module it is
 *     the base for the module's code range (ros_capp_module_base), so that
 *     every later entry into the module has it. A "module" that registers
 *     from an application's own code gets the task's base.
 *   - On return, R0 is the workspace's end (R2). R1 and R2 are the stack's
 *     base and top. R6 is the version (6). R3 to R5 are as they were. An
 *     error returns the error in R0. From C02 onwards it also returns R1
 *     and R2 as success would, because 5.30 has stored them before it looks
 *     at a chunk. For C01, R6 is the size of the root stack in bytes, as in
 *     5.30. After that R6 is as it came in, where RISC OS leaves a working
 *     value there.
 *
 * Some things that RISC OS writes are not written by ROSGD. They are the
 * root stack chunk's header (SC_size and the two offsets below sl) and the
 * ClientFlags byte in the kernel statics. These are ARM details, and the
 * x32 kernel layer defines its own. ROSGD also has no routine variants (an
 * entry with bit 0 set, PickRoutineVariant), because roscc's image never
 * has them. RISC OS would abort on a bad address in the stub's tables.
 * ROSGD refuses with &411 "No writable memory at this address" before it
 * writes anything there.
 *
 * Entering the library is not the module's business. The stubs and the
 * library do it. That covers _kernel_init, _clib_initialise, _main and a
 * module's _kernel_moduleinit.
 */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/capp.h"
#include "rosgd/clibimage.h"
#include "rosgd/cpu.h"
#include "rosgd/heap.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "sharedclib.h"

#if ROS_CAPP_NATIVE
#include "clib_rom.h"                   /* ros_clib_rom: the image that make capps builds (the box) */
#endif

/* RISC_OSLib's errors (s/h_brazil). The comments give their tokens in the
 * CLib messages. */
#define E_BAD_MEMORY     0x800E80u      /* C01 */
#define E_UNKNOWN_LIB    0x800E81u      /* C02 */
#define E_STATIC_SIZE    0x800E83u      /* C04 */
#define E_STATIC_OFFSET  0x800E84u      /* C05 */
#define E_UNKNOWN_SWI    0x800E85u      /* BadSWI, from the Global messages */
#define E_OLD_APCS       0x800E86u      /* C71 (APCS-A), C72 (APCS-R) */
#define E_OLD_LIBRARY    0x800E91u      /* C63 */

#define OLD_ROOT_STACK   16384u         /* s/h_stack, OldRootStackSize */
#define ROM_ONLY         0x80000000u    /* s/h_modmacro, library_segment_is_ROM_only */
#define SLOT_SIZE        8u             /* roclib_x32.s: jmp *(size-6)(%rip), int3, int3 */
#define MAX_CHUNKS       16u

/* The contract major version this module registers clients for
 * (roscc/tools/clibspec/clib-contract.json) */
#define CONTRACT_MAJOR   1u

/* ---- the workspace ------------------------------------------------------------ */

struct workspace {
    char filename[32];
    char title[16];                     /* %0 in its errors */
    uint32_t error[16];                 /* an error block to look up: number, token */
};

static struct workspace *ws(void)
{
    uint32_t pw = sharedclib_module.private_word;
    uint32_t w = pw ? ros_ld32(pw) : 0;
    return w ? ros_ptr(w) : NULL;
}

/* Call a SWI by number. The registers go in and come out, and V is the
 * result. */
static os_error *swi(uint32_t n, uint32_t r[10])
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 10 * sizeof r[0]);
    ros_swi(&c, n | ROS_X_BIT);
    memcpy(r, c.r, 10 * sizeof r[0]);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

/* The CLib messages' descriptor, as k_body's open_messagefile keeps it.
 * It is kept in the kernel's CLibWord (OS_ReadSysInfo 6, 80). The module
 * and every client's library code share it, and whichever needs it first
 * opens it. It is a 16-byte RMA block with "SharedCLibrary:Messages" opened
 * into it. Returns 0 if the file cannot be opened. */
static uint32_t messages(void)
{
    uint32_t d = ros_ld32(ROS_ZP_CLIBWORD);
    if (d)
        return d;
    uint32_t r[10] = { 6, 0, 0, 16 };
    if (swi(XOS_Module, r))
        return 0;
    d = r[2];
    ros_st32(ROS_ZP_CLIBWORD, d);
    struct workspace *w = ws();
    uint32_t o[10] = { d, w ? ros_addr(w->filename) : 0, 0 };
    if (!w || swi(XMessageTrans_OpenFile, o)) {
        uint32_t f[10] = { 7, 0, d };
        swi(XOS_Module, f);
        ros_st32(ROS_ZP_CLIBWORD, 0);
        return 0;
    }
    return d;
}

/* One of the module's errors, as _kernel_copyerror makes it. It calls
 * MessageTrans_ErrorLookup on the token in "SharedCLibrary:Messages" (the
 * descriptor that CLibWord keeps), with %0 as the title. If the file cannot
 * be opened, the lookup goes to the Global messages alone (R1 = 0), as it
 * does in 5.30. */
static os_error *error(uint32_t errnum, const char *token)
{
    struct workspace *w = ws();
    if (!w)
        return ros_error(errnum, "%s", token);
    w->error[0] = errnum;
    snprintf((char *)&w->error[1], sizeof w->error - 4, "%s", token);
    uint32_t r[10] = { ros_addr(w->error), messages(), 0, 0, ros_addr(w->title) };
    return swi(XMessageTrans_ErrorLookup, r);
}

/* ---- the image --------------------------------------------------------------- */

static uint32_t rd(const unsigned char *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* Find the image's .rosclib header in the ELF image at base. It follows the
 * PT_NOTE (its offset plus its size), rounded up to a multiple of 4.
 * Returns 0 if there is none. */
static uint32_t header_in(const unsigned char *img, uint32_t size)
{
    if (size < 52)
        return 0;
    uint32_t phoff = rd(img + 28), phnum = img[44] | (uint32_t)img[45] << 8;
    for (uint32_t i = 0; i < phnum && phoff + 32 * (i + 1) <= size; i++) {
        const unsigned char *ph = img + phoff + 32 * i;
        if (rd(ph) == 4)                                /* PT_NOTE */
            return (rd(ph + 4) + rd(ph + 16) + 3) & ~3u;
    }
    return 0;
}

uint32_t ros_sharedclib_image(void)
{
    if (ros_capp_code(ROS_CROM_BASE) != ROS_CAPP_LIBRARY)
        return 0;
    uint32_t h = header_in(ros_ptr(ROS_CROM_BASE), 4096);
    return h ? ROS_CROM_BASE + h : 0;
}

/* The chunk records that follow the header */
static struct ros_clib_chunk chunk_at(uint32_t header, uint32_t i)
{
    struct ros_clib_chunk c;
    memcpy(&c, ros_ptr(header + sizeof(struct ros_clib_header) + i * sizeof c), sizeof c);
    return c;
}

/* The templates' start, which is the lowest template address of any chunk
 * that has statics. The templates tile the block below TP in region order.
 * This is checked when the image is mapped. */
static uint32_t templates(uint32_t header, const struct ros_clib_header *h)
{
    uint32_t lo = 0;
    for (uint32_t i = 0; i < h->chunks; i++) {
        struct ros_clib_chunk c = chunk_at(header, i);
        if (c.data_end > c.data && (!lo || c.data < lo))
            lo = c.data;
    }
    return lo;
}

#if ROS_CAPP_NATIVE

/* Check the image as roscc writes it, before a byte of it is used. It must
 * be an ELF32 image for this box's machine (x86-64, or AArch64 for A64X32)
 * with OS/ABI 255. It must have one read-only, executable PT_LOAD that runs
 * from the file's start at &72200000. The symbol table and section headers
 * after it belong to the file and not to the image. It must have the ROSGD
 * note with the library flag. Then the header must have the contract's
 * major version that this module knows, and the PT_LOAD's size. Every
 * chunk's entries and template must be inside the image. The templates
 * must tile the block below TP, and the block's header must be 16 bytes
 * above it. On success, *size is what is mapped. */
static const char *image_fault(const unsigned char *img, uint32_t file, uint32_t *size)
{
    uint32_t n = 0;
    if (file < 52 || memcmp(img, "\177ELF\1\1\1\377", 8) || (img[16] | img[17] << 8) != 2 ||
        (img[18] | img[19] << 8) != ROS_CAPP_EM)
        return "not an ELF32 " ROS_CAPP_MACHINE " image for ROSGD";
    uint32_t phoff = rd(img + 28), phnum = img[44] | (uint32_t)img[45] << 8, loads = 0, note = 0;
    for (uint32_t i = 0; i < phnum; i++) {
        if (phoff + 32 * (i + 1) > file)
            return "its program headers are not in it";
        const unsigned char *ph = img + phoff + 32 * i;
        if (rd(ph) == 1) {
            loads++;
            n = rd(ph + 16);
            if (rd(ph + 4) != 0 || rd(ph + 8) != ROS_CROM_BASE || n > file || n < 52 || rd(ph + 20) != n ||
                rd(ph + 24) != 5)
                return "its PT_LOAD is not the file's start, read-only and executable, at &72200000";
        }
        if (rd(ph) == 4) {
            note = rd(ph + 4);
            if (note + 28 > file || rd(ph + 16) < 28)
                return "its note is not in it";
        }
    }
    if (loads != 1 || !note || note + 28 > n || rd(img + note) != 6 || memcmp(img + note + 12, "ROSGD", 6) ||
        rd(img + note + 20) != 1 || rd(img + note + 24) != ROS_CLIB_NOTE_FLAG)
        return "it has no ROSGD note of version 1 with the C library's flag";
    uint32_t at = header_in(img, n);
    struct ros_clib_header h;
    if (!at || at + sizeof h > n)
        return "it has no .rosclib header";
    memcpy(&h, img + at, sizeof h);
    if (memcmp(h.magic, ROS_CLIB_MAGIC, 8) || h.format != ROS_CLIB_FORMAT)
        return "its .rosclib header is not format 1";
    if (h.major != CONTRACT_MAJOR)
        return "it is for another major version of the client contract";
    if (h.base != ROS_CROM_BASE || h.size != n || h.chunks == 0 || h.chunks > MAX_CHUNKS ||
        at + sizeof h + h.chunks * sizeof(struct ros_clib_chunk) > n)
        return "its header's base, size or chunks are wrong";
    if (h.block_size != h.tp_offset + 16 || h.block_align < 16 || (h.block_align & (h.block_align - 1)) ||
        h.tp_offset % h.block_align)
        return "its static block is not a block";
    const uint32_t lo = ROS_CROM_BASE, hi = ROS_CROM_BASE + n;
    uint32_t tbase = 0, statics = 0;
    for (uint32_t i = 0; i < h.chunks; i++) {
        struct ros_clib_chunk c;
        memcpy(&c, img + at + sizeof h + i * sizeof c, sizeof c);
        if (c.entries < lo || c.entries_end < c.entries || c.entries_end > hi || (c.entries_end - c.entries) % 4)
            return "a chunk's entries are not in the image";
        for (uint32_t e = c.entries; e < c.entries_end; e += 4) {
            uint32_t to = rd(img + (e - lo));
            if (to < lo || to >= hi)
                return "an entry is not in the image";
        }
        if (!c.data && !c.data_end)
            continue;
        if (c.data < lo || c.data_end <= c.data || c.data_end > hi || (c.data_end - c.data) % 4)
            return "a chunk's template is not in the image";
        if (!tbase || c.data < tbase)
            tbase = c.data;
        statics += c.data_end - c.data;
    }
    /* The templates tile [tbase, tbase + TP offset), with no gap and no overlap */
    if (!tbase || statics != h.tp_offset)
        return "its templates are not the static block below TP";
    for (uint32_t i = 0; i < h.chunks; i++) {
        struct ros_clib_chunk c;
        memcpy(&c, img + at + sizeof h + i * sizeof c, sizeof c);
        if (c.data_end > c.data && c.data_end > tbase + h.tp_offset)
            return "its templates are not the static block below TP";
        for (uint32_t k = 0; k < i; k++) {
            struct ros_clib_chunk d;
            memcpy(&d, img + at + sizeof h + k * sizeof d, sizeof d);
            if ((c.id & ~ROM_ONLY) == (d.id & ~ROM_ONLY))
                return "two chunks have one id";
            if (c.data_end > c.data && d.data_end > d.data && c.data < d.data_end && d.data < c.data_end)
                return "its templates overlap";
        }
    }
    *size = n;
    return NULL;
}

/* Map the ROM's image. This is done once, at the first initialisation. */
static os_error *map_image(void)
{
    if (ros_capp_code(ROS_CROM_BASE) == ROS_CAPP_LIBRARY)
        return NULL;
    uint32_t size = 0;
    const char *why = image_fault(ros_clib_rom, ROS_CLIB_ROM_SIZE, &size);
    if (why)
        return ros_error(ROS_ERR_UNIMPLEMENTED, "SharedCLibrary: the ROM C library's image is wrong: %s",
                         why);
    uint32_t room = (size + 0xFFFu) & ~0xFFFu;
    if (ros_arena_map_region("SharedCLibrary", ROS_CROM_BASE, room))
        return ros_error(ROS_ERR_UNIMPLEMENTED, "SharedCLibrary: cannot map the ROM C library at &%08X",
                         ROS_CROM_BASE);
    memcpy(ros_ptr(ROS_CROM_BASE), ros_clib_rom, size);
    __builtin___clear_cache((char *)ros_ptr(ROS_CROM_BASE), (char *)ros_ptr(ROS_CROM_BASE + room));
    if (mprotect(ros_ptr(ROS_CROM_BASE), room, PROT_READ | PROT_EXEC)) {
        ros_arena_unmap(ROS_CROM_BASE, room);
        return ros_error(ROS_ERR_UNIMPLEMENTED, "SharedCLibrary: cannot make the ROM C library executable");
    }
    os_error *e = ros_capp_code_add(ROS_CROM_BASE, ROS_CROM_BASE + size, ROS_CAPP_LIBRARY, 0);
    if (e)
        ros_arena_unmap(ROS_CROM_BASE, room);
    return e;
}

#else

static os_error *map_image(void)
{
    return NULL;
}

#endif

/* ---- registration ----------------------------------------------------------------- */

/* Whether [a, a + n) is memory that the library may write for a client */
static int writable(uint32_t a, uint32_t n)
{
    return a + n >= a && ros_arena_valid(a, a + n);
}

static os_error *no_memory(void)
{
    return ros_error(ROS_ERR_CORE_NOT_WRITABLE, "No writable memory at this address");
}

/* _Shared_Lib_Module_SWI_Code for SWIs 3 (an application) and 4 (a module) */
static void lib_init(struct ros_cpu *s, int module)
{
    uint32_t header = ros_sharedclib_image();
    if (!header) {
        ros_swi_fail(s, ros_error(ROS_ERR_UNIMPLEMENTED,
                                  "SharedCLibrary: this build has no C library image (the hosted build "
                                  "has no 32-bit-pointer code: C programs run in the box)"));
        return;
    }
    struct ros_clib_header h;
    memcpy(&h, ros_ptr(header), sizeof h);
    const uint32_t r0 = s->r[0], r1 = s->r[1], r2 = s->r[2], r3 = s->r[3], r4 = s->r[4], r5 = s->r[5],
                   r6 = s->r[6];

    /* The workspace: the stack's base and top (the statics' end, for a
     * module); it must fit (signed, as the ARM's CMP/GT) */
    int32_t own = (int32_t)(r5 - r4);
    if (own < 0)
        own = 0;
    uint32_t base, top;
    if (module) {
        /* The SVC stack's base, as sp >> 20 << 20. RISC OS's sp is below
         * an empty stack's top once the SWI has pushed its registers. The
         * top of an empty stack is a megabyte boundary. */
        base = (s->r[13] - 4) & ~0xFFFFFu;
        top = r1 + (uint32_t)own;
    } else {
        base = r1 + (uint32_t)own;
        uint32_t k = r6 >> 16;
        top = base + (k ? k << 10 : OLD_ROOT_STACK);
    }
    if ((int32_t)top > (int32_t)r2) {
        /* R6 as _Shared_Lib_Module_SWI_Code leaves it here. Failed's EXITVS
         * does not restore it. For an application it is the root stack's
         * size in bytes, with the old default if R6 is below 64K. */
        if (!module)
            s->r[6] = (r6 >> 16) ? (r6 >> 16) << 10 : OLD_ROOT_STACK;
        ros_swi_fail(s, error(E_BAD_MEMORY, "C01"));
        return;
    }
    /* From here RISC OS has stored the results for R1 and R2 (STMIB r13,
     * {r2, r12, lr}). An error after this point returns them too, with R1
     * the stack's base and R2 its top. RISC OS leaves a working value in R6
     * here. ROSGD does not reproduce that, and R6 stays as it came in. */
    s->r[1] = base;
    s->r[2] = top;

    /* The client's own statics, as CopyClientStatics and
     * ZeroInitClientStatics do it. With R5 < R4 there are none and the
     * offset is 0. Otherwise [R4, R3) is copied to R1, if it is not empty,
     * and the offset is R1 - R4. Then R5 - R3 bytes are zeroed after what
     * was copied, if that is positive. The ARM's loops go a word at a time,
     * so a count that is not a whole number of words is rounded up to one. */
    uint32_t client, copy = 0, zero = 0;        /* SL_Client_Offset */
    if ((int32_t)(r5 - r4) < 0) {
        client = 0;
    } else {
        client = r1 - r4;
        if ((int32_t)(r3 - r4) > 0)
            copy = (r3 - r4 + 3) & ~3u;
    }
    if ((int32_t)(r5 - r3) > 0)
        zero = (r5 - r3 + 3) & ~3u;
    if (copy + zero) {
        if (!writable(r1, copy + zero) || (copy && !ros_arena_readable(r4, r4 + copy))) {
            ros_swi_fail(s, no_memory());
            return;
        }
        if (copy) {
            memmove(ros_ptr(r1), ros_ptr(r4), copy);
            __builtin___clear_cache((char *)ros_ptr(r1), (char *)ros_ptr(r1 + copy));
        }
        memset(ros_ptr(r1 + copy), 0, zero);
    }

    /* Each chunk of the stub */
    uint32_t lib_offset = 0;                    /* SL_Lib_Offset: 0 until one is set */
    uint32_t tbase = templates(header, &h), top_bound = 0;  /* End of the registered templates */
    for (uint32_t p = r0;; p += 20) {
        if (!ros_arena_readable(p, p + 4)) {
            ros_swi_fail(s, no_memory());
            return;
        }
        int32_t id = (int32_t)ros_ld32(p);
        if (id < 0)
            break;
        if (!ros_arena_readable(p, p + 20)) {
            ros_swi_fail(s, no_memory());
            return;
        }
        uint32_t eb = ros_ld32(p + 4), ee = ros_ld32(p + 8), ds = ros_ld32(p + 12), de = ros_ld32(p + 16);
        struct ros_clib_chunk c = { 0 };
        uint32_t i;
        for (i = 0; i < h.chunks; i++) {
            c = chunk_at(header, i);
            if ((c.id & ~ROM_ONLY) == (uint32_t)id)
                break;
        }
        if (i == h.chunks || ((c.id & ROM_ONLY) && eb != ee)) {
            ros_swi_fail(s, error(E_UNKNOWN_LIB, "C02"));
            return;
        }
        /* The stub may have no more entries than the library. As in RISC OS,
         * the stub's table may not end further from its start than the
         * library's size allows. */
        uint32_t n = (c.entries_end - c.entries) / 4, slots = (ee - eb) / SLOT_SIZE;
        if (ee > eb && (ee - eb) > n * SLOT_SIZE) {
            ros_swi_fail(s, error(E_OLD_LIBRARY, "C63"));
            return;
        }
        if (ee < eb || (ee - eb) % SLOT_SIZE) {
            ros_swi_fail(s, error(E_UNKNOWN_LIB, "C02"));
            return;
        }
        if (slots && !writable(ee, slots * 8)) {
            ros_swi_fail(s, no_memory());
            return;
        }
        for (uint32_t k = 0; k < slots; k++)
            ros_st64(ee + 8 * k, ros_ld32(c.entries + 4 * k));

        /* The chunk's statics are the template's size. They are copied at one
         * offset for all chunks. */
        if ((ds - de) + (c.data_end - c.data) != 0) {
            ros_swi_fail(s, error(E_STATIC_SIZE, "C04"));
            return;
        }
        uint32_t size = c.data_end - c.data;
        if ((int32_t)size <= 0)
            continue;
        uint32_t to = ds + client, off = to - c.data;
        if (lib_offset && lib_offset != off) {
            ros_swi_fail(s, error(E_STATIC_OFFSET, "C05"));
            return;
        }
        lib_offset = off;
        if (!writable(to, size)) {
            ros_swi_fail(s, no_memory());
            return;
        }
        memcpy(ros_ptr(to), ros_ptr(c.data), size);
        if (c.data_end > top_bound)
            top_bound = c.data_end;
    }

    /* The block's top holds TP, its self word and the stack limit, and then
     * the base is set. TP is above the last region. Only a client that
     * registered the chunk whose template ends the block has a TP. A client
     * of the kernel chunk alone keeps whatever base it had. No such stubs
     * exist today. Nothing is written past the statics the client
     * declared. */
    if (lib_offset && top_bound == tbase + h.tp_offset) {
        uint32_t tp = tbase + lib_offset + h.tp_offset;
        if (!writable(tp, 8)) {
            ros_swi_fail(s, no_memory());
            return;
        }
        ros_st32(tp, tp);
        ros_st32(tp + 4, base);
        if (!module || !ros_capp_module_base(r0, tp))
            ros_capp_task_base(tp);
    }
    s->r[0] = r2;
    s->r[1] = base;
    s->r[2] = top;
    s->r[6] = ROS_CLIB_LIBRARY_VERSION;
    s->v = 0;
}

/* ---- the SWIs ------------------------------------------------------------------- */

void ros_thunk_SharedCLibrary_LibInitAPCS_A(struct ros_cpu *s)
{
    ros_swi_fail(s, error(E_OLD_APCS, "C71"));
}

void ros_thunk_SharedCLibrary_LibInitAPCS_R(struct ros_cpu *s)
{
    ros_swi_fail(s, error(E_OLD_APCS, "C72"));
}

void ros_thunk_SharedCLibrary_LibInitModule(struct ros_cpu *s)
{
    ros_swi_fail(s, error(E_OLD_APCS, "C72"));
}

void ros_thunk_SharedCLibrary_LibInitAPCS_32(struct ros_cpu *s)
{
    lib_init(s, 0);
}

void ros_thunk_SharedCLibrary_LibInitModuleAPCS_32(struct ros_cpu *s)
{
    lib_init(s, 1);
}

/* The rest of the chunk goes to the module's own UnknownSWI. Its text is
 * the Global messages' BadSWI, because the CLib file has no such token.
 * %0 is the title. */
static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    return error(E_UNKNOWN_SWI, "BadSWI");
}

/* ---- initialisation --------------------------------------------------------------- */

/* Set a path variable, as _Shared_Lib_Module_Init_Code's setresourcevar
 * sets it. The variable is set only if it does not exist. OS_ReadVarVal
 * with R2 = -1 tells which. It returns R2 = 0 for a variable that is not
 * there. For one that is there it returns a nonzero R2 with a "Buffer
 * overflow" error. The error itself is not looked at, because
 * setresourcevar looks only at R2 (s/initmodule). */
static void default_path(const char *name, const char *value)
{
    uint32_t n = (uint32_t)strlen(name), v = (uint32_t)strlen(value);
    char *b = ros_rma_alloc(n + v + 2);
    if (!b)
        return;
    memcpy(b, name, n + 1);
    memcpy(b + n + 1, value, v + 1);
    uint32_t r[10] = { ros_addr(b), 0, 0xFFFFFFFFu, 0, 0 };
    (void)swi(XOS_ReadVarVal, r);
    if (r[2] == 0) {
        uint32_t w[10] = { ros_addr(b), ros_addr(b + n + 1), v, 0, 0 };
        swi(XOS_SetVarVal, w);
    }
    ros_rma_free(b);
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    os_error *e = map_image();
    if (e)
        return e;
    struct workspace *w = ros_rma_alloc(sizeof *w);
    if (!w)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memset(w, 0, sizeof *w);
    strcpy(w->filename, "SharedCLibrary:Messages");
    strcpy(w->title, "SharedCLibrary");
    ros_st32(m->private_word, ros_addr(w));
    /* Clear RISC_OSLib's word and the C library's word, as
     * _Shared_Lib_Module_Init_Code clears them. */
    ros_st32(ROS_ZP_RISCOSLIBWORD, 0);
    ros_st32(ROS_ZP_CLIBWORD, 0);
    default_path("SharedCLibrary$Path", "Resources:$.Resources.CLib.");
    default_path("RISC_OSLibrary$Path", "Resources:$.Resources.RISC_OSLib.");
    return NULL;
}

/* _Shared_Lib_Module_Die_Code. It closes the messages that CLibWord keeps,
 * frees their block and clears the word. The image stays, as the ROM does. */
static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    uint32_t d = ros_ld32(ROS_ZP_CLIBWORD);
    if (d) {
        uint32_t r[10] = { d };
        swi(XMessageTrans_CloseFile, r);
        uint32_t f[10] = { 7, 0, d };
        swi(XOS_Module, f);
        ros_st32(ROS_ZP_CLIBWORD, 0);
    }
    struct workspace *w = ws();
    if (w)
        ros_rma_free(w);
    ros_st32(m->private_word, 0);
    return NULL;
}

struct ros_module sharedclib_module = {
    .title = "SharedCLibrary",
    .help = "C Library\t6.23 (15 May 2024) ROSGD native",
    .init = init,
    .final = final,
    .bad_swi = bad_swi,
    .swi_chunk = 0x80680,
    .swi_thunks = ros_swi_thunks_SharedCLibrary,
    .swi_names = ros_swi_names_SharedCLibrary,
    .swi_prefix = "SharedCLibrary",
};

__attribute__((constructor)) static void count(void)
{
    sharedclib_module.swi_count = ros_swi_count_SharedCLibrary;
}
