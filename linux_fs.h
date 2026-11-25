#ifndef MAIOS_LINUX_FS_H
#define MAIOS_LINUX_FS_H

#include <stdint.h>

void linux_fs_process_reset(void);
int64_t linux_fs_open(uint64_t user_path, uint64_t flags, uint64_t mode);
int64_t linux_fs_openat(uint64_t dirfd, uint64_t user_path,
                        uint64_t flags, uint64_t mode);
int64_t linux_fs_close(uint64_t fd);
int linux_fs_fd_valid(uint64_t fd);
int64_t linux_fs_read(uint64_t fd, uint64_t user_buffer, uint64_t count,
                      uint64_t offset, int positional);
int64_t linux_fs_write(uint64_t fd, uint64_t user_buffer, uint64_t count,
                       uint64_t offset, int positional);
int64_t linux_fs_seek(uint64_t fd, int64_t offset, uint64_t whence);
int64_t linux_fs_fstat(uint64_t fd, uint64_t user_stat);
int64_t linux_fs_getdents64(uint64_t fd, uint64_t user_buffer, uint64_t count);
int64_t linux_fs_stat_path(uint64_t user_path, uint64_t user_stat);
int64_t linux_fs_statx_path(uint64_t user_path, uint64_t user_statx);
int64_t linux_fs_access(uint64_t user_path);
int64_t linux_fs_fcntl(uint64_t fd, uint64_t command, uint64_t argument);
int64_t linux_fs_fsync(uint64_t fd);
int64_t linux_fs_ftruncate(uint64_t fd, int64_t length);
int64_t linux_fs_map_file(uint64_t fd, uint64_t file_offset,
                          uint64_t user_address, uint64_t length);
int64_t linux_fs_unlink(uint64_t user_path, int directory);
int64_t linux_fs_rename(uint64_t old_path, uint64_t new_path);
int64_t linux_fs_readlink(uint64_t user_path, uint64_t user_buffer,
                          uint64_t count);
int64_t linux_fs_chdir(uint64_t user_path);
int64_t linux_fs_getcwd(uint64_t user_buffer, uint64_t size);

#endif
