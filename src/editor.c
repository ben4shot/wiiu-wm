#include "editor.h"
#include "draw_shapes.h"
#include "grid_view.h"
#include "page_cache.h"
#include "poly.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------- filas */

typedef enum {
    // Elemento seleccionado
    ROW_PRESET, ROW_VERTEX, ROW_NVERT, ROW_X, ROW_Y, ROW_W, ROW_H, ROW_ROT,
    ROW_RADIUS, ROW_PADDING, ROW_BORDER_STYLE, ROW_BORDER_W,
    ROW_BG, ROW_BORDER_COLOR, ROW_TEXT_COLOR, ROW_FILL, ROW_IMAGE, ROW_SHADOW,
    ROW_ICON_SCALE, ROW_LABEL, ROW_FONT, ROW_PAGE,
    // Sin selección: página y fondo
    ROW_WALLPAPER, ROW_GUTTER, ROW_BG_BASE, ROW_BG_GRAD, ROW_GLOW, ROW_BAR,
    // Barra seleccionada
    ROW_BAR_X, ROW_BAR_Y, ROW_BAR_W, ROW_BAR_H
} RowId;

static const char *BORDER_LABELS[BORDER_COUNT] = { "Ninguno", "Sólido", "Guiones", "Puntos", "Doble", "Tinta" };
static const char *FILL_LABELS[FILL_COUNT] = { "Plano", "Degradado", "Brillo Wii U", "Trama cómic" };
static const char *SHADOW_LABELS[SHADOW_COUNT] = { "Ninguna", "Suave", "Dura" };
static const char *LABEL_LABELS[LABEL_COUNT] = { "Debajo", "Dentro", "Oculto" };

static const char *PALETTE[] = {
    "#fff4d6", "#ffffff", "#ffd60a", "#f77f00", "#e63946", "#ff4d8d", "#c084fc", "#7c3aed",
    "#4c1d95", "#221738", "#1a102c", "#0c0914", "#3a86ff", "#4cc9f0", "#2ec4b6", "#80ed99",
    "#6b6b7b", "#0b0712"
};
#define PALETTE_N ((int)(sizeof(PALETTE) / sizeof(PALETTE[0])))

static int build_rows(const Editor *ed, RowId *rows) {
    int n = 0;
    if (ed->bar_selected) {
        rows[n++] = ROW_BAR_X; rows[n++] = ROW_BAR_Y; rows[n++] = ROW_BAR_W; rows[n++] = ROW_BAR_H;
        rows[n++] = ROW_BAR;
        return n;
    }
    if (ed->selected < 0) {
        rows[n++] = ROW_WALLPAPER; rows[n++] = ROW_BAR; rows[n++] = ROW_GUTTER;
        rows[n++] = ROW_BG_BASE; rows[n++] = ROW_BG_GRAD; rows[n++] = ROW_GLOW;
        return n;
    }
    RowId all[] = { ROW_PRESET, ROW_VERTEX, ROW_NVERT, ROW_X, ROW_Y, ROW_W, ROW_H, ROW_ROT,
                    ROW_RADIUS, ROW_PADDING, ROW_BORDER_STYLE, ROW_BORDER_W,
                    ROW_BG, ROW_BORDER_COLOR, ROW_TEXT_COLOR, ROW_FILL, ROW_IMAGE, ROW_SHADOW,
                    ROW_ICON_SCALE, ROW_LABEL, ROW_FONT, ROW_PAGE };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) rows[n++] = all[i];
    return n;
}

static const char *row_label(RowId r) {
    switch (r) {
        case ROW_PRESET: return "Forma";
        case ROW_VERTEX: return "Vértice";
        case ROW_NVERT: return "Nº vértices";
        case ROW_X: return "Posición X";
        case ROW_Y: return "Posición Y";
        case ROW_W: return "Ancho";
        case ROW_H: return "Alto";
        case ROW_ROT: return "Girar";
        case ROW_RADIUS: return "Redondeado";
        case ROW_PADDING: return "Padding";
        case ROW_BORDER_STYLE: return "Borde";
        case ROW_BORDER_W: return "Grosor borde";
        case ROW_BG: return "Color fondo";
        case ROW_BORDER_COLOR: return "Color borde";
        case ROW_TEXT_COLOR: return "Color texto";
        case ROW_FILL: return "Relleno";
        case ROW_IMAGE: return "Imagen";
        case ROW_SHADOW: return "Sombra";
        case ROW_ICON_SCALE: return "Tamaño icono";
        case ROW_LABEL: return "Nombre";
        case ROW_FONT: return "Tamaño letra";
        case ROW_PAGE: return "Página";
        case ROW_WALLPAPER: return "Fondo pantalla";
        case ROW_GUTTER: return "Separación";
        case ROW_BG_BASE: return "Color fondo";
        case ROW_BG_GRAD: return "Color centro";
        case ROW_GLOW: return "Color foco";
        case ROW_BAR: return "Barra";
        case ROW_BAR_X: return "Posición X";
        case ROW_BAR_Y: return "Posición Y";
        case ROW_BAR_W: return "Ancho";
        case ROW_BAR_H: return "Alto";
    }
    return "";
}

static ColorRGBA *row_color(Editor *ed, RowId r) {
    DesktopConfig *cfg = ed->cfg;
    if (r == ROW_BG_BASE) return &cfg->bg_color;
    if (r == ROW_BG_GRAD) return &cfg->bg_gradient;
    if (r == ROW_GLOW) return &cfg->tile_glow;
    if (ed->selected < 0) return NULL;
    ElementStyle *st = &cfg->apps[ed->selected].style;
    if (r == ROW_BG) return &st->bg;
    if (r == ROW_BORDER_COLOR) return &st->border;
    if (r == ROW_TEXT_COLOR) return &st->text;
    return NULL;
}

// Preajuste que coincide con la forma actual (para mostrarlo), o -1 si es libre
static int current_preset(const AppEntry *a) {
    AppEntry probe = *a;
    for (int p = 0; p < PRESET_COUNT; p++) {
        poly_set_preset(&probe, (PolyPreset)p, a->x, a->y, a->w, a->h);
        if (probe.npts != a->npts) continue;
        bool same = true;
        for (int i = 0; i < a->npts && same; i++)
            same = fabs(probe.px[i] - a->px[i]) < 0.6 && fabs(probe.py[i] - a->py[i]) < 0.6;
        if (same) return p;
    }
    return -1;
}

static void row_value(Editor *ed, RowId r, char *out, size_t len) {
    out[0] = '\0';
    DesktopConfig *cfg = ed->cfg;
    if (r == ROW_WALLPAPER) { image_display_name(cfg->wallpaper, out, (int)len); return; }
    if (r == ROW_GUTTER) { snprintf(out, len, "%d px", cfg->gutter); return; }
    if (r == ROW_BAR) { snprintf(out, len, "%s", cfg->bar_enabled ? "Visible" : "Oculta"); return; }
    if (r == ROW_BAR_X) { snprintf(out, len, "%.0f", cfg->bar_x); return; }
    if (r == ROW_BAR_Y) { snprintf(out, len, "%.0f", cfg->bar_y); return; }
    if (r == ROW_BAR_W) { snprintf(out, len, "%.0f", cfg->bar_w); return; }
    if (r == ROW_BAR_H) { snprintf(out, len, "%.0f", cfg->bar_h); return; }
    if (row_color(ed, r)) { color_to_hex(row_color(ed, r), out, (int)len); return; }
    if (ed->selected < 0) return;
    AppEntry *a = &cfg->apps[ed->selected];
    ElementStyle *st = &a->style;
    switch (r) {
        case ROW_PRESET: {
            int p = current_preset(a);
            snprintf(out, len, "%s", p >= 0 ? poly_preset_label((PolyPreset)p) : "Libre");
            break;
        }
        case ROW_VERTEX:
            if (ed->vertex < 0) snprintf(out, len, "Ninguno");
            else snprintf(out, len, "%d de %d", ed->vertex + 1, a->npts);
            break;
        case ROW_NVERT: snprintf(out, len, "%d", a->npts); break;
        case ROW_X: snprintf(out, len, "%.0f", a->x); break;
        case ROW_Y: snprintf(out, len, "%.0f", a->y); break;
        case ROW_W: snprintf(out, len, "%.0f", a->w); break;
        case ROW_H: snprintf(out, len, "%.0f", a->h); break;
        case ROW_ROT: snprintf(out, len, "± 5°"); break;
        case ROW_RADIUS: snprintf(out, len, "%.0f", st->radius); break;
        case ROW_PADDING: snprintf(out, len, "%.0f", st->padding); break;
        case ROW_BORDER_STYLE: snprintf(out, len, "%s", BORDER_LABELS[st->border_style]); break;
        case ROW_BORDER_W: snprintf(out, len, "%.1f", st->border_width); break;
        case ROW_FILL: snprintf(out, len, "%s", FILL_LABELS[st->fill]); break;
        case ROW_IMAGE: image_display_name(st->image, out, (int)len); break;
        case ROW_SHADOW: snprintf(out, len, "%s", SHADOW_LABELS[st->shadow]); break;
        case ROW_ICON_SCALE: snprintf(out, len, "%.0f%%", st->icon_scale * 100.0); break;
        case ROW_LABEL: snprintf(out, len, "%s", LABEL_LABELS[st->label]); break;
        case ROW_FONT: snprintf(out, len, "%.1f", st->font_size); break;
        case ROW_PAGE: snprintf(out, len, "%d", a->page + 1); break;
        default: break;
    }
}

/* ------------------------------------------------------------ utilidades */

static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int cycle(int v, int dir, int n) { return ((v + dir) % n + n) % n; }

static void set_status(Editor *ed, const char *msg, uint32_t now) {
    snprintf(ed->status, sizeof(ed->status), "%s", msg);
    ed->status_until = now + 2500;
}

static void push_undo(Editor *ed) {
    EditorSnapshot *s = malloc(sizeof(*s));
    if (!s) return;
    *s = *ed->cfg;
    if (ed->undo_count == EDITOR_UNDO) {
        free(ed->undo[0]);
        memmove(ed->undo, ed->undo + 1, (EDITOR_UNDO - 1) * sizeof(ed->undo[0]));
        ed->undo_count--;
    }
    ed->undo[ed->undo_count++] = s;
}

static void mark_dirty(Editor *ed, uint32_t now) {
    ed->dirty = true;
    ed->dirty_ms = now;
}

// Un cambio en la misma fila seguido (mantener +) cuenta como un solo paso de deshacer
static void begin_change(Editor *ed, int row_key, uint32_t now) {
    if (row_key != ed->last_change_row || now - ed->last_change_ms > 900) push_undo(ed);
    ed->last_change_row = row_key;
    ed->last_change_ms = now;
}

static void undo(Editor *ed, uint32_t now) {
    if (ed->undo_count == 0) { set_status(ed, "Nada que deshacer", now); return; }
    EditorSnapshot *s = ed->undo[--ed->undo_count];
    *ed->cfg = *s;
    free(s);
    if (ed->selected >= ed->cfg->num_apps) ed->selected = ed->cfg->num_apps - 1;
    if (ed->selected >= 0 && ed->vertex >= ed->cfg->apps[ed->selected].npts) ed->vertex = -1;
    ed->last_change_row = -1;
    mark_dirty(ed, now);
    set_status(ed, "Deshecho", now);
}

