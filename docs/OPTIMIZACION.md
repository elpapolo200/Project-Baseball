# Optimización del partido — paso 1 (medición), 2026-11-05

Medido en el portátil del usuario, enchufado, sin ventana, resolución interna del MLB15.ini,
partido Play Now (guion `guion_zona.txt`), ~22-23 FPS.

## Herramientas nuevas

- `MLB15_PERFIL=2` (+ `MLB15_PERFIL_SEG=n`, por defecto 10 s tras entrar al partido): muestrea
  todos los hilos del juego cada ~2-3 ms y escribe `perfil.txt` cada 20 s: % en espera por hilo,
  módulo en la cima, funciones con más tiempo propio / incluido y, por hilo, el 1.er y 2.º marco
  del exe. Traducir con `py -I ..\..\tools\perfil_map.py mlb15_recomp.map perfil.txt ..\nombres.csv ..\nombres_auto.csv`.
- `MLB15_JUEGO_HORA=1`: cada línea `[juego]` con ms desde el arranque y nº de fotograma.
- `MLB15_FPS=n` ahora sí limita (antes solo estaba documentado).

## Dónde se va el tiempo (por fotograma de ~45 ms)

| Hilo | Trabajo | Espera | En qué espera |
|---|---|---|---|
| Display Engine (dibujo) | ~49 % | ~51 % | ~22 % en `sceGxmEndScene` → `gpu_scene_end` (espera a la GPU al final de cada escena); ~5 % mutex |
| main (lógica) | ~17 % | ~83 % | 65 % `sceKernelLockLwMutex` (lo tiene el hilo de dibujo), 13 % `DelayThread` |
| Kinematics Jobs ×3 | ~10 % c/u | ~90 % | semáforos |
| MainMultiTask, DataStream, audio, red… | <3 % | resto | semáforos |

Uso medio de CPU: ~0,6 núcleos. **No falta CPU: el camino crítico es el hilo de dibujo** y la
lógica espera a que termine.

Dentro del trabajo del hilo de dibujo: `do_draw`/`draw_v3k` ≈ 20 ms por fotograma (lectura de
vértices/uniformes de la memoria guest con `vita_read32`/`read_comp`/`half_to_float`, búsqueda y
subida de texturas `texture_get`/`upload_*`), más ~8-10 ms esperando a la GPU.

## ¿60 FPS acelera el juego? No

Duraciones con `MLB15_JUEGO_HORA=1` a ~23 FPS y limitado a 12 FPS:

| Fase | ~23 FPS | 12 FPS |
|---|---|---|
| `IN_PITCH` → `PitchInput` | 3,23-3,28 s (81-85 fot.) | 3,25-3,33 s (39-40 fot.) |
| `POST_PITCH` → `PLAY_OVER` | 0,95-1,03 s (22-24 fot.) | 0,91-1,0 s (11-12 fot.) |

El juego avanza con el reloj, no por fotogramas: subir los FPS no cambia la velocidad del juego.

## Presupuesto para 60 FPS (16,7 ms)

- Lógica (main): ~8 ms → cabe.
- Kinemática: ~4-5 ms en paralelo → cabe.
- Hilo de dibujo: hoy ~30 ms de trabajo + espera → hay que bajarlo a <16 ms.

## Orden de trabajo propuesto

1. **No esperar a la GPU en cada `EndScene`**: enviar y seguir; esperar solo cuando la CPU
   necesite el resultado (lecturas de vuelta a memoria guest, reutilizar búferes). Espera actual
   ~8-10 ms/fotograma. Esperado: ~23 → ~30 FPS.
2. **Abaratar la traducción de dibujos** (~20 ms/fotograma): leer vértices/uniformes por puntero
   directo en vez de `vita_read32` por componente, caché de estado/descriptores, no recalcular el
   hash de texturas en cada dibujo, convertir F16 por bloques. Objetivo: ÷2-3.
3. **Skinning en la GPU** (si el perfil lo sigue señalando tras 2).
4. Repetir el perfil tras cada paso; con ventana y a x3 para ver el coste de la GPU.

Expectativa: 30 FPS estables tras 1-2; 60 posible si el paso 2 consigue ÷2-3.

## Paso 2 hecho (2026-11-05): no esperar a la GPU

1. **Superficies lineales como textura desde la GPU** (`target_texture`): antes se releían de
   la memoria guest y se volvían a subir cada fotograma (300-700 subidas/s → 60-150).
   `MLB15_LINEAL_MEMORIA=1` vuelve a lo anterior.
