/* FIOS2 (libfios2 de Sony) sobre archivos del PC, para MLB 15.
 *
 * Todas las operaciones se hacen de forma síncrona: las asíncronas (FHPread,
 * FHClose...) devuelven un "op" que ya está terminado. Los montajes de archivos
 * PSARC (VITA_COMPRESSED_WAD.arc, VITA_PATCH_WAD.arc) se leen directamente del
 * .arc de la carpeta del juego (psarc.c). Para desarrollo, MLB15_WAD y
 * MLB15_WAD_PATCH apuntan en su lugar a carpetas extraídas con tools/psarc.py.
 *
 * Firmas (públicas de libfios2): los SceFiosSize/SceFiosOffset son de 64 bits y
 * van en pareja de registros alineada o en la pila; se devuelven en r0:r1.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"

#include "port.h"
#include "psarc.h"
#include "input.h"
#include "launcher.h"

#define FIOS_ERR_NOT_FOUND 0x80820004u
#define FIOS_ERR_BAD_FH    0x80820008u

static int g_trace_fios = 1;
#define FTRACE(...) do { if (g_trace_fios) { printf("[fios] " __VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

static uint32_t stk(int i) { return vita_read32(sp + 4u * (uint32_t)i); }
static void ret64(int64_t v) { r0 = (uint32_t)(uint64_t)v; r1 = (uint32_t)((uint64_t)v >> 32); }

/* ------------------------------------------------------------------------- */
/* Montajes                                                                    */
/* ------------------------------------------------------------------------- */

/* Un montaje es un .arc abierto (arc >= 0) o una carpeta extraída (arc = -1). */
typedef struct { char mount[256]; int arc; char dir[MAX_PATH]; } Mount;
static Mount g_mounts[8];
static int g_nmounts;
static char g_wad[MAX_PATH], g_wad_patch[MAX_PATH];

static void fios_dirs_init(void) {
    const char *w = getenv("MLB15_WAD"), *p = getenv("MLB15_WAD_PATCH");
    snprintf(g_wad, sizeof(g_wad), "%s", w && *w ? w : "");
    snprintf(g_wad_patch, sizeof(g_wad_patch), "%s", p && *p ? p : "");
}

static int file_exists(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES;
}

/* Dónde está una ruta FIOS: dentro de un .arc (arc, entry; entry = -1 si es un
 * directorio del archivo) o en el disco (host). */
typedef struct { int arc, entry, is_dir; char host[1024]; } Loc;

/* Jugadores siempre con el modelo completo: el juego carga tres cuerpos (PSP2Body_LOD1,
 * LOD4 y Lod5) y, según la distancia, dibuja uno simplificado con shaders "LowLod": el
 * lanzador, los del campo y el bateador en algunos planos salían con menos polígonos y
 * sin detalle. Pidiendo LOD4/Lod5 se sirve LOD1 (el de la sombra, CastShadow_LOD4, no).
 * MLB15.ini [video] jugadores_alta = 1; MLB15_JUGADORES_ALTA manda. */
static const char *lod_alto(const char *guest, char *buf, size_t max) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MLB15_JUGADORES_ALTA");
        on = e ? atoi(e) != 0 : launcher_ini_int("video", "jugadores_alta", 1) != 0;
    }
    if (!on) return guest;
    size_t n = strlen(guest);
    for (size_t i = 0; i + 13 <= n; i++) {
        if (_strnicmp(guest + i, "PSP2Body_Lod", 12) || (guest[i + 12] != '4' && guest[i + 12] != '5')) continue;
        if (n >= max) return guest;
        memcpy(buf, guest, n + 1);
        buf[i + 9] = 'L'; buf[i + 10] = 'O'; buf[i + 11] = 'D'; buf[i + 12] = '1';
        return buf;
    }
    return guest;
}

