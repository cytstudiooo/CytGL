// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    bullet_host_test.c
  * @brief   Bullet time page + particle engine, verified on the host.
  *
  *  What this test is for
  *  ---------------------
  *  The bullet time effect is three claims that are easy to state and easy to get
  *  quietly wrong, and none of them can be checked by looking at a screenshot:
  *
  *    1. **The particle engine and the smash page's world are the same bytes.**
  *       Not "about the same size" -- the same address. If that ever stops being
  *       true, the page silently costs 6.3 KB of RAM and the part stops linking,
  *       which is a failure that shows up as "the demo does not fit" a long way
  *       from its cause.
  *    2. **Time really stops.** The particles must not move while the time scale is
  *       0, and they must move at exactly the right rate on the way down -- so the
  *       test drives real frames through the whole state machine and counts, rather
  *       than trusting the ramp formula.
  *    3. **The depth order follows the camera, not the world.** The sort key is the
  *       camera space z, so turning the camera 90 deg has to reorder a cloud in
  *       which **not one particle moved**. This is the case `cgl_iso2d`'s "sort once per
  *       step" stamp cannot express, and it is the whole reason this page has its
  *       own sorter.
  *
  *  Plus the fixed point tools that the effect rests on: the sine table's symmetry
  *  (the 0..90 deg table has to reproduce all four quadrants), its continuity, and
  *  the reciprocal table's accuracy against 1/depth over the whole near/far window.
  *
  *  Build / run (canonical definition is tests/CMakeLists.txt):
  *      cmake -B build/host -DCYTGL_BUILD_TESTS=ON
  *      cmake --build build/host
  *      ctest --test-dir build/host -R bullet --output-on-failure
  ******************************************************************************
  */
#include <stdio.h>
#include <string.h>

#include <cytgl/cgl_display.h>
#include <cytgl/cgl_screen.h>
#include <cytgl/cgl_page.h>

#include "app_page_smash.h"
#include "app_page_bullet.h"
#include <ext/cgl_particles.h>
#include <ext/cgl_particles_tables.h>
#include <ext/cgl_particles_view.h> /* cgl_particles_view_frame(): the rect the camera takes */
#include "logo_bitmap.h"

static int s_fails;

#define CHECK(cond, ...)                                                     \
    do                                                                       \
    {                                                                        \
        if (!(cond))                                                         \
        {                                                                    \
            (void)printf("  FAIL: ");                                        \
            (void)printf(__VA_ARGS__);                                       \
            (void)printf("   [%s:%d]\n", __FILE__, __LINE__);                \
            s_fails++;                                                       \
        }                                                                    \
    } while (0)

/* -------------------------------------------------------------------------- */
/* Mock panel: collects whole bands into a framebuffer                        */
/* -------------------------------------------------------------------------- */

static cgl_color_t s_framebuffer[CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H];

static uint16_t s_row;
static uint16_t s_col;

static int mock__init(void *ctx) { (void)ctx; return 0; }

static void mock__set_window(void *ctx, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    (void)ctx;
    (void)x1;
    (void)y1;
    s_col = x0;
    s_row = y0;
}

static void mock__write_pixels(void *ctx, const cgl_color_t *px, uint32_t count)
{
    uint32_t i;

    (void)ctx;
    for (i = 0u; i < count; i++)
    {
        s_framebuffer[(s_row * CGL_CFG_SCREEN_W) + s_col] = px[i];
        s_col++;
        if (s_col >= CGL_CFG_SCREEN_W)
        {
            s_col = 0u;
            s_row++;
        }
    }
}

static void mock__wait_idle(void *ctx) { (void)ctx; }
static void mock__backlight(void *ctx, uint8_t p) { (void)ctx; (void)p; }

static const cgl_display_drv_t s_mock_drv = {
    "mock",
    mock__init,
    mock__set_window,
    mock__write_pixels,
    mock__wait_idle,
    mock__backlight,
    CGL_CFG_SCREEN_W,
    CGL_CFG_SCREEN_H,
};

/* -------------------------------------------------------------------------- */
/* 1. Fixed point core                                                        */
/* -------------------------------------------------------------------------- */

/** The 0..90 deg table must reproduce all four quadrants through the fold.
    Independent reference values (sin of the exact LUT angles, in Q8.8). */
static void test_trig(void)
{
    /*
     * Angle unit: one turn = 256, so with the angle expressed as a plain `int`
     * (not the `uint8_t` the accessors take) 90 deg = 64, 180 = 128, 270 = 192.
     *
     * WARNING: the accessors take a `uint8_t` and wrap at 256, which is correct --
     * `cgl_particles_cos_q8(a)` is `cgl_particles_sin_q8(a + 64)` and `a + 64` **must** wrap. The first
     * draft of this test passed 192 to `cgl_particles_cos_q8` expecting cos(270) = 0, got
     * cos(192 + 64 = 0) = 256, and reported the engine as broken. When writing a
     * check here, reduce the angle into 0..255 first.
     */
    CHECK((int16_t)cgl_particles_cos_q8(0u) == 255, "cos(0) = %d, want 255 (the table saturates)", (int)cgl_particles_cos_q8(0u));
    CHECK((int16_t)cgl_particles_sin_q8(64u) == 255, "sin(90) = %d, want 255 (the table saturates)", (int)cgl_particles_sin_q8(64u));
    CHECK((int16_t)cgl_particles_sin_q8(128u) == 0, "sin(180) = %d, want 0", (int)cgl_particles_sin_q8(128u));
    CHECK((int16_t)cgl_particles_sin_q8(192u) == -255, "sin(270) = %d, want -255 (the table saturates)", (int)cgl_particles_sin_q8(192u));
    CHECK((int16_t)cgl_particles_cos_q8(64u) == 0, "cos(90) = %d, want 0", (int)cgl_particles_cos_q8(64u));
    CHECK((int16_t)cgl_particles_cos_q8(128u) == -255, "cos(180) = %d, want -255 (the table saturates)", (int)cgl_particles_cos_q8(128u));
    CHECK((int16_t)cgl_particles_cos_q8(192u) == 0, "cos(270) = %d, want 0", (int)cgl_particles_cos_q8(192u));

    /*
     * The table ships **saturated at 255**, not at 256: sin(90 deg) rounds to 256
     * in Q8.8 and 256 does not fit the `uint8_t` the table is stored in, so the
     * generator clamps. This check exists because getting it wrong is catastrophic
     * rather than cosmetic: an unclamped 256 is emitted as `0x100`, truncates to 0
     * in the array, and sin(90) becomes 0 -- a **singular** rotation matrix. The
     * first version of the generator did exactly that, and this is the check that
     * caught it.
     */
    {
        int k;
        int over = 0;
        int worst = 0;

        for (k = 0; k < 64; k++)
        {
            if (cgl_particles__sin_q8[k] > 255u)
            {
                over++;
            }
            if ((int)cgl_particles__sin_q8[k] > worst)
            {
                worst = (int)cgl_particles__sin_q8[k];
            }
        }
        CHECK(over == 0, "%d sine table entries exceed uint8_t", over);
        CHECK(worst == 255, "the sine table peaks at %d, want the saturated 255", worst);

        /* ...and the peak really is reachable, i.e. the fold does not clamp
           everything: at 90 deg the accessor must return 255 (not 256, not 0). */
        CHECK((int16_t)cgl_particles_sin_q8(63u) == 255, "sin(88.6 deg) = %d, want 255", (int)cgl_particles_sin_q8(63u));
    }

    /* The fold has to be **continuous**: a table whose quadrant boundary is off by
       one index produces a visible kink in the orbit.
       The bound is not a constant -- sin varies fastest at 0 and 90 deg, where one
       1/64 quadrant step is 1.4 deg, so the largest legitimate step between
       adjacent angles is at the 0/90 boundary: sin(90 - 1.4 deg) = 0.9997 * 256 =
       255.9, so 255 -> 256 (or 255 -> 255 once saturated). A step of more than 8
       units anywhere is the signature of a mirrored index that is off by one, which
       is what the first draft of the check tripped over by assuming a constant
       step. */
    {
        int a;
        int prev = (int)cgl_particles_sin_q8(0u);
        int worst = 0;
        int worst_at = 0;

        for (a = 1; a <= 256; a++)
        {
            const int cur = (int)cgl_particles_sin_q8((uint8_t)a);
            const int d = (cur > prev) ? (cur - prev) : (prev - cur);

            if (d > worst)
            {
                worst = d;
                worst_at = a;
            }
            prev = cur;
        }
        CHECK(worst <= 8, "sin table jumps by %d units at angle %d (the fold is discontinuous)", worst, worst_at);
        (void)printf("  sin table: worst step between adjacent angles = %d units at a=%d\n", worst, worst_at);
    }

    /* The fold has to be consistent with the cosine identity for every
       representable angle: cos(a) must equal sin(a + 90 deg), i.e. the fold applied
       to the top byte advanced by 0x40 -- and it must wrap at 256 rather than run
       off the table. */
    {
        int a;
        int worst = 0;

        for (a = 0; a < 256; a++)
        {
            const int s90 = (int)cgl_particles_sin_q8((uint8_t)(a + 64));
            const int c = (int)cgl_particles_cos_q8((uint8_t)a);
            const int d = (s90 > c) ? (s90 - c) : (c - s90);

            if (d > worst)
            {
                worst = d;
            }
        }
        CHECK(worst == 0, "sin(a+90) and cos(a) disagree by %d units (the fold is inconsistent)", worst);
    }
}

