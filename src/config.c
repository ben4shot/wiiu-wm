#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <math.h>
#include <json.h>
#include "poly.h"

/* ------------------------------------------------------ enums <-> texto */

static const char *SHAPE_NAMES[SHAPE_COUNT] = {
    "squircle", "triangle_up", "triangle_down", "triangle_left", "triangle_right",
    "rounded", "circle", "hexagon", "panel", "bubble", "burst", "cloud"
};
static const char *BORDER_NAMES[BORDER_COUNT] = { "none", "solid", "dashed", "dotted", "double", "ink" };
static const char *FILL_NAMES[FILL_COUNT] = { "flat", "gradient", "gloss", "halftone" };
static const char *SHADOW_NAMES[SHADOW_COUNT] = { "none", "soft", "hard" };
static const char *LABEL_NAMES[LABEL_COUNT] = { "below", "inside", "hidden" };

static int name_index(const char *const *names, int n, const char *s, int def) {
    if (!s) return def;
    for (int i = 0; i < n; i++) if (strcasecmp(names[i], s) == 0) return i;
    return def;
}

TileShape shape_from_string(const char *str) {
    if (str && strcasecmp(str, "triangle") == 0) return SHAPE_TRIANGLE_UP;
    if (str && strcasecmp(str, "round_rect") == 0) return SHAPE_ROUNDED;
    if (str && strcasecmp(str, "hex") == 0) return SHAPE_HEXAGON;
    if (str && strcasecmp(str, "starburst") == 0) return SHAPE_BURST;
    return (TileShape)name_index(SHAPE_NAMES, SHAPE_COUNT, str, SHAPE_SQUIRCLE);
}
const char *shape_to_string(TileShape s) { return (s >= 0 && s < SHAPE_COUNT) ? SHAPE_NAMES[s] : "squircle"; }
BorderStyle border_style_from_string(const char *s) { return (BorderStyle)name_index(BORDER_NAMES, BORDER_COUNT, s, BORDER_SOLID); }
const char *border_style_to_string(BorderStyle b) { return (b >= 0 && b < BORDER_COUNT) ? BORDER_NAMES[b] : "solid"; }
FillStyle fill_from_string(const char *s) { return (FillStyle)name_index(FILL_NAMES, FILL_COUNT, s, FILL_FLAT); }
const char *fill_to_string(FillStyle f) { return (f >= 0 && f < FILL_COUNT) ? FILL_NAMES[f] : "flat"; }
ShadowStyle shadow_from_string(const char *s) { return (ShadowStyle)name_index(SHADOW_NAMES, SHADOW_COUNT, s, SHADOW_NONE); }
const char *shadow_to_string(ShadowStyle s) { return (s >= 0 && s < SHADOW_COUNT) ? SHADOW_NAMES[s] : "none"; }
LabelPos label_from_string(const char *s) { return (LabelPos)name_index(LABEL_NAMES, LABEL_COUNT, s, LABEL_BELOW); }
const char *label_to_string(LabelPos l) { return (l >= 0 && l < LABEL_COUNT) ? LABEL_NAMES[l] : "below"; }

ColorRGBA parse_hex_color(const char *hex, double default_alpha) {
    ColorRGBA c = {0.0, 0.0, 0.0, default_alpha};
    if (!hex) return c;
    if (hex[0] == '#') hex++;

    unsigned int r = 0, g = 0, b = 0, a = 255;
    size_t len = strlen(hex);
    if (len == 6 && sscanf(hex, "%02x%02x%02x", &r, &g, &b) == 3) {
        c.r = r / 255.0; c.g = g / 255.0; c.b = b / 255.0; c.a = default_alpha;
    } else if (len == 8 && sscanf(hex, "%02x%02x%02x%02x", &r, &g, &b, &a) == 4) {
        c.r = r / 255.0; c.g = g / 255.0; c.b = b / 255.0; c.a = a / 255.0;
    }
    return c;
}

void color_to_hex(const ColorRGBA *c, char *out, int out_len) {
    int r = (int)(c->r * 255.0 + 0.5), g = (int)(c->g * 255.0 + 0.5), b = (int)(c->b * 255.0 + 0.5);
    int a = (int)(c->a * 255.0 + 0.5);
    if (a >= 255) snprintf(out, out_len, "#%02x%02x%02x", r, g, b);
    else snprintf(out, out_len, "#%02x%02x%02x%02x", r, g, b, a);
}