static int nearest_palette(const ColorRGBA *c) {
    int best = 0;
    double bd = 1e9;
    for (int i = 0; i < PALETTE_N; i++) {
        ColorRGBA p = parse_hex_color(PALETTE[i], 1.0);
        double d = fabs(p.r - c->r) + fabs(p.g - c->g) + fabs(p.b - c->b);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* ------------------------------------------- cambios de forma con choque */

// Aplica `cand` al elemento si cabe; si no, lo deja como estaba y avisa de con quién choca.
static bool try_geometry(Editor *ed, int idx, const AppEntry *cand, uint32_t now, bool quiet) {
    DesktopConfig *cfg = ed->cfg;
    AppEntry old = cfg->apps[idx];
    cfg->apps[idx] = *cand;
    int r = layout_check(cfg, idx);
    if (r == -1) return true;
    cfg->apps[idx] = old;
    if (!quiet) {
        char msg[160];
        if (r >= 0) {
            snprintf(msg, sizeof(msg), "Choca con «%.60s» (separación mínima %d px)", cfg->apps[r].name, cfg->gutter);
            ed->collide_idx = r;
            ed->collide_bar = false;
            ed->collide_until = now + 700;
        } else if (r == LAYOUT_HITS_BAR) {
            snprintf(msg, sizeof(msg), "Choca con la barra (separación mínima %d px)", cfg->gutter);
            ed->collide_idx = -1;
            ed->collide_bar = true;
            ed->collide_until = now + 700;
        } else {
            snprintf(msg, sizeof(msg), "Esa forma no es válida (se cruza, es muy pequeña o se sale)");
        }
        set_status(ed, msg, now);
    }
    return false;
}

// Mueve con deslizamiento: si no cabe en diagonal, prueba solo en X y solo en Y
static bool try_move(Editor *ed, int idx, double dx, double dy, uint32_t now, bool quiet) {
    AppEntry c = ed->cfg->apps[idx];
    poly_translate(&c, dx, dy);
    if (try_geometry(ed, idx, &c, now, true)) return true;
    c = ed->cfg->apps[idx];
    poly_translate(&c, dx, 0);
    if (dx != 0 && try_geometry(ed, idx, &c, now, true)) return true;
    c = ed->cfg->apps[idx];
    poly_translate(&c, 0, dy);
    if (dy != 0 && try_geometry(ed, idx, &c, now, true)) return true;
    c = ed->cfg->apps[idx];
    poly_translate(&c, dx, dy);
    try_geometry(ed, idx, &c, now, quiet);   // solo para el aviso
    return false;
}

static bool try_vertex(Editor *ed, int idx, int v, double x, double y, uint32_t now, bool quiet) {
    AppEntry c = ed->cfg->apps[idx];
    c.px[v] = x;
    c.py[v] = y;
    app_update_bbox(&c);
    return try_geometry(ed, idx, &c, now, quiet);
}

/* ------------------------------------------------------------ acciones */

static void open_image_picker(Editor *ed, bool wallpaper) {
    if (!ed->images) ed->images = calloc(MAX_IMAGE_LIST, MAX_STR);
    if (!ed->images) return;
    ed->n_images = image_list(ed->images, MAX_IMAGE_LIST, wallpaper);
    ed->images_wallpaper = wallpaper;
    ed->picker = wallpaper ? PICK_WALLPAPER : PICK_IMAGE;
    ed->picker_scroll = 0;
}

static int image_index(Editor *ed, const char *path, bool wallpaper) {
    if (!ed->images || ed->images_wallpaper != wallpaper) {
        if (!ed->images) ed->images = calloc(MAX_IMAGE_LIST, MAX_STR);
        if (!ed->images) return 0;
        ed->n_images = image_list(ed->images, MAX_IMAGE_LIST, wallpaper);
        ed->images_wallpaper = wallpaper;
    }
    for (int i = 0; i < ed->n_images; i++) if (strcmp(ed->images[i], path) == 0) return i;
    return 0;
}

static void apply_row(Editor *ed, RowId r, int dir, uint32_t now) {
    DesktopConfig *cfg = ed->cfg;
    begin_change(ed, (int)r * 2 + (dir > 0), now);

    // Filas globales (sin selección)
    if (r == ROW_WALLPAPER) {
        int i = cycle(image_index(ed, cfg->wallpaper, true), dir, ed->n_images > 0 ? ed->n_images : 1);
        snprintf(cfg->wallpaper, sizeof(cfg->wallpaper), "%s", ed->n_images > 0 ? ed->images[i] : "");
        mark_dirty(ed, now);
        return;
    }
    if (r == ROW_BAR || r == ROW_BAR_X || r == ROW_BAR_Y || r == ROW_BAR_W || r == ROW_BAR_H) {
        DesktopConfig old = *cfg;
        if (r == ROW_BAR) cfg->bar_enabled = !cfg->bar_enabled;
        if (r == ROW_BAR_X) cfg->bar_x += 4 * dir;
        if (r == ROW_BAR_Y) cfg->bar_y += 4 * dir;
        if (r == ROW_BAR_W) cfg->bar_w = clampd(cfg->bar_w + 4 * dir, 20, cfg->screen_width);
        if (r == ROW_BAR_H) cfg->bar_h = clampd(cfg->bar_h + 2 * dir, 16, cfg->screen_height);
        int hit = layout_check_bar(cfg, -1);
        if (hit != -1) {
            char msg[160];
            if (hit >= 0) snprintf(msg, sizeof(msg), "La barra chocaría con «%.60s»", cfg->apps[hit].name);
            else snprintf(msg, sizeof(msg), "La barra se saldría de la pantalla");
            *cfg = old;
            set_status(ed, msg, now);
            return;
        }
        if (!cfg->bar_enabled) ed->bar_selected = false;
        mark_dirty(ed, now);
        return;
    }
    if (r == ROW_BAR || r == ROW_BAR_X || r == ROW_BAR_Y || r == ROW_BAR_W || r == ROW_BAR_H) {
        DesktopConfig old = *cfg;
        if (r == ROW_BAR) cfg->bar_enabled = !cfg->bar_enabled;
        if (r == ROW_BAR_X) cfg->bar_x += 4 * dir;
        if (r == ROW_BAR_Y) cfg->bar_y += 4 * dir;
        if (r == ROW_BAR_W) cfg->bar_w = clampd(cfg->bar_w + 4 * dir, 20, cfg->screen_width);
        if (r == ROW_BAR_H) cfg->bar_h = clampd(cfg->bar_h + 2 * dir, 16, cfg->screen_height);
        int hit = layout_check_bar(cfg, -1);
        if (hit != -1) {
            char msg[160];
            if (hit >= 0) snprintf(msg, sizeof(msg), "La barra chocaría con «%.60s»", cfg->apps[hit].name);
            else snprintf(msg, sizeof(msg), "La barra se saldría de la pantalla");
            *cfg = old;
            set_status(ed, msg, now);
            return;
        }
        if (!cfg->bar_enabled) ed->bar_selected = false;
        mark_dirty(ed, now);
        return;
    }
    if (r == ROW_GUTTER) {
        int g = (int)clampd(cfg->gutter + 2 * dir, 0, 40);
        int old = cfg->gutter;
        cfg->gutter = g;
        // Más separación solo si todos siguen cabiendo
        for (int i = 0; i < cfg->num_apps && dir > 0; i++) {
            if (layout_check(cfg, i) >= 0) {
                cfg->gutter = old;
                set_status(ed, "Hay elementos demasiado juntos para más separación", now);
                return;
            }
        }
        mark_dirty(ed, now);
        return;
    }
    ColorRGBA *c = row_color(ed, r);
    if (c) {
        int i = cycle(nearest_palette(c), dir, PALETTE_N);
        double alpha = c->a;
        *c = parse_hex_color(PALETTE[i], alpha);
        mark_dirty(ed, now);
        return;
    }
    if (ed->selected < 0) return;

    int idx = ed->selected;
    AppEntry *a = &cfg->apps[idx];
    ElementStyle *st = &a->style;
    AppEntry cand = *a;

    switch (r) {
        case ROW_PRESET: {
            int p = current_preset(a);
            p = cycle(p < 0 ? 0 : p, dir, PRESET_COUNT);
            poly_set_preset(&cand, (PolyPreset)p, a->x, a->y, a->w, a->h);
            if (try_geometry(ed, idx, &cand, now, false)) ed->vertex = -1;
            break;
        }
        case ROW_VERTEX:
            ed->vertex = cycle(ed->vertex + 1, dir, a->npts + 1) - 1;   // -1 = ninguno
            return;
        case ROW_NVERT:
            if (dir > 0) {
                int v = poly_add_vertex(&cand, ed->vertex);
                if (v >= 0 && try_geometry(ed, idx, &cand, now, false)) ed->vertex = v;
            } else {
                int v = ed->vertex >= 0 ? ed->vertex : a->npts - 1;
                if (poly_remove_vertex(&cand, v) && try_geometry(ed, idx, &cand, now, false)) ed->vertex = -1;
                else if (a->npts <= 3) set_status(ed, "Mínimo 3 vértices", now);
            }
            break;
        case ROW_X: try_move(ed, idx, 4 * dir, 0, now, false); break;
        case ROW_Y: try_move(ed, idx, 0, 4 * dir, now, false); break;
        case ROW_W:
            if (a->w + 4 * dir >= 24) { poly_scale(&cand, (a->w + 4 * dir) / a->w, 1); try_geometry(ed, idx, &cand, now, false); }
            break;
        case ROW_H:
            if (a->h + 4 * dir >= 24) { poly_scale(&cand, 1, (a->h + 4 * dir) / a->h); try_geometry(ed, idx, &cand, now, false); }
            break;
        case ROW_ROT: poly_rotate(&cand, 5.0 * dir); try_geometry(ed, idx, &cand, now, false); break;
        case ROW_RADIUS: st->radius = clampd(st->radius + 2 * dir, 0, 60); break;
        case ROW_PADDING: st->padding = clampd(st->padding + 2 * dir, 0, 60); break;
        case ROW_BORDER_STYLE: st->border_style = (BorderStyle)cycle(st->border_style, dir, BORDER_COUNT); break;
        case ROW_BORDER_W: st->border_width = clampd(st->border_width + 0.5 * dir, 0, 14); break;
        case ROW_FILL: st->fill = (FillStyle)cycle(st->fill, dir, FILL_COUNT); break;
        case ROW_IMAGE: {
            int i = cycle(image_index(ed, st->image, false), dir, ed->n_images > 0 ? ed->n_images : 1);
            snprintf(st->image, sizeof(st->image), "%s", ed->n_images > 0 ? ed->images[i] : "");
            break;
        }
        case ROW_SHADOW: st->shadow = (ShadowStyle)cycle(st->shadow, dir, SHADOW_COUNT); break;
        case ROW_ICON_SCALE: st->icon_scale = clampd(st->icon_scale + 0.05 * dir, 0.2, 2.0); break;
        case ROW_LABEL: st->label = (LabelPos)cycle(st->label, dir, LABEL_COUNT); break;
        case ROW_FONT: st->font_size = clampd(st->font_size + 0.5 * dir, 7, 30); break;
        case ROW_PAGE: {
            int target = (int)clampd(a->page + dir, 0, config_page_count(cfg));   // hasta una página nueva
            if (target == a->page) break;
            double fx, fy;
            // En la otra página va donde haya hueco
            if (layout_find_free(cfg, idx, target, a->w, a->h, &fx, &fy)) {
                cand.page = target;
                poly_translate(&cand, fx - a->x, fy - a->y);
                if (try_geometry(ed, idx, &cand, now, false)) ed->page = target;
            } else {
                set_status(ed, "No hay hueco en esa página", now);
            }
            break;
        }
        default: break;
    }
    mark_dirty(ed, now);
}

/* ------------------------------------------------------------ geometría */

typedef struct { double x, y, w, h; } Rect;

static bool in_rect(const Rect *r, double x, double y) {
    return x >= r->x && x <= r->x + r->w && y >= r->y && y <= r->y + r->h;
}

typedef struct {
    Rect preview;
    double scale;
    Rect panel;
    double row_top, row_h;
    Rect toolbar;
} Layout;

static Layout layout(const Editor *ed) {
    Layout L;
    double panel_w = 296.0;
    double pw = ed->width - panel_w - 26.0;
    L.scale = pw / ed->cfg->screen_width;
    L.preview = (Rect){ 8, 8, pw, ed->cfg->screen_height * L.scale };
    L.panel = (Rect){ ed->width - panel_w - 8, 8, panel_w, ed->height - 16 };
    L.row_top = L.panel.y + 50;
    L.row_h = 40;
    L.toolbar = (Rect){ 8, L.preview.y + L.preview.h + 8, pw, ed->height - (L.preview.y + L.preview.h + 8) - 8 };
    return L;
}

typedef enum {
    BTN_PREV_EL, BTN_NEXT_EL, BTN_PREV_PAGE, BTN_NEXT_PAGE,
    BTN_UNDO, BTN_ADD, BTN_DUP, BTN_REMOVE,
    BTN_STYLE_ALL, BTN_WALLPAPER, BTN_EXIT, BTN_COUNT
} ButtonId;

static const char *BUTTON_LABELS[BTN_COUNT] = {
    "◀ Elemento", "Elemento ▶", "◀ Página", "Página ▶",
    "Deshacer", "+ App", "Duplicar", "Quitar",
    "Estilo a todos", "Fondo", "Listo"
};

static Rect button_rect(const Layout *L, ButtonId b) {
    int row = b < 4 ? 0 : (b < 8 ? 1 : 2);
    int col = b < 4 ? b : (b < 8 ? b - 4 : b - 8);
    int cols = row == 2 ? 3 : 4;
    double gap = 6, bh = (L->toolbar.h - 2 * gap) / 3.0;
    if (bh > 46) bh = 46;
    double bw = (L->toolbar.w - (cols - 1) * gap) / cols;
    return (Rect){ L->toolbar.x + col * (bw + gap), L->toolbar.y + row * (bh + gap), bw, bh };
}

static Rect row_rect(const Layout *L, const Editor *ed, int i) {
    return (Rect){ L->panel.x, L->row_top + i * L->row_h - ed->panel_scroll, L->panel.w, L->row_h };
}
static Rect row_value_rect(const Rect *r) { return (Rect){ r->x + 104, r->y, r->w - 104 - 94, r->h }; }
static Rect row_minus(const Rect *r) { return (Rect){ r->x + r->w - 90, r->y + 4, 40, r->h - 8 }; }
static Rect row_plus(const Rect *r) { return (Rect){ r->x + r->w - 46, r->y + 4, 40, r->h - 8 }; }

static double panel_max_scroll(const Layout *L, int nrows, double row_h) {
    double content = nrows * row_h;
    double visible = L->panel.y + L->panel.h - L->row_top;
    return content > visible ? content - visible : 0;
}

static void ensure_row_visible(Editor *ed) {
    Layout L = layout(ed);
    double top = ed->row * L.row_h, visible = L.panel.y + L.panel.h - L.row_top;
    if (top < ed->panel_scroll) ed->panel_scroll = top;
    if (top + L.row_h > ed->panel_scroll + visible) ed->panel_scroll = top + L.row_h - visible;
}

static void to_screen(const Layout *L, double x, double y, double *sx, double *sy) {
    *sx = (x - L->preview.x) / L->scale;
    *sy = (y - L->preview.y) / L->scale;
}

/* -------------------------------------------------- más acciones (barra) */

static void select_element(Editor *ed, int idx) {
    if (idx != ed->selected) ed->vertex = -1;
    ed->bar_selected = false;
    ed->selected = idx;
    if (idx >= 0) ed->page = ed->cfg->apps[idx].page;
    RowId rows[40];
    int n = build_rows(ed, rows);
    if (ed->row >= n) ed->row = n - 1;
}

static void cycle_element(Editor *ed, int dir) {
    int n = ed->cfg->num_apps;
    if (n == 0) return;
    int i = ed->selected < 0 ? (dir > 0 ? -1 : 0) : ed->selected;
    select_element(ed, cycle(i, dir, n));
}

// Coloca un elemento nuevo/duplicado donde haya hueco (página actual o las siguientes)
static bool place_free(Editor *ed, AppEntry *a, int idx, uint32_t now) {
    DesktopConfig *cfg = ed->cfg;
    double sizes[][2] = { { a->w, a->h }, { cfg->tile_width, cfg->tile_height }, { 84, 80 } };
    for (int page = ed->page; page <= config_page_count(cfg); page++) {
        for (int s = 0; s < 3; s++) {
            double fx, fy;
            if (layout_find_free(cfg, idx, page, sizes[s][0], sizes[s][1], &fx, &fy)) {
                double sx = sizes[s][0] / a->w, sy = sizes[s][1] / a->h;
                poly_scale(a, sx, sy);
                poly_translate(a, fx - a->x, fy - a->y);
                a->page = page;
                return true;
            }
        }
    }
    set_status(ed, "No queda hueco libre", now);
    return false;
}

static void add_app(Editor *ed, const DesktopEntry *de, uint32_t now) {
    DesktopConfig *cfg = ed->cfg;
    if (cfg->num_apps >= MAX_APPS) { set_status(ed, "No caben más elementos", now); return; }
    AppEntry a;
    memset(&a, 0, sizeof(a));
    snprintf(a.name, sizeof(a.name), "%s", de->name);
    snprintf(a.exec, sizeof(a.exec), "%s", de->exec);
    snprintf(a.icon, sizeof(a.icon), "%s", de->icon);
    const char *ex = de->exec;
    size_t len = strcspn(ex, " \t");
    const char *base = ex;
    for (const char *q = ex; q < ex + len; q++) if (*q == '/') base = q + 1;
    snprintf(a.app_id, sizeof(a.app_id), "%.*s", (int)(ex + len - base), base);
    a.style = cfg->default_style;
    poly_set_preset(&a, PRESET_RECT, 0, 0, cfg->tile_width, cfg->tile_height);
    if (!place_free(ed, &a, -1, now)) return;
    push_undo(ed);
    cfg->apps[cfg->num_apps++] = a;
    select_element(ed, cfg->num_apps - 1);
    mark_dirty(ed, now);
    char msg[160];
    snprintf(msg, sizeof(msg), "Añadido «%.100s»", de->name);
    set_status(ed, msg, now);
}

static void press_button(Editor *ed, ButtonId b, uint32_t now) {
    DesktopConfig *cfg = ed->cfg;
    switch (b) {
        case BTN_PREV_EL: cycle_element(ed, -1); break;
        case BTN_NEXT_EL: cycle_element(ed, 1); break;
        case BTN_PREV_PAGE:
            if (ed->page > 0) ed->page--;
            select_element(ed, -1);
            break;
        case BTN_NEXT_PAGE:
            if (ed->page < config_page_count(cfg)) ed->page++;   // permite una página vacía nueva
            select_element(ed, -1);
            break;
        case BTN_UNDO: undo(ed, now); break;
        case BTN_ADD:
            if (!ed->entries) {
                ed->entries = calloc(EDITOR_MAX_ENTRIES, sizeof(DesktopEntry));
                if (ed->entries) ed->n_entries = desktop_entries_list(ed->entries, EDITOR_MAX_ENTRIES);
            }
            ed->picker = PICK_APP;
            ed->picker_scroll = 0;
            break;
        case BTN_DUP:
            if (ed->selected >= 0 && cfg->num_apps < MAX_APPS) {
                AppEntry a = cfg->apps[ed->selected];
                if (!place_free(ed, &a, -1, now)) break;
                push_undo(ed);
                cfg->apps[cfg->num_apps++] = a;
                select_element(ed, cfg->num_apps - 1);
                mark_dirty(ed, now);
            }
            break;
        case BTN_REMOVE:
            if (ed->selected >= 0) {
                push_undo(ed);
                char msg[160];
                snprintf(msg, sizeof(msg), "Quitado «%.100s» (Deshacer lo recupera)", cfg->apps[ed->selected].name);
                memmove(&cfg->apps[ed->selected], &cfg->apps[ed->selected + 1],
                        (cfg->num_apps - ed->selected - 1) * sizeof(AppEntry));
                cfg->num_apps--;
                select_element(ed, -1);
                mark_dirty(ed, now);
                set_status(ed, msg, now);
            }
            break;
        case BTN_STYLE_ALL:
            if (ed->selected >= 0) {
                push_undo(ed);
                ElementStyle st = cfg->apps[ed->selected].style;
                for (int i = 0; i < cfg->num_apps; i++) cfg->apps[i].style = st;
                cfg->default_style = st;
                mark_dirty(ed, now);
                set_status(ed, "Estilo aplicado a todos (la forma de cada uno no cambia)", now);
            }
            break;
        case BTN_WALLPAPER:
            select_element(ed, -1);
            open_image_picker(ed, true);
            break;
        case BTN_EXIT:
            editor_save(ed);
            ed->want_quit = true;
            break;
        default: break;
    }
}

/* ------------------------------------------------------------- dibujo */

static void text(cairo_t *cr, double x, double y, double size, bool bold, double r, double g, double b,
                 const char *s) {
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, s);
}

static void text_centered(cairo_t *cr, const Rect *r, double size, bool bold, double cr_, double cg, double cb,
                          const char *s) {
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_text_extents_t e;
    cairo_text_extents(cr, s, &e);
    double x = r->x + (r->w - e.width) / 2 - e.x_bearing;
    if (e.width > r->w - 4) x = r->x + 2;   // demasiado largo: alineado a la izquierda (se recorta)
    cairo_save(cr);
    cairo_rectangle(cr, r->x, r->y, r->w, r->h);
    cairo_clip(cr);
    cairo_set_source_rgb(cr, cr_, cg, cb);
    cairo_move_to(cr, x, r->y + r->h / 2 + size * 0.36);
    cairo_show_text(cr, s);
    cairo_restore(cr);
}

static void button(cairo_t *cr, const Rect *r, const char *label, bool pressed, bool enabled) {
    shape_path_rounded_rect(cr, r->x, r->y, r->w, r->h, 10);
    if (pressed) cairo_set_source_rgb(cr, 0.49, 0.23, 0.93);
    else cairo_set_source_rgb(cr, 0.16, 0.10, 0.26);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.04, 0.03, 0.07);
    cairo_set_line_width(cr, 2.5);
    cairo_stroke(cr);
    double k = enabled ? 1.0 : 0.45;
    text_centered(cr, r, 13, true, 1.0 * k, 0.96 * k, 0.84 * k, label);
}

