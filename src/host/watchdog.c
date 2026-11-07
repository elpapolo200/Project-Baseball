/* Vigilante para bucles y bloqueos: con MLB15_WATCHDOG=<segundos>, pasado ese
 * tiempo detiene cada hilo del proceso unas cuantas veces y escribe su pila
 * nativa en el mismo formato que host_backtrace ("[pila] RVA ..."), para
 * tools/backtrace.py. Sirve para ver en qué función guest gira un arranque que
 * no avanza o dónde espera cada hilo en un interbloqueo.
 *
 * Escribe en watchdog.txt con WriteFile, no en stdout: si el bloqueo deja
 * tomado el cerrojo de stdout (un hilo detenido dentro de printf), un printf
 * aquí se quedaría esperando para siempre. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

void port_error_box(const wchar_t *text);   /* hle_mlb15.c */

static DWORD  g_secs;
static HANDLE g_out;

static void out(const char *s) {
    DWORD w;
    WriteFile(g_out, s, (DWORD)strlen(s), &w, NULL);
}

/* Pila nativa a partir de un contexto, en formato "[pila] RVA ...". */
static int walk(CONTEXT *ctx, char *line, int n, int max) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    n += snprintf(line + n, (size_t)(max - n), "[pila]");
    for (int i = 0; i < 24 && ctx->Rip && n < max - 24; i++) {
        n += snprintf(line + n, (size_t)(max - n), " %llX", (unsigned long long)(ctx->Rip - base));
        DWORD64 img = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx->Rip, &img, NULL);
        if (!fe) {                           /* hoja sin unwind info */
            ctx->Rip = *(DWORD64 *)ctx->Rsp;
            ctx->Rsp += 8;
            continue;
        }
        PVOID hd;
        DWORD64 est;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, img, ctx->Rip, fe, ctx, &hd, &est, NULL);
    }
    n += snprintf(line + n, (size_t)(max - n), "\n");
    return n;
}

static void sample(HANDLE th, DWORD tid) {
    CONTEXT ctx;
    char line[1024];
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    line[0] = 0;
    if (SuspendThread(th) == (DWORD)-1) return;
    if (GetThreadContext(th, &ctx)) {
        int n = snprintf(line, sizeof(line), "[hilo %lu]\n", (unsigned long)tid);
        walk(&ctx, line, n, (int)sizeof(line));
    }
    ResumeThread(th);
    out(line);                               /* después de reanudar: el hilo puede tener el cerrojo del heap */
}

/* arg != NULL: volcado inmediato (tecla F12); si no, tras MLB15_WATCHDOG s. */
static DWORD WINAPI watchdog(LPVOID arg) {
    static volatile LONG busy;
    if (!arg) Sleep(g_secs * 1000);
    if (InterlockedExchange(&busy, 1)) return 0;
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof(path));     /* junto al exe */
    char *sl = strrchr(path, '\\');
    snprintf(sl ? sl + 1 : path, sizeof(path) - (size_t)(sl ? sl + 1 - path : 0), "watchdog.txt");
    g_out = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_out == INVALID_HANDLE_VALUE) { busy = 0; return 0; }
    char hdr[96];
    if (arg) snprintf(hdr, sizeof(hdr), "[watchdog] F12: pilas de todos los hilos\n");
    else snprintf(hdr, sizeof(hdr), "[watchdog] %u s: pilas de todos los hilos\n", (unsigned)g_secs);
    out(hdr);
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    /* MLB15_WATCHDOG_PASADAS=n: más muestras (perfil pobre pero útil). */
    int passes = 3;
    const char *pe = getenv("MLB15_WATCHDOG_PASADAS");
    if (pe && atoi(pe) > 0) passes = atoi(pe);
    for (int pass = 0; pass < passes; pass++) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
                HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (th) { sample(th, te.th32ThreadID); CloseHandle(th); }
            } while (Thread32Next(snap, &te));
        }
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        out("----\n");
        Sleep(passes > 3 ? 100 : 300);
    }
    CloseHandle(g_out);
    busy = 0;
    return 0;
}

/* Tecla F12: guarda ya las pilas de todos los hilos en watchdog.txt. */
void port_watchdog_dump_now(void) {
    CreateThread(NULL, 0, watchdog, (LPVOID)1, 0, NULL);
}

/* F12 vigilada desde su propio hilo: los eventos de SDL solo se leen cuando el
 * juego presenta un fotograma, y justo cuando se queda colgado no presenta. */
