/* SceAudioOut para el port de MLB 15.
 *
 * El juego abre un puerto de salida y su hilo de audio llama a
 * sceAudioOutOutput con bloques de muestras (la mezcla final de SceNgs, ver
 * hle_ngs.c). Se envían a SDL (cola de audio, S16 a la frecuencia del puerto).
 * El ritmo lo marca la tarjeta de sonido: si hay más de unos pocos bloques en
 * cola, Output espera a que baje. Sin dispositivo de audio (o con
 * MLB15_AUDIO=0) se vuelve a lo de antes: dormir lo que dura el bloque, para
 * que el hilo no gire a miles de vueltas por segundo y el reloj del juego siga.
 *
 * sceAudioOutOpenPort(type, len, freq, mode):
 *   r0 = tipo de puerto, r1 = muestras por bloque, r2 = frecuencia, r3 = modo (0 mono, 1 estéreo)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <SDL.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"

static uint32_t g_port_len[8];
static uint32_t g_port_freq[8];
static uint32_t g_port_mode[8];
static int      g_port_vol[8] = { 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768 };
static SDL_AudioDeviceID g_dev;
static int g_dev_tried;

static void audio_open(uint32_t freq) {
    if (g_dev_tried) return;
    g_dev_tried = 1;
    const char *e = getenv("MLB15_AUDIO");
    if (e && *e == '0') return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        printf("[audio] sin SDL audio: %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = (int)freq;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev) { printf("[audio] no se pudo abrir el dispositivo: %s\n", SDL_GetError()); return; }
    SDL_PauseAudioDevice(g_dev, 0);
    printf("[audio] SDL: %d Hz, %d canales\n", have.freq, have.channels);
}

void vita_hle_sceAudioOutOpenPort(void) {
    uint32_t type = r0, len = r1, freq = r2, mode = r3;
    uint32_t port = 1;                      /* los puertos validos empiezan en 0 */
    if (port < 8) {
        g_port_len[port] = len ? len : 1024;
        g_port_freq[port] = freq ? freq : 48000;
        g_port_mode[port] = mode;
    }
    printf("[audio] OpenPort(tipo=%u, bloque=%u, %u Hz, modo=%u) -> puerto %u\n",
           type, len, freq, mode, port);
    audio_open(freq ? freq : 48000);
    fflush(stdout);
    r0 = port;
}

void vita_hle_sceAudioOutOutput(void) {
    /* r0 = puerto, r1 = buffer de muestras (puede ser NULL para silencio) */
    uint32_t port = r0, buf = r1;
    uint32_t len = (port < 8 && g_port_len[port]) ? g_port_len[port] : 1024;
    uint32_t freq = (port < 8 && g_port_freq[port]) ? g_port_freq[port] : 48000;
    int stereo = port < 8 ? g_port_mode[port] != 0 : 1;
    if (!g_dev) {
        DWORD ms = (DWORD)((uint64_t)len * 1000ull / freq);
        if (ms) Sleep(ms);                  /* NULL tambien espera un bloque */
        r0 = 0;
        return;
    }
    static int16_t tmp[2 * 8192];
    if (len > 8192) len = 8192;
    const int16_t *src = buf ? (const int16_t *)vita_mem_ptr(buf, len * (stereo ? 4u : 2u)) : NULL;
    int vol = port < 8 ? g_port_vol[port] : 32768;
    for (uint32_t i = 0; i < len; i++) {
        int l = src ? src[stereo ? 2 * i : i] : 0, rr = src ? src[stereo ? 2 * i + 1 : i] : 0;
        tmp[2 * i] = (int16_t)(l * vol >> 15);
        tmp[2 * i + 1] = (int16_t)(rr * vol >> 15);
    }
    {   /* MLB15_AUDIO_WAV=archivo: guarda lo que sale (PCM s16 estéreo crudo) para comprobarlo */
        static FILE *wav;
        static int tried;
        if (!tried) { tried = 1; const char *w = getenv("MLB15_AUDIO_WAV"); if (w) wav = fopen(w, "wb"); }
        if (wav) fwrite(tmp, 4, len, wav);
    }
    /* Ritmo: no más de ~4 bloques en cola (≈ 40 ms a 512/48 kHz). */
    uint32_t block = len * 4;
    while (SDL_GetQueuedAudioSize(g_dev) > block * 4) Sleep(1);
    SDL_QueueAudio(g_dev, tmp, block);
    r0 = 0;
}

void vita_hle_sceAudioOutSetVolume(void) {
    /* r0 = puerto, r1 = flags (bit0 izq, bit1 der), r2 = int vol[2] */
    uint32_t port = r0, flags = r1, vp = r2;
    if (port < 8 && vita_mem_ptr(vp, 8)) {
        if (flags & 1) g_port_vol[port] = (int)vita_read32(vp);
        else if (flags & 2) g_port_vol[port] = (int)vita_read32(vp + 4);
        if (g_port_vol[port] < 0) g_port_vol[port] = 0;
        if (g_port_vol[port] > 32768) g_port_vol[port] = 32768;
    }
    r0 = 0;
}
/* ¿El juego tiene el puerto de ese tipo (BGM)? 1 = sí. Con 0 el juego cree que el
 * reproductor de música del sistema ocupa el BGM: sub_81008B86 guarda el resultado en
 * 0x81E4F948 y sub_810061A4 pone a 0 los grupos de música, efectos y comentarios
 * (claves 0x21..0x25 de la tabla de mezclas). */
void vita_hle_sceAudioOutGetAdopt(void) { r0 = 1; }
void vita_hle_sceAudioOutReleasePort(void) { r0 = 0; }
