ENTRY(_start)

SECTIONS
{
    . = 1M;
    .multiboot : { KEEP(*(.multiboot)) }
    .note.Xen : { KEEP(*(.note.Xen)) }
    .text       : { *(.text*) }
    .rodata     : { *(.rodata*) }
    .data       : { *(.data*) }
    .bss        : { *(COMMON) *(.bss*) }
    _kernel_end = .;
    ASSERT(_kernel_end < 0x100000000, "maiOS boot image must be below 4 GiB")
}
