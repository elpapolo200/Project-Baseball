/* Decodificación de texturas GXM a RGBA8 (en la CPU).
 *
 * Formato GXM = formato base (bits 31-24, con el bit 31 para los comprimidos y
 * paletas) | swizzle de componentes (bits 15-12). Tipos de almacenamiento:
 * swizzled (orden Morton, el mismo que tools/tp2png.py y comprobado con las
 * texturas del juego), lineal, lineal con stride y tiled (teselas de 32x32
 * seguidas y en filas, como Vita3K; solo formatos sin comprimir).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vitarecomp/mem.h"

#include "gpu.h"

/* --- orden swizzled ------------------------------------------------------- */

static uint32_t compact1by1(uint32_t v) {
    v &= 0x55555555u;
    v = (v ^ (v >> 1)) & 0x33333333u;
    v = (v ^ (v >> 2)) & 0x0F0F0F0Fu;
    v = (v ^ (v >> 4)) & 0x00FF00FFu;
    v = (v ^ (v >> 8)) & 0x0000FFFFu;
    return v;
}

static int is_pow2(uint32_t v) { return v && !(v & (v - 1)); }

/* MLB15_TILED=0: las tiled como lineales (como antes). */
static int tiled_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("MLB15_TILED"); on = e ? (*e != '0') : 1; }
    return on;
}

/* Índice lineal (y*w + x) del elemento i del almacenamiento swizzled. */
static uint32_t unswizzle(uint32_t i, uint32_t w, uint32_t h) {
    uint32_t mn = w < h ? w : h, k = 0;
    while ((1u << (k + 1)) <= mn) k++;
    uint32_t mx = compact1by1(i), my = compact1by1(i >> 1);
    uint32_t high = (i >> (2 * k)) << (2 * k);
    uint32_t x, y;
    if (h < w) {
        uint32_t j = high | ((my & (mn - 1)) << k) | (mx & (mn - 1));
        x = j / h; y = j % h;
    } else {
        uint32_t j = high | ((mx & (mn - 1)) << k) | (my & (mn - 1));
        x = j % w; y = j / w;
    }
    return y * w + x;
}

/* --- bloques BCn (DXT) ---------------------------------------------------- */

static void rgb565(uint16_t c, uint8_t out[4]) {
    out[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
    out[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
    out[2] = (uint8_t)((c & 31) * 255 / 31);
    out[3] = 255;
}

/* Decodifica un bloque de color DXT1 en 16 píxeles RGBA. `dxt1` permite el modo
 * de 3 colores + transparente (solo en BC1). */
static void bc1_block(const uint8_t *b, uint8_t px[16][4], int dxt1) {
    uint16_t c0 = (uint16_t)(b[0] | (b[1] << 8)), c1 = (uint16_t)(b[2] | (b[3] << 8));
    uint8_t pal[4][4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    if (c0 > c1 || !dxt1) {
        for (int k = 0; k < 3; k++) {
            pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
    uint32_t bits = (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    for (int i = 0; i < 16; i++) memcpy(px[i], pal[(bits >> (2 * i)) & 3], 4);
}

static void bc3_alpha(const uint8_t *b, uint8_t px[16][4]) {
    uint8_t a[8];
    a[0] = b[0]; a[1] = b[1];
    if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1]) / 7);
    else {
        for (int i = 1; i < 5; i++) a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1]) / 5);
        a[6] = 0; a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) px[i][3] = a[(bits >> (3 * i)) & 7];
}

static void bc2_alpha(const uint8_t *b, uint8_t px[16][4]) {
    for (int i = 0; i < 16; i++) {
        int v = (b[i / 2] >> ((i & 1) * 4)) & 15;
        px[i][3] = (uint8_t)(v * 17);
    }
}

/* --- formatos sin comprimir ------------------------------------------------ */

/* Reordena un píxel de 4 componentes según el swizzle GXM a RGBA. `c` viene en
 * el orden de memoria del formato base U8U8U8U8 (byte 0 = componente baja). */
