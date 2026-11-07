/* SceDisplay para el port de MLB 15: captura el frame que el juego manda a
 * pantalla (su callback de flip llama a sceDisplaySetFrameBuf) y lo presenta en
 * una ventana Win32 con GDI. Guarda los primeros frames en BMP para poder
 * inspeccionarlos sin ventana (MLB15_NOWINDOW=1).
 *
 * El buffer de la Vita es A8B8G8R8: bytes R,G,B,A. GDI usa B,G,R,X, asi que se
 * intercambian R y B. Se puede desactivar con MLB15_SWAPRB=0.
 *
 * Variables de entorno:
 *   MLB15_NOWINDOW=1   no abre ventana (solo volcados)
 *   MLB15_SCALE=n      escala de la ventana (por defecto 1)
 *   MLB15_FPS=n        limite de frames presentados (0 = sin limite, 60 por defecto)
 *   MLB15_SWAPRB=0     no intercambiar rojo y azul
 *   MLB15_FRAMES=n     cada cuantos frames se guarda un BMP (0 = solo los 3 primeros)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"

#include "gpu.h"

#include "port.h"

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#define FB_DEFAULT_W 960
#define FB_DEFAULT_H 544

static HWND      g_hwnd;
static int       g_win_w, g_win_h;
static int       g_closed;
static uint32_t  g_base;
static uint32_t  g_pitch;
static int       g_w, g_h;
static uint32_t  g_format;
static uint32_t *g_pixels;       /* BGRA para GDI */
static size_t    g_pixels_cap;   /* en pixeles */
static LARGE_INTEGER g_qpc_freq, g_qpc_last;
static unsigned  g_frames;
static unsigned  g_dump_every = 3;
static double    g_fps_limit = 60.0;

static int env_int(const char *name, int def) {
    const char *v = getenv(name);
    return (v && *v) ? atoi(v) : def;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE || msg == WM_DESTROY) { g_closed = 1; g_hwnd = NULL; return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { g_closed = 1; g_hwnd = NULL; return 0; }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void win_open(void) {
    if (g_hwnd || g_closed || env_int("MLB15_NOWINDOW", 0)) return;
    int scale = env_int("MLB15_SCALE", 1);
    if (scale < 1) scale = 1;
    g_win_w = FB_DEFAULT_W * scale;
    g_win_h = FB_DEFAULT_H * scale;
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "MLB15Recomp";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    RECT r = { 0, 0, g_win_w, g_win_h };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA("MLB15Recomp", "Project Baseball",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL,
                           GetModuleHandleA(NULL), NULL);
    if (g_hwnd) {
        ShowWindow(g_hwnd, SW_SHOW);
        UpdateWindow(g_hwnd);
        printf("[display] ventana %dx%d abierta\n", g_win_w, g_win_h);
    } else {
        printf("[display] no se pudo crear la ventana (seguimos sin ventana)\n");
    }
    fflush(stdout);
}

static void pump(void) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { g_closed = 1; break; }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* ------------------------------------------------------------------------- */
/* Volcado BMP (24 bits, BGR, de abajo arriba)                                */
/* ------------------------------------------------------------------------- */

static void write_bmp(const char *path, const uint8_t *bgra, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int row = (w * 3 + 3) & ~3;
    uint32_t data_size = (uint32_t)(row * h);
    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    uint32_t file_size = 54 + data_size;
    memcpy(hdr + 2, &file_size, 4);
    uint32_t off = 54;
    memcpy(hdr + 10, &off, 4);
    uint32_t hs = 40;
    memcpy(hdr + 14, &hs, 4);
    memcpy(hdr + 18, &w, 4);
    int32_t nh = h;
    memcpy(hdr + 22, &nh, 4);
    uint16_t planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &data_size, 4);
    fwrite(hdr, 1, sizeof(hdr), f);
    uint8_t *line = (uint8_t *)malloc((size_t)row);
    if (!line) { fclose(f); return; }
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t *src = bgra + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            line[x * 3 + 0] = src[x * 4 + 0];
            line[x * 3 + 1] = src[x * 4 + 1];
            line[x * 3 + 2] = src[x * 4 + 2];
        }
        fwrite(line, 1, (size_t)row, f);
    }
    free(line);
    fclose(f);
}

