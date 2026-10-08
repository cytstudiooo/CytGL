// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_particles.h
  * @brief   Bullet time (time freeze + orbiting camera) particle engine, pure
  *          integer. Data structures, fixed point tools, camera and projection.
  *
  *  What this file is
  *  -----------------
  *  A second, **much smaller** pseudo 3D engine than `src/ext/cgl_iso2d.c`, written for
  *  one effect that `cgl_iso2d` structurally cannot do:
  *
  *      the camera **orbits** (rotates about the world Y axis, a full 360 deg)
  *      while looking at the same frozen cloud of particles.
  *
  *  `cgl_iso2d` is a fixed orthographic isometric projection: its 2x2 matrix is
  *  fitted once to the world rectangle and never changes, and its inverse
  *  (screen -> world) does not exist, so "walk the camera around the scene" is
  *  not expressible in it. Rather than bend `cgl_iso2d` into a general 3D camera --
  *  which would cost the whole application its flash and RAM budget and break the
  *  module's published contract -- this page carries its own ~5 KB engine.
  *
  *  The three promises this engine keeps
  *  ------------------------------------
  *  1. **No float and no double, anywhere.** Every quantity that changes with
  *     time is Q8.8 fixed point (`CGL_PARTICLES_SHIFT` = 8, i.e. 1/256 world unit) or a
  *     small integer. There is no `sinf`, no `cosf`, no `sqrtf` and no libm call
  *     in the object this header compiles into -- see `tests/` for how that is
  *     checked, and `cgl_particles.c` for the build gate.
  *  2. **No division and no libgcc helper** in the hot paths. The perspective
  *     divide is a **reciprocal lookup table** (`cgl_particles__recip[]`, generated offline,
  *     see `tools/mkparticletables.py`) with one linear interpolation step; the
  *     trigonometry is a **64 entry 0..90 deg sine table** (`cgl_particles__sin_q8[]`) read
  *     through the quadrant folding in `cgl_particles_sin_q8()` / `cgl_particles_cos_q8()` below, i.e. 128 bytes of
  *     flash cover the whole circle with 16 bit angles.
  *  3. **The particle cloud is 12 bytes per particle.** See `cgl_particle_t`.
  *
  *  Why Q8.8 and not the module's Q12.4
  *  ----------------------------------
  *  `cgl_phys2d`/`cgl_iso2d` speak Q12.4 (16 fractional bits per px... in fact 4), which
  *  reaches +-2048 px but resolves only 1/16 px. This engine has the opposite
  *  requirement: the scene is a single Logo (+-116 px after the 0.55 fit) seen by
  *  a camera whose whole motion is sub-pixel smooth, so **resolution matters and
  *  reach does not**. Q8.8 gives 1/256 px and +-128 px, which is exactly what the
  *  fit macro `CGL_PARTICLES_WORLD_HALF_SPAN_Q8` is there to guarantee. The two conventions
  *  never meet: this engine reads no `cgl_phys2d` type and writes no `cgl_phys2d` field.
  *
  *  Memory: where the storage comes from
  *  ------------------------------------
  *  The demo has ~2.4 KB of BSS left (see `examples/STM32G431CBU6_CytStudio_Demo/docs/application.md` sec. 1), so a
  *  fresh 5 KB of static particles would simply not link. The engine therefore
  *  takes **caller storage**: `cgl_particles_init(&particles, buffer, cap)` binds it to a
  *  plain byte array whose size is checked by the `_Static_assert` below, and the
  *  bullet time page hands it the same RAM the smash page's `cgl_phys2d` world uses
  *  (see `smash_storage_t` in `app_page_smash.h`). The two pages can never be
  *  live at the same time, so the two worlds can never need the memory at the
  *  same time -- that is what makes the overlay safe, and the build breaks if the
  *  buffer is ever too small.
  *
  *  Why a particle is 12 bytes
  *  --------------------------
  *      x, y, z       Q8.8 position, 3 x 2 B     = 6 B   +-128 px, 1/256 px steps
  *      vx, vy, vz    Q8.8 velocity, 3 x 1 B     = 3 B   +-0.5 px/step, 1/256 steps
  *      life, bucket, pad                        = 3 B
  *
  *  The velocity is **int8_t** and that is not a typo. Position stays int16
  *  because a particle has to be able to stand +-116 px away from the world
  *  centre; velocity does not, because the largest speed in the burst is under
  *  0.5 px per 16 ms step (`CGL_PARTICLES_V0_MAX_Q8` plus `CGL_PARTICLES_PUNCH_Q8` is still well under
  *  127). Dropping the three velocities to 8 bit takes the cloud from 7.5 KB to
  *  6.0 KB -- 1.5 KB, which is a quarter of what this page costs in RAM. The sign is
  *  what makes it work: `>>` on a negative int8_t in C99 is an arithmetic shift on
  *  every compiler this project uses, so damping and gravity keep their sign exactly
  *  as they would in 16 bit.
  *
  *  The `pad` byte is not waste: the struct's alignment is 2 (it holds int16_t), so
  *  11 bytes would be padded to 12 by the compiler anyway. Naming it gives the next
  *  person a place to put a field instead of silently growing the struct.
  ******************************************************************************
  */
#ifndef CGL_PARTICLES_H
#define CGL_PARTICLES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* The module's own knobs -- deliberately NOT a framework header. This engine, and
   the host tests that drive it, build with no CytGL header anywhere in the include
   graph; that is what lets the maths be unit tested on its own and reused by a
   consumer that has no UI framework at all. The half of the module that needs the
   canvas is `cgl_particles_view.h`, and it is a separate file for exactly this
   reason (see the CytGL manual, section 15.1). */
#include <ext/cgl_particles_config.h>

/* ========================================================================== */
/* 1. Fixed point tools (Q8.8)                                                */
/* ========================================================================== */

/** Fractional bits of the engine's fixed point format. 8 = Q8.8. */
#define CGL_PARTICLES_SHIFT 8
/** One world unit (one pixel of the fitted scene) in Q8.8. */
#define CGL_PARTICLES_ONE 256
/** Convert px -> Q8.8 (compile time only; never used on a variable). */
#define CGL_PARTICLES_PX(v) ((int32_t)(v) * CGL_PARTICLES_ONE)

