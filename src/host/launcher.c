/* Arranque "como un port de PC": encuentra el juego del usuario, lo comprueba,
 * guarda la configuración en MLB15.ini y deja listo el ELF que carga main.c.
 *
 * Se pide la CARPETA DEL JUEGO (PCSA00511, la de los .arc; con Vita3K es
 * <pref-path>\ux0\app\PCSA00511). Su eboot.bin suele estar cifrado y el port no
 * descifra nada, así que además hace falta una copia descifrada: la de la
 * carpeta del juego si ya está en claro (volcado NoNpDrm), una junto al exe, o
 * la que el usuario elija una vez (Vita3K la deja con "Decrypt All SELF" en
 * <carpeta de Vita3K>\cache\decrypted_selfs\PCSA00511\eboot.bin). Convertirla a
 * ELF (datos\eboot.elf) es solo copiar segmentos.
 *
 * MLB15.ini: [juego] carpeta, eboot; [video] (ventana de opciones, opciones.c);
 * [depuracion] consola.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <shlobj.h>
#include <io.h>

#include "inflate.h"
#include "launcher.h"
#include "recompilar.h"

#define TITLE_ID     "PCSA00511"
#define ELF_SIZE     18058056u          /* MLB 15 The Show (PCSA00511), versión instalada */

static char g_exe_dir[MAX_PATH], g_ini[MAX_PATH], g_dll[MAX_PATH];

const char *launcher_juego_dll(void) { return g_dll; }

/* datos\arranque.txt: registro corto e independiente de stdout (lo abre y cierra en cada línea).
 * El log.txt del otro PC del usuario llegó lleno de datos binarios y sin texto; esto queda siempre. */
