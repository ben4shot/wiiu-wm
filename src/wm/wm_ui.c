#include "wm.h"
#include "wm_sound.h"
#include "../draw_shapes.h"
#include "../icons.h"
#include "../images.h"

#include <drm_fourcc.h>
#include <libgen.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/util/log.h>

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* ------------------------------------------- superficie Cairo -> wlr_buffer */

struct cairo_buffer {
    struct wlr_buffer base;
    cairo_surface_t *surface;
};

static void cb_destroy(struct wlr_buffer *b) {
    struct cairo_buffer *cb = wl_container_of(b, cb, base);
    cairo_surface_destroy(cb->surface);
    free(cb);
}

static bool cb_begin(struct wlr_buffer *b, uint32_t flags, void **data, uint32_t *format, size_t *stride) {
    (void)flags;
    struct cairo_buffer *cb = wl_container_of(b, cb, base);
    *data = cairo_image_surface_get_data(cb->surface);
    *format = cairo_image_surface_get_format(cb->surface) == CAIRO_FORMAT_RGB24 ? DRM_FORMAT_XRGB8888
                                                                                 : DRM_FORMAT_ARGB8888;
    *stride = cairo_image_surface_get_stride(cb->surface);
    return true;
}

static void cb_end(struct wlr_buffer *b) { (void)b; }

static const struct wlr_buffer_impl cairo_buffer_impl = {
    .destroy = cb_destroy,
    .begin_data_ptr_access = cb_begin,
    .end_data_ptr_access = cb_end,
};

// Pasa la superficie (que queda en propiedad del búfer) al nodo de escena
static void set_surface(struct wlr_scene_buffer *node, cairo_surface_t *surf) {
    cairo_surface_flush(surf);
    struct cairo_buffer *cb = calloc(1, sizeof(*cb));
    cb->surface = surf;
    wlr_buffer_init(&cb->base, &cairo_buffer_impl, cairo_image_surface_get_width(surf),
                    cairo_image_surface_get_height(surf));
    wlr_scene_buffer_set_buffer(node, &cb->base);
    wlr_buffer_drop(&cb->base);
}

/* --------------------------------------------------------- lanzar apps */

void wm_spawn(struct wm_server *s, const char *cmd) {
    AppEntry tmp;
    memset(&tmp, 0, sizeof(tmp));
    snprintf(tmp.name, sizeof(tmp.name), "%.60s", cmd);
    snprintf(tmp.exec, sizeof(tmp.exec), "%s", cmd);
    app_manager_launch(&s->apps, &tmp);
}

static struct wm_view *view_for_app(struct wm_server *s, const AppEntry *a) {
    struct wm_view *v;
    wl_list_for_each(v, &s->views, link) {
        if (v->mapped && a->app_id[0] && strcmp(wm_view_app_id(v), a->app_id) == 0) return v;
    }
    return NULL;
}

static void load_config(struct wm_server *s);
static void open_folder(struct wm_server *s, const char *name);

static void launch_app(struct wm_server *s, int idx) {
    if (idx < 0 || idx >= s->cfg.num_apps) return;
    const AppEntry *a = &s->cfg.apps[idx];
    // «folder:<nombre>» abre una carpeta (p. ej. los juegos de una consola); «folder:» vuelve al Inicio
    if (strncmp(a->exec, "folder:", 7) == 0) {
        wm_sound_play(a->exec[7] ? WM_SFX_PAGE : WM_SFX_BACK);
        open_folder(s, a->exec + 7);
        return;
    }
    struct wm_view *v = a->single_instance ? view_for_app(s, a) : NULL;
    if (v) {
        wm_sound_play(WM_SFX_SELECT);
        if (v->workspace != s->workspace) wm_switch_workspace(s, v->workspace);
        wm_ui_show(s, UI_HIDDEN);
        wm_focus_view(s, v);
        return;
    }
    // Siempre una instancia nueva, en el escritorio actual; al aparecer su ventana se oculta el Inicio
    if (app_manager_launch(&s->apps, a)) {
        wm_sound_play(WM_SFX_SELECT);
        char msg[96];
        snprintf(msg, sizeof(msg), "Abriendo «%.60s»…", a->name);
        wm_ui_osd(s, msg);
    }
}

/* ------------------------------------------------------- Recientes */

static const AppEntry *app_for_view(struct wm_server *s, struct wm_view *v) {
    for (int i = 0; i < s->apps.count; i++) {
        if (s->apps.running[i].pid == v->pid) {
            for (int k = 0; k < s->cfg.num_apps; k++)
                if (strcmp(s->cfg.apps[k].exec, s->apps.running[i].exec) == 0) return &s->cfg.apps[k];
        }
    }
    for (int k = 0; k < s->cfg.num_apps; k++)
        if (s->cfg.apps[k].app_id[0] && strcmp(s->cfg.apps[k].app_id, wm_view_app_id(v)) == 0) return &s->cfg.apps[k];
    return NULL;
}