/**
 * Reachable half extent of the world, in px.
 *
 * **This is a safety gate, not a camera parameter.** Q8.8 stored in `int16_t`
 * reaches +-127.99 px, and every world coordinate of this scene must fit that or
 * the particles wrap around and fly backwards. `cgl_particles_build()` scales the Logo
 * bitmap so that the widest half axis (the 106 cell long axis) lands exactly on
 * this number, so the gate is kept by construction instead of by luck:
 *
 *      106 cells / 2 * CGL_PARTICLES_CELL_PITCH_Q8 = 53 * 2.1875 = 115.9 px  <  120 px
 *
 * Raising this without re-checking `CGL_PARTICLES_CELL_PITCH_Q8` re-introduces the wrap.
 */
#define CGL_PARTICLES_WORLD_HALF_SPAN_Q8 CGL_PARTICLES_PX(120)
_Static_assert(CGL_PARTICLES_WORLD_HALF_SPAN_Q8 < 128 * CGL_PARTICLES_ONE,
               "the Q8.8 world must fit int16_t: lower CGL_PARTICLES_WORLD_HALF_SPAN_Q8 or CGL_PARTICLES_CELL_PITCH_Q8");

/**
 * Logo cell pitch, Q8.8 = 2.1875 px per cell (560/256), and the grid it is fitted
 * to. These three are **published** rather than private to the engine because the
 * caller has to lay the grid out: `cgl_particles_build()` takes a cell position callback, and
 * the caller (the page, and the host test with its own synthetic grid) can only
 * write that callback if it knows the fit.
 *
 * Derived, not chosen. The smash page's Logo is 106 cells at 3 px = 318 px wide,
 * which does not fit Q8.8 (i.e. +-120 px, 240 px across), so the scene is scaled by
 * 240/318 = 0.755. 2.1875 is the nearest pitch that keeps the whole grid inside the
 * window with margin:
 *
 *      106 * 2.1875 = 231.9 px, half = 115.9 px   <  120 px (the gate above)
 *       14 * 2.1875 =  30.6 px, half =  15.3 px
 *
 * The height is the number that matters for the effect: at yaw 90 deg the Logo's
 * width becomes depth and its 30.6 px height becomes the on-screen width, so the
 * edge-on Logo is a 31 px sliver -- exactly what a 14 cell tall glyph should look
 * like edge-on, and why the camera pitch is not 0 (with pitch 0 the sliver would be
 * a 1 px line, and "the Logo disappeared" is not an effect).
 */
#define CGL_PARTICLES_GRID_W 106
#define CGL_PARTICLES_GRID_H 14
#define CGL_PARTICLES_CELL_PITCH_Q8 560

/** World x of Logo cell `i`, Q8.8. The grid is centred on the engine's origin. */
static inline int32_t cgl_particles_cell_x(int i)
{
    return ((int32_t)i * CGL_PARTICLES_CELL_PITCH_Q8) - ((CGL_PARTICLES_GRID_W * CGL_PARTICLES_CELL_PITCH_Q8) / 2);
}

/** World y of Logo cell `j`, Q8.8. */
static inline int32_t cgl_particles_cell_y(int j)
{
    return ((int32_t)j * CGL_PARTICLES_CELL_PITCH_Q8) - ((CGL_PARTICLES_GRID_H * CGL_PARTICLES_CELL_PITCH_Q8) / 2);
}

/** `(a * b) >> 8` for Q8.8 x Q8.8 -> Q8.8. **All arithmetic shifts are written
    as a division by a power of two** so that the rounding direction is truncation
    toward zero rather than toward -inf -- with `>>` a negative velocity damps
    slightly faster than a positive one and a burst visibly curves. */
#define CGL_PARTICLES_MUL(a, b) ((int32_t)(((int32_t)(a) * (int32_t)(b)) / CGL_PARTICLES_ONE))

/** Clamp an int32 to an int16 range (used by every projection result). */
static inline int16_t cgl_particles_clamp_i16(int32_t v)
{
    if (v > 32767)
    {
        return 32767;
    }
    if (v < -32768)
    {
        return -32768;
    }
    return (int16_t)v;
}

/** Clamp an int32 to 0..255 (bucket index, life, ...). */
static inline uint8_t cgl_particles_clamp_u8(int32_t v)
{
    if (v < 0)
    {
        return 0u;
    }
    if (v > 255)
    {
        return 255u;
    }
    return (uint8_t)v;
}

/* -------------------------------------------------------------------------- */
/* Angles: one full turn is 65536 (uint16_t), i.e. 0.0055 deg per step.        */
/* -------------------------------------------------------------------------- */

/** A full turn in angle units. Wraps for free because it is 2^16. */
#define CGL_PARTICLES_ANGLE_FULL 65536u

/**
 * Sine of `a`, Q8.8 in -256..256.
 *
 * `a` is the **top byte** of a uint16 angle (`ang >> 8`), so the domain of `a` is
 * 0..255 = one turn, 64 units = 90 deg. Quadrant folding:
 *
 *      a & 0x3F   the index into the 0..90 deg table
 *      a & 0x40   the second quadrant: sin(90+x) =  sin(90-x)  -> mirror the index
 *      a & 0x80   the lower half:      sin(180+x) = -sin(x)    -> negate
 *
 * Why the table stores only 0..90 deg and why that is *enough*: the table has 64
 * entries per quadrant, and the angle type itself has only 256 steps per turn --
 * so the table is **exactly as fine as the angle can be expressed**. A larger
 * table could not produce a different value at any representable angle. 64 x 1 B
 * = 64 bytes of flash, and there is no interpolation and no branch.
 */
extern const uint8_t cgl_particles__sin_q8[64];

static inline int16_t cgl_particles_sin_q8(uint8_t a)
{
    uint8_t idx = (uint8_t)(a & 0x3Fu);
    int16_t v;

    if ((a & 0x40u) != 0u)
    {
        idx = (uint8_t)(0x3Fu - idx); /* 2nd quadrant: mirror instead of a second table */
    }
    v = (int16_t)cgl_particles__sin_q8[idx];
    return ((a & 0x80u) != 0u) ? (int16_t)(-v) : v;
}

