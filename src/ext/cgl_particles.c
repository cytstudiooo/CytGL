// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_particles.c
  * @brief   Bullet time particle engine: simulation, depth bucket sort, orbiting
  *          camera, band renderer. Pure integer, no libm, no heap, no division in
  *          the hot paths.
  *
  *  Read `cgl_particles.h` first: it carries the reasoning (why Q8.8, why
  *  12 bytes per particle, why a bucket sort, why a reciprocal table). This file is
  *  the implementation and the tuning.
  *
  *  The four things that are easy to get wrong here
  *  -----------------------------------------------
  *  1. **The painter's order is per band.** A band renderer (this framework paints
  *     the screen in 4 px strips and hands the widget only the rows of the current
  *     band) means a particle may not be drawn "sometime this frame": it has to be
  *     drawn while its own band is being painted. That is why the painter in
  *     `cgl_particles_view.c` takes the band, and why the sort result is a
  *     whole-frame order the band then filters -- not a per-band sort, which would
  *     be 43 sorts per frame.
  *  2. **The sort key is the camera depth, not y.** After an orbit the two are not
  *     even correlated: at yaw 90 deg a particle's own y says nothing about how
  *     far away it is. Sorting by y "works" at yaw 0 and produces back-to-front
  *     errors everywhere else, which looks like particles flickering through each
  *     other.
  *  3. **`>>` is `floor`, `/` is truncation.** Every fixed point scaling in this
  *     file is written as a division by a power of two on purpose (see `CGL_PARTICLES_MUL`).
  *     With `>>`, a negative x rounds down while a positive x rounds up, so the
  *     cloud becomes asymmetric about the origin by one bit and the orbit shows a
  *     1 px "shiver" at the exact moments a particle crosses zero.
  *  4. **A particle that leaves the Q8.8 window must die, not wrap.** int16 Q8.8
  *     reaches +-127.99 px. A position that overflows does not saturate in C, it
  *     wraps, and a wrapped particle reappears mirrored on the other side of the
  *     Logo. `cgl_particles_step()` therefore culls against the window instead of
  *     trusting the arithmetic.
  ******************************************************************************
  */
#include <ext/cgl_particles.h>
#include <ext/cgl_particles_tables.h>

#include <string.h>

/* The off switch (see cgl_particles_config.h). The normal way not to pay for this
   module is to not link it -- that needs no macro. It is NOT enough for a build that
   collects its sources per directory (this repository's own firmware build does
   exactly that), because such a build compiles this file whether or not anybody
   calls into it; `CGL_CFG_PARTICLES 0` is what makes "we do not want this effect"
   cost nothing there. The whole translation unit is inside, so what the compiler
   emits is an empty object. */
#if CGL_CFG_PARTICLES

/* -------------------------------------------------------------------------- */
/* Tuning: simulation feel                                                    */
/* -------------------------------------------------------------------------- */

/**
 * Damping, as a right shift per logical step: `v -= v >> CGL_PARTICLES_DAMP_SHIFT`.
 *
 * 7 (`v *= 127/128` per step) is "the debris keeps flying": over the 78 step flight
 * the speed falls to 54% of the launch speed and the debris coasts through the whole
 * fly phase, which is what makes the impact read as an explosion.
 *
 * WARNING: this used to be 3 (`v *= 7/8`, 1.6% left after 31 steps) on the theory
 *    that "the cloud coasts and then hangs almost still, so the freeze does not have
 *    to hide a fast moving scene". That reasoning left out the other half of the
 *    arithmetic: the velocity is an int8 (see `cgl_particle_t`), so 127 is also the
 *    hard speed ceiling of 0.5 px per step, and the *displacement* is
 *    `speed x steps x 2^shift`. Heavy damping therefore did not "calm the freeze",
 *    it **shrank the explosion**: the measured spread of the frozen cloud was
 *    **3.3 px mean / 4.2 px max** -- barely 1.5 dot pitches, i.e. a ragged Logo
 *    rather than flying debris, which is exactly how the effect was reported
 *    ("the ball hits the Logo and nothing flies apart").
 *
 * The deceleration the freeze needs is already the page's job: `BULLET_FREEZE_RAMP_MS`
 * scales time down to 0 and the gated logical steps simply stop. Damping only has to
 * keep the cloud from being in constant motion forever.
 *
 * WARNING: shifting a **negative** int8_t is implementation defined in C99 and
 * arithmetic (`asr`) on every ARM compiler this project uses, which is what the
 * engine relies on. A port to a compiler where it is a logical shift would make
 * negative velocities damp towards +inf; see the ARM64 note in the header if that
 * ever happens.
 */
#define CGL_PARTICLES_DAMP_SHIFT 7

/**
 * Gravity along world **+y**, in Q8.8 whole units per step.
 *
 * The engine's world basis is the same one the smash page uses (see `cgl_iso2d.c`):
 *
 *      screen x =  x - z
 *      screen y =  y + z
 *
 * so "down the screen" is **+y**, not -z. Accelerating world z would move the
 * particles *up* the screen, which is the single most common way to get this
 * basis wrong.
 *
 * The value is 1 whole unit per step^2 (Q8.8). The drop grows quadratically and is
 * meant to be a *tilt* on the explosion, not a fall: over the 78 step flight it
 * comes to about 12 px at the end, which reads as the debris being pulled down
 * rather than as the Logo being dropped. This is a horizontal smash; raising it
 * further turns the effect into "the Logo falls" instead of "the Logo is hit".
 */