/* -------------------------------------------------------------- valores */

static void style_defaults(ElementStyle *st) {
    memset(st, 0, sizeof(*st));
    st->radius = 4.0;
    st->padding = 12.0;
    st->border_style = BORDER_INK;
    st->border_width = 3.5;
    st->fill = FILL_HALFTONE;
    st->shadow = SHADOW_HARD;
    st->label = LABEL_INSIDE;
    st->icon_scale = 1.0;
    st->font_size = 12.5;
    st->bg = parse_hex_color("#7c3aed", 1.0);
    st->border = parse_hex_color("#0b0712", 1.0);
    st->text = parse_hex_color("#fff4d6", 1.0);
}

void config_set_defaults(DesktopConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->screen_width = 854;
    cfg->screen_height = 480;
    cfg->fullscreen = false;
    cfg->maximize_apps = true;

    cfg->bg_color = parse_hex_color("#0c0914", 1.0);
    cfg->bg_gradient = parse_hex_color("#1a102c", 1.0);
    cfg->tile_bg = parse_hex_color("#221738", 0.90);
    cfg->tile_border = parse_hex_color("#7c3aed", 0.85);
    cfg->tile_glow = parse_hex_color("#c084fc", 1.0);
    cfg->tile_text = parse_hex_color("#f3e8ff", 1.0);
    cfg->accent = parse_hex_color("#9333ea", 1.0);
    snprintf(cfg->font, sizeof(cfg->font), "Sans");
    cfg->wallpaper[0] = '\0';
    cfg->gutter = 8;

    cfg->tile_width = 110;
    cfg->tile_height = 110;
    cfg->spacing_x = 28;
    cfg->spacing_y = 34;
    cfg->columns = 5;
    cfg->rows = 2;
    style_defaults(&cfg->default_style);

    // Atajos por defecto. HOME = botón HOME del mando o la tecla Super del teclado.
    static const char *def_binds[][2] = {
        { "HOME", "home" },
        { "HOME+LEFT", "focus_prev" },  { "HOME+RIGHT", "focus_next" },
        { "HOME+X", "close" },
        { "HOME+L", "workspace_prev" }, { "HOME+R", "workspace_next" },
        { "HOME+Y", "toggle_tile" },
        { "HOME+ZL", "move_prev" },     { "HOME+ZR", "move_next" },
        { "HOME+UP", "home_grid" },     { "HOME+DOWN", "recents" },
        { "HOME+q", "close" },          { "HOME+t", "toggle_tile" },
        { "HOME+Return", "exec foot" },
        { "HOME+PLUS", "toggle_bar" },
        { "HOME+1", "workspace 1" }, { "HOME+2", "workspace 2" },
        { "HOME+3", "workspace 3" }, { "HOME+4", "workspace 4" },
    };
    cfg->num_binds = 0;
    for (size_t i = 0; i < sizeof(def_binds) / sizeof(def_binds[0]); i++) {
        snprintf(cfg->bind_keys[i], sizeof(cfg->bind_keys[i]), "%s", def_binds[i][0]);
        snprintf(cfg->bind_actions[i], sizeof(cfg->bind_actions[i]), "%s", def_binds[i][1]);
        cfg->num_binds++;
    }
    cfg->workspaces = 4;
    cfg->tile_new_windows = false;
    cfg->tile_gap = 6;
    cfg->bar_enabled = false;
    snprintf(cfg->bar_command, sizeof(cfg->bar_command), "waybar");
    cfg->bar_x = 0; cfg->bar_y = 0; cfg->bar_w = 854; cfg->bar_h = 30;

    cfg->gamepad_auto = true;
    snprintf(cfg->gamepad_device, sizeof(cfg->gamepad_device), "/dev/input/event0");
    snprintf(cfg->gamepad_layout, sizeof(cfg->gamepad_layout), "auto");
    cfg->num_apps = 0;
}

/* ------------------------------------------------------------- lectura */

