// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_iso2d.c
  * @brief   Isometric camera + counting sort depth sort (implementation).
  ******************************************************************************
  */
#include <ext/cgl_iso2d.h>

/* -------------------------------------------------------------------------- */
/* helpers                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * Q8 fixed point quotient of pixels / span (rounded).
 * Fixed point rather than floating point: on the real hardware this project is
 * built -Og without libm, and one float division drags in saving/restoring the
 * floating point context; besides, this only runs once at init, so integer
 * arithmetic is less trouble and more deterministic.
 */
static int16_t cgl_iso2d__ratio_q8(int32_t span, int32_t pixels)
{
    if (span <= 0)
    {
        return 0;
    }
    return (int16_t)(((pixels << 8) + (span / 2)) / span);
}

/** Integer square root (Newton), only used to scan parameters at init. */
static int32_t cgl_iso2d__isqrt(int32_t n)
{
    int32_t x;
    int32_t y;

    if (n <= 0)
    {
        return 0;
    }
    x = n;
    y = (x + 1) / 2;
    while (y < x)
    {
        x = y;
        y = (x + (n / x)) / 2;
    }
    return x;
}

/**
 * Solve for the vertical coefficients of the two world axes: ky1 (paired with
 * world x) and ky2 (paired with world y).
 *
 * Why they cannot simply be split evenly
 * --------------------------------------
 * After projection the spacing between two neighbouring dots has only three
 * candidates (the images of the world axis unit vectors and of their
 * sum/difference):
 *
 *     |u|   = sqrt(kx^2 + ky1^2)      two neighbouring dots in the same row
 *     |v|   = sqrt(kx^2 + ky2^2)      two neighbouring rows
 *     |u+v| = ky1 + ky2               two diagonally adjacent dots  <- usually the smallest one
 *
 * There is only one vertical budget: W*ky1 + H*ky2 = viewport height. The
 * textbook 30 deg takes ky1 == ky2, which on a square world happens to make all
 * three candidates equal (equilateral triangle lattice, optimal). But as soon as
 * the world becomes a long strip (318 x 42 here, a cell ratio of 7.6:1),
 * splitting evenly dilutes ky by (W+H):
 * ky1 = ky2 = 130/360 = 0.36, so |u+v| = 2.16 px -- and in the last measurement,
 * 2 px dots at that spacing merge into continuous diagonal stripes and the whole
 * logo smears into one band.
 *
 * The right approach is to give the vertical budget **as little as possible to
 * the long axis** (every 1/256 of it eats 318 px of budget, while the short axis
 * only eats 42 px); the price is that the long axis becomes closer to horizontal
 * on screen -- which happens to make the letters easier to recognise as well.
 * The optimum occurs at |u| == |u+v| (one is rising while the other falls, so
 * the crossing point is the maximum of the minimum spacing); here it is found by
 * brute force scanning: init runs only once, and scanning ~100 steps replaces a
 * whole page of closed-form derivation.
 *
 * For this example the computed values are: kx = 0.871, ky1 = 0.328,
 * ky2 = 0.611 -> minimum spacing 2.79 px, 29% more than the 2.16 px of the even
 * split, just enough to leave a visible gap between 2 px dots.
 */
static void cgl_iso2d__split_ky(int32_t kx, int32_t w, int32_t h, int32_t vh,
                            int32_t *ky1_out, int32_t *ky2_out)
{
    /* The vertical budget is converted to Q8 so that it can be added directly to
       the Q8 coefficients: w * ky1 + h * ky2 == vh, multiply both sides by 256
       to get w * k1 + h * k2 == VH */
    const int32_t VH = vh << 8;
    /* Upper bound for k1: the long axis alone can eat the whole vertical budget
       (the short axis coefficient is then 0) */
    int32_t k1_max = (w > 0) ? (VH / w) : 0;
    int32_t best_d = -1;
    int32_t best_k1 = 0;
    int32_t k1;

    if (k1_max < 0)
    {
        k1_max = 0;
    }
    for (k1 = 0; k1 <= k1_max; k1++)
    {
        int32_t k2 = (h > 0) ? ((VH - (w * k1)) / h) : 0;
        int32_t d1;
        int32_t d2;
        int32_t d;

        if (k2 < 0)
        {
            k2 = 0;
        }
        d1 = cgl_iso2d__isqrt((kx * kx) + (k1 * k1)); /* |u| */
        d2 = k1 + k2;                             /* |u+v| */
        d = (d1 < d2) ? d1 : d2;
        if (d > best_d)
        {
            best_d = d;
            best_k1 = k1;
        }
    }

    *ky1_out = best_k1;
    *ky2_out = (h > 0) ? ((VH - (w * best_k1)) / h) : 0;
    if (*ky2_out < 0)
    {
        *ky2_out = 0;
    }
}