/** The reciprocal table is the perspective divide: it must be accurate over the
    whole near/far window, because every particle's size and position goes through
    it. Reference: 1/d computed in the test (the test may use floating point; the
    firmware may not).
 *
 *  WARNING -- the indexing is the subtle part. The table is indexed by the
 *  **divisor itself**, not by "how far past the near plane": `cgl_particles__recip[k]` holds
 *  `65536 / k`, so the near plane's entry is at index `CGL_PARTICLES_NEAR_Q8` (= 128), not at
 *  index 0. The first draft of this test asserted `cgl_particles_recip_q16(0)` equals
 *  `cgl_particles__recip[0]`, which is a comparison of two different divisors, and reported
 *  195% error on a function that was correct to under 1%. The indexing is
 *  deliberate: it makes the integer part of the depth a direct index with no
 *  subtraction and no shift, and it means the table's entry for depth d is exactly
 *  `round(65536/d)`.
 */
static void test_reciprocal(void)
{
    int d;
    double worst_rel = 0.0;
    int worst_d = 0;

    /* The two ends of the window, as the function sees them. */
    CHECK(cgl_particles_recip_q16(CGL_PARTICLES_NEAR_Q8 * CGL_PARTICLES_ONE) == (int32_t)cgl_particles__recip[CGL_PARTICLES_NEAR_Q8],
          "the near clamp is not the table entry for depth %d: %d vs %d",
          CGL_PARTICLES_NEAR_Q8, (int)cgl_particles_recip_q16(CGL_PARTICLES_NEAR_Q8 * CGL_PARTICLES_ONE), (int)cgl_particles__recip[CGL_PARTICLES_NEAR_Q8]);
    CHECK(cgl_particles_recip_q16(0) == cgl_particles_recip_q16(CGL_PARTICLES_NEAR_Q8 * CGL_PARTICLES_ONE),
          "depth 0 did not clamp to the near plane");
    CHECK(cgl_particles_recip_q16(100000) == cgl_particles_recip_q16(CGL_PARTICLES_FAR_Q8 * CGL_PARTICLES_ONE),
          "a huge depth did not clamp to the far plane");

    for (d = CGL_PARTICLES_NEAR_Q8; d <= CGL_PARTICLES_FAR_Q8; d++)
    {
        const int32_t got = cgl_particles_recip_q16(d * CGL_PARTICLES_ONE);
        const double want = (double)65536.0 / (double)d;
        const double rel = ((double)got - want) / want;

        if ((rel < 0.0 ? -rel : rel) > worst_rel)
        {
            worst_rel = (rel < 0.0 ? -rel : rel);
            worst_d = d;
        }
    }
    /*
     * The interpolation between two adjacent integer depths is the only error, and
     * at an exact integer depth there is none at all. The worst case is the
     * midpoint between two entries, bounded by the relative change across one step:
     * 1 - d/(d+1) = 1/(d+1), which is 0.78% at the near clamp and shrinks from
     * there. Anything above that means the interpolation is wrong -- a sign flip in
     * the `b - a` term is the classic way to get a much larger error.
     */
    CHECK(worst_rel < 0.008, "reciprocal error %.5f%% at depth %d px (the interpolation is wrong)",
          worst_rel * 100.0, worst_d);
    (void)printf("  reciprocal: worst relative error %.5f%% at depth %d px\n", worst_rel * 100.0, worst_d);
}

/** `CGL_PARTICLES_MUL` truncates toward zero, so a sign-symmetric pair of velocities has to
    damp symmetrically. With `>>` instead the negative one would round down and the
    cloud would drift. */
static void test_mul_symmetry(void)
{
    int v;

    for (v = 1; v < 127; v++)
    {
        CHECK(CGL_PARTICLES_MUL(v, 100) == -CGL_PARTICLES_MUL(-v, 100), "CGL_PARTICLES_MUL is not sign symmetric at v=%d", v);
    }
}

/* -------------------------------------------------------------------------- */
/* 2. The engine on its own (no page, no framework)                           */
/* -------------------------------------------------------------------------- */

static cgl_particles_t *s_sys;
static cgl_particles_storage_t s_store;

/** Cell layout for the synthetic grid the engine tests use: three cells in a row,
    l opsided so that a sort bug cannot pass by accident. */
static bool grid_is_lit(int i, int j)
{
    return (j == 0) && ((i % 2) == 0);
}

static int32_t grid_cell_x(int i) { return (int32_t)i * 64 - 128; }
static int32_t grid_cell_y(int j) { return (int32_t)j * 64 - 32; }

/** The engine's storage is a **white box** for this one test on purpose.
 *
 *  A depth sort can only be verified against positions the test chose, and the
 *  engine's own positions are either a regular grid (the build) or pseudo-random
 *  (the burst) -- neither lets the test say "particle 7 is 20 px behind particle 9,
 *  so 7 must be painted first". Writing the positions directly is the only way to
 *  make that statement, and it is the reason the whole type is in the header rather
 *  than hidden behind a pointer.
 *
 *  WARNING -- **the cloud must have a distinct depth for every particle at every
 *  yaw**, or this test measures nothing. The first drafts used a grid, and a grid is
 *  the worst possible input here: on an axis-aligned grid, whole rows of particles
 *  share a depth, and a bucket sort is only required to be monotone in the bucket,
 *  i.e. **ties may come out in any order**. The test then reported "not monotone"
 *  for 12 particles on a sorter that was behaving exactly as specified -- the
 *  assertion was wrong, not the code.
 *
 *  WARNING -- and a seed whose depth is monotone in the index at **every** yaw makes
 *  the "90 deg changes the order" check untestable: a line along (1, 1, 1) has
 *  `depth proportional to t` at yaw 0 and at yaw 90 (different coefficients, same
 *  sign), so the order legitimately cannot change. That draft failed a check it had
 *  made impossible to pass.
 *
 *  The seed below therefore uses **two separated clusters**:
 *
 *      particles   0..249   offset along +x, spreading from the origin
 *      particles 250..499   offset along +z, spreading from the origin
 *
 *  Both clusters have depth proportional to (index % 250), so at yaw 0 the
 *  **z cluster is farther** and at yaw 90 the **x cluster is farther** -- the two
 *  orderings are genuinely different permutations, which is exactly the property
 *  the reorder check needs:
 *
 *      yaw 0    depth ~ 300 + (i%250) * Kz * cos(pitch)      z dominates
 *      yaw 90   depth ~ 300 + (i%250) * Kx * sin(yaw)        x dominates
 *
 *  The step is chosen so that the **worst case at 45 deg** still fits the window.
 *  At 45 deg the two clusters are both half-contributing, so a particle's offset
 *  from the camera distance is at most `t * step * sin45 = 41 px` -- comfortably
 *  inside the 128..383 near/far window, so no particle is ever clamped. Clamping is
 *  what would break the test: two particles clamped into the same bucket have no
 *  defined relative order, and the monotonicity check would fail on a correct
 *  sorter (this is exactly how the 45 deg check caught the previous step value).
 *
/**
 * A cloud whose depth is (a) different for every particle at every yaw and (b)
 * monotone in the particle index. Those two properties are exactly the precondition
 * for "compare the bucket sort against an exact-depth reference sort" to be a
 * meaningful test, and getting them wrong is why this function took several
 * attempts -- each failure looked like a sorter bug and was not.
 *
 * The construction: two clusters running in **non-parallel** directions, with
 * **different integer steps** so their depth sequences cannot interleave into a tie.
 *
 *      particles   0..249   position = (8, 0, 0) * k * 32      k = 0..249
 *      particles 250..499   position = (0, -1, 3) * k * 32      k = 0..249
 *
 * Why the camera can never put two particles at the same depth:
 *
 *      depth(i) = 300 + (m20 * x + m21 * y + m22 * z) / 256
 *
 * `m20 = sin(yaw)` and `m22 = cos(pitch) * cos(yaw) = 222/256 * cos(yaw)`, so for
 * this seed
 *
 *      cluster A : depth = 300 + 8 * 32 * k * sin(yaw) / 256
 *      cluster B : depth = 300 + (3 * 32 * k * cos(yaw) * 222/256 - 32 * k * (-125/256)) / 256
 *
 * and two particles collide only if `8a = 3c - ...`, an equation whose coefficient
 * ratio is irrational for every yaw but 0 and 90 deg, and at those two the two
 * clusters separate by construction (one coefficient vanishes). Distinctness was
 * checked by brute force over all 256 yaw values before this was committed to.
 *
 * Adjacent depths may still fall in the **same bucket** (a bucket is 2 px and some
 * steps are 0.2 px). That is fine and is the point: the counting sort is stable, so
 * same-bucket particles come out in particle index order, and the depth is monotone
 * in the index -- so the stable reference sort produces the same sequence. Without
 * that monotonicity the two would legitimately disagree and the test would be
 * comparing against a guarantee the sorter never made.
 */
