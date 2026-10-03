# Supported Software Components

*3 October 2026*

## Introduction

This article lists the operating system components in BOX's ROM, how
each one is provided, and how many SWIs it supports. It describes the
Apple silicon box's ROM as built on 3 October 2026. The Intel box's is
the same.

Each component is provided in one of six ways:

| How | Meaning |
| --- | --- |
| **Runtime** | part of `/init` itself: RISC OS's kernel, written in C |
| **Native** | the module rewritten in C, from its sources, the PRMs and RISC OS 5.30's behaviour |
| **Shim** | the module's SWIs and commands kept, with the work done by Linux or a Linux library |
| **New** | a module of BOX's own, with no RISC OS original |
| **Translated** | RISC OS's own ObjAsm, compiled to C by ROSASM (see *Tiers of Translation*) |
| **C module** | RISC OS's own C source, compiled for BOX (x32 or A64X32) |

The SWI counts are the named SWIs in each module's interface as BOX
provides it, taken from the build: for native modules, the SWIs with a
handler; for translated and C modules, the SWIs in the module's own
SWI table. A dash means the module has no SWIs of its own; many
modules work through `*` commands, service calls or vectors instead.

---

## 1. The kernel

| Component | How | SWIs |
| --- | --- | --- |
| Kernel | Runtime | 122, and the 256 `OS_WriteI` SWIs |

The kernel's SWIs (`&00` to `&FF`) are functions of the runtime. They
cover:

* **Characters and the VDU:** `OS_WriteC`, `OS_Write0`, `OS_WriteN`,
  `OS_NewLine`, `OS_ReadC`, `OS_ReadLine`, `OS_Plot`, `OS_ReadPoint`,
  `OS_SetColour`, `OS_ReadPalette`, `OS_ReadVduVariables`,
  `OS_ReadModeVariable`, `OS_ScreenMode`, `OS_CheckModeValid`,
  `OS_SpriteOp` and the cursor calls
* **Files:** `OS_File`, `OS_Find`, `OS_Args`, `OS_BGet`, `OS_BPut`,
  `OS_GBPB` and `OS_FSControl`, which pass through the file vectors to
  FileSwitch
* **Modules, vectors and events:** `OS_Module`, `OS_Claim`,
  `OS_Release`, `OS_CallAVector`, `OS_AddToVector`, `OS_ServiceCall`,
  `OS_GenerateEvent`, `OS_UpCall`, callbacks and tickers
* **Memory:** `OS_Heap`, `OS_DynamicArea`, `OS_ChangeDynamicArea`,
  `OS_ReadDynamicArea`, `OS_AMBControl`, `OS_Memory`,
  `OS_ValidateAddress`
* **The command line and variables:** `OS_CLI`, `OS_ReadArgs`,
  `OS_ReadVarVal`, `OS_SetVarVal`, `OS_GSTrans` and its family,
  `OS_EvaluateExpression`, `OS_SubstituteArgs`
* **The environment:** `OS_Exit`, `OS_GetEnv`,
  `OS_ChangeEnvironment`, `OS_ReadDefaultHandler`, `OS_GenerateError`,
  `OS_ChangeRedirection`
* **OS_Byte and OS_Word**, the mouse and the pointer, the keyboard
  handler, the monotonic clock, `OS_ReadSysInfo`, `OS_PlatformFeatures`,
  `OS_NVMemory`, `OS_Reset`
* **Conversions:** all 30 `OS_Convert…` SWIs, `OS_BinaryToDecimal`,
  `OS_ReadUnsigned`, `OS_SWINumberToString` and `OS_SWINumberFromString`
* **Sorting and checks:** `OS_HeapSort`, `OS_HeapSort32`, `OS_CRC`

---

## 2. Files and resources

| Module | How | SWIs | Notes |
| --- | --- | --- | --- |
| FileSwitch | Native | — | the `OS_File` family, through the file vectors; HostFS built in |
| HostFS | Shim | — | part of FileSwitch: Linux's files, one disc per mount |
| ResourceFS | Native | 2 | the ROM's files, read-only |
| Messages | Native | — | registers the ROM's own files |
| MessageTrans | Native | 10 | |
| HostFSFiler | New | — | each HostFS disc on the icon bar |
| LanManFS | Shim | — | SMB2/3 shares, through Linux's client |
| SparkFS | C module | 7 | archives as directories |
| SparkFS codecs (10) | C module | — | Spark, Tar, Zip, Lzh, ARJ, Cab, CPIO, Zoo, McStuffit, PackdDir |
| MimeMap | Native | 1 | |

---

## 3. The desktop

| Module | How | SWIs | Notes |
| --- | --- | --- | --- |
| WindowManager | Native | 62 | written to a specification (see *The Native Wimp*) |
| FilterManager | Native | 12 | answered by the native Wimp |
| Desktop | Translated | — | |
| TaskManager | Translated | 4 | |
| Filer | Translated | — | |
| FilerSWIs | Translated | 3 | the `FilerAction` SWIs |
| Filer_Action | Native | — | RISC OS's own C application, made a module by ModuleWrap |
| Pinboard | Translated | — | |
| ResourceFiler | Translated | — | |
| Free | Translated | 2 | |
| Hourglass | Translated | 7 | |
| DragASprite | Translated | 2 | |
| DragAnObject | Translated | 2 | |
| WindowUtils | Translated | — | |
| TaskWindow | Native | 1 | |
| ShellCLI | Native | 2 | the F12 command line |
| ColourPicker | C module | 9 | |
| DisplayManager | Native | — | hand-converted from its ObjAsm |
| ScreenModes | Native | 3 | |
| ScreenBlanker | Native | 1 | |
| DeskClock, DeskMeter | New | — | a clock and a load meter on the icon bar |