#include <stdarg.h>
void arranque(const char *fmt, ...) {
    char p[MAX_PATH];
    if (!g_exe_dir[0]) {
        GetModuleFileNameA(NULL, g_exe_dir, sizeof(g_exe_dir));
        char *sl = strrchr(g_exe_dir, '\\');
        if (sl) *sl = 0;
    }
    snprintf(p, sizeof(p), "%s\\datos", g_exe_dir);
    CreateDirectoryA(p, NULL);
    snprintf(p, sizeof(p), "%s\\datos\\arranque.txt", g_exe_dir);
    FILE *f = fopen(p, "a");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "%02u:%02u:%02u ", t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ------------------------------------------------------------------------- */
/* Mensajes (los literales están en UTF-8)                                    */
/* ------------------------------------------------------------------------- */

static void to_wide(const char *s, wchar_t *w, int n) {
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
}

static int msg(const char *text, UINT flags) {
    wchar_t wt[2048], wc[64];
    to_wide(text, wt, 2048);
    to_wide("Project Baseball", wc, 64);
    printf("[launcher] %s\n", text);
    fflush(stdout);
    if (getenv("MLB15_NOWINDOW")) return IDCANCEL;      /* pruebas sin ventana: no preguntar */
    return MessageBoxW(NULL, wt, wc, flags);
}

static int is_file(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static int is_dir(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* ------------------------------------------------------------------------- */
/* Vita3K                                                                      */
/* ------------------------------------------------------------------------- */

/* ¿Es la carpeta del juego? (la de PCSA00511: los dos .arc) */
static int game_dir_ok(const char *dir) {
    char t[MAX_PATH];
    snprintf(t, sizeof(t), "%s\\VITA_COMPRESSED_WAD.arc", dir);
    return is_dir(dir) && is_file(t);
}

/* ¿Los .arc están en claro? Una copia hecha desde la Vita sin descifrar (o un volcado que conserva
 * el cifrado de la tarjeta, con carpeta sce_pfs) tiene los archivos cifrados: el juego no puede
 * abrirlos y se queda en negro (otro PC del usuario, 2026-11-07). Un PSARC empieza por "PSAR". */
static int arc_en_claro(const char *dir) {
    char t[MAX_PATH];
    snprintf(t, sizeof(t), "%s\\VITA_COMPRESSED_WAD.arc", dir);
    FILE *f = fopen(t, "rb");
    if (!f) return 0;
    char h[4] = { 0 };
    size_t n = fread(h, 1, 4, f);
    fclose(f);
    return n == 4 && !memcmp(h, "PSAR", 4);
}

/* ¿Es un SELF con todos sus segmentos en claro? (mira solo la cabecera) */
static int self_plain(const char *path) {
    uint8_t h[0x1000];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(h, 1, sizeof(h), f);
    fclose(f);
    if (n < 0x80 || memcmp(h, "SCE\0", 4)) return 0;
    uint64_t eh, si;
    memcpy(&eh, h + 0x20 + 8 * 4, 8);
    memcpy(&si, h + 0x20 + 8 * 7, 8);
    if (eh + 0x30 > n) return 0;
    uint16_t phnum;
    memcpy(&phnum, h + eh + 0x2C, 2);
    if (!phnum || si + 32ull * phnum > n) return 0;
    for (uint16_t i = 0; i < phnum; i++) {
        uint64_t enc;
        memcpy(&enc, h + si + 32u * i + 24, 8);
        if (enc != 2) return 0;
    }
    return 1;
}

/* ¿Es el ELF del juego en claro (FAGDec, o el datos\eboot.elf ya convertido)? */
static int elf_ok(const char *path) {
    if (!path || !path[0]) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t h[4] = { 0 };
    size_t n = fread(h, 1, 4, f);
    _fseeki64(f, 0, SEEK_END);
    long long t = _ftelli64(f);
    fclose(f);
    if (n != 4 || memcmp(h, "\x7f" "ELF", 4)) return 0;
    if (t != (long long)ELF_SIZE) printf("[launcher] aviso: %s mide %lld bytes y se esperaban %u (¿otra versión?)\n", path, t, ELF_SIZE);
    return t > 0x10000;
}

/* El eboot.bin descifrado: el de la carpeta del juego si ya está en claro
 * (p. ej. un volcado con NoNpDrm), o uno junto a MLB15.exe. */
static int find_eboot(const char *game, char *out, size_t max) {
    char t[MAX_PATH];
    snprintf(t, sizeof(t), "%s\\eboot.bin", game);
    if (self_plain(t)) { snprintf(out, max, "%s", t); return 1; }
    snprintf(t, sizeof(t), "%s\\eboot.bin", g_exe_dir);
    if (self_plain(t)) { snprintf(out, max, "%s", t); return 1; }
    snprintf(t, sizeof(t), "%s\\eboot.elf", g_exe_dir);   /* FAGDec */
    if (elf_ok(t)) { snprintf(out, max, "%s", t); return 1; }
    snprintf(t, sizeof(t), "%s\\eboot.elf", game);
    if (elf_ok(t)) { snprintf(out, max, "%s", t); return 1; }
    return 0;
}

/* Diálogo para elegir un archivo eboot.bin. */
static int pick_file(char *out, size_t max) {
    if (getenv("MLB15_NOWINDOW")) return 0;
    wchar_t path[MAX_PATH] = L"eboot.bin", title[160], filter[64];
    to_wide("Elige el eboot.bin DESCIFRADO de MLB 15 The Show", title, 160);
    memcpy(filter, L"eboot.bin / eboot.elf\0eboot.bin;eboot.elf;*.elf\0Todos\0*.*\0\0", sizeof(L"eboot.bin / eboot.elf\0eboot.bin;eboot.elf;*.elf\0Todos\0*.*\0\0"));
    OPENFILENAMEW of;
    memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of);
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH;
    of.lpstrFilter = filter;
    of.lpstrTitle = title;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&of)) return 0;
    return WideCharToMultiByte(CP_ACP, 0, path, -1, out, (int)max, NULL, NULL) > 0;
}

static int pick_folder(char *out, size_t max) {
    if (getenv("MLB15_NOWINDOW")) return 0;
    wchar_t title[256], path[MAX_PATH];
    to_wide("Elige la carpeta del juego: PCSA00511 (la que contiene VITA_COMPRESSED_WAD.arc)", title, 256);
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof(bi));
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
    int ok = id && SHGetPathFromIDListW(id, path);
    if (id) CoTaskMemFree(id);
    if (!ok) return 0;
    return WideCharToMultiByte(CP_ACP, 0, path, -1, out, (int)max, NULL, NULL) > 0;
}

/* ------------------------------------------------------------------------- */
/* SELF descifrado -> ELF                                                     */
/* ------------------------------------------------------------------------- */