static DWORD WINAPI f12_thread(LPVOID arg) {
    (void)arg;
    DWORD pid = GetCurrentProcessId();
    int was = 0;
    for (;;) {
        Sleep(150);
        DWORD fpid = 0;
        HWND fg = GetForegroundWindow();
        if (fg) GetWindowThreadProcessId(fg, &fpid);
        int down = fpid == pid && (GetAsyncKeyState(VK_F12) & 0x8000);
        if (down && !was) port_watchdog_dump_now();
        was = down;
    }
    return 0;
}

void port_watchdog_start(void) {
    CreateThread(NULL, 0, f12_thread, NULL, 0, NULL);
    const char *e = getenv("MLB15_WATCHDOG");
    if (!e || !*e) return;
    g_secs = (DWORD)atoi(e);
    if (!g_secs) return;
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
}

/* Fallo nativo (acceso a memoria, etc.): sin esto Windows Error Reporting
 * congela el proceso en silencio. Escribe el código, la dirección y la pila en
 * crash.txt y en stderr, y termina. Traducir con tools/backtrace.py.
 * Si no hay consola (doble clic), muestra un MessageBox en español. */
static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    char line[1400];
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    int n = snprintf(line, sizeof(line), "[fallo] excepción 0x%08lX en RVA %llX",
                     (unsigned long)er->ExceptionCode, (unsigned long long)((uintptr_t)er->ExceptionAddress - base));
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        n += snprintf(line + n, sizeof(line) - (size_t)n, " (%s 0x%llX)",
                      er->ExceptionInformation[0] == 1 ? "escritura" : er->ExceptionInformation[0] == 8 ? "ejecución" : "lectura",
                      (unsigned long long)er->ExceptionInformation[1]);
    n += snprintf(line + n, sizeof(line) - (size_t)n, ", hilo %lu\n", (unsigned long)GetCurrentThreadId());
    CONTEXT ctx = *ep->ContextRecord;
    walk(&ctx, line, n, (int)sizeof(line));
    /* Junto al exe, no en la carpeta actual (con doble clic puede ser otra). */
    char cpath[MAX_PATH];
    GetModuleFileNameA(NULL, cpath, sizeof(cpath));
    char *sl = strrchr(cpath, '\\');
    snprintf(sl ? sl + 1 : cpath, sizeof(cpath) - (size_t)(sl ? sl + 1 - cpath : 0), "crash.txt");
    HANDLE f = CreateFileA(cpath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w;
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, line, (DWORD)strlen(line), &w, NULL); CloseHandle(f); }
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, (DWORD)strlen(line), &w, NULL);

    /* Aviso solo si no hay consola (doble clic: FreeConsole en launcher_console). */
    port_error_box(L"El juego se cerró por un error.\n\n"
                   L"Los detalles están en crash.txt (junto al exe) y en datos\\log.txt.\n\n"
                   L"Envía esos archivos para poder diagnosticar el problema.");

    TerminateProcess(GetCurrentProcess(), 0xC0DE0001u);
    return EXCEPTION_EXECUTE_HANDLER;
}

void port_crash_handler_install(void) {
    SetUnhandledExceptionFilter(on_crash);
}

/* --- vigilancia de escrituras (registros de depuración) ---------------------
 * MLB15_VIGILA=<dirección guest>: cada escritura de 4 bytes en esa dirección
 * deja en vigila.txt (junto al exe) el valor nuevo y la pila nativa, para
 * traducir con tools/backtrace.py. Sirve para saber QUÉ función guest escribe un
 * campo (p. ej. un puntero que llega a 0). MLB15_VIGILA_LECTURA=1 también para
 * en las lecturas (el valor mostrado es el actual). Usa DR0 en todos los hilos; los que
 * se crean después lo activan en port_watch_thread_start(). */
#include "vitarecomp/mem.h"

static uintptr_t g_watch_host;
static uint32_t  g_watch_guest;
static HANDLE    g_watch_out = INVALID_HANDLE_VALUE;
static volatile LONG g_watch_hits;

