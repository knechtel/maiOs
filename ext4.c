#include "ext4.h"
#include "scheduler.h"

#define SECTOR_SIZE 512u
#define BLOCK_SIZE 1024u
#define MAX_BLOCKS 2097152u
#define BLOCKS_PER_GROUP_LIMIT 8192u
#define MAX_GROUPS 256u
#define GROUP_DESC_SIZE 32u
#define GROUP_DESCS_PER_BLOCK (BLOCK_SIZE / GROUP_DESC_SIZE)
#define MAX_FILE_BLOCKS (12u + 256u + 65536u + 16777216u)
#define MAX_PATH_DEPTH 16u
#define ROOT_INODE 2u

static uint8_t mounted;
static uint8_t super[BLOCK_SIZE];
static uint8_t group_descs[MAX_GROUPS][GROUP_DESC_SIZE];
static uint32_t blocks_count, inodes_count, blocks_per_group, inodes_per_group;
static uint32_t inode_size, first_data_block, group_count;
static uint32_t active_inode;
static uint8_t active_inode_data[128];
static uint32_t read_inode;
static uint8_t read_inode_data[128];
static uint32_t cwd_stack[MAX_PATH_DEPTH + 1];
static char cwd_names[MAX_PATH_DEPTH][64];
static uint32_t cwd_depth, cwd_inode;

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
static int same(const char *a,const char *b) { while(*a&&*a==*b){a++;b++;}return *a==*b; }

static int wait_ata(int data) {
    for(uint32_t i=0;i<1000000;i++){ uint8_t s=inb(0x1f7); if(s&1)return 0; if(!(s&0x80)&&(!data||(s&8)))return 1; }
    return 0;
}
static int wait_ata_command_ready(void) {
    for(uint32_t i=0;i<1000000;i++){
        uint8_t s=inb(0x1f7);
        if(!(s&0x80)&&!(s&8))return 1;
    }
    return 0;
}
static int sector_io_once(uint32_t lba,uint8_t *buf,int write) {
    if(lba>=0x10000000||!wait_ata_command_ready())return 0;
    outb(0x1f6,0xe0|((lba>>24)&15)); outb(0x1f2,1); outb(0x1f3,lba); outb(0x1f4,lba>>8); outb(0x1f5,lba>>16); outb(0x1f7,write?0x30:0x20);
    if(!wait_ata(1))return 0;
    for(size_t i=0;i<256;i++) if(write) outw(0x1f0,buf[i*2]|(uint16_t)buf[i*2+1]<<8); else {uint16_t w=inw(0x1f0);buf[i*2]=w;buf[i*2+1]=w>>8;}
    if(write){outb(0x1f7,0xe7);return wait_ata(0);} return 1;
}
static int sector_io(uint32_t lba,uint8_t *buf,int write) {
    for(uint8_t attempt=0;attempt<3;attempt++)
        if(sector_io_once(lba,buf,write))return 1;
    return 0;
}
static int block_read(uint32_t b,uint8_t *p){return b<blocks_count&&sector_io(b*2,p,0)&&sector_io(b*2+1,p+512,0);}
static int block_write(uint32_t b,const uint8_t *p){return b<blocks_count&&sector_io(b*2,(uint8_t*)p,1)&&sector_io(b*2+1,(uint8_t*)p+512,1);}
static uint8_t *group_desc(uint32_t group) { return group_descs[group]; }
static int group_desc_write(uint32_t group) {
    uint8_t table[BLOCK_SIZE];
    uint32_t block = 2 + group / GROUP_DESCS_PER_BLOCK;
    uint32_t offset = (group % GROUP_DESCS_PER_BLOCK) * GROUP_DESC_SIZE;
    if (!block_read(block, table)) return 0;
    copy(table + offset, group_desc(group), GROUP_DESC_SIZE);
    return block_write(block, table);
}
static uint32_t group_block_limit(uint32_t group) {
    uint32_t first = first_data_block + group * blocks_per_group;
    uint32_t remaining = blocks_count - first;
    return remaining < blocks_per_group ? remaining : blocks_per_group;
}
static uint32_t group_inode_limit(uint32_t group) {
    uint32_t first = group * inodes_per_group;
    if (first >= inodes_count) return 0;
    uint32_t remaining = inodes_count - first;
    return remaining < inodes_per_group ? remaining : inodes_per_group;
}
static int inode_read(uint32_t ino,uint8_t *out){
    if(!ino||ino>inodes_count)return 0;
    uint32_t group=(ino-1)/inodes_per_group, index=(ino-1)%inodes_per_group;
    uint32_t table=rd32(group_desc(group)+8);
    uint32_t off=index*inode_size, b=table+off/BLOCK_SIZE, pos=off%BLOCK_SIZE;
    uint8_t tmp[BLOCK_SIZE]; if(!block_read(b,tmp))return 0; copy(out,tmp+pos,128); return 1;
}
static int inode_write(uint32_t ino,const uint8_t *in){
    if(!ino||ino>inodes_count)return 0;
    uint32_t group=(ino-1)/inodes_per_group, index=(ino-1)%inodes_per_group;
    uint32_t table=rd32(group_desc(group)+8);
    uint32_t off=index*inode_size, b=table+off/BLOCK_SIZE, pos=off%BLOCK_SIZE;
    uint8_t tmp[BLOCK_SIZE];
    for(uint8_t attempt=0;attempt<3;attempt++){
        if(!block_read(b,tmp))continue;
        copy(tmp+pos,in,128);
        if(block_write(b,tmp))return 1;
    }
    return 0;
}
static int allocate_inode(uint32_t *ino,int dir){
    uint8_t map[BLOCK_SIZE];
    for(uint32_t g=0;g<group_count;g++) {
        uint8_t *desc=group_desc(g);
        uint32_t limit=group_inode_limit(g),bit;
        if(!rd16(desc+14)||!limit)continue;
        if(!block_read(rd32(desc+4),map))return 0;
        int found=0;
        for(bit=0;bit<limit;bit++)if(!(map[bit>>3]&(1u<<(bit&7)))){map[bit>>3]|=1u<<(bit&7);found=1;break;}
        if(!found)continue;
        if(!block_write(rd32(desc+4),map))return 0;
        *ino=g*inodes_per_group+bit+1;
        wr16(desc+14,rd16(desc+14)-1);
        if(dir)wr16(desc+16,rd16(desc+16)+1);
        uint32_t free_inodes=rd32(super+16);if(free_inodes)wr32(super+16,free_inodes-1);
        return group_desc_write(g)&&block_write(1,super);
    }
    return 0;
}
static int allocate_block(uint32_t *block){
    uint8_t map[BLOCK_SIZE];
    for(uint32_t g=0;g<group_count;g++) {
        uint8_t *desc=group_desc(g);
        uint32_t limit=group_block_limit(g),bit;
        if(!rd16(desc+12)||!limit)continue;
        if(!block_read(rd32(desc),map))return 0;
        int found=0;
        for(bit=0;bit<limit;bit++)if(!(map[bit>>3]&(1u<<(bit&7)))){map[bit>>3]|=1u<<(bit&7);found=1;break;}
        if(!found)continue;
        if(!block_write(rd32(desc),map))return 0;
        *block=first_data_block+g*blocks_per_group+bit;
        wr16(desc+12,rd16(desc+12)-1);
        uint32_t free_blocks=rd32(super+12);if(free_blocks)wr32(super+12,free_blocks-1);
        return group_desc_write(g)&&block_write(1,super);
    }
    return 0;
}

