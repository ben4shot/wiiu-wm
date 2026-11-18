#include "images.h"
#include "draw_shapes.h"

#include <dirent.h>
#include <dlfcn.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------- gdk-pixbuf con dlopen */

typedef void *(*fn_new_at_scale)(const char *, int, int, int, void **);
typedef int (*fn_get_int)(const void *);
typedef unsigned char *(*fn_get_pixels)(const void *);
typedef void (*fn_unref)(void *);

static struct {
    bool tried, ok;
    fn_new_at_scale new_at_scale;
    fn_get_int width, height, rowstride, n_channels, has_alpha;
    fn_get_pixels pixels;
    fn_unref unref;
} pb;

static bool pixbuf_load(void) {
    if (pb.tried) return pb.ok;
    pb.tried = true;
    void *h = dlopen("libgdk_pixbuf-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        fprintf(stderr, "[Imágenes] gdk-pixbuf no disponible: solo PNG\n");
        return false;
    }
    pb.new_at_scale = (fn_new_at_scale)dlsym(h, "gdk_pixbuf_new_from_file_at_scale");
    pb.width = (fn_get_int)dlsym(h, "gdk_pixbuf_get_width");
    pb.height = (fn_get_int)dlsym(h, "gdk_pixbuf_get_height");
    pb.rowstride = (fn_get_int)dlsym(h, "gdk_pixbuf_get_rowstride");
    pb.n_channels = (fn_get_int)dlsym(h, "gdk_pixbuf_get_n_channels");
    pb.has_alpha = (fn_get_int)dlsym(h, "gdk_pixbuf_get_has_alpha");
    pb.pixels = (fn_get_pixels)dlsym(h, "gdk_pixbuf_get_pixels");
    pb.unref = (fn_unref)dlsym(RTLD_DEFAULT, "g_object_unref");
    pb.ok = pb.new_at_scale && pb.width && pb.height && pb.rowstride && pb.n_channels &&
            pb.has_alpha && pb.pixels && pb.unref;
    return pb.ok;
}

