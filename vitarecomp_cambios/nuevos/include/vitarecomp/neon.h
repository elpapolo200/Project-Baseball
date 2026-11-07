/* neon.h — Advanced SIMD, executed from the instruction encoding.
 *
 * NEON is many forms, each simple: the emitter does not translate them one by
 * one, it emits a call that hands the encoding to the runtime, which decodes
 * and executes it on the shared register file (vfp_s: D0-D31, Qn = D2n:D2n+1).
 * One implementation, testable on its own, in place of tens of thousands of
 * lines of generated lane loops. An encoding the runtime does not implement
 * traps by name at the moment it runs.
 *
 * `raw` is the Thumb-2 instruction as (first halfword << 16) | second.
 */
#ifndef VITARECOMP_NEON_H
#define VITARECOMP_NEON_H

#include <stdint.h>

/* Data processing (first halfword 0xEFxx / 0xFFxx). */
void neon_dp(uint32_t pc, uint32_t raw);

/* VLDn / VSTn (first halfword 0xF9xx with bit 4 clear). Returns the bytes
 * transferred, which the caller adds to the base for the `[rn]!` form. */
uint32_t neon_ldst(uint32_t pc, uint32_t raw, uint32_t base);

/* Core register <-> NEON: VDUP.n Qd, rt; VMOV.8/16 Dd[x], rt; VMOV rt, Dn[x]. */
void     neon_dup_core(uint32_t raw, uint32_t value);
void     neon_set_lane(uint32_t raw, uint32_t value);
uint32_t neon_get_lane(uint32_t raw);

/* Which of the above an encoding belongs to: 0 none, 1 dp, 2 ldst, 3 dup,
 * 4 set lane, 5 get lane. Header-only so the emitter shares it. */
static inline int neon_kind(uint32_t raw) {
    uint32_t h1 = raw >> 16, h2 = raw & 0xFFFF;
    if ((h1 & 0xEF00) == 0xEF00) return 1;
    if ((h1 & 0xFF10) == 0xF900) return 2;
    if ((h1 & 0xFF90) == 0xEE80 && (h2 & 0x0F5F) == 0x0B10) return 3;
    if ((h1 & 0xFF90) == 0xEE00 && (h2 & 0x0F1F) == 0x0B10) return 4;
    if ((h1 & 0xFF10) == 0xEE10 && (h2 & 0x0F1F) == 0x0B10) return 5;
    return 0;
}

#endif /* VITARECOMP_NEON_H */
