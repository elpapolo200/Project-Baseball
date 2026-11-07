/* atomic.c — LDREX/STREX as a per-thread exclusive monitor.
 *
 * The guest's lock-free code (the allocator, reference counts, spinlocks) is a
 * load-exclusive / store-exclusive loop. The monitor here remembers what the
 * load saw, and the store succeeds only if memory still holds that value, as
 * one host compare-and-swap. That is weaker than hardware in one way — a
 * store of the same value in between (ABA) is not noticed — and that never
 * changes the outcome of the retry loops compilers and the SDK generate.
 */

#include "vitarecomp/recomp_rt.h"
#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

static VITA_TLS uint32_t g_ex_addr;
static VITA_TLS uint32_t g_ex_value;
static VITA_TLS int      g_ex_size;     /* 0 = monitor clear */

uint32_t vita_ldrex(uint32_t addr, int size) {
    uint32_t v = size == 1 ? vita_read8(addr) : size == 2 ? vita_read16(addr) : vita_read32(addr);
    g_ex_addr = addr;
    g_ex_value = v;
    g_ex_size = size;
    return v;
}

uint32_t vita_strex(uint32_t addr, uint32_t value, int size) {
    if (g_ex_size != size || g_ex_addr != addr) { g_ex_size = 0; return 1; }
    g_ex_size = 0;
    void *p = vita_mem_ptr(addr, (uint32_t)size);
    if (!p) return 1;
#if defined(_MSC_VER)
    if (size == 4)
        return (uint32_t)_InterlockedCompareExchange((volatile long *)p, (long)value, (long)g_ex_value) != g_ex_value;
    if (size == 2)
        return (uint16_t)_InterlockedCompareExchange16((volatile short *)p, (short)value, (short)g_ex_value) != (uint16_t)g_ex_value;
    return (uint8_t)_InterlockedCompareExchange8((volatile char *)p, (char)value, (char)g_ex_value) != (uint8_t)g_ex_value;
#else
    if (size == 4) { uint32_t e = g_ex_value; return !__atomic_compare_exchange_n((uint32_t *)p, &e, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    if (size == 2) { uint16_t e = (uint16_t)g_ex_value; return !__atomic_compare_exchange_n((uint16_t *)p, &e, (uint16_t)value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    { uint8_t e = (uint8_t)g_ex_value; return !__atomic_compare_exchange_n((uint8_t *)p, &e, (uint8_t)value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
#endif
}

void vita_clrex(void) { g_ex_size = 0; }
