// Formatting for the generic value endpoint (see valuefmt.h). PURE C - no
// ESP-IDF, no FreeRTOS - so the host tests under Firmware/test/host/ can
// compile it with plain gcc. Deliberately avoids <math.h> too, so the host
// test link needs no -lm.

#include "valuefmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Largest number text we ever produce: 15 integer digits + sign + point +
// 9 decimals + thousands separators still fits with headroom.
#define NUMBUF_MAX 48

// %.*f is only meaningful while the scaled value stays an exactly
// representable double integer; beyond that decimals are noise anyway.
#define DECIMALS_MAX 9

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

// Insert `sep` every three digits of the integer part of `num`, walking
// left from the decimal point (or the end). A leading sign is skipped and
// the fraction/exponent tail is copied through untouched. If the grouped
// text would not fit the buffer the number is left ungrouped - dropping
// separators is safer than dropping digits.
static void insert_thousands(char *num, size_t bufsz, char sep)
{
    if (!sep) return;

    size_t len   = strlen(num);
    size_t begin = (num[0] == '-' || num[0] == '+') ? 1 : 0;
    size_t int_end = begin;
    while (int_end < len && num[int_end] >= '0' && num[int_end] <= '9') {
        int_end++;
    }
    size_t digits = int_end - begin;
    if (digits <= 3) return;

    size_t seps = (digits - 1) / 3;
    if (len + seps + 1 > bufsz || len + seps + 1 > NUMBUF_MAX) return;

    char tmp[NUMBUF_MAX];
    size_t o = 0;
    for (size_t i = 0; i < begin; i++) tmp[o++] = num[i];
    for (size_t i = 0; i < digits; i++) {
        // A separator goes before this digit whenever the count of digits
        // still to come (including this one) is a multiple of three.
        if (i > 0 && (digits - i) % 3 == 0) tmp[o++] = sep;
        tmp[o++] = num[begin + i];
    }
    for (size_t i = int_end; i < len; i++) tmp[o++] = num[i];
    tmp[o] = '\0';
    memcpy(num, tmp, o + 1);
}

void valuefmt_format(const value_cfg_t *cfg, const char *raw,
                     char *out, size_t outsz)
{
    static const value_cfg_t no_cfg = {.decimals = -1};

    if (!out || outsz == 0) return;
    if (!cfg) cfg = &no_cfg;
    if (!raw) raw = "";

    // Numeric only when the WHOLE string (minus surrounding whitespace)
    // parses; "12abc" must pass through untouched rather than show "12".
    // Check for "no conversion" (end == raw) BEFORE skipping trailing
    // whitespace, or a blank string would look numeric.
    char *end = NULL;
    double v = strtod(raw, &end);
    int numeric = (end != NULL) && (end != raw);
    if (numeric) {
        while (*end && is_space(*end)) end++;
        numeric = (*end == '\0');
    }

    char num[NUMBUF_MAX];

    if (!numeric) {
        // Passthrough: prefix + raw text + suffix, nothing else applied.
        snprintf(out, outsz, "%s%s%s", cfg->prefix, raw, cfg->suffix);
        return;
    }

    if (cfg->decimals >= 0) {
        int dec = cfg->decimals > DECIMALS_MAX ? DECIMALS_MAX : cfg->decimals;

        // Round half away from zero explicitly: printf alone rounds the
        // exact binary value to nearest-even, so true ties like "2.5" with
        // 0 decimals would print "2". Scale, adjust by +-0.5, truncate
        // toward zero (the C cast), then let %.*f re-derive the digits.
        // Only safe while the scaled value is an exact double integer;
        // beyond that range ties cannot be represented anyway.
        double scale = 1.0;
        for (int i = 0; i < dec; i++) scale *= 10.0;
        double scaled = v * scale;
        if (scaled > -9e15 && scaled < 9e15) {
            long long ll = (long long)(scaled + (scaled >= 0.0 ? 0.5 : -0.5));
            v = (double)ll / scale;
        }
        snprintf(num, sizeof(num), "%.*f", dec, v);
    } else {
        // decimals -1: keep the value text exactly as sent (trimmed), so
        // "4.20" stays "4.20". Numbers too long for the work buffer fall
        // back to passthrough rather than being cut mid-digit.
        const char *s = raw;
        while (*s && is_space(*s)) s++;
        size_t n = strlen(s);
        while (n > 0 && is_space(s[n - 1])) n--;
        if (n >= sizeof(num)) {
            snprintf(out, outsz, "%s%s%s", cfg->prefix, raw, cfg->suffix);
            return;
        }
        memcpy(num, s, n);
        num[n] = '\0';
    }

    insert_thousands(num, sizeof(num), cfg->thousands);

    snprintf(out, outsz, "%s%s%s", cfg->prefix, num, cfg->suffix);
}
