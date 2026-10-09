/* vm.c: the box's address space, as mmap sees it.
 *
 * A region is a range of pages, with its protection and the memory that
 * backs it. That memory is either anonymous memory or a memfd object. The
 * arena's regions are memfds, and the box changes a task's slot by mapping
 * a different memfd at &8000. A page gets no memory until something touches
 * it: a translation fault in the box's half of the address space comes to
 * vm_fault, which supplies the page. A 1.5 GB slot therefore costs nothing
 * until a program uses it, as under Linux. */
#include "hal.h"

#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4
#define MAP_SHARED  0x01
#define MAP_FIXED   0x10
#define MAP_ANON    0x20
#define MAP_FIXED_NOREPLACE 0x100000

#define EINVAL 22
#define ENOMEM 12
#define EEXIST 17
#define EBADF  9
#define EFAULT 14

/* Where mmap places mappings that are not at a fixed address. This is
 * above 4 GB, and above /init. */
#define MMAP_BASE 0x3000000000UL

/* ---- memfd objects ---------------------------------------------------- */

#define MEMFDS 4096                             /* files in the RAM file system are objects too */
#define MEMFD_DIRS 8                            /* 8 directories of 1 GB each */
static struct memfd {
    int used;
    uint64_t size;
    /* Each entry is the PA of a page of PAs; each of those is the PA of a
     * page holding the PAs of the object's pages. */
    uint64_t dir[MEMFD_DIRS];
} memfds[MEMFDS];

int memfd_obj_new(void)
{
    for (int i = 0; i < MEMFDS; i++)
        if (!memfds[i].used) {
            memset(&memfds[i], 0, sizeof memfds[i]);
            memfds[i].used = 1;
            return i;
        }
    return -ENOMEM;
}

long memfd_obj_truncate(int obj, uint64_t size)
{
    if (obj < 0 || obj >= MEMFDS || !memfds[obj].used)
        return -EBADF;
    if (size > (uint64_t)MEMFD_DIRS << 30)
        return -ENOMEM;
    memfds[obj].size = size;                    /* pages beyond the size are kept (H0) */
    return 0;
}

/* Returns a pointer to the slot that holds the PA of the page at offset off
 * in object obj. It makes the tables leading to the slot if necessary, and
 * returns 0 if there is no memory for them. */
static uint64_t *memfd_slot(int obj, uint64_t off)
{
    struct memfd *m = &memfds[obj];
    uint64_t pg = off >> 12;
    unsigned d = pg >> 18, c = (pg >> 9) & 511, p = pg & 511;
    if (d >= MEMFD_DIRS)
        return 0;
    if (!m->dir[d] && !(m->dir[d] = page_alloc()))
        return 0;
    uint64_t *dir = pa_to_va(m->dir[d]);
    if (!dir[c] && !(dir[c] = page_alloc()))
        return 0;
    return (uint64_t *)pa_to_va(dir[c]) + p;
}

/* Returns the PA of the page at offset off in object obj, allocating the
 * page if necessary. */
static uint64_t memfd_page(int obj, uint64_t off)
{
    uint64_t *slot = memfd_slot(obj, off);
    if (slot && !*slot)
        *slot = page_alloc();
    return slot ? *slot : 0;
}

/* Makes an object over physical memory that the HAL has already allocated:
 * size bytes starting at pa. */
int memfd_obj_phys(uint64_t pa, uint64_t size)
{
    int obj = memfd_obj_new();
    if (obj < 0)
        return obj;
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t *slot = memfd_slot(obj, off);
        if (!slot)
            return -ENOMEM;
        *slot = pa + off;
    }
    memfds[obj].size = size;
    return obj;
}

/* Copies n bytes between offset off of object obj and the box's address
 * box. If to_box is set, it copies from the object to the box. A page that
 * nothing has written reads as zeros, and reading it does not allocate it.
 * Returns 0, or -EFAULT. */
int memfd_obj_copy(int obj, uint64_t off, uint64_t box, uint64_t n, int to_box)
{
    static const unsigned char zero[PAGE_SIZE];
    while (n) {
        uint64_t in = off & PAGE_MASK, c = PAGE_SIZE - in;
        if (c > n)
            c = n;
        uint64_t pa = 0;
        if (to_box) {
            uint64_t *slot = memfd_slot(obj, off);  /* makes the tables, not the page */
            pa = slot ? *slot : 0;
        } else {
            pa = memfd_page(obj, off);
            if (!pa)
                return -ENOMEM;
        }
        int e = to_box ? copy_to_box(box, pa ? (const char *)pa_to_va(pa) + in : (const char *)zero, c)
                       : copy_from_box((char *)pa_to_va(pa) + in, box, c);
        if (e)
            return -EFAULT;
        off += c, box += c, n -= c;
    }
    return 0;
}