/** Cosine, Q8.8. cos(a) = sin(a + 90 deg), and 90 deg = 0x40 in the top byte. */
static inline int16_t cgl_particles_cos_q8(uint8_t a)
{
    return cgl_particles_sin_q8((uint8_t)(a + 0x40u));
}

/* -------------------------------------------------------------------------- */
/* Reciprocals: the perspective divide, without a divide                       */
/* -------------------------------------------------------------------------- */

/**
 * Perspective divisor range, px. `cgl_particles__recip[]` holds
 *
 *      cgl_particles__recip[k] = 65536 / k
 *
 * for k = 0..384, i.e. **the table is indexed by the divisor itself**. Depth 0..127
 * is padding that nothing reads -- `cgl_particles_recip_q16()` clamps the depth into
 * `CGL_PARTICLES_NEAR_Q8..CGL_PARTICLES_FAR_Q8` first, and that clamp is what a near/far plane does in a
 * normal renderer: a particle closer than 128 px or further than 383 px is at the
 * border of what this camera can show, and is drawn at the clamped scale rather
 * than producing a wild value. The padding exists so that "depth -> index" is an
 * **identity** with no subtraction and no shift in the inner loop; it costs 256 B of
 * flash and saves an instruction on a path that runs twice per particle per band.
 *
 * (This identity is the contract, and it was broken once: the generator emitted the
 * table starting at depth 128, so `cgl_particles__recip[k]` was `1/(k + 128)` and every scale,
 * position and dot size in the effect was wrong by a factor of two at the near
 * plane. The host test's `cgl_particles_recip_q16(near*256) == cgl_particles__recip[CGL_PARTICLES_NEAR_Q8]` check is
 * what pins it.)
 */
#define CGL_PARTICLES_NEAR_Q8 128
#define CGL_PARTICLES_FAR_Q8 383

/**
 * The reciprocal table is **defined** by the generated header's `.c` companion
 * (`cgl_particles_tables.c`), which means the `.c` file has to see the declarations. It
 * does so through `cgl_particles_tables.h`; this header repeats the two declarations only
 * because `cgl_particles_sin_q8()`/`cgl_particles_recip_q16()` below are `static inline` and the page and
 * the host tests include **this** header, not the generated one. The numbers must
 * match what the generator emits -- `tools/mkparticletables.py` owns both.
 */
extern const uint16_t cgl_particles__recip[385];

/**
 * Camera geometry the **caller** needs as well as the engine.
 *
 * WARNING -- **these are in whole pixels, not Q8.8**, despite sitting next to a
 * file full of Q8.8 numbers. They are the values a human reasons about ("the camera
 * is 200 px from the Logo", "the focal length is 230 px"), and `cgl_particles_cam_look()`
 * shifts them into Q8.8 on the way into the camera struct. Assigning them raw into
 * a `_q8` field is a 256x error that does **not** fail loudly: the camera distance
 * becomes 1.17 px, every depth in the scene comes out negative, the painter's order
 * collapses and the perspective divide saturates -- while the trig, the
 * interpolation and the sort all keep passing their own tests.
 *
 * **How the pair is chosen, and why both extremes have already shipped.** The
 * projection scale is `focal / distance`, and the scene is `2 *
 * CGL_PARTICLES_WORLD_HALF_SPAN_Q8` = 240 px across in world units, seen through a
 * `CGL_PARTICLES_SCREEN_W` = 320 px viewport:
 *
 *      scale 0.31   (focal 384 / dist 300)  ->  a 70 px blob                     NO
 *      scale 1.18   (focal 200 / dist 170)  ->  283 px, fits -- but only at yaw 0,
 *                                              and the dihedral yaws clip it     NO
 *      scale 1.15   (focal 230 / dist 200)  ->  276 px at its widest, fits        OK
 *      scale 1.56                            ->  374 px, clipped both sides      NO
 *
 * The two rejected rows are not hypothetical: `0.31` shipped first and read as "one
 * yellow ball and nothing else", and `1.18` shipped next and read as "particles fly
 * off the screen". The reason `1.18` is not enough is that the widest yaw is not yaw
 * 0 -- the rotated Logo's other axis contributes too, so the *on-screen* half extent
 * at the worst angle is about `128` px against a 160 px half viewport. The gate
 * below encodes exactly that.
 *
 * The pair also has to keep the whole scene inside the near/far window
 * (`CGL_PARTICLES_NEAR_Q8..CGL_PARTICLES_FAR_Q8` = 128..384 px). The scene depth is
 * `distance +- 66 px` (the Logo's half extent) `+- 40 px` (the dolly), so
 * `distance = 200` gives 94..306 px -- inside the window with real margin, and the
 * margin is what stops the clamp from flattening the perspective at the extremes of
 * the orbit. That is the second constraint on the pair, and it is why the distance
 * is not simply whatever makes the scale come out right.
 *
 * `CGL_PARTICLES_CAM_DOLLY_Q8` is the exception to the unit rule: it is Q8.8, because the page
 * applies it as a depth offset and it is never a number a human reads off a drawing.
 */
#define CGL_PARTICLES_CAM_PITCH_A 21u       /**< fixed pitch, 21/256 turn = 29.5 deg, unit-free */
#define CGL_PARTICLES_CAM_DIST_PX 210       /**< camera distance, px                           */
#define CGL_PARTICLES_CAM_FOCAL_PX 235      /**< focal length, px                              */
#define CGL_PARTICLES_CAM_DOLLY_Q8 40       /**< dolly amplitude, Q8.8 px                      */

