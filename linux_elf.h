#ifndef MAIOS_LINUX_ELF_H
#define MAIOS_LINUX_ELF_H

#include <stddef.h>
#include <stdint.h>

/* Runs an x86_64 Linux ELF image, including a PT_INTERP interpreter if present. */
int linux_elf_run(const void *image, size_t image_size,
                  const char *const arguments[], size_t argument_count);

#endif
