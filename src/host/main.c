/* Anfitrión del MLB 15 recompilado con vitarecomp.
 *
 * Mapa de la memoria guest:
 *   0x81000000  ELF (código y datos, cargados desde eboot.elf)
 *   0x82100000  heap auxiliar de libc (16 MB, solo si el juego no trae malloc propio)
 *   0x83200000  RAM principal del juego (512 MB): pools, tablas, datos
 *   0xA3200000  VRAM del juego (128 MB): solo bloques de tipo 0x9408060
 *   0xAB200000  reserva del port (16 MB): handles, pilas de hilos, TLS
 *   0xAC200000  fin
 *
 * Uso: mlb15_recomp.exe [ruta\eboot.elf]
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv/atoi: sin el prototipo el puntero de getenv se truncaba a 32 bits */
#include <string.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/dispatch.h"
#include "vitarecomp/hle.h"
#include "vitarecomp/loader.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/recomp_rt.h"

#include "port.h"
#include "launcher.h"

#include "juego_dll.h"   /* el código del juego está en juego.dll (generado en el PC del jugador) */
void host_backtrace(void);
void host_trap_fatal(void);
void port_watchdog_start(void);
void port_perfil_start(void);
void port_watch_start(void);
void port_import_vars(void);
void port_hooks_init(void);
void port_crash_handler_install(void);
void port_tls_init(uint32_t tmpl, uint32_t filesz, uint32_t memsz);

/* Presupuesto de memoria del guest, calcado de una Vita con 512 MB de RAM y
 * 128 MB de VRAM dedicada:
 *
 *   0x83200000..0xA3200000  512 MB de RAM principal (pools del juego)
 *   0xA3200000..0xAB200000  128 MB de VRAM (bloques 0x9408060)
 *   0xAB200000..0xAC200000   16 MB de reserva para el port
 *
 * 512 y no 352: al cargar el partido el juego tiene ocupados ~350 MB (MainPool,
 * Persist, SQL, EngineUncachedMainRAM, varios RTdriver de 4-12 MB...) y pide
 * otro RTdriver de 5 MB; con 352 fallaba con "AllocateMainRAM: Allocation
 * Failed" (2026-10-25). Dar algo más de lo que tenía la Vita no le molesta.
 *
 * La separacion VRAM/RAM no es cosmetica: el juego SONDEA la VRAM al arrancar
 * ('TestAvailRAM' de tipo 0x9408060, subiendo hasta 256 MB) y se queda con lo
 * que quepa como EngineVRAM. Si VRAM y RAM principal comparten monton, ese
 * sondeo se lleva por delante el MainPool (56 MB) y el RTdriver, y el juego
 * termina con "AllocateMainRAM: Allocation Failed". Con presupuestos separados
 * el sondeo se queda en lo que hay de VRAM (unos 112 MB tras los bloques
 * fijos) y la RAM principal conserva su presupuesto.
 *
 * Ojo: estos numeros son parte de la emulacion del hardware, no del juego. Si
 * se cambian, revisar el orden de las reservas: la RAM principal llega hasta
 * GAME_END y la VRAM empieza justo ahi. */
#define GUEST_END   0xAC200000u
#define GAME_END    0xA3200000u
#define VRAM_END    0xAB200000u
#define LIBC_HEAP   (16u << 20)
#define ARENA_START 0x83200000u

/* SceProcessParam: u32 tamaño, "PSP2", versión, firmware, ... (+0x18 puntero
 * al tamaño de pila del hilo principal, +0x2C puntero a SceLibcParam). */
static uint32_t find_process_param(uint32_t from, uint32_t to) {
    for (uint32_t a = from; a + 8 <= to; a += 4)
        if (vita_read32(a + 4) == 0x32505350u && vita_read32(a) == 0x34u) return a;
    return 0;
}

/* SceModuleInfo: e_entry (sin los 2 bits altos) es su desplazamiento dentro del
 * segmento 0. +0x38 tls_start (también relativo al segmento 0), +0x3C
 * tls_filesz, +0x40 tls_memsz. */
static void tls_from_module_info(const char *elf, uint32_t seg0) {
    FILE *f = fopen(elf, "rb");
    uint32_t entry = 0;
    if (!f) return;
    if (fseek(f, 0x18, SEEK_SET) == 0 && fread(&entry, 4, 1, f) == 1) {
        uint32_t mi = seg0 + (entry & 0x3FFFFFFFu);
        if (vita_mem_ptr(mi, 0x44) && vita_read32(mi + 0x3C) <= vita_read32(mi + 0x40))
            port_tls_init(seg0 + vita_read32(mi + 0x38), vita_read32(mi + 0x3C), vita_read32(mi + 0x40));
    }
    fclose(f);
}