static bool get_str(struct json_object *o, const char *key, char *out, size_t len) {
    struct json_object *v;
    if (!o || !json_object_object_get_ex(o, key, &v)) return false;
    snprintf(out, len, "%s", json_object_get_string(v));
    return true;
}
static bool get_num(struct json_object *o, const char *key, double *out) {
    struct json_object *v;
    if (!o || !json_object_object_get_ex(o, key, &v)) return false;
    *out = json_object_get_double(v);
    return true;
}
static bool get_int(struct json_object *o, const char *key, int *out) {
    struct json_object *v;
    if (!o || !json_object_object_get_ex(o, key, &v)) return false;
    *out = json_object_get_int(v);
    return true;
}
static bool get_bool(struct json_object *o, const char *key, bool *out) {
    struct json_object *v;
    if (!o || !json_object_object_get_ex(o, key, &v)) return false;
    *out = json_object_get_boolean(v);
    return true;
}
static void get_color(struct json_object *o, const char *key, ColorRGBA *c, double alpha) {
    char buf[32];
    if (get_str(o, key, buf, sizeof(buf))) *c = parse_hex_color(buf, alpha);
}

// Aplica sobre `st` solo las claves presentes (herencia del estilo base)
static void parse_style(struct json_object *o, ElementStyle *st) {
    if (!o) return;
    char buf[64];
    get_num(o, "radius", &st->radius);
    get_num(o, "padding", &st->padding);
    if (get_str(o, "border_style", buf, sizeof(buf))) st->border_style = border_style_from_string(buf);
    get_num(o, "border_width", &st->border_width);
    if (get_str(o, "fill", buf, sizeof(buf))) st->fill = fill_from_string(buf);
    if (get_str(o, "shadow", buf, sizeof(buf))) st->shadow = shadow_from_string(buf);
    if (get_str(o, "label", buf, sizeof(buf))) st->label = label_from_string(buf);
    get_num(o, "icon_scale", &st->icon_scale);
    get_num(o, "font_size", &st->font_size);
    get_color(o, "bg", &st->bg, 1.0);
    get_color(o, "border", &st->border, 1.0);
    get_color(o, "text", &st->text, 1.0);
    get_str(o, "image", st->image, sizeof(st->image));
}

// Formas antiguas: las de cómic pasan a ser imágenes de fondo incluidas
static void legacy_shape_image(TileShape sh, ElementStyle *st) {
    if (st->image[0]) return;
    if (sh == SHAPE_BURST) snprintf(st->image, sizeof(st->image), "builtin:burst");
    else if (sh == SHAPE_BUBBLE) snprintf(st->image, sizeof(st->image), "builtin:bubble");
    else if (sh == SHAPE_CLOUD) snprintf(st->image, sizeof(st->image), "builtin:cloud");
}

void app_update_bbox(AppEntry *a) {
    if (a->npts <= 0) return;
    double x0 = a->px[0], x1 = a->px[0], y0 = a->py[0], y1 = a->py[0];
    for (int i = 1; i < a->npts; i++) {
        if (a->px[i] < x0) x0 = a->px[i];
        if (a->px[i] > x1) x1 = a->px[i];
        if (a->py[i] < y0) y0 = a->py[i];
        if (a->py[i] > y1) y1 = a->py[i];
    }
    a->x = x0; a->y = y0; a->w = x1 - x0; a->h = y1 - y0;
    a->has_pos = true;
}

// Forma antigua de cada elemento mientras se carga (para colocarlo en la rejilla)
static TileShape legacy_shapes[MAX_APPS];