static void poly_outline(cairo_t *cr, const AppEntry *a) {
    cairo_new_path(cr);
    for (int i = 0; i < a->npts; i++) {
        if (i == 0) cairo_move_to(cr, a->px[i], a->py[i]);
        else cairo_line_to(cr, a->px[i], a->py[i]);
    }
    cairo_close_path(cr);
}

static void render_preview(Editor *ed, cairo_t *cr, const Layout *L, uint32_t now) {
    DesktopConfig *cfg = ed->cfg;
    cairo_save(cr);
    cairo_rectangle(cr, L->preview.x, L->preview.y, L->preview.w, L->preview.h);
    cairo_clip(cr);
    cairo_translate(cr, L->preview.x, L->preview.y);
    cairo_scale(cr, L->scale, L->scale);
    page_render_fast(cr, cfg, ed->page, L->scale);

    // La barra (waybar): rectángulo con su nombre; seleccionable como un elemento más
    if (cfg->bar_enabled) {
        cairo_rectangle(cr, cfg->bar_x, cfg->bar_y, cfg->bar_w, cfg->bar_h);
        cairo_set_source_rgba(cr, 0.04, 0.03, 0.07, 0.85);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.75, 0.6, 1.0, 0.8);
        cairo_set_line_width(cr, 1.5 / L->scale);
        cairo_stroke(cr);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 13);
        cairo_set_source_rgb(cr, 0.85, 0.78, 1.0);
        cairo_move_to(cr, cfg->bar_x + 8, cfg->bar_y + cfg->bar_h / 2 + 5);
        cairo_show_text(cr, "Barra (waybar)");
        if (ed->bar_selected) {
            double d[2] = { 6, 4 };
            cairo_rectangle(cr, cfg->bar_x, cfg->bar_y, cfg->bar_w, cfg->bar_h);
            cairo_set_dash(cr, d, 2, 0);
            cairo_set_source_rgb(cr, 1.0, 0.84, 0.04);
            cairo_set_line_width(cr, 2.5 / L->scale);
            cairo_stroke(cr);
            cairo_set_dash(cr, NULL, 0, 0);
            cairo_arc(cr, cfg->bar_x + cfg->bar_w, cfg->bar_y + cfg->bar_h, 9.0 / L->scale, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }

    // Choque reciente: hitbox (silueta + zona de separación mínima) del elemento movido y del otro / la barra
    if (ed->collide_until > now) {
        const AppEntry *hit[2] = { NULL, NULL };
        AppEntry barpoly;
        if (ed->collide_bar && layout_bar_poly(cfg, &barpoly)) hit[0] = &barpoly;
        else if (ed->collide_idx >= 0 && ed->collide_idx < cfg->num_apps &&
                 cfg->apps[ed->collide_idx].page == ed->page) hit[0] = &cfg->apps[ed->collide_idx];
        if (ed->selected >= 0 && ed->selected < cfg->num_apps && cfg->apps[ed->selected].page == ed->page)
            hit[1] = &cfg->apps[ed->selected];
        cairo_save(cr);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        for (int h = 0; h < 2; h++) {
            if (!hit[h]) continue;
            if (cfg->gutter > 0) {   // la zona de separación: un borde de ancho 2·gutter centrado en la silueta
                poly_outline(cr, hit[h]);
                cairo_set_source_rgba(cr, 1.0, 0.2, 0.25, 0.5);
                cairo_set_line_width(cr, 2.0 * cfg->gutter);
                cairo_stroke(cr);
            }
            poly_outline(cr, hit[h]);
            cairo_set_source_rgba(cr, 1.0, 0.2, 0.25, 0.32);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 1.0, 0.95, 0.95, 0.95);
            cairo_set_line_width(cr, 3.5 / L->scale);
            cairo_stroke(cr);
        }
        cairo_restore(cr);
    }

    // Selección: la zona táctil exacta, sus vértices y los «+» para añadir vértices
    if (ed->selected >= 0 && cfg->apps[ed->selected].page == ed->page) {
        AppEntry *a = &cfg->apps[ed->selected];
        double d[2] = { 6, 4 };
        poly_outline(cr, a);
        cairo_set_dash(cr, d, 2, 0);
        cairo_set_source_rgb(cr, 1.0, 0.84, 0.04);
        cairo_set_line_width(cr, 2.0 / L->scale);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
        for (int i = 0; i < a->npts; i++) {
            int j = (i + 1) % a->npts;
            double mx = (a->px[i] + a->px[j]) / 2, my = (a->py[i] + a->py[j]) / 2;
            double r = 6.0 / L->scale;
            cairo_arc(cr, mx, my, r, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 0.04, 0.03, 0.07, 0.85);
            cairo_fill_preserve(cr);
            cairo_set_source_rgb(cr, 1.0, 0.84, 0.04);
            cairo_set_line_width(cr, 1.5 / L->scale);
            cairo_stroke(cr);
            cairo_move_to(cr, mx - r * 0.55, my);
            cairo_line_to(cr, mx + r * 0.55, my);
            cairo_move_to(cr, mx, my - r * 0.55);
            cairo_line_to(cr, mx, my + r * 0.55);
            cairo_stroke(cr);
        }
        for (int i = 0; i < a->npts; i++) {
            bool sel = i == ed->vertex;
            cairo_arc(cr, a->px[i], a->py[i], (sel ? 11.0 : 8.0) / L->scale, 0, 2 * M_PI);
            if (sel) cairo_set_source_rgb(cr, 1.0, 0.3, 0.35);
            else cairo_set_source_rgb(cr, 1.0, 0.84, 0.04);
            cairo_fill_preserve(cr);
            cairo_set_source_rgb(cr, 0.04, 0.03, 0.07);
            cairo_set_line_width(cr, 2.0 / L->scale);
            cairo_stroke(cr);
        }
    }
    cairo_restore(cr);

    cairo_rectangle(cr, L->preview.x, L->preview.y, L->preview.w, L->preview.h);
    cairo_set_source_rgb(cr, 0.49, 0.23, 0.93);
    cairo_set_line_width(cr, 2);
    cairo_stroke(cr);

    char pg[64];
    int pages = config_page_count(cfg);
    snprintf(pg, sizeof(pg), "Página %d/%d", ed->page + 1, pages > ed->page + 1 ? pages : ed->page + 1);
    text(cr, L->preview.x + 8, L->preview.y + L->preview.h - 8, 11, true, 1, 0.84, 0.04, pg);
}