unsigned port_display_frames(void) { return g_frames; }

static void maybe_dump(void) {
    /* Capturas solo para pruebas: sin ventana (cada 3 frames) o si se piden con
     * MLB15_FRAMES. Jugando con ventana no se escribe nada (eran ~30 MB/s). */
    static int on = -1;
    if (on < 0) {
        int nowin = env_int("MLB15_NOWINDOW", 0);
        g_dump_every = (unsigned)env_int("MLB15_FRAMES", nowin ? 3 : 0);
        on = nowin || getenv("MLB15_FRAMES");
    }
    if (!on) return;
    if (g_frames <= 3 || (g_dump_every && (g_frames % g_dump_every) == 0)) {
        char dir[MAX_PATH], path[MAX_PATH];
        GetCurrentDirectoryA(sizeof(dir), dir);
        snprintf(path, sizeof(path), "%s\\frames", dir);
        CreateDirectoryA(path, NULL);
        snprintf(path, sizeof(path), "%s\\frames\\frame_%05u.bmp", dir, g_frames);
        write_bmp(path, (const uint8_t *)g_pixels, g_w, g_h);
        if (g_frames <= 3 || (g_frames % 300) == 0)
            printf("[display] frame %u -> %s (%dx%d)\n", g_frames, path, g_w, g_h);
    }
}

/* ------------------------------------------------------------------------- */
/* Presentacion                                                              */
/* ------------------------------------------------------------------------- */

void port_brillo_aplica(void);   /* main.c */

void gpu_sync_all(void);   /* gpu_vk.c: completa las escenas en vuelo (copias a la memoria guest) */
/* ¿Se va a usar la memoria guest del búfer en este fotograma? Solo para el BMP de prueba
 * (maybe_dump) o sin ventana Vulkan con GDI; con la ventana normal la imagen sale de la GPU
 * y no hace falta esperar a que las escenas en vuelo vuelvan a la memoria. */
