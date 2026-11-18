#include "draw_shapes.h"
#include "icons.h"
#include "images.h"
#include "poly.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Superelipse |x/a|^n + |y/b|^n = 1 (la forma «squircle» de los canales de Wii U).
// `r` controla lo cuadrada que es: más radio, más redonda (n más bajo).
void shape_path_squircle(cairo_t *cr, double x, double y, double w, double h, double r) {
    double a = w / 2.0, b = h / 2.0;
    double cx = x + a, cy = y + b;
    double ratio = (r > 0.0) ? r / (a < b ? a : b) : 0.44;
    if (ratio > 1.0) ratio = 1.0;
    double n = 6.0 - 3.5 * ratio;   // r=24 sobre 55 -> n~4.5
    const int steps = 96;

    cairo_new_sub_path(cr);
    for (int i = 0; i < steps; i++) {
        double t = (2.0 * M_PI * i) / steps;
        double c = cos(t), s = sin(t);
        double px = cx + a * copysign(pow(fabs(c), 2.0 / n), c);
        double py = cy + b * copysign(pow(fabs(s), 2.0 / n), s);
        if (i == 0) cairo_move_to(cr, px, py);
        else cairo_line_to(cr, px, py);
    }
    cairo_close_path(cr);
}

void shape_path_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r > w / 2.0) r = w / 2.0;
    if (r > h / 2.0) r = h / 2.0;

    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0, M_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

void shape_path_circle(cairo_t *cr, double x, double y, double w, double h) {
    cairo_new_sub_path(cr);
    cairo_save(cr);
    cairo_translate(cr, x + w / 2.0, y + h / 2.0);
    cairo_scale(cr, w / 2.0, h / 2.0);
    cairo_arc(cr, 0.0, 0.0, 1.0, 0.0, 2.0 * M_PI);
    cairo_restore(cr);
    cairo_close_path(cr);
}

// Polígono convexo con esquinas redondeadas por arcos tangentes a ambos lados.
static void shape_path_rounded_polygon(cairo_t *cr, const double *px, const double *py, int n, double r) {
    cairo_new_sub_path(cr);
    for (int i = 0; i < n; i++) {
        double ax = px[(i + n - 1) % n], ay = py[(i + n - 1) % n];
        double vx = px[i], vy = py[i];
        double bx = px[(i + 1) % n], by = py[(i + 1) % n];

        double ux = ax - vx, uy = ay - vy, ul = hypot(ux, uy);
        double wx = bx - vx, wy = by - vy, wl = hypot(wx, wy);
        ux /= ul; uy /= ul; wx /= wl; wy /= wl;

        double cosang = ux * wx + uy * wy;
        if (cosang > 1.0) cosang = 1.0;
        if (cosang < -1.0) cosang = -1.0;
        double half = acos(cosang) / 2.0;          // mitad del ángulo interior
        double rr = r;
        double d = rr / tan(half);                 // distancia del vértice al punto tangente
        double dmax = (ul < wl ? ul : wl) * 0.45;
        if (d > dmax) { d = dmax; rr = d * tan(half); }

        double t1x = vx + ux * d, t1y = vy + uy * d;
        double t2x = vx + wx * d, t2y = vy + wy * d;
        double bxs = ux + wx, bys = uy + wy, bl = hypot(bxs, bys);
        double ccx = vx + bxs / bl * (rr / sin(half));
        double ccy = vy + bys / bl * (rr / sin(half));

        double a1 = atan2(t1y - ccy, t1x - ccx);
        double a2 = atan2(t2y - ccy, t2x - ccx);
        // Sentido de giro: producto vectorial de las aristas entrante y saliente
        double cross = (vx - ax) * (by - vy) - (vy - ay) * (bx - vx);

        if (i == 0) cairo_move_to(cr, t1x, t1y);
        else cairo_line_to(cr, t1x, t1y);
        if (cross > 0) cairo_arc(cr, ccx, ccy, rr, a1, a2);
        else cairo_arc_negative(cr, ccx, ccy, rr, a1, a2);
    }
    cairo_close_path(cr);
}