static void refresh_recents(struct wm_server *s, bool focus_active) {
    RecentItem items[MAX_RECENT_ITEMS];
    memset(items, 0, sizeof(items));
    struct wm_view *list[MAX_RECENT_ITEMS];
    int n = 0;
    struct wm_view *v;
    wl_list_for_each(v, &s->views, link) if (v->mapped && n < MAX_RECENT_ITEMS) list[n++] = v;
    // De la usada hace más tiempo a la más reciente (la última queda a la derecha)
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (list[j]->focus_seq < list[i]->focus_seq) { struct wm_view *t = list[i]; list[i] = list[j]; list[j] = t; }
    for (int i = 0; i < n; i++) {
        v = list[i];
        const AppEntry *a = app_for_view(s, v);
        RecentItem *it = &items[i];
        snprintf(it->name, sizeof(it->name), "%s", a ? a->name : (wm_view_app_id(v)[0] ? wm_view_app_id(v) : wm_view_title(v)));
        snprintf(it->subtitle, sizeof(it->subtitle), "%s", wm_view_title(v));
        snprintf(it->icon, sizeof(it->icon), "%s", a ? a->icon : "");
        snprintf(it->exec, sizeof(it->exec), "%s", a ? a->exec : wm_view_app_id(v));
        it->active = v == s->focused;
        it->tiled = v->tiled;
        it->workspace = v->workspace + 1;
        s->recent_views[i] = v;
    }
    recents_view_set_items(&s->recents, items, n, focus_active);
}

static struct wm_view *recent_view(struct wm_server *s, int card) {
    if (card < 0 || card >= s->recents.count) return NULL;
    struct wm_view *target = s->recent_views[card], *v;
    wl_list_for_each(v, &s->views, link) if (v == target && v->mapped) return v;   // sigue viva
    return NULL;
}

static void open_recent(struct wm_server *s, int card) {
    struct wm_view *v = recent_view(s, card);
    if (!v) return;
    wm_sound_play(WM_SFX_SELECT);
    if (v->workspace != s->workspace) wm_switch_workspace(s, v->workspace);
    wm_ui_show(s, UI_HIDDEN);
    wm_focus_view(s, v);
}

/* ------------------------------------------------- cajón de aplicaciones */

#define DRAWER_COLS 6
#define DRAWER_HEADER 40.0
#define DRAWER_CELL_H 104.0
#define STICK_DEAD 9000

static void drawer_build(struct wm_server *s) {
    DesktopEntry *ents = calloc(160, sizeof(DesktopEntry));
    int n = ents ? desktop_entries_list(ents, 160) : 0;
    free(s->drawer);
    s->drawer = calloc(n + s->cfg.num_apps + 1, sizeof(struct wm_drawer_item));
    s->drawer_count = 0;
    if (!s->drawer) { free(ents); return; }
    // Primero los del menú (con su icono y su forma de abrir), luego todo lo instalado
    for (int i = 0; i < s->cfg.num_apps; i++) {
        struct wm_drawer_item *d = &s->drawer[s->drawer_count++];
        snprintf(d->name, sizeof(d->name), "%s", s->cfg.apps[i].name);
        snprintf(d->exec, sizeof(d->exec), "%s", s->cfg.apps[i].exec);
        snprintf(d->icon, sizeof(d->icon), "%s", s->cfg.apps[i].icon);
        d->app_index = i;
    }
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int k = 0; k < s->cfg.num_apps && !dup; k++) {
            char a[64], b[64];
            sscanf(s->cfg.apps[k].exec, "%63s", a);
            sscanf(ents[i].exec, "%63s", b);
            dup = strcmp(a, b) == 0;
        }
        if (dup) continue;
        struct wm_drawer_item *d = &s->drawer[s->drawer_count++];
        snprintf(d->name, sizeof(d->name), "%s", ents[i].name);
        snprintf(d->exec, sizeof(d->exec), "%s", ents[i].exec);
        snprintf(d->icon, sizeof(d->icon), "%s", ents[i].icon);
        d->app_index = -1;
        d->terminal = ents[i].terminal;
    }
    free(ents);
    if (s->drawer_focus >= s->drawer_count) s->drawer_focus = 0;
}

// Si la barra es una franja arriba, el cajón empieza debajo de ella
static double drawer_inset(struct wm_server *s) {
    return wm_usable_area(s).y;
}

static double drawer_content_h(struct wm_server *s) {
    int rows = (s->drawer_count + DRAWER_COLS - 1) / DRAWER_COLS;
    return drawer_inset(s) + DRAWER_HEADER + rows * DRAWER_CELL_H + 16;
}

static double home_max_scroll(struct wm_server *s) {
    double extra = drawer_content_h(s) - s->height;
    return s->height + (extra > 0 ? extra : 0);
}

static bool in_drawer(struct wm_server *s) { return s->home_scroll >= s->height * 0.5; }

static void drawer_cell(struct wm_server *s, int i, double *x, double *y, double *w, double *h) {
    double cw = (s->width - 24.0) / DRAWER_COLS;
    *w = cw;
    *h = DRAWER_CELL_H;
    *x = 12 + (i % DRAWER_COLS) * cw;
    *y = drawer_inset(s) + DRAWER_HEADER + (i / DRAWER_COLS) * DRAWER_CELL_H;   // relativo al inicio del cajón
}

static void drawer_launch(struct wm_server *s, int i) {
    if (i < 0 || i >= s->drawer_count) return;
    struct wm_drawer_item *d = &s->drawer[i];
    if (d->app_index >= 0) { launch_app(s, d->app_index); return; }
    char cmd[MAX_STR + 16];
    if (d->terminal) snprintf(cmd, sizeof(cmd), "foot %s", d->exec);
    else snprintf(cmd, sizeof(cmd), "%s", d->exec);
    wm_sound_play(WM_SFX_SELECT);
    wm_spawn(s, cmd);
    char msg[160];
    snprintf(msg, sizeof(msg), "Abriendo «%.60s»…", d->name);
    wm_ui_osd(s, msg);
}

