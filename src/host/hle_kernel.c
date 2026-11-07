/* Funciones del kernel de la Vita implementadas sobre Windows (HLE) para MLB 15.
 *
 * Cada hilo guest corre en un hilo de Windows con su propio estado de CPU
 * (los registros son thread_local en el runtime). Los objetos del kernel
 * (semáforos, mutex, lightweight mutex, condiciones) se implementan con
 * CRITICAL_SECTION + CONDITION_VARIABLE.
 *
 * Convención: void(void); argumentos en r0-r3 y, del 5.º en adelante, en la pila
 * guest ([sp], [sp+4]...); resultado en r0. Los punteros son direcciones guest.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/dispatch.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/recomp_rt.h"

#include "port.h"

#define ERR_UNKNOWN_UID   0x80028001u
#define ERR_WAIT_TIMEOUT  0x80028005u
#define ERR_SEMA_ZERO     0x8002801Du
#define ERR_MUTEX_FAILED  0x80028045u
#define ERR_LWMUTEX_FAILED 0x80028164u
#define ERR_NO_MEMORY     0x80020190u

#define guest_str port_guest_str
static uint32_t arg_stack(int n) { return vita_read32(sp + 4u * (uint32_t)n); } /* 5.º arg = n 0 */

void port_watch_thread_start(void);   /* watchdog.c */

void port_guest_str(uint32_t addr, char *out, size_t max) {
    out[0] = 0;
    if (!addr) return;
    for (size_t i = 0; i + 1 < max; i++) {
        out[i] = (char)vita_read8(addr + (uint32_t)i);
        out[i + 1] = 0;
        if (!out[i]) break;
    }
}

static DWORD timeout_ms(uint32_t timeout_ptr) {
    if (!timeout_ptr) return INFINITE;
    uint32_t us = vita_read32(timeout_ptr);
    return (us + 999u) / 1000u;
}

/* ------------------------------------------------------------------------- */
/* Memoria del kernel: sceKernelAllocMemBlock                                 */
/* ------------------------------------------------------------------------- */

typedef struct { uint32_t base, size; int used; char name[32]; } MemBlock;
#define MAX_BLOCKS 16384   /* el juego crea ~370 programas de shader, cada uno con varios bloques */

/* Tres montones separados.
 *
 *   g_blocks   RAM principal que ve el juego (tipos 0xC2xxxxxx/0xC8xxxxxx):
 *              pools, tablas, pilas de datos.
 *   g_vblocks  VRAM (tipo 0x9408060): en la Vita es memoria dedicada de la GPU,
 *              no sale de la RAM principal. El juego la sondea aparte y se
 *              queda con lo que quepa; si comparte monton con la RAM, el sondeo
 *              de VRAM (hasta 256 MB) se come el MainPool y el RTdriver.
 *   g_kblocks  reserva del port para sus propios objetos (handles de GXM/Ngs,
 *              pilas de hilos, TLS, bloques de parametros...).
 *
 * Antes los tres eran uno solo y el juego, que llena la arena hasta el ultimo
 * byte con sus pools, dejaba al port sin sitio justo cuando el motor de audio
 * pedia las voces de su rack de 64: sceNgsRackGetVoiceHandle fallaba y el juego
 * se paraba en una asercion. */
static MemBlock g_blocks[MAX_BLOCKS];
static int g_nblocks;
static MemBlock g_vblocks[MAX_BLOCKS];
static int g_vnblocks;
static MemBlock g_kblocks[MAX_BLOCKS];
static int g_knblocks;
static CRITICAL_SECTION g_mem_cs;

/* Recomendable llamar tras inicializar los otros dos arrays (C es C, no hay
 * constructor, pero dejarlo explicito evita sorpresas). */

/* main_end = fin de RAM principal; vram_end = fin de VRAM; port_end = fin de
 * la reserva del port (y del espacio mapeado). */
void port_memblock_init(uint32_t main_base, uint32_t main_end,
                        uint32_t vram_end, uint32_t port_end) {
    InitializeCriticalSection(&g_mem_cs);

    g_blocks[0].base = main_base;
    g_blocks[0].size = main_end - main_base;
    g_blocks[0].used = 0;
    g_nblocks = 1;

    g_vblocks[0].base = main_end;
    g_vblocks[0].size = vram_end - main_end;
    g_vblocks[0].used = 0;
    g_vnblocks = 1;

    g_kblocks[0].base = vram_end;
    g_kblocks[0].size = port_end - vram_end;
    g_kblocks[0].used = 0;
    g_knblocks = 1;
}

/* Primer hueco libre que quepa, con alineación. Devuelve índice o -1.
 * Generico sobre una lista para servir a los dos montones. */
