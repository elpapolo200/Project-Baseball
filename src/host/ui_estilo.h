/* ui_estilo.h — aspecto común de las ventanas del launcher (oscuro, acento azul/rojo béisbol).
 * Coordenadas en unidades lógicas de 96 ppp: ui_px() las pasa a píxeles del monitor. */
#ifndef MLB15_UI_ESTILO_H
#define MLB15_UI_ESTILO_H
#include <windows.h>

enum { UI_TITULO, UI_SUBTITULO, UI_FUERTE, UI_NORMAL, UI_NORMAL_TENUE, UI_PEQUENO, UI_PEQUENO_TENUE, UI_ENORME, UI_NFUENTES };

/* colores */
#define UI_FONDO      RGB(16, 19, 26)
#define UI_PANEL      RGB(25, 30, 40)
#define UI_PANEL2     RGB(33, 39, 52)
#define UI_BORDE      RGB(48, 56, 72)
#define UI_TEXTO      RGB(236, 240, 246)
#define UI_TENUE      RGB(146, 156, 174)
#define UI_ACENTO     RGB(28, 120, 230)
#define UI_ACENTO2    RGB(60, 150, 255)
#define UI_ROJO       RGB(214, 40, 57)

void   ui_init(void);                       /* DPI por monitor, fuentes, GDI+ */
int    ui_px(int v);                        /* lógico -> píxeles */
int    ui_lg(int px);                       /* píxeles -> lógico */
HFONT  ui_fuente(int estilo);
void   ui_ventana_oscura(HWND h);           /* barra de título oscura (Windows 10/11) */
void   ui_pinta_fondo(HWND h, HDC dc, const RECT *rc);   /* fondo + franja superior con el logo */
/* Texto con ajuste de línea en (x, y) lógico y ancho w lógico; devuelve la altura lógica usada. */
int    ui_texto(HWND h, HDC dc, const wchar_t *t, int x, int y, int w, int estilo);
LRESULT ui_color_control(HWND h, HDC dc);
void   ui_pinta_boton(const DRAWITEMSTRUCT *d);   /* BS_OWNERDRAW: botón normal o primario (ver ui_boton) */
void   ui_estilo_barra(HWND barra);         /* barra de progreso con los colores del tema */

/* Rectángulo redondeado relleno con suavizado (GDI+); r en píxeles. */
void   ui_rect_redondo(HDC dc, int x, int y, int w, int h, int r, COLORREF relleno, COLORREF borde);
void   ui_degradado(HDC dc, const RECT *rc, COLORREF a, COLORREF b, int vertical);
void   ui_pelota(HDC dc, int cx, int cy, int r, int alfa);   /* pelota de béisbol dibujada (decoración) */

/* Controles propios (BS_OWNERDRAW). El tipo va en GWLP_USERDATA del botón. */
enum { UIB_NORMAL = 0, UIB_PRIMARIO = 1, UIB_INTERRUPTOR = 2, UIB_SEGMENTO = 3, UIB_PELIGRO = 4 };
HWND   ui_boton(HWND padre, int id, const wchar_t *t, int x, int y, int w, int h, int tipo);
/* Interruptor (on/off) con texto a la izquierda. */
HWND   ui_interruptor(HWND padre, int id, const wchar_t *t, int x, int y, int w, int on);
int    ui_interruptor_on(HWND b);
void   ui_interruptor_pon(HWND b, int on);
/* Selector segmentado: n botones con ids id..id+n-1; ui_segmento_elige marca uno. */
void   ui_segmentos(HWND padre, int id, const wchar_t *const *t, int n, int x, int y, int w, int h, int sel);
void   ui_segmento_elige(HWND padre, int id, int n, int sel);
int    ui_segmento_actual(HWND padre, int id, int n);
/* Deslizador propio: envía WM_HSCROLL al padre al moverse. */
HWND   ui_deslizador(HWND padre, int id, int x, int y, int w, int mn, int mx, int v);
int    ui_deslizador_pos(HWND d);
void   ui_deslizador_pon(HWND d, int v);
int    ui_comando(HWND padre, WPARAM w, LPARAM l);   /* en WM_COMMAND: interruptores y segmentos */
void   ui_boton_fondo(HWND b, COLORREF c);           /* color que hay detrás del botón */
/* Seguimiento del ratón para el resaltado (llamar desde el WM_MOUSEMOVE de los botones, ya hecho
 * con una subclase en ui_boton). */
#endif
