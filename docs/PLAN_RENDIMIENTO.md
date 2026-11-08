# Plan de rendimiento — que el port gaste mucho menos (2026-11-08)

Objetivo del usuario: **el juego igual (misma imagen, 60 FPS), pero con mucho menos consumo**, para
que el port valga la pena frente a Vita3K y funcione en PCs flojos y con OBS.

## 1. Medición de partida

Portátil de pruebas (Core 7 240H, RTX 5050 Laptop, enchufado), partido Play Now con la ventana oculta
(banco de pruebas propio, guion fijo de pulsaciones), media de 60 s ya dentro del partido:

| Resolución | FPS | CPU del proceso | RAM | NVIDIA |
|---|---|---|---|---|
| x3 | 60,0 | **1,74 núcleos** | 1,29 GB | 38 %, 22,8 W, 1 546 MHz |
| x2 | 59,9 | 1,73 núcleos | 1,29 GB | 36 %, 20,9 W, 1 505 MHz |

- **La gráfica no es el problema**: de x3 a x2 casi no cambia. El consumo está en la CPU.
- CPU por hilo (`MLB15_CPU_HILOS=10`): **Display Engine 84 %** de un núcleo (el que dibuja: va al
  límite; si pasa del 100 % se pierden FPS), main 14 %, 3 × Kinematics Jobs ~11 %, 2 × KineCore_MT
  ~10 %, MainMultiTask 3 × 3 %, audio 3 %.
- Por fotograma: ~570 dibujos, **55 escenas** (40 se copian de vuelta a la memoria de la Vita) y
  hasta **13,9 MB de vértices** copiados por escena.

## 2. Dónde se va la CPU (perfil de muestreo, `MLB15_PERFIL=2`, 100 s)

Muestras «trabajando» (las demás son esperas en ntdll), por módulo en la cima de la pila:

| Módulo | Muestras | Qué es |
|---|---|---|
| `ProjectBaseball.exe` (port) | 41 680 | la capa del port: memoria, NEON, llamadas indirectas, dibujo |
| `juego.dll` (código del juego) | 8 720 | la lógica real del juego |
| `VCRUNTIME140` (memcpy/memset) | 4 993 | copias de vértices y de escenas |
| `nvoglv64` (driver NVIDIA) | 2 024 | Vulkan |

**El juego en sí gasta ~5 veces menos que el port que lo rodea.** Funciones del port con más tiempo
propio: `vita_read32` 6 173, `draw_v3k` 4 881, `dp_3same` (NEON) 3 928, `do_draw` 3 152,
`vita_write32` 2 642, `vita_dispatch` 2 575, `texture_get_` 1 868, `vita_read8/16` ~1 070,
`dp_scalar`/`neon_dp`/`neon_ldst` ~1 100.

Causas encontradas en el código:

1. **Cada acceso a memoria del juego es una llamada al exe.** El código traducido hace
   `r0 = vita_read32(r7)`, que en `juego.dll` es una llamada importada del exe → `vita_mem_ptr`
   (otra llamada + comprobación de límites) → `memcpy`. Tres llamadas para leer un número, millones
   de veces por segundo, en todos los hilos.
2. **NEON se interpreta en cada ejecución.** armrecomp no traduce las instrucciones vectoriales:
   emite `neon_dp(pc, raw)` y el runtime decodifica la instrucción de cero cada vez (física,
   animación, matrices: hilos Kinematics/KineCore y parte del dibujo).
3. **Las llamadas indirectas buscan el destino** (`vita_dispatch`) en cada llamada (punteros a
   función, métodos virtuales C++).
4. **El dibujo repite trabajo cada fotograma**: vuelve a copiar vértices desde la memoria de la Vita
   (también con `vita_read32` por palabra), vuelve a buscar texturas y copia 40 escenas por
   fotograma de vuelta a la memoria de la Vita aunque casi nunca se lean.
