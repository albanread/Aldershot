/* hal.h. Declarations for the HAL (HybrisOS), which runs BOX on QEMU's virt
 * machine.
 *
 * The HAL is a small AArch64 kernel. It runs at EL1 in the top half of the
 * address space, where it maps all of physical memory at HAL_KOFF plus the
 * physical address. The bottom half belongs to the box: the arena lies
 * below 4 GB, and /init and its memory lie above it. */
#ifndef HAL_H
#define HAL_H

/* ---- Memory layout, for boot.S and C ---------------------------------- */

#define HAL_KOFF          0xFFFF000000000000
#define HAL_TEXT_OFFSET   0x80000
#define HAL_RAM_PA        0x40000000            /* start of QEMU virt's RAM */
#define HAL_LOAD_PA       0x40080000            /* HAL_RAM_PA + HAL_TEXT_OFFSET */
#define HAL_UART_PA       0x09000000            /* the PL011 UART */
#define HAL_MAP_GB        16                    /* GB of physical memory mapped at boot */

/* MAIR_EL1: attribute 0 is normal write-back memory, attribute 1 is device
 * nGnRE memory */
#define HAL_MAIR          0x04FF
/* 1 GB block descriptors: one for normal memory (inner shareable, and EL0
 * may not execute it), and one for devices (nothing may execute it) */
#define HAL_BLOCK_NORMAL  0x0040000000000701
#define HAL_BLOCK_DEVICE  0x0060000000000405
/* TCR_EL1, without the IPS field: 48-bit address halves, 4 KB granules,
 * write-back table walks */
#define HAL_TCR           0xB5103510

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096UL
#define PAGE_MASK (PAGE_SIZE - 1)

/* Convert between a physical address and the address the HAL uses for it */
static inline void *pa_to_va(uint64_t pa) { return (void *)(pa + HAL_KOFF); }
static inline uint64_t va_to_pa(const void *va) { return (uint64_t)va - HAL_KOFF; }

/* ---- uart.c: the console -------------------------------------------- */
void uart_putc(int c);
void uart_write(const char *s, size_t n);
int uart_getc(void);                    /* -1 if no character is waiting */
void kprintf(const char *fmt, ...);
int ksnprintf(char *buf, size_t n, const char *fmt, ...);
__attribute__((noreturn)) void panic(const char *fmt, ...);

/* ---- dtb.c: what QEMU's device tree says ----------------------------- */
struct hal_boot {
    uint64_t ram_base, ram_size;        /* from the first /memory node */
    uint64_t initrd_start, initrd_end;  /* physical addresses, from /chosen */
    const char *bootargs;               /* from /chosen, or "" */
    uint64_t dtb_pa, dtb_size;
    unsigned ncpus;                     /* number of cpu@ nodes under /cpus */
};
extern struct hal_boot boot;
void dtb_read(uint64_t dtb_pa);

/* ---- smp.c: the cores ------------------------------------------------
 *
 * Each core has its own current thread, the address space it runs in, and
 * its own exception stack. The HAL itself runs on only one core at a time,
 * under a single lock that it takes on every entry from the box. The box
 * runs on all the cores. */
#define NCPU 8
struct thread;
struct mm;
struct cpu {
    int id, idle;                       /* idle: 1 waiting, 2 woken for a thread, 3 dispatching its interrupt */
    struct thread *cur;
    struct mm *mm;                      /* vm.c's address space, in TTBR0 */
    uint64_t root;                      /* mm.c's level-0 table for it */
    /* How the core spent its time (stats.c), in counter ticks. since is
     * when the core took the lock. t_idle is time idle in WFI, t_hal is
     * time in the HAL holding the lock, and t_spin is time waiting for the
     * lock. wakes, switches and ticks count how often the core woke,
     * switched threads, and took a timer tick. */
    uint64_t since, t_idle, t_hal, t_spin;
    uint64_t slept;                     /* when it began waiting in WFI, while it waits */
    uint64_t wakes, switches, ticks;
};
extern struct cpu cpus[NCPU];
static inline struct cpu *this_cpu(void)
{
    struct cpu *c;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(c));
    return c;
}
void hal_lock(void);
void hal_unlock(void);
void smp_start(void);                   /* start the other cores in the scheduler */
void smp_wake_idle(uint32_t cpus);      /* interrupt a waiting core, one of cpus (0 for any) */
unsigned smp_count(void);

