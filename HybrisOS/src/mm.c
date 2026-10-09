/* mm.c: physical pages, and the page tables for the box's half of the
 * address space. That half is the one TTBR0 maps: the arena below 4 GB,
 * and /init above it.
 *
 * A bitmap records physical memory in 4 KB pages, one bit for each page.
 * The HAL sets a page's bit while the page is in use. At start the HAL marks its own
 * image, the device tree and the initrd as in use. The page tables are the
 * architecture's four levels with 4 KB granules. The HAL reaches them, as
 * it reaches all physical memory, through its map at HAL_KOFF. */
#include "hal.h"

extern char _start[], _end[];

#define MAX_PAGES (16UL << 30 >> 12)            /* 16 GB of RAM at most */
static uint64_t bitmap[MAX_PAGES / 64];
static uint64_t first_pa, npages, nfree, hint;

static void mark(uint64_t pa, uint64_t end, int used)
{
    for (uint64_t a = pa & ~PAGE_MASK; a < end; a += PAGE_SIZE) {
        if (a < first_pa || a >= first_pa + npages * PAGE_SIZE)
            continue;
        uint64_t i = (a - first_pa) >> 12;
        uint64_t bit = 1UL << (i & 63);
        if (used && !(bitmap[i / 64] & bit))
            bitmap[i / 64] |= bit, nfree--;
        else if (!used && (bitmap[i / 64] & bit))
            bitmap[i / 64] &= ~bit, nfree++;
    }
}

/* this core's root, which is the running process's level-0 table */
#define ttbr0_root (this_cpu()->root)

void mm_init(void)
{
    first_pa = boot.ram_base;
    npages = boot.ram_size >> 12;
    if (npages > MAX_PAGES)
        npages = MAX_PAGES;
    if (first_pa + npages * PAGE_SIZE > (uint64_t)HAL_MAP_GB << 30)
        npages = (((uint64_t)HAL_MAP_GB << 30) - first_pa) >> 12;
    memset(bitmap, 0, sizeof bitmap);
    nfree = npages;
    mark(va_to_pa(_start), va_to_pa(_end), 1);
    mark(boot.dtb_pa, boot.dtb_pa + boot.dtb_size, 1);
    if (boot.initrd_end > boot.initrd_start)
        mark(boot.initrd_start, boot.initrd_end, 1);
    /* QEMU's own start-up code sits below the image */
    mark(first_pa, va_to_pa(_start), 1);
    ttbr0_root = page_alloc();
    if (!ttbr0_root)
        panic("no memory for the page tables");
}

uint64_t page_alloc(void)
{
    for (uint64_t n = 0; n < npages; n++) {
        uint64_t i = (hint + n) % npages;
        if (bitmap[i / 64] == ~0UL) {
            n += 63 - (i & 63);
            continue;
        }
        uint64_t bit = 1UL << (i & 63);
        if (bitmap[i / 64] & bit)
            continue;
        bitmap[i / 64] |= bit;
        nfree--;
        hint = i + 1;
        uint64_t pa = first_pa + i * PAGE_SIZE;
        memset(pa_to_va(pa), 0, PAGE_SIZE);
        return pa;
    }
    return 0;
}

void page_free(uint64_t pa)
{
    mark(pa, pa + PAGE_SIZE, 0);
}

/* Allocates n consecutive pages and zeroes them. Returns the PA of the
 * first, or 0. Use it for memory that a device reads by physical address,
 * such as ramfb's screen. */
uint64_t pages_alloc_run(uint64_t n)
{
    uint64_t run = 0;
    for (uint64_t i = 0; i < npages; i++) {
        if (bitmap[i / 64] & (1UL << (i & 63))) {
            run = 0;
            continue;
        }
        if (++run == n) {
            uint64_t pa = first_pa + (i + 1 - n) * PAGE_SIZE;
            mark(pa, pa + n * PAGE_SIZE, 1);
            memset(pa_to_va(pa), 0, n * PAGE_SIZE);
            return pa;
        }
    }
    return 0;
}

uint64_t pages_free(void)
{
    return nfree;
}

uint64_t pages_total(void)
{
    return npages;
}

/* ---- the box's page tables ------------------------------------------- */

#define DESC_VALID  1UL
#define DESC_TABLE  3UL                         /* levels 0-2 */
#define DESC_PAGE   3UL                         /* level 3 */
#define DESC_AF     (1UL << 10)
#define DESC_SH     (3UL << 8)
#define DESC_AP_EL0 (1UL << 6)                  /* EL0 may use it */
#define DESC_AP_RO  (1UL << 7)
#define DESC_PXN    (1UL << 53)
#define DESC_UXN    (1UL << 54)
#define DESC_PA     0x0000FFFFFFFFF000UL
/* Marks a page that the HAL keeps while nothing may touch it (mprotect
 * with PROT_NONE). The entry is invalid, so any access faults, and it
 * holds the page's PA in the bits that the table walker ignores, together
 * with this mark. Linux keeps the memory in this case, so the HAL must
 * keep it too (#200). */
#define DESC_PARKED (1UL << 58)

static uint64_t *table(uint64_t pa) { return pa_to_va(pa); }

/* Returns the level-3 entry for va in root's tables. If make is set, it
 * makes the entry and the tables above it where they do not exist. */
static uint64_t *pte_in(uint64_t root, uint64_t va, int make)
{
    uint64_t t = root;
    for (int level = 0; level < 3; level++) {
        unsigned idx = (va >> (39 - 9 * level)) & 511;
        uint64_t *e = &table(t)[idx];
        if (!(*e & DESC_VALID)) {
            if (!make)
                return 0;
            uint64_t n = page_alloc();
            if (!n)
                return 0;
            /* Make every core's walker see the zeroed table before linking it in. */
            __asm__ volatile("dsb ishst" ::: "memory");
            *e = n | DESC_TABLE;
        }
        t = *e & DESC_PA;
    }
    return &table(t)[(va >> 12) & 511];
}

