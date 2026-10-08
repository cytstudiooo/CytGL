// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_iso2d_host_test.c
  * @brief   PC-side test for the isometric projection + depth sorting (not part of the firmware).
  *
  *    gcc -O2 -Wall -Wextra -Wpedantic -Iinclude -o build/cgl_iso2d_test.exe
  *        tests/cgl_iso2d_host_test.c src/ext/cgl_phys2d.c src/ext/cgl_iso2d.c
  *
  *  This is the criterion for "is the pseudo-3D actually correct". Whether the
  *  rendering looks good can only be judged by eye, but the following can be
  *  proven by machine:
  *
  *    1. projection formula: the isometric diamond, the x/y symmetry, the lift direction of z
  *    2. depth key: must be decided by (x+y), not by y (in an elongated dot matrix
  *       the two give completely opposite draw orders -- this is exactly the
  *       classic "sort by y" mistake)
  *    3. sorting: must be a true permutation of the active bodies, with the depth
  *       monotonically non-decreasing along the sequence (the painter's algorithm
  *       wants "far to near", and monotonicity is the necessary and sufficient
  *       condition for correct occlusion)
  *    4. sort stamp: re-sorting within one physics step must be a no-op; a changed
  *       world must re-sort
  *    5. camera fitting: the four world corners must land inside the viewport
  *    6. 2.5D vertical axis: a fast dot lifts off the plane, lands and takes off
  *       again; the kinematic ball never lifts off; a stationary dot must be
  *       **perfectly still** (otherwise the whole Logo shakes)
  *    7. decoupling: the sorter must work on a plain array of positions with no
  *       `cgl_phys2d` world anywhere -- see test_plain_positions(). That is the whole
  *       point of `cgl_iso2d_src_t`, and by construction it is the only test here that
  *       would still compile if the physics module were deleted.
  ******************************************************************************
  */
#include <stdio.h>
#include <stdbool.h>

#include <ext/cgl_phys2d.h>
#include <ext/cgl_iso2d.h>
#include <ext/cgl_iso2d_phys2d.h> /* the bridge: only the physics-flavoured tests need it */

static int s_fails = 0;

static void check(bool ok, const char *what)
{
    printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok)
    {
        s_fails++;
    }
}

/* --------------------------------------------------------------------------- */
/* 1) projection formula                                                       */
/* --------------------------------------------------------------------------- */

static void test_projection(void)
{
    cgl_iso2d_cam_t cam;
    int16_t ax;
    int16_t ay;
    int16_t bx;
    int16_t by;
    int16_t cx;
    int16_t cy;

    printf("\n== 1) Isometric projection formula ==\n");
    /* pure 30 deg matrix, no fitting: screen = (x-y)*cos30, (x+y)*sin30 - z*/
    cgl_iso2d_cam_set(&cam, CGL_ISO2D_COS30_Q8, (int16_t)(-CGL_ISO2D_COS30_Q8),
                  CGL_ISO2D_SIN30_Q8, CGL_ISO2D_SIN30_Q8, CGL_ISO2D_LIFT_Q8, 0, 0);

    cgl_iso2d_project(&cam, 0, 0, 0, &ax, &ay);
    check((ax == 0) && (ay == 0), "the origin projects to the origin");

    /* (x, 0) and (0, x) must be symmetric about the screen's vertical axis: same sy, opposite sx*/
    cgl_iso2d_project(&cam, 160, 0, 0, &ax, &ay);
    cgl_iso2d_project(&cam, 0, 160, 0, &bx, &by);
    printf("     10 px world x -> screen (%d,%d); 10 px world y -> screen (%d,%d)\n",
           ax, ay, bx, by);
    check(ay == by, "(x,0) and (0,y) have the same screen y (the symmetry of 30 deg isometric)");
    check((ax > 0) && (bx < 0), "(x,0) goes right and (0,y) goes left (a diamond, not a rectangle)");
    /*
     * Note that this is +/-1 rather than strict equality: the >>12 in the
     * projection is **floor** (arithmetic shift right), so +8.67 -> 8 while
     * -8.67 -> -9, and the left and right halves differ by 1 px.
     * Switching to "round toward zero" would need a sign test, and cgl_iso2d_project
     * is called for every body and every band on the hot path; a 1 px sub-pixel
     * deviation is invisible on a diamond and not worth a branch.
     */
    check(((ax + bx) == 0) || ((ax + bx) == -1),
          "(x,0) and (0,x) have their screen x symmetric about the origin (a 1 px right-shift rounding deviation is allowed)");

    /* raising z = smaller screen y*/
    cgl_iso2d_project(&cam, 100, 100, 0, &ax, &ay);
    cgl_iso2d_project(&cam, 100, 100, (int16_t)(5 * CGL_PHYS2D_ONE), &bx, &by);
    cgl_iso2d_project(&cam, 100, 100, (int16_t)(10 * CGL_PHYS2D_ONE), &cx, &cy);
    printf("     z = 0/5/10 px -> screen y = %d/%d/%d\n", ay, by, cy);
    check((by == (ay - 5)) && (cy == (ay - 10)), "1 px of height = 1 px of screen lift, and x is unchanged");
    check((bx == ax) && (cx == ax), "the lift does not change screen x (the z axis is vertical)");
}

