#ifndef IMAGES_H
#define IMAGES_H

// Imágenes elegidas por el usuario (fondo de pantalla y fondo de cada elemento).
// JPG/PNG/WebP/SVG... con gdk-pixbuf cargada en ejecución (dlopen); si no está, PNG con Cairo.
// Además hay imágenes «incluidas» (builtin:...) que se dibujan con vectores.

#include "config.h"
#include <cairo.h>
#include <stdbool.h>

#define MAX_IMAGE_LIST 96

// Imagen escalada para caber en max_w×max_h (conserva proporción). En caché: no liberar.
cairo_surface_t *image_get(const char *path, int max_w, int max_h);
void images_clear_cache(void);

bool image_is_builtin(const char *path);
// Nombre para mostrar: «Estallido», «playa.jpg»...
void image_display_name(const char *path, char *out, int out_len);

// Lista para el selector: "" (ninguna), las incluidas y los ficheros encontrados.
// `wallpaper` elige las incluidas de fondo de pantalla o las de elemento.
int image_list(char (*out)[MAX_STR], int max, bool wallpaper);
// Carpeta donde el usuario puede dejar sus imágenes (se crea si no existe)
void image_user_dir(char *out, int out_len);

// Dibuja una imagen incluida de elemento (estallido, bocadillo, nube) dentro de la caja
void image_draw_builtin_element(cairo_t *cr, const char *name, double x, double y, double w, double h,
                                const ColorRGBA *bg, const ColorRGBA *ink);
// Dibuja el fondo de pantalla (imagen «cover», incluida, o el degradado por defecto)
void image_draw_wallpaper(cairo_t *cr, const char *wallpaper, int width, int height,
                          const ColorRGBA *base, const ColorRGBA *grad, const ColorRGBA *accent);
// Rellena el camino actual con la imagen ajustada a la caja (modo «cover»), sin consumir el camino
void image_fill_cover(cairo_t *cr, cairo_surface_t *img, double x, double y, double w, double h);

#endif // IMAGES_H