static int memoria_necesaria(void) {
    static int nowin = -1, every = -1;
    if (nowin < 0) { nowin = env_int("MLB15_NOWINDOW", 0); every = env_int("MLB15_FRAMES", nowin ? 3 : 0); if (!nowin && !getenv("MLB15_FRAMES")) every = 0; }
    unsigned f = g_frames + 1;
    if ((nowin || every) && (f <= 3 || (every && f % (unsigned)every == 0))) return 1;
    return !nowin && !gpu_has_window();
}
static void present(uint32_t base, int w, int h, uint32_t pitch, uint32_t format) {
    if (memoria_necesaria()) gpu_sync_all();
    port_brillo_aplica();
    if (!base || w <= 0 || h <= 0 || w > 1920 || h > 1080) return;
    if (!vita_mem_ptr(base, (uint32_t)(pitch * (uint32_t)h * 4u))) return;

    size_t need = (size_t)w * (size_t)h;
    if (need > g_pixels_cap) {
        uint32_t *p = (uint32_t *)realloc(g_pixels, need * 4);
        if (!p) return;
        g_pixels = p;
        g_pixels_cap = need;
    }

    int swap_rb = env_int("MLB15_SWAPRB", 1);
    const uint8_t *src = (const uint8_t *)vita_mem_ptr(base, (uint32_t)(pitch * (uint32_t)h * 4u));
    for (int y = 0; y < h; y++) {
        const uint32_t *row = (const uint32_t *)(src + (size_t)y * pitch * 4);
        uint32_t *dst = g_pixels + (size_t)y * w;
        if (swap_rb) {
            for (int x = 0; x < w; x++) {
                uint32_t v = row[x];
                dst[x] = (v & 0xFF00FF00u) | ((v & 0x00FF0000u) >> 16) | ((v & 0x000000FFu) << 16);
            }
        } else {
            memcpy(dst, row, (size_t)w * 4);
        }
    }

    g_base = base; g_pitch = pitch; g_w = w; g_h = h; g_format = format;
    g_frames++;
    {   /* FPS en el registro cada 5 s (para ver si el juego va lento) */
        static LARGE_INTEGER f0, t0;
        static unsigned n0;
        LARGE_INTEGER now;
        if (!f0.QuadPart) { QueryPerformanceFrequency(&f0); QueryPerformanceCounter(&t0); n0 = g_frames; }
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - t0.QuadPart) / (double)f0.QuadPart;
        if (dt >= 5.0) {
            extern double g_prof_tex, g_prof_wait, g_prof_v3k;
            { extern void arranque(const char *fmt, ...); static unsigned na;
              if (na++ < 3 || na % 30 == 0) arranque("fps %.1f (%ux%u), fotograma %u", (g_frames - n0) / dt, w, h, g_frames); }
            printf("[fps] %.1f (%ux%u) | por segundo: draws v3k %.0f ms (texturas %.0f ms), esperas GPU %.0f ms\n", (g_frames - n0) / dt, w, h,
                   g_prof_v3k * 1000.0 / dt, g_prof_tex * 1000.0 / dt, g_prof_wait * 1000.0 / dt);
            extern unsigned g_drop_lleno, g_drop_vb, g_drop_ds, g_vb_max;
            printf("[fps]   descartados: escena llena %u (vértices %u), descriptores %u | vértices máx %u KB\n", g_drop_lleno, g_drop_vb,
                   g_drop_ds, g_vb_max >> 10);
            { extern unsigned g_ubt_cambios; printf("[fps]   uniformes que cambiaron tras el draw: %u\n", g_ubt_cambios); g_ubt_cambios = 0; }
            { extern unsigned g_cnt_once, g_cnt_escenas, g_cnt_vuelta;
              printf("[fps]   por segundo: escenas %u (vuelven a memoria %u), subidas %u\n", g_cnt_escenas, g_cnt_vuelta, g_cnt_once);
              g_cnt_once = g_cnt_escenas = g_cnt_vuelta = 0; }
            { extern double g_espera_mot[8];
              printf("[fps]   esperas GPU (ms/s): lineal %.0f, misma sup. %.0f, contexto %.0f, todas %.0f, textura %.0f, sincrono %.0f, subidas %.0f, Finish %.0f\n",
                     g_espera_mot[0] * 1000 / dt, g_espera_mot[1] * 1000 / dt, g_espera_mot[2] * 1000 / dt, g_espera_mot[3] * 1000 / dt,
                     g_espera_mot[4] * 1000 / dt, g_espera_mot[5] * 1000 / dt, g_espera_mot[6] * 1000 / dt, g_espera_mot[7] * 1000 / dt);
              for (int q = 0; q < 8; q++) g_espera_mot[q] = 0; }
            { extern double g_dv3k_t[8];
              printf("[fps]   draw_v3k (ms/s): preparacion+vertices %.0f, tris+estado %.0f, uniformes %.0f, texturas+trazas %.0f, envio %.0f | draws/s %.0f, vertices/draw %.0f\n",
                     g_dv3k_t[0] * 1000 / dt, g_dv3k_t[1] * 1000 / dt, g_dv3k_t[2] * 1000 / dt, g_dv3k_t[3] * 1000 / dt, g_dv3k_t[4] * 1000 / dt,
                     g_dv3k_t[5] / dt, g_dv3k_t[5] > 0 ? g_dv3k_t[6] / g_dv3k_t[5] : 0.0);
              for (int q = 0; q < 8; q++) g_dv3k_t[q] = 0; }
            { extern double g_envio_t[8];
              printf("[fps]   envio (ms/s): pipeline %.0f, texturas %.0f, asignar %.0f, uniformes %.0f, descriptores %.0f, copias %.0f, ordenes %.0f\n",
                     g_envio_t[0] * 1000 / dt, g_envio_t[1] * 1000 / dt, g_envio_t[2] * 1000 / dt, g_envio_t[3] * 1000 / dt,
                     g_envio_t[4] * 1000 / dt, g_envio_t[5] * 1000 / dt, g_envio_t[6] * 1000 / dt);
              for (int q = 0; q < 8; q++) g_envio_t[q] = 0; }
            { extern unsigned g_ib_max, g_mb_max, g_up_max;
              printf("[fps]   por escena (máx): vértices %u KB, índices %u KB, uniformes %u KB, subidas %u KB\n", g_vb_max >> 10, g_ib_max >> 10, g_mb_max >> 10, g_up_max >> 10);
              g_ib_max = g_mb_max = g_up_max = 0; }
            g_drop_lleno = g_drop_vb = g_drop_ds = g_vb_max = 0;
            g_prof_tex = g_prof_wait = g_prof_v3k = 0;
            t0 = now; n0 = g_frames;
        }
    }

    maybe_dump();

    /* Limite de frames: si el juego no espera, no lo dejamos girar a 3000 fps. */
    if (g_fps_limit > 0.0) {
        static int init;
        if (!init) {
            QueryPerformanceFrequency(&g_qpc_freq); QueryPerformanceCounter(&g_qpc_last); init = 1;
            const char *e = getenv("MLB15_FPS");   /* antes solo estaba documentado */
            if (e && atof(e) > 0.0) g_fps_limit = atof(e);
            else {   /* MLB15.ini [video] fps_max (ventana de opciones): 30 = ahorro, 60 */
                extern int launcher_ini_int(const char *section, const char *key, int def);
                int m = launcher_ini_int("video", "fps_max", 60);
                if (m >= 10 && m <= 240) g_fps_limit = (double)m;
            }
        }
        /* Horario fijo (cada fotograma a last + 1/límite) en vez de «desde ahora»: antes Sleep
         * redondeaba hacia arriba y el error se acumulaba (límite 30 -> ~26 FPS). Se duerme hasta
         * ~1,5 ms antes y el resto se espera cediendo la CPU. Si va retrasado, no recupera. */
        static int periodo;
        if (!periodo) { timeBeginPeriod(1); periodo = 1; }
        double f = (double)g_qpc_freq.QuadPart, target = 1.0 / g_fps_limit;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double t = now.QuadPart / f, plazo = g_qpc_last.QuadPart / f + target;
        if (t < plazo) {
            double falta = plazo - t;
            if (falta > 0.002) Sleep((DWORD)((falta - 0.0015) * 1000.0));
            do { SwitchToThread(); QueryPerformanceCounter(&now); } while (now.QuadPart / f < plazo);
            g_qpc_last.QuadPart = (LONGLONG)(plazo * f);
        } else if (t - plazo > target) {
            g_qpc_last = now;   /* muy retrasado: nuevo horario desde ahora */
        } else {
            g_qpc_last.QuadPart = (LONGLONG)(plazo * f);
        }
    }

    /* Con la ventana SDL + Vulkan del renderizador, se presenta ahí. */
    if (gpu_has_window()) { gpu_present(base, (uint32_t)w, (uint32_t)h, pitch); return; }

    win_open();
    if (!g_hwnd) return;
    pump();
    if (!g_hwnd) return;

    HDC dc = GetDC(g_hwnd);
    if (!dc) return;
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;      /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, g_win_w, g_win_h, 0, 0, w, h,
                  g_pixels, &bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(g_hwnd, dc);

    if ((g_frames % 30) == 0) {
        char title[160];
        snprintf(title, sizeof(title), "MLB 15 - %u frames (%.0fx%u) - %lu draws",
                 g_frames, (double)g_fps_limit, g_h, 0ul);
        SetWindowTextA(g_hwnd, title);
    }
    if ((g_frames % 120) == 0) { printf("[display] %u frames presentados\n", g_frames); fflush(stdout); }
}