/* --------------------------------------------------------------------------- */
/* 2) the depth key must come from x+y, not from y                             */
/* --------------------------------------------------------------------------- */

static void test_depth_key(void)
{
    cgl_iso2d_cam_t cam;
    cgl_phys2d_t w;
    cgl_iso2d_sorter_t s;
    int near_left;
    int far_right;

    printf("\n== 2) Depth key = x+y (sorting by y gets it wrong) ==\n");
    cgl_iso2d_cam_set(&cam, CGL_ISO2D_COS30_Q8, (int16_t)(-CGL_ISO2D_COS30_Q8),
                  CGL_ISO2D_SIN30_Q8, CGL_ISO2D_SIN30_Q8, CGL_ISO2D_LIFT_Q8, 0, 0);

    cgl_phys2d_reset(&w);
    /* A: x is large, y is small. B: x is small, y is medium.
       Sorted by y -> A first ("far"); sorted by x+y -> A last ("near"). The conclusions are opposite.*/
    near_left = cgl_phys2d_add(&w, (int16_t)(300 * CGL_PHYS2D_ONE), 0, 0, 0, 16, 0u);
    far_right = cgl_phys2d_add(&w, 0, (int16_t)(100 * CGL_PHYS2D_ONE), 0, 0, 16, 0u);

    {
        const int32_t da = cgl_iso2d_ground_y(&cam, w.body[near_left].x, w.body[near_left].y);
        const int32_t db = cgl_iso2d_ground_y(&cam, w.body[far_right].x, w.body[far_right].y);

        printf("     A(300,0) depth=%d   B(0,100) depth=%d   (sorted by y it would be %d vs %d)\n",
               (int)da, (int)db, 0, 100);
        check(da > db, "the depth key considers A(300,0) nearer than B(0,100) (the opposite of the y conclusion)");
    }

    cgl_iso2d_sorter_reset(&s);
    cgl_iso2d_sort_bodies(&s, &cam, &w);
    check(s.count == 2u, "both active bodies made it into the sort");
    printf("     draw order (drawn first = below / farther): #%d then #%d\n", s.order[0], s.order[1]);
    check(s.order[0] == far_right, "far to near: B(0,100) is drawn first");
    check(s.order[1] == near_left, "far to near: A(300,0) is drawn later (covering B)");
}

/* --------------------------------------------------------------------------- */
/* 3) the sort is a permutation + depth is monotonic                           */
/* --------------------------------------------------------------------------- */

#define SORT_N 200