// Desplaza para que la celda enfocada quede a la vista
static void drawer_reveal(struct wm_server *s) {
    double x, y, w, h;
    drawer_cell(s, s->drawer_focus, &x, &y, &w, &h);
    double top = s->home_scroll - s->height;        // parte del cajón que ya no se ve por arriba
    double target = s->home_scroll;
    if (y - 8 < top + drawer_inset(s)) target = s->height + y - 8 - DRAWER_HEADER - drawer_inset(s);
    if (y + h + 8 > top + s->height) target = s->height + y + h + 8 - s->height;
    if (target < s->height) target = s->height;
    if (target > home_max_scroll(s)) target = home_max_scroll(s);
    s->home_target = target;
}

static void draw_drawer(struct wm_server *s, cairo_t *cr) {
    struct wlr_box u = wm_usable_area(s);
    double y0 = s->height - s->home_scroll;   // borde superior del cajón en pantalla
    if (y0 >= s->height) {
        // Pestaña al pie de la página para abrir el cajón con el dedo (dentro del área utilizable)
        const char *t = "▼  Aplicaciones";
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12);
        cairo_text_extents_t e;
        cairo_text_extents(cr, t, &e);
        double w = e.width + 28, x = u.x + (u.width - w) / 2.0, y = u.y + u.height - 24.0;
        shape_path_rounded_rect(cr, x, y, w, 22, 11);
        cairo_set_source_rgba(cr, 0.05, 0.03, 0.08, 0.85);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 0.96, 0.84);
        cairo_move_to(cr, x + 14, y + 15);
        cairo_show_text(cr, t);
        return;
    }
    cairo_save(cr);
    cairo_rectangle(cr, 0, y0, s->width, s->height - y0 + home_max_scroll(s));
    cairo_set_source_rgba(cr, 0.06, 0.04, 0.10, 0.97);
    cairo_fill(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 15);
    double hy = y0 + drawer_inset(s);
    cairo_set_source_rgb(cr, 1, 0.84, 0.04);
    cairo_move_to(cr, 18, hy + 26);
    cairo_show_text(cr, "▲  Aplicaciones");
    char n[32];
    snprintf(n, sizeof(n), "%d", s->drawer_count);
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgba(cr, 0.85, 0.78, 1.0, 0.8);
    cairo_move_to(cr, s->width - 40, hy + 26);
    cairo_show_text(cr, n);

    for (int i = 0; i < s->drawer_count; i++) {
        double x, y, w, h;
        drawer_cell(s, i, &x, &y, &w, &h);
        y += y0;
        if (y + h < 0 || y > s->height) continue;
        struct wm_drawer_item *d = &s->drawer[i];
        if (i == s->drawer_focus && in_drawer(s)) {
            shape_path_rounded_rect(cr, x + 4, y + 2, w - 8, h - 6, 14);
            cairo_set_source_rgba(cr, s->cfg.tile_glow.r, s->cfg.tile_glow.g, s->cfg.tile_glow.b, 0.25);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, s->cfg.tile_glow.r, s->cfg.tile_glow.g, s->cfg.tile_glow.b, 0.95);
            cairo_set_line_width(cr, 2.5);
            cairo_stroke(cr);
        }
        double cx = x + w / 2, cy = y + 36;
        ColorRGBA fg = { 1, 0.96, 0.84, 1 };
        if (!icon_draw_for_app(cr, d->icon, d->exec, cx, cy, 52, fg.r, fg.g, fg.b))
            draw_icon_placeholder(cr, cx, cy, 44, d->icon, &fg);
        // Nombre (recortado a la celda)
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.5);
        char label[64];
        snprintf(label, sizeof(label), "%s", d->name);
        cairo_text_extents_t e;
        cairo_text_extents(cr, label, &e);
        while (e.width > w - 12 && strlen(label) > 4) {
            size_t l = strlen(label);
            label[l - 4] = 0;
            strcat(label, "…");
            cairo_text_extents(cr, label, &e);
        }
        cairo_set_source_rgb(cr, 0.95, 0.9, 1.0);
        cairo_move_to(cr, cx - e.width / 2 - e.x_bearing, y + 84);
        cairo_show_text(cr, label);
    }
    cairo_restore(cr);
}

/* ------------------------------------------------------------- dibujo */

static void schedule_anim(struct wm_server *s) {
    wl_event_source_timer_update(s->anim_timer, 16);
}

static void draw_topbar(struct wm_server *s, cairo_t *cr) {
    if (s->cfg.bar_enabled) return;   // la barra ya muestra el escritorio
    char ws[64];
    snprintf(ws, sizeof(ws), "Escritorio %d/%d", s->workspace + 1, s->cfg.workspaces);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12);
    cairo_text_extents_t e;
    cairo_text_extents(cr, ws, &e);
    double x = s->width - e.width - 22, y = 8;
    shape_path_rounded_rect(cr, x - 10, y, e.width + 20, 22, 11);
    cairo_set_source_rgba(cr, 0.05, 0.03, 0.08, 0.75);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 1, 0.84, 0.04);
    cairo_move_to(cr, x, y + 15);
    cairo_show_text(cr, ws);
}

long wm_stat_ui_redraws;   // diagnóstico: redibujados de la interfaz (se registran cada 5 s en wm_capture)