static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static uint8_t *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    _fseeki64(f, 0, SEEK_END);
    long long n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d) *len = (size_t)n;
    return d;
}

/* Formato (little-endian): "SCE\0", +0x18 u64 tamaño del ELF; cabecera extendida
 * desde 0x20 con u64: [4] cabecera ELF, [5] phdr, [7] tabla de segmentos (por
 * phdr: desplazamiento, longitud, compresión 1/2, cifrado 1 = sí, 2 = no). */
static int self_to_elf(const char *self, const char *elf_out, char *why, size_t max) {
    size_t n = 0;
    uint8_t *d = read_all(self, &n);
    if (!d) { snprintf(why, max, "No se pudo leer %s", self); return 1; }
    int err = 1;
    uint8_t *e = NULL;
    if (n < 0x80 || memcmp(d, "SCE\0", 4)) { snprintf(why, max, "%s no es un SELF.", self); goto out; }
    uint64_t elf_size = rd64(d + 0x18);
    uint64_t eh = rd64(d + 0x20 + 8 * 4), ph = rd64(d + 0x20 + 8 * 5), si = rd64(d + 0x20 + 8 * 7);
    if (!elf_size || elf_size > (256u << 20) || eh + 52 > n || memcmp(d + eh, "\x7f" "ELF", 4)) {
        snprintf(why, max, "Cabecera del SELF no reconocida: %s", self);
        goto out;
    }
    uint32_t phoff = rd32(d + eh + 0x1C);
    uint16_t phnum = rd16(d + eh + 0x2C);
    if (ph + 32ull * phnum > n || si + 32ull * phnum > n || phoff + 32ull * phnum > elf_size) {
        snprintf(why, max, "Tabla de segmentos fuera del archivo: %s", self);
        goto out;
    }
    e = (uint8_t *)calloc(1, (size_t)elf_size);
    if (!e) { snprintf(why, max, "Sin memoria"); goto out; }
    memcpy(e, d + eh, 52);
    memcpy(e + phoff, d + ph, 32u * phnum);
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *p = d + ph + 32u * i, *s = d + si + 32u * i;
        uint32_t p_off = rd32(p + 4), p_filesz = rd32(p + 16);
        uint64_t off = rd64(s), len = rd64(s + 8), comp = rd64(s + 16), enc = rd64(s + 24);
        if (!p_filesz) continue;
        if (enc != 2) {
            snprintf(why, max, "Este eboot.bin sigue cifrado (segmento %u):\n%s\n\n"
                     "Usa \"Decrypt All SELF\" en Vita3K con el juego.", i, self);
            goto out;
        }
        if (off + len > n || (uint64_t)p_off + p_filesz > elf_size) {
            snprintf(why, max, "Segmento %u fuera de rango en %s", i, self);
            goto out;
        }
        if (comp == 2) {
            if (zlib_inflate(e + p_off, p_filesz, d + off, (size_t)len) != (long)p_filesz) {
                snprintf(why, max, "No se pudo descomprimir el segmento %u de %s", i, self);
                goto out;
            }
        } else {
            memcpy(e + p_off, d + off, (size_t)(len < p_filesz ? len : p_filesz));
        }
    }
    if (elf_size != ELF_SIZE)
        printf("[launcher] aviso: el ELF mide %llu bytes y se esperaban %u (¿otra versión del juego?)\n",
               (unsigned long long)elf_size, ELF_SIZE);
    {
        char tmp[MAX_PATH];
        snprintf(tmp, sizeof(tmp), "%s.tmp", elf_out);
        FILE *f = fopen(tmp, "wb");
        if (!f || fwrite(e, 1, (size_t)elf_size, f) != (size_t)elf_size) {
            if (f) fclose(f);
            snprintf(why, max, "No se pudo escribir %s", tmp);
            goto out;
        }
        fclose(f);
        if (!MoveFileExA(tmp, elf_out, MOVEFILE_REPLACE_EXISTING)) { snprintf(why, max, "No se pudo crear %s", elf_out); goto out; }
    }
    printf("[launcher] %s -> %s (%llu bytes)\n", self, elf_out, (unsigned long long)elf_size);
    err = 0;
out:
    free(e);
    free(d);
    return err;
}