/* ------------------------------------------------------------------------- */
/* HLE de SceDisplay                                                         */
/* ------------------------------------------------------------------------- */

/* Presenta un SceDisplayFrameBuf guest completo (0x18 bytes). Lo usan tanto
 * sceDisplaySetFrameBuf como la cola de presentacion de GXM. */
/* Direcciones que el juego ha puesto en pantalla (gxm.c no les aplica sRGB). */
static uint32_t g_fb_addr[8];
int port_display_is_framebuf(uint32_t addr) {
    for (int i = 0; i < 8; i++) if (g_fb_addr[i] == addr) return 1;
    return 0;
}
static void fb_note(uint32_t addr) {
    static int n;
    if (!addr || port_display_is_framebuf(addr)) return;
    g_fb_addr[n++ & 7] = addr;
}

void port_display_present_guest(uint32_t p) {
    if (!p || !vita_mem_ptr(p, 0x18)) return;
    uint32_t base = vita_read32(p + 4);
    fb_note(base);
    uint32_t pitch = vita_read32(p + 8);
    uint32_t fmt = vita_read32(p + 12);
    uint32_t w = vita_read32(p + 16);
    uint32_t h = vita_read32(p + 20);
    if (w == 0 || w > 1920) w = FB_DEFAULT_W;
    if (h == 0 || h > 1080) h = FB_DEFAULT_H;
    if (pitch == 0) pitch = w;
    {   /* MLB15_NEGRO (gxm.c): fracción negra de la franja baja de la pantalla */
        static int on = -1;
        if (on < 0) on = getenv("MLB15_NEGRO") ? 1 : 0;
        const uint8_t *px = on ? (const uint8_t *)vita_mem_ptr(base, pitch * h * 4) : NULL;
        if (px) {
            unsigned nb = 0, nt = 0;
            for (uint32_t y = h * 72 / 100; y < h; y += 2)
                for (uint32_t x = 0; x < w; x += 2) {
                    const uint8_t *q = px + ((size_t)y * pitch + x) * 4;
                    nt++;
                    if (q[0] + q[1] + q[2] < 36) nb++;
                }
            double negro = nt ? (double)nb / nt : 0.0;
            static int cuna = -1;   /* MLB15_NEGRO=2: solo la cuña junto al plato (negro puro ahí, arriba normal) */
            if (cuna < 0) { const char *e = getenv("MLB15_NEGRO"); cuna = e && *e == '2'; }
            if (cuna) {
                unsigned c0 = 0, ct = 0, a0 = 0, at = 0;
                for (uint32_t y = h * 81 / 100; y < h * 87 / 100; y += 2)
                    for (uint32_t x = w * 6 / 100; x < w * 42 / 100; x += 2) {
                        const uint8_t *q = px + ((size_t)y * pitch + x) * 4;
                        ct++;
                        if (q[0] + q[1] + q[2] < 6) c0++;
                    }
                for (uint32_t y = h * 20 / 100; y < h * 50 / 100; y += 4)
                    for (uint32_t x = 0; x < w; x += 4) {
                        const uint8_t *q = px + ((size_t)y * pitch + x) * 4;
                        at++;
                        if (q[0] + q[1] + q[2] < 36) a0++;
                    }
                negro = (ct && c0 * 100 > ct * 45 && a0 * 100 < at * 30) ? 1.0 : 0.0;
            }
            extern int port_rec_frame(double black);
            int k = port_rec_frame(negro);
            if (k >= 0) {   /* la imagen de ese fotograma (RGBA, w x h) */
                char path[64];
                snprintf(path, sizeof(path), "negro_%d_imagen.raw", k);
                FILE *f = fopen(path, "wb");
                if (f) { for (uint32_t y = 0; y < h; y++) fwrite(px + (size_t)y * pitch * 4, 4, w, f); fclose(f); }
            }
        }
    }
    present(base, (int)w, (int)h, pitch, fmt);
}