void wm_ui_redraw(struct wm_server *s) {
    wm_stat_ui_redraws++;
    if (s->ui == UI_HIDDEN) {
        wlr_scene_node_set_enabled(&s->ui_tree->node, false);
        return;
    }
    wlr_scene_node_set_enabled(&s->ui_tree->node, true);
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, s->width, s->height);
    cairo_t *cr = cairo_create(surf);

    if (s->ui == UI_HOME) {
        // La página sube con el desplazamiento y debajo aparece el cajón de aplicaciones
        if (s->home_scroll < s->height) {
            cairo_save(cr);
            cairo_translate(cr, 0, -s->home_scroll);
            int keep = s->grid.focused_index;
            if (in_drawer(s)) s->grid.focused_index = -1;   // el foco está en el cajón
            grid_view_render(&s->grid, cr, s->width, s->height);
            s->grid.focused_index = keep;
            if (s->home_scroll < 1) draw_topbar(s, cr);
            cairo_restore(cr);
        }
        draw_drawer(s, cr);

        // Si la barra está activa, reservar su espacio con fondo limpio propio (estilo Hyprland)
        if (s->cfg.bar_enabled && s->bar_layer) {
            cairo_save(cr);
            cairo_rectangle(cr, s->cfg.bar_x, s->cfg.bar_y, s->cfg.bar_w, s->cfg.bar_h);
            cairo_set_source_rgb(cr, 0.047, 0.035, 0.078);
            cairo_fill(cr);
            cairo_restore(cr);
        }
    } else {
        // Recientes por encima de lo que haya detrás (oscurecido por la propia vista)
        recents_view_render(&s->recents, cr, s->width, s->height);
    }
    cairo_destroy(cr);
    set_surface(s->ui_buffer, surf);
}

static void draw_osd(struct wm_server *s) {
    if (!s->osd[0] || now_ms() >= s->osd_until) {
        wlr_scene_node_set_enabled(&s->osd_buffer->node, false);
        return;
    }
    cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *pc = cairo_create(probe);
    cairo_select_font_face(pc, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(pc, 15);
    cairo_text_extents_t e;
    cairo_text_extents(pc, s->osd, &e);
    cairo_destroy(pc);
    cairo_surface_destroy(probe);

    int w = (int)e.width + 44, h = 40;
    if (w > s->width - 20) w = s->width - 20;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);
    shape_path_rounded_rect(cr, 1, 1, w - 2, h - 2, 18);
    cairo_set_source_rgba(cr, 0.06, 0.04, 0.10, 0.92);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, s->cfg.tile_glow.r, s->cfg.tile_glow.g, s->cfg.tile_glow.b, 0.95);
    cairo_set_line_width(cr, 2);
    cairo_stroke(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 15);
    cairo_set_source_rgb(cr, 1, 0.96, 0.84);
    cairo_move_to(cr, 22, h / 2.0 + 5);
    cairo_show_text(cr, s->osd);
    cairo_destroy(cr);
    set_surface(s->osd_buffer, surf);
    wlr_scene_node_set_position(&s->osd_buffer->node, (s->width - w) / 2, 14);
    wlr_scene_node_set_enabled(&s->osd_buffer->node, true);
}

void wm_ui_osd(struct wm_server *s, const char *msg) {
    snprintf(s->osd, sizeof(s->osd), "%s", msg);
    s->osd_until = now_ms() + 1400;
    draw_osd(s);
    if (s->ui == UI_HOME) wm_ui_redraw(s);   // el indicador de escritorio
    schedule_anim(s);
}

void wm_ui_stick(struct wm_server *s) {
    if (s->ui != UI_HOME) return;
    if (!s->drawer) drawer_build(s);
    schedule_anim(s);
}

// Paso de desplazamiento del Inicio: el stick manda; sin stick se ajusta a página o cajón
static bool home_scroll_step(struct wm_server *s) {
    int ry = s->gamepad.is_open ? s->gamepad.abs_ry : 0;
    double max = home_max_scroll(s);
    if (abs(ry) > STICK_DEAD) {
        s->home_target = -1;
        double before = s->home_scroll;
        s->home_scroll += (ry / 32767.0) * 36.0;
        if (s->home_scroll < 0) s->home_scroll = 0;
        if (s->home_scroll > max) s->home_scroll = max;
        // Solo hay que redibujar si se movió: con un stick con deriva (o al tope) devolver true
        // redibujaba el Inicio a 60 Hz sin cambios
        return s->home_scroll != before;
    }
    if (s->home_target < 0 && s->home_scroll > 0 && s->home_scroll < s->height)
        s->home_target = s->home_scroll > s->height * 0.35 ? s->height : 0;
    if (s->home_target >= 0) {
        double d = s->home_target - s->home_scroll;
        if (fabs(d) < 1.0) { s->home_scroll = s->home_target; s->home_target = -1; return true; }
        s->home_scroll += d * 0.35;
        return true;
    }
    return false;
}

static int anim_tick(void *data) {
    struct wm_server *s = data;
    bool again = false;
    if (s->ui == UI_HOME && home_scroll_step(s)) again = true;
    if (s->ui == UI_HOME && grid_view_update(&s->grid, s->width)) again = true;
    if (s->ui == UI_RECENTS && recents_view_update(&s->recents, s->width)) again = true;
    if (again) wm_ui_redraw(s);
    if (s->osd[0]) {
        if (now_ms() >= s->osd_until) {
            s->osd[0] = '\0';
            draw_osd(s);
        } else {
            again = true;
        }
    }
    if (again) wl_event_source_timer_update(s->anim_timer, s->osd[0] && !again ? 100 : 16);
    return 0;
}

