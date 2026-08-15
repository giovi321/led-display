#include "sched_window.h"

int sched_day_bit(int tm_wday)
{
    if (tm_wday == 0) return SCHED_DAYS_SUN;
    if (tm_wday < 0 || tm_wday > 6) return 0;
    return 1 << (tm_wday - 1);
}

bool sched_in_window(const sched_window_t *w, int tm_wday, int hour, int min)
{
    if (!(w->days & sched_day_bit(tm_wday))) return false;

    int on  = w->on_hour * 60 + w->on_min;
    int off = w->off_hour * 60 + w->off_min;
    int now = hour * 60 + min;

    if (on <= off) {
        return now >= on && now < off;
    }
    // Wraps midnight: e.g. on=22:00 off=06:00
    return now >= on || now < off;
}

bool sched_holds_panel_down(const sched_window_t *w, int tm_wday,
                            int hour, int min)
{
    if (!(w->days & sched_day_bit(tm_wday))) {
        // The whole point of the setting: an unticked day is either one long
        // off-window or no schedule at all, and nothing in between. Note this
        // ignores the on/off times entirely - on an unticked day they never
        // come into it.
        return w->other_days != SCHED_OTHER_RUN;
    }
    return !sched_in_window(w, tm_wday, hour, min);
}
