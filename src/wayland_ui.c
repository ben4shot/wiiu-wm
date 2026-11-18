#include "wayland_ui.h"
#include "draw_shapes.h"
#include "images.h"
#include "icons.h"
#include "xdg-shell-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <poll.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <libgen.h>

#define SELF_APP_ID "wiiu-shell"
#define EDITOR_APP_ID "wiiu-shell-editor"
#define POINTER_TOUCH_ID 1000

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* ------------------------------------------------------------ búferes SHM */

static int create_shm_file(off_t size) {
    int fd = memfd_create("wiiu_shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        char template[] = "/tmp/wiiu_shm-XXXXXX";
        fd = mkstemp(template);
        if (fd < 0) return -1;
        unlink(template);
    }
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void buffer_release(void *data, struct wl_buffer *wl_buffer) {
    (void)wl_buffer;
    Buffer *buf = (Buffer *)data;
    buf->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void destroy_buffer(Buffer *buf) {
    if (buf->cairo_surface) cairo_surface_destroy(buf->cairo_surface);
    if (buf->wl_buf) wl_buffer_destroy(buf->wl_buf);
    if (buf->shm_data) munmap(buf->shm_data, buf->size);
    memset(buf, 0, sizeof(*buf));
}

static bool create_buffer(WaylandUI *ui, Buffer *buf) {
    int stride = ui->width * 4;
    buf->size = (size_t)stride * ui->height;

    int fd = create_shm_file(buf->size);
    if (fd < 0) return false;

    buf->shm_data = mmap(NULL, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf->shm_data == MAP_FAILED) {
        buf->shm_data = NULL;
        close(fd);
        return false;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(ui->shm, fd, buf->size);
    buf->wl_buf = wl_shm_pool_create_buffer(pool, 0, ui->width, ui->height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_buffer_add_listener(buf->wl_buf, &buffer_listener, buf);
    wl_shm_pool_destroy(pool);
    close(fd);

    buf->cairo_surface = cairo_image_surface_create_for_data(
        (unsigned char *)buf->shm_data, CAIRO_FORMAT_ARGB32, ui->width, ui->height, stride);

    buf->busy = false;
    return true;
}

/* -------------------------------------------------------------- xdg-shell */

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    WaylandUI *ui = (WaylandUI *)data;
    xdg_surface_ack_configure(xdg_surface, serial);
    ui->configured = true;
    ui->needs_redraw = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void refresh_recents(WaylandUI *ui, bool focus_active);

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
                                   int32_t width, int32_t height, struct wl_array *states) {
    (void)xdg_toplevel;
    WaylandUI *ui = (WaylandUI *)data;

    bool active = false;
    uint32_t *st;
    wl_array_for_each(st, states) {
        if (*st == XDG_TOPLEVEL_STATE_ACTIVATED) active = true;
    }
    if (active != ui->shell_active) {
        ui->shell_active = active;
        printf("[Shell] %s\n", active ? "En primer plano" : "En segundo plano (app activa)");
        if (active && ui->current_state == STATE_RECENTS) refresh_recents(ui, false);
    }

    if (width > 0 && height > 0 && (width != ui->width || height != ui->height)) {
        ui->width = width;
        ui->height = height;
        for (int i = 0; i < 2; i++) {
            destroy_buffer(&ui->buffers[i]);
            create_buffer(ui, &ui->buffers[i]);
        }
    }
    ui->needs_redraw = true;
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
    (void)xdg_toplevel;
    WaylandUI *ui = (WaylandUI *)data;
    ui->running = false;
}

static void xdg_toplevel_configure_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h) {
    (void)data; (void)t; (void)w; (void)h;
}

static void xdg_toplevel_wm_capabilities(void *data, struct xdg_toplevel *t, struct wl_array *caps) {
    (void)data; (void)t; (void)caps;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
    .configure_bounds = xdg_toplevel_configure_bounds,
    .wm_capabilities = xdg_toplevel_wm_capabilities,
};

/* ----------------------------------------------- ventanas y estado del shell */

static Toplevel *toplevel_by_serial(WaylandUI *ui, uint32_t serial) {
    for (int i = 0; i < ui->toplevels.count; i++) {
        if (ui->toplevels.items[i]->serial == serial) return ui->toplevels.items[i];
    }
    return NULL;
}

static const AppEntry *app_for_toplevel(WaylandUI *ui, const Toplevel *t) {
    if (t->app_index >= 0 && t->app_index < ui->cfg.num_apps) return &ui->cfg.apps[t->app_index];
    for (int i = 0; i < ui->cfg.num_apps; i++) {
        if (ui->cfg.apps[i].app_id[0] && strcmp(ui->cfg.apps[i].app_id, t->app_id) == 0) return &ui->cfg.apps[i];
    }
    return NULL;
}

// Rellena las tarjetas de Recientes con las ventanas reales (o, sin el protocolo, con los procesos)
static void refresh_recents(WaylandUI *ui, bool focus_active) {
    RecentItem items[MAX_RECENT_ITEMS];
    int n = 0;
    memset(items, 0, sizeof(items));

    if (toplevels_available(&ui->toplevels)) {
        Toplevel *apps[MAX_TOPLEVELS];
        int count = toplevels_apps(&ui->toplevels, apps, MAX_TOPLEVELS);
        for (int i = 0; i < count && n < MAX_RECENT_ITEMS; i++) {
            Toplevel *t = apps[i];
            const AppEntry *app = app_for_toplevel(ui, t);
            RecentItem *it = &items[n];
            snprintf(it->name, sizeof(it->name), "%s",
                     app ? app->name : (t->app_id[0] ? t->app_id : t->title));
            snprintf(it->subtitle, sizeof(it->subtitle), "%s", t->title[0] ? t->title : t->app_id);
            snprintf(it->icon, sizeof(it->icon), "%s", app ? app->icon : "");
            snprintf(it->exec, sizeof(it->exec), "%s", app ? app->exec : t->app_id);
            it->active = (t->serial == ui->last_app_serial);
            ui->recent_serials[n] = t->serial;
            n++;
        }
    } else {
        for (int i = 0; i < ui->app_mgr.count && n < MAX_RECENT_ITEMS; i++) {
            const RunningApp *r = &ui->app_mgr.running[i];
            RecentItem *it = &items[n];
            snprintf(it->name, sizeof(it->name), "%s", r->name);
            snprintf(it->subtitle, sizeof(it->subtitle), "PID %d", r->pid);
            snprintf(it->icon, sizeof(it->icon), "%s", r->icon);
            snprintf(it->exec, sizeof(it->exec), "%s", r->exec);
            it->active = (i == ui->app_mgr.active_index);
            n++;
        }
    }
    recents_view_set_items(&ui->recents, items, n, focus_active);
    ui->needs_redraw = true;
}

static int app_window_count(WaylandUI *ui) {
    if (toplevels_available(&ui->toplevels)) {
        Toplevel *apps[MAX_TOPLEVELS];
        return toplevels_apps(&ui->toplevels, apps, MAX_TOPLEVELS);
    }
    return ui->app_mgr.count;
}

// Trae la ventana del shell al frente (si una app la tapa)
static void show_shell(WaylandUI *ui) {
    if (ui->shell_active) return;
    Toplevel *self = toplevels_self(&ui->toplevels);
    if (self) {
        toplevels_activate(self, ui->seat);
    } else {
        fprintf(stderr, "[Shell] El compositor no expone la ventana del shell: no puedo volver al frente.\n");
    }
}

static void open_recents(WaylandUI *ui) {
    show_shell(ui);
    ui->current_state = STATE_RECENTS;
    refresh_recents(ui, true);
    printf("[Shell] Recientes (%d ventanas)\n", ui->recents.count);
}

static void go_home_grid(WaylandUI *ui) {
    show_shell(ui);
    ui->current_state = STATE_HOME_GRID;
    ui->needs_redraw = true;
    printf("[Shell] Inicio\n");
}

void wayland_ui_handle_home_button(WaylandUI *ui) {
    printf("[Shell] HOME (shell %s, vista %s, %d ventanas)\n", ui->shell_active ? "delante" : "detrás",
           ui->current_state == STATE_RECENTS ? "Recientes" : "Inicio", app_window_count(ui));
    if (!ui->shell_active) {
        // Desde una app: Recientes (si no hay ventanas, directamente al Inicio)
        if (app_window_count(ui) > 0) open_recents(ui);
        else go_home_grid(ui);
    } else if (ui->current_state == STATE_RECENTS) {
        go_home_grid(ui);
    } else if (app_window_count(ui) > 0) {
        open_recents(ui);
    }
    ui->needs_redraw = true;
}

static void focus_recent(WaylandUI *ui, int card) {
    if (card < 0 || card >= ui->recents.count) return;
    if (toplevels_available(&ui->toplevels)) {
        Toplevel *t = toplevel_by_serial(ui, ui->recent_serials[card]);
        if (t) {
            toplevels_activate(t, ui->seat);
            ui->current_state = STATE_HOME_GRID; // al volver con HOME se mostrará Recientes
        }
    } else {
        app_manager_focus(&ui->app_mgr, card);
    }
    ui->needs_redraw = true;
}

static void close_recent(WaylandUI *ui, int card) {
    if (card < 0 || card >= ui->recents.count) return;
    if (toplevels_available(&ui->toplevels)) {
        toplevels_close(toplevel_by_serial(ui, ui->recent_serials[card]));
    } else {
        app_manager_close(&ui->app_mgr, card);
        refresh_recents(ui, false);
    }
    ui->needs_redraw = true;
}

// Toque/A sobre un tile: siempre abre una instancia nueva (se puede tener dos terminales).
// Volver a una ventana ya abierta se hace desde Recientes (HOME una vez).
static void launch_app(WaylandUI *ui, int idx) {
    if (idx < 0 || idx >= ui->cfg.num_apps) return;
    const AppEntry *app = &ui->cfg.apps[idx];
    if (app->single_instance && toplevels_available(&ui->toplevels)) {
        // Apps de una sola instancia (el editor): si ya está abierta, al frente
        Toplevel *t = toplevels_find_for_app(&ui->toplevels, idx, app->app_id);
        if (t) {
            toplevels_activate(t, ui->seat);
            return;
        }
    }
    if (app_manager_launch(&ui->app_mgr, app)) {
        toplevels_expect_window(&ui->toplevels, idx, now_ms());
    }
}

// El shell no debe verse detrás de una app en ventana: cuando una app pasa al frente, se minimiza.
// Vuelve con HOME, con los gestos de borde o cuando se cierra la última app.
static void update_shell_visibility(WaylandUI *ui) {
    if (!toplevels_available(&ui->toplevels)) return;
    Toplevel *self = toplevels_self(&ui->toplevels);
    if (!self) return;

    if (!ui->shell_active && !self->minimized && toplevels_active_app(&ui->toplevels)) {
        zwlr_foreign_toplevel_handle_v1_set_minimized(self->handle);
        printf("[Shell] Oculto detrás de la app\n");
    } else if (self->minimized && app_window_count(ui) == 0) {
        // Sin apps abiertas no puede quedar la pantalla vacía: Inicio
        ui->current_state = STATE_HOME_GRID;
            toplevels_activate(self, ui->seat);
        ui->needs_redraw = true;
        printf("[Shell] Ya no quedan apps: Inicio\n");
    }
}

static void run_action(WaylandUI *ui, const char *action) {
    if (!action || !action[0] || strcmp(action, "none") == 0) return;
    printf("[Shell] Acción '%s'\n", action);
    if (strcmp(action, "home") == 0) {
        wayland_ui_handle_home_button(ui);
    } else if (strcmp(action, "open_recents") == 0) {
        open_recents(ui);
    } else if (strcmp(action, "home_grid") == 0) {
        go_home_grid(ui);
    } else if (strcmp(action, "next_app") == 0 || strcmp(action, "prev_app") == 0) {
        int dir = (strcmp(action, "next_app") == 0) ? 1 : -1;
        if (toplevels_available(&ui->toplevels)) toplevels_cycle(&ui->toplevels, ui->seat, dir);
        else if (dir > 0) app_manager_next(&ui->app_mgr);
        else app_manager_prev(&ui->app_mgr);
    } else {
        fprintf(stderr, "[Shell] Acción desconocida '%s'\n", action);
    }
    ui->needs_redraw = true;
}

/* --------------------------------------------------------- gestos y toques */

static void handle_tap(WaylandUI *ui, double x, double y) {

    if (ui->current_state == STATE_HOME_GRID) {
        // Botón táctil [ HOME MENU ] arriba a la derecha
        if (x >= ui->width - 130.0 && y <= 44.0) {
            wayland_ui_handle_home_button(ui);
            return;
        }
        int app_idx = grid_view_hit_test(&ui->grid, x, y, ui->width, ui->height);
        if (app_idx >= 0) {
            ui->grid.focused_index = app_idx;
            printf("[Touch] Tile '%s'\n", ui->cfg.apps[app_idx].name);
            launch_app(ui, app_idx);
            ui->needs_redraw = true;
        }
    } else if (ui->current_state == STATE_RECENTS) {
        bool is_close = false;
        int card_idx = recents_view_hit_test(&ui->recents, x, y, ui->width, ui->height, &is_close, NULL);
        if (card_idx == -2) {
            go_home_grid(ui);
        } else if (card_idx >= 0) {
            ui->recents.focused_card = card_idx;
            if (is_close) close_recent(ui, card_idx);
            else focus_recent(ui, card_idx);
        }
        ui->needs_redraw = true;
    }
}

static void handle_gesture(WaylandUI *ui, GestureEvent ev, double x, double y) {
    switch (ev) {
        case GESTURE_TAP:
            handle_tap(ui, x, y);
            break;
        default:
            break;
    }
}

static void touch_down(void *data, struct wl_touch *touch, uint32_t serial,
                       uint32_t time, struct wl_surface *surface, int32_t id,
                       wl_fixed_t x, wl_fixed_t y) {
    (void)touch; (void)serial; (void)surface;
    WaylandUI *ui = (WaylandUI *)data;
    double px = wl_fixed_to_double(x), py = wl_fixed_to_double(y);
    if (ui->editor_mode) {
        if (ui->touch_tracker.active_fingers == 0) editor_touch_down(&ui->editor, px, py, now_ms());
        touch_tracker_down(&ui->touch_tracker, id, px, py, time);
        ui->needs_redraw = true;
        return;
    }
    touch_tracker_down(&ui->touch_tracker, id, px, py, time);

    if (ui->current_state == STATE_HOME_GRID) {
        ui->grid.pressed_index = grid_view_hit_test(&ui->grid, px, py, ui->width, ui->height);
        ui->needs_redraw = true;
    }
}

static void touch_motion(void *data, struct wl_touch *touch, uint32_t time,
                         int32_t id, wl_fixed_t x, wl_fixed_t y) {
    (void)touch;
    WaylandUI *ui = (WaylandUI *)data;
    touch_tracker_motion(&ui->touch_tracker, id, wl_fixed_to_double(x), wl_fixed_to_double(y), time);
    if (ui->editor_mode && ui->touch_tracker.points[0].active && ui->touch_tracker.points[0].id == id) {
        editor_touch_motion(&ui->editor, wl_fixed_to_double(x), wl_fixed_to_double(y), now_ms());
        ui->needs_redraw = true;
    }
}

static void touch_up(void *data, struct wl_touch *touch, uint32_t serial,
                     uint32_t time, int32_t id) {
    (void)touch; (void)serial;
    WaylandUI *ui = (WaylandUI *)data;
    double x = 0, y = 0;
    GestureEvent ev = touch_tracker_up(&ui->touch_tracker, id, time, &x, &y);
    if (ui->editor_mode) {
        if (ui->touch_tracker.active_fingers == 0) editor_touch_up(&ui->editor, x, y, now_ms());
        ui->needs_redraw = true;
        return;
    }
    if (ui->grid.pressed_index >= 0) {
        ui->grid.pressed_index = -1;
        ui->needs_redraw = true;
    }
    if (ui->touch_tracker.active_fingers == 0) handle_gesture(ui, ev, x, y);
}

static void touch_frame(void *data, struct wl_touch *touch) {
    (void)data; (void)touch;
}

static void touch_cancel(void *data, struct wl_touch *touch) {
    (void)touch;
    WaylandUI *ui = (WaylandUI *)data;
    touch_tracker_cancel(&ui->touch_tracker);
    ui->grid.pressed_index = -1;
    ui->needs_redraw = true;
}

static void touch_shape(void *data, struct wl_touch *touch, int32_t id, wl_fixed_t major, wl_fixed_t minor) {
    (void)data; (void)touch; (void)id; (void)major; (void)minor;
}

static void touch_orientation(void *data, struct wl_touch *touch, int32_t id, wl_fixed_t orientation) {
    (void)data; (void)touch; (void)id; (void)orientation;
}

static const struct wl_touch_listener touch_listener = {
    .down = touch_down,
    .up = touch_up,
    .motion = touch_motion,
    .frame = touch_frame,
    .cancel = touch_cancel,
    .shape = touch_shape,
    .orientation = touch_orientation,
};

// Ratón (o pantalla táctil que el compositor entregue como puntero): mismo motor de gestos
static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state) {
    (void)pointer; (void)serial;
    WaylandUI *ui = (WaylandUI *)data;
    if (button != BTN_LEFT) return;
    if (ui->editor_mode) {
        ui->pointer_down = state == WL_POINTER_BUTTON_STATE_PRESSED;
        if (ui->pointer_down) editor_touch_down(&ui->editor, ui->pointer_x, ui->pointer_y, now_ms());
        else editor_touch_up(&ui->editor, ui->pointer_x, ui->pointer_y, now_ms());
        ui->needs_redraw = true;
        return;
    }
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        ui->pointer_down = true;
        touch_tracker_down(&ui->touch_tracker, POINTER_TOUCH_ID, ui->pointer_x, ui->pointer_y, time);
        if (ui->current_state == STATE_HOME_GRID) {
            ui->grid.pressed_index = grid_view_hit_test(&ui->grid, ui->pointer_x, ui->pointer_y, ui->width, ui->height);
            ui->needs_redraw = true;
        }
    } else if (ui->pointer_down) {
        ui->pointer_down = false;
        double x = 0, y = 0;
        GestureEvent ev = touch_tracker_up(&ui->touch_tracker, POINTER_TOUCH_ID, time, &x, &y);
        ui->grid.pressed_index = -1;
        ui->needs_redraw = true;
        handle_gesture(ui, ev, x, y);
    }
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                           wl_fixed_t surface_x, wl_fixed_t surface_y) {
    (void)pointer;
    WaylandUI *ui = (WaylandUI *)data;
    ui->pointer_x = wl_fixed_to_double(surface_x);
    ui->pointer_y = wl_fixed_to_double(surface_y);
    if (ui->pointer_down && ui->editor_mode) {
        editor_touch_motion(&ui->editor, ui->pointer_x, ui->pointer_y, now_ms());
        ui->needs_redraw = true;
    } else if (ui->pointer_down) {
        touch_tracker_motion(&ui->touch_tracker, POINTER_TOUCH_ID, ui->pointer_x, ui->pointer_y, time);
    }
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    (void)pointer; (void)serial; (void)surface;
    WaylandUI *ui = (WaylandUI *)data;
    ui->pointer_x = wl_fixed_to_double(surface_x);
    ui->pointer_y = wl_fixed_to_double(surface_y);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface) {
    (void)pointer; (void)serial; (void)surface;
    WaylandUI *ui = (WaylandUI *)data;
    if (ui->pointer_down) {
        ui->pointer_down = false;
        touch_tracker_cancel(&ui->touch_tracker);
        ui->grid.pressed_index = -1;
        ui->needs_redraw = true;
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value) {
    (void)pointer; (void)time;
    WaylandUI *ui = (WaylandUI *)data;
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL || ui->current_state != STATE_HOME_GRID) return;
    if (wl_fixed_to_double(value) > 0) grid_view_next_page(&ui->grid);
    else grid_view_prev_page(&ui->grid);
    ui->needs_redraw = true;
}

