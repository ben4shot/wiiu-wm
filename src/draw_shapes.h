#ifndef DRAW_SHAPES_H
#define DRAW_SHAPES_H

#include "config.h"
#include <cairo.h>
#include <stdbool.h>

// Caminos geométricos básicos
void shape_path_squircle(cairo_t *cr, double x, double y, double w, double h, double r);
void shape_path_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r);
void shape_path_circle(cairo_t *cr, double x, double y, double w, double h);
void shape_path_triangle(cairo_t *cr, double x, double y, double w, double h, TileShape dir, double r);
void shape_path_hexagon(cairo_t *cr, double x, double y, double w, double h, double r);
void shape_path_panel(cairo_t *cr, double x, double y, double w, double h, double slant, double r);
void shape_path_bubble(cairo_t *cr, double x, double y, double w, double h, double tail_deg);
void shape_path_burst(cairo_t *cr, double x, double y, double w, double h, int spikes);
void shape_path_cloud(cairo_t *cr, double x, double y, double w, double h, int bumps);

// Camino del polígono del elemento (con las esquinas redondeadas de su estilo)
void element_path(cairo_t *cr, const AppEntry *a);
// Centro y lado del icono dentro del polígono (descontado el padding)
void element_content(const AppEntry *a, double *cx, double *cy, double *size);

// Dibuja un elemento completo: sombra, relleno/imagen, borde, icono y nombre
void draw_element(cairo_t *cr, const AppEntry *a, const char *font, bool focused, bool pressed,
                  const ColorRGBA *glow);
// Solo el resalte de foco (y el oscurecido al pulsar): se pinta sobre la página pre-renderizada
void element_draw_focus(cairo_t *cr, const AppEntry *a, const ColorRGBA *glow, bool pressed);

void draw_background_wiiu(cairo_t *cr, int width, int height,
                          const ColorRGBA *c_base, const ColorRGBA *c_grad);
void draw_icon_placeholder(cairo_t *cr, double cx, double cy, double size, const char *icon_name, const ColorRGBA *fg);

#endif // DRAW_SHAPES_H
