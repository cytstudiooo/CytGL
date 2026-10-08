// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_canvas.c
  * @brief   LCD UI framework - band canvas and software rasterizer.
  *
  *  Two coordinate systems, kept strictly apart
  *  -------------------------------------------
  *    primitive coordinates (what the public API takes)  -- +pan -->  screen
  *  Screen coordinates are the only system that is clipped and indexed to
  *  pixels (clip is in screen coordinates too).
  *
  *  So there is exactly one rule: **every public primitive function applies its
  *  own pan once**, and the internal helpers (canvas__row / canvas__span) take
  *  screen coordinates only and never pan again. Violating it produces the
  *  classic "panned twice" bug (the picture lands at double the offset);
  *  cgl_canvas_clear_clip() deliberately takes a no-pan path because it needs
  *  clip itself as the rectangle.
  ******************************************************************************
  */
#include <cytgl/cgl_canvas.h>

/** Q8 representation of 1.0. The canvas does not depend on cgl_anim.h (it sits
    at a lower layer), so it carries its own copy. */
#define CANVAS_ONE_Q8 256u

/* -------------------------------------------------------------------------- */
/* internal helpers (screen coordinates, never translate)                     */
/* -------------------------------------------------------------------------- */

/** Row pointer of screen row y, or NULL when the row is outside the band. */
CGL_AINLINE cgl_color_t *canvas__row(const cgl_canvas_t *c, int16_t y)
{
    if ((y < c->origin_y) || (y >= (int16_t)(c->origin_y + c->height)))
    {
        return NULL;
    }
    return c->buf + ((uint32_t)(y - c->origin_y) * (uint32_t)c->width);
}

/** Horizontal span in screen coordinates, clipped against the clip window. */
CGL_AINLINE void canvas__span(cgl_canvas_t *c, int16_t xa, int16_t xb, int16_t y, cgl_color_t color)
{
    cgl_color_t *row;
    int16_t i;
    int16_t n;

    if ((xa > xb) || (y < c->clip.y0) || (y > c->clip.y1))
    {
        return;
    }
    if (xa < c->clip.x0)
    {
        xa = c->clip.x0;
    }
    if (xb > c->clip.x1)
    {
        xb = c->clip.x1;
    }
    if (xa > xb)
    {
        return;
    }
    row = canvas__row(c, y);
    if (row == NULL)
    {
        return;
    }
    n = (int16_t)(xb - xa + 1);
    for (i = 0; i < n; i++)
    {
        row[xa + i] = color;
    }
}

/* primitive coordinates -> screen coordinates */
CGL_AINLINE int16_t canvas__tx(const cgl_canvas_t *c, int16_t x)
{
    return (int16_t)(x + c->pan.x);
}

CGL_AINLINE int16_t canvas__ty(const cgl_canvas_t *c, int16_t y)
{
    return (int16_t)(y + c->pan.y);
}

/* -------------------------------------------------------------------------- */
/* warp: source screen coordinates -> destination screen coordinates           */
/* -------------------------------------------------------------------------- */

/**
 * Row mapping: y' = ty + (y - ty) * (256 - p) / 256, rounded toward -inf.
 *
 * The rounding mode is the only subtle part here:
 *   - It must be monotonic, and the increment between adjacent source rows must
 *     be exactly 0 or 1 ((y-ty) grows by 1 per row and the multiplier is <= 256);
 *     "scatter writes leave no holes" is exactly what that property buys.
 *   - Rows use **rounding toward -inf**, not round-to-nearest: in this project
 *     the distance |y-ty| between a source row and the suction point is always
 *     less than 256 (the screen has only 172 rows and the suction point sits at
 *     the screen edge), so floor(d*(1-eps)) == d, i.e. for small p the row
 *     mapping is **exactly the identity**; with round-to-nearest, every row with
 *     |d| >= 128 would shift down a whole cell on the very first frame (measured:
 *     the top bar shakes for half a screen). Column mapping is the opposite --
 *     there it must be symmetric, otherwise a seam opens in the middle of the
 *     picture, see warp__x().
 */
CGL_AINLINE int32_t warp__y(const cgl_warp_t *w, int16_t y)
{
    int32_t inv = (int32_t)(256u - w->progress_q8);

    return (int32_t)w->ty + (((int32_t)(y - w->ty) * inv) >> 8);
}

/**
 * Column mapping: x' = xc + (x - cx) * k(y) / 256.
 *
 * k(y) is the product of three factors, each of which maps to one aspect of the
 * "does it look like a sprite" impression:
 *
 *   prof  how wide this row is at full collapse: narrow at the end near the
 *         suction point, wide at the far end, and the quadratic curve makes both
 *         sides **concave** like a horn mouth (a straight line would turn into a
 *         triangular cone).
 *   depth the depth of the funnel itself = 1-(1-p)^2: it starts closing from the
 *         bottom early, while the far end is still wide.
 *   shrink global narrowing = p^2: it only thins out as a whole later, then
 *         snaps into a vertical line at the end.
 *
 * At p = 0, depth = 0 and shrink = 0 -> k = 256, xc = cx -> exactly the
 * identity; at p = 256, k = 0 and every column lands on the suction point x ->
 * collapsed to a single point.
 */
CGL_AINLINE int32_t warp__k(const cgl_warp_t *w, int16_t y)
{
    int32_t t = (int32_t)(y - w->src_y0);
    int32_t p = (int32_t)w->progress_q8;
    int32_t inv = 256 - p;
    int32_t s;
    int32_t d;
    int32_t prof;
    int32_t depth;
    int32_t shrink;
    int32_t a = (int32_t)w->narrow_q8;

    if (t < 0)
    {
        t = 0;
    }
    s = (t * (int32_t)w->row_step_q8) >> 8; /* 0..256: 0 = far end, 256 = suction end */
    if (s > 256)
    {
        s = 256;
    }
    d = 256 - s;
    prof = a + (((256 - a) * d * d) >> 16);
    depth = 256 - ((inv * inv) >> 8);
    shrink = (p * p) >> 8;

    return ((256 - (((256 - prof) * depth) >> 8)) * (256 - shrink)) >> 8;
}

/**
 * Lateral S trajectory: along the source rows the offset is a **bell-shaped
 * curve tangent to the centre axis at both ends**, evaluated in fixed point.
 *
 * Parameterised by the normalised position t in [0,256] inside the source range
 * (`u = t/256`, 0 = far end, 256 = the end next to the suction point):
 *
 *     g(t) = 256*u^2(1-u)^2        (Q8: peak 32 @ t = 128, zero at both ends)
 *     S(y) = 3*s_amp_q8*phase*g(t) / 65536
 *
 * The shape of g is everything the S is: **g = 0 and g' = 0 at both ends
 * (tangent to the centre axis), maximum deflection at the midpoint** -- the page
 * first bows out to the right, then the lower half pulls back in and slips into
 * the corner.
 *
 *   - The offset multiplies the **phase**, not p: that is self-consistent with
 *     the vertical suction direction (OUT uses p, IN uses 1-p), so "playing it
 *     backwards" is exactly the inverse of the original process and the S never
 *     flips between the two directions; at p = 0, S == 0 and the identity
 *     property is intact (no shake on the first frame).
 *   - No lookup table: a 257-entry Q8 table would cost 514 B of ROM, whereas
 *     this needs just two multiply-adds per row.
 *   - **Monotonicity**: bounding the slope first, |g'| <= 6/256, gives
 *     |S'| = 3*(amp/256)*|g'|/256 <= 3*amp*6/65536, and that stays below one
 *     pixel per source row exactly while `s_amp_q8 <= 682`. That is the
 *     monotonicity bound quoted everywhere in this project; the derivation is
 *     owned by `CGL_CFG_S_AMP_MAX_Q8` in cytgl_config.h, and the shipped default
 *     (160 = half the screen width, which is all nav__s_amp() can ever derive)
 *     sits far below it, so "adjacent source rows never cross" (map_rect
 *     conservatism, no fan-shaped tearing) holds with a wide margin.
 *     The measured step of a few pixels at the two ends is **rounding**, not a
 *     crossing (see section G2).
 */
CGL_AINLINE int32_t warp__s(const cgl_warp_t *w, int16_t y)
{
    int32_t t;
    int32_t amp = (int32_t)w->s_amp_q8;
    int32_t g;

    if (amp == 0)
    {
        return 0; /* old Genie: no lateral offset at all, bit-identical to before S was added */
    }

    t = (int32_t)(y - w->src_y0);
    if (t <= 0)
    {
        return 0; /* outside the source range there is no contribution (and no
                     extrapolation either: that would bloat the bounding box) */
    }
    t = (t * (int32_t)w->row_step_q8) >> 8;
    if (t >= 256)
    {
        return 0;
    }

    /* g(t) = 256*u^2(1-u)^2, u = t/256 (Q8; peak 32 @ t = 128, zero at both
       ends). Writing it with s = 256 - t gives the integer form with only two
       multiplies and one shift:
           g = (t^2/256) * (s^2/256) / 256
       Both >>8 are powers of two, and truncation only makes g a little smaller;
       it **cannot invert an ordering** -- measured, g is strictly increasing up
       to t = 128 and strictly decreasing after (section G2 of
       tests/page_host_test.c locks this down row by row). For t <= 255 the first
       half is <= 254 and the second <= 256, so the product is <= 65024.
       WARNING: this shape function was written wrongly four times, with two
       symptoms: "not monotonic, it grows back" (split into two divisions, where
       the intermediate truncation threw away the fraction) and "shrunk by tens
       of times" (wrong scaling). The lesson: **get the shape right first, only
       then worry about saving multiplies**; u^2(1-u)^2 is exactly those two
       factors multiplied, so do not reshape it algebraically. (A cubic
       polynomial t^2(3-4t+t^3)/256^2 was used before, and its shape was in fact
       also correct, but fixed-point scaling is far easier to get wrong -- with
       this product form the correctness is obvious at a glance.) */
    {
        const int32_t s = 256 - t;

        g = (((t * t) >> 8) * ((s * s) >> 8)) >> 8;
    }

    return ((3 * amp * (int32_t)w->progress_q8) * g) >> 16;
}

CGL_AINLINE int32_t warp__x(const cgl_warp_t *w, int16_t x, int16_t y)
{
    int32_t k = warp__k(w, y);
    int32_t xc = (int32_t)w->cx + (((int32_t)(w->tx - w->cx) * (int32_t)w->progress_q8) >> 8);

    /* Column mapping must round **symmetrically** (round-to-nearest): with
       rounding toward -inf the same "0.4% narrowing" behaves differently on the
       two sides of the centre -- the left side is unchanged while the right side
       shifts left by a whole pixel, and that seam in the middle is plainly
       visible on a dot matrix (measured: it splits open on the very first frame
       at p=1). With round-to-nearest both sides pull inwards and the 1 px step
       is pushed to the outside of the picture, where it cannot be seen.
       k <= 256 and xc is "centre + offset", so x' is always inside the
       conservative range; anything out of bounds is absorbed by the caller's
       clip. */
    return xc + ((((int32_t)(x - w->cx) * k) + 128) >> 8) + warp__s(w, y);
}

