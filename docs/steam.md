# Importar desde Steam (beta)

**Beta**: sin probar todavía con una cuenta de Steam de verdad (ver abajo); así se llama en los paquetes y la pantalla.

Pedido del usuario (26/10/2026): "el import directo desde Steam", lo que usa
mucho la comunidad de ports de Android (GameNative, Pluvia y GameHub inician
sesión en Steam y bajan los juegos de la cuenta; en las portátiles con Linux,
las guías de PortMaster usan `download_depot` o DepotDownloader). Diablo II:
Resurrected está en Steam desde el 11/02/2026: la **Infernal Edition**, app
**2536520** (con Lord of Destruction y Reign of the Warlock; solo Windows,
`D2R.exe`; para jugarla pide vincular una cuenta de Battle.net, pero para leer
sus archivos no). Las herramientas de mods (D2RMM, que usa CascLib) andan con
la versión de Steam: trae el mismo almacenamiento CASC que la de Battle.net
(`.build.info` y `Data/`).

Se usa así: con Steam se comprueba que el juego es de la persona (Steam solo
entrega la clave de los archivos a una cuenta que lo tiene). Bajarlo del
servidor de Blizzard sin iniciar sesión se podría, pero no prueba nada: no se
hace.

## Cómo funciona

`d2port-importar --steam` (`tools/importar-steam.inc`, incluido en
`tools/d2port-importar.c`):

1. **DepotDownloader** (github.com/SteamRE/DepotDownloader, GPL-2.0, .NET
   autocontenido): no va en los paquetes. Si no está en `<paquete>/steam/`,
   se baja con `curl` (en Windows, el `curl.exe` del sistema; en Linux, `curl`
   o `wget`) la versión fija `DD_TAG` (3.4.0) para ese sistema
   (`windows-x64`, `linux-x64`, `linux-arm64` para la Pi), se compara su
   SHA-256 con el anotado y se saca el ejecutable del `.zip` con zlib (la que
   trae CascLib; no hace falta `unzip`). Su licencia queda al lado. Se corre
   con `DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1` (sin ICU, que Batocera no
   trae).
2. **Las listas** (`-manifest-only`): la primera corrida inicia sesión (`-qr`,
   o `-username` con `--usuario`) con `-remember-password` y baja solo las
   listas de archivos (`manifest_<depot>_<id>.txt`) de los depots del idioma
   elegido (`-language`: `latam`, `english`, `spanish`, `german`...). De ahí
   salen cuánto pesa y qué depots traen `.build.info` o algo de `Data/` (lo
   que lee CascLib). DepotDownloader escribe "Next time you can login with
   -username X": la segunda corrida usa esa cuenta y el QR no se vuelve a
   escanear.
3. **El lugar**: lo que falta bajar (menos lo que ya esté de una vez anterior)
   más 512 MB. Si no entra y la carpeta no la eligió la persona, busca otro
   disco con lugar en `/media`, `/run/media/<usuario>` o `/mnt` (un USB en
   Batocera) y baja ahí (`d2port-steam-descarga`).
4. **La descarga**: `-depot` de esos depots y `-filelist` con `.build.info` y
   `regex:^data/` (sin el ejecutable ni lo demás), en
   `<paquete>/steam/descarga` (o `--descarga`). Si se corta, la próxima vez
   sigue donde quedó (DepotDownloader revisa lo que ya está).
5. **La importación** de siempre desde esa carpeta y, en
   `importar-steam.log`, de qué `data.###` salió lo importado
   (`CascGetFileInfo`, `SegmentIndex`): cuántos de los archivos grandes hacen
   falta de verdad. Si salió todo bien, borra lo bajado (solo `Data/`,
   `.build.info`, `.DepotDownloader/` y las listas, nunca la carpeta entera:
   con `--descarga` puede ser de la persona), salvo `--conservar-descarga`.

En la PC corre en una consola (los `Importar desde Steam (...).bat/.sh` de los
paquetes): la salida de DepotDownloader pasa tal cual, con el QR hecho de "██"
y las preguntas de la contraseña si se usa `--usuario`.

**En la Pi** no hay consola a la vista (los scripts de Ports guardan la salida
en un registro): `Importar desde Steam BETA (Español).sh` corre
`d2port --importar-steam esMX` (`src/steamui.c`), una pantalla que corre el
importador con `--ui` y muestra el QR grande (lo arma de los renglones de "██",
dos caracteres por módulo), la fase (`@@fase`), la barra de avance
(`@@progreso`, de los porcentajes de DepotDownloader y de la copia) y los
últimos mensajes. Dibuja sin los archivos del juego, que todavía no están: la
letra es `src/font8x8.inc` (dominio público). B cancela (corta también a
DepotDownloader: el importador corre en su propio grupo de procesos); al
terminar, A sale. Todo queda en `/userdata/system/logs/d2port-steam.log`.

## Pruebas

- `d2port-importar --prueba-steam [zip nombre sha256]`: el SHA-256, las listas
  de Steam, la salida de DepotDownloader, las comillas de Windows y, con un
  `.zip`, sacar un archivo y comparar su SHA-256.
- `D2_DD=programa`: otro DepotDownloader. El de mentira de las pruebas
  (fuera del repositorio) imprime lo mismo que el de verdad (el QR, la
  cuenta, las listas, el avance) y escribe archivos de mentira, con
  `FAKE_DD_FAIL=login|descarga` y `FAKE_DD_GB=N` (cuánto pesa).
- `D2_STEAMUI_SCRIPT=archivo` (la salida del importador guardada) y
  `D2_STEAMUI_SHOT=archivo.bmp`: la pantalla de la Pi dibujada en un archivo,
  sin ventana.

Probado el 26/10/2026, con el DepotDownloader de mentira: Linux, Windows (con
espacios en las rutas) y la Pi (el importador de aarch64 y la pantalla
dibujada por el `d2port` de la Pi); el QR dibujado es módulo por módulo el que
se imprimió; el otro disco cuando no entra; los errores (no inicia sesión, se
corta la descarga). El SHA-256 de los tres DepotDownloader y sacarlos del
`.zip` (iguales a los de Python).

## Lo que no se probó

- **Con Steam de verdad**: ni DepotDownloader corriendo (no me dejaron
  ejecutarlo en esta PC), ni una cuenta con el juego. No se sabe todavía qué
  trae cada depot (Steam esconde sus nombres sin una cuenta que lo tenga):
  la primera corrida lo deja en la salida (los depots, sus archivos y su
  peso) e `importar-steam.log`.
- DepotDownloader en la Pi (.NET en aarch64 con 1 GB de memoria) y el QR en
  la consola de Windows con la letra de siempre.
- Cancelar con B en la pantalla de la Pi.

## Lo que sigue (cuando haya datos)

Bajar menos: los ~1,6 GB que usa el port están en algunos de los `data.###`
de ~1 GB. Con `importar-steam.log` se ve cuántos hacen falta; si son pocos,
se pueden pedir solo esos (`-filelist` con sus nombres, después de bajar los
`.idx` y `Data/config`, y de abrir el CASC con los que tienen ENCODING y la
raíz) o, con un ayudante propio sobre SteamKit2, solo los pedazos de 1 MB
que hacen falta.
