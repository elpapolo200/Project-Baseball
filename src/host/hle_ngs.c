/* SceNgs (Scream) para el port de MLB 15.
 *
 * El juego trae su propio motor de audio ("Scream") y lo monta sobre SceNgs:
 * pide definiciones de voz, crea un sistema y racks, inicializa voces, las
 * enruta (patches) hacia los buses y el master, y en cada actualización
 * (sceNgsSystemUpdate, 512 muestras a 48 kHz) lee la mezcla del módulo de
 * salida del master con sceNgsVoiceGetStateData y la manda a sceAudioOut.
 *
 * Aquí se sintetiza de verdad, siguiendo el diseño de Vita3K (ngs/, GPLv2,
 * Copyright (C) Vita3K team; el port es GPL desde 2026-10-26):
 * - voces Scream: módulo 0 = player (PCM s16 o HE-ADPCM, tabla de Vita3K
 *   codec/pcm.cpp, de vgmstream); voces Scream AT9: módulo 0 = ATRAC9
 *   (LibAtrac9, MIT, Alex Barney);
 * - envolvente (módulo 0x5CE3: 4 puntos tiempo/amplitud/curva, release y bucle) con
 *   key-off de verdad: el sonido se apaga en su tiempo de release en vez de cortarse;
 *   MLB15_NGS_ENVOLVENTE=0 vuelve al corte;
 * - bus de reverb (0x5CE7, parámetros I3DL2: room, decay, reflections, reverb,
 *   retardos...): sale solo el efecto (antes pasaba la señal tal cual y las voces que
 *   se enviaban ahí sonaban dobles); MLB15_NGS_REVERB=0 vuelve a lo anterior;
 * - EQ, filtros y compresor no se procesan (el juego los deja neutros);
 * - patches con matriz de volúmenes 2x2 [canal origen][canal destino];
 * - callbacks del juego (fin de voz, buffer cambiado/en bucle/fin de datos)
 *   se llaman en el hilo que hace el Update, como en la Vita.
 *
 * Lo que también importa es el ESTADO: los handles son objetos en la arena
 * guest y cada voz lleva su estado (available/active/paused/key-off), su
 * contador de updates y la informacion que sceNgsVoiceGetInfo devuelve. El
 * juego comprueba ese estado (una asercion en snd_synth_interface.cpp:1591 lo
 * hace) y con handles inventados o estructuras a medias se para en seco.
 *
 * Firmas: la API moderna usa HANDLES opacos (SceNgsHSynSystem, SceNgsHRack,
 * SceNgsHVoice, SceNgsHPatch), no punteros a estructuras. Por eso las salidas
 * van en el ultimo registro de la firma (r3 en Init, r1 en CreateRouting...)
 * y NO en la pila: confundirlo corrompe el marco del llamador.
 *
 * MLB15_NGS_TRACE=1 traza llamadas; MLB15_NGS=0 vuelve a no sintetizar nada.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/dispatch.h"
#include "vitarecomp/mem.h"

#include "port.h"

#define COMPILING_DLL
#include "libatrac9.h"

#define NGS_OK     0u
#define NGS_ERROR  ((uint32_t)-1)

enum { K_SYS = 1, K_RACK, K_VOICE, K_PATCH };
enum { T_NONE, T_SCREAM, T_AT9, T_MASTER, T_REVERB, T_COMP, T_COMP_SC };
enum { ST_AVAILABLE = 0, ST_ACTIVE = 1, ST_PAUSED = 0x20, ST_KEY_OFF = 0x40 };

#define PLAYER_ID 0x01015CE6u
#define AT9_ID    0x01015CAAu
#define MAX_G     1024                    /* granularidad máxima admitida */

typedef struct {
    uint32_t kind;
    uint32_t parent;          /* sistema (rack/patch) o rack (voz)   */
    uint32_t index;           /* indice de la voz dentro del rack    */
    uint32_t state;           /* SceNgsVoiceState                    */
    uint32_t update_passed;
    uint32_t num_modules;
    uint32_t num_inputs;
    uint32_t num_outputs;
    uint32_t patches_per_output;
    uint32_t voice_count;
    uint32_t max_voices;
    uint32_t src_voice;       /* solo patches                        */
    uint32_t dst_voice;
    uint32_t type;            /* T_*: rack y voz                     */
    uint32_t channels;        /* canales por voz (rack y voz)        */
    uint32_t slot;            /* voz: índice en g_vh; patch: en g_pt */
} Obj;

static Obj *obj_at(uint32_t addr, uint32_t kind) {
    Obj *o = (Obj *)vita_mem_ptr(addr, sizeof(Obj));
    if (!o || o->kind != kind) return NULL;
    return o;
}

static uint32_t obj_new(uint32_t kind, const char *name) {
    uint32_t a = port_alloc(sizeof(Obj), 0x40, name);
    Obj *o = a ? (Obj *)vita_mem_ptr(a, sizeof(Obj)) : NULL;
    if (!o) return 0;
    memset(o, 0, sizeof(*o));
    o->kind = kind;
    return a;
}

/* --- estado de síntesis por voz (anfitrión) ------------------------------- */

typedef struct {
    uint32_t handle;
    int      type, channels;
    uint8_t  params[160];     /* módulo 0: SceNgsPlayerParams / SceNgsAT9Params (con descriptor) */
    uint32_t params_id;
    uint32_t lock_blk[8];     /* bloque guest que entrega LockParams por módulo */
    uint32_t fin_cb, fin_ud, mod_cb[8], mod_ud[8];
    /* reproducción */
    int      cur_buf, loop_cnt;
    int32_t  byte_pos;
    int32_t  samples_keyon, bytes_keyon, samples_total, bytes_total;
    int32_t  hist[2][4];      /* HE-ADPCM */
    void    *at9;
    uint32_t at9_cfg;
    Atrac9CodecInfo at9_info;
    uint8_t  stage[1024];     /* superframe AT9 partido entre dos buffers */
    uint32_t stage_n;
    float   *q;               /* PCM decodificado (estéreo, frecuencia de origen) */
    uint32_t qn, qcap;
    double   pos;             /* posición fraccional en q */
    int      finish_pending;  /* key-off / fin de datos: cerrar en el próximo Update */
    /* params de los demás módulos (id + tamaño + datos, tal cual) */
    uint8_t  mparams[8][128];
    uint32_t bypass;          /* bit por módulo */
    /* envolvente */
    float    env_amp;         /* amplitud al final del último bloque */
    int      env_seg;         /* segmento actual (punto de origen) */
    double   env_ms;          /* ms dentro del segmento */
    int      env_rel;         /* en release */
    double   env_rel_ms;
    float    env_rel_from;
    /* reverb (solo buses T_REVERB) */
    struct Reverb *rev;
    /* mezcla */
    float    in[2 * MAX_G], out[2 * MAX_G];
    int16_t  out16[2 * MAX_G];
    int      done;
} VHost;

typedef struct {
    uint32_t handle;
    int      src, dst;        /* slots de voz; -1 = libre */
    float    vol[2][2];
} PHost;

static VHost   *g_vh[1024];
static int      g_nvh;
static PHost    g_pt[2048];
static int      g_npt;
static uint32_t g_gran = 512, g_rate = 48000, g_updates;
static CRITICAL_SECTION g_cs;
static int g_cs_init;

static void lock(void) {
    if (!g_cs_init) { InitializeCriticalSection(&g_cs); g_cs_init = 1; }
    EnterCriticalSection(&g_cs);
}
static void unlock(void) { LeaveCriticalSection(&g_cs); }

static int synth_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("MLB15_NGS"); on = e ? (*e != '0') : 1; }
    return on;
}

static VHost *vh_of(uint32_t voice) {
    Obj *v = obj_at(voice, K_VOICE);
    return v && v->slot < (uint32_t)g_nvh ? g_vh[v->slot] : NULL;
}

/* Memoria de un handle de voz ya entregado a un (rack, indice). Repetir
 * sceNgsRackGetVoiceHandle debe devolver el MISMO handle: el juego indexa sus
 * propias tablas por el y comparar handles distintos rompe su contabilidad. */
static uint32_t g_voice_rack[1024];
static uint32_t g_voice_idx[1024];
static uint32_t g_voice_handle[1024];
static int      g_voice_map;

static int g_trace = -1;
static int trace_on(void) {
    if (g_trace < 0) g_trace = getenv("MLB15_NGS_TRACE") ? 1 : 0;
    return g_trace;
}

/* MLB15_NGS_TRACE=1: las primeras llamadas de cada función con sus registros. */
#define TRN(name, n) do { static int _c; if (trace_on() && _c++ < (n)) { \
    printf("[ngs] %s(r0=0x%X r1=0x%X r2=0x%X r3=0x%X)\n", name, r0, r1, r2, r3); fflush(stdout); } } while (0)

#define ONCE(name) do { static int _o; if (!_o) { _o = 1; \
    printf("[ngs] %s(r0=0x%X r1=0x%X r2=0x%X r3=0x%X)\n", name, r0, r1, r2, r3); \
    fflush(stdout); } } while (0)

static uint32_t read_u32(uint32_t addr, uint32_t dflt) {
    return vita_mem_ptr(addr, 4) ? vita_read32(addr) : dflt;
}

