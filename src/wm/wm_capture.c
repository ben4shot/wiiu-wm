#include "wm_capture.h"
#include "wm.h"
#include "yuv_convert.h"
#include "yuv_gpu.h"

#include <drm_fourcc.h>
#include <pixman.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#define CAP_W 854
#define CAP_H 480
#define Y_BYTES (CAP_W * CAP_H)
#define UV_BYTES ((CAP_W / 2) * (CAP_H / 2))
#define I420_BYTES (Y_BYTES + 2 * UV_BYTES)
#define Y4M_HEADER "YUV4MPEG2 W854 H480 F60:1 Ip A1:1 C420jpeg\n"
#define Y4M_FRAME "FRAME\n"

struct wm_capture {
    struct wm_server *s;
    char path[256];
    int listen_fd, client_fd;
    struct wl_event_source *listen_src, *tick;
    uint8_t *xrgb;              // lectura del búfer de salida
    uint8_t *latest;            // último fotograma I420 completo
    bool have_frame;
    bool dirty;                 // hay un fotograma nuevo sin enviar
    bool need_full;             // la próxima lectura debe ser de pantalla completa
    yuv_gpu *gpu;               // conversión YUV en la GPU (NULL = CPU)
    bool gpu_tried;
    long n_gpu_fail;
    bool resend_always;         // WIIU_CAPTURE_RESEND=1: reenviar a 60 Hz aunque no cambie (comportamiento anterior)
    long n_empty, damage_px;
    uint8_t *pend;              // lo que se está enviando: cabecera y/o «FRAME\n» + I420
    size_t pend_len, pend_off;
    long tick_n;
    // estadísticas (cada 5 s en el registro): fotogramas leídos, enviados y tiempo medio de lectura/conversión
    long n_cap, n_sent;
    double t_read_ms, t_conv_ms, t_import_ms;
    struct timespec t0;
};

static struct wm_capture g_cap;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void drop_client(struct wm_capture *c) {
    if (c->client_fd >= 0) close(c->client_fd);
    c->client_fd = -1;
    c->pend_len = c->pend_off = 0;
    c->have_frame = false;
}

