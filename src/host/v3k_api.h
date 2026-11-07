/* Interfaz en C del traductor de shaders de Vita3K (mlb15\recomp\v3k, GPLv2).
 *
 * v3k_translate convierte un programa GXP (USSE) en SPIR-V para Vulkan con el
 * diseño de recursos de Vita3K (sin mapeo de memoria):
 *   set 0: binding 0 = bloque de vértice (RenderVertUniformBlock, 36 B)
 *          binding 1 = bloque de fragmento (RenderFragUniformBlock, 20 B)
 *          binding 2 = SSBO con los buffers de uniformes del vértice
 *          binding 3 = SSBO con los buffers de uniformes del fragmento
 *   set 2: texturas del vértice (binding = unidad), set 3: del fragmento.
 * Los atributos se leen como vec4 por location (uvec4 si regformat). */
#ifndef V3K_API_H
#define V3K_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t reg;        /* regIndex del atributo GXM */
    uint8_t  format;     /* SceGxmAttributeFormat */
    uint8_t  comps;
} V3kAttrHint;

typedef struct {
    uint32_t    nattr;
    V3kAttrHint attr[16];
    uint32_t    tex_format[16];   /* SceGxmTextureFormat de cada unidad (0 = ABGR8) */
    uint32_t    raw16;            /* fragmento: salida cruda 4xU16 (superficie F16x4 como U16x4) */
} V3kHints;

typedef struct {
    uint16_t reg;        /* regIndex del atributo que alimenta esta entrada */
    uint8_t  loc;        /* primera location */
    uint8_t  nloc;       /* locations consecutivas (vec4 cada una) */
    uint8_t  comps;      /* componentes totales */
    uint8_t  regformat;  /* 1: uvec (componentes enteros sin convertir) */
    uint8_t  semantic;   /* 1: índice de vértice, 2: de instancia (builtins, sin location) */
} V3kAttr;

typedef struct {
    int      is_frag;
    uint32_t nattr;
    V3kAttr  attr[16];
    uint32_t nloc;               /* locations de entrada usadas */
    /* buffers de uniformes: índice de host 0 = por defecto, 1..14 = UB 0..13 */
    uint32_t buf_off[16];        /* desplazamiento en bytes dentro del SSBO (~0u = no se usa) */
    uint32_t buf_size[16];       /* bytes a copiar */
    uint32_t data_bytes;         /* tamaño total del SSBO */
    uint32_t tex_mask;           /* unidades de textura muestreadas */
    uint8_t  frag_color;         /* el fragmento lee el color de la superficie (mezcla programable) */
    uint8_t  native_color;       /* salida en formato nativo de la superficie */
    uint8_t  output_type;        /* SceGxmParameterType de la salida del fragmento */
    uint8_t  out_param_type;     /* vertex_varyings()->output_param_type */
    uint8_t  out_start;          /* vertex_varyings()->fragment_output_start (registro pa de la salida) */
} V3kInfo;

/* Devuelve 1 y *spv (liberar con v3k_free) o 0 con el error en err. */
int  v3k_translate(const void *gxp, size_t len, const V3kHints *hints, uint32_t **spv, size_t *words,
                   V3kInfo *info, char *err, size_t errlen);
void v3k_free(void *p);

#ifdef __cplusplus
}
#endif

#endif