/* ------------------------------------------------------ selector de color */

typedef enum { COL_WHEEL = 0, COL_RGB, COL_CMYK, COL_PALETTE, COL_MODES } ColorMode;
static const char *COL_MODE_LABEL[COL_MODES] = { "Rueda", "RGB", "CMYK", "Paleta" };
enum { CD_NONE = 0, CD_WHEEL = 1, CD_VALUE = 2, CD_ALPHA = 3, CD_SLIDER = 10 };   // CD_SLIDER + índice

static void rgb2hsv(double r, double g, double b, double *h, double *s, double *v) {
    double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b)), d = mx - mn;
    *v = mx;
    *s = mx > 1e-9 ? d / mx : 0;
    if (d < 1e-9) return;   // gris: se conserva el matiz anterior
    double hh;
    if (mx == r) hh = fmod((g - b) / d, 6.0);
    else if (mx == g) hh = (b - r) / d + 2.0;
    else hh = (r - g) / d + 4.0;
    hh /= 6.0;
    if (hh < 0) hh += 1.0;
    *h = hh;
}

static void hsv2rgb(double h, double s, double v, double *r, double *g, double *b) {
    h = h - floor(h);
    double c = v * s, x = c * (1 - fabs(fmod(h * 6.0, 2.0) - 1)), m = v - c, rr = 0, gg = 0, bb = 0;
    int sec = (int)(h * 6.0) % 6;
    switch (sec) {
        case 0: rr = c; gg = x; break;
        case 1: rr = x; gg = c; break;
        case 2: gg = c; bb = x; break;
        case 3: gg = x; bb = c; break;
        case 4: rr = x; bb = c; break;
        default: rr = c; bb = x; break;
    }
    *r = rr + m; *g = gg + m; *b = bb + m;
}

static void rgb2cmyk(double r, double g, double b, double *cmyk) {
    double k = 1.0 - fmax(r, fmax(g, b));
    if (k >= 1.0 - 1e-9) { cmyk[0] = cmyk[1] = cmyk[2] = 0; cmyk[3] = 1; return; }
    cmyk[0] = (1 - r - k) / (1 - k);
    cmyk[1] = (1 - g - k) / (1 - k);
    cmyk[2] = (1 - b - k) / (1 - k);
    cmyk[3] = k;
}

static void cmyk2rgb(const double *cmyk, double *r, double *g, double *b) {
    *r = (1 - cmyk[0]) * (1 - cmyk[3]);
    *g = (1 - cmyk[1]) * (1 - cmyk[3]);
    *b = (1 - cmyk[2]) * (1 - cmyk[3]);
}

static ColorRGBA *col_target(Editor *ed) { return row_color(ed, (RowId)ed->col_row); }

// Recalcula las demás representaciones a partir del RGB actual (source = la que manda y no se toca)
static void col_sync(Editor *ed, ColorMode source) {
    ColorRGBA *c = col_target(ed);
    if (!c) return;
    if (source != COL_WHEEL) rgb2hsv(c->r, c->g, c->b, &ed->col_hsv[0], &ed->col_hsv[1], &ed->col_hsv[2]);
    if (source != COL_CMYK) rgb2cmyk(c->r, c->g, c->b, ed->col_cmyk);
}

// Aplica la representación `source` al color real
static void col_commit(Editor *ed, ColorMode source, uint32_t now) {
    ColorRGBA *c = col_target(ed);
    if (!c) return;
    if (source == COL_WHEEL) hsv2rgb(ed->col_hsv[0], ed->col_hsv[1], ed->col_hsv[2], &c->r, &c->g, &c->b);
    else if (source == COL_CMYK) cmyk2rgb(ed->col_cmyk, &c->r, &c->g, &c->b);
    c->r = clampd(c->r, 0, 1); c->g = clampd(c->g, 0, 1); c->b = clampd(c->b, 0, 1);
    col_sync(ed, source);
    mark_dirty(ed, now);
}

static bool col_open(Editor *ed, RowId row) {
    ed->col_row = (int)row;
    if (!col_target(ed)) return false;
    push_undo(ed);
    ed->last_change_row = -1;
    ed->picker = PICK_COLOR;
    ed->col_field = 0;
    ed->col_drag = CD_NONE;
    col_sync(ed, COL_MODES);   // ninguna manda: se rellenan todas
    return true;
}

bool editor_open_color_picker(Editor *ed, int which) {
    if (which == 0) return ed->selected >= 0 && col_open(ed, ROW_BG);
    return col_open(ed, ROW_BG_BASE);
}

typedef struct {
    Rect swatch, close, tabs[COL_MODES], alpha, vbar, sl[5];
    double cx, cy, R;
    double cell_w, cell_h, gap, x0, y0;
} ColorGeom;

static void col_geom(const Layout *L, ColorGeom *g) {
    double bx = L->panel.x + 12, bw = L->panel.w - 24, y0 = L->row_top;
    g->close = (Rect){ L->panel.x + L->panel.w - 104, L->panel.y + 8, 96, 32 };
    g->swatch = (Rect){ bx, y0, bw, 34 };
    double tw = (bw - 3 * 4) / 4;
    for (int i = 0; i < COL_MODES; i++) g->tabs[i] = (Rect){ bx + i * (tw + 4), y0 + 42, tw, 28 };
    double cy0 = y0 + 80;
    g->alpha = (Rect){ bx, L->panel.y + L->panel.h - 38, bw, 22 };
    double avail = g->alpha.y - 22 - cy0;           // alto para el contenido
    g->R = fmin(bw / 2.0 - 4, (avail - 40) / 2.0);
    g->cx = bx + bw / 2.0;
    g->cy = cy0 + g->R + 4;
    g->vbar = (Rect){ bx, g->cy + g->R + 14, bw, 22 };
    for (int i = 0; i < 5; i++) g->sl[i] = (Rect){ bx, cy0 + 16 + i * 46, bw, 22 };
    g->gap = 6;
    g->cell_w = (bw - 4 * g->gap) / 5;
    g->cell_h = 40;
    g->x0 = bx;
    g->y0 = cy0;
}

static Rect col_palette_cell(const ColorGeom *g, int i) {
    return (Rect){ g->x0 + (i % 5) * (g->cell_w + g->gap), g->y0 + (i / 5) * (g->cell_h + g->gap), g->cell_w, g->cell_h };
}

