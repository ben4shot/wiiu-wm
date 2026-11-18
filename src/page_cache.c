#include "page_cache.h"
#include "draw_shapes.h"
#include "icons.h"
#include "images.h"

#include <dirent.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <utime.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void page_cache_dir(char *out, int out_len) {
    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0]) snprintf(out, out_len, "%.200s/wiiu-shell", xdg);
    else snprintf(out, out_len, "%.200s/.cache/wiiu-shell", home ? home : "/tmp");
    char tmp[MAX_STR];
    snprintf(tmp, sizeof(tmp), "%s", out);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

/* ---------------------------------------------------------------- clave */

static uint64_t fnv(uint64_t h, const void *data, size_t n) {
    const unsigned char *p = data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

static uint64_t fnv_file_stat(uint64_t h, const char *path) {
    struct stat st;
    if (path && path[0] && stat(path, &st) == 0) {
        h = fnv(h, path, strlen(path));
        h = fnv(h, &st.st_mtime, sizeof(st.st_mtime));
        h = fnv(h, &st.st_size, sizeof(st.st_size));
    }
    return h;
}

static bool compute_key(const DesktopConfig *cfg, const char *config_file, char *out, int out_len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    FILE *f = fopen(config_file, "rb");
    if (!f) return false;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) h = fnv(h, buf, n);
    fclose(f);

    h = fnv_file_stat(h, "/proc/self/exe");   // otra versión del dibujo -> otra caché
    h = fnv(h, &cfg->screen_width, sizeof(int));
    h = fnv(h, &cfg->screen_height, sizeof(int));
    if (!image_is_builtin(cfg->wallpaper)) h = fnv_file_stat(h, cfg->wallpaper);
    for (int i = 0; i < cfg->num_apps; i++) {
        const AppEntry *a = &cfg->apps[i];
        if (a->style.image[0] && !image_is_builtin(a->style.image)) h = fnv_file_stat(h, a->style.image);
        // Si se instala el programa, cambia su icono
        char icon[512];
        bool sym;
        icon_resolve(a->icon, a->exec, icon, sizeof(icon), &sym);
        h = fnv_file_stat(h, icon);
        h = fnv(h, icon, strlen(icon));
    }
    snprintf(out, out_len, "%016llx", (unsigned long long)h);
    return true;
}

/* ---------------------------------------------- sprites de elemento (editor) */

// Cada elemento se rasteriza una vez a su tamaño en pantalla; mientras no cambie
// (posición, forma, estilo, imagen...) se reutiliza. Así la vista previa del editor
// solo redibuja el elemento que se está tocando.
#define SPRITE_CACHE 48
static struct {
    uint64_t key;
    cairo_surface_t *s;
    double ox, oy;        // esquina de la imagen en coordenadas de pantalla
} sprites[SPRITE_CACHE];
static int sprite_next;

static double sprite_margin_x(const AppEntry *a) {
    return 10 + a->style.border_width + (a->style.label == LABEL_BELOW ? 50 : 0);
}
static double sprite_margin_bottom(const AppEntry *a) {
    return 12 + a->style.border_width + (a->style.label == LABEL_BELOW ? a->style.font_size * 2 + 6 : 0);
}