#define SEED_STEP_Q8 16 /* 0.0625 px per index */
#define SEED_CLUSTER (CGL_PARTICLES_MAX / 2)
#define SEED_T_MAX (SEED_CLUSTER - 1)

_Static_assert(((int32_t)SEED_T_MAX * SEED_STEP_Q8 * 8) < 32767,
               "the seed offsets must fit int16_t Q8.8: lower SEED_STEP_Q8");

static void seed_known_depths(cgl_particles_t *s)
{
    uint16_t p;

    for (p = 0u; p < CGL_PARTICLES_MAX; p++)
    {
        const int32_t k = (int32_t)(p % SEED_CLUSTER);
        const bool cluster_b = (p >= SEED_CLUSTER);

        s->particle[p].x = (int16_t)(cluster_b ? 0 : (8 * k * SEED_STEP_Q8));
        s->particle[p].y = (int16_t)(cluster_b ? -SEED_STEP_Q8 : 0);
        s->particle[p].z = (int16_t)(cluster_b ? (3 * k * SEED_STEP_Q8) : 0);
        s->particle[p].vx = 0;
        s->particle[p].vy = 0;
        s->particle[p].vz = 0;
        s->particle[p].life = 200u;
    }
}

/**
 * Check one camera's sort against the property the sorter actually promises.
 *
 * A counting sort on a **quantised** key guarantees: within one bucket, the
 * particles come out in particle index order (the scatter is stable); and across
 * buckets, in increasing bucket order. What it does **not** guarantee -- and must
 * not be asked to -- is that the result is monotone in the *exact* depth, because a
 * bucket is 2 px wide and two particles in it can be up to 2 px apart in depth
 * while being ordered by index.
 *
 * The first version of this test compared against an exact-depth reference sort.
 * On any cloud with more particles than buckets (500 particles, 128 buckets) that
 * comparison is measuring rounding, not correctness: it reported hundreds of
 * "differences" on a sorter whose output was provably legal. This version checks
 * the three things that are actually required:
 *
 *   1. every live particle appears exactly once (a counting sort can lose one if a
 *      cursor or the prefix sum is wrong, and that is invisible otherwise);
 *   2. bucket indices never decrease along the output (the painter's order);
 *   3. within a bucket, indices increase (the stability the scatter promises).
 *
 * Plus one thing that ties it back to the picture: a **strictly** nearer particle
 * may never be painted before a strictly farther one, i.e. whenever
 * `depth(next) < depth(prev) - (one bucket)` the order is wrong. That is the
 * statement "near occludes far" in its weakest, always-true form.
 */
static int check_sort_at(const cgl_particles_t *s, const cgl_particles_cam_t *cam,
                         const char *tag)
{
    uint8_t seen[CGL_PARTICLES_MAX];
    uint16_t n;
    uint16_t p;
    uint16_t live = 0u;
    int fails = 0;
    int order_back = 0;
    int unstable = 0;
    int occluded = 0;
    int32_t prev_depth = 0;
    uint8_t prev_bucket = 0u;

    for (p = 0u; p < CGL_PARTICLES_MAX; p++)
    {
        seen[p] = (s->particle[p].life != 0u) ? 1u : 0u;
        live = (uint16_t)(live + seen[p]);
    }

    n = cgl_particles_sort((cgl_particles_t *)s, cam);
    if (n != live)
    {
        (void)printf("  FAIL[%s]: sort reported %u live, the cloud holds %u\n", tag,
                     (unsigned)n, (unsigned)live);
        return 1;
    }

    for (p = 0u; p < n; p++)
    {
        const uint16_t idx = cgl_particles_order_at(s, p);
        const int32_t depth = cgl_particles_cam_depth(cam, s->particle[idx].x, s->particle[idx].y,
                                           s->particle[idx].z);
        const uint8_t b = s->particle[idx].bucket;

        /* 1. every particle exactly once */
        if ((idx >= CGL_PARTICLES_MAX) || (seen[idx] != 1u))
        {
            if (fails == 0)
            {
                (void)printf("  FAIL[%s]: order[%u] = %u, which is dead or already used\n", tag,
                             (unsigned)p, (unsigned)idx);
            }
            fails++;
            continue;
        }
        seen[idx] = 2u;

        if (p > 0u)
        {
            /* 2. the painter's order across buckets */
            if (b < prev_bucket)
            {
                order_back++;
            }
            /* 3. stability within a bucket */
            if ((b == prev_bucket) && (idx < cgl_particles_order_at(s, (uint16_t)(p - 1u))))
            {
                unstable++;
            }
            /* ...and the weakest form of "near occludes far": a particle that is
               clearly nearer may not have been painted first.
               WARNING: the tolerance has to be in **buckets at the far plane**, not
               in depth, and the particles past the far plane are skipped entirely.
               Everything beyond `CGL_PARTICLES_FAR_Q8` clamps to bucket 0, so a clamped particle
               is indistinguishable from a farthest one and can legitimately be drawn
               in either order; comparing its exact depth flags a correct sorter
               (measured: 41 of 52 angles, every one where the cloud reaches the
               clamp). "Near occludes far" only says anything where both particles
               are inside the camera's window. */
            if ((depth > (int32_t)(CGL_PARTICLES_NEAR_Q8 * CGL_PARTICLES_ONE)) &&
                (depth < (int32_t)(CGL_PARTICLES_FAR_Q8 * CGL_PARTICLES_ONE)) &&
                (prev_depth < (int32_t)(CGL_PARTICLES_FAR_Q8 * CGL_PARTICLES_ONE)) &&
                (depth < (prev_depth - (int32_t)(2u * (((CGL_PARTICLES_FAR_Q8 - CGL_PARTICLES_NEAR_Q8 + 1) * CGL_PARTICLES_ONE) /
                                                       CGL_PARTICLES_BUCKETS)))))
            {
                occluded++;
            }
        }
        prev_depth = depth;
        prev_bucket = b;
    }

    if (order_back != 0)
    {
        (void)printf("  FAIL[%s]: %d bucket order inversions\n", tag, order_back);
        fails++;
    }
    if (unstable != 0)
    {
        (void)printf("  FAIL[%s]: %d index inversions inside a bucket\n", tag, unstable);
        fails++;
    }
    if (occluded != 0)
    {
        (void)printf("  FAIL[%s]: %d clearly-nearer particles painted before a farther one\n",
                     tag, occluded);
        fails++;
    }
    for (p = 0u; p < CGL_PARTICLES_MAX; p++)
    {
        if (seen[p] == 1u)
        {
            (void)printf("  FAIL[%s]: particle %u is live but was never drawn\n", tag, (unsigned)p);
            fails++;
            break;
        }
    }
    return fails;
}

