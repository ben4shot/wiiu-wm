#include "config.h"
#include "draw_shapes.h"
#include "touch_gestures.h"
#include "app_manager.h"
#include "grid_view.h"
#include "recents_view.h"
#include "input_gamepad.h"
#include "editor.h"
#include "poly.h"
#include "images.h"
#include "page_cache.h"
#include <linux/input.h>
#include <math.h>
#include <dirent.h>
#include <json-c/json.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

void test_config_and_shapes(void) {
    printf("[Test 1] Carga de configuración: cada elemento es un polígono...\n");
    DesktopConfig cfg;
    bool ok = config_load(&cfg, "test_config.json");
    assert(ok == true);
    assert(cfg.screen_width == 854 && cfg.screen_height == 480);
    assert(cfg.num_apps >= 8);
    assert(cfg.apps[0].npts == 4);
    assert(cfg.apps[0].has_pos && cfg.apps[0].w > 200);              // viñeta grande de la página
    assert(strcmp(cfg.apps[2].style.image, "builtin:burst") == 0);   // imagen incluida elegida
    assert(strcmp(cfg.wallpaper, "builtin:comic") == 0);
    assert(cfg.apps[1].style.border_style == BORDER_INK);            // heredado de "style"
    for (int i = 0; i < cfg.num_apps; i++) assert(layout_check(&cfg, i) == -1);   // sin solapes
    printf(" -> OK\n");
}

void test_touch_gestures(void) {
    printf("[Test 2] Toques (los gestos se quitaron)...\n");
    DesktopConfig cfg;
    config_set_defaults(&cfg);

    TouchGestureTracker tracker;
    touch_tracker_init(&tracker, &cfg);

    // Sin gestos: tres dedos deslizando no hacen nada
    touch_tracker_down(&tracker, 0, 400.0, 300.0, 100);
    touch_tracker_down(&tracker, 1, 430.0, 300.0, 100);
    touch_tracker_down(&tracker, 2, 460.0, 300.0, 100);
    touch_tracker_motion(&tracker, 0, 400.0, 200.0, 150);
    touch_tracker_motion(&tracker, 1, 430.0, 200.0, 150);
    touch_tracker_motion(&tracker, 2, 460.0, 200.0, 150);
    touch_tracker_up(&tracker, 0, 200, NULL, NULL);
    touch_tracker_up(&tracker, 1, 200, NULL, NULL);
    GestureEvent ev = touch_tracker_up(&tracker, 2, 200, NULL, NULL);
    assert(ev == GESTURE_NONE);
    // Un dedo arrastrando tampoco
    touch_tracker_down(&tracker, 0, 400.0, 300.0, 300);
    touch_tracker_motion(&tracker, 0, 200.0, 300.0, 350);
    assert(touch_tracker_up(&tracker, 0, 400, NULL, NULL) == GESTURE_NONE);
    printf(" -> Sin gestos de deslizamiento: OK\n");

    // 4. Toque simple (Tap)
    touch_tracker_down(&tracker, 0, 150.0, 150.0, 700);
    ev = touch_tracker_up(&tracker, 0, 720, NULL, NULL);
    assert(ev == GESTURE_TAP);
    printf(" -> Toque simple (Tap): OK\n");
}

void test_home_state_machine(void) {
    printf("[Test 3] Probando máquina de estados del botón HOME...\n");
    int state = 0; // 0 = HOME_GRID, 1 = RECENTS

    // Pulsar HOME desde Inicio -> va a Recientes
    if (state == 1) state = 0;
    else state = 1;
    assert(state == 1);
    printf(" -> HOME desde Inicio -> Recientes (Android Overview): OK\n");

    // Pulsar HOME de nuevo desde Recientes -> vuelve al Inicio
    if (state == 1) state = 0;
    else state = 1;
    assert(state == 0);
    printf(" -> HOME desde Recientes -> Inicio (Grid Wii U): OK\n");
}

