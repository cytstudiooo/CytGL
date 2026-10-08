// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_phys2d_config.h
  * @brief   Compile time knobs of the physics layer (`cgl_phys2d`) and of the world
  *          it simulates.
  *
  *  Where the knobs of the module live
  *  ----------------------------------
  *  `cytgl_ext` is two layers with a one-way dependency -- `cgl_iso2d` (positions,
  *  projection, depth sort) sits above `cgl_phys2d` (bodies, solver) and knows no
  *  bodies -- so its knobs are split the same way:
  *
  *      cgl_phys2d_config.h   world domain, broad phase grid, body pool, physics feel
  *      cgl_iso2d_config.h    camera, depth buckets, sort table capacity
  *      cgl_ext_basis.h       the Q12.4 convention and `CGL_EXT_INLINE`, shared by both
  *
  *  This file is the physics half. It used to hold the camera constants as well;
  *  they moved to `cgl_iso2d_config.h` so that the projection layer does not have to
  *  be configured by a header named after the physics layer.
  *  Overriding is unchanged in both
  *  directions: every macro is an overridable `#ifndef`, so the command line
  *  works the same as before, and a project that only wants the projection half
  *  never includes this file at all.
  *
  *  Why this file exists
  *  --------------------
  *  `cgl_phys2d.[ch]` and `cgl_iso2d.[ch]` are a **standalone, optional module**
  *  (their code lives in `src/ext/`, their public API in `include/ext/`, reached as
  *  `<ext/...>`):
  *  they are not part of the CytGL framework (see the CytGL manual, section 15.1), they
  *  include nothing but `<stdint.h>` / `<stdbool.h>`, and an application that
  *  does not link them pays 0 bytes. What they do owe the framework is the same
  *  discipline every framework knob follows: **nothing that depends on the panel
  *  size or on the memory budget may be a bare literal in the code.**
  *
  *  How to override
  *  ---------------
  *  Either define the macro before the header is first included, or put it on
  *  the command line:
  *
  *      -DCGL_PHYS2D_MAX_BODIES=256 -DCGL_PHYS2D_WORLD_MARGIN_PX=16
  *
  *  Screen size is picked up automatically
  *  --------------------------------------
  *  If `<cytgl/cytgl_config.h>` is reachable, `CGL_CFG_SCREEN_W/H` is used as the
  *  default for "the panel this world is sized for", so retargeting the framework
  *  retargets the simulation domain with it. The include is **optional on
  *  purpose**: the host tests and any other consumer that uses this module next
  *  to a different (or no) UI toolkit keeps the documented 320x172 fallback and
  *  still compiles.
  *
  *  WARNING: the pool sizes here are compile time RAM budgets, not runtime
  *  numbers. `CGL_PHYS2D_MAX_BODIES` alone is 16 B + 2 B per entry, and
  *  `CGL_PHYS2D_GRID_CELLS` is 2 B per cell -- see the sizing table in `cgl_phys2d.h`
  *  before trimming either one. Changing `CGL_PHYS2D_MAX_BODIES` also has to be
  *  reflected in `CGL_ISO2D_MAX_POINTS` (`cgl_iso2d_config.h`), which the build checks.
  ******************************************************************************
  */
#ifndef CGL_PHYS2D_CONFIG_H
#define CGL_PHYS2D_CONFIG_H

#include <stdint.h>

/* The Q12.4 convention (`CGL_EXT_Q4_*`) is shared with the projection layer; it is
   defined once, in the module's basis header. */
#include <ext/cgl_ext_basis.h>

/* -------------------------------------------------------------------------- */
/* Panel the world is sized for                                               */
/* -------------------------------------------------------------------------- */

/**
 * Screen the default world rectangle is derived from, in px.
 *
 * Taken from the framework's own configuration when it is reachable, because
 * there is exactly one source of truth for panel geometry in this project
 * (`CGL_CFG_SCREEN_W/H`) and a second, hand-kept copy would silently go stale.
 * When it is not reachable (host tests of this module in isolation) the
 * reference panel of this demo is used, and the world below therefore comes out
 * at exactly the documented 400 x 256.
 *
 * WARNING: these are defaults, not an assertion. A consumer that wants a world
 * independent of the panel should override `CGL_PHYS2D_WORLD_*` directly.
 */
