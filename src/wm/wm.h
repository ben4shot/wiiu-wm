#ifndef WM_H
#define WM_H

// wiiu-wm: entorno propio (compositor Wayland sobre wlroots 0.20) para la pantalla
// del Wii U GamePad. Integra el Inicio y Recientes, escritorios, mosaico tipo Hyprland
// y atajos configurables con HOME (mando) o Super (teclado).

#define WLR_USE_UNSTABLE 1

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/xwayland.h>
#include <xkbcommon/xkbcommon.h>

#include "../config.h"
#include "../app_manager.h"
#include "../grid_view.h"
#include "../recents_view.h"
#include "../page_cache.h"
#include "../input_gamepad.h"

#define WM_MAX_WS 9

enum wm_ui_mode { UI_HIDDEN = 0, UI_HOME, UI_RECENTS };

enum wm_view_type {
    WM_VIEW_XDG = 0,
    WM_VIEW_XWAYLAND,
};

struct wm_drawer_item {
    char name[128];
    char exec[MAX_STR];
    char icon[128];
    int app_index;      // elemento del menú (-1 = solo .desktop)
    bool terminal;
};

struct wm_server;

struct wm_view {
    struct wl_list link;            // server->views
    struct wm_server *server;
    enum wm_view_type type;
    union {
        struct wlr_xdg_toplevel *xdg;
        struct wlr_xwayland_surface *xwayland_surface;
    };
    struct wlr_scene_tree *tree;    // árbol de la ventana (dentro del escritorio)
    struct wlr_scene_tree *surface_tree;
    struct wlr_scene_rect *border[4];
    struct wlr_xdg_toplevel_decoration_v1 *decoration;

    uint32_t id;                    // orden de creación
    uint32_t focus_seq;             // última vez que tuvo el foco
    int workspace;                  // 0..WM_MAX_WS-1
    bool tiled;
    bool mapped;
    pid_t pid;
    struct wlr_box box;             // caja asignada por la disposición
    int logged_w, logged_h;         // último tamaño del cliente registrado (diagnóstico)

    struct wl_listener map, unmap, commit, destroy;
    struct wl_listener request_maximize, request_fullscreen, request_move, request_resize;
    struct wl_listener associate, dissociate, request_configure, request_activate;
    struct wl_listener set_title, set_class;
};

struct wm_keyboard {
    struct wl_list link;
    struct wm_server *server;
    struct wlr_keyboard *wlr_keyboard;
    struct wl_listener modifiers, key, destroy;
};

struct wm_touch {
    struct wl_list link;
    struct wm_server *server;
    struct wlr_touch *wlr_touch;
    struct wl_listener down, up, motion, cancel, frame, destroy;
};

struct wm_output {
    struct wl_list link;
    struct wm_server *server;
    struct wlr_output *wlr_output;
    struct wl_listener frame, request_state, destroy;
};

// Punto táctil activo: a quién se envía (una superficie o la interfaz)
struct wm_touch_point {
    int32_t id;
    bool active;
    bool to_ui;
    double ox, oy;                  // origen de la superficie en el diseño
    double down_x, down_y, x, y;
};

struct wm_server {
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_output_layout *output_layout;
    struct wl_list outputs;
    int width, height;

    // Capas del escenario (de abajo arriba)
    struct wlr_scene_tree *layer_tree[4];   // background, bottom (layer-shell)
    struct wlr_scene_buffer *wallpaper;
    struct wlr_scene_tree *windows;
    struct wlr_scene_tree *ws_tree[WM_MAX_WS];
    struct wlr_scene_tree *ui_tree;
    struct wlr_scene_buffer *ui_buffer;
    struct wlr_scene_buffer *osd_buffer;
    struct wlr_scene_tree *top_tree;        // top + overlay de layer-shell (teclado)

    struct wlr_xdg_shell *xdg_shell;
    struct wl_listener new_xdg_toplevel, new_xdg_popup;
    struct wlr_layer_shell_v1 *layer_shell;
    struct wl_listener new_layer_surface;
    struct wlr_xdg_decoration_manager_v1 *decoration_mgr;
    struct wl_listener new_decoration;
    struct wl_listener new_virtual_keyboard;

    struct wlr_compositor *compositor;
    struct wlr_xwayland *xwayland;
    struct wl_listener new_xwayland_surface, xwayland_ready;

    struct wlr_seat *seat;
    struct wlr_cursor *cursor;
    struct wl_listener new_input, new_output;
    struct wl_listener cursor_motion, cursor_motion_abs, cursor_button, cursor_axis, cursor_frame;
    struct wl_listener request_set_selection;
    struct wl_list keyboards;
    struct wl_list touches;
    bool has_pointer;

    struct wl_list views;           // más reciente primero
    uint32_t next_view_id;
    uint32_t focus_counter;
    struct wm_view *focused;
    int workspace;

