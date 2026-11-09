#ifndef MAIOS_MICROPY_UNISTD_H
#define MAIOS_MICROPY_UNISTD_H
#include <sys/types.h>
ssize_t read(int fd, void *buffer, size_t size);
ssize_t write(int fd, const void *buffer, size_t size);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int isatty(int fd);
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif
