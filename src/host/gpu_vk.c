/* Renderizador Vulkan del port (ver gpu.h).
 *
 * Sin SDK: se carga vulkan-1.dll (la instala el driver) y se resuelven las
 * funciones a mano. Una escena GXM = una pasada de render sobre una imagen por
 * superficie de color; al empezar se sube lo que hay en memoria guest y al
 * terminar se vuelve a copiar (la GPU de la Vita escribe en su VRAM, que es
 * memoria normal para la CPU). Todo síncrono y bajo un mutex: lo justo para la
 * primera imagen, sin optimizar.
 *
 *   MLB15_GPU=0   desactiva el renderizador (como antes: solo se cuentan draws)
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vitarecomp/mem.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_vulkan.h>

#include "gpu.h"
#include "input.h"
#include "launcher.h"
#include "ui_frag.h"
#include "ui_vert.h"
#include "pp_vert.h"
#include "pp_fxaa_frag.h"
#include "pp_escala_frag.h"
#include "texrep.h"

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


/* --- funciones de Vulkan --------------------------------------------------- */

#define VK_GLOBAL(X) X(vkCreateInstance)
#define VK_INSTANCE(X) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define VK_DEVICE(X) \
    X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) \
    X(vkWaitForFences) X(vkResetFences) X(vkResetCommandBuffer) X(vkCreateBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkBindBufferMemory) X(vkMapMemory) \
    X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) \
    X(vkCreateSampler) X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkCreateShaderModule) \
    X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkResetDescriptorPool) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) \
    X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDrawIndexed) \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdSetDepthBias) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) X(vkCreateSemaphore) X(vkCmdBlitImage) X(vkDeviceWaitIdle) X(vkCmdDraw) \
    X(vkDestroyImage) X(vkDestroyImageView) X(vkFreeMemory) X(vkDestroyFramebuffer)

#define DECL(f) static PFN_##f f;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
VK_GLOBAL(DECL)
VK_INSTANCE(DECL)
VK_DEVICE(DECL)

/* --- estado ---------------------------------------------------------------- */

#define VB_SIZE   (32u << 20)
#define IB_SIZE   (8u << 20)
#define MAX_TARGETS  64
#define MAX_TEXTURES 4096
#define MAX_PIPES    8192   /* con los shaders traducidos hay cientos de programas por estado */
#define PIPE_HASH    16384
#define MAX_SAMPLERS 32

typedef struct { VkBuffer buf; VkDeviceMemory mem; void *ptr; VkDeviceSize size; } Buffer;

struct Texture_;
typedef struct {
    uint32_t addr, w, h;
    uint32_t sw, sh;                                          /* tamaño real de la imagen (w,h x resolución interna) */
    VkImage simg; VkDeviceMemory smem; VkImageView sview;     /* copia a tamaño nativo para subir/bajar (o = img) */
    VkImage img; VkDeviceMemory mem; VkImageView view; VkFramebuffer fb;
    VkImage dimg; VkDeviceMemory dmem; VkImageView dview;      /* profundidad + stencil */
    uint32_t dsaddr;                                          /* depthData GXM de su última escena (0 = sin) */
    uint8_t ds_lectura;                                       /* profundidad en SHADER_READ_ONLY (muestreable) */
    struct Texture_ *dtx;                                     /* la profundidad como textura (mapa de sombras) */
    int ds_ready;                                             /* ya inicializada (se puede cargar) */
    uint8_t tf;                                               /* índice en g_tfmt_vk */
    uint8_t raw16, want_raw16;                                /* F16x4 dibujada como U16x4 (bits crudos) */
    uint32_t ndraws;                                          /* draws recibidos desde que existe */
    VkImageView uview; VkFramebuffer fb_raw;
    VkImageView pview;                                        /* vista UNORM de una superficie sRGB (pase final) */
    uint8_t drawn;                                            /* la GPU ya dibujó aquí (imagen muestreable) */
    uint32_t stamp;                                           /* nº de la última escena dibujada aquí */
    uint8_t resident;                                         /* swizzled/tiled: no se copia a memoria guest */
    uint32_t ghash, gbytes;                                   /* hash de la memoria guest al terminar la escena */
    uint32_t hscene;                                          /* escena en la que se comprobó ghash */
    uint32_t rw, rh;                                          /* zona que escribe la escena (render target) */
    int ntx;                                                  /* vistas como textura (por swizzle GXM) */
    uint32_t txkey[4];
    struct Texture_ *tx[4];
    Buffer staging;
    int pend;                                                 /* escena en vuelo (contexto+1) con su copia de vuelta pendiente */
    uint32_t gstride;                                         /* stride guest de su última escena */
} Target;

/* Escenas en vuelo (MLB15_ESCENAS_VUELO, 4 por defecto, máx. NCTX; 1 = esperar a la GPU en cada escena
 * como antes). Cada una tiene su cmd buffer, valla, vértices/índices/uniformes y descriptores:
 * gpu_scene_end envía y vuelve sin esperar; la copia de la superficie a la memoria guest se hace
 * al completar la escena (ctx_complete): al volver a necesitar el contexto o la superficie,
 * en sceGxmFinish y en cada fotograma (gpu_sync_all). */
#define NCTX 8
#define UP_SIZE (8u << 20)   /* subidas de texturas por escena en vuelo */
typedef struct {
    VkCommandBuffer cmd; VkFence fence;
    Buffer vb, ib, mb;
    VkDescriptorPool dp2, dp3;
    int busy;
    Target *tgt;
    VkCommandBuffer ucmd; Buffer up; uint32_t up_off; int u_open;   /* subidas de texturas de la escena */
    VkDescriptorSet dset0;                                          /* uniformes v3k con offsets dinámicos */
} GCtx;

typedef struct Texture_ {
    uint32_t data, format, w, h, type, stride, palette, gamma;
    uint32_t hash, levels;
    uint32_t iw, ih;                        /* tamaño de la imagen (mayor si es una textura mejorada) */
    uint32_t scene;                         /* escena en la que se comprobó el contenido */
    uint32_t ww_gen;                        /* generación de escrituras cuando se comprobó (0 = nunca) */
    uint32_t ww_frame;                      /* fotograma en el que Windows ya dijo que estaba limpia */
    int next;                               /* cadena de la tabla g_tex_bucket (índice+1) */
    VkImage img; VkDeviceMemory mem; VkImageView view;
    VkDescriptorSet set[MAX_SAMPLERS];
} Texture;

/* Todo lo que cambia el pipeline; se compara con memcmp. */
typedef struct {
    uint8_t blend, color_func, alpha_func, color_src, color_dst, alpha_src, alpha_dst, color_mask;
    uint8_t cull, depth_func, depth_write, stencil, color_off;
    uint8_t st[2][4];                       /* func, fail, zfail, pass */
    uint16_t vs, fs;                        /* módulos de shaders traducidos (0 = genérico) */
    uint8_t nattr;                          /* atributos vec4 del shader traducido */
    uint8_t v3k;                            /* shaders de Vita3K (layout de 4 conjuntos) */
    uint16_t uint_mask;                     /* locations uvec4 (atributos regformat) */
    uint8_t tf;                             /* formato de la superficie (pasada compatible) */
    uint16_t vlay;                          /* disposición de vértices crudos (0 = vec4 float seguidos) */
} PipeKey;

/* --- vértices crudos (fase 4 del plan de rendimiento) --- */
#define MAX_VLAYS 2048
static GpuVLayout g_vlays[MAX_VLAYS];
static uint16_t g_vlay_hash[4096];
static unsigned g_nvlays;
static int g_vb_crudo;                      /* la escena tiene otros buffers en los bindings: reponer el 0 */
static Buffer g_vdef;                       /* (0,0,0,1) para las locations sin atributo */
static uint16_t vlay_id(const GpuVLayout *l) {
    uint32_t h = 2166136261u;
    const uint8_t *p = (const uint8_t *)l;
    for (size_t i = 0; i < sizeof(*l); i++) h = (h ^ p[i]) * 16777619u;
    for (uint32_t s = h & 4095;; s = (s + 1) & 4095) {
        if (!g_vlay_hash[s]) {
            if (g_nvlays >= MAX_VLAYS) return 0;
            g_vlays[g_nvlays] = *l;
            g_vlay_hash[s] = (uint16_t)++g_nvlays;
            return (uint16_t)g_nvlays;
        }
        if (!memcmp(&g_vlays[g_vlay_hash[s] - 1], l, sizeof(*l))) return g_vlay_hash[s];
    }
}
static VkFormat vfmt_vk(uint8_t f, uint8_t c) {
    static const VkFormat t[10][4] = {
        { VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED },
        { VK_FORMAT_R8_SSCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R8G8B8A8_SSCALED },
        { VK_FORMAT_R16_USCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16A16_USCALED },
        { VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED },
        { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM },
        { VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM },
        { VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM },
        { VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM },
        { VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT },
        { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT } };
    return f < 10 && c >= 1 && c <= 4 ? t[f][c - 1] : VK_FORMAT_UNDEFINED;
}

typedef struct { PipeKey key; VkPipeline pipe; } Pipe;
typedef struct { uint32_t key; VkSampler s; } Sampler;

static int g_tried, g_ok;
static DWORD g_win_hilo;   /* hilo que creó la ventana SDL (solo él recibe sus mensajes) */
static volatile int g_win_propio;   /* la ventana tiene su hilo (ventana_hilo) */

/* ventana */
static SDL_Window *g_win;
static VkSurfaceKHR g_surface;
static VkSwapchainKHR g_swap;
static VkImage g_swap_img[8];
static uint32_t g_nswap;
static VkExtent2D g_swap_ext;
static int g_swap_bgr;                    /* imagen de la ventana BGRA (si no, RGBA) */
static VkSemaphore g_sem_acq, g_sem_done;
static VkCommandBuffer g_cmd_present;
static VkFence g_fence_present;
static CRITICAL_SECTION g_cs;
static VkInstance g_inst;
static VkPhysicalDevice g_phys;
static VkPhysicalDeviceMemoryProperties g_memprops;
static VkDevice g_dev;
static VkQueue g_queue;
static uint32_t g_qfam;
static VkCommandPool g_pool;
static VkCommandBuffer g_cmd, g_cmd_once;
static VkFence g_fence, g_fence_once;
static VkRenderPass g_pass, g_pass_load;   /* profundidad/stencil: borrar o conservar */
/* Formatos de superficie de color: la escena se dibuja en el formato real de la
 * superficie GXM (sombras en U8, HDR en F16x4...) y vuelve a la memoria guest
 * con sus bytes por píxel. Índice 0 = U8U8U8U8 (y lo que no está en la tabla). */
#define NTFMT 6
static const VkFormat g_tfmt_vk[NTFMT] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
                                           VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R8G8B8A8_SRGB };
static const uint32_t g_tfmt_bpp[NTFMT] = { 4, 1, 8, 4, 4, 4 };
static VkRenderPass g_passf[NTFMT], g_passf_load[NTFMT];
/* Superficies F16x4 en las que un fragmento escribe datos enteros/empaquetados (el atlas
 * del público): se dibujan con una vista U16x4 y los shaders escriben los 16 bits crudos. */
#define TF_RAW16 NTFMT
static VkRenderPass g_pass_raw, g_pass_raw_load;
static int tfmt_from_gxm(uint32_t f) {
    switch (f & 0xF1800000u) {           /* SCE_GXM_COLOR_BASE_FORMAT_MASK */
        case 0x80800000u: return 1;      /* U8 */
        case 0x01000000u: return 2;      /* F16F16F16F16 */
        case 0x00800000u: return 3;      /* F16F16 */
        case 0x10800000u: return 4;      /* F32 */
        default:          return (f & 1u) ? 5 : 0;   /* U8U8U8U8; bit 0 = gamma (sRGB) */
    }
}
static VkFormat g_depth_fmt;
static uint32_t g_next_ds;   /* gpu_scene_depth */
void gpu_scene_depth(uint32_t depth_addr) { g_next_ds = depth_addr; }
static VkDescriptorSetLayout g_dsl;
static VkDescriptorPool g_dpool;
static VkPipelineLayout g_layout;
static VkShaderModule g_vs, g_fs;
/* Shaders traducidos de los GXP: memoria de uniformes por draw (SSBO) y 16 texturas. */
#define MB_SIZE   (8u << 20)    /* uniformes por escena: máx. medido 3,2 MB (antes 32 MB x 6 escenas en vuelo) */
#define MAX_MODS  4096
static VkDescriptorSetLayout g_dsl_mem, g_dsl_tex;
static VkPipelineLayout g_layout2;
static VkDescriptorPool g_dpool2;          /* conjuntos por draw: se vacía en cada escena */
/* Shaders de Vita3K (v3k_api.h): set 0 = bloques de info + SSBO de uniformes,
 * set 1 vacío, set 2 texturas del vértice, set 3 del fragmento. */
static VkDescriptorSetLayout g_dsl_v3k, g_dsl_empty, g_dsl_tex_all;
static VkPipelineLayout g_layout3;
static VkDescriptorPool g_dpool3, g_dpool3_fijo, g_dpool_dyn;
/* Fase 4b del plan de rendimiento: el conjunto 0 de los shaders v3k (vinfo, finfo, datos de vértice y
 * de fragmento) usa buffers DINÁMICOS. Cada contexto de escena tiene UN conjunto fijo que apunta a su
 * búfer de uniformes y cada dibujo solo pasa los offsets (antes se pedía y escribía un conjunto nuevo
 * por dibujo). Rangos fijos: los datos de un dibujo no pueden pasar de DYN_SB_RANGE (si pasan, se usa
 * un conjunto propio como antes) y el final del búfer se reserva para que offset + rango quepa. */
#define DYN_UB_RANGE 256u
#define DYN_SB_RANGE (64u << 10)
static VkDescriptorSet g_dset0;
static VkDescriptorSet g_vtex_white;       /* texturas del vértice: todas blancas */
static Buffer g_mb;
static uint32_t g_mb_off;
static VkShaderModule g_mods[MAX_MODS];
static uint32_t g_nmods = 1;
static Buffer g_vb, g_ib, g_tex_staging;
static uint32_t g_vb_off, g_ib_off;
static GCtx g_ctx[NCTX];
static int g_nctx = 1, g_ctx_i;
static void ctx_complete(GCtx *c);
static GCtx *ctx_actual(void);
/* Esperas a la GPU por motivo (línea [fps]): 0 lineal-memoria 1 misma superficie 2 contexto
 * 3 todas (fotograma/Finish/texturas viejas) 4 textura que lee memoria pendiente 5 síncrono 6 subidas */
double g_espera_mot[8];
static int g_mot;
#define CTXC(c, m) do { g_mot = (m); ctx_complete(c); } while (0)
static void ctx_complete_all(void);
static void pend_overlap(uint32_t a, uint32_t n);
void gpu_sync_all_m(int motivo);
static Target g_targets[MAX_TARGETS];
static Texture g_tex[MAX_TEXTURES];
static uint32_t g_ntex;
static Pipe g_pipes[MAX_PIPES];
static uint32_t g_npipes;
static uint32_t g_phash[PIPE_HASH];        /* índice + 1 en g_pipes (0 = libre) */

static uint32_t pipe_hash(const PipeKey *k) {
    const uint8_t *p = (const uint8_t *)k;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(*k); i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
static Sampler g_samplers[MAX_SAMPLERS];
static uint32_t g_nsamplers;
static Texture *g_white;
static Texture *g_black;   /* MLB15_TEX_NEGRA=dir,...: esas texturas a 0 (pruebas) */
static Target *g_cur;
static GCtx *ctx_actual(void) { return (g_cur && g_nctx > 1) ? &g_ctx[g_ctx_i] : NULL; }   /* escena que se está grabando */
uint32_t gpu_target_addr(void) { return g_cur ? g_cur->addr : 0; }
static uint32_t g_stat_scenes, g_stat_draws;
static uint32_t *g_decode;           /* RGBA de la textura que se está subiendo */
static size_t g_decode_cap;

static char g_vk_error[256];   /* por qué no arrancó Vulkan (gpu_error) */
#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    printf("[gpu] %s falló (%d) en %s:%d\n", #x, (int)_r, __FILE__, __LINE__); fflush(stdout); \
    snprintf(g_vk_error, sizeof(g_vk_error), "%s devolvió %d", #x, (int)_r); \
    { extern void arranque(const char *fmt, ...); arranque("gpu: FALLO %s", g_vk_error); } return 0; } } while (0)

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

/* readback: memoria HOST_CACHED si la hay. La de por defecto (combinada para
 * escritura) es rápida de escribir pero leerla con la CPU es lentísimo, y cada
 * escena copia su superficie de vuelta a la memoria del juego: en el partido
 * (muchas escenas por fotograma) era el 60 % del hilo de dibujo. */
int gpu_vfmt_ok(uint8_t f, uint8_t c) {
    static uint8_t ok[10][5];   /* 0 sin mirar, 1 sí, 2 no */
    if (f >= 10 || c < 1 || c > 4 || !g_ok) return 0;
    if (!ok[f][c]) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_phys, vfmt_vk(f, c), &fp);
        ok[f][c] = (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : 2;
    }
    return ok[f][c] == 1;
}
static void vb_normal(void) {   /* antes de un dibujo del camino viejo: binding 0 = g_vb desde 0 */
    if (!g_vb_crudo) return;
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(g_cmd, 0, 1, &g_vb.buf, &zero);
    g_vb_crudo = 0;
}

static int buffer_new_ex(Buffer *b, VkDeviceSize size, VkBufferUsageFlags usage, int readback) {
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    CHECK(vkCreateBuffer(g_dev, &bi, NULL, &b->buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_dev, b->buf, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = UINT32_MAX;
    if (readback)
        ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX)
        ai.memoryTypeIndex = mem_type(mr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkAllocateMemory(g_dev, &ai, NULL, &b->mem));
    CHECK(vkBindBufferMemory(g_dev, b->buf, b->mem, 0));
    CHECK(vkMapMemory(g_dev, b->mem, 0, size, 0, &b->ptr));
    b->size = size;
    return 1;
}

static int buffer_new(Buffer *b, VkDeviceSize size, VkBufferUsageFlags usage) { return buffer_new_ex(b, size, usage, 0); }

static float g_aniso;   /* 0 = sin filtrado anisótropo */
/* Resolución interna: las superficies se dibujan a w x g_res (MLB15_RESOLUCION o MLB15.ini
 * [video] resolucion; 3 por defecto: el partido va limitado por la CPU y a x3 enchufado
 * da los mismos FPS que a x2, con los jugadores lejanos mucho más nítidos). La memoria guest sigue a tamaño nativo (se reduce al
 * copiar) y los shaders de Vita3K dividen gl_FragCoord por res_multiplier. */
static uint32_t g_res = 1;
static Buffer g_hd;                         /* MLB15_VOLCAR_HD */
static unsigned g_hd_pending, g_hd_w, g_hd_h;
uint32_t gpu_res_scale(void) { return g_res; }
uint32_t gpu_target_addr(void);
static VkBool32 g_depth_clamp;

static int image_new_lv(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, uint32_t levels, VkImage *img,
                        VkDeviceMemory *mem, VkImageView *view);
static int image_new_fmt(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkImage *img, VkDeviceMemory *mem,
                         VkImageView *view) {
    return image_new_lv(w, h, fmt, usage, 1, img, mem, view);
}
static int image_new_lv(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, uint32_t levels, VkImage *img,
                        VkDeviceMemory *mem, VkImageView *view) {
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    if (fmt == VK_FORMAT_R8G8B8A8_UNORM || fmt == VK_FORMAT_R8G8B8A8_SRGB || fmt == VK_FORMAT_R16G16B16A16_SFLOAT)
        ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;   /* vistas UNORM/sRGB, o F16x4 leída como U32U32 */
    ii.extent.width = w; ii.extent.height = h; ii.extent.depth = 1;
    ii.mipLevels = levels; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkCreateImage(g_dev, &ii, NULL, img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_dev, *img, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(vkAllocateMemory(g_dev, &ai, NULL, mem));
    CHECK(vkBindImageMemory(g_dev, *img, *mem, 0));
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = *img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = levels;
    vi.subresourceRange.layerCount = 1;
    CHECK(vkCreateImageView(g_dev, &vi, NULL, view));
    return 1;
}

static int image_new(uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImage *img, VkDeviceMemory *mem,
                     VkImageView *view) {
    return image_new_fmt(w, h, VK_FORMAT_R8G8B8A8_UNORM, usage, img, mem, view);
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src_a, VkAccessFlags dst_a, VkPipelineStageFlags src_s, VkPipelineStageFlags dst_s) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from; b.newLayout = to;
    b.srcAccessMask = src_a; b.dstAccessMask = dst_a;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, src_s, dst_s, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Ejecuta en el acto un cmd buffer aparte (subidas de texturas). */
static VkCommandBuffer once_begin(void) {
    vkResetCommandBuffer(g_cmd_once, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_cmd_once, &bi);
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(g_cmd_once, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    return g_cmd_once;
}
static double prof_now(void);
/* Cronómetros de gpu_draw_v3k (línea [fps]): 0 pipeline 1 texturas 2 asignar conjuntos
 * 3 uniformes (mb_put) 4 escribir descriptores 5 copiar vértices/índices 6 órdenes */
double g_envio_t[8];
/* Direcciones que son (o fueron) superficies: target_texture recorría las 64 superficies para
 * cada textura de cada draw; casi nunca coincide, así que se descarta antes con este filtro. */
static uint8_t g_tgt_filtro[4096];
static uint32_t g_texset_esc = 1;   /* generación de la caché de conjuntos de texturas (gpu_draw_v3k) */
unsigned g_cnt_once, g_cnt_escenas, g_cnt_vuelta;   /* línea [fps] */
static const char *g_once_quien = "?";               /* MLB15_ESCENAS_HISTO: quién sube */
static void once_end(void) {
    g_cnt_once++;
    {
        static int on = -1;
        static struct { const char *q; unsigned n; } qs[16];
        static unsigned tot;
        if (on < 0) on = env_cached("MLB15_ESCENAS_HISTO") ? 1 : 0;
        if (on) {
            int k;
            for (k = 0; k < 16 && qs[k].q && qs[k].q != g_once_quien; k++) {}
            if (k < 16) { qs[k].q = g_once_quien; qs[k].n++; }
            if (++tot % 2000 == 0) {
                printf("[histo] subidas (ultimas 2000):");
                for (k = 0; k < 16 && qs[k].q; k++) printf(" %s x%u", qs[k].q, qs[k].n);
                printf("\n");
                memset(qs, 0, sizeof(qs));
            }
        }
        g_once_quien = "?";
    }
    vkEndCommandBuffer(g_cmd_once);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd_once;
    vkResetFences(g_dev, 1, &g_fence_once);
    vkQueueSubmit(g_queue, 1, &si, g_fence_once);
    double w0 = prof_now();
    vkWaitForFences(g_dev, 1, &g_fence_once, VK_TRUE, UINT64_MAX);
    g_espera_mot[6] += prof_now() - w0;
}

/* Subidas de texturas sin esperar: dentro de una escena se graban en el cmd buffer de subidas
 * de su contexto (con el búfer de subida del contexto) y gpu_scene_end lo envía justo antes de
 * la escena. Fuera de escena, si no cabe o con MLB15_SUBIDAS_SINCRONAS=1, como antes (once). */
typedef struct { VkCommandBuffer cb; VkBuffer buf; VkDeviceSize off; uint8_t *ptr; int async; } Subida;
static GCtx *ctx_actual(void);
static int subida_begin(VkDeviceSize n, Subida *sb) {
    static int sinc = -1;
    if (sinc < 0) sinc = env_cached("MLB15_SUBIDAS_SINCRONAS") ? 1 : 0;
    GCtx *c = ctx_actual();
    VkDeviceSize off = c ? ((VkDeviceSize)c->up_off + 255u) & ~(VkDeviceSize)255u : 0;
    if (!sinc && c && c->ucmd && off + n <= UP_SIZE) {
        if (!c->u_open) {
            vkResetCommandBuffer(c->ucmd, 0);
            VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(c->ucmd, &bi);
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(c->ucmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
            c->u_open = 1;
        }
        c->up_off = (uint32_t)(off + n);
        { extern unsigned g_up_max; if (c->up_off > g_up_max) g_up_max = c->up_off; }
        sb->cb = c->ucmd; sb->buf = c->up.buf; sb->off = off; sb->ptr = (uint8_t *)c->up.ptr + off; sb->async = 1;
        g_cnt_once++;
        return 1;
    }
    if (n > g_tex_staging.size) return 0;
    sb->cb = once_begin(); sb->buf = g_tex_staging.buf; sb->off = 0; sb->ptr = (uint8_t *)g_tex_staging.ptr; sb->async = 0;
    return 1;
}
static void subida_end(Subida *sb) { if (!sb->async) once_end(); }

/* --- inicialización -------------------------------------------------------- */

/* Hilo de la ventana: la crea y atiende sus mensajes siempre (teclas, ratón, mover, redimensionar,
 * pantalla completa), dibuje quien dibuje. gpu_present ya solo lee el mando (input_poll). */
static HANDLE g_win_listo;
static void manejar_evento(SDL_Event *e);
static DWORD WINAPI ventana_hilo(LPVOID arg) {
    (void)arg;
        SDL_SetMainReady();
        /* Pantallas con escalado de Windows (el portátil del usuario: 2560x1600 al 150 %):
         * sin declararse compatible, Windows dibujaba la ventana a 2/3 y la estiraba
         * (imagen pixelada) y la ventana de 1920x1088 no cabía en los 1707x1067 lógicos. */
        SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
        /* El mando funciona aunque la ventana no tenga el foco (por defecto SDL lo ignora:
         * un clic en otra ventana durante la carga dejaba el mando muerto en el partido). */
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
            printf("[gpu] SDL_Init falló: %s (sin ventana)\n", SDL_GetError());
        } else {
            /* Escala y pantalla completa: MLB15_SCALE manda; si no, MLB15.ini [video]. */
            const char *sc = env_cached("MLB15_SCALE");
            int scale = sc ? atoi(sc) : launcher_ini_int("video", "escala", 0);   /* 0 = automática */
            Uint32 fl = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
            if (launcher_ini_int("video", "pantalla_completa", 0)) fl |= SDL_WINDOW_FULLSCREEN_DESKTOP;
            /* Que la ventana quepa en la pantalla: a escala 2 son 1920x1088, más que un
             * monitor de 1080 con la barra de tareas y el marco, y se salía por los lados y
             * por abajo. Se ajusta (misma proporción) al área útil menos el marco. */
            int ww = 960 * (scale > 0 ? scale : 2), wh = 544 * (scale > 0 ? scale : 2);
            SDL_Rect ub;
            if (SDL_GetDisplayUsableBounds(0, &ub) == 0 && ub.w > 0 && ub.h > 0) {
                int mw = ub.w - 16, mh = ub.h - 48;   /* bordes y barra de título */
                if (scale <= 0) { ww = mw * 85 / 100; wh = ww * 544 / 960; }   /* automática: 85 % */
                if (ww > mw) { wh = wh * mw / ww; ww = mw; }
                if (wh > mh) { ww = ww * mh / wh; wh = mh; }
                printf("[gpu] área útil de la pantalla %dx%d -> ventana %dx%d\n", ub.w, ub.h, ww, wh);
            }
            if (env_cached("MLB15_VENTANA_OCULTA")) fl |= SDL_WINDOW_HIDDEN;   /* pruebas con ventana sin mostrarla */
            g_win = SDL_CreateWindow("Project Baseball", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh, fl);
            g_win_hilo = GetCurrentThreadId();
            if (!g_win) printf("[gpu] SDL_CreateWindow falló: %s (sin ventana)\n", SDL_GetError());
        }
    g_win_propio = g_win != NULL;
    SetEvent(g_win_listo);
    if (!g_win) return 0;
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 20)) {
            do manejar_evento(&e); while (SDL_PollEvent(&e));
        }
    }
}