static void draw_sprite(cairo_t *cr, const AppEntry *a, const char *font, double scale) {
    uint64_t key = fnv(0xcbf29ce484222325ULL, a, sizeof(*a));
    key = fnv(key, &scale, sizeof(scale));
    key = fnv(key, font, strlen(font));
    int slot = -1;
    for (int i = 0; i < SPRITE_CACHE; i++) if (sprites[i].s && sprites[i].key == key) { slot = i; break; }
    if (slot < 0) {
        slot = sprite_next;
        sprite_next = (sprite_next + 1) % SPRITE_CACHE;
        if (sprites[slot].s) cairo_surface_destroy(sprites[slot].s);
        double mx = sprite_margin_x(a), mt = 10 + a->style.border_width, mb = sprite_margin_bottom(a);
        double ox = a->x - mx, oy = a->y - mt;
        int w = (int)ceil((a->w + 2 * mx) * scale), h = (int)ceil((a->h + mt + mb) * scale);
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w > 0 ? w : 1, h > 0 ? h : 1);
        cairo_t *c = cairo_create(s);
        cairo_scale(c, scale, scale);
        cairo_translate(c, -ox, -oy);
        draw_element(c, a, font, false, false, NULL);
        cairo_destroy(c);
        sprites[slot].key = key;
        sprites[slot].s = s;
        sprites[slot].ox = ox;
        sprites[slot].oy = oy;
    }
    cairo_save(cr);
    cairo_translate(cr, sprites[slot].ox, sprites[slot].oy);
    cairo_scale(cr, 1.0 / scale, 1.0 / scale);
    cairo_set_source_surface(cr, sprites[slot].s, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

// Fondo de pantalla rasterizado una vez por tamaño/configuración
static struct { uint64_t key; cairo_surface_t *s; } wall;

void page_render_fast(cairo_t *cr, const DesktopConfig *cfg, int page, double scale) {
    uint64_t key = fnv(0xcbf29ce484222325ULL, cfg->wallpaper, strlen(cfg->wallpaper));
    key = fnv(key, &cfg->bg_color, sizeof(ColorRGBA) * 2);
    key = fnv(key, &cfg->accent, sizeof(ColorRGBA));
    key = fnv(key, &scale, sizeof(scale));
    if (!wall.s || wall.key != key) {
        if (wall.s) cairo_surface_destroy(wall.s);
        int w = (int)ceil(cfg->screen_width * scale), h = (int)ceil(cfg->screen_height * scale);
        wall.s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
        cairo_t *c = cairo_create(wall.s);
        cairo_scale(c, scale, scale);
        image_draw_wallpaper(c, cfg->wallpaper, cfg->screen_width, cfg->screen_height,
                             &cfg->bg_color, &cfg->bg_gradient, &cfg->accent);
        cairo_destroy(c);
        wall.key = key;
    }
    cairo_save(cr);
    cairo_scale(cr, 1.0 / scale, 1.0 / scale);
    cairo_set_source_surface(cr, wall.s, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    for (int i = 0; i < cfg->num_apps; i++) {
        if (cfg->apps[i].page == page) draw_sprite(cr, &cfg->apps[i], cfg->font, scale);
    }
}

/* ------------------------------------------------------------- dibujo */

void page_render(cairo_t *cr, const DesktopConfig *cfg, int page, bool with_wallpaper) {
    if (with_wallpaper) {
        image_draw_wallpaper(cr, cfg->wallpaper, cfg->screen_width, cfg->screen_height,
                             &cfg->bg_color, &cfg->bg_gradient, &cfg->accent);
    }
    for (int i = 0; i < cfg->num_apps; i++) {
        if (cfg->apps[i].page == page) draw_element(cr, &cfg->apps[i], cfg->font, false, false, NULL);
    }
}

static cairo_surface_t *render_surface(const DesktopConfig *cfg, int page) {
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, cfg->screen_width, cfg->screen_height);
    cairo_t *cr = cairo_create(s);
    if (page >= 0) page_render(cr, cfg, page, true);
    else image_draw_wallpaper(cr, cfg->wallpaper, cfg->screen_width, cfg->screen_height,
                              &cfg->bg_color, &cfg->bg_gradient, &cfg->accent);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    return s;
}

static void file_name(char *out, int len, const char *dir, const char *key, int page) {
    if (page >= 0) snprintf(out, len, "%s/page-%s-%d.png", dir, key, page);
    else snprintf(out, len, "%s/bg-%s.png", dir, key);
}

// Borra las imágenes de otras claves que llevan más de 7 días sin usarse (las de otras carpetas
// del Inicio conviven: abrir una carpeta no debe borrar la caché del Inicio ni de las demás)
#define CACHE_MAX_AGE (7 * 24 * 3600)
static void clean_old(const char *dir, const char *key) {
    DIR *d = opendir(dir);
    if (!d) return;
    time_t now = time(NULL);
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "page-", 5) != 0 && strncmp(de->d_name, "bg-", 3) != 0) continue;
        if (strstr(de->d_name, key)) continue;
        char p[MAX_STR + 64];
        snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && now - st.st_mtime > CACHE_MAX_AGE) unlink(p);
    }
    closedir(d);
}