static int col_slider_count(ColorMode m) { return m == COL_RGB ? 3 : (m == COL_CMYK ? 4 : 0); }

static double col_slider_value(Editor *ed, ColorMode m, int i) {
    ColorRGBA *c = col_target(ed);
    if (m == COL_RGB) return i == 0 ? c->r : (i == 1 ? c->g : c->b);
    return ed->col_cmyk[i];
}

static void col_set_slider(Editor *ed, ColorMode m, int i, double f, uint32_t now) {
    f = clampd(f, 0, 1);
    ColorRGBA *c = col_target(ed);
    if (m == COL_RGB) {
        if (i == 0) c->r = f; else if (i == 1) c->g = f; else c->b = f;
        col_commit(ed, COL_RGB, now);
    } else {
        ed->col_cmyk[i] = f;
        col_commit(ed, COL_CMYK, now);
    }
}

// Pone el valor según la posición del dedo/ratón; code = elemento que se arrastra
static void col_drag_to(Editor *ed, const ColorGeom *g, int code, double x, double y, uint32_t now) {
    ColorRGBA *c = col_target(ed);
    if (!c) return;
    if (code == CD_WHEEL) {
        double dx = x - g->cx, dy = y - g->cy;
        double h = atan2(dy, dx) / (2 * M_PI);
        if (h < 0) h += 1.0;
        ed->col_hsv[0] = h;
        ed->col_hsv[1] = clampd(hypot(dx, dy) / g->R, 0, 1);
        if (ed->col_hsv[2] < 0.02) ed->col_hsv[2] = 1.0;   // desde negro no se vería nada
        col_commit(ed, COL_WHEEL, now);
    } else if (code == CD_VALUE) {
        ed->col_hsv[2] = clampd((x - g->vbar.x) / g->vbar.w, 0, 1);
        col_commit(ed, COL_WHEEL, now);
    } else if (code == CD_ALPHA) {
        c->a = clampd((x - g->alpha.x) / g->alpha.w, 0, 1);
        mark_dirty(ed, now);
    } else if (code >= CD_SLIDER) {
        int i = code - CD_SLIDER;
        if (i < col_slider_count((ColorMode)ed->col_mode))
            col_set_slider(ed, (ColorMode)ed->col_mode, i, (x - g->sl[i].x) / g->sl[i].w, now);
    }
}

static void col_apply_palette(Editor *ed, int i, uint32_t now) {
    ColorRGBA *c = col_target(ed);
    if (!c || i < 0 || i >= PALETTE_N) return;
    *c = parse_hex_color(PALETTE[i], c->a);
    col_sync(ed, COL_MODES);
    mark_dirty(ed, now);
}

static void col_touch_down(Editor *ed, const Layout *L, double x, double y, uint32_t now) {
    ColorGeom g;
    col_geom(L, &g);
    ed->col_drag = CD_NONE;
    if (in_rect(&g.close, x, y)) { ed->picker = PICK_NONE; return; }
    for (int i = 0; i < COL_MODES; i++)
        if (in_rect(&g.tabs[i], x, y)) { ed->col_mode = i; ed->col_field = 0; return; }
    if (in_rect(&g.alpha, x, y - 0) || (y >= g.alpha.y - 6 && y <= g.alpha.y + g.alpha.h + 6 && x >= g.alpha.x && x <= g.alpha.x + g.alpha.w)) {
        ed->col_drag = CD_ALPHA;
    } else if (ed->col_mode == COL_WHEEL) {
        if (hypot(x - g.cx, y - g.cy) <= g.R + 6) ed->col_drag = CD_WHEEL;
        else if (y >= g.vbar.y - 6 && y <= g.vbar.y + g.vbar.h + 6 && x >= g.vbar.x && x <= g.vbar.x + g.vbar.w) ed->col_drag = CD_VALUE;
    } else if (ed->col_mode == COL_PALETTE) {
        for (int i = 0; i < PALETTE_N; i++) {
            Rect r = col_palette_cell(&g, i);
            if (in_rect(&r, x, y)) { col_apply_palette(ed, i, now); ed->col_field = i; return; }
        }
        return;
    } else {
        int n = col_slider_count((ColorMode)ed->col_mode);
        for (int i = 0; i < n; i++) {
            Rect r = g.sl[i];
            if (y >= r.y - 12 && y <= r.y + r.h + 8 && x >= r.x - 4 && x <= r.x + r.w + 4) { ed->col_drag = CD_SLIDER + i; ed->col_field = i; break; }
        }
    }
    if (ed->col_drag != CD_NONE) {
        ed->drag = ED_DRAG_COLOR;
        col_drag_to(ed, &g, ed->col_drag, x, y, now);
    }
}

static void col_touch_move(Editor *ed, const Layout *L, double x, double y, uint32_t now) {
    if (ed->col_drag == CD_NONE) return;
    ColorGeom g;
    col_geom(L, &g);
    col_drag_to(ed, &g, ed->col_drag, x, y, now);
}

// Mando: L/R cambian de pestaña, arriba/abajo de control, izquierda/derecha ajustan, B/A cierran
static void col_nav(Editor *ed, NavAction act, uint32_t now) {
    ColorMode m = (ColorMode)ed->col_mode;
    ColorRGBA *c = col_target(ed);
    if (!c) { ed->picker = PICK_NONE; return; }
    int nfields = m == COL_WHEEL ? 4 : (m == COL_RGB ? 4 : (m == COL_CMYK ? 5 : PALETTE_N));
    switch (act) {
        case NAV_ACTION_B: ed->picker = PICK_NONE; return;
        case NAV_ACTION_A:
            if (m == COL_PALETTE) col_apply_palette(ed, ed->col_field, now);
            else ed->picker = PICK_NONE;
            return;
        case NAV_PAGE_PREV: ed->col_mode = (m + COL_MODES - 1) % COL_MODES; ed->col_field = 0; return;
        case NAV_PAGE_NEXT: ed->col_mode = (m + 1) % COL_MODES; ed->col_field = 0; return;
        case NAV_UP:   ed->col_field -= (m == COL_PALETTE ? 5 : 1); break;
        case NAV_DOWN: ed->col_field += (m == COL_PALETTE ? 5 : 1); break;
        case NAV_LEFT:
        case NAV_RIGHT: {
            int dir = act == NAV_LEFT ? -1 : 1;
            int f = ed->col_field;
            if (m == COL_PALETTE) { ed->col_field = (int)clampd(f + dir, 0, PALETTE_N - 1); col_apply_palette(ed, ed->col_field, now); return; }
            begin_change(ed, 2000 + m * 10 + f, now);
            if (m == COL_WHEEL) {
                if (f == 0) { ed->col_hsv[0] = ed->col_hsv[0] + dir / 72.0; ed->col_hsv[0] -= floor(ed->col_hsv[0]); col_commit(ed, COL_WHEEL, now); }
                else if (f == 1) { ed->col_hsv[1] = clampd(ed->col_hsv[1] + dir * 0.05, 0, 1); col_commit(ed, COL_WHEEL, now); }
                else if (f == 2) { ed->col_hsv[2] = clampd(ed->col_hsv[2] + dir * 0.05, 0, 1); col_commit(ed, COL_WHEEL, now); }
                else { c->a = clampd(c->a + dir * 0.05, 0, 1); mark_dirty(ed, now); }
            } else if (f >= col_slider_count(m)) {
                c->a = clampd(c->a + dir * 0.05, 0, 1); mark_dirty(ed, now);
            } else {
                col_set_slider(ed, m, f, col_slider_value(ed, m, f) + dir * (m == COL_RGB ? 5.0 / 255.0 : 0.05), now);
            }
            return;
        }
        default: return;
    }
    ed->col_field = (int)clampd(ed->col_field, 0, nfields - 1);
}

static cairo_surface_t *col_wheel_surface(Editor *ed, int R) {
    if (ed->col_wheel && ed->col_wheel_r == R) return ed->col_wheel;
    if (ed->col_wheel) cairo_surface_destroy(ed->col_wheel);
    int d = 2 * R + 2;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, d, d);
    unsigned char *data = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    cairo_surface_flush(s);
    for (int py = 0; py < d; py++) {
        uint32_t *row = (uint32_t *)(data + py * stride);
        for (int px = 0; px < d; px++) {
            double dx = px + 0.5 - (R + 1), dy = py + 0.5 - (R + 1), dist = hypot(dx, dy);
            double a = clampd(R + 0.5 - dist, 0, 1);
            if (a <= 0) { row[px] = 0; continue; }
            double h = atan2(dy, dx) / (2 * M_PI);
            if (h < 0) h += 1.0;
            double r, g, b;
            hsv2rgb(h, fmin(1.0, dist / R), 1.0, &r, &g, &b);
            uint32_t A = (uint32_t)(a * 255 + 0.5);
            row[px] = (A << 24) | ((uint32_t)(r * a * 255 + 0.5) << 16) | ((uint32_t)(g * a * 255 + 0.5) << 8) | (uint32_t)(b * a * 255 + 0.5);
        }
    }
    cairo_surface_mark_dirty(s);
    ed->col_wheel = s;
    ed->col_wheel_r = R;
    return s;
}

static void col_draw_slider(cairo_t *cr, const Rect *tr, const char *label, const char *val, double f,
                            const double c0[3], const double c1[3], bool active, bool checker) {
    text(cr, tr->x, tr->y - 5, 11, true, 0.85, 0.78, 1.0, label);
    Rect vr = { tr->x + tr->w - 70, tr->y - 18, 70, 16 };
    text_centered(cr, &vr, 11, true, 1, 0.96, 0.84, val);
    if (checker) {   // fondo de ajedrez bajo la opacidad
        cairo_save(cr);
        shape_path_rounded_rect(cr, tr->x, tr->y, tr->w, tr->h, 6);
        cairo_clip(cr);
        for (int cx = 0; cx * 8 < tr->w; cx++)
            for (int cy = 0; cy * 8 < tr->h; cy++) {
                cairo_rectangle(cr, tr->x + cx * 8, tr->y + cy * 8, 8, 8);
                double v = ((cx + cy) & 1) ? 0.75 : 0.45;
                cairo_set_source_rgb(cr, v, v, v);
                cairo_fill(cr);
            }
        cairo_restore(cr);
    }
    cairo_pattern_t *p = cairo_pattern_create_linear(tr->x, 0, tr->x + tr->w, 0);
    cairo_pattern_add_color_stop_rgba(p, 0, c0[0], c0[1], c0[2], checker ? 0.0 : 1.0);
    cairo_pattern_add_color_stop_rgba(p, 1, c1[0], c1[1], c1[2], 1.0);
    shape_path_rounded_rect(cr, tr->x, tr->y, tr->w, tr->h, 6);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    shape_path_rounded_rect(cr, tr->x, tr->y, tr->w, tr->h, 6);
    cairo_set_source_rgba(cr, active ? 1.0 : 0.8, active ? 0.84 : 0.7, active ? 0.04 : 0.95, active ? 1.0 : 0.6);
    cairo_set_line_width(cr, active ? 2.0 : 1.0);
    cairo_stroke(cr);
    double hx = tr->x + clampd(f, 0, 1) * tr->w;
    cairo_arc(cr, hx, tr->y + tr->h / 2, tr->h / 2 + 3, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.1, 0.05, 0.2);
    cairo_set_line_width(cr, 2);
    cairo_stroke(cr);
}

