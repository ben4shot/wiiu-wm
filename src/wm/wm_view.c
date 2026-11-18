#include "wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/log.h>

/* ------------------------------------------------------------- utilidades */

const char *wm_view_app_id(struct wm_view *v) {
    if (!v) return "";
    if (v->type == WM_VIEW_XWAYLAND) {
        return (v->xwayland_surface && v->xwayland_surface->class) ? v->xwayland_surface->class : "";
    }
    return (v->xdg && v->xdg->app_id) ? v->xdg->app_id : "";
}

const char *wm_view_title(struct wm_view *v) {
    if (!v) return "";
    if (v->type == WM_VIEW_XWAYLAND) {
        return (v->xwayland_surface && v->xwayland_surface->title) ? v->xwayland_surface->title : "";
    }
    return (v->xdg && v->xdg->title) ? v->xdg->title : "";
}

void wm_view_close(struct wm_view *v) {
    if (!v) return;
    if (v->type == WM_VIEW_XWAYLAND) {
        if (v->xwayland_surface) wlr_xwayland_surface_close(v->xwayland_surface);
    } else if (v->xdg) {
        wlr_xdg_toplevel_send_close(v->xdg);
    }
}

int wm_view_count(struct wm_server *s, int ws) {
    int n = 0;
    struct wm_view *v;
    wl_list_for_each(v, &s->views, link) {
        if (v->mapped && (ws < 0 || v->workspace == ws)) n++;
    }
    return n;
}

static struct wm_view *view_from_xdg(struct wlr_xdg_toplevel *xdg) {
    struct wlr_scene_tree *tree = xdg->base->data;
    return tree ? tree->node.data : NULL;
}

/* --------------------------------------------------------------- foco */

