ENTRY(_start)
/* PVH flavor omits the Multiboot header so QEMU selects the Xen note. */

SECTIONS
{
    . = 1M;
    /DISCARD/ : { *(.multiboot) }
    .note.Xen : { KEEP(*(.note.Xen)) }
    .text       : { *(.text*) }
    .rodata     : { *(.rodata*) }
    .data       : { *(.data*) }
    .bss        : { *(COMMON) *(.bss*) }
    _kernel_end = .;
    ASSERT(_kernel_end < 0x100000000, "maiOS boot image must be below 4 GiB")
}
