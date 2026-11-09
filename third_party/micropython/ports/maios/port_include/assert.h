#ifndef MAIOS_MICROPY_ASSERT_H
#define MAIOS_MICROPY_ASSERT_H
void __assert_func(const char *file, int line, const char *function, const char *expression);
#define assert(expr) ((expr) ? (void)0 : __assert_func(__FILE__, __LINE__, __func__, #expr))
#endif