static float *volume_matrix(uint32_t addr) {
    return (float *)vita_mem_ptr(addr, 4 * sizeof(float));
}

/* --- callbacks al juego ---------------------------------------------------- */
/* SceNgsCallbackInfo en la pila guest: voz, rack, id de módulo, motivo 1 y 2,
 * puntero, datos del usuario. Se guardan y restauran todos los registros: el
 * llamador (el hilo de audio en sceNgsSystemUpdate) sigue después como si nada. */
static void call_guest(uint32_t cb, uint32_t voice, uint32_t modid, uint32_t reason1, uint32_t reason2,
                       uint32_t ptr, uint32_t ud) {
    if (!cb) return;
    Obj *v = obj_at(voice, K_VOICE);
    uint32_t sv[15] = { r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, sp, lr };
    int fl[4] = { flag_n, flag_z, flag_c, flag_v };
    sp = (sp - 0x40) & ~7u;
    uint32_t info = sp;
    vita_write32(info + 0x00, voice);
    vita_write32(info + 0x04, v ? v->parent : 0);
    vita_write32(info + 0x08, modid);
    vita_write32(info + 0x0C, reason1);
    vita_write32(info + 0x10, reason2);
    vita_write32(info + 0x14, ptr);
    vita_write32(info + 0x18, ud);
    r0 = info;
    lr = 0;
    unlock();                                  /* el callback puede llamar a sceNgs* */
    vita_dispatch(0, cb);
    lock();
    r0 = sv[0]; r1 = sv[1]; r2 = sv[2]; r3 = sv[3]; r4 = sv[4]; r5 = sv[5]; r6 = sv[6]; r7 = sv[7];
    r8 = sv[8]; r9 = sv[9]; r10 = sv[10]; r11 = sv[11]; r12 = sv[12]; sp = sv[13]; lr = sv[14];
    flag_n = fl[0]; flag_z = fl[1]; flag_c = fl[2]; flag_v = fl[3];
}

/* --- definiciones de voz -------------------------------------------------- */
/* Estructuras constantes del firmware. El juego solo guarda el puntero y lo
 * pasa en SceNgsRackDescription; aquí llevan el tipo en la primera palabra. */

static uint32_t def_block(const char *name, uint32_t size, int type) {
    static struct { const char *n; uint32_t p; } cache[16];
    static int n;
    for (int i = 0; i < n; i++) if (cache[i].n == name) return cache[i].p;
    uint32_t p = port_alloc(size, 0x40, name);
    if (p) { vita_write32(p, 0x4447534Eu); vita_write32(p + 4, (uint32_t)type); }   /* "NSGD", tipo */
    if (p && n < 16) { cache[n].n = name; cache[n].p = p; n++; }
    return p;
}
static int def_type(uint32_t p) {
    return read_u32(p, 0) == 0x4447534Eu ? (int)read_u32(p + 4, 0) : T_NONE;
}

void vita_hle_sceNgsVoiceDefGetScreamVoice(void) { ONCE("VoiceDefGetScreamVoice"); r0 = def_block("ngs scream", 0x200, T_SCREAM); }
void vita_hle_sceNgsVoiceDefGetScreamAtrac9Voice(void) { r0 = def_block("ngs atrac9", 0x200, T_AT9); }
void vita_hle_sceNgsVoiceDefGetMasterBuss(void) { r0 = def_block("ngs master", 0x100, T_MASTER); }
void vita_hle_sceNgsVoiceDefGetReverbBuss(void) { r0 = def_block("ngs reverb", 0x100, T_REVERB); }
void vita_hle_sceNgsVoiceDefGetCompressorBuss(void) { r0 = def_block("ngs comp", 0x100, T_COMP); }
void vita_hle_sceNgsVoiceDefGetCompressorSideChainBuss(void) { r0 = def_block("ngs comp side", 0x100, T_COMP_SC); }

/* --- sistema -------------------------------------------------------------- */

void vita_hle_sceNgsSystemGetRequiredMemorySize(void) {
    /* r0 = SceNgsSystemInitParams*, r1 = SceSize* */
    uint32_t params = r0, out = r1;
    uint32_t max_voices = read_u32(params + 4, 64);
    if (max_voices > 256) max_voices = 256;
    uint32_t size = 0x10000 + max_voices * 0x4000;
    if (vita_mem_ptr(out, 4)) vita_write32(out, size);
    r0 = NGS_OK;
}

void vita_hle_sceNgsSystemInit(void) {
    /* r0 = memoria, r1 = tamaño, r2 = params {racks, voces, granularidad, frecuencia, módulos},
     * r3 = SceNgsHSynSystem* */
    uint32_t mem = r0, size = r1, params = r2, out = r3;
    uint32_t h = obj_new(K_SYS, "ngs system");
    Obj *s = h ? obj_at(h, K_SYS) : NULL;
    if (!s) { r0 = NGS_ERROR; return; }
    s->max_voices = read_u32(params + 4, 64);
    s->num_modules = 8;                       /* voces del sistema Scream   */
    uint32_t g = read_u32(params + 8, 512), rate = read_u32(params + 12, 48000);
    g_gran = g && g <= MAX_G ? g : 512;
    g_rate = rate ? rate : 48000;
    if (!vita_mem_ptr(out, 4)) { r0 = NGS_ERROR; return; }
    vita_write32(out, h);
    ONCE("sceNgsSystemInit");
    printf("[ngs]   sistema=0x%X mem=0x%X tamano=0x%X max_voces=%u granularidad=%u %u Hz\n",
           h, mem, size, s->max_voices, g_gran, g_rate);
    fflush(stdout);
    r0 = NGS_OK;
}

void vita_hle_sceNgsSystemSetFlags(void) { r0 = NGS_OK; }
void vita_hle_sceNgsSystemLock(void) { r0 = NGS_OK; }
void vita_hle_sceNgsSystemUnlock(void) { r0 = NGS_OK; }

/* --- decodificación --------------------------------------------------------- */

/* Tabla HE-ADPCM ("HEVAG") de Vita3K codec/pcm.cpp (investigación de id-daemon,
 * implementación de vgmstream: bnnm, korenkonder). */
static const int16_t hevag_coefs[128][4] = {
    { 0, 0, 0, 0 }, { 7680, 0, 0, 0 }, { 14720, -6656, 0, 0 }, { 12544, -7040, 0, 0 },
    { 15616, -7680, 0, 0 }, { 14731, -7059, 0, 0 }, { 14507, -7366, 0, 0 }, { 13920, -7522, 0, 0 },
    { 13133, -7680, 0, 0 }, { 12028, -7680, 0, 0 }, { 10764, -7680, 0, 0 }, { 9359, -7680, 0, 0 },
    { 7832, -7680, 0, 0 }, { 6201, -7680, 0, 0 }, { 4488, -7680, 0, 0 }, { 2717, -7680, 0, 0 },
    { 910, -7680, 0, 0 }, { -910, -7680, 0, 0 }, { -2717, -7680, 0, 0 }, { -4488, -7680, 0, 0 },
    { -6201, -7680, 0, 0 }, { -7832, -7680, 0, 0 }, { -9359, -7680, 0, 0 }, { -10764, -7680, 0, 0 },
    { -12028, -7680, 0, 0 }, { -13133, -7680, 0, 0 }, { -13920, -7522, 0, 0 }, { -14507, -7366, 0, 0 },
    { -14731, -7059, 0, 0 }, { 5376, -9216, 3328, -3072 }, { -6400, -7168, -3328, -2304 }, { -10496, -7424, -3584, -1024 },
    { -167, -2722, -494, -541 }, { -7430, -2221, -2298, 424 }, { -8001, -3166, -2814, 289 }, { 6018, -4750, 2649, -1298 },
    { 3798, -6946, 3875, -1216 }, { -8237, -2596, -2071, 227 }, { 9199, 1982, -1382, -2316 }, { 13021, -3044, -3792, 1267 },
    { 13112, -4487, -2250, 1665 }, { -1668, -3744, -6456, 840 }, { 7819, -4328, 2111, -506 }, { 9571, -1336, -757, 487 },
    { 10032, -2562, 300, 199 }, { -4745, -4122, -5486, -1493 }, { -5896, 2378, -4787, -6947 }, { -1193, -9117, -1237, -3114 },
    { 2783, -7108, -1575, -1447 }, { -7334, -2062, -2212, 446 }, { 6127, -2577, -315, -18 }, { 9457, -1858, 102, 258 },
    { 7876, -4483, 2126, -538 }, { -7172, -1795, -2069, 482 }, { -7358, -2102, -2233, 440 }, { -9170, -3509, -2674, -391 },
    { -2638, -2647, -1929, -1637 }, { 1873, 9183, 1860, -5746 }, { 9214, 1859, -1124, -2427 }, { 13204, -3012, -4139, 1370 },
    { 12437, -4792, -256, 622 }, { -2653, -1144, -3182, -6878 }, { 9331, -1048, -828, 507 }, { 1642, -620, -946, -4229 },
    { 4246, -7585, -533, -2259 }, { -8988, -3891, -2807, 44 }, { -2562, -2735, -1730, -1899 }, { 3182, -483, -714, -1421 },
    { 7937, -3844, 2821, -1019 }, { 10069, -2609, 314, 195 }, { 8400, -3297, 1551, -155 }, { -8529, -2775, -2432, -336 },
    { 9477, -1882, 108, 256 }, { 75, -2241, -298, -6937 }, { -9143, -4160, -2963, 5 }, { -7270, -1958, -2156, 460 },
    { -2740, 3745, 5936, -1089 }, { 8993, 1948, -683, -2704 }, { 13101, -2835, -3854, 1055 }, { 9543, -1961, 130, 250 },
    { 5272, -4270, 3124, -3157 }, { -7696, -3383, -2907, -456 }, { 7309, 2523, 434, -2461 }, { 10275, -2867, 391, 172 },
    { 10940, -3721, 665, 97 }, { 24, -310, -1262, 320 }, { -8122, -2411, -2311, -271 }, { -8511, -3067, -2337, 163 },
    { 326, -3846, 419, -933 }, { 8895, 2194, -541, -2880 }, { 12073, -1876, -2017, -601 }, { 8729, -3423, 1674, -169 },
    { 12950, -3847, -3007, 1946 }, { 10038, -2570, 302, 198 }, { 9385, -2757, 1008, 41 }, { -4720, -5006, -2852, -1161 },
    { 7869, -4326, 2135, -501 }, { 2450, -8597, 1299, -2780 }, { 10192, -2763, 360, 181 }, { 11313, -4213, 833, 53 },
    { 10154, -2716, 345, 185 }, { 9638, -1417, -737, 482 }, { 3854, -4554, 2843, -3397 }, { 6699, -5659, 2249, -1074 },
    { 11082, -3908, 728, 80 }, { -1026, -9810, -805, -3462 }, { 10396, -3746, 1367, -96 }, { 10287, 988, -1915, -1437 },
    { 7953, 3878, -764, -3263 }, { 12689, -3375, -3354, 2079 }, { 6641, 3166, 231, -2089 }, { -2348, -7354, -1944, -4122 },
    { 9290, -4039, 1885, -246 }, { 4633, -6403, 1748, -1619 }, { 11247, -4125, 802, 61 }, { 9807, -2284, 219, 222 },
    { 9736, -1536, -706, 473 }, { 8440, -3436, 1562, -176 }, { 9307, -1021, -835, 509 }, { 1698, -9025, 688, -3037 },
    { 10214, -2791, 368, 179 }, { 8390, 3248, -758, -2989 }, { 7201, 3316, 46, -2614 }, { -88, -7809, -538, -4571 },
    { 6193, -5189, 2760, -1245 }, { 12325, -1290, -3284, 253 }, { 13064, -4075, -2824, 1877 }, { 5333, 2999, 775, -1132 }
};
static const int nibble_lookup[16] = { 0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1 };