static bool write_png_atomic(cairo_surface_t *s, const char *path) {
    char tmp[MAX_STR + 80];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (cairo_surface_write_to_png(s, tmp) != CAIRO_STATUS_SUCCESS) return false;
    return rename(tmp, path) == 0;
}

bool page_cache_build_files(const DesktopConfig *cfg, const char *config_file) {
    char key[24], dir[MAX_STR];
    if (!compute_key(cfg, config_file, key, sizeof(key))) return false;
    page_cache_dir(dir, sizeof(dir));
    int pages = config_page_count(cfg);
    bool ok = true;
    for (int p = -1; p < pages && p < PAGE_CACHE_MAX; p++) {
        char path[MAX_STR + 64];
        file_name(path, sizeof(path), dir, key, p);
        if (access(path, R_OK) == 0) continue;
        cairo_surface_t *s = render_surface(cfg, p);
        ok &= write_png_atomic(s, path);
        cairo_surface_destroy(s);
    }
    clean_old(dir, key);
    return ok;
}

/* ---------------------------------------------------------- en memoria */

void page_cache_init(PageCache *pc) {
    memset(pc, 0, sizeof(*pc));
}

void page_cache_free(PageCache *pc) {
    for (int i = 0; i < PAGE_CACHE_MAX; i++) {
        if (pc->pages[i]) cairo_surface_destroy(pc->pages[i]);
        pc->pages[i] = NULL;
    }
    if (pc->wallpaper) cairo_surface_destroy(pc->wallpaper);
    pc->wallpaper = NULL;
    pc->npages = 0;
    pc->key[0] = '\0';
    pc->building = false;          // una carga a medias se descarta (las páginas ya guardadas siguen en disco)
    pc->bcfg = NULL;
    pc->bsave_wall = false;
    memset(pc->bcursor, 0, sizeof(pc->bcursor));
    memset(pc->bdone, 0, sizeof(pc->bdone));
}

static cairo_surface_t *load_png(const char *path, int w, int h) {
    cairo_surface_t *s = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) utime(path, NULL);   // «usada hoy»: no se limpia
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS ||
        cairo_image_surface_get_width(s) != w || cairo_image_surface_get_height(s) != h) {
        cairo_surface_destroy(s);
        return NULL;
    }
    return s;
}

bool page_cache_prepare(PageCache *pc, const DesktopConfig *cfg, const char *config_file) {
    char key[24], dir[MAX_STR];
    if (!compute_key(cfg, config_file, key, sizeof(key))) {
        page_cache_free(pc);
        return false;
    }
    if (strcmp(key, pc->key) == 0 && pc->wallpaper) return true;   // ya está cargada

    page_cache_free(pc);
    page_cache_dir(dir, sizeof(dir));
    int pages = config_page_count(cfg);
    if (pages > PAGE_CACHE_MAX) pages = PAGE_CACHE_MAX;
    int built = 0;
    for (int p = -1; p < pages; p++) {
        char path[MAX_STR + 64];
        file_name(path, sizeof(path), dir, key, p);
        cairo_surface_t *s = load_png(path, cfg->screen_width, cfg->screen_height);
        if (!s) {
            s = render_surface(cfg, p);
            write_png_atomic(s, path);
            built++;
        }
        if (p < 0) pc->wallpaper = s;
        else pc->pages[p] = s;
    }
    pc->npages = pages;
    snprintf(pc->key, sizeof(pc->key), "%s", key);
    if (built) {
        clean_old(dir, key);
        printf("[Caché] %d imagen(es) de página generadas (%s)\n", built, key);
    } else {
        printf("[Caché] Páginas cargadas de %s (%s)\n", dir, key);
    }
    return true;
}