static int allocate_zero_block(uint32_t *block) {
    uint8_t empty[BLOCK_SIZE];
    if(!allocate_block(block))return 0;
    zero(empty,sizeof(empty));
    return block_write(*block,empty);
}

static int inode_map_block(uint32_t ino,uint8_t *raw,uint32_t logical,int create,uint32_t *result) {
    uint32_t ptr,offset;
    uint8_t table[BLOCK_SIZE];
    if(logical<12) {
        offset=40+logical*4; ptr=rd32(raw+offset);
        if(!ptr&&create) {
            if(!allocate_zero_block(&ptr))return 0;
            wr32(raw+offset,ptr); wr32(raw+28,rd32(raw+28)+2);
            if(!inode_write(ino,raw))return 0;
        }
        *result=ptr; return ptr!=0;
    }

    logical-=12;
    if(logical<256) {
        ptr=rd32(raw+88);
        if(!ptr&&create) {
            if(!allocate_zero_block(&ptr))return 0;
            wr32(raw+88,ptr); wr32(raw+28,rd32(raw+28)+2);
            if(!inode_write(ino,raw))return 0;
        }
        if(!ptr||!block_read(ptr,table))return 0;
        uint32_t data=rd32(table+logical*4);
        if(!data&&create) {
            if(!allocate_zero_block(&data))return 0;
            wr32(table+logical*4,data);
            if(!block_write(ptr,table))return 0;
            wr32(raw+28,rd32(raw+28)+2);
            if(!inode_write(ino,raw))return 0;
        }
        *result=data; return data!=0;
    }

    logical-=256;
    if(logical>=256*256)goto triple_indirect;
    ptr=rd32(raw+92);
    if(!ptr&&create) {
        if(!allocate_zero_block(&ptr))return 0;
        wr32(raw+92,ptr); wr32(raw+28,rd32(raw+28)+2);
        if(!inode_write(ino,raw))return 0;
    }
    if(!ptr||!block_read(ptr,table))return 0;
    uint32_t first=logical/256,second=logical%256;
    uint32_t child=rd32(table+first*4);
    if(!child&&create) {
        if(!allocate_zero_block(&child))return 0;
        wr32(table+first*4,child);
        if(!block_write(ptr,table))return 0;
        wr32(raw+28,rd32(raw+28)+2);
        if(!inode_write(ino,raw))return 0;
    }
    if(!child||!block_read(child,table))return 0;
    uint32_t data=rd32(table+second*4);
    if(!data&&create) {
        if(!allocate_zero_block(&data))return 0;
        wr32(table+second*4,data);
        if(!block_write(child,table))return 0;
        wr32(raw+28,rd32(raw+28)+2);
        if(!inode_write(ino,raw))return 0;
    }
    *result=data; return data!=0;

triple_indirect:
    logical-=256*256;
    if(logical>=256*256*256)return 0;
    ptr=rd32(raw+96);
    if(!ptr&&create) {
        if(!allocate_zero_block(&ptr))return 0;
        wr32(raw+96,ptr);wr32(raw+28,rd32(raw+28)+2);
        if(!inode_write(ino,raw))return 0;
    }
    if(!ptr||!block_read(ptr,table))return 0;
    uint32_t triple_first=logical/(256*256),middle=logical/256%256,last=logical%256;
    uint32_t triple_child=rd32(table+triple_first*4);
    if(!triple_child&&create) {
        if(!allocate_zero_block(&triple_child))return 0;
        wr32(table+triple_first*4,triple_child);
        if(!block_write(ptr,table))return 0;
        wr32(raw+28,rd32(raw+28)+2);
        if(!inode_write(ino,raw))return 0;
    }
    if(!triple_child||!block_read(triple_child,table))return 0;
    uint32_t leaf=rd32(table+middle*4);
    if(!leaf&&create) {
        if(!allocate_zero_block(&leaf))return 0;
        wr32(table+middle*4,leaf);
        if(!block_write(triple_child,table))return 0;
        wr32(raw+28,rd32(raw+28)+2);
        if(!inode_write(ino,raw))return 0;
    }
    if(!leaf||!block_read(leaf,table))return 0;
    data=rd32(table+last*4);
    if(!data&&create) {
        if(!allocate_zero_block(&data))return 0;
        wr32(table+last*4,data);
        if(!block_write(leaf,table))return 0;
        wr32(raw+28,rd32(raw+28)+2);
        if(!inode_write(ino,raw))return 0;
    }
    *result=data;return data!=0;
}

