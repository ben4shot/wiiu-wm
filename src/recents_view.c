#include "recents_view.h"
#include "draw_shapes.h"
#include "icons.h"
#include <stdio.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <string.h>

void recents_view_init(RecentsView *rv, const DesktopConfig *cfg) {
    memset(rv, 0, sizeof(*rv));
    rv->cfg = cfg;
    rv->pressed_card = -1;
}

void recents_view_set_items(RecentsView *rv, const RecentItem *items, int count, bool focus_active) {
    if (count > MAX_RECENT_ITEMS) count = MAX_RECENT_ITEMS;
    if (count < 0) count = 0;
    memcpy(rv->items, items, count * sizeof(RecentItem));
    rv->count = count;
    if (focus_active) {
        rv->focused_card = count > 0 ? count - 1 : 0;
        for (int i = 0; i < count; i++) if (items[i].active) rv->focused_card = i;
    }
    if (rv->focused_card >= count) rv->focused_card = count > 0 ? count - 1 : 0;
}

bool recents_view_update(RecentsView *rv, int width) {
    double card_w = width * 0.44;
    double target_x = -(card_w + 30.0) * rv->focused_card;

    if (fabs(target_x - rv->scroll_x) > 0.5) {
        rv->scroll_x += (target_x - rv->scroll_x) * 0.35;
        if (fabs(target_x - rv->scroll_x) <= 0.5) {
            rv->scroll_x = target_x;
        }
        return true; // Continuar redibujado
    }
    return false;
}

static void get_card_rect(int index, int total, int width, int height,
                          double *out_x, double *out_y, double *out_w, double *out_h) {
    (void)total;
    double card_w = width * 0.44;  // ~375 px en 854x480
    double card_h = height * 0.62; // ~300 px
    double center_x = width / 2.0;
    double card_spacing = card_w + 30.0;

    // Coloca la tarjeta indexada en relación al centro
    *out_w = card_w;
    *out_h = card_h;
    *out_x = center_x - card_w / 2.0 + (index) * card_spacing;
    *out_y = (height - card_h) / 2.0 + 10.0;
}