void shape_path_triangle(cairo_t *cr, double x, double y, double w, double h, TileShape dir, double r) {
    double px[3], py[3];
    switch (dir) {
        case SHAPE_TRIANGLE_DOWN:
            px[0] = x;         py[0] = y;
            px[1] = x + w;     py[1] = y;
            px[2] = x + w / 2; py[2] = y + h;
            break;
        case SHAPE_TRIANGLE_LEFT:
            px[0] = x;         py[0] = y + h / 2;
            px[1] = x + w;     py[1] = y;
            px[2] = x + w;     py[2] = y + h;
            break;
        case SHAPE_TRIANGLE_RIGHT:
            px[0] = x + w;     py[0] = y + h / 2;
            px[1] = x;         py[1] = y + h;
            px[2] = x;         py[2] = y;
            break;
        case SHAPE_TRIANGLE_UP:
        default:
            px[0] = x + w / 2; py[0] = y;
            px[1] = x + w;     py[1] = y + h;
            px[2] = x;         py[2] = y + h;
            break;
    }
    // Las esquinas de un triángulo son agudas: un radio menor evita que parezca una gota
    shape_path_rounded_polygon(cr, px, py, 3, r * 0.6);
}

// Hexágono de lados planos arriba y abajo, inscrito en el rectángulo del tile.
void shape_path_hexagon(cairo_t *cr, double x, double y, double w, double h, double r) {
    double px[6] = { x + w * 0.25, x + w * 0.75, x + w, x + w * 0.75, x + w * 0.25, x };
    double py[6] = { y, y, y + h / 2, y + h, y + h, y + h / 2 };
    shape_path_rounded_polygon(cr, px, py, 6, r * 0.5);
}

/* ------------------------------------------------------- formas de cómic */

// Viñeta: cuadrilátero con los lados inclinados según `slant` (0 = rectángulo)
void shape_path_panel(cairo_t *cr, double x, double y, double w, double h, double slant, double r) {
    double px[4] = { x + slant * w * 0.12, x + w, x + w - slant * w * 0.07, x };
    double py[4] = { y, y + slant * h * 0.09, y + h, y + h - slant * h * 0.12 };
    shape_path_rounded_polygon(cr, px, py, 4, r);
}

// Bocadillo: elipse con una cola hacia `tail_deg` (0 = derecha, 90 = abajo)
void shape_path_bubble(cairo_t *cr, double x, double y, double w, double h, double tail_deg) {
    double t = tail_deg * M_PI / 180.0;
    double ct = cos(t), sn = sin(t);
    // El cuerpo se aparta de la cola, y la punta llega hasta el borde de la caja
    double cx = x + w / 2.0 - ct * w * 0.06, cy = y + h / 2.0 - sn * h * 0.06;
    double a = w / 2.0 * 0.80, b = h / 2.0 * 0.74;
    double spread = 14.0 * M_PI / 180.0;
    double ex = fabs(ct) > 1e-6 ? (w / 2.0) / fabs(ct) : 1e9;
    double ey = fabs(sn) > 1e-6 ? (h / 2.0) / fabs(sn) : 1e9;
    double reach = (ex < ey ? ex : ey) * 0.97;
    double tipx = x + w / 2.0 + ct * reach, tipy = y + h / 2.0 + sn * reach;
    const int steps = 72;
    double start = t + spread, end = t + 2.0 * M_PI - spread;

    cairo_new_sub_path(cr);
    for (int i = 0; i <= steps; i++) {
        double ang = start + (end - start) * i / steps;
        double px = cx + a * cos(ang), py = cy + b * sin(ang);
        if (i == 0) cairo_move_to(cr, px, py);
        else cairo_line_to(cr, px, py);
    }
    // La cola se curva un poco, como dibujada a mano
    double bx = cx + a * cos(t - spread), by = cy + b * sin(t - spread);
    double mx = (bx + tipx) / 2.0 + sin(t) * 6.0, my = (by + tipy) / 2.0 - cos(t) * 6.0;
    cairo_curve_to(cr, mx, my, mx, my, tipx, tipy);
    double ax = cx + a * cos(t + spread), ay = cy + b * sin(t + spread);
    cairo_line_to(cr, ax, ay);
    cairo_close_path(cr);
}

// Pseudoaleatorio estable (la forma no «tiembla» entre redibujados)
static double jitter(int i) {
    double v = sin(i * 12.9898 + 78.233) * 43758.5453;
    return v - floor(v);
}

// Estallido de onomatopeya: estrella irregular de `spikes` puntas
void shape_path_burst(cairo_t *cr, double x, double y, double w, double h, int spikes) {
    if (spikes < 5) spikes = 5;
    if (spikes > 32) spikes = 32;
    double cx = x + w / 2.0, cy = y + h / 2.0;
    double a = w / 2.0, b = h / 2.0;
    int n = spikes * 2;
    cairo_new_sub_path(cr);
    for (int i = 0; i < n; i++) {
        double ang = -M_PI / 2.0 + 2.0 * M_PI * i / n;
        double r = (i % 2 == 0) ? 1.0 - 0.10 * jitter(i) : 0.66 + 0.08 * jitter(i);
        double px = cx + a * r * cos(ang), py = cy + b * r * sin(ang);
        if (i == 0) cairo_move_to(cr, px, py);
        else cairo_line_to(cr, px, py);
    }
    cairo_close_path(cr);
}

