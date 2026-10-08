// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_particles_view.h
  * @brief   cgl_particles - the half that knows the canvas: paint a sorted particle
  *          cloud into one band.
  *
  *  Why this is a separate file from the engine
  *  -------------------------------------------
  *  `cgl_particles.[ch]` is deliberately free of every framework type, so the
  *  simulation, the camera, the projection and the sort can be built and unit
  *  tested with no UI framework in the graph at all. Painting cannot be: it needs
  *  `cgl_canvas_t` and a colour. The rule this module follows (the same one the
  *  physics module follows, see the CytGL manual, section 15.1) is that the dependency points
  *  **one way** -- the view may include the engine, never the other way round:
  *
  *      cgl_particles_view.h   -> cgl_canvas.h, cgl_particles.h
  *      cgl_particles.h        -> nothing but the C standard library
  *
  *  There is no widget type here on purpose. A widget means owning page state and a
  *  palette, which is the application's business, not the module's; what the module
  *  owes a caller is "here is the sorted cloud, here is how to paint one band of
  *  it". The reference application's bullet time page wraps this in its own widget
  *  (it also has a wrecking ball to draw, and a "frozen" palette).
  *
  *  The off switch
  *  --------------
  *  `CGL_CFG_PARTICLES 0` (cgl_particles_config.h) empties this translation unit
  *  together with the engine's, for a build that collects its sources per directory
  *  and therefore compiles it whether or not anybody calls into it.
  ******************************************************************************
  */
#ifndef CGL_PARTICLES_VIEW_H
#define CGL_PARTICLES_VIEW_H

#include <cytgl/cgl_types.h>
#include <cytgl/cgl_canvas.h>

#include <ext/cgl_particles.h>

/**
 * Convert a framework rectangle (`cgl_rect_t`, e.g. a widget frame) into the
 * engine's own `cgl_particles_rect_t`.
 *
 * This is the only place the two rectangle types meet, which is what keeps the
 * engine free of `<cytgl/cgl_types.h>` while still making "point the camera at my
 * widget's frame" a one-liner at the call site:
 *
 *     cgl_particles_cam_look(&cam, yaw, orbit, cgl_particles_view_frame(w->frame));
 */
static inline cgl_particles_rect_t cgl_particles_view_frame(cgl_rect_t r)
{
    cgl_particles_rect_t out;

    out.x0 = r.x0;
    out.y0 = r.y0;
    out.x1 = r.x1;
    out.y1 = r.y1;
    return out;
}

/**
 * Paint the particles of `s` that land in the band `[band_y0, band_y1)` (screen px)
 * into `c`.
 *
 * Must be called **once per band**, in increasing band order -- that is the order a
 * band renderer walks, and it is what lets this function do with no z-buffer at all:
 * `cgl_particles_sort()` produced a far-to-near order, so a later particle simply
 * paints over an earlier one. Culling against the band (rather than against the
 * whole viewport) is what keeps 43 bands from each examining the whole cloud.
 *
 * The sort must already have been run for the camera being passed (`s->sorted`);
 * this function does not sort, because with an orbiting camera the order changes
 * without any particle moving and the sort belongs to the caller's step, not to a
 * per-band draw.
 *
 * @param color the dot colour. It is a parameter rather than a constant because the
 *              palette is the application's decision -- the "frozen" flash of the
 *              reference effect, for instance, is expressed by the widget's group
 *              alpha, not by a second colour.
 * @return the number of particles actually painted (a HUD readout, and the cheap way
 *         for a test to prove "this band drew something").
 */
uint16_t cgl_particles_view_draw_band(const cgl_particles_t *s, const cgl_particles_cam_t *cam,
                                      cgl_canvas_t *c, int16_t band_y0, int16_t band_y1,
                                      cgl_color_t color);

#endif /* CGL_PARTICLES_VIEW_H */
