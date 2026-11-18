#!/usr/bin/env bash
# install_shell.sh - Compila e instala wiiu-shell en el sistema o en la Pi
set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET_HOST="${1:-}"

if [ -z "${TARGET_HOST}" ]; then
    echo "[*] Instalando wiiu-shell localmente en /usr/local/bin y /etc/wiiu-shell/..."
    cd "${SCRIPT_DIR}"
    make
    sudo install -Dm755 wiiu-shell /usr/local/bin/wiiu-shell
    sudo mkdir -p /etc/wiiu-shell
    sudo cp -f config.json /etc/wiiu-shell/config.json
    echo "[✓] Instalado con éxito. Ejecuta con: wiiu-shell --config /etc/wiiu-shell/config.json"
    exit 0
fi

echo "[*] Desplegando en la Raspberry Pi (${TARGET_HOST})..."
ssh -o BatchMode=yes "${TARGET_HOST}" "mkdir -p ~/wiiu-shell-build"
scp -r "${SCRIPT_DIR}"/* "${TARGET_HOST}:~/wiiu-shell-build/"
ssh -o BatchMode=yes "${TARGET_HOST}" "cd ~/wiiu-shell-build && make && sudo install -Dm755 wiiu-shell /usr/local/bin/wiiu-shell && sudo mkdir -p /etc/wiiu-shell && sudo cp -f config.json /etc/wiiu-shell/config.json"
echo "[✓] Desplegado con éxito en ${TARGET_HOST}."