/* -------------------------------------------------------------------------- */
/* camera                                                                     */
/* -------------------------------------------------------------------------- */

void cgl_iso2d_cam_set(cgl_iso2d_cam_t *cam, int16_t m00, int16_t m01,
                   int16_t m10, int16_t m11, int16_t kz,
                   int16_t ox, int16_t oy)
{
    cam->m00 = m00;
    cam->m01 = m01;
    cam->m10 = m10;
    cam->m11 = m11;
    cam->kz = kz;
    cam->ox = ox;
    cam->oy = oy;
}

void cgl_iso2d_project_rect(const cgl_iso2d_cam_t *cam,
                        int16_t wx0, int16_t wy0, int16_t ww, int16_t wh,
                        int16_t *x0, int16_t *y0, int16_t *x1, int16_t *y1)
{
    const int16_t xs[4] = { wx0, (int16_t)(wx0 + ww), (int16_t)(wx0 + ww), wx0 };
    const int16_t ys[4] = { wy0, wy0, (int16_t)(wy0 + wh), (int16_t)(wy0 + wh) };
    int32_t mnx = 32767;
    int32_t mxx = -32768;
    int32_t mny = 32767;
    int32_t mxy = -32768;
    int i;

    for (i = 0; i < 4; i++)
    {
        int16_t sx;
        int16_t sy;

        cgl_iso2d_project(cam, xs[i], ys[i], 0, &sx, &sy);
        if (sx < mnx) { mnx = sx; }
        if (sx > mxx) { mxx = sx; }
        if (sy < mny) { mny = sy; }
        if (sy > mxy) { mxy = sy; }
    }
    if (x0 != NULL) { *x0 = (int16_t)mnx; }
    if (y0 != NULL) { *y0 = (int16_t)mny; }
    if (x1 != NULL) { *x1 = (int16_t)mxx; }
    if (y1 != NULL) { *y1 = (int16_t)mxy; }
}