static int list_alloc(MemBlock *list, int *count, uint32_t size, uint32_t align,
                      const char *name) {
    int found = -1;
    EnterCriticalSection(&g_mem_cs);
    for (int i = 0; i < *count; i++) {
        MemBlock *b = &list[i];
        if (b->used) continue;
        uint32_t start = (b->base + align - 1) & ~(align - 1);
        uint32_t pad = start - b->base;
        if (b->size < pad + size || *count + 2 >= MAX_BLOCKS) continue;
        /* `j` es la entrada del bloque reservado. Con relleno de alineación
         * pasa a ser i+1; usar `i` para el trozo de detrás y para el resultado
         * corrompía la tabla y devolvía la dirección del relleno: bloques
         * solapados (EngineUncachedMainRAM, 51 MB, pisado por los siguientes). */
        int j = i;
        if (pad) { /* trozo libre delante */
            memmove(&list[i + 1], &list[i], sizeof(MemBlock) * (size_t)(*count - i));
            (*count)++;
            list[i].size = pad;
            list[i].used = 0;
            list[i].name[0] = 0;
            j = i + 1;
            list[j].base = start;
            list[j].size -= pad;
        }
        b = &list[j];
        if (b->size > size) { /* trozo libre detrás */
            memmove(&list[j + 2], &list[j + 1], sizeof(MemBlock) * (size_t)(*count - j - 1));
            (*count)++;
            list[j + 1].base = start + size;
            list[j + 1].size = b->size - size;
            list[j + 1].used = 0;
            list[j + 1].name[0] = 0;
            b->size = size;
        }
        b->used = 1;
        strncpy(b->name, name ? name : "", sizeof(b->name) - 1);
        b->name[sizeof(b->name) - 1] = 0;
        memset(vita_mem_ptr(b->base, b->size), 0, b->size);
        found = j;
        break;
    }
    LeaveCriticalSection(&g_mem_cs);
    return found;
}

/* Objetos del port: salen de la reserva, nunca de la RAM que ve el juego. */
uint32_t port_alloc(uint32_t size, uint32_t align, const char *name) {
    int i = list_alloc(g_kblocks, &g_knblocks, (size + 0xFFFu) & ~0xFFFu,
                       align < 0x1000 ? 0x1000 : align, name);
    if (i < 0) {
        /* Reserva del port agotada: decir quién la ocupa (por nombre). */
        static int told;
        if (!told++) {
            printf("[kernel] port_alloc('%s', %u KB) SIN MEMORIA; ocupación de la reserva:\n",
                   name ? name : "?", size >> 10);
            for (int a = 0; a < g_knblocks; a++) {
                if (!g_kblocks[a].used) continue;
                uint32_t tot = 0; int n = 0, seen = 0;
                for (int b = 0; b < a; b++)
                    if (g_kblocks[b].used && !strcmp(g_kblocks[b].name, g_kblocks[a].name)) seen = 1;
                if (seen) continue;
                for (int b = a; b < g_knblocks; b++)
                    if (g_kblocks[b].used && !strcmp(g_kblocks[b].name, g_kblocks[a].name)) { tot += g_kblocks[b].size; n++; }
                printf("    %-32s %5d bloques %8u KB\n", g_kblocks[a].name, n, tot >> 10);
            }
            fflush(stdout);
        }
        return 0;
    }
    return g_kblocks[i].base;
}

/* RAM del juego: lo que pide sceKernelAllocMemBlock. */
static uint32_t game_alloc(uint32_t size, uint32_t align, const char *name) {
    int i = list_alloc(g_blocks, &g_nblocks, (size + 0xFFFu) & ~0xFFFu,
                       align < 0x1000 ? 0x1000 : align, name);
    return i < 0 ? 0 : g_blocks[i].base;
}

/* VRAM del juego: solo los bloques de tipo 0x9408060. */
static uint32_t vram_alloc(uint32_t size, uint32_t align, const char *name) {
    int i = list_alloc(g_vblocks, &g_vnblocks, (size + 0xFFFu) & ~0xFFFu,
                       align < 0x1000 ? 0x1000 : align, name);
    return i < 0 ? 0 : g_vblocks[i].base;
}

static void list_free(MemBlock *list, int *count, uint32_t base) {
    EnterCriticalSection(&g_mem_cs);
    for (int i = 0; i < *count; i++) {
        if (!list[i].used || list[i].base != base) continue;
        list[i].used = 0;
        /* unir con vecinos libres */
        if (i + 1 < *count && !list[i + 1].used) {
            list[i].size += list[i + 1].size;
            memmove(&list[i + 1], &list[i + 2], sizeof(MemBlock) * (size_t)(*count - i - 2));
            (*count)--;
        }
        if (i > 0 && !list[i - 1].used) {
            list[i - 1].size += list[i].size;
            memmove(&list[i], &list[i + 1], sizeof(MemBlock) * (size_t)(*count - i - 1));
            (*count)--;
        }
        break;
    }
    LeaveCriticalSection(&g_mem_cs);
}

static void block_free_addr(uint32_t base) {
    list_free(g_blocks, &g_nblocks, base);
    list_free(g_vblocks, &g_vnblocks, base);
    list_free(g_kblocks, &g_knblocks, base);
}

/* UID de un bloque = su dirección base | 1 (siempre positivo como int: las bases están por debajo de 0x80000000? no: usamos índice). */
#define MEMBLOCK_UID_BASE 0x10000000u
static uint32_t g_block_uid_base[4096];
static int g_nuids = 1;

