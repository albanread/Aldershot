# Bugs and quirks in RISC OS and its applications

*8 October 2026*

Porting RISC OS 5.30 and its applications to BOX means reading their source
line by line and running them side by side with 5.30 on the test farm. Along
the way the project has noticed things in the **original** software that are
bugs, or quirks, or places where the manuals are wrong. This file gathers them
in one place.

None of these are faults in BOX. Where BOX reproduces an original's behaviour
on purpose, so that programs behave as they do on 5.30, the behaviour is listed
here under the original.

**Kind** is the project's judgement:

| Kind | Meaning |
| --- | --- |
| Bug | The original does something wrong: a crash, a hang, a wrong result, memory overwritten. |
| Quirk | Surprising or undocumented, but possibly intended, and often relied on. |
| Doc error | A manual, PRM or help text says something the software does not do. |
| Unclear | Noticed, but the cause, or whether it is the original's fault, was not settled. |

Most items were found by reading the source while translating it. Those marked
**[M]** were also measured on the farm's RISC OS 5.30 machine. Some were seen
only in the source and may never trouble a real program.

**Sources** say where each item was recorded, in the project's own
repositories, which are not yet published (see *Source code* in the
[README](../README.md)): `rosgd/...` (the BOX sources), `GD/...` (the design
and specification documents), `rosbas/...` (the BBC BASIC compiler), `#n` and
`P#n` (the project's issue lists) and *notes* (its working notes).

Please do not report these to the original authors on BOX's account: see
*Ports* in the [README](../README.md).

Contents: [Applications](#1-applications-and-third-party-software) ·
[BBC BASIC](#2-bbc-basic-basicvfp-185) · [Window Manager](#3-the-window-manager) ·
[Kernel and VDU](#4-kernel-vdu-and-os-swis) · [Other modules](#5-other-risc-os-modules) ·
[C library](#6-sharedclibrary-risc_oslib-and-the-ddes-c) · [Headers](#7-headers-and-build)

---

## 1. Applications and third-party software

### StrongHelp 2.90 and its reference manual

| What was noticed | Kind | Source |
| --- | --- | --- |
| SH-RefMan says `#Wrap On` joins a paragraph's lines. 2.90 joins none: under any Wrap setting but Off, every source line is a paragraph of its own. [M] | Doc error | rosgd/ports/stronghelp/README.md; GD/spec/stronghelp/02-pages.md |
| SH-RefMan says an object's length does not count the guard and length words. 2.90 uses the whole block's size, so the data is the length less 8. | Doc error | GD/spec/stronghelp/01-manuals.md |
| An empty `#fN:` line is drawn as a blank line in that font, a spacer. Not documented. [M] | Quirk | rosgd/ports/stronghelp/README.md |
| A sprite's label is plain text, not in the link style; a window is as wide as its widest line, and has scroll bars only when needed. Not documented. [M] | Quirk | rosgd/ports/stronghelp/README.md |
| 5.30's own manuals link to subpages as `Page.subpage` (OS_ReadSysInfo.1), a form SH-RefMan does not describe. | Doc error | rosgd/ports/stronghelp/pages.c |
| 2.90 passes the Wimp an icon validation pointer of &2370, a junk low address. It works on 5.30 only because DebuggerSpace there reads as zeroes. | Bug | #180 |
| `StartUp.OldCmds` uses `*Alias`, which is not a kernel command: it is defined by !Boot's PreDesktop. Without that, `!Run` fails with "File 'Alias' not found". | Quirk | *notes* |
| 5.30's OS manual has dead links: `FileTypes:FF8`, `Portable_Idle`, `Portable_Stop`, `Portable_SMBusOp`, `Assembly:Rn`, a subpage `.fill` that is not there, and many links to PRM manuals that are not installed. | Doc error | rosgd/tests/stronghelp/box_unit.c (run over the OS manual) |

### StrongED 4.70a22

| What was noticed | Kind | Source |
| --- | --- | --- |
| `OpenDialogueBox SearchReplace` jumps into the next routine: there are 20 keywords and 19 jump entries. | Bug | GD/spec/stronged/03-functions.md |
| The Mark functions never work: a `BIC` leaves type bit 29 set. History has the same fault. | Bug | GD/spec/stronged/03-functions.md |
| `ModeList` stores corrupt names; StrongKeys' `CMN R0,R1` accepts unknown key names; CopyCursor with a count over 64 overflows the stack; `ProcessLoW Text` takes every following text. | Bug | GD/spec/stronged/02-modes-config.md, 03-functions.md |
| The menu title's width counts a pointer's bytes; SplitString takes its terminator from the wrong byte; EndOfTag looks for byte 10 past a CR line; a node default is taken one byte too far; an empty word is added to the tree. | Bug | rosgd/ports/stronged/msgfile.c, syntax.c |
| The longest-match keyword trie never falls back to a shorter keyword; embedded options write into the mode record; a comment-slot digit other than 1 selects slot 2; Backspace at the end of a selection copies it to the clipboard. | Quirk | GD/spec/stronged/01-architecture.md, 02-modes-config.md |
| Mode file parsing: a TAB ends a list; a stray character counts as a comma; `"\n"` is read as "n"; numbers are 29-bit; a leading `\` adds `\` to an ID set; `*x` in Tabstops takes x's character code. | Quirk | rosgd/tests/stronged/func_unit.c, test_s5.c; rosgd/ports/stronged/mode.c |
| `FindEor` passes a GCOL where a colour number is expected. | Unclear | GD/spec/stronged/04-display.md |
| The manual says `{.}` matches to the end of the line; it eats the closing quote, so there is no match. | Doc error | rosgd/tests/stronged/test_s6.c |
| The manual and the code differ: undo is not purged on save; NIL consumes the key; marks are 1 to 8, not 0 to 7; Learn holds 127 keys, not about 250; the manual's `AutoRefresh` form gives error 61; `\cX` is not a control code; "`\\` 0x2D" should be &5C; the display font size list is out of date. | Doc error | GD/spec/stronged/03-functions.md, 04-display.md, 05-search-dialogues-messages.md |
| StrongED's Wimp message list leaves out Message_HelpWord (&43B00), yet its F1 path depends on that message bouncing back to it. | Unclear | *notes* |

### PipeDream 4.63

| What was noticed | Kind | Source |
| --- | --- | --- |
| The first printer driver load compares against `current_driver_name` while it is still NULL. It survives on 5.30 only because page zero is readable. | Bug | rosgd/ports/pipedream/PORT_NOTES.md; #171 |
| The stock `!Run`'s nested If/Then/Else for seeding Choices assumes `Choices$Write` is set; under 5.30's `*If` rules (section 4) it misfires when it is not. | Quirk | #2, #7, #60 |
| A bare `%*` in its `!Run` is passed through literally by the kernel; `%*0` is needed. | Quirk | rosgd/ports/pipedream/PORT_NOTES.md |

### OvationPro 2.78

| What was noticed | Kind | Source |
| --- | --- | --- |
| Calls OS_SpriteOp &238 with R1 = &FF, which is not a sprite area. | Bug | rosgd/boot/selftest_jpegplot.c |
| Its Wimp_SendMessage reason 19 carries a garbage size word. | Bug | rosgd/tests/deskprobe/ovationpro.py |
| Deselects its print job (PDriver_SelectJob 0) between bands. | Quirk | rosgd/modules/pdriver/pdriver.c |
| Accepts Message_ClaimEntity but never sends DataRequest, so it cannot paste from another task's clipboard, on 5.30 too. | Quirk | #142 |
| Polls about 130,000 times a second on null events while idle. Not checked on 5.30. | Unclear | #138 |
| After printing, one character is redrawn on screen at page scale. Not checked on 5.30. | Unclear | #189 |

### ChangeFSI 1.70

| What was noticed | Kind | Source |
| --- | --- | --- |
| `plancnv%` loads a word from a byte address, which aborts on the A72 (an IFF/ILBM picture). [M] | Bug | rosgd/ports/changefsi/README.md |
| CFSIpng does not start each Adam7 interlace pass from a zero row, so interlaced PNGs come out wrong (rows 4, 12, 20). | Bug | rosgd/ports/changefsi/README.md |
| Still calls OS_UpdateMEMC, a MEMC1 relic, at start-up. | Quirk | #120 |
| An error ends the whole Obey file it runs from, so each conversion needs a job of its own. | Quirk | *notes* |

### SparkFS

| What was noticed | Kind | Source |
| --- | --- | --- |
| `t_accum` shifts by 32, undefined C that depends on the ARM; SparkFSApp writes into string literals, which depends on Norcroft not merging them. | Bug (latent) | rosgd/ports/sparkfs/README.md |
| SparkCPIO's `strtoul` reads hex fields that are not terminated, so after an odc archive has been read 5.30 calls a newc archive "Bad archive" until a reset. | Bug | rosgd/ports/sparkfs/README.md |
| `*IfThere` with an unset path variable raises an error rather than reporting false. | Quirk | rosgd/ports/sparkfs/README.md |

### Paint, Draw and Edit (5.30)

| What was noticed | Kind | Source |
| --- | --- | --- |
| Paint: the first click in a window without the focus only takes the focus; Copy block leaves selection boxes behind; a black margin is drawn past a sprite at 1:1. | Quirk | rosgd/ports/paint/README.md |
| Paint depends on one case of CompressJPEG's stale longjmp (section 5) surviving. | Quirk | rosgd/modules/compressjpeg/README.md |
| `*Help Desktop_Draw` runs on to the end of the Messages file: Draw's help text has no terminating NUL. | Bug | rosgd/modules/modulewrap/README.md |
| Draw files hold doubles in FPA word order. | Quirk | rosgd/tools/capps.py |
| Edit with `Edit$Path` unset shows an OK box, then "Return code too large". | Quirk | rosgd/modules/modulewrap/README.md |
| The Filer relies on undocumented Wimp internals: the Wimp's sprite routines through Wimp_Extend 1, 2 and 4, viewer sizing through reason 11, and the window tile being plotted behind an unselected icon's text. Filer_Action's Newer test relies on OS_File 5's undocumented result for a missing object. | Quirk | rosgd/modules/wimp/README.md; rosgd/ports/filer_action/README.md |

### ArtWorks modules (GDraw 3.12, DitherExtend)

The original ARM modules, Computer Concepts and MW Software's GDraw 3.12
(17 March 2011) and DitherExtend, as shipped with !AWViewer, measured on the
farm's RISC OS 5.30.

| What was noticed | Kind | Source |
| --- | --- | --- |
| With anti-aliasing, the OR and EOR actions give output that differs from run to run; AND and invert act as overwrite. [M] | Bug | GD/spec/gdraw/01-riscos-module.md |
| Fill style bits 8 and 9 = 2 hang the module. [M] | Bug | GD/spec/gdraw/01-riscos-module.md |
| Fill &38 differs from Draw's on pixel-boundary edges (rows 5 to 24 against 6 to 25); below 8 bpp it blends colour numbers, not colours. [M] | Quirk | GD/spec/gdraw/03-algorithms.md |
| DitherExtend pokes the VDU's ORA and EOR tables directly, through VDU variable 171. | Quirk | *notes* |

### MakePSFont

| What was noticed | Kind | Source |
| --- | --- | --- |
| `get_angle` has two constants swapped, so a 12° italic comes out at 0.37°. | Bug | rosgd/docs/articles/tiers-of-translation.md |

### HERMES-Paige (the open source Paige text engine)

| What was noticed | Kind | Source |
| --- | --- | --- |
| Its 64-bit pass made text offsets `size_t`, so `offset-1 >= run` became an unsigned compare and the first insert ran for ever. | Bug | rosgd/ports/paige/README.md |
| PGMEMMGR's `size_t next_master < 0` is never true, so after 256 references it writes at index -1 and corrupts the heap. | Bug | rosgd/ports/paige/README.md |
| The generic measure procedure writes [i] where [i+1] is the right edge; an assignment to `start_target` was lost. | Bug | rosgd/ports/paige/README.md |
| RTF: the Unicode destinations table is short; export counts characters as bytes; `\ul0` turns italic off and `\ulnone` is unknown; a backslash at a line's end is dropped rather than taken as `\par`; newlines get into font names. | Bug | rosgd/ports/paige/README.md |
| `pgSetStyleBits` toggles bits that are already on, so callers must clear and then set. | Quirk | *notes* |

---

## 2. BBC BASIC (BASICVFP 1.85)

### The sixteen quirks the rosbas compiler can reproduce

rosbas compiles BBC BASIC to C. By default it does not reproduce these; each
can be switched on with `--quirk`, for programs that depend on it.

| Quirk | What was noticed | Kind | Source |
| --- | --- | --- | --- |
| int-add-wrap | Integer `+`, `-` and unary minus wrap modulo 2^32: `&7FFFFFFF+1` is -2147483648. Scalar `*` does not wrap. Not in the manual. | Quirk | rosbas/spec/C-quirks.md |
| int-array-mul-wrap | Whole-array `*`, the integer matrix product and array MOD wrap silently. | Bug | rosbas/spec/C-quirks.md |
| inf-leak | `2^1024` gives an infinity, printed `8E@65`, which the manual says cannot happen. | Bug | rosbas/spec/C-quirks.md |
| tan-reduce | TAN near odd multiples of π/2 is wrong by 10^4 to 10^11, often in sign: `TAN(PI)` is -1.2E-10 (VFPSupport, section 5). | Bug | rosbas/spec/C-quirks.md |
| pow-odd | `(-1)^1048576` is -1: exponents from 2^20 to 2^21 are taken as odd. Very large exponents raise error 20 or give 0. | Bug | rosbas/spec/C-quirks.md |
| cond-truncate | A real condition is truncated first: `IF 0.5` is false, and `IF 3E9` raises error 55. The manual says any non-zero value is true. | Bug | rosbas/spec/C-quirks.md |
| for-byte-word | With a `?` control variable, NEXT reads and writes a whole word, overwriting the three bytes after it. | Bug | rosbas/spec/C-quirks.md |
| error-keeps-locals | An error caught by ON ERROR leaves abandoned routines' LOCALs at their inner values, and RETURN parameters are not written back. | Bug | rosbas/spec/C-quirks.md |
| array-late-fp-error | Real whole-array operations store every element, Inf and NaN included, and raise the error only at the end. | Bug | rosbas/spec/C-quirks.md |
| dollar-no-cr | `$a` with no CR in the first 256 bytes gives "" silently. | Bug | rosbas/spec/C-quirks.md |
| five-byte-zero | INPUT# reads BASIC V's five-byte zero as 2^-129, not 0. | Bug | rosbas/spec/C-quirks.md |
| sys-string-255 | A string returned by `SYS ... TO s$` of 255 bytes or more comes back empty. | Bug | rosbas/spec/C-quirks.md |
| stale-local-handler | An ON ERROR LOCAL handler outlives its routine or loop; entering it later loops for ever. This wedged the farm twice. The manuals give no warning. [M] | Bug | rosbas/spec/C-quirks.md, 09-errors.md |
| restore-data-pop | A failing RESTORE DATA has already popped a word, so a local handler at the same level cannot return, and loops for ever. | Bug | rosbas/spec/C-quirks.md |
| instr-start-wrap | `INSTR(s,t,p)` with p of 256 or more searches from byte 1, so a loop stepping through the matches never ends. | Bug | rosbas/spec/C-quirks.md |
| substr-assign-range | LEFT$, MID$ or RIGHT$ assignment with a length of 0, or a start out of range, still overwrites the start of the string. | Bug | rosbas/spec/C-quirks.md |

### Found in BASICVFP's source and on the farm

| What was noticed | Kind | Source |
| --- | --- | --- |
| A decimal literal is an integer only below &0CC00000: 213909503 is an integer, 213909504 a real. | Quirk | rosbas/spec/03-values.md; rosgd/modules/basicvfp/tier3/units/factor_const.c |
| A numeric literal keeps at most 18 mantissa digits; the tokeniser drops a digit that takes a line number constant past 65280. | Quirk | rosgd/modules/basicvfp/tier3/units/factor_const.c, lexical_match.c |
| Binary `%` literals have no overflow test: a 33rd digit wraps silently. Hex rejects a 9th digit, but consumes it. | Bug | rosgd/modules/basicvfp/tier3/units/factor_core.c |
| RND(0) gives the fraction of the current state, whichever RND form set it; the manual says it repeats the last RND(1). | Doc error | rosbas/spec/05-expressions.md |
| A fresh workspace's zero seed is replaced by "ARW!", so RND gives the same sequence at every start. | Quirk | rosgd/modules/basicvfp/tier3/units/modmain.c |
| SWAP of byte-type variables exchanges nothing: the loop runs TYPE times, and type 0 fails the first test. The source's comment says it swaps one byte. | Bug | rosgd/modules/basicvfp/tier3/units/dispat_assign.c |
| IF's ELSE scan steps two bytes at a time in places, so a colon before ELSE is found only at even alignments. | Bug | rosgd/modules/basicvfp/tier3/units/dispat_control.c |
| FOR with an empty range runs its body once. | Quirk | rosgd/tests/desktop/taskwindow/README.md |
| ON with nothing after it turns the cursor on. | Quirk | rosgd/modules/basicvfp/tier3/units/dispat_control.c |
| SOUND packs channel and amplitude reading the stacked halves in the wrong rotation order. | Bug | rosgd/modules/basicvfp/tier3/units/dispat_io.c |
| PRINT# writes numbers high byte first and strings low byte first. | Quirk | rosgd/modules/basicvfp/tier3/units/dispat_io.c |
| ADC does not mask OS_Byte's R2, so its high bits leak in. The source says "removed since GStark claims its OK". | Bug | rosgd/modules/basicvfp/tier3/units/factor_resid.c |
| WIDTH returns the stored value plus one; TO not followed by P errors with the P already consumed. | Quirk | rosgd/modules/basicvfp/tier3/units/factor_resid.c |
| Out-of-range assignments to PAGE, LOMEM and HIMEM print a message and carry on instead of raising an error. | Quirk | rosgd/modules/basicvfp/tier3/units/dispat_assign.c |
| AUTO stops at 65280, and LIST's default end is 65279. | Quirk | rosgd/modules/basicvfp/tier3/units/clrstk.c |
| MOUSE TO passes OS_Word an unaligned block. | Bug (latent) | rosgd/modules/basicvfp/tier3/units/dispat_sys.c |
| TAB, SPC and comma padding keep only the low byte of the space count. | Quirk | rosbas/spec/10-output.md |
| RIGHT$ with n of -33025 or less returns the whole string, through an address wrap at &8100. | Quirk | rosbas/spec/06-strings.md |
| `\|` at an address that is not a multiple of 4 aborts. | Quirk | rosbas/spec/12-memory.md |
| A CR, NUL or space inside an OPENIN name ends the name: `OPENIN("data junk")` opens `data`. | Quirk | rosbas/spec/13-files.md |
| A text file whose last line has no line end makes TEXTLOAD read past the end of the file; 5.30 said "Line too long". [M] | Bug | rosbas/spec/02-source.md |
| A line whose tokenised form overflows the loader's 512-byte buffer hung 5.30. [M] | Bug | rosbas/spec/02-source.md |
| A typed LOAD of a text file says "Bad program". | Quirk | *notes* |
| `s$()=1+t$()` gives "Internal error: abort on data transfer at &FC1E1CF0". [M] | Bug | rosbas/spec/04-variables.md |
| Overflow in a real matrix product stores every element and then aborts (&FC1DA198) instead of raising error 20; the product does not check C's dimensions; divide by zero in a whole-array operation gives a damaged message. [M] | Bug | rosbas/spec/04-variables.md |
| RESTORE ERROR after a LOCAL ERROR that has been restored automatically raises "control status not found on stack". | Quirk | *notes* |

---

## 3. The Window Manager

Most of these come from reading WindowManager 5.88's source for the native
Wimp's specification; the farm's 5.30 runs 5.87.

### Icons and plotting

| What was noticed | Kind | Source |
| --- | --- | --- |
| Wimp_ResizeIcon's `CMP R1,R0; BHI` accepts handle = icon count and writes 16 bytes past the icon array; in a window with no icons, handle 0 aborts at &FC17A0E8. A report for ROOL was drafted. [M] | Bug | GD/spec/wimp/04-icons.md |
| A bad icon handle returns the untranslated token "BadIconHandle" as the error text; a bad Wimp_TextOp reason returns "BadReasonCode". | Bug | GD/spec/wimp/04-icons.md |
| Wimp_ResizeIcon corrupts R0 (it holds the icon's internal address); PRM5A says R0 to R5 are preserved. | Doc error | GD/spec/wimp/04-icons.md |
| Wimp_TextOp 2 acts on flag bits 28 and 29, which PRM5A reserves, and bit 29 outside a redraw reads through a null window pointer. TextOp 1 bit 31 is documented but not implemented. | Bug | GD/spec/wimp/04-icons.md |
| Wimp_PlotIcon outside a redraw adds the caller's R4 and R5 to the box (PRM3 says screen coordinates) and uses the background of whichever window was last redrawn. | Quirk | GD/spec/wimp/04-icons.md |
| Wimp_GetIconState for a handle at or above the count gives no error, and fills the block with stale register contents. [M] | Bug | GD/spec/wimp/04-icons.md |
| Wimp_WhichIcon compares `(flags AND R2) = R3`, not `R3 AND R2` as PRM3 says, and does not skip deleted icons. | Doc error | GD/spec/wimp/04-icons.md |
| The R highlight default is 3 (PRM: 14); the text-plus-sprite gap is 6 (PRM: 12); the 2 bpp sprite colour map is 0,2,5,7 (PRM: 0,2,4,7). | Doc error | GD/spec/wimp/04-icons.md |
| Selected sprites are recoloured, not EORed as PRM3 says. The L validation command uses its number and works with outline fonts; PRM3 says the number is ignored and outline fonts are not allowed. | Doc error | GD/wimp/03-icons-and-plotting.md |
| Only ESG bits 16 to 19 are compared, so groups n and n+16 are one; bit 20 is really the numeric flag. PRM3 gives the range as 0 to 31. | Doc error | GD/wimp/03-icons-and-plotting.md |
| Wimp_CreateIcon -3 and -4 with an unknown icon give an error, not "extreme end"; `*ToolSprites` replaces the tools rather than merging; Wimp_SetPalette sends no message. | Doc error | GD/spec/wimp/04-icons.md |
| The RAM sprite pool is a dynamic area "WSpr", not the RMA as PRM3 says; Wimp_SpriteOp tries the high-priority area first. | Doc error | GD/wimp/03-icons-and-plotting.md |
| `Doc/24bit`'s example colour 008000 is called mid grey; by its own rule it is green. | Doc error | GD/spec/wimp/04-icons.md |
| Wimp_SetFontColours does not mask values above 15, and indexes past the palette. | Bug | GD/spec/wimp/04-icons.md |
| The greyscale record is cleared only by `*WimpMode`, so Wimp_SetMode to a colour mode may still reprogram greys. | Unclear | GD/spec/wimp/04-icons.md |
| `remembercurrentfont` has its test inverted: it keeps the result only when the call fails. | Bug | GD/spec/wimp/09-icon-drawing.md |
| Glyph &98 (iconise) is never defined; a partial set of title sprites calls OS_SpriteOp with R2 = 0; with no `blank` tool, ReadSpriteSize is called on address 0. | Bug (latent) | GD/spec/wimp/08-furniture-drawing.md |
| When the Wimp is the first task, its first window has no furniture until it is reopened: the tool list is not built until a border is drawn. | Bug | rosgd/modules/wimp/README.md |

### The icon bar

| What was noticed | Kind | Source |
| --- | --- | --- |
| A text-only icon bar icon with no validation string is scanned for X at address 0 or -1; a 12-character text with no terminator is measured past its end. | Bug | GD/spec/wimp/11-iconbar.md |
| Wimp_DeleteIcon on -2 with no icon bar at all lays out through an invalid pointer. | Bug | GD/spec/wimp/11-iconbar.md |
| `*Configure WimpIconBarLayout` swaps the lists and reopens the bar without laying it out; a font change's refit undoes any ResizeIcon; the first open goes to the top of the stack; left and right scroll speeds differ. | Quirk | GD/spec/wimp/11-iconbar.md |

### Windows and redraw

| What was noticed | Kind | Source |
| --- | --- | --- |
| PRM: Wimp_CreateWindow may fail with "Bad work area extent", but the check is in debug builds only. If measuring the title fails, the window stays created and linked but its handle is never returned (the source says "urk"). | Bug, Doc error | GD/spec/wimp/03-windows.md |
| GetWindowOutline works before the first open; SetExtent does no visible-area check; the extent is not rounded again on a mode change; flag bit 10 is ignored for window colours. The PRM says otherwise for each. | Doc error | GD/spec/wimp/03-windows.md |
| Scroll_Request +28 is the window's own handle, not the handle to open behind, so page clicks send Open_Window_Request even at the end of the extent. | Doc error | GD/spec/wimp/03-windows.md, 05-input.md |
| Wimp_ForceRedraw clears the area later, as Wimp_GetRectangle hands out rectangles; its "TASK" border form is not documented. | Doc error | GD/wimp/02-windows-and-redraw.md |
| Wimp_BlockCopy corrupts R0 and does not invalidate the source area; the PRM says both. | Doc error | GD/spec/wimp/03-windows.md |
| Deleting a window that is being dragged gives no error; byte +39 and flag bits 22 and 23, "reserved" in PRM3, are used; the title height comes from the tool sprites, not a fixed 44. | Doc error | GD/spec/wimp/03-windows.md |
| CMOS WimpFlags bit 6 alone frees only the top left from the on-screen limit; the PRM says all directions. | Doc error | GD/spec/wimp/03-windows.md |
| PRM3 page 3-13 prints the y scroll formula with `scroll_offset_x` (page 3-127 is right). | Doc error | GD/wimp/02-windows-and-redraw.md |
| When the rectangle pool runs out, the Wimp falls into an undocumented back-to-front redraw ("braindead" in the source). | Quirk | GD/wimp/02-windows-and-redraw.md |
| The Toolbox relies on a fake null event after Wimp_StartTask, and on the order in which redraw rectangles are returned. | Quirk | rosgd/docs/articles/the-native-wimp.md |

### Tasks, polling and messages

| What was noticed | Kind | Source |
| --- | --- | --- |
| Wimp_Initialise and ReadSysInfo 7 return 587 whatever version is asked for; versions are normalised silently to 200, 300, 310 or 380. [M] | Quirk | GD/spec/wimp/01-interface.md, 02-tasks.md |
| A null message list to Wimp_Initialise means "Quit only", not PRM3's "all messages". | Doc error | GD/spec/wimp/02-tasks.md |
| PRM3 says Wimp_Poll gives an error on a pending Escape; that check is debug-only. | Doc error | GD/wimp/01-tasks-and-poll.md |
| "Unmaskable" reason codes 2, 3 and 6 to 10 are masked when queued, and masked queued items are dropped (a recorded message bounces). | Doc error | GD/wimp/01-tasks-and-poll.md |
| R2 holds the sender for every queued event, not only reason 18 as PRM5A says; after a fast pollword event it holds the last message's sender. [M] | Quirk | GD/spec/wimp/02-tasks.md |
| A Service_WimpCloseDown claimant may object for R0 = 0 too; the PRM allows it only for R0 = 1. | Doc error | GD/wimp/01-tasks-and-poll.md |
| Wimp_StartTask copies the command into a 1,024-byte buffer with no limit. | Bug | GD/spec/wimp/02-tasks.md |
| Wimp_SlotSize preserves R4, which PRM3 says is corrupted. | Doc error | GD/spec/wimp/02-tasks.md |
| Wimp_SendMessage does not keep `my_ref` positive, as PRM3 says, and updates the block for reason 19 as well as 17 and 18. | Doc error | GD/spec/wimp/06-messages-and-edges.md |
| After acting on its own receiver's entry, the Wimp returns to whichever task polled with that code, so a task may see a spurious event. | Unclear | GD/spec/wimp/06-messages-and-edges.md |
| When an icon's owner ignores a DataLoad, 5.30 does not load it: a recorded one bounces, an unrecorded one gets no DataLoadAck. Why is not known. [M] | Unclear | rosgd/modules/wimp/README.md |
| Service_Reset with tasks running discards them silently. | Quirk | rosgd/modules/wimp/README.md |
| Wimp_SetWatchdogState: disabling twice with the same code word turns the watchdog back on; R0 is corrupted (PRM5A: preserved). | Bug, Doc error | GD/wimp/05-messages-and-services.md |
| Wimp_LoadTemplate writes through a null font array; with no file open it reads through handle 0 and gives &DE [M]; R1 negative other than -1 faults, though PRM3 says R1 ≤ 0 asks for the size. After a load, the window's sprite area word is set to 1. | Bug, Doc error | GD/spec/wimp/06-messages-and-edges.md |
| Wimp_CommandWindow closes quietly only for R0 = -1, not for any R0 below 0 as PRM3 says. | Doc error | GD/spec/wimp/06-messages-and-edges.md |
| Wimp_ReadSysInfo 17 is in centiseconds, not milliseconds; reason 13 just returns 13; reasons 10, 12 and 14 return internal addresses. | Doc error | GD/spec/wimp/06-messages-and-edges.md |
| Wimp_Extend leaves R0 as it was for a reason it does not know, so a caller cannot tell it was refused. | Quirk | rosgd/modules/wimp/README.md |

### Input, menus, the caret and drags

| What was noticed | Kind | Source |
| --- | --- | --- |
| The Mouse_Click block has an undocumented sixth word (the previous buttons); a click is queued behind pending messages. | Quirk | GD/wimp/04-input-menus-drags.md |
| Wimp_GetPointerInfo is not live, despite PRM3's "instantaneous"; button types 12 and 13, reserved in PRM3, work; drag and double-click defaults are 0.5 s and 32 units, not PRM3's 1/5 s and 16. | Doc error | GD/wimp/04-input-menus-drags.md |
| 5.30 holds Wimp_Poll for the whole of a selection (type 13) drag; a text drag that leaves the icon before the drag time is reported as a plain click. | Quirk | rosgd/modules/wimp/README.md |
| A drag started by leaving an icon is reported, not acted on; a dragged DataSave's size is -1; the ghost caret is placed using a screen y as a work-area y. | Quirk, Bug | rosgd/modules/wimp/README.md |
| Dialogue handles are told from menus by the low bit; PRM3 says a handle is 1 to &7FFF. A menu title's validation string and buffer length are ignored. | Doc error | GD/wimp/04-input-menus-drags.md; GD/spec/wimp/01-interface.md |
| With click submenus on, Return in a writable item that has a submenu returns through a stack frame the key path did not push. | Bug | GD/spec/wimp/13-menus.md |
| The tick and arrow icons' data words are left-over register values, visible through GetIconState. | Quirk | GD/spec/wimp/13-menus.md |
| Wimp_SetCaretPosition accepts R1 = icon count and reads past the icon list; above the count it aborts. | Bug | GD/spec/wimp/05-input.md |
| With "TASK", R1 rather than R2 is zeroed [M]; the selection checksum is never checked. | Bug | GD/spec/wimp/10-caret-drawing.md |
| A key buffer of exactly 256 bytes stores length 0, and loses its contents. | Bug | GD/spec/wimp/10-caret-drawing.md |
| If a buffered icon's off-screen buffer cannot be made, the caret is drawn at icon-relative coordinates on the screen. | Bug | GD/spec/wimp/10-caret-drawing.md |

### Error boxes

| What was noticed | Kind | Source |
| --- | --- | --- |
| Wimp_ReportError titles of 61 characters or more overflow a 60-byte buffer and overwrite the message's data. | Bug | GD/spec/wimp/12-error-box.md |
| A program error's substitute block loses the caller's bits 4 and 7, so it always beeps and always has the prefix. | Quirk | GD/spec/wimp/12-error-box.md |
| Service_ErrorEnding comes before Service_WimpReportError 0 (PRM5A: after); the title reads "Message from", not PRM3's "Error from"; bits 24 to 31 are honoured though PRM3 says they must be 0; PRM5A's category numbering does not match the source. | Doc error | GD/spec/wimp/06-messages-and-edges.md |

### FilterManager and TaskManager

| What was noticed | Kind | Source |
| --- | --- | --- |
| Pre-filters get the FilterManager's own record in R1, not the poll block as `Filter/Doc/SWIs` and PRM3 say; filters get the internal task handle, masked to 16 bits. | Doc error | GD/spec/wimp/06-messages-and-edges.md |
| PRM3 documents filter kinds 0 to 3; 4 to 11 exist. | Doc error | GD/spec/wimp/01-interface.md |
| The Task Manager never reads the CAO pointer at +20; its own comment says the field "is garbage" when it fakes one. | Quirk | GD/spec/wimp/07-callers.md |

---

## 4. Kernel, VDU and OS SWIs

| What was noticed | Kind | Source |
| --- | --- | --- |
| `*If` on a false condition runs everything after the first " Else ", so in nested If/Then/Else the inner Else fires: `If 1=0 Then If 1=1 Then Echo H Else Echo I Else Echo J` prints `I Else Echo J`. | Quirk | #2, #60 |
| Else is recognised only as " Else " with a space after it, so a trailing "Else" belongs to the Then part. | Quirk | #59 |
| `*Do` with a quoted argument followed by text prints "Helloaworld". | Quirk | rosgd/tests/desktop/bootcmds/README.md |
| `*CDir` does not create intermediate directories. | Quirk | #3 |
| Page zero is a readable compatibility page, and DebuggerSpace (&2000) reads as zeroes, while &1000 and &3000 abort. Null pointer reads in old programs succeed silently. | Quirk | rosgd/docs/articles/roms-modules-and-the-memory-map.md; #171 |
| On the farm's A72, unaligned LDR and LDRH abort, so old binaries that rely on ARMv3 rotated loads fail; SWP hangs 5.30. [M] | Unclear | GD/design/25-arm-binaries.md; rosgd/runtime/armrun/SPRINTS.md |
| OS_ReadArgs: when an item starts with `-` but names no key, the pointer is restored one byte short. | Bug | GD/design/14-runtime.md |
| OS_ReadUnsigned says "Number not recognised" for a bad terminator, and "(Number)" when it reads no digits: its message is translated twice. | Bug | rosgd/tests/desktop/territory/README.md |
| OS_Convert with negative R2 counts 2 characters per nybble. | Quirk | rosgd/README.md |
| OS_File 5 of a missing object leaves the filing system's R2 to R5 (R5 = 0); Filer_Action depends on it. | Quirk | rosgd/ports/filer_action/README.md |
| OS_Byte 20 and 25, unclaimed, leave R1 pointing into the kernel's ROM font. | Quirk | rosgd/tests/desktop/international/README.md |
| The kernel never sets R1 or R2 on entry to the error handler, though the PRM says R1 is the error buffer. | Doc error | rosgd/runtime/armrun/ABI.md |
| "R0 = 0 means claimed" for UpCalls is not enforced by the kernel. | Unclear | rosgd/runtime/armrun/ABI.md |
| OS_UpdateMEMC is still present: it keeps a soft MEMC1 copy with the top 12 bits forced to &036, and acts on bit 10 only. | Quirk | #120 |
| SetColours spreads a text colour with bits above the pixel width into the word's lower lanes, again on every call, so a colour can change when the other colour is set. | Bug | rosgd/runtime/vdu/README.md |
| VDU 23,2 to 5 at 16 and 32 bpp in BBC mode reads past its table, into the LineStyle code in ROM. | Bug | rosgd/runtime/vdu/README.md |
| VDU 23,12 to 15 at 1 bpp reads its colours from address 0: a register is reused before it is used. | Bug | rosgd/runtime/vdu/README.md |
| Switching output back to the screen from MODE 7 with R3 = 1 aborts at the next character ("abort on instruction fetch at &FF00FFFE"): the save area is never filled in teletext. [M] | Bug | rosgd/runtime/vdu/README.md |
| GetSpriteData takes the bits of the first on-screen word that lie outside the window from a stale register. | Bug | rosgd/runtime/vdu/sprplot.c |
| Flipping a new-format sprite reverses an alpha mask as if it were a 1-bit mask. | Bug | rosgd/runtime/vdu/sprite.c |
| A palette read for an index above 259 reads past the table and then uses index AND 255. | Bug | rosgd/runtime/vdu/palette.c |
| Characters 131, 132 and 136 to 139 on the desktop are drawn by the Window Manager, not the International module. | Quirk | rosgd/tests/desktop/international/README.md |
| Service_International 5 for UTF-8 is claimed only when there is a Latin-1 rest. | Quirk | rosgd/modules/international/international.c |

---

## 5. Other RISC OS modules

### FileSwitch and ROM messages

| What was noticed | Kind | Source |
| --- | --- | --- |
| For a filing system with no start-up text, the Service_Reset banner prints the "UntFS" message straight from the compressed ROM message file; its control codes wiped BootFX's splash. Real hardware's filing systems all have banners, so it is never seen. | Bug | P#24 |
| ROM message files are dictionary-compressed, so a plain lookup's pointer points into the raw file. | Quirk | P#24 |
| `*Copy` and `*Count` give per-file sizes as "29 kbytes" but totals as exact byte counts ("43,920 bytes"). | Quirk | #110 |

### TaskWindow

| What was noticed | Kind | Source |
| --- | --- | --- |
| A program that never calls the OS is not pre-empted in its first slice, before the task window has polled. [M] | Quirk | rosgd/modules/taskwindow/README.md |
| `*ShellCLI_Task` needs a space after the second handle, or reports "File '<handle>' not found". | Quirk | rosgd/tests/desktop/taskwindow/README.md |
| `XOS_CLI "TaskWindow"` with no parameters raises its syntax error instead of returning it. | Bug | rosgd/tests/desktop/taskwindow/README.md |
| Any task window with text handle 1 is taken for one waiting for its parent. | Bug | rosgd/tests/desktop/taskwindow/README.md |

### ColourTrans

| What was noticed | Kind | Source |
| --- | --- | --- |
| ReturnOppColour in 256 colours picks the worst tint of the nearest colour, not the furthest colour. | Bug | rosgd/modules/colourtrans/README.md |
| In 8 bpp full-palette modes a cached colour's GCOL depends on whether ReturnGCOL or SetGCOL cached it first; palette changes do not invalidate the caches. | Bug | rosgd/modules/colourtrans/README.md |
| 32K tables built at run time use an unweighted distance on truncated components; the ResourceFS tables use a weighted one. | Quirk | rosgd/modules/colourtrans/README.md |
| "Worst" matching with every error 0 returns the colour word shifted right by 2; SetTextColour passes R3 unmasked; the Set*Colour calls ignore their lookup's error. | Bug | rosgd/modules/colourtrans/README.md |
| SelectTable reads R5 only for sprite sources, and flag bit 1 gives the sprite mode's default palette. | Quirk | rosgd/modules/colourtrans/README.md |
| WriteCalibrationToFile writes a truncated first line, `ColourTransMapColo...`. | Bug | rosgd/modules/colourtrans/README.md |
| Runaways: the default palette reads its own code as palette words; 32K-to-8 bpp tables keep uninitialised heap; HSVToRGB with a hue outside 0 to 720 jumps into code; grey matching with every weight 0 never returns; calibration of more than 16 words runs away; GCOLs of 256 or more read past the table. | Bug | rosgd/modules/colourtrans/README.md |

### Draw module

| What was noticed | Kind | Source |
| --- | --- | --- |
| On a transform cache miss, the untransformed point is kept, so an edge goes down with its old end untransformed. | Bug | rosgd/modules/draw/README.md |
| Draw_FillClipped returns R0 unchanged, and does not report errors from the clipping path. | Bug | rosgd/modules/draw/README.md |
| A clipped span plots two pixels: HLine is given its ends the wrong way round. | Bug | rosgd/modules/draw/README.md |
| Draw_Stroke's floating point forms test R4 for zero as R4<<1; a failing cap or join returns the new point's x as the error pointer. | Bug | rosgd/modules/draw/README.md |
| Fill style bits 6 and 7, which are internal, are accepted from callers. | Quirk | rosgd/modules/draw/README.md |
| The PRM's "leading" cap in Draw_Stroke's cap block is the path's end cap. [M] | Doc error | rosgd/ports/drawfile/README.md |

### Font Manager

| What was noticed | Kind | Source |
| --- | --- | --- |
| Font_EnumerateCharacters on a directly mapped font leaves V set with R0 a handle ("returned a bad error pointer"); from a code at or below the first run's start it skips that run unless the code is 0. | Bug | rosgd/modules/fontmanager/README.md |
| The ConverttoOS cache returns a stale result for the same y even after SetScaleFactor; only a mode change clears it. | Bug | rosgd/modules/fontmanager/README.md |
| Font_SetThresholds writes one threshold whatever count it is given. | Bug | rosgd/modules/fontmanager/README.md |
| Right-to-left kerning almost never swaps the pair: bit 10 of a stale R14 is tested instead of the plot type. | Bug | rosgd/modules/fontmanager/README.md |
| Font_MakeBitmap given a handle puts its saved thresholds into the wrong font, and pads the file's header with stack garbage. | Bug | rosgd/modules/fontmanager/README.md |
| "ROM font directory cannot contain bitmaps" can never be given: the error number compared is wrong. Scaffold lines are never cleared between characters. | Bug | rosgd/modules/fontmanager/README.md |
| Font_ReadColourTable always rebuilds the table, so one given by SetColourTable is never read back; Font_Paint's R3 and R4 ignore the graphics origin; Font_ChangeArea has no name in the SWI table. | Quirk | rosgd/modules/fontmanager/README.md |
| Font_StringWidth needs R2, R3 and R5 = &7FFFFFFF; with -1, every width is 0. | Quirk | *notes* |

### SpriteExtend, CompressJPEG, CompressPNG and Squash

| What was noticed | Kind | Source |
| --- | --- | --- |
| SpriteExtend's merged upsampler shifts the chroma by one pixel on an odd left clip. | Bug | rosgd/modules/spriteextend/README.md |
| A grey JPEG at 8 bpp with no table fails "JPEG plot failed due to fatal inconsistency". | Bug | rosgd/modules/spriteextend/README.md |
| A truncated JPEG draws whatever the workspace last held; a 1×1 JPEG error-diffused at 4 bpp reads a register never loaded. | Bug | rosgd/modules/spriteextend/README.md |
| JPEG_PlotTransformed only scales and refuses rotation; JPEG_Info always says transformed plots are not supported. SpriteExtend's decode matches libjpeg's fast IDCT with no fancy upsampling. | Quirk | rosgd/modules/spriteextend/README.md; #121 |
| CompressJPEG's Finish and Comment have no setjmp of their own, so a libjpeg error there longjmps into a dead frame and the next compression hangs; its warnings go to stderr from inside a module, which also hangs. [M] | Bug | rosgd/modules/compressjpeg/README.md |
| CompressPNG's palette entries are 4-byte words (Norcroft pads `png_color`), and gamma is passed as an FPA-ordered double. | Quirk | *notes* |
| Squash: fast compress with R3 = 0 takes the length as 2^32-1; `zcat_ass` writes past its output and follows codes not yet made; the end test wraps on 3 or 4 byte input; `comp_ass` stores up to 7 bytes past its output; OS_ValidateAddress passes [0,100). | Bug | rosgd/modules/squash/README.md |
| RISC OS's zlib writes OS byte 13 in gzip headers and ignores `gzopen`'s "x". | Quirk | rosgd/tests/desktop/compress/README.md |

### ScreenBlanker and Territory

| What was noticed | Kind | Source |
| --- | --- | --- |
| ScreenBlanker's flash in standby returns through the wrong exit (`LDMEQFD r13!,{R0,R2,pc}` with one word pushed); the farm machine may not survive it. | Bug | rosgd/modules/screenblanker/README.md |
| Territory_Collate of two equal strings with R3's top bits set loops for ever. | Bug | rosgd/modules/territory/README.md |
| Territory_NameToNumber: "Canada" ended by a NUL gives 0 (it matches Canada1 first), ended by a CR it gives 19. | Quirk | rosgd/modules/territory/README.md |
| ReadSymbols beyond 18 reads past its table; ConvertDateAndTime with reserved flag bits, or bit 28 with an offset, corrupts the stack. | Bug | rosgd/modules/territory/README.md |

### VFPSupport's elementary functions

| What was noticed | Kind | Source |
| --- | --- | --- |
| tan's third reduction constant is wrong, so near odd multiples of π/2 it is wrong by 10^4 to 10^11, often in sign: `TAN(PI/2)` is 16455198023.8. | Bug | rosgd/modules/vfpsupport/elementary.c; rosbas/spec/C-quirks.md |
| cos's argument reduction uses a register only sin and tan set, so it reads the caller's R6. | Bug | rosgd/modules/vfpsupport/elementary.c |
| pow takes exponents from 2^20 to 2^21 as odd, and is odd for -INF. | Bug | rosgd/modules/vfpsupport/elementary.c |
| Subnormals come back unchanged from sin and tan with underflow raised; log(+INF) raises invalid operation; atan2 checks x for NaN twice and y not at all. | Bug | rosgd/modules/vfpsupport/elementary.c |

### Toolbox, DrawFile and the rest

| What was noticed | Kind | Source |
| --- | --- | --- |
| Toolbox attached objects are created with their parent, so a menu that shows the window it belongs to recurses. | Quirk | *notes* |
| Toolbox_Initialise takes an empty list to mean all, and NULL to mean none; SaveAs needs flag 8 before it takes SetDataAddress; Iconbar needs flag &20 to raise its select event; Menu_AddEntry needs -2 with flag 1, or the separator moves. | Quirk | *notes* |
| DrawFile's `callback.c` reads its varargs as `&limit + 1`, which works only under Norcroft's APCS. | Bug (latent) | *notes* |
| PRM: Sound_QSchedule time -2 is said to be "immediate"; Sound2's code treats it as -1. Not checked on the farm. | Doc error | #72 |
| PRM: `os_GLOBAL_NO_MEM` (&C0) and `NO_ANY` (&C1) are documented as global errors but are not in 5.30's headers. | Doc error | #22 |
| The PRM does not say whether PDriver_GiveRectangle's origin is the paper's corner or the printable area's. | Doc error | rosgd/modules/pdriver/README.md |
| MakePSFont: see section 1. | | |

---

## 6. SharedCLibrary, RISC_OSLib and the DDE's C

| What was noticed | Kind | Source |
| --- | --- | --- |
| `%a` printing reads `buff[15]`, which nothing wrote: `%a` of 0x1.fffffffffffff can print `0x2.0000000000000p+0`. | Bug | rosgd/clib/README.md |
| FPEmulator's extended argument reduction for sin, cos and tan is off by up to 2^45 ulps (Draw's cos(90°)). | Bug | rosgd/clib/README.md |
| fmod and remainder of a denormal: FPASC's Rem_Uncommon tests the wrong operand's units bit and returns raw internal register words. | Bug | rosgd/clib/README.md |
| round is computed as \|x\|+0.5 truncated, so round(0.49999999999999994) is 1. | Bug | rosgd/clib/README.md |
| scanf's exponent arithmetic relies on signed wrap: "1e-2147483648" gives 0 with ERANGE. fegetenv returns its argument, not 0; narrowing to float sets no ERANGE when denormalising loses nothing. | Quirk | rosgd/clib/README.md |
| system() calls DDEUtils_FlushCL before every command, so without DDEUtils each call leaves `_kernel_last_oserror` as "SWI &4258B not known"; commands of 1000 characters or more give "Too long". [M] | Quirk | rosgd/tests/capps/clib/README.md |
| malloc is only 4-byte aligned; unaligned word loads raise SIGSEGV but halfword loads do not; `INT_MIN/-1` gives INT_MIN; the OS_GetEnv string has a trailing space. [M] | Quirk | GD/notes/capps/evidence/reports/q2f-report.md |
| 5.30 twice aborted in stdio's `putchar` macro on stdout, as the first statement and only in a full suite run. The cause was never found. [M] | Unclear | rosgd/tests/capps/clib/README.md |
| RISC_OSLib: `os_swix` stores every register even on error, and `xferrecv` depends on it; the Nr forms store nothing on error; `wimp_pollidle` always returns 0 (a CMP clears V). | Quirk, Bug | rosgd/docs/articles/riscos-is-easy-to-program-not.md |
| A C module built the usual way (cmhg and stubs) cannot run in place from ROM: its initialisation relocates the image, SharedCLibrary patches its stub vectors, and the C runtime owns the private word. | Quirk | *notes* |

---

## 7. Headers and build

| What was noticed | Kind | Source |
| --- | --- | --- |
| `OS_DynamicArea` has 20 reasons, not 29 (29 is `DAReason_Limit`); `DAReason_EnumerateInfo` is defined twice. | Quirk | GD/notes/A-architecture.md |
| `Kernel/h/HALDevice` has an `#ifndef` with no `#define`: only Norcroft's `include_only_once` saves it. | Bug (latent) | GD/notes/C-c-lp64.md |
| Toolbox gadgets store a value of 2 in 2-bit plain `int` bit-fields, relying on Norcroft treating them as unsigned: 113 such members in product code. | Quirk (latent) | GD/notes/C-c-lp64.md |
| Twelve modules' SWI headers disagree with their cmhg tables: `Font_ChangeArea` is only in the header; PCCardFS puts `MiscOp` at a different slot in each. | Quirk | GD/notes/D-swi-surface.md |