void wm_ui_update_wallpaper(struct wm_server *s) {
    // Fondo bajo las ventanas (se ve en un escritorio vacío o entre ventanas en mosaico)
    cairo_surface_t *src = s->page_cache.wallpaper;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, s->width, s->height);
    cairo_t *cr = cairo_create(surf);
    if (src) {
        cairo_set_source_surface(cr, src, 0, 0);
        cairo_paint(cr);
    } else {
        image_draw_wallpaper(cr, s->cfg.wallpaper, s->width, s->height, &s->cfg.bg_color, &s->cfg.bg_gradient, &s->cfg.accent);
    }
    cairo_destroy(cr);
    set_surface(s->wallpaper, surf);
}

/* ------------------------------------------------------- estado de la IU */

void wm_ui_show(struct wm_server *s, enum wm_ui_mode mode) {
    enum wm_ui_mode prev = s->ui;
    s->ui = mode;
    if (mode == UI_RECENTS) refresh_recents(s, prev != UI_RECENTS);
    if (mode == UI_HOME && prev != UI_HOME) {
        grid_view_reload(&s->grid);
        s->home_scroll = 0;
        s->home_target = -1;
        drawer_build(s);
    }
    if (mode == UI_HIDDEN) {
        // Devolver el teclado a la app
        if (s->focused) {
            struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
            if (kb) wlr_seat_keyboard_notify_enter(s->seat, s->focused->xdg->base->surface,
                                                   kb->keycodes, kb->num_keycodes, &kb->modifiers);
        }
    } else {
        wlr_seat_keyboard_notify_clear_focus(s->seat);
        wlr_seat_pointer_clear_focus(s->seat);
    }
    wm_set_gamepad_grab(s, s->home_held || s->ui != UI_HIDDEN);
    wm_ui_redraw(s);
    schedule_anim(s);
}

// HOME: app -> Recientes -> Inicio -> (de vuelta a la app)
void wm_ui_home_button(struct wm_server *s) {
    int n = wm_view_count(s, -1);
    switch (s->ui) {
        case UI_HIDDEN: wm_sound_play(WM_SFX_HOME); wm_ui_show(s, n > 0 ? UI_RECENTS : UI_HOME); break;
        case UI_RECENTS: wm_sound_play(WM_SFX_HOME); wm_ui_show(s, UI_HOME); break;
        case UI_HOME: if (n > 0) { wm_sound_play(WM_SFX_BACK); wm_ui_show(s, UI_HIDDEN); } break;
    }
}

void wm_ui_views_changed(struct wm_server *s) {
    if (s->ui == UI_RECENTS) {
        refresh_recents(s, false);
        if (s->recents.count == 0) { wm_ui_show(s, UI_HOME); return; }
        wm_ui_redraw(s);
    }
}

