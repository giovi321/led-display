// Host tests for power_model.c: the WS2812B current estimate, the headroom
// figure and the brightness the limiter falls back to.
//   gcc -I../../main test_power_model.c ../../main/power_model.c && ./a.out

#include <assert.h>
#include <stdio.h>

#include "power_model.h"

#define AMBER_SUM 112      // the stored default colour, R=80 G=32 B=0
#define WHITE_SUM 765      // 255 * 3

static power_cfg_t base(void)
{
    power_cfg_t c;
    power_model_default(&c);
    return c;
}

static void test_defaults_are_valid(void)
{
    power_cfg_t c = base();
    assert(power_model_valid(&c));
    assert(c.budget_ma      == POWER_BUDGET_MA_DEFAULT);
    assert(c.ua_per_channel == POWER_UA_PER_CHANNEL_DEFAULT);
    assert(c.ua_quiescent   == POWER_UA_QUIESCENT_DEFAULT);
    // Enforcement is opt-in: an existing device must behave exactly as it did
    // before the update until the user ticks the box.
    assert(c.limit_enabled  == false);
    printf("test_defaults_are_valid ok\n");
}

static void test_validation(void)
{
    power_cfg_t c = base();

    // A budget of 0 is the "no budget configured" case, not an invalid one.
    c.budget_ma = 0;
    assert(power_model_valid(&c));
    c.budget_ma = POWER_BUDGET_MA_MAX;
    assert(power_model_valid(&c));
    c.budget_ma = POWER_BUDGET_MA_MAX + 1;
    assert(!power_model_valid(&c));

    c = base();
    c.ua_per_channel = 0;              // a chain that draws nothing is not a model
    assert(!power_model_valid(&c));
    c.ua_per_channel = POWER_UA_PER_CHANNEL_MAX + 1;
    assert(!power_model_valid(&c));

    c = base();
    c.ua_quiescent = 0;                // plausible: a panel with a better controller
    assert(power_model_valid(&c));
    c.ua_quiescent = POWER_UA_QUIESCENT_MAX + 1;
    assert(!power_model_valid(&c));

    printf("test_validation ok\n");
}

static void test_black_frame_is_quiescent_only(void)
{
    power_cfg_t c = base();

    // 1 mA per LED with every channel at 0, so the answer is the chain length
    // in mA. This is the floor no brightness setting can get under, and the
    // reason a long chain can exceed a small budget while showing nothing.
    assert(power_model_frame_ma(&c, 1024, 0, 255) == 1024);
    assert(power_model_frame_ma(&c, 1024, 0, 0)   == 1024);
    assert(power_model_frame_ma(&c, 0, 0, 255)    == 0);

    c.ua_quiescent = 0;
    assert(power_model_frame_ma(&c, 1024, 0, 255) == 0);
    printf("test_black_frame_is_quiescent_only ok\n");
}

static void test_reproduces_the_documented_measurement(void)
{
    // The one real figure in the docs: 256 LEDs, all lit at the stored amber
    // (80, 32, 0), brightness 255, drew 7.7 W at the 5 V USB-C input on
    // 2026-05-20. Of that, the project notes put roughly 0.5 W on the
    // ESP32-S3, leaving about 7.2 W = 1440 mA for the LEDs.
    //
    // Solving the model for that point gives about 10.5 mA per channel, well
    // under the 20 mA datasheet nominal the defaults use. This test pins the
    // arithmetic to the measurement, and the gap to the default is exactly
    // why the constant is configurable.
    power_cfg_t c = base();
    c.ua_per_channel = 10530;

    const uint32_t ma = power_model_frame_ma(&c, 256, 256 * AMBER_SUM, 255);
    assert(ma >= 1435 && ma <= 1445);

    // The same rig under the conservative default predicts roughly twice that,
    // which is the whole point: the default errs towards dimming early.
    c = base();
    const uint32_t conservative = power_model_frame_ma(&c, 256, 256 * AMBER_SUM, 255);
    assert(conservative > 2400 && conservative < 2600);
    printf("test_reproduces_the_documented_measurement ok\n");
}

static void test_brightness_scales_the_lit_part_only(void)
{
    power_cfg_t c = base();
    const uint32_t px  = 1024;
    const uint32_t sum = px * AMBER_SUM;

    const uint32_t full = power_model_frame_ma(&c, px, sum, 255);
    const uint32_t half = power_model_frame_ma(&c, px, sum, 128);
    const uint32_t off  = power_model_frame_ma(&c, px, sum, 0);

    assert(off == px);                       // quiescent floor
    // The lit contribution is proportional to the brightness, and the
    // quiescent floor sits under it at every brightness. Cross-multiplied
    // rather than halved, because 128/255 is not exactly a half.
    const uint32_t lit_full = full - off;
    const uint32_t lit_half = half - off;
    const uint32_t lhs = lit_half * 255;
    const uint32_t rhs = lit_full * 128;
    assert(lhs + 2000 > rhs && rhs + 2000 > lhs);
    printf("test_brightness_scales_the_lit_part_only ok\n");
}

static void test_monotonic_in_brightness_and_content(void)
{
    power_cfg_t c = base();
    uint32_t prev = 0;
    for (int eff = 0; eff <= 255; eff++) {
        const uint32_t ma = power_model_frame_ma(&c, 1024, 1024 * AMBER_SUM,
                                                 (uint8_t)eff);
        assert(ma >= prev);
        prev = ma;
    }
    prev = 0;
    for (uint32_t lit = 0; lit <= 1024; lit += 16) {
        const uint32_t ma = power_model_frame_ma(&c, 1024, lit * WHITE_SUM, 255);
        assert(ma >= prev);
        prev = ma;
    }
    printf("test_monotonic_in_brightness_and_content ok\n");
}

