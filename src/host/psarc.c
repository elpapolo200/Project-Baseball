/* Lector de PSARC (PlayStation Archive v1.4, zlib): los WAD del juego
 * (VITA_COMPRESSED_WAD.arc y VITA_PATCH_WAD.arc) se leen directamente, sin
 * extraerlos.
 *
 * Formato (big-endian):
 *   cabecera 0x20: "PSAR", versión 1.4, "zlib", tamaño del TOC, tamaño de
 *     entrada (30), nº de entradas, tamaño de bloque, flags
 *   TOC: por entrada md5[16], primer bloque u32, tamaño u40, desplazamiento u40;
 *     después, el tamaño comprimido de cada bloque (2, 3 o 4 bytes según el
 *     tamaño de bloque). 0 = bloque entero sin comprimir.
 *   La entrada 0 es el manifiesto: los nombres de las demás, uno por línea.
 * Un bloque se guarda tal cual si comprimido no ahorra; si no, es un flujo zlib.
 *
 * Las lecturas son posicionales (OVERLAPPED), así que varios hilos pueden leer
 * del mismo archivo a la vez. Cada hilo guarda el último bloque descomprimido.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "inflate.h"
#include "psarc.h"

typedef struct { uint32_t block; uint64_t size, offset; } Entry;

typedef struct {
    HANDLE f;
    uint32_t count, block_size, nblocks;
    Entry *e;
    uint32_t *zsize;
    char **name;             /* nombres tal cual (entrada 0 = manifiesto) */
    int32_t *hash;           /* tabla abierta de índices; -1 = libre */
    uint32_t hcap;
} PsArc;

#define MAX_ARCS 4
static PsArc g_arc[MAX_ARCS];
static int g_narc;

static uint32_t be(const uint8_t *p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}
static uint64_t be40(const uint8_t *p) { return ((uint64_t)p[0] << 32) | be(p + 1, 4); }

/* Nombre normalizado: sin '/' inicial, '\' -> '/', mayúsculas. */
static void norm(const char *in, char *out, size_t max) {
    while (*in == '/' || *in == '\\') in++;
    size_t i = 0;
    for (; *in && i + 1 < max; in++) {
        char c = *in == '\\' ? '/' : *in;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c == '/' && i && out[i - 1] == '/') continue;   /* "//" */
        out[i++] = c;
    }
    out[i] = 0;
}

