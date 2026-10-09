/* blob.S: includes the vDSO's image (vdso/vdso.c, built as a shared
 * object) in the HAL. vdso_init (vm.c) copies it into the page that every
 * process maps. */
    .section .rodata
    .balign 16
    .global vdso_image, vdso_image_end
vdso_image:
    .incbin VDSO_SO
vdso_image_end:
