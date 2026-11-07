/* ui_estilo.c — aspecto común de las ventanas del launcher (ver ui_estilo.h).
 * Win32 + GDI para el texto (ClearType) y GDI+ (API plana, declarada aquí) para formas suavizadas. */
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <stdlib.h>
#include <math.h>
#include "ui_estilo.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

/* --- GDI+ plano (gdiplusflat.h es solo C++) --- */
typedef struct { UINT32 version; void *debug_cb; BOOL sin_hilo; BOOL sin_codecs; } GpEntrada;
int WINAPI GdiplusStartup(ULONG_PTR *tok, const GpEntrada *in, void *out);
int WINAPI GdipCreateFromHDC(HDC dc, void **g);
int WINAPI GdipDeleteGraphics(void *g);
int WINAPI GdipSetSmoothingMode(void *g, int modo);
int WINAPI GdipCreateSolidFill(DWORD argb, void **b);
int WINAPI GdipDeleteBrush(void *b);
int WINAPI GdipCreatePath(int fill, void **p);
int WINAPI GdipDeletePath(void *p);
int WINAPI GdipAddPathArcI(void *p, int x, int y, int w, int h, float a0, float barrido);
int WINAPI GdipClosePathFigure(void *p);
int WINAPI GdipFillPath(void *g, void *b, void *p);
int WINAPI GdipDrawPath(void *g, void *pen, void *p);
int WINAPI GdipCreatePen1(DWORD argb, float ancho, int unidad, void **pen);
int WINAPI GdipDeletePen(void *pen);
int WINAPI GdipFillEllipseI(void *g, void *b, int x, int y, int w, int h);
int WINAPI GdipDrawEllipseI(void *g, void *pen, int x, int y, int w, int h);
int WINAPI GdipDrawArcI(void *g, void *pen, int x, int y, int w, int h, float a0, float barrido);
int WINAPI GdipDrawLineI(void *g, void *pen, int x1, int y1, int x2, int y2);

static ULONG_PTR g_gp;
static double g_esc = 1.0;
static HFONT g_f[UI_NFUENTES];

static DWORD argb(COLORREF c, int a) { return ((DWORD)a << 24) | ((DWORD)GetRValue(c) << 16) | ((DWORD)GetGValue(c) << 8) | GetBValue(c); }

int ui_px(int v) { return (int)(v * g_esc + (v >= 0 ? 0.5 : -0.5)); }
int ui_lg(int p) { return (int)(p / g_esc + 0.5); }
HFONT ui_fuente(int e) { return g_f[e < 0 || e >= UI_NFUENTES ? UI_NORMAL : e]; }

void ui_init(void) {
    static int hecho;
    if (hecho) return;
    hecho = 1;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI * SetCtx)(HANDLE);
    typedef UINT(WINAPI * GetDpi)(void);
    SetCtx set = u32 ? (SetCtx)(void *)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : NULL;
    if (set) set((HANDLE)-4);   /* PER_MONITOR_AWARE_V2 (el juego también lo es) */
    GetDpi gd = u32 ? (GetDpi)(void *)GetProcAddress(u32, "GetDpiForSystem") : NULL;
    g_esc = gd ? gd() / 96.0 : 1.0;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
    GpEntrada in = { 1, NULL, FALSE, FALSE };
    GdiplusStartup(&g_gp, &in, NULL);
    static const struct { int tam, peso; const wchar_t *cara; } fu[UI_NFUENTES] = {
        [UI_TITULO] = { 26, FW_BOLD, L"Segoe UI Variable Display" },
        [UI_SUBTITULO] = { 18, FW_SEMIBOLD, L"Segoe UI Variable Display" },
        [UI_FUERTE] = { 15, FW_SEMIBOLD, L"Segoe UI Variable Text" },
        [UI_NORMAL] = { 15, FW_NORMAL, L"Segoe UI Variable Text" },
        [UI_NORMAL_TENUE] = { 15, FW_NORMAL, L"Segoe UI Variable Text" },
        [UI_PEQUENO] = { 13, FW_NORMAL, L"Segoe UI Variable Text" },
        [UI_PEQUENO_TENUE] = { 13, FW_NORMAL, L"Segoe UI Variable Text" },
        [UI_ENORME] = { 40, FW_BLACK, L"Segoe UI Variable Display" },
    };
    for (int i = 0; i < UI_NFUENTES; i++) {
        g_f[i] = CreateFontW(-ui_px(fu[i].tam), 0, 0, 0, fu[i].peso, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, fu[i].cara);
        /* Windows 10 sin «Segoe UI Variable»: CreateFont devuelve otra; se fuerza Segoe UI */
        wchar_t real[64] = L"";
        HDC dc = GetDC(NULL);
        HGDIOBJ o = SelectObject(dc, g_f[i]);
        GetTextFaceW(dc, 64, real);
        SelectObject(dc, o);
        ReleaseDC(NULL, dc);
        if (wcsncmp(real, L"Segoe UI Variable", 17)) {
            DeleteObject(g_f[i]);
            g_f[i] = CreateFontW(-ui_px(fu[i].tam), 0, 0, 0, fu[i].peso, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        }
    }
}

