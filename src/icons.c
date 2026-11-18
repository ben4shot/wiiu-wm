#include "icons.h"

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* ----------------------------------------------------- entradas .desktop */

static void app_dirs(char dirs[][256], int *n) {
    const char *home = getenv("HOME");
    const char *xdg_data = getenv("XDG_DATA_HOME");
    *n = 0;
    if (xdg_data && xdg_data[0]) snprintf(dirs[(*n)++], 256, "%.200s/applications", xdg_data);
    else if (home) snprintf(dirs[(*n)++], 256, "%.200s/.local/share/applications", home);
    snprintf(dirs[(*n)++], 256, "/usr/local/share/applications");
    snprintf(dirs[(*n)++], 256, "/usr/share/applications");
}

// Primer token de Exec= sin ruta («/usr/bin/foot --server» -> «foot»)
static void exec_basename(const char *exec, char *out, size_t len) {
    while (*exec == ' ') exec++;
    // «env VAR=x prog» -> prog
    if (strncmp(exec, "env ", 4) == 0) {
        exec += 4;
        while (*exec) {
            while (*exec == ' ') exec++;
            const char *sp = strchr(exec, ' ');
            const char *eq = strchr(exec, '=');
            if (eq && (!sp || eq < sp)) { exec = sp ? sp : exec + strlen(exec); continue; }
            break;
        }
    }
    size_t n = strcspn(exec, " \t");
    const char *base = exec;
    for (const char *q = exec; q < exec + n; q++) if (*q == '/') base = q + 1;
    snprintf(out, len, "%.*s", (int)(exec + n - base), base);
}

static bool parse_desktop_file(const char *path, DesktopEntry *e) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    bool in_main = false, hidden = false;
    memset(e, 0, sizeof(*e));
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '[') { in_main = strcmp(line, "[Desktop Entry]") == 0; continue; }
        if (!in_main) continue;
        if (strncmp(line, "Name=", 5) == 0 && !e->name[0]) snprintf(e->name, sizeof(e->name), "%s", line + 5);
        else if (strncmp(line, "Icon=", 5) == 0) snprintf(e->icon, sizeof(e->icon), "%s", line + 5);
        else if (strncmp(line, "Exec=", 5) == 0) {
            // Quitar los códigos de campo %f %F %u %U...
            char *dst = e->exec;
            const char *src = line + 5;
            size_t room = sizeof(e->exec) - 1;
            while (*src && room) {
                if (src[0] == '%' && src[1]) { src += 2; continue; }
                *dst++ = *src++; room--;
            }
            *dst = '\0';
            while (dst > e->exec && dst[-1] == ' ') *--dst = '\0';
        }
        else if (strcmp(line, "Terminal=true") == 0) e->terminal = true;
        else if (strcmp(line, "NoDisplay=true") == 0 || strcmp(line, "Hidden=true") == 0) hidden = true;
        else if (strncmp(line, "Type=", 5) == 0 && strcmp(line + 5, "Application") != 0) hidden = true;
    }
    fclose(f);
    return e->exec[0] && !hidden;
}

bool desktop_entry_for_exec(const char *exec, DesktopEntry *out) {
    char want[128];
    exec_basename(exec, want, sizeof(want));
    if (!want[0]) return false;

    char dirs[4][256];
    int nd;
    app_dirs(dirs, &nd);
    // 1) «<bin>.desktop» directamente
    for (int i = 0; i < nd; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.desktop", dirs[i], want);
        if (parse_desktop_file(path, out)) return true;
    }
    // 2) cualquier .desktop cuyo Exec= lance ese binario
    for (int i = 0; i < nd; i++) {
        DIR *d = opendir(dirs[i]);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d))) {
            size_t l = strlen(de->d_name);
            if (l < 9 || strcmp(de->d_name + l - 8, ".desktop") != 0) continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dirs[i], de->d_name);
            DesktopEntry e;
            if (!parse_desktop_file(path, &e)) continue;
            char got[128];
            exec_basename(e.exec, got, sizeof(got));
            if (strcmp(got, want) == 0) { *out = e; closedir(d); return true; }
        }
        closedir(d);
    }
    return false;
}

