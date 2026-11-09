#include <stdint.h>
#include <stddef.h>
#include "ext4.h"
#include "bearssl.h"
#include "gitpack.h"

#define GIT_PACK_OBJECT_LIMIT 2048u
#define GIT_OBJECT_LIMIT (4u*1024u*1024u)
#define GIT_TREE_DEPTH 16u
#define GIT_TREE_ENTRIES 256u
#define GIT_BLOCK_SIZE 1024u
#define GIT_PACK_HASH_SIZE 20u

typedef struct {
    int16_t child[2];
    int16_t symbol;
} huffman_node_t;

typedef struct {
    huffman_node_t nodes[600];
    uint16_t count;
} huffman_tree_t;

typedef struct {
    uint32_t offset;
    uint32_t size;
    uint8_t type;
    uint8_t oid[20];
} git_pack_object_t;

typedef struct {
    uint32_t file_size;
    uint32_t position;
    uint32_t hash_end;
    uint32_t cached_block;
    uint8_t cache_valid;
    uint8_t cache[GIT_BLOCK_SIZE];
    br_sha1_context hash;
} pack_reader_t;

typedef struct {
    pack_reader_t *reader;
    uint32_t bits;
    uint8_t bit_count;
} bit_reader_t;

typedef struct {
    uint32_t block_index;
    uint16_t used;
    uint32_t length;
    uint8_t block[GIT_BLOCK_SIZE];
} ext4_writer_t;

typedef struct {
    char name[64];
    uint8_t oid[20];
    uint32_t mode;
} git_tree_entry_t;

static git_pack_object_t pack_objects[GIT_PACK_OBJECT_LIMIT];
static uint16_t pack_object_count;
static uint8_t inflated[GIT_OBJECT_LIMIT];
static uint8_t delta_data[GIT_OBJECT_LIMIT];
static uint8_t base_data[GIT_OBJECT_LIMIT];
static uint8_t object_data[GIT_OBJECT_LIMIT];
static huffman_tree_t literal_tree,distance_tree,code_tree;
static git_tree_entry_t tree_entries[GIT_TREE_DEPTH][GIT_TREE_ENTRIES];

static uint8_t git_hex(uint8_t value)
{
    if(value>='0'&&value<='9')return value-'0';
    if(value>='a'&&value<='f')return value-'a'+10;
    if(value>='A'&&value<='F')return value-'A'+10;
    return 0xFF;
}

static void oid_to_hex(const uint8_t oid[20],char text[41])
{
    static const char digits[]="0123456789abcdef";
    for(uint8_t i=0;i<20;i++){text[i*2]=digits[oid[i]>>4];text[i*2+1]=digits[oid[i]&15];}
    text[40]=0;
}

static int hex_to_oid(const char *text,uint8_t oid[20])
{
    for(uint8_t i=0;i<20;i++){
        uint8_t high=git_hex((uint8_t)text[i*2]),low=git_hex((uint8_t)text[i*2+1]);
        if(high==0xFF||low==0xFF)return 0;
        oid[i]=(uint8_t)((high<<4)|low);
    }
    return text[40]==0;
}

static int text_equal(const char *a,const char *b)
{
    while(*a&&*a==*b){a++;b++;}return *a==*b;
}

static uint32_t read_be32(const uint8_t *p)
{
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}

static void write_be32(uint8_t *p,uint32_t value)
{
    p[0]=(uint8_t)(value>>24);p[1]=(uint8_t)(value>>16);p[2]=(uint8_t)(value>>8);p[3]=(uint8_t)value;
}

static int writer_flush(ext4_writer_t *writer)
{
    if(!writer->used)return 1;
    if(!ext4_write_block(writer->block_index,writer->block,writer->used))return 0;
    writer->block_index++;writer->length+=writer->used;writer->used=0;return 1;
}

static int writer_append(ext4_writer_t *writer,const uint8_t *data,uint32_t length)
{
    while(length){
        uint16_t count=(uint16_t)(GIT_BLOCK_SIZE-writer->used);
        if(count>length)count=(uint16_t)length;
        for(uint16_t i=0;i<count;i++)writer->block[writer->used+i]=data[i];
        writer->used+=count;data+=count;length-=count;
        if(writer->used==GIT_BLOCK_SIZE&&!writer_flush(writer))return 0;
    }
    return 1;
}

static int writer_finish(ext4_writer_t *writer)
{
    if(!writer_flush(writer))return 0;
    return ext4_write_finish(writer->length);
}