bool config_load(DesktopConfig *cfg, const char *filepath) {
    config_set_defaults(cfg);
    for (int i = 0; i < MAX_APPS; i++) legacy_shapes[i] = SHAPE_RECT_LEGACY;
    snprintf(cfg->loaded_from, sizeof(cfg->loaded_from), "%s", filepath ? filepath : "");

    struct json_object *root = filepath ? json_object_from_file(filepath) : NULL;
    if (!root) {
        fprintf(stderr, "[Config] No se pudo leer '%s'. Usando valores por defecto.\n", filepath ? filepath : "(nada)");
        config_layout_defaults(cfg);
        return false;
    }

    struct json_object *o;
    TileShape default_shape = SHAPE_RECT_LEGACY;
    if (json_object_object_get_ex(root, "screen", &o)) {
        get_int(o, "width", &cfg->screen_width);
        get_int(o, "height", &cfg->screen_height);
        get_bool(o, "fullscreen", &cfg->fullscreen);
        get_bool(o, "maximize_apps", &cfg->maximize_apps);
    }

    if (json_object_object_get_ex(root, "theme", &o)) {
        get_color(o, "bg_color", &cfg->bg_color, 1.0);
        get_color(o, "bg_gradient", &cfg->bg_gradient, 1.0);
        get_color(o, "tile_bg", &cfg->tile_bg, 0.90);
        get_color(o, "tile_border", &cfg->tile_border, 0.85);
        get_color(o, "tile_glow", &cfg->tile_glow, 1.0);
        get_color(o, "tile_text", &cfg->tile_text, 1.0);
        get_color(o, "accent", &cfg->accent, 1.0);
        get_str(o, "font", cfg->font, sizeof(cfg->font));
        get_str(o, "wallpaper", cfg->wallpaper, sizeof(cfg->wallpaper));
    }

    if (json_object_object_get_ex(root, "grid", &o)) {
        char buf[64];
        // Claves antiguas: alimentan el estilo base
        if (get_str(o, "default_shape", buf, sizeof(buf))) default_shape = shape_from_string(buf);
        get_num(o, "border_radius", &cfg->default_style.radius);
        get_int(o, "gutter", &cfg->gutter);
        get_num(o, "border_width", &cfg->default_style.border_width);
        get_int(o, "tile_width", &cfg->tile_width);
        get_int(o, "tile_height", &cfg->tile_height);
        get_int(o, "spacing_x", &cfg->spacing_x);
        get_int(o, "spacing_y", &cfg->spacing_y);
        get_int(o, "columns", &cfg->columns);
        get_int(o, "rows", &cfg->rows);
    }
    if (cfg->columns < 1) cfg->columns = 5;
    if (cfg->rows < 1) cfg->rows = 2;

    if (json_object_object_get_ex(root, "style", &o)) {
        char buf[64];
        if (get_str(o, "shape", buf, sizeof(buf))) default_shape = shape_from_string(buf);
        parse_style(o, &cfg->default_style);
    }

    if (json_object_object_get_ex(root, "binds", &o) && json_object_is_type(o, json_type_object)) {
        // Los atajos del fichero sustituyen a los de por defecto con la misma tecla
        json_object_object_foreach(o, key, val) {
            int slot = -1;
            for (int i = 0; i < cfg->num_binds; i++)
                if (strcasecmp(cfg->bind_keys[i], key) == 0) slot = i;
            if (slot < 0 && cfg->num_binds < MAX_BINDS) slot = cfg->num_binds++;
            if (slot < 0) continue;
            snprintf(cfg->bind_keys[slot], sizeof(cfg->bind_keys[slot]), "%s", key);
            snprintf(cfg->bind_actions[slot], sizeof(cfg->bind_actions[slot]), "%s", json_object_get_string(val));
        }
    }
    if (json_object_object_get_ex(root, "windows", &o)) {
        get_int(o, "workspaces", &cfg->workspaces);
        get_bool(o, "tile_new_windows", &cfg->tile_new_windows);
        get_int(o, "tile_gap", &cfg->tile_gap);
    }
    if (json_object_object_get_ex(root, "bar", &o)) {
        get_bool(o, "enabled", &cfg->bar_enabled);
        get_str(o, "command", cfg->bar_command, sizeof(cfg->bar_command));
        get_num(o, "x", &cfg->bar_x);
        get_num(o, "y", &cfg->bar_y);
        get_num(o, "w", &cfg->bar_w);
        get_num(o, "h", &cfg->bar_h);
        if (cfg->bar_w < 20) cfg->bar_w = 20;
        if (cfg->bar_h < 16) cfg->bar_h = 16;
    }
    if (cfg->workspaces < 1) cfg->workspaces = 1;
    if (cfg->workspaces > 9) cfg->workspaces = 9;

    if (json_object_object_get_ex(root, "gamepad", &o)) {
        get_bool(o, "device_auto", &cfg->gamepad_auto);
        get_str(o, "preferred_device", cfg->gamepad_device, sizeof(cfg->gamepad_device));
        get_str(o, "layout", cfg->gamepad_layout, sizeof(cfg->gamepad_layout));
    }

    if (json_object_object_get_ex(root, "apps", &o) && json_object_is_type(o, json_type_array)) {
        size_t n = json_object_array_length(o);
        if (n > MAX_APPS) n = MAX_APPS;
        for (size_t i = 0; i < n; i++) {
            struct json_object *a = json_object_array_get_idx(o, i);
            if (!a) continue;
            AppEntry *e = &cfg->apps[cfg->num_apps];
            memset(e, 0, sizeof(*e));
            e->style = cfg->default_style;

            get_str(a, "name", e->name, sizeof(e->name));
            get_str(a, "exec", e->exec, sizeof(e->exec));
            get_str(a, "icon", e->icon, sizeof(e->icon));
            get_str(a, "description", e->description, sizeof(e->description));
            get_bool(a, "single_instance", &e->single_instance);
            // Forma antigua (squircle, triangle_up...): se convierte en polígono
            TileShape shape = default_shape;
            char buf[64];
            if (get_str(a, "shape", buf, sizeof(buf))) shape = shape_from_string(buf);
            struct json_object *so;
            if (json_object_object_get_ex(a, "style", &so)) {
                if (get_str(so, "shape", buf, sizeof(buf))) shape = shape_from_string(buf);
                parse_style(so, &e->style);
            }
            if (shape != SHAPE_RECT_LEGACY) legacy_shape_image(shape, &e->style);
            legacy_shapes[cfg->num_apps] = shape;
            get_int(a, "page", &e->page);

            struct json_object *poly;
            if (json_object_object_get_ex(a, "poly", &poly) && json_object_is_type(poly, json_type_array)) {
                int n = (int)json_object_array_length(poly);
                for (int k = 0; k < n && e->npts < MAX_POLY; k++) {
                    struct json_object *pt = json_object_array_get_idx(poly, k);
                    if (!pt || json_object_array_length(pt) < 2) continue;
                    e->px[e->npts] = json_object_get_double(json_object_array_get_idx(pt, 0));
                    e->py[e->npts] = json_object_get_double(json_object_array_get_idx(pt, 1));
                    e->npts++;
                }
                if (e->npts >= 3) app_update_bbox(e);
                else e->npts = 0;
            }
            double x, y;
            if (e->npts == 0 && get_num(a, "x", &x) && get_num(a, "y", &y)) {
                // Caja de una versión anterior: polígono equivalente a su forma
                double w = cfg->tile_width, h = cfg->tile_height;
                get_num(a, "w", &w);
                get_num(a, "h", &h);
                poly_set_preset(e, poly_preset_for_shape(shape), x, y, w, h);
            }

            if (!get_str(a, "app_id", e->app_id, sizeof(e->app_id))) {
                // Por defecto: nombre del primer ejecutable de la orden («foot», «firefox»...)
                const char *ex = e->exec;
                while (*ex == ' ') ex++;
                size_t len = strcspn(ex, " \t");
                const char *base = ex;
                for (const char *q = ex; q < ex + len; q++) if (*q == '/') base = q + 1;
                snprintf(e->app_id, sizeof(e->app_id), "%.*s", (int)(ex + len - base), base);
            }
            // El editor siempre es de una sola instancia: dos editores se pisarían al guardar
            if (strstr(e->exec, "wiiu-shell") && strstr(e->exec, "--editor")) {
                e->single_instance = true;
                snprintf(e->app_id, sizeof(e->app_id), "wiiu-shell-editor");
            }
            cfg->num_apps++;
        }
    }

    json_object_put(root);
    config_layout_defaults(cfg);
    printf("[Config] '%s': %d aplicaciones.\n", filepath, cfg->num_apps);
    return true;
}