void vita_hle_sceKernelAllocMemBlock(void) {
    char name[32];
    guest_str(r0, name, sizeof(name));
    uint32_t type = r1, size = r2;
    uint32_t align = (type == 0x0C208060u || type == 0x0C80D060u) ? 0x40000u : 0x1000u;
    /* 0x9408060 es VRAM: memoria dedicada de la GPU, con su propio presupuesto.
     * El resto (0xC208060, 0xC20D060, 0xC80D060...) es RAM principal. */
    uint32_t base = (type == 0x9408060u) ? vram_alloc(size, align, name)
                                         : game_alloc(size, align, name);
    if (!base || g_nuids >= 4096) {
        printf("[kernel] AllocMemBlock('%s', 0x%X, %u KB) SIN MEMORIA\n", name, type, size >> 10);
        r0 = ERR_NO_MEMORY;
        return;
    }
    g_block_uid_base[g_nuids] = base;
    r0 = MEMBLOCK_UID_BASE + (uint32_t)g_nuids++;
    printf("[kernel] AllocMemBlock('%s', 0x%X, %u KB) -> 0x%08X\n", name, type, size >> 10, base);
}

static uint32_t memblock_base(uint32_t uid) {
    uint32_t i = uid - MEMBLOCK_UID_BASE;
    return (i > 0 && i < (uint32_t)g_nuids) ? g_block_uid_base[i] : 0;
}

void vita_hle_sceKernelGetMemBlockBase(void) {
    uint32_t base = memblock_base(r0);
    if (!base) { r0 = ERR_UNKNOWN_UID; return; }
    vita_write32(r1, base);
    r0 = 0;
}

void vita_hle_sceKernelFreeMemBlock(void) {
    uint32_t base = memblock_base(r0);
    if (!base) { r0 = ERR_UNKNOWN_UID; return; }
    block_free_addr(base);
    g_block_uid_base[r0 - MEMBLOCK_UID_BASE] = 0;
    r0 = 0;
}

/* ------------------------------------------------------------------------- */
/* Hilos                                                                      */
/* ------------------------------------------------------------------------- */

#define THREAD_UID_BASE 0x40010000u
#define MAX_THREADS 256

typedef struct {
    int used, started, finished;
    char name[32];
    uint32_t entry, stack_base, stack_size, stack_top;
    uint32_t arg_size, arg_ptr;
    int priority;
    int32_t exit_status;
    HANDLE handle;
    jmp_buf exit_jb;
} GThread;

static GThread g_threads[MAX_THREADS];
static CRITICAL_SECTION g_thr_cs;
static VITA_TLS int t_index = -1;
static VITA_TLS uint32_t t_tls_block;
static HANDLE g_main_handle;   /* hilo principal (para el perfilador) */

/* Para el perfilador (MLB15_PERFIL=2): handle y nombre del hilo i, si está en marcha. */
int port_thread_info(int i, HANDLE *h, const char **name) {
    if (i < 0 || i >= MAX_THREADS || !g_threads[i].used || !g_threads[i].started) return 0;
    *h = i == 0 ? g_main_handle : g_threads[i].handle;
    *name = g_threads[i].name;
    return *h != NULL;
}

void port_threads_init(uint32_t main_stack_base, uint32_t main_stack_size) {
    InitializeCriticalSection(&g_thr_cs);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    GThread *t = &g_threads[0];
    t->used = t->started = 1;
    strcpy(t->name, "main");
    t->stack_base = main_stack_base;
    t->stack_size = main_stack_size;
    t->stack_top = main_stack_base + main_stack_size;
    t->priority = 160;
    t_index = 0;
}

int port_current_thread(void) { return t_index; }
const char *port_current_thread_name(void) { return t_index >= 0 && t_index < MAX_THREADS ? g_threads[t_index].name : "?"; }

static GThread *thread_by_uid(uint32_t uid) {
    uint32_t i = uid - THREAD_UID_BASE;
    return (i < MAX_THREADS && g_threads[i].used) ? &g_threads[i] : NULL;
}

void vita_hle_sceKernelCreateThread(void) {
    char name[32];
    guest_str(r0, name, sizeof(name));
    uint32_t entry = r1, stack = r3;
    int prio = (int)r2;
    if (stack < 0x4000) stack = 0x4000;
    EnterCriticalSection(&g_thr_cs);
    int idx = -1;
    for (int i = 1; i < MAX_THREADS; i++) if (!g_threads[i].used) { idx = i; break; }
    if (idx < 0) { LeaveCriticalSection(&g_thr_cs); r0 = ERR_NO_MEMORY; return; }
    GThread *t = &g_threads[idx];
    memset(t, 0, sizeof(*t));
    t->used = 1;
    LeaveCriticalSection(&g_thr_cs);
    strcpy(t->name, name);
    t->entry = entry;
    t->priority = prio;
    t->stack_size = (stack + 0xFFFu) & ~0xFFFu;
    t->stack_base = port_alloc(t->stack_size, 0x1000, name);
    if (!t->stack_base) {          /* sin pila: error, no un hilo con sp cerca de 0 */
        t->used = 0;
        r0 = ERR_NO_MEMORY;
        return;
    }
    t->stack_top = t->stack_base + t->stack_size;
    printf("[kernel] CreateThread('%s', entry=0x%08X, prio=%d, pila=%u KB) -> 0x%X\n",
           name, entry, prio, t->stack_size >> 10, THREAD_UID_BASE + (uint32_t)idx);
    r0 = THREAD_UID_BASE + (uint32_t)idx;
}