static int ensure_directory(const char *path)
{
    char original[1100],component[64];
    if(!ext4_pwd(original,sizeof(original))||!ext4_chdir("/"))return 0;
    while(*path=='/')path++;
    while(*path){
        uint8_t length=0;
        while(path[length]&&path[length]!='/'){
            if(length>=sizeof(component)-1){(void)ext4_chdir(original);return 0;}
            component[length]=path[length];length++;
        }
        component[length]=0;path+=length;while(*path=='/')path++;
        if(!length)continue;
        if(!ext4_chdir(component)){
            if(!ext4_mkdir(component)||!ext4_chdir(component)){
                (void)ext4_chdir(original);return 0;
            }
        }
    }
    return ext4_chdir(original);
}

static int write_file_path(const char *path,const uint8_t *data,uint32_t length)
{
    char directory[256],filename[64],original[1100];
    uint16_t size=0,last_slash=0;
    while(path[size]){if(path[size]=='/')last_slash=size;size++;if(size>=sizeof(directory))return 0;}
    if(!size||size-last_slash-1>=sizeof(filename))return 0;
    for(uint16_t i=0;i<last_slash;i++)directory[i]=path[i];directory[last_slash]=0;
    for(uint16_t i=0;i<size-last_slash-1;i++)filename[i]=path[last_slash+1+i];filename[size-last_slash-1]=0;
    if(!ext4_pwd(original,sizeof(original))||!ensure_directory(directory)||!ext4_chdir(directory))return 0;
    int ok=ext4_write_begin(filename);ext4_writer_t writer={0};
    if(ok&&length)ok=writer_append(&writer,data,length);
    if(ok)ok=writer_finish(&writer);else (void)ext4_write_finish(writer.length);
    (void)ext4_chdir(original);return ok;
}

static int read_file_path(const char *path,uint8_t *destination,uint32_t capacity,uint32_t *size)
{
    char directory[256],filename[64],original[1100];
    uint16_t length=0,last_slash=0;
    while(path[length]){if(path[length]=='/')last_slash=length;length++;if(length>=sizeof(directory))return 0;}
    if(!length||length-last_slash-1>=sizeof(filename))return 0;
    for(uint16_t i=0;i<last_slash;i++)directory[i]=path[i];directory[last_slash]=0;
    for(uint16_t i=0;i<length-last_slash-1;i++)filename[i]=path[last_slash+1+i];filename[length-last_slash-1]=0;
    if(!ext4_pwd(original,sizeof(original))||!ext4_chdir(directory))return 0;
    uint32_t file_size=0;int ok=ext4_read_start(filename,&file_size);
    if(!ok||file_size>capacity){if(ok)ext4_read_end();(void)ext4_chdir(original);return 0;}
    uint32_t position=0;uint8_t block[GIT_BLOCK_SIZE];
    while(position<file_size){
        if(!ext4_read_block(position/GIT_BLOCK_SIZE,block)){ok=0;break;}
        uint16_t take=(uint16_t)(file_size-position>GIT_BLOCK_SIZE?GIT_BLOCK_SIZE:file_size-position);
        for(uint16_t i=0;i<take;i++)destination[position+i]=block[i];
        position+=take;
    }
    ext4_read_end();(void)ext4_chdir(original);if(!ok)return 0;*size=file_size;return 1;
}

static int pack_byte(pack_reader_t *reader,uint8_t *value)
{
    if(reader->position>=reader->file_size)return 0;
    uint32_t block_index=reader->position/GIT_BLOCK_SIZE;
    if(!reader->cache_valid||reader->cached_block!=block_index){
        if(!ext4_read_block(block_index,reader->cache))return 0;
        reader->cached_block=block_index;reader->cache_valid=1;
    }
    *value=reader->cache[reader->position%GIT_BLOCK_SIZE];
    if(reader->position<reader->hash_end)br_sha1_update(&reader->hash,value,1);
    reader->position++;return 1;
}

static int pack_bytes(pack_reader_t *reader,uint8_t *destination,uint32_t length)
{
    for(uint32_t i=0;i<length;i++)if(!pack_byte(reader,destination+i))return 0;
    return 1;
}

static int bit_read(bit_reader_t *bits,uint8_t count,uint32_t *value)
{
    while(bits->bit_count<count){uint8_t byte;if(!pack_byte(bits->reader,&byte))return 0;bits->bits|=(uint32_t)byte<<bits->bit_count;bits->bit_count+=8;}
    *value=bits->bits&((1u<<count)-1);bits->bits>>=count;bits->bit_count-=count;return 1;
}