#if defined(CGL_CFG_SCREEN_W) && defined(CGL_CFG_SCREEN_H)
#define CGL_PHYS2D_SCREEN_W CGL_CFG_SCREEN_W
#define CGL_PHYS2D_SCREEN_H CGL_CFG_SCREEN_H
#else
#define CGL_PHYS2D_SCREEN_W 320
#define CGL_PHYS2D_SCREEN_H 172
#endif

/* -------------------------------------------------------------------------- */
/* Simulation domain                                                          */
/* -------------------------------------------------------------------------- */

/**
 * Simulation domain, in px: the panel plus a margin, plus the offset of that
 * rectangle relative to the panel's top left corner.
 *
 * WARNING: `CGL_PHYS2D_WORLD_*_PX` and `CGL_PHYS2D_WORLD_*0_PX` are the PRIMARY knobs,
 * and the origin is **stated, not derived from the size**. The obvious-looking
 * derivation
 *
 *     CGL_PHYS2D_WORLD_Y0 = -(CGL_PHYS2D_WORLD_H_PX - CGL_PHYS2D_SCREEN_H) / 2
 *
 * is wrong for the shipped geometry and was a real defect while this file was
 * being written: the reference world is -40..216, while centring a 256 px world
 * on a 172 px panel gives -42..214. The margin was never symmetric -- it is 40 px
 * on every side, i.e. the 4 px of "extra" world all sit at the bottom. Shifting
 * the domain by 2 px moved every body to a different broad phase cell index,
 * changed the contact resolution order, and measurably changed the simulation
 * (the 20 s physics soak went from 12.5 px to 13.1 px peak height). Deriving the
 * origin silently re-tunes the physics, so it is written out instead.
 *
 * Bodies that leave the domain are deactivated for good, so it only has to exceed
 * the panel by "as much as a body can travel between two culls": 40 px is far
 * more than the 8 px/step velocity ceiling (`CGL_PHYS2D_MAX_VELOCITY`) needs, and the
 * margin is nearly free -- the domain is 400x256 = 102 400 px^2 against the
 * 320x172 = 55 040 px^2 that is ever visible.
 */
#ifndef CGL_PHYS2D_WORLD_W_PX
#define CGL_PHYS2D_WORLD_W_PX 400
#endif
#ifndef CGL_PHYS2D_WORLD_H_PX
#define CGL_PHYS2D_WORLD_H_PX 256
#endif
#ifndef CGL_PHYS2D_WORLD_X0_PX
#define CGL_PHYS2D_WORLD_X0_PX (-40)
#endif
#ifndef CGL_PHYS2D_WORLD_Y0_PX
#define CGL_PHYS2D_WORLD_Y0_PX (-40)
#endif

/** Left edge of the simulation domain relative to screen x = 0, Q12.4.
 *  WARNING: written as `-(positive << 4)`, not `negative << 4` -- left shifting a
 *  negative value is implementation defined and GCC warns about it
 *  (`-Wshift-negative-value`), which the firmware build turns into noise on every
 *  translation unit that includes this header. */
#define CGL_PHYS2D_WORLD_X0 (-((-CGL_PHYS2D_WORLD_X0_PX) << 4))
/** Top edge of the simulation domain relative to screen y = 0, Q12.4. */
#define CGL_PHYS2D_WORLD_Y0 (-((-CGL_PHYS2D_WORLD_Y0_PX) << 4))
/** Domain width, Q12.4. */
#define CGL_PHYS2D_WORLD_W (CGL_PHYS2D_WORLD_W_PX << 4)
/** Domain height, Q12.4. */
#define CGL_PHYS2D_WORLD_H (CGL_PHYS2D_WORLD_H_PX << 4)

/** Margins between the panel edge and the domain edge, px, for diagnostics and
 *  for the consistency gates in `cgl_phys2d.h`. The two axes of the reference
 *  geometry genuinely differ (40 px left/right, 42 px top/bottom), which is
 *  exactly why the origin above is stated in absolute terms. */