static LONG WINAPI watch_veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Dr6 = 0;
    static LONG max_hits;
    if (!max_hits) { const char *m = getenv("MLB15_VIGILA_MAX"); max_hits = m ? atol(m) : 200; }
    if (InterlockedIncrement(&g_watch_hits) <= max_hits && g_watch_out != INVALID_HANDLE_VALUE) {
        char line[1100];
        /* con lecturas vigiladas, leer el valor aquí volvería a saltar */
        uint32_t v = getenv("MLB15_VIGILA_LECTURA") ? 0xFFFFFFFFu : *(volatile uint32_t *)g_watch_host;
        int n = snprintf(line, sizeof(line), "[vigila] 0x%08X <- 0x%08X (hilo %lu)\n", g_watch_guest,
                         v, (unsigned long)GetCurrentThreadId());
        CONTEXT ctx = *ep->ContextRecord;
        walk(&ctx, line, n, (int)sizeof(line));
        DWORD w;
        WriteFile(g_watch_out, line, (DWORD)strlen(line), &w, NULL);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void watch_set(HANDLE th) {
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(th, &ctx)) return;
    ctx.Dr0 = g_watch_host;
    /* L0, 4 bytes; escritura, o lectura y escritura con MLB15_VIGILA_LECTURA=1 */
    static int rw = -1;
    if (rw < 0) rw = getenv("MLB15_VIGILA_LECTURA") ? 3 : 1;
    ctx.Dr7 = (ctx.Dr7 & ~0x000F0003ull) | 1ull | ((unsigned long long)rw << 16) | (3ull << 18);
    SetThreadContext(th, &ctx);
}

static DWORD WINAPI watch_helper(LPVOID arg) {
    HANDLE th = (HANDLE)arg;
    if (SuspendThread(th) != (DWORD)-1) { watch_set(th); ResumeThread(th); }
    return 0;
}

/* Un hilo no puede cambiar sus propios registros de depuración: lo hace otro. */
void port_watch_thread_start(void) {
    if (!g_watch_host) return;
    HANDLE me;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &me,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, 0);
    HANDLE h = CreateThread(NULL, 0, watch_helper, me, 0, NULL);
    if (h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
    CloseHandle(me);
}

void port_watch_start(void) {
    const char *e = getenv("MLB15_VIGILA");
    if (!e || !*e) return;
    g_watch_guest = (uint32_t)strtoul(e, NULL, 0);
    void *p = vita_mem_ptr(g_watch_guest, 4);
    if (!p) return;
    g_watch_host = (uintptr_t)p;
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof(path));
    char *sl = strrchr(path, '\\');
    snprintf(sl ? sl + 1 : path, sizeof(path) - (size_t)(sl ? sl + 1 - path : 0), "vigila.txt");
    g_watch_out = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    AddVectoredExceptionHandler(1, watch_veh);
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
            if (th) { watch_helper(th); CloseHandle(th); }
        } while (Thread32Next(snap, &te));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    port_watch_thread_start();
    printf("[vigila] escrituras en 0x%08X -> vigila.txt\n", g_watch_guest);
    fflush(stdout);
}

/* MLB15_PERFIL=1: perfilador de muestreo del hilo que dibuja (el que llama a do_draw).
 * Cada ~1 ms detiene ese hilo y cuenta su dirección (RVA) y la de su llamador
 * directo; cada 20 s escribe perfil.txt con las 80 RVA más frecuentes, para
 * traducir con el .map. Solo cuenta durante el partido (port_en_partido). */
