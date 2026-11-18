#ifndef PAGE_CACHE_H
#define PAGE_CACHE_H

// Páginas pre-renderizadas: al guardar (editor) o al cargar una configuración nueva (shell)
// se dibuja cada página una vez (fondo + elementos) y se guarda como PNG en
// ~/.cache/wiiu-shell. El shell pinta esa imagen y encima solo el foco: no recalcula
// formas, tramas, iconos ni texto en cada fotograma.
//
// La clave de la caché sale del contenido de config.json, las fechas de las imágenes
// usadas, los iconos resueltos y la versión del binario: si algo cambia, se regenera.

#include "config.h"
#include <cairo.h>
#include <stdbool.h>

#define PAGE_CACHE_MAX 16

typedef struct {
    char key[24];
    int npages;
    cairo_surface_t *pages[PAGE_CACHE_MAX];
    cairo_surface_t *wallpaper;   // solo el fondo (Recientes lo usa)
    // Carga incremental (page_cache_begin / page_cache_step): las páginas existen desde el principio
    // (solo fondo) y se van rellenando de a un elemento por paso.
    bool building;
    const DesktopConfig *bcfg;
    int bcursor[PAGE_CACHE_MAX];  // siguiente índice de bcfg->apps por mirar en cada página
    bool bdone[PAGE_CACHE_MAX];   // página terminada (cargada del disco o dibujada entera)
    bool bsave_wall;              // el fondo se generó: guardarlo al terminar
    char bdir[MAX_STR];
} PageCache;

void page_cache_init(PageCache *pc);
void page_cache_free(PageCache *pc);
// Carga las páginas de la caché o las genera (y las guarda). `config_file` es el JSON del que salió `cfg`.
bool page_cache_prepare(PageCache *pc, const DesktopConfig *cfg, const char *config_file);
cairo_surface_t *page_cache_page(const PageCache *pc, int page);

// Carga incremental: devuelve enseguida con el fondo y las páginas vacías (o ya completas si estaban en
// memoria o en disco). Mientras `page_cache_pending`, llamar a `page_cache_step` repetidamente (p. ej. desde un
// temporizador): cada llamada dibuja UN elemento, primero los de `visible_page`; `*visible_changed` indica que
// la imagen de esa página cambió. Al terminar guarda los PNG en segundo plano sin borrar los de otras
// configuraciones (solo los que llevan más de 7 días sin usarse).
bool page_cache_begin(PageCache *pc, const DesktopConfig *cfg, const char *config_file);
bool page_cache_pending(const PageCache *pc);
bool page_cache_step(PageCache *pc, int visible_page, bool *visible_changed);
void page_cache_wait_writes(void);     // espera a las escrituras en segundo plano (pruebas, salida)
void page_cache_forget_memory(void);   // vacía las páginas recordadas en memoria (pruebas)
// Solo genera los ficheros (el editor lo hace antes de publicar la configuración)
bool page_cache_build_files(const DesktopConfig *cfg, const char *config_file);

void page_cache_dir(char *out, int out_len);
// Dibuja una página completa en vivo (lo mismo que se guarda en la caché)
void page_render(cairo_t *cr, const DesktopConfig *cfg, int page, bool with_wallpaper);
// Igual, pero con cada elemento cacheado como imagen a la escala `scale` (vista previa del editor):
// solo se vuelve a dibujar el elemento que cambia.
void page_render_fast(cairo_t *cr, const DesktopConfig *cfg, int page, double scale);

#endif // PAGE_CACHE_H
