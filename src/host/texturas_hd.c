/* texturas_hd.c — «Crear texturas HD» del launcher: lo que hacía tools\texturas_hd.py, pero dentro
 * del exe y con la copia del juego del jugador (nada se distribuye):
 *   - recorre los .tp2 de caras, uniformes y cuerpos (y si se pide, estadios) de los WAD (.arc);
 *   - calcula la huella de cada textura igual que texrep.c (<ancho>x<alto>_<fmt>_<crc32 nivel 0>);
 *   - la decodifica (BC1/BC2/BC3 o U8U8U8U8, swizzled o lineal) y la pasa por Real-ESRGAN
 *     (herramientas\realesrgan\realesrgan-ncnn-vulkan.exe, x4plus, en la GPU);
 *   - la reduce a 2x (WIC, cúbico de alta calidad), mezcla con el reescalado normal en caras
 *     (55 % IA) y estadios (75 %), conserva el alfa y la guarda en texturas\<huella>.png.
 * Se puede cortar y volver a lanzar: salta las que ya existen. */
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "launcher.h"
#include "psarc.h"
#include "inflate.h"
#include "ui_estilo.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

#define MIN_LADO 64
#define LOTE 24

/* ------------------------------------------------------------------------------------------ */
/* WIC: PNG y reescalado                                                                        */

static IWICImagingFactory *g_wic;

static int wic_init(void) {
    if (g_wic) return 1;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    return SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&g_wic));
}

static int wic_guardar_png(const char *ruta, int w, int h, const uint8_t *rgba, int con_alfa) {
    wchar_t wr[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, ruta, -1, wr, MAX_PATH);
    IWICStream *st = NULL; IWICBitmapEncoder *enc = NULL; IWICBitmapFrameEncode *fr = NULL;
    int ok = 0;
    if (FAILED(IWICImagingFactory_CreateStream(g_wic, &st))) return 0;
    if (FAILED(IWICStream_InitializeFromFilename(st, wr, GENERIC_WRITE))) goto fin;
    if (FAILED(IWICImagingFactory_CreateEncoder(g_wic, &GUID_ContainerFormatPng, NULL, &enc))) goto fin;
    if (FAILED(IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache))) goto fin;
    if (FAILED(IWICBitmapEncoder_CreateNewFrame(enc, &fr, NULL))) goto fin;
    if (FAILED(IWICBitmapFrameEncode_Initialize(fr, NULL))) goto fin;
    IWICBitmapFrameEncode_SetSize(fr, (UINT)w, (UINT)h);
    WICPixelFormatGUID pf = con_alfa ? GUID_WICPixelFormat32bppRGBA : GUID_WICPixelFormat24bppRGB;
    IWICBitmapFrameEncode_SetPixelFormat(fr, &pf);
    if (con_alfa && IsEqualGUID(&pf, &GUID_WICPixelFormat32bppRGBA)) {
        ok = SUCCEEDED(IWICBitmapFrameEncode_WritePixels(fr, (UINT)h, (UINT)w * 4, (UINT)w * h * 4, (BYTE *)rgba));
    } else {
        /* RGB (o lo que el codificador acepte: se convierte con un mapa de bits WIC) */
        IWICBitmap *bm = NULL;
        if (SUCCEEDED(IWICImagingFactory_CreateBitmapFromMemory(g_wic, (UINT)w, (UINT)h, &GUID_WICPixelFormat32bppRGBA, (UINT)w * 4,
                                                                (UINT)w * h * 4, (BYTE *)rgba, &bm))) {
            IWICFormatConverter *cv = NULL;
            if (SUCCEEDED(IWICImagingFactory_CreateFormatConverter(g_wic, &cv)) &&
                SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)bm, &pf, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom)))
                ok = SUCCEEDED(IWICBitmapFrameEncode_WriteSource(fr, (IWICBitmapSource *)cv, NULL));
            if (cv) IWICFormatConverter_Release(cv);
            IWICBitmap_Release(bm);
        }
    }
    if (ok) ok = SUCCEEDED(IWICBitmapFrameEncode_Commit(fr)) && SUCCEEDED(IWICBitmapEncoder_Commit(enc));
fin:
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) IWICStream_Release(st);
    if (!ok) DeleteFileA(ruta);
    return ok;
}

