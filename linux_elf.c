#include <stddef.h>
#include <stdint.h>
#include "ext4.h"
#include "linux_compat.h"
#include "linux_elf.h"
#include "memory.h"

extern void maios_linux_console_process_active(int active);

#define ELF_PT_LOAD 1u
#define ELF_PT_DYNAMIC 2u
#define ELF_PT_INTERP 3u
#define ELF_PF_X 1u
#define ELF_PF_W 2u
#define ELF_ET_EXEC 2u
#define ELF_ET_DYN 3u
#define ELF_EM_X86_64 62u
#define ELF_MAX_PROGRAM_HEADERS 64u
#define ELF_STACK_PAGES 256u
#define PAGE_SIZE 4096u
#define MAX_ELF_IMAGE (8u * 1024u * 1024u)
#define MAIN_PIE_OFFSET 0x100000000ull
#define INTERP_PIE_OFFSET 0x200000000ull

typedef struct __attribute__((packed)) {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf64_header_t;

typedef struct __attribute__((packed)) {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} elf64_program_header_t;

typedef struct {
    uint64_t bias, entry, phdr, highest_address;
    uint16_t phent, phnum;
    uint8_t has_dynamic;
} loaded_image_t;

static uint8_t syscall_stack[64u * 1024u] __attribute__((aligned(16)));

static int range_inside(uint64_t offset, uint64_t length, size_t total)
{
    return offset <= total && length <= (uint64_t)total - offset;
}

static int parse_elf(const void *image, size_t image_size,
                     const elf64_header_t **header_out,
                     const elf64_program_header_t **programs_out)
{
    if (!image || image_size < sizeof(elf64_header_t)) return 0;
    const elf64_header_t *header = image;
    if (header->ident[0] != 0x7f || header->ident[1] != 'E' ||
        header->ident[2] != 'L' || header->ident[3] != 'F' ||
        header->ident[4] != 2 || header->ident[5] != 1 ||
        header->ident[6] != 1 ||
        (header->type != ELF_ET_EXEC && header->type != ELF_ET_DYN) ||
        header->machine != ELF_EM_X86_64 || header->version != 1 ||
        header->ehsize != sizeof(*header) ||
        header->phentsize != sizeof(elf64_program_header_t) ||
        !header->phnum || header->phnum > ELF_MAX_PROGRAM_HEADERS ||
        !range_inside(header->phoff,
                      (uint64_t)header->phnum * header->phentsize, image_size))
        return 0;
    *header_out = header;
    *programs_out = (const elf64_program_header_t *)
                    ((const uint8_t *)image + header->phoff);
    return 1;
}

static int load_segments(const void *image, size_t image_size, uint64_t bias,
                         loaded_image_t *loaded)
{
    const uint8_t *bytes = image;
    const elf64_header_t *header;
    const elf64_program_header_t *programs;
    if (!parse_elf(image, image_size, &header, &programs)) return 0;
    loaded->bias = bias;
    loaded->entry = bias + header->entry;
    loaded->phdr = 0;
    loaded->highest_address = bias;
    loaded->phent = header->phentsize;
    loaded->phnum = header->phnum;
    loaded->has_dynamic = 0;
    int entry_is_executable = 0;
    uint64_t phdr_bytes = (uint64_t)header->phnum * header->phentsize;

    for (uint16_t i = 0; i < header->phnum; ++i) {
        const elf64_program_header_t *ph = &programs[i];
        if (ph->type == ELF_PT_DYNAMIC) loaded->has_dynamic = 1;
        if (ph->type != ELF_PT_LOAD) continue;
        if (ph->filesz > ph->memsz ||
            !range_inside(ph->offset, ph->filesz, image_size) ||
            ph->vaddr > UINT64_MAX - ph->memsz ||
            bias > UINT64_MAX - ph->vaddr - ph->memsz)
            return 0;
        uint64_t segment_start = bias + ph->vaddr;
        uint64_t segment_end = segment_start + ph->memsz;
        if (segment_start < MAIOS_USER_BASE ||
            segment_end > MAIOS_USER_LIMIT -
                          (ELF_STACK_PAGES + 1u) * PAGE_SIZE)
            return 0;
        uint64_t first = segment_start & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t end = (segment_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        if (end < first) return 0;
        for (uint64_t address = first; address < end; address += PAGE_SIZE) {
            if (memory_user_page_mapped(address)) {
                if ((ph->flags & ELF_PF_W) &&
                    !memory_map_user_page(address, 0, 1)) return 0;
                continue;
            }
            uint64_t physical = memory_alloc_page();
            if (!physical || !memory_map_user_page(address, physical,
                                                    (ph->flags & ELF_PF_W) != 0))
                return 0;
        }
        if (ph->filesz &&
            !memory_load_to_user(segment_start, bytes + ph->offset,
                                 (size_t)ph->filesz)) return 0;
        if ((ph->flags & ELF_PF_X) && header->entry >= ph->vaddr &&
            header->entry - ph->vaddr < ph->memsz) entry_is_executable = 1;
        if (!loaded->phdr && header->phoff >= ph->offset &&
            header->phoff + phdr_bytes <= ph->offset + ph->filesz)
            loaded->phdr = segment_start + (header->phoff - ph->offset);
        if (segment_end > loaded->highest_address) loaded->highest_address = segment_end;
    }
    if (!entry_is_executable || !loaded->phdr ||
        !memory_user_page_mapped(loaded->entry)) return 0;
    return 1;
}

static int load_interpreter(const char *path, uint64_t *image_page,
                            uint32_t *image_size)
{
    if (!ext4_read_start(path, image_size) || !*image_size ||
        *image_size > MAX_ELF_IMAGE) {
        ext4_read_end();
        return 0;
    }
    uint64_t pages = (*image_size + PAGE_SIZE - 1u) / PAGE_SIZE;
    uint64_t first = 0;
    for (uint64_t i = 0; i < pages; ++i) {
        uint64_t page = memory_alloc_page();
        if (!page || (i && page != first + i * PAGE_SIZE)) {
            ext4_read_end(); return 0;
        }
        if (!i) first = page;
    }
    uint8_t block[1024];
    uint32_t offset = 0;
    int ok = 1;
    for (uint32_t i = 0; offset < *image_size; ++i) {
        if (!ext4_read_block(i, block)) { ok = 0; break; }
        uint32_t count = *image_size - offset;
        if (count > sizeof(block)) count = sizeof(block);
        uint8_t *destination = (uint8_t *)(uintptr_t)(first + offset);
        for (uint32_t j = 0; j < count; ++j) destination[j] = block[j];
        offset += count;
    }
    ext4_read_end();
    if (!ok) return 0;
    *image_page = first;
    return 1;
}

static uint64_t push_user_string(uint64_t *stack, const char *text, size_t length)
{
    *stack -= length;
    if (!memory_load_to_user(*stack, text, length)) return 0;
    return *stack;
}

int linux_elf_run(const void *image, size_t image_size,
                  const char *const arguments[], size_t argument_count)
{
    uint64_t checkpoint = memory_checkpoint();
    int result = -1;
    linux_process_reset();
    const elf64_header_t *header;
    const elf64_program_header_t *programs;
    if (!parse_elf(image, image_size, &header, &programs)) goto done;

    char interpreter_path[128];
    interpreter_path[0] = 0;
    for (uint16_t i = 0; i < header->phnum; ++i) {
        if (programs[i].type != ELF_PT_INTERP) continue;
        uint64_t offset = programs[i].offset, size = programs[i].filesz;
        if (!size || size > sizeof(interpreter_path) ||
            !range_inside(offset, size, image_size)) goto done;
        const char *path = (const char *)image + offset;
        size_t length = 0;
        while (length < size && path[length]) { interpreter_path[length] = path[length]; ++length; }
        if (length == size || !length) goto done;
        interpreter_path[length] = 0;
        break;
    }

    uint64_t main_bias = header->type == ELF_ET_EXEC ? MAIOS_USER_BASE :
                         MAIOS_USER_BASE + MAIN_PIE_OFFSET;
    loaded_image_t main_image;
    if (!load_segments(image, image_size, main_bias, &main_image)) goto done;
    if (!interpreter_path[0] && main_image.has_dynamic) goto done;

    uint64_t launch_entry = main_image.entry;
    uint64_t interpreter_base = 0;
    uint64_t highest_address = main_image.highest_address;
    if (interpreter_path[0]) {
        uint64_t interp_page;
        uint32_t interp_size;
        if (!load_interpreter(interpreter_path, &interp_page, &interp_size)) goto done;
        const elf64_header_t *interp_header;
        const elf64_program_header_t *interp_programs;
        if (!parse_elf((const void *)(uintptr_t)interp_page, interp_size,
                       &interp_header, &interp_programs) ||
            interp_header->type != ELF_ET_DYN) goto done;
        for (uint16_t i = 0; i < interp_header->phnum; ++i)
            if (interp_programs[i].type == ELF_PT_INTERP) goto done;
        interpreter_base = MAIOS_USER_BASE + INTERP_PIE_OFFSET;
        loaded_image_t interpreter_image;
        if (!load_segments((const void *)(uintptr_t)interp_page, interp_size,
                           interpreter_base, &interpreter_image)) goto done;
        launch_entry = interpreter_image.entry;
        if (interpreter_image.highest_address > highest_address)
            highest_address = interpreter_image.highest_address;
    }

    uint64_t stack_top = MAIOS_USER_LIMIT - PAGE_SIZE;
    uint64_t stack_bottom = stack_top - ELF_STACK_PAGES * PAGE_SIZE;
    for (uint64_t address = stack_bottom; address < stack_top; address += PAGE_SIZE) {
        uint64_t physical = memory_alloc_page();
        if (!physical || !memory_map_user_page(address, physical, 1)) goto done;
    }
    uint64_t user_stack = stack_top;
    static const char env_term[] = "TERM=vt100";
    static const char env_home[] = "HOME=/";
    static const char env_path[] = "PATH=/bin:/usr/bin";
    if (!argument_count || !arguments || argument_count > 16) goto done;
    uint64_t argv_pointers[16];
    for (size_t i = argument_count; i > 0; --i) {
        const char *argument = arguments[i - 1];
        size_t length = 0;
        while (argument[length] && length < 127) ++length;
        if (argument[length]) goto done;
        argv_pointers[i - 1] = push_user_string(&user_stack, argument, length + 1);
        if (!argv_pointers[i - 1]) goto done;
    }
    uint64_t execfn_ptr = argv_pointers[0];
    uint64_t path_ptr = push_user_string(&user_stack, env_path, sizeof(env_path));
    uint64_t home_ptr = push_user_string(&user_stack, env_home, sizeof(env_home));
    uint64_t term_ptr = push_user_string(&user_stack, env_term, sizeof(env_term));
    uint64_t random_bytes[2];
    uint32_t tsc_low, tsc_high;
    __asm__ volatile("rdtsc" : "=a"(tsc_low), "=d"(tsc_high));
    random_bytes[0] = ((uint64_t)tsc_high << 32) | tsc_low;
    random_bytes[1] = random_bytes[0] ^ (uint64_t)(uintptr_t)image ^ header->entry;
    user_stack -= sizeof(random_bytes);
    uint64_t random_ptr = user_stack;
    if (!memory_load_to_user(random_ptr, random_bytes, sizeof(random_bytes)) ||
        !execfn_ptr || !path_ptr || !home_ptr || !term_ptr) goto done;

    uint64_t words[64];
    size_t word_count = 0;
#define PUSH_WORD(value) do { words[word_count++] = (uint64_t)(value); } while (0)
    PUSH_WORD(argument_count); /* argc */
    for (size_t i = 0; i < argument_count; ++i) PUSH_WORD(argv_pointers[i]);
    PUSH_WORD(0); /* argv terminator */
    PUSH_WORD(term_ptr); PUSH_WORD(home_ptr); PUSH_WORD(path_ptr); PUSH_WORD(0); /* envp */
    PUSH_WORD(3); PUSH_WORD(main_image.phdr); /* AT_PHDR */
    PUSH_WORD(4); PUSH_WORD(main_image.phent); /* AT_PHENT */
    PUSH_WORD(5); PUSH_WORD(main_image.phnum); /* AT_PHNUM */
    PUSH_WORD(6); PUSH_WORD(PAGE_SIZE); /* AT_PAGESZ */
    PUSH_WORD(7); PUSH_WORD(interpreter_base); /* AT_BASE */
    PUSH_WORD(8); PUSH_WORD(0); /* AT_FLAGS */
    PUSH_WORD(9); PUSH_WORD(main_image.entry); /* AT_ENTRY */
    PUSH_WORD(11); PUSH_WORD(0); /* AT_UID */
    PUSH_WORD(12); PUSH_WORD(0); /* AT_EUID */
    PUSH_WORD(13); PUSH_WORD(0); /* AT_GID */
    PUSH_WORD(14); PUSH_WORD(0); /* AT_EGID */
    PUSH_WORD(25); PUSH_WORD(random_ptr); /* AT_RANDOM */
    PUSH_WORD(31); PUSH_WORD(execfn_ptr); /* AT_EXECFN */
    PUSH_WORD(0); PUSH_WORD(0); /* AT_NULL */
#undef PUSH_WORD
    user_stack = (user_stack - word_count * sizeof(uint64_t)) & ~0xfull;
    if (!memory_load_to_user(user_stack, words, word_count * sizeof(uint64_t))) goto done;

    linux_set_program_break(highest_address);
    linux_set_kernel_stack((uint64_t)(uintptr_t)(syscall_stack + sizeof(syscall_stack)));
    maios_linux_console_process_active(1);
    result = linux_enter_user(launch_entry, user_stack,
                              (uint64_t)(uintptr_t)(syscall_stack + sizeof(syscall_stack)));
    maios_linux_console_process_active(0);

done:
    linux_process_reset();
    memory_reset_user(checkpoint);
    return result;
}
