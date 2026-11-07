/* SceGxm (GPU de la Vita) sobre el renderizador Vulkan del port (gpu.h).
 *
 * Sustituye al HLE anterior (host_old/hle_gxm.c), que inventaba los parámetros
 * de los shaders con un hash del nombre y no guardaba el estado de dibujo. Aquí:
 *
 *  - Programas: los parámetros son los REALES del GXP del juego (cabecera y
 *    tabla comprobadas con tools/gxp_info.py): FindParameterByName devuelve un
 *    puntero dentro del propio GXP, como el firmware.
 *  - Uniformes: buffers por índice (SetVertex/FragmentUniformBuffer), el buffer
 *    por defecto (contenedor 14) que se reserva por draw, y SetUniformDataF
 *    escribe en recurso*4 + offset (f32) o con medias floats (f16).
 *  - Texturas: el juego trata SceGxmTexture (16 bytes) como opaco y todas sus
 *    funciones son HLE, así que guardamos ahí NUESTRA codificación: formato,
 *    tamaño, datos, tipo, filtros. Copiar la estructura sigue funcionando.
 *  - Draw: atributos según CreateVertexProgram (formato, stream, offset,
 *    registro) convertidos a un vértice canónico; MVP leída del buffer que toca;
 *    shaders genéricos (ui.vert/ui.frag) elegidos por los parámetros del
 *    programa. Aún sin traductor de microcódigo USSE: vale para el 2D del menú.
 *
 * Convención: void(void), argumentos en r0-r3 y pila; floats en s0-s15
 * (ABI hard-float). Trazas: MLB15_GXM_TRACE=1.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <stdarg.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/vfp.h"

#include "gpu.h"
#include "shc.h"
#include "usse.h"
#include "v3k_api.h"
#include "port.h"

/* getenv recordado: el entorno no cambia mientras corre el port y getenv (bloqueo + recorrido
 * del entorno) se llamaba en caminos de cada draw. Clave = puntero del literal. */
static const char *env_cached(const char *name) {
    static struct { const char *k; const char *v; } tab[256];
    static int n;
    for (int i = 0; i < n; i++) if (tab[i].k == name) return tab[i].v;
    const char *v = getenv(name);
    if (n < 256) { tab[n].k = name; tab[n].v = v; n++; }
    return v;
}


#define GXM_OK                  0u
#define GXM_ERR_INVALID_VALUE   0x805B0003u
#define GXM_ERR_INVALID_POINTER 0x805B0004u
#define GXM_ERR_OUT_OF_MEMORY   0x805B0002u

static uint32_t arg(int n) { return vita_read32(sp + 4u * (uint32_t)n); }

static int g_trace = -1;
static int tr(void) {
    if (g_trace < 0) g_trace = env_cached("MLB15_GXM_TRACE") ? 1 : 0;
    return g_trace;
}

/* --- objetos guest --------------------------------------------------------- */
/* Los manejadores que se devuelven al juego son direcciones guest válidas de un
 * bloque propio (antes era un bloque de 4 KB de la reserva por objeto). */
static uint32_t g_obj_base, g_obj_top, g_obj_end;

static uint32_t obj_alloc(uint32_t size) {
    size = (size + 15u) & ~15u;
    if (!g_obj_base) {
        g_obj_base = port_alloc(4u << 20, 0x1000, "gxm objetos");
        g_obj_top = g_obj_base;
        g_obj_end = g_obj_base + (4u << 20);
    }
    if (!g_obj_base || g_obj_top + size > g_obj_end) return port_alloc(size, 0x10, "gxm objeto");
    uint32_t a = g_obj_top;
    g_obj_top += size;
    memset(vita_mem_ptr(a, size), 0, size);
    return a;
}

/* --- GXP -------------------------------------------------------------------- */

static uint32_t gxp_nparams(uint32_t g) { return vita_read32(g + 0x24); }
static uint32_t gxp_params(uint32_t g)  { return g + 0x28 + vita_read32(g + 0x28); }
static int      gxp_is_frag(uint32_t g) { return vita_read8(g + 0x14) & 1; }
static uint32_t gxp_default_ub(uint32_t g) { return vita_read32(g + 0x64) * 4u; }

static uint32_t p_a1(uint32_t p)   { return vita_read16(p + 4); }
static uint32_t p_cat(uint32_t p)  { return p_a1(p) & 15; }
static uint32_t p_type(uint32_t p) { return (p_a1(p) >> 4) & 15; }
static uint32_t p_comps(uint32_t p){ return (p_a1(p) >> 8) & 15; }
static uint32_t p_cont(uint32_t p) { return (p_a1(p) >> 12) & 15; }
static uint32_t p_res(uint32_t p)  { return vita_read32(p + 12); }
static uint32_t p_name(uint32_t p) { return p + (uint32_t)(int32_t)vita_read32(p); }

static void p_name_str(uint32_t p, char *out, size_t n) { port_guest_str(p_name(p), out, n); }

static uint32_t gxp_find(uint32_t g, const char *name) {
    if (!g) return 0;
    uint32_t n = gxp_nparams(g), base = gxp_params(g);
    char buf[96];
    for (uint32_t i = 0; i < n && i < 256; i++) {
        p_name_str(base + 16 * i, buf, sizeof(buf));
        if (!strcmp(buf, name)) return base + 16 * i;
    }
    return 0;
}

/* --- programas del patcher --------------------------------------------------- */

enum { SLOT_POS, SLOT_UV0, SLOT_UV1, SLOT_COL, SLOT_BIDX, SLOT_BW, SLOT_NRM, SLOT_INST0, SLOT_NONE = -1 };
#define SLOT_INST3 10

typedef struct {
    uint16_t stream, offset;
    uint8_t  format, comps;
    uint16_t reg;
    int8_t   slot;
} Attr;

typedef struct {
    uint32_t self, gxp;
    int nattr, nstream;
    Attr attr[16];
    uint16_t stride[16], index_source[16];
    uint32_t mvp, vpm, model;          /* parámetros de matriz (0 = no hay) */
    uint32_t skin;                     /* UVP_SkinningMatrix: matrices 3x4 de huesos (0 = sin skinning) */
    int has_color;
    int color_is_data;                 /* el atributo Color son parámetros, no color */
    uint32_t light_dir;                /* UVP_LightDirSun (0 = no hay) */
    uint32_t mound_pos, mound_par;     /* UVP_MoundPosition / UVP_MoundParameters */
    int real, mod;                     /* shader traducido: 0 sin probar, 1 sí, -1 no; módulo Vulkan */
    UsseInfo *ui;
    int has_normal;
    int v3k, v3k_mod;                  /* shader de Vita3K: 0 sin probar, 1 sí, -1 no */
    V3kInfo *v3ki;
} VProg;

typedef struct {
    uint32_t self, gxp;
    uint8_t  blend, color_func, alpha_func, color_src, color_dst, alpha_src, alpha_dst, mask;
    int      sampler;                   /* unidad de textura 0, -1 = sin textura */
    uint32_t konst;                     /* parámetro de color constante (ClearColor), 0 = no */
    uint32_t ufp[7];                    /* mezcla programable: UFP_SRC_Ck..UFP_DST_CSc (0 = no hay) */
    uint32_t huella;                    /* FNV-1a del GXP (gxp_huella) */
    int      no_vcolor;                 /* el color de vértice no es color (sombras de jugadores) */
    uint32_t light_dir, light_col, amb_col, amb_mult;   /* iluminación del sol (0 = no hay) */
    int real, mod;
    UsseInfo *ui;
    int v3k_n, v3k_bad;                 /* variantes de Vita3K por formatos de textura */
    struct { uint32_t key; int mod; V3kInfo *info; } v3kv[4];
} FProg;

#define MAX_PROGS 4096
static VProg g_vp[MAX_PROGS];
static FProg g_fp[MAX_PROGS];
static int g_nvp, g_nfp;

/* Programa por su objeto: tabla hash (índice + 1, 0 = libre). Antes se recorría la lista
 * entera dos veces por draw (~400 programas de varios KB: fallos de caché). Los programas
 * solo se añaden (Release no los quita), así que la tabla no necesita borrados. */
#define PROG_HASH 16384
static int g_vp_h[PROG_HASH], g_fp_h[PROG_HASH];
static void prog_hash_put(int *h, uint32_t self, int idx, int is_frag) {
    uint32_t k = (self * 2654435761u) >> 18;
    for (uint32_t i = 0; i < PROG_HASH; i++) {
        uint32_t j = (k + i) & (PROG_HASH - 1);
        int e = h[j];
        if (!e || (is_frag ? g_fp[e - 1].self : g_vp[e - 1].self) == self) { h[j] = idx + 1; return; }
    }
}
static VProg *vp_find(uint32_t self) {
    uint32_t k = (self * 2654435761u) >> 18;
    for (uint32_t i = 0; i < PROG_HASH; i++) {
        int e = g_vp_h[(k + i) & (PROG_HASH - 1)];
        if (!e) return NULL;
        if (g_vp[e - 1].self == self) return &g_vp[e - 1];
    }
    return NULL;
}
static FProg *fp_find(uint32_t self) {
    uint32_t k = (self * 2654435761u) >> 18;
    for (uint32_t i = 0; i < PROG_HASH; i++) {
        int e = g_fp_h[(k + i) & (PROG_HASH - 1)];
        if (!e) return NULL;
        if (g_fp[e - 1].self == self) return &g_fp[e - 1];
    }
    return NULL;
}

static int slot_for(uint32_t gxp, uint16_t reg) {
    uint32_t n = gxp_nparams(gxp), base = gxp_params(gxp);
    for (uint32_t i = 0; i < n && i < 256; i++) {
        uint32_t p = base + 16 * i;
        if (p_cat(p) != 0 || p_res(p) != reg) continue;
        char name[64];
        p_name_str(p, name, sizeof(name));
        uint32_t sem = vita_read16(p + 6), sidx = (sem >> 8) & 15;
        if (!strcmp(name, "Position")) return SLOT_POS;
        if (!strcmp(name, "Color") || !strcmp(name, "Colour")) return SLOT_COL;
        if (!strcmp(name, "UV1")) return SLOT_UV1;
        if (!strcmp(name, "Index")) return SLOT_BIDX;          /* huesos (skinning) */
        if (!strcmp(name, "Weight")) return SLOT_BW;
        if (!strcmp(name, "Normal")) return SLOT_NRM;
        if (!strncmp(name, "UV", 2)) return SLOT_UV0;
        if (!strncmp(name, "InstanceMatrix3x4[", 18) && name[18] >= '0' && name[18] <= '2')
            return SLOT_INST0 + (name[18] - '0');   /* skinning por instancia (bate/casco) */
        if (!strncmp(name, "InstanceAttributes", 18))
            return SLOT_INST3;                       /* traslación/prms de la instancia */
        switch (sem & 0xFF) {
            case 11: return SLOT_POS;
            case 6:  return SLOT_COL;
            case 14: return sidx ? SLOT_UV1 : SLOT_UV0;
        }
        return SLOT_NONE;
    }
    return SLOT_NONE;
}

/* --- contexto ---------------------------------------------------------------- */

typedef struct {
    int used;
    uint32_t self;
    uint32_t vprog, fprog, pfs;                /* pfs: estado de fragmento precalculado */
    uint32_t streams[16];
    uint8_t  ftex[16][16];
    uint32_t vub[16], fub[16];
    uint32_t vdef, fdef;
    uint32_t ring, ring_top;                   /* para los buffers por defecto */
    float    vp[6];
    int      vp_set, vp_enable;
    int32_t  dbias_factor[2], dbias_units[2];   /* sceGxmSet{Front,Back}DepthBias */
    /* profundidad/stencil: valores GXM ya traducidos (ver gpu.h) */
    uint8_t  cull, two_sided, frag_off;
    uint8_t  depth_func[2], depth_write[2];
    struct { uint8_t func, fail, zfail, pass, cmask, wmask, ref; } st[2];
    int      in_scene;
    uint32_t tw, th;
} Ctx;

#define MAX_CTX 8
#define RING_SIZE (1u << 20)
static Ctx g_ctx[MAX_CTX];

static Ctx *ctx_find(uint32_t self) {
    for (int i = 0; i < MAX_CTX; i++) if (g_ctx[i].used && g_ctx[i].self == self) return &g_ctx[i];
    return NULL;
}

static uint32_t ring_alloc(Ctx *c, uint32_t size) {
    size = (size + 15u) & ~15u;
    if (!size) size = 16;
    if (!c->ring) { c->ring = port_alloc(RING_SIZE, 0x1000, "gxm uniformes"); c->ring_top = 0; }
    if (!c->ring) return 0;
    if (c->ring_top + size > RING_SIZE) c->ring_top = 0;
    uint32_t a = c->ring + c->ring_top;
    c->ring_top += size;
    return a;
}

/* --- superficies, render targets, sync -------------------------------------- */

typedef struct { uint32_t self, data, w, h, stride, format, type, gamma; } Surf;
#define MAX_SURF 256
static Surf g_surf[MAX_SURF];
static int g_nsurf;

static Surf *surf_find(uint32_t self) {
    for (int i = 0; i < g_nsurf; i++) if (g_surf[i].self == self) return &g_surf[i];
    return NULL;
}

/* --- cola de presentación ------------------------------------------------------ */

static uint32_t g_display_callback, g_callback_data_size;
static unsigned long g_draws, g_scenes, g_skipped;
/* MLB15_GXM_TRACE: vértices convertidos e índices por bloque de 600 escenas */
static unsigned long long g_stat_verts, g_stat_idx;
static uint32_t g_stat_maxv, g_stat_maxi, g_stat_maxgxp;
/* motivos de descarte: sin programa/vacío, primitiva, índices sin memoria, índice enorme, demasiados, instancias */
static unsigned long g_skip_why[6];
/* desglose de g_skip_why[0]: basura de entrada (0 índices / sin prog), skin sin matrices, color_is_data */
static unsigned long g_skip_junk, g_skip_skin, g_skip_color;
static void scene_dump_check_time(uint32_t width);
/* MLB15_NEGRO=1: registro de los draws de cada fotograma. Si al presentar la zona
 * baja de la pantalla sale negra (> 15 %), escribe negro_N.txt con los draws de
 * ese fotograma y del anterior (normal). Para encontrar qué draw pinta de negro. */
static char  *g_rec[2];
static size_t g_rec_n[2];
static int    g_rec_cur, g_rec_on = -1;
static int rec_on(void) {
    if (g_rec_on < 0) {
        g_rec_on = env_cached("MLB15_NEGRO") ? 1 : 0;
        if (g_rec_on) { g_rec[0] = (char *)malloc(4u << 20); g_rec[1] = (char *)malloc(4u << 20); }
    }
    return g_rec_on;
}
static void rec(const char *fmt, ...) {
    if (!rec_on()) return;
    size_t *n = &g_rec_n[g_rec_cur];
    if (*n + 512 > (4u << 20)) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(g_rec[g_rec_cur] + *n, 512, fmt, ap);
    va_end(ap);
    if (k > 0) *n += (size_t)(k < 511 ? k : 511);
}
int port_rec_frame(double black) {
    if (!rec_on()) return -1;
    int saved = -1;
    extern int port_en_partido;
    static int nout;
    static unsigned ntotal, npart;
    if (port_en_partido) {
        npart++;
        if (black > 0.15) ntotal++;
        if (npart % 600 == 0) printf("[negro] %u de %u fotogramas del partido con la franja baja negra\n", ntotal, npart);
    }
    if (black > 0.15 && port_en_partido && nout < 6) {
        char path[64];
        snprintf(path, sizeof(path), "negro_%d.txt", nout++);
        FILE *fp = fopen(path, "w");
        if (fp) {
            int prev = g_rec_cur ^ 1;
            fprintf(fp, "=== ANTERIOR (normal)\n");
            fwrite(g_rec[prev], 1, g_rec_n[prev], fp);
            fprintf(fp, "=== NEGRO (%.1f %%)\n", black * 100.0);
            fwrite(g_rec[g_rec_cur], 1, g_rec_n[g_rec_cur], fp);
            fclose(fp);
            extern void gpu_mascara_guarda(const char *path);
            snprintf(path, sizeof(path), "negro_%d_mascara.raw", nout - 1);
            gpu_mascara_guarda(path);
            saved = nout - 1;
            printf("[negro] %s (%.1f %%)\n", path, black * 100.0);
        }
    }
    if (black <= 0.05) g_rec_cur ^= 1;    /* el último normal se conserva como «anterior» */
    g_rec_n[g_rec_cur] = 0;
    return saved;
}

static void scene_dump_end(void);
static int real_enabled(void);
static unsigned long g_real_ok, g_real_bad;

/* ========================================================================== */
/* Inicialización, contexto, render target, sync                              */
/* ========================================================================== */

void vita_hle_sceGxmInitialize(void) {
    printf("[gxm] sceGxmInitialize(flags=0x%X queue=%u cb=0x%08X cbData=%u pb=%u)\n",
           vita_read32(r0), vita_read32(r0 + 4), vita_read32(r0 + 8), vita_read32(r0 + 12),
           vita_read32(r0 + 16));
    g_display_callback = vita_read32(r0 + 8);
    g_callback_data_size = vita_read32(r0 + 12);
    fflush(stdout);
    {   extern void arranque(const char *fmt, ...); }
    if (!gpu_init()) {
        { extern void arranque(const char *fmt, ...); extern const char *gpu_error(void); arranque("gpu: Vulkan NO arrancó: %s", gpu_error()); }
        const char *nw = getenv("MLB15_NOWINDOW"), *gg = getenv("MLB15_GPU");
        if (!(nw && *nw == '1') && !(gg && *gg == '0')) {
            extern const char *gpu_error(void);
            wchar_t det[256], m[1200];
            MultiByteToWideChar(CP_UTF8, 0, gpu_error(), -1, det, 256);
            swprintf(m, 1200, L"No se pudieron iniciar los gráficos (Vulkan): %ls.\n\n"
                              L"Instala el último driver de tu tarjeta gráfica (Intel, NVIDIA o AMD; con Intel, el "
                              L"«Intel Driver & Support Assistant»), reinicia y vuelve a abrir Project Baseball.\n\n"
                              L"Detalles en datos\\log.txt.", det);
            MessageBoxW(NULL, m, L"Project Baseball", MB_OK | MB_ICONERROR);
            fflush(stdout);
            ExitProcess(1);
        }
    }
    r0 = GXM_OK;
}
void vita_hle_sceGxmTerminate(void) { r0 = GXM_OK; }

void vita_hle_sceGxmCreateContext(void) {
    uint32_t params = r0, out = r1;
    uint32_t self = obj_alloc(0x100);
    if (!self || !vita_mem_ptr(out, 4)) { r0 = GXM_ERR_OUT_OF_MEMORY; return; }
    for (int i = 0; i < MAX_CTX; i++) if (!g_ctx[i].used) {
        Ctx *c = &g_ctx[i];
        memset(c, 0, sizeof(*c));
        c->used = 1;
        c->self = self;
        c->vp_enable = 1;
        for (int k = 0; k < 2; k++) {
            c->depth_func[k] = 7; c->depth_write[k] = 1;          /* ALWAYS, escribe */
            c->st[k].func = 7; c->st[k].cmask = c->st[k].wmask = 0xFF;
        }
        printf("[gxm] contexto 0x%08X (hostMem=0x%08X +%u)\n", self, vita_read32(params), vita_read32(params + 4));
        fflush(stdout);
        vita_write32(out, self);
        r0 = GXM_OK;
        return;
    }
    r0 = GXM_ERR_OUT_OF_MEMORY;
}
void vita_hle_sceGxmDestroyContext(void) { Ctx *c = ctx_find(r0); if (c) c->used = 0; r0 = GXM_OK; }

void vita_hle_sceGxmGetRenderTargetMemSize(void) {
    uint32_t w = vita_read16(r0 + 4), h = vita_read16(r0 + 6);
    uint32_t size = 0x400000u + w * h * 4u;      /* bloque para el driver; el juego solo lo reserva */
    if (vita_mem_ptr(r1, 4)) vita_write32(r1, size);
    r0 = GXM_OK;
}
void vita_hle_sceGxmCreateRenderTarget(void) {
    uint32_t params = r0, out = r1;
    uint32_t self = obj_alloc(0x40);
    if (!self || !vita_mem_ptr(out, 4)) { r0 = GXM_ERR_OUT_OF_MEMORY; return; }
    vita_write32(self, vita_read16(params + 4));
    vita_write32(self + 4, vita_read16(params + 6));
    vita_write32(out, self);
    if (tr()) printf("[gxm] render target 0x%08X %ux%u\n", self, vita_read16(params + 4), vita_read16(params + 6));
    r0 = GXM_OK;
}
void vita_hle_sceGxmDestroyRenderTarget(void) { r0 = GXM_OK; }

void vita_hle_sceGxmSyncObjectCreate(void) {
    uint32_t self = obj_alloc(0x20);
    if (!self || !vita_mem_ptr(r0, 4)) { r0 = GXM_ERR_OUT_OF_MEMORY; return; }
    vita_write32(r0, self);
    r0 = GXM_OK;
}
void vita_hle_sceGxmSyncObjectDestroy(void) { r0 = GXM_OK; }

/* ========================================================================== */
/* Superficies                                                                */
/* ========================================================================== */

/* sceGxmColorSurfaceInit(surf, format, type, scale, outputRegSize, w, h, stride, data) */
void vita_hle_sceGxmColorSurfaceInit(void) {
    uint32_t self = r0, format = r1;
    uint32_t w = arg(1), h = arg(2), stride = arg(3), data = arg(4);
    Surf *s = surf_find(self);
    if (!s && g_nsurf < MAX_SURF) s = &g_surf[g_nsurf++];
    if (!s) { r0 = GXM_ERR_OUT_OF_MEMORY; return; }
    s->self = self; s->format = format; s->w = w; s->h = h; s->stride = stride; s->data = data; s->type = r2; s->gamma = 0;
    if (tr() || env_cached("MLB15_SUPERFICIES"))
        printf("[gxm] ColorSurface 0x%08X fmt=0x%08X tipo=%u escala=%u %ux%u stride=%u data=0x%08X\n", self, format, r2, r3, w, h, stride, data);
    r0 = GXM_OK;
}
/* SCE_GXM_COLOR_SURFACE_GAMMA_*: la GPU convierte la salida lineal a sRGB al escribir. */
void vita_hle_sceGxmColorSurfaceSetGammaMode(void) {
    Surf *s = surf_find(r0);
    if (tr() || env_cached("MLB15_GAMMA_TRAZA"))
        printf("[gxm] ColorSurfaceSetGammaMode(0x%08X, 0x%08X)%s\n", r0, r1, s ? "" : " (superficie desconocida)");
    if (s) s->gamma = r1 ? 1 : 0;
    r0 = GXM_OK;
}
/* Superficies de profundidad/stencil: solo importa si piden conservar su
 * contenido entre escenas (ForceLoad = 0x2). El estado va en la propia
 * estructura del juego (palabra 0, nuestra): nada que buscar en tablas. */
/* (surface, depthFormat, surfaceType, strideInSamples, depthData, stencilData): la palabra +8
 * guarda depthData, para dar la profundidad dibujada como textura (mapa de sombras). */
void vita_hle_sceGxmDepthStencilSurfaceInit(void) {
    if (vita_mem_ptr(r0, 12)) { vita_write32(r0, 0); vita_write32(r0 + 4, 0x44535046u); vita_write32(r0 + 8, arg(0)); }   /* 'FPSD' */
    r0 = GXM_OK;
}
void vita_hle_sceGxmDepthStencilSurfaceSetForceLoadMode(void) {
    if (vita_mem_ptr(r0, 4)) vita_write32(r0, (vita_read32(r0) & ~1u) | ((r1 & 2u) ? 1u : 0u));
    r0 = GXM_OK;
}
void vita_hle_sceGxmDepthStencilSurfaceSetForceStoreMode(void) { r0 = GXM_OK; }

/* ========================================================================== */
/* Escena                                                                     */
/* ========================================================================== */

/* Conversiones sRGB de GXM (MLB15_GAMMA=0 las quita). Solo con los shaders reales:
 * el shader genérico ya está ajustado a colores sin convertir. */
int port_display_is_framebuf(uint32_t addr);   /* hle_display.c */
static int v3k_enabled(void);
static int gamma_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = env_cached("MLB15_GAMMA"); on = (e ? (*e != '0') : 1) && v3k_enabled(); }
    return on;
}

