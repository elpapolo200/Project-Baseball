#ifndef PORT_SHC_H
#define PORT_SHC_H
#include <stddef.h>
#include <stdint.h>
/* GLSL (versión 450, Vulkan) a SPIR-V. Devuelve 1 si va bien; *spv con malloc. */
int shc_compile(const char *glsl, int frag, uint32_t **spv, size_t *nwords, char *err, size_t errlen);
#endif