void vita_hle_sceDisplaySetFrameBuf(void) {
    /* r0 = SceDisplayFrameBuf*, r1 = sync */
    uint32_t p = r0;
    if (!p) { printf("[display] SetFrameBuf(NULL): pantalla en negro\n"); r0 = 0; return; }
    static int once;
    if (!once) {
        once = 1;
        printf("[display] SetFrameBuf(base=0x%08X pitch=%u %ux%u fmt=0x%X sync=%u size=0x%X)\n",
               vita_read32(p + 4), vita_read32(p + 8), vita_read32(p + 16), vita_read32(p + 20),
               vita_read32(p + 12), r1, vita_read32(p));
        fflush(stdout);
    }
    port_display_present_guest(p);
    r0 = 0;
}

void vita_hle_sceDisplayGetFrameBuf(void) {
    uint32_t p = r0;
    if (p && vita_mem_ptr(p, 0x18)) {
        vita_write32(p + 0, 0x18);
        vita_write32(p + 4, g_base);
        vita_write32(p + 8, g_pitch);
        vita_write32(p + 12, g_format);
        vita_write32(p + 16, (uint32_t)g_w);
        vita_write32(p + 20, (uint32_t)g_h);
    }
    r0 = 0;
}

void vita_hle_sceDisplayWaitSetFrameBuf(void) { r0 = 0; }
void vita_hle_sceDisplayWaitVblankStart(void) { r0 = 0; }

void port_display_shutdown(void) {
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = NULL; }
    free(g_pixels);
    g_pixels = NULL;
    g_pixels_cap = 0;
}
