#ifndef MAIOS_ARCHREPO_H
#define MAIOS_ARCHREPO_H

typedef void (*archrepo_log_fn)(const char *text);

enum {
    ARCHREPO_ERROR_NONE = 0,
    ARCHREPO_ERROR_PACKAGE_NAME,
    ARCHREPO_ERROR_PACKAGE_DATABASE,
    ARCHREPO_ERROR_METADATA,
    ARCHREPO_ERROR_DEPENDENCY,
    ARCHREPO_ERROR_CACHE,
    ARCHREPO_ERROR_DOWNLOAD,
    ARCHREPO_ERROR_EXTRACT,
    ARCHREPO_ERROR_PACKAGE_INFO,
    ARCHREPO_ERROR_REGISTER
};

/* Resolve and install a package plus its required dependencies from Arch. */
int archrepo_install(const char *package_name, archrepo_log_fn log);
int archrepo_last_error(void);

#endif
