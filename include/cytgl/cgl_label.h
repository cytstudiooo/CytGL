// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_label.h
  * @brief   LCD UI framework - text label widget.
  *
  *  The label owns a fixed size text buffer, so no heap and no printf are
  *  required anywhere in the framework.
  ******************************************************************************
  */
#ifndef CGL_LABEL_H
#define CGL_LABEL_H

#include <cytgl/cgl_widget.h>
#include <cytgl/cgl_font.h>
#include <cytgl/cytgl_config.h>

typedef enum
{
    CGL_ALIGN_LEFT = 0,
    CGL_ALIGN_CENTER = 1,
    CGL_ALIGN_RIGHT = 2
} cgl_align_t;

typedef struct
{
    cgl_widget_t base;
    const cgl_font_t *font;
    cgl_color_t fg;
    cgl_color_t bg;
    uint8_t opaque;
    uint8_t align;
    char text[CGL_CFG_LABEL_MAX_TEXT];
} cgl_label_t;

void cgl_label_init(cgl_label_t *l, const char *name, cgl_rect_t frame,
                   const cgl_font_t *font, const char *text,
                   cgl_color_t fg, cgl_color_t bg, bool opaque);

/** Replace the text. Repaints only when the content actually changed. */
void cgl_label_set_text(cgl_label_t *l, const char *text);

/** Replace the text with prefix + decimal value + suffix (no printf needed). */
void cgl_label_set_int(cgl_label_t *l, const char *prefix, int32_t value, const char *suffix);

/** Replace the text with prefix + value + '.' + one fraction digit + suffix. */
void cgl_label_set_fixed1(cgl_label_t *l, const char *prefix, int32_t value_x10, const char *suffix);

void cgl_label_set_align(cgl_label_t *l, cgl_align_t align);
void cgl_label_set_colors(cgl_label_t *l, cgl_color_t fg, cgl_color_t bg, bool opaque);

#endif /* CGL_LABEL_H */
