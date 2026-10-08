// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_particles_config.h
  * @brief   cgl_particles - compile time configuration: the off switch, and the
  *          sizes that decide the RAM bill.
  *
  *  What this module is
  *  -------------------
  *  A fixed point (Q8.8) particle cloud with an orbiting perspective camera, a
  *  counting-sort depth order and a band renderer -- the "bullet time" effect of
  *  the reference demo, extracted so that another application can use it without
  *  copying 800 lines of integer maths.
  *
  *  It comes in two halves, and the split is the point (see the CytGL manual, section 15.1 for
  *  the same rule in the physics module):
  *
  *      cgl_particles_view.[ch]   the widget: knows cgl_canvas_t and the widget tree
  *              |
  *      cgl_particles.[ch]        the engine: simulation, camera, projection, sort.
  *                                Includes nothing but <stdint.h>/<stdbool.h>/<stddef.h>,
  *                                so it can be built and unit tested with no UI
  *                                framework in the graph at all.
  *
  *  **Not linked = 0 bytes.** The core library never lists this module and never
  *  includes it, so a build that does not ask for it pays nothing in flash and
  *  nothing in RAM. The module also owns no static storage at all: every byte it
  *  uses is either in a struct the caller declared or on the caller's stack.
  *
  *  Turning it off when the sources ARE compiled
  *  --------------------------------------------
  *  Opt-in linking is the normal way to not pay for this module, and it needs no
  *  macro. There is one case where it is not enough: a build that collects its
  *  sources per **directory** (this repository's own firmware build does exactly
  *  that -- see `CytGL.rproj` / `tools/verify_firmware_build.ps1`) compiles every
  *  `.c` under `src/ext/` whether or not anybody calls into it. For that build,
  *  `CGL_CFG_PARTICLES 0` compiles the engine and the view down to empty
  *  translation units:
  *
  *      cmake --build ... -DCMAKE_C_FLAGS=-DCGL_CFG_PARTICLES=0
  *      # or, for the firmware script: add the define to its $cflags
  *
  *  The application side has to co-operate for the RAM to actually come back: a
  *  page that declares `cgl_particles_storage_t` / `cgl_particles_t` as a static
  *  member still pays for those bytes even when the engine is compiled out. The
  *  reference application guards its bullet time page with the same macro (see
  *  `CGL_CFG_APP_FX_PARTICLES` there), which is where the ~6 KB of pool RAM lives.
  *
  *  Q8.8, deliberately not the physics module's Q12.4
  *  ------------------------------------------------
  *  This engine reads no `cgl_phys2d` type and writes no `cgl_phys2d` field. The
  *  two fixed point formats answer opposite requirements -- Q12.4 reaches +-2048 px
  *  but resolves only 1/16 px, Q8.8 resolves 1/256 px but reaches only +-128 px --
  *  and the camera motion here is sub-pixel smooth while the collision world is
  *  large. See the note in `cgl_particles.h` for why the whole engine is built
  *  around "resolution matters, reach does not".
  ******************************************************************************
  */
#ifndef CGL_PARTICLES_CONFIG_H
#define CGL_PARTICLES_CONFIG_H

/* --------------------------------------------------------------------------
 * 1. The off switch
 *
 * 1 (the default) = the engine and the view are compiled normally.
 * 0 = both translation units compile to nothing at all.
 *
 * WARNING: this is a **build-wide** switch, in the same sense as
 * `CGL_PHYS2D_USER_STORAGE`: a translation unit that sees 0 and another that sees
 * 1 cannot share a `cgl_particles_t`. Put it in the build system, not in one file's
 * local defines.
 * ------------------------------------------------------------------------ */
#ifndef CGL_CFG_PARTICLES
#define CGL_CFG_PARTICLES 1
#endif

/* --------------------------------------------------------------------------
 * 2. Screen size the default camera geometry is checked against
 *
 * If <cytgl/cytgl_config.h> has already been included, `CGL_CFG_SCREEN_W/H` is used,
 * so retargeting the framework retargets this check with it -- there is exactly one
 * source of truth for panel geometry in this project and a second, hand-kept copy
 * would silently go stale.
 *
 * The include is **optional on purpose**: the engine and its host tests build with
 * no framework header anywhere in the graph, and then the reference panel of the
 * demo is used.
 * ------------------------------------------------------------------------ */
#if defined(CGL_CFG_SCREEN_W) && defined(CGL_CFG_SCREEN_H)
#define CGL_PARTICLES_SCREEN_W CGL_CFG_SCREEN_W
#define CGL_PARTICLES_SCREEN_H CGL_CFG_SCREEN_H
#else
#define CGL_PARTICLES_SCREEN_W 320
#define CGL_PARTICLES_SCREEN_H 172
#endif

/* --------------------------------------------------------------------------
 * 3. Pool and sort table -- the RAM bill
 *
 * CGL_PARTICLES_MAX      particles the pool holds, 12 B each. This is the number
 *                        that dominates: 500 particles = 6.0 KB.
 * CGL_PARTICLES_BUCKETS  depth buckets of the counting sort, 2 B each, plus 2 B per
 *                        particle of sort index. 128 buckets over the depth window
 *                        is 2 px of depth resolution, finer than the 3 px dot a
 *                        particle is drawn as, so the quantisation can never
 *                        reorder two particles visibly.
 *
 * Both are overridable `#ifndef`, so a scene that needs fewer particles pays less
 * without editing the module. `CGL_PARTICLES_STORAGE_BYTES(n)` in cgl_particles.h
 * states the byte count as one expression, and the bridge between the two is
 * `_Static_assert`ed there -- a storage gate that is stricter than the storage it
 * gates fails closed and silently, which is worse than no gate.
 *
 * WARNING: changing either of these changes the **sort and the effect** as well as
 * the size: `CGL_PARTICLES_BUCKETS` sets the depth quantisation, and the pool size
 * decides how much debris the burst can spray.
 * ------------------------------------------------------------------------ */
#ifndef CGL_PARTICLES_MAX
#define CGL_PARTICLES_MAX 500
#endif

#ifndef CGL_PARTICLES_BUCKETS
#define CGL_PARTICLES_BUCKETS 128
#endif

_Static_assert(CGL_PARTICLES_MAX <= 32767,
               "CGL_PARTICLES_MAX must fit the int16_t indices the sort table uses");
_Static_assert((CGL_PARTICLES_BUCKETS >= 2) && (CGL_PARTICLES_BUCKETS <= 255),
               "CGL_PARTICLES_BUCKETS is stored in a uint8_t per particle");

#endif /* CGL_PARTICLES_CONFIG_H */
