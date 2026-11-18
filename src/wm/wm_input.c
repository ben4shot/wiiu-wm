#include "wm.h"
#include "wm_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/util/log.h>

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* -------------------------------------------------------------- acciones */

static bool run_bind(struct wm_server *s, const char *key);
static void run_bind_public(struct wm_server *s, const char *key) {
    if (!run_bind(s, key)) wlr_log(WLR_ERROR, "[wm] Sin atajo para '%s'", key);
}

void wm_run_action(struct wm_server *s, const char *action) {
    if (!action || !action[0] || strcmp(action, "none") == 0) return;
    wlr_log(WLR_INFO, "[wm] Acción '%s'", action);
    const char *arg = strchr(action, ' ');
    while (arg && *arg == ' ') arg++;
    size_t n = strcspn(action, " ");
#define IS(name) (n == strlen(name) && strncmp(action, name, n) == 0)
    if (IS("home")) wm_ui_home_button(s);
    else if (IS("recents")) wm_ui_show(s, UI_RECENTS);
    else if (IS("capture_dump") && arg) wm_capture_dump(s, arg);
    else if (IS("capture_redraw")) wm_capture_redraw(s);
    else if (IS("home_grid")) wm_ui_show(s, UI_HOME);
    else if (IS("focus_next")) { wm_ui_show(s, UI_HIDDEN); wm_focus_cycle(s, 1); }
    else if (IS("focus_prev")) { wm_ui_show(s, UI_HIDDEN); wm_focus_cycle(s, -1); }
    else if (IS("close")) wm_close_focused(s);
    else if (IS("workspace_next")) wm_switch_workspace(s, s->workspace + 1);
    else if (IS("workspace_prev")) wm_switch_workspace(s, s->workspace - 1);
    else if (IS("workspace") && arg) wm_switch_workspace(s, atoi(arg) - 1);
    else if (IS("move_next")) wm_move_focused_to(s, s->workspace + 1);
    else if (IS("move_prev")) wm_move_focused_to(s, s->workspace - 1);
    else if (IS("move") && arg) wm_move_focused_to(s, atoi(arg) - 1);
    else if (IS("toggle_tile")) wm_toggle_tile(s, s->focused);
    else if (IS("toggle_bar")) wm_toggle_bar(s);
    else if (IS("exec") && arg) wm_spawn(s, arg);
    else if (IS("quit")) wl_display_terminate(s->display);
    else if (IS("redraw")) { wm_ui_update_wallpaper(s); wm_ui_redraw(s); }   // repinta toda la pantalla
    else if (IS("tap") && arg) {   // pruebas: toque en la interfaz en (x, y)
        double x = 0, y = 0;
        if (sscanf(arg, "%lf %lf", &x, &y) == 2 && s->ui != UI_HIDDEN) {
            wm_ui_touch_down(s, x, y);
            wm_ui_touch_up(s, x, y);
        }
    }
    else if (IS("nav") && arg) {   // pruebas: navegación de la interfaz (up/down/left/right/a/b/x/y/l/r)
        static const struct { const char *n; NavAction a; } map[] = {
            { "up", NAV_UP }, { "down", NAV_DOWN }, { "left", NAV_LEFT }, { "right", NAV_RIGHT },
            { "a", NAV_ACTION_A }, { "b", NAV_ACTION_B }, { "x", NAV_ACTION_X }, { "y", NAV_ACTION_Y },
            { "l", NAV_PAGE_PREV }, { "r", NAV_PAGE_NEXT } };
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (strcasecmp(arg, map[i].n) == 0) wm_ui_nav(s, map[i].a);
    }
    else if (IS("bind") && arg) run_bind_public(s, arg);   // simula un atajo: «bind HOME+X»
    else wlr_log(WLR_ERROR, "[wm] Acción desconocida '%s'", action);
#undef IS
}

static bool run_bind(struct wm_server *s, const char *key) {
    const char *a = config_bind_action(&s->cfg, key);
    if (!a) return false;
    wm_run_action(s, a);
    return true;
}