#define CGL_PARTICLES_GRAVITY 1

/** Steps a particle may live. 200 steps is 3.2 s of logical time at 16 ms, well
    past the 78 step burst plus the freeze; the particle is normally culled by
    leaving the Q8.8 window long before this runs out. */
#define CGL_PARTICLES_LIFE_STEPS 200

/**
 * Launch speed, Q8.8 per step, and the **forward punch** every particle receives.
 *
 * `CGL_PARTICLES_V0_*` is the radial part and `CGL_PARTICLES_PUNCH_Q8` is a component added to every
 * particle along the direction the ball was travelling. Both are needed, and the
 * first version had only the radial part -- which is why the effect read as "the
 * Logo collapses inward" rather than "the Logo is smashed":
 *
 *      radial only   the far half flies away from the impact, the near half flies
 *                    back towards it, nothing carries the debris along
 *      radial+punch  the sheet bursts **and** the pieces travel with the ball, which
 *                    is what a real impact looks like and what the smash page's own
 *                    `cgl_phys2d` collision produces by momentum transfer
 *
 * The punch is an int8_t-add on top of the radial term, so it is clamped by the
 * same `cgl_particles__clamp_i8()`: a particle already at the ceiling cannot grow, which is why
 * the radial range leaves `CGL_PARTICLES_PUNCH_Q8` of headroom under 127.
 */
#define CGL_PARTICLES_V0_MIN_Q8 45
#define CGL_PARTICLES_V0_MAX_Q8 95
#define CGL_PARTICLES_PUNCH_Q8 32

/** Depth push inherited from the outward normal, Q8.8. This is what turns a flat
    "explosion inside the Logo's plane" into a cloud with real depth: without it
    every particle stays within a pixel of z = 0 and the orbit is a rotation of a
    flat picture, which the eye reads as a 2D trick immediately. */
#define CGL_PARTICLES_V0_DEPTH_Q8 72

/** How far either side of the Logo plane the radial direction is dominated by the
    in-plane component, Q8.8. Beyond it a particle is thrown mostly sideways. */
#define CGL_PARTICLES_IMPACT_PLANE_Q8 24

/** Per particle jitter added to the two in-plane velocity components, Q8.8, and
    **symmetric about zero** so that it scatters the cloud without biasing it in
    any direction. Without it a rigid radial burst reads as a starburst; with a
    one sided bias instead of a symmetric one the whole cloud drifts, which is a
    different (and much more visible) bug. */
#define CGL_PARTICLES_V0_JITTER_Q8 8

/* -------------------------------------------------------------------------- */
/* Tuning: the world                                                          */
/* -------------------------------------------------------------------------- */

/* The world fit -- CGL_PARTICLES_GRID_W / CGL_PARTICLES_GRID_H / CGL_PARTICLES_CELL_PITCH_Q8 and the
   `cgl_particles_cell_x()` / `cgl_particles_cell_y()` pair built on them -- is published by the header,
   because the caller has to lay the grid out to write `cgl_particles_build()`'s cell
   callbacks. See the derivation there. */

/**
 * Camera distance and focal length, Q8.8 px.
 *
 * A pair, solved from "the Logo has to fit the viewport with a little margin at
 * the widest yaw". Worst case is yaw = 45 deg, where the projected half extent in
 * screen x = (x - z) is
 *
 *      outer = 115.9 * cos45 + 115.9 * sin45 ... but z is compressed by the pitch
 *              (the world z axis is foreshortened by cos(pitch)) and the Logo is
 *              only ~15 px half-deep after the fit:
 *              ~ 82 + 15 * cos(pitch) + 15 * sin(pitch) = ~108 px on screen
 *      scale needed = 145 px / 108 px = 1.34
 *      scale = focal / distance  ->  focal = 1.34 * 300 = 402  ->  384 with margin
 *
 * The distance also keeps the whole scene inside the near/far window
 * (`CGL_PARTICLES_NEAR_Q8..CGL_PARTICLES_FAR_Q8` = 128..383 px): the nearest particle of a round lands
 * around 300 - 40 (dolly) - 20 (depth half extent) = 240 px and the farthest
 * around 360 px, both with room to spare. That margin is the point -- it is what
 * stops the clamp from flattening the perspective at the extremes of the orbit.
 *
 * The three numbers (`CGL_PARTICLES_CAM_DIST_PX`, `CGL_PARTICLES_CAM_FOCAL_PX`, `CGL_PARTICLES_CAM_DOLLY_Q8`) and the
 * pitch are declared in the header, because the page applies the dolly and has to
 * stay inside the same window. See `cgl_particles.h`.
 */

/* -------------------------------------------------------------------------- */
/* Derived constants                                                          */
/* -------------------------------------------------------------------------- */

/** Depth per bucket, Q8.8. See `CGL_PARTICLES_BUCKETS`. */
#define CGL_PARTICLES_BUCKET_UNIT_Q8 \
    (((CGL_PARTICLES_FAR_Q8 - CGL_PARTICLES_NEAR_Q8 + 1) * CGL_PARTICLES_ONE) / CGL_PARTICLES_BUCKETS)

/** Distance from the world origin of the Logo grid centre, Q8.8. The grid is
    centred on the engine's origin, so both are 0 -- named for the reader, and the
    one place to change if the scene is ever re-centred. */
