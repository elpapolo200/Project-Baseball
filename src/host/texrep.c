/* Texturas mejoradas (reemplazo por contenido) para el port de MLB 15.
 *
 * Cada textura del juego se reconoce por una huella de sus bytes originales (el primer
 * nivel tal como está en la memoria guest, igual que en el .tp2 del WAD): nombre
 * <ancho>x<alto>_<formato>_<crc32>.png. Si en la carpeta texturas\ (junto al exe) hay un
 * PNG con ese nombre, se sube ese en lugar del original, al tamaño que tenga (las UV van
 * normalizadas). Las genera tools\texturas_hd.py (reescalado con Real-ESRGAN) a partir
 * de los .tp2 del juego; no se distribuyen: cada usuario las crea con su copia.
 *
 * MLB15.ini [video] texturas_hd = 1 (MLB15_TEXTURAS_HD manda); MLB15_TEXTURAS_DIR cambia
 * la carpeta. MLB15_TEX_VOLCAR=<carpeta> guarda cada textura que se sube, ya decodificada,
 * con su nombre de huella (para comprobar que coincide con lo que genera la herramienta).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vitarecomp/mem.h"

#include "gpu.h"
#include "inflate.h"
#include "launcher.h"
#include "texrep.h"

/* --- CRC32 (el de zlib/PNG) ------------------------------------------------- */
static uint32_t g_crct[256];
static void crc_init(void) {
    if (g_crct[1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crct[i] = c;
    }
}
static uint32_t crc32_upd(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) crc = g_crct[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* Bytes del primer nivel: solo formatos de bloque (UBC1/2/3) y U8U8U8U8 sin stride. */
static uint32_t level0_size(const GpuTexture *t) {
    uint32_t base = t->format >> 24, bw = (t->width + 3u) / 4u, bh = (t->height + 3u) / 4u;
    if (t->type == GPU_TEX_CUBE || t->type == GPU_TEX_LINEAR_STRIDED) return 0;
    switch (base) {
        case 0x85: return bw * bh * 8u;
        case 0x86: case 0x87: return bw * bh * 16u;
        case 0x0C: return (uint32_t)t->width * t->height * 4u;
        default: return 0;
    }
}

static uint32_t name_hash(const char *s);
static int has_prefix(const GpuTexture *t);
static int texrep_key(const GpuTexture *t, char *out, size_t max) {
    uint32_t n = level0_size(t);
    if (!n) return 0;
    const uint8_t *p = n ? (const uint8_t *)vita_mem_ptr(t->data, n) : NULL;
    if (!p) return 0;
    crc_init();
    snprintf(out, max, "%ux%u_%08X_%08X", t->width, t->height, t->format, crc32_upd(0, p, n));
    return 1;
}

/* --- índice de la carpeta ----------------------------------------------------- */
typedef struct { uint32_t h, ph; char *name; } Ent;   /* ph: hash de "<ancho>x<alto>_<formato>" */
static Ent *g_ents;
static uint32_t g_nents, g_cap;
static char g_dir[MAX_PATH];
static int g_on = -1;

static uint32_t name_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        uint8_t c = (uint8_t)*s;
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 32);   /* sin distinguir mayúsculas */
        h = (h ^ c) * 16777619u;
    }
    return h;
}

