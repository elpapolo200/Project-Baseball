/* Funciones de libc que el runtime de vitarecomp no trae y MLB 15 importa.
 *
 * printf y familia leen los argumentos variables como lo haría el código ARM
 * (AAPCS): r0-r3 y después la pila guest; los de 64 bits (double, long long)
 * van alineados a pareja de registros / a 8 bytes. Las versiones v* reciben un
 * va_list, que en la Vita es un puntero a la zona de argumentos en la pila.
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/dispatch.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/recomp_rt.h"

/* chivato para trazas del port: 1 cuando el juego está en el partido */
int port_en_partido;
int port_juego_flow;

#include "port.h"

/* ------------------------------------------------------------------------- */
/* Lectura de argumentos variables                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t regs[4];
    int reg;          /* siguiente registro libre (0-3); 4 = ya en la pila */
    uint32_t stack;   /* siguiente palabra en la pila guest */
    int is_va;        /* 1: leer de un va_list */
    uint32_t va;
} Args;

static void args_regs(Args *a, int first) {
    a->regs[0] = r0; a->regs[1] = r1; a->regs[2] = r2; a->regs[3] = r3;
    a->reg = first;
    a->stack = sp;
    a->is_va = 0;
}

static void args_va(Args *a, uint32_t va) { memset(a, 0, sizeof(*a)); a->is_va = 1; a->va = va; }

static uint32_t next32(Args *a) {
    if (a->is_va) { uint32_t v = vita_read32(a->va); a->va += 4; return v; }
    if (a->reg < 4) return a->regs[a->reg++];
    uint32_t v = vita_read32(a->stack);
    a->stack += 4;
    return v;
}

static uint64_t next64(Args *a) {
    uint32_t lo, hi;
    if (a->is_va) {
        a->va = (a->va + 7u) & ~7u;
        lo = vita_read32(a->va); hi = vita_read32(a->va + 4); a->va += 8;
    } else if (a->reg < 4) {
        if (a->reg & 1) a->reg++;
        if (a->reg <= 2) { lo = a->regs[a->reg]; hi = a->regs[a->reg + 1]; a->reg += 2; }
        else { a->reg = 4; a->stack = (a->stack + 7u) & ~7u; lo = vita_read32(a->stack); hi = vita_read32(a->stack + 4); a->stack += 8; }
    } else {
        a->stack = (a->stack + 7u) & ~7u;
        lo = vita_read32(a->stack); hi = vita_read32(a->stack + 4); a->stack += 8;
    }
    return ((uint64_t)hi << 32) | lo;
}

/* ------------------------------------------------------------------------- */
/* Formateador                                                                */
/* ------------------------------------------------------------------------- */

typedef struct { char *buf; size_t cap, len; } Out;

static void out_put(Out *o, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (o->len + 1 < o->cap) o->buf[o->len] = s[i];
        o->len++;
    }
}