void wm_ui_nav(struct wm_server *s, NavAction act) {
    if (s->ui == UI_HOME && in_drawer(s) && s->drawer_count > 0) {
        int f = s->drawer_focus;
        bool moved_out = false;
        switch (act) {
            case NAV_LEFT: if (f > 0) f--; break;
            case NAV_RIGHT: if (f < s->drawer_count - 1) f++; break;
            case NAV_DOWN: if (f + DRAWER_COLS < s->drawer_count) f += DRAWER_COLS; break;
            case NAV_UP:
                if (f >= DRAWER_COLS) f -= DRAWER_COLS;
                else { s->home_target = 0; moved_out = true; }  // de la primera fila, de vuelta a la página
                break;
            case NAV_ACTION_A: drawer_launch(s, f); break;
            case NAV_ACTION_B: s->home_target = 0; moved_out = true; break;
            default: break;
        }
        if (f != s->drawer_focus) { s->drawer_focus = f; drawer_reveal(s); wm_sound_play(WM_SFX_TICK); }
        else if (moved_out) wm_sound_play(WM_SFX_BACK);
        else if (act == NAV_LEFT || act == NAV_RIGHT || act == NAV_UP || act == NAV_DOWN) wm_sound_play(WM_SFX_EDGE);
        wm_ui_redraw(s);
        schedule_anim(s);
        return;
    }
    if (s->ui == UI_HOME) {
        int old_focus = s->grid.focused_index;
        int old_page = s->grid.current_page;
        switch (act) {
            case NAV_UP:
                grid_view_nav(&s->grid, 0, -1);
                wm_sound_play(s->grid.focused_index != old_focus ? WM_SFX_TICK : WM_SFX_EDGE);
                break;
            case NAV_DOWN: {
                grid_view_nav(&s->grid, 0, 1);
                // Si la cruceta hacia abajo ya no puede avanzar a otra baldosa, bajar al cajón
                if (s->grid.focused_index == old_focus) {
                    if (!s->drawer) drawer_build(s);
                    s->home_target = s->height;
                    s->drawer_focus = 0;
                    drawer_reveal(s);
                    wm_sound_play(WM_SFX_PAGE);
                } else {
                    wm_sound_play(WM_SFX_TICK);
                }
                break;
            }
            case NAV_LEFT:
                grid_view_nav(&s->grid, -1, 0);
                wm_sound_play(s->grid.focused_index != old_focus ? WM_SFX_TICK : WM_SFX_EDGE);
                break;
            case NAV_RIGHT:
                grid_view_nav(&s->grid, 1, 0);
                wm_sound_play(s->grid.focused_index != old_focus ? WM_SFX_TICK : WM_SFX_EDGE);
                break;
            case NAV_PAGE_PREV:
                grid_view_prev_page(&s->grid);
                wm_sound_play(s->grid.current_page != old_page ? WM_SFX_PAGE : WM_SFX_EDGE);
                break;
            case NAV_PAGE_NEXT:
                grid_view_next_page(&s->grid);
                wm_sound_play(s->grid.current_page != old_page ? WM_SFX_PAGE : WM_SFX_EDGE);
                break;
            case NAV_ACTION_A: launch_app(s, s->grid.focused_index); break;
            case NAV_ACTION_B:
                if (s->folder[0]) { wm_sound_play(WM_SFX_BACK); open_folder(s, ""); return; }
                if (wm_view_count(s, -1) > 0) { wm_sound_play(WM_SFX_BACK); wm_ui_show(s, UI_HIDDEN); }
                return;
            default: break;
        }
    } else if (s->ui == UI_RECENTS) {
        int card = s->recents.focused_card;
        switch (act) {
            case NAV_LEFT:
                recents_view_nav(&s->recents, -1);
                wm_sound_play(s->recents.focused_card != card ? WM_SFX_TICK : WM_SFX_EDGE);
                break;
            case NAV_RIGHT:
                recents_view_nav(&s->recents, 1);
                wm_sound_play(s->recents.focused_card != card ? WM_SFX_TICK : WM_SFX_EDGE);
                break;
            case NAV_ACTION_A:
                if (s->recents.count == 0) { wm_sound_play(WM_SFX_HOME); wm_ui_show(s, UI_HOME); return; }
                open_recent(s, card);
                return;
            case NAV_ACTION_B: wm_sound_play(WM_SFX_BACK); wm_ui_show(s, UI_HIDDEN); return;
            case NAV_ACTION_X: {
                struct wm_view *v = recent_view(s, card);
                if (v) { wm_sound_play(WM_SFX_CLOSE); wm_view_close(v); }
                break;
            }
            case NAV_ACTION_Y: wm_sound_play(WM_SFX_TICK); wm_toggle_tile(s, recent_view(s, card)); break;
            default: break;
        }
    }
    wm_ui_redraw(s);
    schedule_anim(s);
}

/* ------------------------------------------------------------- toques */

static double tdx, tdy;
static bool tdown;

void wm_ui_touch_down(struct wm_server *s, double x, double y) {
    tdx = x;
    tdy = y;
    tdown = true;
    if (s->ui == UI_HOME) {
        s->grid.pressed_index = grid_view_hit_test(&s->grid, x, y, s->width, s->height);
        if (s->grid.pressed_index >= 0) s->grid.focused_index = s->grid.pressed_index;
        wm_ui_redraw(s);
    }
}

void wm_ui_touch_motion(struct wm_server *s, double x, double y) {
    (void)s; (void)x; (void)y;
}

void wm_ui_touch_up(struct wm_server *s, double x, double y) {
    if (!tdown) return;
    tdown = false;
    bool tap = hypot(x - tdx, y - tdy) < 18.0;
    if (s->ui == UI_HOME && tap) {
        double y0 = s->height - s->home_scroll;
        struct wlr_box u = wm_usable_area(s);
        double tab_w = 180.0;
        double tab_x = u.x + (u.width - tab_w) / 2.0;
        double tab_y = u.y + u.height - 24.0;
        if (y0 >= s->height && y >= tab_y - 12.0 && y <= tab_y + 30.0 && x >= tab_x && x <= tab_x + tab_w) {
            if (!s->drawer) drawer_build(s);
            s->home_target = s->height;                   // pestaña «Aplicaciones»
            s->grid.pressed_index = -1;
            wm_sound_play(WM_SFX_PAGE);
            schedule_anim(s);
            return;
        }
        if (y0 < s->height && y >= y0) {
            s->grid.pressed_index = -1;
            if (y < y0 + drawer_inset(s) + DRAWER_HEADER) {
                s->home_target = 0;
                wm_sound_play(WM_SFX_BACK);
                schedule_anim(s);
                return;
            }
            for (int i = 0; i < s->drawer_count; i++) {
                double cx, cy, cw, ch;
                drawer_cell(s, i, &cx, &cy, &cw, &ch);
                if (x >= cx && x < cx + cw && y >= y0 + cy && y < y0 + cy + ch) {
                    s->drawer_focus = i;
                    drawer_launch(s, i);
                    break;
                }
            }
            wm_ui_redraw(s);
            return;
        }
    }
    if (s->ui == UI_HOME) {
        int hit = s->grid.pressed_index;
        s->grid.pressed_index = -1;
        if (tap && hit >= 0 && hit == grid_view_hit_test(&s->grid, x, y, s->width, s->height)) launch_app(s, hit);
        wm_ui_redraw(s);
        return;
    }
    if (s->ui == UI_RECENTS && tap) {
        bool close = false, tile = false;
        int card = recents_view_hit_test(&s->recents, x, y, s->width, s->height, &close, &tile);
        if (card == -2) { wm_sound_play(WM_SFX_HOME); wm_ui_show(s, UI_HOME); return; }
        if (card < 0) return;
        s->recents.focused_card = card;
        struct wm_view *v = recent_view(s, card);
        if (close) { if (v) { wm_sound_play(WM_SFX_CLOSE); wm_view_close(v); } }
        else if (tile) { wm_sound_play(WM_SFX_TICK); wm_toggle_tile(s, v); }
        else { open_recent(s, card); return; }
        wm_ui_redraw(s);
        schedule_anim(s);
    }
}