2. **Escenas en vuelo** (`GCtx`, `MLB15_ESCENAS_VUELO`, 6 por defecto, 1 = como antes): cada
   escena con su cmd buffer, valla, vértices/índices/uniformes y descriptores; `gpu_scene_end`
   envía sin esperar y la copia a la memoria guest (`scene_finish`) se hace al completar:
   contexto reutilizado, misma superficie (salvo si su escena anterior sigue en vuelo: se
   conserva la imagen), textura que lee esa memoria (`pend_overlap`), `sceGxmFinish`, fotogramas
   que se vuelcan a BMP. Las trazas que leen la escena recién hecha siguen siendo síncronas.
3. **Presentación**: la espera de la valla pasa al principio del fotograma siguiente
   (`MLB15_PRESENTAR_ESPERA=1` vuelve a lo anterior).

Medido (280 s, mismo exe, promedio del partido):

| Prueba | Antes | Ahora |
|---|---|---|
| Sin ventana | 24,8 FPS | 29,2 FPS |
| Ventana oculta x3 (2162x1225) | 24,3 FPS | 29,6 FPS (6 en vuelo; 28,3 con 3) |

Esperas a la GPU: ~220 → ~30-60 ms/s. La traducción de dibujos sigue en ~17 ms/fotograma: es
el siguiente paso para pasar de 30. Trazas nuevas: línea `[fps]` con escenas/copias/subidas por
5 s y esperas por motivo; `MLB15_ESCENAS_HISTO=1`; `MLB15_NOTIF_TRAZA=1`; `MLB15_VENTANA_OCULTA=1`
(prueba con ventana sin mostrarla).

## Paso 3 hecho (2026-11-05 noche): CPU del hilo de dibujo y código del juego optimizado

| Cambio | FPS sin ventana (media del partido) |
|---|---|
| Punto de partida (tras el paso 2) | 29,2 |
| `env_cached`: getenv recordado (138 llamadas, varias en cada draw) | 37,4 |
| Atributos de vértice: bucle por formato + tabla de 65 536 medios floats | 44,4 |
| Programas por tabla hash (`vp_find`/`fp_find`), filtro de superficies en `target_texture`, `prof_now` sin `QueryPerformanceFrequency`, caché de conjuntos de 16 texturas y memo de texturas por escena, estado dinámico sin repetir, subidas de texturas sin esperar (búfer de subidas por escena en vuelo) | ~46 |
| **Partes del juego compiladas con /O2** (antes /Od) | **58,1** |

Con ventana (oculta) a x3, 2162x1225: **59,7 FPS de media, mínimo 58,9** (topa con el límite de 60).
Verificado: partido completo (lanzamientos, jugadas, HUD, zona, rayas), Crear jugador (cara,
nombre), menús.

Interruptores para aislar fallos: `MLB15_SIN_CACHE_TEX`, `MLB15_SIN_MEMO_TEX`, `MLB15_SIN_CACHE_ESTADO`,
`MLB15_SUBIDAS_SINCRONAS`, `MLB15_DECODIFICA_LENTO`, `MLB15_VERTICES_INIT_TODO`; `build.ps1 -Partes -Od`
recompila el juego sin optimizar (copia de los .obj /Od en `partes_obj_Od\`). Trazas: líneas `[fps]`
de `draw_v3k` y `envio` por sección, `MLB15_DELAY_TRAZA=1`, `MLB15_PERFIL_HILO=texto`.

Lecciones: (1) un `getenv` en un camino caliente cuesta un 25 % de FPS; (2) el código recompilado
sin optimizar era el mayor freno restante: compilarlo con /O2 tarda ~200 s por parte y no rompió nada.

## Paso 4 (2026-11-06): consumo

| | CPU a 60 FPS | CPU a 30 FPS | RAM en uso |
|---|---|---|---|
| Antes | 3,26 núcleos | 2,91 núcleos | 1,56 GB |
| Después | 1,74 núcleos | 1,28 núcleos | ~1,3 GB |

Sondeos del juego que giraban en vacío: LwMutex «Render 0/1» con espera 0 (~150 000/s cada uno),
`sceKernelDelayThread(300 us)` convertido en Sleep(0) y semáforos con espera 0 (32 cesiones por
fallo). Búferes por escena en vuelo ajustados a lo medido y 4 escenas en vuelo. Medición: script
`consumo.ps1` (CPU del proceso entre dos instantes, RAM máxima) y `MLB15_CPU_HILOS=10`.
