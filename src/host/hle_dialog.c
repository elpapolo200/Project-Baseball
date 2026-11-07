/* SceCommonDialog: diálogos del sistema (mensajes, guardado, teclado, red, PSN,
 * trofeos).
 *
 * En el port no hay interfaz de sistema: cada diálogo termina en el acto con
 * una respuesta razonable, para que el juego siga su flujo. Mensajes y guardado
 * contestan «Sí/OK» y red/PSN «cancelado por el usuario» (el port va sin conexión).
 * El teclado (ImeDialog) abre una ventanita de Windows para escribir el texto (nombre
 * del jugador creado...); sin ventana (MLB15_NOWINDOW) devuelve el texto inicial. Patrón del SDK: Init → GetStatus
 * hasta FINISHED → GetResult → Term.
 *
 * Estructuras de resultado (solo se escriben los campos de cabeza):
 *   MsgDialog/SaveDataDialog: { int mode; int result; int buttonId; ... }
 *   ImeDialog:                { int result; int button; ... }
 *   resto:                    { int result; ... }
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"

#define ST_NONE     0
#define ST_RUNNING  1
#define ST_FINISHED 2

#define RES_OK            0
#define RES_USER_CANCELED 1

#define BUTTON_YES   1     /* SCE_MSG_DIALOG_BUTTON_ID_YES / OK */
#define IME_ENTER    2     /* SCE_IME_DIALOG_BUTTON_ENTER       */

#define ERR_NOT_RUNNING 0x80020401u   /* SCE_COMMON_DIALOG_ERROR_NOT_RUNNING */

enum { D_MSG, D_SAVE, D_IME, D_NETCHECK, D_FRIENDS, D_NPMSG, D_PROFILE, D_TROPHY, D_COUNT };
static const char *const k_name[D_COUNT] = {
    "MsgDialog", "SaveDataDialog", "ImeDialog", "NetCheckDialog",
    "NpFriendListDialog", "NpMessageDialog", "NpProfileDialog", "NpTrophySetupDialog"
};
static volatile int g_status[D_COUNT];

/* --- teclado (SceImeDialogParam: título +0x28, longitud máx. +0x2C, texto inicial +0x30,
 *     búfer de salida +0x34; SceWChar16 = UTF-16 como WCHAR de Windows) --- */
#define IME_CLOSE 1
static WCHAR g_ime_title[128], g_ime_text[512];
static uint32_t g_ime_buf, g_ime_max;
static volatile int g_ime_button = IME_ENTER;
static HWND g_ime_edit;

static void gstr16(uint32_t a, WCHAR *out, size_t max) {
    size_t i = 0;
    for (; a && i + 1 < max; i++) { uint16_t c = vita_read16(a + 2 * (uint32_t)i); if (!c) break; out[i] = c; }
    out[i] = 0;
}

static LRESULT CALLBACK ime_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_COMMAND:
            if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
                if (LOWORD(w) == IDOK) GetWindowTextW(g_ime_edit, g_ime_text, (int)(sizeof(g_ime_text) / sizeof(WCHAR)));
                g_ime_button = LOWORD(w) == IDOK ? IME_ENTER : IME_CLOSE;
                DestroyWindow(h);
            }
            return 0;
        case WM_CLOSE: g_ime_button = IME_CLOSE; DestroyWindow(h); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* Subclase del cuadro de texto: Intro = Aceptar, Esc = Cancelar. */
static WNDPROC g_edit_old;
static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE)) {
        SendMessageW(GetParent(h), WM_COMMAND, w == VK_RETURN ? IDOK : IDCANCEL, 0);
        return 0;
    }
    if (m == WM_CHAR && (w == '\r' || w == 27)) return 0;
    return CallWindowProcW(g_edit_old, h, m, w, l);
}

static DWORD WINAPI ime_thread(LPVOID arg) {
    (void)arg;
    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = ime_proc; wc.hInstance = hi; wc.lpszClassName = L"MLB15Ime";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN), ww = 460, wh = 150;
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, L"MLB15Ime", g_ime_title[0] ? g_ime_title : L"Project Baseball",
                             WS_CAPTION | WS_SYSMENU | WS_VISIBLE, (sx - ww) / 2, (sy - wh) / 2, ww, wh, NULL, NULL, hi, NULL);
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_ime_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_ime_text, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 14, 16, 416, 26, h,
                                 NULL, hi, NULL);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"Aceptar", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 230, 60, 96, 30, h, (HMENU)IDOK, hi, NULL);
    HWND ca = CreateWindowExW(0, L"BUTTON", L"Cancelar", WS_CHILD | WS_VISIBLE, 334, 60, 96, 30, h, (HMENU)IDCANCEL, hi, NULL);
    SendMessageW(g_ime_edit, WM_SETFONT, (WPARAM)f, TRUE); SendMessageW(ok, WM_SETFONT, (WPARAM)f, TRUE); SendMessageW(ca, WM_SETFONT, (WPARAM)f, TRUE);
    if (g_ime_max) SendMessageW(g_ime_edit, EM_SETLIMITTEXT, g_ime_max, 0);
    g_edit_old = (WNDPROC)SetWindowLongPtrW(g_ime_edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
    SendMessageW(g_ime_edit, EM_SETSEL, 0, -1);
    printf("[dialog] ventana del teclado %p\n", (void *)h);
    fflush(stdout);
    SetForegroundWindow(h);
    SetFocus(g_ime_edit);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    g_status[D_IME] = ST_FINISHED;
    return 0;
}