/**
 * The projection scale, Q8 (256 = 1.0), and the two gates that keep the Logo on
 * screen. This is the pair of numbers that was wrong twice, so it is checked at
 * compile time from the fit rather than trusted:
 *
 *      on-screen half extent at the worst yaw
 *          = scale * (CGL_PARTICLES_WORLD_HALF_SPAN_Q8 in px + 16 px of depth projection)
 *      must be <= (CGL_PARTICLES_SCREEN_W / 2) - 8 px of margin
 *
 * `+16` is the depth axis' contribution to screen x at a dihedral yaw; `-8` is the
 * margin so a dot is never half cut. The lower bound keeps the Logo from being a
 * small blob in the middle instead of filling the viewport. Both ends are dangerous
 * and both have shipped: `focal 384 / dist 300` gave scale 0.31 and a 70 px blob,
 * and `focal 200 / dist 170` gave scale 1.18 which fits at yaw 0 but projects the
 * dihedral yaws off the sides.
 *
 * WARNING: `256` here is the Q8 fixed point scale, not a pixel value -- do not
 * "simplify" it. And `CGL_PARTICLES_WORLD_HALF_SPAN_Q8 / CGL_PARTICLES_ONE` is the half span in **whole
 * pixels**; dividing by `CGL_PARTICLES_ONE` is the unit conversion, not a rounding step.
 */
#define CGL_PARTICLES_CAM_SCALE_Q8 (((int32_t)CGL_PARTICLES_CAM_FOCAL_PX * 256) / (int32_t)CGL_PARTICLES_CAM_DIST_PX)
#define CGL_PARTICLES_CAM_EXTENT_Q8 \
    ((CGL_PARTICLES_CAM_SCALE_Q8 * ((CGL_PARTICLES_WORLD_HALF_SPAN_Q8 / CGL_PARTICLES_ONE) + 16)) / 256)

_Static_assert(CGL_PARTICLES_CAM_EXTENT_Q8 <= ((CGL_PARTICLES_SCREEN_W / 2) - 8),
               "the camera is too wide: the Logo projects past the viewport at its widest yaw. "
               "Raise CGL_PARTICLES_CAM_DIST_PX or lower CGL_PARTICLES_CAM_FOCAL_PX");
_Static_assert(CGL_PARTICLES_CAM_EXTENT_Q8 >= ((CGL_PARTICLES_SCREEN_W / 2) - 25),
               "the camera is too narrow: the Logo projects to a small blob in the middle of the "
               "viewport. Lower CGL_PARTICLES_CAM_DIST_PX or raise CGL_PARTICLES_CAM_FOCAL_PX");

/**
 * 1/depth in Q16.16, by table look-up plus one linear step.
 *
 * `d8` is the camera space depth in Q8.8. The integer part indexes the table and
 * the fractional part interpolates between two entries:
 *
 *      d  = d8 / 256                 (real, px)
 *      k  = d8 >> 8                  (integer px)
 *      f  = d8 & 255                 (fraction, 0..255)
 *      1/d ~= (recip[k] * (256 - f) + recip[k+1] * f) / 256
 *
 * Everything is int32 and the products are bounded by 62874 * 256 = 1.6e7, so no
 * 64 bit helper is pulled in and there is not one division in the expression.
 */
static inline int32_t cgl_particles_recip_q16(int32_t d8)
{
    int32_t k;
    int32_t f;
    uint32_t a;
    uint32_t b;

    /*
     * WARNING: `d8` is Q8.8, so the near/far clamp bounds are the pixel constants
     * **shifted into Q8.8** -- `CGL_PARTICLES_NEAR_Q8 * CGL_PARTICLES_ONE`, not `CGL_PARTICLES_NEAR_Q8`. Getting this
     * wrong does not fail loudly: a raw-pixel bound clamps every depth above 383/256
     * = 1.5 px to the near plane, so every particle in the scene collapses onto the
     * same scale, the perspective divide stops varying with depth, and the "near
     * particles are bigger" cue silently disappears. It also makes the divide's
     * output wrong by the ratio of the two bounds, which is what the host test
     * measures as ~200% relative error at the far plane.
     */
    if (d8 < (CGL_PARTICLES_NEAR_Q8 * CGL_PARTICLES_ONE))
    {
        d8 = CGL_PARTICLES_NEAR_Q8 * CGL_PARTICLES_ONE;
    }
    else if (d8 > (CGL_PARTICLES_FAR_Q8 * CGL_PARTICLES_ONE))
    {
        d8 = CGL_PARTICLES_FAR_Q8 * CGL_PARTICLES_ONE;
    }

    k = d8 / CGL_PARTICLES_ONE; /* the table is indexed by the depth itself, see CGL_PARTICLES_NEAR_Q8 */
    f = d8 % CGL_PARTICLES_ONE; /* 0..255 */
    a = cgl_particles__recip[k];
    b = cgl_particles__recip[k + 1];
    /* a + (b - a) * f / 256, kept in int32: (a << 8) is at most 131072 and the
       second term is at most 512 * 255 = 130560, so nothing overflows and there is
       not one division in the expression. */
    return (int32_t)((((int32_t)a << 8) + (((int32_t)b - (int32_t)a) * f)) >> 8);
}

/* ========================================================================== */
/* 2. Data structures                                                         */
/* ========================================================================== */

/**
 * One particle. **12 bytes**, and every byte is accounted for.
 *
 *      x, y, z       Q8.8 position, 3 x int16_t    = 6 B
 *      vx, vy, vz    Q8.8 velocity, 3 x int8_t     = 3 B
 *      life          steps of life left            = 1 B
 *      bucket        this frame's depth bucket     = 1 B
 *      pad           alignment filler              = 1 B
 *
 * The velocity is **int8_t** and that is not a typo. Position has to be int16
 * because a particle must be able to stand +-116 px from the world centre
 * (`CGL_PARTICLES_WORLD_HALF_SPAN_Q8`); velocity does not, because the largest speed in the
 * burst is under 0.5 px per logical step (`CGL_PARTICLES_V0_MAX_Q8` + `CGL_PARTICLES_PUNCH_Q8` is still
 * well under 127). Dropping the three velocities to 8 bit takes the cloud from
 * 7.5 KB to 6.0 KB -- 1.5 KB, a quarter of this page's total RAM cost. The sign is
 * what makes it work: `>>` on a
 * negative int8_t in C99 is an arithmetic shift on every compiler this project
 * uses, so damping and gravity keep their sign exactly as they would in 16 bit.
 *
 * `hit` used to be a `pad` byte, and it is **not wasted**: the struct's alignment is
 * 2 (it contains int16_t), so 11 bytes would be padded to 12 by the compiler anyway.
 * Naming the byte keeps the arithmetic visible (and the struct at 12 B) while giving
 * the progressive impact somewhere to live: 0 = still parked on its cell, the ball has
 * not reached it; 1 = released (see `cgl_particles_impact()`). A parked particle is skipped by
 * `cgl_particles_step()` entirely -- no gravity, no life burn -- which is what makes the
 * part of the Logo the ball has not touched stay exactly where it was built.
 *
 * The field order is load bearing: positions first, then the three velocity bytes,
 * then the scalars -- that is what leaves the compiler with no hole to insert.
 */
