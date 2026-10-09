/* load.c: loads /init from the ROM's cpio image (QEMU's initrd) and starts
 * it as Linux would.
 *
 * /init is a static position-independent program. The HAL loads it at
 * INIT_BASE, above the arena's 4 GB, and gives it the stack that Linux
 * gives a new program: argc, argv, the environment and the auxiliary
 * vector. musl's start-up code reads these to find the program headers
 * and relocate itself. */
#include "hal.h"

#define INIT_BASE 0x1000000000UL                /* 64 GB */
#define STACK_TOP 0x2000000000UL                /* 128 GB */
#define STACK_SIZE (8UL << 20)

#define MAP_FIXED 0x10
#define MAP_ANON  0x20

/* ---- the cpio image (newc) ------------------------------------------- */

static uint64_t hex(const char *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = p[i];
        v = v << 4 | (uint64_t)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
    }
    return v;
}

const void *cpio_find(const char *want, uint64_t *size)
{
    const char *p = pa_to_va(boot.initrd_start), *end = pa_to_va(boot.initrd_end);
    while (p + 110 <= end && !memcmp(p, "070701", 6)) {
        uint64_t filesize = hex(p + 54), namesize = hex(p + 94);
        const char *name = p + 110;
        const char *data = p + ((110 + namesize + 3) & ~3UL);
        if (!strcmp(name, "TRAILER!!!"))
            break;
        const char *n = name;
        while (*n == '.' || *n == '/')
            n++;
        if (!strcmp(n, want)) {
            *size = filesize;
            return data;
        }
        p = data + ((filesize + 3) & ~3UL);
    }
    return 0;
}

/* ---- ELF ------------------------------------------------------------- */

struct ehdr {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4

/* The HAL builds the contents of the stack here, then copies them to the
 * box's stack. */
static unsigned char stk[8192];
static uint64_t stk_used;

static uint64_t push(const void *d, uint64_t n)
{
    stk_used += n;
    memcpy(stk + sizeof stk - stk_used, d, n);
    return STACK_TOP - stk_used;
}

void load_init(void)
{
    uint64_t size;
    const unsigned char *elf = cpio_find("init", &size);
    if (!elf)
        panic("no init in the initrd");
    const struct ehdr *eh = (const void *)elf;
    if (memcmp(eh->ident, "\177ELF", 4) || eh->machine != 183 || eh->type != 3)
        panic("init is not a position-independent AArch64 program");

    /* Map the segments, each as a region of its own. */
    uint64_t top = 0, phdr_va = 0;
    const struct phdr *ph = (const void *)(elf + eh->phoff);
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD)
            continue;
        uint64_t s = (INIT_BASE + ph[i].vaddr) & ~PAGE_MASK;
        uint64_t e = (INIT_BASE + ph[i].vaddr + ph[i].memsz + PAGE_MASK) & ~PAGE_MASK;
        int prot = 1 | 2;                       /* writable while the HAL fills it */
        if (vm_mmap(s, e - s, prot, MAP_FIXED | MAP_ANON, -1, 0) < 0)
            panic("no memory for init");
        if (copy_to_box(INIT_BASE + ph[i].vaddr, elf + ph[i].offset, ph[i].filesz))
            panic("cannot fill init's segment %d", i);
        int final = (ph[i].flags & PF_R ? 1 : 0) | (ph[i].flags & PF_W ? 2 : 0) | (ph[i].flags & PF_X ? 4 : 0);
        vm_mprotect(s, e - s, final);
        if (ph[i].offset == 0)
            phdr_va = INIT_BASE + ph[i].vaddr + eh->phoff;
        if (e > top)
            top = e;
    }
    vm_set_brk_base(top + (1UL << 20));

    /* Map and build the stack. */
    if (vm_mmap(STACK_TOP - STACK_SIZE, STACK_SIZE, 1 | 2, MAP_FIXED | MAP_ANON, -1, 0) < 0)
        panic("no memory for init's stack");
    stk_used = 0;
    uint64_t a_init = push("/init", 6);
    uint64_t a_home = push("HOME=/", 7);
    uint64_t a_term = push("TERM=linux", 11);
    uint64_t a_plat = push("aarch64", 8);
    uint64_t rnd[2] = {counter_read() * 0x9E3779B97F4A7C15UL, rtc_seconds() ^ 0xC2B2AE3D27D4EB4FUL};
    uint64_t a_rand = push(rnd, 16);
    stk_used = (stk_used + 15) & ~15UL;

    uint64_t v[64];
    int n = 0;
    v[n++] = 1;                                 /* argc */
    v[n++] = a_init;
    v[n++] = 0;
    v[n++] = a_home;
    v[n++] = a_term;
    v[n++] = 0;
#define AUX(k, x) (v[n++] = (k), v[n++] = (x))
    AUX(3, phdr_va);                            /* AT_PHDR */
    AUX(4, sizeof(struct phdr));                /* AT_PHENT */
    AUX(5, eh->phnum);                          /* AT_PHNUM */
    AUX(6, PAGE_SIZE);                          /* AT_PAGESZ */
    AUX(7, 0);                                  /* AT_BASE: no interpreter */
    AUX(8, 0);                                  /* AT_FLAGS */
    AUX(9, INIT_BASE + eh->entry);              /* AT_ENTRY */
    AUX(11, 0), AUX(12, 0), AUX(13, 0), AUX(14, 0);  /* the user and group IDs */
    AUX(15, a_plat);                            /* AT_PLATFORM */
    AUX(16, 3);                                 /* AT_HWCAP: FP, ASIMD */
    AUX(17, 100);                               /* AT_CLKTCK */
    AUX(23, 0);                                 /* AT_SECURE */
    AUX(25, a_rand);                            /* AT_RANDOM */
    AUX(31, a_init);                            /* AT_EXECFN */
    AUX(33, vm_vdso_map());                     /* AT_SYSINFO_EHDR: the vDSO (0 if none) */
    AUX(0, 0);
    if ((n & 1))                                /* keeps sp 16-byte aligned at argc */
        v[n++] = 0;
    uint64_t sp = push(v, (uint64_t)n * 8);
    if (copy_to_box(STACK_TOP - stk_used, stk + sizeof stk - stk_used, stk_used))
        panic("cannot write init's stack");

    kprintf("HAL: /init, %lu KB, at %lx; entering it\n", size >> 10, INIT_BASE + eh->entry);
    hal_enter_box(INIT_BASE + eh->entry, sp);
}