volatile DWORD port_draw_tid;
extern int port_en_partido;
#define PERF_N 65536
static uintptr_t g_pf_rva[PERF_N];
static unsigned  g_pf_cnt[PERF_N];
static unsigned  g_pf_total;
static void pf_add(uintptr_t rva) {
    uint32_t h = (uint32_t)(rva * 2654435761u) >> 16;
    for (uint32_t i = 0; i < PERF_N; i++) {
        uint32_t k = (h + i) & (PERF_N - 1);
        if (g_pf_rva[k] == rva) { g_pf_cnt[k]++; return; }
        if (!g_pf_rva[k]) { g_pf_rva[k] = rva; g_pf_cnt[k] = 1; return; }
    }
}
static void pf_dump(void) {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof(path));
    char *sl = strrchr(path, '\\');
    snprintf(sl ? sl + 1 : path, sizeof(path) - (size_t)(sl ? sl + 1 - path : 0), "perfil.txt");
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "muestras %u\n", g_pf_total);
    static unsigned char used[PERF_N];
    memset(used, 0, sizeof(used));
    for (int r = 0; r < 120; r++) {
        int best = -1;
        for (int i = 0; i < PERF_N; i++)
            if (g_pf_rva[i] && !used[i] && (best < 0 || g_pf_cnt[i] > g_pf_cnt[best])) best = i;
        if (best < 0) break;
        used[best] = 1;
        uintptr_t a = g_pf_rva[best] & ~((uintptr_t)1 << 62);
        HMODULE m = NULL;
        char mod[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
            GetModuleFileNameA(m, mod, sizeof(mod));
            char *b = strrchr(mod, '\\');
            if (b) memmove(mod, b + 1, strlen(b));
        }
        fprintf(fp, "%6u %s %s+%llX\n", g_pf_cnt[best], (g_pf_rva[best] >> 62) ? "llamador" : "        ", mod,
                (unsigned long long)(a - (uintptr_t)m));
    }
    fclose(fp);
}
static DWORD WINAPI perfil(LPVOID arg) {
    (void)arg;
    timeBeginPeriod(1);
    HANDLE th = NULL;
    DWORD tid = 0, last = GetTickCount();
    for (;;) {
        Sleep(1);
        if (!port_draw_tid || !port_en_partido) continue;
        if (tid != port_draw_tid) {
            if (th) CloseHandle(th);
            tid = port_draw_tid;
            th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        }
        if (!th) continue;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_FULL;
        if (SuspendThread(th) == (DWORD)-1) continue;
        uintptr_t r0 = 0, r1 = 0;
        if (GetThreadContext(th, &ctx)) {
            r0 = ctx.Rip;
            DWORD64 img = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &img, NULL);
            if (fe) {
                PVOID hd; DWORD64 est;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, img, ctx.Rip, fe, &ctx, &hd, &est, NULL);
                r1 = ctx.Rip;
            }
        }
        ResumeThread(th);
        if (r0) { pf_add(r0); g_pf_total++; }
        /* el llamador se guarda con el bit 63 para distinguirlo */
        if (r1) pf_add(r1 | ((uintptr_t)1 << 62));
        if (GetTickCount() - last > 20000) { last = GetTickCount(); pf_dump(); }
    }
    return 0;
}
/* MLB15_PERFIL=2: perfilador de TODOS los hilos del juego durante el partido. Cada ~2 ms
 * detiene un instante cada hilo, desenrolla su pila (hasta 32 marcos) y cuenta:
 *   - por hilo: muestras totales, en espera (la cima está fuera del exe: ntdll/kernelbase...)
 *     y por módulo de la cima;
 *   - por dirección del exe: «propio» (la cima) e «incluido» (aparece en la pila; una vez por
 *     muestra), para traducir con mlb15_recomp.map (tools: perfil_map.py).
 * Escribe perfil.txt cada 20 s. MLB15_PERFIL_SEG=n: empieza a contar n s después de entrar al
 * partido (por defecto 10). */