/* sceGxmBeginScene(ctx, flags, rt, validRegion, vsync, fsync, colorSurface, depthStencil) */
void vita_hle_sceGxmBeginScene(void) {
    Ctx *c = ctx_find(r0);
    if (!c) { r0 = GXM_ERR_INVALID_VALUE; return; }
    Surf *s = surf_find(arg(2));
    c->in_scene = 1;
    g_scenes++;
    if (s) rec("escena %u: superficie 0x%08X %ux%u\n", g_scenes, s->data, s->w, s->h);
    if (s) scene_dump_check_time(s->w);
    extern unsigned port_display_frames(void);
    if (scene_dump()) printf("[escena] --- fot %u escena %u superficie 0x%08X %ux%u\n", port_display_frames(), g_scenes, s ? s->data : 0, s ? s->w : 0, s ? s->h : 0);
    if (tr() && !(s && s->data && s->w && s->h)) {
        static int n;
        if (n++ < 12) printf("[gxm] BeginScene con superficie desconocida 0x%08X (rt=0x%08X)\n", arg(2), r2);
    }
    if (env_cached("MLB15_DS_TRAZA") && !(s && s->data && s->w && s->h)) {
        static int n;
        uint32_t ds = arg(3), rt = r2;
        if (n++ < 20)
            printf("[ds] escena sin color: color 0x%08X ds 0x%08X (marca %08X datos 0x%08X) rt %ux%u\n", arg(2), ds,
                   ds && vita_mem_ptr(ds, 12) ? vita_read32(ds + 4) : 0, ds && vita_mem_ptr(ds, 12) ? vita_read32(ds + 8) : 0,
                   rt && vita_mem_ptr(rt, 8) ? vita_read32(rt) : 0, rt && vita_mem_ptr(rt, 8) ? vita_read32(rt + 4) : 0);
    }
    if (s && s->data && s->w && s->h) {
        c->tw = s->w; c->th = s->h;
        if (!c->vp_set) {                       /* viewport por defecto: toda la superficie */
            c->vp[0] = s->w * 0.5f; c->vp[1] = s->w * 0.5f;
            c->vp[2] = s->h * 0.5f; c->vp[3] = -(s->h * 0.5f);
            c->vp[4] = 0.5f; c->vp[5] = 0.5f;
        }
        uint32_t ds = arg(3);
        int load = ds && vita_mem_ptr(ds, 8) && vita_read32(ds + 4) == 0x44535046u && (vita_read32(ds) & 1u);
        uint32_t rt = r2, rw = 0, rh = 0;
        if (rt && vita_mem_ptr(rt, 8)) { rw = vita_read32(rt); rh = vita_read32(rt + 4); }
        /* Los búferes que van a la pantalla también se convierten a sRGB, como en Vita3K
         * (comparado píxel a píxel con capturas suyas el 2026-11-01: sin convertir, los
         * medios tonos salían muy oscuros y saturados, p. ej. la grada 41 frente a 79). El
         * 30-oct se quitó porque salía lavada, pero entonces la exposición automática aún
         * no leía ToneMapSample. MLB15_GAMMA_PANTALLA=0 deja la pantalla sin convertir. */
        int sg = s->gamma && gamma_on();
        static int gp = -1;
        if (gp < 0) { const char *e = env_cached("MLB15_GAMMA_PANTALLA"); gp = e ? atoi(e) : 1; }
        if (sg && !gp && port_display_is_framebuf(s->data)) sg = 0;
        gpu_scene_depth(load || (ds && vita_mem_ptr(ds, 12) && vita_read32(ds + 4) == 0x44535046u) ? vita_read32(ds + 8) : 0);
        gpu_scene_begin(s->data, s->w, s->h, s->stride, load, s->format | (sg ? 1u : 0u), s->type, rw, rh);
    } else {
        /* Escena solo de profundidad (sin superficie de color): el mapa de sombras de los
         * jugadores (2048x1024 en el partido; ShadowMapTexture la lee como F32). Antes se
         * descartaba y la ropa salía con manchas oscuras o el jugador entero a la sombra. Se
         * dibuja con un color ficticio que se queda en la GPU (dirección fuera de la memoria
         * guest) y su profundidad se da como textura (gpu_vk.c, depth_texture). Desactivado
         * por defecto ([video] sombra_jugadores, ver depth_texture en gpu_vk.c). */
        extern int gpu_sombra_jugadores(void);
        int on = gpu_sombra_jugadores();
        uint32_t ds = arg(3), rt = r2, rw = 0, rh = 0;
        if (rt && vita_mem_ptr(rt, 8)) { rw = vita_read32(rt); rh = vita_read32(rt + 4); }
        if (on && ds && vita_mem_ptr(ds, 12) && vita_read32(ds + 4) == 0x44535046u && vita_read32(ds + 8) && rw && rh &&
            rw <= 4096 && rh <= 4096) {
            uint32_t dd = vita_read32(ds + 8);
            int load = (vita_read32(ds) & 1u) != 0;
            c->tw = rw; c->th = rh;
            if (!c->vp_set) {
                c->vp[0] = rw * 0.5f; c->vp[1] = rw * 0.5f;
                c->vp[2] = rh * 0.5f; c->vp[3] = -(rh * 0.5f);
                c->vp[4] = 0.5f; c->vp[5] = 0.5f;
            }
            gpu_scene_depth(dd);
            gpu_scene_begin(dd | 0x40000000u, rw, rh, rw, load, 0x00000000u /* U8U8U8U8 */, 1 /* residente */, rw, rh);
        }
    }
    r0 = GXM_OK;
}

uint32_t gpu_target_addr(void);
void vita_hle_sceGxmEndScene(void) {
    Ctx *c = ctx_find(r0);
    if (!c) { r0 = GXM_ERR_INVALID_VALUE; return; }
    uint32_t vn = r1, fn = r2;
    {
        extern uint32_t g_dbg_ubaddr;
        static int n;
        if (g_dbg_ubaddr && n++ < 6)
            printf("[sa] en EndScene: ub+104 = %08X %08X\n", vita_read32(g_dbg_ubaddr + 104), vita_read32(g_dbg_ubaddr + 108));
    }
    if (env_cached("MLB15_NOTIF_TRAZA")) {   /* qué escenas llevan notificación (superficie, vértice/fragmento) */
        static struct { uint32_t a; unsigned n, nv, nf; } h[32];
        static unsigned tot;
        uint32_t a = gpu_target_addr();
        int k;
        for (k = 0; k < 32 && h[k].n && h[k].a != a; k++) {}
        if (k < 32) { h[k].a = a; h[k].n++; h[k].nv += vn != 0; h[k].nf += fn != 0; }
        if (++tot % 3000 == 0) {
            printf("[notif]");
            for (k = 0; k < 32 && h[k].n; k++) printf(" 0x%08X:%u(v%u f%u)", h[k].a, h[k].n, h[k].nv, h[k].nf);
            printf("\n");
            memset(h, 0, sizeof(h));
        }
    }
    if (c->in_scene) gpu_scene_end();
    scene_dump_end();
    c->in_scene = 0;
    /* Notificaciones: la GPU escribe el valor en la dirección; aquí ya terminó. */
    if (vn && vita_mem_ptr(vn, 8)) { uint32_t a = vita_read32(vn), v = vita_read32(vn + 4); if (a) vita_write32(a, v); }
    if (fn && vita_mem_ptr(fn, 8)) { uint32_t a = vita_read32(fn), v = vita_read32(fn + 4); if (a) vita_write32(a, v); }
    if ((g_scenes % 1000) == 0 && real_enabled()) {
        printf("[usse] programas traducidos %lu, sin traducir %lu\n", g_real_ok, g_real_bad);
        fflush(stdout);
    }
    if ((tr() || env_cached("MLB15_DESCARTES")) && (g_scenes % 1000) == 0) {
        uint32_t sc, dr, tx;
        gpu_stats(&sc, &dr, &tx);
        printf("[gxm] %lu escenas, %lu draws (%lu sin dibujar); gpu: %u escenas %u draws %u texturas\n",
               g_scenes, g_draws, g_skipped, sc, dr, tx);
        printf("[gxm]   %llu vertices, %llu indices; mayor draw %u vertices %u indices (gxp 0x%08X)\n",
               g_stat_verts, g_stat_idx, g_stat_maxv, g_stat_maxi, g_stat_maxgxp);
        g_stat_verts = g_stat_idx = 0; g_stat_maxv = 0;
        printf("[gxm]   descartes: prog/vacio %lu primitiva %lu memoria %lu indice %lu vertices %lu instancias %lu\n",
               g_skip_why[0], g_skip_why[1], g_skip_why[2], g_skip_why[3], g_skip_why[4], g_skip_why[5]);
        printf("[gxm]     desglose [0]: basura %lu skin %lu color_is_data %lu\n", g_skip_junk, g_skip_skin, g_skip_color);
        fflush(stdout);
    }
    r0 = GXM_OK;
}
void vita_hle_sceGxmMidSceneFlush(void) { r0 = GXM_OK; }
void gpu_sync_all_m(int motivo);
void vita_hle_sceGxmFinish(void) { gpu_sync_all_m(7); r0 = GXM_OK; }
void vita_hle_sceGxmWaitEvent(void) { r0 = GXM_OK; }
void vita_hle_sceGxmNotificationWait(void) { r0 = GXM_OK; }
void vita_hle_sceGxmGetNotificationRegion(void) {
    static uint32_t region;
    if (!region) region = obj_alloc(0x1000);
    r0 = region;
}
void vita_hle_sceGxmPadHeartbeat(void) { r0 = GXM_OK; }

/* ========================================================================== */
/* Estado                                                                     */
/* ========================================================================== */

void vita_hle_sceGxmSetVertexProgram(void)   { Ctx *c = ctx_find(r0); if (c) c->vprog = r1; r0 = 0; }
void vita_hle_sceGxmSetFragmentProgram(void) { Ctx *c = ctx_find(r0); if (c) { c->fprog = r1; c->pfs = 0; } r0 = 0; }
/* MLB15_GXM_ESCENA=n: lista todos los draws de la escena n. Con "45s": la
 * primera escena sobre la pantalla (960 de ancho) pasados 45 s del arranque. */
static long g_dump_scene = -2;
static unsigned long g_dump_draws0;
static int g_dump_retry;
/* Con "Ns": si la escena elegida no tuvo draws (copias, borrados), probar la siguiente del mismo ancho. */
static void scene_dump_end(void);
static double g_dump_secs = -1;
static long g_dump_frame = -1;
static int scene_dump(void) {
    if (g_dump_scene == -2) {
        const char *e = env_cached("MLB15_GXM_ESCENA");
        g_dump_scene = -1;
        if (e && strchr(e, 'f')) g_dump_frame = atol(e);   /* "Nf": primera escena tras el fotograma N */
        else if (e && strchr(e, 's')) g_dump_secs = atof(e);
        else if (e) g_dump_scene = atol(e);
    }
    static long cuenta = -1;   /* MLB15_GXM_ESCENA_CUENTA=n: n escenas seguidas */
    if (cuenta < 0) { const char *e = env_cached("MLB15_GXM_ESCENA_CUENTA"); cuenta = e ? atol(e) : 1; if (cuenta < 1) cuenta = 1; }
    return g_dump_scene >= 0 && (long)g_scenes >= g_dump_scene && (long)g_scenes < g_dump_scene + cuenta;
}
static void scene_dump_check_time(uint32_t width) {
    static ULONGLONG t0;
    if (!t0) t0 = GetTickCount64();
    scene_dump();
    static uint32_t want_w;          /* MLB15_GXM_ESCENA_ANCHO: el partido dibuja a 720 */
    if (!want_w) { const char *e = env_cached("MLB15_GXM_ESCENA_ANCHO"); want_w = e ? (uint32_t)atoi(e) : 960; }
    extern unsigned port_display_frames(void);
    if (g_dump_frame >= 0 && width == want_w && (long)port_display_frames() >= g_dump_frame) { g_dump_frame = -1; g_dump_secs = 0; }
    if (g_dump_secs >= 0 && width == want_w && (GetTickCount64() - t0) / 1000.0 >= g_dump_secs) {
        g_dump_scene = (long)g_scenes;
        g_dump_secs = -1;
        g_dump_draws0 = g_draws;
        g_dump_retry = 1;
        printf("[escena] volcando la escena %ld\n", g_dump_scene);
    }
}

void vita_hle_sceGxmSetPrecomputedFragmentState(void) {
    Ctx *c = ctx_find(r0);
    if (scene_dump()) printf("[escena] SetPrecomputedFragmentState(0x%08X)\n", r1);
    if (c) c->pfs = pc_data(r1);   /* nuestros datos (ver "Estado precalculado") */
    r0 = 0;
}

void vita_hle_sceGxmSetVertexStream(void) {
    Ctx *c = ctx_find(r0);
    if (c && r1 < 16) c->streams[r1] = r2;
    r0 = GXM_OK;
}
void vita_hle_sceGxmSetFragmentTexture(void) {
    Ctx *c = ctx_find(r0);
    if (scene_dump()) printf("[escena] SetFragmentTexture(%u, struct 0x%08X -> datos 0x%08X %ux%u fmt 0x%08X)\n", r1, r2,
                             vita_read32(r2 + 8), vita_read32(r2 + 4) & 0xFFFF, vita_read32(r2 + 4) >> 16, vita_read32(r2));
    if (c && r1 < 16 && vita_mem_ptr(r2, 16)) memcpy(c->ftex[r1], vita_mem_ptr(r2, 16), 16);
    r0 = GXM_OK;
}
void vita_hle_sceGxmSetVertexUniformBuffer(void)   {
    Ctx *c = ctx_find(r0);
    if (tr() && r2 && r2 < 0x81000000u) {
        static int shown;
        if (shown++ < 20) {
            /* llamado desde sub_813E5690(obj): r7 = obj, UB3 = obj+0xC */
            /* pila: push de sub_813E468E (16) + marco de sub_813E5690 (0x9C + lr en +32) */
            printf("[gxm] SetVertexUniformBuffer(%u, 0x%08X) puntero raro, r7=0x%08X llamador=0x%08X:", r1, r2, r7,
                   vita_read32(sp + 16 + 0x9C + 32));
            if (vita_mem_ptr(r7, 0x40)) for (uint32_t k = 0; k < 0x40; k += 4) printf(" %08X", vita_read32(r7 + k));
            printf(" | retornos en pila:");
            for (uint32_t k = 0; k < 0x200; k += 4) {
                uint32_t w = vita_read32(sp + k);
                if ((w & 1) && w >= 0x81000000u && w < 0x81C80000u) printf(" +%X:%08X", k, w);
            }
            printf("\n");
        }
    }
    if (c && r1 < 16) c->vub[r1] = r2;
    r0 = GXM_OK;
}
void vita_hle_sceGxmSetFragmentUniformBuffer(void) { Ctx *c = ctx_find(r0); if (c && r1 < 16) c->fub[r1] = r2; r0 = GXM_OK; }

static uint32_t prog_gxp_v(uint32_t vprog) { VProg *v = vp_find(vprog); return v ? v->gxp : 0; }
static uint32_t prog_gxp_f(uint32_t fprog) { FProg *f = fp_find(fprog); return f ? f->gxp : 0; }

void vita_hle_sceGxmReserveVertexDefaultUniformBuffer(void) {
    Ctx *c = ctx_find(r0);
    if (!c || !vita_mem_ptr(r1, 4)) { r0 = GXM_ERR_INVALID_VALUE; return; }
    uint32_t size = gxp_default_ub(prog_gxp_v(c->vprog));
    c->vdef = ring_alloc(c, size ? size : 256);
    vita_write32(r1, c->vdef);
    r0 = c->vdef ? GXM_OK : GXM_ERR_OUT_OF_MEMORY;
}
void vita_hle_sceGxmReserveFragmentDefaultUniformBuffer(void) {
    Ctx *c = ctx_find(r0);
    if (!c || !vita_mem_ptr(r1, 4)) { r0 = GXM_ERR_INVALID_VALUE; return; }
    uint32_t g = c->pfs ? prog_gxp_f(vita_read32(c->pfs)) : prog_gxp_f(c->fprog);
    uint32_t size = gxp_default_ub(g);
    c->fdef = ring_alloc(c, size ? size : 256);
    vita_write32(r1, c->fdef);
    r0 = c->fdef ? GXM_OK : GXM_ERR_OUT_OF_MEMORY;
}

static uint16_t to_half(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u, e = (x >> 23) & 0xFF, m = x & 0x7FFFFFu;
    if (e == 0xFF) return (uint16_t)(sign | 0x7C00u | (m ? 0x200u : 0));
    int ne = (int)e - 127 + 15;
    if (ne >= 31) return (uint16_t)(sign | 0x7C00u);
    if (ne <= 0) {
        if (ne < -10) return (uint16_t)sign;
        m |= 0x800000u;
        return (uint16_t)(sign | (m >> (14 - ne)));
    }
    return (uint16_t)(sign | ((uint32_t)ne << 10) | (m >> 13));
}

/* sceGxmSetUniformDataF(buffer, param, componentOffset, componentCount, src) */
void vita_hle_sceGxmSetUniformDataF(void) {
    uint32_t buf = r0, p = r1, off = r2, n = r3, src = arg(0);
    if (!buf || !p || !src || !n) { r0 = GXM_ERR_INVALID_POINTER; return; }
    uint32_t base = buf + p_res(p) * 4u;
    if (p_type(p) == 1) {                                    /* F16 */
        for (uint32_t i = 0; i < n; i++) {
            vfp_reg v;
            v.u = vita_read32(src + i * 4);
            vita_write16(base + (off + i) * 2u, to_half(v.f));
        }
    } else {                                                 /* F32 (y el resto como 32 bits) */
        for (uint32_t i = 0; i < n; i++) vita_write32(base + (off + i) * 4u, vita_read32(src + i * 4));
    }
    r0 = GXM_OK;
}

/* sceGxmSetViewport(ctx, xOffset, xScale, yOffset, yScale, zOffset, zScale): floats en s0-s5 */
void vita_hle_sceGxmSetViewport(void) {
    Ctx *c = ctx_find(r0);
    if (c) {
        for (int i = 0; i < 6; i++) c->vp[i] = vfp_getf(i);
        c->vp_set = 1;
        if (tr()) { static int n; if (n++ < 2) {
            vfp_reg a[4];
            a[0].u = r1; a[1].u = r2; a[2].u = r3; a[3].u = arg(0);
            printf("[gxm] viewport s0-s7: %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f | r1-r3,[sp]: %.2f %.2f %.2f %.2f\n",
                   vfp_getf(0), vfp_getf(1), vfp_getf(2), vfp_getf(3), vfp_getf(4), vfp_getf(5), vfp_getf(6),
                   vfp_getf(7), a[0].f, a[1].f, a[2].f, a[3].f); } }
    }
    r0 = 0;
}
void vita_hle_sceGxmSetViewportEnable(void) { Ctx *c = ctx_find(r0); if (c) c->vp_enable = (r1 == 0); r0 = 0; }
/* SCE_GXM_CULL_NONE 0, CW 1, CCW 2 */
void vita_hle_sceGxmSetCullMode(void) { Ctx *c = ctx_find(r0); if (c) c->cull = (uint8_t)(r1 & 3); r0 = 0; }
/* SCE_GXM_TWO_SIDED_ENABLED = 0x800; si no, la cara de atrás usa el estado de delante */
void vita_hle_sceGxmSetTwoSidedEnable(void) { Ctx *c = ctx_find(r0); if (c) c->two_sided = r1 ? 1 : 0; r0 = 0; }
void vita_hle_sceGxmSetFrontPolygonMode(void) { r0 = 0; }
/* SceGxmDepthFunc: (v >> 22) & 7 = VkCompareOp */
static void depth_func(int face) { Ctx *c = ctx_find(r0); if (c) c->depth_func[face] = (uint8_t)((r1 >> 22) & 7); r0 = 0; }
void vita_hle_sceGxmSetFrontDepthFunc(void) { depth_func(0); }
void vita_hle_sceGxmSetBackDepthFunc(void)  { depth_func(1); }
/* SCE_GXM_DEPTH_WRITE_DISABLED = 0x00100000 */
static void depth_write(int face) { Ctx *c = ctx_find(r0); if (c) c->depth_write[face] = (r1 & 0x00100000u) ? 0 : 1; r0 = 0; }
void vita_hle_sceGxmSetFrontDepthWriteEnable(void) { depth_write(0); }
void vita_hle_sceGxmSetBackDepthWriteEnable(void)  { depth_write(1); }
/* (ctx, factor, units): sesgo de profundidad. Lo usa la pasada del mapa de sombras de los
 * jugadores; sin él la cara iluminada se hacía sombra a sí misma (jugador entero oscuro). */
void vita_hle_sceGxmSetFrontDepthBias(void) { Ctx *c = ctx_find(r0); if (c) { c->dbias_factor[0] = (int32_t)r1; c->dbias_units[0] = (int32_t)r2; } r0 = 0; }
void vita_hle_sceGxmSetBackDepthBias(void) { Ctx *c = ctx_find(r0); if (c) { c->dbias_factor[1] = (int32_t)r1; c->dbias_units[1] = (int32_t)r2; } r0 = 0; }
/* (ctx, func, stencilFail, depthFail, depthPass, compareMask, writeMask)
 * SceGxmStencilFunc: (v >> 25) & 7 = VkCompareOp; SceGxmStencilOp: 0-7 = VkStencilOp */
static void stencil_func(int face) {
    Ctx *c = ctx_find(r0);
    if (c) {
        c->st[face].func = (uint8_t)((r1 >> 25) & 7);
        c->st[face].fail = (uint8_t)(r2 & 7);
        c->st[face].zfail = (uint8_t)(r3 & 7);
        c->st[face].pass = (uint8_t)(arg(0) & 7);
        c->st[face].cmask = (uint8_t)arg(1);
        c->st[face].wmask = (uint8_t)arg(2);
    }
    r0 = 0;
}
void vita_hle_sceGxmSetFrontStencilFunc(void) { stencil_func(0); }
void vita_hle_sceGxmSetBackStencilFunc(void)  { stencil_func(1); }
void vita_hle_sceGxmSetFrontStencilRef(void) { Ctx *c = ctx_find(r0); if (c) c->st[0].ref = (uint8_t)r1; r0 = 0; }
void vita_hle_sceGxmSetBackStencilRef(void)  { Ctx *c = ctx_find(r0); if (c) c->st[1].ref = (uint8_t)r1; r0 = 0; }
/* SCE_GXM_FRAGMENT_PROGRAM_DISABLED = 0x00200000: solo profundidad/stencil */
void vita_hle_sceGxmSetFrontFragmentProgramEnable(void) { Ctx *c = ctx_find(r0); if (c) c->frag_off = (r1 & 0x00200000u) ? 1 : 0; r0 = 0; }

/* ========================================================================== */
/* Memoria                                                                    */
/* ========================================================================== */

void vita_hle_sceGxmMapMemory(void) { r0 = GXM_OK; }
void vita_hle_sceGxmUnmapMemory(void) { r0 = GXM_OK; }
void vita_hle_sceGxmMapVertexUsseMemory(void) { r0 = GXM_OK; }
void vita_hle_sceGxmUnmapVertexUsseMemory(void) { r0 = GXM_OK; }
void vita_hle_sceGxmMapFragmentUsseMemory(void) { r0 = GXM_OK; }
void vita_hle_sceGxmUnmapFragmentUsseMemory(void) { r0 = GXM_OK; }

/* ========================================================================== */
/* Shader patcher                                                             */
/* ========================================================================== */

static uint32_t g_patcher_user;

void vita_hle_sceGxmShaderPatcherCreate(void) {
    uint32_t params = r0, out = r1;
    uint32_t self = obj_alloc(0x40);
    if (!self || !vita_mem_ptr(out, 4)) { r0 = GXM_ERR_OUT_OF_MEMORY; return; }
    g_patcher_user = vita_read32(params);
    vita_write32(out, self);
    r0 = GXM_OK;
}
void vita_hle_sceGxmShaderPatcherDestroy(void) { r0 = GXM_OK; }
void vita_hle_sceGxmShaderPatcherSetUserData(void) { g_patcher_user = r1; r0 = GXM_OK; }
void vita_hle_sceGxmShaderPatcherGetUserData(void) { r0 = g_patcher_user; }

/* El id de un programa registrado es el propio GXP. */
void vita_hle_sceGxmShaderPatcherRegisterProgram(void) {
    if (vita_mem_ptr(r2, 4)) vita_write32(r2, r1);
    r0 = GXM_OK;
}
void vita_hle_sceGxmShaderPatcherUnregisterProgram(void) { r0 = GXM_OK; }
void vita_hle_sceGxmShaderPatcherGetProgramFromId(void) { /* r0 = id = GXP */ }

/* (patcher, id, attributes, attributeCount, streams, streamCount, out) */
void vita_hle_sceGxmShaderPatcherCreateVertexProgram(void) {
    uint32_t gxp = r1, attrs = r2, na = r3, streams = arg(0), ns = arg(1), out = arg(2);
    if (g_nvp >= MAX_PROGS || !vita_mem_ptr(out, 4)) { r0 = GXM_ERR_OUT_OF_MEMORY; return; }
    if (tr() && na == 0) {
        static int shown;
        if (shown++ < 3) {
            /* Locales de sub_813E2DEE en el momento de la llamada (el HLE corre sobre su pila). */
            printf("[gxm] CreateVertexProgram sin atributos: gxp=0x%08X streams=%u | [sp+0x29c]=%u [sp+0x2b8]=%u "
                   "[sp+0x2a8]=%u [sp+0x2c8]=%u decl=0x%08X\n", gxp, ns, vita_read32(sp + 0x29c),
                   vita_read32(sp + 0x2b8), vita_read32(sp + 0x2a8), vita_read32(sp + 0x2c8), vita_read32(sp + 0x2ac));
        }
    }
    VProg *v = &g_vp[g_nvp];
    memset(v, 0, sizeof(*v));
    v->self = obj_alloc(0x20);
    v->gxp = gxp;
    vita_write32(v->self, gxp);
    v->nattr = (int)(na > 16 ? 16 : na);
    v->nstream = (int)(ns > 16 ? 16 : ns);
    for (int i = 0; i < v->nattr; i++) {
        uint32_t a = attrs + 8u * (uint32_t)i;
        Attr *x = &v->attr[i];
        x->stream = vita_read16(a);
        x->offset = vita_read16(a + 2);
        x->format = vita_read8(a + 4);
        x->comps  = vita_read8(a + 5);
        x->reg    = vita_read16(a + 6);
        x->slot   = (int8_t)slot_for(gxp, x->reg);
        if (x->slot == SLOT_COL) v->has_color = 1;
        if (x->slot == SLOT_NRM) v->has_normal = 1;
    }
    for (int i = 0; i < v->nstream; i++) {
        v->stride[i] = vita_read16(streams + 4u * (uint32_t)i);
        v->index_source[i] = vita_read16(streams + 4u * (uint32_t)i + 2);
    }
    v->mvp = gxp_find(gxp, "UVP_ModelViewProjectionMatrix");
    v->skin = gxp_find(gxp, "UVP_SkinningMatrix");
    /* Tierra del montículo y del cajón de bateo: Color lleva parámetros de la
     * deformación (sale (0..0.25, 1, 1)): como color pintaba un recuadro cian. */
    /* Capas del montículo/cajón (Sprite 32/44): Color son parámetros. Las zapatillas también leen
     * UVP_MoundParameters (se apoyan en el montículo) pero tienen huesos y no Color: esas sí se dibujan. */
    v->color_is_data = gxp_find(gxp, "UVP_MoundParameters") && gxp_find(gxp, "Color") && !gxp_find(gxp, "UVP_SkinningMatrix");
    v->light_dir = gxp_find(gxp, "UVP_LightDirSun");
    v->mound_pos = gxp_find(gxp, "UVP_MoundPosition");
    v->mound_par = gxp_find(gxp, "UVP_MoundParameters");
    if (!v->mvp) { v->vpm = gxp_find(gxp, "UVP_ViewProjectionMatrix"); v->model = gxp_find(gxp, "UVP_ModelMatrix"); }
    prog_hash_put(g_vp_h, v->self, g_nvp, 0);
    g_nvp++;
    vita_write32(out, v->self);
    if (tr()) {
        printf("[gxm] VertexProgram 0x%08X gxp=0x%08X attrs=%d:", v->self, gxp, v->nattr);
        for (int i = 0; i < v->nattr; i++)
            printf(" [s%u+%u f%u x%u r%u ->%d]", v->attr[i].stream, v->attr[i].offset, v->attr[i].format,
                   v->attr[i].comps, v->attr[i].reg, v->attr[i].slot);
        printf(" mvp=%s\n", v->mvp ? "si" : v->vpm ? "VP*M" : "no");
        uint32_t n = gxp_nparams(gxp), base = gxp_params(gxp);
        printf("[gxm]   params:");
        for (uint32_t i = 0; i < n && i < 64; i++) {
            uint32_t p = base + 16 * i;
            char nm[64];
            p_name_str(p, nm, sizeof(nm));
            printf(" %s(c%u t%u x%u a%u r%u k%u s%u)", nm, p_cat(p), p_type(p), p_comps(p), vita_read32(p + 8),
                   p_res(p), p_cont(p), vita_read16(p + 6) & 0xFF);
        }
        printf("\n");
    }
    r0 = GXM_OK;
}

