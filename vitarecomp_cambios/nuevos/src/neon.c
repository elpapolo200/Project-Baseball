/* neon.c — Advanced SIMD, decoded and executed from the encoding.
 *
 * Register file: vfp_s[64] words, D0-D31; Qn is D2n:D2n+1 and is contiguous.
 * Every operation reads its sources into locals first and writes the result
 * last, so a destination that is also a source (vadd q0, q0, q1) is safe.
 *
 * Decoding follows the ARM ARM "Advanced SIMD data-processing" tables, on the
 * A32 bit layout: the Thumb U bit (first halfword bit 12) moves to bit 24 and
 * bits 23-0 are the same in both encodings.
 *
 * Deliberately not modelled: FPSCR.QC (the sticky saturation flag), flush-to-
 * zero, and the exact precision of VRECPE/VRSQRTE (computed exactly; the
 * Newton steps the code runs afterwards keep it exact).
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/neon.h"
#include "vitarecomp/recomp_rt.h"
#include "vitarecomp/vfp.h"

typedef union {
    uint8_t  b[32];         /* 32: room for two concatenated Q registers */
    int8_t   sb[32];
    uint16_t h[16];
    int16_t  sh[16];
    uint32_t w[8];
    int32_t  sw[8];
    float    f[8];
    uint64_t d[4];
    int64_t  sd[4];
} vec;

#define BIT(a, n) (((a) >> (n)) & 1u)

static uint8_t *dptr(int d) { return (uint8_t *)&vfp_s[(d & 31) * 2]; }

static void vget(vec *v, int d, int q) {
    memset(v, 0, sizeof(*v));
    memcpy(v->b, dptr(q ? d & ~1 : d), q ? 16 : 8);
}

static void vput(const vec *v, int d, int q) {
    memcpy(dptr(q ? d & ~1 : d), v->b, q ? 16 : 8);
}

/* Element i of size 0-3 (8-64 bits), zero- or sign-extended to 64. */
static uint64_t el(const vec *v, int size, int i, int sgn) {
    switch (size) {
        case 0:  return sgn ? (uint64_t)(int64_t)v->sb[i] : v->b[i];
        case 1:  return sgn ? (uint64_t)(int64_t)v->sh[i] : v->h[i];
        case 2:  return sgn ? (uint64_t)(int64_t)v->sw[i] : v->w[i];
        default: return v->d[i];
    }
}

static void set_el(vec *v, int size, int i, uint64_t x) {
    switch (size) {
        case 0:  v->b[i] = (uint8_t)x; break;
        case 1:  v->h[i] = (uint16_t)x; break;
        case 2:  v->w[i] = (uint32_t)x; break;
        default: v->d[i] = x; break;
    }
}

static uint64_t emask(int size) { return size >= 3 ? ~0ull : ((1ull << (8 << size)) - 1); }

/* Saturate a value to an element of `size`, signed or unsigned. */
static uint64_t sat(int64_t v, int size, int sgn) {
    int bits = 8 << size;
    if (bits == 64) return (uint64_t)v;
    if (sgn) {
        int64_t hi = (1ll << (bits - 1)) - 1, lo = -(1ll << (bits - 1));
        return (uint64_t)(v > hi ? hi : v < lo ? lo : v);
    }
    int64_t hi = (int64_t)((1ull << bits) - 1);
    return (uint64_t)(v > hi ? hi : v < 0 ? 0 : v);
}

/* Shift by a signed amount: positive left, negative right (arithmetic if sgn). */
static uint64_t shl_signed(uint64_t x, int sh, int size, int sgn) {
    int bits = 8 << size;
    if (sh >= 0) return sh >= bits ? 0 : (x << sh) & emask(size);
    sh = -sh;
    if (sgn) {
        int64_t s = (int64_t)x;
        return (uint64_t)(sh >= 64 ? (s < 0 ? -1 : 0) : (s >> sh)) & emask(size);
    }
    return sh >= 64 ? 0 : (x >> sh);
}

static float fmaxf_arm(float a, float b) { return (a != a || b != b) ? NAN : (a > b ? a : b); }
static float fminf_arm(float a, float b) { return (a != a || b != b) ? NAN : (a < b ? a : b); }

/* --- three registers of the same length ------------------------------------ */

