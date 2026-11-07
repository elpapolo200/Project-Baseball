/* Entrada del PC -> pad de la Vita (teclado y mando por SDL).
 *
 * gpu_vk.c entrega aquí los eventos SDL en el mismo hilo que creó la ventana
 * (input_event) y llama a input_poll una vez por fotograma; hle_ctrl.c lee el
 * resultado desde cualquier hilo con input_buttons / input_sticks.
 *
 * Teclado (valores por defecto, sobrescribibles en MLB15.ini [teclas]):
 *   flechas = cruceta      X = Cruz      C = Círculo   Z = Cuadrado  V = Triángulo
 *   Q = L        E = R     Enter = Start  Shift dcho. = Select
 *   WASD = stick izquierdo  IJKL = stick derecho        Espacio = Cruz también
 * Mando (SDL_GameController, distribución Xbox):
 *   A = Cruz  B = Círculo  X = Cuadrado  Y = Triángulo  LB/LT = L  RB/RT = R
 *   Start = Start  Back = Select  cruceta y sticks tal cual
 *
 * MLB15_PAD_GUION="12:cruz,15.5:abajo,17:cruz" pulsa botones solo, a esos
 * segundos desde la primera lectura del pad (para probar sin ventana).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "input.h"
#include "launcher.h"

static volatile uint32_t g_keys;       /* botones del teclado */
static volatile uint32_t g_pad;        /* botones del mando */
static volatile uint32_t g_sticks = 0x80808080u;   /* lx | ly<<8 | rx<<16 | ry<<24 */
static SDL_GameController *g_ctl;

/* Mapeo configurable: scancode SDL -> botón Vita. Inicializado con valores por defecto. */
static uint32_t g_key_map[SDL_NUM_SCANCODES];

/* Nombres de scancodes SDL para el ini (subconjunto usado). */
static const struct { const char *name; SDL_Scancode code; } g_scancode_names[] = {
    { "arriba",    SDL_SCANCODE_UP },        { "abajo",    SDL_SCANCODE_DOWN },
    { "izquierda", SDL_SCANCODE_LEFT },      { "derecha",  SDL_SCANCODE_RIGHT },
    { "x",         SDL_SCANCODE_X },         { "espacio",  SDL_SCANCODE_SPACE },
    { "c",         SDL_SCANCODE_C },         { "z",        SDL_SCANCODE_Z },
    { "v",         SDL_SCANCODE_V },         { "q",        SDL_SCANCODE_Q },
    { "e",         SDL_SCANCODE_E },         { "enter",    SDL_SCANCODE_RETURN },
    { "shift_dcho",SDL_SCANCODE_RSHIFT },    { "w",        SDL_SCANCODE_W },
    { "s",         SDL_SCANCODE_S },         { "a",        SDL_SCANCODE_A },
    { "d",         SDL_SCANCODE_D },         { "i",        SDL_SCANCODE_I },
    { "k",         SDL_SCANCODE_K },         { "j",        SDL_SCANCODE_J },
    { "l",         SDL_SCANCODE_L },
};

/* Nombres de botones Vita para el ini. */
static const struct { const char *name; uint32_t btn; } g_vita_btn_names[] = {
    { "cruz",       VITA_CROSS },     { "circulo",   VITA_CIRCLE },
    { "cuadrado",   VITA_SQUARE },    { "triangulo", VITA_TRIANGLE },
    { "arriba",     VITA_UP },        { "abajo",     VITA_DOWN },
    { "izquierda",  VITA_LEFT },      { "derecha",   VITA_RIGHT },
    { "l",          VITA_L },         { "r",         VITA_R },
    { "start",      VITA_START },     { "select",    VITA_SELECT },
};

/* Stick bits para el ini (teclas WASD/IJKL). */
static const struct { const char *name; uint32_t bit; } g_stick_names[] = {
    { "stick_izq_arriba",    0 }, { "stick_izq_abajo",  1 },
    { "stick_izq_izquierda", 2 }, { "stick_izq_derecha", 3 },
    { "stick_der_arriba",    4 }, { "stick_der_abajo",  5 },
    { "stick_der_izquierda", 6 }, { "stick_der_derecha", 7 },
};

