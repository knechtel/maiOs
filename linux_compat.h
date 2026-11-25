#ifndef MAIOS_LINUX_COMPAT_H
#define MAIOS_LINUX_COMPAT_H

#include <stdint.h>

void linux_compat_init(void);
int64_t linux_syscall_dispatch(uint64_t *saved_registers);
int linux_enter_user(uint64_t entry, uint64_t user_stack, uint64_t kernel_stack);
void linux_set_kernel_stack(uint64_t stack_top);
void linux_set_program_break(uint64_t address);
void linux_process_reset(void);
void linux_report_user_exception(uint64_t vector, uint64_t error, uint64_t rip, uint64_t address);
void linux_report_kernel_exception(uint64_t vector, uint64_t error, uint64_t rip, uint64_t address);

#endif
