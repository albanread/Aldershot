/* dtb.c: reads the device tree that QEMU passes at start-up.  The HAL reads
 * only the parts it needs: the RAM, the initrd and the command line. */
#include "hal.h"

struct hal_boot boot;

static uint32_t be32(const void *p)
{
    const unsigned char *b = p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static uint64_t be_cells(const void *p, uint32_t len)
{
    return len >= 8 ? (uint64_t)be32(p) << 32 | be32((const char *)p + 4) : be32(p);
}

enum { BEGIN_NODE = 1, END_NODE = 2, PROP = 3, NOP = 4, END = 9 };

void dtb_read(uint64_t dtb_pa)
{
    const char *h = pa_to_va(dtb_pa);
    if (be32(h) != 0xd00dfeed)
        panic("no device tree at %lx", dtb_pa);
    boot.dtb_pa = dtb_pa;
    boot.dtb_size = be32(h + 4);
    const char *st = h + be32(h + 8);           /* the structure block */
    const char *strings = h + be32(h + 12);
    boot.bootargs = "";

    /* On the virt machine the root node's #address-cells and #size-cells
     * are both 2, so the code reads the memory node's reg property as two
     * 64-bit values. */
    int depth = 0, in_memory = 0, in_chosen = 0, in_cpus = 0;
    const char *p = st;
    for (;;) {
        uint32_t tok = be32(p);
        p += 4;
        if (tok == BEGIN_NODE) {
            const char *name = p;
            p += (strlen(name) + 4) & ~3UL;
            depth++;
            if (depth == 2) {
                in_memory = !strncmp(name, "memory", 6) && !boot.ram_size;
                in_chosen = !strcmp(name, "chosen");
                in_cpus = !strcmp(name, "cpus");
            } else if (depth == 3 && in_cpus && !strncmp(name, "cpu@", 4)) {
                boot.ncpus++;
            }
        } else if (tok == END_NODE) {
            if (depth == 2)
                in_memory = in_chosen = in_cpus = 0;
            depth--;
        } else if (tok == PROP) {
            uint32_t len = be32(p), nameoff = be32(p + 4);
            const char *val = p + 8, *name = strings + nameoff;
            p += 8 + ((len + 3) & ~3UL);
            if (in_memory && !strcmp(name, "reg") && len >= 16) {
                boot.ram_base = be_cells(val, 8);
                boot.ram_size = be_cells(val + 8, 8);
            } else if (in_chosen && !strcmp(name, "linux,initrd-start")) {
                boot.initrd_start = be_cells(val, len);
            } else if (in_chosen && !strcmp(name, "linux,initrd-end")) {
                boot.initrd_end = be_cells(val, len);
            } else if (in_chosen && !strcmp(name, "bootargs")) {
                boot.bootargs = val;
            }
        } else if (tok == NOP) {
            continue;
        } else {
            break;                              /* END, or a token that is not valid */
        }
    }
    if (!boot.ram_size)
        panic("the device tree has no memory node");
}
