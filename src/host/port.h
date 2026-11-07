/* Interfaz interna del anfitrión del port de MLB 15. */
#ifndef MLB15_PORT_H
#define MLB15_PORT_H

#include <stdint.h>

/* Memoria del kernel (bloques de sceKernelAllocMemBlock, pilas, TLS). */
void port_memblock_init(uint32_t main_base, uint32_t main_end,
                        uint32_t vram_end, uint32_t port_end);
uint32_t port_alloc(uint32_t size, uint32_t align, const char *name);

/* Hilos y objetos de sincronización. */
void port_objs_init(void);
void port_threads_init(uint32_t main_stack_base, uint32_t main_stack_size);
int port_current_thread(void);
const char *port_current_thread_name(void);

/* Sistema de archivos virtual (app0:, ux0:, savedata0:...). */
#include <stddef.h>
void port_vfs_init(void);
int port_vfs_path(const char *guest, char *out, size_t max);
void port_vfs_mkdirs(const char *host_path);

/* Utilidades de memoria guest. */
void port_guest_str(uint32_t addr, char *out, size_t max);

#endif
