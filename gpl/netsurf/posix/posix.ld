/* posix.ld -- a POSIX application's image (design 22): what the C
 * applications' loader runs (capp.h): ELF32 x86-64 at &8000, code, then
 * read-only data with the ROSGD note, then data and bss, each segment page
 * aligned.  OS/ABI 255 is set after the link (posix/link.py).
 *
 * Thread-local storage is refused: the thread pointer is not %fs here
 * (design 22, section 9). */
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
    note   PT_NOTE FLAGS(4);
}

SECTIONS
{
    . = 0x8000;
    .text : {
        *(.text.startup .text.startup.*)
        *(.text .text.* .stub)
    } :text

    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) } :rodata
    .note.rosgd : { KEEP(*(.note.rosgd)) } :rodata :note
    .eh_frame_hdr : { *(.eh_frame_hdr) } :rodata
    .eh_frame : { KEEP(*(.eh_frame)) } :rodata
    .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) } :rodata

    . = ALIGN(4096);
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    } :data
    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } :data
    .data : { *(.data .data.*) } :data
    .tdata : { *(.tdata .tdata.*) } :data
    .tbss : { *(.tbss .tbss.*) } :data
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data
    . = ALIGN(16);
    _end = .;
    PROVIDE(end = .);

    /DISCARD/ : {
        *(.comment) *(.note.GNU-stack) *(.note.gnu.*) *(.interp) *(.dynamic)
        *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash)
    }
}

ASSERT(SIZEOF(.tdata) + SIZEOF(.tbss) == 0,
       "thread-local storage: a POSIX application may not have any yet (design 22, section 9)")
