/* Traductor de shaders de la Vita (GXP / USSE) a GLSL para Vulkan.
 *
 * El formato de instrucción se reconstruyó comparando, instrucción a
 * instrucción, con el desensamblado que deja Vita3K en shaderlog\ (solo como
 * oráculo: aquí no hay código suyo). Las herramientas de esa investigación
 * están en VitaRE\tools\usse\.
 *
 * Modelo de ejecución del GLSL generado:
 *   - Bancos pa, sa, r, o como arrays de uint (palabras de 32 bits); los F16
 *     son mitades de palabra. Los internos i0-i3 son vec4 en coma flotante.
 *   - El programa secundario (preparación de uniformes) se ejecuta en cada
 *     invocación, antes del primario.
 *   - Memoria de uniformes: un SSBO por draw (set 0, binding 0) con la imagen
 *     inicial de sa de cada etapa y copias de los buffers de uniformes; los LDR
 *     leen de ahí con direcciones relativas al bloque del draw.
 *   - Texturas: set 1, binding = unidad (sampler2D).
 *   - Varyings: v_tc[0..9] (locations 0-9) y v_col0 (location 10), vec4.
 */
#ifndef PORT_USSE_H
#define PORT_USSE_H

#include <stddef.h>
#include <stdint.h>

#define USSE_MAX_UB   16
#define USSE_MAX_LIT  128
#define USSE_MAX_ATTR 16

typedef struct {
    int      is_frag;
    uint32_t sa_count;                       /* palabras de sa que usa el programa */
    uint32_t sa_words;                       /* palabras de la imagen inicial de sa que lee el GLSL */
    uint32_t defub_words;                    /* sa[0..defub_words) = buffer por defecto */
    uint32_t nub;
    struct { uint16_t buf, sa; uint32_t bytes; } ub[USSE_MAX_UB];   /* sa[sa] = dirección del buffer buf */
    uint32_t nlit;
    struct { uint16_t sa; uint32_t val; } lit[USSE_MAX_LIT];
    /* vértice: atributos (registro pa, tipo del parámetro, componentes) */
    uint32_t nattr;
    struct { uint16_t reg; uint8_t type, comps; } attr[USSE_MAX_ATTR];
    /* texturas que muestrea (unidades) */
    uint32_t tex_units, tex_cube;
    uint32_t tex_shadow;                     /* unidades cuyo nombre lleva "Shadow" (render targets de sombras) */
    /* salida de fragmento: 0 = F16x4 en pa0-pa1, 1 = U8x4 en pa0 */
    int      out_u8;
} UsseInfo;

/* Traduce un GXP completo (puntero host, `len` bytes). Devuelve un texto GLSL
 * reservado con malloc, o NULL si el programa usa algo aún no soportado (el
 * motivo va en err). */
char *usse_translate(const uint8_t *gxp, size_t len, UsseInfo *info, char *err, size_t errlen);

/* Depuración: >= 0 hace que el fragmento devuelva la pareja de entradas pa n. */
extern int usse_debug_input;

/* Desensambla (texto de depuración, una instrucción por línea). */
char *usse_disasm(const uint8_t *gxp, size_t len);

#endif
