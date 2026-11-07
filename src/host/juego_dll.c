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

#include "vitarecomp/cpu.h"
#include "vitarecomp/recomp_rt.h"
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
    /* Este hilo pudo usar los registros provisionales (cpu.c) antes de cargar el DLL. */
    vita_cpu_p = NULL;
    printf("[juego] %s cargado (%u variables importadas)\n", ruta, juego_import_var_count);
    { extern void arranque(const char *fmt, ...); arranque("juego.dll cargado"); }
    fflush(stdout);
    return 0;
}
