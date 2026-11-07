/* SceScreenShot para el port de MLB 15.
 *
 * El juego solo configura y habilita la captura de pantalla del sistema (la
 * combinacion PS+START de la Vita). Aqui no hay servicio de captura, pero la
 * llamada se acepta: rechazarla no aporta nada y el juego la trata como
 * opcional. Si algun dia se quiere implementar, la captura natural es guardar
 * el ultimo frame que presenta hle_display.c.
 */
#include <stdint.h>

#include "vitarecomp/cpu.h"

void vita_hle_sceScreenShotSetParam(void) {
    /* r0 = SceScreenShotParam* (prefijo de nombre, etc.) */
    r0 = 0;
}

void vita_hle_sceScreenShotEnable(void) {
    r0 = 0;
}