static int ext4_mount_impl(void){
    uint8_t sec[512]; mounted=0;
    if(!sector_io(2,sec,0))return 0;
    copy(super,sec,512);
    if(!sector_io(3,sec,0))return 0;
    copy(super+512,sec,512);
    if(rd16(super+56)!=0xef53||rd32(super+24)!=0||rd16(super+88)<128)return 0;
    blocks_count=rd32(super+4);inodes_count=rd32(super);first_data_block=rd32(super+20);blocks_per_group=rd32(super+32);inodes_per_group=rd32(super+40);inode_size=rd16(super+88);
    uint32_t compat=rd32(super+92), incompat=rd32(super+96), ro=rd32(super+100);
    /* 1 KiB ext2-layout subset with up to 256 block groups. */
    uint16_t desc_size=rd16(super+254);
    if(!blocks_count||blocks_count>MAX_BLOCKS||first_data_block!=1||!blocks_per_group||blocks_per_group>BLOCKS_PER_GROUP_LIMIT||!inodes_per_group||inodes_per_group>BLOCKS_PER_GROUP_LIMIT||inode_size>BLOCK_SIZE||BLOCK_SIZE%inode_size|| (desc_size&&desc_size!=GROUP_DESC_SIZE))return 0;
    if((compat&~0x59u)||(incompat&~0x02u)||(ro&~0x6bu))return 0;
    group_count=(blocks_count-first_data_block+blocks_per_group-1)/blocks_per_group;
    if(!group_count||group_count>MAX_GROUPS||group_count*inodes_per_group<inodes_count)return 0;
    for(uint32_t g=0;g<group_count;){
        uint8_t table[BLOCK_SIZE];
        if(!block_read(2+g/GROUP_DESCS_PER_BLOCK,table))return 0;
        uint32_t first=g;
        for(uint32_t n=0;n<GROUP_DESCS_PER_BLOCK&&g<group_count;n++,g++)
            copy(group_desc(g),table+n*GROUP_DESC_SIZE,GROUP_DESC_SIZE);
        if(g==first)return 0;
    }
    for(uint32_t g=0;g<group_count;g++){
        uint8_t *desc=group_desc(g);
        if(rd32(desc)>=blocks_count||rd32(desc+4)>=blocks_count||rd32(desc+8)>=blocks_count)return 0;
    }
    cwd_depth=0;cwd_inode=ROOT_INODE;cwd_stack[0]=ROOT_INODE;
    mounted=1;return 1;
}

static int dir_find(uint32_t dir_ino,const char *name,uint32_t *ino,uint8_t *type){
    uint8_t raw[128],data[BLOCK_SIZE];
    if(!inode_read(dir_ino,raw)||!rd32(raw+4))return 0;
    uint32_t blocks=(rd32(raw+4)+BLOCK_SIZE-1)/BLOCK_SIZE;
    for(uint32_t b=0;b<blocks;b++){
        uint32_t physical;if(!inode_map_block(dir_ino,raw,b,0,&physical)||!block_read(physical,data))return 0;
        for(uint32_t off=0;off+8<=BLOCK_SIZE;){
            uint32_t n=rd32(data+off);uint16_t rec=rd16(data+off+4);uint8_t len=data[off+6];
            if(rec<8||off+rec>BLOCK_SIZE||len>rec-8)return 0;
            size_t k=0;while(name[k]&&k<len&&name[k]==(char)data[off+8+k])k++;
            if(name[k]==0&&k==len&&n){*ino=n;*type=data[off+7];return 1;}
            off+=rec;
        }
    }
    return 0;
}
static int entry_find(const char *name,uint32_t *ino,uint8_t *type){return dir_find(cwd_inode,name,ino,type);}
static int dir_add(uint32_t dir_ino,const char *name,uint32_t ino,uint8_t type){
    uint8_t raw[128],data[BLOCK_SIZE];size_t len=0;
    while(name[len])len++;
    if(!len||len>255||!inode_read(dir_ino,raw))return 0;
    uint32_t blocks=(rd32(raw+4)+BLOCK_SIZE-1)/BLOCK_SIZE;
    for(uint32_t b=0;b<blocks;b++){
        uint32_t physical;if(!inode_map_block(dir_ino,raw,b,0,&physical)||!block_read(physical,data))return 0;
        for(uint32_t off=0;off+8<=BLOCK_SIZE;){
            uint16_t rec=rd16(data+off+4);uint8_t old=data[off+6];
            if(rec<8||off+rec>BLOCK_SIZE||old>rec-8)return 0;
            uint16_t need=(uint16_t)((8+old+3)&~3);
            uint16_t entry_need=(uint16_t)((8+len+3)&~3);
            if(rec>=need+entry_need){
                wr16(data+off+4,need);uint32_t at=off+need;wr32(data+at,ino);wr16(data+at+4,rec-need);
                data[at+6]=len;data[at+7]=type;copy(data+at+8,(const uint8_t*)name,len);
                return block_write(physical,data);
            }
            off+=rec;
        }
    }
    uint32_t physical;
    if(!inode_map_block(dir_ino,raw,blocks,1,&physical))return 0;
    zero(data,sizeof(data));wr32(data,ino);wr16(data+4,BLOCK_SIZE);data[6]=len;data[7]=type;copy(data+8,(const uint8_t*)name,len);
    if(!block_write(physical,data))return 0;
    wr32(raw+4,(blocks+1)*BLOCK_SIZE);return inode_write(dir_ino,raw);
}

static int dir_remove(uint32_t dir_ino,const char *name,uint32_t expected_ino){
    uint8_t raw[128],data[BLOCK_SIZE];
    if(!inode_read(dir_ino,raw)||!rd32(raw+4))return 0;
    uint32_t blocks=(rd32(raw+4)+BLOCK_SIZE-1)/BLOCK_SIZE;
    for(uint32_t b=0;b<blocks;b++){
        uint32_t physical;
        if(!inode_map_block(dir_ino,raw,b,0,&physical)||!block_read(physical,data))return 0;
        for(uint32_t off=0;off+8<=BLOCK_SIZE;){
            uint32_t ino=rd32(data+off);uint16_t rec=rd16(data+off+4);uint8_t len=data[off+6];
            if(rec<8||off+rec>BLOCK_SIZE||len>rec-8)return 0;
            size_t n=0;while(name[n]&&n<len&&name[n]==(char)data[off+8+n])n++;
            if(ino==expected_ino&&name[n]==0&&n==len){
                wr32(data+off,0);
                return block_write(physical,data);
            }
            off+=rec;
        }
    }
    return 0;
}