static int format_guest(Out *o, uint32_t fmt_addr, Args *a) {
    char fmt[2048];
    port_guest_str(fmt_addr, fmt, sizeof(fmt));
    for (const char *p = fmt; *p; ) {
        if (*p != '%') { const char *q = strchr(p, '%'); size_t n = q ? (size_t)(q - p) : strlen(p); out_put(o, p, n); p += n; continue; }
        if (p[1] == '%') { out_put(o, "%", 1); p += 2; continue; }
        char spec[32] = "%";
        size_t sl = 1;
        const char *q = p + 1;
        while (*q && strchr("-+ #0", *q) && sl < 20) spec[sl++] = *q++;
        if (*q == '*') { sl += (size_t)snprintf(spec + sl, sizeof(spec) - sl, "%d", (int)next32(a)); q++; }
        while (isdigit((unsigned char)*q) && sl < 24) spec[sl++] = *q++;
        if (*q == '.') {
            spec[sl++] = *q++;
            if (*q == '*') { sl += (size_t)snprintf(spec + sl, sizeof(spec) - sl, "%d", (int)next32(a)); q++; }
            while (isdigit((unsigned char)*q) && sl < 28) spec[sl++] = *q++;
        }
        int lng = 0; /* 0 normal, 2 = long long */
        while (*q && strchr("hlLqjzt", *q)) {
            if (*q == 'l' && q[1] == 'l') { lng = 2; q += 2; continue; }
            if (*q == 'q' || *q == 'j' || *q == 'L') lng = 2;
            q++;
        }
        char conv = *q ? *q++ : 0;
        char tmp[512];
        int n = 0;
        spec[sl] = 0;
        switch (conv) {
        case 'd': case 'i':
            if (lng == 2) { strcat(spec, "lld"); n = snprintf(tmp, sizeof(tmp), spec, (long long)next64(a)); }
            else { spec[sl] = 'd'; spec[sl + 1] = 0; n = snprintf(tmp, sizeof(tmp), spec, (int32_t)next32(a)); }
            break;
        case 'u': case 'x': case 'X': case 'o':
            if (lng == 2) { spec[sl] = 'l'; spec[sl + 1] = 'l'; spec[sl + 2] = conv; spec[sl + 3] = 0; n = snprintf(tmp, sizeof(tmp), spec, (unsigned long long)next64(a)); }
            else { spec[sl] = conv; spec[sl + 1] = 0; n = snprintf(tmp, sizeof(tmp), spec, next32(a)); }
            break;
        case 'c': spec[sl] = 'c'; spec[sl + 1] = 0; n = snprintf(tmp, sizeof(tmp), spec, (int)next32(a)); break;
        case 'p': n = snprintf(tmp, sizeof(tmp), "0x%08X", next32(a)); break;
        case 's': {
            char s[1024];
            uint32_t addr = next32(a);
            if (addr) port_guest_str(addr, s, sizeof(s)); else strcpy(s, "(null)");
            spec[sl] = 's'; spec[sl + 1] = 0;
            n = snprintf(tmp, sizeof(tmp), spec, s);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            uint64_t bits = next64(a);
            double d;
            memcpy(&d, &bits, 8);
            spec[sl] = conv; spec[sl + 1] = 0;
            n = snprintf(tmp, sizeof(tmp), spec, d);
            break;
        }
        case 'n': next32(a); break;
        default: tmp[0] = '%'; tmp[1] = conv; n = 2; break;
        }
        if (n > 0) out_put(o, tmp, (size_t)(n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1));
        p = q;
    }
    if (o->cap) o->buf[o->len < o->cap ? o->len : o->cap - 1] = 0;
    return (int)o->len;
}

static int format_to_guest(uint32_t dst, uint32_t cap, uint32_t fmt, Args *a) {
    char buf[8192];
    Out o = { buf, sizeof(buf), 0 };
    int n = format_guest(&o, fmt, a);
    size_t copy = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
    if (cap) {
        if (copy >= cap) copy = cap - 1;
        if (dst) { memcpy(vita_mem_ptr(dst, (uint32_t)copy + 1), buf, copy); vita_write8(dst + (uint32_t)copy, 0); }
    }
    return n;
}

static void format_to_stdout(uint32_t fmt, Args *a) {
    char buf[8192];
    Out o = { buf, sizeof(buf), 0 };
    int n = format_guest(&o, fmt, a);
    {   /* MLB15_JUEGO_HORA=1: cada línea del juego con ms desde el arranque y nº de fotograma */
        static int hora = -1;
        static ULONGLONG t0;
        if (hora < 0) { hora = getenv("MLB15_JUEGO_HORA") ? 1 : 0; t0 = GetTickCount64(); }
        extern unsigned port_display_frames(void);
        if (hora) printf("[%llu ms f%u] ", (unsigned long long)(GetTickCount64() - t0), port_display_frames());
    }
    printf("[juego] %.*s", n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1, buf);
    if (n > 0 && buf[(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1) - 1] != '\n') printf("\n");
    fflush(stdout);
    /* chivato para trazas del port: 1 desde que el juego entra al partido */
    if (n > 0 && (strstr(buf, "GAME_STATE") || strstr(buf, "FLOW STATE")))
        port_juego_flow = buf[0] == 'F' || strstr(buf, "ENTER") ? 1 : port_juego_flow;
    if (strstr(buf, "LAUNCH_PITCH")) port_en_partido = 1;
    r0 = (uint32_t)n;
}

