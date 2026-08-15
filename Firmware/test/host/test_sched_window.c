// Host tests for sched_window.c: the day mask, the on/off window, the
// midnight wrap and the rule for days outside the mask.
//   gcc -I../../main test_sched_window.c ../../main/sched_window.c && ./a.out

#include <assert.h>
#include <stdio.h>

#include "sched_window.h"

// struct tm's tm_wday, spelled out so the tests read as days not numbers.
enum { SUN = 0, MON, TUE, WED, THU, FRI, SAT };

static sched_window_t office(void)
{
    // The reported setup: Monday only, 08:00 to 19:00, unticked days dark.
    sched_window_t w = {
        .days       = SCHED_DAYS_MON,
        .on_hour    = 8,  .on_min  = 0,
        .off_hour   = 19, .off_min = 0,
        .other_days = SCHED_OTHER_OFF,
    };
    return w;
}

static void test_day_bit_matches_the_mask(void)
{
    // struct tm counts from Sunday, the mask from Monday: the one conversion
    // that would silently shift every schedule by a day if it were wrong.
    assert(sched_day_bit(MON) == SCHED_DAYS_MON);
    assert(sched_day_bit(TUE) == SCHED_DAYS_TUE);
    assert(sched_day_bit(WED) == SCHED_DAYS_WED);
    assert(sched_day_bit(THU) == SCHED_DAYS_THU);
    assert(sched_day_bit(FRI) == SCHED_DAYS_FRI);
    assert(sched_day_bit(SAT) == SCHED_DAYS_SAT);
    assert(sched_day_bit(SUN) == SCHED_DAYS_SUN);
    assert(sched_day_bit(-1) == 0);
    assert(sched_day_bit(7) == 0);
    printf("test_day_bit_matches_the_mask ok\n");
}

static void test_window_on_an_active_day(void)
{
    sched_window_t w = office();
    assert(!sched_in_window(&w, MON, 7, 59));
    assert(sched_in_window(&w, MON, 8, 0));     // inclusive at the on edge
    assert(sched_in_window(&w, MON, 18, 59));
    assert(!sched_in_window(&w, MON, 19, 0));   // exclusive at the off edge
    assert(!sched_in_window(&w, MON, 23, 59));
    printf("test_window_on_an_active_day ok\n");
}

static void test_inactive_day_is_never_in_the_window(void)
{
    sched_window_t w = office();
    // Even at a time of day the window would otherwise cover.
    assert(!sched_in_window(&w, SAT, 12, 0));
    assert(!sched_in_window(&w, SUN, 12, 0));
    printf("test_inactive_day_is_never_in_the_window ok\n");
}

static void test_other_days_off_holds_the_panel_all_day(void)
{
    sched_window_t w = office();
    // The pre-2.8.0 behaviour, kept as the default: Saturday 21:01 is dark,
    // and so is Saturday noon.
    assert(sched_holds_panel_down(&w, SAT, 21, 1));
    assert(sched_holds_panel_down(&w, SAT, 12, 0));
    assert(sched_holds_panel_down(&w, SUN, 12, 0));
    printf("test_other_days_off_holds_the_panel_all_day ok\n");
}

static void test_other_days_run_ignores_the_schedule(void)
{
    sched_window_t w = office();
    w.other_days = SCHED_OTHER_RUN;
    // The reported case: Saturday 21:01 with only Monday ticked must leave
    // the panel alone, at any hour, on every unticked day.
    assert(!sched_holds_panel_down(&w, SAT, 21, 1));
    assert(!sched_holds_panel_down(&w, SAT, 3, 0));
    assert(!sched_holds_panel_down(&w, SUN, 12, 0));
    assert(!sched_holds_panel_down(&w, FRI, 7, 30));
    printf("test_other_days_run_ignores_the_schedule ok\n");
}

static void test_other_days_does_not_touch_active_days(void)
{
    // Whichever rule is set, a ticked day still obeys its window exactly.
    for (int rule = 0; rule <= 1; rule++) {
        sched_window_t w = office();
        w.other_days = (uint8_t)rule;
        assert(sched_holds_panel_down(&w, MON, 7, 59));
        assert(!sched_holds_panel_down(&w, MON, 8, 0));
        assert(!sched_holds_panel_down(&w, MON, 18, 59));
        assert(sched_holds_panel_down(&w, MON, 19, 0));
    }
    printf("test_other_days_does_not_touch_active_days ok\n");
}

static void test_midnight_wrap(void)
{
    sched_window_t w = office();
    w.days     = SCHED_DAYS_ALL;
    w.on_hour  = 22;
    w.off_hour = 6;

    assert(sched_in_window(&w, WED, 22, 0));
    assert(sched_in_window(&w, WED, 23, 59));
    assert(sched_in_window(&w, WED, 0, 0));
    assert(sched_in_window(&w, WED, 5, 59));
    assert(!sched_in_window(&w, WED, 6, 0));
    assert(!sched_in_window(&w, WED, 21, 59));
    printf("test_midnight_wrap ok\n");
}

static void test_wrap_does_not_carry_into_an_unticked_day(void)
{
    // Documented consequence of the day check coming first: "Monday 22:00 to
    // 06:00" lights Monday's two ends, not Tuesday morning.
    sched_window_t w = office();
    w.days     = SCHED_DAYS_MON;
    w.on_hour  = 22;
    w.off_hour = 6;

    assert(sched_in_window(&w, MON, 23, 0));
    assert(sched_in_window(&w, MON, 2, 0));
    assert(!sched_in_window(&w, TUE, 2, 0));
    printf("test_wrap_does_not_carry_into_an_unticked_day ok\n");
}

static void test_all_days_never_consults_the_other_days_rule(void)
{
    sched_window_t w = office();
    w.days = SCHED_DAYS_ALL;
    for (int rule = 0; rule <= 1; rule++) {
        w.other_days = (uint8_t)rule;
        for (int d = SUN; d <= SAT; d++) {
            assert(!sched_holds_panel_down(&w, d, 12, 0));
            assert(sched_holds_panel_down(&w, d, 22, 0));
        }
    }
    printf("test_all_days_never_consults_the_other_days_rule ok\n");
}

static void test_empty_mask(void)
{
    sched_window_t w = office();
    w.days = 0;
    // No day ticked at all: entirely governed by the other-days rule.
    w.other_days = SCHED_OTHER_OFF;
    assert(sched_holds_panel_down(&w, MON, 12, 0));
    w.other_days = SCHED_OTHER_RUN;
    assert(!sched_holds_panel_down(&w, MON, 12, 0));
    printf("test_empty_mask ok\n");
}

int main(void)
{
    test_day_bit_matches_the_mask();
    test_window_on_an_active_day();
    test_inactive_day_is_never_in_the_window();
    test_other_days_off_holds_the_panel_all_day();
    test_other_days_run_ignores_the_schedule();
    test_other_days_does_not_touch_active_days();
    test_midnight_wrap();
    test_wrap_does_not_carry_into_an_unticked_day();
    test_all_days_never_consults_the_other_days_rule();
    test_empty_mask();
    printf("sched_window: all tests passed\n");
    return 0;
}