/* FNV-1a del GXP entero (tamaño en +8): identifica un shader sin depender de dónde se cargó. */
static uint32_t gxp_huella(uint32_t gxp) {
    uint32_t n = vita_read32(gxp + 8), h = 2166136261u;
    if (n > 1u << 20 || !vita_mem_ptr(gxp, n)) return 0;
    for (uint32_t i = 0; i < n; i++) h = (h ^ vita_read8(gxp + i)) * 16777619u;
    return h;
}

/* (patcher, id, outputFormat, msaa, blendInfo, vertexProgramGxp, out) */
void vita_hle_sceGxmShaderPatcherCreateFragmentProgram(void) {
    uint32_t gxp = r1, blend = arg(0), out = arg(2);
    if (g_nfp >= MAX_PROGS || !vita_mem_ptr(out, 4)) { r0 = GXM_ERR_OUT_OF_MEMORY; return; }
    FProg *f = &g_fp[g_nfp];
    memset(f, 0, sizeof(*f));
    f->self = obj_alloc(0x20);
    f->gxp = gxp;
    vita_write32(f->self, gxp);
    f->mask = 0xF;
    if (blend && vita_mem_ptr(blend, 4)) {
        uint8_t b0 = vita_read8(blend), b1 = vita_read8(blend + 1), b2 = vita_read8(blend + 2), b3 = vita_read8(blend + 3);
        f->mask = b0;
        f->color_func = b1 & 15; f->alpha_func = b1 >> 4;
        f->color_src = b2 & 15;  f->color_dst = b2 >> 4;
        f->alpha_src = b3 & 15;  f->alpha_dst = b3 >> 4;
        f->blend = (f->color_func || f->alpha_func) ? 1 : 0;
    }
    f->huella = gxp_huella(gxp);
    {   /* MLB15_SIN_MEZCLA=huella[,huella...] (hex): crea esos fragmentos sin mezcla (pruebas) */
        const char *e = env_cached("MLB15_SIN_MEZCLA");
        char hx[12];
        snprintf(hx, sizeof(hx), "%08X", f->huella);
        if (e && strstr(e, hx)) f->blend = 0;
    }
    /* La unidad de textura que usan los shaders genéricos: el primer sampler. */
    f->sampler = -1;
    uint32_t n = gxp_nparams(gxp), base = gxp_params(gxp);
    for (uint32_t i = 0; i < n && i < 256; i++)
        if (p_cat(base + 16 * i) == 2) { f->sampler = (int)p_res(base + 16 * i); break; }
    f->konst = gxp_find(gxp, "ClearColor");
    /* Suelo con sombras de jugadores: ahí el color de vértice lleva parámetros de
     * la sombra (sale ~(0.8, 1, 1)); multiplicarlo pintaba manchas cian bajo cada jugador. */
    f->no_vcolor = gxp_find(gxp, "PlayerShadowsTarget") != 0;
    f->light_dir = gxp_find(gxp, "UFP_LightDirSun");
    f->light_col = gxp_find(gxp, "UFP_LightColorAndLightMult");
    f->amb_col = gxp_find(gxp, "UFP_QuickAmbientColor");
    f->amb_mult = gxp_find(gxp, "UFP_LightDirShadowMapAndAmbMult");
    {
        static const char *const nm[7] = { "UFP_SRC_Ck", "UFP_SRC_CSa", "UFP_SRC_CDa", "UFP_SRC_CDc",
                                           "UFP_DST_Ck", "UFP_DST_CSa", "UFP_DST_CSc" };
        for (int i = 0; i < 7; i++) f->ufp[i] = gxp_find(gxp, nm[i]);
    }
    prog_hash_put(g_fp_h, f->self, g_nfp, 1);
    g_nfp++;
    vita_write32(out, f->self);
    static uint32_t fp_info = 1;   /* MLB15_FP_INFO=gxp (hex): parámetros y mezcla de ese fragmento */
    if (fp_info == 1) { const char *e = env_cached("MLB15_FP_INFO"); fp_info = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
    if (env_cached("MLB15_FP_TODOS"))   /* una línea por fragmento creado: quién lo crea, mezcla y huella */
        printf("[fp] gxp=0x%08X lr=0x%08X tam=%u huella=%08X blend=%08X r2=%u r3=%u args=%08X %08X\n", gxp, lr, vita_read32(gxp + 8),
               gxp_huella(gxp), blend && vita_mem_ptr(blend, 4) ? vita_read32(blend) : 0xFFFFFFFFu, r2, r3, arg(1), arg(2));
    if (tr() || (fp_info && gxp == fp_info)) {
        printf("[gxm] (tamaño %u, huella %08X, blendInfo 0x%08X = %08X) ", vita_read32(gxp + 8), gxp_huella(gxp), blend,
               blend && vita_mem_ptr(blend, 4) ? vita_read32(blend) : 0);
        printf("[gxm] FragmentProgram 0x%08X gxp=0x%08X blend=%u (%u,%u,%u / %u,%u,%u) mask=0x%X tex=%d params:",
               f->self, gxp, f->blend, f->color_func, f->color_src, f->color_dst,
               f->alpha_func, f->alpha_src, f->alpha_dst, f->mask, f->sampler);
        for (uint32_t i = 0; i < n && i < 32; i++) {
            char nm[48];
            p_name_str(base + 16 * i, nm, sizeof(nm));
            printf(" %s(c%u r%u k%u)", nm, p_cat(base + 16 * i), p_res(base + 16 * i), p_cont(base + 16 * i));
        }
        printf("\n");
    }
    r0 = GXM_OK;
}
void vita_hle_sceGxmShaderPatcherReleaseVertexProgram(void) { r0 = GXM_OK; }
void vita_hle_sceGxmShaderPatcherReleaseFragmentProgram(void) { r0 = GXM_OK; }

/* ========================================================================== */
/* Consultas de programa (parámetros reales del GXP)                          */
/* ========================================================================== */

void vita_hle_sceGxmProgramFindParameterByName(void) {
    char name[96];
    uint32_t g = r0;
    port_guest_str(r1, name, sizeof(name));
    r0 = gxp_find(g, name);
    if (tr()) {
        static int shown;
        if (shown < 40 && g >= 0x83000000u) {
            shown++;
            printf("[gxm] FindParameterByName(gxp=0x%08X '%s') -> 0x%08X cat=%u (params=%u magic=%08X)\n",
                   g, name, r0, r0 ? p_cat(r0) : 99, gxp_nparams(g), vita_read32(g));
        }
    }
}
void vita_hle_sceGxmProgramParameterGetCategory(void)       { r0 = r0 ? p_cat(r0) : 0; }
void vita_hle_sceGxmProgramParameterGetType(void)           { r0 = r0 ? p_type(r0) : 0; }
void vita_hle_sceGxmProgramParameterGetComponentCount(void) { r0 = r0 ? p_comps(r0) : 0; }
void vita_hle_sceGxmProgramParameterGetArraySize(void)      { r0 = r0 ? vita_read32(r0 + 8) : 0; }
void vita_hle_sceGxmProgramParameterGetResourceIndex(void)  { r0 = r0 ? p_res(r0) : 0; }
void vita_hle_sceGxmProgramParameterGetContainerIndex(void) { r0 = r0 ? p_cont(r0) : 0; }
void vita_hle_sceGxmProgramParameterGetName(void)           { r0 = r0 ? p_name(r0) : 0; }
void vita_hle_sceGxmProgramParameterGetSemantic(void)       { r0 = r0 ? (vita_read16(r0 + 6) & 0xFF) : 0; }
void vita_hle_sceGxmProgramParameterGetSemanticIndex(void)  { r0 = r0 ? ((vita_read16(r0 + 6) >> 8) & 15) : 0; }
void vita_hle_sceGxmProgramParameterGetIndex(void) {
    /* (program, parameter) */
    r0 = (r1 && r0) ? (r1 - gxp_params(r0)) / 16u : 0;
}
void vita_hle_sceGxmProgramGetParameterCount(void) { r0 = r0 ? gxp_nparams(r0) : 0; }
void vita_hle_sceGxmProgramGetParameter(void) {
    r0 = (r0 && r1 < gxp_nparams(r0)) ? gxp_params(r0) + 16u * r1 : 0;
}
void vita_hle_sceGxmProgramGetDefaultUniformBufferSize(void) { r0 = r0 ? gxp_default_ub(r0) : 0; }
/* Bit 7 de program_flags (+0x14). Antes devolvía siempre 1: el juego (0x813E3494) quita la
 * mezcla del material cuando el shader lee el color, así que casi todos sus fragmentos se
 * creaban sin blendInfo y salían opacos (zona caliente/fría, guante del lanzamiento, marca
 * de la bola). MLB15_FRAGCOLOR_SIEMPRE=1 vuelve a lo anterior. */
void vita_hle_sceGxmProgramIsFragColorUsed(void) {
    static int siempre = -1;
    if (siempre < 0) { const char *e = env_cached("MLB15_FRAGCOLOR_SIEMPRE"); siempre = e && *e == '1'; }
    r0 = siempre ? 1 : (r0 && vita_mem_ptr(r0, 0x18) ? (vita_read32(r0 + 0x14) >> 7) & 1u : 0);
}

/* ========================================================================== */
/* Texturas: codificación propia dentro de los 16 bytes de SceGxmTexture       */
/*   w0 formato   w1 ancho | alto<<16   w2 datos                              */
/*   w3 tipo:3 mips:4 min:1 mag:1 u:3 v:3 (bits 0-14) | gamma:1 (bit 15)      */
/*      | stride/4 << 17                                                       */
/* ========================================================================== */

#define TEX_SET_W3(mask, shift, val) vita_write32(r0 + 12, (vita_read32(r0 + 12) & ~((uint32_t)(mask) << (shift))) | (((uint32_t)(val) & (mask)) << (shift)))

/* Paletas por dirección de datos (no caben en los 16 bytes). */
#define MAX_PAL 1024
static uint32_t g_pal_data[MAX_PAL], g_pal_addr[MAX_PAL];
static int g_npal;

static void tex_init(uint32_t t, uint32_t data, uint32_t fmt, uint32_t w, uint32_t h, uint32_t mips, uint32_t type) {
    if (!vita_mem_ptr(t, 16)) return;
    vita_write32(t, fmt);
    vita_write32(t + 4, (w & 0xFFFF) | (h << 16));
    vita_write32(t + 8, data);
    vita_write32(t + 12, (type & 7) | ((mips & 15) << 3) | (1u << 7) | (1u << 8) | (2u << 9) | (2u << 12));
}

void vita_hle_sceGxmTextureInitSwizzled(void)          { tex_init(r0, r1, r2, r3, arg(0), arg(1), GPU_TEX_SWIZZLED); r0 = GXM_OK; }
void vita_hle_sceGxmTextureInitSwizzledArbitrary(void) { tex_init(r0, r1, r2, r3, arg(0), arg(1), GPU_TEX_SWIZZLED); r0 = GXM_OK; }
void vita_hle_sceGxmTextureInitLinear(void)            { tex_init(r0, r1, r2, r3, arg(0), arg(1), GPU_TEX_LINEAR); r0 = GXM_OK; }
void vita_hle_sceGxmTextureInitTiled(void)             { tex_init(r0, r1, r2, r3, arg(0), arg(1), GPU_TEX_TILED); r0 = GXM_OK; }
void vita_hle_sceGxmTextureInitCube(void)              { tex_init(r0, r1, r2, r3, arg(0), arg(1), GPU_TEX_CUBE); r0 = GXM_OK; }
void vita_hle_sceGxmTextureInitCubeArbitrary(void)     { tex_init(r0, r1, r2, r3, arg(0), arg(1), GPU_TEX_CUBE); r0 = GXM_OK; }
/* (tex, data, format, width, height, byteStride) */
void vita_hle_sceGxmTextureInitLinearStrided(void) {
    uint32_t t = r0, stride = arg(1);
    tex_init(t, r1, r2, r3, arg(0), 1, GPU_TEX_LINEAR_STRIDED);
    if (vita_mem_ptr(t, 16)) vita_write32(t + 12, (vita_read32(t + 12) & 0x1FFFFu) | ((stride / 4u) << 17));
    r0 = GXM_OK;
}

void vita_hle_sceGxmTextureSetFormat(void) { vita_write32(r0, r1); r0 = GXM_OK; }
void vita_hle_sceGxmTextureSetWidth(void)  { vita_write32(r0 + 4, (vita_read32(r0 + 4) & 0xFFFF0000u) | (r1 & 0xFFFF)); r0 = GXM_OK; }
void vita_hle_sceGxmTextureSetHeight(void) { vita_write32(r0 + 4, (vita_read32(r0 + 4) & 0xFFFFu) | (r1 << 16)); r0 = GXM_OK; }
void vita_hle_sceGxmTextureSetData(void)   { vita_write32(r0 + 8, r1); r0 = GXM_OK; }
void vita_hle_sceGxmTextureSetStride(void) { vita_write32(r0 + 12, (vita_read32(r0 + 12) & 0x1FFFFu) | ((r1 / 4u) << 17)); r0 = GXM_OK; }
void vita_hle_sceGxmTextureSetMipmapCount(void) { TEX_SET_W3(15, 3, r1); r0 = GXM_OK; }
void vita_hle_sceGxmTextureSetMinFilter(void)   { TEX_SET_W3(1, 7, r1 ? 1 : 0); r0 = GXM_OK; }
void vita_hle_sceGxmTextureSetMagFilter(void)   { TEX_SET_W3(1, 8, r1 ? 1 : 0); r0 = GXM_OK; }
void vita_hle_sceGxmTextureSetUAddrMode(void)   { TEX_SET_W3(7, 9, r1); r0 = GXM_OK; }
void vita_hle_sceGxmTextureSetVAddrMode(void)   { TEX_SET_W3(7, 12, r1); r0 = GXM_OK; }
void vita_hle_sceGxmTextureSetMipFilter(void)   { r0 = GXM_OK; }
/* SCE_GXM_TEXTURE_GAMMA_*: distinto de 0 = la textura está en sRGB y la GPU la pasa a
 * lineal al muestrear (los shaders del partido trabajan en lineal). */
void vita_hle_sceGxmTextureSetGammaMode(void) {
    if (tr() || env_cached("MLB15_GAMMA_TRAZA")) {
        static int n;
        if (n++ < 20) printf("[gxm] TextureSetGammaMode(0x%08X, 0x%08X) datos 0x%08X fmt 0x%08X\n", r0, r1, vita_read32(r0 + 8), vita_read32(r0));
    }
    TEX_SET_W3(1, 15, r1 ? 1 : 0);
    r0 = GXM_OK;
}
void vita_hle_sceGxmTextureSetLodBias(void)     { r0 = GXM_OK; }
void vita_hle_sceGxmTextureSetLodMin(void)      { r0 = GXM_OK; }
void vita_hle_sceGxmTextureSetPalette(void) {
    uint32_t data = vita_read32(r0 + 8);
    int i;
    for (i = 0; i < g_npal; i++) if (g_pal_data[i] == data) break;
    if (i == g_npal && g_npal < MAX_PAL) g_npal++;
    if (i < MAX_PAL) { g_pal_data[i] = data; g_pal_addr[i] = r1; }
    r0 = GXM_OK;
}
void vita_hle_sceGxmTextureGetData(void)   { r0 = vita_read32(r0 + 8); }
void vita_hle_sceGxmTextureGetFormat(void) { r0 = vita_read32(r0); }
void vita_hle_sceGxmTextureGetMipmapCount(void) { r0 = (vita_read32(r0 + 12) >> 3) & 15; }
void vita_hle_sceGxmTextureGetType(void) {
    static const uint32_t sce[5] = { 0x00000000u, 0x60000000u, 0xC0000000u, 0x80000000u, 0x40000000u };
    r0 = sce[(vita_read32(r0 + 12) & 7) % 5];
}

static void tex_from_words(const uint8_t w[16], GpuTexture *t) {
    uint32_t w0, w1, w2, w3;
    memcpy(&w0, w, 4); memcpy(&w1, w + 4, 4); memcpy(&w2, w + 8, 4); memcpy(&w3, w + 12, 4);
    memset(t, 0, sizeof(*t));
    t->format = w0;
    t->width = (uint16_t)(w1 & 0xFFFF);
    t->height = (uint16_t)(w1 >> 16);
    t->data = w2;
    t->type = (uint8_t)(w3 & 7);
    t->mips = (uint8_t)((w3 >> 3) & 15);
    t->min_linear = (uint8_t)((w3 >> 7) & 1);
    t->mag_linear = (uint8_t)((w3 >> 8) & 1);
    t->u_addr = (uint8_t)((w3 >> 9) & 7);
    t->v_addr = (uint8_t)((w3 >> 12) & 7);
    t->stride = (w3 >> 17) * 4u;
    t->gamma = (uint8_t)((w3 >> 15) & 1);
    for (int i = 0; i < g_npal; i++) if (g_pal_data[i] == w2) { t->palette = g_pal_addr[i]; break; }
}

/* ========================================================================== */
/* Estado precalculado                                                        */
/* La estructura del juego es opaca y PEQUEÑA (SceGxmPrecomputedDraw = 11       */
/* palabras, FragmentState = 9, VertexState = 7): aquí solo guarda [0] = puntero */
/* a nuestros datos y [4] = 'PCDW'. Los datos van en extraData, el búfer que el  */
/* juego reserva con el tamaño de Get*Size (tercer argumento de Init):           */
/*   draw:     [0] vprog [4] prim [8] tipo de índice [12] índices [16] nº       */
/*             [20] indexWrap (0 = sin instancias) [24..88) 16 streams          */
/*   fragment: [0] fprog [4] buffer por defecto [8..72) 16 UB [72..328) 16 tex  */
/* Antes los datos iban en la propia estructura: 96 y 336 bytes escritos en 44 y */
/* 36 → machacaban lo que el juego guarda detrás (las listas del público 3D, que */
/* acababan copiando matrices a punteros basura: trampa tras una jugada).       */
/* ========================================================================== */

#define PC_MAGIC 0x57444350u   /* 'PCDW' */

static uint32_t pc_init(uint32_t st, uint32_t prog, uint32_t extra, uint32_t size) {
    if (!vita_mem_ptr(st, 8)) return 0;
    if (!extra || !vita_mem_ptr(extra, size)) extra = obj_alloc(size);
    if (!extra) return 0;
    memset(vita_mem_ptr(extra, size), 0, size);
    vita_write32(extra, prog);
    vita_write32(st, extra);
    vita_write32(st + 4, PC_MAGIC);
    return extra;
}

/* Datos de un estado precalculado (0 si no lo inicializamos nosotros). */
static uint32_t pc_data(uint32_t st) {
    if (!st || !vita_mem_ptr(st, 8) || vita_read32(st + 4) != PC_MAGIC) return 0;
    return vita_read32(st);
}

void vita_hle_sceGxmGetPrecomputedDrawSize(void) { r0 = 0x60; }
void vita_hle_sceGxmGetPrecomputedVertexStateSize(void) { r0 = 0x60; }
void vita_hle_sceGxmGetPrecomputedFragmentStateSize(void) { r0 = 0x150; }
/* (pd, vertexProgram, extraData) */
void vita_hle_sceGxmPrecomputedDrawInit(void) { pc_init(r0, r1, r2, 0x60); r0 = GXM_OK; }
void vita_hle_sceGxmPrecomputedDrawSetVertexStream(void) {
    uint32_t d = pc_data(r0);
    if (d && r1 < 16) vita_write32(d + 24 + 4 * r1, r2);
    r0 = GXM_OK;
}
void vita_hle_sceGxmPrecomputedDrawSetAllVertexStreams(void) {
    uint32_t d = pc_data(r0);
    for (uint32_t i = 0; d && i < 16; i++) vita_write32(d + 24 + 4 * i, r1 ? vita_read32(r1 + 4 * i) : 0);
    r0 = GXM_OK;
}
/* (pd, prim, indexType, indexData, indexCount) */
void vita_hle_sceGxmPrecomputedDrawSetParams(void) {
    uint32_t d = pc_data(r0);
    if (d) {
        vita_write32(d + 4, r1); vita_write32(d + 8, r2); vita_write32(d + 12, r3);
        vita_write32(d + 16, arg(0)); vita_write32(d + 20, 0);
    }
    r0 = GXM_OK;
}
/* (pd, prim, indexType, indexData, indexCount, indexWrap) */
void vita_hle_sceGxmPrecomputedDrawSetParamsInstanced(void) {
    uint32_t d = pc_data(r0);
    if (d) {
        vita_write32(d + 4, r1); vita_write32(d + 8, r2); vita_write32(d + 12, r3);
        vita_write32(d + 16, arg(0)); vita_write32(d + 20, arg(1));
    }
    r0 = GXM_OK;
}
/* (state, fragmentProgram, extraData) */
void vita_hle_sceGxmPrecomputedFragmentStateInit(void) { pc_init(r0, r1, r2, 0x150); r0 = GXM_OK; }
void vita_hle_sceGxmPrecomputedFragmentStateSetTexture(void) {
    uint32_t d = pc_data(r0);
    if (d && r1 < 16 && vita_mem_ptr(r2, 16)) memcpy(vita_mem_ptr(d + 72 + 16 * r1, 16), vita_mem_ptr(r2, 16), 16);
    r0 = GXM_OK;
}
void vita_hle_sceGxmPrecomputedFragmentStateSetAllTextures(void) {
    uint32_t d = pc_data(r0);
    for (uint32_t i = 0; d && i < 16 && r1; i++)
        if (vita_mem_ptr(r1 + 16 * i, 16)) memcpy(vita_mem_ptr(d + 72 + 16 * i, 16), vita_mem_ptr(r1 + 16 * i, 16), 16);
    r0 = GXM_OK;
}
void vita_hle_sceGxmPrecomputedFragmentStateSetUniformBuffer(void) {
    uint32_t d = pc_data(r0);
    if (d && r1 < 16) vita_write32(d + 8 + 4 * r1, r2);
    r0 = GXM_OK;
}
void vita_hle_sceGxmPrecomputedFragmentStateSetAllUniformBuffers(void) {
    uint32_t d = pc_data(r0);
    for (uint32_t i = 0; d && i < 16 && r1; i++) vita_write32(d + 8 + 4 * i, vita_read32(r1 + 4 * i));
    r0 = GXM_OK;
}
void vita_hle_sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer(void) {
    uint32_t d = pc_data(r0);
    if (d) vita_write32(d + 4, r1);
    r0 = GXM_OK;
}

/* ========================================================================== */
/* Dibujo                                                                     */
/* ========================================================================== */

static float half_to_float_calc(uint16_t h);
/* Medio float -> float con una tabla de 65 536 entradas (256 KB): los atributos F16 de los
 * vértices y los uniformes se convierten miles de veces por fotograma. */
static float g_h2f[65536];
static volatile LONG g_h2f_lista;
static void h2f_init(void) {
    if (g_h2f_lista) return;
    for (uint32_t i = 0; i < 65536; i++) g_h2f[i] = half_to_float_calc((uint16_t)i);
    InterlockedExchange(&g_h2f_lista, 1);
}
static float half_to_float(uint16_t h) {
    if (!g_h2f_lista) h2f_init();
    return g_h2f[h];
}
static float half_to_float_calc(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000u) << 16, e = (h >> 10) & 31, m = h & 0x3FF, x;
    if (!e) {
        if (!m) x = s;
        else { e = 127 - 15 + 1; while (!(m & 0x400)) { m <<= 1; e--; } m &= 0x3FF; x = s | (e << 23) | (m << 13); }
    } else if (e == 31) x = s | 0x7F800000u | (m << 13);
    else x = s | ((e - 15 + 127) << 23) | (m << 13);
    float f;
    memcpy(&f, &x, 4);
    return f;
}

/* Normales y positivos pequeños (sin infinitos ni subnormales: basta para uniformes). */
static uint16_t float_to_half(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t s = (x >> 16) & 0x8000u;
    int e = (int)((x >> 23) & 0xFF) - 127 + 15;
    if (e <= 0) return (uint16_t)s;
    if (e >= 31) return (uint16_t)(s | 0x7BFFu);
    return (uint16_t)(s | ((uint32_t)e << 10) | ((x >> 13) & 0x3FF));
}

/* Componente i de un atributo, leído del puntero host (el guest es little
 * endian como el PC). Antes cada componente era una llamada a vita_read*: en el
 * partido (miles de draws instanciados por fotograma) era el 85 % del tiempo
 * del hilo de dibujo y el juego iba a 1 fps. */
static float read_comp(const uint8_t *a, uint8_t fmt, int i) {
    uint16_t h;
    switch (fmt) {
        case 0:  return (float)a[i];
        case 1:  return (float)(int8_t)a[i];
        case 2:  memcpy(&h, a + 2 * i, 2); return (float)h;
        case 3:  memcpy(&h, a + 2 * i, 2); return (float)(int16_t)h;
        case 4:  return a[i] / 255.0f;
        case 5:  { float v = (int8_t)a[i] / 127.0f; return v < -1.0f ? -1.0f : v; }
        case 6:  memcpy(&h, a + 2 * i, 2); return h / 65535.0f;
        case 7:  { memcpy(&h, a + 2 * i, 2); float v = (int16_t)h / 32767.0f; return v < -1.0f ? -1.0f : v; }
        case 8:  memcpy(&h, a + 2 * i, 2); return half_to_float(h);
        default: { float f; memcpy(&f, a + 4 * i, 4); return f; }
    }
}

/* Bytes de un componente según el formato de atributo (para validar el rango). */
static uint32_t comp_size(uint8_t fmt) { return fmt <= 1 || fmt == 4 || fmt == 5 ? 1u : fmt <= 8 ? 2u : 4u; }