/* ---- mm.c: physical pages and the box's page tables ---------------- */
void mm_init(void);
uint64_t page_alloc(void);              /* PA of a zeroed page, or 0 */
void page_free(uint64_t pa);
uint64_t pages_free(void);
uint64_t pages_total(void);

/* Page attributes for the box's half of the address space (map_page's attr) */
#define PG_READ     1u
#define PG_WRITE    2u
#define PG_EXEC     4u
#define PG_USER     8u                  /* EL0 may use the page (phase H6) */
/* PG_BOTHX, with PG_EXEC and PG_USER and without PG_WRITE: EL0 and EL1 may both execute the page */
#define PG_BOTHX    16u
int map_page(uint64_t va, uint64_t pa, unsigned attr);   /* 0, or -ENOMEM */
uint64_t unmap_page(uint64_t va);       /* PA the page held, or 0 */
uint64_t lookup_page(uint64_t va);      /* PA, or 0 if not mapped */
uint64_t park_page(uint64_t va);        /* keep, but inaccessible (PROT_NONE); its PA, or 0 */
uint64_t parked_page(uint64_t va);      /* PA of a parked page, or 0 */
void set_page_attr(uint64_t va, unsigned attr);
void mm_activate(void);                 /* load the box's tables into TTBR0 */

/* ---- vm.c: the box's address space, as mmap sees it ---------------- */
void vm_init(void);
struct mm;                              /* an address space (vm.c) */
struct mm *vm_current(void);
struct mm *vm_new(void);                /* a new empty address space, or 0 */
void vm_hold(struct mm *m);
void vm_drop(struct mm *m);             /* drop a reference; the last frees it (never the running one) */
void vm_switch(struct mm *m);
void vm_enter(struct mm *m);            /* switch this core to m and hold it; release the one left */
uint64_t pt_root(void);
uint64_t pt_new(void);
void pt_free(uint64_t root);
void pt_switch(uint64_t root);
int pt_map(uint64_t root, uint64_t va, uint64_t pa, unsigned attr);
int pt_park(uint64_t root, uint64_t va, uint64_t pa);
void vdso_init(void);                   /* set up the vDSO's page at boot */
uint64_t vm_vdso_map(void);             /* map the vDSO into the current address space; its address */
struct mm *vm_fork(void);               /* a copy of the running address space, or 0 */
void vm_set_user(void);                 /* mark the running address space as a program's, at EL0 */
struct frame;
long tty_open_master(void);
long tty_open_slave(int n);
void tty_hold(int i, int master);
void tty_close(int i, int master);
long tty_master_write(int i, uint64_t buf, uint64_t n);
long tty_master_read(struct frame *fr, int i, uint64_t buf, uint64_t n, int nonblock);
long tty_slave_read(struct frame *fr, int i, uint64_t buf, uint64_t n, int nonblock);
long tty_slave_write(struct frame *fr, int i, uint64_t buf, uint64_t n, int nonblock);
unsigned tty_poll(int i, int master);
long tty_ioctl(int i, int master, uint32_t req, uint64_t arg);
long vm_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off);
long vm_munmap(uint64_t addr, uint64_t len);
long vm_mprotect(uint64_t addr, uint64_t len, int prot);
long vm_brk(uint64_t brk);
void vm_set_brk_base(uint64_t base);
int vm_fault(uint64_t va, int write, int el0);   /* 0 if the page was supplied */
int vm_el0(uint64_t va);                /* whether EL0 may use va (arena or program memory) */
int vm_is_user(void);                   /* whether the running address space is a program's */
extern int vm_apps_el0;                 /* run RISC OS applications at EL0 (not with hal.el1apps) */
int memfd_obj_new(void);                /* a new memfd object, or -errno */
long memfd_obj_truncate(int obj, uint64_t size);
int memfd_obj_phys(uint64_t pa, uint64_t size); /* an object over the given pages */
int memfd_obj_copy(int obj, uint64_t off, uint64_t box, uint64_t n, int to_box);
int memfd_obj_copy_hal(int obj, uint64_t off, void *buf, uint64_t n, int to_hal);
void memfd_obj_trim(int obj, uint64_t size, int free);
void hal_now(uint64_t *sec, uint64_t *nsec);    /* syscall.c: the time of day */
uint64_t pages_alloc_run(uint64_t n);   /* n contiguous zeroed pages; PA of the first, or 0 */