/** Source screen row -> destination screen row (identity without a warp). */
CGL_AINLINE int16_t canvas__wy(const cgl_canvas_t *c, int16_t y)
{
    return (c->warp == NULL) ? y : (int16_t)warp__y(c->warp, y);
}

/** Source screen column -> destination screen column (identity without a warp).
    y must be the **source** row that pixel lives on. */
CGL_AINLINE int16_t canvas__wx(const cgl_canvas_t *c, int16_t x, int16_t y)
{
    return (c->warp == NULL) ? x : (int16_t)warp__x(c->warp, x, y);
}

/**
 * Whether source coordinates fall inside the source clip window. Always true
 * without a warp (then clipping uses the target-space clip).
 */
CGL_AINLINE bool canvas__src_ok(const cgl_canvas_t *c, int16_t x, int16_t y)
{
    return (c->warp == NULL) || cgl_rect_contains_point(c->src_clip, x, y);
}

/** Clamp a **source** column range to the source clip window (the source of the
    warp is confined to this level). */
CGL_AINLINE void canvas__src_clamp_x(const cgl_canvas_t *c, int16_t *x0, int16_t *x1)
{
    if (c->warp == NULL)
    {
        return;
    }
    if (*x0 < c->src_clip.x0)
    {
        *x0 = c->src_clip.x0;
    }
    if (*x1 > c->src_clip.x1)
    {
        *x1 = c->src_clip.x1;
    }
}

/** Whether a source row is inside the source clip window (cull whole rows in
    source space first and save the per-column mapping). */
CGL_AINLINE bool canvas__src_row_ok(const cgl_canvas_t *c, int16_t y)
{
    return (c->warp == NULL) || ((y >= c->src_clip.y0) && (y <= c->src_clip.y1));
}

/* -------------------------------------------------------------------------- */
/* division that rounds the way the pre-image maths needs                     */
/* -------------------------------------------------------------------------- */

/*
 * The row map is `warp__y(y) = ty + floor((y - ty) * inv / 256)`, so inverting it
 * means dividing by `inv` with the *floor* of the exact quotient -- and `ty` may sit
 * above or below the band, which makes the numerator negative. C's `/` truncates
 * toward zero, so it is a floor only for a positive numerator; these two say what
 * they mean for either sign. (`canvas__ceil_div` is the other direction: the
 * smallest `d` whose product reaches `a`.)
 */
CGL_AINLINE int32_t canvas__floor_div(int32_t a, int32_t b)
{
    return (a >= 0) ? (a / b) : -(int32_t)(((int32_t)-a + b - 1) / b);
}

CGL_AINLINE int32_t canvas__ceil_div(int32_t a, int32_t b)
{
    return -canvas__floor_div(-a, b);
}

/**
 * The **source-space window that can still reach this band**, inclusive, in screen
 * coordinates -- the source rows and columns whose pixels may land inside the clip
 * window. Empty when nothing of this subtree can be seen at all.
 *
 * Why a row walk needs this: a widget is drawn **once per band**, so a rasteriser
 * that walks its whole shape in every band pays 43 times for one picture. Without a
 * warp the answer is simply the clip window, and that is what lets
 * `cgl_canvas_arc()` / `cgl_canvas_line_thick()` cut a 160 row walk down to the four
 * rows of the band. **With a warp that shortcut disappears**: the deformation
 * compresses the subtree towards the suction point, so a four row band can receive
 * source rows from far outside it -- and the walk then has to cover the whole shape
 * in every band, with a per-pixel warp mapping on top of it. Measured on the watch
 * face (the densest page in the project), one frame of an S_OUT transition cost
 * 18.7 M host cycles that way against 1.8 M for the same page flat: the suction got
 * four frames for the whole transition, which is exactly what "the animation
 * stutters" is.
 *
 * The pre-image is still a *bounded* range, and this is it: `warp__y()` is
 * non-decreasing in `y`, so the source rows landing in `[clip.y0, clip.y1]` are
 * exactly those with
 *
 *     d in [ ceil((clip.y0 - ty) * 256 / inv),
 *            floor(((clip.y1 - ty + 1) * 256 - 1) / inv) ],        d = y - ty
 *
 * (`floor(d * inv / 256) >= A` is equivalent to `d * inv >= A * 256`, and
 * `floor(...) <= B` to `d * inv <= (B + 1) * 256 - 1` -- so both bounds are exact,
 * not conservative). At `inv == 0` the whole subtree has collapsed onto the suction
 * row. The rows are then intersected with the source clip, which is what bounds the
 * range once the pre-image really is the whole page; the columns are the source clip
 * outright, because the column map is affine *in x* within a row and a source column
 * outside that window can never be drawn.
 *
 * WARNING: this is the **bound every pixel of the shape has to be tested against**,
 * not "the pixels that will be drawn": the destination clip test, the coverage and
 * the blend still belong to the rasteriser (see `canvas__blend()`). What it removes
 * is only the part of the walk that provably cannot produce a pixel.
 *
 * WARNING: while a warp is installed this reads `src_clip`, which the tree walker
 * maintains per level (see `cgl_canvas.h`). A caller that installs a warp by hand
 * must install a source clip with it.
 */
CGL_AINLINE cgl_rect_t canvas__src_window(const cgl_canvas_t *c)
{
    const cgl_warp_t *w = c->warp;
    int32_t inv;
    int32_t ty;
    int32_t lo;
    int32_t hi;

    if (w == NULL)
    {
        return c->clip; /* the row map is the identity: the band *is* the window */
    }

    inv = (int32_t)(256u - w->progress_q8);
    ty = (int32_t)w->ty;

    if (inv <= 0)
    {
        /* Fully collapsed: every source row lands on the suction row. */
        if ((ty < (int32_t)c->clip.y0) || (ty > (int32_t)c->clip.y1))
        {
            return cgl_rect_none();
        }
        lo = (int32_t)c->src_clip.y0;
        hi = (int32_t)c->src_clip.y1;
    }
    else
    {
        lo = ty + canvas__ceil_div(((int32_t)c->clip.y0 - ty) * 256, inv);
        hi = ty + canvas__floor_div((((int32_t)c->clip.y1 - ty + 1) * 256) - 1, inv);
        if (lo < (int32_t)c->src_clip.y0)
        {
            lo = (int32_t)c->src_clip.y0;
        }
        if (hi > (int32_t)c->src_clip.y1)
        {
            hi = (int32_t)c->src_clip.y1;
        }
    }
    if ((lo > hi) || (c->src_clip.x0 > c->src_clip.x1))
    {
        return cgl_rect_none();
    }
    return cgl_rect(c->src_clip.x0, (int16_t)lo, c->src_clip.x1, (int16_t)hi);
}

/* -------------------------------------------------------------------------- */
/* public API                                                                 */
/* -------------------------------------------------------------------------- */

void cgl_canvas_init(cgl_canvas_t *c, cgl_color_t *buf,
                    int16_t width, int16_t height, int16_t origin_y)
{
    c->buf = buf;
    c->width = width;
    c->height = height;
    c->origin_y = origin_y;
    c->clip = cgl_rect(0, origin_y, (int16_t)(width - 1), (int16_t)(origin_y + height - 1));
    c->pan = (cgl_point_t){ 0, 0 };
    c->warp = NULL;
    /* The source clip window defaults to "no clipping": without a warp it takes
       no part in any test, and with a warp the tree walker sets it to that
       widget's frame at the level where the warp is installed. */
    c->src_clip = cgl_rect_none();

    /* Group opacity is **off** by default: the canvas does not know the band
       geometry, so scratch must be installed explicitly by the display layer
       (cgl_display_present installs it once per band). That way a host test can
       hand it a buffer of its own at any time to exercise this path, and a build
       with CGL_CFG_ALPHA = 0 does nothing at all. */
    c->alpha_scratch = NULL;
    c->alpha_stride = 0u;
    c->alpha_levels = 0u;
    c->alpha_used = 0u;

    /* Ink alpha starts at "as asked for", which is also the value that skips the
       multiply entirely -- a build that never calls the setter pays one comparison
       per blended pixel and nothing else. */
    c->ink_alpha_q8 = (uint16_t)CGL_CANVAS_INK_OPAQUE;
}

void cgl_canvas_set_ink_alpha(cgl_canvas_t *c, uint16_t alpha_q8)
{
    if (c == NULL)
    {
        return;
    }
    c->ink_alpha_q8 = (alpha_q8 > (uint16_t)CGL_CANVAS_INK_OPAQUE)
                          ? (uint16_t)CGL_CANVAS_INK_OPAQUE
                          : alpha_q8;
}

uint16_t cgl_canvas_get_ink_alpha(const cgl_canvas_t *c)
{
    return (c != NULL) ? c->ink_alpha_q8 : (uint16_t)CGL_CANVAS_INK_OPAQUE;
}

/**
 * Apply the canvas ink alpha to a coverage, Q8. Identity (and free) at the default.
 *
 * WARNING: this is the *only* place the ink alpha is folded in, so that "which
 * primitives honour it" is answered by "which ones come through here" and not by a
 * list somebody has to keep in step with the code.
 */
CGL_AINLINE uint16_t canvas__ink(uint16_t cov_q8, uint16_t alpha_q8)
{
    if (alpha_q8 >= (uint16_t)CGL_CANVAS_INK_OPAQUE)
    {
        return cov_q8;
    }
    return (uint16_t)(((uint32_t)cov_q8 * (uint32_t)alpha_q8) >> 8);
}

void cgl_canvas_set_src_clip(cgl_canvas_t *c, cgl_rect_t clip)
{
    c->src_clip = clip;
}

cgl_rect_t cgl_canvas_get_src_clip(const cgl_canvas_t *c)
{
    return c->src_clip;
}

cgl_rect_t cgl_canvas_src_window(const cgl_canvas_t *c)
{
    return canvas__src_window(c);
}

void cgl_canvas_set_warp(cgl_canvas_t *c, const cgl_warp_t *w)
{
    c->warp = w;
}

const cgl_warp_t *cgl_canvas_get_warp(const cgl_canvas_t *c)
{
    return c->warp;
}

int16_t cgl_canvas_map_y(const cgl_canvas_t *c, int16_t y)
{
    return canvas__wy(c, y);
}

int16_t cgl_canvas_map_x(const cgl_canvas_t *c, int16_t x, int16_t y)
{
    return canvas__wx(c, x, y);
}