static int dp_3same(uint32_t a) {
    int opc = (a >> 8) & 15, B = BIT(a, 4), U = BIT(a, 24), size = (a >> 20) & 3, q = BIT(a, 6);
    int d = BIT(a, 22) << 4 | ((a >> 12) & 15);
    int n = BIT(a, 7) << 4 | ((a >> 16) & 15);
    int m = BIT(a, 5) << 4 | (a & 15);
    vec vn, vm, vd, r;
    vget(&vn, n, q); vget(&vm, m, q); vget(&vd, d, q);
    r = vd;
    int lanes = (q ? 16 : 8) >> size;
    uint64_t mk = emask(size);
    int fl = q ? 4 : 2;

    if (opc == 1 && B) {                                   /* logic, whole words */
        for (int w = 0; w < (q ? 2 : 1); w++) {
            uint64_t x = vn.d[w], y = vm.d[w], z = vd.d[w], o;
            if (!U) o = size == 0 ? x & y : size == 1 ? x & ~y : size == 2 ? x | y : x | ~y;
            else    o = size == 0 ? x ^ y : size == 1 ? (z & x) | (~z & y)
                      : size == 2 ? (x & y) | (z & ~y) : (z & y) | (x & ~y);
            r.d[w] = o;
        }
        vput(&r, d, q);
        return 1;
    }

    if (opc >= 13) {                                       /* float, 32-bit lanes */
        int hi = BIT(a, 21);
        if (opc == 13 && !B && !U) {
            for (int i = 0; i < fl; i++) r.f[i] = hi ? vn.f[i] - vm.f[i] : vn.f[i] + vm.f[i];
        } else if (opc == 13 && !B && U) {
            if (!hi) {                                     /* VPADD.F32 (D only) */
                r.f[0] = vn.f[0] + vn.f[1];
                r.f[1] = vm.f[0] + vm.f[1];
            } else {
                for (int i = 0; i < fl; i++) r.f[i] = fabsf(vn.f[i] - vm.f[i]);
            }
        } else if (opc == 13 && B && !U) {
            for (int i = 0; i < fl; i++)
                r.f[i] = hi ? vd.f[i] - vn.f[i] * vm.f[i] : vd.f[i] + vn.f[i] * vm.f[i];
        } else if (opc == 13 && B && U && !hi) {
            for (int i = 0; i < fl; i++) r.f[i] = vn.f[i] * vm.f[i];
        } else if (opc == 14 && !B) {
            for (int i = 0; i < fl; i++) {
                int t = !U ? vn.f[i] == vm.f[i] : hi ? vn.f[i] > vm.f[i] : vn.f[i] >= vm.f[i];
                if (!U && hi) return 0;
                r.w[i] = t ? 0xFFFFFFFFu : 0;
            }
        } else if (opc == 14 && B && U) {
            for (int i = 0; i < fl; i++) {
                float x = fabsf(vn.f[i]), y = fabsf(vm.f[i]);
                r.w[i] = (hi ? x > y : x >= y) ? 0xFFFFFFFFu : 0;
            }
        } else if (opc == 15 && !B && !U) {
            for (int i = 0; i < fl; i++)
                r.f[i] = hi ? fminf_arm(vn.f[i], vm.f[i]) : fmaxf_arm(vn.f[i], vm.f[i]);
        } else if (opc == 15 && !B && U) {                 /* VPMAX/VPMIN.F32 */
            float (*op)(float, float) = hi ? fminf_arm : fmaxf_arm;
            r.f[0] = op(vn.f[0], vn.f[1]);
            r.f[1] = op(vm.f[0], vm.f[1]);
        } else if (opc == 15 && B && !U) {                 /* VRECPS / VRSQRTS */
            for (int i = 0; i < fl; i++)
                r.f[i] = hi ? (3.0f - vn.f[i] * vm.f[i]) * 0.5f : 2.0f - vn.f[i] * vm.f[i];
        } else {
            return 0;
        }
        vput(&r, d, q);
        return 1;
    }

    int pair = (opc == 10) || (opc == 11 && B && !U);
    if (pair) {                                            /* VPMAX/VPMIN/VPADD int, D only */
        if (q) return 0;
        for (int i = 0; i < lanes; i++) {
            const vec *src = i < lanes / 2 ? &vn : &vm;
            int k = (i % (lanes / 2)) * 2;
            uint64_t x = el(src, size, k, !U), y = el(src, size, k + 1, !U), o;
            if (opc == 11) o = x + y;
            else if (!U) o = B ? ((int64_t)x < (int64_t)y ? x : y) : ((int64_t)x > (int64_t)y ? x : y);
            else         o = B ? (x < y ? x : y) : (x > y ? x : y);
            set_el(&r, size, i, o & mk);
        }
        vput(&r, d, q);
        return 1;
    }

    for (int i = 0; i < lanes; i++) {
        int sg = !U;
        uint64_t x = el(&vn, size, i, sg), y = el(&vm, size, i, sg), z = el(&vd, size, i, 0), o;
        int64_t sx = (int64_t)x, sy = (int64_t)y;
        switch (opc) {
            case 0:
                if (!B) o = sg ? (uint64_t)((sx + sy) >> 1) : (x + y) >> 1;
                else    o = sg ? sat(sx + sy, size, 1) : sat((int64_t)(x + y), size, 0);
                if (size == 3 && B) return 0;
                break;
            case 1:                                        /* VRHADD */
                if (size == 3) return 0;
                o = sg ? (uint64_t)((sx + sy + 1) >> 1) : (x + y + 1) >> 1;
                break;
            case 2:
                if (!B) o = sg ? (uint64_t)((sx - sy) >> 1) : (x - y) >> 1;
                else    o = sg ? sat(sx - sy, size, 1) : (x > y ? x - y : 0);
                if (size == 3 && B) return 0;
                break;
            case 3:
                o = (sg ? (B ? sx >= sy : sx > sy) : (B ? x >= y : x > y)) ? mk : 0;
                break;
            case 4:
                if (B) return 0;                           /* VQSHL (register) */
                /* VSHL (register): Dm shifted by the signed low byte of Dn. */
                o = shl_signed(y, (int8_t)(x & 0xFF), size, sg);
                break;
            case 6:
                if (size == 3) return 0;
                o = sg ? (B ? (sx < sy ? x : y) : (sx > sy ? x : y))
                       : (B ? (x < y ? x : y) : (x > y ? x : y));
                break;
            case 7:
                if (size == 3) return 0;
                o = sg ? (uint64_t)(sx > sy ? sx - sy : sy - sx) : (x > y ? x - y : y - x);
                if (B) o += z;
                break;
            case 8:
                if (!B) o = U ? x - y : x + y;
                else    o = (U ? (x & mk) == (y & mk) : (x & y & mk) != 0) ? mk : 0;
                break;
            case 9:
                if (size == 3) return 0;
                if (!B) o = U ? z - x * y : z + x * y;
                else if (!U) o = x * y;
                else return 0;                             /* VMUL.P8 */
                break;
            default:
                return 0;
        }
        set_el(&r, size, i, o & mk);
    }
    vput(&r, d, q);
    return 1;
}

