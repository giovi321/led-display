#include "light_curve.h"

#include <math.h>

void light_curve_defaults(light_curve_t *c)
{
    if (!c) return;
    // An LDR divider covered reads low, lit reads high; the brightness ends
    // stay conservative so a first enable cannot slam a big panel to full.
    c->dark_raw   = 200;
    c->bright_raw = 2800;
    c->min_bri    = 8;
    c->max_bri    = 128;
    c->gamma_x10  = LIGHT_GAMMA_UNITY;
}

bool light_curve_valid(const light_curve_t *c)
{
    if (!c) return false;
    if (c->dark_raw > LIGHT_RAW_MAX || c->bright_raw > LIGHT_RAW_MAX) return false;
    if (c->dark_raw == c->bright_raw) return false;
    if (c->gamma_x10 < LIGHT_GAMMA_MIN || c->gamma_x10 > LIGHT_GAMMA_MAX) return false;
    return true;
}

bool light_curve_valid_gpio(int gpio)
{
    return gpio >= LIGHT_GPIO_MIN && gpio <= LIGHT_GPIO_MAX;
}

uint8_t light_curve_brightness(const light_curve_t *c, int raw)
{
    if (!c) return 0;
    if (raw < 0) raw = 0;
    if (raw > LIGHT_RAW_MAX) raw = LIGHT_RAW_MAX;

    // No axis flip: a sensor that reads high in the dark arrives as
    // dark_raw > bright_raw and the signed slope below handles it.
    int lo = (int)c->dark_raw;
    int hi = (int)c->bright_raw;
    if (lo == hi) return c->min_bri;   // rejected by light_curve_valid()

    float t = (float)(raw - lo) / (float)(hi - lo);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    if (c->gamma_x10 != LIGHT_GAMMA_UNITY && t > 0.0f) {
        t = powf(t, (float)c->gamma_x10 / 10.0f);
    }

    int span = (int)c->max_bri - (int)c->min_bri;
    int bri  = (int)c->min_bri + (int)lrintf(t * (float)span);
    if (bri < 0)   bri = 0;
    if (bri > 255) bri = 255;
    return (uint8_t)bri;
}

int light_curve_smooth(int prev, int raw)
{
    if (prev < 0) return raw;
    int d = raw - prev;
    int step = d / 4;
    if (step == 0 && d != 0) step = (d > 0) ? 1 : -1;
    return prev + step;
}

bool light_curve_material(int applied, int candidate, int hyst)
{
    if (applied < 0) return true;
    if (candidate == applied) return false;
    if (candidate == 0) return true;             // going dark reads as one step
    // Leaving 0 has to clear the band as well. Exempting it both ways lets a
    // reading that jitters across the 0/1 boundary re-apply on every sample.
    if (applied == 0) return candidate >= hyst;
    int d = candidate - applied;
    if (d < 0) d = -d;
    return d >= hyst;
}