void cgl_iso2d_cam_fit(cgl_iso2d_cam_t *cam, cgl_iso2d_style_t style,
                   int16_t wx0, int16_t wy0, int16_t ww, int16_t wh,
                   int16_t vx, int16_t vy, int16_t vw, int16_t vh)
{
    /* World rectangle side lengths converted to pixels (Q12.4 -> px) */
    const int32_t wpx = (int32_t)ww >> CGL_EXT_Q4_SHIFT;
    const int32_t hpx = (int32_t)wh >> CGL_EXT_Q4_SHIFT;
    const int32_t span = wpx + hpx;
    /* Centre of the world rectangle, Q12.4: after projection it is the centre of
       the parallelogram */
    const int32_t cx = (int32_t)wx0 + ((int32_t)ww / 2);
    const int32_t cy = (int32_t)wy0 + ((int32_t)wh / 2);
    int32_t kx_q8 = 0;
    int32_t ky1_q8 = 0;
    int32_t ky2_q8 = 0;
    int guard;

    if ((cam == NULL) || (span <= 0) || (wpx <= 0) || (hpx <= 0) ||
        (vw <= 0) || (vh <= 0))
    {
        /* Degenerate input: hand back a unit camera that runs; never return
           uninitialised coefficients */
        cgl_iso2d_cam_set(cam, (int16_t)CGL_EXT_Q4_ONE, 0, 0, (int16_t)CGL_EXT_Q4_ONE,
                      CGL_ISO2D_LIFT_Q8, vx, vy);
        return;
    }

    if (style == CGL_ISO2D_STYLE_FLAT)
    {
        /* Flat: sx = x * k, sy = y * k, a uniform scale squeezed into the
           viewport, the same k for both axes */
        kx_q8 = cgl_iso2d__ratio_q8(wpx, vw);
        ky1_q8 = cgl_iso2d__ratio_q8(hpx, vh);
        if (ky1_q8 < kx_q8)
        {
            kx_q8 = ky1_q8;
        }
        ky1_q8 = kx_q8;
        ky2_q8 = kx_q8;
    }
    else
    {
        /*
         * Horizontal: world x and y both contribute to screen x (m01 is
         * negative), so the total width of the parallelogram is (W + H) * kx --
         * fill the viewport width straight up.
         * Widening horizontally does not cram the dots together (of the three
         * minimum spacing candidates only |u| and |v| contain kx, and they are
         * usually not the smallest one), so filling it is a pure win: wider
         * letters, easier to read.
         */
        kx_q8 = cgl_iso2d__ratio_q8(span, vw);

        if (style == CGL_ISO2D_STYLE_ISO_UNIFORM)
        {
            /* Textbook 30 deg: split the vertical budget evenly between the two
               axes, keep the sqrt(3) equilateral relation horizontally */
            ky1_q8 = cgl_iso2d__ratio_q8(span, vh);
            ky2_q8 = ky1_q8;
            {
                const int32_t limit_x = kx_q8;

                kx_q8 = (ky1_q8 * CGL_ISO2D_SQRT3_Q8) >> 8;
                if (kx_q8 > limit_x)
                {
                    kx_q8 = limit_x;
                }
            }
        }
        else
        {
            /* Fit-style isometric: the vertical budget is split so that the
               minimum dot spacing is maximal (derivation in the comment above) */
            cgl_iso2d__split_ky(kx_q8, wpx, hpx, vh, &ky1_q8, &ky2_q8);
        }
    }

    if (kx_q8 <= 0) { kx_q8 = 1; }
    if (ky1_q8 <= 0) { ky1_q8 = 1; }
    if (ky2_q8 <= 0) { ky2_q8 = 1; }

    /*
     * Finishing pass: fixed point rounding makes the actual bounding box 1~2 px
     * larger than the ideal value, so "computed proportionally to fit exactly"
     * ends up leaking one or two pixels out of the viewport. Rather than
     * guessing a conservative margin, **measure and then shrink**: set the
     * coefficients, project the four corners, and if it does not fit take one
     * step off each of the offending dimensions and retry.
     */
    for (guard = 0; guard < 64; guard++)
    {
        int16_t x0;
        int16_t y0;
        int16_t x1;
        int16_t y1;
        int32_t gx;
        int32_t gy;
        bool too_wide;
        bool too_tall;

        cam->m00 = (int16_t)kx_q8;
        cam->m01 = (int16_t)((style == CGL_ISO2D_STYLE_FLAT) ? 0 : -kx_q8);
        cam->m10 = (int16_t)((style == CGL_ISO2D_STYLE_FLAT) ? 0 : ky1_q8);
        cam->m11 = (int16_t)ky2_q8;
        cam->kz = (int16_t)CGL_ISO2D_LIFT_Q8;

        /* Project the world centre onto the viewport centre to derive the offset */
        gx = (((int32_t)cam->m00 * cx) + ((int32_t)cam->m01 * cy)) >> 12;
        gy = (((int32_t)cam->m10 * cx) + ((int32_t)cam->m11 * cy)) >> 12;
        cam->ox = (int16_t)((vx + (vw / 2)) - gx);
        cam->oy = (int16_t)((vy + (vh / 2)) - gy);

        cgl_iso2d_project_rect(cam, wx0, wy0, ww, wh, &x0, &y0, &x1, &y1);
        too_wide = (x1 > (int16_t)(vx + vw - 1)) || (x0 < vx);
        too_tall = (y1 > (int16_t)(vy + vh - 1)) || (y0 < vy);
        if (!too_wide && !too_tall)
        {
            return;
        }
        if (too_wide && (kx_q8 > 1))
        {
            kx_q8--;
        }
        if (too_tall)
        {
            /* Shrink both vertical coefficients together, the ratio stays
               essentially unchanged */
            if (ky1_q8 > 1) { ky1_q8--; }
            if (ky2_q8 > 1) { ky2_q8--; }
        }
        if ((!too_wide || (kx_q8 <= 1)) && (!too_tall || ((ky1_q8 <= 1) && (ky2_q8 <= 1))))
        {
            return; /* Already down to 1 and still does not fit: the viewport is smaller than the world, so accept it */
        }
    }
}

