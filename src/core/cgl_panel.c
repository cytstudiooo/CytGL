// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_panel.c
  * @brief   LCD UI framework - solid background / frame container widget.
  ******************************************************************************
  */
#include <cytgl/cgl_panel.h>

static void panel__draw(cgl_widget_t *w, cgl_canvas_t *c)
{
    cgl_panel_t *p = (cgl_panel_t *)w;
    uint8_t i;

    cgl_canvas_fill_rect(c, w->frame, p->bg);

    for (i = 0; i < p->border_w; i++)
    {
        cgl_rect_t r = cgl_rect((int16_t)(w->frame.x0 + i), (int16_t)(w->frame.y0 + i),
                              (int16_t)(w->frame.x1 - i), (int16_t)(w->frame.y1 - i));
        if (cgl_rect_is_empty(r))
        {
            break;
        }
        cgl_canvas_rect(c, r, p->border);
    }
}

static const cgl_widget_vt_t panel__vt = {
    .type = "panel",
    .draw = panel__draw,
    .update = NULL,
    .event = NULL,
};

void cgl_panel_init(cgl_panel_t *p, const char *name, cgl_rect_t frame,
                   cgl_color_t bg, cgl_color_t border)
{
    cgl_widget_init(&p->base, &panel__vt, name, frame);
    p->bg = bg;
    p->border = border;
    p->border_w = (border == bg) ? 0u : 1u;
}

void cgl_panel_set_colors(cgl_panel_t *p, cgl_color_t bg, cgl_color_t border)
{
    p->bg = bg;
    p->border = border;
    p->border_w = (border == bg) ? 0u : 1u;
    cgl_widget_invalidate(&p->base);
}
