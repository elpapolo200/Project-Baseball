/* Entrada del PC (input.c) para el pad de la Vita (hle_ctrl.c). */
#ifndef PORT_INPUT_H
#define PORT_INPUT_H

#include <stdint.h>

/* Botones de SceCtrlData.buttons */
enum {
    VITA_SELECT = 0x0001, VITA_START = 0x0008,
    VITA_UP = 0x0010, VITA_RIGHT = 0x0020, VITA_DOWN = 0x0040, VITA_LEFT = 0x0080,
    VITA_L = 0x0100, VITA_R = 0x0200,
    VITA_TRIANGLE = 0x1000, VITA_CIRCLE = 0x2000, VITA_CROSS = 0x4000, VITA_SQUARE = 0x8000,
};

union SDL_Event;
void input_init(void);                         /* carga mapeo desde MLB15.ini [teclas] */
void input_event(const union SDL_Event *e);   /* hilo de la ventana */
void input_poll(void);                         /* una vez por fotograma */

uint32_t input_buttons(void);                  /* cualquier hilo */
void     input_sticks(uint8_t out[4]);         /* lx, ly, rx, ry (0x80 = centro) */
int      input_touch(int *x, int *y);          /* 1 si hay dedo en el panel delantero (1920x1088) */
void     input_evento(const char *nombre);     /* FIOS: archivo abierto (para "@TEXTO+N" del guion) */

#endif /* PORT_INPUT_H */
