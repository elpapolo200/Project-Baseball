/* juego_dll.c — carga juego.dll: el código del juego recompilado EN EL PC DEL JUGADOR a partir de
 * su copia (recompilar.c). El ejecutable no lleva nada del juego; lo que necesita de él se pide
 * aquí por nombre:
 *   vita_cpu_tls              registros de la CPU emulada del hilo actual (cpu.h, VITA_CPU_REMOTA)
 *   vita_register_functions   rellena la tabla de despacho (direcciones guest -> funciones)
 *   vita_func_810056D4        module_start
 *   vita_func_81B078C8        división sin signo del runtime (solo MLB15_SELFTEST)
 *   vita_import_vars/_count   celdas de las variables importadas (_Ctype, _Stdout...)
 * El DLL importa del ejecutable las ~200 funciones del runtime y de las HLE (exports.def). */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/recomp_rt.h"
#include "vitarecomp/mem.h"
#include "juego_dll.h"

void (*juego_register_functions)(void);
void (*juego_module_start)(void);
void (*juego_udiv)(void);
const vita_import_var *juego_import_vars;
uint32_t juego_import_var_count;

static FARPROC pide(HMODULE h, const char *nombre, int obligatorio) {
    FARPROC p = GetProcAddress(h, nombre);
    if (!p && obligatorio) printf("[juego] juego.dll no exporta %s\n", nombre);
    return p;
}

/* Memoria en línea (PLAN_RENDIMIENTO.md fase 1): el DLL accede a la memoria de la Vita sin llamar
 * al exe. main.c reserva la ventana después de cargar el DLL y entonces llama a juego_memoria().
 * Con un DLL de antes no existe la función y todo sigue por las funciones del exe. */
static void (*g_mem_directa)(unsigned char *, uint32_t, unsigned long long);
void juego_memoria(void) {
    uint8_t *mem = NULL; uint32_t base = 0; size_t tam = 0;
    vita_mem_window(&mem, &base, &tam);
    if (g_mem_directa && mem && !getenv("MLB15_MEM_LLAMADAS")) g_mem_directa(mem, base, (unsigned long long)tam);
    printf("[juego] memoria en línea: %s\n", !g_mem_directa ? "no (juego.dll antiguo)" : getenv("MLB15_MEM_LLAMADAS") ? "no (MLB15_MEM_LLAMADAS)" : "sí");
}

int juego_cargar(const char *ruta) {
    HMODULE h = LoadLibraryA(ruta);
    if (!h) {
        printf("[juego] no se pudo cargar %s (error %lu)\n", ruta, GetLastError());
        { extern void arranque(const char *fmt, ...); arranque("NO SE PUDO CARGAR juego.dll (error %lu)", GetLastError()); }
        return -1;
    }
    vita_cpu_fuente = (vita_cpu_state *(*)(void))pide(h, "vita_cpu_tls", 1);
    juego_register_functions = (void (*)(void))pide(h, "vita_register_functions", 1);
    juego_module_start = (void (*)(void))pide(h, "vita_func_810056D4", 1);
    juego_udiv = (void (*)(void))pide(h, "vita_func_81B078C8", 0);
    juego_import_vars = (const vita_import_var *)pide(h, "vita_import_vars", 1);
    const uint32_t *n = (const uint32_t *)pide(h, "vita_import_var_count", 1);
    if (!vita_cpu_fuente || !juego_register_functions || !juego_module_start || !juego_import_vars || !n) return -1;
    juego_import_var_count = *n;
    g_mem_directa = (void (*)(unsigned char *, uint32_t, unsigned long long))pide(h, "vita_mem_directa", 0);
    /* Este hilo pudo usar los registros provisionales (cpu.c) antes de cargar el DLL. */
    vita_cpu_p = NULL;
    printf("[juego] %s cargado (%u variables importadas)\n", ruta, juego_import_var_count);
    { extern void arranque(const char *fmt, ...); arranque("juego.dll cargado"); }
    fflush(stdout);
    return 0;
}
