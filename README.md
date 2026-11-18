# wiiu-wm

Escritorio propio del Wii U GamePad (compositor Wayland + shell) de 854x480. Es opcional para el repo `drc`: se elige con
`DESKTOP_SESSION="wiiu-wm"` en `drc.conf`.

## Requisitos (antes de compilar)

| | Arch | Debian / Ubuntu |
|---|---|---|
| Compilador y make | `base-devel` | `build-essential` |
| pkg-config | `pkgconf` | `pkg-config` |
| wlroots 0.20 | `wlroots0.20` | `libwlroots-0.20-dev` |
| Wayland (cliente, servidor, scanner, protocolos) | `wayland wayland-protocols` | `libwayland-dev wayland-protocols` |
| Gráficos y teclado | `cairo pixman libxkbcommon mesa` | `libcairo2-dev libpixman-1-dev libxkbcommon-dev libegl-dev libgles-dev` |
| JSON | `json-c` | `libjson-c-dev` |
| XWayland (apps X11) | `xcb-util-wm` | `libxcb-ewmh-dev libxcb-icccm4-dev` |
| Terminal del menú | `foot` | `foot` |

## Compilar

`make` compila el shell (`wiiu-shell`) y `make wm` el compositor (`wiiu-wm`, necesita wlroots 0.20); `make test` corre las pruebas (ver `Makefile`).
`scripts/build_desktop_local.sh [ruta/a/drc]` compila el compositor y lo deja en `build_local/prefix/bin` del repo `drc`
(por defecto `../drc`, junto a este repo, o `DRC_DIR`), donde lo encuentra `drchost/start_drc.sh`.