void recents_view_render(RecentsView *rv, cairo_t *cr, int width, int height) {
    const DesktopConfig *cfg = rv->cfg;

    // Fondo oscuro translúcido con desenfoque simulado
    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.04, 0.03, 0.08, 0.94);
    cairo_paint(cr);
    cairo_restore(cr);

    // Título de la vista
    cairo_save(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 17.0);
    cairo_set_source_rgba(cr, cfg->tile_glow.r, cfg->tile_glow.g, cfg->tile_glow.b, 0.95);
    cairo_move_to(cr, 32.0, 42.0);
    cairo_show_text(cr, "Aplicaciones Abiertas");

    // Subtítulo con instrucciones
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11.5);
    cairo_set_source_rgba(cr, 0.75, 0.65, 0.9, 0.7);
    cairo_move_to(cr, 32.0, 62.0);
    cairo_show_text(cr, "A: abrir  |  X / ×: cerrar  |  Y / ▦: mosaico  |  HOME: Inicio");
    cairo_restore(cr);

    if (rv->count == 0) {
        cairo_save(cr);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 15.0);
        cairo_set_source_rgba(cr, 0.6, 0.5, 0.75, 0.8);
        const char *msg = "No hay aplicaciones abiertas recientes";
        cairo_text_extents_t ext;
        cairo_text_extents(cr, msg, &ext);
        cairo_move_to(cr, (width - ext.width) / 2.0, height / 2.0);
        cairo_show_text(cr, msg);

        // Botón grande para ir al inicio
        double bw = 160.0, bh = 44.0;
        double bx = (width - bw) / 2.0, by = height / 2.0 + 30.0;
        shape_path_rounded_rect(cr, bx, by, bw, bh, 12.0);
        cairo_set_source_rgba(cr, cfg->accent.r, cfg->accent.g, cfg->accent.b, 0.85);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        cairo_move_to(cr, bx + 22.0, by + 27.0);
        cairo_show_text(cr, "Ir al Menú de Inicio");
        cairo_restore(cr);
        return;
    }

    // Renderizar tarjetas con la posición calculada
    for (int i = 0; i < rv->count; i++) {
        double x, y, w, h;
        get_card_rect(i, rv->count, width, height, &x, &y, &w, &h);
        x += rv->scroll_x;

        // Si la tarjeta está fuera de pantalla, omitir
        if (x + w < -50 || x > width + 50) continue;

        bool is_focused = (i == rv->focused_card);
        const RecentItem *app = &rv->items[i];

        // Sombra de la tarjeta
        cairo_save(cr);
        shape_path_rounded_rect(cr, x + 2.0, y + 4.0, w, h, 18.0);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
        cairo_fill(cr);
        cairo_restore(cr);

        // Cuerpo de la tarjeta
        cairo_save(cr);
        shape_path_rounded_rect(cr, x, y, w, h, 18.0);
        cairo_pattern_t *pat = cairo_pattern_create_linear(x, y, x, y + h);
        cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.18, 0.12, 0.28, 0.95);
        cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.09, 0.06, 0.16, 0.95);
        cairo_set_source(cr, pat);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(pat);

        // Borde
        if (is_focused) {
            cairo_set_source_rgba(cr, cfg->tile_glow.r, cfg->tile_glow.g, cfg->tile_glow.b, 1.0);
            cairo_set_line_width(cr, 3.0);
        } else {
            cairo_set_source_rgba(cr, cfg->tile_border.r, cfg->tile_border.g, cfg->tile_border.b, 0.6);
            cairo_set_line_width(cr, 1.5);
        }
        cairo_stroke(cr);
        cairo_restore(cr);

        // Barra superior de la tarjeta (Icono + Título + Botón Cerrar)
        cairo_save(cr);
        if (!icon_draw_for_app(cr, app->icon, app->exec, x + 24.0, y + 24.0, 24.0,
                               cfg->tile_text.r, cfg->tile_text.g, cfg->tile_text.b)) {
            draw_icon_placeholder(cr, x + 24.0, y + 24.0, 26.0, app->icon, &cfg->tile_text);
        }

        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 14.0);
        cairo_set_source_rgba(cr, cfg->tile_text.r, cfg->tile_text.g, cfg->tile_text.b, 1.0);
        cairo_move_to(cr, x + 44.0, y + 29.0);
        cairo_show_text(cr, app->name);

        // Botón Cerrar 'X' estilo Android en la esquina superior derecha de la tarjeta
        double close_btn_x = x + w - 28.0;
        double close_btn_y = y + 24.0;
        cairo_arc(cr, close_btn_x, close_btn_y, 11.0, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0.35, 0.25, 0.45, 0.85);
        cairo_fill(cr);

        cairo_set_line_width(cr, 2.0);
        cairo_set_source_rgba(cr, 0.9, 0.85, 1.0, 0.95);
        cairo_move_to(cr, close_btn_x - 4.5, close_btn_y - 4.5);
        cairo_line_to(cr, close_btn_x + 4.5, close_btn_y + 4.5);
        cairo_move_to(cr, close_btn_x + 4.5, close_btn_y - 4.5);
        cairo_line_to(cr, close_btn_x - 4.5, close_btn_y + 4.5);
        cairo_stroke(cr);
        cairo_restore(cr);

        // Área de contenido / previsualización de la tarjeta
        cairo_save(cr);
        double prev_x = x + 16.0, prev_y = y + 48.0, prev_w = w - 32.0, prev_h = h - 64.0;
        shape_path_rounded_rect(cr, prev_x, prev_y, prev_w, prev_h, 10.0);
        cairo_set_source_rgba(cr, 0.05, 0.04, 0.09, 0.9);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.25, 0.18, 0.38, 0.5);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        // Detalle en la previsualización
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.5);
        cairo_set_source_rgba(cr, 0.65, 0.55, 0.8, 0.8);
        cairo_move_to(cr, prev_x + 16.0, prev_y + prev_h / 2.0);
        cairo_show_text(cr, app->subtitle);
        if (app->active) {
            cairo_move_to(cr, prev_x + 16.0, prev_y + prev_h / 2.0 + 20.0);
            cairo_show_text(cr, "En primer plano");
        }
        if (app->workspace > 0) {
            char ws[64];
            snprintf(ws, sizeof(ws), "Escritorio %d%s", app->workspace, app->tiled ? "  ·  mosaico" : "");
            cairo_move_to(cr, prev_x + 16.0, prev_y + 22.0);
            cairo_show_text(cr, ws);
        }

        // Botón de mosaico (esquina inferior derecha): 4 cuadritos; relleno si está en mosaico
        double tbx = x + w - 30.0, tby = y + h - 30.0;
        shape_path_rounded_rect(cr, tbx - 18, tby - 18, 36, 36, 9);
        if (app->tiled) cairo_set_source_rgba(cr, cfg->tile_glow.r, cfg->tile_glow.g, cfg->tile_glow.b, 0.95);
        else cairo_set_source_rgba(cr, 0.35, 0.25, 0.45, 0.9);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, app->tiled ? 0.08 : 0.92, app->tiled ? 0.05 : 0.86, app->tiled ? 0.12 : 1.0, 1.0);
        for (int q = 0; q < 4; q++) {
            double qx = tbx - 10 + (q % 2) * 11, qy = tby - 10 + (q / 2) * 11;
            shape_path_rounded_rect(cr, qx, qy, 9, 9, 2);
            cairo_fill(cr);
        }
        cairo_restore(cr);
    }

    // Botón inferior fijo para volver al Menú de Inicio
    cairo_save(cr);
    double hbw = 140.0, hbh = 34.0;
    double hbx = (width - hbw) / 2.0, hby = height - 44.0;
    shape_path_rounded_rect(cr, hbx, hby, hbw, hbh, 17.0);
    cairo_set_source_rgba(cr, cfg->accent.r, cfg->accent.g, cfg->accent.b, 0.8);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, cfg->tile_glow.r, cfg->tile_glow.g, cfg->tile_glow.b, 0.9);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11.5);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
    cairo_move_to(cr, hbx + 26.0, hby + 21.0);
    cairo_show_text(cr, "[ INICIO (HOME) ]");
    cairo_restore(cr);
}