5. `juego.dll` se compila con `-O2 -msse4.1` genérico, aunque se compila en el PC del jugador y
   podría aprovechar su procesador (AVX2, etc.).

Otros datos: el hilo main pasa el 89 % esperando (bien).

## 3. Plan por fases (de más beneficio y menos riesgo a menos)

Cada fase: se implementa, se mide con el mismo banco de pruebas (CPU, hilo de dibujo, FPS, vatios)
y se compara la imagen con capturas en los mismos momentos. Si no mejora o cambia la imagen, se
deshace. Cada fase sube `RECOMP_VERSION` si toca el código traducido (cada copia se vuelve a
preparar sola, ~3 min, una vez).

### Fase 0 — Banco de pruebas fijo (preparación)
- Pasar `medir_base.ps1` a `tools\medir_rendimiento.ps1`: FPS, núcleos, % del hilo de dibujo, RAM,
  vatios de la NVIDIA, y capturas en fotogramas fijos para comparar la imagen.
- Opcional: misma medición de Vita3K como referencia.

### Fase 1 — Memoria directa en el código traducido  ★ mayor beneficio
- `vita_read*/vita_write*` pasan a ser `static inline` en `juego.dll`: puntero base + una
  comparación de límites sin llamadas (`(a - base) < tam ? *(u32*)(mem + a - base) : lento(a)`).
  El exe exporta la base/tamaño (como ya hace con `vita_cpu_tls`). Lo raro (fuera de rango) sigue
  por la función lenta y se cuenta igual que ahora.
- Lo mismo en el dibujo del port (`gxm.c`/`gpu_vk.c`): leer vértices/uniformes con un puntero y
  `memcpy` en bloque en vez de `vita_read32` por palabra.
- Esperado: la lógica del juego y todos los hilos bastante más ligeros (−20 a −35 % de CPU total).
  Riesgo: bajo (mismo comportamiento, solo sin llamadas).

### Fase 2 — Compilar `juego.dll` para el procesador de cada jugador  ★ fácil
- `clang -march=native -O2` (o `-O3` si mide mejor) en `recompilar.c`. La huella `juego.txt`
  incluye el modelo de CPU: si se copia la carpeta a otro PC, se vuelve a preparar sola (si no,
  un PC más viejo se cerraría con una instrucción no soportada).
- Esperado: −5 a −15 % en la lógica. Riesgo: bajo.

### Fase 3 — NEON traducido de antemano
- armrecomp decodifica cada instrucción NEON al emitir y genera una llamada directa a una función
  concreta con los registros ya resueltos (p. ej. `neon_vmla_f32_q(d, n, m)`) o C con SSE en línea
  para las más frecuentes (las de `dp_3same`: suma/multiplica/multiplica-acumula en float).
  Lo no cubierto sigue por `neon_dp`.
- Esperado: hilos Kinematics/KineCore a la mitad aprox. Riesgo: medio (hay tests de NEON en
  vitarecomp; añadir uno por forma).

### Fase 4 — Dibujo sin trabajo repetido  ★ el hilo crítico
- **Caché de geometría**: la memoria de la Vita se reserva con `MEM_WRITE_WATCH`;
  `GetWriteWatch` dice qué páginas cambiaron desde el fotograma anterior. Los búferes de vértices
  e índices que no cambiaron (estadio, campo, gradas) se quedan en la GPU y no se copian otra vez;
  solo se copian los que se mueven (jugadores, bola, marcador).
- Lo mismo para texturas (`texture_get_`/`tex_infos`): no volver a comprobar las que no cambiaron.
- Esperado: Display Engine de 84 % a ~40-50 % de un núcleo. Riesgo: medio (si una página cambia
  sin avisar se vería algo viejo; `GetWriteWatch` lo evita).

### Fase 5 — Llamadas indirectas con tabla
- `vita_dispatch` con una tabla plana (dirección → función) en vez de búsqueda. Riesgo: bajo.

