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
int ext4_read(const char *name, uint8_t *data, size_t capacity, size_t *size);
int ext4_write(const char *name, const uint8_t *data, size_t size);
int ext4_mkdir(const char *name);

#endif
