// wiiu-wm: compositor Wayland propio para la pantalla del Wii U GamePad.
//
// Sustituye a labwc en la sesión de drc: salida headless de 854×480 que captura
// wf-recorder (wlr-screencopy), pantalla táctil y teclados por libinput, y el teclado
// en pantalla (wvkbd: layer-shell + virtual-keyboard). El Inicio y Recientes están
// integrados; las ventanas se organizan en escritorios, a pantalla completa o en mosaico.

#include <pixman.h>
#include "wm.h"
#include "wm_capture.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>

static int opt_width = 854, opt_height = 480;

long wm_stat_frames, wm_stat_buffers;   // diagnóstico (ver wm_capture)

static void output_frame(struct wl_listener *l, void *data) {
    wm_stat_frames++;
    (void)data;
    struct wm_output *o = wl_container_of(l, o, frame);
    struct wlr_scene_output *so = wlr_scene_get_scene_output(o->server->scene, o->wlr_output);
    // Se dibuja la escena y, antes del commit, el búfer resultante se entrega a drc_stream
    // (captura directa: sin wf-recorder ni segunda conversión de pantalla).
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    if (wlr_scene_output_build_state(so, &st, NULL)) {
        if (st.committed & WLR_OUTPUT_STATE_BUFFER) wm_stat_buffers++;
        if ((st.committed & WLR_OUTPUT_STATE_BUFFER) && st.buffer && wm_capture_wanted(o->server))
            wm_capture_buffer(o->server, st.buffer, (st.committed & WLR_OUTPUT_STATE_DAMAGE) ? &st.damage : NULL);
        // Sin daño, sin ventanas y con drc_stream consumiendo: no hay nada que presentar. No hacer commit corta
        // el bucle (cada commit con búfer provoca el siguiente evento «frame») hasta que haya daño de verdad.
        // Con ventanas se sigue: sus frame callbacks (vsync) dependen de los eventos de fotograma.
        bool idle = (st.committed & WLR_OUTPUT_STATE_DAMAGE) && !pixman_region32_not_empty(&st.damage) &&
                    wm_capture_wanted(o->server) && wm_view_count(o->server, -1) == 0;
        if (!idle) wlr_output_commit_state(o->wlr_output, &st);
    }
    wlr_output_state_finish(&st);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(so, &now);
    // Salida headless: si un fotograma no produce daño no llega otro evento de frame y los clientes
    // que esperan su frame callback (RetroArch con vsync) se quedan congelados hasta que algo
    // redibuja (p. ej. un toque). Mientras haya ventanas se mantiene el ritmo de fotogramas.
    if (getenv("WIIU_KICK") && wm_view_count(o->server, -1) > 0) wlr_output_schedule_frame(o->wlr_output);
}

// Los clientes (Mesa/EGL) esperan el wl_buffer.release o el frame_done, que solo llegan cuando la
// salida renderiza. En una salida headless la cadena de eventos «frame» puede cortarse (p. ej. tras un
// commit sin daño) y RetroArch/SDL se quedan parados hasta que algo redibuja. Este temporizador
// independiente pide un fotograma cada 16 ms mientras haya ventanas.
static int frame_kick(void *data) {
    struct wm_server *s = data;
    if (wm_view_count(s, -1) > 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        struct wm_output *o;
        wl_list_for_each(o, &s->outputs, link) {
            // Un commit de cliente sin daño no provoca evento «frame» de la salida y su frame callback
            // nunca se entregaría (RetroArch con vsync se quedaba parado hasta un toque): se entregan
            // aquí a 60 Hz, sin renderizar nada.
            struct wlr_scene_output *so = wlr_scene_get_scene_output(s->scene, o->wlr_output);
            if (so) wlr_scene_output_send_frame_done(so, &now);
            if (getenv("WIIU_KICK")) wlr_output_schedule_frame(o->wlr_output);
        }
    }
    wl_event_source_timer_update(s->frame_kick, 16);
    return 0;
}

static void output_request_state(struct wl_listener *l, void *data) {
    struct wm_output *o = wl_container_of(l, o, request_state);
    const struct wlr_output_event_request_state *ev = data;
    wlr_output_commit_state(o->wlr_output, ev->state);
}

static void output_destroy(struct wl_listener *l, void *data) {
    (void)data;
    struct wm_output *o = wl_container_of(l, o, destroy);
    wl_list_remove(&o->frame.link);
    wl_list_remove(&o->request_state.link);
    wl_list_remove(&o->destroy.link);
    wl_list_remove(&o->link);
    free(o);
}

