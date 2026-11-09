#ifndef MAIOS_MICROPY_ALLOCA_H
#define MAIOS_MICROPY_ALLOCA_H
#define alloca(size) __builtin_alloca(size)
#endif