/* --- one register and a modified immediate --------------------------------- */

static uint64_t expand_imm(int op, int cmode, uint32_t imm8, int *ok) {
    uint64_t r = 0;
    *ok = 1;
    switch (cmode >> 1) {
        case 0: r = imm8; r |= r << 32; break;
        case 1: r = (uint64_t)imm8 << 8;  r |= r << 32; break;
        case 2: r = (uint64_t)imm8 << 16; r |= r << 32; break;
        case 3: r = (uint64_t)imm8 << 24; r |= r << 32; break;
        case 4: r = imm8; r |= r << 16; r |= r << 32; break;
        case 5: r = (uint64_t)imm8 << 8; r |= r << 16; r |= r << 32; break;
        case 6:
            r = (cmode & 1) ? ((uint64_t)imm8 << 16) | 0xFFFF : ((uint64_t)imm8 << 8) | 0xFF;
            r |= r << 32;
            break;
        default:
            if (!(cmode & 1)) {
                if (!op) { r = imm8 * 0x0101010101010101ull; }
                else {
                    for (int i = 0; i < 8; i++) if (imm8 & (1u << i)) r |= 0xFFull << (8 * i);
                }
            } else {
                if (op) { *ok = 0; return 0; }
                uint32_t b6 = (imm8 >> 6) & 1;
                uint32_t f = ((imm8 >> 7) << 31) | ((b6 ^ 1) << 30) | (b6 ? 0x3E000000u : 0)
                           | ((imm8 & 0x3F) << 19);
                r = f | ((uint64_t)f << 32);
            }
    }
    return r;
}

static int dp_modimm(uint32_t a) {
    uint32_t imm8 = (BIT(a, 24) << 7) | (((a >> 16) & 7) << 4) | (a & 15);
    int cmode = (a >> 8) & 15, op = BIT(a, 5), q = BIT(a, 6);
    int d = BIT(a, 22) << 4 | ((a >> 12) & 15);
    int ok;
    uint64_t imm = expand_imm(op, cmode, imm8, &ok);
    if (!ok) return 0;
    vec v;
    vget(&v, d, q);
    for (int w = 0; w < (q ? 2 : 1); w++) {
        if ((cmode & 1) && cmode < 12) v.d[w] = op ? v.d[w] & ~imm : v.d[w] | imm;  /* VBIC / VORR */
        else if (op && cmode != 14)    v.d[w] = ~imm;                               /* VMVN */
        else                           v.d[w] = imm;                                /* VMOV */
    }
    vput(&v, d, q);
    return 1;
}

/* --- two registers and a shift amount -------------------------------------- */