static uint32_t g_stick_map[8];   /* scancode -> bit de stick (0-7), 0 = no asignado */

/* Busca un scancode por nombre (ej. "x", "espacio", "shift_dcho"). */
static SDL_Scancode scancode_from_name(const char *s) {
    for (size_t i = 0; i < sizeof(g_scancode_names)/sizeof(g_scancode_names[0]); i++)
        if (!_stricmp(s, g_scancode_names[i].name)) return g_scancode_names[i].code;
    return SDL_SCANCODE_UNKNOWN;
}

/* Busca un botón Vita por nombre (ej. "cruz", "circulo"). */
static uint32_t vita_btn_from_name(const char *s) {
    for (size_t i = 0; i < sizeof(g_vita_btn_names)/sizeof(g_vita_btn_names[0]); i++)
        if (!_stricmp(s, g_vita_btn_names[i].name)) return g_vita_btn_names[i].btn;
    return 0;
}

/* Busca un bit de stick por nombre (ej. "stick_izq_arriba"). */
static int stick_bit_from_name(const char *s) {
    for (size_t i = 0; i < sizeof(g_stick_names)/sizeof(g_stick_names[0]); i++)
        if (!_stricmp(s, g_stick_names[i].name)) return (int)g_stick_names[i].bit;
    return -1;
}

/* Nombre del botón Vita a partir de su valor. */
static const char *vita_btn_name(uint32_t btn) {
    for (size_t i = 0; i < sizeof(g_vita_btn_names)/sizeof(g_vita_btn_names[0]); i++)
        if (g_vita_btn_names[i].btn == btn) return g_vita_btn_names[i].name;
    return "";
}

/* Nombre de la tecla (scancode) a partir de su código. */
static const char *scancode_name(SDL_Scancode sc) {
    for (size_t i = 0; i < sizeof(g_scancode_names)/sizeof(g_scancode_names[0]); i++)
        if (g_scancode_names[i].code == sc) return g_scancode_names[i].name;
    return "";
}

/* Escribe la sección [teclas] por defecto en el ini si no existe. */
static void input_write_default_keymap(const char *ini) {
    /* Solo escribir si la sección está vacía. */
    char val[64];
    GetPrivateProfileStringA("teclas", "x", "", val, sizeof(val), ini);
    if (val[0]) return;   /* ya hay mapeo personalizado */

    /* Botones. */
    for (size_t i = 0; i < sizeof(g_scancode_names)/sizeof(g_scancode_names[0]); i++) {
        SDL_Scancode sc = g_scancode_names[i].code;
        uint32_t btn = g_key_map[sc];
        if (btn) {
            WritePrivateProfileStringA("teclas", g_scancode_names[i].name, vita_btn_name(btn), ini);
        }
    }
    /* Sticks. */
    for (int i = 0; i < 8; i++) {
        if (g_stick_map[i]) {
            WritePrivateProfileStringA("teclas", g_stick_names[i].name, scancode_name(g_stick_map[i]), ini);
        }
    }
}