/* Matriz 4x4 de un parámetro uniforme: del buffer de su contenedor. */
static int read_matrix(Ctx *c, uint32_t p, float m[16]) {
    if (!p) return 0;
    uint32_t cont = p_cont(p);
    uint32_t buf = cont == 14 ? c->vdef : c->vub[cont];
    if (!buf || !vita_mem_ptr(buf + p_res(p) * 4u, 64)) return 0;
    for (int i = 0; i < 16; i++) { vfp_reg v; v.u = vita_read32(buf + p_res(p) * 4u + 4u * (uint32_t)i); m[i] = v.f; }
    return 1;
}

/* Convención comprobada con el menú: los 16 floats que deja el juego son
 * directamente la matriz en el orden de columnas de GLSL (el shader Cg hace
 * mul(v, M) con M por filas, que es lo mismo). Trasponerla dibujaba los
 * diálogos como una línea. MLB15_MVP_RAW=0 vuelve a trasponer. */
static void to_glsl(const float m[16], float out[16]) {
    static int raw = -1;
    if (raw < 0) { const char *e = env_cached("MLB15_MVP_RAW"); raw = e ? (*e != '0') : 1; }
    if (raw) { memcpy(out, m, 64); return; }
    for (int r = 0; r < 4; r++) for (int col = 0; col < 4; col++) out[col * 4 + r] = m[r * 4 + col];
}

static void mat_mul(const float a[16], const float b[16], float o[16]) {   /* filas: o = a * b */
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + col];
            o[r * 4 + col] = s;
        }
}

/* Vector uniforme (hasta 4 componentes, F32 o F16) del buffer de su contenedor. */
static int read_vec(Ctx *c, uint32_t p, int frag, float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (!p) return 0;
    uint32_t cont = p_cont(p), buf;
    if (frag) buf = cont == 14 ? (c->pfs ? vita_read32(c->pfs + 4) : c->fdef)
                               : (c->pfs ? vita_read32(c->pfs + 8 + 4 * cont) : c->fub[cont]);
    else buf = cont == 14 ? c->vdef : c->vub[cont];
    if (!buf) return 0;
    uint32_t at = buf + p_res(p) * 4u;
    if (p_type(p) == 1) {
        const uint8_t *h = (const uint8_t *)vita_mem_ptr(at, 8);
        if (!h) return 0;
        for (int i = 0; i < 4; i++) { uint16_t x; memcpy(&x, h + 2 * i, 2); out[i] = half_to_float(x); }
    } else {
        const uint8_t *h = (const uint8_t *)vita_mem_ptr(at, 16);
        if (!h) return 0;
        memcpy(out, h, 16);
    }
    for (int i = 0; i < 4; i++) if (out[i] != out[i]) out[i] = 0.0f;   /* NaN */
    return 1;
}

#define MAX_DRAW_VERTS 65536
#define MAX_DRAW_IDX   (3 * 65536)
static GpuVertex g_verts[MAX_DRAW_VERTS];
static uint32_t g_idx[MAX_DRAW_IDX], g_tri[MAX_DRAW_IDX];
#define MAX_INDEX_VALUE (1u << 20)
static uint32_t g_stamp[MAX_INDEX_VALUE], g_slot[MAX_INDEX_VALUE], g_stamp_id;
static uint32_t g_vsrc[MAX_DRAW_VERTS];

/* --- skinning ------------------------------------------------------------- */
/* Los shaders de jugadores (Index u8x3, Weight unorm8x3, UVP_SkinningMatrix: 236
 * vec4 en el contenedor 3) hacen  mundo = sum(w_k * M[idx_k] * pos)  y luego
 * UVP_ViewProjectionMatrix. Cada M es 4x4 por filas con la traslación en la
 * 4.ª fila (convención de vector fila, como la VP: mundo = x*f0 + y*f1 + z*f2 + f3),
 * así que el hueso k empieza en el vec4 4*k (59 huesos). Comprobado con los
 * valores del juego: la fila 3 es (x, y, z, 1). Se hace en la CPU: el shader
 * genérico solo sabe multiplicar por una matriz. */
static float g_bidx[MAX_DRAW_VERTS][4], g_bw[MAX_DRAW_VERTS][4], g_nrm[MAX_DRAW_VERTS][3];
/* matrices por instancia del skinning stream (InstanceMatrix3x4), hasta 64 instancias */
static float g_inst[64][16];   /* 3 filas de matriz + traslación (SLOT_INST3 escribe [12..15]) */

static int comps_bidx(const VProg *v) {
    for (int k = 0; k < v->nattr; k++) if (v->attr[k].slot == SLOT_BIDX) return v->attr[k].comps > 4 ? 4 : v->attr[k].comps;
    return 0;
}

static int skin_vertices(Ctx *c, const VProg *v, uint32_t nv, int nb) {
    uint32_t p = v->skin, cont = p_cont(p);
    uint32_t buf = cont == 14 ? c->vdef : c->vub[cont];
    uint32_t nvec = vita_read32(p + 8);                       /* tamaño del array en vec4 */
    const float *mtx = buf >= 0x81000000u ? (const float *)vita_mem_ptr(buf + p_res(p) * 4u, nvec * 16u) : NULL;
    /* Público 3D (InstanceMatrix3x4 + Index/Weight, vgxp 0x83A1BDD0): a veces llega con el UB
     * de huesos a un valor que no es puntero (0x226, 0x7001D...: objeto 0x80/0x100 en
     * sub_813E5690, sin resolver) y se descarta. MLB15_HUESOS_PREVIOS=1 prueba a usar los
     * huesos del último draw válido (experimento; no es lo que hace la Vita). */
    static uint32_t last_buf, last_nvec;
    static int prev_on = -1;
    if (prev_on < 0) { const char *e = env_cached("MLB15_HUESOS_PREVIOS"); prev_on = e ? atoi(e) : 0; }
    if (mtx && nvec >= 4) { last_buf = buf + p_res(p) * 4u; last_nvec = nvec; }
    else if (prev_on && last_buf && nvec >= 4 && nb > 0) {
        mtx = (const float *)vita_mem_ptr(last_buf, (nvec < last_nvec ? nvec : last_nvec) * 16u);
        static int shown;
        if (mtx && env_cached("MLB15_HUESOS_TRAZA") && shown++ < 16) {
            const float *p0 = g_verts[0].pos;
            uint32_t b0 = (uint32_t)g_bidx[0][0];
            const float *r = mtx + 16u * (b0 < nvec / 4 ? b0 : 0);
            printf("[huesos] previos: vgxp=0x%08X ub=0x%08X nv=%u nb=%d p0(%.3f %.3f %.3f) idx(%.0f %.0f %.0f) w(%.2f %.2f %.2f) M[%u]=%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f\n",
                   v->gxp, buf, nv, nb, p0[0], p0[1], p0[2], g_bidx[0][0], g_bidx[0][1], g_bidx[0][2], g_bw[0][0], g_bw[0][1], g_bw[0][2],
                   b0, r[0], r[1], r[2], r[4], r[5], r[6], r[8], r[9], r[10], r[12], r[13], r[14]);
        }
    }
    if (!mtx || nvec < 4 || nb <= 0) {
        if (tr()) {
            static int shown;
            if (shown++ < 6) printf("[skin] sin matrices: vgxp=0x%08X cont=%u buf=0x%08X res=%u nvec=%u nb=%d vub=%08X %08X %08X %08X\n", v->gxp, cont, buf,
                                    p_res(p), nvec, nb, c->vub[0], c->vub[1], c->vub[2], c->vub[3]);
        }
        return 0;
    }
    uint32_t nbones = nvec / 4;
    static uint32_t want_gxp = 1;                 /* MLB15_SKIN_GXP=hex: trazar ese programa */
    if (want_gxp == 1) { const char *e = env_cached("MLB15_SKIN_GXP"); want_gxp = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
    if (tr() || (want_gxp && v->gxp == want_gxp)) {
        static int shown, shown_g;
        if (want_gxp ? (v->gxp == want_gxp && shown_g++ < 8) : shown++ < 4) {
            const float *p0 = g_verts[0].pos;
            printf("[skin] nv=%u huesos=%u buf=0x%08X res=%u | v0 pos(%.3f %.3f %.3f) idx(%.0f %.0f %.0f) w(%.2f %.2f %.2f)\n",
                   nv, nbones, buf, p_res(p), p0[0], p0[1], p0[2], g_bidx[0][0], g_bidx[0][1], g_bidx[0][2],
                   g_bw[0][0], g_bw[0][1], g_bw[0][2]);
            for (int b = 0; b < 3; b++) {
                const float *r = mtx + 16 * b;
                printf("[skin]   M%d: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n", b, r[0], r[1], r[2],
                       r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11]);
            }
            float vp[16];
            if (read_matrix(c, v->vpm, vp))
                printf("[skin]   VP: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n",
                       vp[0], vp[1], vp[2], vp[3], vp[4], vp[5], vp[6], vp[7], vp[8], vp[9], vp[10], vp[11], vp[12], vp[13],
                       vp[14], vp[15]);
            else printf("[skin]   sin VP (vpm=0x%08X)\n", v->vpm);
        }
    }
    for (uint32_t i = 0; i < nv; i++) {
        float *pos = g_verts[i].pos, *n = g_nrm[i], out[3] = { 0, 0, 0 }, on[3] = { 0, 0, 0 }, wsum = 0;
        for (int k = 0; k < nb; k++) {
            float w = g_bw[i][k];
            uint32_t b = (uint32_t)g_bidx[i][k];
            if (w <= 0.0f || b >= nbones) continue;
            const float *r = mtx + 16u * b;
            for (int q = 0; q < 3; q++) {
                out[q] += w * (pos[0] * r[q] + pos[1] * r[4 + q] + pos[2] * r[8 + q] + r[12 + q]);
                on[q] += w * (n[0] * r[q] + n[1] * r[4 + q] + n[2] * r[8 + q]);
            }
            wsum += w;
        }
        if (wsum <= 0.0f) {                                    /* sin pesos: hueso 0 entero */
            const float *r = mtx + 16u * ((uint32_t)g_bidx[i][0] < nbones ? (uint32_t)g_bidx[i][0] : 0);
            for (int q = 0; q < 3; q++) {
                out[q] = pos[0] * r[q] + pos[1] * r[4 + q] + pos[2] * r[8 + q] + r[12 + q];
                on[q] = n[0] * r[q] + n[1] * r[4 + q] + n[2] * r[8 + q];
            }
            wsum = 1.0f;
        }
        pos[0] = out[0] / wsum; pos[1] = out[1] / wsum; pos[2] = out[2] / wsum; pos[3] = 1.0f;
        n[0] = on[0]; n[1] = on[1]; n[2] = on[2];
    }
    return 1;
}

/* --- iluminación ------------------------------------------------------------ */
/* Los shaders reales (Normal + UVP/UFP_LightDirSun + UFP_LightColorAndLightMult +
 * UFP_QuickAmbientColor) hacen sol difuso + ambiente, sombras y especular. El
 * shader genérico no sabe nada de eso, así que se hace aquí por vértice y se
 * multiplica en el color de vértice: luz = ambiente + color_sol * max(0, N·L).
 * MLB15_LUZ=0 lo quita; MLB15_LUZ_TRAZA=1 enseña los valores que manda el juego. */
static void light_vertices(Ctx *c, const VProg *v, const FProg *f, uint32_t nv, const float *model, int *use_color) {
    static int on = -1, trace = -1;
    if (on < 0) { const char *e = env_cached("MLB15_LUZ"); on = e ? atoi(e) : 1; }
    if (trace < 0) trace = env_cached("MLB15_LUZ_TRAZA") ? 1 : 0;
    if (!on) return;
    /* El estadio (solo MVP, sin matriz de modelo ni huesos) ya trae la luz
     * horneada en sus texturas: iluminarlo otra vez lo oscurecía. MLB15_LUZ=2
     * ilumina también eso. */
    if (on == 1 && !v->skin && !model) return;
    float L[4], lc[4], amb[4], am[4];
    int hl = read_vec(c, f->light_dir, 1, L) || read_vec(c, v->light_dir, 0, L);
    int hc = read_vec(c, f->light_col, 1, lc);
    int ha = read_vec(c, f->amb_col, 1, amb);
    int hm = read_vec(c, f->amb_mult, 1, am);
    if (trace) {
        static int shown;
        if (shown++ < 40)
            printf("[luz] vgxp=0x%08X fgxp=0x%08X L%c(%.3f %.3f %.3f %.3f) col%c(%.3f %.3f %.3f %.3f) amb%c(%.3f %.3f %.3f %.3f) "
                   "sm%c(%.3f %.3f %.3f %.3f) n0(%.2f %.2f %.2f) skin=%d model=%d\n", v->gxp, f->gxp, hl ? '+' : '-',
                   L[0], L[1], L[2], L[3], hc ? '+' : '-', lc[0], lc[1], lc[2], lc[3], ha ? '+' : '-', amb[0], amb[1], amb[2],
                   amb[3], hm ? '+' : '-', am[0], am[1], am[2], am[3], g_nrm[0][0], g_nrm[0][1], g_nrm[0][2], v->skin != 0,
                   model != NULL);
    }
    if (!hl) return;
    float ll = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    if (ll < 1e-6f) return;
    L[0] /= ll; L[1] /= ll; L[2] /= ll;
    float sun[3] = { 1.0f, 1.0f, 1.0f }, ambient[3] = { 0.42f, 0.44f, 0.5f };
    /* El color del sol llega en HDR (~(12.3, 11.7, 9.6) x 0.97, luego lo baja la
     * exposición): se normaliza a su componente mayor y se deja en 0.75. El
     * ambiente de UFP_QuickAmbientColor no es un color ((0, 0.5, 1, 0)), así que
     * va fijo, algo azulado como el cielo. */
    (void)ha; (void)hm;
    if (hc) {
        float mx = lc[0] > lc[1] ? lc[0] : lc[1];
        if (lc[2] > mx) mx = lc[2];
        if (mx > 1e-6f) for (int q = 0; q < 3; q++) sun[q] = 0.75f * lc[q] / mx;
    }
    for (uint32_t i = 0; i < nv; i++) {
        float *n = g_nrm[i], w[3];
        if (model) for (int q = 0; q < 3; q++) w[q] = n[0] * model[q] + n[1] * model[4 + q] + n[2] * model[8 + q];
        else { w[0] = n[0]; w[1] = n[1]; w[2] = n[2]; }
        float nl = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        /* L apunta hacia el sol (sale (-0.18, 0.89, 0.42): hacia arriba). */
        float dif = nl > 1e-6f ? (w[0] * L[0] + w[1] * L[1] + w[2] * L[2]) / nl : 1.0f;
        if (dif < 0.0f) dif = 0.0f;
        float *col = g_verts[i].col;
        if (!*use_color) col[0] = col[1] = col[2] = col[3] = 1.0f;
        for (int q = 0; q < 3; q++) {
            float k = ambient[q] + sun[q] * dif;
            col[q] *= k > 1.25f ? 1.25f : k;
        }
    }
    *use_color = 1;
}

/* --- sombras de jugadores --------------------------------------------------- */
/* El juego pinta a los jugadores en PlayerShadowsTarget y el shader del césped
 * lo muestrea con coordenadas que calcula el propio GXP. Aquí se hace la sombra
 * plana clásica: cada draw con huesos (ya en mundo) se proyecta sobre el suelo
 * (y = 0) en la dirección del sol y se dibuja en negro translúcido. La prueba
 * de profundidad LESS con escritura evita que los triángulos que se solapan
 * oscurezcan dos veces (todos caen en el mismo plano).
 * MLB15_SOMBRAS=0 las quita; MLB15_SOMBRAS_ALFA=0.45 cambia la intensidad. */
static GpuVertex g_shadow[MAX_DRAW_VERTS];
static float g_mound[4];
static int g_mound_ok;

/* Altura del suelo (en pies, la unidad del juego): plano y = 0 salvo el
 * montículo, cuya cima da UVP_MoundPosition ((0, 0.833, 2.64): 10 pulgadas).
 * Reglamento: meseta alrededor de la goma y bajada hasta 9 pies del centro. */
static float ground_height(float x, float z, float ground) {
    if (!g_mound_ok || g_mound[1] <= 0.0f) return ground;
    float dx = x - g_mound[0], dz = z - g_mound[2], r = sqrtf(dx * dx + dz * dz);
    if (r <= 3.0f) return g_mound[1];
    if (r >= 9.0f) return ground;
    return ground + (g_mound[1] - ground) * (9.0f - r) / 6.0f;
}

static void player_shadow(Ctx *c, const VProg *v, const FProg *f, const GpuDraw *src, uint32_t nv, uint32_t nt) {
    static int on = -1;
    static float alpha = 0.45f, ground = 0.0f;
    if (on < 0) {
        const char *e = env_cached("MLB15_SOMBRAS");
        on = e ? (*e != '0') : 1;
        if ((e = env_cached("MLB15_SOMBRAS_ALFA"))) alpha = (float)atof(e);
        if ((e = env_cached("MLB15_SUELO_Y"))) ground = (float)atof(e);
    }
    if (!on || c->tw < 480 || src->color_off || src->blend) return;   /* solo la escena principal y cuerpos opacos */
    float L[4];
    if (!(read_vec(c, f->light_dir, 1, L) || read_vec(c, v->light_dir, 0, L))) { L[0] = 0.0f; L[1] = 1.0f; L[2] = 0.3f; }
    if (L[1] < 0.2f) return;                                            /* sol muy bajo o datos raros */
    if (env_cached("MLB15_SOMBRAS_TRAZA")) {
        static int shown;
        float y0 = 1e30f, y1 = -1e30f, x0 = 0, z0 = 0;
        for (uint32_t i = 0; i < nv; i++) {
            const float *p = g_verts[i].pos;
            if (p[1] < y0) { y0 = p[1]; x0 = p[0]; z0 = p[2]; }
            if (p[1] > y1) y1 = p[1];
        }
        if (shown++ < 60) printf("[sombra] nv=%u y=[%.3f..%.3f] pie=(%.2f %.2f %.2f) skin=0x%08X\n", nv, y0, y1, x0, y0, z0, c->vub[3]);
    }
    for (uint32_t i = 0; i < nv; i++) {
        const float *p = g_verts[i].pos;
        float gx = p[0], gz = p[2], gy = ground;
        for (int it = 0; it < 3; it++) {                                /* el suelo no es plano en el montículo */
            gy = ground_height(gx, gz, ground);
            float t = (p[1] - gy) / L[1];
            gx = p[0] - L[0] * t; gz = p[2] - L[2] * t;
        }
        GpuVertex *o = &g_shadow[i];
        memset(o, 0, sizeof(*o));
        o->pos[0] = gx;
        o->pos[1] = ground_height(gx, gz, ground) + 0.08f;              /* pies: ~1 cm sobre el suelo */
        o->pos[2] = gz;
        o->pos[3] = 1.0f;
    }
    GpuDraw d = *src;
    d.use_tex = 0; d.use_color = 0;
    d.use_const = 1;
    d.const_color[0] = d.const_color[1] = d.const_color[2] = 0.0f; d.const_color[3] = alpha;
    d.blend = 1;
    d.color_func = d.alpha_func = 1;                                    /* ADD */
    d.color_src = 4; d.color_dst = 5;                                   /* SRC_ALPHA, ONE_MINUS_SRC_ALPHA */
    d.alpha_src = 0; d.alpha_dst = 1;                                   /* conserva el alfa de destino */
    d.color_mask = 0xF;
    d.cull = 0;
    d.depth_func = 1;                                                   /* LESS */
    d.depth_write = 1;
    d.stencil = 0;
    gpu_draw(&d, g_shadow, nv, g_tri, nt);
}

/* Índices g_idx (icount por instancia) a lista de triángulos en g_tri (tiras y
 * abanicos en la CPU); las demás instancias repiten los triángulos sobre su
 * copia de vértices. Devuelve el número de índices. */
static uint32_t build_tris(uint32_t ptype, uint32_t icount, uint32_t nvb, uint32_t ninst) {
    uint32_t nt = 0;
    if (ptype == 0) { nt = icount / 3 * 3; memcpy(g_tri, g_idx, nt * 4); }
    else if (ptype == 3) {
        for (uint32_t i = 0; i + 2 < icount; i++) {
            uint32_t a0 = g_idx[i], a1 = g_idx[i + 1], a2 = g_idx[i + 2];
            if (i & 1) { uint32_t t = a0; a0 = a1; a1 = t; }
            g_tri[nt++] = a0; g_tri[nt++] = a1; g_tri[nt++] = a2;
        }
    } else {
        for (uint32_t i = 1; i + 1 < icount; i++) { g_tri[nt++] = g_idx[0]; g_tri[nt++] = g_idx[i]; g_tri[nt++] = g_idx[i + 1]; }
    }
    for (uint32_t j = 1, n1 = nt; j < ninst; j++)
        for (uint32_t i = 0; i < n1; i++) g_tri[nt++] = g_tri[i] + j * nvb;
    return nt;
}

/* Estado fijo del draw (viewport, mezcla, profundidad, stencil): igual para el
 * shader genérico y para los traducidos. */
/* Mezcla programable traducida a la de la GPU en el draw en curso: los UFP_SRC/DST del
 * shader (o NULL). En la Vita el shader lee el píxel de destino y mezcla él mismo; aquí ese
 * píxel le llega como 0, así que la mezcla la hace Vulkan y al shader se le pasan los
 * coeficientes de "solo origen" (Ck = 1, resto 0): devuelve el color de origen tal cual.
 * Antes el shader mezclaba con un destino 0 y luego Vulkan volvía a mezclar con el alfa ya
 * calculado; donde la fórmula del alfa dependía del destino daba 0 y no se pintaba nada
 * (la cara del jugador creado). MLB15_SPRITE_SHADER=1 vuelve a lo anterior. */
static const uint32_t *g_sprite_ufp;
static void draw_state(Ctx *c, const FProg *f, GpuDraw *dp) {
    g_sprite_ufp = NULL;
    memcpy(dp->vp, c->vp, 16);
    dp->dbias_factor = (float)c->dbias_factor[0]; dp->dbias_units = (float)c->dbias_units[0];
    {   /* MLB15_SESGO=factor,unidades (pruebas): multiplica el sesgo de profundidad del juego */
        static float mf = -1.0f, mu = 1.0f;
        if (mf < 0.0f) { const char *e = env_cached("MLB15_SESGO"); mf = 1.0f; if (e) sscanf(e, "%f,%f", &mf, &mu); }
        dp->dbias_factor *= mf; dp->dbias_units *= mu;
    }
    dp->vz[0] = c->vp[4]; dp->vz[1] = c->vp[5];
    dp->vp_enable = c->vp_enable;
    if (dp->vp[1] == 0.0f || dp->vp[3] == 0.0f) {
        /* Viewport degenerado (el juego lo calcula de campos que aún llegan a 0,
         * ver LEEME_RECOMP): se usa la superficie entera. */
        dp->vp[0] = c->tw * 0.5f; dp->vp[1] = c->tw * 0.5f;
        dp->vp[2] = c->th * 0.5f; dp->vp[3] = -(c->th * 0.5f);
    }
    dp->blend = f->blend;
    dp->color_func = f->color_func; dp->alpha_func = f->alpha_func;
    dp->color_src = f->color_src; dp->color_dst = f->color_dst;
    dp->alpha_src = f->alpha_src; dp->alpha_dst = f->alpha_dst;
    dp->color_mask = f->mask;
    dp->cull = c->cull;
    dp->color_off = c->frag_off;
    {
        int back = c->two_sided ? 1 : 0;
        dp->depth_func = c->depth_func[0];
        dp->depth_write = c->depth_write[0];
        for (int k = 0; k < 2; k++) {
            int s = k ? back : 0;
            dp->st[k].func = c->st[s].func; dp->st[k].fail = c->st[s].fail; dp->st[k].zfail = c->st[s].zfail;
            dp->st[k].pass = c->st[s].pass; dp->st[k].cmask = c->st[s].cmask; dp->st[k].wmask = c->st[s].wmask;
            dp->st[k].ref = c->st[s].ref;
        }
        dp->stencil = 0;
        for (int k = 0; k < 2; k++)
            if (dp->st[k].func != 7 || dp->st[k].fail || dp->st[k].zfail || dp->st[k].pass) dp->stencil = 1;
    }
    /* Mezcla programable de los shaders Sprite: el propio shader calcula
     *   out = src*(Ck + CSa*As + CDa*Ad + CDc*Cd) + dst*(Ck + CSa*As + CSc*Cs)
     * con coeficientes en un buffer de uniformes. Los casos que usa el juego son
     * mezclas fijas de toda la vida, así que se traducen a factores de Vulkan. */
    if (f->ufp[0]) {
        /* Coeficientes vec4: x para el color (R=G=B en los casos del juego) y w para el alfa.
         * Antes se usaba x también para el alfa: la pasada que escribe en el alfa la máscara de
         * cada región de la cara del jugador creado (color ZERO/ONE, alfa con CDa.w = 1) no
         * escribía nada y la cara compuesta salía vacía (cabeza blanca). */
        float k[7][2];
        for (int i = 0; i < 7; i++) {
            k[i][0] = k[i][1] = 0.0f;
            if (!f->ufp[i]) continue;
            uint32_t cont = p_cont(f->ufp[i]);
            uint32_t buf = cont == 14 ? (c->pfs ? vita_read32(c->pfs + 4) : c->fdef)
                                      : (c->pfs ? vita_read32(c->pfs + 8 + 4 * cont) : c->fub[cont]);
            if (buf) {
                k[i][0] = half_to_float(vita_read16(buf + p_res(f->ufp[i]) * 4u));
                k[i][1] = p_comps(f->ufp[i]) >= 4 ? half_to_float(vita_read16(buf + p_res(f->ufp[i]) * 4u + 6u)) : k[i][0];
            }
        }
        uint8_t fs[2], fd[2];
        for (int ch = 0; ch < 2; ch++) {
            /* factor de origen: Ck, CSa, CDa, CDc ; de destino: Ck, CSa, CSc */
            float c0 = k[0][ch], c1 = k[1][ch], c2 = k[2][ch], c3 = k[3][ch], d0 = k[4][ch], d1 = k[5][ch], d2 = k[6][ch];
            uint8_t src = 255, dst = 255;
            if      (c0 == 1 && !c1 && !c2 && !c3) src = 1;        /* ONE */
            else if (!c0 && !c1 && !c2 && !c3)     src = 0;        /* ZERO */
            else if (!c0 && c1 == 1 && !c2 && !c3) src = 4;        /* SRC_ALPHA */
            else if (!c0 && !c1 && c2 == 1 && !c3) src = 8;        /* DST_ALPHA */
            else if (!c0 && !c1 && !c2 && c3 == 1) src = 6;        /* DST_COLOR */
            else if (c0 == 1 && !c1 && c2 == -1 && !c3) src = 9;   /* ONE_MINUS_DST_ALPHA */
            if      (d0 == 1 && !d1 && !d2)        dst = 1;        /* ONE */
            else if (!d0 && !d1 && !d2)            dst = 0;        /* ZERO */
            else if (d0 == 1 && d1 == -1 && !d2)   dst = 5;        /* ONE_MINUS_SRC_ALPHA */
            else if (!d0 && d1 == 1 && !d2)        dst = 4;        /* SRC_ALPHA */
            else if (!d0 && !d1 && d2 == 1)        dst = 2;        /* SRC_COLOR */
            else if (d0 == 1 && !d1 && d2 == -1)   dst = 3;        /* ONE_MINUS_SRC_COLOR */
            fs[ch] = src; fd[ch] = dst;
        }
        if (fs[0] != 255 && fd[0] != 255 && fs[1] != 255 && fd[1] != 255) {
            g_sprite_ufp = f->ufp;   /* v3k_fill_data: el shader solo da el origen */
            dp->blend = 1;
            dp->color_func = dp->alpha_func = 1;                       /* ADD */
            dp->color_src = fs[0]; dp->color_dst = fd[0];
            dp->alpha_src = fs[1]; dp->alpha_dst = fd[1];
        } else {
            static int told;
            if (told++ < 8) printf("[gxm] mezcla programable sin traducir: src(%.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f) dst(%.2f %.2f %.2f / %.2f %.2f %.2f)\n",
                                   k[0][0], k[1][0], k[2][0], k[3][0], k[0][1], k[1][1], k[2][1], k[3][1], k[4][0], k[5][0], k[6][0],
                                   k[4][1], k[5][1], k[6][1]);
        }
    }
    /* Zona caliente/fría del bateador (cuadrícula roja/azul de la zona de strike): fragmento de
     * DynamicLitSkin (277 B, textura x color de vértice) con la textura blanca de 8x8. El juego
     * lo mezcla con (SRC_ALPHA, 1-SRC_ALPHA), pero el color ya llega multiplicado por el alfa
     * del vértice (0,5) y aquí se mezcla en lineal: quedaba muy tenue. Con (ONE, 1-SRC_ALPHA)
     * se ve el rojo/azul semitransparente aprobado. Solo con esa textura: el mismo shader
     * dibuja iconos con transparencia propia. MLB15_ZONA_MEZCLA=0 lo quita. */
    if (f->huella == 0x965ED7C5u && dp->blend && dp->color_src == 4) {
        static int on = -1;
        if (on < 0) { const char *e = env_cached("MLB15_ZONA_MEZCLA"); on = !e || *e != '0'; }
        const uint8_t *tw = c->pfs ? (const uint8_t *)vita_mem_ptr(c->pfs + 72, 16) : c->ftex[0];
        uint32_t wh = 0;
        if (tw) memcpy(&wh, tw + 4, 4);
        if (on && wh == (8u | 8u << 16)) dp->color_src = 1;
    }
    {   /* MLB15_MEZCLA_FP=gxp:src,dst (prueba): fuerza la mezcla de un fragmento */
        static uint32_t want = 1, ms, md;
        if (want == 1) {
            const char *e = env_cached("MLB15_MEZCLA_FP");
            want = 0;
            if (e) { want = (uint32_t)strtoul(e, NULL, 16); if (sscanf(strchr(e, ':') ? strchr(e, ':') + 1 : "4,5", "%u,%u", &ms, &md) != 2) { ms = 4; md = 5; } }
        }
        if (want && f->gxp == want) {
            dp->blend = 1; dp->color_func = dp->alpha_func = 1;
            dp->color_src = dp->alpha_src = (uint8_t)ms; dp->color_dst = dp->alpha_dst = (uint8_t)md;
        }
    }
}