// Nube de pensamiento: elipse con bultos
void shape_path_cloud(cairo_t *cr, double x, double y, double w, double h, int bumps) {
    if (bumps < 6) bumps = 6;
    if (bumps > 20) bumps = 20;
    double cx = x + w / 2.0, cy = y + h / 2.0;
    double a = w / 2.0 * 0.80, b = h / 2.0 * 0.76;
    double bulge = 0.30;
    cairo_new_sub_path(cr);
    for (int i = 0; i <= bumps; i++) {
        double t0 = 2.0 * M_PI * i / bumps;
        double px = cx + a * cos(t0), py = cy + b * sin(t0);
        if (i == 0) { cairo_move_to(cr, px, py); continue; }
        double tp = 2.0 * M_PI * (i - 1) / bumps;
        double c1x = cx + a * (1.0 + bulge * 1.6) * cos(tp + 0.12), c1y = cy + b * (1.0 + bulge * 1.6) * sin(tp + 0.12);
        double c2x = cx + a * (1.0 + bulge * 1.6) * cos(t0 - 0.12), c2y = cy + b * (1.0 + bulge * 1.6) * sin(t0 - 0.12);
        cairo_curve_to(cr, c1x, c1y, c2x, c2y, px, py);
    }
    cairo_close_path(cr);
}

/* --------------------------------------------- geometría de un elemento */

void element_path(cairo_t *cr, const AppEntry *a) {
    if (a->npts < 3) return;
    shape_path_rounded_polygon(cr, a->px, a->py, a->npts, a->style.radius);
}

// Hueco para el icono: centroide del polígono y su distancia al borde (círculo inscrito)
void element_content(const AppEntry *a, double *cx, double *cy, double *size) {
    poly_centroid(a, cx, cy);
    if (!poly_contains(a, *cx, *cy)) {   // polígono cóncavo: el centro de la caja
        *cx = a->x + a->w / 2;
        *cy = a->y + a->h / 2;
    }
    double r = poly_dist_to_edge(a, *cx, *cy);
    *size = 2.0 * r * 0.92 - 2.0 * a->style.padding;
    if (*size > 96) *size = 96;   // en viñetas grandes el icono no crece sin límite
    if (*size < 10) *size = 10;
}

/* ------------------------------------------------------------- dibujo */

static ColorRGBA mix(const ColorRGBA *a, double r, double g, double b, double t) {
    ColorRGBA c = { a->r + (r - a->r) * t, a->g + (g - a->g) * t, a->b + (b - a->b) * t, a->a };
    return c;
}

static void set_color(cairo_t *cr, const ColorRGBA *c) {
    cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);
}

