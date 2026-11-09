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
    mov $stack_top, %esp
    call kernel_main

1:
    hlt
    jmp 1b

.size _start, . - _start
