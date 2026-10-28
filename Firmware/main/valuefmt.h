#pragma once

// Formatting for the generic value endpoint: raw value string -> display
// text via prefix/suffix/decimals/thousands-separator. PURE C (no ESP-IDF)
// so it is host-testable alongside mapping.c. The value_cfg_t struct lives
// here (not in integrations.h) for exactly that reason.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fallback.h"

#ifdef __cplusplus
extern "C" {
#endif

// Number scaling: the value is divided by 1000^n and a letter appended.
// AUTO picks the largest of k/M/B/T with |v| >= 1000^n.
typedef enum {
    VALUE_SCALE_NONE = 0,
    VALUE_SCALE_AUTO = 1,
    VALUE_SCALE_K    = 2,
    VALUE_SCALE_M    = 3,
    VALUE_SCALE_B    = 4,
} value_scale_t;

#define VALUE_LETTERS_MAX 23
#define VALUE_AFFIX_MAX  16
#define VALUE_TEXT_MAX   64

typedef struct {
    char           prefix[VALUE_AFFIX_MAX + 1];
    char           suffix[VALUE_AFFIX_MAX + 1];
    int8_t         decimals;        // -1 = leave the value as sent
    char           thousands;       // separator char, 0 = none
    double         multiplier;      // 0 is treated as 1
    value_scale_t  scale;
    char           scale_letters[VALUE_LETTERS_MAX + 1];  // "" = "k M B T"
    char           dec_sep;         // 0 or '.' = dot, ',' = comma
    bool           plus_sign;       // "+" on positive numbers
    fallback_cfg_t fb;              // fallback text/mode and triggers
} value_cfg_t;

// Format `raw` (e.g. "1234567.891") into `out`. Numeric pipeline: multiply,
// scale, round to cfg->decimals (half away from zero), thousands and decimal
// separators, plus sign, scale letter, prefix/suffix. decimals -1 keeps the
// sent text when there is no scale and the multiplier is 1; otherwise it
// prints the computed value with %.15g. Non-numeric input is passed through
// with prefix/suffix only. Truncates safely to outsz.
void valuefmt_format(const value_cfg_t *cfg, const char *raw,
                     char *out, size_t outsz);

// False when the thousands and decimal separators are the same character.
bool valuefmt_cfg_valid(const value_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