void config_layout_defaults(DesktopConfig *cfg) {
    // Rejilla centrada, igual que la del menú clásico, para los elementos sin posición
    int cols = cfg->columns, rows = cfg->rows, per_page = cols * rows;
    double grid_w = cols * cfg->tile_width + (cols - 1) * cfg->spacing_x;
    double grid_h = rows * cfg->tile_height + (rows - 1) * cfg->spacing_y;
    double start_x = (cfg->screen_width - grid_w) / 2.0;
    double start_y = (cfg->screen_height - grid_h) / 2.0 + 4.0;

    int slot = 0;
    for (int i = 0; i < cfg->num_apps; i++) {
        AppEntry *e = &cfg->apps[i];
        if (e->has_pos) continue;
        int page = slot / per_page, s = slot % per_page;
        e->page = page;
        double x = start_x + (s % cols) * (cfg->tile_width + cfg->spacing_x);
        double y = start_y + (s / cols) * (cfg->tile_height + cfg->spacing_y);
        poly_set_preset(e, poly_preset_for_shape(legacy_shapes[i]), x, y, cfg->tile_width, cfg->tile_height);
        slot++;
    }
}

int config_page_count(const DesktopConfig *cfg) {
    int pages = 1;
    for (int i = 0; i < cfg->num_apps; i++) {
        if (cfg->apps[i].page + 1 > pages) pages = cfg->apps[i].page + 1;
    }
    return pages;
}