// Envía lo pendiente sin bloquear. true = terminado
static bool flush_pending(struct wm_capture *c) {
    while (c->pend_off < c->pend_len) {
        ssize_t n = send(c->client_fd, c->pend + c->pend_off, c->pend_len - c->pend_off, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) { c->pend_off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return false;
        drop_client(c);   // EPIPE/ECONNRESET: drc_stream se fue
        return false;
    }
    return true;
}

// 60 Hz sin deriva: el siguiente disparo se calcula desde el primero
static int cap_tick(void *data) {
    struct wm_capture *c = data;
    c->tick_n++;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_us = (now.tv_sec - c->t0.tv_sec) * 1000000L + (now.tv_nsec - c->t0.tv_nsec) / 1000;
    long next_us = c->tick_n * 1000000L / 60;
    long wait_ms = (next_us - elapsed_us + 999) / 1000;
    if (wait_ms < 1) wait_ms = 1;
    wl_event_source_timer_update(c->tick, (int)wait_ms);

    if (c->tick_n % 300 == 0 && c->client_fd >= 0) {
        extern long wm_stat_ui_redraws, wm_stat_frames, wm_stat_buffers;
        wlr_log(WLR_INFO, "[wm] captura: %ld leídos, %ld enviados en 5 s (eventos frame %ld, con búfer %ld, redibujos UI %ld; sin cambios %ld, área media leída %ld px); importar %.2f ms, leer %.2f ms, convertir %.2f ms por fotograma",
                c->n_cap, c->n_sent, wm_stat_frames, wm_stat_buffers, wm_stat_ui_redraws, c->n_empty, c->n_cap ? c->damage_px / c->n_cap : 0L, c->n_cap ? c->t_import_ms / c->n_cap : 0.0, c->n_cap ? c->t_read_ms / c->n_cap : 0.0, c->n_cap ? c->t_conv_ms / c->n_cap : 0.0);
        c->n_cap = c->n_sent = c->n_empty = c->damage_px = 0;
        wm_stat_ui_redraws = wm_stat_frames = wm_stat_buffers = 0;
        c->t_read_ms = c->t_conv_ms = c->t_import_ms = 0;
    }
    if (c->client_fd < 0) return 0;
    if (c->pend_off < c->pend_len && !flush_pending(c)) return 0;   // el cliente aún no terminó el anterior: se omite
    if (c->client_fd < 0 || !c->have_frame) return 0;
    if (!c->dirty && !c->resend_always) return 0;    // nada nuevo: libdrc rellena el tick sin codificar
    c->dirty = false;
    memcpy(c->pend, Y4M_FRAME, sizeof(Y4M_FRAME) - 1);
    memcpy(c->pend + sizeof(Y4M_FRAME) - 1, c->latest, I420_BYTES);
    c->pend_len = sizeof(Y4M_FRAME) - 1 + I420_BYTES;
    c->pend_off = 0;
    c->n_sent++;
    flush_pending(c);
    return 0;
}

void wm_capture_redraw(struct wm_server *s) {
    wm_ui_redraw(s);   // la interfaz también (puede no haberse dibujado aún)
    struct wm_output *o;
    wl_list_for_each(o, &s->outputs, link) {
        struct wlr_scene_output *so = wlr_scene_get_scene_output(s->scene, o->wlr_output);
        if (so) wlr_damage_ring_add_whole(&so->damage_ring);
        wlr_output_schedule_frame(o->wlr_output);
    }
}

void wm_capture_dump(struct wm_server *s, const char *path) {
    (void)s;
    struct wm_capture *c = &g_cap;
    if (!path || !path[0]) return;
    if (!c->have_frame) { wlr_log(WLR_ERROR, "[wm] capture_dump: aún no hay fotograma"); return; }
    FILE *f = fopen(path, "wb");
    if (!f) { wlr_log(WLR_ERROR, "[wm] capture_dump: no se pudo abrir %s", path); return; }
    fwrite(Y4M_HEADER, 1, sizeof(Y4M_HEADER) - 1, f);
    fwrite(Y4M_FRAME, 1, sizeof(Y4M_FRAME) - 1, f);
    fwrite(c->latest, 1, I420_BYTES, f);
    fclose(f);
    wlr_log(WLR_INFO, "[wm] capture_dump: %s", path);
}

static int cap_accept(int fd, uint32_t mask, void *data) {
    (void)mask;
    struct wm_capture *c = data;
    int cl = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (cl < 0) return 0;
    if (c->client_fd >= 0) {
        // Un solo consumidor: una conexión extra (una sonda, otro programa) no debe desplazar a drc_stream.
        // Si el cliente actual murió, el próximo envío lo detecta (EPIPE) y la siguiente conexión entra.
        wlr_log(WLR_INFO, "[wm] Conexión extra a la captura rechazada (ya hay un consumidor)");
        close(cl);
        return 0;
    }
    int sz = 1 << 20;
    setsockopt(cl, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
    c->client_fd = cl;
    c->need_full = true;
    c->have_frame = false;
    c->dirty = false;
    // Cabecera y4m y, hasta que haya un fotograma nuevo, nada más
    memcpy(c->pend, Y4M_HEADER, sizeof(Y4M_HEADER) - 1);
    c->pend_len = sizeof(Y4M_HEADER) - 1;
    c->pend_off = 0;
    flush_pending(c);
    // Pantalla entera de nuevo: el fotograma que se mande debe reflejar el estado actual
    wm_capture_redraw(c->s);
    wlr_log(WLR_INFO, "[wm] drc_stream conectado a la captura directa");
    return 0;
}

void wm_capture_init(struct wm_server *s) {
    struct wm_capture *c = &g_cap;
    memset(c, 0, sizeof(*c));
    c->s = s;
    c->resend_always = getenv("WIIU_CAPTURE_RESEND") != NULL;
    c->listen_fd = c->client_fd = -1;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    snprintf(c->path, sizeof(c->path), "%.200s/wiiu-wm-frames", rt ? rt : "/tmp");
    c->xrgb = malloc((size_t)CAP_W * CAP_H * 4);
    c->latest = malloc(I420_BYTES);
    c->pend = malloc(sizeof(Y4M_FRAME) + I420_BYTES + 64);
    if (!c->xrgb || !c->latest || !c->pend) return;
    unlink(c->path);
    c->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", c->path);
    if (c->listen_fd < 0 || bind(c->listen_fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(c->listen_fd, 2) != 0) {
        wlr_log(WLR_ERROR, "[wm] captura directa desactivada (%s): %s", c->path, strerror(errno));
        if (c->listen_fd >= 0) close(c->listen_fd);
        c->listen_fd = -1;
        return;
    }
    chmod(c->path, 0666);
    c->listen_src = wl_event_loop_add_fd(s->loop, c->listen_fd, WL_EVENT_READABLE, cap_accept, c);
    clock_gettime(CLOCK_MONOTONIC, &c->t0);
    c->tick = wl_event_loop_add_timer(s->loop, cap_tick, c);
    wl_event_source_timer_update(c->tick, 16);
    wlr_log(WLR_INFO, "[wm] Captura directa para drc_stream en %s", c->path);
}

void wm_capture_finish(struct wm_server *s) {
    (void)s;
    struct wm_capture *c = &g_cap;
    if (c->tick) wl_event_source_remove(c->tick);
    if (c->listen_src) wl_event_source_remove(c->listen_src);
    drop_client(c);
    if (c->listen_fd >= 0) { close(c->listen_fd); unlink(c->path); }
    free(c->xrgb); free(c->latest); free(c->pend);
    memset(c, 0, sizeof(*c));
}

bool wm_capture_wanted(struct wm_server *s) {
    (void)s;
    return g_cap.client_fd >= 0;
}

void wm_capture_buffer(struct wm_server *s, struct wlr_buffer *buf, const pixman_region32_t *damage) {
    struct wm_capture *c = &g_cap;
    if (c->client_fd < 0 || !buf || buf->width != CAP_W || buf->height != CAP_H) return;

    // Zona a leer: la dañada (alineada a pares por el submuestreo de croma) o, si no se sabe o es la
    // primera vez, todo
    int x0 = 0, y0 = 0, x1 = CAP_W, y1 = CAP_H;
    if (damage && !c->need_full) {
        if (!pixman_region32_not_empty(damage)) { c->n_empty++; return; }       // sin cambios
        pixman_box32_t *e = pixman_region32_extents((pixman_region32_t *)damage);
        x0 = e->x1 & ~1; y0 = e->y1 & ~1;
        x1 = (e->x2 + 1) & ~1; y1 = (e->y2 + 1) & ~1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > CAP_W) x1 = CAP_W;
        if (y1 > CAP_H) y1 = CAP_H;
        if (x1 <= x0 || y1 <= y0) { c->n_empty++; return; }
    }
    c->damage_px += (long)(x1 - x0) * (y1 - y0);

    double t0 = now_ms();
    struct wlr_texture *tex = wlr_texture_from_buffer(s->renderer, buf);
    if (!tex) { c->need_full = true; return; }
    double t_imp = now_ms();
    if (!c->gpu_tried) {
        c->gpu_tried = true;
        if (!getenv("WIIU_CAPTURE_CPU") && wlr_renderer_is_gles2(s->renderer) && wlr_texture_is_gles2(tex)) {
            struct wlr_egl *egl = wlr_gles2_renderer_get_egl(s->renderer);
            EGLDisplay d = wlr_egl_get_display(egl);
            EGLContext sc = eglGetCurrentContext();
            EGLDisplay sd = eglGetCurrentDisplay();
            EGLSurface sdraw = eglGetCurrentSurface(EGL_DRAW), sread = eglGetCurrentSurface(EGL_READ);
            if (eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, wlr_egl_get_context(egl))) {
                c->gpu = yuv_gpu_create();
                eglMakeCurrent(sd, sdraw, sread, sc);
            }
            wlr_log(WLR_INFO, "[wm] conversión YUV en %s", c->gpu ? "la GPU" : "la CPU (no se pudo crear el pase de GPU)");
        }
    }
    if (c->gpu && wlr_texture_is_gles2(tex)) {
        struct wlr_gles2_texture_attribs at;
        wlr_gles2_texture_get_attribs(tex, &at);
        struct wlr_egl *egl = wlr_gles2_renderer_get_egl(s->renderer);
        EGLDisplay d = wlr_egl_get_display(egl);
        EGLContext sc = eglGetCurrentContext();
        EGLDisplay sd = eglGetCurrentDisplay();
        EGLSurface sdraw = eglGetCurrentSurface(EGL_DRAW), sread = eglGetCurrentSurface(EGL_READ);
        bool ok2 = false;
        if (eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, wlr_egl_get_context(egl))) {
            ok2 = yuv_gpu_convert(c->gpu, at.tex, at.target, y0, y1, c->latest, c->latest + Y_BYTES,
                                  c->latest + Y_BYTES + UV_BYTES);
            eglMakeCurrent(sd, sdraw, sread, sc);
        }
        wlr_texture_destroy(tex);
        if (ok2) {
            double t2 = now_ms();
            if (x1 - x0 == CAP_W && y1 - y0 == CAP_H) c->need_full = false;
            c->have_frame = true;
            c->dirty = true;
            c->n_cap++;
            c->t_import_ms += t_imp - t0;
            c->t_read_ms += t2 - t_imp;   // lectura + conversión juntas (todo ocurre en la GPU)
            return;
        }
        c->n_gpu_fail++;
        c->need_full = true;
        return;   // el siguiente fotograma lo reintenta (si falla siempre, WIIU_CAPTURE_CPU=1)
    }
    struct wlr_texture_read_pixels_options opt = {
        .data = c->xrgb, .format = DRM_FORMAT_XRGB8888, .stride = CAP_W * 4,
        .dst_x = (uint32_t)x0, .dst_y = (uint32_t)y0,
        .src_box = { .x = x0, .y = y0, .width = x1 - x0, .height = y1 - y0 },
    };
    bool ok = wlr_texture_read_pixels(tex, &opt);
    wlr_texture_destroy(tex);
    if (!ok) { c->need_full = true; return; }
    double t1 = now_ms();
    xrgb_to_i420_rect(c->xrgb, CAP_W * 4, CAP_W, CAP_H, x0, y0, x1, y1,
                      c->latest, c->latest + Y_BYTES, c->latest + Y_BYTES + UV_BYTES);
    if (x1 - x0 == CAP_W && y1 - y0 == CAP_H) c->need_full = false;
    c->have_frame = true;
    c->dirty = true;
    c->n_cap++;
    c->t_import_ms += t_imp - t0;
    c->t_read_ms += t1 - t_imp;
    c->t_conv_ms += now_ms() - t1;
}
