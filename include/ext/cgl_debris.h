// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_debris.h
  * @brief   cgl_debris - the "shatter" scene: a dot matrix turned into a cloud of
  *          rigid bodies, struck by a heavy ball, rebuilt on a cycle.
  *
  *  What this file is
  *  -----------------
  *  The **framework-free** half of the effect. It includes `cgl_phys2d` (the bodies),
  *  `cgl_iso2d` (the camera and the depth sort) and nothing that knows what a
  *  canvas, a widget or a colour is. The drawing lives in `cgl_debris_view.h`.
  *
  *  The scene it owns is *borrowed storage*: the caller declares the physics world
  *  and the camera/sorter and hands them over, exactly as it does for the module's
  *  other two pieces. The module allocates nothing, keeps no mutable global state,
  *  and its whole state is the struct below.
  *
  *  What it does per call
  *  ---------------------
  *      cgl_debris_init()     bind world + camera + bitmap + viewport, build round 1
  *      cgl_debris_step()     observe the world after the CALLER stepped it, advance
  *                            the cycle, rebuild when the wake has settled
  *      cgl_debris_restart()  rebuild now (a key press, or entering the page)
  *
  *  Note what is **not** here: stepping the physics. `cgl_phys2d_step()` is the
  *  application's call (see the page's `on_step`), because "who advances the world"
  *  has to have exactly one answer in a project; a widget or a scene helper that
  *  stepped it would make "the world advances N times per frame once N things call
  *  it" possible again. This module only ever *observes*.
  ******************************************************************************
  */
#ifndef CGL_DEBRIS_H
#define CGL_DEBRIS_H

#include <stdint.h>
#include <stdbool.h>

#include <ext/cgl_phys2d.h>
#include <ext/cgl_iso2d.h>
#include <ext/cgl_debris_config.h>

/**
 * A viewport rectangle in screen pixels, inclusive on all four sides -- the same
 * shape as the framework's `cgl_rect_t`, declared here so this header needs no
 * framework type. The adapter does the four-field conversion in one place.
 */
typedef struct
{
    int16_t x0, y0, x1, y1;
} cgl_debris_rect_t;

/**
 * The dot matrix, as the scene sees it: **the bitmap is the caller's**.
 *
 * Two members and a callback, rather than an included bitmap header: this is what
 * keeps the module a module. The reference application passes its generated
 * `logo_bitmap_get()`; a host test passes a synthetic grid; a product passes
 * whatever it shattered.
 */
typedef struct
{
    int grid_w; /**< cells across, > 0 */
    int grid_h; /**< cells down,   > 0 */
    /** True when cell (i, j) is lit. Cells that are not lit are simply skipped, so
        a bitmap with blank margins costs nothing to walk. */
    bool (*is_lit)(int i, int j);
} cgl_debris_src_t;

/** Phases of the cycle, in the order they happen. */
typedef enum
{
    CGL_DEBRIS_PHASE_SMASH = 0, /**< the ball is on its way / ploughing through   */
    CGL_DEBRIS_PHASE_SETTLE = 1 /**< the ball has left, waiting before the rebuild */
} cgl_debris_phase_t;

/** The scene. Every pointer in it is **borrowed** and must outlive it. */
typedef struct
{
    cgl_phys2d_t *world;         /**< the body pool; the caller declares and steps it */
    cgl_iso2d_view_t *view;      /**< camera + depth sorter. NULL = "draw nothing",
                                      which is what a pure physics test passes       */
    const cgl_debris_src_t *src; /**< the bitmap                                     */
    cgl_debris_rect_t frame;     /**< the viewport the camera is fitted into         */

    /**
     * Called whenever the scene changed in a way that has to be repainted (a
     * rebuild, a phase change). **This is the module's only outward call**, and it
     * exists because "repaint" is the framework's business: the module cannot
     * include the widget header, so it cannot call `cgl_widget_invalidate()`.
     * Optional -- a host test passes NULL and simply does not care.
     */
    void (*on_change)(void *user);
    void *on_change_user;

    uint16_t dot_count;  /**< dots the last build placed                     */
    uint16_t ball_index; /**< index of the heavy ball in the world           */
    uint16_t active;     /**< dots still inside the simulation domain        */
    uint8_t phase;       /**< cgl_debris_phase_t                             */
    uint8_t style;       /**< current camera style (cgl_iso2d_style_t)       */
    uint16_t settle_ms;  /**< ms accumulated in the SETTLE phase             */
    uint32_t restarts;   /**< completed rounds                               */
} cgl_debris_t;

/**
 * Bind the scene to its storage and build the first round.
 *
 * `world` and `src` are required; `view` may be NULL (draw nothing, physics still
 * runs -- that is what the physics host test wants). `frame` is the viewport the
 * camera is fitted into on the next `cgl_debris_set_style()` (this call runs one).
 */
void cgl_debris_init(cgl_debris_t *d, cgl_phys2d_t *world, cgl_iso2d_view_t *view,
                     const cgl_debris_src_t *src, cgl_debris_rect_t frame);

/**
 * Rebuild the dot matrix and put the heavy ball back at its start, then reset the
 * depth sorter (a rebuilt world starts its `step` counter over, and a stale sort
 * stamp would otherwise be hit by the next frame).
 */
void cgl_debris_restart(cgl_debris_t *d);

/**
 * The wrap-up after one physics step: observe the world, advance the
 * SMASH/SETTLE cycle, and rebuild once the settle time is up.
 *
 * WARNING: the caller **must** have stepped the world once already
 * (`cgl_phys2d_step()`); this function does not, and that is deliberate (see the
 * note at the top of the file). `settle_ms` is accumulated from `step_ms`, so
 * `CGL_DEBRIS_SETTLE_MS` stays in milliseconds and is not distorted by the clock's
 * step size.
 */
void cgl_debris_step(cgl_debris_t *d, uint32_t step_ms);

/** Switch the camera style at runtime (re-fits to the current frame). */
void cgl_debris_set_style(cgl_debris_t *d, cgl_iso2d_style_t style);

/** Number of dots the current round was built from. */
uint16_t cgl_debris_dot_count(const cgl_debris_t *d);

/** Number of dots still inside the simulation domain. */
uint16_t cgl_debris_active_dots(const cgl_debris_t *d);

/** Index of the heavy ball in the world (the view paints it differently). */
uint16_t cgl_debris_ball_index(const cgl_debris_t *d);

/**
 * The world coordinates (Q12.4) of dot-matrix cell (i, j) -- exactly the ones
 * `cgl_debris_restart()` used.
 *
 * Meant for render tests: project the world coordinate of every cell back with the
 * current camera and read the pixels back, and a machine can decide whether the
 * matrix is still recognisable after the projection. Without this entry point a
 * test could only keep its own copy of the arithmetic, and the moment the two
 * disagree the test loses its meaning.
 *
 * @return false when (i, j) is outside the grid
 */
bool cgl_debris_cell_world(const cgl_debris_t *d, int i, int j, int16_t *x, int16_t *y);

#endif /* CGL_DEBRIS_H */