/* As memfd_obj_copy, but the other end of the copy is a HAL address. */
int memfd_obj_copy_hal(int obj, uint64_t off, void *buf, uint64_t n, int to_hal)
{
    while (n) {
        uint64_t in = off & PAGE_MASK, c = PAGE_SIZE - in;
        if (c > n)
            c = n;
        uint64_t pa = to_hal ? 0 : memfd_page(obj, off);
        if (to_hal) {
            uint64_t *slot = memfd_slot(obj, off);
            pa = slot ? *slot : 0;
        }
        if (!to_hal && !pa)
            return -ENOMEM;
        if (to_hal) {
            if (pa)
                memcpy(buf, (char *)pa_to_va(pa) + in, c);
            else
                memset(buf, 0, c);
        } else {
            memcpy((char *)pa_to_va(pa) + in, buf, c);
        }
        off += c, buf = (char *)buf + c, n -= c;
    }
    return 0;
}

/* Frees the object's pages from offset size onwards, as when a file is made
 * shorter. If free is set, it frees all the pages and the object itself.
 * Use it only on objects that nothing maps. */
void memfd_obj_trim(int obj, uint64_t size, int free)
{
    struct memfd *m = &memfds[obj];
    uint64_t first = (size + PAGE_MASK) >> 12;
    for (unsigned d = 0; d < MEMFD_DIRS; d++) {
        if (!m->dir[d])
            continue;
        uint64_t *dir = pa_to_va(m->dir[d]);
        for (unsigned c = 0; c < 512; c++) {
            if (!dir[c])
                continue;
            uint64_t *chunk = pa_to_va(dir[c]);
            for (unsigned p = 0; p < 512; p++) {
                uint64_t pg = (uint64_t)d << 18 | (uint64_t)c << 9 | p;
                if (chunk[p] && pg >= first) {
                    page_free(chunk[p]);
                    chunk[p] = 0;
                }
            }
            if (free) {
                page_free(dir[c]);
                dir[c] = 0;
            }
        }
        if (free) {
            page_free(m->dir[d]);
            m->dir[d] = 0;
        }
    }
    if (free)
        m->used = 0;
    else if (size < m->size || !m->size)
        m->size = size;
}

/* ---- regions ---------------------------------------------------------- */

/* An address space consists of its page tables and its regions. /init's
 * address space is the first. Each program that the box starts has an
 * address space of its own (process.c). */
/* Linux allows about 65530; coalesce_at merges adjacent regions that match */
#define REGIONS 16384
struct region {
    uint64_t start, end;                        /* page aligned; end is exclusive */
    int prot;
    int memfd;                                  /* the object, or -1 for anonymous memory */
    uint64_t off;                               /* offset of start within the object */
};
struct mm {
    int refs;
    int user;                                   /* a program's, run at EL0; its pages are EL0's */
    uint64_t root;                              /* the level-0 table */
    int m_nregions;
    uint64_t m_mmap_next, m_brk_base, m_brk_now;
    struct region m_regions[REGIONS];
};
#define mmc (this_cpu()->mm)                    /* this core's, i.e. the running process's */
#define regions (mmc->m_regions)
#define nregions (mmc->m_nregions)
#define mmap_next (mmc->m_mmap_next)
#define brk_base (mmc->m_brk_base)
#define brk_now (mmc->m_brk_now)

static struct mm *mm_alloc(uint64_t root)
{
    uint64_t pa = pages_alloc_run((sizeof(struct mm) + PAGE_MASK) >> 12);
    if (!pa)
        return 0;
    struct mm *m = pa_to_va(pa);
    memset(m, 0, sizeof *m);
    m->refs = 1;
    m->root = root;
    m->m_mmap_next = MMAP_BASE;
    return m;
}

void vm_init(void)
{
    if (strstr(boot.bootargs, "hal.el1apps"))
        vm_apps_el0 = 0;
    mmc = mm_alloc(pt_root());
    if (!mmc)
        panic("no memory for the box's address space");
}

struct mm *vm_current(void)
{
    return mmc;
}

