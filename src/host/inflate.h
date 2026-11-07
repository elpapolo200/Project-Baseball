/* Descompresor DEFLATE / zlib (inflate.c). Devuelven los bytes escritos o -1. */
#ifndef PORT_INFLATE_H
#define PORT_INFLATE_H

#include <stddef.h>

long raw_inflate(void *dst, size_t dst_len, const void *src, size_t src_len);
long zlib_inflate(void *dst, size_t dst_len, const void *src, size_t src_len);   /* con cabecera de 2 bytes */

#endif
