/* hle_savedialog.c — sceSaveDataDialog: la pantalla del sistema de la Vita que lista las partidas.
 *
 * Antes terminaba en el acto con «OK» y sin elegir ranura: al CARGAR (Franquicia, Carrera...) el
 * juego no recibía ninguna partida y decía «Load failed»; al guardar desde la lista recibía la
 * ranura 0 (todo acababa en FRANCHISE_000 / AI_SLIDERS_000 compartiendo slot_0.bin).
 *
 * Ahora, en modo LISTA, se abre una ventana con las ranuras que pide el juego (título, subtítulo y
 * fecha de sce_slots\slot_N.bin): teclado (flechas, Intro, Esc), ratón (doble clic) o mando de Xbox
 * (cruceta, A, B). Sin ventana (MLB15_NOWINDOW) elige la primera con datos (cargar) o la enfocada.
 * El resto de modos (ranura fija, mensajes, barra de progreso) contestan OK/Sí con la ranura en curso.
 *
 * SceSaveDataDialogParam (SDK): +0x00 sdkVersion, +0x04 SceCommonDialogParam (76 B; magic en +0x4C),
 *   +0x50 mode (1 fija, 2 lista, 3 mensaje de usuario, 4 de sistema, 5 código de error, 6 progreso),
 *   +0x54 dispType (1 guardar, 2 cargar, 3 borrar), +0x58 fixedParam, +0x5C listParam, ...
 * ListParam: +0 slotList (SceAppUtilSaveDataSlot[16 B]: id, status, userParam, emptyParam), +4 tamaño,
 *   +8 focusPos, +0xC focusId, +0x10 listTitle. FixedParam: +0 targetSlot (id primero).
 * Result: +0 mode, +4 result (0 OK, 1 cancelado), +8 buttonId (1 OK/Sí, 2 No), +0xC slotId,
 *   +0x10 slotInfo (SlotParam* opcional), +0x14 userdata. */
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comctl32.lib")
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"
#include "port.h"
#include "ui_estilo.h"

#define SLOT_PARAM_SIZE 0x348u
#define MAXS 64

static volatile int g_estado;           /* 0 nada, 1 en marcha, 2 terminado */
static uint32_t g_modo, g_tipo, g_slot, g_resultado, g_boton;
static int g_n, g_sel;
static struct { uint32_t id; int hay; uint64_t t; wchar_t tit[64], sub[128], fecha[48]; } g_e[MAXS];
static wchar_t g_titulo[128];
static HWND g_wnd, g_lista;

static void slot_ruta(uint32_t id, char *out, size_t max) {
    char g[64];
    snprintf(g, sizeof(g), "savedata0:sce_slots/slot_%u.bin", id);
    port_vfs_path(g, out, max);
}

static int slot_lee(uint32_t id, uint8_t *p) {
    char r[MAX_PATH];
    slot_ruta(id, r, sizeof(r));
    FILE *f = fopen(r, "rb");
    if (!f) return 0;
    memset(p, 0, SLOT_PARAM_SIZE);
    fread(p, 1, SLOT_PARAM_SIZE, f);
    fclose(f);
    /* fecha: la del archivo (el juego no rellena modifiedTime) */
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExA(r, GetFileExInfoStandard, &a)) {
        FILETIME lt; SYSTEMTIME st;
        FileTimeToLocalFileTime(&a.ftLastWriteTime, &lt);
        FileTimeToSystemTime(&lt, &st);
        memcpy(p + 0x308, &st, sizeof(st) < 16 ? sizeof(st) : 16);   /* solo para mostrarla aquí */
    }
    return 1;
}

static void u8_a_w(const char *s, wchar_t *o, int n) { MultiByteToWideChar(CP_UTF8, 0, s, -1, o, n); o[n - 1] = 0; }

/* ---------------------------------------------------------------- ventana */

enum { ID_LISTA = 10, ID_ACEPTAR, ID_CANCELAR };
#define VW 620
#define VH 520

static void termina(uint32_t res, int idx) {
    g_resultado = res;
    g_boton = res == 0 ? 1 : 0;
    if (res == 0 && idx >= 0 && idx < g_n) g_slot = g_e[idx].id;
    if (g_wnd) DestroyWindow(g_wnd);
}

