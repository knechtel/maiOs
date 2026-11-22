.section .multiboot
.align 4
.long 0x1BADB002              /* magic do Multiboot 1 */
.long 0x00000003              /* alinhar módulos e pedir memória */
.long -(0x1BADB002 + 0x00000003)

/* QEMU recente usa o protocolo PVH para carregar ELF com -kernel. */
.section .note.Xen,"a",@note
.align 4
.long 4                        /* tamanho do nome */
.long 4                        /* tamanho do descritor */
.long 18                       /* XEN_ELFNOTE_PHYS32_ENTRY */
.asciz "Xen"
.align 4
.long _start
.align 4

.section .bss
.align 16
stack_bottom:
.skip 65536
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    cli

    /* Use selectors owned by the kernel before installing interrupt gates. */
    lgdt gdt_descriptor
    ljmp $0x08, $1f
1:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    /* O BearSSL pode selecionar rotinas SSE2 após detectar os recursos da CPU. */
    mov %cr0, %eax
    and $~0x4, %eax             /* CR0.EM = 0 */
    or $0x2, %eax               /* CR0.MP = 1 */
    mov %eax, %cr0
    mov %cr4, %eax
    or $0x600, %eax             /* CR4.OSFXSR | CR4.OSXMMEXCPT */
    mov %eax, %cr4

    mov $stack_top, %esp
    call kernel_main

2:
    hlt
    jmp 2b

.global irq0_stub
irq0_stub:
    pusha
    mov %esp, %eax
    push $1
    push %eax
    call scheduler_tick
    add $8, %esp
    mov %eax, %esp
    popa
    iret

.global yield_stub
yield_stub:
    pusha
    mov %esp, %eax
    push $0
    push %eax
    call scheduler_tick
    add $8, %esp
    mov %eax, %esp
    popa
    iret

.size _start, . - _start

.section .rodata
.align 8
gdt:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF
    .quad 0x00CF92000000FFFF
gdt_end:
gdt_descriptor:
    .word gdt_end - gdt - 1
    .long gdt
