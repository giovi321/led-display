#include "display.h"

#include <ctype.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "fonts.h"
#include "led_driver.h"

static const char *TAG = "display";

typedef struct {
    char text[DISPLAY_TEXT_MAX];
    display_color_t color;
    uint8_t scroll_mode;  // display_scroll_t
    uint8_t speed;        // 0..255, mapped to a frame interval
    uint8_t scale;        // 1..FONT_SCALE_MAX (integer font multiplier)
    uint8_t font;         // FONT_ID_*
    bool prop;            // proportional spacing (monospace when false)
} display_state_t;

// The stored font id survives layout changes even when it no longer fits;
// rendering falls back via font_fit so the user's choice comes back when a
// tall enough layout returns.
static const font_def_t *active_font(uint8_t font_id)
{
    return font_get(font_fit(font_id, led_driver_height()));
}

static uint8_t clamp_scale(const font_def_t *f, uint8_t s)
{
    if (s < FONT_SCALE_MIN) s = FONT_SCALE_MIN;
    if (s > FONT_SCALE_MAX) s = FONT_SCALE_MAX;
    // A glyph at scale N is f->h*N rows tall, so clamp to what actually
    // fits the panel - no point silently clipping the top/bottom.
    int fits = led_driver_height() / f->h;
    if (fits < 1) fits = 1;
    if (s > fits) s = (uint8_t)fits;
    return s;
}

// Pick a sensible default scale based on the active display height: 2x for
// the 64x16 grid, 1x for a single 32x8 panel, 3x if the display is taller.
// Deliberately conservative (max 3) - anything bigger is a user choice.
static uint8_t default_scale(const font_def_t *f)
{
    int h = led_driver_height();
    if (h >= f->h * 3) return 3;
    if (h >= f->h * 2) return 2;
    return 1;
}

static display_state_t s_state;
static SemaphoreHandle_t s_state_lock;
static TaskHandle_t s_task;
static TaskHandle_t s_test_task;
// Identify mode freezes the scroll task WITHOUT clearing the stored state
// (unlike the destructive test patterns): the text must survive so
// display_identify(false) can put it back.
static volatile bool s_identify;

// Bumped on every s_state mutation. The scroll task re-checks it under the
// driver lock so it never paints a frame copied from state that a concurrent
// display_show_text/display_clear has already replaced (the stale frame
// would stay on screen forever once the task goes idle).
static volatile uint32_t s_state_gen;

// Set by any content change to abort a walking test pattern early; the
// step_raw/step_xy walkers otherwise run for minutes on large grids.
static volatile bool s_test_cancel;

static int text_pixel_width_at(const font_def_t *f, const char *s,
                               bool prop, uint8_t scale)
{
    return font_text_width(f, s, prop, scale);
}

static void draw_char(const font_def_t *f, int x_origin, char c,
                      display_color_t col, uint8_t scale,
                      int col_start, int col_count)
{
    const int y_off = (led_driver_height() - f->h * scale) / 2;
    for (int col_i = 0; col_i < col_count; col_i++) {
        uint16_t bits = font_glyph_col(f, c, col_start + col_i);
        for (int row_i = 0; row_i < f->h; row_i++) {
            if (!(bits & (1u << row_i))) continue;
            // Each source bit becomes a `scale x scale` block of LEDs.
            int x0 = x_origin + col_i * scale;
            int y0 = y_off    + row_i * scale;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    led_driver_set_pixel(x0 + dx, y0 + dy,
                                         col.r, col.g, col.b);
                }
            }
        }
    }
}