// GdkPixbuf (RGB/RGBA sin premultiplicar) -> superficie Cairo ARGB32 premultiplicada
static cairo_surface_t *pixbuf_to_surface(void *pix) {
    int w = pb.width(pix), h = pb.height(pix), rs = pb.rowstride(pix), nc = pb.n_channels(pix);
    bool alpha = pb.has_alpha(pix);
    const unsigned char *src = pb.pixels(pix);
    cairo_surface_t *s = cairo_image_surface_create(alpha ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24, w, h);
    unsigned char *dst = cairo_image_surface_get_data(s);
    int ds = cairo_image_surface_get_stride(s);
    for (int y = 0; y < h; y++) {
        const unsigned char *p = src + y * rs;
        uint32_t *q = (uint32_t *)(dst + y * ds);
        for (int x = 0; x < w; x++, p += nc) {
            unsigned a = alpha ? p[3] : 255;
            unsigned r = p[0] * a / 255, g = p[1] * a / 255, b = p[2] * a / 255;
            q[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(s);
    return s;
}

static cairo_surface_t *load_scaled(const char *path, int max_w, int max_h) {
    if (pixbuf_load()) {
        void *err = NULL;
        void *pix = pb.new_at_scale(path, max_w, max_h, 1, &err);
        if (pix) {
            cairo_surface_t *s = pixbuf_to_surface(pix);
            pb.unref(pix);
            return s;
        }
    }
    // Sin gdk-pixbuf: PNG con Cairo, escalado a mano
    size_t l = strlen(path);
    if (l < 4 || strcasecmp(path + l - 4, ".png") != 0) return NULL;
    cairo_surface_t *png = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(png) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(png);
        return NULL;
    }
    int w = cairo_image_surface_get_width(png), h = cairo_image_surface_get_height(png);
    double sc = fmin(1.0, fmin((double)max_w / w, (double)max_h / h));
    if (sc >= 1.0) return png;
    int nw = (int)(w * sc + 0.5), nh = (int)(h * sc + 0.5);
    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, nw > 0 ? nw : 1, nh > 0 ? nh : 1);
    cairo_t *cr = cairo_create(out);
    cairo_scale(cr, sc, sc);
    cairo_set_source_surface(cr, png, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(png);
    return out;
}

/* ---------------------------------------------------------------- caché */

#define IMG_CACHE 24
static struct { char key[MAX_STR + 16]; cairo_surface_t *s; } cache[IMG_CACHE];
static int cache_next;

cairo_surface_t *image_get(const char *path, int max_w, int max_h) {
    if (!path || !path[0] || image_is_builtin(path)) return NULL;
    char key[MAX_STR + 16];
    snprintf(key, sizeof(key), "%s|%d|%d", path, max_w, max_h);
    for (int i = 0; i < IMG_CACHE; i++) {
        if (cache[i].key[0] && strcmp(cache[i].key, key) == 0) return cache[i].s;
    }
    int i = cache_next;
    cache_next = (cache_next + 1) % IMG_CACHE;
    if (cache[i].s) cairo_surface_destroy(cache[i].s);
    snprintf(cache[i].key, sizeof(cache[i].key), "%s", key);
    cache[i].s = load_scaled(path, max_w, max_h);
    if (!cache[i].s) fprintf(stderr, "[Imágenes] No se pudo cargar '%s'\n", path);
    return cache[i].s;
}

void images_clear_cache(void) {
    for (int i = 0; i < IMG_CACHE; i++) {
        if (cache[i].s) cairo_surface_destroy(cache[i].s);
        cache[i].s = NULL;
        cache[i].key[0] = '\0';
    }
}

/* -------------------------------------------------------------- listas */

static const char *BUILTIN_ELEMENT[] = { "builtin:burst", "builtin:bubble", "builtin:cloud" };
static const char *BUILTIN_WALL[] = { "builtin:comic", "builtin:halftone" };

bool image_is_builtin(const char *path) {
    return path && strncmp(path, "builtin:", 8) == 0;
}

void image_display_name(const char *path, char *out, int out_len) {
    if (!path || !path[0]) { snprintf(out, out_len, "Ninguna"); return; }
    if (strcmp(path, "builtin:burst") == 0) { snprintf(out, out_len, "Estallido"); return; }
    if (strcmp(path, "builtin:bubble") == 0) { snprintf(out, out_len, "Bocadillo"); return; }
    if (strcmp(path, "builtin:cloud") == 0) { snprintf(out, out_len, "Nube"); return; }
    if (strcmp(path, "builtin:comic") == 0) { snprintf(out, out_len, "Cómic (rayos)"); return; }
    if (strcmp(path, "builtin:halftone") == 0) { snprintf(out, out_len, "Trama"); return; }
    const char *b = strrchr(path, '/');
    snprintf(out, out_len, "%s", b ? b + 1 : path);
}

void image_user_dir(char *out, int out_len) {
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0]) snprintf(out, out_len, "%.200s/wiiu-shell/images", xdg);
    else snprintf(out, out_len, "%.200s/.config/wiiu-shell/images", home ? home : "/tmp");
    char tmp[MAX_STR];
    snprintf(tmp, sizeof(tmp), "%s", out);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

static bool is_image_name(const char *n) {
    const char *dot = strrchr(n, '.');
    if (!dot) return false;
    const char *ok[] = { ".png", ".jpg", ".jpeg", ".webp", ".svg", ".gif", ".bmp", ".tif", ".tiff" };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) if (strcasecmp(dot, ok[i]) == 0) return true;
    return false;
}

static int cmp_str(const void *a, const void *b) {
    return strcasecmp((const char *)a, (const char *)b);
}

static void scan_dir(const char *dir, int depth, char (*out)[MAX_STR], int *n, int max) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    int start = *n;
    while ((de = readdir(d)) && *n < max) {
        if (de->d_name[0] == '.') continue;
        char path[MAX_STR];
        snprintf(path, sizeof(path), "%.180s/%.70s", dir, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0) scan_dir(path, depth - 1, out, n, max);
        } else if (is_image_name(de->d_name)) {
            snprintf(out[(*n)++], MAX_STR, "%s", path);
        }
    }
    closedir(d);
    qsort(out + start, *n - start, MAX_STR, cmp_str);
}

int image_list(char (*out)[MAX_STR], int max, bool wallpaper) {
    int n = 0;
    if (n < max) out[n++][0] = '\0';   // «Ninguna»
    if (wallpaper) {
        for (size_t i = 0; i < sizeof(BUILTIN_WALL) / sizeof(BUILTIN_WALL[0]) && n < max; i++)
            snprintf(out[n++], MAX_STR, "%s", BUILTIN_WALL[i]);
    } else {
        for (size_t i = 0; i < sizeof(BUILTIN_ELEMENT) / sizeof(BUILTIN_ELEMENT[0]) && n < max; i++)
            snprintf(out[n++], MAX_STR, "%s", BUILTIN_ELEMENT[i]);
    }
    char user[MAX_STR];
    image_user_dir(user, sizeof(user));
    const char *home = getenv("HOME");
    char dirs[6][MAX_STR];
    int nd = 0;
    snprintf(dirs[nd++], MAX_STR, "%s", user);
    if (home) {
        snprintf(dirs[nd++], MAX_STR, "%.200s/Imágenes", home);
        snprintf(dirs[nd++], MAX_STR, "%.200s/Pictures", home);
    }
    snprintf(dirs[nd++], MAX_STR, "/usr/share/backgrounds");
    snprintf(dirs[nd++], MAX_STR, "/usr/share/wallpapers");
    snprintf(dirs[nd++], MAX_STR, "/usr/share/rpd-wallpaper");
    for (int i = 0; i < nd && n < max; i++) scan_dir(dirs[i], 2, out, &n, max);
    return n;
}