/* Estado del público 3D cuando su copia de matrices apunta a basura
 * (sub_81851546: r9 = objeto, r4 = lista de 128 B, r5 = índice de lista). */
static void bad_dst_report(uint32_t addr, uint32_t len) {
    (void)len;
    uint32_t o = r9;
    printf("  r4=0x%08X r5=%u r9=0x%08X", r4, r5, r9);
    if (vita_mem_ptr(o, 0x9C))
        printf(" | obj+14=%u +18=%u +60=0x%08X +64=0x%08X +68=0x%08X +94=%u",
               vita_read32(o + 0x14), vita_read32(o + 0x18), vita_read32(o + 0x60), vita_read32(o + 0x64),
               vita_read32(o + 0x68), vita_read32(o + 0x94));
    if (vita_mem_ptr(r4, 0x20))
        printf(" | lista: cap=%u n=%u +8=0x%08X buf0=0x%08X buf1=0x%08X", vita_read32(r4), vita_read32(r4 + 4),
               vita_read32(r4 + 8), vita_read32(r4 + 0x14), vita_read32(r4 + 0x18));
    printf(" (dst 0x%08X)\n", addr);
}

/* Windows 11 manda los procesos que no tiene por «primer plano» a los núcleos de
 * eficiencia (EcoQoS): en el portátil del usuario (Core 7 240H) el juego corría
 * en los 4 núcleos E a 1,5 GHz con los P parados, ~5 FPS; quitándoselo al
 * proceso abierto subió a ~27 FPS (2026-10-31). Se pide desde el arranque. */
/* Brillo: objetivo de la exposición automática del juego (sub_813F292A acerca la media
 * de ToneMapSample a este entero; el juego lo pone a 120 al arrancar, no viene en el
 * ELF). Con la pantalla en sRGB (como Vita3K) al 100 % el cielo se quema un poco más
 * que en Vita3K; por defecto va al 85 % (al 95 % el usuario lo veía demasiado claro). MLB15.ini [video] brillo en % (100 = como la Vita); MLB15_BRILLO
 * manda. Se llama en cada fotograma (hle_display.c). */
void port_brillo_aplica(void) {
    static int pct = -1;
    static uint32_t want;
    if (pct < 0) {
        const char *e = getenv("MLB15_BRILLO");
        pct = e ? atoi(e) : launcher_ini_int("video", "brillo", 85);
        if (pct < 10) pct = 100;
        if (pct > 200) pct = 200;
        want = (uint32_t)((120 * pct + 50) / 100);
        printf("[video] brillo %d %% (objetivo de exposición %u)\n", pct, want);
    }
    if (pct != 100 && vita_read32(0x81F78554u) == 120) vita_write32(0x81F78554u, want);
}

static void sin_modo_eficiencia(void) {
    PROCESS_POWER_THROTTLING_STATE s = {0};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    s.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s));
}

/* Prioridad del proceso (2026-11-08). Con OBS u otro programa que ocupe la CPU el juego caía
 * de 60 a ~20 FPS: sus hilos se pasan el trabajo esperando por turnos y, a prioridad normal,
 * cada espera regala el núcleo a otro programa. Medido con 10 procesos quemando CPU (ventana
 * oculta, x3): 14 FPS a prioridad normal, 58,5 «por encima de lo normal». Desactivado por
 * defecto; se activa con [sistema] prioridad=1 (2 = alta) o MLB15_PRIORIDAD=n. Se aplica
 * después de preparar juego.dll: la compilación sigue a prioridad normal. */
static void prioridad_juego(void) {
    extern void arranque(const char *fmt, ...);
    const char *e = getenv("MLB15_PRIORIDAD");
    int nivel = e ? atoi(e) : launcher_ini_int("sistema", "prioridad", 0);
    if (nivel <= 0) return;
    SetPriorityClass(GetCurrentProcess(), nivel == 1 ? ABOVE_NORMAL_PRIORITY_CLASS : HIGH_PRIORITY_CLASS);
    const char *nom = nivel == 1 ? "por encima de lo normal" : "alta";
    printf("[sistema] prioridad %s\n", nom);
    arranque("sistema: prioridad %s", nom);
}

