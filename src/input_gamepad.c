#include "input_gamepad.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <math.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#define STICK_DEADZONE 16000
#define RESCAN_INTERVAL_MS 1000

#define BITS_PER_LONG (8 * sizeof(unsigned long))
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)

bool evdev_test_bit(const unsigned long *bits, unsigned int bit) {
    return (bits[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1UL;
}

/* ---------------------------------------------------------------- mando */

static bool is_gamepad_fd(int fd, bool *has_dpad) {
    unsigned long ev_bits[NLONGS(EV_CNT)] = {0};
    unsigned long key_bits[NLONGS(KEY_CNT)] = {0};

    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) return false;
    if (!evdev_test_bit(ev_bits, EV_KEY)) return false;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) return false;

    if (has_dpad) *has_dpad = evdev_test_bit(key_bits, BTN_DPAD_UP);
    return evdev_test_bit(key_bits, BTN_SOUTH) || evdev_test_bit(key_bits, BTN_EAST) ||
           evdev_test_bit(key_bits, BTN_MODE);
}

static void gamepad_adopt(GamepadInput *gp, int fd, const char *path, const char *name, bool has_dpad) {
    gp->fd = fd;
    gp->is_open = true;
    gp->has_dpad_buttons = has_dpad;
    gp->stick_x_prev = gp->stick_y_prev = 0;
    gp->abs_lx = gp->abs_ly = gp->abs_rx = gp->abs_ry = 0;
    gp->q_head = gp->q_len = 0;
    snprintf(gp->device_path, sizeof(gp->device_path), "%s", path);
    snprintf(gp->device_name, sizeof(gp->device_name), "%s", name);

    const char *layout = gp->cfg ? gp->cfg->gamepad_layout : "auto";
    if (strcasecmp(layout, "nintendo") == 0) gp->nintendo_layout = true;
    else if (strcasecmp(layout, "xbox") == 0) gp->nintendo_layout = false;
    else gp->nintendo_layout = strcasestr(name, "Wii U") || strcasestr(name, "Nintendo");

    printf("[Gamepad] Mando '%s' en %s (A=%s)\n", name, path,
           gp->nintendo_layout ? "BTN_EAST" : "BTN_SOUTH");
}

// Busca el mando: prefiere «Wii U GamePad»; si no, el primero con botones de mando.
static bool gamepad_scan(GamepadInput *gp) {
    DIR *d = opendir("/dev/input");
    if (!d) return false;

    int best_fd = -1, best_score = 0;
    bool best_dpad = false;
    char best_path[MAX_STR] = "", best_name[128] = "";
    struct dirent *de;

    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[MAX_STR];
        snprintf(path, sizeof(path), "/dev/input/%.200s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        bool has_dpad = false;
        char name[128] = "";
        if (is_gamepad_fd(fd, &has_dpad)) {
            if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) snprintf(name, sizeof(name), "Mando");
            int score = (strcmp(name, "Wii U GamePad") == 0) ? 2 : 1;
            if (score > best_score) {
                if (best_fd >= 0) close(best_fd);
                best_fd = fd; best_score = score; best_dpad = has_dpad;
                snprintf(best_path, sizeof(best_path), "%s", path);
                snprintf(best_name, sizeof(best_name), "%s", name);
                continue;
            }
        }
        close(fd);
    }
    closedir(d);

    if (best_fd < 0) return false;
    gamepad_adopt(gp, best_fd, best_path, best_name, best_dpad);
    return true;
}

bool gamepad_init(GamepadInput *gp, const DesktopConfig *cfg) {
    memset(gp, 0, sizeof(*gp));
    gp->fd = -1;
    gp->cfg = cfg;
    gp->stick_nav = true;

    if (cfg && !cfg->gamepad_auto && cfg->gamepad_device[0]) {
        int fd = open(cfg->gamepad_device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            bool has_dpad = false;
            char name[128] = "Mando";
            is_gamepad_fd(fd, &has_dpad);
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            gamepad_adopt(gp, fd, cfg->gamepad_device, name, has_dpad);
            return true;
        }
    }

    if (gamepad_scan(gp)) return true;
    printf("[Gamepad] Aún no hay mando; se buscará cada segundo (drc_stream lo crea al conectar).\n");
    return false;
}

void gamepad_close(GamepadInput *gp) {
    if (gp->fd >= 0) {
        close(gp->fd);
        gp->fd = -1;
    }
    gp->is_open = false;
}