static void fill_shape(cairo_t *cr, const ElementStyle *st, double x, double y, double w, double h) {
    const ColorRGBA *bg = &st->bg;
    switch (st->fill) {
        case FILL_GRADIENT:
        case FILL_GLOSS: {
            ColorRGBA top = mix(bg, 1, 1, 1, 0.22), bot = mix(bg, 0, 0, 0, 0.30);
            cairo_pattern_t *pat = cairo_pattern_create_linear(x, y, x, y + h);
            cairo_pattern_add_color_stop_rgba(pat, 0.0, top.r, top.g, top.b, bg->a);
            cairo_pattern_add_color_stop_rgba(pat, 1.0, bot.r, bot.g, bot.b, bg->a);
            cairo_set_source(cr, pat);
            cairo_fill_preserve(cr);
            cairo_pattern_destroy(pat);
            if (st->fill == FILL_GLOSS) {
                cairo_save(cr);
                cairo_clip_preserve(cr);
                cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x, y + h * 0.55);
                cairo_pattern_add_color_stop_rgba(g, 0.0, 1, 1, 1, 0.30);
                cairo_pattern_add_color_stop_rgba(g, 1.0, 1, 1, 1, 0.03);
                cairo_new_path(cr);
                cairo_move_to(cr, x - 10, y - 10);
                cairo_line_to(cr, x + w + 10, y - 10);
                cairo_line_to(cr, x + w + 10, y + h * 0.35);
                cairo_curve_to(cr, x + w * 0.6, y + h * 0.52, x + w * 0.4, y + h * 0.52, x - 10, y + h * 0.35);
                cairo_close_path(cr);
                cairo_set_source(cr, g);
                cairo_fill(cr);
                cairo_pattern_destroy(g);
                cairo_restore(cr);
            }
            break;
        }
        case FILL_HALFTONE: {
            set_color(cr, bg);
            cairo_fill_preserve(cr);
            // Trama de puntos de imprenta: grandes arriba a la izquierda, se desvanecen hacia abajo
            cairo_save(cr);
            cairo_clip_preserve(cr);
            cairo_new_path(cr);
            ColorRGBA dot = mix(bg, 1, 1, 1, 0.38);
            cairo_set_source_rgba(cr, dot.r, dot.g, dot.b, 0.85 * bg->a);
            const double step = 7.0;
            for (double yy = y; yy < y + h + step; yy += step) {
                int row = (int)((yy - y) / step);
                for (double xx = x + (row % 2) * step / 2.0; xx < x + w + step; xx += step) {
                    double t = ((xx - x) / w + (yy - y) / h) / 2.0;   // 0 arriba-izq, 1 abajo-der
                    double rad = 2.6 * (1.0 - t);
                    if (rad < 0.35) continue;
                    cairo_new_sub_path(cr);
                    cairo_arc(cr, xx, yy, rad, 0, 2 * M_PI);
                }
            }
            cairo_fill(cr);
            cairo_restore(cr);
            break;
        }
        case FILL_FLAT:
        default:
            set_color(cr, bg);
            cairo_fill_preserve(cr);
            break;
    }
}

static void stroke_border(cairo_t *cr, const ElementStyle *st, double x, double y, double w, double h) {
    double bw = st->border_width;
    if (st->border_style == BORDER_NONE || bw <= 0) { cairo_new_path(cr); return; }
    set_color(cr, &st->border);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, bw);
    switch (st->border_style) {
        case BORDER_DASHED: {
            double d[2] = { bw * 3.0, bw * 2.0 };
            cairo_set_dash(cr, d, 2, 0);
            cairo_stroke(cr);
            break;
        }
        case BORDER_DOTTED: {
            double d[2] = { 0.01, bw * 2.2 };
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_dash(cr, d, 2, 0);
            cairo_stroke(cr);
            break;
        }
        case BORDER_DOUBLE:
            cairo_set_line_width(cr, bw * 2.4);
            cairo_stroke_preserve(cr);
            set_color(cr, &st->bg);
            cairo_set_line_width(cr, bw * 0.8);
            cairo_stroke(cr);
            break;
        case BORDER_INK: {
            // Tinta de cómic: trazo grueso + un segundo trazo fino desplazado, como a pulso
            cairo_path_t *path = cairo_copy_path(cr);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_stroke(cr);
            cairo_save(cr);
            cairo_translate(cr, x + w / 2.0, y + h / 2.0);
            cairo_scale(cr, 1.0 + 1.6 / w, 1.0 - 1.2 / h);
            cairo_translate(cr, -(x + w / 2.0) + 0.9, -(y + h / 2.0) - 0.7);
            cairo_append_path(cr, path);
            cairo_restore(cr);
            cairo_set_line_width(cr, bw * 0.45);
            cairo_set_source_rgba(cr, st->border.r, st->border.g, st->border.b, st->border.a * 0.8);
            cairo_stroke(cr);
            cairo_path_destroy(path);
            break;
        }
        case BORDER_SOLID:
        default:
            cairo_stroke(cr);
            break;
    }
    cairo_set_dash(cr, NULL, 0, 0);
}

static void draw_label(cairo_t *cr, const ElementStyle *st, const char *font, const char *text,
                       double cx, double baseline) {
    if (!text || !text[0]) return;
    cairo_select_font_face(cr, font && font[0] ? font : "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, st->font_size);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text, &ext);
    double tx = cx - ext.width / 2.0 - ext.x_bearing;
    // Rotulado de cómic: letra con contorno de tinta
    cairo_move_to(cr, tx, baseline);
    cairo_text_path(cr, text);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, 3.2);
    cairo_set_source_rgba(cr, st->border.r, st->border.g, st->border.b, 0.95);
    cairo_stroke_preserve(cr);
    set_color(cr, &st->text);
    cairo_fill(cr);
}

