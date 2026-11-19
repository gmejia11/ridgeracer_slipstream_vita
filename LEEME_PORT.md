# Ridge Racer Slipstream – port para PS Vita (soloader-boilerplate)

## Compilar

```sh
export VITASDK=$HOME/vitasdk-softfp   # VitaSDK-softfp
export PATH=$VITASDK/bin:$PATH
cd vita_port && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug   # Debug = con logs; Release para jugar
make -j8
```

Resultado: `build/ridgeracer_slipstream.vpk` (TitleID `RRSS00001`).

## Requisitos en la Vita

- `kubridge.skprx` en `ur0:tai/config.txt` (sección `*KERNEL`)
- `libshacccg.suprx` (ShaRKBR33D)

## Archivos de datos → `ux0:data/ridgeracer/`

```
ux0:data/ridgeracer/
├── libRidgeRacer.so      (de lib/armeabi-v7a/)
├── libfmodex.so          (de lib/armeabi-v7a/)
├── libfmodevent.so       (de lib/armeabi-v7a/)
├── main.obb              (main.2319.com.namcobandaigames.ridgeracerss.obb renombrado)
└── assets/               (la carpeta assets/ completa del APK)
```

Las partidas guardadas se crean en `ux0:data/ridgeracer/files/`.

## Controles

| Vita              | Juego (mando Android)          |
|-------------------|--------------------------------|
| D-Pad             | D-Pad (menús)                  |
| Stick izquierdo   | Eje X/Y                        |
| Stick derecho     | Eje Z/RZ                       |
| X / O / □ / △     | A / B / X / Y                  |
| L / R             | L1 / R1 y gatillos LT / RT     |
| START             | START (pausa)                  |
| SELECT            | BACK de Android                |
| Pantalla táctil   | Táctil                         |
| Inclinar la Vita  | Acelerómetro (volante)         |

## Qué hace el port (resumen técnico)

- Carga 3 módulos: `libfmodex.so` (0x9A100000), `libfmodevent.so` (0x9A400000) y
  `libRidgeRacer.so` (0x98000000). Los símbolos FMOD se resuelven entre módulos
  por DT_NEEDED. NMALib (anuncios) no se carga: está stubeada en `source/stubs.c`.
- **Audio**: se fuerza FMOD a la salida AudioTrack (tipo 21) enganchando
  `EventSystem::init`, y un hilo propio (`source/audio.c`) hace lo que hacía
  `org.fmod.FMODAudioDevice` en Java: llama a `fmodProcess()` y manda el PCM a
  `sceAudioOut`.
- **Mando**: el juego sondea `RRSSAndroidNwInputControllerDelegate` por JNI cada
  frame (`CController::Update`); se emula en `source/java.c` + `source/controls_rr.c`.
  Los eventos de joystick de FalsoNDK se filtran porque el juego los trataría como
  toques de pantalla.
- **OBB**: `GetExpansionFileNameWithPath` devuelve `ux0:data/ridgeracer/main.obb`,
  `GetVersionCode` devuelve 2319 (debe coincidir con el OBB), y `CFile::Delete`
  está bloqueado para el OBB por seguridad.
- **Intro**: no hay vídeo; `PlayIntroVideo` llama directamente a `ContinueSplashUpdate`.
- **Excepciones C++**: `__gnu_Unwind_Find_exidx` devuelve la tabla `.ARM.exidx`
  del .so correspondiente, para que los `throw` dentro del juego funcionen.

## Depurar

Con la build Debug, los logs salen por `sceClibPrintf` (PrincessLog / psp2shell) y
los imports sin resolver aparecen como `Unresolved import: ...`. Si crashea, el
target `make dump` baja el core dump y lo analiza con vita-parse-core.

## Rendimiento / opciones

- Build final: `cmake .. -DCMAKE_BUILD_TYPE=Release -DVITA3K_BUILD=OFF` (carpeta `build_rel/`).
- `ux0:data/ridgeracer/config.txt` → línea `msaa N` (0 = sin MSAA, 1 = 2x, 2 = 4x). 2x por defecto: 4x probado en el Vita y la GPU no da abasto (menús a 30 fps, tirones en carrera).
- Los shaders compilados se guardan con el caché propio de vitaGL (`HAVE_SHADER_CACHE`) en
  `ux0:data/shader_cache/RRSS00001/` (la primera partida tiene tirones al compilar; después
  cargan del caché). No usar `DUMP_COMPILED_SHADERS` del boilerplate: con
  `VGL_MODE_POSTPONED` el shader aún no está compilado en `glCompileShader` y crashea.
