/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest.h: the box's acceptance test. */
#ifndef ROSGD_SELFTEST_H
#define ROSGD_SELFTEST_H

struct ros_selftest {
    unsigned passed, failed;
    unsigned count;
    unsigned char ok[128];      /* per check, in order, for the display */
};

/* Run every check, reporting each on the console.  1 if all passed. */
int ros_selftest(struct ros_selftest *t);

/* One check, counted in the current run and reported on the console. */
void ros_check(int ok, const char *what, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* The Buffer Manager's contract (selftest_buffers.c). */
void ros_selftest_basicvfp(void);
void ros_selftest_buffers(void);
void ros_selftest_speakers(void);
void ros_selftest_machine(void);

/* Floating point, lifted from FPA and VFP to C (selftest_fp.c). */
void ros_selftest_fp(void);

/* Integer code lifted to C (tier 1), against the same compiled tier 0's
 * way, on random inputs: selftest_int.c. */
void ros_selftest_int(void);

/* DRMVideo, the GraphicsV driver (selftest_graphicsv.c); and, after the
 * test, an 8 bpp mode and a pointer left on screen (rosgd.gvdemo). */
void ros_selftest_graphicsv(void);
void ros_graphicsv_demo(void);

/* The VDU drivers, pixel by pixel (selftest_vdu.c). */
void ros_selftest_vdu(void);
/* Virtual displays, GraphTask's first stage (selftest_vdisplay.c). */
void ros_selftest_vdisplay(void);

/* OS_AMBControl, the Wimp's task slots (selftest_amb.c). */
void ros_selftest_amb(void);
/* The free pool and dynamic areas (selftest_dynarea.c). */
void ros_selftest_dynarea(void);
void ros_selftest_wimpswis(void);

/* The kernel's CallBack, the Wimp's switch (selftest_callback.c). */
void ros_selftest_callback(void);

/* The Pinboard, compiled, before the Wimp starts it; OS_CLI's
 * International_Help (selftest_pinboard.c). */
void ros_selftest_pinboard(void);

/* DragASprite and DragAnObject, compiled into the ROM (selftest_drag.c). */
void ros_selftest_drag(void);

/* The Task Manager, compiled, before the desktop (selftest_taskmanager.c). */
void ros_selftest_taskmanager(void);

/* The Filer and FilerSWIs, compiled into the ROM, before a desktop
 * starts the Filer (selftest_filer.c). */
void ros_selftest_filer(void);

/* The Territory Manager and the UK territory, native (selftest_territory.c). */
void ros_selftest_territory(void);

/* The International module, native, and OS_Byte 70 and 71, the system
 * font and the keyboard by the alphabet (selftest_international.c). */
void ros_selftest_international(void);

/* TaskWindow, native, before any task window runs (selftest_taskwindow.c). */
void ros_selftest_taskwindow(void);

/* ShellCLI, native, before a desktop starts a shell (selftest_shellcli.c). */
void ros_selftest_shellcli(void);

/* HostFSFiler, native, before a desktop starts it; the runtime's native
 * start entry (selftest_hostfsfiler.c). */
void ros_selftest_hostfsfiler(void);

/* ScreenModes and the DisplayManager, native: the display's mode list
 * (OS_ScreenMode 2), the mode strings (13, 14), ReadInfo, and the
 * DisplayManager's services, before a desktop starts it
 * (selftest_display.c). */
void ros_selftest_display(void);

/* The Desk Clock, native: its services and command, and the sprite
 * pipeline it draws with (selftest_deskclock.c). */
void ros_selftest_deskclock(void);

/* The Desk Meter and the runtime's busyness counters it graphs
 * (selftest_deskmeter.c). */
void ros_selftest_deskmeter(void);

/* GDraw, the anti-aliasing Draw, against its measured vectors
 * (selftest_gdraw.c). */
void ros_selftest_gdraw(void);

/* Smooth, the desktop's anti-aliasing switch (selftest_smooth.c). */
void ros_selftest_smooth(void);
/* The Font Manager over font files that lie (selftest_fontfiles.c). */
void ros_selftest_fontfiles(void);

/* Play a VDU stream, keep what it drew (vduplay.c, tests/vdu). */
int ros_vdu_play(const char *dir);

/* The Internet module, over Linux's sockets (selftest_internet.c). */
void ros_selftest_internet(void);

/* The Resolver, over the host's name lookup (selftest_resolver.c). */
void ros_selftest_resolver(void);

/* URL_Fetcher and AcornHTTP (selftest_fetch.c). */
void ros_selftest_fetch(void);

/* AcornSSL over OpenSSL (selftest_acornssl.c). */
void ros_selftest_acornssl(void);

/* PTY: Linux programs on pseudo-terminals, its VT100, *SSH-KeyGen
 * (selftest_pty.c). */
void ros_selftest_pty(void);

/* SSHD: RISC OS command lines for SSH sessions, and sshd in the box
 * (selftest_sshd.c). */
void ros_selftest_sshd(void);

/* SMB: LanManFS and SMBServer (selftest_smb.c). */
void ros_selftest_smb(void);

/* BoxTools: *CC, *RunBox, *RosAsm (selftest_boxtools.c). */
void ros_selftest_boxtools(void);

/* CMOS, simulated in a file (selftest_cmos.c). */
void ros_selftest_cmos(void);

/* Kernel SWIs the desktop calls: OS_ScreenMode 4-6, OS_DynamicArea 5 and
 * 27, OS_SynchroniseCodeAreas, OS_Memory 8 (selftest_kernelswis.c). */
void ros_selftest_kernelswis(void);
void ros_selftest_vm(void);

/* The network's commands, shims over Linux: InetRes, *InetInfo,
 * *ResolverConfig, DHCP (selftest_netcmds.c). */
void ros_selftest_netcmds(void);

/* The personality lock, the event queue and background work
 * (selftest_background.c). */
void ros_selftest_background(void);

/* The centisecond tick: MetroGnome, TickerV, ticker events
 * (selftest_ticker.c). */
void ros_selftest_ticker(void);

/* Tasks and the baton (selftest_tasks.c). */
void ros_selftest_tasks(void);

/* The environment handlers (selftest_environment.c). */
void ros_selftest_environment(void);

/* Input: KeyV and PointerV from input events (selftest_input.c). */
void ros_selftest_input(void);

/* OS_Byte and OS_Word: events, VSync, the timers, the clock
 * (selftest_osbyte.c). */
void ros_selftest_osbyte(void);

/* The conversions: OS_Convert*, OS_BinaryToDecimal, OS_ReadUnsigned
 * (selftest_convert.c). */
void ros_selftest_convert(void);

/* System variables and GSTrans (selftest_sysvars.c). */
void ros_selftest_sysvars(void);

/* OS_EvaluateExpression and OS_ReadArgs (selftest_args.c). */
void ros_selftest_args(void);

/* OS_CLI and the kernel's *commands (selftest_oscli.c). */
void ros_selftest_oscli(void);

/* The Filter Manager and the Hourglass, compiled into the ROM
 * (selftest_filterhourglass.c). */
void ros_selftest_filterhourglass(void);

/* OS_Heap and OS_Module (selftest_modules.c). */
void ros_selftest_modules(void);

/* ResourceFS and MessageTrans (selftest_messagetrans.c). */
void ros_selftest_messagetrans(void);

/* The Desktop and WindowUtils, compiled into the ROM: *Help Desktop, its
 * resources (selftest_desktop.c). */
void ros_selftest_desktop(void);

/* The Resource Filer and Free, compiled into the ROM, before a desktop
 * starts them; OS_FSControl 13 and OS_Byte 143 (selftest_resfiler.c). */
void ros_selftest_resfiler(void);

/* The Screen Blanker and SpriteUtils, native modules in the ROM:
 * ScreenBlanker_Control, *BlankTime, the *S commands on the system sprite
 * area, and what the runtime does for them (selftest_sprutils.c). */
void ros_selftest_sprutils(void);

/* Squash, reimplemented: RISC OS 5.30's bytes, made and unmade; the
 * Desktop's banner sprites unsquashed (selftest_squash.c). */
void ros_selftest_squash(void);
/* ZLib, CompressPNG and CompressJPEG, native over zlib, libpng and
 * libjpeg-turbo: 5.30's bytes, round trips, decodes and errors
 * (selftest_zlib.c, selftest_compresspng.c, selftest_compressjpeg.c). */
void ros_selftest_zlib(void);
void ros_selftest_compresspng(void);
/* The Worker module: a mandel_row job on a pool thread (selftest_worker.c) */
void ros_selftest_worker(void);
void ros_selftest_pdriver(void);
/* The ARM container's engine, dynarmic, inside /init (selftest_armrun.c) */
void ros_selftest_armrun(void);
/* ARM shadows: caller kind, routing, load rules (selftest_shadow.c) */
void ros_selftest_shadow(void);
void ros_selftest_compressjpeg(void);
/* SpriteExtend's JPEG SWIs (selftest_jpegplot.c) */
void ros_selftest_jpegplot(void);
/* MimeMap, native: the ROM's table found with no !Boot, HostFS's extensions
 * typed alike, conversions and errors (selftest_mimemap.c). */
void ros_selftest_mimemap(void);
/* Portable, native (selftest_portable.c). */
void ros_selftest_portable(void);

/* BootCommands, native: the *commands !Boot is written in
 * (selftest_bootcmds.c). */
void ros_selftest_bootcmds(void);

/* FileSwitch, HostFS and ResourceFS as a filing system (selftest_files.c). */
void ros_selftest_files(void);

/* C applications: the loader and the gate, or the hosted refusal
 * (selftest_capps.c). */
void ros_selftest_capps(void);

/* SharedCLibrary, the module in front of the ROM C library: its surface
 * against 5.30's, hosted and in the box; registration from native code, in
 * the box (selftest_sclib.c). */
void ros_selftest_sclib(void);
/* The C applications' test hooks, until SharedCLibrary registers clients
 * (package R5): UpCallV claimants for the probes' registration ("R2AB")
 * and for what the runtime keeps of the calling task ("R3TS").  The
 * self-test puts them on and takes them off itself; rosgd.capptest puts
 * them on in the box for probes run by rosgd.run (tests/capps/baton.py). */
void ros_selftest_capps_hooks(int on);

/* What a program starts with: OS_GetEnv, StartApplication, OS_Module 2,
 * OS_PrettyPrint, the Escape bytes (selftest_program.c). */
void ros_selftest_program(void);

/* The HostFS suite's guest executor, "hfstest --serve <dir>", native
 * (hfstest.c): scripts from <dir>.q until <dir>.stop appears. */
int ros_hfstest_serve(const char *dir);

/* Filing systems added by modules, image filing systems (selftest_imagefs.c). */
void ros_selftest_imagefs(void);

#endif

/* SharedSound, native: the handler table, the SWIs, the fill's
 * arithmetic (selftest_sharedsnd.c). */
void ros_selftest_sharedsnd(void);
void ros_selftest_sound(void);
