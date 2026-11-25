#ifndef MAIOS_EXT4_H
#define MAIOS_EXT4_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char name[64];
    uint8_t directory;
} ext4_entry_t;

int ext4_mount(void);
int ext4_list(ext4_entry_t *entries, size_t capacity);
int ext4_chdir(const char *path);
int ext4_pwd(char *buffer, size_t capacity);
int ext4_resolve_path(const char *path, char *resolved, size_t capacity,
                      int allow_missing_leaf);
int ext4_inode_number(const char *path, uint32_t *inode);
int ext4_readlink(const char *path, char *target, size_t capacity);
/* Returns 0 when absent, 1 for a file, 2 for a directory, or 3 for a symlink. */
int ext4_entry_type(const char *name);
int ext4_read_start(const char *name, uint32_t *size);
int ext4_read_block(uint32_t index, uint8_t *data);
void ext4_read_end(void);
int ext4_write_begin(const char *name);
int ext4_write_block(uint32_t index, const uint8_t *data, size_t size);
int ext4_write_finish(uint32_t size);
int ext4_write(const char *name, const uint8_t *data, size_t size);
int ext4_mkdir(const char *name);
int ext4_symlink(const char *target, const char *name);
int ext4_link(const char *existing_path, const char *new_path);
int ext4_unlink(const char *name);
int ext4_rename(const char *old_name, const char *new_name);
int ext4_rmdir(const char *name);
int ext4_delete(const char *name);

#endif
