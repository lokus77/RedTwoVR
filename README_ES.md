[English](README.md) | **Español**

# Red Two VR

Versión PCVR del recomp **Star Wars: Rogue Squadron II – Rogue Leader** (GameCube).

Basado en la versión 1.4 de Red Two: https://github.com/gminustwo/red_two/releases/tag/v1.4

**No se incluyen datos de juego**: necesitas tu propia imagen de disco de la versión estadounidense
(`GSWE64`, revisión 0), tanto para las descargas como para tu propia compilación.

**Descarga**: el zip para Windows está en la página de [Releases](../../releases). Descomprímelo
donde quieras y ejecuta `red_two.exe`; si no encuentra el disco, el launcher lo pide (un
`GSWE64.rvz` junto a `red_two.exe` se encuentra solo). El zip es portable (todo lo tuyo se queda en
su carpeta `user`) y lleva el runtime de Visual C++.

## Realidad virtual

- Un visor con OpenXR activo antes de arrancar: Virtual Desktop, Quest Link o SteamVR.
- Pestaña **VR** del launcher > **Jugar en VR** arranca en el visor; `red_two.exe --no-xr` arranca
  en el monitor.
- Vuelo en 3D estereoscópico con seguimiento de cabeza y el HUD a distancia fija; menús, hangar y
  cinemáticas en una pantalla virtual.
- **Resolución** (pestaña VR): lo que se dibuja por ojo, de Baja (1728) a Godlike (3200), o la del
  visor; los ojos llegan al visor a resolución completa.
- **Frecuencia** del visor: 72, 80, 90 o 120 Hz, con el juego a sus propios fotogramas por segundo.
- **Vibración** en los dos mandos Touch: láser flojo, torpedos fuerte, daño según el golpe, y una
  vibración larga cuando tu nave explota.

### Mandos Touch

| Control | En vuelo |
|---|---|
| A | dispara láser |
| B | dispara torpedo (mantener 2 seg: saltar cinemática) |
| X | modo cabina |
| Y | modo cabina con computadora de puntería (pulsar para activar y desactivar); en menús, descripción audio |
| Grip derecho + stick derecho arriba / abajo | acercar la cámara |
| Grip izquierdo + stick izquierdo | dar órdenes a tu escuadrón (la cruceta) |
| Gatillos | acelerar y frenar |

## Castellano

Esta versión admite jugar en castellano (textos, menús y voces de radio) con los ficheros extraídos
de tu propio disco de la edición española (`GSWS64`), en una carpeta `red_two_es` junto al disco
USA:

```
red_two.exe --extract-spanish <tu imagen de disco GSWS64> red_two_es
```

Se requieren `frontendSP.bin`, `resDir_rows.bin`, `staticGame_sp.bin`, `text_Spanish.bin`,
`voiceDir.bin` y `r2voice.vdy` (ocupan unos 132 MB). Después, en el launcher: JUEGO > Idioma del
juego > Español. JUEGO > Idioma pone en castellano los menús de Red Two.

## Compilar (Windows)

Requisitos: Visual Studio 2022 Build Tools (C++), CMake 3.25+, Ninja, clang-cl (LLVM).

```powershell
# 1. Compilar: recompila el juego directamente desde tu imagen de disco (RVZ, WIA, ISO o GCM)
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Disc "<tu Rogue Leader.rvz>"

# 2. Ejecutar
build\windows\red_two.exe --disc "<tu Rogue Leader.rvz>"
```

Sin `--disc` el juego usa el disco elegido antes en el launcher (F1 > JUEGO > Disco del juego); si
no, busca `orig\GSWE64.iso`, `orig\GSWE64.rvz`, `GSWE64.rvz`, `GSWE64.iso` y cualquier imagen
GSWE64 junto al programa o en la carpeta de usuario; si no, el launcher la pide.
`tools\rvz.py` (Python + libzstd) también puede extraer un disco o convertirlo a ISO.

El código generado a partir de tu disco queda en `build/` y es solo tuyo: no lo compartas nunca.

`tools\make_release_win.ps1` empaqueta una compilación en `build\RedTwo-windows-x64.zip` (el
programa, sus símbolos, las DLL y un léeme) para usarla tú en otro PC.

### Jugar