static DWORD WINAPI thread_main(LPVOID param) {
    int idx = (int)(intptr_t)param;
    GThread *t = &g_threads[idx];
    t_index = idx;
    port_watch_thread_start();          /* MLB15_VIGILA (watchdog.c) */
    sp = (t->arg_ptr ? t->arg_ptr : t->stack_top) & ~7u;
    sp -= 8;
    r0 = t->arg_size;
    r1 = t->arg_ptr;
    r2 = r3 = 0;
    lr = 0;
    if (!setjmp(t->exit_jb)) {
        vita_dispatch(0, t->entry);
        t->exit_status = (int32_t)r0;
    }
    t->finished = 1;
    printf("[kernel] hilo '%s' terminado (estado 0x%X)\n", t->name, (uint32_t)t->exit_status);
    fflush(stdout);
    return 0;
}

void vita_hle_sceKernelStartThread(void) {
    GThread *t = thread_by_uid(r0);
    if (!t) { r0 = ERR_UNKNOWN_UID; return; }
    uint32_t size = r1, argp = r2;
    t->arg_size = size;
    t->arg_ptr = 0;
    if (size && argp) { /* la Vita copia los argumentos en lo alto de la pila del hilo */
        uint32_t dst = (t->stack_top - size) & ~7u;
        memcpy(vita_mem_ptr(dst, size), vita_mem_ptr(argp, size), size);
        t->arg_ptr = dst;
    }
    t->started = 1;
    int idx = (int)(t - g_threads);
    t->handle = CreateThread(NULL, 4u << 20, thread_main, (LPVOID)(intptr_t)idx, 0, NULL);
    r0 = t->handle ? 0 : ERR_NO_MEMORY;
}

void vita_hle_sceKernelExitDeleteThread(void) {
    if (t_index > 0) {
        GThread *t = &g_threads[t_index];
        t->exit_status = (int32_t)r0;
        longjmp(t->exit_jb, 1);
    }
    printf("[kernel] ExitDeleteThread en el hilo principal: se ignora\n");
    r0 = 0;
}

void vita_hle_sceKernelDeleteThread(void) {
    GThread *t = thread_by_uid(r0);
    if (!t) { r0 = ERR_UNKNOWN_UID; return; }
    if (t->handle) CloseHandle(t->handle);
    if (t->stack_base) block_free_addr(t->stack_base);
    t->used = 0;
    r0 = 0;
}

void vita_hle_sceKernelWaitThreadEnd(void) {
    GThread *t = thread_by_uid(r0);
    if (!t) { r0 = ERR_UNKNOWN_UID; return; }
    uint32_t stat_ptr = r1;
    DWORD w = t->handle ? WaitForSingleObject(t->handle, timeout_ms(r2)) : WAIT_OBJECT_0;
    if (w == WAIT_TIMEOUT) { r0 = ERR_WAIT_TIMEOUT; return; }
    if (stat_ptr) vita_write32(stat_ptr, (uint32_t)t->exit_status);
    r0 = 0;
}

void vita_hle_sceKernelGetThreadExitStatus(void) {
    GThread *t = thread_by_uid(r0);
    if (!t) { r0 = ERR_UNKNOWN_UID; return; }
    r0 = t->finished ? (uint32_t)t->exit_status : 0x80028023u; /* NOT_DORMANT */
}

void vita_hle_sceKernelGetThreadId(void) { r0 = THREAD_UID_BASE + (uint32_t)(t_index < 0 ? 0 : t_index); }

void vita_hle_sceKernelGetThreadCurrentPriority(void) {
    r0 = (uint32_t)g_threads[t_index < 0 ? 0 : t_index].priority;
}

void vita_hle_sceKernelChangeThreadPriority(void) {
    GThread *t = r0 == 0 ? &g_threads[t_index < 0 ? 0 : t_index] : thread_by_uid(r0);
    if (t) t->priority = (int)r1;
    r0 = 0;
}

/* ------------------------------------------------------------------------- */
/* TLS de la guest (__thread)                                                 */
/* ------------------------------------------------------------------------- */

/* Plantilla TLS del módulo (SceModuleInfo: tls_start/tls_filesz/tls_memsz).
 * Cada hilo empieza con una copia: los __thread con valor inicial no son 0
 * (p. ej. el byte que hace que el cargador de texturas añada ".tp2"). */
static uint32_t g_tls_tmpl, g_tls_filesz, g_tls_memsz = 0x1000;