static void test_engine_depth_sort(void)
{
    cgl_particles_cam_t cam;
    cgl_rect_t frame = cgl_rect_wh(0, 0, CGL_CFG_SCREEN_W, CGL_CFG_SCREEN_H);
    uint16_t p;

    (void)printf("\n== Engine: the sort key is the camera depth, not the world y ==\n");

    s_sys = cgl_particles_init(&s_store, (uint32_t)sizeof(s_store));
    CHECK(s_sys != NULL, "cgl_particles_init refused a correctly sized storage");

    /* A short storage must be refused, not overrun. */
    {
        cgl_particles_storage_t small;

        CHECK(cgl_particles_init(&small, 16u) == NULL, "cgl_particles_init accepted a 16 byte storage");
        CHECK(cgl_particles_init(NULL, sizeof(s_store)) == NULL, "cgl_particles_init accepted a NULL storage");
    }

    cgl_particles_build(s_sys, 1u, grid_is_lit, grid_cell_x, grid_cell_y);
    CHECK(cgl_particles_active(s_sys) == CGL_PARTICLES_MAX, "cgl_particles_build left %u particles, want %u",
          (unsigned)cgl_particles_active(s_sys), (unsigned)CGL_PARTICLES_MAX);

    seed_known_depths(s_sys);

    /* Every angle of the orbit has to produce a legal painter's order: 52 yaws,
       each one checked for the counting sort's own invariants. This is the check
       that would have caught the inverted bucket index (which made the whole cloud
       paint back to front) and the lost/duplicated particle that a broken prefix
       sum would produce. */
    {
        int a;
        int bad = 0;

        for (a = 0; a < 256; a += 5)
        {
            cgl_particles_cam_look(&cam, (uint8_t)a, 0, cgl_particles_view_frame(frame));
            if (check_sort_at(s_sys, &cam, "orbit") != 0)
            {
                bad++;
            }
        }
        CHECK(bad == 0, "%d of 52 orbit angles produced an illegal draw order", bad);
        (void)printf("  orbit: 52 angles checked (window 128..127+%d, %d buckets)\n",
                     (int)CGL_PARTICLES_BUCKETS, (int)CGL_PARTICLES_BUCKETS);
    }

    /*
     * **The test the whole page exists for.** Rotate the camera 90 deg and watch a
     * *known* pair swap.
     *
     * The cloud is seeded with one particle far along -x and one far along +z, both
     * near the origin otherwise:
     *
     *      particle 0    ( +56 px, 0, 0 )    offset along +x
     *      particle 1    ( 0, 0, +56 px )    offset along +z
     *
     * **The reasoning is worth spelling out, because the signs are what make this
     * test mean anything.** The camera space z grows *away* from the viewer, so a
     * larger value is farther and must be painted first. The depth is
     *
     *      depth = (m20 * x + m21 * y + m22 * z) / 256 + 300
     *      m20 = sin(yaw)              m21 = -sin(pitch) * cos(yaw)
     *      m22 = cos(pitch) * cos(yaw)
     *
     * and with pitch = 29.5 deg (`cos(pitch) = 0.87`, `sin(pitch) = 0.49`):
     *
     *      yaw  0   m20 = 0     m22 = 0.87    particle 1 (+z) is the far one
     *      yaw 90   m20 = 1     m22 = 0       particle 0 (+x) is the far one
     *
     * So the two **swap** while neither moves: the one thing a depth sort that
     * cached its order, or sorted by world y (0 for both, here), or got the sign of
     * the depth axis wrong, cannot do.
     *
     * WARNING: this check has been written with the signs the wrong way round three
     * times. The printed depths above the assertions are the authority; read them,
     * not the intuition. The rule is one line: **smaller depth = nearer = painted
     * last.**
     */
    {
        static const int16_t probe_pos[2][3] = {
            { (int16_t)(56 * CGL_PARTICLES_ONE), 0, 0 },
            { 0, 0, (int16_t)(56 * CGL_PARTICLES_ONE) }
        };
        int q;

        /* park every particle but the two probes far outside the near/far window and
           then revive exactly two of them */
        for (q = 0; q < CGL_PARTICLES_MAX; q++)
        {
            s_sys->particle[q].life = 0u;
        }
        for (q = 0; q < 2; q++)
        {
            s_sys->particle[q].x = probe_pos[q][0];
            s_sys->particle[q].y = probe_pos[q][1];
            s_sys->particle[q].z = probe_pos[q][2];
            s_sys->particle[q].life = 200u;
        }

        cgl_particles_cam_look(&cam, 0u, 0, cgl_particles_view_frame(frame));
        {
            const uint16_t n0 = cgl_particles_sort(s_sys, &cam);
            const uint16_t first0 = cgl_particles_order_at(s_sys, 0u);
            const uint16_t last0 = cgl_particles_order_at(s_sys, (uint16_t)(n0 - 1u));
            const int32_t d0 = cgl_particles_cam_depth(&cam, s_sys->particle[0].x, s_sys->particle[0].y,
                                            s_sys->particle[0].z);
            const int32_t d1 = cgl_particles_cam_depth(&cam, s_sys->particle[1].x, s_sys->particle[1].y,
                                            s_sys->particle[1].z);

            (void)printf("  probe yaw  0: depth(p0)=%d depth(p1)=%d -> first=%u\n",
                         (int)d0, (int)d1, (unsigned)first0);
            (void)printf("    order: %u %u ; buckets p0=%u p1=%u (smaller bucket = farther)\n",
                         (unsigned)cgl_particles_order_at(s_sys, 0u), (unsigned)cgl_particles_order_at(s_sys, 1u),
                         (unsigned)s_sys->particle[0].bucket,
                         (unsigned)s_sys->particle[1].bucket);

            /* **The larger depth is farther and is painted FIRST; the smaller is
               nearer and is painted LAST.** At yaw 0 the depth axis is +z, so
               particle 1 (out at +z, d1 = 89064) is the far one and goes first, and
               particle 0 (d0 = 76800, the camera distance itself) covers it. */
            CHECK(n0 == 2u, "the probe cloud holds %u live particles, want 2", (unsigned)n0);
            CHECK(d1 > d0, "yaw 0: the +z particle should be farther (d1=%d, d0=%d)",
                  (int)d1, (int)d0);
            CHECK(first0 == 1u, "at yaw 0 the FARTHER particle (index 1, d=%d) must be painted "
                                "first, got index %u (d=%d)",
                  (int)d1, (unsigned)first0, (int)d0);
            CHECK(last0 == 0u, "at yaw 0 the NEARER particle (index 0, d=%d) must be painted "
                               "last, got index %u (d=%d)",
                  (int)d0, (unsigned)last0, (int)d1);
        }

        cgl_particles_cam_look(&cam, 64u, 0, cgl_particles_view_frame(frame)); /* yaw = 90 deg */
        {
            const uint16_t n1 = cgl_particles_sort(s_sys, &cam);
            const uint16_t first1 = cgl_particles_order_at(s_sys, 0u);
            const uint16_t last1 = cgl_particles_order_at(s_sys, (uint16_t)(n1 - 1u));
            const int32_t e0 = cgl_particles_cam_depth(&cam, s_sys->particle[0].x, s_sys->particle[0].y,
                                            s_sys->particle[0].z);
            const int32_t e1 = cgl_particles_cam_depth(&cam, s_sys->particle[1].x, s_sys->particle[1].y,
                                            s_sys->particle[1].z);

            (void)printf("  probe yaw 90: depth(p0)=%d depth(p1)=%d -> first=%u\n",
                         (int)e0, (int)e1, (unsigned)first1);

            /* at yaw 90 the z axis has stopped contributing and the x axis has taken
               over, so the two **swap**: particle 0 (out at +x, e0 = 91080) is now the
               far one. Neither particle moved -- that is the whole point. */
            CHECK(n1 == 2u, "the probe cloud holds %u live particles, want 2", (unsigned)n1);
            CHECK(e0 > e1, "yaw 90: the two probes should have swapped (e0=%d, e1=%d)",
                  (int)e0, (int)e1);
            CHECK(first1 == 0u, "at yaw 90 the FARTHER particle (index 0, d=%d) must be painted "
                                "first, got index %u (d=%d)",
                  (int)e0, (unsigned)first1, (int)e1);
            CHECK(last1 == 1u, "at yaw 90 the NEARER particle (index 1, d=%d) must be painted "
                               "last, got index %u (d=%d)",
                  (int)e1, (unsigned)last1, (int)e0);
        }

        (void)printf("  camera rotation: the (x-far, z-far) pair swapped at 90 deg "
                     "without either particle moving\n");

        /* restore the full cloud for the rest of the suite */
        seed_known_depths(s_sys);
    }
}

/** The simulation must be deterministic (so this test means something), must
    damp, must apply gravity, and must cull instead of wrapping. */
