/* Lector de PSARC (psarc.c): los WAD del juego sin extraer. */
#ifndef PORT_PSARC_H
#define PORT_PSARC_H

#include <stddef.h>
#include <stdint.h>

int     psarc_open(const char *path);                 /* índice del archivo o -1 */
int     psarc_find(int arc, const char *name);        /* entrada o -1; sin distinguir mayúsculas */
int64_t psarc_size(int arc, int entry);
int64_t psarc_read(int arc, int entry, uint64_t off, void *dst, uint64_t len);
int     psarc_is_dir(int arc, const char *dir);
int     psarc_dir_next(int arc, const char *dir, uint32_t *it, char *out, size_t max, int *is_dir);
int     psarc_count(int arc);                     /* entradas (para recorrerlas todas) */
const char *psarc_name(int arc, int entry);       /* nombre original o NULL */

#endif
