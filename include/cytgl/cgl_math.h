// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_math.h
  * @brief   LCD UI framework - integer trigonometry (the one place an angle or a
  *          sine is computed).
  *
  *  Why this module exists
  *  ----------------------
  *  The framework had no trigonometry at all, and for a long time it did not need
  *  any: rectangles, discs and blits are all angle-free. That held right up to the
  *  first **radial** widget -- a dial, a gauge, a speedometer, a radar sweep, a
  *  clock hand -- every one of which is "a point on a circle", and every one of
  *  which therefore needs sin/cos. The alternatives are worse than a table:
  *
  *    - libm's sinf()/cosf() is a link-time dependency the framework refuses on
  *      principle (no libm, see README "Design rules"); on this part it also pulls
  *      the soft-float/64 bit helpers in through the back door.
  *    - hand-rolling `x*x + y*y <= r*r` tests per pixel (the "circle without trig"
  *      trick) works for a **full** circle but cannot answer "where is 10:09 on the
  *      dial" -- a partial arc, a hand and a tick all need the angle -> point map.
  *
  *  So the framework gets one small, honest trig module: a quarter-wave table plus
  *  linear interpolation. 65 int16 entries = 130 bytes of flash, no RAM, no libm.
  *
  *  The angle type: 16 bit, and why not 8
  *  -------------------------------------
  *  `cgl_angle_t` is a fraction of a turn: 65536 = one full turn, i.e. 1 unit =
  *  0.0055 degrees. The framework's optional modules use an **8 bit** angle
  *  (`cgl_particles_cos_q8(uint8_t yaw_u8)`), and for a camera matrix that is
  *  plenty. It is *not* plenty for anything a user watches move: an 8 bit angle is
  *  1.41 degrees per step, and at a 70 px radius that moves the endpoint of a hand
  *  by 1.7 px -- a second hand would visibly jump in ~3 discrete steps per second
  *  instead of sweeping. 16 bit costs one extra byte per angle and makes the step
  *  0.007 px: the sweep is smooth with no interpolation of positions.
  *  (`CGL_ANGLE_FROM_U8()` converts an 8 bit module angle when the two meet, which
  *  is the only place the difference is visible.)
  *
  *  The value type: Q8, and why not Q15
  *  -----------------------------------
  *  Q8 (256 = 1.0) matches the rest of the framework -- `cgl_anim`'s curves,
  *  `cgl_widget_set_alpha`, the warp's `progress_q8` are all Q8 -- and its error is
  *  already below the pixel grid: 0.5/256 of full scale is 0.2 px at a 100 px
  *  radius, so fixed point is *not* what limits a radial drawing. Q15 would buy
  *  two more bits of a quantity that is about to be shifted into an int16 pixel
  *  coordinate anyway, and would make every consumer write `(int32_t)r * v >> 15`
  *  instead of the `>> 8` it already knows.
  *
  *  Convention (screen space, y grows **down**)
  *  -------------------------------------------
  *      angle 0            -> +x (3 o'clock)
  *      angle grows        -> toward +y, i.e. **clockwise on the screen**
  *      angle TURN/4       -> +y (6 o'clock)
  *      angle TURN/2       -> -x (9 o'clock)
  *      angle 3*TURN/4     -> -y (12 o'clock)
  *
  *  This is the usual mathematical parameterisation evaluated in screen
  *  coordinates, so `x = cx + r*cos(a)`, `y = cy + r*sin(a)`, and a clock face is
  *  "12 o'clock is three quarters of a turn". The alternative (0 = up, clockwise)
  *  reads slightly nicer for a clock and is wrong for everything else -- polar
  *  plots, radar, gauge needles -- so the general convention wins and the clock
  *  page carries the one-line offset.
  *
  *  WARNING: a `cgl_angle_t` **wraps at one turn**. That is what makes the type a
  *  turn fraction rather than a length, and it has one consequence worth stating:
  *  "one full turn" cannot be written as a `cgl_angle_t` (it would be 0), which is
  *  why the arc primitive takes its sweep as a wider `uint32_t` -- see
  *  `cgl_canvas_arc()`. `CGL_ANGLE_TURN` is deliberately an `unsigned int` for the
  *  same reason, and is meant to be used in wider arithmetic and cast down.
  *
  *  This module is pure computation: no canvas, no widget, no HAL, no libm.
  ******************************************************************************
  */
#ifndef CGL_MATH_H
#define CGL_MATH_H

#include <stdint.h>
#include <stddef.h>

/**
 * Angle in 1/65536 of a turn (Q16 turn fraction).
 *
 * WARNING: do not confuse this with the 8 bit angles of the optional modules
 * (the headers under `include/ext`); those describe a camera whose steps nobody can
 * see, this describes something a hand sweeps along. Use `CGL_ANGLE_FROM_U8()` at
 * the boundary between the two worlds.
 */
typedef uint16_t cgl_angle_t;

/**
 * One full turn, in angle units.
 *
 * Deliberately **not** cast to `cgl_angle_t`: 65536 does not fit the type (that is
 * the point of a wrapping turn fraction), and a `(cgl_angle_t)CGL_ANGLE_TURN` would
 * silently read as "no turn at all". Use it in `uint32_t` arithmetic -- sweep
 * lengths, "half of the dial", `CGL_ANGLE_TURN / 12` -- and cast the *result*.
 */
#define CGL_ANGLE_TURN 65536u

/** A quarter turn (90 degrees): 6 o'clock in screen coordinates. */
#define CGL_ANGLE_QUARTER ((cgl_angle_t)16384u)

/** A half turn (180 degrees): 9 o'clock in screen coordinates. */
#define CGL_ANGLE_HALF ((cgl_angle_t)32768u)

/** Three quarters of a turn: 12 o'clock / "straight up". */
#define CGL_ANGLE_UP ((cgl_angle_t)49152u)

/**
 * Whole degrees -> angle (fits a `cgl_angle_t` for 0..359; 360 wraps to 0, which is
 * the same direction and therefore harmless).
 */
#define CGL_ANGLE_DEG(d) ((cgl_angle_t)(((uint32_t)(d) * 65536u) / 360u))

/** 8 bit module angle (256 = turn) -> cgl_angle_t. Exactly `a << 8`, no rounding. */
#define CGL_ANGLE_FROM_U8(a) ((cgl_angle_t)((uint16_t)(a) << 8))

/** cgl_angle_t -> 8 bit module angle. Truncates (the target has 1.4 degree steps). */
#define CGL_ANGLE_TO_U8(a) ((uint8_t)((uint16_t)(a) >> 8))

/**
 * The compass / clock reading of an angle in whole degrees, 0..359.
 *
 * Note that this is the *mathematical* reading (0 = +x = 3 o'clock), not a clock
 * face reading: a clock page that wants "7 o'clock" adds `CGL_ANGLE_UP` before
 * converting. It exists for readouts (a HUD, an INFO page, a test that wants to
 * assert "the hand is at 210 degrees"), not for drawing.
 */
uint16_t cgl_angle_deg(cgl_angle_t a);

/**
 * Sine of `a`, Q8: the result is in -256..256 and 256 is exactly 1.0.
 *
 * The table holds one quadrant (65 entries) and the other three are mirrored, so
 * the symmetry costs three branches and no memory. Between two table entries the
 * value is linearly interpolated; at 1.4 degrees apart the chord of a sine is
 * 2.5 LSB in Q15 away from the arc, i.e. 0.01 LSB in Q8 -- the rounding of this
 * function is the table's rounding, not the interpolation's.
 */
int16_t cgl_sin_q8(cgl_angle_t a);

/** Cosine of `a`, Q8 (the same table, a quarter turn ahead: cos(a) = sin(a + 90 deg)). */
int16_t cgl_cos_q8(cgl_angle_t a);

/**
 * Angle + radius -> an offset from the centre, in whole pixels:
 *
 *      dx = round(radius * cos(a) / 256)
 *      dy = round(radius * sin(a) / 256)
 *
 * `radius` may be 0..32767 (the product stays inside int32). Either output may be
 * NULL. This is the one call a radial widget needs, and it lives here rather than
 * in every widget so that "which axis is which" and "where the rounding goes" are
 * answered exactly once.
 *
 * WARNING: the rounding is to nearest, and that matters more than it looks: a
 * truncating `>> 8` would bias both axes toward the origin independently, i.e. a
 * point that belongs on radius r would land up to 1.4 px **inside** it. A bias
 * does not average out -- a ring drawn from those points comes out visibly smaller
 * than its nominal radius. See the measurement in cgl_math.c.
 */
void cgl_math_polar(cgl_angle_t a, int16_t radius, int16_t *dx, int16_t *dy);

/**
 * The same map, in **Q6** (1/64 of a pixel) and around an arbitrary centre: the
 * sub-pixel form of `cgl_math_polar()`.
 *
 *      x_q6 = cx << 6 + (radius * cos(a)) >> 2
 *      y_q6 = cy << 6 + (radius * sin(a)) >> 2
 *
 * Why Q6 and not whole pixels: a sweeping second hand moves 0.12 px per 16 ms
 * frame (6 degrees a second at a 71 px radius), so a whole-pixel endpoint holds
 * still for eight frames and then jumps. `cgl_arc_stroke` consumes exactly this
 * form -- see `cgl_arc.h` -- and rounds the coverage rather than the position,
 * which is what keeps a slowly rotating hand moving instead of ticking.
 *
 * `radius` may be 0..32767; either output may be NULL.
 */
void cgl_math_polar_q6(int16_t cx, int16_t cy, cgl_angle_t a, int16_t radius,
                       int32_t *x_q6, int32_t *y_q6);

/**
 * `(value - vmin) / (vmax - vmin)` as Q8 (0..256), rounded to nearest.
 *
 * The one place the framework turns "a value in a range" into "how far along",
 * shared by `cgl_bar` (a linear fill) and `cgl_arc_gauge` (an angular sweep) so
 * the two can never disagree about where half way is. `vmax <= vmin` is a
 * rejected range and draws empty rather than dividing by zero.
 *
 * Everything is unsigned 32-bit inside: `vmax - vmin` is not a valid `int32_t`
 * for `INT32_MIN..INT32_MAX`, but it is a perfectly good `uint32_t`. The
 * fractional bits come out of a shift/subtract long division rather than
 * `(off << 8) / span`, because the latter overflows for any range wider than
 * 2^23 and `int64` is banned in this project (see the note in cgl_anim.h: it
 * drags `__aeabi_ldivmod` in from libgcc). Nine iterations per draw is nothing;
 * per pixel it would be a disaster, which is why callers compute it once and
 * then multiply.
 */
uint16_t cgl_math_frac_q8(int32_t value, int32_t vmin, int32_t vmax);

/**
 * Integer square root: `floor(sqrt(v))`, no libm and no floating point.
 *
 * It exists because the rasterizer needs it and cannot have `sqrtf()` (see the note
 * at the top of this header). The useful form for a fixed-point caller is that a
 * **Q16** argument yields a **Q8** result, because `sqrt(x * 65536) == sqrt(x) *
 * 256`: `cgl_math_isqrt(r_q8 * r_q8 - dy_q8 * dy_q8)` is the half-width of one row
 * of a circle, in Q8, with no scaling arithmetic at the call site -- which is
 * exactly how `cgl_canvas_arc()` walks its rows.
 *
 * Digit-by-digit (restoring) method: at most 16 iterations of shift / compare /
 * subtract, so it is a known, bounded cost rather than a Newton loop whose iteration
 * count depends on the seed. `tests/arc_host_test.c` checks it against `sqrt()` over
 * every value a 320x172 panel can produce, plus the boundaries (0, 1, the perfect
 * squares, and the uint32 maximum).
 */
uint32_t cgl_math_isqrt(uint32_t v);

#endif /* CGL_MATH_H */