/* ------------------------------------------------------------- teclado */

static void keyboard_modifiers(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_keyboard *k = wl_container_of(l, k, modifiers);
    wlr_seat_set_keyboard(k->server->seat, k->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(k->server->seat, &k->wlr_keyboard->modifiers);
}

static NavAction key_to_nav(xkb_keysym_t sym) {
    switch (sym) {
        case XKB_KEY_Up: return NAV_UP;
        case XKB_KEY_Down: return NAV_DOWN;
        case XKB_KEY_Left: return NAV_LEFT;
        case XKB_KEY_Right: return NAV_RIGHT;
        case XKB_KEY_Return: case XKB_KEY_KP_Enter: case XKB_KEY_space: return NAV_ACTION_A;
        case XKB_KEY_Escape: case XKB_KEY_BackSpace: return NAV_ACTION_B;
        case XKB_KEY_Delete: return NAV_ACTION_X;
        case XKB_KEY_t: return NAV_ACTION_Y;
        case XKB_KEY_Prior: return NAV_PAGE_PREV;
        case XKB_KEY_Next: return NAV_PAGE_NEXT;
        default: return NAV_NONE;
    }
}

static void keyboard_key(struct wl_listener *l, void *data) {
    struct wm_keyboard *k = wl_container_of(l, k, key);
    struct wm_server *s = k->server;
    struct wlr_keyboard_key_event *ev = data;
    uint32_t keycode = ev->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(k->wlr_keyboard->xkb_state, keycode, &syms);
    bool pressed = ev->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    bool handled = false;

    for (int i = 0; i < nsyms; i++) {
        xkb_keysym_t sym = syms[i];
        // Super hace de HOME: solo = HOME; con otra tecla = combinación «HOME+tecla»
        if (sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R) {
            if (pressed) { s->super_held = true; s->super_combo = false; }
            else {
                if (s->super_held && !s->super_combo) run_bind(s, "HOME");
                s->super_held = false;
            }
            handled = true;
            continue;
        }
        if (pressed && s->super_held) {
            char name[64], key[96];
            xkb_keysym_get_name(sym, name, sizeof(name));
            snprintf(key, sizeof(key), "HOME+%s", name);
            if (run_bind(s, key)) { s->super_combo = true; handled = true; continue; }
        }
        if (pressed && s->ui != UI_HIDDEN) {
            NavAction a = key_to_nav(sym);
            if (a != NAV_NONE) { wm_ui_nav(s, a); handled = true; }
        }
    }
    if (!handled) {
        wlr_seat_set_keyboard(s->seat, k->wlr_keyboard);
        wlr_seat_keyboard_notify_key(s->seat, ev->time_msec, ev->keycode, ev->state);
    }
}

static void keyboard_destroy(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_keyboard *k = wl_container_of(l, k, destroy);
    wl_list_remove(&k->modifiers.link);
    wl_list_remove(&k->key.link);
    wl_list_remove(&k->destroy.link);
    wl_list_remove(&k->link);
    free(k);
}

static void update_caps(struct wm_server *s) {
    uint32_t caps = 0;
    if (s->has_pointer) caps |= WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&s->keyboards)) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    if (!wl_list_empty(&s->touches)) caps |= WL_SEAT_CAPABILITY_TOUCH;
    wlr_seat_set_capabilities(s->seat, caps);
}

static void add_keyboard(struct wm_server *s, struct wlr_keyboard *wk) {
    struct wm_keyboard *k = calloc(1, sizeof(*k));
    k->server = s;
    k->wlr_keyboard = wk;
    // Los teclados virtuales (wvkbd) traen su propio mapa
    if (!wk->keymap) {
        struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
        wlr_keyboard_set_keymap(wk, km);
        xkb_keymap_unref(km);
        xkb_context_unref(ctx);
    }
    wlr_keyboard_set_repeat_info(wk, 25, 600);
    k->modifiers.notify = keyboard_modifiers;
    wl_signal_add(&wk->events.modifiers, &k->modifiers);
    k->key.notify = keyboard_key;
    wl_signal_add(&wk->events.key, &k->key);
    k->destroy.notify = keyboard_destroy;
    wl_signal_add(&wk->base.events.destroy, &k->destroy);
    wlr_seat_set_keyboard(s->seat, wk);
    wl_list_insert(&s->keyboards, &k->link);
}

