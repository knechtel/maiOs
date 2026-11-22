#ifndef MAIOS_SCHEDULER_H
#define MAIOS_SCHEDULER_H

typedef void (*task_entry_t)(void *argument);

/* Cria uma tarefa de kernel. Retorna seu identificador ou -1 se a tabela lotou. */
int task_create(task_entry_t entry, void *argument);
void task_yield(void);
unsigned int task_count(void);
void scheduler_init(void);

#endif
