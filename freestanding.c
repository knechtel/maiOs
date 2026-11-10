#include <stddef.h>
#include <stdint.h>

#define MAIOS_HEAP_SIZE (16u * 1024u * 1024u)
#define MAIOS_HEAP_ALIGNMENT 16u
#define MAIOS_HEAP_MAGIC 0x4D41494Fu
#define MAIOS_ENOMEM 12

int errno;

typedef struct heap_block {
    size_t size;
    struct heap_block *next;
    uint32_t magic;
    uint8_t free;
    uint8_t reserved[3];
} heap_block_t;

_Static_assert(sizeof(heap_block_t) == MAIOS_HEAP_ALIGNMENT,
               "heap block header must stay 16-byte aligned");

static uint8_t maios_heap[MAIOS_HEAP_SIZE] __attribute__((aligned(MAIOS_HEAP_ALIGNMENT)));
static heap_block_t *maios_heap_first;

static size_t heap_align(size_t size)
{
    if (size > (size_t)-1 - (MAIOS_HEAP_ALIGNMENT - 1)) {
        return 0;
    }
    return (size + MAIOS_HEAP_ALIGNMENT - 1) & ~(size_t)(MAIOS_HEAP_ALIGNMENT - 1);
}

static void heap_initialize(void)
{
    maios_heap_first = (heap_block_t *)maios_heap;
    maios_heap_first->size = sizeof(maios_heap) - sizeof(heap_block_t);
    maios_heap_first->next = NULL;
    maios_heap_first->magic = MAIOS_HEAP_MAGIC;
    maios_heap_first->free = 1;
}

static heap_block_t *heap_find_block(const void *pointer)
{
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t begin = (uintptr_t)maios_heap;
    uintptr_t end = begin + sizeof(maios_heap);
    heap_block_t *block;

    if (address < begin + sizeof(heap_block_t) || address >= end ||
        (address & (MAIOS_HEAP_ALIGNMENT - 1)) != 0) {
        return NULL;
    }
    block = (heap_block_t *)pointer - 1;
    if ((uintptr_t)block < begin || block->magic != MAIOS_HEAP_MAGIC ||
        (uint8_t *)(block + 1) != (const uint8_t *)pointer) {
        return NULL;
    }
    return block;
}

void *malloc(size_t size)
{
    heap_block_t *block;
    size_t needed;

    if (size == 0) {
        size = 1;
    }
    needed = heap_align(size);
    if (needed == 0 || needed > sizeof(maios_heap) - sizeof(heap_block_t)) {
        errno = MAIOS_ENOMEM;
        return NULL;
    }
    if (maios_heap_first == NULL) {
        heap_initialize();
    }

    for (block = maios_heap_first; block != NULL; block = block->next) {
        if (!block->free || block->size < needed) {
            continue;
        }
        if (block->size - needed >= sizeof(heap_block_t) + MAIOS_HEAP_ALIGNMENT) {
            heap_block_t *remainder = (heap_block_t *)((uint8_t *)(block + 1) + needed);
            remainder->size = block->size - needed - sizeof(heap_block_t);
            remainder->next = block->next;
            remainder->magic = MAIOS_HEAP_MAGIC;
            remainder->free = 1;
            block->next = remainder;
            block->size = needed;
        }
        block->free = 0;
        return block + 1;
    }
    errno = MAIOS_ENOMEM;
    return NULL;
}

void free(void *pointer)
{
    heap_block_t *block;
    heap_block_t *previous = NULL;

    if (pointer == NULL || maios_heap_first == NULL) {
        return;
    }
    block = heap_find_block(pointer);
    if (block == NULL || block->free) {
        return;
    }
    block->free = 1;

    for (heap_block_t *current = maios_heap_first; current != NULL && current != block;
         current = current->next) {
        previous = current;
    }
    if (previous != NULL && previous->free) {
        previous->size += sizeof(heap_block_t) + block->size;
        previous->next = block->next;
        block->magic = 0;
        block = previous;
    }
    if (block->next != NULL && block->next->free) {
        heap_block_t *next = block->next;
        block->size += sizeof(heap_block_t) + next->size;
        block->next = next->next;
        next->magic = 0;
    }
}

