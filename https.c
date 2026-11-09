#include <stdint.h>
#include <stddef.h>
#include "net.h"
#include "bearssl.h"

extern const br_x509_trust_anchor maios_trust_anchors[];
extern const unsigned int maios_trust_anchor_count;

/* TLS buffers and contexts live in BSS; the boot stack is intentionally small. */
static br_ssl_client_context tls_client;
static br_x509_minimal_context tls_x509;
static br_sslio_context tls_io;
static uint8_t tls_buffer[32768];

typedef struct {
    uint8_t buffer[512];
    uint16_t position;
    uint16_t length;
    uint8_t eof;
} https_reader_t;

static inline void outb(uint16_t port,uint8_t value)
{
    __asm__ volatile("outb %0,%1"::"a"(value),"Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1,%0":"=a"(value):"Nd"(port));
    return value;
}

static uint8_t cmos(uint8_t reg)
{
    outb(0x70,(uint8_t)(reg|0x80));
    return inb(0x71);
}

static uint8_t from_bcd(uint8_t value)
{
    return (uint8_t)((value&0x0F)+((value>>4)*10));
}

/* Set the certificate verifier's clock from the emulated/physical RTC. */
static int rtc_set_validation_time(void)
{
    uint8_t status,second,minute,hour,day,month,year,century;
    for(uint32_t wait=0;wait<1000000;wait++)if(!(cmos(0x0A)&0x80))break;
    status=cmos(0x0B);second=cmos(0);minute=cmos(2);hour=cmos(4);
    day=cmos(7);month=cmos(8);year=cmos(9);century=cmos(0x32);
    uint8_t pm=hour&0x80;hour&=0x7F;
    if(!(status&0x04)){
        second=from_bcd(second);minute=from_bcd(minute);hour=from_bcd(hour);
        day=from_bcd(day);month=from_bcd(month);year=from_bcd(year);
        if(century!=0&&century!=0xFF)century=from_bcd(century);
    }
    if(!(status&0x02)){
        hour=(uint8_t)(hour%12);
        if(pm)hour=(uint8_t)(hour+12);
    }
    if(century==0||century==0xFF)century=20;
    uint32_t full_year=(uint32_t)century*100+year;
    if(month<1||month>12||day<1||day>31||hour>23||minute>59||second>60)return 0;
    static const uint16_t month_days[12]={0,31,59,90,120,151,181,212,243,273,304,334};
    uint32_t leap_days=full_year?full_year/4-full_year/100+full_year/400+1:0;
    uint32_t days=365*full_year+leap_days;
    days+=month_days[month-1]+day-1;
    if(month>2&&((full_year%4==0&&full_year%100!=0)||full_year%400==0))days++;
    br_x509_minimal_set_time(&tls_x509,days,(uint32_t)hour*3600+(uint32_t)minute*60+second);
    return 1;
}