#define CGL_PARTICLES_ORIGIN_X_Q8 0
#define CGL_PARTICLES_ORIGIN_Y_Q8 0

/* -------------------------------------------------------------------------- */
/* Small integer helpers                                                      */
/* -------------------------------------------------------------------------- */

/** xorshift32. No libc `rand()`: it is not reentrant, it costs flash, and this
    engine's determinism is what lets the host test assert on exact positions. */
static inline uint32_t cgl_particles__rand(cgl_particles_t *s)
{
    uint32_t x = s->seed;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->seed = x;
    return x;
}

/** Uniform-ish value in `lo..hi` (inclusive) from the deterministic stream. */
static inline int32_t cgl_particles__rand_range(cgl_particles_t *s, int32_t lo, int32_t hi)
{
    const uint32_t span = (uint32_t)(hi - lo + 1);

    return lo + (int32_t)(cgl_particles__rand(s) % span);
}

/**
 * Floor of the square root of a **uint32**, integer only.
 *
 * Used **once per particle per burst** (500 calls for a whole round, not 500 per
 * frame), which is why a bit-by-bit restore loop is acceptable here even though the
 * render path may not contain one. `sqrtf()` is not an option at all: it is a libm
 * call and the target has no FPU.
 *
 * Why not "just use the squared distance": the burst needs the radial **unit**
 * vector, and dividing by the magnitude is the only way to get a direction whose
 * speed does not grow with distance from the impact. A `1/sqrt` reciprocal table
 * would need a second table and a second fixed point scale, to remove one division
 * that runs 500 times per 13 second round.
 *
 * The algorithm is the classic bit-by-bit restore, which is **exact** for every
 * uint32 input -- the first draft followed it with two Newton steps, which is a
 * common habit from float implementations and is pure waste here: the restore loop
 * already returns floor(sqrt(v)) exactly, and "polishing" it only re-derives the
 * same answer while introducing a division and a rounding question.
 *
 * WARNING: the parameter is **unsigned**, and that is a bug fix rather than a style
 *    choice. It used to take an int32 and open with `if (v <= 0) return 0;`. The
 *    caller then formed the sum of squares in int32, which **overflows** for this
 *    scene (see `cgl_particles__dir_mag()`), and with signed overflow being undefined behaviour
 *    both compilers deleted that guard as dead code -- the sum "cannot" be negative
 *    -- leaving `while (bit > v) bit >>= 2;` to spin forever on the wrapped negative
 *    value (`bit` walks to 0 and `0 > v` stays true). Measured: the host test hung
 *    inside `cgl_particles_burst()` (bullet_host_test section [7], 120 s ctest timeout) and the
 *    firmware ELF contains the same unterminated loop. As an unsigned parameter
 *    there is no such state at all: `bit` reaches 0 and `0 > v` is false for every
 *    uint32 v, whatever the caller passes.
 *
 * `bit` starts at 2^30 -- the square of 2^15 -- so the restore is exact over the
 * whole uint32 range (the result can reach 65535); the older 2^14 start was only
 * exact below 2^30 and is one more thing that would have to be re-derived if this
 * helper ever grew a second caller.
 */
static int32_t cgl_particles__isqrt_u32(uint32_t v)
{
    uint32_t x = 0u;
    uint32_t bit = 1u << 30;

    while (bit > v)
    {
        bit >>= 2;
    }
    while (bit != 0u)
    {
        if (v >= (x + bit))
        {
            v -= (x + bit);
            x = (x >> 1) + bit;
        }
        else
        {
            x >>= 1;
        }
        bit >>= 2;
    }
    return (int32_t)x;
}

/**
 * `|d|`, i.e. `floor(sqrt(d0^2 + d1^2 + d2^2))`, computed without ever forming a
 * value that does not fit.
 *
 * WARNING: this function exists because the obvious one-liner is wrong, and the way
 *    it is wrong is what stopped the bullet time page dead on the panel. The obvious
 *    version is
 *
 *        mag2 = (d0 * d0) + (d1 * d1) + (d2 * d2);      -- overflows in this scene
 *
 *    `d` is the vector from the impact point to a particle, and in this scene it is
 *    not small: the Logo is 106 cells of 2.1875 px, so a particle at its far edge is
 *    ~232 px -- 59 392 Q8.8 units -- from an impact on the near face, and
 *    59 392^2 = 3.5e9 does not fit an int32 (2.1e9). The wrapped sum is negative,
 *    and because signed overflow is undefined behaviour the compiler may assume it
 *    is not: both compilers **deleted** the `v <= 0` guard in `cgl_particles__isqrt_u32()`,
 *    whose loop then never terminated. The page stopped at the frame the ball
 *    touched the Logo, with the Logo still intact -- the symptom that brought this
 *    fix (see the WARNING in `cgl_particles__isqrt_u32()` for the measurements).
 *
 * The fix keeps the sum of squares in **uint32 with the low two bits of every square
 * carried separately**, so no intermediate exceeds 32 bits and nothing is undefined:
 *
 *     N = d0^2 + d1^2 + d2^2            the exact sum, up to 1.3e10: too big
 *     q = (d0^2 >> 2) + (d1^2 >> 2) + (d2^2 >> 2)   <= 3 * 65535^2 / 4 = 3.2e9, fits
 *     r = (d0^2 & 3)  + (d1^2 & 3)  + (d2^2 & 3)    <= 3, fits
 *     N = 4q + r                        exactly -- the two low bits are not lost
 *
 * `m = floor(sqrt(q))` then gives `|d| = 2m + delta` with `delta` in 0..2, because
 * `(2m+3)^2 = 4m^2 + 12m + 9` is greater than `N = 4q + r <= 4m^2 + 8m + 3` (using
 * `q - m^2 <= 2m` and `r <= 3`), and the two comparisons below pick `delta` out
 * exactly: they test `(2m+1)^2 <= N` and `(2m+2)^2 <= N`, rearranged into
 * `4*(q - m^2) + r >= 4m + 1` and `>= 8m + 4` so that every term is small.
 *
 * So the magnitude is **exact** -- which is what the original comment here demanded
 * ("doing the sum of squares at full precision... is what keeps the direction
 * exact") and what the int32 version could not deliver -- with no pre-scaling of `d`
 * (that quantises the direction) and no int64 (that drags the 64 bit shift helpers
 * into the firmware; see the note in app_logo_smash.c).
 *
 * Domain: `|d[i]| <= 65535`, i.e. the int16 world window against an int16 impact,
 * which is what `cgl_particles_burst()` is called with. Outside it the squares simply wrap --
 * defined behaviour, no UB -- the direction is then meaningless, but the magnitude
 * stays bounded and both loops terminate, which is the property the caller relies on.
 */