static int dp_shift(uint32_t a) {
    int imm6 = (a >> 16) & 63, L = BIT(a, 7), opc = (a >> 8) & 15, U = BIT(a, 24), q = BIT(a, 6);
    int d = BIT(a, 22) << 4 | ((a >> 12) & 15);
    int m = BIT(a, 5) << 4 | (a & 15);
    int size, esize;
    if (L)              { size = 3; esize = 64; }
    else if (imm6 & 32) { size = 2; esize = 32; }
    else if (imm6 & 16) { size = 1; esize = 16; }
    else if (imm6 & 8)  { size = 0; esize = 8; }
    else return 0;
    int rs = (L ? 64 : 2 * esize) - imm6;      /* right shift amount, 1..esize */
    int ls = imm6 - (L ? 0 : esize);           /* left shift amount, 0..esize-1 */
    uint64_t mk = emask(size);
    vec vm, vd, r;

    if (opc <= 7) {
        vget(&vm, m, q); vget(&vd, d, q);
        r = vd;
        int lanes = (q ? 16 : 8) >> size;
        for (int i = 0; i < lanes; i++) {
            uint64_t x = el(&vm, size, i, !U), z = el(&vd, size, i, 0), o;
            int64_t sx = (int64_t)x;
            switch (opc) {
                case 0: case 1: case 2: case 3: {
                    uint64_t sh;
                    int rnd = opc == 2 || opc == 3;
                    if (!U) {
                        int64_t v = sx;
                        if (rnd) { if (rs >= 64) v = 0; else v = (v >> (rs - 1)) + 1, v >>= 1; }
                        else v = rs >= 64 ? (v < 0 ? -1 : 0) : v >> rs;
                        sh = (uint64_t)v;
                    } else {
                        if (rnd) sh = rs >= 64 ? (x >> 63) : ((x >> (rs - 1)) + 1) >> 1;
                        else sh = rs >= 64 ? 0 : x >> rs;
                    }
                    o = (opc & 1) ? z + sh : sh;
                    break;
                }
                case 4:
                    if (!U) return 0;
                    { uint64_t keep = rs >= 64 ? mk : (mk & ~(mk >> rs));
                      o = (z & keep) | (rs >= 64 ? 0 : (x & mk) >> rs); }
                    break;
                case 5:
                    if (!U) o = x << ls;
                    else    o = (z & ((1ull << ls) - 1)) | (x << ls);
                    break;
                case 6: case 7: {                          /* VQSHLU (6, U=1) / VQSHL (7) */
                    if (opc == 6 && !U) return 0;
                    int sgn_src = opc == 7 ? !U : 1, sgn_dst = opc == 7 ? !U : 0;
                    if (size == 3) return 0;
                    int64_t v = sgn_src ? (int64_t)el(&vm, size, i, 1) : (int64_t)el(&vm, size, i, 0);
                    int64_t big = v * (1ll << ls);
                    o = sat(big, size, sgn_dst);
                    break;
                }
                default: return 0;
            }
            set_el(&r, size, i, o & mk);
        }
        vput(&r, d, q);
        return 1;
    }

    if (opc == 8 || opc == 9) {                            /* narrowing right shifts */
        int rnd = BIT(a, 6);
        int ssize = size + 1;                              /* source element size */
        if (ssize > 3) return 0;
        vget(&vm, m, 1);
        memset(&r, 0, sizeof(r));
        int lanes = 8 >> size;
        for (int i = 0; i < lanes; i++) {
            int sgn_src = opc == 8 ? (U ? 1 : 0) : !U;
            int64_t v = (int64_t)el(&vm, ssize, i, sgn_src);
            if (rnd) v = (v >> (rs - 1)) + 1, v >>= 1;
            else     v >>= rs;
            uint64_t o;
            if (opc == 8 && !U) o = (uint64_t)v & emask(size);       /* VSHRN / VRSHRN */
            else if (opc == 8)  o = sat(v, size, 0);                  /* VQSHRUN */
            else                o = sat(v, size, !U);                 /* VQSHRN */
            set_el(&r, size, i, o);
        }
        vput(&r, d, 0);
        return 1;
    }

    if (opc == 10 && !BIT(a, 6)) {                         /* VSHLL / VMOVL */
        if (size == 3) return 0;
        vget(&vm, m, 0);
        memset(&r, 0, sizeof(r));
        int lanes = 8 >> size;
        for (int i = 0; i < lanes; i++)
            set_el(&r, size + 1, i, (el(&vm, size, i, !U) << ls) & emask(size + 1));
        vput(&r, d, 1);
        return 1;
    }

    if ((opc == 14 || opc == 15) && !L && size == 2) {     /* VCVT fixed <-> float */
        int frac = 64 - imm6;
        int tofix = BIT(a, 8);
        vget(&vm, m, q);
        r = vm;
        double scale = ldexp(1.0, frac);
        for (int i = 0; i < (q ? 4 : 2); i++) {
            if (!tofix) r.f[i] = (float)((U ? (double)vm.w[i] : (double)vm.sw[i]) / scale);
            else        r.w[i] = U ? vfp_cvt_u32(trunc((double)vm.f[i] * scale))
                                   : vfp_cvt_s32(trunc((double)vm.f[i] * scale));
        }
        vput(&r, d, q);
        return 1;
    }
    return 0;
}

/* --- three registers of different lengths ---------------------------------- */

static int dp_3diff(uint32_t a) {
    int opc = (a >> 8) & 15, U = BIT(a, 24), size = (a >> 20) & 3;
    int d = BIT(a, 22) << 4 | ((a >> 12) & 15);
    int n = BIT(a, 7) << 4 | ((a >> 16) & 15);
    int m = BIT(a, 5) << 4 | (a & 15);
    if (size == 3) return 0;
    int lanes = 8 >> size, ws = size + 1;
    uint64_t wm = emask(ws);
    vec vn, vm, vd, r;
    int wide_n = opc == 1 || opc == 3 || opc == 4 || opc == 6;
    int narrow = opc == 4 || opc == 6;
    vget(&vn, n, wide_n); vget(&vm, m, narrow); vget(&vd, d, !narrow);
    r = vd;
    if (narrow) memset(&r, 0, sizeof(r));
    for (int i = 0; i < lanes; i++) {
        uint64_t x = wide_n ? el(&vn, ws, i, !U) : el(&vn, size, i, !U);
        uint64_t y = narrow ? el(&vm, ws, i, 0) : el(&vm, size, i, !U);
        uint64_t z = el(&vd, ws, i, 0), o;
        switch (opc) {
            case 0: case 1: o = x + y; break;
            case 2: case 3: o = x - y; break;
            case 4: case 6: {                              /* VADDHN / VSUBHN (+R) */
                uint64_t s = opc == 4 ? x + y : x - y;
                if (U) s += 1ull << ((8 << size) - 1);
                set_el(&r, size, i, (s >> (8 << size)) & emask(size));
                continue;
            }
            case 5: case 7: {
                int64_t sx = (int64_t)x, sy = (int64_t)y;
                uint64_t ab = U ? (x > y ? x - y : y - x) : (uint64_t)(sx > sy ? sx - sy : sy - sx);
                o = opc == 5 ? z + ab : ab;
                break;
            }
            case 8:  o = z + x * y; break;
            case 10: o = z - x * y; break;
            case 12: o = x * y; break;
            default: return 0;
        }
        set_el(&r, ws, i, o & wm);
    }
    vput(&r, d, !narrow);
    return 1;
}

