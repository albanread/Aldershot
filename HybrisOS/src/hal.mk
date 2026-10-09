# hal.mk: builds the HAL (HybrisOS) for BOX on QEMU's virt machine. BOX's
# Makefile includes this file.
#
#   make hal          builds build/hal/Image, a kernel image QEMU boots with -kernel
#   run/run-hal.sh    boots it, with build/aarch64/initramfs.cpio as the ROM
#
# The HAL is freestanding AArch64 C and assembler, built with the pinned
# LLVM. It has no C library, because the HAL is the layer beneath one. The
# compiler uses the general registers only, so the box's floating-point
# registers pass through the HAL untouched.

HAL_DIR := hal
HAL_OUT := build/hal
HAL_LLVM ?= $(abspath .cache/llvm/bin)
HAL_CC := $(HAL_LLVM)/clang --target=aarch64-none-elf
HAL_LD := $(HAL_LLVM)/ld.lld
HAL_OBJCOPY ?= $(firstword $(wildcard $(HAL_LLVM)/llvm-objcopy /opt/homebrew/opt/llvm/bin/llvm-objcopy) llvm-objcopy)
HAL_CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -ffreestanding -mgeneral-regs-only \
    -fno-pic -fno-pie -mcmodel=small -fno-stack-protector -fno-asynchronous-unwind-tables \
    -mno-outline-atomics -ffile-prefix-map=$(CURDIR)/=rosgd/
# lwIP (fetched by deps/get-lwip.sh) is built with the HAL's own
# configuration (hal/lwip) and the few C library headers it needs.
LWIP_DIR := .cache/lwip-2.2.1
LWIP_INC := -I$(HAL_DIR)/lwip -I$(HAL_DIR)/lwip/libc -I$(LWIP_DIR)/src/include
LWIP_SRCS := core/init.c core/def.c core/dns.c core/inet_chksum.c core/ip.c core/mem.c core/memp.c \
    core/netif.c core/pbuf.c core/raw.c core/stats.c core/sys.c core/tcp.c core/tcp_in.c \
    core/tcp_out.c core/timeouts.c core/udp.c core/ipv4/acd.c core/ipv4/dhcp.c core/ipv4/etharp.c \
    core/ipv4/icmp.c core/ipv4/ip4.c core/ipv4/ip4_addr.c core/ipv4/ip4_frag.c netif/ethernet.c
LWIP_OBJS := $(patsubst %.c,$(HAL_OUT)/lwip/%.o,$(LWIP_SRCS))
LWIP_CFLAGS := $(filter-out -Werror -Wextra,$(HAL_CFLAGS)) -Wno-unused-parameter $(LWIP_INC)

$(LWIP_DIR)/src/core/tcp.c:
	sh deps/get-lwip.sh
$(HAL_OUT)/lwip/%.o: $(LWIP_DIR)/src/%.c $(HAL_DIR)/lwip/lwipopts.h $(HAL_DIR)/lwip/arch/cc.h | $(LWIP_DIR)/src/core/tcp.c
	@mkdir -p $(@D)
	$(HAL_CC) $(LWIP_CFLAGS) -c $< -o $@

HAL_SRCS := boot.S vectors.S fpsimd.S main.c uart.c dtb.c mm.c vm.c trap.c syscall.c signal.c thread.c timer.c ramfb.c virtio.c ninep.c ramfs.c vfs.c net.c socket.c proc.c sound.c gpu.c process.c tty.c smp.c unix.c load.c stats.c vdso/blob.S
HAL_OBJS := $(patsubst %,$(HAL_OUT)/%.o,$(HAL_SRCS)) $(LWIP_OBJS)

$(HAL_OUT)/%.S.o: $(HAL_DIR)/%.S $(HAL_DIR)/hal.h $(HAL_DIR)/hal.mk
	@mkdir -p $(@D)
	$(HAL_CC) $(HAL_CFLAGS) -c $< -o $@
$(HAL_OUT)/%.c.o: $(HAL_DIR)/%.c $(HAL_DIR)/hal.h $(HAL_DIR)/hal.mk $(HAL_DIR)/lwip/lwipopts.h | $(LWIP_DIR)/src/core/tcp.c
	@mkdir -p $(@D)
	$(HAL_CC) $(HAL_CFLAGS) $(LWIP_INC) -c $< -o $@

# The vDSO (vdso/vdso.c) is a shared object of one page. Its one symbol is
# versioned and placed in a SysV hash table, which is where musl's
# __vdsosym looks for it. It has one load segment (vdso.lds), and the
# linker strips it as it links it. Stripping it afterwards moves the file
# offsets away from the addresses, and so moves the base that musl
# calculates.
$(HAL_OUT)/vdso.so: $(HAL_DIR)/vdso/vdso.c $(HAL_DIR)/vdso/vdso.ver $(HAL_DIR)/vdso/vdso.lds $(HAL_DIR)/hal.mk
	@mkdir -p $(@D)
	$(HAL_CC) --target=aarch64-linux-gnu -O2 -fPIC -ffreestanding -nostdlib -shared -fuse-ld=lld \
	    -mgeneral-regs-only -fno-stack-protector -fno-asynchronous-unwind-tables \
	    -Wl,--hash-style=sysv -Wl,--version-script=$(HAL_DIR)/vdso/vdso.ver -Wl,-soname=linux-vdso.so.1 \
	    -Wl,-T,$(HAL_DIR)/vdso/vdso.lds -Wl,-z,max-page-size=16 -Wl,-z,norelro -Wl,--build-id=none -Wl,--strip-all \
	    -o $@ $<
	@test $$(wc -c < $@) -le 4096 || { echo "vdso.so is over a page"; exit 1; }
$(HAL_OUT)/vdso/blob.S.o: $(HAL_OUT)/vdso.so
$(HAL_OUT)/vdso/blob.S.o: HAL_CFLAGS += -DVDSO_SO='"$(HAL_OUT)/vdso.so"'

$(HAL_OUT)/hal.elf: $(HAL_OBJS) $(HAL_DIR)/link.ld
	$(HAL_LD) -T $(HAL_DIR)/link.ld -nostdlib -static --no-pie -z max-page-size=4096 -o $@ $(HAL_OBJS)

$(HAL_OUT)/Image: $(HAL_OUT)/hal.elf
	$(HAL_OBJCOPY) -O binary $< $@

hal: $(HAL_OUT)/Image
.PHONY: hal
