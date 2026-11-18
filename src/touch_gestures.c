#include "touch_gestures.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#define TAP_MAX_DISTANCE 15.0

void touch_tracker_init(TouchGestureTracker *tracker, const DesktopConfig *cfg) {
    memset(tracker, 0, sizeof(*tracker));
    (void)cfg;
}

static void update_averages(TouchGestureTracker *tracker) {
    double sum_cur_x = 0, sum_cur_y = 0;
    double sum_init_x = 0, sum_init_y = 0;
    int count = 0;

    for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
        if (tracker->points[i].active) {
            sum_cur_x += tracker->points[i].current_x;
            sum_cur_y += tracker->points[i].current_y;
            sum_init_x += tracker->points[i].start_x;
            sum_init_y += tracker->points[i].start_y;
            count++;
        }
    }

    if (count > 0) {
        tracker->current_avg_x = sum_cur_x / count;
        tracker->current_avg_y = sum_cur_y / count;
        tracker->initial_avg_x = sum_init_x / count;
        tracker->initial_avg_y = sum_init_y / count;
    }
}

void touch_tracker_down(TouchGestureTracker *tracker, int32_t id, double x, double y, uint32_t time_ms) {
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
        if (!tracker->points[i].active) {
            tracker->points[i].id = id;
            tracker->points[i].start_x = x;
            tracker->points[i].start_y = y;
            tracker->points[i].current_x = x;
            tracker->points[i].current_y = y;
            tracker->points[i].start_time_ms = time_ms;
            tracker->points[i].active = true;
            tracker->active_fingers++;
            if (tracker->active_fingers > tracker->max_fingers_seen) {
                tracker->max_fingers_seen = tracker->active_fingers;
            }
            if (tracker->active_fingers == 1) {
                tracker->gesture_start_time = time_ms;
            }
            update_averages(tracker);
            return;
        }
    }
}

void touch_tracker_motion(TouchGestureTracker *tracker, int32_t id, double x, double y, uint32_t time_ms) {
    (void)time_ms;
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
        if (tracker->points[i].active && tracker->points[i].id == id) {
            tracker->points[i].current_x = x;
            tracker->points[i].current_y = y;
            update_averages(tracker);
            return;
        }
    }
}

GestureEvent touch_tracker_up(TouchGestureTracker *tracker, int32_t id, uint32_t time_ms, double *out_x, double *out_y) {
    (void)time_ms;
    GestureEvent event = GESTURE_NONE;
    int idx = -1;

    for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
        if (tracker->points[i].active && tracker->points[i].id == id) {
            idx = i;
            break;
        }
    }

    if (idx == -1) return GESTURE_NONE;

    double tap_x = tracker->points[idx].current_x;
    double tap_y = tracker->points[idx].current_y;
    double dist = hypot(tap_x - tracker->points[idx].start_x, tap_y - tracker->points[idx].start_y);

    tracker->last_start_x = tracker->points[idx].start_x;
    tracker->last_start_y = tracker->points[idx].start_y;
    if (out_x) *out_x = tap_x;
    if (out_y) *out_y = tap_y;

    // Desactivar el punto que se levantó
    tracker->points[idx].active = false;
    tracker->active_fingers--;

    // Cuando se levantan todos los dedos, evaluamos el gesto global acumulado
    if (tracker->active_fingers == 0) {
        double dx = tracker->current_avg_x - tracker->initial_avg_x;
        double dy = tracker->current_avg_y - tracker->initial_avg_y;

        // Solo toques: con varios dedos o con recorrido no hay gesto
        (void)dx; (void)dy;
        if (tracker->max_fingers_seen == 1 && dist < TAP_MAX_DISTANCE) event = GESTURE_TAP;

        // Reiniciar conteo para el próximo gesto
        tracker->max_fingers_seen = 0;
    }

    return event;
}

void touch_tracker_cancel(TouchGestureTracker *tracker) {
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
        tracker->points[i].active = false;
    }
    tracker->active_fingers = 0;
    tracker->max_fingers_seen = 0;
}
