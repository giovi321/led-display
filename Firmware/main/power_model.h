#pragma once

#include <stdbool.h>
#include <stdint.h>

// Current estimation for a WS2812B chain.
//
// The draw of a WS2812B is linear in the byte values clocked out to it: a
// fixed quiescent current per LED for the controller inside it, plus a share
// of the full-scale channel current for each of R, G and B. That makes the
// whole frame's draw a function of one number - the sum of every channel byte
// in the frame - which is cheap enough to compute on every refresh:
//
//     I = px * I_quiescent + (sum_of_channel_bytes / 255) * I_channel_full
//
// The three constants are configurable rather than baked in, because panels
// sold as "WS2812B" vary and the only honest way to pin them down is to
// measure the chain you actually own. See the power budget page in the docs
// for the calibration procedure.
//
// Everything here is integer-only and free of ESP-IDF, so it is host-testable
// (Firmware/test/host/test_power_model.c).

// Defaults. The channel current is the datasheet nominal rather than a value
// fitted to a measurement: over-predicting dims the panel earlier than
// strictly necessary, under-predicting means the limiter fails to protect the
// supply, and only one of those two is a safe way to be wrong.
#define POWER_BUDGET_MA_DEFAULT       2600   // 3 A USB-C less the controller and margin
#define POWER_UA_PER_CHANNEL_DEFAULT 20000   // one channel at 255
#define POWER_UA_QUIESCENT_DEFAULT    1000   // per LED, everything black

// Accepted ranges for the configurable constants.
#define POWER_BUDGET_MA_MAX          25000   // 5 A at 5 V, the PD-trigger upgrade path
#define POWER_UA_PER_CHANNEL_MIN         1
#define POWER_UA_PER_CHANNEL_MAX     40000
#define POWER_UA_QUIESCENT_MAX        5000

typedef struct {
    uint16_t budget_ma;       // LED-only budget; 0 = no budget, nothing is limited
    uint16_t ua_per_channel;  // one channel at 255
    uint16_t ua_quiescent;    // per LED with every channel at 0
    bool     limit_enabled;   // enforce the budget, or only report against it
} power_cfg_t;

void power_model_default(power_cfg_t *out);

// True if every field is inside its accepted range. A budget of 0 is valid and
// means "no budget configured".
bool power_model_valid(const power_cfg_t *cfg);

// Estimated draw of a frame, in mA, rounded up.
//
// `px` is the chain length, `sum_raw` the sum of every channel byte in the
// frame buffer before brightness scaling, and `eff` the brightness the frame
// goes out at. Brightness scaling is uniform across the frame, so the scaled
// sum is derived from the raw one instead of needing a second pass; ignoring
// the per-pixel integer truncation makes the answer very slightly high, which
// is the safe direction.
uint32_t power_model_frame_ma(const power_cfg_t *cfg, uint32_t px,
                              uint32_t sum_raw, uint8_t eff);

// Estimated draw if every one of `px` LEDs were lit at (r, g, b) and `eff`.
// The headroom figure: content changes, and a frame that fits today can stop
// fitting when the text gets longer.
uint32_t power_model_worst_ma(const power_cfg_t *cfg, uint32_t px,
                              uint8_t r, uint8_t g, uint8_t b, uint8_t eff);

// The highest brightness at or below `eff` whose frame fits the budget.
//
// Returns `eff` unchanged when there is nothing to do (no budget configured,
// a black frame, or the frame already fits), and 0 when the quiescent draw of
// the chain alone exceeds the budget - at that point no brightness helps,
// because the LEDs draw that current with every channel off.
uint8_t power_model_fit_brightness(const power_cfg_t *cfg, uint32_t px,
                                   uint32_t sum_raw, uint8_t eff);