static void test_sort_order(void)
{
    cgl_iso2d_cam_t cam;
    cgl_phys2d_t w;
    cgl_iso2d_sorter_t s;
    int32_t depth[SORT_N];
    uint8_t seen[SORT_N];
    int i;
    bool perm = true;
    bool mono = true;

    printf("\n== 3) Sort result: a true permutation + depth monotonically non-decreasing ==\n");
    cgl_iso2d_cam_fit(&cam, CGL_ISO2D_STYLE_ISO,
                  (int16_t)(-40 * CGL_PHYS2D_ONE), (int16_t)(-40 * CGL_PHYS2D_ONE),
                  (int16_t)(400 * CGL_PHYS2D_ONE), (int16_t)(256 * CGL_PHYS2D_ONE),
                  0, 19, 320, 134);

    cgl_phys2d_reset(&w);
    /* pseudo-random but deterministic scatter: x is deliberately made to span far more than y (which is the shape of the dot matrix)*/
    for (i = 0; i < SORT_N; i++)
    {
        const int32_t x = ((i * 137) % 318) + 1;
        const int32_t y = ((i * 61) % 40) + 66;

        (void)cgl_phys2d_add(&w, (int16_t)(x * CGL_PHYS2D_ONE), (int16_t)(y * CGL_PHYS2D_ONE),
                         0, 0, 16, 0u);
    }
    check(w.count == SORT_N, "all 200 bodies were added to the world");

    cgl_iso2d_sorter_reset(&s);
    cgl_iso2d_sort_bodies(&s, &cam, &w);
    check(s.count == (uint16_t)SORT_N, "the sort covers every active body");

    for (i = 0; i < SORT_N; i++)
    {
        seen[i] = 0u;
    }
    for (i = 0; i < SORT_N; i++)
    {
        const int32_t idx = s.order[i];
        const cgl_phys2d_body_t *b;

        if ((idx < 0) || (idx >= SORT_N) || (seen[idx] != 0u))
        {
            perm = false;
            break;
        }
        seen[idx] = 1u;
        b = &w.body[idx];
        depth[i] = cgl_iso2d_ground_y(&cam, b->x, b->y) + cam.oy;
    }
    check(perm, "the order is a true permutation of the active bodies (no duplicates, no out-of-range index)");

    for (i = 1; i < SORT_N; i++)
    {
        if (depth[i] < depth[i - 1])
        {
            mono = false;
            printf("     monotonicity broken at %d: %d -> %d\n", i, (int)depth[i - 1], (int)depth[i]);
            break;
        }
    }
    check(mono, "depth is monotonically non-decreasing along the draw order (far to near = occlusion holds)");
    printf("     depth range %d .. %d (screen height 172)\n", (int)depth[0], (int)depth[SORT_N - 1]);
    check((depth[0] >= 0) && (depth[SORT_N - 1] < CGL_ISO2D_DEPTH_BUCKETS),
          "every depth falls inside the bucket range (none overflowed into the clamping end)");
}

/* --------------------------------------------------------------------------- */
/* 4) sort stamp: a no-op within one step, a re-sort once the world changed    */
/* --------------------------------------------------------------------------- */

static void test_sort_stamp(void)
{
    cgl_iso2d_cam_t cam;
    cgl_phys2d_t w;
    cgl_iso2d_sorter_t s;
    int16_t first_before;
    int16_t first_after;
    cgl_phys2d_body_t *b;

    printf("\n== 4) Sort stamp (sort only once per physics step) ==\n");
    cgl_iso2d_cam_set(&cam, CGL_ISO2D_COS30_Q8, (int16_t)(-CGL_ISO2D_COS30_Q8),
                  CGL_ISO2D_SIN30_Q8, CGL_ISO2D_SIN30_Q8, CGL_ISO2D_LIFT_Q8, 0, 0);

    cgl_phys2d_reset(&w);
    /* Use positive depths, so we do not land on the "off-screen depth is clamped into bucket 0" path*/
    (void)cgl_phys2d_add(&w, (int16_t)(100 * CGL_PHYS2D_ONE), (int16_t)(100 * CGL_PHYS2D_ONE),
                     0, 0, 16, 0u);
    (void)cgl_phys2d_add(&w, (int16_t)(200 * CGL_PHYS2D_ONE), (int16_t)(200 * CGL_PHYS2D_ONE),
                     0, 0, 16, 0u);

    cgl_iso2d_sorter_reset(&s);
    cgl_iso2d_sort_bodies(&s, &cam, &w);
    first_before = s.order[0];
    check(first_before == 0, "initial: body 0 has the smaller depth (farther), drawn first");

    /* Do not step the world, just move body 1 to the very front: the stamp did not change, so the sort result must stay as it was*/
    b = &w.body[1];
    b->x = (int16_t)(10 * CGL_PHYS2D_ONE);
    b->y = (int16_t)(10 * CGL_PHYS2D_ONE);
    cgl_iso2d_sort_bodies(&s, &cam, &w);
    first_after = s.order[0];
    check(first_after == first_before, "re-sorting without stepping the world is a no-op (saves the 43x waste)");

    /* After stepping, a re-sort is required*/
    cgl_phys2d_step(&w);
    cgl_iso2d_sort_bodies(&s, &cam, &w);
    printf("     after re-sorting order[0] = %d (body 1 depth %d, body 0 depth %d)\n",
           s.order[0], (int)(cgl_iso2d_ground_y(&cam, w.body[1].x, w.body[1].y) + cam.oy),
           (int)(cgl_iso2d_ground_y(&cam, w.body[0].x, w.body[0].y) + cam.oy));
    check(s.order[0] == 1, "the sort was refreshed after stepping; the farthest body moved to the very front");

    cgl_iso2d_sorter_reset(&s);
    check(!s.valid, "after reset valid = false (a rebuilt world always re-sorts)");
}