void wm_focus_view(struct wm_server *s, struct wm_view *v) {
    struct wlr_surface *prev = s->seat->keyboard_state.focused_surface;
    if (s->focused && s->focused != v && s->focused->mapped) {
        if (s->focused->type == WM_VIEW_XWAYLAND) {
            if (s->focused->xwayland_surface)
                wlr_xwayland_surface_activate(s->focused->xwayland_surface, false);
        } else if (s->focused->xdg) {
            wlr_xdg_toplevel_set_activated(s->focused->xdg, false);
        }
    }
    s->focused = v;
    if (!v) {
        wlr_seat_keyboard_notify_clear_focus(s->seat);
        wm_arrange(s);
        wm_write_state(s);
        return;
    }
    v->focus_seq = ++s->focus_counter;
    struct wlr_surface *surface = NULL;
    if (v->type == WM_VIEW_XWAYLAND) {
        if (v->xwayland_surface) {
            wlr_xwayland_surface_activate(v->xwayland_surface, true);
            wlr_xwayland_surface_restack(v->xwayland_surface, NULL, XCB_STACK_MODE_ABOVE);
            surface = v->xwayland_surface->surface;
        }
    } else if (v->xdg) {
        wlr_xdg_toplevel_set_activated(v->xdg, true);
        surface = v->xdg->base ? v->xdg->base->surface : NULL;
    }
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
    if (kb && prev != surface && surface) {
        wlr_seat_keyboard_notify_enter(s->seat, surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
    }
    wm_arrange(s);   // orden de apilado y bordes
    wm_write_state(s);
}

// La ventana del escritorio actual que tuvo el foco más recientemente
static struct wm_view *last_focused_in(struct wm_server *s, int ws) {
    struct wm_view *best = NULL, *v;
    wl_list_for_each(v, &s->views, link) {
        if (!v->mapped || v->workspace != ws) continue;
        if (!best || v->focus_seq > best->focus_seq) best = v;
    }
    return best;
}

void wm_focus_cycle(struct wm_server *s, int dir) {
    // Orden estable: por orden de creación dentro del escritorio
    struct wm_view *list[64];
    int n = 0;
    struct wm_view *v;
    wl_list_for_each_reverse(v, &s->views, link) {
        if (v->mapped && v->workspace == s->workspace && n < 64) list[n++] = v;
    }
    if (n == 0) return;
    int cur = -1;
    for (int i = 0; i < n; i++) if (list[i] == s->focused) cur = i;
    int next = cur < 0 ? 0 : ((cur + dir) % n + n) % n;
    wm_focus_view(s, list[next]);
}

void wm_close_focused(struct wm_server *s) {
    if (s->focused && s->focused->mapped) wm_view_close(s->focused);
}

/* ------------------------------------------------------ escritorios */

void wm_switch_workspace(struct wm_server *s, int ws) {
    if (ws < 0) ws = s->cfg.workspaces - 1;
    if (ws >= s->cfg.workspaces) ws = 0;
    s->workspace = ws;
    for (int i = 0; i < WM_MAX_WS; i++) wlr_scene_node_set_enabled(&s->ws_tree[i]->node, i == ws);
    wm_focus_view(s, last_focused_in(s, ws));
    char msg[64];
    int n = wm_view_count(s, ws);
    snprintf(msg, sizeof(msg), "Escritorio %d/%d%s", ws + 1, s->cfg.workspaces, n == 0 ? "  ·  vacío" : "");
    wm_ui_osd(s, msg);
}

void wm_move_focused_to(struct wm_server *s, int ws) {
    struct wm_view *v = s->focused;
    if (!v) return;
    if (ws < 0) ws = s->cfg.workspaces - 1;
    if (ws >= s->cfg.workspaces) ws = 0;
    v->workspace = ws;
    wlr_scene_node_reparent(&v->tree->node, s->ws_tree[ws]);
    // Se sigue a la ventana a su nuevo escritorio
    wm_switch_workspace(s, ws);
    wm_focus_view(s, v);
    wm_ui_views_changed(s);
}

void wm_toggle_tile(struct wm_server *s, struct wm_view *v) {
    if (!v) return;
    v->tiled = !v->tiled;
    wm_arrange(s);
    wm_ui_osd(s, v->tiled ? "Mosaico activado" : "Mosaico desactivado");
    wm_ui_views_changed(s);
}

/* ---------------------------------------------------- disposición */

// «Dwindle» como Hyprland: cada ventana nueva parte el hueco restante por su lado largo
static void dwindle(struct wm_view **list, int n, struct wlr_box area, int gap) {
    for (int i = 0; i < n; i++) {
        if (i == n - 1) { list[i]->box = area; break; }
        struct wlr_box a = area, b = area;
        if (area.width >= area.height) {
            a.width = (area.width - gap) / 2;
            b.x = area.x + a.width + gap;
            b.width = area.width - a.width - gap;
        } else {
            a.height = (area.height - gap) / 2;
            b.y = area.y + a.height + gap;
            b.height = area.height - a.height - gap;
        }
        list[i]->box = a;
        area = b;
    }
}

static void place_view(struct wm_view *v) {
    int geo_x = 0, geo_y = 0;
    if (v->type == WM_VIEW_XDG && v->xdg && v->xdg->base) {
        struct wlr_box *geo = &v->xdg->base->geometry;
        geo_x = geo->x;
        geo_y = geo->y;
    }
    wlr_scene_node_set_position(&v->tree->node, v->box.x - geo_x, v->box.y - geo_y);
    for (int i = 0; i < 4; i++) wlr_scene_node_set_enabled(&v->border[i]->node, v->tiled);
    if (v->tiled) {
        // Marco de 2 px alrededor de la caja (en coordenadas del árbol de la ventana)
        int b = 2, x = geo_x - b, y = geo_y - b, w = v->box.width + 2 * b, h = v->box.height + 2 * b;
        wlr_scene_rect_set_size(v->border[0], w, b);  wlr_scene_node_set_position(&v->border[0]->node, x, y);
        wlr_scene_rect_set_size(v->border[1], w, b);  wlr_scene_node_set_position(&v->border[1]->node, x, y + h - b);
        wlr_scene_rect_set_size(v->border[2], b, h);  wlr_scene_node_set_position(&v->border[2]->node, x, y);
        wlr_scene_rect_set_size(v->border[3], b, h);  wlr_scene_node_set_position(&v->border[3]->node, x + w - b, y);
        const ColorRGBA *c = v == v->server->focused ? &v->server->cfg.tile_glow : &v->server->cfg.accent;
        float col[4] = { c->r, c->g, c->b, v == v->server->focused ? 1.0f : 0.55f };
        for (int i = 0; i < 4; i++) wlr_scene_rect_set_color(v->border[i], col);
    }
}

void wm_arrange(struct wm_server *s) {
    int gap = s->cfg.tile_gap;
    for (int ws = 0; ws < WM_MAX_WS; ws++) {
        struct wm_view *tiled[64];
        int nt = 0;
        struct wm_view *v;
        wl_list_for_each_reverse(v, &s->views, link) {
            if (!v->mapped || v->workspace != ws) continue;
            if (v->tiled && nt < 64) tiled[nt++] = v;
        }
        struct wlr_box full = wm_usable_area(s);
        struct wlr_box area = { full.x + gap, full.y + gap, full.width - 2 * gap, full.height - 2 * gap };
        dwindle(tiled, nt, area, gap);
        wl_list_for_each(v, &s->views, link) {
            if (!v->mapped || v->workspace != ws) continue;
            if (!v->tiled) v->box = full;
            if (v->type == WM_VIEW_XWAYLAND) {
                wlr_xwayland_surface_configure(v->xwayland_surface,
                    v->box.x, v->box.y, v->box.width, v->box.height);
                wlr_xwayland_surface_set_maximized(v->xwayland_surface, !v->tiled, !v->tiled);
            } else {
                wlr_xdg_toplevel_set_size(v->xdg, v->box.width, v->box.height);
                wlr_xdg_toplevel_set_maximized(v->xdg, !v->tiled);
                wlr_xdg_toplevel_set_tiled(v->xdg, v->tiled ? WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT
                                                            : WLR_EDGE_NONE);
            }
            place_view(v);
        }
        // Apilado: mosaico abajo; flotantes (pantalla completa) encima; el enfocado arriba de su grupo
        for (int pass = 0; pass < 2; pass++) {
            struct wm_view *order[64];
            int n = 0;
            wl_list_for_each(v, &s->views, link) {
                if (v->mapped && v->workspace == ws && v->tiled == (pass == 0) && n < 64) order[n++] = v;
            }
            // de menos a más reciente en foco
            for (int i = 0; i < n; i++)
                for (int j = i + 1; j < n; j++)
                    if (order[j]->focus_seq < order[i]->focus_seq) { struct wm_view *t = order[i]; order[i] = order[j]; order[j] = t; }
            for (int i = 0; i < n; i++) wlr_scene_node_raise_to_top(&order[i]->tree->node);
        }
    }
}

/* ------------------------------------------------------- ventanas xdg */

static void view_map(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_view *v = wl_container_of(l, v, map);
    struct wm_server *s = v->server;
    v->mapped = true;
    wl_client_get_credentials(wl_resource_get_client(v->xdg->resource), &v->pid, NULL, NULL);
    wlr_log(WLR_INFO, "[wm] Ventana '%s' (%s) en el escritorio %d", wm_view_title(v), wm_view_app_id(v), v->workspace + 1);
    if (s->ui != UI_HIDDEN) wm_ui_show(s, UI_HIDDEN);   // la app recién abierta pasa al frente
    wm_focus_view(s, v);
    wm_ui_views_changed(s);
}

static void view_unmap(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_view *v = wl_container_of(l, v, unmap);
    struct wm_server *s = v->server;
    v->mapped = false;
    if (s->focused == v) {
        s->focused = NULL;
        wm_focus_view(s, last_focused_in(s, s->workspace));
    } else {
        wm_arrange(s);
    }
    wm_ui_views_changed(s);
    // Sin ninguna ventana no puede quedar la pantalla vacía: Inicio
    if (wm_view_count(s, -1) == 0 && s->ui == UI_HIDDEN) wm_ui_show(s, UI_HOME);
}

static void view_commit(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_view *v = wl_container_of(l, v, commit);
    struct wm_server *s = v->server;
    if (v->xdg->base->initial_commit) {
        v->workspace = s->workspace;
        v->tiled = s->cfg.tile_new_windows;
        if (v->decoration)
            wlr_xdg_toplevel_decoration_v1_set_mode(v->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        wlr_xdg_toplevel_set_wm_capabilities(v->xdg, WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
        // Primer tamaño ya descontando la barra: hay apps que solo hacen caso a la primera orden
        struct wlr_box area = wm_usable_area(s);
        if (!v->tiled) {
            wlr_xdg_toplevel_set_maximized(v->xdg, true);
            wlr_xdg_toplevel_set_size(v->xdg, area.width, area.height);
        } else {
            wlr_xdg_toplevel_set_size(v->xdg, area.width / 2, area.height);
        }
        return;
    }
    if (v->mapped) {
        place_view(v);   // la geometría del cliente puede cambiar
        struct wlr_box *g = &v->xdg->base->geometry;
        if (g->width != v->logged_w || g->height != v->logged_h) {   // diagnóstico: ¿obedece al tamaño pedido?
            v->logged_w = g->width;
            v->logged_h = g->height;
            wlr_log(WLR_INFO, "[wm] '%s' ocupa %dx%d (caja %dx%d)", wm_view_app_id(v), g->width, g->height, v->box.width, v->box.height);
        }
    }
}

static void view_destroy(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_view *v = wl_container_of(l, v, destroy);
    struct wm_server *s = v->server;
    wl_list_remove(&v->map.link);
    wl_list_remove(&v->unmap.link);
    wl_list_remove(&v->commit.link);
    wl_list_remove(&v->destroy.link);
    wl_list_remove(&v->request_maximize.link);
    wl_list_remove(&v->request_fullscreen.link);
    wl_list_remove(&v->request_move.link);
    wl_list_remove(&v->request_resize.link);
    wl_list_remove(&v->link);
    if (s->focused == v) s->focused = NULL;
    free(v);
    wm_ui_views_changed(s);
}

// Maximizar/pantalla completa/mover/redimensionar los decide el gestor: solo se responde
static void view_request_noop(struct wl_listener *l, void *data) {
    (void)data;
    (void)l;
}

static void view_request_maximize(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_view *v = wl_container_of(l, v, request_maximize);
    if (v->xdg->base->initialized) wlr_xdg_surface_schedule_configure(v->xdg->base);
}

static void view_request_fullscreen(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_view *v = wl_container_of(l, v, request_fullscreen);
    if (v->xdg->base->initialized) wlr_xdg_surface_schedule_configure(v->xdg->base);
}

static void new_xdg_toplevel(struct wl_listener *l, void *data) {
    struct wm_server *s = wl_container_of(l, s, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg = data;

    struct wm_view *v = calloc(1, sizeof(*v));
    v->server = s;
    v->xdg = xdg;
    v->id = ++s->next_view_id;
    v->workspace = s->workspace;
    v->tree = wlr_scene_tree_create(s->ws_tree[s->workspace]);
    v->tree->node.data = v;
    const float dim[4] = { 0.5f, 0.3f, 0.9f, 1.0f };
    for (int i = 0; i < 4; i++) {
        v->border[i] = wlr_scene_rect_create(v->tree, 1, 1, dim);
        wlr_scene_node_set_enabled(&v->border[i]->node, false);
    }
    v->surface_tree = wlr_scene_xdg_surface_create(v->tree, xdg->base);
    xdg->base->data = v->surface_tree;
    v->surface_tree->node.data = v;

    v->map.notify = view_map;
    wl_signal_add(&xdg->base->surface->events.map, &v->map);
    v->unmap.notify = view_unmap;
    wl_signal_add(&xdg->base->surface->events.unmap, &v->unmap);
    v->commit.notify = view_commit;
    wl_signal_add(&xdg->base->surface->events.commit, &v->commit);
    v->destroy.notify = view_destroy;
    wl_signal_add(&xdg->events.destroy, &v->destroy);
    v->request_maximize.notify = view_request_maximize;
    wl_signal_add(&xdg->events.request_maximize, &v->request_maximize);
    v->request_fullscreen.notify = view_request_fullscreen;
    wl_signal_add(&xdg->events.request_fullscreen, &v->request_fullscreen);
    v->request_move.notify = view_request_noop;
    wl_signal_add(&xdg->events.request_move, &v->request_move);
    v->request_resize.notify = view_request_noop;
    wl_signal_add(&xdg->events.request_resize, &v->request_resize);

    wl_list_insert(&s->views, &v->link);
}

/* ------------------------------------------------------------- popups */

struct wm_popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wl_listener commit, destroy;
};

static void popup_commit(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_popup *p = wl_container_of(l, p, commit);
    if (p->xdg_popup->base->initial_commit) wlr_xdg_surface_schedule_configure(p->xdg_popup->base);
}

static void popup_destroy(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_popup *p = wl_container_of(l, p, destroy);
    wl_list_remove(&p->commit.link);
    wl_list_remove(&p->destroy.link);
    free(p);
}

static void new_xdg_popup(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_xdg_popup *xdg_popup = data;
    // Los popups de layer-shell (las teclas ampliadas de wvkbd) nacen SIN padre: este se asigna
    // después con get_popup y entonces se colocan en layer_new_popup(). Aquí solo los de ventanas.
    if (xdg_popup->parent) {
        struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
        if (parent && parent->data)
            xdg_popup->base->data = wlr_scene_xdg_surface_create(parent->data, xdg_popup->base);
    }

    // En todo caso hay que responder a su primer commit con un configure
    struct wm_popup *p = calloc(1, sizeof(*p));
    p->xdg_popup = xdg_popup;
    p->commit.notify = popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &p->commit);
    p->destroy.notify = popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &p->destroy);
}

/* ----------------------------------------- layer-shell (teclado wvkbd) */

struct wm_layer {
    struct wm_server *server;
    struct wlr_layer_surface_v1 *ls;
    struct wlr_scene_layer_surface_v1 *scene;
    bool is_bar;
    struct wl_listener commit, destroy, new_popup;
};

static void layer_new_popup(struct wl_listener *l, void *data) {
    struct wm_layer *L = wl_container_of(l, L, new_popup);
    struct wlr_xdg_popup *popup = data;
    if (!popup->base->data)
        popup->base->data = wlr_scene_xdg_surface_create(L->scene->tree, popup->base);
}

static void layer_configure(struct wm_layer *L) {
    struct wm_server *s = L->server;
    if (L->is_bar) {
        // La barra va donde diga la configuración (editable en el editor), no donde la ancle waybar
        wlr_layer_surface_v1_configure(L->ls, (uint32_t)s->cfg.bar_w, (uint32_t)s->cfg.bar_h);
        wlr_scene_node_set_position(&L->scene->tree->node, (int)s->cfg.bar_x, (int)s->cfg.bar_y);
        wlr_scene_node_set_enabled(&L->scene->tree->node, s->cfg.bar_enabled);
        return;
    }
    struct wlr_box full = { 0, 0, s->width, s->height }, usable = full;
    wlr_scene_layer_surface_v1_configure(L->scene, &full, &usable);
}

void wm_bar_reconfigure(struct wm_server *s) {
    if (s->bar_layer) {
        struct wm_layer *L = s->bar_layer;
        wlr_scene_node_set_enabled(&L->scene->tree->node, s->cfg.bar_enabled);
        layer_configure(L);
    }
    wm_arrange(s);
}

pid_t wm_bar_client_pid(struct wm_server *s) {
    if (!s->bar_layer) return 0;
    struct wm_layer *L = s->bar_layer;
    if (!L->ls || !L->ls->resource) return 0;
    struct wl_client *client = wl_resource_get_client(L->ls->resource);
    if (!client) return 0;
    pid_t pid = 0;
    wl_client_get_credentials(client, &pid, NULL, NULL);
    return pid;
}

// Zona para las ventanas: toda la pantalla menos la barra si es una franja pegada a un borde
struct wlr_box wm_usable_area(struct wm_server *s) {
    struct wlr_box a = { 0, 0, s->width, s->height };
    const DesktopConfig *c = &s->cfg;
    if (!c->bar_enabled || !s->bar_layer) return a;
    if (c->bar_w >= s->width * 0.8) {
        if (c->bar_y + c->bar_h / 2 < s->height / 2) { a.y = (int)(c->bar_y + c->bar_h); a.height = s->height - a.y; }
        else a.height = (int)c->bar_y;
    } else if (c->bar_h >= s->height * 0.8) {
        if (c->bar_x + c->bar_w / 2 < s->width / 2) { a.x = (int)(c->bar_x + c->bar_w); a.width = s->width - a.x; }
        else a.width = (int)c->bar_x;
    }
    return a;
}

static void layer_commit(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_layer *L = wl_container_of(l, L, commit);
    if (L->ls->initial_commit || L->ls->current.committed) layer_configure(L);
}

static void layer_destroy(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_layer *L = wl_container_of(l, L, destroy);
    wl_list_remove(&L->commit.link);
    wl_list_remove(&L->destroy.link);
    wl_list_remove(&L->new_popup.link);
    if (L->server->bar_layer == L) {
        L->server->bar_layer = NULL;
        wm_arrange(L->server);
    }
    free(L);
}

static void new_layer_surface(struct wl_listener *l, void *data) {
    struct wm_server *s = wl_container_of(l, s, new_layer_surface);
    struct wlr_layer_surface_v1 *ls = data;
    if (!ls->output) {
        struct wm_output *o;
        wl_list_for_each(o, &s->outputs, link) { ls->output = o->wlr_output; break; }
        if (!ls->output) { wlr_layer_surface_v1_destroy(ls); return; }
    }
    struct wlr_scene_tree *parent;
    switch (ls->pending.layer) {
        case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: parent = s->layer_tree[0]; break;
        case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM: parent = s->layer_tree[1]; break;
        default: parent = s->top_tree; break;   // top/overlay: encima de todo (teclado en pantalla)
    }
    struct wm_layer *L = calloc(1, sizeof(*L));
    L->server = s;
    L->ls = ls;
    L->scene = wlr_scene_layer_surface_v1_create(parent, ls);
    ls->data = L->scene->tree;
    // Solo el primer waybar es «la barra»; un segundo (desplegable de ajustes) es una capa normal con su ancla
    if (ls->namespace && strcmp(ls->namespace, "waybar") == 0 && !s->bar_layer) {
        L->is_bar = true;
        s->bar_layer = L;
        wlr_scene_node_reparent(&L->scene->tree->node, s->top_tree);   // por encima de todo
        // Al reactivar la barra, waybar tarda en volver a aparecer: hasta ahora no había capa y las ventanas
        // se dejaron a pantalla completa; al aparecer hay que reservarle su franja otra vez
        wm_arrange(s);
    }
    L->commit.notify = layer_commit;
    wl_signal_add(&ls->surface->events.commit, &L->commit);
    L->destroy.notify = layer_destroy;
    wl_signal_add(&ls->events.destroy, &L->destroy);
    L->new_popup.notify = layer_new_popup;
    wl_signal_add(&ls->events.new_popup, &L->new_popup);
}

/* ------------------------------------------- decoraciones (ninguna) */

struct wm_deco {
    struct wlr_xdg_toplevel_decoration_v1 *dec;
    struct wl_listener request_mode, destroy;
};

static void deco_request_mode(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_deco *d = wl_container_of(l, d, request_mode);
    // Lado servidor = el cliente no dibuja barra de título; el gestor tampoco dibuja ninguna
    if (d->dec->toplevel->base->initialized)
        wlr_xdg_toplevel_decoration_v1_set_mode(d->dec, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void deco_destroy(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_deco *d = wl_container_of(l, d, destroy);
    struct wm_view *v = view_from_xdg(d->dec->toplevel);
    if (v) v->decoration = NULL;
    wl_list_remove(&d->request_mode.link);
    wl_list_remove(&d->destroy.link);
    free(d);
}

static void new_decoration(struct wl_listener *l, void *data) {
    (void)l;
    struct wlr_xdg_toplevel_decoration_v1 *dec = data;
    struct wm_view *v = view_from_xdg(dec->toplevel);
    if (v) v->decoration = dec;
    struct wm_deco *d = calloc(1, sizeof(*d));
    d->dec = dec;
    d->request_mode.notify = deco_request_mode;
    wl_signal_add(&dec->events.request_mode, &d->request_mode);
    d->destroy.notify = deco_destroy;
    wl_signal_add(&dec->events.destroy, &d->destroy);
    if (dec->toplevel->base->initialized)
        wlr_xdg_toplevel_decoration_v1_set_mode(dec, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

/* ----------------------------------------------------------- búsqueda */

struct wm_view *wm_view_at(struct wm_server *s, double lx, double ly,
                           struct wlr_surface **surface, double *sx, double *sy) {
    *surface = NULL;
    struct wlr_scene_node *node = wlr_scene_node_at(&s->scene->tree.node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) return NULL;
    struct wlr_scene_buffer *buf = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buf);
    if (!ss) return NULL;
    *surface = ss->surface;
    struct wlr_scene_tree *tree = node->parent;
    while (tree && !tree->node.data) tree = tree->node.parent;
    return tree ? tree->node.data : NULL;
}

/* --------------------------------------------------- ventanas xwayland */

struct wm_xwayland_unmanaged {
    struct wm_server *server;
    struct wlr_xwayland_surface *xsurface;
    struct wlr_scene_tree *tree;
    struct wl_listener associate, dissociate, destroy, request_configure, set_geometry;
};

static void unmanaged_destroy(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_xwayland_unmanaged *u = wl_container_of(l, u, destroy);
    wl_list_remove(&u->associate.link);
    wl_list_remove(&u->dissociate.link);
    wl_list_remove(&u->destroy.link);
    wl_list_remove(&u->request_configure.link);
    wl_list_remove(&u->set_geometry.link);
    if (u->tree) wlr_scene_node_destroy(&u->tree->node);
    free(u);
}

static void unmanaged_associate(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_xwayland_unmanaged *u = wl_container_of(l, u, associate);
    if (!u->tree && u->xsurface->surface) {
        u->tree = wlr_scene_subsurface_tree_create(u->server->top_tree, u->xsurface->surface);
        wlr_scene_node_set_position(&u->tree->node, u->xsurface->x, u->xsurface->y);
    }
}

static void unmanaged_dissociate(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_xwayland_unmanaged *u = wl_container_of(l, u, dissociate);
    if (u->tree) {
        wlr_scene_node_destroy(&u->tree->node);
        u->tree = NULL;
    }
}

static void unmanaged_request_configure(struct wl_listener *l, void *data) {
    struct wm_xwayland_unmanaged *u = wl_container_of(l, u, request_configure);
    struct wlr_xwayland_surface_configure_event *ev = data;
    wlr_xwayland_surface_configure(u->xsurface, ev->x, ev->y, ev->width, ev->height);
    if (u->tree) {
        wlr_scene_node_set_position(&u->tree->node, ev->x, ev->y);
    }
}

static void unmanaged_set_geometry(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_xwayland_unmanaged *u = wl_container_of(l, u, set_geometry);
    if (u->tree) {
        wlr_scene_node_set_position(&u->tree->node, u->xsurface->x, u->xsurface->y);
    }
}

static void safe_remove_listener(struct wl_listener *l) {
    if (l->link.prev && l->link.next && !wl_list_empty(&l->link)) {
        wl_list_remove(&l->link);
        wl_list_init(&l->link);
    }
}

static void xwayland_view_map(struct wl_listener *l, void *data);
static void xwayland_view_unmap(struct wl_listener *l, void *data);
static void xwayland_view_commit(struct wl_listener *l, void *data);
static void xwayland_view_destroy(struct wl_listener *l, void *data);
static void xwayland_view_request_configure(struct wl_listener *l, void *data);
static void xwayland_view_request_activate(struct wl_listener *l, void *data);
static void xwayland_view_request_maximize(struct wl_listener *l, void *data);
static void xwayland_view_request_fullscreen(struct wl_listener *l, void *data);
static void xwayland_view_set_title(struct wl_listener *l, void *data);
static void xwayland_view_set_class(struct wl_listener *l, void *data);
static void xwayland_view_associate(struct wl_listener *l, void *data);
static void xwayland_view_dissociate(struct wl_listener *l, void *data);

static void xwayland_view_associate(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_view *v = wl_container_of(l, v, associate);
    struct wlr_xwayland_surface *xsurface = v->xwayland_surface;
    if (!xsurface || !xsurface->surface) return;
    if (!v->surface_tree) {
        v->surface_tree = wlr_scene_subsurface_tree_create(v->tree, xsurface->surface);
        v->surface_tree->node.data = v;
    }
    safe_remove_listener(&v->map);
    v->map.notify = xwayland_view_map;
    wl_signal_add(&xsurface->surface->events.map, &v->map);

    safe_remove_listener(&v->unmap);
    v->unmap.notify = xwayland_view_unmap;
    wl_signal_add(&xsurface->surface->events.unmap, &v->unmap);

    safe_remove_listener(&v->commit);
    v->commit.notify = xwayland_view_commit;
    wl_signal_add(&xsurface->surface->events.commit, &v->commit);
}

static void xwayland_view_dissociate(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_view *v = wl_container_of(l, v, dissociate);
    safe_remove_listener(&v->map);
    safe_remove_listener(&v->unmap);
    safe_remove_listener(&v->commit);
    if (v->surface_tree) {
        wlr_scene_node_destroy(&v->surface_tree->node);
        v->surface_tree = NULL;
    }
}

static void xwayland_view_map(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_view *v = wl_container_of(l, v, map);
    struct wm_server *s = v->server;
    v->mapped = true;
    v->pid = v->xwayland_surface->pid;
    v->workspace = s->workspace;
    v->tiled = s->cfg.tile_new_windows;

    struct wlr_box area = wm_usable_area(s);
    if (!v->tiled) {
        v->box = area;
        wlr_xwayland_surface_set_maximized(v->xwayland_surface, true, true);
        wlr_xwayland_surface_configure(v->xwayland_surface, area.x, area.y, area.width, area.height);
    } else {
        v->box = (struct wlr_box){ area.x, area.y, area.width / 2, area.height };
        wlr_xwayland_surface_configure(v->xwayland_surface, v->box.x, v->box.y, v->box.width, v->box.height);
    }
    place_view(v);

    wlr_log(WLR_INFO, "[wm] Ventana Xwayland '%s' (%s) en el escritorio %d",
            wm_view_title(v), wm_view_app_id(v), v->workspace + 1);
    if (s->ui != UI_HIDDEN) wm_ui_show(s, UI_HIDDEN);
    wm_focus_view(s, v);
    wm_ui_views_changed(s);
}

static void xwayland_view_unmap(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_view *v = wl_container_of(l, v, unmap);
    struct wm_server *s = v->server;
    v->mapped = false;
    if (s->focused == v) {
        s->focused = NULL;
        wm_focus_view(s, last_focused_in(s, s->workspace));
    } else {
        wm_arrange(s);
    }
    wm_ui_views_changed(s);
    if (wm_view_count(s, -1) == 0 && s->ui == UI_HIDDEN) wm_ui_show(s, UI_HOME);
}

static void xwayland_view_commit(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_view *v = wl_container_of(l, v, commit);
    if (v->mapped) {
        place_view(v);
        int w = v->xwayland_surface->width;
        int h = v->xwayland_surface->height;
        if (w != v->logged_w || h != v->logged_h) {
            v->logged_w = w;
            v->logged_h = h;
            wlr_log(WLR_INFO, "[wm] (Xwayland) '%s' ocupa %dx%d (caja %dx%d)",
                    wm_view_app_id(v), w, h, v->box.width, v->box.height);
        }
    }
}

static void xwayland_view_destroy(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_view *v = wl_container_of(l, v, destroy);
    struct wm_server *s = v->server;
    safe_remove_listener(&v->map);
    safe_remove_listener(&v->unmap);
    safe_remove_listener(&v->commit);
    safe_remove_listener(&v->destroy);
    safe_remove_listener(&v->request_configure);
    safe_remove_listener(&v->request_activate);
    safe_remove_listener(&v->request_maximize);
    safe_remove_listener(&v->request_fullscreen);
    safe_remove_listener(&v->set_title);
    safe_remove_listener(&v->set_class);
    safe_remove_listener(&v->associate);
    safe_remove_listener(&v->dissociate);
    wl_list_remove(&v->link);
    if (s->focused == v) s->focused = NULL;
    if (v->surface_tree) {
        wlr_scene_node_destroy(&v->surface_tree->node);
        v->surface_tree = NULL;
    }
    if (v->tree) {
        wlr_scene_node_destroy(&v->tree->node);
        v->tree = NULL;
    }
    free(v);
    wm_ui_views_changed(s);
}

static void xwayland_view_request_configure(struct wl_listener *l, void *data) {
    struct wm_view *v = wl_container_of(l, v, request_configure);
    struct wlr_xwayland_surface_configure_event *ev = data;
    if (!v->mapped) {
        wlr_xwayland_surface_configure(v->xwayland_surface, ev->x, ev->y, ev->width, ev->height);
        return;
    }
    wlr_xwayland_surface_configure(v->xwayland_surface, v->box.x, v->box.y, v->box.width, v->box.height);
}

static void xwayland_view_request_activate(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_view *v = wl_container_of(l, v, request_activate);
    if (v->mapped) {
        if (v->workspace != v->server->workspace) {
            wm_switch_workspace(v->server, v->workspace);
        }
        wm_focus_view(v->server, v);
    }
}

static void xwayland_view_request_maximize(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_view *v = wl_container_of(l, v, request_maximize);
    struct wlr_box area = wm_usable_area(v->server);
    wlr_xwayland_surface_configure(v->xwayland_surface, area.x, area.y, area.width, area.height);
}

static void xwayland_view_request_fullscreen(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_view *v = wl_container_of(l, v, request_fullscreen);
    struct wlr_box area = wm_usable_area(v->server);
    wlr_xwayland_surface_configure(v->xwayland_surface, area.x, area.y, area.width, area.height);
}

static void xwayland_view_set_title(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_view *v = wl_container_of(l, v, set_title);
    wm_ui_views_changed(v->server);
}

static void xwayland_view_set_class(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_view *v = wl_container_of(l, v, set_class);
    wm_ui_views_changed(v->server);
}

static void new_xwayland_surface(struct wl_listener *l, void *data) {
    struct wm_server *s = wl_container_of(l, s, new_xwayland_surface);
    struct wlr_xwayland_surface *xsurface = data;

    if (xsurface->override_redirect) {
        struct wm_xwayland_unmanaged *u = calloc(1, sizeof(*u));
        u->server = s;
        u->xsurface = xsurface;
        u->destroy.notify = unmanaged_destroy;
        wl_signal_add(&xsurface->events.destroy, &u->destroy);
        u->associate.notify = unmanaged_associate;
        wl_signal_add(&xsurface->events.associate, &u->associate);
        u->dissociate.notify = unmanaged_dissociate;
        wl_signal_add(&xsurface->events.dissociate, &u->dissociate);
        u->request_configure.notify = unmanaged_request_configure;
        wl_signal_add(&xsurface->events.request_configure, &u->request_configure);
        u->set_geometry.notify = unmanaged_set_geometry;
        wl_signal_add(&xsurface->events.set_geometry, &u->set_geometry);
        if (xsurface->surface) unmanaged_associate(&u->associate, NULL);
        return;
    }

    struct wm_view *v = calloc(1, sizeof(*v));
    v->server = s;
    v->type = WM_VIEW_XWAYLAND;
    v->xwayland_surface = xsurface;
    xsurface->data = v;
    v->id = ++s->next_view_id;
    v->workspace = s->workspace;
    v->tree = wlr_scene_tree_create(s->ws_tree[s->workspace]);
    v->tree->node.data = v;
    const float dim[4] = { 0.5f, 0.3f, 0.9f, 1.0f };
    for (int i = 0; i < 4; i++) {
        v->border[i] = wlr_scene_rect_create(v->tree, 1, 1, dim);
        wlr_scene_node_set_enabled(&v->border[i]->node, false);
    }

    wl_list_init(&v->map.link);
    wl_list_init(&v->unmap.link);
    wl_list_init(&v->commit.link);

    v->destroy.notify = xwayland_view_destroy;
    wl_signal_add(&xsurface->events.destroy, &v->destroy);
    v->request_configure.notify = xwayland_view_request_configure;
    wl_signal_add(&xsurface->events.request_configure, &v->request_configure);
    v->request_activate.notify = xwayland_view_request_activate;
    wl_signal_add(&xsurface->events.request_activate, &v->request_activate);
    v->request_maximize.notify = xwayland_view_request_maximize;
    wl_signal_add(&xsurface->events.request_maximize, &v->request_maximize);
    v->request_fullscreen.notify = xwayland_view_request_fullscreen;
    wl_signal_add(&xsurface->events.request_fullscreen, &v->request_fullscreen);
    v->set_title.notify = xwayland_view_set_title;
    wl_signal_add(&xsurface->events.set_title, &v->set_title);
    v->set_class.notify = xwayland_view_set_class;
    wl_signal_add(&xsurface->events.set_class, &v->set_class);
    v->associate.notify = xwayland_view_associate;
    wl_signal_add(&xsurface->events.associate, &v->associate);
    v->dissociate.notify = xwayland_view_dissociate;
    wl_signal_add(&xsurface->events.dissociate, &v->dissociate);

    wl_list_insert(&s->views, &v->link);

    if (xsurface->surface) {
        xwayland_view_associate(&v->associate, NULL);
    }
}

static void xwayland_ready(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_server *s = wl_container_of(l, s, xwayland_ready);
    wlr_log(WLR_INFO, "[wm] Xwayland listo en DISPLAY=%s", s->xwayland->display_name);
    setenv("DISPLAY", s->xwayland->display_name, 1);
    if (s->seat) wlr_xwayland_set_seat(s->xwayland, s->seat);
}

void wm_views_init(struct wm_server *s) {
    wl_list_init(&s->views);
    s->xdg_shell = wlr_xdg_shell_create(s->display, 6);
    s->new_xdg_toplevel.notify = new_xdg_toplevel;
    wl_signal_add(&s->xdg_shell->events.new_toplevel, &s->new_xdg_toplevel);
    s->new_xdg_popup.notify = new_xdg_popup;
    wl_signal_add(&s->xdg_shell->events.new_popup, &s->new_xdg_popup);

    s->layer_shell = wlr_layer_shell_v1_create(s->display, 4);
    s->new_layer_surface.notify = new_layer_surface;
    wl_signal_add(&s->layer_shell->events.new_surface, &s->new_layer_surface);

    s->decoration_mgr = wlr_xdg_decoration_manager_v1_create(s->display);
    s->new_decoration.notify = new_decoration;
    wl_signal_add(&s->decoration_mgr->events.new_toplevel_decoration, &s->new_decoration);

    s->xwayland = wlr_xwayland_create(s->display, s->compositor, true);
    if (s->xwayland) {
        s->new_xwayland_surface.notify = new_xwayland_surface;
        wl_signal_add(&s->xwayland->events.new_surface, &s->new_xwayland_surface);
        s->xwayland_ready.notify = xwayland_ready;
        wl_signal_add(&s->xwayland->events.ready, &s->xwayland_ready);
        setenv("DISPLAY", s->xwayland->display_name, 1);
        wlr_log(WLR_INFO, "[wm] Xwayland inicializado (lazy) en DISPLAY=%s", s->xwayland->display_name);
    } else {
        wlr_log(WLR_ERROR, "[wm] No se pudo inicializar Xwayland");
    }
}