static int directory_is_empty(uint32_t ino,const uint8_t *raw){
    uint8_t data[BLOCK_SIZE];
    uint32_t blocks=(rd32(raw+4)+BLOCK_SIZE-1)/BLOCK_SIZE;
    for(uint32_t b=0;b<blocks;b++){
        uint32_t physical;
        if(!inode_map_block(ino,(uint8_t *)raw,b,0,&physical)||!block_read(physical,data))return 0;
        for(uint32_t off=0;off+8<=BLOCK_SIZE;){
            uint32_t child=rd32(data+off);uint16_t rec=rd16(data+off+4);uint8_t len=data[off+6];
            if(rec<8||off+rec>BLOCK_SIZE||len>rec-8)return 0;
            if(child&&len&&!(len==1&&data[off+8]=='.')&&
               !(len==2&&data[off+8]=='.'&&data[off+9]=='.'))return 0;
            off+=rec;
        }
    }
    return 1;
}

static int free_block(uint32_t block){
    if(block<first_data_block||block>=blocks_count)return 0;
    uint32_t relative=block-first_data_block;
    uint32_t group=relative/blocks_per_group,bit=relative%blocks_per_group;
    if(group>=group_count||bit>=group_block_limit(group))return 0;
    uint8_t *desc=group_desc(group);uint8_t map[BLOCK_SIZE];
    if(!block_read(rd32(desc),map))return 0;
    if(!(map[bit>>3]&(1u<<(bit&7))))return 0;
    map[bit>>3]&=(uint8_t)~(1u<<(bit&7));
    if(!block_write(rd32(desc),map))return 0;
    wr16(desc+12,rd16(desc+12)+1);wr32(super+12,rd32(super+12)+1);
    return group_desc_write(group)&&block_write(1,super);
}

static int free_block_tree(uint32_t block,uint8_t level){
    if(!block)return 1;
    if(!level)return free_block(block);
    uint8_t pointers[BLOCK_SIZE];
    if(block>=blocks_count||!block_read(block,pointers))return 0;
    for(uint16_t i=0;i<BLOCK_SIZE/4;i++){
        uint32_t child=rd32(pointers+i*4);
        if(child&&!free_block_tree(child,(uint8_t)(level-1)))return 0;
    }
    return free_block(block);
}

static int free_inode(uint32_t ino,int directory){
    uint32_t group=(ino-1)/inodes_per_group,index=(ino-1)%inodes_per_group;
    if(group>=group_count||index>=group_inode_limit(group))return 0;
    uint8_t *desc=group_desc(group);uint8_t map[BLOCK_SIZE];
    if(!block_read(rd32(desc+4),map)||!(map[index>>3]&(1u<<(index&7))))return 0;
    map[index>>3]&=(uint8_t)~(1u<<(index&7));
    if(!block_write(rd32(desc+4),map))return 0;
    wr16(desc+14,rd16(desc+14)+1);
    if(directory&&rd16(desc+16))wr16(desc+16,rd16(desc+16)-1);
    wr32(super+16,rd32(super+16)+1);
    return group_desc_write(group)&&block_write(1,super);
}

static int remove_entry(const char *name,int want_directory){
    uint32_t ino,parent_ino=cwd_inode;uint8_t type,raw[128];
    if(!mounted||!name||!name[0]||same(name,".")||same(name,"..")||
       active_inode||!entry_find(name,&ino,&type)||!inode_read(ino,raw))return 0;
    uint16_t mode=rd16(raw);
    int is_directory=(mode&0xF000)==0x4000;
    uint16_t file_type=mode&0xF000;
    if(is_directory!=want_directory||
       (!is_directory&&file_type!=0x8000&&file_type!=0xA000))return 0;
    if(read_inode==ino)return 0;
    if(is_directory&&!directory_is_empty(ino,raw))return 0;
    if(!dir_remove(parent_ino,name,ino))return 0;
    if(!is_directory&&rd16(raw+26)>1){
        wr16(raw+26,rd16(raw+26)-1);
        return inode_write(ino,raw);
    }

    int fast_symlink=file_type==0xA000&&rd32(raw+28)==0&&rd32(raw+4)<=60;
    if(!fast_symlink){
        for(uint8_t i=0;i<12;i++){
            uint32_t block=rd32(raw+40+i*4);
            if(block&&!free_block(block))return 0;
        }
        if(!free_block_tree(rd32(raw+88),1)||
           !free_block_tree(rd32(raw+92),2)||
           !free_block_tree(rd32(raw+96),3))return 0;
    }
    if(is_directory){
        uint8_t parent[128];
        if(!inode_read(parent_ino,parent))return 0;
        if(rd16(parent+26)>2)wr16(parent+26,rd16(parent+26)-1);
        if(!inode_write(parent_ino,parent))return 0;
    }
    return free_inode(ino,is_directory);
}

static int ext4_unlink_impl(const char *name){return remove_entry(name,0);}
static int ext4_rmdir_impl(const char *name){return remove_entry(name,1);}

static int delete_directory_contents(void){
    ext4_entry_t entries[16];
    for(;;){
        int count=ext4_list(entries,sizeof(entries)/sizeof(entries[0]));
        if(count<0)return 0;
        if(count==0)return 1;
        const char *name=entries[0].name;
        if(entries[0].directory){
            if(!ext4_chdir(name))return 0;
            int deleted=delete_directory_contents();
            if(!ext4_chdir("..")||!deleted||!ext4_rmdir(name))return 0;
        }else if(!ext4_unlink(name))return 0;
    }
}

