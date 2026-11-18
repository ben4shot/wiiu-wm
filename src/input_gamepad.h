#ifndef INPUT_GAMEPAD_H
#define INPUT_GAMEPAD_H

#include "config.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NAV_NONE = 0,
    NAV_UP,
    NAV_DOWN,
    NAV_LEFT,
    NAV_RIGHT,
    NAV_ACTION_A,     // Aceptar / Lanzar
    NAV_ACTION_B,     // Volver / Cancelar
    NAV_ACTION_X,     // Cerrar la tarjeta enfocada en Recientes
    NAV_ACTION_Y,     // Mosaico de la tarjeta enfocada en Recientes
    NAV_ACTION_HOME,  // Botón HOME (Máquina de estados: App -> Recientes -> Inicio)
    NAV_ACTION_PLUS,  // Panel de ajustes rápidos
    NAV_PAGE_PREV,    // Gatillo L
    NAV_PAGE_NEXT,    // Gatillo R
    NAV_ACTION_MINUS  // Botón -
} NavAction;

#define NAV_QUEUE_LEN 16

typedef struct {
    int fd;
    char device_path[MAX_STR];
    char device_name[128];
    bool is_open;
    bool nintendo_layout;   // A = BTN_EAST (como emite drc_stream para el Wii U GamePad)
    bool has_dpad_buttons;  // si emite BTN_DPAD_*, se ignora el HAT (llegan los dos)
    uint32_t last_scan_ms;

    // Estado de ejes analógicos para evitar repeticiones excesivas
    int stick_x_prev;
    int stick_y_prev;

    // Valores crudos de los sticks (el editor mueve/redimensiona con ellos de forma continua)
    int abs_lx, abs_ly, abs_rx, abs_ry;
    bool stick_nav;         // los sticks generan NAV_* (false en el editor)

    NavAction queue[NAV_QUEUE_LEN];
    int q_head, q_len;

    const DesktopConfig *cfg;
} GamepadInput;

bool gamepad_init(GamepadInput *gp, const DesktopConfig *cfg);
void gamepad_close(GamepadInput *gp);
// Reintenta abrir el mando si aún no existe (drc_stream lo crea después que el shell)
void gamepad_maintain(GamepadInput *gp, uint32_t now_ms);
NavAction gamepad_poll(GamepadInput *gp);

// Utilidad común: ¿el bit `bit` está activo en un mapa de bits de evdev?
bool evdev_test_bit(const unsigned long *bits, unsigned int bit);

#endif // INPUT_GAMEPAD_H