/* Device register access, by physical address. Each access is a single
 * plain load or store. Under HVF the processor must describe a register
 * access fully in its syndrome (so no load or store pair, and no
 * write-back), or QEMU cannot emulate it. */
static inline uint32_t mmio_r32(uint64_t pa)
{
    uint32_t v;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(pa_to_va(pa)) : "memory");
    return v;
}
static inline void mmio_w32(uint64_t pa, uint32_t v)
{
    __asm__ volatile("str %w0, [%1]" :: "r"(v), "r"(pa_to_va(pa)) : "memory");
}
static inline uint8_t mmio_r8(uint64_t pa)
{
    uint32_t v;
    __asm__ volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(pa_to_va(pa)) : "memory");
    return (uint8_t)v;
}
static inline void mmio_w8(uint64_t pa, uint8_t v)
{
    __asm__ volatile("strb %w0, [%1]" :: "r"((uint32_t)v), "r"(pa_to_va(pa)) : "memory");
}
static inline void mmio_w64(uint64_t pa, uint64_t v)
{
    __asm__ volatile("str %0, [%1]" :: "r"(v), "r"(pa_to_va(pa)) : "memory");
}

/* ---- virtio.c: the virtio-mmio transport, and virtio-input ------------ */
#define VIRTIO_IRQ0 48                  /* the INTID of transport n is 48 + n */
struct vdesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;
};
#define VDESC_NEXT  1
#define VDESC_WRITE 2
struct vq {                             /* a split virtqueue, one page for each part */
    struct vdesc *desc;
    volatile uint16_t *avail;           /* flags, idx, ring[] */
    volatile uint16_t *used;            /* flags, idx, then (id, len) pairs as 32-bit words */
    uint16_t size, avail_idx, used_seen;
};
void virtio_init(void);
int virtio_begin(uint64_t base, uint32_t want);  /* reset; agree the modern interface and the
                                                    features in want that are offered; 0 or -1 */
void spi_enable(unsigned intid);
int virtio_queue(uint64_t base, unsigned q, struct vq *vq, uint16_t size);
void virtio_go(uint64_t base);          /* set DRIVER_OK */
void virtio_offer(struct vq *vq, uint16_t head);
void virtio_notify(uint64_t base, unsigned q);
int ninep_attach(uint64_t base);        /* ninep.c: called when a virtio-9p device is found */

/* The value a call returns when its thread waits and another thread runs.
 * The frame no longer belongs to the caller, so the HAL writes no result
 * into it. */
#define SWITCHED (-100000L)

/* ---- socket.c: the box's BSD sockets, over lwIP --------------------- */
struct frame;
long sock_socket(long domain, long type, long proto);   /* returns an object */
void sock_hold(int s);
void sock_close(int s);
int sock_nonblock(int s, int set);      /* if set < 0, only reads the setting */
unsigned sock_poll(int s);              /* POLLIN, POLLOUT, POLLERR, POLLHUP */
long sock_bind(int s, uint64_t addr, uint64_t len);
long sock_listen(int s, long backlog);
long sock_accept(struct frame *fr, int s, uint64_t addr, uint64_t lenp);  /* returns an object */
long sock_connect(struct frame *fr, int s, uint64_t addr, uint64_t len);
long sock_send(struct frame *fr, int s, uint64_t buf, uint64_t n, long flags, uint64_t addr, uint64_t alen);
long sock_recv(struct frame *fr, int s, uint64_t buf, uint64_t n, long flags, uint64_t addr, uint64_t alenp);
long sock_sendmsg(struct frame *fr, int s, uint64_t msg, long flags);
long sock_recvmsg(struct frame *fr, int s, uint64_t msg, long flags);
long sock_name(int s, uint64_t addr, uint64_t lenp, int peer);
long sock_setopt(int s, long level, long name, uint64_t val, uint64_t len);
long sock_getopt(int s, long level, long name, uint64_t val, uint64_t lenp);
long sock_shutdown(int s, long how);
long sock_ioctl(int s, uint32_t req, uint64_t arg);

