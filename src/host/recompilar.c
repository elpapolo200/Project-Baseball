/* recompilar.c — genera datos\juego.dll EN EL PC DEL JUGADOR a partir de su eboot descifrado.
 *
 * Así lo que se reparte (MLB15.exe + herramientas\) no lleva nada del juego: el código del
 * juego se traduce aquí, la primera vez, con las mismas herramientas que en el taller:
 *   1. armrecomp emit datos\eboot.elf -> C (~870 MB, ~5 min)
 *   2. se parte en trozos y se corrige (lo que hacían split_recomp.py y fix_recomp.py)
 *   3. clang compila los trozos en paralelo (según núcleos y memoria libre)
 *   4. llvm-dlltool crea la biblioteca de importación del exe (exports.def) y lld-link
 *      enlaza juego.dll con juego_base.c (registros por hilo, TLS, memcpy).
 * datos\juego.txt guarda la huella del ELF y la versión: si cambian, se repite.
 *
 * herramientas\ (junto al exe): armrecomp.exe, clang.exe, lld-link.exe, llvm-dlltool.exe,
 * db\*.yml (NIDs, vita-headers), include\ (vitarecomp + shim + clang\), juego_base.c,
 * exports.def. Todo es código propio o libre (LLVM: Apache 2.0; vita-headers: MIT).
 */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "vitarecomp/cpu.h"
#include "recompilar.h"
#include <intrin.h>

/* Huella del procesador (PLAN_RENDIMIENTO.md fase 2): juego.dll se compila con -march=native, así
 * que solo vale para este modelo de CPU. Si se copia la carpeta a otro PC la huella no coincide y
 * se vuelve a preparar (si no, un procesador más viejo se cerraría con una instrucción ilegal). */
static uint32_t cpu_huella(char *marca, size_t max) {
    int r[4]; char b[49] = { 0 };
    __cpuid(r, 0x80000000);
    if ((unsigned)r[0] >= 0x80000004u) {
        for (int i = 0; i < 3; i++) { __cpuid(r, 0x80000002 + i); memcpy(b + 16 * i, r, 16); }
    }
    uint32_t h = 2166136261u;
    for (const char *c = b; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    __cpuid(r, 1); h = (h ^ (uint32_t)r[0]) * 16777619u;      /* familia/modelo/stepping */
    __cpuidex(r, 7, 0); h = (h ^ (uint32_t)r[1]) * 16777619u;  /* AVX2, BMI, AVX-512... */
    if (marca) { const char *c = b; while (*c == ' ') c++; snprintf(marca, max, "%s", c); }
    return h;
}

/* Cambiar si cambia lo que el exe espera del DLL (exports, vita_cpu_state, flags). */
#define RECOMP_VERSION 6   /* 3: armrecomp con caídas tras decodificaciones solapadas (2026-11-07)
                            * 4: memoria de la Vita en línea, VITA_MEM_DIRECTA (2026-11-08)
                            * 5: -march=native, huella con el procesador (2026-11-08)
                            * 6: NEON frecuente escrito en C (neon_emit.h) y -ffp-contract=off (2026-11-08) */
#define MB_POR_PARTE   12          /* trozos pequeños: menos memoria por clang */

/* ------------------------------------------------------------------------------------------ */
/* estado compartido con la ventana                                                           */

static CRITICAL_SECTION g_cs;
static char g_paso[256], g_detalle[256];
static int g_pct = -1;               /* -1 = barra indeterminada */
static volatile LONG g_cancelar, g_fin;
static int g_resultado;
static char g_error[1024];
static DWORD g_t0;
static HWND g_wnd;
static HANDLE g_procs[64];
static int g_nprocs;

static void estado(const char *paso, const char *detalle, int pct) {
    EnterCriticalSection(&g_cs);
    if (paso) snprintf(g_paso, sizeof(g_paso), "%s", paso);
    if (detalle) snprintf(g_detalle, sizeof(g_detalle), "%s", detalle);
    g_pct = pct;
    LeaveCriticalSection(&g_cs);
    if (g_wnd) PostMessageA(g_wnd, WM_APP, 0, 0);
    printf("[recompilar] %s %s %d\n", paso ? paso : "", detalle ? detalle : "", pct);
    fflush(stdout);
}

static int fallo(const char *fmt, const char *a) {
    snprintf(g_error, sizeof(g_error), fmt, a ? a : "");
    printf("[recompilar] ERROR: %s\n", g_error);
    fflush(stdout);
    return -1;
}

/* ------------------------------------------------------------------------------------------ */
/* procesos                                                                                   */

static HANDLE lanzar(const char *cmd, const char *log) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE out = CreateFileA(log, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    char *c = _strdup(cmd);
    BOOL ok = CreateProcessA(NULL, c, NULL, NULL, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi);
    free(c);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) return NULL;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static void registra(HANDLE h) {
    EnterCriticalSection(&g_cs);
    if (g_nprocs < 64) g_procs[g_nprocs++] = h;
    LeaveCriticalSection(&g_cs);
}
static void olvida(HANDLE h) {
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_nprocs; i++) if (g_procs[i] == h) { g_procs[i] = g_procs[--g_nprocs]; break; }
    LeaveCriticalSection(&g_cs);
}