void vita_hle_printf(void)    { Args a; uint32_t f = r0; args_regs(&a, 1); format_to_stdout(f, &a); }
void vita_hle_vprintf(void)   { Args a; uint32_t f = r0; args_va(&a, r1); format_to_stdout(f, &a); }
void vita_hle_sprintf(void)   { Args a; uint32_t d = r0, f = r1; args_regs(&a, 2); r0 = (uint32_t)format_to_guest(d, 0xFFFFFFFFu, f, &a); }
void vita_hle_snprintf(void)  { Args a; uint32_t d = r0, c = r1, f = r2; args_regs(&a, 3); r0 = (uint32_t)format_to_guest(d, c, f, &a); }
void vita_hle_vsprintf(void)  { Args a; uint32_t d = r0, f = r1; args_va(&a, r2); r0 = (uint32_t)format_to_guest(d, 0xFFFFFFFFu, f, &a); }
void vita_hle_vsnprintf(void) { Args a; uint32_t d = r0, c = r1, f = r2; args_va(&a, r3); r0 = (uint32_t)format_to_guest(d, c, f, &a); }

void vita_hle_sceClibPrintf(void) { vita_hle_printf(); }

/* ------------------------------------------------------------------------- */
/* stdio sobre el sistema de archivos virtual                                 */
/* ------------------------------------------------------------------------- */

#define FILE_TOKEN 0xF1000000u
#define MAX_FILES 256
static FILE *g_files[MAX_FILES];
static CRITICAL_SECTION g_files_cs;
static int g_files_init;

/* Direcciones guest de las variables importadas _Stdout/_Stderr (import_vars.c). */
static uint32_t g_stdout_addr, g_stderr_addr;
void port_stdio_alias(uint32_t out, uint32_t err) { g_stdout_addr = out; g_stderr_addr = err; }

static FILE *file_of(uint32_t token) {
    if (token && token == g_stdout_addr) return stdout;
    if (token && token == g_stderr_addr) return stderr;
    uint32_t i = token - FILE_TOKEN;
    return i < MAX_FILES ? g_files[i] : NULL;
}

void vita_hle_fopen(void) {
    char guest[512], host[1024], mode[16];
    port_guest_str(r0, guest, sizeof(guest));
    port_guest_str(r1, mode, sizeof(mode));
    if (!g_files_init) { InitializeCriticalSection(&g_files_cs); g_files_init = 1; }
    if (!port_vfs_path(guest, host, sizeof(host))) { r0 = 0; return; }
    if (strchr(mode, 'w') || strchr(mode, 'a')) port_vfs_mkdirs(host);
    FILE *fp = fopen(host, mode);
    printf("[io] fopen('%s', '%s') -> %s\n", guest, mode, fp ? "ok" : "no existe");
    if (!fp) { r0 = 0; return; }
    EnterCriticalSection(&g_files_cs);
    uint32_t tok = 0;
    for (int i = 1; i < MAX_FILES; i++) if (!g_files[i]) { g_files[i] = fp; tok = FILE_TOKEN + (uint32_t)i; break; }
    LeaveCriticalSection(&g_files_cs);
    if (!tok) fclose(fp);
    r0 = tok;
}

void vita_hle_fclose(void) {
    FILE *fp = file_of(r0);
    if (fp) { fclose(fp); g_files[r0 - FILE_TOKEN] = NULL; }
    r0 = 0;
}