static void q_push(VHost *v, float l, float r) {
    if (v->qn + 1 > v->qcap) {
        uint32_t nc = v->qcap ? v->qcap * 2 : 8192;
        float *nq = (float *)realloc(v->q, nc * 2 * sizeof(float));
        if (!nq) return;
        v->q = nq; v->qcap = nc;
    }
    v->q[2 * v->qn] = l; v->q[2 * v->qn + 1] = r;
    v->qn++;
}

/* Un marco HE-ADPCM (16 bytes, 28 muestras) de un canal. */
static void adpcm_frame(const uint8_t *frame, int32_t *h, int16_t out[28]) {
    uint8_t coef = (uint8_t)(((frame[0] >> 4) & 0xF) | (frame[1] & 0xF0));
    int shift = 20 - (frame[0] & 0xF);
    uint8_t flag = frame[1] & 0xF;
    if (coef > 127) coef = 0;
    int32_t h1 = h[0], h2 = h[1], h3 = h[2], h4 = h[3];
    for (int j = 0; j < 28; j++) {
        int32_t s = 0;
        if (flag < 7) {
            uint8_t nb = frame[2 + j / 2];
            s = (j & 1 ? nibble_lookup[nb >> 4] : nibble_lookup[nb & 0xF]) * (1 << shift);
            s += (h1 * hevag_coefs[coef][0] + h2 * hevag_coefs[coef][1] + h3 * hevag_coefs[coef][2] + h4 * hevag_coefs[coef][3]) >> 5;
            s >>= 8;
        }
        out[j] = (int16_t)(s < -32768 ? -32768 : s > 32767 ? 32767 : s);
        h4 = h3; h3 = h2; h2 = h1; h1 = s;
    }
    h[0] = h1; h[1] = h2; h[2] = h3; h[3] = h4;
}

/* Desplazamientos en los params (con el descriptor de 8 bytes delante). */
#define PB(v, i)       ((v)->params + 8 + (i) * ((v)->type == T_AT9 ? 16 : 12))
static uint32_t pb_buf(VHost *v, int i)   { uint32_t x; memcpy(&x, PB(v, i), 4); return x; }
static int32_t  pb_bytes(VHost *v, int i) { int32_t x; memcpy(&x, PB(v, i) + 4, 4); return x; }
static int16_t  pb_loop(VHost *v, int i)  { int16_t x; memcpy(&x, PB(v, i) + 8, 2); return x; }
static int16_t  pb_next(VHost *v, int i)  { int16_t x; memcpy(&x, PB(v, i) + 10, 2); return x; }
static int16_t  pb_disc0(VHost *v, int i) { int16_t x; memcpy(&x, PB(v, i) + 12, 2); return x; }
static int16_t  pb_disc1(VHost *v, int i) { int16_t x; memcpy(&x, PB(v, i) + 14, 2); return x; }
static float p_freq(VHost *v)   { float f; memcpy(&f, v->params + (v->type == T_AT9 ? 72 : 56), 4); return f; }
static float p_scalar(VHost *v) { float f; memcpy(&f, v->params + (v->type == T_AT9 ? 76 : 60), 4); return f; }
static int   p_chans(VHost *v)  { int c = (int8_t)v->params[v->type == T_AT9 ? 88 : 76]; return c == 1 ? 1 : 2; }
static int   p_adpcm(VHost *v)  { return v->type != T_AT9 && v->params[79] != 0; }
static int32_t p_start_bytes(VHost *v) { int32_t x; if (v->type == T_AT9) return 0; memcpy(&x, v->params + 72, 4); return x; }
static int   p_start_buf(VHost *v) { return v->type == T_AT9 ? 0 : (int8_t)v->params[81]; }
static uint32_t p_at9cfg(VHost *v) { uint32_t x; memcpy(&x, v->params + 92, 4); return x; }

static int buf_ok(VHost *v, int i) {
    return i >= 0 && i < 4 && pb_buf(v, i) && pb_bytes(v, i) > 0;
}

static uint32_t mod_id(VHost *v) { return v->type == T_AT9 ? 0x5CAA : 0x5CE6; }

/* Fin del buffer actual: bucle, siguiente buffer o fin de datos (con callbacks).
 * Devuelve 0 si ya no hay nada que reproducir. */
static int advance_buffer(VHost *v) {
    int prev = v->cur_buf;
    v->byte_pos = 0;
    v->stage_n = 0;
    v->loop_cnt++;
    int16_t loops = pb_loop(v, prev);
    if (loops != -1 && v->loop_cnt > loops) {
        v->cur_buf = pb_next(v, prev);
        v->loop_cnt = 0;
        if (!buf_ok(v, v->cur_buf)) {
            v->cur_buf = -1;
            call_guest(v->mod_cb[0], v->handle, mod_id(v), 0 /* END_OF_DATA */, 0, 0, v->mod_ud[0]);
            return 0;
        }
        call_guest(v->mod_cb[0], v->handle, mod_id(v), 1 /* SWAPPED_BUFFER */, (uint32_t)prev,
                   pb_buf(v, v->cur_buf), v->mod_ud[0]);
    } else {
        call_guest(v->mod_cb[0], v->handle, mod_id(v), v->type == T_AT9 ? 4u : 2u /* LOOPED_BUFFER */,
                   (uint32_t)v->loop_cnt, pb_buf(v, v->cur_buf), v->mod_ud[0]);
    }
    return buf_ok(v, v->cur_buf);
}

