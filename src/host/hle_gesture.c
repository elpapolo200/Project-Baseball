/* SceSystemGesture para el port de MLB 15: reconocedor de gestos táctiles.
 *
 * El juego pasa cada fotograma los datos del panel (sceTouchPeek) a
 * UpdatePrimitiveTouchRecognizer(front, back) y luego, por cada reconocedor
 * (CreateTouchRecognizer: toque = 1, arrastre = 2, mantener = 4, pellizco = 8),
 * llama a UpdateTouchRecognizer(rec) y lee los eventos con
 * GetTouchEventsCount(rec) (devuelve el número EN r0) y GetTouchEventByIndex /
 * ByEventID(rec, i|id, ev). Pantallas como la de "SWIPE" del menú esperan un
 * arrastre: sin esto el juego parecía congelado.
 *
 * Evento (campos deducidos de cómo los lee el juego, sub_815A1B..):
 *   +0x00 u32 id  +0x04 u32 estado (1 empieza, 2 sigue, 3 termina)
 *   +0x08 u32 tipo  +0x10 u64 tiempo transcurrido (µs)
 *   toque/mantener: +0x1A s16 x, +0x1C s16 y, +0x1E u8 nº de toques
 *   arrastre:       +0x1E s16 x actual, +0x20 y actual; +0x22/+0x24 posición
 *                   inicial (el juego la lee al empezar y luego resta)
 * Coordenadas del panel delantero: 1920x1088. Sin pellizcos (no hay 2 dedos).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"

enum { G_TAP = 1, G_DRAG = 2, G_HOLD = 4, G_PINCH = 8 };
enum { S_BEGIN = 1, S_ACTIVE = 2, S_END = 3 };

#define DRAG_MIN   24        /* píxeles del panel antes de considerarlo arrastre */
#define TAP_MAX_MS 500
#define EV_SIZE    0x28

static int trace(void) {
    static int t = -1;
    if (t < 0) t = getenv("MLB15_GESTO_TRACE") ? 1 : 0;
    return t;
}

static uint64_t now_us(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000.0 / (double)f.QuadPart);
}

/* Dedo en el panel delantero, según lo último que pasó el juego. */
static struct {
    int down, x, y, px, py, moved;
    uint64_t t_press, t_release;
    uint32_t presses, releases;     /* contadores: los reconocedores ven transiciones */
} g_p;

typedef struct { uint32_t id, state, type; int x, y, sx, sy, count; uint64_t t; } Ev;

typedef struct {
    uint32_t addr, type, port;
    uint32_t hold_ms;
    uint32_t seen_press, seen_release;
    int active;                     /* arrastre o pulsación larga en curso */
    uint32_t active_id;
    Ev ev[4];
    int nev;
} Rec;

#define MAX_REC 16
static Rec g_rec[MAX_REC];
static int g_nrec;
static uint32_t g_next_id = 1;

static Rec *rec_find(uint32_t a) {
    for (int i = 0; i < g_nrec; i++) if (g_rec[i].addr == a) return &g_rec[i];
    return NULL;
}

void vita_hle_sceSystemGestureInitializePrimitiveTouchRecognizer(void) { r0 = 0; }
void vita_hle_sceSystemGestureFinalizePrimitiveTouchRecognizer(void) { r0 = 0; }

/* (const SceTouchData *front, const SceTouchData *back) */
void vita_hle_sceSystemGestureUpdatePrimitiveTouchRecognizer(void) {
    uint32_t front = r0;
    int down = 0, x = 0, y = 0;
    if (front && vita_mem_ptr(front, 0x20) && vita_read32(front + 12) > 0) {
        down = 1;
        x = (int16_t)(vita_read32(front + 0x10) >> 16);
        y = (int16_t)(vita_read32(front + 0x14) & 0xFFFF);
    }
    if (down && !g_p.down) {
        g_p.px = x; g_p.py = y; g_p.moved = 0;
        g_p.t_press = now_us();
        g_p.presses++;
    }
    if (down && (abs(x - g_p.px) > DRAG_MIN || abs(y - g_p.py) > DRAG_MIN)) g_p.moved = 1;
    if (!down && g_p.down) { g_p.t_release = now_us(); g_p.releases++; }
    if (down) { g_p.x = x; g_p.y = y; }
    g_p.down = down;
    r0 = 0;
}

/* (rec, gestureType, touchPanelPort, const Rectangle *, const Parameter *) */
void vita_hle_sceSystemGestureCreateTouchRecognizer(void) {
    uint32_t a = r0;
    if (!vita_mem_ptr(a, 0x10)) { r0 = (uint32_t)-1; return; }
    Rec *r = rec_find(a);
    if (!r) {
        if (g_nrec >= MAX_REC) { r0 = (uint32_t)-1; return; }
        r = &g_rec[g_nrec++];
    }
    memset(r, 0, sizeof(*r));
    r->addr = a;
    r->type = r1;
    r->port = r2;
    uint32_t param = vita_read32(sp);
    r->hold_ms = (r1 == G_HOLD && param && vita_mem_ptr(param, 4)) ? vita_read32(param) : 800;
    if (!r->hold_ms || r->hold_ms > 5000) r->hold_ms = 800;
    r->seen_press = g_p.presses;
    r->seen_release = g_p.releases;
    if (trace()) printf("[gesto] reconocedor 0x%08X tipo %u puerto %u\n", a, r1, r2);
    r0 = 0;
}