/* --- two registers and a scalar -------------------------------------------- */

static int dp_scalar(uint32_t a) {
    int opc = (a >> 8) & 15, size = (a >> 20) & 3, U = BIT(a, 24);
    int d = BIT(a, 22) << 4 | ((a >> 12) & 15);
    int n = BIT(a, 7) << 4 | ((a >> 16) & 15);
    int mreg, idx;
    if (size == 1)      { mreg = a & 7;  idx = (BIT(a, 5) << 1) | BIT(a, 3); }
    else if (size == 2) { mreg = a & 15; idx = BIT(a, 5); }
    else return 0;
    vec vm, vn, vd, r;
    vget(&vm, mreg, 0);
    int longf = opc == 2 || opc == 6 || opc == 10;
    if (longf) {
        vget(&vn, n, 0); vget(&vd, d, 1);
        r = vd;
        uint64_t s = el(&vm, size, idx, !U);
        for (int i = 0; i < (8 >> size); i++) {
            uint64_t x = el(&vn, size, i, !U), z = el(&vd, size + 1, i, 0);
            uint64_t o = opc == 2 ? z + x * s : opc == 6 ? z - x * s : x * s;
            set_el(&r, size + 1, i, o & emask(size + 1));
        }
        vput(&r, d, 1);
        return 1;
    }
    int q = U;                                             /* bit 24 is Q here */
    vget(&vn, n, q); vget(&vd, d, q);
    r = vd;
    int fl = opc == 1 || opc == 5 || opc == 9;
    if (fl) {
        if (size != 2) return 0;
        float s = vm.f[idx];
        for (int i = 0; i < (q ? 4 : 2); i++)
            r.f[i] = opc == 1 ? vd.f[i] + vn.f[i] * s : opc == 5 ? vd.f[i] - vn.f[i] * s : vn.f[i] * s;
    } else if (opc == 0 || opc == 4 || opc == 8) {
        uint64_t s = el(&vm, size, idx, 0);
        for (int i = 0; i < ((q ? 16 : 8) >> size); i++) {
            uint64_t x = el(&vn, size, i, 0), z = el(&vd, size, i, 0);
            uint64_t o = opc == 0 ? z + x * s : opc == 4 ? z - x * s : x * s;
            set_el(&r, size, i, o & emask(size));
        }
    } else {
        return 0;
    }
    vput(&r, d, q);
    return 1;
}

/* --- miscellaneous: VEXT, VTBL, VDUP scalar, two-register misc ------------- */

static void rev_elems(vec *r, const vec *v, int size, int container_bytes, int bytes) {
    int eb = 1 << size;
    for (int base = 0; base < bytes; base += container_bytes)
        for (int i = 0; i < container_bytes; i += eb)
            memcpy(&r->b[base + container_bytes - eb - i], &v->b[base + i], (size_t)eb);
}

