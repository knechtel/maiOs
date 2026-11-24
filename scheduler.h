#ifndef MAIOS_SCHEDULER_H
#define MAIOS_SCHEDULER_H

#include <stdint.h>

typedef void (*task_entry_t)(void *argument);

/* Cria uma tarefa de kernel. Retorna seu identificador ou -1 se a tabela lotou. */
int task_create(task_entry_t entry, void *argument);
void task_yield(void);
unsigned int task_count(void);
uint64_t scheduler_ticks(void);
uint64_t *scheduler_tick(uint64_t *saved_stack, uint64_t hardware_irq);
void scheduler_init(void);
void scheduler_critical_enter(void);
void scheduler_critical_leave(void);

#endif
