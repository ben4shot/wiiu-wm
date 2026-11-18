#ifndef EDITOR_H
#define EDITOR_H

// Editor del menú: una app más («wiiu-shell --editor»). Vista previa idéntica al Inicio,
// panel de propiedades y barra de herramientas. Guarda solo (autoguardado) en la
// configuración del usuario; el shell la vigila y se recarga en vivo.

#include "config.h"
#include "icons.h"
#include "input_gamepad.h"
#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>

#define EDITOR_UNDO 16
#define EDITOR_MAX_ENTRIES 160

#include "images.h"

typedef DesktopConfig EditorSnapshot;   // deshacer guarda la configuración entera

typedef enum { ED_DRAG_NONE = 0, ED_DRAG_ELEMENT, ED_DRAG_VERTEX, ED_DRAG_BAR, ED_DRAG_BAR_RESIZE, ED_DRAG_PANEL, ED_DRAG_PICKER, ED_DRAG_BUTTON, ED_DRAG_COLOR } EditorDrag;
typedef enum { PICK_NONE = 0, PICK_APP, PICK_IMAGE, PICK_WALLPAPER, PICK_COLOR } PickerMode;

typedef struct {
    DesktopConfig *cfg;
    char save_path[MAX_STR];
    int width, height;

    int selected;          // índice del elemento (-1 = ninguno: panel de página/fondo)
    int vertex;            // vértice seleccionado del elemento (-1 = ninguno)
    bool bar_selected;     // la barra (waybar) está seleccionada en vez de un elemento
    int page;
    int row;               // fila enfocada del panel (mando)
    double panel_scroll;

    // Arrastre táctil
    EditorDrag drag;
    double down_x, down_y;
    double orig_x, orig_y, orig_w, orig_h, orig_scroll;
    double last_dx, last_dy;     // último desplazamiento aceptado al arrastrar
    bool drag_moved;
    int held_row, held_dir;      // botón −/+ mantenido (repetición)
    uint32_t held_since, held_last;
    int pressed_button;          // botón de la barra pulsado (-1)

    // Deshacer
    EditorSnapshot *undo[EDITOR_UNDO];
    int undo_count;
    int last_change_row;
    uint32_t last_change_ms;

    // Autoguardado
    bool dirty;
    uint32_t dirty_ms;

    // Selector (apps instaladas, imagen del elemento, fondo de pantalla)
    PickerMode picker;
    DesktopEntry *entries;
    int n_entries;
    char (*images)[MAX_STR];
    int n_images;
    bool images_wallpaper;
    double picker_scroll;

    // Selector de color (rueda, RGB, CMYK, paleta)
    int col_row;             // fila (RowId) cuyo color se edita
    int col_mode;            // pestaña activa
    int col_field;           // control enfocado con el mando
    int col_drag;            // control que se arrastra (táctil)
    double col_hsv[3];       // matiz 0..1, saturación, brillo (conserva el matiz en grises)
    double col_cmyk[4];
    cairo_surface_t *col_wheel;
    int col_wheel_r;

    // Choque: el elemento con el que se chocó se marca en rojo un momento
    int collide_idx;
    bool collide_bar;        // el choque es con la barra
    uint32_t collide_until;

    char status[160];
    uint32_t status_until;
    bool want_quit;
} Editor;

void editor_init(Editor *ed, DesktopConfig *cfg, const char *save_path, int width, int height);
void editor_free(Editor *ed);
void editor_render(Editor *ed, cairo_t *cr, int width, int height);

void editor_touch_down(Editor *ed, double x, double y, uint32_t now);
void editor_touch_motion(Editor *ed, double x, double y, uint32_t now);
void editor_touch_up(Editor *ed, double x, double y, uint32_t now);
void editor_nav(Editor *ed, NavAction act, uint32_t now);
// Repeticiones, sticks y autoguardado. Devuelve true si hay que redibujar.
bool editor_tick(Editor *ed, const GamepadInput *gp, uint32_t now);
// ¿Sigue animando? (repetición en curso, mensaje visible, sticks fuera de la zona muerta)
bool editor_busy(const Editor *ed, const GamepadInput *gp, uint32_t now);
bool editor_save(Editor *ed);
// Abre el selector de color: 0 = fondo del elemento seleccionado, 1 = color base del fondo de pantalla
bool editor_open_color_picker(Editor *ed, int which);

#endif // EDITOR_H
