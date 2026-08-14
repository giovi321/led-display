#include "display.h"

#include <ctype.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "led_driver.h"

static const char *TAG = "display";

// 5x7 monospace pixel font, ASCII 32..126. Each glyph is 5 bytes; each byte
// is one column. Bit 0 = top row, bit 6 = bottom row. Bit 7 unused.
//
// Compact public-domain style 5x7 set, sufficient for the panel UI.
static const uint8_t font5x7[95][FONT_WIDTH] = {
    {0x00,0x00,0x00,0x00,0x00}, // 0x20 ' '
    {0x00,0x00,0x5F,0x00,0x00}, // 0x21 '!'
    {0x00,0x07,0x00,0x07,0x00}, // 0x22 '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // 0x23 '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // 0x24 '$'
    {0x23,0x13,0x08,0x64,0x62}, // 0x25 '%'
    {0x36,0x49,0x55,0x22,0x50}, // 0x26 '&'
    {0x00,0x05,0x03,0x00,0x00}, // 0x27 '\''
    {0x00,0x1C,0x22,0x41,0x00}, // 0x28 '('
    {0x00,0x41,0x22,0x1C,0x00}, // 0x29 ')'
    {0x08,0x2A,0x1C,0x2A,0x08}, // 0x2A '*'
    {0x08,0x08,0x3E,0x08,0x08}, // 0x2B '+'
    {0x00,0x50,0x30,0x00,0x00}, // 0x2C ','
    {0x08,0x08,0x08,0x08,0x08}, // 0x2D '-'
    {0x00,0x60,0x60,0x00,0x00}, // 0x2E '.'
    {0x20,0x10,0x08,0x04,0x02}, // 0x2F '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // 0x30 '0'
    {0x00,0x42,0x7F,0x40,0x00}, // 0x31 '1'
    {0x42,0x61,0x51,0x49,0x46}, // 0x32 '2'
    {0x21,0x41,0x45,0x4B,0x31}, // 0x33 '3'
    {0x18,0x14,0x12,0x7F,0x10}, // 0x34 '4'
    {0x27,0x45,0x45,0x45,0x39}, // 0x35 '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // 0x36 '6'
    {0x01,0x71,0x09,0x05,0x03}, // 0x37 '7'
    {0x36,0x49,0x49,0x49,0x36}, // 0x38 '8'
    {0x06,0x49,0x49,0x29,0x1E}, // 0x39 '9'
    {0x00,0x36,0x36,0x00,0x00}, // 0x3A ':'
    {0x00,0x56,0x36,0x00,0x00}, // 0x3B ';'
    {0x00,0x08,0x14,0x22,0x41}, // 0x3C '<'
    {0x14,0x14,0x14,0x14,0x14}, // 0x3D '='
    {0x41,0x22,0x14,0x08,0x00}, // 0x3E '>'
    {0x02,0x01,0x51,0x09,0x06}, // 0x3F '?'
    {0x32,0x49,0x79,0x41,0x3E}, // 0x40 '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 0x41 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 0x42 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 0x43 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 0x44 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 0x45 'E'
    {0x7F,0x09,0x09,0x01,0x01}, // 0x46 'F'
    {0x3E,0x41,0x41,0x51,0x32}, // 0x47 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 0x48 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 0x49 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 0x4A 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 0x4B 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 0x4C 'L'
    {0x7F,0x02,0x04,0x02,0x7F}, // 0x4D 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 0x4E 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 0x4F 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 0x50 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 0x51 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 0x52 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 0x53 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 0x54 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 0x55 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 0x56 'V'
    {0x7F,0x20,0x18,0x20,0x7F}, // 0x57 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 0x58 'X'
    {0x03,0x04,0x78,0x04,0x03}, // 0x59 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 0x5A 'Z'
    {0x00,0x7F,0x41,0x41,0x00}, // 0x5B '['
    {0x02,0x04,0x08,0x10,0x20}, // 0x5C '\'
    {0x00,0x41,0x41,0x7F,0x00}, // 0x5D ']'
    {0x04,0x02,0x01,0x02,0x04}, // 0x5E '^'
    {0x40,0x40,0x40,0x40,0x40}, // 0x5F '_'
    {0x00,0x01,0x02,0x04,0x00}, // 0x60 '`'
    {0x20,0x54,0x54,0x54,0x78}, // 0x61 'a'
    {0x7F,0x48,0x44,0x44,0x38}, // 0x62 'b'
    {0x38,0x44,0x44,0x44,0x20}, // 0x63 'c'
    {0x38,0x44,0x44,0x48,0x7F}, // 0x64 'd'
    {0x38,0x54,0x54,0x54,0x18}, // 0x65 'e'
    {0x08,0x7E,0x09,0x01,0x02}, // 0x66 'f'
    {0x0C,0x52,0x52,0x52,0x3E}, // 0x67 'g'
    {0x7F,0x08,0x04,0x04,0x78}, // 0x68 'h'
    {0x00,0x44,0x7D,0x40,0x00}, // 0x69 'i'
    {0x20,0x40,0x44,0x3D,0x00}, // 0x6A 'j'
    {0x7F,0x10,0x28,0x44,0x00}, // 0x6B 'k'
    {0x00,0x41,0x7F,0x40,0x00}, // 0x6C 'l'
    {0x7C,0x04,0x18,0x04,0x78}, // 0x6D 'm'
    {0x7C,0x08,0x04,0x04,0x78}, // 0x6E 'n'
    {0x38,0x44,0x44,0x44,0x38}, // 0x6F 'o'
    {0x7C,0x14,0x14,0x14,0x08}, // 0x70 'p'
    {0x08,0x14,0x14,0x18,0x7C}, // 0x71 'q'
    {0x7C,0x08,0x04,0x04,0x08}, // 0x72 'r'
    {0x48,0x54,0x54,0x54,0x20}, // 0x73 's'
    {0x04,0x3F,0x44,0x40,0x20}, // 0x74 't'
    {0x3C,0x40,0x40,0x20,0x7C}, // 0x75 'u'
    {0x1C,0x20,0x40,0x20,0x1C}, // 0x76 'v'
    {0x3C,0x40,0x30,0x40,0x3C}, // 0x77 'w'
    {0x44,0x28,0x10,0x28,0x44}, // 0x78 'x'
    {0x0C,0x50,0x50,0x50,0x3C}, // 0x79 'y'
    {0x44,0x64,0x54,0x4C,0x44}, // 0x7A 'z'
    {0x00,0x08,0x36,0x41,0x00}, // 0x7B '{'
    {0x00,0x00,0x7F,0x00,0x00}, // 0x7C '|'
    {0x00,0x41,0x36,0x08,0x00}, // 0x7D '}'
    {0x08,0x04,0x08,0x10,0x08}, // 0x7E '~'
};