void vita_hle_fread(void) {
    FILE *fp = file_of(r3);
    uint32_t total = r1 * r2;
    if (!fp || !total) { r0 = 0; return; }
    size_t got = fread(vita_mem_ptr(r0, total), 1, total, fp);
    r0 = r1 ? (uint32_t)(got / r1) : 0;
}

void vita_hle_fwrite(void) {
    FILE *fp = file_of(r3);
    uint32_t total = r1 * r2;
    if (!total) { r0 = 0; return; }
    if (!fp) {
        /* FILE desconocido (p. ej. un fopen del juego que falló): antes iba todo a stdout, y en el
         * otro PC del usuario datos\log.txt se llenó de cientos de MB de datos binarios (índices de
         * mallas) sin una línea de texto. Ahora solo pasa el texto; lo binario se cuenta y se tira. */
        const unsigned char *p = (const unsigned char *)vita_mem_ptr(r0, total);
        uint32_t raros = 0;
        for (uint32_t i = 0; p && i < total && i < 4096; i++)
            if (p[i] < 9 || (p[i] > 13 && p[i] < 32)) raros++;
        if (p && raros == 0) fwrite(p, 1, total, stdout);
        else {
            static unsigned avisos; static unsigned long long bytes;
            bytes += total;
            if (avisos++ < 5 || (avisos & 1023) == 0)
            {   printf("[io] fwrite de %u bytes binarios a un FILE desconocido 0x%08X (descartado; %llu bytes en total)\n", total, r3, bytes);
                extern void arranque(const char *fmt, ...);
                arranque("aviso: el juego escribe %u bytes binarios a un archivo que no abrió (0x%08X, %llu bytes en total)", total, r3, bytes); }
        }
        r0 = r2;
        return;
    }
    size_t put = fwrite(vita_mem_ptr(r0, total), 1, total, fp);
    r0 = r1 ? (uint32_t)(put / r1) : 0;
}

void vita_hle_fseek(void) { FILE *fp = file_of(r0); r0 = fp ? (uint32_t)fseek(fp, (long)(int32_t)r1, (int)r2) : 0xFFFFFFFFu; }
void vita_hle_ftell(void) { FILE *fp = file_of(r0); r0 = fp ? (uint32_t)ftell(fp) : 0xFFFFFFFFu; }

void vita_hle_fputs(void) {
    char s[4096];
    port_guest_str(r0, s, sizeof(s));
    FILE *fp = file_of(r1);
    if (fp) fputs(s, fp); else printf("[juego] %s", s);
    r0 = 0;
}

void vita_hle_fprintf(void) {
    uint32_t tok = r0, f = r1;
    Args a;
    args_regs(&a, 2);
    FILE *fp = file_of(tok);
    if (!fp) { format_to_stdout(f, &a); return; }
    char buf[8192];
    Out o = { buf, sizeof(buf), 0 };
    int n = format_guest(&o, f, &a);
    fwrite(buf, 1, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1), fp);
    r0 = (uint32_t)n;
}

/* ------------------------------------------------------------------------- */
/* Cadenas                                                                    */
/* ------------------------------------------------------------------------- */

static uint32_t gstr_len(uint32_t a) { uint32_t n = 0; while (vita_read8(a + n)) n++; return n; }
static char *hp(uint32_t a) { return (char *)vita_mem_ptr(a, 1); }

