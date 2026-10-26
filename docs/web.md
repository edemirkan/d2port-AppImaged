# La versión para el navegador (WebAssembly)

Pedido del usuario (26/10/2026): dejar todo hecho para que quien quiera use el
port en el navegador, **sin publicar ninguna página**. Es un paquete más del
release, `diablo2-port-web.zip`, que cada uno abre en su computadora (o sube
a donde quiera, con sus propios archivos del juego).

## Qué trae el paquete

| Archivo | Qué es |
|---|---|
| `index.html` | La página: el botón Jugar, la precarga con su progreso, el lienzo, las partidas en el navegador |
| `d2port.js`, `d2port.wasm` | El juego compilado con Emscripten (adentro de `d2port.js` va `web/cache.js`) |
| `archivos.txt` | La lista del importador: qué archivos del juego hay (el juego no pide lo que no está) |
| `precarga.txt`, `precarga-juego.txt` | Lo que se lee hasta el menú y lo que agrega entrar a jugar (ver abajo) |
| `d2port-servidor.exe`, `d2port-servidor` | El servidor de Windows y de Linux (`tools/d2port-servidor.c`, ver abajo): sirve la página, guarda las partidas en `save/` y hace de puente de la red local; abre el navegador |
| `d2port-importar.exe`, `d2port-importar` y los `Importar … .bat/.sh` | Los importadores de Windows y Linux: copian los archivos del juego a `data/` de esta carpeta |

Los navegadores no cargan WebAssembly desde un archivo suelto (`file://`), por
eso hace falta el servidor local. Escucha solo en `localhost` (puerto 8642, o
el siguiente libre; `--puerto N` o `PORT=N` lo cambia; `--sin-navegador` o
`D2_NO_BROWSER=1` no abre el navegador, para pruebas).

## Cómo se compila

```bash
# Una vez: Emscripten en ~/emsdk
git clone https://github.com/emscripten-core/emsdk ~/emsdk && ~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest
bash scripts/build-web.sh          # build-web/d2port.js y d2port.wasm, y build-web/servidor/{linux,windows}
bash scripts/package-release.sh    # publicar/diablo2-port-web/ y su zip (con los importadores de build-importador.sh)
```

El `CMakeLists.txt` tiene la rama `if(EMSCRIPTEN)`: el SDL2 de Emscripten,
`ASYNCIFY` (los bucles del juego y la lectura de archivos esperan sin trabar la
página), `idbfs.js` (las partidas), `INVOKE_RUN=0` (la página llama a `main`
después de leer las partidas y de la precarga) y `--pre-js web/cache.js`. Todo
lo propio del navegador está detrás de `#ifdef __EMSCRIPTEN__`: el build de la
PC, la Pi y Windows no cambia.

## Cómo funciona

- **Los archivos del juego** (`src/fmt/fs.c`): `fs_read` y `fs_open` piden
  cada archivo al servidor (`data/…` al lado de la página) cuando hace falta.
  La lista `archivos.txt` dice cuáles hay sin pedirlos (`fs_exists`; antes
  `world.c` abría el archivo entero para saber si existía). Las cinemáticas y
  `idioma.txt`, que el importador genera sin estar en la lista, se preguntan
  con un `HEAD`. `fs_find` busca en la lista.
- **La caché** (`web/cache.js`): pedir de a un archivo cuesta milisegundos y
  hasta el menú son ~840. La página baja en paralelo (de a 24) los de
  `precarga.txt` antes de arrancar y, ya jugando, los de `precarga-juego.txt`
  en segundo plano; `fs_prefetch_dir` (las baldosas del acto) también baja en
  segundo plano. El juego toma de ahí al instante; lo que no está lo pide y
  espera. Lo que toma sale de la caché; lo precargado de más tiene un tope de
  192 MB. Las listas se rearman con `scripts/precarga-web.sh` (el juego
  nativo con `D2_FS_LOG`, solo lo de la lista del importador que existe); si
  quedan viejas, anda igual, más lento.