// Draw one run of `text` with its leftmost column at `x_origin` (which may
// be negative or past the display width for scrolling). No clear/refresh:
// the marquee mode paints several runs into one frame. Clipping is implicit:
// `led_driver_set_pixel` ignores out-of-range coordinates.
static void draw_text_run(const font_def_t *f, int x_origin, const char *text,
                          display_color_t col, uint8_t scale, bool prop)
{
    const int disp_w = led_driver_width();
    int cursor = x_origin;
    for (const char *p = text; *p; p++) {
        int start, width;
        font_glyph_span(f, *p, prop, &start, &width);
        // Skip whole glyphs that fall entirely off either edge for speed.
        if (cursor + width * scale > 0 && cursor < disp_w) {
            draw_char(f, cursor, *p, col, scale, start, width);
        }
        cursor += (width + 1) * scale;
        if (cursor >= disp_w && x_origin >= 0) {
            break;
        }
    }
}

static void render_text_at(const font_def_t *f, int x_origin, const char *text,
                           display_color_t col, uint8_t scale, bool prop)
{
    led_driver_clear();
    draw_text_run(f, x_origin, text, col, scale, prop);
    led_driver_refresh();
}

static void render_static_locked(void)
{
    const font_def_t *f = active_font(s_state.font);
    int w = text_pixel_width_at(f, s_state.text, s_state.prop, s_state.scale);
    int x = 0;
    if (w < led_driver_width()) {
        x = (led_driver_width() - w) / 2;
    }
    render_text_at(f, x, s_state.text, s_state.color, s_state.scale,
                   s_state.prop);
}

static TickType_t speed_to_period(uint8_t speed)
{
    // 0 -> slow (~200 ms / frame), 255 -> fast (~15 ms / frame).
    int ms = 200 - (int)speed * (200 - 15) / 255;
    if (ms < 15) ms = 15;
    return pdMS_TO_TICKS(ms);
}

// Gap between marquee repetitions: two character cells of the active font,
// so it grows with the text instead of a fixed pixel count.
static int marquee_gap(const font_def_t *f, uint8_t scale)
{
    return 2 * (f->w + 1) * scale;
}

static void scroll_task(void *arg)
{
    (void)arg;
    int offset = led_driver_width();  // start off the right edge
    uint32_t seen_gen = s_state_gen;

    while (1) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        uint8_t mode = s_state.scroll_mode;
        char text[DISPLAY_TEXT_MAX];
        strncpy(text, s_state.text, sizeof(text));
        text[sizeof(text) - 1] = '\0';
        display_color_t col = s_state.color;
        TickType_t period = speed_to_period(s_state.speed);
        uint8_t scale = s_state.scale;
        uint8_t font = s_state.font;
        bool prop = s_state.prop;
        uint32_t gen = s_state_gen;
        xSemaphoreGive(s_state_lock);

        // New content enters from the right edge instead of picking up the
        // previous text's position mid-screen.
        if (gen != seen_gen) {
            seen_gen = gen;
            offset = led_driver_width();
        }

        if (s_identify || mode == DISPLAY_SCROLL_OFF || text[0] == '\0') {
            // Idle: just wait for the next state change.
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
            continue;
        }

        const font_def_t *f = active_font(font);
        int w = text_pixel_width_at(f, text, prop, scale);
        led_driver_lock();
        // Re-check under the driver lock: identify may have been switched on
        // or the state replaced (clear, new static text) after we copied it,
        // and one stale frame painted now would stay on screen forever once
        // this task goes idle on the next iteration.
        if (!s_identify && gen == s_state_gen) {
            if (mode == DISPLAY_SCROLL_CONTINUOUS) {
                // Repetitions follow the tail after a gap; paint every copy
                // that overlaps the frame so the display is never left empty.
                const int span = w + marquee_gap(f, scale);
                int x = offset;
                while (x + w > 0) x -= span;
                led_driver_clear();
                for (x += span; x < led_driver_width(); x += span) {
                    draw_text_run(f, x, text, col, scale, prop);
                }
                led_driver_refresh();
            } else {
                render_text_at(f, offset, text, col, scale, prop);
            }
        }
        led_driver_unlock();

        offset--;
        if (mode == DISPLAY_SCROLL_CONTINUOUS) {
            const int span = w + marquee_gap(f, scale);
            if (offset <= -span) {
                offset += span;
            }
        } else if (offset < -w) {
            offset = led_driver_width();
        }

        vTaskDelay(period);
    }
}

