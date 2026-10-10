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