/* ------------------------------------------------ configuración en vivo */

/* --------------------------------------------------------------- barra */

void wm_write_state(struct wm_server *s) {
    if (!s->state_path[0]) {
        const char *rt = getenv("XDG_RUNTIME_DIR");
        if (!rt) return;
        snprintf(s->state_path, sizeof(s->state_path), "%.200s/wiiu-wm.state", rt);
    }
    char tmp[MAX_STR + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s->state_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "workspace=%d\nworkspaces=%d\nwindows=%d\nwindows_here=%d\ntiled=%d\ntitle=%s\n",
            s->workspace + 1, s->cfg.workspaces, wm_view_count(s, -1), wm_view_count(s, s->workspace),
            s->focused && s->focused->tiled ? 1 : 0, s->focused ? wm_view_title(s->focused) : "");
    fclose(f);
    rename(tmp, s->state_path);
    // waybar: módulo con "signal": 8 se refresca al momento
    if (s->bar_pid > 0) kill(s->bar_pid, SIGRTMIN + 8);
}

static char bar_running_cmd[MAX_STR];

void wm_bar_sync(struct wm_server *s) {
    bool want = s->cfg.bar_enabled && s->cfg.bar_command[0];
    pid_t layer_pid = wm_bar_client_pid(s);
    bool running = (s->bar_pid > 0 && kill(s->bar_pid, 0) == 0) ||
                   (layer_pid > 0 && kill(layer_pid, 0) == 0);

    if (running && (!want || strcmp(bar_running_cmd, s->cfg.bar_command) != 0)) {
        if (layer_pid > 0) kill(layer_pid, SIGTERM);
        if (s->bar_pid > 0) {
            kill(-s->bar_pid, SIGTERM);
            kill(s->bar_pid, SIGTERM);
        }
        s->bar_pid = 0;
        running = false;
    }
    if (want && !running) {
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            signal(SIGCHLD, SIG_DFL);
            execl("/bin/sh", "sh", "-c", s->cfg.bar_command, (char *)NULL);
            _exit(127);
        }
        if (pid > 0) {
            s->bar_pid = pid;
            snprintf(bar_running_cmd, sizeof(bar_running_cmd), "%s", s->cfg.bar_command);
            wlr_log(WLR_INFO, "[wm] Barra: %s (PID %d)", s->cfg.bar_command, pid);
        }
    }
    wm_bar_reconfigure(s);
}

void wm_toggle_bar(struct wm_server *s) {
    // Solo para esta sesión: la configuración guardada no cambia
    s->cfg.bar_enabled = !s->cfg.bar_enabled;
    wm_bar_sync(s);
    wm_ui_osd(s, s->cfg.bar_enabled ? "Barra visible" : "Barra oculta");
    wm_ui_redraw(s);
}

// Carga incremental de las páginas del Inicio/carpetas: un elemento por paso y 2 ms de respiro entre pasos
// para que el compositor siga atendiendo toques, mando y fotogramas mientras se dibuja.
static int build_tick(void *data) {
    struct wm_server *s = data;
    bool changed = false;
    bool more = page_cache_step(&s->page_cache, s->grid.current_page, &changed);
    if (changed) wm_ui_redraw(s);
    if (more) wl_event_source_timer_update(s->build_timer, 2);
    return 0;
}

static void build_kick(struct wm_server *s) {
    if (page_cache_pending(&s->page_cache)) wl_event_source_timer_update(s->build_timer, 1);
}

static void load_config(struct wm_server *s) {
    s->folder[0] = '\0';
    config_resolve_path(s->config_fallback, s->config_path, sizeof(s->config_path));
    DesktopConfig *fresh = malloc(sizeof(DesktopConfig));
    if (!fresh) return;
    config_load(fresh, s->config_path);
    fresh->screen_width = s->width;
    fresh->screen_height = s->height;
    s->cfg = *fresh;
    free(fresh);
    icons_clear_cache();
    images_clear_cache();
    page_cache_begin(&s->page_cache, &s->cfg, s->config_path);
    build_kick(s);
    if (s->grid.cfg) grid_view_reload(&s->grid);
    wm_ui_update_wallpaper(s);
}

/* Carpetas: el script «wiiu-games <nombre>» escribe un config con los elementos de la carpeta
 * (juegos de una consola, etc.) en $XDG_RUNTIME_DIR/wiiu-folder-<nombre>.json; se muestra con el
 * mismo motor de viñetas. Con nombre vacío se vuelve al Inicio normal. */
