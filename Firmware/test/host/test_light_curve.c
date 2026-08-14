// Host tests for light_curve.c: the ambient-reading -> brightness mapping, the
// smoothing filter and the hysteresis rule the sampling task applies.
//   gcc -I../../main test_light_curve.c ../../main/light_curve.c -lm && ./a.out

#include <assert.h>
#include <stdio.h>

#include "light_curve.h"

static light_curve_t base(void)
{
    light_curve_t c;
    light_curve_defaults(&c);
    c.dark_raw   = 0;
    c.bright_raw = 1000;
    c.min_bri    = 0;
    c.max_bri    = 100;
    c.gamma_x10  = LIGHT_GAMMA_UNITY;
    return c;
}

static void test_defaults_are_valid(void)
{
    light_curve_t c;
    light_curve_defaults(&c);
    assert(light_curve_valid(&c));
    assert(c.gamma_x10 == LIGHT_GAMMA_UNITY);
    printf("test_defaults_are_valid ok\n");
}

static void test_linear_ends_and_middle(void)
{
    light_curve_t c = base();
    assert(light_curve_brightness(&c, 0) == 0);
    assert(light_curve_brightness(&c, 1000) == 100);
    assert(light_curve_brightness(&c, 500) == 50);
    assert(light_curve_brightness(&c, 250) == 25);
    printf("test_linear_ends_and_middle ok\n");
}

// Readings outside the calibrated window pin to the nearest end instead of
// running off: a sunlit sensor must not wrap round to dark.
static void test_clamps_outside_window(void)
{
    light_curve_t c = base();
    c.min_bri = 10;
    c.max_bri = 80;
    assert(light_curve_brightness(&c, -50) == 10);
    assert(light_curve_brightness(&c, 4095) == 80);
    assert(light_curve_brightness(&c, 999999) == 80);
    printf("test_clamps_outside_window ok\n");
}

// A pulled-up LDR reads high in the dark: the calibration pair then arrives
// with dark_raw > bright_raw and must still map the right way round.
static void test_reversed_calibration_points(void)
{
    light_curve_t c = base();
    c.dark_raw   = 3000;
    c.bright_raw = 500;
    c.min_bri    = 5;
    c.max_bri    = 205;
    assert(light_curve_brightness(&c, 3000) == 5);
    assert(light_curve_brightness(&c, 500) == 205);
    assert(light_curve_brightness(&c, 1750) == 105);
    assert(light_curve_brightness(&c, 4095) == 5);    // darker than "dark"
    printf("test_reversed_calibration_points ok\n");
}

// The pair the calibration UI actually produces for a pulled-up LDR: reversed
// and not symmetric about half scale, which is where an extra axis flip used to
// map the whole curve backwards.
static void test_reversed_pair_asymmetric(void)
{
    light_curve_t c = base();
    c.dark_raw   = 3800;
    c.bright_raw = 300;
    c.min_bri    = 8;
    c.max_bri    = 128;
    assert(light_curve_brightness(&c, 3800) == 8);
    assert(light_curve_brightness(&c, 300) == 128);
    assert(light_curve_brightness(&c, 4095) == 8);    // darker than "dark"
    assert(light_curve_brightness(&c, 0) == 128);     // brighter than "bright"
    printf("test_reversed_pair_asymmetric ok\n");
}

// gamma > 1 holds the panel down until the room is properly lit; gamma < 1
// brightens early. Both keep the two ends exactly where the linear curve has
// them - only the shape between them changes.
static void test_gamma_shape(void)
{
    light_curve_t c = base();

    c.gamma_x10 = 22;
    uint8_t slow = light_curve_brightness(&c, 500);
    c.gamma_x10 = 5;
    uint8_t fast = light_curve_brightness(&c, 500);
    c.gamma_x10 = LIGHT_GAMMA_UNITY;
    uint8_t lin = light_curve_brightness(&c, 500);

    assert(slow < lin);
    assert(fast > lin);

    for (int g = LIGHT_GAMMA_MIN; g <= LIGHT_GAMMA_MAX; g++) {
        c.gamma_x10 = (uint8_t)g;
        assert(light_curve_brightness(&c, 0) == 0);
        assert(light_curve_brightness(&c, 1000) == 100);
    }
    printf("test_gamma_shape ok\n");
}

// A window narrower than the brightness range still has to produce every step
// monotonically - no dead spots where a change of light does nothing.
static void test_monotonic(void)
{
    light_curve_t c = base();
    c.dark_raw   = 100;
    c.bright_raw = 900;
    c.min_bri    = 3;
    c.max_bri    = 255;
    c.gamma_x10  = 18;

    int prev = -1;
    for (int raw = 0; raw <= LIGHT_RAW_MAX; raw += 7) {
        int b = light_curve_brightness(&c, raw);
        assert(b >= prev);
        prev = b;
    }
    assert(prev == 255);
    printf("test_monotonic ok\n");
}