/* Carga el mapeo desde MLB15.ini [teclas]. Formato: tecla = botón (ej. x = cruz). */
static void input_load_keymap(void) {
    const char *ini = launcher_ini_path();

    /* Valores por defecto (los mismos que el switch de key_button original). */
    memset(g_key_map, 0, sizeof(g_key_map));
    g_key_map[SDL_SCANCODE_UP]        = VITA_UP;
    g_key_map[SDL_SCANCODE_DOWN]      = VITA_DOWN;
    g_key_map[SDL_SCANCODE_LEFT]      = VITA_LEFT;
    g_key_map[SDL_SCANCODE_RIGHT]     = VITA_RIGHT;
    g_key_map[SDL_SCANCODE_X]         = VITA_CROSS;
    g_key_map[SDL_SCANCODE_SPACE]     = VITA_CROSS;
    g_key_map[SDL_SCANCODE_C]         = VITA_CIRCLE;
    g_key_map[SDL_SCANCODE_Z]         = VITA_SQUARE;
    g_key_map[SDL_SCANCODE_V]         = VITA_TRIANGLE;
    g_key_map[SDL_SCANCODE_Q]         = VITA_L;
    g_key_map[SDL_SCANCODE_E]         = VITA_R;
    g_key_map[SDL_SCANCODE_RETURN]    = VITA_START;
    g_key_map[SDL_SCANCODE_KP_ENTER]  = VITA_START;
    g_key_map[SDL_SCANCODE_RSHIFT]    = VITA_SELECT;

    memset(g_stick_map, 0, sizeof(g_stick_map));
    g_stick_map[0] = SDL_SCANCODE_W;   /* stick_izq_arriba */
    g_stick_map[1] = SDL_SCANCODE_S;   /* stick_izq_abajo */
    g_stick_map[2] = SDL_SCANCODE_A;   /* stick_izq_izquierda */
    g_stick_map[3] = SDL_SCANCODE_D;   /* stick_izq_derecha */
    g_stick_map[4] = SDL_SCANCODE_I;   /* stick_der_arriba */
    g_stick_map[5] = SDL_SCANCODE_K;   /* stick_der_abajo */
    g_stick_map[6] = SDL_SCANCODE_J;   /* stick_der_izquierda */
    g_stick_map[7] = SDL_SCANCODE_L;   /* stick_der_derecha */

    /* Leer [teclas] botones: cada línea "nombre_tecla = nombre_boton". */
    char val[64];
    for (size_t i = 0; i < sizeof(g_scancode_names)/sizeof(g_scancode_names[0]); i++) {
        GetPrivateProfileStringA("teclas", g_scancode_names[i].name, "", val, sizeof(val), ini);
        if (val[0]) {
            uint32_t btn = vita_btn_from_name(val);
            if (btn) g_key_map[g_scancode_names[i].code] = btn;
        }
    }

    /* Leer [teclas] sticks: cada línea "stick_izq_arriba = w" (nombre de tecla). */
    for (size_t i = 0; i < sizeof(g_stick_names)/sizeof(g_stick_names[0]); i++) {
        GetPrivateProfileStringA("teclas", g_stick_names[i].name, "", val, sizeof(val), ini);
        if (val[0]) {
            SDL_Scancode sc = scancode_from_name(val);
            if (sc != SDL_SCANCODE_UNKNOWN) g_stick_map[g_stick_names[i].bit] = sc;
        }
    }

    /* Escribir valores por defecto en el ini para que el usuario los vea. */
    input_write_default_keymap(ini);

    /* [controles] (ventana de opciones, opciones.c): botón de la Vita = tecla (nombre SDL, cualquier
     * tecla). Si existe, manda sobre [teclas]. */
    static const struct { const char *clave; uint32_t btn; int stick; } acc[] = {
        { "cruz", VITA_CROSS, -1 }, { "circulo", VITA_CIRCLE, -1 }, { "cuadrado", VITA_SQUARE, -1 }, { "triangulo", VITA_TRIANGLE, -1 },
        { "l", VITA_L, -1 }, { "r", VITA_R, -1 }, { "start", VITA_START, -1 }, { "select", VITA_SELECT, -1 },
        { "arriba", VITA_UP, -1 }, { "abajo", VITA_DOWN, -1 }, { "izquierda", VITA_LEFT, -1 }, { "derecha", VITA_RIGHT, -1 },
        { "stick_izq_arriba", 0, 0 }, { "stick_izq_abajo", 0, 1 }, { "stick_izq_izquierda", 0, 2 }, { "stick_izq_derecha", 0, 3 },
        { "stick_der_arriba", 0, 4 }, { "stick_der_abajo", 0, 5 }, { "stick_der_izquierda", 0, 6 }, { "stick_der_derecha", 0, 7 },
    };
    GetPrivateProfileStringA("controles", "cruz", "", val, sizeof(val), ini);
    if (val[0]) {
        memset(g_key_map, 0, sizeof(g_key_map));
        memset(g_stick_map, 0, sizeof(g_stick_map));
        for (size_t i = 0; i < sizeof(acc) / sizeof(acc[0]); i++) {
            GetPrivateProfileStringA("controles", acc[i].clave, "", val, sizeof(val), ini);
            SDL_Scancode sc = val[0] ? SDL_GetScancodeFromName(val) : SDL_SCANCODE_UNKNOWN;
            if (sc == SDL_SCANCODE_UNKNOWN) continue;
            if (acc[i].stick >= 0) g_stick_map[acc[i].stick] = sc;
            else g_key_map[sc] = acc[i].btn;
            if (acc[i].btn == VITA_START && sc == SDL_SCANCODE_RETURN) g_key_map[SDL_SCANCODE_KP_ENTER] = VITA_START;
        }
        printf("[input] teclas de [controles] (ventana de opciones)\n");
    }
}

