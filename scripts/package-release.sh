#!/bin/bash
# Arma publicar/: lo que se puede subir a GitHub, sin ningún archivo de Blizzard.
#
#   publicar/diablo2-port/      el port compilado para Batocera (la Pi) y el
#                               importador para Linux: cada uno saca de su
#                               propio Diablo II: Resurrected los archivos que
#                               usa el port (los de archivos.txt, no el juego
#                               entero)
#   publicar/diablo2-port-windows/  el port para Windows (d2port.exe, SDL2.dll),
#                               su importador y dos .bat para importar
#   publicar/diablo2-port-web/  la versión para el navegador (WebAssembly, docs/web.md): se abre con
#                               d2port-servidor (Linux y Windows), que guarda las partidas en save/ y hace de
#                               puente de la red local; los importadores copian el juego a su data/
#   publicar/*.zip              lo mismo comprimido, para las descargas
#
# La skin personal del Brujo (sus dibujos editados, .d2rw) no se publica.
#
# Antes: compilar build-pi (ver package-batocera.sh), build-win (Windows:
# cmake/mingw-w64.cmake; SDL2_MINGW la carpeta de SDL2 para MinGW) y los
# importadores (scripts/build-importador.sh). Los que falten se avisan y se saltan.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
pub="$root/publicar"
port="$pub/diablo2-port"
imp="${IMPORTADORES:-$HOME/build-importador/bin}"

[ -d "$pub" ] && rm -rf "$pub"
mkdir -p "$port"

# El paquete de Batocera sin los archivos del juego (con la lista que usa el importador)
OUT="$port/batocera/ports" SIN_DATOS=1 bash "$root/scripts/package-batocera.sh" >/dev/null
game="$port/batocera/ports/.data/d2port"

# Los importadores: el de Linux para la PC; el de la Pi va adentro del
# paquete, con dos entradas en el menú de ports para importar desde la Pi
# (el de Windows, cuando esté probado)
missing=()
copy_imp() { # sistema destino
    local src="$imp/$1/d2port-importar"
    [ "$1" = windows ] && src="$src.exe"
    if [ -f "$src" ]; then
        mkdir -p "$2"
        cp "$src" "$2/"
    else
        missing+=("$1")
    fi
}
copy_imp linux "$port/importador/linux"
copy_imp pi "$game"
if [ -f "$game/d2port-importar" ]; then
    for lang in es en; do
        if [ $lang = es ]; then name="Importar Diablo II (Español).sh"; else name="Import Diablo II (English).sh"; fi
        cat > "$port/batocera/ports/$name" <<EOF
#!/bin/bash
# Copia los archivos del juego desde Diablo II: Resurrected (una carpeta del
# juego en un USB, en /userdata o en roms/ports) al port
DIR="\$(dirname "\$(readlink -f "\$0")")"
"\$DIR/.data/d2port/d2port-importar" --idioma $lang --sin-pausa > /userdata/system/logs/d2port-importar.log 2>&1
EOF
        chmod +x "$port/batocera/ports/$name"
        # Desde Steam (docs/steam.md): la pantalla del juego muestra el QR para iniciar sesión y el avance
        if [ $lang = es ]; then name="Importar desde Steam BETA (Español).sh" code=esMX; else name="Import from Steam BETA (English).sh" code=enUS; fi
        cat > "$port/batocera/ports/$name" <<EOF
#!/bin/bash
# Baja Diablo II: Resurrected de tu cuenta de Steam (escaneando un código QR con la app de Steam) y lo importa
DIR="\$(dirname "\$(readlink -f "\$0")")"
cd "\$DIR/.data/d2port" || exit 1
./d2port --importar-steam $code > /userdata/system/logs/d2port-steam.log 2>&1
EOF
        chmod +x "$port/batocera/ports/$name"
    done