/* Decodifica un trozo del buffer actual a la cola. 0 = no hay más datos. */
static int decode_more(VHost *v) {
    if (!buf_ok(v, v->cur_buf)) return 0;
    if (v->byte_pos >= pb_bytes(v, v->cur_buf) && !advance_buffer(v)) return 0;
    int i = v->cur_buf, ch = p_chans(v);
    int32_t left = pb_bytes(v, i) - v->byte_pos;
    const uint8_t *src = (const uint8_t *)vita_mem_ptr(pb_buf(v, i) + (uint32_t)v->byte_pos, (uint32_t)left);
    if (!src) return 0;
    if (v->type == T_AT9) {
        uint32_t cfg = p_at9cfg(v);
        if (!v->at9 || cfg != v->at9_cfg) {
            if (v->at9) Atrac9ReleaseHandle(v->at9);
            v->at9 = Atrac9GetHandle();
            v->at9_cfg = cfg;
            if (!v->at9 || Atrac9InitDecoder(v->at9, (unsigned char *)&cfg) != 0 ||
                Atrac9GetCodecInfo(v->at9, &v->at9_info) != 0 || v->at9_info.superframeSize <= 0 ||
                v->at9_info.superframeSize > (int)sizeof(v->stage)) {
                if (v->at9) Atrac9ReleaseHandle(v->at9);
                v->at9 = NULL;
                return 0;
            }
        }
        int sfs = v->at9_info.superframeSize;
        const uint8_t *sf = src;
        int32_t take = left < sfs - (int32_t)v->stage_n ? left : sfs - (int32_t)v->stage_n;
        int first = v->byte_pos == 0 && v->stage_n == 0;
        if (left < sfs || v->stage_n) {                 /* superframe entre dos buffers */
            memcpy(v->stage + v->stage_n, src, (size_t)take);
            v->stage_n += (uint32_t)take;
            v->byte_pos += take;
            v->bytes_keyon += take; v->bytes_total += take;
            if (v->stage_n < (uint32_t)sfs) return 1;   /* falta el resto: siguiente buffer */
            sf = v->stage;
            v->stage_n = 0;
        } else {
            v->byte_pos += sfs;
            v->bytes_keyon += sfs; v->bytes_total += sfs;
        }
        int fs = v->at9_info.frameSamples, nch = v->at9_info.channels;
        int total = fs * v->at9_info.framesInSuperframe;
        int16_t pcm[2 * 4096];
        if (total > 4096 || nch > 2 || nch < 1) return 0;
        const uint8_t *p = sf;
        for (int f = 0; f < v->at9_info.framesInSuperframe; f++) {
            int used = 0, err = Atrac9Decode(v->at9, p, pcm + f * fs * nch, &used);
            if (trace_on()) {
                static int c;
                if (c++ < 6) { printf("[ngs] at9 decode err=%d usados=%d sf=%d fs=%d x%d can=%d pcm0=%d %d\n", err, used, sfs, fs,
                                      v->at9_info.framesInSuperframe, nch, pcm[f * fs * nch], pcm[f * fs * nch + 1]); fflush(stdout); }
            }
            if (err != 0) return 0;
            p += used;
        }
        int s0 = 0, s1 = total;
        if (first && pb_disc0(v, i) > 0) s0 = pb_disc0(v, i) < total ? pb_disc0(v, i) : total;
        if (v->byte_pos >= pb_bytes(v, i) && pb_disc1(v, i) > 0) s1 = total - pb_disc1(v, i) > s0 ? total - pb_disc1(v, i) : s0;
        for (int s = s0; s < s1; s++) {
            float l = pcm[s * nch] / 32768.0f, r = nch > 1 ? pcm[s * nch + 1] / 32768.0f : l;
            q_push(v, l, r);
        }
        return 1;
    }
    if (p_adpcm(v)) {
        int32_t fb = 16 * ch;                           /* un marco por canal, intercalados */
        int32_t n = left / fb;
        if (n <= 0) { v->byte_pos = pb_bytes(v, i); return 1; }
        if (n > 8) n = 8;
        for (int32_t f = 0; f < n; f++) {
            int16_t s[2][28];
            for (int c = 0; c < ch; c++) adpcm_frame(src + f * fb + 16 * c, v->hist[c], s[c]);
            for (int j = 0; j < 28; j++) q_push(v, s[0][j] / 32768.0f, (ch > 1 ? s[1][j] : s[0][j]) / 32768.0f);
        }
        v->byte_pos += n * fb;
        v->bytes_keyon += n * fb; v->bytes_total += n * fb;
        return 1;
    }
    int32_t fb = 2 * ch;
    int32_t n = left / fb;
    if (n <= 0) { v->byte_pos = pb_bytes(v, i); return 1; }
    if (n > 1024) n = 1024;
    const int16_t *s = (const int16_t *)src;
    for (int32_t k = 0; k < n; k++)
        q_push(v, s[k * ch] / 32768.0f, s[k * ch + (ch > 1)] / 32768.0f);
    v->byte_pos += n * fb;
    v->bytes_keyon += n * fb; v->bytes_total += n * fb;
    return 1;
}

/* --- envolvente (0x5CE3) ------------------------------------------------------ */
/* SceNgsEnvelopeParams tras el descriptor: 4 puntos {ms hasta el siguiente, amplitud,
 * curva}, ms de release, nº de puntos, inicio y fin de bucle (-1: mantener el último
 * punto). Curvas: se interpola en lineal (las del juego son 0 y 1). */
#define ENV_ID 0x5CE3u
#define REV_ID 0x5CE7u
static int env_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("MLB15_NGS_ENVOLVENTE"); on = e ? atoi(e) != 0 : 1; }
    return on;
}
static const uint8_t *mod_params(VHost *v, uint32_t id) {
    for (int m = 1; m < 8; m++) {
        uint32_t mid; memcpy(&mid, v->mparams[m], 4);
        if ((mid & 0xFFFF) == id && !(v->bypass >> m & 1)) return v->mparams[m];
    }
    return NULL;
}
typedef struct { uint32_t ms[4]; float amp[4]; uint32_t release, npoints; int32_t loop_start, loop_end; } EnvP;
static int env_params(VHost *v, EnvP *e) {
    const uint8_t *p = env_on() ? mod_params(v, ENV_ID) : NULL;
    if (!p) return 0;
    for (int i = 0; i < 4; i++) { memcpy(&e->ms[i], p + 8 + 12 * i, 4); memcpy(&e->amp[i], p + 12 + 12 * i, 4); }
    memcpy(&e->release, p + 56, 4); memcpy(&e->npoints, p + 60, 4);
    memcpy(&e->loop_start, p + 64, 4); memcpy(&e->loop_end, p + 68, 4);
    if (e->npoints < 1 || e->npoints > 4) return 0;
    for (int i = 0; i < 4; i++) if (!(e->amp[i] >= 0.0f && e->amp[i] <= 4.0f)) e->amp[i] = 1.0f;
    return 1;
}
static void env_reset(VHost *v) {
    EnvP e;
    v->env_seg = 0; v->env_ms = 0; v->env_rel = 0; v->env_rel_ms = 0;
    v->env_amp = env_params(v, &e) ? e.amp[0] : 1.0f;
}
/* Avanza dt ms y devuelve la amplitud; *fin = 1 si el release terminó. */
static float env_step(VHost *v, double dt, int *fin) {
    EnvP e;
    *fin = 0;
    if (!env_params(v, &e)) return 1.0f;
    if (v->env_rel) {
        v->env_rel_ms += dt;
        if (!e.release || v->env_rel_ms >= e.release) { *fin = 1; return 0.0f; }
        return v->env_rel_from * (float)(1.0 - v->env_rel_ms / e.release);
    }
    int last = (int)e.npoints - 1;
    v->env_ms += dt;
    for (int guard = 0; guard < 16 && v->env_seg < last && v->env_ms >= e.ms[v->env_seg]; guard++) {
        v->env_ms -= e.ms[v->env_seg];
        v->env_seg++;
        if (e.loop_end >= 0 && e.loop_end > e.loop_start && v->env_seg > e.loop_end && e.loop_start >= 0 && e.loop_start <= last)
            v->env_seg = e.loop_start;
    }
    if (v->env_seg >= last) return e.amp[last];
    float a = e.amp[v->env_seg], b = e.amp[v->env_seg + 1];
    double d = e.ms[v->env_seg] ? v->env_ms / e.ms[v->env_seg] : 1.0;
    return a + (b - a) * (float)(d > 1.0 ? 1.0 : d);
}
/* Aplica la envolvente al bloque (rampa lineal dentro del bloque). Devuelve 1 si la voz
 * terminó (release completo). */
static int env_apply(VHost *v) {
    if (!env_on()) return 0;
    int fin = 0;
    float a0 = v->env_amp, a1 = env_step(v, g_gran * 1000.0 / g_rate, &fin);
    if (a0 == 1.0f && a1 == 1.0f) return fin;
    for (uint32_t k = 0; k < g_gran; k++) {
        float g = a0 + (a1 - a0) * (float)(k + 1) / (float)g_gran;
        v->out[2 * k] *= g; v->out[2 * k + 1] *= g;
    }
    v->env_amp = a1;
    return fin;
}
/* Key-off: empieza el release desde la amplitud actual. 0 si no hay envolvente (cortar ya). */
static int env_key_off(VHost *v) {
    EnvP e;
    if (!env_on() || !env_params(v, &e) || !e.release) return 0;
    if (!v->env_rel) { v->env_rel = 1; v->env_rel_ms = 0; v->env_rel_from = v->env_amp; }
    return 1;
}

/* --- reverb (0x5CE7) ------------------------------------------------------------ */
/* Freeverb reducido: pre-retardo, 4 comb con amortiguación por canal y 2 all-pass.
 * Parámetros I3DL2 tras el descriptor: room (mB), roomHF (mB), decay (s), decayHF,
 * reflections (mB), reflections delay (s), reverb (mB), reverb delay (s), difusión,
 * densidad, HF reference... Sale solo el efecto. */