static uint32_t key_button(SDL_Scancode s) {
    if (s < SDL_NUM_SCANCODES) return g_key_map[s];
    return 0;
}

static uint32_t key_stick(SDL_Scancode s) {
    for (int i = 0; i < 8; i++)
        if (g_stick_map[i] == s) return 1u << i;
    return 0;
}

/* Inicialización explícita (llamada desde gpu_vk.c al crear la ventana). */
void input_init(void) {
    input_load_keymap();
}

/* Teclas de los sticks: bit 0..3 izquierdo (arriba, abajo, izq., der.), 4..7 derecho. */
static volatile uint32_t g_stick_keys;

/* Ratón como panel táctil, en coordenadas del panel de la Vita (1920x1088). */
static volatile int g_mouse_down, g_mouse_x, g_mouse_y;

void input_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        uint32_t b = key_button(e->key.keysym.scancode), s = key_stick(e->key.keysym.scancode);
        if (e->type == SDL_KEYDOWN) { g_keys |= b; g_stick_keys |= s; }
        else                        { g_keys &= ~b; g_stick_keys &= ~s; }
        break;
    }
    case SDL_CONTROLLERDEVICEADDED:
        if (!g_ctl) {
            g_ctl = SDL_GameControllerOpen(e->cdevice.which);
            if (g_ctl) printf("[input] mando conectado: %s\n", SDL_GameControllerName(g_ctl));
            fflush(stdout);
        }
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        if (g_ctl && e->cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_ctl))) {
            printf("[input] mando desconectado\n");
            SDL_GameControllerClose(g_ctl);
            g_ctl = NULL;
            g_pad = 0;
        }
        break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) { g_keys = 0; g_stick_keys = 0; g_mouse_down = 0; }
        break;
    /* Ratón = panel táctil delantero (clic izquierdo y arrastrar = deslizar el dedo). */
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEMOTION: {
        SDL_Window *w = SDL_GetWindowFromID(e->type == SDL_MOUSEMOTION ? e->motion.windowID : e->button.windowID);
        int ww = 960, wh = 544, mx, my;
        if (w) SDL_GetWindowSize(w, &ww, &wh);
        if (e->type == SDL_MOUSEMOTION) { mx = e->motion.x; my = e->motion.y; }
        else {
            if (e->button.button != SDL_BUTTON_LEFT) break;
            mx = e->button.x; my = e->button.y;
            g_mouse_down = e->type == SDL_MOUSEBUTTONDOWN;
        }
        if (ww < 1) ww = 1;
        if (wh < 1) wh = 1;
        g_mouse_x = mx * 1920 / ww;
        g_mouse_y = my * 1088 / wh;
        break;
    }
    default:
        break;
    }
}

static uint8_t axis8(Sint16 v) {
    int x = ((int)v + 32768) >> 8;      /* -32768..32767 -> 0..255 */
    if (x > 112 && x < 144) x = 128;    /* zona muerta */
    return (uint8_t)x;
}

static uint8_t keys_axis(uint32_t k, int neg_bit, int pos_bit, uint8_t def) {
    int n = (k >> neg_bit) & 1, p = (k >> pos_bit) & 1;
    if (n && !p) return 0;
    if (p && !n) return 255;
    return def;
}

