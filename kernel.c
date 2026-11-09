#include <stdint.h>
#include <stddef.h>
#include "ext4.h"
#include "e1000.h"
#include "net.h"
#include "gitpack.h"
#include "micropython_port.h"

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

int maios_python_read_char(void);
void maios_python_write_char(char value);
void maios_python_move_cursor_back(unsigned int count);
void maios_python_erase_line_from_cursor(unsigned int count);

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

static void print_u32(uint32_t value)
{
    char digits[10];uint8_t count=0;
    do{digits[count++]=(char)('0'+value%10);value/=10;}while(value);
    while(count)put_char(digits[--count]);
}

static void print_https_failure(void)
{
    int failure=net_https_last_error();
    if(failure==NET_HTTPS_ERROR_DNS)print("Falha DNS ao resolver o host.\n");
    else if(failure==NET_HTTPS_ERROR_TCP)print("Falha na conexao TCP porta 443.\n");
    else if(failure==NET_HTTPS_ERROR_CLOCK)print("Relogio RTC invalido para validar certificados.\n");
    else if(failure==NET_HTTPS_ERROR_RANDOM)print("RDRAND indisponivel para iniciar TLS.\n");
    else if(failure==NET_HTTPS_ERROR_TLS){
        int direction=net_https_last_io_direction();
        print("I/O TCP do TLS falhou na ");print(direction==1?"leitura":"escrita");
        if(direction==2){
            int tcp_error=net_tcp_last_send_error();
            if(tcp_error==NET_TCP_SEND_ERROR_FRAME)print(" (placa recusou o envio)");
            else if(tcp_error==NET_TCP_SEND_ERROR_RESET)print(" (servidor enviou RST)");
            else if(tcp_error==NET_TCP_SEND_ERROR_FIN)print(" (servidor encerrou a conexao)");
            else if(tcp_error==NET_TCP_SEND_ERROR_ACK_TIMEOUT)print(" (ACK nao chegou apos retransmissoes)");
            else if(tcp_error==NET_TCP_SEND_ERROR_BAD_ACK_DATA)print(" (segmento recebido invalido ou fora de ordem)");
        }else if(direction==1){
            int tcp_error=net_https_last_tcp_read_error();
            if(tcp_error==NET_TCP_READ_ERROR_TIMEOUT)print(" (tempo de espera esgotado sem dados)");
            else if(tcp_error==NET_TCP_READ_ERROR_RESET)print(" (servidor enviou RST)");
            else if(tcp_error==NET_TCP_READ_ERROR_FIN)print(" (servidor encerrou a conexao)");
            else if(tcp_error==NET_TCP_READ_ERROR_BAD_SEQUENCE)print(" (segmento recebido fora de ordem)");
            else if(tcp_error==NET_TCP_READ_ERROR_OVERSIZE)print(" (segmento maior que o buffer TCP)");
        }
        print("; erro BearSSL ");print_u32((uint32_t)net_https_last_tls_error());put_char('.');put_char('\n');
    }else if(failure==NET_HTTPS_ERROR_HTTP)print("Resposta recebida, mas HTTP estava incompleto ou malformado.\n");
    else print("Falha HTTPS sem detalhe disponivel.\n");
}