static void test_worst_case_matches_a_fully_lit_frame(void)
{
    power_cfg_t c = base();
    const uint32_t px = 1024;

    assert(power_model_worst_ma(&c, px, 80, 32, 0, 255) ==
           power_model_frame_ma(&c, px, px * AMBER_SUM, 255));
    assert(power_model_worst_ma(&c, px, 0, 0, 0, 255) == px);

    // The largest frame the firmware accepts, at white and full brightness:
    // 8192 * 765 * 255 * 20000 overflows 32 bits, so this is the intermediate
    // width check as much as the arithmetic.
    const uint32_t big = power_model_worst_ma(&c, 8192, 255, 255, 255, 255);
    const uint32_t expect = 8192 * 1 + 8192 * 3 * 20;   // mA: quiescent + 3 channels
    assert(big == expect);
    printf("test_worst_case_matches_a_fully_lit_frame ok\n");
}

static void test_fit_leaves_a_fitting_frame_alone(void)
{
    power_cfg_t c = base();
    // 1024 LEDs, a line of text: inside 2600 mA, so the limiter must not touch
    // it. Silent dimming of frames that were always fine is the failure mode
    // that would make this feature worse than the fuse.
    //
    // 120 amber pixels at full brightness is about 2080 mA. Under the default
    // constants the budget runs out at roughly 180 such pixels, so the margin
    // here is deliberately modest rather than generous.
    assert(power_model_fit_brightness(&c, 1024, 120 * AMBER_SUM, 255) == 255);
    assert(power_model_fit_brightness(&c, 1024, 181 * AMBER_SUM, 255) < 255);
    assert(power_model_fit_brightness(&c, 1024, 0, 200) == 200);

    c.budget_ma = 0;   // no budget configured: never limit
    assert(power_model_fit_brightness(&c, 8192, 8192 * WHITE_SUM, 255) == 255);
    printf("test_fit_leaves_a_fitting_frame_alone ok\n");
}

static void test_fit_result_fits_the_budget(void)
{
    power_cfg_t c = base();
    const uint32_t px = 1024;

    // Sweep coverage and colour; whatever brightness comes back must produce a
    // frame at or under the budget, and must never exceed what was asked for.
    for (uint32_t lit = 0; lit <= px; lit += 7) {
        const uint32_t sums[] = {lit * AMBER_SUM, lit * WHITE_SUM, lit * 255};
        for (unsigned i = 0; i < sizeof(sums) / sizeof(sums[0]); i++) {
            for (int asked = 0; asked <= 255; asked += 51) {
                const uint8_t got = power_model_fit_brightness(&c, px, sums[i],
                                                               (uint8_t)asked);
                assert(got <= asked);
                assert(power_model_frame_ma(&c, px, sums[i], got) <= (uint32_t)c.budget_ma);
            }
        }
    }
    printf("test_fit_result_fits_the_budget ok\n");
}

static void test_fit_is_monotonic_in_the_budget(void)
{
    power_cfg_t c = base();
    const uint32_t px  = 1024;
    const uint32_t sum = px * WHITE_SUM;   // every pixel white: needs limiting

    uint8_t prev = 0;
    for (uint32_t budget = 1024; budget <= 25000; budget += 250) {
        c.budget_ma = (uint16_t)budget;
        const uint8_t got = power_model_fit_brightness(&c, px, sum, 255);
        assert(got >= prev);
        prev = got;
    }
    printf("test_fit_is_monotonic_in_the_budget ok\n");
}

static void test_fit_gives_up_when_quiescent_alone_exceeds_the_budget(void)
{
    power_cfg_t c = base();
    c.budget_ma = 500;   // 1024 LEDs draw ~1024 mA with everything off

    // Nothing a brightness can do: the answer is 0, and the caller is expected
    // to say so rather than pretend the budget is being met.
    assert(power_model_fit_brightness(&c, 1024, 1024 * WHITE_SUM, 255) == 0);
    assert(power_model_fit_brightness(&c, 1024, 0, 255) == 0);

    // Exactly at the floor still leaves no room for light.
    c.budget_ma = 1024;
    assert(power_model_fit_brightness(&c, 1024, 1024 * WHITE_SUM, 255) == 0);
    printf("test_fit_gives_up_when_quiescent_alone_exceeds_the_budget ok\n");
}

static void test_the_reported_symptom(void)
{
    // The bug this exists for: the reference build, a colour and brightness
    // that trip the fuse. 1024 LEDs, white text at full brightness over about
    // a quarter of the panel is far past 2600 mA, and the limiter has to bring
    // it back under without the user touching anything.
    power_cfg_t c = base();
    c.limit_enabled = true;
    const uint32_t px  = 1024;
    const uint32_t sum = 256 * WHITE_SUM;

    assert(power_model_frame_ma(&c, px, sum, 255) > (uint32_t)c.budget_ma);
    const uint8_t got = power_model_fit_brightness(&c, px, sum, 255);
    assert(got > 0 && got < 255);
    assert(power_model_frame_ma(&c, px, sum, got) <= (uint32_t)c.budget_ma);
    printf("test_the_reported_symptom ok\n");
}

int main(void)
{
    test_defaults_are_valid();
    test_validation();
    test_black_frame_is_quiescent_only();
    test_reproduces_the_documented_measurement();
    test_brightness_scales_the_lit_part_only();
    test_monotonic_in_brightness_and_content();
    test_worst_case_matches_a_fully_lit_frame();
    test_fit_leaves_a_fitting_frame_alone();
    test_fit_result_fits_the_budget();
    test_fit_is_monotonic_in_the_budget();
    test_fit_gives_up_when_quiescent_alone_exceeds_the_budget();
    test_the_reported_symptom();
    printf("power_model: all tests passed\n");
    return 0;
}