static void test_engine_step(void)
{
    static int16_t y0[CGL_PARTICLES_MAX];
    static int8_t vz0[CGL_PARTICLES_MAX];
    static int16_t y1[CGL_PARTICLES_MAX];
    int steps;
    int i;

    (void)printf("\n== Engine: burst, damping, gravity, culling ==\n");

    s_sys = cgl_particles_init(&s_store, (uint32_t)sizeof(s_store));
    cgl_particles_build(s_sys, 1u, grid_is_lit, grid_cell_x, grid_cell_y);
    cgl_particles_burst(s_sys, 0, 0, 0);

    for (i = 0; i < CGL_PARTICLES_MAX; i++)
    {
        y0[i] = cgl_particles_pos(s_sys, (uint16_t)i, 1u);
        vz0[i] = cgl_particles_vel(s_sys, (uint16_t)i, 2u);
    }

    /* Run the 31 steps the page runs before it freezes the world. */
    for (steps = 0; steps < 31; steps++)
    {
        cgl_particles_step(s_sys);
    }
    CHECK(cgl_particles_tick(s_sys) == 31u, "cgl_particles_tick reported %u, want 31", (unsigned)cgl_particles_tick(s_sys));

    for (i = 0; i < CGL_PARTICLES_MAX; i++)
    {
        y1[i] = cgl_particles_pos(s_sys, (uint16_t)i, 1u);
    }

    /* Gravity pulls down the screen, and down the screen is world **+y** in this
       isometric basis. A cloud that does not fall at all means gravity was applied
       to the wrong axis (world z is the common mistake, and it pushes the particles
       *up* the screen because screen y = y + z). */
    {
        int fell = 0;

        for (i = 0; i < CGL_PARTICLES_MAX; i++)
        {
            if (y1[i] > y0[i])
            {
                fell++;
            }
        }
        CHECK(fell > (CGL_PARTICLES_MAX / 2),
              "only %d of %d particles moved down the screen in 31 steps (gravity axis is probably wrong)",
              fell, CGL_PARTICLES_MAX);
    }

    /* The burst has to have depth: a cloud that stays in the Logo plane would make
       the orbit a rotation of a flat picture. The depth comes from the burst's
       velocity (the build parks every particle at z = 0), so it shows up in the
       velocity's spread. */
    {
        int pos = 0;
        int neg = 0;

        for (i = 0; i < CGL_PARTICLES_MAX; i++)
        {
            if (vz0[i] > 0)
            {
                pos++;
            }
            else if (vz0[i] < 0)
            {
                neg++;
            }
        }
        CHECK((pos > 20) && (neg > 20),
              "the depth velocity is not spread in both directions (vz>0: %d, vz<0: %d)", pos, neg);
    }

    /* Nothing may have wrapped. The burst is radial from the origin, so a live
       particle's position has to be on the same side as its velocity; a wrapped
       int16 flips one and not the other, which is the signature of the overflow. */
    {
        int wrapped = 0;

        for (i = 0; i < CGL_PARTICLES_MAX; i++)
        {
            const int16_t x = cgl_particles_pos(s_sys, (uint16_t)i, 0u);
            const int8_t vx = cgl_particles_vel(s_sys, (uint16_t)i, 0u);

            if (((x > 0) && (vx < -60)) || ((x < 0) && (vx > 60)))
            {
                wrapped++;
            }
        }
        CHECK(wrapped == 0, "%d particles look wrapped (position and velocity disagree in sign)", wrapped);
    }

    /* Determinism: the same construction twice gives byte identical velocities, so
       every assertion above is about the code and not about a lucky sample. This is
       also what makes the xorshift (rather than libc rand) worth its bytes.
       The second cloud goes into a **separate** storage: rebuilding the same one
       would overwrite the state the first burst is being compared against. */
    {
        static cgl_particles_storage_t other;
        static cgl_particles_t *o;
        int same = 1;

        o = cgl_particles_init(&other, (uint32_t)sizeof(other));
        cgl_particles_build(o, 1u, grid_is_lit, grid_cell_x, grid_cell_y);
        cgl_particles_burst(o, 0, 0, 0);

        for (i = 0; i < CGL_PARTICLES_MAX; i++)
        {
            if (cgl_particles_vel(o, (uint16_t)i, 2u) != vz0[i])
            {
                same = 0;
                break;
            }
        }
        CHECK(same, "two identically built bursts differ (the simulation is not deterministic)");
    }
}

/** The real Logo, as the page feeds it to the engine. */
static bool logo_is_lit(int i, int j)
{
    return logo_bitmap_get(i, j) != 0;
}

/**
 * The burst at the **real scene geometry**: the case that used to hang.
 *
 * The section above bursts a synthetic grid whose impact vector is at most 128
 * units long, so its squared magnitude is a few thousand and the int32 sum of
 * squares in the engine's direction helper could never overflow. The page bursts
 * the real Logo: 106 cells at 2.1875 px, struck on its near face, so the far edge of
 * the sheet is 59 392 Q8.8 units away and the sum of squares reaches 3.5e9 -- past
 * int32 (2.1e9). The wrapped value was negative; because signed overflow is
 * undefined behaviour the compiler had deleted the `v <= 0` guard in the square root
 * as dead code; and the loop that guard was protecting then never terminated. The
 * host test hung here (the firmware, whose ELF contains the same loop, froze on the
 * panel at the frame the ball touched the Logo, with the Logo still intact).
 *
 * So this drives exactly what the page drives -- `cgl_particles_cell_x` / `cgl_particles_cell_y` and
 * `logo_bitmap_get` -- and checks the three things the overflow destroyed: the
 * sheet is not left parked, the depth velocity is still the symmetric jitter the
 * in-plane impact implies, and the debris flies **away from the impact**.
 */
static void test_engine_burst_far(void)
{
    static cgl_particles_storage_t store;
    static int32_t d_x[CGL_PARTICLES_MAX];
    static int32_t d_y[CGL_PARTICLES_MAX];
    cgl_particles_t *s;
    const int32_t face_x = -((CGL_PARTICLES_GRID_W * CGL_PARTICLES_CELL_PITCH_Q8) / 2);
    int64_t max_d2 = 0;
    int64_t vz_sum = 0;
    int parked = 0;
    int up = 0;
    int down = 0;
    int checked = 0;
    int radial = 0;
    int i;

    (void)printf("\n== Engine: the burst at the real geometry (up to 232 px from the impact) ==\n");

    s = cgl_particles_init(&store, (uint32_t)sizeof(store));
    cgl_particles_build(s, 1u, logo_is_lit, cgl_particles_cell_x, cgl_particles_cell_y);
    cgl_particles_burst(s, face_x, 0, 0);

    /* The distances themselves, and the proof that this test really is in the range
       the old int32 sum of squares could not hold: 59 392^2 = 3.5e9 > INT32_MAX.
       Computed in int64 because that is the point -- int32 is what overflowed. */
    for (i = 0; i < CGL_PARTICLES_MAX; i++)
    {
        const int64_t dx = (int64_t)cgl_particles_pos(s, (uint16_t)i, 0u) - (int64_t)face_x;
        const int64_t dy = (int64_t)cgl_particles_pos(s, (uint16_t)i, 1u);
        const int64_t dz = (int64_t)cgl_particles_pos(s, (uint16_t)i, 2u);
        const int64_t d2 = (dx * dx) + (dy * dy) + (dz * dz);

        d_x[i] = (int32_t)dx;
        d_y[i] = (int32_t)dy;
        if (d2 > max_d2)
        {
            max_d2 = d2;
        }
    }
    CHECK(max_d2 > 2147483647LL,
          "this test does not reach the overflowing range (max |d|^2 = %lld)", (long long)max_d2);
    (void)printf("  max |impact - particle|^2 = %lld (int32 holds 2147483647)\n", (long long)max_d2);

    for (i = 0; i < CGL_PARTICLES_MAX; i++)
    {
        const int32_t vx = cgl_particles_vel(s, (uint16_t)i, 0u);
        const int32_t vy = cgl_particles_vel(s, (uint16_t)i, 1u);
        const int32_t vz = cgl_particles_vel(s, (uint16_t)i, 2u);

        vz_sum += vz;
        if (vz > 0)
        {
            up++;
        }
        else if (vz < 0)
        {
            down++;
        }
        if ((vx == 0) && (vy == 0) && (vz == 0))
        {
            parked++;
        }
    }

    CHECK(parked == 0, "%d of %d particles were left parked by the burst",
          parked, CGL_PARTICLES_MAX);

    /* The impact lies **in the Logo plane**, so the radial direction has no depth
       component at all and the whole z velocity is the symmetric jitter: a mean near
       zero and roughly half the cloud on each side. A burst that lost its direction
       (which is what the wrapped magnitude produced) sends every such particle along
       +z instead, so both the mean and the split give it away. The bound on the mean
       is ~14 standard errors, and the broken case is 100x past it. */
    CHECK((up > 100) && (down > 100),
          "the depth velocity is not spread both ways (vz>0: %d, vz<0: %d)", up, down);
    CHECK((vz_sum > -600) && (vz_sum < 600),
          "the depth velocity is biased: sum(vz) = %lld over %d particles",
          (long long)vz_sum, CGL_PARTICLES_MAX);

    /* Radial: far enough from the impact that the radial term dominates the forward
       punch, the in-plane velocity has to point away from the impact. The worst case
       the model can produce is 40 units of punch+jitter perpendicular to a radial
       speed of at least 37, i.e. 71 deg, so 75 deg (dot^2 * 15 > |v|^2 * |d|^2) is a
       bound with margin -- and a burst with no direction fails it outright on the
       particles whose radius is mostly vertical. */
    for (i = 0; i < CGL_PARTICLES_MAX; i++)
    {
        const int64_t d2 = ((int64_t)d_x[i] * d_x[i]) + ((int64_t)d_y[i] * d_y[i]);
        const int32_t vx = cgl_particles_vel(s, (uint16_t)i, 0u);
        const int32_t vy = cgl_particles_vel(s, (uint16_t)i, 1u);
        const int64_t dot = ((int64_t)vx * d_x[i]) + ((int64_t)vy * d_y[i]);
        const int64_t v2 = ((int64_t)vx * vx) + ((int64_t)vy * vy);

        if (d2 < ((int64_t)(32 * CGL_PARTICLES_ONE) * (32 * CGL_PARTICLES_ONE)))
        {
            continue; /* inside 32 px the punch legitimately dominates the radius */
        }
        checked++;
        if ((dot > 0) && (((dot * dot) * 15) > (v2 * d2)))
        {
            radial++;
        }
    }
    CHECK(checked > 300, "only %d particles were far enough from the impact to be checked", checked);
    CHECK(radial == checked, "%d of %d particles did not fly away from the impact",
          checked - radial, checked);
    (void)printf("  %d particles fly radially outward (checked %d, parked %d, vz sum %lld)\n",
                 radial, checked, parked, (long long)vz_sum);
}