void vita_hle_strcasecmp(void)  {
    static const char *tr = (const char *)1;   /* MLB15_STRCMP_TRAZA=texto */
    static int n;
    if (tr == (const char *)1) tr = getenv("MLB15_STRCMP_TRAZA");
    int v = _stricmp(hp(r0), hp(r1));
    if (tr && (!strcmp(hp(r0), tr) || !strcmp(hp(r1), tr)) && n++ < 20000) {
        uint32_t obj = vita_read32(sp + 0x584u);   /* (pruebas del creador de caras) */
        printf("[strcasecmp] '%.40s' '%.40s' -> %d | r0 %08X sp %08X | obj %08X +54 %08X +58 %08X | 588 %08X\n", hp(r0), hp(r1), v, r0, sp, obj,
               obj && vita_mem_ptr(obj, 0x60) ? vita_read32(obj + 0x54) : 0, obj && vita_mem_ptr(obj, 0x60) ? vita_read32(obj + 0x58) : 0,
               vita_read32(sp + 0x588u));
    }
    r0 = (uint32_t)v;
}
void vita_hle_strncasecmp(void) { r0 = (uint32_t)_strnicmp(hp(r0), hp(r1), r2); }
void vita_hle_strcspn(void)     { r0 = (uint32_t)strcspn(hp(r0), hp(r1)); }

void vita_hle_strncat(void) {
    uint32_t dst = r0, src = r1, n = r2, dl = gstr_len(dst), i = 0;
    for (; i < n; i++) { uint8_t c = vita_read8(src + i); if (!c) break; vita_write8(dst + dl + i, c); }
    vita_write8(dst + dl + i, 0);
    r0 = dst;
}

/* Cadenas anchas: en la Vita wchar_t es de 16 bits (el juego reserva 0x202 bytes para
 * 0x101 caracteres). mbstowcs faltaba y cerraba el port al confirmar el nombre en Crear
 * jugador (Road to the Show). El texto multibyte se toma como UTF-8. */
void vita_hle_mbstowcs(void) {
    uint32_t dst = r0, src = r1, n = r2, i = 0, out = 0;
    for (;;) {
        uint32_t c = vita_read8(src + i);
        if (!c) break;
        uint32_t cp = c, extra = 0;
        if (c >= 0xF0) { cp = c & 7; extra = 3; } else if (c >= 0xE0) { cp = c & 15; extra = 2; } else if (c >= 0xC0) { cp = c & 31; extra = 1; }
        else if (c >= 0x80) { r0 = (uint32_t)-1; return; }
        for (uint32_t k = 1; k <= extra; k++) {
            uint32_t cc = vita_read8(src + i + k);
            if ((cc & 0xC0) != 0x80) { r0 = (uint32_t)-1; return; }
            cp = cp << 6 | (cc & 63);
        }
        i += 1 + extra;
        if (cp > 0xFFFF) cp = '?';
        if (dst) {
            if (out >= n) break;
            vita_write16(dst + 2 * out, (uint16_t)cp);
        }
        out++;
    }
    if (dst && out < n) vita_write16(dst + 2 * out, 0);
    r0 = out;
}
/* int _Wctob(wint_t c): carácter ancho a byte (EOF si no cabe en uno). */
void vita_hle__Wctob(void) { uint32_t c = r0 & 0xFFFF; r0 = c < 0x80 ? c : (uint32_t)-1; }
/* wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n) */
void vita_hle_wmemset(void) {
    uint32_t s = r0, c = r1 & 0xFFFF, n = r2;
    for (uint32_t i = 0; i < n; i++) vita_write16(s + 2 * i, (uint16_t)c);
    r0 = s;
}
void vita_hle_wcslen(void) {
    uint32_t s = r0, n = 0;
    while (s && vita_read16(s + 2 * n)) n++;
    r0 = n;
}

static VITA_TLS uint32_t t_strtok_next;
void vita_hle_strtok(void) {
    uint32_t s = r0 ? r0 : t_strtok_next, delim = r1;
    if (!s) { r0 = 0; return; }
    const char *d = hp(delim);
    while (vita_read8(s) && strchr(d, vita_read8(s))) s++;
    if (!vita_read8(s)) { t_strtok_next = 0; r0 = 0; return; }
    uint32_t tok = s;
    while (vita_read8(s) && !strchr(d, vita_read8(s))) s++;
    if (vita_read8(s)) { vita_write8(s, 0); t_strtok_next = s + 1; } else t_strtok_next = 0;
    r0 = tok;
    {   /* MLB15_STRTOK_TRAZA=texto: tokens que contienen ese texto */
        static const char *tr = (const char *)1;
        static int n;
        if (tr == (const char *)1) tr = getenv("MLB15_STRTOK_TRAZA");
        if (tr && strstr(hp(tok), tr) && n++ < 40) printf("[strtok] '%.60s'\n", hp(tok));
    }
}