static void new_output(struct wl_listener *l, void *data) {
    struct wm_server *s = wl_container_of(l, s, new_output);
    struct wlr_output *wo = data;
    if (!wl_list_empty(&s->outputs)) {
        // Una sola pantalla: la del GamePad
        wlr_log(WLR_INFO, "[wm] Salida adicional '%s' ignorada", wo->name);
        return;
    }
    wlr_output_init_render(wo, s->allocator, s->renderer);
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, true);
    if (wlr_output_is_headless(wo)) {
        wlr_output_state_set_custom_mode(&st, opt_width, opt_height, 60000);
    } else {
        struct wlr_output_mode *m = wlr_output_preferred_mode(wo);
        if (m) wlr_output_state_set_mode(&st, m);
    }
    wlr_output_commit_state(wo, &st);
    wlr_output_state_finish(&st);

    struct wm_output *o = calloc(1, sizeof(*o));
    o->server = s;
    o->wlr_output = wo;
    o->frame.notify = output_frame;
    wl_signal_add(&wo->events.frame, &o->frame);
    o->request_state.notify = output_request_state;
    wl_signal_add(&wo->events.request_state, &o->request_state);
    o->destroy.notify = output_destroy;
    wl_signal_add(&wo->events.destroy, &o->destroy);
    wl_list_insert(&s->outputs, &o->link);

    struct wlr_output_layout_output *lo = wlr_output_layout_add_auto(s->output_layout, wo);
    struct wlr_scene_output *so = wlr_scene_output_create(s->scene, wo);
    wlr_scene_output_layout_add_output(s->scene_layout, lo, so);
    s->width = wo->width;
    s->height = wo->height;
    wlr_log(WLR_INFO, "[wm] Pantalla '%s' %dx%d", wo->name, s->width, s->height);
    wm_ui_redraw(s);   // ya hay tamaño: dibujar el Inicio (sin esto el arranque podía quedarse solo con el fondo)
}

