// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_debris_view.h
  * @brief   cgl_debris - the half that knows the canvas: the floor, the grid, and
  *          the depth-sorted bodies.
  *
  *  Why the floor is not decoration
  *  -------------------------------
  *  The debris gets a one-pixel shadow at its ground point, and on a dark
  *  background a shadow can only be seen if there is something to fall on. The
  *  floor is the precondition that makes "this dot is in the air" readable at all,
  *  which is why it is part of the module and not the application.
  *
  *  The one-renderer rule (read this before using the module)
  *  --------------------------------------------------------
  *  The bodies are painted in **one** depth order, produced by `cgl_iso2d_sort()`:
  *  far first, near last, later paints over earlier. A widget tree paints its
  *  children in insertion order and cannot express "these two dots belong between
  *  those two dots", so **one call to `cgl_debris_view_draw()` must paint every
  *  rigid body of the world**. If one widget painted the matrix and another painted
  *  the balls, the balls would always cover the matrix and the occlusion would be
  *  wrong the moment a ball flies behind it. See the CytGL manual, section 15.6.
  *
  *  Drawing is a function, not a widget type, for the same reason the particle
  *  module does it that way: a widget means owning page state and a palette, which
  *  is the application's business. The application's own widget calls this from its
  *  `draw` with the band the canvas is currently showing.
  ******************************************************************************
  */
#ifndef CGL_DEBRIS_VIEW_H
#define CGL_DEBRIS_VIEW_H

#include <cytgl/cgl_types.h>
#include <cytgl/cgl_canvas.h>

#include <ext/cgl_debris.h>

/**
 * Convert a framework rectangle (a widget frame) into the scene's own rect type.
 *
 * The one place the two rectangle types meet, which is what keeps the engine free of
 * `<cytgl/cgl_types.h>` while still making "fit the camera to my widget" a one-liner:
 *
 *     cgl_debris_init(&scene, world, view, &src, cgl_debris_view_frame(w->frame));
 */
static inline cgl_debris_rect_t cgl_debris_view_frame(cgl_rect_t r)
{
    cgl_debris_rect_t out;

    out.x0 = r.x0;
    out.y0 = r.y0;
    out.x1 = r.x1;
    out.y1 = r.y1;
    return out;
}

/**
 * Colours of the scene. The application decides them (the module has no opinion
 * about a product's palette); these are the reference scene's.
 *
 * `ball_other` is for any *other* sphere in the world -- the demo's bounce ball is
 * a body of the same world, and it is deliberately drawn in a different colour so
 * that "which ball knocked the matrix apart" stays visible.
 */
typedef struct
{
    cgl_color_t floor;     /**< one notch brighter than the background */
    cgl_color_t grid;      /**< the floor's grid lines                 */
    cgl_color_t shadow;    /**< one notch darker than the floor        */
    cgl_color_t dot;       /**< an ordinary (dot-matrix) body          */
    cgl_color_t ball;      /**< the heavy ball of this scene           */
    cgl_color_t ball_other;/**< any other sphere in the world          */
} cgl_debris_palette_t;

/**
 * Paint the floor, its grid and every rigid body that lands in the band
 * `[band_y0, band_y1)` (screen px).
 *
 * Must be called **once per band**, in increasing band order, from the widget that
 * owns this scene -- that order is what a band renderer walks, and it is what lets
 * this function work with no z-buffer: the bodies come out of the sorter far to
 * near, so a later one simply paints over an earlier one.
 *
 * @param pal the palette; NULL means "no colour at all", which draws nothing (the
 *            function does not invent a default, so a caller that forgot the
 *            palette gets an empty frame instead of a surprise).
 * @return the number of bodies actually painted (a cheap way for a test to prove
 *         "this band drew something").
 */
uint16_t cgl_debris_view_draw(const cgl_debris_t *d, cgl_canvas_t *c,
                              int16_t band_y0, int16_t band_y1,
                              const cgl_debris_palette_t *pal);

#endif /* CGL_DEBRIS_VIEW_H */
