// IANA zone name -> POSIX TZ rule lookup (see tz.h). The table itself is
// generated into tzdata.c; this file is only the search over it.

#include "tz.h"

#include <string.h>

const char *tz_posix_for_zone(const char *iana)
{
    if (!iana || !*iana) return NULL;

    size_t lo = 0, hi = tz_zone_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(iana, tz_zones[mid].name);
        if (c == 0) {
            uint16_t r = tz_zones[mid].rule;
            return (r < tz_rule_count) ? tz_rules[r] : NULL;
        }
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

const char *tz_resolve(const char *stored)
{
    if (!stored || !*stored) return NULL;
    const char *posix = tz_posix_for_zone(stored);
    return posix ? posix : stored;
}
