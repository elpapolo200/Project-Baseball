/* Interfaz entre el HLE de GXM (gxm.c) y el renderizador (gpu_vk.c).
 *
 * gxm.c ya resuelve el estado GXM a algo plano: vértices convertidos a un
 * formato fijo, matriz MVP, viewport, mezcla y la textura 0. El renderizador no
 * sabe nada de GXP ni de registros de la Vita.
 *
 * Modelo de memoria: cada escena se dibuja en una imagen y al terminar se copia
 * a la superficie de color en memoria guest, como hace la GPU real con su VRAM.
 * Así lo que lee la CPU, lo que se presenta y el render-a-textura salen de la
 * misma memoria.
 */
#ifndef PORT_GPU_H
#define PORT_GPU_H

#include <stddef.h>
#include <stdint.h>

/* Vértice canónico: lo que necesitan los shaders del menú. */
typedef struct {
    float pos[4];
    float uv0[2];
    float uv1[2];
    float col[4];
} GpuVertex;

enum { GPU_TEX_SWIZZLED = 0, GPU_TEX_LINEAR = 1, GPU_TEX_LINEAR_STRIDED = 2, GPU_TEX_TILED = 3, GPU_TEX_CUBE = 4 };

typedef struct {
    uint32_t data;        /* dirección guest */
    uint32_t format;      /* SceGxmTextureFormat */
    uint16_t width, height;
    uint32_t stride;      /* bytes por fila (lineal con stride) */
    uint32_t palette;     /* dirección guest de la paleta (P4/P8) */
    uint8_t  type;        /* GPU_TEX_* */
    uint8_t  mips;
    uint8_t  min_linear, mag_linear;
    uint8_t  u_addr, v_addr;   /* SceGxmTextureAddrMode: 0 repeat, 1 mirror, 2 clamp... */
    uint8_t  gamma;            /* sRGB: se muestrea convertida a lineal */
} GpuTexture;

typedef struct {
    float    mvp[16];        /* columnas (orden GLSL) */
    float    vp[4];          /* xOffset, xScale, yOffset, yScale */
    float    vz[2];          /* zOffset, zScale */
    int      vp_enable;
    float    dbias_factor, dbias_units;   /* sesgo de profundidad (pendiente, constante) */
    int      use_tex, use_color;
    int      use_const;       /* color constante del shader (p. ej. ClearColor) */
    float    const_color[4];
    /* mezcla: SceGxmBlendInfo ya separado */
    uint8_t  blend;          /* 0 = sin mezcla */
    uint8_t  color_func, alpha_func;
    uint8_t  color_src, color_dst, alpha_src, alpha_dst;
    uint8_t  color_mask;     /* bit 0 A, 1 R, 2 G, 3 B (SceGxmColorMask) */
    uint8_t  cull;           /* 0 nada, 1 CW, 2 CCW */
    /* profundidad y stencil (los enums de GXM ya en el orden de Vulkan) */
    uint8_t  depth_func;     /* VkCompareOp */
    uint8_t  depth_write;
    uint8_t  stencil;        /* 1 = prueba de stencil activa */
    struct { uint8_t func, fail, zfail, pass, cmask, wmask, ref; } st[2];   /* 0 delante, 1 detrás */
    uint8_t  color_off;      /* programa de fragmento desactivado: solo profundidad/stencil */
    GpuTexture tex;
} GpuDraw;

/* Devuelve 0 si no hay Vulkan (el port sigue sin dibujar). */
int  gpu_init(void);
int  gpu_ready(void);

/* Escena sobre una superficie de color guest (U8U8U8U8, `stride` en píxeles). */
/* `load_ds`: conservar profundidad/stencil de la escena anterior (ForceLoad de
 * la superficie de profundidad); si no, se empieza en profundidad 1 y stencil 0. */
/* `gxm_format`: SceGxmColorFormat de la superficie (U8, F16F16F16F16... tienen su formato Vulkan). */
/* `gxm_format` lleva en el bit 0 la conversión sRGB de la superficie (los formatos GXM
 * no usan ese bit). `surf_type`: SceGxmColorSurfaceType (0 lineal; swizzled/tiled se quedan en la GPU).
 * `rw`, `rh`: tamaño del render target (lo que escribe la escena; 0 = toda). */
void gpu_scene_depth(uint32_t depth_addr);   /* dirección de la profundidad de la próxima escena (0 = ninguna) */
void gpu_scene_begin(uint32_t color_addr, uint32_t width, uint32_t height, uint32_t stride, int load_ds, uint32_t gxm_format,
                     uint32_t surf_type, uint32_t rw, uint32_t rh);
