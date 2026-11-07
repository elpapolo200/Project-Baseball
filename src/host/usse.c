/* Traductor GXP/USSE -> GLSL (ver usse.h). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usse.h"

/* ========================================================================== */
/* GXP                                                                         */
/* ========================================================================== */

typedef struct { const uint8_t *p; size_t n; } Gxp;

static uint32_t g32(const Gxp *g, uint32_t o) {
    if (o + 4 > g->n) return 0;
    return (uint32_t)g->p[o] | ((uint32_t)g->p[o + 1] << 8) | ((uint32_t)g->p[o + 2] << 16) | ((uint32_t)g->p[o + 3] << 24);
}
static uint32_t g16(const Gxp *g, uint32_t o) { return o + 2 <= g->n ? (uint32_t)g->p[o] | ((uint32_t)g->p[o + 1] << 8) : 0; }
static uint32_t grel(const Gxp *g, uint32_t o) { return o + g32(g, o); }
static uint64_t g64(const Gxp *g, uint32_t o) { return (uint64_t)g32(g, o) | ((uint64_t)g32(g, o + 4) << 32); }

typedef struct { const char *name; uint32_t cat, type, comps, cont, sem, arr, res; } Param;

static int gxp_param(const Gxp *g, uint32_t i, Param *p) {
    uint32_t n = g32(g, 0x24), base = grel(g, 0x28);
    if (i >= n) return 0;
    uint32_t o = base + 16 * i;
    uint32_t a1 = g16(g, o + 4);
    uint32_t no = o + g32(g, o);
    p->name = no < g->n ? (const char *)g->p + no : "";
    p->cat = a1 & 15; p->type = (a1 >> 4) & 15; p->comps = (a1 >> 8) & 15; p->cont = a1 >> 12;
    p->sem = g16(g, o + 6); p->arr = g32(g, o + 8); p->res = g32(g, o + 12);
    return 1;
}

/* ========================================================================== */
/* Representación intermedia                                                   */
/* ========================================================================== */

enum { RB_R, RB_O, RB_PA, RB_SA, RB_I, RB_C, RB_IMM, RB_NONE };
static const char *const RB_NAME[] = { "r", "o", "pa", "sa", "i", "c", "imm", "?" };

/* sel: 0-3 componente, 4 = 0.0, 5 = 1.0, 6 = 2.0, 7 = 0.5 */
typedef struct { uint8_t bank, neg, abs, h; uint16_t idx; int8_t sel[4]; } UOp;   /* h: leer/escribir como F16 */

enum {
    UI_NOP, UI_MAD, UI_DP, UI_ALU2, UI_MOV, UI_MOVC, UI_COMP, UI_PCK, UI_LDR, UI_OR, UI_AND,
    UI_IMAD, UI_TEST, UI_SMP, UI_KILL, UI_DUAL1, UI_DUAL2
};
/* operaciones de la doble emisión (código de 4 bits) */
enum { D_MAD, D_DP, D_SSQ, D_MUL, D_ADD, D_MOV, D_RSQ, D_RCP, D_FMAD, D_FADD, D_FMUL, D_FSUBFLR, D_FEXP, D_FLOG };
enum { A_MUL, A_ADD, A_FRC, A_DSX, A_DSY, A_MIN, A_MAX, A_DP };
enum { C_RCP, C_RSQ, C_LOG, C_EXP };
enum { F_U8, F_S8, F_O8, F_U16, F_S16, F_F16, F_F32, F_C10 };

typedef struct {
    uint8_t kind, sub, f16, mask, n;
    int8_t  pred;                 /* -1 = sin predicado; 0-3 pN; +8 = negado */
    UOp d, s[3];
    uint8_t sfmt, dfmt, scale;    /* VPCK */
    uint16_t off; uint8_t count;  /* LDR */
    uint8_t dim, cf16, df32, unit, gather;   /* SMP */
    uint8_t tsign, tzero, tand, tpdst, twben, top, tint; /* TEST */
} UInsn;

typedef struct {
    UInsn *v; uint32_t n, cap;
    int sec;                      /* decodificando el programa secundario */
    int frag;                     /* programa de fragmento (doble emisión con F16) */
    int inc[4], swz_inc[4][4], is_swz[4];
    char err[160];
    const Gxp *g;
    uint32_t c19;                 /* base sa del contenedor 19 */
    int8_t samp_unit[256];        /* sa -> unidad de textura (estado de sampler dependiente) */
} Dec;

static UInsn *push(Dec *d) {
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 256;
        d->v = (UInsn *)realloc(d->v, d->cap * sizeof(UInsn));
    }
    UInsn *x = &d->v[d->n++];
    memset(x, 0, sizeof(*x));
    x->pred = -1;
    for (int i = 0; i < 3; i++) x->s[i].bank = RB_NONE;
    x->d.bank = RB_NONE;
    return x;
}

static void fail(Dec *d, const char *fmt, ...) {
    if (d->err[0]) return;
    va_list a;
    va_start(a, fmt);
    vsnprintf(d->err, sizeof(d->err), fmt, a);
    va_end(a);
}

#define F(x, lo, w) ((uint32_t)(((x) >> (lo)) & ((1ull << (w)) - 1)))
#define B1(x, lo) F(x, lo, 1)

static const char *const SWZ4[32] = {
    "xxxx", "yyyy", "zzzz", "wwww", "xyzw", "yzww", "xxyz", "xyzz", "xyxy", "xywz", "zxyw", "zwzw", "yzxw", "xxyy", "xzww", "xyz1",
    NULL, NULL, NULL, "yyyy", "wyzw", NULL, "0000", "1111", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

static int set_sel(int8_t sel[4], const char *s) {
    if (!s) return 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        sel[i] = c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : c == 'w' ? 3 : c == '0' ? 4 : c == '1' ? 5 : c == '2' ? 6 : c == 'h' ? 7 : -1;
    }
    return 1;
}

static int pred3(uint64_t x) {
    switch (F(x, 56, 3)) {
        case 0: return -1;
        case 1: return 0; case 2: return 1; case 3: return 2; case 4: return 3;
        case 5: return 8 | 0; case 6: return 8 | 1;
        default: return -2;
    }
}
static int pred2(uint32_t v) { return v == 0 ? -1 : v == 1 ? 0 : v == 2 ? 1 : 8 | 0; }

/* Banco con índice de 6 bits en unidades de 64 bits (r60-63 = internos). */
static void op6(Dec *d, UOp *o, int ext, int bank, uint32_t n) {
    static const uint8_t m[2][4] = { { RB_R, RB_O, RB_PA, RB_SA }, { RB_I, RB_C, RB_IMM, RB_NONE } };
    o->bank = m[ext & 1][bank & 3];
    if (d->sec && o->bank == RB_PA) o->bank = RB_SA;
    if (o->bank == RB_R && n >= 60) { o->bank = RB_I; o->idx = (uint16_t)(n - 60); return; }
    if (o->bank == RB_C || o->bank == RB_I || o->bank == RB_IMM) { o->idx = (uint16_t)n; return; }
    o->idx = (uint16_t)(n * 2);
}
/* Índice de 7 bits en palabras; internos como 124 + k. */
static void op7(Dec *d, UOp *o, int ext, int bank, uint32_t n) {
    static const uint8_t m[2][4] = { { RB_R, RB_O, RB_PA, RB_SA }, { RB_I, RB_C, RB_IMM, RB_NONE } };
    o->bank = m[ext & 1][bank & 3];
    if (d->sec && o->bank == RB_PA) o->bank = RB_SA;
    if (o->bank == RB_R && n >= 124) { o->bank = RB_I; o->idx = (uint16_t)(n - 124); return; }
    o->idx = (uint16_t)n;
}
/* Índice de 7 bits en palabras con internos como 120 + 2k (VCOMP). */
static void op7b(Dec *d, UOp *o, int ext, int bank, uint32_t n, int *lane_off) {
    static const uint8_t m[2][4] = { { RB_R, RB_O, RB_PA, RB_SA }, { RB_I, RB_C, RB_IMM, RB_NONE } };
    o->bank = m[ext & 1][bank & 3];
    if (d->sec && o->bank == RB_PA) o->bank = RB_SA;
    *lane_off = 0;
    if (o->bank == RB_R && n >= 120) { o->bank = RB_I; o->idx = (uint16_t)((n - 120) >> 1); *lane_off = (n & 1) ? 1 : 0; return; }
    o->idx = (uint16_t)n;
}

static uint32_t f16_mask(uint32_t mask, int bank) {
    if (bank == RB_I) return mask;
    return (mask & 1 ? 3u : 0u) | (mask & 4 ? 12u : 0u);
}

/* Desplazamiento del registro en la repetición r (SMLSI: incremento o swizzle). */
static int inc_off(Dec *d, int slot, int r, int unit) {
    if (d->is_swz[slot]) return d->swz_inc[slot][r & 3] * unit;
    return r * d->inc[slot] * unit;
}

/* --- grupos de instrucciones -------------------------------------------- */