static void index_dir(void) {
    char pat[MAX_PATH + 8];
    snprintf(pat, sizeof(pat), "%s\\*.png", g_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char *dot = strrchr(fd.cFileName, '.');
        if (!dot) continue;
        *dot = 0;
        if (g_nents == g_cap) {
            g_cap = g_cap ? g_cap * 2 : 1024;
            Ent *n = (Ent *)realloc(g_ents, g_cap * sizeof(Ent));
            if (!n) break;
            g_ents = n;
        }
        g_ents[g_nents].name = _strdup(fd.cFileName);
        g_ents[g_nents].h = name_hash(fd.cFileName);
        char *u = strrchr(fd.cFileName, '_');
        if (u) { *u = 0; g_ents[g_nents].ph = name_hash(fd.cFileName); }
        g_nents++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int texrep_on(void) {
    if (g_on < 0) {
        const char *e = getenv("MLB15_TEXTURAS_HD");
        g_on = e ? atoi(e) != 0 : launcher_ini_int("video", "texturas_hd", 1) != 0;
        if ((e = getenv("MLB15_TEXTURAS_DIR")) != NULL) snprintf(g_dir, sizeof(g_dir), "%s", e);
        else {
            char exe[MAX_PATH];
            DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
            char *s = n ? strrchr(exe, '\\') : NULL;
            if (s) *s = 0; else snprintf(exe, sizeof(exe), ".");
            snprintf(g_dir, sizeof(g_dir), "%s\\texturas", exe);
        }
        if (g_on) index_dir();
        printf("[texturas] mejoradas: %s (%u en %s)\n", g_on ? "sí" : "no", g_nents, g_dir);
        fflush(stdout);
        if (!g_nents) g_on = 0;
    }
    return g_on;
}

/* Hay alguna mejorada de ese tamaño y formato (antes de calcular la huella). */
static int has_prefix(const GpuTexture *t) {
    char pre[40];
    snprintf(pre, sizeof(pre), "%ux%u_%08X", t->width, t->height, t->format);
    uint32_t h = name_hash(pre);
    for (uint32_t i = 0; i < g_nents; i++) if (g_ents[i].ph == h) return 1;
    return 0;
}

static int has(const char *key) {
    uint32_t h = name_hash(key);
    for (uint32_t i = 0; i < g_nents; i++)
        if (g_ents[i].h == h && !_stricmp(g_ents[i].name, key)) return 1;
    return 0;
}

/* --- PNG: lectura (8 bits, RGB/RGBA/gris, sin entrelazado) ---------------------- */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static uint32_t *png_load(const char *path, uint32_t *ow, uint32_t *oh) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = len > 8 ? (uint8_t *)malloc((size_t)len) : NULL;
    if (!d || fread(d, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(d); return NULL; }
    fclose(f);
    uint32_t w = 0, h = 0, ct = 0, bd = 0, il = 0;
    uint8_t *idat = (uint8_t *)malloc((size_t)len);
    size_t ni = 0;
    for (long p = 8; p + 12 <= len;) {
        uint32_t n = be32(d + p);
        const uint8_t *ty = d + p + 4, *body = d + p + 8;
        if (p + 12 + (long)n > len) break;
        if (!memcmp(ty, "IHDR", 4) && n >= 13) { w = be32(body); h = be32(body + 4); bd = body[8]; ct = body[9]; il = body[12]; }
        else if (!memcmp(ty, "IDAT", 4) && idat) { memcpy(idat + ni, body, n); ni += n; }
        else if (!memcmp(ty, "IEND", 4)) break;
        p += 12 + (long)n;
    }
    free(d);
    int ch = ct == 6 ? 4 : ct == 2 ? 3 : ct == 0 ? 1 : ct == 4 ? 2 : 0;
    if (!idat || !w || !h || w > 8192 || h > 8192 || bd != 8 || il || !ch) { free(idat); return NULL; }
    size_t row = (size_t)w * ch, raw_n = (row + 1) * h;
    uint8_t *raw = (uint8_t *)malloc(raw_n);
    uint32_t *out = (uint32_t *)malloc((size_t)w * h * 4);
    if (!raw || !out || zlib_inflate(raw, raw_n, idat, ni) != (long)raw_n) { free(idat); free(raw); free(out); return NULL; }
    free(idat);
    for (uint32_t y = 0; y < h; y++) {   /* deshace los filtros por fila */
        uint8_t *r = raw + y * (row + 1), ft = r[0], *c = r + 1, *u = y ? raw + (y - 1) * (row + 1) + 1 : NULL;
        for (size_t i = 0; i < row; i++) {
            int a = i >= (size_t)ch ? c[i - ch] : 0, b = u ? u[i] : 0, cc = (u && i >= (size_t)ch) ? u[i - ch] : 0;
            switch (ft) {
                case 1: c[i] = (uint8_t)(c[i] + a); break;
                case 2: c[i] = (uint8_t)(c[i] + b); break;
                case 3: c[i] = (uint8_t)(c[i] + ((a + b) >> 1)); break;
                case 4: {
                    int pp = a + b - cc, pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - cc);
                    c[i] = (uint8_t)(c[i] + (pa <= pb && pa <= pc ? a : pb <= pc ? b : cc));
                    break;
                }
                default: break;
            }
        }
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *s = c + (size_t)x * ch;
            uint8_t R = s[0], G = ch >= 3 ? s[1] : s[0], B = ch >= 3 ? s[2] : s[0];
            uint8_t A = ch == 4 ? s[3] : ch == 2 ? s[1] : 255;
            out[(size_t)y * w + x] = (uint32_t)R | (uint32_t)G << 8 | (uint32_t)B << 16 | (uint32_t)A << 24;
        }
    }
    free(raw);
    *ow = w; *oh = h;
    return out;
}