/* Returns a new, empty address space, or 0 */
struct mm *vm_new(void)
{
    uint64_t root = pt_new();
    if (!root)
        return 0;
    struct mm *m = mm_alloc(root);
    if (!m)
        page_free(root);
    return m;
}

void vm_hold(struct mm *m)
{
    m->refs++;
}

/* Switches to address space m, using its page tables and its regions. */
void vm_switch(struct mm *m)
{
    mmc = m;
    pt_switch(m->root);
}

/* Moves this core into address space m for good. The core takes a reference
 * to m and drops its reference to the address space it leaves. Each core
 * holds a reference to the address space it is in, so that the end of a
 * process cannot free it while another core is using it. */
void vm_enter(struct mm *m)
{
    struct mm *was = mmc;
    if (m == was)
        return;
    vm_hold(m);
    vm_switch(m);
    if (was)
        vm_drop(was);
}

static void cut(uint64_t s, uint64_t e);

/* Drops one reference to m. Dropping the last reference frees its memory.
 * m must not be the running address space. */
void vm_drop(struct mm *m)
{
    if (--m->refs > 0)
        return;
    struct mm *was = mmc;
    vm_switch(m);                               /* cut works on the running address space */
    while (nregions)
        cut(regions[0].start, regions[0].end);
    vm_switch(was);
    pt_free(m->root);
    uint64_t pa = va_to_pa(m);
    for (uint64_t i = 0; i < (sizeof(struct mm) + PAGE_MASK) >> 12; i++)
        page_free(pa + i * PAGE_SIZE);
}

static struct region *find(uint64_t va)
{
    for (int i = 0; i < nregions; i++)
        if (va >= regions[i].start && va < regions[i].end)
            return &regions[i];
    return 0;
}

static int overlaps(uint64_t s, uint64_t e)
{
    for (int i = 0; i < nregions; i++)
        if (s < regions[i].end && e > regions[i].start)
            return 1;
    return 0;
}

static int add(uint64_t s, uint64_t e, int prot, int memfd, uint64_t off)
{
    if (nregions == REGIONS) {
        static int told;
        if (!told++)
            kprintf("HAL: an address space has its %d regions: no more mappings\n", REGIONS);
        return -ENOMEM;
    }
    regions[nregions++] = (struct region){s, e, prot, memfd, off};
    return 0;
}

/* Joins the region that ends at va to the one that starts there, if they
 * are alike. They are alike if they have the same protection and the same
 * kind of memory: both anonymous, or consecutive pages of one object. Linux
 * merges its areas in the same way. Without this, each mprotect or remap
 * leaves its pieces behind, and a busy process (the ARM container's code
 * buffers, or a slot's guard pages) ran out of regions. */
static void coalesce_at(uint64_t va)
{
    int a = -1, b = -1;
    for (int i = 0; i < nregions; i++) {
        if (regions[i].end == va)
            a = i;
        if (regions[i].start == va)
            b = i;
    }
    if (a < 0 || b < 0)
        return;
    struct region *ra = &regions[a], *rb = &regions[b];
    if (ra->prot != rb->prot || ra->memfd != rb->memfd ||
        (ra->memfd >= 0 && ra->off + (ra->end - ra->start) != rb->off))
        return;
    ra->end = rb->end;
    regions[b] = regions[--nregions];           /* if a was the last entry, it is now at b */
}

/* Removes [s, e) from every region, freeing any anonymous pages in it. */
static void cut(uint64_t s, uint64_t e)
{
    for (int i = 0; i < nregions; i++) {
        struct region r = regions[i];
        if (s >= r.end || e <= r.start)
            continue;
        uint64_t cs = s > r.start ? s : r.start, ce = e < r.end ? e : r.end;
        for (uint64_t va = cs; va < ce; va += PAGE_SIZE) {
            uint64_t pa = unmap_page(va);
            if (pa && r.memfd < 0)
                page_free(pa);
        }
        /* What is left may be a piece before the cut, after it, or both. */
        regions[i] = regions[--nregions];
        i--;
        if (r.start < cs)
            add(r.start, cs, r.prot, r.memfd, r.off);
        if (ce < r.end)
            add(ce, r.end, r.prot, r.memfd, r.off + (ce - r.start));
    }
}

/* RISC OS applications run at EL0. In /init's address space the
 * arena belongs to EL0. EL1 can never execute a page that EL0 can write
 * (this is the architecture's rule for EL0-writable pages), so when the
 * runtime jumps to A64X32 code there it faults, and the code carries on
 * at EL0 (trap.c). Read-only code, such as the gate page and Workers'
 * kernels, may run at either level. The runtime's own memory, above 4 GB,
 * belongs to EL1 alone. hal.el1apps on the command line turns this off. */
