// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_bounce.h
  * @brief   cgl_bounce - a ball under gravity that bounces off four walls.
  *
  *  What this file is
  *  -----------------
  *  The **framework-free** half of the effect: the motion, and nothing else. It
  *  includes `cgl_phys2d` (the body pool) and nothing that knows what a canvas, a
  *  widget or a colour is. The drawing lives in `cgl_bounce_view.h`.
  *
  *  Why this is a module and not thirty lines in your application
  *  ------------------------------------------------------------
  *  `cgl_phys2d` is a *collision solver*: it has no gravity, no walls and no
  *  restitution (see its header -- that is by design, not an omission, because a
  *  general force model is the part that costs RAM). So every gravity ball in every
  *  project re-derives the same four things, and each of them has a way of being
  *  subtly wrong:
  *
  *    1. **`vy += g` must happen before the integration, not after.** Added first it
  *       is semi-implicit Euler (stable); added after it is explicit Euler, which
  *       gains height at the top of every arc.
  *    2. **Reflecting the velocity is not enough -- the position has to be clamped
  *       back too.** Without the clamp the ball ends up out of range after the hit
  *       and then "bounces" from somewhere it never was, which reads as a stutter.
  *    3. **The world has no memory of your body.** `cgl_phys2d_reset()` only zeroes
  *       `count`; it frees nothing, and it does not tell you that your index is now
  *       in range but belongs to somebody else. This module re-establishes its body
  *       by **identity** (`r` + the sphere flag), not by index, so a rebuilt world
  *       cannot silently turn the ball into a debris dot. See `cgl_bounce__ensure`.
  *    4. **Restitution has a floor.** The reference playground loses (1 - e^2) of the
  *       energy per bounce, so anything much below the default makes the ball stick
  *       to the floor after two or three hits -- which reads as "the animation
  *       stopped", not as "the ball settled".
  *
  *  What it does per call
  *  ---------------------
  *      cgl_bounce_init()        bind to a world + an active area, throw the ball
  *      cgl_bounce_pre_step()    vy += g      -- before cgl_phys2d_step()
  *      cgl_bounce_post_step()   four walls   -- after  cgl_phys2d_step()
  *      cgl_bounce_restart()     back to the spawn point now
  *
  *  Note what is **not** here: stepping the physics. `cgl_phys2d_step()` is the
  *  application's call, exactly as it is for `cgl_debris` and `cgl_particles`,
  *  because "who advances the world" has to have exactly one answer in a project.
  *  The two halves are separate entry points rather than one `cgl_bounce_step()`
  *  for that reason: the single `cgl_phys2d_step()` call has to sit **between**
  *  them, and a helper that hid it would be a second place that steps the world.
  *
  *  WARNING: the two halves must come in pairs, and **exactly once per step**. A
  *  missing `pre_step` is a ball that floats; a missing `post_step` is a ball that
  *  leaves the area and is only clamped back when somebody reads its position.
  ******************************************************************************
  */
#ifndef CGL_BOUNCE_H
#define CGL_BOUNCE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h> /* NULL */

#include <ext/cgl_phys2d.h>
#include <ext/cgl_bounce_config.h>

/**
 * A rectangle in screen pixels, inclusive on all four sides -- the same shape as
 * the framework's `cgl_rect_t`, declared here so this header needs no framework
 * type. The adapter `cgl_bounce_view_frame()` does the four-field conversion.
 */
typedef struct
{
    int16_t x0, y0, x1, y1;
} cgl_bounce_rect_t;

/**
 * The active area: **the inclusive rectangle of screen pixels the ball's CENTRE
 * may occupy**.
 *
 * Centre range rather than "the box the ball fits in" because that is what the two
 * clamps and the two reflections actually compare against, so there is no place in
 * this module where a radius is added to or subtracted from a bound and no way for
 * the collision area and the painted area to drift apart. The painted extent is
 * this rectangle grown by `CGL_BOUNCE_RADIUS_PX` on each side, and a caller that
 * wants the ball to stay wholly inside a frame passes the frame inset by the
 * radius -- which is what `cgl_bounce_view_area()` computes.
 *
 * @note A ball whose centre is on the boundary is **inside** it (the walls are
 *       inclusive), and the ball is exactly touching the wall at that moment.
 */

/**
 * The ball. Every pointer in it is **borrowed** and must outlive it.
 *
 * Deliberately plain data with no hidden state: a caller can put it in a page
 * struct, keep it on the stack for a test, or memcpy it around, and the only thing
 * that has to stay true is that `world` outlives it.
 */
