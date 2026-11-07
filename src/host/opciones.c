/* Ventana del launcher al arrancar MLB15.exe (después de localizar el juego y preparar juego.dll).
 * Diseño propio (ui_estilo.c): panel lateral con el estado y JUGAR, y a la derecha las opciones.
 * Lee y guarda MLB15.ini [video]: resolucion, pantalla_completa, escala, fps_max, brillo, nitidez,
 * suavizado, mostrar_fps, texturas_hd, jugadores_alta, sombra_jugadores y mostrar_opciones.
 * Se muestra si mostrar_opciones = 1 (por defecto) o si se mantiene Ctrl al arrancar.
 * Solo con el lanzador (doble clic): con un ELF por la línea de órdenes no aparece. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

#include "launcher.h"
#include "ui_estilo.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

int texturas_hd_dialogo(HWND padre);   /* texturas_hd.c */
int texturas_hd_cuenta(void);

enum {
    ID_RES = 100,                 /* 4 segmentos: 100..103 */
    ID_MODO = 110,                /* 2: 110..111 */
    ID_TAM = 120,                 /* 4: 120..123 */
    ID_FPS = 130,                 /* 2: 130..131 */
    ID_BRILLO = 140, ID_NITIDEZ, ID_SUAV, ID_VERFPS, ID_TEXHD, ID_JUGALTA, ID_SOMBRA, ID_MOSTRAR,
    ID_JUGAR = 200, ID_SALIR, ID_DEFECTO, ID_CONTROLES, ID_TEXGEN,
    ID_CTL_BASE = 300, ID_CTL_DEF = 400, ID_CTL_OK, ID_CTL_CANCEL
};

static HWND g_dlg;
static int g_resultado;   /* 1 = jugar, 0 = salir */
#define LADO 292          /* ancho del panel lateral (lógico) */
#define W_TOTAL 900
#define H_TOTAL 610

static int marcada(int id) { return ui_interruptor_on(GetDlgItem(g_dlg, id)); }
static int pos(int id) { return ui_deslizador_pos(GetDlgItem(g_dlg, id)); }

/* Valores del ini (con sus valores por defecto del port) */
typedef struct { int res, completa, escala, fps, brillo, nitidez, suav, verfps, texhd, jugalta, mostrar, sombra; } Opc;
static Opc leer(void) {
    Opc o;
    o.res = launcher_ini_int("video", "resolucion", 3);
    o.completa = launcher_ini_int("video", "pantalla_completa", 0);
    o.escala = launcher_ini_int("video", "escala", 0);
    o.fps = launcher_ini_int("video", "fps_max", 60);
    o.brillo = launcher_ini_int("video", "brillo", 85);
    o.nitidez = launcher_ini_int("video", "nitidez", 35);
    o.suav = launcher_ini_int("video", "suavizado", 1);
    o.verfps = launcher_ini_int("video", "mostrar_fps", 0);
    o.texhd = launcher_ini_int("video", "texturas_hd", 1);
    o.jugalta = launcher_ini_int("video", "jugadores_alta", 1);
    o.mostrar = launcher_ini_int("video", "mostrar_opciones", 1);
    o.sombra = launcher_ini_int("video", "sombra_jugadores", 1);
    return o;
}
static void poner(const Opc *o) {
    ui_segmento_elige(g_dlg, ID_RES, 4, (o->res < 1 ? 1 : o->res > 4 ? 4 : o->res) - 1);
    ui_segmento_elige(g_dlg, ID_MODO, 2, o->completa ? 1 : 0);
    ui_segmento_elige(g_dlg, ID_TAM, 4, o->escala < 0 ? 0 : o->escala > 3 ? 3 : o->escala);
    ui_segmento_elige(g_dlg, ID_FPS, 2, o->fps <= 30 ? 0 : 1);
    ui_deslizador_pon(GetDlgItem(g_dlg, ID_BRILLO), o->brillo);
    ui_deslizador_pon(GetDlgItem(g_dlg, ID_NITIDEZ), o->nitidez);
    ui_interruptor_pon(GetDlgItem(g_dlg, ID_SUAV), o->suav);
    ui_interruptor_pon(GetDlgItem(g_dlg, ID_VERFPS), o->verfps);
    ui_interruptor_pon(GetDlgItem(g_dlg, ID_TEXHD), o->texhd);
    ui_interruptor_pon(GetDlgItem(g_dlg, ID_JUGALTA), o->jugalta);
    ui_interruptor_pon(GetDlgItem(g_dlg, ID_MOSTRAR), o->mostrar);
    ui_interruptor_pon(GetDlgItem(g_dlg, ID_SOMBRA), o->sombra);
    InvalidateRect(g_dlg, NULL, FALSE);
}
static void guardar(void) {
    launcher_ini_set_int("video", "resolucion", ui_segmento_actual(g_dlg, ID_RES, 4) + 1);
    launcher_ini_set_int("video", "pantalla_completa", ui_segmento_actual(g_dlg, ID_MODO, 2) == 1);
    launcher_ini_set_int("video", "escala", ui_segmento_actual(g_dlg, ID_TAM, 4));
    launcher_ini_set_int("video", "fps_max", ui_segmento_actual(g_dlg, ID_FPS, 2) == 0 ? 30 : 60);
    launcher_ini_set_int("video", "brillo", pos(ID_BRILLO));
    launcher_ini_set_int("video", "nitidez", pos(ID_NITIDEZ));
    launcher_ini_set_int("video", "suavizado", marcada(ID_SUAV));
    launcher_ini_set_int("video", "mostrar_fps", marcada(ID_VERFPS));
    launcher_ini_set_int("video", "texturas_hd", marcada(ID_TEXHD));
    launcher_ini_set_int("video", "jugadores_alta", marcada(ID_JUGALTA));
    launcher_ini_set_int("video", "mostrar_opciones", marcada(ID_MOSTRAR));
    launcher_ini_set_int("video", "sombra_jugadores", marcada(ID_SOMBRA));
}