/* ---- sound.c: virtio-sound ----------------------------------------- */
#define HAL_SYS_AUDIO 0x7201            /* HAL call: the sound descriptor; Linux gives -ENOSYS */
int sound_attach(uint64_t base, unsigned slot);
int sound_irq(unsigned slot);
int sound_present(void);
long sound_write(struct frame *fr, uint64_t buf, uint64_t n);
long sound_ioctl(uint32_t req);

/* ---- unix.c: AF_UNIX stream sockets ---------------------------------- */
long unix_socket(long type);
long unix_pair(long type, int pair[2]);
void unix_hold(int i);
void unix_close(int i);
int unix_nonblock(int i, int set);
long unix_bind(int i, uint64_t va, uint64_t len);
long unix_listen(int i);
long unix_connect(int i, uint64_t va, uint64_t len);
long unix_accept(struct frame *fr, int i);
long unix_send(struct frame *fr, int i, uint64_t buf, uint64_t n, int dontwait);
long unix_recv(struct frame *fr, int i, uint64_t buf, uint64_t n, int flags);
unsigned unix_poll(int i);
long unix_shutdown(int i, long how);
long unix_name(int i, uint64_t va, uint64_t lenp, int peer);
long unix_getopt(int i, long level, long name, uint64_t val, uint64_t lenp);
long unix_fionread(int i);

/* ---- net.c: virtio-net and lwIP -------------------------------------- */
int net_attach(uint64_t base, unsigned slot);   /* called when a virtio-net device is found */
void net_init(void);                    /* start lwIP, the loopback and DHCP */
int net_irq(unsigned slot);             /* 1 if the interrupt was the network's */
void net_poll(void);                    /* run lwIP's timers and the loopback; call often */
const char *net_pnp(void);              /* text of /proc/net/pnp: the name servers */
const char *proc_text(const char *path);  /* proc.c: one of the HAL's /proc files, or 0 */
const char *vm_maps(void);              /* vm.c: text of /proc/self/maps */
long vm_mincore(uint64_t addr, uint64_t len, uint64_t vec);   /* vm.c: which pages are resident */
unsigned hal_random(void);
void *memmove(void *d, const void *s, size_t n);

/* ---- ninep.c: the share at /host; each call returns -errno on failure - */
int share_has(const char *path);        /* whether path is under /host and there is a share */
long share_open(const char *path, long flags, long mode);   /* returns an object */
void share_hold(int obj);
void share_close(int obj);
const char *share_path(int obj);
long share_read(int obj, uint64_t buf, uint64_t n, int64_t off);    /* if off < 0, at the file position */
long share_write(int obj, uint64_t buf, uint64_t n, int64_t off);
long share_seek(int obj, int64_t off, int whence);
long share_stat(const char *path, uint64_t st, int follow);
long share_fstat(int obj, uint64_t st);
long share_truncate(int obj, uint64_t size);
long share_chmod(const char *path, uint32_t mode);
long share_utimens(const char *path, uint64_t times, int follow);
long share_fsync(int obj);
long share_access(const char *path);
long share_statfs(const char *path, uint64_t out);         /* if path is 0, the share's root */
long share_mkdir(const char *path, uint32_t mode);
long share_unlink(const char *path, long flags);
long share_rename(const char *from, const char *to);
long share_readlink(const char *path, uint64_t buf, uint64_t n);
long share_getdents(int obj, uint64_t buf, uint64_t n);
long share_getxattr(const char *path, const char *name, uint64_t buf, uint64_t n, int follow);
long share_setxattr(const char *path, const char *name, uint64_t val, uint64_t n, long flags, int follow);
int virtio_irq(unsigned n);             /* 1 if there is new input */
int input_devices(void);
const char *input_name(int i);
const int32_t *input_absinfo(int i, int axis);  /* value, min, max, fuzz, flat, resolution */
int input_pending(int i);
long input_read(int i, uint64_t buf, uint64_t n);

