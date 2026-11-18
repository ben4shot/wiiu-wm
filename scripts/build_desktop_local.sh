#!/usr/bin/env bash
# Compila en ESTE equipo wiiu-wm (el escritorio propio del GamePad, el mismo de la Pi) y lo deja en
# build_local/prefix/bin del repo drc, donde lo busca drchost/start_drc.sh en modo separate. No necesita root.
#   scripts/build_desktop_local.sh [ruta/al/repo/drc]     (por defecto ../drc junto a este repo, o $DRC_DIR)
# Dependencias (Arch):  sudo pacman -S --needed wlroots0.20 json-c cairo xcb-util-wm foot
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRC_DIR="${1:-${DRC_DIR:-$ROOT/../drc}}"
[ -f "$DRC_DIR/drchost/start_drc.sh" ] || {
    echo "No encuentro el repo drc en «$DRC_DIR»: pásalo como argumento o con DRC_DIR=ruta." >&2; exit 1; }
DRC_DIR="$(cd "$DRC_DIR" && pwd)"
pkg-config --exists wlroots-0.20 || { echo "Falta wlroots 0.20 (Arch: sudo pacman -S --needed wlroots0.20)." >&2; exit 1; }
BUILD="$(mktemp -d)"; trap 'rm -rf "$BUILD"' EXIT
# Se compila en una copia: los .o del árbol pueden ser de otra arquitectura y el disco puede no admitir permisos Unix
cp -r "$ROOT/Makefile" "$ROOT/src" "$ROOT/protocols" "$BUILD/"
find "$BUILD" \( -name '*.o' -o -name '*.d' \) -delete
make -C "$BUILD" -j"$(nproc)" wm
install -Dm755 "$BUILD/wiiu-wm" "$DRC_DIR/build_local/prefix/bin/wiiu-wm"
echo "OK: $DRC_DIR/build_local/prefix/bin/wiiu-wm"
command -v foot >/dev/null 2>&1 || echo "Aviso: no está «foot» (el terminal del menú del escritorio). Arch: sudo pacman -S foot"
