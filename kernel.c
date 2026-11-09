#include <stdint.h>
#include <stddef.h>
#include "ext4.h"
#include "e1000.h"
#include "net.h"

#define VGA_MEMORY ((volatile uint16_t *)0xB8000)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_COLOR 0x0Fu
#define INPUT_SIZE 1100
static size_t cursor_x;
static size_t cursor_y;
static uint8_t color = VGA_COLOR;
static int disk_ready;
static int network_stack_ready;

static int streq(const char *left, const char *right);
static void print(const char *text);
static void put_char(char c);
static char read_key(void);
static void write_interactive(const char *name);

static void print_hex_byte(uint8_t value)
{
    static const char digits[] = "0123456789abcdef";
    put_char(digits[value >> 4]);
    put_char(digits[value & 0x0F]);
}

static void print_decimal(uint8_t value)
{
    if (value >= 100) put_char('0' + value / 100);
    if (value >= 10) put_char('0' + (value / 10) % 10);
    put_char('0' + value % 10);
}

static void print_ipv4(uint32_t address)
{
    print_decimal((uint8_t)(address >> 24)); put_char('.');
    print_decimal((uint8_t)(address >> 16)); put_char('.');
    print_decimal((uint8_t)(address >> 8)); put_char('.');
    print_decimal((uint8_t)address);
}

static int parse_ipv4(const char *text,uint32_t *address)
{
    uint32_t result=0;
    for(uint8_t part=0;part<4;part++){
        uint16_t value=0;uint8_t digits=0;
        while(*text>='0'&&*text<='9'){
            if(++digits>3)return 0;
            value=value*10+(uint8_t)(*text-'0');
            if(value>255)return 0;
            text++;
        }
        if(!digits)return 0;
        result=(result<<8)|value;
        if(part<3){if(*text!='.')return 0;text++;}
        else if(*text)return 0;
    }
    *address=result;return 1;
}

