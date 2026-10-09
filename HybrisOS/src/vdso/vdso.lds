/* vdso.lds: links the vDSO as one load segment, as Linux's vDSO is linked.
 * musl finds the vDSO's base from the last PT_LOAD (p_offset - p_vaddr).
 * There is therefore exactly one load segment, its file offsets equal its
 * addresses, and the dynamic section lies inside it. */
PHDRS
{
    text PT_LOAD FLAGS(5) FILEHDR PHDRS;
    dynamic PT_DYNAMIC FLAGS(4);
}
SECTIONS
{
    . = SIZEOF_HEADERS;
    .hash           : { *(.hash) }          :text
    .dynsym         : { *(.dynsym) }
    .dynstr         : { *(.dynstr) }
    .gnu.version    : { *(.gnu.version) }
    .gnu.version_d  : { *(.gnu.version_d) }
    .dynamic        : { *(.dynamic) }       :text :dynamic
    .rodata         : { *(.rodata*) }       :text
    .text           : { *(.text*) }
    /DISCARD/       : { *(.data*) *(.bss*) *(.note*) *(.eh_frame*) *(.comment) *(.gnu.hash) }
}