static int init_vulkan(void) {
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) { printf("[gpu] no hay vulkan-1.dll: sin dibujo\n"); snprintf(g_vk_error, sizeof(g_vk_error), "no hay vulkan-1.dll"); return 0; }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress(lib, "vkGetInstanceProcAddr");
#define LOADG(f) f = (PFN_##f)vkGetInstanceProcAddr(NULL, #f);
    VK_GLOBAL(LOADG)

    /* Ventana SDL (salvo MLB15_NOWINDOW=1): la instancia necesita sus extensiones. */
    const char *exts[16];
    unsigned int next = 0;
    const char *nw = env_cached("MLB15_NOWINDOW");
    if (!(nw && *nw == '1')) {
        /* La ventana vive en un hilo propio (ventana_hilo): en Windows solo el hilo que crea una
         * ventana recibe sus mensajes, y en el partido presenta el hilo «Display Engine», no el que la
         * creaba (main). Nadie recogía las teclas (el teclado no iba en el partido) y, tras un clic,
         * Windows la daba por colgada («No responde», imagen blanquecina). */
        g_win_listo = CreateEventA(NULL, TRUE, FALSE, NULL);
        HANDLE th = g_win_listo ? CreateThread(NULL, 0, ventana_hilo, NULL, 0, NULL) : NULL;
        if (th) { WaitForSingleObject(g_win_listo, 15000); CloseHandle(th); }
        if (g_win) {
            next = 16;
            if (!SDL_Vulkan_GetInstanceExtensions(g_win, &next, exts)) next = 0;
            input_init();   /* carga mapeo de teclas desde MLB15.ini */
        }
    }
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Project Baseball";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = next;
    ici.ppEnabledExtensionNames = exts;
    CHECK(vkCreateInstance(&ici, NULL, &g_inst));
