#ifndef MAIOS_SCHEDULER_H
#define MAIOS_SCHEDULER_H

typedef void (*task_entry_t)(void *argument);

/* Cria uma tarefa de kernel. Retorna seu identificador ou -1 se a tabela lotou. */
int task_create(task_entry_t entry, void *argument);
void task_yield(void);
unsigned int task_count(void);
uint32_t scheduler_ticks(void);
void scheduler_init(void);
void scheduler_critical_enter(void);
void scheduler_critical_leave(void);

#endif