static int rdrand_available(void)
{
    uint32_t eax=1,ebx,ecx,edx;
    __asm__ volatile("cpuid":"+a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx));
    (void)ebx;(void)edx;
    return (ecx&(1u<<30))!=0;
}

static int secure_seed(uint8_t seed[32])
{
    if(!rdrand_available())return 0;
    for(uint8_t i=0;i<8;i++){
        uint32_t word=0;uint8_t ok=0;
        for(uint8_t attempt=0;attempt<10&&!ok;attempt++)
            __asm__ volatile(".byte 0x0f,0xc7,0xf0; setc %1":"=a"(word),"=m"(ok));
        if(!ok)return 0;
        seed[i*4]=(uint8_t)word;seed[i*4+1]=(uint8_t)(word>>8);
        seed[i*4+2]=(uint8_t)(word>>16);seed[i*4+3]=(uint8_t)(word>>24);
    }
    return 1;
}

static int tcp_tls_read(void *ctx,unsigned char *data,size_t length)
{
    (void)ctx;
    uint16_t received=0;
    if(length>1000)length=1000;
    int result=net_tcp_receive(data,(uint16_t)length,&received);
    return result==1?(int)received:-1;
}

static int tcp_tls_write(void *ctx,const unsigned char *data,size_t length)
{
    (void)ctx;
    size_t offset=0;
    while(offset<length){
        uint16_t part=(uint16_t)(length-offset>1000?1000:length-offset);
        if(!net_tcp_send(data+offset,part))return -1;
        offset+=part;
    }
    return (int)length;
}

static int append_text(char *buffer,uint16_t capacity,uint16_t *used,const char *text)
{
    while(*text){
        if(*used+1>=capacity)return 0;
        buffer[(*used)++]=*text++;
    }
    return 1;
}

static int append_decimal(char *buffer,uint16_t capacity,uint16_t *used,uint32_t value)
{
    char digits[10];uint8_t count=0;
    do{digits[count++]=(char)('0'+value%10);value/=10;}while(value);
    if((uint32_t)*used+count>=capacity)return 0;
    while(count)buffer[(*used)++]=digits[--count];
    return 1;
}

static int ascii_lower(int c){return c>='A'&&c<='Z'?c+('a'-'A'):c;}

static int text_equal_ci(const char *left,const char *right)
{
    while(*left&&*right&&ascii_lower((uint8_t)*left)==ascii_lower((uint8_t)*right)){left++;right++;}
    return *left==0&&*right==0;
}

static int text_contains_ci(const char *text,const char *needle)
{
    for(;*text;text++){
        const char *a=text,*b=needle;
        while(*a&&*b&&ascii_lower((uint8_t)*a)==ascii_lower((uint8_t)*b)){a++;b++;}
        if(!*b)return 1;
    }
    return 0;
}

static int reader_byte(https_reader_t *reader,uint8_t *value)
{
    if(reader->position==reader->length){
        if(reader->eof)return 0;
        int count=br_sslio_read(&tls_io,reader->buffer,sizeof(reader->buffer));
        if(count<=0){reader->eof=1;return 0;}
        reader->position=0;reader->length=(uint16_t)count;
    }
    *value=reader->buffer[reader->position++];return 1;
}

static int reader_line(https_reader_t *reader,char *line,uint16_t capacity)
{
    uint16_t used=0;uint8_t value;int saw=0;
    while(reader_byte(reader,&value)){
        saw=1;
        if(value=='\n'){line[used]=0;return 1;}
        if(value=='\r')continue;
        if(used+1>=capacity)return 0;
        line[used++]=(char)value;
    }
    return saw?0:0;
}

static int sink_data(net_https_sink_fn sink,void *context,const uint8_t *data,
                     uint16_t length,uint32_t *total)
{
    if(*total>0xFFFFFFFFu-length)return 0;
    if(sink&&!sink(data,length,context))return 0;
    *total+=length;return 1;
}

static int reader_exact(https_reader_t *reader,uint32_t length,net_https_sink_fn sink,
                        void *context,uint32_t *total)
{
    uint8_t block[512];
    while(length){
        uint16_t take=length>sizeof(block)?sizeof(block):(uint16_t)length;
        for(uint16_t i=0;i<take;i++)if(!reader_byte(reader,&block[i]))return 0;
        if(!sink_data(sink,context,block,take,total))return 0;
        length-=take;
    }
    return 1;
}

static int reader_close_delimited(https_reader_t *reader,net_https_sink_fn sink,
                                  void *context,uint32_t *total)
{
    while(reader->position<reader->length){
        uint16_t count=reader->length-reader->position;
        if(!sink_data(sink,context,reader->buffer+reader->position,count,total))return 0;
        reader->position=reader->length;
    }
    while(!reader->eof){
        int count=br_sslio_read(&tls_io,reader->buffer,sizeof(reader->buffer));
        if(count<=0){reader->eof=1;break;}
        if(!sink_data(sink,context,reader->buffer,(uint16_t)count,total))return 0;
    }
    return 1;
}

static int parse_chunk_size(const char *line,uint32_t *size)
{
    uint32_t result=0;uint8_t digits=0;
    while(*line&&*line!=';'){
        uint8_t c=(uint8_t)*line++,value;
        if(c==' '||c=='\t')continue;
        if(c>='0'&&c<='9')value=c-'0';
        else if(ascii_lower(c)>='a'&&ascii_lower(c)<='f')value=(uint8_t)(ascii_lower(c)-'a'+10);
        else return 0;
        if(result>(0xFFFFFFFFu-value)/16)return 0;
        result=result*16+value;digits++;
    }
    if(!digits)return 0;
    *size=result;return 1;
}

static int reader_chunked(https_reader_t *reader,net_https_sink_fn sink,
                          void *context,uint32_t *total)
{
    char line[128];uint8_t a,b;
    for(;;){
        if(!reader_line(reader,line,sizeof(line)))return 0;
        uint32_t size;if(!parse_chunk_size(line,&size))return 0;
        if(!size){
            do{if(!reader_line(reader,line,sizeof(line)))return 0;}while(line[0]);
            return 1;
        }
        if(!reader_exact(reader,size,sink,context,total))return 0;
        if(!reader_byte(reader,&a)||!reader_byte(reader,&b)||a!='\r'||b!='\n')return 0;
    }
}

static int parse_http_response(https_reader_t *reader,net_https_sink_fn sink,void *context,
                               uint32_t *body_length,uint16_t *status_code)
{
    char line[1024];
    if(!reader_line(reader,line,sizeof(line))||line[0]!='H'||line[1]!='T'||line[2]!='T'||line[3]!='P'||line[4]!='/')return 0;
    char *status=line;while(*status&&*status!=' ')status++;while(*status==' ')status++;
    if(status[0]<'0'||status[0]>'9'||status[1]<'0'||status[1]>'9'||status[2]<'0'||status[2]>'9')return 0;
    *status_code=(uint16_t)((status[0]-'0')*100+(status[1]-'0')*10+status[2]-'0');
    uint8_t chunked=0,has_length=0,headers_done=0;uint32_t content_length=0;uint32_t header_bytes=0;
    for(uint16_t lines=0;lines<128;lines++){
        if(!reader_line(reader,line,sizeof(line)))return 0;
        uint16_t line_length=0;while(line[line_length])line_length++;
        header_bytes+=line_length+2;if(header_bytes>8192)return 0;
        if(!line_length){headers_done=1;break;}
        char *colon=line;while(*colon&&*colon!=':')colon++;
        if(!*colon)continue;
        *colon++=0;while(*colon==' '||*colon=='\t')colon++;
        if(text_equal_ci(line,"Content-Length")){
            uint32_t value=0;uint8_t digits=0;
            while(*colon>='0'&&*colon<='9'){
                uint8_t d=(uint8_t)(*colon++-'0');if(value>(0xFFFFFFFFu-d)/10)return 0;
                value=value*10+d;digits++;
            }
            while(*colon==' '||*colon=='\t')colon++;
            if(!digits||*colon||(has_length&&value!=content_length))return 0;
            content_length=value;has_length=1;
        }else if(text_equal_ci(line,"Transfer-Encoding")&&text_contains_ci(colon,"chunked"))chunked=1;
    }
    if(!headers_done)return 0;
    *body_length=0;
    if((*status_code>=100&&*status_code<200)||*status_code==204||*status_code==304)return 1;
    if(chunked)return reader_chunked(reader,sink,context,body_length);
    if(has_length){
        if(!reader_exact(reader,content_length,sink,context,body_length))return 0;
        return 1;
    }
    return reader_close_delimited(reader,sink,context,body_length);
}

int net_https_request(const char *host,const char *path,const char *method,
                      const char *headers,const uint8_t *body,uint32_t body_length,
                      net_https_sink_fn sink,void *context,uint32_t *response_length,
                      uint16_t *status_code)
{
    uint32_t address,response_size=0;uint16_t request_length=0;char request[4096];uint8_t seed[32];
    if(!host||!path||!method||!response_length||!status_code||(!body&&body_length))return 0;
    if(!text_equal_ci(method,"GET")&&!text_equal_ci(method,"POST"))return 0;
    if(path[0]!='/')return 0;
    if(text_equal_ci(method,"GET")&&body_length)return 0;
    *response_length=0;*status_code=0;
    for(uint16_t i=0;host[i];i++)if(i>=253||host[i]=='\r'||host[i]=='\n'||host[i]==' '||host[i]=='/')return 0;
    for(uint16_t i=0;path[i];i++)if(path[i]=='\r'||path[i]=='\n'||path[i]==' ')return 0;
    for(uint16_t i=0;headers&&headers[i];i++)if(headers[i]=='\r'&&(headers[i+1]!='\n'))return 0;
    if(!net_dns_resolve(host,&address)||!net_tcp_connect(address,443))return 0;

    br_ssl_client_init_full(&tls_client,&tls_x509,maios_trust_anchors,maios_trust_anchor_count);
    br_ssl_engine_set_versions(&tls_client.eng,BR_TLS12,BR_TLS12);
    br_ssl_engine_set_buffer(&tls_client.eng,tls_buffer,sizeof tls_buffer,1);
    if(!rtc_set_validation_time()||!secure_seed(seed)){
        (void)net_tcp_close();return 0;
    }
    br_ssl_engine_inject_entropy(&tls_client.eng,seed,sizeof seed);
    for(uint8_t i=0;i<sizeof seed;i++)seed[i]=0;
    if(!br_ssl_client_reset(&tls_client,host,0)){
        (void)net_tcp_close();return 0;
    }
    br_sslio_init(&tls_io,&tls_client.eng,tcp_tls_read,0,tcp_tls_write,0);
    if(!append_text(request,sizeof request,&request_length,method)||
       !append_text(request,sizeof request,&request_length," ")||
       !append_text(request,sizeof request,&request_length,path[0]?path:"/")||
       !append_text(request,sizeof request,&request_length," HTTP/1.1\r\nHost: ")||
       !append_text(request,sizeof request,&request_length,host)||
       !append_text(request,sizeof request,&request_length,"\r\nUser-Agent: maiOs/0.1\r\nAccept-Encoding: identity\r\nConnection: close\r\n")||
       (headers&&*headers&&!append_text(request,sizeof request,&request_length,headers))||
       (headers&&*headers&&request[request_length-1]!='\n'&&!append_text(request,sizeof request,&request_length,"\r\n"))||
       (text_equal_ci(method,"POST")&&(!append_text(request,sizeof request,&request_length,"Content-Length: ")||
           !append_decimal(request,sizeof request,&request_length,body_length)||!append_text(request,sizeof request,&request_length,"\r\n")))||
       !append_text(request,sizeof request,&request_length,"\r\n")||
       br_sslio_write_all(&tls_io,request,request_length)<0){
        (void)net_tcp_close();return 0;
    }
    for(uint32_t offset=0;offset<body_length;){
        uint16_t count=(uint16_t)(body_length-offset>512?512:body_length-offset);
        if(br_sslio_write_all(&tls_io,body+offset,count)<0){(void)net_tcp_close();return 0;}
        offset+=count;
    }
    if(br_sslio_flush(&tls_io)<0){(void)net_tcp_close();return 0;}
    https_reader_t reader={0};int ok=parse_http_response(&reader,sink,context,&response_size,status_code);
    (void)net_tcp_close();
    *response_length=response_size;
    return ok;
}

typedef struct {uint8_t *buffer;uint16_t capacity;uint16_t length;} memory_sink_t;
static int memory_sink(const uint8_t *data,uint16_t length,void *context)
{
    memory_sink_t *sink=context;
    if((uint32_t)sink->length+length>=sink->capacity)return 0;
    for(uint16_t i=0;i<length;i++)sink->buffer[sink->length+i]=data[i];
    sink->length+=length;sink->buffer[sink->length]=0;return 1;
}

int net_https_get(const char *host,const char *path,uint8_t *response,
                  uint16_t capacity,uint16_t *response_length)
{
    if(!response||capacity<2||!response_length)return 0;
    memory_sink_t sink={response,capacity,0};uint32_t length=0;uint16_t status=0;
    int ok=net_https_request(host,path,"GET",0,0,0,memory_sink,&sink,&length,&status);
    *response_length=sink.length;
    response[sink.length]=0;
    (void)status;
    return ok;
}