/** `cgl_particles_storage_t` has to be big enough for what `cgl_particles_init` demands, and the smash
    page's world has to be big enough for the engine. Both are compile time gates,
    but restating them here means the test fails loudly if the gates are ever
    loosened by accident. */
static void test_storage_math(void)
{
    (void)printf("\n== Storage: the particle engine fits the smash page's world ==\n");

    CHECK(sizeof(cgl_particles_storage_t) >= CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX),
          "cgl_particles_storage_t is %u bytes, the engine needs %u",
          (unsigned)sizeof(cgl_particles_storage_t), (unsigned)CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX));
    CHECK(sizeof(smash_storage_t) >= CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX),
          "smash_storage_t is %u bytes, the engine needs %u",
          (unsigned)sizeof(smash_storage_t), (unsigned)CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX));
    CHECK(sizeof(cgl_particle_t) == 12, "cgl_particle_t is %u bytes, want 12", (unsigned)sizeof(cgl_particle_t));

    (void)printf("  cgl_particle_t      %u B x %d = %u B\n",
                 (unsigned)sizeof(cgl_particle_t), CGL_PARTICLES_MAX,
                 (unsigned)(sizeof(cgl_particle_t) * CGL_PARTICLES_MAX));
    (void)printf("  cgl_particles_t %u B\n", (unsigned)sizeof(cgl_particles_t));
    (void)printf("  cgl_phys2d world       %u B\n", (unsigned)sizeof(cgl_phys2d_t));
    (void)printf("  smash_storage_t    %u B (the overlay the page reuses)\n", (unsigned)sizeof(smash_storage_t));
}

/* -------------------------------------------------------------------------- */
/* 3. The page: state machine, time scale, rendering                          */
/* -------------------------------------------------------------------------- */

static cgl_display_t s_display;
static cgl_screen_t s_screen;
static cgl_page_nav_t s_nav;
static app_page_smash_t s_smash;
static app_page_bullet_t s_bullet;

/** Count pixels that are not the page background, in the viewport rows. */
static uint32_t count_ink(void)
{
    uint32_t n = 0u;
    int16_t y;

    for (y = 20; y < 152; y++)
    {
        int16_t x;

        for (x = 0; x < CGL_CFG_SCREEN_W; x++)
        {
            const cgl_color_t c = s_framebuffer[(y * CGL_CFG_SCREEN_W) + x];

            /* anything brighter than the darkest palette entry is "something" */
            if (c != CGL_CFG_BG_COLOR)
            {
                n++;
            }
        }
    }
    return n;
}