static void swz4(uint32_t swz, const uint8_t c[4], uint8_t o[4]) {
    switch (swz) {
        case 0x0: o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = c[3]; break;   /* ABGR */
        case 0x1: o[0] = c[2]; o[1] = c[1]; o[2] = c[0]; o[3] = c[3]; break;   /* ARGB */
        case 0x2: o[0] = c[3]; o[1] = c[2]; o[2] = c[1]; o[3] = c[0]; break;   /* RGBA */
        case 0x3: o[0] = c[1]; o[1] = c[2]; o[2] = c[3]; o[3] = c[0]; break;   /* BGRA */
        case 0x4: o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = 255; break;    /* 1BGR */
        case 0x5: o[0] = c[2]; o[1] = c[1]; o[2] = c[0]; o[3] = 255; break;    /* 1RGB */
        case 0x6: o[0] = c[3]; o[1] = c[2]; o[2] = c[1]; o[3] = 255; break;    /* RGB1 */
        default:  o[0] = c[1]; o[1] = c[2]; o[2] = c[3]; o[3] = 255; break;    /* BGR1 */
    }
}

static void swz1(uint32_t swz, uint8_t v, uint8_t o[4]) {
    switch (swz) {
        case 0x0: o[0] = v; o[1] = 0; o[2] = 0; o[3] = 255; break;        /* R    */
        case 0x1: o[0] = 0; o[1] = 0; o[2] = 0; o[3] = v; break;          /* 000R */
        case 0x2: o[0] = 255; o[1] = 255; o[2] = 255; o[3] = v; break;    /* 111R */
        case 0x3: o[0] = v; o[1] = v; o[2] = v; o[3] = v; break;          /* RRRR */
        case 0x4: o[0] = 0; o[1] = v; o[2] = v; o[3] = v; break;          /* 0RRR */
        case 0x5: o[0] = 255; o[1] = v; o[2] = v; o[3] = v; break;        /* 1RRR */
        case 0x6: o[0] = v; o[1] = 0; o[2] = 0; o[3] = 0; break;          /* R000 */
        default:  o[0] = v; o[1] = 255; o[2] = 255; o[3] = 255; break;    /* R111 */
    }
}

static void swz2(uint32_t swz, uint8_t r, uint8_t g, uint8_t o[4]) {
    switch (swz) {
        case 0x0: o[0] = r; o[1] = g; o[2] = 0; o[3] = 255; break;   /* GR   */
        case 0x1: o[0] = 0; o[1] = 0; o[2] = r; o[3] = g; break;     /* 00GR */
        case 0x2: o[0] = r; o[1] = r; o[2] = r; o[3] = g; break;     /* GRRR */
        case 0x3: o[0] = g; o[1] = g; o[2] = g; o[3] = r; break;     /* RGGG */
        case 0x4: o[0] = r; o[1] = g; o[2] = r; o[3] = g; break;     /* GRGR */
        default:  o[0] = 0; o[1] = 0; o[2] = g; o[3] = r; break;     /* 00RG */
    }
}

static int bpp_of(uint32_t base) {               /* bits por píxel; 0 = no soportado */
    switch (base) {
        case 0x00: case 0x01: case 0x95: return 8;          /* U8, S8, P8 */
        case 0x94: return 4;                                /* P4 */
        case 0x02: case 0x03: case 0x04: case 0x05:
        case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: return 16;
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11:
        case 0x12: case 0x17: case 0x18: return 32;
        case 0x98: return 24;                               /* U8U8U8 */
        case 0x1B: return 64;                               /* F16F16F16F16 (tex_decode_f16) */
        default: return 0;
    }
}

static void unsupported(uint32_t fmt) {
    static uint32_t told[64];
    for (int i = 0; i < 64; i++) {
        if (told[i] == fmt) return;
        if (!told[i]) { told[i] = fmt; printf("[tex] formato GXM 0x%08X sin decodificar (se pinta magenta)\n", fmt); fflush(stdout); return; }
    }
}

uint32_t tex_data_size(const GpuTexture *t) {
    uint32_t base = t->format >> 24, w = t->width, h = t->height;
    if (base >= 0x85 && base <= 0x87) {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        return bw * bh * (base == 0x85 ? 8u : 16u);
    }
    int bpp = bpp_of(base);
    if (!bpp) return 0;
    if (t->type == GPU_TEX_TILED && tiled_on()) return ((w + 31) & ~31u) * ((h + 31) & ~31u) * (uint32_t)bpp / 8;
    uint32_t row = t->type == GPU_TEX_LINEAR_STRIDED && t->stride ? t->stride : (w * (uint32_t)bpp + 7) / 8;
    return row * h;
}