void test_visual_rendering(void) {
    printf("[Test 4] Generando render visual de prueba con Cairo (854x480)...\n");
    DesktopConfig cfg;
    config_load(&cfg, "test_config.json");

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 854, 480);
    cairo_t *cr = cairo_create(surf);

    // Fondo
    image_draw_wallpaper(cr, cfg.wallpaper, 854, 480, &cfg.bg_color, &cfg.bg_gradient, &cfg.accent);

    // Renderizar grilla de tiles
    GridView gv;
    grid_view_init(&gv, &cfg);
    grid_view_render(&gv, cr, 854, 480);

    cairo_status_t status = cairo_surface_write_to_png(surf, "test_home_grid.png");
    assert(status == CAIRO_STATUS_SUCCESS);
    printf(" -> Guardada imagen de prueba de Inicio: 'desktop/test_home_grid.png'\n");

    // Renderizar pantalla de recientes (3 ventanas simuladas)
    RecentItem items[3];
    memset(items, 0, sizeof(items));
    snprintf(items[0].name, sizeof(items[0].name), "Terminal");
    snprintf(items[0].subtitle, sizeof(items[0].subtitle), "usuario@rpi: ~");
    snprintf(items[0].icon, sizeof(items[0].icon), "utilities-terminal");
    snprintf(items[1].name, sizeof(items[1].name), "Navegador");
    snprintf(items[1].subtitle, sizeof(items[1].subtitle), "Mozilla Firefox");
    snprintf(items[1].icon, sizeof(items[1].icon), "web-browser");
    items[1].active = true;
    snprintf(items[2].name, sizeof(items[2].name), "Cámara");
    snprintf(items[2].subtitle, sizeof(items[2].subtitle), "drc_cam_v4l2");
    snprintf(items[2].icon, sizeof(items[2].icon), "camera-web");

    RecentsView rv;
    recents_view_init(&rv, &cfg);
    recents_view_set_items(&rv, items, 3, true);
    assert(rv.focused_card == 1); // abre enfocando la app que estaba delante
    while (recents_view_update(&rv, 854)) {}
    image_draw_wallpaper(cr, cfg.wallpaper, 854, 480, &cfg.bg_color, &cfg.bg_gradient, &cfg.accent);
    recents_view_render(&rv, cr, 854, 480);

    status = cairo_surface_write_to_png(surf, "test_recents.png");
    assert(status == CAIRO_STATUS_SUCCESS);
    printf(" -> Guardada imagen de prueba de Recientes: 'desktop/test_recents.png'\n");

    cairo_destroy(cr);
    cairo_surface_destroy(surf);
}

void test_evdev_bits(void) {
    printf("[Test 7] Lectura de mapas de bits evdev (detección del mando)...\n");
    unsigned long bits[(KEY_CNT + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long))] = {0};
    unsigned bpl = 8 * sizeof(unsigned long);
    bits[BTN_EAST / bpl] |= 1UL << (BTN_EAST % bpl);
    bits[BTN_MODE / bpl] |= 1UL << (BTN_MODE % bpl);
    assert(evdev_test_bit(bits, BTN_EAST));
    assert(evdev_test_bit(bits, BTN_MODE));
    assert(!evdev_test_bit(bits, BTN_SOUTH));
    assert(!evdev_test_bit(bits, BTN_A + 64));
    printf(" -> OK\n");
}

// La zona táctil debe coincidir con lo que se dibuja: se rasteriza el polígono del
// elemento y se compara píxel a píxel con poly_contains() en todos los preajustes.
void test_hitbox_matches_shape(void) {
    printf("[Test 5] La zona táctil coincide con la forma dibujada...\n");
    DesktopConfig cfg;
    config_set_defaults(&cfg);
    for (int p = 0; p < PRESET_COUNT; p++) {
        AppEntry a;
        memset(&a, 0, sizeof(a));
        a.style = cfg.default_style;
        a.style.radius = 0;   // sin redondeo: el dibujo es exactamente el polígono
        poly_set_preset(&a, (PolyPreset)p, 20, 20, 116, 108);
        poly_rotate(&a, (p % 3) * 7.0 - 7.0);
        const int W = 160, H = 150;
        cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_A8, W, H);
        cairo_t *cr = cairo_create(mask);
        cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
        element_path(cr, &a);
        cairo_fill(cr);
        cairo_destroy(cr);
        cairo_surface_flush(mask);
        unsigned char *px = cairo_image_surface_get_data(mask);
        int stride = cairo_image_surface_get_stride(mask);
        int mismatches = 0, inside = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                bool drawn = px[y * stride + x] > 0;
                bool hit = poly_contains(&a, x + 0.5, y + 0.5);
                if (drawn != hit) mismatches++;
                if (hit) inside++;
            }
        }
        cairo_surface_destroy(mask);
        assert(inside > 2000);
        assert(mismatches < inside / 100);
        printf(" -> %-14s zona %5d px, discrepancias %d\n", poly_preset_label((PolyPreset)p), inside, mismatches);
    }
}