int main(int argc, char *argv[]) {
    // Con una ventana a pantalla completa wlroots entrega directamente el búfer del cliente a la salida
    // (direct scanout): la captura directa leería solo esa ventana y no la interfaz (Recientes, OSD...).
    setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", 1);
    const char *startup = NULL;
    const char *config = "/etc/wiiu-shell/config.json";
    bool debug = false;
    static const struct option longopts[] = {
        { "config", required_argument, 0, 'c' },
        { "startup", required_argument, 0, 's' },
        { "size", required_argument, 0, 'g' },
        { "debug", no_argument, 0, 'd' },
        { "no-gamepad", no_argument, 0, 'n' },
        { "help", no_argument, 0, 'h' },
        { 0, 0, 0, 0 },
    };
    int c;
    bool no_gamepad = getenv("WIIU_WM_NO_GAMEPAD") != NULL;
    while ((c = getopt_long(argc, argv, "c:s:g:dnh", longopts, NULL)) != -1) {
        switch (c) {
            case 'c': config = optarg; break;
            case 's': startup = optarg; break;
            case 'g': sscanf(optarg, "%dx%d", &opt_width, &opt_height); break;
            case 'd': debug = true; break;
            case 'n': no_gamepad = true; break;
            default:
                printf("Uso: %s [-s orden_de_arranque] [--config config.json] [--size 854x480] [--no-gamepad] [--debug]\n", argv[0]);
                printf("Control: echo <acción> > $XDG_RUNTIME_DIR/wiiu-wm-<socket>.ctl (WIIU_WM_CTL en las apps)\n");
                printf("Atajos, escritorios y mosaico se configuran en config.json (\"binds\", \"windows\").\n");
                return 0;
        }
    }
    wlr_log_init(debug ? WLR_DEBUG : WLR_INFO, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);

    static struct wm_server s;
    snprintf(s.config_fallback, sizeof(s.config_fallback), "%s", config);
    s.width = opt_width;
    s.height = opt_height;
    s.inotify_fd = -1;
    s.ctl_fd = -1;
    s.no_gamepad = no_gamepad;

    s.display = wl_display_create();
    s.loop = wl_display_get_event_loop(s.display);
    s.backend = wlr_backend_autocreate(s.loop, NULL);
    if (!s.backend) { wlr_log(WLR_ERROR, "[wm] No se pudo crear el backend"); return 1; }
    s.renderer = wlr_renderer_autocreate(s.backend);
    if (!s.renderer) { wlr_log(WLR_ERROR, "[wm] No se pudo crear el renderizador"); return 1; }
    wlr_renderer_init_wl_display(s.renderer, s.display);
    s.allocator = wlr_allocator_autocreate(s.backend, s.renderer);
    if (!s.allocator) { wlr_log(WLR_ERROR, "[wm] No se pudo crear el asignador"); return 1; }

    s.compositor = wlr_compositor_create(s.display, 6, s.renderer);
    wlr_subcompositor_create(s.display);
    wlr_data_device_manager_create(s.display);
    wlr_primary_selection_v1_device_manager_create(s.display);
    wlr_viewporter_create(s.display);
    wlr_single_pixel_buffer_manager_v1_create(s.display);

    s.output_layout = wlr_output_layout_create(s.display);
    wlr_xdg_output_manager_v1_create(s.display, s.output_layout);
    wlr_screencopy_manager_v1_create(s.display);   // wf-recorder captura de aquí
    wl_list_init(&s.outputs);
    s.new_output.notify = new_output;
    wl_signal_add(&s.backend->events.new_output, &s.new_output);

    // Escenario: fondo de pantalla, layer-shell inferior, escritorios, interfaz, layer-shell superior
    s.scene = wlr_scene_create();
    s.scene_layout = wlr_scene_attach_output_layout(s.scene, s.output_layout);
    s.layer_tree[0] = wlr_scene_tree_create(&s.scene->tree);
    s.layer_tree[1] = wlr_scene_tree_create(&s.scene->tree);
    s.windows = wlr_scene_tree_create(&s.scene->tree);
    for (int i = 0; i < WM_MAX_WS; i++) {
        s.ws_tree[i] = wlr_scene_tree_create(s.windows);
        wlr_scene_node_set_enabled(&s.ws_tree[i]->node, i == 0);
    }
    s.ui_tree = wlr_scene_tree_create(&s.scene->tree);
    s.top_tree = wlr_scene_tree_create(&s.scene->tree);

    wm_views_init(&s);
    wm_ui_init(&s);       // carga config.json (atajos incluidos) antes de la entrada
    wm_input_init(&s);
    if (s.xwayland && s.seat) {
        wlr_xwayland_set_seat(s.xwayland, s.seat);
    }

    const char *socket = wl_display_add_socket_auto(s.display);
    if (!socket) { wlr_backend_destroy(s.backend); return 1; }
    if (!wlr_backend_start(s.backend)) {
        wlr_backend_destroy(s.backend);
        wl_display_destroy(s.display);
        return 1;
    }
    // Si no hay ninguna salida (p. ej. sin WLR_BACKENDS=headless), crear la de 854×480
    if (wl_list_empty(&s.outputs) && wlr_backend_is_headless(s.backend)) {
        wlr_headless_add_output(s.backend, opt_width, opt_height);
    }
    s.cfg.screen_width = s.width;
    s.cfg.screen_height = s.height;
    wm_ui_update_wallpaper(&s);
    wm_ui_redraw(&s);

    setenv("WAYLAND_DISPLAY", socket, true);
    // Bus de sesión de D-Bus para lo que se lance (waybar no arranca sin él). drc arranca
    // desde systemd, fuera de una sesión: se usa el del usuario (linger) si existe.
    if (!getenv("DBUS_SESSION_BUS_ADDRESS")) {
        char bus[128];
        snprintf(bus, sizeof(bus), "/run/user/%d/bus", (int)getuid());
        if (access(bus, F_OK) == 0) {
            char addr[160];
            snprintf(addr, sizeof(addr), "unix:path=%s", bus);
            setenv("DBUS_SESSION_BUS_ADDRESS", addr, true);
        }
    }
    wm_ctl_init(&s, socket);
    wm_capture_init(&s);
    s.frame_kick = wl_event_loop_add_timer(s.loop, frame_kick, &s);
    wl_event_source_timer_update(s.frame_kick, 16);
    wm_write_state(&s);
    wm_bar_sync(&s);
    signal(SIGPIPE, SIG_IGN);
    if (startup) {
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            execl("/bin/sh", "/bin/sh", "-c", startup, (char *)NULL);
            _exit(127);
        }
    }
    wlr_log(WLR_INFO, "[wm] wiiu-wm en WAYLAND_DISPLAY=%s", socket);
    wl_display_run(s.display);

    wl_display_destroy_clients(s.display);
    if (s.bar_pid > 0) kill(s.bar_pid, SIGTERM);
    wm_ui_finish(&s);
    wm_capture_finish(&s);
    wm_ctl_finish(&s);
    wm_set_gamepad_grab(&s, false);
    // wlroots comprueba al destruir que no quedan oyentes: quitarlos todos antes
    wl_list_remove(&s.new_xdg_toplevel.link);
    wl_list_remove(&s.new_xdg_popup.link);
    wl_list_remove(&s.new_layer_surface.link);
    wl_list_remove(&s.new_decoration.link);
    wl_list_remove(&s.new_virtual_keyboard.link);
    wl_list_remove(&s.new_input.link);
    wl_list_remove(&s.new_output.link);
    wl_list_remove(&s.request_set_selection.link);
    wl_list_remove(&s.cursor_motion.link);
    wl_list_remove(&s.cursor_motion_abs.link);
    wl_list_remove(&s.cursor_button.link);
    wl_list_remove(&s.cursor_axis.link);
    wl_list_remove(&s.cursor_frame.link);
    if (s.xwayland) {
        wl_list_remove(&s.new_xwayland_surface.link);
        wl_list_remove(&s.xwayland_ready.link);
        wlr_xwayland_destroy(s.xwayland);
        s.xwayland = NULL;
    }
    wlr_scene_node_destroy(&s.scene->tree.node);
    wlr_cursor_destroy(s.cursor);
    wlr_allocator_destroy(s.allocator);
    wlr_renderer_destroy(s.renderer);
    wlr_backend_destroy(s.backend);
    wl_display_destroy(s.display);
    return 0;
}
