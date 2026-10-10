/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* rom.c -- placing the ROM and starting its modules. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "rosgd/arena.h"
#include "rosgd/capp.h"
#include "rosgd/cpu.h"
#include "rosgd/keyboard.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/rom.h"
#include "rosgd/sysvars.h"
#include "rosgd/vdu.h"

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 |
           (uint32_t) p[3] << 24;
}

/* Loads an x32 C module's image, as roscc link --module writes it
 * (../roscc src/x32.rs). The image is in the ROM, or in a file that *RMLoad
 * reads (runtime/module.c). It is an ELF32 whose module begins at the
 * PT_LOAD segment's file offset, with the ELF header and program headers
 * before it. One copy is claimed in the RMA, as a loaded module's, RWX as
 * the RMA is. The p_filesz bytes are copied and the rest of p_memsz is
 * zeroed (the BSS). Then the RELATIVE fix-ups of the dynamic table are
 * applied. Each Elf32_Rela's addend is added to the copy's address, and
 * the result goes into the word at the entry's offset (type 8 is a 32-bit
 * word, and 38 is a 64-bit one). With the fix-ups done the module is
 * position-free. It runs wherever it was copied, and is registered as
 * module code, entered with %gs = its base (capp.h).
 *
 * An A64X32 module (the Apple Silicon box, roscc's src/a64.rs) moves only
 * by whole 4 KB pages, because adrp is PC-relative in pages. Its copy
 * starts on a page boundary in the RMA. Its fix-ups are
 * R_AARCH64_P32_RELATIVE (&B7), which put base + addend into a 32-bit
 * word. The copy is made coherent for the instruction cache before it
 * runs.
 *
 * An x32 module's copy starts at its segment's alignment (p_align, at
 * least 16). Clang aligns its constants to 16 and loads them with aligned
 * SSE moves (movdqa). These fault where the RMA's 8-byte alignment leaves
 * the image 8 bytes off (#102). */
os_error *ros_module_image_load(const uint8_t *file, uint32_t size, const char *name,
                                uint32_t *base_out, void **block_out, uint32_t *size_out)
{
    static const uint32_t PT_LOAD = 1, PT_DYNAMIC = 2;
    static const uint32_t DT_RELA = 7, DT_RELASZ = 8;

    if (size < 52 || file[0] != 0x7F || file[1] != 'E' || file[2] != 'L' || file[3] != 'F')
        return ros_error(ROS_ERR_RM_HEADER, "%s is not an x32 module image", name);
    uint32_t phoff = le32(file + 28);
    uint16_t phnum = (uint16_t) (file[44] | file[45] << 8);

    uint32_t load_off = 0, load_filesz = 0, load_memsz = 0, load_align = 16, dyn_off = 0;
    for (uint32_t i = 0; i < phnum && (uint64_t)phoff + 32 * ((uint64_t)i + 1) <= size; i++) {
        const uint8_t *p = file + phoff + 32 * i;
        uint32_t type = le32(p);
        if (type == PT_LOAD) {
            load_off = le32(p + 4);
            load_filesz = le32(p + 16);
            load_memsz = le32(p + 20);
            uint32_t a = le32(p + 28);
            if (a > load_align && a <= 4096 && !(a & (a - 1)))
                load_align = a;
        } else if (type == PT_DYNAMIC) {
            dyn_off = le32(p + 4);
        }
    }
    if (load_off == 0 || load_off > size || load_filesz > size - load_off ||
        load_memsz < load_filesz || (dyn_off > size) || load_memsz > ROS_RMA_SIZE)
        return ros_error(ROS_ERR_RM_HEADER, "Module %s has no loadable image", name);

#if ROS_CAPP_A64
    /* A page boundary inside a block a page larger. */
    uint8_t *block = ros_rma_alloc(load_memsz + 4096);
    uint8_t *copy = block ? ros_ptr((ros_addr(block) + 4095) & ~4095u) : NULL;
#else
    /* Its segment's alignment inside a block that much larger. */
    uint8_t *block = ros_rma_alloc(load_memsz + load_align);
    uint8_t *copy = block ? ros_ptr((ros_addr(block) + load_align - 1) & ~(load_align - 1)) : NULL;
#endif
    if (!copy)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in the RMA for %s", name);
    memset(copy, 0, load_memsz);
    memcpy(copy, file + load_off, load_filesz);
    uint32_t base = ros_addr(copy);

    /* The dynamic table is inside the image. Its values are image offsets. */
    if (dyn_off) {
        uint32_t rela = 0, relasz = 0;
        for (uint32_t o = dyn_off; (uint64_t)o + 8 <= size; o += 8) {
            uint32_t tag = le32(file + o), val = le32(file + o + 4);
            if (tag == 0)
                break;
            if (tag == DT_RELA)
                rela = val;
            else if (tag == DT_RELASZ)
                relasz = val;
        }
        /* Each Elf32_Rela is r_offset (in the image), r_info and r_addend. */
        if ((uint64_t) load_off + rela + relasz <= size)
            for (uint32_t o = 0; o + 12 <= relasz; o += 12) {
                const uint8_t *e = file + load_off + rela + o;
                uint32_t off = le32(e), info = le32(e + 4);
                int32_t addend = (int32_t) le32(e + 8);
                uint32_t type = info & 0xFF;
                if (type == 8) {                        /* R_X86_64_RELATIVE */
                    if ((uint64_t)off + 4 <= load_memsz) {
                        uint32_t w = base + (uint32_t) addend;
                        memcpy(copy + off, &w, 4);
                    }
                } else if (type == 38) {                /* R_X86_64_RELATIVE64 */
                    if ((uint64_t)off + 8 <= load_memsz) {
                        uint64_t w = (uint64_t) base + (uint64_t) (uint32_t) addend;
                        memcpy(copy + off, &w, 8);
                    }
                }
#if ROS_CAPP_A64
                else if (type == 0xB7) {                /* R_AARCH64_P32_RELATIVE */
                    if ((uint64_t)off + 4 <= load_memsz) {
                        uint32_t w = base + (uint32_t) addend;
                        memcpy(copy + off, &w, 4);
                    }
                }
#endif
            }
    }
#if ROS_CAPP_A64
    __builtin___clear_cache((char *) copy, (char *) copy + load_memsz);
#endif

    os_error *e = ros_capp_code_add(base, base + load_memsz, ROS_CAPP_MODULE, base);
    if (e) {
        ros_rma_free(block);
        return e;
    }
    *base_out = base;
    if (block_out)
        *block_out = block;
    if (size_out)
        *size_out = load_memsz;
    return NULL;
}