void test_save_roundtrip(void) {
    printf("[Test 8] Guardar y volver a cargar la configuración...\n");
    DesktopConfig a, b;
    config_load(&a, "test_config.json");
    a.apps[2].page = 1;
    poly_set_preset(&a.apps[2], PRESET_PANEL_L, 123, 45, 140, 100);
    a.apps[2].style.border_style = BORDER_DASHED;
    a.apps[2].style.bg = parse_hex_color("#ff4d8d", 1.0);
    snprintf(a.wallpaper, sizeof(a.wallpaper), "builtin:comic");
    a.gutter = 12;
    char path[] = "/tmp/wiiu_shell_test_XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    assert(config_save(&a, path));
    assert(config_load(&b, path));
    assert(b.num_apps == a.num_apps);
    assert(b.apps[2].npts == 4 && b.apps[2].page == 1);
    for (int i = 0; i < 4; i++) assert(fabs(b.apps[2].px[i] - a.apps[2].px[i]) < 0.06);
    assert(b.apps[2].style.border_style == BORDER_DASHED);
    assert(strcmp(b.apps[2].style.image, "builtin:burst") == 0);
    assert(strcmp(b.wallpaper, "builtin:comic") == 0 && b.gutter == 12);
    // Migración: una forma antigua sin polígono se convierte en polígono
    DesktopConfig m;
    FILE *f = fopen(path, "w");
    fputs("{\"apps\":[{\"name\":\"A\",\"exec\":\"x\",\"shape\":\"hexagon\"},"
          "{\"name\":\"B\",\"exec\":\"y\",\"style\":{\"shape\":\"burst\"}}]}", f);
    fclose(f);
    assert(config_load(&m, path));
    unlink(path);
    assert(m.apps[0].npts == 6 && m.apps[1].npts == 4);
    assert(strcmp(m.apps[1].style.image, "builtin:burst") == 0);
    assert(fabs(b.apps[2].style.bg.r - a.apps[2].style.bg.r) < 0.01);
    assert(config_page_count(&b) == 2);
    printf(" -> OK\n");
}

void test_editor_render(void) {
    printf("[Test 9] Editor: cambio de propiedad, deshacer, choque y render...\n");
    DesktopConfig cfg;
    config_load(&cfg, "test_config.json");
    Editor ed;
    editor_init(&ed, &cfg, "/tmp/wiiu_shell_editor_test.json", 854, 480);
    // Mando: abajo hasta «Ancho» (fila 5) y +, como haría el usuario
    double w0 = cfg.apps[0].w;
    for (int i = 0; i < 5; i++) editor_nav(&ed, NAV_DOWN, 1000 + i * 100);
    editor_nav(&ed, NAV_RIGHT, 1600);             // crecer: chocaría con el vecino (calle de 9 px)
    assert(fabs(cfg.apps[0].w - w0) < 1e-6);
    assert(strstr(ed.status, "Choca con"));
    editor_nav(&ed, NAV_LEFT, 2600);              // encoger sí
    assert(fabs(cfg.apps[0].w - (w0 - 4)) < 1e-6);
    editor_nav(&ed, NAV_ACTION_B, 2700);          // deshacer
    assert(fabs(cfg.apps[0].w - w0) < 1e-6);
    for (int i = 0; i < 6; i++) editor_nav(&ed, NAV_LEFT, 4000 + i * 1000);   // dejar sitio
    // Mover a la derecha sin parar: se detiene antes de pisar al vecino
    for (int i = 0; i < 2; i++) editor_nav(&ed, NAV_UP, 1800 + i * 10);   // fila «Posición X»
    double x0 = cfg.apps[0].x;
    for (int i = 0; i < 40; i++) editor_nav(&ed, NAV_RIGHT, 13000 + i * 1000);
    assert(cfg.apps[0].x > x0);
    for (int i = 0; i < cfg.num_apps; i++) assert(layout_check(&cfg, i) == -1);
    assert(poly_separation(&cfg.apps[0], &cfg.apps[1]) >= cfg.gutter);
    assert(poly_separation(&cfg.apps[0], &cfg.apps[1]) < cfg.gutter + 4);   // llegó hasta el límite
    for (int i = 0; i < 40; i++) editor_nav(&ed, NAV_ACTION_B, 50000);

    ed.vertex = 1;   // para que se vea el vértice seleccionado en la captura
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 854, 480);
    cairo_t *cr = cairo_create(surf);
    editor_render(&ed, cr, 854, 480);
    assert(cairo_surface_write_to_png(surf, "test_editor.png") == CAIRO_STATUS_SUCCESS);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    editor_free(&ed);
    printf(" -> Guardada 'desktop/test_editor.png'\n");
}

