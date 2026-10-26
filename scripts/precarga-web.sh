#!/bin/bash
# precarga-web.sh [BINARIO]: rearma las listas de precarga de la versión para el navegador (web/precarga.txt y
# web/precarga-juego.txt, ver docs/web.md): lo que lee el juego hasta el menú, y lo que agrega al entrar a jugar.
# Usa el juego nativo (por defecto build/d2port) con D2_FS_LOG, desde una carpeta con data/ (D2RUN, por defecto
# la del código). Si cambia lo que se carga al arrancar y no se rearman, la versión web anda igual (más lenta).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
bin="${1:-$root/build/d2port}"
run="${D2RUN:-$root}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cd "$run"
mkdir -p "$tmp/sv" "$tmp/sv2"
D2_FS_LOG="$tmp/menu.txt" D2_SAVE_DIR="$tmp/sv" D2_NOSOUND=1 D2_MENU_SCRIPT=" " "$bin" --headless --frames 5 > /dev/null 2>&1
D2_FS_LOG="$tmp/juego.txt" D2_SAVE_DIR="$tmp/sv2" D2_NOSOUND=1 "$bin" --headless --players 1 --frames 60 > /dev/null 2>&1
# (solo lo de la lista del importador que existe: lo que el juego solo pregunta si está, o lo personal como los
# dibujos .d2rw, no se pide)
LC_ALL=C sort -u "$root/tools/archivos-del-juego.txt" > "$tmp/lista.txt"
existing() {
    tr 'A-Z\\' 'a-z/' < "$1" | LC_ALL=C sort -u | LC_ALL=C comm -12 - "$tmp/lista.txt" | while IFS= read -r f; do
        [ -f "$run/$f" ] && echo "$f"
    done
}
existing "$tmp/menu.txt" > "$tmp/menu-u.txt"
existing "$tmp/juego.txt" > "$tmp/juego-u.txt"
{
    echo "# Lo que lee el juego hasta el menú: la página lo baja en paralelo antes de arrancar (scripts/precarga-web.sh)"
    cat "$tmp/menu-u.txt"
} > "$root/web/precarga.txt"
{
    echo "# Lo que agrega entrar a jugar: la página lo baja en segundo plano (scripts/precarga-web.sh)"
    LC_ALL=C comm -13 "$tmp/menu-u.txt" "$tmp/juego-u.txt"
} > "$root/web/precarga-juego.txt"
wc -l "$root/web/precarga.txt" "$root/web/precarga-juego.txt"