static int ext4_delete_impl(const char *name){
    uint32_t ino;uint8_t type;
    if(!mounted||!name||!name[0]||same(name,".")||same(name,"..")||active_inode||
       !entry_find(name,&ino,&type)||!ext4_chdir(name))return 0;
    int deleted=delete_directory_contents();
    if(!ext4_chdir("..")||!deleted)return 0;
    return ext4_rmdir(name);
}

static int ext4_rename_impl(const char *old_name,const char *new_name){
    uint32_t ino,destination;uint8_t source_type,destination_type,raw[128];
    if(!mounted||!old_name||!new_name||!old_name[0]||!new_name[0]||
       same(old_name,".")||same(old_name,"..")||same(new_name,".")||same(new_name,"..")||
       active_inode||!entry_find(old_name,&ino,&source_type)||
       !inode_read(ino,raw)||
       (((rd16(raw)&0xF000)!=0x8000)&&((rd16(raw)&0xF000)!=0xA000))||
       read_inode==ino)return 0;
    if(same(old_name,new_name))return 1;
    if(entry_find(new_name,&destination,&destination_type)){
        if(destination==ino)return 1;
        if(destination_type==2||!remove_entry(new_name,0))return 0;
    }
    if(!dir_add(cwd_inode,new_name,ino,source_type))return 0;
    if(dir_remove(cwd_inode,old_name,ino))return 1;
    dir_remove(cwd_inode,new_name,ino);
    return 0;
}