/* --- shaders traducidos (usse.c) --------------------------------------------- */
/* MLB15_SHADERS=0 vuelve al shader genérico para todo. MLB15_SHADER_DUMP=carpeta
 * guarda el GLSL de cada programa. Los programas que el traductor aún no sabe
 * hacer (doble emisión, ...) siguen por el camino genérico. */
static int real_enabled(void) {
    static int on = -1;
    if (on < 0) { const char *e = env_cached("MLB15_SHADERS"); on = e ? (*e != '0') : 0; }   /* desactivados por defecto: los fragmentos salen negros (exposición .z = 0); MLB15_SHADERS=1 los activa */
    return on;
}

static int prog_translate(uint32_t gxp, int *state, int *mod, UsseInfo **out) {
    if (*state) return *state > 0;
    *state = -1;
    if (!real_enabled() || !gxp) return 0;
    {   /* MLB15_SHADERS_SOLO / MLB15_SHADERS_NO: listas de GXP (hex) para aislar fallos */
        char key[16];
        snprintf(key, sizeof(key), "%08X", gxp);
        const char *solo = env_cached("MLB15_SHADERS_SOLO"), *no = env_cached("MLB15_SHADERS_NO");
        if (solo && !strstr(solo, key)) return 0;
        if (no && strstr(no, key)) return 0;
    }
    uint32_t size = vita_read32(gxp + 8);
    const uint8_t *p = size >= 0xA0 && size < (4u << 20) ? (const uint8_t *)vita_mem_ptr(gxp, size) : NULL;
    if (!p) return 0;
    UsseInfo *ui = (UsseInfo *)calloc(1, sizeof(*ui));
    char err[2048];
    if (env_cached("MLB15_FRAG_DEBUG")) usse_debug_input = atoi(env_cached("MLB15_FRAG_DEBUG"));
    char *glsl = ui ? usse_translate(p, size, ui, err, sizeof(err)) : NULL;
    if (!glsl) {
        g_real_bad++;
        if (tr()) printf("[usse] gxp 0x%08X sin traducir: %s\n", gxp, ui ? err : "memoria");
        free(ui);
        return 0;
    }
    const char *dump = env_cached("MLB15_SHADER_DUMP");
    if (dump) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%08X.%s", dump, gxp, ui->is_frag ? "frag" : "vert");
        FILE *fp = fopen(path, "w");
        if (fp) { fputs(glsl, fp); fclose(fp); }
        snprintf(path, sizeof(path), "%s/%08X.gxp", dump, gxp);
        if ((fp = fopen(path, "wb"))) { fwrite(p, 1, size, fp); fclose(fp); }
    }
    uint32_t *spv = NULL;
    size_t n = 0;
    int ok = shc_compile(glsl, ui->is_frag, &spv, &n, err, sizeof(err));
    free(glsl);
    int m = ok ? gpu_module(spv, n) : 0;
    free(spv);
    if (!m) {
        g_real_bad++;
        printf("[usse] gxp 0x%08X no compila: %.300s\n", gxp, ok ? "vkCreateShaderModule" : err);
        free(ui);
        return 0;
    }
    g_real_ok++;
    *mod = m; *out = ui; *state = 1;
    return 1;
}

/* Imagen inicial de sa de una etapa en g_mem[at..]: buffer por defecto, direcciones
 * de los buffers de uniformes (relativas al bloque del draw; sus datos se copian
 * detrás) y literales. */
#define MEM_WORDS (1u << 16)
static uint32_t g_mem[MEM_WORDS];

static int fill_sa(Ctx *c, const UsseInfo *ui, int frag, uint32_t gxp, uint32_t at, uint32_t *top) {
    uint32_t n = ui->sa_words;
    if (at + n > MEM_WORDS) return 0;
    uint32_t *sa = g_mem + at;
    memset(sa, 0, n * 4);
    uint32_t defub = frag ? (c->pfs ? vita_read32(c->pfs + 4) : c->fdef) : c->vdef;
    if (ui->defub_words && defub) {
        const uint8_t *p = (const uint8_t *)vita_mem_ptr(defub, ui->defub_words * 4);
        if (p) memcpy(sa, p, (ui->defub_words < n ? ui->defub_words : n) * 4);
    }
    for (uint32_t k = 0; k < ui->nub; k++) {
        uint32_t b = ui->ub[k].buf, addr;
        if (b >= 14) addr = defub;
        else if (frag) addr = c->pfs ? vita_read32(c->pfs + 8 + 4 * b) : c->fub[b];
        else addr = c->vub[b];
        uint32_t bytes = ui->ub[k].bytes ? ui->ub[k].bytes : 256;
        uint32_t words = (bytes + 3) / 4, pos = (*top + 3) & ~3u;
        if (pos + words > MEM_WORDS) return 0;
        const uint8_t *p = addr ? (const uint8_t *)vita_mem_ptr(addr, bytes) : NULL;
        if (p) memcpy(g_mem + pos, p, words * 4);
        else memset(g_mem + pos, 0, words * 4);
        if (ui->ub[k].sa < n) sa[ui->ub[k].sa] = pos * 4;
        /* UFP_ExposureControls (vec4 f16): el juego deja .z = 0 en el port (la exposición
         * automática sale de leer el objetivo ToneMapSample 32x32, que nuestro backend no
         * devuelve a la memoria guest) y el shader multiplica el color por .z → negro.
         * Mientras no se resuelva: .z = 1.0 si llega a 0. MLB15_EXPOSICION=0 lo quita. */
        static int expo = -1;
        if (expo < 0) { const char *e = env_cached("MLB15_EXPOSICION"); expo = e ? atoi(e) : 1; }
        if (frag && expo && gxp) {
            uint32_t pe = gxp_find(gxp, "UFP_ExposureControls");
            if (pe && p_cont(pe) == b) {
                uint8_t *dst = (uint8_t *)(g_mem + pos) + p_res(pe) * 4u + 4u;
                if (p_res(pe) * 4u + 6u <= words * 4u && dst[0] == 0 && dst[1] == 0) { dst[0] = 0x00; dst[1] = 0x3C; }
            }
        }
        *top = pos + words;
    }
    for (uint32_t k = 0; k < ui->nlit; k++) if (ui->lit[k].sa < n) sa[ui->lit[k].sa] = ui->lit[k].val;
    return 1;
}

static float *g_rverts;   /* nattr vec4 por vértice */
uint32_t g_dbg_ubaddr;

static void draw_real(Ctx *c, VProg *v, FProg *f, const uint32_t streams[16], uint32_t ptype, uint32_t icount,
                      uint32_t nvb, uint32_t ninst, uint32_t maxsrc) {
    const UsseInfo *vi = v->ui, *fi = f->ui;
    uint32_t na = vi->nattr ? vi->nattr : 1, nv = nvb * ninst;
    if (!g_rverts) g_rverts = (float *)malloc((size_t)MAX_DRAW_VERTS * 16 * 4 * sizeof(float));
    if (!g_rverts) return;
    for (uint32_t i = 0; i < nv * na; i++) {
        float *o = g_rverts + 4 * i;
        o[0] = o[1] = o[2] = 0.0f; o[3] = 1.0f;
    }
    for (uint32_t k = 0; k < vi->nattr; k++) {
        const Attr *a = NULL;
        for (int q = 0; q < v->nattr; q++) if (v->attr[q].reg == vi->attr[k].reg) { a = &v->attr[q]; break; }
        if (!a || a->stream >= 16 || !streams[a->stream]) continue;
        uint32_t stride = v->stride[a->stream];
        int per_inst = (v->index_source[a->stream] & 2) != 0;
        int comps = a->comps > 4 ? 4 : a->comps;
        uint32_t nsrc = per_inst ? ninst : maxsrc + 1;
        const uint8_t *base = (const uint8_t *)vita_mem_ptr(streams[a->stream] + a->offset,
                                                            (nsrc - 1) * stride + comp_size(a->format) * (uint32_t)comps);
        if (!base) continue;
        for (uint32_t i = 0; i < nv; i++) {
            uint32_t src = per_inst ? i / nvb : g_vsrc[i % nvb];
            const uint8_t *at = base + src * stride;
            float *o = g_rverts + 4 * ((size_t)i * na + k);
            for (int ci = 0; ci < comps; ci++) o[ci] = read_comp(at, a->format, ci);
        }
    }
    uint32_t nt = build_tris(ptype, icount, nvb, ninst);
    /* memoria: sa del vértice, sa del fragmento, buffers */
    uint32_t vsa = 0, fsa = vi->sa_words, top = vi->sa_words + fi->sa_words;
    if (!fill_sa(c, vi, 0, v->gxp, vsa, &top) || !fill_sa(c, fi, 1, f->gxp, fsa, &top)) { g_skipped++; return; }
    /* texturas del fragmento */
    static GpuTexture tex[16];
    const GpuTexture *tp[16];
    for (int u = 0; u < 16; u++) {
        tp[u] = NULL;
        if (!(fi->tex_units >> u & 1)) continue;
        {   /* MLB15_SOMBRAS_TEX=0: las texturas de sombras van en blanco (sin sombra) */
            static int sh = -1;
            if (sh < 0) { const char *e = env_cached("MLB15_SOMBRAS_TEX"); sh = e ? (*e != '0') : 1; }
            if (!sh && (fi->tex_shadow >> u & 1)) continue;
        }
        const uint8_t *tw = c->pfs ? (const uint8_t *)vita_mem_ptr(c->pfs + 72 + 16u * (uint32_t)u, 16) : c->ftex[u];
        if (!tw) continue;
        memset(&tex[u], 0, sizeof(tex[u]));
        tex_from_words(tw, &tex[u]);
        if (tex[u].data && tex[u].width && tex[u].height) tp[u] = &tex[u];
    }
    GpuDraw d;
    memset(&d, 0, sizeof(d));
    draw_state(c, f, &d);
    if (env_cached("MLB15_SA_TRAZA")) {   /* imagen de sa del fragmento y el buffer al que apunta sa0 */
        static int shown;
        extern int port_en_partido;
        if ((fi->nub > 0 || c->pfs) && port_en_partido) {
            if (shown++ < 40) {
            const uint32_t *sa = g_mem + fsa;
            printf("[sa] fgxp=0x%08X fsa=%u top=%u nub=%u ub0(buf %u sa %u bytes %u) pfs=0x%08X fub7=0x%08X | sa0-7:", f->gxp, fsa, top,
                   fi->nub, fi->ub[0].buf, fi->ub[0].sa, fi->ub[0].bytes, c->pfs, c->fub[7]);
            for (int k = 0; k < 8; k++) printf(" %08X", sa[k]);
            uint32_t w = sa[fi->ub[0].sa] / 4;
            printf(" | mem[sa0/4..]:");
            for (int k = 0; k < 8 && w + k < top; k++) printf(" %08X", g_mem[w + k]);
            printf("\n");
            {   /* ¿el juego rellena el buffer después del draw? se compara en EndScene */
                extern uint32_t g_dbg_ubaddr;
                uint32_t a = c->pfs ? vita_read32(c->pfs + 8 + 4 * fi->ub[0].buf) : c->fub[fi->ub[0].buf];
                g_dbg_ubaddr = a;
                printf("[sa] en el draw: ub+104 = %08X %08X\n", vita_read32(a + 104), vita_read32(a + 108));
            }
            if (shown == 1 || (shown > 1 && fi->nub == 2 && !env_cached("MEM_DUMPEADO2"))) {   /* memoria entera del primer draw, para probar el GLSL fuera del juego */
                char path[64];
                snprintf(path, sizeof(path), "mem_%08X_%u.bin", f->gxp, fsa);
                FILE *fp = fopen(path, "wb");
                if (fp) { fwrite(g_mem, 4, top, fp); fclose(fp); }
                /* MLB15_UB7_DUMP=n: volcar n bytes del UB 7 del fragmento (plantar donde
                 * viven los parámetros del fragmento en el guest, para ver TODA la zona,
                 * no solo los primeros "bytes" declarados). Uno por GXP. */
                {
                    static int ub7dumped;
                    const char *ud = env_cached("MLB15_UB7_DUMP");
                    uint32_t na = ud ? (uint32_t)atol(ud) : 1024;
                    if (!ub7dumped && c->fub[7] && vita_mem_ptr(c->fub[7], na)) {
                        char p2[64];
                        ub7dumped = 1;
                        snprintf(p2, sizeof(p2), "ub7_%08X.bin", f->gxp);
                        FILE *f2 = fopen(p2, "wb");
                        if (f2) { fwrite(vita_mem_ptr(c->fub[7], na), 1, na, f2); fclose(f2); }
                        printf("[sa] ub7 volcado: ptr=0x%08X n=%u\n", c->fub[7], na);
                    }
                }
                if (fi->nub == 2) { _putenv("MEM_DUMPEADO2=1"); }
            }
            }
        }
    }
    if (scene_dump()) {
        printf("[escena real] vgxp=0x%08X fgxp=0x%08X tri=%u v=%u attrs=%u tex=0x%X blend=%u(%u,%u) z=%u/%u mem=%u\n", v->gxp, f->gxp,
               nt / 3, nv, vi->nattr, fi->tex_units, d.blend, d.color_src, d.color_dst, d.depth_func, d.depth_write, top);
        for (int u = 0; u < 16; u++)
            if (tp[u]) printf("    T%d: 0x%08X %ux%u fmt=0x%08X\n", u, tp[u]->data, tp[u]->width, tp[u]->height, tp[u]->format);
    }
    int fmod = f->mod;
    {   /* MLB15_SHADERS=3: fragmento fijo (magenta translúcido) para ver solo los vértices traducidos */
        static int dbg = -1, dbg_mod;
        if (dbg < 0) {
            const char *e = env_cached("MLB15_SHADERS");
            dbg = e && *e == '3';
            if (dbg) {
                static const char *src =
                    "#version 450\nlayout(location = 0) in vec4 v_tc[10];\nlayout(location = 10) in vec4 v_col0;\n"
                    "layout(location = 0) out vec4 o;\n"
                    "void main() { o = vec4(fract(v_tc[0].xy), fract(gl_FragCoord.z * 64.0), 1.0); }\n";
                uint32_t *spv = NULL; size_t n = 0; char err[512];
                if (shc_compile(src, 1, &spv, &n, err, sizeof(err))) dbg_mod = gpu_module(spv, n);
                free(spv);
            }
        }
        if (dbg && dbg_mod) fmod = dbg_mod;
    }
    gpu_draw_prog(&d, v->mod, fmod, na, g_rverts, nv, g_tri, nt, g_mem, top, vsa, fsa, tp);
}


/* --- shaders de Vita3K (v3k_api.h) ------------------------------------------- */
/* Traductor USSE → SPIR-V de Vita3K, compilado dentro del port: el SPIR-V corre
 * en la GPU como cualquier shader nativo. Activado por defecto (MLB15_V3K=0 =
 * camino genérico). Cada programa de fragmento guarda hasta 4 variantes según los
 * formatos de textura (el traductor necesita sus componentes). */
static int v3k_enabled(void) {
    static int on = -1;
    if (on < 0) { const char *e = env_cached("MLB15_V3K"); on = e ? (*e != '0') : 1; }   /* activados (2026-10-30) */
    return on;
}

static int v3k_build(uint32_t gxp, const V3kHints *h, int *mod, V3kInfo **out) {
    uint32_t size = vita_read32(gxp + 8);
    const uint8_t *p = size >= 0xA0 && size < (4u << 20) ? (const uint8_t *)vita_mem_ptr(gxp, size) : NULL;
    if (!p) return 0;
    {   /* MLB15_GXP_DUMP=carpeta: guarda cada GXP como <dirección>.gxp (ver gxpnames.py) */
        const char *dd = env_cached("MLB15_GXP_DUMP");
        if (dd) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%08X.gxp", dd, gxp);
            FILE *fh = fopen(path, "wb");
            if (fh) { fwrite(p, 1, size, fh); fclose(fh); }
        }
    }
    V3kInfo *info = (V3kInfo *)calloc(1, sizeof(*info));
    uint32_t *spv = NULL;
    size_t n = 0;
    char err[512];
    if (!info || !v3k_translate(p, size, h, &spv, &n, info, err, sizeof(err))) {
        g_real_bad++;
        printf("[v3k] gxp 0x%08X sin traducir: %s\n", gxp, info ? err : "memoria");
        free(info);
        return 0;
    }
    const char *dump = env_cached("MLB15_SHADER_DUMP");
    if (dump) {
        char path[512];
        FILE *fp;
        snprintf(path, sizeof(path), "%s/%08X.spv", dump, gxp);
        if ((fp = fopen(path, "wb"))) { fwrite(spv, 4, n, fp); fclose(fp); }
        snprintf(path, sizeof(path), "%s/%08X.gxp", dump, gxp);
        if ((fp = fopen(path, "wb"))) { fwrite(p, 1, size, fp); fclose(fp); }
    }
    int m = gpu_module(spv, n);
    v3k_free(spv);
    if (!m) {
        g_real_bad++;
        printf("[v3k] gxp 0x%08X: vkCreateShaderModule falló\n", gxp);
        free(info);
        return 0;
    }
    g_real_ok++;
    *mod = m;
    *out = info;
    return 1;
}

static int v3k_vert(VProg *v) {
    if (v->v3k) return v->v3k > 0;
    v->v3k = -1;
    V3kHints h;
    memset(&h, 0, sizeof(h));
    h.nattr = (uint32_t)v->nattr;
    for (int i = 0; i < v->nattr; i++) {
        h.attr[i].reg = v->attr[i].reg;
        h.attr[i].format = v->attr[i].format;
        h.attr[i].comps = v->attr[i].comps;
    }
    if (!v3k_build(v->gxp, &h, &v->v3k_mod, &v->v3ki)) return 0;
    v->v3k = 1;
    return 1;
}

/* Palabras de la textura de la unidad u del draw (estado precalculado o suelto). */
static const uint8_t *frag_tex_words(Ctx *c, int u) {
    return c->pfs ? (const uint8_t *)vita_mem_ptr(c->pfs + 72 + 16u * (uint32_t)u, 16) : c->ftex[u];
}

/* Índice de la variante para las texturas actuales, -1 si no se puede traducir. */
static int v3k_frag(Ctx *c, FProg *f, int raw16) {
    if (f->v3k_bad) return -1;
    V3kHints h;
    memset(&h, 0, sizeof(h));
    h.raw16 = raw16 ? 1u : 0u;
    uint32_t key = 2166136261u ^ (raw16 ? 0x9E3779B9u : 0u);
    for (int u = 0; u < 16; u++) {
        const uint8_t *tw = frag_tex_words(c, u);
        if (!tw) continue;
        GpuTexture t;
        memset(&t, 0, sizeof(t));
        tex_from_words(tw, &t);
        h.tex_format[u] = t.format;
        if (f->v3k_n && !(f->v3kv[0].info->tex_mask >> u & 1)) continue;
        key = (key ^ t.format) * 16777619u;
    }
    /* Samplers que el GXP declara de 1 componente (p. ej. ShadowMapTexture f32x1, una U8
     * con swizzle 1RRR): para el traductor van con swizzle R. Si no, Vita3K cuenta 4
     * componentes, su gather4.uv llena 16 registros y deja los pesos bilineales fuera del
     * sitio donde el shader los lee (r4): sombra total → jugadores negros. */
    uint32_t raw_fmt[16];
    memcpy(raw_fmt, h.tex_format, sizeof(raw_fmt));
    if (f->v3k_n) {   /* variante ya construida: no hace falta recorrer los parámetros */
        for (int i = 0; i < f->v3k_n; i++) if (f->v3kv[i].key == key) return i;
    }
    {
        uint32_t n = gxp_nparams(f->gxp), base = gxp_params(f->gxp);
        for (uint32_t i = 0; i < n && i < 256; i++) {
            uint32_t p = base + 16 * i, res = p_res(p), fb = h.tex_format[res & 15] >> 24;
            if (p_cat(p) != 2 || p_comps(p) != 1 || res >= 16 || !h.tex_format[res]) continue;
            if (fb == 0x00 || fb == 0x01 || fb == 0x02 || fb == 0x03 || fb == 0x10 || fb == 0x12 || fb == 0x13)
                h.tex_format[res] &= ~0xF000u;
        }
    }
    if (f->v3k_n) {
        /* la clave solo cuenta las unidades que muestrea el programa */
        for (int i = 0; i < f->v3k_n; i++) if (f->v3kv[i].key == key) return i;
        if (f->v3k_n >= 4) return 0;
    }
    int mod;
    V3kInfo *info;
    if (!v3k_build(f->gxp, &h, &mod, &info)) { f->v3k_bad = 1; return -1; }
    if (!f->v3k_n) {   /* recalcular la clave con la máscara ya conocida */
        key = 2166136261u ^ (raw16 ? 0x9E3779B9u : 0u);
        for (int u = 0; u < 16; u++) if (info->tex_mask >> u & 1) key = (key ^ raw_fmt[u]) * 16777619u;
    }
    int i = f->v3k_n++;
    f->v3kv[i].key = key; f->v3kv[i].mod = mod; f->v3kv[i].info = info;
    if (i == 0 && env_cached("MLB15_UB_TRAZA")) {   /* uniformes del GXP que caen fuera del búfer calculado */
        uint32_t n = gxp_nparams(f->gxp), base = gxp_params(f->gxp);
        for (uint32_t k = 0; k < n && k < 64; k++) {
            uint32_t p = base + 16 * k, b = p_cont(p), host = b < 14 ? b + 1 : 0;
            if (p_cat(p) != 1) continue;
            char nm[64];
            p_name_str(p, nm, sizeof(nm));
            uint32_t arr = vita_read16(p + 6) ? vita_read16(p + 6) : 1, end = p_res(p) * 4u + p_comps(p) * arr * 4u;
            if (env_cached("MLB15_UB_TODOS")) printf("[ub] fgxp 0x%08X %s cat %u cont %u res %u comps %u x%u\n", f->gxp, nm, p_cat(p), b,
                                                 p_res(p), p_comps(p), arr);
            if (info->buf_off[host] == ~0u || end > info->buf_size[host])
                printf("[ub] fgxp 0x%08X %s: cont %u res %u comps %u x%u fin %u B > búfer %u B (host %u)\n", f->gxp, nm, b,
                       p_res(p), p_comps(p), arr, end, info->buf_off[host] == ~0u ? 0u : info->buf_size[host], host);
        }
        printf("[ub] fgxp 0x%08X datos %u B\n", f->gxp, info->data_bytes);
    }
    return i;
}

