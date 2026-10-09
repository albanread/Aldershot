/* main.c: the HAL's entry point.  It sets up the
 * exception vectors, reads what QEMU passed in the device tree, and
 * initialises physical memory and the box's empty address space.  It then
 * opens the console as /init's first three files and starts /init itself. */
#include "hal.h"

void hal_main(uint64_t dtb_pa)
{
    __asm__ volatile("msr tpidr_el1, %0" :: "r"(&cpus[0]));     /* this core's state (smp.c) */
    trap_init();
    kprintf("\nHybrisOS 0.01, the BOX HAL\n");
    dtb_read(dtb_pa);
    kprintf("HAL: RAM %lu MB at %lx; initrd %lu KB; command line \"%s\"\n", boot.ram_size >> 20,
            boot.ram_base, (boot.initrd_end - boot.initrd_start) >> 10, boot.bootargs);
    if (boot.initrd_end <= boot.initrd_start)
        panic("no initrd: QEMU's -initrd names the ROM (build/aarch64/initramfs.cpio)");
    mm_init();
    vm_init();
    ram_init();
    mm_activate();
    kprintf("HAL: %lu MB free\n", pages_free() >> 8);
    thread_init();
    syscall_init();
    gic_init();
    virtio_init();
    net_init();
    tick_arm();
    smp_start();
    vdso_init();                                /* every process's clock_gettime page (vm.c) */
    load_init();
}
