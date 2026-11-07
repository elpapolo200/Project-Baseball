/* SceLibm (matemáticas) para MLB 15.
 *
 * El SDK de la Vita usa el ABI hard-float: un float va en s0 (el segundo en s1),
 * un double en d0, y el resultado vuelve en s0 / d0. Los enteros y punteros
 * siguen en r0-r3. Los _F* son funciones internas de la libc Dinkumware.
 */
#include <math.h>
#include <stdint.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/vfp.h"

#define F1(name, expr) void vita_hle_##name(void) { float x = vfp_getf(0); vfp_setf(0, (expr)); }
#define F2(name, expr) void vita_hle_##name(void) { float x = vfp_getf(0), y = vfp_getf(1); vfp_setf(0, (expr)); }
#define D1(name, expr) void vita_hle_##name(void) { double x = vfp_getd(0); vfp_setd(0, (expr)); }

F1(floorf, floorf(x))
F1(ceilf,  ceilf(x))
F1(truncf, truncf(x))
F1(fabsf,  fabsf(x))
F1(sqrtf,  sqrtf(x))
F1(tanf,   tanf(x))
F1(atanf,  atanf(x))
F1(asinf,  asinf(x))
F1(acosf,  acosf(x))
F1(expf,   expf(x))
F2(atan2f, atan2f(x, y))
F2(powf,   powf(x, y))
F2(fmodf,  fmodf(x, y))
D1(floor,  floor(x))
D1(trunc,  trunc(x))
D1(acos,   acos(x))

/* float frexpf(float x, int *exp) */
void vita_hle_frexpf(void) {
    int e = 0;
    float m = frexpf(vfp_getf(0), &e);
    if (r0) vita_write32(r0, (uint32_t)e);
    vfp_setf(0, m);
}

/* float modff(float x, float *ipart) */
void vita_hle_modff(void) {
    float ip = 0.0f;
    float fr = modff(vfp_getf(0), &ip);
    vfp_reg v;
    v.f = ip;
    if (r0) vita_write32(r0, v.u);
    vfp_setf(0, fr);
}

/* float _FSin(float x, unsigned qoff): seno desplazado qoff cuartos de vuelta
 * (qoff 0 = sin, 1 = cos, 2 = -sin, 3 = -cos). sinf/cosf del juego van por aquí. */
static float sin_q(float x, uint32_t q) {
    switch (q & 3) {
        case 0:  return sinf(x);
        case 1:  return cosf(x);
        case 2:  return -sinf(x);
        default: return -cosf(x);
    }
}
void vita_hle__FSin(void)  { vfp_setf(0, sin_q(vfp_getf(0), r0)); }
void vita_hle__FSinx(void) { vfp_setf(0, sin_q(vfp_getf(0), r0)); }

/* float _FLog(float x, int base): 0 = ln, 1 = log10, 2 = log2. */
void vita_hle__FLog(void) {
    float x = vfp_getf(0);
    vfp_setf(0, r0 == 1 ? log10f(x) : r0 == 2 ? log2f(x) : logf(x));
}

/* short _FDtest(float *px): clasifica *px. Códigos Dinkumware:
 * 0 cero, -1 finito, -2 subnormal, 1 infinito, 2 NaN. */
void vita_hle__FDtest(void) {
    vfp_reg v;
    v.u = vita_read32(r0);
    switch (fpclassify(v.f)) {
        case FP_ZERO:      r0 = 0; break;
        case FP_SUBNORMAL: r0 = (uint32_t)-2; break;
        case FP_INFINITE:  r0 = 1; break;
        case FP_NAN:       r0 = 2; break;
        default:           r0 = (uint32_t)-1; break;
    }
}