/* Mandos de Xbox por XInput directamente (xinput1_4 / xinput9_1_0). Con SDL, en el
 * mando del usuario (Xbox One) los botones llegaban por RawInput y los gatillos por
 * XInput; al cargar el partido RawInput dejaba de entregar y solo funcionaban L/R.
 * XInput no depende de la ventana ni del foco. MLB15_XINPUT=0 vuelve a SDL. */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XPad;
typedef struct { DWORD dwPacketNumber; XPad Gamepad; } XState;
typedef DWORD (WINAPI *XGetState)(DWORD, XState *);
static int xinput_poll(uint32_t *pad, uint8_t *lx, uint8_t *ly, uint8_t *rx, uint8_t *ry) {
    static int init;
    static XGetState get;
    if (!init) {
        init = 1;
        const char *e = getenv("MLB15_XINPUT");
        if (e && *e == '0') return 0;
        HMODULE m = LoadLibraryA("xinput1_4.dll");
        if (!m) m = LoadLibraryA("xinput1_3.dll");
        if (!m) m = LoadLibraryA("xinput9_1_0.dll");
        if (m) get = (XGetState)(void (*)(void))GetProcAddress(m, "XInputGetState");
        printf("[input] XInput: %s\n", get ? "sí" : "no disponible");
        fflush(stdout);
    }
    if (!get) return 0;
    static int which = -1;
    static ULONGLONG next_scan;
    XState st;
    if (which >= 0 && get((DWORD)which, &st) != 0) which = -1;
    if (which < 0) {
        ULONGLONG now = GetTickCount64();
        if (now < next_scan) return 0;
        next_scan = now + 1000;   /* buscar mando una vez por segundo (barato) */
        for (int i = 0; i < 4 && which < 0; i++) if (get((DWORD)i, &st) == 0) which = i;
        if (which < 0) return 0;
        printf("[input] mando XInput %d conectado\n", which);
        fflush(stdout);
    }
    WORD b = st.Gamepad.wButtons;
    static const struct { WORD x; uint32_t v; } map[] = {
        { 0x1000, VITA_CROSS }, { 0x2000, VITA_CIRCLE }, { 0x4000, VITA_SQUARE }, { 0x8000, VITA_TRIANGLE },
        { 0x0100, VITA_L }, { 0x0200, VITA_R }, { 0x0010, VITA_START }, { 0x0020, VITA_SELECT },
        { 0x0001, VITA_UP }, { 0x0002, VITA_DOWN }, { 0x0004, VITA_LEFT }, { 0x0008, VITA_RIGHT },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) if (b & map[i].x) *pad |= map[i].v;
    static int trig[2];
    if (st.Gamepad.bLeftTrigger > 160) trig[0] = 1; else if (st.Gamepad.bLeftTrigger < 60) trig[0] = 0;
    if (st.Gamepad.bRightTrigger > 160) trig[1] = 1; else if (st.Gamepad.bRightTrigger < 60) trig[1] = 0;
    if (trig[0]) *pad |= VITA_L;
    if (trig[1]) *pad |= VITA_R;
    *lx = axis8(st.Gamepad.sThumbLX);
    *ly = axis8((Sint16)(st.Gamepad.sThumbLY == -32768 ? 32767 : -st.Gamepad.sThumbLY));   /* XInput: y hacia arriba */
    *rx = axis8(st.Gamepad.sThumbRX);
    *ry = axis8((Sint16)(st.Gamepad.sThumbRY == -32768 ? 32767 : -st.Gamepad.sThumbRY));
    return 1;
}

