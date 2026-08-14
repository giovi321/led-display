// Host tests for devname.c: the one device name that has to work as a network
// hostname, a WiFi SSID and a webhook label at the same time.
//   gcc -I../../main test_devname.c ../../main/devname.c && ./a.out

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "devname.h"

static void test_valid_names(void)
{
    assert(devname_valid("led-display"));
    assert(devname_valid("a"));
    assert(devname_valid("Office2"));
    assert(devname_valid("panel-01"));
    assert(devname_valid("0"));
    assert(devname_valid("abcdefghijklmnopqrstuvwx"));   // exactly 24
    printf("test_valid_names ok\n");
}

static void test_rejected_names(void)
{
    assert(!devname_valid(NULL));
    assert(!devname_valid(""));
    assert(!devname_valid("abcdefghijklmnopqrstuvwxy"));  // 25
    assert(!devname_valid("-lead"));
    assert(!devname_valid("trail-"));
    assert(!devname_valid("has space"));
    assert(!devname_valid("under_score"));
    assert(!devname_valid("dot.name"));
    assert(!devname_valid("emoji\xf0\x9f\x92\xa1"));
    printf("test_rejected_names ok\n");
}

static void test_clean_keeps_valid_names(void)
{
    char out[DEVNAME_MAX + 1];
    assert(devname_clean("panel-01", out, sizeof(out)));
    assert(strcmp(out, "panel-01") == 0);
    printf("test_clean_keeps_valid_names ok\n");
}

// The pre-2.2 webhook name allowed anything; migration has to turn it into
// something legal rather than dropping it.
static void test_clean_rewrites(void)
{
    char out[DEVNAME_MAX + 1];

    assert(devname_clean("Office Panel", out, sizeof(out)));
    assert(strcmp(out, "Office-Panel") == 0);

    assert(devname_clean("  spaced  out  ", out, sizeof(out)));
    assert(strcmp(out, "spaced-out") == 0);

    assert(devname_clean("dots.and_underscores", out, sizeof(out)));
    assert(strcmp(out, "dots-and-underscores") == 0);

    assert(devname_clean("--lead-and-trail--", out, sizeof(out)));
    assert(strcmp(out, "lead-and-trail") == 0);

    // Truncation still leaves a valid name (no trailing hyphen).
    assert(devname_clean("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaXX", out, sizeof(out)));
    assert(strlen(out) == DEVNAME_MAX);
    assert(devname_valid(out));

    assert(devname_clean("twelve chars twelve chars twelve", out, sizeof(out)));
    assert(devname_valid(out));
    printf("test_clean_rewrites ok\n");
}

static void test_clean_gives_up_on_nothing_usable(void)
{
    char out[DEVNAME_MAX + 1];
    assert(!devname_clean("", out, sizeof(out)));
    assert(out[0] == '\0');
    assert(!devname_clean("---", out, sizeof(out)));
    assert(out[0] == '\0');
    assert(!devname_clean("   ", out, sizeof(out)));
    assert(out[0] == '\0');
    assert(!devname_clean(NULL, out, sizeof(out)));
    assert(!devname_clean("ok", NULL, 8));
    assert(!devname_clean("ok", out, 0));
    printf("test_clean_gives_up_on_nothing_usable ok\n");
}

// A short buffer must never be overrun, and what fits must still be valid.
static void test_clean_respects_small_buffers(void)
{
    char small[5];
    assert(devname_clean("abcdefgh", small, sizeof(small)));
    assert(strcmp(small, "abcd") == 0);
    assert(devname_valid(small));

    char tiny[2];
    assert(devname_clean("xy", tiny, sizeof(tiny)));
    assert(strcmp(tiny, "x") == 0);
    printf("test_clean_respects_small_buffers ok\n");
}

// An unset name has to resolve to exactly what the firmware used before the
// name existed, so an untouched device keeps its hostname and setup-AP SSID.
static void test_defaults_for_unset(void)
{
    assert(strcmp(devname_hostname(NULL), "led-display") == 0);
    assert(strcmp(devname_hostname(""), "led-display") == 0);
    assert(strcmp(devname_ap_ssid(NULL), "LEDDisplay-Setup") == 0);
    assert(strcmp(devname_ap_ssid(""), "LEDDisplay-Setup") == 0);
    printf("test_defaults_for_unset ok\n");
}