typedef struct
{
    cgl_phys2d_t *world; /**< the body pool; the caller declares and steps it */

    /**
     * Index of the ball inside `world`, or `CGL_BOUNCE_NO_INDEX` when it is not in
     * the pool. **Never read this directly and act on it** -- it is only valid
     * until the next `cgl_phys2d_reset()`. `cgl_bounce__ensure()` re-establishes the
     * body before every use; use `cgl_bounce_index()` when you want it for display.
     */
    uint16_t index;

    uint16_t bounces;      /**< cumulative wall hits (a HUD readout, and what a test asserts on) */
    uint16_t max_speed_q4; /**< historical maximum |v| per step, Q12.4 -- observation only        */
    uint16_t area_rejected;/**< how many times an impossible area was rejected (see set_area)     */

    /**
     * 1 = there is a playground, 0 = there is not (`cgl_bounce_init()` was handed an
     * impossible area, or no area at all).
     *
     * This flag is what makes "rejected" mean **inert** rather than "left in some
     * default state". Without it the four bounds below would be the only record of the
     * playground, and their zero value -- a one-pixel-square playground at the origin --
     * is a perfectly *legal* area: an "inert" ball would then happily add itself to the
     * world and bounce in the corner of the screen, which is a far worse answer than
     * nothing happening. So the state is explicit, and every entry point checks it.
     */
    uint8_t has_area;

    /* The active area, Q12.4, derived from the rect `cgl_bounce_set_area()` accepted. */
    int16_t left_q4;
    int16_t right_q4;
    int16_t top_q4;
    int16_t bot_q4;

    /* The spawn: where `restart()` and the self-healing put the ball back. */
    int16_t start_x_q4;
    int16_t start_y_q4;
    int16_t start_vx_q4;
    int16_t start_vy_q4;

    /**
     * Called whenever the ball moved in a way that has to be repainted -- which is
     * every step it takes. **This is the module's only outward call**, and it exists
     * because "repaint" is the framework's business: this half cannot include the
     * widget header, so it cannot call `cgl_widget_invalidate()`. Optional -- a host
     * test passes NULL and simply does not care, and the drop-in widget in
     * `cgl_bounce_view.h` wires it up for you.
     */
    void (*on_change)(void *user);
    void *on_change_user;
} cgl_bounce_t;

/** `cgl_bounce_t::index` when the ball is not in the pool. Not a valid pool index. */
#define CGL_BOUNCE_NO_INDEX 0xFFFFu

/**
 * Bind the ball to a world and an active area (see `cgl_bounce_rect_t`) and throw
 * it: the spawn point and velocity are derived from the area by the
 * `CGL_BOUNCE_SPAWN_*` knobs, so "top left, tossed to the right" holds wherever the
 * caller puts the playground. Use `cgl_bounce_set_spawn()` for anything else.
 *
 * `world` is required and must outlive the ball.
 *
 * @return false when the ball cannot run: either there is **no world**, or the area was
 *         **rejected** -- inverted or empty, or reaching outside the physics simulation
 *         domain. A kinematic body is allowed to leave that domain (the engine keeps
 *         integrating it so it can come back), but while it is out there it is not in
 *         the broad phase, so it silently stops colliding with everything else; a
 *         playground that reaches out there is a ball that stops ploughing through the
 *         scenery at the edges, which is the kind of failure that gets diagnosed as
 *         "the physics is flaky".
 *
 *         In every failure case nothing is thrown, every later call is a no-op, and the
 *         ball is left **inert rather than half-alive**: safe to keep calling, safe to
 *         step. A rejected *area* is counted (`area_rejected`), so "the ball never
 *         appeared" is a number rather than a guess; a missing world is not, because it
 *         is a programming error rather than a value.
 */
bool cgl_bounce_init(cgl_bounce_t *b, cgl_phys2d_t *world, cgl_bounce_rect_t area);

/**
 * Move the playground at run time (a resize, a rotation, a different widget frame).
 *
 * This moves the **walls**, not the ball: a ball that ends up outside the new area
 * is clamped back into it by the next `cgl_bounce_post_step()`, and one that is
 * inside keeps its position and velocity. The spawn point is **not** recomputed, so
 * a caller that resizes before the first step does not get the ball re-thrown --
 * call `cgl_bounce_restart()` if that is what you want.
 *
 * @return false when the area was rejected, in which case the previous area stays
 *         in force (a resize that cannot be honoured must not leave the ball with no
 *         walls at all).
 */