### Fase 6 — Escenas que vuelven a memoria solo si hace falta
- 40 escenas por fotograma se copian a la memoria de la Vita. Copiarlas solo cuando el juego las
  vaya a leer (páginas protegidas: la primera lectura dispara la copia). Riesgo: alto; se hace al
  final y solo si las fases anteriores no bastan.

### Fase 7 — Esperas que duermen de verdad
- Mutex/semáforos que se sondean con cesiones (`mutex_respiro`, `poll_backoff`) pasan a
  `WaitOnAddress` (el hilo duerme hasta que el otro suelta). Menos CPU en reposo y menos
  sensibilidad a OBS sin tocar la prioridad. Riesgo: medio (sincronización).

### Fase 8 — Gráfica (menor prioridad: no es el cuello de botella)
- Mapas de sombra limitados a x2 aunque la pantalla vaya a x3 (6144x3072 es mucho para una sombra).
- **AMD FSR 1** (licencia MIT, solo shaders) como reescalado opcional: x2 + FSR ≈ x3 en nitidez con
  menos GPU (pensado para portátiles con gráfica integrada).
- Caché de pipelines de Vulkan en disco (`VkPipelineCache` en `datos\`): menos tirones la primera
  vez que aparece cada efecto.
- Descartado: DLSS/XeSS (necesitan vectores de movimiento que el juego no da).

## Progreso

| Paso | CPU (núcleos) | Display Engine | Notas |
|---|---|---|---|
| Partida (2026-11-08) | 1,74 | 84 % | |
| Fase 1: memoria en línea en `juego.dll` | 1,63 | 84 % | `mem.h` `VITA_MEM_DIRECTA` (volatile; nombres `vita_md_*` + macros: con el mismo nombre que la etiqueta asm del camino lento, el camino lento se llamaba a sí mismo y el menú se colgaba al 98 %). `MLB15_MEM_LLAMADAS=1` vuelve a las llamadas. RECOMP_VERSION 4 |
| + exe con `/GL` `/LTCG` | 1,59 | 83 % | `build.ps1` |
| Fase 2: `-march=native` + huella del procesador | **1,52** | 80-84 % | RECOMP_VERSION 5; `juego.txt` lleva `proc XXXXXXXX` (otro PC → se prepara otra vez). Kinematics 11 → 8-9 %, KineCore 10 → 6-7 % |

| Fase 4a: vértices crudos (formatos nativos de Vulkan) | — | **87 → 76 %** | medido por dibujo (no depende de lo que haya en pantalla): preparación 2,93 → 0,69 µs, total `draw_v3k` 5,85 → 3,81 µs (−35 %). 94 % de los dibujos van por el camino nuevo (el resto: atributo desalineado). Rango seguido de vértices = una copia por flujo. Imagen comparada con el camino viejo: igual. `MLB15_VERTICES_CPU=1` vuelve atrás; `[crudo]` en el log cada 300 000 dibujos |

Nota de medida: el total de CPU del proceso varía ±0,1 núcleos entre partidos aunque no cambie nada
(cuántos dibujos hay depende del momento). Para comparar usar µs por dibujo de las líneas `[fps]` y la
media de `[cpu]` por hilo en todo el partido.

| Fase 4b: uniformes con offsets dinámicos + texturas del dibujo anterior | — | 76-78 % | `draw_v3k` 3,81 → 3,6 µs por dibujo (poco). Un conjunto de uniformes fijo por contexto (`dset0`, `DYN_SB_RANGE` 64 KB; `MLB15_SIN_UNIF_DIN=1` vuelve atrás); texturas iguales al dibujo anterior solo el 30 % (`MLB15_SIN_TEX_ANTERIOR=1`). Vigilancia de escrituras (`GetWriteWatch`, memoria con `MEM_WRITE_WATCH` en vitarecomp `mem.c`) hecha pero DESACTIVADA (`MLB15_WRITEWATCH=1`): la huella de texturas ya era barata. Imagen igual |

Perfil tras 4b (muestras propias del exe): NEON interpretado `dp_3same` 5 096 + `dp_scalar` 785 + `neon_ldst` 612 +
`neon_dp` 474 ≈ **7 000** (lo más caro), `do_draw` 3 407, `vita_dispatch` 2 900. `vita_read32` ya no aparece
(fase 1). OJO: las líneas `[fps] envio/texturas` incluyen el coste de los propios `QueryPerformanceCounter`.

| Fase 3: NEON frecuente escrito en C al traducir | **1,39** | **63 %** | Medido: ~8,7 M NEON/s en el partido; 9 formas = 83 % (VMLA/VMUL.F32 por escalar, VORR, VMUL/VADD/VSUB.F32, VCGT, VBSL, VBIF). `vitarecomp/tools/armrecomp/neon_emit.h` las escribe en C sobre `vfp_s[]`; el resto sigue por `neon_dp`. Prueba `neon_emit_test.c` contra neon.c: 6 000 casos x 8 registros, 0 diferencias (NaN contra NaN cuenta como igual: ninguno modela el NaN por defecto de NEON). `-ffp-contract=off` en juego.dll (la Vita no fusiona mul+add; con `-march=native` clang podía usar FMA). `ARMRECOMP_NEON_RUNTIME=1` al traducir vuelve al intérprete. RECOMP_VERSION 6. Suma de hilos 158 % → 126 %; Kinematics 9-11 → 7 %, KineCore 9-10 → 5 %. Imagen igual |

| Fase 5: caché por hilo de llamadas indirectas | 1,37 | 65 % | `vitarecomp/src/dispatch.c`: 1 024 destinos recientes por hilo delante de la búsqueda binaria (42 000 funciones) y contador de aciertos por hilo (antes uno compartido escrito por todos los hilos en cada llamada). Perfil: `vita_dispatch` 2 900 → 568 muestras. En CPU total, dentro del ruido. ctest 4/4. No hace falta preparar juego.dll |

Perfil tras la fase 5 (muestras propias): exe **17 500** (al empezar 41 700), juego.dll 13 700 (sube porque memoria
y NEON ya se hacen dentro del juego). Lo que queda en el exe: `do_draw` 3 400, `texture_get_` 1 800 (la huella
por escena; con `MLB15_WRITEWATCH=1` desaparece del perfil pero la llamada al sistema cuesta lo mismo: medido
125 % → 143 % de suma de hilos, se queda desactivado), `draw_v3k_x` 725, `neon_ldst` 713, reverb del audio 482.

**Siguiente:** `do_draw` (compactación de índices y estado por dibujo) y `neon_ldst` (VLD/VST traducidos como la fase 3).

Antes (fase 4b): el «envío» es ahora lo más caro por dibujo (~2,7 µs: texturas `tex_infos` ~1 µs,
asignar conjuntos de descriptores ~0,5, órdenes ~0,4). Después, caché de vértices en la GPU con
`GetWriteWatch` (ahorro menor ya: la preparación quedó en 0,7 µs).

Antes: fase 4. `draw_v3k` convierte en la CPU cada vértice de cada dibujo a vec4 float (`g_rverts`)
y lo sube; la idea es subir los flujos de vértices tal cual con formatos nativos de Vulkan (la GPU
convierte gratis) e índices originales, y conservar en la GPU los que no cambian (`GetWriteWatch`).

## 4. Metas

| Medida | Ahora | Meta |
|---|---|---|
| CPU del proceso a 60 FPS (x3) | 1,74 núcleos | **≤ 0,9 núcleos** |
| Hilo de dibujo | 84 % de un núcleo | ≤ 50 % |
| FPS con 10 núcleos ocupados, prioridad normal | 14 | ≥ 50 |
| PC flojo (i5 10ª gen + Intel UHD) | 30 FPS a x1 | 60 FPS a x1 |
| Imagen | — | igual (capturas comparadas) |

Orden propuesto: 0 → 1 → 2 → 4 → 3 → 5 → 7 → 8 → 6, midiendo después de cada fase.
