#include "wayland_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <stdbool.h>

static WaylandUI g_ui;

static void sig_handler(int sig) {
    (void)sig;
    printf("\n[Shell] Señal recibida, cerrando entorno de escritorio...\n");
    g_ui.running = false;
}

int main(int argc, char *argv[]) {
    const char *config_path = "/etc/wiiu-shell/config.json";
    bool editor = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--editor") == 0) {
            editor = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Uso: %s [--editor] [--config ruta/a/config.json]\n", argv[0]);
            printf("  --config  configuración de respaldo; si existe ~/.config/wiiu-shell/config.json se usa esa\n");
            printf("  --editor  abre el editor del menú (guarda en ~/.config/wiiu-shell/config.json)\n");
            printf("Entorno de escritorio ligero y configurable estilo Wii U en Wayland nativo (C).\n");
            return 0;
        }
    }

    // Log útil aunque stdout vaya a un fichero (labwc -s, systemd)
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("=========================================\n");
    printf("   Wii U GamePad Native Wayland Shell    \n");
    printf("=========================================\n");
    printf("[*] Cargando configuración desde: %s\n", config_path);

    if (!wayland_ui_init(&g_ui, config_path, editor)) {
        fprintf(stderr, "[Fatal] No se pudo inicializar la interfaz Wayland.\n");
        return 1;
    }

    printf("[*] Entorno iniciado correctamente.\n");
    printf("    - HOME: Alterna entre Recientes (Android Overview) e Inicio (Grid Wii U)\n");
    printf("    - Gestos desde el borde: abajo->arriba (HOME), arriba->abajo (ajustes), laterales (cambiar app)\n");
    printf("    - Mando: cruceta/stick (moverse), A (abrir), B (atrás), X (cerrar en Recientes), + (ajustes), L/R (página)\n");

    wayland_ui_run(&g_ui);

    wayland_ui_cleanup(&g_ui);
    printf("[*] Entorno finalizado con éxito.\n");
    return 0;
}