// ---- Calibration / test patterns ---------------------------------------
//
// Test patterns are run from a dedicated short-lived task so the HTTP request
// returns immediately. The scroll task is paused by clearing the text first.

static void run_step_raw(void)
{
    const int pixels = led_driver_pixels();
    for (int i = 0; i < pixels; i++) {
        if (s_test_cancel) return;   // minutes-long walk on big grids
        led_driver_lock();
        led_driver_clear();
        // Hue ramp so the user can read out direction at a glance.
        uint8_t r = (i * 7)  & 0xFF;
        uint8_t g = (i * 11) & 0xFF;
        uint8_t b = (i * 13) & 0xFF;
        led_driver_set_raw(i, r ? r : 64, g ? g : 32, b ? b : 32);
        led_driver_refresh();
        led_driver_unlock();
        vTaskDelay(pdMS_TO_TICKS(120));
    }
}

static void run_step_xy(void)
{
    const int w = led_driver_width();
    const int h = led_driver_height();
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (s_test_cancel) return;
            led_driver_lock();
            led_driver_clear();
            led_driver_set_pixel(x, y, 80, 0, 0);
            led_driver_refresh();
            led_driver_unlock();
            vTaskDelay(pdMS_TO_TICKS(80));
        }
    }
}

static void run_corners(void)
{
    const int w = led_driver_width();
    const int h = led_driver_height();
    led_driver_lock();
    led_driver_clear();
    led_driver_set_pixel(0,     0,     255, 0,   0);   // TL red
    led_driver_set_pixel(w - 1, 0,     0,   255, 0);   // TR green
    led_driver_set_pixel(0,     h - 1, 0,   0,   255); // BL blue
    led_driver_set_pixel(w - 1, h - 1, 255, 255, 0);   // BR yellow
    led_driver_refresh();
    led_driver_unlock();
}

static void draw_border_locked(display_color_t col)
{
    const int w = led_driver_width();
    const int h = led_driver_height();
    led_driver_clear();
    for (int x = 0; x < w; x++) {
        led_driver_set_pixel(x, 0,     col.r, col.g, col.b);
        led_driver_set_pixel(x, h - 1, col.r, col.g, col.b);
    }
    for (int y = 0; y < h; y++) {
        led_driver_set_pixel(0,     y, col.r, col.g, col.b);
        led_driver_set_pixel(w - 1, y, col.r, col.g, col.b);
    }
    led_driver_refresh();
}

static void run_border(void)
{
    led_driver_lock();
    draw_border_locked((display_color_t){80, 32, 0});
    led_driver_unlock();
}

void display_draw_border(display_color_t col)
{
    led_driver_lock();
    draw_border_locked(col);
    led_driver_unlock();
}

static void run_fill(void)
{
    led_driver_lock();
    led_driver_fill(80, 32, 0);
    led_driver_refresh();
    led_driver_unlock();
}

static void test_task(void *arg)
{
    display_test_t p = (display_test_t)(uintptr_t)arg;
    // Make sure the scroll task isn't going to overwrite us.
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_state.text[0] = '\0';
    s_state.scroll_mode = DISPLAY_SCROLL_OFF;
    s_state_gen++;
    xSemaphoreGive(s_state_lock);
    if (s_task) xTaskNotifyGive(s_task);

    s_test_cancel = false;
    switch (p) {
        case DISPLAY_TEST_STEP_RAW: run_step_raw(); break;
        case DISPLAY_TEST_STEP_XY:  run_step_xy();  break;
        case DISPLAY_TEST_CORNERS:  run_corners();  break;
        case DISPLAY_TEST_BORDER:   run_border();   break;
        case DISPLAY_TEST_FILL:     run_fill();     break;
    }

    s_test_task = NULL;
    vTaskDelete(NULL);
}