void *calloc(size_t count, size_t size)
{
    size_t total;
    uint8_t *memory;

    if (size != 0 && count > (size_t)-1 / size) {
        return NULL;
    }
    total = count * size;
    memory = malloc(total);
    if (memory != NULL) {
        for (size_t i = 0; i < total; ++i) {
            memory[i] = 0;
        }
    }
    return memory;
}

void *realloc(void *pointer, size_t size)
{
    heap_block_t *block;
    void *replacement;
    size_t copy_size;

    if (pointer == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(pointer);
        return NULL;
    }
    block = heap_find_block(pointer);
    if (block == NULL || block->free) {
        return NULL;
    }
    if (size <= block->size) {
        return pointer;
    }
    replacement = malloc(size);
    if (replacement == NULL) {
        return NULL;
    }
    copy_size = block->size;
    for (size_t i = 0; i < copy_size; ++i) {
        ((uint8_t *)replacement)[i] = ((const uint8_t *)pointer)[i];
    }
    free(pointer);
    return replacement;
}

void *memcpy(void *destination,const void *source,size_t length)
{
    unsigned char *d=destination;const unsigned char *s=source;
    for(size_t i=0;i<length;i++)d[i]=s[i];
    return destination;
}

void *memmove(void *destination,const void *source,size_t length)
{
    unsigned char *d=destination;const unsigned char *s=source;
    if(d<s){for(size_t i=0;i<length;i++)d[i]=s[i];}
    else if(d>s){while(length){length--;d[length]=s[length];}}
    return destination;
}

void *memset(void *destination,int value,size_t length)
{
    unsigned char *d=destination;
    for(size_t i=0;i<length;i++)d[i]=(unsigned char)value;
    return destination;
}

int memcmp(const void *left,const void *right,size_t length)
{
    const unsigned char *a=left,*b=right;
    for(size_t i=0;i<length;i++)if(a[i]!=b[i])return (int)a[i]-(int)b[i];
    return 0;
}

size_t strlen(const char *text)
{
    size_t length=0;while(text[length])length++;return length;
}

size_t strnlen(const char *text,size_t limit)
{
    size_t length=0;while(length<limit&&text[length])length++;return length;
}

int strcmp(const char *left,const char *right)
{
    while(*left&&*left==*right){left++;right++;}
    return (unsigned char)*left-(unsigned char)*right;
}

int strncmp(const char *left,const char *right,size_t limit)
{
    while(limit&&*left&&*left==*right){left++;right++;limit--;}
    return limit?(unsigned char)*left-(unsigned char)*right:0;
}

char *strcpy(char *destination,const char *source)
{
    char *result=destination;while((*destination++=*source++)){}return result;
}

char *strncpy(char *destination,const char *source,size_t limit)
{
    char *result=destination;size_t i=0;
    for(;i<limit&&source[i];i++)destination[i]=source[i];
    for(;i<limit;i++)destination[i]=0;
    return result;
}

char *strchr(const char *text,int value)
{
    while(*text){if((unsigned char)*text==(unsigned char)value)return (char *)text;text++;}
    return value==0?(char *)text:0;
}

char *strrchr(const char *text,int value)
{
    const char *found=0;
    do{if((unsigned char)*text==(unsigned char)value)found=text;}while(*text++);
    return (char *)found;
}

char *strstr(const char *text,const char *pattern)
{
    if(!*pattern)return (char *)text;
    for(;*text;text++){
        const char *a=text,*b=pattern;
        while(*a&&*b&&*a==*b){a++;b++;}
        if(!*b)return (char *)text;
    }
    return 0;
}

void *memchr(const void *memory,int value,size_t length)
{
    const unsigned char *data=memory;
    for(size_t i=0;i<length;i++)if(data[i]==(unsigned char)value)return (void *)(data+i);
    return 0;
}