// A set name is used verbatim in both roles: the setup network is "the device".
static void test_set_name_used_verbatim(void)
{
    assert(strcmp(devname_hostname("kitchen-panel"), "kitchen-panel") == 0);
    assert(strcmp(devname_ap_ssid("kitchen-panel"), "kitchen-panel") == 0);
    printf("test_set_name_used_verbatim ok\n");
}

// Whatever the lenient pass produces must pass the strict check, and must fit
// an SSID with room to spare.
static void test_clean_output_always_valid(void)
{
    static const char *inputs[] = {
        "Office Panel", "a b c", "x", "9lives", "-x-", "ALLCAPS NAME",
        "tab\there", "slash/es", "plus+plus", "0000000000000000000000000000",
    };
    char out[DEVNAME_MAX + 1];
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        if (!devname_clean(inputs[i], out, sizeof(out))) continue;
        assert(devname_valid(out));
        assert(strlen(out) <= 32 - 8);   // headroom under the SSID limit
    }
    printf("test_clean_output_always_valid ok\n");
}

// The pre-2.2 webhook label is adopted as the device name on the first boot
// after an update, but only when it is really a name: a value that cleans to
// the default means the device was never named, so it has to stay unset.
static void test_clean_can_produce_the_default(void)
{
    char out[DEVNAME_MAX + 1];
    static const char *same_as_default[] = {
        "led-display", "led display", "led_display", "  led-display  ",
        "led.display",
    };
    for (unsigned i = 0; i < sizeof(same_as_default) / sizeof(same_as_default[0]); i++) {
        assert(devname_clean(same_as_default[i], out, sizeof(out)));
        assert(strcmp(out, DEVNAME_DEFAULT_HOST) == 0);
    }
    assert(devname_clean("LED-display", out, sizeof(out)));
    assert(strcmp(out, DEVNAME_DEFAULT_HOST) != 0);   // case is preserved
    printf("test_clean_can_produce_the_default ok\n");
}

// Writes are rejected unless the Host/Origin names this device, so the check
// has to accept every form a browser sends for it.
static void test_host_matches_the_device_name(void)
{
    assert(devname_host_matches("led-display", "led-display"));
    assert(devname_host_matches("led-display:80", "led-display"));
    assert(devname_host_matches("led-display.local", "led-display"));
    assert(devname_host_matches("led-display.lan:8080", "led-display"));
    assert(devname_host_matches("LED-DISPLAY", "led-display"));   // DNS is case-insensitive
    assert(devname_host_matches("kitchen", "kitchen"));
    printf("test_host_matches_the_device_name ok\n");
}

// Anything else stays rejected: that is the DNS-rebinding guard.
static void test_host_rejects_other_names(void)
{
    assert(!devname_host_matches("led-display.evil.com", "led-display"));
    assert(!devname_host_matches("led-display.local.evil.com", "led-display"));
    assert(!devname_host_matches("notled-display", "led-display"));
    assert(!devname_host_matches("led-displays", "led-display"));
    assert(!devname_host_matches("led-display.", "led-display"));
    assert(!devname_host_matches("led-displa", "led-display"));
    assert(!devname_host_matches("", "led-display"));
    assert(!devname_host_matches("anything", ""));       // no name to match
    assert(!devname_host_matches(NULL, "led-display"));
    assert(!devname_host_matches("led-display", NULL));
    printf("test_host_rejects_other_names ok\n");
}

int main(void)
{
    test_valid_names();
    test_rejected_names();
    test_clean_keeps_valid_names();
    test_clean_rewrites();
    test_clean_gives_up_on_nothing_usable();
    test_clean_respects_small_buffers();
    test_defaults_for_unset();
    test_set_name_used_verbatim();
    test_clean_output_always_valid();
    test_clean_can_produce_the_default();
    test_host_matches_the_device_name();
    test_host_rejects_other_names();
    printf("devname: all tests passed\n");
    return 0;
}