/* Espera a un proceso; devuelve su código de salida o -1 si se canceló. */
static int esperar(HANDLE h) {
    registra(h);
    while (WaitForSingleObject(h, 200) == WAIT_TIMEOUT)
        if (g_cancelar) { TerminateProcess(h, 1); WaitForSingleObject(h, 2000); olvida(h); CloseHandle(h); return -1; }
    DWORD code = 1;
    GetExitCodeProcess(h, &code);
    olvida(h);
    CloseHandle(h);
    return (int)code;
}

static int ejecutar(const char *cmd, const char *log) {
    HANDLE h = lanzar(cmd, log);
    if (!h) return fallo("no se pudo ejecutar: %s", cmd);
    return esperar(h);
}

/* ------------------------------------------------------------------------------------------ */
/* 2. partir y corregir (antes split_recomp.py + fix_recomp.py)                               */

typedef struct { char *p; size_t n, cap; } Buf;
static void buf_add(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = (char *)realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}

/* Lee una línea (con su '\n') en *b; 0 al final del archivo. */
static int leer_linea(FILE *f, Buf *b) {
    b->n = 0;
    char tmp[65536];
    while (fgets(tmp, sizeof(tmp), f)) {
        size_t n = strlen(tmp);
        buf_add(b, tmp, n);
        if (n && tmp[n - 1] == '\n') return 1;
    }
    return b->n > 0;
}

static int empieza(const char *s, const char *pre) { return strncmp(s, pre, strlen(pre)) == 0; }

/* void vita_func_XXXXXXXX(void) { */
static int es_def(const char *s) {
    if (!empieza(s, "void vita_func_")) return 0;
    for (int i = 0; i < 8; i++) if (!strchr("0123456789ABCDEF", s[15 + i]) || !s[15 + i]) return 0;
    return empieza(s + 23, "(void) {");
}

/* La línea sin comentarios /*...* / ni cadenas "..." (como strip_code). */
static void sin_comentarios(const char *s, Buf *o) {
    o->n = 0;
    buf_add(o, "", 0);
    while (*s) {
        if (s[0] == '/' && s[1] == '*') {
            const char *e = strstr(s + 2, "*/");
            if (!e) break;   /* comentario sin cerrar en esta línea: como el regex, no se quita */
            s = e + 2;
        } else if (*s == '"') {
            const char *e = strchr(s + 1, '"');
            if (!e) { buf_add(o, s, strlen(s)); break; }
            s = e + 1;
        } else { buf_add(o, s, 1); s++; }
    }
}

static int cuenta(const char *s, char c) { int n = 0; for (; *s; s++) n += *s == c; return n; }

static int es_etiqueta(const char *s) {   /* ^L_[0-9A-F]+:\s*$ */
    if (!empieza(s, "L_")) return 0;
    const char *p = s + 2;
    if (!strchr("0123456789ABCDEF", *p) || !*p) return 0;
    while (*p && strchr("0123456789ABCDEF", *p)) p++;
    if (*p != ':') return 0;
    for (p++; *p; p++) if (!strchr(" \t\r\n", *p)) return 0;
    return 1;
}