typedef struct
{
    int16_t x, y, z;     /**< position, Q8.8, world space (origin = Logo centre)   */
    int8_t vx, vy, vz;   /**< velocity, Q8.8 **per logical step** (see cgl_particles_step) */
    uint8_t life;        /**< steps of life left; 0 = free slot                    */
    uint8_t bucket;      /**< this frame's depth bucket, 0 = farthest from camera  */
    uint8_t hit;         /**< 0 = parked on its cell, 1 = released (see cgl_particles_impact) */
} cgl_particle_t;

_Static_assert(sizeof(cgl_particle_t) == 12,
               "cgl_particle_t must stay 12 bytes (6 B of int16 position + 3 B of int8 velocity + 3 B of state): "
               "it is the whole point of the 8 bit velocity, and the smash page's storage overlay is sized from it");

/** Particle count ceiling of the engine (the storage must be sized for it).
    `CGL_PARTICLES_MAX` and `CGL_PARTICLES_BUCKETS` are knobs of the module and live
    in `cgl_particles_config.h`, next to the reason each is overridable. */

/**
 * Logical steps after the burst at which the caller should trigger bullet time:
 * 78 logical steps = 1.25 s at the reference application's 16 ms fixed step.
 *
 * The engine itself has no notion of milliseconds -- it counts logical steps and
 * reports them through `cgl_particles_tick()`, and a logical step is exactly one
 * of the caller's fixed step (16 ms in the reference application). It is published here, rather
 * than kept in the .c, because the **page** owns the phase machine that acts on it
 * (it used to hold its own literal `31`, which is one edit away from drifting away
 * from the engine's own trigger -- the exact duplication this header exists to stop).
 *
 * WARNING: this is not just "how long the fly phase looks nice". Together with
 *    `CGL_PARTICLES_DAMP_SHIFT` it *is* the size of the explosion, because a debris velocity is
 *    an int8: at most 127 units, i.e. 0.5 px per step. The displacement is
 *    `speed x steps x 2^CGL_PARTICLES_DAMP_SHIFT`, so the original 31 steps (0.5 s) with strong
 *    damping opened the cloud by a measured mean of 3.3 px -- a ragged Logo, not an
 *    explosion, which is how the effect was reported. See the WARNING on
 *    `CGL_PARTICLES_DAMP_SHIFT` in `cgl_particles.c`.
 */
#define CGL_PARTICLES_TRIGGER_TICKS 78u

/**
 * Depth buckets of the painter's algorithm sort.
 *
 * The push is a **counting sort** (bucket sort) on the camera depth:
 *
 *      pass 1   bucket[ p->bucket ]++ for every particle
 *      pass 2   prefix sum -> where each bucket's run starts in order[]
 *      pass 3   order[ cursor[ p->bucket ]++ ] = i
 *
 * Three linear passes, no comparison, no recursion, no worst case, and the result
 * is already in "far to near" order. It costs 2 B per bucket and 2 B per particle
 * of index. 128 buckets over the 128..383 px depth window is 2 px of depth
 * resolution -- finer than the 3 px dot a particle is drawn as, so two particles
 * can never be reordered visibly by the quantisation.
 *
 * Bucket 0 is assigned to "at or behind the camera plane" and bucket
 * `CGL_PARTICLES_BUCKETS - 1` to "at or past the far plane", so the two degenerate
 * ends of the clamp collect there instead of wrapping to the wrong end of the
 * painter's order (which is the one failure a depth sort must never have).
 */
/* CGL_PARTICLES_BUCKETS: the table below is sized by it (cgl_particles_config.h). */

/** Depth window the buckets span, in Q8.8. Chosen to line up with the
    reciprocal table's window so "where the camera can see" is one number. */
#define CGL_PARTICLES_DEPTH_SPAN_Q8 ((CGL_PARTICLES_FAR_Q8 - CGL_PARTICLES_NEAR_Q8 + 1) * CGL_PARTICLES_ONE / CGL_PARTICLES_BUCKETS)

/** Camera. Filled by `cgl_particles_cam_look()`; all fields are Q8.8 unless marked. */
typedef struct
{
    /**
     * The full 3x3 world -> camera rotation, composed as pitch (about X) then yaw
     * (about Y). Written out as nine coefficients rather than derived per particle
     * because a particle is projected **twice per band**: three multiply-adds for
     * the depth row would be 500 x 43 x 3 extra multiplies per frame to save nine
     * `int16_t` in a struct that exists once.
     */
    int16_t m00, m01, m02; /**< camera x = (m00*x + m01*y + m02*z) / 256 */
    int16_t m10, m11, m12; /**< camera y = (m10*x + m11*y + m12*z) / 256 */
    int16_t m20, m21, m22; /**< camera z = (m20*x + m21*y + m22*z) / 256 **is the sort key** */
    int32_t dist_q8;       /**< camera distance along the view axis, Q8.8       */
    int32_t orbit_z_q8;    /**< extra depth offset of the dolly, Q8.8           */
    int32_t focal_q8;      /**< focal length, Q8.8 px per px                    */
    /**
     * Projection origin: the screen centre, Q8.8.
     *
     * **int32_t, not int16_t.** These are Q8.8, so a 320 px panel needs 160 * 256 =
     * 40960 for the centre -- which does not fit int16_t (32767) and silently wraps
     * to -24576. The symptom is not a crash: the projection origin moves 160 px to
     * the left, the whole cloud is drawn off screen, and the depth sort keeps
     * working perfectly on particles nobody can see. The first version of this
     * struct had them as int16_t.
     */
    int32_t ox_q8;
    int32_t oy_q8;
} cgl_particles_cam_t;

