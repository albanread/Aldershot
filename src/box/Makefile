# rosgd -- the box: a Linux kernel, and /init, which is the ROSGD runtime
# with its ROM of compiled modules.
#
#   make              build/initramfs.cpio: /init, and the ROM inside it; and
#                     !Write (ports/write), when its sources or Paige's changed
#   make hosttest     the runtime and its ROM as a process on this machine
#   make boot         boot the box under HVF headless: self-test, power off
#   make tcctest      the Mac's tcc's x32 mode against clang's, in the box (tests/capps/interop)
#   make rospdf       !PDF, the PDF viewer (ports/rospdf), for make usershare's Apps
#   make write        !Write, the word processor (ports/write, over ports/paige), for
#                     make usershare's Apps
#   make sparkfs      SparkFS, archives as directories -- Zip, Tar, Spark, LHA ... (ports/sparkfs):
#                     its modules and !SparkFS, which make builds into the ROM
#   make changefsi    !ChangeFSI, the image converter (ports/changefsi), which make builds into
#                     the ROM; its CFSI module is in /init
#   make graphtask    !GraphTask, a graphical screen in a window (apps/graphtask),
#                     which make builds into the ROM
#   make kernel       the kernel, built in the Alpine build VM
#
#   HAND=1            use T0Demo's hand translation, not the compiler's
#                     (clean first: the two share build/gen/rom_t0demo.*)
#
# Works with the GNU make 3.81 that macOS ships.  Give it -j: every source
# is its own object, and all of them compile at once.

ZIG     ?= zig
# ARCH, the machine the box is built for: x86_64, the Intel
# box, the default; or aarch64, the Apple Silicon box (make ARCH=aarch64,
# run/run-vz.sh).  It chooses the rest:
#   TARGET  what /init is compiled for: x86_64-linux-musl, aarch64-linux-musl
#   ABI     how RISC OS C in the arena is compiled and linked: x32, or
#           A64X32; what differs per ABI is in abi/$(ABI)
#   B       the build tree: build/ for x86_64, as it has always been (the
#           scripts and tests that name build/ are the Intel box's), and
#           build/aarch64/ for aarch64, so one checkout holds both
#   DEPS    where the box's third-party libraries and programs are built
#           (deps/*.sh): .cache for x86_64, .cache/aarch64 for aarch64
ARCH    ?= x86_64
ifeq ($(ARCH),x86_64)
TARGET  ?= x86_64-linux-musl
ABI     ?= x32
B   := build
DEPS    := .cache
else ifeq ($(ARCH),aarch64)
TARGET  ?= aarch64-linux-musl
ABI     ?= a64x32
B   := build/aarch64
DEPS    := .cache/aarch64
# capps.mk's A64X32 tree (P5's), named here for the rules that need it
# before capps.mk is read
CAPPS   ?= build/a64x32/capps
else
$(error ARCH must be x86_64 or aarch64)
endif
ABIDIR  ?= abi/$(ABI)
ROSASM  ?= ../rosasm/target/release/rosasm
PYTHON  ?= python3
HOSTCC  ?= cc

GEN := $(B)/gen

# The translations of RISC OS's ObjAsm (rosasm, through build.py or
# rom_unit.py) are cached by what they read (tools/gencache.py): a tree
# that has not translated a unit takes it from the cache when another did,
# so a clean build or a fresh scratch tree skips the minutes BASICVFP and
# the Wimp take.  GENCACHE= turns it off.
GENCACHE ?= $(HOME)/.cache/rosgd-gen
CACHED = $(PYTHON) tools/gencache.py --dir '$(GENCACHE)' --gen $(GEN) --out $@ --in '$^' --

CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror=implicit-function-declaration -ffp-contract=off \
          -D_GNU_SOURCE -Iinclude -I$(GEN) -Iboot -Imodules/buffers -Imodules/drmvideo -Imodules/internet -Imodules/resolver -Imodules/inetres -Imodules/dhcp -Imodules/acornssl -Imodules/urlfetcher -Imodules/acornhttp -Imodules/pty -Imodules/sshd -Imodules/lanmanfs -Imodules/smbserver -Imodules/boxtools -Imodules/bootcmds -Imodules/input -Imodules/international -Imodules/intkey -Imodules/hostfsfiler -Imodules/scrmodes -Imodules/display -Imodules/deskclock -Imodules/deskmeter -Imodules/waylandwin -Imodules/speakers -Imodules/machine \
          -Imodules/resourcefs -Imodules/messagetrans -Imodules/fileswitch -Imodules/unixbridge -Imodules/vfpsupport -Imodules/spriteextend -Imodules/colourtrans -Imodules/draw -Imodules/gdraw -Imodules/smooth -Imodules/supersample \
          -Imodules/fontmanager -Imodules/screenblanker -Imodules/spriteutils -Imodules/squash -Imodules/sharedsnd -Imodules/sound -Imodules/soundsched -Imodules/portable -Imodules/fpemulator -Imodules/basicname -Imodules/systemdevs -Imodules/wimp -Imodules/territory -Imodules/taskwindow -Imodules/sharedclib \
          -Imodules/shellcli -Imodules/modulewrap -Imodules/zlibmod -Imodules/compresspng -Imodules/compressjpeg -Imodules/mimemap -Imodules/worker -Imodules/pdriver \
          -Iports/changefsi/module -Iruntime/armrun

# /init is a static PIE -- zig links musl statically, and -fPIE makes that
# a static PIE -- which the kernel loads far above 4 GB, leaving the low
# 4 GB free for the arena (runtime/arena.c).  The checkout's path is
# mapped to rosgd in the debug information (the compile directory and the
# file names), which would otherwise differ by checkout: on AArch64 the
# debug sections' mapping symbols ($d), whose values are offsets into them,
# are in /init's .symtab (#56).  gencache.py --object's key already makes
# the path a placeholder, so a cached object is now the same whatever tree
# compiled it.
GUEST_CC := $(ZIG) cc -target $(TARGET) -fPIE -ffile-prefix-map=$(CURDIR)=rosgd

# OpenSSL, for AcornSSL (modules/acornssl), and libcurl over it, for
# AcornHTTP (modules/acornhttp): the box's, static for musl, built by
# deps/build-openssl.sh and deps/build-curl.sh; the hosted test's, the host's
# own OpenSSL 3 and libcurl.
OSSL_GUEST ?= $(DEPS)/openssl-musl
CURL_GUEST ?= $(DEPS)/curl-musl
# OpenSSH's ssh and ssh-keygen, over that OpenSSL, for *SSH (modules/pty):
# programs in the initramfs, built by deps/build-openssh.sh
SSH_GUEST  ?= $(DEPS)/openssh-musl
# ksmbd-tools, ksmbd's user-space half, for *Share (modules/smbserver):
# built by deps/build-ksmbd.sh over glib and libnl
KSMBD_GUEST ?= $(DEPS)/ksmbd-musl
# tcc, for the hosted build's BoxTools *CC (modules/boxtools): the Mac's,
# under host/, with x32-tcc beside it (tcc's x32 mode, abi/x32/tcc-x32.patch,
# for *RosBas and the interop suite) -- built by deps/build-tcc.sh, which
# makes a box tcc under box/usr too, though no box carries it now (both
# boxes' *CC is clang)
TCC_GUEST  ?= .cache/tcc
OSSL_HOST  ?= $(shell brew --prefix openssl@3 2>/dev/null || echo /usr)
# zlib, libpng and libjpeg-turbo, for the ZLib, CompressPNG and
# CompressJPEG modules (modules/zlibmod, compresspng, compressjpeg): the
# box's, static for musl, and the hosted test's, the same versions built
# for this machine -- both by deps/build-imagelibs.sh
IMG_GUEST  ?= $(DEPS)/imagelibs-musl
IMG_HOST   ?= .cache/imagelibs-host
IMG_LIBS    = $(1)/lib/libpng16.a $(1)/lib/libjpeg.a $(1)/lib/libz.a
GUEST_CC   += -I$(OSSL_GUEST)/include -I$(CURL_GUEST)/include -DCURL_STATICLIB -I$(IMG_GUEST)/include
GUEST_LIBS := $(CURL_GUEST)/lib/libcurl.a $(OSSL_GUEST)/lib/libssl.a $(OSSL_GUEST)/lib/libcrypto.a \
              $(call IMG_LIBS,$(IMG_GUEST))