static int32_t cgl_particles__dir_mag(const int32_t *d)
{
    uint32_t q = 0u;
    uint32_t r = 0u;
    uint32_t m;
    uint32_t rest;
    uint32_t mag;
    int i;

    for (i = 0; i < 3; i++)
    {
        /* Cast to uint32 **before** the multiply: for a negative d[i] the product is
           then the exact square in the low 32 bits (it fits, per the domain above),
           while `d[i] * d[i]` in int32 would be the signed overflow this replaces. */
        const uint32_t v = (uint32_t)d[i];
        const uint32_t sq = v * v;

        q += (sq >> 2);
        r += (sq & 3u);
    }

    m = (uint32_t)cgl_particles__isqrt_u32(q);   /* floor(|d| / 2) */
    rest = (4u * (q - (m * m))) + r; /* |d|^2 - (2m)^2, at most 8m + 3 */
    mag = 2u * m;

    if (rest >= ((4u * m) + 1u)) /* (2m + 1)^2 <= |d|^2 ? */
    {
        mag++;
    }
    if (rest >= ((8u * m) + 4u)) /* (2m + 2)^2 <= |d|^2 ? -- still against the m from isqrt */
    {
        mag++;
    }
    return (int32_t)mag;
}

/**
 * Radial unit vector (Q8.8, components in -256..256) between two int32 fixed
 * point vectors, written into `out[0..2]`.
 *
 * Helper for the burst only. Scaling by `256/|d|` is one division -- the single
 * division in the whole engine, executed 500 times per **round** (which is 14 s
 * apart), not per frame. Keeping it out of the render path is the design rule;
 * eliminating it would mean a second reciprocal table for a path that runs 500
 * times a round, which is not where the cycles are.
 *
 * `|d|` comes from `cgl_particles__dir_mag()`, which is exact and overflow free: the previous
 * `(d[0] * d[0]) + ...` in int32 wrapped for any particle more than ~181 px from the
 * impact, and what that did to the page is documented there.
 */
static void cgl_particles__dir_unit(const int32_t *d, int32_t *out)
{
    const int32_t mag = cgl_particles__dir_mag(d);
    int i;

    if (mag < CGL_PARTICLES_ONE)
    {
        /* a degenerate (sub-pixel) direction: push along +z, which is "towards the
           viewer" and is the one direction a zero-length impact vector can mean */
        out[0] = 0;
        out[1] = 0;
        out[2] = CGL_PARTICLES_ONE;
        return;
    }
    for (i = 0; i < 3; i++)
    {
        out[i] = (d[i] * CGL_PARTICLES_ONE) / mag;
    }
}

/** Clamp an int32 into -128..127 (the int8 velocity range). */
static inline int8_t cgl_particles__clamp_i8(int32_t v)
{
    if (v > 127)
    {
        return 127;
    }
    if (v < -128)
    {
        return -128;
    }
    return (int8_t)v;
}

/**
 * Bucket for a Q8.8 depth. Bucket 0 is **farthest from the camera** and is
 * therefore painted first; `CGL_PARTICLES_BUCKETS - 1` is the nearest.
 *
 * The direction is the one thing in this file that was got wrong twice, so it is
 * derived here rather than asserted:
 *
 *      depth = 76800   (the camera distance, a particle at the origin)
 *      depth = 89064   (a particle 56 px out along the depth axis)  <- FARTHER
 *      depth = 32768   (the near clamp)
 *      depth = 98048   (the far clamp)
 *
 * with `unit = (far - near + 1) * 256 / CGL_PARTICLES_BUCKETS = 512`:
 *
 *      b = (far_q8 - depth) / unit
 *      depth 98048 -> 0     (farthest, painted first)
 *      depth 89064 -> 17
 *      depth 76800 -> 41
 *      depth 32768 -> 127   (nearest, painted last)
 *
 * so the **larger the depth, the smaller the bucket** -- which is what makes
 * "iterate the buckets in increasing order" the painter's algorithm. The mirrored
 * form `(depth - near) / unit` is equally plausible on paper and exactly backwards
 * in practice: it draws the nearest particles first and lets the far ones cover
 * them. The host test (two probes a known distance apart, whose depth is printed
 * next to their draw order) is what settles it.
 *
 * The two clamps are the near/far plane, and they collect an out of range particle
 * at the correct **end** of the order (a particle behind the camera joins the
 * nearest group, one past the far plane joins the farthest), never at the opposite
 * one. That property only holds for this direction, which is a second reason not to
 * mirror the expression.
 */
