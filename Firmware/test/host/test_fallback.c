// Host-side unit tests for fallback.c (plain gcc + assert, no ESP-IDF).
// Fake clock: every call takes now_s explicitly.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "fallback.h"

static fallback_cfg_t cfg(uint16_t after_min, uint8_t poll_failures, bool mqtt)
{
    fallback_cfg_t c;
    fb_cfg_defaults(&c);
    c.after_min     = after_min;
    c.poll_failures = poll_failures;
    c.on_mqtt_loss  = mqtt;
    return c;
}

static void test_boot(void)
{
    fb_state_t s;
    fb_init(&s, 0);
    assert(s.active);
    assert(s.reason == FB_REASON_NO_VALUE);
    assert(!fb_notifies(s.reason));
    assert(fb_on_value(&s, 5) == FB_CHANGE_CLEARED);
    assert(!s.active);
    assert(s.reason == FB_REASON_NONE);
    assert(fb_on_value(&s, 6) == FB_CHANGE_NONE);
}

static void test_timeout(void)
{
    fallback_cfg_t c = cfg(10, 0, false);
    fb_state_t s;
    fb_init(&s, 0);
    fb_on_value(&s, 0);
    assert(fb_tick(&s, &c, false, 599) == FB_CHANGE_NONE);
    assert(fb_tick(&s, &c, false, 600) == FB_CHANGE_ENGAGED);
    assert(s.active && s.reason == FB_REASON_TIMEOUT);
    assert(fb_notifies(s.reason));
    assert(fb_tick(&s, &c, false, 900) == FB_CHANGE_NONE);
    assert(fb_on_value(&s, 901) == FB_CHANGE_CLEARED);
    assert(!s.active);
}

static void test_timeout_no_value_ever(void)
{
    fallback_cfg_t c = cfg(10, 0, false);
    fb_state_t s;
    fb_init(&s, 0);
    assert(fb_tick(&s, &c, false, 599) == FB_CHANGE_NONE);
    assert(s.reason == FB_REASON_NO_VALUE);
    assert(fb_tick(&s, &c, false, 600) == FB_CHANGE_ENGAGED);
    assert(s.active && s.reason == FB_REASON_TIMEOUT);
}

static void test_poll_threshold(void)
{
    fallback_cfg_t c = cfg(0, 3, false);
    fb_state_t s;
    fb_init(&s, 0);
    fb_on_value(&s, 0);
    assert(fb_on_poll_result(&s, &c, false, 10) == FB_CHANGE_NONE);
    assert(fb_on_poll_result(&s, &c, false, 20) == FB_CHANGE_NONE);
    assert(fb_on_poll_result(&s, &c, false, 30) == FB_CHANGE_ENGAGED);
    assert(s.reason == FB_REASON_POLL_FAILURES);
    assert(fb_on_poll_result(&s, &c, false, 40) == FB_CHANGE_NONE);
    assert(fb_on_poll_result(&s, &c, true, 50) == FB_CHANGE_CLEARED);
    assert(!s.active);
    assert(s.poll_fail_count == 0);
}

static void test_poll_before_first_value(void)
{
    // NO_VALUE is upgraded by a poll-failure trigger.
    fallback_cfg_t c = cfg(0, 2, false);
    fb_state_t s;
    fb_init(&s, 0);
    assert(fb_on_poll_result(&s, &c, false, 5) == FB_CHANGE_NONE);
    assert(fb_on_poll_result(&s, &c, false, 10) == FB_CHANGE_ENGAGED);
    assert(s.reason == FB_REASON_POLL_FAILURES);
}

static void test_no_flap(void)
{
    fallback_cfg_t c = cfg(0, 3, false);
    fb_state_t s;
    fb_init(&s, 0);
    fb_on_value(&s, 0);
    fb_on_poll_result(&s, &c, false, 1);
    fb_on_poll_result(&s, &c, false, 2);
    assert(fb_on_poll_result(&s, &c, false, 3) == FB_CHANGE_ENGAGED);
    assert(fb_on_value(&s, 4) == FB_CHANGE_CLEARED);
    assert(fb_on_poll_result(&s, &c, false, 5) == FB_CHANGE_NONE);
    assert(fb_tick(&s, &c, false, 6) == FB_CHANGE_NONE);
    assert(fb_on_poll_result(&s, &c, false, 7) == FB_CHANGE_NONE);
    assert(fb_on_poll_result(&s, &c, false, 8) == FB_CHANGE_ENGAGED);
}

static void test_mqtt(void)
{
    fallback_cfg_t c = cfg(0, 0, true);
    fb_state_t s;
    fb_init(&s, 0);
    fb_on_value(&s, 0);
    assert(fb_on_mqtt(&s, false, 0) == FB_CHANGE_NONE);
    assert(fb_tick(&s, &c, true, 60) == FB_CHANGE_NONE);
    assert(fb_tick(&s, &c, true, 61) == FB_CHANGE_ENGAGED);
    assert(s.reason == FB_REASON_MQTT_LOST);
    assert(fb_on_value(&s, 70) == FB_CHANGE_CLEARED);
    assert(fb_tick(&s, &c, true, 200) == FB_CHANGE_NONE);
    assert(fb_on_mqtt(&s, true, 250) == FB_CHANGE_NONE);
    assert(fb_on_mqtt(&s, false, 300) == FB_CHANGE_NONE);
    assert(fb_tick(&s, &c, true, 361) == FB_CHANGE_ENGAGED);
}

