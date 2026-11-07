/* Ganchos sobre funciones del juego (llamadas indirectas: vtables, punteros).
 *
 * vita_dispatch_override hace que una llamada indirecta a una dirección guest
 * ejecute una función del anfitrión en lugar de la recompilada. Sirve para
 * trazar qué pide el juego (MLB15_GANCHOS=1) y para sustituir funciones.
 * Las llamadas directas (BL) son llamadas C estáticas y no pasan por aquí.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vitarecomp/recomp_rt.h"
#include "vitarecomp/dispatch.h"
#include "vitarecomp/mem.h"

#include "port.h"

/* Traza: llamada, quién llama y la cadena que devuelve en r0. */
typedef struct { uint32_t addr; vita_fn orig; const char *name; } Traza;
static Traza g_tr[8];
static int g_ntr;

static void traza_cadena(int i) {
    uint32_t from = lr;
    g_tr[i].orig();
    char s[96];
    port_guest_str(r0, s, sizeof(s));
    printf("[gancho] %s (desde 0x%08X) -> 0x%08X \"%s\"\n", g_tr[i].name, from, r0, s);
    fflush(stdout);
}
#define T(n) static void traza_##n(void) { traza_cadena(n); }
T(0) T(1) T(2) T(3) T(4) T(5) T(6) T(7)
static const vita_fn g_tr_fn[8] = { traza_0, traza_1, traza_2, traza_3, traza_4, traza_5, traza_6, traza_7 };

static void trazar(uint32_t addr, const char *name) {
    if (g_ntr >= 8) return;
    vita_fn f = vita_dispatch_lookup(addr);
    if (!f) return;
    g_tr[g_ntr].addr = addr;
    g_tr[g_ntr].orig = f;
    g_tr[g_ntr].name = name;
    vita_dispatch_override(addr, g_tr_fn[g_ntr]);
    g_ntr++;
}

/* MLB15_GANCHOS_AUDIO=1: claves de volumen que llegan a sub_810061A4 (tabla de mezclas). */
static vita_fn g_audio_orig;
static void gancho_audio(void) {
    uint32_t p = r1;
    uint32_t key = vita_read32(p), idx = vita_read32(p + 8), base = vita_read32(0x81E7C8D4u);
    uint32_t prof = base + (idx < 30 ? idx : 0) * 0x20Cu;
    printf("[gancho] mezcla clave=0x%X perfil=%u base=0x%08X activo=%u ajuste=%u\n", key, idx, base,
           base ? vita_read8(prof + 0x11) : 0, base && key < 0x95 ? vita_read8(prof + 0x12 + key) : 999);
    fflush(stdout);
    g_audio_orig();
}

void port_hooks_init(void) {
    if (getenv("MLB15_GANCHOS_AUDIO")) {
        g_audio_orig = vita_dispatch_lookup(0x810061A4);
        if (g_audio_orig) vita_dispatch_override(0x810061A4, gancho_audio);
        printf("[gancho] mezcla: %s\n", g_audio_orig ? "instalado" : "no encontrado");
    }
    if (getenv("MLB15_GANCHOS")) {
        /* Interfaz de plataforma de la UI (vtable 0x81E3F768): extensiones. */
        trazar(0x813C691E, "ui.ext_layout");
        trazar(0x813C6928, "ui.ext_textura");
        trazar(0x813C690E, "ui.ruta_pres");
    }
}