typedef struct Reverb {
    float pre[2][9600];                     /* hasta 0,2 s a 48 kHz */
    float comb[2][4][4096]; int comb_len[2][4], comb_pos[2][4]; float comb_lp[2][4];
    float ap[2][2][1024]; int ap_len[2][2], ap_pos[2][2];
    int pos;
} Reverb;
static int rev_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("MLB15_NGS_REVERB"); on = e ? atoi(e) != 0 : 1; }
    return on;
}
static float mb2lin(float mb) { if (!(mb > -10000.0f)) return 0.0f; if (mb > 2000.0f) mb = 2000.0f; return powf(10.0f, mb / 2000.0f); }
static void reverb_process(VHost *v) {
    const uint8_t *p = mod_params(v, REV_ID);
    float pr[11] = { -10000, 0, 1, 0.5f, -10000, 0.02f, -10000, 0.04f, 100, 100, 5000 };
    if (p) for (int i = 0; i < 11; i++) memcpy(&pr[i], p + 8 + 4 * i, 4);
    for (int i = 0; i < 11; i++) if (pr[i] != pr[i]) pr[i] = 0;   /* NaN */
    float room = mb2lin(pr[0]), wet = room * mb2lin(pr[6]), refl = room * mb2lin(pr[4]);
    float decay = pr[2] < 0.1f ? 0.1f : pr[2] > 20.0f ? 20.0f : pr[2];
    float hf = pr[3] < 0.1f ? 0.1f : pr[3] > 2.0f ? 2.0f : pr[3];
    float damp = hf < 1.0f ? (1.0f - hf) * 0.5f : 0.0f;          /* menos agudos si decayHF < 1 */
    if (!v->rev) {
        v->rev = (Reverb *)calloc(1, sizeof(Reverb));
        if (!v->rev) { memset(v->out, 0, g_gran * 2 * sizeof(float)); return; }
        static const int cl[4] = { 1557, 1617, 1491, 1422 }, al[2] = { 556, 441 };
        for (int c = 0; c < 2; c++) {
            for (int i = 0; i < 4; i++) v->rev->comb_len[c][i] = (cl[i] + 23 * c) * 48000 / 44100;
            for (int i = 0; i < 2; i++) v->rev->ap_len[c][i] = (al[i] + 23 * c) * 48000 / 44100;
        }
    }
    Reverb *r = v->rev;
    float g[2][4];
    for (int c = 0; c < 2; c++) for (int i = 0; i < 4; i++) g[c][i] = powf(10.0f, -3.0f * (float)r->comb_len[c][i] / ((float)g_rate * decay));
    int pre = (int)((pr[5] + pr[7]) * (float)g_rate); if (pre < 1) pre = 1; if (pre > 9599) pre = 9599;
    int rpre = (int)(pr[5] * (float)g_rate); if (rpre < 0) rpre = 0; if (rpre > 9599) rpre = 9599;
    for (uint32_t k = 0; k < g_gran; k++) {
        for (int c = 0; c < 2; c++) {
            float x = v->in[2 * k + c];
            r->pre[c][r->pos] = x;
            float xd = r->pre[c][(r->pos - pre + 9600) % 9600];
            float xr = r->pre[c][(r->pos - rpre + 9600) % 9600];
            float acc = 0;
            for (int i = 0; i < 4; i++) {
                int len = r->comb_len[c][i];
                float y = r->comb[c][i][r->comb_pos[c][i]];
                r->comb_lp[c][i] = y * (1.0f - damp) + r->comb_lp[c][i] * damp;
                r->comb[c][i][r->comb_pos[c][i]] = xd + r->comb_lp[c][i] * g[c][i];
                if (++r->comb_pos[c][i] >= len) r->comb_pos[c][i] = 0;
                acc += y;
            }
            for (int i = 0; i < 2; i++) {
                float b = r->ap[c][i][r->ap_pos[c][i]];
                float y = -acc + b;
                r->ap[c][i][r->ap_pos[c][i]] = acc + b * 0.5f;
                if (++r->ap_pos[c][i] >= r->ap_len[c][i]) r->ap_pos[c][i] = 0;
                acc = y;
            }
            v->out[2 * k + c] = acc * 0.25f * wet + xr * refl * 0.5f;
        }
        if (++r->pos >= 9600) r->pos = 0;
    }
}

static void voice_reset_play(VHost *v) {
    v->cur_buf = p_start_buf(v);
    v->byte_pos = p_start_bytes(v);
    v->loop_cnt = 0;
    v->samples_keyon = v->bytes_keyon = 0;
    memset(v->hist, 0, sizeof(v->hist));
    v->qn = 0; v->pos = 0; v->stage_n = 0;
    v->finish_pending = 0;
    env_reset(v);
}

/* G muestras estéreo a g_rate en v->out. Devuelve 1 si la voz ha terminado. */
static int gen_source(VHost *v) {
    float freq = p_freq(v), sc = p_scalar(v);
    if (!(freq > 0 && freq < 192000.0f)) freq = 48000.0f;
    if (!(sc > 0 && sc < 10.0f)) sc = 1.0f;
    double step = (double)freq * sc / g_rate;
    int ended = 0, guard = 0;
    while (!ended && v->pos + step * g_gran + 2 >= v->qn && guard++ < 256)
        if (!decode_more(v)) ended = 1;
    uint32_t k = 0;
    for (; k < g_gran; k++) {
        uint32_t i0 = (uint32_t)v->pos;
        if (i0 + 1 >= v->qn) break;
        float t = (float)(v->pos - i0);
        v->out[2 * k] = v->q[2 * i0] + (v->q[2 * i0 + 2] - v->q[2 * i0]) * t;
        v->out[2 * k + 1] = v->q[2 * i0 + 1] + (v->q[2 * i0 + 3] - v->q[2 * i0 + 1]) * t;
        v->pos += step;
    }
    for (; k < g_gran; k++) v->out[2 * k] = v->out[2 * k + 1] = 0;
    v->samples_keyon += (int32_t)g_gran; v->samples_total += (int32_t)g_gran;
    /* descartar lo ya consumido de la cola */
    uint32_t drop = (uint32_t)v->pos;
    if (drop > v->qn) drop = v->qn;
    if (drop) {
        memmove(v->q, v->q + 2 * drop, (v->qn - drop) * 2 * sizeof(float));
        v->qn -= drop;
        v->pos -= drop;
    }
    return ended && v->qn < 2;
}

static int is_source(const VHost *v) { return v->type == T_SCREAM || v->type == T_AT9; }

static void deliver(int slot) {
    VHost *s = g_vh[slot];
    for (int p = 0; p < g_npt; p++) {
        PHost *ph = &g_pt[p];
        if (ph->src != slot || ph->dst < 0 || ph->dst >= g_nvh) continue;
        VHost *d = g_vh[ph->dst];
        float m00 = ph->vol[0][0], m01 = ph->vol[0][1], m10 = ph->vol[1][0], m11 = ph->vol[1][1];
        if (s->channels == 1) m10 = m11 = 0;
        if (d->channels == 1) m01 = m11 = 0;
        if (m00 == 0 && m01 == 0 && m10 == 0 && m11 == 0) continue;
        for (uint32_t k = 0; k < g_gran; k++) {
            float l = s->out[2 * k], r = s->out[2 * k + 1];
            d->in[2 * k] += l * m00 + r * m10;
            d->in[2 * k + 1] += l * m01 + r * m11;
        }
    }
}

static void mix_update(uint32_t sys) {
    for (int i = 0; i < g_nvh; i++) {
        VHost *v = g_vh[i];
        memset(v->in, 0, g_gran * 2 * sizeof(float));
        v->done = 0;
    }
    /* 1) fuentes */
    for (int i = 0; i < g_nvh; i++) {
        VHost *v = g_vh[i];
        Obj *o = obj_at(v->handle, K_VOICE);
        if (!o || !is_source(v)) continue;
        v->done = 1;
        if (o->state != ST_ACTIVE && o->state != ST_KEY_OFF) continue;
        int fin = v->finish_pending || (o->state == ST_KEY_OFF && !v->env_rel);
        if (!fin && v->params_id) {
            fin = gen_source(v);
            if (env_apply(v)) fin = 1;   /* release terminado */
        } else memset(v->out, 0, g_gran * 2 * sizeof(float));
        if (!fin) deliver(i);
        if (fin) {
            if (trace_on()) {
                static int c;
                if (c++ < 40) { printf("[ngs] fin voz=0x%X tipo=%d buf=%d pos=%d muestras=%d at9=%p\n", v->handle, v->type, v->cur_buf, v->byte_pos, v->samples_keyon, v->at9); fflush(stdout); }
            }
            o->state = ST_AVAILABLE;
            v->finish_pending = 0;
            call_guest(v->fin_cb, v->handle, mod_id(v), 0, 0, 0, v->fin_ud);
        }
    }
    /* 2) buses en orden de dependencias: un bus va cuando todo lo que le llega está hecho */
    for (int pass = 0; pass < 8; pass++) {
        int any = 0;
        for (int i = 0; i < g_nvh; i++) {
            VHost *v = g_vh[i];
            if (v->done) continue;
            int ready = 1;
            for (int p = 0; p < g_npt && ready; p++)
                if (g_pt[p].dst == i && g_pt[p].src >= 0 && g_pt[p].src < g_nvh && !g_vh[g_pt[p].src]->done) ready = 0;
            if (!ready && pass < 7) continue;
            if (v->type == T_REVERB && rev_on()) reverb_process(v);
            else memcpy(v->out, v->in, g_gran * 2 * sizeof(float));
            if (v->type == T_MASTER)
                for (uint32_t k = 0; k < 2 * g_gran; k++) {
                    float x = v->in[k] * 32768.0f;
                    v->out16[k] = (int16_t)(x < -32768.0f ? -32768 : x > 32767.0f ? 32767 : x);
                }
            v->done = 1;
            any = 1;
            deliver(i);
        }
        if (!any) break;
    }
    if (trace_on()) {   /* cada ~5 s: nivel de cada voz que suena y de los buses */
        static int n;
        if (++n % 470 == 0) {
            printf("[ngs] niveles:");
            for (int i = 0; i < g_nvh; i++) {
                VHost *v = g_vh[i];
                Obj *o = obj_at(v->handle, K_VOICE);
                float mx = 0, mi = 0;
                for (uint32_t k = 0; k < 2 * g_gran; k++) {
                    if (fabsf(v->out[k]) > mx) mx = fabsf(v->out[k]);
                    if (fabsf(v->in[k]) > mi) mi = fabsf(v->in[k]);
                }
                if (mx > 0 || mi > 0 || !is_source(v)) printf(" %d(t%d s%u)=%.3f/%.3f", i, v->type, o ? o->state : 99, mi, mx);
            }
            printf("\n");
            fflush(stdout);
        }
    }
    (void)sys;
}

