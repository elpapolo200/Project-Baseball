/* SceRtc para el port de MLB 15.
 *
 * El juego usa el reloj para sellos de tiempo (una vez al arrancar:
 * GetCurrentNetworkTick, GetCurrentClockLocalTime, SetTick, ConvertUtcToLocal,
 * ParseRFC3339...). Aqui se usa el reloj de Windows.
 *
 * Unidades: el "tick" de la Vita son MICROSEGUNDOS desde el 0001-01-01 00:00:00
 * UTC, que es el mismo origen que usa el firmware real. La resolucion que
 * declara la API es 1.000.000.
 *
 * SceDateTime (0x10 bytes): u16 year, month, day, hour, minute, second,
 * u32 microsecond.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <windows.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"

/* Segundos entre 0001-01-01 y 1970-01-01 (constante del calendario gregoriano
 * proleptico, la misma que usa el firmware de la Vita). */
#define RTC_UNIX_OFFSET_S 62135596800ull
#define RTC_RESOLUTION    1000000u

static uint64_t rtc_now_us(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);        /* 100 ns desde 1601-01-01 UTC */
    uint64_t t100 = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    uint64_t unix_us = t100 / 10ull - 11644473600000000ull;
    return unix_us + RTC_UNIX_OFFSET_S * 1000000ull;
}

static void put_u64(uint32_t addr, uint64_t v) {
    if (!vita_mem_ptr(addr, 8)) return;
    vita_write32(addr, (uint32_t)v);
    vita_write32(addr + 4, (uint32_t)(v >> 32));
}

static void ticks_to_datetime(uint64_t ticks, SYSTEMTIME *st) {
    uint64_t vita_s = ticks / RTC_RESOLUTION;
    uint64_t unix_s = vita_s - RTC_UNIX_OFFSET_S;
    time_t t = (time_t)unix_s;
    struct tm tmv;
    if (gmtime_s(&tmv, &t) != 0) {
        memset(st, 0, sizeof(*st));
        return;
    }
    st->wYear = (WORD)(tmv.tm_year + 1900);
    st->wMonth = (WORD)(tmv.tm_mon + 1);
    st->wDayOfWeek = (WORD)tmv.tm_wday;
    st->wDay = (WORD)tmv.tm_mday;
    st->wHour = (WORD)tmv.tm_hour;
    st->wMinute = (WORD)tmv.tm_min;
    st->wSecond = (WORD)tmv.tm_sec;
    st->wMilliseconds = (WORD)((ticks / 1000ull) % 1000ull);
}

static void write_datetime(uint32_t addr, const SYSTEMTIME *st) {
    if (!vita_mem_ptr(addr, 0x10)) return;
    vita_write32(addr + 0x0, (uint32_t)st->wYear | ((uint32_t)st->wMonth << 16));
    vita_write32(addr + 0x4, (uint32_t)st->wDay | ((uint32_t)st->wHour << 16));
    vita_write32(addr + 0x8, (uint32_t)st->wMinute | ((uint32_t)st->wSecond << 16));
    vita_write32(addr + 0xC, (uint32_t)st->wMilliseconds * 1000u);
}

void vita_hle_sceRtcGetTickResolution(void) { r0 = RTC_RESOLUTION; }

void vita_hle_sceRtcGetCurrentTick(void) {
    uint64_t t = rtc_now_us();
    put_u64(r0, t);
    r0 = 0;
}
void vita_hle_sceRtcGetCurrentNetworkTick(void) {
    uint64_t t = rtc_now_us();
    put_u64(r0, t);
    r0 = 0;
}

