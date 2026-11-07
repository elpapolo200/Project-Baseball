/* Primeras funciones del sistema implementadas para MLB 15 (HLE).
 *
 * Convención (ver vitarecomp/hle.h): void(void); argumentos en r0-r3, resultado
 * en r0 (y r1 para 64 bits). Se enlaza ANTES de hle_defaults.lib para que estas
 * sustituyan a los stubs que paran con trampa.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"

static int g_trace = 1;

/* Pila nativa como RVAs del ejecutable: tools/backtrace.py las traduce a
 * vita_func_XXXXXXXX con el .map del enlazador (el código guest no actualiza lr). */
void host_backtrace(void) {
    void *frames[24];
    USHORT n = RtlCaptureStackBackTrace(1, 24, frames, NULL);
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    printf("[pila]");
    for (USHORT i = 0; i < n; i++) printf(" %llX", (unsigned long long)((uintptr_t)frames[i] - base));
    printf("\n");
    fflush(stdout);
}

/* Aviso en español cuando el port se cierra (solo con doble clic, sin consola).
 * Los literales están en UTF-8: MessageBoxW, no A (con A salían "cerrÃ³"). */
void port_error_box(const wchar_t *text) {
    static volatile LONG shown;
    if (GetConsoleWindow() != NULL || InterlockedExchange(&shown, 1)) return;
    MessageBoxW(NULL, text, L"Project Baseball — error", MB_OK | MB_ICONERROR);
}

/* vita_trap_hook: solo las trampas FATALES (el proceso aborta justo después).
 * host_backtrace a secas también lo usan las trazas de abajo, por eso el aviso
 * va aparte: antes salía en cada arranque con el primer sceKernelDelayThread. */
void host_trap_fatal(void) {
    host_backtrace();
    port_error_box(L"El juego se cerró por un error interno (trampa del recompilador).\n\n"
                   L"Los detalles están en datos\\log.txt (registro completo, con la pila de la trampa).\n\n"
                   L"Envía ese archivo para poder diagnosticar el problema.");
}

#define TRACE(name) do { if (g_trace) { printf("[hle] %s(r0=0x%X r1=0x%X r2=0x%X)\n", name, r0, r1, r2); \
    if (g_trace == 1) host_backtrace(); fflush(stdout); if (++g_trace > 6) g_trace = 0; } } while (0)

static uint64_t now_us(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000.0 / freq.QuadPart);
}

/* --- hilos ---------------------------------------------------------------- */
/* Pausa de us microsegundos durmiendo de verdad. Antes era Sleep(us / 1000): el hilo principal
 * pide 300 us una y otra vez mientras espera al dibujo, eso daba Sleep(0) (vuelve al instante)
 * y el hilo giraba en vacío (~90 % de un núcleo, más a 30 FPS que a 60). Temporizador de alta
 * resolución (Windows 10 1803+) por hilo; si no lo hay, Sleep(1). MLB15_DELAY_ANTIGUO=1: como antes. */
void port_dormir_us(uint32_t us);
static void dormir_us(uint32_t us) { port_dormir_us(us); }
void port_dormir_us(uint32_t us) {
    static int antiguo = -1;
    if (antiguo < 0) antiguo = getenv("MLB15_DELAY_ANTIGUO") ? 1 : 0;
    if (antiguo) { Sleep(us / 1000); return; }
    if (!us) { SwitchToThread(); return; }
    static __declspec(thread) HANDLE tm;
    static __declspec(thread) int sin_tm;
    if (!tm && !sin_tm) {
        tm = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        if (!tm) sin_tm = 1;
    }
    if (tm) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)us * 10;   /* relativo, en unidades de 100 ns */
        if (SetWaitableTimerEx(tm, &due, 0, NULL, NULL, NULL, 0)) { WaitForSingleObject(tm, INFINITE); return; }
    }
    Sleep(us < 1000 ? 1 : us / 1000);
}
void vita_hle_sceKernelDelayThread(void) {
    TRACE("sceKernelDelayThread");
    {   /* MLB15_DELAY_TRAZA=1: duraciones pedidas por hilo (cada 2000 llamadas) */
        static int on = -1;
        if (on < 0) on = getenv("MLB15_DELAY_TRAZA") ? 1 : 0;
        if (on) {
            extern const char *port_current_thread_name(void);
            static struct { const char *t; uint32_t us; unsigned n; double real; } h[32];
            static unsigned tot;
            const char *t = port_current_thread_name();
            uint64_t a = now_us();
            dormir_us(r0);
            double real = (double)(now_us() - a);
            int k;
            for (k = 0; k < 32 && h[k].n && !(h[k].t == t && h[k].us == r0); k++) {}
            if (k < 32) { h[k].t = t; h[k].us = r0; h[k].n++; h[k].real += real; }
            if (++tot % 2000 == 0) {
                printf("[delay]");
                for (k = 0; k < 32 && h[k].n; k++) printf(" %s:%uus x%u (real %.0fus)", h[k].t, h[k].us, h[k].n, h[k].real / h[k].n);
                printf("\n");
                memset(h, 0, sizeof(h));
            }
            r0 = 0;
            return;
        }
    }
    dormir_us(r0);
    r0 = 0;
}
void vita_hle_sceKernelDelayThreadCB(void) { vita_hle_sceKernelDelayThread(); }
void vita_hle_sceKernelCheckCallback(void) { r0 = 0; }

/* --- tiempo --------------------------------------------------------------- */
void vita_hle_sceKernelGetProcessTimeWide(void) {
    uint64_t t = now_us();
    r0 = (uint32_t)t;
    r1 = (uint32_t)(t >> 32);
}
void vita_hle_sceKernelGetProcessTimeLow(void) { r0 = (uint32_t)now_us(); }
void vita_hle_sceKernelGetSystemTimeWide(void) { vita_hle_sceKernelGetProcessTimeWide(); }


/* --- energia -------------------------------------------------------------- */
/* Gxm_Init lo llama justo despues de sceGxmInitialize; en el PC no hay modo de
 * configuracion que aplicar, pero devolver 0 evita que el juego lo trate como
 * fallo y aborte el arranque de GXM. */
void vita_hle_scePowerSetConfigurationMode(void) {
    printf("[hle] scePowerSetConfigurationMode(0x%X)\n", r0);
    fflush(stdout);
    r0 = 0;
}