static int ext4_list_impl(ext4_entry_t *entries,size_t capacity){
    uint8_t raw[128],data[BLOCK_SIZE];if(!mounted||!inode_read(cwd_inode,raw))return -1;int count=0;
    uint32_t blocks=(rd32(raw+4)+BLOCK_SIZE-1)/BLOCK_SIZE;
    for(uint32_t b=0;b<blocks;b++){
        uint32_t physical;if(!inode_map_block(cwd_inode,raw,b,0,&physical)||!block_read(physical,data))return -1;
        for(uint32_t off=0;off+8<=BLOCK_SIZE;){uint32_t ino=rd32(data+off);uint16_t rec=rd16(data+off+4);uint8_t len=data[off+6];if(rec<8||off+rec>BLOCK_SIZE||len>rec-8)return -1;
            if(ino&&len&&!(len==1&&data[off+8]=='.')&&!(len==2&&data[off+8]=='.'&&data[off+9]=='.')){if((size_t)count<capacity){size_t n=len<63?len:63;copy((uint8_t*)entries[count].name,data+off+8,n);entries[count].name[n]=0;entries[count].directory=data[off+7]==2;}count++;}off+=rec;}
    }
    return count;
}
static int ext4_chdir_impl(const char *path){
    if(!mounted||!path||!*path)return 0;
    if(*path=='/'){cwd_depth=0;cwd_inode=ROOT_INODE;cwd_stack[0]=ROOT_INODE;}
    while(*path){
        while(*path=='/')path++;
        if(!*path)break;
        char component[64];size_t n=0;
        while(path[n]&&path[n]!='/'){if(n==63)return 0;component[n]=path[n];n++;}
        component[n]=0;path+=n;
        if(same(component,"."))continue;
        if(same(component,"..")){if(cwd_depth){cwd_depth--;cwd_inode=cwd_stack[cwd_depth];}continue;}
        uint32_t ino;uint8_t type;
        if(cwd_depth==MAX_PATH_DEPTH||!dir_find(cwd_inode,component,&ino,&type)||type!=2)return 0;
        copy((uint8_t*)cwd_names[cwd_depth],(const uint8_t*)component,n+1);
        cwd_depth++;cwd_inode=ino;cwd_stack[cwd_depth]=ino;
    }
    return 1;
}
static int ext4_pwd_impl(char *buffer,size_t capacity){
    size_t pos=0;if(!capacity)return 0;buffer[pos++]='/';
    for(uint32_t i=0;i<cwd_depth;i++){
        for(size_t j=0;cwd_names[i][j];j++){if(pos+1>=capacity)return 0;buffer[pos++]=cwd_names[i][j];}
        if(i+1<cwd_depth){if(pos+1>=capacity)return 0;buffer[pos++]='/';}
    }
    buffer[pos]=0;return 1;
}
static int symlink_target(uint32_t ino,uint8_t *raw,char *target,size_t capacity){
    if(!inode_read(ino,raw)||(rd16(raw)&0xF000)!=0xA000)return 0;
    uint32_t size=rd32(raw+4);if(!size||size>=capacity)return 0;
    if(!rd32(raw+28))copy((uint8_t*)target,raw+40,size);
    else {
        uint8_t block[BLOCK_SIZE];uint32_t offset=0;
        while(offset<size){uint32_t physical;if(!inode_map_block(ino,raw,offset/BLOCK_SIZE,0,&physical)||!block_read(physical,block))return 0;
            uint32_t count=size-offset;if(count>BLOCK_SIZE)count=BLOCK_SIZE;
            copy((uint8_t*)target+offset,block,count);offset+=count;}
    }
    target[size]=0;return 1;
}
static int path_append(char *path,size_t capacity,const char *component){
    size_t length=0,component_length=0;while(path[length])length++;while(component[component_length])component_length++;
    size_t separator=length>1; if(length+separator+component_length>=capacity)return 0;
    if(separator)path[length++]='/';
    copy((uint8_t*)path+length,(const uint8_t*)component,component_length+1);
    return 1;
}
static int ext4_resolve_path_impl(const char *path,char *resolved,size_t capacity,int allow_missing_leaf){
    if(!mounted||!path||!*path||!capacity)return 0;
    char old_directory[256],pending[512];if(!ext4_pwd_impl(old_directory,sizeof(old_directory)))return 0;
    size_t input_length=0;while(path[input_length]&&input_length<sizeof(pending)-1)input_length++;
    if(path[input_length])return 0;
    copy((uint8_t*)pending,(const uint8_t*)path,input_length+1);
    size_t base_length=0;
    if(path[0]=='/')base_length=1;
    else {while(old_directory[base_length])base_length++;}
    if(base_length>=sizeof(pending))return 0;
    char base[512];if(path[0]=='/'){base[0]='/';base[1]=0;}else copy((uint8_t*)base,(const uint8_t*)old_directory,base_length+1);
    unsigned int links=0;int ok=0;
    while(1){
        while(*pending=='/')for(size_t i=0;pending[i];i++)pending[i]=pending[i+1];
        if(!*pending){ok=1;break;}
        char component[64],remainder[512];size_t n=0;
        while(pending[n]&&pending[n]!='/'){if(n>=63)goto done;component[n]=pending[n];n++;}
        component[n]=0;size_t rest=0;while(pending[n]=='/')n++;
        while(pending[n+rest]){if(rest>=sizeof(remainder)-1)goto done;remainder[rest]=pending[n+rest];rest++;}
        remainder[rest]=0;
        for(size_t i=0;i<sizeof(pending);i++)pending[i]=0;
        if(same(component,".")){copy((uint8_t*)pending,(const uint8_t*)remainder,rest+1);continue;}
        if(same(component,"..")){
            size_t length=0;while(base[length])length++;
            if(length>1){while(length>1&&base[length-1]!='/')length--;if(length>1)length--;base[length]=0;}
            copy((uint8_t*)pending,(const uint8_t*)remainder,rest+1);continue;
        }
        if(!ext4_chdir_impl(base))goto done;
        uint32_t ino;uint8_t type,raw[128];
        if(!dir_find(cwd_inode,component,&ino,&type)){
            if(allow_missing_leaf&&!rest&&path_append(base,sizeof(base),component)){ok=1;break;}
            goto done;
        }
        if(!inode_read(ino,raw))goto done;
        uint16_t mode=rd16(raw)&0xF000;
        if(mode==0xA000||type==7){
            char target[512],combined[512];if(++links>16||!symlink_target(ino,raw,target,sizeof(target)))goto done;
            size_t t=0,r=0;while(target[t])t++;while(remainder[r])r++;
            if(t+r+(r?1:0)>=sizeof(combined))goto done;
            copy((uint8_t*)combined,(const uint8_t*)target,t);
            if(r){combined[t++]='/';copy((uint8_t*)combined+t,(const uint8_t*)remainder,r+1);}else combined[t]=0;
            if(target[0]=='/'){base[0]='/';base[1]=0;}
            copy((uint8_t*)pending,(const uint8_t*)combined,t+r+(r?2:1));
            continue;
        }
        if(rest&&mode!=0x4000&&type!=2)goto done;
        if(!path_append(base,sizeof(base),component))goto done;
        copy((uint8_t*)pending,(const uint8_t*)remainder,rest+1);
    }
done:
    if(ok){size_t length=0;while(base[length])length++;if(length>=capacity)ok=0;else copy((uint8_t*)resolved,(const uint8_t*)base,length+1);}
    ext4_chdir_impl(old_directory);
    return ok;
}
static int canonical_parent_leaf(const char *path,int allow_missing,
                                 char *parent,size_t parent_capacity,
                                 char *leaf,size_t leaf_capacity){
    char canonical[512];
    if(!ext4_resolve_path_impl(path,canonical,sizeof(canonical),allow_missing))return 0;
    size_t length=0,slash=(size_t)-1;while(canonical[length]){if(canonical[length]=='/')slash=length;length++;}
    size_t start=slash==(size_t)-1?0:slash+1,leaf_length=length-start;
    if(!leaf_length||leaf_length>=leaf_capacity)return 0;
    for(size_t i=0;i<=leaf_length;i++)leaf[i]=canonical[start+i];
    if(slash==(size_t)-1){if(parent_capacity<2)return 0;parent[0]='/';parent[1]=0;}
    else {size_t n=slash?slash:1;if(n>=parent_capacity)return 0;for(size_t i=0;i<n;i++)parent[i]=canonical[i];parent[n]=0;}
    return 1;
}
static int ext4_link_impl(const char *existing_path,const char *new_path){
    char original[256],source_parent[512],destination_parent[512],source_leaf[64],destination_leaf[64];
    uint32_t ino;uint8_t type,raw[128];
    if(!ext4_pwd_impl(original,sizeof(original))||
       !canonical_parent_leaf(existing_path,0,source_parent,sizeof(source_parent),source_leaf,sizeof(source_leaf))||
       !canonical_parent_leaf(new_path,1,destination_parent,sizeof(destination_parent),destination_leaf,sizeof(destination_leaf)))return 0;
    if(!ext4_chdir_impl(source_parent)||!entry_find(source_leaf,&ino,&type)||!inode_read(ino,raw))goto failed;
    uint16_t mode=rd16(raw)&0xF000;
    if((mode!=0x8000&&mode!=0xA000)||rd16(raw+26)==0xFFFF)goto failed;
    if(!ext4_chdir_impl(destination_parent))goto failed;
    {uint32_t existing;uint8_t existing_type;if(entry_find(destination_leaf,&existing,&existing_type))goto failed;}
    if(!dir_add(cwd_inode,destination_leaf,ino,type))goto failed;
    wr16(raw+26,rd16(raw+26)+1);
    if(!inode_write(ino,raw)){dir_remove(cwd_inode,destination_leaf,ino);goto failed;}
    ext4_chdir_impl(original);return 1;
failed:
    ext4_chdir_impl(original);return 0;
}
int ext4_resolve_path(const char *path,char *resolved,size_t capacity,int allow_missing_leaf){
    scheduler_critical_enter();int ok=ext4_resolve_path_impl(path,resolved,capacity,allow_missing_leaf);scheduler_critical_leave();return ok;
}
static int ext4_inode_number_impl(const char *path,uint32_t *result){
    char original[256],parent[512],leaf[64];uint32_t ino;uint8_t type;
    if(!result||!ext4_pwd_impl(original,sizeof(original)))return 0;
    char canonical[512];
    if(!ext4_resolve_path_impl(path,canonical,sizeof(canonical),0))return 0;
    if(same(canonical,"/")){*result=ROOT_INODE;return 1;}
    if(!canonical_parent_leaf(canonical,0,parent,sizeof(parent),leaf,sizeof(leaf))||
       !ext4_chdir_impl(parent)||!entry_find(leaf,&ino,&type)){
        ext4_chdir_impl(original);return 0;
    }
    ext4_chdir_impl(original);*result=ino;return 1;
}
int ext4_inode_number(const char *path,uint32_t *inode){
    scheduler_critical_enter();int ok=ext4_inode_number_impl(path,inode);scheduler_critical_leave();return ok;
}
static int ext4_readlink_impl(const char *path,char *target,size_t capacity){
    if(!path||!*path||!target||!capacity)return 0;
    char old_directory[256],input[512],parent_input[512],parent[512],leaf[64];
    if(!ext4_pwd_impl(old_directory,sizeof(old_directory)))return 0;
    size_t length=0;while(path[length]&&length<sizeof(input)-1)length++;if(path[length])return 0;
    copy((uint8_t*)input,(const uint8_t*)path,length+1);
    char *slash=0;for(size_t i=0;i<length;i++)if(input[i]=='/')slash=&input[i];
    char *name=slash?slash+1:input;size_t name_length=0;while(name[name_length])name_length++;
    if(!name_length||name_length>63)return 0;
    copy((uint8_t*)leaf,(const uint8_t*)name,name_length+1);
    if(slash){size_t parent_length=slash==input?1:(size_t)(slash-input);if(slash==input){parent_input[0]='/';parent_input[1]=0;}else{copy((uint8_t*)parent_input,(const uint8_t*)input,parent_length);parent_input[parent_length]=0;}}
    else {size_t old_length=0;while(old_directory[old_length])old_length++;copy((uint8_t*)parent_input,(const uint8_t*)old_directory,old_length+1);}
    if(!ext4_resolve_path_impl(parent_input,parent,sizeof(parent),0)||!ext4_chdir_impl(parent))goto failed;
    uint32_t ino;uint8_t type,raw[128];
    if(!dir_find(cwd_inode,leaf,&ino,&type)||!symlink_target(ino,raw,target,capacity))goto failed;
    ext4_chdir_impl(old_directory);
    size_t target_length=0;while(target[target_length])target_length++;return (int)target_length;
failed:
    ext4_chdir_impl(old_directory);
    return 0;
}
int ext4_readlink(const char *path,char *target,size_t capacity){
    scheduler_critical_enter();int length=ext4_readlink_impl(path,target,capacity);scheduler_critical_leave();return length;
}
static int ext4_entry_type_impl(const char *name){
    uint32_t ino;uint8_t type,raw[128];
    if(!mounted||!name||!name[0]||!entry_find(name,&ino,&type)||!inode_read(ino,raw))return 0;
    uint16_t mode=rd16(raw)&0xF000;
    if(mode==0x4000)return 2;
    if(mode==0xA000)return 3;
    if(mode==0x8000)return 1;
    return 0;
}
int ext4_entry_type(const char *name){
    scheduler_critical_enter();int type=ext4_entry_type_impl(name);scheduler_critical_leave();return type;
}
static int ext4_read_start_impl(const char *name,uint32_t *size){
    uint32_t ino;uint8_t type;
    if(!mounted||!entry_find(name,&ino,&type)||type==2||!inode_read(ino,read_inode_data))return 0;
    read_inode=ino;*size=rd32(read_inode_data+4);return 1;
}
static int ext4_read_block_impl(uint32_t index,uint8_t *data){
    uint32_t block;
    return read_inode&&inode_map_block(read_inode,read_inode_data,index,0,&block)&&block_read(block,data);
}
static void ext4_read_end_impl(void){read_inode=0;zero(read_inode_data,sizeof(read_inode_data));}
static int ext4_write_begin_impl(const char *name){
    uint32_t ino;uint8_t type;
    if(!mounted||active_inode)return 0;
    if(entry_find(name,&ino,&type)) {
        if(type==2||!inode_read(ino,active_inode_data))return 0;
    } else {
        if(!allocate_inode(&ino,0))return 0;
        zero(active_inode_data,sizeof(active_inode_data));
        wr16(active_inode_data,0x81a4);wr16(active_inode_data+26,1);
        if(!inode_write(ino,active_inode_data)||!dir_add(cwd_inode,name,ino,1))return 0;
    }
    active_inode=ino;
    wr32(active_inode_data+4,0);
    return 1;
}
static int ext4_write_block_impl(uint32_t index,const uint8_t *data,size_t size){
    uint32_t block;uint8_t buffer[BLOCK_SIZE];
    if(!active_inode||index>=MAX_FILE_BLOCKS||size>BLOCK_SIZE||!inode_map_block(active_inode,active_inode_data,index,1,&block))return 0;
    zero(buffer,sizeof(buffer));copy(buffer,data,size);return block_write(block,buffer);
}
static int ext4_write_finish_impl(uint32_t size){
    if(!active_inode)return 0;
    wr32(active_inode_data+4,size);
    int ok=inode_write(active_inode,active_inode_data);
    active_inode=0;zero(active_inode_data,sizeof(active_inode_data));return ok;
}
static int ext4_write_impl(const char *name,const uint8_t *data,size_t size){
    if(size>MAX_BLOCKS*BLOCK_SIZE||!ext4_write_begin(name))return 0;
    uint32_t blocks=(size+BLOCK_SIZE-1)/BLOCK_SIZE;
    for(uint32_t i=0;i<blocks;i++){
        size_t n=size-(size_t)i*BLOCK_SIZE;if(n>BLOCK_SIZE)n=BLOCK_SIZE;
        if(!ext4_write_block(i,data+(size_t)i*BLOCK_SIZE,n)){ext4_write_finish(i*BLOCK_SIZE);return 0;}
    }
    return ext4_write_finish((uint32_t)size);
}
static int ext4_mkdir_impl(const char *name){
    if(!mounted)return 0;
    uint32_t exists,ino,blk;uint8_t type,raw[128],dir[BLOCK_SIZE];
    if(entry_find(name,&exists,&type))return 0;
    if(!allocate_inode(&ino,1)||!allocate_block(&blk))return 0;
    zero(raw,128);wr16(raw,0x41ed);wr16(raw+26,2);wr32(raw+4,BLOCK_SIZE);wr32(raw+28,2);wr32(raw+40,blk);
    zero(dir,BLOCK_SIZE);wr32(dir,ino);wr16(dir+4,12);dir[6]=1;dir[7]=2;dir[8]='.';wr32(dir+12,cwd_inode);wr16(dir+16,BLOCK_SIZE-12);dir[18]=2;dir[19]=2;dir[20]='.';dir[21]='.';
    if(!block_write(blk,dir)||!inode_write(ino,raw)||!dir_add(cwd_inode,name,ino,2))return 0;
    uint8_t parent[128];if(inode_read(cwd_inode,parent)){wr16(parent+26,rd16(parent+26)+1);inode_write(cwd_inode,parent);}return 1;
}
static int ext4_symlink_impl(const char *target,const char *name){
    if(!mounted||!target||!target[0]||!name||!name[0]||active_inode)return 0;
    size_t length=0;while(target[length]&&length<1024)length++;
    if(target[length]||length>1023)return 0;
    uint32_t exists,ino;uint8_t type,raw[128],data[BLOCK_SIZE];
    if(entry_find(name,&exists,&type)||!allocate_inode(&ino,0))return 0;
    zero(raw,sizeof(raw));wr16(raw,0xA1FF);wr16(raw+26,1);wr32(raw+4,(uint32_t)length);
    if(length<=60)copy(raw+40,(const uint8_t*)target,length);
    else {
        for(uint32_t offset=0,logical=0;offset<length;logical++){
            uint32_t block;size_t count=length-offset;if(count>BLOCK_SIZE)count=BLOCK_SIZE;
            if(!inode_map_block(ino,raw,logical,1,&block))return 0;
            zero(data,sizeof(data));copy(data,(const uint8_t*)target+offset,count);
            if(!block_write(block,data))return 0;
            offset+=(uint32_t)count;
        }
    }
    if(!inode_write(ino,raw)||!dir_add(cwd_inode,name,ino,7))return 0;
    return 1;
}

