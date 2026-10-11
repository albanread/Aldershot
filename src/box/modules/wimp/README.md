# Native Window Manager

The RISC OS 5.30 Window Manager written in C. It is the BOX's Window Manager and it replaces the translated one (`build/gen/rom_wimp.c`). Both are in the ROM. Booting with `rosgd.wimp=translated` (hosted: `ROSGD_WIMP=translated`) starts the translated Wimp and FilterManager instead. With the native Wimp the translated FilterManager is left out, because the native Wimp answers the Filter SWIs itself.

Every Wimp SWI is written. A reason or feature that is not done answers with an error that says so.

## Source files

| File | Contents |
| --- | --- |
| `wimp.c` | The module, workspace, errors, SWI table and the small SWIs. |
| `task.c`, `message.c` | Tasks, Wimp_Initialise, CloseDown, Wimp_Poll and its order of delivery, Wimp_SlotSize, `*WimpSlot`. The message queue. |
| `window.c`, `redraw.c`, `region.c` | Windows, stacks and placement. The invalid region, block copies and the redraw protocol. Rectangle lists, in 5.30's order. |
| `screen.c`, `tools.c`, `draw.c`, `font.c` | The mode and palette, the tool sprites, the border and icon drawing, the desktop font. |
| `caret.c`, `icons.c`, `sprites.c`, `input.c` | The caret, keys and writable icons. Icons and the sprite pool. The pointer, hit tests, clicks, drags and Wimp_AutoScroll. |
| `menus.c`, `iconbar.c`, `errorbox.c` | Menus, the icon bar and Wimp_ReportError. |
| `cmdwin.c`, `clipboard.c` | The command window. The Clipboard Manager, a real Wimp task started as 5.30 starts it. |
| `surface.c` | Surface windows, which are the BOX's own (below). |
| `templates.c`, `filters.c`, `commands.c` | Templates, the filters, and the * commands, Wimp_SetMode, Wimp_SetPointerShape and Wimp_SetWatchdogState. |

All state is in an RMA workspace reached through the private word. A task is a runtime task with a record here. Its baton, slot, environment handlers and DomainId move with `ros_task_switch`. Wimp_Poll searches in 5.30's order on the calling task's thread. An event for another task is left in that task's record and the baton is handed over.

## Commands

`*WimpStats [-reset]` lists each task's null events and paced null events. `*WimpVisualFlags` reads 5.30's template and covers textured menus, separator grooves and 3D borders.

## Themes

`theme.c` holds the desktop's theme (RISCOSGrandDesign design 35, phase P1). A theme is a directory on `Themes:` with a `Tools` file and a `Sprites` file (and `Sprites22` and the rest), as RISC OS 5's theme folders are; the disc ships 5.30's Acorn, Iyonix, Morris4, Sovereign and Ursula in `Resources.Themes`, and its `!Boot` sets `Themes$Path` to `Choices.Themes` then those. `*WimpTheme <name>` switches on the running desktop: the tools come from the theme's `Tools`, the theme's `Sprites` take the ROM sprite area's place (as 5.30's `!ThemeDefs` lays them over `Resources.Wimp.Sprites`, so the RAM pool and applications' own icons are untouched), `Wimp$IconTheme` is set to `<name>.`, every window is reopened for the new furniture sizes, Message_WimpThemeChanged (&C0240, the name at +20) is broadcast and the screen is redrawn. `*WimpTheme Classic` goes back to the ROM's look exactly; `*WimpTheme` alone names the theme. `*ToolSprites <file>` loads a tool sprite file, as on RISC OS, and `*ToolSprites` alone the ROM's again. `tests/deskprobe/themes.py` proves them in the box.

