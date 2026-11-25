#ifndef MAIOS_PACMAN_H
#define MAIOS_PACMAN_H

typedef void (*pacman_write_fn)(const char *text);
int pacman_list_installed(pacman_write_fn write, int quiet, const char *wanted);
int pacman_install_local(const char *archive_name);
int pacman_register_arch(const char *name, const char *version);
int pacman_package_installed(const char *name);
int pacman_last_database_error(void);
int pacman_remove(const char *package_name);

#endif
