#ifndef TOUCH_GESTURES_H
#define TOUCH_GESTURES_H

#include "config.h"
#include <stdbool.h>
#include <stdint.h>

#define MAX_TOUCH_POINTS 10

typedef enum {
    GESTURE_NONE = 0,
    GESTURE_TAP,
    GESTURE_DRAG
} GestureEvent;

typedef struct {
    int32_t id;
    double start_x, start_y;
    double current_x, current_y;
    uint32_t start_time_ms;
    bool active;
} TouchPoint;

typedef struct {
    TouchPoint points[MAX_TOUCH_POINTS];
    int active_fingers;
    int max_fingers_seen;
    double initial_avg_x, initial_avg_y;
    double current_avg_x, current_avg_y;
    uint32_t gesture_start_time;
    // Inicio del último dedo levantado (para reconocer gestos desde el borde)
    double last_start_x, last_start_y;

} TouchGestureTracker;

void touch_tracker_init(TouchGestureTracker *tracker, const DesktopConfig *cfg);
void touch_tracker_down(TouchGestureTracker *tracker, int32_t id, double x, double y, uint32_t time_ms);
void touch_tracker_motion(TouchGestureTracker *tracker, int32_t id, double x, double y, uint32_t time_ms);
GestureEvent touch_tracker_up(TouchGestureTracker *tracker, int32_t id, uint32_t time_ms, double *out_x, double *out_y);
void touch_tracker_cancel(TouchGestureTracker *tracker);

#endif // TOUCH_GESTURES_H