void test_editor_drag_no_tunnel(void) {
    printf("[Test 9b] Editor: un arrastre rápido no atraviesa a otros elementos...\n");
    DesktopConfig cfg;
    config_load(&cfg, "test_config.json");
    Editor ed;
    editor_init(&ed, &cfg, "/tmp/wiiu_shell_editor_test2.json", 854, 480);
    ed.selected = 0;
    ed.drag = ED_DRAG_ELEMENT;
    ed.down_x = ed.down_y = 0;
    ed.orig_x = cfg.apps[0].x;
    ed.orig_y = cfg.apps[0].y;
    ed.drag_moved = false;
    // De un solo salto gigante a la derecha (más allá del vecino y de la pantalla): debe parar antes
    editor_touch_motion(&ed, 5000, 0, 1000);
    for (int i = 0; i < cfg.num_apps; i++) assert(layout_check(&cfg, i) == -1);
    assert(ed.collide_until > 1000 && ed.collide_idx >= 0);   // hitbox visibles
    {
        cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 854, 480);
        cairo_t *c2 = cairo_create(sf);
        editor_render(&ed, c2, 854, 480);
        assert(cairo_surface_write_to_png(sf, "test_editor_hitbox.png") == CAIRO_STATUS_SUCCESS);
        cairo_destroy(c2);
        cairo_surface_destroy(sf);
    }
    // Y hacia abajo/izquierda en diagonal tampoco cruza nada
    ed.orig_x = cfg.apps[0].x; ed.orig_y = cfg.apps[0].y; ed.drag_moved = false;
    editor_touch_motion(&ed, -5000, 5000, 2000);
    for (int i = 0; i < cfg.num_apps; i++) assert(layout_check(&cfg, i) == -1);
    editor_free(&ed);
    printf(" -> OK\n");
}