static size_t str_length(const char *text)
{
    size_t length=0;while(text[length])length++;return length;
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

int maios_python_read_char(void)
{
    return read_key();
}

void maios_python_write_char(char value)
{
    put_char(value);
}

void maios_python_move_cursor_back(unsigned int count)
{
    while(count--){
        if(cursor_x)cursor_x--;
        else if(cursor_y){cursor_y--;cursor_x=VGA_WIDTH-1;}
    }
}

void maios_python_erase_line_from_cursor(unsigned int count)
{
    size_t x=cursor_x,y=cursor_y;
    while(count--&&y<VGA_HEIGHT){
        VGA_MEMORY[y*VGA_WIDTH+x]=(uint16_t)((color<<8)|' ');
        if(++x==VGA_WIDTH){x=0;y++;}
    }
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

static char *next_argument(char **cursor)
{
    while(**cursor==' ')++*cursor;
    if(!**cursor)return 0;
    char *argument=*cursor;
    while(**cursor&&**cursor!=' ')++*cursor;
    if(**cursor){**cursor='\0';++*cursor;}
    return argument;
}

static int parse_https_target(char *target,char *separate_path,char host[256],const char **path)
{
    if(!target||!*target)return 0;
    if(starts_with(target,"https://"))target+=8;
    char *embedded_path=target;
    while(*embedded_path&&*embedded_path!='/')embedded_path++;
    uint16_t length=(uint16_t)(embedded_path-target);
    if(!length||length>253)return 0;
    for(uint16_t i=0;i<length;i++)host[i]=target[i];
    host[length]='\0';
    *path=separate_path&&*separate_path?separate_path:(*embedded_path?embedded_path:"/");
    return 1;
}

typedef struct {uint32_t total;uint16_t shown;} https_display_t;
static int https_display_sink(const uint8_t *data,uint16_t length,void *context)
{
    https_display_t *display=context;
    for(uint16_t i=0;i<length;i++){
        uint8_t ch=data[i];
        if(display->shown<2048){
            if(ch=='\r'){}
            else if(ch=='\n'){put_char('\n');display->shown++;}
            else if(ch=='\t'){put_char(' ');display->shown++;}
            else if(ch>=0x20&&ch<0x7F){put_char((char)ch);display->shown++;}
        }
    }
    display->total+=length;
    return 1;
}

typedef struct {
    uint8_t block[1024];
    uint16_t used;
    uint32_t block_index;
    uint32_t total;
    uint8_t failed;
} https_file_writer_t;

static int https_file_sink(const uint8_t *data,uint16_t length,void *context)
{
    https_file_writer_t *writer=context;
    if(writer->total>0x7FFFFFFFu-length){writer->failed=1;return 0;}
    for(uint16_t i=0;i<length;i++){
        writer->block[writer->used++]=data[i];
        if(writer->used==sizeof(writer->block)){
            if(!ext4_write_block(writer->block_index,writer->block,sizeof(writer->block))){writer->failed=1;return 0;}
            writer->block_index++;writer->used=0;
        }
    }
    writer->total+=length;return 1;
}

static uint32_t https_file_finish(https_file_writer_t *writer)
{
    uint32_t size=writer->block_index*1024u;
    if(writer->used&&!writer->failed){
        if(ext4_write_block(writer->block_index,writer->block,writer->used))size+=writer->used;
        else writer->failed=1;
    }
    if(!writer->failed)size=writer->total;
    (void)ext4_write_finish(size);
    return size;
}

typedef struct {
    uint8_t prefix[4];
    uint8_t prefix_length;
    uint16_t packet_left;
    char line[512];
    uint16_t line_length;
    char requested[128];
    char oid[65];
    char default_ref[128];
    uint8_t oid_length;
    uint8_t found;
    uint8_t malformed;
    uint8_t saw_flush;
    uint8_t side_band_64k;
    uint8_t list_refs;
} git_refs_state_t;

static int git_hex_value(uint8_t c)
{
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}

static int git_parse_packet_length(const uint8_t prefix[4])
{
    int value=0;
    for(uint8_t i=0;i<4;i++){int digit=git_hex_value(prefix[i]);if(digit<0)return -1;value=(value<<4)|digit;}
    return value;
}

static void git_print_oid(const char *oid,uint8_t length)
{
    for(uint8_t i=0;i<length;i++)put_char(oid[i]);
}

static int git_oid_valid(const char *text,uint8_t length)
{
    if(length!=40&&length!=64)return 0;
    for(uint8_t i=0;i<length;i++)if(git_hex_value((uint8_t)text[i])<0)return 0;
    return 1;
}

static int git_has_capability(const char *text,uint16_t length,const char *capability)
{
    uint16_t cap_length=0;while(capability[cap_length])cap_length++;
    for(uint16_t i=0;i+cap_length<=length;i++){
        if(i&&text[i-1]!=' ')continue;
        uint16_t j=0;while(j<cap_length&&text[i+j]==capability[j])j++;
        if(j==cap_length&&(i+j==length||text[i+j]==' '))return 1;
    }
    return 0;
}

static int git_ref_matches(const char *requested,const char *name)
{
    if(!requested[0])return 0;
    uint16_t i=0;while(requested[i]&&name[i]&&requested[i]==name[i])i++;
    if(!requested[i]&&!name[i])return 1;
    if(!requested[i]&&(!name[i]||(name[0]=='r'&&name[1]=='e'&&name[2]=='f'&&name[3]=='s'&&name[4]=='/'))){
        const char *short_name=name;
        if(starts_with(short_name,"refs/heads/"))short_name+=11;
        else if(starts_with(short_name,"refs/tags/"))short_name+=10;
        i=0;while(requested[i]&&short_name[i]&&requested[i]==short_name[i])i++;
        if(!requested[i]&&!short_name[i])return 1;
    }
    return 0;
}

static void git_process_ref_line(git_refs_state_t *state)
{
    state->line[state->line_length]=0;
    char *line=state->line;
    while(*line=='\n'||*line=='\r')line++;
    if(starts_with(line,"# service=")||!line[0]){state->line_length=0;return;}
    uint8_t oid_length=0;while(line[oid_length]&&line[oid_length]!=' '&&oid_length<65)oid_length++;
    if(!line[oid_length]||line[oid_length]!=' '||!git_oid_valid(line,oid_length)){state->line_length=0;return;}
    char *name=line+oid_length+1;uint16_t name_length=0;
    while(name[name_length]&&name[name_length]!='\n'&&name[name_length]!='\r'&&name[name_length]!='\0')name_length++;
    uint16_t caps_offset=(uint16_t)(oid_length+1+name_length);
    if(caps_offset<state->line_length&&line[caps_offset]=='\0'&&
       caps_offset+1<state->line_length){
        const char *caps=line+caps_offset+1;uint16_t caps_length=(uint16_t)(state->line_length-caps_offset-1);
        if(git_has_capability(caps,caps_length,"side-band-64k"))state->side_band_64k=1;
        static const char symref[]="symref=HEAD:";
        for(uint16_t i=0;i+sizeof(symref)-1<=caps_length;i++){
            if(i&&caps[i-1]!=' ')continue;
            uint8_t match=1;for(uint8_t j=0;j<sizeof(symref)-1;j++)if(caps[i+j]!=symref[j])match=0;
            if(!match)continue;
            uint16_t begin=(uint16_t)(i+sizeof(symref)-1),end=begin;
            while(end<caps_length&&caps[end]!=' '&&caps[end]!='\0')end++;
            if(end>begin&&end-begin<sizeof(state->default_ref)){
                uint16_t n=0;while(begin+n<end){state->default_ref[n]=caps[begin+n];n++;}state->default_ref[n]=0;
            }
            break;
        }
    }
    name[name_length]=0;
    if(!name_length||starts_with(name,"^{}")){state->line_length=0;return;}
    if(!state->default_ref[0]&&starts_with(name,"refs/heads/")){
        uint16_t n=0;while(name[n]&&n+1<sizeof(state->default_ref)){state->default_ref[n]=name[n];n++;}
        state->default_ref[n]=0;
    }
    if(state->list_refs){git_print_oid(line,oid_length);put_char(' ');print(name);put_char('\n');}
    if(!state->found&&git_ref_matches(state->requested,name)){
        for(uint8_t i=0;i<oid_length;i++)state->oid[i]=line[i];
        state->oid[oid_length]=0;state->oid_length=oid_length;state->found=1;
    }
    state->line_length=0;
}

static int git_refs_sink(const uint8_t *data,uint16_t length,void *context)
{
    git_refs_state_t *state=context;
    for(uint16_t i=0;i<length;i++){
        if(state->packet_left){
            uint8_t c=data[i];state->packet_left--;
            if(c=='\n')git_process_ref_line(state);
            else if((uint32_t)state->line_length+1u<sizeof(state->line))state->line[state->line_length++]=(char)c;
            else state->malformed=1;
            if(state->packet_left==0&&state->line_length)git_process_ref_line(state);
            continue;
        }
        state->prefix[state->prefix_length++]=data[i];
        if(state->prefix_length==4){
            int packet_length=git_parse_packet_length(state->prefix);state->prefix_length=0;
            if(packet_length<0){state->malformed=1;return 0;}
            if(packet_length==0){state->saw_flush=1;state->line_length=0;continue;}
            if(packet_length==1||packet_length==2)continue;
            if(packet_length<4){state->malformed=1;return 0;}
            state->packet_left=(uint16_t)(packet_length-4);state->line_length=0;
        }
    }
    return !state->malformed;
}

static int git_endpoint(char *buffer,uint16_t capacity,const char *repository,const char *suffix)
{
    uint16_t used=0;
    if(!repository||repository[0]!='/')return 0;
    while(*repository){if(used+1>=capacity)return 0;buffer[used++]=*repository++;}
    while(used>1&&buffer[used-1]=='/')used--;
    while(*suffix){if(used+1>=capacity)return 0;buffer[used++]=*suffix++;}
    buffer[used]=0;return 1;
}

static int git_discover_refs(const char *host,const char *repository,const char *requested,
                             uint8_t list_refs,char oid[65],uint8_t *oid_length,uint8_t *side_band_64k,
                             char *default_ref,uint16_t default_ref_capacity)
{
    char endpoint[512];git_refs_state_t state={0};
    if(!git_endpoint(endpoint,sizeof(endpoint),repository,"/info/refs?service=git-upload-pack"))return 0;
    state.list_refs=list_refs;
    if(requested){uint16_t i=0;while(requested[i]&&i<sizeof(state.requested)-1){state.requested[i]=requested[i];i++;}if(requested[i])return 0;state.requested[i]=0;}
    uint32_t response_length=0;uint16_t status=0;
    int ok=net_https_request(host,endpoint,"GET","Accept: application/x-git-upload-pack-advertisement\r\n",
                             0,0,git_refs_sink,&state,&response_length,&status);
    if(!ok){
        print_https_failure();return 0;
    }
    if(status!=200){print("Servidor respondeu HTTP ");print_u32(status);print(" ao consultar info/refs.\n");return 0;}
    if(state.malformed){print("Resposta Git info/refs contem pkt-line invalido.\n");return 0;}
    if(!state.saw_flush){print("Resposta Git info/refs terminou sem o pacote flush.\n");return 0;}
    if(state.found){for(uint8_t i=0;i<=state.oid_length;i++)oid[i]=state.oid[i];*oid_length=state.oid_length;}
    if(side_band_64k)*side_band_64k=state.side_band_64k;
    if(default_ref&&default_ref_capacity){uint16_t i=0;while(state.default_ref[i]&&i+1<default_ref_capacity){default_ref[i]=state.default_ref[i];i++;}default_ref[i]=0;}
    if(requested&&!state.found){print("A referencia pedida nao foi anunciada pelo servidor.\n");return 0;}
    return !requested||state.found;
}

typedef struct {
    uint8_t prefix[4];
    uint8_t prefix_length;
    uint16_t packet_left;
    uint16_t packet_position;
    uint8_t channel;
    uint8_t malformed;
    uint8_t saw_flush;
    uint8_t pack_magic[4];
    uint8_t pack_magic_length;
    uint8_t output[512];
    uint16_t output_length;
    https_file_writer_t *writer;
} git_pack_state_t;

static int git_pack_flush_output(git_pack_state_t *state)
{
    if(!state->output_length)return 1;
    if(!https_file_sink(state->output,state->output_length,state->writer))return 0;
    state->output_length=0;return 1;
}

static int git_pack_data(git_pack_state_t *state,uint8_t value)
{
    if(state->pack_magic_length<4)state->pack_magic[state->pack_magic_length++]=value;
    state->output[state->output_length++]=value;
    if(state->output_length==sizeof(state->output))return git_pack_flush_output(state);
    return 1;
}

static int git_pack_sink(const uint8_t *data,uint16_t length,void *context)
{
    git_pack_state_t *state=context;
    for(uint16_t i=0;i<length;i++){
        uint8_t value=data[i];
        if(state->packet_left){
            if(state->packet_position==0)state->channel=value;
            else if(state->channel==1){if(!git_pack_data(state,value))return 0;}
            else if(state->channel==3){state->malformed=1;return 0;}
            state->packet_position++;state->packet_left--;
            if(state->packet_left==0)state->packet_position=0;
            continue;
        }
        state->prefix[state->prefix_length++]=value;
        if(state->prefix_length==4){
            int packet_length=git_parse_packet_length(state->prefix);state->prefix_length=0;
            if(packet_length<0){state->malformed=1;return 0;}
            if(packet_length==0){state->saw_flush=1;continue;}
            if(packet_length==1||packet_length==2)continue;
            if(packet_length<4){state->malformed=1;return 0;}
            state->packet_left=(uint16_t)(packet_length-4);state->packet_position=0;
        }
    }
    return !state->malformed;
}

static void git_put_hex4(uint8_t *output,uint16_t value)
{
    static const char digits[]="0123456789abcdef";
    output[0]=digits[(value>>12)&15];output[1]=digits[(value>>8)&15];
    output[2]=digits[(value>>4)&15];output[3]=digits[value&15];
}

static int git_fetch_pack(const char *host,const char *repository,const char *requested,const char *filename)
{
    char oid[65],endpoint[512];uint8_t oid_length=0;uint8_t request[160];uint16_t line_length;
    uint32_t response_length=0;uint16_t status=0;
    uint8_t side_band_64k=0;
    if(!git_discover_refs(host,repository,requested,0,oid,&oid_length,&side_band_64k,0,0)||!side_band_64k)return 0;
    if(oid_length!=40)return 0;
    if(!git_endpoint(endpoint,sizeof(endpoint),repository,"/git-upload-pack"))return 0;
    static const char command_prefix[]="want ",command_suffix[]=" side-band-64k\n";
    line_length=(uint16_t)(sizeof(command_prefix)-1+oid_length+sizeof(command_suffix)-1);
    git_put_hex4(request,(uint16_t)(line_length+4));
    uint16_t p=4;
    for(uint8_t i=0;i<sizeof(command_prefix)-1;i++)request[p++]=command_prefix[i];
    for(uint8_t i=0;i<oid_length;i++)request[p++]=(uint8_t)oid[i];
    for(uint8_t i=0;i<sizeof(command_suffix)-1;i++)request[p++]=command_suffix[i];
    request[p++]='0';request[p++]='0';request[p++]='0';request[p++]='0';
    request[p++]='0';request[p++]='0';request[p++]='0';request[p++]='9';
    request[p++]='d';request[p++]='o';request[p++]='n';request[p++]='e';request[p++]='\n';
    if(!ext4_write_begin(filename))return 0;
    https_file_writer_t writer={0};git_pack_state_t pack={0};pack.writer=&writer;
    int ok=net_https_request(host,endpoint,"POST",
        "Content-Type: application/x-git-upload-pack-request\r\nAccept: application/x-git-upload-pack-result\r\n",
        request,p,git_pack_sink,&pack,&response_length,&status);
    if(!git_pack_flush_output(&pack))pack.malformed=1;
    uint32_t saved=https_file_finish(&writer);
    int received=ok&&status==200&&!pack.malformed&&!writer.failed&&pack.saw_flush&&
           !pack.packet_left&&!pack.prefix_length&&pack.pack_magic_length==4&&
           pack.pack_magic[0]=='P'&&pack.pack_magic[1]=='A'&&pack.pack_magic[2]=='C'&&pack.pack_magic[3]=='K'&&saved>0;
    if(!received)return 0;
    oid[40]=0;
    return git_install_pack(filename,oid,requested);
}

static int git_clone_repo(const char *host,const char *repository,const char *directory)
{
    char oid[65],default_ref[128];uint8_t oid_length=0;
    if(!git_discover_refs(host,repository,0,0,oid,&oid_length,0,default_ref,sizeof(default_ref))){
        print("Nao foi possivel consultar as refs remotas. Verifique host, caminho e HTTPS.\n");return 0;
    }
    if(!default_ref[0]){
        static const char fallback[]="refs/heads/main";
        uint8_t i=0;while(fallback[i]){default_ref[i]=fallback[i];i++;}default_ref[i]=0;
    }
    print("Branch selecionada: ");print(default_ref);put_char('\n');
    int created=create_directory(directory);
    if(created!=1){
        if(created!=-1||!ext4_chdir(directory)){
            print("Nao foi possivel criar o diretorio de destino.\n");return 0;
        }
        ext4_entry_t entries[32];int count=ext4_list(entries,32);uint8_t resumable=(count==0);
        for(int i=0;i<count&&i<32;i++)if(streq(entries[i].name,".git-pack"))resumable=1;
        if(!resumable){(void)ext4_chdir("..");print("O diretorio de destino ja existe e nao parece um clone incompleto.\n");return 0;}
    }else if(!ext4_chdir(directory))return 0;
    int ok=git_fetch_pack(host,repository,default_ref,".git-pack");
    if(!ok&&streq(default_ref,"refs/heads/main"))
        ok=git_fetch_pack(host,repository,"refs/heads/master",".git-pack");
    (void)ext4_chdir("..");
    if(!ok)print("Falha ao baixar ou instalar o pack dessa branch. Consulte `git refs` para conferir as refs anunciadas.\n");
    return ok;
}

static void show_help(void)
{
    static const char *const lines[]={
        "Comandos disponiveis:",
        "  help       mostra esta ajuda",
        "  clear      limpa a tela",
        "  echo TEXTO escreve uma mensagem",
        "  about      informacoes do sistema",
        "  ls         lista arquivos e diretorios",
        "  cd CAMINHO muda de diretorio (use .. para voltar)",
        "  pwd        mostra o caminho atual",
        "  ifconfig   mostra a configuracao IPv4",
        "  ping [IPv4] testa ICMP (sem endereco, usa o gateway)",
        "  dhcp       solicita configuracao IPv4 automaticamente",
        "  dns NOME   consulta um registro IPv4 (A)",
        "  tcp IP PORT conecta e encerra uma conexao TCP",
        "  https HOST [CAMINHO] busca uma pagina HTTPS",
        "  curl URL busca uma URL HTTPS e mostra o corpo",
        "  curl -o ARQUIVO URL salva o corpo HTTPS no ext4",
        "  https-post HOST CAMINHO CORPO envia POST HTTPS",
        "  https-save HOST CAMINHO ARQUIVO baixa corpo para ext4",
        "  git refs HOST REPO lista referencias remotas",
        "  git fetch HOST REPO REF ARQUIVO recebe o pack e faz checkout",
        "  git clone URL [DIRETORIO] clona um repositorio",
        "  mkdir NOME cria um diretorio no disco",
        "  rm NOME    apaga um arquivo",
        "  rmdir NOME apaga um diretorio vazio",
        "  python     inicia o REPL MicroPython",
        "  python ARQUIVO.py executa um script do ext4",
        "  write NOME TEXTO grava uma linha; sem TEXTO inicia escrita longa",
        "  cat NOME   mostra um arquivo do disco",
        "  halt       para a CPU",
        "  reboot     reinicia a maquina"
    };
    const size_t count=sizeof(lines)/sizeof(lines[0]);
    uint8_t page_lines=0;
    for(size_t i=0;i<count;i++){
        print(lines[i]);put_char('\n');page_lines++;
        if(page_lines==18&&i+1<count){
            print("-- Mais -- (tecla para continuar, q para sair)\n");
            char key=read_key();
            if(key=='q'||key=='Q')break;
            clear_screen();page_lines=0;
        }
    }
}

static void execute_command(char *command)
{
    if (*command == '\0') return;

    if (streq(command, "help")) {
        show_help();
    } else if (streq(command, "clear")) {
        clear_screen();
    } else if (starts_with(command, "echo ")) {
        print(command + 5);
        put_char('\n');
    } else if (streq(command, "echo")) {
        put_char('\n');
    } else if (streq(command, "about")) {
        print("maiOs - kernel experimental de 32 bits\nExecutando no modo protegido x86.\n");
    } else if (streq(command,"python")) {
        maios_python_repl();
    } else if (starts_with(command,"python ")) {
        const char *filename=command+7;
        if(!disk_ready)print("Disco indisponivel.\n");
        else if(!valid_file_name(filename))print("Uso: python ARQUIVO.py (arquivo no diretorio atual).\n");
        else if(!maios_python_run_file(filename))print("Nao foi possivel carregar ou executar o script Python.\n");
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
    } else if (starts_with(command, "rm ")) {
        const char *name=command+3;
        if(!disk_ready)print("Disco indisponivel.\n");
        else if(!valid_file_name(name))print("Uso: rm NOME (apaga um arquivo no diretorio atual).\n");
        else if(ext4_unlink(name))print("Arquivo apagado.\n");
        else print("rm: arquivo nao encontrado ou nao pode ser apagado.\n");
    } else if (streq(command,"rm")) {
        print("Uso: rm NOME (apaga um arquivo no diretorio atual).\n");
    } else if (starts_with(command, "rmdir ")) {
        const char *name=command+6;
        if(!disk_ready)print("Disco indisponivel.\n");
        else if(!valid_file_name(name))print("Uso: rmdir NOME (apaga um diretorio vazio no diretorio atual).\n");
        else if(ext4_rmdir(name))print("Diretorio apagado.\n");
        else print("rmdir: diretorio nao encontrado, nao vazio ou erro no disco.\n");
    } else if (streq(command,"rmdir")) {
        print("Uso: rmdir NOME (apaga um diretorio vazio no diretorio atual).\n");
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
    } else if (starts_with(command, "git refs ")) {
        if(!network_stack_ready)print("Rede indisponivel.\n");
        else {
            char *cursor=command+9,*host=next_argument(&cursor),*repository=next_argument(&cursor);
            char oid[65];uint8_t oid_length=0;
            if(!host||!repository||next_argument(&cursor))print("Uso: git refs HOST REPO_PATH\n");
            else {
                print("Referencias remotas:\n");
                (void)git_discover_refs(host,repository,0,1,oid,&oid_length,0,0,0);
            }
        }
    } else if (starts_with(command, "git fetch ")) {
        if(!network_stack_ready)print("Rede indisponivel.\n");
        else if(!disk_ready)print("Disco indisponivel.\n");
        else {
            char *cursor=command+10,*host=next_argument(&cursor),*repository=next_argument(&cursor);
            char *reference=next_argument(&cursor),*filename=next_argument(&cursor);
            if(!host||!repository||!reference||!filename||next_argument(&cursor)||!valid_file_name(filename))
                print("Uso: git fetch HOST REPO_PATH REF ARQUIVO_PACK\n");
            else {
                print("Negociando pack Git para ");print(reference);print("...\n");
                if(git_fetch_pack(host,repository,reference,filename)){
                    uint32_t size;
                    if(ext4_read_start(filename,&size)){print("Checkout concluido; pack de ");print_u32(size);print(" bytes salvo em ");print(filename);put_char('\n');ext4_read_end();}
                    else print("Checkout concluido, mas nao foi possivel ler o tamanho do pack.\n");
                } else print("Fetch Git falhou ou o servidor nao entregou um pack valido.\n");
            }
        }
    } else if (starts_with(command, "git clone ")) {
        if(!network_stack_ready)print("Rede indisponivel.\n");
        else if(!disk_ready)print("Disco indisponivel.\n");
        else {
            char *cursor=command+10,*target=next_argument(&cursor),*second=next_argument(&cursor);
            char clone_host[256];const char *host=clone_host,*repository=0;char *directory=0;
            uint8_t url_form=0;
            if(target){for(uint16_t i=0;target[i];i++)if(target[i]=='/')url_form=1;}
            if(target&&(starts_with(target,"https://")||url_form)){
                const char *url_path;
                if(!parse_https_target(target,0,clone_host,&url_path))target=0;
                else {repository=url_path;directory=second;}
            }else if(target){
                uint16_t n=0;while(target[n]&&n<sizeof(clone_host)-1)n++;
                if(target[n])target=0;else {for(uint16_t i=0;i<n;i++)clone_host[i]=target[i];clone_host[n]=0;}
                repository=second;directory=next_argument(&cursor);
            }
            char inferred[64];
            if(!directory&&repository){
                const char *base=repository;for(const char *p=repository;*p;p++)if(*p=='/')base=p+1;
                uint16_t length=0;while(base[length]&&base[length]!='?'&&length<sizeof(inferred)-1)length++;
                if(length>4&&base[length-4]=='.'&&base[length-3]=='g'&&base[length-2]=='i'&&base[length-1]=='t')length-=4;
                for(uint16_t i=0;i<length;i++)inferred[i]=base[i];inferred[length]=0;directory=inferred;
            }
            if(!host||!repository||!directory||!*directory||next_argument(&cursor)||!valid_file_name(directory))
                print("Uso: git clone URL [DIRETORIO] ou git clone HOST REPO_PATH [DIRETORIO]\n");
            else {
                print("Clonando ");print(repository);print(" em ");print(directory);print("...\n");
                if(git_clone_repo(host,repository,directory))print("Clone concluido.\n");
                else print("Clone falhou; confira a rede, a branch padrao e o espaco em disco.\n");
            }
        }
    } else if (streq(command,"git")) {
        print("Uso: git clone URL [DIRETORIO] | git refs HOST REPO_PATH | git fetch HOST REPO_PATH REF ARQUIVO_PACK\n");
    } else if (starts_with(command, "curl ")) {
        char *cursor=command+5,*first=next_argument(&cursor),*filename=0,*target=0;
        if(first&&streq(first,"-o")){
            filename=next_argument(&cursor);
            target=next_argument(&cursor);
        }else target=first;
        char host[256];const char *path;uint32_t response_length=0;uint16_t status=0;
        https_display_t display={0,0};https_file_writer_t writer={0};
        if(!network_stack_ready)print("Rede indisponivel.\n");
        else if(!target||(filename&&!valid_file_name(filename))||next_argument(&cursor)||
                !starts_with(target,"https://")||!parse_https_target(target,0,host,&path))
            print("Uso: curl [-o ARQUIVO] https://HOST/CAMINHO\n");
        else if(filename&&!disk_ready)print("Disco indisponivel.\n");
        else if(filename&&!ext4_write_begin(filename))print("Nao foi possivel abrir o arquivo no ext4.\n");
        else {
            print("Conectando via HTTPS a ");print(host);print("...\n");
            int ok=net_https_request(host,path,"GET",0,0,0,
                filename?https_file_sink:https_display_sink,
                filename?(void *)&writer:(void *)&display,&response_length,&status);
            uint32_t saved=filename?https_file_finish(&writer):0;
            if(!ok)print_https_failure();
            else if(filename&&writer.failed)print("Download incompleto; arquivo parcial preservado.\n");
            else if(filename){print("HTTP ");print_u32(status);print("; salvos ");print_u32(saved);print(" bytes em ");print(filename);put_char('\n');}
            else {
                put_char('\n');
                if(response_length>display.shown)print("Exibidos no maximo 2048 caracteres imprimiveis.\n");
            }
        }
    } else if (streq(command,"curl")) {
        print("Uso: curl [-o ARQUIVO] https://HOST/CAMINHO\n");
    } else if (starts_with(command, "https-post ")) {
        if(!network_stack_ready)print("Rede indisponivel.\n");
        else {
            char *cursor=command+11,*target=next_argument(&cursor),*path_arg=next_argument(&cursor);
            while(*cursor==' ')cursor++;
            char host[256];const char *path;uint32_t response_length=0;uint16_t status=0;
            https_display_t display={0,0};
            if(!target||!path_arg||!*cursor||!parse_https_target(target,path_arg,host,&path))
                print("Uso: https-post HOST CAMINHO CORPO\n");
            else {
                print("Resposta HTTPS POST:\n");
                int ok=net_https_request(host,path,"POST","Content-Type: application/x-www-form-urlencoded\r\n",
                    (const uint8_t *)cursor,(uint32_t)str_length(cursor),https_display_sink,&display,&response_length,&status);
                if(!ok)print_https_failure();
                else {print("\nHTTP ");print_u32(status);print("; corpo recebido: ");print_u32(response_length);print(" bytes.\n");
                    if(response_length>display.total)print("Resposta truncada na exibicao.\n");}
            }
        }
    } else if (starts_with(command, "https-save ")) {
        if(!network_stack_ready)print("Rede indisponivel.\n");
        else if(!disk_ready)print("Disco indisponivel.\n");
        else {
            char *cursor=command+11,*target=next_argument(&cursor),*path_arg=next_argument(&cursor),*filename=next_argument(&cursor);
            char host[256];const char *path;uint32_t response_length=0;uint16_t status=0;
            https_file_writer_t writer={0};
            if(!target||!path_arg||!filename||next_argument(&cursor)||!valid_file_name(filename)||
               !parse_https_target(target,path_arg,host,&path))
                print("Uso: https-save HOST CAMINHO ARQUIVO\n");
            else if(!ext4_write_begin(filename))print("Nao foi possivel abrir o arquivo no ext4.\n");
            else {
                int ok=net_https_request(host,path,"GET",0,0,0,https_file_sink,&writer,&response_length,&status);
                uint32_t saved=https_file_finish(&writer);
                if(!ok)print_https_failure();
                else if(writer.failed)print("Download HTTPS incompleto; arquivo parcial preservado.\n");
                else {print("HTTP ");print_u32(status);print("; salvos ");print_u32(saved);print(" bytes em ");print(filename);put_char('\n');}
            }
        }
    } else if (starts_with(command, "https ")) {
        if(!network_stack_ready)print("Rede indisponivel.\n");
        else {
            char *cursor=command+6,*target=next_argument(&cursor),*path_arg=next_argument(&cursor);
            char host[256];const char *path;uint32_t response_length=0;uint16_t status=0;
            https_display_t display={0,0};
            if(!target||next_argument(&cursor)||!parse_https_target(target,path_arg,host,&path)){
                print("Uso: https HOST [CAMINHO]\n");return;
            }
            print("Conectando via HTTPS a ");print(host);print("...\nCorpo da resposta:\n");
            if(!net_https_request(host,path,"GET",0,0,0,https_display_sink,&display,&response_length,&status))
                print_https_failure();
            else {
                put_char('\n');print("HTTP ");print_u32(status);print("; corpo recebido: ");print_u32(response_length);print(" bytes.\n");
                if(response_length>display.shown)print("Exibidos no maximo 2048 caracteres imprimiveis.\n");
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
    static uint8_t left_shift, right_shift, left_control, right_control, altgr, caps_lock, extended;
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
            if (!is_extended && code == 0x1D) { left_control = !released; continue; }
            if (is_extended && code == 0x1D) { right_control = !released; continue; }
            if (is_extended && code == 0x38) { altgr = !released; continue; }
            if (!is_extended && code == 0x3A && !released) { caps_lock = !caps_lock; continue; }
            if (released || is_extended) continue;
            char value = keymap[code];
            uint8_t shifted = left_shift || right_shift;
            if (altgr && altgr_keymap[code]) value = altgr_keymap[code];
            else if (value >= 'a' && value <= 'z') {
                if (shifted != caps_lock) value -= 'a' - 'A';
            } else if (shifted && shift_keymap[code]) value = shift_keymap[code];
            if ((left_control || right_control) && value >= 'a' && value <= 'z')
                value=(char)(value-'a'+1);
            else if ((left_control || right_control) && value >= 'A' && value <= 'Z')
                value=(char)(value-'A'+1);
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