The translated WindowManager and FilterManager are also in the ROM, and
can be chosen at boot with `rosgd.wimp=translated`.

---

## 4. Graphics and fonts

| Module | How | SWIs | Notes |
| --- | --- | --- | --- |
| DRMVideo | Shim | — | the GraphicsV driver, over Linux's DRM |
| ColourTrans | Native | 36 | |
| SpriteExtend | Native | 7 | JPEG through libjpeg-turbo |
| SpriteUtils | Native | — | the system sprite area's commands |
| Draw | Native | 16 | |
| GDraw | New | 25 | anti-aliased drawing |
| Smooth | New | — | the switch that sends the desktop's drawing through GDraw |
| FontManager | Native | 43 | |
| ROMFonts | Native | — | |
| SuperSample | Native | 2 | |
| BlendTable | Translated | 2 | |
| InverseTable | Translated | 2 | |

---

## 5. Sound

| Module | How | SWIs | Notes |
| --- | --- | --- | --- |
| SoundDMA | Shim | 7 | over Linux's ALSA |
| Sound1 | Native | 13 | the 8-bit voice system |
| SoundScheduler | Native | 7 | |
| SharedSound | Native | 15 | |
| WaveSynth, StringLib, Percussion | Native | — | the voices |
| Speakers | New | — | which output sound goes to |

---

## 6. Input, territory and system

| Module | How | SWIs | Notes |
| --- | --- | --- | --- |
| Input | Shim | — | keyboard and pointer, over Linux's evdev |
| International | Native | — | |
| InternationalKeyboard | Native | — | |
| TerritoryManager | Native | 40 | |
| UK territory | Native | — | |
| BufferManager | Native | 10 | |
| Portable | Native | 19 | |
| BootCommands | Native | — | the `*` commands `!Boot` is written in |
| Machine | New | — | `*MachineInfo`, `*Reboot`, `*PowerOff` |

---

## 7. Languages and libraries

| Module | How | SWIs | Notes |
| --- | --- | --- | --- |
| BASICVFP | Translated | — | the only BASIC; `*BASIC` is an alias for it |
| SharedCLibrary | Native | 5 | the module in front of the ROM's C library, which is RISC_OSLib's own C compiled for BOX |
| VFPSupport | Native | 11 | |
| FPEmulator | Native | 10 | a stub, so `RMEnsure FPEmulator` lines pass; nothing is emulated |
| Squash | Native | 2 | |
| ZLib | Shim | 38 | over zlib |
| CompressPNG | Shim | 4 | over libpng |
| CompressJPEG | Shim | 4 | over libjpeg-turbo |
| CFSI | New | 3 | ChangeFSI's routines, in C |
| Worker | New | 9 | computation on the spare processor cores |
| BoxTools | New | — | `*CC`, `*RosBas`, `*RunBox` (see *BOX Tools*) |

---

## 8. Networking

| Module | How | SWIs | Notes |
| --- | --- | --- | --- |
| Internet | Shim | 31 | sockets, over Linux's |
| Resolver | Shim | 3 | over Linux's resolver |
| DHCP | Shim | 6 | over the kernel's DHCP |
| InetRes | Shim | — | `*Ping`, `*IfConfig` and the other network tools |
| AcornSSL | Shim | 16 | over OpenSSL 3 |
| URL_Fetcher | Native | 12 | |
| AcornHTTP | Shim | 13 | over libcurl |
| SMBServer | Shim | — | `*Share`, over Linux's ksmbd |
| SSHD | Shim | — | `*SSHD`, OpenSSH's server |
| PTY | New | 6 | Linux programs on a pseudo-terminal, such as `*SSH` |
| UnixBridge | New | 1 | Linux system calls for POSIX programs |

---

## 9. Applications in the ROM

`!Edit`, `!Draw`, `!Paint`, `!SparkFS` and `!ChangeFSI` are in
`Resources:$.Apps`. Edit, Draw and Paint are RISC OS's own C,
recompiled. Each is made into a module by ModuleWrap, as in RISC OS's
ROM, and started by its `*Desktop_` command.

---

## 10. In total

| | Modules | SWIs |
| --- | --- | --- |
| Kernel (runtime) | — | 122, and 256 `OS_WriteI` |
| Native, shim and new modules | 68 | 508 |
| Translated modules | 14 | 24 |
| C modules | 12 | 16 |
| **Total** | **94** | **670**, and 256 `OS_WriteI` |

The module counts include the ten SparkFS codecs and the module forms
of Edit, Draw, Paint and Filer_Action. HostFS is counted as part of
FileSwitch. They do not include the
translated WindowManager and FilterManager held in reserve, or the test
modules the self-test uses.