void display_run_test(display_test_t pattern)
{
    if (s_test_task) {
        // A previous walker is still running: tell it to stop. The caller
        // can re-send the request once it has exited (within one step).
        s_test_cancel = true;
        return;
    }
    xTaskCreatePinnedToCore(test_task, "display_test", 4096,
                            (void *)(uintptr_t)pattern, 5, &s_test_task, 1);
}

// ---- Identify mode ------------------------------------------------------

// Identify palette keyed by chain index: 8 distinct hues, repeated at half
// value for chains 8..15 and quarter value for 16 and up, so every panel on
// a large grid still reads as unique against its neighbours.
static display_color_t identify_color(uint8_t chain)
{
    static const display_color_t base[8] = {
        {120,   0,   0},  // 0 red
        {  0, 120,   0},  // 1 green
        {  0,   0, 120},  // 2 blue
        {120, 120,   0},  // 3 yellow
        {120,   0, 120},  // 4 magenta
        {  0, 120, 120},  // 5 cyan
        {120,  60,   0},  // 6 orange
        {120, 120, 120},  // 7 white
    };
    display_color_t c = base[chain % 8];
    int shift = chain / 8;
    if (shift > 2) shift = 2;
    c.r >>= shift;
    c.g >>= shift;
    c.b >>= shift;
    return c;
}

void display_identify(bool on)
{
    if (!on) {
        s_identify = false;
        // Restore whatever the user had configured, the same way the v1
        // map-change handler did: re-show the stored text (scale=0 keeps
        // the current scale).
        char tx[DISPLAY_TEXT_MAX];
        display_color_t col;
        uint8_t mode;
        uint8_t speed, scale;
        display_get_state(tx, sizeof(tx), &col, &mode, &speed, &scale);
        if (tx[0] != '\0') {
            display_show_text(tx, col, mode, speed, 0);
        } else {
            // No stored text: leave a blank display rather than stale
            // identify colours.
            led_driver_lock();
            led_driver_clear();
            led_driver_refresh();
            led_driver_unlock();
            if (s_task) xTaskNotifyGive(s_task);
        }
        return;
    }

    s_identify = true;
    s_test_cancel = true;   // identify colours must not race a walker
    if (s_task) xTaskNotifyGive(s_task);

    // Paint each panel's logical rectangle in its chain colour. The grid
    // geometry comes from the driver so the drawing always matches whatever
    // arrangement is active right now.
    mapping_config_t cfg;
    led_driver_get_config(&cfg);

    led_driver_lock();
    led_driver_clear();
    for (int row = 0; row < cfg.rows; row++) {
        for (int col = 0; col < cfg.cols; col++) {
            display_color_t c = identify_color(cfg.cells[row][col].chain);
            const int x0 = col * cfg.panel.w;
            const int y0 = row * cfg.panel.h;
            for (int dy = 0; dy < cfg.panel.h; dy++) {
                for (int dx = 0; dx < cfg.panel.w; dx++) {
                    led_driver_set_pixel(x0 + dx, y0 + dy, c.r, c.g, c.b);
                }
            }
        }
    }
    led_driver_refresh();
    led_driver_unlock();
}

// ------------------------------------------------------------------------

