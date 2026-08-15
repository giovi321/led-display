#include "led_driver.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"

static const char *TAG = "led_driver";

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} pixel_t;

static led_strip_handle_t s_strip;
static mapping_config_t   s_cfg;        // active mapping (copy)
static pixel_t           *s_frame;      // heap, s_pixels entries
static uint16_t          *s_lut;        // heap, logical (x,y) -> strip index
static int                s_width;      // cached mapping_width(&s_cfg)
static int                s_height;     // cached mapping_height(&s_cfg)
static int                s_pixels;     // cached mapping_pixels(&s_cfg)
static uint8_t            s_brightness     = LED_DEFAULT_BRIGHTNESS;
static uint8_t            s_brightness_cap = 255;   // 255 = uncapped
static SemaphoreHandle_t  s_lock;

// Overlay pixel, applied at refresh time and never stored in s_frame.
static bool               s_marker_on;
static int                s_marker_x, s_marker_y;
static pixel_t            s_marker_col;
static uint8_t            s_marker_bri = 255;

static inline uint8_t scale(uint8_t v)
{
    // Effective brightness is the user setting clamped by the cap, so a
    // power-budget limit configured once survives careless API calls.
    uint8_t eff = (s_brightness < s_brightness_cap) ? s_brightness
                                                    : s_brightness_cap;
    return (uint8_t)(((uint16_t)v * eff) / 255);
}

// Create the RMT strip device for `pixels` LEDs. RMT parameters are kept
// exactly as v1 (10 MHz ticks, 64 mem block symbols, no DMA): they are
// timing-proven on the hardware and independent of the chain length.
static esp_err_t strip_create(int pixels)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num   = LED_DATA_GPIO,
        .max_leds         = pixels,
        .led_model        = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  // 10 MHz -> 100 ns ticks
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        s_strip = NULL;
    }
    return err;
}

esp_err_t led_driver_init(const mapping_config_t *cfg)
{
    mapping_config_t fallback;
    if (!cfg || mapping_validate(cfg) != MAPPING_OK) {
        // A corrupt or missing layout must never brick the boot: fall back
        // to the factory default so the user can still reach the web UI and
        // fix the arrangement from there.
        ESP_LOGW(TAG, "invalid or missing mapping config, using default");
        mapping_default(&fallback);
        cfg = &fallback;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    s_cfg    = *cfg;
    s_width  = mapping_width(&s_cfg);
    s_height = mapping_height(&s_cfg);
    s_pixels = mapping_pixels(&s_cfg);

    s_frame = calloc((size_t)s_pixels, sizeof(pixel_t));
    s_lut   = malloc((size_t)s_pixels * sizeof(uint16_t));
    if (!s_frame || !s_lut) {
        free(s_frame);
        free(s_lut);
        s_frame = NULL;
        s_lut   = NULL;
        return ESP_ERR_NO_MEM;
    }
    // Build the LUT once; set_pixel is then a pure table lookup with zero
    // per-frame mapping cost. Cannot fail: the config was validated above.
    mapping_build_lut(&s_cfg, s_lut);

    esp_err_t err = strip_create(s_pixels);
    if (err != ESP_OK) {
        return err;
    }

    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);

    ESP_LOGI(TAG, "led_driver ready: %dx%d (%d px, %d panels of %dx%d) on GPIO %d, "
                  "brightness=%d cap=%d",
             s_width, s_height, s_pixels, mapping_panels(&s_cfg),
             s_cfg.panel.w, s_cfg.panel.h, LED_DATA_GPIO,
             s_brightness, s_brightness_cap);
    return ESP_OK;
}

esp_err_t led_driver_reconfigure(const mapping_config_t *cfg)
{
    // Validate before touching anything so a bad config from the API leaves
    // the running display completely untouched.
    if (!cfg || mapping_validate(cfg) != MAPPING_OK) {
        ESP_LOGW(TAG, "reconfigure rejected: invalid mapping config");
        return ESP_ERR_INVALID_ARG;
    }

    const int pixels = mapping_pixels(cfg);

    led_driver_lock();

    // Allocate fresh buffers and swap only after every fallible step has
    // succeeded. realloc would be cheaper but can shrink the old blocks in
    // place, which corrupts the heap if a later step fails and we bail out
    // with the old (larger) geometry still active.
    pixel_t  *frame = malloc((size_t)pixels * sizeof(pixel_t));
    uint16_t *lut   = malloc((size_t)pixels * sizeof(uint16_t));
    if (!frame || !lut) {
        free(frame);
        free(lut);
        led_driver_unlock();
        return ESP_ERR_NO_MEM;
    }

    // Swap the RMT device for one sized to the new chain length.
    if (s_strip) {
        led_strip_del(s_strip);
        s_strip = NULL;
    }
    esp_err_t err = strip_create(pixels);
    if (err != ESP_OK) {
        // Best effort: put a strip of the old length back so the driver
        // stays usable; the mapping and old buffers are unchanged either way.
        strip_create(s_pixels);
        free(frame);
        free(lut);
        led_driver_unlock();
        return err;
    }

    free(s_frame);
    free(s_lut);
    s_frame = frame;
    s_lut   = lut;

    s_cfg    = *cfg;
    s_width  = mapping_width(&s_cfg);
    s_height = mapping_height(&s_cfg);
    s_pixels = pixels;
    mapping_build_lut(&s_cfg, s_lut);  // cannot fail: validated above

    // Start from black: the old frame content is meaningless under the new
    // geometry, and stale pixels past the new chain end would never refresh.
    memset(s_frame, 0, (size_t)s_pixels * sizeof(pixel_t));
    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);

    led_driver_unlock();

    ESP_LOGI(TAG, "reconfigured: %dx%d (%d px, %d panels of %dx%d)",
             s_width, s_height, s_pixels, mapping_panels(&s_cfg),
             s_cfg.panel.w, s_cfg.panel.h);
    return ESP_OK;
}