void draw_element(cairo_t *cr, const AppEntry *a, const char *font, bool focused, bool pressed,
                  const ColorRGBA *glow) {
    if (a->npts < 3) return;
    const ElementStyle *st = &a->style;
    double x = a->x, y = a->y, w = a->w, h = a->h;
    cairo_save(cr);
    if (pressed) {
        cairo_translate(cr, x + w / 2.0, y + h / 2.0);
        cairo_scale(cr, 0.95, 0.95);
        cairo_translate(cr, -(x + w / 2.0), -(y + h / 2.0));
    }

    // 1. Sombra (fuera de la zona táctil: solo decorativa)
    if (st->shadow == SHADOW_HARD) {
        cairo_save(cr);
        cairo_translate(cr, 5.0, 6.0);
        element_path(cr, a);
        cairo_set_source_rgba(cr, st->border.r * 0.6, st->border.g * 0.6, st->border.b * 0.6, 0.95);
        cairo_fill(cr);
        cairo_restore(cr);
    } else if (st->shadow == SHADOW_SOFT) {
        for (int i = 3; i >= 1; i--) {
            cairo_save(cr);
            cairo_translate(cr, 1.0 + i * 0.6, 2.0 + i);
            element_path(cr, a);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.16);
            cairo_fill(cr);
            cairo_restore(cr);
        }
    }

    // 2. Relleno: color/trama y, encima, la imagen elegida recortada a la forma
    element_path(cr, a);
    fill_shape(cr, st, x, y, w, h);
    if (st->image[0]) {
        cairo_save(cr);
        cairo_new_path(cr);       // la trama consume el camino: se vuelve a trazar
        element_path(cr, a);
        cairo_clip_preserve(cr);   // image_fill_cover vuelve a recortar con este mismo trazado
        if (image_is_builtin(st->image)) {
            double p = st->padding * 0.5;
            image_draw_builtin_element(cr, st->image, x + p, y + p, w - 2 * p, h - 2 * p, &st->bg, &st->border);
        } else {
            cairo_surface_t *img = image_get(st->image, (int)(w * 2), (int)(h * 2));
            if (img) image_fill_cover(cr, img, x, y, w, h);
        }
        cairo_restore(cr);
    }

    // 3. Borde
    cairo_new_path(cr);
    element_path(cr, a);
    stroke_border(cr, st, x, y, w, h);

    // 4. Icono del programa y nombre
    double icx, icy, isz;
    element_content(a, &icx, &icy, &isz);
    if (st->label == LABEL_INSIDE) {
        icy -= st->font_size * 0.7;
        isz -= st->font_size * 1.4;
        if (isz < 10) isz = 10;
    }
    isz *= st->icon_scale;
    if (!icon_draw_for_app(cr, a->icon, a->exec, icx, icy, isz, st->text.r, st->text.g, st->text.b)) {
        draw_icon_placeholder(cr, icx, icy, isz * 0.9, a->icon, &st->text);
    }
    if (st->label == LABEL_BELOW) {
        draw_label(cr, st, font, a->name, x + w / 2.0, y + h + st->font_size + 3.0);
    } else if (st->label == LABEL_INSIDE) {
        draw_label(cr, st, font, a->name, icx, icy + isz / 2.0 + st->font_size + 2.0);
    }
    cairo_restore(cr);

    if (focused) element_draw_focus(cr, a, glow, false);
}

void element_draw_focus(cairo_t *cr, const AppEntry *a, const ColorRGBA *glow, bool pressed) {
    if (a->npts < 3 || !glow) return;
    cairo_save(cr);
    element_path(cr, a);
    if (pressed) {
        cairo_set_source_rgba(cr, 0, 0, 0, 0.30);
        cairo_fill_preserve(cr);
    }
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_source_rgba(cr, glow->r, glow->g, glow->b, 0.35);
    cairo_set_line_width(cr, a->style.border_width + 12.0);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgba(cr, glow->r, glow->g, glow->b, 0.95);
    cairo_set_line_width(cr, a->style.border_width + 4.0);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void draw_background_wiiu(cairo_t *cr, int width, int height,
                          const ColorRGBA *c_base, const ColorRGBA *c_grad) {
    // Degradado radial oscuro sutil desde el centro superior
    cairo_pattern_t *pat = cairo_pattern_create_radial(width * 0.5, height * 0.35, 30.0,
                                                       width * 0.5, height * 0.5, width * 0.7);
    cairo_pattern_add_color_stop_rgba(pat, 0.0, c_grad->r, c_grad->g, c_grad->b, 1.0);
    cairo_pattern_add_color_stop_rgba(pat, 1.0, c_base->r, c_base->g, c_base->b, 1.0);
    cairo_set_source(cr, pat);
    cairo_paint(cr);
    cairo_pattern_destroy(pat);

    // Patrón geométrico sutil estilo cuadrícula punteada de la interfaz Wii U
    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.6, 0.4, 0.9, 0.035);
    for (int y = 20; y < height; y += 32) {
        for (int x = 20; x < width; x += 32) {
            cairo_arc(cr, x, y, 1.5, 0, 2.0 * M_PI);
            cairo_fill(cr);
        }
    }
    cairo_restore(cr);
}