static void push(Rec *r, uint32_t id, uint32_t state, int x, int y) {
    if (r->nev >= 4) return;
    Ev *e = &r->ev[r->nev++];
    memset(e, 0, sizeof(*e));
    e->id = id; e->state = state; e->type = r->type;
    e->x = x; e->y = y; e->sx = g_p.px; e->sy = g_p.py; e->count = 1;
    e->t = now_us() - g_p.t_press;
    if (trace()) printf("[gesto] tipo %u id %u estado %u (%d,%d)\n", r->type, id, state, x, y);
}

/* (rec): saca los eventos de este fotograma a partir del estado del dedo. */
void vita_hle_sceSystemGestureUpdateTouchRecognizer(void) {
    Rec *r = rec_find(r0);
    if (!r) { r0 = vita_mem_ptr(r0, 0x10) ? 0 : (uint32_t)-1; return; }
    r->nev = 0;
    int new_press = g_p.presses != r->seen_press, new_release = g_p.releases != r->seen_release;
    r->seen_press = g_p.presses;
    r->seen_release = g_p.releases;
    if (r->port != 0) { r0 = 0; return; }        /* solo panel delantero */
    uint64_t held_ms = g_p.down ? (now_us() - g_p.t_press) / 1000 : 0;
    switch (r->type) {
    case G_TAP:
        if (new_release && !g_p.moved && (g_p.t_release - g_p.t_press) / 1000 <= TAP_MAX_MS)
            push(r, g_next_id++, S_END, g_p.px, g_p.py);
        break;
    case G_DRAG:
        if (g_p.down && g_p.moved && !r->active) {
            r->active = 1; r->active_id = g_next_id++;
            push(r, r->active_id, S_BEGIN, g_p.x, g_p.y);
        } else if (g_p.down && r->active) {
            push(r, r->active_id, S_ACTIVE, g_p.x, g_p.y);
        } else if (!g_p.down && r->active) {
            r->active = 0;
            push(r, r->active_id, S_END, g_p.x, g_p.y);
        }
        break;
    case G_HOLD:
        if (g_p.down && !g_p.moved && held_ms >= r->hold_ms && !r->active) {
            r->active = 1; r->active_id = g_next_id++;
            push(r, r->active_id, S_BEGIN, g_p.x, g_p.y);
        } else if (g_p.down && r->active) {
            push(r, r->active_id, S_ACTIVE, g_p.x, g_p.y);
        } else if (!g_p.down && r->active) {
            r->active = 0;
            push(r, r->active_id, S_END, g_p.x, g_p.y);
        }
        break;
    default:                                      /* pellizco: hace falta otro dedo */
        break;
    }
    (void)new_press;
    r0 = 0;
}

void vita_hle_sceSystemGestureGetTouchEventsCount(void) {
    Rec *r = rec_find(r0);
    r0 = r ? (uint32_t)r->nev : 0;
}

static void write_ev(uint32_t dst, const Ev *e) {
    uint8_t *p = (uint8_t *)vita_mem_ptr(dst, EV_SIZE);
    if (!p) return;
    memset(p, 0, EV_SIZE);
    memcpy(p + 0x00, &e->id, 4);
    memcpy(p + 0x04, &e->state, 4);
    memcpy(p + 0x08, &e->type, 4);
    memcpy(p + 0x10, &e->t, 8);
    int16_t v;
    if (e->type == G_DRAG) {
        v = (int16_t)e->sx; memcpy(p + 0x1A, &v, 2);
        v = (int16_t)e->sy; memcpy(p + 0x1C, &v, 2);
        v = (int16_t)e->x;  memcpy(p + 0x1E, &v, 2);
        v = (int16_t)e->y;  memcpy(p + 0x20, &v, 2);
        v = (int16_t)e->sx; memcpy(p + 0x22, &v, 2);
        v = (int16_t)e->sy; memcpy(p + 0x24, &v, 2);
    } else {
        v = (int16_t)e->x; memcpy(p + 0x1A, &v, 2);
        v = (int16_t)e->y; memcpy(p + 0x1C, &v, 2);
        p[0x1E] = (uint8_t)e->count;
    }
}

/* (rec, index, ev) */
void vita_hle_sceSystemGestureGetTouchEventByIndex(void) {
    Rec *r = rec_find(r0);
    if (!r || r1 >= (uint32_t)r->nev) { r0 = (uint32_t)-1; return; }
    write_ev(r2, &r->ev[r1]);
    r0 = 0;
}

/* (rec, eventID, ev) */
void vita_hle_sceSystemGestureGetTouchEventByEventID(void) {
    Rec *r = rec_find(r0);
    if (r)
        for (int i = 0; i < r->nev; i++)
            if (r->ev[i].id == r1) { write_ev(r2, &r->ev[i]); r0 = 0; return; }
    r0 = (uint32_t)-1;
}
