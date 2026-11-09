#ifndef MAIOS_MICROPY_SYS_STAT_H
#define MAIOS_MICROPY_SYS_STAT_H
#include <sys/types.h>
struct stat { mode_t st_mode; off_t st_size; };
#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
int stat(const char *path, struct stat *buffer);
int fstat(int fd, struct stat *buffer);
#endif
