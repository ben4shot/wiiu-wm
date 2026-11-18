#ifndef POLY_H
#define POLY_H

// Geometría de la zona táctil (= forma) de cada elemento: un polígono editable.
// Reglas de la maqueta tipo viñetas de cómic:
//   - el polígono es simple (sin cruzarse) y tiene un área mínima;
//   - queda dentro de la pantalla;
//   - entre dos elementos de la misma página hay al menos `gutter` píxeles (la «calle»).

#include "config.h"
#include <stdbool.h>

typedef enum {
    PRESET_RECT = 0,
    PRESET_PANEL_L,       // viñeta inclinada hacia la izquierda
    PRESET_PANEL_R,       // viñeta inclinada hacia la derecha
    PRESET_TRAPEZOID,
    PRESET_TRI_UP, PRESET_TRI_DOWN, PRESET_TRI_LEFT, PRESET_TRI_RIGHT,
    PRESET_DIAMOND,
    PRESET_HEXAGON,
    PRESET_OCTAGON,
    PRESET_COUNT
} PolyPreset;

const char *poly_preset_label(PolyPreset p);
// Rellena el polígono del elemento con un preajuste inscrito en la caja (x, y, w, h)
void poly_set_preset(AppEntry *a, PolyPreset p, double x, double y, double w, double h);
// Preajuste equivalente a una forma antigua (migración de config.json)
PolyPreset poly_preset_for_shape(TileShape s);

bool poly_contains(const AppEntry *a, double x, double y);
double poly_area(const AppEntry *a);
void poly_centroid(const AppEntry *a, double *cx, double *cy);
// Distancia de un punto al contorno
double poly_dist_to_edge(const AppEntry *a, double x, double y);
bool poly_is_simple(const AppEntry *a);
// Distancia mínima entre dos polígonos (0 si se tocan, cruzan o uno contiene al otro)
double poly_separation(const AppEntry *a, const AppEntry *b);

void poly_translate(AppEntry *a, double dx, double dy);
void poly_scale(AppEntry *a, double sx, double sy);      // alrededor del centroide
void poly_rotate(AppEntry *a, double degrees);           // alrededor del centroide
// Añade un vértice en el punto medio del lado más largo (o tras `after` si >= 0). Devuelve su índice.
int poly_add_vertex(AppEntry *a, int after);
bool poly_remove_vertex(AppEntry *a, int idx);

#define LAYOUT_HITS_BAR (-3)   // layout_check: choca con la barra

// Comprueba si el elemento `idx` (con su polígono actual) cabe en su página.
// Devuelve -1 si es válido, el índice del elemento con el que choca, o -2 si el
// polígono es inválido o se sale de la pantalla, o LAYOUT_HITS_BAR si pisa la barra.
int layout_check(const DesktopConfig *cfg, int idx);
// La barra como polígono (rectángulo); false si está desactivada
bool layout_bar_poly(const DesktopConfig *cfg, AppEntry *out);
// -1 si la barra cabe sin pisar ningún elemento; si no, el índice del primero con el que choca
int layout_check_bar(const DesktopConfig *cfg, int page);
// Busca hueco libre de w×h en `page` (rejilla de 8 px). Devuelve false si no cabe.
bool layout_find_free(const DesktopConfig *cfg, int ignore_idx, int page, double w, double h,
                      double *out_x, double *out_y);

#endif // POLY_H
