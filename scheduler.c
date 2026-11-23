#include <stdint.h>
#include "scheduler.h"

#define MAX_TASKS 8
#define STACK_SIZE 65536
#define TASK_UNUSED 0
#define TASK_READY 1
#define TASK_RUNNING 2
#define TASK_MAIN 0

typedef struct {
    uint32_t *stack;
    task_entry_t entry;
    void *argument;
    uint8_t state;
    uint32_t storage[STACK_SIZE / sizeof(uint32_t)];
} task_t;

static task_t tasks[MAX_TASKS];
static unsigned int current_task;
static unsigned int next_id = 1;
static volatile unsigned int critical_depth;
static uint32_t critical_initial_flags;
static volatile uint32_t pit_ticks;

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t attributes;
    uint16_t offset_high;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint32_t base;
} idt_pointer_t;

static idt_entry_t idt[256];
extern void irq0_stub(void);
extern void yield_stub(void);

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0,%1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port));
    return value;
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
    uint32_t handler = (uint32_t)irq0_stub;
    idt[0x20].offset_low = (uint16_t)handler;
    idt[0x20].selector = 0x08;
    idt[0x20].zero = 0;
    idt[0x20].attributes = 0x8E;
    idt[0x20].offset_high = (uint16_t)(handler >> 16);
    handler = (uint32_t)yield_stub;
    idt[0x30].offset_low = (uint16_t)handler;
    idt[0x30].selector = 0x08;
    idt[0x30].zero = 0;
    idt[0x30].attributes = 0x8E;
    idt[0x30].offset_high = (uint16_t)(handler >> 16);
    idt_pointer_t pointer = { sizeof(idt) - 1, (uint32_t)idt };
    __asm__ volatile("lidt %0" : : "m"(pointer));

    /* Remapeia o PIC e deixa habilitado somente o IRQ0 (PIT). */
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0xFE); outb(0xA1, 0xFF);

    /* PIT em aproximadamente 100 Hz. */
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
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    int id = -1;
    for (unsigned int i = 1; i < MAX_TASKS; ++i) {
        if (tasks[i].state == TASK_UNUSED) {
            task_t *task = &tasks[i];
            uint32_t *sp = task->storage + STACK_SIZE / sizeof(uint32_t);
            *--sp = 0x202;                    /* EFLAGS com interrupcoes */
            *--sp = 0x08;                     /* seletor de codigo */
            *--sp = (uint32_t)task_bootstrap;  /* EIP */
            for (unsigned int n = 0; n < 8; ++n) *--sp = 0;
            task->stack = sp;
            task->entry = entry;
            task->argument = argument;
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
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    if (critical_depth++ == 0) critical_initial_flags = flags;
    if (flags & 0x200) __asm__ volatile("sti" : : : "memory");
}

void scheduler_critical_leave(void)
{
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
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

uint32_t scheduler_ticks(void)
{
    return pit_ticks;
}

/* Chamado pelo IRQ0 stub: recebe a pilha salva por pushal e devolve a proxima. */
uint32_t *scheduler_tick(uint32_t *saved_stack, uint32_t hardware_irq)
{
    if (hardware_irq) ++pit_ticks;
    if (hardware_irq) outb(0x20, 0x20);
    if (critical_depth) return saved_stack;
    tasks[current_task].stack = saved_stack;
    if (tasks[current_task].state == TASK_RUNNING)
        tasks[current_task].state = TASK_READY;

    for (unsigned int step = 1; step <= MAX_TASKS; ++step) {
        unsigned int candidate = (current_task + step) % MAX_TASKS;
        if (tasks[candidate].state == TASK_READY) {
            current_task = candidate;
            tasks[candidate].state = TASK_RUNNING;
            return tasks[candidate].stack;
        }
    }
    current_task = TASK_MAIN;
    tasks[TASK_MAIN].state = TASK_RUNNING;
    return tasks[TASK_MAIN].stack ? tasks[TASK_MAIN].stack : saved_stack;
}