/* ---------------------------------------------------- dibujos incluidos */

void image_fill_cover(cairo_t *cr, cairo_surface_t *img, double x, double y, double w, double h) {
    int iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
    if (iw <= 0 || ih <= 0) return;
    double sc = fmax(w / iw, h / ih);
    cairo_save(cr);
    cairo_clip_preserve(cr);
    cairo_translate(cr, x + (w - iw * sc) / 2, y + (h - ih * sc) / 2);
    cairo_scale(cr, sc, sc);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
}

void image_draw_builtin_element(cairo_t *cr, const char *name, double x, double y, double w, double h,
                                const ColorRGBA *bg, const ColorRGBA *ink) {
    cairo_save(cr);
    cairo_new_path(cr);
    if (strcmp(name, "builtin:burst") == 0) shape_path_burst(cr, x, y, w, h, 12);
    else if (strcmp(name, "builtin:bubble") == 0) shape_path_bubble(cr, x, y, w, h, 125);
    else if (strcmp(name, "builtin:cloud") == 0) shape_path_cloud(cr, x, y, w, h, 9);
    else { cairo_restore(cr); return; }
    // Versión clara del color del elemento, con contorno de tinta
    cairo_set_source_rgba(cr, bg->r + (1 - bg->r) * 0.35, bg->g + (1 - bg->g) * 0.35, bg->b + (1 - bg->b) * 0.35, 1);
    cairo_fill_preserve(cr);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, 2.5);
    cairo_set_source_rgba(cr, ink->r, ink->g, ink->b, 0.9);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void image_draw_wallpaper(cairo_t *cr, const char *wallpaper, int width, int height,
                          const ColorRGBA *base, const ColorRGBA *grad, const ColorRGBA *accent) {
    if (wallpaper && wallpaper[0] && !image_is_builtin(wallpaper)) {
        cairo_surface_t *img = image_get(wallpaper, width * 2, height * 2);
        if (img) {
            cairo_save(cr);
            cairo_set_source_rgb(cr, 0, 0, 0);
            cairo_paint(cr);
            cairo_rectangle(cr, 0, 0, width, height);
            image_fill_cover(cr, img, 0, 0, width, height);
            cairo_new_path(cr);
            cairo_restore(cr);
            return;
        }
    }
    draw_background_wiiu(cr, width, height, base, grad);
    if (!wallpaper) return;

    if (strcmp(wallpaper, "builtin:comic") == 0) {
        // Rayos de acción desde el centro, como en una viñeta de impacto
        cairo_save(cr);
        double cx = width / 2.0, cy = height / 2.0, R = hypot(width, height);
        int rays = 36;
        for (int i = 0; i < rays; i++) {
            double a0 = 2 * M_PI * i / rays, a1 = a0 + M_PI / rays * 0.55;
            cairo_move_to(cr, cx, cy);
            cairo_line_to(cr, cx + R * cos(a0), cy + R * sin(a0));
            cairo_line_to(cr, cx + R * cos(a1), cy + R * sin(a1));
            cairo_close_path(cr);
        }
        cairo_set_source_rgba(cr, accent->r, accent->g, accent->b, 0.16);
        cairo_fill(cr);
        cairo_restore(cr);
    } else if (strcmp(wallpaper, "builtin:halftone") == 0) {
        cairo_save(cr);
        cairo_set_source_rgba(cr, accent->r, accent->g, accent->b, 0.22);
        for (int y = 0, row = 0; y < height + 12; y += 12, row++) {
            for (int x = (row % 2) * 6; x < width + 12; x += 12) {
                double t = hypot(x - width * 0.85, y - height * 0.15) / hypot(width, height);
                double r = 4.2 * (1.0 - t);
                if (r < 0.4) continue;
                cairo_new_sub_path(cr);
                cairo_arc(cr, x, y, r, 0, 2 * M_PI);
            }
        }
        cairo_fill(cr);
        cairo_restore(cr);
    }
}