static inline uint8_t cgl_particles__bucket_of(int32_t depth_q8)
{
    int32_t b = ((CGL_PARTICLES_FAR_Q8 * CGL_PARTICLES_ONE) - depth_q8) / CGL_PARTICLES_BUCKET_UNIT_Q8;

    if (b < 0)
    {
        return 0u;
    }
    if (b >= CGL_PARTICLES_BUCKETS)
    {
        return (uint8_t)(CGL_PARTICLES_BUCKETS - 1);
    }
    return (uint8_t)b;
}

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

/** The single engine's published world constants (see `cgl_particles_world()`). */
static cgl_particles_world_t s_world;

const cgl_particles_world_t *cgl_particles_world(const cgl_particles_t *s)
{
    /* The world constants are a function of the compile time fit and of the round's
       spawn state, not of the camera, so they live in one file scope struct rather
       than in every system: a `cgl_particles_world_t` per engine would be 12 B of RAM for data
       that is identical, and the application has exactly one engine. */
    s_world.origin_x_q8 = CGL_PARTICLES_ORIGIN_X_Q8;
    s_world.origin_y_q8 = CGL_PARTICLES_ORIGIN_Y_Q8;
    s_world.pitch_q8 = CGL_PARTICLES_CELL_PITCH_Q8;
    s_world.max_active = (s != NULL) ? s->max_active : 0u;
    return &s_world;
}

cgl_particles_t *cgl_particles_init(void *storage, uint32_t storage_bytes)
{
    cgl_particles_t *s;

    if ((storage == NULL) || (storage_bytes < CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX)))
    {
        return NULL;
    }

    s = (cgl_particles_t *)storage;
    memset(s, 0, sizeof(*s));
    s->seed = 0x2545F491u; /* non-zero: xorshift is stuck at 0 */
    return s;
}

void cgl_particles_reset(cgl_particles_t *s)
{
    if (s == NULL)
    {
        return;
    }
    s->active = 0u;
    s->max_active = 0u;
    s->tick = 0u;
    s->sorted = false;
    memset(s->particle, 0, sizeof(s->particle));
}

void cgl_particles_build(cgl_particles_t *s, uint16_t dot_count,
              bool (*is_lit)(int i, int j),
              int32_t (*cell_x)(int i),
              int32_t (*cell_y)(int j))
{
    uint16_t n = 0u;
    int i;
    int j;

    if (s == NULL)
    {
        return;
    }

    memset(s->particle, 0, sizeof(s->particle));
    s->tick = 0u;
    s->sorted = false;

    /* One particle per **lit** dot, parked exactly on its cell: this is the
       "intact Logo" the page shows before the ball arrives, and because it is the
       same bitmap the smash page uses, the two pages show the same glyph. */
    for (j = 0; (j < 14) && (n < CGL_PARTICLES_MAX); j++)
    {
        for (i = 0; (i < 106) && (n < CGL_PARTICLES_MAX); i++)
        {
            if ((is_lit != NULL) && !is_lit(i, j))
            {
                continue;
            }
            s->particle[n].x = cgl_particles_clamp_i16(cell_x(i));
            s->particle[n].y = cgl_particles_clamp_i16(cell_y(j));
            s->particle[n].z = 0;
            s->particle[n].life = CGL_PARTICLES_LIFE_STEPS;
            /* the static per particle 6.2 normals: one byte each, and they are what
               gives the burst its depth spread without a per step random call */
            s->particle[n].bucket = 0u;
            n++;
        }
    }

    /* The remainder is the **impact debris**: spare particles at the centre, which
       `cgl_particles_burst()` will hand the largest velocities. Without them the cloud has
       exactly as many particles as the Logo has dots, and the ball itself never
       seems to contribute anything to the splash. */
    {
        uint16_t k = n;
        for (; k < CGL_PARTICLES_MAX; k++)
        {
            s->particle[k].x = 0;
            s->particle[k].y = 0;
            s->particle[k].z = 0;
            s->particle[k].life = CGL_PARTICLES_LIFE_STEPS;
        }
        n = CGL_PARTICLES_MAX;
    }

    (void)dot_count;
    s->active = n;
    s->max_active = n;
}