/* Primero los montajes (el último montado gana: el parche va encima), si no el VFS. */
static int fios_resolve(const char *guest, Loc *l) {
    char lodbuf[1024];
    guest = lod_alto(guest, lodbuf, sizeof(lodbuf));
    l->arc = -1; l->entry = -1; l->is_dir = 0; l->host[0] = 0;
    int matched = -1;
    for (int i = g_nmounts - 1; i >= 0; i--) {
        size_t ml = strlen(g_mounts[i].mount);
        /* Un punto de montaje que acaba en ':' ya separa: el juego pide tanto
         * "wadC:/SNDPSP2/..." como "wadC:SNDPSP2/...". */
        char last = g_mounts[i].mount[ml ? ml - 1 : 0];
        if (!ml || _strnicmp(guest, g_mounts[i].mount, ml) ||
            !(guest[ml] == '/' || guest[ml] == 0 || last == ':' || last == '/'))
            continue;
        const char *rest = guest + ml;
        while (*rest == '/') rest++;
        matched = i;
        if (g_mounts[i].arc >= 0) {
            int e = psarc_find(g_mounts[i].arc, rest);
            if (e >= 0) { l->arc = g_mounts[i].arc; l->entry = e; return 1; }
            if (psarc_is_dir(g_mounts[i].arc, rest)) { l->arc = g_mounts[i].arc; l->is_dir = 1; return 1; }
        } else {
            snprintf(l->host, sizeof(l->host), "%s\\%s", g_mounts[i].dir, rest);
            for (char *c = l->host; *c; c++) if (*c == '/') *c = '\\';
            if (file_exists(l->host)) return 1;
        }
    }
    if (matched >= 0) { l->host[0] = 0; return 0; }    /* era de un montaje y no está */
    /* "/app0/..." -> "app0:/..." */
    char tmp[1024];
    if (guest[0] == '/') {
        const char *s = guest + 1, *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        snprintf(tmp, sizeof(tmp), "%.*s:%s", (int)n, s, e ? e : "");
        guest = tmp;
    }
    return port_vfs_path(guest, l->host, sizeof(l->host));
}

