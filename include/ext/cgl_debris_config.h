// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_debris_config.h
  * @brief   cgl_debris - compile time configuration for the "shatter" effect.
  *
  *  What this module is
  *  -------------------
  *  The effect behind the demo's smash page: a dot matrix (usually a bitmap of
  *  text) becomes a cloud of equal-mass rigid bodies, a heavy ball flies in and
  *  ploughs through it, and the round rebuilds itself once the wake has settled.
  *  The physics and the projection are the module's neighbours (`cgl_phys2d`,
  *  `cgl_iso2d`); what lives here is the **scene**: the grid the dots are laid out
  *  on, the ball that strikes it, the camera that fits it and the cycle that repeats
  *  it.
  *
  *  It comes in two halves, the same way the particle module does:
  *
  *      cgl_debris_view.[ch]   the drawing: floor, grid, depth-sorted bodies
  *              |
  *      cgl_debris.[ch]        the scene: bitmap -> body cloud, camera fit, cycle.
  *                             Framework free -- it includes cgl_phys2d / cgl_iso2d
  *                             and nothing that knows what a canvas is.
  *
  *  **Not linked = 0 bytes**, exactly as for the rest of the module: the core never
  *  lists this effect, and opt-in linking is what keeps a UI-only build free of it.
  *  `CGL_CFG_DEBRIS 0` additionally empties the translation units, for a build that
  *  collects its sources per directory (this repository's own firmware build does);
  *  see cgl_particles_config.h for the full reasoning -- it is the same trade.
  *
  *  WARNING: the switch is build-wide. Two translation units that disagree about it
  *  cannot share a `cgl_debris_t`.
  ******************************************************************************
  */
#ifndef CGL_DEBRIS_CONFIG_H
#define CGL_DEBRIS_CONFIG_H

/* --------------------------------------------------------------------------
 * 1. The off switch
 * ------------------------------------------------------------------------ */
#ifndef CGL_CFG_DEBRIS
#define CGL_CFG_DEBRIS 1
#endif

/* --------------------------------------------------------------------------
 * 2. The panel the dot matrix is centred on
 *
 * The reference scene centres the grid on the **panel**, not on the widget frame
 * (the frame is the animation viewport, which sits between two bars). Kept as a
 * config knob for the same reason the physics module keeps its world rectangle:
 * the scene is allowed to know the panel, and a consumer that wants a grid centred
 * somewhere else overrides this or lays its own out and does not use
 * `cgl_debris_cell_world()`.
 *
 * Picked up from the framework's own configuration when it is reachable (there is
 * exactly one source of truth for panel geometry in this project), with the
 * reference panel as the fallback so the module also builds standalone.
 * ------------------------------------------------------------------------ */
#if defined(CGL_CFG_SCREEN_W) && defined(CGL_CFG_SCREEN_H)
#define CGL_DEBRIS_SCREEN_W CGL_CFG_SCREEN_W
#define CGL_DEBRIS_SCREEN_H CGL_CFG_SCREEN_H
#else
#define CGL_DEBRIS_SCREEN_W 320
#define CGL_DEBRIS_SCREEN_H 172
#endif

/* --------------------------------------------------------------------------
 * 3. Grid geometry
 *
 * CGL_DEBRIS_STEP_Q4 is the distance between two dots, Q12.4: 48/16 = exactly
 * 3 px, so the dot matrix lands on a perfectly regular pixel grid and a painted dot
 * never bleeds into the neighbouring cell.
 *
 * CGL_DEBRIS_DOT_R_Q4 is the **collision** radius, small enough that two
 * neighbouring dots can never start overlapped (2*23 <= 48). The painted size is
 * `CGL_DEBRIS_DOT_PX` and it is the view's business.
 *
 * The grid's **size** is not here: it comes from the caller's
 * `cgl_debris_src_t` (`grid_w` / `grid_h`), because it is the bitmap's shape and
 * only the application knows it.
 * ------------------------------------------------------------------------ */
#ifndef CGL_DEBRIS_STEP_Q4
#define CGL_DEBRIS_STEP_Q4 48
#endif

#ifndef CGL_DEBRIS_DOT_R_Q4
#define CGL_DEBRIS_DOT_R_Q4 23
#endif

/** Size of the painted dot, in pixels. WARNING: it and the camera style are a
    **pair** and must be tuned together -- the smallest on-screen dot pitch is
    decided entirely by the camera, and a dot bigger than the pitch makes neighbours
    touch and fills the negative space of the glyphs solid. The reference scene's
    measurement (3 px dots on a 2.79 px pitch bleed 3.0 %; a textbook-uniform camera
    with 3 px dots bleeds 32.9 % and reads as one diagonal band) is pinned by
    `tests/render_host_test.c`, which projects every cell back and counts the
    wrongly-lit ones. */
#ifndef CGL_DEBRIS_DOT_PX
#define CGL_DEBRIS_DOT_PX 3
#endif

/* --------------------------------------------------------------------------
 * 4. The wrecking ball
 *
 * A kinematic sphere (inverse mass 0): it is never pushed by the dot matrix, it
 * ploughs it apart. It starts well off screen and a little below the centre, so the
 * matrix is visible on its own for a moment before the impact.
 * ------------------------------------------------------------------------ */
#define CGL_DEBRIS_BALL_R_Q4 (5 * CGL_PHYS2D_ONE)       /**< collision radius, Q12.4 -> 5 px */
#define CGL_DEBRIS_BALL_SPEED_Q4 75                     /**< speed per logical step, Q4    */
#define CGL_DEBRIS_BALL_PX 5                            /**< painted radius, px             */
#define CGL_DEBRIS_BALL_START_X_Q4 (-140 * CGL_PHYS2D_ONE)      /**< spawn x -> -140 px     */
#define CGL_DEBRIS_BALL_START_Y_OFFSET_Q4 (-4 * CGL_PHYS2D_ONE) /**< offset from the centre */

/* --------------------------------------------------------------------------
 * 5. The cycle
 * ------------------------------------------------------------------------ */
/** Time the screen is left alone after the ball left, before rebuilding. It is
    accumulated in `step_ms` (see `cgl_debris_step`), so it is still
    **milliseconds**: changing the clock's step does not distort this value. */
#ifndef CGL_DEBRIS_SETTLE_MS
#define CGL_DEBRIS_SETTLE_MS 900u
#endif

/** How far off screen the ball has to be (px, beyond the panel's right edge) before
    the scene counts as "the ball has left" and moves on to settling. */
#ifndef CGL_DEBRIS_EXIT_MARGIN_PX
#define CGL_DEBRIS_EXIT_MARGIN_PX 30
#endif

/* --------------------------------------------------------------------------
 * 6. The camera fit (passed straight to cgl_iso2d_cam_fit)
 * ------------------------------------------------------------------------ */
/** Camera style for the fitted scene (a `cgl_iso2d_style_t`). `CGL_ISO2D_STYLE_ISO`
    is the fitted isometric one: it allocates the vertical budget so that the
    **smallest** dot spacing is as large as possible. WARNING: enlarging it on its
    own shrinks the dot pitch -- reduce `CGL_DEBRIS_DOT_PX` at the same time or the
    colour bleed explodes (see the note on `CGL_DEBRIS_DOT_PX`). */
#ifndef CGL_DEBRIS_SCENE_STYLE
#define CGL_DEBRIS_SCENE_STYLE CGL_ISO2D_STYLE_ISO
#endif

/** Margin kept inside the viewport while the camera is fitted, px. */
#ifndef CGL_DEBRIS_SCENE_MARGIN_PX
#define CGL_DEBRIS_SCENE_MARGIN_PX 2
#endif

/** World spacing of the floor grid lines, px (the view's business, kept here
    because it is scene geometry and pairs with the camera fit). */
#ifndef CGL_DEBRIS_FLOOR_STEP_PX
#define CGL_DEBRIS_FLOOR_STEP_PX 32
#endif

/** Only a dot above this height (Q12.4) gets a shadow: drawing a shadow for a dot
    glued to the ground just wastes pixels. */
#ifndef CGL_DEBRIS_SHADOW_MIN_Z_Q4
#define CGL_DEBRIS_SHADOW_MIN_Z_Q4 (CGL_PHYS2D_ONE / 2)
#endif

/* --------------------------------------------------------------------------
 * 7. Derived: where cell (0,0) of a `grid_w` x `grid_h` grid sits, Q12.4
 *
 * Macros rather than stored fields: the grid size is the caller's, and the centre
 * is a compile time constant, so nothing has to be kept in RAM to answer "where is
 * cell i,j". The reference application's `cgl_debris_cell_world()` is these two
 * expressions, and it exists so that a render test can project a known cell and
 * read the pixels back.
 * ------------------------------------------------------------------------ */
#define CGL_DEBRIS_CENTER_X_Q4 ((CGL_DEBRIS_SCREEN_W * CGL_PHYS2D_ONE) / 2)
#define CGL_DEBRIS_CENTER_Y_Q4 ((CGL_DEBRIS_SCREEN_H * CGL_PHYS2D_ONE) / 2)
#define CGL_DEBRIS_X0_Q4(grid_w) (CGL_DEBRIS_CENTER_X_Q4 - (((grid_w) * CGL_DEBRIS_STEP_Q4) / 2))
#define CGL_DEBRIS_Y0_Q4(grid_h) (CGL_DEBRIS_CENTER_Y_Q4 - (((grid_h) * CGL_DEBRIS_STEP_Q4) / 2))

/* The whole grid, including half a cell of margin: what the camera is fitted to
   when the bitmap turns out to be empty (so a zero-sized camera is never
   produced). */
#define CGL_DEBRIS_WORLD_X0_Q4(grid_w) (CGL_DEBRIS_X0_Q4(grid_w) - (CGL_DEBRIS_STEP_Q4 / 2))
#define CGL_DEBRIS_WORLD_Y0_Q4(grid_h) (CGL_DEBRIS_Y0_Q4(grid_h) - (CGL_DEBRIS_STEP_Q4 / 2))
#define CGL_DEBRIS_WORLD_W_Q4(grid_w) ((grid_w) * CGL_DEBRIS_STEP_Q4)
#define CGL_DEBRIS_WORLD_H_Q4(grid_h) ((grid_h) * CGL_DEBRIS_STEP_Q4)

#endif /* CGL_DEBRIS_CONFIG_H */