static uint32_t fnv(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static int read_at(HANDLE f, uint64_t off, void *dst, uint32_t len) {
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    if (!ReadFile(f, dst, len, &got, &ov)) return -1;
    return (int)got;
}

/* ------------------------------------------------------------------------- */
/* Bloques                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct { int arc; uint32_t block; uint32_t len; uint8_t *data; uint8_t *tmp; uint32_t cap; } Cache;
static __declspec(thread) Cache t_cache = { -1, 0, 0, NULL, NULL, 0 };

/* Bloque `b` (del archivo `a`) descomprimido en la caché del hilo. `want` es su
 * tamaño real (el último de una entrada puede ser más corto). */
static const uint8_t *get_block(int a, uint32_t b, uint64_t file_off, uint32_t want) {
    PsArc *ar = &g_arc[a];
    Cache *c = &t_cache;
    if (c->data && c->arc == a && c->block == b && c->len == want) return c->data;
    if (c->cap < ar->block_size) {
        free(c->data); free(c->tmp);
        c->data = (uint8_t *)malloc(ar->block_size);
        c->tmp = (uint8_t *)malloc(ar->block_size);
        c->cap = c->data && c->tmp ? ar->block_size : 0;
        if (!c->cap) return NULL;
    }
    uint32_t z = b < ar->nblocks ? ar->zsize[b] : 0;
    if (z == 0 || z == want) {                         /* sin comprimir */
        if (read_at(ar->f, file_off, c->data, want) != (int)want) return NULL;
    } else {
        if (z > c->cap || read_at(ar->f, file_off, c->tmp, z) != (int)z) return NULL;
        long n = -1;
        if (c->tmp[0] == 0x78) n = zlib_inflate(c->data, want, c->tmp, z);
        if (n != (long)want) {                         /* no era zlib: datos tal cual */
            if (z > want) return NULL;
            memcpy(c->data, c->tmp, z);
        }
    }
    c->arc = a;
    c->block = b;
    c->len = want;
    return c->data;
}

int64_t psarc_read(int a, int entry, uint64_t off, void *dst, uint64_t len) {
    if (a < 0 || a >= g_narc) return -1;
    PsArc *ar = &g_arc[a];
    if (entry < 0 || (uint32_t)entry >= ar->count) return -1;
    const Entry *e = &ar->e[entry];
    if (off >= e->size) return 0;
    if (len > e->size - off) len = e->size - off;
    uint8_t *out = (uint8_t *)dst;
    uint64_t done = 0;
    /* Recorre los bloques desde el primero: hay que sumar los tamaños
     * comprimidos para saber dónde empieza cada uno en el archivo. */
    uint64_t pos = 0, foff = e->offset;
    uint32_t b = e->block;
    while (done < len) {
        uint32_t want = (uint32_t)((e->size - pos) < ar->block_size ? (e->size - pos) : ar->block_size);
        uint32_t z = b < ar->nblocks ? ar->zsize[b] : 0;
        uint32_t stored = z ? z : ar->block_size;
        if (z == 0 && want < ar->block_size) stored = want;
        if (off + done < pos + want) {
            const uint8_t *blk = get_block(a, b, foff, want);
            if (!blk) return done ? (int64_t)done : -1;
            uint32_t from = (uint32_t)(off + done - pos);
            uint32_t n = want - from;
            if (n > len - done) n = (uint32_t)(len - done);
            memcpy(out + done, blk + from, n);
            done += n;
        }
        pos += want;
        foff += stored;
        b++;
    }
    return (int64_t)done;
}

/* ------------------------------------------------------------------------- */
/* Apertura e índice                                                          */
/* ------------------------------------------------------------------------- */

int psarc_open(const char *path) {
    if (g_narc >= MAX_ARCS) return -1;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { printf("[psarc] %s: no se pudo abrir (error %lu)\n", path, GetLastError()); return -1; }
    uint8_t h[0x20];
    if (read_at(f, 0, h, 0x20) != 0x20 || memcmp(h, "PSAR", 4) || memcmp(h + 8, "zlib", 4)) {
        printf("[psarc] %s: no es un PSARC legible (empieza por %02X %02X %02X %02X): ¿copia cifrada?\n", path, h[0], h[1], h[2], h[3]);
        CloseHandle(f);
        return -1;
    }
    uint32_t toc_len = be(h + 0x0C, 4), esize = be(h + 0x10, 4), count = be(h + 0x14, 4), bsize = be(h + 0x18, 4);
    if (toc_len < 0x20 || esize < 30 || !count || !bsize) { CloseHandle(f); return -1; }
    uint8_t *toc = (uint8_t *)malloc(toc_len);
    if (!toc || read_at(f, 0, toc, toc_len) != (int)toc_len) { free(toc); CloseHandle(f); return -1; }

    int a = g_narc;
    PsArc *ar = &g_arc[a];
    memset(ar, 0, sizeof(*ar));
    ar->f = f;
    ar->count = count;
    ar->block_size = bsize;
    ar->e = (Entry *)calloc(count, sizeof(Entry));
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = toc + 0x20 + (size_t)i * esize;
        ar->e[i].block = be(p + 16, 4);
        ar->e[i].size = be40(p + 20);
        ar->e[i].offset = be40(p + 25);
    }
    int width = bsize <= 0x10000 ? 2 : bsize <= 0x1000000 ? 3 : 4;
    size_t zstart = 0x20 + (size_t)count * esize;
    ar->nblocks = (uint32_t)((toc_len - zstart) / (size_t)width);
    ar->zsize = (uint32_t *)malloc((ar->nblocks ? ar->nblocks : 1) * sizeof(uint32_t));
    for (uint32_t i = 0; i < ar->nblocks; i++) ar->zsize[i] = be(toc + zstart + (size_t)i * width, width);
    free(toc);
    g_narc++;

    /* Manifiesto: nombres de las entradas 1..count-1. */
    uint64_t ms = ar->e[0].size;
    char *man = (char *)malloc((size_t)ms + 1);
    ar->name = (char **)calloc(count, sizeof(char *));
    if (!man || !ar->name || psarc_read(a, 0, 0, man, ms) != (int64_t)ms) {
        printf("[psarc] %s: no se pudo leer el manifiesto\n", path);
        g_narc--;
        CloseHandle(f);
        return -1;
    }
    man[ms] = 0;
    uint32_t k = 1;
    for (char *s = man; *s && k < count;) {
        char *e = s + strcspn(s, "\r\n");
        char saved = *e;
        *e = 0;
        if (*s) ar->name[k++] = s;
        if (!saved) break;
        s = e + 1;
        while (*s == '\r' || *s == '\n') s++;
    }
    /* Índice por nombre normalizado. */
    ar->hcap = 1;
    while (ar->hcap < count * 2) ar->hcap <<= 1;
    ar->hash = (int32_t *)malloc(ar->hcap * sizeof(int32_t));
    for (uint32_t i = 0; i < ar->hcap; i++) ar->hash[i] = -1;
    for (uint32_t i = 1; i < count; i++) {
        if (!ar->name[i]) continue;
        char n[512];
        norm(ar->name[i], n, sizeof(n));
        uint32_t hh = fnv(n) & (ar->hcap - 1);
        while (ar->hash[hh] >= 0) hh = (hh + 1) & (ar->hcap - 1);
        ar->hash[hh] = (int32_t)i;
    }
    printf("[psarc] %s: %u archivos, bloque %u KB\n", path, count - 1, bsize >> 10);
    return a;
}

