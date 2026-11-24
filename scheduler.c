#include <stdint.h>
#include "scheduler.h"
#include "linux_compat.h"

#define MAX_TASKS 8
#define STACK_SIZE 65536
#define TASK_UNUSED 0
#define TASK_READY 1
#define TASK_RUNNING 2
#define TASK_MAIN 0

typedef struct {
    uint64_t *stack;
    task_entry_t entry;
    void *argument;
    uint8_t state;
    uint8_t fpu_state[512] __attribute__((aligned(16)));
    uint64_t storage[STACK_SIZE / sizeof(uint64_t)];
} task_t;

static task_t tasks[MAX_TASKS];
static unsigned int current_task;
static unsigned int next_id = 1;
static volatile unsigned int critical_depth;
static uint64_t critical_initial_flags;
static volatile uint64_t pit_ticks;
static uint8_t initial_fpu_state[512] __attribute__((aligned(16)));

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idt_pointer_t;

static idt_entry_t idt[256];
extern void irq0_stub(void);
extern void yield_stub(void);
extern void linux_syscall_entry(void);

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0,%1" : : "a"(value), "Nd"(port));
}

static void idt_set_gate(unsigned int vector, void (*entry)(void), uint8_t attributes)
{
    uint64_t address = (uint64_t)(uintptr_t)entry;
    idt[vector].offset_low = (uint16_t)address;
    idt[vector].selector = 0x18;
    idt[vector].ist = 0;
    idt[vector].attributes = attributes;
    idt[vector].offset_middle = (uint16_t)(address >> 16);
    idt[vector].offset_high = (uint32_t)(address >> 32);
    idt[vector].reserved = 0;
}

static void task_bootstrap(void)
{
    task_t *task = &tasks[current_task];
    task->entry(task->argument);
    task->state = TASK_UNUSED;
    task_yield();
    for (;;) __asm__ volatile("hlt");
}

void scheduler_init(void)
{
    idt_set_gate(0x20, irq0_stub, 0x8E);
    idt_set_gate(0x30, yield_stub, 0x8E);
    idt_set_gate(0x80, linux_syscall_entry, 0xEE);
    linux_compat_init();
    __asm__ volatile("fninit; fxsave %0" : "=m"(initial_fpu_state));
    for (unsigned int i = 0; i < MAX_TASKS; ++i)
        for (unsigned int j = 0; j < sizeof(initial_fpu_state); ++j)
            tasks[i].fpu_state[j] = initial_fpu_state[j];
    idt_pointer_t pointer = { sizeof(idt) - 1, (uint64_t)(uintptr_t)idt };
    __asm__ volatile("lidt %0" : : "m"(pointer));

    /* Remapeia o PIC e deixa habilitado somente o IRQ0 (PIT). */
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0xFE); outb(0xA1, 0xFF);

    outb(0x43, 0x36);
    outb(0x40, 11932 & 0xFF);
    outb(0x40, 11932 >> 8);
    tasks[TASK_MAIN].state = TASK_RUNNING;
    current_task = TASK_MAIN;
    __asm__ volatile("sti" : : : "memory");
}

int task_create(task_entry_t entry, void *argument)
{
    if (!entry) return -1;
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    int id = -1;
    for (unsigned int i = 1; i < MAX_TASKS; ++i) {
        if (tasks[i].state == TASK_UNUSED) {
            task_t *task = &tasks[i];
            uint64_t *sp = task->storage + STACK_SIZE / sizeof(uint64_t) - 1;
            *--sp = 0x202;                    /* RFLAGS with interrupts enabled */
            *--sp = 0x18;                     /* 64-bit kernel code selector */
            *--sp = (uint64_t)(uintptr_t)task_bootstrap;
            for (unsigned int n = 0; n < 15; ++n) *--sp = 0;
            task->stack = sp;
            task->entry = entry;
            task->argument = argument;
            for (unsigned int n = 0; n < sizeof(initial_fpu_state); ++n)
                task->fpu_state[n] = initial_fpu_state[n];
            task->state = TASK_READY;
            id = (int)next_id++;
            break;
        }
    }
    if (flags & 0x200) __asm__ volatile("sti" : : : "memory");
    return id;
}

void task_yield(void)
{
    __asm__ volatile("int $0x30" : : : "memory");
}

void scheduler_critical_enter(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    if (critical_depth++ == 0) critical_initial_flags = flags;
    if (flags & 0x200) __asm__ volatile("sti" : : : "memory");
}

void scheduler_critical_leave(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    if (critical_depth) --critical_depth;
    if (critical_depth == 0 && (critical_initial_flags & 0x200))
        __asm__ volatile("sti" : : : "memory");
    else if (critical_depth != 0 && (flags & 0x200))
        __asm__ volatile("sti" : : : "memory");
}

unsigned int task_count(void)
{
    unsigned int count = 0;
    for (unsigned int i = 1; i < MAX_TASKS; ++i)
        if (tasks[i].state != TASK_UNUSED) ++count;
    return count;
}

uint64_t scheduler_ticks(void)
{
    return pit_ticks;
}

uint64_t *scheduler_tick(uint64_t *saved_stack, uint64_t hardware_irq)
{
    if (hardware_irq) ++pit_ticks;
    if (hardware_irq) outb(0x20, 0x20);
    if (critical_depth) return saved_stack;
    __asm__ volatile("fxsave %0" : "=m"(tasks[current_task].fpu_state));
    tasks[current_task].stack = saved_stack;
    if (tasks[current_task].state == TASK_RUNNING)
        tasks[current_task].state = TASK_READY;

    for (unsigned int step = 1; step <= MAX_TASKS; ++step) {
        unsigned int candidate = (current_task + step) % MAX_TASKS;
        if (tasks[candidate].state == TASK_READY) {
            current_task = candidate;
            tasks[candidate].state = TASK_RUNNING;
            __asm__ volatile("fxrstor %0" : : "m"(tasks[candidate].fpu_state));
            return tasks[candidate].stack;
        }
    }
    current_task = TASK_MAIN;
    tasks[TASK_MAIN].state = TASK_RUNNING;
    __asm__ volatile("fxrstor %0" : : "m"(tasks[TASK_MAIN].fpu_state));
    return tasks[TASK_MAIN].stack ? tasks[TASK_MAIN].stack : saved_stack;
}
