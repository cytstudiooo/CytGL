// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_bounce_view.c
  * @brief   cgl_bounce - one disc into one band, and the widget that wraps it.
  *
  *  A disc is the cheapest primitive the canvas has, so the only things worth
  *  getting right here are **which** position to draw and **whether to draw at all**:
  *
  *    - the position comes from `cgl_bounce_draw_position_px()`, i.e. the clamped one,
  *      so "the walls promise the ball is inside the playground" and "the disc is
  *      painted inside the playground" are the same statement rather than two pieces
  *      of code that have to agree. A step that integrated the ball past a wall can
  *      therefore never paint a pixel outside it, whatever order the caller ran the
  *      walls and the draw in.
  *    - the cull is a band test plus a horizontal test, both against the **canvas**
  *      (`c->width`), not against the panel configuration: the module has no business
  *      knowing how wide the screen is, and a caller that renders into a narrower
  *      canvas gets the right answer for free.
  ******************************************************************************
  */
#include <ext/cgl_bounce_view.h>

#if CGL_CFG_BOUNCE

/* -------------------------------------------------------------------------- */
/* The painter                                                                */
/* -------------------------------------------------------------------------- */

uint16_t cgl_bounce_view_draw(const cgl_bounce_t *b, cgl_canvas_t *c, int16_t band_y0,
                              int16_t band_y1, cgl_color_t color)
{
    int16_t sx;
    int16_t sy;

    if ((b == NULL) || (c == NULL))
    {
        return 0u;
    }
    if (!cgl_bounce_draw_position_px(b, &sx, &sy))
    {
        return 0u;
    }

    /* Vertical culling first: a band is 4 px high and the ball is 3 px in radius, so
       most bands hold nothing of it, and the y test is the one that rejects them.
       The tolerance is the radius, because a disc whose *centre* is just outside the
       band still paints its top or bottom edge inside it. */
    if ((sy < (int16_t)(band_y0 - CGL_BOUNCE_DRAW_R_PX)) || (sy > band_y1))
    {
        return 0u;
    }
    if ((sx < (int16_t)(-CGL_BOUNCE_DRAW_R_PX)) ||
        (sx > (int16_t)(c->width + CGL_BOUNCE_DRAW_R_PX)))
    {
        return 0u;
    }

    cgl_canvas_disc(c, sx, sy, CGL_BOUNCE_DRAW_R_PX, color);
    return 1u;
}

/* -------------------------------------------------------------------------- */
/* The drop-in widget                                                         */
/* -------------------------------------------------------------------------- */

/** The engine's only outward call, pointed at the framework's "repaint me". */
static void cgl_bounce__widget_change(void *user)
{
    cgl_bounce_widget_t *w = (cgl_bounce_widget_t *)user;

    if (w != NULL)
    {
        cgl_widget_invalidate(&w->base);
    }
}

static void cgl_bounce__widget_draw(cgl_widget_t *widget, cgl_canvas_t *c)
{
    const cgl_bounce_widget_t *w = (const cgl_bounce_widget_t *)widget;

    /* The widget paints exactly one band's worth of one disc; a band that holds none
       of it costs the two tests inside cgl_bounce_view_draw() and nothing else. The
       return value (0 or 1) is dropped here -- it exists for tests. */
    (void)cgl_bounce_view_draw(&w->ball, c, c->origin_y, (int16_t)(c->origin_y + c->height),
                               w->color);
}

static const cgl_widget_vt_t cgl_bounce__widget_vt = {
    .type = "cgl.bounce",
    .draw = cgl_bounce__widget_draw,
    /* update = NULL: the ball's position is decided by the physics step, and the
       physics step belongs to the **page** (see the module's central rule). The widget
       has no `dt` to work with and deliberately does not take one -- a widget that
       animated itself here would advance a world it does not own. */
    .update = NULL,
    .event = NULL,
};

bool cgl_bounce_widget_init(cgl_bounce_widget_t *w, const char *name, cgl_rect_t frame,
                            cgl_phys2d_t *world, cgl_color_t color)
{
    bool accepted;

    if (w == NULL)
    {
        return false;
    }

    cgl_widget_init(&w->base, &cgl_bounce__widget_vt, name, frame);
    w->base.flags |= CGL_WF_CLIP; /* the disc must not paint outside the widget */
    w->color = color;

    accepted = cgl_bounce_init(&w->ball, world, cgl_bounce_view_area(frame));

    /* Wired **after** init, because cgl_bounce_init() deliberately clears both fields:
       an engine that inherited a stale callback from whatever was in the struct before
       would call into a dead widget. */
    w->ball.on_change = cgl_bounce__widget_change;
    w->ball.on_change_user = w;

    return accepted;
}

void cgl_bounce_widget_set_color(cgl_bounce_widget_t *w, cgl_color_t color)
{
    if ((w == NULL) || (w->color == color))
    {
        return; /* the framework's rule: a setter repaints only when the pixels changed */
    }
    w->color = color;
    cgl_widget_invalidate(&w->base);
}

#endif /* CGL_CFG_BOUNCE */
