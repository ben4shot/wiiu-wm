#ifndef TOPLEVELS_H
#define TOPLEVELS_H

// Ventanas de las demás aplicaciones vía wlr-foreign-toplevel-management (labwc, sway, wayfire).
// Sin este protocolo el shell no puede volver a mostrar una app ya abierta ni traerse al frente.

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

#define MAX_TOPLEVELS 32

struct zwlr_foreign_toplevel_manager_v1;
struct zwlr_foreign_toplevel_handle_v1;

struct ToplevelList;

typedef struct Toplevel {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    struct ToplevelList *list;
    char title[128];
    char app_id[128];
    bool activated, minimized, maximized, fullscreen;
    bool seen_done;         // ya recibió su primer «done»
    bool is_self;           // la ventana del propio shell
    int app_index;          // entrada de config.json que la lanzó (-1 si no se sabe)
    uint32_t serial;        // orden de aparición
} Toplevel;

typedef struct ToplevelList {
    struct zwlr_foreign_toplevel_manager_v1 *manager;
    Toplevel *items[MAX_TOPLEVELS];
    int count;
    uint32_t next_serial;
    bool changed;           // hay que redibujar Recientes

    const char *self_app_id;
    bool maximize_new;

    // Asociación de la próxima ventana nueva con la app lanzada desde el shell
    int pending_app_index;
    uint32_t pending_since_ms;
} ToplevelList;

void toplevels_init(ToplevelList *tl, const char *self_app_id, bool maximize_new);
void toplevels_bind(ToplevelList *tl, struct wl_registry *registry, uint32_t name, uint32_t version);
void toplevels_destroy(ToplevelList *tl);
bool toplevels_available(const ToplevelList *tl);

// Ventanas de aplicaciones (sin la del shell), de la más antigua a la más reciente
int toplevels_apps(const ToplevelList *tl, Toplevel **out, int max);
Toplevel *toplevels_self(const ToplevelList *tl);
Toplevel *toplevels_find_for_app(const ToplevelList *tl, int app_index, const char *app_id);
Toplevel *toplevels_active_app(const ToplevelList *tl);

void toplevels_expect_window(ToplevelList *tl, int app_index, uint32_t now_ms);
void toplevels_activate(Toplevel *t, struct wl_seat *seat);
void toplevels_close(Toplevel *t);
// Pasa a la siguiente/anterior ventana de aplicación (dir = +1 / -1)
void toplevels_cycle(ToplevelList *tl, struct wl_seat *seat, int dir);

#endif // TOPLEVELS_H