static void ime_init(uint32_t param) {
    g_ime_title[0] = g_ime_text[0] = 0;
    g_ime_buf = vita_read32(param + 0x34);
    g_ime_max = vita_read32(param + 0x2C);
    if (g_ime_max > 500) g_ime_max = 500;
    gstr16(vita_read32(param + 0x28), g_ime_title, sizeof(g_ime_title) / sizeof(WCHAR));
    gstr16(vita_read32(param + 0x30), g_ime_text, sizeof(g_ime_text) / sizeof(WCHAR));
    g_ime_button = IME_ENTER;
    printf("[dialog] teclado: búfer 0x%08X máx %u\n", g_ime_buf, g_ime_max);
    fflush(stdout);
    const char *nw = getenv("MLB15_NOWINDOW");
    if (nw && *nw == '1') { g_status[D_IME] = ST_FINISHED; return; }   /* sin ventana: texto inicial */
    g_status[D_IME] = ST_RUNNING;
    CloseHandle(CreateThread(NULL, 0, ime_thread, NULL, 0, NULL));
}

/* Copia el texto (o el inicial si se canceló) al búfer del juego. */
static void ime_store(void) {
    if (!g_ime_buf) return;
    uint32_t n = (uint32_t)wcslen(g_ime_text);
    if (g_ime_max && n > g_ime_max) n = g_ime_max;
    for (uint32_t i = 0; i < n; i++) vita_write16(g_ime_buf + 2 * i, g_ime_text[i]);
    vita_write16(g_ime_buf + 2 * n, 0);
}

static void dlg_init(int d) {
    if (getenv("MLB15_DIALOG_TRACE")) printf("[dialog] %s: Init -> terminado en el acto\n", k_name[d]);
    g_status[d] = ST_FINISHED;
    r0 = 0;
}

static void dlg_result(int d, uint32_t res) {
    if (g_status[d] == ST_NONE) { r0 = ERR_NOT_RUNNING; return; }
    if (res) {
        switch (d) {
            case D_MSG: case D_SAVE:
                vita_write32(res + 4, RES_OK);
                vita_write32(res + 8, BUTTON_YES);
                break;
            case D_IME:
                if (g_ime_button == IME_ENTER) ime_store();
                vita_write32(res + 0, RES_OK);
                vita_write32(res + 4, (uint32_t)g_ime_button);
                break;
            case D_TROPHY:
                vita_write32(res + 0, RES_OK);
                break;
            default:                                   /* red y PSN: sin conexión */
                vita_write32(res + 0, RES_USER_CANCELED);
                break;
        }
    }
    r0 = 0;
}

#define DIALOG(prefix, d)                                                          \
    void vita_hle_##prefix##Init(void)      { if (d == D_IME) { ime_init(r0); r0 = 0; } else dlg_init(d); } \
    void vita_hle_##prefix##GetStatus(void) { r0 = (uint32_t)g_status[d]; }        \
    void vita_hle_##prefix##GetResult(void) { dlg_result(d, r0); }                 \
    void vita_hle_##prefix##Term(void)      { g_status[d] = ST_NONE; r0 = 0; }

DIALOG(sceMsgDialog, D_MSG)
/* sceSaveDataDialog: lista de partidas de verdad (hle_savedialog.c) */
void savedialog_init(uint32_t param);
uint32_t savedialog_status(void);
void savedialog_result(uint32_t res);
void savedialog_term(void);
void savedialog_finish(void);
void vita_hle_sceSaveDataDialogInit(void)      { savedialog_init(r0); r0 = 0; }
void vita_hle_sceSaveDataDialogGetStatus(void) { r0 = savedialog_status(); }
void vita_hle_sceSaveDataDialogGetResult(void) { savedialog_result(r0); r0 = 0; }
void vita_hle_sceSaveDataDialogTerm(void)      { savedialog_term(); r0 = 0; }
DIALOG(sceImeDialog, D_IME)
DIALOG(sceNetCheckDialog, D_NETCHECK)
DIALOG(sceNpFriendListDialog, D_FRIENDS)
DIALOG(sceNpMessageDialog, D_NPMSG)
DIALOG(sceNpProfileDialog, D_PROFILE)
DIALOG(sceNpTrophySetupDialog, D_TROPHY)

void vita_hle_sceMsgDialogAbort(void)       { g_status[D_MSG] = ST_FINISHED; r0 = 0; }
void vita_hle_sceImeDialogAbort(void)       { g_status[D_IME] = ST_FINISHED; r0 = 0; }
void vita_hle_sceNpMessageDialogAbort(void) { g_status[D_NPMSG] = ST_FINISHED; r0 = 0; }
void vita_hle_sceSaveDataDialogAbort(void)  { savedialog_finish(); r0 = 0; }

/* Guardado en varios pasos: no hay subpantallas, todo continúa al momento. */
void vita_hle_sceSaveDataDialogContinue(void)     { savedialog_init(r0); r0 = 0; }   /* mismo parámetro que Init */
void vita_hle_sceSaveDataDialogFinish(void)       { savedialog_finish(); r0 = 0; }
void vita_hle_sceSaveDataDialogSubClose(void)     { r0 = 0; }
void vita_hle_sceSaveDataDialogGetSubStatus(void) { r0 = ST_NONE; }

/* sceCommonDialogUpdate(const SceCommonDialogUpdateParam *): no hay nada que dibujar. */
void vita_hle_sceCommonDialogUpdate(void) { r0 = 0; }