/* Copia RGBA de una fuente WIC (convertida a 32bppRGBA). */
static uint8_t *wic_pixeles(IWICBitmapSource *src, int *w, int *h) {
    UINT uw, uh;
    IWICBitmapSource_GetSize(src, &uw, &uh);
    IWICFormatConverter *cv = NULL;
    uint8_t *px = NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateFormatConverter(g_wic, &cv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(cv, src, &GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))) {
        px = (uint8_t *)malloc((size_t)uw * uh * 4);
        if (px && FAILED(IWICFormatConverter_CopyPixels(cv, NULL, uw * 4, uw * uh * 4, px))) { free(px); px = NULL; }
    }
    if (cv) IWICFormatConverter_Release(cv);
    *w = (int)uw; *h = (int)uh;
    return px;
}

static uint8_t *wic_cargar(const char *ruta, int *w, int *h) {
    wchar_t wr[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, ruta, -1, wr, MAX_PATH);
    IWICBitmapDecoder *dec = NULL; IWICBitmapFrameDecode *fr = NULL;
    uint8_t *px = NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(g_wic, wr, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)))
        px = wic_pixeles((IWICBitmapSource *)fr, w, h);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    return px;
}

static uint8_t *wic_escalar(const uint8_t *rgba, int w, int h, int nw, int nh) {
    IWICBitmap *bm = NULL; IWICBitmapScaler *sc = NULL;
    uint8_t *px = NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateBitmapFromMemory(g_wic, (UINT)w, (UINT)h, &GUID_WICPixelFormat32bppRGBA, (UINT)w * 4,
                                                            (UINT)w * h * 4, (BYTE *)rgba, &bm)) &&
        SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(g_wic, &sc)) &&
        SUCCEEDED(IWICBitmapScaler_Initialize(sc, (IWICBitmapSource *)bm, (UINT)nw, (UINT)nh, WICBitmapInterpolationModeHighQualityCubic))) {
        int a, b;
        px = wic_pixeles((IWICBitmapSource *)sc, &a, &b);
    }
    if (sc) IWICBitmapScaler_Release(sc);
    if (bm) IWICBitmap_Release(bm);
    return px;
}

/* ------------------------------------------------------------------------------------------ */
/* .tp2: IFF0 'TX00' con una cabecera GXT y los píxeles al final del bloque DRBL               */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t crc32b(const uint8_t *b, size_t n) {
    static uint32_t tab[256];
    if (!tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ b[i]) & 255] ^ (c >> 8);
    return ~c;
}

typedef struct { const uint8_t *raw; uint32_t size, tipo, fmt; int w, h; uint32_t n0; } Tex;

static const uint8_t *buscar(const uint8_t *d, size_t n, const char *pat, size_t pn) {
    for (size_t i = 0; i + pn <= n; i++) if (!memcmp(d + i, pat, pn)) return d + i;
    return NULL;
}

static int tex_info(const uint8_t *d, size_t n, Tex *t) {
    const uint8_t *g = buscar(d, n, "GXT\0", 4);
    if (!g || (size_t)(g - d) + 0x34 > n) return 0;
    t->size = rd32(g + 0x24); t->tipo = rd32(g + 0x30); t->fmt = rd32(g + 0x34);
    t->w = rd16(g + 0x38); t->h = rd16(g + 0x3A);
    /* bloque DRBL */
    size_t pos = 16;
    while (pos + 16 <= n) {
        uint32_t sz = rd32(d + pos + 12);
        if (!memcmp(d + pos + 4, "DRBL", 4)) {
            size_t fin = pos + 16 + sz;
            if (fin > n || t->size > sz + 16) return 0;
            t->raw = d + fin - t->size;
            break;
        }
        pos += 16 + (size_t)sz;
    }
    if (!t->raw) return 0;
    uint32_t base = t->fmt >> 24, bw = ((uint32_t)t->w + 3) / 4, bh = ((uint32_t)t->h + 3) / 4;
    t->n0 = base == 0x85 ? bw * bh * 8 : (base == 0x86 || base == 0x87) ? bw * bh * 16 : base == 0x0C ? (uint32_t)t->w * t->h * 4 : 0;
    return t->n0 && t->n0 <= t->size;
}