static int cmp_entry(const void *a, const void *b) {
    return strcasecmp(((const DesktopEntry *)a)->name, ((const DesktopEntry *)b)->name);
}

int desktop_entries_list(DesktopEntry *out, int max) {
    char dirs[4][256];
    int nd, n = 0;
    app_dirs(dirs, &nd);
    for (int i = 0; i < nd && n < max; i++) {
        DIR *d = opendir(dirs[i]);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) && n < max) {
            size_t l = strlen(de->d_name);
            if (l < 9 || strcmp(de->d_name + l - 8, ".desktop") != 0) continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dirs[i], de->d_name);
            DesktopEntry e;
            if (!parse_desktop_file(path, &e)) continue;
            bool dup = false;
            for (int k = 0; k < n; k++) if (strcmp(out[k].name, e.name) == 0) dup = true;
            if (!dup) out[n++] = e;
        }
        closedir(d);
    }
    qsort(out, n, sizeof(DesktopEntry), cmp_entry);
    return n;
}

/* ------------------------------------------------------- tema de iconos */

static bool file_ok(const char *p) { return access(p, R_OK) == 0; }

// Directorios de un tema según su index.theme («Directories=a,b,c»)
static int theme_dirs(const char *theme_root, char dirs[][96], int max) {
    char idx[512];
    snprintf(idx, sizeof(idx), "%s/index.theme", theme_root);
    FILE *f = fopen(idx, "r");
    int n = 0;
    if (f) {
        char line[8192];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "Directories=", 12) != 0 && strncmp(line, "ScaledDirectories=", 18) != 0) continue;
            char *p = strchr(line, '=') + 1;
            p[strcspn(p, "\r\n")] = '\0';
            for (char *tok = strtok(p, ","); tok && n < max; tok = strtok(NULL, ",")) {
                snprintf(dirs[n++], 96, "%s", tok);
            }
        }
        fclose(f);
    }
    return n;
}

// Puntuación de un directorio: escalable o grande es mejor; «apps» mejor que otras categorías
static int dir_score(const char *dir) {
    int s = 0;
    if (strstr(dir, "scalable")) s += 1000;
    else s += atoi(dir) > 0 ? atoi(dir) : 0;   // «48x48/apps» -> 48
    if (strstr(dir, "@2")) s -= 1;
    if (strstr(dir, "apps")) s += 5;
    return s;
}

static bool search_theme(const char *root, const char *name, char *best, int best_len, int *best_score) {
    static char dirs[512][96];
    int nd = theme_dirs(root, dirs, 512);
    bool found = false;
    const char *exts[] = { ".svg", ".png" };
    for (int i = 0; i < nd; i++) {
        for (int e = 0; e < 2; e++) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s/%s%s", root, dirs[i], name, exts[e]);
            if (!file_ok(path)) continue;
            int sc = dir_score(dirs[i]) + (e == 0 ? 1 : 0);
            if (sc > *best_score) {
                *best_score = sc;
                snprintf(best, best_len, "%s", path);
                found = true;
            }
        }
    }
    return found;
}

static void default_theme(char *out, size_t len) {
    out[0] = '\0';
    FILE *f = fopen("/usr/share/icons/default/index.theme", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Inherits=", 9) == 0) {
            line[strcspn(line, "\r\n,")] = '\0';
            snprintf(out, len, "%s", line + 9);
        }
    }
    fclose(f);
}

