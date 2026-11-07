#ifndef MLB15_SHIM_STRING_H
#define MLB15_SHIM_STRING_H
#include <stddef.h>
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);
#define memcpy(d, s, n) __builtin_memcpy(d, s, n)
#endif