- **Launcher**: Red Two abre sus ajustes (las mismas páginas que el menú del juego) antes de que
  empiece el juego. Iniciar juego (la primera fila) o el botón START del mando lo lanza. Desactiva
  ahí "Mostrar launcher al inicio" para entrar directamente al juego a partir de entonces; el menú
  del juego (F1 o Back) tiene el mismo interruptor y "Volver al launcher". `--launcher` /
  `--no-launcher` cambian el ajuste para un arranque. Un reinicio desde el menú vuelve
  directamente al juego.
- Los mandos funcionan con SDL (con vibración), también con la ventana en segundo plano. En un
  mando tipo Xbox los botones siguen las posiciones de GameCube: A = A (disparar), X (izquierda) =
  B, B (derecha) = X, Y = Y; LT/RT = L/R (acelerar y frenar), LB y RB = Z (con el stick para
  girar), stick derecho = C-stick, Menú = Start (en un mando de PlayStation: cruz = A, círculo = B,
  cuadrado = X, triángulo = Y). Teclado: WASD o flechas para dirigir, Espacio = A (disparar),
  Shift izquierdo = B, E = X, Q = Y, R = Z, 1/2 = L/R, Enter = Start, IJKL = C-stick,
  TFGH = cruceta.
- Los avisos de botones del juego muestran los botones de tu mando (el asignado a cada botón de
  GameCube, p. ej. "PRESS (Y)" y "(B) > pilot" en SELECT CRAFT): los símbolos de PlayStation con
  un mando de PS3 / PS4 / PS5, las letras de Switch con un Switch Pro Controller o Joy-Con, los
  botones de Xbox con cualquier otro, y el C-stick de SELECT CRAFT pasa a ser el stick derecho.
  F1 > Controles > Iconos de botones elige Automático, Xbox, PlayStation, Switch o los originales
  de GameCube.
- **Sticks** (F1 > Controles): el stick de control y el C-stick tienen cada uno una sensibilidad
  (25-200%) y una zona muerta redonda (0-50%; la salida crece desde cero en su borde), y todo el
  recorrido del stick pasada la zona muerta es el rango del juego. **Respuesta** fija cómo sigue
  el giro de la nave al stick: la curva GameCube del propio juego (por defecto), Suave (lento cerca
  del centro y subiendo poco a poco hasta la máxima) o Lineal. Un medidor por stick muestra dónde
  está y lo que recibe el juego (la velocidad de giro y cabeceo en el stick de control). **Invertir
  eje Y** hace que el stick suba al empujarlo arriba y baje al tirar, solo en vuelo (en los menús
  arriba sigue siendo arriba).
- **Gatillos**: L y R son analógicos, como en un mando de GameCube: acelerar y frenar crecen con
  todo el recorrido, y el clic del final da turbo a fondo o freno a fondo. **Clics de gatillo a
  LB / RB** (Controles) pone los clics en los botones superiores, de modo que los gatillos solo
  aceleran y frenan (el giro, Z, pasa al clic del stick derecho). Unas barras en la página de
  Controles muestran los dos gatillos y lo que recibe el juego.
- **Apuntado con giroscopio** (Controles): gira e inclina el mando para dirigir en vuelo, además
  del stick, con la Steam Deck, un DualSense, DualShock 4 o Switch Pro Controller (los mandos de
  Xbox no tienen giroscopio): siempre activo, mientras se mantiene un botón, alternado o desactivado
  mientras se mantiene; tocar el stick izquierdo de la Deck o un dedo en el panel táctil lo mantiene
  activo mientras está apoyado. Steam Input oculta el giroscopio: desactívalo para Red Two
  (Propiedades > Mando).
- F11 o Alt+Enter cambia a pantalla completa; `--fullscreen` arranca así.
- **Gráficos** (F1, PANTALLA): resolución interna hasta 4K, antialiasing 4x MSAA (desde el
  siguiente arranque; WebGPU no ofrece otro número de muestras) y filtrado anisotrópico hasta 16x.
  **Panorámico** (16:9, 16:10, 21:9 o la forma de la ventana) ensancha la vista 3D, lleva el HUD de
  vuelo a los bordes de la pantalla y llena la pantalla en las cinemáticas (**Barras de cine** las
  mantiene en su encuadre de cine 2.1:1 con barras negras); las ventanas más anchas tienen barras a
  los lados. **Fotogramas por segundo**: 60, 90, 120 o 144, con el juego a su velocidad normal.
  **V-Sync** (desactivado por defecto) muestra cada fotograma al ritmo del refresco de la pantalla:
  sin cortes, algo más de retraso. Con él activo, unos fotogramas por segundo iguales o mayores que
  el refresco de la pantalla van al refresco (144 en una pantalla de 120 Hz: 120), y saltar
  cinemáticas sigue yendo a toda velocidad.
