// Host tests for fonts.c: table integrity (every printable glyph drawn,
// no bits outside the glyph height) and the proportional/tabular metrics
// the renderer and scroll math depend on.
//   gcc -I../../main test_fonts.c ../../main/fonts.c && ./a.out

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "fonts.h"

static void test_tables(void)
{
    for (uint8_t id = 0; id < FONT_COUNT; id++) {
        const font_def_t *f = font_get(id);
        assert(f->name && f->name[0]);
        assert(f->w >= 1 && f->w <= 16);
        assert(f->h >= 1 && f->h <= 16);
        assert(f->digit_w >= 1 && f->digit_w <= f->w);
        assert(f->space_w >= 1 && f->space_w <= f->w);

        const uint16_t row_mask = (uint16_t)((1u << f->h) - 1);
        for (int c = 0x20; c <= 0x7E; c++) {
            int any = 0;
            for (int col = 0; col < f->w; col++) {
                uint16_t bits = font_glyph_col(f, (char)c, col);
                assert((bits & ~row_mask) == 0);   // nothing below the cell
                any |= bits;
            }
            if (c == ' ') {
                assert(!any);
            } else {
                assert(any);   // every printable glyph has pixels
            }
        }
        // Out-of-range characters fall back to '?', never to garbage - except
        // the thin space (0x1F), which is deliberately blank.
        assert(font_glyph_col(f, (char)0x1E, 0) == font_glyph_col(f, '?', 0));
        assert(font_glyph_col(f, (char)0x7F, 0) == font_glyph_col(f, '?', 0));
    }
    printf("test_tables ok\n");
}

static void test_lookup(void)
{
    assert(font_id_by_name("5x7") == FONT_ID_5X7);
    assert(font_id_by_name("3x5") == FONT_ID_3X5);
    assert(font_id_by_name("7x12") == FONT_ID_7X12);
    assert(font_id_by_name("6x8") == -1);
    assert(font_id_by_name(NULL) == -1);
    assert(strcmp(font_name(FONT_ID_7X12), "7x12") == 0);
    // Unknown ids fall back to the default font instead of reading off the
    // end of the table.
    assert(font_get(200) == font_get(FONT_ID_5X7));
    printf("test_lookup ok\n");
}

static void test_fit(void)
{
    // Fits: keep the requested font.
    assert(font_fit(FONT_ID_7X12, 16) == FONT_ID_7X12);
    assert(font_fit(FONT_ID_5X7, 7) == FONT_ID_5X7);
    // Too tall: fall back to the tallest font that fits.
    assert(font_fit(FONT_ID_7X12, 8) == FONT_ID_5X7);
    assert(font_fit(FONT_ID_7X12, 5) == FONT_ID_3X5);
    assert(font_fit(FONT_ID_5X7, 6) == FONT_ID_3X5);
    // Nothing fits: the shortest font, clipped by the renderer.
    assert(font_fit(FONT_ID_7X12, 3) == FONT_ID_3X5);
    printf("test_fit ok\n");
}

static void test_spans(void)
{
    for (uint8_t id = 0; id < FONT_COUNT; id++) {
        const font_def_t *f = font_get(id);
        int start, width;

        // Monospace: full cell for everything.
        font_glyph_span(f, 'i', false, &start, &width);
        assert(start == 0 && width == f->w);
        font_glyph_span(f, ' ', false, &start, &width);
        assert(start == 0 && width == f->w);

        // Proportional: trimmed to the inked columns.
        for (int c = 0x21; c <= 0x7E; c++) {
            font_glyph_span(f, (char)c, true, &start, &width);
            assert(width >= 1 && start >= 0 && start + width <= f->w);
            assert(font_glyph_col(f, (char)c, start) != 0 ||
                   (c >= '0' && c <= '9'));
            if (c >= '0' && c <= '9') {   // tabular figures
                assert(start == 0 && width == f->digit_w);
            }
        }
        font_glyph_span(f, ' ', true, &start, &width);
        assert(start == 0 && width == f->space_w);

        // Thin space: narrower than ' ' in both spacing modes, and blank.
        int thin_w;
        font_glyph_span(f, FONT_THIN_SPACE, true, &start, &thin_w);
        assert(start == 0 && thin_w >= 1);
        assert(thin_w < f->space_w || f->space_w == 1);
        font_glyph_span(f, FONT_THIN_SPACE, false, &start, &width);
        assert(width == thin_w);
        for (int col = 0; col < f->w; col++) {
            assert(font_glyph_col(f, FONT_THIN_SPACE, col) == 0);
        }

        // A narrow glyph really is narrower than the cell.
        font_glyph_span(f, '.', true, &start, &width);
        assert(width < f->w || f->w <= 2);
    }
    printf("test_spans ok\n");
}

static void test_widths(void)
{
    const font_def_t *f = font_get(FONT_ID_5X7);

    assert(font_text_width(f, "", true, 1) == 0);
    assert(font_text_width(f, NULL, true, 1) == 0);

    // Monospace matches the historical cell math: n*(w+1)*scale - scale.
    assert(font_text_width(f, "AB", false, 1) == 2 * 6 - 1);
    assert(font_text_width(f, "AB", false, 2) == (2 * 6 - 1) * 2);

    // Proportional never exceeds monospace and scales linearly.
    const char *s = "Hi there 42!";
    int p1 = font_text_width(f, s, true, 1);
    int m1 = font_text_width(f, s, false, 1);
    assert(p1 > 0 && p1 <= m1);
    assert(font_text_width(f, s, true, 3) == p1 * 3);

    // Tabular digits: any digit string of equal length has equal width,
    // so a ticking value cannot jitter.
    for (uint8_t id = 0; id < FONT_COUNT; id++) {
        const font_def_t *g = font_get(id);
        assert(font_text_width(g, "111", true, 1) ==
               font_text_width(g, "888", true, 1));
        assert(font_text_width(g, "0.1", true, 2) ==
               font_text_width(g, "9.7", true, 2));
    }
    printf("test_widths ok\n");
}

int main(void)
{
    test_tables();
    test_lookup();
    test_fit();
    test_spans();
    test_widths();
    printf("fonts: all tests passed\n");
    return 0;
}
