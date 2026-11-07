/* sceIo* (SceIofilemgr / SceLibKernel) sobre archivos del PC, para MLB 15. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"

#include "port.h"

#define SCE_O_RDONLY 0x0001
#define SCE_O_WRONLY 0x0002
#define SCE_O_RDWR   0x0003
#define SCE_O_APPEND 0x0100
#define SCE_O_CREAT  0x0200
#define SCE_O_TRUNC  0x0400
#define IO_ERR_NOENT 0x80010002u
#define IO_ERR_BADF  0x80010009u
#define FD_BASE 0x100

#define MAX_FD 256
static HANDLE g_fd[MAX_FD];

static HANDLE fd_get(uint32_t fd) {
    uint32_t i = fd - FD_BASE;
    return i < MAX_FD ? g_fd[i] : NULL;
}

/* SceUID sceIoOpen(const char *path, int flags, SceMode mode) */
void vita_hle_sceIoOpen(void) {
    char guest[512], host[1024];
    port_guest_str(r0, guest, sizeof(guest));
    uint32_t flags = r1;
    if (!port_vfs_path(guest, host, sizeof(host))) { r0 = IO_ERR_NOENT; return; }
    DWORD access = 0;
    if ((flags & 3) == SCE_O_RDONLY || (flags & 3) == SCE_O_RDWR) access |= GENERIC_READ;
    if ((flags & 3) == SCE_O_WRONLY || (flags & 3) == SCE_O_RDWR) access |= GENERIC_WRITE;
    DWORD disp = OPEN_EXISTING;
    if (flags & SCE_O_CREAT) disp = (flags & SCE_O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
    else if (flags & SCE_O_TRUNC) disp = TRUNCATE_EXISTING;
    if (flags & SCE_O_CREAT) port_vfs_mkdirs(host);
    HANDLE h = CreateFileA(host, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    printf("[io] sceIoOpen('%s', 0x%X) -> %s\n", guest, flags, h == INVALID_HANDLE_VALUE ? "no existe" : "ok");
    if (h == INVALID_HANDLE_VALUE) { r0 = IO_ERR_NOENT; return; }
    if (flags & SCE_O_APPEND) SetFilePointer(h, 0, NULL, FILE_END);
    for (int i = 0; i < MAX_FD; i++) if (!g_fd[i]) { g_fd[i] = h; r0 = FD_BASE + (uint32_t)i; return; }
    CloseHandle(h);
    r0 = IO_ERR_BADF;
}

void vita_hle_sceIoClose(void) {
    HANDLE h = fd_get(r0);
    if (!h) { r0 = IO_ERR_BADF; return; }
    CloseHandle(h);
    g_fd[r0 - FD_BASE] = NULL;
    r0 = 0;
}

void vita_hle_sceIoRead(void) {
    HANDLE h = fd_get(r0);
    DWORD got = 0;
    if (!h) { r0 = IO_ERR_BADF; return; }
    void *dst = r2 ? vita_mem_ptr(r1, r2) : NULL;
    if (r2 && dst) ReadFile(h, dst, r2, &got, NULL);
    static int tr = -1;
    if (tr < 0) tr = getenv("MLB15_IO_TRAZA") ? 1 : 0;
    if (tr || (r2 && !dst)) printf("[io] read(fd %u, 0x%08X, %u) -> %lu%s\n", r0, r1, r2, got, dst ? "" : " (DESTINO FUERA DE LA MEMORIA)");
    r0 = got;
}

void vita_hle_sceIoWrite(void) {
    HANDLE h = fd_get(r0);
    DWORD put = 0;
    if (!h) { r0 = IO_ERR_BADF; return; }
    if (r2) WriteFile(h, vita_mem_ptr(r1, r2), r2, &put, NULL);
    r0 = put;
}

static DWORD whence_of(uint32_t w) { return w == 1 ? FILE_CURRENT : w == 2 ? FILE_END : FILE_BEGIN; }

/* SceOff sceIoLseek(SceUID fd, SceOff offset (r2:r3), int whence (pila)) */
void vita_hle_sceIoLseek(void) {
    HANDLE h = fd_get(r0);
    if (!h) { r0 = IO_ERR_BADF; r1 = 0xFFFFFFFFu; return; }
    LARGE_INTEGER li, out;
    li.QuadPart = (int64_t)((uint64_t)r3 << 32 | r2);
    SetFilePointerEx(h, li, &out, whence_of(vita_read32(sp)));
    r0 = (uint32_t)out.QuadPart;
    r1 = (uint32_t)((uint64_t)out.QuadPart >> 32);
}

/* int sceIoLseek32(SceUID fd, int offset, int whence) */
void vita_hle_sceIoLseek32(void) {
    HANDLE h = fd_get(r0);
    if (!h) { r0 = IO_ERR_BADF; return; }
    r0 = SetFilePointer(h, (LONG)r1, NULL, whence_of(r2));
}

void vita_hle_sceIoMkdir(void) {
    char guest[512], host[1024];
    port_guest_str(r0, guest, sizeof(guest));
    if (port_vfs_path(guest, host, sizeof(host))) { port_vfs_mkdirs(host); CreateDirectoryA(host, NULL); }
    r0 = 0;
}
