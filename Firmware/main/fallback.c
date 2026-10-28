#include "fallback.h"

#include <string.h>

void fb_init(fb_state_t *s, int64_t now_s)
{
    memset(s, 0, sizeof(*s));
    s->active            = true;
    s->reason            = FB_REASON_NO_VALUE;
    s->last_value_s      = now_s;
    s->poll_armed        = true;
    s->mqtt_armed        = true;
    s->mqtt_down_since_s = -1;
}

bool fb_notifies(fb_reason_t r)
{
    return r != FB_REASON_NONE && r != FB_REASON_NO_VALUE;
}

// A trigger fired. Engage when inactive, or upgrade NO_VALUE to a notifying
// reason. When already active with a notifying reason, nothing changes.
static fb_change_t engage(fb_state_t *s, fb_reason_t reason)
{
    if (s->active && s->reason != FB_REASON_NO_VALUE) return FB_CHANGE_NONE;
    s->active = true;
    s->reason = reason;
    return FB_CHANGE_ENGAGED;
}

fb_change_t fb_on_value(fb_state_t *s, int64_t now_s)
{
    bool was_active = s->active;
    s->have_value      = true;
    s->last_value_s    = now_s;
    s->poll_fail_count = 0;
    s->poll_armed      = true;
    // Still disconnected: that disconnect has already had its chance to fire.
    if (s->mqtt_down_since_s < 0) s->mqtt_armed = true;
    s->active = false;
    s->reason = FB_REASON_NONE;
    return was_active ? FB_CHANGE_CLEARED : FB_CHANGE_NONE;
}

fb_change_t fb_on_poll_result(fb_state_t *s, const fallback_cfg_t *c, bool ok,
                              int64_t now_s)
{
    if (ok) return fb_on_value(s, now_s);
    if (s->poll_fail_count < UINT16_MAX) s->poll_fail_count++;
    if (c->poll_failures > 0 && s->poll_armed &&
        s->poll_fail_count >= c->poll_failures) {
        s->poll_armed = false;
        return engage(s, FB_REASON_POLL_FAILURES);
    }
    return FB_CHANGE_NONE;
}

fb_change_t fb_on_mqtt(fb_state_t *s, bool connected, int64_t now_s)
{
    if (connected) {
        s->mqtt_down_since_s = -1;
        s->mqtt_armed        = true;
    } else if (s->mqtt_down_since_s < 0) {
        s->mqtt_down_since_s = now_s;
    }
    return FB_CHANGE_NONE;
}

fb_change_t fb_tick(fb_state_t *s, const fallback_cfg_t *c, bool mqtt_enabled,
                    int64_t now_s)
{
    if (c->after_min > 0 &&
        now_s - s->last_value_s >= (int64_t)c->after_min * 60) {
        fb_change_t ch = engage(s, FB_REASON_TIMEOUT);
        if (ch != FB_CHANGE_NONE) return ch;
    }
    if (c->on_mqtt_loss && mqtt_enabled && s->mqtt_armed &&
        s->mqtt_down_since_s >= 0 &&
        now_s - s->mqtt_down_since_s > FB_MQTT_GRACE_S) {
        s->mqtt_armed = false;
        return engage(s, FB_REASON_MQTT_LOST);
    }
    return FB_CHANGE_NONE;
}

const char *fb_reason_str(fb_reason_t r)
{
    switch (r) {
    case FB_REASON_NO_VALUE:      return "no_value";
    case FB_REASON_TIMEOUT:       return "timeout";
    case FB_REASON_POLL_FAILURES: return "poll_failures";
    case FB_REASON_MQTT_LOST:     return "mqtt_lost";
    default:                      return "";
    }
}

const char *fb_mode_str(fb_mode_t m)
{
    switch (m) {
    case FB_MODE_MARKER: return "marker";
    case FB_MODE_DIM:    return "dim";
    default:             return "text";
    }
}

bool fb_mode_from_str(const char *s, fb_mode_t *out)
{
    if (!s) return false;
    if (!strcmp(s, "text"))   { *out = FB_MODE_TEXT;   return true; }
    if (!strcmp(s, "marker")) { *out = FB_MODE_MARKER; return true; }
    if (!strcmp(s, "dim"))    { *out = FB_MODE_DIM;    return true; }
    return false;
}

void fb_cfg_defaults(fallback_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->mode          = FB_MODE_TEXT;
    c->poll_failures = 3;
}

static void copy_text(char *dst, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n > FB_TEXT_MAX) n = FB_TEXT_MAX;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

void fb_migrate_legacy(fallback_cfg_t *out, uint16_t stale_hours,
                       int stale_action, const char *stale_text,
                       const char *default_text)
{
    fb_cfg_defaults(out);
    copy_text(out->text, (default_text && default_text[0]) ? default_text
                                                           : stale_text);
    uint32_t min = (uint32_t)stale_hours * 60u;
    if (min > FB_AFTER_MIN_MAX) min = FB_AFTER_MIN_MAX;
    out->after_min = (uint16_t)min;
    switch (stale_action) {
    case 1:  out->mode = FB_MODE_DIM;    break;
    case 2:  out->mode = FB_MODE_MARKER; break;
    case 3:  out->mode = FB_MODE_TEXT;   break;
    default: out->mode = FB_MODE_TEXT;   out->after_min = 0; break;
    }
}
