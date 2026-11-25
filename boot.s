.section .multiboot,"a"
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
.global pml4_table
.skip 4096
pdpt_table:
.global pdpt_table
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

/* Exceptions 0..31: normalize the CPU frame and report user faults. */
.macro EXCEPTION_NOERR n
.global linux_exception_stub_\n
linux_exception_stub_\n:
    pushq $0
    pushq $\n
    jmp linux_exception_common
.endm
.macro EXCEPTION_ERR n
.global linux_exception_stub_\n
linux_exception_stub_\n:
    pushq $\n
    jmp linux_exception_common
.endm
EXCEPTION_NOERR 0
EXCEPTION_NOERR 1
EXCEPTION_NOERR 2
EXCEPTION_NOERR 3
EXCEPTION_NOERR 4
EXCEPTION_NOERR 5
EXCEPTION_NOERR 6
EXCEPTION_NOERR 7
EXCEPTION_ERR 8
EXCEPTION_NOERR 9
EXCEPTION_ERR 10
EXCEPTION_ERR 11
EXCEPTION_ERR 12
EXCEPTION_ERR 13
EXCEPTION_ERR 14
EXCEPTION_NOERR 15
EXCEPTION_NOERR 16
EXCEPTION_ERR 17
EXCEPTION_NOERR 18
EXCEPTION_NOERR 19
EXCEPTION_NOERR 20
EXCEPTION_ERR 21
EXCEPTION_NOERR 22
EXCEPTION_NOERR 23
EXCEPTION_NOERR 24
EXCEPTION_NOERR 25
EXCEPTION_NOERR 26
EXCEPTION_NOERR 27
EXCEPTION_NOERR 28
EXCEPTION_ERR 29
EXCEPTION_ERR 30
EXCEPTION_NOERR 31

.global linux_exception_common
linux_exception_common:
    testb $3, 24(%rsp)              /* saved CS RPL */
    jz .Lkernel_exception
    swapgs
    movq 0(%rsp), %rdi              /* vector */
    movq 8(%rsp), %rsi              /* error code */
    movq 16(%rsp), %rdx             /* faulting RIP */
    xorq %rcx, %rcx
    cmpq $14, %rdi
    jne .Lreport_user_exception
    movq %cr2, %rcx
.Lreport_user_exception:
    andq $-16, %rsp
    call linux_report_user_exception
    cli
.Luser_exception_halt:
    hlt
    jmp .Luser_exception_halt
.Lkernel_exception:
    movq 0(%rsp), %rdi
    movq 8(%rsp), %rsi
    movq 16(%rsp), %rdx
    xorq %rcx, %rcx
    cmpq $14, %rdi
    jne .Lreport_kernel_exception
    movq %cr2, %rcx
.Lreport_kernel_exception:
    andq $-16, %rsp
    call linux_report_kernel_exception
    cli
.Lkernel_exception_halt:
    hlt
    jmp .Lkernel_exception_halt

.section .rodata
.align 8
.global linux_exception_stub_table
linux_exception_stub_table:
.quad linux_exception_stub_0, linux_exception_stub_1, linux_exception_stub_2, linux_exception_stub_3
.quad linux_exception_stub_4, linux_exception_stub_5, linux_exception_stub_6, linux_exception_stub_7
.quad linux_exception_stub_8, linux_exception_stub_9, linux_exception_stub_10, linux_exception_stub_11
.quad linux_exception_stub_12, linux_exception_stub_13, linux_exception_stub_14, linux_exception_stub_15
.quad linux_exception_stub_16, linux_exception_stub_17, linux_exception_stub_18, linux_exception_stub_19
.quad linux_exception_stub_20, linux_exception_stub_21, linux_exception_stub_22, linux_exception_stub_23
.quad linux_exception_stub_24, linux_exception_stub_25, linux_exception_stub_26, linux_exception_stub_27
.quad linux_exception_stub_28, linux_exception_stub_29, linux_exception_stub_30, linux_exception_stub_31
.text

.global linux_syscall_entry
linux_syscall_entry:
    swapgs
    movq %rsp, %gs:0
    movq %gs:8, %rsp
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
    movabsq $0x8000000000000000, %rcx
    cmpq %rcx, %rax
    je .Llinux_syscall_exit
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
    movq %gs:0, %rsp
    swapgs
    sysretq
.Llinux_syscall_exit:
    movq %gs:24, %rax
    movq %gs:32, %r11
    movq %gs:16, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    pushq %r11
    popfq
    retq

.global linux_enter_user
linux_enter_user:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %gs:16
    movq %rdx, %gs:8
    pushfq
    popq %rax
    movq %rax, %gs:32
    movq %rsi, %rsp
    andq $-16, %rsp
    pushq $0x2b
    pushq %rsi
    pushq $0x202
    pushq $0x33
    pushq %rdi
    movw $0x2b, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    swapgs
    /* Preserve KERNEL_GS_BASE while installing the user GS selector. */
    movw %ax, %gs
    xorq %rax, %rax
    xorq %rbx, %rbx
    xorq %rcx, %rcx
    xorq %rdx, %rdx
    xorq %rbp, %rbp
    xorq %rdi, %rdi
    xorq %rsi, %rsi
    xorq %r8, %r8
    xorq %r9, %r9
    xorq %r10, %r10
    xorq %r11, %r11
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15
    iretq

.section .data
.align 16
.global gdt
gdt:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF /* 32-bit boot code */
    .quad 0x00CF92000000FFFF /* kernel data */
    .quad 0x00AF9A000000FFFF /* 64-bit kernel code */
    .quad 0x00CF92000000FFFF /* syscall stack segment (STAR kernel SS) */
    .quad 0x00CFF2000000FFFF /* user data */
    .quad 0x00AFFA000000FFFF /* 64-bit user code */
    .quad 0x0000000000000000 /* 64-bit TSS descriptor, low half */
    .quad 0x0000000000000000 /* 64-bit TSS descriptor, high half */
gdt_end:
.global gdt_descriptor
gdt_descriptor:
    .word gdt_end - gdt - 1
    .long gdt