void gpu_draw(const GpuDraw *d, const GpuVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni);
void gpu_scene_end(void);

/* Shaders traducidos de los GXP (usse.c): crea un módulo SPIR-V (0 = error) y
 * dibuja con un par de módulos. `verts` son nattr vec4 por vértice (atributos
 * en el orden de UsseInfo.attr); `mem` es la memoria de uniformes del draw
 * (imágenes de sa en las palabras vsa y fsa, buffers después); tex[u] es la
 * textura de la unidad u o NULL. */
int  gpu_module(const uint32_t *spv, size_t words);
void gpu_draw_prog(const GpuDraw *d, int vsm, int fsm, uint32_t nattr, const float *verts, uint32_t nv,
                   const uint32_t *idx, uint32_t ni, const uint32_t *mem, uint32_t memwords, uint32_t vsa, uint32_t fsa,
                   const GpuTexture *const tex[16]);

/* Shaders de Vita3K (v3k_api.h): `verts` son nloc vec4 por vértice (uvec4 en las
 * locations de uint_mask); vinfo/finfo son los bloques RenderVert/FragUniformBlock,
 * vdata/fdata los SSBO de uniformes de cada etapa; viewport = x, y, ancho, alto. */
/* Superficie actual F16x4 / escrita como U16x4 crudo (atlas del público): ver gpu_vk.c. */
uint32_t gpu_res_scale(void);   /* resolución interna (1 = nativa) */
int gpu_target_raw16(void);
int gpu_target_f16(void);
void gpu_target_want_raw16(void);

void gpu_draw_v3k(const GpuDraw *d, int vsm, int fsm, uint32_t nloc, uint32_t uint_mask, const float *verts, uint32_t nv,
                  const uint32_t *idx, uint32_t ni, const void *vinfo, uint32_t vinfo_bytes, const void *finfo,
                  uint32_t finfo_bytes, const void *vdata, uint32_t vbytes_data, const void *fdata, uint32_t fbytes_data,
                  const float viewport[4], const GpuTexture *const tex[16]);

/* Vértices «crudos» (PLAN_RENDIMIENTO.md fase 4): en vez de convertir cada vértice a vec4 float
 * en la CPU, se copian los bytes del juego tal cual (solo los vértices que usa el dibujo) y la
 * GPU los convierte al leerlos con el formato de cada atributo. Binding nbind = valor por
 * defecto (0,0,0,1) para las locations que ningún atributo escribe. */
#define GPU_VLAY_MAXB 8
typedef struct {
    uint8_t  nloc, nbind;
    uint8_t  loc_bind[16];          /* 0xFF = valor por defecto */
    uint8_t  loc_fmt[16];           /* formato GXM << 4 | componentes */
    uint16_t loc_off[16];
    uint16_t stride[GPU_VLAY_MAXB];
    uint8_t  inst[GPU_VLAY_MAXB];   /* 1 = por instancia */
} GpuVLayout;
int gpu_vfmt_ok(uint8_t gxm_fmt, uint8_t comps);   /* la tarjeta lee ese formato directamente */
void gpu_draw_v3k_crudo(const GpuDraw *d, int vsm, int fsm, uint32_t nloc, const GpuVLayout *lay,
                        const uint8_t *const bdata[GPU_VLAY_MAXB], const uint32_t bbytes[GPU_VLAY_MAXB], uint32_t ninst,
                        uint32_t nv, const uint32_t *idx, uint32_t ni, const void *vinfo, uint32_t vinfo_bytes,
                        const void *finfo, uint32_t finfo_bytes, const void *vdata, uint32_t vbytes_data,
                        const void *fdata, uint32_t fbytes_data, const float viewport[4], const GpuTexture *const tex[16]);

/* Ventana SDL con swapchain Vulkan (no se abre con MLB15_NOWINDOW=1).
 * gpu_present copia a la ventana la superficie que el juego presenta: la
 * imagen que ya dibujó Vulkan, o su memoria guest si no la dibujó la GPU. */
int  gpu_has_window(void);
void gpu_present(uint32_t addr, uint32_t width, uint32_t height, uint32_t stride);

/* Estadísticas para la traza. */
void gpu_stats(uint32_t *scenes, uint32_t *draws, uint32_t *textures);

/* Decodifica una textura GXM a RGBA8 (texdec.c). 0 si el formato no está. */
int      tex_decode(const GpuTexture *t, uint32_t *out_rgba);
int      tex_decode_f16(const GpuTexture *t, uint16_t *out_rgba16f);   /* F16F16F16F16 */
uint32_t tex_data_size(const GpuTexture *t);

#endif /* PORT_GPU_H */
