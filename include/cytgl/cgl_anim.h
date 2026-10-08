// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_anim.h
  * @brief   LCD UI framework - integer tweens (one tween object = one timeline).
  *
  *  Why "tween" is a framework facility
  *  ------------------------------------
  *  A page transition needs something that "moves/fades from A to B over T
  *  milliseconds"; menu highlighting, toast overlays and value-change animation
  *  all need the same thing. If every widget wrote its own
  *
  *      acc += dt; if (acc >= TOTAL) { acc = 0; ... }
  *
  *  the project would regrow four or five separate copies of progress
  *  arithmetic -- exactly the "clock clutter" we want to eliminate. So the tween
  *  moves into the framework and a widget only holds one cgl_anim_t.
  *
  *  Pure integers, no floating point, no libm
  *  -----------------------------------------
  *  The easing curves are implemented as Q8 fixed-point polynomials (see
  *  cgl_anim_ease). The Cortex-M4 has an FPU, but float drags in libm's 64-bit
  *  helper functions (the project already has code rewritten for this, see the
  *  "do not use int64 and drag in __aeabi_ldivmod" note in app_logo_smash.c),
  *  and these curves never needed that much precision anyway.
  *
  *  This module is pure computation; it does not depend on canvas / widget / HAL.
  ******************************************************************************
  */
#ifndef CGL_ANIM_H
#define CGL_ANIM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h> /* NULL */

/** Easing curve. The direction in the name says which end of "from -> to" is slow. */
typedef enum
{
    CGL_EASE_LINEAR = 0,  /**< constant speed: mechanical, but hardest to get wrong */
    CGL_EASE_IN_QUAD,     /**< slow start (accelerating)                          */
    CGL_EASE_OUT_QUAD,    /**< fast start then decelerate; the usual "natural" one */
    CGL_EASE_IN_OUT_QUAD, /**< slow at both ends, fast in the middle              */
    CGL_EASE_OUT_CUBIC,   /**< a "softer" finish than OUT_QUAD; the page default   */
    CGL_EASE_OUT_BACK,    /**< overshoots then springs back (Q8 value > 256); for emphasis */
    CGL_EASE_COUNT
} cgl_ease_t;

/** Q8 fixed-point 1.0. */
#define CGL_ANIM_ONE_Q8 256

#define CGL_ANIM_F_RUNNING 0x01u /**< still running (includes the delay phase)   */
#define CGL_ANIM_F_DONE 0x02u    /**< reached `to`; the value no longer changes  */

typedef struct
{
    int32_t from;        /**< start value                                  */
    int32_t to;          /**< end value                                    */
    int32_t value;       /**< current value (updated on every tick)        */
    uint32_t dur_ms;     /**< duration; 0 = jump straight to `to` next frame */
    uint32_t elapsed_ms; /**< elapsed duration (excluding delay)           */
    uint32_t delay_ms;   /**< remaining delay; while >0 the value stays at `from` */
    uint8_t ease;        /**< cgl_ease_t                                   */
    uint8_t flags;       /**< CGL_ANIM_F_*                                 */
} cgl_anim_t;

/** Clear into a "never started" tween (value = 0). */
void cgl_anim_init(cgl_anim_t *a);

/** Start: from `from` to `to`, duration dur_ms, curve `ease`. Clears delay and done. */
void cgl_anim_start(cgl_anim_t *a, int32_t from, int32_t to, uint32_t dur_ms, cgl_ease_t ease);

/** Wait delay_ms milliseconds before it starts moving. Must be called **after** start() (start clears delay). */
void cgl_anim_set_delay(cgl_anim_t *a, uint32_t delay_ms);

/**
 * Advance by dt_ms.
 * @return true means the value changed this frame -- the caller uses that to
 *         decide whether to repaint. Still inside the delay, or already
 *         finished, both return false.
 */
bool cgl_anim_tick(cgl_anim_t *a, uint32_t dt_ms);

/** Jump straight to the end value and set DONE. */
void cgl_anim_finish(cgl_anim_t *a);

/** Stop immediately, keep the current value and clear RUNNING (does not jump to the end). */
void cgl_anim_stop(cgl_anim_t *a);

static inline int32_t cgl_anim_value(const cgl_anim_t *a) { return a->value; }
static inline bool cgl_anim_running(const cgl_anim_t *a) { return (a->flags & CGL_ANIM_F_RUNNING) != 0u; }
static inline bool cgl_anim_done(const cgl_anim_t *a) { return (a->flags & CGL_ANIM_F_DONE) != 0u; }

/** Linear progress as Q8 (0..256), no easing and no delay. */
uint16_t cgl_anim_progress_q8(const cgl_anim_t *a);

/**
 * The easing function itself: input linear progress p (Q8, 0..256), output the
 * eased progress (Q8). Except for CGL_EASE_OUT_BACK the output stays inside
 * 0..256; OUT_BACK goes slightly past 256. It is exposed separately so the
 * regression tests can drive it directly (the end points must hit exactly 0 and
 * 256).
 */
uint16_t cgl_anim_ease(cgl_ease_t ease, uint16_t p_q8);

/**
 * Q8 linear interpolation: t=0 returns a, t=256 returns b.
 * Integer arithmetic; it does not overflow while a/b are of screen-coordinate
 * magnitude.
 */
int32_t cgl_anim_lerp(int32_t a, int32_t b, uint16_t t_q8);

#endif /* CGL_ANIM_H */