/* ---- ramfs.c: the box's own file tree, held in memory ----------------- */
void ram_init(void);
long ram_resolve(const char *path, char *canon, int follow);
long ram_open(const char *path, long flags, long mode);
void ram_hold(int obj);
void ram_close(int obj);
const char *ram_path(int obj);
long ram_read(int obj, uint64_t buf, uint64_t n, int64_t off);
long ram_write(int obj, uint64_t buf, uint64_t n, int64_t off);
long ram_seek(int obj, int64_t off, int whence);
long ram_truncate(int obj, uint64_t size);
long ram_stat(const char *path, uint64_t st, int follow);
long ram_fstat(int obj, uint64_t st);
long ram_chmod(const char *path, uint32_t mode);
long ram_utimens(const char *path, uint64_t times, int follow);
long ram_access(const char *path);
long ram_statfs(uint64_t out);
long ram_mkdir(const char *path, uint32_t mode);
long ram_symlink(const char *target, const char *path);
long ram_unlink(const char *path, long flags);
long ram_rename(const char *from, const char *to);
long ram_readlink(const char *path, uint64_t buf, uint64_t n);
long ram_getdents(int obj, uint64_t buf, uint64_t n);
long ram_getxattr(const char *path, const char *name, uint64_t buf, uint64_t n, int follow);
long ram_setxattr(const char *path, const char *name, uint64_t val, uint64_t n, long flags, int follow);

/* ---- vfs.c: chooses the share or the RAM file system by path. An open
 * file is a handle (with H_SHARE added for the share's files) ---------- */
long vfs_resolve(const char *path, char *canon, int follow);     /* canon holds 1024 bytes */
long vfs_open(const char *canon, long flags, long mode);
void vfs_hold(long h);
void vfs_close(long h);
const char *vfs_path(long h);
long vfs_read(long h, uint64_t buf, uint64_t n, int64_t off);
long vfs_write(long h, uint64_t buf, uint64_t n, int64_t off);
long vfs_seek(long h, int64_t off, int whence);
long vfs_fstat(long h, uint64_t st);
long vfs_truncate(long h, uint64_t size);
long vfs_fsync(long h);
long vfs_getdents(long h, uint64_t buf, uint64_t n);
long vfs_stat(const char *p, uint64_t st, int follow);
long vfs_statfs(const char *p, uint64_t out);
long vfs_chmod(const char *p, uint32_t mode);
long vfs_utimens(const char *p, uint64_t times, int follow);
long vfs_access(const char *p);
long vfs_mkdir(const char *p, uint32_t mode);
long vfs_unlink(const char *p, long flags);
long vfs_rename(const char *from, const char *to);
long vfs_readlink(const char *p, uint64_t buf, uint64_t n);
long vfs_symlink(const char *target, const char *p);
long vfs_getxattr(const char *p, const char *name, uint64_t buf, uint64_t n, int follow);
long vfs_setxattr(const char *p, const char *name, uint64_t val, uint64_t n, long flags, int follow);

/* ---- ramfb.c: the screen ------------------------------------------------ */
struct hal_screen {                     /* what HAL_SYS_SCREEN returns to the box */
    int32_t fd;                         /* a memfd over the pixels, for mmap */
    uint32_t width, height, pitch;      /* XRGB8888 */
    /* gpu is 1 for virtio-gpu: the box may ask for any size, and HAL_SYS_FLUSH shows changes */
    uint32_t gpu;
};
#define HAL_SYS_FLUSH 0x7202            /* x, y, w, h: the area the box wrote, to be shown */
#define HAL_SYS_STATS 0x7203            /* buffer, size: fills struct hal_stats (stats.c), returns its size */

/* What the HAL has done since it started, for DeskMeter's HAL window. The
 * values are running totals; the box reads them twice to work out a rate.
 * Times are in nanoseconds. This is version 1. The box reads only the
 * fields it knows of, judged by the size. */
