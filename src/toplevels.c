#include "toplevels.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PENDING_WINDOW_TIMEOUT_MS 20000

static void handle_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *title) {
    (void)h;
    Toplevel *t = data;
    snprintf(t->title, sizeof(t->title), "%s", title ? title : "");
}

static void handle_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *app_id) {
    (void)h;
    Toplevel *t = data;
    snprintf(t->app_id, sizeof(t->app_id), "%s", app_id ? app_id : "");
}

static void handle_output_enter(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {
    (void)data; (void)h; (void)o;
}

static void handle_output_leave(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {
    (void)data; (void)h; (void)o;
}

static void handle_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *state) {
    (void)h;
    Toplevel *t = data;
    t->activated = t->minimized = t->maximized = t->fullscreen = false;
    uint32_t *s;
    wl_array_for_each(s, state) {
        switch (*s) {
            case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED: t->activated = true; break;
            case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED: t->minimized = true; break;
            case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED: t->maximized = true; break;
            case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN: t->fullscreen = true; break;
        }
    }
}

static void handle_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)h;
    Toplevel *t = data;
    ToplevelList *tl = t->list;
    if (!tl) return;

    if (!t->seen_done) {
        t->seen_done = true;
        t->is_self = tl->self_app_id && strcmp(t->app_id, tl->self_app_id) == 0;
        if (!t->is_self) {
            if (tl->pending_app_index >= 0) {
                t->app_index = tl->pending_app_index;
                tl->pending_app_index = -1;
            }
            if (tl->maximize_new && !t->maximized && !t->fullscreen) {
                zwlr_foreign_toplevel_handle_v1_set_maximized(t->handle);
            }
            printf("[Ventanas] Nueva: '%s' (app_id '%s')\n", t->title, t->app_id);
        }
    }
    tl->changed = true;
}

static void handle_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *h) {
    Toplevel *t = data;
    ToplevelList *tl = t->list;
    if (tl) {
        for (int i = 0; i < tl->count; i++) {
            if (tl->items[i] == t) {
                memmove(&tl->items[i], &tl->items[i + 1], (tl->count - i - 1) * sizeof(tl->items[0]));
                tl->count--;
                break;
            }
        }
        tl->changed = true;
    }
    if (!t->is_self) printf("[Ventanas] Cerrada: '%s'\n", t->title);
    zwlr_foreign_toplevel_handle_v1_destroy(h);
    free(t);
}

static void handle_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                          struct zwlr_foreign_toplevel_handle_v1 *parent) {
    (void)data; (void)h; (void)parent;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
    .title = handle_title,
    .app_id = handle_app_id,
    .output_enter = handle_output_enter,
    .output_leave = handle_output_leave,
    .state = handle_state,
    .done = handle_done,
    .closed = handle_closed,
    .parent = handle_parent,
};

static void manager_toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *mgr,
                             struct zwlr_foreign_toplevel_handle_v1 *handle) {
    (void)mgr;
    ToplevelList *tl = data;
    if (tl->count >= MAX_TOPLEVELS) {
        zwlr_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }
    Toplevel *t = calloc(1, sizeof(*t));
    if (!t) {
        zwlr_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }
    t->handle = handle;
    t->list = tl;
    t->app_index = -1;
    t->serial = tl->next_serial++;
    tl->items[tl->count++] = t;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, t);
}

static void manager_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *mgr) {
    ToplevelList *tl = data;
    zwlr_foreign_toplevel_manager_v1_destroy(mgr);
    tl->manager = NULL;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
    .toplevel = manager_toplevel,
    .finished = manager_finished,
};

void toplevels_init(ToplevelList *tl, const char *self_app_id, bool maximize_new) {
    memset(tl, 0, sizeof(*tl));
    tl->self_app_id = self_app_id;
    tl->maximize_new = maximize_new;
    tl->pending_app_index = -1;
}

void toplevels_bind(ToplevelList *tl, struct wl_registry *registry, uint32_t name, uint32_t version) {
    uint32_t v = version < 3 ? version : 3;
    tl->manager = wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface, v);
    zwlr_foreign_toplevel_manager_v1_add_listener(tl->manager, &manager_listener, tl);
}

void toplevels_destroy(ToplevelList *tl) {
    for (int i = 0; i < tl->count; i++) {
        zwlr_foreign_toplevel_handle_v1_destroy(tl->items[i]->handle);
        free(tl->items[i]);
    }
    tl->count = 0;
    if (tl->manager) zwlr_foreign_toplevel_manager_v1_stop(tl->manager);
    tl->manager = NULL;
}

bool toplevels_available(const ToplevelList *tl) {
    return tl->manager != NULL;
}

int toplevels_apps(const ToplevelList *tl, Toplevel **out, int max) {
    int n = 0;
    for (int i = 0; i < tl->count && n < max; i++) {
        Toplevel *t = tl->items[i];
        if (t->seen_done && !t->is_self) out[n++] = t;
    }
    return n;
}

Toplevel *toplevels_self(const ToplevelList *tl) {
    for (int i = 0; i < tl->count; i++) {
        if (tl->items[i]->is_self) return tl->items[i];
    }
    return NULL;
}

Toplevel *toplevels_find_for_app(const ToplevelList *tl, int app_index, const char *app_id) {
    Toplevel *by_id = NULL;
    for (int i = tl->count - 1; i >= 0; i--) {
        Toplevel *t = tl->items[i];
        if (!t->seen_done || t->is_self) continue;
        if (t->app_index == app_index) return t;
        if (!by_id && app_id && app_id[0] && strcmp(t->app_id, app_id) == 0) by_id = t;
    }
    return by_id;
}

Toplevel *toplevels_active_app(const ToplevelList *tl) {
    for (int i = 0; i < tl->count; i++) {
        Toplevel *t = tl->items[i];
        if (t->seen_done && !t->is_self && t->activated) return t;
    }
    return NULL;
}

void toplevels_expect_window(ToplevelList *tl, int app_index, uint32_t now_ms) {
    tl->pending_app_index = app_index;
    tl->pending_since_ms = now_ms;
}

void toplevels_activate(Toplevel *t, struct wl_seat *seat) {
    if (!t || !seat) return;
    if (t->minimized) zwlr_foreign_toplevel_handle_v1_unset_minimized(t->handle);
    zwlr_foreign_toplevel_handle_v1_activate(t->handle, seat);
}

void toplevels_close(Toplevel *t) {
    if (t) zwlr_foreign_toplevel_handle_v1_close(t->handle);
}

void toplevels_cycle(ToplevelList *tl, struct wl_seat *seat, int dir) {
    Toplevel *apps[MAX_TOPLEVELS];
    int n = toplevels_apps(tl, apps, MAX_TOPLEVELS);
    if (n == 0) return;
    int cur = -1;
    for (int i = 0; i < n; i++) if (apps[i]->activated) cur = i;
    int next = (cur < 0) ? (dir > 0 ? 0 : n - 1) : (cur + dir + n) % n;
    toplevels_activate(apps[next], seat);
}