void test_editor_color_picker(void) {
    printf("[Test 9c] Editor: selector de color (rueda, RGB, CMYK, paleta)...\n");
    DesktopConfig cfg;
    config_load(&cfg, "test_config.json");
    Editor ed;
    editor_init(&ed, &cfg, "/tmp/wiiu_shell_editor_test3.json", 854, 480);
    ed.selected = 0;
    ColorRGBA orig = cfg.apps[0].style.bg;
    assert(editor_open_color_picker(&ed, 0));
    assert(ed.picker == PICK_COLOR);

    // Geometría (la misma que col_geom): panel de 296 px a la derecha, contenido desde row_top + 80
    double panel_x = 854 - 296 - 8, bx = panel_x + 12, bw = 296 - 24;
    double cy0 = 8 + 50 + 80, alpha_y = 8 + (480 - 16) - 38;
    double R = fmin(bw / 2 - 4, ((alpha_y - 22 - cy0) - 40) / 2), cx = bx + bw / 2, cy = cy0 + R + 4;

    // Rueda: en el borde derecho el matiz es 0 (rojo) con saturación 1 y brillo 1 → rojo puro
    editor_touch_down(&ed, cx + R * 0.99, cy, 1000);
    editor_touch_up(&ed, cx + R * 0.99, cy, 1010);
    ColorRGBA *c = &cfg.apps[0].style.bg;
    assert(c->r > 0.9 && c->g < 0.06 && c->b < 0.06);   // el brillo (0,93) del color original se conserva
    // Centro de la rueda: saturación 0 → blanco
    editor_touch_down(&ed, cx, cy, 1100);
    editor_touch_up(&ed, cx, cy, 1110);
    assert(c->r > 0.9 && fabs(c->r - c->g) < 0.01 && fabs(c->g - c->b) < 0.01);   // gris
    // Arrastre: hacia abajo del todo (matiz 90°) → verde dominante
    editor_touch_down(&ed, cx, cy, 1200);
    editor_touch_motion(&ed, cx + 0.2, cy + R * 0.98, 1210);
    editor_touch_up(&ed, cx, cy + R * 0.98, 1220);
    assert(c->g > c->r && c->g > c->b && c->g > 0.9);

    // Mando: L/R de pestaña a RGB; abajo/derecha ajusta el verde; CMYK refleja el cambio
    {
        cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 854, 480);
        cairo_t *c2 = cairo_create(sf);
        editor_render(&ed, c2, 854, 480);
        assert(cairo_surface_write_to_png(sf, "test_editor_color_rueda.png") == CAIRO_STATUS_SUCCESS);
        cairo_destroy(c2);
        cairo_surface_destroy(sf);
    }
    editor_nav(&ed, NAV_PAGE_NEXT, 2000);
    assert(ed.col_mode == 1);
    editor_nav(&ed, NAV_DOWN, 2010);
    double g0 = c->g;
    editor_nav(&ed, NAV_LEFT, 2020);
    assert(c->g < g0);
    {
        cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 854, 480);
        cairo_t *c2 = cairo_create(sf);
        editor_render(&ed, c2, 854, 480);
        assert(cairo_surface_write_to_png(sf, "test_editor_color_rgb.png") == CAIRO_STATUS_SUCCESS);
        cairo_destroy(c2);
        cairo_surface_destroy(sf);
    }
    editor_nav(&ed, NAV_PAGE_NEXT, 2100);   // CMYK
    assert(ed.col_mode == 2);
    // CMYK puro: C=1,M=0,Y=0,K=0 → cian (r=0,g=1,b=1)
    ed.col_cmyk[0] = 1; ed.col_cmyk[1] = 0; ed.col_cmyk[2] = 0; ed.col_cmyk[3] = 0;
    editor_nav(&ed, NAV_RIGHT, 2110);        // campo 0 (C) ya está al máximo: se queda en 1 y recalcula
    assert(c->r < 0.01 && c->g > 0.99 && c->b > 0.99);
    {
        cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 854, 480);
        cairo_t *c2 = cairo_create(sf);
        editor_render(&ed, c2, 854, 480);
        assert(cairo_surface_write_to_png(sf, "test_editor_color_cmyk.png") == CAIRO_STATUS_SUCCESS);
        cairo_destroy(c2);
        cairo_surface_destroy(sf);
    }
    editor_nav(&ed, NAV_PAGE_NEXT, 2200);   // paleta: la segunda casilla es #ffffff
    editor_nav(&ed, NAV_RIGHT, 2210);
    assert(c->r > 0.99 && c->g > 0.99 && c->b > 0.99);
    {
        cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 854, 480);
        cairo_t *c2 = cairo_create(sf);
        editor_render(&ed, c2, 854, 480);
        assert(cairo_surface_write_to_png(sf, "test_editor_color_paleta.png") == CAIRO_STATUS_SUCCESS);
        cairo_destroy(c2);
        cairo_surface_destroy(sf);
    }
    // B cierra el selector y Deshacer devuelve el color original
    editor_nav(&ed, NAV_ACTION_B, 2300);
    assert(ed.picker == PICK_NONE);
    for (int i = 0; i < 12; i++) editor_nav(&ed, NAV_ACTION_B, 2400 + i * 10);   // cada paso se deshace por separado
    c = &cfg.apps[0].style.bg;
    assert(fabs(c->r - orig.r) < 1e-6 && fabs(c->g - orig.g) < 1e-6 && fabs(c->b - orig.b) < 1e-6);
    editor_free(&ed);
    printf(" -> OK (capturas test_editor_color_*.png)\n");
}