void cgl_particles_step(cgl_particles_t *s)
{
    uint16_t i;

    if (s == NULL)
    {
        return;
    }

    for (i = 0u; i < CGL_PARTICLES_MAX; i++)
    {
        cgl_particle_t *p = &s->particle[i];
        int32_t nx;
        int32_t ny;
        int32_t nz;

        if (p->life == 0u)
        {
            continue;
        }

        /* A particle the ball has not reached yet is **parked**: it is not stepped at
           all, so it stays exactly on the cell `cgl_particles_build()` put it on, keeps its life
           and does not sag under gravity. That is what makes the untouched part of the
           Logo stay intact while the ball carves through the rest (see `cgl_particles_impact()`). */
        if (p->hit == 0u)
        {
            continue;
        }

        /* damping first, then the force, then the integration: semi-implicit
           Euler, the same order the smash page's cgl_phys2d uses, so "what a particle
           does" and "what a rigid body does" agree. Doing it the other way round
           (integrate, then damp) makes the very first step integrate an undamped
           velocity and the burst jumps by one step's worth of overshoot. */
        p->vx = (int8_t)(p->vx - (p->vx >> CGL_PARTICLES_DAMP_SHIFT));
        p->vy = (int8_t)(p->vy - (p->vy >> CGL_PARTICLES_DAMP_SHIFT));
        p->vz = (int8_t)(p->vz - (p->vz >> CGL_PARTICLES_DAMP_SHIFT));

        /* gravity pulls the cloud down the screen, and the screen's down is world
           +y in this isometric basis (see CGL_PARTICLES_GRAVITY) */
        p->vy = cgl_particles__clamp_i8((int32_t)p->vy + CGL_PARTICLES_GRAVITY);

        nx = p->x + p->vx;
        ny = p->y + p->vy;
        nz = p->z + p->vz;

        /* Cull against the Q8.8 window **before** storing: what is written must
           always be a legal int16 Q8.8 coordinate, or the next frame starts from a
           wrapped value and the particle reappears mirrored on the far side of the
           Logo. CGL_PARTICLES_WORLD_HALF_SPAN_Q8 is the real physical bound; this test is the
           arithmetic one, and it is deliberately the looser of the two so that the
           engine stays correct if the fit is ever retuned. */
        if ((nx < -32768) || (nx > 32767) || (ny < -32768) || (ny > 32767) ||
            (nz < -32768) || (nz > 32767))
        {
            p->life = 0u;
            if (s->active > 0u)
            {
                s->active--;
            }
            continue;
        }

        p->x = (int16_t)nx;
        p->y = (int16_t)ny;
        p->z = (int16_t)nz;

        p->life = (uint8_t)(p->life - 1u);
        if (p->life == 0u)
        {
            if (s->active > 0u)
            {
                s->active--;
            }
        }
    }

    s->tick = (uint8_t)(s->tick + 1u);
    s->sorted = false;
}

int16_t cgl_particles_pos(const cgl_particles_t *s, uint16_t i, uint8_t axis)
{
    if ((s == NULL) || (i >= CGL_PARTICLES_MAX) || (s->particle[i].life == 0u))
    {
        return 0;
    }
    switch (axis)
    {
    case 0:
        return s->particle[i].x;
    case 1:
        return s->particle[i].y;
    default:
        return s->particle[i].z;
    }
}

int8_t cgl_particles_vel(const cgl_particles_t *s, uint16_t i, uint8_t axis)
{
    if ((s == NULL) || (i >= CGL_PARTICLES_MAX) || (s->particle[i].life == 0u))
    {
        return 0;
    }
    switch (axis)
    {
    case 0:
        return s->particle[i].vx;
    case 1:
        return s->particle[i].vy;
    default:
        return s->particle[i].vz;
    }
}

/**
 * Release **one** parked particle from an impact at `(impact_x, impact_y, impact_z)`:
 * radial direction from the impact point, `CGL_PARTICLES_V0_*` magnitude, the forward punch and
 * the symmetric jitter. Shared by `cgl_particles_burst()` (the whole sheet at once) and
 * `cgl_particles_impact()` (each dot as the ball reaches it) so the two can never diverge in
 * feel, and the tuning above applies to both.
 */
static void cgl_particles__release(cgl_particles_t *s, cgl_particle_t *p,
                       int32_t impact_x, int32_t impact_y, int32_t impact_z)
{
    int32_t d[3];
    int32_t u[3];
    int32_t in_plane;
    int32_t speed;
    int32_t r;

    d[0] = (int32_t)p->x - impact_x;
    d[1] = (int32_t)p->y - impact_y;
    d[2] = (int32_t)p->z - impact_z;

    /* Distance from the Logo plane along the depth axis. A particle that is
       already well in front of or behind the plane gets its direction from the
       plane's own (x, y) component alone; one sitting in the plane is thrown
       outwards in all three dimensions. Without this term the splash is a flat
       fan in one plane, and the orbit shows it as a line. */
    r = (d[2] < 0) ? -d[2] : d[2];
    in_plane = (r < CGL_PARTICLES_IMPACT_PLANE_Q8) ? CGL_PARTICLES_ONE : (CGL_PARTICLES_ONE / 2);

    cgl_particles__dir_unit(d, u);

    /* Radial speed: the direction is a unit vector, the magnitude is drawn
       from the deterministic stream, so the cloud is radial (the direction is
       preserved) rather than flat-topped at the int8 ceiling. */
    speed = cgl_particles__rand_range(s, CGL_PARTICLES_V0_MIN_Q8, CGL_PARTICLES_V0_MAX_Q8);

    /* The **forward punch**: every particle gets the same push along +x, the
       direction the ball was travelling. Without it the radial term alone makes
       the far half of the sheet fly away from the impact and the near half fly
       back towards it, so the net read is "the Logo implodes" rather than "the
       Logo is smashed" -- which is exactly how the first version looked.
       The impact arrives travelling towards +x and the isometric basis maps
       world +x to screen +x, so a punch on `vx` is what "the ball keeps going"
       should be.
       WARNING: the depth spread below must be **symmetric about zero**. The first
       version wrote it as `rand(0, CGL_PARTICLES_V0_DEPTH_Q8)`, a strictly positive bias of
       ~36 units on every particle; since the radial depth term is often smaller
       than that, the bias dominated and **all 500 particles flew towards the
       viewer**, so the cloud had no far side at all. The host test's "vz is spread
       in both directions" check is what caught it. */
    p->vx = cgl_particles__clamp_i8(((u[0] * in_plane) / CGL_PARTICLES_ONE) * speed / CGL_PARTICLES_ONE + CGL_PARTICLES_PUNCH_Q8 +
                        cgl_particles__rand_range(s, -CGL_PARTICLES_V0_JITTER_Q8, CGL_PARTICLES_V0_JITTER_Q8));
    p->vy = cgl_particles__clamp_i8(((u[1] * in_plane) / CGL_PARTICLES_ONE) * speed / CGL_PARTICLES_ONE +
                        cgl_particles__rand_range(s, -CGL_PARTICLES_V0_JITTER_Q8, CGL_PARTICLES_V0_JITTER_Q8));
    p->vz = cgl_particles__clamp_i8(((u[2] * in_plane) / CGL_PARTICLES_ONE) * speed / CGL_PARTICLES_ONE +
                        cgl_particles__rand_range(s, -CGL_PARTICLES_V0_DEPTH_Q8, CGL_PARTICLES_V0_DEPTH_Q8));

    p->life = CGL_PARTICLES_LIFE_STEPS;
    p->hit = 1u;
}

