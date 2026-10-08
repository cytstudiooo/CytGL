// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_font.c
  * @brief   LCD UI framework - bitmap font rendering implementation.
  *
  *  The renderer walks the glyph row by row and writes straight into the band
  *  row returned by cgl_canvas_row(), so no per pixel function call is needed.
  *
  *  WARNING: bypassing the primitive functions has a price: cgl_canvas_row()
  *  wants a **screen row number**, while text is given in **primitive
  *  coordinates**. So this code must add the canvas pan itself -- without that
  *  step the **text inside a panned subtree (page-slide transition, scrolling
  *  list) stays put** while the rectangles in the same widget do move. Measured:
  *  during a COVER_UP transition all 430 text pixels of the new page's top bar
  *  were misplaced (this is exactly what the per-pixel transition check in
  *  render_host_test.c caught).
  ******************************************************************************
  */
#include <cytgl/cgl_font.h>

static const uint8_t *font__glyph(const cgl_font_t *font, char ch)
{
    uint8_t code = (uint8_t)ch;

    if ((code < font->first) || (code > font->last))
    {
        return NULL;
    }
    return font->data + ((uint32_t)(code - font->first) * font->bytes);
}

static uint8_t font__bytes_per_row(const cgl_font_t *font)
{
    return (uint8_t)((font->width + 7u) / 8u);
}

int16_t cgl_font_text_width(const cgl_font_t *font, const char *text)
{
    int16_t n = 0;

    if ((font == NULL) || (text == NULL))
    {
        return 0;
    }
    while (text[n] != '\0')
    {
        n++;
    }
    return (int16_t)(n * font->width);
}

/**
 * Shared glyph blitter. `bg_enabled` selects between a transparent draw and a
 * full glyph cell fill.
 */
static int16_t font__draw(cgl_canvas_t *c, const cgl_font_t *font,
                          int16_t x, int16_t y, const char *text,
                          cgl_color_t fg, cgl_color_t bg, bool bg_enabled)
{
    cgl_rect_t clip;
    cgl_point_t pan;
    uint8_t bpr;
    int16_t sx;
    int16_t sy;
    int16_t cx;
    bool warped = (cgl_canvas_get_warp(c) != NULL);

    if ((font == NULL) || (text == NULL))
    {
        return 0;
    }

    clip = cgl_canvas_get_clip(c);
    bpr = font__bytes_per_row(font);

    /* Primitive coordinates -> screen coordinates. clip is already in screen
       coordinates, so the comparisons below need no further adjustment. With a
       warp this step yields **source** coordinates, and the landing position
       still has to be mapped once more (see below). */
    pan = cgl_canvas_get_pan(c);
    sx = (int16_t)(x + pan.x);
    sy = (int16_t)(y + pan.y);
    cx = sx;

    while (*text != '\0')
    {
        const uint8_t *g = font__glyph(font, *text);

        if (g != NULL)
        {
            uint8_t row;

            for (row = 0u; row < font->height; row++)
            {
                int16_t py = (int16_t)(sy + row);
                int16_t dy;
                cgl_color_t *dst;
                const uint8_t *grow;
                uint8_t col;

                /* With a warp, "which row it lands on" must be computed from the
                   source row: without mapping, the whole run of text stays put
                   while only the warped rectangles fly away -- the same class of
                   bug as forgetting the pan. */
                dy = warped ? cgl_canvas_map_y(c, py) : py;
                if ((dy < clip.y0) || (dy > clip.y1))
                {
                    continue;
                }
                dst = cgl_canvas_row(c, dy);
                if (dst == NULL)
                {
                    continue;
                }
                grow = g + ((uint32_t)row * bpr);

                for (col = 0u; col < font->width; col++)
                {
                    int16_t lx = (int16_t)(cx + col);
                    int16_t px;
                    bool on = ((grow[col >> 3] >> (col & 7u)) & 0x01u) != 0u;

                    if (warped)
                    {
                        /* The source clipping window is also tested in **source**
                           coordinates (reason in src_clip in cgl_canvas.h). */
                        if ((lx < cgl_canvas_get_src_clip(c).x0) ||
                            (lx > cgl_canvas_get_src_clip(c).x1))
                        {
                            continue;
                        }
                        px = cgl_canvas_map_x(c, lx, py);
                    }
                    else
                    {
                        px = lx;
                    }

                    if ((px < clip.x0) || (px > clip.x1))
                    {
                        continue;
                    }
                    if (on)
                    {
                        dst[px] = fg;
                    }
                    else if (bg_enabled)
                    {
                        dst[px] = bg;
                    }
                }
            }
        }
        cx = (int16_t)(cx + font->width);
        text++;
    }
    return (int16_t)(cx - sx);
}

int16_t cgl_font_draw_text(cgl_canvas_t *c, const cgl_font_t *font,
                          int16_t x, int16_t y, const char *text, cgl_color_t fg)
{
    return font__draw(c, font, x, y, text, fg, fg, false);
}

int16_t cgl_font_draw_text_bg(cgl_canvas_t *c, const cgl_font_t *font,
                             int16_t x, int16_t y, const char *text,
                             cgl_color_t fg, cgl_color_t bg)
{
    return font__draw(c, font, x, y, text, fg, bg, true);
}