void ui_ventana_oscura(HWND h) {
    BOOL si = TRUE;
    DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &si, sizeof(si));
    COLORREF c = UI_FONDO;
    DwmSetWindowAttribute(h, 35 /* DWMWA_CAPTION_COLOR (Windows 11) */, &c, sizeof(c));
}

void ui_degradado(HDC dc, const RECT *rc, COLORREF a, COLORREF b, int vertical) {
    TRIVERTEX v[2] = {
        { rc->left, rc->top, (COLOR16)(GetRValue(a) << 8), (COLOR16)(GetGValue(a) << 8), (COLOR16)(GetBValue(a) << 8), 0 },
        { rc->right, rc->bottom, (COLOR16)(GetRValue(b) << 8), (COLOR16)(GetGValue(b) << 8), (COLOR16)(GetBValue(b) << 8), 0 },
    };
    GRADIENT_RECT r = { 0, 1 };
    GradientFill(dc, v, 2, &r, 1, vertical ? GRADIENT_FILL_RECT_V : GRADIENT_FILL_RECT_H);
}

static void *gp_de(HDC dc) {
    void *g = NULL;
    GdipCreateFromHDC(dc, &g);
    if (g) GdipSetSmoothingMode(g, 4 /* AntiAlias */);
    return g;
}

static void *camino_redondo(int x, int y, int w, int h, int r) {
    void *p = NULL;
    GdipCreatePath(0, &p);
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    int d = r * 2;
    if (d <= 0) d = 1;
    GdipAddPathArcI(p, x, y, d, d, 180, 90);
    GdipAddPathArcI(p, x + w - d - 1, y, d, d, 270, 90);
    GdipAddPathArcI(p, x + w - d - 1, y + h - d - 1, d, d, 0, 90);
    GdipAddPathArcI(p, x, y + h - d - 1, d, d, 90, 90);
    GdipClosePathFigure(p);
    return p;
}

void ui_rect_redondo(HDC dc, int x, int y, int w, int h, int r, COLORREF relleno, COLORREF borde) {
    void *g = gp_de(dc);
    if (!g) return;
    void *p = camino_redondo(x, y, w, h, r);
    void *b = NULL;
    GdipCreateSolidFill(argb(relleno, 255), &b);
    GdipFillPath(g, b, p);
    if (borde != (COLORREF)-1) {
        void *pen = NULL;
        GdipCreatePen1(argb(borde, 255), (float)ui_px(1), 2, &pen);
        GdipDrawPath(g, pen, p);
        GdipDeletePen(pen);
    }
    GdipDeleteBrush(b);
    GdipDeletePath(p);
    GdipDeleteGraphics(g);
}

/* Pelota de béisbol: círculo claro con las dos costuras rojas (arcos y puntadas). */
void ui_pelota(HDC dc, int cx, int cy, int r, int alfa) {
    void *g = gp_de(dc);
    if (!g) return;
    void *b = NULL, *pen = NULL, *pc = NULL;
    GdipCreateSolidFill(argb(RGB(244, 241, 232), alfa), &b);
    GdipFillEllipseI(g, b, cx - r, cy - r, 2 * r, 2 * r);
    GdipCreatePen1(argb(UI_ROJO, alfa), (float)(r / 22 + 1), 2, &pen);
    GdipCreatePen1(argb(UI_ROJO, alfa), (float)(r / 40 + 1), 2, &pc);
    /* costuras: dos arcos de un círculo mayor que asoman por los lados */
    int R = (int)(r * 1.25), off = (int)(r * 1.62);
    GdipDrawArcI(g, pen, cx - off - R, cy - R, 2 * R, 2 * R, -38, 76);
    GdipDrawArcI(g, pen, cx + off - R, cy - R, 2 * R, 2 * R, 142, 76);
    /* puntadas */
    for (int lado = -1; lado <= 1; lado += 2) {
        int ccx = cx + lado * off;
        for (int k = -4; k <= 4; k++) {
            double a = (lado < 0 ? 0.0 : 3.14159265) + k * 0.13;
            double px0 = ccx + R * cos(a), py0 = cy + R * sin(a);
            double nx = cos(a), ny = sin(a), tx = -ny, ty = nx;
            int l = r / 9 + 2;
            GdipDrawLineI(g, pc, (int)(px0 - nx * l * 0.2 - tx * l * 0.5), (int)(py0 - ny * l * 0.2 - ty * l * 0.5),
                          (int)(px0 - nx * l * 0.9 + tx * l * 0.5), (int)(py0 - ny * l * 0.9 + ty * l * 0.5));
        }
    }
    GdipDeletePen(pc); GdipDeletePen(pen); GdipDeleteBrush(b);
    GdipDeleteGraphics(g);
}