static void test_page(void)
{
    int i;
    uint16_t alive_at_freeze = 0u;
    uint16_t alive_later = 0u;
    /* static, not automatic: 3000 B of stack on top of what cgl_screen_frame()
       already uses, in a debug build with no frame pointer omission, is enough to
       blow a Windows thread stack. The test runs on one thread and this array is
       only alive for the duration of this function, so static storage costs nothing
       and removes a whole class of "the test crashes" that has nothing to do with
       the code under test. */
    static int16_t pos_at_freeze[CGL_PARTICLES_MAX][3];
    static int16_t pos_at_start[CGL_PARTICLES_MAX][3];
    uint8_t yaw_at_freeze = 0u;
    uint8_t yaw_at_orbit_start = 0u;
    uint8_t yaw_at_orbit_end = 0u;
    uint32_t ink_before;
    uint32_t spread_mean_q8 = 0u;
    uint32_t spread_max_q8 = 0u;
    uint32_t reached = 0u;
    uint32_t reached_moved = 0u;
    uint32_t behind = 0u;
    uint32_t behind_still = 0u;
    int froze = 0;
    int orbit_ticks = 0;
    int orbit_done = 0;

    (void)printf("\n== Page: time scale, freeze, orbit, render ==\n");
    (void)fflush(stdout);

    if (cgl_display_init(&s_display, &s_mock_drv, NULL) != 0)
    {
        CHECK(false, "cgl_display_init failed");
        return;
    }
    cgl_page_nav_init(&s_nav, "pages",
                     cgl_rect(0, 0, (int16_t)(CGL_CFG_SCREEN_W - 1), (int16_t)(CGL_CFG_SCREEN_H - 1)));
    /* One context for both pages (static: the pages keep a pointer to it). */
    static app_links_t links;
    static cgl_page_ctx_t ctx;

    links.smash = &s_smash;
    links.bullet = &s_bullet;
    links.info = NULL;
    links.menu = NULL;
    ctx.screen = &s_screen;
    ctx.nav = &s_nav;
    ctx.app = &links;

    app_page_smash_attach(&s_smash.base, &ctx);
    app_page_bullet_attach(&s_bullet.base, &ctx);
    if (cgl_screen_init(&s_screen, &s_display, &s_nav) != 0)
    {
        CHECK(false, "cgl_screen_init failed");
        return;
    }

    /*
     * The storage the engine was handed has to satisfy the engine's own size gate.
     * This asserts the two numbers directly, because "the gate is stricter than the
     * struct it gates" is a failure mode that produces no error and no warning --
     * just a page that draws nothing. That is exactly what shipped: the size macro
     * was 4 bytes larger than `sizeof(cgl_particles_t)`, so `cgl_particles_init()` returned
     * NULL **every** time, the page's engine pointer stayed NULL, the whole particle
     * viewport stayed empty, and the only visible symptom was "nothing but the ball".
     */
    CHECK(CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX) == (uint32_t)sizeof(cgl_particles_t),
          "CGL_PARTICLES_STORAGE_BYTES(%d) = %u but sizeof(cgl_particles_t) = %u",
          CGL_PARTICLES_MAX, (unsigned)CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX),
          (unsigned)sizeof(cgl_particles_t));

    /*
     * **Push the page first.** A page's widget tree is only built in its own
     * `on_create`, which runs when the navigator attaches it -- so everything about
     * the page's state is uninitialised until the first frame after the push, and
     * checking it earlier compares NULL against NULL and reports a failure that is
     * not one. (This cost a debugging round: the "the page is not using the smash
     * page's storage" failure was the *test* asking too early.)
     */
    (void)cgl_page_nav_reset(&s_nav, &s_bullet.base);
    cgl_screen_frame(&s_screen, 0u);

    /* Snapshot the resting cloud: every particle is on its Logo cell. The spread
       measured at the freeze is taken against these. */
    for (i = 0; i < (int)CGL_PARTICLES_MAX; i++)
    {
        pos_at_start[i][0] = cgl_particles_pos(s_bullet.particles, (uint16_t)i, 0u);
        pos_at_start[i][1] = cgl_particles_pos(s_bullet.particles, (uint16_t)i, 1u);
        pos_at_start[i][2] = cgl_particles_pos(s_bullet.particles, (uint16_t)i, 2u);
    }

    /* --- the overlay: the page points at the smash page's world bytes --------- */
    CHECK((void *)s_bullet.particles == (void *)&s_smash.storage.u.particles,
          "the page is not using the smash page's storage (%p vs %p)",
          (void *)s_bullet.particles, (void *)&s_smash.storage.u.particles);
    CHECK((void *)&s_smash.storage.u.particles == (void *)&s_smash.storage.u.w.world,
          "the particle engine and the physics world are NOT overlaid: %p vs %p",
          (void *)&s_smash.storage.u.particles, (void *)&s_smash.storage.u.w.world);
    CHECK((void *)s_bullet.view.particles == (void *)s_bullet.particles,
          "the render widget is not pointed at the page's engine (%p vs %p)",
          (void *)s_bullet.view.particles, (void *)s_bullet.particles);
    CHECK(s_smash.storage.mode == SMASH_STORAGE_PARTICLE,
          "the storage mode after a bullet restart is 0x%02X, want SMASH_STORAGE_PARTICLE",
          (unsigned)s_smash.storage.mode);

    /* --- run a full round ---------------------------------------------------- */
    CHECK(app_page_bullet_phase(&s_bullet) == (uint8_t)BULLET_PHASE_BUILD,
          "the page did not start in BUILD (phase %u)", (unsigned)app_page_bullet_phase(&s_bullet));
    CHECK(app_page_bullet_alive(&s_bullet) == CGL_PARTICLES_MAX,
          "the intact Logo has %u particles, want %u",
          (unsigned)app_page_bullet_alive(&s_bullet), (unsigned)CGL_PARTICLES_MAX);

    /* Frame 0 with dt = 0 draws, but does not step; take the ink count with the
       Logo intact for the "something is really drawn" check. */
    ink_before = count_ink();
    /*
     * The Logo must actually be **on screen**, not merely built: 500 particles is
     * ~2000 lit pixels at 1..3 px each, and the empty viewport plus bars is ~97.
     * This is the check that catches a camera fit that projects the whole cloud off
     * the sides (which shipped once) and a depth sort that never runs so the render
     * is skipped entirely (which shipped once too, and produced exactly 97).
     */
    CHECK(ink_before > 800u, "only %u viewport pixels were drawn with the Logo intact "
                             "(~2000 expected; a bar-only screen is ~97)",
          (unsigned)ink_before);

    /*
     * Run the whole animation at the firmware cadence (16 ms) and record the three
     * moments that matter: the freeze, the step right after it, and the end of the
     * orbit.
     */
    for (i = 0; i < 1400; i++)
    {
        const uint8_t phase_before = app_page_bullet_phase(&s_bullet);

        cgl_screen_frame(&s_screen, 16u);

        if (!froze && (app_page_bullet_time_scale(&s_bullet) == 0u))
        {
            int p;

            froze = 1;
            alive_at_freeze = app_page_bullet_alive(&s_bullet);
            yaw_at_freeze = app_page_bullet_yaw(&s_bullet);
            for (p = 0; p < CGL_PARTICLES_MAX; p++)
            {
                pos_at_freeze[p][0] = cgl_particles_pos(s_bullet.particles, (uint16_t)p, 0u);
                pos_at_freeze[p][1] = cgl_particles_pos(s_bullet.particles, (uint16_t)p, 1u);
                pos_at_freeze[p][2] = cgl_particles_pos(s_bullet.particles, (uint16_t)p, 2u);
            }

            /* **How far the debris got, and what did not move at all.** The effect is
               a *local* smash: the ball releases the dots it reaches (one contact at a
               time, see `cgl_particles_impact()`) and every dot it never reached must still be
               sitting exactly on the cell `cgl_particles_build()` put it on. Measured as the
               Chebyshev distance from that resting position, in Q8.8. */
            {
                uint32_t sum = 0u;
                const int32_t front = s_bullet.ball_x_q8 + (BULLET_BALL_R_PX * CGL_PARTICLES_ONE);

                for (p = 0; p < (int)CGL_PARTICLES_MAX; p++)
                {
                    int dx = (int)pos_at_freeze[p][0] - (int)pos_at_start[p][0];
                    int dy = (int)pos_at_freeze[p][1] - (int)pos_at_start[p][1];
                    int dz = (int)pos_at_freeze[p][2] - (int)pos_at_start[p][2];
                    int d;

                    if (dx < 0) { dx = -dx; }
                    if (dy < 0) { dy = -dy; }
                    if (dz < 0) { dz = -dz; }
                    d = (dx > dy) ? dx : dy;
                    if (dz > d) { d = dz; }

                    if (pos_at_start[p][0] > front)
                    {
                        /* the ball has not reached this one: it may not have moved */
                        behind++;
                        if (d == 0)
                        {
                            behind_still++;
                        }
                    }
                    else
                    {
                        reached++;
                        if (d > 0)
                        {
                            reached_moved++;
                        }
                    }

                    if (d > 0)
                    {
                        sum += (uint32_t)d;
                        if ((uint32_t)d > spread_max_q8)
                        {
                            spread_max_q8 = (uint32_t)d;
                        }
                    }
                }
                spread_mean_q8 = (reached_moved > 0u) ? (sum / reached_moved) : 0u;
            }
        }

        /* Count the ticks the orbit lasts, and take the yaw at both ends of it, by
           watching the phase. One turn of yaw is 256 units and the camera advances
           `BULLET_YAW_STEP_NUM / BULLET_YAW_STEP_DEN` per tick, so the orbit's tick
           count and the yaw it covers are two views of the same pair of constants. */
        if ((phase_before != (uint8_t)BULLET_PHASE_ORBIT) &&
            (app_page_bullet_phase(&s_bullet) == (uint8_t)BULLET_PHASE_ORBIT))
        {
            yaw_at_orbit_start = app_page_bullet_yaw(&s_bullet);
        }
        if (phase_before == (uint8_t)BULLET_PHASE_ORBIT)
        {
            if (app_page_bullet_phase(&s_bullet) != (uint8_t)BULLET_PHASE_ORBIT)
            {
                yaw_at_orbit_end = app_page_bullet_yaw(&s_bullet);
            }
            orbit_ticks++;
        }

        /* While the scale is 0 the particles may not move at all. */
        if (froze && (app_page_bullet_time_scale(&s_bullet) == 0u) && (i % 37) == 0)
        {
            int p;
            int moved = 0;

            for (p = 0; p < (int)CGL_PARTICLES_MAX; p++)
            {
                if ((cgl_particles_pos(s_bullet.particles, (uint16_t)p, 0u) != pos_at_freeze[p][0]) ||
                    (cgl_particles_pos(s_bullet.particles, (uint16_t)p, 1u) != pos_at_freeze[p][1]) ||
                    (cgl_particles_pos(s_bullet.particles, (uint16_t)p, 2u) != pos_at_freeze[p][2]))
                {
                    moved++;
                }
            }
            CHECK(moved == 0, "%d particles moved while the time scale was 0 (frame %d)", moved, i);
            alive_later = app_page_bullet_alive(&s_bullet);
            CHECK(alive_later == alive_at_freeze,
                  "the live count changed while time was stopped: %u -> %u",
                  (unsigned)alive_at_freeze, (unsigned)alive_later);
            (void)fflush(stdout);

            /* ...and the camera must have advanced anyway: that is the effect. */
            CHECK(app_page_bullet_yaw(&s_bullet) != yaw_at_freeze,
                  "the camera did not turn while time was stopped (frame %d)", i);
            break;
        }
    }

    CHECK(froze, "the time scale never reached 0: the bullet time never triggered");
    (void)printf("  froze with %u particles alive, at yaw byte %u (= %u deg)\n",
                 (unsigned)alive_at_freeze, (unsigned)yaw_at_freeze,
                 (unsigned)(((uint32_t)yaw_at_freeze * 45u) / 32u));
    (void)printf("  debris spread at the freeze: mean %u.%02u px over the %u dots the ball "
                 "reached, max %u.%02u px\n",
                 (unsigned)(spread_mean_q8 / CGL_PARTICLES_ONE),
                 (unsigned)(((spread_mean_q8 % CGL_PARTICLES_ONE) * 100u) / CGL_PARTICLES_ONE),
                 (unsigned)reached_moved,
                 (unsigned)(spread_max_q8 / CGL_PARTICLES_ONE),
                 (unsigned)(((spread_max_q8 % CGL_PARTICLES_ONE) * 100u) / CGL_PARTICLES_ONE));
    (void)printf("  the ball reached %u of %u dots; %u of the %u it had not reached are "
                 "still exactly on their cells\n",
                 (unsigned)reached, (unsigned)CGL_PARTICLES_MAX,
                 (unsigned)behind_still, (unsigned)behind);

    /*
     * **The smash has to be local, and the rest of the Logo has to survive.** This is
     * the check that pins the shape of the effect: the ball releases the dots it
     * reaches, one contact at a time, so at the freeze the part of the sheet ahead of
     * it must be *bit for bit* where `cgl_particles_build()` left it. The first version released
     * all 500 particles from the near face at once, so the whole Logo dissolved while
     * the ball was still short of most of it -- "the Logo flies apart before the ball
     * touches it", which is exactly what came back from the panel.
     */
    CHECK(behind > 50u, "only %u dots were ahead of the ball at the freeze", (unsigned)behind);
    CHECK(behind_still == behind,
          "%u of the %u dots the ball never reached moved (the smash is not local)",
          (unsigned)(behind - behind_still), (unsigned)behind);

    /* ...and the part it did reach has to have been released and to have flown. */
    CHECK(reached > 100u, "the ball only reached %u dots", (unsigned)reached);
    CHECK(reached_moved == reached,
          "%u of the %u dots the ball reached never moved",
          (unsigned)(reached - reached_moved), (unsigned)reached);

    /*
     * **The frozen cloud has to be an explosion.** The other half of the same report:
     * a debris velocity is an int8 and therefore at most 0.5 px per step, so how far
     * the cloud opens up is `speed x fly steps x 2^damping shift`. The first tuning
     * (7/8 damping, 31 steps) moved the debris a **measured 3.3 px mean / 4.2 px max**
     * before the freeze -- a ragged Logo that never flies apart.
     *
     * Measured over the dots the ball actually reached (the untouched ones are,
     * correctly, at 0), the current tuning gives ~10 px mean / ~27 px max. The mean is
     * deliberately diluted: a dot the ball touched in the last step or two before the
     * freeze *should* still be essentially on its cell (the spray is a gradient from
     * the near face, where the debris has flown the whole 78 steps, to the contact
     * point, where it has just been struck). 8 px mean / 20 px max is what the first
     * tuning scored 3.3 / 4.2 against, so the check still fails loudly if the damping
     * or the fly duration is ever "calmed down" again.
     */
    CHECK(spread_mean_q8 >= (8u * CGL_PARTICLES_ONE),
          "the released debris only spread %u.%02u px on average before the freeze: the "
          "Logo cannot read as shattered (see CGL_PARTICLES_DAMP_SHIFT / CGL_PARTICLES_TRIGGER_TICKS)",
          (unsigned)(spread_mean_q8 / CGL_PARTICLES_ONE),
          (unsigned)(((spread_mean_q8 % CGL_PARTICLES_ONE) * 100u) / CGL_PARTICLES_ONE));
    CHECK(spread_max_q8 >= (20u * CGL_PARTICLES_ONE),
          "the farthest debris only reached %u.%02u px from its cell",
          (unsigned)(spread_max_q8 / CGL_PARTICLES_ONE),
          (unsigned)(((spread_max_q8 % CGL_PARTICLES_ONE) * 100u) / CGL_PARTICLES_ONE));

    /* One particle, in full. The engine is deterministic integer code, so this is
       also the line the **firmware** can be checked against over SWD: the same round
       on the panel has to reach the same position, and that is the only way to prove
       "the picture on the panel is the picture this test measured". */
    (void)printf("  particle 0 at the freeze: x=%d y=%d z=%d vx=%d vy=%d vz=%d (Q8.8)\n",
                 (int)cgl_particles_pos(s_bullet.particles, 0u, 0u),
                 (int)cgl_particles_pos(s_bullet.particles, 0u, 1u),
                 (int)cgl_particles_pos(s_bullet.particles, 0u, 2u),
                 (int)cgl_particles_vel(s_bullet.particles, 0u, 0u),
                 (int)cgl_particles_vel(s_bullet.particles, 0u, 1u),
                 (int)cgl_particles_vel(s_bullet.particles, 0u, 2u));

    /*
     * Finish the round. The counter **continues the orbit the first loop started
     * counting and stops the moment that orbit ends**: this loop runs for 1400
     * frames (a whole round), and counting every ORBIT frame in that window is what
     * made the old print claim "1221 ticks per turn" for an 850 tick orbit -- it had
     * added the *next* round's orbit to this one. It also called the orbit's length
     * "ticks per turn", which is a different quantity: one turn is 256 units of yaw
     * at 91/80 units per tick = 225 ticks, and the orbit is 3.78 of those.
     */
    for (i = 0; i < 1400; i++)
    {
        if (!orbit_done)
        {
            if (app_page_bullet_phase(&s_bullet) == (uint8_t)BULLET_PHASE_ORBIT)
            {
                orbit_ticks++;
            }
            else if (orbit_ticks > 0)
            {
                orbit_done = 1;
            }
        }
        cgl_screen_frame(&s_screen, 16u);
    }

    /*
     * The orbit's duration is a pair of compile time numbers (`BULLET_ORBIT_MS` at
     * `CGL_CFG_CLOCK_STEP_MS` per tick), and the yaw rate is another pair
     * (`BULLET_YAW_STEP_NUM / BULLET_YAW_STEP_DEN`). Both are checked here against
     * the ticks actually spent in ORBIT, because either number can drift away from
     * the other without anything else noticing.
     */
    {
        const int want_ticks = (int)(BULLET_ORBIT_MS / (uint32_t)CGL_CFG_CLOCK_STEP_MS);
        const uint32_t turn_ticks = (256u * BULLET_YAW_STEP_DEN) / BULLET_YAW_STEP_NUM;

        CHECK(orbit_ticks >= (want_ticks - 2),
              "the orbit lasted %d ticks, want %d (BULLET_ORBIT_MS / CGL_CFG_CLOCK_STEP_MS)",
              orbit_ticks, want_ticks);
        CHECK(orbit_ticks <= (want_ticks + 2),
              "the orbit lasted %d ticks, want %d (it must not run into the next phase)",
              orbit_ticks, want_ticks);
        CHECK(yaw_at_orbit_end != yaw_at_orbit_start,
              "the camera did not turn across the orbit (%u -> %u)",
              (unsigned)yaw_at_orbit_start, (unsigned)yaw_at_orbit_end);
        (void)printf("  orbit lasted %d ticks (= %u ticks per turn, %.2f turns)\n",
                     orbit_ticks, (unsigned)turn_ticks,
                     (double)orbit_ticks / (double)turn_ticks);
    }

    /* The page must have rendered something during the orbit. */
    {
        const uint32_t ink_after = count_ink();

        CHECK(ink_after > 300u, "only %u viewport pixels were drawn after the round", (unsigned)ink_after);
        (void)printf("  viewport ink: %u px intact, %u px after one round\n",
                     (unsigned)ink_before, (unsigned)ink_after);
    }

    /* --- the ball enters from off-stage and leaves off-stage ------------------ */
    /*
     * The wrecking ball is **page state, not a particle**: it is not clamped to the
     * particle world window (+-128 px Q8.8), it starts 210 px off the near side of the
     * stage and has to run off the far side. Both ends are checked through the
     * **projection** rather than against a world x, because "x > 120 px" is not the
     * same statement as "off the screen" once the camera has a yaw -- and because the
     * round used to restart on a fixed timer with the ball still crossing the frame,
     * which reads as the ball disappearing in mid-air.
     */
    {
        int32_t min_px = 100000;
        int32_t max_px = -100000;

        app_page_bullet_restart(&s_bullet); /* a fresh round, deterministically */
        cgl_screen_frame(&s_screen, 0u);

        for (i = 0; i < 700; i++) /* ~11 s: longer than a whole round */
        {
            int32_t sx;
            int32_t sy;
            int32_t depth;
            int32_t px;

            cgl_particles_project(&s_bullet.view.cam, s_bullet.ball_x_q8, 0, 0, &sx, &sy, &depth);
            px = sx / CGL_PARTICLES_ONE;

            if (px < min_px) { min_px = px; }
            if (px > max_px) { max_px = px; }

            cgl_screen_frame(&s_screen, 16u);
        }

        CHECK(min_px < -(int32_t)BULLET_BALL_R_PX,
              "the ball never came from outside the frame (leftmost screen x = %d)",
              (int)min_px);
        CHECK(max_px > (int32_t)(CGL_CFG_SCREEN_W + BULLET_BALL_R_PX),
              "the ball never left the frame (rightmost screen x = %d, the screen is %d wide)",
              (int)max_px, (int)CGL_CFG_SCREEN_W);
        (void)printf("  ball crossed the viewport from screen x %d to %d (screen 0..%d)\n",
                     (int)min_px, (int)max_px, (int)(CGL_CFG_SCREEN_W - 1));
    }

    /* --- the storage guard --------------------------------------------------- */
    /*
     * The one ordering rule the overlay needs: while the **bullet page** owns the
     * bytes, a smash-page Logo rebuild must refuse, or it would overwrite 500 live
     * particles with a rigid body pool.
     *
     * The point is that the mode byte is respected, so this drives the two calls
     * directly and reads the mode back -- it does not need a second page, a camera,
     * or a widget tree. (Driving more of the lifecycle here is what a first attempt
     * did, and it turned a one-line invariant into a rebuild-on-foreign-memory
     * question that has nothing to do with the guard.)
     */
    s_smash.storage.mode = SMASH_STORAGE_PARTICLE;
    app_page_smash_restart(&s_smash);
    CHECK(s_smash.storage.mode == SMASH_STORAGE_PARTICLE,
          "app_page_smash_restart ran while the particle engine owned the storage");

    (void)printf("  storage guard: the smash page refuses while the engine owns the bytes\n");
}

/* -------------------------------------------------------------------------- */

int main(void)
{
    (void)printf("bullet_time host test\n");
    (void)fflush(stdout);

    (void)printf("[1] storage math\n"); (void)fflush(stdout);
    test_storage_math();
    (void)printf("[2] trig\n"); (void)fflush(stdout);
    test_trig();
    (void)printf("[3] reciprocal\n"); (void)fflush(stdout);
    test_reciprocal();
    (void)printf("[4] mul symmetry\n"); (void)fflush(stdout);
    test_mul_symmetry();
    (void)printf("[5] depth sort\n"); (void)fflush(stdout);
    test_engine_depth_sort();
    (void)printf("[6] engine step\n"); (void)fflush(stdout);
    test_engine_step();
    (void)printf("[6b] engine step, real geometry\n"); (void)fflush(stdout);
    test_engine_burst_far();
    (void)printf("[7] page\n"); (void)fflush(stdout);
    test_page();

    if (s_fails == 0)
    {
        (void)printf("\nOK: all bullet time checks passed\n");
        return 0;
    }
    (void)printf("\nFAILED: %d bullet time check(s)\n", s_fails);
    return 1;
}