uint16_t cgl_particles_impact(cgl_particles_t *s, int32_t ball_x_q8, int32_t ball_r_q8)
{
    const int32_t front = ball_x_q8 + ball_r_q8;
    uint16_t released = 0u;
    uint16_t i;

    if (s == NULL)
    {
        return 0u;
    }

    /* The test is one dimensional on purpose: the ball travels along x, the sheet is
       a plane, and a particle is "reached" exactly when the ball's leading edge has
       passed it. A radial test against the ball's centre would leave the dots the ball
       drove straight over untouched (their distance from the centre is 0 at that
       moment, until it is too late), i.e. a hole-free tunnel through the Logo. */
    for (i = 0u; i < CGL_PARTICLES_MAX; i++)
    {
        cgl_particle_t *p = &s->particle[i];

        if ((p->life == 0u) || (p->hit != 0u) || ((int32_t)p->x > front))
        {
            continue;
        }

        /* The direction comes from the **ball's centre**, not from a fixed plane: that
           is what makes the damage local and gives each dot the sideways push of the
           contact it actually had, so the cloud fans out around the ball instead of
           radiating from one point of the Logo. */
        cgl_particles__release(s, p, ball_x_q8, 0, 0);
        released++;
    }

    if (released > 0u)
    {
        s->sorted = false;
    }
    return released;
}

void cgl_particles_burst(cgl_particles_t *s, int32_t impact_x, int32_t impact_y, int32_t impact_z)
{
    uint16_t i;

    if (s == NULL)
    {
        return;
    }

    /* The all-at-once form: everything alive is released from one point. The page
       uses `cgl_particles_impact()` instead (see the header for why), this is what the engine
       tests drive because a single release makes the direction field checkable. */
    for (i = 0u; i < CGL_PARTICLES_MAX; i++)
    {
        cgl_particle_t *p = &s->particle[i];

        if (p->life == 0u)
        {
            continue;
        }
        cgl_particles__release(s, p, impact_x, impact_y, impact_z);
    }

    s->active = CGL_PARTICLES_MAX;
    s->max_active = CGL_PARTICLES_MAX;
    s->tick = 0u;
    s->sorted = false;
}

/* -------------------------------------------------------------------------- */
/* Depth sort: counting sort (bucket sort) on the camera depth                */
/* -------------------------------------------------------------------------- */

uint16_t cgl_particles_sort(cgl_particles_t *s, const cgl_particles_cam_t *cam)
{
    /**
     * The scatter cursor, a **local**, not the prefix-sum table itself.
     *
     * Reusing `s->bucket[]` as both the prefix sum and the moving cursor is the
     * obvious way to save 256 B, and it is what the first version did -- and it was
     * wrong: pass 3 writes into `order[cursor]` while the *cursor array* is being
     * mutated, so any mistake in the cursor's start value silently deposits a
     * particle into another bucket's run. The measured symptom was 12 out of 500
     * particles drawn out of order (a particle at the far end of one bucket landing
     * in the middle of the previous one), i.e. a handful of dots flickering through
     * their neighbours -- the kind of defect that never reproduces on a screenshot.
     *
     * 128 x 2 B = 256 B of stack buys a scatter that cannot alias, and the stack is
     * the right place for it: the array is dead the moment this function returns.
     */
    uint16_t cursor[CGL_PARTICLES_BUCKETS];
    uint16_t i;
    uint16_t sum;
    uint16_t b;

    if ((s == NULL) || (cam == NULL))
    {
        return 0u;
    }

    /* pass 0: clear. 128 x 2 B = 256 B, one memset -- cheaper than a loop. */
    memset(s->bucket, 0, sizeof(s->bucket));

    /* pass 1: count. One multiply-add per particle, no comparison, no branch on
       the key: this is the whole reason a bucket sort beats a comparison sort
       here. The key is a bounded screen-space depth, so the counting array is a
       fixed 256 B rather than an O(n) distribution. */
    for (i = 0u; i < CGL_PARTICLES_MAX; i++)
    {
        const cgl_particle_t *p = &s->particle[i];

        if (p->life == 0u)
        {
            continue;
        }
        b = cgl_particles__bucket_of(cgl_particles_cam_depth(cam, p->x, p->y, p->z));
        s->bucket[b]++;
        /* stash the bucket in the particle: the sort needs it again in pass 3, and
           recomputing the key there would double the multiplies. 1 byte, and it is
           overwritten every frame. */
        s->particle[i].bucket = (uint8_t)b;
    }

    /* pass 2: prefix sum -> bucket b's run starts at order[sum of buckets < b].
       The order runs from bucket 0 (farthest) to CGL_PARTICLES_BUCKETS-1 (nearest),
       which is exactly the painter's order: far first, near last, later paints
       over earlier. */
    sum = 0u;
    for (b = 0u; b < CGL_PARTICLES_BUCKETS; b++)
    {
        const uint16_t n = s->bucket[b];

        s->bucket[b] = sum;
        cursor[b] = sum;
        sum = (uint16_t)(sum + n);
    }

    /* pass 3: scatter. Stable within a bucket, which is not required but means two
       particles at the same depth never swap between frames, so a particle cannot
       flicker through its neighbour. */
    for (i = 0u; i < CGL_PARTICLES_MAX; i++)
    {
        const cgl_particle_t *p = &s->particle[i];

        if (p->life == 0u)
        {
            continue;
        }
        s->order[cursor[p->bucket]++] = i;
    }

    s->sorted = true;
    return sum;
}

