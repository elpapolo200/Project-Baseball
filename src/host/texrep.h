/* Texturas mejoradas por contenido (texrep.c). */
#ifndef PORT_TEXREP_H
#define PORT_TEXREP_H

#include <stdint.h>

#include "gpu.h"

/* RGBA (malloc, lo libera quien llama) del PNG que reemplaza a t, o NULL. */
uint32_t *texrep_find(const GpuTexture *t, uint32_t *w, uint32_t *h);
/* MLB15_TEX_VOLCAR=<carpeta>: guarda la textura decodificada con su nombre de huella. */
void texrep_volcar(const GpuTexture *t, const uint32_t *rgba);

#endif
