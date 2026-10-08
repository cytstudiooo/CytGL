// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_arc.h
  * @brief   LCD UI framework - the radial toolkit: a shaded circular surface, a
  *          tapered antialiased blade, and the ring-gauge widget.
  *
  *  Why this module exists
  *  ----------------------
  *  `cgl_canvas.h` has the three radial **primitives** (circle, thick line, arc)
  *  and `cgl_math.h` has the trigonometry. That pair is enough to draw a circle,
  *  and it is not enough to draw a *dial*: everything a watch face, a speedometer
  *  or a radar sweep is actually made of turned out to be one of three things, and
  *  until this module existed each of them was written out by hand inside the demo
  *  page -- 2 500 lines of `examples/common/App/app_page_clock.c`, of which this
  *  module is the reusable half.
  *
  *    1. **A shaded radial surface**: "one colour per pixel, out of a table".
  *       The dial's face (a radial gradient, a brushed texture and a specular
  *       highlight) and the ring the dial is set into (a cylindrical
  *       cross-section, shaded top-dark / bottom-bright) are both this, and they
  *       are the same code with different parameters. A rectangle of flat colour
  *       is a panel; a *disc* of flat colour is a sticker, because the eye reads
  *       a surface by how it is lit across its radius.
  *    2. **A tapered, antialiased blade**: a watch hand, a needle, a comet tail.
  *       `cgl_canvas_line_thick()` draws a *constant* width capsule, so a tapered
  *       hand can only be faked out of several segments -- and the seams between
  *       them bulge, which is the one thing a hand must not do. A
  *       `cgl_arc_stroke_t` is a single quadrilateral whose half width runs
  *       linearly from one end to the other, evaluated per pixel from two dot
  *       products: nothing is stamped, so there is no seam to hide.
  *    3. **A ring gauge**: "how far along is this value between two limits",
  *       drawn as an arc -- the radial twin of `cgl_bar`, and the third built-in
  *       widget.
  *
  *  The three share one property that is worth more than any of them: **each is a
  *  pure function of its inputs**. A surface is `f(dx, dy)`; a stroke is
  *  `f(geometry)`; a gauge is `f(value, range, geometry)`. Nothing is stored
  *  between frames, nothing accumulates, and the same call draws the same pixels
  *  for ever. That is what lets a test assert the *design* ("the centre is
  *  brighter than the rim", "the ring is dark at the top and bright at the
  *  bottom") rather than photograph it -- see `cgl_arc_surface_color()` and
  *  `tests/watch_host_test.c`.
  *
  *  What the surface is, exactly
  *  ----------------------------
  *  A surface is a **table plus three per-pixel terms**, and which is which is the
  *  whole design:
  *
  *      colour = lut[index(r^2)]                  the gradient and the circular
  *                                                brushing, baked in flash or RAM
  *             -> sunburst(direction)             per pixel: only a *direction*
  *                                                can separate one ray from the
  *                                                next, and a table indexed by
  *                                                r^2 cannot carry a direction
  *             -> specular / tilt(dx, dy)         per pixel, signed
  *
  *  The table is indexed by `r^2` and not by `r` because the inner loop knows
  *  `dx*dx + dy*dy` and nothing else: taking a square root per pixel to index by
  *  radius would cost more than the whole rest of the surface (see the note on
  *  isqrt in cgl_math.h). The table is *built* at the radius each entry really
  *  stands for, so the curve drawn is the curve designed.
  *
  *  `lut == NULL` is legal and means "shade procedurally": the gradient is
  *  evaluated per pixel instead of looked up. That costs one `isqrt` and a blend
  *  per pixel (roughly three times the table path) and **zero bytes of flash**,
  *  which is the right trade for a small or rarely drawn surface.
  *
  *  This module depends on cgl_canvas / cgl_math / cgl_widget / cgl_anim and on
  *  nothing else. It allocates nothing: every table is the caller's, statically or
  *  on the stack.
  ******************************************************************************
  */
#ifndef CGL_ARC_H
#define CGL_ARC_H

#include <cytgl/cytgl_config.h>
#include <cytgl/cgl_widget.h>
#include <cytgl/cgl_anim.h>

#if CGL_CFG_ARC

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Coverage: the one opinion about what "half covered" means                  */
/* -------------------------------------------------------------------------- */

/** Entries in the coverage ramp, at 1/64 px per entry: 1 px inside to 1 px outside. */
#define CGL_ARC_COV_N 128

/**
 * Coverage, Q8 (0..256), of a pixel whose **signed distance inside the shape** is
 * `d_q6` in Q6 pixels: positive inside, negative outside, 0 exactly on the edge.
 *
 * This is a one pixel box filter sampled out of a 128 entry table in flash -- 256
 * levels of coverage over 64 entries -- so antialiasing a blade costs neither a
 * square root nor a multiply. It is deliberately the **same** ramp the framework's
 * own antialiased strokes use (see `canvas__cov` in cgl_canvas.c): a hand drawn
 * with `cgl_arc_stroke_draw()` and a ring drawn with `cgl_canvas_arc()` have to
 * have the same edge softness, or the hands read as out of focus next to
 * everything else on the dial.
 *
 * A caller with its own rasteriser is welcome to it; that is why it is public.
 */
uint16_t cgl_arc_cov_q6(int32_t d_q6);

/* -------------------------------------------------------------------------- */
/* The tapered blade                                                          */
/* -------------------------------------------------------------------------- */

/**
 * A blade's geometry, in **Q6 pixels** (1/64 px), and the sub-pixel part is not
 * decoration: a second hand sweeps 6 degrees a second, which is 7 px at its tip,
 * i.e. 0.12 px in a 16 ms frame. Whole pixel endpoints would hold still for eight
 * frames and then jump -- exactly the stutter the sweep exists to avoid. So a
 * caller that wants a smooth hand hands over `cgl_math_polar_q6()`'s output and
 * lets the coverage ramp do the rest.
 *
 * `wb`/`wt` are the **half** widths at the two ends, so the shape is a tapered
 * quadrilateral; `dw_q12` is "how much the half width grows per unit of length",
 * pre-divided so that the per-pixel taper is one multiply.
 *
 * The fields are public because a widget may want to keep a prepared stroke
 * around (a needle that is only re-aimed each frame can re-`setup` and skip the
 * rest); treat them as read-only and build them with `cgl_arc_stroke_setup()`.
 */
typedef struct
{
    int32_t bx_q6;  /**< the base end (the arbor side)               */
    int32_t by_q6;
    int32_t ax_q6;  /**< the axis, base -> tip                       */
    int32_t ay_q6;
    int32_t ux_q16; /**< the axis direction, normalised, Q16         */
    int32_t uy_q16;
    int32_t len_q6; /**< the length along the axis, Q6               */
    int32_t wb_q6;  /**< half width at the base, Q6                  */
    int32_t wt_q6;  /**< half width at the tip, Q6                   */
    int32_t dw_q12; /**< (wt - wb) / length, Q12                     */
} cgl_arc_stroke_t;

/**
 * Work out a blade's normalised axis and taper from its two endpoints in Q6 and
 * its two full widths in whole pixels.
 *
 * @return false for a zero length blade (a dot is not a hand; draw it with
 *         `cgl_arc` on radius 0 or `cgl_canvas_disc` instead), in which case `g`
 *         is left untouched and the caller must not draw with it.
 */
bool cgl_arc_stroke_setup(cgl_arc_stroke_t *g, int32_t bx_q6, int32_t by_q6, int32_t tx_q6,
                          int32_t ty_q6, int16_t w_base, int16_t w_tip);

/**
 * Rasterise one blade: the tapered quad of `g`, shifted by (ox, oy) pixels and
 * blended at `alpha_q8` of full coverage.
 *
 * The shift exists so that a caller can lay a drop shadow down first
 * (`cgl_arc_needle()` does exactly that) without rebuilding the geometry.
 *
 * Cost is bounded by walking the blade's bounding box **row by row, narrowed to
 * the strip the axis crosses in that row**, and then clamped to the band's source
 * window: a diagonal blade costs a dozen pixels per row instead of sweeping the
 * 150 x 150 box its endpoints imply.
 *
 * WARNING: the clamp to `cgl_canvas_src_window()` is not an optimisation, it is
 * what makes a blade affordable at all. A widget is drawn **once per band**, so a
 * rasteriser that walks its whole bounding box in every band pays 43 times for one
 * picture: measured on the reference panel, that was two thirds of a
 * full-repaint frame and the reason the watch face stuttered during a page
 * transition. And under a warp the band's pre-image is the only bound that is both
 * correct and finite -- see the note on `cgl_canvas_src_window()`.
 */
void cgl_arc_stroke_draw(cgl_canvas_t *c, const cgl_arc_stroke_t *g, cgl_color_t col,
                         uint16_t alpha_q8, int16_t ox, int16_t oy);

/**
 * One complete hand: its shadow, then its ink, laid out at an angle around (cx, cy).
 *
 * `r_base` is how far the blade reaches back **behind** the centre (where the hub
 * covers it) and `r_tip` how far it points; the widths are full widths in whole
 * pixels. The shadow is drawn first and *under* the ink, offset down and to the
 * right by `shadow_off`, and it is a **blend** rather than a second dark hand for a
 * reason worth stating: 30 % of black over the dial *and over whatever the hand
 * crosses* is what makes the hand look like it is floating a millimetre above the
 * face, while a solid dark outline would look like a sticker.
 *
 * `shadow_alpha == 0` or `shadow_off == 0` with a zero alpha simply draws no
 * shadow; there is no separate switch.
 *
 * @param tip   optional out: the tip in whole pixels, which is what a damage model
 *              needs (see `cgl_arc_needle_damage()`). May be NULL.
 */
void cgl_arc_needle(cgl_canvas_t *c, int16_t cx, int16_t cy, cgl_angle_t a, int16_t r_base,
                    int16_t r_tip, int16_t w_base, int16_t w_tip, cgl_color_t col,
                    cgl_color_t shadow_col, uint16_t shadow_alpha, int16_t shadow_off,
                    cgl_point_t *tip);

/**
 * The bounding box a needle of this shape covers, in screen coordinates: the tip's
 * and the tail's extent plus the pen's half width, the antialiasing ramp and the
 * shadow's offset.
 *
 * WARNING: forgetting `shadow_off` here leaves a two pixel ghost of the shadow
 * behind a hand that has moved -- which is precisely the bug the watch page's
 * no-ghost test exists to catch.
 */
cgl_rect_t cgl_arc_needle_damage(cgl_rect_t dmg, int16_t cx, int16_t cy, cgl_angle_t a,
                                 int16_t r_base, int16_t r_tip, int16_t half_w,
                                 int16_t shadow_off);

/* -------------------------------------------------------------------------- */
/* Drawing a curve with transparency                                          */
/* -------------------------------------------------------------------------- */

/*
 * Where transparency lives, and why it is not an argument on the canvas primitives
 *
 * The opacity of a pixel is a *product* of two independent facts: how much of it the
 * shape covers (which the rasteriser works out per pixel) and how opaque the ink is
 * meant to be (which is the caller's decision). The framework already has the first;
 * `cgl_canvas_set_ink_alpha()` is the second, and it is applied at the one place the
 * two meet -- the store -- so it covers every blended primitive at once
 * (`cgl_canvas_arc`, `_circle`, `_line_thick`, `cgl_canvas_pixel_blend_q8`) rather
 * than only the handful of them that grew a parameter.
 *
 * The wrappers below add the part a caller should not have to remember: set the ink
 * alpha, draw **one** shape, put it back. Ink alpha is canvas *state*, so without that
 * it is only a matter of time before one translucent ring leaves every later element
 * on the page faint -- a bug that looks like a palette problem and is not one.
 *
 * The `alpha_q8` arguments are 0 (invisible) .. 256 (`CGL_CANVAS_INK_OPAQUE`, fully
 * opaque); anything above 256 is clamped, and `alpha_q8 == 256` is the zero-cost path
 * (one comparison per pixel, no multiply). The edge stays antialiased at every alpha,
 * because what is scaled is the coverage, not the geometry: `cgl_arc_draw_ring(...,
 * 128)` is the same ring at half opacity, not a thinner ring.
 *
 * WARNING: none of these paints a background. Drawing a ring at 40 % over whatever is
 * already in the band is exactly the point; drawing one over *nothing* gives 40 % of
 * the ink over whatever the band happened to hold.
 */

/** A thick arc, drawn at `alpha_q8`. `cgl_canvas_arc()` under the hood. */
void cgl_arc_draw_arc(cgl_canvas_t *c, int16_t cx, int16_t cy, int16_t radius, int16_t thickness,
                      cgl_angle_t start, uint32_t sweep, cgl_color_t color, uint16_t alpha_q8);

/** A whole ring: `cgl_arc_draw_arc()` with a full turn, which is both the common case
    (a gauge's track) and the cheapest, since a full turn needs no angular clip. */
void cgl_arc_draw_ring(cgl_canvas_t *c, int16_t cx, int16_t cy, int16_t radius, int16_t thickness,
                       cgl_color_t color, uint16_t alpha_q8);

/** A one pixel circle outline, antialiased, at `alpha_q8` (`cgl_canvas_circle()`). */
void cgl_arc_draw_circle(cgl_canvas_t *c, int16_t cx, int16_t cy, int16_t radius, cgl_color_t color,
                         uint16_t alpha_q8);

/**
 * A filled **antialiased** disc at `alpha_q8`.
 *
 * WARNING: this is deliberately not `cgl_canvas_disc()`. That one is a *block*
 * primitive -- it fills whole rows, has no coverage ramp and therefore cannot be
 * translucent at all -- while a disc that can be is the radius-zero case of the arc
 * primitive (a stroke of width 2r on a radius of 0), which is antialiased and does go
 * through the blend. The two are the same picture at alpha 256, to within the edge.
 */
void cgl_arc_draw_disc(cgl_canvas_t *c, int16_t cx, int16_t cy, int16_t radius, cgl_color_t color,
                       uint16_t alpha_q8);

/** A thick line with round caps, at `alpha_q8` (`cgl_canvas_line_thick()`).
    WARNING: `thickness <= 1` takes the plain Bresenham path inside the canvas, and that
    one is a byte store -- a translucent hairline is what `cgl_canvas_pixel_blend_q8()`
    is for. */
void cgl_arc_draw_line(cgl_canvas_t *c, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                       int16_t thickness, cgl_color_t color, uint16_t alpha_q8);

/* -------------------------------------------------------------------------- */
/* The shaded radial surface                                                  */
/* -------------------------------------------------------------------------- */

/** Which of the two surfaces a `cgl_arc_surface_t` describes. */
typedef enum
{
    /** A disc: a radial gradient lit from one side, with a per-pixel sunburst and
        a specular highlight. The dial's face. */
    CGL_ARC_FACE = 0,
    /** An annulus: a cylindrical cross-section sampled from the inner radius to
        the outer one, shaded top-dark / bottom-bright because it is *sunk* where
        the face is *domed*. The ring a dial is set into. */
    CGL_ARC_RING = 1
} cgl_arc_surface_kind_t;

/** Default table resolution of a face: 256 entries, uniform in `r^2`. */
#define CGL_ARC_FACE_LUT_N 256

/** Default table resolution of a ring's cross-section. It is small because the
    profile is 8 px of radius: 33 entries already give the lit crown its own value
    per entry, and a finer table would only lengthen the bake. */
#define CGL_ARC_RING_LUT_N 33

/**
 * A shaded circular surface, as data.
 *
 * Every field is a knob a designer would want; the defaults
 * (`cgl_arc_surface_default_face()` / `_ring()`) are a dark watch dial and its
 * machined well, tuned against a 5-6-5 panel where a gradient over more than a
 * handful of levels comes out in visible bands.
 *
 * WARNING: `cx`/`cy` are **primitive** coordinates, the same space
 * `cgl_canvas_arc()` takes -- the canvas adds the pan, not this. A surface asked
 * to paint a band that holds none of it is a no-op.
 */
typedef struct
{
    uint8_t kind;     /**< cgl_arc_surface_kind_t                              */
    int16_t cx;       /**< centre, primitive coordinates                       */
    int16_t cy;
    int16_t r_in;     /**< RING: inner radius. FACE: ignored                   */
    int16_t r_out;    /**< FACE: the face radius. RING: outer radius           */

    /* --- the gradient the table (or the procedural path) is built from ------
     *
     * FACE: `c_hi -> c_mid` out to `knee_q8`, then `c_mid -> c_lo` out to the rim.
     * RING: `c_lo -> c_hi` across the cross-section, a half sine, so the surface
     *       turns away from the eye at both edges and is brightest in the middle.
     * `c_lo` is also the flat colour the warp fallback paints with, which is why
     * it is the "edge" stop in both readings. */
    cgl_color_t c_hi;
    cgl_color_t c_mid;
    cgl_color_t c_lo;
    uint8_t knee_q8;  /**< FACE: where the gradient turns over, Q8 of the radius */

    /* --- the baked texture -------------------------------------------------- */
    cgl_color_t tex;      /**< FACE: a brushed ridge catching the light        */
    uint8_t tex_amp;      /**< FACE: how strong it gets at the rim, Q8         */
    uint8_t tex_density;  /**< FACE: ridges over the radius                    */

    /* --- the per-pixel sunburst (FACE only) --------------------------------- */
    uint8_t ray_amp;  /**< how strong the lit rays get at the rim, Q8; 0 = off  */
    uint16_t ray_min; /**< first table entry a ray may appear at                */

    /* --- the per-pixel light (FACE: the specular) --------------------------- */
    cgl_color_t spec;      /**< the highlight's colour                       */
    int16_t light_dx;      /**< where the light lands, offset from the centre */
    int16_t light_dy;
    int16_t light_r;       /**< the highlight's radius                       */
    uint8_t light_alpha;   /**< its peak opacity, Q8                         */

    /* --- the per-pixel light (RING: the well's tilt) ------------------------ */
    cgl_color_t tilt_dark;  /**< the wall facing away from the light         */
    cgl_color_t tilt_light; /**< the wall facing it                          */
    uint8_t tilt;           /**< how strong the tilt gets, Q8                */

    /* --- the table ----------------------------------------------------------
     * NULL = shade procedurally (no flash, more CPU). `lut_n` is the resolution;
     * 0 means "the default for this kind", which is what the two `_default()`
     * functions install. */
    const uint16_t *lut;
    uint16_t lut_n;
} cgl_arc_surface_t;

/** Fill `s` with the reference dial face's parameters (radius left at 0 for the
    caller to set, and `lut` left NULL). */
void cgl_arc_surface_default_face(cgl_arc_surface_t *s);

/** Fill `s` with the reference recessed ring's parameters (`lut` left NULL). */
void cgl_arc_surface_default_ring(cgl_arc_surface_t *s);

/**
 * How many entries this surface's table has: `lut_n` when it is non-zero,
 * otherwise the default for its kind.
 */
uint16_t cgl_arc_surface_lut_entries(const cgl_arc_surface_t *s);

/**
 * Bake `n` entries of the surface's table into `lut`, which must hold at least `n`
 * entries. Nothing is stored in `s`: the caller assigns
 *
 *      cgl_arc_surface_bake(&s, table, CGL_ARC_FACE_LUT_N);
 *      s.lut = table;  s.lut_n = CGL_ARC_FACE_LUT_N;
 *
 * which keeps the ownership question ("flash? RAM? who frees it?") in the caller's
 * hands where it belongs -- this framework has no heap and never will.
 *
 * The bake is exact in the sense that matters: entry `i` is the colour of the
 * **real** radius it stands for, `sqrt(i / (n-1))` of the surface, so a gradient
 * built on a linear `r^2` ramp still draws the curve that was designed rather than
 * the one a squared radius happens to describe. That is one square root per
 * *entry*, not per pixel.
 *
 * Both the flash tables of the watch module and any table a caller wants are
 * produced by this one function, so "the table drifted from its generator" is not
 * a class of bug this framework can have.
 */
void cgl_arc_surface_bake(const cgl_arc_surface_t *s, uint16_t *lut, uint16_t n);

/**
 * The surface's colour at an offset from its centre: **this function is the whole
 * surface** -- gradient, texture, sunburst and highlight together -- and it is
 * pure: same offset, same colour, for ever, with no state read and none written.
 *
 * It answers for any offset, on the surface or not; the caller decides whether the
 * pixel is on it (`cgl_arc_surface_paint()` does). Safe to call before anything
 * has been drawn, which is what makes it usable as a test oracle.
 */
cgl_color_t cgl_arc_surface_color(const cgl_arc_surface_t *s, int16_t dx, int16_t dy);

/**
 * Paint the surface into the current band -- the intersection of the surface, the
 * clip window and the band, straight into the row the canvas hands back.
 *
 * The row span is `sqrt(R^2 - dy^2)` per row, i.e. one `isqrt` per *row* and not
 * one per pixel, and the interior is written opaquely with no blend: for a surface
 * that is by definition "the thing underneath", that is the cheap and the correct
 * path.
 *
 * WARNING: **a warp is not supported here, deliberately**. During a suction
 * transition the clip is in destination space and a source row far outside the band
 * can still land inside it, so a rasteriser that writes rows itself cannot say
 * which destination row it is writing to. The framework's scanline primitives
 * answer that with the band's pre-image and a per-pixel blend; here every pixel is
 * a *different* colour, so the same choice would be ~900 000 blended calls per
 * frame for a face being sucked into a corner. The warp gets a flat disc (FACE) or
 * a flat annulus (RING) in the surface's edge colour instead -- a gradient nobody
 * can see is not worth a frame rate.
 */
void cgl_arc_surface_paint(cgl_canvas_t *c, const cgl_arc_surface_t *s);

/* -------------------------------------------------------------------------- */
/* The ring gauge                                                             */
/* -------------------------------------------------------------------------- */

/** Which way round the sweep grows from the start angle. */
typedef enum
{
    CGL_ARC_CW = 0, /**< clockwise on screen (the way a clock runs)             */
    CGL_ARC_CCW = 1 /**< anticlockwise                                          */
} cgl_arc_dir_t;

/**
 * A ring gauge: a value between two limits, drawn as a sweep along an arc, with an
 * optional lead dot and an optional flash.
 *
 * It is the radial twin of `cgl_bar` and it deliberately has the same bones -- an
 * arbitrary signed range, an optional tween (`cgl_arc_gauge_animate_to()`), a
 * rejected range that is counted rather than silently turned into something
 * arbitrary. What it adds is the two things only a radial gauge can have: an
 * arbitrary **span** (a 270 degree speedometer, a 120 degree thermometer), and a
 * **lead dot** at the growing end.
 *
 * The centre is stored as an offset from the widget frame's origin, so the whole
 * gauge moves with `cgl_widget_set_frame()` like any other widget.
 *
 * Two families of setters, and the difference is deliberate
 * ---------------------------------------------------------
 * `cgl_arc_gauge_set_value()` and friends mark damage, exactly like `cgl_bar`'s
 * setters: a standalone gauge must repaint itself, because nobody else will.
 *
 * `cgl_arc_gauge_set_sweep()`, `_set_scale()` and `_set_pulse()` do **not**, and
 * they are the doors an **owner** uses: a watch face whose sweep is derived from
 * the clock, and whose damage model already knows which bands the ring's leading
 * end touched, must not have a widget union a 64 x 64 box into every frame's
 * damage -- that is the difference between 22 bands pushed per frame and 39. The
 * page marks the damage; the gauge just draws.
 */
typedef struct
{
    cgl_widget_t base; /**< <- must be first, see the CytGL manual, section 16.1 */

    cgl_anim_t anim;  /**< the value tween (`_animate_to`)              */
    cgl_anim_t pulse; /**< the 256 -> 0 flash tween (`_pulse`)           */

    int32_t value; /**< the logical (requested) value, clamped into the range */
    int32_t shown; /**< what is drawn (equal to `value` when not animating)   */
    int32_t vmin;  /**< low end of the range                           */
    int32_t vmax;  /**< high end of the range                          */

    int16_t cx_off; /**< centre, as an offset from the widget frame's origin */
    int16_t cy_off;
    int16_t radius;    /**< radius of the arc's centre line              */
    int16_t thickness; /**< stroke width in pixels                       */

    cgl_angle_t start; /**< where a full sweep begins (`CGL_ANGLE_UP` = 12 o'clock) */
    uint32_t span;     /**< angle units covered by a full range (`CGL_ANGLE_TURN` = a ring) */
    uint16_t scale_q8; /**< how much of the sweep to draw, Q8: the "reveal"  */
    uint8_t dir;       /**< cgl_arc_dir_t                                 */
    uint8_t dot_r;     /**< the lead dot's radius at rest; 0 = no dot     */
    uint8_t dot_grow;  /**< how many pixels it gains over a full pulse    */
    uint16_t pulse_q8; /**< the flash, Q8 (256 = at its peak)             */

    cgl_color_t track;    /**< the unfilled arc                        */
    cgl_color_t ink;      /**< the completed part                      */
    cgl_color_t dot;      /**< the lead dot                            */
    cgl_color_t pulse_to; /**< what the track blends toward while it flashes */

    /**
     * The **explicit sweep** (`cgl_arc_gauge_set_sweep()`), and whether it is in
     * force.
     *
     * It has to be a field and not a sentinel in one of the fields above: `value`
     * and `shown` are read back by `get_value()` / `get_shown_value()` and by the
     * next `animate_to()`, and `anim`/`pulse` are `cgl_anim_t` objects whose spare
     * bits belong to `cgl_anim`. `cgl_arc_gauge_init()` clears `direct`, so a
     * recycled stack address cannot inherit a dead gauge's sweep; `set_value()`,
     * `animate_to()`, `set_range()` and `set_span()` clear it too, because each of
     * them puts the value machinery back in charge.
     */
    uint32_t direct_sweep; /**< the arc to draw, in angle units, when `direct` is set */
    uint8_t direct;        /**< 0 = the value maps to the sweep; 1 = `direct_sweep`  */
} cgl_arc_gauge_t;

/**
 * Create a gauge centred on `frame`, covering the whole ring
 * (`start = CGL_ANGLE_UP`, `span = CGL_ANGLE_TURN`, clockwise) with the range
 * `0..100`, value 0, no dot and no flash.
 *
 * WARNING: a gauge does **not** paint its own background -- the ring's empty track
 * is drawn, but nothing fills the frame. A gauge is meant to sit on a dial, a card
 * or a panel that has already been painted; give it a `cgl_panel` parent if the
 * area behind it has to be opaque.
 */
void cgl_arc_gauge_init(cgl_arc_gauge_t *g, const char *name, cgl_rect_t frame, int16_t radius,
                        int16_t thickness);

/**
 * Set the range. `vmax` must be strictly greater than `vmin`; a degenerate or
 * inverted range is **rejected** (returns false, counts in `cgl_arc_range_rejected()`
 * and leaves the old range in place) rather than silently turned into something
 * arbitrary. The current value is re-clamped and the gauge repainted, because the
 * same value draws at a different angle once the range changes.
 */
bool cgl_arc_gauge_set_range(cgl_arc_gauge_t *g, int32_t vmin, int32_t vmax);

/**
 * Set the value and draw it **immediately** (any running tween is snapped to the
 * new value). Clamped into the range, and repainted only when the drawn pixels can
 * really change.
 */
void cgl_arc_gauge_set_value(cgl_arc_gauge_t *g, int32_t value);

/**
 * Move to `value` over `dur_ms`, using the tween in `g->anim`. The gauge advances
 * itself from its own `update` callback, so the caller only has to keep calling
 * `cgl_widget_update_tree()` -- which `cgl_screen_frame()` already does once per
 * frame.
 *
 * The tween starts from **what is currently drawn**, not from the previous target,
 * so retargeting mid-flight is smooth rather than a jump back. `dur_ms == 0` means
 * "no animation" and degrades to `cgl_arc_gauge_set_value()`.
 *
 * @return true when a tween was really started.
 */
bool cgl_arc_gauge_animate_to(cgl_arc_gauge_t *g, int32_t value, uint32_t dur_ms,
                              cgl_ease_t ease);

/**
 * Where the sweep starts, how much of a turn a full range covers, and which way it
 * grows. `start` is an absolute angle (`CGL_ANGLE_UP` is 12 o'clock in this
 * convention); a speedometer that opens at 225 degrees and closes at 315 is
 * `start = CGL_ANGLE_DEG(135)`, `span = CGL_ANGLE_DEG(270)`, `CGL_ARC_CW`.
 */
void cgl_arc_gauge_set_span(cgl_arc_gauge_t *g, cgl_angle_t start, uint32_t span,
                            cgl_arc_dir_t dir);

/** Replace the track and ink colours. Repaints only when one really changed. */
void cgl_arc_gauge_set_colors(cgl_arc_gauge_t *g, cgl_color_t track, cgl_color_t ink);

/** What the track blends toward while a pulse is running (see
    `cgl_arc_gauge_pulse()`). Repaints only when it really changed. */
void cgl_arc_gauge_set_pulse_color(cgl_arc_gauge_t *g, cgl_color_t color);

/**
 * Draw a lead dot of radius `r` pixels on the gauge's leading end, growing by
 * `grow` pixels over a full pulse. `r == 0` (the default) removes it.
 */
void cgl_arc_gauge_set_dot(cgl_arc_gauge_t *g, uint8_t r, uint8_t grow, cgl_color_t color);

/**
 * Set the sweep **directly**, in angle units, bypassing the value -> fraction
 * mapping until the next `set_value()`.
 *
 * This is the owner's door: a watch face whose ring is driven by the clock says
 * "this is the arc" and manages its own damage (see the note on the two families
 * of setters above). **It marks no damage**, on purpose.
 */
void cgl_arc_gauge_set_sweep(cgl_arc_gauge_t *g, uint32_t sweep);

/** How far along the drawn sweep is right now, in angle units (0..span). */
uint32_t cgl_arc_gauge_sweep(const cgl_arc_gauge_t *g);

/**
 * Draw only `q8/256` of the current sweep: the "the ring fills as it appears"
 * animation. 256 (the default) is the whole arc; 0 draws the track alone (the lead
 * dot is suppressed at 0, because it would otherwise sit alone at the start angle
 * and read as a stray dot).
 *
 * **Marks no damage** -- see the note on the two families of setters.
 */
void cgl_arc_gauge_set_scale(cgl_arc_gauge_t *g, uint16_t q8);

/**
 * Start a flash: the track blends toward `pulse_to` and the lead dot grows, both
 * decaying from 256 to 0 over `dur_ms`. The tween runs from the gauge's own
 * `update`, which also marks the damage while it runs.
 *
 * WARNING: the tween runs **256 -> 0**, not 0 -> 256, and the direction is the
 * whole point: a pulse's resting value has to be the "nothing is happening" value,
 * or every draw that multiplies by it stays tinted for the rest of the day after
 * the first flash. (Measured on the reference page before the fix: the ring's
 * track stayed half way blended toward the accent for ever.)
 *
 * An owner that keeps the pulse in its own timeline (the watch face tints the
 * hands with the same number) sets the value directly instead, with
 * `cgl_arc_gauge_set_pulse()`.
 */
void cgl_arc_gauge_pulse(cgl_arc_gauge_t *g, uint32_t dur_ms, cgl_ease_t ease);

/**
 * The ambient flash level, Q8, set directly. Stops any running pulse tween.
 * **Marks no damage** -- the owner drives it and the owner damages.
 */
void cgl_arc_gauge_set_pulse(cgl_arc_gauge_t *g, uint16_t q8);

/** Move the gauge's centre to an offset from the widget frame's origin. */
void cgl_arc_gauge_set_center(cgl_arc_gauge_t *g, int16_t cx_off, int16_t cy_off);

/**
 * The point the sweep currently ends at, in screen coordinates -- where the lead
 * dot is and the only place the arc's *change* happens. Exactly what a damage
 * model needs, and the reason it is public.
 */
cgl_point_t cgl_arc_gauge_tip(const cgl_arc_gauge_t *g);

/** The logical (requested) value -- the animation target while one is running. */
int32_t cgl_arc_gauge_get_value(const cgl_arc_gauge_t *g);

/** What is drawn right now (equal to `cgl_arc_gauge_get_value()` when not animating). */
int32_t cgl_arc_gauge_get_shown_value(const cgl_arc_gauge_t *g);

/** Is an `cgl_arc_gauge_animate_to()` tween still running? */
bool cgl_arc_gauge_is_animating(const cgl_arc_gauge_t *g);

/**
 * How many `cgl_arc_gauge_set_range()` calls were rejected because `vmax <= vmin`.
 *
 * Same pattern as `cgl_bar_range_rejected()` / `cgl_widget_children_dropped()`:
 * observable, so "my gauge shows the wrong fraction" does not have to be guessed.
 * Under a normal configuration it is 0.
 */
uint32_t cgl_arc_range_rejected(void);

#ifdef __cplusplus
}
#endif

#endif /* CGL_CFG_ARC */

#endif /* CGL_ARC_H */
