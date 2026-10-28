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

// 1000^unit for unit 0..4.
static double unit_divisor(int unit)
{
    double d = 1.0;
    for (int i = 0; i < unit; i++) d *= 1000.0;
    return d;
}

// Largest unit 0..4 with |v| >= 1000^unit; returns it and its divisor.
static int auto_unit(double v, double *divisor)
{
    double a = v < 0.0 ? -v : v;
    int n = 0;
    double p = 1.0;
    while (n < 4 && a >= p * 1000.0) {
        p *= 1000.0;
        n++;
    }
    *divisor = p;
    return n;
}

// Round half away from zero to `dec` decimals. printf alone rounds the exact
// binary value to nearest-even, so true ties like "2.5" with 0 decimals would
// print "2". Scale, adjust by +-0.5, truncate toward zero (the C cast). Only
// safe while the scaled value is an exact double integer; beyond that range
// ties cannot be represented anyway, so v is returned unchanged.
static double round_dec(double v, int dec)
{
    double scale = 1.0;
    for (int i = 0; i < dec; i++) scale *= 10.0;
    double scaled = v * scale;
    if (scaled > -9e15 && scaled < 9e15) {
        long long ll = (long long)(scaled + (scaled >= 0.0 ? 0.5 : -0.5));
        return (double)ll / scale;
    }
    return v;
}

// Split `src` on spaces into up to four tokens (k M B T); missing tokens
// fall back to the defaults. Tokens longer than the buffer are cut.
static void parse_letters(const char *src, char out[4][VALUE_LETTERS_MAX + 1])
{
    static const char *const defaults[4] = {"k", "M", "B", "T"};
    for (int i = 0; i < 4; i++) {
        while (*src == ' ') src++;
        size_t n = 0;
        while (src[n] && src[n] != ' ') n++;
        if (n > 0) {
            size_t keep = n > VALUE_LETTERS_MAX ? VALUE_LETTERS_MAX : n;
            memcpy(out[i], src, keep);
            out[i][keep] = '\0';
            src += n;
        } else {
            snprintf(out[i], VALUE_LETTERS_MAX + 1, "%s", defaults[i]);
        }
    }
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

    double mult = (cfg->multiplier == 0.0) ? 1.0 : cfg->multiplier;
    int unit = 0;           // 1..4 = k M B T, 0 = no letter
    double divisor = 1.0;
    if (cfg->scale == VALUE_SCALE_AUTO) {
        unit = auto_unit(v * mult, &divisor);
    } else if (cfg->scale >= VALUE_SCALE_K && cfg->scale <= VALUE_SCALE_B) {
        unit = (int)cfg->scale - 1;
        divisor = unit_divisor(unit);
    }

    double sv = 0.0;        // value that ends up printed (rounded if decimals >= 0)
    if (cfg->decimals >= 0) {
        int dec = cfg->decimals > DECIMALS_MAX ? DECIMALS_MAX : cfg->decimals;
        sv = round_dec(v * mult / divisor, dec);
        // Rounding can carry into the next unit (999.95k -> 1000.0k).
        if (cfg->scale == VALUE_SCALE_AUTO && unit < 4 &&
            (sv >= 1000.0 || sv <= -1000.0)) {
            unit++;
            divisor = unit_divisor(unit);
            sv = round_dec(v * mult / divisor, dec);
        }
        snprintf(num, sizeof(num), "%.*f", dec, sv);
    } else if (unit == 0 && mult == 1.0) {
        // decimals -1, nothing computed: keep the value text exactly as
        // sent (trimmed), so "4.20" stays "4.20". Numbers too long for the
        // work buffer fall back to passthrough rather than being cut
        // mid-digit.
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
        sv = v;
    } else {
        sv = v * mult / divisor;
        snprintf(num, sizeof(num), "%.15g", sv);
    }

    // Decimal separator: swap the first '.' for the configured character
    // before grouping (insert_thousands stops at the first non-digit).
    if (cfg->dec_sep == ',') {
        char *dot = strchr(num, '.');
        if (dot) *dot = ',';
    }

    // A space separator becomes the renderer's thin space (FONT_THIN_SPACE in
    // fonts.h): a full space cell makes grouped values noticeably wider than
    // the same value with a dot separator. Kept out of fonts.h on purpose -
    // this file must stay compilable standalone by the host tests.
    char sep = cfg->thousands == ' ' ? '\x1F' : cfg->thousands;
    insert_thousands(num, sizeof(num), sep);

    if (cfg->plus_sign && sv > 0.0 && num[0] != '+') {
        size_t n = strlen(num);
        if (n + 2 <= sizeof(num)) {
            memmove(num + 1, num, n + 1);
            num[0] = '+';
        }
    }

    char letters[4][VALUE_LETTERS_MAX + 1];
    parse_letters(cfg->scale_letters, letters);
    const char *letter = (unit >= 1 && unit <= 4) ? letters[unit - 1] : "";

    snprintf(out, outsz, "%s%s%s%s", cfg->prefix, num, letter, cfg->suffix);
}

bool valuefmt_cfg_valid(const value_cfg_t *cfg)
{
    if (!cfg || !cfg->thousands) return true;
    char dec = (cfg->dec_sep == ',') ? ',' : '.';
    return cfg->thousands != dec;
}