static void bit_align(bit_reader_t *bits){bits->bits=0;bits->bit_count=0;}

static int huffman_build(huffman_tree_t *tree,const uint8_t *lengths,uint16_t symbol_count)
{
    uint16_t counts[16]={0},next_code[16]={0};
    tree->count=1;
    for(uint16_t i=0;i<600;i++){tree->nodes[i].child[0]=-1;tree->nodes[i].child[1]=-1;tree->nodes[i].symbol=-1;}
    for(uint16_t i=0;i<symbol_count;i++){
        if(lengths[i]>15)return 0;
        if(lengths[i])counts[lengths[i]]++;
    }
    uint16_t code=0;
    for(uint8_t bits=1;bits<=15;bits++){
        code=(uint16_t)((code+counts[bits-1])<<1);next_code[bits]=code;
    }
    for(uint16_t symbol=0;symbol<symbol_count;symbol++){
        uint8_t length=lengths[symbol];if(!length)continue;
        uint16_t code=next_code[length]++;int node=0;
        for(uint8_t bit=0;bit<length;bit++){
            uint8_t direction=(uint8_t)((code>>(length-bit-1))&1);
            int child=tree->nodes[node].child[direction];
            if(child<0){if(tree->count>=600)return 0;child=tree->count++;tree->nodes[node].child[direction]=(int16_t)child;}
            node=child;
        }
        if(tree->nodes[node].symbol>=0)return 0;
        tree->nodes[node].symbol=(int16_t)symbol;
    }
    return tree->count>1;
}

static int huffman_symbol(bit_reader_t *bits,const huffman_tree_t *tree,uint16_t *symbol)
{
    int node=0;
    for(uint8_t length=0;length<15;length++){
        uint32_t direction;if(!bit_read(bits,1,&direction))return 0;
        node=tree->nodes[node].child[direction];if(node<0)return 0;
        if(tree->nodes[node].symbol>=0){*symbol=(uint16_t)tree->nodes[node].symbol;return 1;}
    }
    return 0;
}

static int build_fixed_trees(void)
{
    uint8_t literal_lengths[288],distance_lengths[32];
    for(uint16_t i=0;i<288;i++)literal_lengths[i]=i<=143?8:i<=255?9:i<=279?7:8;
    for(uint8_t i=0;i<32;i++)distance_lengths[i]=5;
    return huffman_build(&literal_tree,literal_lengths,288)&&huffman_build(&distance_tree,distance_lengths,32);
}