static void col_render(Editor *ed, cairo_t *cr, const Layout *L) {
    ColorRGBA *c = col_target(ed);
    ColorGeom g;
    col_geom(L, &g);
    ColorMode m = (ColorMode)ed->col_mode;
    char title[64];
    snprintf(title, sizeof(title), "Color: %s", row_label((RowId)ed->col_row));
    text(cr, L->panel.x + 12, L->panel.y + 30, 15, true, 1, 0.96, 0.84, title);
    button(cr, &g.close, "Listo", false, true);
    if (!c) return;

    // Vista previa (sobre ajedrez para ver la opacidad) y valor hex
    cairo_save(cr);
    shape_path_rounded_rect(cr, g.swatch.x, g.swatch.y, g.swatch.w, g.swatch.h, 8);
    cairo_clip(cr);
    for (int cx = 0; cx * 8 < g.swatch.w; cx++)
        for (int cy = 0; cy * 8 < g.swatch.h; cy++) {
            cairo_rectangle(cr, g.swatch.x + cx * 8, g.swatch.y + cy * 8, 8, 8);
            double v = ((cx + cy) & 1) ? 0.75 : 0.45;
            cairo_set_source_rgb(cr, v, v, v);
            cairo_fill(cr);
        }
    cairo_rectangle(cr, g.swatch.x, g.swatch.y, g.swatch.w, g.swatch.h);
    cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);
    cairo_fill(cr);
    cairo_restore(cr);
    shape_path_rounded_rect(cr, g.swatch.x, g.swatch.y, g.swatch.w, g.swatch.h, 8);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    char hex[32];
    color_to_hex(c, hex, sizeof(hex));
    double lum = 0.3 * c->r + 0.59 * c->g + 0.11 * c->b;
    text_centered(cr, &g.swatch, 14, true, lum > 0.55 ? 0.05 : 1, lum > 0.55 ? 0.03 : 1, lum > 0.55 ? 0.1 : 1, hex);

    for (int i = 0; i < COL_MODES; i++) {
        bool on = i == (int)m;
        shape_path_rounded_rect(cr, g.tabs[i].x, g.tabs[i].y, g.tabs[i].w, g.tabs[i].h, 7);
        if (on) cairo_set_source_rgba(cr, 0.49, 0.23, 0.93, 1.0); else cairo_set_source_rgba(cr, 0.16, 0.1, 0.26, 1.0);
        cairo_fill(cr);
        text_centered(cr, &g.tabs[i], 12, true, on ? 1 : 0.8, on ? 1 : 0.72, on ? 1 : 0.95, COL_MODE_LABEL[i]);
    }

    if (m == COL_WHEEL) {
        int R = (int)g.R;
        cairo_surface_t *ws = col_wheel_surface(ed, R);
        cairo_save(cr);
        cairo_set_source_surface(cr, ws, g.cx - R - 1, g.cy - R - 1);
        cairo_paint(cr);
        cairo_restore(cr);
        // La rueda se pinta con brillo 1: se oscurece según el brillo elegido
        cairo_arc(cr, g.cx, g.cy, R, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0, 0, 0, 1.0 - ed->col_hsv[2]);
        cairo_fill(cr);
        cairo_arc(cr, g.cx, g.cy, R, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);
        double a = ed->col_hsv[0] * 2 * M_PI, rr = ed->col_hsv[1] * g.R;
        cairo_arc(cr, g.cx + cos(a) * rr, g.cy + sin(a) * rr, 8, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, c->r, c->g, c->b, 1);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_set_line_width(cr, 2.5);
        cairo_stroke(cr);
        double lo[3] = { 0, 0, 0 }, hi[3];
        hsv2rgb(ed->col_hsv[0], ed->col_hsv[1], 1.0, &hi[0], &hi[1], &hi[2]);
        char v[16];
        snprintf(v, sizeof(v), "%d %%", (int)lround(ed->col_hsv[2] * 100));
        col_draw_slider(cr, &g.vbar, "Brillo", v, ed->col_hsv[2], lo, hi, ed->col_field == 2 && ed->col_drag == CD_NONE && false, false);
        char info[64];
        snprintf(info, sizeof(info), "H %d°   S %d %%", (int)lround(ed->col_hsv[0] * 360) % 360, (int)lround(ed->col_hsv[1] * 100));
        text(cr, g.vbar.x + 52, g.vbar.y - 5, 11, false, 0.8, 0.72, 0.95, info);
    } else if (m == COL_RGB || m == COL_CMYK) {
        static const char *RGB_L[3] = { "Rojo", "Verde", "Azul" };
        static const char *CMYK_L[4] = { "Cian", "Magenta", "Amarillo", "Negro (K)" };
        int n = col_slider_count(m);
        for (int i = 0; i < n; i++) {
            double c0[3], c1[3];
            if (m == COL_RGB) {
                c0[0] = c1[0] = c->r; c0[1] = c1[1] = c->g; c0[2] = c1[2] = c->b;
                c0[i] = 0; c1[i] = 1;
            } else {
                double t0[4], t1[4];
                memcpy(t0, ed->col_cmyk, sizeof(t0)); memcpy(t1, ed->col_cmyk, sizeof(t1));
                t0[i] = 0; t1[i] = 1;
                cmyk2rgb(t0, &c0[0], &c0[1], &c0[2]);
                cmyk2rgb(t1, &c1[0], &c1[1], &c1[2]);
            }
            double f = col_slider_value(ed, m, i);
            char v[16];
            if (m == COL_RGB) snprintf(v, sizeof(v), "%d", (int)lround(f * 255));
            else snprintf(v, sizeof(v), "%d %%", (int)lround(f * 100));
            col_draw_slider(cr, &g.sl[i], m == COL_RGB ? RGB_L[i] : CMYK_L[i], v, f, c0, c1, ed->col_field == i, false);
        }
    } else {
        for (int i = 0; i < PALETTE_N; i++) {
            Rect r = col_palette_cell(&g, i);
            ColorRGBA pc = parse_hex_color(PALETTE[i], 1.0);
            shape_path_rounded_rect(cr, r.x, r.y, r.w, r.h, 7);
            cairo_set_source_rgb(cr, pc.r, pc.g, pc.b);
            cairo_fill_preserve(cr);
            bool same = fabs(pc.r - c->r) < 0.004 && fabs(pc.g - c->g) < 0.004 && fabs(pc.b - c->b) < 0.004;
            cairo_set_source_rgba(cr, 1, same ? 0.84 : 1, same ? 0.04 : 1, same ? 1.0 : 0.5);
            cairo_set_line_width(cr, same ? 3 : 1);
            cairo_stroke(cr);
            if (i == ed->col_field) {
                shape_path_rounded_rect(cr, r.x - 2, r.y - 2, r.w + 4, r.h + 4, 8);
                cairo_set_source_rgba(cr, 1, 0.84, 0.04, 1);
                cairo_set_line_width(cr, 1.5);
                cairo_stroke(cr);
            }
        }
    }

    // Opacidad (en todos los modos)
    double a0[3] = { c->r, c->g, c->b };
    char av[16];
    snprintf(av, sizeof(av), "%d %%", (int)lround(c->a * 100));
    bool alpha_active = (m == COL_WHEEL && ed->col_field == 3) || (m == COL_RGB && ed->col_field == 3) || (m == COL_CMYK && ed->col_field == 4);
    col_draw_slider(cr, &g.alpha, "Opacidad", av, c->a, a0, a0, alpha_active, true);
}

static void render_picker(Editor *ed, cairo_t *cr, const Layout *L) {
    if (ed->picker == PICK_COLOR) { col_render(ed, cr, L); return; }
    const char *title = ed->picker == PICK_APP ? "Añadir app" :
                        ed->picker == PICK_WALLPAPER ? "Fondo de pantalla" : "Imagen del elemento";
    text(cr, L->panel.x + 12, L->panel.y + 30, 15, true, 1, 0.96, 0.84, title);
    Rect cancel = { L->panel.x + L->panel.w - 104, L->panel.y + 8, 96, 32 };
    button(cr, &cancel, "Cancelar", false, true);

    cairo_save(cr);
    cairo_rectangle(cr, L->panel.x, L->row_top, L->panel.w, L->panel.y + L->panel.h - L->row_top);
    cairo_clip(cr);
    int n = ed->picker == PICK_APP ? ed->n_entries : ed->n_images;
    if (ed->picker == PICK_APP && n == 0)
        text(cr, L->panel.x + 12, L->row_top + 24, 12, false, 0.8, 0.7, 0.95, "No hay apps (.desktop) instaladas");
    for (int i = 0; i < n; i++) {
        double y = L->row_top + i * L->row_h - ed->picker_scroll;
        if (y + L->row_h < L->row_top || y > L->panel.y + L->panel.h) continue;
        double ix = L->panel.x + 26, iy = y + L->row_h / 2;
        char name[128];
        if (ed->picker == PICK_APP) {
            if (!icon_draw_for_app(cr, ed->entries[i].icon, ed->entries[i].exec, ix, iy, 28, 1, 0.96, 0.84)) {
                ColorRGBA fg = { 1, 0.96, 0.84, 1 };
                draw_icon_placeholder(cr, ix, iy, 24, ed->entries[i].icon, &fg);
            }
            snprintf(name, sizeof(name), "%s", ed->entries[i].name);
        } else {
            // Miniatura de la imagen (o de la incluida)
            const char *path = ed->images[i];
            Rect th = { ix - 16, iy - 14, 32, 28 };
            cairo_save(cr);
            cairo_rectangle(cr, th.x, th.y, th.w, th.h);
            if (!path[0]) {
                cairo_set_source_rgb(cr, 0.2, 0.15, 0.3);
                cairo_fill(cr);
            } else if (image_is_builtin(path)) {
                cairo_clip(cr);
                ColorRGBA bg = ed->selected >= 0 ? ed->cfg->apps[ed->selected].style.bg : ed->cfg->accent;
                ColorRGBA ink = { 0.04, 0.03, 0.07, 1 };
                cairo_set_source_rgb(cr, bg.r, bg.g, bg.b);
                cairo_paint(cr);
                if (ed->picker == PICK_WALLPAPER) {
                    cairo_translate(cr, th.x, th.y);
                    cairo_scale(cr, th.w / ed->cfg->screen_width, th.h / ed->cfg->screen_height);
                    image_draw_wallpaper(cr, path, ed->cfg->screen_width, ed->cfg->screen_height,
                                         &ed->cfg->bg_color, &ed->cfg->bg_gradient, &ed->cfg->accent);
                } else {
                    image_draw_builtin_element(cr, path, th.x + 2, th.y + 2, th.w - 4, th.h - 4, &bg, &ink);
                }
            } else {
                cairo_surface_t *img = image_get(path, 64, 64);
                if (img) image_fill_cover(cr, img, th.x, th.y, th.w, th.h);
                cairo_new_path(cr);
            }
            cairo_restore(cr);
            image_display_name(path, name, sizeof(name));
        }
        text(cr, L->panel.x + 50, y + L->row_h / 2 + 5, 13, false, 1, 0.96, 0.84, name);
    }
    cairo_restore(cr);
    if (ed->picker != PICK_APP) {
        char dir[MAX_STR], hint[MAX_STR + 32];
        image_user_dir(dir, sizeof(dir));
        const char *home = getenv("HOME");
        if (home && strncmp(dir, home, strlen(home)) == 0) snprintf(hint, sizeof(hint), "Tus imágenes: ~%s", dir + strlen(home));
        else snprintf(hint, sizeof(hint), "Tus imágenes: %s", dir);
        text(cr, L->panel.x + 12, L->panel.y + 46, 10, false, 0.7, 0.62, 0.85, hint);
    }
}