#define HAL_STATS_CPUS 8
struct hal_stats {
    uint64_t version, ncpu, now_ns, hz;
    uint64_t mem_pages, mem_free_pages, processes, threads;
    uint64_t syscalls, irqs, faults, signals;
    uint64_t net_rx_packets, net_rx_bytes, net_tx_packets, net_tx_bytes;
    uint64_t share_calls, share_bytes, screen_flushes, screen_bytes;
    struct {
        uint64_t idle_ns, hal_ns, spin_ns, wakes, switches, ticks, pad[2];
    } cpu[HAL_STATS_CPUS];
};
extern struct hal_counts {              /* counts the HAL keeps as it runs, under its lock */
    uint64_t syscalls, irqs, faults, signals;
    uint64_t net_rx_packets, net_rx_bytes, net_tx_packets, net_tx_bytes;
    uint64_t share_calls, share_bytes, screen_flushes, screen_bytes;
} counts;
long stats_read(uint64_t buf, uint64_t size);
int gpu_attach(uint64_t base);
int gpu_present(void);
int gpu_screen(uint32_t w, uint32_t h, struct hal_screen *s);
long gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
#define HAL_SYS_SCREEN 0x7200          /* the HAL's own calls; Linux returns -ENOSYS */
int ramfb_screen(struct hal_screen *s); /* the screen's memfd object, or -errno */
int memfd_obj_of_fd(int fd);            /* syscall.c: the descriptor's object, or -1 */
int copy_from_box(void *dst, uint64_t src, size_t n);    /* 0, or -EFAULT */
int copy_to_box(uint64_t dst, const void *src, size_t n);
long strlen_box(uint64_t s, size_t max);

/* ---- Exceptions ------------------------------------------------------ */
struct frame {                          /* the registers, as vectors.S saves them */
    uint64_t x[31];
    uint64_t sp;                        /* SP_EL0 */
    uint64_t elr, spsr;
    uint64_t esr, far;
};
void trap_init(void);

/* ---- thread.c --------------------------------------------------------- */
enum { T_FREE, T_RUN, T_WAIT };
/* the kernel's struct sigaction on arm64 */
struct ksigaction {
    uint64_t handler, flags, restorer, mask;
};

/* A process: /init, or a program that the box started (process.c). Its
 * threads share its address space, descriptors and signal actions. */
struct process {
    int used, pid, ppid;
    int threads;                        /* number of live threads */
    struct mm *mm;
    void *fdt;                          /* syscall.c's descriptor table */
    struct ksigaction act[65];
    int zombie, status;                 /* set when it has ended; status is for wait4 */
    int vfork_tid;                      /* a vfork's parent thread, waiting until exec or exit */
    unsigned umask;                     /* applied to what openat and mkdirat create */
    char cwd[256];                      /* where relative paths start */
    uint32_t uid[3], gid[3];            /* real, effective, saved (sshd drops root) */
    uint64_t alarm_at, alarm_every;     /* ITIMER_REAL, in counter ticks, or 0 */
    char exe[128];
};