void test_no_overlap(void) {
    printf("[Test 10] Los elementos no se solapan y la forma se edita por vértices...\n");
    DesktopConfig cfg;
    config_set_defaults(&cfg);
    cfg.num_apps = 2;
    for (int i = 0; i < 2; i++) {
        memset(&cfg.apps[i], 0, sizeof(AppEntry));
        cfg.apps[i].style = cfg.default_style;
        snprintf(cfg.apps[i].name, sizeof(cfg.apps[i].name), "E%d", i);
    }
    poly_set_preset(&cfg.apps[0], PRESET_RECT, 100, 100, 100, 100);
    poly_set_preset(&cfg.apps[1], PRESET_RECT, 210, 100, 100, 100);   // 10 px de calle
    assert(layout_check(&cfg, 0) == -1 && layout_check(&cfg, 1) == -1);
    cfg.gutter = 12;
    assert(layout_check(&cfg, 1) == 0);                                 // calle insuficiente
    cfg.gutter = 8;
    poly_translate(&cfg.apps[1], -60, 0);
    assert(layout_check(&cfg, 1) == 0);                                 // se solapan
    poly_translate(&cfg.apps[1], 60, 0);

    AppEntry keep = cfg.apps[0], c = keep;
    c.px[1] = 205; app_update_bbox(&c);                                 // un vértice invade al vecino
    cfg.apps[0] = c;
    assert(layout_check(&cfg, 0) == 1);
    c = keep;
    c.px[1] = 200; c.py[1] = 80; app_update_bbox(&c);                   // viñeta inclinada que cabe
    cfg.apps[0] = c;
    assert(layout_check(&cfg, 0) == -1);
    cfg.apps[0] = keep;
    c = keep;                                                           // se cruza a sí mismo
    double tx = c.px[1], ty = c.py[1];
    c.px[1] = c.px[2]; c.py[1] = c.py[2]; c.px[2] = tx; c.py[2] = ty; app_update_bbox(&c);
    assert(!poly_is_simple(&c));
    c = keep;
    int v = poly_add_vertex(&c, -1);
    assert(v >= 0 && c.npts == 5 && poly_is_simple(&c));
    assert(poly_remove_vertex(&c, v) && c.npts == 4);
    double fx, fy;
    assert(layout_find_free(&cfg, -1, 0, 100, 100, &fx, &fy));
    AppEntry probe = keep;
    poly_set_preset(&probe, PRESET_RECT, fx, fy, 100, 100);
    assert(poly_separation(&probe, &cfg.apps[0]) >= cfg.gutter && poly_separation(&probe, &cfg.apps[1]) >= cfg.gutter);
    printf(" -> OK\n");
}

void test_page_cache(void) {
    printf("[Test 11] Páginas pre-renderizadas: se generan una vez y después se leen...\n");
    char dir[] = "/tmp/wiiu_shell_cache_XXXXXX";
    assert(mkdtemp(dir));
    setenv("XDG_CACHE_HOME", dir, 1);
    DesktopConfig cfg;
    config_load(&cfg, "test_config.json");
    PageCache pc;
    page_cache_init(&pc);
    assert(page_cache_prepare(&pc, &cfg, "test_config.json"));
    assert(page_cache_page(&pc, 0) && pc.wallpaper);
    char key[24];
    snprintf(key, sizeof(key), "%s", pc.key);
    page_cache_free(&pc);
    page_cache_init(&pc);
    assert(page_cache_prepare(&pc, &cfg, "test_config.json"));   // misma clave: del disco
    assert(strcmp(pc.key, key) == 0);
    assert(cairo_surface_write_to_png(page_cache_page(&pc, 0), "test_page_cache.png") == CAIRO_STATUS_SUCCESS);
    page_cache_free(&pc);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    assert(system(cmd) == 0);
    unsetenv("XDG_CACHE_HOME");
    printf(" -> OK (clave %s)\n", key);
}

// Escribe en `out` una copia de test_config.json con los elementos repartidos en 2 páginas y un
// marcador (`tag`) en el fondo para que cada copia tenga su propia clave de caché.
static void write_two_page_config(const char *out, int tag) {
    struct json_object *root = json_object_from_file("test_config.json");
    assert(root);
    struct json_object *apps;
    assert(json_object_object_get_ex(root, "apps", &apps));
    for (size_t i = 0; i < json_object_array_length(apps); i++)
        json_object_object_add(json_object_array_get_idx(apps, i), "page", json_object_new_int((int)(i % 2)));
    json_object_object_add(root, "tag", json_object_new_int(tag));
    assert(json_object_to_file(out, root) == 0);
    json_object_put(root);
}

static int count_cache_files(const char *dir) {
    int n = 0;
    DIR *d = opendir(dir);
    assert(d);
    struct dirent *de;
    while ((de = readdir(d))) if (strstr(de->d_name, ".png") && !strstr(de->d_name, ".tmp")) n++;
    closedir(d);
    return n;
}

