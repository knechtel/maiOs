#ifndef MAIOS_FREESTANDING_STDLIB_H
#define MAIOS_FREESTANDING_STDLIB_H
#include <stddef.h>
void abort(void);
void *malloc(size_t size);
void free(void *pointer);
#endif