fi
# Linux (PC): lo mismo desde Steam, en la terminal
if [ -f "$port/importador/linux/d2port-importar" ]; then
    for lang in es en; do
        if [ $lang = es ]; then name="Importar desde Steam BETA (Español).sh" code=esMX; else name="Import from Steam BETA (English).sh" code=enUS; fi
        printf '#!/bin/sh\n# Baja Diablo II: Resurrected de tu cuenta de Steam y lo importa (docs/steam.md)\ncd "$(dirname "$0")" && ./d2port-importar --steam --idioma %s\n' \
            "$code" > "$port/importador/linux/$name"
        chmod +x "$port/importador/linux/$name"
    done
fi
cp "$root/README.md" "$root/LICENSE" "$root/THIRD-PARTY-NOTICES.md" "$port/" # las instrucciones (en inglés) y las licencias

# Windows: el juego, SDL2.dll, el importador, la lista y dos .bat para importar (en una sola carpeta:
# el importador copia a su data/ y el juego guarda en su save/)
# Windows: "Importar desde Steam" (docs/steam.md) en una consola, que muestra el QR para iniciar sesión
steam_bat() { # código-de-idioma carpeta
    local name="Import from Steam BETA (English).bat"
    [ "$1" = esMX ] && name="Importar desde Steam BETA (Español).bat"
    printf '@echo off\r\nrem Baja Diablo II: Resurrected de tu cuenta de Steam y lo importa a esta carpeta\r\ncd /d "%%~dp0"\r\n"%%~dp0d2port-importar.exe" --steam --idioma %s\r\n' \
        "$1" > "$2/$name"
}
win="$pub/diablo2-port-windows"
sdl="${SDL2_MINGW:-$HOME/sdl2-mingw/SDL2-2.30.12/x86_64-w64-mingw32}"
if [ -f "$root/build-win/d2port.exe" ] && [ -f "$sdl/bin/SDL2.dll" ] && [ -f "$imp/windows/d2port-importar.exe" ]; then
    mkdir -p "$win"
    cp "$root/build-win/d2port.exe" "$sdl/bin/SDL2.dll" "$imp/windows/d2port-importar.exe" "$win/"
    cp "$game/archivos.txt" "$win/"
    for lang in es en; do
        if [ $lang = es ]; then name="Importar Diablo II (Español).bat" code=esMX; else name="Import Diablo II (English).bat" code=enUS; fi
        # (con la ruta entera: con NoDefaultCurrentDirectoryInExePath, cmd no busca en la carpeta actual)
        printf '@echo off\r\nrem Copia los archivos del juego desde tu Diablo II: Resurrected a esta carpeta\r\ncd /d "%%~dp0"\r\n"%%~dp0d2port-importar.exe" --idioma %s\r\n' \
            "$code" > "$win/$name"
        steam_bat "$code" "$win"
    done
    cp "$root/README.md" "$root/LICENSE" "$root/THIRD-PARTY-NOTICES.md" "$win/"
else
    missing+=(windows-juego)
fi

