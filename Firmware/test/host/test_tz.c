// Host tests for the IANA zone -> POSIX rule table (tz.c + tzdata.c).
// Checks the table's invariants (sorted, in-range rule indices) rather than
// every zone, plus the lookup behaviour callers depend on.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "tz.h"

static void test_table_shape(void)
{
    assert(tz_zone_count > 100);   // a real table, not a stub
    assert(tz_rule_count > 0);

    for (size_t i = 0; i < tz_zone_count; i++) {
        assert(tz_zones[i].name != NULL);
        assert(tz_zones[i].name[0] != '\0');
        // The lookup binary searches, so the names must be sorted and unique.
        if (i > 0) {
            assert(strcmp(tz_zones[i - 1].name, tz_zones[i].name) < 0);
        }
        assert(tz_zones[i].rule < tz_rule_count);
    }
    for (size_t i = 0; i < tz_rule_count; i++) {
        assert(tz_rules[i] != NULL);
        assert(tz_rules[i][0] != '\0');
    }
    printf("test_table_shape ok (%u zones, %u rules)\n",
           (unsigned)tz_zone_count, (unsigned)tz_rule_count);
}

static void test_lookup(void)
{
    // Every zone in the table must be findable, and give back a rule.
    for (size_t i = 0; i < tz_zone_count; i++) {
        const char *p = tz_posix_for_zone(tz_zones[i].name);
        assert(p != NULL && p[0] != '\0');
    }

    assert(tz_posix_for_zone("Europe/Zurich") != NULL);
    assert(tz_posix_for_zone("America/New_York") != NULL);
    assert(tz_posix_for_zone("Asia/Tokyo") != NULL);

    // Unknown names, and the edge cases around them.
    assert(tz_posix_for_zone("Middle_Earth/Shire") == NULL);
    assert(tz_posix_for_zone("") == NULL);
    assert(tz_posix_for_zone(NULL) == NULL);
    // Case matters: the IANA names are the canonical spelling.
    assert(tz_posix_for_zone("europe/zurich") == NULL);
    printf("test_lookup ok\n");
}

static void test_rules_are_posix(void)
{
    // A northern-hemisphere DST zone: standard offset, daylight offset, and
    // both transition rules.
    const char *zurich = tz_posix_for_zone("Europe/Zurich");
    assert(strchr(zurich, ',') != NULL);
    assert(strstr(zurich, ",M") != NULL);

    // Southern hemisphere: DST starts later in the year than it ends, which
    // POSIX expresses by the start month being the larger one.
    const char *sydney = tz_posix_for_zone("Australia/Sydney");
    assert(strstr(sydney, ",M") != NULL);

    // A zone with no daylight saving carries no rule pair at all.
    const char *tokyo = tz_posix_for_zone("Asia/Tokyo");
    assert(strchr(tokyo, ',') == NULL);
    printf("test_rules_are_posix ok\n");
}

static void test_resolve(void)
{
    // A known zone resolves to its rule...
    const char *r = tz_resolve("Europe/Zurich");
    assert(r != NULL && strcmp(r, "Europe/Zurich") != 0);
    assert(strcmp(r, tz_posix_for_zone("Europe/Zurich")) == 0);

    // ...anything else is handed through untouched, which is what keeps a
    // raw POSIX rule (and a config written by the build that stored those)
    // working.
    const char *raw = "CET-1CEST,M3.5.0,M10.5.0/3";
    assert(strcmp(tz_resolve(raw), raw) == 0);

    // Nothing stored means no override at all.
    assert(tz_resolve("") == NULL);
    assert(tz_resolve(NULL) == NULL);
    printf("test_resolve ok\n");
}

int main(void)
{
    test_table_shape();
    test_lookup();
    test_rules_are_posix();
    test_resolve();
    printf("tz: all tests passed\n");
    return 0;
}
