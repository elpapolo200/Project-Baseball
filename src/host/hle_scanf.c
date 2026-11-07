/* sscanf para el port de MLB 15 (SceLibc, variadica).
 *
 * El juego la usa al arrancar para leer identificadores en hexadecimal:
 *     "%2x:%2x:%2x:%2x:%2x:%2x"                       (MAC)
 *     "%2hhx%2hhx%2hhx%2hhx-..."                      (UUID)
 * y en otros sitios con %d / %f / %s.
 *
 * Convencion variadica de la ABI ARM (AAPCS): el 1.er argumento variable esta
 * en r2, el 2.0 en r3 y del 3.0 en adelante en la pila guest ([sp], [sp+4]...).
 * Por eso al entrar se guardan r2/r3 y se leen los siguientes de `sp`.
 *
 * Subconjunto implementado: espacios, literales, %% , %n, %c, %s, %[ (con
 * negacion y rangos), %d %i %u %o %x %X %p, %f %e %g, con ancho y modificadores
 * hh h l ll z. Es lo que usan los juegos; no pretende ser una libc completa.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"

typedef struct {
    const char *s;     /* cursor de entrada (host, ya validado) */
    uint32_t    args[16];
    int         nargs;
    int         assigned;
} Scan;

static const char *guest_str_ptr(uint32_t addr) {
    return (const char *)vita_mem_ptr(addr, 1);   /* el llamador valida el resto */
}

static uint32_t next_arg(Scan *sc) {
    if (sc->nargs >= 16) return 0;
    return sc->args[sc->nargs++];
}

static void skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r' ||
           **p == '\v' || **p == '\f') (*p)++;
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/* Copia como mucho `width` caracteres validos de `src` en `dst` y devuelve
 * cuantos se copiaron. Se usa para acotar strtoul/strtod y para %s. */
static int take(char *dst, size_t dstsz, const char *src, int width) {
    int n = 0;
    while (src[n] && n < width && (size_t)n + 1 < dstsz) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
    return n;
}