/* ---- Controles: botón de la Vita -> tecla, en MLB15.ini [controles] (nombres de tecla de SDL;
 * input.c los lee y mandan sobre [teclas]). El mando sigue la distribución Xbox. ---- */
static const struct { const wchar_t *nom; const char *clave; SDL_Scancode def; } g_acc[] = {
    { L"Cruz", "cruz", SDL_SCANCODE_X },                 { L"Círculo", "circulo", SDL_SCANCODE_C },
    { L"Cuadrado", "cuadrado", SDL_SCANCODE_Z },         { L"Triángulo", "triangulo", SDL_SCANCODE_V },
    { L"L", "l", SDL_SCANCODE_Q },                       { L"R", "r", SDL_SCANCODE_E },
    { L"Start", "start", SDL_SCANCODE_RETURN },          { L"Select", "select", SDL_SCANCODE_RSHIFT },
    { L"Cruceta arriba", "arriba", SDL_SCANCODE_UP },    { L"Cruceta abajo", "abajo", SDL_SCANCODE_DOWN },
    { L"Cruceta izquierda", "izquierda", SDL_SCANCODE_LEFT }, { L"Cruceta derecha", "derecha", SDL_SCANCODE_RIGHT },
    { L"Stick izq. arriba", "stick_izq_arriba", SDL_SCANCODE_W },       { L"Stick izq. abajo", "stick_izq_abajo", SDL_SCANCODE_S },
    { L"Stick izq. izquierda", "stick_izq_izquierda", SDL_SCANCODE_A }, { L"Stick izq. derecha", "stick_izq_derecha", SDL_SCANCODE_D },
    { L"Stick der. arriba", "stick_der_arriba", SDL_SCANCODE_I },       { L"Stick der. abajo", "stick_der_abajo", SDL_SCANCODE_K },
    { L"Stick der. izquierda", "stick_der_izquierda", SDL_SCANCODE_J }, { L"Stick der. derecha", "stick_der_derecha", SDL_SCANCODE_L },
};
#define NACC ((int)(sizeof(g_acc) / sizeof(g_acc[0])))
static SDL_Scancode g_tec[NACC];
static int g_esperando = -1;   /* acción que espera tecla */
static HWND g_cdlg;
static int g_cres;
#define CW 640
#define C_FILA 40
#define C_Y0 150