/* Copia los buffers de uniformes de una etapa al bloque del SSBO (índice de host
 * 0 = buffer por defecto, 1..14 = buffers 0..13). */
static uint8_t g_v3k_data[2][1u << 18];
uint32_t g_v3k_fgxp_cur;   /* fragmento del draw en curso (pruebas en gpu_vk.c) */

/* Lo que v3k_fill_data copió para el fragmento (draw_v3k lo registra para la relectura tardía). */
static struct { uint32_t off, guest, bytes; } g_fill[18];
static int g_nfill;
static uint32_t g_fill_expo = ~0u;
static uint32_t v3k_fill_data(Ctx *c, const V3kInfo *in, int frag, uint32_t gxp, uint8_t *out) {
    if (frag) { g_nfill = 0; g_fill_expo = ~0u; }
    uint32_t n = in->data_bytes > sizeof(g_v3k_data[0]) ? (uint32_t)sizeof(g_v3k_data[0]) : in->data_bytes, n_extra = 0;
    memset(out, 0, n);
    for (int i = 0; i < 16; i++) {
        if (in->buf_off[i] == ~0u || in->buf_off[i] >= n) continue;
        uint32_t addr;
        if (i == 0) addr = frag ? (c->pfs ? vita_read32(c->pfs + 4) : c->fdef) : c->vdef;
        else if (frag) addr = c->pfs ? vita_read32(c->pfs + 8 + 4u * (uint32_t)(i - 1)) : c->fub[i - 1];
        else addr = c->vub[i - 1];
        uint32_t bytes = in->buf_size[i];
        if (in->buf_off[i] + bytes > n) bytes = n - in->buf_off[i];
        const uint8_t *p = addr ? (const uint8_t *)vita_mem_ptr(addr, bytes) : NULL;
        if (p) memcpy(out + in->buf_off[i], p, bytes);
        if (p && frag && g_nfill < 18) { g_fill[g_nfill].off = in->buf_off[i]; g_fill[g_nfill].guest = addr; g_fill[g_nfill].bytes = bytes; g_nfill++; }
        /* PRUEBA: MLB15_UB_EXTRA=bytes copia también la memoria guest que sigue al último búfer (lo
         * que leería un shader que se pasara de su tamaño). Con MLB15_UB_VENENO=1 esa zona va a NaN:
         * medido, ningún shader del partido la lee (las cuñas negras de la tierra eran registros sin
         * inicializar en el traductor, ver spirv_recompiler.cpp). */
        static int extra = -1;
        if (extra < 0) { const char *e = env_cached("MLB15_UB_EXTRA"); extra = e ? atoi(e) : 0; }
        if (extra && p && in->buf_off[i] + ((in->buf_size[i] + 15u) & ~15u) == n && n + (uint32_t)extra <= sizeof(g_v3k_data[0])) {
            const uint8_t *q = (const uint8_t *)vita_mem_ptr(addr + bytes, (uint32_t)extra);
            if (q) memcpy(out + n, q, (size_t)extra);
            else memset(out + n, 0, (size_t)extra);
            static int veneno = -1;   /* MLB15_UB_VENENO=1: NaN más allá de los datos (prueba: quién lee fuera) */
            if (veneno < 0) veneno = env_cached("MLB15_UB_VENENO") ? 1 : 0;
            if (veneno) memset(out + n, 0xFF, (size_t)extra);
            if (q && frag && g_nfill < 18) { g_fill[g_nfill].off = n; g_fill[g_nfill].guest = addr + bytes; g_fill[g_nfill].bytes = (uint32_t)extra; g_nfill++; }
            n_extra = (uint32_t)extra;
        }
    }
    n += n_extra;
    {   /* PRUEBA: MLB15_ZAP=nombre de uniforme del vértice a poner a 0 / 1 (n:valor) */
        static const char *zap = (const char *)1;
        if (zap == (const char *)1) zap = env_cached("MLB15_ZAP");
        if (zap && !frag && gxp && gxp_find(gxp, "UVP_SkinningMatrix")) {
            char nm[64]; float val = 0.0f;
            const char *colon = strchr(zap, ':');
            size_t ln = colon ? (size_t)(colon - zap) : strlen(zap);
            if (ln >= sizeof(nm)) ln = sizeof(nm) - 1;
            memcpy(nm, zap, ln); nm[ln] = 0;
            if (colon) val = (float)atof(colon + 1);
            uint32_t pe = gxp_find(gxp, nm);
            if (pe) {
                uint32_t b = p_cont(pe), host = b < 14 ? b + 1 : 0;
                if (in->buf_off[host] != ~0u) {
                    uint32_t at = in->buf_off[host] + p_res(pe) * 4u;
                    for (int k = 0; k < 4 && at + 4 * k + 4 <= n; k++) memcpy(out + at + 4 * k, &val, 4);
                }
            }
        }
    }
    /* UFP_ExposureControls.z = 0 → negro (ver fill_sa); mismo apaño mientras no se
     * devuelva el objetivo ToneMapSample a la memoria guest. */
    static int expo = -1;
    if (expo < 0) { const char *e = env_cached("MLB15_EXPOSICION"); expo = e ? atoi(e) : 1; }
    if (frag && expo && gxp) {
        static uint32_t cache_gxp[64], cache_pe[64];   /* gxp_find por nombre era caro en cada draw */
        uint32_t slot = (gxp >> 4) & 63, pe;
        if (cache_gxp[slot] == gxp) pe = cache_pe[slot];
        else { pe = gxp_find(gxp, "UFP_ExposureControls"); cache_gxp[slot] = gxp; cache_pe[slot] = pe; }
        if (pe) {
            uint32_t b = p_cont(pe), host = b < 14 ? b + 1 : 0;
            if (in->buf_off[host] != ~0u) {
                uint32_t at = in->buf_off[host] + p_res(pe) * 4u + 4u;
                static int expo_tr = -1;
                if (expo_tr < 0) expo_tr = env_cached("MLB15_EXPO_TRAZA") ? 1 : 0;
                if (expo_tr && at + 4 <= n) {
                    static int ne;
                    if (ne++ % 2000 == 0) {
                        const uint16_t *h = (const uint16_t *)(out + at - 4);
                        printf("[expo] gxp 0x%08X cont %u res %u: %04X %04X %04X %04X\n", gxp, b, p_res(pe), h[0], h[1], h[2], h[3]);
                    }
                }
                if (rec_on() && at + 4 <= n) {
                    const uint16_t *h = (const uint16_t *)(out + at - 4);
                    rec("    expo %04X %04X %04X %04X\n", h[0], h[1], h[2], h[3]);
                }
                if (at + 2 <= n && out[at] == 0 && out[at + 1] == 0) { out[at] = 0x00; out[at + 1] = 0x3C; }
                if (at + 2 <= n) g_fill_expo = at;
            }
        }
    }
    if (frag && g_sprite_ufp) {
        static int off = -1;
        if (off < 0) off = env_cached("MLB15_SPRITE_SHADER") ? 1 : 0;
        for (int i = 0; i < 7 && !off; i++) {
            uint32_t pu = g_sprite_ufp[i];
            if (!pu) continue;
            uint32_t b = p_cont(pu), host = b < 14 ? b + 1 : 0;
            if (in->buf_off[host] == ~0u) continue;
            uint32_t at = in->buf_off[host] + p_res(pu) * 4u, nc = p_comps(pu) ? p_comps(pu) : 4;
            uint16_t v = i == 0 ? 0x3C00 : 0;   /* SRC_Ck = 1; el resto 0 */
            for (uint32_t k = 0; k < nc && at + 2 * k + 2 <= n; k++) memcpy(out + at + 2 * k, &v, 2);
        }
    }
    {   /* MLB15_AMB=valor: UFP_LightDirShadowMapAndAmbMult.w (luz ambiente de los jugadores)
         * si llega a 0 (prueba) */
        static float amb = -1.0f;
        if (amb < -0.5f) { const char *e = env_cached("MLB15_AMB"); amb = e ? (float)atof(e) : 0.0f; }
        if (frag && amb > 0.0f && gxp) {
            static uint32_t cg[64], cp[64];
            uint32_t slot = (gxp >> 4) & 63, pa;
            if (cg[slot] == gxp) pa = cp[slot];
            else { pa = gxp_find(gxp, "UFP_LightDirShadowMapAndAmbMult"); cg[slot] = gxp; cp[slot] = pa; }
            if (pa) {
                uint32_t b = p_cont(pa), host = b < 14 ? b + 1 : 0;
                if (in->buf_off[host] != ~0u) {
                    uint32_t at = in->buf_off[host] + p_res(pa) * 4u + 6u;   /* .w (f16) */
                    if (at + 2 <= n && out[at] == 0 && out[at + 1] == 0) {
                        uint16_t h = float_to_half(amb);
                        memcpy(out + at, &h, 2);
                    }
                }
            }
        }
    }
    return n;
}

/* Cronómetros por sección de draw_v3k (línea [fps]): 0 preparación+vértices, 1 triángulos+estado,
 * 2 uniformes, 3 texturas+trazas, 4 envío a la GPU (gpu_draw_v3k). */
