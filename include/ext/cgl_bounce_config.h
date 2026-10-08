// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_bounce_config.h
  * @brief   cgl_bounce - compile time configuration for the "gravity ball" effect.
  *
  *  What this module is
  *  -------------------
  *  A ball that falls under gravity and bounces off the four walls of a rectangle,
  *  forever, as a UI widget. It is the third effect of the optional module, next to
  *  the shatter scene (`cgl_debris`) and the particle cloud (`cgl_particles`), and
  *  it is by far the smallest of the three.
  *
  *  The physics of "throw a ball" is the one thing `cgl_phys2d` deliberately does
  *  **not** have: the engine is a collision solver with no forces, no walls and no
  *  gravity (see its header). So a caller that wants a ball to fall has to write
  *  `vy += g` before every step and reflect off four walls after it -- thirty lines
  *  that every gravity-ball toy writes the same way, and the reference application
  *  used to carry them in `App/app_bounce_ball.c`. This module is those thirty lines
  *  with the tuning measured and the traps documented.
  *
  *  It comes in two halves, the same way the other two effects do:
  *
  *      cgl_bounce_view.[ch]   the drawing: one disc, culled to the band
  *              |
  *      cgl_bounce.[ch]        the motion: gravity, four walls, restitution,
  *                             self-healing. Framework free -- it includes
  *                             cgl_phys2d and nothing that knows what a canvas is
  *
  *  **Not linked = 0 bytes**, exactly as for the rest of the module: the core never
  *  lists this effect, and opt-in linking is what keeps a UI-only build free of it.
  *  `CGL_CFG_BOUNCE 0` additionally empties both translation units, for a build that
  *  collects its sources per directory (this repository's own firmware build does);
  *  see `cgl_particles_config.h` for the full reasoning -- it is the same trade.
  *
  *  WARNING: the switch is build-wide. Two translation units that disagree about it
  *  cannot share a `cgl_bounce_t`.
  ******************************************************************************
  */
#ifndef CGL_BOUNCE_CONFIG_H
#define CGL_BOUNCE_CONFIG_H

/* The Q12.4 convention (CGL_EXT_Q4_ONE) and the physics knobs the gates below
   quote. Nothing is used from either header at this point -- macro bodies expand
   lazily -- but naming them here is what keeps this file reading the Q4 constants
   from the one place that defines them instead of spelling `<< 4` again. */
#include <ext/cgl_ext_basis.h>
#include <ext/cgl_phys2d_config.h>

/* --------------------------------------------------------------------------
 * 1. The off switch
 * ------------------------------------------------------------------------ */
#ifndef CGL_CFG_BOUNCE
#define CGL_CFG_BOUNCE 1
#endif

/* --------------------------------------------------------------------------
 * 2. The ball
 *
 * One radius for both jobs (collision and drawing). They are deliberately not two
 * knobs: a ball whose painted disc is bigger than its collision circle reads as
 * "it bounced before it touched the wall", and the gap is exactly the kind of
 * thing that survives review because both numbers look reasonable on their own.
 * ------------------------------------------------------------------------ */
#ifndef CGL_BOUNCE_RADIUS_PX
#define CGL_BOUNCE_RADIUS_PX 3
#endif

/** Collision radius, Q12.4. */
#define CGL_BOUNCE_R_Q4 (CGL_BOUNCE_RADIUS_PX * CGL_EXT_Q4_ONE)

/* --------------------------------------------------------------------------
 * 3. Gravity and the spawn
 *
 * Gravity is **not** written into the engine; every step adds it by hand
 * (`cgl_bounce_pre_step()`), and it is added *before* the integration, which
 * makes the integrator semi-implicit Euler rather than explicit. That is not a
 * detail: explicit Euler on a bouncing ball gains energy at the top of the arc,
 * and the ball ends up a few pixels higher on every bounce.
 *
 * The value is per **step**, not per second, because the module has no clock at
 * all (see the CytGL manual, section 15.9). At the reference 16 ms logical step,
 * 0.2 px/step^2 is ~780 px/s^2.
 * ------------------------------------------------------------------------ */
#ifndef CGL_BOUNCE_GRAVITY_Q4
#define CGL_BOUNCE_GRAVITY_Q4 (CGL_EXT_Q4_ONE / 5) /**< 0.2 px/step^2 */
#endif

/** How much speed survives a wall hit, Q8 (256 = 1.0 = perfectly elastic).
 *
 *  It has to be fairly high for a viewport-sized playground: the energy lost per
 *  bounce is on the order of (1 - e^2), so with e = 0.85 the reference ball sticks
 *  to the floor after two or three bounces (measured: only 2 bounces in 20 s).
 *  240 ~ 0.9375 is what keeps it crossing the whole area for as long as you watch.
 *  Lower it deliberately to make the ball settle. */
#ifndef CGL_BOUNCE_RESTITUTION_Q8
#define CGL_BOUNCE_RESTITUTION_Q8 240u
#endif

/* The spawn, as `cgl_bounce_init()` derives it from the active area:
 *
 *     x = area.x0 + CGL_BOUNCE_SPAWN_INSET_PX
 *     y = area.y0                       (hanging from the ceiling)
 *     vx = +CGL_BOUNCE_SPAWN_VX_Q4      (tossed to the right)
 *     vy =  CGL_BOUNCE_SPAWN_VY_Q4      (a slight extra upward toss first)
 *
 * Relative to the area rather than absolute pixels, so that "throw the ball from
 * the top left" stays true after the caller moves or resizes the playground. */
#ifndef CGL_BOUNCE_SPAWN_INSET_PX
#define CGL_BOUNCE_SPAWN_INSET_PX 7
#endif
#ifndef CGL_BOUNCE_SPAWN_VX_Q4
#define CGL_BOUNCE_SPAWN_VX_Q4 (4 * CGL_EXT_Q4_ONE) /**< 4 px/step to the right */
#endif
#ifndef CGL_BOUNCE_SPAWN_VY_Q4
#define CGL_BOUNCE_SPAWN_VY_Q4 (-(CGL_EXT_Q4_ONE / 2)) /**< -0.5 px/step, i.e. up */
#endif

/** The painted radius, px. Equal to `CGL_BOUNCE_RADIUS_PX` by construction; kept
    as its own name so a caller reading the view code does not have to know that
    the collision knob is also the drawing knob. */
#define CGL_BOUNCE_DRAW_R_PX CGL_BOUNCE_RADIUS_PX

/* --------------------------------------------------------------------------
 * 4. Gates
 *
 * `_Static_assert`/`#error` rather than silent clamping, for the reason
 * the CytGL manual, section 15.9 states: a knob set to something impossible has to stop the
 * build with a sentence, not show up as "the effect looks a bit odd".
 * ------------------------------------------------------------------------ */

/* A restitution above 1.0 is an energy *source*: the ball gains speed on every
   wall hit until it hits CGL_PHYS2D_MAX_VELOCITY, and because the reflection
   clamps the *position* back inside the area each time, the visible symptom is a
   ball that vibrates in a corner rather than one that leaves the screen. */
#if CGL_BOUNCE_RESTITUTION_Q8 > 256u
#error "CGL_BOUNCE_RESTITUTION_Q8 > 256 adds energy on every bounce: the ball accelerates until it saturates CGL_PHYS2D_MAX_VELOCITY"
#endif

_Static_assert(CGL_BOUNCE_RADIUS_PX >= 1, "CGL_BOUNCE_RADIUS_PX must be at least 1 px");
_Static_assert(CGL_BOUNCE_GRAVITY_Q4 > 0, "CGL_BOUNCE_GRAVITY_Q4 must be positive: the ball falls down the +y axis");

#endif /* CGL_BOUNCE_CONFIG_H */
