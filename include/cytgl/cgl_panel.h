// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_panel.h
  * @brief   LCD UI framework - solid background / frame container widget.
  ******************************************************************************
  */
#ifndef CGL_PANEL_H
#define CGL_PANEL_H

#include <cytgl/cgl_widget.h>

typedef struct
{
    cgl_widget_t base;
    cgl_color_t bg;
    cgl_color_t border;
    uint8_t border_w;
} cgl_panel_t;

void cgl_panel_init(cgl_panel_t *p, const char *name, cgl_rect_t frame,
                   cgl_color_t bg, cgl_color_t border);

void cgl_panel_set_colors(cgl_panel_t *p, cgl_color_t bg, cgl_color_t border);

#endif /* CGL_PANEL_H */