void input_poll(void) {
    uint32_t pad = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    if (xinput_poll(&pad, &lx, &ly, &rx, &ry)) {
        /* mando de Xbox leído por XInput: SDL no hace falta */
    } else if (g_ctl) {
        static const struct { SDL_GameControllerButton b; uint32_t v; } map[] = {
            { SDL_CONTROLLER_BUTTON_A, VITA_CROSS },        { SDL_CONTROLLER_BUTTON_B, VITA_CIRCLE },
            { SDL_CONTROLLER_BUTTON_X, VITA_SQUARE },       { SDL_CONTROLLER_BUTTON_Y, VITA_TRIANGLE },
            { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, VITA_L }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, VITA_R },
            { SDL_CONTROLLER_BUTTON_START, VITA_START },    { SDL_CONTROLLER_BUTTON_BACK, VITA_SELECT },
            { SDL_CONTROLLER_BUTTON_DPAD_UP, VITA_UP },     { SDL_CONTROLLER_BUTTON_DPAD_DOWN, VITA_DOWN },
            { SDL_CONTROLLER_BUTTON_DPAD_LEFT, VITA_LEFT }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, VITA_RIGHT },
        };
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (SDL_GameControllerGetButton(g_ctl, map[i].b)) pad |= map[i].v;
        /* Gatillos como L/R con histéresis: con el umbral fijo en 16000 los gatillos del
         * mando del usuario (Xbox One) parpadeaban entre pulsado y suelto sin tocarlos; en
         * el partido L/R cambian de modo (corredores, tipos de lanzamiento) y el bateo y lo
         * demás dejaban de responder. Pulsado por encima de 26000, suelto por debajo de 10000. */
        static int trig[2];
        Sint16 tl = SDL_GameControllerGetAxis(g_ctl, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        Sint16 tr = SDL_GameControllerGetAxis(g_ctl, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (tl > 26000) trig[0] = 1; else if (tl < 10000) trig[0] = 0;
        if (tr > 26000) trig[1] = 1; else if (tr < 10000) trig[1] = 0;
        if (trig[0]) pad |= VITA_L;
        if (trig[1]) pad |= VITA_R;
        {
            static int told;
            if (!told && (tl > 4000 || tr > 4000)) { told = 1; printf("[input] gatillos en %d / %d\n", tl, tr); fflush(stdout); }
        }
        lx = axis8(SDL_GameControllerGetAxis(g_ctl, SDL_CONTROLLER_AXIS_LEFTX));
        ly = axis8(SDL_GameControllerGetAxis(g_ctl, SDL_CONTROLLER_AXIS_LEFTY));
        rx = axis8(SDL_GameControllerGetAxis(g_ctl, SDL_CONTROLLER_AXIS_RIGHTX));
        ry = axis8(SDL_GameControllerGetAxis(g_ctl, SDL_CONTROLLER_AXIS_RIGHTY));
    }
    uint32_t k = g_stick_keys;
    lx = keys_axis(k, 2, 3, lx); ly = keys_axis(k, 0, 1, ly);
    rx = keys_axis(k, 6, 7, rx); ry = keys_axis(k, 4, 5, ry);
    {   /* registro (datos\log.txt): los primeros cambios de botones del mando */
        static unsigned n;
        static uint32_t last;
        if (pad != last && n < 400) {
            n++;
            printf("[input] mando: botones 0x%04X sticks %u,%u %u,%u\n", pad, lx, ly, rx, ry);
            fflush(stdout);
        }
        last = pad;
    }
    g_pad = pad;
    g_sticks = (uint32_t)lx | ((uint32_t)ly << 8) | ((uint32_t)rx << 16) | ((uint32_t)ry << 24);
}

/* ------------------------------------------------------------------------- */
/* Guion de pulsaciones (MLB15_PAD_GUION)                                    */
/* ------------------------------------------------------------------------- */

#define GUION_MAX 512
#define GUION_PULSO_MS 150
/* Gestos táctiles del guion (bits por encima de los botones de la Vita):
 * deslizar el dedo por el centro de la pantalla en 0,4 s, o un toque corto. */
#define GUION_DESLIZA_IZQ 0x10000u
#define GUION_DESLIZA_DER 0x20000u
#define GUION_TOQUE       0x40000u
#define GUION_DESLIZA_SWIPE 0x80000u   /* desde el botón SWIPE (arriba a la derecha) hacia la izquierda */
#define GUION_GESTOS      (GUION_DESLIZA_IZQ | GUION_DESLIZA_DER | GUION_TOQUE | GUION_DESLIZA_SWIPE)
#define GUION_GESTO_MS    400
/* modo 0: t absoluto; 1: "+N" = N s tras la pulsación anterior; 2: "@TEXTO+N" =
 * N s tras abrir el juego un archivo cuyo nombre contiene TEXTO (cuando las
 * anteriores ya se hicieron). t < 0: aún sin resolver. */