/* ------------------------------------------------------------------------- */
/* Números y miscelánea                                                       */
/* ------------------------------------------------------------------------- */

/* double _Stod(const char *s, char **end, long pten) -> r0:r1 */
void vita_hle__Stod(void) {
    char s[256];
    uint32_t src = r0, endp = r1;
    port_guest_str(src, s, sizeof(s));
    char *e;
    double d = strtod(s, &e);
    if (endp) vita_write32(endp, src + (uint32_t)(e - s));
    uint64_t bits;
    memcpy(&bits, &d, 8);
    r0 = (uint32_t)bits;
    r1 = (uint32_t)(bits >> 32);
}

/* unsigned long _Stoul(const char *s, char **end, int base) */
void vita_hle__Stoul(void) {
    char s[256];
    uint32_t src = r0, endp = r1;
    port_guest_str(src, s, sizeof(s));
    char *e;
    unsigned long v = strtoul(s, &e, (int)r2);
    if (endp) vita_write32(endp, src + (uint32_t)(e - s));
    r0 = (uint32_t)v;
}

void vita_hle_rand(void) { r0 = (uint32_t)rand(); }
void vita_hle_time(void) {
    uint32_t t = (uint32_t)time(NULL);
    if (r0) vita_write32(r0, t);
    r0 = t;
}

void vita_hle_abort(void) { printf("[juego] abort()\n"); fflush(stdout); vita_trap_unimpl(0, 0, "abort"); }
void vita_hle_exit(void)  { printf("[juego] exit(%d)\n", (int)r0); fflush(stdout); ExitProcess(r0); }
void vita_hle__Assert(void) {
    char msg[512];
    port_guest_str(r0, msg, sizeof(msg));
    printf("[juego] ASSERT: %s\n", msg);
    fflush(stdout);
    vita_trap_unimpl(0, 0, "_Assert");
}

/* qsort/bsearch: la comparación es una función guest (r0, r1 -> r0). */
static int guest_cmp(uint32_t fn, uint32_t a, uint32_t b) {
    r0 = a;
    r1 = b;
    vita_dispatch(0, fn);
    return (int)r0;
}

void vita_hle_qsort(void) {
    uint32_t base = r0, n = r1, size = r2, cmp = r3;
    uint32_t saved_sp = sp;
    if (n < 2 || !size) return;
    uint8_t *tmp = (uint8_t *)malloc(size);
    /* ordenación por inserción con búsqueda binaria: estable y sin recursión */
    for (uint32_t i = 1; i < n; i++) {
        uint32_t lo = 0, hi = i;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (guest_cmp(cmp, base + i * size, base + mid * size) < 0) hi = mid; else lo = mid + 1;
            sp = saved_sp;
        }
        if (lo == i) continue;
        memcpy(tmp, vita_mem_ptr(base + i * size, size), size);
        memmove(vita_mem_ptr(base + (lo + 1) * size, (i - lo) * size), vita_mem_ptr(base + lo * size, (i - lo) * size), (i - lo) * size);
        memcpy(vita_mem_ptr(base + lo * size, size), tmp, size);
    }
    free(tmp);
}

void vita_hle_bsearch(void) {
    uint32_t key = r0, base = r1, n = r2, size = r3, cmp = vita_read32(sp);
    uint32_t saved_sp = sp, lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = guest_cmp(cmp, key, base + mid * size);
        sp = saved_sp;
        if (c == 0) { r0 = base + mid * size; return; }
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    r0 = 0;
}
