// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_iso2d_config.h
  * @brief   Compile time knobs of the projection / depth sort layer (`cgl_iso2d`).
  *
  *  Why the knobs are split in two files
  *  ------------------------------------
  *  `cytgl_ext` is two layers with a one-way dependency: `cgl_iso2d` (positions,
  *  projection, depth sort) sits **above** `cgl_phys2d` (bodies, solver) and does not
  *  know what a rigid body is. A single `cgl_phys2d_config.h` holding both layers'
  *  knobs would undo exactly that: the projection layer would be configured by a
  *  header named after the physics layer, and `cgl_iso2d.h` could not be compiled, or
  *  read, without the physics module.
  *
  *  So the knobs live with their layer:
  *
  *      cgl_phys2d_config.h   world domain, broad phase grid, body pool, physics feel
  *      cgl_iso2d_config.h    camera, depth buckets, sort table capacity  <- this file
  *      cgl_ext_basis.h       the Q12.4 convention and `CGL_EXT_INLINE`, shared by both
  *
  *  Overriding did not change: every macro below is still an overridable
  *  `#ifndef`, so the command line (`-DCGL_ISO2D_DEPTH_BUCKETS=172`) and a consumer's
  *  own config header both keep working exactly as before.
  *
  *  WARNING: the values here are compile time RAM/size budgets, not runtime
  *  numbers. `CGL_ISO2D_DEPTH_BUCKETS` alone is 2 B per bucket and
  *  `CGL_ISO2D_MAX_POINTS` is 2 B per slot -- see the sizing note on each.
  ******************************************************************************
  */
#ifndef CGL_ISO2D_CONFIG_H
#define CGL_ISO2D_CONFIG_H

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Depth sort                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * Depth sort resolution: one counting-sort bucket per **screen row**.
 *
 * The sort key is the projected ground y, i.e. a bounded screen coordinate, so
 * this only has to cover the rows the renderer can ever draw. Bodies projected
 * outside are clamped into the two end buckets and then culled when drawn, so
 * over-covering costs RAM only and can never change the draw order.
 *
 * 256 is the shipped default and is deliberately larger than the 172 rows the
 * reference panel has: it is the historical value, it covers any panel up to 256
 * rows, and keeping it means this parameterisation stays **byte-for-byte
 * footprint neutral** (`sizeof(cgl_iso2d_sorter_t)` remains 1580 B: 1056 B of sort
 * table at `CGL_ISO2D_MAX_POINTS` = 528, 512 B of buckets, 12 B of counters). A port
 * that wants the RAM back can set it to the real panel height -- on this project
 * `-DCGL_ISO2D_DEPTH_BUCKETS=172` returns 168 B, exactly the 84 unusable entries at
 * 2 B each. That is a conscious trade rather than a default, because it changes
 * the reported resource usage.
 *
 * WARNING: raising it costs 2 B per bucket and is pointless above 256; the
 * buckets hold "one count per active body" in a `uint16_t`.
 */
#ifndef CGL_ISO2D_DEPTH_BUCKETS
#define CGL_ISO2D_DEPTH_BUCKETS 256
#endif

/**
 * Capacity of the sorter's output table: how many elements one scene may sort.
 *
 * `cgl_iso2d_sorter_t.order[]` holds one `int16_t` index per sorted element, so this
 * is 2 B per slot of static RAM. Elements beyond it are not sorted (see
 * `cgl_iso2d_sort()`), which is why the default is generous rather than tight.
 *
 * **528 is the demo's `CGL_PHYS2D_MAX_BODIES`** (`cgl_phys2d_config.h`): every body of
 * the physics world is drawn, so the sort table has to have room for all of
 * them. The value is repeated here rather than derived, because this header must
 * not depend on the physics layer (that is the whole point of the split) -- and
 * the repetition is safe because it is *gated*: `cgl_iso2d_phys2d.h`, the one header
 * that sees both layers, fails the build with `_Static_assert` when the pool is
 * the larger of the two. A projection-only consumer that does not link `cgl_phys2d`
 * should set it to its own sprite count.
 *
 * WARNING: this is a *safe* default only in the "too big" direction. Lowering it
 * behind the real element count makes the sorter silently ignore the excess --
 * which looks like sprites that never appear.
 */
#ifndef CGL_ISO2D_MAX_POINTS
#define CGL_ISO2D_MAX_POINTS 528
#endif

/* -------------------------------------------------------------------------- */
/* Camera                                                                     */
/* -------------------------------------------------------------------------- */

/** cos(30 deg) Q8: round(0.866025 * 256) = 222. Used by the "textbook 30 deg" style. */
#ifndef CGL_ISO2D_COS30_Q8
#define CGL_ISO2D_COS30_Q8 222
#endif
/** sin(30 deg) Q8: 0.5 -> 128. */
#ifndef CGL_ISO2D_SIN30_Q8
#define CGL_ISO2D_SIN30_Q8 128
#endif
/** sqrt(3) Q8: round(1.7320508 * 256) = 443 (the width/height ratio of the isometric diamond). */
#ifndef CGL_ISO2D_SQRT3_Q8
#define CGL_ISO2D_SQRT3_Q8 443
#endif

/**
 * Lift factor from height to screen y, Q8: 256 = 1 px of height = 1 px of screen
 * displacement. In an isometric projection the z axis is exactly 1:1 with no
 * perspective scaling, so there is no other choice.
 *
 * This is the one camera constant that is genuinely tunable rather than
 * geometric: a smaller value flattens the scene (debris barely rises), a larger
 * one exaggerates it.
 */
#ifndef CGL_ISO2D_LIFT_Q8
#define CGL_ISO2D_LIFT_Q8 256
#endif

/* The consistency gates for these knobs live at the end of `cgl_iso2d.h`: they need
   the derived pieces (`CGL_ISO2D_MAX_POINTS` vs the `int16_t` indices of the sort
   table) and they belong to the layer that has to live with them. */

#endif /* CGL_ISO2D_CONFIG_H */