static void new_virtual_keyboard(struct wl_listener *l, void *data) {
    struct wm_server *s = wl_container_of(l, s, new_virtual_keyboard);
    struct wlr_virtual_keyboard_v1 *vk = data;
    add_keyboard(s, &vk->keyboard);
    update_caps(s);
}

/* -------------------------------------------------------------- táctil */

static struct wm_touch_point *touch_slot(struct wm_server *s, int32_t id, bool create) {
    for (int i = 0; i < 10; i++) if (s->touch[i].active && s->touch[i].id == id) return &s->touch[i];
    if (!create) return NULL;
    for (int i = 0; i < 10; i++) {
        if (!s->touch[i].active) {
            memset(&s->touch[i], 0, sizeof(s->touch[i]));
            s->touch[i].id = id;
            s->touch[i].active = true;
            return &s->touch[i];
        }
    }
    return NULL;
}

static void touch_down(struct wl_listener *l, void *data) {
    struct wm_touch *t = wl_container_of(l, t, down);
    struct wm_server *s = t->server;
    struct wlr_touch_down_event *ev = data;
    double lx = ev->x * s->width, ly = ev->y * s->height;
    struct wm_touch_point *p = touch_slot(s, ev->touch_id, true);
    if (!p) return;
    p->down_x = p->x = lx;
    p->down_y = p->y = ly;

    // Con el Inicio o Recientes delante, el toque es para la interfaz (salvo el teclado en pantalla)
    double sx, sy;
    struct wlr_surface *surface = NULL;
    struct wm_view *v = wm_view_at(s, lx, ly, &surface, &sx, &sy);
    bool on_layer = surface && !v && wlr_layer_surface_v1_try_from_wlr_surface(surface);
    if (s->ui != UI_HIDDEN && !on_layer) {
        p->to_ui = true;
        wm_ui_touch_down(s, lx, ly);
        return;
    }
    if (!surface) return;
    if (v && v != s->focused) wm_focus_view(s, v);
    p->ox = lx - sx;
    p->oy = ly - sy;
    wlr_seat_touch_notify_down(s->seat, surface, ev->time_msec, ev->touch_id, sx, sy);
}

static void touch_motion(struct wl_listener *l, void *data) {
    struct wm_touch *t = wl_container_of(l, t, motion);
    struct wm_server *s = t->server;
    struct wlr_touch_motion_event *ev = data;
    struct wm_touch_point *p = touch_slot(s, ev->touch_id, false);
    if (!p) return;
    p->x = ev->x * s->width;
    p->y = ev->y * s->height;
    if (p->to_ui) { wm_ui_touch_motion(s, p->x, p->y); return; }
    wlr_seat_touch_notify_motion(s->seat, ev->time_msec, ev->touch_id, p->x - p->ox, p->y - p->oy);
}

static void touch_up(struct wl_listener *l, void *data) {
    struct wm_touch *t = wl_container_of(l, t, up);
    struct wm_server *s = t->server;
    struct wlr_touch_up_event *ev = data;
    struct wm_touch_point *p = touch_slot(s, ev->touch_id, false);
    if (!p) return;
    p->active = false;
    if (p->to_ui) { wm_ui_touch_up(s, p->x, p->y); return; }
    wlr_seat_touch_notify_up(s->seat, ev->time_msec, ev->touch_id);
}