void draw_icon_placeholder(cairo_t *cr, double cx, double cy, double size, const char *icon_name, const ColorRGBA *fg) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, fg->r, fg->g, fg->b, fg->a);
    cairo_set_line_width(cr, 2.4);

    double r = size / 2.0;

    if (strstr(icon_name, "terminal")) {
        // Prompt >_
        cairo_move_to(cr, cx - r * 0.6, cy - r * 0.5);
        cairo_line_to(cr, cx - r * 0.1, cy);
        cairo_line_to(cr, cx - r * 0.6, cy + r * 0.5);
        cairo_stroke(cr);
        cairo_move_to(cr, cx + r * 0.1, cy + r * 0.5);
        cairo_line_to(cr, cx + r * 0.6, cy + r * 0.5);
        cairo_stroke(cr);
    } else if (strstr(icon_name, "camera")) {
        // Cámara Wii U
        cairo_arc(cr, cx, cy, r * 0.4, 0, 2 * M_PI);
        cairo_stroke(cr);
        shape_path_rounded_rect(cr, cx - r * 0.75, cy - r * 0.55, r * 1.5, r * 1.1, 4.0);
        cairo_stroke(cr);
        cairo_arc(cr, cx + r * 0.4, cy - r * 0.25, 2.0, 0, 2 * M_PI);
        cairo_fill(cr);
    } else if (strstr(icon_name, "web") || strstr(icon_name, "browser")) {
        // Globo terráqueo
        cairo_arc(cr, cx, cy, r * 0.75, 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_move_to(cr, cx - r * 0.75, cy);
        cairo_line_to(cr, cx + r * 0.75, cy);
        cairo_stroke(cr);
        cairo_save(cr);
        cairo_scale(cr, 0.45, 1.0);
        cairo_arc(cr, cx / 0.45, cy, r * 0.75, 0, 2 * M_PI);
        cairo_stroke(cr);
        cairo_restore(cr);
    } else if (strstr(icon_name, "player") || strstr(icon_name, "media")) {
        // Símbolo Play
        cairo_move_to(cr, cx - r * 0.4, cy - r * 0.6);
        cairo_line_to(cr, cx + r * 0.6, cy);
        cairo_line_to(cr, cx - r * 0.4, cy + r * 0.6);
        cairo_close_path(cr);
        cairo_fill(cr);
    } else if (strstr(icon_name, "gaming") || strstr(icon_name, "game")) {
        // D-Pad y botones
        shape_path_rounded_rect(cr, cx - r * 0.7, cy - r * 0.45, r * 1.4, r * 0.9, 6.0);
        cairo_stroke(cr);
        cairo_arc(cr, cx + r * 0.35, cy, 2.5, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_arc(cr, cx + r * 0.2, cy - r * 0.15, 2.5, 0, 2 * M_PI);
        cairo_fill(cr);
    } else if (strstr(icon_name, "preferences") || strstr(icon_name, "settings")) {
        // Engranaje
        cairo_arc(cr, cx, cy, r * 0.4, 0, 2 * M_PI);
        cairo_stroke(cr);
        for (int i = 0; i < 8; i++) {
            double angle = i * (M_PI / 4.0);
            cairo_move_to(cr, cx + cos(angle) * (r * 0.5), cy + sin(angle) * (r * 0.5));
            cairo_line_to(cr, cx + cos(angle) * (r * 0.8), cy + sin(angle) * (r * 0.8));
            cairo_stroke(cr);
        }
    } else {
        // Icono genérico (Rombo / Cubo)
        cairo_save(cr);
        cairo_translate(cr, cx, cy);
        cairo_rotate(cr, M_PI / 4.0);
        shape_path_rounded_rect(cr, -r * 0.45, -r * 0.45, r * 0.9, r * 0.9, 4.0);
        cairo_stroke(cr);
        cairo_restore(cr);
    }

    cairo_restore(cr);
}
