#include <stdint.h>
#include <stddef.h>

#define VGA_MEMORY ((volatile uint16_t *)0xB8000)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_COLOR 0x0Fu
#define INPUT_SIZE 128
#define MAX_DIRECTORIES 8
#define DIRECTORY_NAME_SIZE 22
#define MAX_FILES 16
#define FILE_NAME_SIZE 16
#define DISK_SECTOR_SIZE 512
#define FILE_DATA_LBA 2

typedef struct {
    char name[FILE_NAME_SIZE];
    uint32_t size;
} file_entry_t;

typedef struct {
    char magic[8];
    file_entry_t files[MAX_FILES];
    uint32_t directory_count;
    char directories[MAX_DIRECTORIES][DIRECTORY_NAME_SIZE];
} filesystem_t;

static size_t cursor_x;
static size_t cursor_y;
static uint8_t color = VGA_COLOR;
static filesystem_t filesystem;
static int disk_ready;

static int streq(const char *left, const char *right);
static void print(const char *text);
static void put_char(char c);

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static int ata_wait(int require_data)
{
    for (uint32_t i = 0; i < 1000000; ++i) {
        uint8_t status = inb(0x1F7);
        if (status & 0x01) return 0;
        if (!(status & 0x80) && (!require_data || (status & 0x08))) return 1;
    }
    return 0;
}

