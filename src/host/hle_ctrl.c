/* SceCtrl para el port de MLB 15.
 *
 * El juego solo consulta el pad (3 PeekBufferPositive y un SetSamplingMode).
 * Los botones y los sticks salen del teclado/mando del PC (input.c, SDL) o del
 * guion MLB15_PAD_GUION; sin nada pulsado, sticks en el centro (0x80).
 *
 * SceCtrlData (0x18 bytes): u64 timeStamp, u32 buttons, u8 lx, ly, rx, ry,
 * u8 reserved[4].
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"

#include "input.h"
#include "port.h"

static int g_trace = -1;
static int trace_on(void) {
    if (g_trace < 0) g_trace = getenv("MLB15_CTRL_TRACE") ? 1 : 0;
    return g_trace;
}

static void ret64_u(uint64_t v) {
    r0 = (uint32_t)v;
    r1 = (uint32_t)(v >> 32);
}

/* Reloj monotonico en microsegundos; el juego solo lo usa como sello de tiempo. */
static uint64_t now_us(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000.0 / freq.QuadPart);
}

void vita_hle_sceCtrlSetSamplingMode(void) {
    static int once;
    if (!once) {
        once = 1;
        printf("[ctrl] SetSamplingMode(%u)\n", r0);
        fflush(stdout);
    }
    r0 = 0;
}

void vita_hle_sceCtrlPeekBufferPositive(void) {
    /* r0 = puerto, r1 = SceCtrlData*, r2 = cuantos buffers */
    uint32_t port = r0, data = r1, count = r2 ? r2 : 1;
    static int once;
    if (!once && trace_on()) {
        once = 1;
        printf("[ctrl] PeekBufferPositive(port=%u data=0x%08X count=%u)\n", port, data, count);
        fflush(stdout);
    }
    if (count > 64) count = 64;
    if (trace_on()) {   /* quién lee el pad: dirección de retorno y cuántas veces (cada 5 s) */
        static uint32_t lrs[8], cnt[8];
        static ULONGLONG t0;
        int k;
        uint32_t who = (uint32_t)port_current_thread();
        for (k = 0; k < 8 && lrs[k] && lrs[k] != who + 1; k++) {}
        if (k < 8) { if (!lrs[k]) printf("[ctrl] lee el pad el hilo %u '%s'\n", who, port_current_thread_name()); lrs[k] = who + 1; cnt[k]++; }
        ULONGLONG now = GetTickCount64();
        if (!t0) t0 = now;
        if (now - t0 >= 5000) {
            printf("[ctrl] lecturas 5 s:");
            for (k = 0; k < 8 && lrs[k]; k++) { printf(" hilo %u x%u", lrs[k] - 1, cnt[k]); cnt[k] = 0; }
            printf(" (port %u count %u)\n", port, count);
            t0 = now;
        }
    }
    uint32_t buttons = input_buttons();
    uint8_t st[4];
    input_sticks(st);
    {
        static uint32_t last;
        if (buttons != last && trace_on()) printf("[ctrl] botones 0x%04X\n", buttons);
        last = buttons;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t p = data + i * 0x18u;
        if (!vita_mem_ptr(p, 0x18)) { r0 = (uint32_t)-1; return; }
        uint64_t t = now_us();
        vita_write32(p + 0, (uint32_t)t);
        vita_write32(p + 4, (uint32_t)(t >> 32));
        vita_write32(p + 8, buttons);
        vita_write32(p + 12, (uint32_t)st[0] | ((uint32_t)st[1] << 8) | ((uint32_t)st[2] << 16) | ((uint32_t)st[3] << 24));
        vita_write32(p + 16, 0);
        vita_write32(p + 20, 0);
    }
    r0 = count;
}