static void render_panel(Editor *ed, cairo_t *cr, const Layout *L) {
    shape_path_rounded_rect(cr, L->panel.x, L->panel.y, L->panel.w, L->panel.h, 12);
    cairo_set_source_rgb(cr, 0.09, 0.06, 0.15);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.49, 0.23, 0.93);
    cairo_set_line_width(cr, 2);
    cairo_stroke(cr);

    if (ed->picker != PICK_NONE) { render_picker(ed, cr, L); return; }

    char head[96];
    if (ed->bar_selected) snprintf(head, sizeof(head), "Barra (waybar)");
    else if (ed->selected < 0) snprintf(head, sizeof(head), "Página y fondo");
    else snprintf(head, sizeof(head), "%.40s", ed->cfg->apps[ed->selected].name);
    text(cr, L->panel.x + 12, L->panel.y + 30, 15, true, 1, 0.96, 0.84, head);

    RowId rows[40];
    int n = build_rows(ed, rows);
    cairo_save(cr);
    cairo_rectangle(cr, L->panel.x, L->row_top, L->panel.w, L->panel.y + L->panel.h - L->row_top);
    cairo_clip(cr);
    for (int i = 0; i < n; i++) {
        Rect r = row_rect(L, ed, i);
        if (r.y + r.h < L->row_top || r.y > L->panel.y + L->panel.h) continue;
        if (i == ed->row) {
            shape_path_rounded_rect(cr, r.x + 3, r.y + 1, r.w - 6, r.h - 2, 8);
            cairo_set_source_rgba(cr, 0.49, 0.23, 0.93, 0.45);
            cairo_fill(cr);
        }
        text(cr, r.x + 12, r.y + r.h / 2 + 5, 12, false, 0.85, 0.78, 1.0, row_label(rows[i]));
        char val[96];
        row_value(ed, rows[i], val, sizeof(val));
        Rect vr = row_value_rect(&r);
        ColorRGBA *c = row_color(ed, rows[i]);
        if (c) {
            shape_path_rounded_rect(cr, vr.x + 4, r.y + 9, vr.w - 8, r.h - 18, 5);
            cairo_set_source_rgba(cr, c->r, c->g, c->b, 1);
            cairo_fill_preserve(cr);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        } else {
            text_centered(cr, &vr, 12, true, 1, 0.96, 0.84, val);
            if (rows[i] == ROW_IMAGE || rows[i] == ROW_WALLPAPER) {
                // Subrayado: tocar el valor abre el selector
                cairo_move_to(cr, vr.x + 8, r.y + r.h - 7);
                cairo_line_to(cr, vr.x + vr.w - 8, r.y + r.h - 7);
                cairo_set_source_rgba(cr, 1, 0.84, 0.04, 0.7);
                cairo_set_line_width(cr, 1);
                cairo_stroke(cr);
            }
        }
        Rect m = row_minus(&r), p = row_plus(&r);
        bool hm = ed->held_row == i && ed->held_dir < 0, hp = ed->held_row == i && ed->held_dir > 0;
        button(cr, &m, "−", hm, true);
        button(cr, &p, "+", hp, true);
    }
    cairo_restore(cr);

    if (ed->selected < 0 && !ed->bar_selected) {
        double y = L->row_top + n * L->row_h + 22 - ed->panel_scroll;
        const char *help[] = {
            "Toca un elemento para editarlo.",
            "Arrastra sus vértices amarillos para",
            "cambiar la forma; los «+» añaden uno.",
            "Los elementos no se pueden solapar.",
            "Mando: L/R elemento, cruceta valores,",
            "stick izq. mueve, stick der. el vértice."
        };
        for (size_t i = 0; i < sizeof(help) / sizeof(help[0]); i++)
            text(cr, L->panel.x + 12, y + i * 17, 11, false, 0.8, 0.7, 0.95, help[i]);
    }
}

void editor_render(Editor *ed, cairo_t *cr, int width, int height) {
    ed->width = width;
    ed->height = height;
    uint32_t now = ed->collide_until > 700 ? ed->collide_until - 700 : 0;
    Layout L = layout(ed);

    cairo_set_source_rgb(cr, 0.05, 0.04, 0.08);
    cairo_paint(cr);
    render_preview(ed, cr, &L, now);

    for (int b = 0; b < BTN_COUNT; b++) {
        Rect r = button_rect(&L, (ButtonId)b);
        bool needs_sel = (b == BTN_DUP || b == BTN_REMOVE || b == BTN_STYLE_ALL);
        button(cr, &r, BUTTON_LABELS[b], ed->pressed_button == b, !needs_sel || ed->selected >= 0);
    }
    render_panel(ed, cr, &L);

    if (ed->status[0] && ed->status_until) {
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12);
        cairo_text_extents_t e;
        cairo_text_extents(cr, ed->status, &e);
        double w = fmin(e.width + 28, L.preview.w - 16);
        Rect r = { L.preview.x + (L.preview.w - w) / 2, L.preview.y + 10, w, 28 };
        shape_path_rounded_rect(cr, r.x, r.y, r.w, r.h, 14);
        cairo_set_source_rgba(cr, 1.0, 0.84, 0.04, 0.95);
        cairo_fill(cr);
        text_centered(cr, &r, 12, true, 0.05, 0.03, 0.08, ed->status);
    }
}

/* ------------------------------------------------------------- entrada */

void editor_touch_down(Editor *ed, double x, double y, uint32_t now) {
    Layout L = layout(ed);
    ed->down_x = x;
    ed->down_y = y;
    ed->drag = ED_DRAG_NONE;
    ed->drag_moved = false;
    ed->pressed_button = -1;
    ed->last_dx = ed->last_dy = 0;

    if (ed->picker == PICK_COLOR) {
        if (in_rect(&L.panel, x, y)) { col_touch_down(ed, &L, x, y, now); return; }
        ed->picker = PICK_NONE;   // tocar fuera cierra
    }
    if (ed->picker != PICK_NONE) {
        Rect cancel = { L.panel.x + L.panel.w - 104, L.panel.y + 8, 96, 32 };
        if (in_rect(&cancel, x, y)) { ed->picker = PICK_NONE; return; }
        if (in_rect(&L.panel, x, y)) {
            ed->drag = ED_DRAG_PICKER;
            ed->orig_scroll = ed->picker_scroll;
            return;
        }
    }

    DesktopConfig *cfg = ed->cfg;
    if (in_rect(&L.preview, x, y)) {
        double sx, sy;
        to_screen(&L, x, y, &sx, &sy);
        if (ed->selected >= 0 && cfg->apps[ed->selected].page == ed->page) {
            AppEntry *a = &cfg->apps[ed->selected];
            // 1) un vértice
            for (int i = 0; i < a->npts; i++) {
                if (hypot(sx - a->px[i], sy - a->py[i]) * L.scale < 14) {
                    ed->vertex = i;
                    ed->drag = ED_DRAG_VERTEX;
                    ed->orig_x = a->px[i];
                    ed->orig_y = a->py[i];
                    return;
                }
            }
            // 2) un «+» de lado: añade un vértice ahí y se puede arrastrar ya
            for (int i = 0; i < a->npts; i++) {
                int j = (i + 1) % a->npts;
                double mx = (a->px[i] + a->px[j]) / 2, my = (a->py[i] + a->py[j]) / 2;
                if (hypot(sx - mx, sy - my) * L.scale < 10 && a->npts < MAX_POLY) {
                    push_undo(ed);
                    ed->last_change_row = -1;
                    int v = poly_add_vertex(a, i);
                    ed->vertex = v;
                    ed->drag = ED_DRAG_VERTEX;
                    ed->orig_x = a->px[v];
                    ed->orig_y = a->py[v];
                    ed->drag_moved = true;   // ya cuenta como cambio
                    mark_dirty(ed, now);
                    return;
                }
            }
        }
        // 3) la barra: tirador de tamaño o la propia barra
        if (cfg->bar_enabled) {
            if (ed->bar_selected && hypot(sx - (cfg->bar_x + cfg->bar_w), sy - (cfg->bar_y + cfg->bar_h)) * L.scale < 14) {
                ed->drag = ED_DRAG_BAR_RESIZE;
                ed->orig_w = cfg->bar_w;
                ed->orig_h = cfg->bar_h;
                return;
            }
            if (sx >= cfg->bar_x && sx <= cfg->bar_x + cfg->bar_w && sy >= cfg->bar_y && sy <= cfg->bar_y + cfg->bar_h) {
                select_element(ed, -1);
                ed->bar_selected = true;
                ed->drag = ED_DRAG_BAR;
                ed->orig_x = cfg->bar_x;
                ed->orig_y = cfg->bar_y;
                return;
            }
        }
        // 4) un elemento (su zona táctil exacta)
        int hit = grid_view_hit_test_page(cfg, ed->page, sx, sy);
        if (hit >= 0) {
            select_element(ed, hit);
            ed->drag = ED_DRAG_ELEMENT;
            ed->orig_x = cfg->apps[hit].x;
            ed->orig_y = cfg->apps[hit].y;
        } else {
            select_element(ed, -1);
        }
        return;
    }

    if (ed->picker == PICK_NONE && in_rect(&L.panel, x, y)) {
        RowId rows[40];
        int n = build_rows(ed, rows);
        for (int i = 0; i < n; i++) {
            Rect r = row_rect(&L, ed, i);
            if (r.y + r.h < L.row_top || !in_rect(&r, x, y)) continue;
            Rect m = row_minus(&r), p = row_plus(&r), v = row_value_rect(&r);
            ed->row = i;
            if (in_rect(&m, x, y) || in_rect(&p, x, y)) {
                int dir = in_rect(&p, x, y) ? 1 : -1;
                ed->held_row = i;
                ed->held_dir = dir;
                ed->held_since = ed->held_last = now;
                ed->drag = ED_DRAG_BUTTON;
                apply_row(ed, rows[i], dir, now);
                return;
            }
            if (in_rect(&v, x, y) && row_color(ed, rows[i])) { col_open(ed, rows[i]); return; }
            if (in_rect(&v, x, y) && (rows[i] == ROW_IMAGE || rows[i] == ROW_WALLPAPER)) {
                open_image_picker(ed, rows[i] == ROW_WALLPAPER);
                return;
            }
        }
        ed->drag = ED_DRAG_PANEL;
        ed->orig_scroll = ed->panel_scroll;
        return;
    }

    for (int b = 0; b < BTN_COUNT; b++) {
        Rect r = button_rect(&L, (ButtonId)b);
        if (in_rect(&r, x, y)) { ed->pressed_button = b; return; }
    }
}

