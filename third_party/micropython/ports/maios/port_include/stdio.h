#ifndef MAIOS_MICROPY_STDIO_H
#define MAIOS_MICROPY_STDIO_H
#include <stdarg.h>
#include <stddef.h>
typedef struct maios_FILE FILE;
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
int printf(const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
int snprintf(char *buffer, size_t size, const char *format, ...);
int vsnprintf(char *buffer, size_t size, const char *format, va_list args);
int putchar(int value);
int getchar(void);
#endif
