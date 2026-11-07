/* Descompresor DEFLATE / zlib (RFC 1950/1951), propio y sin dependencias.
 *
 * Lo usan el lector de PSARC (cada bloque de los WAD es un flujo zlib) y la
 * carga del eboot.bin cuando sus segmentos vienen comprimidos. Prima la
 * sencillez: decodificación de Huffman canónica por longitudes, como "puff" de
 * zlib, más que suficiente para bloques de 64 KB.
 */
#include <stdint.h>
#include <string.h>

#include "inflate.h"

typedef struct {
    const uint8_t *in;
    size_t in_len, in_pos;
    uint32_t bitbuf, bitcnt;
    uint8_t *out;
    size_t out_len, out_pos;
} St;

#define MAXBITS 15

typedef struct {
    uint16_t count[MAXBITS + 1];   /* cuántos códigos de cada longitud */
    uint16_t symbol[320];          /* símbolos ordenados por código */
} Huff;

static int bits(St *s, int need, uint32_t *v) {
    uint32_t b = s->bitbuf;
    while (s->bitcnt < (uint32_t)need) {
        if (s->in_pos >= s->in_len) return -1;
        b |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    *v = b & ((1u << need) - 1u);
    s->bitbuf = b >> need;
    s->bitcnt -= (uint32_t)need;
    return 0;
}

static int decode(St *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        uint32_t b;
        if (bits(s, 1, &b)) return -1;
        code |= (int)b;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -2;   /* código imposible */
}

/* Construye la tabla. Devuelve 0 si es completa, >0 si incompleta (válido
 * para un solo código de distancia), <0 si está sobresuscrita. */
static int construct(Huff *h, const uint8_t *length, int n) {
    uint16_t offs[MAXBITS + 1];
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++) h->count[length[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (length[i]) h->symbol[offs[length[i]]++] = (uint16_t)i;
    return left;
}

static const uint16_t LBASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t  LEXT[29]  = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t DBASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                    8193, 12289, 16385, 24577 };
static const uint8_t  DEXT[30]  = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(St *s, const Huff *lc, const Huff *dc) {
    for (;;) {
        int sym = decode(s, lc);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->out_pos >= s->out_len) return -2;
            s->out[s->out_pos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            uint32_t e;
            if (bits(s, LEXT[sym], &e)) return -1;
            size_t len = LBASE[sym] + e;
            int ds = decode(s, dc);
            if (ds < 0 || ds >= 30) return -1;
            if (bits(s, DEXT[ds], &e)) return -1;
            size_t dist = DBASE[ds] + e;
            if (dist > s->out_pos) return -1;
            if (s->out_pos + len > s->out_len) return -2;
            uint8_t *o = s->out + s->out_pos;
            for (size_t i = 0; i < len; i++) o[i] = o[(ptrdiff_t)i - (ptrdiff_t)dist];
            s->out_pos += len;
        }
    }
}

static int stored(St *s) {
    s->bitbuf = 0;                  /* al límite de byte */
    s->bitcnt = 0;
    if (s->in_pos + 4 > s->in_len) return -1;
    unsigned len = s->in[s->in_pos] | (s->in[s->in_pos + 1] << 8);
    unsigned nlen = s->in[s->in_pos + 2] | (s->in[s->in_pos + 3] << 8);
    s->in_pos += 4;
    if (len != (~nlen & 0xFFFFu)) return -1;
    if (s->in_pos + len > s->in_len) return -1;
    if (s->out_pos + len > s->out_len) return -2;
    memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
    s->in_pos += len;
    s->out_pos += len;
    return 0;
}

static int fixed(St *s) {
    static Huff lc, dc;
    static int built;
    if (!built) {
        uint8_t l[320];
        int i;
        for (i = 0; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        construct(&lc, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        construct(&dc, l, 30);
        built = 1;     /* carrera benigna: dos hilos construirían lo mismo */
    }
    return codes(s, &lc, &dc);
}

static int dynamic(St *s) {
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint8_t lengths[320];
    Huff lc, dc;
    uint32_t v;
    if (bits(s, 5, &v)) return -1;
    int nlen = (int)v + 257;
    if (bits(s, 5, &v)) return -1;
    int ndist = (int)v + 1;
    if (bits(s, 4, &v)) return -1;
    int ncode = (int)v + 4;
    if (nlen > 286 || ndist > 30) return -1;
    int i;
    for (i = 0; i < ncode; i++) { if (bits(s, 3, &v)) return -1; lengths[order[i]] = (uint8_t)v; }
    for (; i < 19; i++) lengths[order[i]] = 0;
    if (construct(&lc, lengths, 19) != 0) return -1;
    i = 0;
    while (i < nlen + ndist) {
        int sym = decode(s, &lc);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
        } else {
            uint8_t len = 0;
            int rep;
            if (sym == 16) {
                if (i == 0) return -1;
                len = lengths[i - 1];
                if (bits(s, 2, &v)) return -1;
                rep = 3 + (int)v;
            } else if (sym == 17) {
                if (bits(s, 3, &v)) return -1;
                rep = 3 + (int)v;
            } else {
                if (bits(s, 7, &v)) return -1;
                rep = 11 + (int)v;
            }
            if (i + rep > nlen + ndist) return -1;
            while (rep--) lengths[i++] = len;
        }
    }
    if (lengths[256] == 0) return -1;
    int err = construct(&lc, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lc.count[0] != 1)) return -1;
    err = construct(&dc, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - dc.count[0] != 1)) return -1;
    return codes(s, &lc, &dc);
}

long raw_inflate(void *dst, size_t dst_len, const void *src, size_t src_len) {
    St s;
    memset(&s, 0, sizeof(s));
    s.in = (const uint8_t *)src;
    s.in_len = src_len;
    s.out = (uint8_t *)dst;
    s.out_len = dst_len;
    uint32_t last, type;
    do {
        if (bits(&s, 1, &last) || bits(&s, 2, &type)) return -1;
        int err = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
        if (err) return -1;
    } while (!last);
    return (long)s.out_pos;
}

long zlib_inflate(void *dst, size_t dst_len, const void *src, size_t src_len) {
    const uint8_t *p = (const uint8_t *)src;
    if (src_len < 2 || (p[0] & 0x0F) != 8 || ((p[0] << 8) | p[1]) % 31 != 0 || (p[1] & 0x20)) return -1;
    return raw_inflate(dst, dst_len, p + 2, src_len - 2);
}
