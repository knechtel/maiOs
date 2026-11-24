#ifndef MAIOS_LINUX_COMPAT_H
#define MAIOS_LINUX_COMPAT_H

#include <stdint.h>

void linux_compat_init(void);
int64_t linux_syscall_dispatch(uint64_t *saved_registers);

#endif
