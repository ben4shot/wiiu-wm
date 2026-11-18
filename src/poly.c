#include "poly.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MIN_AREA 900.0      // ~30×30 px: algo que se pueda tocar con el dedo
#define MIN_EDGE 6.0

static const char *PRESET_LABELS[PRESET_COUNT] = {
    "Rectángulo", "Viñeta ◢", "Viñeta ◣", "Trapecio",
    "Triángulo ▲", "Triángulo ▼", "Triángulo ◀", "Triángulo ▶",
    "Rombo", "Hexágono", "Octágono"
};

const char *poly_preset_label(PolyPreset p) {
    return (p >= 0 && p < PRESET_COUNT) ? PRESET_LABELS[p] : "";
}

static void set_pts(AppEntry *a, const double *u, const double *v, int n,
                    double x, double y, double w, double h) {
    a->npts = n;
    for (int i = 0; i < n; i++) {
        a->px[i] = x + u[i] * w;
        a->py[i] = y + v[i] * h;
    }
    app_update_bbox(a);
}

void poly_set_preset(AppEntry *a, PolyPreset p, double x, double y, double w, double h) {
    switch (p) {
        case PRESET_PANEL_L: {
            double u[] = { 0.10, 1.00, 0.92, 0.00 }, v[] = { 0.00, 0.06, 1.00, 0.92 };
            set_pts(a, u, v, 4, x, y, w, h); break;
        }
        case PRESET_PANEL_R: {
            double u[] = { 0.00, 0.90, 1.00, 0.08 }, v[] = { 0.06, 0.00, 0.94, 1.00 };
            set_pts(a, u, v, 4, x, y, w, h); break;
        }
        case PRESET_TRAPEZOID: {
            double u[] = { 0.14, 0.86, 1.00, 0.00 }, v[] = { 0, 0, 1, 1 };
            set_pts(a, u, v, 4, x, y, w, h); break;
        }
        case PRESET_TRI_UP: {
            double u[] = { 0.5, 1, 0 }, v[] = { 0, 1, 1 };
            set_pts(a, u, v, 3, x, y, w, h); break;
        }
        case PRESET_TRI_DOWN: {
            double u[] = { 0, 1, 0.5 }, v[] = { 0, 0, 1 };
            set_pts(a, u, v, 3, x, y, w, h); break;
        }
        case PRESET_TRI_LEFT: {
            double u[] = { 0, 1, 1 }, v[] = { 0.5, 0, 1 };
            set_pts(a, u, v, 3, x, y, w, h); break;
        }
        case PRESET_TRI_RIGHT: {
            double u[] = { 0, 1, 0 }, v[] = { 0, 0.5, 1 };
            set_pts(a, u, v, 3, x, y, w, h); break;
        }
        case PRESET_DIAMOND: {
            double u[] = { 0.5, 1, 0.5, 0 }, v[] = { 0, 0.5, 1, 0.5 };
            set_pts(a, u, v, 4, x, y, w, h); break;
        }
        case PRESET_HEXAGON: {
            double u[] = { 0.25, 0.75, 1, 0.75, 0.25, 0 }, v[] = { 0, 0, 0.5, 1, 1, 0.5 };
            set_pts(a, u, v, 6, x, y, w, h); break;
        }
        case PRESET_OCTAGON: {
            double k = 0.29;
            double u[] = { k, 1 - k, 1, 1, 1 - k, k, 0, 0 }, v[] = { 0, 0, k, 1 - k, 1, 1, 1 - k, k };
            set_pts(a, u, v, 8, x, y, w, h); break;
        }
        case PRESET_RECT:
        default: {
            double u[] = { 0, 1, 1, 0 }, v[] = { 0, 0, 1, 1 };
            set_pts(a, u, v, 4, x, y, w, h); break;
        }
    }
}

PolyPreset poly_preset_for_shape(TileShape s) {
    switch (s) {
        case SHAPE_TRIANGLE_UP: return PRESET_TRI_UP;
        case SHAPE_TRIANGLE_DOWN: return PRESET_TRI_DOWN;
        case SHAPE_TRIANGLE_LEFT: return PRESET_TRI_LEFT;
        case SHAPE_TRIANGLE_RIGHT: return PRESET_TRI_RIGHT;
        case SHAPE_HEXAGON: return PRESET_HEXAGON;
        case SHAPE_PANEL: return PRESET_PANEL_L;
        case SHAPE_CIRCLE: return PRESET_OCTAGON;
        default: return PRESET_RECT;
    }
}

/* ------------------------------------------------------------ medidas */

bool poly_contains(const AppEntry *a, double x, double y) {
    bool in = false;
    for (int i = 0, j = a->npts - 1; i < a->npts; j = i++) {
        if (((a->py[i] > y) != (a->py[j] > y)) &&
            (x < (a->px[j] - a->px[i]) * (y - a->py[i]) / (a->py[j] - a->py[i]) + a->px[i])) {
            in = !in;
        }
    }
    return in;
}