cairo_surface_t *page_cache_page(const PageCache *pc, int page) {
    if (page < 0 || page >= pc->npages) return NULL;
    return pc->pages[page];
}

/* ------------------------------------------------- carga incremental */

// Páginas completas recordadas en memoria (el Inicio y las carpetas ya abiertas): reabrirlas no cuesta nada.
// Cada entrada guarda su propia referencia a las superficies.
#define LRU_MAX 6
static struct {
    char key[24];
    int npages;
    cairo_surface_t *pages[PAGE_CACHE_MAX];
    cairo_surface_t *wall;
    unsigned long used;
} lru[LRU_MAX];
static unsigned long lru_clock;

static void lru_clear_entry(int i) {
    for (int p = 0; p < PAGE_CACHE_MAX; p++) {
        if (lru[i].pages[p]) cairo_surface_destroy(lru[i].pages[p]);
        lru[i].pages[p] = NULL;
    }
    if (lru[i].wall) cairo_surface_destroy(lru[i].wall);
    lru[i].wall = NULL;
    lru[i].key[0] = '\0';
    lru[i].npages = 0;
}

void page_cache_forget_memory(void) {
    for (int i = 0; i < LRU_MAX; i++) lru_clear_entry(i);
}

static void lru_put(const PageCache *pc) {
    int slot = 0;
    for (int i = 0; i < LRU_MAX; i++) {
        if (strcmp(lru[i].key, pc->key) == 0) { slot = i; goto fill; }
        if (!lru[i].key[0]) { slot = i; goto fill; }
        if (lru[i].used < lru[slot].used) slot = i;
    }
fill:
    lru_clear_entry(slot);
    snprintf(lru[slot].key, sizeof(lru[slot].key), "%s", pc->key);
    lru[slot].npages = pc->npages;
    for (int p = 0; p < pc->npages; p++) lru[slot].pages[p] = cairo_surface_reference(pc->pages[p]);
    lru[slot].wall = cairo_surface_reference(pc->wallpaper);
    lru[slot].used = ++lru_clock;
}

static bool lru_get(PageCache *pc, const char *key) {
    for (int i = 0; i < LRU_MAX; i++) {
        if (!lru[i].key[0] || strcmp(lru[i].key, key) != 0) continue;
        lru[i].used = ++lru_clock;
        pc->npages = lru[i].npages;
        for (int p = 0; p < lru[i].npages; p++) pc->pages[p] = cairo_surface_reference(lru[i].pages[p]);
        pc->wallpaper = cairo_surface_reference(lru[i].wall);
        snprintf(pc->key, sizeof(pc->key), "%s", key);
        return true;
    }
    return false;
}

// Guardado de PNG en segundo plano, con prioridad baja: no compite con el hilo del compositor
static pthread_mutex_t writes_lock = PTHREAD_MUTEX_INITIALIZER;
static int writes_inflight;

struct png_job { cairo_surface_t *surf; char path[MAX_STR + 80]; };

static void *png_writer(void *arg) {
    struct png_job *j = arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    write_png_atomic(j->surf, j->path);
    cairo_surface_destroy(j->surf);
    free(j);
    pthread_mutex_lock(&writes_lock);
    writes_inflight--;
    pthread_mutex_unlock(&writes_lock);
    return NULL;
}

static void save_async(cairo_surface_t *surf, const char *path) {
    struct png_job *j = malloc(sizeof(*j));
    if (!j) return;
    j->surf = cairo_surface_reference(surf);
    snprintf(j->path, sizeof(j->path), "%s", path);
    pthread_mutex_lock(&writes_lock);
    writes_inflight++;
    pthread_mutex_unlock(&writes_lock);
    pthread_t t;
    if (pthread_create(&t, NULL, png_writer, j) != 0) {
        png_writer(j);   // sin hilos: se guarda aquí
        return;
    }
    pthread_detach(t);
}

void page_cache_wait_writes(void) {
    for (;;) {
        pthread_mutex_lock(&writes_lock);
        int n = writes_inflight;
        pthread_mutex_unlock(&writes_lock);
        if (n <= 0) return;
        usleep(2000);
    }
}