/**
 * The scene's world constants, derived once by `cgl_particles_init()`.
 *
 * The engine's world space is the same isometric basis the smash page uses
 * (`src/ext/cgl_iso2d.c`: screen x = x - z, screen y = y + z), with the origin moved to
 * the Logo grid centre and the scale chosen so the long axis fits Q8.8 (see
 * `CGL_PARTICLES_WORLD_HALF_SPAN_Q8`). The constants live in a struct of their own rather
 * than as macros because they depend on the caller's viewport, and because the
 * render test needs to project known cells to known pixels.
 */
typedef struct
{
    int32_t origin_x_q8; /**< world x of the Logo grid centre, Q8.8      */
    int32_t origin_y_q8; /**< world y of the Logo grid centre, Q8.8      */
    int16_t pitch_q8;    /**< Logo cell pitch, Q8.8 = 2.1875 px per cell */
    uint16_t max_active; /**< particles ever spawned in this round       */
} cgl_particles_world_t;

/** Engine state. **This is the big one**: `particle[]` is 6.0 KB at 500. */
typedef struct
{
    cgl_particle_t particle[CGL_PARTICLES_MAX];
    uint16_t order[CGL_PARTICLES_MAX]; /**< sorted indices, far -> near (bucket sort output) */
    uint16_t bucket[CGL_PARTICLES_BUCKETS]; /**< bucket counts, reused as the prefix sum       */
    uint16_t active;      /**< particles alive right now                        */
    uint16_t max_active;  /**< ceiling reached this round (HUD readout)         */
    uint32_t seed;        /**< xorshift state: no libc rand, deterministic      */
    uint8_t tick;         /**< logical steps simulated this round (0..255)      */
    bool sorted;          /**< order[] is valid for the current camera          */
} cgl_particles_t;

/* -------------------------------------------------------------------------- */
/* Storage: how much a caller has to provide, as one expression                */
/* -------------------------------------------------------------------------- */

/**
 * Bytes a caller has to hand over for `n` particles.
 *
 * **The `n == CGL_PARTICLES_MAX` case is defined as exactly
 * `sizeof(cgl_particles_t)`**, and the arithmetic form is kept only so that a
 * caller sizing for a *different* particle count (a future variant of the engine)
 * can still compute it. The two are tied together by the `_Static_assert` below, and
 * that assert is load bearing rather than documentation:
 *
 * The first version added a flat `+ 16` of "headroom for the scalars" on top of the
 * three arrays. That made the macro 4 bytes **larger** than the struct it describes
 * (the struct's own tail padding was already inside `sizeof`), `cgl_particles_init()` compares
 * the caller's buffer against the macro, and `smash_storage_t` -- which is sized
 * `sizeof(cgl_particles_t)`-ish by the union -- came out 4 bytes short. The
 * result was that **every** `cgl_particles_init()` call returned NULL, the page drew no
 * particles at all, and the only visible symptom was "the demo shows nothing but the
 * ball". A storage gate that is stricter than the storage it gates is worse than no
 * gate: it fails closed and silently.
 */
#define CGL_PARTICLES_STORAGE_BYTES(n) \
    (((n) == CGL_PARTICLES_MAX) \
         ? (uint32_t)sizeof(cgl_particles_t) \
         : ((uint32_t)(n) * (uint32_t)sizeof(cgl_particle_t) + \
            (uint32_t)(n) * (uint32_t)sizeof(uint16_t) + \
            (uint32_t)CGL_PARTICLES_BUCKETS * (uint32_t)sizeof(uint16_t) + 32u))

/**
 * Alignment and size of a conforming storage block, as a type.
 *
 * Use this (or a union with `cgl_phys2d_t`) as the overlay buffer rather than a plain
 * `uint8_t[]`: the engine casts the storage to `cgl_particles_t`, whose
 * `uint32_t seed` makes 4 byte alignment a requirement, and an unaligned cast is
 * undefined behaviour even when the size is right.
 */
typedef union
{
    cgl_particle_t align_particle;
    uint16_t align_u16;
    uint32_t align_u32;
    uint8_t bytes[CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX)];
} cgl_particles_storage_t;

_Static_assert(sizeof(cgl_particles_storage_t) >= CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX),
               "cgl_particles_storage_t must hold CGL_PARTICLES_MAX particles, their sort table and the buckets");

/* The gate that keeps the macro and the struct from drifting apart. Without it,
   `CGL_PARTICLES_STORAGE_BYTES()` can be larger than the struct that `cgl_particles_init()` is asked to
   accept, and every caller fails the size check for no visible reason -- see the
   WARNING on the macro. */
_Static_assert(CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX) == (uint32_t)sizeof(cgl_particles_t),
               "CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX) and sizeof(cgl_particles_t) disagree: "
               "cgl_particles_init() would reject correctly sized storage");

/* ========================================================================== */
/* 3. Projection (hot path)                                                   */
/* ========================================================================== */

/**
 * Camera space z, Q8.8. **This is the painter's algorithm sort key**: the larger
 * it is, the further the particle is from the camera, and the earlier it must be
 * painted.
 *
 * WARNING: `x`/`y`/`z` are int32, unlike the particle coordinates they are usually
 *    fed from. The **particles** live inside the +-128 px Q8.8 world window (they are
 *    built from the Logo grid and culled when they leave it), but not every body in
 *    the scene does: the wrecking ball is page state that starts off the stage and has
 *    to run off the far side of it (see `app_page_bullet.c`), so the projection takes
 *    the wider type rather than clamping the caller's position. The products stay
 *    bounded because the matrix coefficients are Q8.8 unit vectors (|m| <= 256) and
 *    the ball's world x is a few hundred px, not thousands: |m*x| <= 256 * 1e5 = 2.6e7.
 */
static inline int32_t cgl_particles_cam_depth(const cgl_particles_cam_t *c, int32_t x, int32_t y, int32_t z)
{
    return ((((int32_t)c->m20 * x) + ((int32_t)c->m21 * y) + ((int32_t)c->m22 * z)) / CGL_PARTICLES_ONE) +
           c->dist_q8 + c->orbit_z_q8;
}

