#ifndef ICONS_H
#define ICONS_H

// Iconos reales de los programas: entrada .desktop (Icon=) + tema de iconos freedesktop
// (hicolor, Adwaita, el tema por defecto...). PNG con Cairo; SVG con librsvg cargada
// en tiempo de ejecución (dlopen), así no hace falta librsvg2-dev para compilar.

#include <cairo.h>
#include <stdbool.h>

typedef struct {
    char name[128];   // Name= del .desktop
    char exec[256];   // Exec= sin los %f/%u...
    char icon[128];   // Icon=
    bool terminal;    // Terminal=true: se abre dentro de una terminal
} DesktopEntry;

// Busca la entrada .desktop de un ejecutable (por nombre del binario o del fichero .desktop)
bool desktop_entry_for_exec(const char *exec, DesktopEntry *out);
// Lista las aplicaciones instaladas (para «Añadir app» en el editor). Devuelve cuántas.
int desktop_entries_list(DesktopEntry *out, int max);

// Resuelve un nombre de icono (o ruta) a un fichero; prefiere iconos a color frente a *-symbolic.
bool icon_find_file(const char *name, char *out_path, int out_len, bool *is_symbolic);

// Dibuja el icono de una app centrado en (cx,cy) con lado `size`.
// Orden: ruta absoluta configurada -> icono del .desktop del programa -> nombre de icono
// configurado -> nombre del ejecutable.
// Los iconos simbólicos se tiñen con `tint`. Devuelve false si no encontró ninguno.
bool icon_draw_for_app(cairo_t *cr, const char *config_icon, const char *exec,
                       double cx, double cy, double size, double tint_r, double tint_g, double tint_b);

// Fichero de icono que se usaría para una app ("" si ninguno)
bool icon_resolve(const char *config_icon, const char *exec, char *path, int len, bool *is_symbolic);

void icons_clear_cache(void);

#endif // ICONS_H
