#include "wm_frameclock.h"

#include <string.h>

void wm_frameclock_init(wm_frameclock *fc) { memset(fc, 0, sizeof(*fc)); }

bool wm_frameclock_set_active(wm_frameclock *fc, bool active) {
    bool stopped = fc->active && !active;
    fc->active = active;
    if (!active) fc->due = false;
    return stopped;
}

void wm_frameclock_tick(wm_frameclock *fc) {
    fc->ticks++;
    fc->due = true;
}

bool wm_frameclock_accept(wm_frameclock *fc) {
    if (!fc->active) return true;
    if (!fc->due) { fc->ignored++; return false; }
    fc->due = false;
    fc->accepted++;
    return true;
}
