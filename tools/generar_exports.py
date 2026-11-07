"""Genera dist/exports.def: lo que juego.dll (código del juego) importa de MLB15.exe.

Uso: py -I generar_exports.py <llvm-nm.exe> <salida.def> <objetos del juego...>
Son los símbolos sin definir de las partes que no define el propio DLL (registros, TLS,
memcpy... van en juego_base.c). El exe los exporta (/DEF) y el launcher crea con este
mismo archivo la biblioteca de importación en el PC del jugador (llvm-dlltool).
"""
import subprocess
import sys

PROPIOS = {"__ImageBase", "__chkstk", "__imp_trunc", "_fltused", "_tls_index", "sqrt", "trunc", "truncf",
           "memcpy", "memset", "memmove", "__dyn_tls_on_demand_init", "__tls_guard", "vita_cpu",
           "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr",
           "flag_n", "flag_z", "flag_c", "flag_v", "fpscr_n", "fpscr_z", "fpscr_c", "fpscr_v", "vfp_s"}


def main(argv):
    nm, salida, objs = argv[1], argv[2], argv[3:]
    und, dfn = set(), set()
    for o in objs:
        for l in subprocess.run([nm, o], capture_output=True, text=True).stdout.splitlines():
            p = l.split()
            if len(p) == 2 and p[0] == "U":
                und.add(p[1])
            elif len(p) == 3 and p[1] not in "Uw":
                dfn.add(p[2])
    exp = sorted(und - dfn - PROPIOS)
    with open(salida, "w", newline="\n") as f:
        f.write("; lo que juego.dll importa de MLB15.exe (tools/generar_exports.py)\nEXPORTS\n")
        for s in exp:
            f.write("    %s\n" % s)
    print("%d símbolos exportados" % len(exp))


if __name__ == "__main__":
    main(sys.argv)
