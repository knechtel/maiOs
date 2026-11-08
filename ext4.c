#include "ext4.h"

#define SECTOR_SIZE 512u
#define BLOCK_SIZE 1024u
#define MAX_BLOCKS 8192u
#define ROOT_INODE 2u

static uint8_t mounted;
static uint8_t super[BLOCK_SIZE];
static uint8_t group[BLOCK_SIZE];
static uint32_t blocks_count, inodes_count, blocks_per_group, inodes_per_group;
static uint32_t inode_size, block_bitmap, inode_bitmap, inode_table;

static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outb(uint16_t p,uint8_t v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outw(uint16_t p,uint16_t v) { __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static uint16_t rd16(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (uint32_t)rd16(p+2)<<16; }
static void wr16(uint8_t *p,uint16_t v) { p[0]=v; p[1]=v>>8; }
static void wr32(uint8_t *p,uint32_t v) { wr16(p,v); wr16(p+2,v>>16); }
static void zero(uint8_t *p,size_t n) { for(size_t i=0;i<n;i++) p[i]=0; }
static void copy(uint8_t *d,const uint8_t *s,size_t n) { for(size_t i=0;i<n;i++) d[i]=s[i]; }

static int wait_ata(int data) {
    for(uint32_t i=0;i<1000000;i++){ uint8_t s=inb(0x1f7); if(s&1)return 0; if(!(s&0x80)&&(!data||(s&8)))return 1; }
    return 0;
}
static int sector_io(uint32_t lba,uint8_t *buf,int write) {
    if(lba>=0x10000000||!wait_ata(0))return 0;
    outb(0x1f6,0xe0|((lba>>24)&15)); outb(0x1f2,1); outb(0x1f3,lba); outb(0x1f4,lba>>8); outb(0x1f5,lba>>16); outb(0x1f7,write?0x30:0x20);
    if(!wait_ata(1))return 0;
    for(size_t i=0;i<256;i++) if(write) outw(0x1f0,buf[i*2]|(uint16_t)buf[i*2+1]<<8); else {uint16_t w=inw(0x1f0);buf[i*2]=w;buf[i*2+1]=w>>8;}
    if(write){outb(0x1f7,0xe7);return wait_ata(0);} return 1;
}
static int block_read(uint32_t b,uint8_t *p){return b<blocks_count&&sector_io(b*2,p,0)&&sector_io(b*2+1,p+512,0);}
static int block_write(uint32_t b,const uint8_t *p){return b<blocks_count&&sector_io(b*2,(uint8_t*)p,1)&&sector_io(b*2+1,(uint8_t*)p+512,1);}
static int inode_read(uint32_t ino,uint8_t *out){
    if(!ino||ino>inodes_count)return 0;
    uint32_t off=(ino-1)*inode_size, b=inode_table+off/BLOCK_SIZE, pos=off%BLOCK_SIZE;
    uint8_t tmp[BLOCK_SIZE]; if(!block_read(b,tmp))return 0; copy(out,tmp+pos,128); return 1;
}
static int inode_write(uint32_t ino,const uint8_t *in){
    if(!ino||ino>inodes_count)return 0;
    uint32_t off=(ino-1)*inode_size, b=inode_table+off/BLOCK_SIZE, pos=off%BLOCK_SIZE;
    uint8_t tmp[BLOCK_SIZE]; if(!block_read(b,tmp))return 0; copy(tmp+pos,in,128); return block_write(b,tmp);
}
static int alloc_bit(uint32_t bitmap_block,uint32_t limit,uint32_t *value){
    uint8_t map[BLOCK_SIZE]; if(!block_read(bitmap_block,map))return 0;
    for(uint32_t i=0;i<limit;i++) {
        if(!(map[i>>3]&(1u<<(i&7)))) {
            map[i>>3]|=1u<<(i&7);
            if(!block_write(bitmap_block,map))return 0;
            *value=i;
            return 1;
        }
    }
    return 0;
}
static int allocate_inode(uint32_t *ino,int dir){
    uint32_t bit;if(!alloc_bit(inode_bitmap,inodes_per_group,&bit))return 0;
    *ino=bit+1; uint16_t freei=rd16(group+14); if(!freei)return 0; wr16(group+14,freei-1);
    uint32_t sf=rd32(super+16); if(sf)wr32(super+16,sf-1);
    if(dir)wr16(group+16,rd16(group+16)+1);
    return block_write(2,group)&&block_write(1,super);
}
static int allocate_block(uint32_t *block){
    uint32_t bit;if(!alloc_bit(block_bitmap,blocks_per_group,&bit))return 0;
    *block=bit+1; uint16_t freeb=rd16(group+12); if(!freeb)return 0; wr16(group+12,freeb-1);
    uint32_t sf=rd32(super+12); if(sf)wr32(super+12,sf-1);
    return block_write(2,group)&&block_write(1,super);
}

int ext4_mount(void){
    uint8_t sec[512]; mounted=0;
    if(!sector_io(2,sec,0))return 0;
    copy(super,sec,512);
    if(!sector_io(3,sec,0))return 0;
    copy(super+512,sec,512);
    if(rd16(super+56)!=0xef53||rd32(super+24)!=0||rd16(super+88)<128)return 0;
    blocks_count=rd32(super+4);inodes_count=rd32(super);blocks_per_group=rd32(super+32);inodes_per_group=rd32(super+40);inode_size=rd16(super+88);
    uint32_t compat=rd32(super+92), incompat=rd32(super+96), ro=rd32(super+100);
    /* Single-group, 1 KiB ext2-layout subset; ext4 can mount this feature set. */
    if(!blocks_count||blocks_count>MAX_BLOCKS||blocks_count>blocks_per_group||inodes_count>inodes_per_group||!blocks_per_group||!inodes_per_group)return 0;
    if((compat&~0x59u)||(incompat&~0x02u)||(ro&~0x6bu))return 0;
    if(!block_read(2,group))return 0;
    block_bitmap=rd32(group);inode_bitmap=rd32(group+4);inode_table=rd32(group+8);
    if(block_bitmap>=blocks_count||inode_bitmap>=blocks_count||inode_table>=blocks_count)return 0;
    mounted=1;return 1;
}

static int root_block(uint8_t *data,uint8_t *root){return inode_read(ROOT_INODE,root)&&rd32(root+40)&&block_read(rd32(root+40),data);}
static int entry_find(const char *name,uint32_t *ino,uint8_t *type){
    uint8_t data[BLOCK_SIZE],root[128];if(!root_block(data,root))return 0;
    for(uint32_t off=0;off+8<=BLOCK_SIZE;){uint32_t n=rd32(data+off);uint16_t rec=rd16(data+off+4);uint8_t len=data[off+6];if(rec<8||off+rec>BLOCK_SIZE)return 0;
        size_t k=0;while(name[k]&&k<len&&name[k]==(char)data[off+8+k])k++;
        if(name[k]==0&&k==len&&n){*ino=n;*type=data[off+7];return 1;}off+=rec;}
    return 0;
}
static int root_add(const char *name,uint32_t ino,uint8_t type){
    uint8_t data[BLOCK_SIZE],root[128];if(!root_block(data,root))return 0;
    size_t len=0;while(name[len])len++;if(!len||len>255)return 0;
    for(uint32_t off=0;off+8<=BLOCK_SIZE;){uint16_t rec=rd16(data+off+4);uint8_t old=data[off+6];if(rec<8||off+rec>BLOCK_SIZE)return 0;
        uint16_t need=(uint16_t)((8+old+3)&~3);if(rec>=need+8+len){wr16(data+off+4,need);uint32_t at=off+need;wr32(data+at,ino);wr16(data+at+4,rec-need);data[at+6]=len;data[at+7]=type;copy(data+at+8,(const uint8_t*)name,len);wr32(root+4,rd32(root+4));return block_write(rd32(root+40),data);}off+=rec;}
    return 0;
}
int ext4_list(ext4_entry_t *entries,size_t capacity){
    uint8_t data[BLOCK_SIZE],root[128];if(!mounted||!root_block(data,root))return -1;int count=0;
    for(uint32_t off=0;off+8<=BLOCK_SIZE;){uint32_t ino=rd32(data+off);uint16_t rec=rd16(data+off+4);uint8_t len=data[off+6];if(rec<8||off+rec>BLOCK_SIZE)return -1;
        if(ino&&len&&!(len==1&&data[off+8]=='.')&&!(len==2&&data[off+8]=='.'&&data[off+9]=='.')){if((size_t)count<capacity){size_t n=len<63?len:63;copy((uint8_t*)entries[count].name,data+off+8,n);entries[count].name[n]=0;entries[count].directory=data[off+7]==2;}count++;}off+=rec;}
    return count;
}
int ext4_read(const char *name,uint8_t *data,size_t cap,size_t *size){
    uint32_t ino;uint8_t type,raw[128],block[BLOCK_SIZE];if(!mounted||!entry_find(name,&ino,&type)||type==2||!inode_read(ino,raw))return 0;
    uint32_t n=rd32(raw+4);if(n>BLOCK_SIZE||n>cap||!block_read(rd32(raw+40),block))return 0;copy(data,block,n);*size=n;return 1;
}
int ext4_write(const char *name,const uint8_t *data,size_t size){
    if(!mounted||size>BLOCK_SIZE)return 0;
    uint32_t ino,blk;uint8_t type,raw[128],buf[BLOCK_SIZE];
    if(entry_find(name,&ino,&type)){if(type==2||!inode_read(ino,raw))return 0;blk=rd32(raw+40);}else{
        if(!allocate_inode(&ino,0)||!allocate_block(&blk))return 0;
        zero(raw,128);wr16(raw,0x81a4);wr16(raw+26,1);wr32(raw+40,blk);wr32(raw+28,2);
        if(!inode_write(ino,raw)||!root_add(name,ino,1))return 0;
    }
    zero(buf,BLOCK_SIZE);copy(buf,data,size);if(!block_write(blk,buf))return 0;wr32(raw+4,size);return inode_write(ino,raw);
}
int ext4_mkdir(const char *name){
    if(!mounted)return 0;
    uint32_t exists,ino,blk;uint8_t type,raw[128],dir[BLOCK_SIZE];
    if(entry_find(name,&exists,&type))return 0;
    if(!allocate_inode(&ino,1)||!allocate_block(&blk))return 0;
    zero(raw,128);wr16(raw,0x41ed);wr16(raw+26,2);wr32(raw+4,BLOCK_SIZE);wr32(raw+28,2);wr32(raw+40,blk);
    zero(dir,BLOCK_SIZE);wr32(dir,ino);wr16(dir+4,12);dir[6]=1;dir[7]=2;dir[8]='.';wr32(dir+12,ROOT_INODE);wr16(dir+16,BLOCK_SIZE-12);dir[18]=2;dir[19]=2;dir[20]='.';dir[21]='.';
    if(!block_write(blk,dir)||!inode_write(ino,raw)||!root_add(name,ino,2))return 0;
    uint8_t root[128];if(inode_read(ROOT_INODE,root)){wr16(root+26,rd16(root+26)+1);inode_write(ROOT_INODE,root);}return 1;
}