static void touch_cancel(struct wl_listener *l, void *data) {
    struct wm_touch *t = wl_container_of(l, t, cancel);
    struct wm_server *s = t->server;
    struct wlr_touch_cancel_event *ev = data;
    struct wm_touch_point *p = touch_slot(s, ev->touch_id, false);
    if (p) p->active = false;
    struct wlr_touch_point *tp = wlr_seat_touch_get_point(s->seat, ev->touch_id);
    if (tp && tp->client) wlr_seat_touch_notify_cancel(s->seat, tp->client);
}

static void touch_frame(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_touch *t = wl_container_of(l, t, frame);
    wlr_seat_touch_notify_frame(t->server->seat);
}

static void touch_destroy(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_touch *t = wl_container_of(l, t, destroy);
    struct wm_server *s = t->server;
    wl_list_remove(&t->down.link);
    wl_list_remove(&t->up.link);
    wl_list_remove(&t->motion.link);
    wl_list_remove(&t->cancel.link);
    wl_list_remove(&t->frame.link);
    wl_list_remove(&t->destroy.link);
    wl_list_remove(&t->link);
    free(t);
    update_caps(s);
}

static void add_touch(struct wm_server *s, struct wlr_touch *wt) {
    struct wm_touch *t = calloc(1, sizeof(*t));
    t->server = s;
    t->wlr_touch = wt;
    t->down.notify = touch_down;
    wl_signal_add(&wt->events.down, &t->down);
    t->up.notify = touch_up;
    wl_signal_add(&wt->events.up, &t->up);
    t->motion.notify = touch_motion;
    wl_signal_add(&wt->events.motion, &t->motion);
    t->cancel.notify = touch_cancel;
    wl_signal_add(&wt->events.cancel, &t->cancel);
    t->frame.notify = touch_frame;
    wl_signal_add(&wt->events.frame, &t->frame);
    t->destroy.notify = touch_destroy;
    wl_signal_add(&wt->base.events.destroy, &t->destroy);
    wl_list_insert(&s->touches, &t->link);
    wlr_log(WLR_INFO, "[wm] Pantalla táctil: %s", wt->base.name ? wt->base.name : "?");
}

/* ------------------------------------------------- ratón (para pruebas) */

static void pointer_update(struct wm_server *s, uint32_t time) {
    if (s->ui != UI_HIDDEN) { wlr_seat_pointer_clear_focus(s->seat); return; }
    double sx, sy;
    struct wlr_surface *surface = NULL;
    wm_view_at(s, s->cursor->x, s->cursor->y, &surface, &sx, &sy);
    if (surface) {
        wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(s->seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(s->seat);
    }
}

static void cursor_motion(struct wl_listener *l, void *data) {
    struct wm_server *s = wl_container_of(l, s, cursor_motion);
    struct wlr_pointer_motion_event *ev = data;
    wlr_cursor_move(s->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
    if (s->ui != UI_HIDDEN) wm_ui_touch_motion(s, s->cursor->x, s->cursor->y);
    pointer_update(s, ev->time_msec);
}

static void cursor_motion_abs(struct wl_listener *l, void *data) {
    struct wm_server *s = wl_container_of(l, s, cursor_motion_abs);
    struct wlr_pointer_motion_absolute_event *ev = data;
    wlr_cursor_warp_absolute(s->cursor, &ev->pointer->base, ev->x, ev->y);
    if (s->ui != UI_HIDDEN) wm_ui_touch_motion(s, s->cursor->x, s->cursor->y);
    pointer_update(s, ev->time_msec);
}

static void cursor_button(struct wl_listener *l, void *data) {
    struct wm_server *s = wl_container_of(l, s, cursor_button);
    struct wlr_pointer_button_event *ev = data;
    if (s->ui != UI_HIDDEN) {
        if (ev->button == BTN_LEFT) {
            if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED) wm_ui_touch_down(s, s->cursor->x, s->cursor->y);
            else wm_ui_touch_up(s, s->cursor->x, s->cursor->y);
        }
        return;
    }
    if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        double sx, sy;
        struct wlr_surface *surface;
        struct wm_view *v = wm_view_at(s, s->cursor->x, s->cursor->y, &surface, &sx, &sy);
        if (v && v != s->focused) wm_focus_view(s, v);
    }
    wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button, ev->state);
}

