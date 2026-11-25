#ifndef MAIOS_ARCHPKG_H
#define MAIOS_ARCHPKG_H

#include <stddef.h>

enum {
    ARCHPKG_ERROR_NONE = 0,
    ARCHPKG_ERROR_ARCHIVE_READ,
    ARCHPKG_ERROR_ZSTD_INIT,
    ARCHPKG_ERROR_ZSTD_DECODE,
    ARCHPKG_ERROR_TEMP_WRITE,
    ARCHPKG_ERROR_TAR_FORMAT,
    ARCHPKG_ERROR_TAR_FILESYSTEM,
    ARCHPKG_ERROR_MANIFEST
};

typedef void (*archpkg_progress_fn)(size_t entries);
void archpkg_set_progress_callback(archpkg_progress_fn callback);

enum {
    ARCHPKG_FS_ERROR_NONE = 0,
    ARCHPKG_FS_ERROR_PATH,
    ARCHPKG_FS_ERROR_REPLACE,
    ARCHPKG_FS_ERROR_SYMLINK,
    ARCHPKG_FS_ERROR_HARDLINK,
    ARCHPKG_FS_ERROR_FILE_OPEN,
    ARCHPKG_FS_ERROR_FILE_READ,
    ARCHPKG_FS_ERROR_FILE_WRITE,
    ARCHPKG_FS_ERROR_FILE_FINISH
};

/* Installs an uncompressed POSIX/ustar package archive from ext4. */
int archpkg_install_tar(const char *archive_path);
int archpkg_install_zstd(const char *archive_path);
int archpkg_last_error(void);
const char *archpkg_last_failed_path(void);
int archpkg_last_filesystem_error(void);
int archpkg_package_info(char *name, size_t name_capacity,
                         char *version, size_t version_capacity);
size_t archpkg_dependency_count(void);
int archpkg_dependency(size_t index, char *dependency, size_t capacity);
int archpkg_remove_files(const char *package_name);

#endif
