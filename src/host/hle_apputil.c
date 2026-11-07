/* SceAppUtil para el port de MLB 15.
 *
 * En el arranque el juego pide parametros del sistema (idioma, boton de
 * confirmar, nombre de usuario) y consulta eventos de la aplicacion. Aqui se
 * responde como una consola recien inicializada: idioma ingles, boton de
 * confirmar el de siempre y sin eventos pendientes.
 *
 * El guardado (SaveData) va sobre la carpeta datos\ (ver más abajo).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"

#include "port.h"

static uint32_t arg(int i) { return vita_read32(sp + 4u * (uint32_t)i); }

#define APPUTIL_ERROR ((uint32_t)-1)

static void put_str(uint32_t dst, uint32_t size, const char *s) {
    if (!dst || !size) return;
    uint32_t n = (uint32_t)strlen(s);
    if (n >= size) n = size - 1;
    char *p = (char *)vita_mem_ptr(dst, size);
    if (!p) return;
    memcpy(p, s, n);
    p[n] = 0;
}

void vita_hle_sceAppUtilInit(void) {
    static int once;
    if (!once) { once = 1; printf("[apputil] Init\n"); fflush(stdout); }
    r0 = 0;
}
void vita_hle_sceAppUtilTerm(void) { r0 = 0; }

void vita_hle_sceAppUtilSystemParamGetString(void) {
    /* r0 = paramId, r1 = buffer, r2 = tamano */
    uint32_t id = r0, buf = r1, size = r2;
    const char *val = "";
    switch (id) {
        case 1: val = "en_US"; break;        /* idioma                      */
        case 3: val = "Player"; break;       /* nombre de usuario           */
        default: val = ""; break;
    }
    put_str(buf, size, val);
    printf("[apputil] SystemParamGetString(id=%u) -> \"%s\"\n", id, val);
    fflush(stdout);
    r0 = 0;
}

void vita_hle_sceAppUtilSystemParamGetInt(void) {
    /* r0 = paramId, r1 = int* */
    uint32_t id = r0, out = r1;
    int v = 0;
    switch (id) {
        case 1: v = 0; break;    /* idioma: 0 = ingles      */
        case 2: v = 1; break;    /* boton de confirmar: cruz */
        default: v = 0; break;
    }
    if (vita_mem_ptr(out, 4)) vita_write32(out, (uint32_t)v);
    r0 = 0;
}

void vita_hle_sceAppUtilReceiveAppEvent(void) {
    /* r0 = int* event; sin eventos pendientes. */
    if (vita_mem_ptr(r0, 4)) vita_write32(r0, 0);
    r0 = 0;
}
void vita_hle_sceAppUtilAppEventParseNpInviteMessage(void) { r0 = APPUTIL_ERROR; }

/* --- guardado -------------------------------------------------------------- */
/* savedata0: ya apunta a datos\ (vfs.c): los archivos del juego van ahí tal
 * cual. Los "slots" (título, detalle, fecha que muestra el diálogo de
 * guardado) se guardan como el SceAppUtilSaveDataSlotParam en bruto en
 * datos\sce_slots\slot_<id>.bin.
 *
 * Sin esto, al entrar en Play Now el juego quiere guardar los ajustes,
 * GetQuota fallaba ("Error trying to retrieve free space.") y se quedaba
 * esperando para siempre. */

#define SLOT_PARAM_SIZE 0x348u                 /* status, title[64], subTitle[128], detail[512], iconPath[64], userParam, modifiedTime, reserved[48] */
#define ERR_SLOT_EXISTS    0x80100660u
#define ERR_SLOT_NOT_FOUND 0x80100661u
#define QUOTA_KIB          (1024u * 1024u)       /* 1 GB: de sobra */

static int sd_trace(void) {
    static int t = -1;
    if (t < 0) t = getenv("MLB15_SAVE_TRACE") ? 1 : 0;
    return t;
}

static void slot_path(uint32_t id, char *out, size_t max) {
    char g[64];
    snprintf(g, sizeof(g), "savedata0:sce_slots/slot_%u.bin", id);
    port_vfs_path(g, out, max);
}