static void pointer_frame(void *data, struct wl_pointer *pointer) {
    (void)data; (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source) {
    (void)data; (void)pointer; (void)axis_source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {
    (void)data; (void)pointer; (void)time; (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete) {
    (void)data; (void)pointer; (void)axis; (void)discrete;
}

static void pointer_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t value120) {
    (void)data; (void)pointer; (void)axis; (void)value120;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
};

/* ------------------------------------------------ navegación mando/teclado */

static void handle_nav(WaylandUI *ui, NavAction act) {
    if (ui->editor_mode) {
        // HOME lo atiende el shell; el resto, solo si el editor está delante
        if (ui->shell_active && act != NAV_ACTION_HOME) {
            editor_nav(&ui->editor, act, now_ms());
            ui->needs_redraw = true;
        }
        return;
    }
    // Con una app en primer plano el mando es suyo: solo HOME vuelve al shell
    if (!ui->shell_active && act != NAV_ACTION_HOME) return;

    switch (act) {
        case NAV_ACTION_HOME:
            wayland_ui_handle_home_button(ui);
            break;
        case NAV_UP:
            if (ui->current_state == STATE_HOME_GRID) grid_view_nav(&ui->grid, 0, -1);
            break;
        case NAV_DOWN:
            if (ui->current_state == STATE_HOME_GRID) grid_view_nav(&ui->grid, 0, 1);
            break;
        case NAV_LEFT:
            if (ui->current_state == STATE_HOME_GRID) grid_view_nav(&ui->grid, -1, 0);
            else if (ui->current_state == STATE_RECENTS) recents_view_nav(&ui->recents, -1);
            break;
        case NAV_RIGHT:
            if (ui->current_state == STATE_HOME_GRID) grid_view_nav(&ui->grid, 1, 0);
            else if (ui->current_state == STATE_RECENTS) recents_view_nav(&ui->recents, 1);
            break;
        case NAV_ACTION_A:
            if (ui->current_state == STATE_HOME_GRID) launch_app(ui, ui->grid.focused_index);
            else if (ui->current_state == STATE_RECENTS) {
                if (ui->recents.count == 0) go_home_grid(ui);
                else focus_recent(ui, ui->recents.focused_card);
            }
            break;
        case NAV_ACTION_B:
            if (ui->current_state == STATE_RECENTS) go_home_grid(ui);
            break;
        case NAV_ACTION_X:
            if (ui->current_state == STATE_RECENTS) close_recent(ui, ui->recents.focused_card);
            break;
        case NAV_ACTION_PLUS:
            break;
        case NAV_PAGE_PREV:
            if (ui->current_state == STATE_HOME_GRID) grid_view_prev_page(&ui->grid);
            break;
        case NAV_PAGE_NEXT:
            if (ui->current_state == STATE_HOME_GRID) grid_view_next_page(&ui->grid);
            break;
        default:
            break;
    }
    ui->needs_redraw = true;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state) {
    (void)keyboard; (void)serial; (void)time;
    WaylandUI *ui = (WaylandUI *)data;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;

    switch (key) {
        case KEY_HOME:
        case KEY_HOMEPAGE: handle_nav(ui, NAV_ACTION_HOME); break;
        case KEY_UP:       handle_nav(ui, NAV_UP); break;
        case KEY_DOWN:     handle_nav(ui, NAV_DOWN); break;
        case KEY_LEFT:     handle_nav(ui, NAV_LEFT); break;
        case KEY_RIGHT:    handle_nav(ui, NAV_RIGHT); break;
        case KEY_ENTER:
        case KEY_SPACE:    handle_nav(ui, NAV_ACTION_A); break;
        case KEY_ESC:
        case KEY_BACKSPACE: handle_nav(ui, NAV_ACTION_B); break;
        case KEY_DELETE:   handle_nav(ui, NAV_ACTION_X); break;
        case KEY_PAGEUP:   handle_nav(ui, NAV_PAGE_PREV); break;
        case KEY_PAGEDOWN: handle_nav(ui, NAV_PAGE_NEXT); break;
        case KEY_TAB:      run_action(ui, "next_app"); break;
    }
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size) {
    (void)data; (void)keyboard; (void)format; (void)size;
    close(fd);
}
static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys) {
    (void)data; (void)keyboard; (void)serial; (void)surface; (void)keys;
}
static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface) {
    (void)data; (void)keyboard; (void)serial; (void)surface;
}
static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                               uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    (void)data; (void)keyboard; (void)serial; (void)mods_depressed; (void)mods_latched; (void)mods_locked; (void)group;
}
static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {
    (void)data; (void)keyboard; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* ------------------------------------------------------- seat y registro */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    WaylandUI *ui = (WaylandUI *)data;
    if ((capabilities & WL_SEAT_CAPABILITY_TOUCH) && !ui->touch) {
        ui->touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(ui->touch, &touch_listener, ui);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !ui->pointer) {
        ui->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(ui->pointer, &pointer_listener, ui);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !ui->keyboard) {
        ui->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(ui->keyboard, &keyboard_listener, ui);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    WaylandUI *ui = (WaylandUI *)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        ui->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        ui->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        ui->xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, version < 4 ? version : 4);
        xdg_wm_base_add_listener(ui->xdg_wm_base, &xdg_wm_base_listener, ui);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && !ui->seat) {
        ui->seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 7 ? version : 7);
        wl_seat_add_listener(ui->seat, &seat_listener, ui);
    } else if (!ui->editor_mode && strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        toplevels_bind(&ui->toplevels, registry, name, version);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ------------------------------------------------------------ renderizado */

void wayland_ui_render_frame(WaylandUI *ui) {
    Buffer *buf = &ui->buffers[ui->current_buffer];
    if (buf->busy) {
        buf = &ui->buffers[1 - ui->current_buffer];
        if (buf->busy) return; // los dos en uso: se reintenta en la siguiente vuelta
        ui->current_buffer = 1 - ui->current_buffer;
    }
    if (!buf->cairo_surface) return;

    cairo_t *cr = cairo_create(buf->cairo_surface);
    if (ui->editor_mode) {
        editor_render(&ui->editor, cr, ui->width, ui->height);
        goto done;
    }
    if (ui->current_state == STATE_RECENTS) {
        if (ui->page_cache.wallpaper) {
            cairo_set_source_surface(cr, ui->page_cache.wallpaper, 0, 0);
            cairo_paint(cr);
        } else {
            image_draw_wallpaper(cr, ui->cfg.wallpaper, ui->width, ui->height,
                                 &ui->cfg.bg_color, &ui->cfg.bg_gradient, &ui->cfg.accent);
        }
    }

    if (ui->current_state == STATE_RECENTS) {
        recents_view_render(&ui->recents, cr, ui->width, ui->height);
    } else {
        grid_view_render(&ui->grid, cr, ui->width, ui->height);
    }

    // Aviso temporal (app no instalada, etc.)
    if (ui->toast_until_ms && now_ms() < ui->toast_until_ms) {
        const char *msg = ui->app_mgr.last_error;
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 13.0);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, msg, &ext);
        double bw = ext.width + 40.0, bh = 36.0;
        if (bw > ui->width - 20.0) bw = ui->width - 20.0;
        double bx = (ui->width - bw) / 2.0, by = ui->height - bh - 46.0;
        shape_path_rounded_rect(cr, bx, by, bw, bh, 18.0);
        cairo_set_source_rgba(cr, 0.12, 0.05, 0.10, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.95, 0.35, 0.55, 0.9);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, 1.0, 0.92, 0.95, 1.0);
        cairo_move_to(cr, bx + 20.0, by + bh / 2.0 + 5.0);
        cairo_show_text(cr, msg);
    }