#define CGL_PHYS2D_WORLD_MARGIN_L_PX (-CGL_PHYS2D_WORLD_X0_PX)
#define CGL_PHYS2D_WORLD_MARGIN_T_PX (-CGL_PHYS2D_WORLD_Y0_PX)
#define CGL_PHYS2D_WORLD_MARGIN_R_PX (CGL_PHYS2D_WORLD_W_PX + CGL_PHYS2D_WORLD_X0_PX - CGL_PHYS2D_SCREEN_W)
#define CGL_PHYS2D_WORLD_MARGIN_B_PX (CGL_PHYS2D_WORLD_H_PX + CGL_PHYS2D_WORLD_Y0_PX - CGL_PHYS2D_SCREEN_H)

/* -------------------------------------------------------------------------- */
/* Broad phase grid                                                           */
/* -------------------------------------------------------------------------- */

/**
 * Broad phase cell size, as the **Q12.4 shift** of one cell.
 *
 * The shift is the primary knob and the pixel size is derived, not the other way
 * round, and that is deliberate: the cell index is computed with a shift in
 * `cgl_phys2d__cell_index()`, so the shift is what the code needs, and any
 * "pixel size -> log2" conversion would be a hand-written `?:` ladder that has to
 * be kept in step with it by hand. `CGL_PHYS2D_CELL_PX` is provided for readable
 * diagnostics and is checked against the shift by a `_Static_assert`.
 *
 * 7 = 8 px cells (the reference geometry).
 *
 * WARNING: this is not a free knob. The physics tuning -- the 5-neighbour broad
 * phase scan and the "only neighbours are ever offered" assumption inside
 * `cgl_phys2d__resolve()` -- was measured against 8 px cells. Changing it changes how
 * many pairs reach the narrow phase.
 *
 * Smaller -> fewer false pairs, more `cell_head` RAM (2 B per cell).
 * Bigger  -> less RAM, more pairs offered to the narrow phase.
 */
#ifndef CGL_PHYS2D_CELL_SHIFT
#define CGL_PHYS2D_CELL_SHIFT 7
#endif

/** Broad phase cell size in pixels, for diagnostics: the Q12.4 shift of 7 is
 *  `1 << 7 = 128` in Q12.4, i.e. 8 px. */
#define CGL_PHYS2D_CELL_PX ((1 << CGL_PHYS2D_CELL_SHIFT) >> 4)
/** Q12.4 size of one grid cell (`CGL_PHYS2D_CELL_PX << 4`). This is the value that
 *  used to be the literal 128 (8 px). */
#define CGL_PHYS2D_CELL_Q4 (1 << CGL_PHYS2D_CELL_SHIFT)
/** The same size, evaluated from the pixel size instead of the shift, so the
 *  consistency gate in `cgl_phys2d.h` is a real cross-check and not a tautology. */
#define CGL_PHYS2D_CELL_Q4_FROM_SHIFT (CGL_PHYS2D_CELL_PX << 4)

/* -------------------------------------------------------------------------- */
/* Body pool                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * Body pool size.
 *
 * Was 1056, trimmed to 640 when the vertical axis added 4 B per body, then to
 * **528** when the group-opacity scratch ate 5 KB of RAM: keeping 640 would have
 * left only 0.6 KB unallocated on this 32 KB MCU.
 *
 * The scene needs 480 dots + 2 balls = 483, so 528 leaves 9 % headroom (was
 * 32 %). `tools/mklogo.py` reads this value and refuses to emit a bitmap that no
 * longer fits, so raising the logo resolution fails loudly instead of
 * overflowing at runtime.
 *
 * WARNING: do not just look at "the scene uses 483" and start trimming further:
 * `cgl_bounce` borrows this world to add one more rigid body, and it re-adds it
 * itself after a `cgl_phys2d_reset()` on every rebuild, with
 * flying debris in between. Below about 500, `cgl_phys2d_add` starts returning -1
 * (silently missing dots in the matrix).
 * If you really need more RAM, tune `CGL_CFG_BAND_H` or `CGL_CFG_ALPHA_LEVELS`
 * first.
 */
