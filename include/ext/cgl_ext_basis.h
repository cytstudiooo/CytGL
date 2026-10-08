// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_ext_basis.h
  * @brief   The basis both layers of the `cytgl_ext` module are written in:
  *          the Q12.4 fixed point convention and the force-inline shim.
  *
  *  Why this file exists
  *  --------------------
  *  `cytgl_ext` is deliberately two layers with a one-way dependency:
  *
  *      cgl_iso2d   (positions, projection, depth sort -- knows NO rigid bodies)
  *        |
  *      cgl_phys2d  (bodies, broad phase grid, solver, vertical axis)
  *
  *  Two things are genuinely *shared* by both of them rather than owned by
  *  either, and this header is where they live:
  *
  *    1. **The fixed point format** (`CGL_EXT_Q4_*`). Both layers speak Q12.4
  *       (1 unit = 1/16 px): the solver integrates in it, and the projection
  *       matrix is applied to it. Defining it twice would be the classic way to
  *       get the two out of step, and the failure mode is silent -- a body that
  *       is drawn at 1/16 of its position.
  *    2. **`CGL_EXT_INLINE`**, the force-inline shim for the hot-path accessors of
  *       both headers.
  *
  *  Note that this is *not* a place for physics or projection knobs: those are
  *  per-layer and live in `cgl_phys2d_config.h` / `cgl_iso2d_config.h`.
  *
  *  The module still includes nothing but the C standard library. That is what
  *  lets it be built, unit-tested and reused with no UI framework present, and
  *  it is why the module can be an optional target rather than a core feature.
  *
  *  About CGL_EXT_INLINE
  *  ----------------
  *  The hot-path accessors (`cgl_phys2d_to_px`, `cgl_iso2d_ground_x/y`, `cgl_iso2d_project`)
  *  are declared force-inlined so that the *shape* of that hot path does not
  *  depend on the build options: the target firmware is compiled with
  *  `-fno-inline` (nicer call stacks while debugging) and the renderer calls
  *  these twice per body per band.
  *
  *  Why not reuse the framework's `CGL_AINLINE`
  *  ------------------------------------------
  *  `cgl_types.h` defines exactly this macro, and reusing it would have saved ten
  *  lines -- but it would pull a framework header into a module whose whole
  *  contract is "includes nothing but the C standard library". A ten-line shim
  *  is a much smaller price than a dependency on the framework's type header.
  *
  *  WARNING: if the framework's definition is ever revised, revise this one too.
  *  The two are intentionally identical in behaviour, and a one-shot check is
  *  cheap -- compile `cgl_phys2d.c` with `-DCGL_CFG_CGL_PHYS2D_PORT=1` and diff the
  *  object files.
  *
  *  Measured cost of the attribute (arm-none-eabi-gcc 14.3, cortex-m4, this
  *  module): with `-O2` it changes nothing at all, because the optimiser inlines
  *  these bodies anyway. Under `-fno-inline` it removes three emitted helper
  *  symbols from `cgl_iso2d.c` and shrinks it by 48 B (`-O2`) / 68 B (`-Og`), and it
  *  changes nothing measurable in `cgl_phys2d.c`. So this is purely a code-size knob:
  *  dropping the attribute (which is what a compiler without the extension gets)
  *  is a performance/size fallback, never a correctness problem.
  ******************************************************************************
  */
#ifndef EXT_BASIS_H
#define EXT_BASIS_H

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Q12.4 fixed point: the module's number format                             */
/* -------------------------------------------------------------------------- */

/**
 * Fraction bits of the module's fixed point format: 4, i.e. 1 unit = 1/16 px.
 *
 * Range for the `int16_t` coordinates both layers use: about -2048 .. +2047 px
 * in steps of 1/16 px, which is why the world geometry is pinned by the
 * `_Static_assert`s in `cgl_phys2d.h` rather than allowed to grow.
 */
#define CGL_EXT_Q4_SHIFT 4
/** One pixel in Q12.4. */
#define CGL_EXT_Q4_ONE (1 << CGL_EXT_Q4_SHIFT)

/* -------------------------------------------------------------------------- */
/* Force-inlined hot path                                                     */
/* -------------------------------------------------------------------------- */

/*
 * Detection order matters:
 *   - the framework's own macro wins when it is already visible, so a consumer
 *     that includes both cannot end up with two different definitions;
 *   - `__GNUC__` is checked before `_MSC_VER`: clang defines both, and it must
 *     get the GCC style attribute, not `__forceinline`;
 *   - `__has_attribute` is then used to prove the extension is really available,
 *     rather than assuming every compiler that defines `__GNUC__` accepts it;
 *   - only then MSVC, whose `__forceinline` works at `/Od` as well (and is
 *     ignored with a warning under `/clr`, where no form of forced inlining is
 *     possible anyway);
 *   - anything else silently degrades to a plain `static inline`. That is
 *     deliberately not an `#error`: `inline` is a hint, so an unknown compiler
 *     gets correct code that is merely larger, which is exactly the trade this
 *     module wants. Forcing a compile error here would lock the module to two
 *     compiler families for no functional reason.
 */
#if defined(CGL_AINLINE)
#define CGL_EXT_INLINE CGL_AINLINE
#elif defined(__GNUC__) || defined(__clang__)
#if defined(__has_attribute) && __has_attribute(always_inline)
#define CGL_EXT_INLINE static inline __attribute__((always_inline))
#else
#define CGL_EXT_INLINE static inline
#endif
#elif defined(_MSC_VER)
#define CGL_EXT_INLINE static __forceinline
#else
#define CGL_EXT_INLINE static inline
#endif

#endif /* EXT_BASIS_H */