void ui_pinta_fondo(HWND h, HDC dc, const RECT *rc) {
    (void)h;
    HBRUSH fb = CreateSolidBrush(UI_FONDO);
    FillRect(dc, rc, fb);
    DeleteObject(fb);
    /* franja superior: degradado azul marino con el título del port */
    RECT top = { rc->left, rc->top, rc->right, rc->top + ui_px(56) };
    ui_degradado(dc, &top, RGB(14, 40, 86), RGB(16, 19, 26), 0);
    RECT linea = { rc->left, top.bottom - ui_px(2), rc->left + (rc->right - rc->left) / 3, top.bottom };
    ui_degradado(dc, &linea, UI_ROJO, RGB(16, 19, 26), 0);
    ui_pelota(dc, rc->left + ui_px(30), rc->top + ui_px(28), ui_px(14), 255);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ o = SelectObject(dc, g_f[UI_SUBTITULO]);
    SetTextColor(dc, UI_TEXTO);
    TextOutW(dc, rc->left + ui_px(54), rc->top + ui_px(15), L"Project Baseball", 16);
    SIZE s; GetTextExtentPoint32W(dc, L"Project Baseball", 16, &s);
    SelectObject(dc, g_f[UI_PEQUENO]);
    SetTextColor(dc, UI_TENUE);
    TextOutW(dc, rc->left + ui_px(54) + s.cx + ui_px(10), rc->top + ui_px(20), L"port nativo de MLB 15 The Show", 30);
    SelectObject(dc, o);
}

int ui_texto(HWND h, HDC dc, const wchar_t *t, int x, int y, int w, int e) {
    (void)h;
    SetBkMode(dc, TRANSPARENT);
    COLORREF c = (e == UI_NORMAL_TENUE || e == UI_PEQUENO_TENUE) ? UI_TENUE : UI_TEXTO;
    SetTextColor(dc, c);
    HGDIOBJ o = SelectObject(dc, ui_fuente(e));
    RECT r = { ui_px(x), ui_px(y), ui_px(x + w), ui_px(y) + 4000 };
    DrawTextW(dc, t, -1, &r, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    DrawTextW(dc, t, -1, &r, DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, o);
    return ui_lg(r.bottom - r.top);
}

static COLORREF fondo_de(HWND c) {
    HANDLE p = GetPropW(c, L"ui_bg");
    return p ? (COLORREF)((UINT_PTR)p - 1) : UI_FONDO;
}

LRESULT ui_color_control(HWND h, HDC dc) {
    static HBRUSH b;
    if (!b) b = CreateSolidBrush(UI_FONDO);
    (void)h;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, UI_TEXTO);
    SetBkColor(dc, UI_FONDO);
    return (LRESULT)b;
}

void ui_estilo_barra(HWND barra) {
    SetWindowTheme(barra, L"", L"");
    SendMessageW(barra, PBM_SETBARCOLOR, 0, (LPARAM)UI_ACENTO2);
    SendMessageW(barra, PBM_SETBKCOLOR, 0, (LPARAM)UI_PANEL2);
}

/* --- botones propios --- */
#define TIPO(b) ((int)(GetWindowLongPtrW(b, GWLP_USERDATA) & 0xFF))
#define ACTIVO(b) ((int)((GetWindowLongPtrW(b, GWLP_USERDATA) >> 8) & 1))
static void pon_activo(HWND b, int on) {
    LONG_PTR v = GetWindowLongPtrW(b, GWLP_USERDATA);
    SetWindowLongPtrW(b, GWLP_USERDATA, (v & ~(LONG_PTR)0x100) | (on ? 0x100 : 0));
    InvalidateRect(b, NULL, FALSE);
}

