// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_time.h
  * @brief   LCD UI framework - the one and only clock.
  *
  *  Why this file exists (and what it replaced)
  *  -------------------------------------------
  *  Before it existed the project had **four mutually unaware notions of time**:
  *
  *      1. App/app.c               dt = difference of HAL_GetTick(), clamped to 100 ms
  *      2. src/render/cgl_display.c a 500 ms FPS sliding-window accumulator
  *      3. App/phys2d_clock.c      the physics fixed-step accumulator (1/3 ms units)
  *                                 plus a hook table
  *      4. App/app_logo_smash.c    a settle_ms phase timer inside a widget
  *
  *  Each of them worked on its own, but together they produced two incurable bugs:
  *
  *  **Bug A: the frame boundary lived outside the framework.** phys2d_clock required
  *  the caller to "tick first, then walk the widget tree"; forget the tick and the
  *  physics silently froze (the test suite contains a whole paragraph explaining
  *  exactly this). A framework's correct usage should not depend on the caller
  *  remembering to call two functions in the right order.
  *
  *  **Bug B: "whoever calls first advances the world".** When several widgets share one
  *  physics world, each of them calls phys2d_clock_step() and a convention -- "only the
  *  first caller in a frame actually steps" -- is what keeps it at 60 Hz. That is a
  *  convention, not a type guarantee: add one more widget and someone gets it wrong.
  *
  *  The replacement: **time has exactly one source, the frame boundary moves inside the
  *  framework, and the framework iterates the logic steps.**
  *
  *      cgl_clock_begin(&clock, dt_ms);            // the only place time accumulates
  *      while (cgl_clock_next_step(&clock)) {      // 0..N times, N computed by the clock
  *          page->on_step(step_ms);                // physics: exactly once per step
  *      }
  *      page->on_frame(&clock);                    // once per frame: tweens, state machine
  *      cgl_widget_update_tree(root, clock.dt_ms); // purely visual
  *
  *  With that shape, "the world was stepped twice" becomes structurally impossible --
  *  a widget simply has no step to call.
  *
  *  Two time domains, each with exactly one owner
  *  ---------------------------------------------
  *      step_ms domain (logic):   fixed step, replayable, frame-rate independent
  *                                -> on_step
  *      dt_ms   domain (visual):  follows the frame, may drop frames
  *                                -> on_frame / widget update
  *  The difference in resolution is deliberate: physics speed must not depend on SPI
  *  bandwidth, while fades look best when they follow the frame. Both are converted in
  *  this file, so "what time is it really" has exactly one answer.
  *
  *  This module is pure computation: it depends on no HAL header, and on neither canvas
  *  nor widget.
  ******************************************************************************
  */
#ifndef CGL_TIME_H
#define CGL_TIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h> /* NULL */

typedef struct
{
    /* --- monotonic time --------------------------------------------------- */
    uint32_t now_ms;  /**< milliseconds since cgl_clock_init                    */
    uint32_t frame;   /**< frames begun so far (the first frame = 1)             */
    uint32_t dt_ms;   /**< this frame's dt, already clamped by max_dt_ms          */

    /* --- fixed-step scheduling -------------------------------------------- */
    uint32_t step_ms;     /**< milliseconds per logic step; 0 = run no logic steps */
    uint32_t accum_ms;    /**< fixed-step accumulator (the sub-step remainder stays here) */
    uint32_t max_dt_ms;   /**< upper bound on a single frame's dt                 */
    uint32_t step_total;  /**< total logic steps, **not cleared by reset** (observation) */
    uint8_t max_steps;    /**< steps per frame limit; init takes CGL_CFG_CLOCK_MAX_STEPS */
    uint8_t steps;        /**< steps planned for this frame (0..max_steps)        */
    uint8_t step_index;   /**< iteration cursor of cgl_clock_next_step()          */

    /* --- FPS (from accumulated time, no sliding window, so it cannot drift) - */
    uint32_t fps;         /**< frame rate of the most recent statistics window    */
    uint32_t _fps_ms;     /**< internal: time accumulated in the window           */
    uint32_t _fps_frames; /**< internal: frames in the window                     */
} cgl_clock_t;

/** Milliseconds in one second (so nothing has to spell out 1000u). */
#define CGL_CLOCK_MS_PER_S 1000u

/** Stall protection: a single frame may consume at most this many milliseconds; the
 *  excess is dropped outright. */
#define CGL_CLOCK_DEFAULT_MAX_DT_MS 100u

/**
 * Initialise the clock. `step_ms` = 0 means frame time only, no logic steps (a pure UI
 * scene).
 * Must be called before the first cgl_clock_begin().
 */
void cgl_clock_init(cgl_clock_t *c, uint32_t step_ms, uint32_t max_dt_ms);

/** Reset the counters, keeping the step_ms / max_dt_ms configuration. For restarting a scene. */
void cgl_clock_reset(cgl_clock_t *c);

/**
 * Begin one frame. **This is the only place in the whole framework that accumulates time.**
 * @param dt_ms milliseconds since the previous frame; anything above max_dt_ms is truncated
 *              (a stall is not caught up afterwards)
 */
void cgl_clock_begin(cgl_clock_t *c, uint32_t dt_ms);

/**
 * Iterate the fixed steps of this frame: returning true means "there is another step to
 * run", and every call consumes one step.
 *
 *     while (cgl_clock_next_step(&clock)) { ...the logic of one step... }
 *
 * The step count is computed by begin() from the accumulator, so the loop body runs
 * **exactly** the number of steps this frame should run; the caller can neither run an
 * extra step nor run fewer (unless it deliberately breaks out).
 * @return true while there are steps left; false once this frame's steps are used up
 */
bool cgl_clock_next_step(cgl_clock_t *c);

/**
 * How many steps are left in this frame (i.e. how many next_step calls have not happened
 * yet). For "do the wrap-up only on the final step" style logic; ordinary pages do not
 * need it.
 */
uint8_t cgl_clock_steps_left(const cgl_clock_t *c);

/** Milliseconds in one logic step (this is what on_step receives). */
static inline uint32_t cgl_clock_step_ms(const cgl_clock_t *c)
{
    return c->step_ms;
}

/**
 * Inter-frame interpolation factor, Q8 (0..255): how far the accumulator has moved into
 * the current step.
 *
 * To render "60 Hz physics" on a 40 Hz panel without juddering, keep last frame's
 * positions and interpolate between them by this ratio. This project does not use it
 * (RAM cannot hold a second copy of the position arrays), but the interface stays because
 * it is the **only** place that ratio can be obtained -- a widget computing its own would
 * inevitably drift out of step with the physics.
 */
uint16_t cgl_clock_alpha_q8(const cgl_clock_t *c);

/** Frame rate over the most recent statistics window. Updated at the end of the window,
 *  so this returns 0 for the first 500 ms. */
uint32_t cgl_clock_fps(const cgl_clock_t *c);

#endif /* CGL_TIME_H */