done:
    cairo_destroy(cr);
    cairo_surface_flush(buf->cairo_surface);

    wl_surface_attach(ui->surface, buf->wl_buf, 0, 0);
    wl_surface_damage_buffer(ui->surface, 0, 0, ui->width, ui->height);
    buf->busy = true;
    wl_surface_commit(ui->surface);

    ui->current_buffer = 1 - ui->current_buffer;
    ui->needs_redraw = false;
}

/* ------------------------------------------------------- arranque y bucle */

// Vigila el directorio de la configuración del usuario (el editor escribe con rename)
static void watch_config(WaylandUI *ui) {
    char dir[MAX_STR];
    snprintf(dir, sizeof(dir), "%s", ui->config_user);
    char *d = dirname(dir);
    // Crear el directorio para poder vigilarlo aunque aún no exista la configuración
    char tmp[MAX_STR];
    snprintf(tmp, sizeof(tmp), "%s", d);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);

    ui->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ui->inotify_fd >= 0 && inotify_add_watch(ui->inotify_fd, d, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE) < 0) {
        close(ui->inotify_fd);
        ui->inotify_fd = -1;
    }
}

static void reload_config(WaylandUI *ui) {
    char path[MAX_STR];
    config_resolve_path(ui->config_fallback, path, sizeof(path));
    DesktopConfig *fresh = malloc(sizeof(DesktopConfig));
    if (!fresh) return;
    if (!config_load(fresh, path)) { free(fresh); return; }
    // Lo que no se edita en caliente se conserva (tamaño de la pantalla)
    fresh->screen_width = ui->cfg.screen_width;
    fresh->screen_height = ui->cfg.screen_height;
    ui->cfg = *fresh;
    free(fresh);
    icons_clear_cache();
    images_clear_cache();
    page_cache_prepare(&ui->page_cache, &ui->cfg, path);
    grid_view_reload(&ui->grid);
    ui->needs_redraw = true;
    printf("[Shell] Configuración recargada (%s)\n", path);
}