#ifndef CGL_PHYS2D_MAX_BODIES
#define CGL_PHYS2D_MAX_BODIES 528
#endif

/* -------------------------------------------------------------------------- */
/* Physics tuning                                                             */
/* -------------------------------------------------------------------------- */

/** Longest velocity component, in Q12.4 units per step (8 px/step). */
#ifndef CGL_PHYS2D_MAX_VELOCITY_PX
#define CGL_PHYS2D_MAX_VELOCITY_PX 8
#endif
#define CGL_PHYS2D_MAX_VELOCITY (CGL_PHYS2D_MAX_VELOCITY_PX * (1 << 4))

/* --- vertical (2.5D) axis tuning ----------------------------------------- */

/** Gravity of the vertical axis: 1 px/step^2. At 60 Hz that is ~3600 px/s^2,
 *  which pops a hard hit dot ~30 px up and brings it back within ~0.3 s. */
#ifndef CGL_PHYS2D_GRAVITY_Z_PX
#define CGL_PHYS2D_GRAVITY_Z_PX 1
#endif
#define CGL_PHYS2D_GRAVITY_Z_Q4 (CGL_PHYS2D_GRAVITY_Z_PX * (1 << 4))

/** Cap on the launch speed, Q12.4 per step. */
#ifndef CGL_PHYS2D_MAX_VZ
#define CGL_PHYS2D_MAX_VZ CGL_PHYS2D_MAX_VELOCITY
#endif

/** Plane speed below which a body stays on the floor, Q12.4 per step. */
#ifndef CGL_PHYS2D_LAUNCH_MIN_Q4
#define CGL_PHYS2D_LAUNCH_MIN_Q4 (1 << 4)
#endif

/**
 * vz = (plane speed - LAUNCH_MIN) * LAUNCH_GAIN, Q8 (384 = 1.5).
 *
 * Semi-implicit Euler loses one gravity step at take off, so the apex is about
 * vz^2/(2g) - vz/2. With g = 1 px/step^2 and this gain the useful range is:
 *   plane 2 px/step -> apex ~0.4 px  (a nudge barely lifts)
 *   plane 4 px/step -> apex ~9 px
 *   plane 8 px/step -> apex ~28 px   (a direct hit by the wrecking ball)
 */
#ifndef CGL_PHYS2D_LAUNCH_GAIN_Q8
#define CGL_PHYS2D_LAUNCH_GAIN_Q8 384u
#endif

/** Landing keeps this fraction of the vertical speed, Q8 (64 = 25%). */
#ifndef CGL_PHYS2D_LAND_REST_Q8
#define CGL_PHYS2D_LAND_REST_Q8 64u
#endif

/** Impact speeds below this are absorbed instead of bounced, Q12.4. It is what
 *  keeps a body lying on the floor perfectly still instead of buzzing at
 *  sub-pixel heights. */
#ifndef CGL_PHYS2D_LAND_MIN_Q4
#define CGL_PHYS2D_LAND_MIN_Q4 ((1 << 4) / 2)
#endif

/* -------------------------------------------------------------------------- */
/* The camera constants of the projection layer are NOT here any more:          */
/* `CGL_ISO2D_DEPTH_BUCKETS` / `CGL_ISO2D_MAX_POINTS` / `CGL_ISO2D_COS30_Q8` /              */
/* `CGL_ISO2D_SIN30_Q8` / `CGL_ISO2D_SQRT3_Q8` / `CGL_ISO2D_LIFT_Q8` live in                */
/* `cgl_iso2d_config.h`. Nothing in this file refers to them, and nothing in        */
/* `cgl_iso2d.h` refers to this file -- that separation is the point of the split, */
/* and it is what lets the projection half be used with no physics engine at all.  */
/* -------------------------------------------------------------------------- */

/* Consistency gates live at the end of `cgl_phys2d.h`: they need the derived
   `CGL_PHYS2D_GRID_*` / `CGL_PHYS2D_ONE`, which do not exist yet at this point. */

#endif /* CGL_PHYS2D_CONFIG_H */
