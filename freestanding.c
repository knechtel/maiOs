#include <stddef.h>

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