int main(int argc, char **argv) {
    sin_modo_eficiencia();
    /* Con un ELF en la línea de órdenes (desarrollo) se usa ese; si no, el
     * launcher busca el juego del usuario en Vita3K y prepara datos\eboot.elf. */
    static char elf_buf[520];
    if (getenv("MLB15_OPCIONES_CAPTURA")) return opciones_dialogo() ? 0 : 1;   /* prueba de la ventana de opciones */
    launcher_console();
    if (getenv("MLB15_SIN_BUFER")) { setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0); }   /* pruebas: nada se pierde al matar el proceso */
    /* El público 3D (sub_81851546, tras una jugada con presentación) a veces copia
     * una matriz a un puntero basura: se omite esa copia en vez de cerrar el juego.
     * MLB15_MEMCPY_ESTRICTO=1 vuelve a la trampa (para depurar). */
    {
        extern int vita_libc_skip_bad_dst;
        extern void (*vita_libc_bad_dst_hook)(uint32_t, uint32_t);
        vita_libc_skip_bad_dst = getenv("MLB15_MEMCPY_ESTRICTO") ? 0 : 1;
        vita_libc_bad_dst_hook = bad_dst_report;
    }
    const char *elf = argc > 1 ? argv[1] : elf_buf;
    if (argc <= 1 && launcher_prepare(elf_buf, sizeof(elf_buf)) != 0) return 1;
    {   /* juego.dll: con el launcher, datos\juego.dll (lo genera recompilar.c); en desarrollo,
         * MLB15_JUEGO_DLL o juego.dll en la carpeta actual. */
        if (getenv("MLB15_RECOMPILAR")) {   /* build.ps1 -Partes: generar datos\juego.dll y salir */
            extern int recompilar_desarrollo(const char *elf);
            return recompilar_desarrollo(elf) ? 1 : 0;
        }
        const char *dll = argc <= 1 ? launcher_juego_dll() : getenv("MLB15_JUEGO_DLL");
        if (!dll || !*dll) dll = "datos\\juego.dll";
        if (juego_cargar(dll) != 0) {
            fprintf(stderr, "no se pudo cargar el código del juego (%s)\n", dll);
            return 1;
        }
    }
    prioridad_juego();
    vita_load_info info;
    if (vita_probe_elf(elf, &info) != 0) {
        fprintf(stderr, "no se pudo leer %s\n", elf);
        return 1;
    }
    uint32_t base = info.lowest & 0xFFF00000u;
    if (vita_mem_init(base, (size_t)(GUEST_END - base)) != 0) {
        fprintf(stderr, "no se pudo reservar la memoria de la guest\n");
        return 1;
    }
    juego_memoria();
    if (vita_load_elf(elf, &info) != 0) {
        fprintf(stderr, "no se pudo cargar %s\n", elf);
        return 1;
    }
    uint32_t libc_heap = (info.highest + 0xFFFFFu) & 0xFFF00000u;
    printf("ELF cargado: 0x%08X-0x%08X (%d segmentos)\n", info.lowest, info.highest, info.segments);

    port_memblock_init(ARENA_START, GAME_END, VRAM_END, GUEST_END);
    port_objs_init();
    port_vfs_init();
    tls_from_module_info(elf, info.lowest);

    uint32_t pp = find_process_param(0x81E4F000u, info.highest);
    uint32_t stack_size = 1u << 20;
    if (pp) {
        uint32_t ss_ptr = vita_read32(pp + 0x18);
        if (ss_ptr && vita_read32(ss_ptr)) stack_size = vita_read32(ss_ptr);
        printf("SceProcessParam en 0x%08X (pila principal %u KB)\n", pp, stack_size >> 10);
    }
    uint32_t stack = port_alloc(stack_size, 0x1000, "main stack");
    port_threads_init(stack, stack_size);
    vita_heap_init(libc_heap, LIBC_HEAP);
    juego_register_functions();
    port_hooks_init();        /* ganchos sobre llamadas indirectas (hooks.c) */
    port_import_vars();       /* antes de cualquier código guest: _Ctype, _Stdout... */
    vita_trap_hook = host_trap_fatal;
    port_watchdog_start();
    port_perfil_start();
    port_watch_start();
    port_crash_handler_install();

    sp = stack + stack_size - 16;
    lr = 0;
    if (pp) {
        printf("inicializando el malloc propio del juego (SceLibcParam)...\n");
        fflush(stdout);
        vita_libc_init(pp);
    }
    if (getenv("MLB15_SELFTEST")) {
        /* sub_81B078C8 es la división sin signo del runtime (r0 / r1). */
        static const uint32_t casos[][2] = { {100, 7}, {0x12345678u, 0x10}, {640, 3},
                                             {0xFFFFFFFFu, 0xFFFFu}, {1000000, 1000}, {7, 9} };
        for (int i = 0; i < 6 && juego_udiv; i++) {
            r0 = casos[i][0]; r1 = casos[i][1];
            sp = stack + stack_size - 16;
            juego_udiv();
            printf("[selftest] %u / %u = %u (esperado %u)\n", casos[i][0], casos[i][1], r0,
                   casos[i][0] / casos[i][1]);
        }
        fflush(stdout);
    }
    r0 = r1 = r2 = r3 = 0;
    sp = stack + stack_size - 16;
    printf("llamando a module_start (0x810056D4)...\n");
    fflush(stdout);
    juego_module_start();
    printf("module_start devolvió r0=0x%08X; trampas=%u\n", r0, vita_trap_count());
    return 0;
}
