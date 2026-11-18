#ifndef RECENTS_VIEW_H
#define RECENTS_VIEW_H

#include "config.h"
#include <cairo.h>
#include <stdbool.h>

#define MAX_RECENT_ITEMS 32

typedef struct {
    char name[128];       // título de la tarjeta (nombre de la app o de la ventana)
    char subtitle[128];   // título de la ventana / app_id
    char icon[MAX_STR];
    char exec[MAX_STR];   // para encontrar el icono del programa
    bool active;          // es la ventana que estaba en primer plano
    bool tiled;           // está en mosaico (botón de la esquina)
    int workspace;        // escritorio (1..9; 0 = sin escritorios)
} RecentItem;

typedef struct {
    const DesktopConfig *cfg;
    RecentItem items[MAX_RECENT_ITEMS];
    int count;
    int focused_card;
    int pressed_card;
    double scroll_x;
} RecentsView;

void recents_view_init(RecentsView *rv, const DesktopConfig *cfg);
// Sustituye las tarjetas; `focus_active` enfoca la ventana activa (al abrir Recientes)
void recents_view_set_items(RecentsView *rv, const RecentItem *items, int count, bool focus_active);
bool recents_view_update(RecentsView *rv, int width);
void recents_view_render(RecentsView *rv, cairo_t *cr, int width, int height);

void recents_view_nav(RecentsView *rv, int dir);
// Devuelve la tarjeta tocada (-1 nada, -2 botón Inicio); indica si fue en la × o en el botón de mosaico
int recents_view_hit_test(RecentsView *rv, double x, double y, int width, int height, bool *out_close_btn,
                          bool *out_tile_btn);

#endif // RECENTS_VIEW_H
