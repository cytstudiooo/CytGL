// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_debris_view.c
  * @brief   cgl_debris - the floor, the grid and the depth-sorted bodies.
  *
  *  One band at a time, and **one** painter for the whole world (see the note in
  *  cgl_debris_view.h: the painter's algorithm needs a single sequence, and a
  *  widget tree paints in insertion order).
  ******************************************************************************
  */
#include <ext/cgl_debris_view.h>
#include <ext/cgl_iso2d_phys2d.h> /* the bridge: a body pool seen as a sort source */

#if CGL_CFG_DEBRIS
/**
 * Clip a line segment to the height of the "band" and then draw it.
 *
 * cgl_canvas_line() is Bresenham and walks the whole segment from end to end --
 * a floor grid line is 300 px long on screen while a band is only 4 px high, so
 * without clipping every band would burn 300 pixel tests per line for nothing
 * (22 lines x 43 bands = 280k per frame). Clipped to the band only a dozen or so
 * remain.
 */
static void cgl_debris__line_band(cgl_canvas_t *c, int32_t x0, int32_t y0, int32_t x1,
                            int32_t y1, int16_t by0, int16_t by1, cgl_color_t color)
{
    int32_t ya;
    int32_t yb;
    int32_t lo;
    int32_t hi;

    if (y0 == y1)
    {
        if ((y0 >= by0) && (y0 <= by1))
        {
            cgl_canvas_line(c, (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, color);
        }
        return;
    }

    ya = (y0 < y1) ? y0 : y1;
    yb = (y0 < y1) ? y1 : y0;
    lo = (by0 > ya) ? by0 : ya;
    hi = (by1 < yb) ? by1 : yb;
    if (lo > hi)
    {
        return;
    }

    {
        /* 32-bit integers are enough: dx is ~700 at most and the interpolation
           span ~200 at most, so the product 1.4e5 is far from overflow. Written
           as int64 this would pull __aeabi_ldivmod in from libgcc (software
           64-bit division, a few hundred bytes of flash + tens of cycles per
           call), which is pure waste on this 128 KB flash chip. */
        const int32_t dx = x1 - x0;
        const int32_t dy = y1 - y0;
        const int32_t xa = x0 + (((dx * (lo - y0)) / dy));
        const int32_t xb = x0 + (((dx * (hi - y0)) / dy));

        cgl_canvas_line(c, (int16_t)xa, (int16_t)lo, (int16_t)xb, (int16_t)hi, color);
    }
}

/**
 * Horizontal span at a given row y of "the convex quad a world rectangle
 * projects into".
 * Used to fill the floor as a diamond -- the band renderer draws only 4 rows at a
 * time, so it cannot fill a polygon in one go.
 * @return false means this row lies completely outside the quad
 */
static bool cgl_debris__quad_span(const int16_t *qx, const int16_t *qy, int32_t y,
                            int32_t *span_a, int32_t *span_b)
{
    int32_t mn = 32767;
    int32_t mx = -32768;
    int32_t i;

    for (i = 0; i < 4; i++)
    {
        const int32_t j = (i + 1) & 3;
        const int32_t ay = qy[i];
        const int32_t by = qy[j];
        int32_t ax;
        int32_t bx;

        if (ay == by)
        {
            if (y != ay)
            {
                continue;
            }
            ax = qx[i];
            bx = qx[j];
        }
        else
        {
            if (((y < ay) || (y > by)) && ((y < by) || (y > ay)))
            {
                continue;
            }
            /* same as above: products of screen coordinates are far inside the
               int32 range, do not drag __aeabi_ldivmod in with int64 */
            ax = qx[i] + (((qx[j] - qx[i]) * (y - ay)) / (by - ay));
            bx = ax;
        }
        if (ax < mn)
        {
            mn = ax;
        }
        if (bx > mx)
        {
            mx = bx;
        }
        if (ax > mx)
        {
            mx = ax;
        }
        if (bx < mn)
        {
            mn = bx;
        }
    }

    if (mn > mx)
    {
        return false;
    }
    *span_a = mn;
    *span_b = mx;
    return true;
}

/** Draw the "floor": fill a diamond one notch brighter than the background plus
    sparse grid lines. */
static void cgl_debris__draw_floor(const cgl_iso2d_cam_t *cam, cgl_canvas_t *c,
                                int16_t by0, int16_t by1,
                                const cgl_debris_palette_t *pal)
{
    int16_t qx[4];
    int16_t qy[4];
    int32_t x;
    int32_t y;
    int32_t wx0 = CGL_PHYS2D_WORLD_X0;
    int32_t wy0 = CGL_PHYS2D_WORLD_Y0;
    int32_t wx1 = CGL_PHYS2D_WORLD_X0 + CGL_PHYS2D_WORLD_W;
    int32_t wy1 = CGL_PHYS2D_WORLD_Y0 + CGL_PHYS2D_WORLD_H;

    /* the four corners of the world rectangle, projected to the screen (z = 0) */
    cgl_iso2d_project(cam, (int16_t)wx0, (int16_t)wy0, 0, &qx[0], &qy[0]);
    cgl_iso2d_project(cam, (int16_t)wx1, (int16_t)wy0, 0, &qx[1], &qy[1]);
    cgl_iso2d_project(cam, (int16_t)wx1, (int16_t)wy1, 0, &qx[2], &qy[2]);
    cgl_iso2d_project(cam, (int16_t)wx0, (int16_t)wy1, 0, &qx[3], &qy[3]);

    /* fill: compute the span of every single row in this band */
    for (y = by0; y < by1; y++)
    {
        int32_t sa;
        int32_t sb;

        if (cgl_debris__quad_span(qx, qy, y, &sa, &sb))
        {
            cgl_canvas_hline(c, (int16_t)sa, (int16_t)sb, (int16_t)y, pal->floor);
        }
    }

    /* grid lines: one group per direction, each group drawn from far to near
       (what is drawn first is covered by what follows, but the colour is the
       same here, so the order does not matter -- it just keeps the code simple). */
    for (x = wx0; x <= wx1; x += (CGL_DEBRIS_FLOOR_STEP_PX * CGL_PHYS2D_ONE))
    {
        int16_t ax;
        int16_t ay;
        int16_t bx;
        int16_t by;

        cgl_iso2d_project(cam, (int16_t)x, (int16_t)wy0, 0, &ax, &ay);
        cgl_iso2d_project(cam, (int16_t)x, (int16_t)wy1, 0, &bx, &by);
        cgl_debris__line_band(c, ax, ay, bx, by, by0, (int16_t)(by1 - 1), pal->grid);
    }
    for (y = wy0; y <= wy1; y += (CGL_DEBRIS_FLOOR_STEP_PX * CGL_PHYS2D_ONE))
    {
        int16_t ax;
        int16_t ay;
        int16_t bx;
        int16_t by;

        cgl_iso2d_project(cam, (int16_t)wx0, (int16_t)y, 0, &ax, &ay);
        cgl_iso2d_project(cam, (int16_t)wx1, (int16_t)y, 0, &bx, &by);
        cgl_debris__line_band(c, ax, ay, bx, by, by0, (int16_t)(by1 - 1), pal->grid);
    }
}
uint16_t cgl_debris_view_draw(const cgl_debris_t *l, cgl_canvas_t *c,
                              int16_t band_y0, int16_t band_y1,
                              const cgl_debris_palette_t *pal)
{
    const cgl_phys2d_t *world;
    cgl_iso2d_view_t *view;
    const cgl_iso2d_cam_t *cam;
    const cgl_iso2d_sorter_t *s;
    const cgl_phys2d_body_t *bodies;
    uint16_t k;
    uint16_t painted = 0u;

    if ((l == NULL) || (c == NULL) || (pal == NULL))
    {
        return 0u;
    }
    world = l->world;
    view = l->view;

    if ((world == NULL) || (view == NULL))
    {
        return 0u;
    }
    cam = &view->cam;

    /* depth sort: within one physics step it really sorts only once (the source's
       stamp is world->step), so calling it once per band is not wasteful. */
    cgl_iso2d_sort_bodies(&view->sorter, cam, world);
    s = &view->sorter;

    cgl_debris__draw_floor(cam, c, band_y0, band_y1, pal);

    bodies = world->body;

    /* draw far to near: a rigid body further back is drawn later and naturally
       covers the ones in front -- that is the whole occlusion logic. */
    for (k = 0u; k < s->count; k++)
    {
        const int16_t i = s->order[k];
        const cgl_phys2d_body_t *b = &bodies[i];
        const bool sphere = ((b->flags & CGL_PHYS2D_FLAG_SPHERE) != 0u);
        int32_t gx;
        int32_t gy;
        int32_t sx;
        int32_t sy;
        int32_t r;
        int32_t margin;

        gx = cgl_iso2d_ground_x(cam, b->x, b->y) + cam->ox;
        gy = cgl_iso2d_ground_y(cam, b->x, b->y) + cam->oy;
        /* height lift: this is the -z*height of screen_y = (x+y)*sin30 - z*height */
        sy = gy - (((int32_t)cam->kz * b->z) >> 12);
        r = cgl_phys2d_to_px(b->r);
        if (sphere)
        {
            /* the ball "stands" on the floor: its centre is one radius above its
               ground point */
            sy -= r;
        }
        sx = gx;

        /* band culling: balls and dots each use their own smallest margin, do
           not use the ball's big margin to let a heap of dots through */
        margin = sphere ? (r + 1) : CGL_DEBRIS_DOT_PX;
        if ((sy < ((int32_t)band_y0 - margin)) || (sy > ((int32_t)band_y1 + margin)))
        {
            continue;
        }
        if ((sx < -margin) || (sx > (CGL_DEBRIS_SCREEN_W + margin)))
        {
            continue;
        }

        /* shadow: drawn at the ground point. It is what lets an airborne dot be
           read as having "height".
           The ball always has a shadow, a dot only while it is off the ground. */
        if (sphere || (b->z > CGL_DEBRIS_SHADOW_MIN_Z_Q4))
        {
            const int32_t shadow_y = gy;

            if ((shadow_y >= ((int32_t)band_y0 - margin)) &&
                (shadow_y <= ((int32_t)band_y1 + margin)))
            {
                if (sphere)
                {
                    cgl_canvas_disc(c, (int16_t)sx, (int16_t)shadow_y, (int16_t)r,
                                   pal->shadow);
                }
                else
                {
                    cgl_canvas_dot(c, (int16_t)sx, (int16_t)shadow_y, 1, pal->shadow);
                }
            }
        }

        if (sphere)
        {
            /* this widget's heavy ball is amber, the bounce ball (added by another
               widget) is red */
            const cgl_color_t col = (i == l->ball_index) ? pal->ball : pal->ball_other;

            cgl_canvas_disc(c, (int16_t)sx, (int16_t)sy, (int16_t)r, col);
            /* a touch of highlight: one saved line of code buys "this is a ball"
               instead of "this is a circle" */
            if (r >= 3)
            {
                cgl_canvas_dot(c, (int16_t)(sx - 1), (int16_t)(sy - 1), 2, pal->dot);
            }
        }
        else
        {
            cgl_canvas_dot(c, (int16_t)sx, (int16_t)sy, CGL_DEBRIS_DOT_PX, pal->dot);
        }
        painted++;
    }

    return painted;
}
#endif /* CGL_CFG_DEBRIS */
