#include "net.h"
#include "e1000.h"

#define ETH_FRAME_MAX 1518u
#define IPV4_HEADER_MIN 20u
#define IPV4_PAYLOAD_MAX (1500u - IPV4_HEADER_MIN)
#define TCP_MSS 1000u
#define ARP_CACHE_SIZE 8u
#define IPV4_QUEUE_SIZE 8u
#define ETH_TYPE_ARP 0x0806u
#define ETH_TYPE_IPV4 0x0800u

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
    uint8_t valid;
} arp_entry_t;

typedef struct {
    uint32_t source;
    uint32_t destination;
    uint16_t length;
    uint8_t protocol;
    uint8_t data[IPV4_PAYLOAD_MAX];
} ipv4_queue_entry_t;

static uint8_t local_mac[6];
static uint32_t local_ip = 0x0A00020Fu; /* QEMU user networking: 10.0.2.15 */
static uint32_t subnet_mask = 0xFFFFFF00u;
static uint32_t gateway_ip = 0x0A000202u; /* 10.0.2.2 */
static uint32_t dns_ip = 0x0A000203u; /* 10.0.2.3 */
static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static uint8_t arp_replace;
static uint16_t ipv4_id;
static ipv4_queue_entry_t ipv4_queue[IPV4_QUEUE_SIZE];
static uint8_t queue_head, queue_tail, queue_count;
static uint16_t icmp_sequence;
static uint16_t dns_id, udp_port_seed;
static struct {
    uint32_t address;
    uint32_t send_sequence;
    uint32_t receive_sequence;
    uint16_t local_port;
    uint16_t remote_port;
    uint16_t pending_length;
    uint8_t connected;
    uint8_t pending_fin;
    uint8_t pending[TCP_MSS*4];
} tcp_state;
static uint16_t tcp_port_seed=49152;
static uint32_t tcp_sequence_seed=0x4D41494Fu;