static int build_dynamic_trees(bit_reader_t *bits)
{
    static const uint8_t order[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    uint32_t value;uint16_t hlit,hdist,hclen;
    if(!bit_read(bits,5,&value))return 0;hlit=(uint16_t)(value+257);
    if(!bit_read(bits,5,&value))return 0;hdist=(uint16_t)(value+1);
    if(!bit_read(bits,4,&value))return 0;hclen=(uint16_t)(value+4);
    if(hlit>286||hdist>32)return 0;
    uint8_t code_lengths[19]={0};
    for(uint16_t i=0;i<hclen;i++){if(!bit_read(bits,3,&value))return 0;code_lengths[order[i]]=(uint8_t)value;}
    if(!huffman_build(&code_tree,code_lengths,19))return 0;
    uint8_t lengths[318]={0};uint16_t total=(uint16_t)(hlit+hdist),index=0;
    while(index<total){
        uint16_t symbol;if(!huffman_symbol(bits,&code_tree,&symbol))return 0;
        if(symbol<16){lengths[index++]=(uint8_t)symbol;continue;}
        uint32_t repeat;uint8_t repeated;
        if(symbol==16){if(!index||!bit_read(bits,2,&value))return 0;repeat=value+3;repeated=lengths[index-1];}
        else if(symbol==17){if(!bit_read(bits,3,&value))return 0;repeat=value+3;repeated=0;}
        else if(symbol==18){if(!bit_read(bits,7,&value))return 0;repeat=value+11;repeated=0;}
        else return 0;
        if(index+repeat>total)return 0;
        while(repeat--)lengths[index++]=repeated;
    }
    return huffman_build(&literal_tree,lengths,hlit)&&huffman_build(&distance_tree,lengths+hlit,hdist);
}

static uint32_t adler_update(uint32_t adler,uint8_t byte)
{
    uint32_t a=adler&0xFFFF,b=adler>>16;a=(a+byte)%65521;b=(b+a)%65521;return (b<<16)|a;
}

static int inflate_zlib(pack_reader_t *reader,uint8_t *output,uint32_t capacity,
                        uint32_t *output_length,uint32_t expected_size)
{
    uint8_t cmf,flg;
    if(!pack_byte(reader,&cmf)||!pack_byte(reader,&flg)||(cmf&15)!=8||(cmf>>4)>7||
       (((uint16_t)cmf<<8)|flg)%31||(flg&0x20))return 0;
    bit_reader_t bits={reader,0,0};uint32_t produced=0,adler=1,value;uint8_t final=0;
    static const uint16_t length_base[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint8_t length_extra[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t distance_base[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint8_t distance_extra[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    do{
        uint32_t block_type;
        if(!bit_read(&bits,1,&value))return 0;
        final=(uint8_t)value;
        if(!bit_read(&bits,2,&block_type))return 0;
        if(block_type==0){
            bit_align(&bits);uint8_t pair[4];
            if(!pack_bytes(reader,pair,4))return 0;
            uint16_t length=(uint16_t)(pair[0]|(uint16_t)pair[1]<<8),inverse=(uint16_t)(pair[2]|(uint16_t)pair[3]<<8);
            if((uint16_t)~length!=inverse||produced+length>capacity)return 0;
            for(uint16_t i=0;i<length;i++){
                uint8_t c;if(!pack_byte(reader,&c))return 0;output[produced++]=c;adler=adler_update(adler,c);
            }
            continue;
        }
        if(block_type==1){if(!build_fixed_trees())return 0;}
        else if(block_type==2){if(!build_dynamic_trees(&bits))return 0;}
        else return 0;
        for(;;){
            uint16_t symbol;if(!huffman_symbol(&bits,&literal_tree,&symbol))return 0;
            if(symbol<256){if(produced>=capacity)return 0;output[produced++]=(uint8_t)symbol;adler=adler_update(adler,(uint8_t)symbol);continue;}
            if(symbol==256)break;
            if(symbol<257||symbol>285)return 0;
            uint16_t length=length_base[symbol-257];
            if(length_extra[symbol-257]){if(!bit_read(&bits,length_extra[symbol-257],&value))return 0;length=(uint16_t)(length+value);}
            uint16_t distance_symbol;if(!huffman_symbol(&bits,&distance_tree,&distance_symbol)||distance_symbol>29)return 0;
            uint32_t distance=distance_base[distance_symbol];
            if(distance_extra[distance_symbol]){if(!bit_read(&bits,distance_extra[distance_symbol],&value))return 0;distance+=value;}
            if(!distance||distance>produced||produced+length>capacity)return 0;
            for(uint16_t i=0;i<length;i++){uint8_t c=output[produced-distance];output[produced++]=c;adler=adler_update(adler,c);}
        }
    }while(!final);
    bit_align(&bits);uint8_t trailer[4];if(!pack_bytes(reader,trailer,4))return 0;
    if(read_be32(trailer)!=adler||produced!=expected_size)return 0;
    *output_length=produced;return 1;
}

static int find_object_oid(const uint8_t oid[20],uint16_t *index)
{
    for(uint16_t i=0;i<pack_object_count;i++){
        uint8_t diff=0;for(uint8_t j=0;j<20;j++)diff|=pack_objects[i].oid[j]^oid[j];
        if(!diff){*index=i;return 1;}
    }
    return 0;
}

static int read_memory_varint(const uint8_t *data,uint32_t length,uint32_t *position,uint32_t *value);

static int apply_delta(const uint8_t *delta,uint32_t delta_length,const uint8_t *base,
                       uint32_t base_length,uint8_t *output,uint32_t capacity,uint32_t *output_length)
{
    uint32_t position=0,declared_base=0,result_size=0;
    if(!read_memory_varint(delta,delta_length,&position,&declared_base)||
       !read_memory_varint(delta,delta_length,&position,&result_size)||declared_base!=base_length||result_size>capacity)return 0;
    uint32_t written=0;
    while(position<delta_length){
        uint8_t command=delta[position++];
        if(command&0x80){
            uint32_t offset=0,length=0;
            for(uint8_t i=0;i<4;i++)if(command&(1u<<i)){if(position>=delta_length)return 0;offset|=(uint32_t)delta[position++]<<(8*i);}
            for(uint8_t i=0;i<3;i++)if(command&(0x10u<<i)){if(position>=delta_length)return 0;length|=(uint32_t)delta[position++]<<(8*i);}
            if(!length)length=0x10000;
            if(offset>base_length||length>base_length-offset||length>result_size-written)return 0;
            for(uint32_t i=0;i<length;i++)output[written++]=base[offset+i];
        }else{
            uint32_t length=command;
            if(!length||length>delta_length-position||length>result_size-written)return 0;
            for(uint32_t i=0;i<length;i++)output[written++]=delta[position++];
        }
    }
    if(written!=result_size)return 0;*output_length=written;return 1;
}

static int read_memory_varint(const uint8_t *data,uint32_t length,uint32_t *position,uint32_t *value)
{
    uint32_t result=0;uint8_t shift=0;
    for(;;){
        if(*position>=length||shift>=32)return 0;
        uint8_t byte=data[(*position)++];
        if((uint32_t)(byte&0x7F)>((0xFFFFFFFFu-result)>>shift))return 0;
        result|=(uint32_t)(byte&0x7F)<<shift;
        if(!(byte&0x80))break;
        shift+=7;
    }
    *value=result;return 1;
}

static const char *git_type_name(uint8_t type)
{
    static const char *names[5]={"","commit","tree","blob","tag"};
    return type<5?names[type]:"";
}

static uint16_t decimal_text(uint32_t value,char output[10])
{
    char reverse[10];uint16_t count=0;
    do{reverse[count++]=(char)('0'+value%10);value/=10;}while(value);
    for(uint16_t i=0;i<count;i++)output[i]=reverse[count-i-1];
    return count;
}

static int git_object_hash(uint8_t type,const uint8_t *data,uint32_t length,uint8_t oid[20])
{
    char size_text[10];uint16_t size_length=decimal_text(length,size_text);
    const char *name=git_type_name(type);uint16_t name_length=0;
    while(name[name_length])name_length++;
    uint8_t header[32];uint16_t header_length=0;
    for(uint16_t i=0;i<name_length;i++)header[header_length++]=(uint8_t)name[i];
    header[header_length++]=' ';
    for(uint16_t i=0;i<size_length;i++)header[header_length++]=(uint8_t)size_text[i];
    header[header_length++]=0;
    br_sha1_context context;br_sha1_init(&context);
    br_sha1_update(&context,header,header_length);br_sha1_update(&context,data,length);br_sha1_out(&context,oid);
    return 1;
}

static int store_raw_object(const uint8_t oid[20],const uint8_t *data,uint32_t length)
{
    char hex[41],path[64]="/.git/maios/raw_";uint16_t p=15;
    oid_to_hex(oid,hex);for(uint8_t i=0;i<40;i++)path[p++]=hex[i];path[p]=0;
    return write_file_path(path,data,length);
}

static void adler_bytes(uint32_t *adler,const uint8_t *data,uint32_t length)
{
    uint32_t value=*adler;
    for(uint32_t i=0;i<length;i++)value=adler_update(value,data[i]);
    *adler=value;
}

static int store_loose_object(const uint8_t oid[20],uint8_t type,const uint8_t *data,uint32_t length)
{
    char hex[41],directory[32]="/.git/objects/00",filename[39];
    oid_to_hex(oid,hex);directory[14]=hex[0];directory[15]=hex[1];
    for(uint8_t i=0;i<38;i++)filename[i]=hex[i+2];filename[38]=0;
    char original[1100];if(!ext4_pwd(original,sizeof(original))||!ensure_directory(directory)||!ext4_chdir(directory))return 0;
    if(!ext4_write_begin(filename)){(void)ext4_chdir(original);return 0;}
    ext4_writer_t writer={0};uint8_t zlib_header[2]={0x78,0x01};
    if(!writer_append(&writer,zlib_header,sizeof(zlib_header))){(void)ext4_write_finish(0);(void)ext4_chdir(original);return 0;}
    char size_text[10];uint16_t size_length=decimal_text(length,size_text);
    const char *name=git_type_name(type);uint16_t name_length=0;while(name[name_length])name_length++;
    uint8_t header[32];uint16_t header_length=0;
    for(uint16_t i=0;i<name_length;i++)header[header_length++]=(uint8_t)name[i];
    header[header_length++]=' ';
    for(uint16_t i=0;i<size_length;i++)header[header_length++]=(uint8_t)size_text[i];
    header[header_length++]=0;
    uint32_t canonical_length=(uint32_t)header_length+length,position=0,adler=1;
    int ok=1;
    while(position<canonical_length&&ok){
        uint16_t chunk=(uint16_t)(canonical_length-position>65535?65535:canonical_length-position);
        uint8_t deflate_header[5];uint16_t inverse=(uint16_t)~chunk;
        deflate_header[0]=(position+chunk==canonical_length)?1:0;
        deflate_header[1]=(uint8_t)chunk;deflate_header[2]=(uint8_t)(chunk>>8);
        deflate_header[3]=(uint8_t)inverse;deflate_header[4]=(uint8_t)(inverse>>8);
        ok=writer_append(&writer,deflate_header,sizeof(deflate_header));
        uint32_t block_position=position;
        while(chunk&&ok){
            const uint8_t *source;uint32_t source_offset,available;
            if(block_position<header_length){source=header;source_offset=block_position;available=header_length-block_position;}
            else {source=data;source_offset=block_position-header_length;available=length-source_offset;}
            uint16_t amount=(uint16_t)(chunk<available?chunk:available);
            adler_bytes(&adler,source+source_offset,amount);
            ok=writer_append(&writer,source+source_offset,amount);
            block_position+=amount;position+=amount;chunk-=amount;
        }
    }
    if(ok){uint8_t trailer[4];write_be32(trailer,adler);ok=writer_append(&writer,trailer,sizeof(trailer));}
    if(ok)ok=writer_finish(&writer);else (void)ext4_write_finish(writer.length);
    (void)ext4_chdir(original);return ok;
}

static int store_git_object(uint8_t type,const uint8_t *data,uint32_t length,uint8_t oid[20])
{
    if(!type||type>4||length>GIT_OBJECT_LIMIT)return 0;
    git_object_hash(type,data,length,oid);
    return store_raw_object(oid,data,length)&&store_loose_object(oid,type,data,length);
}

static int load_raw_oid(const uint8_t oid[20],uint8_t *destination,uint32_t capacity,uint32_t *length)
{
    char hex[41],path[64]="/.git/maios/raw_";uint16_t p=15;
    oid_to_hex(oid,hex);for(uint8_t i=0;i<40;i++)path[p++]=hex[i];path[p]=0;
    return read_file_path(path,destination,capacity,length);
}

static int pack_reopen(const char *filename,pack_reader_t *reader)
{
    uint32_t file_size;
    ext4_read_end();
    return ext4_read_start(filename,&file_size)&&file_size==reader->file_size;
}

static int parse_pack_objects(const char *filename,pack_reader_t *reader,uint32_t count)
{
    pack_object_count=0;
    for(uint32_t index=0;index<count;index++){
        if(pack_object_count>=GIT_PACK_OBJECT_LIMIT)return 0;
        uint32_t object_offset=reader->position;uint8_t byte;
        if(!pack_byte(reader,&byte))return 0;
        uint8_t type=(byte>>4)&7;uint32_t declared_size=byte&0x0F;uint8_t shift=4;
        while(byte&0x80){
            if(!pack_byte(reader,&byte)||shift>=32||(uint32_t)(byte&0x7F)>((0xFFFFFFFFu-declared_size)>>shift))return 0;
            declared_size|=(uint32_t)(byte&0x7F)<<shift;shift+=7;
        }
        if(declared_size>GIT_OBJECT_LIMIT)return 0;
        uint32_t base_offset=0;uint8_t base_oid[20];
        if(type==6){
            if(!pack_byte(reader,&byte))return 0;
            uint32_t distance=byte&0x7F;
            while(byte&0x80){
                if(!pack_byte(reader,&byte)||distance>0x01FFFFFFu)return 0;
                distance=((distance+1)<<7)|(byte&0x7F);
            }
            if(!distance||distance>object_offset)return 0;
            base_offset=object_offset-distance;
        }else if(type==7){if(!pack_bytes(reader,base_oid,20))return 0;}
        else if(type<1||type>4)return 0;

        uint32_t inflated_length=0;
        uint8_t *compressed_output=(type==6||type==7)?delta_data:inflated;
        if(!inflate_zlib(reader,compressed_output,GIT_OBJECT_LIMIT,&inflated_length,declared_size))return 0;
        if(type==6||type==7){
            uint16_t base_index=0;int found=0;
            if(type==6){
                for(uint16_t i=0;i<pack_object_count;i++)if(pack_objects[i].offset==base_offset){base_index=i;found=1;break;}
            }else found=find_object_oid(base_oid,&base_index);
            if(!found||!pack_reopen(filename,reader))return 0;
            git_pack_object_t *base=&pack_objects[base_index];uint32_t base_length=0;
            if(!load_raw_oid(base->oid,base_data,GIT_OBJECT_LIMIT,&base_length)||base_length!=base->size||
               !pack_reopen(filename,reader))return 0;
            uint32_t result_size=0;
            if(!apply_delta(delta_data,inflated_length,base_data,base_length,object_data,GIT_OBJECT_LIMIT,&result_size))return 0;
            type=base->type;inflated_length=result_size;compressed_output=object_data;
        }
        git_pack_object_t *object=&pack_objects[pack_object_count];
        object->offset=object_offset;object->size=inflated_length;object->type=type;
        if(!store_git_object(type,compressed_output,inflated_length,object->oid))return 0;
        pack_object_count++;
        if(index+1<count&&!pack_reopen(filename,reader))return 0;
    }
    return 1;
}

static int ensure_child_directory(const char *name)
{
    if(ext4_chdir(name))return 1;
    return ext4_mkdir(name)&&ext4_chdir(name);
}

static int checkout_tree(const uint8_t tree_oid[20],uint8_t depth)
{
    if(depth>=GIT_TREE_DEPTH)return 0;
    uint32_t tree_size=0;
    if(!load_raw_oid(tree_oid,object_data,GIT_OBJECT_LIMIT,&tree_size))return 0;
    git_tree_entry_t *entries=tree_entries[depth];uint16_t count=0;uint32_t pos=0;
    while(pos<tree_size){
        uint32_t mode=0;uint8_t digits=0;
        while(pos<tree_size&&object_data[pos]!=' '){
            if(object_data[pos]<'0'||object_data[pos]>'7'||digits++>=7)return 0;
            mode=(mode<<3)|(object_data[pos++]-'0');
        }
        if(pos>=tree_size||!digits)return 0;pos++;
        uint32_t name_start=pos;while(pos<tree_size&&object_data[pos])pos++;
        uint32_t name_length=pos-name_start;
        if(pos>=tree_size||!name_length||name_length>63||pos+21>tree_size||count>=GIT_TREE_ENTRIES)return 0;
        git_tree_entry_t *entry=&entries[count++];
        for(uint32_t i=0;i<name_length;i++)entry->name[i]=(char)object_data[name_start+i];entry->name[name_length]=0;
        entry->mode=mode;pos++;
        for(uint8_t i=0;i<20;i++)entry->oid[i]=object_data[pos++];
    }
    for(uint16_t i=0;i<count;i++){
        git_tree_entry_t *entry=&entries[i];
        if(text_equal(entry->name,".git"))continue;
        if(entry->mode==0040000u){
            if(!ensure_child_directory(entry->name))return 0;
            if(!checkout_tree(entry->oid,(uint8_t)(depth+1)))return 0;
            if(!ext4_chdir(".."))return 0;
        }else if(entry->mode==0160000u){
            continue;
        }else{
            uint32_t blob_size=0;
            if(!load_raw_oid(entry->oid,object_data,GIT_OBJECT_LIMIT,&blob_size)||!ext4_write_begin(entry->name))return 0;
            ext4_writer_t writer={0};
            if(blob_size&&!writer_append(&writer,object_data,blob_size)){(void)ext4_write_finish(writer.length);return 0;}
            if(!writer_finish(&writer))return 0;
        }
    }
    return 1;
}

static int commit_tree_oid(const uint8_t commit_oid[20],uint8_t tree_oid[20])
{
    uint32_t size=0;if(!load_raw_oid(commit_oid,object_data,GIT_OBJECT_LIMIT,&size))return 0;
    static const char prefix[]="tree ";
    for(uint32_t i=0;i+45<=size;i++){
        if(i&&!(object_data[i-1]=='\n'))continue;
        uint8_t match=1;for(uint8_t j=0;j<5;j++)if(object_data[i+j]!=(uint8_t)prefix[j])match=0;
        if(!match)continue;
        char hex[41];for(uint8_t j=0;j<40;j++)hex[j]=(char)object_data[i+5+j];hex[40]=0;
        return hex_to_oid(hex,tree_oid);
    }
    return 0;
}

static int write_ref(const uint8_t commit_oid[20],const char *reference)
{
    char oid[41],path[180];uint16_t pos=0;
    oid_to_hex(commit_oid,oid);
    const char *name=reference;
    if(text_equal(name,"HEAD"))name="main";
    uint8_t detached=0;
    if(name[0]=='r'&&name[1]=='e'&&name[2]=='f'&&name[3]=='s'&&name[4]=='/'&&
       name[5]=='t'&&name[6]=='a'&&name[7]=='g'&&name[8]=='s'&&name[9]=='/')detached=1;
    if(!detached&&name[0]=='r'&&name[1]=='e'&&name[2]=='f'&&name[3]=='s'&&name[4]=='/'&&
       name[5]=='h'&&name[6]=='e'&&name[7]=='a'&&name[8]=='d'&&name[9]=='s'&&name[10]=='/')name+=11;
    else if(detached)name+=10;
    const char *prefix=detached?"/.git/refs/tags/":"/.git/refs/heads/";
    while(prefix[pos]){path[pos]=prefix[pos];pos++;}
    if(!*name)return 0;
    uint8_t component_length=0;char component[64];
    while(*name){
        char c=*name++;
        if(c=='/'){
            if(!component_length||(component_length==1&&component[0]=='.')||
               (component_length==2&&component[0]=='.'&&component[1]=='.'))return 0;
            component_length=0;if(pos>=sizeof(path)-50)return 0;path[pos++]=c;continue;
        }
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'){
            if(pos>=sizeof(path)-50||component_length>=sizeof(component))return 0;
            path[pos++]=c;component[component_length++]=c;
        }else return 0;
    }
    if(!component_length||(component_length==1&&component[0]=='.')||
       (component_length==2&&component[0]=='.'&&component[1]=='.'))return 0;
    path[pos]=0;uint8_t content[42],head[196];for(uint8_t i=0;i<40;i++)content[i]=(uint8_t)oid[i];content[40]='\n';
    uint16_t head_length=0;
    if(detached){for(uint8_t i=0;i<40;i++)head[head_length++]=(uint8_t)oid[i];}
    else {
        static const char head_prefix[]="ref: refs/heads/";
        for(uint16_t i=0;i<sizeof(head_prefix)-1;i++)head[head_length++]=(uint8_t)head_prefix[i];
        for(uint16_t i=17;i<pos;i++)head[head_length++]=(uint8_t)path[i];
    }
    head[head_length++]='\n';
    if(!write_file_path("/.git/HEAD",head,head_length))return 0;
    return write_file_path(path,content,41);
}

int git_install_pack(const char *pack_filename,const char *commit_text,const char *reference)
{
    uint8_t commit_oid[20],tree_oid[20],header[12],trailer[20],calculated[20];
    if(!hex_to_oid(commit_text,commit_oid))return 0;
    uint32_t file_size=0;
    if(!ext4_read_start(pack_filename,&file_size)||file_size<32)return 0;
    pack_reader_t reader={0};reader.file_size=file_size;reader.hash_end=file_size-20;br_sha1_init(&reader.hash);
    if(!pack_bytes(&reader,header,sizeof(header))||header[0]!='P'||header[1]!='A'||header[2]!='C'||header[3]!='K'){
        ext4_read_end();return 0;
    }
    uint32_t version=read_be32(header+4),count=read_be32(header+8);
    if((version!=2&&version!=3)||!count||count>GIT_PACK_OBJECT_LIMIT||!parse_pack_objects(pack_filename,&reader,count)||reader.position!=reader.hash_end){
        ext4_read_end();return 0;
    }
    br_sha1_out(&reader.hash,calculated);
    if(!pack_bytes(&reader,trailer,sizeof(trailer))){ext4_read_end();return 0;}
    uint8_t difference=0;for(uint8_t i=0;i<20;i++)difference|=calculated[i]^trailer[i];
    ext4_read_end();
    uint16_t commit_index=0;
    if(difference||!find_object_oid(commit_oid,&commit_index)||pack_objects[commit_index].type!=1||
       !commit_tree_oid(commit_oid,tree_oid))return 0;
    char original[1100];if(!ext4_pwd(original,sizeof(original)))return 0;
    if(!ensure_directory("/.git/objects")||!ensure_directory("/.git/refs/heads")||!ensure_directory("/.git/maios"))return 0;
    if(!ext4_chdir(original))return 0;
    if(!checkout_tree(tree_oid,0)||!write_ref(commit_oid,reference))return 0;
    static const char config[]="[core]\n\trepositoryformatversion = 0\n\tbare = false\n";
    (void)write_file_path("/.git/config",(const uint8_t*)config,sizeof(config)-1);
    return 1;
}