cgl_rect_t cgl_canvas_map_rect(const cgl_canvas_t *c, cgl_rect_t r)
{
    cgl_rect_t o;
    int16_t xa;
    int16_t xb;

    if (cgl_rect_is_empty(r))
    {
        return r;
    }
    if (c->warp == NULL)
    {
        return r;
    }

    /* Rows are monotonic: the two endpoints are enough. Columns are "one affine
       map per row", and the extremes can only occur at the four corners of the
       top and bottom edges (k(y) and xc(y) are both monotonic in y), so taking
       the extremes of the four corners is enough -- a conservative bounding box:
       drawing an extra column is fine, wrongly culling is not.
       * The exception is the S trajectory: S(y) peaks in the **middle** of the
       source range, which neither the top nor the bottom edge of the rectangle
       touches, so the four corners alone would cull away the widest stretch (a
       chunk cut out of the middle of the page). So one more edge is added, the
       centre line -- the peak lies on it, and since S is symmetric about the
       midpoint, taking the midpoint is enough. */
    o.y0 = (int16_t)warp__y(c->warp, r.y0);
    o.y1 = (int16_t)warp__y(c->warp, r.y1);
    xa = (int16_t)warp__x(c->warp, r.x0, r.y0);
    xb = (int16_t)warp__x(c->warp, r.x1, r.y0);
    o.x0 = (xa < xb) ? xa : xb;
    o.x1 = (xa < xb) ? xb : xa;
    xa = (int16_t)warp__x(c->warp, r.x0, r.y1);
    xb = (int16_t)warp__x(c->warp, r.x1, r.y1);
    if (xa < o.x0)
    {
        o.x0 = xa;
    }
    if (xb < o.x0)
    {
        o.x0 = xb;
    }
    if (xa > o.x1)
    {
        o.x1 = xa;
    }
    if (xb > o.x1)
    {
        o.x1 = xb;
    }
    if (c->warp->s_amp_q8 != 0u)
    {
        /* The extreme of S is **not at the corners** of the rectangle: the
           lateral offset is largest in the interior of the source range, and
           which row the peak falls on drifts with p (the row mapping
           y' = ty + (y-ty)(1-p) flattens the source range, xc(y) is monotonic,
           and the rate of change of k(y) also varies with p, so the
           rightmost/leftmost point wanders inside the "upper half").
           Measured with the four corners only: at p=160 the body reaches
           x=391 while the corners only reach 356 -- culling would slice off a
           vertical edge.
           The approach is "a predictable coarse grid + denser sampling near the
           peak":
             - one sample every 16 rows of the source range (11 for a whole page
               at 320x172, fewer for child widgets: their sample rows are clamped
               into their own frame, and duplicates only cost one column
               mapping);
             - plus rows at the analytic peak of S(y) and +/-2 / +/-4 / +/-8 rows
               around it -- at low p the peak drifts far away, but then the
               offset itself is small and the coarse grid already suffices; at
               high p the peak sits near the centre and these points land right
               on its shoulders.
           WARNING: sample rows must be clamped into "this rectangle ^ the source
           range": S(y) is identically 0 outside the source range, so sampling a
           row outside it turns "no offset at all" into a phantom offset (the
           bounding box bloats -- that only wastes a few drawn columns and never
           cuts content, but hugging the content tightly is the whole value of a
           bounding box). */
        const int16_t sy0 = c->warp->src_y0;
        const int16_t sy1 = (int16_t)(c->warp->src_y0 + c->warp->src_h - 1);
        uint8_t pass;

        for (pass = 0u; pass < 2u; pass++)
        {
            /* First pass: one sample every 16 rows of the source range (11 for a
               whole page).
               Second pass: the single midpoint of the source range -- at high p
               the peak sits right next to it, and adding it costs one more
               mapping while smoothing out "the peak falls between two grid
               points". */
            int32_t n = (pass == 0u) ? ((int32_t)c->warp->src_h / 16) : 0;
            int32_t i;

            for (i = 0; i <= n; i++)
            {
                int32_t ym = (pass == 0u)
                                 ? ((int32_t)sy0 + (i * 16))
                                 : ((int32_t)sy0 + ((int32_t)c->warp->src_h / 2));

                if (ym < (int32_t)r.y0)
                {
                    ym = (int32_t)r.y0;
                }
                if (ym > (int32_t)r.y1)
                {
                    ym = (int32_t)r.y1;
                }
                if (ym < (int32_t)sy0)
                {
                    ym = (int32_t)sy0;
                }
                if (ym > (int32_t)sy1)
                {
                    ym = (int32_t)sy1;
                }

                xa = (int16_t)warp__x(c->warp, r.x0, (int16_t)ym);
                xb = (int16_t)warp__x(c->warp, r.x1, (int16_t)ym);
                if (xa < o.x0)
                {
                    o.x0 = xa;
                }
                if (xb < o.x0)
                {
                    o.x0 = xb;
                }
                if (xa > o.x1)
                {
                    o.x1 = xa;
                }
                if (xb > o.x1)
                {
                    o.x1 = xb;
                }
            }
        }

        /* * Sampling uses "a finite number of points", so one further sample
           spacing of margin is required: the offset in the stretch between two
           sample points can exceed the sampled value by at most
           "spacing x S'(y)". The upper bound on S'(y) is 0.1875 (see warp__s),
           so with a 16-row spacing the error is < 3 px. Here the margin is
           widened by the **spacing itself** (16 px), far more than necessary,
           but it buys a hard guarantee that does not depend on arithmetic
           details: the bounding box always covers everything, and never wrongly
           culls because "the peak happened to fall between two sample points".
           Measured worst underestimate is 2 px (full p sweep at the shipped
           amp = 160), so 16 px has an 8x margin.
           WARNING: widen only when s_amp != 0: with s_amp = 0 (old Genie) the
           four-corner sampling is **exact**, and adding margin would break the
           identity property at p=0 (map_rect returning its input unchanged). */
        o.x0 = (int16_t)(o.x0 - 16);
        o.x1 = (int16_t)(o.x1 + 16);
    }
    return o;
}

cgl_color_t *cgl_canvas_row(const cgl_canvas_t *c, int16_t y)
{
    return canvas__row(c, y);
}

cgl_rect_t cgl_canvas_get_clip(const cgl_canvas_t *c)
{
    return c->clip;
}

void cgl_canvas_set_clip(cgl_canvas_t *c, cgl_rect_t clip)
{
    c->clip = clip;
}

void cgl_canvas_reset_clip(cgl_canvas_t *c)
{
    c->clip = cgl_rect(0, c->origin_y, (int16_t)(c->width - 1),
                      (int16_t)(c->origin_y + c->height - 1));
}

cgl_point_t cgl_canvas_get_pan(const cgl_canvas_t *c)
{
    return c->pan;
}

void cgl_canvas_set_pan(cgl_canvas_t *c, cgl_point_t pan)
{
    c->pan = pan;
}

void cgl_canvas_clear(cgl_canvas_t *c, cgl_color_t color)
{
    int32_t i;
    int32_t n = (int32_t)c->width * (int32_t)c->height;

    for (i = 0; i < n; i++)
    {
        c->buf[i] = color;
    }
}

void cgl_canvas_clear_clip(cgl_canvas_t *c, cgl_color_t color)
{
    int16_t y;

    /* Careful: cgl_canvas_fill_rect(c, c->clip, color) cannot be used here --
       clip is already in screen coordinates, and panning it once more would be
       wrong. This goes straight down the no-pan internal path. */
    for (y = c->clip.y0; y <= c->clip.y1; y++)
    {
        canvas__span(c, c->clip.x0, c->clip.x1, y, color);
    }
}

/**
 * Single-pixel plot (**internal version**).
 *
 * Why an internal version is needed: `cgl_canvas_pixel()` is a public function,
 * and the Bresenham line (`cgl_canvas_line`; the floor grid lines walk tens of
 * thousands of pixels every frame) calls it once per step, while a public
 * function cannot be forced inline safely (adding always_inline to an
 * externally visible function raises a warning, and other translation units
 * would still have to make the call). So the static function here is what
 * actually does the work -- the line loop calls it and the public API calls it
 * too, and the two paths produce exactly the same pixels.
 */
CGL_AINLINE void canvas__pixel(cgl_canvas_t *c, int16_t x, int16_t y, cgl_color_t color)
{
    cgl_color_t *row;
    int16_t px = canvas__tx(c, x);
    int16_t py = canvas__ty(c, y);
    int16_t sx;
    int16_t sy;

    if (!canvas__src_ok(c, px, py))
    {
        return;
    }
    sx = canvas__wx(c, px, py);
    sy = canvas__wy(c, py);

    if (!cgl_rect_contains_point(c->clip, sx, sy))
    {
        return;
    }
    row = canvas__row(c, sy);
    if (row != NULL)
    {
        row[sx] = color;
    }
}

void cgl_canvas_pixel(cgl_canvas_t *c, int16_t x, int16_t y, cgl_color_t color)
{
    canvas__pixel(c, x, y, color);
}

void cgl_canvas_hline(cgl_canvas_t *c, int16_t x0, int16_t x1, int16_t y, cgl_color_t color)
{
    int16_t py;
    int16_t sy;

    if (x0 > x1)
    {
        int16_t t = x0;
        x0 = x1;
        x1 = t;
    }
    py = canvas__ty(c, y);
    if (!canvas__src_row_ok(c, py))
    {
        return;
    }
    x0 = canvas__tx(c, x0);
    x1 = canvas__tx(c, x1);
    canvas__src_clamp_x(c, &x0, &x1);
    if (x0 > x1)
    {
        return;
    }
    sy = canvas__wy(c, py);
    /* Column mapping is monotonic in x (k >= 0), so x0 <= x1 still implies
       x0' <= x1' after mapping. */
    canvas__span(c, canvas__wx(c, x0, py), canvas__wx(c, x1, py), sy, color);
}

void cgl_canvas_vline(cgl_canvas_t *c, int16_t x, int16_t y0, int16_t y1, cgl_color_t color)
{
    int16_t y;
    int16_t sx = canvas__tx(c, x);

    if (y0 > y1)
    {
        int16_t t = y0;
        y0 = y1;
        y1 = t;
    }

    if (c->warp != NULL)
    {
        /* Warp path: map per **source** row (column mapping depends on the row,
           so the loop cannot run over destination rows). */
        for (y = y0; y <= y1; y++)
        {
            int16_t py = canvas__ty(c, y);
            int16_t sy;
            int16_t dx;
            cgl_color_t *row;

            if (!canvas__src_ok(c, sx, py))
            {
                continue;
            }
            sy = canvas__wy(c, py);
            dx = canvas__wx(c, sx, py);
            if ((sy < c->clip.y0) || (sy > c->clip.y1) ||
                (dx < c->clip.x0) || (dx > c->clip.x1))
            {
                continue;
            }
            row = canvas__row(c, sy);
            if (row != NULL)
            {
                row[dx] = color;
            }
        }
        return;
    }

    y0 = canvas__ty(c, y0);
    y1 = canvas__ty(c, y1);

    if ((sx < c->clip.x0) || (sx > c->clip.x1))
    {
        return;
    }
    if (y0 < c->clip.y0)
    {
        y0 = c->clip.y0;
    }
    if (y1 > c->clip.y1)
    {
        y1 = c->clip.y1;
    }
    for (y = y0; y <= y1; y++)
    {
        cgl_color_t *row = canvas__row(c, y);
        if (row != NULL)
        {
            row[sx] = color;
        }
    }
}

