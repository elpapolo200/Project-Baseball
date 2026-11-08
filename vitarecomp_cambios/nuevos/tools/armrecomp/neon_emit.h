/* neon_emit.h — the busiest NEON forms written out as C at translation time.
 *
 * MLB 15 port, 2026-11-08 (PLAN_RENDIMIENTO.md fase 3). The runtime executes NEON from its
 * encoding (neon.c): every execution decodes the instruction again, copies whole registers
 * into temporaries and loops over lanes through switches. Measured in a match: ~8.7 million
 * NEON instructions per second, and a handful of forms are ~85 % of them. For those forms the
 * emitter writes the operation directly on vfp_s[] (D k = vfp_s[2k], vfp_s[2k+1]); the
 * compiler turns it into a few SSE instructions. Anything else still calls neon_dp.
 *
 * The C here must give bit-for-bit what neon.c gives: same reads-before-writes (sources into
 * locals first, so vadd q0, q0, q1 is safe), same float expressions (a + b * c with two
 * roundings: the game DLL is built with -ffp-contract=off), same lane selection for Q
 * registers (d & ~1). tools/armrecomp/neon_emit_test.c checks it against neon.c.
 *
 * Covered:
 *   three registers, same length (D and Q):
 *     VADD/VSUB.F32, VMLA/VMLS.F32, VMUL.F32, VCEQ/VCGE/VCGT.F32,
 *     VAND/VBIC/VORR/VORN/VEOR/VBSL/VBIT/VBIF
 *   by scalar, F32 (D and Q): VMLA, VMLS, VMUL
 */
#ifndef ARMRECOMP_NEON_EMIT_H
#define ARMRECOMP_NEON_EMIT_H

#include <stdint.h>
#include <stdio.h>

#define NE_BIT(a, n) (((a) >> (n)) & 1u)

/* Index into vfp_s of the first word of register `reg` (a D number), D or Q. */
static int ne_base(int reg, int q) { return 2 * (q ? (reg & ~1) : reg); }

static int neon_emit_inline(FILE *f, uint32_t raw) {
    uint32_t a = (((raw >> 28) & 1) << 24) | (raw & 0x00FFFFFFu);   /* A32 layout, as neon_dp */
    int d = (int)(NE_BIT(a, 22) << 4 | ((a >> 12) & 15));
    int n = (int)(NE_BIT(a, 7) << 4 | ((a >> 16) & 15));
    if (!NE_BIT(a, 23)) {                                           /* dp_3same */
        int opc = (a >> 8) & 15, B = NE_BIT(a, 4), U = NE_BIT(a, 24), size = (a >> 20) & 3, q = NE_BIT(a, 6);
        int m = (int)(NE_BIT(a, 5) << 4 | (a & 15));
        int bd = ne_base(d, q), bn = ne_base(n, q), bm = ne_base(m, q), w = q ? 4 : 2, i;
        if (opc == 1 && B) {                                        /* logic on whole words */
            static const char *expr[2][4] = {
                { "x & y", "x & ~y", "x | y", "x | ~y" },
                { "x ^ y", "(z & x) | (~z & y)", "(x & y) | (z & ~y)", "(z & y) | (x & ~y)" } };
            fprintf(f, "    {");
            for (i = 0; i < w; i++)
                fprintf(f, " uint32_t x%d = vfp_s[%d].u, y%d = vfp_s[%d].u, z%d = vfp_s[%d].u;", i, bn + i, i, bm + i, i, bd + i);
            for (i = 0; i < w; i++) {
                char e[64]; const char *s = expr[U][size]; int k = 0;
                for (; *s && k < 60; s++) {                        /* x -> x<i>, y -> y<i>, z -> z<i> */
                    e[k++] = *s;
                    if (*s == 'x' || *s == 'y' || *s == 'z') e[k++] = (char)('0' + i);
                }
                e[k] = 0;
                fprintf(f, " vfp_s[%d].u = %s;", bd + i, e);
            }
            fprintf(f, " }\n");
            return 1;
        }
        if (opc == 13 || opc == 14) {
            int hi = NE_BIT(a, 21);
            const char *op = NULL; int kind = 0;                    /* 1 = n op m, 2 = d op n*m, 3 = compare */
            if (opc == 13 && !B && !U)          { kind = 1; op = hi ? "-" : "+"; }
            else if (opc == 13 && B && !U)      { kind = 2; op = hi ? "-" : "+"; }
            else if (opc == 13 && B && U && !hi){ kind = 1; op = "*"; }
            else if (opc == 14 && !B && !(!U && hi)) { kind = 3; op = !U ? "==" : hi ? ">" : ">="; }
            if (!kind) return 0;
            fprintf(f, "    {");
            for (i = 0; i < w; i++) {
                fprintf(f, " float n%d = vfp_s[%d].f, m%d = vfp_s[%d].f;", i, bn + i, i, bm + i);
                if (kind == 2) fprintf(f, " float d%d = vfp_s[%d].f;", i, bd + i);
            }
            for (i = 0; i < w; i++) {
                if (kind == 1)      fprintf(f, " vfp_s[%d].f = n%d %s m%d;", bd + i, i, op, i);
                else if (kind == 2) fprintf(f, " vfp_s[%d].f = d%d %s n%d * m%d;", bd + i, i, op, i, i);
                else                fprintf(f, " vfp_s[%d].u = (n%d %s m%d) ? 0xFFFFFFFFu : 0u;", bd + i, i, op, i);
            }
            fprintf(f, " }\n");
            return 1;
        }
        (void)size;
        return 0;
    }
    /* dp_scalar, F32: bit 23, !bit 4, size 2, bit 6 */
    if (!NE_BIT(a, 4) && ((a >> 20) & 3) == 2 && NE_BIT(a, 6)) {
        int opc = (a >> 8) & 15, q = NE_BIT(a, 24);              /* bit 24 is Q here */
        if (opc != 1 && opc != 5 && opc != 9) return 0;
        int mreg = (int)(a & 15), idx = (int)NE_BIT(a, 5);
        int bd = ne_base(d, q), bn = ne_base(n, q), w = q ? 4 : 2, i;
        fprintf(f, "    { float s = vfp_s[%d].f;", 2 * mreg + idx);
        for (i = 0; i < w; i++) {
            fprintf(f, " float n%d = vfp_s[%d].f;", i, bn + i);
            if (opc != 9) fprintf(f, " float d%d = vfp_s[%d].f;", i, bd + i);
        }
        for (i = 0; i < w; i++) {
            if (opc == 9) fprintf(f, " vfp_s[%d].f = n%d * s;", bd + i, i);
            else          fprintf(f, " vfp_s[%d].f = d%d %s n%d * s;", bd + i, i, opc == 1 ? "+" : "-", i);
        }
        fprintf(f, " }\n");
        return 1;
    }
    return 0;
}

#endif
