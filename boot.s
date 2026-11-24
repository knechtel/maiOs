.section .multiboot
.align 4
.long 0x1BADB002
.long 0x00000003
.long -(0x1BADB002 + 0x00000003)

/* QEMU PVH enters the kernel in 32-bit protected mode. */
.section .note.Xen,"a",@note
.align 4
.long 4
.long 4
.long 18
.asciz "Xen"
.align 4
.long _start
.align 4

.section .bss
.align 16
stack_bottom:
.skip 65536
stack_top:
.global kernel_stack_top
kernel_stack_top = stack_top

.align 4096
pml4_table:
.skip 4096
pdpt_table:
.skip 4096
.align 4096
page_directories:
.skip 16384

.section .text
.code32
.global _start
.type _start, @function
_start:
    cli
    lgdt gdt_descriptor
    ljmp $0x08, $protected32

protected32:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    /* Build identity mappings for the first 4 GiB with 2 MiB pages. */
    movl $pdpt_table, %eax
    orl $0x03, %eax
    movl %eax, pml4_table
    movl $page_directories, %eax
    orl $0x03, %eax
    movl %eax, pdpt_table
    addl $4096, %eax
    movl %eax, pdpt_table + 8
    addl $4096, %eax
    movl %eax, pdpt_table + 16
    addl $4096, %eax
    movl %eax, pdpt_table + 24

    xorl %ecx, %ecx
1:
    movl %ecx, %eax
    shll $21, %eax
    orl $0x83, %eax
    movl %eax, page_directories(,%ecx,8)
    movl $0, page_directories+4(,%ecx,8)
    incl %ecx
    cmpl $2048, %ecx
    jne 1b

    movl $pml4_table, %eax
    movl %eax, %cr3

    /* Enable PAE, SSE state, and long mode before paging. */
    movl %cr4, %eax
    orl $0x620, %eax
    movl %eax, %cr4
    movl $0xC0000080, %ecx
    rdmsr
    orl $0x100, %eax
    wrmsr
    movl %cr0, %eax
    andl $~0x04, %eax
    orl $0x80010002, %eax
    movl %eax, %cr0
    ljmp $0x18, $long_mode_entry

.code64
long_mode_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    movabs $stack_top, %rsp
    andq $-16, %rsp
    call kernel_main
2:
    cli
    hlt
    jmp 2b

.global irq0_stub
irq0_stub:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    movl $1, %esi
    jmp scheduler_interrupt_common

.global yield_stub
yield_stub:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    xorl %esi, %esi

scheduler_interrupt_common:
    movq %rsp, %r12
    andq $-16, %rsp
    call scheduler_tick
    movq %rax, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    iretq

.global linux_syscall_entry
linux_syscall_entry:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %r12
    movq %rsp, %rdi
    andq $-16, %rsp
    call linux_syscall_dispatch
    movq %r12, %rsp
    movq %rax, 112(%rsp)
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    iretq

.section .data
.align 16
.global gdt
gdt:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF /* 32-bit boot code */
    .quad 0x00CF92000000FFFF /* kernel data */
    .quad 0x00AF9A000000FFFF /* 64-bit kernel code */
    .quad 0x00AFFA000000FFFF /* 64-bit user code */
    .quad 0x00CFF2000000FFFF /* user data */
    .quad 0x0000000000000000 /* 64-bit TSS descriptor, low half */
    .quad 0x0000000000000000 /* 64-bit TSS descriptor, high half */
gdt_end:
.global gdt_descriptor
gdt_descriptor:
    .word gdt_end - gdt - 1
    .long gdt
