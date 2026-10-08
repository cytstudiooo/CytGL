// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_bounce_view.h
  * @brief   cgl_bounce - the half that knows the canvas: one disc, and the
  *          drop-in widget that shows it.
  *
  *  Why the disc is a function *and* a widget
  *  -----------------------------------------
  *  The other two effects of this module ship a painter function and no widget
  *  (`cgl_debris_view.h`, `cgl_particles_view.h`), and both say why: a widget means
  *  owning page state and a palette, which is the application's business. That still
  *  holds, and `cgl_bounce_view_draw()` is the entry point that respects it -- a
  *  scene that owns a depth sort must paint every body itself, in one order, and a
  *  widget that painted this ball would put it in front of whatever it is behind
  *  (see "the one-renderer rule" in the CytGL manual, section 15.6).
  *
  *  But a bare gravity ball is the case the other two are not: it has **one**
  *  element, **one** colour and **no** page state, so "the application's business" is
  *  two fields, and asking every caller to write the same fifteen-line widget is how
  *  a module ends up with a copy of itself in every project. So this half offers
  *  both, and the rule for choosing is one line:
  *
  *      floating on its own?          cgl_bounce_widget_t        (z is not an issue)
  *      inside a sorted scene?        cgl_bounce_view_draw()     (one painter per world)
  *
  *  The reference application does the second: its ball lives in the shatter scene's
  *  world, so the scene's own painter draws it, in depth order, in the `ball_other`
  *  colour of the palette.
  *
  *  The off switch
  *  --------------
  *  `CGL_CFG_BOUNCE 0` (cgl_bounce_config.h) empties this translation unit together
  *  with the engine's, for a build that collects its sources per directory and
  *  therefore compiles it whether or not anybody calls into it.
  ******************************************************************************
  */
#ifndef CGL_BOUNCE_VIEW_H
#define CGL_BOUNCE_VIEW_H

#include <cytgl/cgl_types.h>
#include <cytgl/cgl_canvas.h>
#include <cytgl/cgl_widget.h>

#include <ext/cgl_bounce.h>

/**
 * Convert a framework rectangle (`cgl_rect_t`, e.g. a widget frame) into the
 * engine's own `cgl_bounce_rect_t`.
 *
 * The one place the two rectangle types meet, which is what keeps the engine free of
 * `<cytgl/cgl_types.h>` while still making "bounce inside my widget" a one-liner:
 *
 *     cgl_bounce_init(&ball, world, cgl_bounce_view_area(w->frame));
 */
static inline cgl_bounce_rect_t cgl_bounce_view_frame(cgl_rect_t r)
{
    cgl_bounce_rect_t out;

    out.x0 = r.x0;
    out.y0 = r.y0;
    out.x1 = r.x1;
    out.y1 = r.y1;
    return out;
}

/**
 * The **default playground**: `frame` inset by the ball's radius on all four sides.
 *
 * This is the rectangle that makes the ball's painted disc stay wholly inside the
 * frame -- the ball touches a wall exactly when its edge reaches it, and nothing is
 * ever clipped. A caller that wants the ball to overlap something (the reference
 * scene lets it plough through a dot matrix, and lets its bottom edge run two pixels
 * past the frame, where `CGL_WF_CLIP` trims it) passes its own `cgl_bounce_rect_t`
 * to `cgl_bounce_init()` instead: the playground is the application's decision, and
 * these two functions only exist so that "the obvious one" is also the short one.
 *
 * @note A frame narrower than `2 * CGL_BOUNCE_RADIUS_PX + 1` on either axis produces
 *       an inverted rect, which `cgl_bounce_init()` rejects (`area_rejected`). That is
 *       deliberate: a playground with no room for the ball is a layout bug, and a
 *       silently clamped one would hide it.
 */