# El navegador (docs/web.md): la página, el juego en WebAssembly (build-web: scripts/build-web.sh), la lista de
# archivos, las de precarga, d2port-servidor de Linux y de Windows (sirve la página, guarda las partidas en save/
# y hace de puente de la red local; también de scripts/build-web.sh) y los importadores de Windows y Linux, que
# copian los archivos del juego a su data/. Sin publicar ninguna página: cada uno lo abre en su computadora (o
# lo sube a donde quiera, con sus archivos: con otro servidor, las partidas quedan en el navegador y no hay red)
web="$pub/diablo2-port-web"
srv="$root/build-web/servidor"
if [ -f "$root/build-web/d2port.wasm" ] && [ -f "$root/build-web/d2port.js" ]; then
    mkdir -p "$web"
    cp "$root/build-web/d2port.js" "$root/build-web/d2port.wasm" "$web/"
    cp "$root/web/index.html" "$root/web/precarga.txt" "$root/web/precarga-juego.txt" "$web/"
    cp "$game/archivos.txt" "$web/"
    for s in linux/d2port-servidor windows/d2port-servidor.exe; do
        if [ -f "$srv/$s" ]; then
            cp "$srv/$s" "$web/"
        else
            missing+=("servidor-${s%%/*}")
        fi
    done
    [ -f "$web/d2port-servidor" ] && chmod +x "$web/d2port-servidor"
    if [ -f "$imp/windows/d2port-importar.exe" ]; then
        cp "$imp/windows/d2port-importar.exe" "$web/"
        for lang in es en; do
            if [ $lang = es ]; then name="Importar Diablo II (Español).bat" code=esMX; else name="Import Diablo II (English).bat" code=enUS; fi
            printf '@echo off\r\nrem Copia los archivos del juego desde tu Diablo II: Resurrected a esta carpeta\r\ncd /d "%%~dp0"\r\n"%%~dp0d2port-importar.exe" --idioma %s\r\n' \
                "$code" > "$web/$name"
            steam_bat "$code" "$web"
        done
    else
        missing+=(web-windows)
    fi
    if [ -f "$imp/linux/d2port-importar" ]; then
        cp "$imp/linux/d2port-importar" "$web/"
        chmod +x "$web/d2port-importar"
        for lang in es en; do
            if [ $lang = es ]; then name="Importar Diablo II (Español).sh" code=esMX; else name="Import Diablo II (English).sh" code=enUS; fi
            printf '#!/bin/sh\n# Copia los archivos del juego desde tu Diablo II: Resurrected a esta carpeta\ncd "$(dirname "$0")" && ./d2port-importar --idioma %s\n' \
                "$code" > "$web/$name"
            chmod +x "$web/$name"
            if [ $lang = es ]; then name="Importar desde Steam BETA (Español).sh"; else name="Import from Steam BETA (English).sh"; fi
            printf '#!/bin/sh\n# Baja Diablo II: Resurrected de tu cuenta de Steam y lo importa a esta carpeta (docs/steam.md)\ncd "$(dirname "$0")" && ./d2port-importar --steam --idioma %s\n' \
                "$code" > "$web/$name"
            chmod +x "$web/$name"
        done
    else
        missing+=(web-linux)
    fi
    cp "$root/README.md" "$root/LICENSE" "$root/THIRD-PARTY-NOTICES.md" "$web/"
else
    missing+=(web)
fi

# Que la Pi, Windows y el navegador sean de la misma versión del código (la red local junta máquinas de la misma:
# compilar después del último commit, volviendo a configurar: cmake -S . -B build-pi / build-win; build-web.sh)
ver=$(git -C "$root" rev-parse --short=12 HEAD 2>/dev/null || echo dev)
for b in "$game/d2port" "$pub/diablo2-port-windows/d2port.exe" "$web/d2port.wasm"; do
    if [ -f "$b" ] && ! grep -q "D2PN2 $ver" "$b"; then
        echo "ERROR: $b no es de la versión $ver: volvé a configurar y compilar (cmake -S . -B build-pi o build-win, scripts/build-web.sh)"
        exit 1
    fi
done

# Que no se cuele nada del juego ni dibujos editados
leaks=$(cd "$pub" && find . -type f \( -path '*/data/*' -o -name '*.d2rw' \) | head -5)
if [ -n "$leaks" ]; then
    echo "ERROR: hay archivos del juego en publicar/:"
    echo "$leaks"
    exit 1
fi

# Los .zip (los permisos de ejecución quedan en el zip)
( cd "$pub" && python3 - <<'EOF'
import os, zipfile
for name in [n for n in ('diablo2-port', 'diablo2-port-windows', 'diablo2-port-web') if os.path.isdir(n)]:
    with zipfile.ZipFile(name + '.zip', 'w', zipfile.ZIP_DEFLATED) as z:
        for base, dirs, files in os.walk(name):
            dirs.sort()
            for f in sorted(files):
                path = os.path.join(base, f)
                info = zipfile.ZipInfo.from_file(path)
                info.compress_type = zipfile.ZIP_DEFLATED
                with open(path, 'rb') as fh:
                    z.writestr(info, fh.read())
EOF
)
du -sh "$port" "$pub"/*.zip
[ -d "$win" ] && du -sh "$win"
[ -d "$web" ] && du -sh "$web"
[ ${#missing[@]} -eq 0 ] || echo "Faltan (los importadores: scripts/build-importador.sh; el servidor: scripts/build-web.sh): ${missing[*]}"
echo "Listo: $pub"