- **El ritmo**: después de cada cuadro el juego espera el próximo refresco de
  la página (`requestAnimationFrame`, como mucho 50 ms: con la página tapada
  el navegador da uno por segundo y la carga, que dibuja la puerta muchas
  veces, se hacía eterna).
- **El lienzo** queda en 800x600 (la ventana no se redimensiona en el
  navegador) y la página lo agranda con CSS manteniendo 4:3. F11 pone la
  página en pantalla completa (la API del navegador).
- **Las partidas** van a `/save`. Con `d2port-servidor` es memoria: antes de
  arrancar la página lee las de `save/` de la carpeta (`GET /partidas` y cada
  archivo) y cada 3 segundos, al taparse y al irse sube las que cambiaron
  (`PUT /save/…`, por fecha y tamaño) y borra las que ya no están
  (`DELETE`): son las mismas que las de la versión de PC (descomprimiendo el
  paquete del navegador en la carpeta del de Windows, comparten `data/` y
  `save/`). Antes de recargar por el idioma, primero guarda. Con otro
  servidor, `/save` se monta con IDBFS (IndexedDB del navegador, de esa
  dirección y puerto).
- **El sonido**: los navegadores lo dejan recién después de que la persona
  toca algo; la página despierta el audio de SDL con la primera tecla o clic.
- **El hilo de carga** (`jobs.c`) no existe en el navegador (sin pthreads):
  lo que iba a ese hilo se hace en el momento, y lo que se puede saltear
  (`jobs_push_idle`, precargar sonidos) no se hace.
- **La red local** pasa por `d2port-servidor` (ver abajo); sin él, el menú
  avisa que hace falta. Al cambiar el idioma la página se recarga (sin las
  cinemáticas del principio).

## d2port-servidor

Un programa en C sin dependencias (`tools/d2port-servidor.c`, estático; se
compila con `scripts/build-web.sh` para Linux y, con mingw-w64, para
Windows), un hilo por conexión. Va en la carpeta de `index.html` y sirve:

