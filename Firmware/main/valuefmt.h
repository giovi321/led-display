#pragma once

// Formatting for the generic value endpoint: raw value string -> display
// text via prefix/suffix/decimals/thousands-separator. PURE C (no ESP-IDF)
// so it is host-testable alongside mapping.c. The value_cfg_t struct lives
// here (not in integrations.h) for exactly that reason.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Staleness action when no value update arrived for stale_hours.
typedef enum {
    STALE_ACT_NONE   = 0,
    STALE_ACT_DIM    = 1,   // halve brightness until the next update
    STALE_ACT_MARKER = 2,   // blinking corner pixel
    STALE_ACT_TEXT   = 3,   // replace display with stale_text
} stale_action_t;

#define VALUE_AFFIX_MAX  16
#define VALUE_TEXT_MAX   64

typedef struct {
    char           prefix[VALUE_AFFIX_MAX + 1];
    char           suffix[VALUE_AFFIX_MAX + 1];
    int8_t         decimals;        // -1 = leave the value as sent
    char           thousands;       // separator char, 0 = none
    uint16_t       stale_hours;     // 0 = staleness check disabled
    stale_action_t stale_action;
    char           stale_text[VALUE_TEXT_MAX + 1];
    // Placeholder shown in integration mode while no value has arrived yet
    // (fresh boot, integration not wired up). Shown verbatim - no
    // prefix/suffix/formatting applied. Empty = blank panel.
    char           default_text[VALUE_TEXT_MAX + 1];
} value_cfg_t;

// Format `raw` (e.g. "1234567.891") into `out` applying cfg->decimals
// (round half away from zero; -1 keeps the value untouched), thousands
// separator and prefix/suffix. Non-numeric input is passed through with
// prefix/suffix only. Truncates safely to outsz.
void valuefmt_format(const value_cfg_t *cfg, const char *raw,
                     char *out, size_t outsz);

#ifdef __cplusplus
}
#endif