static void dec_vmad(Dec *d, uint64_t x) {
    int pred = pred3(x);
    uint32_t mask = F(x, 39, 4);
    UOp dst, g0, g1, s1;
    memset(&dst, 0, sizeof(dst)); memset(&g0, 0, sizeof(g0)); memset(&g1, 0, sizeof(g1)); memset(&s1, 0, sizeof(s1));
    op6(d, &dst, 0, F(x, 32, 2), F(x, 22, 6));
    g0.bank = RB_I; g0.idx = (uint16_t)F(x, 28, 2); g0.abs = (uint8_t)B1(x, 46);
    g1.bank = RB_I; g1.idx = (uint16_t)F(x, 12, 2); g1.neg = (uint8_t)B1(x, 36); g1.abs = (uint8_t)B1(x, 35);
    op6(d, &s1, B1(x, 49), F(x, 30, 2), F(x, 0, 6));
    s1.neg = (uint8_t)B1(x, 38); s1.abs = (uint8_t)B1(x, 37);
    if (B1(x, 53)) {   /* VMAD: dest = gpi0 * src1 + gpi1 */
        if (!set_sel(g0.sel, SWZ4[F(x, 18, 4) | (B1(x, 34) << 4)]) || !set_sel(g1.sel, SWZ4[F(x, 14, 4) | (B1(x, 54) << 4)]) ||
            !set_sel(s1.sel, SWZ4[F(x, 6, 4) | (B1(x, 10) << 4)])) { fail(d, "VMAD swizzle"); return; }
        int rep = (int)F(x, 44, 2) + 1;
        for (int r = 0; r < rep; r++) {
            UInsn *i = push(d);
            i->kind = UI_MAD; i->pred = (int8_t)pred; i->mask = (uint8_t)mask;
            i->d = dst; i->s[0] = g0; i->s[1] = s1; i->s[2] = g1;
            {   /* también en r = 0: en modo swizzle de SMLSI el primer desplazamiento no es 0 */
                if (dst.bank != RB_I) i->d.idx = (uint16_t)(dst.idx + inc_off(d, 0, r, 2));
                if (s1.bank != RB_I && s1.bank != RB_C) i->s[1].idx = (uint16_t)(s1.idx + inc_off(d, 2, r, 2));
            }
        }
        return;
    }
    /* VDP: dot(src1, gpi0) en 3 o 4 componentes */
    int vec4 = (int)B1(x, 52);
    uint32_t l[4] = { F(x, 6, 3), F(x, 9, 3), F(x, 12, 3), B1(x, 15) | (F(x, 16, 2) << 1) };
    for (int k = 0; k < 4; k++) s1.sel[k] = (int8_t)(l[k] < 4 ? l[k] : l[k]);
    uint32_t gc = F(x, 18, 4);
    const char *gs = gc == 4 ? "xyzw" : gc == 15 ? (vec4 ? "xyz1" : "xy1w") : NULL;
    if (!set_sel(g0.sel, gs)) { fail(d, "VDP gpi swizzle %u", gc); return; }
    int rep = (int)F(x, 44, 2) + 1, step = B1(x, 48) ? 8 : 4;
    int lane0 = 0;
    for (int k = 0; k < 4; k++) if (mask >> k & 1) { lane0 = k; break; }
    for (int r = 0; r < rep; r++) {
        UInsn *i = push(d);
        i->kind = UI_DP; i->pred = (int8_t)pred; i->n = (uint8_t)(vec4 ? 4 : 3);
        i->d = dst; i->mask = (uint8_t)(1u << ((lane0 + r) & 3));
        i->s[0] = s1; i->s[0].idx = (uint16_t)(s1.idx + r * step); i->s[1] = g0;
    }
}

static void dec_vnmad(Dec *d, uint64_t x, int f16) {
    UInsn *i = push(d);
    i->pred = (int8_t)pred3(x);
    i->f16 = (uint8_t)f16;
    i->mask = (uint8_t)F(x, 39, 4);
    uint32_t sw = F(x, 15, 7) | (F(x, 34, 2) << 7) | (B1(x, 50) << 9) | (F(x, 53, 2) << 10);
    op6(d, &i->d, B1(x, 51), F(x, 32, 2), F(x, 22, 6));
    op6(d, &i->s[0], B1(x, 49), F(x, 30, 2), F(x, 6, 6));
    op6(d, &i->s[1], B1(x, 48), F(x, 28, 2), F(x, 0, 6));
    for (int k = 0; k < 4; k++) i->s[0].sel[k] = (int8_t)((sw >> (3 * k)) & 7);
    i->s[0].neg = (uint8_t)B1(x, 37); i->s[0].abs = (uint8_t)B1(x, 38);
    i->s[1].abs = (uint8_t)B1(x, 36);
    if (!set_sel(i->s[1].sel, SWZ4[F(x, 44, 4)])) fail(d, "VNMAD swizzle");
    i->sub = (uint8_t)F(x, 12, 3);
    i->kind = i->sub == A_DP ? UI_DP : UI_ALU2;
    if (i->kind == UI_DP) i->n = 4;
}

static void dec_vmad2(Dec *d, uint64_t x) {
    static const char *const T0[8] = { "xxxx", "yyyy", "zzzz", "wwww", "xyzw", "yzxw", NULL, NULL };
    static const char *const T1[8] = { "xxxx", "yyyy", "zzzz", "wwww", "xyzw", NULL, NULL, "wyzw" };
    static const char *const T2[8] = { "xxxx", "yyyy", "zzzz", "wwww", "xyzw", NULL, "xxyz", NULL };
    UInsn *i = push(d);
    i->kind = UI_MAD;
    i->f16 = (uint8_t)B1(x, 58);
    i->pred = (int8_t)pred2(F(x, 56, 2));
    op6(d, &i->d, 0, F(x, 32, 2), F(x, 22, 6));
    op6(d, &i->s[0], 0, B1(x, 34) ? 2 : 0, F(x, 12, 6));
    op6(d, &i->s[1], B1(x, 49), F(x, 30, 2), F(x, 6, 6));
    op6(d, &i->s[2], B1(x, 48), F(x, 28, 2), F(x, 0, 6));
    i->s[0].abs = (uint8_t)B1(x, 50);
    i->s[1].neg = (uint8_t)B1(x, 37); i->s[1].abs = (uint8_t)B1(x, 38);
    i->s[2].neg = (uint8_t)B1(x, 35); i->s[2].abs = (uint8_t)B1(x, 36);
    if (!set_sel(i->s[0].sel, T0[F(x, 18, 2) | (B1(x, 53) << 2)]) || !set_sel(i->s[1].sel, T1[F(x, 20, 2) | (B1(x, 44) << 2)]) ||
        !set_sel(i->s[2].sel, T2[F(x, 45, 3)])) { fail(d, "VMAD2 swizzle"); return; }
    i->mask = (uint8_t)F(x, 39, 4);
    if (i->f16) i->mask = (uint8_t)f16_mask(i->mask, i->d.bank);
}

static void dec_vpck(Dec *d, uint64_t x) {
    UInsn *i = push(d);
    i->kind = UI_PCK;
    i->pred = (int8_t)pred3(x);
    i->sfmt = (uint8_t)F(x, 41, 3); i->dfmt = (uint8_t)F(x, 38, 3);
    i->scale = (uint8_t)B1(x, 18);
    i->mask = (uint8_t)F(x, 34, 4);
    op7(d, &i->d, B1(x, 51), F(x, 32, 2), F(x, 21, 7));
    if (i->sfmt == F_F32) {
        int8_t sel[4] = { (int8_t)(B1(x, 0) | (B1(x, 7) << 1)), (int8_t)F(x, 16, 2), (int8_t)F(x, 14, 2), (int8_t)F(x, 19, 2) };
        memcpy(i->s[0].sel, sel, 4);
        op6(d, &i->s[0], B1(x, 49), F(x, 30, 2), F(x, 8, 6));
        op6(d, &i->s[1], B1(x, 48), F(x, 28, 2), F(x, 1, 6));
    } else {
        int8_t sel[4] = { (int8_t)F(x, 0, 2), (int8_t)F(x, 16, 2), (int8_t)F(x, 14, 2), (int8_t)F(x, 19, 2) };
        memcpy(i->s[0].sel, sel, 4);
        op6(d, &i->s[0], B1(x, 49), F(x, 30, 2), F(x, 7, 7) >> 1);
        if (i->s[0].bank != RB_I && i->s[0].bank != RB_C && i->s[0].bank != RB_IMM) i->s[0].idx = (uint16_t)F(x, 7, 7);
    }
    int rep = (int)F(x, 44, 2) + 1;
    if (rep > 1) fail(d, "VPCK con repeticiones");
}

static void dec_ldr(Dec *d, uint64_t x) {
    UInsn *i = push(d);
    i->kind = UI_LDR;
    i->pred = (int8_t)pred3(x);
    op7(d, &i->d, 0, B1(x, 39) ? 2 : 0, F(x, 21, 7));
    op7(d, &i->s[0], 0, B1(x, 50) ? 1 : 2, F(x, 14, 7));
    i->off = (uint16_t)(F(x, 7, 7) * 4);
    i->count = (uint8_t)(B1(x, 53) ? 1 : F(x, 44, 4) + 1);
}

static void dec_bitwise(Dec *d, uint64_t x) {
    int rep = (int)F(x, 44, 3) + 1;
    for (int r = 0; r < rep; r++) {
        UInsn *i = push(d);
        i->kind = B1(x, 35) ? UI_OR : UI_AND;
        i->pred = (int8_t)pred3(x);
        op7(d, &i->d, 0, F(x, 32, 2), F(x, 21, 7) + inc_off(d, 0, r, 1));
        op7(d, &i->s[0], 0, F(x, 30, 2), F(x, 7, 7) + inc_off(d, 2, r, 1));
        if (B1(x, 48)) { i->s[1].bank = RB_IMM; i->s[1].idx = (uint16_t)F(x, 0, 7); }
        else op7(d, &i->s[1], 0, F(x, 28, 2), F(x, 0, 7) + inc_off(d, 3, r, 1));
    }
}