esp_err_t display_init(void)
{
    s_state_lock = xSemaphoreCreateMutex();
    if (!s_state_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_state.text[0] = '\0';
    // Stored amber chosen so that brightness=32/255 lands on the spec's
    // ~(10, 4, 0) displayed value. At brightness=255 the panel shows the
    // full (80, 32, 0).
    s_state.color       = (display_color_t){80, 32, 0};
    s_state.scroll_mode = DISPLAY_SCROLL_OFF;
    s_state.speed       = 50;
    s_state.font        = FONT_ID_5X7;
    s_state.prop        = true;
    s_state.scale       = default_scale(font_get(s_state.font));

    BaseType_t ok = xTaskCreatePinnedToCore(scroll_task, "display_scroll",
                                            4096, NULL, 5, &s_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start scroll task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "display ready (%dx%d, default scale %u)",
             led_driver_width(), led_driver_height(), (unsigned)s_state.scale);
    return ESP_OK;
}

void display_show_text(const char *text, display_color_t color,
                       uint8_t scroll_mode, uint8_t speed, uint8_t scale)
{
    if (!text) text = "";
    if (scroll_mode > DISPLAY_SCROLL_CONTINUOUS) {
        scroll_mode = DISPLAY_SCROLL_RESTART;
    }

    // Any explicit content change ends identify mode and aborts a walking
    // test pattern, mirroring the documented exit contract.
    s_identify = false;
    s_test_cancel = true;

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    strncpy(s_state.text, text, sizeof(s_state.text));
    s_state.text[sizeof(s_state.text) - 1] = '\0';
    s_state.color       = color;
    s_state.scroll_mode = scroll_mode;
    s_state.speed       = speed;
    // scale 0 means "keep the current scale", but the kept value still has
    // to fit the CURRENT display: after a reconfigure to a shorter grid the
    // stored scale may be too tall, and every re-render path passes 0.
    s_state.scale = clamp_scale(active_font(s_state.font),
                                scale != 0 ? scale : s_state.scale);
    s_state_gen++;
    xSemaphoreGive(s_state_lock);

    // Empty text repaints even in a scroll mode: the scroll task's idle branch
    // covers an empty string and never clears, so the last marquee frame would
    // stay lit until the next reboot.
    if (scroll_mode == DISPLAY_SCROLL_OFF || text[0] == '\0') {
        led_driver_lock();
        render_static_locked();
        led_driver_unlock();
    }
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void display_show_text_static(const char *text, display_color_t color)
{
    display_show_text(text, color, DISPLAY_SCROLL_OFF, 50, 0);
}

void display_set_offline_marker(bool on, int x, int y,
                                display_color_t color, uint8_t brightness)
{
    // Deliberately does not touch s_state, identify or the test pattern: the
    // marker is an overlay, and nothing about what the panel is showing
    // changes when the link drops.
    led_driver_lock();
    led_driver_set_marker(on, x, y, color.r, color.g, color.b, brightness);
    // Put the current frame back out so the change lands now. Static content
    // has no next frame to wait for, so without this the marker would only
    // appear the next time something else repainted.
    led_driver_refresh();
    led_driver_unlock();
}

void display_set_font(uint8_t font_id, bool prop)
{
    if (font_id >= FONT_COUNT) font_id = FONT_ID_5X7;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_state.font = font_id;
    s_state.prop = prop;
    s_state.scale = clamp_scale(active_font(font_id), s_state.scale);
    s_state_gen++;
    xSemaphoreGive(s_state_lock);
    if (s_task) xTaskNotifyGive(s_task);
}

void display_get_font(uint8_t *font_id_out, bool *prop_out)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (font_id_out) *font_id_out = s_state.font;
    if (prop_out)    *prop_out    = s_state.prop;
    xSemaphoreGive(s_state_lock);
}

void display_clear(void)
{
    s_identify = false;
    s_test_cancel = true;

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_state.text[0] = '\0';
    s_state.scroll_mode = DISPLAY_SCROLL_OFF;
    s_state_gen++;
    xSemaphoreGive(s_state_lock);

    led_driver_lock();
    led_driver_clear();
    led_driver_refresh();
    led_driver_unlock();

    if (s_task) xTaskNotifyGive(s_task);
}

void display_get_state(char *text_out, size_t text_size,
                       display_color_t *color_out,
                       uint8_t *scroll_mode_out,
                       uint8_t *speed_out,
                       uint8_t *scale_out)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (text_out && text_size) {
        strncpy(text_out, s_state.text, text_size);
        text_out[text_size - 1] = '\0';
    }
    if (color_out)       *color_out       = s_state.color;
    if (scroll_mode_out) *scroll_mode_out = s_state.scroll_mode;
    if (speed_out)       *speed_out       = s_state.speed;
    if (scale_out)       *scale_out       = s_state.scale;
    xSemaphoreGive(s_state_lock);
}