static LRESULT CALLBACK sub_boton(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    (void)id; (void)ref;
    switch (m) {
    case WM_MOUSEMOVE:
        if (!GetPropW(h, L"ui_hover")) {
            SetPropW(h, L"ui_hover", (HANDLE)1);
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            InvalidateRect(h, NULL, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(h, L"ui_hover");
        InvalidateRect(h, NULL, FALSE);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        RemovePropW(h, L"ui_hover");
        RemovePropW(h, L"ui_bg");
        RemovePropW(h, L"ui_grupo");
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

HWND ui_boton(HWND padre, int id, const wchar_t *t, int x, int y, int w, int h, int tipo) {
    HWND b = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                             ui_px(x), ui_px(y), ui_px(w), ui_px(h), padre, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SetWindowLongPtrW(b, GWLP_USERDATA, tipo);
    SendMessageW(b, WM_SETFONT, (WPARAM)g_f[tipo == UIB_PRIMARIO ? UI_FUERTE : UI_NORMAL], TRUE);
    SetWindowSubclass(b, sub_boton, 1, 0);
    return b;
}

HWND ui_interruptor(HWND padre, int id, const wchar_t *t, int x, int y, int w, int on) {
    HWND b = ui_boton(padre, id, t, x, y, w, 30, UIB_INTERRUPTOR);
    pon_activo(b, on);
    return b;
}
int ui_interruptor_on(HWND b) { return ACTIVO(b); }
void ui_interruptor_pon(HWND b, int on) { pon_activo(b, on); }

void ui_segmentos(HWND padre, int id, const wchar_t *const *t, int n, int x, int y, int w, int h, int sel) {
    int cw = w / n;
    for (int i = 0; i < n; i++) {
        HWND b = ui_boton(padre, id + i, t[i], x + i * cw, y, i == n - 1 ? w - cw * (n - 1) : cw, h, UIB_SEGMENTO);
        SetPropW(b, L"ui_grupo", (HANDLE)(INT_PTR)(((i == 0) ? 1 : 0) | ((i == n - 1) ? 2 : 0) | 4));
        pon_activo(b, i == sel);
    }
}
void ui_segmento_elige(HWND padre, int id, int n, int sel) {
    for (int i = 0; i < n; i++) pon_activo(GetDlgItem(padre, id + i), i == sel);
}
int ui_segmento_actual(HWND padre, int id, int n) {
    for (int i = 0; i < n; i++) if (ACTIVO(GetDlgItem(padre, id + i))) return i;
    return 0;
}

static COLORREF mezcla(COLORREF a, COLORREF b, int t) {   /* t de 0 a 255 */
    return RGB((GetRValue(a) * (255 - t) + GetRValue(b) * t) / 255, (GetGValue(a) * (255 - t) + GetGValue(b) * t) / 255,
               (GetBValue(a) * (255 - t) + GetBValue(b) * t) / 255);
}

void ui_pinta_boton(const DRAWITEMSTRUCT *d) {
    HWND b = d->hwndItem;
    int tipo = TIPO(b), on = ACTIVO(b), hover = GetPropW(b, L"ui_hover") != NULL;
    int pulsado = (d->itemState & ODS_SELECTED) != 0, foco = (d->itemState & ODS_FOCUS) != 0;
    int desact = (d->itemState & ODS_DISABLED) != 0;
    RECT rc = d->rcItem;
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    /* doble búfer */
    HDC dc = CreateCompatibleDC(d->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(d->hDC, W, H);
    HGDIOBJ ob = SelectObject(dc, bmp);
    COLORREF bg = fondo_de(b);
    HBRUSH fb = CreateSolidBrush(bg);
    RECT r0 = { 0, 0, W, H };
    FillRect(dc, &r0, fb);
    DeleteObject(fb);
    wchar_t t[128];
    GetWindowTextW(b, t, 128);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, (HFONT)SendMessageW(b, WM_GETFONT, 0, 0));
    int rad = ui_px(8);
    if (tipo == UIB_INTERRUPTOR) {
        int tw = ui_px(42), th = ui_px(22), tx = W - tw - ui_px(2), ty = (H - th) / 2;
        COLORREF pista = on ? (hover ? UI_ACENTO2 : UI_ACENTO) : (hover ? RGB(70, 80, 100) : RGB(58, 66, 84));
        ui_rect_redondo(dc, tx, ty, tw, th, th / 2, pista, (COLORREF)-1);
        int kd = th - ui_px(6), kx = on ? tx + tw - kd - ui_px(3) : tx + ui_px(3);
        ui_rect_redondo(dc, kx, ty + ui_px(3), kd, kd, kd / 2, RGB(250, 250, 252), (COLORREF)-1);
        RECT rt = { 0, 0, tx - ui_px(10), H };
        SetTextColor(dc, desact ? UI_TENUE : UI_TEXTO);
        DrawTextW(dc, t, -1, &rt, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (foco) { RECT f = { tx - ui_px(3), ty - ui_px(3), tx + tw + ui_px(1), ty + th + ui_px(3) }; (void)f; }
    } else if (tipo == UIB_SEGMENTO) {
        int g = (int)(INT_PTR)GetPropW(b, L"ui_grupo");
        COLORREF c = on ? UI_ACENTO : (pulsado ? UI_BORDE : hover ? UI_PANEL2 : UI_PANEL);
        /* cada segmento se dibuja como píldora recortada: los extremos redondeados solo por fuera */
        int x0 = (g & 1) ? 0 : -rad, x1 = (g & 2) ? W : W + rad;
        HRGN clip = CreateRectRgn(0, 0, W, H);
        SelectClipRgn(dc, clip);
        ui_rect_redondo(dc, x0, 0, x1 - x0, H, rad, c, on ? c : UI_BORDE);
        SelectClipRgn(dc, NULL);
        DeleteObject(clip);
        if (!(g & 1)) {   /* separador fino */
            HPEN p = CreatePen(PS_SOLID, 1, UI_BORDE); HGDIOBJ op = SelectObject(dc, p);
            MoveToEx(dc, 0, ui_px(6), NULL); LineTo(dc, 0, H - ui_px(6));
            SelectObject(dc, op); DeleteObject(p);
        }
        SetTextColor(dc, on ? RGB(255, 255, 255) : UI_TEXTO);
        RECT rt = { ui_px(4), 0, W - ui_px(4), H };
        DrawTextW(dc, t, -1, &rt, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else {
        COLORREF c, borde, txt = UI_TEXTO;
        if (tipo == UIB_PRIMARIO) { c = pulsado ? RGB(20, 96, 190) : hover ? UI_ACENTO2 : UI_ACENTO; borde = c; txt = RGB(255, 255, 255); }
        else if (tipo == UIB_PELIGRO) { c = pulsado ? RGB(150, 30, 40) : hover ? UI_ROJO : UI_PANEL2; borde = hover ? UI_ROJO : UI_BORDE; }
        else { c = pulsado ? UI_BORDE : hover ? mezcla(UI_PANEL2, UI_BORDE, 120) : UI_PANEL2; borde = UI_BORDE; }
        if (desact) { c = UI_PANEL; txt = UI_TENUE; borde = UI_PANEL2; }
        ui_rect_redondo(dc, 0, 0, W, H, rad, c, borde);
        if (foco && tipo != UIB_PRIMARIO) ui_rect_redondo(dc, ui_px(2), ui_px(2), W - ui_px(4), H - ui_px(4), rad, c, UI_ACENTO2);
        SetTextColor(dc, txt);
        RECT rt = { ui_px(6), 0, W - ui_px(6), H };
        DrawTextW(dc, t, -1, &rt, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, of);
    BitBlt(d->hDC, rc.left, rc.top, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
}

/* --- deslizador propio --- */
typedef struct { int mn, mx, v, arrastra; } Desl;

static void desl_desde_x(HWND h, Desl *s, int x) {
    RECT rc; GetClientRect(h, &rc);
    int m = ui_px(9), w = rc.right - 2 * m;
    int v = s->mn + (int)((double)(x - m) / (w > 0 ? w : 1) * (s->mx - s->mn) + 0.5);
    if (v < s->mn) v = s->mn;
    if (v > s->mx) v = s->mx;
    if (v != s->v) {
        s->v = v;
        InvalidateRect(h, NULL, FALSE);
        SendMessageW(GetParent(h), WM_HSCROLL, SB_THUMBTRACK, (LPARAM)h);
    }
}

static LRESULT CALLBACK desl_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Desl *s = (Desl *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_NCCREATE: {
        Desl *n = (Desl *)calloc(1, sizeof(Desl));
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)n);
        break;
    }
    case WM_NCDESTROY: free(s); break;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: SetFocus(h); SetCapture(h); s->arrastra = 1; desl_desde_x(h, s, GET_X_LPARAM(l)); return 0;
    case WM_MOUSEMOVE: if (s->arrastra) desl_desde_x(h, s, GET_X_LPARAM(l)); return 0;
    case WM_LBUTTONUP: if (s->arrastra) { s->arrastra = 0; ReleaseCapture(); } return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_KEYDOWN:
        if (w == VK_LEFT || w == VK_RIGHT) {
            int v = s->v + (w == VK_RIGHT ? 1 : -1);
            if (v >= s->mn && v <= s->mx) { s->v = v; InvalidateRect(h, NULL, FALSE); SendMessageW(GetParent(h), WM_HSCROLL, SB_THUMBTRACK, (LPARAM)h); }
            return 0;
        }
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(h, NULL, FALSE); break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC pdc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC dc = CreateCompatibleDC(pdc);
        HBITMAP bmp = CreateCompatibleBitmap(pdc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(dc, bmp);
        HBRUSH fb = CreateSolidBrush(UI_FONDO);
        FillRect(dc, &rc, fb);
        DeleteObject(fb);
        int m2 = ui_px(9), w2 = rc.right - 2 * m2, cy = rc.bottom / 2, th = ui_px(6);
        int kx = m2 + (int)((double)(s->v - s->mn) / (s->mx > s->mn ? s->mx - s->mn : 1) * w2);
        ui_rect_redondo(dc, m2, cy - th / 2, w2, th, th / 2, UI_PANEL2, (COLORREF)-1);
        ui_rect_redondo(dc, m2, cy - th / 2, kx - m2 + th / 2, th, th / 2, UI_ACENTO, (COLORREF)-1);
        int kd = ui_px(18);
        ui_rect_redondo(dc, kx - kd / 2, cy - kd / 2, kd, kd, kd / 2, RGB(250, 250, 252), GetFocus() == h ? UI_ACENTO2 : (COLORREF)-1);
        BitBlt(pdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

HWND ui_deslizador(HWND padre, int id, int x, int y, int w, int mn, int mx, int v) {
    static int reg;
    if (!reg) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = desl_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"MLB15Deslizador";
        wc.hCursor = LoadCursor(NULL, IDC_HAND);
        RegisterClassW(&wc);
        reg = 1;
    }
    HWND d = CreateWindowExW(0, L"MLB15Deslizador", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, ui_px(x), ui_px(y), ui_px(w), ui_px(28),
                             padre, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    Desl *s = (Desl *)GetWindowLongPtrW(d, GWLP_USERDATA);
    s->mn = mn; s->mx = mx; s->v = v;
    return d;
}
int ui_deslizador_pos(HWND d) { Desl *s = (Desl *)GetWindowLongPtrW(d, GWLP_USERDATA); return s ? s->v : 0; }
void ui_deslizador_pon(HWND d, int v) {
    Desl *s = (Desl *)GetWindowLongPtrW(d, GWLP_USERDATA);
    if (!s) return;
    s->v = v < s->mn ? s->mn : v > s->mx ? s->mx : v;
    InvalidateRect(d, NULL, FALSE);
}

/* Para el WM_COMMAND de la ventana: interruptores y segmentos cambian solos. Devuelve 1 si era uno. */
int ui_comando(HWND padre, WPARAM w, LPARAM l) {
    HWND b = (HWND)l;
    if (!b || HIWORD(w) != BN_CLICKED) return 0;
    int tipo = TIPO(b);
    if (tipo == UIB_INTERRUPTOR) { pon_activo(b, !ACTIVO(b)); return 1; }
    if (tipo == UIB_SEGMENTO) {
        int id = LOWORD(w);
        /* el grupo son los segmentos contiguos: hacia atrás hasta el primero y hacia delante hasta el último */
        int a = id, z = id;
        while (!((int)(INT_PTR)GetPropW(GetDlgItem(padre, a), L"ui_grupo") & 1)) a--;
        while (!((int)(INT_PTR)GetPropW(GetDlgItem(padre, z), L"ui_grupo") & 2)) z++;
        for (int i = a; i <= z; i++) pon_activo(GetDlgItem(padre, i), i == id);
        return 1;
    }
    return 0;
}

/* Fondo propio para un botón que va sobre otro color (p. ej. el panel lateral). */
void ui_boton_fondo(HWND b, COLORREF c) { SetPropW(b, L"ui_bg", (HANDLE)(UINT_PTR)(c + 1)); }