void port_tls_init(uint32_t tmpl, uint32_t filesz, uint32_t memsz) {
    g_tls_tmpl = tmpl;
    g_tls_filesz = filesz;
    g_tls_memsz = memsz > 0x1000 ? memsz : 0x1000;
    printf("[kernel] plantilla TLS 0x%08X (%u bytes con valor, %u en total)\n", tmpl, filesz, memsz);
}

/* r0 = descriptor {u32 módulo, u32 desplazamiento}; devuelve dirección guest. */
void vita_hle___tls_get_addr(void) {
    if (!t_tls_block) {
        uint32_t b = port_alloc(g_tls_memsz, 0x1000, "tls");
        uint8_t *dst = vita_mem_ptr(b, g_tls_memsz);
        const uint8_t *src = g_tls_filesz ? vita_mem_ptr(g_tls_tmpl, g_tls_filesz) : NULL;
        if (dst) {
            memset(dst, 0, g_tls_memsz);
            if (src) memcpy(dst, src, g_tls_filesz);
        }
        t_tls_block = b;
    }
    r0 = t_tls_block + vita_read32(r0 + 4);
}

/* ------------------------------------------------------------------------- */
/* Objetos de sincronización                                                  */
/* ------------------------------------------------------------------------- */

enum { OBJ_FREE, OBJ_SEMA, OBJ_MUTEX, OBJ_LWMUTEX, OBJ_LWCOND };

typedef struct {
    int type;
    char name[32];
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;     /* espera de semáforo / de propiedad del mutex */
    CONDITION_VARIABLE cond;   /* para LwCond */
    int count, max;            /* semáforo */
    int owner, recursion, recursive; /* mutex */
    int mutex_obj;             /* LwCond: objeto lwmutex asociado */
} KObj;

#define OBJ_UID_BASE 0x20000000u
#define MAX_OBJS 4096
static KObj g_objs[MAX_OBJS];
static CRITICAL_SECTION g_obj_cs;

void port_objs_init(void) { InitializeCriticalSection(&g_obj_cs); }

static int obj_new(int type, const char *name) {
    EnterCriticalSection(&g_obj_cs);
    int idx = -1;
    for (int i = 1; i < MAX_OBJS; i++) if (g_objs[i].type == OBJ_FREE) { idx = i; break; }
    if (idx > 0) {
        KObj *o = &g_objs[idx];
        memset(o, 0, sizeof(*o));
        o->type = type;
        strncpy(o->name, name ? name : "", sizeof(o->name) - 1);
        InitializeCriticalSection(&o->cs);
        InitializeConditionVariable(&o->cv);
        InitializeConditionVariable(&o->cond);
        o->owner = -1;
    }
    LeaveCriticalSection(&g_obj_cs);
    return idx;
}

static KObj *obj_get(uint32_t uid, int type) {
    uint32_t i = uid - OBJ_UID_BASE;
    return (i > 0 && i < MAX_OBJS && g_objs[i].type == type) ? &g_objs[i] : NULL;
}

static void obj_delete(KObj *o) {
    DeleteCriticalSection(&o->cs);
    o->type = OBJ_FREE;
}

/* --- semáforos --- */

void vita_hle_sceKernelCreateSema(void) {
    char name[32];
    guest_str(r0, name, sizeof(name));
    int idx = obj_new(OBJ_SEMA, name);
    if (idx < 0) { r0 = ERR_NO_MEMORY; return; }
    g_objs[idx].count = (int)r2;
    g_objs[idx].max = (int)r3;
    r0 = OBJ_UID_BASE + (uint32_t)idx;
}

void vita_hle_sceKernelDeleteSema(void) {
    KObj *o = obj_get(r0, OBJ_SEMA);
    if (!o) { r0 = ERR_UNKNOWN_UID; return; }
    obj_delete(o);
    r0 = 0;
}

void vita_hle_sceKernelOpenSema(void) {
    char name[32];
    guest_str(r0, name, sizeof(name));
    for (int i = 1; i < MAX_OBJS; i++)
        if (g_objs[i].type == OBJ_SEMA && !strcmp(g_objs[i].name, name)) { r0 = OBJ_UID_BASE + (uint32_t)i; return; }
    r0 = ERR_UNKNOWN_UID;
}

void vita_hle_sceKernelCloseSema(void) { r0 = 0; }

/* Esperas con timeout 0 que fallan: hilos del juego que sondean en bucle
 * (DataStream, MultiTask, Kinematics: ~650 000 esperas por segundo). En la
 * Vita el planificador los aparta porque tienen menos prioridad; en el PC cada
 * uno quemaba un núcleo entero y el hilo principal se quedaba sin CPU (la carga
 * del partido parecía congelada). Se cede la CPU, y tras muchos fallos
 * seguidos se duerme 1 ms: de ~12 núcleos a menos de 2.
 * Solo en semáforos: en los TryLock de mutex rompía el menú (X en "Exhibition
 * Game" dejaba de responder). MLB15_SIN_CEDER=1 lo desactiva. */