/* ¿Hay que regenerar el ELF? Si falta o si el eboot.bin es más nuevo. */
static int elf_stale(const char *eboot, const char *elf) {
    WIN32_FILE_ATTRIBUTE_DATA a, b;
    if (!GetFileAttributesExA(elf, GetFileExInfoStandard, &b)) return 1;
    if (!GetFileAttributesExA(eboot, GetFileExInfoStandard, &a)) return 0;
    return CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) > 0;
}

/* ------------------------------------------------------------------------- */
/* Configuración                                                               */
/* ------------------------------------------------------------------------- */

static void exe_dir(void) {
    if (g_exe_dir[0]) return;
    GetModuleFileNameA(NULL, g_exe_dir, sizeof(g_exe_dir));
    char *slash = strrchr(g_exe_dir, '\\');
    if (slash) *slash = 0;
    snprintf(g_ini, sizeof(g_ini), "%s\\ProjectBaseball.ini", g_exe_dir);
    {   /* antes se llamaba MLB15.ini: se conserva la configuración */
        char viejo[MAX_PATH];
        snprintf(viejo, sizeof(viejo), "%s\\MLB15.ini", g_exe_dir);
        if (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES && GetFileAttributesA(viejo) != INVALID_FILE_ATTRIBUTES)
            CopyFileA(viejo, g_ini, TRUE);
    }
}

const char *launcher_ini_path(void) { exe_dir(); return g_ini; }

int launcher_ini_int(const char *section, const char *key, int def) {
    exe_dir();
    return (int)GetPrivateProfileIntA(section, key, def, g_ini);
}

void launcher_ini_set_int(const char *section, const char *key, int v) {
    char s[32];
    exe_dir();
    snprintf(s, sizeof(s), "%d", v);
    WritePrivateProfileStringA(section, key, s, g_ini);
}