/* --------------------------------------------------------------------------- */
/* 4b) decoupling: the sorter must not need a physics world at all              */
/* --------------------------------------------------------------------------- */

/**
 * The point of `cgl_iso2d_src_t`, and the reason `cgl_iso2d.h` includes nothing from the
 * physics layer: a caller with a plain array of positions gets the same sort.
 *
 * Nothing in this function touches `cgl_phys2d`: not a world, not a body, not a flag.
 * If the physics module disappeared tomorrow this test would still compile and
 * still have to pass -- which is a stronger statement than any grep, because it
 * is the *compiler and the sorter* making it rather than a comment.
 */
#define PLAIN_N 5

static cgl_iso2d_pos_t s_plain[PLAIN_N];

static bool plain_read(const void *ctx, uint16_t i, cgl_iso2d_pos_t *out)
{
    const cgl_iso2d_pos_t *a = (const cgl_iso2d_pos_t *)ctx;

    if (i >= PLAIN_N)
    {
        return false;
    }
    /* Element 3 stands for "not in the scene" (a sprite that is not spawned, a
       body that has been culled): the sorter must skip it silently. */
    if (i == 3u)
    {
        return false;
    }
    *out = a[i];
    return true;
}

static void test_plain_positions(void)
{
    cgl_iso2d_cam_t cam;
    cgl_iso2d_sorter_t s;
    cgl_iso2d_src_t src;
    uint8_t seen[PLAIN_N];
    int32_t depth[PLAIN_N];
    int n = 0;
    int i;
    bool perm = true;
    bool mono = true;

    printf("\n== 4b) Decoupling: sorting a plain array of positions (no cgl_phys2d anywhere) ==\n");
    cgl_iso2d_cam_set(&cam, CGL_ISO2D_COS30_Q8, (int16_t)(-CGL_ISO2D_COS30_Q8),
                  CGL_ISO2D_SIN30_Q8, CGL_ISO2D_SIN30_Q8, CGL_ISO2D_LIFT_Q8, 0, 0);

    /* Depths are decided by x+y here (the camera above has no offset), so this
       order is far -> near for a viewer at the bottom of the screen. */
    s_plain[0].x = (int16_t)(10 * CGL_PHYS2D_ONE);  s_plain[0].y = (int16_t)(10 * CGL_PHYS2D_ONE);  s_plain[0].z = 0;
    s_plain[1].x = (int16_t)(90 * CGL_PHYS2D_ONE);  s_plain[1].y = (int16_t)(10 * CGL_PHYS2D_ONE);  s_plain[1].z = (int16_t)(3 * CGL_PHYS2D_ONE);
    s_plain[2].x = (int16_t)(20 * CGL_PHYS2D_ONE);  s_plain[2].y = (int16_t)(10 * CGL_PHYS2D_ONE);  s_plain[2].z = 0;
    s_plain[3].x = (int16_t)(200 * CGL_PHYS2D_ONE); s_plain[3].y = (int16_t)(200 * CGL_PHYS2D_ONE); s_plain[3].z = 0; /* skipped */
    s_plain[4].x = (int16_t)(50 * CGL_PHYS2D_ONE);  s_plain[4].y = (int16_t)(10 * CGL_PHYS2D_ONE);  s_plain[4].z = 0;

    src.read = plain_read;
    src.ctx = s_plain;
    src.count = PLAIN_N;
    src.stamp = 1u;

    cgl_iso2d_sorter_reset(&s);
    cgl_iso2d_sort(&s, &cam, &src);

    check(s.count == (uint16_t)(PLAIN_N - 1),
          "the element the accessor rejects is left out of the sorted count");
    for (i = 0; i < PLAIN_N; i++)
    {
        seen[i] = 0u;
    }
    for (i = 0; i < (int)s.count; i++)
    {
        const int32_t idx = s.order[i];

        if ((idx < 0) || (idx >= PLAIN_N) || (seen[idx] != 0u))
        {
            perm = false;
            break;
        }
        seen[idx] = 1u;
        depth[i] = cgl_iso2d_ground_y(&cam, s_plain[idx].x, s_plain[idx].y) + cam.oy;
        n++;
    }
    check(perm, "the order is a true permutation of the elements the accessor accepted");
    check(seen[3] == 0u, "the rejected element never appears in the order");
    check((n == 4) && (s.order[0] == 0) && (s.order[3] == 1),
          "draw order is far to near: the (10,10) position first, the (90,10) one last");
    for (i = 1; i < n; i++)
    {
        if (depth[i] < depth[i - 1])
        {
            mono = false;
            break;
        }
    }
    check(mono, "depth is monotonically non-decreasing for the plain array as well");

    /* The stamp contract is the caller's: same stamp = no-op, new stamp = redo. */
    s_plain[0].x = (int16_t)(300 * CGL_PHYS2D_ONE);
    cgl_iso2d_sort(&s, &cam, &src);
    check(s.order[0] == 0, "with an unchanged stamp the new position is ignored (it is the caller's job to bump it)");

    src.stamp = 2u;
    cgl_iso2d_sort(&s, &cam, &src);
    check(s.order[3] == 0, "after bumping the stamp the moved position sorts to the near end");
}

