/* Arranque del port: juego del usuario, MLB15.ini y ELF listo (launcher.c). */
#ifndef PORT_LAUNCHER_H
#define PORT_LAUNCHER_H

#include <stddef.h>

/* Busca/pide el juego, guarda MLB15.ini, deja el ELF en datos\eboot.elf y
 * define MLB15_APP0. Devuelve 0 si se puede arrancar. */
int launcher_prepare(char *elf_out, size_t max);

const char *launcher_juego_dll(void);   /* datos\juego.dll (tras launcher_prepare) */
void launcher_console(void);      /* doble clic: registro a datos/log.txt, sin consola */
const char *launcher_ini_path(void);
int  opciones_dialogo(void);   /* opciones.c: 1 = jugar, 0 = salir */
int  launcher_ini_int(const char *section, const char *key, int def);
void launcher_ini_set_int(const char *section, const char *key, int v);

#endif
