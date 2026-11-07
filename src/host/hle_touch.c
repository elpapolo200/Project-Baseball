/* SceTouch para el port de MLB 15.
 *
 * El juego consulta el panel tactil (informacion de area y estado) porque su
 * interfaz admite entrada tactil en la Vita. En PC no hay panel: se devuelve
 * un panel valido sin toques (reportNum = 0).
 *
 * SceTouchPanelInfo (0x40): 8 short (area activa y de display), 2 bytes de
 * fuerza y 30 de reserva.
 * SceTouchData (0x90): u64 timeStamp, u32 status, u32 reportNum y 16 informes
 * de 0x10 bytes.
 */
#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"

#include "input.h"

static void write16(uint32_t addr, uint32_t v) {
    if (vita_mem_ptr(addr, 2)) *(uint16_t *)vita_mem_ptr(addr, 2) = (uint16_t)v;
}

void vita_hle_sceTouchSetSamplingState(void) {
    /* r0 = puerto, r1 = estado (0 = parado, 1 = encendido) */
    r0 = 0;
}

void vita_hle_sceTouchGetPanelInfo(void) {
    /* r0 = puerto, r1 = SceTouchPanelInfo* */
    uint32_t out = r1;
    if (!vita_mem_ptr(out, 0x40)) { r0 = (uint32_t)-1; return; }
    memset(vita_mem_ptr(out, 0x40), 0, 0x40);
    /* Panel delantero de la Vita: 1920 x 1088 en las dos areas. Los valores
     * del panel trasero son distintos en la consola real, pero el juego solo
     * los usa para escalar coordenadas y aqui nunca hay toques. */
    write16(out + 0x0, 0);        /* minAaX   */
    write16(out + 0x2, 0);        /* minAaY   */
    write16(out + 0x4, 1919);     /* maxAaX   */
    write16(out + 0x6, 1087);     /* maxAaY   */
    write16(out + 0x8, 0);        /* minDispX */
    write16(out + 0xA, 0);        /* minDispY */
    write16(out + 0xC, 1919);     /* maxDispX */
    write16(out + 0xE, 1087);     /* maxDispY */
    if (vita_mem_ptr(out + 0x10, 1)) *(uint8_t *)vita_mem_ptr(out + 0x10, 1) = 1;
    if (vita_mem_ptr(out + 0x11, 1)) *(uint8_t *)vita_mem_ptr(out + 0x11, 1) = 255;
    r0 = 0;
}

void vita_hle_sceTouchPeek(void) {
    /* r0 = puerto, r1 = SceTouchData*, r2 = numero de buffers */
    uint32_t port = r0, data = r1, count = r2 ? r2 : 1;
    if (count > 64) count = 64;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t p = data + i * 0x90u;
        if (!vita_mem_ptr(p, 0x90)) { r0 = (uint32_t)-1; return; }
        memset(vita_mem_ptr(p, 0x90), 0, 0x90);
        LARGE_INTEGER freq, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        uint64_t us = (uint64_t)(now.QuadPart * 1000000.0 / freq.QuadPart);
        vita_write32(p + 0, (uint32_t)us);
        vita_write32(p + 4, (uint32_t)(us >> 32));
        vita_write32(p + 8, 0);      /* status    */
        vita_write32(p + 12, 0);     /* reportNum: sin toques */
        /* Panel delantero: el ratón o el guion (input.c). SceTouchReport (0x10):
         * u8 id, u8 fuerza, u16 x, u16 y, reservado, u16 info. */
        static int was, id;                /* id nuevo en cada toque */
        int x, y;
        if (port == 0 && input_touch(&x, &y)) {
            if (!was) id = (id + 1) & 0x7F;
            vita_write32(p + 12, 1);
            *(uint8_t *)vita_mem_ptr(p + 0x10, 1) = (uint8_t)id;
            *(uint8_t *)vita_mem_ptr(p + 0x11, 1) = 128;
            write16(p + 0x12, (uint32_t)x);
            write16(p + 0x14, (uint32_t)y);
            was = 1;
        } else if (port == 0) {
            was = 0;
        }
    }
    r0 = count;
}
