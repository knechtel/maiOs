#include <stdint.h>
#include "linux_compat.h"

#define LINUX_X86_64_NR_GETPID 39u
#define LINUX_ENOSYS 38
#define TSS_SELECTOR 0x30u

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} x86_64_tss_t;

static x86_64_tss_t kernel_tss;
extern uint64_t gdt[];
extern uint8_t kernel_stack_top[];

static void tss_descriptor_set(uint64_t *descriptor, uint64_t base, uint32_t limit)
{
    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xffffu);
    low |= (base & 0xffffffu) << 16;
    low |= (uint64_t)0x89u << 40; /* present, available 64-bit TSS */
    low |= (uint64_t)((limit >> 16) & 0x0fu) << 48;
    low |= ((base >> 24) & 0xffu) << 56;
    descriptor[0] = low;
    descriptor[1] = base >> 32;
}

void linux_compat_init(void)
{
    kernel_tss.rsp0 = (uint64_t)(uintptr_t)kernel_stack_top;
    kernel_tss.iomap_base = sizeof(kernel_tss);
    tss_descriptor_set(&gdt[6], (uint64_t)(uintptr_t)&kernel_tss,
                       sizeof(kernel_tss) - 1);
    __asm__ volatile("ltr %0" : : "r"((uint16_t)TSS_SELECTOR) : "memory");
}

/* `pusha` frame order used by the syscall entry: r15 ... rax. */
int64_t linux_syscall_dispatch(uint64_t *saved_registers)
{
    uint64_t syscall_number = saved_registers[14];
    if (syscall_number == LINUX_X86_64_NR_GETPID) return 1;
    return -LINUX_ENOSYS;
}