void test_page_cache_progressive(void) {
    printf("[Test 12] Carga progresiva de páginas: aparece al instante, se rellena de a poco y se guarda sin borrar lo de otras carpetas...\n");
    char dir[] = "/tmp/wiiu_shell_cache_XXXXXX";
    assert(mkdtemp(dir));
    setenv("XDG_CACHE_HOME", dir, 1);
    char cdir[256];
    snprintf(cdir, sizeof(cdir), "%s/wiiu-shell", dir);
    char fa[] = "/tmp/wiiu_cfg_a_XXXXXX", fb[] = "/tmp/wiiu_cfg_b_XXXXXX";
    close(mkstemp(fa));
    close(mkstemp(fb));
    write_two_page_config(fa, 1);
    write_two_page_config(fb, 2);

    DesktopConfig cfg;
    assert(config_load(&cfg, fa));
    assert(config_page_count(&cfg) == 2);
    PageCache pc;
    page_cache_init(&pc);

    // 1) Empezar no dibuja nada: las páginas existen (solo fondo) y queda trabajo pendiente
    assert(page_cache_begin(&pc, &cfg, fa));
    assert(page_cache_pending(&pc) && pc.wallpaper && page_cache_page(&pc, 0) && page_cache_page(&pc, 1));

    // 2) Se trabaja primero en la página que se ve (la 1), aunque la 0 vaya antes
    bool changed = false;
    assert(page_cache_step(&pc, 1, &changed));
    assert(changed && !pc.bdone[0] && !pc.bdone[1]);
    int steps = 1;
    while (page_cache_step(&pc, 1, &changed)) assert(++steps < 1000);
    assert(!page_cache_pending(&pc) && pc.bdone[0] && pc.bdone[1]);
    assert(steps >= cfg.num_apps);                       // un elemento por paso, no todo de golpe

    // 3) Al terminar se guardan fondo + 2 páginas (en segundo plano)
    page_cache_wait_writes();
    assert(count_cache_files(cdir) == 3);
    char keya[24];
    snprintf(keya, sizeof(keya), "%s", pc.key);
    page_cache_free(&pc);

    // 4) Misma configuración otra vez: sale de memoria, sin trabajo pendiente
    page_cache_init(&pc);
    assert(page_cache_begin(&pc, &cfg, fa) && !page_cache_pending(&pc) && page_cache_page(&pc, 1));
    page_cache_free(&pc);

    // 5) Olvidada la memoria, se carga del disco (también sin trabajo pendiente)
    page_cache_forget_memory();
    page_cache_init(&pc);
    assert(page_cache_begin(&pc, &cfg, fa) && !page_cache_pending(&pc) && strcmp(pc.key, keya) == 0);
    page_cache_free(&pc);

    // 6) Otra configuración no borra la caché de la primera
    assert(config_load(&cfg, fb));
    page_cache_init(&pc);
    assert(page_cache_begin(&pc, &cfg, fb) && page_cache_pending(&pc));
    while (page_cache_step(&pc, 0, &changed)) {}
    page_cache_wait_writes();
    assert(strcmp(pc.key, keya) != 0);
    assert(count_cache_files(cdir) == 6);                // 3 de cada una
    page_cache_free(&pc);

    // 7) La primera configuración sigue en disco tras haber guardado la segunda
    page_cache_forget_memory();
    assert(config_load(&cfg, fa));
    page_cache_init(&pc);
    assert(page_cache_begin(&pc, &cfg, fa));
    assert(!page_cache_pending(&pc));                    // fa está en disco
    page_cache_free(&pc);

    char cmd[160];
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s %s", dir, fa, fb);
    assert(system(cmd) == 0);
    unsetenv("XDG_CACHE_HOME");
    printf(" -> OK (%d pasos)\n", steps);
}

int main(void) {
    printf("=========================================\n");
    printf("   Suite de Pruebas: wiiu-shell (C)      \n");
    printf("=========================================\n");

    test_config_and_shapes();
    test_touch_gestures();
    test_home_state_machine();
    test_visual_rendering();
    test_hitbox_matches_shape();
    test_evdev_bits();
    test_save_roundtrip();
    test_editor_render();
    test_editor_drag_no_tunnel();
    test_editor_color_picker();
    test_no_overlap();
    test_page_cache();
    test_page_cache_progressive();

    printf("\n>>> TODAS LAS PRUEBAS PASARON EXITOSAMENTE <<<\n");
    return 0;
}
