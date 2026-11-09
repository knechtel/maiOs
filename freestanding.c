#include <stddef.h>

int errno;

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
