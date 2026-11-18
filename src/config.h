#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_APPS 64
#define MAX_STR 256

typedef enum {
    SHAPE_SQUIRCLE = 0,
    SHAPE_TRIANGLE_UP,
    SHAPE_TRIANGLE_DOWN,
    SHAPE_TRIANGLE_LEFT,
    SHAPE_TRIANGLE_RIGHT,
    SHAPE_ROUNDED,
    SHAPE_CIRCLE,
    SHAPE_HEXAGON,
    // Formas de cómic
    SHAPE_PANEL,      // viñeta: cuadrilátero con lados inclinados
    SHAPE_BUBBLE,     // bocadillo de diálogo con cola
    SHAPE_BURST,      // estallido / onomatopeya («¡POW!»)
    SHAPE_CLOUD,      // nube de pensamiento
    SHAPE_COUNT,
    SHAPE_RECT_LEGACY = 100   // sin forma antigua indicada
} TileShape;

typedef enum { BORDER_NONE = 0, BORDER_SOLID, BORDER_DASHED, BORDER_DOTTED, BORDER_DOUBLE, BORDER_INK, BORDER_COUNT } BorderStyle;
typedef enum { FILL_FLAT = 0, FILL_GRADIENT, FILL_GLOSS, FILL_HALFTONE, FILL_COUNT } FillStyle;
typedef enum { SHADOW_NONE = 0, SHADOW_SOFT, SHADOW_HARD, SHADOW_COUNT } ShadowStyle;
typedef enum { LABEL_BELOW = 0, LABEL_INSIDE, LABEL_HIDDEN, LABEL_COUNT } LabelPos;

typedef struct {
    double r, g, b, a;
} ColorRGBA;

// Estilo de un elemento del menú. Todo es editable desde el editor.
typedef struct {
    double radius;        // redondeado de las esquinas del polígono
    double padding;       // espacio entre el borde y el icono
    BorderStyle border_style;
    double border_width;
    FillStyle fill;
    ShadowStyle shadow;
    LabelPos label;
    double icon_scale;    // 1.0 = el icono llena el hueco disponible
    double font_size;
    ColorRGBA bg;
    ColorRGBA border;
    ColorRGBA text;
    char image[MAX_STR];  // imagen de fondo elegida: "" | ruta | "builtin:burst|bubble|cloud"
} ElementStyle;

#define MAX_POLY 16

typedef struct {
    char name[64];
    char exec[MAX_STR];
    char icon[MAX_STR];   // nombre de icono o ruta; vacío = el del programa (.desktop)
    char description[128];
    char app_id[64];      // app_id Wayland de su ventana (por defecto: nombre del ejecutable)
    bool single_instance; // si ya tiene ventana, el tile la trae al frente en vez de abrir otra

    // Forma = zona táctil: polígono (coordenadas de pantalla) en su página.
    // Es lo que se dibuja y lo que responde al toque; no puede solaparse con otros.
    int npts;
    double px[MAX_POLY], py[MAX_POLY];
    int page;
    // Caja que envuelve el polígono (derivada: app_update_bbox)
    bool has_pos;
    double x, y, w, h;

    ElementStyle style;
} AppEntry;

typedef struct {
    int screen_width;
    int screen_height;
    bool fullscreen;
    bool maximize_apps;           // maximizar las ventanas nuevas (pantalla de 854x480)

    ColorRGBA bg_color;
    ColorRGBA bg_gradient;
    ColorRGBA tile_bg;
    ColorRGBA tile_border;
    ColorRGBA tile_glow;
    ColorRGBA tile_text;
    ColorRGBA accent;
    char font[64];
    char wallpaper[MAX_STR];      // "" (degradado), ruta de imagen o "builtin:comic|halftone"
    int gutter;                   // separación mínima entre elementos (calle de la viñeta)

    // Rejilla para colocar los elementos que no tienen posición propia
    int tile_width;
    int tile_height;
    int spacing_x;
    int spacing_y;
    int columns;
    int rows;

    ElementStyle default_style;   // estilo base (sección "style" de config.json)

    bool gamepad_auto;
    char gamepad_device[MAX_STR];
    char gamepad_layout[16];      // "auto", "nintendo" (A=BTN_EAST) o "xbox" (A=BTN_SOUTH)

    // Entorno propio (wiiu-wm): atajos estilo Hyprland, escritorios y mosaico
    #define MAX_BINDS 64
    char bind_keys[MAX_BINDS][32];     // "HOME+LEFT", "HOME+X", "SUPER+Return"...
    char bind_actions[MAX_BINDS][128]; // "focus_prev", "close", "exec foot"...
    int num_binds;
    int workspaces;                    // número de escritorios (1..9)
    bool tile_new_windows;             // las ventanas nuevas entran en mosaico
    int tile_gap;                      // separación entre ventanas en mosaico

    // Barra (waybar u otra con layer-shell): caja libre en la pantalla, como un elemento más
    bool bar_enabled;
    char bar_command[MAX_STR];
    double bar_x, bar_y, bar_w, bar_h;

    AppEntry apps[MAX_APPS];
    int num_apps;

    char loaded_from[MAX_STR];    // fichero del que se cargó
} DesktopConfig;

TileShape shape_from_string(const char *str);
const char *shape_to_string(TileShape shape);
const char *border_style_to_string(BorderStyle b);
BorderStyle border_style_from_string(const char *s);
const char *fill_to_string(FillStyle f);
FillStyle fill_from_string(const char *s);
const char *shadow_to_string(ShadowStyle s);
ShadowStyle shadow_from_string(const char *s);
const char *label_to_string(LabelPos l);
LabelPos label_from_string(const char *s);

ColorRGBA parse_hex_color(const char *hex, double default_alpha);
void color_to_hex(const ColorRGBA *c, char *out, int out_len);

bool config_load(DesktopConfig *cfg, const char *filepath);
void config_set_defaults(DesktopConfig *cfg);
// Da posición de rejilla a los elementos que no la tienen
void config_layout_defaults(DesktopConfig *cfg);
// Recalcula x, y, w, h a partir del polígono
void app_update_bbox(AppEntry *a);
int config_page_count(const DesktopConfig *cfg);
bool config_save(const DesktopConfig *cfg, const char *filepath);

// Acción del atajo `key` (sin distinguir mayúsculas), o NULL
const char *config_bind_action(const DesktopConfig *cfg, const char *key);

// Configuración del usuario (la que escribe el editor): $XDG_CONFIG_HOME/wiiu-shell/config.json
void config_user_path(char *out, int out_len);
// Elige qué cargar: la del usuario si existe; si no, `fallback`
void config_resolve_path(const char *fallback, char *out, int out_len);

#endif // CONFIG_H