struct thread {
    int state, tid;
    int oncpu;                          /* the core running it, or -1 */
    uint32_t cpus;                      /* cores it may run on (sched_setaffinity); 0 for any */
    struct process *proc;
    struct frame fr;                    /* saved registers, while it is not running */
    unsigned char fp[528] __attribute__((aligned(16)));   /* V0-V31, FPSR, FPCR */
    uint64_t tpidr;                     /* musl's thread pointer */
    uint64_t clear_tid;                 /* set_tid_address, CLONE_CHILD_CLEARTID */
    uint64_t futex;                     /* the key it waits on, or 0 */
    uint64_t wake_at;                   /* the counter value it waits until, or 0 */
    long timeout_result;                /* its x0 if the time runs out first */
    int restart;                        /* when woken, it makes its call again */
    /* recall: when woken this way, its frame points at the svc, so that it repeats the call */
    int recall;
    uint64_t restart_x0, poll_deadline;
    uint64_t sock_deadline;             /* a socket call's timeout, while it waits */
    uint64_t sigmask, pending;
    uint64_t saved_mask;                /* for ppoll: mask to restore while mask_restore is set */
    int mask_restore;
    uint64_t last_nr, last_ret;         /* last call number and result (for thread_dump) */
    uint64_t alt_sp;                    /* the alternate signal stack (stack_t) */
    int32_t alt_flags, alt_pad;
    uint64_t alt_size;
};
void proc_init(void);                   /* process.c: set up /init as process 1 */
void thread_kill_process(struct process *p);
void thread_signal_process(struct process *p, int sig);
void thread_exec_reset(struct thread *t);
void thread_end_quiet(struct frame *f);
void thread_interrupt(struct thread *t);  /* end a waiting thread's call with EINTR */
void thread_first(struct frame *f);
int thread_dead(void);
void gic_init_cpu(unsigned id);
void secondary_main(unsigned id);
long vfs_pread_hal(long h, void *buf, uint64_t n, uint64_t off);
long share_pread_hal(int obj, void *buf, uint64_t n, uint64_t off);
long ram_pread_hal(int obj, void *buf, uint64_t n, uint64_t off);
void vm_set_brk_base(uint64_t base);
struct process *proc_new(struct process *parent, int share_mm);
/* A thread has ended; proc_thread_gone checks whether its process has ended too */
void proc_thread_gone(struct frame *f, struct thread *t);
void proc_exit(struct process *p, int status);
long proc_wait(struct frame *f, long pid, uint64_t status, long options);
long proc_execve(struct frame *f, uint64_t path, uint64_t argv, uint64_t envp);
long proc_kill(long pid, long sig);
void proc_alarms(void);
struct process *proc_by_pid(int pid);
void *fdt_new(void *from);              /* syscall.c: a new descriptor table, copied from from */
void fdt_close_all(void *fdt);
void fdt_free(void *fdt);
void fdt_exec(void *fdt);               /* close the close-on-exec descriptors */
#define KEY_CONSOLE 1                   /* thread_block keys that are not addresses */
#define KEY_POLL    2
void thread_init(void);
struct thread *thread_current(void);
struct thread *thread_by_tid(int tid);
void schedule(struct frame *f);
void thread_block(struct frame *f, uint64_t key, uint64_t wake_at, long timeout_result);
void thread_block_restart(struct frame *f, uint64_t key, uint64_t wake_at);
void thread_wake(struct thread *t, long result);
void thread_irq(struct frame *f);
long thread_clone(struct frame *f);
void thread_exit(struct frame *f);
long futex_wake(uint64_t key, long n);
long futex_requeue(uint64_t a, long n, long m, uint64_t b);
void hal_enter_box(uint64_t entry, uint64_t sp);   /* never returns */

/* ---- signal.c -------------------------------------------------------- */
int signal_deliver(struct frame *f, int sig, int code, uint64_t addr);  /* 0, or -1 for the default action */
long signal_return(struct frame *f);
long signal_action(long sig, uint64_t act, uint64_t old);
long signal_procmask(long how, uint64_t set, uint64_t old);
long signal_altstack(uint64_t ss, uint64_t old);

/* ---- syscall.c ------------------------------------------------------- */
void syscall(struct frame *f);
void syscall_init(void);

/* ---- timer.c, psci.c ------------------------------------------------- */
uint64_t counter_read(void);
uint64_t counter_freq(void);
uint64_t rtc_seconds(void);             /* the PL031 clock: seconds since 1970 */
#define HZ 100                          /* tick rate */
#define TIMER_IRQ 27                    /* the virtual timer's PPI */
void gic_init(void);
void tick_arm(void);                    /* set the next tick, 1/HZ from now */
void tick_stop(void);                   /* stop ticks on a waiting core (not the boot core) */
unsigned irq_ack(void);
void irq_end(unsigned id);
__attribute__((noreturn)) void psci_off(void);
__attribute__((noreturn)) void psci_reset(void);

/* ---- load.c: loads /init from the ROM's cpio image ------------------- */
const void *cpio_find(const char *name, uint64_t *size);
void load_init(void);                   /* never returns */

/* ---- The few C library functions the HAL needs ----------------------- */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strstr(const char *h, const char *n);

#endif /* !__ASSEMBLER__ */
#endif