int ext4_mount(void){scheduler_critical_enter();int ok=ext4_mount_impl();scheduler_critical_leave();return ok;}
int ext4_unlink(const char *name){scheduler_critical_enter();int ok=ext4_unlink_impl(name);scheduler_critical_leave();return ok;}
int ext4_rmdir(const char *name){scheduler_critical_enter();int ok=ext4_rmdir_impl(name);scheduler_critical_leave();return ok;}
int ext4_delete(const char *name){scheduler_critical_enter();int ok=ext4_delete_impl(name);scheduler_critical_leave();return ok;}
int ext4_rename(const char *old_name,const char *new_name){scheduler_critical_enter();int ok=ext4_rename_impl(old_name,new_name);scheduler_critical_leave();return ok;}
int ext4_list(ext4_entry_t *entries,size_t capacity){scheduler_critical_enter();int count=ext4_list_impl(entries,capacity);scheduler_critical_leave();return count;}
int ext4_chdir(const char *path){scheduler_critical_enter();char resolved[512];int ok=ext4_resolve_path_impl(path,resolved,sizeof(resolved),0)&&ext4_chdir_impl(resolved);scheduler_critical_leave();return ok;}
int ext4_pwd(char *buffer,size_t capacity){scheduler_critical_enter();int ok=ext4_pwd_impl(buffer,capacity);scheduler_critical_leave();return ok;}