double poly_area(const AppEntry *a) {
    double s = 0;
    for (int i = 0, j = a->npts - 1; i < a->npts; j = i++) s += a->px[j] * a->py[i] - a->px[i] * a->py[j];
    return fabs(s) / 2.0;
}

void poly_centroid(const AppEntry *a, double *cx, double *cy) {
    double s = 0, x = 0, y = 0;
    for (int i = 0, j = a->npts - 1; i < a->npts; j = i++) {
        double c = a->px[j] * a->py[i] - a->px[i] * a->py[j];
        s += c;
        x += (a->px[j] + a->px[i]) * c;
        y += (a->py[j] + a->py[i]) * c;
    }
    if (fabs(s) < 1e-9) {
        *cx = a->x + a->w / 2;
        *cy = a->y + a->h / 2;
        return;
    }
    *cx = x / (3.0 * s);
    *cy = y / (3.0 * s);
}

static double seg_point_dist(double ax, double ay, double bx, double by, double px, double py) {
    double dx = bx - ax, dy = by - ay;
    double len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return hypot(ax + t * dx - px, ay + t * dy - py);
}

double poly_dist_to_edge(const AppEntry *a, double x, double y) {
    double best = 1e18;
    for (int i = 0, j = a->npts - 1; i < a->npts; j = i++) {
        double d = seg_point_dist(a->px[j], a->py[j], a->px[i], a->py[i], x, y);
        if (d < best) best = d;
    }
    return best;
}