static const char *salta_blancos(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }
static int es_blanco(const char *s) { for (; *s; s++) if (!strchr(" \t\r\n", *s)) return 0; return 1; }
static int es_cierre(const char *s) {   /* strip() == "}" */
    s = salta_blancos(s);
    if (*s != '}') return 0;
    return es_blanco(s + 1);
}

typedef struct {
    FILE *out;
    size_t tam;
    char addr[9];
    Buf pend;          /* etiqueta + comentarios/blancos que la siguen */
    int hay_pend;
    int saltando;      /* profundidad de llaves de una sentencia con '?' */
    long trampas, etiquetas;
} Fix;

static void fix_salida(Fix *x, const char *s, size_t n) { fwrite(s, 1, n, x->out); x->tam += n; }

static void fix_vacia_pend(Fix *x, int poner_punto_coma) {
    if (!x->hay_pend) return;
    if (poner_punto_coma) {
        /* la etiqueta es la primera línea del pendiente: se le añade " ;" antes del salto */
        char *nl = strchr(x->pend.p, '\n');
        size_t n1 = nl ? (size_t)(nl - x->pend.p) : x->pend.n;
        size_t m = n1;
        while (m && (x->pend.p[m - 1] == '\r' || x->pend.p[m - 1] == ' ' || x->pend.p[m - 1] == '\t')) m--;   /* rstrip */
        fix_salida(x, x->pend.p, m);
        fix_salida(x, " ;", 2);
        if (nl) fix_salida(x, x->pend.p + n1, x->pend.n - n1);
        x->etiquetas++;
    } else fix_salida(x, x->pend.p, x->pend.n);
    x->pend.n = 0;
    x->hay_pend = 0;
}

static void fix_linea(Fix *x, const char *s) {
    static Buf code;
    size_t n = strlen(s);
    /* dirección del comentario de la instrucción: "  /* 81234567 ..." */
    const char *t = salta_blancos(s);
    if (t[0] == '/' && t[1] == '*' && t[2] == ' ') {
        int ok = 1;
        for (int i = 0; i < 8; i++) if (!t[3 + i] || !strchr("0123456789ABCDEF", t[3 + i])) { ok = 0; break; }
        if (ok && t[11] == ' ') { memcpy(x->addr, t + 3, 8); x->addr[8] = 0; }
    }
    sin_comentarios(s, &code);
    if (x->saltando > 0) {   /* resto del bloque de una sentencia con '?' */
        x->saltando += cuenta(code.p, '{') - cuenta(code.p, '}');
        return;
    }
    if (x->hay_pend) {
        if (es_blanco(s) || empieza(t, "/*")) { buf_add(&x->pend, s, n); return; }
        fix_vacia_pend(x, es_cierre(s));
    }
    if (strchr(code.p, '?') && !strchr(code.p, ':') && *t != '#') {
        char linea[160];
        int indent = (int)(t - s);
        snprintf(linea, sizeof(linea), "%.*svita_trap_unimpl(0x%s, 0, \"operando ?\");", indent, s, x->addr);
        fix_salida(x, linea, strlen(linea));
        /* el salto de línea de la original (si la sentencia ocupa más líneas, el de la última se pierde
         * igual que en fix_recomp.py, que unía con "\n": aquí se pone uno siempre) */
        fix_salida(x, "\n", 1);
        x->trampas++;
        int depth = cuenta(code.p, '{') - cuenta(code.p, '}');
        if (depth > 0) x->saltando = depth;
        return;
    }
    if (es_etiqueta(s)) {
        x->pend.n = 0;
        buf_add(&x->pend, s, n);
        x->hay_pend = 1;
        return;
    }
    fix_salida(x, s, n);
}

/* Parte el C generado en trozos (cabecera + cuerpos) y aplica las correcciones. Devuelve el número
 * de trozos de código (además de parte_tabla.c) o -1. */