int port_thread_info(int i, HANDLE *h, const char **name);
#define PT_MAX 256
static struct { char name[32]; unsigned tot, espera; } g_pt[PT_MAX];
#define PM_N 32
static struct { char mod[48]; unsigned n; } g_pm[PM_N];
static uintptr_t g_ps_rva[PERF_N], g_pi_rva[PERF_N];
static unsigned  g_ps_cnt[PERF_N], g_pi_cnt[PERF_N];
static unsigned  g_p2_total;
/* por hilo: 1.er y 2.º marco del exe de cada muestra (clave = hilo << 40 | rva) */
static uintptr_t g_ph1_rva[PERF_N], g_ph2_rva[PERF_N];
static unsigned  g_ph1_cnt[PERF_N], g_ph2_cnt[PERF_N];
static uintptr_t g_exe_base, g_exe_end;
static void p2_add(uintptr_t *tab, unsigned *cnt, uintptr_t rva) {
    uint32_t h = (uint32_t)(rva * 2654435761u) >> 16;
    for (uint32_t i = 0; i < PERF_N; i++) {
        uint32_t k = (h + i) & (PERF_N - 1);
        if (tab[k] == rva) { cnt[k]++; return; }
        if (!tab[k]) { tab[k] = rva; cnt[k] = 1; return; }
    }
}
static void p2_mod(uintptr_t a) {
    HMODULE m = NULL;
    char mod[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
        GetModuleFileNameA(m, mod, sizeof(mod));
    }
    char *b = strrchr(mod, '\\');
    const char *nm = b ? b + 1 : mod;
    for (int i = 0; i < PM_N; i++) {
        if (!g_pm[i].n) { snprintf(g_pm[i].mod, sizeof(g_pm[i].mod), "%s", nm); g_pm[i].n = 1; return; }
        if (!strcmp(g_pm[i].mod, nm)) { g_pm[i].n++; return; }
    }
}
static void p2_top(FILE *fp, const char *tit, uintptr_t *tab, unsigned *cnt, int n) {
    static unsigned char used[PERF_N];
    memset(used, 0, sizeof(used));
    fprintf(fp, "== %s\n", tit);
    for (int r = 0; r < n; r++) {
        int best = -1;
        for (int i = 0; i < PERF_N; i++)
            if (tab[i] && !used[i] && (best < 0 || cnt[i] > cnt[best])) best = i;
        if (best < 0) break;
        used[best] = 1;
        fprintf(fp, "%7u %5.1f%% +%llX\n", cnt[best], 100.0 * cnt[best] / (g_p2_total ? g_p2_total : 1), (unsigned long long)tab[best]);
    }
}
static void p2_dump(double seg) {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof(path));
    char *sl = strrchr(path, '\\');
    snprintf(sl ? sl + 1 : path, sizeof(path) - (size_t)(sl ? sl + 1 - path : 0), "perfil.txt");
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    extern unsigned port_display_frames(void);
    fprintf(fp, "muestras %u en %.1f s (fotograma %u)\n== hilos (muestras, %% en espera)\n", g_p2_total, seg, port_display_frames());
    for (int i = 0; i < PT_MAX; i++)
        if (g_pt[i].tot)
            fprintf(fp, "%7u %5.1f%% espera  %s\n", g_pt[i].tot, 100.0 * g_pt[i].espera / g_pt[i].tot, g_pt[i].name);
    fprintf(fp, "== módulo en la cima (todas las muestras)\n");
    for (int i = 0; i < PM_N; i++) if (g_pm[i].n) fprintf(fp, "%7u %s\n", g_pm[i].n, g_pm[i].mod);
    for (int t = 0; t < PT_MAX; t++) {
        if (!g_pt[t].tot || g_pt[t].espera * 100u > g_pt[t].tot * 99u) continue;   /* hilos casi parados: fuera */
        for (int q = 0; q < 2; q++) {
            uintptr_t *tab = q ? g_ph2_rva : g_ph1_rva;
            unsigned *cnt = q ? g_ph2_cnt : g_ph1_cnt;
            fprintf(fp, "== hilo %d %s (%s marco del exe)\n", t, g_pt[t].name, q ? "2.o" : "1.er");
            static unsigned char used[PERF_N];
            memset(used, 0, sizeof(used));
            for (int r = 0; r < 14; r++) {
                int best = -1;
                for (int i = 0; i < PERF_N; i++)
                    if (tab[i] && (int)(tab[i] >> 40) == t && !used[i] && (best < 0 || cnt[i] > cnt[best])) best = i;
                if (best < 0) break;
                used[best] = 1;
                fprintf(fp, "%7u %5.1f%% +%llX\n", cnt[best], 100.0 * cnt[best] / g_pt[t].tot,
                        (unsigned long long)(tab[best] & (((uintptr_t)1 << 40) - 1)));
            }
        }
    }
    p2_top(fp, "propio (exe)", g_ps_rva, g_ps_cnt, 200);
    p2_top(fp, "incluido (exe)", g_pi_rva, g_pi_cnt, 300);
    fclose(fp);
}
static DWORD WINAPI perfil2(LPVOID arg) {
    (void)arg;
    timeBeginPeriod(1);
    HMODULE me = GetModuleHandleA(NULL);
    g_exe_base = (uintptr_t)me;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)me + ((IMAGE_DOS_HEADER *)me)->e_lfanew);
    g_exe_end = g_exe_base + nt->OptionalHeader.SizeOfImage;
    const char *e = getenv("MLB15_PERFIL_SEG");
    DWORD retraso = (DWORD)((e ? atoi(e) : 10) * 1000), t_partido = 0, t0 = 0, last = 0;
    for (;;) {
        Sleep(2);
        if (!port_en_partido) continue;
        DWORD now = GetTickCount();
        if (!t_partido) t_partido = now;
        if (now - t_partido < retraso) continue;
        if (!t0) { t0 = last = now; }
        for (int i = 0; i < PT_MAX; i++) {
            HANDLE th;
            const char *nm;
            if (!port_thread_info(i, &th, &nm)) continue;
            {   /* MLB15_PERFIL_HILO=texto: solo los hilos cuyo nombre lo contiene */
                static const char *solo = (const char *)1;
                if (solo == (const char *)1) solo = getenv("MLB15_PERFIL_HILO");
                if (solo && !strstr(nm, solo)) continue;
            }
            if (SuspendThread(th) == (DWORD)-1) continue;
            CONTEXT ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_FULL;
            uintptr_t pila[32];
            int np = 0;
            if (GetThreadContext(th, &ctx)) {
                for (np = 0; np < 32 && ctx.Rip; np++) {
                    pila[np] = (uintptr_t)ctx.Rip;
                    DWORD64 img = 0;
                    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &img, NULL);
                    if (fe) {
                        PVOID hd; DWORD64 est;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, img, ctx.Rip, fe, &ctx, &hd, &est, NULL);
                    } else {   /* hoja sin datos de desenrollado: solo en la cima */
                        if (np || !ctx.Rsp) break;
                        ctx.Rip = *(DWORD64 *)ctx.Rsp;
                        ctx.Rsp += 8;
                    }
                }
            }
            ResumeThread(th);
            if (!np) continue;
            if (!g_pt[i].name[0]) snprintf(g_pt[i].name, sizeof(g_pt[i].name), "%s", nm);
            g_pt[i].tot++;
            g_p2_total++;
            int dentro = pila[0] >= g_exe_base && pila[0] < g_exe_end;
            if (!dentro) g_pt[i].espera++;
            p2_mod(pila[0]);
            if (dentro) p2_add(g_ps_rva, g_ps_cnt, pila[0] - g_exe_base);
            {
                int q = 0;
                for (int k = 0; k < np && q < 2; k++)
                    if (pila[k] >= g_exe_base && pila[k] < g_exe_end) {
                        uintptr_t key = ((uintptr_t)i << 40) | (pila[k] - g_exe_base);
                        if (q++ == 0) p2_add(g_ph1_rva, g_ph1_cnt, key);
                        else p2_add(g_ph2_rva, g_ph2_cnt, key);
                    }
            }
            for (int k = 0; k < np; k++) {
                if (pila[k] < g_exe_base || pila[k] >= g_exe_end) continue;
                int rep = 0;
                for (int j = 0; j < k; j++) if (pila[j] == pila[k]) rep = 1;
                if (!rep) p2_add(g_pi_rva, g_pi_cnt, pila[k] - g_exe_base);
            }
        }
        if (now - last > 20000) { last = now; p2_dump((now - t0) / 1000.0); }
    }
    return 0;
}
/* MLB15_CPU_HILOS=s: cada s segundos, CPU real de cada hilo del juego en ese intervalo
 * (GetThreadTimes; a diferencia de MLB15_PERFIL, cuenta también las esperas activas). */