HOST_SSL   := -I$(OSSL_HOST)/include -I$(IMG_HOST)/include
HOST_IMG_LIBS := $(call IMG_LIBS,$(IMG_HOST))
HOST_LIBS  := $(HOST_IMG_LIBS) $(OSSL_HOST)/lib/libssl.a $(OSSL_HOST)/lib/libcrypto.a -lcurl
# The ARM container's engine (runtime/armrun): dynarmic, built by deps/build-dynarmic.sh with our patches
# (runtime/armrun/patches) -- the box's for its machine, the hosted test's
# for this one -- and armrun.cpp, its C face, the one C++ in /init.  The
# box's engine leaves SIGSEGV and SIGBUS to the runtime's handler, which
# offers it a fault first (DYNARMIC_EXTERNAL_SIGNALS; fault.h).
ARMRUN_GUEST ?= $(DEPS)/dynarmic-musl
ARMRUN_HOST  ?= .cache/dynarmic-host
ARMRUN_PATCHES := $(wildcard runtime/armrun/patches/*.patch)
GUEST_CXX  := $(ZIG) c++ -target $(TARGET) -fPIE -ffile-prefix-map=$(CURDIR)=rosgd
HOSTCXX    ?= c++
CXXFLAGS   := -std=c++20 -O2 -g -Wall -Wextra -Iruntime/armrun -MMD -MP
GUEST_LIBS += $(ARMRUN_GUEST)/lib/libarmrun-engine.a
HOST_LIBS  += $(ARMRUN_HOST)/lib/libarmrun-engine.a -lc++
# libsmb2, LanManFS's SMB2/3 client (modules/lanmanfs): the box's and the
# hosted test's, built by deps/build-libsmb2.sh
SMB2_GUEST ?= $(DEPS)/libsmb2-musl
SMB2_HOST  ?= .cache/libsmb2-host
GUEST_CC   += -I$(SMB2_GUEST)/include
GUEST_LIBS += $(SMB2_GUEST)/lib/libsmb2.a
HOST_SSL   += -I$(SMB2_HOST)/include
HOST_LIBS  += $(SMB2_HOST)/lib/libsmb2.a

RUNTIME  := runtime/arena.c runtime/error.c runtime/dispatch.c runtime/swi.c runtime/meter.c runtime/switrace.c runtime/wimprec.c runtime/heapsort.c \
            runtime/module.c runtime/rma.c runtime/heap.c runtime/os_native.c runtime/rom.c \
            runtime/vector.c runtime/fp.c runtime/amb.c runtime/callback.c runtime/background.c \
            runtime/ticker.c runtime/sleep.c runtime/task.c runtime/environment.c runtime/capp.c runtime/fault.c \
            runtime/osbyte.c runtime/cmos.c runtime/sysinfo.c runtime/hardware.c runtime/swinames.c runtime/convert.c runtime/sysvars.c \
            runtime/expr.c runtime/readargs.c runtime/oscli.c runtime/files.c runtime/filecmds.c runtime/streams.c runtime/keyboard.c runtime/pretty.c \
            runtime/vdu/vdu.c runtime/vdu/text.c runtime/vdu/modes.c runtime/vdu/palette.c runtime/vdu/font.c runtime/vdu/ttx.c runtime/vdu/ttxfont.c runtime/vdu/plot.c runtime/vdu/shapes.c runtime/vdu/vdu5.c runtime/vdu/sprite.c runtime/vdu/sprplot.c runtime/vdu/sprout.c runtime/vdu/pointer.c runtime/vdu/vdisplay.c runtime/dynarea.c \
            runtime/armrun/bridge.c runtime/armrun/box.c runtime/armrun/fpa.c
LINUX    := platform/console.c platform/display_drm.c platform/input_evdev.c \
            platform/audio_alsa.c platform/sine.c platform/net_linux.c platform/time_power.c
HOSTED   := platform/console.c platform/hosted.c platform/sine.c platform/time_power.c
# The Apple Silicon box's harness agent: rosgd-vz has no QMP
ifeq ($(ARCH),aarch64)
LINUX    += platform/agent_vsock.c
endif
NATIVE   := modules/buffers/buffers.c modules/drmvideo/drmvideo.c modules/internet/internet.c \
            modules/resolver/resolver.c modules/inetres/inetres.c modules/inetres/ping.c modules/inetres/traceroute.c \
            modules/inetres/arp.c modules/inetres/ifconfig.c modules/inetres/route.c modules/inetres/inetstat.c \
            modules/inetres/sysctl.c modules/inetres/misc.c modules/inetres/files.c modules/inetres/digest.c \
            modules/inetres/md5.c modules/inetres/tftp.c modules/inetres/utils.c modules/dhcp/dhcp.c \
            modules/acornssl/acornssl.c modules/urlfetcher/urlfetcher.c modules/urlfetcher/parseurl.c \
            modules/acornhttp/acornhttp.c modules/acornhttp/cookie.c modules/acornhttp/dates.c modules/pty/pty.c modules/pty/term.c modules/sshd/sshd.c modules/sshd/session.c modules/sshd/relay.c modules/lanmanfs/lanmanfs.c modules/smbserver/smbserver.c modules/boxtools/boxtools.c modules/bootcmds/bootcmds.c modules/international/international.c modules/intkey/intkey.c modules/input/input.c \
            modules/resourcefs/resourcefs.c modules/messagetrans/messagetrans.c \
            modules/territory/territorymanager.c modules/territory/uk.c modules/territory/timecalc.c \
            modules/fileswitch/fileswitch.c modules/fileswitch/fsutils.c modules/fileswitch/hostfs.c modules/fileswitch/modfs.c \
            modules/hostfsfiler/hostfsfiler.c \
            modules/scrmodes/scrmodes.c modules/display/display.c modules/deskclock/deskclock.c modules/deskmeter/deskmeter.c modules/waylandwin/waylandwin.c modules/waylandwin/browser.c modules/speakers/speakers.c modules/machine/machine.c \
            modules/unixbridge/unixbridge.c \
            modules/vfpsupport/vfpsupport.c modules/vfpsupport/elementary.c \
            modules/spriteextend/spriteextend.c modules/spriteextend/scaled.c modules/spriteextend/jpeg.c \
            modules/colourtrans/colourtrans.c modules/colourtrans/match.c modules/colourtrans/dither.c modules/colourtrans/tables.c modules/colourtrans/models.c modules/colourtrans/fontcol.c \
            modules/fontmanager/fontmanager.c modules/fontmanager/catalogue.c modules/fontmanager/encoding.c \
            modules/fontmanager/fonts.c modules/fontmanager/files.c modules/fontmanager/romfonts.c \
            modules/fontmanager/arith.c modules/fontmanager/metrics.c modules/fontmanager/scan.c modules/fontmanager/colours.c modules/fontmanager/pixels.c modules/fontmanager/paint.c modules/fontmanager/vduhooks.c \
            modules/draw/draw.c modules/draw/process.c modules/draw/output.c modules/draw/arith.c \
            modules/gdraw/gdraw.c modules/gdraw/aa.c modules/smooth/smooth.c \
            modules/supersample/supersample.c modules/screenblanker/screenblanker.c modules/spriteutils/spriteutils.c \
            modules/squash/squash.c modules/sharedsnd/sharedsnd.c modules/sound/sound.c modules/sound/voices.c modules/soundsched/soundsched.c modules/squash/fast.c modules/portable/portable.c modules/fpemulator/fpemulator.c modules/basicname/basicname.c modules/systemdevs/systemdevs.c modules/wimp/wimp.c modules/wimp/task.c modules/wimp/message.c modules/wimp/window.c modules/wimp/redraw.c modules/wimp/region.c modules/wimp/screen.c modules/wimp/tools.c modules/wimp/icons.c modules/wimp/draw.c modules/wimp/font.c modules/wimp/caret.c modules/wimp/templates.c modules/wimp/input.c modules/wimp/filters.c modules/wimp/iconbar.c modules/wimp/errorbox.c modules/wimp/commands.c modules/wimp/menus.c modules/wimp/cmdwin.c modules/wimp/clipboard.c modules/wimp/sprites.c modules/wimp/surface.c modules/taskwindow/taskwindow.c \
            modules/sharedclib/sharedclib.c modules/shellcli/shellcli.c modules/modulewrap/modulewrap.c \
            modules/zlibmod/zlibmod.c modules/zlibmod/gz.c modules/compresspng/compresspng.c modules/compressjpeg/compressjpeg.c \
            modules/mimemap/mimemap.c modules/worker/worker.c modules/worker/kcheck.c \
            modules/pdriver/pdriver.c modules/pdriver/printerfs.c modules/pdriver/dest.c modules/pdriver/ipp.c \
            ports/changefsi/module/cfsi.c ports/changefsi/module/cfsi_pix.c ports/changefsi/module/cfsi_rle.c ports/changefsi/module/cfsi_codec.c
BOOT     := boot/selftest.c boot/selftest_imagefs.c boot/selftest_basicvfp.c boot/selftest_buffers.c boot/selftest_fp.c boot/selftest_int.c \
            boot/selftest_graphicsv.c boot/selftest_internet.c \
            boot/selftest_resolver.c boot/selftest_netcmds.c boot/selftest_cmos.c boot/selftest_kernelswis.c boot/selftest_vm.c boot/selftest_acornssl.c boot/selftest_fetch.c boot/selftest_pty.c boot/selftest_sshd.c boot/selftest_smb.c boot/selftest_boxtools.c boot/selftest_background.c \
            boot/selftest_ticker.c boot/selftest_tasks.c boot/selftest_amb.c boot/selftest_dynarea.c boot/selftest_wimpswis.c boot/selftest_callback.c \
            boot/selftest_taskmanager.c boot/selftest_drag.c boot/selftest_filer.c boot/selftest_pinboard.c \
            boot/selftest_armrun.c boot/selftest_shadow.c boot/selftest_filterhourglass.c boot/selftest_desktop.c boot/selftest_resfiler.c boot/selftest_sprutils.c boot/selftest_gdraw.c boot/selftest_smooth.c boot/selftest_fontfiles.c boot/selftest_squash.c boot/selftest_zlib.c boot/selftest_compresspng.c boot/selftest_compressjpeg.c boot/selftest_worker.c boot/selftest_pdriver.c boot/selftest_jpegplot.c boot/selftest_mimemap.c boot/selftest_portable.c boot/selftest_sharedsnd.c boot/selftest_sound.c \
            boot/selftest_territory.c boot/selftest_international.c boot/selftest_taskwindow.c boot/selftest_shellcli.c boot/selftest_hostfsfiler.c boot/selftest_bootcmds.c boot/selftest_display.c boot/selftest_deskclock.c boot/selftest_deskmeter.c boot/selftest_speakers.c boot/selftest_machine.c \
            boot/selftest_environment.c boot/selftest_input.c \
            boot/selftest_osbyte.c boot/selftest_convert.c boot/selftest_sysvars.c boot/selftest_args.c boot/selftest_oscli.c boot/selftest_modules.c boot/selftest_messagetrans.c boot/selftest_files.c boot/selftest_program.c boot/selftest_sclib.c boot/selftest_capps.c boot/selftest_vdu.c boot/selftest_vdisplay.c boot/vduplay.c \
            boot/hfstest.c boot/rom_contents.c
# SparkFS's modules in the ROM (ports/sparkfs/sparkfs.mk): each module's
# file, its ROM unit's name, and its slot, SPARKFS_SLOT bytes each, above
# the ColourPicker's
SPARKFS_ROM  := SparkFS:sparkfs:0xFCE00000 Zip:sparkzip:0xFCE40000 Tar:sparktar:0xFCE80000 \
                Spark:sparkspark:0xFCEC0000 Lzh:sparklzh:0xFCF00000 ARJ:sparkarj:0xFCF40000 \
                Cab:sparkcab:0xFCF80000 CPIO:sparkcpio:0xFCFC0000 Zoo:sparkzoo:0xFD000000 \
                McStuffit:sparkmcstuffit:0xFD040000 PackdDir:sparkpackddir:0xFD080000
SPARKFS_SLOT := 0x40000
SPARKFS_ROMC := $(foreach m,$(SPARKFS_ROM),$(GEN)/rom_$(word 2,$(subst :, ,$(m))).c)
SPARKFS_ROMH := $(SPARKFS_ROMC:.c=.h)
# The Toolbox's modules in the ROM (ports/toolbox/toolbox.mk): each module,
# its ROM unit's name and its slot, TOOLBOX_SLOT bytes each, above SparkFS's
TOOLBOX_ROM  := Toolbox:tbtoolbox:0xFD0C0000 Window:tbwindow:0xFD100000 ToolAction:tbtoolaction:0xFD140000 \
                Menu:tbmenu:0xFD180000 IconBar:tbiconbar:0xFD1C0000 ColourDbox:tbcolourdbox:0xFD200000 \
                ColourMenu:tbcolourmenu:0xFD240000 DCS:tbdcs:0xFD280000 FileInfo:tbfileinfo:0xFD2C0000 \
                FontDbox:tbfontdbox:0xFD300000 FontMenu:tbfontmenu:0xFD340000 PrintDbox:tbprintdbox:0xFD380000 \
                ProgInfo:tbproginfo:0xFD3C0000 SaveAs:tbsaveas:0xFD400000 Scale:tbscale:0xFD440000 \
                Gadgets:tbgadgets:0xFD480000
TOOLBOX_SLOT := 0x40000
TOOLBOX_ROMC := $(foreach m,$(TOOLBOX_ROM),$(GEN)/rom_$(word 2,$(subst :, ,$(m))).c)
TOOLBOX_ROMH := $(TOOLBOX_ROMC:.c=.h)
# Which interpreter the ROM's BASICVFP is: BASIC=tier3 (the default, the
# hand translation) or BASIC=lift (rosasm's).  The rule that makes the
# twin, and what the two are, is beside the BASICVFP rules below.
BASIC  ?= tier3
T3DIR  := modules/basicvfp/tier3
T3SRC  := $(T3DIR)/substitute.py $(T3DIR)/manifest.py $(wildcard $(T3DIR)/units/*.c) $(wildcard $(T3DIR)/units/*.h)
ifeq ($(BASIC),tier3)
  BASICGEN := $(GEN)/rom_basicvfp_t3.c
else ifeq ($(BASIC),lift)
  BASICGEN := $(GEN)/rom_basicvfp.c
else
  $(error BASIC must be tier3 or lift)
endif
GENSRC   := $(GEN)/api_gen.c $(GEN)/cmos_default.c $(GEN)/rom_t0demo.c $(BASICGEN) $(GEN)/rom_blendtable.c $(GEN)/rom_itable.c $(GEN)/rom_wimp.c \
            $(GEN)/rom_desktop.c $(GEN)/rom_taskmanager.c $(GEN)/rom_dragasprite.c $(GEN)/rom_draganobj.c $(GEN)/rom_filer.c $(GEN)/rom_filerswis.c $(GEN)/rom_hourglass.c $(GEN)/rom_pinboard.c $(GEN)/rom_windowutils.c $(GEN)/rom_filtermgr.c \
            $(GEN)/rom_free.c $(GEN)/rom_resfiler.c $(GEN)/rom_fptest.c $(GEN)/rom_ctxtest.c $(GEN)/rom_inttest.c \
            $(GEN)/rom_inttest0.c $(GEN)/rom_picker.c $(GEN)/resources.c $(GEN)/typemap.c $(GEN)/intkey_uk.c $(GEN)/intkey_tables.c \
            $(GEN)/squash/cssr.c $(GEN)/squash/zssr.c $(GEN)/territory_uk.c $(GEN)/international_tables.c \
            $(SPARKFS_ROMC) $(TOOLBOX_ROMC) $(GEN)/rom_drawfile.c
# Squash's headers from the RISC OS sources (below), which the module includes
SQUASHHDRS := $(GEN)/squash/cssr.h $(GEN)/squash/zssr.h $(GEN)/squash/defs.h \
              $(GEN)/squash/debug.h $(GEN)/squash/kernel.h
HEADERS  := $(wildcard include/rosgd/*.h) boot/selftest.h runtime/vdu/vduws.h runtime/vdu/plot.h runtime/vdu/sprite.h modules/buffers/buffers.h \
            modules/drmvideo/drmvideo.h modules/internet/internet.h \
            modules/resolver/resolver.h modules/inetres/inetres.h modules/inetres/digest.h modules/dhcp/dhcp.h modules/acornssl/acornssl.h modules/urlfetcher/urlfetcher.h modules/urlfetcher/parseurl.h \
            modules/acornhttp/acornhttp.h modules/acornhttp/dates.h modules/pty/pty.h modules/sshd/sshd.h modules/lanmanfs/lanmanfs.h modules/smbserver/smbserver.h modules/boxtools/boxtools.h modules/bootcmds/bootcmds.h modules/input/input.h modules/international/international.h modules/intkey/intkey.h modules/intkey/keylayout.h \
            modules/resourcefs/resourcefs.h modules/messagetrans/messagetrans.h modules/territory/territory.h \
            modules/fileswitch/fileswitch.h modules/fileswitch/fsw.h modules/unixbridge/unixbridge.h modules/unixbridge/ubsys.h modules/hostfsfiler/hostfsfiler.h modules/scrmodes/scrmodes.h modules/display/display.h modules/deskclock/deskclock.h modules/deskmeter/deskmeter.h modules/waylandwin/waylandwin.h modules/waylandwin/wwin.h modules/vfpsupport/vfpsupport.h modules/vfpsupport/vfpmath.h modules/vfpsupport/elementary.h modules/spriteextend/sprext.h modules/spriteextend/spriteextend.h modules/colourtrans/ct.h modules/colourtrans/colourtrans.h modules/fontmanager/fm.h modules/fontmanager/fontmanager.h modules/draw/draw.h modules/draw/drawmodule.h modules/gdraw/gd.h modules/gdraw/gdraw.h modules/smooth/smooth.h modules/supersample/supersample.h modules/screenblanker/screenblanker.h modules/spriteutils/spriteutils.h modules/squash/squash.h modules/zlibmod/zlibmod.h modules/compresspng/compresspng.h modules/compressjpeg/compressjpeg.h modules/mimemap/mimemap.h modules/worker/worker.h modules/sharedsnd/sharedsnd.h modules/sound/sound.h modules/soundsched/soundsched.h modules/squash/sq.h modules/portable/portable.h modules/fpemulator/fpemulator.h modules/basicname/basicname.h modules/systemdevs/systemdevs.h modules/wimp/wimp.h \
            ports/changefsi/module/cfsi.h modules/taskwindow/taskwindow.h modules/sharedclib/sharedclib.h modules/shellcli/shellcli.h modules/modulewrap/modulewrap.h $(GEN)/modwrap_edit.h $(GEN)/modwrap_draw.h $(GEN)/modwrap_paint.h $(GEN)/modwrap_fileract.h $(GEN)/romapps_stamps.h $(SQUASHHDRS) $(GEN)/rosgd/api.h $(GEN)/rom_t0demo.h $(GEN)/rom_basicvfp.h $(GEN)/rom_blendtable.h $(GEN)/rom_itable.h $(GEN)/rom_wimp.h modules/speakers/speakers.h modules/machine/machine.h modules/worker/kcheck.h modules/pdriver/pdriver.h modules/pdriver/ipp.h \
            $(GEN)/rom_desktop.h $(GEN)/rom_taskmanager.h $(GEN)/rom_dragasprite.h $(GEN)/rom_draganobj.h $(GEN)/rom_filer.h $(GEN)/rom_filerswis.h $(GEN)/rom_hourglass.h $(GEN)/rom_pinboard.h $(GEN)/rom_windowutils.h $(GEN)/rom_filtermgr.h \
            $(GEN)/rom_free.h $(GEN)/rom_resfiler.h $(GEN)/rom_fptest.h $(GEN)/rom_ctxtest.h $(GEN)/rom_inttest.h \
            $(GEN)/rom_inttest0.h $(GEN)/rom_picker.h $(GEN)/capp_images.h $(GEN)/sclib_probe.h $(SPARKFS_ROMH) $(TOOLBOX_ROMH) $(GEN)/rom_drawfile.h

# !Write (ports/write): its image and what it is built from (below, by
# make write's rule)
# the engine is HERMES-Paige with ports/paige's patch, made by its build.sh
# in .cache/paige (PAIGE=dir: another tree, as it stands)
WRITE_APP := $(B)/ports/write/!Write/!RunImage,ff8
WRITE_SRCS := $(wildcard ports/write/build.sh ports/write/src/*.[ch] ports/write/tools/*.py \
                ports/write/res/!Write/* ports/paige/build.sh ports/paige/rosc/*.[ch] \
                ports/paige/PGPLATFO/* ports/paige/patches/*.patch tools/mkres.py \
                $(if $(PAIGE),$(PAIGE)/PGSOURCE/*.C $(PAIGE)/PGHLEVEL/*.C $(PAIGE)/PGPLATFO/*.C \
                $(PAIGE)/PGTXR/*.C $(PAIGE)/PGTXR/*.H $(PAIGE)/PGHEADER/*.H))

all: $(B)/initramfs.cpio $(WRITE_APP)

# ---- generated: the typed API, and the ROM image -------------------------

# RISC OS's sources, which the ROM's parts below are made from: riscos-src
# sits beside the toolchain; override BASICVPFSRC to read another.  Set
# before the rules that read it as they are made (:=).
BASICVPFSRC ?= ../../riscos-src/BCM2835/RiscOS

$(GEN)/api_gen.c: api/gen.py $(wildcard api/defs/*.toml)
	$(PYTHON) api/gen.py $(GEN)
$(GEN)/rosgd/api.h: $(GEN)/api_gen.c ;

# ColourTrans's tables and palettes (Resources:$.Resources.Colours), made
# by RISC OS's own MkTables/c/maketables on the host, as its build makes them
# (ITable reads 8desktop).
COLOURSSRC := $(BASICVPFSRC)/Sources/Video/Render/Colours
COLOURTABLES := $(GEN)/restables/Resources/Colours
$(COLOURTABLES)/Tables/8desktop,ffd: $(COLOURSSRC)/MkTables/c/maketables
	@mkdir -p $(COLOURTABLES)/Tables $(COLOURTABLES)/Palettes
	$(HOSTCC) -O2 -D__unix__=1 -x c $< -o $(GEN)/maketables
	cd $(COLOURTABLES) && $(abspath $(GEN))/maketables

# Squash's restartable compressor and decompressor: RISC OS's own C,
# Programmer/Squash's c/cssr and c/zssr (the objects its Makefile also
# builds as SquashLib), copied as they are, for the module (modules/squash)
# and for this machine's squashfile.  Their kernel.h is empty: they need
# nothing of it.  They are compiled without warnings (below).
SQUASHSRC := $(BASICVPFSRC)/Sources/Programmer/Squash
$(GEN)/squash/%.c: $(SQUASHSRC)/c/%
	@mkdir -p $(GEN)/squash
	cp $< $@
$(GEN)/squash/%.h: $(SQUASHSRC)/h/%
	@mkdir -p $(GEN)/squash
	cp $< $@
$(GEN)/squash/kernel.h:
	@mkdir -p $(GEN)/squash
	: > $@
.SECONDARY: $(GEN)/squash/cssr.c $(GEN)/squash/zssr.c

# The Desktop's banner sprites, squashed as its build squashes them (its
# Makefile's ${SQUASH} LocalRes:Sprites), by Squash's own compressor --
# c/cssr, built for this machine (tools/squashfile.c).
# The Raspberry theme's, as the Pi build's.
DESKTOPSRC := $(BASICVPFSRC)/Sources/Desktop/Desktop
DESKTOPSPRITES := $(GEN)/restables/Resources/Desktop/Sprites,fca
$(GEN)/squash/squashfile: tools/squashfile.c $(GEN)/squash/cssr.c $(SQUASHHDRS)
	$(HOSTCC) -O1 -w -I$(GEN)/squash -o $@ tools/squashfile.c $(GEN)/squash/cssr.c
$(DESKTOPSPRITES): $(GEN)/squash/squashfile $(DESKTOPSRC)/Resources/UK/Raspberry/Sprites,ff9
	@mkdir -p $(dir $@)
	$(GEN)/squash/squashfile $(DESKTOPSRC)/Resources/UK/Raspberry/Sprites,ff9 $@ ff9

# The ROM's applications, as its build makes them (tools/mkromapps.py).
# BASIC's (BuildSys's BasicApp): Resources:$.Apps.!Chars, which the Apps
# icon opens, and Resources:$.Resources.Chars, the program BasCrunched
# (tools/bascrunch.py) and its Messages with their version.  C's (CApp):
# Resources:$.Apps.!Edit, its ROM !Boot, !Help and !Run -- the !Run with
# one line changed, Export's for Edit$Tokenise and Edit$Detokenise -1 --
# and Resources:$.Resources.Edit, Export, Messages (its version) and
# Templates, as RISC OS 5.30's ROM has them, and ROSGD's addition, the
# program: Edit's x32 image (capps.mk's, make capps's objects linked by
# roscc) as !RunImage, which the native !Edit module's *Desktop_Edit runs
# (modules/modulewrap), where 5.30 has the program as that module.  Draw's
# (StdTools and ModuleLibs, an older C application's Makefile): its
# resources rule's files -- Resources:$.Apps.!Draw, its ROM !Boot, !Help and
# ROM !Run, RISC OS's byte for byte; Resources:$.Resources.Draw, Messages
# (its version) and Templates -- its ModuleWrap's ResFiles, Sprites, and
# the program beside them, Draw's x32 image, which the native !Draw module's
# *Desktop_Draw runs.  Paint's the same way (StdTools and ModuleLibs): its
# resources rule's Resources:$.Apps.!Paint -- !Boot, !Help, ROM !Run -- and
# Resources:$.Resources.Paint -- Messages (its version), Sprites and
# Templates (its ResFiles registers nothing) -- and Paint's image as
# !RunImage, which the native !Paint module's *Desktop_Paint runs.  Dated
# as their sources, which romapps_stamps.h records for the self-test (the
# dates are the files' in riscos-src, so as it was unpacked).  Help and
# Alarm are not here yet.  Filer_Action (Desktop/FilerAct: CModule with
# MODULEWRAP, ModuleWrap's FilerAct shape) is no application: its files are
# Resources:$.Resources.FilerAct, Messages (CmdHelp appended) and Templates
# as CModule's resources rule puts them in the Messages module, and its x32
# image as !RunImage, which the native Filer_Action module's *Filer_Action
# runs when the Filer starts a copy, move, delete, count and the rest.
#
# The images are capps.mk's edit-image-built, draw-image-built,
# paint-image-built and fileract-image-built, sub-makes that run each time,
# after rlib.a's (rlib-archive-built), one after the other, and before make
# capps-apps's (which links Edit's, Draw's, Paint's and Filer_Action's
# objects too), so no two sub-makes
# build one object at once under make -j; the copies here, and so the ROM,
# change only when an image does.
ROMAPPSSRC := $(BASICVPFSRC)/Sources
ROMAPPS := Apps/Chars Apps/EditApp Apps/Draw Apps/Paint Desktop/FilerAct
ROMAPPSDIR := $(GEN)/romapps
ROMAPP_EDIT := $(GEN)/romapps-images/Edit,ff8
ROMAPP_DRAW := $(GEN)/romapps-images/Draw,ff8
ROMAPP_PAINT := $(GEN)/romapps-images/Paint,ff8
ROMAPP_FILERACT := $(GEN)/romapps-images/FilerAct,ff8
$(ROMAPP_EDIT): edit-image-built
	@mkdir -p $(@D)
	@cmp -s '$(EDIT_IMAGE)' $@ || cp '$(EDIT_IMAGE)' $@
$(ROMAPP_DRAW): draw-image-built
	@mkdir -p $(@D)
	@cmp -s '$(DRAW_IMAGE)' $@ || cp '$(DRAW_IMAGE)' $@
$(ROMAPP_PAINT): paint-image-built
	@mkdir -p $(@D)
	@cmp -s '$(PAINT_IMAGE)' $@ || cp '$(PAINT_IMAGE)' $@
$(ROMAPP_FILERACT): fileract-image-built
	@mkdir -p $(@D)
	@cmp -s '$(FILERACT_IMAGE)' $@ || cp '$(FILERACT_IMAGE)' $@
$(ROMAPPSDIR)/.made: tools/mkromapps.py tools/bascrunch.py $(ROMAPP_EDIT) $(ROMAPP_DRAW) $(ROMAPP_PAINT) $(ROMAPP_FILERACT) \
                     $(foreach a,$(ROMAPPS),$(shell find $(ROMAPPSSRC)/$(a)/Makefile $(ROMAPPSSRC)/$(a)/VersionNum $(wildcard $(ROMAPPSSRC)/$(a)/bas $(ROMAPPSSRC)/$(a)/asm $(ROMAPPSSRC)/$(a)/s) $(ROMAPPSSRC)/$(a)/Resources -type f))
	rm -rf $(ROMAPPSDIR)
	$(PYTHON) tools/mkromapps.py --stamps $(GEN)/romapps_stamps.h --image Apps/EditApp=$(ROMAPP_EDIT) \
	    --image Apps/Draw=$(ROMAPP_DRAW) --image Apps/Paint=$(ROMAPP_PAINT) \
	    --image Desktop/FilerAct=$(ROMAPP_FILERACT) $(ROMAPPSSRC) $(ROMAPPSDIR) $(ROMAPPS)
	touch $@
$(GEN)/romapps_stamps.h: $(ROMAPPSDIR)/.made ;
# The !Edit, !Draw and !Paint modules' titles and help strings, as ModuleWrap makes
# them from each VersionNum, and their commands' help and syntax as 5.30
# prints them from each Messages
$(GEN)/modwrap_edit.h: tools/mkromapps.py $(shell find $(ROMAPPSSRC)/Apps/EditApp/Makefile \
                       $(ROMAPPSSRC)/Apps/EditApp/VersionNum $(ROMAPPSSRC)/Apps/EditApp/Resources -type f)
	@mkdir -p $(GEN)
	$(PYTHON) tools/mkromapps.py --modwrap $@ $(ROMAPPSSRC) Apps/EditApp
$(GEN)/modwrap_draw.h: tools/mkromapps.py $(shell find $(ROMAPPSSRC)/Apps/Draw/Makefile $(ROMAPPSSRC)/Apps/Draw/VersionNum \
                       $(ROMAPPSSRC)/Apps/Draw/asm $(ROMAPPSSRC)/Apps/Draw/Resources -type f)
	@mkdir -p $(GEN)
	$(PYTHON) tools/mkromapps.py --modwrap $@ $(ROMAPPSSRC) Apps/Draw
$(GEN)/modwrap_paint.h: tools/mkromapps.py $(shell find $(ROMAPPSSRC)/Apps/Paint/Makefile $(ROMAPPSSRC)/Apps/Paint/VersionNum \
                        $(ROMAPPSSRC)/Apps/Paint/asm $(ROMAPPSSRC)/Apps/Paint/Resources -type f)
	@mkdir -p $(GEN)
	$(PYTHON) tools/mkromapps.py --modwrap $@ $(ROMAPPSSRC) Apps/Paint
$(GEN)/modwrap_fileract.h: tools/mkromapps.py $(shell find $(ROMAPPSSRC)/Desktop/FilerAct/Makefile \
                           $(ROMAPPSSRC)/Desktop/FilerAct/VersionNum $(ROMAPPSSRC)/Desktop/FilerAct/s \
                           $(ROMAPPSSRC)/Desktop/FilerAct/Resources -type f)
	@mkdir -p $(GEN)
	$(PYTHON) tools/mkromapps.py --modwrap $@ $(ROMAPPSSRC) Desktop/FilerAct

# The ROM's files (resources/, and those made above), as one ResourceFS
# block for Messages.  $(GEN)/stronged-rom (apps/StrongED/stronged.mk) is
# StrongED for the ROM, when make stronged has built it; empty, and the
# ROM carries no StrongED, when it has not.  $(B)/ports/sparkfs/rom is
# !SparkFS (ports/sparkfs/sparkfs.mk, which make builds), Apps.!SparkFS;
# $(B)/ports/changefsi/rom is !ChangeFSI (ports/changefsi/changefsi.mk),
# Apps.!ChangeFSI.  $(B)/graphtask/rom is !GraphTask (apps/graphtask/
# graphtask.mk), Apps.!GraphTask.  $(B)/stronghelp/rom is !StrongHlp
# (ports/stronghelp/stronghelp.mk), Apps.!StrongHlp; its manuals are the
# disc's Utilities.!Manuals.
$(GEN)/resources.c: tools/mkresources.py $(shell find resources -type f) $(COLOURTABLES)/Tables/8desktop,ffd $(DESKTOPSPRITES) $(ROMAPPSDIR)/.made $(GEN)/stronged-rom/.made \
                    $(B)/ports/sparkfs/.made $(B)/ports/changefsi/.made $(B)/graphtask/.made $(B)/stronghelp/.made
	@mkdir -p $(GEN)
	$(PYTHON) tools/mkresources.py resources $(GEN)/restables --dated $(ROMAPPSDIR) --dated $(GEN)/stronged-rom \
	    --dated $(B)/ports/sparkfs/rom --dated $(B)/ports/changefsi/rom --dated $(B)/graphtask/rom \
	    --dated $(B)/stronghelp/rom $@

# The default CMOS: the team emulator's seed for a HostFS share (runtime/cmos.c).
$(GEN)/cmos_default.c: tools/mkcmosdefault.py runtime/cmos_default.bin
	@mkdir -p $(GEN)
	$(PYTHON) tools/mkcmosdefault.py runtime/cmos_default.bin $@

# HostFS's table of extensions to file types.
$(GEN)/typemap.c: tools/mktypemap.py modules/fileswitch/typemap.txt
	@mkdir -p $(GEN)
	$(PYTHON) tools/mktypemap.py modules/fileswitch/typemap.txt $@

# T0Demo, compiled: rosasm --emit c writes its ROM image and its code as C
# in one file, with a header of label addresses.  HAND=1 builds the box
# from the hand translation instead -- the compiler's golden reference --
# with the image placed by tools/romimage.py.
ifeq ($(HAND),1)
$(B)/t0demo.o: modules/t0demo/t0demo.s
	@mkdir -p $(B)
	$(ROSASM) $< -o $@ --elf --map $(B)/t0demo.map
$(B)/t0demo.map: $(B)/t0demo.o ;

$(GEN)/rom_t0demo.c: $(B)/t0demo.o $(B)/t0demo.map tools/romimage.py
	$(PYTHON) tools/romimage.py t0demo $(B)/t0demo.o $(B)/t0demo.map 0xFC010000 $(GEN)
MODULES := modules/t0demo/t0demo.c
else
$(GEN)/rom_t0demo.c: modules/t0demo/t0demo.s $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)
	$(ROSASM) $< --emit c --rom-base 0xFC010000 --swis $(GEN)/native_swis.txt --swi-regs $(GEN)/swi_regs.txt -o $@
MODULES :=
endif
$(GEN)/rom_t0demo.h: $(GEN)/rom_t0demo.c ;

# BASICVFP: RISC OS 5.31's BASIC, the VFP build, from the ROOL sources
# (BASICVPFSRC, set above).
# The CPUs BASIC's '[' inline assembler speaks besides ARM (asmlib): both
# x64 and a64 (the default), one of them, or none for the ROM's ARM
# assembler alone.  The driver is always built; the encoders are chosen.
BASICASM ?= both
ASMLIB   := modules/basicvfp/asmlib
ifeq ($(BASICASM),x64)
  ASMSRC   := $(ASMLIB)/asmx64.c
  ASMDEFS  := -DBASICASM_HAVE_X64=1 -DBASICASM_HAVE_A64=0
else ifeq ($(BASICASM),a64)
  ASMSRC   := $(ASMLIB)/asma64.c $(ASMLIB)/asma64_mv.c $(ASMLIB)/asma64_tab.c
  ASMDEFS  := -DBASICASM_HAVE_X64=0 -DBASICASM_HAVE_A64=1
else ifeq ($(BASICASM),both)
  ASMSRC   := $(ASMLIB)/asmx64.c $(ASMLIB)/asma64.c $(ASMLIB)/asma64_mv.c $(ASMLIB)/asma64_tab.c
  ASMDEFS  := -DBASICASM_HAVE_X64=1 -DBASICASM_HAVE_A64=1
else ifeq ($(BASICASM),none)
  ASMSRC   :=
  ASMDEFS  := -DBASICASM_HAVE_X64=0 -DBASICASM_HAVE_A64=0
else
  $(error BASICASM must be x64, a64, both or none)
endif
ASMSRC += $(ASMLIB)/asmeval.c $(ASMLIB)/basicasm.c
# What '[' assembles by default: the CPU its code runs on -- the box's
# TARGET, the hosted build's machine -- x86-64 or AArch64 (ARM where it is
# neither); *BasicAsmCPU X64|A64|ARM switches, for cross assembly.
asmcpu = $(if $(filter x86_64%,$(1)),BASICASM_CPU_X64,$(if $(filter aarch64% arm64%,$(1)),BASICASM_CPU_A64,BASICASM_CPU_ARM))
GUEST_ASMCPU := $(call asmcpu,$(TARGET))
HOST_ASMCPU  := $(call asmcpu,$(shell uname -m))
$(GEN)/rom_basicvfp.c: $(BASICVPFSRC)/Sources/Programmer/BASIC/s/BASICVFP modules/basicvfp/VFPData modules/basicvfp/build.py modules/basicvfp/patch-basicasm.py $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)
	$(CACHED) $(PYTHON) modules/basicvfp/build.py $(BASICVPFSRC) $(ROSASM) $@ \
	    ';;' $(PYTHON) modules/basicvfp/patch-basicasm.py $@
$(GEN)/rom_basicvfp.h: $(GEN)/rom_basicvfp.c ;

# Which interpreter the ROM's BASICVFP is (BASIC=tier3, the default, or
# BASIC=lift):
#
#   lift    rosasm's translation of the ObjAsm, as above -- every
#           statement an emulated register machine.
#   tier3   the same translation with BBC BASIC hand-translated into C
#           substituted into it, function by function and statement by
#           statement, until the translation was all replaced
#           (basic-tier3; modules/basicvfp/tier3/README.md).  The twin is
#           built here, from the lift the rules above emit: substitute.py
#           applies manifest.py to it -- the hand bodies at their labels,
#           the dispatcher and FACTOR swapped for the hand ones, the
#           replaced translation deleted -- and the units are appended, as
#           one translation unit (the shape the gates were run in).
#
# The twin is NOT committed: it is made from the lift, which is made from
# the ROOL sources, so the chain is reproducible from the two.  Which one a
# binary carries is in its symbols: basicvfp_hand_DISPAT and
# basicvfp_hand_FACTOR present, basicvfp_FACTOR_lift absent, is the twin.
# The choice, in a file the binaries depend on: both objects can be there
# at once, so without this a `make BASIC=lift` after a tier 3 build would
# find the binary newer than the lift's object and keep it -- the gate
# would run the interpreter it was not asked for (basic-tier3's rig calls
# that vacuity, and checks the symbols for the same reason).
BASICSTAMP := $(B)/basic.flavour
.PHONY: basic-flavour
$(BASICSTAMP): basic-flavour
	@mkdir -p $(B) && echo '$(BASIC)' | cmp -s - $@ || echo '$(BASIC)' > $@

$(GEN)/rom_basicvfp_t3.c: $(GEN)/rom_basicvfp.c $(T3SRC)
	BASIC_T3_ROOT=$(B) $(PYTHON) $(T3DIR)/substitute.py
	@mv $@ $@.tmp && printf '\n/* ---- the hand units (%s/units), appended: one translation unit, as the gates ran it; the translation they replaced is $(GEN)/rom_basicvfp.c ---- */\n\n' $(T3DIR) >> $@.tmp \
	    && cat $(T3DIR)/units/*.c >> $@.tmp && mv $@.tmp $@
	@grep -c basicvfp_hand_ $@ | xargs printf '$@: %s hand lines\n'

# BASICVFP is the only BASIC in the ROM (the user's decision, 27 September
# 2026): *BASIC is an alias for it (runtime/sysvars.c).  BASIC105, the
# 5-byte BASIC V, is not built; modules/basicvfp/build.py still makes it
# (BASIC105 0xFC200000) if it is wanted again.

# BlendTable, compiled with the corpus sweep's flags and headers.
BLENDSRC := $(BASICVPFSRC)/Sources/Video/Render/BlendTable
$(GEN)/rom_blendtable.c: $(BLENDSRC)/s/Module $(BLENDSRC)/s/GetAll ../rosasm/tools/rom_unit.py $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)
	$(CACHED) $(PYTHON) ../rosasm/tools/rom_unit.py $(BASICVPFSRC) Sources/Video/Render/BlendTable/s/GetAll 0xFC300000 $(GEN) $(ROSASM) $@