static bool find_in_themes(const char *name, char *out, int out_len) {
    const char *home = getenv("HOME");
    char bases[3][256];
    int nb = 0;
    if (home) snprintf(bases[nb++], 256, "%.200s/.local/share/icons", home);
    snprintf(bases[nb++], 256, "/usr/local/share/icons");
    snprintf(bases[nb++], 256, "/usr/share/icons");

    char def[64];
    default_theme(def, sizeof(def));
    const char *themes[] = { def, "hicolor", "Papirus", "Adwaita", "breeze", "gnome" };

    for (size_t t = 0; t < sizeof(themes) / sizeof(themes[0]); t++) {
        if (!themes[t][0]) continue;
        int score = -1;
        for (int b = 0; b < nb; b++) {
            char root[512];
            snprintf(root, sizeof(root), "%s/%s", bases[b], themes[t]);
            search_theme(root, name, out, out_len, &score);
        }
        if (score >= 0) return true;
    }
    // /usr/share/pixmaps
    const char *exts[] = { ".svg", ".png" };
    for (int e = 0; e < 2; e++) {
        snprintf(out, out_len, "/usr/share/pixmaps/%s%s", name, exts[e]);
        if (file_ok(out)) return true;
    }
    return false;
}

bool icon_find_file(const char *name, char *out_path, int out_len, bool *is_symbolic) {
    if (is_symbolic) *is_symbolic = false;
    if (!name || !name[0]) return false;
    if (name[0] == '/') {
        size_t l = strlen(name);
        bool ok = file_ok(name) && l > 4 &&
                  (strcasecmp(name + l - 4, ".png") == 0 || strcasecmp(name + l - 4, ".svg") == 0);
        if (ok) snprintf(out_path, out_len, "%s", name);
        return ok;
    }
    if (find_in_themes(name, out_path, out_len)) return true;
    // Sin versión a color: la simbólica (monocroma, se tiñe)
    char sym[160];
    snprintf(sym, sizeof(sym), "%.140s-symbolic", name);
    if (find_in_themes(sym, out_path, out_len)) {
        if (is_symbolic) *is_symbolic = true;
        return true;
    }
    return false;
}

/* ------------------------------------------------------ SVG con librsvg */

typedef struct { double x, y, width, height; } RsvgRect;
typedef void *(*fn_new_from_file)(const char *, void **);
typedef int (*fn_render_document)(void *, cairo_t *, const RsvgRect *, void **);
typedef void (*fn_unref)(void *);

static struct {
    bool tried, ok;
    fn_new_from_file new_from_file;
    fn_render_document render_document;
    fn_unref unref;
} rsvg;

static bool rsvg_load(void) {
    if (rsvg.tried) return rsvg.ok;
    rsvg.tried = true;
    void *h = dlopen("librsvg-2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        fprintf(stderr, "[Iconos] librsvg no disponible: solo PNG (%s)\n", dlerror());
        return false;
    }
    rsvg.new_from_file = (fn_new_from_file)dlsym(h, "rsvg_handle_new_from_file");
    rsvg.render_document = (fn_render_document)dlsym(h, "rsvg_handle_render_document");
    rsvg.unref = (fn_unref)dlsym(RTLD_DEFAULT, "g_object_unref");
    rsvg.ok = rsvg.new_from_file && rsvg.render_document && rsvg.unref;
    if (!rsvg.ok) fprintf(stderr, "[Iconos] librsvg demasiado antigua (falta rsvg_handle_render_document)\n");
    return rsvg.ok;
}

/* ------------------------------------------------------------ caché */

#define ICON_CACHE 96

typedef struct {
    char key[384];            // icono configurado | exec | tamaño
    cairo_surface_t *surf;    // NULL = no hay icono
    bool symbolic;
} IconCacheEntry;

static IconCacheEntry cache[ICON_CACHE];
static int cache_next;

void icons_clear_cache(void) {
    for (int i = 0; i < ICON_CACHE; i++) {
        if (cache[i].surf) cairo_surface_destroy(cache[i].surf);
        cache[i].surf = NULL;
        cache[i].key[0] = '\0';
    }
    cache_next = 0;
}