void gamepad_maintain(GamepadInput *gp, uint32_t now_ms) {
    if (gp->is_open) return;
    if (now_ms - gp->last_scan_ms < RESCAN_INTERVAL_MS && gp->last_scan_ms != 0) return;
    gp->last_scan_ms = now_ms ? now_ms : 1;
    gamepad_scan(gp);
}

static void push(GamepadInput *gp, NavAction a) {
    if (a == NAV_NONE || gp->q_len >= NAV_QUEUE_LEN) return;
    gp->queue[(gp->q_head + gp->q_len) % NAV_QUEUE_LEN] = a;
    gp->q_len++;
}

static NavAction button_action(const GamepadInput *gp, int code) {
    int btn_a = gp->nintendo_layout ? BTN_EAST : BTN_SOUTH;
    int btn_b = gp->nintendo_layout ? BTN_SOUTH : BTN_EAST;
    int btn_x = gp->nintendo_layout ? BTN_NORTH : BTN_WEST;

    if (code == btn_a) return NAV_ACTION_A;
    if (code == btn_b) return NAV_ACTION_B;
    if (code == btn_x) return NAV_ACTION_X;
    switch (code) {
        case BTN_DPAD_UP:    return NAV_UP;
        case BTN_DPAD_DOWN:  return NAV_DOWN;
        case BTN_DPAD_LEFT:  return NAV_LEFT;
        case BTN_DPAD_RIGHT: return NAV_RIGHT;
        case BTN_MODE:
        case KEY_HOMEPAGE:   return NAV_ACTION_HOME;
        case BTN_START:      return NAV_ACTION_PLUS;
        case BTN_SELECT:     return NAV_ACTION_MINUS;
        case BTN_TL:         return NAV_PAGE_PREV;
        case BTN_TR:         return NAV_PAGE_NEXT;
        default:             return NAV_NONE;
    }
}

static void stick_axis(GamepadInput *gp, int value, int *prev, NavAction neg, NavAction pos) {
    int dir = 0;
    if (value < -STICK_DEADZONE) dir = -1;
    else if (value > STICK_DEADZONE) dir = 1;
    if (dir != *prev) {
        *prev = dir;
        if (dir < 0) push(gp, neg);
        else if (dir > 0) push(gp, pos);
    }
}

NavAction gamepad_poll(GamepadInput *gp) {
    if (gp->is_open && gp->fd >= 0) {
        struct input_event ev;
        ssize_t n;
        while ((n = read(gp->fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_KEY && ev.value == 1) {
                push(gp, button_action(gp, ev.code));
            } else if (ev.type == EV_ABS) {
                if (!gp->has_dpad_buttons && ev.code == ABS_HAT0X) {
                    if (ev.value < 0) push(gp, NAV_LEFT);
                    else if (ev.value > 0) push(gp, NAV_RIGHT);
                } else if (!gp->has_dpad_buttons && ev.code == ABS_HAT0Y) {
                    if (ev.value < 0) push(gp, NAV_UP);
                    else if (ev.value > 0) push(gp, NAV_DOWN);
                } else if (ev.code == ABS_X) {
                    gp->abs_lx = ev.value;
                    if (gp->stick_nav) stick_axis(gp, ev.value, &gp->stick_x_prev, NAV_LEFT, NAV_RIGHT);
                } else if (ev.code == ABS_Y) {
                    gp->abs_ly = ev.value;
                    if (gp->stick_nav) stick_axis(gp, ev.value, &gp->stick_y_prev, NAV_UP, NAV_DOWN);
                } else if (ev.code == ABS_RX || ev.code == ABS_Z) {
                    gp->abs_rx = ev.value;   // stick derecho X (estándar ABS_RX)
                } else if (ev.code == ABS_RY) {
                    gp->abs_ry = ev.value;   // stick derecho Y (estándar ABS_RY)
                }
            }
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR) {
            // ENODEV: drc_stream se cerró o el mando se desconectó; se volverá a buscar
            printf("[Gamepad] Mando '%s' desconectado (%s).\n", gp->device_name, strerror(errno));
            gamepad_close(gp);
        }
    }

    if (gp->q_len == 0) return NAV_NONE;
    NavAction a = gp->queue[gp->q_head];
    gp->q_head = (gp->q_head + 1) % NAV_QUEUE_LEN;
    gp->q_len--;
    return a;
}