static int partir(const char *entrada, const char *dir) {
    FILE *f = fopen(entrada, "rb");
    if (!f) return fallo("no se pudo abrir %s", entrada);
    setvbuf(f, NULL, _IOFBF, 1 << 20);
    Buf linea = { 0 }, cab = { 0 };
    Fix x;
    memset(&x, 0, sizeof(x));
    strcpy(x.addr, "00000000");
    int cabecera = 1, tabla = 0, idx = 0;
    size_t limite = (size_t)MB_POR_PARTE << 20;
    _fseeki64(f, 0, SEEK_END);
    long long total = _ftelli64(f), leido = 0;
    _fseeki64(f, 0, SEEK_SET);
    char ruta[MAX_PATH], det[128];
    int ultimo_pct = -1;
    while (leer_linea(f, &linea)) {
        if (g_cancelar) break;
        leido += (long long)linea.n;
        if (cabecera) {
            if (es_def(linea.p)) cabecera = 0;
            else { buf_add(&cab, linea.p, linea.n); continue; }
        }
        int nueva = 0;
        if (!tabla && empieza(linea.p, "static const vita_dispatch_entry")) {
            tabla = 1;
            snprintf(ruta, sizeof(ruta), "%s\\parte_tabla.c", dir);
            nueva = 1;
        } else if (!tabla && es_def(linea.p) && (!x.out || x.tam > limite)) {
            snprintf(ruta, sizeof(ruta), "%s\\parte_%03d.c", dir, idx++);
            nueva = 1;
        }
        if (nueva) {
            fix_vacia_pend(&x, 0);
            if (x.out) fclose(x.out);
            x.out = fopen(ruta, "wb");
            if (!x.out) { fclose(f); return fallo("no se pudo crear %s", ruta); }
            setvbuf(x.out, NULL, _IOFBF, 1 << 20);
            fwrite(cab.p, 1, cab.n, x.out);
            x.tam = 0;
            x.saltando = 0;
        }
        fix_linea(&x, linea.p);
        int pct = total ? (int)(leido * 100 / total) : 0;
        if (pct != ultimo_pct) {
            ultimo_pct = pct;
            snprintf(det, sizeof(det), "%d trozos", idx);
            estado(NULL, det, pct);
        }
    }
    fix_vacia_pend(&x, 0);
    if (x.out) fclose(x.out);
    fclose(f);
    free(linea.p); free(cab.p); free(x.pend.p);
    printf("[recompilar] %d trozos + tabla; %ld sentencias con '?' -> trampa, %ld etiquetas corregidas\n", idx, x.trampas, x.etiquetas);
    if (g_cancelar) return -1;
    if (!tabla) return fallo("el C generado no tiene tabla de despacho (%s)", entrada);
    return idx;
}

/* ------------------------------------------------------------------------------------------ */
/* utilidades                                                                                  */

static uint32_t crc32_archivo(const char *ruta, long long *tam) {
    static uint32_t tab[256];
    if (!tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; }
    FILE *f = fopen(ruta, "rb");
    if (!f) return 0;
    static unsigned char b[1 << 16];
    uint32_t c = 0xFFFFFFFFu;
    size_t n;
    *tam = 0;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) { *tam += (long long)n; for (size_t i = 0; i < n; i++) c = tab[(c ^ b[i]) & 255] ^ (c >> 8); }
    fclose(f);
    return ~c;
}