/**
 * How many pixels one world unit becomes at this depth, Q8.8.
 *
 * This is the single perspective divide of the whole engine, and it is why
 * nothing here needs a `/`. The returned value is used **twice**: once to scale
 * the two screen offsets, and once to scale the dot size (see
 * `cgl_particles_dot_px`), so "near particles are bigger" is free rather than a
 * second divide.
 */
static inline int32_t cgl_particles_persp_scale_q8(const cgl_particles_cam_t *c, int32_t depth_q8)
{
    return (c->focal_q8 * cgl_particles_recip_q16(depth_q8)) >> 16;
}

/**
 * Full world -> screen projection.
 *
 *      cam_x  = (m00*x + m01*y + m02*z) / 256     camera space x, Q8.8
 *      cam_y  = (m10*x + m11*y + m12*z) / 256     camera space y, Q8.8
 *      depth  = (m20*x + m21*y + m22*z) / 256 + dist + orbit
 *      scale  = focal / depth                     Q8.8
 *      sx     = ox + (cam_x * scale) / 256        offset from the screen centre
 *      sy     = oy + (cam_y * scale) / 256
 *
 * `sx`/`sy` are screen centre offsets in Q8.8. Every product is bounded:
 * |cam_x| <= sqrt(3)*128 px, |scale| <= 1.4 px/px, |ox| <= 160 px, so the largest
 * intermediate is ~5e5 and the largest final ~1.9e5 -- int32 with three orders of
 * magnitude to spare, and (this is the point) no `__aeabi_lmul`. The int32 world
 * coordinates the signature takes (see `cgl_particles_cam_depth()`) push the first of those to
 * ~2.6e7 for a body a few hundred px outside the particle window, which is still four
 * orders of magnitude inside int32.
 *
 * @param depth_out receives the sort key so the caller does not compute it twice
 */
static inline void cgl_particles_project(const cgl_particles_cam_t *c, int32_t x, int32_t y, int32_t z,
                              int32_t *sx_out, int32_t *sy_out, int32_t *depth_out)
{
    const int32_t cx = ((((int32_t)c->m00 * x) + ((int32_t)c->m01 * y) + ((int32_t)c->m02 * z)) / CGL_PARTICLES_ONE);
    const int32_t cy = ((((int32_t)c->m10 * x) + ((int32_t)c->m11 * y) + ((int32_t)c->m12 * z)) / CGL_PARTICLES_ONE);
    const int32_t depth = cgl_particles_cam_depth(c, x, y, z);
    const int32_t scale = cgl_particles_persp_scale_q8(c, depth);

    *sx_out = (int32_t)c->ox_q8 + ((cx * scale) / CGL_PARTICLES_ONE);
    *sy_out = (int32_t)c->oy_q8 + ((cy * scale) / CGL_PARTICLES_ONE);
    if (depth_out != NULL)
    {
        *depth_out = depth;
    }
}

/**
 * Size the particle is painted at, in px (1..3, the sizes the band painter accepts).
 *
 * Derived from the **same** perspective scale the position uses, so the size
 * gradient and the position gradient can never disagree; the two constants below
 * only set "how big is a particle at the near clamp" and "how fast does it shrink
 * with depth":
 *
 *      dot_q8 = (scale * CGL_PARTICLES_DOT_SPAN_Q8 + CGL_PARTICLES_ONE * CGL_PARTICLES_DOT_BASE_Q8) >> 8
 *      px     = dot_q8 >> 8
 */
#define CGL_PARTICLES_DOT_SPAN_Q8 384 /**< 1.5 px at scale = 1.0 */
#define CGL_PARTICLES_DOT_BASE_Q8 256 /**< 1.0 px floor            */

static inline uint8_t cgl_particles_dot_px(const cgl_particles_cam_t *c, int32_t depth_q8)
{
    const int32_t scale = cgl_particles_persp_scale_q8(c, depth_q8);
    const int32_t dot_q8 = CGL_PARTICLES_MUL(scale, CGL_PARTICLES_DOT_SPAN_Q8) + CGL_PARTICLES_DOT_BASE_Q8;
    const int32_t px = dot_q8 / CGL_PARTICLES_ONE;

    if (px < 1)
    {
        return 1u;
    }
    if (px > 3)
    {
        return 3u;
    }
    return (uint8_t)px;
}

/* ========================================================================== */
/* 4. Lifecycle and simulation API                                            */
/* ========================================================================== */

/** Bytes the embedded form needs; the caller compares it with its buffer. */
#define CGL_PARTICLES_BYTES ((uint32_t)sizeof(cgl_particles_t))

/**
 * Bind the engine to caller storage. **Nothing is allocated here** (the module's
 * no-heap rule): the storage is bound once and outlives the engine.
 *
 * @param storage  **at least `CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX)` bytes**, aligned
 *                 for `cgl_particles_t` (4 B). Use `cgl_particles_storage_t` or a union
 *                 with `cgl_phys2d_t`; a bare `uint8_t buf[N]` is not guaranteed to be
 *                 aligned and the cast would be undefined.
 * @param storage_bytes  how large `storage` really is. A short buffer is
 *                 **refused** (returns NULL) rather than overrun.
 * @return the bound engine, or NULL when the storage is NULL or too small
 */
cgl_particles_t *cgl_particles_init(void *storage, uint32_t storage_bytes);

/**
 * Build one round of the Logo cloud, in its **resting** state: one particle per
 * lit dot of `logo_bitmap.h`, sitting exactly on its Logo cell, plus
 * `CGL_PARTICLES_MAX - lit dots` spare particles parked at the centre for the
 * impact debris. No velocity yet -- that is `cgl_particles_burst()`'s job, and keeping the
 * two apart is what lets the page show the intact Logo before the ball arrives.
 *
 * The engine reaches the bitmap through two small app-provided callbacks rather
 * than by including `logo_bitmap.h` itself: that keeps this file a self-contained,
 * host-testable integer engine with no demo data baked in, and it means the
 * render test can feed it a synthetic grid.
 *
 * @param dot_count   number of lit dots to walk (a cell that is not lit is skipped)
 * @param is_lit      returns true when cell (i, j) is lit
 * @param cell_x      world x of cell (i, j), Q8.8, engine world space
 * @param cell_y      world y of cell (i, j), Q8.8, engine world space
 */