void vita_hle_sscanf(void) {
    uint32_t str_addr = r0, fmt_addr = r1;
    Scan sc;
    memset(&sc, 0, sizeof(sc));
    sc.args[0] = r2;
    sc.args[1] = r3;
    sc.nargs = 0;   /* el primer destino es r2 (antes empezaba en 2 y se saltaba r2 y r3: los dos
                     * primeros destinos iban a lo que hubiera en la pila; el creador de jugadores
                     * no leía ninguna región de la cara y la cabeza salía en blanco) */
    /* Del 3.er variable en adelante, en la pila guest. */
    for (int i = 2; i < 16; i++) sc.args[i] = vita_read32(sp + 4u * (uint32_t)(i - 2));

    const char *in = guest_str_ptr(str_addr);
    const char *fmt = guest_str_ptr(fmt_addr);
    if (!in || !fmt) { r0 = (uint32_t)-1; return; }
    sc.s = in;

    while (*fmt) {
        if (is_space(*fmt)) { skip_ws(&fmt); skip_ws(&sc.s); continue; }
        if (*fmt != '%') {
            if (*sc.s != *fmt) break;
            fmt++; sc.s++; continue;
        }
        fmt++;                                   /* '%' */

        int suppress = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt++ - '0'); }
        if (width <= 0) width = 0x7FFFFFFF;

        int len = 0;                             /* 0, 'h', 'H'(hh), 'l', 'L'(ll), 'z' */
        if (*fmt == 'h') { fmt++; if (*fmt == 'h') { fmt++; len = 'H'; } else len = 'h'; }
        else if (*fmt == 'l') { fmt++; if (*fmt == 'l') { fmt++; len = 'L'; } else len = 'l'; }
        else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { fmt++; len = 'z'; }

        char conv = *fmt;
        if (!conv) break;
        fmt++;

        uint32_t dst = suppress ? 0 : next_arg(&sc);

        if (conv == 'n') {
            if (dst && vita_mem_ptr(dst, 4)) vita_write32(dst, (uint32_t)(sc.s - in));
            continue;
        }
        if (conv == '%') {
            if (*sc.s == '%') sc.s++; else break;
            continue;
        }
        if (conv == 'c') {
            int n = (width == 0x7FFFFFFF) ? 1 : width;
            if (dst) {
                char *p = (char *)vita_mem_ptr(dst, (uint32_t)n);
                if (!p) break;
                for (int i = 0; i < n; i++) {
                    if (!sc.s[i]) { n = i; break; }
                    p[i] = sc.s[i];
                }
                sc.s += n;
            } else sc.s += n;
            sc.assigned++;
            continue;
        }
        if (conv == 's' || conv == '[') {
            skip_ws(&sc.s);
            int cap = width;
            int n = 0;
            if (conv == 's') {
                while (sc.s[n] && !is_space(sc.s[n]) && n < cap) n++;
            } else {
                /* %[...]: conjunto hasta ']' (con ^ inicial). */
                int neg = 0;
                const char *set_start = fmt;
                if (*fmt == '^') { neg = 1; fmt++; }
                while (*fmt && *fmt != ']') fmt++;
                size_t set_len = (size_t)(fmt - set_start);
                if (*fmt == ']') fmt++;
                while (sc.s[n] && n < cap) {
                    char c = sc.s[n];
                    int member = 0;
                    for (size_t k = (neg ? 1u : 0u); k < set_len; k++) {
                        if (k + 2 < set_len && set_start[k + 1] == '-' && set_start[k + 2] != ']') {
                            if ((unsigned char)c >= (unsigned char)set_start[k] &&
                                (unsigned char)c <= (unsigned char)set_start[k + 2]) { member = 1; k += 2; }
                        } else if (c == set_start[k]) member = 1;
                    }
                    if (member == neg) break;
                    n++;
                }
            }
            if (n == 0) break;
            /* Solo los bytes que se escriben: antes se pedían 4097 de golpe y, con el búfer en
             * la pila cerca de su final, vita_mem_ptr fallaba y no se copiaba nada (el creador
             * de jugadores leía " %s %s " vacío: ninguna región de la cara, cabeza en blanco). */
            if (dst && !suppress) {
                char *p = (char *)vita_mem_ptr(dst, (uint32_t)n + 1);
                if (!p) break;
                memcpy(p, sc.s, (size_t)n); p[n] = 0;
            }
            sc.s += n;
            sc.assigned++;
            continue;
        }

        /* Conversiones numericas: primero se salta espacio. */
        skip_ws(&sc.s);
        char tmp[128];
        int n = take(tmp, sizeof(tmp), sc.s, width);
        if (n == 0) break;

        char *end = NULL;
        if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G') {
            double d = strtod(tmp, &end);
            if (end == tmp) break;
            if (dst && !suppress) {
                if (len == 'l' || len == 'L') {
                    if (!vita_mem_ptr(dst, 8)) break;
                    memcpy(vita_mem_ptr(dst, 8), &d, 8);
                } else {
                    float f = (float)d;
                    if (!vita_mem_ptr(dst, 4)) break;
                    memcpy(vita_mem_ptr(dst, 4), &f, 4);
                }
            }
            sc.s += (end - tmp);
            sc.assigned++;
            continue;
        }

        int base = 10;
        if (conv == 'x' || conv == 'X' || conv == 'p') base = 16;
        else if (conv == 'o') base = 8;
        else if (conv == 'i') base = 0;
        unsigned long long v = strtoull(tmp, &end, base);
        if (end == tmp) break;
        if (dst && !suppress) {
            if (len == 'H') { if (!vita_mem_ptr(dst, 1)) break; *(uint8_t *)vita_mem_ptr(dst, 1) = (uint8_t)v; }
            else if (len == 'h') { if (!vita_mem_ptr(dst, 2)) break; *(uint16_t *)vita_mem_ptr(dst, 2) = (uint16_t)v; }
            else { if (!vita_mem_ptr(dst, 4)) break; vita_write32(dst, (uint32_t)v); }
        }
        sc.s += (end - tmp);
        sc.assigned++;
    }

    {   /* MLB15_SCANF_TRAZA=texto: llamadas cuyo formato contiene ese texto */
        static const char *tr = (const char *)1;
        static int n;
        if (tr == (const char *)1) tr = getenv("MLB15_SCANF_TRAZA");
        const char *f0 = guest_str_ptr(fmt_addr);
        if (tr && f0 && strstr(f0, tr) && n++ < 40)
            printf("[sscanf] '%.60s' con '%s' -> %d (r2 %08X '%.20s' r3 %08X '%.20s' | r4 %08X r7 %08X)\n", in, f0, sc.assigned,
                   sc.args[0], (const char *)vita_mem_ptr(sc.args[0], 1), sc.args[1], (const char *)vita_mem_ptr(sc.args[1], 1), r4, r7);
    }
    r0 = (uint32_t)sc.assigned;
}