/* Tamaño y si existe / es directorio, sea cual sea el sitio. */
static int loc_stat(const Loc *l, int64_t *size, int *is_dir) {
    if (l->arc >= 0) {
        *is_dir = l->is_dir;
        *size = l->is_dir ? 0 : psarc_size(l->arc, l->entry);
        return 1;
    }
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!l->host[0] || !GetFileAttributesExA(l->host, GetFileExInfoStandard, &fa)) return 0;
    *is_dir = (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    *size = (int64_t)((uint64_t)fa.nFileSizeHigh << 32 | fa.nFileSizeLow);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Manejadores de archivo, directorio y operaciones                          */
/* ------------------------------------------------------------------------- */

typedef struct { int used; HANDLE h; int arc, entry; int64_t pos; } FH;
#define MAX_FH 512
static FH g_fh[MAX_FH];
static CRITICAL_SECTION g_fios_cs;
static int g_fios_init;

typedef struct { int used; int64_t actual; int32_t error; } FiosOp;
#define MAX_OPS 1024
static FiosOp g_ops[MAX_OPS];

static void lazy_init(void) {
    if (g_fios_init) return;
    InitializeCriticalSection(&g_fios_cs);
    fios_dirs_init();
    g_fios_init = 1;
}

static uint32_t op_new(int64_t actual, int32_t error) {
    EnterCriticalSection(&g_fios_cs);
    uint32_t id = 0;
    for (uint32_t i = 1; i < MAX_OPS; i++) if (!g_ops[i].used) { g_ops[i].used = 1; g_ops[i].actual = actual; g_ops[i].error = error; id = i; break; }
    LeaveCriticalSection(&g_fios_cs);
    return id;
}

static FiosOp *op_get(uint32_t id) { return id && id < MAX_OPS && g_ops[id].used ? &g_ops[id] : NULL; }

static int fh_new(HANDLE h, int arc, int entry) {
    EnterCriticalSection(&g_fios_cs);
    int id = -1;
    for (int i = 1; i < MAX_FH; i++)
        if (!g_fh[i].used) { g_fh[i].used = 1; g_fh[i].h = h; g_fh[i].arc = arc; g_fh[i].entry = entry; g_fh[i].pos = 0; id = i; break; }
    LeaveCriticalSection(&g_fios_cs);
    return id;
}

static FH *fh_get(uint32_t id) { return id && id < MAX_FH && g_fh[id].used ? &g_fh[id] : NULL; }

static int64_t fh_size(FH *f) {
    if (f->arc >= 0) return psarc_size(f->arc, f->entry);
    LARGE_INTEGER sz = { 0 };
    GetFileSizeEx(f->h, &sz);
    return sz.QuadPart;
}

/* off < 0: desde la posición actual, que avanza. */
static int64_t fh_read(FH *f, uint32_t buf, int64_t len, int64_t off) {
    if (len <= 0) return 0;
    void *dst = vita_mem_ptr(buf, (uint32_t)len);
    if (!dst) return -1;
    if (f->arc >= 0) {
        int64_t at = off >= 0 ? off : f->pos;
        int64_t got = psarc_read(f->arc, f->entry, (uint64_t)at, dst, (uint64_t)len);
        if (off < 0 && got > 0) f->pos += got;
        return got;
    }
    if (off >= 0) { LARGE_INTEGER li; li.QuadPart = off; SetFilePointerEx(f->h, li, NULL, FILE_BEGIN); }
    DWORD got = 0;
    if (!ReadFile(f->h, dst, (DWORD)len, &got, NULL)) return -1;
    return got;
}

/* ------------------------------------------------------------------------- */
/* API                                                                        */
/* ------------------------------------------------------------------------- */

void vita_hle_sceFiosInitialize(void) { lazy_init(); FTRACE("Initialize"); r0 = 0; }
void vita_hle_sceFiosTerminate(void) { r0 = 0; }

void vita_hle_sceFiosIOFilterAdd(void) { FTRACE("IOFilterAdd(%d, 0x%08X)", (int)r0, r1); r0 = 0; }
void vita_hle_sceFiosIOFilterPsarcDearchiver(void) { r0 = 0; }

/* SceFiosSize ArchiveGetMountBufferSizeSync(attr, path, params) */
void vita_hle_sceFiosArchiveGetMountBufferSizeSync(void) {
    char p[256];
    port_guest_str(r1, p, sizeof(p));
    FTRACE("ArchiveGetMountBufferSizeSync('%s')", p);
    ret64(0x1000);
}

/* int ArchiveMountSync(attr, SceFiosFH *fh_out, archive_path, mount_point, SceFiosBuffer, params) */
void vita_hle_sceFiosArchiveMountSync(void) {
    lazy_init();
    char arc[256], mnt[256];
    uint32_t fh_out = r1;
    port_guest_str(r2, arc, sizeof(arc));
    port_guest_str(r3, mnt, sizeof(mnt));
    if (g_nmounts >= 8) { r0 = FIOS_ERR_BAD_FH; return; }
    Mount *m = &g_mounts[g_nmounts];
    snprintf(m->mount, sizeof(m->mount), "%s", mnt);
    m->arc = -1;
    m->dir[0] = 0;
    const char *dev = strstr(arc, "PATCH") ? g_wad_patch : g_wad;
    if (dev[0]) {                                   /* desarrollo: carpeta extraída */
        snprintf(m->dir, sizeof(m->dir), "%s", dev);
        FTRACE("ArchiveMountSync('%s' en '%s') -> carpeta %s", arc, mnt, dev);
    } else {
        Loc l;
        if (!fios_resolve(arc, &l) || !l.host[0] || (m->arc = psarc_open(l.host)) < 0) {
            FTRACE("ArchiveMountSync('%s' en '%s') -> NO SE PUDO ABRIR", arc, mnt);
            r0 = FIOS_ERR_NOT_FOUND;
            return;
        }
        FTRACE("ArchiveMountSync('%s' en '%s') -> %s", arc, mnt, l.host);
    }
    g_nmounts++;
    if (fh_out) vita_write32(fh_out, 0x7FFF0000u + (uint32_t)g_nmounts);
    r0 = 0;
}

static uint32_t open_file(uint32_t fh_out, uint32_t path_addr, uint32_t params, int log) {
    lazy_init();
    char guest[512];
    port_guest_str(path_addr, guest, sizeof(guest));
    (void)params;
    Loc l;
    HANDLE h = INVALID_HANDLE_VALUE;
    int ok = fios_resolve(guest, &l);
    if (ok && l.arc >= 0) ok = !l.is_dir;
    else if (ok) {
        h = CreateFileA(l.host, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        ok = h != INVALID_HANDLE_VALUE;
    }
    if (log) FTRACE("FHOpen('%s') -> %s", guest, ok ? "ok" : "NO EXISTE");
    if (ok) input_evento(guest);
    {   /* MLB15_FIOS_PILA=1: pila nativa de las primeras aperturas fallidas, para
         * ver qué función guest construyó la ruta (tools/backtrace.py).
         * Con otro valor, solo las rutas que lo contienen (p. ej. FONTS). */
        static int shown;
        const char *want = getenv("MLB15_FIOS_PILA");
        if (want && ((!ok && !strcmp(want, "1")) || strstr(guest, want)) && shown++ < 3) {
            void host_backtrace(void);
            host_backtrace();
        }
    }
    if (!ok) return FIOS_ERR_NOT_FOUND;
    int id = fh_new(l.arc >= 0 ? NULL : h, l.arc, l.entry);
    if (id < 0) { if (l.arc < 0) CloseHandle(h); return FIOS_ERR_BAD_FH; }
    if (fh_out) vita_write32(fh_out, (uint32_t)id);
    return 0;
}

/* int FHOpenSync(attr, SceFiosFH *out, path, openParams) */
void vita_hle_sceFiosFHOpenSync(void) { r0 = open_file(r1, r2, r3, 1); }

/* SceFiosOp FHOpen(attr, SceFiosFH *out, path, openParams) */
void vita_hle_sceFiosFHOpen(void) {
    uint32_t err = open_file(r1, r2, r3, 1);
    r0 = op_new(0, (int32_t)err);
}

/* SceFiosSize FHReadSync(attr, fh, buf, SceFiosSize len) -> len en la pila */
void vita_hle_sceFiosFHReadSync(void) {
    FH *f = fh_get(r1);
    int64_t len = (int64_t)((uint64_t)stk(1) << 32 | stk(0));
    if (!f) { ret64(-1); return; }
    ret64(fh_read(f, r2, len, -1));
}

/* SceFiosOp FHPread(attr, fh, buf, SceFiosSize len, SceFiosOffset off) */
void vita_hle_sceFiosFHPread(void) {
    FH *f = fh_get(r1);
    int64_t len = (int64_t)((uint64_t)stk(1) << 32 | stk(0));
    int64_t off = (int64_t)((uint64_t)stk(3) << 32 | stk(2));
    int64_t got = f ? fh_read(f, r2, len, off) : -1;
    r0 = op_new(got, got < 0 ? (int32_t)FIOS_ERR_BAD_FH : 0);
}

/* SceFiosOffset FHSeek(fh, SceFiosOffset off (r2:r3), whence (pila)) */
void vita_hle_sceFiosFHSeek(void) {
    FH *f = fh_get(r0);
    if (!f) { ret64(-1); return; }
    int64_t off = (int64_t)((uint64_t)r3 << 32 | r2);
    uint32_t whence = stk(0);
    if (f->arc >= 0) {
        int64_t base = whence == 1 ? f->pos : whence == 2 ? fh_size(f) : 0;
        f->pos = base + off;
        if (f->pos < 0) f->pos = 0;
        ret64(f->pos);
        return;
    }
    LARGE_INTEGER li, out;
    li.QuadPart = off;
    SetFilePointerEx(f->h, li, &out, whence == 1 ? FILE_CURRENT : whence == 2 ? FILE_END : FILE_BEGIN);
    ret64(out.QuadPart);
}

void vita_hle_sceFiosFHTell(void) {
    FH *f = fh_get(r0);
    if (!f) { ret64(-1); return; }
    if (f->arc >= 0) { ret64(f->pos); return; }
    LARGE_INTEGER z = { 0 }, out = { 0 };
    SetFilePointerEx(f->h, z, &out, FILE_CURRENT);
    ret64(out.QuadPart);
}

void vita_hle_sceFiosFHGetSize(void) {
    FH *f = fh_get(r0);
    ret64(f ? fh_size(f) : -1);
}

static uint32_t close_fh(uint32_t id) {
    FH *f = fh_get(id);
    if (!f) return FIOS_ERR_BAD_FH;
    if (f->arc < 0 && f->h) CloseHandle(f->h);
    f->used = 0;
    return 0;
}

void vita_hle_sceFiosFHCloseSync(void) { r0 = close_fh(r1); }
void vita_hle_sceFiosFHClose(void) { r0 = op_new(0, (int32_t)close_fh(r1)); }

void vita_hle_sceFiosFHTruncateSync(void) { r0 = 0; }

/* bool FileExistsSync(attr, path) */
void vita_hle_sceFiosFileExistsSync(void) {
    lazy_init();
    char guest[512];
    port_guest_str(r1, guest, sizeof(guest));
    Loc l;
    int64_t size;
    int is_dir, ok = fios_resolve(guest, &l) && loc_stat(&l, &size, &is_dir) && !is_dir;
    FTRACE("FileExists('%s') -> %d", guest, ok);
    /* La versión Sync devuelve el bool en r0 (la asíncrona, sceFiosFileExists,
     * es la que lo escribe en un puntero). Antes: r0 = 0 siempre y un byte
     * escrito en r2, que aquí es basura (el juego lo deja apuntando a su pila).
     * Con "no existe" el cargador de modelos no pedía las partes .0.pp2 y los
     * jugadores se dibujaban sin vértices ni índices (punteros 0/0xDEADF00D). */
    r0 = (uint32_t)ok;
}

/* SceFiosSize FileGetSizeSync(attr, path) */
void vita_hle_sceFiosFileGetSizeSync(void) {
    lazy_init();
    char guest[512];
    port_guest_str(r1, guest, sizeof(guest));
    Loc l;
    int64_t size;
    int is_dir;
    ret64(fios_resolve(guest, &l) && loc_stat(&l, &size, &is_dir) ? size : -1);
}

/* int StatSync(attr, path, SceFiosStat *out): offset(8) size(8) ... statFlags(+0x20) */
void vita_hle_sceFiosStatSync(void) {
    lazy_init();
    char guest[512];
    port_guest_str(r1, guest, sizeof(guest));
    uint32_t out = r2;
    Loc l;
    int64_t size;
    int is_dir;
    if (!fios_resolve(guest, &l) || !loc_stat(&l, &size, &is_dir)) {
        FTRACE("Stat('%s') -> NO EXISTE", guest);
        r0 = FIOS_ERR_NOT_FOUND;
        return;
    }
    if (out) {
        memset(vita_mem_ptr(out, 0x60), 0, 0x60);
        vita_write32(out + 8, (uint32_t)size);
        vita_write32(out + 12, (uint32_t)((uint64_t)size >> 32));
        vita_write32(out + 0x20, is_dir ? 0x7u : 0x6u);
    }
    r0 = 0;
}

/* Directorios: DHOpenSync(attr, SceFiosDH *out, path, SceFiosBuffer) */
typedef struct {
    int used;
    HANDLE find; WIN32_FIND_DATAA data; int first;      /* en el disco */
    int arc; uint32_t it; char dirs[64][64]; int ndirs; /* en un .arc */
    char base[512];
} Dir;
static Dir g_dirs[64];

void vita_hle_sceFiosDHOpenSync(void) {
    lazy_init();
    char guest[512], pat[1100];
    port_guest_str(r2, guest, sizeof(guest));
    uint32_t out = r1;
    Loc l;
    if (!fios_resolve(guest, &l)) { r0 = FIOS_ERR_NOT_FOUND; return; }
    for (int i = 1; i < 64; i++) {
        Dir *d = &g_dirs[i];
        if (d->used) continue;
        memset(d, 0, sizeof(*d));
        if (l.arc >= 0) {
            if (!l.is_dir) { r0 = FIOS_ERR_NOT_FOUND; return; }
            d->arc = l.arc;
        } else {
            snprintf(pat, sizeof(pat), "%s\\*", l.host);
            d->arc = -1;
            d->find = FindFirstFileA(pat, &d->data);
            if (d->find == INVALID_HANDLE_VALUE) { r0 = FIOS_ERR_NOT_FOUND; return; }
            d->first = 1;
        }
        d->used = 1;
        snprintf(d->base, sizeof(d->base), "%s", guest);
        if (out) vita_write32(out, (uint32_t)i);
        FTRACE("DHOpen('%s')", guest);
        r0 = 0;
        return;
    }
    r0 = FIOS_ERR_BAD_FH;
}

/* La ruta del directorio dentro de su montaje (para preguntar al .arc). */
static const char *in_mount(const char *guest) {
    for (int i = g_nmounts - 1; i >= 0; i--) {
        size_t ml = strlen(g_mounts[i].mount);
        if (ml && !_strnicmp(guest, g_mounts[i].mount, ml)) {
            const char *r = guest + ml;
            while (*r == '/') r++;
            return r;
        }
    }
    return guest;
}

/* DHReadSync(attr, dh, SceFiosDirEntry *out): size(8) statFlags(4) nameLen(2) fullLen(2) nameOff(2) pad(6) path[1024] */
void vita_hle_sceFiosDHReadSync(void) {
    uint32_t id = r1, out = r2;
    if (!id || id >= 64 || !g_dirs[id].used) { r0 = FIOS_ERR_BAD_FH; return; }
    Dir *d = &g_dirs[id];
    char name[260];
    uint64_t size = 0;
    int is_dir = 0;
    if (d->arc >= 0) {
        const char *rel = in_mount(d->base);
        for (;;) {
            if (!psarc_dir_next(d->arc, rel, &d->it, name, sizeof(name), &is_dir)) { r0 = FIOS_ERR_NOT_FOUND; return; }
            if (!is_dir) break;
            int seen = 0;                                   /* un subdirectorio, una vez */
            for (int k = 0; k < d->ndirs && !seen; k++) seen = !_stricmp(d->dirs[k], name);
            if (seen) continue;
            if (d->ndirs < 64) snprintf(d->dirs[d->ndirs++], sizeof(d->dirs[0]), "%s", name);
            break;
        }
        if (!is_dir) {
            char rel_full[800];
            snprintf(rel_full, sizeof(rel_full), "%s/%s", rel, name);
            int64_t s = psarc_size(d->arc, psarc_find(d->arc, rel_full));
            size = s > 0 ? (uint64_t)s : 0;
        }
    } else {
        for (;;) {
            if (!d->first && !FindNextFileA(d->find, &d->data)) { r0 = FIOS_ERR_NOT_FOUND; return; }
            d->first = 0;
            if (strcmp(d->data.cFileName, ".") && strcmp(d->data.cFileName, "..")) break;
        }
        snprintf(name, sizeof(name), "%s", d->data.cFileName);
        size = (uint64_t)d->data.nFileSizeHigh << 32 | d->data.nFileSizeLow;
        is_dir = (d->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    char full[1024];
    int n = snprintf(full, sizeof(full), "%s/%s", d->base, name);
    memset(vita_mem_ptr(out, 0x418), 0, 0x418);
    vita_write32(out, (uint32_t)size);
    vita_write32(out + 4, (uint32_t)(size >> 32));
    vita_write32(out + 8, is_dir ? 0x7u : 0x6u);
    vita_write16(out + 12, (uint16_t)strlen(name));
    vita_write16(out + 14, (uint16_t)n);
    vita_write16(out + 16, (uint16_t)(strlen(d->base) + 1));
    memcpy(vita_mem_ptr(out + 24, (uint32_t)n + 1), full, (size_t)n + 1);
    r0 = 0;
}

void vita_hle_sceFiosDHCloseSync(void) {
    uint32_t id = r1;
    if (id && id < 64 && g_dirs[id].used) {
        if (g_dirs[id].arc < 0 && g_dirs[id].find) FindClose(g_dirs[id].find);
        g_dirs[id].used = 0;
    }
    r0 = 0;
}

void vita_hle_sceFiosDeleteSync(void) { r0 = 0; }
void vita_hle_sceFiosDirectoryCreateSync(void) {
    char guest[512];
    port_guest_str(r1, guest, sizeof(guest));
    Loc l;
    if (fios_resolve(guest, &l) && l.arc < 0 && l.host[0]) { port_vfs_mkdirs(l.host); CreateDirectoryA(l.host, NULL); }
    r0 = 0;
}
void vita_hle_sceFiosDirectoryDeleteSync(void) { r0 = 0; }

/* Operaciones (ya terminadas) */
void vita_hle_sceFiosOpIsDone(void) { r0 = 1; }
void vita_hle_sceFiosOpGetError(void) { FiosOp *o = op_get(r0); r0 = o ? (uint32_t)o->error : 0; }
void vita_hle_sceFiosOpGetActualCount(void) { FiosOp *o = op_get(r0); ret64(o ? o->actual : 0); }
void vita_hle_sceFiosOpGetRequestCount(void) { FiosOp *o = op_get(r0); ret64(o ? o->actual : 0); }
void vita_hle_sceFiosOpDelete(void) { FiosOp *o = op_get(r0); if (o) o->used = 0; r0 = 0; }

void vita_hle_sceFiosGetAllOps(void) { r0 = 0; }
void vita_hle_sceFiosGetAllFHs(void) { r0 = 0; }
void vita_hle_sceFiosDebugDumpFH(void) { }
void vita_hle_sceFiosDebugDumpOp(void) { }