int ext4_read_start(const char *name,uint32_t *size){
    scheduler_critical_enter();
    char canonical[512],old_directory[256],basename[64];
    if(!ext4_pwd_impl(old_directory,sizeof(old_directory))||
       !ext4_resolve_path_impl(name,canonical,sizeof(canonical),0)){
        scheduler_critical_leave();return 0;
    }
    char *slash=0;for(size_t i=0;canonical[i];i++)if(canonical[i]=='/')slash=&canonical[i];
    char *leaf=slash?slash+1:canonical;
    size_t length=0;while(leaf[length])length++;
    if(!length||length>63){scheduler_critical_leave();return 0;}
    for(size_t i=0;i<=length;i++)basename[i]=leaf[i];
    if(slash){if(slash==canonical)slash[1]=0;else *slash=0;if(!ext4_chdir_impl(canonical)){ext4_chdir_impl(old_directory);scheduler_critical_leave();return 0;}}
    int ok=ext4_read_start_impl(basename,size);
    ext4_chdir_impl(old_directory);
    if(!ok){scheduler_critical_leave();return 0;}
    return 1;
}
int ext4_read_block(uint32_t index,uint8_t *data){return ext4_read_block_impl(index,data);}
void ext4_read_end(void){if(read_inode){ext4_read_end_impl();scheduler_critical_leave();}}

int ext4_write_begin(const char *name){
    scheduler_critical_enter();
    if(!ext4_write_begin_impl(name)){scheduler_critical_leave();return 0;}
    return 1;
}
int ext4_write_block(uint32_t index,const uint8_t *data,size_t size){return ext4_write_block_impl(index,data,size);}
int ext4_write_finish(uint32_t size){
    if(!active_inode)return 0;
    int ok=ext4_write_finish_impl(size);scheduler_critical_leave();return ok;
}
int ext4_write(const char *name,const uint8_t *data,size_t size){
    scheduler_critical_enter();int ok=ext4_write_impl(name,data,size);scheduler_critical_leave();return ok;
}
int ext4_mkdir(const char *name){scheduler_critical_enter();int ok=ext4_mkdir_impl(name);scheduler_critical_leave();return ok;}
int ext4_symlink(const char *target,const char *name){scheduler_critical_enter();int ok=ext4_symlink_impl(target,name);scheduler_critical_leave();return ok;}
int ext4_link(const char *existing_path,const char *new_path){scheduler_critical_enter();int ok=ext4_link_impl(existing_path,new_path);scheduler_critical_leave();return ok;}