static cairo_surface_t *render_file(const char *path, int px) {
    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_t *cr = cairo_create(out);
    bool ok = false;
    size_t l = strlen(path);

    if (l > 4 && strcasecmp(path + l - 4, ".png") == 0) {
        cairo_surface_t *png = cairo_image_surface_create_from_png(path);
        if (cairo_surface_status(png) == CAIRO_STATUS_SUCCESS) {
            int w = cairo_image_surface_get_width(png), h = cairo_image_surface_get_height(png);
            double sc = (double)px / (w > h ? w : h);
            cairo_scale(cr, sc, sc);
            cairo_set_source_surface(cr, png, (px / sc - w) / 2.0, (px / sc - h) / 2.0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
            cairo_paint(cr);
            ok = true;
        }
        cairo_surface_destroy(png);
    } else if (rsvg_load()) {
        void *err = NULL;
        void *handle = rsvg.new_from_file(path, &err);
        if (handle) {
            RsvgRect vp = { 0, 0, px, px };
            ok = rsvg.render_document(handle, cr, &vp, &err) != 0;
            rsvg.unref(handle);
        }
    }
    cairo_destroy(cr);
    if (!ok) {
        cairo_surface_destroy(out);
        return NULL;
    }
    return out;
}

bool icon_resolve(const char *config_icon, const char *exec, char *path, int len, bool *sym) {
    bool found = false;
    *sym = false;
    // 1) una ruta absoluta configurada manda siempre
    if (config_icon && config_icon[0] == '/') found = icon_find_file(config_icon, path, len, sym);
    // 2) el icono propio del programa: Icon= de su .desktop
    DesktopEntry de;
    if (!found && exec && desktop_entry_for_exec(exec, &de)) found = icon_find_file(de.icon, path, len, sym);
    // 3) el nombre de icono de config.json (genérico: «web-browser», «camera-web»...)
    if (!found && config_icon && config_icon[0]) found = icon_find_file(config_icon, path, len, sym);
    // 4) el nombre del ejecutable
    if (!found && exec) {
        char base[128];
        exec_basename(exec, base, sizeof(base));
        found = icon_find_file(base, path, len, sym);
    }
    if (!found) path[0] = '\0';
    return found;
}

static IconCacheEntry *lookup(const char *config_icon, const char *exec, int px) {
    char key[384];
    snprintf(key, sizeof(key), "%.150s|%.200s|%d", config_icon ? config_icon : "", exec ? exec : "", px);
    for (int i = 0; i < ICON_CACHE; i++) {
        if (cache[i].key[0] && strcmp(cache[i].key, key) == 0) return &cache[i];
    }

    IconCacheEntry *e = &cache[cache_next];
    cache_next = (cache_next + 1) % ICON_CACHE;
    if (e->surf) cairo_surface_destroy(e->surf);
    memset(e, 0, sizeof(*e));
    snprintf(e->key, sizeof(e->key), "%s", key);

    char path[512] = "";
    bool sym = false, found = icon_resolve(config_icon, exec, path, sizeof(path), &sym);
    if (found) {
        e->surf = render_file(path, px);
        e->symbolic = sym;
    }
    return e;
}

bool icon_draw_for_app(cairo_t *cr, const char *config_icon, const char *exec,
                       double cx, double cy, double size, double tint_r, double tint_g, double tint_b) {
    if (size < 4) return false;
    // Se rasteriza al tamaño en dispositivo para que se vea nítido también en la vista del editor
    double sx = 1.0, sy = 1.0;
    cairo_user_to_device_distance(cr, &sx, &sy);
    double scale = sx > 0 ? sx : 1.0;
    int px = (int)(size * scale + 0.5);
    px = (px + 3) & ~3;   // agrupa tamaños parecidos en la caché
    if (px < 8) px = 8;

    IconCacheEntry *e = lookup(config_icon, exec, px);
    if (!e->surf) return false;

    cairo_save(cr);
    cairo_translate(cr, cx - size / 2.0, cy - size / 2.0);
    cairo_scale(cr, size / px, size / px);
    if (e->symbolic) {
        cairo_set_source_rgb(cr, tint_r, tint_g, tint_b);
        cairo_mask_surface(cr, e->surf, 0, 0);
    } else {
        cairo_set_source_surface(cr, e->surf, 0, 0);
        cairo_paint(cr);
    }
    cairo_restore(cr);
    return true;
}