| Pedido | Qué hace |
|---|---|
| `GET`/`HEAD` de cualquier archivo | La página, el juego y `data/` (sin salir de la carpeta: nada de `..`, `\` ni `:`) |
| `GET /servidor` | `{"servidor":"d2port-servidor","version":…,"ip":…}`: la página sabe que está y la IP para mostrar al crear una partida |
| `GET /partidas`, `PUT`/`DELETE /save/…` | Las partidas de `save/` (escribe en un `.tmp` y lo reemplaza de una vez; nombres con letras, números, `._-/`) |
| WebSocket `/red/conectar?a=IP&p=6112` | Unirse: abre la conexión TCP con esa máquina (espera hasta 3 s) y pasa los bytes tal cual en las dos direcciones |
| WebSocket `/red/anfitrion?p=6112` | Crear una partida: escucha en el puerto (TCP para unirse, UDP para contestar a los que buscan) por la página; cada conexión que llega va y viene por ese WebSocket con un número (`[1][n]` llegó, `[2][n][datos]`, `[3][n]` se fue; la página manda `[2][n][datos]`, `[3][n]` cortar, `[4]texto` la respuesta a los que buscan y `[5]` ya no entra nadie) |
| `GET /red/buscar?p=6112` | La pregunta `D2PN?` por UDP a cada red, a 255.255.255.255 y a esta misma; contesta lo que respondieron en medio segundo, `IP<tab>D2PN!…` por renglón |

Del lado del juego, `src/net.c` con `__EMSCRIPTEN__` usa eso en vez de los
sockets (`web/red.js`, la otra `--pre-js`; la página pone `D2_SERVIDOR=1`,
que habilita RED LOCAL en el menú, y `D2_SERVIDOR_IP`, la IP que se muestra
al crear una partida): los mensajes del juego son los
mismos, así que el navegador juega con la Pi, Linux y Windows (de la misma
versión: `package-release.sh` revisa también el `D2PN2 <commit>` de
`d2port.wasm`).

La seguridad: escucha solo en `127.0.0.1`; el `Host` tiene que ser
`localhost`/`127.0.0.1`/`[::1]` con su puerto (contra el DNS rebinding) y lo
que cambia algo o abre conexiones (`PUT`, `DELETE`, los WebSocket,
`/red/buscar`) solo lo puede pedir su propia página (el `Origin`; sin
`Origin` es un programa de esta computadora): otra página abierta en el
navegador no puede usarlo para conectarse a la red. `d2port-servidor
--prueba` revisa el SHA-1 del saludo, las rutas, los pedidos y los mensajes
del WebSocket.

## Medido (26/10/2026, Chrome en Windows, `servir.ps1`)

- El paquete: `d2port.wasm` 3,5 MB, `d2port.js` 180 KB.
- Del clic en Jugar al menú: la precarga en 1,4 s (840 archivos, unos 40 MB) y
  el juego listo a los ~2,6 s (antes de la precarga, ~20 s pidiendo de a uno).
- Cada archivo pedido de a uno: ~5 ms con `servir.ps1` (desde WSL a Windows,
  ~160 ms: no sirve para medir).
- Jugando en el pueblo del Acto 1: dibujo ~6 ms y lógica ~3 ms por cuadro (con
  la página a la vista va a 60 cuadros; tapada, ~20).
- Las partidas quedan al recargar; las cinemáticas se ven (se bajan enteras
  antes de pasar: la presentación son 56 MB).
- La red local con `d2port-servidor` (Windows, todo en esta PC con
  `D2_NET_BIND=127.0.0.1` para que no pregunte el firewall): el navegador
  crea la partida desde el menú y `d2port.exe` se une; `d2port.exe` la crea y
  el navegador la encuentra sola en "Unirse" y entra. 15000 ticks con los dos
  bots (`--autotest act1`; el del navegador con `--headless`, por el enganche
  de prueba de abajo): los 60 resúmenes (`D2_HASH=250`) iguales a los de dos
  `d2port.exe`, "sin separarse".
- Ojo al probar: la prueba automática pone a sus jugadores invulnerables
  (`s_god`) y mata de un golpe; un navegador jugando normal contra un
  `d2port.exe` con `--autotest` se separa en el primer golpe (en el tick 1676
  del Acto 1, semilla 7). Las dos máquinas tienen que jugar con las mismas
  reglas.

Para probar en el navegador con variables de entorno y argumentos (como las
pruebas sin ventana): en la consola de la página,
`sessionStorage.d2_prueba = JSON.stringify({env: {D2_NET_JOIN: '127.0.0.1', D2_HASH: '250'}, args: ['--headless', '--autotest', 'act1', '--players', '1']})`,
recargar y Jugar (se usa una vez). La salida queda en la consola.

## Lo que queda

- La primera vez que se entra a una zona nueva, sus archivos se piden y el
  juego espera (de décimas a un segundo).
- Safari y los celulares no están probados (en el iPhone el almacenamiento y
  los controles complican). No hay `d2port-servidor` para Mac: ahí sirve
  cualquier servidor local (partidas en el navegador, sin red local).
- Un importador dentro de la página (leyendo la carpeta del Resurrected con
  CascLib compilada a WebAssembly) no está: se usan los importadores de
  Windows y Linux.
- La red local no se probó todavía con otra máquina de verdad (una Pi u otra
  PC en la red): solo en esta PC, por 127.0.0.1, y con el firewall de
  Windows sin preguntar. Tampoco con personas jugando del lado del navegador
  durante mucho tiempo.
- `d2port-servidor` solo escucha a esta computadora: para jugar desde otro
  aparato de la casa (un celular) haría falta abrirlo a la red, con lo que
  eso implica (cualquiera de la red podría usar el puente).