- **Cinemáticas** (F1, PARTIDA): con Cinemáticas saltables activado, mantén B 2 segundos durante el
  texto inicial o una cinemática para saltarla (un anillo en la esquina se llena mientras la
  mantienes); mantener B también salta la intro al arrancar. Saltar logos llega al título en unos
  10 segundos.
- **Extras**: el documental y el teaser se reproducen a su propia velocidad con cualquier
  fotogramas por segundo; izquierda / derecha saltan 5 segundos, y A, B o START los pausan
  (CONTINUE / BACK). Mientras suena el comentario de audio en una misión, L3 (tecla C; F1 >
  Controles > Atajos) lo pausa.
- **El ratón** funciona en los menús del propio juego: apunta a una opción para elegirla, clic
  izquierdo para A, clic derecho para B, y la rueda sube y baja (también en la pantalla de pausa y
  en la galería de arte).
- **Carga**: Carga rápida (Partida, activada) lee el disco a velocidad de SSD en lugar de los
  3 MB/s del lector de GameCube. La imagen de disco se lee por delante del juego en un hilo propio,
  así que puede estar en un disco duro o una unidad de red sin tirones mientras carga música y voces.
- **Dónde están tus ficheros**: en Windows, una carpeta `user` junto a `red_two.exe` (el zip trae
  una) guarda los ajustes, la tarjeta de memoria, los estados guardados, los logs, los packs de
  texturas, los códigos y la caché de shaders, así que la carpeta puede estar en cualquier sitio
  (otra unidad, un NAS). Una instalación sin carpeta `user` pero con un `%APPDATA%\Red Two` antiguo
  sigue usando ese; crea `user` para hacerla portable (su primer arranque copia los ajustes, los
  controles y la tarjeta de memoria).
- **Trucos** (F1 > Trucos): invencibilidad, vidas infinitas, bombas infinitas, estadísticas de
  misión perfectas (la pantalla de resultados dice 0:01, todos los enemigos destruidos, 100% de
  precisión, ni aliados ni vidas perdidas y 100% de eficacia de la computadora de puntería, y se
  concede la medalla por ello) y todas las naves (cada nave que permite una misión espera en su
  hangar). **Contraseñas**: los desbloqueos del propio juego sin escribir códigos (todas las
  misiones, todas las mejoras, modo as, galería de arte, sala de música, documental, comentario de
  audio, créditos, blanco y negro), guardados en la partida del juego como si se hubieran metido
  los códigos.
- **Reiniciar juego**, **Volver al launcher** y **Salir al escritorio** están en la primera pestaña
  del menú, JUEGO.
- **Estados guardados**: Back + RB guarda, Back + LB carga, Back + cruceta derecha / izquierda
  elige la ranura (8 ranuras; teclado F5 / F8, F7 / F6). F1 > Controles tiene la ranura, los
  botones de guardar / cargar y los atajos, cada uno cualquier combinación de botones (o teclas)
  mantenidos a la vez: elige la fila, pulsa A, mantén los botones y suelta. En un mando, Back solo
  (soltado sin nada más pulsado) sigue abriendo el menú. Los estados están en la carpeta `states`
  de la carpeta de usuario y también cargan tras reiniciar, pero solo en la misma compilación de
  Red Two (una recompilación los marca como "OLD"). La tarjeta de memoria no forma parte de un
  estado: cargar uno no deshace las partidas guardadas del juego.
- Las partidas van a `saves/GSWE64_A.raw` de la carpeta de usuario (ranura A; una tarjeta nueva
  viene formateada y el juego hace su partida en el primer arranque sin preguntar). Una tarjeta
  `.raw` de Dolphin o Nintendont se puede copiar ahí tal cual, sea del tamaño que sea (59 a 2043
  bloques). `R2_CARD_A=<ruta>` usa otra imagen de tarjeta.