/* -------------------------------------------------------------------------- */
/* depth sort                                                                 */
/* -------------------------------------------------------------------------- */

void cgl_iso2d_sorter_reset(cgl_iso2d_sorter_t *s)
{
    if (s != NULL)
    {
        s->count = 0u;
        s->stamp = 0u;
        s->valid = false;
    }
}

void cgl_iso2d_sort(cgl_iso2d_sorter_t *s, const cgl_iso2d_cam_t *cam, const cgl_iso2d_src_t *src)
{
    uint16_t i;
    uint16_t n = 0u;
    uint16_t limit;
    int32_t k;

    if ((s == NULL) || (cam == NULL) || (src == NULL) || (src->read == NULL))
    {
        return;
    }
    /* Sort only once per step: the renderer calls draw per 4 px band, otherwise
       it would sort 43 times for nothing. What a "step" is belongs to the caller:
       `src->stamp` is their identity for the current set of positions. */
    if (s->valid && (s->stamp == src->stamp))
    {
        return;
    }

    /* The table cannot hold more than CGL_ISO2D_MAX_POINTS; the excess is ignored
       rather than written past the end. (For the physics world this can never
       happen: cgl_iso2d_phys2d.h gates the pool against the table at compile time.) */
    limit = (src->count > (uint16_t)CGL_ISO2D_MAX_POINTS) ? (uint16_t)CGL_ISO2D_MAX_POINTS
                                                      : src->count;

    for (k = 0; k < CGL_ISO2D_DEPTH_BUCKETS; k++)
    {
        s->bucket[k] = 0u;
    }

    /* pass 1: count how many elements are in each depth bucket */
    for (i = 0u; i < limit; i++)
    {
        cgl_iso2d_pos_t p;
        int32_t d;

        if (!src->read(src->ctx, i, &p))
        {
            continue;
        }
        d = cgl_iso2d_ground_y(cam, p.x, p.y) + cam->oy;
        /* Depths off screen are clamped to the two ends: they get culled anyway,
           and clamping saves a branch */
        if (d < 0)
        {
            d = 0;
        }
        else if (d >= CGL_ISO2D_DEPTH_BUCKETS)
        {
            d = CGL_ISO2D_DEPTH_BUCKETS - 1;
        }
        s->bucket[d]++;
        n++;
    }

    s->count = n;
    if (n == 0u)
    {
        s->stamp = src->stamp;
        s->valid = true;
        return;
    }

    /* pass 2: prefix sum -> bucket[d] becomes "the first slot for depth d" */
    {
        uint16_t sum = 0u;

        for (k = 0; k < CGL_ISO2D_DEPTH_BUCKETS; k++)
        {
            const uint16_t c = s->bucket[k];

            s->bucket[k] = sum;
            sum = (uint16_t)(sum + c);
        }
    }

    /* pass 3: place by increasing depth = far to near, so drawing in order gives
       occlusion straight away */
    for (i = 0u; i < limit; i++)
    {
        cgl_iso2d_pos_t p;
        int32_t d;

        if (!src->read(src->ctx, i, &p))
        {
            continue;
        }
        d = cgl_iso2d_ground_y(cam, p.x, p.y) + cam->oy;
        if (d < 0)
        {
            d = 0;
        }
        else if (d >= CGL_ISO2D_DEPTH_BUCKETS)
        {
            d = CGL_ISO2D_DEPTH_BUCKETS - 1;
        }
        s->order[s->bucket[d]] = (int16_t)i;
        s->bucket[d]++;
    }

    s->stamp = src->stamp;
    s->valid = true;
}

void cgl_iso2d_view_init(cgl_iso2d_view_t *v)
{
    if (v == NULL)
    {
        return;
    }
    cgl_iso2d_cam_set(&v->cam, (int16_t)CGL_EXT_Q4_ONE, 0, 0, (int16_t)CGL_EXT_Q4_ONE,
                  CGL_ISO2D_LIFT_Q8, 0, 0);
    cgl_iso2d_sorter_reset(&v->sorter);
}