static LRESULT CALLBACK lista_sub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    (void)id; (void)ref;
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE)) {
        termina(w == VK_RETURN ? 0 : 1, (int)SendMessageW(h, LB_GETCURSEL, 0, 0));
        return 0;
    }
    if (m == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return DefSubclassProc(h, m, w, l);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == ID_ACEPTAR || (LOWORD(w) == ID_LISTA && HIWORD(w) == LBN_DBLCLK)) {
            termina(0, (int)SendMessageW(g_lista, LB_GETCURSEL, 0, 0)); return 0;
        }
        if (LOWORD(w) == ID_CANCELAR) { termina(1, -1); return 0; }
        break;
    case WM_TIMER: {   /* mando de Xbox: cruceta/stick, A acepta, B cancela */
        typedef DWORD(WINAPI * XGet)(DWORD, void *);
        static XGet xget; static int cargado; static WORD antes;
        if (!cargado) { HMODULE x = LoadLibraryA("xinput1_4.dll"); if (!x) x = LoadLibraryA("xinput9_1_0.dll");
                        xget = x ? (XGet)(void *)GetProcAddress(x, "XInputGetState") : NULL; cargado = 1; }
        if (!xget) return 0;
        struct { DWORD pkt; WORD bot; BYTE lt, rt; SHORT lx, ly, rx, ry; } s;
        WORD b = 0;
        for (DWORD i = 0; i < 4; i++) if (xget(i, &s) == 0) {
            b |= s.bot;
            if (s.ly > 16000) b |= 0x0001; if (s.ly < -16000) b |= 0x0002;
        }
        WORD nuevo = b & ~antes;
        antes = b;
        int cur = (int)SendMessageW(g_lista, LB_GETCURSEL, 0, 0);
        if ((nuevo & 0x0001) && cur > 0) SendMessageW(g_lista, LB_SETCURSEL, cur - 1, 0);
        if ((nuevo & 0x0002) && cur + 1 < g_n) SendMessageW(g_lista, LB_SETCURSEL, cur + 1, 0);
        if (nuevo & 0x1000) termina(0, cur);            /* A */
        else if (nuevo & 0x2000) termina(1, -1);        /* B */
        return 0;
    }
    case WM_MEASUREITEM: ((MEASUREITEMSTRUCT *)l)->itemHeight = (UINT)ui_px(58); return TRUE;
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)l;
        if (d->CtlType == ODT_BUTTON) { ui_pinta_boton(d); return TRUE; }
        if ((int)d->itemID < 0 || (int)d->itemID >= g_n) return TRUE;
        int sel = (d->itemState & ODS_SELECTED) != 0, i = (int)d->itemID;
        HBRUSH fb = CreateSolidBrush(UI_FONDO);
        FillRect(d->hDC, &d->rcItem, fb);
        DeleteObject(fb);
        RECT r = d->rcItem;
        ui_rect_redondo(d->hDC, r.left + ui_px(2), r.top + ui_px(3), r.right - r.left - ui_px(4), r.bottom - r.top - ui_px(6), ui_px(8),
                        sel ? UI_ACENTO : UI_PANEL, sel ? UI_ACENTO2 : UI_BORDE);
        SetBkMode(d->hDC, TRANSPARENT);
        SetTextColor(d->hDC, UI_TEXTO);
        HGDIOBJ o = SelectObject(d->hDC, ui_fuente(UI_FUERTE));
        RECT t = { r.left + ui_px(16), r.top + ui_px(9), r.right - ui_px(150), r.top + ui_px(30) };
        DrawTextW(d->hDC, g_e[i].tit, -1, &t, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(d->hDC, ui_fuente(UI_PEQUENO));
        SetTextColor(d->hDC, sel ? RGB(225, 235, 250) : UI_TENUE);
        RECT f = { r.right - ui_px(160), r.top + ui_px(11), r.right - ui_px(14), r.top + ui_px(30) };
        DrawTextW(d->hDC, g_e[i].fecha, -1, &f, DT_SINGLELINE | DT_RIGHT | DT_NOPREFIX);
        RECT s2 = { r.left + ui_px(16), r.top + ui_px(30), r.right - ui_px(14), r.bottom - ui_px(6) };
        DrawTextW(d->hDC, g_e[i].sub, -1, &s2, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(d->hDC, o);
        return TRUE;
    }
    case WM_CTLCOLORLISTBOX: { static HBRUSH b; if (!b) b = CreateSolidBrush(UI_FONDO); return (LRESULT)b; }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        ui_pinta_fondo(h, dc, &rc);
        ui_texto(h, dc, g_titulo, 28, 70, VW - 56, UI_TITULO);
        ui_texto(h, dc, L"Flechas o cruceta para elegir · Intro / A para aceptar · Esc / B para cancelar", 28, 106, VW - 56, UI_PEQUENO_TENUE);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE: termina(1, -1); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI hilo(void *x) {
    (void)x;
    ui_init();
    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = proc; wc.hInstance = hi; wc.lpszClassName = L"MLB15Partidas";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r = { 0, 0, ui_px(VW), ui_px(VH) };
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const char *cap = getenv("MLB15_PARTIDAS_CAPTURA");   /* pruebas: dibujarla fuera de pantalla y guardarla */
    g_wnd = CreateWindowExW(cap ? WS_EX_TOOLWINDOW : WS_EX_TOPMOST, wc.lpszClassName, L"Project Baseball — Partidas guardadas", WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                            cap ? -8000 : wa.left + (wa.right - wa.left - ww) / 2, cap ? -8000 : wa.top + (wa.bottom - wa.top - wh) / 2, ww, wh, NULL, NULL, hi, NULL);
    ui_ventana_oscura(g_wnd);
    g_lista = CreateWindowExW(0, L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_OWNERDRAWFIXED | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                              ui_px(28), ui_px(136), ui_px(VW - 56), ui_px(VH - 136 - 80), g_wnd, (HMENU)ID_LISTA, hi, NULL);
    for (int i = 0; i < g_n; i++) SendMessageW(g_lista, LB_ADDSTRING, 0, (LPARAM)g_e[i].tit);
    SendMessageW(g_lista, LB_SETCURSEL, g_sel, 0);
    SetWindowSubclass(g_lista, lista_sub, 1, 0);
    SetWindowTheme(g_lista, L"DarkMode_Explorer", NULL);   /* barra de desplazamiento oscura */
    ui_boton(g_wnd, ID_CANCELAR, L"Cancelar", VW - 28 - 150 - 10 - 120, VH - 62, 120, 40, UIB_NORMAL);
    ui_boton(g_wnd, ID_ACEPTAR, g_tipo == 1 ? L"Guardar aquí" : g_tipo == 3 ? L"Borrar" : L"Cargar", VW - 28 - 150, VH - 62, 150, 40,
             g_tipo == 3 ? UIB_PELIGRO : UIB_PRIMARIO);
    if (cap) {
        ShowWindow(g_wnd, SW_SHOWNOACTIVATE); UpdateWindow(g_wnd);
        MSG mm;
        for (int i = 0; i < 50; i++) { while (PeekMessageW(&mm, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&mm); Sleep(10); }
        extern void opciones_captura(HWND h, const char *path);
        opciones_captura(g_wnd, cap);
        termina(0, g_sel);
    }
    SetTimer(g_wnd, 1, 50, NULL);
    if (!cap) ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
    SetFocus(g_lista);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    g_wnd = NULL;
    printf("[partidas] %s: ranura %u\n", g_resultado ? "cancelado" : "elegida", g_slot);
    fflush(stdout);
    g_estado = 2;
    return 0;
}

/* ---------------------------------------------------------------- API */

static void lista(uint32_t lp) {
    uint32_t sl = vita_read32(lp), n = vita_read32(lp + 4), foco = vita_read32(lp + 0xC), tit = vita_read32(lp + 0x10);
    char t8[128] = "";
    if (tit) port_guest_str(tit, t8, sizeof(t8));
    if (t8[0]) u8_a_w(t8, g_titulo, 128);
    else wcscpy(g_titulo, g_tipo == 1 ? L"Guardar partida" : g_tipo == 3 ? L"Borrar partida" : L"Cargar partida");
    g_n = 0; g_sel = 0;
    uint8_t p[SLOT_PARAM_SIZE];
    for (uint32_t i = 0; i < n && g_n < MAXS; i++) {
        uint32_t id = vita_read32(sl + 16 * i), vacio = vita_read32(sl + 16 * i + 12);
        printf("[partidas]   ranura %u estado %u userParam %u vacío 0x%X\n", id, vita_read32(sl + 16 * i + 4), vita_read32(sl + 16 * i + 8), vacio);
        int hay = slot_lee(id, p);
        if (!hay && g_tipo != 1) continue;                  /* cargar/borrar: solo las que existen */
        g_e[g_n].id = id; g_e[g_n].hay = hay;
        if (hay) {
            char a[600];
            memcpy(a, p + 4, 64); a[64] = 0; u8_a_w(a, g_e[g_n].tit, 64);
            memcpy(a, p + 0x44, 128); a[128] = 0; u8_a_w(a, g_e[g_n].sub, 128);
            SYSTEMTIME st; memcpy(&st, p + 0x308, sizeof(st));
            { FILETIME ft; SystemTimeToFileTime(&st, &ft); g_e[g_n].t = st.wYear ? ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) : 0; }
            if (st.wYear) swprintf(g_e[g_n].fecha, 48, L"%02u/%02u/%04u %02u:%02u", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute);
            else g_e[g_n].fecha[0] = 0;
            if (!g_e[g_n].tit[0]) swprintf(g_e[g_n].tit, 64, L"Partida %u", id);
        } else {
            char a[128] = "";
            if (vacio && vita_read32(vacio)) port_guest_str(vita_read32(vacio), a, sizeof(a));   /* emptyParam->title */
            if (a[0]) u8_a_w(a, g_e[g_n].tit, 64); else wcscpy(g_e[g_n].tit, L"Nueva partida guardada");
            wcscpy(g_e[g_n].sub, L"(vacía)");
            g_e[g_n].fecha[0] = 0;
            g_e[g_n].t = 0;
        }
        if (id == foco) g_sel = g_n;
        g_n++;
    }
    if (g_tipo == 2) {   /* cargar: marcada la más reciente (como la Vita) */
        int best = -1;
        for (int i = 0; i < g_n; i++) if (g_e[i].hay && (best < 0 || g_e[i].t > g_e[best].t)) best = i;
        if (best >= 0) g_sel = best;
    }
    printf("[partidas] lista (%s): %d de %u ranuras, foco %u\n", g_tipo == 1 ? "guardar" : g_tipo == 2 ? "cargar" : "borrar", g_n, n, foco);
    fflush(stdout);
}

void savedialog_init(uint32_t param) {
    g_modo = vita_read32(param + 0x50);
    g_tipo = vita_read32(param + 0x54);
    g_resultado = 0; g_boton = 1;
    printf("[partidas] SaveDataDialog modo %u tipo %u\n", g_modo, g_tipo);
    fflush(stdout);
    uint32_t fp = vita_read32(param + 0x58), lp = vita_read32(param + 0x5C);
    {   uint32_t um = vita_read32(param + 0x60), sm = vita_read32(param + 0x64), ec = vita_read32(param + 0x68);
        if (sm) printf("[partidas]   mensaje del sistema: tipo %u valor %u ranura? %u\n", vita_read32(sm), vita_read32(sm + 4), vita_read32(sm + 8));
        if (um) { char t[256] = ""; port_guest_str(vita_read32(um + 4), t, sizeof(t)); printf("[partidas]   mensaje: botones %u '%s'\n", vita_read32(um), t); }
        if (ec) printf("[partidas]   código de error 0x%08X\n", vita_read32(ec));
        printf("[partidas]   magic 0x%08X (esperado 0x%08X)\n", vita_read32(param + 0x4C), 0xC0D1A109u + (param + 4));
    }
    if (g_modo == 1 && fp) g_slot = vita_read32(fp);   /* ranura fija */
    if (g_modo == 2 && lp) {
        lista(lp);
        const char *nw = getenv("MLB15_NOWINDOW");
        if (g_n == 0) { g_resultado = 1; g_boton = 0; g_estado = 2; return; }   /* nada que cargar: como cancelar */
        if (((nw && *nw == '1') || getenv("MLB15_PARTIDAS_AUTO")) && !getenv("MLB15_PARTIDAS_CAPTURA")) {
            int k = g_sel;
            const char *fr = getenv("MLB15_PARTIDAS_RANURA");   /* pruebas: ranura concreta */
            if (fr) for (k = 0; k < g_n && g_e[k].id != (uint32_t)atoi(fr); k++) {}
            else
            if (g_tipo == 2) for (k = 0; k < g_n && !g_e[k].hay; k++) {}
            if (k >= g_n) k = 0;
            g_slot = g_e[k].id;
            g_estado = 2;
            printf("[partidas] sin ventana: ranura %u\n", g_slot);
            return;
        }
        g_estado = 1;
        CloseHandle(CreateThread(NULL, 0, hilo, NULL, 0, NULL));
        return;
    }
    g_estado = 2;
}

uint32_t savedialog_status(void) { return (uint32_t)g_estado; }
void savedialog_term(void) { g_estado = 0; }
void savedialog_finish(void) { if (g_wnd) PostMessageW(g_wnd, WM_CLOSE, 0, 0); else g_estado = 2; }

void savedialog_result(uint32_t res) {
    if (!res) return;
    vita_write32(res + 0, g_modo);
    vita_write32(res + 4, g_resultado);
    vita_write32(res + 8, g_boton);
    vita_write32(res + 0xC, g_slot);
    /* slotInfo = SceSaveDataDialogSlotInfo { isExist; SceAppUtilSaveDataSlotParam *slotParam; reserved[32] }
     * (el juego, 0x81003350: búfer de 0x34C en su pila apuntado desde +4). isExist = 0 le hacía mostrar
     * «no hay datos guardados» (mensaje de sistema tipo 1) sin intentar cargar. */
    uint32_t info = vita_read32(res + 0x10);
    if (info && vita_mem_ptr(info, 8)) {
        uint8_t p[SLOT_PARAM_SIZE];
        int hay = slot_lee(g_slot, p);
        vita_write32(info, hay ? 1u : 0u);
        uint32_t sp_ = vita_read32(info + 4);
        if (hay && sp_ && vita_mem_ptr(sp_, SLOT_PARAM_SIZE)) { memset(p + 0x308, 0, 16); memcpy(vita_mem_ptr(sp_, SLOT_PARAM_SIZE), p, SLOT_PARAM_SIZE); }
    }
    printf("[partidas] resultado: modo %u result %u botón %u ranura %u\n", g_modo, g_resultado, g_boton, g_slot);
    fflush(stdout);
}