- `lib/vitagl` está parcheado (`gpu_store_mipmap` en `utils/gpu_utils.c`): las texturas sin
  comprimir guardan los mipmaps que manda el juego en vez de regenerarlos. vitaGL los
  regeneraba con un formato de transferencia equivocado para RGBA5551 y el follaje y los
  edificios lejanos salían con puntos de colores.
- `build/`, `build_hw/` y `build_rel/` comparten `lib/vitagl`: al cambiar de carpeta borra
  `vitagl_flags.stamp` en la que vayas a compilar.
- Perfilado: `cmake .. -DCMAKE_BUILD_TYPE=Release -DVITA3K_BUILD=OFF -DPERF_PROFILE=ON` (carpeta
  `build_perf/`). Cada 120 frames escribe en `ux0:data/ridgeracer/perf.txt` los ms/frame de
  update, render, física (Bullet), sonido, draw calls, subidas de texturas/buffers y espera de GPU.
- **fastpath** (`source/fastpath.c`, `source/fastpath_asm.S`): `libRidgeRacer.so` está compilado a
  `-O0`. Antes de inicializar el módulo se reescriben en su sitio unas 1.300 funciones triviales
  (getters, `vector::size/[]`, "return this") y las operaciones de vectores, y `CMatrix::MatrixMultiply`
  / `VecTransform` pasan a C optimizado (mismo orden de operaciones → resultados idénticos).
  Se desactiva con `fastpath 0` en `config.txt`.
- **Núcleos** (`source/reimpl/pthr.c`): el hilo principal del juego (android_main, creado desde
  `ANativeActivity_onCreate`, llamada en 0x277a04) se fija al núcleo 0 con prioridad un poco más alta;
  el resto de hilos (Bullet, RakNet, FMOD, cargas) van a los núcleos 1-2, el hilo de audio al 1 y el
  recolector de vitaGL al 2. Se desactiva con `cores 0` en `config.txt`.
- fastpath ronda 2026-11-11: `IGameObject::{Pre,,Post,Post2}UpdateNode` (recorrido del árbol de
  objetos sin copiar el vector ni std::find a -O0), `sBoundingBox::AddPoint`, `CMatrix::VecRotate`,
  `CMatrix::Get(int,int)`, `CRenderMesh::GetMaterial/GetGeometry`. El perfilador mide además
  CCar/CArtInt/CTrackScene por partes. Nota: el binario es mixto, parte ya está a -O2 (p.ej. splines).
- `GetLightingFactorFromMesh` (sombra bajo coches/instancias) usa una caché de cajas XZ por triángulo
  de la malla de sombras para descartar los lejanos (probado en PC: resultado idéntico bit a bit).
- Reescrituras grandes (`rewrite 0` en `config.txt` las desactiva sin tocar el resto de fastpath):
  `CRenderNodeMesh::Collect` en C (mismo flujo y mismas llamadas a funciones del juego; push_back con
  camino rápido en línea). Comprueba que el código original es el de la 2.5.4 antes de engancharse.
  `CRenderShaderPass::Begin` en C (texturas + 26 de los 28 tipos de uniform); las pasadas con uniforms
  de huesos (18), animación de sprites (19) o tipos desconocidos llaman al código original vía trampolín.
- perf.c puede medir también funciones ya redirigidas por fastpath (ldr pc en el prólogo).
  `CRenderGeometry::LoadVertexData` en C para todos los formatos (huesos, normales 10:10:10:2, color,
  UV float/half, réplicas de instancias): carga de mallas y marcas de neumático.
- `framepacing` en `config.txt` (glutil.c `eglSwapBuffers_soloader`): 1 (por defecto) = si el frame no
  llega a 60 fps se muestra exactamente 2 refrescos (30 fps estables, sin tirones de 2/3 refrescos) y
  vuelve a 60 cuando hay margen; 0 = vsync normal (comportamiento antiguo); 2 = siempre 30 fps.
  perf.txt muestra ahora el frame más lento de cada ventana y cuántos pasan de 40/60 ms.
  Camino de dibujo por malla en C: `CRenderNodeMesh::Render`, `CRenderGeometry::Begin/BindVBO/Draw/End`,
  `CRenderShaderPass::End/UpdateForInstance` (misma secuencia de llamadas GL que el original).