static void open_folder(struct wm_server *s, const char *name_in) {
    char nm[48];
    snprintf(nm, sizeof(nm), "%s", name_in);   // name_in apunta dentro de s->cfg, que se reemplaza
    const char *name = nm;
    if (!name[0]) {
        load_config(s);
        s->grid.current_page = 0;
        s->grid.focused_index = 0;
        wm_ui_redraw(s);
        return;
    }
    // Solo letras, cifras, «_» y «-»: el nombre va a una orden de shell
    for (const char *c = name; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-')) return;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char path[MAX_STR], cmd[MAX_STR + 64];
    snprintf(path, sizeof(path), "%.180s/wiiu-folder-%.40s.json", rt ? rt : "/tmp", name);
    // Reutilizar la lista si es más nueva que la carpeta de ROMs y que la de carátulas
    // (generarla con python tarda ~0,2 s en la Pi)
    struct stat sj, sr, sc;
    const char *home = getenv("HOME");
    char rdir[MAX_STR], cdir[MAX_STR];
    snprintf(rdir, sizeof(rdir), "%.200s/Games/%.40s", home ? home : "", name);
    snprintf(cdir, sizeof(cdir), "%.200s/.config/wiiu-shell/covers/%.40s", home ? home : "", name);
    bool fresh_list = stat(path, &sj) == 0 &&
                      (stat(rdir, &sr) != 0 || sj.st_mtime > sr.st_mtime) &&
                      (stat(cdir, &sc) != 0 || sj.st_mtime > sc.st_mtime);
    snprintf(cmd, sizeof(cmd), "wiiu-games %.40s '%s' >/dev/null 2>&1", name, path);
    if ((!fresh_list && system(cmd) != 0) || access(path, R_OK) != 0) {
        wm_ui_osd(s, "No se pudo abrir la carpeta");
        return;
    }
    DesktopConfig *fresh = malloc(sizeof(DesktopConfig));
    if (!fresh) return;
    config_load(fresh, path);
    fresh->screen_width = s->width;
    fresh->screen_height = s->height;
    s->cfg = *fresh;
    free(fresh);
    page_cache_begin(&s->page_cache, &s->cfg, path);
    snprintf(s->folder, sizeof(s->folder), "%s", name);
    grid_view_reload(&s->grid);
    s->grid.current_page = 0;
    s->grid.focused_index = 0;
    s->home_scroll = 0;
    s->home_target = -1;
    wm_ui_update_wallpaper(s);
    wm_ui_redraw(s);   // fondo y viñetas vacías al instante; el resto se dibuja de a poco (build_tick)
    build_kick(s);
}

static int config_changed(int fd, uint32_t mask, void *data) {
    (void)mask;
    struct wm_server *s = data;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool changed = false;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (char *p = buf; p < buf + n;) {
            struct inotify_event *ev = (struct inotify_event *)p;
            if (ev->len && strcmp(ev->name, "config.json") == 0) changed = true;
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    if (changed) {
        load_config(s);
        wm_bar_sync(s);
        wm_write_state(s);
        wm_ui_redraw(s);
        wlr_log(WLR_INFO, "[wm] Configuración recargada (%s)", s->config_path);
    }
    return 0;
}

static int child_exited(int sig, void *data) {
    (void)sig;
    struct wm_server *s = data;
    app_manager_refresh(&s->apps);
    // Procesos que no son apps del menú (p. ej. el comando de arranque)
    pid_t dead;
    while ((dead = waitpid(-1, NULL, WNOHANG)) > 0) {
        if (dead == s->bar_pid) s->bar_pid = 0;
    }
    if (s->apps.error_seq != s->toast_seq) {
        s->toast_seq = s->apps.error_seq;
        wm_ui_osd(s, s->apps.last_error);
        s->osd_until = now_ms() + 3500;
    }
    return 0;
}

void wm_ui_init(struct wm_server *s) {
    page_cache_init(&s->page_cache);
    app_manager_init(&s->apps);
    config_user_path(s->config_user, sizeof(s->config_user));

    s->wallpaper = wlr_scene_buffer_create(s->layer_tree[0], NULL);
    s->ui_buffer = wlr_scene_buffer_create(s->ui_tree, NULL);
    s->osd_buffer = wlr_scene_buffer_create(s->top_tree, NULL);
    wlr_scene_node_set_enabled(&s->osd_buffer->node, false);
    wlr_scene_node_lower_to_bottom(&s->wallpaper->node);

    s->build_timer = wl_event_loop_add_timer(s->loop, build_tick, s);
    load_config(s);
    grid_view_init(&s->grid, &s->cfg);
    s->grid.cache = &s->page_cache;
    recents_view_init(&s->recents, &s->cfg);

    // Vigilar ~/.config/wiiu-shell (el editor guarda con rename)
    char dir[MAX_STR];
    snprintf(dir, sizeof(dir), "%s", s->config_user);
    char *d = dirname(dir);
    char tmp[MAX_STR];
    snprintf(tmp, sizeof(tmp), "%s", d);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
    s->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (s->inotify_fd >= 0 && inotify_add_watch(s->inotify_fd, d, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE) >= 0) {
        s->inotify_source = wl_event_loop_add_fd(s->loop, s->inotify_fd, WL_EVENT_READABLE, config_changed, s);
    }
    s->anim_timer = wl_event_loop_add_timer(s->loop, anim_tick, s);
    wl_event_loop_add_signal(s->loop, SIGCHLD, child_exited, s);

    wm_sound_init();
    s->home_target = -1;
    s->ui = UI_HOME;
    drawer_build(s);
    wm_ui_redraw(s);
}

void wm_ui_finish(struct wm_server *s) {
    wm_sound_finish();
    free(s->drawer);
    page_cache_free(&s->page_cache);
    page_cache_wait_writes();
    if (s->inotify_fd >= 0) close(s->inotify_fd);
}