static uint16_t be16(const uint8_t *p) { return (uint16_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static void put_be16(uint8_t *p,uint16_t n) { p[0]=n>>8;p[1]=n; }
static void put_be32(uint8_t *p,uint32_t n) { p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n; }
static void bytes_copy(uint8_t *d,const uint8_t *s,uint16_t n) { for(uint16_t i=0;i<n;i++)d[i]=s[i]; }

static uint16_t checksum(const uint8_t *data,uint16_t length)
{
    uint32_t sum=0;
    for(uint16_t i=0;i+1<length;i+=2)sum+=be16(data+i);
    if(length&1)sum+=(uint16_t)data[length-1]<<8;
    while(sum>>16)sum=(sum&0xffff)+(sum>>16);
    return (uint16_t)~sum;
}

static uint16_t udp_checksum(uint32_t source,uint32_t destination,const uint8_t *udp,uint16_t length)
{
    uint32_t sum=0;
    uint8_t pseudo[12];
    put_be32(pseudo,source);put_be32(pseudo+4,destination);
    pseudo[8]=0;pseudo[9]=17;put_be16(pseudo+10,length);
    for(uint8_t i=0;i<12;i+=2)sum+=be16(pseudo+i);
    for(uint16_t i=0;i+1<length;i+=2)sum+=be16(udp+i);
    if(length&1)sum+=(uint16_t)udp[length-1]<<8;
    while(sum>>16)sum=(sum&0xffff)+(sum>>16);
    return (uint16_t)~sum;
}

static int send_ethernet(const uint8_t destination[6],uint16_t type,const uint8_t *data,uint16_t length)
{
    static uint8_t frame[ETH_FRAME_MAX];
    if(length>ETH_FRAME_MAX-14)return 0;
    bytes_copy(frame,destination,6);bytes_copy(frame+6,local_mac,6);put_be16(frame+12,type);
    bytes_copy(frame+14,data,length);
    return e1000_send(frame,(uint16_t)(length+14))==1;
}

static void arp_learn(uint32_t ip,const uint8_t mac[6])
{
    if(!ip||ip==0xFFFFFFFFu||mac[0]&1)return;
    for(uint8_t i=0;i<ARP_CACHE_SIZE;i++)if(arp_cache[i].valid&&arp_cache[i].ip==ip){bytes_copy(arp_cache[i].mac,mac,6);return;}
    arp_entry_t *entry=&arp_cache[arp_replace++%ARP_CACHE_SIZE];entry->ip=ip;bytes_copy(entry->mac,mac,6);entry->valid=1;
}

static int arp_lookup(uint32_t ip,uint8_t mac[6])
{
    for(uint8_t i=0;i<ARP_CACHE_SIZE;i++)if(arp_cache[i].valid&&arp_cache[i].ip==ip){bytes_copy(mac,arp_cache[i].mac,6);return 1;}
    return 0;
}

static int arp_request(uint32_t target_ip)
{
    static const uint8_t broadcast[6]={0xff,0xff,0xff,0xff,0xff,0xff};
    uint8_t arp[28]={0};
    put_be16(arp,1);put_be16(arp+2,ETH_TYPE_IPV4);arp[4]=6;arp[5]=4;put_be16(arp+6,1);
    bytes_copy(arp+8,local_mac,6);put_be32(arp+14,local_ip);put_be32(arp+24,target_ip);
    return send_ethernet(broadcast,ETH_TYPE_ARP,arp,sizeof(arp));
}

static int arp_resolve(uint32_t ip,uint8_t mac[6])
{
    if(ip==local_ip){bytes_copy(mac,local_mac,6);return 1;}
    if(arp_lookup(ip,mac))return 1;
    for(uint8_t attempt=0;attempt<3;attempt++){
        if(!arp_request(ip))return 0;
        for(uint32_t wait=0;wait<1000000;wait++){
            net_poll();
            if(arp_lookup(ip,mac))return 1;
        }
    }
    return 0;
}

static void handle_arp(const uint8_t *frame,uint16_t length)
{
    if(length<42)return;
    const uint8_t *arp=frame+14;
    if(be16(arp)!=1||be16(arp+2)!=ETH_TYPE_IPV4||arp[4]!=6||arp[5]!=4)return;
    uint16_t operation=be16(arp+6);
    if(operation!=1&&operation!=2)return;
    uint32_t sender_ip=be32(arp+14),target_ip=be32(arp+24);
    const uint8_t *sender_mac=arp+8;
    arp_learn(sender_ip,sender_mac);
    if(operation==1&&target_ip==local_ip){
        uint8_t reply[28]={0};
        put_be16(reply,1);put_be16(reply+2,ETH_TYPE_IPV4);reply[4]=6;reply[5]=4;put_be16(reply+6,2);
        bytes_copy(reply+8,local_mac,6);put_be32(reply+14,local_ip);
        bytes_copy(reply+18,sender_mac,6);put_be32(reply+24,sender_ip);
        (void)send_ethernet(sender_mac,ETH_TYPE_ARP,reply,sizeof(reply));
    }
}

static void handle_ipv4(const uint8_t *frame,uint16_t length)
{
    if(length<14+IPV4_HEADER_MIN)return;
    const uint8_t *ip=frame+14;uint16_t available=length-14;
    uint8_t version=ip[0]>>4,ihl=(ip[0]&0x0f)*4;
    if(version!=4||ihl<IPV4_HEADER_MIN||ihl>60||available<ihl)return;
    uint16_t total=be16(ip+2);
    if(total<ihl||total>available||checksum(ip,ihl)!=0)return;
    uint16_t fragment=be16(ip+6);
    if(fragment&0xbfffu)return; /* Fragment reassembly is not implemented. */
    uint32_t destination=be32(ip+16);
    int dhcp_unicast=ip[9]==17&&total>=ihl+8&&be16(ip+ihl+2)==68;
    if(destination!=local_ip&&destination!=0xFFFFFFFFu&&destination!=(local_ip|~subnet_mask)&&!dhcp_unicast)return;
    uint16_t payload_length=total-ihl;
    if(payload_length>IPV4_PAYLOAD_MAX||queue_count==IPV4_QUEUE_SIZE)return;
    ipv4_queue_entry_t *entry=&ipv4_queue[queue_tail];
    entry->source=be32(ip+12);entry->destination=destination;entry->protocol=ip[9];entry->length=payload_length;
    bytes_copy(entry->data,ip+ihl,payload_length);
    queue_tail=(uint8_t)((queue_tail+1)%IPV4_QUEUE_SIZE);queue_count++;
}

void net_init(const uint8_t mac[6])
{
    bytes_copy(local_mac,mac,6);
    for(uint8_t i=0;i<ARP_CACHE_SIZE;i++)arp_cache[i].valid=0;
    queue_head=queue_tail=queue_count=arp_replace=0;ipv4_id=0;icmp_sequence=0;
    dns_id=0;udp_port_seed=53000;tcp_port_seed=49152;tcp_sequence_seed=0x4D41494Fu;
    tcp_state.connected=0;
    local_ip=0x0A00020F;subnet_mask=0xFFFFFF00;gateway_ip=0x0A000202;dns_ip=0x0A000203;
}

void net_poll(void)
{
    uint8_t frame[ETH_FRAME_MAX];
    for(uint8_t i=0;i<8;i++){
        int length=e1000_receive(frame,sizeof(frame));
        if(length==0)return;
        if(length<0)continue;
        uint16_t type=be16(frame+12);
        if(type==ETH_TYPE_ARP)handle_arp(frame,(uint16_t)length);
        else if(type==ETH_TYPE_IPV4)handle_ipv4(frame,(uint16_t)length);
    }
}

static int ipv4_send_from(uint32_t source,uint32_t destination,uint8_t protocol,const uint8_t *payload,uint16_t length)
{
    if((length&&!payload)||length>IPV4_PAYLOAD_MAX)return 0;
    uint8_t target_mac[6];
    if(destination==0xFFFFFFFFu||destination==(local_ip|~subnet_mask)){
        for(uint8_t i=0;i<6;i++)target_mac[i]=0xFF;
    } else {
        uint32_t next_hop=((destination&subnet_mask)==(local_ip&subnet_mask))?destination:gateway_ip;
        if(!arp_resolve(next_hop,target_mac))return 0;
    }
    uint8_t packet[IPV4_HEADER_MIN+IPV4_PAYLOAD_MAX]={0};
    packet[0]=0x45;put_be16(packet+2,IPV4_HEADER_MIN+length);put_be16(packet+4,++ipv4_id);
    put_be16(packet+6,0x4000);packet[8]=64;packet[9]=protocol;
    put_be32(packet+12,source);put_be32(packet+16,destination);
    put_be16(packet+10,checksum(packet,IPV4_HEADER_MIN));
    if(length)bytes_copy(packet+IPV4_HEADER_MIN,payload,length);
    return send_ethernet(target_mac,ETH_TYPE_IPV4,packet,IPV4_HEADER_MIN+length);
}

int net_ipv4_send(uint32_t destination,uint8_t protocol,const uint8_t *payload,uint16_t length)
{
    return ipv4_send_from(local_ip,destination,protocol,payload,length);
}

int net_udp_send(uint32_t source,uint32_t destination,uint16_t source_port,uint16_t destination_port,const uint8_t *payload,uint16_t length)
{
    uint8_t datagram[1480];
    if(length>sizeof(datagram)-8||(length&&!payload))return 0;
    put_be16(datagram,source_port);put_be16(datagram+2,destination_port);
    put_be16(datagram+4,length+8);put_be16(datagram+6,0);
    if(length)bytes_copy(datagram+8,payload,length);
    if(source){uint16_t sum=udp_checksum(source,destination,datagram,length+8);put_be16(datagram+6,sum?sum:0xFFFF);}
    return ipv4_send_from(source,destination,17,datagram,length+8);
}

int net_ipv4_receive(ipv4_packet_t *packet,uint8_t *payload,uint16_t capacity)
{
    if(!packet||!payload||!queue_count)return 0;
    ipv4_queue_entry_t *entry=&ipv4_queue[queue_head];
    if(entry->length>capacity)return -1;
    packet->source=entry->source;packet->destination=entry->destination;packet->protocol=entry->protocol;packet->length=entry->length;
    bytes_copy(payload,entry->data,entry->length);
    queue_head=(uint8_t)((queue_head+1)%IPV4_QUEUE_SIZE);queue_count--;
    return 1;
}

int net_udp_receive(udp_packet_t *packet,uint8_t *payload,uint16_t capacity)
{
    uint8_t data[IPV4_PAYLOAD_MAX];ipv4_packet_t ip;
    if(!packet||!payload)return -1;
    while(net_ipv4_receive(&ip,data,sizeof(data))==1){
        if(ip.protocol!=17||ip.length<8)continue;
        uint16_t length=be16(data+4),checksum_value=be16(data+6);
        if(length<8||length>ip.length)continue;
        if(checksum_value&&udp_checksum(ip.source,ip.destination,data,length)!=0)continue;
        uint16_t payload_length=length-8;
        if(payload_length>capacity)return -1;
        packet->source=ip.source;packet->source_port=be16(data);packet->destination_port=be16(data+2);packet->length=payload_length;
        if(payload_length)bytes_copy(payload,data+8,payload_length);
        return 1;
    }
    return 0;
}

int net_ping(uint32_t destination)
{
    uint8_t request[40]={0};
    static uint8_t reply[IPV4_PAYLOAD_MAX];
    ipv4_packet_t packet;
    uint16_t sequence=++icmp_sequence;
    request[0]=8; /* ICMP Echo Request */
    put_be16(request+4,0x4D41);put_be16(request+6,sequence);
    for(uint8_t i=8;i<sizeof(request);i++)request[i]=(uint8_t)(i^sequence);
    put_be16(request+2,checksum(request,sizeof(request)));
    if(!net_ipv4_send(destination,1,request,sizeof(request)))return -1;

    for(uint32_t wait=0;wait<3000000;wait++){
        net_poll();
        while(net_ipv4_receive(&packet,reply,sizeof(reply))==1){
            if(packet.source!=destination||packet.protocol!=1||packet.length!=sizeof(request))continue;
            if(checksum(reply,packet.length)!=0||reply[0]!=0||reply[1]!=0)continue;
            if(be16(reply+4)!=0x4D41||be16(reply+6)!=sequence)continue;
            return 1;
        }
    }
    return 0;
}

#define DHCP_PORT_CLIENT 68u
#define DHCP_PORT_SERVER 67u
#define DHCP_OPTIONS_OFFSET 240u
#define DHCP_BUFFER_SIZE 576u

static int bytes_equal(const uint8_t *a,const uint8_t *b,uint8_t length)
{
    uint8_t diff=0;
    for(uint8_t i=0;i<length;i++)diff|=a[i]^b[i];
    return diff==0;
}

static int dhcp_option(const uint8_t *message,uint16_t length,uint8_t wanted,uint8_t *value,uint8_t *value_length)
{
    if(length<DHCP_OPTIONS_OFFSET||be32(message+236)!=0x63825363u)return 0;
    uint16_t offset=DHCP_OPTIONS_OFFSET;
    while(offset<length){
        uint8_t code=message[offset++];
        if(code==255)break;
        if(code==0)continue;
        if(offset>=length)return 0;
        uint8_t size=message[offset++];
        if((uint32_t)offset+size>length)return 0;
        if(code==wanted){
            if(size>*value_length)return 0;
            bytes_copy(value,message+offset,size);*value_length=size;return 1;
        }
        offset+=size;
    }
    return 0;
}

static uint16_t dhcp_build(uint8_t *message,uint8_t type,uint32_t xid,uint32_t requested,uint32_t server)
{
    for(uint16_t i=0;i<DHCP_BUFFER_SIZE;i++)message[i]=0;
    message[0]=1;message[1]=1;message[2]=6;put_be32(message+4,xid);
    put_be16(message+10,0x8000);bytes_copy(message+28,local_mac,6);
    put_be32(message+236,0x63825363u);
    uint16_t p=DHCP_OPTIONS_OFFSET;
    message[p++]=53;message[p++]=1;message[p++]=type;
    if(type==3){
        message[p++]=50;message[p++]=4;put_be32(message+p,requested);p+=4;
        message[p++]=54;message[p++]=4;put_be32(message+p,server);p+=4;
    }
    message[p++]=55;message[p++]=4;message[p++]=1;message[p++]=3;message[p++]=6;message[p++]=51;
    message[p++]=12;message[p++]=5;bytes_copy(message+p,(const uint8_t*)"maiOs",5);p+=5;
    message[p++]=255;
    if(p<300)p=300; /* BOOTP/DHCP clients commonly send at least 300 bytes. */
    return p;
}

static int dhcp_wait(uint32_t xid,uint8_t expected,uint8_t *message,uint16_t *message_length,uint32_t *offered_ip,uint32_t *server_id)
{
    uint8_t payload[DHCP_BUFFER_SIZE],option[32];
    udp_packet_t packet;
    for(uint32_t wait=0;wait<2000000;wait++){
        net_poll();
        int received;
        while((received=net_udp_receive(&packet,payload,sizeof(payload)))==1){
            if(packet.source_port!=DHCP_PORT_SERVER||packet.destination_port!=DHCP_PORT_CLIENT||packet.length<DHCP_OPTIONS_OFFSET)continue;
            if(payload[0]!=2||payload[1]!=1||payload[2]!=6||be32(payload+4)!=xid||!bytes_equal(payload+28,local_mac,6))continue;
            uint8_t option_length=sizeof(option);
            if(!dhcp_option(payload,packet.length,53,option,&option_length)||option_length!=1)continue;
            if(option[0]==6)return -1;
            if(option[0]!=expected)continue;
            *offered_ip=be32(payload+16);
            option_length=4;
            if(!dhcp_option(payload,packet.length,54,option,&option_length)||option_length!=4)*server_id=packet.source;
            else *server_id=be32(option);
            bytes_copy(message,payload,packet.length);*message_length=packet.length;
            return 1;
        }
        if(received<0)continue;
    }
    return 0;
}

int net_dhcp_configure(void)
{
    uint8_t message[DHCP_BUFFER_SIZE],reply[DHCP_BUFFER_SIZE];uint16_t reply_length;
    uint32_t xid=0x4D41494Fu,offered_ip=0,server_id=0;
    local_ip=0x0A00020F;subnet_mask=0xFFFFFF00;gateway_ip=0x0A000202;dns_ip=0x0A000203;
    uint16_t length=dhcp_build(message,1,xid,0,0);
    if(!net_udp_send(0,0xFFFFFFFFu,DHCP_PORT_CLIENT,DHCP_PORT_SERVER,message,length))return 0;
    int result=dhcp_wait(xid,2,reply,&reply_length,&offered_ip,&server_id);
    if(result!=1||!offered_ip)return 0;
    length=dhcp_build(message,3,xid,offered_ip,server_id);
    if(!net_udp_send(0,0xFFFFFFFFu,DHCP_PORT_CLIENT,DHCP_PORT_SERVER,message,length))return 0;
    result=dhcp_wait(xid,5,reply,&reply_length,&offered_ip,&server_id);
    if(result!=1||!offered_ip)return 0;

    uint8_t option[32],option_length=sizeof(option);
    local_ip=offered_ip;
    option_length=sizeof(option);
    if(dhcp_option(reply,reply_length,1,option,&option_length)&&option_length==4)subnet_mask=be32(option);
    option_length=sizeof(option);
    if(dhcp_option(reply,reply_length,3,option,&option_length)&&option_length>=4)gateway_ip=be32(option);
    option_length=sizeof(option);
    if(dhcp_option(reply,reply_length,6,option,&option_length)&&option_length>=4)dns_ip=be32(option);
    return 1;
}

static int dns_skip_name(const uint8_t *message,uint16_t length,uint16_t *offset)
{
    uint16_t pos=*offset;
    for(uint16_t steps=0;steps<128;steps++){
        if(pos>=length)return 0;
        uint8_t label=message[pos];
        if((label&0xC0)==0xC0){if(pos+1>=length)return 0;*offset=pos+2;return 1;}
        if(label&0xC0)return 0;
        pos++;
        if(label==0){*offset=pos;return 1;}
        if((uint32_t)pos+label>length)return 0;
        pos+=label;
    }
    return 0;
}

int net_dns_resolve(const char *name,uint32_t *address)
{
    uint8_t query[512]={0},response[IPV4_PAYLOAD_MAX];udp_packet_t packet;
    if(!name||!address)return 0;
    uint16_t id=++dns_id;put_be16(query,id);put_be16(query+2,0x0100);put_be16(query+4,1);
    uint16_t pos=12;const char *label=name;
    while(*label){
        const char *end=label;uint8_t label_length=0;
        while(*end&&*end!='.'){if(label_length==63)return 0;label_length++;end++;}
        if(!label_length||(uint32_t)pos+1u+label_length+5u>sizeof(query))return 0;
        query[pos++]=label_length;
        while(label<end)query[pos++]=(uint8_t)*label++;
        if(*label=='.')label++;
    }
    query[pos++]=0;put_be16(query+pos,1);pos+=2;put_be16(query+pos,1);pos+=2;
    uint16_t source_port=udp_port_seed++;if(udp_port_seed<53000)udp_port_seed=53000;
    if(!net_udp_send(local_ip,dns_ip,source_port,53,query,pos))return 0;

    for(uint32_t wait=0;wait<3000000;wait++){
        net_poll();int received;
        while((received=net_udp_receive(&packet,response,sizeof(response)))==1){
            if(packet.source!=dns_ip||packet.source_port!=53||packet.destination_port!=source_port||packet.length<12)continue;
            uint16_t flags=be16(response+2);
            if(be16(response)!=id||!(flags&0x8000)||(flags&0x0200)||(flags&0x000F))continue;
            uint16_t questions=be16(response+4),answers=be16(response+6),offset=12;
            int valid=1;
            for(uint16_t i=0;i<questions;i++){
                if(!dns_skip_name(response,packet.length,&offset)||offset+4>packet.length){valid=0;break;}
                offset+=4;
            }
            if(!valid)continue;
            for(uint16_t i=0;i<answers;i++){
                if(!dns_skip_name(response,packet.length,&offset)||offset+10>packet.length){valid=0;break;}
                uint16_t type=be16(response+offset),class_code=be16(response+offset+2),rdlength=be16(response+offset+8);
                offset+=10;if((uint32_t)offset+rdlength>packet.length){valid=0;break;}
                if(type==1&&class_code==1&&rdlength==4){*address=be32(response+offset);return 1;}
                offset+=rdlength;
            }
            if(!valid)return 0;
        }
    }
    return 0;
}

void net_get_config(uint32_t *address,uint32_t *mask,uint32_t *gateway)
{
    if(address)*address=local_ip;
    if(mask)*mask=subnet_mask;
    if(gateway)*gateway=gateway_ip;
}

uint32_t net_get_dns(void){return dns_ip;}

#define TCP_HEADER_SIZE 20u

static uint16_t tcp_checksum(uint32_t source,uint32_t destination,const uint8_t *segment,uint16_t length)
{
    uint32_t sum=0;
    uint8_t pseudo[12];
    put_be32(pseudo,source);put_be32(pseudo+4,destination);
    pseudo[8]=0;pseudo[9]=6;put_be16(pseudo+10,length);
    for(uint8_t i=0;i<12;i+=2)sum+=be16(pseudo+i);
    for(uint16_t i=0;i+1<length;i+=2)sum+=be16(segment+i);
    if(length&1)sum+=(uint16_t)segment[length-1]<<8;
    while(sum>>16)sum=(sum&0xffff)+(sum>>16);
    return (uint16_t)~sum;
}

static int tcp_send_segment(uint8_t flags,const uint8_t *payload,uint16_t length)
{
    uint8_t segment[TCP_HEADER_SIZE+TCP_MSS]={0};
    if(!tcp_state.address||length>TCP_MSS||(length&&!payload))return 0;
    put_be16(segment,tcp_state.local_port);put_be16(segment+2,tcp_state.remote_port);
    put_be32(segment+4,tcp_state.send_sequence);put_be32(segment+8,tcp_state.receive_sequence);
    segment[12]=0x50;segment[13]=flags;put_be16(segment+14,4096);
    if(length)bytes_copy(segment+TCP_HEADER_SIZE,payload,length);
    uint16_t sum=tcp_checksum(local_ip,tcp_state.address,segment,TCP_HEADER_SIZE+length);
    put_be16(segment+16,sum?sum:0xFFFF);
    return net_ipv4_send(tcp_state.address,6,segment,TCP_HEADER_SIZE+length);
}

/* Returns 1 for a matching valid TCP segment, 0 while no match is queued. */
static int tcp_next_segment(uint8_t *flags,uint32_t *sequence,uint32_t *acknowledgment,
                            uint8_t *payload,uint16_t capacity,uint16_t *length)
{
    uint8_t data[IPV4_PAYLOAD_MAX];ipv4_packet_t ip;
    while(net_ipv4_receive(&ip,data,sizeof(data))==1){
        if(ip.protocol!=6||ip.source!=tcp_state.address||ip.length<TCP_HEADER_SIZE)continue;
        uint16_t header=(uint16_t)(data[12]>>4)*4;
        if(header<TCP_HEADER_SIZE||header>ip.length)continue;
        if(be16(data)!=tcp_state.remote_port||be16(data+2)!=tcp_state.local_port)continue;
        if(tcp_checksum(ip.source,ip.destination,data,ip.length)!=0)continue;
        uint16_t payload_length=ip.length-header;
        if(payload_length>capacity)return -1;
        *flags=data[13];*sequence=be32(data+4);*acknowledgment=be32(data+8);*length=payload_length;
        if(payload_length)bytes_copy(payload,data+header,payload_length);
        return 1;
    }
    return 0;
}

static int tcp_store_received(uint8_t flags,uint32_t sequence,const uint8_t *payload,uint16_t length)
{
    if(sequence!=tcp_state.receive_sequence){
        (void)tcp_send_segment(0x10,0,0);
        return 0;
    }
    if(length>sizeof(tcp_state.pending)-tcp_state.pending_length)return 0;
    if(length){
        for(uint16_t i=0;i<length;i++)tcp_state.pending[tcp_state.pending_length+i]=payload[i];
        tcp_state.pending_length+=length;tcp_state.receive_sequence+=length;
    }
    if(flags&0x01){tcp_state.pending_fin=1;tcp_state.receive_sequence++;}
    if(length||(flags&0x01))(void)tcp_send_segment(0x10,0,0);
    return 1;
}

static int tcp_take_pending(uint8_t *data,uint16_t capacity,uint16_t *length)
{
    if(tcp_state.pending_length){
        uint16_t count=tcp_state.pending_length<capacity?tcp_state.pending_length:capacity;
        for(uint16_t i=0;i<count;i++)data[i]=tcp_state.pending[i];
        for(uint16_t i=count;i<tcp_state.pending_length;i++)tcp_state.pending[i-count]=tcp_state.pending[i];
        tcp_state.pending_length-=count;*length=count;return 1;
    }
    if(tcp_state.pending_fin){tcp_state.pending_fin=0;*length=0;return 2;}
    return 0;
}

static int tcp_wait_ack(uint32_t expected)
{
    uint8_t payload[TCP_MSS],flags;uint16_t length;uint32_t sequence,acknowledgment;
    for(uint32_t wait=0;wait<3000000;wait++){
        net_poll();
        while(tcp_next_segment(&flags,&sequence,&acknowledgment,payload,sizeof(payload),&length)==1){
            if(flags&0x04)return -1;
            if((flags&0x08)&&acknowledgment==expected){
                if(length||(flags&0x01)){
                    if(!tcp_store_received(flags,sequence,payload,length))return -1;
                }
                return 1;
            }
            if(flags&0x01){tcp_state.receive_sequence=sequence+length+1;(void)tcp_send_segment(0x10,0,0);return -1;}
        }
    }
    return 0;
}

int net_tcp_connect(uint32_t destination,uint16_t port)
{
    if(!destination||!port)return 0;
    tcp_state.address=destination;tcp_state.remote_port=port;
    tcp_state.local_port=tcp_port_seed++;if(tcp_port_seed<49152)tcp_port_seed=49152;
    tcp_state.send_sequence=++tcp_sequence_seed;tcp_state.connected=0;
    tcp_state.pending_length=0;tcp_state.pending_fin=0;
    if(!tcp_send_segment(0x02,0,0))return 0;
    tcp_state.send_sequence++;
    uint8_t payload[TCP_MSS],flags;uint16_t length;uint32_t sequence,acknowledgment;
    for(uint32_t wait=0;wait<3000000;wait++){
        net_poll();
        while(tcp_next_segment(&flags,&sequence,&acknowledgment,payload,sizeof(payload),&length)==1){
            if(flags&0x04)return 0;
            if((flags&0x12)==0x12&&acknowledgment==tcp_state.send_sequence){
                tcp_state.receive_sequence=sequence+1;
                if((length||(flags&0x01))&&!tcp_store_received(flags,sequence+1,payload,length))return 0;
                if(!length&&!(flags&0x01)&&!tcp_send_segment(0x10,0,0))return 0;
                tcp_state.connected=1;return 1;
            }
        }
    }
    return 0;
}

int net_tcp_send(const uint8_t *data,uint16_t length)
{
    if(!tcp_state.connected||!data||!length||length>TCP_MSS)return 0;
    if(!tcp_send_segment(0x18,data,length))return 0; /* PSH + ACK */
    tcp_state.send_sequence+=length;
    return tcp_wait_ack(tcp_state.send_sequence);
}

int net_tcp_receive(uint8_t *data,uint16_t capacity,uint16_t *length)
{
    if(!tcp_state.connected||!data||!capacity||!length)return -1;
    int pending=tcp_take_pending(data,capacity,length);
    if(pending)return pending==1?1:0;
    uint8_t flags,payload[TCP_MSS];uint16_t received;uint32_t sequence,acknowledgment;
    for(uint32_t wait=0;wait<3000000;wait++){
        net_poll();
        while(tcp_next_segment(&flags,&sequence,&acknowledgment,payload,
                               capacity<TCP_MSS?capacity:TCP_MSS,&received)==1){
            if(flags&0x04)return -1;
            if((flags&0x10)&&sequence==tcp_state.receive_sequence&&(received||(flags&0x01)))
                (void)tcp_store_received(flags,sequence,payload,received);
            pending=tcp_take_pending(data,capacity,length);
            if(pending)return pending==1?1:0;
        }
    }
    return 0;
}

int net_tcp_close(void)
{
    if(!tcp_state.connected)return 0;
    if(!tcp_send_segment(0x11,0,0)){tcp_state.connected=0;return 0;}
    tcp_state.send_sequence++;int result=tcp_wait_ack(tcp_state.send_sequence);
    tcp_state.connected=0;return result==1;
}