void vita_hle_sceRtcSetTick(void) {
    /* r0 = SceDateTime*, r1 = SceRtcTick* */
    uint32_t dt = r0, tickp = r1;
    if (!vita_mem_ptr(dt, 0x10) || !vita_mem_ptr(tickp, 8)) { r0 = (uint32_t)-1; return; }
    SYSTEMTIME st;
    memset(&st, 0, sizeof(st));
    uint32_t v0 = vita_read32(dt), v1 = vita_read32(dt + 4), v2 = vita_read32(dt + 8);
    uint32_t us = vita_read32(dt + 12);
    st.wYear = (WORD)(v0 & 0xFFFF);
    st.wMonth = (WORD)(v0 >> 16);
    st.wDay = (WORD)(v1 & 0xFFFF);
    st.wHour = (WORD)(v1 >> 16);
    st.wMinute = (WORD)(v2 & 0xFFFF);
    st.wSecond = (WORD)(v2 >> 16);

    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = st.wYear - 1900;
    tmv.tm_mon = st.wMonth - 1;
    tmv.tm_mday = st.wDay;
    tmv.tm_hour = st.wHour;
    tmv.tm_min = st.wMinute;
    tmv.tm_sec = st.wSecond;
    time_t t = _mkgmtime(&tmv);
    uint64_t ticks = ((uint64_t)t + RTC_UNIX_OFFSET_S) * RTC_RESOLUTION + us;
    put_u64(tickp, ticks);
    r0 = 0;
}

void vita_hle_sceRtcGetCurrentClockLocalTime(void) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    write_datetime(r0, &st);
    r0 = 0;
}

void vita_hle_sceRtcGetCurrentClock(void) {
    /* r0 = SceDateTime*, r1 = zona horaria (0 = UTC) */
    SYSTEMTIME st;
    if (r1 == 0) GetSystemTime(&st); else GetLocalTime(&st);
    write_datetime(r0, &st);
    r0 = 0;
}

void vita_hle_sceRtcConvertUtcToLocalTime(void) {
    /* r0 = origen, r1 = destino. Se copia: el PC ya esta en hora local. */
    if (vita_mem_ptr(r0, 8) && vita_mem_ptr(r1, 8)) {
        vita_write32(r1, vita_read32(r0));
        vita_write32(r1 + 4, vita_read32(r0 + 4));
        r0 = 0;
    } else r0 = (uint32_t)-1;
}
void vita_hle_sceRtcConvertLocalTimeToUtc(void) { vita_hle_sceRtcConvertUtcToLocalTime(); }

void vita_hle_sceRtcParseRFC3339(void) {
    /* r0 = SceRtcTick*, r1 = cadena. Se acepta "YYYY-MM-DDTHH:MM:SSZ". */
    uint32_t out = r0, strp = r1;
    char s[64];
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (vita_mem_ptr(strp, 1)) {
        char *p = (char *)vita_mem_ptr(strp, 64);
        if (p) { strncpy(s, p, sizeof(s) - 1); s[sizeof(s) - 1] = 0; }
        else s[0] = 0;
    } else s[0] = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) >= 6) {
        struct tm tmv;
        memset(&tmv, 0, sizeof(tmv));
        tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d;
        tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = se;
        time_t t = _mkgmtime(&tmv);
        put_u64(out, ((uint64_t)t + RTC_UNIX_OFFSET_S) * RTC_RESOLUTION);
        r0 = 0;
    } else {
        r0 = (uint32_t)-1;
    }
}

void vita_hle_sceRtcFormatRFC3339LocalTime(void) {
    /* r0 = char*, r1 = SceRtcTick* */
    uint32_t out = r0, tickp = r1;
    if (vita_mem_ptr(out, 32) && vita_mem_ptr(tickp, 8)) {
        uint64_t t = (uint64_t)vita_read32(tickp) | ((uint64_t)vita_read32(tickp + 4) << 32);
        SYSTEMTIME st;
        ticks_to_datetime(t, &st);
        char *p = (char *)vita_mem_ptr(out, 32);
        if (p) snprintf(p, 32, "%04u-%02u-%02uT%02u:%02u:%02uZ",
                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        r0 = 0;
    } else r0 = (uint32_t)-1;
}