static void borrar_carpeta(const char *dir) {
    char pat[MAX_PATH], r[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            snprintf(r, sizeof(r), "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) borrar_carpeta(r); else DeleteFileA(r);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

static int existe(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

static void mmss(DWORD ms, char *o, size_t n) { snprintf(o, n, "%lu:%02lu", ms / 60000, (ms / 1000) % 60); }

/* ------------------------------------------------------------------------------------------ */
/* el trabajo                                                                                  */

static char g_exe_dir[MAX_PATH], g_elf[MAX_PATH], g_dll[MAX_PATH], g_huella[256];

static int trabajo(void) {
    char her[MAX_PATH], tmp[MAX_PATH], c[MAX_PATH], cmd[4096], log[MAX_PATH];
    snprintf(her, sizeof(her), "%s\\herramientas", g_exe_dir);
    snprintf(tmp, sizeof(tmp), "%s\\datos\\compilando", g_exe_dir);
    const char *nec[] = { "armrecomp.exe", "clang.exe", "lld-link.exe", "llvm-dlltool.exe", "juego_base.c", "exports.def", "db", "include" };
    for (size_t i = 0; i < sizeof(nec) / sizeof(nec[0]); i++) {
        snprintf(c, sizeof(c), "%s\\%s", her, nec[i]);
        if (!existe(c)) return fallo("falta %s (la carpeta herramientas debe ir junto a ProjectBaseball.exe)", c);
    }
    borrar_carpeta(tmp);
    CreateDirectoryA(tmp, NULL);

    /* 1. traducir */
    estado("Traduciendo el código del juego (paso 1 de 3)", "armrecomp: ARM → C", -1);
    snprintf(c, sizeof(c), "%s\\juego.c", tmp);
    snprintf(log, sizeof(log), "%s\\armrecomp.log", tmp);
    snprintf(cmd, sizeof(cmd), "\"%s\\armrecomp.exe\" emit \"%s\" \"%s\" 999999 \"%s\\db\"", her, g_elf, c, her);
    int r = ejecutar(cmd, log);
    if (r < 0 && g_cancelar) return -1;
    if (r != 0 || !existe(c)) return fallo("la traducción falló (mira %s)", log);
    {   /* resumen de armrecomp al registro (el .log se borra al terminar) */
        FILE *lf = fopen(log, "r");
        char ln[256];
        while (lf && fgets(ln, sizeof(ln), lf))
            if (strstr(ln, "translated") || strstr(ln, "trapped") || strstr(ln, "overlaps") || strstr(ln, "functions"))
                printf("[recompilar] armrecomp: %s", ln);
        if (lf) fclose(lf);
    }

    /* 2. partir y corregir */
    estado("Preparando los trozos (paso 2 de 3)", "", 0);
    int n = partir(c, tmp);
    if (n < 0) return -1;
    DeleteFileA(c);

    /* 3. compilar en paralelo: cada clang usa ~1 GB con trozos de 12 MB */
    int total = n + 2;   /* trozos + tabla + juego_base */
    char (*fuentes)[MAX_PATH] = calloc((size_t)total, MAX_PATH);
    for (int i = 0; i < n; i++) snprintf(fuentes[i], MAX_PATH, "%s\\parte_%03d.c", tmp, i);
    snprintf(fuentes[n], MAX_PATH, "%s\\parte_tabla.c", tmp);
    snprintf(fuentes[n + 1], MAX_PATH, "%s\\juego_base.c", her);
    SYSTEM_INFO si; GetSystemInfo(&si);
    MEMORYSTATUSEX ms = { sizeof(ms) }; GlobalMemoryStatusEx(&ms);
    int por_cpu = (int)si.dwNumberOfProcessors - 1;
    int por_ram = (int)((ms.ullAvailPhys >> 20) / 1400);
    int hilos = por_cpu < por_ram ? por_cpu : por_ram;
    if (hilos < 1) hilos = 1;
    if (hilos > 32) hilos = 32;
    printf("[recompilar] %d trozos, %d a la vez (%lu núcleos, %llu MB libres)\n", total, hilos, si.dwNumberOfProcessors, ms.ullAvailPhys >> 20);
    const char *flags = "--target=x86_64-pc-windows-msvc -fms-compatibility-version=19.20 -O2 -ffreestanding -nostdinc "
                        "-fno-stack-protector -mno-stack-arg-probe -msse4.1 -fno-math-errno -fasynchronous-unwind-tables -w "
                        "-DVITA_MEM_DIRECTA -march=native -ffp-contract=off";   /* sin FMA: la Vita redondea mul y add por separado */
    HANDLE act[32]; int act_i[32], nact = 0, sig = 0, hechos = 0;
    while (hechos < total) {
        while (nact < hilos && sig < total && !g_cancelar) {
            const char *extra = sig == n + 1 ? "-fno-builtin" : "";
            snprintf(log, sizeof(log), "%s\\c%03d.log", tmp, sig);
            snprintf(cmd, sizeof(cmd), "\"%s\\clang.exe\" %s %s -isystem \"%s\\include\\clang\" -I \"%s\\include\\shim\" -I \"%s\\include\" "
                     "-c \"%s\" -o \"%s\\o%03d.obj\"", her, flags, extra, her, her, her, fuentes[sig], tmp, sig);
            HANDLE h = lanzar(cmd, log);
            if (!h) { free(fuentes); return fallo("no se pudo ejecutar clang (%s)", her); }
            registra(h);
            act[nact] = h; act_i[nact] = sig; nact++; sig++;
        }
        if (g_cancelar) { free(fuentes); return -1; }
        DWORD w = WaitForMultipleObjects((DWORD)nact, act, FALSE, 300);
        if (w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + (DWORD)nact) {
            int k = (int)(w - WAIT_OBJECT_0);
            DWORD code = 1; GetExitCodeProcess(act[k], &code);
            olvida(act[k]); CloseHandle(act[k]);
            int i = act_i[k];
            act[k] = act[nact - 1]; act_i[k] = act_i[nact - 1]; nact--;
            if (code != 0) {
                snprintf(log, sizeof(log), "%s\\c%03d.log", tmp, i);
                free(fuentes);
                return fallo("falló la compilación de un trozo (mira %s)", log);
            }
            if (i < n + 1) DeleteFileA(fuentes[i]);   /* el .c ya no hace falta (juego_base es de herramientas) */
            hechos++;
        }
        char det[160], t1[16], t2[16];
        DWORD dt = GetTickCount() - g_t0;
        mmss(dt, t1, sizeof(t1));
        if (hechos) { DWORD resto = (DWORD)((double)dt / hechos * (total - hechos)); mmss(resto, t2, sizeof(t2)); }
        else strcpy(t2, "calculando");
        snprintf(det, sizeof(det), "%d de %d trozos · %d a la vez · %s transcurrido · quedan ~%s", hechos, total, hilos, t1, t2);
        estado("Compilando el juego para tu PC (paso 3 de 3)", det, hechos * 100 / total);
    }
    free(fuentes);

    /* 4. biblioteca de importación del exe (con su nombre real) y enlace */
    estado("Enlazando juego.dll", "", -1);
    char exe[MAX_PATH], *base;
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    base = strrchr(exe, '\\'); base = base ? base + 1 : exe;
    snprintf(log, sizeof(log), "%s\\dlltool.log", tmp);
    snprintf(cmd, sizeof(cmd), "\"%s\\llvm-dlltool.exe\" -m i386:x86-64 -D \"%s\" -d \"%s\\exports.def\" -l \"%s\\exe.lib\"", her, base, her, tmp);
    if (ejecutar(cmd, log) != 0) return g_cancelar ? -1 : fallo("falló llvm-dlltool (mira %s)", log);
    /* respuesta con todos los objetos */
    snprintf(c, sizeof(c), "%s\\enlace.rsp", tmp);
    FILE *rsp = fopen(c, "w");
    if (!rsp) return fallo("no se pudo crear %s", c);
    for (int i = 0; i < total; i++) fprintf(rsp, "\"%s\\o%03d.obj\"\n", tmp, i);
    fprintf(rsp, "\"%s\\exe.lib\"\n", tmp);
    fclose(rsp);
    char dll_tmp[MAX_PATH];
    snprintf(dll_tmp, sizeof(dll_tmp), "%s\\juego.dll", tmp);
    snprintf(log, sizeof(log), "%s\\enlace.log", tmp);
    snprintf(cmd, sizeof(cmd), "\"%s\\lld-link.exe\" /nologo /DLL /ENTRY:DllMainCRTStartup /NODEFAULTLIB /OPT:REF "
             "/EXPORT:vita_register_functions /EXPORT:vita_func_810056D4 /EXPORT:vita_func_81B078C8 "
             "/EXPORT:vita_import_vars,DATA /EXPORT:vita_import_var_count,DATA /OUT:\"%s\" @\"%s\"", her, dll_tmp, c);
    if (ejecutar(cmd, log) != 0) return g_cancelar ? -1 : fallo("falló el enlace (mira %s)", log);
    DeleteFileA(g_dll);
    if (!MoveFileA(dll_tmp, g_dll)) return fallo("no se pudo mover juego.dll a %s", g_dll);
    borrar_carpeta(tmp);
    return 0;
}

static DWORD WINAPI hilo_trabajo(void *p) {
    (void)p;
    g_resultado = trabajo();
    InterlockedExchange(&g_fin, 1);
    if (g_wnd) PostMessageA(g_wnd, WM_APP + 1, 0, 0);
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* ventana de progreso                                                                          */

#include "ui_estilo.h"

enum { ID_BARRA = 100, ID_CANCELAR = 101 };
static HWND g_barra;

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_APP: {
        int pct;
        EnterCriticalSection(&g_cs); pct = g_pct; LeaveCriticalSection(&g_cs);
        LONG_PTR st = GetWindowLongPtrA(g_barra, GWL_STYLE);
        if (pct < 0) {
            if (!(st & PBS_MARQUEE)) { SetWindowLongPtrA(g_barra, GWL_STYLE, st | PBS_MARQUEE); SendMessageA(g_barra, PBM_SETMARQUEE, 1, 30); }
        } else {
            if (st & PBS_MARQUEE) { SendMessageA(g_barra, PBM_SETMARQUEE, 0, 0); SetWindowLongPtrA(g_barra, GWL_STYLE, st & ~PBS_MARQUEE); }
            SendMessageA(g_barra, PBM_SETPOS, (WPARAM)pct, 0);
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_APP + 1:
        DestroyWindow(h);
        return 0;
    case WM_TIMER:
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == ID_CANCELAR) {
            if (MessageBoxW(h, L"¿Cancelar la preparación? La próxima vez que abras el juego empezará de nuevo.",
                            L"Project Baseball", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                InterlockedExchange(&g_cancelar, 1);
                EnableWindow((HWND)l, FALSE);
            }
        }
        return 0;
    case WM_CLOSE:
        SendMessageA(h, WM_COMMAND, ID_CANCELAR, (LPARAM)GetDlgItem(h, ID_CANCELAR));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        char paso[256], det[256];
        EnterCriticalSection(&g_cs); strcpy(paso, g_paso); strcpy(det, g_detalle); LeaveCriticalSection(&g_cs);
        ui_pinta_fondo(h, dc, &rc);
        ui_texto(h, dc, L"Preparando Project Baseball para tu PC", 28, 74, ui_lg(rc.right) - 56, UI_TITULO);
        ui_texto(h, dc, L"Solo la primera vez: se traduce y se compila el juego a partir de tu copia. "
                        L"Puede tardar entre 15 y 40 minutos según el PC; después arrancará al momento.", 28, 112, ui_lg(rc.right) - 56, UI_NORMAL_TENUE);
        wchar_t wp[256], wd[256];
        MultiByteToWideChar(CP_UTF8, 0, paso, -1, wp, 256);
        MultiByteToWideChar(CP_UTF8, 0, det, -1, wd, 256);
        ui_texto(h, dc, wp, 28, 178, ui_lg(rc.right) - 56, UI_FUERTE);
        ui_texto(h, dc, wd, 28, 238, ui_lg(rc.right) - 56, UI_PEQUENO_TENUE);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLORBTN: case WM_CTLCOLORSTATIC:
        return ui_color_control(h, (HDC)w);
    case WM_DRAWITEM:
        ui_pinta_boton((DRAWITEMSTRUCT *)l);
        return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}

static int ya_esta(void) {
    char txt[MAX_PATH], leido[256] = "";
    snprintf(txt, sizeof(txt), "%s\\datos\\juego.txt", g_exe_dir);
    FILE *f = fopen(txt, "r");
    if (f) { if (!fgets(leido, sizeof(leido), f)) leido[0] = 0; fclose(f); }
    size_t n = strlen(leido);
    while (n && (leido[n - 1] == '\n' || leido[n - 1] == '\r')) leido[--n] = 0;
    return existe(g_dll) && !strcmp(leido, g_huella);
}

int recompilar_si_hace_falta(const char *exe_dir, const char *elf, char *dll_out, size_t max) {
    snprintf(g_exe_dir, sizeof(g_exe_dir), "%s", exe_dir);
    snprintf(g_elf, sizeof(g_elf), "%s", elf);
    snprintf(g_dll, sizeof(g_dll), "%s\\datos\\juego.dll", exe_dir);
    snprintf(dll_out, max, "%s", g_dll);
    long long tam = 0;
    uint32_t crc = crc32_archivo(elf, &tam);
    char exe[MAX_PATH], *base;
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    base = strrchr(exe, '\\'); base = base ? base + 1 : exe;
    snprintf(g_huella, sizeof(g_huella), "v%d cpu%u elf %lld %08X exe %s proc %08X", RECOMP_VERSION, (unsigned)sizeof(vita_cpu_state), tam, crc, base,
             cpu_huella(NULL, 0));
    if (ya_esta()) return 0;
    if (getenv("MLB15_SIN_RECOMPILAR")) return existe(g_dll) ? 0 : -1;

    InitializeCriticalSection(&g_cs);
    g_t0 = GetTickCount();
    g_cancelar = g_fin = 0;
    estado("Empezando…", "", -1);

    int sin_ventana = getenv("MLB15_RECOMPILAR_SIN_VENTANA") != NULL;
    HANDLE th = CreateThread(NULL, 0, hilo_trabajo, NULL, 0, NULL);
    if (!sin_ventana) {
        ui_init();
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"MLB15Recompilar";
        RegisterClassExW(&wc);
        int W = ui_px(640), H = ui_px(380);
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        g_wnd = CreateWindowExW(0, wc.lpszClassName, L"Project Baseball — Preparando", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                                wa.left + (wa.right - wa.left - W) / 2, wa.top + (wa.bottom - wa.top - H) / 2, W, H, NULL, NULL, wc.hInstance, NULL);
        ui_ventana_oscura(g_wnd);
        RECT rc; GetClientRect(g_wnd, &rc);
        g_barra = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                  ui_px(28), ui_px(212), rc.right - ui_px(56), ui_px(14), g_wnd, (HMENU)ID_BARRA, NULL, NULL);
        ui_estilo_barra(g_barra);
        SendMessageW(g_barra, PBM_SETRANGE32, 0, 100);
        HWND bc = CreateWindowExW(0, L"BUTTON", L"Cancelar", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                  rc.right - ui_px(28 + 130), rc.bottom - ui_px(24 + 40), ui_px(130), ui_px(40), g_wnd, (HMENU)ID_CANCELAR, NULL, NULL);
        SendMessageW(bc, WM_SETFONT, (WPARAM)ui_fuente(UI_NORMAL), TRUE);
        SetTimer(g_wnd, 1, 1000, NULL);
        ShowWindow(g_wnd, SW_SHOW);
        PostMessageA(g_wnd, WM_APP, 0, 0);
        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            if (!IsDialogMessageW(g_wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            if (!IsWindow(g_wnd)) break;
        }
        g_wnd = NULL;
    }
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    if (g_resultado == 0) {
        char txt[MAX_PATH];
        snprintf(txt, sizeof(txt), "%s\\datos\\juego.txt", g_exe_dir);
        FILE *f = fopen(txt, "w");
        if (f) { fprintf(f, "%s\n", g_huella); fclose(f); }
        char t[16]; mmss(GetTickCount() - g_t0, t, sizeof(t));
        printf("[recompilar] juego.dll listo en %s\n", t);
        return 0;
    }
    if (!g_cancelar && !sin_ventana) {
        wchar_t w[1200];
        MultiByteToWideChar(CP_UTF8, 0, g_error, -1, w, 1024);
        wchar_t m[1400];
        swprintf(m, 1400, L"No se pudo preparar el juego:\n\n%ls\n\nVuelve a abrir Project Baseball para intentarlo otra vez.", w);
        MessageBoxW(NULL, m, L"Project Baseball", MB_OK | MB_ICONERROR);
    }
    return -1;
}

/* build.ps1 -Partes: el mismo proceso, sin ventana y siempre (datos\juego.dll junto al exe). */
int recompilar_desarrollo(const char *elf) {
    char exe[MAX_PATH], abs_elf[MAX_PATH], dll[MAX_PATH], txt[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    char *sl = strrchr(exe, '\\'); if (sl) *sl = 0;
    GetFullPathNameA(elf, sizeof(abs_elf), abs_elf, NULL);
    snprintf(txt, sizeof(txt), "%s\\datos", exe); CreateDirectoryA(txt, NULL);
    snprintf(txt, sizeof(txt), "%s\\datos\\juego.txt", exe); DeleteFileA(txt);
    _putenv_s("MLB15_RECOMPILAR_SIN_VENTANA", "1");
    return recompilar_si_hace_falta(exe, abs_elf, dll, sizeof(dll));
}
