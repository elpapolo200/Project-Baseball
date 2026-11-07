# Créditos del port de MLB 15 The Show a PC

Port nativo (recompilación ARM → x86) de **MLB 15 The Show (PCSA00511)** para uso
personal con una copia propia del juego. Licencia: **GPLv2** (ver `LICENSE`).

## Código de terceros

- **Vita3K-Plus** (<https://github.com/nckstwrt/Vita3K-Plus/>), GPLv2, © sus autores.
  Repo fuente local: `VitaRE\deps\vita3k-src` (commit 28dd8fb, 2026-10-18).
  Piezas reutilizadas del port:
  - Diseño de la mezcla NGS (voces, buses, patches con matriz 2x2, callbacks de
    player/AT9, estructuras de parámetros y estado) y la tabla HE-ADPCM ("HEVAG",
    de `vita3k\codec\src\pcm.cpp`, originalmente de vgmstream: bnnm, korenkonder;
    investigación de id-daemon) → `host\hle_ngs.c` (2026-10-30).
  - `vita3k\gxm\src\stream.cpp`: los streams por instancia son `index_source` 2/3.
  - Traductor de shaders USSE → SPIR-V (`vita3k\shader`, más `gxm\types.h`, `gxm\src\gxp.cpp`,
    `color.cpp`, `textures.cpp`) → `v3k\` (2026-10-30), con sus avisos de copyright; cambios del port
    marcados en `spirv_recompiler.cpp` y adaptadores propios en `v3k\include\util\log.h` y `fs.h`.

- **glslang** (<https://github.com/KhronosGroup/glslang>, commit fc9889c), licencias BSD/MIT/Apache de
  sus autores: `SpvBuilder` y compañía que usa el traductor de Vita3K (`deps\glslang-v3k`).

- **{fmt}** (<https://github.com/fmtlib/fmt>, commit e424e3f), MIT: formato de mensajes del traductor
  (`deps\fmt`, solo cabeceras).

- **LibAtrac9** (<https://github.com/Thealexbarney/LibAtrac9>), MIT, © 2018 Alex Barney.
  Decodificador ATRAC9 de la música y los comentarios. Fuente en `VitaRE\deps\LibAtrac9`
  (`build.ps1` lo compila como `at9.lib`).

- **SDL2** (<https://www.libsdl.org/>) — zlib license. Ventana, entrada, audio.

- **Vulkan-Headers / glslang** (Khonshu/copyright de sus autores) — backend
  gráfico y compilación de shaders en tiempo de ejecución.

## Propio (VitaRE)

- `vitarecomp` / `armrecomp`: recompilador ARM Thumb-2 → C/COFF generado y
  corregido para este juego (NEON, VFP, IT, tablas de salto, stub scan…).
- HLE de los módulos de la Vita (`hle_*.c`), launcher, conversor SELF→ELF,
  lector de PSARC y zlib propio (`inflate.c`).
- No se versiona ni redistribuye nada del juego: cada Usuario aporta su copia.