static VITA_TLS unsigned t_failed_polls;
static void poll_backoff(void) {
    static int off = -1;
    if (off < 0) off = getenv("MLB15_SIN_CEDER") ? 1 : 0;      /* para comparar con el comportamiento anterior */
    if (off) return;
    /* 4 cesiones y luego 1 ms (antes 32: cada hilo parado hacía ~33 000 sondeos por segundo,
     * ~6 % de CPU cada uno). MLB15_CEDER=n cambia el número de cesiones. */
    static int ceder = -1;
    if (ceder < 0) { const char *e = getenv("MLB15_CEDER"); ceder = e ? atoi(e) : 4; }
    if (++t_failed_polls < (unsigned)ceder) SwitchToThread();
    else { t_failed_polls = 0; Sleep(1); }
}

void vita_hle_sceKernelWaitSema(void) {
    KObj *o = obj_get(r0, OBJ_SEMA);
    if (!o) { r0 = ERR_UNKNOWN_UID; return; }
    int need = (int)r1;
    DWORD ms = timeout_ms(r2);
    EnterCriticalSection(&o->cs);
    while (o->count < need) {
        if (!SleepConditionVariableCS(&o->cv, &o->cs, ms)) {
            LeaveCriticalSection(&o->cs);
            if (ms == 0) poll_backoff();
            r0 = ERR_WAIT_TIMEOUT;
            return;
        }
    }
    o->count -= need;
    LeaveCriticalSection(&o->cs);
    t_failed_polls = 0;
    r0 = 0;
}

void vita_hle_sceKernelSignalSema(void) {
    KObj *o = obj_get(r0, OBJ_SEMA);
    if (!o) { r0 = ERR_UNKNOWN_UID; return; }
    EnterCriticalSection(&o->cs);
    o->count += (int)r1;
    if (o->max > 0 && o->count > o->max) o->count = o->max;
    LeaveCriticalSection(&o->cs);
    WakeAllConditionVariable(&o->cv);
    r0 = 0;
}

/* --- mutex (normal y lightweight comparten la lógica) --- */

/* MLB15_MUTEX_TRAZA=1: cada ~5 s, mutex con más actividad (bloqueos, esperas, desbloqueos con
 * alguien esperando, despertares en vano). */
static volatile LONG g_mx_bloq[1024], g_mx_esp[1024], g_mx_wake[1024], g_mx_vano[1024];
static void mutex_traza(void) {
    static int on = -1;
    static ULONGLONG t0;
    if (on < 0) { on = getenv("MLB15_MUTEX_TRAZA") ? 1 : 0; t0 = GetTickCount64(); }
    if (!on || GetTickCount64() - t0 < 5000) return;
    t0 = GetTickCount64();
    printf("[mutex] /5s:");
    for (int i = 0; i < 1024 && i < MAX_OBJS; i++)
        if (g_mx_bloq[i] > 2000) printf(" %s: bloq %ld esp %ld despierta %ld vano %ld |", g_objs[i].name, g_mx_bloq[i], g_mx_esp[i], g_mx_wake[i], g_mx_vano[i]);
    printf("\n");
    for (int i = 0; i < 1024; i++) g_mx_bloq[i] = g_mx_esp[i] = g_mx_wake[i] = g_mx_vano[i] = 0;
}
/* Bloqueo con espera 0 que falla: el juego sondea los mutex «Render 0/1» en bucle (~150 000
 * veces por segundo cada uno) mientras espera al dibujo; el hilo principal giraba en vacío. Tras
 * unos fallos seguidos se cede y se duerme 0,2 ms (temporizador de alta resolución). No se toca
 * TryLock (rompía el menú cuando se dormía 1 ms con el temporizador grueso). MLB15_MUTEX_SONDEO=0
 * vuelve a lo anterior. */
void port_dormir_us(uint32_t us);   /* hle_mlb15.c */
static VITA_TLS unsigned t_mx_fallos;
static void mutex_respiro(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("MLB15_MUTEX_SONDEO"); on = e ? atoi(e) != 0 : 1; }
    if (!on) return;
    if (++t_mx_fallos < 4) SwitchToThread();
    else { t_mx_fallos = 0; port_dormir_us(200); }
}
static uint32_t mutex_lock(KObj *o, int count, DWORD ms, int try_only) {
    int me = t_index;
    int oi = (int)(o - g_objs) & 1023;
    InterlockedIncrement(&g_mx_bloq[oi]);
    mutex_traza();
    EnterCriticalSection(&o->cs);
    if (o->owner == me && o->recursion > 0) {
        if (!o->recursive) { LeaveCriticalSection(&o->cs); return ERR_MUTEX_FAILED; }
        o->recursion += count;
        LeaveCriticalSection(&o->cs);
        return 0;
    }
    int vueltas = 0;
    while (o->recursion > 0) {
        if (!try_only) { if (!vueltas) InterlockedIncrement(&g_mx_esp[oi]); else InterlockedIncrement(&g_mx_vano[oi]); }
        vueltas++;
        if (try_only || !SleepConditionVariableCS(&o->cv, &o->cs, ms)) {
            LeaveCriticalSection(&o->cs);
            if (!try_only && ms == 0) mutex_respiro();
            return try_only ? ERR_LWMUTEX_FAILED : ERR_WAIT_TIMEOUT;
        }
    }
    t_mx_fallos = 0;
    o->owner = me;
    o->recursion = count > 0 ? count : 1;
    LeaveCriticalSection(&o->cs);
    return 0;
}