void editor_touch_motion(Editor *ed, double x, double y, uint32_t now) {
    Layout L = layout(ed);
    Layout Lc = L;
    double dx = x - ed->down_x, dy = y - ed->down_y;
    if (ed->drag != ED_DRAG_COLOR && !ed->drag_moved && hypot(dx, dy) < 4) return;
    DesktopConfig *cfg = ed->cfg;

    switch (ed->drag) {
        case ED_DRAG_ELEMENT: {
            if (!ed->drag_moved) { push_undo(ed); ed->last_change_row = -1; }
            AppEntry *a = &cfg->apps[ed->selected];
            // Objetivo en rejilla de 2 px; avanza lo que se pueda sin chocar (se desliza por los bordes)
            double tx = round((ed->orig_x + dx / L.scale) / 2) * 2, ty = round((ed->orig_y + dy / L.scale) / 2) * 2;
            double mx = tx - a->x, my = ty - a->y;
            // Barrido: se avanza en pasos de ≤2 px por la recta hacia el objetivo (cada paso se desliza por el eje
            // libre si el otro choca), así un arrastre rápido no «salta» por encima de otros elementos.
            double steps = ceil(fmax(fabs(mx), fabs(my)) / 2.0);
            if (steps > 600) steps = 600;
            if (steps < 1) steps = 1;
            double sx = mx / steps, sy = my / steps;
            bool blocked = false;
            for (int k = 0; k < (int)steps; k++) {
                double bx = a->x, by = a->y;
                if (!try_move(ed, ed->selected, sx, sy, now, true) ||
                    fabs(a->x - bx - sx) > 0.01 || fabs(a->y - by - sy) > 0.01)
                    blocked = true;
            }
            if (blocked) {
                AppEntry c = *a;
                poly_translate(&c, sx, sy);
                try_geometry(ed, ed->selected, &c, now, false);   // marca el choque (hitbox visibles)
            }
            mark_dirty(ed, now);
            break;
        }
        case ED_DRAG_VERTEX: {
            if (!ed->drag_moved) { push_undo(ed); ed->last_change_row = -1; }
            double tx = round(ed->orig_x + dx / L.scale), ty = round(ed->orig_y + dy / L.scale);
            AppEntry *a = &cfg->apps[ed->selected];
            if (!try_vertex(ed, ed->selected, ed->vertex, tx, ty, now, true)) {
                // Deslizar en un solo eje si en diagonal no cabe
                if (!try_vertex(ed, ed->selected, ed->vertex, tx, a->py[ed->vertex], now, true) &&
                    !try_vertex(ed, ed->selected, ed->vertex, a->px[ed->vertex], ty, now, true)) {
                    try_vertex(ed, ed->selected, ed->vertex, tx, ty, now, false);   // aviso
                }
            }
            mark_dirty(ed, now);
            break;
        }
        case ED_DRAG_BAR:
        case ED_DRAG_BAR_RESIZE: {
            if (!ed->drag_moved) { push_undo(ed); ed->last_change_row = -1; }
            DesktopConfig old = *cfg;
            double sdx = dx / L.scale, sdy = dy / L.scale;
            if (ed->drag == ED_DRAG_BAR) {
                cfg->bar_x = round((ed->orig_x + sdx) / 2) * 2;
                cfg->bar_y = round((ed->orig_y + sdy) / 2) * 2;
            } else {
                cfg->bar_w = clampd(round((ed->orig_w + sdx) / 2) * 2, 20, cfg->screen_width);
                cfg->bar_h = clampd(round((ed->orig_h + sdy) / 2) * 2, 16, cfg->screen_height);
            }
            int hit = layout_check_bar(cfg, -1);
            if (hit != -1) {
                *cfg = old;
                if (hit >= 0) {
                    ed->collide_idx = hit;
                    ed->collide_until = now + 700;
                }
            }
            mark_dirty(ed, now);
            break;
        }
        case ED_DRAG_PANEL: {
            RowId rows[40];
            int n = build_rows(ed, rows);
            ed->panel_scroll = clampd(ed->orig_scroll - dy, 0, panel_max_scroll(&L, n, L.row_h));
            break;
        }
        case ED_DRAG_COLOR:
            col_touch_move(ed, &Lc, x, y, now);
            break;
        case ED_DRAG_PICKER: {
            int n = ed->picker == PICK_APP ? ed->n_entries : ed->n_images;
            double max = n * L.row_h - (L.panel.y + L.panel.h - L.row_top);
            ed->picker_scroll = clampd(ed->orig_scroll - dy, 0, max > 0 ? max : 0);
            break;
        }
        case ED_DRAG_BUTTON:
            if (hypot(dx, dy) > 24) ed->held_row = -1;
            break;
        default:
            break;
    }
    ed->drag_moved = true;
}

void editor_touch_up(Editor *ed, double x, double y, uint32_t now) {
    Layout L = layout(ed);
    (void)x;
    if (ed->drag == ED_DRAG_COLOR) { ed->col_drag = CD_NONE; ed->drag = ED_DRAG_NONE; return; }
    if (ed->drag == ED_DRAG_PICKER && !ed->drag_moved) {
        int i = (int)((y - L.row_top + ed->picker_scroll) / L.row_h);
        DesktopConfig *cfg = ed->cfg;
        if (y >= L.row_top && i >= 0) {
            if (ed->picker == PICK_APP && i < ed->n_entries) {
                add_app(ed, &ed->entries[i], now);
            } else if (ed->picker == PICK_WALLPAPER && i < ed->n_images) {
                push_undo(ed);
                snprintf(cfg->wallpaper, sizeof(cfg->wallpaper), "%s", ed->images[i]);
                mark_dirty(ed, now);
            } else if (ed->picker == PICK_IMAGE && i < ed->n_images && ed->selected >= 0) {
                push_undo(ed);
                ElementStyle *st = &cfg->apps[ed->selected].style;
                snprintf(st->image, sizeof(st->image), "%s", ed->images[i]);
                mark_dirty(ed, now);
            }
            ed->picker = PICK_NONE;
        }
    }
    if (ed->pressed_button >= 0) {
        Rect r = button_rect(&L, (ButtonId)ed->pressed_button);
        if (in_rect(&r, x, y)) press_button(ed, (ButtonId)ed->pressed_button, now);
    }
    ed->pressed_button = -1;
    ed->held_row = -1;
    ed->held_dir = 0;
    ed->drag = ED_DRAG_NONE;
}

void editor_nav(Editor *ed, NavAction act, uint32_t now) {
    RowId rows[40];
    int n = build_rows(ed, rows);
    if (ed->picker == PICK_COLOR) { col_nav(ed, act, now); return; }
    if (ed->picker != PICK_NONE) {
        if (act == NAV_ACTION_B) ed->picker = PICK_NONE;
        return;
    }
    switch (act) {
        case NAV_UP:
            if (ed->row > 0) ed->row--;
            ensure_row_visible(ed);
            break;
        case NAV_DOWN:
            if (ed->row < n - 1) ed->row++;
            ensure_row_visible(ed);
            break;
        case NAV_ACTION_A:
            if (ed->row >= 0 && ed->row < n && row_color(ed, rows[ed->row])) { col_open(ed, rows[ed->row]); break; }
            /* fallthrough */
        case NAV_LEFT:
        case NAV_RIGHT:
            if (ed->row >= 0 && ed->row < n) apply_row(ed, rows[ed->row], act == NAV_LEFT ? -1 : 1, now);
            break;
        case NAV_PAGE_PREV: cycle_element(ed, -1); break;
        case NAV_PAGE_NEXT: cycle_element(ed, 1); break;
        case NAV_ACTION_B: undo(ed, now); break;
        case NAV_ACTION_X: press_button(ed, BTN_DUP, now); break;
        case NAV_ACTION_MINUS: press_button(ed, BTN_REMOVE, now); break;
        case NAV_ACTION_PLUS: press_button(ed, BTN_EXIT, now); break;
        default: break;
    }
}

#define STICK_DEAD 9000

bool editor_busy(const Editor *ed, const GamepadInput *gp, uint32_t now) {
    if (ed->held_row >= 0 && ed->held_dir != 0) return true;
    if (ed->status_until && now < ed->status_until) return true;
    if (ed->collide_until && now < ed->collide_until) return true;
    if (ed->dirty) return true;
    if (gp && gp->is_open && (abs(gp->abs_lx) > STICK_DEAD || abs(gp->abs_ly) > STICK_DEAD ||
                              abs(gp->abs_rx) > STICK_DEAD || abs(gp->abs_ry) > STICK_DEAD)) return true;
    return false;
}

bool editor_tick(Editor *ed, const GamepadInput *gp, uint32_t now) {
    bool redraw = false;

    if (ed->held_row >= 0 && ed->held_dir != 0 && now - ed->held_since > 420 && now - ed->held_last > 70) {
        RowId rows[40];
        int n = build_rows(ed, rows);
        if (ed->held_row < n) apply_row(ed, rows[ed->held_row], ed->held_dir, now);
        ed->held_last = now;
        redraw = true;
    }

    // Sticks: izquierdo mueve el elemento; derecho mueve el vértice elegido (o escala si no hay)
    if (gp && gp->is_open && ed->selected >= 0 && ed->picker == PICK_NONE) {
        AppEntry *a = &ed->cfg->apps[ed->selected];
        double vx = abs(gp->abs_lx) > STICK_DEAD ? gp->abs_lx / 32767.0 : 0;
        double vy = abs(gp->abs_ly) > STICK_DEAD ? gp->abs_ly / 32767.0 : 0;
        double rx = abs(gp->abs_rx) > STICK_DEAD ? gp->abs_rx / 32767.0 : 0;
        double ry = abs(gp->abs_ry) > STICK_DEAD ? gp->abs_ry / 32767.0 : 0;
        if (vx || vy) {
            begin_change(ed, 1000, now);
            try_move(ed, ed->selected, vx * 4, vy * 4, now, false);
            mark_dirty(ed, now);
            redraw = true;
        }
        if (rx || ry) {
            begin_change(ed, 1001, now);
            if (ed->vertex >= 0 && ed->vertex < a->npts) {
                try_vertex(ed, ed->selected, ed->vertex, a->px[ed->vertex] + rx * 3, a->py[ed->vertex] + ry * 3, now, false);
            } else {
                AppEntry c = *a;
                poly_scale(&c, (a->w + rx * 3) / a->w, (a->h + ry * 3) / a->h);
                if (c.w >= 24 && c.h >= 24) try_geometry(ed, ed->selected, &c, now, false);
            }
            mark_dirty(ed, now);
            redraw = true;
        }
    }

    if (ed->dirty && now - ed->dirty_ms > 600 && ed->drag == ED_DRAG_NONE) {
        if (editor_save(ed)) set_status(ed, "Guardado", now);
        redraw = true;
    }
    if (ed->status_until && now >= ed->status_until) {
        ed->status_until = 0;
        ed->status[0] = '\0';
        redraw = true;
    }
    if (ed->collide_until && now >= ed->collide_until) {
        ed->collide_until = 0;
        redraw = true;
    }
    return redraw;
}

// Guardar = escribir la configuración, generar las imágenes de página desde ESA
// configuración (como la leerá el shell) y publicarla con un rename: cuando el shell
// la recarga, sus páginas ya están en la caché.
bool editor_save(Editor *ed) {
    if (!ed->dirty) return true;
    char staging[MAX_STR + 16];
    snprintf(staging, sizeof(staging), "%s.staging", ed->save_path);
    bool ok = config_save(ed->cfg, staging);
    if (ok) {
        DesktopConfig *fresh = malloc(sizeof(DesktopConfig));
        if (fresh && config_load(fresh, staging)) page_cache_build_files(fresh, staging);
        free(fresh);
        ok = rename(staging, ed->save_path) == 0;
    }
    if (ok) {
        ed->dirty = false;
        printf("[Editor] Guardado en %s\n", ed->save_path);
    } else {
        snprintf(ed->status, sizeof(ed->status), "No se pudo guardar en %.120s", ed->save_path);
    }
    return ok;
}

void editor_init(Editor *ed, DesktopConfig *cfg, const char *save_path, int width, int height) {
    memset(ed, 0, sizeof(*ed));
    ed->cfg = cfg;
    snprintf(ed->save_path, sizeof(ed->save_path), "%s", save_path);
    ed->width = width;
    ed->height = height;
    ed->selected = cfg->num_apps > 0 ? 0 : -1;
    ed->vertex = -1;
    ed->held_row = -1;
    ed->pressed_button = -1;
    ed->last_change_row = -1;
    ed->collide_idx = -1;
}

void editor_free(Editor *ed) {
    for (int i = 0; i < ed->undo_count; i++) free(ed->undo[i]);
    ed->undo_count = 0;
    free(ed->entries);
    ed->entries = NULL;
    free(ed->images);
    ed->images = NULL;
    if (ed->col_wheel) { cairo_surface_destroy(ed->col_wheel); ed->col_wheel = NULL; }
}