double g_dv3k_t[8];
static double dv3k_now(void) {
    static double inv;
    if (inv == 0.0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); inv = 1.0 / (double)f.QuadPart; }
    LARGE_INTEGER q; QueryPerformanceCounter(&q); return (double)q.QuadPart * inv;
}
static void draw_v3k(Ctx *c, VProg *v, FProg *f, int fv, const uint32_t streams[16], uint32_t ptype, uint32_t icount,
                     uint32_t nvb, uint32_t ninst, uint32_t maxsrc) {
    {   /* MLB15_V3K_SALTA=lista de fgxp (hex): no dibujar esos programas (aislar fallos) */
        static const char *salta = (const char *)1;
        if (salta == (const char *)1) salta = env_cached("MLB15_V3K_SALTA");
        if (salta) {
            char key[16];
            snprintf(key, sizeof(key), "%08X", f->gxp);
            if (strstr(salta, key)) return;
        }
        /* MLB15_V3K_SALTA_CICLO=fgxp: omite el draw n.º k de ese programa en cada escena, con
         * k = round(fotograma / 150) % 16; con MLB15_FRAMES=150 cada captura deja un hueco distinto. */
        static uint32_t ciclo = 1;
        if (ciclo == 1) { const char *e = env_cached("MLB15_V3K_SALTA_CICLO"); ciclo = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
        if (ciclo && f->gxp == ciclo) {
            extern unsigned port_display_frames(void);
            static unsigned esc = ~0u, k;
            if (esc != g_scenes) { esc = g_scenes; k = 0; }
            if (k++ == ((port_display_frames() + 75) / 150) % 16) return;
        }
        static const char *stex = (const char *)1;   /* MLB15_V3K_SALTA_TEX=datos (hex) de la textura 0 */
        if (stex == (const char *)1) stex = env_cached("MLB15_V3K_SALTA_TEX");
        if (stex) {
            const uint8_t *tw = frag_tex_words(c, 0);
            char key[16];
            if (tw) {
                uint32_t d; memcpy(&d, tw + 8, 4);
                snprintf(key, sizeof(key), "%08X", d);
                if (strstr(stex, key)) return;
            }
        }
    }
    {   /* Capas del montículo/cajón de bateo (UVP_MoundParameters, Sprite 32/44): sin mezcla salen
         * como rectángulos negros bajo los jugadores. Igual que en el camino genérico, no se
         * dibujan; MLB15_MONTICULO=1 las deja. */
        static int show = -1;
        if (show < 0) show = env_cached("MLB15_MONTICULO") ? 1 : 0;
        if (v->color_is_data && !show) { rec("  (montículo omitido) v %08X f %08X\n", v->gxp, f->gxp); return; }
    }
    double cr0 = dv3k_now();
    const V3kInfo *vi = v->v3ki, *fi = f->v3kv[fv].info;
    uint32_t nloc = vi->nloc ? (vi->nloc > 16 ? 16 : vi->nloc) : 1, nv = nvb * ninst, uint_mask = 0;
    if (!g_rverts) g_rverts = (float *)malloc((size_t)MAX_DRAW_VERTS * 16 * 4 * sizeof(float));
    if (!g_rverts) return;
    /* Valores por defecto (0,0,0,1) solo en las locations que ningún atributo escribe entero:
     * antes se rellenaba todo (nv x nloc vec4) y luego se sobrescribía, ~1 200 draws por
     * fotograma. Las de formato registro se ponen a 0 en el bucle de abajo. */
    uint32_t llenas = 0;
    {
        static int todo = -1;
        if (todo < 0) todo = env_cached("MLB15_VERTICES_INIT_TODO") ? 1 : 0;
        for (uint32_t k = 0; k < vi->nattr && !todo; k++) {
            const V3kAttr *va = &vi->attr[k];
            if (va->semantic || va->loc >= nloc) continue;
            const Attr *a = NULL;
            for (int q = 0; q < v->nattr; q++) if (v->attr[q].reg == va->reg) { a = &v->attr[q]; break; }
            if (!a || a->stream >= 16 || !streams[a->stream]) continue;
            uint32_t nl = va->loc + va->nloc > nloc ? nloc - va->loc : va->nloc;
            int comps = a->comps < va->comps ? a->comps : va->comps;
            if (comps > (int)nl * 4) comps = (int)nl * 4;
            uint32_t stride = v->stride[a->stream], cs = comp_size(a->format);
            uint32_t nsrc = (v->index_source[a->stream] & 2) ? ninst : maxsrc + 1;
            if (!vita_mem_ptr(streams[a->stream] + a->offset, (nsrc - 1) * stride + cs * (uint32_t)comps)) continue;
            for (uint32_t l = 0; l < nl; l++)
                if (va->regformat || comps >= (int)(l + 1) * 4) llenas |= 1u << (va->loc + l);
        }
    }
    if (llenas != (nloc >= 32 ? ~0u : (1u << nloc) - 1)) {
        for (uint32_t i = 0; i < nv; i++)
            for (uint32_t l = 0; l < nloc; l++) {
                if (llenas >> l & 1) continue;
                float *o = g_rverts + 4 * ((size_t)i * nloc + l);
                o[0] = o[1] = o[2] = 0.0f; o[3] = 1.0f;
            }
    }
    for (uint32_t k = 0; k < vi->nattr; k++) {
        const V3kAttr *va = &vi->attr[k];
        if (va->semantic || va->loc >= nloc) continue;
        const Attr *a = NULL;
        for (int q = 0; q < v->nattr; q++) if (v->attr[q].reg == va->reg) { a = &v->attr[q]; break; }
        if (!a || a->stream >= 16 || !streams[a->stream]) continue;
        uint32_t nl = va->loc + va->nloc > nloc ? nloc - va->loc : va->nloc;
        int comps = a->comps < va->comps ? a->comps : va->comps;
        if (comps > (int)nl * 4) comps = (int)nl * 4;
        if (va->regformat) {
            for (uint32_t l = 0; l < nl; l++) uint_mask |= 1u << (va->loc + l);
            for (uint32_t i = 0; i < nv; i++)
                for (uint32_t l = 0; l < nl; l++) {
                    uint32_t *o = (uint32_t *)(g_rverts + 4 * ((size_t)i * nloc + va->loc + l));
                    o[0] = o[1] = o[2] = o[3] = 0;
                }
        }
        uint32_t stride = v->stride[a->stream], cs = comp_size(a->format);
        int per_inst = (v->index_source[a->stream] & 2) != 0;
        uint32_t nsrc = per_inst ? ninst : maxsrc + 1;
        const uint8_t *base = (const uint8_t *)vita_mem_ptr(streams[a->stream] + a->offset, (nsrc - 1) * stride + cs * (uint32_t)comps);
        if (!base) continue;
        /* El formato se decide una vez por atributo (antes un switch por componente y vértice). */
        static int lento = -1;
        if (lento < 0) lento = env_cached("MLB15_DECODIFICA_LENTO") ? 1 : 0;
        if (va->regformat || lento || a->format > 9) {
            for (uint32_t i = 0; i < nv; i++) {
                uint32_t src = per_inst ? i / nvb : g_vsrc[i % nvb];
                const uint8_t *at = base + src * stride;
                float *o = g_rverts + 4 * ((size_t)i * nloc + va->loc);
                for (int ci = 0; ci < comps; ci++) {
                    if (va->regformat) {
                        uint32_t x = 0;
                        memcpy(&x, at + cs * (uint32_t)ci, cs);
                        memcpy(&o[ci], &x, 4);
                    } else {
                        o[ci] = read_comp(at, a->format, ci);
                    }
                }
            }
        } else {
            if (!g_h2f_lista) h2f_init();
#define DEC_LOOP(EXPR)                                                                         \
            for (uint32_t i = 0; i < nv; i++) {                                                \
                uint32_t src = per_inst ? i / nvb : g_vsrc[i % nvb];                           \
                const uint8_t *at = base + src * stride;                                       \
                float *o = g_rverts + 4 * ((size_t)i * nloc + va->loc);                        \
                for (int ci = 0; ci < comps; ci++) { EXPR; }                                   \
            }
            switch (a->format) {
                case 0: DEC_LOOP(o[ci] = (float)at[ci]) break;
                case 1: DEC_LOOP(o[ci] = (float)(int8_t)at[ci]) break;
                case 2: DEC_LOOP(uint16_t h; memcpy(&h, at + 2 * ci, 2); o[ci] = (float)h) break;
                case 3: DEC_LOOP(uint16_t h; memcpy(&h, at + 2 * ci, 2); o[ci] = (float)(int16_t)h) break;
                case 4: DEC_LOOP(o[ci] = at[ci] * (1.0f / 255.0f)) break;
                case 5: DEC_LOOP(float x = (int8_t)at[ci] / 127.0f; o[ci] = x < -1.0f ? -1.0f : x) break;
                case 6: DEC_LOOP(uint16_t h; memcpy(&h, at + 2 * ci, 2); o[ci] = h / 65535.0f) break;
                case 7: DEC_LOOP(uint16_t h; memcpy(&h, at + 2 * ci, 2); float x = (int16_t)h / 32767.0f; o[ci] = x < -1.0f ? -1.0f : x) break;
                case 8: DEC_LOOP(uint16_t h; memcpy(&h, at + 2 * ci, 2); o[ci] = g_h2f[h]) break;
                default: DEC_LOOP(memcpy(&o[ci], at + 4 * ci, 4)) break;
            }
#undef DEC_LOOP
        }
    }
    {   /* MLB15_TRAZA_FGXP=hex: rango de cada location y buffers del vértice */
        static uint32_t want = 1;
        static int n;
        if (want == 1) { const char *e = env_cached("MLB15_TRAZA_FGXP"); want = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
        if (want && f->gxp == want && n++ < 4) {
            printf("[trazav] vgxp 0x%08X nv %u nloc %u uint_mask 0x%X\n", v->gxp, nv, nloc, uint_mask);
            for (uint32_t l = 0; l < nloc; l++) {
                float mn[4] = { 1e30f, 1e30f, 1e30f, 1e30f }, mx[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
                for (uint32_t i = 0; i < nv; i++)
                    for (int ci = 0; ci < 4; ci++) {
                        float x = g_rverts[4 * ((size_t)i * nloc + l) + ci];
                        if (uint_mask >> l & 1) { uint32_t u; memcpy(&u, &x, 4); x = (float)u; }
                        if (x < mn[ci]) mn[ci] = x;
                        if (x > mx[ci]) mx[ci] = x;
                    }
                printf("   loc%u: [%.3g..%.3g] [%.3g..%.3g] [%.3g..%.3g] [%.3g..%.3g]\n", l, mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], mn[3], mx[3]);
            }
            for (uint32_t k = 0; k < vi->nattr; k++) {
                const Attr *a = NULL;
                for (int q = 0; q < v->nattr; q++) if (v->attr[q].reg == vi->attr[k].reg) { a = &v->attr[q]; break; }
                printf("   attr reg %u loc %u nloc %u comps %u regformat %u sem %u | fmt %u comps %u\n", vi->attr[k].reg, vi->attr[k].loc,
                       vi->attr[k].nloc, vi->attr[k].comps, vi->attr[k].regformat, vi->attr[k].semantic, a ? a->format : 99u, a ? a->comps : 0u);
            }
            uint32_t pm = gxp_find(v->gxp, "UVP_SkinningMatrix");
            if (pm) printf("   UVP_SkinningMatrix cont %u res %u comps %u array %u\n", p_cont(pm), p_res(pm), p_comps(pm), vita_read16(pm + 6));
            for (int i = 0; i < 16; i++) if (vi->buf_off[i] != ~0u) printf("   vbuf h%d off %u size %u\n", i, vi->buf_off[i], vi->buf_size[i]);
            for (int b = 0; b < 8; b++) {
                uint32_t addr = b == 0 ? c->vdef : c->vub[b - 1];
                if (!addr || vi->buf_off[b] == ~0u) continue;
                printf("   h%d @0x%08X f32:", b, addr);
                for (uint32_t k = 0; k < 12 && k * 4 < vi->buf_size[b]; k++) { uint32_t x = vita_read32(addr + 4 * k); float fx; memcpy(&fx, &x, 4); printf(" %.4g", fx); }
                printf("\n");
            }
            {   uint32_t n = gxp_nparams(v->gxp), base = gxp_params(v->gxp);
                for (uint32_t i = 0; i < n && i < 64; i++) {
                    uint32_t p = base + 16 * i;
                    if (p_cat(p) != 1) continue;
                    char nm[64]; p_name_str(p, nm, sizeof(nm));
                    printf("   uniforme %-28s cont %u res %u tipo %u comps %u\n", nm, p_cont(p), p_res(p), p_type(p), p_comps(p));
                }
            }
            fflush(stdout);
        }
    }
    double cr1 = dv3k_now(); g_dv3k_t[0] += cr1 - cr0;
    uint32_t nt = build_tris(ptype, icount, nvb, ninst);
    GpuDraw d;
    memset(&d, 0, sizeof(d));
    draw_state(c, f, &d);
    /* RenderVertUniformBlock / RenderFragUniformBlock (shader/uniform_block.h) */
    float vinfo[9] = { 1.0f, -1.0f, 1.0f, 1.0f, d.vp_enable ? 1.0f : 0.0f, (float)c->tw, (float)c->th,
                       d.vp_enable ? d.vz[0] : 0.0f, d.vp_enable ? d.vz[1] : 1.0f };
    float finfo[5] = { 0.0f, 0.0f, 0.0f, 0.0f, (float)gpu_res_scale() };   /* [4] = res_multiplier */
    float viewport[4];
    if (d.vp_enable) {
        float xs = fabsf(d.vp[1]), ys = d.vp[3];
        viewport[0] = d.vp[0] - xs; viewport[1] = d.vp[2] - ys;
        viewport[2] = 2.0f * xs;    viewport[3] = 2.0f * ys;
    } else {
        viewport[0] = viewport[1] = 0.0f;
        viewport[2] = (float)c->tw; viewport[3] = (float)c->th;
    }
    if (env_cached("MLB15_DS_TRAZA") && c->tw == 2048 && c->th == 1024) {
        static int n;
        if (n++ < 6)
            printf("[ds] draw sombras: vp_en %d vp %.1f %.1f %.1f %.1f z %.3f %.3f | zfunc %u zwrite %u cull %u cmask %X vgxp 0x%08X fgxp 0x%08X sesgo %g/%g\n",
                   d.vp_enable, d.vp[0], d.vp[1], d.vp[2], d.vp[3], d.vz[0], d.vz[1], d.depth_func, d.depth_write, d.cull,
                   d.color_mask, v->gxp, f->gxp, d.dbias_factor, d.dbias_units);
    }
    double cr2 = dv3k_now(); g_dv3k_t[1] += cr2 - cr1;
    uint32_t vb = v3k_fill_data(c, vi, 0, v->gxp, g_v3k_data[0]);
    uint32_t fb = v3k_fill_data(c, fi, 1, f->gxp, g_v3k_data[1]);
    double cr3 = dv3k_now(); g_dv3k_t[2] += cr3 - cr2;
    static int clear_tr = -1;
    if (clear_tr < 0) clear_tr = env_cached("MLB15_CLEAR_TRAZA") ? 1 : 0;
    if (clear_tr && nv == 3 && fb >= 16 && c->tw >= 1024) {   /* borrados de superficies grandes */
        static int n;
        if (n++ < 40) {
            const float *cc = (const float *)g_v3k_data[1];
            const uint16_t *hh = (const uint16_t *)g_v3k_data[1];
            printf("[clear] fgxp 0x%08X %ux%u f32(%.3f %.3f %.3f %.3f) f16(%04X %04X %04X %04X) out %u\n", f->gxp, c->tw, c->th,
                   cc[0], cc[1], cc[2], cc[3], hh[0], hh[1], hh[2], hh[3], fi->output_type);
        }
    }
    {   /* MLB15_V3K_UBTRAZA=fgxp (hex): uniformes del fragmento (f16) de ese programa */
        static uint32_t want = 1;
        static int shown;
        if (want == 1) { const char *e = env_cached("MLB15_V3K_UBTRAZA"); want = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
        extern int port_en_partido;
        if (want && f->gxp == want && port_en_partido && shown < 6 && (g_draws % 97) == 0) {
            shown++;
            printf("[ubtraza] fgxp=0x%08X pfs=0x%08X data=%u B:", f->gxp, c->pfs, fb);
            for (int i = 0; i < 16; i++) if (fi->buf_off[i] != ~0u) printf(" [h%d @%u %uB]", i, fi->buf_off[i], fi->buf_size[i]);
            printf("\n  f16:");
            for (uint32_t i = 0; i < fb / 2 && i < 160; i++) {
                uint16_t h;
                memcpy(&h, g_v3k_data[1] + 2 * i, 2);
                printf(" %.3g", half_to_float(h));
            }
            printf("\n");
        }
    }
    {   /* MLB15_UNIF_NOMBRES=fgxp (hex): uniformes del fragmento por nombre (4 f16 de cada uno) */
        static uint32_t want = 1;
        static int shown;
        if (want == 1) { const char *e = env_cached("MLB15_UNIF_NOMBRES"); want = e ? (uint32_t)strtoul(e, NULL, 16) : 0; if (want == 0 && e) want = 2; }
        if (want == 2) want = 1u;   /* "0": cualquier programa con UFP_QuickAmbientColor */
        { static int once; if (!once++) printf("[unif] activo want=%u\n", want); }
        extern int port_en_partido;
        static unsigned calls;
        if (want && shown < 4 && (f->gxp == want || (want == 1u && env_cached("MLB15_UNIF_NOMBRES") && gxp_find(f->gxp, "UFP_QuickAmbientColor"))) && ++calls % 400 == 0) {
            shown++;
            uint32_t n = gxp_nparams(f->gxp), base = gxp_params(f->gxp);
            printf("[unif] fgxp=0x%08X\n", f->gxp);
            for (uint32_t i = 0; i < n && i < 64; i++) {
                uint32_t p = base + 16 * i, b = p_cont(p), host = b < 14 ? b + 1 : 0;
                if (p_cat(p) != 1 || fi->buf_off[host] == ~0u) continue;
                char nm[64];
                p_name_str(p, nm, sizeof(nm));
                uint32_t at = fi->buf_off[host] + p_res(p) * 4u;
                printf("  %-32s tipo %u comps %u:", nm, p_type(p), p_comps(p));
                for (int k = 0; k < 4 && at + 2 * k + 2 <= fb; k++) {
                    uint16_t h; memcpy(&h, g_v3k_data[1] + at + 2 * k, 2);
                    printf(" %.3g", half_to_float(h));
                }
                printf("\n");
            }
            fflush(stdout);
        }
    }
    static GpuTexture tex[16];
    const GpuTexture *tp[16];
    for (int u = 0; u < 16; u++) {
        tp[u] = NULL;
        if (!(fi->tex_mask >> u & 1)) continue;
        {   /* MLB15_V3K_BLANCO=máscara: esas unidades van en blanco (para aislar fallos) */
            static int blanco = -1;
            if (blanco < 0) { const char *e = env_cached("MLB15_V3K_BLANCO"); blanco = e ? (int)strtol(e, NULL, 0) : 0; }
            static const char *solo = (const char *)1;   /* MLB15_V3K_BLANCO_SOLO=fgxp,...: solo en esos programas */
            if (solo == (const char *)1) solo = env_cached("MLB15_V3K_BLANCO_SOLO");
            if (blanco >> u & 1) {   /* (antes el snprintf se hacía en cada draw y unidad aunque no se usara) */
                char key[16];
                snprintf(key, sizeof(key), "%08X", f->gxp);
                if (!solo || strstr(solo, key)) continue;
            }
            /* MLB15_TEX_NOMBRE_BLANCO=Nombre,...: esas texturas por nombre del sampler del GXP */
            static const char *nb = (const char *)1;
            if (nb == (const char *)1) nb = env_cached("MLB15_TEX_NOMBRE_BLANCO");
            if (nb) {
                int skip = 0;
                uint32_t np = gxp_nparams(f->gxp), base = gxp_params(f->gxp);
                for (uint32_t k = 0; k < np && k < 64 && !skip; k++) {
                    uint32_t p = base + 16 * k;
                    if (p_cat(p) != 2 || p_res(p) != (uint32_t)u) continue;
                    char nm[64];
                    p_name_str(p, nm, sizeof(nm));
                    if (strstr(nb, nm)) skip = 1;
                }
                if (skip) continue;
            }
            /* MLB15_TEX_NOMBRE_COLOR=Nombre=RRGGBBAA,...: esa textura (por nombre del sampler)
             * como un color fijo 1x1 (gpu_vk.c, texture_get_) */
            static const char *nc = (const char *)1;
            if (nc == (const char *)1) nc = env_cached("MLB15_TEX_NOMBRE_COLOR");
            if (nc) {
                uint32_t np = gxp_nparams(f->gxp), base = gxp_params(f->gxp);
                int hit = -1;
                for (uint32_t k = 0; k < np && k < 64 && hit < 0; k++) {
                    uint32_t p = base + 16 * k;
                    if (p_cat(p) != 2 || p_res(p) != (uint32_t)u) continue;
                    char nm[64];
                    p_name_str(p, nm, sizeof(nm));
                    const char *q = nc;
                    for (int idx = 0; q && *q; idx++) {
                        const char *eq = strchr(q, '=');
                        if (!eq) break;
                        if ((size_t)(eq - q) == strlen(nm) && !strncmp(q, nm, (size_t)(eq - q))) { hit = idx; break; }
                        q = strchr(eq, ',');
                        if (q) q++;
                    }
                }
                if (hit >= 0) {
                    memset(&tex[u], 0, sizeof(tex[u]));
                    tex[u].data = 0xFE000000u | (uint32_t)hit; tex[u].width = tex[u].height = 1;
                    tp[u] = &tex[u];
                    continue;
                }
            }
        }
        const uint8_t *tw = frag_tex_words(c, u);
        if (!tw) continue;
        memset(&tex[u], 0, sizeof(tex[u]));
        tex_from_words(tw, &tex[u]);
        if (tex[u].data && tex[u].width && tex[u].height) tp[u] = &tex[u];
    }
    {   /* MLB15_TRAZA_FGXP=hex: texturas de cada unidad en los dibujos de ese fragmento */
        static uint32_t want = 1;
        static int n;
        if (want == 1) { const char *e = env_cached("MLB15_TRAZA_FGXP"); want = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
        if (want && f->gxp == want && n++ < 24) {
            printf("[trazatex] mask 0x%X:", fi->tex_mask);
            for (int u = 0; u < 16; u++)
                if (fi->tex_mask >> u & 1)
                    printf(" T%d=%08X/%ux%u/%08X", u, tp[u] ? tp[u]->data : 0, tp[u] ? tp[u]->width : 0, tp[u] ? tp[u]->height : 0,
                           tp[u] ? tp[u]->format : 0);
            printf("\n");
        }
        /* MLB15_TRAZA_TEX=hex: dibujos que leen esa textura, y dibujos sobre una superficie
         * de ese tamaño (MLB15_TRAZA_TAM=512x256) */
        static uint32_t wt = 1, tw = 0, th = 0;
        static int n2, n3;
        if (wt == 1) {
            const char *e = env_cached("MLB15_TRAZA_TEX"); wt = e ? (uint32_t)strtoul(e, NULL, 16) : 0;
            if ((e = env_cached("MLB15_TRAZA_TAM")) != NULL) sscanf(e, "%ux%u", &tw, &th);
        }
        int lee = 0;
        for (int u = 0; u < 16 && wt; u++) if (tp[u] && tp[u]->data == wt) lee = 1;
        int sobre = tw && c->tw == tw && c->th == th;
        if ((lee && n2++ < 30) || (sobre && n3++ < 60)) {
            extern uint32_t gpu_target_addr(void);
            printf("[trazatex%s] sobre %08X v %08X f %08X tri %u blend %u(%u,%u) cmask %X:", lee ? " lee" : " dibuja", gpu_target_addr(), v->gxp, f->gxp, nt / 3,
                   d.blend, d.color_src, d.color_dst, d.color_mask);
            for (int u = 0; u < 16; u++)
                if (tp[u]) printf(" T%d=%08X/%ux%u/%08X/t%u%s", u, tp[u]->data, tp[u]->width, tp[u]->height, tp[u]->format, tp[u]->type,
                                  tp[u]->gamma ? "g" : "");
            printf("\n");
        }
    }
    if (rec_on()) {
        rec("  v %08X f %08X tri %u blend %u(%u,%u) z %u/%u st %u cmask %X coff %u cull %u:", v->gxp, f->gxp, nt / 3, d.blend,
            d.color_src, d.color_dst, d.depth_func, d.depth_write, d.stencil, d.color_mask, d.color_off, d.cull);
        for (int u = 0; u < 16; u++) if (tp[u]) rec(" T%d=%08X/%ux%u/%08X", u, tp[u]->data, tp[u]->width, tp[u]->height, tp[u]->format);
        uint32_t hv = 2166136261u, hf = 2166136261u;
        for (uint32_t i = 0; i < vb; i++) hv = (hv ^ g_v3k_data[0][i]) * 16777619u;
        for (uint32_t i = 0; i < fb; i++) hf = (hf ^ g_v3k_data[1][i]) * 16777619u;
        rec(" UV=%08X UF=%08X", hv, hf);
        {   /* huella de los atributos de vértice ya convertidos */
            uint32_t ha = 2166136261u;
            const uint8_t *pa = (const uint8_t *)g_rverts;
            for (size_t i = 0; i < (size_t)nv * nloc * 16; i++) ha = (ha ^ pa[i]) * 16777619u;
            rec(" VA=%08X", ha);
            uint32_t hi = 2166136261u;
            for (uint32_t i = 0; i < nt; i++) hi = (hi ^ g_tri[i]) * 16777619u;
            rec(" IX=%08X vp=%g,%g,%g,%g z=%g,%g", hi, viewport[0], viewport[1], viewport[2], viewport[3], vinfo[7], vinfo[8]);
        }
        static uint32_t want = 1;   /* MLB15_NEGRO_FGXP=hex: uniformes por nombre de ese fragmento (f16) */
        if (want == 1) { const char *e = env_cached("MLB15_NEGRO_FGXP"); want = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
        if (want && f->gxp == want) {
            rec("\n      bruto %u B:", fb);
            for (uint32_t i = 0; i + 4 <= fb && i < 1024; i += 4) { uint32_t w; memcpy(&w, g_v3k_data[1] + i, 4); rec(" %08X", w); }
            for (int i = 0; i < 16; i++) if (fi->buf_off[i] != ~0u) rec(" [h%d @%u %uB]", i, fi->buf_off[i], fi->buf_size[i]);
            rec("\n      vertice %u B:", vb);
            for (uint32_t i = 0; i + 4 <= vb && i < 1024; i += 4) { uint32_t w; memcpy(&w, g_v3k_data[0] + i, 4); rec(" %08X", w); }
            for (int i = 0; i < 16; i++) if (vi->buf_off[i] != ~0u) rec(" [h%d @%u %uB]", i, vi->buf_off[i], vi->buf_size[i]);
            uint32_t n = gxp_nparams(f->gxp), base = gxp_params(f->gxp);
            for (uint32_t i = 0; i < n && i < 64; i++) {
                uint32_t p = base + 16 * i, b = p_cont(p), host = b < 14 ? b + 1 : 0;
                if (p_cat(p) != 1 || fi->buf_off[host] == ~0u) continue;
                char nm[64];
                p_name_str(p, nm, sizeof(nm));
                uint32_t at = fi->buf_off[host] + p_res(p) * 4u, nc = p_comps(p) * (vita_read16(p + 6) ? vita_read16(p + 6) : 1);
                rec("\n      %s:", nm);
                for (uint32_t k = 0; k < nc && k < 16 && at + 2 * k + 2 <= fb; k++) {
                    uint16_t h; memcpy(&h, g_v3k_data[1] + at + 2 * k, 2);
                    rec(" %.3g", half_to_float(h));
                }
            }
        }
        rec("\n");
    }
    {   /* MLB15_TRAZA_FGXP=hex: uniformes (f16) del fragmento por nombre, en los primeros dibujos */
        static uint32_t want = 1;
        static int n;
        if (want == 1) { const char *e = env_cached("MLB15_TRAZA_FGXP"); want = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
        if (want && f->gxp == want && n++ < 40) {
            extern uint32_t gpu_target_addr(void);
            uint32_t np = gxp_nparams(f->gxp), base = gxp_params(f->gxp);
            printf("[unif] --- sobre %08X T0 %08X blend %u c(%u,%u) a(%u,%u) cmask %X z %u/%u st %u cull %u vp %d %.0f %.0f %.0f %.0f\n",
                   gpu_target_addr(), tp[0] ? tp[0]->data : 0, d.blend, d.color_src, d.color_dst, d.alpha_src, d.alpha_dst, d.color_mask,
                   d.depth_func, d.depth_write, d.stencil, d.cull, d.vp_enable, d.vp[0], d.vp[1], d.vp[2], d.vp[3]);
            for (uint32_t i = 0; i < nv && i < 4; i++) {
                printf("[unif]   v%u:", i);
                for (uint32_t l = 0; l < nloc && l < 3; l++) printf(" (%.3g %.3g %.3g %.3g)", g_rverts[4 * ((size_t)i * nloc + l)], g_rverts[4 * ((size_t)i * nloc + l) + 1],
                                                                 g_rverts[4 * ((size_t)i * nloc + l) + 2], g_rverts[4 * ((size_t)i * nloc + l) + 3]);
                printf("\n");
            }
            for (uint32_t i = 0; i < np && i < 64; i++) {
                uint32_t p = base + 16 * i, b = p_cont(p), host = b < 14 ? b + 1 : 0;
                if (p_cat(p) != 1 || fi->buf_off[host] == ~0u) continue;
                char nm[64];
                p_name_str(p, nm, sizeof(nm));
                uint32_t at = fi->buf_off[host] + p_res(p) * 4u, nc = p_comps(p) * (vita_read16(p + 6) ? vita_read16(p + 6) : 1);
                printf("[unif] %s:", nm);
                for (uint32_t k = 0; k < nc && k < 24 && at + 2 * k + 2 <= fb; k++) {
                    uint16_t h; memcpy(&h, g_v3k_data[1] + at + 2 * k, 2);
                    printf(" %.3g", half_to_float(h));
                }
                printf("\n");
            }
        }
    }
    if (scene_dump())
        printf("[escena v3k] vgxp=0x%08X fgxp=0x%08X tri=%u v=%u locs=%u tex=0x%X blend=%u(%u,%u) z=%u/%u vp=%d%s\n", v->gxp, f->gxp,
               nt / 3, nv, nloc, fi->tex_mask, d.blend, d.color_src, d.color_dst, d.depth_func, d.depth_write, d.vp_enable, fi->frag_color ? " LEE-COLOR" : "");
    if (scene_dump())
        printf("    st=%u [%u %u/%u/%u r%u m%02X/%02X | %u %u/%u/%u] coff=%u cull=%u mask=%X\n", d.stencil, d.st[0].func,
               d.st[0].fail, d.st[0].zfail, d.st[0].pass, d.st[0].ref, d.st[0].cmask, d.st[0].wmask, d.st[1].func, d.st[1].fail,
               d.st[1].zfail, d.st[1].pass, d.color_off, d.cull, d.color_mask);
    if (scene_dump())
        for (int u = 0; u < 16; u++)
            if (tp[u]) printf("    T%d: 0x%08X %ux%u fmt=0x%08X tipo=%u\n", u, tp[u]->data, tp[u]->width, tp[u]->height, tp[u]->format, tp[u]->type);
    if (scene_dump() && nv <= 12 && env_cached("MLB15_ESCENA_VERTICES")) {   /* atributos de draws pequeños */
        for (uint32_t i = 0; i < nv; i++) {
            printf("      v%u:", i);
            for (uint32_t l = 0; l < nloc && l < 6; l++)
                printf(" (%.3f %.3f %.3f %.3f)", g_rverts[(i * nloc + l) * 4], g_rverts[(i * nloc + l) * 4 + 1],
                       g_rverts[(i * nloc + l) * 4 + 2], g_rverts[(i * nloc + l) * 4 + 3]);
            printf("\n");
        }
        printf("      tri:");
        for (uint32_t i = 0; i < nt && i < 18; i++) printf(" %u", g_tri[i]);
        printf("\n");
    }
    {   /* MLB15_TRAZA_ROJO=N: draws pequeños con algún atributo rojo puro (zona caliente/fría del bateador) */
        static long on = -2; static int n;   /* =N: desde el fotograma N */
        extern unsigned port_display_frames(void);
        if (on == -2) { const char *e = env_cached("MLB15_TRAZA_ROJO"); on = e ? atol(e) : -1; }
        if (on >= 0 && (long)port_display_frames() >= on && nv <= 12 && n < 200) {
            int rojo = 0;
            for (uint32_t i = 0; i < nv && !rojo; i++)
                for (uint32_t l = nloc - 1; l < nloc; l++) {   /* el último atributo es el color */
                    const float *a = &g_rverts[(i * nloc + l) * 4];
                    if (a[0] > 0.25f && a[1] < 0.08f && a[2] < 0.08f && a[3] > 0.05f && a[3] <= 1.0f) rojo = 1;
                }
            if (rojo) {   /* una vez por firma (fragmento, nº de vértices, color, textura) */
                static uint32_t vistos[256]; static int nvistos;
                const float *c0 = &g_rverts[(nloc - 1) * 4];
                uint32_t firma = f->gxp ^ (nv << 24) ^ ((uint32_t)(c0[0] * 255) << 16) ^ ((uint32_t)(c0[1] * 255) << 8) ^
                                 (uint32_t)(c0[2] * 255) ^ (tp[0] ? tp[0]->data : 0) ^ ((uint32_t)g_rverts[0] << 4);
                for (int k = 0; k < nvistos; k++) if (vistos[k] == firma) rojo = 0;
                if (rojo && nvistos < 256) vistos[nvistos++] = firma;
            }
            if (rojo) {
                n++;
                printf("[rojo] fot %u vgxp=0x%08X fgxp=0x%08X v=%u locs=%u tex=0x%X blend=%u c(%u,%u) a(%u,%u) mask=%X st=%u\n",
                       port_display_frames(), v->gxp, f->gxp, nv, nloc, fi->tex_mask, d.blend, d.color_src, d.color_dst,
                       d.alpha_src, d.alpha_dst, d.color_mask, d.stencil);
                for (uint32_t i = 0; i < nv; i++) {
                    printf("      v%u:", i);
                    for (uint32_t l = 0; l < nloc && l < 4; l++)
                        printf(" (%.3f %.3f %.3f %.3f)", g_rverts[(i * nloc + l) * 4], g_rverts[(i * nloc + l) * 4 + 1],
                               g_rverts[(i * nloc + l) * 4 + 2], g_rverts[(i * nloc + l) * 4 + 3]);
                    printf("\n");
                }
                for (int u = 0; u < 16; u++)
                    if (tp[u]) printf("      T%d: 0x%08X %ux%u fmt=0x%08X\n", u, tp[u]->data, tp[u]->width, tp[u]->height, tp[u]->format);
            }
        }
    }
    int fmod_v3k = f->v3kv[fv].mod;
    {   /* MLB15_V3K_FRAGDBG=fgxp:n (hex:dec): ese fragmento pinta |v_TexCoord<n>| (depuración de varyings) */
        static uint32_t want = 1;
        static int dbg_mod, which;
        if (want == 1) {
            const char *e = env_cached("MLB15_V3K_FRAGDBG");
            want = e ? (uint32_t)strtoul(e, NULL, 16) : 0;
            which = e && strchr(e, ':') ? atoi(strchr(e, ':') + 1) : 0;
            if (want) {
                char src[512];
                snprintf(src, sizeof(src), "#version 450\nlayout(location = %d) in vec4 t;\nlayout(location = 0) out vec4 o;\n"
                         "void main() { o = vec4(abs(t.xyz), 1.0); }\n", 4 + which);
                uint32_t *spv = NULL; size_t n = 0; char err[512];
                if (shc_compile(src, 1, &spv, &n, err, sizeof(err))) {
                    for (size_t i = 0; i + 1 < n; i++)   /* "main" -> "main_fs" (mismo tamaño en palabras) */
                        if (spv[i] == 0x6E69616Du && spv[i + 1] == 0) spv[i + 1] = 0x0073665Fu;
                    dbg_mod = gpu_module(spv, n);
                } else printf("[v3k] fragdbg: %s\n", err);
                free(spv);
            }
        }
        if (want && f->gxp == want && dbg_mod) fmod_v3k = dbg_mod;
    }
    g_v3k_fgxp_cur = f->gxp;
    extern uint32_t gpu_last_fdata_off(void);
    extern void gpu_ub_tarde(uint32_t guest, uint32_t bytes, uint32_t mb_off);
    uint32_t prev_off = gpu_last_fdata_off();
    double cr4 = dv3k_now(); g_dv3k_t[3] += cr4 - cr3;
    gpu_draw_v3k(&d, v->v3k_mod, fmod_v3k, nloc, uint_mask, g_rverts, nv, g_tri, nt, vinfo, sizeof(vinfo), finfo,
                 sizeof(finfo), g_v3k_data[0], vb, g_v3k_data[1], fb, viewport, tp);
    g_dv3k_t[4] += dv3k_now() - cr4; g_dv3k_t[5] += 1.0; g_dv3k_t[6] += nv;
    {   /* MLB15_UB_TARDE=0 lo desactiva */
        static int tarde = -1;
        if (tarde < 0) { const char *e = env_cached("MLB15_UB_TARDE"); tarde = e ? atoi(e) : 0; }   /* medido: el juego reutiliza esa memoria en anillo durante la escena; releerla al final da datos de otros draws */
        uint32_t mo = gpu_last_fdata_off();
        (void)prev_off;
        if (tarde && mo != ~0u) {
            for (int i = 0; i < g_nfill; i++) gpu_ub_tarde(g_fill[i].guest, g_fill[i].bytes, mo + g_fill[i].off);
            if (g_fill_expo != ~0u) gpu_ub_tarde(0, 0, mo + g_fill_expo);
        }
    }
}

/* wrap != 0: draw instanciado. Como en GXM, el buffer de índices tiene `wrap`
 * índices que se repiten para cada una de las icount/wrap instancias; los
 * streams con index_source de instancia (bit 0) avanzan una vez por instancia.
 * Antes se leían icount índices: basura más allá del buffer, hasta 65 536
 * vértices convertidos por draw, y los atributos por instancia mal leídos. */
static void do_draw(Ctx *c, uint32_t vprog, const uint32_t streams[16], uint32_t prim, uint32_t itype,
                    uint32_t idata, uint32_t icount, uint32_t wrap) {
    g_draws++;
    { extern volatile unsigned long port_draw_tid; static unsigned long t; if (!t) port_draw_tid = t = GetCurrentThreadId(); }
    VProg *v = vp_find(vprog);
    uint32_t fprog = c->pfs ? vita_read32(c->pfs) : c->fprog;
    FProg *f = fp_find(fprog);
    if (!v || !f || !gpu_ready() || !icount || icount > MAX_DRAW_IDX) {
        g_skipped++, g_skip_why[0]++, g_skip_junk++;
        if (tr()) {
            static int n;
            if (n++ < 12) printf("[gxm] draw descartado: vprog=0x%08X(%s) fprog=0x%08X(%s) pfs=0x%08X n=%u\n", vprog,
                                 v ? "ok" : "?", fprog, f ? "ok" : "?", c->pfs, icount);
        }
        return;
    }
    uint32_t ptype = prim >> 26;                   /* 0 tri, 1 lines, 2 points, 3 strip, 4 fan */
    if (ptype == 1 || ptype == 2 || ptype > 4) {
        g_skipped++, g_skip_why[1]++;
        if (env_cached("MLB15_PRIM_TRAZA")) {
            static unsigned n;
            if (n++ % 500 == 0) printf("[prim] descartado tipo %u (n=%u) vgxp=0x%08X fgxp=0x%08X indices=%u\n", ptype, n, v->gxp, f->gxp, icount);
        }
        return;
    }

    if (tr() && v->skin) {
        static int shown;
        if (shown++ < 12) printf("[skin] draw vgxp=0x%08X idata=0x%08X n=%u wrap=%u prim=0x%08X s0=0x%08X s1=0x%08X vub3=0x%08X\n",
                                 v->gxp, idata, icount, wrap, prim, streams[0], streams[1], c->vub[3]);
    }
    uint32_t ninst = 1;
    if (wrap && wrap < icount) { ninst = icount / wrap; icount = wrap; }
    int u32 = (itype & 0x01000000u) != 0;
    const uint8_t *ip = (const uint8_t *)vita_mem_ptr(idata, icount * (u32 ? 4u : 2u));
    if (!ip) {
        g_skipped++, g_skip_why[2]++;
        if (tr()) {
            static int shown;
            if (shown++ < 10) printf("[gxm] índices sin memoria: idata=0x%08X n=%u wrap=%u itype=0x%08X prim=0x%08X vgxp=0x%08X skin=%d\n",
                                     idata, icount, wrap, itype, prim, v->gxp, v->skin != 0);
        }
        return;
    }
    /* Solo los vértices que usan los índices, renumerados 0..nvb-1 (g_vsrc da el
     * original). El partido dibuja trozos de 63 índices de un buffer de 64 000
     * vértices: convertir 0..máx eran 100 vértices por índice. */
    uint32_t nvb = 0;
    if (++g_stamp_id == 0) { memset(g_stamp, 0, sizeof(g_stamp)); g_stamp_id = 1; }
    for (uint32_t i = 0; i < icount; i++) {
        uint32_t x;
        if (u32) memcpy(&x, ip + 4 * i, 4);
        else { uint16_t h; memcpy(&h, ip + 2 * i, 2); x = h; }
        if (x >= MAX_INDEX_VALUE || nvb >= MAX_DRAW_VERTS) { g_skipped++, g_skip_why[3]++; return; }
        if (g_stamp[x] != g_stamp_id) { g_stamp[x] = g_stamp_id; g_slot[x] = nvb; g_vsrc[nvb++] = x; }
        g_idx[i] = g_slot[x];
    }
    uint32_t maxsrc = 0;
    for (uint32_t i = 0; i < nvb; i++) if (g_vsrc[i] > maxsrc) maxsrc = g_vsrc[i];
    if (nvb > MAX_DRAW_VERTS) { g_skipped++, g_skip_why[4]++; return; }
    if (nvb * ninst > MAX_DRAW_VERTS) ninst = MAX_DRAW_VERTS / nvb;
    if (icount * 3u * ninst > MAX_DRAW_IDX) ninst = MAX_DRAW_IDX / (icount * 3u);
    if (!ninst) { g_skipped++, g_skip_why[5]++; return; }
    uint32_t nv = nvb * ninst;
    g_stat_verts += nv; g_stat_idx += icount * ninst;
    if (nv > g_stat_maxv) { g_stat_maxv = nv; g_stat_maxi = icount; g_stat_maxgxp = v->gxp; }
    if (tr()) {
        static int shown;
        if (nvb == 1 && icount > 3 && shown++ < 3) {
            printf("[gxm] indices todos 0: prim=0x%08X itype=0x%08X idata=0x%08X n=%u bytes:", prim, itype, idata, icount);
            for (int i = 0; i < 16; i++) printf(" %02X", vita_read8(idata + (uint32_t)i));
            uint32_t nz = 0, first = 0;
            for (uint32_t i = 0; i < 0x4000; i += 2) if (vita_read16(idata + i)) { if (!nz) first = i; nz++; }
            printf(" | 16 KB siguientes: %u halfwords no cero (primero en +0x%X)", nz, first);
            vfp_reg q[6];
            for (int i = 0; i < 6; i++) q[i].u = vita_read32(streams[0] + 4u * (uint32_t)i);
            printf(" | v0: %.2f %.2f %.2f uv %.3f %.3f col %08X", q[0].f, q[1].f, q[2].f, q[3].f, q[4].f, q[5].u);
            printf(" | vprog gxp=0x%08X attrs=%d", v->gxp, v->nattr);
            for (int k = 0; k < v->nattr; k++)
                printf(" [s%u+%u f%u x%u r%u->%d stream=0x%08X stride=%u]", v->attr[k].stream, v->attr[k].offset,
                       v->attr[k].format, v->attr[k].comps, v->attr[k].reg, v->attr[k].slot,
                       streams[v->attr[k].stream & 15], v->stride[v->attr[k].stream & 15]);
            printf("\n");
        }
    }

    {   /* MLB15_TRAZA_FGXP=hex: por qué no se dibuja ese programa de fragmento */
        static uint32_t want = 1;
        static int n;
        if (want == 1) { const char *e = env_cached("MLB15_TRAZA_FGXP"); want = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
        if (want && f->gxp == want && n++ < 12) {
            int vv = v3k_vert(v), ff = vv ? v3k_frag(c, f, gpu_target_raw16()) : -9;
            printf("[trazafgxp] vgxp 0x%08X v3k_vert %d v3k_frag %d vbad %d fbad %d nv %u ninst %u ptype %u\n", v->gxp, vv, ff,
                   v->v3k, f->v3k_bad, nvb, ninst, ptype);
            fflush(stdout);
        }
    }
    if (v3k_enabled() && v3k_vert(v)) {
        int raw = gpu_target_raw16();
        int fv = v3k_frag(c, f, raw);
        if (fv >= 0) {
            extern double g_prof_v3k;
            LARGE_INTEGER pf, p0, p1; QueryPerformanceFrequency(&pf); QueryPerformanceCounter(&p0);
            /* Salida entera/empaquetada (U32..S8) en una superficie F16x4: desde la escena
             * siguiente se dibuja como U16x4 con los bits crudos (atlas del público). */
            uint8_t ot = f->v3kv[fv].info->output_type;
            if (!raw && ot >= 3 && ot <= 8 && gpu_target_f16()) gpu_target_want_raw16();
            draw_v3k(c, v, f, fv, streams, ptype, icount, nvb, ninst, maxsrc);
            QueryPerformanceCounter(&p1); g_prof_v3k += (double)(p1.QuadPart - p0.QuadPart) / (double)pf.QuadPart;
            return;
        }
        if (raw) return;   /* el camino genérico no sabe escribir U16x4 */
    }
    if (!f->ufp[0] && prog_translate(v->gxp, &v->real, &v->mod, &v->ui) && prog_translate(f->gxp, &f->real, &f->mod, &f->ui)) {
        draw_real(c, v, f, streams, ptype, icount, nvb, ninst, maxsrc);
        return;
    }
    {   /* MLB15_SOLO_REAL=1: solo lo traducido (para ver qué dibuja) */
        static int only = -1;
        if (only < 0) only = env_cached("MLB15_SOLO_REAL") != NULL;
        if (only && c->tw >= 480) return;
    }
    int have_bidx = 0, have_bw = 0, have_nrm = 0, inst_seen = 0;
    memset(g_inst, 0, sizeof(g_inst));
    for (uint32_t i = 0; i < nv; i++) {
        GpuVertex *o = &g_verts[i];
        o->pos[0] = o->pos[1] = o->pos[2] = 0.0f; o->pos[3] = 1.0f;
        o->uv0[0] = o->uv0[1] = o->uv1[0] = o->uv1[1] = 0.0f;
        o->col[0] = o->col[1] = o->col[2] = o->col[3] = 1.0f;
        g_nrm[i][0] = g_nrm[i][1] = 0.0f; g_nrm[i][2] = 1.0f;
    }
    for (int k = 0; k < v->nattr; k++) {
        const Attr *a = &v->attr[k];
        if (a->slot == SLOT_NONE || a->stream >= 16 || !streams[a->stream]) continue;
        uint32_t stride = v->stride[a->stream];
        int per_inst = (v->index_source[a->stream] & 2) != 0;
        int comps = a->comps > 4 ? 4 : a->comps;
        uint32_t nsrc = per_inst ? ninst : maxsrc + 1;
        const uint8_t *base = (const uint8_t *)vita_mem_ptr(streams[a->stream] + a->offset,
                                                            (nsrc - 1) * stride + comp_size(a->format) * (uint32_t)comps);
        if (!base) continue;
        for (uint32_t i = 0; i < nv; i++) {
            uint32_t src = per_inst ? i / nvb : g_vsrc[i % nvb];
            const uint8_t *at = base + src * stride;
            float val[4] = { 0, 0, 0, 1 };
            for (int ci = 0; ci < comps; ci++) val[ci] = read_comp(at, a->format, ci);
            GpuVertex *o = &g_verts[i];
            switch (a->slot) {
                case SLOT_POS: memcpy(o->pos, val, 16); if (comps < 4) o->pos[3] = 1.0f; break;
                case SLOT_UV0: o->uv0[0] = val[0]; o->uv0[1] = val[1]; break;
                case SLOT_UV1: o->uv1[0] = val[0]; o->uv1[1] = val[1]; break;
                case SLOT_COL: memcpy(o->col, val, 16); if (comps < 4) o->col[3] = 1.0f; break;
                case SLOT_BIDX: for (int q = 0; q < 4; q++) g_bidx[i][q] = val[q]; have_bidx = 1; break;
                case SLOT_BW:   for (int q = 0; q < 4; q++) g_bw[i][q] = q < comps ? val[q] : 0.0f; have_bw = 1; break;
                case SLOT_NRM:  g_nrm[i][0] = val[0]; g_nrm[i][1] = val[1]; g_nrm[i][2] = val[2]; have_nrm = 1; break;
                case SLOT_INST0: case SLOT_INST0 + 1: case SLOT_INST0 + 2: case SLOT_INST3: {
                    /* filas de la matriz de instancia + traslación (InstanceAttributes) */
                    int j = a->slot == SLOT_INST3 ? 3 : a->slot - SLOT_INST0;
                    uint32_t q = per_inst ? (nvb ? i / nvb : 0) : 0;
                    if (q < 64) {
                        if (j == 0 && q == 0) inst_seen = 1;    /* solo fila 0 marca presencia */
                        for (int cc = 0; cc < 4; cc++) g_inst[q][j * 4 + cc] = val[cc];
                    }
                    break;
                }
            }
        }
    }
    /* Capas de tierra del montículo/cajón de bateo: el shader real mezcla su máscara
     * con la tierra de debajo según Color; el genérico la pintaba como un recuadro
     * blanco (o cian con Color). Sin ellas se ve la tierra base. MLB15_MONTICULO=1 las dibuja. */
    {
        static int show = -1;
        if (show < 0) show = env_cached("MLB15_MONTICULO") ? 1 : 0;
        if (v->mound_pos) {
            float mp[4], mq[4];
            int a = read_vec(c, v->mound_pos, 0, mp), b = read_vec(c, v->mound_par, 0, mq);
            if (a) { memcpy(g_mound, mp, sizeof(g_mound)); g_mound_ok = 1; }
            if (env_cached("MLB15_SOMBRAS_TRAZA")) {
                static int shown;
                if (shown++ < 10) printf("[monticulo] pos%c(%.3f %.3f %.3f %.3f) par%c(%.3f %.3f %.3f %.3f)\n", a ? '+' : '-',
                                         mp[0], mp[1], mp[2], mp[3], b ? '+' : '-', mq[0], mq[1], mq[2], mq[3]);
            }
        }
        if (v->color_is_data && !show) { g_skipped++, g_skip_why[0]++, g_skip_color++; return; }
    }
    /* Huesos primero (como el shader real: Index/Weight → UVP_SkinningMatrix), luego la
     * matriz de instancia y por último el MVP del draw. Rígidos sin Weight: hueso 0 entero. */
    int skinned = 0;
    if (v->skin && have_bidx) {
        if (!have_bw) for (uint32_t i = 0; i < nv; i++) { g_bw[i][0] = 1.0f; g_bw[i][1] = g_bw[i][2] = g_bw[i][3] = 0.0f; }
        skinned = skin_vertices(c, v, nv, comps_bidx(v));
    }
    /* Matriz de instancia (InstanceMatrix3x4[0..2]): 3 filas con la traslación en .w
     * (shader: VDP fila.xyzw · pos.xyz1). InstanceAttributes son parámetros, no posición.
     * Los streams de instancia son index_source 2/3. MLB15_INST_MTX=0 la quita. */
    {
        static int inst_mtx = -1;
        if (inst_mtx < 0) { const char *e = env_cached("MLB15_INST_MTX"); inst_mtx = e ? atoi(e) : 1; }
        if (inst_seen && inst_mtx) {
        extern int port_en_partido;
        static int shown_inst;
        static int last_flag;
        if (!port_en_partido) last_flag = 0;
        else if (!last_flag) { last_flag = 1; shown_inst = 0; }
        if (tr() && nvb && shown_inst++ < 12) {
            /* bytes crudos de la instancia 0 del stream de la matriz */
            uint32_t streamraw = 0;
            for (int k = 0; k < v->nattr; k++)
                if (v->attr[k].slot == SLOT_INST0) { streamraw = streams[v->attr[k].stream & 15]; break; }
            printf("[inst] vgxp=0x%08X nvb=%u m0=(%.3f %.3f %.3f %.3f) m1=(%.3f %.3f %.3f %.3f) m2=(%.3f %.3f %.3f %.3f)\n",
                   v->gxp, nvb, g_inst[0][0], g_inst[0][1], g_inst[0][2], g_inst[0][3],
                   g_inst[0][4], g_inst[0][5], g_inst[0][6], g_inst[0][7],
                   g_inst[0][8], g_inst[0][9], g_inst[0][10], g_inst[0][11]);
            printf("[inst]   inst3=(%.3f %.3f %.3f %.3f) raw:", g_inst[0][12], g_inst[0][13], g_inst[0][14], g_inst[0][15]);
            if (streamraw)
                for (int i = 0; i < 64; i++) printf(" %02X", vita_read8(streamraw + i));
            printf("\n");
            float *p0 = g_verts[0].pos;
            printf("[inst]   p0 antes=(%.3f %.3f %.3f)", p0[0], p0[1], p0[2]);
        }
        for (uint32_t i = 0; i < nv; i++) {
            uint32_t q = nvb ? i / nvb : 0;
            if (q >= 64) q = 0;                     /* demasiadas instancias: la 0 */
            float *pos = g_verts[i].pos, *n = g_nrm[i];
            const float *r = g_inst[q];
            if (!r[0] && !r[1] && !r[2] && !r[4] && !r[5] && !r[6] && !r[8] && !r[9] && !r[10]) continue;
            float x = pos[0], y = pos[1], z = pos[2];
            float o0 = r[0] * x + r[1] * y + r[2] * z + r[3];
            float o1 = r[4] * x + r[5] * y + r[6] * z + r[7];
            float o2 = r[8] * x + r[9] * y + r[10] * z + r[11];
            pos[0] = o0; pos[1] = o1; pos[2] = o2; pos[3] = 1.0f;
            float n0 = r[0] * n[0] + r[1] * n[1] + r[2] * n[2];
            float n1 = r[4] * n[0] + r[5] * n[1] + r[6] * n[2];
            n[2] = r[8] * n[0] + r[9] * n[1] + r[10] * n[2];
            n[0] = n0; n[1] = n1;
        }
        if (tr() && nvb && shown_inst <= 12)
            printf(" despues=(%.3f %.3f %.3f)\n", g_verts[0].pos[0], g_verts[0].pos[1], g_verts[0].pos[2]);
        }
    }
    if (inst_seen && !v->skin) skinned = 2;         /* instancia sin huesos: ya en mundo */
    if (v->skin && !skinned) {
        g_skipped++, g_skip_why[0]++, g_skip_skin++;
        static int shown0;
        if (tr() && shown0++ < 20) {
            printf("[skin] descartado: vgxp=0x%08X have_bidx=%d have_bw=%d nattr=%d\n", v->gxp, have_bidx, have_bw, v->nattr);
            {   /* todos los uniformes del GXP: nombre(contenedor, vec4s del recurso) */
                uint32_t n = gxp_nparams(v->gxp), base = gxp_params(v->gxp);
                printf("[skin]   uniformes:");
                for (uint32_t i = 0; i < n && i < 64; i++) {
                    uint32_t p = base + 16 * i;
                    if (p_cat(p) == 0) continue;
                    char nm[48] = "?";
                    p_name_str(p, nm, sizeof(nm));
                    printf(" %s(k%u a%u r%u)", nm, p_cont(p), vita_read16(p + 4), p_res(p));
                }
                printf("\n");
            }
            for (int k = 0; k < v->nattr; k++) {
                char nm[48] = "?";
                {   /* nombre del parámetro que usa este registro */
                    uint32_t n = gxp_nparams(v->gxp), base = gxp_params(v->gxp);
                    for (uint32_t i = 0; i < n && i < 256; i++) {
                        uint32_t p = base + 16 * i;
                        if (p_cat(p) != 0 || p_res(p) != v->attr[k].reg) continue;
                        p_name_str(p, nm, sizeof(nm));
                        break;
                    }
                }
                uint32_t s = v->attr[k].stream & 15;
                printf("[skin]   attr '%s': s%u off%u fmt%u c%u reg=%u->slot%d stream=0x%08X stride=%u idxsrc=%u\n",
                       nm, v->attr[k].stream, v->attr[k].offset, v->attr[k].format, v->attr[k].comps, v->attr[k].reg,
                       v->attr[k].slot, streams[s], v->stride[s], v->index_source[s]);
            }
            if (nvb) {
                printf("[skin]    v0 idx(%.0f %.0f) col(%g %g %g %g)", g_bidx[0][0], g_bidx[0][1],
                       g_bidx[0][2], g_verts[0].col[0], g_verts[0].col[1], g_verts[0].col[2], g_verts[0].col[3]);
                if (nvb > 1)
                    printf(" v1 idx(%.0f %.0f) col(%g %g %g %g)", g_bidx[1][0], g_bidx[1][1],
                           g_bidx[1][2], g_verts[1].col[0], g_verts[1].col[1], g_verts[1].col[2], g_verts[1].col[3]);
                printf("\n");
            }
        }
        return;   /* sin huesos: basura en pantalla */
    }

    uint32_t nt = build_tris(ptype, icount, nvb, ninst);

    GpuDraw d;
    memset(&d, 0, sizeof(d));
    float m[16], vpm[16], model[16];
    int with_model = 0;
    if (skinned && read_matrix(c, v->vpm, vpm)) to_glsl(vpm, d.mvp);   /* el skinning ya llevó a mundo */
    else if (read_matrix(c, v->mvp, m)) to_glsl(m, d.mvp);
    else if (read_matrix(c, v->vpm, vpm) && read_matrix(c, v->model, model)) {
        /* Vector fila (como el skinning): clip = v·Model·VP → Model*VP. Antes VP*Model
         * dejaba el casco, el bate y las gorras fuera de sitio. MLB15_VPM_ORDEN=0: el viejo. */
        static int orden = -1;
        if (orden < 0) { const char *e = env_cached("MLB15_VPM_ORDEN"); orden = e ? atoi(e) : 1; }
        if (orden) mat_mul(model, vpm, m); else mat_mul(vpm, model, m);
        to_glsl(m, d.mvp); with_model = 1;
    }
    else { for (int i = 0; i < 16; i++) d.mvp[i] = (i % 5) ? 0.0f : 1.0f; }
    {
        extern int port_en_partido;
        static int shown_mvp;
        if (inst_seen && port_en_partido && tr() && nvb && shown_mvp++ < 10)
            printf("[inst] mvp=%d%d [%.2g %.2g %.2g %.2g | %.2g %.2g %.2g %.2g | %.2g %.2g %.2g %.2g | %.2g %.2g %.2g %.2g] vub=%08X %08X %08X %08X\n",
                   read_matrix(c, v->vpm, vpm) ? 1 : 0, read_matrix(c, v->mvp, m) ? 1 : 0,
                   d.mvp[0], d.mvp[1], d.mvp[2], d.mvp[3], d.mvp[4], d.mvp[5], d.mvp[6], d.mvp[7],
                   d.mvp[8], d.mvp[9], d.mvp[10], d.mvp[11], d.mvp[12], d.mvp[13], d.mvp[14], d.mvp[15],
                   c->vub[0], c->vub[1], c->vub[2], c->vub[3]);
    }
    draw_state(c, f, &d);
    d.use_color = v->has_color && !f->no_vcolor && !v->color_is_data;
    if (have_nrm) light_vertices(c, v, f, nv, with_model ? model : NULL, &d.use_color);
    {   /* MLB15_INST_TINTE=1: draws con matriz de instancia (bate, casco...) en magenta */
        static int tint = -1;
        if (tint < 0) tint = env_cached("MLB15_INST_TINTE") != NULL;
        if (tint && inst_seen) {
            for (uint32_t i = 0; i < nv; i++) { g_verts[i].col[0] = 1; g_verts[i].col[1] = 0; g_verts[i].col[2] = 1; g_verts[i].col[3] = 1; }
            d.use_color = 1;
        }
    }
    if (f->konst) {
        uint32_t cont = p_cont(f->konst);
        uint32_t buf = cont == 14 ? (c->pfs ? vita_read32(c->pfs + 4) : c->fdef)
                                  : (c->pfs ? vita_read32(c->pfs + 8 + 4 * cont) : c->fub[cont]);
        if (buf && vita_mem_ptr(buf + p_res(f->konst) * 4u, 16)) {
            for (int i = 0; i < 4; i++) { vfp_reg x; x.u = vita_read32(buf + p_res(f->konst) * 4u + 4u * (uint32_t)i); d.const_color[i] = x.f; }
            d.use_const = 1;
        }
    }
    if (f->sampler >= 0 && f->sampler < 16) {
        const uint8_t *tw = c->pfs ? (const uint8_t *)vita_mem_ptr(c->pfs + 72 + 16u * (uint32_t)f->sampler, 16)
                                   : c->ftex[f->sampler];
        if (tw) {
            tex_from_words(tw, &d.tex);
            d.use_tex = d.tex.data && d.tex.width && d.tex.height;
        }
    }

    if (tr()) {
        static int shown;
        if (shown++ < 6) {
            const float *p = g_verts[g_tri[0]].pos;
            float clip[4];
            for (int r = 0; r < 4; r++) clip[r] = d.mvp[0 * 4 + r] * p[0] + d.mvp[1 * 4 + r] * p[1] + d.mvp[2 * 4 + r] * p[2] + d.mvp[3 * 4 + r] * p[3];
            printf("[gxm] draw: %u idx %u verts, pos0 (%.2f %.2f %.2f %.2f) -> clip (%.2f %.2f %.2f %.2f) tex=%s %ux%u fmt=0x%08X\n",
                   nt, nv, p[0], p[1], p[2], p[3], clip[0], clip[1], clip[2], clip[3],
                   d.use_tex ? "si" : "no", d.tex.width, d.tex.height, d.tex.format);
            fflush(stdout);
        }
    }
    /* MLB15_GXM_ESCENA=n: lista todos los draws de la escena n. */
    {
        long dump = g_dump_scene;
        if (scene_dump()) {
            const GpuVertex *a = &g_verts[g_tri[0]];
            float u0 = 1e9f, u1 = -1e9f, w0 = 1e9f, w1 = -1e9f;
            for (uint32_t i = 0; i < nt; i++) {
                const GpuVertex *x = &g_verts[g_tri[i]];
                if (x->uv0[0] < u0) u0 = x->uv0[0];
                if (x->uv0[0] > u1) u1 = x->uv0[0];
                if (x->uv0[1] < w0) w0 = x->uv0[1];
                if (x->uv0[1] > w1) w1 = x->uv0[1];
            }
            {   /* coeficientes de mezcla programable (Sprite: UFP_SRC_xx y UFP_DST_xx en f16x4) */
                static const char *const nm[7] = { "UFP_SRC_Ck", "UFP_SRC_CSa", "UFP_SRC_CDa", "UFP_SRC_CDc",
                                                   "UFP_DST_Ck", "UFP_DST_CSa", "UFP_DST_CSc" };
                printf("[mezcla]");
                for (int k = 0; k < 7; k++) {
                    uint32_t p = gxp_find(f->gxp, nm[k]);
                    if (!p) continue;
                    uint32_t cont = p_cont(p);
                    uint32_t buf = cont == 14 ? c->fdef : (c->pfs ? vita_read32(c->pfs + 8 + 4 * cont) : c->fub[cont]);
                    uint32_t at = buf + p_res(p) * 4u;
                    printf(" %s=(%.2f %.2f %.2f %.2f)", nm[k] + 4, half_to_float(vita_read16(at)), half_to_float(vita_read16(at + 2)),
                           half_to_float(vita_read16(at + 4)), half_to_float(vita_read16(at + 6)));
                }
                printf("\n");
            }
            if (nv > 400) {
                printf("[uvs]");
                for (uint32_t k = 0; k < 12; k++) {
                    const GpuVertex *x = &g_verts[g_tri[k]];
                    printf(" #%u(%.1f,%.1f uv %.4f,%.4f c%.2f/%.2f)", g_tri[k], x->pos[0], x->pos[1],
                           x->uv0[0], x->uv0[1], x->col[0], x->col[3]);
                }
                printf("\n");
            }
            if (nv <= 6) {
                printf("[verts]");
                for (uint32_t i = 0; i < nv; i++) {
                    const float *p = g_verts[i].pos;
                    float cl[4];
                    for (int r = 0; r < 4; r++) cl[r] = d.mvp[r] * p[0] + d.mvp[4 + r] * p[1] + d.mvp[8 + r] * p[2] + d.mvp[12 + r] * p[3];
                    printf(" (%.1f %.1f)->(%.3f %.3f w%.2f)", p[0], p[1], cl[0], cl[1], cl[3]);
                }
                printf(" | vp %.1f %.1f %.1f %.1f set=%d en=%d\n", c->vp[0], c->vp[1], c->vp[2], c->vp[3], c->vp_set, c->vp_enable);
            }
            printf("[ds] zfunc=%u zw=%u st=%u f(%u %u %u %u m%02X/%02X r%02X) coloroff=%u cull=%u blend=%u(%u,%u)\n",
                   d.depth_func, d.depth_write, d.stencil, d.st[0].func, d.st[0].fail, d.st[0].zfail, d.st[0].pass,
                   d.st[0].cmask, d.st[0].wmask, d.st[0].ref, d.color_off, d.cull, d.blend, d.color_src, d.color_dst);
            printf("[escena %ld] vgxp=0x%08X fgxp=0x%08X tri=%u v=%u tex=0x%08X %ux%u fmt=0x%08X col=%d blend=%u "
                   "uv=[%.3f..%.3f]x[%.3f..%.3f] pos0=(%.2f %.2f %.2f) col0=(%.2f %.2f %.2f %.2f)\n",
                   dump, v->gxp, f->gxp, nt / 3, nv, d.use_tex ? d.tex.data : 0, d.tex.width, d.tex.height,
                   d.tex.format, d.use_color, d.blend, u0, u1, w0, w1,
                   a->pos[0], a->pos[1], a->pos[2], a->col[0], a->col[1], a->col[2], a->col[3]);
        }
    }
    gpu_draw(&d, g_verts, nv, g_tri, nt);
    if (skinned) player_shadow(c, v, f, &d, nv, nt);
}

/* sceGxmDraw(ctx, primType, indexType, indexData, indexCount) */
void vita_hle_sceGxmDraw(void) {
    Ctx *c = ctx_find(r0);
    if (tr() && !r3) {
        static int shown;
        if (shown++ < 4) printf("[gxm] sceGxmDraw sin índices: prim=0x%08X itype=0x%08X n=%u lr=0x%08X\n", r1, r2, arg(0), lr);
    }
    if (c) do_draw(c, c->vprog, c->streams, r1, r2, r3, arg(0), 0);
    r0 = GXM_OK;
}
/* sceGxmDrawInstanced(ctx, primType, indexType, indexData, indexCount, indexWrap) */
void vita_hle_sceGxmDrawInstanced(void) {
    Ctx *c = ctx_find(r0);
    if (c) do_draw(c, c->vprog, c->streams, r1, r2, r3, arg(0), arg(1));
    r0 = GXM_OK;
}
void vita_hle_sceGxmDrawPrecomputed(void) {
    Ctx *c = ctx_find(r0);
    uint32_t pd = pc_data(r1);
    if (c && pd && vita_mem_ptr(pd, 0x60)) {
        uint32_t streams[16];
        for (uint32_t i = 0; i < 16; i++) streams[i] = vita_read32(pd + 24 + 4 * i);
        do_draw(c, vita_read32(pd), streams, vita_read32(pd + 4), vita_read32(pd + 8),
                vita_read32(pd + 12), vita_read32(pd + 16), vita_read32(pd + 20));
    }
    r0 = GXM_OK;
}

/* ========================================================================== */
/* Cola de presentación                                                       */
/* ========================================================================== */

void port_display_present_guest(uint32_t guest_struct);

void vita_hle_sceGxmDisplayQueueAddEntry(void) {
    /* r2 = callbackData. En MLB 15 es un SceDisplayFrameBuf y su callback
     * (sub_810323A4) hace sceDisplaySetFrameBuf(data) y suma 1 a 0x81F15BB8. */
    if (r2) port_display_present_guest(r2);
    uint32_t counter = 0x81F15BB8u;
    if (vita_mem_ptr(counter, 4)) vita_write32(counter, vita_read32(counter) + 1u);
    r0 = GXM_OK;
}
void vita_hle_sceGxmDisplayQueueFinish(void) { r0 = GXM_OK; }


static void scene_dump_end(void) {
    if (!g_dump_retry || (long)g_scenes != g_dump_scene) return;
    g_dump_retry = 0;
    if (g_draws == g_dump_draws0) { g_dump_secs = 0; printf("[escena] sin draws, pruebo la siguiente\n"); return; }
    static long cada = -1;   /* MLB15_GXM_ESCENA_CADA=k (con "Nf"): otra escena cada k fotogramas */
    if (cada < 0) { const char *e = env_cached("MLB15_GXM_ESCENA_CADA"); cada = e ? atol(e) : 0; }
    extern unsigned port_display_frames(void);
    if (cada > 0) g_dump_frame = ((long)port_display_frames() / cada + 1) * cada;
}