static int dp_misc2(uint32_t a) {
    int size = (a >> 18) & 3, A = (a >> 16) & 3, opc4 = (a >> 7) & 15, q = BIT(a, 6);
    int d = BIT(a, 22) << 4 | ((a >> 12) & 15);
    int m = BIT(a, 5) << 4 | (a & 15);
    int bytes = q ? 16 : 8, lanes = bytes >> size;
    vec vm, vd, r;
    vget(&vm, m, q); vget(&vd, d, q);
    memset(&r, 0, sizeof(r));

    if (A == 0) {
        switch (opc4) {
            case 0: case 1: case 2: {                      /* VREV64 / 32 / 16 */
                int cb = opc4 == 0 ? 8 : opc4 == 1 ? 4 : 2;
                if ((1 << size) >= cb) return 0;
                rev_elems(&r, &vm, size, cb, bytes);
                break;
            }
            case 4: case 5: case 12: case 13: {            /* VPADDL / VPADAL */
                int u = opc4 & 1, acc = opc4 >= 12;
                if (size == 3) return 0;
                for (int i = 0; i < lanes / 2; i++) {
                    uint64_t s = el(&vm, size, 2 * i, !u) + el(&vm, size, 2 * i + 1, !u);
                    if (acc) s += el(&vd, size + 1, i, 0);
                    set_el(&r, size + 1, i, s & emask(size + 1));
                }
                break;
            }
            case 9:                                        /* VCLZ */
                for (int i = 0; i < lanes; i++) {
                    uint64_t x = el(&vm, size, i, 0);
                    int bits = 8 << size, c = 0;
                    while (c < bits && !(x & (1ull << (bits - 1 - c)))) c++;
                    set_el(&r, size, i, (uint64_t)c);
                }
                break;
            case 10:                                       /* VCNT */
                if (size) return 0;
                for (int i = 0; i < bytes; i++) {
                    int c = 0;
                    for (int k = 0; k < 8; k++) c += (vm.b[i] >> k) & 1;
                    r.b[i] = (uint8_t)c;
                }
                break;
            case 11:                                       /* VMVN */
                for (int w = 0; w < bytes / 8; w++) r.d[w] = ~vm.d[w];
                break;
            default:
                return 0;
        }
        vput(&r, d, q);
        return 1;
    }

    if (A == 1) {
        int F = BIT(a, 10), op = (a >> 7) & 7;
        if (F && size != 2) return 0;
        for (int i = 0; i < lanes; i++) {
            if (F) {
                float x = vm.f[i];
                switch (op) {
                    case 0: r.w[i] = x >  0.0f ? 0xFFFFFFFFu : 0; break;
                    case 1: r.w[i] = x >= 0.0f ? 0xFFFFFFFFu : 0; break;
                    case 2: r.w[i] = x == 0.0f ? 0xFFFFFFFFu : 0; break;
                    case 3: r.w[i] = x <= 0.0f ? 0xFFFFFFFFu : 0; break;
                    case 4: r.w[i] = x <  0.0f ? 0xFFFFFFFFu : 0; break;
                    case 6: r.w[i] = vm.w[i] & 0x7FFFFFFFu; break;
                    case 7: r.w[i] = vm.w[i] ^ 0x80000000u; break;
                    default: return 0;
                }
            } else {
                int64_t x = (int64_t)el(&vm, size, i, 1);
                uint64_t mk = emask(size), o;
                switch (op) {
                    case 0: o = x >  0 ? mk : 0; break;
                    case 1: o = x >= 0 ? mk : 0; break;
                    case 2: o = x == 0 ? mk : 0; break;
                    case 3: o = x <= 0 ? mk : 0; break;
                    case 4: o = x <  0 ? mk : 0; break;
                    case 6: o = (uint64_t)(x < 0 ? -x : x); break;
                    case 7: o = (uint64_t)(-x); break;
                    default: return 0;
                }
                set_el(&r, size, i, o & mk);
            }
        }
        vput(&r, d, q);
        return 1;
    }

    if (A == 2) {
        int op6 = BIT(a, 6);
        if (opc4 == 0) {                                   /* VSWP */
            vput(&vd, m, q); vput(&vm, d, q);
            return 1;
        }
        if (opc4 >= 1 && opc4 <= 3) {                      /* VTRN / VUZP / VZIP */
            vec nd = vd, nm = vm, cat;
            if (opc4 == 1) {
                for (int i = 0; i + 1 < lanes; i += 2) {
                    uint64_t t = el(&vd, size, i + 1, 0);
                    set_el(&nd, size, i + 1, el(&vm, size, i, 0));
                    set_el(&nm, size, i, t);
                }
            } else {
                memset(&cat, 0, sizeof(cat));
                if (opc4 == 2) {                           /* VUZP */
                    memcpy(cat.b, vd.b, (size_t)bytes);
                    memcpy(cat.b + bytes, vm.b, (size_t)bytes);
                    for (int i = 0; i < lanes; i++) {
                        set_el(&nd, size, i, el(&cat, size, 2 * i, 0));
                        set_el(&nm, size, i, el(&cat, size, 2 * i + 1, 0));
                    }
                } else {                                   /* VZIP */
                    for (int i = 0; i < lanes; i++) {
                        set_el(&cat, size, 2 * i, el(&vd, size, i, 0));
                        set_el(&cat, size, 2 * i + 1, el(&vm, size, i, 0));
                    }
                    memcpy(nd.b, cat.b, (size_t)bytes);
                    memcpy(nm.b, cat.b + bytes, (size_t)bytes);
                }
            }
            vput(&nd, d, q); vput(&nm, m, q);
            return 1;
        }
        if (opc4 == 4 || opc4 == 5) {                      /* VMOVN, VQMOVUN, VQMOVN */
            if (size == 3) return 0;
            vget(&vm, m, 1);
            for (int i = 0; i < (8 >> size); i++) {
                uint64_t o;
                if (opc4 == 4 && !op6) o = el(&vm, size + 1, i, 0) & emask(size);
                else if (opc4 == 4)    o = sat((int64_t)el(&vm, size + 1, i, 1), size, 0);
                else if (!op6)         o = sat((int64_t)el(&vm, size + 1, i, 1), size, 1);
                else {
                    uint64_t x = el(&vm, size + 1, i, 0);
                    o = x > emask(size) ? emask(size) : x;
                }
                set_el(&r, size, i, o);
            }
            vput(&r, d, 0);
            return 1;
        }
        if (opc4 == 6 && !op6) {                           /* VSHLL by the element size */
            if (size == 3) return 0;
            vget(&vm, m, 0);
            for (int i = 0; i < (8 >> size); i++)
                set_el(&r, size + 1, i, (el(&vm, size, i, 0) << (8 << size)) & emask(size + 1));
            vput(&r, d, 1);
            return 1;
        }
        return 0;
    }

    /* A == 3 */
    if (size != 2) return 0;
    if (opc4 == 8 || opc4 == 9 || opc4 == 10 || opc4 == 11) {
        int fl = BIT(a, 8), rsq = opc4 & 1;
        for (int i = 0; i < lanes; i++) {
            if (fl) r.f[i] = rsq ? 1.0f / sqrtf(vm.f[i]) : 1.0f / vm.f[i];
            else {
                /* Unsigned fixed-point estimate: 0.32 format input. */
                double x = vm.w[i] / 4294967296.0;
                double e = rsq ? 1.0 / sqrt(x) : 1.0 / x;
                r.w[i] = (x < (rsq ? 0.25 : 0.5)) ? 0xFFFFFFFFu : vfp_cvt_u32(e * 2147483648.0);
            }
        }
    } else if (opc4 >= 12) {
        int op = opc4 & 3;
        for (int i = 0; i < lanes; i++) {
            if (op == 0)      r.f[i] = (float)vm.sw[i];
            else if (op == 1) r.f[i] = (float)vm.w[i];
            else if (op == 2) r.w[i] = vfp_cvt_s32(trunc((double)vm.f[i]));
            else              r.w[i] = vfp_cvt_u32(trunc((double)vm.f[i]));
        }
    } else {
        return 0;
    }
    vput(&r, d, q);
    return 1;
}