static void cursor_axis(struct wl_listener *l, void *data) {
    struct wm_server *s = wl_container_of(l, s, cursor_axis);
    struct wlr_pointer_axis_event *ev = data;
    wlr_seat_pointer_notify_axis(s->seat, ev->time_msec, ev->orientation, ev->delta,
                                 ev->delta_discrete, ev->source, ev->relative_direction);
}

static void cursor_frame(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_server *s = wl_container_of(l, s, cursor_frame);
    wlr_seat_pointer_notify_frame(s->seat);
}

static void new_input(struct wl_listener *l, void *data) {
    struct wm_server *s = wl_container_of(l, s, new_input);
    struct wlr_input_device *dev = data;
    switch (dev->type) {
        case WLR_INPUT_DEVICE_KEYBOARD:
            add_keyboard(s, wlr_keyboard_from_input_device(dev));
            break;
        case WLR_INPUT_DEVICE_POINTER:
            wlr_cursor_attach_input_device(s->cursor, dev);
            s->has_pointer = true;
            break;
        case WLR_INPUT_DEVICE_TOUCH:
            add_touch(s, wlr_touch_from_input_device(dev));
            break;
        default:
            break;
    }
    update_caps(s);
}

static void request_set_selection(struct wl_listener *l, void *data) {
    struct wm_server *s = wl_container_of(l, s, request_set_selection);
    struct wlr_seat_request_set_selection_event *ev = data;
    wlr_seat_set_selection(s->seat, ev->source, ev->serial);
}

/* ----------------------------------------------------------- el mando */

// Nombre del botón para los atajos («A», «LEFT», «ZL»...), según la distribución del mando
static const char *button_name(const GamepadInput *gp, int code) {
    switch (code) {
        case BTN_EAST: return gp->nintendo_layout ? "A" : "B";
        case BTN_SOUTH: return gp->nintendo_layout ? "B" : "A";
        case BTN_NORTH: return gp->nintendo_layout ? "X" : "Y";
        case BTN_WEST: return gp->nintendo_layout ? "Y" : "X";
        case BTN_TL: return "L";
        case BTN_TR: return "R";
        case BTN_TL2: return "ZL";
        case BTN_TR2: return "ZR";
        case BTN_START: return "PLUS";
        case BTN_SELECT: return "MINUS";
        case BTN_THUMBL: return "L3";
        case BTN_THUMBR: return "R3";
        case BTN_DPAD_UP: return "UP";
        case BTN_DPAD_DOWN: return "DOWN";
        case BTN_DPAD_LEFT: return "LEFT";
        case BTN_DPAD_RIGHT: return "RIGHT";
        case KEY_KEYBOARD: return "TV";
        default: return NULL;
    }
}

static NavAction button_nav(const char *name) {
    if (!name) return NAV_NONE;
    if (!strcmp(name, "UP")) return NAV_UP;
    if (!strcmp(name, "DOWN")) return NAV_DOWN;
    if (!strcmp(name, "LEFT")) return NAV_LEFT;
    if (!strcmp(name, "RIGHT")) return NAV_RIGHT;
    if (!strcmp(name, "A")) return NAV_ACTION_A;
    if (!strcmp(name, "B")) return NAV_ACTION_B;
    if (!strcmp(name, "X")) return NAV_ACTION_X;
    if (!strcmp(name, "Y")) return NAV_ACTION_Y;
    if (!strcmp(name, "L")) return NAV_PAGE_PREV;
    if (!strcmp(name, "R")) return NAV_PAGE_NEXT;
    return NAV_NONE;
}

// Mientras HOME está pulsado o se ve la interfaz, el mando es solo del gestor:
// la app de delante no recibe esos botones (EVIOCGRAB).
void wm_set_gamepad_grab(struct wm_server *s, bool grab) {
    if (s->no_gamepad || !s->gamepad.is_open || grab == s->gamepad_grabbed) return;
    if (ioctl(s->gamepad.fd, EVIOCGRAB, grab ? 1 : 0) == 0) s->gamepad_grabbed = grab;
}