static os_error *x32_module_load(const struct ros_rom_image *r, uint32_t *base_out,
                                 uint32_t *size_out)
{
    return ros_module_image_load(r->bytes, *r->size, r->name, base_out, NULL, size_out);
}

void ros_module_keep_pristine(struct ros_module *m, uint32_t size)
{
    m->pristine = malloc(size);
    if (m->pristine) {
        memcpy(m->pristine, ros_ptr(m->base), size);
        m->pristine_size = size;
    }
}

/* rosgd.armprefer=Title[,Title...] on the kernel command line (hosted:
 * ROSGD_ARMPREFER) is for boot-time tests. It does what *ARMPrefer Title on
 * does for each title, before any module starts. */
static void armprefer_from_cmdline(void)
{
    const char *v = getenv("ROSGD_ARMPREFER");
    if (!v && ros_cmdline_has("rosgd.armprefer"))
        v = ros_cmdline_value("rosgd.armprefer");
    char list[256];
    snprintf(list, sizeof list, "%s", v ? v : "");
    char *save = NULL;
    for (char *t = strtok_r(list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        os_error *e = ros_module_armprefer(t, 1);
        if (e)
            ros_console_printf("rosgd: rosgd.armprefer=%s: %s\n", t, e->errmess);
    }
}

os_error *ros_rom_init(void)
{
    armprefer_from_cmdline();
    for (unsigned i = 0; i < ros_rom_image_count; i++) {
        const struct ros_rom_image *r = &ros_rom_images[i];
        uint32_t base = *r->base, size = *r->size;
        /* The image must lie below the gate page. The gate page and the
         * native entries above it are mapped over the ROM's range
         * (capp.h), and would replace ROM code there without a word. */
        if (base < ROS_ROM_BASE || base >= ROS_CAPP_GATE_PAGE || size > ROS_CAPP_GATE_PAGE - base)
            return ros_error(ROS_ERR_BAD_ADDRESS,
                             "ROM image %s at &%08X (&%X bytes) is outside the ROM, or over the "
                             "C applications' gate at &%08X",
                             r->name, base, size, ROS_CAPP_GATE_PAGE);
        memcpy(ros_ptr(base), r->bytes, size);
        r->register_code();
    }

    /* The kernel's own module is in the ROM, at its start, as the kernel
     * is. The Wimp finds the ROM from its address (Wimp02, "Figure out ROM
     * location": UtilityModule's address, rounded down to a megabyte, and
     * 64 MB above). It does not write a message's sender and reference into
     * a block there. The Resource Filer sends Message_FilerOpenDir from its
     * ROM. If that message were in the RMA, the Wimp would have to fill in
     * the sender and reference. */
    ros_module_header_at(&ros_utility_module, ROS_ROM_BASE);

    /* The ROM is read-only, so a stray store into it faults at once. */
    if (mprotect(ros_ptr(ROS_ROM_BASE), ROS_ROM_SIZE, PROT_READ) != 0)
        return ros_error(ROS_ERR_BAD_ADDRESS, "Cannot make the ROM read-only");
    /* The exception is one page in its range, which is read-only and
     * executable. It is the gate that C applications make SWIs through
     * (capp.h). */
    if (ros_capp_gate_init() != 0)
        return ros_error(ROS_ERR_BAD_ADDRESS, "Cannot map the SWI gate at &%08X", ROS_CAPP_GATE_PAGE);

    ros_sysvars_init();                 /* before any module, as the kernel's */
    ros_keyboard_init();                 /* sets up the OS_Byte variables, KeyV and the
                                           buffers' owners, at the vectors' feet */
    ros_utility_module.shadow_policy = ROS_SHADOW_NEVER;
    ros_module_register_rom(&ros_utility_module);
    ros_module_add(&ros_utility_module, "");

    /* Each module is started with its policy from rom_contents.c's column.
     * A ROM module's own policy is never 0, so a module loaded from a file
     * of its title finds it (runtime/module.c, policy_of). */
    for (unsigned i = 0; i < ros_native_module_count; i++) {
        struct ros_module *m = ros_native_modules[i].module;
        m->shadow_policy = (uint8_t)(ros_native_modules[i].shadow ? ros_native_modules[i].shadow
                                                                   : ROS_SHADOW_NEVER);
        ros_module_register_rom(m);
        os_error *e = ros_module_add(m, "");
        if (e)
            ros_console_printf("rosgd: %s failed to start: %s (&%X)\n", m->title, e->errmess,
                               e->errnum);
    }

    /* The screen is set up before the compiled modules, as the kernel's VDU
     * is ready before any ROM module starts. The Wimp's initialisation
     * validates the configured mode. It is set up after the native modules,
     * since it needs GraphicsV's driver. */
    ros_dynarea_init();                 /* the system sprite area */
    ros_vdu_init();

    /* The native Window Manager takes the compiled one's place in the
     * order, and there is no compiled FilterManager. The exception is
     * rosgd.wimp=translated (hosted: ROSGD_WIMP=translated), which asks for
     * the translated 5.30 Wimp. */
    const char *wimp = getenv("ROSGD_WIMP");
    if (!wimp && ros_cmdline_has("rosgd.wimp"))
        wimp = ros_cmdline_value("rosgd.wimp");
    int native_wimp = !(wimp && !strcmp(wimp, "translated"));

    for (unsigned i = 0; i < ros_rom_image_count; i++) {
        const struct ros_rom_image *r = &ros_rom_images[i];
        if (r->code_only)
            continue;
        if (native_wimp && !strcmp(r->name, "FilterManager")) {
            /* The native Wimp's shim takes its place. */
            ros_native_filtermgr->shadow_policy = ROS_SHADOW_NEVER;
            ros_module_register_rom(ros_native_filtermgr);
            os_error *e = ros_module_add(ros_native_filtermgr, "");
            if (e)
                ros_console_printf("rosgd: the native FilterManager failed to start: %s (&%X)\n",
                                   e->errmess, e->errnum);
            continue;
        }
        if (native_wimp && !strcmp(r->name, "WindowManager")) {
            ros_native_wimp->shadow_policy = ROS_SHADOW_NEVER;
            ros_module_register_rom(ros_native_wimp);
            os_error *e = ros_module_add(ros_native_wimp, "");
            if (e)
                ros_console_printf("rosgd: the native WindowManager failed to start: %s (&%X)\n",
                                   e->errmess, e->errnum);
            continue;
        }
        struct ros_module *m = calloc(1, sizeof *m);
        if (!m)
            return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room for module %s", r->name);
        uint32_t base = *r->base, placed = 0;
        os_error *e = NULL;
        if (r->x32)
            e = x32_module_load(r, &base, &placed);
        if (!e)
            e = ros_module_from_rom(m, base);
        if (!e && r->x32)
            ros_module_keep_pristine(m, placed);
        int in_rom = 0;
        if (!e) {
            m->shadow_policy = (uint8_t)(r->shadow ? r->shadow : ROS_SHADOW_NEVER);
            ros_module_register_rom(m);
            in_rom = 1;
            e = ros_module_add(m, "");
        }
        if (e) {
            ros_console_printf("rosgd: %s failed to start: %s (&%X)\n", r->name, e->errmess,
                               e->errnum);
            /* A module whose initialisation failed stays in the ROM's
             * chain, as a ROM module does on RISC OS (*ROMModules:
             * Dormant). So the chain's entry is kept. Freeing it left
             * OS_Module 19 and 20, *ROMModules and *RMReInit reading freed
             * memory. */
            if (!in_rom)
                free(m);
        }
    }

    ros_keyboard_post_init();           /* KeyPostInit, making the console a keyboard */

    /* Service_Reset. The kernel issues it at every start, once the ROM's
     * modules are going and before Service_PostInit (Kernel s/NewReset).
     * OS_Byte 253 says which reset it was. Here it is always power-on. */
    {
        struct ros_cpu r;
        ros_cpu_enter(&r);
        r.r[1] = 0x27;
        ros_service_call(&r);
    }

    /* Service_PostInit. Every module in the ROM has started. The kernel
     * issues it after its ROM's modules have started (ITable looks for its
     * table then). */
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[1] = 0x73;
    ros_service_call(&s);
    return NULL;
}
