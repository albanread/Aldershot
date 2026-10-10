/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* rom_contents.c: what the ROM holds, in start order.  First come the
 * native modules, written in C, then the compiled ones.
 *
 * The Buffer Manager is the first module rewritten for Linux.  DRMVideo is
 * the GraphicsV driver over DRM.  Input is the keyboard and pointer driver
 * over evdev.  Internet is the socket interface over Linux's TCP/IP, and the
 * Resolver sits over its name lookup.
 *
 * FileSwitch, ResourceFS, Messages and MessageTrans come first, as in
 * RISC OS's ROM, so that the rest can look up their messages.  FileSwitch
 * brings HostFS with it.  The Territory Manager and the UK territory come
 * next, as there, and are rewritten in C too.
 *
 * Then comes SharedCLibrary, the module in front of the ROM C library.  Its
 * image is in the C ROM and the module maps it.  The Pi ROM has it after the
 * Desktop and before BASIC.  It is the first of what follows the territories
 * there that ROSGD writes in C.
 *
 * The other native modules are rewritten from RISC OS 5's, and their
 * contracts are checked against 5.30.  They stand where the Pi ROM
 * (BuildSys/Components/ROOL/BCM2835) has them, against their neighbours:
 *   - the Screen Blanker, then ShellCLI (the command line at the bottom of
 *     the desktop's screen: F12), before SpriteExtend;
 *   - SpriteUtils, the system sprite area's *commands, after SpriteExtend;
 *   - Squash, RISC OS's LZW, before SuperSample.  Its fast paths are
 *     rewritten in C and its restartable ones are RISC OS's own C;
 *   - ZLib, CompressPNG and CompressJPEG after SuperSample.  5.30 loads
 *     them from !System's Modules and the box has them in its ROM.  They
 *     are native over zlib, libpng and libjpeg-turbo;
 *   - the International module, which names the countries and alphabets
 *     and draws the alphabets in the system font, before
 *     InternationalKeyboard, as there.  IntKey asks it about the keyboard as
 *     it starts;
 *   - HostFSFiler, which puts HostFS's discs on the icon bar once the Filer
 *     asks for filers, before TaskWindow as the Pi ROM's RAMFSFiler is;
 *   - TaskWindow, which runs a command as a Wimp task and sends its output
 *     to its parent's messages;
 *   - BootCommands, the *commands !Boot is written in, before Internet;
 *   - after DHCP, MimeMap.  It holds the file types, MIME types and
 *     extensions that the ROM's MimeMap file maps, and 5.30 has it in its
 *     ROM before !Edit (#106);
 *   - then !Edit, !Draw and !Paint, the modules ModuleWrap makes of Edit,
 *     Draw and Paint.  Their *Desktop_Edit, *Desktop_Draw and *Desktop_Paint
 *     run their images from ResourceFS (modules/modulewrap).  They stand
 *     where the Pi ROM's !Edit, !Draw and !Paint are;
 *   - Filer_Action, after Percussion (the Pi ROM's is after SharedSound).
 *     It is the module that ModuleWrap's FilerAct switch makes.  The Filer
 *     starts its *Filer_Action for each copy, move, delete and the rest.
 *
 * BASICVFP is compiled from ObjAsm.  It is RISC OS 5.31's BASIC, the VFP
 * build, twenty-two source files chained through LNK.  It is the first of
 * the ROM's real modules and the ROM's only BASIC: *BASIC is an alias for
 * it (runtime/sysvars.c).
 *
 * BlendTable is compiled from ObjAsm too.  Its symbols are getall_*, because
 * rosasm names a unit after its source (s/GetAll).  So are ITable and
 * InverseTable, whose one AsmUtils import is a routine of its own
 * (tools/itable_source.py).
 *
 * The desktop's modules are compiled from ObjAsm too.  They start in the Pi
 * ROM's order (BuildSys/Components/ROOL/BCM2835):
 *   - the Window Manager;
 *   - the Desktop;
 *   - the Task Manager (Desktop/Switcher, its symbols switcher_*);
 *   - DragASprite (gets_*, after s/Gets) and DragAnObject;
 *   - the Filer and FilerSWIs;
 *   - Free;
 *   - the Hourglass;
 *   - the Pinboard;
 *   - the Resource Filer (resfiler_*), which the Filer starts with
 *     Service_StartFiler;
 *   - WindowUtils (wimputils2_*, after s/WimpUtils2);
 *   - the Filter Manager, which reads the Wimp's version as it starts.
 * At *Desktop the Task Manager, the Filer, Free and the Pinboard claim
 * Service_StartWimp in that order, as the Desktop module issues it until no
 * one claims it.  The images of the Desktop, WindowUtils, DragASprite and
 * DragAnObject are RISC OS 5.30's modules byte for byte.
 *
 * T0Demo is compiled from ObjAsm.  IntTest and IntTest0 are one file
 * compiled two ways, so that the self-test can hold one against the other.
 * They are code, not modules. */
#include "buffers.h"
#include "drmvideo.h"
#include "fileswitch.h"
#include "input.h"
#include "international.h"
#include "intkey.h"
#include "internet.h"
#include "messagetrans.h"
#include "modulewrap.h"
#include "territory.h"
#include "resolver.h"
#include "inetres.h"
#include "dhcp.h"
#include "acornssl.h"
#include "urlfetcher.h"
#include "pty.h"
#include "sshd.h"
#include "lanmanfs.h"
#include "smbserver.h"
#include "boxtools.h"
#include "hostfsfiler.h"
#include "scrmodes.h"
#include "display.h"
#include "deskclock.h"
#include "waylandwin.h"
#include "deskmeter.h"
#include "speakers.h"
#include "machine.h"
#include "bootcmds.h"
#include "acornhttp.h"
#include "resourcefs.h"
#include "rosgd/module.h"
#include "rosgd/rom.h"
#include "sharedclib.h"
#include "spriteextend.h"
#include "colourtrans.h"
#include "drawmodule.h"
#include "gdraw.h"
#include "smooth.h"
#include "sharedsnd.h"
#include "sound.h"
#include "soundsched.h"
#include "squash.h"
#include "zlibmod.h"
#include "compresspng.h"
#include "compressjpeg.h"
#include "worker.h"
#include "pdriver.h"
#include "rosgd/vdu.h"
#include "portable.h"
#include "fpemulator.h"
#include "basicname.h"
#include "systemdevs.h"
#include "mimemap.h"
#include "cfsi.h"
#include "wimp.h"
#include "supersample.h"
#include "screenblanker.h"
#include "spriteutils.h"
#include "fontmanager.h"
#include "vfpsupport.h"
#include "taskwindow.h"
#include "shellcli.h"
#include "unixbridge.h"
#include "rom_basicvfp.h"
#include "rom_blendtable.h"
#include "rom_itable.h"
#include "rom_wimp.h"
#include "rom_desktop.h"
#include "rom_taskmanager.h"
#include "rom_dragasprite.h"
#include "rom_draganobj.h"
#include "rom_filer.h"
#include "rom_filerswis.h"
#include "rom_hourglass.h"
#include "rom_pinboard.h"
#include "rom_windowutils.h"
#include "rom_filtermgr.h"
#include "rom_free.h"
#include "rom_resfiler.h"
#include "rom_fptest.h"
#include "rom_picker.h"
#include "rom_sparkfs.h"
#include "rom_sparkzip.h"
#include "rom_sparktar.h"
#include "rom_sparkspark.h"
#include "rom_sparklzh.h"
#include "rom_sparkarj.h"
#include "rom_sparkcab.h"
#include "rom_sparkcpio.h"
#include "rom_sparkzoo.h"
#include "rom_sparkmcstuffit.h"
#include "rom_sparkpackddir.h"
#include "rom_tbtoolbox.h"
#include "rom_tbwindow.h"
#include "rom_tbtoolaction.h"
#include "rom_tbmenu.h"
#include "rom_tbiconbar.h"
#include "rom_tbcolourdbox.h"
#include "rom_tbcolourmenu.h"
#include "rom_tbdcs.h"
#include "rom_tbfileinfo.h"
#include "rom_tbfontdbox.h"
#include "rom_tbfontmenu.h"
#include "rom_tbprintdbox.h"
#include "rom_tbproginfo.h"
#include "rom_tbsaveas.h"
#include "rom_tbscale.h"
#include "rom_tbgadgets.h"
#include "rom_drawfile.h"
#include "rom_ctxtest.h"
#include "rom_inttest.h"
#include "rom_inttest0.h"
#include "rom_t0demo.h"

void t0demo_register(void);

/* Each module has a shadow policy (#147).  It says what happens when an ARM
 * module of the same title is loaded.
 * ROS_SHADOW_ALLOW is for the libraries.  They keep per-call or per-caller
 * state, use no machine resource and register in no other module's
 * registry.  The ARM module becomes the shadow.  ARM callers reach it and
 * native callers do not.
 * ROS_SHADOW_BUILTIN is for SharedCLibrary.  It is 5.30's, built in, and it
 * is the shadow for ARM callers.
 * Where the policy is left out, the ARM load is absorbed and the native
 * module serves ARM code too.  A module loaded from a file has its ROM
 * title's policy, or ROSGD$Shadowable's (runtime/module.c). */
const struct ros_rom_native ros_native_modules[] = {
    { &fileswitch_module },
    { &resourcefs_module },
    { &messages_module },
    { &messagetrans_module },
    { &territory_manager_module },
    { &territory_uk_module },
    { &sharedclib_module, ROS_SHADOW_BUILTIN },
    { &fpemulator_module },             /* a stub, before VFPSupport as the Pi
                                         * ROM's: stock !Runs' RMEnsure (#114) */
    { &systemdevs_module },             /* name and version: stock !Runs' RMEnsure */
    { &basicname_module },              /* the same, for RMEnsure BASIC */
    { &vfpsupport_module },
    { &buffer_manager_module },
    { &drmvideo_module },
    { &scrmodes_module },
    { &portable_module },
    { &screenblanker_module },
    { &shellcli_module },
    { &spriteextend_module },
    { &colourtrans_module },
    { &draw_module },
    { &gdraw_module },
    { &smooth_module },
    { &spriteutils_module },
    { &sharedsnd_module },
    { &soundsched_module },             /* before Sound1: its queue's tick
                                         * is what the Sound fill drives */
    { &sound1_module },
    { &sound_module },
    { &wavesynth_module },              /* the voices, in the ROM's order:
                                         * slots 1, 2-5, 6-9 */
    { &stringlib_module },
    { &percussion_module },
    { &fileract_module },               /* after SharedSound and Percussion,
                                         * as the Pi ROM has it */
    { &squash_module, ROS_SHADOW_ALLOW },
    { &zlib_module, ROS_SHADOW_ALLOW }, /* the compressors Paint exports with */
    { &compresspng_module, ROS_SHADOW_ALLOW }, /* (#99): on 5.30's disc in !System, */
    { &compressjpeg_module, ROS_SHADOW_ALLOW }, /* here with Squash */
    { &worker_module },                 /* computation jobs on the spare cores */
    { &pdriver_module },                /* printing: pages out as PDF */
    { &vdisplay_module },               /* virtual displays */
    { &cfsi_module },                   /* ChangeFSI's readers and pixel routines
                                         * (ports/changefsi), over the same codecs */
    { &supersample_module },
    { &fontmanager_module },
    { &romfonts_module },
    { &international_module },
    { &intkey_module },
    { &input_module },
    { &hostfsfiler_module },
    { &display_module },
    { &deskclock_module },              /* DeskClock and DeskMeter start with the */
    { &deskmeter_module },              /* desktop only when asked: rosgd.deskclock, .deskmeter */
    { &waylandwin_module },             /* with rosgd.display=compositor */
    { &speakers_module },               /* *ListSpeakers, *SelectSpeaker */
    { &machine_module },                /* *MachineInfo, *KernelLog, *Reboot, *PowerOff */
    { &taskwindow_module },
    { &bootcmds_module, ROS_SHADOW_ALLOW },
    { &internet_module },
    { &resolver_module },
    { &inetres_module },
    { &dhcp_module },
    { &mimemap_module, ROS_SHADOW_ALLOW }, /* before !Edit, as 5.30's ROM has it */
    { &edit_module },
    { &drawapp_module },
    { &paintapp_module },
    { &acornssl_module },
    { &urlfetcher_module },
    { &acornhttp_module },
    { &pty_module },
    { &unixbridge_module },
    { &sshd_module },
    { &lanmanfs_module },
    { &smbserver_module },
    { &boxtools_module },
};
const unsigned ros_native_module_count = sizeof ros_native_modules / sizeof ros_native_modules[0];
struct ros_module *const ros_native_wimp = &wimp_module;
struct ros_module *const ros_native_filtermgr = &filtermgr_native_module;

const struct ros_rom_image ros_rom_images[] = {
    { "BASICVFP", rom_basicvfp, &rom_basicvfp_base, &rom_basicvfp_size, basicvfp_register, 0, 0,
      ROS_SHADOW_ALLOW },                 /* shadowable */
    { "BlendTable", rom_getall, &rom_getall_base, &rom_getall_size, getall_register, 0 },
    { "InverseTable", rom_itable, &rom_itable_base, &rom_itable_size, itable_register, 0 },
    { "WindowManager", rom_wimp, &rom_wimp_base, &rom_wimp_size, wimp_register, 0 },
    { "Desktop", rom_desktop, &rom_desktop_base, &rom_desktop_size, desktop_register, 0 },
    { "TaskManager", rom_switcher, &rom_switcher_base, &rom_switcher_size, switcher_register, 0 },
    { "DragASprite", rom_gets, &rom_gets_base, &rom_gets_size, gets_register, 0 },
    { "DragAnObject", rom_draganobj, &rom_draganobj_base, &rom_draganobj_size, draganobj_register, 0 },
    { "Filer", rom_filer, &rom_filer_base, &rom_filer_size, filer_register, 0 },
    { "FilerSWIs", rom_filerswis, &rom_filerswis_base, &rom_filerswis_size, filerswis_register, 0 },
    { "Free", rom_free, &rom_free_base, &rom_free_size, free_register, 0 },
    { "Hourglass", rom_hourglass, &rom_hourglass_base, &rom_hourglass_size, hourglass_register, 0 },
    { "Pinboard", rom_pinboard, &rom_pinboard_base, &rom_pinboard_size, pinboard_register, 0 },
    { "ResourceFiler", rom_resfiler, &rom_resfiler_base, &rom_resfiler_size, resfiler_register, 0 },
    { "WindowUtils", rom_wimputils2, &rom_wimputils2_base, &rom_wimputils2_size,
      wimputils2_register, 0 },
    { "FilterManager", rom_filtermgr, &rom_filtermgr_base, &rom_filtermgr_size, filtermgr_register, 0 },
    { "T0Demo", rom_t0demo, &rom_t0demo_base, &rom_t0demo_size, t0demo_register, 0 },
    { "CtxTest", rom_ctxtest, &rom_ctxtest_base, &rom_ctxtest_size, ctxtest_register, 0 },
    { "FPTest", rom_fptest, &rom_fptest_base, &rom_fptest_size, fptest_register, 0 },
    /* The ColourPicker: RISC OS's own module (Apache 2.0), the ROM's first
     * x32 C module, through the clean-room OSLib-compatible library
     * (modules/picker).  Started late: it needs the
     * MessageTrans, the Territory and the Wimp that stand before it.  On
     * since #63: its dialogue opens and redraws (modules/picker/README.md). */
    { "ColourPicker", rom_picker, &rom_picker_base, &rom_picker_size, picker_register, 0, 1,
      ROS_SHADOW_ALLOW },                 /* shadowable: its dialogues the caller's */
    /* SparkFS (ports/sparkfs, #107): the filing system, then its ten
     * codecs, which register with it as they start.  They are x32 or A64X32 C
     * modules from third_party/SparkFS, as the ColourPicker is, after it.
     * The codecs in !SparkFS's Config.Choices's order, the order !RunImage
     * loads them in on RISC OS: SparkFS keeps each in the first free slot
     * of its table, which orders the New archive save box's types (Spark
     * file, Spark dir, PK arc, Tar, Zip, as 5.30's), and starting one again
     * puts it back in its slot.
     * Started with the image filing system off (SparkFSImage 0, its
     * default): !SparkFS's !Run turns it on and !RunImage starts the codecs
     * again, which declare their archive types then, as when !Run loads
     * them on RISC OS.  SparkFS$Memory unset, a dynamic area of up to 32 MB
     * (patches/rom-memory.patch: what !Run sets on RISC OS 5). */
    { "SparkFS", rom_sparkfs, &rom_sparkfs_base, &rom_sparkfs_size, sparkfs_register, 0, 1 },
    { "Spark", rom_sparkspark, &rom_sparkspark_base, &rom_sparkspark_size, sparkspark_register, 0, 1 },
    { "Tar", rom_sparktar, &rom_sparktar_base, &rom_sparktar_size, sparktar_register, 0, 1 },
    { "Zip", rom_sparkzip, &rom_sparkzip_base, &rom_sparkzip_size, sparkzip_register, 0, 1 },
    { "Lzh", rom_sparklzh, &rom_sparklzh_base, &rom_sparklzh_size, sparklzh_register, 0, 1 },
    { "ARJ", rom_sparkarj, &rom_sparkarj_base, &rom_sparkarj_size, sparkarj_register, 0, 1 },
    { "Cab", rom_sparkcab, &rom_sparkcab_base, &rom_sparkcab_size, sparkcab_register, 0, 1 },
    { "CPIO", rom_sparkcpio, &rom_sparkcpio_base, &rom_sparkcpio_size, sparkcpio_register, 0, 1 },
    { "Zoo", rom_sparkzoo, &rom_sparkzoo_base, &rom_sparkzoo_size, sparkzoo_register, 0, 1 },
    { "McStuffit", rom_sparkmcstuffit, &rom_sparkmcstuffit_base, &rom_sparkmcstuffit_size, sparkmcstuffit_register, 0, 1 },
    { "PackdDir", rom_sparkpackddir, &rom_sparkpackddir_base, &rom_sparkpackddir_size, sparkpackddir_register, 0, 1 },
    /* The Toolbox (ports/toolbox): RISC OS's own modules (Apache 2.0),
     * x32 or A64X32 C modules from ROOL's sources, as the ColourPicker is.
     * The Toolbox comes first, then Window.  The object modules register with
     * the Toolbox as they start and the gadget modules with Window.  They
     * are in the order RISC OS's ROM holds them. */
    { "Toolbox", rom_tbtoolbox, &rom_tbtoolbox_base, &rom_tbtoolbox_size, tbtoolbox_register, 0, 1 },
    { "Window", rom_tbwindow, &rom_tbwindow_base, &rom_tbwindow_size, tbwindow_register, 0, 1 },
    { "ToolAction", rom_tbtoolaction, &rom_tbtoolaction_base, &rom_tbtoolaction_size, tbtoolaction_register, 0, 1 },
    { "Menu", rom_tbmenu, &rom_tbmenu_base, &rom_tbmenu_size, tbmenu_register, 0, 1 },
    { "Iconbar", rom_tbiconbar, &rom_tbiconbar_base, &rom_tbiconbar_size, tbiconbar_register, 0, 1 },
    { "ColourDbox", rom_tbcolourdbox, &rom_tbcolourdbox_base, &rom_tbcolourdbox_size, tbcolourdbox_register, 0, 1 },
    { "ColourMenu", rom_tbcolourmenu, &rom_tbcolourmenu_base, &rom_tbcolourmenu_size, tbcolourmenu_register, 0, 1 },
    { "DCS", rom_tbdcs, &rom_tbdcs_base, &rom_tbdcs_size, tbdcs_register, 0, 1 },
    { "FileInfo", rom_tbfileinfo, &rom_tbfileinfo_base, &rom_tbfileinfo_size, tbfileinfo_register, 0, 1 },
    { "FontDbox", rom_tbfontdbox, &rom_tbfontdbox_base, &rom_tbfontdbox_size, tbfontdbox_register, 0, 1 },
    { "FontMenu", rom_tbfontmenu, &rom_tbfontmenu_base, &rom_tbfontmenu_size, tbfontmenu_register, 0, 1 },
    { "PrintDbox", rom_tbprintdbox, &rom_tbprintdbox_base, &rom_tbprintdbox_size, tbprintdbox_register, 0, 1 },
    { "ProgInfo", rom_tbproginfo, &rom_tbproginfo_base, &rom_tbproginfo_size, tbproginfo_register, 0, 1 },
    { "SaveAs", rom_tbsaveas, &rom_tbsaveas_base, &rom_tbsaveas_size, tbsaveas_register, 0, 1 },
    { "Scale", rom_tbscale, &rom_tbscale_base, &rom_tbscale_size, tbscale_register, 0, 1 },
    { "TextGadgets", rom_tbgadgets, &rom_tbgadgets_base, &rom_tbgadgets_size, tbgadgets_register, 0, 1 },
    /* DrawFile (ports/drawfile): RISC OS's own module (Apache 2.0), from
     * ROOL's sources through oslcr, as the Toolbox's are */
    { "DrawFile", rom_drawfile, &rom_drawfile_base, &rom_drawfile_size, drawfile_register, 0, 1 },
    { "IntTest", rom_inttest, &rom_inttest_base, &rom_inttest_size, inttest_register, 1 },
    { "IntTest0", rom_inttest0, &rom_inttest0_base, &rom_inttest0_size, inttest0_register, 1 },
};
const unsigned ros_rom_image_count = sizeof ros_rom_images / sizeof ros_rom_images[0];
