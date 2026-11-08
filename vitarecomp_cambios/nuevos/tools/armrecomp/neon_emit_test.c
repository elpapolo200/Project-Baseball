/* neon_emit_test.c — generates a C test that checks neon_emit.h against the runtime (neon.c).
 *
 *   neon_emit_test gen.c [count]
 *   cl /O2 gen.c vitarecomp.lib   (or clang with -ffp-contract=off, like the game DLL)
 *   gen.exe                       -> "neon_emit: N casos, 0 distintos"
 *
 * For each case: a random encoding of a form neon_emit_inline covers (random registers,
 * D/Q, operation), a function with the emitted C, and main() runs it and neon_dp on the
 * same random register file (normal values, negatives, zeros, infinities, NaNs) and
 * compares all 64 words bit for bit. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "neon_emit.h"

static uint32_t rnd(void) { static uint32_t s = 0x12345678u; s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

/* Thumb raw from an A32-layout word: U (bit 24) goes to bit 28, first halfword 0xEF/0xFF. */
static uint32_t thumb(uint32_t a) { return 0xEF000000u | (((a >> 24) & 1) << 28) | (a & 0x00FFFFFFu); }

static uint32_t regs(uint32_t a, int q, int scalar) {
    int d = (int)(rnd() & 31), n = (int)(rnd() & 31), m = (int)(rnd() & 31);
    if (q) { d &= ~1; n &= ~1; m &= ~1; }
    if (rnd() % 4 == 0) d = n;                        /* destination = source */
    a |= (uint32_t)(d >> 4) << 22 | (uint32_t)(d & 15) << 12;
    a |= (uint32_t)(n >> 4) << 7 | (uint32_t)(n & 15) << 16;
    if (scalar) a |= (uint32_t)(rnd() & 15) | (rnd() & 1) << 5;   /* Dm 0-15, index */
    else a |= (uint32_t)(m >> 4) << 5 | (uint32_t)(m & 15);
    return a;
}

static uint32_t caso(void) {
    for (;;) {
        int q = (int)(rnd() & 1);
        uint32_t a;
        switch (rnd() % 5) {
            case 0:  /* logic: opc 1, B, U, size */
                a = (1u << 8) | (1u << 4) | (rnd() & 1) << 24 | (rnd() & 3) << 20 | (uint32_t)q << 6;
                return regs(a, q, 0);
            case 1:  /* float add/sub, mla/mls, mul */
                a = (13u << 8) | (rnd() & 1) << 4 | (rnd() & 1) << 24 | (rnd() & 1) << 21 | (uint32_t)q << 6;
                break;
            case 2:  /* float compares */
                a = (14u << 8) | (rnd() & 1) << 24 | (rnd() & 1) << 21 | (uint32_t)q << 6;
                break;
            default: { /* by scalar: bit 23, size 2, bit 6, opc 1/5/9, bit 24 = Q */
                static const uint32_t ops[3] = { 1, 5, 9 };
                a = (1u << 23) | (2u << 20) | (1u << 6) | ops[rnd() % 3] << 8 | (uint32_t)q << 24;
                return regs(a, q, 1);
            }
        }
        a = regs(a, q, 0);
        char buf[4096];
        FILE *t = tmpfile();
        if (!t) continue;
        int ok = neon_emit_inline(t, thumb(a));
        fclose(t);
        (void)buf;
        if (ok) return a;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: neon_emit_test gen.c [casos]\n"); return 2; }
    int n = argc > 2 ? atoi(argv[2]) : 2000;
    FILE *f = fopen(argv[1], "w");
    if (!f) return 1;
    fprintf(f, "#include <stdio.h>\n#include <stdint.h>\n#include <string.h>\n#include <math.h>\n"
               "#include \"vitarecomp/cpu.h\"\n#include \"vitarecomp/vfp.h\"\n#include \"vitarecomp/neon.h\"\n\n");
    uint32_t *raws = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)n);
    for (int i = 0; i < n; i++) {
        raws[i] = thumb(caso());
        fprintf(f, "static void t%d(void) {\n", i);
        neon_emit_inline(f, raws[i]);
        fprintf(f, "}\n");
    }
    fprintf(f, "static const uint32_t raws[%d] = {", n);
    for (int i = 0; i < n; i++) fprintf(f, "%s0x%08Xu", i ? ", " : "", raws[i]);
    fprintf(f, "};\nstatic void (*const fns[%d])(void) = {", n);
    for (int i = 0; i < n; i++) fprintf(f, "%st%d", i ? ", " : "", i);
    fprintf(f, "};\n"
        "static uint32_t s = 0x9E3779B9u;\n"
        "static uint32_t r32(void) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }\n"
        "static uint32_t valor(void) {\n"
        "    switch (r32() %% 10) {\n"
        "        case 0: return 0x7FC00000u; case 1: return 0x7F800000u; case 2: return 0xFF800000u;\n"
        "        case 3: return 0u; case 4: return 0x80000000u; case 5: return r32();\n"
        "        default: { float x = ((float)(r32() %% 20001) - 10000.0f) / 37.0f; uint32_t u; memcpy(&u, &x, 4); return u; }\n"
        "    }\n"
        "}\n"
        "int main(void) {\n"
        "    int malos = 0;\n"
        "    for (int i = 0; i < %d; i++) for (int rep = 0; rep < 8; rep++) {\n"
        "        uint32_t ini[64], a[64], b[64];\n"
        "        for (int k = 0; k < 64; k++) ini[k] = valor();\n"
        "        for (int k = 0; k < 64; k++) vfp_s[k].u = ini[k];\n"
        "        fns[i]();\n"
        "        for (int k = 0; k < 64; k++) { a[k] = vfp_s[k].u; vfp_s[k].u = ini[k]; }\n"
        "        neon_dp(0, raws[i]);\n"
        "        for (int k = 0; k < 64; k++) b[k] = vfp_s[k].u;\n"
        "        for (int k = 0; k < 64; k++) if (a[k] != b[k] && !(((a[k] & 0x7F800000u) == 0x7F800000u && (a[k] & 0x7FFFFFu)) && ((b[k] & 0x7F800000u) == 0x7F800000u && (b[k] & 0x7FFFFFu)))) {\n"
        "            if (malos < 10) printf(\"distinto: caso %%d raw %%08X palabra %%d: en linea %%08X runtime %%08X\\n\", i, raws[i], k, a[k], b[k]);\n"
        "            malos++; break;\n"
        "        }\n"
        "    }\n"
        "    printf(\"neon_emit: %d casos x 8 registros, %%d distintos\\n\", malos);\n"
        "    return malos != 0;\n"
        "}\n", n, n);
    fclose(f);
    free(raws);
    return 0;
}