static int dp_other(uint32_t a) {
    int d = BIT(a, 22) << 4 | ((a >> 12) & 15);
    int n = BIT(a, 7) << 4 | ((a >> 16) & 15);
    int m = BIT(a, 5) << 4 | (a & 15);
    int q = BIT(a, 6);
    if (!BIT(a, 24)) {                                     /* VEXT */
        int imm = (a >> 8) & 15, bytes = q ? 16 : 8;
        if (imm >= bytes) return 0;
        vec vn, vm, r;
        vget(&vn, n, q); vget(&vm, m, q);
        memset(&r, 0, sizeof(r));
        for (int i = 0; i < bytes; i++)
            r.b[i] = i + imm < bytes ? vn.b[i + imm] : vm.b[i + imm - bytes];
        vput(&r, d, q);
        return 1;
    }
    int sel = (a >> 10) & 3;
    if (sel == 3) {                                        /* VDUP (scalar) */
        int imm4 = (a >> 16) & 15, size, idx;
        if (imm4 & 1)      { size = 0; idx = imm4 >> 1; }
        else if (imm4 & 2) { size = 1; idx = imm4 >> 2; }
        else if (imm4 & 4) { size = 2; idx = imm4 >> 3; }
        else return 0;
        vec vm, r;
        vget(&vm, m, 0);
        memset(&r, 0, sizeof(r));
        uint64_t x = el(&vm, size, idx, 0);
        for (int i = 0; i < ((q ? 16 : 8) >> size); i++) set_el(&r, size, i, x);
        vput(&r, d, q);
        return 1;
    }
    if (sel == 2) {                                        /* VTBL / VTBX */
        int len = ((a >> 8) & 3) + 1, tbx = BIT(a, 6);
        uint8_t table[32];
        for (int k = 0; k < len; k++) memcpy(table + 8 * k, dptr(n + k), 8);
        vec vm, vd, r;
        vget(&vm, m, 0); vget(&vd, d, 0);
        r = vd;
        for (int i = 0; i < 8; i++) {
            int ix = vm.b[i];
            r.b[i] = ix < 8 * len ? table[ix] : (tbx ? vd.b[i] : 0);
        }
        vput(&r, d, 0);
        return 1;
    }
    return dp_misc2(a);
}

void neon_dp(uint32_t pc, uint32_t raw) {
    uint32_t a = (((raw >> 28) & 1) << 24) | (raw & 0x00FFFFFFu);
    int ok;
    if (!BIT(a, 23))                                       ok = dp_3same(a);
    else if (BIT(a, 4) && ((a >> 19) & 7) == 0 && !BIT(a, 7)) ok = dp_modimm(a);
    else if (BIT(a, 4))                                    ok = dp_shift(a);
    else if (((a >> 20) & 3) != 3)                         ok = BIT(a, 6) ? dp_scalar(a) : dp_3diff(a);
    else                                                   ok = dp_other(a);
    if (!ok) vita_trap_unimpl(pc, raw, "neon");
}

/* --- element and structure loads / stores ---------------------------------- */

static void xfer(int load, int d, int e, int ebytes, uint32_t addr) {
    uint8_t *p = dptr(d) + e * ebytes;
    if (load) {
        switch (ebytes) {
            case 1: p[0] = vita_read8(addr); break;
            case 2: { uint16_t v = vita_read16(addr); memcpy(p, &v, 2); break; }
            case 4: { uint32_t v = vita_read32(addr); memcpy(p, &v, 4); break; }
            default: {
                uint32_t lo = vita_read32(addr), hi = vita_read32(addr + 4);
                memcpy(p, &lo, 4); memcpy(p + 4, &hi, 4);
            }
        }
    } else {
        switch (ebytes) {
            case 1: vita_write8(addr, p[0]); break;
            case 2: { uint16_t v; memcpy(&v, p, 2); vita_write16(addr, v); break; }
            case 4: { uint32_t v; memcpy(&v, p, 4); vita_write32(addr, v); break; }
            default: {
                uint32_t lo, hi;
                memcpy(&lo, p, 4); memcpy(&hi, p + 4, 4);
                vita_write32(addr, lo); vita_write32(addr + 4, hi);
            }
        }
    }
}