/* Comprueba si se mantiene Shift (para forzar cambio de carpeta del juego). */
static int shift_pressed(void) {
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

int launcher_prepare(char *elf_out, size_t max) {
    exe_dir();
    {   /* cabecera del registro de arranque */
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%s\\datos\\arranque.txt", g_exe_dir);
        DeleteFileA(p);
        MEMORYSTATUSEX ms = { sizeof(ms) }; GlobalMemoryStatusEx(&ms);
        SYSTEM_INFO si; GetSystemInfo(&si);
        arranque("=== Project Baseball (%s) | %lu núcleos | RAM %llu MB (libre %llu MB)", __DATE__, si.dwNumberOfProcessors,
                 ms.ullTotalPhys >> 20, ms.ullAvailPhys >> 20);
    }
    char game[MAX_PATH], eboot[MAX_PATH], why[1024];
    GetPrivateProfileStringA("juego", "carpeta", "", game, sizeof(game), g_ini);
    GetPrivateProfileStringA("juego", "eboot", "", eboot, sizeof(eboot), g_ini);

    /* Shift al arrancar = volver a preguntar la carpeta del juego. */
    int force_pick = shift_pressed();
    if (force_pick) game[0] = 0;   /* invalida la guardada */

    /* 1. La carpeta del juego (PCSA00511, con los .arc descifrados). */
    if (game_dir_ok(game) && !arc_en_claro(game)) {
        snprintf(why, sizeof(why),
                 "Los archivos del juego de esta carpeta están CIFRADOS:\n%s\n\n"
                 "Es una copia sacada de la Vita sin descifrar (suele llevar una carpeta sce_pfs). El port no "
                 "descifra nada, así que el juego se quedaría en negro.\n\n"
                 "Usa la carpeta del juego instalada en Vita3K (ux0\\app\\PCSA00511), o cópiala desde tu Vita con "
                 "VitaShell usando \"Open decrypted\".\n\nA continuación elige otra carpeta.", game);
        printf("[launcher] carpeta con los .arc cifrados: %s\n", game);
        arranque("carpeta del juego con los .arc CIFRADOS: %s", game);
        if (msg(why, MB_OKCANCEL | MB_ICONWARNING) != IDOK) return 1;
        game[0] = 0;
    }
    if (!game_dir_ok(game)) {
        if (msg(game[0] ? "Ya no encuentro el juego en la carpeta guardada. Elígela otra vez."
                        : "Bienvenido. Para jugar necesito tu copia de MLB 15 The Show.\n\n"
                          "A continuación elige la carpeta del juego: PCSA00511 "
                          "(la que contiene VITA_COMPRESSED_WAD.arc).",
                MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
            return 1;
        for (;;) {
            if (!pick_folder(game, sizeof(game))) return 1;
            if (game_dir_ok(game) && !arc_en_claro(game)) {
                snprintf(why, sizeof(why), "Los archivos del juego de esa carpeta están CIFRADOS (copia de la Vita sin descifrar):\n%s\n\n"
                         "Usa la carpeta instalada en Vita3K o una copiada con VitaShell \"Open decrypted\".", game);
                if (msg(why, MB_OKCANCEL | MB_ICONWARNING) != IDOK) return 1;
                continue;
            }
            if (game_dir_ok(game)) break;
            snprintf(why, sizeof(why), "Esa carpeta no es la del juego:\n%s\n\n"
                     "Elige la carpeta PCSA00511, la que contiene VITA_COMPRESSED_WAD.arc "
                     "y VITA_PATCH_WAD.arc.", game);
            if (msg(why, MB_OKCANCEL | MB_ICONWARNING) != IDOK) return 1;
        }
        WritePrivateProfileStringA("juego", "carpeta", game, g_ini);
    }

    /* 2-3. El código del juego en claro -> datos\eboot.elf.
     *    Vale cualquiera de estas (2026-11-07, para probar en otro PC sin Vita3K):
     *      - datos\eboot.elf ya convertido y del tamaño esperado: no se pide nada más;
     *      - un eboot.bin DESCIFRADO (SELF en claro: Vita3K "Decrypt All SELF", NoNpDrm);
     *      - un eboot.elf (el que saca FAGDec en una Vita con homebrew).
     *    Se buscan solos en la carpeta del juego y junto al exe; si no, se pide uno. */
    char datos[MAX_PATH];
    snprintf(datos, sizeof(datos), "%s\\datos", g_exe_dir);
    CreateDirectoryA(datos, NULL);
    snprintf(elf_out, max, "%s\\eboot.elf", datos);
    int listo = elf_ok(elf_out);
    if (!self_plain(eboot) && !elf_ok(eboot) && !find_eboot(game, eboot, sizeof(eboot)) && !listo) {
        if (msg("Falta el código del juego DESCIFRADO (el port no descifra nada). Se necesita una sola vez.\n\n"
                "Con Vita3K: clic derecho sobre MLB 15 The Show y \"Decrypt All SELF\"; queda en la carpeta de "
                "Vita3K, en cache\\decrypted_selfs\\PCSA00511\\eboot.bin.\n\n"
                "Con una PS Vita con homebrew: el eboot.bin o eboot.elf que saca FAGDec.\n\n"
                "También vale copiarlo junto a ProjectBaseball.exe. A continuación elige ese archivo.",
                MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
            return 1;
        for (;;) {
            if (!pick_file(eboot, sizeof(eboot))) return 1;
            if (self_plain(eboot) || elf_ok(eboot)) break;
            snprintf(why, sizeof(why), "Ese archivo no es el código del juego descifrado:\n%s\n\n"
                     "El eboot.bin de la carpeta del juego está cifrado: elige el descifrado (Vita3K o FAGDec).", eboot);
            if (msg(why, MB_OKCANCEL | MB_ICONWARNING) != IDOK) return 1;
        }
    }
    if (self_plain(eboot) || elf_ok(eboot)) {
        WritePrivateProfileStringA("juego", "eboot", eboot, g_ini);
        if (!listo || elf_stale(eboot, elf_out)) {
            if (elf_ok(eboot)) {
                if (_stricmp(eboot, elf_out) && !CopyFileA(eboot, elf_out, FALSE)) {
                    snprintf(why, sizeof(why), "No se pudo copiar %s a %s", eboot, elf_out);
                    msg(why, MB_OK | MB_ICONERROR);
                    return 1;
                }
            } else if (self_to_elf(eboot, elf_out, why, sizeof(why))) {
                msg(why, MB_OK | MB_ICONERROR);
                return 1;
            }
        }
    } else
        printf("[launcher] eboot.bin no encontrado: se usa el datos\\eboot.elf que ya hay\n");
    /* 4. El código del juego (datos\juego.dll): se genera aquí la primera vez a partir de este
     *    ELF (recompilar.c); el exe no lleva nada del juego. */
    {   /* Vulkan: sin él no se dibuja nada (antes, ventana negra sin aviso). Se mira antes de
         * preparar el juego para no esperar minutos en balde. */
        extern int vk_comprobar(char *por_que, size_t max, char *tarjeta, size_t tmax);
        char vk_por[512], vk_tar[256];
        if (!getenv("MLB15_NOWINDOW") && !vk_comprobar(vk_por, sizeof(vk_por), vk_tar, sizeof(vk_tar))) {
            printf("[launcher] sin Vulkan: %s\n", vk_por);
            arranque("SIN VULKAN: %s", vk_por);
            snprintf(why, sizeof(why),
                     "Este PC no puede mostrar el juego: %s.\n\n"
                     "El port dibuja con Vulkan. Casi todas las tarjetas de los últimos años lo tienen "
                     "(también los gráficos Intel de 6ª generación en adelante), pero hace falta un driver "
                     "reciente: los de los fabricantes de portátiles suelen ser antiguos.\n\n"
                     "Qué hacer: instala el último driver de la tarjeta desde la web de Intel, NVIDIA o AMD "
                     "(con Intel, el «Intel Driver & Support Assistant» lo hace solo), reinicia y vuelve a abrir Project Baseball.\n\n"
                     "¿Seguir de todos modos?", vk_por);
            if (msg(why, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return 1;
        } else if (!getenv("MLB15_NOWINDOW")) {
            /* Solo gráficos integrados y sin resolución elegida: empezar ligero (x1, 30 FPS). A x3
             * (2880x1632 internos) una Intel UHD tarda tanto en cada fotograma que parece colgado. */
            extern int vk_hay_dedicada;
            if (!vk_hay_dedicada && launcher_ini_int("video", "resolucion", -1) < 0) {
                launcher_ini_set_int("video", "resolucion", 1);
                launcher_ini_set_int("video", "fps_max", 30);
                printf("[launcher] solo gráficos integrados (%s): resolución x1 y 30 FPS por defecto\n", vk_tar);
            }
        }
    }
    if (recompilar_si_hace_falta(g_exe_dir, elf_out, g_dll, sizeof(g_dll)) != 0) return 1;
    /* 5. Opciones gráficas (opciones.c): Salir cierra sin jugar. */
    if (!opciones_dialogo()) return 1;
    /* app0: para vfs.c (MLB15_APP0 manda si el usuario ya la definió) */
    if (!getenv("MLB15_APP0")) _putenv_s("MLB15_APP0", game);
    printf("[launcher] juego: %s\n[launcher] eboot: %s\n", game, eboot);
    arranque("juego: %s | eboot: %s | elf: %s", game, eboot[0] ? eboot : "(no hace falta)", elf_out);
    return 0;
}

/* Doble clic: Windows crea una consola solo para nosotros (somos su único
 * proceso). Entonces el registro va a datos\log.txt y la consola se cierra;
 * desde una terminal se sigue viendo como siempre. [depuracion] consola=1 la
 * conserva. */
void launcher_console(void) {
    DWORD ids[4];
    if (GetConsoleProcessList(ids, 4) != 1 || launcher_ini_int("depuracion", "consola", 0)) return;
    char path[MAX_PATH];
    exe_dir();
    snprintf(path, sizeof(path), "%s\\datos", g_exe_dir);
    CreateDirectoryA(path, NULL);
    snprintf(path, sizeof(path), "%s\\datos\\log.txt", g_exe_dir);
    /* Orden importante: primero soltar la consola y después redirigir. Con
     * freopen antes de FreeConsole el archivo se quedaba vacío (probado). */
    FreeConsole();
    if (freopen(path, "w", stdout)) {
        /* stderr al MISMO descriptor: dos flujos sobre el archivo se pisaban. */
        _dup2(_fileno(stdout), _fileno(stderr));
        /* Sin búfer: si el port se cierra de golpe (trampa, fallo), lo último
         * que escribió es justo lo que explica por qué. */
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
    }
}