bool cgl_bounce_set_area(cgl_bounce_t *b, cgl_bounce_rect_t area);

/**
 * Where the ball is put back, in px (the position) and Q12.4 per step (the velocity).
 *
 * It does **not** move the ball -- this is where the *next* throw starts, and a setter
 * that also teleported a ball in flight would be a surprise. Use `cgl_bounce_restart()`
 * for that.
 *
 * @note `x_px` / `y_px` are converted to the engine's Q12.4, whose `int16_t` range is
 *       about +-2048 px. A spawn outside the playground is fine (the first
 *       `cgl_bounce_post_step()` clamps it back), but a coordinate beyond the Q12.4
 *       range wraps, so do not pass one.
 */
void cgl_bounce_set_spawn(cgl_bounce_t *b, int16_t x_px, int16_t y_px, int16_t vx_q4, int16_t vy_q4);

/**
 * The **first half** of one physics step: apply gravity (`vy += g`).
 * Must be called before `cgl_phys2d_step()` (semi-implicit Euler -- see the file header).
 */
void cgl_bounce_pre_step(cgl_bounce_t *b);

/**
 * The **second half** of one physics step: four-wall reflection + restitution
 * damping + bounce counting + speed observation. Must be called after
 * `cgl_phys2d_step()`.
 */
void cgl_bounce_post_step(cgl_bounce_t *b);

/** Throw the ball again from the spawn point and clear the counters. */
void cgl_bounce_restart(cgl_bounce_t *b);

/**
 * Ball-centre screen coordinates, in px, **always clamped inside the active area**.
 *
 * The clamp is the point of this function. One step is "integrate, then reflect",
 * and the reflection runs immediately after `cgl_phys2d_step()` -- so *between* those
 * two moments the ball can legitimately be out of bounds, and whether an outside
 * observer ever sees that depends on the order the caller happens to read things in.
 * Rather than relying on that order, the invariant is enforced at the read point:
 * what comes out of here is always a legal position.
 *
 * Self-healing: if a `cgl_phys2d_reset()` wiped the ball out of the pool, this adds
 * it back, so the function returns true whenever the world is usable. It returns
 * false only when `world` is NULL or the pool is exhausted. Note that the parameter
 * is not const -- self-healing modifies the ball.
 */
bool cgl_bounce_position_px(cgl_bounce_t *b, int16_t *x, int16_t *y);

/**
 * The same clamped position, **read-only**: the const counterpart of
 * `cgl_bounce_position_px()`, and the one a painter uses.
 *
 * It differs from its non-const twin in exactly one way: it does **not** self-heal.
 * A painter may not add a body to the world -- it runs inside a `draw` callback, and
 * a body added there would join the pool at a moment nobody planned for. So when the
 * ball has been wiped out by a `cgl_phys2d_reset()` and no step has run since, this
 * returns false and the painter draws nothing for that frame; the next
 * `cgl_bounce_post_step()` puts it back, which is the same frame in every real
 * caller (the walls pass runs before the draw pass).
 *
 * @return false when there is no world, the ball is not in the pool, or it is inactive
 */
bool cgl_bounce_draw_position_px(const cgl_bounce_t *b, int16_t *x, int16_t *y);

/** Cumulative number of wall hits. */
uint16_t cgl_bounce_bounces(const cgl_bounce_t *b);

/**
 * The active area as it was accepted, back in px -- the exact rectangle the walls
 * clamp to, so that a caller can draw the playground, lay something out inside it, or
 * (the reference test) assert that the ball never left it, without re-deriving the
 * arithmetic and drifting from it.
 *
 * @return false when the ball has no accepted area (init rejected it), leaving `area`
 *         untouched
 */
bool cgl_bounce_area_px(const cgl_bounce_t *b, cgl_bounce_rect_t *area);

/** Historical maximum |velocity| per step, Q12.4 (observation: "did it ever blow up"). */
uint16_t cgl_bounce_max_speed_q4(const cgl_bounce_t *b);

/** The ball's pool index right now, or `CGL_BOUNCE_NO_INDEX`. Never cached by the caller. */
uint16_t cgl_bounce_index(const cgl_bounce_t *b);

/** How many areas have been rejected as unusable (see `cgl_bounce_init`). */
uint16_t cgl_bounce_area_rejected(const cgl_bounce_t *b);

#endif /* CGL_BOUNCE_H */