int vm_apps_el0 = 1;

#define ARENA_TOP (1UL << 32)

int vm_is_user(void)
{
    return mmc->user;
}

int vm_el0(uint64_t va)
{
    return mmc->user || (vm_apps_el0 && va < ARENA_TOP);
}

static unsigned attr_va(int prot, uint64_t va)
{
    unsigned a = vm_el0(va) ? PG_READ | PG_USER | PG_BOTHX : PG_READ;
    if (prot & PROT_WRITE)
        a |= PG_WRITE;
    if (prot & PROT_EXEC)
        a |= PG_EXEC;
    return a;
}

int memfd_obj_of_fd(int fd);                    /* syscall.c */

long vm_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off)
{
    if (!len || (addr & PAGE_MASK) || (off & PAGE_MASK))
        return -EINVAL;
    len = (len + PAGE_MASK) & ~PAGE_MASK;
    int memfd = -1;
    if (!(flags & MAP_ANON)) {
        memfd = memfd_obj_of_fd(fd);
        if (memfd < 0)
            return -EBADF;                      /* only memfds may be mapped */
    }
    uint64_t s;
    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        s = addr;
        if ((flags & MAP_FIXED_NOREPLACE) && overlaps(s, s + len))
            return -EEXIST;
        cut(s, s + len);
    } else if (addr && !overlaps(addr, addr + len)) {
        s = addr;
    } else {
        s = mmap_next;
        while (overlaps(s, s + len))
            s += PAGE_SIZE;
        mmap_next = s + len;
    }
    int e = add(s, s + len, prot, memfd, off);
    if (e)
        return e;
    coalesce_at(s + len);
    coalesce_at(s);
    return (long)s;
}

long vm_munmap(uint64_t addr, uint64_t len)
{
    if (addr & PAGE_MASK)
        return -EINVAL;
    len = (len + PAGE_MASK) & ~PAGE_MASK;
    cut(addr, addr + len);
    return 0;
}

/* Splits the region that spans va, if there is one, so that a region
 * boundary falls at va. */
static void split_at(uint64_t va)
{
    struct region *r = find(va);
    if (!r || r->start == va)
        return;
    struct region tail = *r;
    r->end = va;
    tail.off += va - tail.start;
    tail.start = va;
    add(tail.start, tail.end, tail.prot, tail.memfd, tail.off);
}

long vm_mprotect(uint64_t addr, uint64_t len, int prot)
{
    if (addr & PAGE_MASK)
        return -EINVAL;
    uint64_t e = addr + ((len + PAGE_MASK) & ~PAGE_MASK);
    split_at(addr);
    split_at(e);
    for (int i = 0; i < nregions; i++) {
        struct region *r = &regions[i];
        if (r->start < addr || r->end > e)
            continue;                           /* after the splits: wholly inside or outside */
        r->prot = prot;
        for (uint64_t va = r->start; va < r->end; va += PAGE_SIZE) {
            uint64_t parked;
            if (!lookup_page(va)) {
                if (prot && (parked = parked_page(va)))
                    map_page(va, parked, attr_va(prot, va));    /* accessible again, unchanged */
                continue;
            }
            if (!prot) {
                if (r->memfd < 0)
                    park_page(va);              /* anonymous: kept, but inaccessible (#200) */
                else
                    unmap_page(va);             /* the object keeps the page */
            } else {
                set_page_attr(va, attr_va(prot, va));
            }
        }
    }
    /* Join the pieces again where they are alike: first at the boundaries
     * inside the range, then at its two ends. */
    static uint64_t inside[REGIONS];
    int n = 0;
    for (int i = 0; i < nregions; i++)
        if (regions[i].start > addr && regions[i].start < e)
            inside[n++] = regions[i].start;
    for (int i = 0; i < n; i++)
        coalesce_at(inside[i]);
    coalesce_at(addr);
    coalesce_at(e);
    return 0;
}

void vm_set_brk_base(uint64_t base)
{
    brk_base = brk_now = (base + PAGE_MASK) & ~PAGE_MASK;
}

