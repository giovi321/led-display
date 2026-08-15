#pragma once

// IANA time zone names ("Europe/Zurich") resolved to the POSIX TZ rules that
// newlib's tzset() understands ("<+01>-1<+02>-2,M3.5.0,M10.5.0/3").
//
// The device carries no tzdata: the table in tzdata.c is generated from the
// IANA database by tools/gen_tz.js and holds one rule per zone, deduplicated
// (a few hundred zones share a few dozen rules). PURE C - no ESP-IDF - so the
// host tests can compile it with plain gcc.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;   // IANA zone name, e.g. "Europe/Zurich"
    uint16_t    rule;   // index into tz_rules
} tz_entry_t;

// Generated (tzdata.c), sorted by name so the lookup can binary search.
extern const tz_entry_t   tz_zones[];
extern const char *const  tz_rules[];
extern const size_t       tz_zone_count;
extern const size_t       tz_rule_count;

// POSIX rule for an IANA zone name; NULL when the name is not in the table.
const char *tz_posix_for_zone(const char *iana);

// What to hand tzset() for a stored setting. A known IANA name resolves to
// its rule; anything else is passed through unchanged, which keeps a raw
// POSIX string working - both for power users and for devices upgrading from
// the build that stored POSIX strings directly. NULL/empty returns NULL.
const char *tz_resolve(const char *stored);

#ifdef __cplusplus
}
#endif