void cgl_canvas_line(cgl_canvas_t *c, int16_t x0, int16_t y0, int16_t x1, int16_t y1, cgl_color_t color)
{
    int16_t dx = (int16_t)((x1 > x0) ? (x1 - x0) : (x0 - x1));
    int16_t dy = (int16_t)((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int16_t sx = (x0 < x1) ? 1 : -1;
    int16_t sy = (y0 < y1) ? 1 : -1;
    int16_t err = (int16_t)(dx - dy);

    /* cgl_canvas_pixel() pans by itself, so the Bresenham walk here uses
       untranslated coordinates. */
    for (;;)
    {
        canvas__pixel(c, x0, y0, color);
        if ((x0 == x1) && (y0 == y1))
        {
            break;
        }
        {
            int16_t e2 = (int16_t)(2 * err);
            if (e2 > -dy)
            {
                err = (int16_t)(err - dy);
                x0 = (int16_t)(x0 + sx);
            }
            if (e2 < dx)
            {
                err = (int16_t)(err + dx);
                y0 = (int16_t)(y0 + sy);
            }
        }
    }
}

void cgl_canvas_fill_rect(cgl_canvas_t *c, cgl_rect_t r, cgl_color_t color)
{
    cgl_rect_t s;
    cgl_rect_t k;
    int16_t y;

    if (c->warp != NULL)
    {
        /* Warp path: for each **source** row, work out "which row it is
           compressed onto and how wide it is compressed to".
           The same destination row can be written by several source rows (while
           compressing), and the last write wins -- that makes no difference for
           a solid rectangle, and for content it is "the most recently sampled
           row", consistent with how point sampling looks. */
        int16_t sx0 = canvas__tx(c, r.x0);
        int16_t sx1 = canvas__tx(c, r.x1);

        canvas__src_clamp_x(c, &sx0, &sx1);
        if (sx0 > sx1)
        {
            return;
        }
        for (y = r.y0; y <= r.y1; y++)
        {
            int16_t py = canvas__ty(c, y);
            int16_t dy;

            if (!canvas__src_row_ok(c, py))
            {
                continue;
            }
            dy = canvas__wy(c, py);
            if ((dy < c->origin_y) || (dy >= (int16_t)(c->origin_y + c->height)) ||
                (dy < c->clip.y0) || (dy > c->clip.y1))
            {
                continue;
            }
            canvas__span(c, canvas__wx(c, sx0, py), canvas__wx(c, sx1, py), dy, color);
        }
        return;
    }

    s = cgl_rect(canvas__tx(c, r.x0), canvas__ty(c, r.y0),
                canvas__tx(c, r.x1), canvas__ty(c, r.y1));
    k = cgl_rect_intersect(s, c->clip);

    if (cgl_rect_is_empty(k))
    {
        return;
    }
    for (y = k.y0; y <= k.y1; y++)
    {
        canvas__span(c, k.x0, k.x1, y, color);
    }
}

void cgl_canvas_rect(cgl_canvas_t *c, cgl_rect_t r, cgl_color_t color)
{
    /* hline/vline pan by themselves, so the original coordinates are passed
       here. */
    cgl_canvas_hline(c, r.x0, r.x1, r.y0, color);
    cgl_canvas_hline(c, r.x0, r.x1, r.y1, color);
    cgl_canvas_vline(c, r.x0, r.y0, r.y1, color);
    cgl_canvas_vline(c, r.x1, r.y0, r.y1, color);
}

void cgl_canvas_disc(cgl_canvas_t *c, int16_t cx, int16_t cy, int16_t radius, cgl_color_t color)
{
    int16_t x;
    int16_t y;
    int16_t err;

    if (radius <= 0)
    {
        canvas__pixel(c, cx, cy, color);
        return;
    }

    x = radius;
    y = 0;
    err = (int16_t)(1 - radius);
    while (x >= y)
    {
        cgl_canvas_hline(c, (int16_t)(cx - x), (int16_t)(cx + x), (int16_t)(cy + y), color);
        cgl_canvas_hline(c, (int16_t)(cx - x), (int16_t)(cx + x), (int16_t)(cy - y), color);
        cgl_canvas_hline(c, (int16_t)(cx - y), (int16_t)(cx + y), (int16_t)(cy + x), color);
        cgl_canvas_hline(c, (int16_t)(cx - y), (int16_t)(cx + y), (int16_t)(cy - x), color);

        y++;
        if (err < 0)
        {
            err = (int16_t)(err + (2 * y) + 1);
        }
        else
        {
            x--;
            err = (int16_t)(err + (2 * (y - x)) + 1);
        }
    }
}

void cgl_canvas_dot(cgl_canvas_t *c, int16_t cx, int16_t cy, uint8_t size, cgl_color_t color)
{
    int16_t half = (int16_t)(size >> 1);
    int16_t xa = (int16_t)(cx - half);
    int16_t xb = (int16_t)(xa + size - 1);
    int16_t y;

    if (c->warp != NULL)
    {
        int16_t sx0 = canvas__tx(c, xa);
        int16_t sx1 = canvas__tx(c, xb);

        canvas__src_clamp_x(c, &sx0, &sx1);
        if (sx0 > sx1)
        {
            return;
        }
        for (y = (int16_t)(cy - half); y <= (int16_t)(cy - half + size - 1); y++)
        {
            int16_t py = canvas__ty(c, y);

            if (!canvas__src_row_ok(c, py))
            {
                continue;
            }
            canvas__span(c, canvas__wx(c, sx0, py), canvas__wx(c, sx1, py),
                         canvas__wy(c, py), color);
        }
        return;
    }

    /* Calls the internal span directly, so it must pan by itself (it cannot go
       through hline, which is called once per row and would add a function call
       -- in a dot matrix this is the hottest path). */
    for (y = (int16_t)(cy - half); y <= (int16_t)(cy - half + size - 1); y++)
    {
        int16_t py = canvas__ty(c, y);

        canvas__span(c, canvas__wx(c, canvas__tx(c, xa), py),
                     canvas__wx(c, canvas__tx(c, xb), py), canvas__wy(c, py), color);
    }
}
/* -------------------------------------------------------------------------- */
/* radial primitives: antialiased pen strokes                                 */
/* -------------------------------------------------------------------------- */

/*
 * Why these three are antialiased and the rest of the canvas is not
 *
 * A rectangle, a horizontal line and a blit have no jaggies to remove -- their
 * edges land on the pixel grid. A **circle, a hand and a ring** are the shapes
 * where the grid is visible: a 70 px radius circle drawn crisp has a staircase two
 * to three pixels deep wherever the tangent is near 45 degrees, and on a 250 dpi
 * panel that reads as a burr rather than as a curve. So the three radial strokes
 * below trade a multiply and a divide per boundary pixel for a smooth edge, while
 * the block primitives (`fill_rect`, `line`, `disc`, `dot`) stay crisp: they are on
 * the dot matrix / bar / panel hot path, where "one pixel is one pixel" is the
 * whole point.
 *
 * How the coverage is computed, and why there is no sqrt in the inner loop
 *
 * The ideal shape is a distance field: a pixel is fully covered when it is at
 * least half a pixel inside the edge, not drawn at all when it is half a pixel
 * outside, and ramped linearly in between. The exact distance needs a square root
 * (~160 cycles per pixel); instead this works in **squared** distances, which cost
 * two multiplies, and linearises only where it matters:
 *
 *      (r^2 - d^2) / (r + d) == r - d,   and near the edge  r + d == 2r
 *
 * so `(r^2 - d^2) / 2r` is the signed distance in pixels for any pixel close
 * enough to the edge to be partially covered -- and for a pixel far inside, the
 * `r^2` comparison has already answered "fully covered". That is one division per
 * boundary pixel and no root anywhere, and the approximation is exact where it is
 * used (the error factor is d/r, i.e. 1.2 % on the outer edge of an 80 px dial and
 * 0 on the stroke's centre line).
 *
 * Distances are carried in **Q6** (1/64 px): enough levels for a 64 step alpha
 * ramp, and small enough that `dx^2 + dy^2` stays inside a uint32 for any
 * coordinate a 320x172 panel can hold.
 *
 * WARNING: `canvas__blend()` is deliberately **not** force-inlined. An earlier
 * version of the circle used the force-inlined `canvas__pixel()` eight times per
 * loop body, so the whole src-clip/warp/clip/row path was emitted eight times:
 * 4076 bytes for a 25 line algorithm, a quarter of the reference application's
 * flash growth. Code size is part of a primitive's cost.
 */

/** Coverage of one pixel from its squared distance to an edge. All values Q6. */
CGL_AINLINE uint16_t canvas__cov(uint32_t d2_q12, uint32_t edge2_q12, uint32_t edge_q6)
{
    int32_t inner_q6 = (int32_t)edge_q6 - 32; /* the ramp starts half a pixel inside */
    int32_t outer_q6 = (int32_t)edge_q6 + 32; /* and ends half a pixel outside */
    uint32_t inner2;
    uint32_t outer2;
    int32_t diff;
    uint32_t mag;
    int32_t d_est;
    int32_t delta;

    if (inner_q6 < 0)
    {
        inner_q6 = 0;
    }
    inner2 = (uint32_t)(inner_q6 * inner_q6);
    outer2 = (uint32_t)(outer_q6 * outer_q6);

    /* Two comparisons answer the overwhelming majority of pixels -- the whole core
       and everything outside -- with no division at all. */
    if (d2_q12 <= inner2)
    {
        return 256u;
    }
    if (d2_q12 >= outer2)
    {
        return 0u;
    }

    /* Inside the ramp. `(edge^2 - d^2) / (edge + d)` is exactly `edge - d`, and `d`
       is known to far better than a pixel from the first-order estimate
       `edge - (edge^2 - d^2) / 2*edge`, so one refinement step is exact. Using
       `2*edge` as the divisor and skipping the refinement is the tempting shortcut,
       and it is visibly wrong on a small pen: a pixel 2.0 px from the centre of a
       2.5 px pen came out at 240/256 instead of fully covered, which lays a faint
       film over the inside of every thin stroke. */
    diff = (int32_t)edge2_q12 - (int32_t)d2_q12;
    mag = (uint32_t)((diff < 0) ? -diff : diff);
    d_est = (int32_t)edge_q6 - (int32_t)(mag / (2u * edge_q6));
    if (d_est < 0)
    {
        d_est = 0;
    }
    delta = (int32_t)(mag / (uint32_t)(edge_q6 + (uint32_t)d_est)); /* |edge - d|, Q6 */
    if (diff < 0)
    {
        delta = -delta;
    }
    delta += 32; /* the coverage ramp is centred on the edge */
    if (delta <= 0)
    {
        return 0u;
    }
    if (delta >= 64)
    {
        return 256u;
    }
    return (uint16_t)((uint32_t)delta * 4u); /* Q6 -> Q8 */
}

/** Pixel at (x, y), moved `cov_q8` of the way from what is there to `color`. */
static void canvas__blend(cgl_canvas_t *c, int16_t x, int16_t y, cgl_color_t color, uint16_t cov_q8)
{
    int16_t px = canvas__tx(c, x);
    int16_t py = canvas__ty(c, y);
    int16_t sx;
    int16_t sy;
    cgl_color_t *row;

    if (cov_q8 == 0u)
    {
        return;
    }
    if (!canvas__src_ok(c, px, py))
    {
        return;
    }
    sx = canvas__wx(c, px, py);
    sy = canvas__wy(c, py);
    if (!cgl_rect_contains_point(c->clip, sx, sy))
    {
        return;
    }
    row = canvas__row(c, sy);
    if (row == NULL)
    {
        return;
    }
    /* The ink alpha is folded in here, once, at the last moment before the store: the
       coverage the rasteriser computed is a *shape* fact ("this pixel is 30 % inside
       the pen") and the alpha is an *ink* fact ("draw at 40 %"), and the pixel's
       opacity is their product. Doing it here rather than in every rasteriser is what
       makes "which primitives honour cgl_canvas_set_ink_alpha()" answer itself. */
    cov_q8 = canvas__ink(cov_q8, c->ink_alpha_q8);
    if (cov_q8 == 0u)
    {
        return;
    }
    row[sx] = (cov_q8 >= 256u) ? color : cgl_color_blend_q8(row[sx], color, cov_q8);
}

void cgl_canvas_pixel_blend_q8(cgl_canvas_t *c, int16_t x, int16_t y, cgl_color_t color,
                               uint16_t cov_q8)
{
    canvas__blend(c, x, y, color, cov_q8);
}

/**
 * The flat (no warp) store used by the scanline loops of `line_thick` and `arc`.
 *
 * WARNING: it writes blindly -- the caller has already resolved the row pointer and
 * checked the clip. `canvas__blend()` remains the general entry point (warp, src
 * clip, and everything a caller outside this file may need).
 *
 * It takes the canvas rather than a bare row pointer for one reason: the ink alpha
 * (see `cgl_canvas_set_ink_alpha()`) has to be applied to the flat path too, or "a
 * translucent arc" would come out opaque on an unwarped canvas and translucent on a
 * warped one -- the kind of difference that only shows up during a page transition.
 */
CGL_AINLINE void canvas__store(cgl_canvas_t *c, cgl_color_t *row, int16_t sx, cgl_color_t color,
                               uint16_t cov_q8)
{
    cov_q8 = canvas__ink(cov_q8, c->ink_alpha_q8);
    if (cov_q8 == 0u)
    {
        return;
    }
    row[sx] = (cov_q8 >= 256u) ? color : cgl_color_blend_q8(row[sx], color, cov_q8);
}

/**
 * Filled antialiased disc with a **sub-pixel centre** (everything in Q6).
 *
 * It is what the arc draws its two round caps with, and it is also the whole of "a
 * stroke of width 2r on a radius of 0" -- which is how a caller gets an
 * antialiased dot out of cgl_canvas_arc(). The sub-pixel centre is not decoration:
 * a pointer that sweeps a fraction of a pixel per frame has to move its cap by that
 * fraction, or the cap steps from whole pixel to whole pixel and the stroke looks
 * like it is vibrating.
 */
static void canvas__disc_aa(cgl_canvas_t *c, int32_t cx_q6, int32_t cy_q6, uint32_t r_q6,
                            cgl_color_t color)
{
    uint32_t r2_q12 = r_q6 * r_q6;
    int32_t cx_px = cx_q6 / 64; /* C truncation: for a negative centre this floors, which
                                   only makes the scan one pixel wider -- harmless */
    int32_t cy_px = cy_q6 / 64;
    /* The sub-pixel offsets are measured from the pixel's **centre**, and the `- 32` is
       what puts it there: a pixel column x covers [x*64, x*64 + 63] in Q6, so its centre
       is at x*64 + 32, which the inner loop's `(x - cx_px) * 64` does not know about.
       WARNING: this is not a fudge. Without it the distance is measured from the pixel's
       top-left *corner*, i.e. the whole disc is drawn half a pixel up and to the left of
       where it was asked for. On a full circle that is a constant offset nobody would
       notice, but this function draws the two **round caps** of `cgl_canvas_arc()` -- and
       a cap is a small blob standing on a circle, so a fixed offset in screen space
       trades between "further out along the radius" and "further in" as the cap goes
       round. Measured on a 60 px arc with a 3 px pen, sweeping the end through a full
       turn: the outermost inked pixel reached 4034 Q6 at some angles against 3937 at
       others, with an ideal outer edge of 3936 -- up to a pixel and a half of overshoot
       that came and went with the angle, which on the panel reads as the head running out
       past the end of the arc and snapping back. Centring the measurement pins the outer
       reach at 3982..4006 for every angle. */
    int32_t dx_c = cx_q6 - (cx_px * 64) - 32;
    int32_t dy_c = cy_q6 - (cy_px * 64) - 32;
    int32_t half = (int32_t)(r_q6 / 64u) + 1;
    int32_t dy_lo;
    int32_t dy_hi;
    int32_t x_lo;
    int32_t x_hi;
    int32_t dy;

    /* Bind the walk to this band before anything else: a widget is drawn once per
       band, so a disc that iterates its whole box in every band pays 43 times for one
       picture, and with a warp installed (where the box is mapped per pixel) that is
       the difference between a slide and a slideshow -- see cgl_canvas_src_window(). */
    {
        cgl_rect_t win = canvas__src_window(c);

        if (cgl_rect_is_empty(win))
        {
            return;
        }
        x_lo = (int32_t)win.x0 - (int32_t)c->pan.x;
        x_hi = (int32_t)win.x1 - (int32_t)c->pan.x;
        dy_lo = ((int32_t)win.y0 - (int32_t)c->pan.y) - cy_px;
        dy_hi = ((int32_t)win.y1 - (int32_t)c->pan.y) - cy_px;
        if (dy_lo < -half)
        {
            dy_lo = -half;
        }
        if (dy_hi > half)
        {
            dy_hi = half;
        }
        if (dy_lo > dy_hi)
        {
            return;
        }
    }

    for (dy = dy_lo; dy <= dy_hi; dy++)
    {
        int32_t dy_q6 = (dy * 64) + dy_c;
        uint32_t dy2_q12 = (uint32_t)(dy_q6 * dy_q6);
        int32_t span;
        int32_t xs;
        int32_t xe;
        int32_t x;

        if (dy2_q12 >= r2_q12)
        {
            continue;
        }
        span = (int32_t)cgl_math_isqrt(r2_q12 - dy2_q12);
        xs = cx_px + ((dx_c - span) / 64) - 1;
        xe = cx_px + ((dx_c + span) / 64) + 1;
        if (xs < x_lo)
        {
            xs = x_lo;
        }
        if (xe > x_hi)
        {
            xe = x_hi;
        }

        for (x = xs; x <= xe; x++)
        {
            int32_t dx_q6 = ((x - cx_px) * 64) - dx_c;
            uint32_t d2_q12 = (uint32_t)(dx_q6 * dx_q6) + dy2_q12;
            uint16_t cov = canvas__cov(d2_q12, r2_q12, r_q6);

            if (cov != 0u)
            {
                canvas__blend(c, (int16_t)x, (int16_t)(cy_px + dy), color, cov);
            }
        }
    }
}

/**
 * Coverage, Q8, of a pixel by the sector made of `parts` half planes (see
 * cgl_canvas_arc).
 *
 * Why this is a coverage and not a yes/no test, and what it fixes
 * --------------------------------------------------------------
 * It used to answer `>= 0` / `<= 0` -- "is the pixel **centre** inside the angular
 * wedge" -- which makes both of an arc's ends a hard, whole-pixel staircase while
 * the two round caps are placed at the **analytic** angle. The body's end therefore
 * snapped a pixel at a time while the cap moved a fraction of a pixel at a time, and
 * the joint between them opened and closed as the sweep grew: on the reference panel
 * that reads as a head that pops out past the end of the arc and back, worst around
 * 45 and 300 degrees, where neither `dx` nor `dy` is near zero and a one-pixel
 * quantisation of the cut is largest relative to the edge's slope.
 *
 * The ramp is the same one pixel box filter the radial edge already uses, and it
 * costs no division. `c0 = ax*dy - ay*dx` is 256 x the signed perpendicular distance
 * from the pixel centre to the boundary **in pixels** (the direction vectors are
 * Q8, i.e. 256 long), so the coverage of a half plane whose inside is `c0 >= 0` is
 * exactly `c0 + 128`, clamped: 256 when the pixel is half a pixel or more inside, 0
 * when it is half a pixel outside, linear in between. The closing half plane is the
 * same with the sign flipped (`-c1 + 128`, since its inside is `c1 <= 0`).
 *
 * A sweep longer than half a turn is split into several parts, and the parts are a
 * **union**: a pixel between two of them is inside at least one, so the answer is the
 * largest coverage any part gives it -- taking the minimum would carve a visible seam
 * across the arc every 180 degrees.
 */
CGL_AINLINE uint16_t canvas__sector_cov(const int16_t *ax, const int16_t *ay, const int16_t *bx,
                                        const int16_t *by, uint8_t parts, int32_t dx, int32_t dy)
{
    uint16_t best = 0u;
    uint8_t i;

    for (i = 0u; i < parts; i++)
    {
        int32_t c0 = ((int32_t)ax[i] * dy) - ((int32_t)ay[i] * dx);
        int32_t c1 = ((int32_t)bx[i] * dy) - ((int32_t)by[i] * dx);
        int32_t cov;

        if ((c0 <= -128) || (c1 >= 128))
        {
            continue; /* outside this part's ramp: no coverage to contribute */
        }
        cov = c0 + 128;
        if (cov > 256)
        {
            cov = 256;
        }
        c1 = 128 - c1;
        if (c1 < cov)
        {
            cov = c1;
        }
        if (cov > (int32_t)best)
        {
            best = (uint16_t)cov;
        }
    }
    return best;
}

void cgl_canvas_line_thick(cgl_canvas_t *c, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                          int16_t thickness, cgl_color_t color)
{
    int32_t dx = (int32_t)x1 - (int32_t)x0;
    int32_t dy = (int32_t)y1 - (int32_t)y0;
    int32_t adx = (dx < 0) ? -dx : dx;
    int32_t ady = (dy < 0) ? -dy : dy;
    bool major_x = (adx >= ady);
    int32_t major_delta = major_x ? dx : dy;
    int32_t minor_delta = major_x ? dy : dx;
    int32_t major0 = major_x ? x0 : y0;
    int32_t minor0 = major_x ? y0 : x0;
    uint32_t pen_q6;
    uint32_t pen2_q12;
    int32_t pen_px;
    uint32_t ad_major;
    int32_t len2;
    uint32_t hyp = cgl_math_isqrt(((uint32_t)(adx * adx)) + ((uint32_t)(ady * ady)));
    uint32_t spread_q8; /* how much wider than the pen a row of a slanted stroke gets */
    uint32_t inv_hyp_q16;
    int32_t m;
    int32_t m_lo;
    int32_t m_hi;
    cgl_rect_t win;

    if (thickness <= 1)
    {
        /* The 1 px case is the plain Bresenham walk: crisp, cheapest, and pixel for
           pixel identical to cgl_canvas_line(). An antialiased "hairline" is a
           *thinner* hairline, not a better one. */
        cgl_canvas_line(c, x0, y0, x1, y1, color);
        return;
    }

    /* The band, resolved once: it is both the cheap rejection below and the clamp of
       the walk further down. With a warp it is the pre-image of the band rather than
       the band itself (see cgl_canvas_src_window), which is what keeps a suction
       transition from re-walking every stroke in all 43 bands. */
    win = canvas__src_window(c);
    if (cgl_rect_is_empty(win))
    {
        return;
    }

    /* Cheap rejection first, **before** the square root and the setup below: a widget
       is drawn once per band, and a stroke that lies entirely outside this band can
       answer that with four comparisons. The reference dial draws 60 ticks and 9 hand
       segments per band; paying the isqrt and the division for each of them in all 43
       bands cost more than the ink they put down. */
    {
        int32_t pad = (int32_t)(thickness / 2) + 2;
        int32_t y_lo = ((y0 < y1) ? y0 : y1) - pad;
        int32_t y_hi = ((y0 < y1) ? y1 : y0) + pad;
        int32_t b0 = (int32_t)win.y0 - (int32_t)c->pan.y;
        int32_t b1 = (int32_t)win.y1 - (int32_t)c->pan.y;

        if ((y_hi < b0) || (y_lo > b1))
        {
            return;
        }
    }

    pen_q6 = (uint32_t)thickness * 32u; /* thickness / 2, in Q6 */
    pen2_q12 = pen_q6 * pen_q6;
    pen_px = (int32_t)(thickness / 2) + 1;
    ad_major = (uint32_t)((major_delta < 0) ? -major_delta : major_delta);
    len2 = (major_delta * major_delta) + (minor_delta * minor_delta);

    if (ad_major == 0u)
    {
        spread_q8 = 256u;
    }
    else
    {
        /* With the major axis chosen as the longer one, |major| >= hyp / sqrt(2), so
           this is at most 362 (1.41 px) and never needs a clamp. */
        spread_q8 = (hyp << 8) / ad_major;
        if (spread_q8 < 256u)
        {
            spread_q8 = 256u;
        }
    }

    /* `1 / hypot` in Q16: a pixel's perpendicular distance to the centre line is
       `|cross| / hypot`, and multiplying by this instead of dividing costs one
       multiply per pixel and cannot overflow (the hypot cancels in the product). */
    inv_hyp_q16 = (hyp > 0u) ? (65536u / hyp) : 65536u;

    m_lo = (major0 < (major0 + major_delta)) ? major0 : (major0 + major_delta);
    m_hi = (major0 < (major0 + major_delta)) ? (major0 + major_delta) : major0;
    /* The round caps stick out by up to `pen_px` past each end along the stroke. */
    m_lo -= pen_px;
    m_hi += pen_px;

    /* A stroke whose major axis is y can be clamped to the band **in one step**,
       which turns "walk 60 steps and reject each one" into a two-comparison clamp.
       For an x-major stroke the band limits the minor range instead, so the loop has
       to stay (its steps are cheap: the per-step band test below skips the pixels).
       WARNING: while a warp is installed `win` is the band's *pre-image*, not the
       band, so the clamp is wider -- but it is still a bound, and without one the
       stroke would re-walk its whole length in every band. */
    if (!major_x)
    {
        int32_t b0 = (int32_t)win.y0 - (int32_t)c->pan.y;
        int32_t b1 = (int32_t)win.y1 - (int32_t)c->pan.y;

        if (m_lo < b0)
        {
            m_lo = b0;
        }
        if (m_hi > b1)
        {
            m_hi = b1;
        }
        if (m_lo > m_hi)
        {
            return;
        }
    }

    for (m = m_lo; m <= m_hi; m++)
    {
        /* Closest point on the segment, in the (major, minor) frame: clamp the major
           coordinate into the segment, then walk the minor one by the slope.
           WARNING: `pm_q6` is kept in **Q6**. An integer minor coordinate -- the first
           version of this -- quantises the stroke's centre line to whole pixels, and
           the coverage then measures the distance to that quantised centre: a
           diagonal 5 px stroke came out with a 240/256 film over its whole core
           because its centre line was half a pixel from where the coverage function
           thought it was. */
        int32_t mc = m;
        int32_t pm_q6;
        int32_t pm_px;
        int32_t span_px = (int32_t)((((uint32_t)pen_px * spread_q8) + 128u) >> 8) + 1;
        int32_t n;

        if (mc < ((major_delta < 0) ? (major0 + major_delta) : major0))
        {
            mc = (major_delta < 0) ? (major0 + major_delta) : major0;
        }
        if (mc > ((major_delta < 0) ? major0 : (major0 + major_delta)))
        {
            mc = (major_delta < 0) ? major0 : (major0 + major_delta);
        }
        pm_q6 = (major_delta == 0)
                    ? (minor0 << 6)
                    : ((minor0 << 6) + (int32_t)((((mc - major0) * minor_delta) << 6) / major_delta));
        pm_px = pm_q6 / 64;

        /* Bind this step to the current band. A widget is drawn **once per band**, so
           a stroke that walks its whole length in every band pays 43 times for one
           picture: measured on the reference build, the three hands re-walked ~60
           major steps in each of ~20 bands, and that alone was worth several
           milliseconds of the frame.
           WARNING: while a warp is installed `win` is the band's *pre-image* (a
           source row far outside the band can still land inside it -- see
           cgl_canvas_src_window), so the test is against a wider range, but it is
           still a bound: the alternative is re-walking the whole stroke per band. */
        {
            int32_t r0 = major_x ? (pm_px - span_px) : m;
            int32_t r1 = major_x ? (pm_px + span_px) : m;
            int32_t b0 = (int32_t)win.y0 - (int32_t)c->pan.y; /* sources are screen space */
            int32_t b1 = (int32_t)win.y1 - (int32_t)c->pan.y;

            if ((r1 < b0) || (r0 > b1))
            {
                continue;
            }
        }

        for (n = pm_px - span_px; n <= (pm_px + span_px + 1); n++)
        {
            /* The exact distance from the pixel to the **segment**, in the same
               (major, minor) frame the loop walks -- a transpose, so distances are
               preserved. Three cases, and getting any of them subtly wrong is visible:
                 - before the start / after the end: the nearest point is the endpoint,
                   a plain Euclidean distance (this is the round cap);
                 - in between: the perpendicular distance to the line, which is
                   |cross| / hypot. Using the *minor* offset directly instead is a
                   factor cos(angle) too large: a shallow 4 px stroke read 1.78 px
                   where the truth was 1.40, so its core carried a 179/256 film;
                   using the major coordinate alone to decide "am I past the end"
                   (the first fix) draws half a pixel of cap too far, because a pixel
                   whose major coordinate lands exactly on the endpoint is inside the
                   major range but outside the segment. */
            int32_t dmaj = m - major0;
            int32_t dmin = n - minor0;
            int32_t dot = (dmaj * major_delta) + (dmin * minor_delta);
            uint32_t d2_q12;
            uint16_t cov;

            if (dot <= 0)
            {
                d2_q12 = (uint32_t)(((dmaj * dmaj) + (dmin * dmin)) << 12);
            }
            else if (dot >= len2)
            {
                int32_t e1 = dmaj - major_delta;
                int32_t e2 = dmin - minor_delta;

                d2_q12 = (uint32_t)(((e1 * e1) + (e2 * e2)) << 12);
            }
            else
            {
                /* Interior: the perpendicular distance to the centre line is
                   `|cross| / hypot`, where `cross` is the 2-D cross product of the
                   pixel offset with the segment direction.
                   WARNING: the offset must be measured **from the centre line**, not
                   from the segment's start. A "scaled pen along the minor axis"
                   shortcut (comparing `|minor - minor0|` against a widened pen) looks
                   equivalent, passes a capsule-containment test, and draws the wrong
                   picture: every row tests the same x, so the stroke comes out as a
                   band near its first point with the rest of its length missing --
                   the watch hands rendered as three disconnected fragments.
                   The divide by hypot is turned into a multiply by a reciprocal taken
                   once per call; the `hyp` cancels, so nothing can overflow. */
                int32_t cross = (dmaj * minor_delta) - (dmin * major_delta);
                int32_t mag = (cross < 0) ? -cross : cross;
                /* The shift is 16 - 6: `mag * 65536 / hyp >> 16` is the distance in
                   whole pixels, and the coverage function works in Q6. */
                int32_t dperp_q6 = (int32_t)(((uint32_t)mag * inv_hyp_q16) >> 10);

                cov = canvas__cov((uint32_t)(dperp_q6 * dperp_q6), pen2_q12, pen_q6);
                if (cov == 0u)
                {
                    continue;
                }
                canvas__blend(c, (int16_t)(major_x ? m : n), (int16_t)(major_x ? n : m), color,
                              cov);
                continue;
            }

            cov = canvas__cov(d2_q12, pen2_q12, pen_q6);
            if (cov == 0u)
            {
                continue;
            }
            canvas__blend(c, (int16_t)(major_x ? m : n), (int16_t)(major_x ? n : m), color, cov);
        }
    }
}

void cgl_canvas_arc(cgl_canvas_t *c, int16_t cx, int16_t cy, int16_t radius, int16_t thickness,
                    cgl_angle_t start, uint32_t sweep, cgl_color_t color)
{
    uint32_t pen_q6;
    int32_t r_out_q6;
    int32_t r_in_q6;
    uint32_t r2_out_q12;
    uint32_t r2_in_q12;
    int16_t ax[4];
    int16_t ay[4];
    int16_t bx[4];
    int16_t by[4];
    uint8_t parts = 0u;
    int32_t r_out_px;
    int32_t dy;

    if (sweep == 0u)
    {
        return;
    }
    if (thickness < 1)
    {
        thickness = 1;
    }

    pen_q6 = (uint32_t)thickness * 32u; /* thickness / 2, in Q6 */
    r_out_q6 = ((int32_t)radius * 64) + (int32_t)pen_q6;
    r_in_q6 = ((int32_t)radius * 64) - (int32_t)pen_q6;
    if (r_out_q6 < 32)
    {
        /* Smaller than half a pixel: draw a dot, so "a gauge at radius 0" is the
           antialiased dot cgl_canvas_arc() promises rather than nothing at all. */
        canvas__disc_aa(c, (int32_t)cx * 64, (int32_t)cy * 64, pen_q6, color);
        return;
    }
    if (r_in_q6 < 0)
    {
        r_in_q6 = 0;
    }
    r2_out_q12 = (uint32_t)(r_out_q6 * r_out_q6);
    r2_in_q12 = (uint32_t)(r_in_q6 * r_in_q6);
    r_out_px = (r_out_q6 / 64) + 1;

    /* The angular clip, as at most four half-plane intersections. A full turn needs
       none at all, which is the common case (a ring's track). */
    if (sweep < (uint32_t)CGL_ANGLE_TURN)
    {
        uint32_t rem = sweep;
        cgl_angle_t a = start;

        while ((rem > 0u) && (parts < 4u))
        {
            uint32_t len = (rem > (uint32_t)CGL_ANGLE_HALF) ? (uint32_t)CGL_ANGLE_HALF : rem;
            cgl_angle_t a1 = (cgl_angle_t)(a + (cgl_angle_t)len);

            ax[parts] = cgl_cos_q8(a);
            ay[parts] = cgl_sin_q8(a);
            bx[parts] = cgl_cos_q8(a1);
            by[parts] = cgl_sin_q8(a1);
            parts++;
            a = a1;
            rem -= len;
        }
    }

    /* Rows are walked in **pairs** (one above the centre, one below) with a
       two-pointer bound instead of a square root per row.

       WARNING: this is the single most important cost decision in the file, and it is
       made twice over.

       (1) The half-width of a row is `sqrt(r^2 - dy^2)`. Taking that with
       `cgl_math_isqrt()` twice per row (outer and inner radius) costs more than every
       pixel of the ring put together, so the bounds are found by advancing a cursor
       through `x^2 + dy^2 <= r^2` instead -- roughly ten cycles per row and no root.
       The bounds only limit the scan; the coverage maths still uses the exact Q6
       radii, so the drawn shape is bit for bit what it was.

       (2) **The walk is clamped to the current band.** A widget is drawn once per
       band, so a shape that iterates all of its rows in every band pays 43 times for
       one picture: on the reference build that alone was ~17 ms of a frame, more than
       the whole SPI budget, and it is what made the watch face stutter. The walk
       therefore starts at the row nearest the clip window and stops at the far edge,
       which leaves ~4 rows per band instead of 160. The two cursors are seeded with
       one isqrt each at the starting row, which is cheaper than the 40 rows of
       iteration it saves.
       WARNING: the window used is `cgl_canvas_src_window()`, **not** the clip window:
       with a warp installed the clip is in *destination* space and a source row far
       outside the band can still land inside it, so clamping to the band itself would
       lose pixels. The pre-image of the band is still a bounded range, and using it
       is what keeps a suction transition of an arc-heavy page from re-walking every
       ring in all 43 bands (measured on the watch face: 18.7 M host cycles a frame
       unbounded against 1.8 M flat). */
    {
        int32_t ro_px = r_out_px;
        int32_t ri_px = (r_in_q6 > 0) ? ((r_in_q6 / 64) - 1) : -1; /* -1: the hole may
                                                                      reach a pixel in */
        int32_t ro2 = ro_px * ro_px;
        int32_t ri2 = (ri_px > 0) ? (ri_px * ri_px) : 0;
        int32_t xo_i = 0;
        int32_t xi_i = 0;
        int32_t k_lo;
        int32_t k_hi;
        int32_t k;

        /* Bind the walk to the band (the *pre-image* of it while warped): see the
           WARNING above. Two comparisons per band instead of a full pass over the
           ring's rows in every one of the 43 bands. */
        {
            cgl_rect_t win = canvas__src_window(c);
            int32_t b0;
            int32_t b1;
            int32_t d0;
            int32_t d1;
            int32_t a0;
            int32_t a1;

            if (cgl_rect_is_empty(win))
            {
                return;
            }
            b0 = (int32_t)win.y0 - (int32_t)c->pan.y; /* the window is screen space */
            b1 = (int32_t)win.y1 - (int32_t)c->pan.y;
            d0 = b0 - (int32_t)cy;
            d1 = b1 - (int32_t)cy;
            a0 = (d0 < 0) ? -d0 : d0;
            a1 = (d1 < 0) ? -d1 : d1;

            k_lo = ((b0 > (int32_t)cy) || (b1 < (int32_t)cy))
                       ? ((a0 < a1) ? a0 : a1) /* the window does not straddle the centre */
                       : 0;
            k_hi = (a0 > a1) ? a0 : a1;
            if (k_hi > ro_px)
            {
                k_hi = ro_px;
            }
            if (k_lo > k_hi)
            {
                return;
            }
            if (k_hi <= 0)
            {
                /* Only the centre row: no root needed. */
                k_lo = 0;
                k_hi = 0;
            }
            else
            {
                /* Seed the two cursors at the far edge of the window: one root each is
                   cheaper than the rows of iteration it saves. */
                xo_i = (int32_t)cgl_math_isqrt((uint32_t)(ro2 - (k_hi * k_hi)));
                if (ri_px > 0)
                {
                    xi_i = (k_hi < ri_px)
                                ? (int32_t)cgl_math_isqrt((uint32_t)(ri2 - (k_hi * k_hi)))
                                : 0;
                }
            }
        }

        for (k = k_hi; k >= k_lo; k--)
        {
            int32_t k2 = k * k;

            while ((((xo_i + 1) * (xo_i + 1)) + k2) <= ro2)
            {
                xo_i++;
            }
            while ((ri_px > 0) && ((((xi_i + 1) * (xi_i + 1)) + k2) <= ri2))
            {
                xi_i++;
            }

            for (dy = (k == 0) ? 0 : -k; dy <= k; dy += (k == 0) ? 1 : (2 * k))
            {
                int32_t y = (int32_t)cy + dy;
                uint32_t dy2_q12 = (uint32_t)((dy * dy) << 12);
                int32_t xo = xo_i + 1;
                int32_t xi = xi_i;
                int32_t arm_x[2][2];
                uint8_t n_arms;
                uint8_t arm;

                if ((r_in_q6 > 0) && (xi > 0))
                {
                    arm_x[0][0] = cx - xo;
                    arm_x[0][1] = cx - xi + 1;
                    arm_x[1][0] = cx + xi - 1;
                    arm_x[1][1] = cx + xo;
                    n_arms = 2u;
                }
                else
                {
                    arm_x[0][0] = cx - xo;
                    arm_x[0][1] = cx + xo;
                    n_arms = 1u;
                }

        /* Resolve this row **once** when the canvas is flat (no warp): the pan, the y
           clip test and the destination row pointer are identical for every pixel of
           the row, and `canvas__blend()` re-derives all three per pixel because it
           also has to serve the warped case. Measured on the host, the difference is
           about 4x on the pixel cost of every arc and stroke -- which is what the
           whole watch face is made of. */
        {
            bool flat = (c->warp == NULL);
            cgl_color_t *row = NULL;
            int16_t panx = c->pan.x;
            int16_t cx0 = c->clip.x0;
            int16_t cx1 = c->clip.x1;

            if (flat)
            {
                int16_t sy = (int16_t)(y + c->pan.y);

                if ((sy < c->origin_y) || (sy >= (int16_t)(c->origin_y + c->height)) ||
                    (sy < c->clip.y0) || (sy > c->clip.y1))
                {
                    continue;
                }
                row = c->buf + ((uint32_t)(sy - c->origin_y) * (uint32_t)c->width);
            }

            for (arm = 0u; arm < n_arms; arm++)
            {
                int32_t x;
                int32_t xa = arm_x[arm][0];
                int32_t xb = arm_x[arm][1];

                if (xa > xb)
                {
                    continue;
                }
                for (x = xa; x <= xb; x++)
                {
                    int32_t dx = x - (int32_t)cx;
                    uint32_t d2_q12 = (uint32_t)((dx * dx) << 12) + dy2_q12;
                    uint16_t cov = canvas__cov(d2_q12, r2_out_q12, (uint32_t)r_out_q6);

                    if ((cov != 0u) && (r_in_q6 > 0))
                    {
                        /* WARNING: the inner edge is the **complement**: the outer test
                           answers "how much of this pixel is inside the outer disc", so
                           the hole needs "how much of it is outside the inner disc",
                           which is 256 minus that. Forgetting the inversion draws the
                           hole: a 1 px circle outline at radius 37 came out as a filled
                           disc with a rim (the geometry test caught it as "pixels
                           2.29 px off the radius"). */
                        uint16_t inner =
                            256u - canvas__cov(d2_q12, r2_in_q12, (uint32_t)r_in_q6);

                        if (inner < cov)
                        {
                            cov = inner;
                        }
                    }
                    if (cov == 0u)
                    {
                        continue;
                    }
                    if (parts > 0u)
                    {
                        /* The angular edge is a second, independent ramp, and the two
                           edges **multiply**: a pixel is covered by the arc only where it
                           is covered by both the annulus and the wedge. Taking the smaller
                           of the two would be the same answer with a visible seam at the
                           ends (a pixel half covered by each is a quarter covered, not a
                           half). */
                        uint16_t sec = canvas__sector_cov(ax, ay, bx, by, parts, dx, dy);

                        if (sec == 0u)
                        {
                            continue;
                        }
                        if (sec < 256u)
                        {
                            cov = (uint16_t)(((uint32_t)cov * (uint32_t)sec) >> 8);
                            if (cov == 0u)
                            {
                                continue;
                            }
                        }
                    }
                    if (flat)
                    {
                        int16_t sx = (int16_t)(x + panx);

                        if ((sx >= cx0) && (sx <= cx1))
                        {
                            canvas__store(c, row, sx, color, cov);
                        }
                    }
                    else
                    {
                        canvas__blend(c, (int16_t)x, y, color, cov);
                    }
                }
            }
            }
        }
    }

    /* The two round caps, placed in Q6 so that a sweep of a fraction of a pixel
       moves them by that fraction. */
    if (parts > 0u)
    {
        cgl_angle_t a_end = (cgl_angle_t)(start + (cgl_angle_t)sweep);

        canvas__disc_aa(c, ((int32_t)cx * 64) + (((int32_t)radius * (int32_t)cgl_cos_q8(start)) / 4),
                        ((int32_t)cy * 64) + (((int32_t)radius * (int32_t)cgl_sin_q8(start)) / 4),
                        pen_q6, color);
        canvas__disc_aa(c, ((int32_t)cx * 64) + (((int32_t)radius * (int32_t)cgl_cos_q8(a_end)) / 4),
                        ((int32_t)cy * 64) + (((int32_t)radius * (int32_t)cgl_sin_q8(a_end)) / 4),
                        pen_q6, color);
    }
    } /* the two-pointer bound block */
} /* cgl_canvas_arc */

void cgl_canvas_circle(cgl_canvas_t *c, int16_t cx, int16_t cy, int16_t radius, cgl_color_t color)
{
    if (radius <= 0)
    {
        canvas__blend(c, cx, cy, color, 256u);
        return;
    }
    /* A circle outline is exactly "a one pixel stroke on this radius": the annulus
       [radius - 0.5, radius + 0.5] that cgl_canvas_arc() already draws. */
    cgl_canvas_arc(c, cx, cy, radius, 1, 0u, CGL_ANGLE_TURN, color);
}

void cgl_canvas_blit(cgl_canvas_t *c, int16_t x, int16_t y,
                    const cgl_color_t *src, int16_t w, int16_t h)
{
    int16_t yy;

    if (c->warp != NULL)
    {
        /* The warp path must scan **by source** (column mapping depends on the
           row; recovering a source column from a destination row costs an extra
           division, and while compressing several source columns land on the
           same destination column anyway -- scatter writes are naturally point
           sampling). */
        for (yy = 0; yy < h; yy++)
        {
            int16_t py = canvas__ty(c, (int16_t)(y + yy));
            int16_t dy;

            if (!canvas__src_row_ok(c, py))
            {
                continue;
            }
            dy = canvas__wy(c, py);
            if ((dy < c->origin_y) || (dy >= (int16_t)(c->origin_y + c->height)) ||
                (dy < c->clip.y0) || (dy > c->clip.y1))
            {
                continue;
            }
            {
                cgl_color_t *dst = canvas__row(c, dy);
                int16_t xx;

                if (dst == NULL)
                {
                    continue;
                }
                for (xx = 0; xx < w; xx++)
                {
                    int16_t sxp = canvas__tx(c, (int16_t)(x + xx));
                    int16_t dx;

                    if (!canvas__src_ok(c, sxp, py))
                    {
                        continue;
                    }
                    dx = canvas__wx(c, sxp, py);
                    if ((dx >= c->clip.x0) && (dx <= c->clip.x1))
                    {
                        dst[dx] = src[((uint32_t)yy * (uint32_t)w) + (uint32_t)xx];
                    }
                }
            }
        }
        return;
    }

    {
        int16_t tx = canvas__tx(c, x);
        int16_t ty = canvas__ty(c, y);
        cgl_rect_t r = cgl_rect_wh(tx, ty, w, h);
        cgl_rect_t k = cgl_rect_intersect(r, c->clip);

        if (cgl_rect_is_empty(k))
        {
            return;
        }
        for (yy = k.y0; yy <= k.y1; yy++)
        {
            cgl_color_t *dst = canvas__row(c, yy);
            int16_t xx;
            if (dst == NULL)
            {
                continue;
            }
            for (xx = k.x0; xx <= k.x1; xx++)
            {
                /* The source image is an image in "primitive coordinates", so
                   the pan must be subtracted again to index a source pixel. */
                int16_t sx = (int16_t)(xx - c->pan.x);
                int16_t sy = (int16_t)(yy - c->pan.y);
                dst[xx] = src[((uint32_t)sy * (uint32_t)w) + (uint32_t)sx];
            }
        }
    }
}

void cgl_canvas_fill_rect_blend_q8(cgl_canvas_t *c, cgl_rect_t r,
                                  cgl_color_t color, uint16_t alpha_q8)
{
    cgl_rect_t s;
    cgl_rect_t k;
    int16_t yy;

    if (alpha_q8 == 0u)
    {
        return;
    }
    if (alpha_q8 >= CANVAS_ONE_Q8)
    {
        cgl_canvas_fill_rect(c, r, color);
        return;
    }

    if (c->warp != NULL)
    {
        int16_t sx0 = canvas__tx(c, r.x0);
        int16_t sx1 = canvas__tx(c, r.x1);

        canvas__src_clamp_x(c, &sx0, &sx1);
        if (sx0 > sx1)
        {
            return;
        }
        for (yy = r.y0; yy <= r.y1; yy++)
        {
            int16_t py = canvas__ty(c, yy);
            int16_t dy;
            cgl_color_t *row;
            int16_t xa;
            int16_t xb;
            int16_t xx;

            if (!canvas__src_row_ok(c, py))
            {
                continue;
            }
            dy = canvas__wy(c, py);
            if ((dy < c->origin_y) || (dy >= (int16_t)(c->origin_y + c->height)) ||
                (dy < c->clip.y0) || (dy > c->clip.y1))
            {
                continue;
            }
            row = canvas__row(c, dy);
            if (row == NULL)
            {
                continue;
            }
            xa = canvas__wx(c, sx0, py);
            xb = canvas__wx(c, sx1, py);
            if (xa < c->clip.x0)
            {
                xa = c->clip.x0;
            }
            if (xb > c->clip.x1)
            {
                xb = c->clip.x1;
            }
            for (xx = xa; xx <= xb; xx++)
            {
                row[xx] = cgl_color_blend_q8(row[xx], color, alpha_q8);
            }
        }
        return;
    }

    s = cgl_rect(canvas__tx(c, r.x0), canvas__ty(c, r.y0),
                canvas__tx(c, r.x1), canvas__ty(c, r.y1));
    k = cgl_rect_intersect(s, c->clip);

    if (cgl_rect_is_empty(k))
    {
        return;
    }

    for (yy = k.y0; yy <= k.y1; yy++)
    {
        cgl_color_t *row = canvas__row(c, yy);
        int16_t xx;
        if (row == NULL)
        {
            continue;
        }
        for (xx = k.x0; xx <= k.x1; xx++)
        {
            row[xx] = cgl_color_blend_q8(row[xx], color, alpha_q8);
        }
    }
}

void cgl_canvas_fill_rect_blend(cgl_canvas_t *c, cgl_rect_t r, cgl_color_t color)
{
    cgl_canvas_fill_rect_blend_q8(c, r, color, 128u);
}

/* -------------------------------------------------------------------------- */
/* Group opacity: layered buffers + two auxiliary primitives                    */
/* -------------------------------------------------------------------------- */

void cgl_canvas_set_alpha_scratch(cgl_canvas_t *c, cgl_color_t *scratch,
                                 uint8_t levels, uint32_t level_stride_px)
{
    if (c == NULL)
    {
        return;
    }
    c->alpha_scratch = scratch;
    c->alpha_levels = (scratch != NULL) ? levels : 0u;
    c->alpha_stride = (scratch != NULL) ? level_stride_px : 0u;
    c->alpha_used = 0u;
}

uint8_t cgl_canvas_alpha_room(const cgl_canvas_t *c)
{
    if ((c == NULL) || (c->alpha_scratch == NULL))
    {
        return 0u;
    }
    return (uint8_t)(c->alpha_levels - c->alpha_used);
}

/**
 * First step of group rendering: copy "how this level looks right now" (= the
 * background) into scratch.
 *
 * Why it must be a copy rather than a clear: semi-transparent blending between
 * child widgets must blend **over the background**; clearing would amount to
 * replacing the background with black, and a 50% child would become 50% black.
 * After the copy, the pixels no child drew on are still equal to the background
 * inside scratch, and the final blend step is the identity transform for them --
 * so **no coverage mask is needed**.
 */
void cgl_canvas_copy_rect(const cgl_canvas_t *c, cgl_color_t *dst, cgl_rect_t r)
{
    cgl_rect_t k;
    int16_t y;

    if ((c == NULL) || (dst == NULL))
    {
        return;
    }
    k = cgl_rect_intersect(r, c->clip);
    if (cgl_rect_is_empty(k))
    {
        return;
    }
    for (y = k.y0; y <= k.y1; y++)
    {
        const cgl_color_t *src_row = canvas__row(c, y);
        cgl_color_t *dst_row;
        int16_t x;

        if (src_row == NULL)
        {
            continue;
        }
        dst_row = dst + ((uint32_t)(y - c->origin_y) * (uint32_t)c->width);
        for (x = k.x0; x <= k.x1; x++)
        {
            dst_row[x] = src_row[x];
        }
    }
}

/**
 * Last step of group rendering: `dst = dst*(256-a) + src*a`, per pixel.
 *
 * Mathematically identical to `cgl_canvas_fill_rect_blend_q8()`, except that
 * the source goes from "one constant colour" to "a whole block of pixels" -- so
 * both share cgl_color_blend_q8() and cannot diverge.
 */
void cgl_canvas_blend_src_q8(cgl_canvas_t *c, cgl_rect_t r,
                            const cgl_color_t *src, uint16_t alpha_q8)
{
    cgl_rect_t k;
    int16_t y;

    if ((c == NULL) || (src == NULL) || (alpha_q8 == 0u))
    {
        return;
    }
    /* The endpoints take the general path too: at alpha = 256, inv = 0 and the
       result is exactly src. */
    if (alpha_q8 > (uint16_t)CANVAS_ONE_Q8)
    {
        alpha_q8 = (uint16_t)CANVAS_ONE_Q8;
    }

    k = cgl_rect_intersect(r, c->clip);
    if (cgl_rect_is_empty(k))
    {
        return;
    }
    for (y = k.y0; y <= k.y1; y++)
    {
        cgl_color_t *row = canvas__row(c, y);
        const cgl_color_t *src_row;
        int16_t x;

        if (row == NULL)
        {
            continue;
        }
        src_row = src + ((uint32_t)(y - c->origin_y) * (uint32_t)c->width);
        for (x = k.x0; x <= k.x1; x++)
        {
            row[x] = cgl_color_blend_q8(row[x], src_row[x], alpha_q8);
        }
    }
}



