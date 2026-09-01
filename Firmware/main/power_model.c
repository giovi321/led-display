#include "power_model.h"

// 255 (brightness) * 255 (channel byte): the denominator that turns a raw
// channel-byte sum and a brightness into a fraction of full-scale current.
#define FULL_SCALE (255u * 255u)

void power_model_default(power_cfg_t *out)
{
    if (!out) return;
    out->budget_ma      = POWER_BUDGET_MA_DEFAULT;
    out->ua_per_channel = POWER_UA_PER_CHANNEL_DEFAULT;
    out->ua_quiescent   = POWER_UA_QUIESCENT_DEFAULT;
    out->limit_enabled  = false;
}

bool power_model_valid(const power_cfg_t *cfg)
{
    if (!cfg) return false;
    if (cfg->budget_ma > POWER_BUDGET_MA_MAX) return false;
    if (cfg->ua_per_channel < POWER_UA_PER_CHANNEL_MIN ||
        cfg->ua_per_channel > POWER_UA_PER_CHANNEL_MAX) return false;
    if (cfg->ua_quiescent > POWER_UA_QUIESCENT_MAX) return false;
    return true;
}

// Frame current in uA. 64-bit throughout: the largest chain the firmware
// accepts is 8192 LEDs, and 8192 * 765 * 255 * 20000 is about 3.2e13.
static uint64_t frame_ua(const power_cfg_t *cfg, uint32_t px,
                         uint32_t sum_raw, uint8_t eff)
{
    const uint64_t quiescent = (uint64_t)px * cfg->ua_quiescent;
    const uint64_t lit = (uint64_t)sum_raw * eff * cfg->ua_per_channel / FULL_SCALE;
    return quiescent + lit;
}

uint32_t power_model_frame_ma(const power_cfg_t *cfg, uint32_t px,
                              uint32_t sum_raw, uint8_t eff)
{
    if (!cfg) return 0;
    // Rounded up, so a frame reported as fitting the budget really does.
    return (uint32_t)((frame_ua(cfg, px, sum_raw, eff) + 999u) / 1000u);
}

uint32_t power_model_worst_ma(const power_cfg_t *cfg, uint32_t px,
                              uint8_t r, uint8_t g, uint8_t b, uint8_t eff)
{
    if (!cfg) return 0;
    // px * 765 is at most 6.3e6, so the sum itself still fits 32 bits.
    return power_model_frame_ma(cfg, px, px * ((uint32_t)r + g + b), eff);
}

uint8_t power_model_fit_brightness(const power_cfg_t *cfg, uint32_t px,
                                   uint32_t sum_raw, uint8_t eff)
{
    if (!cfg || cfg->budget_ma == 0) return eff;   // no budget, nothing to fit

    const uint64_t budget_ua = (uint64_t)cfg->budget_ma * 1000u;
    const uint64_t quiescent = (uint64_t)px * cfg->ua_quiescent;
    if (quiescent >= budget_ua) {
        // The chain draws this much with every channel off. No brightness
        // helps; the caller reports it rather than pretending otherwise.
        return 0;
    }
    if (sum_raw == 0 || cfg->ua_per_channel == 0) return eff;

    // Invert frame_ua() for eff. Integer division truncates, which lands the
    // answer at or under the budget rather than just over it.
    const uint64_t avail = budget_ua - quiescent;
    const uint64_t fit = avail * FULL_SCALE /
                         ((uint64_t)sum_raw * cfg->ua_per_channel);
    return (fit >= eff) ? eff : (uint8_t)fit;
}