long vm_brk(uint64_t want)
{
    if (want <= brk_now || want < brk_base) {
        if (want >= brk_base && want < brk_now) {
            uint64_t ne = (want + PAGE_MASK) & ~PAGE_MASK;
            cut(ne, (brk_now + PAGE_MASK) & ~PAGE_MASK);
            brk_now = want;
        }
        return (long)brk_now;
    }
    uint64_t from = (brk_now + PAGE_MASK) & ~PAGE_MASK, to = (want + PAGE_MASK) & ~PAGE_MASK;
    if (to > from) {
        if (overlaps(from, to))
            return (long)brk_now;
        add(from, to, PROT_READ | PROT_WRITE, -1, 0);
    }
    brk_now = want;
    return (long)brk_now;
}

/* Handles a fault in the box's half of the address space by supplying the
 * page, if the region allows the access. A fault from EL0 is also refused
 * unless the page belongs to EL0. */
int vm_fault(uint64_t va, int write, int el0)
{
    struct region *r = find(va);
    if (!r || !r->prot || (write == 1 && !(r->prot & PROT_WRITE)) ||
        (write == 2 && !(r->prot & PROT_EXEC)) || (el0 && !vm_el0(va)))
        return -1;                              /* write: 1 for a write, 2 for a fetch */
    uint64_t page = va & ~PAGE_MASK;
    /* If the page is already mapped and the region allows the access, this
     * core's table walker took the fault before it saw a mapping that
     * another core made (or used a TLB entry that another core has since
     * replaced). Return so that the core tries the access again. */
    if (lookup_page(page))
        return 0;
    uint64_t pa = parked_page(page);            /* kept while it could not be touched */
    if (!pa)
        pa = r->memfd < 0 ? page_alloc() : memfd_page(r->memfd, r->off + (page - r->start));
    if (!pa || map_page(page, pa, attr_va(r->prot, page)))
        return -1;
    return 0;
}

/* ---- the box's memory from the HAL ---------------------------------- */

static int touch(uint64_t a, size_t n, int write)
{
    if (!n)
        return 0;
    if (a >= (1UL << 48) || a + n > (1UL << 48) || a + n < a)
        /* outside the box's half: a bad pointer, never to be read from the HAL's half */
        return -EFAULT;
    for (uint64_t p = a & ~PAGE_MASK; p < a + n; p += PAGE_SIZE)
        if (!lookup_page(p) && vm_fault(p, write, 0))
            return -EFAULT;
    return 0;
}

int copy_from_box(void *dst, uint64_t src, size_t n)
{
    if (touch(src, n, 0))
        return -EFAULT;
    memcpy(dst, (const void *)src, n);
    return 0;
}

int copy_to_box(uint64_t dst, const void *src, size_t n)
{
    if (touch(dst, n, 1))
        return -EFAULT;
    memcpy((void *)dst, src, n);
    return 0;
}

long strlen_box(uint64_t s, size_t max)
{
    for (size_t n = 0; n < max; n++) {
        char c;
        if (copy_from_box(&c, s + n, 1))
            return -EFAULT;
        if (!c)
            return (long)n;
    }
    return -EFAULT;
}

/* ---- /proc/self/maps -------------------------------------------------- */

/* Lists the regions as Linux does, in address order, one per line in the
 * form "start-end rwxp offset 00:00 0". It shows regions backed by a memfd
 * as shared ('s'). */
const char *vm_maps(void)
{
    static char text[96 * 1024];
    char *p = text, *end = text + sizeof text;
    uint64_t after = 0;
    for (;;) {
        struct region *next = 0;
        for (int i = 0; i < nregions; i++)
            if (regions[i].start >= after && (!next || regions[i].start < next->start))
                next = &regions[i];
        if (!next || end - p < 80)
            break;
        p += ksnprintf(p, (size_t)(end - p), "%08lx-%08lx %c%c%c%c %08lx 00:00 0\n", next->start, next->end,
                       next->prot & PROT_READ ? 'r' : '-', next->prot & PROT_WRITE ? 'w' : '-',
                       next->prot & PROT_EXEC ? 'x' : '-', next->memfd < 0 ? 'p' : 's', next->off);
        after = next->end;
    }
    *p = 0;
    return text;
}

/* mincore writes one byte for each page: 1 if the page is present, as Linux
 * reports residence. For a memfd mapping, the byte says whether the object
 * has the page; arena.c uses this to copy a slot through a view it has just
 * made. For anonymous memory, the byte says whether something has touched
 * the page. Returns -ENOMEM if a page is in no region at all. */