static inline cgl_bounce_rect_t cgl_bounce_view_area(cgl_rect_t frame)
{
    cgl_bounce_rect_t a;

    a.x0 = (int16_t)(frame.x0 + CGL_BOUNCE_RADIUS_PX);
    a.y0 = (int16_t)(frame.y0 + CGL_BOUNCE_RADIUS_PX);
    a.x1 = (int16_t)(frame.x1 - CGL_BOUNCE_RADIUS_PX);
    a.y1 = (int16_t)(frame.y1 - CGL_BOUNCE_RADIUS_PX);
    return a;
}

/**
 * Paint the ball into the band `[band_y0, band_y1)` (screen px) of `c`.
 *
 * The ball's centre is read through the same clamp the readout uses, so a step that
 * integrated it past a wall cannot paint one pixel outside the playground -- the
 * frame the walls promise and the frame the disc is drawn in are the same frame, by
 * construction rather than by both being written correctly.
 *
 * Culling is against the **band**, not the viewport: this effect has exactly one
 * element, so an early return is cheaper than any per-primitive test (compare the
 * 481 rigid bodies `cgl_debris_view_draw()` has to walk).
 *
 * @param color the disc colour. A parameter rather than a constant because the
 *              palette is the application's decision -- the reference scene paints
 *              its ball from a `cgl_debris_palette_t`, not from here.
 * @return 1 when the disc was painted, 0 when it was culled, the ball is not in the
 *         world, or it is inactive. A count of 0/1 rather than `void` so a test can
 *         say "this band drew the ball" without reading pixels.
 */
uint16_t cgl_bounce_view_draw(const cgl_bounce_t *b, cgl_canvas_t *c, int16_t band_y0,
                              int16_t band_y1, cgl_color_t color);

/**
 * A gravity ball that draws itself: the engine, one colour, and a widget frame.
 *
 * Use it when the ball is a thing on the screen rather than a body in a scene. It
 * is still a *widget* and not a page, so it obeys the framework's rules (it has no
 * `update` -- see the warning below) and the module's (it never steps the world).
 *
 * WARNING: **the widget does not advance its own physics**, and that is the module's
 * central rule rather than an omission. One `cgl_phys2d_step()` belongs to exactly
 * one place in a project; a widget that stepped it would make "the world advances
 * once per thing that calls it" possible again. So the caller drives the step, with
 * the widget's ball in the middle of it:
 *
 *     cgl_bounce_widget_init(&ball, "ball", frame, &world, CGL_COLOR_RED);
 *     cgl_widget_add_child(root, &ball.base);
 *     ...
 *     void on_step(...) {
 *         cgl_bounce_pre_step(&ball.ball);   // 1) vy += g
 *         cgl_phys2d_step(&world);           // 2) the one integration
 *         cgl_bounce_post_step(&ball.ball);  // 3) four walls
 *     }
 *
 * The widget repaints itself: `cgl_bounce_widget_init()` wires the engine's
 * `on_change` to `cgl_widget_invalidate()`, so there is nothing to remember after a
 * step.
 */
typedef struct
{
    cgl_widget_t base; /**< <- must be the first member */
    cgl_bounce_t ball; /**< the engine; `ball.world` is the caller's, borrowed */
    cgl_color_t color; /**< the disc colour */
} cgl_bounce_widget_t;

/**
 * Bind the widget to a world, give it a frame and throw the ball. The playground is
 * `cgl_bounce_view_area(frame)` -- see it for the two reasons you might want a
 * `cgl_bounce_init()` of your own instead.
 *
 * @return false when the frame is too small to hold the ball (the area was rejected),
 *         in which case the widget is still built and still safe to add to the tree --
 *         it simply never draws anything. Check it if the frame is computed at run
 *         time; a literal frame that fails is a layout bug.
 */
bool cgl_bounce_widget_init(cgl_bounce_widget_t *w, const char *name, cgl_rect_t frame,
                            cgl_phys2d_t *world, cgl_color_t color);

/** Recolour the disc. Repaints only when the colour really changed, per the framework's
    rule that a setter must not repaint for nothing. */
void cgl_bounce_widget_set_color(cgl_bounce_widget_t *w, cgl_color_t color);

#endif /* CGL_BOUNCE_VIEW_H */