static uint32_t compact1by1(uint32_t v) {
    v &= 0x55555555; v = (v ^ (v >> 1)) & 0x33333333; v = (v ^ (v >> 2)) & 0x0F0F0F0F;
    v = (v ^ (v >> 4)) & 0x00FF00FF; v = (v ^ (v >> 8)) & 0x0000FFFF;
    return v;
}
static int pot2(uint32_t v) { return v && !(v & (v - 1)); }
/* posición lineal (en la cuadrícula w x h) del elemento i en orden swizzled (como tp2png.py) */
static uint32_t desswizzle(uint32_t i, uint32_t w, uint32_t h) {
    uint32_t mn = w < h ? w : h, k = 0;
    while ((1u << (k + 1)) <= mn) k++;
    uint32_t mx = compact1by1(i), my = compact1by1(i >> 1), high = (i >> (2 * k)) << (2 * k), x, y, j;
    if (h < w) { j = high | ((my & (mn - 1)) << k) | (mx & (mn - 1)); x = j / h; y = j % h; }
    else { j = high | ((mx & (mn - 1)) << k) | (my & (mn - 1)); x = j % w; y = j / w; }
    return y * w + x;
}

static void rgb565(uint16_t c, uint8_t o[4]) {
    o[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31); o[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63); o[2] = (uint8_t)((c & 31) * 255 / 31); o[3] = 255;
}
static void bloque_color(const uint8_t *b, uint8_t px[16][4], int dxt1) {
    uint16_t c0 = rd16(b), c1 = rd16(b + 2);
    uint8_t c[4][4];
    rgb565(c0, c[0]); rgb565(c1, c[1]);
    for (int k = 0; k < 4; k++) {
        if (c0 > c1 || !dxt1) { c[2][k] = (uint8_t)((2 * c[0][k] + c[1][k]) / 3); c[3][k] = (uint8_t)((c[0][k] + 2 * c[1][k]) / 3); }
        else { c[2][k] = (uint8_t)((c[0][k] + c[1][k]) / 2); c[3][k] = 0; }
    }
    c[2][3] = 255; c[3][3] = (c0 > c1 || !dxt1) ? 255 : 0;
    uint32_t bits = rd32(b + 4);
    for (int i = 0; i < 16; i++) memcpy(px[i], c[(bits >> (2 * i)) & 3], 4);
}
static void bloque_alfa3(const uint8_t *b, uint8_t px[16][4]) {
    uint8_t a[8]; a[0] = b[0]; a[1] = b[1];
    if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1]) / 7);
    else { for (int i = 1; i < 5; i++) a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1]) / 5); a[6] = 0; a[7] = 255; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) px[i][3] = a[(bits >> (3 * i)) & 7];
}
static void bloque_alfa2(const uint8_t *b, uint8_t px[16][4]) {
    for (int i = 0; i < 16; i++) { int v = (b[i / 2] >> ((i & 1) * 4)) & 15; px[i][3] = (uint8_t)(v * 17); }
}

/* RGBA de w x h; NULL si no se puede. */
static uint8_t *tex_decodifica(const Tex *t) {
    int w = t->w, h = t->h;
    uint8_t *o = (uint8_t *)malloc((size_t)w * h * 4);
    if (!o) return NULL;
    uint32_t base = t->fmt >> 24;
    if (base == 0x0C) {
        for (uint32_t i = 0; i < (uint32_t)(w * h); i++) {
            uint32_t d = t->tipo == 0 && pot2((uint32_t)w) && pot2((uint32_t)h) ? desswizzle(i, (uint32_t)w, (uint32_t)h) : i;
            const uint8_t *p = t->raw + 4 * i;
            o[4 * d + 0] = p[2]; o[4 * d + 1] = p[1]; o[4 * d + 2] = p[0]; o[4 * d + 3] = p[3];   /* BGRA -> RGBA */
        }
        return o;
    }
    uint32_t bw = ((uint32_t)w + 3) / 4, bh = ((uint32_t)h + 3) / 4, bs = base == 0x85 ? 8 : 16;
    int sw = t->tipo == 0 && bw > 1 && bh > 1 && pot2(bw) && pot2(bh);
    for (uint32_t i = 0; i < bw * bh; i++) {
        uint32_t d = sw ? desswizzle(i, bw, bh) : i, bx = d % bw, by = d / bw;
        const uint8_t *b = t->raw + (size_t)i * bs;
        uint8_t px[16][4];
        if (base == 0x85) bloque_color(b, px, 1);
        else { bloque_color(b + 8, px, 0); if (base == 0x86) bloque_alfa2(b, px); else bloque_alfa3(b, px); }
        for (int k = 0; k < 16; k++) {
            uint32_t x = bx * 4 + (uint32_t)(k & 3), y = by * 4 + (uint32_t)(k >> 2);
            if (x < (uint32_t)w && y < (uint32_t)h) memcpy(o + ((size_t)y * w + x) * 4, px[k], 4);
        }
    }
    return o;
}

