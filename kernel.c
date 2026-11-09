#include <stdint.h>
#include <stddef.h>
#include "ext4.h"

#define VGA_MEMORY ((volatile uint16_t *)0xB8000)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_COLOR 0x0Fu
#define INPUT_SIZE 1100
static size_t cursor_x;
static size_t cursor_y;
static uint8_t color = VGA_COLOR;
static int disk_ready;

static int streq(const char *left, const char *right);
static void print(const char *text);
static void put_char(char c);
static char read_key(void);
static void write_interactive(const char *name);

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

static int valid_file_name(const char *name)
{
    size_t length = 0;
    while (name[length]) {
        if (name[length] == ' ' || name[length] == '/' || name[length] == '\\' || length >= 63)
            return 0;
        ++length;
    }
    return length > 0;
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
    if (!valid_file_name(name)) return 0;
    ext4_entry_t entries[32];
    int count = ext4_list(entries, 32);
    for (int i = 0; i < count && i < 32; ++i)
        if (entries[i].directory && streq(entries[i].name, name)) return -1;
    return ext4_mkdir(name) ? 1 : -3;
}

static void list_files(void)
{
    ext4_entry_t entries[32];
    int count = ext4_list(entries, 32);
    if (count < 0) { print("Erro ao ler diretorio ext4.\n"); return; }
    if (count == 0) print("Diretorio vazio.\n");
    for (int i = 0; i < count && i < 32; ++i) {
        print(entries[i].name);
        if (entries[i].directory) put_char('/');
        put_char('\n');
    }
}

static void show_file(const char *name)
{
    uint32_t size;
    uint8_t data[1024];
    if (!ext4_read_start(name, &size)) { print("Arquivo nao encontrado ou nao suportado.\n"); return; }
    for (uint32_t block = 0; block < (size + 1023) / 1024; ++block) {
        if (!ext4_read_block(block, data)) { print("Erro ao ler bloco do arquivo.\n"); break; }
        uint32_t count = size - block * 1024;
        if (count > 1024) count = 1024;
        for (uint32_t i = 0; i < count; ++i) put_char((char)data[i]);
    }
    ext4_read_end();
    put_char('\n');
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
        print("  cd CAMINHO muda de diretorio (use .. para voltar)\n");
        print("  pwd        mostra o caminho atual\n");
        print("  mkdir NOME cria um diretorio no disco\n");
        print("  write NOME TEXTO grava uma linha; sem TEXTO inicia escrita longa\n");
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
        else if (result == -3)
            print("mkdir: erro ao gravar no disco.\n");
        else if (result == -4)
            print("Disco indisponivel.\n");
        else
            print("Uso: mkdir NOME (nome simples, maximo 63 caracteres).\n");
    } else if (streq(command, "mkdir")) {
        print("Uso: mkdir NOME (nome simples, maximo 63 caracteres).\n");
    } else if (streq(command, "ls")) {
        if (disk_ready) list_files();
        else print("Disco ext4 indisponivel ou formato nao suportado.\n");
    } else if (starts_with(command, "write ")) {
        char *name = command + 6;
        char *contents = name;
        while (*contents && *contents != ' ') ++contents;
        if (*contents == '\0') {
            if (!disk_ready) print("Disco indisponivel.\n");
            else if (!valid_file_name(name)) print("Uso: write NOME [TEXTO]\n");
            else write_interactive(name);
        } else {
            *contents++ = '\0';
            if (!disk_ready) print("Disco indisponivel.\n");
            else if (!valid_file_name(name)) print("Nome invalido (ate 63 caracteres, sem espacos ou barras).\n");
            else {
                size_t size = 0; while (contents[size]) ++size;
                if (ext4_write(name, (const uint8_t *)contents, size)) print("Arquivo salvo no ext4.\n");
                else print("Erro ao gravar: espaco insuficiente ou recurso ext4 nao suportado.\n");
            }
        }
    } else if (starts_with(command, "cat ")) {
        if (!disk_ready) print("Disco indisponivel.\n");
        else show_file(command + 4);
    } else if (starts_with(command, "cd ")) {
        if (!disk_ready) print("Disco indisponivel.\n");
        else if (!ext4_chdir(command + 3)) print("Diretorio nao encontrado ou caminho profundo demais.\n");
    } else if (streq(command, "cd")) {
        print("Uso: cd CAMINHO (use / para raiz e .. para voltar).\n");
    } else if (streq(command, "pwd")) {
        char path[1100];
        if (disk_ready && ext4_pwd(path, sizeof(path))) { print(path); put_char('\n'); }
        else print("Disco indisponivel.\n");
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

static void write_interactive(const char *name)
{
    char line[INPUT_SIZE];
    uint8_t block[1024];
    size_t line_length = 0;
    size_t used = 0;
    uint32_t written = 0;
    uint32_t block_index = 0;

    if (!ext4_write_begin(name)) { print("Nao foi possivel abrir o arquivo para gravacao.\n"); return; }
    print("Digite o texto; finalize com um ponto sozinho em uma linha.\n");
    for (;;) {
        char key = read_key();
        if (key == '\n') {
            put_char('\n');
            if (line_length == 1 && line[0] == '.') {
                if (used && !ext4_write_block(block_index, block, used)) {
                    ext4_write_finish(written);
                    print("Erro ao gravar o arquivo.\n");
                    return;
                }
                written += (uint32_t)used;
                if (ext4_write_finish(written)) print("Arquivo salvo no ext4.\n");
                else print("Erro ao finalizar o arquivo.\n");
                return;
            }
            for (size_t i = 0; i <= line_length; ++i) {
                block[used++] = i == line_length ? '\n' : (uint8_t)line[i];
                if (used == sizeof(block)) {
                    if (!ext4_write_block(block_index++, block, used)) {
                        ext4_write_finish(written);
                        print("Erro ao gravar o arquivo.\n");
                        return;
                    }
                    written += sizeof(block);
                    used = 0;
                }
            }
            line_length = 0;
        } else if (key == '\b') {
            if (line_length) { --line_length; put_char('\b'); }
        } else if (key != '\t' && line_length < INPUT_SIZE - 1) {
            line[line_length++] = key;
            put_char(key);
        }
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
    disk_ready = ext4_mount();
    print("maiOs 0.1 - terminal do sistema\nDigite 'help' para ver os comandos.\n\n");
    if (disk_ready) print("Disco pronto.\n");
    else print("Imagem ext4 ausente ou com recursos nao suportados.\n");
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