void cgl_particles_build(cgl_particles_t *s, uint16_t dot_count,
              bool (*is_lit)(int i, int j),
              int32_t (*cell_x)(int i),
              int32_t (*cell_y)(int j));

/**
 * Fire the burst: give every parked particle its outward velocity, as if the ball
 * had struck the Logo at `impact_x/impact_y/impact_z` (Q8.8) travelling towards
 * +x. Also the moment the caller's "bullet time" countdown starts.
 *
 * The velocity model is deliberately the cheapest one that reads correctly:
 * radial in the plane (the Logo's own plane, which the impact pushes apart) plus
 * an outward depth component whose size is modulated by a per particle 6.2 fixed
 * point normal (`nx`, `nz` -- a static per particle value, so it costs one byte
 * each and no recomputation), and one pseudo-random jitter term from the
 * engine's xorshift state.
 */
void cgl_particles_burst(cgl_particles_t *s, int32_t impact_x, int32_t impact_y, int32_t impact_z);

/**
 * Advance the wrecking ball's **sweep** through the cloud by one logical step:
 * release every particle the ball's leading edge has reached, with the same radial +
 * punch velocity `cgl_particles_burst()` gives, and return how many were released (0 = the ball
 * has not reached the sheet yet).
 *
 * This is the shape the effect actually wants, and the reason it exists next to
 * `cgl_particles_burst()`: a burst releases the **whole** sheet from one point, so the ball has
 * not touched most of the Logo when those dots fly away -- on the panel that reads as
 * "the Logo dissolves before the ball arrives". Sweeping instead releases each dot
 * when the ball reaches it, the direction is taken from the **ball's centre** (not
 * from one fixed plane), and every dot the ball has not reached stays parked on its
 * cell: the ball visibly carves a path, exactly like the smash page's `cgl_phys2d`
 * collision, and the frozen frame is a half-eaten Logo with the debris sprayed around
 * the contact point.
 *
 * `ball_x_q8` is the ball's centre, `ball_r_q8` its radius; both Q8.8 in the engine's
 * world space. Call it once per logical step while the ball is inside the sheet, then
 * `cgl_particles_step()`.
 */
uint16_t cgl_particles_impact(cgl_particles_t *s, int32_t ball_x_q8, int32_t ball_r_q8);

/** Drop every particle (life = 0). The next `cgl_particles_build()` refills the cloud. */
void cgl_particles_reset(cgl_particles_t *s);

/** The world constants of the current round, for the camera and for the tests. */
const cgl_particles_world_t *cgl_particles_world(const cgl_particles_t *s);

/**
 * One **logical** simulation step: gravity, damping, integration, culling.
 *
 * The step is unitary on purpose -- it does not know what a millisecond is. The
 * page owns a time scale and calls this 0..N times per fixed step, which is
 * exactly how "time nearly stops" is expressed in integers (see
 * `app_page_bullet.c`). A particle whose position would leave the Q8.8 window is
 * killed here, which is also the only reaping the engine does.
 */
void cgl_particles_step(cgl_particles_t *s);

/** Position component `axis` (0 = x, 1 = y, 2 = z) of particle `i`, Q8.8.
    Used by the tests so that they need not know the layout. 0 for a dead or
    out of range particle. */
int16_t cgl_particles_pos(const cgl_particles_t *s, uint16_t i, uint8_t axis);

/** Velocity component `axis` (0 = x, 1 = y, 2 = z) of particle `i`, Q8.8. */
int8_t cgl_particles_vel(const cgl_particles_t *s, uint16_t i, uint8_t axis);

/**
 * Depth-sort the cloud for one camera (far -> near) and return how many are
 * alive. Runs the counting sort described at `CGL_PARTICLES_BUCKETS`; it must be
 * re-run whenever the **camera** changes, which for this page is every frame
 * while the orbit is running (the stamp trick `cgl_iso2d` uses cannot be borrowed
 * here: a rotating camera makes the order change without any particle moving).
 *
 * `order[0]` is the farthest and `order[count - 1]` the nearest.
 */
uint16_t cgl_particles_sort(cgl_particles_t *s, const cgl_particles_cam_t *cam);

/** Index of the k-th particle in the current sort order (0 = farthest). */
uint16_t cgl_particles_order_at(const cgl_particles_t *s, uint16_t k);

/* The band painter is NOT here: it needs `cgl_canvas_t`, and this header must stay
   free of framework types so that the engine can be built and tested alone. It lives
   in the module's other half, `cgl_particles_view.h`. */

/**
 * A viewport rectangle, in screen pixels, inclusive on all four sides -- the same
 * shape as the framework's `cgl_rect_t`, declared here so that the camera does not
 * need `<cytgl/cgl_types.h>`.
 *
 * WARNING: it is **not** `cgl_rect_t` and the two are not interchangeable by
 * accident: passing one where the other is expected is a compile error, which is
 * the point. The adapter (`cgl_particles_view.h`) does the four-field conversion in
 * one place.
 */
typedef struct
{
    int16_t x0, y0, x1, y1;
} cgl_particles_rect_t;

/**
 * Compute the orbit camera for a given yaw and dolly.
 *
 * @param yaw_u8      top byte of the uint16 angle (0..255 = one turn)
 * @param orbit_q8    dolly offset along the view axis, Q8.8 px (the "slight
 *                    zoom" of the effect; keep |orbit_q8| < 64 or the depth
 *                    clamp starts eating the far half of the scene)
 * @param frame       the viewport, used to place the projection centre
 */
void cgl_particles_cam_look(cgl_particles_cam_t *cam, uint8_t yaw_u8, int32_t orbit_q8,
                            cgl_particles_rect_t frame);

/** Number of particles alive. */
uint16_t cgl_particles_active(const cgl_particles_t *s);

/** Ceiling reached this round (HUD readout). */
uint16_t cgl_particles_max_active(const cgl_particles_t *s);

/** Logical steps run this round, modulo 256. */
uint8_t cgl_particles_tick(const cgl_particles_t *s);

#endif /* CGL_PARTICLES_H */