static double cross3(double ax, double ay, double bx, double by, double cx, double cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static bool segs_intersect(double a1x, double a1y, double a2x, double a2y,
                           double b1x, double b1y, double b2x, double b2y) {
    double d1 = cross3(b1x, b1y, b2x, b2y, a1x, a1y);
    double d2 = cross3(b1x, b1y, b2x, b2y, a2x, a2y);
    double d3 = cross3(a1x, a1y, a2x, a2y, b1x, b1y);
    double d4 = cross3(a1x, a1y, a2x, a2y, b2x, b2y);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

static double seg_seg_dist(double a1x, double a1y, double a2x, double a2y,
                           double b1x, double b1y, double b2x, double b2y) {
    if (segs_intersect(a1x, a1y, a2x, a2y, b1x, b1y, b2x, b2y)) return 0;
    double d = seg_point_dist(a1x, a1y, a2x, a2y, b1x, b1y);
    double t;
    if ((t = seg_point_dist(a1x, a1y, a2x, a2y, b2x, b2y)) < d) d = t;
    if ((t = seg_point_dist(b1x, b1y, b2x, b2y, a1x, a1y)) < d) d = t;
    if ((t = seg_point_dist(b1x, b1y, b2x, b2y, a2x, a2y)) < d) d = t;
    return d;
}

bool poly_is_simple(const AppEntry *a) {
    int n = a->npts;
    if (n < 3 || n > MAX_POLY) return false;
    for (int i = 0; i < n; i++) {
        int i2 = (i + 1) % n;
        if (hypot(a->px[i2] - a->px[i], a->py[i2] - a->py[i]) < MIN_EDGE) return false;
        for (int j = i + 1; j < n; j++) {
            int j2 = (j + 1) % n;
            if (j == i2 || j2 == i) continue;   // lados contiguos
            if (segs_intersect(a->px[i], a->py[i], a->px[i2], a->py[i2],
                               a->px[j], a->py[j], a->px[j2], a->py[j2])) return false;
        }
    }
    return poly_area(a) >= MIN_AREA;
}

double poly_separation(const AppEntry *a, const AppEntry *b) {
    // Cajas muy alejadas: no hace falta mirar lado a lado
    double gap_x = fmax(b->x - (a->x + a->w), a->x - (b->x + b->w));
    double gap_y = fmax(b->y - (a->y + a->h), a->y - (b->y + b->h));
    if (gap_x > 64 || gap_y > 64) return fmax(gap_x, gap_y);

    if (poly_contains(a, b->px[0], b->py[0]) || poly_contains(b, a->px[0], a->py[0])) return 0;
    double best = 1e18;
    for (int i = 0, i0 = a->npts - 1; i < a->npts; i0 = i++) {
        for (int j = 0, j0 = b->npts - 1; j < b->npts; j0 = j++) {
            double d = seg_seg_dist(a->px[i0], a->py[i0], a->px[i], a->py[i],
                                    b->px[j0], b->py[j0], b->px[j], b->py[j]);
            if (d < best) best = d;
            if (best == 0) return 0;
        }
    }
    return best;
}

/* ------------------------------------------------------- transformaciones */

void poly_translate(AppEntry *a, double dx, double dy) {
    for (int i = 0; i < a->npts; i++) { a->px[i] += dx; a->py[i] += dy; }
    app_update_bbox(a);
}

void poly_scale(AppEntry *a, double sx, double sy) {
    double cx, cy;
    poly_centroid(a, &cx, &cy);
    for (int i = 0; i < a->npts; i++) {
        a->px[i] = cx + (a->px[i] - cx) * sx;
        a->py[i] = cy + (a->py[i] - cy) * sy;
    }
    app_update_bbox(a);
}

void poly_rotate(AppEntry *a, double degrees) {
    double cx, cy, t = degrees * M_PI / 180.0, c = cos(t), s = sin(t);
    poly_centroid(a, &cx, &cy);
    for (int i = 0; i < a->npts; i++) {
        double dx = a->px[i] - cx, dy = a->py[i] - cy;
        a->px[i] = cx + dx * c - dy * s;
        a->py[i] = cy + dx * s + dy * c;
    }
    app_update_bbox(a);
}

int poly_add_vertex(AppEntry *a, int after) {
    if (a->npts >= MAX_POLY) return -1;
    int e = after;
    if (e < 0 || e >= a->npts) {
        double best = -1;
        for (int i = 0; i < a->npts; i++) {
            int j = (i + 1) % a->npts;
            double l = hypot(a->px[j] - a->px[i], a->py[j] - a->py[i]);
            if (l > best) { best = l; e = i; }
        }
    }
    int j = (e + 1) % a->npts;
    double mx = (a->px[e] + a->px[j]) / 2, my = (a->py[e] + a->py[j]) / 2;
    for (int k = a->npts; k > e + 1; k--) { a->px[k] = a->px[k - 1]; a->py[k] = a->py[k - 1]; }
    a->px[e + 1] = mx;
    a->py[e + 1] = my;
    a->npts++;
    app_update_bbox(a);
    return e + 1;
}

bool poly_remove_vertex(AppEntry *a, int idx) {
    if (a->npts <= 3 || idx < 0 || idx >= a->npts) return false;
    for (int k = idx; k < a->npts - 1; k++) { a->px[k] = a->px[k + 1]; a->py[k] = a->py[k + 1]; }
    a->npts--;
    app_update_bbox(a);
    return true;
}

/* ------------------------------------------------------------- maqueta */

bool layout_bar_poly(const DesktopConfig *cfg, AppEntry *out) {
    if (!cfg->bar_enabled) return false;
    memset(out, 0, sizeof(*out));
    poly_set_preset(out, PRESET_RECT, cfg->bar_x, cfg->bar_y, cfg->bar_w, cfg->bar_h);
    return true;
}

int layout_check_bar(const DesktopConfig *cfg, int page) {
    AppEntry bar;
    if (!layout_bar_poly(cfg, &bar)) return -1;
    if (cfg->bar_x < 0 || cfg->bar_y < 0 || cfg->bar_x + cfg->bar_w > cfg->screen_width + 0.5 ||
        cfg->bar_y + cfg->bar_h > cfg->screen_height + 0.5) return -2;
    for (int k = 0; k < cfg->num_apps; k++) {
        if (page >= 0 && cfg->apps[k].page != page) continue;
        if (poly_separation(&bar, &cfg->apps[k]) < cfg->gutter) return k;
    }
    return -1;
}

int layout_check(const DesktopConfig *cfg, int idx) {
    const AppEntry *a = &cfg->apps[idx];
    if (!poly_is_simple(a)) return -2;
    for (int i = 0; i < a->npts; i++) {
        if (a->px[i] < 0 || a->py[i] < 0 || a->px[i] > cfg->screen_width || a->py[i] > cfg->screen_height) return -2;
    }
    for (int k = 0; k < cfg->num_apps; k++) {
        if (k == idx || cfg->apps[k].page != a->page) continue;
        if (poly_separation(a, &cfg->apps[k]) < cfg->gutter) return k;
    }
    // La barra está en todas las páginas
    AppEntry bar;
    if (layout_bar_poly(cfg, &bar) && poly_separation(a, &bar) < cfg->gutter) return LAYOUT_HITS_BAR;
    return -1;
}

bool layout_find_free(const DesktopConfig *cfg, int ignore_idx, int page, double w, double h,
                      double *out_x, double *out_y) {
    AppEntry probe;
    memset(&probe, 0, sizeof(probe));
    probe.page = page;
    double top = 40;   // bajo la barra superior
    for (double y = top; y + h <= cfg->screen_height - 8; y += 8) {
        for (double x = 8; x + w <= cfg->screen_width - 8; x += 8) {
            poly_set_preset(&probe, PRESET_RECT, x, y, w, h);
            bool ok = true;
            AppEntry bar;
            if (layout_bar_poly(cfg, &bar) && poly_separation(&probe, &bar) < cfg->gutter) ok = false;
            for (int k = 0; k < cfg->num_apps && ok; k++) {
                if (k == ignore_idx || cfg->apps[k].page != page) continue;
                if (poly_separation(&probe, &cfg->apps[k]) < cfg->gutter) ok = false;
            }
            if (ok) { *out_x = x; *out_y = y; return true; }
        }
    }
    return false;
}