static inline const uint8_t *glyph_for(char c)
{
    unsigned char u = (unsigned char)c;
    if (u < 0x20 || u > 0x7E) {
        u = '?';
    }
    return font5x7[u - 0x20];
}

typedef struct {
    char text[DISPLAY_TEXT_MAX];
    display_color_t color;
    bool scroll;
    uint8_t speed;   // 0..255, mapped to a frame interval
    uint8_t scale;   // 1..FONT_SCALE_MAX (integer font multiplier)
} display_state_t;

static uint8_t clamp_scale(uint8_t s)
{
    if (s < FONT_SCALE_MIN) s = FONT_SCALE_MIN;
    if (s > FONT_SCALE_MAX) s = FONT_SCALE_MAX;
    // A glyph at scale N is FONT_HEIGHT*N rows tall, so clamp to what
    // actually fits the panel - no point silently clipping the top/bottom.
    int fits = led_driver_height() / FONT_HEIGHT;
    if (fits < 1) fits = 1;
    if (s > fits) s = (uint8_t)fits;
    return s;
}

// Pick a sensible default scale based on the active display height: 2x for
// the 64x16 grid, 1x for a single 32x8 panel, 3x if the display is taller.
// Deliberately conservative (max 3) - anything bigger is a user choice.
static uint8_t default_scale(void)
{
    int h = led_driver_height();
    if (h >= FONT_HEIGHT * 3) return 3;
    if (h >= FONT_HEIGHT * 2) return 2;
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

static int cell_width_at(uint8_t scale)
{
    return FONT_WIDTH * scale + scale;  // glyph + 1-col gap, both scaled
}

static int glyph_height_at(uint8_t scale)
{
    return FONT_HEIGHT * scale;
}

static int text_pixel_width_at(const char *s, uint8_t scale)
{
    int len = (int)strlen(s);
    if (len == 0) return 0;
    return len * cell_width_at(scale) - scale;  // drop the trailing gap
}

static void draw_char(int x_origin, char c, display_color_t col, uint8_t scale)
{
    const uint8_t *g = glyph_for(c);
    const int y_off = (led_driver_height() - glyph_height_at(scale)) / 2;
    for (int col_i = 0; col_i < FONT_WIDTH; col_i++) {
        uint8_t bits = g[col_i];
        for (int row_i = 0; row_i < FONT_HEIGHT; row_i++) {
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

// Render `text` so its leftmost column lands at column `x_origin` (which may
// be negative or past the display width for scrolling). Clipping is implicit:
// `led_driver_set_pixel` ignores out-of-range coordinates.
static void render_text_at(int x_origin, const char *text,
                           display_color_t col, uint8_t scale)
{
    const int disp_w = led_driver_width();
    led_driver_clear();
    int cursor = x_origin;
    const int cw = cell_width_at(scale);
    const int gw = FONT_WIDTH * scale;
    for (const char *p = text; *p; p++) {
        // Skip whole glyphs that fall entirely off either edge for speed.
        if (cursor + gw > 0 && cursor < disp_w) {
            draw_char(cursor, *p, col, scale);
        }
        cursor += cw;
        if (cursor >= disp_w && x_origin >= 0) {
            break;
        }
    }
    led_driver_refresh();
}

static void render_static_locked(void)
{
    int w = text_pixel_width_at(s_state.text, s_state.scale);
    int x = 0;
    if (w < led_driver_width()) {
        x = (led_driver_width() - w) / 2;
    }
    render_text_at(x, s_state.text, s_state.color, s_state.scale);
}

static TickType_t speed_to_period(uint8_t speed)
{
    // 0 -> slow (~200 ms / frame), 255 -> fast (~15 ms / frame).
    int ms = 200 - (int)speed * (200 - 15) / 255;
    if (ms < 15) ms = 15;
    return pdMS_TO_TICKS(ms);
}

static void scroll_task(void *arg)
{
    (void)arg;
    int offset = led_driver_width();  // start off the right edge

    while (1) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        bool scroll = s_state.scroll;
        char text[DISPLAY_TEXT_MAX];
        strncpy(text, s_state.text, sizeof(text));
        text[sizeof(text) - 1] = '\0';
        display_color_t col = s_state.color;
        TickType_t period = speed_to_period(s_state.speed);
        uint8_t scale = s_state.scale;
        uint32_t gen = s_state_gen;
        xSemaphoreGive(s_state_lock);

        if (s_identify || !scroll || text[0] == '\0') {
            // Idle: just wait for the next state change.
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
            continue;
        }

        int w = text_pixel_width_at(text, scale);
        led_driver_lock();
        // Re-check under the driver lock: identify may have been switched on
        // or the state replaced (clear, new static text) after we copied it,
        // and one stale frame painted now would stay on screen forever once
        // this task goes idle on the next iteration.
        if (!s_identify && gen == s_state_gen) {
            render_text_at(offset, text, col, scale);
        }
        led_driver_unlock();

        offset--;
        if (offset < -w) {
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
    s_state.scroll  = false;
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
        bool scroll;
        uint8_t speed, scale;
        display_get_state(tx, sizeof(tx), &col, &scroll, &speed, &scale);
        if (tx[0] != '\0') {
            display_show_text(tx, col, scroll, speed, 0);
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
    s_state.color   = (display_color_t){80, 32, 0};
    s_state.scroll  = false;
    s_state.speed   = 50;
    s_state.scale   = default_scale();

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
                       bool scroll, uint8_t speed, uint8_t scale)
{
    if (!text) text = "";

    // Any explicit content change ends identify mode and aborts a walking
    // test pattern, mirroring the documented exit contract.
    s_identify = false;
    s_test_cancel = true;

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    strncpy(s_state.text, text, sizeof(s_state.text));
    s_state.text[sizeof(s_state.text) - 1] = '\0';
    s_state.color  = color;
    s_state.scroll = scroll;
    s_state.speed  = speed;
    // scale 0 means "keep the current scale", but the kept value still has
    // to fit the CURRENT display: after a reconfigure to a shorter grid the
    // stored scale may be too tall, and every re-render path passes 0.
    s_state.scale = clamp_scale(scale != 0 ? scale : s_state.scale);
    s_state_gen++;
    xSemaphoreGive(s_state_lock);

    if (!scroll) {
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
    display_show_text(text, color, false, 50, 0);
}

void display_clear(void)
{
    s_identify = false;
    s_test_cancel = true;

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_state.text[0] = '\0';
    s_state.scroll  = false;
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
                       bool *scroll_out,
                       uint8_t *speed_out,
                       uint8_t *scale_out)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (text_out && text_size) {
        strncpy(text_out, s_state.text, text_size);
        text_out[text_size - 1] = '\0';
    }
    if (color_out)  *color_out  = s_state.color;
    if (scroll_out) *scroll_out = s_state.scroll;
    if (speed_out)  *speed_out  = s_state.speed;
    if (scale_out)  *scale_out  = s_state.scale;
    xSemaphoreGive(s_state_lock);
}