static DWORD WINAPI cpu_hilos(LPVOID arg) {
    int seg = (int)(intptr_t)arg;
    static ULONGLONG prev[PT_MAX];
    for (;;) {
        Sleep((DWORD)seg * 1000);
        printf("[cpu]");
        for (int i = 0; i < PT_MAX; i++) {
            HANDLE th; const char *nm;
            if (!port_thread_info(i, &th, &nm)) continue;
            FILETIME a, b, k, u;
            if (!GetThreadTimes(th, &a, &b, &k, &u)) continue;
            ULONGLONG t = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
            double pct = prev[i] ? (double)(t - prev[i]) / (seg * 1e7) * 100.0 : 0.0;
            prev[i] = t;
            if (pct >= 3.0) printf(" %s %.0f%%", nm, pct);
        }
        printf("\n");
        fflush(stdout);
    }
    return 0;
}

void port_perfil_start(void) {
    const char *ch = getenv("MLB15_CPU_HILOS");
    if (ch && atoi(ch) > 0) CreateThread(NULL, 0, cpu_hilos, (LPVOID)(intptr_t)atoi(ch), 0, NULL);
    const char *e = getenv("MLB15_PERFIL");
    if (e && *e == '2') CreateThread(NULL, 0, perfil2, NULL, 0, NULL);
    else if (e) CreateThread(NULL, 0, perfil, NULL, 0, NULL);
}
