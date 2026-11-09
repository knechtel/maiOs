#ifndef MAIOS_MICROPY_STDLIB_H
#define MAIOS_MICROPY_STDLIB_H
#include <stddef.h>
void abort(void);
void *malloc(size_t size);
void *realloc(void *pointer, size_t size);
void free(void *pointer);
long strtol(const char *text, char **end, int base);
unsigned long strtoul(const char *text, char **end, int base);
long long strtoll(const char *text, char **end, int base);
unsigned long long strtoull(const char *text, char **end, int base);
int abs(int value);
void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *));
#endif
