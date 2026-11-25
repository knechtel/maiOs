#ifndef MAIOS_MEMORY_H
#define MAIOS_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#define MAIOS_USER_BASE 0x0000004000000000ull
#define MAIOS_USER_LIMIT 0x0000800000000000ull

int memory_init(void);
uint64_t memory_alloc_page(void);
void memory_free_page(uint64_t physical_address);
uint64_t memory_checkpoint(void);
void memory_reset_user(uint64_t checkpoint);
int memory_map_user_page(uint64_t virtual_address, uint64_t physical_address,
                         int writable);
int memory_user_page_mapped(uint64_t virtual_address);
int memory_load_to_user(uint64_t destination, const void *source, size_t length);
int memory_unmap_user(uint64_t address, size_t length);
int memory_protect_user(uint64_t address, size_t length, int writable);
int memory_copy_from_user(void *destination, uint64_t source, size_t length);
int memory_copy_to_user(uint64_t destination, const void *source, size_t length);

#endif