/* --------------------------------------------------------------------------- */
/* 5) camera fitting: the four world corners must land inside the viewport     */
/* --------------------------------------------------------------------------- */

static void test_cam_fit(void)
{
    cgl_iso2d_cam_t cam;
    const int16_t vx = 0;
    const int16_t vy = 19;
    const int16_t vw = 320;
    const int16_t vh = 134;
    /* the dot matrix's real bounding box: 318 x 42 px, placed centred*/
    const int16_t wx0 = (int16_t)((160 - 159) * CGL_PHYS2D_ONE);
    const int16_t wy0 = (int16_t)((86 - 21) * CGL_PHYS2D_ONE);
    const int16_t ww = (int16_t)(318 * CGL_PHYS2D_ONE);
    const int16_t wh = (int16_t)(42 * CGL_PHYS2D_ONE);
    const int16_t xs[4] = { wx0, (int16_t)(wx0 + ww), (int16_t)(wx0 + ww), wx0 };
    const int16_t ys[4] = { wy0, wy0, (int16_t)(wy0 + wh), (int16_t)(wy0 + wh) };
    int i;
    cgl_iso2d_style_t style;

    printf("\n== 5) Camera fitting (the four world corners land inside the viewport) ==\n");
    for (style = CGL_ISO2D_STYLE_ISO; style <= CGL_ISO2D_STYLE_FLAT; style++)
    {
        bool inside = true;
        int32_t min_sx = 32767;
        int32_t max_sx = -32768;
        int32_t min_sy = 32767;
        int32_t max_sy = -32768;
        const char *name = (style == CGL_ISO2D_STYLE_ISO) ? "ISO"
                           : ((style == CGL_ISO2D_STYLE_ISO_UNIFORM) ? "ISO_UNI"
                                                                 : "FLAT");

        cgl_iso2d_cam_fit(&cam, style, wx0, wy0, ww, wh, vx, vy, vw, vh);
        for (i = 0; i < 4; i++)
        {
            int16_t sx;
            int16_t sy;

            cgl_iso2d_project(&cam, xs[i], ys[i], 0, &sx, &sy);
            if ((sx < vx) || (sx > (vx + vw - 1)) || (sy < vy) || (sy > (vy + vh - 1)))
            {
                inside = false;
            }
            if (sx < min_sx) { min_sx = sx; }
            if (sx > max_sx) { max_sx = sx; }
            if (sy < min_sy) { min_sy = sy; }
            if (sy > max_sy) { max_sy = sy; }
        }
        printf("     %-8s projected bbox x %d..%d  y %d..%d  (viewport %d..%d / %d..%d)\n",
               name, (int)min_sx, (int)max_sx, (int)min_sy, (int)max_sy,
               vx, vx + vw - 1, vy, vy + vh - 1);
        check(inside, "all four world corners are inside the viewport");
    }

    /* The isometric diamond must be "shorter and wider" than the flat mode: that is why it fits into a 320x134 viewport*/
    {
        int16_t ax;
        int16_t ay;
        int16_t bx;
        int16_t by;

        cgl_iso2d_cam_fit(&cam, CGL_ISO2D_STYLE_ISO, wx0, wy0, ww, wh, vx, vy, vw, vh);
        cgl_iso2d_project(&cam, (int16_t)(wx0 + ww), wy0, 0, &ax, &ay);
        cgl_iso2d_project(&cam, wx0, (int16_t)(wy0 + wh), 0, &bx, &by);
        check((ax > vx) && (bx < (vx + vw)), "isometric: stretched horizontally to fill the viewport width");
    }
}