static uint64_t *pte(uint64_t va, int make)
{
    return pte_in(ttbr0_root, va, make);
}

static uint64_t desc(uint64_t pa, unsigned attr)
{
    uint64_t d = (pa & DESC_PA) | DESC_PAGE | DESC_AF | DESC_SH;   /* attribute 0: normal memory */
    if (!(attr & PG_WRITE))
        d |= DESC_AP_RO;
    if (attr & PG_USER)
        d |= DESC_AP_EL0;
    if (!(attr & PG_EXEC))
        d |= DESC_PXN | DESC_UXN;
    else if ((attr & PG_BOTHX) && (attr & PG_USER) && !(attr & PG_WRITE))
        /* read-only code that both EL0 and EL1 may run (the gate page) */
        ;
    else if (attr & PG_USER)
        d |= DESC_PXN;                          /* code for EL0 only; the OS may not run it */
    else
        d |= DESC_UXN;
    return d;
}

static void flush(uint64_t va)
{
    __asm__ volatile("dsb ishst\n tlbi vae1is, %0\n dsb ish\n isb" :: "r"(va >> 12 & 0xFFFFFFFFFFFUL) : "memory");
}

int map_page(uint64_t va, uint64_t pa, unsigned attr)
{
    uint64_t *e = pte(va, 1);
    if (!e)
        return -12;                             /* ENOMEM */
    uint64_t old = *e;
    /* Make the page's contents (zeroed or copied) visible before its
     * mapping, then make the mapping visible to every core's walker. */
    __asm__ volatile("dsb ishst" ::: "memory");
    *e = desc(pa, attr);
    if (old & DESC_VALID)
        flush(va);
    else
        __asm__ volatile("dsb ishst\n isb" ::: "memory");
    return 0;
}

uint64_t unmap_page(uint64_t va)
{
    uint64_t *e = pte(va, 0);
    if (!e || !(*e & (DESC_VALID | DESC_PARKED)))
        return 0;                               /* a parked page too; returns its PA */
    uint64_t pa = *e & DESC_PA, was = *e;
    *e = 0;
    if (was & DESC_VALID)
        flush(va);
    return pa;
}

/* Parks the page at va: the HAL keeps the page but nothing can reach it.
 * Returns its PA, or 0 if no page was mapped there. */
uint64_t park_page(uint64_t va)
{
    uint64_t *e = pte(va, 0);
    if (!e || !(*e & DESC_VALID))
        return 0;
    uint64_t pa = *e & DESC_PA;
    *e = pa | DESC_PARKED;
    flush(va);
    return pa;
}

/* Returns the PA of the page parked at va, or 0 */
uint64_t parked_page(uint64_t va)
{
    uint64_t *e = pte(va, 0);
    return e && !(*e & DESC_VALID) && (*e & DESC_PARKED) ? (*e & DESC_PA) : 0;
}

uint64_t lookup_page(uint64_t va)
{
    uint64_t *e = pte(va, 0);
    return e && (*e & DESC_VALID) ? (*e & DESC_PA) : 0;
}

void set_page_attr(uint64_t va, unsigned attr)
{
    uint64_t *e = pte(va, 0);
    if (!e || !(*e & DESC_VALID))
        return;
    *e = desc(*e & DESC_PA, attr);
    flush(va);
}

void mm_activate(void)
{
    __asm__ volatile("msr ttbr0_el1, %0\n isb\n tlbi vmalle1\n dsb ish\n isb" :: "r"(ttbr0_root) : "memory");
}

/* ---- more than one address space (vm.c's struct mm) -------------------- */

/* Maps a page in another address space's tables. fork uses it for its copy. */
int pt_map(uint64_t root, uint64_t va, uint64_t pa, unsigned attr)
{
    uint64_t *e = pte_in(root, va, 1);
    if (!e)
        return -12;
    __asm__ volatile("dsb ishst" ::: "memory");
    *e = desc(pa, attr);
    __asm__ volatile("dsb ishst" ::: "memory");
    return 0;
}

/* Parks a page in another address space's tables. fork uses it to copy a
 * parked page. */
int pt_park(uint64_t root, uint64_t va, uint64_t pa)
{
    uint64_t *e = pte_in(root, va, 1);
    if (!e)
        return -12;
    __asm__ volatile("dsb ishst" ::: "memory");
    *e = (pa & DESC_PA) | DESC_PARKED;
    return 0;
}

uint64_t pt_root(void)
{
    return ttbr0_root;
}

/* Allocates a level-0 table for a new address space. Returns its PA, or 0. */
uint64_t pt_new(void)
{
    return page_alloc();
}

/* Frees the tables of root, but not the pages they map. */
static void free_level(uint64_t t, int level)
{
    uint64_t *e = table(t);
    for (int i = 0; level < 2 && i < 512; i++)
        if (e[i] & DESC_VALID)
            free_level(e[i] & DESC_PA, level + 1);
    if (level == 2)
        for (int i = 0; i < 512; i++)
            if (e[i] & DESC_VALID)
                page_free(e[i] & DESC_PA);      /* the level-3 tables */
    page_free(t);
}

void pt_free(uint64_t root)
{
    free_level(root, 0);
}

/* Switches to root's tables: it loads TTBR0 and discards the TLB's old
 * entries. */
void pt_switch(uint64_t root)
{
    if (root == ttbr0_root)
        return;
    ttbr0_root = root;
    mm_activate();
}