long vm_mincore(uint64_t addr, uint64_t len, uint64_t vec)
{
    if (addr & PAGE_MASK)
        return -EINVAL;
    for (uint64_t a = addr, i = 0; a < addr + len; a += PAGE_SIZE, i++) {
        if (!find(a))
            return -ENOMEM;
        struct region *r = find(a);
        uint64_t *slot = r->memfd < 0 ? 0 : memfd_slot(r->memfd, r->off + (a - r->start));
        uint8_t one = r->memfd < 0 ? lookup_page(a) || parked_page(a) : slot && *slot;
        if (copy_to_box(vec + i, &one, 1))
            return -EFAULT;
    }
    return 0;
}

/* ---- fork ----------------------------------------------------------------- */

/* Makes a copy of the running address space. The copy has the same regions.
 * It copies the pages of private (anonymous) memory now, and shares the
 * pages of memfds as Linux shares a MAP_SHARED mapping. The arena consists
 * of memfds, so what it copies is the program's own heap, stacks and data.
 * It copies at once rather than on write because the box forks only to
 * call exec straight away. */
struct mm *vm_fork(void)
{
    struct mm *c = vm_new();
    if (!c)
        return 0;
    c->user = mmc->user;
    c->m_nregions = nregions;
    memcpy(c->m_regions, regions, (size_t)nregions * sizeof regions[0]);
    c->m_mmap_next = mmap_next, c->m_brk_base = brk_base, c->m_brk_now = brk_now;
    for (int i = 0; i < nregions; i++) {
        struct region *r = &regions[i];
        if (r->memfd >= 0)
            continue;                           /* shared: pages fault in as they are used */
        for (uint64_t va = r->start; va < r->end; va += PAGE_SIZE) {
            uint64_t pa = lookup_page(va), parked = pa ? 0 : parked_page(va);
            if (!pa && !parked)
                continue;
            uint64_t np = page_alloc();
            if (!np || (pa ? pt_map(c->root, va, np, attr_va(r->prot, va)) : pt_park(c->root, va, np))) {
                vm_drop(c);
                return 0;
            }
            memcpy(pa_to_va(np), pa_to_va(pa ? pa : parked), PAGE_SIZE);
        }
    }
    return c;
}

/* Marks the running address space as a program's, run at EL0. Everything
 * mapped in it from now on belongs to EL0. execve calls this before it maps
 * the program. */
void vm_set_user(void)
{
    mmc->user = 1;
}

/* ---- the vDSO ------------------------------------------------------------
 *
 * The vDSO is one memfd object of two pages, which the HAL makes at boot.
 * The first page holds the image (vdso/vdso.c, vdso/blob.S). The second
 * holds the clock's base: the counter value at boot and the time in seconds
 * at that moment, which syscall.c's now() adds together. Every process maps
 * the object at VDSO_VA, the image readable and executable and the data
 * readable only. The process's auxv gives the address (AT_SYSINFO_EHDR), so
 * musl's clock_gettime calls into the vDSO and makes no system call. A fork
 * shares it, as it shares any memfd region. */

#define VDSO_VA 0x2FFFFFE000UL                  /* below MMAP_BASE */
static int vdso_obj = -1;
extern const char vdso_image[], vdso_image_end[];
void hal_clock_base(uint64_t *counter, uint64_t *seconds);  /* syscall.c */

void vdso_init(void)
{
    uint64_t n = (uint64_t)((uintptr_t)vdso_image_end - (uintptr_t)vdso_image);
    int obj = memfd_obj_new();
    if (obj < 0 || n > PAGE_SIZE || memfd_obj_truncate(obj, 2 * PAGE_SIZE))
        return;
    uint64_t code = memfd_page(obj, 0), data = memfd_page(obj, PAGE_SIZE);
    if (!code || !data)
        return;
    memcpy(pa_to_va(code), vdso_image, n);
    uint64_t *d = pa_to_va(data);
    hal_clock_base(&d[0], &d[1]);
    vdso_obj = obj;
}

/* Maps the vDSO in the current address space. Returns its address, or 0. */
uint64_t vm_vdso_map(void)
{
    if (vdso_obj < 0)
        return 0;
    cut(VDSO_VA, VDSO_VA + 2 * PAGE_SIZE);
    if (add(VDSO_VA, VDSO_VA + PAGE_SIZE, PROT_READ | PROT_EXEC, vdso_obj, 0) ||
        add(VDSO_VA + PAGE_SIZE, VDSO_VA + 2 * PAGE_SIZE, PROT_READ, vdso_obj, PAGE_SIZE))
        return 0;
    return VDSO_VA;
}