static uint32_t mutex_unlock(KObj *o, int count) {
    EnterCriticalSection(&o->cs);
    if (o->owner != t_index || o->recursion <= 0) { LeaveCriticalSection(&o->cs); return ERR_MUTEX_FAILED; }
    o->recursion -= count > 0 ? count : 1;
    if (o->recursion <= 0) { o->recursion = 0; o->owner = -1; InterlockedIncrement(&g_mx_wake[(int)(o - g_objs) & 1023]); }
    LeaveCriticalSection(&o->cs);
    WakeAllConditionVariable(&o->cv);
    return 0;
}

void vita_hle_sceKernelCreateMutex(void) {
    char name[32];
    guest_str(r0, name, sizeof(name));
    int idx = obj_new(OBJ_MUTEX, name);
    if (idx < 0) { r0 = ERR_NO_MEMORY; return; }
    g_objs[idx].recursive = (r1 & 0x2) != 0;
    if ((int)r2 > 0) { g_objs[idx].owner = t_index; g_objs[idx].recursion = (int)r2; }
    r0 = OBJ_UID_BASE + (uint32_t)idx;
}

void vita_hle_sceKernelDeleteMutex(void) {
    KObj *o = obj_get(r0, OBJ_MUTEX);
    if (!o) { r0 = ERR_UNKNOWN_UID; return; }
    obj_delete(o);
    r0 = 0;
}

void vita_hle_sceKernelLockMutex(void) {
    KObj *o = obj_get(r0, OBJ_MUTEX);
    r0 = o ? mutex_lock(o, (int)r1, timeout_ms(r2), 0) : ERR_UNKNOWN_UID;
}

void vita_hle_sceKernelUnlockMutex(void) {
    KObj *o = obj_get(r0, OBJ_MUTEX);
    r0 = o ? mutex_unlock(o, (int)r1) : ERR_UNKNOWN_UID;
}

/* LwMutex: el "work" (32 bytes en memoria guest) guarda nuestro UID. */
static KObj *lw_get(uint32_t work, int type) { return work ? obj_get(vita_read32(work), type) : NULL; }

void vita_hle_sceKernelCreateLwMutex(void) {
    char name[32];
    guest_str(r1, name, sizeof(name));
    int idx = obj_new(OBJ_LWMUTEX, name);
    if (idx < 0) { r0 = ERR_NO_MEMORY; return; }
    g_objs[idx].recursive = (r2 & 0x2) != 0;
    if ((int)r3 > 0) { g_objs[idx].owner = t_index; g_objs[idx].recursion = (int)r3; }
    vita_write32(r0, OBJ_UID_BASE + (uint32_t)idx);
    r0 = 0;
}

void vita_hle_sceKernelDeleteLwMutex(void) {
    KObj *o = lw_get(r0, OBJ_LWMUTEX);
    if (!o) { r0 = ERR_UNKNOWN_UID; return; }
    obj_delete(o);
    r0 = 0;
}

void vita_hle_sceKernelLockLwMutex(void) {
    KObj *o = lw_get(r0, OBJ_LWMUTEX);
    r0 = o ? mutex_lock(o, (int)r1, timeout_ms(r2), 0) : ERR_UNKNOWN_UID;
}

void vita_hle_sceKernelTryLockLwMutex(void) {
    KObj *o = lw_get(r0, OBJ_LWMUTEX);
    r0 = o ? mutex_lock(o, (int)r1, 0, 1) : ERR_UNKNOWN_UID;
}

void vita_hle_sceKernelUnlockLwMutex(void) {
    KObj *o = lw_get(r0, OBJ_LWMUTEX);
    r0 = o ? mutex_unlock(o, (int)r1) : ERR_UNKNOWN_UID;
}

/* --- LwCond: variable de condición ligada a un LwMutex --- */

void vita_hle_sceKernelCreateLwCond(void) {
    char name[32];
    guest_str(r1, name, sizeof(name));
    int idx = obj_new(OBJ_LWCOND, name);
    if (idx < 0) { r0 = ERR_NO_MEMORY; return; }
    g_objs[idx].mutex_obj = (int)(vita_read32(r3) - OBJ_UID_BASE);
    vita_write32(r0, OBJ_UID_BASE + (uint32_t)idx);
    r0 = 0;
}

void vita_hle_sceKernelDeleteLwCond(void) {
    KObj *o = lw_get(r0, OBJ_LWCOND);
    if (!o) { r0 = ERR_UNKNOWN_UID; return; }
    obj_delete(o);
    r0 = 0;
}