static int ata_read_sector(uint32_t lba, uint8_t *buffer)
{
    if (lba >= 0x10000000 || !ata_wait(0)) return 0;
    outb(0x1F6, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(0x1F2, 1); outb(0x1F3, (uint8_t)lba);
    outb(0x1F4, (uint8_t)(lba >> 8)); outb(0x1F5, (uint8_t)(lba >> 16));
    outb(0x1F7, 0x20);
    if (!ata_wait(1)) return 0;
    for (size_t i = 0; i < DISK_SECTOR_SIZE / 2; ++i) {
        uint16_t word = inw(0x1F0);
        buffer[i * 2] = (uint8_t)word;
        buffer[i * 2 + 1] = (uint8_t)(word >> 8);
    }
    return 1;
}

static int ata_write_sector(uint32_t lba, const uint8_t *buffer)
{
    if (lba >= 0x10000000 || !ata_wait(0)) return 0;
    outb(0x1F6, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(0x1F2, 1); outb(0x1F3, (uint8_t)lba);
    outb(0x1F4, (uint8_t)(lba >> 8)); outb(0x1F5, (uint8_t)(lba >> 16));
    outb(0x1F7, 0x30);
    if (!ata_wait(1)) return 0;
    for (size_t i = 0; i < DISK_SECTOR_SIZE / 2; ++i) {
        uint16_t word = buffer[i * 2] | ((uint16_t)buffer[i * 2 + 1] << 8);
        outw(0x1F0, word);
    }
    outb(0x1F7, 0xE7);
    return ata_wait(0);
}

static void copy_bytes(void *destination, const void *source, size_t length)
{
    uint8_t *to = destination;
    const uint8_t *from = source;
    for (size_t i = 0; i < length; ++i) to[i] = from[i];
}

static void zero_bytes(void *destination, size_t length)
{
    uint8_t *to = destination;
    for (size_t i = 0; i < length; ++i) to[i] = 0;
}

static int filesystem_save(void)
{
    uint8_t sector[DISK_SECTOR_SIZE];
    zero_bytes(sector, sizeof(sector));
    copy_bytes(sector, &filesystem, sizeof(filesystem));
    return ata_write_sector(1, sector);
}

static void filesystem_init(void)
{
    uint8_t sector[DISK_SECTOR_SIZE];
    int valid = 1;
    if (!ata_read_sector(1, sector)) return;
    copy_bytes(&filesystem, sector, sizeof(filesystem));
    for (size_t i = 0; i < 8; ++i)
        if (filesystem.magic[i] != "maiFS01"[i]) valid = 0;
    if (!valid) {
        zero_bytes(&filesystem, sizeof(filesystem));
        copy_bytes(filesystem.magic, "maiFS01", 8);
        if (!filesystem_save()) return;
    }
    disk_ready = 1;
}

static int valid_file_name(const char *name)
{
    size_t length = 0;
    while (name[length]) {
        if (name[length] == ' ' || name[length] == '/' || length >= FILE_NAME_SIZE - 1)
            return 0;
        ++length;
    }
    return length > 0;
}

static int find_file(const char *name)
{
    for (size_t i = 0; i < MAX_FILES; ++i)
        if (filesystem.files[i].name[0] && streq(filesystem.files[i].name, name))
            return (int)i;
    return -1;
}

static int save_file(const char *name, const char *contents)
{
    uint8_t sector[DISK_SECTOR_SIZE];
    size_t size = 0;
    while (contents[size]) ++size;
    if (size > DISK_SECTOR_SIZE) return -2;
    int slot = find_file(name);
    if (slot < 0)
        for (size_t i = 0; i < MAX_FILES; ++i)
            if (!filesystem.files[i].name[0]) { slot = (int)i; break; }
    if (slot < 0) return -3;
    zero_bytes(sector, sizeof(sector));
    copy_bytes(sector, contents, size);
    if (!ata_write_sector(FILE_DATA_LBA + (uint32_t)slot, sector)) return 0;
    zero_bytes(filesystem.files[slot].name, FILE_NAME_SIZE);
    copy_bytes(filesystem.files[slot].name, name, FILE_NAME_SIZE - 1);
    filesystem.files[slot].size = (uint32_t)size;
    return filesystem_save() ? 1 : 0;
}

static void list_files(void)
{
    int found = 0;
    for (size_t i = 0; i < MAX_FILES; ++i) {
        if (filesystem.files[i].name[0]) {
            print(filesystem.files[i].name);
            put_char('\n');
            found = 1;
        }
    }
    if (!found) print("Nenhum arquivo salvo.\n");
}

static void show_file(const char *name)
{
    int slot = find_file(name);
    uint8_t sector[DISK_SECTOR_SIZE];
    if (slot < 0) { print("Arquivo nao encontrado.\n"); return; }
    if (!ata_read_sector(FILE_DATA_LBA + (uint32_t)slot, sector)) {
        print("Erro ao ler o disco.\n"); return;
    }
    for (uint32_t i = 0; i < filesystem.files[slot].size; ++i)
        put_char((char)sector[i]);
    put_char('\n');
}

static void clear_screen(void)
{
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; ++i)
        VGA_MEMORY[i] = (uint16_t)((color << 8) | ' ');
    cursor_x = 0;
    cursor_y = 0;
}

static void scroll_screen(void)
{
    for (size_t y = 1; y < VGA_HEIGHT; ++y)
        for (size_t x = 0; x < VGA_WIDTH; ++x)
            VGA_MEMORY[(y - 1) * VGA_WIDTH + x] = VGA_MEMORY[y * VGA_WIDTH + x];
    for (size_t x = 0; x < VGA_WIDTH; ++x)
        VGA_MEMORY[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = (uint16_t)((color << 8) | ' ');
    cursor_y = VGA_HEIGHT - 1;
}

static void put_char(char c)
{
    if (c == '\n') {
        cursor_x = 0;
        ++cursor_y;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\b') {
        if (cursor_x > 0) {
            --cursor_x;
            VGA_MEMORY[cursor_y * VGA_WIDTH + cursor_x] = (uint16_t)((color << 8) | ' ');
        }
    } else {
        VGA_MEMORY[cursor_y * VGA_WIDTH + cursor_x] = (uint16_t)((color << 8) | (uint8_t)c);
        if (++cursor_x == VGA_WIDTH) {
            cursor_x = 0;
            ++cursor_y;
        }
    }
    if (cursor_y >= VGA_HEIGHT)
        scroll_screen();
}

static void print(const char *text)
{
    while (*text)
        put_char(*text++);
}

static int streq(const char *left, const char *right)
{
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

static int starts_with(const char *text, const char *prefix)
{
    while (*prefix && *text == *prefix) {
        ++text;
        ++prefix;
    }
    return *prefix == '\0';
}

static int create_directory(const char *name)
{
    size_t length = 0;
    while (name[length]) {
        if (name[length] == ' ' || name[length] == '/' || length >= DIRECTORY_NAME_SIZE - 1)
            return 0;
        ++length;
    }
    if (length == 0)
        return 0;

    for (size_t i = 0; i < filesystem.directory_count; ++i)
        if (streq(filesystem.directories[i], name))
            return -1;
    if (filesystem.directory_count == MAX_DIRECTORIES)
        return -2;

    for (size_t i = 0; i <= length; ++i)
        filesystem.directories[filesystem.directory_count][i] = name[i];
    ++filesystem.directory_count;
    if (!filesystem_save()) {
        --filesystem.directory_count;
        zero_bytes(filesystem.directories[filesystem.directory_count], DIRECTORY_NAME_SIZE);
        return -3;
    }
    return 1;
}

static void reboot(void)
{
    __asm__ volatile ("cli");
    while (inb(0x64) & 0x02) { }
    outb(0x64, 0xFE);
    for (;;) __asm__ volatile ("hlt");
}

static void execute_command(char *command)
{
    if (*command == '\0') return;

    if (streq(command, "help")) {
        print("Comandos disponiveis:\n");
        print("  help       mostra esta ajuda\n");
        print("  clear      limpa a tela\n");
        print("  echo TEXTO escreve uma mensagem\n");
        print("  about      informacoes do sistema\n");
        print("  ls         lista arquivos e diretorios\n");
        print("  pwd        mostra o diretorio atual\n");
        print("  mkdir NOME cria um diretorio no disco\n");
        print("  write NOME TEXTO salva texto no disco\n");
        print("  cat NOME   mostra um arquivo do disco\n");
        print("  halt       para a CPU\n");
        print("  reboot     reinicia a maquina\n");
    } else if (streq(command, "clear")) {
        clear_screen();
    } else if (starts_with(command, "echo ")) {
        print(command + 5);
        put_char('\n');
    } else if (streq(command, "echo")) {
        put_char('\n');
    } else if (streq(command, "about")) {
        print("maiOs - kernel experimental de 32 bits\nExecutando no modo protegido x86.\n");
    } else if (starts_with(command, "mkdir ")) {
        int result = disk_ready ? create_directory(command + 6) : -4;
        if (result == 1)
            print("Diretorio criado no disco.\n");
        else if (result == -1)
            print("mkdir: diretorio ja existe.\n");
        else if (result == -2)
            print("mkdir: limite de diretorios atingido.\n");
        else if (result == -3)
            print("mkdir: erro ao gravar no disco.\n");
        else if (result == -4)
            print("Disco indisponivel.\n");
        else
            print("Uso: mkdir NOME (nome sem espacos ou barras, maximo 31 caracteres).\n");
    } else if (streq(command, "mkdir")) {
        print("Uso: mkdir NOME (nome sem espacos ou barras, maximo 31 caracteres).\n");
    } else if (streq(command, "ls")) {
        for (size_t i = 0; i < filesystem.directory_count; ++i) {
            print(filesystem.directories[i]); put_char('/'); put_char('\n');
        }
        if (disk_ready) list_files();
        else if (filesystem.directory_count == 0) print("Disco indisponivel.\n");
    } else if (starts_with(command, "write ")) {
        char *name = command + 6;
        char *contents = name;
        while (*contents && *contents != ' ') ++contents;
        if (*contents == '\0') {
            print("Uso: write NOME TEXTO\n");
        } else {
            *contents++ = '\0';
            if (!disk_ready) print("Disco indisponivel.\n");
            else if (!valid_file_name(name)) print("Nome invalido (maximo 15 caracteres, sem espacos).\n");
            else {
                int result = save_file(name, contents);
                if (result == 1) print("Arquivo salvo no disco.\n");
                else if (result == -3) print("Limite de arquivos atingido.\n");
                else if (result == -2) print("Arquivo excede 512 bytes.\n");
                else print("Erro ao gravar no disco.\n");
            }
        }
    } else if (starts_with(command, "cat ")) {
        if (!disk_ready) print("Disco indisponivel.\n");
        else show_file(command + 4);
    } else if (streq(command, "pwd")) {
        print("/\n");
    } else if (streq(command, "halt")) {
        print("Sistema parado. Reinicie a maquina para continuar.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    } else if (streq(command, "reboot")) {
        print("Reiniciando...\n");
        reboot();
    } else {
        print("Comando desconhecido: ");
        print(command);
        print("\nDigite 'help' para ver os comandos.\n");
    }
}

/* Teclado PS/2 no mapa de scancodes Set 1 (teclas sem Shift). */
static const char keymap[128] = {
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',
    [0x07]='6',[0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',
    [0x0C]='-',[0x0D]='=',[0x0E]='\b',[0x0F]='\t',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',
    [0x15]='y',[0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',
    [0x1A]='[',[0x1B]=']',[0x1C]='\n',[0x1E]='a',[0x1F]='s',
    [0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',
    [0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',
    [0x2B]='\\',[0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',
    [0x30]='b',[0x31]='n',[0x32]='m',[0x33]=',',[0x34]='.',
    [0x35]='/',[0x39]=' '
};

static char read_key(void)
{
    for (;;) {
        if (inb(0x64) & 1) {
            uint8_t scan = inb(0x60);
            if (scan < 128 && keymap[scan])
                return keymap[scan];
        }
        __asm__ volatile ("pause");
    }
}

static void prompt(void)
{
    print("maiOs:/# ");
}

void kernel_main(void)
{
    char input[INPUT_SIZE];
    size_t length = 0;

    clear_screen();
    filesystem_init();
    print("maiOs 0.1 - terminal do sistema\nDigite 'help' para ver os comandos.\n\n");
    if (disk_ready) print("Disco pronto.\n");
    else print("Disco nao encontrado; conecte uma imagem IDE.\n");
    prompt();

    for (;;) {
        char key = read_key();
        if (key == '\n') {
            put_char('\n');
            input[length] = '\0';
            execute_command(input);
            length = 0;
            prompt();
        } else if (key == '\b') {
            if (length > 0) {
                --length;
                put_char('\b');
            }
        } else if (key != '\t' && length < INPUT_SIZE - 1) {
            input[length++] = key;
            put_char(key);
        }
    }
}
