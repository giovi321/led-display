// Host-side unit tests for valuefmt.c (plain gcc + assert, no ESP-IDF).
// Covers: decimals rounding (half away from zero, asserted on exact binary
// ties where printf's nearest-even would differ), 12-digit numbers,
// thousands grouping with/without decimals, negatives, non-numeric
// passthrough, empty input and truncation into a tiny buffer.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "valuefmt.h"

static int s_failures;

static value_cfg_t mkcfg(const char *prefix, const char *suffix,
                         int decimals, char thousands)
{
    value_cfg_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.prefix, sizeof(c.prefix), "%s", prefix);
    snprintf(c.suffix, sizeof(c.suffix), "%s", suffix);
    c.decimals  = (int8_t)decimals;
    c.thousands = thousands;
    return c;
}

static void check(const value_cfg_t *cfg, const char *raw,
                  const char *expect, const char *what)
{
    char out[128];
    valuefmt_format(cfg, raw, out, sizeof(out));
    if (strcmp(out, expect) != 0) {
        fprintf(stderr, "FAIL %s: raw='%s' expected='%s' got='%s'\n",
                what, raw, expect, out);
        s_failures++;
    }
}

static void test_decimals_rounding(void)
{
    value_cfg_t c0 = mkcfg("", "", 0, 0);
    check(&c0, "2.4",  "2",  "round down");
    check(&c0, "2.6",  "3",  "round up");
    check(&c0, "42",   "42", "integer with decimals=0");
    check(&c0, "42.0", "42", "x.0 with decimals=0");

    // Exact binary ties: nearest-even would give 0, 2, 2, -2. The contract
    // is half AWAY from zero.
    check(&c0, "0.5",  "1",  "tie 0.5 -> 1");
    check(&c0, "1.5",  "2",  "tie 1.5 -> 2");
    check(&c0, "2.5",  "3",  "tie 2.5 -> 3");
    check(&c0, "-2.5", "-3", "tie -2.5 -> -3");

    value_cfg_t c1 = mkcfg("", "", 1, 0);
    check(&c1, "1.25",  "1.3",  "tie 1.25 -> 1.3");
    check(&c1, "-1.25", "-1.3", "tie -1.25 -> -1.3");
    check(&c1, "1.24",  "1.2",  "1.24 -> 1.2");

    value_cfg_t c2 = mkcfg("", "", 2, 0);
    check(&c2, "0.125",  "0.13",   "tie 0.125 -> 0.13");
    check(&c2, "-0.125", "-0.13",  "tie -0.125 -> -0.13");
    check(&c2, "4.2",    "4.20",   "padding to 2 decimals");
    check(&c2, "4.239",  "4.24",   "rounding to 2 decimals");
}

static void test_decimals_keep_raw(void)
{
    value_cfg_t c = mkcfg("", "", -1, 0);
    check(&c, "4.20",   "4.20",   "decimals=-1 keeps trailing zero");
    check(&c, "4.2",    "4.2",    "decimals=-1 keeps short form");
    check(&c, " 42 ",   "42",     "decimals=-1 trims whitespace");
    check(&c, "-0.5",   "-0.5",   "decimals=-1 keeps negatives");
}

static void test_big_numbers(void)
{
    // 12-digit values must survive the double round-trip exactly.
    value_cfg_t plain = mkcfg("", "", -1, 0);
    check(&plain, "123456789012", "123456789012", "12 digits, no grouping");

    value_cfg_t grp = mkcfg("", "", -1, ',');
    check(&grp, "123456789012", "123,456,789,012", "12 digits grouped");
    check(&grp, "999999999999", "999,999,999,999", "12 nines grouped");

    value_cfg_t grp2 = mkcfg("", "", 2, ',');
    check(&grp2, "123456789012", "123,456,789,012.00",
          "12 digits grouped + 2 decimals");
}