static struct { double t; uint32_t b; int dicho; int tx, ty; int modo, armado; double retardo; char trig[64]; } g_guion[GUION_MAX];
static int g_nguion = -1;
static ULONGLONG g_t0;

static uint32_t nombre_boton(const char *s, size_t n) {
    static const struct { const char *n; uint32_t b; } t[] = {
        { "cruz", VITA_CROSS }, { "circulo", VITA_CIRCLE }, { "cuadrado", VITA_SQUARE }, { "triangulo", VITA_TRIANGLE },
        { "arriba", VITA_UP }, { "abajo", VITA_DOWN }, { "izquierda", VITA_LEFT }, { "derecha", VITA_RIGHT },
        { "l", VITA_L }, { "r", VITA_R }, { "start", VITA_START }, { "select", VITA_SELECT },
        { "desliza_izq", GUION_DESLIZA_IZQ }, { "desliza_der", GUION_DESLIZA_DER }, { "toque", GUION_TOQUE }, { "desliza_swipe", GUION_DESLIZA_SWIPE },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (strlen(t[i].n) == n && !_strnicmp(s, t[i].n, n)) return t[i].b;
    return 0;
}

static void guion_cargar(void) {
    g_nguion = 0;
    const char *g = getenv("MLB15_PAD_GUION");
    while (g && *g && g_nguion < GUION_MAX) {
        char *fin;
        int modo = 0;
        char trig[64] = "";
        if (*g == '@') {
            size_t k = strcspn(g + 1, "+:");
            snprintf(trig, sizeof(trig), "%.*s", (int)k, g + 1);
            _strupr(trig);
            g += 1 + k;
            modo = 2;
        } else if (*g == '+') {
            modo = 1;
        }
        double t = strtod(g, &fin);
        if (fin == g || *fin != ':') break;
        g_guion[g_nguion].modo = modo;
        g_guion[g_nguion].retardo = t;
        snprintf(g_guion[g_nguion].trig, sizeof(g_guion[0].trig), "%s", trig);
        g_guion[g_nguion].armado = modo == 2 && g_nguion == 0;
        if (modo == 1) t = g_nguion == 0 ? t : -1;
        if (modo == 2) t = -1;
        const char *nb = fin + 1;
        size_t n = strcspn(nb, ",");
        uint32_t b = nombre_boton(nb, n);
        int tx = -1, ty = -1;
        if (!b && n > 6 && !_strnicmp(nb, "toque_", 6) && sscanf(nb + 6, "%d_%d", &tx, &ty) == 2)
            b = GUION_TOQUE;                       /* toque_X_Y: en coordenadas de pantalla 960x544 */
        if (b) { g_guion[g_nguion].t = t; g_guion[g_nguion].b = b; g_guion[g_nguion].dicho = 0; g_guion[g_nguion].tx = tx; g_guion[g_nguion].ty = ty; g_nguion++; }
        else printf("[input] guion: botón desconocido '%.*s'\n", (int)n, nb);
        g = nb + n;
        if (*g == ',') g++;
    }
    if (g_nguion) printf("[input] guion de pad: %d pulsaciones\n", g_nguion);
}

/* La entrada i ya se hizo en el segundo s: resuelve la siguiente. */
static void guion_hecho(int i, double s) {
    if (i + 1 >= g_nguion) return;
    if (g_guion[i + 1].modo == 1 && g_guion[i + 1].t < 0) g_guion[i + 1].t = s + g_guion[i + 1].retardo;
    if (g_guion[i + 1].modo == 2) g_guion[i + 1].armado = 1;
}

/* Lo llama FIOS al abrir un archivo: dispara las entradas "@TEXTO+N". */
void input_evento(const char *nombre) {
    if (g_nguion <= 0) return;
    char up[512];
    snprintf(up, sizeof(up), "%s", nombre);
    _strupr(up);
    double s = (double)(GetTickCount64() - g_t0) / 1000.0;
    for (int i = 0; i < g_nguion; i++)
        if (g_guion[i].modo == 2 && g_guion[i].armado && g_guion[i].t < 0 && strstr(up, g_guion[i].trig)) {
            g_guion[i].t = s + g_guion[i].retardo;
            printf("[input] guion: '%s' a los %.1f s -> pulsación a los %.1f s\n", g_guion[i].trig, s, g_guion[i].t);
            fflush(stdout);
        }
}

static uint32_t guion_botones(void) {
    if (g_nguion < 0) { guion_cargar(); g_t0 = GetTickCount64(); }
    if (!g_nguion) return 0;
    double s = (double)(GetTickCount64() - g_t0) / 1000.0;
    uint32_t b = 0;
    for (int i = 0; i < g_nguion; i++) {
        if (g_guion[i].b & GUION_GESTOS) continue;           /* eso va por input_touch */
        /* Si el juego no leyó el pad en su ventana (cargando), la pulsación empieza ahora:
         * si no, se perdía y las siguientes ("+N") no se programaban nunca. */
        if (g_guion[i].t >= 0 && !g_guion[i].dicho && s >= g_guion[i].t + GUION_PULSO_MS / 1000.0) g_guion[i].t = s;
        if (g_guion[i].t >= 0 && s >= g_guion[i].t && s < g_guion[i].t + GUION_PULSO_MS / 1000.0) {
            b |= g_guion[i].b;
            if (!g_guion[i].dicho) {
                g_guion[i].dicho = 1;
                guion_hecho(i, s);
                printf("[input] guion: %.1f s botones 0x%04X\n", s, g_guion[i].b);
                fflush(stdout);
            }
        }
    }
    return b;
}

uint32_t input_buttons(void) { return g_keys | g_pad | guion_botones(); }

/* Dedo en el panel delantero: el ratón (clic izquierdo) o un gesto del guion. */
int input_touch(int *x, int *y) {
    if (g_mouse_down) { *x = g_mouse_x; *y = g_mouse_y; return 1; }
    if (g_nguion < 0) { guion_cargar(); g_t0 = GetTickCount64(); }
    double s = (double)(GetTickCount64() - g_t0) / 1000.0;
    for (int i = 0; i < g_nguion; i++) {
        uint32_t g = g_guion[i].b & GUION_GESTOS;
        double dur = (g == GUION_TOQUE ? 100 : GUION_GESTO_MS) / 1000.0;
        if (!g || g_guion[i].t < 0 || s < g_guion[i].t || s >= g_guion[i].t + dur) continue;
        double p = (s - g_guion[i].t) / dur;
        if (!g_guion[i].dicho) {
            g_guion[i].dicho = 1;
            guion_hecho(i, s);
            printf("[input] guion: %.1f s gesto táctil %s\n", s,
                   g == GUION_TOQUE ? "toque" : g == GUION_DESLIZA_IZQ ? "deslizar a la izquierda" : g == GUION_DESLIZA_SWIPE ? "deslizar desde SWIPE" : "deslizar a la derecha");
            fflush(stdout);
        }
        if (g == GUION_TOQUE && g_guion[i].tx >= 0) { *x = g_guion[i].tx * 2; *y = g_guion[i].ty * 2; return 1; }
        *y = g == GUION_DESLIZA_SWIPE ? 130 : 544;
        *x = g == GUION_TOQUE ? 960 : g == GUION_DESLIZA_DER ? (int)(400 + p * 1100)
           : g == GUION_DESLIZA_SWIPE ? (int)(1700 - p * 1300) : (int)(1500 - p * 1100);
        return 1;
    }
    return 0;
}

void input_sticks(uint8_t out[4]) {
    uint32_t s = g_sticks;
    out[0] = (uint8_t)s; out[1] = (uint8_t)(s >> 8); out[2] = (uint8_t)(s >> 16); out[3] = (uint8_t)(s >> 24);
}
