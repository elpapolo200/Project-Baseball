# Project Baseball

Port nativo para PC (Windows) de **MLB 15 The Show** de PS Vita, hecho por recompilación estática:
el código ARM del juego se traduce a C y se compila para x86-64, y las funciones del sistema de la
Vita (gráficos GXM, audio NGS, archivos, guardado, mando...) se reimplementan sobre Windows,
Vulkan y SDL2.

> **Este repositorio no contiene nada del juego.** Ni código, ni gráficos, ni sonidos, ni claves.
> Para jugar hace falta **tu propia copia** de MLB 15 The Show (PCSA00511) descifrada. El código
> del juego se traduce y compila **en tu PC**, a partir de tu copia, la primera vez que abres el
> lanzador.
>
> MLB 15 The Show es de Sony Interactive Entertainment / San Diego Studio. Este proyecto no tiene
> relación con ellos ni con la MLB.

## Qué ofrece

- Resolución interna x1 a x4, ventana o pantalla completa, 30/60 FPS.
- Suavizado (FXAA) y escalado con nitidez, brillo ajustable.
- Mando de Xbox (XInput) y teclado configurable; ratón como pantalla táctil.
- Guardado y carga de partidas con una lista propia (Franquicia, Carrera...).
- Texturas HD opcionales, generadas con Real-ESRGAN a partir de tu copia.
- Lanzador con ventana de opciones, comprobación de Vulkan y avisos claros.

## Cómo se usa (versión ya compilada)

1. Ten tu copia del juego descifrada, de una de estas formas (solo una vez):
   - **Vita3K**: instala el juego y haz clic derecho > *Decrypt All SELF*.
   - **PS Vita con homebrew**: descifra con FAGDec y copia la carpeta `ux0:app/PCSA00511`
     con VitaShell usando *Open decrypted*.
2. Abre `ProjectBaseball.exe`, elige la carpeta `PCSA00511` y el `eboot.bin` descifrado.
3. La primera vez se traduce y compila el juego para tu PC (5-30 minutos). Después arranca al
   momento.

Detalles para jugadores: [`src/dist/LEEME_AMIGOS.txt`](src/dist/LEEME_AMIGOS.txt).

Requisitos: Windows 10/11 de 64 bits, tarjeta gráfica con Vulkan 1.1 o más y driver reciente,
8 GB de RAM.

## Cómo está hecho

```
src/host/        el «anfitrión»: HLE de la Vita (gxm.c, gpu_vk.c, hle_*.c), lanzador y su UI
                 (launcher.c, opciones.c, ui_estilo.c), recompilación en el PC (recompilar.c),
                 diálogo de partidas (hle_savedialog.c), texturas HD (texrep.c, texturas_hd.c)
src/v3k/         traductor de shaders USSE -> SPIR-V tomado de Vita3K (GPL v2)
src/dist/        lo que se reparte junto al exe: exports.def, juego_base.c, cabeceras de apoyo
src/paquete/     LEEME de la carpeta de uso personal
src/build.ps1    compilación (MSVC) y creación de las carpetas para jugar / repartir
vitarecomp_cambios/  parche sobre vitarecomp (armrecomp + runtime) que el port necesita
tools/           generar_exports.py
docs/            notas de optimización y créditos
```

Flujo en el PC del jugador (`src/host/recompilar.c`): `armrecomp emit` (eboot -> C, ~870 MB) ->
partir en trozos de 12 MB y corregir -> `clang -O2` en paralelo -> `llvm-dlltool` + `lld-link`
-> `datos\juego.dll`, que el exe carga. Los registros de la CPU emulada viven en el DLL (TLS por
hilo) y el exe accede a ellos a través de un puntero (`VITA_CPU_REMOTA`).

## Compilar

El script espera esta estructura (la del taller original):

```
<raíz>/
  vitarecomp/          https://github.com/sp00nznet/vitarecomp + vitarecomp_cambios/cambios_locales.patch
  vita-headers/        base de NIDs (db/360/*.yml)
  deps/                SDL2 2.32, Vulkan-Headers, glslang (+ glslang-v3k), LibAtrac9, fmt,
                       LLVM 19.1.7 (clang, lld-link, llvm-dlltool), Real-ESRGAN ncnn (opcional)
  mlb15/recomp/        = src/ de este repositorio
```

```powershell
cd mlb15\recomp
.\build.ps1                 # exe del port
.\build.ps1 -Partes         # además genera datos\juego.dll a partir de ..\eboot.elf (tu copia)
.\build.ps1 -Distribuir     # carpeta para repartir (sin nada del juego)
```

Se necesita Visual Studio 2022 Build Tools (C/C++).

## Licencia

GPL v2 (ver [`LICENSE`](LICENSE)): el port incluye el traductor de shaders de Vita3K. Otros
componentes y sus licencias: [`src/dist/LICENCIAS_TERCEROS.txt`](src/dist/LICENCIAS_TERCEROS.txt)
y [`docs/CREDITOS.md`](docs/CREDITOS.md).