uint32_t neon_ldst(uint32_t pc, uint32_t raw, uint32_t base) {
    uint32_t h1 = raw >> 16, h2 = raw & 0xFFFF;
    int load = BIT(h1, 5), single = BIT(h1, 7);
    int d = (int)(BIT(h1, 6) << 4 | ((h2 >> 12) & 15));
    uint32_t addr = base;

    if (!single) {
        int type = (h2 >> 8) & 15, size = (h2 >> 6) & 3, eb = 1 << size, elems = 8 / eb;
        int nst, regs, inc;
        switch (type) {
            case 7:  nst = 1; regs = 1; inc = 1; break;
            case 10: nst = 1; regs = 2; inc = 1; break;
            case 6:  nst = 1; regs = 3; inc = 1; break;
            case 2:  nst = 1; regs = 4; inc = 1; break;
            case 8:  nst = 2; regs = 1; inc = 1; break;
            case 9:  nst = 2; regs = 1; inc = 2; break;
            case 3:  nst = 2; regs = 2; inc = 2; break;
            case 4:  nst = 3; regs = 1; inc = 1; break;
            case 5:  nst = 3; regs = 1; inc = 2; break;
            case 0:  nst = 4; regs = 1; inc = 1; break;
            case 1:  nst = 4; regs = 1; inc = 2; break;
            default: vita_trap_unimpl(pc, raw, "neon vld/vst type"); return 0;
        }
        if (nst > 1 && size == 3) { vita_trap_unimpl(pc, raw, "neon vldN.64"); return 0; }
        if (nst == 1) {
            for (int r = 0; r < regs; r++)
                for (int e = 0; e < elems; e++) { xfer(load, d + r, e, eb, addr); addr += (uint32_t)eb; }
        } else {
            for (int r = 0; r < regs; r++)
                for (int e = 0; e < elems; e++)
                    for (int k = 0; k < nst; k++) {
                        xfer(load, d + r + k * inc, e, eb, addr);
                        addr += (uint32_t)eb;
                    }
        }
        return addr - base;
    }

    int size = (h2 >> 10) & 3, nst = ((h2 >> 8) & 3) + 1;
    if (size == 3) {                                       /* to all lanes (loads only) */
        if (!load) { vita_trap_unimpl(pc, raw, "neon vst all lanes"); return 0; }
        int sz = (h2 >> 6) & 3, T = BIT(h2, 5), eb = 1 << sz;
        if (sz == 3) { vita_trap_unimpl(pc, raw, "neon vld all lanes size"); return 0; }
        int regs = nst == 1 ? (T ? 2 : 1) : 1, inc = nst == 1 ? 1 : (T ? 2 : 1);
        for (int k = 0; k < nst; k++) {
            uint8_t tmp[8];
            for (int b = 0; b < eb; b++) tmp[b] = vita_read8(addr + (uint32_t)b);
            for (int r = 0; r < regs; r++) {
                uint8_t *p = dptr(d + k * inc + r);
                for (int e = 0; e < 8 / eb; e++) memcpy(p + e * eb, tmp, (size_t)eb);
            }
            addr += (uint32_t)eb;
        }
        return addr - base;
    }
    int eb = 1 << size, ia = (h2 >> 4) & 15, idx, inc = 1;
    if (size == 0)      idx = ia >> 1;
    else if (size == 1) { idx = ia >> 2; if (nst > 1 && BIT(ia, 1)) inc = 2; }
    else                { idx = ia >> 3; if (nst > 1 && BIT(ia, 2)) inc = 2; }
    for (int k = 0; k < nst; k++) { xfer(load, d + k * inc, idx, eb, addr); addr += (uint32_t)eb; }
    return addr - base;
}

/* --- core register <-> NEON ------------------------------------------------ */

void neon_dup_core(uint32_t raw, uint32_t value) {
    uint32_t h1 = raw >> 16, h2 = raw & 0xFFFF;
    int q = BIT(h1, 5), d = (int)(BIT(h2, 7) << 4 | (h1 & 15));
    int size = BIT(h1, 6) ? 0 : BIT(h2, 5) ? 1 : 2;
    vec r;
    memset(&r, 0, sizeof(r));
    for (int i = 0; i < ((q ? 16 : 8) >> size); i++) set_el(&r, size, i, value);
    vput(&r, d, q);
}

static void lane_of(uint32_t h1, uint32_t h2, int *size, int *idx) {
    int opc1 = (h1 >> 5) & 3, opc2 = (h2 >> 5) & 3;
    if (opc1 & 2)      { *size = 0; *idx = ((opc1 & 1) << 2) | opc2; }
    else if (opc2 & 1) { *size = 1; *idx = ((opc1 & 1) << 1) | (opc2 >> 1); }
    else               { *size = 2; *idx = opc1 & 1; }
}

void neon_set_lane(uint32_t raw, uint32_t value) {
    uint32_t h1 = raw >> 16, h2 = raw & 0xFFFF;
    int d = (int)(BIT(h2, 7) << 4 | (h1 & 15)), size, idx;
    lane_of(h1, h2, &size, &idx);
    vec r;
    vget(&r, d, 0);
    set_el(&r, size, idx, value);
    vput(&r, d, 0);
}

uint32_t neon_get_lane(uint32_t raw) {
    uint32_t h1 = raw >> 16, h2 = raw & 0xFFFF;
    int n = (int)(BIT(h2, 7) << 4 | (h1 & 15)), size, idx;
    lane_of(h1, h2, &size, &idx);
    vec v;
    vget(&v, n, 0);
    return (uint32_t)el(&v, size, idx, !BIT(h1, 7));
}