/* F16F16F16F16 a RGBA en medio-flotantes (8 bytes por texel), sin pasar a 8 bits: el
 * juego guarda datos en ellas, no colores. La camiseta lee de una de 512x1 por jugador
 * la colocación de cada letra del apellido y cada cifra del dorsal (atlas de letras y
 * números en las unidades 1 y 2); sin esto no salían. */
int tex_decode_f16(const GpuTexture *t, uint16_t *out) {
    uint32_t swz = (t->format >> 12) & 0xF, w = t->width, h = t->height;
    uint32_t size = tex_data_size(t);
    const uint8_t *src = size ? (const uint8_t *)vita_mem_ptr(t->data, size) : NULL;
    int swizzled = (t->type == GPU_TEX_SWIZZLED || t->type == GPU_TEX_CUBE) && is_pow2(w) && is_pow2(h);
    uint32_t row = t->type == GPU_TEX_LINEAR_STRIDED && t->stride ? t->stride : w * 8u;
    if (!src) { memset(out, 0, (size_t)w * h * 8); return 0; }
    for (uint32_t n = 0; n < w * h; n++) {
        uint32_t lin = swizzled ? unswizzle(n, w, h) : n, x = lin % w, y = lin / w;
        const uint8_t *p;
        if (swizzled) p = src + (size_t)n * 8;
        else if (t->type == GPU_TEX_TILED && tiled_on()) {
            uint32_t tile = (x >> 5) + (y >> 5) * ((w + 31) >> 5), in = (x & 31) | ((y & 31) << 5);
            p = src + ((size_t)tile * 1024 + in) * 8;
        } else p = src + (size_t)y * row + (size_t)x * 8;
        uint16_t c[4], *o = out + (size_t)lin * 4;
        memcpy(c, p, 8);
        switch (swz) {   /* como swz4: c = componentes en memoria */
            case 0x1: o[0] = c[2]; o[1] = c[1]; o[2] = c[0]; o[3] = c[3]; break;   /* ARGB */
            case 0x2: o[0] = c[3]; o[1] = c[2]; o[2] = c[1]; o[3] = c[0]; break;   /* RGBA */
            case 0x3: o[0] = c[1]; o[1] = c[2]; o[2] = c[3]; o[3] = c[0]; break;   /* BGRA */
            case 0x4: o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = 0x3C00; break; /* 1BGR */
            case 0x5: o[0] = c[2]; o[1] = c[1]; o[2] = c[0]; o[3] = 0x3C00; break; /* 1RGB */
            default:  o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = c[3]; break;   /* ABGR */
        }
    }
    return 1;
}