static int parse_port(const char *text,uint16_t *port)
{
    uint32_t value=0;uint8_t digits=0;
    while(*text>='0'&&*text<='9'){
        if(++digits>5)return 0;
        value=value*10+(uint8_t)(*text-'0');
        if(value>65535)return 0;
        text++;
    }
    if(!digits||!value||*text)return 0;
    *port=(uint16_t)value;return 1;
}

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
        print("  ifconfig   mostra a configuracao IPv4\n");
        print("  ping [IPv4] testa ICMP (sem endereco, usa o gateway)\n");
        print("  dhcp       solicita configuracao IPv4 automaticamente\n");
        print("  dns NOME   consulta um registro IPv4 (A)\n");
        print("  tcp IP PORT conecta e encerra uma conexao TCP\n");
        print("  https HOST [CAMINHO] busca uma pagina HTTPS\n");
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
    } else if (streq(command, "ifconfig")) {
        if (network_stack_ready) {
            uint32_t address,mask,gateway;
            net_get_config(&address,&mask,&gateway);
            print("IPv4: ");print_ipv4(address);print("  mascara: ");print_ipv4(mask);
            print("  gateway: ");print_ipv4(gateway);print("  DNS: ");print_ipv4(net_get_dns());put_char('\n');
        } else print("Rede indisponivel.\n");
    } else if (streq(command, "ping") || starts_with(command, "ping ")) {
        if (!network_stack_ready) print("Rede indisponivel.\n");
        else {
            uint32_t destination,gateway;
            net_get_config(0,0,&gateway);
            if (streq(command,"ping")) destination=gateway;
            else {
                char *host=command+5;
                if(starts_with(host,"http://"))host+=7;
                else if(starts_with(host,"https://"))host+=8;
                for(char *end=host;*end;end++){
                    if(*end=='/'||*end=='?'||*end=='#'){*end='\0';break;}
                }
                if(!parse_ipv4(host,&destination)){
                    print("Resolvendo ");print(host);print("...\n");
                    if(!net_dns_resolve(host,&destination)){
                        print("Falha no DNS. Verifique a rede e tente: ping www.google.com.br\n");
                        return;
                    }
                    print(host);print(" -> ");print_ipv4(destination);put_char('\n');
                }
            }
            print("PING ");print_ipv4(destination);print(" ...\n");
            int result=net_ping(destination);
            if(result==1){print("Resposta ICMP de ");print_ipv4(destination);print(": 32 bytes\n");}
            else if(result==0){print("Tempo esgotado aguardando resposta.\n");}
            else print("Falha ao enviar o pedido ICMP.\n");
        }
    } else if (streq(command, "dhcp")) {
        if (!network_stack_ready) print("Rede indisponivel.\n");
        else if (net_dhcp_configure()) print("Configuracao recebida por DHCP.\n");
        else print("DHCP falhou; configuracao anterior mantida.\n");
    } else if (starts_with(command, "dns ")) {
        if (!network_stack_ready) print("Rede indisponivel.\n");
        else {
            uint32_t address;
            if (net_dns_resolve(command + 4, &address)) { print(command + 4);print(" -> ");print_ipv4(address);put_char('\n'); }
            else print("Falha na consulta DNS (registro A).\n");
        }
    } else if (streq(command, "dns")) {
        print("Uso: dns NOME\n");
    } else if (starts_with(command, "tcp ")) {
        char *ip_text=command+4,*port_text=ip_text;
        while(*port_text&&*port_text!=' ')port_text++;
        if(*port_text)*port_text++='\0';
        uint32_t address;uint16_t port;
        if(!network_stack_ready)print("Rede indisponivel.\n");
        else if(!*port_text||!parse_ipv4(ip_text,&address)||!parse_port(port_text,&port))
            print("Uso: tcp IP PORT (exemplo: tcp 10.0.2.2 80)\n");
        else {
            print("TCP conectando a ");print_ipv4(address);put_char(':');
            put_char((char)('0'+(port/10000)%10));put_char((char)('0'+(port/1000)%10));
            put_char((char)('0'+(port/100)%10));put_char((char)('0'+(port/10)%10));put_char((char)('0'+port%10));print(" ...\n");
            if(net_tcp_connect(address,port)){
                print("Handshake TCP concluido.\n");
                if(!net_tcp_close())print("Aviso: encerramento TCP nao confirmado.\n");
            } else print("Falha ao conectar (porta fechada ou tempo esgotado).\n");
        }
    } else if (streq(command, "tcp")) {
        print("Uso: tcp IP PORT\n");
    } else if (starts_with(command, "https ")) {
        if(!network_stack_ready){print("Rede indisponivel.\n");}
        else {
            char *input_host=command+6,*path="/";
            char host[256];uint16_t host_length=0;
            if(starts_with(input_host,"https://"))input_host+=8;
            char *separator=input_host;
            while(*separator&&*separator!='/')separator++;
            while(input_host+host_length<separator&&host_length<sizeof(host)-1){host[host_length]=input_host[host_length];host_length++;}
            host[host_length]='\0';
            if(*separator)path=separator;
            uint8_t response[2048];uint16_t response_length=0;
            if(input_host+host_length<separator){print("Nome do host muito longo.\n");return;}
            print("Conectando via HTTPS a ");print(host);print("...\n");
            if(!net_https_get(host,path,response,sizeof(response),&response_length))
                print("HTTPS falhou: DNS, TCP, certificado ou TLS nao validado.\n");
            else {
                print("Resposta HTTPS (ate 2047 bytes):\n");
                for(uint16_t i=0;i<response_length;i++){
                    uint8_t ch=response[i];
                    if(ch=='\r')continue;
                    if(ch=='\n'||ch=='\t'||(ch>=0x20&&ch<0x7F))put_char((char)ch);
                }
                if(!response_length||response[response_length-1]!='\n')put_char('\n');
            }
        }
    } else if (streq(command, "https")) {
        print("Uso: https HOST [CAMINHO] (exemplo: https google.com.br /)\n");
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

/* Teclado PS/2 Set 1, com posições ABNT2. ç/Ç usam glifos CP437 da VGA. */
static const char keymap[128] = {
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',
    [0x07]='6',[0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',
    [0x0C]='-',[0x0D]='=',[0x0E]='\b',[0x0F]='\t',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',
    [0x15]='y',[0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',
    [0x1A]='\'',[0x1B]='[',[0x1C]='\n',[0x1E]='a',[0x1F]='s',
    [0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',
    [0x25]='k',[0x26]='l',[0x27]=0x87,[0x28]='~',[0x29]='-',
    [0x2B]=']',[0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',
    [0x30]='b',[0x31]='n',[0x32]='m',[0x33]=',',[0x34]='.',
    [0x35]=';',[0x39]=' ',[0x56]='\\'
};

static const char shift_keymap[128] = {
    [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',
    [0x07]='^',[0x08]='&',[0x09]='*',[0x0A]='(',[0x0B]=')',
    [0x0C]='_',[0x0D]='+',[0x1A]='"',[0x1B]='{',[0x27]=0x80,
    [0x28]='^',[0x29]='_',[0x2B]='}',[0x33]=';',[0x34]=':',
    [0x35]='/',[0x56]='|'
};

static const char altgr_keymap[128] = {
    [0x03]='@',[0x10]='\\',[0x11]='|',[0x1B]='[',[0x2B]=']',
    [0x35]='?',[0x56]='|'
};

static char read_key(void)
{
    static uint8_t left_shift, right_shift, altgr, caps_lock, extended;
    for (;;) {
        if (network_stack_ready) net_poll();
        if (inb(0x64) & 1) {
            uint8_t scan = inb(0x60);
            if (scan == 0xE0) { extended = 1; continue; }
            uint8_t is_extended = extended;
            extended = 0;
            uint8_t released = (scan & 0x80) != 0;
            uint8_t code = scan & 0x7F;
            if (!is_extended && code == 0x2A) { left_shift = !released; continue; }
            if (!is_extended && code == 0x36) { right_shift = !released; continue; }
            if (is_extended && code == 0x38) { altgr = !released; continue; }
            if (!is_extended && code == 0x3A && !released) { caps_lock = !caps_lock; continue; }
            if (released || is_extended) continue;
            char value = keymap[code];
            uint8_t shifted = left_shift || right_shift;
            if (altgr && altgr_keymap[code]) value = altgr_keymap[code];
            else if (value >= 'a' && value <= 'z') {
                if (shifted != caps_lock) value -= 'a' - 'A';
            } else if (shifted && shift_keymap[code]) value = shift_keymap[code];
            if (value) return value;
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
    uint8_t mac[6];
    int network_device_ready = e1000_init(mac);
    network_stack_ready = network_device_ready;
    if (network_stack_ready) net_init(mac);
    print("maiOs 0.1 - terminal do sistema\nDigite 'help' para ver os comandos.\n\n");
    if (disk_ready) print("Disco pronto.\n");
    else print("Imagem ext4 ausente ou com recursos nao suportados.\n");
    if (network_device_ready) {
        print("E1000 inicializada. MAC=");
        for (uint8_t i = 0; i < 6; ++i) {
            if (i) put_char(':');
            print_hex_byte(mac[i]);
        }
        put_char('\n');
        print("Solicitando configuracao DHCP...\n");
        if (net_dhcp_configure()) print("Configuracao recebida por DHCP.\n");
        else print("DHCP indisponivel; usando configuracao estatica 10.0.2.15/24.\n");
    } else {
        print("Placa E1000 nao encontrada ou falha na inicializacao.\n");
    }
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