static void test_validation(void)
{
    light_curve_t c = base();
    assert(light_curve_valid(&c));

    light_curve_t bad = c;
    bad.dark_raw = bad.bright_raw = 500;      // no slope
    assert(!light_curve_valid(&bad));

    bad = c;
    bad.gamma_x10 = LIGHT_GAMMA_MIN - 1;
    assert(!light_curve_valid(&bad));
    bad.gamma_x10 = LIGHT_GAMMA_MAX + 1;
    assert(!light_curve_valid(&bad));

    bad = c;
    bad.bright_raw = LIGHT_RAW_MAX + 1;
    assert(!light_curve_valid(&bad));

    assert(!light_curve_valid(NULL));
    // A degenerate window must not crash the mapper even so.
    light_curve_t flat = c;
    flat.dark_raw = flat.bright_raw = 42;
    flat.min_bri = 7;
    assert(light_curve_brightness(&flat, 42) == 7);
    assert(light_curve_brightness(NULL, 42) == 0);
    printf("test_validation ok\n");
}

// ESP32-S3: ADC1 only (GPIO1..GPIO10). ADC2 shares hardware with WiFi.
static void test_valid_gpio(void)
{
    assert(!light_curve_valid_gpio(0));
    assert(light_curve_valid_gpio(1));
    assert(light_curve_valid_gpio(10));
    assert(!light_curve_valid_gpio(11));
    assert(!light_curve_valid_gpio(16));   // the LED data pin
    assert(!light_curve_valid_gpio(-3));
    printf("test_valid_gpio ok\n");
}

static void test_smoothing(void)
{
    assert(light_curve_smooth(-1, 800) == 800);   // first sample seeds
    assert(light_curve_smooth(800, 800) == 800);
    assert(light_curve_smooth(0, 400) == 100);    // quarter of the step
    assert(light_curve_smooth(400, 0) == 300);

    // Small differences still converge instead of parking short of the target.
    int v = 100;
    for (int i = 0; i < 40; i++) v = light_curve_smooth(v, 103);
    assert(v == 103);
    v = 100;
    for (int i = 0; i < 40; i++) v = light_curve_smooth(v, 97);
    assert(v == 97);

    // A step change lands within a handful of samples, not instantly.
    v = light_curve_smooth(-1, 0);
    int n = 0;
    while (v < 1000 && n < 200) { v = light_curve_smooth(v, 1000); n++; }
    assert(v == 1000 && n > 5);
    printf("test_smoothing ok\n");
}

static void test_hysteresis(void)
{
    assert(light_curve_material(-1, 0, LIGHT_HYSTERESIS));    // first reading
    assert(!light_curve_material(50, 50, LIGHT_HYSTERESIS));
    assert(!light_curve_material(50, 52, LIGHT_HYSTERESIS));  // inside the band
    assert(light_curve_material(50, 53, LIGHT_HYSTERESIS));
    assert(light_curve_material(50, 47, LIGHT_HYSTERESIS));
    // Going fully dark is always worth applying; coming back out of it needs
    // the full band, or a reading on the boundary flaps once per sample.
    assert(light_curve_material(2, 0, LIGHT_HYSTERESIS));
    assert(!light_curve_material(0, 1, LIGHT_HYSTERESIS));
    assert(light_curve_material(0, LIGHT_HYSTERESIS, LIGHT_HYSTERESIS));
    assert(!light_curve_material(0, 0, LIGHT_HYSTERESIS));
    printf("test_hysteresis ok\n");
}

// min_brightness 0 puts a 0/1 step in the curve. A reading jittering across it
// must settle instead of re-applying on every sample (a visible 1 Hz blink).
static void test_zero_boundary_settles(void)
{
    int applied = -1, applies = 0;
    const int candidates[] = {0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1};
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (light_curve_material(applied, candidates[i], LIGHT_HYSTERESIS)) {
            applied = candidates[i];
            applies++;
        }
    }
    assert(applies == 1);      // the first sample only
    assert(applied == 0);

    // A genuine dim from a lit room still reaches 0 in one step.
    applied = 40;
    assert(light_curve_material(applied, 0, LIGHT_HYSTERESIS));
    printf("test_zero_boundary_settles ok\n");
}

int main(void)
{
    test_defaults_are_valid();
    test_linear_ends_and_middle();
    test_clamps_outside_window();
    test_reversed_calibration_points();
    test_reversed_pair_asymmetric();
    test_gamma_shape();
    test_monotonic();
    test_validation();
    test_valid_gpio();
    test_smoothing();
    test_hysteresis();
    test_zero_boundary_settles();
    printf("light_curve: all tests passed\n");
    return 0;
}