A theme may have a `Theme` file (design 35 P2): plain text, `Key: value` in sections, `Base:` naming a theme it builds on. With `Chrome.Style: painted` the Wimp paints the furniture instead of plotting tool sprites (`draw.c`, `painted_border`): the title row as a fill, a two-colour gradient or pinstripes (with a plain gap behind the text), the buttons as glyphs, coloured dots or bevelled boxes, the scroll bars classic, slim or channel, bevels of the theme's depth and colours, and the outline in its colour and width. The theme gives the furniture's sizes (`Chrome.TitleHeight`, `Chrome.ScrollWidth`), which `tools.c` uses, so outlines and hit-testing follow; the sliders are where `vslider` and `hslider` put them. A window with title colours of its own keeps them. `Window.VisualFlags` runs `*WimpVisualFlags`, undone at the next switch. `*WimpThemeSet <Section.Key> <value>` changes one setting at once. A `[Menus]` section themes every menu (`wimp_menu_themed`): its background (`redraw.c`), its items in the standard black on white drawn in the theme's text and background and highlighted in its highlight (`munge`), its separators (`menus.c`) and its title bar; items an application coloured keep their colours. `[Buttons.Action]` colours dialogue boxes' action and default buttons, and `[Iconbar]` gives the icon bar its own background and label colour (its labels' backgrounds too, not the texture tile). `Title.Font: Homerton.Bold 12` draws painted titles in an outline font of that size, centred on the bar by the font's bounding box (`title_in_font`), found when first drawn and lost at the next theme; `desktop`, or a font that cannot be found, is the desktop font. The disc's painted themes are Graphite, Paper, Nord, HighContrast, Archimedes, Cube (its icons square tiles, `tools/mkcubesprites.py`) and Pinstripe; AcornFlat is Acorn with BOX's visual flags.

Round corners and drop shadows (design 35 P3; `shape.c`, `shadow.c`) are for top-level windows with a title bar, and menus, under a painted theme. The Wimp has no alpha and no compositor, so a round-cornered window is a shaped window, as X's Shape extension makes one: its opaque area is its outline less a notch at each corner, stepped a pixel row at a time along the arc (`Chrome.Radius`, OS units; `Chrome.RoundBottom: no` for square bottom corners). `wimp_visible` gives the notches to the windows below, which draw them as they draw the rest of themselves; the redraw scan and the hit test leave them out; the border painter draws the arc's edge in the outline colour, and again over each work area rectangle, since a corner can be in the work area. A shadow (`Chrome.Shadow: <extent> <strength>`) falls a third of its extent below the window and fades with the square of the distance from the round-cornered box. It is laid over what is below by darkening the screen's memory (32 bits per pixel only) after each redraw rectangle, after the border pass and after the desktop is cleared, by the shadows of the windows above. Darkening twice would darken twice, so nothing draws into a shadow except a redraw: Wimp_UpdateWindow and in-place icon changes leave shadowed parts out and make them invalid, Wimp_BlockCopy and the copies made when windows move leave them out, and a window that moves, opens, closes or changes places makes its old and new shadows invalid. `tests/deskprobe/themes.py`'s corners probe checks that a dragged window leaves the screen exactly as a full redraw draws it.

Tool sprites of fewer than 8 bits per pixel without a palette (RISC OS 4's, in the Morris4 and Ursula themes) are translated through a palette made from the Wimp's colours, as 5.30's `recache_tools_trans` does. Title bars are tinted as 5.30's TrueIcon3 tints them (`tools.c`, `wimp_title_tool_table`): when a window's title is not the standard light grey on black, its title bar's tools are drawn through a table generated as the plain one, with `tintfunc`'s transfer function -- light greys moved towards white by the title background, darker greys from the background towards the foreground -- kept for one pair of colours. Only the title bar (5.30's `colourmoreborder` is false); not with custom tables. So RISC OS 4's opaque, textured title bars take the window's title colour, cream with the input focus.

## Surface windows

A window can be bound to a surface, which is a sprite or a virtual display (`runtime/vdu/vdisplay.c`). The Wimp then draws the work area itself, scaled by a whole number or fitted. The owner gets no redraw request. GraphTask uses this (`apps/graphtask`). A virtual display tells the Wimp of each change, and the change is drawn at the next Wimp_Poll or VSync. After a mode change the owner is sent Message_SurfaceResized (&C01C2).

The calls are Wimp_Extend reasons in the BOX's own range. A caller knows the native Wimp handled the call when R0 = 0 on exit, because 5.30 leaves R0 alone for a reason it does not know.

| Reason | Meaning |
| --- | --- |
| &5200 SurfaceBind | R1 window, R2 flags (bit 0: R3 is a VDisplay handle), R3 sprite area or handle, R4 sprite, R5 scale 1 to 16 or 0 fitted. |
| &5201 SurfaceUnbind | R1 window. |
| &5202 SurfaceChanged | R1 window, R2 flags, R3 to R6 the changed rectangle in sprite pixels. |
| &5203 SurfaceInfo | R1 window. Returns the flags, times shown, rectangles plotted and the scale. |

## Differences from RISC OS 5.30

- Wimp_ReadSysInfo reports version 588 where 5.30 reports 587.
- Null events are paced (#138). A task that polls again within 0.5 ms of each of its last four nulls gets its next null only 5 ms after its last. The interval is `Wimp$NullPace` or the boot word `rosgd.nullpace` (ms, or `<n>Hz`; 0 turns pacing off). Wimp_PollIdle, single-tasking programs and a task window's child are not paced.
- Service_Reset with tasks running does nothing. 5.30 discards the tasks silently, which would strand their domains here.
- Service_NewApplication is not issued by the runtime yet (#101). Service_SwitchingOutputToSprite has no farm check yet (#98).

## Not done

- Drawing: the pressed furniture, tinted titles where no `table_n` matches, the ghost caret's checksums, right-to-left text and editing, and border children (`wf_inborder`).
- Wimp_CloseDown by handle of a task waiting in its poll leaves its thread waiting.
- Pasting by DataLoad that the icon's owner ignores is not understood. The result matches 5.30, but why 5.30 does not take the parked copy over is not known.

## Tests

`tests/wimp` (`compare.py --guest [CASE...]`, with `cases` and `expected`) compares Wimp logs and screens with RISC OS 5.30 recorded on the farm, and with the translated Wimp. `tests/wimprec/wimprec.py suite` records and compares message traffic. `tests/wimpbench` times both Wimps. `ROSGD_BOX_APPEND=rosgd.wimp=translated` runs any harness or desk probe under the translated Wimp. Desktop probes are in `tests/deskprobe`, and `tests/wimp/ptrcheck.py` compares pointer shapes. Known failures: `shutdown` fails under both Wimps (#93), `iconbar` is flaky under the translated Wimp (#94).

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
