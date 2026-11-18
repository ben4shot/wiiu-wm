#ifndef WM_CAPTURE_H
#define WM_CAPTURE_H

#include <stdbool.h>

struct wm_server;
struct wlr_buffer;
typedef struct pixman_region32 pixman_region32_t;

/* Entrega de la imagen renderizada a drc_stream (sustituye a wf-recorder).
 * El WM escucha en $XDG_RUNTIME_DIR/wiiu-wm-frames; drc_stream conecta y recibe un flujo y4m
 * (YUV420, 854x480, rango limitado) generado a partir del búfer que el propio WM acaba de dibujar:
 * solo se lee y convierte cuando hay daño, y se reenvía el último fotograma a 60 Hz. */
void wm_capture_init(struct wm_server *s);
void wm_capture_finish(struct wm_server *s);
/* Hay un cliente conectado: conviene leer cada búfer renderizado */
bool wm_capture_wanted(struct wm_server *s);
/* Búfer recién renderizado (antes del commit) */
/* damage: región cambiada respecto al fotograma anterior (NULL = no se sabe: leer todo). Solo se lee y
 * convierte esa zona; si no hay cambios no se hace nada ni se envía nada. */
void wm_capture_buffer(struct wm_server *s, struct wlr_buffer *buf, const pixman_region32_t *damage);

/* Diagnóstico (orden de control «capture_dump <fichero>»): vuelca el último fotograma entregado
 * como y4m de un solo fotograma. «capture_redraw» fuerza un redibujado completo. */
void wm_capture_dump(struct wm_server *s, const char *path);
void wm_capture_redraw(struct wm_server *s);

#endif
