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

int net_https_get(const char *host,const char *path,uint8_t *response,
                  uint16_t capacity,uint16_t *response_length)
{
    uint32_t address;uint16_t request_length=0;char request[768];uint8_t seed[32];
    if(!host||!path||!response||capacity<2||!response_length)return 0;
    *response_length=0;
    for(uint16_t i=0;host[i];i++)if(i>=253||host[i]=='\r'||host[i]=='\n'||host[i]==' '||host[i]=='/')return 0;
    for(uint16_t i=0;path[i];i++)if(path[i]=='\r'||path[i]=='\n')return 0;
    if(!net_dns_resolve(host,&address))return 0;
    if(!net_tcp_connect(address,443))return 0;

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
    if(!append_text(request,sizeof request,&request_length,"GET ")||
       !append_text(request,sizeof request,&request_length,path[0]?path:"/")||
       !append_text(request,sizeof request,&request_length," HTTP/1.0\r\nHost: ")||
       !append_text(request,sizeof request,&request_length,host)||
       !append_text(request,sizeof request,&request_length,"\r\nConnection: close\r\nUser-Agent: maiOs/0.1\r\n\r\n")||
       br_sslio_write_all(&tls_io,request,request_length)<0||br_sslio_flush(&tls_io)<0){
        (void)net_tcp_close();return 0;
    }
    while(*response_length+1<capacity){
        size_t remaining=capacity-*response_length-1;
        int count=br_sslio_read(&tls_io,response+*response_length,remaining);
        if(count<=0)break;
        *response_length=(uint16_t)(*response_length+count);
    }
    response[*response_length]=0;
    (void)net_tcp_close();
    return *response_length!=0;
}