static void dec_vmov(Dec *d, uint64_t x) {
    uint32_t dt = F(x, 40, 3);
    if (dt != 4 && dt != 5) { fail(d, "VMOV tipo %u", dt); return; }
    int f16 = dt == 4;
    int cond = B1(x, 46);
    int rep = (int)F(x, 44, 2) + 1;
    for (int r = 0; r < rep; r++) {
        UInsn *i = push(d);
        i->kind = cond ? UI_MOVC : UI_MOV;
        i->f16 = (uint8_t)f16;
        i->pred = (int8_t)pred3(x);
        op6(d, &i->d, 0, F(x, 32, 2), F(x, 18, 6));
        uint32_t mask = F(x, 24, 4);
        i->mask = (uint8_t)(f16 ? f16_mask(mask, i->d.bank) : mask);
        const char *sw = SWZ4[F(x, 35, 4)];
        if (!cond) {
            op6(d, &i->s[0], B1(x, 49), F(x, 30, 2), F(x, 6, 6));
            if (!set_sel(i->s[0].sel, sw)) { fail(d, "VMOV swizzle"); return; }
            {   /* también en r = 0: en modo swizzle de SMLSI el primer desplazamiento no es 0 */
                if (i->d.bank != RB_I) i->d.idx = (uint16_t)(i->d.idx + inc_off(d, 0, r, 2));
                if (i->s[0].bank != RB_I && i->s[0].bank != RB_C) i->s[0].idx = (uint16_t)(i->s[0].idx + inc_off(d, 2, r, 2));
            }
        } else {
            if (rep > 1) { fail(d, "VMOVC con repeticiones"); return; }
            op6(d, &i->s[0], 0, B1(x, 34) ? 2 : 0, F(x, 12, 6));
            op6(d, &i->s[1], B1(x, 49), F(x, 30, 2), F(x, 6, 6));
            op6(d, &i->s[2], B1(x, 48), F(x, 28, 2), F(x, 0, 6));
            set_sel(i->s[0].sel, "xyzw");
            if (!set_sel(i->s[1].sel, sw) || !set_sel(i->s[2].sel, sw)) { fail(d, "VMOVC swizzle"); return; }
            i->sub = (uint8_t)(B1(x, 39) | (B1(x, 54) << 1));   /* 0 ==, 1 !=, 2 <, 3 <= */
        }
    }
}

static void dec_vcomp(Dec *d, uint64_t x) {
    UInsn *i = push(d);
    i->kind = UI_COMP;
    i->pred = (int8_t)pred3(x);
    i->sub = (uint8_t)F(x, 41, 2);
    int lo;
    op7b(d, &i->d, 0, F(x, 32, 2), F(x, 20, 7), &lo);
    int dl = (int)F(x, 1, 2) + lo;
    op7b(d, &i->s[0], B1(x, 49), F(x, 30, 2), F(x, 6, 7), &lo);
    int sl = (int)F(x, 35, 2) + lo;
    i->mask = (uint8_t)(1u << (dl & 3));
    for (int k = 0; k < 4; k++) i->s[0].sel[k] = (int8_t)(sl & 3);
    i->s[0].neg = (uint8_t)B1(x, 37); i->s[0].abs = (uint8_t)B1(x, 38);
}

static void dec_test(Dec *d, uint64_t x) {
    UInsn *i = push(d);
    i->kind = UI_TEST;
    i->pred = (int8_t)pred3(x);
    i->tsign = (uint8_t)F(x, 42, 2); i->tzero = (uint8_t)F(x, 40, 2); i->tand = (uint8_t)B1(x, 39);
    i->tpdst = (uint8_t)F(x, 34, 2); i->twben = (uint8_t)B1(x, 20);
    i->top = (uint8_t)F(x, 14, 4);
    uint32_t asel = F(x, 18, 2);
    i->tint = (uint8_t)(asel != 0);
    i->f16 = (uint8_t)(!i->tint && !B1(x, 47));
    int lane = (int)F(x, 36, 2);
    if (x >> 59 == 15) {   /* CMPMSK: máscara 1.0/0.0 por carril del vector src1 contra el escalar src2 */
        i->kind = UI_NOP;
        UInsn *m = i;
        m->kind = UI_TEST;
        m->twben = 2;
        op7(d, &m->d, B1(x, 52), F(x, 32, 2), F(x, 21, 7));
        for (int k = 0; k < 2; k++) {
            UOp *o = &m->s[k];
            uint32_t n = k ? F(x, 0, 7) : F(x, 7, 7);
            op7(d, o, (int)B1(x, 49 - k), (int)F(x, 30 - 2 * k, 2), n);
            if (o->bank != RB_I && o->bank != RB_C && o->bank != RB_IMM) o->idx = (uint16_t)(n * 2);
        }
        set_sel(m->s[0].sel, "xyzw");
        set_sel(m->s[1].sel, "xxxx");
        m->mask = 15;
        return;
    }
    if (i->twben) { fail(d, "TEST con escritura"); return; }
    if (i->tint) {
        op7(d, &i->s[0], B1(x, 49), F(x, 30, 2), F(x, 7, 7));
        op7(d, &i->s[1], B1(x, 48), F(x, 28, 2), F(x, 0, 7));
        lane = 0;
    } else {
        for (int k = 0; k < 2; k++) {
            UOp *o = &i->s[k];
            int ext = (int)B1(x, 49 - k), bank = (int)F(x, 30 - 2 * k, 2);
            uint32_t n = k ? F(x, 0, 7) : F(x, 7, 7);
            op7(d, o, ext, bank, n);
            if (o->bank != RB_I && o->bank != RB_C && o->bank != RB_IMM) o->idx = (uint16_t)(n * 2);
        }
    }
    for (int k = 0; k < 4; k++) { i->s[0].sel[k] = (int8_t)lane; i->s[1].sel[k] = (int8_t)lane; }
    if (i->s[1].bank == RB_C) for (int k = 0; k < 4; k++) i->s[1].sel[k] = 0;
    i->s[0].neg = (uint8_t)(i->top == 10 && B1(x, 51));
    if (!i->tint && i->top != 14 && i->top != 10 && i->top != 13) fail(d, "TEST op %u", i->top);
    if (i->tint && i->top != 8) fail(d, "TEST entero op %u", i->top);
}

static void dec_smp(Dec *d, uint64_t x) {
    UInsn *i = push(d);
    i->kind = UI_SMP;
    i->pred = (int8_t)pred3(x);
    i->dim = (uint8_t)(F(x, 42, 2) + 1);
    i->cf16 = (uint8_t)B1(x, 35);
    i->gather = (uint8_t)B1(x, 46);
    i->df32 = (uint8_t)(B1(x, 46) || B1(x, 37));
    i->d.bank = (uint8_t)(B1(x, 41) ? RB_SA : B1(x, 39) ? RB_PA : RB_R);
    if (d->sec && i->d.bank == RB_PA) i->d.bank = RB_SA;
    i->d.idx = (uint16_t)F(x, 21, 7);
    if (i->d.bank == RB_R && i->d.idx >= 124) { i->d.bank = RB_I; i->d.idx = (uint16_t)(i->d.idx - 124); }
    op6(d, &i->s[0], 0, B1(x, 34) ? 2 : 0, F(x, 14, 6));
    uint32_t sa = F(x, 7, 7) * 2;
    int u = sa < 256 ? d->samp_unit[sa] : -1;
    if (u < 0) { fail(d, "SMP sin sampler (sa%u)", sa); return; }
    i->unit = (uint8_t)u;
    if (i->gather && i->dim != 2) fail(d, "SMP gather4 %uD", i->dim);
}

static void dec_imad2(Dec *d, uint64_t x) {
    UInsn *i = push(d);
    i->kind = UI_IMAD;
    i->pred = (int8_t)pred2(F(x, 57, 2));
    op7(d, &i->d, 0, F(x, 32, 2), F(x, 21, 7));
    op7(d, &i->s[0], 0, B1(x, 34) ? 2 : 0, F(x, 14, 7));
    op7(d, &i->s[1], B1(x, 49), F(x, 30, 2), F(x, 7, 7));
    op7(d, &i->s[2], B1(x, 48), F(x, 28, 2), F(x, 0, 7));
}

static void dec_special(Dec *d, uint64_t x) {
    uint32_t a = F(x, 56, 3), b = F(x, 52, 4);
    if (a == 2 && b == 1) {   /* SMLSI */
        static const int lo[4] = { 24, 16, 8, 0 }, mb[4] = { 35, 34, 33, 32 };
        for (int k = 0; k < 4; k++) {
            uint32_t v = F(x, lo[k], 8);
            if (B1(x, mb[k])) {
                d->is_swz[k] = 1;
                for (int q = 0; q < 4; q++) d->swz_inc[k][q] = (int)((v >> (2 * q)) & 3);
            } else {
                d->is_swz[k] = 0;
                d->inc[k] = v >= 128 ? (int)v - 256 : (int)v;
            }
        }
        return;
    }
    if (a == 1 && b == 3) {   /* KILL */
        UInsn *i = push(d);
        i->kind = UI_KILL;
        i->pred = (int8_t)((int)F(x, 32, 2) - 1);
        return;
    }
    if ((a == 2 && b == 4) || a == 0 || b == 2 || (a == 1 && b == 0)) return;   /* PHAS, NOP, SPEC; 1/0 no sale en el .dsm de Vita3K */
    fail(d, "especial %u/%u (0x%016llX)", a, b, (unsigned long long)x);
}


