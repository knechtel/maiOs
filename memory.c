#include <stddef.h>
#include <stdint.h>
#include "memory.h"

#define PAGE_SIZE 4096ull
#define PAGE_PRESENT 0x001ull
#define PAGE_WRITABLE 0x002ull
#define PAGE_USER 0x004ull
#define PAGE_ADDRESS 0x000ffffffffff000ull
#define PHYSICAL_MEMORY_LIMIT 0x10000000ull
#define FREE_PAGE_CAPACITY 8192u

/* PML4 slots 1..255 are allocated on demand; slot 0 shares the identity PDPT. */
static uint64_t user_pdpt_pages[256];
static uint64_t next_free_page;
static uint64_t free_pages[FREE_PAGE_CAPACITY];
static uint32_t free_page_count;
static uint64_t first_free_page;
extern uint64_t pml4_table[];
extern uint64_t pdpt_table[];
extern uint8_t _kernel_end[];

static uint64_t align_page(uint64_t address)
{
    return (address + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

int memory_init(void)
{
    next_free_page = align_page((uint64_t)(uintptr_t)_kernel_end);
    if (next_free_page >= PHYSICAL_MEMORY_LIMIT) return 0;
    first_free_page = next_free_page;
    free_page_count = 0;
    pml4_table[0] |= PAGE_USER;
    for (unsigned int i = 1; i < 256; ++i) {
        pml4_table[i] = 0;
        user_pdpt_pages[i] = 0;
    }
    for (unsigned int i = 256; i < 512; ++i) pdpt_table[i] = 0;
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    return 1;
}

uint64_t memory_alloc_page(void)
{
    uint64_t page;
    if (free_page_count) page = free_pages[--free_page_count];
    else {
        if (!next_free_page || next_free_page + PAGE_SIZE > PHYSICAL_MEMORY_LIMIT) return 0;
        page = next_free_page;
        next_free_page += PAGE_SIZE;
    }
    uint8_t *bytes = (uint8_t *)(uintptr_t)page;
    for (size_t i = 0; i < PAGE_SIZE; ++i) bytes[i] = 0;
    return page;
}

void memory_free_page(uint64_t physical_address)
{
    if (physical_address < first_free_page || physical_address >= next_free_page ||
        (physical_address & (PAGE_SIZE - 1)) || free_page_count >= FREE_PAGE_CAPACITY)
        return;
    free_pages[free_page_count++] = physical_address;
}

uint64_t memory_checkpoint(void)
{
    return next_free_page;
}

void memory_reset_user(uint64_t checkpoint)
{
    if (!checkpoint || checkpoint > next_free_page) return;
    for (unsigned int i = 1; i < 256; ++i) {
        pml4_table[i] = 0;
        user_pdpt_pages[i] = 0;
    }
    free_page_count = 0;
    for (unsigned int i = 256; i < 512; ++i) pdpt_table[i] = 0;
    next_free_page = checkpoint;
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

static uint64_t *user_pte(uint64_t virtual_address, int create)
{
    if (virtual_address < MAIOS_USER_BASE || virtual_address >= MAIOS_USER_LIMIT)
        return 0;
    unsigned int pml4_index = (unsigned int)((virtual_address >> 39) & 0x1ffu);
    unsigned int pdpt_index = (unsigned int)((virtual_address >> 30) & 0x1ffu);
    unsigned int pd_index = (unsigned int)((virtual_address >> 21) & 0x1ffu);
    unsigned int pt_index = (unsigned int)((virtual_address >> 12) & 0x1ffu);
    if (pml4_index >= 256) return 0;

    uint64_t *pdpt;
    if (!pml4_index) {
        pdpt = pdpt_table;
    } else {
        uint64_t page = user_pdpt_pages[pml4_index];
        if (!page) {
            if (!create) return 0;
            page = memory_alloc_page();
            if (!page) return 0;
            user_pdpt_pages[pml4_index] = page;
            pml4_table[pml4_index] = page | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
        }
        pdpt = (uint64_t *)(uintptr_t)page;
    }

    uint64_t pdpt_entry = pdpt[pdpt_index];
    if (!(pdpt_entry & PAGE_PRESENT)) {
        if (!create) return 0;
        uint64_t page = memory_alloc_page();
        if (!page) return 0;
        pdpt[pdpt_index] = page | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
        pdpt_entry = pdpt[pdpt_index];
    }
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt_entry & PAGE_ADDRESS);
    uint64_t pd_entry = pd[pd_index];
    if (!(pd_entry & PAGE_PRESENT)) {
        if (!create) return 0;
        uint64_t page = memory_alloc_page();
        if (!page) return 0;
        pd[pd_index] = page | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
        pd_entry = pd[pd_index];
    }
    uint64_t *pt = (uint64_t *)(uintptr_t)(pd_entry & PAGE_ADDRESS);
    return &pt[pt_index];
}

int memory_map_user_page(uint64_t virtual_address, uint64_t physical_address,
                         int writable)
{
    if ((virtual_address & (PAGE_SIZE - 1)) ||
        (physical_address & (PAGE_SIZE - 1)) ||
        physical_address >= PHYSICAL_MEMORY_LIMIT) return 0;
    uint64_t *entry = user_pte(virtual_address, 1);
    if (!entry) return 0;
    if (*entry & PAGE_PRESENT) {
        if (writable) *entry |= PAGE_WRITABLE;
        return 1;
    }
    *entry = physical_address | PAGE_PRESENT | PAGE_USER |
             (writable ? PAGE_WRITABLE : 0);
    __asm__ volatile("invlpg (%0)" : : "r"((uintptr_t)virtual_address) : "memory");
    return 1;
}

int memory_user_page_mapped(uint64_t virtual_address)
{
    uint64_t *entry = user_pte(virtual_address, 0);
    return entry && ((*entry & (PAGE_PRESENT | PAGE_USER)) ==
                     (PAGE_PRESENT | PAGE_USER));
}

int memory_load_to_user(uint64_t destination, const void *source, size_t length)
{
    const uint8_t *input = source;
    if (destination < MAIOS_USER_BASE || destination > MAIOS_USER_LIMIT ||
        length > MAIOS_USER_LIMIT - destination) return 0;
    while (length) {
        uint64_t virtual_page = destination & ~(PAGE_SIZE - 1);
        uint64_t *entry = user_pte(virtual_page, 0);
        if (!entry || !(*entry & PAGE_PRESENT) || !(*entry & PAGE_USER)) return 0;
        size_t offset = (size_t)(destination & (PAGE_SIZE - 1));
        size_t count = PAGE_SIZE - offset;
        if (count > length) count = length;
        uint8_t *physical = (uint8_t *)(uintptr_t)((*entry & PAGE_ADDRESS) + offset);
        for (size_t i = 0; i < count; ++i) physical[i] = input[i];
        destination += count;
        input += count;
        length -= count;
    }
    return 1;
}

static int user_range_valid(uint64_t address, size_t length)
{
    return address >= MAIOS_USER_BASE && address < MAIOS_USER_LIMIT && length &&
           !(address & (PAGE_SIZE - 1)) && length <= MAIOS_USER_LIMIT - address &&
           !(length & (PAGE_SIZE - 1));
}

int memory_unmap_user(uint64_t address, size_t length)
{
    if (!user_range_valid(address, length)) return 0;
    for (uint64_t current = address; current < address + length; current += PAGE_SIZE) {
        uint64_t *entry = user_pte(current, 0);
        if (entry) *entry = 0;
        __asm__ volatile("invlpg (%0)" : : "r"((uintptr_t)current) : "memory");
    }
    return 1;
}

int memory_protect_user(uint64_t address, size_t length, int writable)
{
    if (!user_range_valid(address, length)) return 0;
    for (uint64_t current = address; current < address + length; current += PAGE_SIZE) {
        uint64_t *entry = user_pte(current, 0);
        if (!entry || !(*entry & PAGE_PRESENT)) return 0;
    }
    for (uint64_t current = address; current < address + length; current += PAGE_SIZE) {
        uint64_t *entry = user_pte(current, 0);
        if (writable) *entry |= PAGE_WRITABLE;
        else *entry &= ~PAGE_WRITABLE;
        __asm__ volatile("invlpg (%0)" : : "r"((uintptr_t)current) : "memory");
    }
    return 1;
}

static int user_copy(void *kernel_buffer, uint64_t user_address, size_t length,
                     int to_user)
{
    uint8_t *kernel = kernel_buffer;
    if (user_address < MAIOS_USER_BASE || user_address > MAIOS_USER_LIMIT ||
        length > MAIOS_USER_LIMIT - user_address) return 0;
    while (length) {
        uint64_t virtual_page = user_address & ~(PAGE_SIZE - 1);
        uint64_t *entry = user_pte(virtual_page, 0);
        if (!entry || !(*entry & PAGE_PRESENT) || !(*entry & PAGE_USER) ||
            (to_user && !(*entry & PAGE_WRITABLE))) return 0;
        uint64_t physical = (*entry & PAGE_ADDRESS) + (user_address & (PAGE_SIZE - 1));
        size_t count = PAGE_SIZE - (size_t)(user_address & (PAGE_SIZE - 1));
        if (count > length) count = length;
        uint8_t *mapped = (uint8_t *)(uintptr_t)physical;
        for (size_t i = 0; i < count; ++i) {
            if (to_user) mapped[i] = kernel[i];
            else kernel[i] = mapped[i];
        }
        kernel += count;
        user_address += count;
        length -= count;
    }
    return 1;
}

int memory_copy_from_user(void *destination, uint64_t source, size_t length)
{
    return user_copy(destination, source, length, 0);
}

int memory_copy_to_user(uint64_t destination, const void *source, size_t length)
{
    return user_copy((void *)source, destination, length, 1);
}