$(GEN)/rom_blendtable.h: $(GEN)/rom_blendtable.c ;

# The keyboard layouts: IntKey's own keygen, built for this machine from the
# RISC OS sources, turns a layout's text (Internat/IntKey/layout) into
# tables; tools/keylayout.py makes C of them, and of the tables every layout
# shares (IntKeyBody, Accents, the International module's Latin1).
INTKEYSRC := $(BASICVPFSRC)/Sources/Internat/IntKey
KEYHDR    := $(BASICVPFSRC)/Sources/Programmer/HdrSrc/hdr/Keyboard
$(GEN)/keygen/keygen: $(INTKEYSRC)/c/keygen $(INTKEYSRC)/c/unicdata $(INTKEYSRC)/c/throwback $(KEYHDR) tools/keylayout.py
	$(PYTHON) tools/keylayout.py keygen-src $(INTKEYSRC) $(BASICVPFSRC)/Sources/Lib/UnicodeLib/h $(KEYHDR) $(GEN)/keygen
	$(HOSTCC) -O1 -w -DUNIX -I$(GEN)/keygen/inc -I$(GEN)/keygen -o $@ $(GEN)/keygen/keygen.c $(GEN)/keygen/unicdata.c $(GEN)/keygen/throwback.c
$(GEN)/keygen/UK.s: $(GEN)/keygen/keygen $(INTKEYSRC)/layout/UK
	cd $(INTKEYSRC) && $(abspath $(GEN)/keygen/keygen) layout/UK $(abspath $@)
$(GEN)/intkey_uk.c: $(GEN)/keygen/UK.s $(KEYHDR) tools/keylayout.py
	$(PYTHON) tools/keylayout.py layout $< $(KEYHDR) uk 1 $@
$(GEN)/intkey_tables.c: $(INTKEYSRC)/Source/IntKeyBody $(INTKEYSRC)/Source/Accents $(KEYHDR) tools/keylayout.py
	@mkdir -p $(GEN)
	$(PYTHON) tools/keylayout.py tables $(INTKEYSRC) $(KEYHDR) $@

# The UK territory's values and tables, compiled to C from its ObjAsm
# source (TerritoryModule's s/UK and the tables every territory shares,
# s/Tables) by tools/mkterritory.py: RISC OS's own, byte for byte.
TERRITORYSRC := $(BASICVPFSRC)/Sources/Internat/Territory/TerritoryModule
HDRSRC       := $(BASICVPFSRC)/Sources/Programmer/HdrSrc/hdr
$(GEN)/territory_uk.c: tools/mkterritory.py $(TERRITORYSRC)/s/UK $(TERRITORYSRC)/s/Tables $(HDRSRC)/Countries
	@mkdir -p $(GEN)
	$(PYTHON) tools/mkterritory.py $(TERRITORYSRC) $(HDRSRC) UK $@

# The International module's tables -- the names, the countries' alphabets,
# the alphabets' UCS tables and the system font's glyphs -- from its ObjAsm
# source (Internat/Inter's s/InterBody, s/InterFonts, s/UCSTables) by
# tools/mkinternational.py: RISC OS's own, byte for byte.
INTERSRC := $(BASICVPFSRC)/Sources/Internat/Inter
$(GEN)/international_tables.c: tools/mkinternational.py $(INTERSRC)/s/InterBody $(INTERSRC)/s/InterFonts $(INTERSRC)/s/UCSTables $(HDRSRC)/Countries
	@mkdir -p $(GEN)
	$(PYTHON) tools/mkinternational.py $(INTERSRC) $(HDRSRC) $@

# ITable (InverseTable), compiled from ObjAsm with the corpus sweep's flags;
# its one import from AsmUtils made a routine of its own (tools/itable_source.py).
ITABLESRC := $(BASICVPFSRC)/Sources/Video/Render/Fonts/ITable
$(GEN)/rom_itable.c: $(ITABLESRC)/s/ITable tools/itable_source.py ../rosasm/tools/rom_unit.py $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)/itable
	$(PYTHON) tools/itable_source.py $(ITABLESRC)/s/ITable $(GEN)/itable/ITable
	ROM_UNIT_SOURCE=$(GEN)/itable/ITable $(CACHED) $(PYTHON) ../rosasm/tools/rom_unit.py $(BASICVPFSRC) Sources/Video/Render/Fonts/ITable/s/ITable 0xFC500000 $(GEN) $(ROSASM) $@