static void test_mqtt_ignored(void)
{
    fallback_cfg_t off = cfg(0, 0, false);
    fallback_cfg_t on  = cfg(0, 0, true);
    fb_state_t s;
    fb_init(&s, 0);
    fb_on_value(&s, 0);
    fb_on_mqtt(&s, false, 0);
    assert(fb_tick(&s, &off, true, 1000) == FB_CHANGE_NONE);
    assert(fb_tick(&s, &on, false, 1000) == FB_CHANGE_NONE);
    assert(!s.active);
}

static void test_all_off(void)
{
    fallback_cfg_t c = cfg(0, 0, false);
    fb_state_t s;
    fb_init(&s, 0);
    assert(s.active && s.reason == FB_REASON_NO_VALUE);
    assert(fb_tick(&s, &c, true, 100000) == FB_CHANGE_NONE);
    assert(s.reason == FB_REASON_NO_VALUE);
    fb_on_value(&s, 100001);
    for (int i = 0; i < 10; i++)
        assert(fb_on_poll_result(&s, &c, false, 100002 + i) == FB_CHANGE_NONE);
    fb_on_mqtt(&s, false, 100100);
    assert(fb_tick(&s, &c, true, 900000) == FB_CHANGE_NONE);
    assert(!s.active);
}

static void test_notifying_reason_sticks(void)
{
    // Already active with a notifying reason: another trigger changes nothing.
    fallback_cfg_t c = cfg(10, 3, true);
    fb_state_t s;
    fb_init(&s, 0);
    fb_on_value(&s, 0);
    assert(fb_tick(&s, &c, true, 600) == FB_CHANGE_ENGAGED);
    assert(s.reason == FB_REASON_TIMEOUT);
    fb_on_poll_result(&s, &c, false, 610);
    fb_on_poll_result(&s, &c, false, 620);
    assert(fb_on_poll_result(&s, &c, false, 630) == FB_CHANGE_NONE);
    assert(s.reason == FB_REASON_TIMEOUT);
    fb_on_mqtt(&s, false, 640);
    assert(fb_tick(&s, &c, true, 701) == FB_CHANGE_NONE);
    assert(s.reason == FB_REASON_TIMEOUT);
}

static void test_strings(void)
{
    fb_mode_t m;
    assert(!strcmp(fb_reason_str(FB_REASON_NO_VALUE), "no_value"));
    assert(!strcmp(fb_reason_str(FB_REASON_TIMEOUT), "timeout"));
    assert(!strcmp(fb_reason_str(FB_REASON_POLL_FAILURES), "poll_failures"));
    assert(!strcmp(fb_reason_str(FB_REASON_MQTT_LOST), "mqtt_lost"));
    assert(!strcmp(fb_mode_str(FB_MODE_TEXT), "text"));
    assert(!strcmp(fb_mode_str(FB_MODE_MARKER), "marker"));
    assert(!strcmp(fb_mode_str(FB_MODE_DIM), "dim"));
    assert(fb_mode_from_str("dim", &m) && m == FB_MODE_DIM);
    assert(fb_mode_from_str("marker", &m) && m == FB_MODE_MARKER);
    assert(fb_mode_from_str("text", &m) && m == FB_MODE_TEXT);
    assert(!fb_mode_from_str("bogus", &m));
    assert(!fb_mode_from_str(NULL, &m));
    assert(!fb_notifies(FB_REASON_NONE));
    assert(!fb_notifies(FB_REASON_NO_VALUE));
    assert(fb_notifies(FB_REASON_MQTT_LOST));
}

static void test_defaults(void)
{
    fallback_cfg_t c;
    memset(&c, 0xAA, sizeof(c));
    fb_cfg_defaults(&c);
    assert(c.text[0] == 0);
    assert(c.mode == FB_MODE_TEXT);
    assert(c.after_min == 0);
    assert(c.poll_failures == 3);
    assert(!c.on_mqtt_loss);
}

static void test_migration(void)
{
    fallback_cfg_t c;

    fb_migrate_legacy(&c, 5, 3, "stale", "waiting");
    assert(!strcmp(c.text, "waiting"));
    assert(c.mode == FB_MODE_TEXT && c.after_min == 300);
    assert(c.poll_failures == 3 && !c.on_mqtt_loss);

    fb_migrate_legacy(&c, 2, 1, "", "");
    assert(c.mode == FB_MODE_DIM && c.after_min == 120 && c.text[0] == 0);

    fb_migrate_legacy(&c, 0, 0, "s", "");
    assert(!strcmp(c.text, "s") && c.after_min == 0);

    fb_migrate_legacy(&c, 4, 0, "", "");
    assert(c.after_min == 0 && c.mode == FB_MODE_TEXT);

    fb_migrate_legacy(&c, 1, 2, "x", "");
    assert(c.mode == FB_MODE_MARKER && c.after_min == 60);

    fb_migrate_legacy(&c, 65535, 3, "x", "");
    assert(c.after_min == 43200);

    fb_migrate_legacy(&c, 1, 3, NULL, NULL);
    assert(c.text[0] == 0);

    // Overlong text is truncated to FB_TEXT_MAX.
    char big[200];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    fb_migrate_legacy(&c, 1, 3, big, "");
    assert(strlen(c.text) == FB_TEXT_MAX);
}

int main(void)
{
    test_boot();
    test_timeout();
    test_timeout_no_value_ever();
    test_poll_threshold();
    test_poll_before_first_value();
    test_no_flap();
    test_mqtt();
    test_mqtt_ignored();
    test_all_off();
    test_notifying_reason_sticks();
    test_strings();
    test_defaults();
    test_migration();
    printf("test_fallback: all passed\n");
    return 0;
}