static void test_thousands(void)
{
    value_cfg_t c = mkcfg("", "", -1, ',');
    check(&c, "1",        "1",         "1 digit: no separator");
    check(&c, "123",      "123",       "3 digits: no separator");
    check(&c, "1234",     "1,234",     "4 digits");
    check(&c, "123456",   "123,456",   "6 digits");
    check(&c, "1234567",  "1,234,567", "7 digits");
    check(&c, "1234.5",   "1,234.5",   "integer part only (raw decimals)");

    value_cfg_t cd = mkcfg("", "", 2, ',');
    check(&cd, "1234567.891", "1,234,567.89", "grouping + rounding");
    check(&cd, "1234567.895", "1,234,567.90", "grouping + round up");

    value_cfg_t apo = mkcfg("", " EUR", 0, '\'');
    check(&apo, "12345.6", "12'346 EUR", "apostrophe separator + suffix");
}

static void test_negatives(void)
{
    value_cfg_t c = mkcfg("", "", -1, ',');
    check(&c, "-1234",        "-1,234",         "negative grouped");
    check(&c, "-123",         "-123",           "negative, no separator");
    check(&c, "-1234567.891", "-1,234,567.891", "negative with raw decimals");

    value_cfg_t cd = mkcfg("", "", 2, ',');
    check(&cd, "-1234567.891", "-1,234,567.89", "negative grouped + rounded");
}

static void test_passthrough(void)
{
    value_cfg_t c = mkcfg("P:", ":S", 2, ',');
    check(&c, "n/a",   "P:n/a:S",   "non-numeric passthrough");
    check(&c, "12abc", "P:12abc:S", "partial number passes through whole");
    check(&c, "",      "P::S",      "empty raw: prefix+suffix only");
    check(&c, "  ",    "P:  :S",    "whitespace-only passes through");

    valuefmt_format(&c, NULL, (char[8]){0}, 8);   // must not crash

    value_cfg_t bare = mkcfg("", "", -1, 0);
    check(&bare, "n/a", "n/a", "passthrough without affixes");
}

static void test_prefix_suffix(void)
{
    value_cfg_t c = mkcfg("$", "/mo", 2, 0);
    check(&c, "42", "$42.00/mo", "prefix + decimals + suffix");

    value_cfg_t ws = mkcfg("", " kg", 0, 0);
    check(&ws, " 42 ", "42 kg", "whitespace-wrapped number still numeric");
}

static void test_truncation(void)
{
    // Format into an 8-byte buffer: output must be cut to 7 chars + NUL and
    // the bytes beyond the buffer must stay untouched.
    char guard[16];
    memset(guard, 'Z', sizeof(guard));

    value_cfg_t c = mkcfg("", "", 2, 0);
    valuefmt_format(&c, "1234567.891", guard, 8);
    assert(strcmp(guard, "1234567") == 0);
    assert(strlen(guard) == 7);
    for (size_t i = 8; i < sizeof(guard); i++) assert(guard[i] == 'Z');

    memset(guard, 'Z', sizeof(guard));
    value_cfg_t p = mkcfg("value ", "", -1, 0);
    valuefmt_format(&p, "12345", guard, 8);
    assert(strcmp(guard, "value 1") == 0);
    for (size_t i = 8; i < sizeof(guard); i++) assert(guard[i] == 'Z');

    // Degenerate sizes must be safe no-ops / empty strings.
    memset(guard, 'Z', sizeof(guard));
    valuefmt_format(&c, "42", guard, 1);
    assert(guard[0] == '\0' && guard[1] == 'Z');
    valuefmt_format(&c, "42", NULL, 0);   // must not crash
}

int main(void)
{
    test_decimals_rounding();
    test_decimals_keep_raw();
    test_big_numbers();
    test_thousands();
    test_negatives();
    test_passthrough();
    test_prefix_suffix();
    test_truncation();

    if (s_failures) {
        fprintf(stderr, "test_valuefmt: %d FAILURE(S)\n", s_failures);
        return 1;
    }
    printf("test_valuefmt: all tests passed\n");
    return 0;
}