    // Interfaz integrada (Inicio, Recientes, avisos)
    enum wm_ui_mode ui;
    DesktopConfig cfg;
    char config_fallback[MAX_STR];
    char config_user[MAX_STR];
    char config_path[MAX_STR];
    struct wl_event_source *frame_kick;   // temporizador que mantiene vivos los fotogramas de salida
    char folder[48];                // carpeta abierta en el Inicio ("" = Inicio normal); ver open_folder()
    PageCache page_cache;
    GridView grid;
    RecentsView recents;
    struct wm_view *recent_views[MAX_RECENT_ITEMS];
    AppManager apps;
    uint32_t toast_seq;
    char osd[160];
    uint32_t osd_until;
    struct wl_event_source *anim_timer;
    struct wl_event_source *build_timer;   // carga incremental de las páginas (page_cache_step)
    struct wl_event_source *inotify_source;
    int inotify_fd;
    struct wm_touch_point touch[10];

    // Mando: combinaciones con HOME (con el dispositivo capturado mientras se mantiene)
    GamepadInput gamepad;
    struct wl_event_source *gamepad_source;
    struct wl_event_source *gamepad_rescan;
    bool home_held, home_combo;
    bool gamepad_grabbed;
    bool super_held, super_combo;
    bool no_gamepad;                // --no-gamepad: no tocar el mando (instancias de prueba)

    // Cajón de aplicaciones bajo la página del Inicio (stick derecho)
    double home_scroll;             // 0 = página; height = cajón entero a la vista
    double home_target;             // destino de la animación (-1 = ninguno)
    int drawer_focus;
    int drawer_count;
    struct wm_drawer_item *drawer;

    // Barra (waybar): proceso lanzado por el entorno y su superficie layer-shell
    pid_t bar_pid;
    void *bar_layer;                // struct wm_layer * (wm_view.c)
    char state_path[MAX_STR];       // $XDG_RUNTIME_DIR/wiiu-wm.state (escritorio, ventanas)

    // Control externo (como «hyprctl dispatch»): una acción por línea
    char ctl_path[MAX_STR];
    int ctl_fd;
    struct wl_event_source *ctl_source;
};

/* wm_view.c */
void wm_views_init(struct wm_server *s);
void wm_arrange(struct wm_server *s);
void wm_focus_view(struct wm_server *s, struct wm_view *v);
void wm_focus_cycle(struct wm_server *s, int dir);
void wm_close_focused(struct wm_server *s);
void wm_view_close(struct wm_view *v);
void wm_switch_workspace(struct wm_server *s, int ws);
void wm_move_focused_to(struct wm_server *s, int ws);
void wm_toggle_tile(struct wm_server *s, struct wm_view *v);
int wm_view_count(struct wm_server *s, int ws);   // ws < 0: todas
struct wm_view *wm_view_at(struct wm_server *s, double lx, double ly,
                           struct wlr_surface **surface, double *sx, double *sy);
const char *wm_view_app_id(struct wm_view *v);
const char *wm_view_title(struct wm_view *v);

struct wlr_box wm_usable_area(struct wm_server *s);
void wm_bar_reconfigure(struct wm_server *s);
pid_t wm_bar_client_pid(struct wm_server *s);

/* wm_input.c */
void wm_input_init(struct wm_server *s);
void wm_run_action(struct wm_server *s, const char *action);
void wm_set_gamepad_grab(struct wm_server *s, bool grab);
void wm_ctl_init(struct wm_server *s, const char *socket);
void wm_ctl_finish(struct wm_server *s);

/* wm_ui.c */
void wm_ui_init(struct wm_server *s);
void wm_ui_finish(struct wm_server *s);
void wm_ui_show(struct wm_server *s, enum wm_ui_mode mode);
void wm_ui_redraw(struct wm_server *s);
void wm_ui_osd(struct wm_server *s, const char *msg);
void wm_ui_nav(struct wm_server *s, NavAction act);
void wm_ui_touch_down(struct wm_server *s, double x, double y);
void wm_ui_touch_motion(struct wm_server *s, double x, double y);
void wm_ui_touch_up(struct wm_server *s, double x, double y);
void wm_ui_views_changed(struct wm_server *s);
void wm_ui_home_button(struct wm_server *s);
void wm_ui_stick(struct wm_server *s);        // el stick derecho cambió (desplazar el Inicio)
void wm_toggle_bar(struct wm_server *s);
void wm_spawn(struct wm_server *s, const char *cmd);
void wm_ui_update_wallpaper(struct wm_server *s);
void wm_bar_sync(struct wm_server *s);        // lanzar/parar la barra según la configuración
void wm_write_state(struct wm_server *s);     // estado para la barra (escritorio, ventanas)

#endif // WM_H