int tex_decode(const GpuTexture *t, uint32_t *out) {
    uint32_t base = t->format >> 24, swz = (t->format >> 12) & 0xF;
    uint32_t w = t->width, h = t->height;
    uint32_t size = tex_data_size(t);
    const uint8_t *src = size ? (const uint8_t *)vita_mem_ptr(t->data, size) : NULL;
    int swizzled = (t->type == GPU_TEX_SWIZZLED || t->type == GPU_TEX_CUBE) && is_pow2(w) && is_pow2(h);
    if (!src) {
        if (size) unsupported(t->format);
        for (uint32_t i = 0; i < w * h; i++) out[i] = 0xFFFF00FFu;
        if (!size) unsupported(t->format);
        return 0;
    }

    if (base >= 0x85 && base <= 0x87) {                       /* UBC1 / UBC2 / UBC3 */
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bsz = base == 0x85 ? 8u : 16u;
        int sw = swizzled && is_pow2(bw) && is_pow2(bh) && bw > 1 && bh > 1;
        for (uint32_t n = 0; n < bw * bh; n++) {
            const uint8_t *b = src + n * bsz;
            uint8_t px[16][4];
            if (base == 0x85) bc1_block(b, px, 1);
            else {
                bc1_block(b + 8, px, 0);
                if (base == 0x86) bc2_alpha(b, px); else bc3_alpha(b, px);
            }
            uint32_t lin = sw ? unswizzle(n, bw, bh) : n;
            uint32_t bx = (lin % bw) * 4, by = (lin / bw) * 4;
            for (int k = 0; k < 16; k++) {
                uint32_t x = bx + (uint32_t)(k & 3), y = by + (uint32_t)(k >> 2);
                if (x < w && y < h)
                    out[y * w + x] = (uint32_t)px[k][0] | ((uint32_t)px[k][1] << 8) |
                                     ((uint32_t)px[k][2] << 16) | ((uint32_t)px[k][3] << 24);
            }
        }
        return 1;
    }

    int bpp = bpp_of(base);
    uint32_t row = t->type == GPU_TEX_LINEAR_STRIDED && t->stride ? t->stride : (w * (uint32_t)bpp + 7) / 8;
    const uint8_t *pal = NULL;
    if (base == 0x95 || base == 0x94) pal = (const uint8_t *)vita_mem_ptr(t->palette, base == 0x95 ? 1024 : 64);
    for (uint32_t n = 0; n < w * h; n++) {
        uint32_t lin = swizzled ? unswizzle(n, w, h) : n;
        const uint8_t *p;
        uint32_t x = lin % w, y = lin / w;
        if (swizzled) p = src + (size_t)n * (uint32_t)bpp / 8;
        else if (t->type == GPU_TEX_TILED && tiled_on()) {   /* teselas de 32x32 seguidas, en filas (como Vita3K) */
            uint32_t tile = (x >> 5) + (y >> 5) * ((w + 31) >> 5), in = (x & 31) | ((y & 31) << 5);
            p = src + ((size_t)tile * 1024 + in) * (uint32_t)bpp / 8;
        }
        else          p = src + (size_t)y * row + (size_t)x * (uint32_t)bpp / 8;
        uint8_t o[4] = { 255, 0, 255, 255 };
        switch (base) {
            case 0x0C: swz4(swz, p, o); break;                               /* U8U8U8U8 */
            case 0x00: swz1(swz, p[0], o); break;                            /* U8       */
            case 0x07: swz2(swz, p[0], p[1], o); break;                      /* U8U8     */
            case 0x05: {                                                     /* U5U6U5   */
                uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
                uint8_t c[4];
                c[0] = (uint8_t)((v & 31) * 255 / 31);
                c[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
                c[2] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
                c[3] = 255;
                if (swz == 0x0 || swz == 0x4) { o[0] = c[2]; o[1] = c[1]; o[2] = c[0]; }   /* BGR */
                else { o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; }
                o[3] = 255;
                break;
            }
            case 0x02: {                                                     /* U4U4U4U4 */
                uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
                uint8_t c[4] = { (uint8_t)((v & 15) * 17), (uint8_t)(((v >> 4) & 15) * 17),
                                 (uint8_t)(((v >> 8) & 15) * 17), (uint8_t)(((v >> 12) & 15) * 17) };
                swz4(swz, c, o);
                break;
            }
            case 0x04: {                                                     /* U1U5U5U5 */
                uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
                uint8_t c[4] = { (uint8_t)((v & 31) * 255 / 31), (uint8_t)(((v >> 5) & 31) * 255 / 31),
                                 (uint8_t)(((v >> 10) & 31) * 255 / 31), (uint8_t)((v >> 15) ? 255 : 0) };
                swz4(swz, c, o);
                break;
            }
            case 0x98: { uint8_t c[4] = { p[0], p[1], p[2], 255 }; swz4(swz == 0 ? 0x4 : 0x5, c, o); break; }
            case 0x95: case 0x94: {                                          /* P8 / P4 */
                uint32_t ix = base == 0x95 ? p[0] : ((swizzled ? n : x) & 1 ? (p[0] >> 4) : (p[0] & 15));
                if (pal) { uint8_t c[4]; memcpy(c, pal + ix * 4, 4); swz4(swz, c, o); }
                break;
            }
            default:
                unsupported(t->format);
                break;
        }
        out[y * w + x] = (uint32_t)o[0] | ((uint32_t)o[1] << 8) | ((uint32_t)o[2] << 16) | ((uint32_t)o[3] << 24);
    }
    return 1;
}
