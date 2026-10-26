#!/bin/bash
# build-web.sh: compila la versión para el navegador (build-web/d2port.js y d2port.wasm) con Emscripten (ver
# docs/web.md). EMSDK: la carpeta del SDK (por defecto ~/emsdk). Se instala una vez con:
#   git clone https://github.com/emscripten-core/emsdk ~/emsdk && ~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest
# También d2port-servidor para Linux y Windows (build-web/servidor/). Después, scripts/package-release.sh arma
# publicar/diablo2-port-web.zip con lo de web/.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
emsdk="${EMSDK:-$HOME/emsdk}"
if [ ! -f "$emsdk/emsdk_env.sh" ]; then
    echo "No está Emscripten en $emsdk (ver el comienzo de este archivo)"
    exit 1
fi
# shellcheck disable=SC1091
source "$emsdk/emsdk_env.sh" > /dev/null 2>&1
emcmake cmake -S "$root" -B "$root/build-web" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$root/build-web" --target d2port --clean-first -j"$(nproc)" 2>&1 | grep -E " error|warning:|Built target" || true
ls -la "$root/build-web/d2port.js" "$root/build-web/d2port.wasm"

# d2port-servidor (tools/d2port-servidor.c) para Linux y Windows: sirve la página, guarda las partidas en save/ y
# hace de puente de la red local. Estáticos, para que no dependan de nada instalado; sin mingw-w64, solo Linux
ver=$(git -C "$root" rev-parse --short=12 HEAD 2>/dev/null || echo dev)
srv="$root/build-web/servidor"
mkdir -p "$srv/linux" "$srv/windows"
gcc -O2 -Wall -Wextra -static -s -pthread -DD2_VERSION="\"$ver\"" "$root/tools/d2port-servidor.c" -o "$srv/linux/d2port-servidor"
"$srv/linux/d2port-servidor" --prueba > /dev/null || { echo "falló la prueba de d2port-servidor"; exit 1; }
if command -v x86_64-w64-mingw32-gcc > /dev/null; then
    x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -static -s -DD2_VERSION="\"$ver\"" "$root/tools/d2port-servidor.c" \
        -o "$srv/windows/d2port-servidor.exe" -lws2_32 -liphlpapi -lshell32
fi
ls -la "$srv"/*/d2port-servidor*