#define LOADI(f) f = (PFN_##f)vkGetInstanceProcAddr(g_inst, #f);
    VK_INSTANCE(LOADI)

    if (g_win && !SDL_Vulkan_CreateSurface(g_win, g_inst, &g_surface)) {
        printf("[gpu] SDL_Vulkan_CreateSurface falló: %s (sin ventana)\n", SDL_GetError());
        SDL_DestroyWindow(g_win);
        g_win = NULL;
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_inst, &n, NULL);
    VkPhysicalDevice devs[8];
    if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(g_inst, &n, devs);
    if (!n) { printf("[gpu] ninguna GPU con Vulkan\n"); return 0; }
    g_phys = devs[0];
    for (uint32_t i = 0; i < n; i++) {                        /* mejor la dedicada */
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { g_phys = devs[i]; break; }
    }
    {   /* MLB15_GPU_INDICE=n o MLB15.ini [video] tarjeta=n: usar esa tarjeta (pruebas con la integrada) */
        const char *gi = env_cached("MLB15_GPU_INDICE");
        int k = gi ? atoi(gi) : launcher_ini_int("video", "tarjeta", -1);
        if (k >= 0 && (uint32_t)k < n) g_phys = devs[k];
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_phys, &props);
    printf("[gpu] tarjeta: %s\n", props.deviceName);
    if (env_cached("MLB15_VFMT_TRAZA")) {   /* formatos de vértice que la tarjeta lee directamente */
        static const struct { VkFormat f; const char *n; } fs[] = {
            { VK_FORMAT_R8G8B8_UNORM, "u8n x3" }, { VK_FORMAT_R8G8B8_SNORM, "s8n x3" }, { VK_FORMAT_R8G8B8_USCALED, "u8 x3" },
            { VK_FORMAT_R8G8B8_UINT, "u8 x3 int" }, { VK_FORMAT_R16G16B16_SFLOAT, "f16 x3" }, { VK_FORMAT_R16G16B16_SNORM, "s16n x3" },
            { VK_FORMAT_R16G16B16_SSCALED, "s16 x3" }, { VK_FORMAT_R8_USCALED, "u8 x1" }, { VK_FORMAT_R8G8B8A8_USCALED, "u8 x4" },
            { VK_FORMAT_R16G16_SFLOAT, "f16 x2" }, { VK_FORMAT_R32G32B32_SFLOAT, "f32 x3" } };
        for (unsigned i = 0; i < sizeof(fs) / sizeof(fs[0]); i++) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(g_phys, fs[i].f, &fp);
            printf("[vfmt] %s: %s\n", fs[i].n, (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? "sí" : "NO");
        }
    }
    vkGetPhysicalDeviceMemoryProperties(g_phys, &g_memprops);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &nq, NULL);
    VkQueueFamilyProperties qf[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &nq, qf);
    g_qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 pres = VK_TRUE;
        if (g_win) vkGetPhysicalDeviceSurfaceSupportKHR(g_phys, i, g_surface, &pres);
        if (pres) { g_qfam = i; break; }
    }
    if (g_qfam == UINT32_MAX) return 0;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = g_qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    const char *dext[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    if (g_win) { di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = dext; }
    VkPhysicalDeviceFeatures have = { 0 }, want = { 0 };
    vkGetPhysicalDeviceFeatures(g_phys, &have);
    if (have.depthClamp && !env_cached("MLB15_SIN_DEPTHCLAMP")) { want.depthClamp = VK_TRUE; g_depth_clamp = VK_TRUE; }
    if (have.samplerAnisotropy) {
        want.samplerAnisotropy = VK_TRUE;
        g_aniso = props.limits.maxSamplerAnisotropy < 16.0f ? props.limits.maxSamplerAnisotropy : 16.0f;
    }
    di.pEnabledFeatures = &want;
    CHECK(vkCreateDevice(g_phys, &di, NULL, &g_dev));
    {
        const char *e = env_cached("MLB15_RESOLUCION");
        int r = e ? atoi(e) : launcher_ini_int("video", "resolucion", 3);
        g_res = r < 1 ? 1u : r > 4 ? 4u : (uint32_t)r;
        printf("[gpu] resolución interna x%u\n", g_res);
        { extern void arranque(const char *fmt, ...); arranque("gpu: tarjeta elegida %s; resolución interna x%u", props.deviceName, g_res); }
    }
#define LOADD(f) f = (PFN_##f)vkGetDeviceProcAddr(g_dev, #f);
    VK_DEVICE(LOADD)
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
    printf("[gpu] Vulkan en %s\n", props.deviceName);
    fflush(stdout);

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_qfam;
    CHECK(vkCreateCommandPool(g_dev, &pci, NULL, &g_pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    CHECK(vkAllocateCommandBuffers(g_dev, &cai, &g_cmd));
    CHECK(vkAllocateCommandBuffers(g_dev, &cai, &g_cmd_once));
    CHECK(vkAllocateCommandBuffers(g_dev, &cai, &g_cmd_present));
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CHECK(vkCreateFence(g_dev, &fi, NULL, &g_fence));
    CHECK(vkCreateFence(g_dev, &fi, NULL, &g_fence_once));
    CHECK(vkCreateFence(g_dev, &fi, NULL, &g_fence_present));
    VkSemaphoreCreateInfo semi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    CHECK(vkCreateSemaphore(g_dev, &semi, NULL, &g_sem_acq));
    CHECK(vkCreateSemaphore(g_dev, &semi, NULL, &g_sem_done));

    /* Pasada: conserva lo que hay (la escena GXM empieza sobre la memoria). */
    VkAttachmentDescription att = { 0 };
    att.format = VK_FORMAT_R8G8B8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    /* Profundidad/stencil: GXM empieza cada escena con los valores de fondo
     * (profundidad 1, stencil 0) salvo ForceLoad, que el juego no usa aquí. */
    VkFormat cand[3] = { VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM_S8_UINT };
    g_depth_fmt = VK_FORMAT_UNDEFINED;
    for (int i = 0; i < 3 && g_depth_fmt == VK_FORMAT_UNDEFINED; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_phys, cand[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) g_depth_fmt = cand[i];
    }
    if (g_depth_fmt == VK_FORMAT_UNDEFINED) { printf("[gpu] sin formato de profundidad+stencil\n"); snprintf(g_vk_error, sizeof(g_vk_error), "la tarjeta no tiene un formato de profundidad+stencil"); return 0; }
    VkAttachmentDescription atts[2];
    atts[0] = att;
    memset(&atts[1], 0, sizeof(atts[1]));
    atts[1].format = g_depth_fmt;
    atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { 0 };
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    sub.pDepthStencilAttachment = &dref;
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = 2;
    rpi.pAttachments = atts;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    for (int i = 0; i < NTFMT; i++) {
        atts[0].format = g_tfmt_vk[i];
        CHECK(vkCreateRenderPass(g_dev, &rpi, NULL, &g_passf[i]));
    }
    atts[0].format = VK_FORMAT_R16G16B16A16_UINT;
    CHECK(vkCreateRenderPass(g_dev, &rpi, NULL, &g_pass_raw));
    atts[0].format = g_tfmt_vk[0];
    g_pass = g_passf[0];
    /* La misma pasada pero conservando profundidad/stencil (compatible con los
     * mismos framebuffers: solo cambian las operaciones de carga). */
    atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    atts[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    for (int i = 0; i < NTFMT; i++) {
        atts[0].format = g_tfmt_vk[i];
        CHECK(vkCreateRenderPass(g_dev, &rpi, NULL, &g_passf_load[i]));
    }
    atts[0].format = VK_FORMAT_R16G16B16A16_UINT;
    CHECK(vkCreateRenderPass(g_dev, &rpi, NULL, &g_pass_raw_load));
    atts[0].format = g_tfmt_vk[0];
    g_pass_load = g_passf_load[0];

    VkDescriptorSetLayoutBinding bind = { 0 };
    bind.binding = 0;
    bind.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bind.descriptorCount = 1;
    bind.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dli.bindingCount = 1;
    dli.pBindings = &bind;
    CHECK(vkCreateDescriptorSetLayout(g_dev, &dli, NULL, &g_dsl));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpi.maxSets = 16384;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    CHECK(vkCreateDescriptorPool(g_dev, &dpi, NULL, &g_dpool));

    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &g_dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    CHECK(vkCreatePipelineLayout(g_dev, &pli, NULL, &g_layout));

    {   /* layout de los shaders traducidos: set 0 = memoria, set 1 = texturas 0-15 */
        VkDescriptorSetLayoutBinding mb = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 1; li.pBindings = &mb;
        CHECK(vkCreateDescriptorSetLayout(g_dev, &li, NULL, &g_dsl_mem));
        VkDescriptorSetLayoutBinding tb[16];
        for (uint32_t i = 0; i < 16; i++) {
            tb[i].binding = i; tb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            tb[i].descriptorCount = 1; tb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; tb[i].pImmutableSamplers = NULL;
        }
        li.bindingCount = 16; li.pBindings = tb;
        CHECK(vkCreateDescriptorSetLayout(g_dev, &li, NULL, &g_dsl_tex));
        VkDescriptorSetLayout two[2] = { g_dsl_mem, g_dsl_tex };
        VkPipelineLayoutCreateInfo l2 = pli;
        l2.setLayoutCount = 2; l2.pSetLayouts = two;
        CHECK(vkCreatePipelineLayout(g_dev, &l2, NULL, &g_layout2));
        VkDescriptorPoolSize ps2[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192 },
                                        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192 * 16 } };
        VkDescriptorPoolCreateInfo dp2 = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dp2.maxSets = 16384; dp2.poolSizeCount = 2; dp2.pPoolSizes = ps2;
        CHECK(vkCreateDescriptorPool(g_dev, &dp2, NULL, &g_dpool2));
        {
            const char *e = env_cached("MLB15_ESCENAS_VUELO");
            g_nctx = e ? atoi(e) : 4;   /* 2026-11-06, con ventana a x3: 4 y 6 dan 59,9 FPS; 4 reserva ~180 MB menos */
            if (g_nctx < 1) g_nctx = 1;
            if (g_nctx > NCTX) g_nctx = NCTX;
        }
        g_ctx[0].dp2 = g_dpool2;
        for (int k = 1; k < g_nctx; k++) CHECK(vkCreateDescriptorPool(g_dev, &dp2, NULL, &g_ctx[k].dp2));

        VkDescriptorSetLayoutBinding vb[4];
        for (uint32_t i = 0; i < 4; i++) {
            vb[i].binding = i;
            vb[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            vb[i].descriptorCount = 1;
            vb[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            vb[i].pImmutableSamplers = NULL;
        }
        li.bindingCount = 4; li.pBindings = vb;
        CHECK(vkCreateDescriptorSetLayout(g_dev, &li, NULL, &g_dsl_v3k));
        li.bindingCount = 0; li.pBindings = NULL;
        CHECK(vkCreateDescriptorSetLayout(g_dev, &li, NULL, &g_dsl_empty));
        for (uint32_t i = 0; i < 16; i++) tb[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        li.bindingCount = 16; li.pBindings = tb;
        CHECK(vkCreateDescriptorSetLayout(g_dev, &li, NULL, &g_dsl_tex_all));
        VkDescriptorSetLayout four[4] = { g_dsl_v3k, g_dsl_empty, g_dsl_tex_all, g_dsl_tex_all };
        VkPipelineLayoutCreateInfo l3 = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        l3.setLayoutCount = 4; l3.pSetLayouts = four;
        CHECK(vkCreatePipelineLayout(g_dev, &l3, NULL, &g_layout3));
        VkDescriptorPoolSize ps3[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 32768 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 32768 },
                                        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384 * 16 } };
        VkDescriptorPoolCreateInfo dp3 = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dp3.maxSets = 32768; dp3.poolSizeCount = 3; dp3.pPoolSizes = ps3;
        CHECK(vkCreateDescriptorPool(g_dev, &dp3, NULL, &g_dpool3));
        g_ctx[0].dp3 = g_dpool3;
        for (int k = 1; k < g_nctx; k++) CHECK(vkCreateDescriptorPool(g_dev, &dp3, NULL, &g_ctx[k].dp3));
        VkDescriptorPoolSize ps4 = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 };
        dp3.maxSets = 1; dp3.poolSizeCount = 1; dp3.pPoolSizes = &ps4;
        CHECK(vkCreateDescriptorPool(g_dev, &dp3, NULL, &g_dpool3_fijo));
    }

    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smi.codeSize = sizeof(spv_ui_vert);
    smi.pCode = spv_ui_vert;
    CHECK(vkCreateShaderModule(g_dev, &smi, NULL, &g_vs));
    smi.codeSize = sizeof(spv_ui_frag);
    smi.pCode = spv_ui_frag;
    CHECK(vkCreateShaderModule(g_dev, &smi, NULL, &g_fs));

    if (!buffer_new(&g_vb, VB_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
    if (!buffer_new(&g_ib, IB_SIZE, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return 0;
    if (!buffer_new(&g_vdef, 256, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
    { float def[4] = { 0.0f, 0.0f, 0.0f, 1.0f }; memcpy(g_vdef.ptr, def, sizeof(def)); }
    if (!buffer_new(&g_mb, MB_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return 0;
    g_ctx[0].cmd = g_cmd; g_ctx[0].fence = g_fence; g_ctx[0].vb = g_vb; g_ctx[0].ib = g_ib; g_ctx[0].mb = g_mb;
    for (int k = 1; k < g_nctx; k++) {
        VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ca.commandPool = g_pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
        CHECK(vkAllocateCommandBuffers(g_dev, &ca, &g_ctx[k].cmd));
        VkFenceCreateInfo fc = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        CHECK(vkCreateFence(g_dev, &fc, NULL, &g_ctx[k].fence));
        if (!buffer_new(&g_ctx[k].vb, VB_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
        if (!buffer_new(&g_ctx[k].ib, IB_SIZE, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return 0;
        if (!buffer_new(&g_ctx[k].mb, MB_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return 0;
    }
    for (int k = 0; k < g_nctx && g_nctx > 1; k++) {
        VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ca.commandPool = g_pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
        CHECK(vkAllocateCommandBuffers(g_dev, &ca, &g_ctx[k].ucmd));
        if (!buffer_new(&g_ctx[k].up, UP_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return 0;
    }
    {
        VkDescriptorPoolSize pd[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 * NCTX }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 2 * NCTX } };
        VkDescriptorPoolCreateInfo dpc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpc.maxSets = NCTX; dpc.poolSizeCount = 2; dpc.pPoolSizes = pd;
        CHECK(vkCreateDescriptorPool(g_dev, &dpc, NULL, &g_dpool_dyn));
        for (int k = 0; k < g_nctx; k++) {
            VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = g_dpool_dyn; ai.descriptorSetCount = 1; ai.pSetLayouts = &g_dsl_v3k;
            CHECK(vkAllocateDescriptorSets(g_dev, &ai, &g_ctx[k].dset0));
            VkBuffer mb = k ? g_ctx[k].mb.buf : g_mb.buf;
            VkDescriptorBufferInfo bi[4] = { { mb, 0, DYN_UB_RANGE }, { mb, 0, DYN_UB_RANGE }, { mb, 0, DYN_SB_RANGE }, { mb, 0, DYN_SB_RANGE } };
            VkWriteDescriptorSet w[4];
            for (int i = 0; i < 4; i++) {
                memset(&w[i], 0, sizeof(w[i])); w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstSet = g_ctx[k].dset0; w[i].dstBinding = (uint32_t)i; w[i].descriptorCount = 1;
                w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
                w[i].pBufferInfo = &bi[i];
            }
            vkUpdateDescriptorSets(g_dev, 4, w, 0, NULL);
        }
        g_dset0 = env_cached("MLB15_SIN_UNIF_DIN") ? VK_NULL_HANDLE : g_ctx[0].dset0;
    }
    printf("[gpu] escenas en vuelo: %d\n", g_nctx);
    if (!buffer_new(&g_tex_staging, 4096u * 4096u * 4u, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return 0;
    return 1;
}

int gpu_init(void) {
    if (g_tried) return g_ok;
    g_tried = 1;
    InitializeCriticalSection(&g_cs);
    const char *e = env_cached("MLB15_GPU");
    if (e && *e == '0') { printf("[gpu] desactivado (MLB15_GPU=0)\n"); return 0; }
    g_ok = init_vulkan();
    if (!g_ok) printf("[gpu] Vulkan no disponible: el port sigue sin dibujar\n");
    fflush(stdout);
    return g_ok;
}

int gpu_ready(void) { return g_ok; }
const char *gpu_error(void) { return g_vk_error[0] ? g_vk_error : "Vulkan no disponible"; }

/* --- recursos con caché ---------------------------------------------------- */

static uint32_t guest_hash(uint32_t addr, uint32_t bytes) {
    const uint8_t *p = bytes ? (const uint8_t *)vita_mem_ptr(addr, bytes) : NULL;
    uint32_t h = 2166136261u ^ bytes;
    if (!p) return h;
    uint32_t step = bytes > 4096 ? (bytes / 1024) & ~3u : 4;
    for (uint32_t i = 0; i + 4 <= bytes; i += step) { uint32_t v; memcpy(&v, p + i, 4); h = (h ^ v) * 16777619u; }
    return h;
}

static Target *target_for(uint32_t addr, uint32_t w, uint32_t h, int tf) {
    for (int i = 0; i < MAX_TARGETS; i++)
        if (g_targets[i].img && g_targets[i].addr == addr && g_targets[i].w == w && g_targets[i].h == h && g_targets[i].tf == tf)
            return &g_targets[i];
    for (int i = 0; i < MAX_TARGETS; i++) if (!g_targets[i].img) {
        Target *t = &g_targets[i];
        t->addr = addr; t->w = w; t->h = h; t->tf = (uint8_t)tf;
        g_tgt_filtro[(addr >> 4) & 4095] = 1;
        t->sw = w * g_res; t->sh = h * g_res;
        if (!image_new_fmt(t->sw, t->sh, g_tfmt_vk[tf], VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       &t->img, &t->mem, &t->view)) { t->img = VK_NULL_HANDLE; return NULL; }
        if (g_res > 1) {
            if (!image_new_fmt(w, h, g_tfmt_vk[tf], VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT,   /* pase final de lo que escribe la CPU */
                               &t->simg, &t->smem, &t->sview)) return NULL;
        } else {
            t->simg = t->img;
        }
        {   /* profundidad + stencil del mismo tamaño */
            VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = g_depth_fmt;
            ii.extent.width = t->sw; ii.extent.height = t->sh; ii.extent.depth = 1;
            ii.mipLevels = 1; ii.arrayLayers = 1;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;   /* mapa de sombras */
            if (vkCreateImage(g_dev, &ii, NULL, &t->dimg) != VK_SUCCESS) return NULL;
            VkMemoryRequirements mr;
            vkGetImageMemoryRequirements(g_dev, t->dimg, &mr);
            VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(g_dev, &ai, NULL, &t->dmem) != VK_SUCCESS) return NULL;
            vkBindImageMemory(g_dev, t->dimg, t->dmem, 0);
            VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vi.image = t->dimg;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = g_depth_fmt;
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.layerCount = 1;
            if (vkCreateImageView(g_dev, &vi, NULL, &t->dview) != VK_SUCCESS) return NULL;
        }
        VkImageView views[2] = { t->view, t->dview };
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = g_passf[tf];
        fi.attachmentCount = 2;
        fi.pAttachments = views;
        fi.width = t->sw; fi.height = t->sh; fi.layers = 1;
        if (vkCreateFramebuffer(g_dev, &fi, NULL, &t->fb) != VK_SUCCESS) return NULL;
        if (!buffer_new_ex(&t->staging, (VkDeviceSize)w * h * g_tfmt_bpp[tf],
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1)) return NULL;
        printf("[gpu] superficie 0x%08X %ux%u formato %d\n", addr, w, h, tf);
        fflush(stdout);
        return t;
    }
    return NULL;
}

static VkSampler sampler_for(const GpuTexture *t, uint32_t *index) {
    static const VkSamplerAddressMode am[8] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, /* CLAMP_FULL_BORDER: el «borde» GXM son
                                                                                 * los texels del contorno; con el borde negro de
                                                                                 * Vulkan salían cuñas negras en la tierra */
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    uint32_t key = (uint32_t)(t->min_linear & 1) | ((uint32_t)(t->mag_linear & 1) << 1) |
                   ((uint32_t)(t->u_addr & 7) << 2) | ((uint32_t)(t->v_addr & 7) << 5);
    for (uint32_t i = 0; i < g_nsamplers; i++) if (g_samplers[i].key == key) { *index = i; return g_samplers[i].s; }
    if (g_nsamplers >= MAX_SAMPLERS) { *index = 0; return g_samplers[0].s; }
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = (key & 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = (key & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = (key & 1) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (g_aniso > 1.0f && (key & 1)) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = g_aniso; }
    si.addressModeU = am[(key >> 2) & 7];
    si.addressModeV = am[(key >> 5) & 7];
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    {   /* MLB15_BORDE=negro|blanco: color fuera de la textura con CLAMP_FULL_BORDER */
        static int blanco = -1;
        if (blanco < 0) { const char *e = env_cached("MLB15_BORDE"); blanco = e && *e == 'n' ? 0 : 1; }
        si.borderColor = blanco ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    si.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler s;
    if (vkCreateSampler(g_dev, &si, NULL, &s) != VK_SUCCESS) { *index = 0; return g_samplers[0].s; }
    g_samplers[g_nsamplers].key = key;
    g_samplers[g_nsamplers].s = s;
    *index = g_nsamplers;
    return g_samplers[g_nsamplers++].s;
}

/* Hash barato del contenido: detecta texturas que el juego reescribe (fuentes,
 * render targets) sin decodificar en cada draw. */
static uint32_t cube_face_bytes(const GpuTexture *t);
/* --- páginas escritas (PLAN_RENDIMIENTO.md fase 4b) ---------------------------------------------
 * La memoria de la Vita se reserva con MEM_WRITE_WATCH (vitarecomp mem.c): Windows apunta qué
 * páginas se escriben. Una vez por fotograma se recogen y se reinician (ww_scan: cada página
 * escrita recibe la generación nueva); en cualquier momento, GetWriteWatch sin reiniciar sobre un
 * rango dice si se escribió desde el último recuento. Así una textura que nadie tocó no se vuelve a
 * leer para calcular su huella (antes: ~1000 lecturas salteadas por textura y escena, 55 escenas
 * por fotograma). Lo que escribe la propia GPU de vuelta (escenas copiadas a memoria) también
 * cuenta: son escrituras normales. Desactivado por defecto: MLB15_WRITEWATCH=1. */
static uint8_t *g_ww_mem;
static uint32_t g_ww_base, g_ww_pages, g_ww_gen = 1;
static size_t g_ww_size;
static uint32_t *g_ww_pgen;
static void **g_ww_addrs;
static int g_ww_on = -1;
static unsigned g_ww_frame = ~0u;
static void ww_init(void) {
    g_ww_on = 0;
    /* Desactivado por defecto (medido: la huella de texturas ya era barata; sin beneficio no compensa que
     * una escritura del juego a mitad de fotograma se vea un fotograma tarde). MLB15_WRITEWATCH=1 lo activa. */
    if (!env_cached("MLB15_WRITEWATCH")) return;
    vita_mem_window(&g_ww_mem, &g_ww_base, &g_ww_size);
    if (!g_ww_mem || !g_ww_size) return;
    ULONG_PTR n = 1; ULONG gran = 0; void *a1;
    if (GetWriteWatch(0, g_ww_mem, 4096, &a1, &n, &gran) != 0 || gran != 4096) { printf("[gpu] sin vigilancia de escrituras\n"); return; }
    g_ww_pages = (uint32_t)((g_ww_size + 4095) / 4096);
    g_ww_pgen = (uint32_t *)calloc(g_ww_pages, sizeof(uint32_t));
    g_ww_addrs = (void **)malloc((size_t)g_ww_pages * sizeof(void *));
    if (!g_ww_pgen || !g_ww_addrs) return;
    g_ww_on = 1;
    printf("[gpu] vigilancia de escrituras: %u páginas\n", g_ww_pages);
}
static void ww_scan(void) {   /* una vez por fotograma */
    ULONG_PTR n = g_ww_pages; ULONG gran = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_ww_mem, g_ww_size, g_ww_addrs, &n, &gran) != 0) { g_ww_on = 0; return; }
    g_ww_gen++;
    for (ULONG_PTR i = 0; i < n; i++) g_ww_pgen[((uint8_t *)g_ww_addrs[i] - g_ww_mem) >> 12] = g_ww_gen;
}
/* Escrituras del propio port a mitad de fotograma (escenas copiadas de vuelta a la memoria): se
 * apuntan a mano para que una textura que lea esa memoria se vuelva a mirar en el acto. */
static void ww_marca(uint32_t addr, uint32_t bytes) {
    if (g_ww_on <= 0 || !bytes || addr < g_ww_base || (uint64_t)addr - g_ww_base + bytes > g_ww_size) return;
    g_ww_gen++;
    for (uint32_t p = (addr - g_ww_base) >> 12, p1 = (addr - g_ww_base + bytes - 1) >> 12; p <= p1; p++) g_ww_pgen[p] = g_ww_gen;
}
/* 1 si nadie escribió en [addr, addr+bytes) desde la generación gen. Windows se consulta una vez
 * por textura y fotograma (*visto): una escritura de la CPU del juego a mitad de fotograma en una
 * textura ya mirada se ve en el fotograma siguiente; las del port (ww_marca) en el acto. */
static int ww_limpio(uint32_t addr, uint32_t bytes, uint32_t gen, uint32_t *visto) {
    if (g_ww_on < 0) ww_init();
    if (g_ww_on <= 0 || !gen || !bytes || addr < g_ww_base || (uint64_t)addr - g_ww_base + bytes > g_ww_size) return 0;
    extern unsigned port_display_frames(void);
    unsigned fr = port_display_frames();
    if (fr != g_ww_frame) { g_ww_frame = fr; ww_scan(); if (g_ww_on <= 0) return 0; }
    uint32_t p0 = (addr - g_ww_base) >> 12, p1 = (addr - g_ww_base + bytes - 1) >> 12;
    for (uint32_t p = p0; p <= p1; p++) if (g_ww_pgen[p] > gen) return 0;
    if (*visto == fr + 1u) return 1;
    ULONG_PTR n = 1; ULONG gran = 0; void *a1;
    uint8_t *ini = g_ww_mem + ((size_t)p0 << 12);
    if (GetWriteWatch(0, ini, (size_t)(p1 - p0 + 1) << 12, &a1, &n, &gran) != 0) return 0;
    if (n == 0) *visto = fr + 1u;
    return n == 0;
}
static unsigned long g_ww_si, g_ww_no;

static uint32_t content_hash(const GpuTexture *t) {
    uint32_t size = t->type == GPU_TEX_CUBE ? cube_face_bytes(t) * 6u : tex_data_size(t);
    const uint8_t *p = size ? (const uint8_t *)vita_mem_ptr(t->data, size) : NULL;
    uint32_t h = 2166136261u ^ size;
    if (!p) return h;
    uint32_t step = size > 4096 ? size / 1024 : 4;
    for (uint32_t i = 0; i + 4 <= size; i += step) { uint32_t v; memcpy(&v, p + i, 4); h = (h ^ v) * 16777619u; }
    return h;
}

static void barrier_lv(VkCommandBuffer cb, VkImage img, uint32_t level, uint32_t count, VkImageLayout from, VkImageLayout to,
                       VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from; b.newLayout = to;
    b.srcAccessMask = sa; b.dstAccessMask = da;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = level; b.subresourceRange.levelCount = count;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

/* MLB15_MIPS=0: texturas sin mipmaps (como antes). */
static int mips_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = env_cached("MLB15_MIPS"); on = e ? (*e != '0') : 1; }
    return on;
}

static int upload_rgba(Texture *x, const uint32_t *rgba, uint32_t w, uint32_t h);
static int upload_f16(Texture *x, const GpuTexture *t);
static int upload_cube(Texture *x, const GpuTexture *t);
static int upload(Texture *x, const GpuTexture *t) {
    size_t px = (size_t)t->width * t->height;
    if (px > g_decode_cap) {
        uint32_t *p = (uint32_t *)realloc(g_decode, px * 4);
        if (!p) return 0;
        g_decode = p; g_decode_cap = px;
    }
    tex_decode(t, g_decode);
    texrep_volcar(t, g_decode);
    {   /* MLB15_TEX_DUMP=1: cada textura subida a textures\ como BMP (para revisar el decodificador). */
        static int dump = -1;
        if (dump < 0) { const char *e = env_cached("MLB15_TEX_DUMP"); dump = e ? atoi(e) : 0; }
        if (dump) {   /* =2: con número de subida y escena (ver el orden) */
            char path[260];
            static unsigned seq;
            CreateDirectoryA("textures", NULL);
            if (dump == 2)
                snprintf(path, sizeof(path), "textures\\%05u_e%06u_tex_%08X_%ux%u_%08X.bmp", seq++, g_stat_scenes, t->data, t->width,
                         t->height, t->format);
            else
                snprintf(path, sizeof(path), "textures\\tex_%08X_%ux%u_%08X.bmp", t->data, t->width, t->height, t->format);
            FILE *fh = fopen(path, "wb");
            if (fh) {
                uint32_t w = t->width, h = t->height, sz = 54 + w * h * 4;
                uint8_t hd[54] = { 'B', 'M' };
                memcpy(hd + 2, &sz, 4);
                hd[10] = 54; hd[14] = 40;
                int32_t ih = -(int32_t)h;
                memcpy(hd + 18, &w, 4); memcpy(hd + 22, &ih, 4);
                hd[26] = 1; hd[28] = 32;
                fwrite(hd, 1, 54, fh);
                for (size_t i = 0; i < px; i++) {
                    uint32_t c = g_decode[i];
                    uint8_t bgra[4] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, (uint8_t)(c >> 24) };
                    fwrite(bgra, 1, 4, fh);
                }
                fclose(fh);
            }
        }
    }
    return upload_rgba(x, g_decode, t->width, t->height);
}

/* Bytes de una cara de un cubemap GXM con todos sus niveles: las caras van seguidas
 * (+X, -X, +Y, -Y, +Z, -Z). En la camiseta, CubeGloss es un cubo 8x8 por jugador cada 0x600. */
static uint32_t cube_face_bytes(const GpuTexture *t) {
    uint32_t n = 0, levels = t->mips ? t->mips : 1;
    for (uint32_t l = 0; l < levels; l++) {
        GpuTexture m = *t;
        m.width = (uint16_t)(t->width >> l ? t->width >> l : 1);
        m.height = (uint16_t)(t->height >> l ? t->height >> l : 1);
        m.type = GPU_TEX_SWIZZLED;
        n += tex_data_size(&m);
    }
    return n;
}

/* Cubemap (CubeGloss: luz ambiente y brillo de los jugadores): las 6 caras a las 6 capas.
 * Antes se subía la primera cara como textura 2D y el shader, que declara samplerCube, la
 * leía mal: sin luz ambiente, el lado que no da al sol salía negro. */
static int upload_cube(Texture *x, const GpuTexture *t) {
    size_t px = (size_t)t->width * t->height;
    Subida sb;
    if (!subida_begin(px * 4 * 6, &sb)) return 0;
    if (px > g_decode_cap) {
        uint32_t *p = (uint32_t *)realloc(g_decode, px * 4);
        if (!p) return 0;
        g_decode = p; g_decode_cap = px;
    }
    uint32_t fb = cube_face_bytes(t);
    for (uint32_t f = 0; f < 6; f++) {
        GpuTexture face = *t;
        face.type = GPU_TEX_SWIZZLED;
        face.data = t->data + f * fb;
        tex_decode(&face, g_decode);
        memcpy(sb.ptr + px * 4 * f, g_decode, px * 4);
    }
    g_once_quien = "cubo"; VkCommandBuffer cb = sb.cb;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = x->img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 6;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy r[6];
    memset(r, 0, sizeof(r));
    for (uint32_t f = 0; f < 6; f++) {
        r[f].bufferOffset = sb.off + px * 4 * f;
        r[f].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r[f].imageSubresource.baseArrayLayer = f;
        r[f].imageSubresource.layerCount = 1;
        r[f].imageExtent.width = t->width; r[f].imageExtent.height = t->height; r[f].imageExtent.depth = 1;
    }
    vkCmdCopyBufferToImage(cb, sb.buf, x->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, r);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    subida_end(&sb);
    return 1;
}

/* F16F16F16F16: los medio-flotantes tal cual a una imagen R16G16B16A16_SFLOAT (un nivel). */
static int upload_f16(Texture *x, const GpuTexture *t) {
    size_t bytes = (size_t)t->width * t->height * 8;
    Subida sb;
    if (!subida_begin(bytes, &sb)) return 0;
    tex_decode_f16(t, (uint16_t *)sb.ptr);
    g_once_quien = "f16"; VkCommandBuffer cb = sb.cb;
    barrier_lv(cb, x->img, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy r = { 0 };
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = t->width; r.imageExtent.height = t->height; r.imageExtent.depth = 1;
    r.bufferOffset = sb.off;
    vkCmdCopyBufferToImage(cb, sb.buf, x->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    barrier_lv(cb, x->img, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    subida_end(&sb);
    return 1;
}

/* Sube w x h RGBA al nivel 0 de x (del tamaño x->iw x x->ih) y genera los mipmaps. */
static int upload_rgba(Texture *x, const uint32_t *rgba, uint32_t w, uint32_t h) {
    size_t px = (size_t)w * h;
    Subida sb;
    if (!subida_begin(px * 4, &sb)) return 0;
    memcpy(sb.ptr, rgba, px * 4);
    g_once_quien = "rgba"; VkCommandBuffer cb = sb.cb;
    uint32_t levels = x->levels ? x->levels : 1;
    barrier_lv(cb, x->img, 0, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy r = { 0 };
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = w; r.imageExtent.height = h; r.imageExtent.depth = 1;
    r.bufferOffset = sb.off;
    vkCmdCopyBufferToImage(cb, sb.buf, x->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    /* Mipmaps: cada nivel es el anterior a la mitad (filtro lineal). Sin ellos el césped,
     * el público y las líneas tiemblan y parpadean al alejarse. */
    int32_t mw = (int32_t)w, mh = (int32_t)h;
    for (uint32_t l = 1; l < levels; l++) {
        barrier_lv(cb, x->img, l - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
        int32_t nw = mw > 1 ? mw / 2 : 1, nh = mh > 1 ? mh / 2 : 1;
        VkImageBlit b = { 0 };
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.mipLevel = l - 1; b.srcSubresource.layerCount = 1;
        b.dstSubresource = b.srcSubresource; b.dstSubresource.mipLevel = l;
        b.srcOffsets[1].x = mw; b.srcOffsets[1].y = mh; b.srcOffsets[1].z = 1;
        b.dstOffsets[1].x = nw; b.dstOffsets[1].y = nh; b.dstOffsets[1].z = 1;
        vkCmdBlitImage(cb, x->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, x->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                       VK_FILTER_LINEAR);
        barrier_lv(cb, x->img, l - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        mw = nw; mh = nh;
    }
    barrier_lv(cb, x->img, levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    subida_end(&sb);
    return 1;
}

/* Componentes de la vista de una superficie leída como textura: el swizzle GXM
 * de la textura (mismas tablas que texdec.c). 0 si el formato no encaja. */
static int target_mapping(const Target *g, uint32_t fmt, VkComponentMapping *m) {
    uint32_t base = fmt >> 24, swz = (fmt >> 12) & 0xF;
    const VkComponentSwizzle R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G, B = VK_COMPONENT_SWIZZLE_B,
                             A = VK_COMPONENT_SWIZZLE_A, Z = VK_COMPONENT_SWIZZLE_ZERO, O = VK_COMPONENT_SWIZZLE_ONE;
    static const VkComponentSwizzle s4[8][4] = {
        { 0, 1, 2, 3 }, { 2, 1, 0, 3 }, { 3, 2, 1, 0 }, { 1, 2, 3, 0 }, { 0, 1, 2, 9 }, { 2, 1, 0, 9 }, { 3, 2, 1, 9 }, { 1, 2, 3, 9 } };
    VkComponentSwizzle o[4];
    if (((g->tf == 0 || g->tf == 5) && base == 0x0C) || (g->tf == 2 && base == 0x1B)) {   /* U8U8U8U8 / F16F16F16F16 */
        const VkComponentSwizzle ch[4] = { R, G, B, A };
        for (int i = 0; i < 4; i++) o[i] = s4[swz & 7][i] == 9 ? O : ch[s4[swz & 7][i]];
    } else if (g->tf == 1 && base == 0x00) {                                       /* U8 */
        /* como Vita3K (gxm_to_vulkan.cpp): R, 000R=(R,0,0,0), 111R, RRRR, 0RRR=(R,R,R,0), 1RRR=(R,R,R,1),
         * R000=(0,0,0,R), R111. (texdec.c los tiene al revés para el shader genérico.) */
        static const VkComponentSwizzle s1[8][4] = {
            { 1, 0, 0, 2 }, { 1, 0, 0, 0 }, { 1, 2, 2, 2 }, { 1, 1, 1, 1 }, { 1, 1, 1, 0 }, { 1, 1, 1, 2 }, { 0, 0, 0, 1 }, { 2, 2, 2, 1 } };
        const VkComponentSwizzle v[3] = { Z, R, O };
        for (int i = 0; i < 4; i++) o[i] = v[s1[swz & 7][i]];
    } else if (g->tf == 2 && base == 0x1F) {                                       /* F16x4 leída como U32U32 */
        o[0] = R; o[1] = G; o[2] = Z; o[3] = O;
    } else if (g->tf == 4 && base == 0x13) {                                       /* F32 */
        o[0] = R; o[1] = Z; o[2] = Z; o[3] = O;
    } else {
        return 0;
    }
    m->r = o[0]; m->g = o[1]; m->b = o[2]; m->a = o[3];
    return 1;
}

static int base_is_u8x4(uint32_t fmt) { return (fmt >> 24) == 0x0C; }

/* Texturas F32: el mapa de sombras de los jugadores (ShadowMapTexture), la profundidad de
 * una escena solo de profundidad. Antes se leía la memoria guest, que la GPU nunca escribe
 * (F32 sin decodificar): manchas oscuras en la ropa y jugadores a la sombra en planos enteros.
 * Activada por defecto desde el 2026-11-06 ([video] sombra_jugadores, MLB15_SOMBRA_JUGADORES=0
 * la quita: textura en blanco = sin sombra propia). El 1-nov se desactivó porque el bateador
 * salía medio negro, pero eso eran los cubemaps de luz ambiente (arreglados después): medido en
 * todos los planos de bateo, brillo del uniforme 146 sin sombra, 138 con ella y 126 en la Vita
 * real (55 % de zonas en sombra en la Vita, 52 % con ella, 48 % sin ella). */
static int sombra_jugadores_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = env_cached("MLB15_SOMBRA_JUGADORES");
        on = e ? atoi(e) != 0 : launcher_ini_int("video", "sombra_jugadores", 1) != 0;
    }
    return on;
}
int gpu_sombra_jugadores(void) { return sombra_jugadores_on(); }
static Texture *depth_texture(const GpuTexture *t) {
    int on = sombra_jugadores_on();
    if ((t->format >> 24) != 0x13 || !t->data) return NULL;
    if (!on) return g_white;
    Target *g = NULL;
    for (int i = 0; i < MAX_TARGETS; i++)
        if (g_targets[i].img && g_targets[i].dsaddr == t->data && (!g || g_targets[i].stamp > g->stamp)) g = &g_targets[i];
    if (!g || !g->dimg || g == g_cur) return NULL;
    if (!g->dtx) {
        Texture *x = (Texture *)calloc(1, sizeof(Texture));
        if (!x) return NULL;
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = g->dimg;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = g_depth_fmt;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(g_dev, &vi, NULL, &x->view) != VK_SUCCESS) { free(x); return NULL; }
        x->img = g->dimg;
        x->data = t->data; x->format = t->format; x->w = t->width; x->h = t->height;
        g->dtx = x;
        printf("[gpu] profundidad de 0x%08X (%ux%u) como textura 0x%08X %ux%u\n", g->addr, g->w, g->h, t->data, t->width, t->height);
        fflush(stdout);
    }
    /* La primera vez su escena ya terminó sin pasar a lectura: se pasa ahora. */
    if (!g->ds_lectura) {
        g_once_quien = "otro1061"; VkCommandBuffer cb = once_begin();
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = g->dimg;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        once_end();
        g->ds_lectura = 1;
    }
    return g->dtx;
}

static Texture *target_texture(const GpuTexture *t) {
    Texture *dx = depth_texture(t);
    if (dx) return dx;
    if (!g_tgt_filtro[(t->data >> 4) & 4095]) return NULL;
    static int traza = -1;
    if (traza < 0) traza = env_cached("MLB15_RT_TRAZA") ? 1 : 0;
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *g = &g_targets[i];
        if (traza && g->img && g->addr == t->data) {
            static int n;
            if (n++ < 40) printf("[rt] textura 0x%08X %ux%u fmt 0x%08X tipo %u -> superficie %ux%u tf %d residente %d dibujada %d\n",
                                 t->data, t->width, t->height, t->format, t->type, g->w, g->h, g->tf, g->resident, g->drawn);
        }
        if (!g->img || !g->drawn || g == g_cur || g->addr != t->data || g->w != t->width || g->h != t->height) continue;
        /* Lineal: antes se leía siempre de la memoria guest (ya correcta tras la copia de vuelta),
         * lo que suponía decodificar y volver a subir a la GPU en cada fotograma lo que la GPU
         * acababa de dibujar (~5 subidas por fotograma en el partido). Ahora se usa la imagen de
         * la GPU mientras la memoria no cambie (comprobación de más abajo, una vez por escena).
         * MLB15_LINEAL_MEMORIA=1 vuelve a leer de la memoria. */
        static int lineal_mem = -1;
        if (lineal_mem < 0) lineal_mem = env_cached("MLB15_LINEAL_MEMORIA") ? 1 : 0;
        if (!g->resident && lineal_mem) { if (g->pend) CTXC(&g_ctx[g->pend - 1], 0); return NULL; }
        {   static int tr = -1; static unsigned nn;
            if (tr < 0) tr = env_cached("MLB15_MAPAS_TRAZA") ? 1 : 0;
            if (tr && g->tf == 1 && nn++ % 3000 == 0)
                printf("[mapa] 0x%08X %ux%u draws %u u %u v %u min %u fmt 0x%08X\n", g->addr, g->w, g->h, g->ndraws, t->u_addr, t->v_addr, t->min_linear, t->format);
        }
        static int sombra_tr = -1, sombra_vacia = -1;
        if (sombra_tr < 0) { sombra_tr = env_cached("MLB15_SOMBRA_TRAZA") ? 1 : 0; sombra_vacia = env_cached("MLB15_SOMBRA_BLANCA") ? 0 : 1; }
        if (g->tf == 1 && g->w >= 2048 && sombra_tr) {
            static int n;
            if (n++ % 500 == 0) printf("[sombra] mapa 0x%08X %ux%u draws %u fmt 0x%08X\n", g->addr, g->w, g->h, g->ndraws, t->format);
        }
        /* Mapa U8 de 2048x2048 que solo se borra a 0 (unidad «Texture3» de StadiumLines/StadiumDirt).
         * Antes se sustituía por blanco porque en 0 dejaba cuñas negras junto al plato, pero esas cuñas
         * eran los registros sin inicializar del traductor (arreglado el 31-oct). Desde que las rayas
         * de cal tienen su mezcla real (alfa = alfa_textura * (1 - este mapa)) el blanco las borraba:
         * ahora se deja en 0. MLB15_SOMBRA_BLANCA=1 vuelve al blanco. */
        if (g->tf == 1 && g->ndraws <= 1 && g->w >= 2048 && g_white && !sombra_vacia) return g_white;
        /* Mapas U8 de 1024 que la GPU solo borra (1 draw; sombras del estadio 0xA58DC960/0xA59DC970):
         * su contenido es el de la GPU. Esa memoria guest se reutiliza para otras cosas, guest_hash
         * cambiaba algunos fotogramas y se pasaba a leer la memoria: la tierra junto al plato salía
         * negra a ratos (el «parpadeo»). Medido: con la memoria, negro en el 100 % de los fotogramas
         * de bateo. MLB15_MAPAS_MEMORIA=1 vuelve a lo anterior. */
        static int mapas_mem = -1;
        if (mapas_mem < 0) mapas_mem = env_cached("MLB15_MAPAS_MEMORIA") ? 1 : 0;
        int fijo = g->tf == 1 && g->w == 1024 && g->ndraws <= 1 && !mapas_mem;
        if (!fijo && !g->pend && g->hscene != g_stat_scenes && (g->hscene = g_stat_scenes, guest_hash(g->addr, g->gbytes) != g->ghash)) {   /* una vez por escena */
            g->drawn = 0;                   /* la CPU reutilizó la memoria */
            static unsigned nsalto;
            if (nsalto++ < 60) printf("[rt] 0x%08X %ux%u tf %d draws %u: la memoria cambió, se lee de la memoria\n", g->addr, g->w, g->h,
                                      g->tf, g->ndraws);
            return NULL;
        }
        VkComponentMapping m;
        if (!target_mapping(g, t->format, &m)) return NULL;
        /* Una F16x4 que se lee como U32U32 guarda datos empaquetados (atlas del público, NaN
         * incluidos): desde su próxima escena se escribe con los bits crudos (U16x4). */
        if (g->tf == 2 && (t->format >> 24) == 0x1F) g->want_raw16 = 1;
        uint32_t key = (t->format & 0xFF00F000u) | (t->gamma ? 1u : 0u);
        for (int k = 0; k < g->ntx; k++) if (g->txkey[k] == key) return g->tx[k];
        if (g->ntx >= 4) return g->tx[0];
        Texture *x = (Texture *)calloc(1, sizeof(Texture));
        if (!x) return NULL;
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = g->img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = g_tfmt_vk[g->tf];
        if ((g->tf == 0 || g->tf == 5) && base_is_u8x4(t->format))   /* la textura decide si es sRGB */
            vi.format = t->gamma ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        if (g->tf == 2 && (t->format >> 24) == 0x1F)   /* bits tal cual (Vita3K usa R32G32_UINT) */
            vi.format = VK_FORMAT_R32G32_SFLOAT;
        vi.components = m;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(g_dev, &vi, NULL, &x->view) != VK_SUCCESS) { free(x); return NULL; }
        x->img = g->img;
        x->data = t->data; x->format = t->format; x->w = t->width; x->h = t->height;
        g->txkey[g->ntx] = key;
        g->tx[g->ntx++] = x;
        return x;
    }
    return NULL;
}

/* Perfil (MLB15_PERFIL=1): tiempo en búsqueda de texturas y en esperas a la GPU. */
double g_prof_tex, g_prof_wait, g_prof_v3k;
unsigned g_drop_lleno, g_drop_vb, g_drop_ds, g_vb_max, g_ib_max, g_mb_max, g_up_max;   /* draws descartados (escena llena) */
static uint32_t g_nubt;   /* relectura tardía de uniformes (más abajo) */
static uint8_t *g_mask;
static size_t g_mask_cap, g_mask_n;
void gpu_mascara_guarda(const char *path) {
    if (!g_mask_n) return;
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(g_mask, 1, g_mask_n, f); fclose(f); }
}
static double prof_now(void) {
    static double inv;
    if (inv == 0.0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); inv = 1.0 / (double)f.QuadPart; }
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * inv;
}
static Texture *texture_get_(const GpuTexture *t);
static Texture *texture_get(const GpuTexture *t) {
    double t0 = prof_now();
    Texture *x = texture_get_(t);
    g_prof_tex += prof_now() - t0;
    return x;
}
/* Imágenes sustituidas (una textura que pasa a tener otro tamaño): se destruyen al empezar
 * la escena siguiente, cuando la GPU ya no las usa. */
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; } Dead;
static Dead g_dead[256];
static uint32_t g_ndead;
static void dead_flush(void) {
    for (uint32_t i = 0; i < g_ndead; i++) {
        vkDestroyImageView(g_dev, g_dead[i].view, NULL);
        vkDestroyImage(g_dev, g_dead[i].img, NULL);
        vkFreeMemory(g_dev, g_dead[i].mem, NULL);
    }
    g_ndead = 0;
}

/* (Re)crea la imagen de x a w x h, con mipmaps si la textura los tiene. */
static int tex_image(Texture *x, const GpuTexture *t, uint32_t w, uint32_t h) {
    if (x->img) {
        if (g_ndead == 256) { vkDeviceWaitIdle(g_dev); dead_flush(); }
        g_dead[g_ndead].img = x->img; g_dead[g_ndead].mem = x->mem; g_dead[g_ndead].view = x->view; g_ndead++;
        x->img = VK_NULL_HANDLE;
        memset(x->set, 0, sizeof(x->set));   /* (los conjuntos viejos se quedan en el pool) */
    }
    if (t->type == GPU_TEX_CUBE) {   /* 6 caras, un nivel, vista CUBE (CubeGloss) */
        VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = t->gamma ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent.width = w; ii.extent.height = h; ii.extent.depth = 1;
        ii.mipLevels = 1; ii.arrayLayers = 6;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (vkCreateImage(g_dev, &ii, NULL, &x->img) != VK_SUCCESS) { x->img = VK_NULL_HANDLE; return 0; }
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(g_dev, x->img, &mr);
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(g_dev, &ai, NULL, &x->mem) != VK_SUCCESS) return 0;
        vkBindImageMemory(g_dev, x->img, x->mem, 0);
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = x->img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        vi.format = ii.format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 6;
        if (vkCreateImageView(g_dev, &vi, NULL, &x->view) != VK_SUCCESS) return 0;
        x->levels = 1;
        x->iw = w; x->ih = h;
        return 1;
    }
    int f16 = (t->format >> 24) == 0x1B;   /* F16F16F16F16: datos, en coma flotante y sin mipmaps */
    uint32_t levels = 1;
    if (t->mips > 1 && mips_on() && !f16)
        for (uint32_t m = w > h ? w : h; m > 1; m >>= 1) levels++;
    if (!image_new_lv(w, h, f16 ? VK_FORMAT_R16G16B16A16_SFLOAT : t->gamma ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                      levels, &x->img, &x->mem, &x->view)) { x->img = VK_NULL_HANDLE; x->iw = x->ih = 0; return 0; }
    x->levels = levels;
    x->iw = w; x->ih = h;
    return 1;
}

static Texture *white_texture_(uint32_t white, Texture **slot);
#define TEX_BUCKETS 4096
static int g_tex_bucket[TEX_BUCKETS];       /* índice+1 del primer Texture con esa dirección */
static Texture *texture_get_(const GpuTexture *t) {
    {   /* MLB15_TEX_BLANCA=dir,dir,... (hex): esas texturas en blanco (aislar fallos) */
        static const char *bl = (const char *)1;
        if (bl == (const char *)1) bl = env_cached("MLB15_TEX_BLANCA");
        if (bl && g_white) {
            char key[16];
            snprintf(key, sizeof(key), "%08X", t->data);
            if (strstr(bl, key)) return g_white;
        }
        static const char *ng = (const char *)1;
        if (ng == (const char *)1) ng = env_cached("MLB15_TEX_NEGRA");
        if (ng && g_black) {
            char key[16];
            snprintf(key, sizeof(key), "%08X", t->data);
            if (strstr(ng, key)) return g_black;
        }
    }
    if ((t->data & 0xFF000000u) == 0xFE000000u) {   /* MLB15_TEX_NOMBRE_COLOR (gxm.c) */
        static Texture *solid[16];
        uint32_t i = t->data & 15;
        if (!solid[i]) {
            const char *q = env_cached("MLB15_TEX_NOMBRE_COLOR");
            for (uint32_t k = 0; q && k < i; k++) { q = strchr(q, ','); if (q) q++; }
            const char *eq = q ? strchr(q, '=') : NULL;
            uint32_t v = eq ? (uint32_t)strtoul(eq + 1, NULL, 16) : 0xFFFFFFFFu;
            uint32_t rgba = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);   /* RRGGBBAA -> bytes R,G,B,A */
            white_texture_(rgba, &solid[i]);
        }
        return solid[i];
    }
    Texture *x = target_texture(t);
    if (x) return x;
    uint32_t bk = (t->data >> 4) * 2654435761u >> 20 & (TEX_BUCKETS - 1);
    for (int i = g_tex_bucket[bk]; i; i = g_tex[i - 1].next) {
        Texture *c = &g_tex[i - 1];
        if (c->data == t->data && c->format == t->format && c->w == t->width && c->h == t->height &&
            c->type == t->type && c->stride == t->stride && c->palette == t->palette && c->gamma == t->gamma) { x = c; break; }
    }
    /* El contenido se comprueba una vez por escena (la CPU no reescribe texturas a mitad de una). */
    if (x && x->scene == g_stat_scenes) return x;
    pend_overlap(t->data, t->width * t->height * 8u * (t->type == GPU_TEX_CUBE ? 6u : 1u));
    uint32_t tbytes = t->type == GPU_TEX_CUBE ? cube_face_bytes(t) * 6u : tex_data_size(t);
    if (x && ww_limpio(t->data, tbytes, x->ww_gen, &x->ww_frame)) { x->scene = g_stat_scenes; g_ww_si++; return x; }   /* nadie la tocó */
    g_ww_no++;
    uint32_t gen_antes = g_ww_gen;
    uint32_t hash = content_hash(t);
    if (!x) {
        if (g_ntex >= MAX_TEXTURES) return NULL;
        x = &g_tex[g_ntex];
        memset(x, 0, sizeof(*x));
        if (!tex_image(x, t, t->width, t->height)) return NULL;
        g_ntex++;
        x->gamma = t->gamma;
        x->data = t->data; x->format = t->format; x->w = t->width; x->h = t->height;
        x->type = t->type; x->stride = t->stride; x->palette = t->palette;
        x->hash = hash ^ 1u;                                   /* fuerza la primera subida */
        x->next = g_tex_bucket[bk];
        g_tex_bucket[bk] = (int)g_ntex;                        /* g_ntex ya incrementado: índice+1 */
    }
    x->scene = g_stat_scenes;
    if (x->hash != hash) {
        static int tr = -1;   /* MLB15_SUBIDAS_TRAZA=1: texturas que se vuelven a subir porque cambió su memoria */
        if (tr < 0) tr = env_cached("MLB15_SUBIDAS_TRAZA") ? 1 : 0;
        if (tr && x->hash != (hash ^ 1u)) printf("[subida] 0x%08X %ux%u fmt 0x%08X escena %u\n", t->data, t->width, t->height, t->format, g_stat_scenes);
        uint32_t rw = 0, rh = 0, *hd = t->type == GPU_TEX_CUBE ? NULL : texrep_find(t, &rw, &rh);   /* mejorada (texrep.c) */
        uint32_t iw = hd ? rw : t->width, ih = hd ? rh : t->height;
        if ((x->iw != iw || x->ih != ih) && !tex_image(x, t, iw, ih)) { free(hd); return NULL; }
        if (hd) { upload_rgba(x, hd, rw, rh); free(hd); }
        else if (t->type == GPU_TEX_CUBE) upload_cube(x, t);
        else if ((t->format >> 24) == 0x1B) upload_f16(x, t);
        else upload(x, t);
        x->hash = hash;
    }
    x->ww_gen = g_ww_on > 0 ? gen_antes : 0;
    x->ww_frame = 0;
    if (env_cached("MLB15_WW_ESTAD") && (g_ww_si + g_ww_no) % 20000 == 0)
        printf("[ww] texturas sin mirar %lu, con huella %lu\n", g_ww_si, g_ww_no);
    return x;
}

static VkDescriptorSet texture_set(const GpuTexture *t) {
    uint32_t si;
    VkSampler smp = sampler_for(t, &si);
    Texture *x = texture_get(t);
    if (!x) return g_white ? g_white->set[0] : VK_NULL_HANDLE;
    if (!x->set[si]) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = g_dpool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g_dsl;
        if (vkAllocateDescriptorSets(g_dev, &ai, &x->set[si]) != VK_SUCCESS) return g_white->set[0];
        VkDescriptorImageInfo ii = { smp, x->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = x->set[si];
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(g_dev, 1, &w, 0, NULL);
    }
    return x->set[si];
}

static Texture *white_texture_(uint32_t white, Texture **slot);
static Texture *white_texture(void) {
    if (!g_black && env_cached("MLB15_TEX_NEGRA")) white_texture_(0, &g_black);
    return white_texture_(0xFFFFFFFFu, &g_white);
}
static Texture *white_texture_(uint32_t white, Texture **slot) {
    if (*slot) return *slot;
    /* Una textura 1x1 blanca para los draws sin textura (el layout lleva sampler). */
    Texture *x = &g_tex[g_ntex++];
    memset(x, 0, sizeof(*x));
    if (!image_new(1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &x->img, &x->mem, &x->view))
        return NULL;
    memcpy(g_tex_staging.ptr, &white, 4);
    g_once_quien = "blanca"; VkCommandBuffer cb = once_begin();
    barrier(cb, x->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy r = { 0 };
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = 1; r.imageExtent.height = 1; r.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, g_tex_staging.buf, x->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    barrier(cb, x->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    once_end();
    GpuTexture t;
    memset(&t, 0, sizeof(t));
    uint32_t si;
    VkSampler smp = sampler_for(&t, &si);
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = g_dpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_dsl;
    vkAllocateDescriptorSets(g_dev, &ai, &x->set[0]);
    VkDescriptorImageInfo ii = { smp, x->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = x->set[0];
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(g_dev, 1, &w, 0, NULL);
    *slot = x;
    return x;
}

static VkBlendFactor factor(uint8_t f) {
    static const VkBlendFactor m[12] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_DST_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_ONE };
    return f < 12 ? m[f] : VK_BLEND_FACTOR_ONE;
}
static VkBlendOp blend_op(uint8_t f) {
    switch (f) {
        case 2: return VK_BLEND_OP_SUBTRACT;
        case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 4: return VK_BLEND_OP_MIN;
        case 5: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

static VkPipeline pipeline_for_l(const GpuDraw *d, uint16_t vsm, uint16_t fsm, uint8_t nattr, uint8_t v3k, uint16_t uint_mask, uint16_t vlay);
static VkPipeline pipeline_for(const GpuDraw *d, uint16_t vsm, uint16_t fsm, uint8_t nattr, uint8_t v3k, uint16_t uint_mask) {
    return pipeline_for_l(d, vsm, fsm, nattr, v3k, uint_mask, 0);
}
static VkPipeline pipeline_for_l(const GpuDraw *d, uint16_t vsm, uint16_t fsm, uint8_t nattr, uint8_t v3k, uint16_t uint_mask, uint16_t vlay) {
    PipeKey key;
    memset(&key, 0, sizeof(key));
    key.vs = vsm; key.fs = fsm; key.nattr = nattr; key.v3k = v3k; key.uint_mask = uint_mask; key.vlay = vlay;
    key.tf = g_cur ? (g_cur->raw16 ? TF_RAW16 : g_cur->tf) : 0;
    key.blend = d->blend; key.color_func = d->color_func; key.alpha_func = d->alpha_func;
    key.color_src = d->color_src; key.color_dst = d->color_dst;
    key.alpha_src = d->alpha_src; key.alpha_dst = d->alpha_dst; key.color_mask = d->color_mask;
    key.cull = d->cull; key.depth_func = d->depth_func; key.depth_write = d->depth_write;
    key.stencil = d->stencil; key.color_off = d->color_off;
    for (int k = 0; k < 2; k++) {
        key.st[k][0] = d->st[k].func; key.st[k][1] = d->st[k].fail;
        key.st[k][2] = d->st[k].zfail; key.st[k][3] = d->st[k].pass;
    }
    uint32_t slot = pipe_hash(&key) & (PIPE_HASH - 1);
    for (; g_phash[slot]; slot = (slot + 1) & (PIPE_HASH - 1))
        if (!memcmp(&g_pipes[g_phash[slot] - 1].key, &key, sizeof(key))) return g_pipes[g_phash[slot] - 1].pipe;
    if (g_npipes >= MAX_PIPES) {   /* no debería pasar: avisar en vez de dibujar con otro shader */
        static int told;
        if (!told++) printf("[gpu] caché de pipelines llena (%u)\n", g_npipes);
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                              { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vsm ? g_mods[vsm] : g_vs; st[0].pName = v3k ? "main_vs" : "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fsm ? g_mods[fsm] : g_fs; st[1].pName = v3k ? "main_fs" : "main";
    VkVertexInputBindingDescription vb = { 0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[16] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },  { 1, 0, VK_FORMAT_R32G32_SFLOAT, 16 },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT, 24 },       { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32 } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 4; vi.pVertexAttributeDescriptions = va;
    if (vsm) {   /* shader traducido: nattr vec4 seguidos */
        vb.stride = 16u * (nattr ? nattr : 1);
        for (uint32_t i = 0; i < nattr && i < 16; i++) {
            va[i].location = i; va[i].binding = 0; va[i].offset = 16u * i;
            va[i].format = (uint_mask >> i & 1) ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT;
        }
        vi.vertexAttributeDescriptionCount = nattr;
    }
    VkVertexInputBindingDescription vbs[GPU_VLAY_MAXB + 1];
    if (vsm && vlay) {   /* vértices crudos: un binding por flujo del juego + valores por defecto */
        const GpuVLayout *l = &g_vlays[vlay - 1];
        for (uint32_t b = 0; b < l->nbind; b++) {
            vbs[b].binding = b; vbs[b].stride = l->stride[b];
            vbs[b].inputRate = l->inst[b] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
        }
        vbs[l->nbind].binding = l->nbind; vbs[l->nbind].stride = 0; vbs[l->nbind].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        for (uint32_t i = 0; i < nattr && i < 16; i++) {
            va[i].location = i;
            if (l->loc_bind[i] == 0xFF) { va[i].binding = l->nbind; va[i].offset = 0; va[i].format = VK_FORMAT_R32G32B32A32_SFLOAT; }
            else { va[i].binding = l->loc_bind[i]; va[i].offset = l->loc_off[i]; va[i].format = vfmt_vk(l->loc_fmt[i] >> 4, l->loc_fmt[i] & 15); }
        }
        vi.vertexBindingDescriptionCount = l->nbind + 1u; vi.pVertexBindingDescriptions = vbs;
    }
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.depthClampEnable = g_depth_clamp;   /* el suelo junto a la cámara no se recorta por el plano cercano */
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    /* GXM recorta las caras en sentido horario (CW) o antihorario (CCW) en
     * pantalla; nuestras coordenadas de Vulkan tienen la y hacia abajo como la
     * pantalla de GXM, así que el sentido se conserva. */
    rs.cullMode = d->cull == 1 ? VK_CULL_MODE_BACK_BIT : d->cull == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    rs.depthBiasEnable = VK_TRUE;   /* valor dinámico (0 si el juego no pone sesgo) */
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba = { 0 };
    ba.blendEnable = d->blend && g_cur && !g_cur->raw16 ? VK_TRUE : VK_FALSE;   /* U16x4: sin mezcla */
    ba.srcColorBlendFactor = factor(d->color_src);
    ba.dstColorBlendFactor = factor(d->color_dst);
    ba.colorBlendOp = blend_op(d->color_func);
    ba.srcAlphaBlendFactor = factor(d->alpha_src);
    ba.dstAlphaBlendFactor = factor(d->alpha_dst);
    ba.alphaBlendOp = blend_op(d->alpha_func);
    ba.colorWriteMask = d->color_off ? 0 : ((d->color_mask & 2) ? VK_COLOR_COMPONENT_R_BIT : 0) |
                        ((d->color_mask & 4) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                        ((d->color_mask & 8) ? VK_COLOR_COMPONENT_B_BIT : 0) |
                        ((d->color_mask & 1) ? VK_COLOR_COMPONENT_A_BIT : 0);
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dss.depthTestEnable = (d->depth_func != 7 || d->depth_write) ? VK_TRUE : VK_FALSE;
    dss.depthWriteEnable = d->depth_write ? VK_TRUE : VK_FALSE;
    dss.depthCompareOp = (VkCompareOp)d->depth_func;
    dss.stencilTestEnable = d->stencil ? VK_TRUE : VK_FALSE;
    for (int k = 0; k < 2; k++) {
        VkStencilOpState *o = k ? &dss.back : &dss.front;
        o->failOp = (VkStencilOp)d->st[k].fail;
        o->depthFailOp = (VkStencilOp)d->st[k].zfail;
        o->passOp = (VkStencilOp)d->st[k].pass;
        o->compareOp = (VkCompareOp)d->st[k].func;
        o->compareMask = o->writeMask = 0xFF;               /* dinámicos */
    }
    VkDynamicState dyn[6] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                              VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                              VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 6; ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pi.stageCount = 2; pi.pStages = st;
    pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vps;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pColorBlendState = &cb;
    pi.pDepthStencilState = &dss;
    pi.pDynamicState = &ds; pi.layout = v3k ? g_layout3 : vsm ? g_layout2 : g_layout;
    pi.renderPass = key.tf == TF_RAW16 ? g_pass_raw : g_passf[key.tf];
    VkPipeline p;
    VkResult pr = vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &pi, NULL, &p);
    if (pr != VK_SUCCESS) {
        static int told;
        if (told++ < 8) printf("[gpu] vkCreateGraphicsPipelines = %d (vs %u fs %u nattr %u v3k %u)\n", (int)pr, vsm, fsm, nattr, v3k);
        return VK_NULL_HANDLE;
    }
    g_pipes[g_npipes].key = key;   /* struct */
    g_pipes[g_npipes].pipe = p;
    g_phash[slot] = g_npipes + 1;
    g_npipes++;
    return p;
}

/* --- escenas --------------------------------------------------------------- */

static uint32_t g_guest_stride;

/* Estado dinámico ya grabado en el cmd buffer de la escena: solo se envía lo que cambia
 * (antes ~10 llamadas al driver por draw aunque fueran iguales). Se reinicia en cada escena.
 * MLB15_SIN_CACHE_ESTADO=1 lo quita. */
static struct {
    VkPipeline p; VkViewport vp, full;
    uint32_t cmask[2], wmask[2], ref[2]; float bias[2];
    int st_ok, vp_ok;
    VkDescriptorSet tex; VkPipelineLayout layout;
} g_dyn;
static int dyn_off(void) {
    static int off = -1;
    if (off < 0) off = env_cached("MLB15_SIN_CACHE_ESTADO") ? 1 : 0;
    return off;
}
static void dyn_viewport(const VkViewport *v) {
    if (!dyn_off() && g_dyn.vp_ok && !memcmp(&g_dyn.vp, v, sizeof(*v))) return;
    vkCmdSetViewport(g_cmd, 0, 1, v);
    g_dyn.vp = *v; g_dyn.vp_ok = 1;
}
static void dyn_estado(VkPipeline p, const GpuDraw *d) {
    int off = dyn_off();
    if (off || g_dyn.p != p) { vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p); g_dyn.p = p; }
    if (!off && g_dyn.st_ok && g_dyn.cmask[0] == d->st[0].cmask && g_dyn.cmask[1] == d->st[1].cmask && g_dyn.wmask[0] == d->st[0].wmask &&
        g_dyn.wmask[1] == d->st[1].wmask && g_dyn.ref[0] == d->st[0].ref && g_dyn.ref[1] == d->st[1].ref &&
        g_dyn.bias[0] == d->dbias_units && g_dyn.bias[1] == d->dbias_factor) return;
    vkCmdSetStencilCompareMask(g_cmd, VK_STENCIL_FACE_FRONT_BIT, d->st[0].cmask);
    vkCmdSetStencilCompareMask(g_cmd, VK_STENCIL_FACE_BACK_BIT, d->st[1].cmask);
    vkCmdSetStencilWriteMask(g_cmd, VK_STENCIL_FACE_FRONT_BIT, d->st[0].wmask);
    vkCmdSetStencilWriteMask(g_cmd, VK_STENCIL_FACE_BACK_BIT, d->st[1].wmask);
    vkCmdSetStencilReference(g_cmd, VK_STENCIL_FACE_FRONT_BIT, d->st[0].ref);
    vkCmdSetStencilReference(g_cmd, VK_STENCIL_FACE_BACK_BIT, d->st[1].ref);
    {   /* El sesgo de pendiente se mide en píxeles de la superficie: a resolución interna xN el
         * píxel es N veces más pequeño y el sesgo se quedaba N veces más corto que en la Vita.
         * En el mapa de sombras de los jugadores eso daba «acné» (manchas y rayas oscuras en la
         * ropa, sobre todo en los jugadores lejanos como el lanzador). MLB15_SESGO_RES=0 lo quita. */
        static int por_res = -1;
        if (por_res < 0) { const char *e = env_cached("MLB15_SESGO_RES"); por_res = e ? atoi(e) != 0 : 1; }
        vkCmdSetDepthBias(g_cmd, d->dbias_units, 0.0f, por_res ? d->dbias_factor * (float)g_res : d->dbias_factor);
    }
    g_dyn.cmask[0] = d->st[0].cmask; g_dyn.cmask[1] = d->st[1].cmask; g_dyn.wmask[0] = d->st[0].wmask; g_dyn.wmask[1] = d->st[1].wmask;
    g_dyn.ref[0] = d->st[0].ref; g_dyn.ref[1] = d->st[1].ref; g_dyn.bias[0] = d->dbias_units; g_dyn.bias[1] = d->dbias_factor;
    g_dyn.st_ok = 1;
}

void gpu_scene_begin(uint32_t addr, uint32_t w, uint32_t h, uint32_t stride, int load_ds, uint32_t gxm_format,
                     uint32_t surf_type, uint32_t rw, uint32_t rh) {
    if (!g_ok) return;
    EnterCriticalSection(&g_cs);
    if (!white_texture()) { LeaveCriticalSection(&g_cs); return; }
    int tf = tfmt_from_gxm(gxm_format);
    uint32_t bpp = g_tfmt_bpp[tf];
    if (g_ndead) { g_mot = 3; ctx_complete_all(); dead_flush(); }   /* imágenes viejas: que ninguna escena en vuelo las use */
    Target *t = target_for(addr, w, h, tf);
    if (!t) { LeaveCriticalSection(&g_cs); return; }
    /* Su escena anterior sigue en vuelo (p. ej. el 3D y luego el marcador sobre el mismo búfer):
     * la memoria guest no ha cambiado desde entonces, así que se conserva la imagen sin esperar
     * (antes la espera solo servía para comprobar el hash de la memoria). */
    int pend_keep = t->pend && t->drawn;
    if (t->pend && !pend_keep) CTXC(&g_ctx[t->pend - 1], 1);
    {
        g_ctx_i = (g_ctx_i + 1) % g_nctx;
        GCtx *c = &g_ctx[g_ctx_i];
        CTXC(c, 2);
        g_cmd = c->cmd; g_fence = c->fence; g_vb = c->vb; g_ib = c->ib; g_mb = c->mb; g_dpool2 = c->dp2; g_dpool3 = c->dp3;
        if (g_dset0) g_dset0 = c->dset0;
        c->up_off = 0; c->u_open = 0;
    }
    g_cur = t;
    if (g_next_ds && env_cached("MLB15_DS_TRAZA") && t->dsaddr != g_next_ds)
        printf("[ds] escena 0x%08X %ux%u tf %d: profundidad 0x%08X\n", addr, w, h, tf, g_next_ds);
    if (g_next_ds) {   /* esa profundidad ya no es de otra superficie */
        for (int i = 0; i < MAX_TARGETS; i++) if (g_targets[i].dsaddr == g_next_ds) g_targets[i].dsaddr = 0;
        t->dsaddr = g_next_ds;
    }
    g_next_ds = 0;
    g_guest_stride = stride ? stride : w;
    t->gstride = g_guest_stride;
    t->rw = rw && rw < w ? rw : w;
    t->rh = rh && rh < h ? rh : h;
    /* Superficies swizzled/tiled (sombras, HDR, render a textura): se quedan en
     * la GPU. Las texturas que las leen usan la imagen (texture_get); copiarlas a
     * la memoria guest en lineal machacaba memoria vecina y no servía de nada. */
    t->resident = surf_type != 0;
    int keep = (t->resident && t->drawn) || pend_keep;
    /* Si la memoria guest sigue como la dejó la última escena (nadie la tocó con la CPU),
     * se conserva la imagen de la GPU: recargarla de la memoria, que va a tamaño nativo,
     * borraba la resolución interna. En el partido el juego dibuja el 3D y luego el
     * marcador en otra escena sobre el mismo búfer, y a la ventana llegaba el 3D a 720x408
     * ampliado (pixelado). */
    if (!keep && t->drawn && t->gbytes == g_guest_stride * h * bpp && guest_hash(addr, t->gbytes) == t->ghash) keep = 1;
    g_vb_off = g_ib_off = 0;
    g_mb_off = 0;
    g_nubt = 0;
    vkResetDescriptorPool(g_dev, g_dpool2, 0);
    vkResetDescriptorPool(g_dev, g_dpool3, 0);
    g_texset_esc++;
    g_stat_scenes++;
    {   /* MLB15_ESCENA_TAM=512x256: cada escena sobre superficies de ese tamaño */
        static uint32_t ew = 1, eh;
        if (ew == 1) { const char *e = env_cached("MLB15_ESCENA_TAM"); ew = 0; if (e) sscanf(e, "%ux%u", &ew, &eh); }
        if (ew && w == ew && h == eh)
            printf("[escena-tam] e%06u 0x%08X tf %d keep %d drawn %d res %d\n", g_stat_scenes, addr, tf, keep, t->drawn, t->resident);
    }

    /* Lo que ya hay en la superficie (la escena no borra por sí sola). */
    if (!keep) {
        const uint8_t *src = t->resident ? NULL : (const uint8_t *)vita_mem_ptr(addr, g_guest_stride * h * bpp);
        uint8_t *dst = (uint8_t *)t->staging.ptr;
        for (uint32_t y = 0; y < h; y++) {
            if (src) memcpy(dst + (size_t)y * w * bpp, src + (size_t)y * g_guest_stride * bpp, (size_t)w * bpp);
            else memset(dst + (size_t)y * w * bpp, 0, (size_t)w * bpp);
        }
    }

    vkResetCommandBuffer(g_cmd, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_cmd, &bi);
    if (keep) {   /* conserva lo que dibujó la GPU */
        barrier(g_cmd, t->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    } else {
        barrier(g_cmd, t->simg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy r = { 0 };
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.imageExtent.width = w; r.imageExtent.height = h; r.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(g_cmd, t->staging.buf, t->simg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        if (t->simg != t->img) {   /* resolución interna: ampliar la copia nativa */
            barrier(g_cmd, t->simg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            barrier(g_cmd, t->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageBlit b = { 0 };
            b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1;
            b.dstSubresource = b.srcSubresource;
            b.srcOffsets[1].x = (int32_t)w; b.srcOffsets[1].y = (int32_t)h; b.srcOffsets[1].z = 1;
            b.dstOffsets[1].x = (int32_t)t->sw; b.dstOffsets[1].y = (int32_t)t->sh; b.dstOffsets[1].z = 1;
            vkCmdBlitImage(g_cmd, t->simg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &b, tf == 2 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        }
        barrier(g_cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }
    if (t->want_raw16 && !t->raw16 && tf == 2) {   /* vista U16x4 + framebuffer propio */
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = t->img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R16G16B16A16_UINT;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(g_dev, &vi, NULL, &t->uview) == VK_SUCCESS) {
            VkImageView views[2] = { t->uview, t->dview };
            VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fi.renderPass = g_pass_raw;
            fi.attachmentCount = 2;
            fi.pAttachments = views;
            fi.width = t->sw; fi.height = t->sh; fi.layers = 1;
            if (vkCreateFramebuffer(g_dev, &fi, NULL, &t->fb_raw) == VK_SUCCESS) {
                t->raw16 = 1;
                printf("[gpu] superficie 0x%08X %ux%u: F16x4 con escritura cruda U16x4\n", t->addr, t->w, t->h);
            }
        }
    }
    VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    if (t->raw16) rp.renderPass = (load_ds && t->ds_ready) ? g_pass_raw_load : g_pass_raw;
    else rp.renderPass = (load_ds && t->ds_ready) ? g_passf_load[tf] : g_passf[tf];
    t->ds_ready = 1;
    rp.framebuffer = t->raw16 ? t->fb_raw : t->fb;
    rp.renderArea.extent.width = t->sw;
    rp.renderArea.extent.height = t->sh;
    VkClearValue clears[2];
    memset(clears, 0, sizeof(clears));
    clears[1].depthStencil.depth = 1.0f;
    clears[1].depthStencil.stencil = 0;
    rp.clearValueCount = 2;
    rp.pClearValues = clears;
    if (t->ds_lectura) {   /* se muestreó como textura: vuelve a ser adjunto (se carga o se borra) */
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t->dimg;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0,
                             NULL, 1, &b);
        t->ds_lectura = 0;
    }
    vkCmdBeginRenderPass(g_cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = { 0.0f, 0.0f, (float)t->sw, (float)t->sh, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { t->rw * g_res, t->rh * g_res } };
    vkCmdSetViewport(g_cmd, 0, 1, &vp);
    vkCmdSetScissor(g_cmd, 0, 1, &sc);
    memset(&g_dyn, 0, sizeof(g_dyn));
    g_dyn.vp = g_dyn.full = vp; g_dyn.vp_ok = 1;
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(g_cmd, 0, 1, &g_vb.buf, &zero);
    g_vb_crudo = 0;
    vkCmdBindIndexBuffer(g_cmd, g_ib.buf, 0, VK_INDEX_TYPE_UINT32);
    /* el mutex queda tomado hasta gpu_scene_end: una escena a la vez */
}

int gpu_target_raw16(void) { return g_cur && g_cur->raw16; }
int gpu_target_f16(void) { return g_cur && g_cur->tf == 2; }
void gpu_target_want_raw16(void) { if (g_cur && g_cur->tf == 2) g_cur->want_raw16 = 1; }

void gpu_draw(const GpuDraw *d, const GpuVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni) {
    if (!g_ok || !g_cur || !nv || !ni || g_cur->raw16) return;
    uint32_t vbytes = nv * (uint32_t)sizeof(GpuVertex), ibytes = ni * 4u;
    if (g_vb_off + vbytes > VB_SIZE || g_ib_off + ibytes > IB_SIZE) return;   /* escena llena */
    VkPipeline p = pipeline_for(d, 0, 0, 0, 0, 0);
    if (!p) return;
    VkDescriptorSet set = d->use_tex ? texture_set(&d->tex) : g_white->set[0];
    if (!set) return;
    memcpy((uint8_t *)g_vb.ptr + g_vb_off, v, vbytes);
    /* Los índices se rebasan al primer vértice de este draw dentro del anillo. */
    uint32_t *ip = (uint32_t *)((uint8_t *)g_ib.ptr + g_ib_off);
    for (uint32_t i = 0; i < ni; i++) ip[i] = idx[i];

    struct { float mvp[16]; float vp[4]; float vz[4]; int32_t flags[4]; float konst[4]; } pc;
    memcpy(pc.mvp, d->mvp, sizeof(pc.mvp));
    memcpy(pc.vp, d->vp, sizeof(pc.vp));
    pc.vz[0] = d->vz[0]; pc.vz[1] = d->vz[1];
    pc.vz[2] = (float)g_cur->w; pc.vz[3] = (float)g_cur->h;
    pc.flags[0] = d->use_tex; pc.flags[1] = d->use_color; pc.flags[2] = d->vp_enable; pc.flags[3] = d->use_const;
    memcpy(pc.konst, d->const_color, sizeof(pc.konst));

    dyn_estado(p, d);
    dyn_viewport(&g_dyn.full);
    vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &set, 0, NULL);
    g_dyn.tex = VK_NULL_HANDLE;   /* otro layout: los conjuntos de texturas de v3k ya no valen */
    vkCmdPushConstants(g_cmd, g_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(pc), &pc);
    vb_normal();
    vkCmdDrawIndexed(g_cmd, ni, 1, g_ib_off / 4u, (int32_t)(g_vb_off / sizeof(GpuVertex)), 0);
    g_vb_off += vbytes;
    g_ib_off += ibytes;
    g_stat_draws++;
    g_cur->ndraws++;
}

/* --- shaders traducidos --------------------------------------------------- */

int gpu_module(const uint32_t *spv, size_t words) {
    if (!g_ok || g_nmods >= MAX_MODS) return 0;
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smi.codeSize = words * 4;
    smi.pCode = spv;
    EnterCriticalSection(&g_cs);
    int id = 0;
    if (vkCreateShaderModule(g_dev, &smi, NULL, &g_mods[g_nmods]) == VK_SUCCESS) id = (int)g_nmods++;
    LeaveCriticalSection(&g_cs);
    return id;
}

void gpu_draw_prog(const GpuDraw *d, int vsm, int fsm, uint32_t nattr, const float *verts, uint32_t nv,
                   const uint32_t *idx, uint32_t ni, const uint32_t *mem, uint32_t memwords, uint32_t vsa, uint32_t fsa,
                   const GpuTexture *const tex[16]) {
    if (!g_ok || !g_cur || !nv || !ni || !vsm || !fsm) return;
    uint32_t stride = 16u * (nattr ? nattr : 1);
    uint32_t vbytes = nv * stride, ibytes = ni * 4u, mbytes = (memwords * 4u + 255u) & ~255u;
    if (!mbytes) mbytes = 256;
    uint32_t voff = (g_vb_off + stride - 1) / stride * stride;
    if (voff + vbytes > VB_SIZE || g_ib_off + ibytes > IB_SIZE || g_mb_off + mbytes > MB_SIZE) return;
    VkPipeline p = pipeline_for(d, (uint16_t)vsm, (uint16_t)fsm, (uint8_t)nattr, 0, 0);
    if (!p) return;
    /* conjuntos: memoria del draw y las 16 unidades de textura */
    VkDescriptorSet sets[2];
    VkDescriptorSetLayout lays[2] = { g_dsl_mem, g_dsl_tex };
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = g_dpool2; ai.descriptorSetCount = 2; ai.pSetLayouts = lays;
    if (vkAllocateDescriptorSets(g_dev, &ai, sets) != VK_SUCCESS) return;
    VkDescriptorBufferInfo bi = { g_mb.buf, g_mb_off, mbytes };
    VkDescriptorImageInfo ii[16];
    for (int u = 0; u < 16; u++) {
        Texture *x = NULL;
        VkSampler smp = VK_NULL_HANDLE;
        uint32_t si;
        if (tex && tex[u] && tex[u]->data && tex[u]->width && tex[u]->height) {
            smp = sampler_for(tex[u], &si);
            x = texture_get(tex[u]);
        }
        if (!x) { x = g_white; smp = sampler_for(&(GpuTexture){ 0 }, &si); }
        ii[u].sampler = smp; ii[u].imageView = x->view; ii[u].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
    w[0].dstSet = sets[0]; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &bi;
    w[1].dstSet = sets[1]; w[1].descriptorCount = 16; w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = ii;
    vkUpdateDescriptorSets(g_dev, 2, w, 0, NULL);

    memcpy((uint8_t *)g_vb.ptr + voff, verts, vbytes);
    memcpy((uint8_t *)g_ib.ptr + g_ib_off, idx, ibytes);
    memcpy((uint8_t *)g_mb.ptr + g_mb_off, mem, memwords * 4u);

    struct { float vp[4]; float vz[4]; uint32_t vsa, fsa, flags, pad; } pc;
    memcpy(pc.vp, d->vp, sizeof(pc.vp));
    pc.vz[0] = d->vz[0]; pc.vz[1] = d->vz[1];
    pc.vz[2] = (float)g_cur->w; pc.vz[3] = (float)g_cur->h;
    static int tint = -1;
    if (tint < 0) { const char *e = env_cached("MLB15_SHADERS"); tint = e && *e == '2'; }
    pc.vsa = vsa; pc.fsa = fsa; pc.flags = (d->vp_enable ? 1u : 0u) | (tint ? 2u : 0u); pc.pad = 0;

    dyn_estado(p, d);
    dyn_viewport(&g_dyn.full);
    vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout2, 0, 2, sets, 0, NULL);
    g_dyn.tex = VK_NULL_HANDLE;
    vkCmdPushConstants(g_cmd, g_layout2, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vb_normal();
    vkCmdDrawIndexed(g_cmd, ni, 1, g_ib_off / 4u, (int32_t)(voff / stride), 0);
    g_vb_off = voff + vbytes;
    g_ib_off += ibytes;
    g_mb_off += mbytes;
    g_stat_draws++;
    g_cur->ndraws++;
}

/* --- shaders de Vita3K ---------------------------------------------------- */

/* Cada bloque va al anillo g_mb alineado a 256 bytes. */
/* Relectura tardía de uniformes de fragmento: la GPU de la Vita lee los búferes de uniformes al
 * ejecutar la escena, no al llamar a sceGxmDraw, y el juego termina de escribir algunos después
 * del draw. Se copian otra vez de la memoria guest justo antes de enviar la escena.
 * bytes == 0: parche de exposición (.z = 0 -> 1.0, ver v3k_fill_data) en mb_off. */
typedef struct { uint32_t guest, bytes, mb_off; } UbTarde;
#define MAX_UB_TARDE 65536
static UbTarde g_ubt[MAX_UB_TARDE];
static uint32_t g_nubt, g_last_fdata_off = ~0u;
unsigned g_ubt_cambios;   /* búferes que cambiaron entre el draw y el envío (línea [fps]) */
uint32_t gpu_last_fdata_off(void) { return g_last_fdata_off; }
void gpu_ub_tarde(uint32_t guest, uint32_t bytes, uint32_t mb_off) {
    if (g_nubt < MAX_UB_TARDE) { g_ubt[g_nubt].guest = guest; g_ubt[g_nubt].bytes = bytes; g_ubt[g_nubt].mb_off = mb_off; g_nubt++; }
}
static void ub_tarde_aplica(void) {
    for (uint32_t i = 0; i < g_nubt; i++) {
        const UbTarde *u = &g_ubt[i];
        uint8_t *dst = (uint8_t *)g_mb.ptr + u->mb_off;
        if (!u->bytes) {
            if (dst[0] == 0 && dst[1] == 0) { dst[0] = 0x00; dst[1] = 0x3C; }
            continue;
        }
        const uint8_t *src = (const uint8_t *)vita_mem_ptr(u->guest, u->bytes);
        if (src && memcmp(dst, src, u->bytes)) g_ubt_cambios++;
        if (src) memcpy(dst, src, u->bytes);
    }
    g_nubt = 0;
}
static int g_mb_cero = 1;   /* gpu_draw_v3k lo ajusta por binding (MLB15_MB_CERO=máscara, pruebas) */
static VkDescriptorBufferInfo mb_put(const void *data, uint32_t bytes) {
    VkDescriptorBufferInfo bi = { g_mb.buf, g_mb_off, bytes ? (bytes + 15u) & ~15u : 16u };
    uint32_t slot = ((uint32_t)bi.range + 255u) & ~255u;
    if (data && bytes) memcpy((uint8_t *)g_mb.ptr + g_mb_off, data, bytes);
    else bytes = 0;
    /* El resto del hueco a ceros, para que nada dependa de restos de otros draws (medido con
     * MLB15_UB_VENENO: ningún shader del partido lee ahí). */
    static int veneno = -1;   /* MLB15_UB_VENENO=1: NaN en el resto del hueco (prueba: quién lee fuera) */
    if (veneno < 0) veneno = env_cached("MLB15_UB_VENENO") ? 1 : 0;
    if (g_mb_cero && g_mb_off + slot <= MB_SIZE) memset((uint8_t *)g_mb.ptr + g_mb_off + bytes, veneno ? 0xFF : 0, slot - bytes);
    g_mb_off += slot;
    return bi;
}

/* Texturas ya resueltas en la escena en curso (vista + sampler por GpuTexture idéntica):
 * cada draw pedía las 16 unidades una a una (texture_get, sampler_for...). La comprobación de
 * contenido de texture_get ya era una vez por escena, así que el resultado es el mismo.
 * MLB15_SIN_MEMO_TEX=1 lo quita. */
#define TEXMEMO_N 512
static struct { GpuTexture t; uint32_t esc; VkImageView view; VkSampler smp; } g_texmemo[TEXMEMO_N];
static void tex_infos(const GpuTexture *const tex[16], VkDescriptorImageInfo ii[16]) {
    static VkSampler smp_blanco;
    static uint32_t si_blanco;
    static int sin_memo = -1;
    if (sin_memo < 0) sin_memo = env_cached("MLB15_SIN_MEMO_TEX") ? 1 : 0;
    if (!smp_blanco) smp_blanco = sampler_for(&(GpuTexture){ 0 }, &si_blanco);
    for (int u = 0; u < 16; u++) {
        Texture *x = NULL;
        VkSampler smp = VK_NULL_HANDLE;
        uint32_t si;
        uint32_t mj = ~0u;
        if (!sin_memo && tex && tex[u] && tex[u]->data && tex[u]->width && tex[u]->height) {
            uint32_t hh = 2166136261u;
            const uint8_t *b = (const uint8_t *)tex[u];
            for (size_t k = 0; k < sizeof(GpuTexture); k++) hh = (hh ^ b[k]) * 16777619u;
            mj = hh & (TEXMEMO_N - 1);
            if (g_texmemo[mj].esc == g_texset_esc && !memcmp(&g_texmemo[mj].t, tex[u], sizeof(GpuTexture))) {
                ii[u].sampler = g_texmemo[mj].smp; ii[u].imageView = g_texmemo[mj].view;
                ii[u].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                continue;
            }
        }
        if (tex && tex[u] && tex[u]->data && tex[u]->width && tex[u]->height) {
            GpuTexture tt = *tex[u];
            if ((tt.format >> 24) == 0x1F) tt.min_linear = tt.mag_linear = 0;   /* bits crudos: sin filtrar */
            smp = sampler_for(&tt, &si);
            x = texture_get(tex[u]);
        }
        if (!x) { x = g_white; smp = smp_blanco; }
        ii[u].sampler = smp; ii[u].imageView = x->view; ii[u].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (mj != ~0u) { g_texmemo[mj].t = *tex[u]; g_texmemo[mj].esc = g_texset_esc; g_texmemo[mj].view = x->view; g_texmemo[mj].smp = smp; }
    }
}

/* Conjuntos de 16 texturas ya escritos en la escena en curso: muchos draws seguidos usan las
 * mismas (público, estadio, uniformes), así que se reutiliza el conjunto en vez de pedir y
 * escribir uno nuevo en cada draw. Se vacía al empezar cada escena (se reinicia su pool).
 * MLB15_SIN_CACHE_TEX=1 lo quita. */
#define TEXSET_N 1024
typedef struct { uint32_t h; VkDescriptorImageInfo ii[16]; VkDescriptorSet set; } TexSet;
static TexSet g_texset[TEXSET_N];
static uint32_t g_texset_gen[TEXSET_N];
static VkDescriptorSet texset_busca(const VkDescriptorImageInfo ii[16], uint32_t *hp, uint32_t *slot) {
    uint32_t h = 2166136261u;
    const uint8_t *b = (const uint8_t *)ii;
    for (size_t i = 0; i < sizeof(VkDescriptorImageInfo) * 16; i++) h = (h ^ b[i]) * 16777619u;
    *hp = h;
    for (uint32_t k = 0; k < 8; k++) {
        uint32_t j = (h + k) & (TEXSET_N - 1);
        if (g_texset_gen[j] != g_texset_esc) { *slot = j; return VK_NULL_HANDLE; }
        if (g_texset[j].h == h && !memcmp(g_texset[j].ii, ii, sizeof(g_texset[j].ii))) return g_texset[j].set;
    }
    *slot = ~0u;
    return VK_NULL_HANDLE;
}

static void draw_v3k_x(const GpuDraw *d, int vsm, int fsm, uint32_t nloc, uint32_t uint_mask, const float *verts, uint32_t nv,
                       const uint32_t *idx, uint32_t ni, const void *vinfo, uint32_t vinfo_bytes, const void *finfo,
                       uint32_t finfo_bytes, const void *vdata, uint32_t vbytes_data, const void *fdata, uint32_t fbytes_data,
                       const float viewport[4], const GpuTexture *const tex[16], const GpuVLayout *lay,
                       const uint8_t *const *bdata, const uint32_t *bbytes, uint32_t ninst);
void gpu_draw_v3k(const GpuDraw *d, int vsm, int fsm, uint32_t nloc, uint32_t uint_mask, const float *verts, uint32_t nv,
                  const uint32_t *idx, uint32_t ni, const void *vinfo, uint32_t vinfo_bytes, const void *finfo,
                  uint32_t finfo_bytes, const void *vdata, uint32_t vbytes_data, const void *fdata, uint32_t fbytes_data,
                  const float viewport[4], const GpuTexture *const tex[16]) {
    draw_v3k_x(d, vsm, fsm, nloc, uint_mask, verts, nv, idx, ni, vinfo, vinfo_bytes, finfo, finfo_bytes, vdata, vbytes_data,
               fdata, fbytes_data, viewport, tex, NULL, NULL, NULL, 1);
}
void gpu_draw_v3k_crudo(const GpuDraw *d, int vsm, int fsm, uint32_t nloc, const GpuVLayout *lay,
                        const uint8_t *const bdata[GPU_VLAY_MAXB], const uint32_t bbytes[GPU_VLAY_MAXB], uint32_t ninst,
                        uint32_t nv, const uint32_t *idx, uint32_t ni, const void *vinfo, uint32_t vinfo_bytes,
                        const void *finfo, uint32_t finfo_bytes, const void *vdata, uint32_t vbytes_data,
                        const void *fdata, uint32_t fbytes_data, const float viewport[4], const GpuTexture *const tex[16]) {
    draw_v3k_x(d, vsm, fsm, nloc, 0, NULL, nv, idx, ni, vinfo, vinfo_bytes, finfo, finfo_bytes, vdata, vbytes_data,
               fdata, fbytes_data, viewport, tex, lay, bdata, bbytes, ninst ? ninst : 1);
}
static void draw_v3k_x(const GpuDraw *d, int vsm, int fsm, uint32_t nloc, uint32_t uint_mask, const float *verts, uint32_t nv,
                       const uint32_t *idx, uint32_t ni, const void *vinfo, uint32_t vinfo_bytes, const void *finfo,
                       uint32_t finfo_bytes, const void *vdata, uint32_t vbytes_data, const void *fdata, uint32_t fbytes_data,
                       const float viewport[4], const GpuTexture *const tex[16], const GpuVLayout *lay,
                       const uint8_t *const *bdata, const uint32_t *bbytes, uint32_t ninst) {
    g_last_fdata_off = ~0u;
    if (!g_ok || !g_cur || !nv || !ni || !vsm || !fsm) return;
    if (!nloc) nloc = 1;
    uint32_t stride = 16u * nloc;
    uint32_t vbytes = nv * stride, ibytes = ni * 4u;
    uint16_t vlay = 0;
    if (lay) {   /* crudos: cada flujo alineado a 16 */
        vbytes = 0;
        for (uint32_t b = 0; b < lay->nbind; b++) vbytes += (bbytes[b] + 15u) & ~15u;
        stride = 16u;
        vlay = vlay_id(lay);
        if (!vlay) return;
    }
    uint32_t need = 4u * 256u + ((vinfo_bytes + finfo_bytes + vbytes_data + fbytes_data + 4u * 255u) & ~255u);
    uint32_t voff = (g_vb_off + stride - 1) / stride * stride;
    if (voff + vbytes > VB_SIZE || g_ib_off + ibytes > IB_SIZE || g_mb_off + need > MB_SIZE - DYN_SB_RANGE) {
        g_drop_lleno++;
        if (voff + vbytes > VB_SIZE) g_drop_vb++;
        return;
    }
    double e0 = prof_now();
    VkPipeline p = pipeline_for_l(d, (uint16_t)vsm, (uint16_t)fsm, (uint8_t)nloc, 1, (uint16_t)uint_mask, vlay);
    double e1 = prof_now(); g_envio_t[0] += e1 - e0;
    if (!p) {
        static int told;
        if (told++ < 8) printf("[gpu] pipeline v3k no creado (vs %d fs %d nloc %u)\n", vsm, fsm, nloc);
        return;
    }
    if (!g_vtex_white) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = g_dpool3_fijo; ai.descriptorSetCount = 1; ai.pSetLayouts = &g_dsl_tex_all;
        if (vkAllocateDescriptorSets(g_dev, &ai, &g_vtex_white) != VK_SUCCESS) return;
        VkDescriptorImageInfo wi[16];
        tex_infos(NULL, wi);
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = g_vtex_white; w.descriptorCount = 16; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = wi;
        vkUpdateDescriptorSets(g_dev, 1, &w, 0, NULL);
    }
    double e2 = prof_now();
    VkDescriptorImageInfo ii[16];
    static int sin_cache = -1;
    if (sin_cache < 0) sin_cache = env_cached("MLB15_SIN_CACHE_TEX") ? 1 : 0;
    /* Mismas texturas que el dibujo anterior de esta escena (lo normal): mismo resultado y mismo
     * conjunto, sin recorrer las 16 unidades (fase 4b). MLB15_SIN_TEX_ANTERIOR=1 lo quita. */
    static GpuTexture prev_t[16];
    static uint8_t prev_has[16];
    static VkDescriptorImageInfo prev_ii[16];
    static VkDescriptorSet prev_set;
    static uint32_t prev_esc = ~0u;
    static int sin_prev = -1;
    if (sin_prev < 0) sin_prev = env_cached("MLB15_SIN_TEX_ANTERIOR") ? 1 : 0;
    int igual = !sin_prev && !sin_cache && prev_esc == g_texset_esc && prev_set;
    for (int u = 0; u < 16 && igual; u++) {
        uint8_t has = tex && tex[u] && tex[u]->data && tex[u]->width && tex[u]->height;
        if (has != prev_has[u] || (has && memcmp(tex[u], &prev_t[u], sizeof(GpuTexture)))) igual = 0;
    }
    if (igual) memcpy(ii, prev_ii, sizeof(ii));
    else tex_infos(tex, ii);
    {   /* MLB15_TEX_ESTAD=1: cuántas veces sirve el dibujo anterior y cuántas texturas usa cada dibujo */
        static int on = -1; static unsigned long n, si, unid, memo_mal;
        if (on < 0) on = env_cached("MLB15_TEX_ESTAD") ? 1 : 0;
        if (on) {
            n++; si += igual;
            for (int u = 0; u < 16; u++) if (tex && tex[u] && tex[u]->data) unid++;
            if (n % 300000 == 0) printf("[texestad] dibujos %lu, igual al anterior %lu (%.0f %%), unidades/dibujo %.2f\n", n, si, 100.0 * si / n, (double)unid / n);
        }
    }
    double e3 = prof_now(); g_envio_t[1] += e3 - e2;
    uint32_t th = 0, tslot = ~0u;
    VkDescriptorSet sets[2];
    sets[1] = igual ? prev_set : sin_cache ? VK_NULL_HANDLE : texset_busca(ii, &th, &tslot);
    VkDescriptorSetLayout lays[2] = { g_dsl_v3k, g_dsl_tex_all };
    int dyn = g_dset0 && vinfo_bytes <= DYN_UB_RANGE && finfo_bytes <= DYN_UB_RANGE && vbytes_data <= DYN_SB_RANGE &&
              fbytes_data <= DYN_SB_RANGE;
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = g_dpool3;
    int tex_nuevo = !sets[1];
    if (dyn) {
        sets[0] = g_dset0;
        if (tex_nuevo) {
            ai.descriptorSetCount = 1; ai.pSetLayouts = &lays[1];
            if (vkAllocateDescriptorSets(g_dev, &ai, &sets[1]) != VK_SUCCESS) { g_drop_ds++; return; }
        }
    } else {
        ai.descriptorSetCount = sets[1] ? 1 : 2; ai.pSetLayouts = lays;
        if (vkAllocateDescriptorSets(g_dev, &ai, sets) != VK_SUCCESS) { g_drop_ds++; return; }
    }
    if (!sin_prev) {
        for (int u = 0; u < 16; u++) {
            prev_has[u] = tex && tex[u] && tex[u]->data && tex[u]->width && tex[u]->height;
            if (prev_has[u]) prev_t[u] = *tex[u];
        }
        memcpy(prev_ii, ii, sizeof(ii)); prev_set = sets[1]; prev_esc = g_texset_esc;
    }
    if (tex_nuevo && tslot != ~0u) {
        g_texset_gen[tslot] = g_texset_esc; g_texset[tslot].h = th;
        memcpy(g_texset[tslot].ii, ii, sizeof(ii)); g_texset[tslot].set = sets[1];
    }
    double e4 = prof_now(); g_envio_t[2] += e4 - e3;
    VkDescriptorBufferInfo bi[4];
    static int cero = -1;
    if (cero < 0) { const char *e = env_cached("MLB15_MB_CERO"); cero = e ? (int)strtol(e, NULL, 0) : 15; }
    static const char *solo = (const char *)1;   /* MLB15_MB_CERO_SOLO=fgxp,...: ceros solo en esos (el resto, restos) */
    if (solo == (const char *)1) solo = env_cached("MLB15_MB_CERO_SOLO");
    int cero_d = cero;
    if (solo) {
        extern uint32_t g_v3k_fgxp_cur;
        char key[16];
        snprintf(key, sizeof(key), "%08X", g_v3k_fgxp_cur);
        cero_d = strstr(solo, key) ? 15 : 0;
    }
    g_mb_cero = cero_d & 1; bi[0] = mb_put(vinfo, vinfo_bytes);
    g_mb_cero = cero_d & 2; bi[1] = mb_put(finfo, finfo_bytes);
    g_mb_cero = cero_d & 4; bi[2] = mb_put(vdata, vbytes_data);
    g_mb_cero = cero_d & 8; bi[3] = mb_put(fdata, fbytes_data);
    g_last_fdata_off = (uint32_t)bi[3].offset;
    g_mb_cero = 1;
    VkWriteDescriptorSet w[5];
    for (int i = 0; i < 5; i++) { memset(&w[i], 0, sizeof(w[i])); w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; }
    uint32_t doffs[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 4; i++) {
        w[i].dstSet = sets[0]; w[i].dstBinding = (uint32_t)i; w[i].descriptorCount = 1;
        w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        w[i].pBufferInfo = &bi[i];
        if (dyn) doffs[i] = (uint32_t)bi[i].offset;
    }
    w[4].dstSet = sets[1]; w[4].descriptorCount = 16; w[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[4].pImageInfo = ii;
    double e5 = prof_now(); g_envio_t[3] += e5 - e4;
    if (dyn) { if (tex_nuevo) vkUpdateDescriptorSets(g_dev, 1, &w[4], 0, NULL); }
    else vkUpdateDescriptorSets(g_dev, tex_nuevo ? 5 : 4, w, 0, NULL);
    double e6 = prof_now(); g_envio_t[4] += e6 - e5;

    VkDeviceSize boffs[GPU_VLAY_MAXB + 1];
    VkBuffer bbufs[GPU_VLAY_MAXB + 1];
    if (lay) {
        uint32_t o = voff;
        for (uint32_t b = 0; b < lay->nbind; b++) {
            memcpy((uint8_t *)g_vb.ptr + o, bdata[b], bbytes[b]);
            bbufs[b] = g_vb.buf; boffs[b] = o;
            o += (bbytes[b] + 15u) & ~15u;
        }
        bbufs[lay->nbind] = g_vdef.buf; boffs[lay->nbind] = 0;
    } else memcpy((uint8_t *)g_vb.ptr + voff, verts, vbytes);
    memcpy((uint8_t *)g_ib.ptr + g_ib_off, idx, ibytes);
    double e7 = prof_now(); g_envio_t[5] += e7 - e6;

    const float rs = (float)g_res;
    VkViewport vp = { viewport[0] * rs, viewport[1] * rs, viewport[2] * rs, viewport[3] * rs, 0.0f, 1.0f };
    VkViewport full = { 0.0f, 0.0f, (float)g_cur->sw, (float)g_cur->sh, 0.0f, 1.0f };
    dyn_estado(p, d);
    dyn_viewport(&vp);
    VkDescriptorSet tail[2] = { g_vtex_white, sets[1] };
    vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout3, 0, 1, &sets[0], 4, doffs);
    if (g_dyn.tex != sets[1] || g_dyn.layout != g_layout3) {
        vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout3, 2, 2, tail, 0, NULL);
        g_dyn.tex = sets[1]; g_dyn.layout = g_layout3;
    }
    if (lay) {
        vkCmdBindVertexBuffers(g_cmd, 0, lay->nbind + 1u, bbufs, boffs);
        g_vb_crudo = 1;
        vkCmdDrawIndexed(g_cmd, ni, ninst, g_ib_off / 4u, 0, 0);
    } else {
        vb_normal();
        vkCmdDrawIndexed(g_cmd, ni, 1, g_ib_off / 4u, (int32_t)(voff / stride), 0);
    }
    g_envio_t[6] += prof_now() - e7;
    (void)full;   /* (antes se reponía el viewport completo tras cada draw: dyn_viewport lo hace si hace falta) */
    g_vb_off = voff + vbytes;
    g_ib_off += ibytes;
    if (g_vb_off > g_vb_max) g_vb_max = g_vb_off;
    if (g_ib_off > g_ib_max) g_ib_max = g_ib_off;
    if (g_mb_off > g_mb_max) g_mb_max = g_mb_off;
    g_stat_draws++;
    g_cur->ndraws++;
}

/* --- pase final: suavizado (FXAA) + escalado bicúbico con nitidez ------------------
 * El juego no tiene antialiasing: a 2x en pantalla se veían dientes de sierra en las
 * líneas del campo, el bate y las siluetas, y el escalado lineal ablandaba la imagen.
 * Dos pases: FXAA al tamaño de la imagen del juego y Catmull-Rom + nitidez adaptativa
 * (tipo AMD CAS) al tamaño de destino. Se dibuja en una imagen propia que luego se copia
 * 1:1 a la ventana, así también se puede volcar sin ventana (MLB15_VOLCAR_POST=n cada n
 * escenas de pantalla, tamaño MLB15_POST_TAM=1920x1080, en frames\post_NNNNN.bmp).
 * MLB15.ini [video]: posproceso (1), suavizado (1), nitidez (0-100, 35); las variables
 * MLB15_POSPROCESO / MLB15_SUAVIZADO / MLB15_NITIDEZ mandan sobre el ini. */
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; VkFramebuffer fb; uint32_t w, h; } PpImg;
static int g_pp_ok = -1;                       /* -1 = sin preparar */
static float g_pp_fxaa, g_pp_sharp;
static VkRenderPass g_pp_pass;
static VkDescriptorSetLayout g_pp_dsl;
static VkPipelineLayout g_pp_layout;
static VkPipeline g_pp_pipe_fxaa, g_pp_pipe_esc;
static VkDescriptorPool g_pp_pool;
static VkDescriptorSet g_pp_set_src, g_pp_set_mid;
static VkImageView g_pp_mid_bound;
static VkSampler g_pp_smp;
static PpImg g_pp_mid, g_pp_out;

static int pp_opt(const char *env, const char *key, int def) {
    const char *e = getenv(env);
    return e ? atoi(e) : launcher_ini_int("video", key, def);
}

static int pp_pipeline(VkShaderModule vs, VkShaderModule fs, VkPipeline *out) {
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                              { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba = { 0 };
    ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pi.stageCount = 2; pi.pStages = st;
    pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vps;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pColorBlendState = &cb;
    pi.pDynamicState = &ds; pi.layout = g_pp_layout; pi.renderPass = g_pp_pass;
    return vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &pi, NULL, out) == VK_SUCCESS;
}

static int pp_init_inner(void) {
    VkAttachmentDescription att = { 0 };
    att.format = VK_FORMAT_R8G8B8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { 0 };
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    VkSubpassDependency dep[2];
    memset(dep, 0, sizeof(dep));
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = 1; rpi.pAttachments = &att;
    rpi.subpassCount = 1; rpi.pSubpasses = &sub;
    rpi.dependencyCount = 2; rpi.pDependencies = dep;
    CHECK(vkCreateRenderPass(g_dev, &rpi, NULL, &g_pp_pass));

    VkDescriptorSetLayoutBinding bind = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dli.bindingCount = 1; dli.pBindings = &bind;
    CHECK(vkCreateDescriptorSetLayout(g_dev, &dli, NULL, &g_pp_dsl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1; pli.pSetLayouts = &g_pp_dsl;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    CHECK(vkCreatePipelineLayout(g_dev, &pli, NULL, &g_pp_layout));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpi.maxSets = 2; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
    CHECK(vkCreateDescriptorPool(g_dev, &dpi, NULL, &g_pp_pool));
    VkDescriptorSetLayout two[2] = { g_pp_dsl, g_pp_dsl };
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = g_pp_pool; dai.descriptorSetCount = 2; dai.pSetLayouts = two;
    CHECK(vkAllocateDescriptorSets(g_dev, &dai, sets));
    g_pp_set_src = sets[0]; g_pp_set_mid = sets[1];
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    CHECK(vkCreateSampler(g_dev, &si, NULL, &g_pp_smp));

    VkShaderModule vs, fx, es;
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smi.codeSize = sizeof(spv_pp_vert); smi.pCode = spv_pp_vert;
    CHECK(vkCreateShaderModule(g_dev, &smi, NULL, &vs));
    smi.codeSize = sizeof(spv_pp_fxaa_frag); smi.pCode = spv_pp_fxaa_frag;
    CHECK(vkCreateShaderModule(g_dev, &smi, NULL, &fx));
    smi.codeSize = sizeof(spv_pp_escala_frag); smi.pCode = spv_pp_escala_frag;
    CHECK(vkCreateShaderModule(g_dev, &smi, NULL, &es));
    return pp_pipeline(vs, fx, &g_pp_pipe_fxaa) && pp_pipeline(vs, es, &g_pp_pipe_esc);
}

static int pp_on(void) {
    if (g_pp_ok < 0) {
        g_pp_fxaa = pp_opt("MLB15_SUAVIZADO", "suavizado", 1) ? 1.0f : 0.0f;
        int n = pp_opt("MLB15_NITIDEZ", "nitidez", 35);
        g_pp_sharp = (float)(n < 0 ? 0 : n > 100 ? 100 : n) / 100.0f;
        g_pp_ok = pp_opt("MLB15_POSPROCESO", "posproceso", 1) && pp_init_inner();
        printf("[gpu] pase final: %s (suavizado %d, nitidez %d)\n", g_pp_ok ? "sí" : "no", (int)g_pp_fxaa,
               (int)(g_pp_sharp * 100.0f + 0.5f));
        fflush(stdout);
    }
    return g_pp_ok;
}

static int pp_img(PpImg *p, uint32_t w, uint32_t h) {
    if (p->img && p->w == w && p->h == h) return 1;
    if (p->img) {
        vkDeviceWaitIdle(g_dev);
        vkDestroyFramebuffer(g_dev, p->fb, NULL);
        vkDestroyImageView(g_dev, p->view, NULL);
        vkDestroyImage(g_dev, p->img, NULL);
        vkFreeMemory(g_dev, p->mem, NULL);
        memset(p, 0, sizeof(*p));
    }
    if (!image_new_fmt(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       &p->img, &p->mem, &p->view)) { p->img = VK_NULL_HANDLE; return 0; }
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = g_pp_pass; fi.attachmentCount = 1; fi.pAttachments = &p->view;
    fi.width = w; fi.height = h; fi.layers = 1;
    if (vkCreateFramebuffer(g_dev, &fi, NULL, &p->fb) != VK_SUCCESS) return 0;
    p->w = w; p->h = h;
    if (p == &g_pp_mid) g_pp_mid_bound = VK_NULL_HANDLE;
    return 1;
}

static void pp_bind(VkDescriptorSet set, VkImageView view) {
    VkDescriptorImageInfo ii = { g_pp_smp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(g_dev, 1, &w, 0, NULL);
}

/* Antes de grabar: los conjuntos no se pueden tocar mientras un comando que los usa
 * esté pendiente (quien llama ya esperó a su valla). */
static int pp_prepare(VkImageView src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh) {
    if (!pp_on() || !pp_img(&g_pp_mid, sw, sh) || !pp_img(&g_pp_out, dw, dh)) return 0;
    pp_bind(g_pp_set_src, src);
    if (g_pp_mid_bound != g_pp_mid.view) { pp_bind(g_pp_set_mid, g_pp_mid.view); g_pp_mid_bound = g_pp_mid.view; }
    return 1;
}

static void pp_pass(VkCommandBuffer cb, PpImg *dst, VkPipeline pipe, VkDescriptorSet set, const float pc[8]) {
    VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rb.renderPass = g_pp_pass; rb.framebuffer = dst->fb;
    rb.renderArea.extent.width = dst->w; rb.renderArea.extent.height = dst->h;
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pp_layout, 0, 1, &set, 0, NULL);
    VkViewport vp = { 0, 0, (float)dst->w, (float)dst->h, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { dst->w, dst->h } };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdPushConstants(cb, g_pp_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
}

/* Vista para el pase final: los bytes tal cual. Una pantalla sRGB (gamma de GXM) se lee
 * con una vista UNORM: lo guardado ya es el color que se ve. */
static VkImageView target_present_view(Target *t) {
    if (t->tf == 0) return t->view;
    if (t->tf != 5) return VK_NULL_HANDLE;
    if (!t->pview) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = t->img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(g_dev, &vi, NULL, &t->pview) != VK_SUCCESS) t->pview = VK_NULL_HANDLE;
    }
    return t->pview;
}

/* El origen ha de estar en SHADER_READ_ONLY; deja g_pp_out.img en TRANSFER_SRC. */
static void pp_record(VkCommandBuffer cb) {
    float sw = (float)g_pp_mid.w, sh = (float)g_pp_mid.h;
    float pc[8] = { 1.0f / sw, 1.0f / sh, sw, sh, g_pp_fxaa, g_pp_sharp, (float)g_pp_out.w / sw, 0 };
    pp_pass(cb, &g_pp_mid, g_pp_pipe_fxaa, g_pp_set_src, pc);
    pp_pass(cb, &g_pp_out, g_pp_pipe_esc, g_pp_set_mid, pc);
    barrier(cb, g_pp_out.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
}

/* MLB15_VOLCAR_POST=n: cada n escenas de pantalla pasa el pase final y lo guarda (sin ventana). */
static void pp_volcar(Target *t) {
    static int every = -1;
    static unsigned nsc;
    static uint32_t dw = 1920, dh = 1080;
    static Buffer buf;
    if (every < 0) {
        const char *e = env_cached("MLB15_VOLCAR_POST");
        every = e ? atoi(e) : 0;
        if ((e = env_cached("MLB15_POST_TAM")) != NULL) sscanf(e, "%ux%u", &dw, &dh);
    }
    extern int port_display_is_framebuf(uint32_t addr);
    if (every <= 0 || (t->tf != 0 && t->tf != 5) || !port_display_is_framebuf(t->addr) || ++nsc % (unsigned)every) return;
    uint32_t ow = dw, oh = dw * t->h / t->w;   /* misma proporción que en la ventana */
    if (oh > dh) { oh = dh; ow = dh * t->w / t->h; }
    VkImageView view = target_present_view(t);
    if (!view || !pp_prepare(view, t->sw, t->sh, ow, oh)) return;
    VkDeviceSize need = (VkDeviceSize)ow * oh * 4;
    if (buf.size < need) {
        memset(&buf, 0, sizeof(buf));   /* (la anterior se queda: solo depuración) */
        if (!buffer_new_ex(&buf, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1)) return;
    }
    VkCommandBuffer cb = g_cmd_once;
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    pp_record(cb);
    VkBufferImageCopy r = { 0 };
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = ow; r.imageExtent.height = oh; r.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, g_pp_out.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf.buf, 1, &r);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vkResetFences(g_dev, 1, &g_fence_once);
    vkQueueSubmit(g_queue, 1, &si, g_fence_once);
    vkWaitForFences(g_dev, 1, &g_fence_once, VK_TRUE, UINT64_MAX);
    char name[64];
    snprintf(name, sizeof(name), "frames\\post_%05u.bmp", nsc);
    FILE *fh = fopen(name, "wb");
    if (!fh) return;
    uint32_t sz = 54 + ow * oh * 4;
    uint8_t hd[54] = { 'B', 'M' };
    int32_t ih = -(int32_t)oh;
    memcpy(hd + 2, &sz, 4); hd[10] = 54; hd[14] = 40;
    memcpy(hd + 18, &ow, 4); memcpy(hd + 22, &ih, 4); hd[26] = 1; hd[28] = 32;
    fwrite(hd, 1, 54, fh);
    const uint8_t *s = (const uint8_t *)buf.ptr;
    for (uint32_t i = 0; i < ow * oh; i++) {
        uint8_t bgra[4] = { s[4 * i + 2], s[4 * i + 1], s[4 * i], 255 };
        fwrite(bgra, 1, 4, fh);
    }
    fclose(fh);
}

/* Lo que se hacía tras esperar a la GPU al final de la escena: copia a la memoria guest,
 * trazas y hash de la memoria (para saber si la CPU la toca después). */
static void scene_finish(Target *t) {
    pp_volcar(t);

    /* De vuelta a la memoria guest: lo que la GPU de la Vita escribiría. */
    /* Solo la zona del render target: lo de fuera no lo escribe la GPU. */
    uint32_t bpp = g_tfmt_bpp[t->tf];
    uint8_t *dst = t->resident ? NULL : (uint8_t *)vita_mem_ptr(t->addr, t->gstride * t->h * bpp);
    const uint8_t *src = (const uint8_t *)t->staging.ptr;
    if (dst) {
        for (uint32_t y = 0; y < t->rh; y++)
            memcpy(dst + (size_t)y * t->gstride * bpp, src + (size_t)y * t->w * bpp, (size_t)t->rw * bpp);
        ww_marca(t->addr, t->gstride * t->rh * bpp);
    }
    {   /* MLB15_ESCENA_TAM: media de lo que vuelve a la memoria */
        static uint32_t ew = 1, eh;
        if (ew == 1) { const char *e = env_cached("MLB15_ESCENA_TAM"); ew = 0; if (e) sscanf(e, "%ux%u", &ew, &eh); }
        if (ew && t->w == ew && t->h == eh) {
            double sm[4] = { 0 };
            for (uint32_t i = 0; i < t->w * t->h && bpp == 4; i++) for (int c = 0; c < 4; c++) sm[c] += src[4 * i + c];
            uint32_t np = t->w * t->h;
            printf("[escena-tam] fin 0x%08X: rw %u rh %u stride %u dst %s media %.0f %.0f %.0f %.0f\n", t->addr, t->rw, t->rh, t->gstride,
                   dst ? "sí" : "no", sm[0] / np, sm[1] / np, sm[2] / np, sm[3] / np);
        }
    }
    {   /* MLB15_TONO_TRAZA=1: lo que ve la exposición automática del juego (sub_813F292A) en
         * ToneMapSample 32x32: media de R,G,B de los píxeles con B > umbral (0x81E6EDE4),
         * objetivo (0x81F78554) y límites (+4, +8) */
        static int tt = -1;
        if (tt < 0) tt = env_cached("MLB15_TONO_TRAZA") ? 1 : 0;
        static unsigned nt;
        if (tt && dst && t->w == 32 && t->h == 32 && bpp == 4 && nt++ % 60 == 0) {
            float thr, mn, mx;
            uint32_t u = vita_read32(0x81E6EDE4); memcpy(&thr, &u, 4);
            u = vita_read32(0x81F78558); memcpy(&mn, &u, 4);
            u = vita_read32(0x81F7855C); memcpy(&mx, &u, 4);
            unsigned sr = 0, sg = 0, sb = 0, n = 0, ar = 0, ag = 0, ab = 0;
            for (int i = 0; i < 32 * 32; i++) {
                const uint8_t *px = dst + i * 4;
                ar += px[0]; ag += px[1]; ab += px[2];
                if (px[2] > thr) { sr += px[0]; sg += px[1]; sb += px[2]; n++; }
            }
            printf("[tono] 0x%08X n=%u media %u,%u,%u (todos %u,%u,%u) umbral %.1f objetivo %d lim %.3f..%.3f\n", t->addr, n,
                   n ? sr / n : 0, n ? sg / n : 0, n ? sb / n : 0, ar / 1024, ag / 1024, ab / 1024, thr,
                   (int)vita_read32(0x81F78554), mn, mx);
        }
    }
    {   /* MLB15_NEGRO_MASCARA=dir: copia de la última escena de esa superficie (gpu_mascara_guarda) */
        static uint32_t want = 1;
        if (want == 1) { const char *e = env_cached("MLB15_NEGRO_MASCARA"); want = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
        if (want && t->addr == want) {
            size_t n = (size_t)t->w * t->h * bpp;
            if (g_mask_cap < n) { free(g_mask); g_mask = (uint8_t *)malloc(n); g_mask_cap = g_mask ? n : 0; }
            if (g_mask) { memcpy(g_mask, src, n); g_mask_n = n; }
        }
    }
    {   /* MLB15_VOLCAR_SUPERFICIE=dir,n: guarda la n-ésima escena de esa superficie como BMP (32 bpp) */
        static uint32_t want = 1, nth, seen, wantw;
        if (want == 1) {
            const char *e = env_cached("MLB15_VOLCAR_SUPERFICIE");
            want = e ? (uint32_t)strtoul(e, NULL, 16) : 0;
            nth = e && strchr(e, ',') ? (uint32_t)atoi(strchr(e, ',') + 1) : 1;
            const char *c2 = e && strchr(e, ',') ? strchr(strchr(e, ',') + 1, ',') : NULL;
            wantw = c2 ? (uint32_t)atoi(c2 + 1) : 0;   /* tercer campo opcional: ancho */
        }
        if (want && t->addr == want && (!wantw || t->w == wantw) && bpp == 1 && ++seen == nth) {
            FILE *fr = fopen("superficie.raw", "wb");
            if (fr) { fwrite(src, 1, (size_t)t->w * t->h, fr); fclose(fr); }
            printf("[gpu] superficie 0x%08X volcada en bruto (escena %u)\n", t->addr, nth);
        } else if (want && t->addr == want && (!wantw || t->w == wantw) && (bpp == 4 || bpp == 8) && ++seen == nth) {
            FILE *fh = fopen("superficie.bmp", "wb");
            if (fh) {   /* 8 bytes por píxel: se guarda el doble de ancho (bytes crudos de 4 en 4) */
                uint32_t w = t->w * (bpp / 4), h = t->h, sz = 54 + w * h * 4;
                uint8_t hd[54] = { 'B', 'M' };
                int32_t ih = -(int32_t)h;
                memcpy(hd + 2, &sz, 4); hd[10] = 54; hd[14] = 40;
                memcpy(hd + 18, &w, 4); memcpy(hd + 22, &ih, 4); hd[26] = 1; hd[28] = 32;
                fwrite(hd, 1, 54, fh);
                for (uint32_t i = 0; i < w * h; i++) {
                    const uint8_t *p = src + 4 * i;
                    uint8_t bgra[4] = { p[2], p[1], p[0], 255 };
                    fwrite(bgra, 1, 4, fh);
                }
                fclose(fh);
                if ((fh = fopen("superficie.raw", "wb"))) { fwrite(src, 1, (size_t)t->w * t->h * bpp, fh); fclose(fh); }
                printf("[gpu] superficie 0x%08X volcada (escena %u)\n", t->addr, nth);
            }
        }
    }
    if (g_hd_pending) {
        char name[64];
        snprintf(name, sizeof(name), "frames\\hd_%05u.bmp", g_hd_pending);
        FILE *fh = fopen(name, "wb");
        if (fh) {
            uint32_t w = g_hd_w, h = g_hd_h, sz = 54 + w * h * 4;
            uint8_t hd[54] = { 'B', 'M' };
            int32_t ih = -(int32_t)h;
            memcpy(hd + 2, &sz, 4); hd[10] = 54; hd[14] = 40;
            memcpy(hd + 18, &w, 4); memcpy(hd + 22, &ih, 4); hd[26] = 1; hd[28] = 32;
            fwrite(hd, 1, 54, fh);
            const uint8_t *q = (const uint8_t *)g_hd.ptr;
            for (uint32_t i = 0; i < w * h; i++) { uint8_t bgra[4] = { q[4 * i + 2], q[4 * i + 1], q[4 * i], 255 }; fwrite(bgra, 1, 4, fh); }
            fclose(fh);
        }
        g_hd_pending = 0;
    }
    t->gbytes = t->gstride * t->h * bpp;
    t->ghash = guest_hash(t->addr, t->gbytes);
}

static void ctx_complete(GCtx *c) {
    if (!c->busy) return;
    double w0 = prof_now();
    vkWaitForFences(g_dev, 1, &c->fence, VK_TRUE, UINT64_MAX);
    g_prof_wait += prof_now() - w0;
    g_espera_mot[g_mot & 7] += prof_now() - w0;
    c->busy = 0;
    Target *t = c->tgt;
    c->tgt = NULL;
    /* Si después se envió otra escena sobre la misma superficie, la copia la hará esa. */
    if (t && t->pend == (int)(c - g_ctx) + 1) { t->pend = 0; scene_finish(t); }
}
static void ctx_complete_all(void) {   /* de la más antigua a la más reciente */
    int m = g_mot;
    for (int k = 1; k <= g_nctx; k++) { g_mot = m; ctx_complete(&g_ctx[(g_ctx_i + k) % g_nctx]); }
}
void gpu_sync_all(void) { gpu_sync_all_m(3); }
void gpu_sync_all_m(int motivo) {   /* 3 fotograma/texturas viejas, 7 sceGxmFinish */
    if (!g_ok) return;
    EnterCriticalSection(&g_cs);
    g_mot = motivo;
    ctx_complete_all();
    LeaveCriticalSection(&g_cs);
}
/* La CPU va a leer [a, a+n) de la memoria guest: completar las escenas pendientes que la escriben. */
static void pend_overlap(uint32_t a, uint32_t n) {
    for (int k = 0; k < g_nctx; k++) {
        Target *t = g_ctx[k].busy ? g_ctx[k].tgt : NULL;
        if (t && !t->resident && a < t->addr + t->gstride * t->h * g_tfmt_bpp[t->tf] && t->addr < a + n) CTXC(&g_ctx[k], 4);
    }
}

void gpu_scene_end(void) {
    if (!g_ok || !g_cur) return;
    Target *t = g_cur;
    vkCmdEndRenderPass(g_cmd);                                 /* deja TRANSFER_SRC */
    if (t->dsaddr && t->dtx) {   /* su profundidad se lee como textura: a SHADER_READ_ONLY */
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t->dimg;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &b);
        t->ds_lectura = 1;
    }
    static uint32_t volcar = 1;
    if (volcar == 1) {
        const char *e = env_cached("MLB15_VOLCAR_SUPERFICIE");
        if (!e) e = env_cached("MLB15_NEGRO_MASCARA");
        volcar = e ? (uint32_t)strtoul(e, NULL, 16) : 0;
    }
    if (!t->resident || (volcar && t->addr == volcar)) {
        VkImage from = t->img;
        if (t->simg != t->img) {   /* resolución interna: reducir a tamaño nativo (filtro lineal = media 2x2) */
            barrier(g_cmd, t->simg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageBlit b = { 0 };
            b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1;
            b.dstSubresource = b.srcSubresource;
            b.srcOffsets[1].x = (int32_t)t->sw; b.srcOffsets[1].y = (int32_t)t->sh; b.srcOffsets[1].z = 1;
            b.dstOffsets[1].x = (int32_t)t->w; b.dstOffsets[1].y = (int32_t)t->h; b.dstOffsets[1].z = 1;
            vkCmdBlitImage(g_cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->simg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &b, t->raw16 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
            barrier(g_cmd, t->simg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            from = t->simg;
        }
        VkBufferImageCopy r = { 0 };
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.imageExtent.width = t->w; r.imageExtent.height = t->h; r.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(g_cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->staging.buf, 1, &r);
    }
    {   /* MLB15_VOLCAR_HD=n: cada n escenas de una superficie de pantalla guarda la imagen a
         * resolución interna en frames\hd_NNNNN.bmp (para ver la nitidez sin ventana) */
        static int every = -1;
        static unsigned nsc;
        if (every < 0) { const char *e = env_cached("MLB15_VOLCAR_HD"); every = e ? atoi(e) : 0; }
        extern int port_display_is_framebuf(uint32_t addr);
        if (every > 0 && t->tf == 0 && port_display_is_framebuf(t->addr) && ++nsc % (unsigned)every == 0) {
            VkDeviceSize need = (VkDeviceSize)t->sw * t->sh * 4;
            if (g_hd.size < need) {
                memset(&g_hd, 0, sizeof(g_hd));   /* (la anterior se queda: solo depuración) */
                buffer_new_ex(&g_hd, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);
            }
            if (g_hd.buf) {
                VkBufferImageCopy r = { 0 };
                r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                r.imageSubresource.layerCount = 1;
                r.imageExtent.width = t->sw; r.imageExtent.height = t->sh; r.imageExtent.depth = 1;
                vkCmdCopyImageToBuffer(g_cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_hd.buf, 1, &r);
                g_hd_pending = nsc; g_hd_w = t->sw; g_hd_h = t->sh;
            }
        }
    }
    /* Entre escenas la imagen queda lista para muestrearse (texturas que leen
     * una superficie dibujada por la GPU, ver texture_get). */
    barrier(g_cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    t->drawn = 1;
    {
        static uint32_t scene_stamp;
        t->stamp = ++scene_stamp;
    }
    vkEndCommandBuffer(g_cmd);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkCommandBuffer cbs[2];
    int ncb = 0;
    if (g_ctx[g_ctx_i].u_open) { vkEndCommandBuffer(g_ctx[g_ctx_i].ucmd); cbs[ncb++] = g_ctx[g_ctx_i].ucmd; g_ctx[g_ctx_i].u_open = 0; }
    cbs[ncb++] = g_cmd;
    si.commandBufferCount = (uint32_t)ncb;
    si.pCommandBuffers = cbs;
    vkResetFences(g_dev, 1, &g_fence);
    g_cnt_escenas++;
    if (!t->resident) g_cnt_vuelta++;
    {   /* MLB15_ESCENAS_HISTO=1: cada ~1000 escenas, las combinaciones (tamaño, formato, residente) más frecuentes */
        static int on = -1;
        static struct { uint32_t w, h, tf, res, addr, n; } hs[64];
        static unsigned tot;
        if (on < 0) on = env_cached("MLB15_ESCENAS_HISTO") ? 1 : 0;
        if (on) {
            int k;
            for (k = 0; k < 64 && hs[k].n; k++)
                if (hs[k].w == t->w && hs[k].h == t->h && hs[k].tf == (uint32_t)t->tf && hs[k].res == (uint32_t)t->resident) break;
            if (k < 64) { if (!hs[k].n) { hs[k].w = t->w; hs[k].h = t->h; hs[k].tf = (uint32_t)t->tf; hs[k].res = (uint32_t)t->resident; hs[k].addr = t->addr; } hs[k].n++; }
            if (++tot % 4000 == 0) {
                printf("[histo] escenas (ultimas 4000):");
                for (k = 0; k < 64 && hs[k].n; k++) printf(" %ux%u/tf%u/%s x%u (0x%08X)", hs[k].w, hs[k].h, hs[k].tf, hs[k].res ? "res" : "mem", hs[k].n, hs[k].addr);
                printf("\n");
                memset(hs, 0, sizeof(hs));
            }
        }
    }
    ub_tarde_aplica();
    vkQueueSubmit(g_queue, 1, &si, g_fence);
    {
        GCtx *c = &g_ctx[g_ctx_i];
        c->busy = 1; c->tgt = t; t->pend = g_ctx_i + 1;
        static int sincrono = -1;   /* trazas que leen la escena recién terminada: esperar como antes */
        if (sincrono < 0)
            sincrono = env_cached("MLB15_VOLCAR_SUPERFICIE") || env_cached("MLB15_NEGRO_MASCARA") || env_cached("MLB15_ESCENA_TAM") ||
                       env_cached("MLB15_TONO_TRAZA") || (env_cached("MLB15_VOLCAR_POST") && atoi(env_cached("MLB15_VOLCAR_POST")) > 0) ||
                       (env_cached("MLB15_VOLCAR_HD") && atoi(env_cached("MLB15_VOLCAR_HD")) > 0);
        if (sincrono || g_nctx == 1 || g_hd_pending) CTXC(c, 5);
    }
    g_cur = NULL;
    LeaveCriticalSection(&g_cs);
}

void gpu_stats(uint32_t *scenes, uint32_t *draws, uint32_t *textures) {
    if (scenes) *scenes = g_stat_scenes;
    if (draws) *draws = g_stat_draws;
    if (textures) *textures = g_ntex;
}

/* --- ventana: swapchain y presentación -------------------------------------- */

static int swapchain_create(void) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_phys, g_surface, &caps);
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[32];
    if (nf > 32) nf = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &nf, fmts);
    VkSurfaceFormatKHR fmt = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) { fmt = fmts[i]; break; }
    g_swap_bgr = fmt.format == VK_FORMAT_B8G8R8A8_UNORM || fmt.format == VK_FORMAT_B8G8R8A8_SRGB;
    /* MAILBOX si lo hay (el juego ya marca su ritmo), si no FIFO. */
    uint32_t npm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &npm, NULL);
    VkPresentModeKHR pms[8], mode = VK_PRESENT_MODE_FIFO_KHR;
    if (npm > 8) npm = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &npm, pms);
    for (uint32_t i = 0; i < npm; i++) if (pms[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = pms[i];

    g_swap_ext = caps.currentExtent;
    if (g_swap_ext.width == UINT32_MAX) {
        int w, h;
        SDL_Vulkan_GetDrawableSize(g_win, &w, &h);
        g_swap_ext.width = (uint32_t)w; g_swap_ext.height = (uint32_t)h;
    }
    if (!g_swap_ext.width || !g_swap_ext.height) return 0;                /* minimizada */
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR si = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    si.surface = g_surface;
    si.minImageCount = count;
    si.imageFormat = fmt.format;
    si.imageColorSpace = fmt.colorSpace;
    si.imageExtent = g_swap_ext;
    si.imageArrayLayers = 1;
    si.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    si.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    si.preTransform = caps.currentTransform;
    si.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    si.presentMode = mode;
    si.clipped = VK_TRUE;
    si.oldSwapchain = g_swap;
    VkSwapchainKHR sw;
    if (vkCreateSwapchainKHR(g_dev, &si, NULL, &sw) != VK_SUCCESS) return 0;
    if (g_swap) vkDestroySwapchainKHR(g_dev, g_swap, NULL);
    g_swap = sw;
    g_nswap = 8;
    vkGetSwapchainImagesKHR(g_dev, g_swap, &g_nswap, g_swap_img);
    printf("[gpu] ventana %ux%u, %u imágenes, modo %s\n", g_swap_ext.width, g_swap_ext.height, g_nswap,
           mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo");
    fflush(stdout);
    return 1;
}

int gpu_has_window(void) { return g_ok && g_win != NULL; }

/* ---- Rótulo en la ventana: FPS (F10) y avisos (F9 resolución) --------------------------
 * Texto con una fuente de 5x7 ampliada, escrito en un búfer y copiado a la imagen de la
 * ventana después del escalado (no toca la imagen del juego ni las capturas). */
#define OSD_ESC   3                         /* cada punto de la fuente = 3x3 píxeles */
#define OSD_W     (64 * 6 * OSD_ESC)
#define OSD_H     (2 * 9 * OSD_ESC + 2 * OSD_ESC)
static int g_osd_fps = -1;                  /* [video] mostrar_fps */
static char g_osd_aviso[64];
static ULONGLONG g_osd_aviso_hasta;
static Buffer g_osd_buf;
static const struct { char c; uint8_t f[7]; } g_fuente[] = {
    { '0', { 14, 17, 19, 21, 25, 17, 14 } }, { '1', { 4, 12, 4, 4, 4, 4, 14 } },
    { '2', { 14, 17, 1, 2, 4, 8, 31 } },      { '3', { 31, 2, 4, 2, 1, 17, 14 } },
    { '4', { 2, 6, 10, 18, 31, 2, 2 } },      { '5', { 31, 16, 30, 1, 1, 17, 14 } },
    { '6', { 6, 8, 16, 30, 17, 17, 14 } },    { '7', { 31, 1, 2, 4, 8, 8, 8 } },
    { '8', { 14, 17, 17, 14, 17, 17, 14 } },  { '9', { 14, 17, 17, 15, 1, 2, 12 } },
    { 'A', { 14, 17, 17, 31, 17, 17, 17 } },  { 'C', { 14, 17, 16, 16, 16, 17, 14 } },
    { 'D', { 28, 18, 17, 17, 17, 18, 28 } },  { 'E', { 31, 16, 16, 30, 16, 16, 31 } },
    { 'F', { 31, 16, 16, 30, 16, 16, 16 } },  { 'I', { 14, 4, 4, 4, 4, 4, 14 } },
    { 'L', { 16, 16, 16, 16, 16, 16, 31 } },  { 'N', { 17, 17, 25, 21, 19, 17, 17 } },
    { 'O', { 14, 17, 17, 17, 17, 17, 14 } },  { 'P', { 30, 17, 17, 30, 16, 16, 16 } },
    { 'R', { 30, 17, 17, 30, 20, 18, 17 } },  { 'S', { 15, 16, 16, 14, 1, 1, 30 } },
    { 'U', { 17, 17, 17, 17, 17, 17, 14 } },  { 'X', { 17, 17, 10, 4, 10, 17, 17 } },
    { 'T', { 31, 4, 4, 4, 4, 4, 4 } },        { 'V', { 17, 17, 17, 17, 17, 10, 4 } },
    { 'Z', { 31, 1, 2, 4, 8, 16, 31 } },      { 'G', { 14, 17, 16, 23, 17, 17, 15 } },
    { 'M', { 17, 27, 21, 21, 17, 17, 17 } },  { 'B', { 30, 17, 17, 30, 17, 17, 30 } },
    { '.', { 0, 0, 0, 0, 0, 12, 12 } },       { ':', { 0, 12, 12, 0, 12, 12, 0 } },
    { '(', { 2, 4, 8, 8, 8, 4, 2 } },         { ')', { 8, 4, 2, 2, 2, 4, 8 } },
};

/* Pinta las líneas en g_osd_buf (fondo negro translúcido no: opaco, con margen) y devuelve
 * ancho y alto usados. */
static void osd_pintar(const char *l1, const char *l2, uint32_t *ow, uint32_t *oh) {
    const char *ls[2] = { l1, l2 };
    size_t n = strlen(l1) > (l2 ? strlen(l2) : 0) ? strlen(l1) : strlen(l2);
    if (n > 64) n = 64;
    uint32_t w = (uint32_t)n * 6 * OSD_ESC + 2 * OSD_ESC, h = (l2 ? 2 : 1) * 9 * OSD_ESC + OSD_ESC;
    uint32_t *px = (uint32_t *)g_osd_buf.ptr;
    for (uint32_t i = 0; i < w * h; i++) px[i] = 0xFF000000u;
    uint32_t blanco = 0xFFFFFFFFu, amarillo = g_swap_bgr ? 0xFFFFE000u : 0xFF00E0FFu;
    for (int li = 0; li < 2 && ls[li]; li++)
        for (size_t k = 0; k < n && ls[li][k]; k++) {
            char c = ls[li][k];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            const uint8_t *f = NULL;
            for (size_t q = 0; q < sizeof(g_fuente) / sizeof(g_fuente[0]); q++) if (g_fuente[q].c == c) f = g_fuente[q].f;
            if (!f) continue;
            for (int y = 0; y < 7; y++)
                for (int x = 0; x < 5; x++)
                    if (f[y] >> (4 - x) & 1)
                        for (int dy = 0; dy < OSD_ESC; dy++)
                            for (int dx = 0; dx < OSD_ESC; dx++) {
                                uint32_t X = OSD_ESC + ((uint32_t)k * 6 + (uint32_t)x) * OSD_ESC + (uint32_t)dx;
                                uint32_t Y = OSD_ESC + ((uint32_t)li * 9 + (uint32_t)y) * OSD_ESC + (uint32_t)dy;
                                px[Y * w + X] = li ? amarillo : blanco;
                            }
        }
    *ow = w; *oh = h;
}

/* Graba la copia del rótulo a la imagen de la ventana (ya en TRANSFER_DST). */
static void osd_record(VkCommandBuffer cb, VkImage dst) {
    static ULONGLONG t0;
    static unsigned n, fps;
    ULONGLONG now = GetTickCount64();
    n++;
    if (!t0) t0 = now;
    if (now - t0 >= 1000) { fps = (unsigned)(n * 1000 / (now - t0)); n = 0; t0 = now; }
    if (g_osd_fps < 0) g_osd_fps = launcher_ini_int("video", "mostrar_fps", 0) ? 1 : 0;
    int aviso = g_osd_aviso[0] && now < g_osd_aviso_hasta;
    if (!g_osd_fps && !aviso) return;
    if (!g_osd_buf.buf && !buffer_new(&g_osd_buf, (VkDeviceSize)OSD_W * OSD_H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return;
    char l1[32];
    snprintf(l1, sizeof(l1), "%u FPS", fps);
    uint32_t w, h;
    if (g_osd_fps) osd_pintar(l1, aviso ? g_osd_aviso : NULL, &w, &h);
    else osd_pintar(g_osd_aviso, NULL, &w, &h);
    if (w > g_swap_ext.width || h > g_swap_ext.height) return;
    VkBufferImageCopy c = { 0 };
    c.bufferRowLength = w;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageOffset.x = 8; c.imageOffset.y = 8;
    if (c.imageOffset.x + w > g_swap_ext.width) c.imageOffset.x = 0;
    if (c.imageOffset.y + h > g_swap_ext.height) c.imageOffset.y = 0;
    c.imageExtent.width = w; c.imageExtent.height = h; c.imageExtent.depth = 1;
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkCmdCopyBufferToImage(cb, g_osd_buf.buf, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

static void manejar_evento(SDL_Event *ep) {
    SDL_Event e = *ep;
    {
        int full = (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP;
        /* Alt+Enter o F11: pantalla completa (se guarda en MLB15.ini). En pantalla
         * completa Esc solo sale de ella; en ventana cierra el port. */
        if (e.type == SDL_KEYDOWN && !e.key.repeat &&
            ((e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)) || e.key.keysym.sym == SDLK_F11 ||
             (e.key.keysym.sym == SDLK_ESCAPE && full))) {
            full = !full;
            SDL_SetWindowFullscreen(g_win, full ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            launcher_ini_set_int("video", "pantalla_completa", full);
            return;                         /* que Enter no llegue al juego como Start */
        }
        /* F10: mostrar/ocultar los FPS ([video] mostrar_fps). */
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym == SDLK_F10) {
            if (g_osd_fps < 0) g_osd_fps = launcher_ini_int("video", "mostrar_fps", 0) ? 1 : 0;
            g_osd_fps = !g_osd_fps;
            launcher_ini_set_int("video", "mostrar_fps", g_osd_fps);
            return;
        }
        /* F9: resolución interna x1 -> x2 -> x3 -> x4 -> x1 ([video] resolucion). Las superficies
         * ya creadas tienen el tamaño de la anterior, así que se aplica al reiniciar el juego. */
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym == SDLK_F9) {
            int r = launcher_ini_int("video", "resolucion", (int)g_res);
            r = r >= 4 ? 1 : r + 1;
            launcher_ini_set_int("video", "resolucion", r);
            if ((uint32_t)r == g_res) snprintf(g_osd_aviso, sizeof(g_osd_aviso), "RESOLUCION X%d (ACTUAL)", r);
            else snprintf(g_osd_aviso, sizeof(g_osd_aviso), "RESOLUCION X%d AL REINICIAR", r);
            g_osd_aviso_hasta = GetTickCount64() + 3000;
            printf("[gpu] F9: resolución x%d guardada (ahora x%u)\n", r, g_res);
            return;
        }
        if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) {
            printf("[gpu] ventana cerrada: fin\n");
            fflush(stdout);
            ExitProcess(0);
        }
        input_event(&e);
    }
}

static void pump_events(void) {
    if (!g_win_propio) {   /* sin hilo de ventana (no debería pasar): como antes */
        SDL_Event e;
        while (SDL_PollEvent(&e)) manejar_evento(&e);
    }
    input_poll();
}

void gpu_present(uint32_t addr, uint32_t w, uint32_t h, uint32_t stride) {
    if (!gpu_has_window()) return;
    EnterCriticalSection(&g_cs);
    pump_events();
    if (!g_swap && !swapchain_create()) { LeaveCriticalSection(&g_cs); return; }
    /* La presentación anterior: se espera aquí y no justo tras enviarla, para que la GPU dibuje
     * ese fotograma mientras el juego prepara el siguiente (MLB15_PRESENTAR_ESPERA=1: como antes). */
    static int g_present_busy;
    if (g_present_busy) {
        double w0 = prof_now();
        vkWaitForFences(g_dev, 1, &g_fence_present, VK_TRUE, UINT64_MAX);
        g_espera_mot[3] += prof_now() - w0;
        g_present_busy = 0;
    }

    uint32_t idx;
    VkResult r = vkAcquireNextImageKHR(g_dev, g_swap, UINT64_MAX, g_sem_acq, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(g_dev);
        swapchain_create();
        LeaveCriticalSection(&g_cs);
        return;
    }
    if (r != VK_SUCCESS) { LeaveCriticalSection(&g_cs); return; }

    /* La imagen del juego: la que dibujó Vulkan, o su memoria si la escribió la CPU. */
    Target *t = NULL;
    for (int i = 0; i < MAX_TARGETS; i++)   /* la última escena dibujada en esa dirección */
        if (g_targets[i].img && g_targets[i].addr == addr && g_targets[i].w == w && g_targets[i].h == h &&
            (!t || g_targets[i].stamp > t->stamp)) t = &g_targets[i];
    int upload_cpu = 0;
    if (env_cached("MLB15_PRESENT_TRAZA")) {
        static unsigned n;
        if (n++ % 120 == 0) {
            printf("[present] 0x%08X %ux%u stride %u ->", addr, w, h, stride);
            for (int i = 0; i < MAX_TARGETS; i++)
                if (g_targets[i].img && g_targets[i].addr == addr)
                    printf(" [%ux%u tf %d stamp %u res %d%s]", g_targets[i].w, g_targets[i].h, g_targets[i].tf,
                           (unsigned)g_targets[i].stamp, g_targets[i].resident, t == &g_targets[i] ? " ELEGIDO" : "");
            printf("\n");
        }
    }
    /* sRGB: con el pase final se lee con una vista UNORM; sin él (o con otro formato) se
     * presenta desde la memoria guest. */
    if (t && t->tf != 0 && !(t->tf == 5 && pp_on() && target_present_view(t))) t = NULL;
    if (!t) {
        t = target_for(addr, w, h, 0);
        upload_cpu = 1;
        if (!t) { LeaveCriticalSection(&g_cs); return; }
    }

    /* Escalado al tamaño de la ventana conservando 960x544 (16:9 aprox.). */
    uint32_t ww = g_swap_ext.width, wh = g_swap_ext.height, dw = ww, dh = ww * h / w;
    if (dh > wh) { dh = wh; dw = wh * w / h; }
    int32_t ox = (int32_t)(ww - dw) / 2, oy = (int32_t)(wh - dh) / 2;
    VkImage pimg = upload_cpu ? t->simg : t->img;   /* la memoria guest va a tamaño nativo */
    VkImageView pview = upload_cpu ? (t->simg != t->img ? t->sview : t->view) : target_present_view(t);
    uint32_t pw = upload_cpu ? w : t->sw, ph = upload_cpu ? h : t->sh;
    int post = dw && dh && pp_prepare(pview, pw, ph, dw, dh);
    if (env_cached("MLB15_PRESENT_TRAZA")) {
        static unsigned n2;
        if (n2++ % 120 == 0)
            printf("[present] cpu %d tf %d origen %ux%u destino %ux%u pase %d\n", upload_cpu, t->tf, pw, ph, dw, dh, post);
    }

    VkCommandBuffer cb = g_cmd_present;
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    if (upload_cpu) {
        const uint8_t *src = (const uint8_t *)vita_mem_ptr(addr, (stride ? stride : w) * h * 4);
        uint8_t *dst = (uint8_t *)t->staging.ptr;
        for (uint32_t y = 0; y < h && src; y++)
            memcpy(dst + (size_t)y * w * 4, src + (size_t)y * (stride ? stride : w) * 4, (size_t)w * 4);
        barrier(cb, t->simg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy c = { 0 };
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = w; c.imageExtent.height = h; c.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cb, t->staging.buf, t->simg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(cb, t->simg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                post ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, post ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, post ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT);
    } else if (!post) {   /* entre escenas las superficies quedan para muestrear */
        barrier(cb, t->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
    }
    if (post) pp_record(cb);   /* g_pp_out: ya al tamaño de destino */
    barrier(cb, g_swap_img[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit bl = { 0 };
    bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bl.srcSubresource.layerCount = 1;
    bl.dstSubresource = bl.srcSubresource;
    bl.srcOffsets[1].x = (int32_t)(post ? dw : pw); bl.srcOffsets[1].y = (int32_t)(post ? dh : ph); bl.srcOffsets[1].z = 1;
    bl.dstOffsets[0].x = ox; bl.dstOffsets[0].y = oy;
    bl.dstOffsets[1].x = ox + (int32_t)dw; bl.dstOffsets[1].y = oy + (int32_t)dh; bl.dstOffsets[1].z = 1;
    vkCmdBlitImage(cb, post ? g_pp_out.img : pimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_swap_img[idx],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, post ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    if (!post)
        barrier(cb, pimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    osd_record(cb, g_swap_img[idx]);
    barrier(cb, g_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(cb);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &g_sem_acq; si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &g_sem_done;
    vkResetFences(g_dev, 1, &g_fence_present);
    vkQueueSubmit(g_queue, 1, &si, g_fence_present);
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &g_sem_done;
    pi.swapchainCount = 1; pi.pSwapchains = &g_swap; pi.pImageIndices = &idx;
    r = vkQueuePresentKHR(g_queue, &pi);
    {
        static int espera = -1, vv = -1;
        if (espera < 0) { espera = env_cached("MLB15_PRESENTAR_ESPERA") ? 1 : 0; const char *e = env_cached("MLB15_VOLCAR_VENTANA"); vv = e && atoi(e) > 0; }
        if (espera || vv) vkWaitForFences(g_dev, 1, &g_fence_present, VK_TRUE, UINT64_MAX);
        else g_present_busy = 1;
    }
    {   /* MLB15_VOLCAR_VENTANA=n: cada n presentaciones guarda en ventana_NNNNN.bmp la imagen
         * que se manda a la ventana (la de la GPU, no la memoria guest) */
        static int every = -1;
        static unsigned np;
        if (every < 0) { const char *e = env_cached("MLB15_VOLCAR_VENTANA"); every = e ? atoi(e) : 0; }
        if (every > 0 && ++np % (unsigned)every == 0 && t->tf == 0 && t->staging.ptr && g_res == 1) {
            vkResetCommandBuffer(cb, 0);
            vkBeginCommandBuffer(cb, &bi);
            barrier(cb, t->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy c = { 0 };
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.imageSubresource.layerCount = 1;
            c.imageExtent.width = w; c.imageExtent.height = h; c.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->staging.buf, 1, &c);
            barrier(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            vkEndCommandBuffer(cb);
            VkSubmitInfo s2 = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            s2.commandBufferCount = 1; s2.pCommandBuffers = &cb;
            vkResetFences(g_dev, 1, &g_fence_present);
            vkQueueSubmit(g_queue, 1, &s2, g_fence_present);
            vkWaitForFences(g_dev, 1, &g_fence_present, VK_TRUE, UINT64_MAX);
            char name[64];
            snprintf(name, sizeof(name), "frames\\ventana_%05u.bmp", np);
            FILE *fh = fopen(name, "wb");
            if (fh) {
                uint32_t sz = 54 + w * h * 4;
                uint8_t hd[54] = { 'B', 'M' };
                int32_t ih = -(int32_t)h;
                memcpy(hd + 2, &sz, 4); hd[10] = 54; hd[14] = 40;
                memcpy(hd + 18, &w, 4); memcpy(hd + 22, &ih, 4); hd[26] = 1; hd[28] = 32;
                fwrite(hd, 1, 54, fh);
                const uint8_t *src = (const uint8_t *)t->staging.ptr;
                for (uint32_t i = 0; i < w * h; i++) {
                    uint8_t bgra[4] = { src[4 * i + 2], src[4 * i + 1], src[4 * i], 255 };
                    fwrite(bgra, 1, 4, fh);
                }
                fclose(fh);
            }
        }
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(g_dev); swapchain_create(); }
    LeaveCriticalSection(&g_cs);
}
