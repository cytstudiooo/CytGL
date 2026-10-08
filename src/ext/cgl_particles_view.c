// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_particles_view.c
  * @brief   cgl_particles - the band painter (the module's only file that knows
  *          what a canvas is).
  *
  *  Two things are worth knowing before changing this file
  *  ------------------------------------------------------
  *  1. **A dot outside the band is not drawn, but a dot that OVERLAPS the band is.**
  *     The margin in the band test is the dot's own size, not zero: a 3 px dot whose
  *     centre is one row above the band still has rows inside it, and dropping it
  *     would leave a visible seam every band (every 4 rows). This was a real defect
  *     and the test that pins it is in tests/bullet_host_test.c.
  *  2. **The horizontal cull uses the canvas width, not the panel width.** They are
  *     the same number for the framework's band canvas, but reading it from the
  *     canvas means the painter also works for a canvas that is narrower than the
  *     panel (a sub-view), and it keeps the module free of `CGL_CFG_SCREEN_W`.
  ******************************************************************************
  */
#include <ext/cgl_particles_view.h>

#include <string.h>

#if CGL_CFG_PARTICLES

uint16_t cgl_particles_view_draw_band(const cgl_particles_t *s, const cgl_particles_cam_t *cam,
                                      cgl_canvas_t *c, int16_t band_y0, int16_t band_y1,
                                      cgl_color_t color)
{
    uint16_t painted = 0u;
    uint16_t k;

    if ((s == NULL) || (cam == NULL) || (c == NULL) || !s->sorted)
    {
        return 0u;
    }

    /* The canvas is the authority on how wide the window is (see the note at the top
       of the file): a dot may overhang the right edge by its own size and still touch
       the window, so the cull margin is the dot size here too. */
    {
        const int32_t width = (int32_t)c->width;

        for (k = 0u; k < CGL_PARTICLES_MAX; k++)
        {
            const uint16_t i = s->order[k];
            const cgl_particle_t *p = &s->particle[i];
            int32_t sx;
            int32_t sy;
            int32_t depth;
            uint8_t dot;
            int32_t px;
            int32_t py;

            if (p->life == 0u)
            {
                continue;
            }

            cgl_particles_project(cam, p->x, p->y, p->z, &sx, &sy, &depth);

            /* Screen centre offsets come back in Q8.8, so the comparison against the
               band is done in whole px after one shift, with the dot's own size as the
               margin (see 1. above). */
            dot = cgl_particles_dot_px(cam, depth);
            px = sx / CGL_PARTICLES_ONE;
            py = sy / CGL_PARTICLES_ONE;

            if ((py + (int32_t)dot < (int32_t)band_y0) ||
                (py - (int32_t)dot >= (int32_t)band_y1))
            {
                continue;
            }
            if ((px < -(int32_t)dot) || (px > (width + (int32_t)dot)))
            {
                continue;
            }

            cgl_canvas_dot(c, (int16_t)px, (int16_t)py, dot, color);
            painted++;
        }
    }

    return painted;
}

#endif /* CGL_CFG_PARTICLES */