/* --- doble emisión ------------------------------------------------------- */
/* Dos operaciones a la vez sobre los internos (gpi0-2) y una ranura del almacén
 * unificado (us). Formato y semántica reconstruidos con el desensamblado y el
 * SPIR-V de Vita3K: los operandos us son F16 en los programas de fragmento y
 * F32 en los de vértice; su índice va en unidades de 64 bits en las operaciones
 * vectoriales y de 32 en las escalares; el destino us escribe 64 bits (2 F32 o
 * 4 F16) salvo si es un registro interno. */
static const char *const DSWZ3[32] = {
    "xxxx", "yyyx", "zzzx", "wwwx", "xyzx", "yzwx", NULL, "yzxx", NULL, NULL, "zxyx", "xz??", "0000", NULL, "1111", NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, "yzxx", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
static const int DUAL_NSRC[14] = { 3, 2, 1, 2, 2, 1, 1, 1, 3, 2, 2, 2, 1, 1 };

static int dual_scalar_result(int op) { return op == D_DP || op == D_SSQ || op >= D_RSQ; }
static int dual_vector_src(int op) { return op <= D_MOV; }

static void dec_dual(Dec *d, uint64_t x) {
    int cct = (int)B1(x, 59);
    int op1 = (int)(F(x, 44, 3) | (cct ? 0u : (B1(x, 54) << 3)));
    int op2 = (int)(F(x, 18, 3) | (B1(x, 43) << 3));
    if (op1 > D_FLOG || op2 > D_FLOG) { fail(d, "doble emisión op %d/%d", op1, op2); return; }
    int n1 = DUAL_NSRC[op1], n2 = DUAL_NSRC[op2];
    int sc = (int)F(x, 16, 2);
    /* ranuras de fuente: 0 = g0, 1 = g1, 2 = g2, 3 = us (tabla del corpus) */
    static const struct { int sc, n1, n2; int8_t a[3], b[2]; } T[] = {
        { 0, 1, 1, { 3 }, { 0 } }, { 0, 1, 2, { 3 }, { 0, 1 } }, { 0, 2, 1, { 3, 1 }, { 0 } }, { 0, 2, 2, { 3, 1 }, { 0, 2 } },
        { 0, 3, 1, { 3, 1, 2 }, { 0 } }, { 1, 1, 1, { 0 }, { 3 } }, { 1, 1, 2, { 0 }, { 3, 1 } }, { 1, 2, 1, { 0, 3 }, { 1 } },
        { 1, 2, 2, { 0, 3 }, { 1, 2 } }, { 1, 3, 1, { 0, 3, 2 }, { 1 } }, { 2, 1, 2, { 1 }, { 0, 3 } }, { 2, 2, 1, { 0, 1 }, { 3 } },
        { 2, 2, 2, { 0, 1 }, { 2, 3 } }, { 2, 3, 1, { 0, 1, 3 }, { 2 } }, { 3, 1, 2, { 2 }, { 1, 3 } }, { 3, 3, 1, { 0, 1, 2 }, { 3 } },
    };
    int t = -1;
    for (int k = 0; k < (int)(sizeof(T) / sizeof(T[0])); k++) if (T[k].sc == sc && T[k].n1 == n1 && T[k].n2 == n2) { t = k; break; }
    if (t < 0) { fail(d, "doble emisión fuentes sc=%d %d/%d", sc, n1, n2); return; }
    int g2used = 0;
    for (int k = 0; k < n1; k++) if (T[t].a[k] == 2) g2used = 1;
    for (int k = 0; k < n2; k++) if (T[t].b[k] == 2) g2used = 1;
    int nvec = cct ? 4 : 3;
    UOp slot[4];
    memset(slot, 0, sizeof(slot));
    const char *const *tab = cct ? SWZ4 : DSWZ3;
    slot[0].bank = RB_I; slot[0].idx = (uint16_t)F(x, 10, 2);
    slot[1].bank = RB_I; slot[1].idx = (uint16_t)F(x, 12, 2); slot[1].neg = (uint8_t)B1(x, 58);
    slot[2].bank = RB_I; slot[2].idx = (uint16_t)(g2used ? (B1(x, 15) << 1) | B1(x, 14) : B1(x, 15) << 1);
    set_sel(slot[2].sel, "xyzw");
    if (!set_sel(slot[0].sel, tab[F(x, 38, 4)])) { fail(d, "doble emisión swizzle gpi0 %u", F(x, 38, 4)); return; }
    if (!set_sel(slot[1].sel, (cct ? SWZ4 : DSWZ3)[F(x, 34, 4) | (B1(x, 52) << 4)])) { fail(d, "doble emisión swizzle gpi1"); return; }
    if (!set_sel(slot[3].sel, tab[F(x, 48, 4)])) { fail(d, "doble emisión swizzle us %u", F(x, 48, 4)); return; }
    slot[3].neg = (uint8_t)B1(x, 47);
    slot[3].abs = (uint8_t)(!g2used && B1(x, 14));
    int pu = (int)B1(x, 42);
    for (int q = 0; q < 2; q++) {
        int op = q ? op2 : op1, ns = q ? n2 : n1;
        const int8_t *sl = q ? T[t].b : T[t].a;
        UInsn *i = push(d);
        i->kind = (uint8_t)(q ? UI_DUAL2 : UI_DUAL1);
        i->sub = (uint8_t)op;
        i->n = (uint8_t)nvec;
        i->pred = (int8_t)pred2(F(x, 56, 2));
        for (int k = 0; k < ns; k++) {
            i->s[k] = slot[sl[k]];
            if (sl[k] == 3) {   /* us: banco, índice y formato según la operación */
                uint32_t n = F(x, 0, 7);
                UOp *o = &i->s[k];
                int bank = (int)F(x, 30, 2);
                static const uint8_t m[4] = { RB_R, RB_O, RB_PA, RB_SA };
                o->bank = m[bank];
                if (d->sec && o->bank == RB_PA) o->bank = RB_SA;
                if (o->bank == RB_R && n >= 124) { o->bank = RB_I; o->idx = (uint16_t)(n - 124); }
                else { o->idx = (uint16_t)(dual_vector_src(op) ? n * 2 : n); o->h = (uint8_t)d->frag; }
            }
        }
        int to_us = q ? !pu : pu;
        if (to_us) {
            uint32_t n = F(x, 21, 7);
            static const uint8_t m[4] = { RB_R, RB_O, RB_PA, RB_SA };
            i->d.bank = m[F(x, 32, 2)];
            if (d->sec && i->d.bank == RB_PA) i->d.bank = RB_SA;
            if (i->d.bank == RB_R && n >= 124) {
                i->d.bank = RB_I; i->d.idx = (uint16_t)(n - 124);
                i->mask = (uint8_t)(cct ? 15 : 7);
            } else {
                int sca = dual_scalar_result(op);
                i->d.idx = (uint16_t)(sca ? n : n * 2);
                i->d.h = (uint8_t)d->frag;
                i->mask = (uint8_t)(d->frag ? (sca ? 3 : 15) : (sca ? 1 : 3));
            }
        } else {
            i->d.bank = RB_I; i->d.idx = (uint16_t)F(x, 28, 2);
            i->mask = (uint8_t)(F(x, 7, 3) | (cct && B1(x, 54) ? 8u : 0u));
        }
    }
}

static void decode(Dec *d, uint64_t x) {
    switch ((uint32_t)(x >> 59)) {
        case 0:  dec_vmad2(d, x); break;
        case 1:  dec_vnmad(d, x, 0); break;
        case 2:  dec_vnmad(d, x, 1); break;
        case 3:  dec_vmad(d, x); break;
        case 6:  dec_vcomp(d, x); break;
        case 7:  dec_vmov(d, x); break;
        case 8:  dec_vpck(d, x); break;
        case 9:  case 15: dec_test(d, x); break;
        case 10: dec_bitwise(d, x); break;
        case 21: dec_imad2(d, x); break;
        case 28: dec_smp(d, x); break;
        case 29: dec_ldr(d, x); break;
        case 31: dec_special(d, x); break;
        case 4: case 5: dec_dual(d, x); break;
        default: fail(d, "opcode %u", (uint32_t)(x >> 59)); break;
    }
}

/* ========================================================================== */
/* GLSL                                                                        */
/* ========================================================================== */

typedef struct { char *p; size_t n, cap; } Str;

static void sp(Str *s, const char *fmt, ...) {
    va_list a;
    for (;;) {
        size_t room = s->cap - s->n;
        va_start(a, fmt);
        int k = vsnprintf(s->p ? s->p + s->n : NULL, s->p ? room : 0, fmt, a);
        va_end(a);
        if (k < 0) return;
        if (s->p && (size_t)k < room) { s->n += (size_t)k; return; }
        s->cap = (s->cap + (size_t)k + 1) * 2;
        s->p = (char *)realloc(s->p, s->cap);
    }
}

/* Constantes del banco c (tabla fija de la USSE). Se van comprobando contra el
 * SPIR-V de Vita3K; las que no se conocen valen 0. */
static float cbank(int idx, int sel, int f16) {
    /* Valores comprobados en el SPIR-V de Vita3K (los únicos que usa el juego). */
    if (idx == 0) return 0.0f;
    if (!f16) {
        if (idx == 1 && sel == 1) return 1.0f;
        if (idx == 4 && sel == 0) return 2.0f;
        if (idx == 12 && sel == 0) return 0.5f;
    } else {
        if (idx == 15 && sel == 1) return 1.0f;
        if (idx == 34 && sel == 0) return 1.0f;
    }
    return 1.0f;   /* sin comprobar */
}

static const char *bank_arr(int b) {
    switch (b) { case RB_R: return "R"; case RB_O: return "O"; case RB_PA: return "PA"; case RB_SA: return "SA"; default: return "?"; }
}

/* Expresión float del carril `lane` del operando. */
static void src_expr(Str *s, const UOp *o, int lane, int f16) {
    int sel = o->sel[lane];
    char tmp[96];
    if (sel < 0) sel = 0;
    if (sel >= 4) {
        static const char *const K[4] = { "0.0", "1.0", "2.0", "0.5" };
        snprintf(tmp, sizeof(tmp), "%s", K[sel - 4]);
    } else if (o->bank == RB_I) snprintf(tmp, sizeof(tmp), "I[%u][%d]", o->idx & 3, sel);
    else if (o->bank == RB_C) snprintf(tmp, sizeof(tmp), "%#.9g", (double)cbank(o->idx, sel, f16 || o->h));
    else if (o->bank == RB_IMM) snprintf(tmp, sizeof(tmp), "%u.0", o->idx);
    else if (f16 || o->h) snprintf(tmp, sizeof(tmp), "unpackHalf2x16(%s[%u])[%d]", bank_arr(o->bank), o->idx + (unsigned)(sel >> 1), sel & 1);
    else snprintf(tmp, sizeof(tmp), "uintBitsToFloat(%s[%u])", bank_arr(o->bank), o->idx + (unsigned)sel);
    if (o->abs && o->neg) sp(s, "(-abs(%s))", tmp);
    else if (o->abs) sp(s, "abs(%s)", tmp);
    else if (o->neg) sp(s, "(-%s)", tmp);
    else sp(s, "%s", tmp);
}

/* Escribe el valor float `v` (nombre de variable GLSL) en el carril `lane`. */
static void dst_write(Str *s, const UOp *o, int lane, int f16, const char *v) {
    if (o->bank == RB_I) { sp(s, "I[%u][%d] = %s; ", o->idx & 3, lane, v); return; }
    const char *a = bank_arr(o->bank);
    if (f16 || o->h) {
        unsigned w = o->idx + (unsigned)(lane >> 1);
        sp(s, "%s[%u] = wh(%s[%u], %d, %s); ", a, w, a, w, lane & 1, v);
    } else sp(s, "%s[%u] = floatBitsToUint(%s); ", a, o->idx + (unsigned)lane, v);
}

static void emit_pred_open(Str *s, int pred) {
    if (pred < 0) return;
    sp(s, "if (%sP[%d]) { ", (pred & 8) ? "!" : "", pred & 3);
}
static void emit_pred_close(Str *s, int pred) { if (pred >= 0) sp(s, "}"); }

static void emit_insn(Str *s, const UInsn *i, int is_frag, char *err, size_t errlen) {
    sp(s, "    ");
    emit_pred_open(s, i->pred);
    switch (i->kind) {
        case UI_MAD: case UI_ALU2: {
            sp(s, "{ ");
            for (int l = 0; l < 4; l++) {
                if (!(i->mask >> l & 1)) continue;
                sp(s, "float t%d = ", l);
                if (i->kind == UI_MAD) {
                    sp(s, "fma("); src_expr(s, &i->s[0], l, i->f16); sp(s, ", "); src_expr(s, &i->s[1], l, i->f16);
                    sp(s, ", "); src_expr(s, &i->s[2], l, i->f16); sp(s, ")");
                } else {
                    switch (i->sub) {
                        case A_MUL: src_expr(s, &i->s[0], l, i->f16); sp(s, " * "); src_expr(s, &i->s[1], l, i->f16); break;
                        case A_ADD: src_expr(s, &i->s[0], l, i->f16); sp(s, " + "); src_expr(s, &i->s[1], l, i->f16); break;
                        case A_FRC: sp(s, "fract("); src_expr(s, &i->s[0], l, i->f16); sp(s, ")"); break;
                        case A_DSX: sp(s, is_frag ? "dFdx(" : "(0.0*"); src_expr(s, &i->s[0], l, i->f16); sp(s, ")"); break;
                        case A_DSY: sp(s, is_frag ? "dFdy(" : "(0.0*"); src_expr(s, &i->s[0], l, i->f16); sp(s, ")"); break;
                        case A_MIN: sp(s, "min("); src_expr(s, &i->s[0], l, i->f16); sp(s, ", "); src_expr(s, &i->s[1], l, i->f16); sp(s, ")"); break;
                        case A_MAX: sp(s, "max("); src_expr(s, &i->s[0], l, i->f16); sp(s, ", "); src_expr(s, &i->s[1], l, i->f16); sp(s, ")"); break;
                    }
                }
                sp(s, "; ");
            }
            for (int l = 0; l < 4; l++) {
                if (!(i->mask >> l & 1)) continue;
                char v[8]; snprintf(v, sizeof(v), "t%d", l);
                dst_write(s, &i->d, l, i->f16, v);
            }
            sp(s, "}");
            break;
        }
        case UI_DP: {
            sp(s, "{ float t = ");
            for (int k = 0; k < i->n; k++) {
                if (k) sp(s, " + ");
                src_expr(s, &i->s[0], k, i->f16); sp(s, " * "); src_expr(s, &i->s[1], k, i->f16);
            }
            sp(s, "; ");
            for (int l = 0; l < 4; l++) if (i->mask >> l & 1) dst_write(s, &i->d, l, i->f16, "t");
            sp(s, "}");
            break;
        }
        case UI_MOV: case UI_MOVC: {
            sp(s, "{ ");
            for (int l = 0; l < 4; l++) {
                if (!(i->mask >> l & 1)) continue;
                if (i->kind == UI_MOV) { sp(s, "float t%d = ", l); src_expr(s, &i->s[0], l, i->f16); sp(s, "; "); }
                else {
                    static const char *const C[4] = { "==", "!=", "<", "<=" };
                    sp(s, "float t%d = (", l); src_expr(s, &i->s[0], l, i->f16); sp(s, " %s 0.0) ? ", C[i->sub & 3]);
                    src_expr(s, &i->s[1], l, i->f16); sp(s, " : "); src_expr(s, &i->s[2], l, i->f16); sp(s, "; ");
                }
            }
            for (int l = 0; l < 4; l++) {
                if (!(i->mask >> l & 1)) continue;
                char v[8]; snprintf(v, sizeof(v), "t%d", l);
                dst_write(s, &i->d, l, i->f16, v);
            }
            sp(s, "}");
            break;
        }
        case UI_COMP: {
            int l = 0;
            while (l < 3 && !(i->mask >> l & 1)) l++;
            static const char *const FN[4] = { "1.0 / ", "inversesqrt", "log2", "exp2" };
            sp(s, "{ float t = %s(", FN[i->sub & 3]); src_expr(s, &i->s[0], l, 0); sp(s, "); ");
            dst_write(s, &i->d, l, 0, "t");
            sp(s, "}");
            break;
        }
        case UI_PCK: {
            /* origen: vector de 4 componentes según el formato */
            sp(s, "{ ");
            for (int l = 0; l < 4; l++) {
                if (!(i->mask >> l & 1)) continue;
                int sel = i->s[0].sel[l];
                char src[96];
                const UOp *o = &i->s[0];
                if (i->sfmt == F_F32) {
                    if (o->bank == RB_I) snprintf(src, sizeof(src), "I[%u][%d]", o->idx & 3, sel & 3);
                    else if (sel < 2) snprintf(src, sizeof(src), "uintBitsToFloat(%s[%u])", bank_arr(o->bank), o->idx + (unsigned)sel);
                    else {
                        const UOp *o2 = &i->s[1];
                        if (o2->bank == RB_I) snprintf(src, sizeof(src), "I[%u][%d]", o2->idx & 3, sel - 2);
                        else snprintf(src, sizeof(src), "uintBitsToFloat(%s[%u])", bank_arr(o2->bank), o2->idx + (unsigned)(sel - 2));
                    }
                } else if (i->sfmt == F_F16) {
                    if (o->bank == RB_I) snprintf(src, sizeof(src), "I[%u][%d]", o->idx & 3, sel & 3);
                    else snprintf(src, sizeof(src), "unpackHalf2x16(%s[%u])[%d]", bank_arr(o->bank), o->idx + (unsigned)(sel >> 1), sel & 1);
                } else if (i->sfmt == F_U8 || i->sfmt == F_S8) {
                    const char *t = i->sfmt == F_U8 ? "float((%s[%u] >> %du) & 0xFFu)" : "float(bitfieldExtract(int(%s[%u]), %d, 8))";
                    char t2[96]; snprintf(t2, sizeof(t2), t, bank_arr(o->bank), o->idx, 8 * (sel & 3));
                    if (i->scale) snprintf(src, sizeof(src), i->sfmt == F_U8 ? "(%s / 255.0)" : "max(%s / 127.0, -1.0)", t2);
                    else snprintf(src, sizeof(src), "%s", t2);
                } else if (i->sfmt == F_U16 || i->sfmt == F_S16) {
                    const char *t = i->sfmt == F_U16 ? "float((%s[%u] >> %du) & 0xFFFFu)" : "float(bitfieldExtract(int(%s[%u]), %d, 16))";
                    char t2[96]; snprintf(t2, sizeof(t2), t, bank_arr(o->bank), o->idx + (unsigned)((sel & 3) >> 1), 16 * (sel & 1));
                    if (i->scale) snprintf(src, sizeof(src), i->sfmt == F_U16 ? "(%s / 65535.0)" : "max(%s / 32767.0, -1.0)", t2);
                    else snprintf(src, sizeof(src), "%s", t2);
                } else { snprintf(err, errlen, "VPCK origen %u", i->sfmt); return; }
                sp(s, "float t%d = %s; ", l, src);
            }
            for (int l = 0; l < 4; l++) {
                if (!(i->mask >> l & 1)) continue;
                char v[8]; snprintf(v, sizeof(v), "t%d", l);
                const UOp *o = &i->d;
                if (i->dfmt == F_F32) dst_write(s, o, l, 0, v);
                else if (i->dfmt == F_F16) dst_write(s, o, l, 1, v);
                else if (o->bank == RB_I) dst_write(s, o, l, 0, v);
                else if (i->dfmt == F_U8 || i->dfmt == F_S8) {
                    const char *a = bank_arr(o->bank);
                    if (i->dfmt == F_U8)
                        sp(s, "%s[%u] = bitfieldInsert(%s[%u], uint(clamp(%s%s, 0.0, 255.0) + 0.5), %d, 8); ", a, o->idx, a, o->idx, v,
                           i->scale ? " * 255.0" : "", 8 * l);
                    else
                        sp(s, "%s[%u] = bitfieldInsert(%s[%u], uint(int(clamp(%s%s, -128.0, 127.0))), %d, 8); ", a, o->idx, a, o->idx, v,
                           i->scale ? " * 127.0" : "", 8 * l);
                } else if (i->dfmt == F_U16 || i->dfmt == F_S16) {
                    const char *a = bank_arr(o->bank);
                    unsigned w = o->idx + (unsigned)(l >> 1);
                    if (i->dfmt == F_U16)
                        sp(s, "%s[%u] = bitfieldInsert(%s[%u], uint(clamp(%s%s, 0.0, 65535.0)), %d, 16); ", a, w, a, w, v,
                           i->scale ? " * 65535.0" : "", 16 * (l & 1));
                    else
                        sp(s, "%s[%u] = bitfieldInsert(%s[%u], uint(int(clamp(%s%s, -32768.0, 32767.0))), %d, 16); ", a, w, a, w, v,
                           i->scale ? " * 32767.0" : "", 16 * (l & 1));
                } else { snprintf(err, errlen, "VPCK destino %u", i->dfmt); return; }
            }
            sp(s, "}");
            break;
        }
        case UI_LDR: {
            if (i->d.bank == RB_I || i->s[0].bank == RB_I) { snprintf(err, errlen, "LDR con internos"); return; }
            sp(s, "{ uint a = (%s[%u] + %uu) >> 2; ", bank_arr(i->s[0].bank), i->s[0].idx, i->off);
            for (int k = 0; k < i->count; k++) sp(s, "%s[%u] = ld(a + %du); ", bank_arr(i->d.bank), i->d.idx + (unsigned)k, k);
            sp(s, "}");
            break;
        }
        case UI_OR: case UI_AND: case UI_IMAD: {
            char a[3][48];
            for (int k = 0; k < 3; k++) {
                const UOp *o = &i->s[k];
                if (o->bank == RB_IMM) snprintf(a[k], sizeof(a[k]), "%uu", o->idx);
                else if (o->bank == RB_I) snprintf(a[k], sizeof(a[k]), "floatBitsToUint(I[%u][0])", o->idx & 3);
                else if (o->bank == RB_NONE) snprintf(a[k], sizeof(a[k]), "0u");
                else snprintf(a[k], sizeof(a[k]), "%s[%u]", bank_arr(o->bank), o->idx);
            }
            char v[160];
            if (i->kind == UI_OR) snprintf(v, sizeof(v), "(%s | %s)", a[0], a[1]);
            else if (i->kind == UI_AND) snprintf(v, sizeof(v), "(%s & %s)", a[0], a[1]);
            else snprintf(v, sizeof(v), "(%s * %s + %s)", a[0], a[1], a[2]);
            if (i->d.bank == RB_I) sp(s, "I[%u][0] = uintBitsToFloat(%s);", i->d.idx & 3, v);
            else sp(s, "%s[%u] = %s;", bank_arr(i->d.bank), i->d.idx, v);
            break;
        }
        case UI_TEST: {
            sp(s, "{ ");
            if (i->twben == 2) {   /* CMPMSK */
                const char *sg = i->tsign == 1 ? "t < 0.0" : i->tsign == 2 ? "t > 0.0" : (i->tand ? "true" : "false");
                const char *zr = i->tzero == 1 ? "t == 0.0" : i->tzero == 2 ? "t != 0.0" : (i->tand ? "true" : "false");
                for (int l = 0; l < 4; l++) {
                    sp(s, "{ float t = "); src_expr(s, &i->s[0], l, i->f16); sp(s, " - "); src_expr(s, &i->s[1], l, 0);
                    sp(s, "; float m = ((%s) %s (%s)) ? 1.0 : 0.0; ", sg, i->tand ? "&&" : "||", zr);
                    dst_write(s, &i->d, l, i->f16, "m");
                    sp(s, "} ");
                }
                sp(s, "}");
                break;
            }
            if (i->tint) {
                char a[2][48];
                for (int k = 0; k < 2; k++) {
                    const UOp *o = &i->s[k];
                    if (o->bank == RB_IMM) snprintf(a[k], sizeof(a[k]), "%d", o->idx);
                    else if (o->bank == RB_I) snprintf(a[k], sizeof(a[k]), "int(floatBitsToUint(I[%u][0]) & 0xFFu)", o->idx & 3);
                    else snprintf(a[k], sizeof(a[k]), "int(%s[%u] & 0xFFu)", bank_arr(o->bank), o->idx);
                }
                sp(s, "float t = float(%s - %s); ", a[0], a[1]);
            } else {
                sp(s, "float t = ");
                if (i->top == 14) { src_expr(s, &i->s[0], 0, i->f16); sp(s, " - "); src_expr(s, &i->s[1], 0, i->f16); }
                else if (i->top == 10) { sp(s, "max("); src_expr(s, &i->s[0], 0, i->f16); sp(s, ", "); src_expr(s, &i->s[1], 0, i->f16); sp(s, ")"); }
                else { src_expr(s, &i->s[0], 0, i->f16); sp(s, " * "); src_expr(s, &i->s[1], 0, i->f16); }
                sp(s, "; ");
            }
            const char *sg = i->tsign == 1 ? "t < 0.0" : i->tsign == 2 ? "t > 0.0" : (i->tand ? "true" : "false");
            const char *zr = i->tzero == 1 ? "t == 0.0" : i->tzero == 2 ? "t != 0.0" : (i->tand ? "true" : "false");
            sp(s, "P[%u] = (%s) %s (%s); }", i->tpdst, sg, i->tand ? "&&" : "||", zr);
            break;
        }
        case UI_SMP: {
            if (!is_frag) { snprintf(err, errlen, "SMP en vértice"); return; }
            sp(s, "{ vec4 c = %s(T%u, vec2(", i->gather ? "textureGather" : "texture", i->unit);
            UOp c = i->s[0];
            for (int k = 0; k < 2; k++) {
                c.sel[0] = (int8_t)k;
                if (k) sp(s, ", ");
                src_expr(s, &c, 0, i->cf16);
            }
            sp(s, i->gather ? "), 0); " : ")); ");
            UOp dd = i->d;
            for (int l = 0; l < 4; l++) {
                char v[8]; snprintf(v, sizeof(v), "c[%d]", l);
                dst_write(s, &dd, l, !i->df32, v);
            }
            sp(s, "}");
            break;
        }
        case UI_KILL:
            if (is_frag) sp(s, "discard;");
            break;
        default: break;
    }
    emit_pred_close(s, i->pred);
    sp(s, "\n");
}


/* Una mitad de la doble emisión: deja los valores en <p>0..<p>3 (o <p>s). */
static void dual_values(Str *s, const UInsn *i, const char *p) {
    int op = i->sub;
    if (dual_scalar_result(op)) {
        sp(s, "float %ss = ", p);
        switch (op) {
            case D_DP: case D_SSQ:
                for (int k = 0; k < i->n; k++) {
                    if (k) sp(s, " + ");
                    src_expr(s, &i->s[0], k, 0); sp(s, " * "); src_expr(s, &i->s[op == D_SSQ ? 0 : 1], k, 0);
                }
                break;
            case D_RSQ: sp(s, "inversesqrt("); src_expr(s, &i->s[0], 0, 0); sp(s, ")"); break;
            case D_RCP: sp(s, "1.0 / ("); src_expr(s, &i->s[0], 0, 0); sp(s, ")"); break;
            case D_FEXP: sp(s, "exp2("); src_expr(s, &i->s[0], 0, 0); sp(s, ")"); break;
            case D_FLOG: sp(s, "log2("); src_expr(s, &i->s[0], 0, 0); sp(s, ")"); break;
            case D_FMAD: sp(s, "fma("); src_expr(s, &i->s[0], 0, 0); sp(s, ", "); src_expr(s, &i->s[1], 0, 0); sp(s, ", ");
                         src_expr(s, &i->s[2], 0, 0); sp(s, ")"); break;
            case D_FADD: src_expr(s, &i->s[0], 0, 0); sp(s, " + "); src_expr(s, &i->s[1], 0, 0); break;
            case D_FMUL: src_expr(s, &i->s[0], 0, 0); sp(s, " * "); src_expr(s, &i->s[1], 0, 0); break;
            case D_FSUBFLR: src_expr(s, &i->s[0], 0, 0); sp(s, " - floor("); src_expr(s, &i->s[1], 0, 0); sp(s, ")"); break;
        }
        sp(s, "; ");
        return;
    }
    for (int l = 0; l < 4; l++) {
        if (!(i->mask >> l & 1)) continue;
        sp(s, "float %s%d = ", p, l);
        switch (op) {
            case D_MAD: sp(s, "fma("); src_expr(s, &i->s[0], l, 0); sp(s, ", "); src_expr(s, &i->s[1], l, 0); sp(s, ", ");
                        src_expr(s, &i->s[2], l, 0); sp(s, ")"); break;
            case D_MUL: src_expr(s, &i->s[0], l, 0); sp(s, " * "); src_expr(s, &i->s[1], l, 0); break;
            case D_ADD: src_expr(s, &i->s[0], l, 0); sp(s, " + "); src_expr(s, &i->s[1], l, 0); break;
            default:    src_expr(s, &i->s[0], l, 0); break;   /* MOV */
        }
        sp(s, "; ");
    }
}

static void dual_writes(Str *s, const UInsn *i, const char *p) {
    for (int l = 0; l < 4; l++) {
        if (!(i->mask >> l & 1)) continue;
        char v[16];
        if (dual_scalar_result(i->sub)) snprintf(v, sizeof(v), "%ss", p);
        else snprintf(v, sizeof(v), "%s%d", p, l);
        dst_write(s, &i->d, l, 0, v);
    }
}

static void emit_dual(Str *s, const UInsn *a, const UInsn *b) {
    sp(s, "    ");
    emit_pred_open(s, a->pred);
    sp(s, "{ ");
    dual_values(s, a, "a");
    dual_values(s, b, "b");
    dual_writes(s, a, "a");
    dual_writes(s, b, "b");
    sp(s, "}");
    emit_pred_close(s, a->pred);
    sp(s, "\n");
}

/* ========================================================================== */
/* Programa completo                                                           */
/* ========================================================================== */

static int decode_range(Dec *d, uint32_t off, uint32_t count, int sec) {
    d->sec = sec;
    for (uint32_t k = 0; k < count && !d->err[0]; k++) decode(d, g64(d->g, off + 8 * k));
    return !d->err[0];
}

/* Bytes que ocupa el contenido de un buffer de uniformes según los parámetros. */
static uint32_t ub_extent(const Gxp *g, uint32_t cont) {
    uint32_t n = g32(g, 0x24), end = 0;
    Param p;
    for (uint32_t i = 0; i < n; i++) {
        if (!gxp_param(g, i, &p) || p.cat != 1 || p.cont != cont) continue;
        uint32_t comps = p.comps ? p.comps : 4;
        uint32_t words = p.arr ? p.arr * ((comps + 3) & ~3u) : comps;
        uint32_t bytes = p.type == 1 ? words * 2 : words * 4;   /* F16 */
        uint32_t e = p.res * 4 + bytes;
        if (e > end) end = e;
    }
    return (end + 15) & ~15u;
}

int usse_debug_input = -1;

char *usse_translate(const uint8_t *bytes, size_t len, UsseInfo *info, char *err, size_t errlen) {
    Gxp g = { bytes, len };
    Dec d;
    memset(&d, 0, sizeof(d));
    memset(info, 0, sizeof(*info));
    memset(d.samp_unit, -1, sizeof(d.samp_unit));
    d.g = &g;
    for (int k = 0; k < 4; k++) d.inc[k] = 1;
    if (len < 0xA0 || memcmp(bytes, "GXP", 3)) { snprintf(err, errlen, "no es GXP"); return NULL; }
    info->is_frag = (int)(bytes[0x14] & 1);
    d.frag = info->is_frag;

    /* contenedores: el 19 da la base de los registros de buffers y literales */
    uint32_t nc = g32(&g, 0x90), co = grel(&g, 0x94);
    for (uint32_t i = 0; i < nc; i++) if (g16(&g, co + 8 * i) == 19) d.c19 = g16(&g, co + 8 * i + 4);
    info->defub_words = g32(&g, 0x64);
    info->sa_count = g32(&g, 0x30) >> 16;
    /* buffers de uniformes */
    uint32_t nub = g32(&g, 0x78), uo = grel(&g, 0x7c);
    for (uint32_t i = 0; i < nub && info->nub < USSE_MAX_UB; i++) {
        uint32_t w = g32(&g, uo + 8 * i);
        info->ub[info->nub].buf = (uint16_t)(w & 0xFFFF);
        info->ub[info->nub].sa = (uint16_t)(d.c19 + (w >> 16));
        info->ub[info->nub].bytes = ub_extent(&g, w & 0xFFFF);
        info->nub++;
    }
    /* literales */
    uint32_t nl = g32(&g, 0x70), lo = grel(&g, 0x74);
    for (uint32_t i = 0; i < nl && info->nlit < USSE_MAX_LIT; i++) {
        info->lit[info->nlit].sa = (uint16_t)(d.c19 + g32(&g, lo + 8 * i));
        info->lit[info->nlit].val = g32(&g, lo + 8 * i + 4);
        info->nlit++;
    }
    /* samplers dependientes: sa (relativo al contenedor 19) con el estado de la unidad */
    uint32_t nds = g32(&g, 0x80), dso = grel(&g, 0x84);
    for (uint32_t i = 0; i < nds; i++) {
        uint32_t a = g16(&g, dso + 4 * i), b = g16(&g, dso + 4 * i + 2) + d.c19;
        if (b < 256 && !(a & 3)) d.samp_unit[b] = (int8_t)(a >> 2);
    }
    /* parámetros: atributos y texturas */
    Param p;
    for (uint32_t i = 0; gxp_param(&g, i, &p); i++) {
        if (p.cat == 0 && !info->is_frag && info->nattr < USSE_MAX_ATTR) {
            info->attr[info->nattr].reg = (uint16_t)p.res;
            info->attr[info->nattr].type = (uint8_t)p.type;
            info->attr[info->nattr].comps = (uint8_t)p.comps;
            info->nattr++;
        }
        if (p.cat == 2) {
            info->tex_units |= 1u << (p.res & 31);
            if (strstr(p.name, "Shadow")) info->tex_shadow |= 1u << (p.res & 31);
            if (p.sem & 0x1000) info->tex_cube |= 1u << (p.res & 31);
        }
    }

    uint32_t nsec = g32(&g, 0x44), so = grel(&g, 0x48);
    uint32_t npri = g32(&g, 0x3c), po = grel(&g, 0x40);
    uint32_t nsec_insn = 0;
    if (!decode_range(&d, so, nsec, 1)) goto bad;
    nsec_insn = d.n;
    for (int k = 0; k < 4; k++) { d.inc[k] = 1; d.is_swz[k] = 0; }
    if (!decode_range(&d, po, npri, 0)) goto bad;

    Str s = { 0 };
    sp(&s, "#version 450\n");
    sp(&s, "layout(std430, set = 0, binding = 0) readonly buffer Mem { uint mem[]; };\n");
    sp(&s, "layout(push_constant) uniform PC { vec4 vp; vec4 vz; uint vsa, fsa, flags, pad; } pc;\n");
    sp(&s, "uint ld(uint a) { return a < uint(mem.length()) ? mem[a] : 0u; }\n");
    sp(&s, "uint wh(uint w, int hi, float v) { uint h = packHalf2x16(vec2(v, 0.0)) & 0xFFFFu; "
           "return hi != 0 ? (w & 0xFFFFu) | (h << 16) : (w & 0xFFFF0000u) | h; }\n");
    uint32_t npa = (g32(&g, 0x30) & 0xFFFF) + 8, nsa = info->sa_count + 8, nr = g32(&g, 0x34) + 8;
    if (npa < 64) npa = 64;
    if (nsa < 64) nsa = 64;
    if (nr < 64) nr = 64;
    if (nsa < 256) nsa = 256;
    info->sa_words = nsa;
    const char *mainname = "main";
    if (info->is_frag) {   /* unidades de las muestras no dependientes (entradas) y de SMP */
        uint32_t vo = grel(&g, 0x2c), cnt = g16(&g, vo + 12);
        for (uint32_t k = 0; k < cnt; k++) {
            uint32_t w1 = g32(&g, vo + 16 + 16 * k + 4), w2 = g32(&g, vo + 16 + 16 * k + 8);
            if ((w1 & 0xFF) != 0x0F && (w1 & 0xFF) != 0xFF && w2 < 16) info->tex_units |= 1u << w2;
        }
        for (uint32_t k = 0; k < d.n; k++) if (d.v[k].kind == UI_SMP) info->tex_units |= 1u << (d.v[k].unit & 15);
    }
    if (info->is_frag) {
        sp(&s, "layout(location = 0) in vec4 v_tc[10];\nlayout(location = 10) in vec4 v_col0;\n");
        sp(&s, "layout(location = 0) out vec4 o_color;\n");
        for (int u = 0; u < 16; u++)
            if (info->tex_units >> u & 1) sp(&s, "layout(set = 1, binding = %d) uniform sampler2D T%d;\n", u, u);
        /* unidades de muestras no dependientes que no son parámetros (por si acaso) */
    } else {
        for (uint32_t k = 0; k < info->nattr; k++) sp(&s, "layout(location = %u) in vec4 a%u;\n", k, k);
        sp(&s, "layout(location = 0) out vec4 v_tc[10];\nlayout(location = 10) out vec4 v_col0;\n");
    }
    sp(&s, "void %s() {\n", mainname);
    sp(&s, "    uint PA[%u]; uint SA[%u]; uint R[%u]; uint O[64]; vec4 I[4]; bool P[4];\n", npa, nsa, nr);
    sp(&s, "    for (int k = 0; k < %u; k++) PA[k] = 0u;\n", npa);
    sp(&s, "    for (int k = 0; k < %u; k++) R[k] = 0u;\n", nr);
    sp(&s, "    for (int k = 0; k < 64; k++) O[k] = 0u;\n");
    sp(&s, "    for (int k = 0; k < 4; k++) { I[k] = vec4(0.0); P[k] = false; }\n");
    sp(&s, "    { uint b = pc.%s; for (int k = 0; k < %u; k++) SA[k] = mem[b + uint(k)]; }\n", info->is_frag ? "fsa" : "vsa", nsa);

    if (info->is_frag) {
        /* entradas: varyings y muestras no dependientes, en orden, a partir de pa0 */
        uint32_t vo = grel(&g, 0x2c);
        uint32_t cnt = g16(&g, vo + 12), cur = 0;
        for (uint32_t k = 0; k < cnt; k++) {
            uint32_t e = vo + 16 + 16 * k;
            uint32_t w1 = g32(&g, e + 4), w2 = g32(&g, e + 8);
            uint32_t V = (w1 >> 12) & 0xF, S = w1 & 0xFF;
            if (V != 0xF) {
                const char *src = V == 0xA ? "v_col0" : NULL;
                char nm[24];
                if (!src) { snprintf(nm, sizeof(nm), "v_tc[%u]", V); src = nm; }
                if (V > 0xA) { snprintf(err, errlen, "entrada de fragmento %u", V); free(s.p); goto bad_noerr; }
                if ((w1 >> 29) & 1) {   /* F16: 2 palabras */
                    sp(&s, "    PA[%u] = packHalf2x16(%s.xy); PA[%u] = packHalf2x16(%s.zw);\n", cur, src, cur + 1, src);
                    cur += 2;
                } else {
                    for (int q = 0; q < 4; q++) sp(&s, "    PA[%u] = floatBitsToUint(%s[%d]);\n", cur + (uint32_t)q, src, q);
                    cur += 4;
                }
            }
            if (S != 0x0F && S != 0xFF) {
                if (S > 9) { snprintf(err, errlen, "muestra con coordenada %u", S); free(s.p); goto bad_noerr; }
                if (w2 < 16) info->tex_units |= 1u << w2;
                sp(&s, "    { vec4 c = texture(T%u, v_tc[%u].xy); PA[%u] = packHalf2x16(c.xy); PA[%u] = packHalf2x16(c.zw); }\n", w2, S,
                   cur, cur + 1);
                cur += 2;
            }
        }
        if (usse_debug_input == 200)   /* depuración: fsa, SA[0] y tamaño de la memoria como color */
            sp(&s, "    o_color = vec4(pc.fsa == 256u ? 1.0 : 0.0, mem.length() > 500 ? 1.0 : 0.0, SA[0] == 2144u ? 1.0 : 0.0, 1.0); return;\n");
        if (usse_debug_input >= 0 && usse_debug_input < 100)   /* depuración: la pareja de entradas n tal cual */
            sp(&s, "    o_color = vec4(unpackHalf2x16(PA[%d]), unpackHalf2x16(PA[%d])); return;\n", 2 * usse_debug_input,
               2 * usse_debug_input + 1);
    } else {
        for (uint32_t k = 0; k < info->nattr; k++) {
            uint32_t r = info->attr[k].reg, c = info->attr[k].comps ? info->attr[k].comps : 4;
            if (info->attr[k].type == 1) {   /* F16 */
                sp(&s, "    PA[%u] = packHalf2x16(a%u.xy);", r, k);
                if (c > 2) sp(&s, " PA[%u] = packHalf2x16(a%u.zw);", r + 1, k);
                sp(&s, "\n");
            } else if (info->attr[k].type == 0) {
                for (uint32_t q = 0; q < c; q++) sp(&s, "    PA[%u] = floatBitsToUint(a%u[%u]);\n", r + q, k, q);
            } else {
                for (uint32_t q = 0; q < c; q++) sp(&s, "    PA[%u] = uint(a%u[%u]);\n", r + q, k, q);
            }
        }
    }

    for (uint32_t k = 0; k < d.n; k++) {
        if (k == nsec_insn) {
            sp(&s, "    // primario\n");
            if (info->is_frag && usse_debug_input >= 100)   /* depuración: sa[k], sa[k+1] tras el secundario */
                sp(&s, "    o_color = vec4(unpackHalf2x16(SA[%d]), unpackHalf2x16(SA[%d])); return;\n", usse_debug_input - 100,
                   usse_debug_input - 99);
        }
        char e2[128] = "";
        if (d.v[k].kind == UI_DUAL1 && k + 1 < d.n) { emit_dual(&s, &d.v[k], &d.v[k + 1]); k++; continue; }
        emit_insn(&s, &d.v[k], info->is_frag, e2, sizeof(e2));
        if (e2[0]) { snprintf(err, errlen, "%s", e2); free(s.p); goto bad_noerr; }
    }

    if (info->is_frag) {
        info->out_u8 = 0;
        sp(&s, "    o_color = vec4(unpackHalf2x16(PA[0]), unpackHalf2x16(PA[1]));\n");
        sp(&s, "    if ((pc.flags & 2u) != 0u) o_color = vec4(mix(o_color.rgb, vec3(1.0, 0.0, 1.0), 0.5), o_color.a);\n");
    } else {
        /* salidas: posición o0-3, Color0 (si hay), TEXCOORDn según outputs2 */
        uint32_t vo = grel(&g, 0x2c);
        uint32_t o1 = g32(&g, vo + 16), o2 = g32(&g, vo + 20), cur = 4;
        sp(&s, "    vec4 cp = vec4(uintBitsToFloat(O[0]), uintBitsToFloat(O[1]), uintBitsToFloat(O[2]), uintBitsToFloat(O[3]));\n");
        if (o1 & 0x800) {
            sp(&s, "    v_col0 = vec4(uintBitsToFloat(O[%u]), uintBitsToFloat(O[%u]), uintBitsToFloat(O[%u]), uintBitsToFloat(O[%u]));\n",
               cur, cur + 1, cur + 2, cur + 3);
            cur += 4;
        } else sp(&s, "    v_col0 = vec4(1.0);\n");
        for (int t = 0; t < 10; t++) {
            uint32_t code = (o2 >> (3 * t)) & 7, n = 0;
            for (int b = 0; b < 3; b++) n += (code >> b) & 1;
            if (!code) { sp(&s, "    v_tc[%d] = vec4(0.0);\n", t); continue; }
            n += 1;
            sp(&s, "    v_tc[%d] = vec4(", t);
            for (uint32_t q = 0; q < 4; q++) {
                if (q) sp(&s, ", ");
                if (q < n) sp(&s, "uintBitsToFloat(O[%u])", cur + q);
                else sp(&s, q == 3 ? "1.0" : "0.0");
            }
            sp(&s, ");\n");
            cur += n;
        }
        /* viewport de GXM como en el shader genérico (ver ui.vert) */
        sp(&s, "    vec2 size = pc.vz.zw;\n");
        sp(&s, "    if ((pc.flags & 1u) != 0u) { vec2 ndc = cp.xy / cp.w; vec2 px = vec2(pc.vp.x + pc.vp.y * ndc.x, pc.vp.z + pc.vp.w * ndc.y);\n"
               "        float z = pc.vz.x + pc.vz.y * (cp.z / cp.w); gl_Position = vec4((px / size * 2.0 - 1.0) * cp.w, z * cp.w, cp.w); }\n"
               "    else gl_Position = vec4(cp.xy / size * 2.0 - 1.0, clamp(cp.z, 0.0, 1.0), 1.0);\n");
    }
    sp(&s, "}\n");
    free(d.v);
    return s.p;
bad:
    snprintf(err, errlen, "%s", d.err);
bad_noerr:
    free(d.v);
    return NULL;
}

char *usse_disasm(const uint8_t *gxp, size_t len) { (void)gxp; (void)len; return NULL; }