static cairo_surface_t *copy_surface(cairo_surface_t *src, int w, int h) {
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cairo_t *cr = cairo_create(s);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    return s;
}

bool page_cache_pending(const PageCache *pc) {
    return pc->building;
}

bool page_cache_begin(PageCache *pc, const DesktopConfig *cfg, const char *config_file) {
    char key[24];
    if (!compute_key(cfg, config_file, key, sizeof(key))) {
        page_cache_free(pc);
        return false;
    }
    if (strcmp(key, pc->key) == 0 && pc->wallpaper) return true;   // ya está (completa o en curso)

    page_cache_free(pc);
    if (lru_get(pc, key)) return true;

    page_cache_dir(pc->bdir, sizeof(pc->bdir));
    int pages = config_page_count(cfg);
    if (pages > PAGE_CACHE_MAX) pages = PAGE_CACHE_MAX;
    int missing = 0;
    char path[MAX_STR + 64];
    file_name(path, sizeof(path), pc->bdir, key, -1);
    pc->wallpaper = load_png(path, cfg->screen_width, cfg->screen_height);
    if (!pc->wallpaper) {
        pc->wallpaper = render_surface(cfg, -1);   // solo el degradado/fondo: barato
        pc->bsave_wall = true;
        missing++;
    }
    for (int p = 0; p < pages; p++) {
        file_name(path, sizeof(path), pc->bdir, key, p);
        pc->pages[p] = load_png(path, cfg->screen_width, cfg->screen_height);
        pc->bdone[p] = pc->pages[p] != NULL;
        pc->bcursor[p] = 0;
        if (!pc->pages[p]) {
            pc->pages[p] = copy_surface(pc->wallpaper, cfg->screen_width, cfg->screen_height);
            missing++;
        }
    }
    pc->npages = pages;
    snprintf(pc->key, sizeof(pc->key), "%s", key);
    pc->bcfg = cfg;
    pc->building = false;
    for (int p = 0; p < pages; p++) if (!pc->bdone[p]) pc->building = true;
    if (!pc->building) {
        if (pc->bsave_wall) { file_name(path, sizeof(path), pc->bdir, key, -1); save_async(pc->wallpaper, path); pc->bsave_wall = false; }
        lru_put(pc);
    }
    return true;
}

bool page_cache_step(PageCache *pc, int visible_page, bool *visible_changed) {
    if (visible_changed) *visible_changed = false;
    if (!pc->building) return false;
    const DesktopConfig *cfg = pc->bcfg;
    int p = -1;
    if (visible_page >= 0 && visible_page < pc->npages && !pc->bdone[visible_page]) p = visible_page;
    else for (int i = 0; i < pc->npages; i++) if (!pc->bdone[i]) { p = i; break; }
    if (p < 0) { pc->building = false; return false; }

    int i = pc->bcursor[p];
    while (i < cfg->num_apps && cfg->apps[i].page != p) i++;
    if (i < cfg->num_apps) {
        cairo_t *cr = cairo_create(pc->pages[p]);
        draw_element(cr, &cfg->apps[i], cfg->font, false, false, NULL);
        cairo_destroy(cr);
        cairo_surface_flush(pc->pages[p]);
        pc->bcursor[p] = i + 1;
        if (visible_changed && p == visible_page) *visible_changed = true;
        return true;
    }
    // Página terminada: guardarla en segundo plano
    pc->bdone[p] = true;
    char path[MAX_STR + 64];
    file_name(path, sizeof(path), pc->bdir, pc->key, p);
    save_async(pc->pages[p], path);
    for (int k = 0; k < pc->npages; k++) if (!pc->bdone[k]) return true;
    pc->building = false;
    if (pc->bsave_wall) {
        file_name(path, sizeof(path), pc->bdir, pc->key, -1);
        save_async(pc->wallpaper, path);
        pc->bsave_wall = false;
    }
    clean_old(pc->bdir, pc->key);
    lru_put(pc);
    return false;
}