/* Tecla virtual de Windows -> scancode de SDL (las teclas útiles para jugar). */
static SDL_Scancode vk_a_sdl(WPARAM vk, LPARAM l) {
    int ext = (l >> 24) & 1;
    if (vk >= 'A' && vk <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + (int)(vk - 'A'));
    if (vk >= '1' && vk <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (int)(vk - '1'));
    if (vk == '0') return SDL_SCANCODE_0;
    if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) return (SDL_Scancode)(SDL_SCANCODE_KP_1 + (int)(vk - VK_NUMPAD1));
    if (vk >= VK_F1 && vk <= VK_F8) return (SDL_Scancode)(SDL_SCANCODE_F1 + (int)(vk - VK_F1));
    switch (vk) {
        case VK_NUMPAD0: return SDL_SCANCODE_KP_0;
        case VK_UP: return SDL_SCANCODE_UP;       case VK_DOWN: return SDL_SCANCODE_DOWN;
        case VK_LEFT: return SDL_SCANCODE_LEFT;   case VK_RIGHT: return SDL_SCANCODE_RIGHT;
        case VK_SPACE: return SDL_SCANCODE_SPACE; case VK_RETURN: return ext ? SDL_SCANCODE_KP_ENTER : SDL_SCANCODE_RETURN;
        case VK_TAB: return SDL_SCANCODE_TAB;     case VK_BACK: return SDL_SCANCODE_BACKSPACE;
        case VK_SHIFT: return MapVirtualKeyW((UINT)((l >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? SDL_SCANCODE_RSHIFT : SDL_SCANCODE_LSHIFT;
        case VK_CONTROL: return ext ? SDL_SCANCODE_RCTRL : SDL_SCANCODE_LCTRL;
        case VK_MENU: return ext ? SDL_SCANCODE_RALT : SDL_SCANCODE_LALT;
        case VK_INSERT: return SDL_SCANCODE_INSERT; case VK_DELETE: return SDL_SCANCODE_DELETE;
        case VK_HOME: return SDL_SCANCODE_HOME;   case VK_END: return SDL_SCANCODE_END;
        case VK_PRIOR: return SDL_SCANCODE_PAGEUP; case VK_NEXT: return SDL_SCANCODE_PAGEDOWN;
        case VK_OEM_COMMA: return SDL_SCANCODE_COMMA; case VK_OEM_PERIOD: return SDL_SCANCODE_PERIOD;
        case VK_OEM_MINUS: return SDL_SCANCODE_MINUS; case VK_OEM_PLUS: return SDL_SCANCODE_EQUALS;
        case VK_OEM_1: return SDL_SCANCODE_SEMICOLON; case VK_OEM_2: return SDL_SCANCODE_SLASH;
        case VK_OEM_3: return SDL_SCANCODE_GRAVE;  case VK_OEM_4: return SDL_SCANCODE_LEFTBRACKET;
        case VK_OEM_5: return SDL_SCANCODE_BACKSLASH; case VK_OEM_6: return SDL_SCANCODE_RIGHTBRACKET;
        case VK_OEM_7: return SDL_SCANCODE_APOSTROPHE; case VK_OEM_102: return SDL_SCANCODE_NONUSBACKSLASH;
        case VK_ADD: return SDL_SCANCODE_KP_PLUS; case VK_SUBTRACT: return SDL_SCANCODE_KP_MINUS;
        case VK_MULTIPLY: return SDL_SCANCODE_KP_MULTIPLY; case VK_DIVIDE: return SDL_SCANCODE_KP_DIVIDE;
        case VK_DECIMAL: return SDL_SCANCODE_KP_PERIOD;
    }
    return SDL_SCANCODE_UNKNOWN;   /* Esc, F9-F12 y el resto: reservadas o no admitidas */
}
/* Nombre de tecla para mostrar (en el ini va el nombre de SDL, en inglés). */
static const char *tecla_es(SDL_Scancode sc) {
    static const struct { SDL_Scancode sc; const char *n; } es[] = {
        { SDL_SCANCODE_RETURN, "Intro" }, { SDL_SCANCODE_KP_ENTER, "Intro (num.)" }, { SDL_SCANCODE_SPACE, "Espacio" },
        { SDL_SCANCODE_UP, "Flecha arriba" }, { SDL_SCANCODE_DOWN, "Flecha abajo" }, { SDL_SCANCODE_LEFT, "Flecha izq." },
        { SDL_SCANCODE_RIGHT, "Flecha dcha." }, { SDL_SCANCODE_LSHIFT, "Shift izq." }, { SDL_SCANCODE_RSHIFT, "Shift dcho." },
        { SDL_SCANCODE_LCTRL, "Ctrl izq." }, { SDL_SCANCODE_RCTRL, "Ctrl dcho." }, { SDL_SCANCODE_LALT, "Alt" },
        { SDL_SCANCODE_RALT, "Alt Gr" }, { SDL_SCANCODE_BACKSPACE, "Retroceso" }, { SDL_SCANCODE_TAB, "Tab" },
        { SDL_SCANCODE_DELETE, "Supr" }, { SDL_SCANCODE_INSERT, "Insert" }, { SDL_SCANCODE_HOME, "Inicio" },
        { SDL_SCANCODE_END, "Fin" }, { SDL_SCANCODE_PAGEUP, "Re Pág" }, { SDL_SCANCODE_PAGEDOWN, "Av Pág" },
    };
    for (size_t i = 0; i < sizeof(es) / sizeof(es[0]); i++) if (es[i].sc == sc) return es[i].n;
    return SDL_GetScancodeName(sc);
}
static void ctl_texto(int i) {
    wchar_t t[64];
    if (g_esperando == i) wcscpy(t, L"Pulsa una tecla…");
    else MultiByteToWideChar(CP_UTF8, 0, g_tec[i] ? tecla_es(g_tec[i]) : "(ninguna)", -1, t, 64);
    HWND b = GetDlgItem(g_cdlg, ID_CTL_BASE + i);
    SetWindowTextW(b, t);
    SetWindowLongPtrW(b, GWLP_USERDATA, g_esperando == i ? UIB_PRIMARIO : UIB_NORMAL);
    InvalidateRect(b, NULL, FALSE);
}
static void ctl_leer(void) {
    char v[64];
    for (int i = 0; i < NACC; i++) {
        GetPrivateProfileStringA("controles", g_acc[i].clave, "", v, sizeof(v), launcher_ini_path());
        SDL_Scancode sc = v[0] ? SDL_GetScancodeFromName(v) : SDL_SCANCODE_UNKNOWN;
        g_tec[i] = sc != SDL_SCANCODE_UNKNOWN ? sc : g_acc[i].def;
    }
}
static LRESULT CALLBACK cproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id >= ID_CTL_BASE && id < ID_CTL_BASE + NACC) {
                int prev = g_esperando;
                g_esperando = id - ID_CTL_BASE;
                if (prev >= 0) ctl_texto(prev);
                ctl_texto(g_esperando);
                SetFocus(h);
                return 0;
            }
            if (id == ID_CTL_DEF) { g_esperando = -1; for (int i = 0; i < NACC; i++) { g_tec[i] = g_acc[i].def; ctl_texto(i); } return 0; }
            if (id == ID_CTL_OK) {
                for (int i = 0; i < NACC; i++)
                    WritePrivateProfileStringA("controles", g_acc[i].clave, SDL_GetScancodeName(g_tec[i]), launcher_ini_path());
                g_cres = 1; DestroyWindow(h); return 0;
            }
            if (id == ID_CTL_CANCEL) { g_cres = 0; DestroyWindow(h); return 0; }
            break;
        }
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            if (g_esperando >= 0) {
                SDL_Scancode sc = vk_a_sdl(w, l);
                if (w == VK_ESCAPE) { int i = g_esperando; g_esperando = -1; ctl_texto(i); return 0; }
                if (sc == SDL_SCANCODE_UNKNOWN) { MessageBeep(MB_ICONWARNING); return 0; }
                for (int j = 0; j < NACC; j++)   /* la tecla ya usada en otra acción: se le quita */
                    if (j != g_esperando && g_tec[j] == sc) { g_tec[j] = 0; ctl_texto(j); }
                int i = g_esperando;
                g_esperando = -1; g_tec[i] = sc; ctl_texto(i);
                return 0;
            }
            break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC pdc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
            HDC dc = CreateCompatibleDC(pdc);
            HBITMAP bmp = CreateCompatibleBitmap(pdc, rc.right, rc.bottom);
            HGDIOBJ ob = SelectObject(dc, bmp);
            ui_pinta_fondo(h, dc, &rc);
            ui_texto(h, dc, L"Controles del teclado", 28, 72, CW - 56, UI_TITULO);
            ui_texto(h, dc, L"Pulsa un botón y luego la tecla que quieras (Esc cancela; F9-F12 están reservadas). "
                            L"El mando de Xbox va siempre con su distribución: A = Cruz, B = Círculo, X = Cuadrado, Y = Triángulo.",
                     28, 108, CW - 56, UI_PEQUENO_TENUE);
            for (int i = 0; i < NACC; i++) {
                int col = i % 2, fila = i / 2, x = 28 + col * 300, y = C_Y0 + fila * C_FILA;
                ui_texto(h, dc, g_acc[i].nom, x, y + 7, 140, UI_NORMAL);
            }
            BitBlt(pdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
            SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DRAWITEM: ui_pinta_boton((const DRAWITEMSTRUCT *)l); return TRUE;
        case WM_CLOSE: g_cres = 0; DestroyWindow(h); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
/* Ventana modal de controles (desde «Controles…»). cap: solo dibujarla y guardarla en ese BMP. */
static void captura(HWND h, const char *path);
static void controles_dialogo(HWND padre, const char *cap) {
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = cproc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"MLB15Controles";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    int filas = (NACC + 1) / 2, H = C_Y0 + filas * C_FILA + 76;
    RECT r = { 0, 0, ui_px(CW), ui_px(H) };
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    RECT pr; GetWindowRect(padre, &pr);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    g_cdlg = CreateWindowExW(WS_EX_DLGMODALFRAME | (cap ? WS_EX_TOOLWINDOW : 0), L"MLB15Controles", L"Project Baseball — Controles",
                             WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, cap ? -8000 : pr.left + ((pr.right - pr.left) - ww) / 2,
                             cap ? -8000 : pr.top + ((pr.bottom - pr.top) - wh) / 2, ww, wh, padre, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_cdlg) return;
    ui_ventana_oscura(g_cdlg);
    ctl_leer();
    for (int i = 0; i < NACC; i++) {
        int col = i % 2, fila = i / 2, x = 28 + col * 300, y = C_Y0 + fila * C_FILA;
        ui_boton(g_cdlg, ID_CTL_BASE + i, L"", x + 140, y, 140, 32, UIB_NORMAL);
        ctl_texto(i);
    }
    int y = H - 58;
    ui_boton(g_cdlg, ID_CTL_DEF, L"Por defecto", 28, y, 140, 38, UIB_NORMAL);
    ui_boton(g_cdlg, ID_CTL_CANCEL, L"Cancelar", CW - 28 - 130 - 10 - 120, y, 120, 38, UIB_NORMAL);
    ui_boton(g_cdlg, ID_CTL_OK, L"Guardar", CW - 28 - 130, y, 130, 38, UIB_PRIMARIO);
    if (cap) {
        ShowWindow(g_cdlg, SW_SHOWNOACTIVATE); UpdateWindow(g_cdlg);
        MSG mm;
        for (int i = 0; i < 50; i++) { while (PeekMessageW(&mm, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&mm); Sleep(10); }
        captura(g_cdlg, cap);
        DestroyWindow(g_cdlg);
        while (PeekMessageW(&mm, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&mm);
        return;
    }
    EnableWindow(padre, FALSE);
    ShowWindow(g_cdlg, SW_SHOW);
    g_esperando = -1;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* esperando tecla: las teclas van a la ventana (sin que IsDialogMessage se coma flechas, Tab, Intro...) */
        if (g_esperando >= 0 && (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN)) { msg.hwnd = g_cdlg; DispatchMessageW(&msg); continue; }
        if (!IsDialogMessageW(g_cdlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    EnableWindow(padre, TRUE);
    SetForegroundWindow(padre);
}

/* ---- ventana principal ---- */

static void seccion(HWND h, HDC dc, const wchar_t *t, int y) {
    int x = LADO + 32;
    SetTextColor(dc, UI_ACENTO2);
    HGDIOBJ o = SelectObject(dc, ui_fuente(UI_PEQUENO));
    SetBkMode(dc, TRANSPARENT);
    TextOutW(dc, ui_px(x), ui_px(y), t, (int)wcslen(t));
    SIZE s; GetTextExtentPoint32W(dc, t, (int)wcslen(t), &s);
    SelectObject(dc, o);
    HPEN p = CreatePen(PS_SOLID, 1, UI_BORDE);
    HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, ui_px(x) + s.cx + ui_px(10), ui_px(y) + s.cy / 2, NULL);
    LineTo(dc, ui_px(W_TOTAL - 32), ui_px(y) + s.cy / 2);
    SelectObject(dc, op);
    DeleteObject(p);
    (void)h;
}

static void etiqueta_fila(HWND h, HDC dc, const wchar_t *t, int y) { ui_texto(h, dc, t, LADO + 32, y + 8, 170, UI_NORMAL); }

static void estado_linea(HWND h, HDC dc, int y, int ok, const wchar_t *t, const wchar_t *det) {
    COLORREF c = ok ? RGB(70, 200, 120) : RGB(240, 180, 60);
    ui_rect_redondo(dc, ui_px(28), ui_px(y + 6), ui_px(10), ui_px(10), ui_px(5), c, (COLORREF)-1);
    ui_texto(h, dc, t, 46, y, LADO - 70, UI_FUERTE);
    if (det) ui_texto(h, dc, det, 46, y + 21, LADO - 70, UI_PEQUENO_TENUE);
}

static void pinta(HWND h, HDC dc, const RECT *rc) {
    ui_pinta_fondo(h, dc, rc);
    /* panel lateral */
    RECT lado = { 0, ui_px(56), ui_px(LADO), rc->bottom };
    ui_degradado(dc, &lado, RGB(20, 34, 62), RGB(18, 22, 31), 1);
    {   /* pelota decorativa, recortada al panel */
        HRGN clip = CreateRectRgn(lado.left, lado.top, lado.right, lado.bottom);
        SelectClipRgn(dc, clip);
        ui_pelota(dc, ui_px(LADO - 10), ui_px(H_TOTAL - 250), ui_px(130), 22);
        SelectClipRgn(dc, NULL);
        DeleteObject(clip);
    }
    ui_texto(h, dc, L"Listo para jugar", 28, 80, LADO - 56, UI_TITULO);
    ui_texto(h, dc, L"Port nativo de MLB 15 The Show (PS Vita), recompilado para tu PC a partir de tu copia del juego.", 28, 118, LADO - 56, UI_PEQUENO_TENUE);
    char carpeta[MAX_PATH] = "";
    GetPrivateProfileStringA("juego", "carpeta", "", carpeta, sizeof(carpeta), launcher_ini_path());
    wchar_t wc[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, carpeta, -1, wc, MAX_PATH);
    const wchar_t *base = wcsrchr(wc, L'\\'); base = base ? base + 1 : wc;
    wchar_t det[160];
    swprintf(det, 160, L"%ls", base[0] ? base : L"(sin elegir)");
    int y = 190;
    estado_linea(h, dc, y, carpeta[0] != 0, L"Tu copia del juego", det); y += 52;
    estado_linea(h, dc, y, 1, L"Código del juego preparado", L"juego.dll generado en este PC"); y += 52;
    int ntex = texturas_hd_cuenta();
    if (ntex > 0) swprintf(det, 160, L"%d texturas mejoradas", ntex);
    else wcscpy(det, L"Opcional: créalas con el botón de abajo");
    estado_linea(h, dc, y, ntex > 0, L"Texturas HD", det);
    /* derecha */
    seccion(h, dc, L"PANTALLA", 78);
    etiqueta_fila(h, dc, L"Resolución interna", 102);
    etiqueta_fila(h, dc, L"Modo", 146);
    etiqueta_fila(h, dc, L"Tamaño de ventana", 190);
    etiqueta_fila(h, dc, L"Límite de FPS", 234);
    seccion(h, dc, L"IMAGEN", 286);
    etiqueta_fila(h, dc, L"Brillo", 308);
    etiqueta_fila(h, dc, L"Nitidez", 344);
    wchar_t v[16];
    swprintf(v, 16, L"%d %%", pos(ID_BRILLO)); ui_texto(h, dc, v, W_TOTAL - 82, 316, 60, UI_NORMAL_TENUE);
    swprintf(v, 16, L"%d %%", pos(ID_NITIDEZ)); ui_texto(h, dc, v, W_TOTAL - 82, 352, 60, UI_NORMAL_TENUE);
    seccion(h, dc, L"CALIDAD", 392);
    ui_texto(h, dc, L"En el juego: F9 resolución · F10 FPS · F11 pantalla completa", LADO + 32, H_TOTAL - 72, 360, UI_PEQUENO_TENUE);
    ui_texto(h, dc, L"Al abrir: Ctrl = esta ventana · Shift = carpeta del juego", LADO + 32, H_TOTAL - 52, 360, UI_PEQUENO_TENUE);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_HSCROLL: {
            RECT r = { ui_px(W_TOTAL - 90), ui_px(300), ui_px(W_TOTAL), ui_px(380) };
            InvalidateRect(h, &r, FALSE);
            return 0;
        }
        case WM_COMMAND:
            if (ui_comando(h, w, l)) return 0;
            switch (LOWORD(w)) {
                case ID_JUGAR: case IDOK: guardar(); g_resultado = 1; DestroyWindow(h); return 0;
                case ID_SALIR: case IDCANCEL: g_resultado = 0; DestroyWindow(h); return 0;
                case ID_CONTROLES: controles_dialogo(h, NULL); return 0;
                case ID_TEXGEN: texturas_hd_dialogo(h); InvalidateRect(h, NULL, FALSE); return 0;
                case ID_DEFECTO: {
                    Opc o = { 3, 0, 0, 60, 85, 35, 1, 0, 1, 1, 1, 1 };
                    poner(&o);
                    return 0;
                }
            }
            break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC pdc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
            HDC dc = CreateCompatibleDC(pdc);
            HBITMAP bmp = CreateCompatibleBitmap(pdc, rc.right, rc.bottom);
            HGDIOBJ ob = SelectObject(dc, bmp);
            pinta(h, dc, &rc);
            BitBlt(pdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
            SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DRAWITEM: ui_pinta_boton((const DRAWITEMSTRUCT *)l); return TRUE;
        case WM_CLOSE: g_resultado = 0; DestroyWindow(h); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* MLB15_OPCIONES_CAPTURA=fichero.bmp (pruebas): la ventana se crea fuera de la pantalla, se
 * guarda su imagen y se cierra sin guardar nada. */
static void captura(HWND h, const char *path) {
    RECT r; GetWindowRect(h, &r);
    int w = r.right - r.left, hh = r.bottom - r.top;
    HDC sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -hh, 1, 32, BI_RGB } };
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    PrintWindow(h, mdc, 2 /* PW_RENDERFULLCONTENT */);
    FILE *f = fopen(path, "wb");
    if (f && bits) {
        BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(sizeof(fh) + sizeof(BITMAPINFOHEADER) + (DWORD)w * hh * 4), 0, 0,
                                sizeof(fh) + sizeof(BITMAPINFOHEADER) };
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
        fwrite(bits, 4, (size_t)w * hh, f);
    }
    if (f) fclose(f);
    SelectObject(mdc, old); DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(NULL, sdc);
}

int opciones_dialogo(void) {
    const char *cap = getenv("MLB15_OPCIONES_CAPTURA");
    int ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (!cap && !ctrl && !launcher_ini_int("video", "mostrar_opciones", 1)) return 1;

    /* El port es "per monitor v2" (el escalado de Windows recortaba la ventana del juego): ui_init
     * lo fija ya aquí, antes de crear ventanas. */
    ui_init();

    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = proc; wc.hInstance = hi; wc.lpszClassName = L"MLB15Opciones";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    RECT r = { 0, 0, ui_px(W_TOTAL), ui_px(H_TOTAL) };
    DWORD st = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    AdjustWindowRect(&r, st, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_dlg = CreateWindowExW(cap ? WS_EX_TOOLWINDOW : 0, L"MLB15Opciones", L"Project Baseball", st,
                            cap ? -8000 : wa.left + (wa.right - wa.left - ww) / 2, cap ? -8000 : wa.top + (wa.bottom - wa.top - wh) / 2,
                            ww, wh, NULL, NULL, hi, NULL);
    if (!g_dlg) return 1;
    ui_ventana_oscura(g_dlg);

    /* panel lateral */
    HWND jugar = ui_boton(g_dlg, ID_JUGAR, L"JUGAR", 28, H_TOTAL - 150, LADO - 56, 54, UIB_PRIMARIO);
    SendMessageW(jugar, WM_SETFONT, (WPARAM)ui_fuente(UI_SUBTITULO), TRUE);
    HWND b;
    b = ui_boton(g_dlg, ID_TEXGEN, L"Crear texturas HD…", 28, 360, LADO - 56, 38, UIB_NORMAL); ui_boton_fondo(b, RGB(19, 27, 44));
    b = ui_boton(g_dlg, ID_CONTROLES, L"Controles…", 28, H_TOTAL - 86, (LADO - 56 - 10) / 2, 38, UIB_NORMAL); ui_boton_fondo(b, RGB(18, 22, 31));
    b = ui_boton(g_dlg, ID_SALIR, L"Salir", 28 + (LADO - 56 - 10) / 2 + 10, H_TOTAL - 86, (LADO - 56 - 10) / 2, 38, UIB_NORMAL); ui_boton_fondo(b, RGB(18, 22, 31));

    /* derecha */
    static const wchar_t *const res[] = { L"x1", L"x2", L"x3", L"x4" };
    static const wchar_t *const modo[] = { L"Ventana", L"Pantalla completa" };
    static const wchar_t *const tam[] = { L"Auto", L"960×544", L"1920×1088", L"2880×1632" };
    static const wchar_t *const fps[] = { L"30 (ahorro)", L"60" };
    int xc = LADO + 32 + 180, wc2 = W_TOTAL - 32 - xc;
    ui_segmentos(g_dlg, ID_RES, res, 4, xc, 100, wc2, 36, 2);
    ui_segmentos(g_dlg, ID_MODO, modo, 2, xc, 144, wc2, 36, 0);
    ui_segmentos(g_dlg, ID_TAM, tam, 4, xc, 188, wc2, 36, 0);
    ui_segmentos(g_dlg, ID_FPS, fps, 2, xc, 232, wc2, 36, 1);
    ui_deslizador(g_dlg, ID_BRILLO, xc - 8, 310, wc2 - 50, 50, 100, 85);
    ui_deslizador(g_dlg, ID_NITIDEZ, xc - 8, 346, wc2 - 50, 0, 100, 35);
    int x1 = LADO + 32, wcol = (W_TOTAL - 64 - LADO - 24) / 2, x2 = x1 + wcol + 24;
    ui_interruptor(g_dlg, ID_SUAV, L"Suavizado de bordes (FXAA)", x1, 416, wcol, 1);
    ui_interruptor(g_dlg, ID_VERFPS, L"Mostrar los FPS", x2, 416, wcol, 0);
    ui_interruptor(g_dlg, ID_TEXHD, L"Usar texturas HD", x1, 452, wcol, 1);
    ui_interruptor(g_dlg, ID_JUGALTA, L"Jugadores en máxima calidad", x2, 452, wcol, 1);
    ui_interruptor(g_dlg, ID_SOMBRA, L"Sombras de los jugadores", x1, 488, wcol, 1);
    ui_interruptor(g_dlg, ID_MOSTRAR, L"Mostrar al arrancar", x2, 488, wcol, 1);
    ui_boton(g_dlg, ID_DEFECTO, L"Valores por defecto", W_TOTAL - 32 - 170, H_TOTAL - 72, 170, 36, UIB_NORMAL);

    Opc o = leer();
    poner(&o);
    if (cap) {
        ShowWindow(g_dlg, SW_SHOWNOACTIVATE);
        UpdateWindow(g_dlg);
        MSG mm;
        for (int i = 0; i < 50; i++) { while (PeekMessageW(&mm, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&mm); Sleep(10); }
        captura(g_dlg, cap);
        const char *cc = getenv("MLB15_CONTROLES_CAPTURA");
        if (cc) controles_dialogo(g_dlg, cc);
        const char *tc = getenv("MLB15_TEXTURAS_CAPTURA");
        if (tc) texturas_hd_dialogo(g_dlg);
        DestroyWindow(g_dlg);
        while (PeekMessageW(&mm, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&mm);
        return 1;
    }
    ShowWindow(g_dlg, SW_SHOW);
    SetForegroundWindow(g_dlg);
    SetFocus(jugar);

    g_resultado = 0;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return g_resultado;
}

/* Para otras ventanas del launcher (texturas_hd.c) en las capturas de prueba. */
void opciones_captura(HWND h, const char *path) { captura(h, path); }