void vita_hle_sceKernelWaitLwCond(void) {
    KObj *c = lw_get(r0, OBJ_LWCOND);
    if (!c || c->mutex_obj <= 0 || c->mutex_obj >= MAX_OBJS) { r0 = ERR_UNKNOWN_UID; return; }
    KObj *m = &g_objs[c->mutex_obj];
    DWORD ms = timeout_ms(r1);
    EnterCriticalSection(&m->cs);
    int saved = m->recursion;           /* soltar el mutex por completo */
    m->recursion = 0;
    m->owner = -1;
    WakeAllConditionVariable(&m->cv);
    BOOL ok = SleepConditionVariableCS(&c->cond, &m->cs, ms);
    while (m->recursion > 0) SleepConditionVariableCS(&m->cv, &m->cs, INFINITE); /* recuperarlo */
    m->owner = t_index;
    m->recursion = saved > 0 ? saved : 1;
    LeaveCriticalSection(&m->cs);
    r0 = ok ? 0 : ERR_WAIT_TIMEOUT;
}

void vita_hle_sceKernelSignalLwCond(void) {
    KObj *c = lw_get(r0, OBJ_LWCOND);
    if (!c) { r0 = ERR_UNKNOWN_UID; return; }
    WakeConditionVariable(&c->cond);
    r0 = 0;
}

void vita_hle_sceKernelSignalLwCondAll(void) {
    KObj *c = lw_get(r0, OBJ_LWCOND);
    if (!c) { r0 = ERR_UNKNOWN_UID; return; }
    WakeAllConditionVariable(&c->cond);
    r0 = 0;
}

/* ------------------------------------------------------------------------- */
/* std::_Mutex de la librería C++ de Sony (this = objeto guest)               */
/* ------------------------------------------------------------------------- */

void vita_hle__ZNSt6_MutexC1Ev(void) {
    uint32_t self = r0;
    int idx = obj_new(OBJ_MUTEX, "std::_Mutex");
    if (idx > 0) g_objs[idx].recursive = 1;
    vita_write32(self, idx > 0 ? OBJ_UID_BASE + (uint32_t)idx : 0);
    r0 = self;
}

void vita_hle__ZNSt6_MutexD1Ev(void) {
    KObj *o = obj_get(vita_read32(r0), OBJ_MUTEX);
    if (o) obj_delete(o);
}

void vita_hle__ZNSt6_Mutex5_LockEv(void) {
    KObj *o = obj_get(vita_read32(r0), OBJ_MUTEX);
    if (o) mutex_lock(o, 1, INFINITE, 0);
}

void vita_hle__ZNSt6_Mutex7_UnlockEv(void) {
    KObj *o = obj_get(vita_read32(r0), OBJ_MUTEX);
    if (o) mutex_unlock(o, 1);
}

/* ios_base: el juego no usa iostreams de verdad; basta con no hacer nada. */
void vita_hle__ZNSt8ios_base5_InitEv(void) { }
void vita_hle__ZNSt8ios_baseD2Ev(void) { }
void vita_hle__ZNSt8ios_base5clearENSt5_IosbIiE8_IostateEb(void) { }

/* ------------------------------------------------------------------------- */
/* Módulos                                                                    */
/* ------------------------------------------------------------------------- */

static uint32_t g_next_modid = 0x00100001u;

/* sceKernelLoadStartModule(path, args, argp, flags, opt, *status): los módulos
 * del juego (libc, libfios2...) los sustituye el HLE; se finge la carga. */
void vita_hle_sceKernelLoadStartModule(void) {
    char path[128];
    guest_str(r0, path, sizeof(path));
    uint32_t status_ptr = arg_stack(1);
    printf("[kernel] LoadStartModule('%s') -> simulado\n", path);
    if (status_ptr) vita_write32(status_ptr, 0);
    r0 = g_next_modid++;
}

void vita_hle_sceSysmoduleLoadModule(void)   { printf("[kernel] SysmoduleLoadModule(0x%X)\n", r0); r0 = 0; }
void vita_hle_sceSysmoduleUnloadModule(void) { r0 = 0; }
void vita_hle_sceSysmoduleIsLoaded(void)     { r0 = 0; }

/* ------------------------------------------------------------------------- */
/* Utilidades                                                                 */
/* ------------------------------------------------------------------------- */

void vita_hle_sceClibMemcpy_safe(void) {
    if (r2) memmove(vita_mem_ptr(r0, r2), vita_mem_ptr(r1, r2), r2);
}
void vita_hle_sceClibMemmove(void) { vita_hle_sceClibMemcpy_safe(); }
void vita_hle_sceClibMemset(void) {
    if (r2) memset(vita_mem_ptr(r0, r2), (int)(r1 & 0xFF), r2);
}

void vita_hle___stack_chk_fail(void) {
    printf("[guest] __stack_chk_fail: corrupción de pila detectada\n");
    fflush(stdout);
    vita_trap_unimpl(0, 0, "__stack_chk_fail");
}
void vita_hle___sce_aeabi_idiv0(void) { r0 = 0; }
void vita_hle___sce_aeabi_ldiv0(void) { r0 = 0; r1 = 0; }

/* AppMgr: sin eventos del sistema; estado de la app a cero. */
void vita_hle_sceAppMgrReceiveSystemEvent(void) { r0 = 0x80800003u; }
void vita_hle__sceAppMgrGetAppState(void) {
    if (r0 && r1) memset(vita_mem_ptr(r0, r1), 0, r1);
    r0 = 0;
}
