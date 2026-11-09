#ifndef MAIOS_FREESTANDING_STRING_H
#define MAIOS_FREESTANDING_STRING_H
#include <stddef.h>
void *memcpy(void *dst,const void *src,size_t length);
void *memmove(void *dst,const void *src,size_t length);
void *memset(void *dst,int value,size_t length);
int memcmp(const void *left,const void *right,size_t length);
size_t strlen(const char *text);
#endif
