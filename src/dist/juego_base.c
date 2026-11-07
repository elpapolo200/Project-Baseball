/* juego_base.c — lo mínimo que necesita juego.dll (el código del juego recompilado en el PC
 * del jugador) para funcionar sin la biblioteca de C de Microsoft: los registros de la CPU
 * emulada (uno por hilo), el directorio TLS que Windows usa para reservarlos, y memcpy/memset.
 * Se compila con clang junto a las partes generadas; no contiene nada del juego. */

#include <stddef.h>
#include <stdint.h>
#include "vitarecomp/cpu.h"

/* --- estado de la CPU por hilo (cpu.h, VITA_CPU propia) --- */
VITA_TLS vita_cpu_state vita_cpu;
__declspec(dllexport) vita_cpu_state *vita_cpu_tls(void) { return &vita_cpu; }

/* --- TLS implícito sin CRT: lo que normalmente pone tlssup.obj ---
 * El enlazador busca el símbolo _tls_used para rellenar el directorio TLS del DLL. Los datos
 * __declspec(thread) van a las secciones .tls$; _tls_start/_tls_end las delimitan. */
#pragma section(".tls", read, write)
#pragma section(".tls$ZZZ", read, write)
#pragma section(".CRT$XLA", read)
#pragma section(".CRT$XLZ", read)
__declspec(allocate(".tls")) char _tls_start = 0;
__declspec(allocate(".tls$ZZZ")) char _tls_end = 0;
typedef void (__stdcall *tls_cb)(void *, unsigned long, void *);
__declspec(allocate(".CRT$XLA")) const tls_cb __xl_a = 0;
__declspec(allocate(".CRT$XLZ")) const tls_cb __xl_z = 0;
unsigned long _tls_index = 0;
typedef struct {
    uint64_t start, end, index, callbacks;
    uint32_t zero_fill, characteristics;
} tls_dir;
const tls_dir _tls_used = {
    (uint64_t)&_tls_start, (uint64_t)&_tls_end, (uint64_t)&_tls_index, (uint64_t)(&__xl_a + 1), 0, 0
};

/* clang lo pide en cuanto hay coma flotante (convención de MSVC). */
int _fltused = 0x9875;

/* --- memcpy/memset/memmove: el compilador los genera para copias de estructuras ---
 * (este archivo se compila con -fno-builtin para que no se llamen a sí mismos) */
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *a = (unsigned char *)d; const unsigned char *b = (const unsigned char *)s;
    while (n >= 8) { uint64_t v; __builtin_memcpy_inline(&v, b, 8); __builtin_memcpy_inline(a, &v, 8); a += 8; b += 8; n -= 8; }
    while (n--) *a++ = *b++;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a = (unsigned char *)d; const unsigned char *b = (const unsigned char *)s;
    if (a == b || !n) return d;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *a = (unsigned char *)d;
    while (n--) *a++ = (unsigned char)c;
    return d;
}

/* Por si el compilador no los expande en línea (se compila con -msse4.1 y -fno-math-errno,
 * así que normalmente no hacen falta). */
double sqrt(double x) { return __builtin_sqrt(x); }
double trunc(double x) { return __builtin_trunc(x); }
float truncf(float x) { return __builtin_truncf(x); }

/* Punto de entrada del DLL: no hay nada que inicializar. */
int __stdcall DllMainCRTStartup(void *h, unsigned long motivo, void *r) { (void)h; (void)motivo; (void)r; return 1; }