void vita_hle_sceNgsSystemUpdate(void) {
    uint32_t sys = r0;
    lock();
    Obj *s = obj_at(sys, K_SYS);
    if (s) {
        g_updates++;
        if (synth_on()) mix_update(sys);
        for (int i = 0; i < g_nvh; i++) {
            Obj *v = obj_at(g_vh[i]->handle, K_VOICE);
            if (v) v->update_passed++;
        }
    }
    unlock();
    r0 = NGS_OK;
}

void vita_hle_sceNgsSystemRelease(void) {
    Obj *s = obj_at(r0, K_SYS);
    if (s) s->kind = 0;
    r0 = NGS_OK;
}

/* --- rack ----------------------------------------------------------------- */

void vita_hle_sceNgsRackGetRequiredMemorySize(void) {
    /* r0 = sistema, r1 = SceNgsRackDescription*, r2 = SceSize* */
    uint32_t desc = r1, out = r2;
    uint32_t voices = read_u32(desc + 4, 1);
    if (voices == 0 || voices > 256) voices = 1;
    uint32_t size = 0x1000 + voices * 0x400;
    if (vita_mem_ptr(out, 4)) vita_write32(out, size);
    r0 = NGS_OK;
}

void vita_hle_sceNgsRackInit(void) {
    /* r0 = sistema, r1 = SceNgsBufferInfo*, r2 = SceNgsRackDescription {def, voces,
     * canales por voz, patches por entrada, patches por salida}, r3 = SceNgsHRack* */
    uint32_t system = r0, buffer_info = r1, desc = r2, out = r3;
    if (!obj_at(system, K_SYS)) { r0 = NGS_ERROR; return; }
    if (!vita_mem_ptr(desc, 0x18)) { r0 = NGS_ERROR; return; }

    uint32_t voices  = read_u32(desc + 0x04, 1);
    uint32_t chans   = read_u32(desc + 0x08, 2);
    uint32_t patches = read_u32(desc + 0x10, 1);
    if (voices == 0 || voices > 256) { r0 = NGS_ERROR; return; }

    uint32_t h = obj_new(K_RACK, "ngs rack");
    Obj *rack = h ? obj_at(h, K_RACK) : NULL;
    if (!rack) { r0 = NGS_ERROR; return; }
    rack->parent = system;
    rack->voice_count = voices;
    rack->patches_per_output = patches;
    rack->num_modules = 4;
    rack->num_inputs = 1;
    rack->num_outputs = 1;
    rack->type = (uint32_t)def_type(read_u32(desc, 0));
    rack->channels = chans == 1 ? 1 : 2;

    if (!vita_mem_ptr(out, 4)) { r0 = NGS_ERROR; return; }
    vita_write32(out, h);
    ONCE("sceNgsRackInit");
    printf("[ngs]   rack=0x%X buffer=0x%X tipo=%u voces=%u canales=%u patches/salida=%u\n",
           h, read_u32(buffer_info, 0), rack->type, voices, chans, patches);
    fflush(stdout);
    r0 = NGS_OK;
}

void vita_hle_sceNgsRackGetVoiceHandle(void) {
    /* r0 = rack, r1 = indice, r2 = SceNgsHVoice* */
    uint32_t rack_h = r0, index = r1, out = r2;
    Obj *rack = obj_at(rack_h, K_RACK);
    if (!rack) {
        printf("[ngs] RackGetVoiceHandle: rack 0x%X no valido (indice %u)\n", rack_h, index);
        fflush(stdout);
        r0 = NGS_ERROR;
        return;
    }
    if (index >= rack->voice_count || !vita_mem_ptr(out, 4)) {
        printf("[ngs] RackGetVoiceHandle: indice %u fuera de rango (voces=%u) o salida 0x%X invalida\n",
               index, rack->voice_count, out);
        fflush(stdout);
        r0 = NGS_ERROR;
        return;
    }
    if (trace_on()) {
        printf("[ngs] RackGetVoiceHandle(rack=0x%X, indice=%u, out=0x%X)\n", rack_h, index, out);
        fflush(stdout);
    }
    lock();
    for (int i = 0; i < g_voice_map; i++) {
        if (g_voice_rack[i] == rack_h && g_voice_idx[i] == index) {
            vita_write32(out, g_voice_handle[i]);
            unlock();
            r0 = NGS_OK;
            return;
        }
    }
    uint32_t h = obj_new(K_VOICE, "ngs voice");
    Obj *v = h ? obj_at(h, K_VOICE) : NULL;
    VHost *vh = v && g_nvh < 1024 ? (VHost *)calloc(1, sizeof(VHost)) : NULL;
    if (!v || !vh) { unlock(); r0 = NGS_ERROR; return; }
    v->parent = rack_h;
    v->index = index;
    v->state = ST_AVAILABLE;
    v->num_modules = rack->num_modules;
    v->num_inputs = rack->num_inputs;
    v->num_outputs = rack->num_outputs;
    v->patches_per_output = rack->patches_per_output;
    v->type = rack->type;
    v->channels = rack->channels;
    v->slot = (uint32_t)g_nvh;
    vh->handle = h;
    vh->type = (int)rack->type;
    vh->channels = (int)rack->channels;
    vh->cur_buf = -1;
    g_vh[g_nvh++] = vh;
    if (g_voice_map < 1024) {
        g_voice_rack[g_voice_map] = rack_h;
        g_voice_idx[g_voice_map] = index;
        g_voice_handle[g_voice_map] = h;
        g_voice_map++;
    }
    vita_write32(out, h);
    unlock();
    r0 = NGS_OK;
}

void vita_hle_sceNgsRackRelease(void) {
    Obj *rack = obj_at(r0, K_RACK);
    if (rack) rack->kind = 0;
    r0 = NGS_OK;
}

/* --- voces ---------------------------------------------------------------- */

void vita_hle_sceNgsVoiceInit(void) {
    /* r0 = voz, r1 = preset, r2 = flags */
    TRN("VoiceInit", 40);
    lock();
    Obj *v = obj_at(r0, K_VOICE);
    VHost *vh = vh_of(r0);
    if (v) { v->state = ST_AVAILABLE; v->update_passed = 0; }
    if (vh) { vh->qn = 0; vh->pos = 0; vh->finish_pending = 0; }
    unlock();
    ONCE("sceNgsVoiceInit");
    r0 = v ? NGS_OK : NGS_ERROR;
}

void vita_hle_sceNgsVoicePlay(void) {
    TRN("VoicePlay", 40);
    lock();
    Obj *v = obj_at(r0, K_VOICE);
    VHost *vh = vh_of(r0);
    if (v) {
        if (vh && v->state != ST_ACTIVE) voice_reset_play(vh);
        v->state = ST_ACTIVE;
        if (vh && trace_on() && is_source(vh)) {
            static int c;
            if (c++ < 40) {
                printf("[ngs] Play voz=0x%X tipo=%d id=0x%08X %.0f Hz x%.2f can=%d adpcm=%d cfg=%08X ini=%d:", r0, vh->type,
                       vh->params_id, p_freq(vh), p_scalar(vh), p_chans(vh), p_adpcm(vh), p_at9cfg(vh), vh->cur_buf);
                for (int i = 0; i < 4; i++) printf(" [%08X %d bucle=%d sig=%d]", pb_buf(vh, i), pb_bytes(vh, i), pb_loop(vh, i), pb_next(vh, i));
                printf("\n");
                fflush(stdout);
            }
        }
    }
    unlock();
    r0 = v ? NGS_OK : NGS_ERROR;
}
void vita_hle_sceNgsVoiceKill(void) {
    TRN("VoiceKill", 20);
    lock();
    Obj *v = obj_at(r0, K_VOICE);
    VHost *vh = vh_of(r0);
    if (v) { v->state = ST_AVAILABLE; v->update_passed = 0; }
    if (vh) { vh->qn = 0; vh->pos = 0; vh->finish_pending = 0; }
    unlock();
    r0 = v ? NGS_OK : NGS_ERROR;
}
void vita_hle_sceNgsVoicePause(void) {
    Obj *v = obj_at(r0, K_VOICE);
    if (v) v->state = ST_PAUSED;
    r0 = v ? NGS_OK : NGS_ERROR;
}
void vita_hle_sceNgsVoiceResume(void) {
    Obj *v = obj_at(r0, K_VOICE);
    if (v) v->state = ST_ACTIVE;
    r0 = v ? NGS_OK : NGS_ERROR;
}
void vita_hle_sceNgsVoiceKeyOff(void) {
    /* Con envolvente: release en su tiempo y luego fin (callback). Sin ella: fin en el
     * próximo Update. */
    TRN("VoiceKeyOff", 20);
    lock();
    Obj *v = obj_at(r0, K_VOICE);
    VHost *vh = vh_of(r0);
    if (v && v->state != ST_AVAILABLE) {
        v->state = ST_KEY_OFF;
        if (vh && is_source(vh)) env_key_off(vh);
    }
    unlock();
    r0 = v ? NGS_OK : NGS_ERROR;
}