/* ------------------------------------------------------------------------------------------ */
/* qué texturas se mejoran (como clase() de texturas_hd.py)                                     */

enum { C_NADA, C_CARA, C_UNIFORME, C_CUERPO, C_ESTADIO };

static int clase(const char *nombre) {
    char r[512];
    size_t n = strlen(nombre);
    if (n >= sizeof(r)) return C_NADA;
    for (size_t i = 0; i <= n; i++) r[i] = nombre[i] == '\\' ? '/' : (char)tolower((unsigned char)nombre[i]);
    if (!strstr(r, ".tp2")) return C_NADA;
    const char *b = strrchr(r, '/'); b = b ? b + 1 : r;
    if (strstr(r, "/characters/heads/")) return strstr(b, "-color.tp2") ? C_CARA : C_NADA;
    if (strstr(r, "/uniforms/")) return !strncmp(b, "fe", 2) ? C_NADA : C_UNIFORME;
    if (strstr(r, "/characters/masterfiles/")) return strstr(b, "normal") || strstr(b, "shadow") ? C_NADA : C_CUERPO;
    if (strstr(r, "/stadiums/")) {
        if (strstr(r, "/crowd/") || (strstr(b, "lighting_") && !strstr(b, "color")) || strstr(b, "normal") ||
            strstr(b, "unifiedlighting") || strstr(b, "clouds") || strstr(b, "flag") || strstr(b, "skylighting"))
            return C_NADA;
        return C_ESTADIO;
    }
    return C_NADA;
}

/* ------------------------------------------------------------------------------------------ */
/* trabajo                                                                                      */

typedef struct { int arc, entrada, sub, clase; char clave[48]; } Pend;

static char g_exe_dir[MAX_PATH], g_salida[MAX_PATH], g_tmp[MAX_PATH], g_esrgan[MAX_PATH];
static volatile LONG g_cancelar, g_fin;
static CRITICAL_SECTION g_cs;
static wchar_t g_paso[256], g_det[256];
static int g_pct, g_estadios, g_res;
static HWND g_wnd, g_barra;
static HANDLE g_proc;

static void exe_dir(void) {
    if (g_exe_dir[0]) return;
    GetModuleFileNameA(NULL, g_exe_dir, sizeof(g_exe_dir));
    char *s = strrchr(g_exe_dir, '\\'); if (s) *s = 0;
    const char *e = getenv("MLB15_TEXTURAS_DIR");
    if (e) snprintf(g_salida, sizeof(g_salida), "%s", e); else snprintf(g_salida, sizeof(g_salida), "%s\\texturas", g_exe_dir);
    snprintf(g_esrgan, sizeof(g_esrgan), "%s\\herramientas\\realesrgan\\realesrgan-ncnn-vulkan.exe", g_exe_dir);
}