static void update_grab(struct wm_server *s) {
    wm_set_gamepad_grab(s, s->home_held || s->ui != UI_HIDDEN);
}

static int stick_dir(int v) { return v < -16000 ? -1 : (v > 16000 ? 1 : 0); }

static int gamepad_readable(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct wm_server *s = data;
    GamepadInput *gp = &s->gamepad;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) goto lost;

    struct input_event ev;
    ssize_t n;
    while ((n = read(gp->fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_KEY && ev.code == BTN_MODE) {
            if (ev.value == 1) {
                s->home_held = true;
                s->home_combo = false;
            } else if (ev.value == 0 && s->home_held) {
                s->home_held = false;
                if (!s->home_combo) run_bind(s, "HOME");
            }
            update_grab(s);
            continue;
        }
        if (ev.type == EV_KEY && ev.value == 1) {
            const char *name = button_name(gp, ev.code);
            if (!name) continue;
            if (s->home_held) {
                char key[48];
                snprintf(key, sizeof(key), "HOME+%s", name);
                if (run_bind(s, key)) s->home_combo = true;
                continue;
            }
            if (s->ui != UI_HIDDEN) {
                NavAction a = button_nav(name);
                if (!strcmp(name, "PLUS")) a = NAV_ACTION_PLUS;
                if (a != NAV_NONE) wm_ui_nav(s, a);
            } else {
                char key[48];
                snprintf(key, sizeof(key), "%s", name);   // atajos sin HOME (p. ej. "TV")
                run_bind(s, key);
            }
        } else if (ev.type == EV_ABS && (ev.code == ABS_RX || ev.code == ABS_RY || ev.code == ABS_Z)) {
            // Stick derecho (estándar Linux: ABS_RX = X, ABS_RY = Y): desplaza el Inicio hacia el cajón
            if (ev.code == ABS_RX || ev.code == ABS_Z) gp->abs_rx = ev.value; else gp->abs_ry = ev.value;
            if (s->ui == UI_HOME && !s->home_held) wm_ui_stick(s);
        } else if (ev.type == EV_ABS && s->ui != UI_HIDDEN && !s->home_held) {
            // Stick izquierdo para moverse por la interfaz
            if (ev.code == ABS_X) {
                int d = stick_dir(ev.value);
                if (d != gp->stick_x_prev && d) wm_ui_nav(s, d < 0 ? NAV_LEFT : NAV_RIGHT);
                gp->stick_x_prev = d;
            } else if (ev.code == ABS_Y) {
                int d = stick_dir(ev.value);
                if (d != gp->stick_y_prev && d) wm_ui_nav(s, d < 0 ? NAV_UP : NAV_DOWN);
                gp->stick_y_prev = d;
            }
        }
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR) goto lost;
    update_grab(s);
    return 0;

lost:
    wlr_log(WLR_INFO, "[wm] Mando desconectado");
    wl_event_source_remove(s->gamepad_source);
    s->gamepad_source = NULL;
    s->gamepad_grabbed = false;
    s->home_held = false;
    gamepad_close(gp);
    return 0;
}

// drc_stream crea el mando al conectar el GamePad: se busca cada segundo
static int gamepad_rescan(void *data) {
    struct wm_server *s = data;
    if (s->no_gamepad) return 0;
    if (!s->gamepad.is_open) {
        gamepad_maintain(&s->gamepad, now_ms());
        if (s->gamepad.is_open) {
            s->gamepad_source = wl_event_loop_add_fd(s->loop, s->gamepad.fd, WL_EVENT_READABLE,
                                                     gamepad_readable, s);
            update_grab(s);
        }
    }
    wl_event_source_timer_update(s->gamepad_rescan, 1000);
    return 0;
}

