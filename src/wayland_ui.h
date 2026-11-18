#ifndef WAYLAND_UI_H
#define WAYLAND_UI_H

#include "config.h"
#include "app_manager.h"
#include "grid_view.h"
#include "recents_view.h"
#include "touch_gestures.h"
#include "input_gamepad.h"
#include "toplevels.h"
#include "editor.h"
#include "page_cache.h"

#include <wayland-client.h>
#include <cairo.h>
#include <stdbool.h>

typedef enum {
    STATE_HOME_GRID = 0,
    STATE_RECENTS,
    STATE_APP_RUNNING
} ShellState;

typedef struct {
    struct wl_buffer *wl_buf;
    void *shm_data;
    size_t size;
    bool busy;
    cairo_surface_t *cairo_surface;
} Buffer;

typedef struct {
    DesktopConfig cfg;
    AppManager app_mgr;
    GridView grid;
    PageCache page_cache;   // páginas pre-renderizadas del Inicio
    RecentsView recents;
    TouchGestureTracker touch_tracker;
    GamepadInput gamepad;
    ToplevelList toplevels;
    uint32_t recent_serials[MAX_RECENT_ITEMS]; // ventana de cada tarjeta de Recientes
    uint32_t last_app_serial;                  // última app que estuvo en primer plano

    // Modo editor («wiiu-shell --editor»): otra ventana con el editor del menú
    bool editor_mode;
    Editor editor;

    // Configuración: la del usuario (si existe) o la de respaldo; se recarga al cambiar
    char config_fallback[MAX_STR];
    char config_user[MAX_STR];
    int inotify_fd;

    ShellState current_state;
    ShellState previous_state;

    // Wayland core
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *xdg_wm_base;
    struct wl_seat *seat;
    struct wl_touch *touch;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    Buffer buffers[2];
    int current_buffer;

    int width;
    int height;
    uint32_t toast_seq;     // último aviso de AppManager mostrado
    uint32_t toast_until_ms;
    bool shell_active;      // la ventana del shell tiene el foco
    bool pointer_down;
    double pointer_x, pointer_y;
    bool configured;
    bool running;
    bool needs_redraw;
} WaylandUI;

bool wayland_ui_init(WaylandUI *ui, const char *config_file, bool editor_mode);
void wayland_ui_cleanup(WaylandUI *ui);
void wayland_ui_run(WaylandUI *ui);

void wayland_ui_handle_home_button(WaylandUI *ui);
void wayland_ui_render_frame(WaylandUI *ui);

#endif // WAYLAND_UI_H
