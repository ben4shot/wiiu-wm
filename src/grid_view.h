#ifndef GRID_VIEW_H
#define GRID_VIEW_H

#include "config.h"
#include "page_cache.h"
#include <cairo.h>
#include <stdbool.h>

typedef struct {
    const DesktopConfig *cfg;
    const PageCache *cache;     // páginas pre-renderizadas (NULL = dibujar en vivo)
    int current_page;
    int total_pages;
    int focused_index;
    int pressed_index;
    double scroll_offset_x; // Para animaciones de cambio de página
} GridView;

void grid_view_init(GridView *gv, const DesktopConfig *cfg);
// Tras recargar la configuración (páginas y foco válidos)
void grid_view_reload(GridView *gv);
// Dibuja los elementos de una página (lo usa también la vista previa del editor)
void grid_view_render_page(const DesktopConfig *cfg, cairo_t *cr, int page, double offset_x,
                           int focused, int pressed);
int grid_view_hit_test_page(const DesktopConfig *cfg, int page, double x, double y);
bool grid_view_update(GridView *gv, int width);
void grid_view_render(GridView *gv, cairo_t *cr, int width, int height);

void grid_view_nav(GridView *gv, int dx, int dy);
void grid_view_next_page(GridView *gv);
void grid_view_prev_page(GridView *gv);

int grid_view_hit_test(GridView *gv, double x, double y, int width, int height);

#endif // GRID_VIEW_H