/* -------------------------------------------------------------------------------------------------------- */
/* 5b) vertical distribution: the fitted form must spread the dot matrix further apart than textbook 30 deg */
/* -------------------------------------------------------------------------------------------------------- */

static void test_ky_split(void)
{
    cgl_iso2d_cam_t fit_cam;
    cgl_iso2d_cam_t uni_cam;
    const int16_t vx = 0;
    const int16_t vy = 19;
    const int16_t vw = 316;
    const int16_t vh = 130;
    const int16_t wx0 = (int16_t)(1 * CGL_PHYS2D_ONE);
    const int16_t wy0 = (int16_t)(65 * CGL_PHYS2D_ONE);
    const int16_t ww = (int16_t)(318 * CGL_PHYS2D_ONE);
    const int16_t wh = (int16_t)(42 * CGL_PHYS2D_ONE);
    /* the dot matrix's world step: 3 px*/
    const int16_t step = (int16_t)(3 * CGL_PHYS2D_ONE);
    int32_t d_fit;
    int32_t d_uni;

    printf("\n== 5b) Vertical distribution: fitted form vs textbook 30 deg ==\n");
    cgl_iso2d_cam_fit(&fit_cam, CGL_ISO2D_STYLE_ISO, wx0, wy0, ww, wh, vx, vy, vw, vh);
    cgl_iso2d_cam_fit(&uni_cam, CGL_ISO2D_STYLE_ISO_UNIFORM, wx0, wy0, ww, wh, vx, vy, vw, vh);

    /*
     * Of the three candidates for the distance between adjacent dots, the smallest
     * is the diagonal |u+v| = (m10 + m11) * step, which is the screen spacing of two
     * vertically adjacent rows -- it decides whether 2 px dots merge into one blob.
     * Compare the raw Q8 product (without the right shift), otherwise both values are
     * truncated to the same integer.
     */
    d_fit = (((int32_t)fit_cam.m10 + fit_cam.m11) * (step >> CGL_PHYS2D_SHIFT));
    d_uni = (((int32_t)uni_cam.m10 + uni_cam.m11) * (step >> CGL_PHYS2D_SHIFT));
    printf("     coefficients m10/m11: fitted %d/%d -> nearest dot distance %d/256 px\n",
           fit_cam.m10, fit_cam.m11, (int)d_fit);
    printf("                   equal  %d/%d -> nearest dot distance %d/256 px\n",
           uni_cam.m10, uni_cam.m11, (int)d_uni);
    check(d_fit > d_uni, "the fitted form makes the nearest dot distance larger than textbook 30 deg (the dot matrix breathes more)");
    check(d_fit >= (2 * 256 + 128), "fitted nearest dot distance >= 2.5 px (2 px dots still leave a gap)");
}