void vita_hle_sceNgsVoiceSetFinishedCallback(void) {
    /* r0 = voz, r1 = callback, r2 = datos */
    TRN("VoiceSetFinishedCallback", 20);
    VHost *vh = vh_of(r0);
    if (vh) { vh->fin_cb = r1; vh->fin_ud = r2; }
    r0 = NGS_OK;
}
void vita_hle_sceNgsVoiceSetModuleCallback(void) {
    /* r0 = voz, r1 = módulo, r2 = callback, r3 = datos */
    TRN("VoiceSetModuleCallback", 20);
    VHost *vh = vh_of(r0);
    if (vh && r1 < 8) { vh->mod_cb[r1] = r2; vh->mod_ud[r1] = r3; }
    r0 = NGS_OK;
}
void vita_hle_sceNgsVoiceBypassModule(void) {
    /* r0 = voz, r1 = módulo, r2 = 1 bypass / 0 activo */
    TRN("VoiceBypassModule", 20);
    lock();
    VHost *vh = vh_of(r0);
    if (vh && r1 < 8) { if (r2) vh->bypass |= 1u << r1; else vh->bypass &= ~(1u << r1); }
    unlock();
    r0 = NGS_OK;
}

/* MLB15_NGS_MODULOS=1: cada combinación (tipo de voz, módulo, id, tamaño) distinta, con su
 * contenido la primera vez (para identificar envolvente, EQ, reverb...). */
static void mod_trace(VHost *vh, uint32_t module, uint32_t addr) {
    static int on = -1;
    static uint32_t seen[512];
    static int nseen;
    if (on < 0) on = getenv("MLB15_NGS_MODULOS") ? 1 : 0;
    if (!on || !vh) return;
    uint32_t id = read_u32(addr, 0), sz = read_u32(addr + 4, 0);
    uint32_t key = ((uint32_t)vh->type << 28) ^ (module << 24) ^ (id & 0xFFFF) ^ (sz << 16);
    if ((id & 0xFFFF) == 0x5CE7 || (id & 0xFFFF) == 0x5CE3) {   /* reverb y envolvente: también cada contenido distinto */
        for (uint32_t o = 8; o + 4 <= sz && o < 80; o += 4) key = key * 16777619u ^ read_u32(addr + o, 0);
    }
    for (int i = 0; i < nseen; i++) if (seen[i] == key) return;
    if (nseen < 512) seen[nseen++] = key;
    else return;
    printf("[ngsmod] tipo %d mod %u id 0x%08X tam %u:", vh->type, module, id, sz);
    for (uint32_t o = 8; o < sz && o < 8 + 4 * 40; o += 4) {
        uint32_t w = read_u32(addr + o, 0);
        float f; memcpy(&f, &w, 4);
        if (w && (w & 0x7F800000u) && (w & 0x7F800000u) != 0x7F800000u && fabsf(f) < 1e7f && fabsf(f) > 1e-7f) printf(" %g", f);
        else printf(" %08X", w);
    }
    printf("\n");
    fflush(stdout);
}

/* Params de otro módulo (envolvente, reverb...): se guardan tal cual con su descriptor. */
static void take_mparams(VHost *vh, uint32_t module, uint32_t addr) {
    uint32_t sz = read_u32(addr + 4, 0);
    if (module < 1 || module >= 8 || sz < 8 || sz > sizeof(vh->mparams[0])) return;
    const uint8_t *p = (const uint8_t *)vita_mem_ptr(addr, sz);
    if (p) memcpy(vh->mparams[module], p, sz);
}

/* Copia los params del módulo 0 (player o AT9, con su descriptor). */
static void take_params(VHost *vh, uint32_t addr) {
    uint32_t id = read_u32(addr, 0), sz = read_u32(addr + 4, 0);
    if ((id != PLAYER_ID && id != AT9_ID) || sz < 8 || sz > sizeof(vh->params)) return;
    const uint8_t *p = (const uint8_t *)vita_mem_ptr(addr, sz);
    if (!p) return;
    memcpy(vh->params, p, sz);
    vh->params_id = id;
}

void vita_hle_sceNgsVoiceSetParamsBlock(void) {
    /* r0 = voz, r1 = datos, r2 = tamaño, r3 = int* errores. Bloques de
     * {módulo, canal} + descriptor {id, tamaño} + params. */
    if (trace_on()) { static int c; if (c++ < 60) { printf("[ngs] SetParamsBlock(voz=0x%X datos=0x%X tam=%u) mod=0x%X can=%d id=0x%08X sz=%u\n", r0, r1, r2,
        read_u32(r1, 0), read_u32(r1 + 4, 0), read_u32(r1 + 8, 0), read_u32(r1 + 12, 0)); fflush(stdout); } }
    uint32_t voice = r0, data = r1, size = r2, errs = r3;
    lock();
    VHost *vh = vh_of(voice);
    for (uint32_t off = 0; vh && off + 16 <= size;) {
        uint32_t module = read_u32(data + off, 0xFFFFFFFF), sz = read_u32(data + off + 12, 0);
        if (sz < 8 || off + 8 + sz > size) break;
        mod_trace(vh, module, data + off + 8);
        if (module == 0) take_params(vh, data + off + 8);
        else take_mparams(vh, module, data + off + 8);
        off += 8 + sz;
    }
    if (vh && trace_on() && vh->type == T_AT9) {
        static int c;
        if (c++ < 40) {
            Obj *o = obj_at(voice, K_VOICE);
            printf("[ngs]   at9 params: estado=%u reproduciendo buf=%d pos=%d bucle=%d:", o ? o->state : 99, vh->cur_buf, vh->byte_pos, vh->loop_cnt);
            for (int i = 0; i < 4; i++) printf(" [%08X %d b%d s%d]", pb_buf(vh, i), pb_bytes(vh, i), pb_loop(vh, i), pb_next(vh, i));
            printf("\n");
        }
    }
    unlock();
    if (vita_mem_ptr(errs, 4)) vita_write32(errs, 0);
    r0 = NGS_OK;
}

void vita_hle_sceNgsVoiceGetParamsOutOfRange(void) {
    /* r0 = voz, r1 = modulo, r2 = char* mensaje */
    uint32_t out = r2;
    if (vita_mem_ptr(out, 1)) *(char *)vita_mem_ptr(out, 1) = 0;
    r0 = NGS_OK;
}

void vita_hle_sceNgsVoiceGetInfo(void) {
    /* r0 = voz, r1 = SceNgsVoiceInfo* (0x18) */
    Obj *v = obj_at(r0, K_VOICE);
    uint32_t out = r1;
    if (!v || !vita_mem_ptr(out, 0x18)) { r0 = NGS_ERROR; return; }
    vita_write32(out + 0x00, v->state);
    vita_write32(out + 0x04, v->num_modules);
    vita_write32(out + 0x08, v->num_inputs);
    vita_write32(out + 0x0C, v->num_outputs);
    vita_write32(out + 0x10, v->patches_per_output);
    vita_write32(out + 0x14, v->update_passed);
    r0 = NGS_OK;
}

void vita_hle_sceNgsVoiceGetStateData(void) {
    /* r0 = voz, r1 = modulo, r2 = memoria, r3 = tamaño. Master: módulo 1 = salida
     * (granularidad x 2 canales s16). Fuente: módulo 0 = SceNgsPlayerStates / AT9States. */
    TRN("VoiceGetStateData", 30);
    uint32_t voice = r0, module = r1, mem = r2, size = r3;
    uint8_t *dst = size ? (uint8_t *)vita_mem_ptr(mem, size) : NULL;
    if (trace_on()) {   /* cada (voz, módulo) distinto que el juego consulta */
        static uint32_t seen[64][2];
        static int ns;
        int k = 0;
        for (; k < ns; k++) if (seen[k][0] == voice && seen[k][1] == module) break;
        if (k == ns && ns < 64) {
            seen[ns][0] = voice; seen[ns][1] = module; ns++;
            VHost *t = vh_of(voice);
            printf("[ngs] GetStateData nuevo: voz=0x%X tipo=%d modulo=%u tam=%u\n", voice, t ? t->type : -1, module, size);
            fflush(stdout);
        }
    }
    if (dst) {
        lock();
        VHost *vh = vh_of(voice);
        memset(dst, 0, size);
        if (vh && vh->type == T_MASTER && module == 1) {
            uint32_t n = g_gran * 4 < size ? g_gran * 4 : size;
            memcpy(dst, vh->out16, n);
        } else if (vh && is_source(vh) && module == 0) {
            int32_t st[6] = { vh->byte_pos, vh->cur_buf, vh->samples_keyon, vh->bytes_keyon, vh->samples_total, vh->bytes_total };
            memcpy(dst, st, size < sizeof(st) ? size : sizeof(st));
        }
        unlock();
    }
    r0 = NGS_OK;
}

