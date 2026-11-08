/* juego_dll.h — el código del juego, en juego.dll (ver juego_dll.c). */
#ifndef MLB15_JUEGO_DLL_H
#define MLB15_JUEGO_DLL_H
#include <stdint.h>
#include "vitarecomp/recomp_rt.h"

extern void (*juego_register_functions)(void);
extern void (*juego_module_start)(void);
extern void (*juego_udiv)(void);
extern const vita_import_var *juego_import_vars;
extern uint32_t juego_import_var_count;

int juego_cargar(const char *ruta);
void juego_memoria(void);   /* tras vita_mem_init: el DLL accede a la memoria en línea */
#endif