/* ------------------------------------------------------------ escritura */

static void put_color(struct json_object *o, const char *key, const ColorRGBA *c) {
    char buf[16];
    color_to_hex(c, buf, sizeof(buf));
    json_object_object_add(o, key, json_object_new_string(buf));
}
static void put_num(struct json_object *o, const char *key, double v) {
    // Redondeo a 2 decimales para un JSON legible
    double r = (double)(long long)(v * 100.0 + (v < 0 ? -0.5 : 0.5)) / 100.0;
    json_object_object_add(o, key, json_object_new_double(r));
}
static void put_str(struct json_object *o, const char *key, const char *v) {
    json_object_object_add(o, key, json_object_new_string(v));
}

static struct json_object *style_to_json(const ElementStyle *st) {
    struct json_object *o = json_object_new_object();
    put_num(o, "radius", st->radius);
    put_num(o, "padding", st->padding);
    put_str(o, "border_style", border_style_to_string(st->border_style));
    put_num(o, "border_width", st->border_width);
    put_str(o, "fill", fill_to_string(st->fill));
    put_str(o, "shadow", shadow_to_string(st->shadow));
    put_str(o, "label", label_to_string(st->label));
    put_num(o, "icon_scale", st->icon_scale);
    put_num(o, "font_size", st->font_size);
    put_color(o, "bg", &st->bg);
    put_color(o, "border", &st->border);
    put_color(o, "text", &st->text);
    put_str(o, "image", st->image);
    return o;
}

static void mkdir_parents(const char *filepath) {
    char dir[MAX_STR];
    snprintf(dir, sizeof(dir), "%s", filepath);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(dir, 0755); *p = '/'; }
    }
    mkdir(dir, 0755);
}