- **Mods** (F1, pestaña MODS): los packs de texturas en formato de Dolphin van en `textures\` de
  la carpeta de usuario (con una carpeta `GSWE64` dentro, igual que en `Load\Textures` de Dolphin;
  funcionan `.dds` y `.png`, ficheros mip y comodines), o se quedan donde los descomprimiste: MODS ->
  Carpeta de packs de texturas abre un explorador de carpetas (cruceta / stick, A abre, X sube, B
  vuelve): elige la carpeta del pack (la que contiene `GSWE64`, o `GSWE64` misma), p. ej. en
  Descargas o en una tarjeta SD. La fila de la carpeta muestra cuántas texturas se cargaron, y dice
  por qué cuando no hay ninguna (un archivo sin descomprimir, imágenes que no se llaman
  `tex1_...`). Las filas IMPORTAR DE DOLPHIN solo sirven para copiar cosas de Dolphin. Back +
  cruceta arriba (F9) desactiva y vuelve a activar los packs de texturas al momento, para comparar
  (se puede cambiar en Controles). Los códigos Gecko / Action Replay y los parches van en
  `codes.ini` de la carpeta de usuario (formato GameSettings de Dolphin) y se activan en el menú.
  El juego está recompilado de antemano, así que los códigos que cambian su código (Gecko C2
  "insert ASM", escrituras en código) no pueden funcionar; el menú los marca como no compatibles.
- **Importar de Dolphin**: MODS -> IMPORTAR DE DOLPHIN, o
  `red_two.exe --import-dolphin[=saves,textures,codes] --console`. Red Two encuentra la carpeta de
  usuario de Dolphin (AppData, Documentos, una instalación portable, el registro, una carpeta Load
  propia en `Dolphin.ini`) y trae la partida más reciente de este juego (de carpetas GCI o tarjetas
  `.raw`; la tarjeta anterior se guarda en `saves\backup` y el juego se reinicia), sus packs de
  texturas (copiados) y sus códigos (con su estado activado / desactivado).
  `R2_DOLPHIN_DIR=<carpeta User>` lo apunta a otro sitio.
- **Shaders** (F1, PANTALLA -> RENDIMIENTO): "Preparar shaders al inicio" (activado) compila antes
  de empezar cada shader que el juego ha usado en este equipo; desactivado, el juego arranca al
  momento y los objetos pueden aparecer de golpe mientras se compilan sus shaders. "Ubershaders"
  (activado) dibuja los objetos aún sin compilar con un shader genérico en lugar de saltarlos.
- **Sonido** (F1, AUDIO): el juego mezcla para Dolby Pro Logic II. **Altavoces**: Automático sigue
  la configuración de altavoces del sistema; Estéreo envía la mezcla codificada para el
  decodificador Pro Logic II de un receptor; 5.1 y 7.1 dan a la parte trasera sus propios altavoces
  (7.1 la divide en laterales y traseros). Deja el ajuste de sonido del propio juego (Options >
  Sound Settings) en STEREO. **Búfer de audio** (normal, grande, enorme): uno más grande si el
  sonido se oye entrecortado. `R2_NO_AUDIO=1` ejecuta sin sonido.
- Cada sesión escribe un log en `logs` de la carpeta de usuario, también cuando otro programa
  arranca Red Two (RecompLauncher, una terminal): ese programa recibe también una copia.
  `--console` lo muestra en una ventana de consola en Windows.

## Créditos

Red Two es la [recompilación](https://github.com/gminustwo/red_two) de gminustwo. Algunas funciones
vienen del fork [Red Two Plus](https://github.com/ZeDctr/red_two_plus) de ZeDctr: mantener para
saltar cinemáticas, la velocidad del documental, avanzar y el menú de pausa, los iconos de Switch,
la primera partida de la tarjeta de memoria, pausar el comentario de audio, el ratón en los menús
del juego, las barras de cine y una recuperación de audio más rápida. El dibujado es de
[Aurora](https://github.com/encounter/aurora).

## Legal

Red Two se publica bajo la [licencia MIT](LICENSE). Cubre solo el código propio de este
repositorio, no el código del juego que una compilación (y las descargas ya compiladas) contienen
en forma recompilada.

Este repositorio no contiene código ni recursos del juego. Star Wars y Rogue Leader son marcas de
Lucasfilm Ltd.; GameCube es una marca de Nintendo. Este proyecto no está afiliado a ellos ni
cuenta con su respaldo.