void wm_input_init(struct wm_server *s) {
    wl_list_init(&s->keyboards);
    wl_list_init(&s->touches);
    s->seat = wlr_seat_create(s->display, "seat0");
    s->new_input.notify = new_input;
    wl_signal_add(&s->backend->events.new_input, &s->new_input);
    s->request_set_selection.notify = request_set_selection;
    wl_signal_add(&s->seat->events.request_set_selection, &s->request_set_selection);

    s->cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(s->cursor, s->output_layout);
    s->cursor_motion.notify = cursor_motion;
    wl_signal_add(&s->cursor->events.motion, &s->cursor_motion);
    s->cursor_motion_abs.notify = cursor_motion_abs;
    wl_signal_add(&s->cursor->events.motion_absolute, &s->cursor_motion_abs);
    s->cursor_button.notify = cursor_button;
    wl_signal_add(&s->cursor->events.button, &s->cursor_button);
    s->cursor_axis.notify = cursor_axis;
    wl_signal_add(&s->cursor->events.axis, &s->cursor_axis);
    s->cursor_frame.notify = cursor_frame;
    wl_signal_add(&s->cursor->events.frame, &s->cursor_frame);

    struct wlr_virtual_keyboard_manager_v1 *vkm = wlr_virtual_keyboard_manager_v1_create(s->display);
    s->new_virtual_keyboard.notify = new_virtual_keyboard;
    wl_signal_add(&vkm->events.new_virtual_keyboard, &s->new_virtual_keyboard);

    if (s->no_gamepad) {
        memset(&s->gamepad, 0, sizeof(s->gamepad));
        s->gamepad.fd = -1;
        return;
    }
    gamepad_init(&s->gamepad, &s->cfg);
    s->gamepad.stick_nav = false;
    if (s->gamepad.is_open)
        s->gamepad_source = wl_event_loop_add_fd(s->loop, s->gamepad.fd, WL_EVENT_READABLE, gamepad_readable, s);
    s->gamepad_rescan = wl_event_loop_add_timer(s->loop, gamepad_rescan, s);
    wl_event_source_timer_update(s->gamepad_rescan, 1000);
}

/* --------------------------------------------- control externo (FIFO) */

static int ctl_readable(int fd, uint32_t mask, void *data) {
    (void)mask;
    struct wm_server *s = data;
    static char buf[1024];
    static size_t used;
    ssize_t n;
    while ((n = read(fd, buf + used, sizeof(buf) - 1 - used)) > 0) {
        used += n;
        buf[used] = '\0';
        char *line, *nl;
        line = buf;
        while ((nl = strchr(line, '\n'))) {
            *nl = '\0';
            if (*line) wm_run_action(s, line);
            line = nl + 1;
        }
        used = strlen(line);
        memmove(buf, line, used + 1);
        if (used >= sizeof(buf) - 1) used = 0;
    }
    return 0;
}

void wm_ctl_init(struct wm_server *s, const char *socket) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt) return;
    snprintf(s->ctl_path, sizeof(s->ctl_path), "%.180s/wiiu-wm-%.40s.ctl", rt, socket);
    unlink(s->ctl_path);
    if (mkfifo(s->ctl_path, 0600) != 0) { s->ctl_path[0] = '\0'; return; }
    // O_RDWR: el FIFO no da EOF cuando un escritor cierra
    s->ctl_fd = open(s->ctl_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (s->ctl_fd < 0) return;
    s->ctl_source = wl_event_loop_add_fd(s->loop, s->ctl_fd, WL_EVENT_READABLE, ctl_readable, s);
    setenv("WIIU_WM_CTL", s->ctl_path, 1);
    wlr_log(WLR_INFO, "[wm] Control: echo <acción> > %s", s->ctl_path);
}

void wm_ctl_finish(struct wm_server *s) {
    if (s->ctl_source) wl_event_source_remove(s->ctl_source);
    if (s->ctl_fd > 0) close(s->ctl_fd);
    if (s->ctl_path[0]) unlink(s->ctl_path);
}
