# DeskClock

A native desktop module (`deskclock.c`, `DeskClock` in the ROM) that puts a
round analogue clock on the icon bar. It is not part of RISC OS 5.30.

SELECT cycles the face through five styles (Classic, Night, Deco, Mint and
Digital) and ADJUST cycles back. MENU offers "Next style" and "Quit". The clock
is redrawn once a second, and `Service_ModeChange` remakes the sprite for the
new mode.

The module starts the task at `Service_StartWimp` through `*DeskClock`, but
only when the kernel command line has `rosgd.deskclock`. The run scripts give
it to a box that starts in a window. A probe's box leaves it out, as does
`ROSGD_DESKICONS=none`.

The time comes from `OS_Word 14,0`. The face is drawn straight into a 4 bpp
old-format sprite with a palette, not through the VDU (#19, #13). The sprite
mask is laid out as the image is, as the PRM describes for old-format sprites.

## Tests

`boot/selftest_deskclock.c` checks the module's place in the chain, its command
and services, and the sprite it draws. The deskprobes show the faces on the
bar.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