static void check_config_changes(WaylandUI *ui) {
    if (ui->inotify_fd < 0) return;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool changed = false;
    ssize_t n;
    while ((n = read(ui->inotify_fd, buf, sizeof(buf))) > 0) {
        for (char *p = buf; p < buf + n;) {
            struct inotify_event *ev = (struct inotify_event *)p;
            if (ev->len && strcmp(ev->name, "config.json") == 0) changed = true;
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    if (changed) reload_config(ui);
}

bool wayland_ui_init(WaylandUI *ui, const char *config_file, bool editor_mode) {
    memset(ui, 0, sizeof(*ui));
    ui->inotify_fd = -1;
    ui->editor_mode = editor_mode;
    snprintf(ui->config_fallback, sizeof(ui->config_fallback), "%s", config_file);
    config_user_path(ui->config_user, sizeof(ui->config_user));
    char path[MAX_STR];
    config_resolve_path(config_file, path, sizeof(path));
    config_load(&ui->cfg, path);

    ui->width = ui->cfg.screen_width;
    ui->height = ui->cfg.screen_height;
    ui->current_state = STATE_HOME_GRID;
    ui->running = true;
    ui->needs_redraw = true;
    ui->shell_active = true;

    app_manager_init(&ui->app_mgr);
    grid_view_init(&ui->grid, &ui->cfg);
    recents_view_init(&ui->recents, &ui->cfg);
    touch_tracker_init(&ui->touch_tracker, &ui->cfg);
    toplevels_init(&ui->toplevels, SELF_APP_ID, ui->cfg.maximize_apps);
    gamepad_init(&ui->gamepad, &ui->cfg);
    if (editor_mode) {
        ui->gamepad.stick_nav = false;   // los sticks mueven y redimensionan
        editor_init(&ui->editor, &ui->cfg, ui->config_user, ui->width, ui->height);
    } else {
        watch_config(ui);
        page_cache_init(&ui->page_cache);
        page_cache_prepare(&ui->page_cache, &ui->cfg, path);
        ui->grid.cache = &ui->page_cache;
    }

    ui->display = wl_display_connect(NULL);
    if (!ui->display) {
        fprintf(stderr, "[Wayland] Error: No se pudo conectar al servidor Wayland ($WAYLAND_DISPLAY).\n");
        return false;
    }

    ui->registry = wl_display_get_registry(ui->display);
    wl_registry_add_listener(ui->registry, &registry_listener, ui);
    wl_display_roundtrip(ui->display);

    if (!ui->compositor || !ui->shm || !ui->xdg_wm_base) {
        fprintf(stderr, "[Wayland] Error: Faltan interfaces Wayland esenciales (compositor/shm/xdg_wm_base).\n");
        return false;
    }
    if (!ui->editor_mode && !toplevels_available(&ui->toplevels)) {
        fprintf(stderr, "[Wayland] Aviso: el compositor no ofrece wlr-foreign-toplevel; "
                        "no se podrá volver a una app abierta ni cambiar entre ellas.\n");
    }

    for (int i = 0; i < 2; i++) {
        if (!create_buffer(ui, &ui->buffers[i])) {
            fprintf(stderr, "[Wayland] Error al crear búfer SHM %d.\n", i);
            return false;
        }
    }

    ui->surface = wl_compositor_create_surface(ui->compositor);
    ui->xdg_surface = xdg_wm_base_get_xdg_surface(ui->xdg_wm_base, ui->surface);
    xdg_surface_add_listener(ui->xdg_surface, &xdg_surface_listener, ui);

    ui->xdg_toplevel = xdg_surface_get_toplevel(ui->xdg_surface);
    xdg_toplevel_add_listener(ui->xdg_toplevel, &xdg_toplevel_listener, ui);
    xdg_toplevel_set_title(ui->xdg_toplevel, editor_mode ? "Editor del menú" : "Wii U GamePad Shell");
    xdg_toplevel_set_app_id(ui->xdg_toplevel, editor_mode ? EDITOR_APP_ID : SELF_APP_ID);

    if (ui->cfg.fullscreen) {
        xdg_toplevel_set_fullscreen(ui->xdg_toplevel, NULL);
    } else {
        xdg_toplevel_set_maximized(ui->xdg_toplevel);
    }

    wl_surface_commit(ui->surface);
    wl_display_roundtrip(ui->display);

    printf("[Wayland] Superficie Wayland nativa inicializada con éxito (%dx%d).\n", ui->width, ui->height);
    return true;
}

void wayland_ui_cleanup(WaylandUI *ui) {
    if (ui->editor_mode) {
        editor_save(&ui->editor);
        editor_free(&ui->editor);
    }
    if (ui->inotify_fd >= 0) close(ui->inotify_fd);
    page_cache_free(&ui->page_cache);
    gamepad_close(&ui->gamepad);
    toplevels_destroy(&ui->toplevels);

    for (int i = 0; i < 2; i++) destroy_buffer(&ui->buffers[i]);

    if (ui->touch) wl_touch_destroy(ui->touch);
    if (ui->pointer) wl_pointer_destroy(ui->pointer);
    if (ui->keyboard) wl_keyboard_destroy(ui->keyboard);
    if (ui->xdg_toplevel) xdg_toplevel_destroy(ui->xdg_toplevel);
    if (ui->xdg_surface) xdg_surface_destroy(ui->xdg_surface);
    if (ui->surface) wl_surface_destroy(ui->surface);
    if (ui->xdg_wm_base) xdg_wm_base_destroy(ui->xdg_wm_base);
    if (ui->seat) wl_seat_destroy(ui->seat);
    if (ui->shm) wl_shm_destroy(ui->shm);
    if (ui->compositor) wl_compositor_destroy(ui->compositor);
    if (ui->registry) wl_registry_destroy(ui->registry);
    if (ui->display) wl_display_disconnect(ui->display);
}

// Recuerda qué app estaba delante para enfocarla al abrir Recientes
static void track_active_app(WaylandUI *ui) {
    Toplevel *t = toplevels_active_app(&ui->toplevels);
    if (t) ui->last_app_serial = t->serial;
}

void wayland_ui_run(WaylandUI *ui) {
    int wl_fd = wl_display_get_fd(ui->display);

    while (ui->running) {
        uint32_t now = now_ms();
        app_manager_refresh(&ui->app_mgr);
        if (ui->app_mgr.error_seq != ui->toast_seq) {
            ui->toast_seq = ui->app_mgr.error_seq;
            ui->toast_until_ms = now + 4000;
            ui->needs_redraw = true;
            printf("[Shell] Aviso: %s\n", ui->app_mgr.last_error);
        } else if (ui->toast_until_ms && now >= ui->toast_until_ms) {
            ui->toast_until_ms = 0;
            ui->needs_redraw = true;
        }

        // El mando y la pantalla táctil de drc_stream aparecen cuando el GamePad conecta
        gamepad_maintain(&ui->gamepad, now);

        bool animating = false;
        if (ui->current_state == STATE_HOME_GRID) {
            if (grid_view_update(&ui->grid, ui->width)) animating = true;
        } else if (ui->current_state == STATE_RECENTS) {
            if (recents_view_update(&ui->recents, ui->width)) animating = true;
        }
        if (animating) ui->needs_redraw = true;

        NavAction act;
        while ((act = gamepad_poll(&ui->gamepad)) != NAV_NONE) handle_nav(ui, act);

        if (ui->editor_mode) {
            const GamepadInput *gp = ui->shell_active ? &ui->gamepad : NULL;
            if (editor_tick(&ui->editor, gp, now)) ui->needs_redraw = true;
            if (editor_busy(&ui->editor, gp, now)) animating = true;
            if (ui->editor.want_quit) ui->running = false;
        } else {
            check_config_changes(ui);
        }


        if (ui->toplevels.changed) {
            ui->toplevels.changed = false;
            track_active_app(ui);
            if (ui->current_state == STATE_RECENTS) refresh_recents(ui, false);
            update_shell_visibility(ui);
        }

        while (wl_display_prepare_read(ui->display) != 0) {
            wl_display_dispatch_pending(ui->display);
        }

        if (ui->needs_redraw && ui->configured) {
            wayland_ui_render_frame(ui);
        }

        if (wl_display_flush(ui->display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(ui->display);
            fprintf(stderr, "[Wayland] Conexión con el compositor perdida.\n");
            break;
        }

        struct pollfd pfds[4];
        int nfds = 0;
        pfds[nfds++] = (struct pollfd){ .fd = wl_fd, .events = POLLIN };
        if (ui->gamepad.is_open) pfds[nfds++] = (struct pollfd){ .fd = ui->gamepad.fd, .events = POLLIN };
        if (ui->inotify_fd >= 0) pfds[nfds++] = (struct pollfd){ .fd = ui->inotify_fd, .events = POLLIN };

        // Animando: ~60 Hz. En reposo: despertar por eventos o cada 250 ms (procesos, mando nuevo)
        int timeout_ms = (animating || ui->needs_redraw) ? 16 : 250;
        int ret = poll(pfds, nfds, timeout_ms);

        if (ret > 0 && (pfds[0].revents & POLLIN)) {
            if (wl_display_read_events(ui->display) < 0) {
                fprintf(stderr, "[Wayland] Error leyendo eventos del compositor.\n");
                break;
            }
            wl_display_dispatch_pending(ui->display);
        } else {
            wl_display_cancel_read(ui->display);
        }
    }
}
