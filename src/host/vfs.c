/* Sistema de archivos virtual: traduce rutas de la Vita a carpetas del PC.
 *
 *   app0:          -> carpeta del juego instalado (eboot, WAD...). Por defecto la
 *                     de Vita3K; se puede cambiar con la variable MLB15_APP0.
 *   ux0:, savedata0:, cache0:, host0: -> carpeta "datos" junto al ejecutable
 *                     (partidas guardadas y archivos que escribe el juego).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "port.h"

static char g_app0[MAX_PATH];
static char g_data[MAX_PATH];

void port_vfs_init(void) {
    const char *env = getenv("MLB15_APP0");
    if (env && *env) {
        snprintf(g_app0, sizeof(g_app0), "%s", env);
    } else {
        const char *appdata = getenv("APPDATA");
        snprintf(g_app0, sizeof(g_app0), "%s\\Vita3K\\Vita3K\\ux0\\app\\PCSA00511", appdata ? appdata : ".");
    }
    GetModuleFileNameA(NULL, g_data, sizeof(g_data));
    char *slash = strrchr(g_data, '\\');
    if (slash) *slash = 0;
    strncat(g_data, "\\datos", sizeof(g_data) - strlen(g_data) - 1);
    CreateDirectoryA(g_data, NULL);
    printf("[vfs] app0: = %s\n[vfs] datos = %s\n", g_app0, g_data);
}

/* Convierte "app0:/PSP2/x.bin" en una ruta del PC. Devuelve 0 si no se reconoce. */
int port_vfs_path(const char *guest, char *out, size_t max) {
    const char *colon = strchr(guest, ':');
    const char *rest = colon ? colon + 1 : guest;
    const char *root;
    if (colon && !_strnicmp(guest, "app0:", 5)) root = g_app0;
    else if (colon) root = g_data;          /* ux0:, savedata0:, cache0:... */
    else root = g_app0;                     /* ruta relativa: dentro del juego */
    while (*rest == '/' || *rest == '\\') rest++;
    int n = snprintf(out, max, "%s\\%s", root, rest);
    if (n < 0 || (size_t)n >= max) return 0;
    for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
    return 1;
}

/* Crea las carpetas intermedias de una ruta (para archivos que escribe el juego). */
void port_vfs_mkdirs(const char *host_path) {
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", host_path);
    for (char *p = tmp + 3; *p; p++) {
        if (*p == '\\') { *p = 0; CreateDirectoryA(tmp, NULL); *p = '\\'; }
    }
}