int psarc_find(int a, const char *name) {
    if (a < 0 || a >= g_narc) return -1;
    PsArc *ar = &g_arc[a];
    char n[512], m[512];
    norm(name, n, sizeof(n));
    for (uint32_t hh = fnv(n) & (ar->hcap - 1); ar->hash[hh] >= 0; hh = (hh + 1) & (ar->hcap - 1)) {
        int32_t i = ar->hash[hh];
        norm(ar->name[i], m, sizeof(m));
        if (!strcmp(n, m)) return i;
    }
    return -1;
}

int64_t psarc_size(int a, int entry) {
    if (a < 0 || a >= g_narc || entry < 0 || (uint32_t)entry >= g_arc[a].count) return -1;
    return (int64_t)g_arc[a].e[entry].size;
}

/* ¿Hay algún archivo bajo `dir/`? */
int psarc_is_dir(int a, const char *dir) {
    if (a < 0 || a >= g_narc) return 0;
    char d[512], m[512];
    norm(dir, d, sizeof(d));
    size_t dl = strlen(d);
    if (!dl) return 1;
    for (uint32_t i = 1; i < g_arc[a].count; i++) {
        if (!g_arc[a].name[i]) continue;
        norm(g_arc[a].name[i], m, sizeof(m));
        if (!strncmp(m, d, dl) && m[dl] == '/') return 1;
    }
    return 0;
}

/* Hijos directos de `dir`, a partir del índice *it (empieza en 0). Devuelve 1 y
 * el nombre (sin ruta) en `out`, con *is_dir; 0 al terminar. Los
 * subdirectorios salen una vez por cada archivo que contienen: el que llama
 * deduplica si le importa. */
int psarc_dir_next(int a, const char *dir, uint32_t *it, char *out, size_t max, int *is_dir) {
    if (a < 0 || a >= g_narc) return 0;
    char d[512], m[512];
    norm(dir, d, sizeof(d));
    size_t dl = strlen(d);
    for (uint32_t i = *it + 1; i < g_arc[a].count; i++) {
        if (!g_arc[a].name[i]) continue;
        norm(g_arc[a].name[i], m, sizeof(m));
        const char *rest;
        if (!dl) rest = m;
        else if (!strncmp(m, d, dl) && m[dl] == '/') rest = m + dl + 1;
        else continue;
        const char *slash = strchr(rest, '/');
        size_t n = slash ? (size_t)(slash - rest) : strlen(rest);
        /* el nombre original, con sus mayúsculas: misma longitud que el normalizado */
        const char *orig = g_arc[a].name[i];
        while (*orig == '/' || *orig == '\\') orig++;
        size_t skip = (size_t)(rest - m);
        snprintf(out, max, "%.*s", (int)n, orig + skip);
        *is_dir = slash != NULL;
        *it = i;
        return 1;
    }
    return 0;
}

/* Recorrido plano de todas las entradas (texturas_hd.c). */
int psarc_count(int a) { return a >= 0 && a < g_narc ? (int)g_arc[a].count : 0; }
const char *psarc_name(int a, int i) { return a >= 0 && a < g_narc && i >= 0 && (uint32_t)i < g_arc[a].count ? g_arc[a].name[i] : NULL; }