$(GEN)/rom_itable.h: $(GEN)/rom_itable.c ;

# The Window Manager, compiled from ObjAsm with the corpus sweep's flags.
# Its task switch is the kernel's CallBack and callbackpoll's return to user
# mode, which the compiler makes ros_user_return (runtime/callback.c).
WIMPSRC := $(BASICVPFSRC)/Sources/Desktop/Wimp
$(GEN)/rom_wimp.c: $(wildcard $(WIMPSRC)/s/*) ../rosasm/tools/rom_unit.py $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)
	$(CACHED) $(PYTHON) ../rosasm/tools/rom_unit.py $(BASICVPFSRC) Sources/Desktop/Wimp/s/Wimp 0xFC400000 $(GEN) $(ROSASM) $@
$(GEN)/rom_wimp.h: $(GEN)/rom_wimp.c ;

# The desktop's modules, compiled from ObjAsm with the corpus sweep's
# flags, each at its own base; in rom_contents.c they start in the Pi
# ROM's order.

# The Desktop -- *Desktop, the Wimp task that starts the others -- and
# WindowUtils, a module that is only its name and version, for RMEnsure
# (its symbols are wimputils2_*: rosasm names a unit after its source).
WINDOWUTILSSRC := $(BASICVPFSRC)/Sources/Desktop/WimpUtils
$(GEN)/rom_desktop.c: $(wildcard $(DESKTOPSRC)/s/*) $(DESKTOPSRC)/VersionASM ../rosasm/tools/rom_unit.py $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)
	$(CACHED) $(PYTHON) ../rosasm/tools/rom_unit.py $(BASICVPFSRC) Sources/Desktop/Desktop/s/Desktop 0xFC900000 $(GEN) $(ROSASM) $@
$(GEN)/rom_desktop.h: $(GEN)/rom_desktop.c ;
$(GEN)/rom_windowutils.c: $(WINDOWUTILSSRC)/s/WimpUtils2 $(WINDOWUTILSSRC)/VersionASM ../rosasm/tools/rom_unit.py $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)
	$(CACHED) $(PYTHON) ../rosasm/tools/rom_unit.py $(BASICVPFSRC) Sources/Desktop/WimpUtils/s/WimpUtils2 0xFC980000 $(GEN) $(ROSASM) $@
$(GEN)/rom_windowutils.h: $(GEN)/rom_windowutils.c ;

# The Task Manager (Desktop/Switcher); its symbols are switcher_*, after
# its source, s/Switcher.
SWITCHERSRC := $(BASICVPFSRC)/Sources/Desktop/Switcher
$(GEN)/rom_taskmanager.c: $(SWITCHERSRC)/s/Switcher $(SWITCHERSRC)/VersionASM ../rosasm/tools/rom_unit.py $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)
	$(CACHED) $(PYTHON) ../rosasm/tools/rom_unit.py $(BASICVPFSRC) Sources/Desktop/Switcher/s/Switcher 0xFC600000 $(GEN) $(ROSASM) $@
$(GEN)/rom_taskmanager.h: $(GEN)/rom_taskmanager.c ;

# DragASprite and DragAnObject, the Wimp's drag helpers.  DragASprite's
# ROM source is s/Gets, as its Makefile says; while a drag runs the Wimp
# calls its UnPlot, Plot and Move, whose addresses it gave in the drag block.
DRAGSPRSRC := $(BASICVPFSRC)/Sources/Desktop/DragASprit
$(GEN)/rom_dragasprite.c: $(wildcard $(DRAGSPRSRC)/s/*) $(DRAGSPRSRC)/hdr/DragASprit ../rosasm/tools/rom_unit.py $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)
	$(CACHED) $(PYTHON) ../rosasm/tools/rom_unit.py $(BASICVPFSRC) Sources/Desktop/DragASprit/s/Gets 0xFCA00000 $(GEN) $(ROSASM) $@
$(GEN)/rom_dragasprite.h: $(GEN)/rom_dragasprite.c ;
DRAGOBJSRC := $(BASICVPFSRC)/Sources/Desktop/DragAnObj
$(GEN)/rom_draganobj.c: $(DRAGOBJSRC)/s/DragAnObj $(DRAGSPRSRC)/hdr/DragASprit ../rosasm/tools/rom_unit.py $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)
	$(CACHED) $(PYTHON) ../rosasm/tools/rom_unit.py $(BASICVPFSRC) Sources/Desktop/DragAnObj/s/DragAnObj 0xFCA80000 $(GEN) $(ROSASM) $@
$(GEN)/rom_draganobj.h: $(GEN)/rom_draganobj.c ;

# The Filer.  It is a Wimp task started with the desktop (Service_StartWimp,
# *Desktop_Filer); until then it has a small workspace and its *commands,
# and queues nothing.  Its resources are Resources:$.Resources.Filer.
FILERSRC := $(BASICVPFSRC)/Sources/Desktop/Filer
$(GEN)/rom_filer.c: $(wildcard $(FILERSRC)/s/*) ../rosasm/tools/rom_unit.py $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)
	$(CACHED) $(PYTHON) ../rosasm/tools/rom_unit.py $(BASICVPFSRC) Sources/Desktop/Filer/s/Filer 0xFC700000 $(GEN) $(ROSASM) $@
$(GEN)/rom_filer.h: $(GEN)/rom_filer.c ;

# FilerSWIs: FilerAction_SendSelectedDirectory, _SendSelectedFile and
# _SendStartOperation, which pack a Filer_Action task's work into messages.
FILERSWISSRC := $(BASICVPFSRC)/Sources/Desktop/FilerSWIs
$(GEN)/rom_filerswis.c: $(FILERSWISSRC)/s/FilerSWIs ../rosasm/tools/rom_unit.py $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)
	$(CACHED) $(PYTHON) ../rosasm/tools/rom_unit.py $(BASICVPFSRC) Sources/Desktop/FilerSWIs/s/FilerSWIs 0xFC780000 $(GEN) $(ROSASM) $@
$(GEN)/rom_filerswis.h: $(GEN)/rom_filerswis.c ;

# The Hourglass.  Its source is s/Module; the copy, unchanged, names the
# unit hourglass (rosasm names a unit after its source).
HOURSRC := $(BASICVPFSRC)/Sources/Video/Render/Hourglass
$(GEN)/rom_hourglass.c: $(HOURSRC)/s/Module ../rosasm/tools/rom_unit.py $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)/hourglass
	cp $(HOURSRC)/s/Module $(GEN)/hourglass/Hourglass
	ROM_UNIT_SOURCE=$(GEN)/hourglass/Hourglass $(CACHED) $(PYTHON) ../rosasm/tools/rom_unit.py $(BASICVPFSRC) Sources/Video/Render/Hourglass/s/Module 0xFCB80000 $(GEN) $(ROSASM) $@
$(GEN)/rom_hourglass.h: $(GEN)/rom_hourglass.c ;

# The Pinboard: a Wimp task in a module, which offers itself to the Wimp
# at Service_StartWimp.
PINBOARDSRC := $(BASICVPFSRC)/Sources/Desktop/Pinboard
$(GEN)/rom_pinboard.c: $(wildcard $(PINBOARDSRC)/s/*) ../rosasm/tools/rom_unit.py $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)
	$(CACHED) $(PYTHON) ../rosasm/tools/rom_unit.py $(BASICVPFSRC) Sources/Desktop/Pinboard/s/Pinboard 0xFC800000 $(GEN) $(ROSASM) $@
$(GEN)/rom_pinboard.h: $(GEN)/rom_pinboard.c ;

# The Filter Manager.  It starts after the Wimp: it reads the Wimp's
# version as it starts.
FILTERSRC := $(BASICVPFSRC)/Sources/Desktop/Filter
$(GEN)/rom_filtermgr.c: $(wildcard $(FILTERSRC)/s/*) ../rosasm/tools/rom_unit.py $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)
	$(CACHED) $(PYTHON) ../rosasm/tools/rom_unit.py $(BASICVPFSRC) Sources/Desktop/Filter/s/FilterMgr 0xFCB00000 $(GEN) $(ROSASM) $@
$(GEN)/rom_filtermgr.h: $(GEN)/rom_filtermgr.c ;

# Free, the free-space windows the filers open (*ShowFree), with
# Free_Register for the filing systems that answer them; it is a Wimp task
# started with the desktop (Service_StartWimp, *Desktop_Free).  Its
# resources are Resources:$.Resources.Free.
FREESRC := $(BASICVPFSRC)/Sources/Desktop/Free
$(GEN)/rom_free.c: $(wildcard $(FREESRC)/s/*) $(FREESRC)/hdr/Free $(FREESRC)/VersionASM ../rosasm/tools/rom_unit.py $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)
	$(CACHED) $(PYTHON) ../rosasm/tools/rom_unit.py $(BASICVPFSRC) Sources/Desktop/Free/s/Free 0xFCC80000 $(GEN) $(ROSASM) $@
$(GEN)/rom_free.h: $(GEN)/rom_free.c ;

# The Resource Filer: Resources:'s Apps icon on the icon bar, a Wimp task
# the Filer starts (Service_StartFiler, *Desktop_ResourceFiler); a click
# asks the Filer to open Resources:$.Apps.
RESFILERSRC := $(BASICVPFSRC)/Sources/FileSys/ResourceFS/ResFiler
$(GEN)/rom_resfiler.c: $(RESFILERSRC)/s/ResFiler $(RESFILERSRC)/VersionASM ../rosasm/tools/rom_unit.py $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)
	$(CACHED) $(PYTHON) ../rosasm/tools/rom_unit.py $(BASICVPFSRC) Sources/FileSys/ResourceFS/ResFiler/s/ResFiler 0xFCC00000 $(GEN) $(ROSASM) $@
$(GEN)/rom_resfiler.h: $(GEN)/rom_resfiler.c ;

# FPTest: floating point, lifted to C expressions by the compiler.
$(GEN)/rom_fptest.c: modules/fptest/fptest.s $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)
	$(ROSASM) $< --emit c --rom-base 0xFC020000 --swis $(GEN)/native_swis.txt --swi-regs $(GEN)/swi_regs.txt -o $@
$(GEN)/rom_fptest.h: $(GEN)/rom_fptest.c ;

# CtxTest: the Wimp's task switch, small -- the kernel's CallBack armed as
# ExitPoll arms it, a handler ending as callbackpoll ends.
$(GEN)/rom_ctxtest.c: modules/ctxtest/ctxtest.s $(GEN)/api_gen.c $(ROSASM)
	@mkdir -p $(GEN)
	$(ROSASM) $< --emit c --rom-base 0xFC050000 --swis $(GEN)/native_swis.txt --swi-regs $(GEN)/swi_regs.txt -o $@
$(GEN)/rom_ctxtest.h: $(GEN)/rom_ctxtest.c ;


# IntTest, twice: lifted to C expressions, and as IntTest0 compiled tier 0's
# way (--no-lift), which the self-test holds it against.  The copy gives
# the second its own name.
$(GEN)/rom_inttest.c: modules/inttest/inttest.s $(ROSASM)
	@mkdir -p $(GEN)
	$(ROSASM) $< --emit c --rom-base 0xFC030000 -o $@
$(GEN)/rom_inttest.h: $(GEN)/rom_inttest.c ;
$(GEN)/inttest0.s: modules/inttest/inttest.s
	@mkdir -p $(GEN)
	cp $< $@
$(GEN)/rom_inttest0.c: $(GEN)/inttest0.s $(ROSASM)
	$(ROSASM) $< --emit c --no-lift --rom-base 0xFC040000 -o $@
$(GEN)/rom_inttest0.h: $(GEN)/rom_inttest0.c ;

# C applications (include/rosgd/capp.h): the self-test's start-up probe,
# x32 images linked at &8000 by roscc (OS/ABI 255 and the ROSGD note the
# loader requires) and carried in the self-test as bytes (tools/bin2c.py).
# zig's clang, not zig cc, which will not build x32 without PIC.  roscc is
# built from ../roscc by cargo: the toolchain's own (rustup which cargo),
# not rustup's proxy on PATH, which has no rustlib beside it (#33); else
# cargo on PATH, or rustup's stable toolchain.
CARGO ?= $(or $(shell rustup which cargo 2>/dev/null),$(shell command -v cargo 2>/dev/null),$(firstword $(wildcard $(HOME)/.rustup/toolchains/stable-*/bin/cargo)),cargo)
ROSCC ?= ../roscc/target/release/roscc
$(ROSCC): $(wildcard ../roscc/src/*.rs) ../roscc/Cargo.toml
	cd ../roscc && PATH="$(abspath $(dir $(CARGO))):$$PATH" $(CARGO) build --release --bin roscc
	@touch $@
# rosasm, which translates the ROM's ObjAsm units (the Wimp, BASICVFP, ...),
# built from ../rosasm the same way: a stale one silently mistranslates the
# ROM (#61).  cargo is incremental; the touch keeps make from asking again.
# The translations' gencache key hashes the binary, so a rebuilt rosasm that
# is byte for byte the same still takes them from the cache.  Only this
# tree's own: ROSASM= naming another rosasm is used as it is.
ifeq ($(ROSASM),../rosasm/target/release/rosasm)
$(ROSASM): $(wildcard ../rosasm/src/*.rs ../rosasm/src/bin/*.rs) ../rosasm/Cargo.toml ../rosasm/Cargo.lock
	cd ../rosasm && PATH="$(abspath $(dir $(CARGO))):$$PATH" $(CARGO) build --release --bin rosasm
	@touch $@
endif
ifeq ($(ABI),x32)
CAPP_CC := $(ZIG) clang -target x86_64-linux-gnux32 -ffreestanding -fno-pic -fno-stack-protector \
           -fno-omit-frame-pointer -fno-asynchronous-unwind-tables -O2 -Wall
$(GEN)/capps/probe.elf: tests/capps/probe.c $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -c -o $(GEN)/capps/probe.o $<
	$(ROSCC) link -o $@ $(GEN)/capps/probe.o
$(GEN)/capps/bigprobe.elf: tests/capps/probe.c $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -DBIG -c -o $(GEN)/capps/bigprobe.o $<
	$(ROSCC) link -o $@ $(GEN)/capps/bigprobe.o
# The entries probe (package R2a), linked by roscc as the probe is, and the
# stand-in for the library's and a module's code it is entered alongside:
# position-independent, linked by ld.lld at &77000000, the top of the C ROM,
# clear of the ROM library at &72200000 (it is not an application: the
# self-test maps it from its ELF segments), and copied into the RMA by the
# self-test as well.
$(GEN)/capps/entries.elf: tests/capps/entries.c $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -c -o $(GEN)/capps/entries.o $<
	$(ROSCC) link -o $@ $(GEN)/capps/entries.o
$(GEN)/capps/xlib.elf: tests/capps/xlib.c tests/capps/lib.ld
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -fPIE -c -o $(GEN)/capps/xlib.o $<
	$(ZIG) ld.lld -m elf32_x86_64 -T tests/capps/lib.ld -o $@ $(GEN)/capps/xlib.o
# An x32 C module's SharedCLibrary registration (package T1b), run as an
# application: roscc's modclib_x32.o and the stubs, roclib_x32.o
# (../roscc/tools/build-rt-x32.sh builds them, and crt0_x32.o), linked with
# a caller.
$(GEN)/capps/rt/modclib_x32.o: ../roscc/rostrt/modclib_x32.c ../roscc/rostrt/roclib_x32.s \
                               ../roscc/rostrt/crt0_x32.c ../roscc/tools/build-rt-x32.sh
	ZIG="$(ZIG)" sh ../roscc/tools/build-rt-x32.sh $(GEN)/capps/rt
$(GEN)/capps/rt/crt0_x32.o $(GEN)/capps/rt/roclib_x32.o: $(GEN)/capps/rt/modclib_x32.o ;
# A C program through the ROM library (package L1a, tests/capps/clibapp.c):
# roscc link --clib, an application's crt0_x32 and the stubs
$(GEN)/capps/clibapp.elf: tests/capps/clibapp.c $(GEN)/capps/rt/crt0_x32.o $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -c -o $(GEN)/capps/clibapp.o $<
	ROSCC_X32_RT=$(GEN)/capps/rt $(ROSCC) link --clib -o $@ $(GEN)/capps/clibapp.o
# The Wimp timed (#30; tests/wimpbench): a C program
# through the ROM library, as clibapp is built
$(GEN)/capps/wimpbench.elf: tests/wimpbench/wimpbench.c $(GEN)/capps/rt/crt0_x32.o $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -c -o $(GEN)/capps/wimpbench.o $<
	ROSCC_X32_RT=$(GEN)/capps/rt $(ROSCC) link --clib -o $@ $(GEN)/capps/wimpbench.o
$(GEN)/capps/modclib.elf: tests/capps/modclib.c $(GEN)/capps/rt/modclib_x32.o $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -c -o $(GEN)/capps/modclib.o $<
	$(ROSCC) link -o $@ $(GEN)/capps/modclib.o $(GEN)/capps/rt/modclib_x32.o $(GEN)/capps/rt/roclib_x32.o
# The C library's image (package L0), proposal-v3 appendix A.1's stand-in:
# A.1's own objects (roscc's committed fixtures: clibstand.o, its code A.1's
# byte for byte, and clibcall.o, the way the self-test calls it) linked by
# roscc --clib-image, as the ROM library is (make clib-image) -- at the top
# of the C ROM, where the self-test maps it, clear of the ROM library
CLIBSTAND := ../roscc/tests/x32
$(GEN)/capps/clibstand.elf: $(CLIBSTAND)/obj/clibstand.o $(CLIBSTAND)/obj/clibcall.o $(CLIBSTAND)/src/clibstand.json \
                            $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(ROSCC) link --clib-image --base 0x77000000 --contract $(CLIBSTAND)/src/clibstand.json \
	    --spec $(GEN)/capps/clibstand.json -o $@ $(CLIBSTAND)/obj/clibstand.o $(CLIBSTAND)/obj/clibcall.o
# A stub-only SharedCLibrary client (package R5, tests/capps/sclib/clibreg.c),
# linked with the stubs alone: its registrations, good and spoiled, and calls
# through its slots, held in the box to RISC OS 5.30's record of its AArch32
# build (tests/capps/farm/clibreg.txt, made by tests/capps/sclib/farm.py)
$(GEN)/capps/clibreg.elf: tests/capps/sclib/clibreg.c $(GEN)/capps/rt/modclib_x32.o $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -c -o $(GEN)/capps/clibreg.o $<
	$(ROSCC) link -o $@ $(GEN)/capps/clibreg.o $(GEN)/capps/rt/roclib_x32.o
# Per-task state (package R3): the self-test's pertask probe, whose handler
# the runtime finds by the memory at &8000; and the two different images of
# the baton, BatonA and BatonB, two Wimp tasks in the box (tests/capps/
# baton.py, make boxtest).
$(GEN)/capps/pertask.elf: tests/capps/pertask.c $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -c -o $(GEN)/capps/pertask.o $<
	$(ROSCC) link -o $@ $(GEN)/capps/pertask.o
$(GEN)/capps/batona.elf $(GEN)/capps/batonb.elf: $(GEN)/capps/baton%.elf: tests/capps/baton.c $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -DBATON_$(subst a,A,$(subst b,B,$*)) -c -o $(GEN)/capps/baton$*.o $<
	$(ROSCC) link -o $@ $(GEN)/capps/baton$*.o
# The fault probe (package R2b, tests/capps/faults): the same source as the
# farm's ARM build; faults.py runs it in the box against the farm's log.
# A C module's allocation (#21, tests/capps/modmalloc.c): registration as a
# module, then the library's module initialisation, then malloc -- the RMA's
$(GEN)/capps/modmalloc.elf: tests/capps/modmalloc.c $(GEN)/capps/rt/modclib_x32.o $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -c -o $(GEN)/capps/modmalloc.o $<
	$(ROSCC) link -o $@ $(GEN)/capps/modmalloc.o $(GEN)/capps/rt/modclib_x32.o $(GEN)/capps/rt/roclib_x32.o
$(GEN)/capps/faults.elf: tests/capps/faults/faults.c tests/capps/faults/sys_x32.c tests/capps/faults/sys.h $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -c -o $(GEN)/capps/faults.o tests/capps/faults/faults.c
	$(CAPP_CC) -c -o $(GEN)/capps/faults_sys.o tests/capps/faults/sys_x32.c
	$(ROSCC) link -o $@ $(GEN)/capps/faults.o $(GEN)/capps/faults_sys.o
# A Worker application kernel (tests/worker/mandelk.c):
# roscc link --kernel, position-independent (-fPIE: its constants reached
# [rip + d]), for boot/selftest_worker.c to load and run
$(GEN)/capps/mandelk.wk: tests/worker/mandelk.c $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -fPIE -fno-jump-tables -ffp-contract=off -c -o $(GEN)/capps/mandelk.o $<
	$(ROSCC) link --kernel -o $@ $(GEN)/capps/mandelk.o
$(GEN)/capp_images.h: $(GEN)/capps/probe.elf $(GEN)/capps/bigprobe.elf $(GEN)/capps/entries.elf \
                      $(GEN)/capps/xlib.elf $(GEN)/capps/modclib.elf $(GEN)/capps/clibstand.elf \
                      $(GEN)/capps/clibreg.elf tests/capps/farm/clibreg.txt $(GEN)/capps/clibapp.elf \
                      $(GEN)/capps/pertask.elf $(GEN)/capps/faults.elf $(GEN)/capps/modmalloc.elf \
                      $(GEN)/capps/mandelk.wk tools/bin2c.py
	$(PYTHON) tools/bin2c.py $@ capp_probe=$(GEN)/capps/probe.elf capp_bigprobe=$(GEN)/capps/bigprobe.elf \
	    capp_entries=$(GEN)/capps/entries.elf capp_xlib=$(GEN)/capps/xlib.elf \
	    capp_modclib=$(GEN)/capps/modclib.elf capp_clibstand=$(GEN)/capps/clibstand.elf \
	    capp_clibreg=$(GEN)/capps/clibreg.elf capp_clibreg_farm=tests/capps/farm/clibreg.txt \
	    capp_clibapp=$(GEN)/capps/clibapp.elf capp_pertask=$(GEN)/capps/pertask.elf \
	    capp_faults=$(GEN)/capps/faults.elf capp_modmalloc=$(GEN)/capps/modmalloc.elf \
	    capp_mandelk=$(GEN)/capps/mandelk.wk
else
# A64X32: the probes the A64X32 contract has so far (capp.h),
# compiled as capps.mk compiles A64X32 C -- the pinned clang, flags.mk's
# target, x18 reserved, the division trap -- and linked by roscc's A64X32
# link, at &8000 with the ROSGD note.  The x32 probes that are x86 code
# (the entries, library, registration and fault probes) have no A64X32
# twins yet: boot/selftest_capps.c runs these and clibapp on AArch64.
CAPP_CC = $(LLVM_BIN)/clang $(A64X32_TARGET) $(A64X32_SAFETY) -ffreestanding -nostdinc -fno-stack-protector \
          -fno-omit-frame-pointer -fno-asynchronous-unwind-tables -O2 -Wall
$(GEN)/capps/probe.elf: tests/capps/probe.c $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -c -o $(GEN)/capps/probe.o $<
	$(ROSCC) link -o $@ $(GEN)/capps/probe.o
$(GEN)/capps/bigprobe.elf: tests/capps/probe.c $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -DBIG -c -o $(GEN)/capps/bigprobe.o $<
	$(ROSCC) link -o $@ $(GEN)/capps/bigprobe.o
# A C program through the ROM library (tests/capps/clibapp.c): roscc link
# --clib, crt0_a64x32 and the stubs (capps.mk's $(CAPPS)/rt).  It is RISC OS
# C, so it is compiled as capps.mk compiles a client.
$(GEN)/capps/clibapp.elf: tests/capps/clibapp.c $(CAPPS)/rt/crt0_a64x32.o $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -c -o $(GEN)/capps/clibapp.o $<
	ROSCC_A64X32_RT=$(CAPPS)/rt $(ROSCC) link --clib -o $@ $(GEN)/capps/clibapp.o
# The Wimp timed (#30; tests/wimpbench), as clibapp
$(GEN)/capps/wimpbench.elf: tests/wimpbench/wimpbench.c $(CAPPS)/rt/crt0_a64x32.o $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -c -o $(GEN)/capps/wimpbench.o $<
	ROSCC_A64X32_RT=$(CAPPS)/rt $(ROSCC) link --clib -o $@ $(GEN)/capps/wimpbench.o
# A C module's allocation (#21, tests/capps/modmalloc.c): modclib_a64x32 and
# the stubs, capps.mk's runtime objects
$(GEN)/capps/modmalloc.elf: tests/capps/modmalloc.c $(CAPPS)/rt/modclib_a64x32.o $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -c -o $(GEN)/capps/modmalloc.o $<
	$(ROSCC) link -o $@ $(GEN)/capps/modmalloc.o $(CAPPS)/rt/modclib_a64x32.o $(CAPPS)/rt/roclib_a64x32.o
# A Worker application kernel, as x32's: roscc link
# --kernel of A64X32 code, which is position-independent as it stands
$(GEN)/capps/mandelk.wk: tests/worker/mandelk.c $(ROSCC)
	@mkdir -p $(GEN)/capps
	$(CAPP_CC) -fno-jump-tables -ffp-contract=off -c -o $(GEN)/capps/mandelk.o $<
	$(ROSCC) link --kernel -o $@ $(GEN)/capps/mandelk.o
$(GEN)/capp_images.h: $(GEN)/capps/probe.elf $(GEN)/capps/bigprobe.elf $(GEN)/capps/clibapp.elf \
                      $(GEN)/capps/modmalloc.elf $(GEN)/capps/mandelk.wk tools/bin2c.py
	$(PYTHON) tools/bin2c.py $@ capp_probe=$(GEN)/capps/probe.elf capp_bigprobe=$(GEN)/capps/bigprobe.elf \
	    capp_clibapp=$(GEN)/capps/clibapp.elf capp_modmalloc=$(GEN)/capps/modmalloc.elf \
	    capp_mandelk=$(GEN)/capps/mandelk.wk
endif

# SharedCLibrary's module surface (package R5): the BASIC probe and RISC OS
# 5.30's answer to it (tests/capps/farm/sclib.txt), for the self-test, hosted
# and in the box (boot/selftest_sclib.c)
$(GEN)/sclib_probe.h: tests/capps/sclib/sclib.bas tests/capps/farm/sclib.txt tools/bin2c.py
	@mkdir -p $(GEN)
	$(PYTHON) tools/bin2c.py $@ sclib_bas=tests/capps/sclib/sclib.bas sclib_farm=tests/capps/farm/sclib.txt

# ---- /init, and the initramfs that is the ROM ----------------------------

# One object per source, the box's under build/obj/guest and the hosted
# test's under build/obj/host: make -j compiles them side by side, and an
# edit recompiles only the files that include what changed (-MMD's .d
# files).  The generated headers, and OpenSSL's for the box, come first
# (order-only).  Clean after changing BASICASM or HAND: the objects do not
# know the flags they had.
GOBJ := $(B)/obj/guest
HOBJ := $(B)/obj/host
objs = $(patsubst $(GEN)/%.c,$(1)/gen/%.o,$(filter $(GEN)/%.c,$(2))) \
       $(patsubst %.c,$(1)/%.o,$(filter-out $(GEN)/%.c,$(2)))
GUEST_OBJ := $(call objs,$(GOBJ),boot/main.c $(RUNTIME) $(LINUX) $(NATIVE) $(MODULES) $(BOOT) $(GENSRC) $(ASMSRC)) \
             $(GOBJ)/runtime/armrun/armrun.o
HOST_OBJ  := $(call objs,$(HOBJ),tests/host/host_main.c $(RUNTIME) $(HOSTED) $(NATIVE) $(MODULES) $(BOOT) $(GENSRC) $(ASMSRC)) \
             $(HOBJ)/runtime/armrun/armrun.o
GENHDRS   := $(filter $(GEN)/%,$(HEADERS))
OBJFLAGS   = $(CFLAGS) $(ASMDEFS) -I$(ASMLIB) -MMD -MP -c -o $@ $<

# The generated ROM units' objects are cached too (tools/gencache.py
# --object: by the preprocessed source and the flags): the Wimp's C alone
# is a minute of one core.  These come before the general rules below, as
# make 3.81 takes the first pattern that matches.
CACHED_OBJ = $(PYTHON) tools/gencache.py --object --dir '$(GENCACHE)' --out $@ --
$(GOBJ)/gen/rom_%.o: $(GEN)/rom_%.c | $(GENHDRS) $(GUEST_LIBS)
	@mkdir -p $(dir $@)
	$(CACHED_OBJ) $(GUEST_CC) $(OBJFLAGS)
$(HOBJ)/gen/rom_%.o: $(GEN)/rom_%.c | $(GENHDRS) $(HOST_IMG_LIBS) $(SMB2_HOST)/lib/libsmb2.a
	@mkdir -p $(dir $@)
	$(CACHED_OBJ) $(HOSTCC) -DROS_ARENA_HOSTED $(HOST_SSL) $(OBJFLAGS)
$(GOBJ)/gen/%.o: $(GEN)/%.c | $(GENHDRS) $(GUEST_LIBS)
	@mkdir -p $(dir $@)
	$(GUEST_CC) $(OBJFLAGS)
$(GOBJ)/%.o: %.c | $(GENHDRS) $(GUEST_LIBS)
	@mkdir -p $(dir $@)
	$(GUEST_CC) $(OBJFLAGS)
$(HOBJ)/gen/%.o: $(GEN)/%.c | $(GENHDRS) $(HOST_IMG_LIBS) $(SMB2_HOST)/lib/libsmb2.a
	@mkdir -p $(dir $@)
	$(HOSTCC) -DROS_ARENA_HOSTED $(HOST_SSL) $(OBJFLAGS)
$(HOBJ)/%.o: %.c | $(GENHDRS) $(HOST_IMG_LIBS) $(SMB2_HOST)/lib/libsmb2.a
	@mkdir -p $(dir $@)
	$(HOSTCC) -DROS_ARENA_HOSTED $(HOST_SSL) $(OBJFLAGS)

# Squash's C from the RISC OS sources, as it is: no warnings
$(GOBJ)/gen/squash/%.o $(HOBJ)/gen/squash/%.o: OBJFLAGS += -w

# The ARM container's FPA takes packed decimal through the runtime's own
# converters (runtime/fp.c), which the hosted engine tests do not link
$(GOBJ)/runtime/armrun/fpa.o $(HOBJ)/runtime/armrun/fpa.o: OBJFLAGS += -DROS_FPA_PACKED

# BASIC's '[' assembles the CPU each build runs on, by default
# the twin's own: the hand units' headers, which only it includes, and
# the register locals the substitution orphans -- a body whose statements
# are hand code keeps the lift's declarations, which nothing then reads
$(GOBJ)/gen/rom_basicvfp_t3.o $(HOBJ)/gen/rom_basicvfp_t3.o: OBJFLAGS += -I$(T3DIR)/units \
    -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function
$(GOBJ)/$(ASMLIB)/basicasm.o: OBJFLAGS += -DBASICASM_DEFAULT_CPU=$(GUEST_ASMCPU)
$(HOBJ)/$(ASMLIB)/basicasm.o: OBJFLAGS += -DBASICASM_DEFAULT_CPU=$(HOST_ASMCPU)

# BoxTools, hosted: the Mac's tcc, the rosasm this build uses, and *RosBas's
# programs and files as this build has them
$(HOBJ)/modules/boxtools/boxtools.o: OBJFLAGS += -DBOXTOOLS_TCC='"$(abspath $(TCC_GUEST))/host/bin/tcc"' \
    -DBOXTOOLS_ROSASM='"$(abspath $(ROSASM))"' \
    -DBOXTOOLS_ROSBAS='"$(abspath $(ROSBAS_HOST))"' -DBOXTOOLS_X32TCC='"$(abspath $(TCC_GUEST))/host/bin/x32-tcc"' \
    -DBOXTOOLS_ROSCC='"$(abspath $(ROSCC))"' -DBOXTOOLS_ROSBAS_KIT='"$(abspath $(ROSBAS_KIT))"' \
    -DBOXTOOLS_CAPPS_INC='"$(abspath $(CAPPS))/inc/C"' -DBOXTOOLS_X32_RT='"$(abspath $(GEN))/capps/rt"' \
    -DBOXTOOLS_X32_LIBTCC='"$(abspath $(TCC_GUEST))/host/lib/tcc/x32-libtcc1.a"'

# *RosBas (modules/boxtools): BBC BASIC to a RISC OS application in the box --
# rosbas (../rosbas) makes C, the box's clang compiles it, roscc links it with the
# rosbas runtime.  rosbas and roscc go in the initramfs as static x86_64
# Linux programs (musl), linked by Rust's own lld (the target is rustup's
# x86_64-unknown-linux-musl); the runtime is rbbuild.py's kit: librb.a,
# built for x32, and the headers the C includes.  Beside them, RISC OS's C
# headers as make capps stages them, and roscc's x32 start and stubs.
# The Apple Silicon box's are the same programs for aarch64 (rustup's
# aarch64-unknown-linux-musl), its kit A64X32's, compiled by clang (design
# 26 P7).  Rust's lld is the one beside the toolchain's cargo, or in the
# sysroot of the rustc beside CARGO when CARGO is rustup's proxy (#33: a
# Homebrew cargo has neither).
RUST_TARGET := $(ARCH)-unknown-linux-musl
RUST_TARGET_ENV := $(shell echo '$(RUST_TARGET)' | tr a-z- A-Z_)
RUST_LLD ?= $(firstword $(wildcard $(dir $(CARGO))../lib/rustlib/*/bin/rust-lld \
    $(shell '$(dir $(CARGO))rustc' --print sysroot 2>/dev/null)/lib/rustlib/*/bin/rust-lld))
# rust-lld (and rust-objcopy) from Rust 1.98 load libLLVM.dylib from the
# toolchain's lib, which rustc does not add for a linker given this way
MUSL_CARGO = PATH="$(abspath $(dir $(CARGO))):$$PATH" \
    DYLD_FALLBACK_LIBRARY_PATH="$(abspath $(dir $(CARGO))../lib)" \
    CARGO_TARGET_$(RUST_TARGET_ENV)_LINKER=$(RUST_LLD) \
    CARGO_TARGET_$(RUST_TARGET_ENV)_RUSTFLAGS="-C linker-flavor=ld.lld -C link-self-contained=yes" \
    $(CARGO) build --release --target $(RUST_TARGET)
ROSBAS_BOX := ../rosbas/target/$(RUST_TARGET)/release/rosbas
ROSCC_BOX := ../roscc/target/$(RUST_TARGET)/release/roscc
ROSBAS_HOST := ../rosbas/target/release/rosbas
ROSBAS_KIT := $(GEN)/rosbas-kit
ROSBAS_SRC := $(wildcard ../rosbas/src/*.rs ../rosbas/src/bin/*.rs) ../rosbas/Cargo.toml
$(ROSBAS_BOX): $(ROSBAS_SRC)
	@test -n '$(RUST_LLD)' || { echo 'no rust-lld beside CARGO=$(CARGO) or in its rustc sysroot: set CARGO to rustup'"'"'s toolchain cargo, or RUST_LLD' >&2; exit 1; }
	cd ../rosbas && $(MUSL_CARGO) --bin rosbas
	@touch $@
$(ROSCC_BOX): $(wildcard ../roscc/src/*.rs) ../roscc/Cargo.toml
	@test -n '$(RUST_LLD)' || { echo 'no rust-lld beside CARGO=$(CARGO) or in its rustc sysroot: set CARGO to rustup'"'"'s toolchain cargo, or RUST_LLD' >&2; exit 1; }
	cd ../roscc && $(MUSL_CARGO) --bin roscc
	@touch $@

# pdfsvc: the PDF writer the printer driver pipes a job through, a 64-bit
# Linux program in the initramfs beside roscc.  The hosted
# build has no initramfs, so the module is told where the Mac's build is.
PDFSVC_SRC  := $(wildcard ../pdfsvc/src/*.rs) ../pdfsvc/Cargo.toml
PDFSVC_BOX  := ../pdfsvc/target/$(RUST_TARGET)/release/pdfsvc
PDFSVC_HOST := ../pdfsvc/target/release/pdfsvc
$(PDFSVC_BOX): $(PDFSVC_SRC)
	@test -n '$(RUST_LLD)' || { echo 'no rust-lld beside CARGO=$(CARGO) or in its rustc sysroot: set CARGO to rustup'"'"'s toolchain cargo, or RUST_LLD' >&2; exit 1; }
	cd ../pdfsvc && $(MUSL_CARGO) --bin pdfsvc
	@touch $@
$(PDFSVC_HOST): $(PDFSVC_SRC)
	cd ../pdfsvc && $(CARGO) build --release --bin pdfsvc
	@touch $@
$(HOBJ)/modules/pdriver/pdriver.o: OBJFLAGS += -DPDRIVER_PDFSVC='"$(abspath $(PDFSVC_HOST))"'
$(ROSBAS_HOST): $(ROSBAS_SRC)
	cd ../rosbas && PATH="$(abspath $(dir $(CARGO))):$$PATH" $(CARGO) build --release --bin rosbas
	@touch $@
ifeq ($(ARCH),x86_64)
$(ROSBAS_KIT)/librb.a: $(wildcard ../rosbas/runtime/*.c ../rosbas/runtime/*.h ../rosbas/runtime/x32/*) \
                       $(wildcard modules/basicvfp/asmlib/*.c modules/basicvfp/asmlib/*.h) \
                       ../rosbas/tools/rbbuild.py $(GEN)/capps/rt/crt0_x32.o | clib-image-built
	rm -rf $(ROSBAS_KIT)
	$(PYTHON) ../rosbas/tools/rbbuild.py --box-kit $(ROSBAS_KIT)

# The Intel box's own clang, tcc's replacement:
# deps/build-clang-box.sh --arch x86_64 -- the same recipe, x32's flag table
# (abi/x32/flags.mk, capps.mk's) as the default triple's config.  That build
# takes most of an hour, so deps/get-clang-box.sh fetches its output (the
# private release clang-box-1, checked by SHA-256) and writes the flag table
# from this tree; CLANG_BOX_BUILD=1 builds it here instead.
CLANG_BOX_X86 ?= .cache/clang-box-x86_64
$(CLANG_BOX_X86)/usr/bin/llvm: deps/get-clang-box.sh abi/x32/flags.mk capps.mk
	sh deps/get-clang-box.sh x86_64 $(CLANG_BOX_X86)
	@touch $@
else
# A64X32's kit: the runtime compiled by the pinned clang with abi/a64x32's
# table against the A64X32 staging ($(CAPPS)), linked later with capps.mk's
# A64X32 start and stubs ($(CAPPS)/rt)
$(ROSBAS_KIT)/librb.a: $(wildcard ../rosbas/runtime/*.c ../rosbas/runtime/*.h ../rosbas/runtime/x32/include/*) \
                       $(wildcard modules/basicvfp/asmlib/*.c modules/basicvfp/asmlib/*.h) \
                       ../rosbas/tools/rbbuild.py $(ABIDIR)/flags.mk $(CAPPS)/rt/crt0_a64x32.o | clib-image-built
	rm -rf $(ROSBAS_KIT)
	ROSBAS_CLIB_INC=$(abspath $(CAPPS))/inc/C ROSCC_A64X32_RT=$(abspath $(CAPPS))/rt \
	    ROSBAS_A64X32_LLVM=$(abspath $(LLVM_BIN)) $(PYTHON) ../rosbas/tools/rbbuild.py --box-kit $(ROSBAS_KIT) --target a64x32

# The box's compiler: the patched clang and lld as one static
# aarch64-linux-musl program, with musl and compiler-rt's builtins for
# *CC --linux (deps/build-clang-box.sh, .cache/clang-box: a stamp there
# holds the script, the patch and the flag table, so it rebuilds when one
# changes and is otherwise quick)
CLANG_BOX ?= .cache/clang-box
$(CLANG_BOX)/usr/bin/llvm: deps/build-clang-box.sh $(ABIDIR)/clang-a64x32.patch $(ABIDIR)/flags.mk
	sh deps/build-clang-box.sh $(CLANG_BOX)
	@touch $@

# The Apple Silicon box's rosbas has LLVM's code generator in it (rosbas's
# llvm-box feature): it can compile a program to machine code in the box.
# It does all the plain rosbas does, so it replaces it as /usr/bin/rosbas.
# LLVM's static libraries are deps/build-llvm-libs-box.sh's
# (.cache/llvm-libs-box, about 5 minutes the first time); zig 0.15
# cross-compiles them and links rosbas, with its libc++.
# ROSBAS_BOX_ZIG names that zig 0.15 alone, since the rest of the build
# may use another (ZIG).  By default it is ~/zig015/zig, where the Studio
# keeps its zig 0.15, else ZIG.
ROSBAS_BOX_ZIG ?= $(firstword $(wildcard $(HOME)/zig015/zig) $(ZIG))
ROSBAS_BOX_LLVM := ../rosbas/target/llvm-box/$(RUST_TARGET)/release/rosbas
# It holds the A64X32 runtime compiled beforehand, as the kit's librb.a is,
# and its small functions as bitcode to inline (rosbas build.rs,
# tools/rbllsplit.c), and links its images with roscc's linker as a library.
$(ROSBAS_BOX_LLVM): $(ROSBAS_SRC) ../rosbas/build.rs ../rosbas/tools/build-box-rosbas.sh ../rosbas/tools/rbllsplit.c deps/build-llvm-libs-box.sh \
                    $(wildcard ../rosbas/runtime/*.c ../rosbas/runtime/*.h ../rosbas/runtime/jit/*.c) \
                    $(wildcard ../roscc/src/*.rs) $(ABIDIR)/flags.mk $(CAPPS)/rt/crt0_a64x32.o | clib-image-built
	cd ../rosbas && PATH="$(abspath $(dir $(CARGO))):$$PATH" ZIG="$(ROSBAS_BOX_ZIG)" sh tools/build-box-rosbas.sh
	@touch $@
endif

# The ROM C library's image (package R5): make capps's library, linked by
# roscc --clib-image (capps.mk's clib-image-built: RISC OS's C staged, the
# library's units compiled, the image linked), carried in /init as the ROM
# carries SharedCLibrary's code, and mapped at &72200000 by the module
# (modules/sharedclib).  The box's /init only: the hosted build has no
# 32-bit-pointer code, and its SharedCLibrary no image.  The sub-make runs
# each time; the copy, and so /init, changes only when the image does.
CLIB_ROM := $(GEN)/capps/clib_rom.elf
$(CLIB_ROM): clib-image-built
	@mkdir -p $(@D)
	@cmp -s $(CAPPS)/clib/clib.elf $@ || cp $(CAPPS)/clib/clib.elf $@
$(GEN)/clib_rom.h: $(CLIB_ROM) tools/bin2c.py
	$(PYTHON) tools/bin2c.py $@ ros_clib_rom=$(CLIB_ROM)
$(GOBJ)/modules/sharedclib/sharedclib.o: $(GEN)/clib_rom.h


-include $(GUEST_OBJ:.o=.d) $(HOST_OBJ:.o=.d)

$(B)/init: $(GUEST_OBJ) $(GUEST_LIBS) $(BASICSTAMP)
	$(GUEST_CC) $(CFLAGS) -o $@ $(GUEST_OBJ) $(GUEST_LIBS) -lc++

# The ARM container's engine: dynarmic for the box's machine, and for this
# one; the C face over it (runtime/armrun/armrun.cpp)
$(ARMRUN_GUEST)/lib/libarmrun-engine.a: deps/build-dynarmic.sh $(ARMRUN_PATCHES)
	TARGET=$(TARGET) sh deps/build-dynarmic.sh $(ARMRUN_GUEST)
$(ARMRUN_HOST)/lib/libarmrun-engine.a: deps/build-dynarmic.sh $(ARMRUN_PATCHES)
	TARGET=host sh deps/build-dynarmic.sh $(ARMRUN_HOST)
$(GOBJ)/runtime/armrun/armrun.o: runtime/armrun/armrun.cpp runtime/armrun/armrun.h $(ARMRUN_GUEST)/lib/libarmrun-engine.a
	@mkdir -p $(dir $@)
	$(GUEST_CXX) $(CXXFLAGS) -DDYNARMIC_EXTERNAL_SIGNALS -I$(ARMRUN_GUEST)/include -c -o $@ $<
$(HOBJ)/runtime/armrun/armrun.o: runtime/armrun/armrun.cpp runtime/armrun/armrun.h $(ARMRUN_HOST)/lib/libarmrun-engine.a
	@mkdir -p $(dir $@)
	$(HOSTCXX) $(CXXFLAGS) -I$(ARMRUN_HOST)/include -c -o $@ $<

# The engine alone, on this machine (tests/armrun/smoke.c): A32 code run,
# a fault taken to the hooks, a range of blocks invalidated
$(B)/armrun-smoke: tests/armrun/smoke.c $(HOBJ)/runtime/armrun/armrun.o $(ARMRUN_HOST)/lib/libarmrun-engine.a
	$(HOSTCXX) -O2 -Iruntime/armrun -x c -std=c11 tests/armrun/smoke.c -x none $(HOBJ)/runtime/armrun/armrun.o \
	    $(ARMRUN_HOST)/lib/libarmrun-engine.a -o $@
armrun-smoke: $(B)/armrun-smoke
	$(B)/armrun-smoke

# The corpus tools (runtime/armrun/SPRINTS.md A2), on this machine: armscan
# unsqueezes an AIF by running its decompressor, probes words, runs idiom
# frames; tools/corpus_decode.py and tests/armrun/idioms.py drive it
$(B)/armrun/armscan: runtime/armrun/tools/armscan.cpp $(HOBJ)/runtime/armrun/armrun.o $(ARMRUN_HOST)/lib/libarmrun-engine.a
	@mkdir -p $(@D)
	$(HOSTCXX) $(CXXFLAGS) -I$(ARMRUN_HOST)/include -o $@ $< $(HOBJ)/runtime/armrun/armrun.o $(ARMRUN_HOST)/lib/libarmrun-engine.a
armscan: $(B)/armrun/armscan

# The ARM programs the self-test runs (boot/selftest_armrun.c): rosasm's
# code areas (tools/elftext.py) as C arrays
$(GEN)/armrun/boxhello.bin: tests/armrun/boxhello.s tools/elftext.py $(ROSASM)
	@mkdir -p $(@D)
	$(ROSASM) $< --elf -o $(GEN)/armrun/boxhello.elf
	$(PYTHON) tools/elftext.py $(GEN)/armrun/boxhello.elf $@
$(GEN)/armrun/nest.bin: tests/armrun/nest.s tools/elftext.py $(ROSASM)
	@mkdir -p $(@D)
	$(ROSASM) $< --elf -o $(GEN)/armrun/nest.elf
	$(PYTHON) tools/elftext.py $(GEN)/armrun/nest.elf $@
$(GEN)/armrun/shadowtest.bin: tests/armrun/shadowtest.s tools/elftext.py $(ROSASM)
	@mkdir -p $(@D)
	$(ROSASM) $< --elf -o $(GEN)/armrun/shadowtest.elf
	$(PYTHON) tools/elftext.py $(GEN)/armrun/shadowtest.elf $@
$(GEN)/armrun_tests.h: $(GEN)/armrun/boxhello.bin $(GEN)/armrun/nest.bin $(GEN)/armrun/shadowtest.bin tools/bin2c.py
	$(PYTHON) tools/bin2c.py $@ armrun_boxhello=$(GEN)/armrun/boxhello.bin armrun_nest=$(GEN)/armrun/nest.bin \
	    armrun_shadowtest=$(GEN)/armrun/shadowtest.bin
$(GOBJ)/boot/selftest_armrun.o $(HOBJ)/boot/selftest_armrun.o $(GOBJ)/boot/selftest_shadow.o $(HOBJ)/boot/selftest_shadow.o: $(GEN)/armrun_tests.h
# The ARM modules the container runs emulated (runtime/armrun/arm/README.md)
$(GEN)/armrun_modules.h: runtime/armrun/arm/SharedCLibrary,ffa tools/bin2c.py
	$(PYTHON) tools/bin2c.py $@ armrun_sharedclib=runtime/armrun/arm/SharedCLibrary,ffa
$(GOBJ)/runtime/armrun/box.o $(HOBJ)/runtime/armrun/box.o: $(GEN)/armrun_modules.h

# The bridge, hosted (runtime/armrun/SPRINTS.md A3): hello.s's programs,
# assembled by rosasm, run through the engine and the bridge with stub SWIs
# -- this machine's back end (make armrun-hello), and, on Apple silicon, the
# x86-64 one under Rosetta (make armrun-hello-x86_64; the Mac Pro runs it
# natively as make armrun-hello)
ARMRUN_HOST_X86 ?= .cache/dynarmic-host-x86_64
$(ARMRUN_HOST_X86)/lib/libarmrun-engine.a: deps/build-dynarmic.sh $(ARMRUN_PATCHES)
	TARGET=host-x86_64 sh deps/build-dynarmic.sh $(ARMRUN_HOST_X86)
$(B)/armrun/hello.elf: tests/armrun/hello.s $(ROSASM)
	@mkdir -p $(@D)
	$(ROSASM) $< --elf -o $@
ARMRUN_HELLO_H := runtime/armrun/bridge.h runtime/armrun/armrun.h
# $(1) the build's directory, $(2) the engine's, $(3) extra flags (-arch)
define armrun_hello
$(B)/armrun/$(1)/hello.o: tests/armrun/hello.c $(ARMRUN_HELLO_H)
	@mkdir -p $$(@D)
	$(HOSTCC) $(3) -std=c11 -O2 -g -Wall -Iinclude -Iruntime/armrun -DROS_ARENA_HOSTED -c -o $$@ $$<
$(B)/armrun/$(1)/bridge.o: runtime/armrun/bridge.c runtime/armrun/fpa.h $(ARMRUN_HELLO_H)
	@mkdir -p $$(@D)
	$(HOSTCC) $(3) -std=c11 -O2 -g -Wall -Iinclude -Iruntime/armrun -DROS_ARENA_HOSTED -c -o $$@ $$<
$(B)/armrun/$(1)/armrun.o: runtime/armrun/armrun.cpp runtime/armrun/armrun.h $(2)/lib/libarmrun-engine.a
	@mkdir -p $$(@D)
	$(HOSTCXX) $(3) -std=c++20 -O2 -g -Wall -Iruntime/armrun -I$(2)/include -c -o $$@ $$<
$(B)/armrun/$(1)/fpa.o: runtime/armrun/fpa.c runtime/armrun/fpa.h
	@mkdir -p $$(@D)
	$(HOSTCC) $(3) -std=c11 -O2 -g -Wall -Iinclude -Iruntime/armrun -DROS_ARENA_HOSTED -c -o $$@ $$<
$(B)/armrun/$(1)/hello: $(B)/armrun/$(1)/hello.o $(B)/armrun/$(1)/bridge.o $(B)/armrun/$(1)/fpa.o $(B)/armrun/$(1)/armrun.o $(2)/lib/libarmrun-engine.a
	$(HOSTCXX) $(3) -o $$@ $$^
endef
$(eval $(call armrun_hello,host,$(ARMRUN_HOST),))
$(eval $(call armrun_hello,x86_64,$(ARMRUN_HOST_X86),-arch x86_64))
armrun-hello: $(B)/armrun/host/hello $(B)/armrun/hello.elf
	$(B)/armrun/host/hello $(B)/armrun/hello.elf
armrun-hello-x86_64: $(B)/armrun/x86_64/hello $(B)/armrun/hello.elf
	$(B)/armrun/x86_64/hello $(B)/armrun/hello.elf

# One run of the image libraries' script makes all three, for the box or,
# TARGET=host, for the hosted test
$(IMG_GUEST)/lib/libpng16.a: deps/build-imagelibs.sh
	TARGET=$(TARGET) sh deps/build-imagelibs.sh $(IMG_GUEST)
$(IMG_GUEST)/lib/libjpeg.a $(IMG_GUEST)/lib/libz.a: $(IMG_GUEST)/lib/libpng16.a ;
$(IMG_HOST)/lib/libpng16.a: deps/build-imagelibs.sh
	TARGET=host sh deps/build-imagelibs.sh $(IMG_HOST)
$(IMG_HOST)/lib/libjpeg.a $(IMG_HOST)/lib/libz.a: $(IMG_HOST)/lib/libpng16.a ;

$(SMB2_GUEST)/lib/libsmb2.a: deps/build-libsmb2.sh deps/libsmb2/config.h
	TARGET=$(TARGET) sh deps/build-libsmb2.sh $(SMB2_GUEST)
$(SMB2_HOST)/lib/libsmb2.a: deps/build-libsmb2.sh deps/libsmb2/config.h
	TARGET=host sh deps/build-libsmb2.sh $(SMB2_HOST)

# One run of the script makes both: under make -j a rule naming the two
# would run it twice at once, the downloads racing.
$(OSSL_GUEST)/lib/libssl.a:
	TARGET=$(TARGET) sh deps/build-openssl.sh $(OSSL_GUEST)
$(OSSL_GUEST)/lib/libcrypto.a: $(OSSL_GUEST)/lib/libssl.a ;
$(CURL_GUEST)/lib/libcurl.a: $(OSSL_GUEST)/lib/libcrypto.a
	TARGET=$(TARGET) sh deps/build-curl.sh $(CURL_GUEST) $(OSSL_GUEST)
$(SSH_GUEST)/bin/ssh: $(OSSL_GUEST)/lib/libcrypto.a
	TARGET=$(TARGET) sh deps/build-openssh.sh $(SSH_GUEST) $(OSSL_GUEST)
$(SSH_GUEST)/bin/ssh-keygen $(SSH_GUEST)/bin/sshd $(SSH_GUEST)/bin/sshd-session $(SSH_GUEST)/bin/sshd-auth: $(SSH_GUEST)/bin/ssh ;

SSH_PROGS := $(addprefix $(SSH_GUEST)/bin/,ssh ssh-keygen sshd sshd-session sshd-auth)
# One run makes all four compilers; the box's x32-tcc is the last it
# writes.  A change to the script or the patch builds them again.
TCC_ALL := $(TCC_GUEST)/box/usr/bin/x32-tcc
$(TCC_ALL): deps/build-tcc.sh $(ABIDIR)/tcc-x32.patch
	sh deps/build-tcc.sh $(TCC_GUEST)
$(TCC_GUEST)/host/bin/tcc $(TCC_GUEST)/host/bin/x32-tcc $(TCC_GUEST)/box/usr/bin/tcc: $(TCC_ALL) ;

$(KSMBD_GUEST)/bin/ksmbd.tools:
	TARGET=$(TARGET) sh deps/build-ksmbd.sh $(KSMBD_GUEST)

# Mojo, BoxTools' *Mojo (modules/boxtools): the compiler as a Linux program
# behind the * command, as tcc, clang, rosbas and roscc are.  The compiler
# is not built here: deps/get-mojo.sh fetches the machine's archive (the
# private release mojo-box-4, checked by SHA-256, which deps/build-mojo-
# box.sh makes) into .cache.  It is one static program against musl, with
# mimalloc for its malloc and no libraries beside it.  Around it, made here from this tree, as the box
# has them under /usr/lib/mojo:
#   bin/mojo                the compiler (the archive); /usr/bin/mojo a
#                           link to it
#   lib/mojo/std.mojoc      the standard library (the archive)
#   pkg/riscos, pkg/demos   the riscos bindings and demos packages, Mojo
#                           source (../mojo), imported with -I
#   rt_x32/ or rt_a64x32/   the runtime an application is linked with:
#                           roscc/rostrt's rostrt.c (the CompilerRT surface
#                           Mojo's code calls, the heap, write(), any SWI
#                           for the library), wimp.c, the SWI gate shims
#                           swis_os and swis_wimp, crt0_mojo_<abi>.c,
#                           maths.c (std.math's C functions) as the archive
#                           maths_<abi>.a, and the other chunks' generated
#                           shims (the riscos package's territory, font,
#                           sound ... modules: swis_*_x32.c) as the archive
#                           swis_<abi>.a, compiled as
#                           roscc/tools/build-mojo-<abi>.sh compiles them
# and /etc/modular/modular.cfg, which the compiler reads as it starts: the
# standard library's directory; a linker driver that is there (the box's
# clang), so that it does not look for a C linker (*Mojo makes objects and
# roscc links them); and no crash reporting or telemetry -- the box has no
# crash handler program, and its compiler reports to no one.
MOJO_ARCHIVE := $(DEPS)/mojo-box-musl-$(ARCH)
MOJO_STAGE   := $(GEN)/mojo-stage
MOJO_RT_SRC  := ../roscc/rostrt
MOJO_PKGS    := $(wildcard ../mojo/riscos/*.mojo ../mojo/demos/*.mojo)
ifeq ($(ARCH),x86_64)
MOJO_RT      := rt_x32
MOJO_RT_CC    = $(ZIG) clang -target x86_64-linux-gnux32 -march=x86-64 -nostdinc
# each object as source:name in rt_x32
MOJO_RT_OBJS := crt0_mojo_x32:crt0_mojo_x32 rostrt:rostrt_x32 wimp:wimp_x32 \
    swis_os_x32:swis_os_x32 swis_wimp_x32:swis_wimp_x32
MOJO_RT_LIB  := maths_x32
MOJO_RT_SWIS := swis_x32
MOJO_RT_AR    = $(ZIG) ar
MOJO_LINKER  := /usr/bin/clang
else
MOJO_RT      := rt_a64x32
MOJO_RT_CC    = $(LLVM_BIN)/clang --target=aarch64-unknown-linux-gnu_ilp32 -march=armv8-a -ffixed-x18 \
    -fwrapv-pointer
# the gate shims are arch-neutral C: the x32 profile's, compiled for A64X32
MOJO_RT_OBJS := crt0_mojo_a64x32:crt0_mojo_a64x32 rostrt:rostrt_a64x32 wimp:wimp_a64x32 \
    swis_os_x32:swis_os_a64x32 swis_wimp_x32:swis_wimp_a64x32
MOJO_RT_LIB  := maths_a64x32
MOJO_RT_SWIS := swis_a64x32
MOJO_RT_AR    = $(LLVM_BIN)/llvm-ar
MOJO_LINKER  := /usr/bin/clang
endif
MOJO_RT_FLAGS := -ffreestanding -fno-pic -fno-common -fno-stack-protector \
    -fno-asynchronous-unwind-tables -fno-unwind-tables -O2 -Wall
$(MOJO_ARCHIVE)/.sha256: deps/get-mojo.sh
	sh deps/get-mojo.sh $(ARCH) $(MOJO_ARCHIVE)
	@touch $@
MOJO_RT_O := $(foreach o,$(MOJO_RT_OBJS),$(GEN)/mojo-rt/$(firstword $(subst :, ,$(o))).o)
$(GEN)/mojo-rt/%.o: $(MOJO_RT_SRC)/%.c $(wildcard $(MOJO_RT_SRC)/*.h)
	@mkdir -p $(@D)
	$(MOJO_RT_CC) $(MOJO_RT_FLAGS) -c $< -o $@
# the maths functions (rostrt/maths.c) as an archive, which roscc draws on
# only for a program that calls them
$(GEN)/mojo-rt/maths.a: $(GEN)/mojo-rt/maths.o
	rm -f $@
	$(MOJO_RT_AR) rcs $@ $<
# and the other chunks' shims, one member each, the same way
MOJO_SWIS_O := $(patsubst $(MOJO_RT_SRC)/%.c,$(GEN)/mojo-rt/%.o,\
    $(filter-out %/swis_os_x32.c %/swis_wimp_x32.c,$(wildcard $(MOJO_RT_SRC)/swis_*_x32.c)))
$(GEN)/mojo-rt/swis.a: $(MOJO_SWIS_O)
	rm -f $@
	$(MOJO_RT_AR) rcs $@ $^
$(MOJO_STAGE)/.made: $(MOJO_ARCHIVE)/.sha256 $(MOJO_PKGS) $(MOJO_RT_O) $(GEN)/mojo-rt/maths.a $(GEN)/mojo-rt/swis.a
	rm -rf $(MOJO_STAGE)
	mkdir -p $(MOJO_STAGE)/usr/bin $(MOJO_STAGE)/usr/lib $(MOJO_STAGE)/etc/modular
	cp -R $(MOJO_ARCHIVE)/mojo $(MOJO_STAGE)/usr/lib/mojo
	ln -s ../lib/mojo/bin/mojo $(MOJO_STAGE)/usr/bin/mojo
	mkdir -p $(MOJO_STAGE)/usr/lib/mojo/pkg/riscos $(MOJO_STAGE)/usr/lib/mojo/pkg/demos \
	    $(MOJO_STAGE)/usr/lib/mojo/$(MOJO_RT)
	cp ../mojo/riscos/*.mojo $(MOJO_STAGE)/usr/lib/mojo/pkg/riscos/
	cp ../mojo/demos/*.mojo $(MOJO_STAGE)/usr/lib/mojo/pkg/demos/
	for o in $(MOJO_RT_OBJS); do cp $(GEN)/mojo-rt/$${o%%:*}.o $(MOJO_STAGE)/usr/lib/mojo/$(MOJO_RT)/$${o#*:}.o; done
	cp $(GEN)/mojo-rt/maths.a $(MOJO_STAGE)/usr/lib/mojo/$(MOJO_RT)/$(MOJO_RT_LIB).a
	cp $(GEN)/mojo-rt/swis.a $(MOJO_STAGE)/usr/lib/mojo/$(MOJO_RT)/$(MOJO_RT_SWIS).a
	printf '[mojo-max]\nimport_path = /usr/lib/mojo/lib/mojo\nlinker_driver = %s\n\n[crash_reporting]\nenabled = false\n\n[telemetry]\nenabled = false\n' \
	    $(MOJO_LINKER) > $(MOJO_STAGE)/etc/modular/modular.cfg
	@touch $@

# The Intel box's ROM: /init, ssh and sshd, ksmbd-tools, clang and ld.lld
# with musl and compiler-rt's builtins (above), rosbas, roscc and their
# kits, pdfsvc, and Mojo's compiler and kit (above)
ifeq ($(ARCH),x86_64)
$(B)/initramfs.cpio: $(B)/init $(B)/wperun initramfs/mkcpio.py $(SSH_PROGS) $(KSMBD_GUEST)/bin/ksmbd.tools $(CLANG_BOX_X86)/usr/bin/llvm \
                    $(ROSBAS_BOX) $(ROSCC_BOX) $(ROSBAS_KIT)/librb.a $(PDFSVC_BOX) $(MOJO_STAGE)/.made
	$(PYTHON) initramfs/mkcpio.py $@ --dir /dev --dir /proc --dir /sys \
	    --char /dev/console=5,1 --file /init=$(B)/init:0755 --dir /usr --dir /usr/bin \
	    --file /usr/bin/ssh=$(SSH_GUEST)/bin/ssh:0755 --file /usr/bin/ssh-keygen=$(SSH_GUEST)/bin/ssh-keygen:0755 \
	    --dir /usr/sbin --dir /usr/libexec --file /usr/sbin/sshd=$(SSH_GUEST)/bin/sshd:0755 \
	    --file /usr/libexec/sshd-session=$(SSH_GUEST)/bin/sshd-session:0755 \
	    --file /usr/libexec/sshd-auth=$(SSH_GUEST)/bin/sshd-auth:0755 \
	    --file /usr/sbin/ksmbd.tools=$(KSMBD_GUEST)/bin/ksmbd.tools:0755 \
	    --file /usr/bin/wperun=$(B)/wperun:0755 \
	    --tree /usr=$(CLANG_BOX_X86)/usr \
	    --file /usr/bin/rosbas=$(ROSBAS_BOX):0755 --file /usr/bin/roscc=$(ROSCC_BOX):0755 \
	    --file /usr/bin/pdfsvc=$(PDFSVC_BOX):0755 \
	    --tree /usr/lib/rosbas=$(ROSBAS_KIT) --dir /usr/lib/capps --dir /usr/lib/capps/inc \
	    --tree /usr/lib/capps/inc/C=$(CAPPS)/inc/C --dir /usr/lib/roscc --tree /usr/lib/roscc/x32=$(GEN)/capps/rt \
	    --tree /usr=$(MOJO_STAGE)/usr --dir /etc --tree /etc=$(MOJO_STAGE)/etc
else
# The Apple Silicon box's ROM: /init, ssh and sshd, ksmbd-tools -- built for
# aarch64 as /init is -- and BoxTools' compiler: clang and
# ld.lld, musl and compiler-rt's builtins (.cache/clang-box, under /usr, as
# the Intel box's are), rosbas (with LLVM's code generator: above) and
# roscc, rosbas's A64X32 kit,
# RISC OS's C headers staged for A64X32, and roscc's A64X32 start and stubs
# (/usr/lib/roscc/a64x32), and Mojo's compiler and kit (/usr/bin/mojo,
# /usr/lib/mojo, /etc/modular: above).  In the initramfs, as the Intel box's clang is,
# until the box has a disc of its own: it costs RAM, not
# simplicity.
$(B)/initramfs.cpio: $(B)/init $(B)/wperun initramfs/mkcpio.py $(SSH_PROGS) $(KSMBD_GUEST)/bin/ksmbd.tools \
                    $(CLANG_BOX)/usr/bin/llvm $(ROSBAS_BOX_LLVM) $(ROSCC_BOX) $(ROSBAS_KIT)/librb.a $(PDFSVC_BOX) \
                    $(MOJO_STAGE)/.made
	$(PYTHON) initramfs/mkcpio.py $@ --dir /dev --dir /proc --dir /sys \
	    --char /dev/console=5,1 --file /init=$(B)/init:0755 --dir /usr --dir /usr/bin \
	    --file /usr/bin/ssh=$(SSH_GUEST)/bin/ssh:0755 --file /usr/bin/ssh-keygen=$(SSH_GUEST)/bin/ssh-keygen:0755 \
	    --dir /usr/sbin --dir /usr/libexec --file /usr/sbin/sshd=$(SSH_GUEST)/bin/sshd:0755 \
	    --file /usr/libexec/sshd-session=$(SSH_GUEST)/bin/sshd-session:0755 \
	    --file /usr/libexec/sshd-auth=$(SSH_GUEST)/bin/sshd-auth:0755 \
	    --file /usr/sbin/ksmbd.tools=$(KSMBD_GUEST)/bin/ksmbd.tools:0755 \
	    --file /usr/bin/wperun=$(B)/wperun:0755 \
	    --tree /usr=$(CLANG_BOX)/usr \
	    --file /usr/bin/rosbas=$(ROSBAS_BOX_LLVM):0755 --file /usr/bin/roscc=$(ROSCC_BOX):0755 \
	    --file /usr/bin/pdfsvc=$(PDFSVC_BOX):0755 \
	    --tree /usr/lib/rosbas=$(ROSBAS_KIT) --dir /usr/lib/capps --dir /usr/lib/capps/inc \
	    --tree /usr/lib/capps/inc/C=$(CAPPS)/inc/C --dir /usr/lib/roscc --tree /usr/lib/roscc/a64x32=$(CAPPS)/rt \
	    --tree /usr=$(MOJO_STAGE)/usr --dir /etc --tree /etc=$(MOJO_STAGE)/etc
endif

# wperun (ports/wpe): the WPE edition's root mounted and a program run in
# it.  In the ROM, so that WaylandWindows can start the compositor
# (rosgd.display=compositor) with no share.
$(B)/wperun: ports/wpe/wperun.c
	$(ZIG) cc -target $(TARGET) -static -O2 -Wall -o $@ $<

# ---- running it ----------------------------------------------------------

# The user's share, build/usershare: what run/run-x86_64.sh shares with a
# box in a window -- disc/ as the PC image's RISC OS disc has it (the
# examples, the apps, !Boot, ReadMe; BASIC text tokenised as RISC OS saves
# it, tools/basictok.py), and !NetSurf in Apps when ports/netsurf/build.sh
# has built it (it is a build, not a source), !PDF when make rospdf has
# (ports/rospdf/build.sh), !Write when make write has (ports/write/build.sh;
# disc/'s !Write is the x32 one, make write-disc's),
# and !StrongED when make stronged has
# (ports/stronged/stronged.mk).  Made again over what is
# there: the shipped files are replaced, anything else the user saved stays.
# !SparkFS is in the ROM now (ports/sparkfs/sparkfs.mk): the copy earlier
# shares were given in Apps is taken away, so there is one.
usershare:
	$(PYTHON) tools/basictok.py $(BASICVPFSRC)/Sources/Programmer/BASIC --tree disc $(B)/usershare
	@if [ -d '$(B)/ports/netsurf/!NetSurf' ]; then \
	    rm -rf '$(B)/usershare/Apps/!NetSurf' && cp -R '$(B)/ports/netsurf/!NetSurf' '$(B)/usershare/Apps/!NetSurf' && \
	    echo "usershare: Apps.!NetSurf (ports/netsurf/build.sh's)"; fi
	@if [ -d '$(B)/ports/rospdf/!PDF' ]; then \
	    rm -rf '$(B)/usershare/Apps/!PDF' && cp -R '$(B)/ports/rospdf/!PDF' '$(B)/usershare/Apps/!PDF' && \
	    echo "usershare: Apps.!PDF (make rospdf's)"; fi
	@if [ -d '$(B)/ports/write/!Write' ]; then \
	    rm -rf '$(B)/usershare/Apps/!Write' && cp -R '$(B)/ports/write/!Write' '$(B)/usershare/Apps/!Write' && \
	    echo "usershare: Apps.!Write (make write's)"; \
	elif [ '$(ARCH)' = aarch64 ]; then rm -rf '$(B)/usershare/Apps/!Write' && \
	    echo "usershare: no Apps.!Write: disc/'s is x32 (make ARCH=aarch64 write)"; fi
	@if [ -d '$(B)/usershare/Apps/!SparkFS' ]; then \
	    rm -rf '$(B)/usershare/Apps/!SparkFS' && \
	    echo "usershare: Apps.!SparkFS taken away (the ROM's is Resources:\$$.Apps.!SparkFS)"; fi
	@if [ -d '$(B)/stronged/!StrongED' ]; then \
	    rm -rf '$(B)/usershare/Apps/!StrongED' && cp -R '$(B)/stronged/!StrongED' '$(B)/usershare/Apps/!StrongED' && \
	    echo "usershare: Apps.!StrongED (make stronged's)"; fi
	@if [ -d '$(B)/usershare/Apps/!StrongHlp' ]; then \
	    rm -rf '$(B)/usershare/Apps/!StrongHlp' && \
	    echo "usershare: Apps.!StrongHlp taken away (the ROM's is Resources:\$$.Apps.!StrongHlp)"; fi
ifeq ($(ARCH),aarch64)
	@# disc/'s !PipeDream carries the x32 image: the A64X32 one in its place
	@# (ARCH=aarch64 ports/pipedream/tools/build.sh), else none
	@if [ -f '$(B)/pipedream/!RunImage' ]; then \
	    cp '$(B)/pipedream/!RunImage' '$(B)/usershare/Apps/!PipeDream/!RunImage,ff8' && \
	    echo "usershare: Apps.!PipeDream's A64X32 !RunImage (ports/pipedream/tools/build.sh's)"; \
	else rm -f '$(B)/usershare/Apps/!PipeDream/!RunImage,ff8'; \
	    echo "usershare: Apps.!PipeDream has no A64X32 !RunImage (ARCH=aarch64 ports/pipedream/tools/build.sh)"; fi
	@# MandelX64 is x86-64 code BASIC's assembler writes: it cannot run here
	@rm -f '$(B)/usershare/Examples/MandelX64,fd1' '$(B)/usershare/Examples/MandelX64,ffb'
else
	@# MandelA64 is AArch64 code BASIC's assembler writes: the Apple Silicon box's
	@rm -f '$(B)/usershare/Examples/MandelA64,fd1' '$(B)/usershare/Examples/MandelA64,ffb'
endif
	@# every image on the share is this box's machine (tools/elfcheck.py)
	$(PYTHON) tools/elfcheck.py --machine $(ARCH) '$(B)/usershare'
	@echo "usershare: $(B)/usershare (HostFS::Host in a box window)"

$(B)/hosttest: $(HOST_OBJ) $(BASICSTAMP) | $(TCC_GUEST)/host/bin/tcc $(ROSBAS_HOST) $(PDFSVC_HOST)
	$(HOSTCC) $(CFLAGS) -o $@ $(HOST_OBJ) $(HOST_LIBS)

hosttest: $(B)/hosttest
	$(B)/hosttest

# The hosted tests, all of them: the self-test, BASIC's exact comparisons
# (BASICVFP, the ROM's only BASIC), the Rosetta corpus -- 163 programs against
# RISC OS 5.30's output (tests/basic/rosetta.py; the corpus is
# zcode-work's, ROSGD_ROSETTA names another); the Wimp cases; the Pinboard
# with its task running, pinning (tests/desktop/pinboard/pin.py); the ROM's
# applications (tests/desktop/romapps); BASIC's inline assembler for ARM,
# x86-64 and AArch64 against its ARM assembler on RISC OS 5.30
# (tests/basic/asm).  Each runs even if one before it failed; make fails if
# any did.  The box's is make boot.
test: $(B)/hosttest
	@fail=0; \
	$(B)/hosttest < /dev/null > $(B)/selftest.log 2>&1 || fail=1; \
	grep -a "ROSGD-SELFTEST" $(B)/selftest.log || fail=1; \
	$(PYTHON) tests/basic/compare.py --binary $(B)/hosttest > $(B)/compare.log 2>&1 || fail=1; \
	tail -1 $(B)/compare.log; \
	$(PYTHON) tests/basic/rosetta.py --binary $(B)/hosttest || fail=1; \
	$(PYTHON) tests/wimp/compare.py --binary $(B)/hosttest > $(B)/wimp.log 2>&1 || fail=1; \
	tail -1 $(B)/wimp.log; \
	$(PYTHON) tests/desktop/pinboard/pin.py --hosted --binary $(B)/hosttest > $(B)/pin.log 2>&1 || fail=1; \
	tail -1 $(B)/pin.log; \
	BASICVPFSRC=$(BASICVPFSRC) $(PYTHON) tests/desktop/romapps/check.py --binary $(B)/hosttest > $(B)/romapps.log 2>&1 || fail=1; \
	tail -1 $(B)/romapps.log; \
	$(PYTHON) tests/basic/asm/compare.py --binary $(B)/hosttest > $(B)/asm.log 2>&1 || fail=1; \
	grep "^asm/compare: .* as they should be" $(B)/asm.log; \
	exit $$fail

# The self-test in the box, then power off.  QEMU exits 0 however the box
# powers off, so make reads /init's last word from the serial console and
# fails unless it was "power off (success)" -- every check passed.  The
# Apple Silicon box's is run/run-vz.sh --test (rosgd-vz, headless).
ifeq ($(ARCH),aarch64)
RUN_TEST := run/run-vz.sh --test
else
RUN_TEST := run/run-x86_64.sh --test
endif
boot: $(B)/initramfs.cpio
	@mkdir -p $(B)/share
	@echo "hello from the host" > $(B)/share/hello.txt
	$(RUN_TEST) | tee $(B)/boot-serial.log
	@grep -aq "^rosgd: power off (success)" $(B)/boot-serial.log || \
	    { echo "boot: the box's self-test did not pass: $(B)/boot-serial.log"; exit 1; }

# The suites in the box, a boot each through tests/lib/boxrun.py (rosgd.run,
# the share over 9p, a fault report on the serial console failing the run):
# the runner's own probes, BASIC's exact comparisons (BASICVFP),
# the Wimp cases, BASIC's inline assembler (tests/basic/asm, with x86-64
# code run by CALL and USR), the Mac's tcc's x32 mode against clang's, the ROM C library's
# image (tests/capps/clibimage/boxcheck.py, as make clib-boxcheck: the image
# from make capps's library objects, built by this make's clib-image-built,
# which the ROM's SharedCLibrary needs too), and the SharedCLibrary's
# behaviour suite (tests/capps/clib/compare.py --box, package Q2b: its C
# programs built for x32 against the ROM library, each case's state against
# RISC OS 5.30's record, the cases package L1a owns the same), rlib's
# ObjAsm in C (tests/capps/rlib/rlib.py --box, package L4: programs linked
# against the library and make capps's rlib.a, against what rlib's ObjAsm
# does; and first, on the host, make capps-apps: Edit, Draw, Paint and
# Filer_Action link with nothing undefined), two C applications as two Wimp tasks passing a baton 10^5 times,
# each with its own statics and base (tests/capps/baton.py, package R3),
# and a program's faults (tests/capps/faults, package R2b), against RISC OS
# 5.30's output as make test does hosted; then the desktop itself, headless
# (tests/lib/deskdrive.py), a box each: F12's ShellCLI (tests/desktop/
# shellcli); error boxes and command windows answering keys and clicks,
# Exit desktop to the * prompt and back, iconising, Shutdown and Restart,
# Host's Free, two BASIC desktop programs' VFP contexts kept apart
# (tests/deskprobe/vfptasks.py), commands that call themselves for ever --
# a run action, an Obey file's last line, an alias -- ending in RISC OS's
# errors on a task's thread, in a task window and at F12's prompt
# (tests/deskprobe/recursion.py), RISC OS's !Edit -- the ROM's -- started,
# typed into, saved, and its task windows, and its !Paint, a sprite drawn
# with each tool, saved and read back pixel for pixel, sprite files loaded,
# and SparkFS: a zip opened as a directory, a file in it edited and saved,
# files dragged in and out; ChangeFSI: images converted at the command line
# against RISC OS 5.30's sprites (ports/changefsi/test), and from the
# desktop, saved and sent to Paint; and the Filer's operations through the ROM's
# Filer_Action -- copies, a move, Abort, Count, Find, Access, Set type,
# Stamp, a locked file's error, Delete and its confirm, read off the
# screen (tests/deskprobe); the Filer's viewers updating, pinning
# with the Pinboard's task, !Chars choosing an outline font, the ROM's
# applications (tests/desktop); StrongED in C: make stronged's
# !StrongED started, texts opened, drawn, edited with its keys and menus,
# saved and checked byte for byte, in BaseMode, C, BASIC, HTML and WML
# (tests/stronged: desk, view, edit, keys, syntax, modes, tools, settings,
# search, lof, dbox, bars, macros, reindent, display, windows, choices --
# modes.py's BASIC texts are make stronged-test's disc).  Each runs even if
# one before it failed; make fails if any did.
boxtest: $(B)/initramfs.cpio clib-image-built rlib-archive-built capps-apps $(GEN)/capps/batona.elf $(GEN)/capps/batonb.elf \
    stronged $(B)/stronged/host/disc/.made sparkfs changefsi
	@fail=0; \
	$(PYTHON) tests/lib/boxrun.py --check > $(B)/boxrun.log 2>&1 || fail=1; \
	tail -1 $(B)/boxrun.log; \
	$(PYTHON) tests/basic/compare.py --guest > $(B)/compare-guest.log 2>&1 || fail=1; \
	tail -1 $(B)/compare-guest.log; \
	$(PYTHON) tests/wimp/compare.py --guest > $(B)/wimp-guest.log 2>&1 || fail=1; \
	tail -1 $(B)/wimp-guest.log; \
	$(PYTHON) tests/basic/asm/compare.py --guest > $(B)/asm-guest.log 2>&1 || fail=1; \
	grep "^asm/compare: .* as they should be" $(B)/asm-guest.log; \
	$(PYTHON) tests/capps/interop/interop.py > $(B)/interop.log 2>&1 || fail=1; \
	grep -E "^(x32-tcc|interop): " $(B)/interop.log; \
	$(PYTHON) tests/capps/clibimage/boxcheck.py $(CAPPS) > $(B)/clibimage-box.log 2>&1 || fail=1; \
	grep -E "^boxcheck: " $(B)/clibimage-box.log || tail -5 $(B)/clibimage-box.log; \
	RISCOSSRC=$(RISCOSSRC) $(PYTHON) tests/capps/clib/compare.py --box > $(B)/clib-box.log 2>&1 || fail=1; \
	grep -E "^clib-box: " $(B)/clib-box.log || tail -5 $(B)/clib-box.log; \
	$(PYTHON) tests/capps/rlib/rlib.py --box > $(B)/rlib-box.log 2>&1 || fail=1; \
	grep -E "^rlib-box: " $(B)/rlib-box.log || tail -5 $(B)/rlib-box.log; \
	$(PYTHON) tests/capps/baton.py > $(B)/baton.log 2>&1 || fail=1; \
	grep -E "^(time|baton): " $(B)/baton.log; \
	$(PYTHON) tests/capps/faults/faults.py > $(B)/faults.log 2>&1 || fail=1; \
	grep -E "^faults: box" $(B)/faults.log; \
	$(PYTHON) tests/desktop/shellcli/probe.py > $(B)/shellcli-box.log 2>&1 || fail=1; \
	tail -1 $(B)/shellcli-box.log; \
	for p in errorbox exitdesktop iconise shutdown free vfptasks recursion; do \
	    RISCOSSRC=$(RISCOSSRC) $(PYTHON) tests/deskprobe/$$p.py > $(B)/$$p-box.log 2>&1 || fail=1; \
	    tail -1 $(B)/$$p-box.log; \
	done; \
	RISCOSSRC=$(RISCOSSRC) $(PYTHON) tests/deskprobe/edit.py > $(B)/edit-box.log 2>&1 || fail=1; \
	tail -1 $(B)/edit-box.log; \
	RISCOSSRC=$(RISCOSSRC) $(PYTHON) tests/deskprobe/paint.py > $(B)/paint-box.log 2>&1 || fail=1; \
	tail -1 $(B)/paint-box.log; \
	$(PYTHON) tests/deskprobe/sparkfs.py > $(B)/sparkfs-box.log 2>&1 || fail=1; \
	tail -1 $(B)/sparkfs-box.log; \
	$(PYTHON) tests/deskprobe/fileraction.py > $(B)/fileraction-box.log 2>&1 || fail=1; \
	tail -1 $(B)/fileraction-box.log; \
	$(PYTHON) ports/sparkfs/test/codectest.py > $(B)/sparkfs-codecs-box.log 2>&1 || fail=1; \
	tail -1 $(B)/sparkfs-codecs-box.log; \
	$(PYTHON) ports/changefsi/test/converttest.py > $(B)/changefsi-convert-box.log 2>&1 || fail=1; \
	tail -1 $(B)/changefsi-convert-box.log; \
	$(PYTHON) tests/deskprobe/changefsi.py > $(B)/changefsi-box.log 2>&1 || fail=1; \
	tail -1 $(B)/changefsi-box.log; \
	$(PYTHON) tests/desktop/filer/viewer_changes.py > $(B)/viewer-box.log 2>&1 || fail=1; \
	tail -1 $(B)/viewer-box.log; \
	$(PYTHON) tests/desktop/pinboard/pin.py > $(B)/pin-box.log 2>&1 || fail=1; \
	tail -1 $(B)/pin-box.log; \
	$(PYTHON) tests/desktop/international/charsbox.py > $(B)/charsbox.log 2>&1 || fail=1; \
	tail -1 $(B)/charsbox.log; \
	$(PYTHON) tests/desktop/romapps/check.py --guest > $(B)/romapps-box.log 2>&1 || fail=1; \
	tail -1 $(B)/romapps-box.log; \
	for p in desk view edit keys syntax modes tools settings search lof dbox bars macros reindent display windows choices help messages misc fold snippets dump print dialogs spell; do \
	    $(PYTHON) tests/stronged/$$p.py > $(B)/stronged-$$p-box.log 2>&1 || fail=1; \
	    tail -1 $(B)/stronged-$$p-box.log; \
	done; \
	exit $$fail

# !PDF (ports/rospdf): the ROSPDF module -- hayro, a PDF renderer in no_std
# Rust, behind a cmhg C module -- and its Wimp viewer, for ARCH's box: x32
# or A64X32, into $(B)/ports/rospdf/!PDF, which make usershare puts in Apps.
# Needs rustup's nightly with rust-src, and make capps (ARCH's) first.
rospdf:
	ARCH=$(ARCH) sh ports/rospdf/build.sh

# !Write (ports/write): a Toolbox word processor over the Paige engine
# (ports/paige, HERMES-Paige with the box's patch), linked against the box's C library,
# for ARCH's box, into $(B)/ports/write/!Write, which make usershare puts in
# Apps.  Needs make capps (ARCH's) first.
write:
	ARCH=$(ARCH) sh ports/write/build.sh

# make's own: !Write built again when its sources, the machine layer's or
# the engine's patch (ports/paige/build.sh makes the engine's tree from it)
# are newer than its image.  After the ROM,
# whose build stages the C library's headers (capps.mk) Write compiles
# against.  (WRITE_APP and WRITE_SRCS are set above all:.)
$(WRITE_APP): $(WRITE_SRCS) | $(B)/initramfs.cpio
	ARCH=$(ARCH) sh ports/write/build.sh

# disc/Apps/!Write, the committed one (the Intel box's share and the PC
# image take it): make write's x32 build copied there.  Run it, and commit
# disc/Apps/!Write, when Write changes.
write-disc:
	sh ports/write/build.sh
	rm -rf 'disc/Apps/!Write' && cp -R 'build/ports/write/!Write' 'disc/Apps/!Write'
	@echo "write-disc: disc/Apps/!Write (x32)"

# The BOX articles (docs/articles/, Markdown; README.md is their index) as
# HTML for NetSurf in the box: disc/Docs/Articles, index.html first.  The
# HTML is committed; run this when the articles change.  ARTICLES names
# another directory of them.
ARTICLES ?= docs/articles
articles:
	$(PYTHON) tools/mkarticles.py $(ARTICLES) disc/Docs/Articles

# tcc's x32 mode (abi/x32/tcc-x32.patch) against clang's x32: the compiler's
# own checks, then the interop suite in the box, its log RISC OS 5.30's
# (tests/capps/interop/interop.py; --farm records the log on the farm).
# roscc links the image ($(ROSCC), built with the initramfs's probes).
tcctest: $(B)/initramfs.cpio
	$(PYTHON) tests/capps/interop/interop.py

kernel:
	$(PYTHON) vm/buildvm.py

clean:
	rm -rf $(B)

.PHONY: articles usershare all hosttest test boot boxtest tcctest rospdf write write-disc kernel clean armrun-smoke armscan armrun-hello armrun-hello-x86_64

# RISC OS C for the C applications, x32: make capps (stage riscos-src's C,
# compile the ROM C library, rlib and the Toolbox), make capps-repro (two
# builds, byte for byte).  The flag table is in capps.mk.
include capps.mk

# POSIX applications over the Unix bridge: make posix-libc, posix-tests,
# posix-boxcheck.  The rules are in posix.mk.
include posix.mk

# StrongED in C: make stronged, make stronged-test.  The rules
# are in ports/stronged/stronged.mk.
include ports/stronged/stronged.mk
# StrongHelp in C: make stronghelp, stronghelp-disc, stronghelp-test
include ports/stronghelp/stronghelp.mk

# The HAL for BOX on QEMU without Linux:
# make hal, run/run-hal.sh.  The rules are in hal/hal.mk.
include hal/hal.mk

# SparkFS in the ROM (#107): its modules and !SparkFS, which the ROM's
# images and resources take.  The rules are in ports/sparkfs/sparkfs.mk.
include ports/sparkfs/sparkfs.mk
include ports/toolbox/toolbox.mk
include ports/drawfile/drawfile.mk
# ChangeFSI in the ROM (#119): ports/changefsi/changefsi.mk.
include ports/changefsi/changefsi.mk

# !GraphTask (G2): apps/graphtask/graphtask.mk
include apps/graphtask/graphtask.mk
