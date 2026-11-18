#include "grid_view.h"
#include "draw_shapes.h"
#include "page_cache.h"
#include "images.h"
#include "poly.h"
#include <stdio.h>
#include <math.h>

// Inicio: cada elemento está en su caja (x, y, w, h) de su página; el editor las cambia.

static void clamp_focus(GridView *gv) {
    const DesktopConfig *cfg = gv->cfg;
    if (gv->focused_index >= 0 && gv->focused_index < cfg->num_apps &&
        cfg->apps[gv->focused_index].page == gv->current_page) return;
    gv->focused_index = -1;
    for (int i = 0; i < cfg->num_apps; i++) {
        if (cfg->apps[i].page == gv->current_page) { gv->focused_index = i; break; }
    }
}

static void image_draw_wallpaper_cfg(cairo_t *cr, const DesktopConfig *cfg, int w, int h) {
    image_draw_wallpaper(cr, cfg->wallpaper, w, h, &cfg->bg_color, &cfg->bg_gradient, &cfg->accent);
}

void grid_view_init(GridView *gv, const DesktopConfig *cfg) {
    gv->cfg = cfg;
    gv->cache = NULL;
    gv->current_page = 0;
    gv->focused_index = 0;
    gv->pressed_index = -1;
    gv->scroll_offset_x = 0.0;
    grid_view_reload(gv);
}

void grid_view_reload(GridView *gv) {
    gv->total_pages = config_page_count(gv->cfg);
    if (gv->current_page >= gv->total_pages) gv->current_page = gv->total_pages - 1;
    if (gv->pressed_index >= gv->cfg->num_apps) gv->pressed_index = -1;
    clamp_focus(gv);
}

void grid_view_render_page(const DesktopConfig *cfg, cairo_t *cr, int page, double offset_x,
                           int focused, int pressed) {
    cairo_save(cr);
    cairo_translate(cr, offset_x, 0);
    page_render(cr, cfg, page, false);
    if (focused >= 0 && focused < cfg->num_apps && cfg->apps[focused].page == page)
        element_draw_focus(cr, &cfg->apps[focused], &cfg->tile_glow, focused == pressed);
    cairo_restore(cr);
}

void grid_view_render(GridView *gv, cairo_t *cr, int width, int height) {
    const DesktopConfig *cfg = gv->cfg;
    cairo_surface_t *img = gv->cache ? page_cache_page(gv->cache, gv->current_page) : NULL;
    if (img) {
        // Página pre-renderizada: una sola copia de píxeles; encima solo el foco
        if (gv->scroll_offset_x != 0.0 && gv->cache->wallpaper) {
            cairo_set_source_surface(cr, gv->cache->wallpaper, 0, 0);
            cairo_paint(cr);
        }
        cairo_set_source_surface(cr, img, gv->scroll_offset_x, 0);
        cairo_paint(cr);
        int f = gv->focused_index, p = gv->pressed_index;
        if (p >= 0 && p < cfg->num_apps && cfg->apps[p].page == gv->current_page) f = p;
        if (f >= 0 && f < cfg->num_apps && cfg->apps[f].page == gv->current_page) {
            cairo_save(cr);
            cairo_translate(cr, gv->scroll_offset_x, 0);
            element_draw_focus(cr, &cfg->apps[f], &cfg->tile_glow, f == p);
            cairo_restore(cr);
        }
    } else {
        image_draw_wallpaper_cfg(cr, cfg, width, height);
        grid_view_render_page(cfg, cr, gv->current_page, gv->scroll_offset_x, gv->focused_index, gv->pressed_index);
    }

    // Puntos de página estilo Wii U
    if (gv->total_pages > 1) {
        cairo_save(cr);
        double dot_spacing = 18.0;
        double dots_x = (width - (gv->total_pages - 1) * dot_spacing) / 2.0;
        double dots_y = height - 16.0;
        for (int p = 0; p < gv->total_pages; p++) {
            double px = dots_x + p * dot_spacing;
            if (p == gv->current_page) {
                cairo_arc(cr, px, dots_y, 4.5, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, cfg->tile_glow.r, cfg->tile_glow.g, cfg->tile_glow.b, 1.0);
            } else {
                cairo_arc(cr, px, dots_y, 3.0, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.35);
            }
            cairo_fill(cr);
        }
        cairo_restore(cr);
    }

    // Franja superior (con la barra activada, la barra ocupa su lugar)
    if (cfg->bar_enabled) return;
    cairo_save(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11.0);
    cairo_set_source_rgba(cr, cfg->accent.r, cfg->accent.g, cfg->accent.b, 0.85);
    cairo_move_to(cr, width - 110.0, 28.0);
    cairo_show_text(cr, "[ HOME MENU ]");
    cairo_restore(cr);
}

// Navegación espacial: el elemento más cercano en la dirección pedida
void grid_view_nav(GridView *gv, int dx, int dy) {
    const DesktopConfig *cfg = gv->cfg;
    clamp_focus(gv);
    if (gv->focused_index < 0) return;
    const AppEntry *cur = &cfg->apps[gv->focused_index];
    double cx = cur->x + cur->w / 2.0, cy = cur->y + cur->h / 2.0;

    int best = -1;
    double best_cost = 1e18;
    for (int i = 0; i < cfg->num_apps; i++) {
        const AppEntry *a = &cfg->apps[i];
        if (i == gv->focused_index || a->page != gv->current_page) continue;
        double ox = a->x + a->w / 2.0 - cx, oy = a->y + a->h / 2.0 - cy;
        double along = ox * dx + oy * dy;          // avance en la dirección
        double across = fabs(ox * dy) + fabs(oy * dx);  // desvío lateral
        if (along <= 1.0) continue;
        double cost = along + across * 2.0;
        if (cost < best_cost) { best_cost = cost; best = i; }
    }
    if (best >= 0) {
        gv->focused_index = best;
    } else if (dx > 0) {
        grid_view_next_page(gv);
    } else if (dx < 0) {
        grid_view_prev_page(gv);
    }
}

bool grid_view_update(GridView *gv, int width) {
    (void)width;
    if (fabs(gv->scroll_offset_x) > 0.5) {
        gv->scroll_offset_x *= 0.65;
        if (fabs(gv->scroll_offset_x) <= 0.5) gv->scroll_offset_x = 0.0;
        return true;
    }
    return false;
}

void grid_view_next_page(GridView *gv) {
    if (gv->current_page < gv->total_pages - 1) {
        gv->current_page++;
        gv->scroll_offset_x = 350.0;
        gv->focused_index = -1;
        clamp_focus(gv);
    }
}

void grid_view_prev_page(GridView *gv) {
    if (gv->current_page > 0) {
        gv->current_page--;
        gv->scroll_offset_x = -350.0;
        gv->focused_index = -1;
        clamp_focus(gv);
    }
}

int grid_view_hit_test_page(const DesktopConfig *cfg, int page, double x, double y) {
    // El último dibujado está encima: se prueba primero
    for (int i = cfg->num_apps - 1; i >= 0; i--) {
        const AppEntry *a = &cfg->apps[i];
        if (a->page != page) continue;
        if (poly_contains(a, x, y)) return i;   // la forma ES la zona táctil
    }
    return -1;
}

int grid_view_hit_test(GridView *gv, double x, double y, int width, int height) {
    (void)width; (void)height;
    return grid_view_hit_test_page(gv->cfg, gv->current_page, x, y);
}