int texturas_hd_cuenta(void) {
    exe_dir();
    char pat[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s\\*.png", g_salida);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do n++; while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

static void estado(const wchar_t *paso, const wchar_t *det, int pct) {
    EnterCriticalSection(&g_cs);
    if (paso) wcsncpy(g_paso, paso, 255);
    if (det) wcsncpy(g_det, det, 255);
    g_pct = pct;
    LeaveCriticalSection(&g_cs);
    if (g_wnd) PostMessageW(g_wnd, WM_APP, 0, 0);
}

/* bytes del archivo (descomprimidos si es .mlbz); hay que liberar */
static uint8_t *leer_archivo(int arc, int e, size_t *n) {
    int64_t t = psarc_size(arc, e);
    if (t <= 0 || t > (256 << 20)) return NULL;
    uint8_t *d = (uint8_t *)malloc((size_t)t);
    if (!d || psarc_read(arc, e, 0, d, (uint64_t)t) != t) { free(d); return NULL; }
    const char *nm = psarc_name(arc, e);
    size_t ln = nm ? strlen(nm) : 0;
    if (ln > 5 && !_stricmp(nm + ln - 5, ".mlbz")) {
        uint32_t tam = t >= 4 ? rd32(d) : 0;
        uint8_t *o = tam && tam < (256u << 20) ? (uint8_t *)malloc(tam) : NULL;
        if (!o || zlib_inflate(o, tam, d + 4, (size_t)t - 4) != (long)tam) { free(o); free(d); return NULL; }
        free(d);
        *n = tam;
        return o;
    }
    *n = (size_t)t;
    return d;
}

/* subtextura i de un .tp2: la del principio (antes del primer IFF0TX00 anidado) o la i-ésima anidada */
static int subtexturas(const uint8_t *d, size_t n, const uint8_t **pp, size_t *pn, int max) {
    int k = 0;
    size_t pos = 16, primera = 0;
    while (pos + 16 <= n && k < max - 1) {
        uint32_t sz = rd32(d + pos + 12);
        if (!memcmp(d + pos, "IFF0TX00", 8)) {
            if (!primera) primera = pos;
            if (pos + 16 + sz > n) break;
            pp[k] = d + pos; pn[k] = 16 + (size_t)sz; k++;
        }
        pos += 16 + (size_t)sz;
    }
    pp[k] = d; pn[k] = primera ? primera : n; k++;
    return k;
}

static int existe(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

static void borrar_carpeta(const char *dir) {
    char pat[MAX_PATH], r[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.') continue;
            snprintf(r, sizeof(r), "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) borrar_carpeta(r); else DeleteFileA(r);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

static int trabajo(void) {
    if (!wic_init()) return -1;
    if (!existe(g_esrgan)) { estado(L"Falta Real-ESRGAN en herramientas\\realesrgan", L"", 0); return -1; }
    char juego[MAX_PATH], p[MAX_PATH];
    GetPrivateProfileStringA("juego", "carpeta", "", juego, sizeof(juego), launcher_ini_path());
    int arcs[2], na = 0;
    snprintf(p, sizeof(p), "%s\\VITA_PATCH_WAD.arc", juego);   /* el parche manda: va primero */
    if ((arcs[na] = psarc_open(p)) >= 0) na++;
    snprintf(p, sizeof(p), "%s\\VITA_COMPRESSED_WAD.arc", juego);
    if ((arcs[na] = psarc_open(p)) >= 0) na++;
    if (!na) { estado(L"No encuentro los archivos del juego (.arc)", L"", 0); return -1; }
    CreateDirectoryA(g_salida, NULL);

    /* 1. qué hay que hacer */
    estado(L"Buscando texturas en tu copia del juego…", L"", -1);
    size_t cap = 4096, npend = 0, nvistas = 0, capv = 8192;
    Pend *pend = (Pend *)malloc(cap * sizeof(Pend));
    char (*vistas)[48] = malloc(capv * 48);
    /* nombres ya cubiertos por el parche (para no leer dos veces el mismo archivo) */
    for (int ai = 0; ai < na && !g_cancelar; ai++) {
        int nent = psarc_count(arcs[ai]);
        for (int e = 0; e < nent && !g_cancelar; e++) {
            const char *nm = psarc_name(arcs[ai], e);
            if (!nm) continue;
            int c = clase(nm);
            if (c == C_NADA || (c == C_ESTADIO && !g_estadios)) continue;
            if (ai > 0 && na > 1 && psarc_find(arcs[0], nm) >= 0) continue;
            size_t n;
            uint8_t *d = leer_archivo(arcs[ai], e, &n);
            if (!d) continue;
            const uint8_t *pp[512]; size_t pn[512];
            int ns = subtexturas(d, n, pp, pn, 512);
            for (int s = 0; s < ns; s++) {
                Tex t; memset(&t, 0, sizeof(t));
                if (!tex_info(pp[s], pn[s], &t) || (t.w < MIN_LADO || t.h < MIN_LADO)) continue;
                char clave[48];
                snprintf(clave, sizeof(clave), "%dx%d_%08X_%08X", t.w, t.h, t.fmt, crc32b(t.raw, t.n0));
                int ya = 0;
                for (size_t v = 0; v < nvistas && !ya; v++) ya = !strcmp(vistas[v], clave);
                if (ya) continue;
                if (nvistas == capv) { capv *= 2; vistas = realloc(vistas, capv * 48); }
                strcpy(vistas[nvistas++], clave);
                snprintf(p, sizeof(p), "%s\\%s.png", g_salida, clave);
                if (existe(p)) continue;
                if (npend == cap) { cap *= 2; pend = (Pend *)realloc(pend, cap * sizeof(Pend)); }
                Pend *q = &pend[npend++];
                q->arc = arcs[ai]; q->entrada = e; q->sub = s; q->clase = c; strcpy(q->clave, clave);
            }
            free(d);
            if ((e & 63) == 0) {
                wchar_t det[128];
                swprintf(det, 128, L"%zu texturas distintas, %zu por crear", nvistas, npend);
                estado(NULL, det, -1);
            }
        }
    }
    free(vistas);
    if (g_cancelar) { free(pend); return -1; }

    /* 2. por lotes: decodificar -> IA -> 2x -> guardar */
    snprintf(g_tmp, sizeof(g_tmp), "%s\\datos\\texturas_tmp", g_exe_dir);
    DWORD t0 = GetTickCount();
    size_t hechas = 0;
    for (size_t i = 0; i < npend && !g_cancelar; i += LOTE) {
        size_t nl = npend - i < LOTE ? npend - i : LOTE;
        char din[MAX_PATH], dout[MAX_PATH];
        borrar_carpeta(g_tmp);
        CreateDirectoryA(g_tmp, NULL);
        snprintf(din, sizeof(din), "%s\\in", g_tmp); CreateDirectoryA(din, NULL);
        snprintf(dout, sizeof(dout), "%s\\out", g_tmp); CreateDirectoryA(dout, NULL);
        uint8_t *orig[LOTE] = { 0 }; int ow[LOTE], oh[LOTE];
        for (size_t k = 0; k < nl; k++) {
            Pend *q = &pend[i + k];
            size_t n;
            uint8_t *d = leer_archivo(q->arc, q->entrada, &n);
            if (!d) continue;
            const uint8_t *pp[512]; size_t pn[512];
            int ns = subtexturas(d, n, pp, pn, 512);
            Tex t; memset(&t, 0, sizeof(t));
            if (q->sub < ns && tex_info(pp[q->sub], pn[q->sub], &t) && (orig[k] = tex_decodifica(&t))) {
                ow[k] = t.w; oh[k] = t.h;
                snprintf(p, sizeof(p), "%s\\%s.png", din, q->clave);
                if (!wic_guardar_png(p, t.w, t.h, orig[k], 0)) { free(orig[k]); orig[k] = NULL; }
            }
            free(d);
        }
        /* Real-ESRGAN con todo el lote */
        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "\"%s\" -i \"%s\" -o \"%s\" -n realesrgan-x4plus -f png", g_esrgan, din, dout);
        STARTUPINFOA si = { sizeof(si) }; PROCESS_INFORMATION pi;
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            g_proc = pi.hProcess;
            while (WaitForSingleObject(pi.hProcess, 200) == WAIT_TIMEOUT)
                if (g_cancelar) { TerminateProcess(pi.hProcess, 1); break; }
            CloseHandle(pi.hProcess);
            g_proc = NULL;
        }
        for (size_t k = 0; k < nl; k++) {
            if (!orig[k]) continue;
            Pend *q = &pend[i + k];
            int w2 = ow[k] * 2, h2 = oh[k] * 2, iw, ih;
            snprintf(p, sizeof(p), "%s\\%s.png", dout, q->clave);
            uint8_t *ia = g_cancelar ? NULL : wic_cargar(p, &iw, &ih);
            if (ia) {
                uint8_t *ia2 = wic_escalar(ia, iw, ih, w2, h2);
                uint8_t *lz = wic_escalar(orig[k], ow[k], oh[k], w2, h2);   /* también da el alfa reescalado */
                if (ia2 && lz) {
                    int m = q->clase == C_CARA ? 55 : q->clase == C_ESTADIO ? 75 : 100;
                    int con_alfa = 0;
                    for (int j = 0; j < ow[k] * oh[k] && !con_alfa; j++) con_alfa = orig[k][4 * j + 3] != 255;
                    for (int j = 0; j < w2 * h2; j++) {
                        for (int c = 0; c < 3; c++) ia2[4 * j + c] = (uint8_t)((ia2[4 * j + c] * m + lz[4 * j + c] * (100 - m) + 50) / 100);
                        ia2[4 * j + 3] = con_alfa ? lz[4 * j + 3] : 255;
                    }
                    snprintf(p, sizeof(p), "%s\\%s.png", g_salida, q->clave);
                    wic_guardar_png(p, w2, h2, ia2, con_alfa);
                }
                free(ia2); free(lz); free(ia);
            }
            free(orig[k]);
            hechas++;
        }
        wchar_t det[160];
        DWORD dt = GetTickCount() - t0, resto = hechas ? (DWORD)((double)dt / hechas * (npend - hechas)) : 0;
        swprintf(det, 160, L"%zu de %zu · quedan ~%lu min", hechas, npend, (resto + 59999) / 60000);
        estado(L"Mejorando texturas con IA (Real-ESRGAN, en tu tarjeta gráfica)…", det, (int)(hechas * 100 / (npend ? npend : 1)));
    }
    borrar_carpeta(g_tmp);
    free(pend);
    if (!g_cancelar) {
        wchar_t det[128];
        swprintf(det, 128, L"%zu texturas nuevas; en total hay %d", hechas, texturas_hd_cuenta());
        estado(L"¡Listo!", det, 100);
    }
    return g_cancelar ? -1 : 0;
}

static DWORD WINAPI hilo(void *x) {
    (void)x;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    g_res = trabajo();
    InterlockedExchange(&g_fin, 1);
    if (g_wnd) PostMessageW(g_wnd, WM_APP + 1, 0, 0);
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* ventana                                                                                      */

enum { ID_EMPEZAR = 500, ID_ESTADIOS, ID_CERRAR, ID_BARRA2 };
#define TW 620
#define TH 420
static int g_corriendo;

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_APP: {
        int pct;
        EnterCriticalSection(&g_cs); pct = g_pct; LeaveCriticalSection(&g_cs);
        LONG_PTR st = GetWindowLongPtrW(g_barra, GWL_STYLE);
        if (pct < 0) { if (!(st & PBS_MARQUEE)) { SetWindowLongPtrW(g_barra, GWL_STYLE, st | PBS_MARQUEE); SendMessageW(g_barra, PBM_SETMARQUEE, 1, 30); } }
        else { if (st & PBS_MARQUEE) { SendMessageW(g_barra, PBM_SETMARQUEE, 0, 0); SetWindowLongPtrW(g_barra, GWL_STYLE, st & ~PBS_MARQUEE); } SendMessageW(g_barra, PBM_SETPOS, (WPARAM)pct, 0); }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_APP + 1:
        g_corriendo = 0;
        SetWindowTextW(GetDlgItem(h, ID_CERRAR), L"Cerrar");
        EnableWindow(GetDlgItem(h, ID_EMPEZAR), TRUE);
        SetWindowTextW(GetDlgItem(h, ID_EMPEZAR), L"Buscar más");
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_COMMAND:
        if (ui_comando(h, w, l)) return 0;
        if (LOWORD(w) == ID_EMPEZAR && !g_corriendo) {
            g_estadios = ui_interruptor_on(GetDlgItem(h, ID_ESTADIOS));
            g_cancelar = g_fin = 0;
            g_corriendo = 1;
            EnableWindow(GetDlgItem(h, ID_EMPEZAR), FALSE);
            SetWindowTextW(GetDlgItem(h, ID_CERRAR), L"Detener");
            ShowWindow(g_barra, SW_SHOW);
            CloseHandle(CreateThread(NULL, 0, hilo, NULL, 0, NULL));
            return 0;
        }
        if (LOWORD(w) == ID_CERRAR) {
            if (g_corriendo) { InterlockedExchange(&g_cancelar, 1); estado(L"Deteniendo… (lo hecho se conserva)", L"", -1); return 0; }
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (g_corriendo) { InterlockedExchange(&g_cancelar, 1); estado(L"Deteniendo… (lo hecho se conserva)", L"", -1); return 0; }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC pdc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC dc = CreateCompatibleDC(pdc);
        HBITMAP bmp = CreateCompatibleBitmap(pdc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(dc, bmp);
        ui_pinta_fondo(h, dc, &rc);
        ui_texto(h, dc, L"Texturas HD", 28, 72, TW - 56, UI_TITULO);
        ui_texto(h, dc, L"Se crean a partir de TU copia del juego con Real-ESRGAN (IA, en tu tarjeta gráfica): caras, "
                        L"uniformes y cuerpos de los jugadores al doble de resolución. Se guardan en la carpeta «texturas» "
                        L"y el juego las usa solas. Puedes detenerlo cuando quieras y seguir otro día.", 28, 108, TW - 56, UI_PEQUENO_TENUE);
        wchar_t paso[256], det[256];
        EnterCriticalSection(&g_cs); wcscpy(paso, g_paso); wcscpy(det, g_det); LeaveCriticalSection(&g_cs);
        ui_texto(h, dc, paso, 28, 240, TW - 56, UI_FUERTE);
        ui_texto(h, dc, det, 28, 296, TW - 56, UI_PEQUENO_TENUE);
        BitBlt(pdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DRAWITEM: ui_pinta_boton((const DRAWITEMSTRUCT *)l); return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}

int texturas_hd_dialogo(HWND padre) {
    exe_dir();
    ui_init();
    static int cs;
    if (!cs) { InitializeCriticalSection(&g_cs); cs = 1; }
    swprintf(g_paso, 256, L"Ahora hay %d texturas mejoradas.", texturas_hd_cuenta());
    wcscpy(g_det, existe(g_esrgan) ? L"Pulsa Empezar. La primera vez tarda bastante (de media hora a varias horas según la tarjeta)."
                                   : L"Falta herramientas\\realesrgan: esta copia del launcher no puede crearlas.");
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"MLB15Texturas";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r = { 0, 0, ui_px(TW), ui_px(TH) };
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    RECT pr; GetWindowRect(padre, &pr);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int cap = getenv("MLB15_TEXTURAS_CAPTURA") != NULL;
    g_wnd = CreateWindowExW(WS_EX_DLGMODALFRAME | (cap ? WS_EX_TOOLWINDOW : 0), L"MLB15Texturas", L"Project Baseball — Texturas HD", WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                            cap ? -8000 : pr.left + (pr.right - pr.left - ww) / 2, cap ? -8000 : pr.top + (pr.bottom - pr.top - wh) / 2,
                            ww, wh, padre, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_wnd) return 0;
    ui_ventana_oscura(g_wnd);
    ui_interruptor(g_wnd, ID_ESTADIOS, L"Incluir también los estadios (tarda mucho más)", 28, 196, TW - 56, 0);
    g_barra = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | PBS_SMOOTH, ui_px(28), ui_px(274), ui_px(TW - 56), ui_px(12),
                              g_wnd, (HMENU)ID_BARRA2, NULL, NULL);
    ui_estilo_barra(g_barra);
    SendMessageW(g_barra, PBM_SETRANGE32, 0, 100);
    ui_boton(g_wnd, ID_CERRAR, L"Cerrar", TW - 28 - 140 - 10 - 120, TH - 62, 120, 40, UIB_NORMAL);
    HWND emp = ui_boton(g_wnd, ID_EMPEZAR, L"Empezar", TW - 28 - 140, TH - 62, 140, 40, UIB_PRIMARIO);
    if (!existe(g_esrgan)) EnableWindow(emp, FALSE);
    if (cap) {   /* pruebas: dibujarla y guardarla */
        ShowWindow(g_wnd, SW_SHOWNOACTIVATE); UpdateWindow(g_wnd);
        MSG mm;
        for (int i = 0; i < 50; i++) { while (PeekMessageW(&mm, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&mm); Sleep(10); }
        extern void opciones_captura(HWND h, const char *path);
        opciones_captura(g_wnd, getenv("MLB15_TEXTURAS_CAPTURA"));
        DestroyWindow(g_wnd);
        while (PeekMessageW(&mm, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&mm);
        g_wnd = NULL;
        return 0;
    }
    EnableWindow(padre, FALSE);
    ShowWindow(g_wnd, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
        if (!IsDialogMessageW(g_wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    g_wnd = NULL;
    EnableWindow(padre, TRUE);
    SetForegroundWindow(padre);
    return 1;
}
