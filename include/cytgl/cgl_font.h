// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_font.h
  * @brief   LCD UI framework - bitmap font rendering.
  *
  *  Glyph storage is row major, 8 horizontally adjacent pixels per byte:
  *
  *      byte index = row * bytes_per_row + (column / 8)
  *      bit  index = column % 8      (bit 0 is the left most pixel)
  *
  *  This is the layout of the classic vendor ASCII tables (LCD_ShowChar in the
  *  1.47" ST7789 example consumes exactly one byte per 8 pixel row), so any
  *  table in that format can be plugged in by filling a cgl_font_t.
  ******************************************************************************
  */
#ifndef CGL_FONT_H
#define CGL_FONT_H

#include <cytgl/cgl_types.h>
#include <cytgl/cgl_canvas.h>

typedef struct
{
    uint8_t width;        /**< glyph cell width in pixels                    */
    uint8_t height;       /**< glyph cell height in pixels                   */
    uint8_t bytes;        /**< bytes per glyph = bytes_per_row * height      */
    const uint8_t *data;  /**< glyph table, ASCII order from `first`         */
    uint8_t first;        /**< first encoded character                       */
    uint8_t last;         /**< last encoded character                        */
} cgl_font_t;

/** 8 x 16 ASCII font, printable range 0x20..0x7E (default). */
extern const cgl_font_t cgl_font_8x16;

/** 12 x 24 ASCII font, printable range 0x20..0x7E (readable on 250 dpi). */
extern const cgl_font_t cgl_font_12x24;

/** Pixel width of a string, 0 for NULL/empty. */
int16_t cgl_font_text_width(const cgl_font_t *font, const char *text);

/** Draw text with a transparent background; returns the advance width. */
int16_t cgl_font_draw_text(cgl_canvas_t *c, const cgl_font_t *font,
                          int16_t x, int16_t y, const char *text, cgl_color_t fg);

/** Draw text filling the glyph cell with `bg` (opaque), returns advance width. */
int16_t cgl_font_draw_text_bg(cgl_canvas_t *c, const cgl_font_t *font,
                             int16_t x, int16_t y, const char *text,
                             cgl_color_t fg, cgl_color_t bg);

#endif /* CGL_FONT_H */
