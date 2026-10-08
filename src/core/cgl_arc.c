// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_arc.c
  * @brief   LCD UI framework - the radial toolkit: a shaded circular surface, a
  *          tapered antialiased blade, and the ring-gauge widget.
  *
  *  What this file is made of
  *  -------------------------
  *  Three independent things that happen to share a subject (a point at a radius
  *  around a centre), in the order the header declares them:
  *
  *    1. **a coverage ramp** (one table, 128 bytes of flash) -- the one opinion
  *       about what "half covered" means, shared by every antialiased edge this
  *       module draws;
  *    2. **a blade** (`cgl_arc_stroke_*`, `cgl_arc_needle*`) -- a tapered quad
  *       rasterised per pixel from two dot products, plus the damage box a caller
  *       needs to erase the previous frame's blade;
  *    3. **a surface** (`cgl_arc_surface_*`) -- "one colour per pixel out of a
  *       table", with a procedural path for callers who will not spend the flash,
  *       and the two painters that walk a band row by row;
  *    4. **a gauge** (`cgl_arc_gauge_*`) -- a `cgl_widget_t` that draws a value as
  *       a sweep, the radial twin of `cgl_bar`.
  *
  *  The pixel maths below is the demo page's, copied and not re-derived
  *  ------------------------------------------------------------------
  *  Every expression in `arc__face_px`, `arc__ring_px`, `arc__face_gradient`,
  *  `arc__bake_*`, `arc__angle_code` and `arc__hash` is the one
  *  `examples/common/App/app_page_clock.c` (with its dial header) has drawn the
  *  reference dial with since the dial was written, term for term. That is
  *  deliberate and it is the module's acceptance criterion: this file is a
  *  *move*, not a rewrite, so that the pixels of the watch face do not change
  *  when the face moves out of the demo and into the framework. Anything that
  *  reads like an odd way to compute something (`small/large` as an angle, the
  *  Q16 LUT scale, the `>> 8` truncations, `isqrt(d2 << 16)`) is odd in the demo
  *  too, and is documented at the generator that owns it.
  *
  *  Two consequences worth stating, because both are easy to "fix" by accident:
  *
  *    - **The generator and the pixel function must be one opinion.** The face
  *      table is uniform in `r^2` while its colours are functions of `r`, so the
  *      bake evaluates the gradient at the *real* radius each entry stands for
  *      (`sqrt(i/(n-1))`) and the pixel path evaluates the procedural version at
  *      the pixel's own radius. Both call `arc__face_gradient()`, so an entry and
  *      the pixel it describes cannot disagree -- and a caller who uses a baked
  *      table and one who leaves `lut == NULL` see the same surface.
  *    - **The coverage ramp is the demo's table, not `canvas__cov()`.** The
  *      framework's own strokes derive coverage from squared distances with a
  *      division per boundary pixel; this module's blade uses a 128 entry ramp so
  *      that antialiasing a hand costs neither a root nor a multiply (see
  *      `cgl_arc_cov_q6`). The two ramps agree in character -- that is what the
  *      header promises -- and rebuilding one from the other is exactly how a
  *      page's pixels change without anybody editing the page.
  *
  *  The blade's cost, and the clamp that is not an optimisation
  *  -----------------------------------------------------------
  *  A widget is drawn **once per band** (4 rows of a 320 x 172 panel, so 43
  *  times for one picture). A rasteriser that walks its own bounding box in every
  *  band therefore pays 43 times for the same shape, and a hand's box is up to
  *  150 x 150 px: measured on the reference board that walk alone was about two
  *  thirds of a frame that pushes every band, which is what made the watch page
  *  stutter during a transition. The bound is `cgl_canvas_src_window()` -- the
  *  band while the canvas is flat, the band's **pre-image** while a warp is
  *  installed -- and with a warp that is the only bound that is both correct and
  *  finite. See `cgl_arc_stroke_draw()`: removing that clamp is not a
  *  micro-optimisation, it is the difference between four frames and forty for a
  *  suction transition.
  *
  *  A surface is *not* allowed to do that
  *  -------------------------------------
  *  Every pixel of a surface is a different colour, so the per-pixel blend the
  *  scanline primitives answer a warp with would be ~900 000 blended calls per
  *  frame for a face being sucked into a corner. `cgl_arc_surface_paint()`
  *  therefore refuses a warp outright and paints the surface's **edge colour**
  *  as a flat disc (FACE) or annulus (RING) instead -- a gradient nobody can
  *  see is not worth a frame rate (the header carries the full reasoning).
  *
  *  The gauge, and one genuine gap in `cgl_arc_gauge_t`
  *  ---------------------------------------------------
  *  `cgl_arc_gauge_set_sweep()` is documented as "the owner's door": it sets the
  *  sweep in angle units and bypasses the value -> fraction mapping **until the
  *  next `set_value()`**. That "until" is state, and `cgl_arc_gauge_t` has no
  *  field to keep it in: `value`/`shown`/`vmin`/`vmax` are the value machinery
  *  (a sentinel written into one of them would corrupt `get_value()` and the next
  *  `animate_to()`), and `anim`/`pulse` are the tween objects (`cgl_anim_t`'s
  *  spare flag bits and `from == to == -1` style markers are internals this file
  *  has no business writing). The honest fix is one field in the header -- an
  *  explicit sweep of `uint32_t` plus a one byte "this is in force" flag, at the
  *  end of `cgl_arc_gauge_t`, where the two cost no alignment padding on the
  *  reference build:
  *
  *      uint32_t direct_sweep;
  *      uint8_t  direct;
  *
  *  Headers are contracts and this file was asked not to touch them, so the
  *  state lives in a small file-scope table instead (`ARC__DIRECT_SLOTS` entries,
  *  keyed by the gauge's address, cleared by `cgl_arc_gauge_init()` so a recycled
  *  stack address cannot inherit a dead gauge's sweep). For the use the header
  *  describes -- **one** owner-driven gauge, a watch face's complication ring --
  *  the result is identical to the field; with more owner-driven gauges than
  *  slots, the oldest entry is recycled and that gauge falls back to the value
  *  mapping. The field above removes the table and the caveat in one line, and it
  *  is the change this module wants.
  *
  *  Constraints, as everywhere in the framework: C11, no `malloc`, no VLA, no
  *  libm, no floating point and no 64 bit arithmetic (the last would drag
  *  `__aeabi_ldivmod` into the firmware -- see the note at the top of cgl_anim.h).
  *  Nothing here is stored between frames except the gauge's own widget state.
  ******************************************************************************
  */
#include <cytgl/cgl_arc.h>
/* cgl_math_frac_q8() / cgl_math_isqrt() / cgl_math_polar() arrive through
   cgl_arc.h -> cgl_widget.h -> cgl_canvas.h -> cgl_math.h, but this file names
   them directly, so it says so directly (the same rule cgl_bar.c follows). */
#include <cytgl/cgl_math.h>

/* The whole module is behind its switch, exactly as cgl_watch.c is.
 *
 * WARNING: without this the file does not compile at `-DCGL_CFG_ARC=0` at all --
 * `cgl_arc.h` expands to nothing, so `CGL_ARC_COV_N` and every type below are
 * undeclared. A compile-time switch that only works when the module is *on* is not a
 * switch, and the check that would have caught it is `gcc -DCGL_CFG_ARC=0
 * -DCGL_CFG_WATCH=0 -c src/core/cgl_arc.c`.
 *
 * The guard is here rather than around the includes because the two headers are what
 * say `#if CGL_CFG_ARC` themselves; this file only has to agree with them. */
#if CGL_CFG_ARC

/* ========================================================================== */
/* Coverage: the one opinion about what "half covered" means                  */
/* ========================================================================== */

/**
 * The ramp, Q7 -- stored halved so an entry fits a byte, restored to Q8 by
 * `cgl_arc_cov_q6()`.
 *
 * Index `k` is the pixel's signed distance **inside** the shape in 1/64 px,
 * offset by +64: k = 0 is a full pixel inside (solid), k = 32 is half a pixel
 * inside, k = 64 is exactly on the edge (half covered), k = 96 is half a pixel
 * outside and k >= 128 is invisible. The ramp is one pixel wide, which is a one
 * pixel box filter -- the definition of "antialiased" the whole framework uses.
 *
 * WARNING: this is the demo page's table, byte for byte (`s_cov_q7` in
 * app_page_clock.c, where the same 128 bytes also round the complication card's
 * corners), and it is *not* regenerated from `canvas__cov()`'s continuous ramp in
 * cgl_canvas.c. A hand drawn here and a ring drawn by `cgl_canvas_arc()` have to
 * read as the same edge softness, and they do -- 256 coverage levels over 64
 * entries, indexed by the same signed distance -- but the two implementations
 * answer with slightly different numbers on the ramp, and re-deriving one from
 * the other would silently change every antialiased hand on the reference panel.
 */
static const uint8_t s_cov_q7[CGL_ARC_COV_N] = {
    /* k  0.. 15: 1 px inside and deeper -- solid. */
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    /* k 16.. 31 */
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    /* k 32.. 47: the ramp's solid end (k = 32 is half a pixel inside). */
    128, 126, 124, 122, 120, 118, 116, 114, 112, 110, 108, 106, 104, 102, 100, 98,
    /* k 48.. 63 */
    96, 94, 92, 90, 88, 86, 84, 82, 80, 78, 76, 74, 72, 70, 68, 66,
    /* k 64.. 79: the edge itself (k = 64 is exactly on it: half covered). */
    64, 62, 60, 58, 56, 54, 52, 50, 48, 46, 44, 42, 40, 38, 36, 34,
    /* k 80.. 95: the ramp's empty end (k = 96 is half a pixel outside). */
    32, 30, 28, 26, 24, 22, 20, 18, 16, 14, 12, 10, 8, 6, 4, 2,
    /* k 96..127: outside -- invisible. */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

uint16_t cgl_arc_cov_q6(int32_t d_q6)
{
    int32_t k = 64 - d_q6;

    if (k < 0)
    {
        return 256u; /* more than a pixel inside: the answer is not a ramp value */
    }
    if (k >= (int32_t)CGL_ARC_COV_N)
    {
        return 0u; /* more than a pixel outside */
    }
    return (uint16_t)((uint32_t)s_cov_q7[k] * 2u);
}

/* ========================================================================== */
/* The tapered blade                                                          */
/* ========================================================================== */

bool cgl_arc_stroke_setup(cgl_arc_stroke_t *g, int32_t bx_q6, int32_t by_q6, int32_t tx_q6,
                          int32_t ty_q6, int16_t w_base, int16_t w_tip)
{
    int32_t ax;
    int32_t ay;
    uint32_t len;

    if (g == NULL)
    {
        return false;
    }
    ax = tx_q6 - bx_q6;
    ay = ty_q6 - by_q6;
    /* `isqrt` of a sum of **Q6** squares is the length in Q6 -- the sum is a Q12
       quantity and the root brings it back down -- so `len` is in the same units as
       the endpoints and every ratio below is dimensionless.
       WARNING: this is the one line that invites a "fix". Shifting the argument
       because "the length should be Q6" leaves a length 64 times too long, which
       makes `ux_q16`/`uy_q16` (and with them the across-the-axis distance the
       coverage is computed from) 64 times too small: a 3 px blade then paints half
       of its own bounding box. Caught on the host while this file was being moved,
       as 2081 inked pixels at 42 degrees against the 546 the same needle inks
       upright -- a needle-shaped smear rather than a needle. */
    len = cgl_math_isqrt((uint32_t)((ax * ax) + (ay * ay)));

    if (len == 0u)
    {
        return false; /* a dot is not a hand; the hub covers this case anyway */
    }

    /* Only assigned once the shape is known to be drawable: the caller is told
       "false" and must not draw with `g`, and a half-built stroke that still
       looks plausible is the one way that rule gets broken. */
    g->bx_q6 = bx_q6;
    g->by_q6 = by_q6;
    g->ax_q6 = ax;
    g->ay_q6 = ay;
    g->len_q6 = (int32_t)len;
    /* The axis in Q16 over its own length: a dimensionless direction, and the
       truncation is harmless -- an axis 1 LSB of Q16 off rotates the blade by
       0.02 degrees, which is a hundredth of the coverage ramp's own step. */
    g->ux_q16 = (ax << 16) / (int32_t)len;
    g->uy_q16 = (ay << 16) / (int32_t)len;
    g->wb_q6 = (int32_t)w_base * 32; /* a full width of w is a half width of w/2 px */
    g->wt_q6 = (int32_t)w_tip * 32;
    /* Normalised by the length here rather than at every pixel: the pixel's
       multiply is then `t * dw_q12`, and since `t` is clamped to the stroke, the
       product is bounded by the width difference and cannot overflow. */
    g->dw_q12 = (int32_t)(((g->wt_q6 - g->wb_q6) << 12) / (int32_t)len);
    return true;
}

void cgl_arc_stroke_draw(cgl_canvas_t *c, const cgl_arc_stroke_t *g, cgl_color_t col,
                         uint16_t alpha_q8, int16_t ox, int16_t oy)
{
    int32_t bx;
    int32_t by;
    int32_t tx;
    int32_t ty;
    int32_t wmax;
    int32_t pad;
    int32_t xlo;
    int32_t xhi;
    int32_t ylo;
    int32_t yhi;
    int32_t y;

    if ((c == NULL) || (g == NULL) || (g->len_q6 <= 0))
    {
        return;
    }

    bx = g->bx_q6 + ((int32_t)ox << 6);
    by = g->by_q6 + ((int32_t)oy << 6);
    tx = bx + g->ax_q6;
    ty = by + g->ay_q6;
    wmax = (g->wb_q6 > g->wt_q6) ? g->wb_q6 : g->wt_q6;
    pad = wmax + 64; /* one pixel of ramp beyond the edge */
    xlo = ((((bx < tx) ? bx : tx) - pad) >> 6);
    xhi = ((((bx < tx) ? tx : bx) + pad) >> 6);
    ylo = ((((by < ty) ? by : ty) - pad) >> 6);
    yhi = ((((by < ty) ? ty : by) + pad) >> 6);

    /* --- bind the walk to this band ---------------------------------------
     * A widget is drawn **once per band**, so a rasteriser that walks its whole
     * bounding box in every band pays 43 times for one picture. A hand's box is up
     * to 150 x 150 px, so the three hands and their shadows re-walked ~244 000
     * pixels to put down ~10 000 -- and on a frame that pushes every band (a page
     * transition, the entrance reveal, the minute pulse) that walk was about **two
     * thirds of the frame's CPU cost**, which is why the clock page stuttered
     * there while the smash and bullet pages, whose frames are cheap enough to be
     * limited by the SPI, did not.
     *
     * The bound is `cgl_canvas_src_window()`: the band itself while the canvas is
     * flat, and the band's **pre-image** while a warp is installed. A suction
     * transition compresses the page towards the suction point, so a source row
     * far outside the band can still land inside it -- and the pre-image is the
     * only bound that is both correct and finite. Without it the hands are
     * re-walked in all 43 bands *with* a warp mapping per pixel: measured on the
     * host, one S_OUT frame of the watch page cost 18.7 M cycles that way against
     * 1.8 M for the same page flat, which is what made "the clock page with
     * S_IN/S_OUT" run at four frames for the whole transition. Clamping x the same
     * way is what stops a hand that is nearly horizontal, whose per-row strip is
     * its whole bounding box (see `half_x` below), from walking the full width of
     * that box.
     *
     * Nothing about the picture changes: every pixel this removes was already
     * outside the window, where the blend call below would have discarded it.
     * --------------------------------------------------------------------- */
    {
        cgl_rect_t win = cgl_canvas_src_window(c);

        if (cgl_rect_is_empty(win))
        {
            return; /* this band cannot see the hand at all */
        }
        if (xlo < (int32_t)win.x0 - (int32_t)c->pan.x)
        {
            xlo = (int32_t)win.x0 - (int32_t)c->pan.x;
        }
        if (xhi > (int32_t)win.x1 - (int32_t)c->pan.x)
        {
            xhi = (int32_t)win.x1 - (int32_t)c->pan.x;
        }
        if (ylo < (int32_t)win.y0 - (int32_t)c->pan.y)
        {
            ylo = (int32_t)win.y0 - (int32_t)c->pan.y;
        }
        if (yhi > (int32_t)win.y1 - (int32_t)c->pan.y)
        {
            yhi = (int32_t)win.y1 - (int32_t)c->pan.y;
        }
        if ((xlo > xhi) || (ylo > yhi))
        {
            return; /* this band does not cross the hand at all */
        }
    }

    for (y = ylo; y <= yhi; y++)
    {
        int32_t yc = (y << 6) + 32; /* this row's pixel centres, Q6 */
        int32_t xa = xlo;
        int32_t xb = xhi;
        int32_t x;

        if (g->ay_q6 != 0)
        {
            /* The strip this row may write is the pen **projected onto the row**:
               `half width / |sin|`, the angle being between the hand and the row. It
               is the pen itself for a hand near vertical, but a row crossing a hand
               near *horizontal* runs a long way inside it -- 16 px at 11 degrees for
               a 3 px needle -- so narrowing to the pen's own width there cuts the
               blade down to a hairline and then, at exactly horizontal, lets it back
               out again. (Found on hardware as "the second hand fades to a dotted
               line from :10 to :20 and from :40 to :50, then recovers":
               `test_second_hand_continuous` measures the thickness across the hand at
               every second of the minute, and caught it going to zero.)
               WARNING: the clamp to the endpoint box below is not optional. `half_x`
               is unbounded as the hand approaches horizontal (it is a division by a
               Q16 sine, so 1.4e8 at its worst), and that is exactly the case the box
               is here to cut back. */
            int32_t uy = (g->uy_q16 < 0) ? -g->uy_q16 : g->uy_q16;
            int32_t half_x = (pad << 16) / ((uy != 0) ? uy : 1);
            int32_t xc = bx + (((yc - by) * g->ax_q6) / g->ay_q6);

            xa = (xc - half_x) >> 6;
            xb = (xc + half_x) >> 6;
            if (xa < xlo)
            {
                xa = xlo;
            }
            if (xb > xhi)
            {
                xb = xhi;
            }
        }

        for (x = xa; x <= xb; x++)
        {
            int32_t px = ((x << 6) + 32) - bx;
            int32_t py = yc - by;
            int32_t n_q6 = (px * g->uy_q16 - py * g->ux_q16) >> 16; /* across */
            int32_t t_q6 = (px * g->ux_q16 + py * g->uy_q16) >> 16; /* along  */
            int32_t tc;
            int32_t hw_q6;
            uint16_t cov;
            uint16_t end;

            if (n_q6 < 0)
            {
                n_q6 = -n_q6;
            }
            /* The taper is evaluated from the *clamped* position (so the width is
               the width of the quad at its own ends), while the two end ramps use
               the true one -- that is what rounds the hand off square instead of
               running it out to a point. */
            tc = (t_q6 < 0) ? 0 : ((t_q6 > g->len_q6) ? g->len_q6 : t_q6);
            hw_q6 = g->wb_q6 + ((tc * g->dw_q12) >> 12);

            cov = cgl_arc_cov_q6(hw_q6 - n_q6);
            if (cov == 0u)
            {
                continue;
            }
            end = cgl_arc_cov_q6(g->len_q6 - t_q6);
            if (end < cov)
            {
                cov = end;
            }
            end = cgl_arc_cov_q6(t_q6);
            if (end < cov)
            {
                cov = end;
            }
            if (cov == 0u)
            {
                continue;
            }
            if (alpha_q8 < 256u)
            {
                cov = (uint16_t)(((uint32_t)cov * alpha_q8) >> 8);
                if (cov == 0u)
                {
                    continue;
                }
            }
            cgl_canvas_pixel_blend_q8(c, (int16_t)x, (int16_t)y, col, cov);
        }
    }
}

void cgl_arc_needle(cgl_canvas_t *c, int16_t cx, int16_t cy, cgl_angle_t a, int16_t r_base,
                    int16_t r_tip, int16_t w_base, int16_t w_tip, cgl_color_t col,
                    cgl_color_t shadow_col, uint16_t shadow_alpha, int16_t shadow_off,
                    cgl_point_t *tip)
{
    cgl_arc_stroke_t g;
    int32_t bx_q6;
    int32_t by_q6;
    int32_t tx_q6;
    int32_t ty_q6;
    int16_t dx;
    int16_t dy;

    /* The tip is answered **before** the stroke is set up, and whether or not it
       can be drawn: it is the whole-pixel point the damage model has to mark, and
       a caller that asked for it is erasing the previous frame's hand with it. A
       degenerate blade (base == tip) draws nothing but still has a tip. */
    if (tip != NULL)
    {
        cgl_math_polar(a, r_tip, &dx, &dy);
        tip->x = (int16_t)(cx + dx);
        tip->y = (int16_t)(cy + dy);
    }

    /* The base is at the *opposite* angle: `r_base` is how far the blade reaches
       back behind the centre, where the hub covers it. */
    cgl_math_polar_q6(cx, cy, (cgl_angle_t)(a + CGL_ANGLE_HALF), r_base, &bx_q6, &by_q6);
    cgl_math_polar_q6(cx, cy, a, r_tip, &tx_q6, &ty_q6);
    if (!cgl_arc_stroke_setup(&g, bx_q6, by_q6, tx_q6, ty_q6, w_base, w_tip))
    {
        return; /* a zero length blade is a dot, and a dot is not a hand */
    }

    /* Shadow first and *under* the ink, offset down and to the right, which is
       where the light on the reference dial comes from (the specular highlight
       sits up and to the left). A blend rather than a second dark hand for a
       reason worth stating: 30 % of black over the dial *and over whatever the
       hand crosses* is what makes the hand look like it is floating a millimetre
       above the face, while a solid dark outline would look like a sticker.
       `shadow_alpha == 0` needs no separate switch -- but it does need no work
       either, so the stroke is not even started. */
    if (shadow_alpha != 0u)
    {
        cgl_arc_stroke_draw(c, &g, shadow_col, shadow_alpha, shadow_off, shadow_off);
    }
    cgl_arc_stroke_draw(c, &g, col, 256u, 0, 0);
}

cgl_rect_t cgl_arc_needle_damage(cgl_rect_t dmg, int16_t cx, int16_t cy, cgl_angle_t a,
                                 int16_t r_base, int16_t r_tip, int16_t half_w,
                                 int16_t shadow_off)
{
    /* The pad has to cover the widest half width *and* the drop shadow, which puts
       ink `shadow_off` further out than the geometry does -- forgetting it leaves a
       two pixel ghost of the shadow behind a hand that has moved, which is exactly
       the kind of bug the watch page's no-ghost test exists to catch. The extra 2
       is the antialiasing ramp, which reaches a pixel beyond the pen. */
    int16_t pad = (int16_t)(half_w + shadow_off + 2);
    int16_t dx;
    int16_t dy;
    int16_t tx;
    int16_t ty;
    int16_t bx;
    int16_t by;
    int16_t x0;
    int16_t x1;
    int16_t y0;
    int16_t y1;

    /* Both ends are taken at the *padded* radius, from the same polar function the
       drawing uses: a box computed from the endpoints "mathematically" instead
       (r*cos, truncating) would sit a pixel inside the ink at some angles, and the
       ghost is only ever visible at some angles. */
    cgl_math_polar(a, (int16_t)(r_tip + pad), &dx, &dy);
    tx = (int16_t)(cx + dx);
    ty = (int16_t)(cy + dy);
    cgl_math_polar((cgl_angle_t)(a + CGL_ANGLE_HALF), (int16_t)(r_base + pad), &dx, &dy);
    bx = (int16_t)(cx + dx);
    by = (int16_t)(cy + dy);

    x0 = (tx < bx) ? tx : bx;
    x1 = (tx < bx) ? bx : tx;
    y0 = (ty < by) ? ty : by;
    y1 = (ty < by) ? by : ty;
    return cgl_rect_union(dmg, cgl_rect_wh(x0, y0, (int16_t)(x1 - x0 + 1),
                                           (int16_t)(y1 - y0 + 1)));
}

/* ========================================================================== */
/* Drawing a curve with transparency                                          */
/* ========================================================================== */

/*
 * These five are four lines each and they exist for exactly one reason: ink alpha is
 * canvas **state**, and state that a caller has to put back is state that will
 * eventually not be put back. Each of them saves the alpha, sets it, draws one shape
 * and restores it, so a translucent ring can sit in the middle of a page without the
 * elements after it coming out faint.
 *
 * The save/restore is a local, not a canvas field, and the drawing call in between is
 * the real primitive -- there is no second rasteriser here and nothing to keep in
 * step with one.
 */
static uint16_t arc__alpha_enter(cgl_canvas_t *c, uint16_t alpha_q8)
{
    uint16_t was = cgl_canvas_get_ink_alpha(c);

    cgl_canvas_set_ink_alpha(c, alpha_q8);
    return was;
}

void cgl_arc_draw_arc(cgl_canvas_t *c, int16_t cx, int16_t cy, int16_t radius, int16_t thickness,
                      cgl_angle_t start, uint32_t sweep, cgl_color_t color, uint16_t alpha_q8)
{
    uint16_t was;

    if (c == NULL)
    {
        return;
    }
    was = arc__alpha_enter(c, alpha_q8);
    cgl_canvas_arc(c, cx, cy, radius, thickness, start, sweep, color);
    cgl_canvas_set_ink_alpha(c, was);
}

void cgl_arc_draw_ring(cgl_canvas_t *c, int16_t cx, int16_t cy, int16_t radius, int16_t thickness,
                       cgl_color_t color, uint16_t alpha_q8)
{
    uint16_t was;

    if (c == NULL)
    {
        return;
    }
    was = arc__alpha_enter(c, alpha_q8);
    cgl_canvas_arc(c, cx, cy, radius, thickness, 0u, (uint32_t)CGL_ANGLE_TURN, color);
    cgl_canvas_set_ink_alpha(c, was);
}

void cgl_arc_draw_circle(cgl_canvas_t *c, int16_t cx, int16_t cy, int16_t radius, cgl_color_t color,
                         uint16_t alpha_q8)
{
    uint16_t was;

    if (c == NULL)
    {
        return;
    }
    was = arc__alpha_enter(c, alpha_q8);
    cgl_canvas_circle(c, cx, cy, radius, color);
    cgl_canvas_set_ink_alpha(c, was);
}

void cgl_arc_draw_disc(cgl_canvas_t *c, int16_t cx, int16_t cy, int16_t radius, cgl_color_t color,
                       uint16_t alpha_q8)
{
    uint16_t was;

    if (c == NULL)
    {
        return;
    }
    was = arc__alpha_enter(c, alpha_q8);
    /* A stroke of width 2r on a radius of 0 is an antialiased disc -- see cgl_canvas_arc.
       The `radius <= 0` case is the dot this degenerates to, and it is still a disc, so
       there is no special case to write here. */
    cgl_canvas_arc(c, cx, cy, 0, (int16_t)(2 * radius), 0u, (uint32_t)CGL_ANGLE_TURN, color);
    cgl_canvas_set_ink_alpha(c, was);
}

void cgl_arc_draw_line(cgl_canvas_t *c, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                       int16_t thickness, cgl_color_t color, uint16_t alpha_q8)
{
    uint16_t was;

    if (c == NULL)
    {
        return;
    }
    was = arc__alpha_enter(c, alpha_q8);
    cgl_canvas_line_thick(c, x0, y0, x1, y1, thickness, color);
    cgl_canvas_set_ink_alpha(c, was);
}

/* ========================================================================== */
/* The shaded radial surface                                                  */
/* ========================================================================== */

/** xorshift32 -- the whole of "the texture is procedural".
 *
 *  Called once per table entry and once per sunburst ray, never once per pixel, so
 *  the texture costs a lookup in the inner loop. What it buys is that no two
 *  ridges and no two rays have the same strength: a sunburst with 128 *identical*
 *  rays reads as a printed grating, and one with 128 slightly different ones reads
 *  as metal. */
static uint32_t arc__hash(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/** Which of the 2048 sectors of a turn (dx, dy) points into: 16 sectors per ray of
 *  a 128 ray sunburst.
 *
 *  Only a *direction* can tell one ray from the next, and the cheap way to get one
 *  is the classic "larger component as the denominator": the ratio `small/large`
 *  is the angle inside the octant, and the octant is two sign tests and one
 *  comparison. That is one hardware division per pixel, and on a surface that has
 *  a table it is the *only* division anywhere in the pixel path.
 *
 *  WARNING: `small/large` is not the angle -- it is linear in the *tangent* where
 *  the angle is linear in the arc, and the error peaks at the octant boundary at
 *  4.07 % of an octant (1.1 degrees). On a 128 ray sunburst that is a third of a
 *  ray, i.e. the rays come out very slightly uneven in width. Nobody measures a
 *  watch dial in degrees, and the exact version costs an arctangent table; the
 *  unevenness is invisible at these amplitudes. */
static uint16_t arc__angle_code(int32_t dx, int32_t dy)
{
    uint32_t ax = (uint32_t)((dx < 0) ? -dx : dx);
    uint32_t ay = (uint32_t)((dy < 0) ? -dy : dy);
    uint32_t t;

    if (ax >= ay)
    {
        t = (ay << 8) / ((ax != 0u) ? ax : 1u); /* 0..256 across the octant */
        return (uint16_t)((dx >= 0) ? ((dy >= 0) ? t : (2048u - t))
                                    : ((dy >= 0) ? (1024u - t) : (1024u + t)));
    }
    t = (ax << 8) / ((ay != 0u) ? ay : 1u);
    return (uint16_t)((dy >= 0) ? ((dx >= 0) ? (512u - t) : (512u + t))
                                : ((dx >= 0) ? (1536u + t) : (1536u - t)));
}

/** `dx*dx + dy*dy` in unsigned 32 bit.
 *
 *  Written as two unsigned multiplies rather than `(uint32_t)(dx*dx + dy*dy)` so
 *  that no intermediate is a signed product: the public door takes an `int16_t`
 *  offset, but `dx*dx` in `int32_t` is one step away from its own overflow for the
 *  extreme offsets a caller may legitimately ask about, and a signed overflow is
 *  undefined behaviour rather than a wrong colour. The value is identical wherever
 *  the exact answer fits in 32 bits, which is every offset a 320 x 172 panel can
 *  put on a surface. */
CGL_AINLINE uint32_t arc__d2(int32_t dx, int32_t dy)
{
    uint32_t x = (uint32_t)dx;
    uint32_t y = (uint32_t)dy;

    return (x * x) + (y * y);
}

/** The FACE's radial gradient, at a radius in Q8 of the face radius.
 *
 *  Three stops: `c_hi -> c_mid` out to `knee_q8`, then `c_mid -> c_lo` out to the
 *  rim. Both slopes are evaluated by one blend each, and the callers pass a radius
 *  that is either a table entry's *real* radius or a pixel's own (see the note at
 *  the top of the file) -- which is what makes the two paths agree.
 *
 *  The degenerate knees are answered rather than guarded: `knee_q8` is a
 *  `uint8_t`, so the only value that can break the ramp is 0, and it means "there
 *  is no lit centre" -- the whole radius is then the `c_mid -> c_lo` segment. A
 *  radius past the rim (>= 1.0 of the face) is the edge stop, which is what the
 *  second segment converges to anyway. */
static cgl_color_t arc__face_gradient(const cgl_arc_surface_t *s, uint16_t r_q8)
{
    uint32_t r = (uint32_t)r_q8;
    uint32_t knee = (uint32_t)s->knee_q8;

    if (r >= 256u)
    {
        return s->c_lo; /* past the rim: the last stop, exactly what r == 256 gives */
    }
    if ((knee != 0u) && (r < knee))
    {
        return cgl_color_blend_q8(s->c_hi, s->c_mid, (uint16_t)((r << 8) / knee));
    }
    /* knee <= 255 here (`uint8_t`), so 256 - knee is at least 1: the divisor can
       never be zero and never negative, and the knee >= 256 case the header warns
       about cannot be represented in the field at all. */
    return cgl_color_blend_q8(s->c_mid, s->c_lo, (uint16_t)(((r - knee) << 8) / (256u - knee)));
}

/** The FACE's shading at the pixel's own radius: the gradient plus the circular
 *  brushing, with no table involved.
 *
 *  This is `lut == NULL`: one `isqrt` and a blend per pixel, roughly three times
 *  the table path, and **zero bytes of flash** -- the right trade for a small or
 *  rarely drawn surface.
 *
 *  WARNING: the radius has to be divided by `r_out` before it means anything to
 *  `arc__face_gradient()`. `knee_q8` is a fraction **of the face radius** (the
 *  reference dial's 132 is 0.52 of it), while `isqrt(d2 << 16)` is a radius in Q8
 *  *pixels*: on an 80 px face a pixel 47 px out would come back as `r_q8 = 12076`
 *  and every stop would be read as "past the rim", i.e. the procedural dial would
 *  be a flat edge-coloured disc. The table path never meets this because its index
 *  is already normalised by the Q16 scale; the procedural path normalises here
 *  instead, which is the same mapping the bake performs when it turns entry `i`
 *  into `sqrt(i/(n-1))` -- so a baked face and a procedural one are the same
 *  surface, which is the property the two paths exist to have.
 *
 *  The radius is clamped before the shift because `r_q8` is a `uint16_t`, so the
 *  largest radius this representation can carry is 256 px: a surface that fits a
 *  320 x 172 panel is always inside that, and an off-surface offset still gets a
 *  defined answer (the rim). */
CGL_AINLINE cgl_color_t arc__face_procedural(const cgl_arc_surface_t *s, uint32_t d2)
{
    uint32_t d = (d2 > 0xFFFFu) ? 0xFFFFu : d2;
    uint32_t r_out = (uint32_t)s->r_out;
    uint32_t r_abs = cgl_math_isqrt(d << 16); /* the radius in Q8 pixels */
    uint16_t r_q8 = (uint16_t)((r_abs + (r_out / 2u)) / r_out); /* Q8 of the face radius */
    cgl_color_t col = arc__face_gradient(s, r_q8);
    uint32_t amp = ((uint32_t)r_q8 * s->tex_amp) >> 8;
    uint32_t h = arc__hash(((uint32_t)r_q8 * s->tex_density) >> 8);
    uint32_t k = (amp * (1u + (h & 3u))) >> 2;

    return cgl_color_blend_q8(col, s->tex, (uint16_t)k);
}

/** The RING's cross-section at the pixel's own radius: a half sine across the
 *  annulus, from the inner edge to the outer one.
 *
 *  The interpretation is the whole shading of the cylinder, and it is worth
 *  spelling out: the surface turns away from the eye at *both* edges, so the
 *  profile is dark where the ring meets the dial, brightest in the middle and dark
 *  again where it meets the case lip -- which is the difference between "a band"
 *  and "a turned metal ring". The tilt (top-dark / bottom-bright, because the ring
 *  is sunk where the face is domed) is applied by the caller, per pixel.
 *
 *  WARNING: the angle is derived from the pixel's **sub-pixel** radius
 *  (`isqrt(d2 << 16)`), not from its whole-pixel one. An 8 px cross-section
 *  sampled at integer radii can only ever produce 9 distinct values and the lit
 *  crown comes out as a staircase with flat tops; in Q8 every entry of an 8 px
 *  profile has its own value and the crown curves. */
CGL_AINLINE cgl_color_t arc__ring_procedural(const cgl_arc_surface_t *s, uint32_t d2)
{
    int32_t r_in = (s->r_in > 0) ? (int32_t)s->r_in : 0;
    int32_t r_out = (int32_t)s->r_out;
    uint32_t d = (d2 > 0xFFFFu) ? 0xFFFFu : d2;
    int32_t r_q8 = (int32_t)cgl_math_isqrt(d << 16);
    int32_t num = r_q8 - (r_in << 8);
    int32_t den = (r_out - r_in) << 8;
    uint32_t t;

    if (num <= 0)
    {
        return cgl_color_blend_q8(s->c_lo, s->c_hi, 0u); /* at or inside the inner edge */
    }
    if (num >= den)
    {
        return cgl_color_blend_q8(s->c_lo, s->c_hi, 256u); /* at or past the outer one */
    }
    /* `den` is only large enough for `num * CGL_ANGLE_HALF` to overflow if the
       cross-section is more than 256 px thick, which no ring that fits a 320 x 172
       panel is -- and the scaling below keeps it safe even there. It scales both
       terms equally, so the fraction is unchanged; below 256 px of thickness (every
       real ring, and the reference 8) the branch never runs and the arithmetic is
       the header's, term for term. */
    if (den > 0xFFFF)
    {
        num >>= 8;
        den >>= 8;
    }
    t = ((uint32_t)num * (uint32_t)CGL_ANGLE_HALF) / (uint32_t)den;
    if (t > (uint32_t)CGL_ANGLE_HALF)
    {
        t = (uint32_t)CGL_ANGLE_HALF;
    }
    return cgl_color_blend_q8(s->c_lo, s->c_hi, (uint16_t)cgl_sin_q8((cgl_angle_t)t));
}

/** How many entries the surface's table has: `lut_n` when it is non-zero,
 *  otherwise the default for its kind. Shared by the public door and the pixel
 *  path so that the two cannot disagree about where the table ends. */
CGL_AINLINE uint32_t arc__entries(const cgl_arc_surface_t *s)
{
    if (s->lut_n != 0u)
    {
        return (uint32_t)s->lut_n;
    }
    return (uint32_t)((s->kind == (uint8_t)CGL_ARC_RING) ? CGL_ARC_RING_LUT_N
                                                         : CGL_ARC_FACE_LUT_N);
}

/** The FACE's colour at an offset from its centre: gradient, sunburst and specular
 *  together. Pure -- same offset, same colour, for ever. */
CGL_AINLINE cgl_color_t arc__face_px(const cgl_arc_surface_t *s, int32_t dx, int32_t dy)
{
    uint32_t d2 = arc__d2(dx, dy);
    uint32_t n = arc__entries(s);
    uint32_t r2;
    uint32_t scale;
    uint32_t idx;
    cgl_color_t col;

    if (s->r_out <= 0)
    {
        return s->c_mid; /* no radius, no surface: the flat colour the warp path uses */
    }
    /* `(n-1) / r_out^2` in Q16, evaluated once per pixel and then applied with one
       multiply: the table is indexed by r^2 because that is what the inner loop
       already knows (see the note on isqrt in cgl_math.h). The largest `n` a
       `uint16_t` can hold still leaves `(n-1) << 16` inside a uint32. */
    r2 = (uint32_t)s->r_out * (uint32_t)s->r_out;
    scale = ((n - 1u) << 16) / r2;
    idx = (d2 * scale) >> 16;
    if (idx > (n - 1u))
    {
        idx = n - 1u;
    }
    col = (s->lut != NULL) ? s->lut[idx] : arc__face_procedural(s, d2);

    /* --- the sunburst ---
     * 128 rays, every other one catching the light, each with its own strength
     * from arc__hash(). Its amplitude is proportional to `idx` (i.e. to `r^2`), so
     * the fan dissolves into a smooth pool in the middle of the dial: that is what
     * a sunburst does, and it is also what keeps the near-centre rays -- a whole
     * 3 degree sector inside one pixel -- from turning into noise.
     * `ray_amp == 0` is "off": the blend would be the identity anyway (alpha 0),
     * so the branch is a saving, not a behaviour. */
    if ((s->ray_amp != 0u) && (idx >= (uint32_t)s->ray_min))
    {
        uint16_t code = arc__angle_code(dx, dy);
        uint32_t h = arc__hash((uint32_t)(code >> 4));
        /* Every other ray catches the light, and each ray's own hash decides how
           lit, so no two are the same. */
        uint32_t ray = ((code & 0x10u) != 0u) ? (uint32_t)s->ray_amp
                                              : ((uint32_t)s->ray_amp >> 2);
        uint32_t amp = (idx * ray) >> 8;
        uint32_t k = (amp * (1u + (h & 3u))) >> 2;

        col = cgl_color_blend_q8(col, s->tex, (uint16_t)k);
    }

    /* --- the specular ---
     * A quadratic falloff inside `light_r`, and it is clipped by the surface for
     * free because only pixels on the surface reach it. `light_r <= 0` is "no
     * highlight" and is checked before the division. */
    if (s->light_r > 0)
    {
        int32_t sx = dx - (int32_t)s->light_dx;
        int32_t sy = dy - (int32_t)s->light_dy;
        uint32_t sd2 = arc__d2(sx, sy);
        uint32_t lr2 = (uint32_t)s->light_r * (uint32_t)s->light_r;

        if (sd2 < lr2)
        {
            uint32_t sscale = ((uint32_t)255u << 16) / lr2;
            uint32_t q = (sd2 * sscale) >> 16; /* 0 at the centre .. 255 at the rim */
            uint32_t inv = 256u - q;

            col = cgl_color_blend_q8(col, s->spec,
                                     (uint16_t)(((uint32_t)s->light_alpha * inv * inv) >> 16));
        }
    }
    return col;
}

/** The RING's colour at an offset from its centre: the cross-section out of the
 *  table (or shaded procedurally), then the well's tilt -- dark at the top, bright
 *  at the bottom, neutral at the two sides, which is what "the light is above, the
 *  surface is below" looks like. Pure, like the face. */
CGL_AINLINE cgl_color_t arc__ring_px(const cgl_arc_surface_t *s, int32_t dx, int32_t dy)
{
    uint32_t d2 = arc__d2(dx, dy);
    uint32_t n = arc__entries(s);
    int32_t r_in = (s->r_in > 0) ? (int32_t)s->r_in : 0;
    int32_t r_out = (int32_t)s->r_out;
    uint32_t r2in;
    uint32_t r2out;
    uint32_t idx;
    uint32_t tiltk;
    uint32_t tilt;
    cgl_color_t col;

    if (r_out <= r_in)
    {
        return s->c_lo; /* no cross-section to shade: the inner/edge stop, as the
                           procedural path answers for the same degenerate ring */
    }
    r2in = (uint32_t)r_in * (uint32_t)r_in;
    r2out = (uint32_t)r_out * (uint32_t)r_out;

    if (d2 <= r2in)
    {
        idx = 0u; /* inside the hole: the cross-section's first entry */
    }
    else
    {
        /* `(n-1) / (r_out^2 - r_in^2)` in Q16: the annulus' index of a pixel's r^2.
           The denominator is positive because the guard above rejected every
           degenerate cross-section. */
        uint32_t scale = ((n - 1u) << 16) / (r2out - r2in);

        idx = ((d2 - r2in) * scale) >> 16;
        if (idx > (n - 1u))
        {
            idx = n - 1u;
        }
    }
    col = (s->lut != NULL) ? s->lut[idx] : arc__ring_procedural(s, d2);

    /* --- the well's tilt ---
     * `tilt / r_out` in Q8 is the tilt per pixel of vertical offset, so the term is
     * "how far up or down the pixel is, as a fraction of the ring", which is what
     * makes the shading follow the ring rather than the screen. */
    tiltk = ((uint32_t)s->tilt << 8) / (uint32_t)r_out;
    tilt = ((uint32_t)((dy < 0) ? -dy : dy) * tiltk) >> 8;
    if (tilt > 255u)
    {
        /* Off the surface (|dy| > r_out): a Q8 opacity is not defined past 1.0, and
           on the surface the term is bounded by `tilt` itself, so this clamp costs
           the drawn pixels nothing. */
        tilt = 255u;
    }
    if (tilt != 0u)
    {
        col = cgl_color_blend_q8(col, (dy < 0) ? s->tilt_dark : s->tilt_light, (uint16_t)tilt);
    }
    return col;
}

/** The surface's colour at an offset: the whole surface, and the one function the
 *  painter and `cgl_arc_surface_color()` both go through.
 *
 *  A kind that is neither RING nor FACE is treated as FACE, which is also what the
 *  enumerator's value 0 means -- a mis-set `kind` then draws a dial rather than
 *  nothing at all. */
CGL_AINLINE cgl_color_t arc__surface_px(const cgl_arc_surface_t *s, int32_t dx, int32_t dy)
{
    if (s->kind == (uint8_t)CGL_ARC_RING)
    {
        return arc__ring_px(s, dx, dy);
    }
    return arc__face_px(s, dx, dy);
}

void cgl_arc_surface_default_face(cgl_arc_surface_t *s)
{
    if (s == NULL)
    {
        return;
    }
    /* Geometry left at zero for the caller: a face's radius is the one number that
       says what the dial *is*, so guessing it here would be guessing the panel.
       See the note on `cgl_arc_surface_default_ring()` for the other choice. */
    s->kind = (uint8_t)CGL_ARC_FACE;
    s->cx = 0;
    s->cy = 0;
    s->r_in = 0;
    s->r_out = 0;

    /* The reference dial's palette: four levels of red and eleven of green between
       the lit centre and the rim, which is as much gradient as a 5-6-5 panel can
       carry under a dark dial (see app_page_clock_dial.h -- a stop four levels
       lower leaves no room and puts a green speckle where the dial was). */
    s->c_hi = CGL_RGB565(58, 68, 84);
    s->c_mid = CGL_RGB565(34, 40, 52);
    s->c_lo = CGL_RGB565(12, 15, 20);
    s->knee_q8 = 132u;

    s->tex = CGL_RGB565(120, 136, 162);
    s->tex_amp = 44u;
    s->tex_density = 84u;

    s->ray_amp = 60u;
    s->ray_min = 30u;

    s->spec = CGL_RGB565(176, 194, 224);
    /* Up and to the left, at 48 px of radius: the same light the well's tilt and
       the crystal's reflection come from. */
    s->light_dx = -27;
    s->light_dy = -31;
    s->light_r = 48;
    s->light_alpha = 54u;

    /* The RING's knobs are zero, not "some sensible tilt": a face with a tilt term
       left over from another surface is a dial shaded by the wrong light. */
    s->tilt_dark = 0;
    s->tilt_light = 0;
    s->tilt = 0u;

    s->lut = NULL;
    s->lut_n = CGL_ARC_FACE_LUT_N;
}

void cgl_arc_surface_default_ring(cgl_arc_surface_t *s)
{
    if (s == NULL)
    {
        return;
    }
    /* The two radii **are** the ring's cross-section (they are what `lut_n` entries
       are spread over), so unlike the face's radius they come from the reference
       dial rather than being left for the caller -- a ring with no radii is not a
       ring. A caller with another dial size overrides both. */
    s->kind = (uint8_t)CGL_ARC_RING;
    s->cx = 0;
    s->cy = 0;
    s->r_in = 71;
    s->r_out = 79;

    /* `c_lo -> c_hi` across the cross-section: dark where the ring turns away at
       the inner edge, lit at the crown. `c_mid` is not part of a two-stop profile;
       it is set to `c_lo` so that a caller who switches this surface's `kind` sees
       a coherent set of stops rather than a stale lit centre. */
    s->c_hi = CGL_RGB565(126, 138, 158);
    s->c_mid = CGL_RGB565(16, 19, 25);
    s->c_lo = CGL_RGB565(16, 19, 25);
    s->knee_q8 = 0u;

    /* No texture and no sunburst: the ring's detail is its cross-section, and a
       brushed ridge on a 8 px band is noise. */
    s->tex = 0u;
    s->tex_amp = 0u;
    s->tex_density = 0u;
    s->ray_amp = 0u;
    s->ray_min = 0u;
    s->spec = 0u;
    s->light_dx = 0;
    s->light_dy = 0;
    s->light_r = 0;
    s->light_alpha = 0u;

    /* Top-dark / bottom-bright, and it is the *opposite* way round to the dial's
       own highlight -- it has to be: a convex surface under a light from above is
       bright where it faces the light, a concave one is dark there. Shading both
       the same way is what makes a bezel look like a sticker instead of a
       machined recess. */
    s->tilt_dark = CGL_RGB565(7, 8, 11);
    s->tilt_light = CGL_RGB565(168, 182, 204);
    s->tilt = 120u;

    s->lut = NULL;
    s->lut_n = CGL_ARC_RING_LUT_N;
}

uint16_t cgl_arc_surface_lut_entries(const cgl_arc_surface_t *s)
{
    if (s == NULL)
    {
        return 0u;
    }
    return (uint16_t)arc__entries(s);
}

void cgl_arc_surface_bake(const cgl_arc_surface_t *s, uint16_t *lut, uint16_t n)
{
    uint32_t i;
    /* A one entry table is legal (a surface drawn in a single flat colour) and has
       no spacing to divide by, so both generators use 1 as the entry denominator
       they would otherwise take from `n - 1`. For n >= 2 this is the demo's
       arithmetic, term for term. */
    uint32_t den;
    uint32_t r_in;
    uint32_t r_out;
    uint32_t r2in;
    uint32_t r2out;
    uint32_t span_den;

    if ((s == NULL) || (lut == NULL) || (n == 0u))
    {
        return;
    }
    den = (n > 1u) ? (uint32_t)(n - 1u) : 1u;

    if (s->kind != (uint8_t)CGL_ARC_RING)
    {
        /* --- FACE: `sqrt(i/(n-1))` of the face radius per entry ---
         * The entries are uniform in `r^2` (what the inner loop can afford to index
         * by), so entry `i` stands for radius `sqrt(i/(n-1))` -- a square root per
         * *entry*, not per pixel. That is what makes the curve the designed one
         * rather than the one a squared radius happens to describe. */
        for (i = 0u; i < (uint32_t)n; i++)
        {
            uint32_t q16 = ((uint32_t)i << 16) / den;
            /* sqrt of a Q16 fraction is its Q8 root (see cgl_math_isqrt): the radius
               is then `r_q8 / 256` of the face, with no further scaling. */
            uint16_t r_q8 = (uint16_t)cgl_math_isqrt(q16);
            cgl_color_t col = arc__face_gradient(s, r_q8);
            /* The brush amplitude is faded by `r` (through `amp`), which does two
               jobs at once: near the middle one entry spans ~8 px of radius, where a
               1 px ridge would alias into a moire rosette; at the rim one entry spans
               0.16 px, where the ridges are finally fine enough to be seen. Fading
               with `r` therefore both prevents the artefact and produces what
               circular brushing actually looks like -- soft in the middle, fine and
               bright at the edge. */
            uint32_t amp = ((uint32_t)r_q8 * s->tex_amp) >> 8;
            uint32_t h = arc__hash(((uint32_t)r_q8 * s->tex_density) >> 8);
            uint32_t k = (amp * (1u + (h & 3u))) >> 2;

            lut[i] = cgl_color_blend_q8(col, s->tex, (uint16_t)k);
        }
        return;
    }

    /* --- RING: the half sine across the cross-section ---
     * Both radii are clamped the same way the pixel path clamps them (a negative
     * inner radius is "no hole"), so the baked table and the procedural shade
     * describe the same ring -- including the degenerate one, which is filled with
     * the edge stop rather than divided by a zero span. */
    r_out = (s->r_out > 0) ? (uint32_t)s->r_out : 0u;
    r_in = (s->r_in > 0) ? (uint32_t)s->r_in : 0u;
    if (r_out <= r_in)
    {
        /* A degenerate cross-section has one colour, not a profile: fill and leave
           rather than divide by a zero span. */
        for (i = 0u; i < (uint32_t)n; i++)
        {
            lut[i] = s->c_lo;
        }
        return;
    }
    r2in = r_in * r_in;
    r2out = r_out * r_out;
    span_den = (r_out - r_in) << 8; /* >= 256: r_out > r_in >= 0 */

    for (i = 0u; i < (uint32_t)n; i++)
    {
        uint32_t d2 = r2in + (((r2out - r2in) * i) / den);
        uint32_t r_q8 = cgl_math_isqrt(d2 << 16); /* the radius in Q8 */
        uint32_t t = ((r_q8 - (r_in << 8)) * den) / span_den;
        /* WARNING: `CGL_ANGLE_HALF / (n-1)` is an **integer** division, exactly as
           the demo's generator had it (32768 / 32 = 1024), and `t` is in entry
           units. A table resolution that does not divide a half turn therefore gets
           its angles truncated -- which is the curve the reference table was baked
           with, so it is the curve that stays. */
        uint16_t k = (uint16_t)cgl_sin_q8((cgl_angle_t)(t * ((uint32_t)CGL_ANGLE_HALF / den)));

        lut[i] = cgl_color_blend_q8(s->c_lo, s->c_hi, k);
    }
}

cgl_color_t cgl_arc_surface_color(const cgl_arc_surface_t *s, int16_t dx, int16_t dy)
{
    if (s == NULL)
    {
        return 0u;
    }
    return arc__surface_px(s, (int32_t)dx, (int32_t)dy);
}

/** Paint the FACE: the disc `r_out` around (cx, cy), row by row, opaquely. */
static void arc__paint_face(cgl_canvas_t *c, const cgl_arc_surface_t *s)
{
    int32_t r_out = (int32_t)s->r_out;
    int32_t r2;
    int32_t x0;
    int32_t x1;
    int32_t y0;
    int32_t y1;
    int32_t y;

    if (r_out <= 0)
    {
        return; /* nothing to fill: a surface with no radius covers no pixel */
    }
    if (c->warp != NULL)
    {
        /* A flat disc during a suction transition: see the trade documented in
           cgl_arc.h ("a gradient nobody can see is not worth a frame rate"). */
        cgl_canvas_disc(c, s->cx, s->cy, s->r_out, s->c_mid);
        return;
    }

    r2 = r_out * r_out;
    x0 = (int32_t)s->cx - r_out;
    x1 = (int32_t)s->cx + r_out;
    y0 = (int32_t)s->cy - r_out;
    y1 = (int32_t)s->cy + r_out;

    /* The clip window (which is in screen coordinates, hence the pan) ... */
    if (x0 < (int32_t)c->clip.x0 - (int32_t)c->pan.x)
    {
        x0 = (int32_t)c->clip.x0 - (int32_t)c->pan.x;
    }
    if (x1 > (int32_t)c->clip.x1 - (int32_t)c->pan.x)
    {
        x1 = (int32_t)c->clip.x1 - (int32_t)c->pan.x;
    }
    if (y0 < (int32_t)c->clip.y0 - (int32_t)c->pan.y)
    {
        y0 = (int32_t)c->clip.y0 - (int32_t)c->pan.y;
    }
    if (y1 > (int32_t)c->clip.y1 - (int32_t)c->pan.y)
    {
        y1 = (int32_t)c->clip.y1 - (int32_t)c->pan.y;
    }
    /* ... and the band, which is not a clip: outside it there is no row to write
       to, and asking cgl_canvas_row() once per pixel would be the whole cost. */
    if (y0 < (int32_t)c->origin_y - (int32_t)c->pan.y)
    {
        y0 = (int32_t)c->origin_y - (int32_t)c->pan.y;
    }
    if (y1 > (int32_t)c->origin_y + (int32_t)c->height - 1 - (int32_t)c->pan.y)
    {
        y1 = (int32_t)c->origin_y + (int32_t)c->height - 1 - (int32_t)c->pan.y;
    }
    if (x0 > x1)
    {
        return;
    }

    for (y = y0; y <= y1; y++)
    {
        int32_t dy = y - (int32_t)s->cy;
        int32_t d2r = dy * dy;
        int32_t span;
        int32_t xa;
        int32_t xb;
        int32_t x;
        cgl_color_t *row;

        if (d2r > r2)
        {
            continue;
        }
        row = cgl_canvas_row(c, (int16_t)(y + c->pan.y));
        if (row == NULL)
        {
            continue;
        }
        /* One isqrt per *row*, not one per pixel: `sqrt(R^2 - dy^2)` is the row's
           half span, which is the whole affordability argument for painting a disc
           without a framebuffer. */
        span = (int32_t)cgl_math_isqrt((uint32_t)(r2 - d2r));
        xa = (int32_t)s->cx - span;
        xb = (int32_t)s->cx + span;
        if (xa < x0)
        {
            xa = x0;
        }
        if (xb > x1)
        {
            xb = x1;
        }
        for (x = xa; x <= xb; x++)
        {
            /* Opaque, straight into the row: for a surface that is by definition
               "the thing underneath", that is both the cheap and the correct path.
               The per-pixel call is the *same* function `cgl_arc_surface_color()`
               returns, so the painter and any test oracle cannot drift -- that is
               worth two loads per pixel next to the isqrt the row already paid. */
            row[(int16_t)(x + c->pan.x)] = arc__surface_px(s, x - (int32_t)s->cx, dy);
        }
    }
}

/** Paint the RING: the annulus `r_in .. r_out`, row by row.
 *
 *  A row crosses an annulus in **two** spans (one either side of the hole), and the
 *  hole is absent altogether on the rows past the inner radius: `ri` is negative
 *  there and the two spans meet in the middle, which is why the arms below are
 *  written in terms of `il = max(ri, 0)` rather than with a branch per arm. */
static void arc__paint_ring(cgl_canvas_t *c, const cgl_arc_surface_t *s)
{
    int32_t r_in = (s->r_in > 0) ? (int32_t)s->r_in : 0;
    int32_t r_out = (int32_t)s->r_out;
    uint32_t r2in;
    uint32_t r2out;
    int32_t x0;
    int32_t x1;
    int32_t y0;
    int32_t y1;
    int32_t y;

    if (r_out <= r_in)
    {
        return; /* a degenerate cross-section has no pixels */
    }
    r2in = (uint32_t)r_in * (uint32_t)r_in;
    r2out = (uint32_t)r_out * (uint32_t)r_out;

    if (c->warp != NULL)
    {
        /* A flat ring during a suction transition: see the same trade in
           arc__paint_face(). */
        cgl_canvas_arc(c, s->cx, s->cy, (int16_t)((r_in + r_out) / 2),
                       (int16_t)(r_out - r_in), 0u, CGL_ANGLE_TURN, s->c_lo);
        return;
    }

    x0 = (int32_t)s->cx - r_out;
    x1 = (int32_t)s->cx + r_out;
    y0 = (int32_t)s->cy - r_out;
    y1 = (int32_t)s->cy + r_out;
    if (x0 < (int32_t)c->clip.x0 - (int32_t)c->pan.x)
    {
        x0 = (int32_t)c->clip.x0 - (int32_t)c->pan.x;
    }
    if (x1 > (int32_t)c->clip.x1 - (int32_t)c->pan.x)
    {
        x1 = (int32_t)c->clip.x1 - (int32_t)c->pan.x;
    }
    if (y0 < (int32_t)c->clip.y0 - (int32_t)c->pan.y)
    {
        y0 = (int32_t)c->clip.y0 - (int32_t)c->pan.y;
    }
    if (y1 > (int32_t)c->clip.y1 - (int32_t)c->pan.y)
    {
        y1 = (int32_t)c->clip.y1 - (int32_t)c->pan.y;
    }
    if (y0 < (int32_t)c->origin_y - (int32_t)c->pan.y)
    {
        y0 = (int32_t)c->origin_y - (int32_t)c->pan.y;
    }
    if (y1 > (int32_t)c->origin_y + (int32_t)c->height - 1 - (int32_t)c->pan.y)
    {
        y1 = (int32_t)c->origin_y + (int32_t)c->height - 1 - (int32_t)c->pan.y;
    }
    if (x0 > x1)
    {
        return;
    }

    for (y = y0; y <= y1; y++)
    {
        int32_t dy = y - (int32_t)s->cy;
        uint32_t d2r = (uint32_t)(dy * dy);
        int32_t ro;
        int32_t ri;
        int32_t il;
        int32_t arm;
        cgl_color_t *row;

        if (d2r > r2out)
        {
            continue;
        }
        row = cgl_canvas_row(c, (int16_t)(y + c->pan.y));
        if (row == NULL)
        {
            continue;
        }
        ro = (int32_t)cgl_math_isqrt(r2out - d2r);
        ri = (d2r <= r2in) ? (int32_t)cgl_math_isqrt(r2in - d2r) : -1;
        il = (ri >= 0) ? ri : 0;

        for (arm = 0; arm < 2; arm++)
        {
            int32_t xa = (arm == 0) ? ((int32_t)s->cx - ro) : ((int32_t)s->cx + il);
            int32_t xb = (arm == 0) ? ((int32_t)s->cx - il) : ((int32_t)s->cx + ro);
            int32_t x;

            if (xa < x0)
            {
                xa = x0;
            }
            if (xb > x1)
            {
                xb = x1;
            }
            for (x = xa; x <= xb; x++)
            {
                row[(int16_t)(x + c->pan.x)] = arc__surface_px(s, x - (int32_t)s->cx, dy);
            }
        }
    }
}

void cgl_arc_surface_paint(cgl_canvas_t *c, const cgl_arc_surface_t *s)
{
    if ((c == NULL) || (s == NULL))
    {
        return;
    }
    if (s->kind == (uint8_t)CGL_ARC_RING)
    {
        arc__paint_ring(c, s);
        return;
    }
    arc__paint_face(c, s);
}

/* ========================================================================== */
/* The ring gauge                                                             */
/* ========================================================================== */

/* rejected-range counter (observable, same pattern as cgl_bar_range_rejected) */
static uint32_t s_range_rejected = 0u;

uint32_t cgl_arc_range_rejected(void)
{
    return s_range_rejected;
}

/* --- the explicit sweep an owner asked for ---------------------------------
 *
 * `cgl_arc_gauge_set_sweep()` must survive until the next `set_value()`, and
 * `cgl_arc_gauge_t` has no field for it -- see the note at the top of this file,
 * which also names the one field that fixes it properly. Until then the state
 * lives here, keyed by the gauge's address.
 *
 * Why a table and not a sentinel in one of the gauge's own fields: `shown` and
 * `value` are the value machinery (`get_value()`, `get_shown_value()` and the
 * next `animate_to()` all read them back, so a marker written into one of them
 * comes out of an unrelated API as a wildly wrong number), and `anim`/`pulse` are
 * `cgl_anim_t` objects whose spare bits belong to `cgl_anim`, not to this file.
 * A table is at least honest about what it is: per-gauge state that has nowhere
 * else to live.
 *
 * `cgl_arc_gauge_init()` clears the entry for its address, which is what makes a
 * recycled stack address safe: a gauge that has just been created cannot inherit
 * the sweep of a dead one.
 *
 * WARNING: the capacity is a bound, not a promise. Four owner-driven gauges are
 * far more than any page in this project has (the owner use is "a watch face's
 * complication ring, driven from the clock"), and past it the **oldest** entry is
 * recycled -- the gauge its entry was taken from then falls back to the value
 * mapping, which is why the field named above is the real fix. */
#define ARC__DIRECT_SLOTS 4u

typedef struct
{
    const cgl_arc_gauge_t *owner;
    uint32_t sweep;
    uint32_t age; /**< bumped on every touch; the smallest one is recycled */
} arc__direct_t;

static arc__direct_t s_direct[ARC__DIRECT_SLOTS];
static uint32_t s_direct_age = 0u;

/** Forget any explicit sweep recorded for `g` (the value machinery is in charge
 *  again). */
static void arc__direct_clear(const cgl_arc_gauge_t *g)
{
    uint32_t i;

    for (i = 0u; i < ARC__DIRECT_SLOTS; i++)
    {
        if (s_direct[i].owner == g)
        {
            s_direct[i].owner = NULL;
            s_direct[i].sweep = 0u;
            s_direct[i].age = 0u;
            return;
        }
    }
}

/** Record `sweep` as the arc `g` draws, whatever its value maps to. */
static void arc__direct_set(const cgl_arc_gauge_t *g, uint32_t sweep)
{
    uint32_t i;
    uint32_t slot = ARC__DIRECT_SLOTS;
    uint32_t oldest = ARC__DIRECT_SLOTS;

    s_direct_age++;
    for (i = 0u; i < ARC__DIRECT_SLOTS; i++)
    {
        if (s_direct[i].owner == g)
        {
            s_direct[i].sweep = sweep;
            s_direct[i].age = s_direct_age;
            return;
        }
        if ((slot == ARC__DIRECT_SLOTS) && (s_direct[i].owner == NULL))
        {
            slot = i;
        }
        if ((oldest == ARC__DIRECT_SLOTS) ||
            ((s_direct[i].owner != NULL) && (s_direct[i].age < s_direct[oldest].age)))
        {
            oldest = i;
        }
    }
    if (slot == ARC__DIRECT_SLOTS)
    {
        slot = oldest; /* no free slot: see the WARNING above */
    }
    s_direct[slot].owner = g;
    s_direct[slot].sweep = sweep;
    s_direct[slot].age = s_direct_age;
}

/** The explicit sweep of `g`, when it has one. */
static bool arc__direct_get(const cgl_arc_gauge_t *g, uint32_t *sweep)
{
    uint32_t i;

    for (i = 0u; i < ARC__DIRECT_SLOTS; i++)
    {
        if (s_direct[i].owner == g)
        {
            s_direct[i].age = ++s_direct_age;
            *sweep = s_direct[i].sweep;
            return true;
        }
    }
    return false;
}

static int32_t arc__clamp(const cgl_arc_gauge_t *g, int32_t value)
{
    if (value < g->vmin)
    {
        return g->vmin;
    }
    if (value > g->vmax)
    {
        return g->vmax;
    }
    return value;
}

/** Put the drawn value exactly on `target` with no tween in flight, in a way that
 *  keeps `shown` and `anim.value` equal -- the invariant the rest of this file
 *  reads (`shown` is what a draw uses, `anim.value` is what a retarget continues
 *  from). */
static void arc__jump(cgl_arc_gauge_t *g, int32_t target)
{
    cgl_anim_start(&g->anim, target, target, 0u, CGL_EASE_LINEAR);
    cgl_anim_finish(&g->anim);
    g->shown = target;
}

uint32_t cgl_arc_gauge_sweep(const cgl_arc_gauge_t *g)
{
    uint32_t sweep;
    uint32_t direct;

    if (g == NULL)
    {
        return 0u;
    }
    if (arc__direct_get(g, &direct))
    {
        sweep = direct;
    }
    else
    {
        /* The value -> fraction mapping is `cgl_math_frac_q8()`, shared with
           `cgl_bar` so that a bar and a gauge can never disagree about where half
           way is. */
        uint16_t frac = cgl_math_frac_q8(g->shown, g->vmin, g->vmax);

        sweep = ((uint32_t)g->span * (uint32_t)frac) >> 8;
    }
    /* The reveal (`set_scale`) is applied **after** the mapping and as a separate
       shift on purpose: `span * frac * scale` overflows a uint32 for a span near a
       full turn and a fraction near 1.0, and the two-step form is exact for every
       input the three fields can hold. */
    sweep = (sweep * (uint32_t)g->scale_q8) >> 8;
    if (sweep > g->span)
    {
        sweep = g->span; /* an explicit sweep past the span is clamped to the arc */
    }
    return sweep;
}

cgl_point_t cgl_arc_gauge_tip(const cgl_arc_gauge_t *g)
{
    cgl_point_t p;
    uint32_t sweep;
    cgl_angle_t end;
    int16_t dx;
    int16_t dy;

    p.x = 0;
    p.y = 0;
    if (g == NULL)
    {
        return p;
    }
    p.x = (int16_t)(g->base.frame.x0 + g->cx_off);
    p.y = (int16_t)(g->base.frame.y0 + g->cy_off);
    sweep = cgl_arc_gauge_sweep(g);
    if (sweep == 0u)
    {
        return p; /* an empty arc changes nothing: its "end" is the centre */
    }
    /* Angles grow clockwise from +x (see cgl_math.h), so a clockwise sweep ends at
       `start + sweep` and an anticlockwise one at `start - sweep` -- the same two
       angles the arc is drawn from, which is what makes this the point the *change*
       happens at. */
    end = (g->dir == (uint8_t)CGL_ARC_CCW) ? (cgl_angle_t)(g->start - sweep)
                                           : (cgl_angle_t)(g->start + sweep);
    cgl_math_polar(end, g->radius, &dx, &dy);
    p.x = (int16_t)(p.x + dx);
    p.y = (int16_t)(p.y + dy);
    return p;
}

/** The gauge's whole picture: the track, then the ink, then the lead dot.
 *
 *  The track is drawn first and always -- a gauge does not paint its own
 *  background (the frame is the host's), but it does own the ring it lives on, and
 *  a track that was not redrawn would leave the previous frame's ink behind the
 *  moment the value drops. See the note on `cgl_arc_gauge_set_sweep()` for why the
 *  owner may be the one that damages this. */
static void arc__gauge_draw(cgl_widget_t *w, cgl_canvas_t *c)
{
    cgl_arc_gauge_t *g = (cgl_arc_gauge_t *)w;
    int16_t cx = (int16_t)(w->frame.x0 + g->cx_off);
    int16_t cy = (int16_t)(w->frame.y0 + g->cy_off);
    cgl_color_t track = g->track;
    cgl_angle_t start0;
    uint32_t sweep;

    if (g->pulse_q8 != 0u)
    {
        /* Half the pulse, exactly as the reference page tints its ring: the flash
           is a suggestion of light on the track, not a repaint of it. */
        track = cgl_color_blend_q8(g->track, g->pulse_to, (uint16_t)(g->pulse_q8 >> 1));
    }
    /* For CCW the *span* is the arc between `start` and `start - span`, so the
       track is drawn from the far end; the ink then grows from the far end towards
       `start`, which is what "anticlockwise from here" means on screen. */
    start0 = (g->dir == (uint8_t)CGL_ARC_CCW) ? (cgl_angle_t)(g->start - g->span) : g->start;
    cgl_canvas_arc(c, cx, cy, g->radius, g->thickness, start0, g->span, track);

    sweep = cgl_arc_gauge_sweep(g);
    if (sweep != 0u)
    {
        cgl_angle_t ink0 = (g->dir == (uint8_t)CGL_ARC_CCW) ? (cgl_angle_t)(g->start - sweep)
                                                            : start0;

        cgl_canvas_arc(c, cx, cy, g->radius, g->thickness, ink0, sweep, g->ink);
        if (g->dot_r != 0u)
        {
            /* The dot is the gauge's cheapest piece of "expensive looking" detail:
               it grows by `dot_grow` pixels over a full pulse, which reads as the
               ring taking a breath. `(pulse * grow) >> 8` is the reference page's
               `pulse >> 6` with the growth written as a knob. */
            cgl_point_t tip = cgl_arc_gauge_tip(g);

            cgl_canvas_disc(c, tip.x, tip.y,
                            (int16_t)((uint32_t)g->dot_r +
                                      (((uint32_t)g->pulse_q8 * (uint32_t)g->dot_grow) >> 8)),
                            g->dot);
        }
    }
}

static void arc__gauge_update(cgl_widget_t *w, uint32_t dt_ms)
{
    cgl_arc_gauge_t *g = (cgl_arc_gauge_t *)w;

    /* The tween returns true when the drawn value changed, which is exactly the
       condition under which the pixels can change. */
    if (cgl_anim_tick(&g->anim, dt_ms))
    {
        g->shown = g->anim.value;
        cgl_widget_invalidate(w);
    }
    if (cgl_anim_tick(&g->pulse, dt_ms))
    {
        /* Clamped into Q8 before the cast: `CGL_EASE_OUT_BACK` overshoots its
           target, and this tween runs **256 -> 0**, so the overshoot is *below*
           zero -- a negative value cast to uint16 would become a huge alpha and
           the track would come out of `cgl_color_blend_q8()` as noise. */
        int32_t pulse = cgl_anim_value(&g->pulse);

        if (pulse < 0)
        {
            pulse = 0;
        }
        else if (pulse > 256)
        {
            pulse = 256;
        }
        g->pulse_q8 = (uint16_t)pulse;
        cgl_widget_invalidate(w);
    }
}

static const cgl_widget_vt_t arc__gauge_vt = {
    /* Designated initialisers, never positional: `cgl_widget_vt_t` grows members as
       the framework evolves and positional initialisation silently shifts. */
    .type = "arc_gauge",       /* so a debugger can tell what widget this is */
    .draw = arc__gauge_draw,   /* called every frame (once per band)          */
    .update = arc__gauge_update, /* drives `_animate_to` and `_pulse`         */
    .event = NULL,             /* a gauge is an output only, it takes no input */
};

void cgl_arc_gauge_init(cgl_arc_gauge_t *g, const char *name, cgl_rect_t frame, int16_t radius,
                        int16_t thickness)
{
    if (g == NULL)
    {
        return;
    }
    cgl_widget_init(&g->base, &arc__gauge_vt, name, frame);
    cgl_anim_init(&g->anim);
    cgl_anim_init(&g->pulse);
    g->value = 0;
    g->shown = 0;
    g->vmin = 0;
    g->vmax = 100;
    /* "Centred on the frame" is the offset from the frame's origin to its middle,
       because the whole gauge moves with cgl_widget_set_frame(). */
    g->cx_off = (int16_t)(cgl_rect_width(frame) / 2);
    g->cy_off = (int16_t)(cgl_rect_height(frame) / 2);
    g->radius = radius;
    g->thickness = thickness;
    g->start = CGL_ANGLE_UP; /* 12 o'clock */
    g->span = CGL_ANGLE_TURN;
    g->scale_q8 = 256u; /* the whole sweep is revealed */
    g->dir = (uint8_t)CGL_ARC_CW;
    g->dot_r = 0u;
    g->dot_grow = 0u;
    g->pulse_q8 = 0u;

    /* The reference page's complication ring: a groove cut into the card, an
       orange arc and a lighter orange lead dot. They are the colours the demo has
       always used for exactly this widget, so "show me a gauge" looks like the
       reference without a palette being configured. */
    g->track = CGL_RGB565(16, 20, 27);
    g->ink = CGL_RGB565(255, 106, 0);
    g->dot = CGL_RGB565(255, 186, 92);
    g->pulse_to = CGL_RGB565(255, 186, 92);

    /* A recycled address must not inherit a dead gauge's explicit sweep: see the
       note on the direct-sweep table above. */
    arc__direct_clear(g);
}

bool cgl_arc_gauge_set_range(cgl_arc_gauge_t *g, int32_t vmin, int32_t vmax)
{
    int32_t target;

    if (g == NULL)
    {
        return false;
    }
    if (vmax <= vmin)
    {
        /* Rejected and counted rather than "fixed up": a gauge whose range is
           silently something else draws a plausible-looking wrong fraction, which
           is the hardest kind of bug to see. */
        s_range_rejected++;
        return false;
    }
    g->vmin = vmin;
    g->vmax = vmax;
    target = arc__clamp(g, g->value);
    g->value = target;
    arc__direct_clear(g);
    arc__jump(g, target);
    /* The same value draws at a different angle once the range changed, so a range
       change always damages, even when the value itself did not move. */
    cgl_widget_invalidate(&g->base);
    return true;
}

void cgl_arc_gauge_set_value(cgl_arc_gauge_t *g, int32_t value)
{
    int32_t target;
    uint32_t ignored;
    bool changed;

    if (g == NULL)
    {
        return;
    }
    target = arc__clamp(g, value);
    /* An explicit sweep is a *drawn* state this call ends, so clearing it can
       change the pixels even when the value does not move: that is why it is part
       of the damage decision below and not just of the state. */
    changed = (g->shown != target) || arc__direct_get(g, &ignored);
    g->value = target;
    arc__direct_clear(g);
    cgl_anim_stop(&g->anim);
    arc__jump(g, target);
    if (changed)
    {
        cgl_widget_invalidate(&g->base);
    }
}

bool cgl_arc_gauge_animate_to(cgl_arc_gauge_t *g, int32_t value, uint32_t dur_ms, cgl_ease_t ease)
{
    int32_t target;
    uint32_t ignored;
    bool was_direct;

    if (g == NULL)
    {
        return false;
    }
    target = arc__clamp(g, value);
    g->value = target;
    if ((dur_ms == 0u) || (target == g->shown))
    {
        /* Both cases mean "there is nothing to tween": fall through to the
           immediate path so the damage decision stays in exactly one place. */
        cgl_arc_gauge_set_value(g, target);
        return false;
    }
    /* A tween replaces the explicit sweep, and *that* is the one thing here that
       can change the pixels before the first tick: the drawn arc jumps from the
       owner's sweep to the value-mapped one it is about to tween away from. */
    was_direct = arc__direct_get(g, &ignored);
    arc__direct_clear(g);
    /* Start from what is drawn, not from the previous target: retargeting in
       mid-flight continues from the current pixel instead of jumping back. */
    cgl_anim_start(&g->anim, g->shown, target, dur_ms, ease);
    if (was_direct)
    {
        cgl_widget_invalidate(&g->base);
    }
    return true;
}

void cgl_arc_gauge_set_span(cgl_arc_gauge_t *g, cgl_angle_t start, uint32_t span,
                            cgl_arc_dir_t dir)
{
    uint8_t d = (uint8_t)((dir == CGL_ARC_CCW) ? CGL_ARC_CCW : CGL_ARC_CW);

    if (g == NULL)
    {
        return;
    }
    if ((g->start == start) && (g->span == span) && (g->dir == d))
    {
        return;
    }
    g->start = start;
    g->span = span;
    g->dir = d;
    arc__direct_clear(g);
    cgl_widget_invalidate(&g->base);
}

void cgl_arc_gauge_set_colors(cgl_arc_gauge_t *g, cgl_color_t track, cgl_color_t ink)
{
    if (g == NULL)
    {
        return;
    }
    if ((g->track == track) && (g->ink == ink))
    {
        return;
    }
    g->track = track;
    g->ink = ink;
    cgl_widget_invalidate(&g->base);
}

void cgl_arc_gauge_set_pulse_color(cgl_arc_gauge_t *g, cgl_color_t color)
{
    if (g == NULL)
    {
        return;
    }
    if (g->pulse_to == color)
    {
        return;
    }
    g->pulse_to = color;
    cgl_widget_invalidate(&g->base);
}

void cgl_arc_gauge_set_dot(cgl_arc_gauge_t *g, uint8_t r, uint8_t grow, cgl_color_t color)
{
    if (g == NULL)
    {
        return;
    }
    if ((g->dot_r == r) && (g->dot_grow == grow) && (g->dot == color))
    {
        return;
    }
    g->dot_r = r;
    g->dot_grow = grow;
    g->dot = color;
    cgl_widget_invalidate(&g->base);
}

void cgl_arc_gauge_set_sweep(cgl_arc_gauge_t *g, uint32_t sweep)
{
    if (g == NULL)
    {
        return;
    }
    /* The owner's door: no damage, deliberately. A watch face whose ring is driven
       by the clock knows which bands the leading end touched, and having the widget
       union its own 64 x 64 box into every frame's damage is the difference between
       22 bands pushed per frame and 39 (see the note on the two families of setters
       in cgl_arc.h). */
    arc__direct_set(g, sweep);
}

void cgl_arc_gauge_set_scale(cgl_arc_gauge_t *g, uint16_t q8)
{
    if (g == NULL)
    {
        return;
    }
    /* No damage: the owner's door, like `_set_sweep()`. The scale is the "the ring
       fills as it appears" animation, and whoever runs that reveal is by definition
       already damaging what it reveals. */
    g->scale_q8 = q8;
}

void cgl_arc_gauge_pulse(cgl_arc_gauge_t *g, uint32_t dur_ms, cgl_ease_t ease)
{
    if (g == NULL)
    {
        return;
    }
    /* **256 -> 0**, not 0 -> 256, and the direction is the whole point: a pulse's
       resting value has to be the "nothing is happening" value, or every draw that
       multiplies by it stays tinted for the rest of the day after the first flash.
       (Measured on the reference page before the fix: the ring's track stayed half
       way blended toward the accent for ever.) */
    cgl_anim_start(&g->pulse, 256, 0, dur_ms, ease);
}

void cgl_arc_gauge_set_pulse(cgl_arc_gauge_t *g, uint16_t q8)
{
    if (g == NULL)
    {
        return;
    }
    /* An owner that keeps the flash in its own timeline (the watch face tints the
       hands with the same number) sets the value directly: the tween would fight
       it, so it is stopped. No damage -- the owner damages. */
    cgl_anim_stop(&g->pulse);
    g->pulse_q8 = q8;
}

void cgl_arc_gauge_set_center(cgl_arc_gauge_t *g, int16_t cx_off, int16_t cy_off)
{
    if (g == NULL)
    {
        return;
    }
    if ((g->cx_off == cx_off) && (g->cy_off == cy_off))
    {
        return;
    }
    g->cx_off = cx_off;
    g->cy_off = cy_off;
    /* The whole arc moved, and the widget only knows its own frame -- which is
       where the gauge is drawn, so it is the right rectangle to report. */
    cgl_widget_invalidate(&g->base);
}

int32_t cgl_arc_gauge_get_value(const cgl_arc_gauge_t *g)
{
    return (g != NULL) ? g->value : 0;
}

int32_t cgl_arc_gauge_get_shown_value(const cgl_arc_gauge_t *g)
{
    return (g != NULL) ? g->shown : 0;
}

bool cgl_arc_gauge_is_animating(const cgl_arc_gauge_t *g)
{
    return (g != NULL) && cgl_anim_running(&g->anim);
}

#endif /* CGL_CFG_ARC */
