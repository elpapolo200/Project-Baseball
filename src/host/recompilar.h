/* recompilar.h — genera datos\juego.dll en el PC del jugador (recompilar.c). */
#ifndef MLB15_RECOMPILAR_H
#define MLB15_RECOMPILAR_H
#include <stddef.h>
/* 0 = juego.dll listo (ya estaba o se acaba de generar) en dll_out. */
int recompilar_si_hace_falta(const char *exe_dir, const char *elf, char *dll_out, size_t max);
#endif