void vita_hle_sceNgsVoiceLockParams(void) {
    /* r0 = voz, r1 = modulo, r2 = params id, r3 = SceNgsBufferInfo* */
    TRN("VoiceLockParams", 40);
    uint32_t voice = r0, module = r1, info = r3;
    lock();
    VHost *vh = vh_of(voice);
    if (vita_mem_ptr(info, 8)) {
        uint32_t blk = 0;
        if (vh && module < 8) {
            if (!vh->lock_blk[module]) vh->lock_blk[module] = port_alloc(0x100, 0x10, "ngs params");
            blk = vh->lock_blk[module];
            if (blk && module == 0 && vh->params_id) {     /* el juego lee los params actuales */
                uint32_t psz; memcpy(&psz, vh->params + 4, 4);
                if (psz <= 0x100 && vita_mem_ptr(blk, psz)) memcpy(vita_mem_ptr(blk, psz), vh->params, psz);
            } else if (blk && module > 0) {                /* también los de los demás módulos */
                uint32_t psz; memcpy(&psz, vh->mparams[module] + 4, 4);
                if (psz >= 8 && psz <= sizeof(vh->mparams[0]) && vita_mem_ptr(blk, psz)) memcpy(vita_mem_ptr(blk, psz), vh->mparams[module], psz);
            }
        } else blk = port_alloc(0x100, 0x10, "ngs params");
        vita_write32(info + 0, blk);
        vita_write32(info + 4, 0x100);
    }
    unlock();
    r0 = NGS_OK;
}
void vita_hle_sceNgsVoiceUnlockParams(void) {
    /* r0 = voz, r1 = modulo: lo escrito en el bloque pasa a ser el params actual */
    TRN("VoiceUnlockParams", 40);
    lock();
    VHost *vh = vh_of(r0);
    if (vh && r1 < 8 && vh->lock_blk[r1]) mod_trace(vh, r1, vh->lock_blk[r1]);
    if (vh && r1 == 0 && vh->lock_blk[0]) take_params(vh, vh->lock_blk[0]);
    else if (vh && r1 < 8 && vh->lock_blk[r1]) take_mparams(vh, r1, vh->lock_blk[r1]);
    unlock();
    r0 = NGS_OK;
}

void vita_hle_sceNgsVoicePatchSetVolumesMatrix(void) {
    /* r0 = patch, r1 = float[2][2] [canal origen][canal destino] */
    TRN("VoicePatchSetVolumesMatrix", 20);
    Obj *p = obj_at(r0, K_PATCH);
    float *m = volume_matrix(r1);
    if (p && m && p->slot < (uint32_t)g_npt) {
        lock();
        memcpy(g_pt[p->slot].vol, m, sizeof(g_pt[p->slot].vol));
        if (trace_on()) {
            static int c;
            if (c++ < 5000) {
                PHost *q = &g_pt[p->slot];
                printf("[ngs] matriz u%u patch=0x%X %d(t%d)->%d(t%d) = %.3f %.3f %.3f %.3f\n", g_updates, r0, q->src, q->src >= 0 ? g_vh[q->src]->type : -1,
                       q->dst, q->dst >= 0 ? g_vh[q->dst]->type : -1, m[0], m[1], m[2], m[3]);
                fflush(stdout);
            }
        }
        unlock();
    }
    r0 = NGS_OK;
}

/* --- rutas ----------------------------------------------------------------- */

void vita_hle_sceNgsPatchCreateRouting(void) {
    /* r0 = SceNgsPatchSetupInfo {origen, salida, subíndice, destino, entrada}, r1 = SceNgsHPatch* */
    TRN("PatchCreateRouting", 30);
    uint32_t setup = r0, out = r1;
    if (!vita_mem_ptr(setup, 0x14) || !vita_mem_ptr(out, 4)) { r0 = NGS_ERROR; return; }
    uint32_t src = vita_read32(setup + 0x00);
    uint32_t dst = vita_read32(setup + 0x0C);
    Obj *so = obj_at(src, K_VOICE), *dobj = obj_at(dst, K_VOICE);
    if (!so || !dobj) { r0 = NGS_ERROR; return; }
    uint32_t h = obj_new(K_PATCH, "ngs patch");
    Obj *p = h ? obj_at(h, K_PATCH) : NULL;
    if (!p) { r0 = NGS_ERROR; return; }
    p->src_voice = src;
    p->dst_voice = dst;
    lock();
    int slot = -1;
    for (int i = 0; i < g_npt; i++) if (g_pt[i].src < 0) { slot = i; break; }
    if (slot < 0 && g_npt < (int)(sizeof(g_pt) / sizeof(g_pt[0]))) slot = g_npt++;
    if (slot >= 0) {
        g_pt[slot].handle = h;
        g_pt[slot].src = (int)so->slot;
        g_pt[slot].dst = (int)dobj->slot;
        g_pt[slot].vol[0][0] = 1; g_pt[slot].vol[0][1] = 0;   /* identidad hasta SetVolumesMatrix */
        g_pt[slot].vol[1][0] = 0; g_pt[slot].vol[1][1] = 1;
        p->slot = (uint32_t)slot;
    } else p->slot = 0xFFFFFFFF;
    unlock();
    vita_write32(out, h);
    ONCE("sceNgsPatchCreateRouting");
    r0 = NGS_OK;
}

void vita_hle_sceNgsPatchRemoveRouting(void) {
    Obj *p = obj_at(r0, K_PATCH);
    if (p) {
        lock();
        if (p->slot < (uint32_t)g_npt) { g_pt[p->slot].src = g_pt[p->slot].dst = -1; }
        unlock();
        p->kind = 0;
    }
    r0 = NGS_OK;
}

void vita_hle_sceNgsPatchGetInfo(void) {
    /* r0 = patch, r1 = SceNgsPatchRouteInfo*, r2 = SceNgsPatchSetupInfo* */
    uint32_t route = r1, setup_out = r2;
    Obj *p = obj_at(r0, K_PATCH);
    if (!p) { r0 = NGS_ERROR; return; }
    if (vita_mem_ptr(route, 0x18)) {
        vita_write32(route + 0x00, 2);        /* canales de salida  */
        vita_write32(route + 0x04, 2);        /* canales de entrada */
        float *m = volume_matrix(route + 0x08);
        if (m) {
            if (p->slot < (uint32_t)g_npt) memcpy(m, g_pt[p->slot].vol, 4 * sizeof(float));
            else { m[0] = 1.0f; m[1] = 0.0f; m[2] = 0.0f; m[3] = 1.0f; }
        }
    }
    if (vita_mem_ptr(setup_out, 0x14)) {
        vita_write32(setup_out + 0x00, p->src_voice);
        vita_write32(setup_out + 0x04, 0);
        vita_write32(setup_out + 0x08, -1);
        vita_write32(setup_out + 0x0C, p->dst_voice);
        vita_write32(setup_out + 0x10, 0);
    }
    r0 = NGS_OK;
}

void vita_hle_sceNgsAT9GetSectionDetails(void) {
    /* r0 = primera muestra, r1 = nº de muestras, r2 = config AT9, r3 = SceNgsAT9SkipBufferInfo*
     * {byte inicial, nº de bytes, s16 saltar al inicio, s16 saltar al final, es superpaquete}. */
    uint32_t start = r0, count = r1, cfg = r2, out = r3;
    if (vita_mem_ptr(out, 0x10)) memset(vita_mem_ptr(out, 0x10), 0, 0x10);
    void *h = Atrac9GetHandle();
    Atrac9CodecInfo ci;
    if (h && Atrac9InitDecoder(h, (unsigned char *)&cfg) == 0 && Atrac9GetCodecInfo(h, &ci) == 0 &&
        vita_mem_ptr(out, 0x10)) {
        uint32_t spsf = (uint32_t)(ci.frameSamples * ci.framesInSuperframe);
        uint32_t first = start / spsf, last = (start + count + spsf - 1) / spsf;
        vita_write32(out + 0, first * (uint32_t)ci.superframeSize);
        vita_write32(out + 4, (last - first) * (uint32_t)ci.superframeSize);
        vita_write16(out + 8, (uint16_t)(start - first * spsf));
        vita_write16(out + 10, (uint16_t)(last * spsf - (start + count)));
        vita_write32(out + 12, ci.framesInSuperframe > 1);
    }
    if (h) Atrac9ReleaseHandle(h);
    r0 = NGS_OK;
}