static int write_file(const char *path, const void *buf, uint32_t len, int64_t off, int truncate) {
    port_vfs_mkdirs(path);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, truncate ? CREATE_ALWAYS : OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER li;
    li.QuadPart = off > 0 ? off : 0;
    SetFilePointerEx(h, li, NULL, FILE_BEGIN);
    DWORD w = 0;
    int ok = !len || (WriteFile(h, buf, len, &w, NULL) && w == len);
    CloseHandle(h);
    return ok;
}

static int slot_exists(uint32_t id) {
    char p[MAX_PATH];
    slot_path(id, p, sizeof(p));
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

static int slot_write(uint32_t id, uint32_t param) {
    char p[MAX_PATH];
    slot_path(id, p, sizeof(p));
    uint8_t blank[SLOT_PARAM_SIZE];
    memset(blank, 0, sizeof(blank));
    const void *src = param ? vita_mem_ptr(param, SLOT_PARAM_SIZE) : NULL;
    return write_file(p, src ? src : blank, SLOT_PARAM_SIZE, 0, 1);
}

/* El primer campo del SlotParam es el estado (0 = disponible, 1 = dañado) y lo
 * lleva el sistema: el juego crea la ranura con 1 y es el sistema quien la deja
 * en 0 cuando el guardado termina bien. Si se quedaba en 1, en el siguiente
 * arranque el juego no leía los datos: borraba la ranura y la creaba de nuevo. */
static void slot_mark_ok(uint32_t id) {
    char p[MAX_PATH];
    slot_path(id, p, sizeof(p));
    static const uint8_t zero[4] = { 0 };
    write_file(p, zero, 4, 0, 0);
}

void vita_hle_sceAppUtilSaveDataMount(void) { r0 = 0; }
void vita_hle_sceAppUtilSaveDataUmount(void) { r0 = 0; }

/* (slotId, SlotParam *, mountPoint) */
void vita_hle_sceAppUtilSaveDataSlotCreate(void) {
    uint32_t id = r0, param = r1;
    if (sd_trace()) printf("[apputil] SlotCreate(%u)\n", id);
    if (slot_exists(id)) { r0 = ERR_SLOT_EXISTS; return; }
    r0 = slot_write(id, param) ? 0 : APPUTIL_ERROR;
}

void vita_hle_sceAppUtilSaveDataSlotDelete(void) {
    char p[MAX_PATH];
    slot_path(r0, p, sizeof(p));
    if (sd_trace()) printf("[apputil] SlotDelete(%u)\n", r0);
    r0 = DeleteFileA(p) ? 0 : ERR_SLOT_NOT_FOUND;
}

void vita_hle_sceAppUtilSaveDataSlotGetParam(void) {
    uint32_t id = r0, param = r1;
    char p[MAX_PATH];
    slot_path(id, p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f) { r0 = ERR_SLOT_NOT_FOUND; return; }
    uint8_t *dst = (uint8_t *)vita_mem_ptr(param, SLOT_PARAM_SIZE);
    if (dst) { memset(dst, 0, SLOT_PARAM_SIZE); fread(dst, 1, SLOT_PARAM_SIZE, f); }
    fclose(f);
    if (sd_trace()) printf("[apputil] SlotGetParam(%u) -> ok\n", id);
    r0 = 0;
}

void vita_hle_sceAppUtilSaveDataSlotSetParam(void) {
    uint32_t id = r0, param = r1;
    if (!slot_exists(id)) { r0 = ERR_SLOT_NOT_FOUND; return; }
    r0 = slot_write(id, param) ? 0 : APPUTIL_ERROR;
}

/* (FileSlot *slot, File *files, fileNum, mountPoint, SceSize *requiredSizeKiB)
 *   FileSlot: u32 id, SlotParam *param, reserved[32]
 *   File (0x40): filePath, buf, bufSize, pad, u64 offset (+0x10), mode (+0x18), progDelta, reserved
 *   mode: 0 archivo, 1 archivo truncando, 2 directorio */
void vita_hle_sceAppUtilSaveDataDataSave(void) {
    uint32_t slot = r0, files = r1, n = r2, req = arg(1);
    if (req && vita_mem_ptr(req, 4)) vita_write32(req, 0);
    for (uint32_t i = 0; i < n && i < 256; i++) {
        uint32_t e = files + 0x40u * i;
        char rel[256], g[300], host[MAX_PATH];
        port_guest_str(vita_read32(e), rel, sizeof(rel));
        uint32_t buf = vita_read32(e + 4), size = vita_read32(e + 8), mode = vita_read32(e + 0x18);
        int64_t off = (int64_t)((uint64_t)vita_read32(e + 0x14) << 32 | vita_read32(e + 0x10));
        snprintf(g, sizeof(g), "savedata0:%s", rel);
        if (!port_vfs_path(g, host, sizeof(host))) continue;
        if (mode == 2) { port_vfs_mkdirs(host); CreateDirectoryA(host, NULL); continue; }
        const void *src = size ? vita_mem_ptr(buf, size) : NULL;
        if (size && !src) { r0 = APPUTIL_ERROR; return; }
        if (!write_file(host, src, size, off, mode == 1)) { r0 = APPUTIL_ERROR; return; }
        if (sd_trace()) printf("[apputil] DataSave '%s' %u bytes (modo %u)\n", rel, size, mode);
    }
    if (slot && vita_mem_ptr(slot, 8)) {
        uint32_t id = vita_read32(slot), param = vita_read32(slot + 4);
        if (param || !slot_exists(id)) slot_write(id, param);
        slot_mark_ok(id);
    }
    r0 = 0;
}

static int borrar_arbol(const char *dir) {
    char pat[MAX_PATH], r[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            snprintf(r, sizeof(r), "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) borrar_arbol(r); else DeleteFileA(r);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(dir) != 0;
}

/* (FileSlot *slot, RemoveItem *items, num, mountPoint); RemoveItem (0x30): dataPath, mode, reserved */
void vita_hle_sceAppUtilSaveDataDataRemove(void) {
    uint32_t slot = r0, items = r1, n = r2;
    for (uint32_t i = 0; i < n && i < 256; i++) {
        char rel[256], g[300], host[MAX_PATH];
        port_guest_str(vita_read32(items + 0x30u * i), rel, sizeof(rel));
        uint32_t mode = vita_read32(items + 0x30u * i + 4);
        snprintf(g, sizeof(g), "savedata0:%s", rel);
        if (!port_vfs_path(g, host, sizeof(host))) continue;
        /* Una carpeta se borra con todo lo que tenga (como la Vita). Antes RemoveDirectory fallaba
         * con la carpeta llena: el guardado siguiente escribía encima del archivo viejo sin
         * recortarlo, quedaban bytes del guardado anterior al final y el juego decía «CRC value
         * is not correct, data is corrupt» al cargar (ajustes y perfil se perdían). */
        DWORD at = GetFileAttributesA(host);
        int ok = 0;
        if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) ok = borrar_arbol(host);
        else if (at != INVALID_FILE_ATTRIBUTES) ok = DeleteFileA(host) != 0;
        if (sd_trace()) printf("[apputil] DataRemove '%s' (modo %u) -> %s\n", rel, mode, at == INVALID_FILE_ATTRIBUTES ? "no existe" : ok ? "borrado" : "NO SE PUDO");
    }
    if (slot && vita_mem_ptr(slot, 8)) {
        uint32_t param = vita_read32(slot + 4);
        if (param) slot_write(vita_read32(slot), param);
    }
    r0 = 0;
}

/* (SceSize *quotaKiB, SceSize *usedKiB, mountPoint) */
void vita_hle_sceAppUtilSaveDataGetQuota(void) {
    /* Guardado real por defecto. Tras guardar la primera vez el juego muestra su
     * portada (fondo + pestaña "SWIPE"): se pasa al menú con START (Enter), no
     * con Cruz. MLB15_GUARDADO=0: sin espacio, como antes (el juego avisa "Error
     * trying to retrieve free space", no guarda y sigue al menú). */
    static int on = -1;
    if (on < 0) { const char *e = getenv("MLB15_GUARDADO"); on = e ? atoi(e) != 0 : 1; }
    if (!on) { r0 = APPUTIL_ERROR; return; }
    if (r0 && vita_mem_ptr(r0, 4)) vita_write32(r0, QUOTA_KIB);
    if (r1 && vita_mem_ptr(r1, 4)) vita_write32(r1, 0);
    r0 = 0;
}
void vita_hle_sceAppUtilStoreBrowse(void) { r0 = APPUTIL_ERROR; }
void vita_hle_sceAppUtilDrmOpen(void) { r0 = APPUTIL_ERROR; }
void vita_hle_sceAppUtilDrmClose(void) { r0 = APPUTIL_ERROR; }
