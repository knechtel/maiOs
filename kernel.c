#include <stdint.h>
#include <stddef.h>

#define VGA_MEMORY ((volatile uint16_t *)0xB8000)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_COLOR 0x0Fu
#define INPUT_SIZE 128
#define MAX_DIRECTORIES 16
#define DIRECTORY_NAME_SIZE 32

static size_t cursor_x;
static size_t cursor_y;
static uint8_t color = VGA_COLOR;
static char directories[MAX_DIRECTORIES][DIRECTORY_NAME_SIZE];
static size_t directory_count;

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

    for (size_t i = 0; i < directory_count; ++i)
        if (streq(directories[i], name))
            return -1;
    if (directory_count == MAX_DIRECTORIES)
        return -2;

    for (size_t i = 0; i <= length; ++i)
        directories[directory_count][i] = name[i];
    ++directory_count;
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
        print("  ls         lista diretorios criados\n");
        print("  pwd        mostra o diretorio atual\n");
        print("  mkdir NOME cria um diretorio na memoria\n");
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
        int result = create_directory(command + 6);
        if (result == 1)
            print("Diretorio criado na memoria.\n");
        else if (result == -1)
            print("mkdir: diretorio ja existe.\n");
        else if (result == -2)
            print("mkdir: limite de diretorios atingido.\n");
        else
            print("Uso: mkdir NOME (nome sem espacos ou barras, maximo 31 caracteres).\n");
    } else if (streq(command, "mkdir")) {
        print("Uso: mkdir NOME (nome sem espacos ou barras, maximo 31 caracteres).\n");
    } else if (streq(command, "ls")) {
        if (directory_count == 0) {
            print("Nenhum diretorio criado.\n");
        } else {
            for (size_t i = 0; i < directory_count; ++i) {
                print(directories[i]);
                put_char('/');
                put_char('\n');
            }
        }
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
    print("maiOs 0.1 - terminal do sistema\nDigite 'help' para ver os comandos.\n\n");
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