/* --------------------------------------------------------------------------- */
/* 6) 2.5D vertical axis                                                       */
/* --------------------------------------------------------------------------- */

static void test_vertical_axis(void)
{
    cgl_phys2d_t w;
    int moving;
    int resting;
    int slow;
    int kin;
    int i;
    int32_t max_z = 0;
    int32_t max_z_rest = 0;
    int32_t max_z_slow = 0;
    int32_t max_z_kin = 0;

    printf("\n== 6) 2.5D vertical axis (airborne / landing / the kinematic ball never lifts off) ==\n");
    cgl_phys2d_reset(&w);
    /* Fast dot: 6 px/step planar speed (the order of magnitude of a head-on hit by the heavy ball), far above the 1 px/step threshold*/
    moving = cgl_phys2d_add(&w, (int16_t)(-30 * CGL_PHYS2D_ONE), (int16_t)(60 * CGL_PHYS2D_ONE),
                        (int16_t)(6 * CGL_PHYS2D_ONE), 0, 16, 0u);
    /* Stationary dot: must stay on the ground forever*/
    resting = cgl_phys2d_add(&w, (int16_t)(100 * CGL_PHYS2D_ONE), (int16_t)(100 * CGL_PHYS2D_ONE),
                         0, 0, 16, 0u);
    /* Slow dot: below the threshold, so it must not lift off either*/
    slow = cgl_phys2d_add(&w, (int16_t)(-30 * CGL_PHYS2D_ONE), (int16_t)(150 * CGL_PHYS2D_ONE),
                      (int16_t)(CGL_PHYS2D_ONE / 2), 0, 16, 0u);
    /* Kinematic ball: no matter how hard it is hit, it does not lift off*/
    kin = cgl_phys2d_add(&w, (int16_t)(200 * CGL_PHYS2D_ONE), (int16_t)(100 * CGL_PHYS2D_ONE),
                     (int16_t)(6 * CGL_PHYS2D_ONE), 0, 16, CGL_PHYS2D_FLAG_KINEMATIC);

    for (i = 0; i < 60; i++)
    {
        cgl_phys2d_step(&w);
        if (w.body[moving].z > max_z) { max_z = w.body[moving].z; }
        if (w.body[resting].z > max_z_rest) { max_z_rest = w.body[resting].z; }
        if (w.body[slow].z > max_z_slow) { max_z_slow = w.body[slow].z; }
        if (w.body[kin].z > max_z_kin) { max_z_kin = w.body[kin].z; }
        if ((w.body[resting].z < 0) || (w.body[moving].z < 0) || (w.body[slow].z < 0))
        {
            printf("FAIL: z became negative\n");
            s_fails++;
            break;
        }
    }
    printf("     max height over 60 steps: fast dot %d/16 px   stationary %d   slow %d   kinematic ball %d\n",
           (int)max_z, (int)max_z_rest, (int)max_z_slow, (int)max_z_kin);
    check(max_z > (4 * CGL_PHYS2D_ONE), "a 6 px/step dot bounces to a visible height (> 4 px)");
    check(max_z_rest == 0, "a stationary dot does not move at all (otherwise the whole Logo would shake in place)");
    check(max_z_slow == 0, "a slow dot below the threshold does not lift off (it slides instead of jumping)");
    check(max_z_kin == 0, "the kinematic ball never lifts off (it is the heavy ball ploughing along the ground)");
}

/* -------------------------------------------------------------------------- */

int main(void)
{
    printf("=== cgl_iso2d: isometric projection + depth sorting test ===\n");
    printf("body pool %d, depth buckets %d, %u B per body, world about %u B\n",
           CGL_PHYS2D_MAX_BODIES, CGL_ISO2D_DEPTH_BUCKETS,
           (unsigned)sizeof(cgl_phys2d_body_t), (unsigned)sizeof(cgl_phys2d_t));

    test_projection();
    test_depth_key();
    test_sort_order();
    test_sort_stamp();
    test_plain_positions();
    test_cam_fit();
    test_ky_split();
    test_vertical_axis();

    printf("\n");
    if (s_fails == 0)
    {
        printf("all passed.\n");
        return 0;
    }
    printf("%d failed.\n", s_fails);
    return 1;
}