uint16_t cgl_particles_order_at(const cgl_particles_t *s, uint16_t k)
{
    if ((s == NULL) || (k >= CGL_PARTICLES_MAX))
    {
        return 0u;
    }
    return s->order[k];
}

/* -------------------------------------------------------------------------- */
/* Camera                                                                     */
/* -------------------------------------------------------------------------- */

void cgl_particles_cam_look(cgl_particles_cam_t *cam, uint8_t yaw_u8, int32_t orbit_q8,
                           cgl_particles_rect_t frame)
{
    int16_t sy;
    int16_t cy;
    int16_t sp;
    int16_t cp;

    if (cam == NULL)
    {
        return;
    }

    sy = cgl_particles_sin_q8(yaw_u8);
    cy = cgl_particles_cos_q8(yaw_u8);
    sp = cgl_particles_sin_q8((uint8_t)CGL_PARTICLES_CAM_PITCH_A);
    cp = cgl_particles_cos_q8((uint8_t)CGL_PARTICLES_CAM_PITCH_A);

    /*
     * Rotation = pitch (about X) then yaw (about Y). Composed symbolically first,
     * so that the six `CGL_PARTICLES_MUL`s below are a transcription and not a guess:
     *
     *   R = Ry(yaw) * Rx(pitch)
     *
     *       |  cy     sp*sy    -cp*sy |
     *   R = |  0      cp        sp    |
     *       |  sy    -sp*cy     cp*cy |
     *
     * Sanity anchors, which are what the host test pins:
     *   yaw = 0    -> m00=+1, m02=0, m11=cp, m22=cp   (facing the Logo head on)
     *   yaw = 90   -> m00=0, m02=-1, m11=cp, m22=-sp  (edge on: world +x is depth)
     *   yaw = 180  -> m00=-1, the row 2 sign flips
     *   yaw = 270  -> m00=0, m02=+1
     *
     * The engine keeps a **left handed** camera space on purpose: +x is to the
     * screen right, +y is screen down and +z is *away* from the viewer, so that
     * 'larger depth = further' matches the painter's algorithm and the reciprocal
     * table's direction. A right handed camera would flip every sign in row 2 and
     * invert the depth sort.
     */
    cam->m00 = cy;
    cam->m01 = (int16_t)CGL_PARTICLES_MUL(sp, sy);
    cam->m02 = (int16_t)(-CGL_PARTICLES_MUL(cp, sy));

    cam->m10 = 0;
    cam->m11 = cp;
    cam->m12 = sp;

    cam->m20 = sy;
    cam->m21 = (int16_t)(-CGL_PARTICLES_MUL(sp, cy));
    cam->m22 = (int16_t)CGL_PARTICLES_MUL(cp, cy);

    cam->dist_q8 = CGL_PARTICLES_PX(CGL_PARTICLES_CAM_DIST_PX);
    cam->orbit_z_q8 = orbit_q8;
    cam->focal_q8 = CGL_PARTICLES_PX(CGL_PARTICLES_CAM_FOCAL_PX);
    cam->ox_q8 = CGL_PARTICLES_PX((frame.x0 + frame.x1 + 1) / 2);
    cam->oy_q8 = CGL_PARTICLES_PX((frame.y0 + frame.y1 + 1) / 2);
}

/* -------------------------------------------------------------------------- */
/* Readouts                                                                   */
/* -------------------------------------------------------------------------- */

uint16_t cgl_particles_active(const cgl_particles_t *s)
{
    return (s != NULL) ? s->active : 0u;
}

uint16_t cgl_particles_max_active(const cgl_particles_t *s)
{
    return (s != NULL) ? s->max_active : 0u;
}

uint8_t cgl_particles_tick(const cgl_particles_t *s)
{
    return (s != NULL) ? s->tick : 0u;
}

#endif /* CGL_CFG_PARTICLES */

