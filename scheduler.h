#ifndef MAIOS_SCHEDULER_H
#define MAIOS_SCHEDULER_H

#include <stdint.h>

typedef void (*task_entry_t)(void *argument);

/* Cria uma tarefa de kernel. Retorna seu identificador ou -1 se a tabela lotou. */
int task_create(task_entry_t entry, void *argument);
int task_create_linux_user(const uint64_t registers[15], uint64_t rip,
                           uint64_t rsp, uint64_t rflags,
                           uint64_t fs_base, uint64_t gs_base,
                           uint64_t kernel_gs_base, uint64_t kernel_stack_top);
unsigned int task_current_id(void);
void task_set_current_kernel_stack(uint64_t top);
void task_set_current_cpu_context(uint64_t fs_base, uint64_t gs_base,
                                  uint64_t kernel_gs_base);
int task_slot_available(unsigned int id);
int task_is_linux_user(void);
void task_block_on(uint64_t key);
unsigned int task_wake_key(uint64_t key, unsigned int maximum);
void task_exit_current(void);
void task_kill_linux_threads(void);
void task_yield(void);
unsigned int task_count(void);
uint64_t scheduler_ticks(void);
uint64_t *scheduler_tick(uint64_t *saved_stack, uint64_t hardware_irq);
void scheduler_init(void);
void scheduler_critical_enter(void);
void scheduler_critical_leave(void);

#endif