void led_driver_lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
void led_driver_unlock(void) { xSemaphoreGive(s_lock); }

// Plain struct copy, no lock: callers (test patterns, identify) often hold
// the driver lock already and the mutex is non-recursive. Reconfigure is a
// rare, user-triggered event; a torn read here would only mis-colour one
// identify frame.
void led_driver_get_config(mapping_config_t *out)
{
    if (out) {
        *out = s_cfg;
    }
}

int led_driver_width(void)  { return s_width;  }
int led_driver_height(void) { return s_height; }
int led_driver_pixels(void) { return s_pixels; }

void led_driver_set_brightness(uint8_t brightness) { s_brightness = brightness; }
uint8_t led_driver_get_brightness(void)            { return s_brightness; }

void led_driver_set_brightness_cap(uint8_t cap) { s_brightness_cap = cap; }
uint8_t led_driver_get_brightness_cap(void)     { return s_brightness_cap; }

void led_driver_clear(void)
{
    memset(s_frame, 0, (size_t)s_pixels * sizeof(pixel_t));
}


void led_driver_fill(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < s_pixels; i++) {
        s_frame[i].r = r;
        s_frame[i].g = g;
        s_frame[i].b = b;
    }
}

void led_driver_set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || x >= s_width || y < 0 || y >= s_height) {
        return;
    }
    pixel_t *p = &s_frame[s_lut[y * s_width + x]];
    p->r = r;
    p->g = g;
    p->b = b;
}

void led_driver_set_raw(int strip_index, uint8_t r, uint8_t g, uint8_t b)
{
    if (strip_index < 0 || strip_index >= s_pixels) {
        return;
    }
    s_frame[strip_index].r = r;
    s_frame[strip_index].g = g;
    s_frame[strip_index].b = b;
}

bool led_driver_get_pixel(int x, int y, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (x < 0 || x >= s_width || y < 0 || y >= s_height) {
        return false;
    }
    const pixel_t *p = &s_frame[s_lut[y * s_width + x]];
    if (r) *r = p->r;
    if (g) *g = p->g;
    if (b) *b = p->b;
    return true;
}

void led_driver_set_marker(bool on, int x, int y,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
    // Bounds are checked in refresh, not here: a layout change after this
    // call can invalidate a position that was good when it was set.
    s_marker_x   = x;
    s_marker_y   = y;
    s_marker_col = (pixel_t){r, g, b};
    s_marker_bri = brightness;
    s_marker_on  = on;
}

void led_driver_refresh(void)
{
    if (!s_strip) {
        // Only possible after a reconfigure whose strip re-creation failed;
        // dropping frames beats crashing until the user retries.
        return;
    }
    for (int i = 0; i < s_pixels; i++) {
        led_strip_set_pixel(s_strip, i,
                            scale(s_frame[i].r),
                            scale(s_frame[i].g),
                            scale(s_frame[i].b));
    }
    if (s_marker_on &&
        s_marker_x >= 0 && s_marker_x < s_width &&
        s_marker_y >= 0 && s_marker_y < s_height) {
        // Last write wins, so the marker sits over the content rather than
        // being blended into it, and at its own brightness rather than the
        // panel's.
        const uint16_t idx = s_lut[s_marker_y * s_width + s_marker_x];
        led_strip_set_pixel(s_strip, idx,
                            (uint8_t)(((uint16_t)s_marker_col.r * s_marker_bri) / 255),
                            (uint8_t)(((uint16_t)s_marker_col.g * s_marker_bri) / 255),
                            (uint8_t)(((uint16_t)s_marker_col.b * s_marker_bri) / 255));
    }
    led_strip_refresh(s_strip);
}