void recents_view_nav(RecentsView *rv, int dir) {
    if (rv->count == 0) return;
    int next = rv->focused_card + dir;
    if (next < 0) next = 0;
    if (next >= rv->count) next = rv->count - 1;
    rv->focused_card = next;
}

int recents_view_hit_test(RecentsView *rv, double x, double y, int width, int height, bool *out_close_btn,
                          bool *out_tile_btn) {
    if (out_close_btn) *out_close_btn = false;
    if (out_tile_btn) *out_tile_btn = false;
    if (rv->count == 0) {
        // Comprobar si se tocó el botón de ir al inicio
        double bw = 160.0, bh = 44.0;
        double bx = (width - bw) / 2.0, by = height / 2.0 + 30.0;
        if (x >= bx && x <= bx + bw && y >= by && y <= by + bh) {
            return -2; // Señal especial: Ir al inicio
        }
        return -1;
    }

    // Comprobar botón inferior de inicio
    double hbw = 140.0, hbh = 34.0;
    double hbx = (width - hbw) / 2.0, hby = height - 44.0;
    if (x >= hbx && x <= hbx + hbw && y >= hby && y <= hby + hbh) {
        return -2; // Ir al inicio
    }

    for (int i = 0; i < rv->count; i++) {
        double cx, cy, cw, ch;
        get_card_rect(i, rv->count, width, height, &cx, &cy, &cw, &ch);
        cx += rv->scroll_x;

        if (x >= cx && x <= cx + cw && y >= cy && y <= cy + ch) {
            // Comprobar si tocó la 'X' de cerrar
            double close_x = cx + cw - 28.0;
            double close_y = cy + 24.0;
            if (hypot(x - close_x, y - close_y) <= 16.0) {
                if (out_close_btn) *out_close_btn = true;
            }
            // Botón de mosaico en la esquina inferior derecha
            if (hypot(x - (cx + cw - 30.0), y - (cy + ch - 30.0)) <= 22.0) {
                if (out_tile_btn) *out_tile_btn = true;
            }
            return i;
        }
    }

    return -1;
}