bool config_save(const DesktopConfig *cfg, const char *filepath) {
    struct json_object *root = json_object_new_object();

    struct json_object *screen = json_object_new_object();
    json_object_object_add(screen, "width", json_object_new_int(cfg->screen_width));
    json_object_object_add(screen, "height", json_object_new_int(cfg->screen_height));
    json_object_object_add(screen, "fullscreen", json_object_new_boolean(cfg->fullscreen));
    json_object_object_add(screen, "maximize_apps", json_object_new_boolean(cfg->maximize_apps));
    json_object_object_add(root, "screen", screen);

    struct json_object *theme = json_object_new_object();
    put_color(theme, "bg_color", &cfg->bg_color);
    put_color(theme, "bg_gradient", &cfg->bg_gradient);
    put_color(theme, "tile_bg", &cfg->tile_bg);
    put_color(theme, "tile_border", &cfg->tile_border);
    put_color(theme, "tile_glow", &cfg->tile_glow);
    put_color(theme, "tile_text", &cfg->tile_text);
    put_color(theme, "accent", &cfg->accent);
    put_str(theme, "font", cfg->font);
    put_str(theme, "wallpaper", cfg->wallpaper);
    json_object_object_add(root, "theme", theme);

    struct json_object *grid = json_object_new_object();
    json_object_object_add(grid, "tile_width", json_object_new_int(cfg->tile_width));
    json_object_object_add(grid, "tile_height", json_object_new_int(cfg->tile_height));
    json_object_object_add(grid, "spacing_x", json_object_new_int(cfg->spacing_x));
    json_object_object_add(grid, "spacing_y", json_object_new_int(cfg->spacing_y));
    json_object_object_add(grid, "columns", json_object_new_int(cfg->columns));
    json_object_object_add(grid, "rows", json_object_new_int(cfg->rows));
    json_object_object_add(grid, "gutter", json_object_new_int(cfg->gutter));
    json_object_object_add(root, "grid", grid);

    json_object_object_add(root, "style", style_to_json(&cfg->default_style));

    struct json_object *binds = json_object_new_object();
    for (int i = 0; i < cfg->num_binds; i++) put_str(binds, cfg->bind_keys[i], cfg->bind_actions[i]);
    json_object_object_add(root, "binds", binds);
    struct json_object *win = json_object_new_object();
    json_object_object_add(win, "workspaces", json_object_new_int(cfg->workspaces));
    json_object_object_add(win, "tile_new_windows", json_object_new_boolean(cfg->tile_new_windows));
    json_object_object_add(win, "tile_gap", json_object_new_int(cfg->tile_gap));
    json_object_object_add(root, "windows", win);
    struct json_object *bar = json_object_new_object();
    json_object_object_add(bar, "enabled", json_object_new_boolean(cfg->bar_enabled));
    put_str(bar, "command", cfg->bar_command);
    put_num(bar, "x", cfg->bar_x);
    put_num(bar, "y", cfg->bar_y);
    put_num(bar, "w", cfg->bar_w);
    put_num(bar, "h", cfg->bar_h);
    json_object_object_add(root, "bar", bar);

    struct json_object *gp = json_object_new_object();
    json_object_object_add(gp, "device_auto", json_object_new_boolean(cfg->gamepad_auto));
    put_str(gp, "preferred_device", cfg->gamepad_device);
    put_str(gp, "layout", cfg->gamepad_layout);
    json_object_object_add(root, "gamepad", gp);

    struct json_object *apps = json_object_new_array();
    for (int i = 0; i < cfg->num_apps; i++) {
        const AppEntry *e = &cfg->apps[i];
        struct json_object *a = json_object_new_object();
        put_str(a, "name", e->name);
        put_str(a, "exec", e->exec);
        put_str(a, "icon", e->icon);
        put_str(a, "description", e->description);
        put_str(a, "app_id", e->app_id);
        if (e->single_instance) json_object_object_add(a, "single_instance", json_object_new_boolean(true));
        json_object_object_add(a, "page", json_object_new_int(e->page));
        struct json_object *poly = json_object_new_array();
        for (int k = 0; k < e->npts; k++) {
            struct json_object *pt = json_object_new_array();
            json_object_array_add(pt, json_object_new_double(round(e->px[k] * 10) / 10));
            json_object_array_add(pt, json_object_new_double(round(e->py[k] * 10) / 10));
            json_object_array_add(poly, pt);
        }
        json_object_object_add(a, "poly", poly);
        json_object_object_add(a, "style", style_to_json(&e->style));
        json_object_array_add(apps, a);
    }
    json_object_object_add(root, "apps", apps);

    mkdir_parents(filepath);
    // Escritura atómica: el shell vigila el fichero y no debe leerlo a medias
    char tmp[MAX_STR + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", filepath);
    int rc = json_object_to_file_ext(tmp, root, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED);
    json_object_put(root);
    if (rc != 0) {
        fprintf(stderr, "[Config] No se pudo escribir '%s'.\n", tmp);
        return false;
    }
    if (rename(tmp, filepath) != 0) {
        fprintf(stderr, "[Config] No se pudo renombrar '%s': %s\n", tmp, strerror(errno));
        unlink(tmp);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------ rutas */

const char *config_bind_action(const DesktopConfig *cfg, const char *key) {
    for (int i = 0; i < cfg->num_binds; i++)
        if (strcasecmp(cfg->bind_keys[i], key) == 0) return cfg->bind_actions[i];
    return NULL;
}

void config_user_path(char *out, int out_len) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0]) snprintf(out, out_len, "%.200s/wiiu-shell/config.json", xdg);
    else snprintf(out, out_len, "%.200s/.config/wiiu-shell/config.json", home ? home : "/tmp");
}

void config_resolve_path(const char *fallback, char *out, int out_len) {
    char user[MAX_STR];
    config_user_path(user, sizeof(user));
    if (access(user, R_OK) == 0) snprintf(out, out_len, "%s", user);
    else snprintf(out, out_len, "%s", fallback ? fallback : user);
}
