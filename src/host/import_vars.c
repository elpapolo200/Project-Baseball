/* Variables importadas del firmware (_Ctype, _Touptab, _Stdout, ...).
 *
 * armrecomp da a cada una una celda en memoria guest (vita_import_vars) y emite
 * los movw/movt que la usaban con esa dirección; aquí se rellena cada celda,
 * como haría el cargador de la Vita. Sin esto el juego leía las tablas de
 * mayúsculas de bytes al azar: "BOOT/CONFIG_MAIN.TXT" salía "CONF$G_r$$N.TXT".
 */
#include <stdint.h>
#include <stdio.h>

#include "vitarecomp/mem.h"
#include "vitarecomp/recomp_rt.h"

#include "port.h"

#define NID_TOUPTAB 0x36878958u
#define NID_CTYPE   0x3CE6109Du
#define NID_STDOUT  0x5D8C1282u
#define NID_TOLOTAB 0xD662E07Cu
#define NID_STDERR  0xDDF9BB09u
#define NID_BADOFF  0x9C449444u
#define NID_FPZ     0xD9478DCFu
#define NID_STKCHK  0x93B8AA67u

/* Bits de clasificación de la libc Dinkumware (la del SDK de la Vita). */
#define _XD 0x01
#define _UP 0x02
#define _SP 0x04
#define _PU 0x08
#define _LO 0x10
#define _DI 0x20
#define _CN 0x40
#define _BB 0x80

/* Tablas de short con índice -128..255 (char con signo, EOF y unsigned char);
 * la variable apunta a la entrada 0. */
#define TAB_ENTRIES 384
#define TAB_ZERO    128

static uint16_t ctype_of(int c) {
    if (c < 0 || c > 127) return 0;
    uint16_t f = 0;
    if (c >= 9 && c <= 13) f |= _CN;
    else if (c < 32 || c == 127) f |= _BB;
    if (c == ' ') f |= _SP;
    if (c >= '0' && c <= '9') f |= _DI | _XD;
    if (c >= 'A' && c <= 'Z') f |= _UP;
    if (c >= 'a' && c <= 'z') f |= _LO;
    if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) f |= _XD;
    if (c > 32 && c < 127 && !(f & (_DI | _UP | _LO))) f |= _PU;
    return f;
}

/* Escribe una tabla en `at` y devuelve la dirección de su entrada 0. */
static uint32_t put_table(uint32_t at, int kind) {
    for (int i = 0; i < TAB_ENTRIES; i++) {
        int c = i - TAB_ZERO;
        int v;
        if (kind == 0)      v = ctype_of(c);
        else if (kind == 1) v = (c >= 'a' && c <= 'z') ? c - 32 : c;   /* toupper */
        else                v = (c >= 'A' && c <= 'Z') ? c + 32 : c;   /* tolower */
        vita_write16(at + (uint32_t)i * 2, (uint16_t)v);
    }
    return at + TAB_ZERO * 2;
}

void port_stdio_alias(uint32_t out, uint32_t err);

#include "juego_dll.h"   /* las celdas las define el código del juego (juego.dll) */
#define vita_import_vars juego_import_vars
#define vita_import_var_count juego_import_var_count

void port_import_vars(void) {
    /* Las tablas van justo detrás de las celdas (hueco entre el ELF y el heap). */
    uint32_t tabs = 0;
    for (uint32_t i = 0; i < vita_import_var_count; i++)
        if (vita_import_vars[i].cell + 32 > tabs) tabs = vita_import_vars[i].cell + 32;
    tabs = (tabs + 0xFFFu) & ~0xFFFu;

    uint32_t out = 0, err = 0;
    for (uint32_t i = 0; i < vita_import_var_count; i++) {
        uint32_t cell = vita_import_vars[i].cell, nid = vita_import_vars[i].nid;
        const char *what = "desconocida (celda a cero)";
        for (int k = 0; k < 32; k += 4) vita_write32(cell + (uint32_t)k, 0);
        switch (nid) {
            case NID_CTYPE:   vita_write32(cell, put_table(tabs, 0));          tabs += 0x400; what = "_Ctype"; break;
            case NID_TOUPTAB: vita_write32(cell, put_table(tabs, 1));          tabs += 0x400; what = "_Touptab"; break;
            case NID_TOLOTAB: vita_write32(cell, put_table(tabs, 2));          tabs += 0x400; what = "_Tolotab"; break;
            /* _Stdout/_Stderr SON el FILE: su dirección es la que se pasa a fputs. */
            case NID_STDOUT:  out = cell; what = "_Stdout"; break;
            case NID_STDERR:  err = cell; what = "_Stderr"; break;
            case NID_BADOFF:  vita_write32(cell, 0xFFFFFFFFu); vita_write32(cell + 4, 0xFFFFFFFFu); what = "_BADOFF = -1"; break;
            case NID_FPZ:     what = "_Fpz = 0"; break;
            case NID_STKCHK:  vita_write32(cell, 0x7E5A11C3u); what = "__stack_chk_guard"; break;
        }
        printf("[vars] 0x%08X NID 0x%08X %s\n", cell, nid, what);
    }
    port_stdio_alias(out, err);
    fflush(stdout);
}