/* --- PNG: escritura sin comprimir (bloques "stored"), solo para volcados ------------ */
static void put32(FILE *f, uint32_t v) { uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; fwrite(b, 1, 4, f); }
static void png_save(const char *path, const uint32_t *rgba, uint32_t w, uint32_t h) {
    size_t row = (size_t)w * 4 + 1, raw_n = row * h;
    uint8_t *raw = (uint8_t *)malloc(raw_n);
    if (!raw) return;
    for (uint32_t y = 0; y < h; y++) { raw[y * row] = 0; memcpy(raw + y * row + 1, rgba + (size_t)y * w, (size_t)w * 4); }
    size_t nblk = (raw_n + 65534) / 65535, zn = 2 + raw_n + nblk * 5 + 4;
    uint8_t *z = (uint8_t *)malloc(zn);
    if (!z) { free(raw); return; }
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    for (size_t p = 0; p < raw_n; p += 65535) {
        size_t n = raw_n - p < 65535 ? raw_n - p : 65535;
        z[o++] = p + n >= raw_n ? 1 : 0;
        z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8); z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + p, n); o += n;
    }
    uint32_t ad = b << 16 | a;
    z[o++] = (uint8_t)(ad >> 24); z[o++] = (uint8_t)(ad >> 16); z[o++] = (uint8_t)(ad >> 8); z[o++] = (uint8_t)ad;
    free(raw);
    FILE *f = fopen(path, "wb");
    if (f) {
        crc_init();
        static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
        fwrite(sig, 1, 8, f);
        uint8_t ih[17] = { 'I', 'H', 'D', 'R' };
        ih[4] = (uint8_t)(w >> 24); ih[5] = (uint8_t)(w >> 16); ih[6] = (uint8_t)(w >> 8); ih[7] = (uint8_t)w;
        ih[8] = (uint8_t)(h >> 24); ih[9] = (uint8_t)(h >> 16); ih[10] = (uint8_t)(h >> 8); ih[11] = (uint8_t)h;
        ih[12] = 8; ih[13] = 6;
        put32(f, 13); fwrite(ih, 1, 17, f); put32(f, crc32_upd(0, ih, 17));
        put32(f, (uint32_t)o); fwrite("IDAT", 1, 4, f); fwrite(z, 1, o, f);
        put32(f, crc32_upd(crc32_upd(0, (const uint8_t *)"IDAT", 4), z, o));
        put32(f, 0); fwrite("IEND", 1, 4, f); put32(f, crc32_upd(0, (const uint8_t *)"IEND", 4));
        fclose(f);
    }
    free(z);
}

/* --- API ----------------------------------------------------------------------- */
uint32_t *texrep_find(const GpuTexture *t, uint32_t *w, uint32_t *h) {
    if (!texrep_on()) return NULL;
    char key[64], path[MAX_PATH + 80];
    if (!level0_size(t) || !has_prefix(t) || !texrep_key(t, key, sizeof(key)) || !has(key)) return NULL;
    snprintf(path, sizeof(path), "%s\\%s.png", g_dir, key);
    uint32_t *p = png_load(path, w, h);
    static unsigned n;
    if (n++ < 400) printf("[texturas] %s -> %s (%ux%u)\n", key, p ? "mejorada" : "PNG no válido", p ? *w : 0, p ? *h : 0);
    return p;
}

void texrep_volcar(const GpuTexture *t, const uint32_t *rgba) {
    static const char *dir = (const char *)1;
    if (dir == (const char *)1) dir = getenv("MLB15_TEX_VOLCAR");
    char key[64], path[MAX_PATH + 80];
    if (!dir || !texrep_key(t, key, sizeof(key))) return;
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof(path), "%s\\%s.png", dir, key);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    png_save(path, rgba, t->width, t->height);
}