- Menús con cruceta: el menú principal del juego solo enlaza izquierda/derecha (diseño original).
  Reasignar arriba/abajo se probó y se quitó (en pantallas de opciones izquierda/derecha cambian
  valores). La versión PERF registra cada transición en `ux0:data/ridgeracer/menu.txt`.
- Cargas: el juego incluye su propia zlib 1.2.8 compilada a -O0. fastpath (`patch_zlib`) redirige toda
  la API inflate*, `uncompress`, `crc32` y `adler32` a la zlib optimizada del VitaSDK (todo o nada;
  deflate queda con la del juego). Se desactiva con `fastpath 0`.
- **OBB de una sola apertura** (`source/obbfile.c`): el OBB es un pack TLIB propio del motor y
  `CFile::Open` hacía fopen/fseek/fread/fclose del OBB entero por cada archivo. Ahora se abre una vez y
  cada fopen devuelve un FILE virtual (posición + lectura adelantada de hasta 256 KB que nunca pasa del
  final del archivo del pack, según el índice TLIB; lecturas con sceIoPread). Se envuelven
  todas las funciones FILE* que importan el juego y FMOD. `obbcache 0` en `config.txt` lo desactiva.
  perf.txt muestra "OBB I/O (opens)": ms/frame de E/S del OBB y aperturas por frame.
- Caché de "archivo no existe" (`stubs.c`): ahora cubre cualquier ruta (16384 entradas), también
  `access()`, y se vacía entera ante cualquier escritura/creación/renombrado/borrado (fopen/open en
  escritura, mkdir, rename, remove, unlink, rmdir). `TextureManager::FindFile` prueba varias carpetas
  por textura y cada prueba fallida iba a la tarjeta.
  `CTextureManager::FindFile` memorizado (nombre+flags -> ruta; se vacía con AddCommonTexturesPath /
  SetPVRTCETCDir). El resultado se reconstruye con el std::string del propio juego.
- Inventario de `ux0:data/ridgeracer` (`stubs.c`, `snap_query`): se lista una vez qué archivos y carpetas
  existen y las comprobaciones de archivos sueltos se responden desde memoria (sin distinguir mayúsculas);
  se rehace tras cualquier escritura/creación/renombrado/borrado.
- **libdeflate** (`lib/libdeflate`, MIT, v1.22, solo el descompresor): `uncompress()` (bloques TEPK de
  64 KB del OBB) usa libdeflate; si no devuelve éxito se repite con zlib. La versión PERF además
  descomprime cada bloque con zlib, compara y escribe en perf.txt "libdeflate check: N chunks identical".
  Se desactiva junto con el resto con `fastpath 0`.
- **Redes sociales ocultas** (`source/social.c`): al cargar los XML de interfaz (pugixml `load_buffer`)
  los botones de Facebook (FBLogin/FBLogout en Perfil, BFbShare en Resultados), BShareIntent y los
  iconos de Facebook de los desafíos reciben `visible="0"` y una posición fuera de pantalla (los
  atributos originales se renombran, el XML sigue bien formado). `social 1` en `config.txt` los muestra.
- `CFile::Read` de archivos comprimidos del pack (`cfile_read` en fastpath.c): mismo algoritmo pero con
  caché de los últimos 8 bloques de 64 KB descomprimidos (clave = posición absoluta + tamaños, el OBB no
  cambia). FMOD lee sonidos y música por streaming en trozos de 2 KB a través de CFile, y el original
  descomprimía el bloque entero en cada trozo (~32 veces por bloque). Probado en PC con 935 archivos.
- Menús con cruceta (`controls_rr.c`): las ruedas (`MenuControllerSlotRoller`: menú principal, selección
  de coche/circuito) solo giran con el stick derecho salvo que la pantalla active la "configuración
  básica" del juego; en una rueda, la dirección de la cruceta sin enlace se envía como la del stick
  derecho (2/3 -> 7/8 en ruedas verticales, 0/1 -> 5/6 en horizontales).
- Stick izquierdo: se activa el muestreo analógico y se lee con `sceCtrlPeekBufferPositiveExt2`.
- Interruptores individuales en `config.txt` (para aislar fallos; todos a 1 por defecto, requieren
  `rewrite 1`): `rw_collect`, `rw_begin`, `rw_vertex` (LoadVertexData), `rw_render` (dibujo por malla),
  `rw_findfile`, `rw_cfile` (caché de CFile::Read), y `deflate` (libdeflate). `obbcache` ya existía.
- Stick izquierdo = cruceta (umbral 0,6) para navegar los menús; en carrera el juego usa antes el valor analógico.
