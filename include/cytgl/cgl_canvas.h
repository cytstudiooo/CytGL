// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_canvas.h
  * @brief   LCD UI framework - band canvas and software rasterizer.
  *
  *  A canvas is a horizontal strip of the screen ("band") plus a clip window
  *  expressed in *screen* coordinates. All drawing helpers take screen
  *  coordinates; the canvas translates them to band rows transparently and
  *  clips against its clip window.
  *
  *  pan -- subtree translation
  *  --------------------------
  *  The canvas also has a pan: every primitive has it added before landing on a
  *  pixel. The widget tree walker (cgl_widget_draw_tree) pushes the pan on
  *  entering a widget that has an offset and restores it on exit, which is how
  *  "translate a whole subtree" is implemented -- the page-change slide
  *  transition relies on exactly this.
  *
  *  WARNING: clip is always in **screen coordinates** (pan excluded); pan only
  *  applies to the coordinates handed to a primitive. So the idiom "feed clip
  *  back into fill_rect" would translate twice, which is why
  *  cgl_canvas_clear_clip() takes the internal pan-free path.
  *
  *  The canvas is hardware independent (no HAL, no panel knowledge).
  ******************************************************************************
  */
#ifndef CGL_CANVAS_H
#define CGL_CANVAS_H

#include <cytgl/cgl_types.h>

/* cgl_angle_t / cgl_cos_q8 / cgl_sin_q8 for the radial primitives below.
   WARNING: this include goes **downwards**: cgl_math.h is a basis module that
   depends on <stdint.h> and nothing else, so the canvas may name its angle type
   without creating a cycle. (Same rule as the note on CANVAS_ONE_Q8 in
   cgl_canvas.c: the canvas must not reach *up* into a facility that is built on
   top of it -- cgl_anim, cgl_widget -- which is why the Q8 constant of 1.0 is
   spelled out locally down there.) */
#include <cytgl/cgl_math.h>

/* --------------------------------------------------------------------------
 * warp -- the non-linear "suck in" deformation (macOS Genie)
 *
 * pan is **linear**: every primitive is shifted by the same displacement. warp is
 * its non-linear version: source coordinates (the "resting screen coordinates"
 * with pan already added) are mapped to destination coordinates by the rules
 *
 *      rows: y' = ty + (y - ty) * (1 - p)                    monotonic, step 0 or 1
 *      cols: x' = cx' + (x - cx) * k(y) / 256 + S(y)         k(y) in [0,256]
 *
 * At p = 0 the whole mapping is an **exact identity** (see the derivation in
 * warp__x/warp__y), so pulling a deformation from 0 to 256 is "from as-is to
 * fully sucked into a point". At p = 256 every row lands on ty and every column
 * lands on the destination x: the page shrinks to a single point.
 *
 * S(y) is the **optional horizontal S trajectory** (identically 0 when
 * `s_amp_q8 = 0`, i.e. the old vertical suction): along a source row the
 * horizontal offset is a cubic Hermite -- tangent to the "screen centre axis" at
 * both ends, bulging out the most at the midpoint. So when the page is sucked
 * away it first arches out to one side and then tucks into the corner, tracing an
 * S. It only changes "where each row converges to"; it changes neither the row
 * mapping nor the funnel width, so the property in section "the deformation is a
 * monotonic compression" still holds as long as the amplitude stays under the
 * monotonicity bound (monotonicity requires `s_amp_q8 <= 682`; the derivation
 * lives in `cytgl_config.h` under `CGL_CFG_S_AMP_MAX_Q8`, which is the single
 * source of truth for that number -- do not restate a different value here).
 *
 * Why "deform the geometry" instead of "resample pixels": the band renderer has
 * only a 4-row buffer and no full-screen framebuffer (320x172x2 = 110 KB does not
 * fit in 32 KB of RAM). So instead of picking at source pixels it makes every
 * primitive recompute its own landing point per row -- solid rectangles stay
 * solid, dot matrix/text narrow row by row, at the cost of two extra
 * multiply-adds per row.
 *
 * WARNING: the deformation is a **monotonic compression** (the row step is always
 * 0/1 and columns only converge, never spread), and that property has two uses:
 *   1. Scattered writes (computing the destination row from the source row) leave
 *      no holes in the destination;
 *   2. The top-to-bottom order of source rows is preserved in the destination, so
 *      the occlusion relationships cannot be inverted by the deformation.
 * ------------------------------------------------------------------------ */
typedef struct
{
    int16_t src_y0;       /**< start of the source row range (the screen row where the page rests) */
    int16_t src_h;        /**< number of source rows (= the page height)   */
    int16_t cx;           /**< source centre column: the axis of the narrowing */
    int16_t tx;           /**< suction point x                             */
    int16_t ty;           /**< suction point y                             */
    uint16_t progress_q8; /**< 0 = identity; 256 = fully sucked to the suction point */
    uint16_t narrow_q8;   /**< width factor of the far end (the end near the suction point), Q8 */
    uint16_t row_step_q8; /**< normalised step per source row = 256*256/src_h */
    /**
     * Maximum offset of the horizontal S trajectory, Q8 (0 = no S trajectory,
     * straight line = the old Genie).
     * The offset is distributed along the source row as a cubic Hermite (see the
     * top of cgl_canvas.h), peaking at the middle of the source range and 0 at
     * both ends, so at p = 0 it is still an exact identity.
     * WARNING: the monotonicity bound is `s_amp_q8 <= 682`. The derivation is
     * owned by CGL_CFG_S_AMP_MAX_Q8 in cytgl_config.h -- read it there rather
     * than restating a different number here. Above that bound x' genuinely
     * stops being monotone in y and the page folds like a fan.
     *
     * Note for anyone tuning this: because the column term rounds to nearest
     * while the S term truncates, the *rounded* output shows one- or two-pixel
     * rises between adjacent rows even at very small amplitudes. Those are
     * rounding, not crossings -- a real fold shows up as steps on the order of
     * the page width, and at amp = 0 (no S at all) the funnel mapping is exactly
     * monotone.
     */
    uint16_t s_amp_q8;
} cgl_warp_t;

typedef struct
{
    cgl_color_t *buf;    /**< CGL_CFG_SCREEN_W x height pixels, row major       */
    int16_t width;      /**< pixels per row                                   */
    int16_t height;     /**< rows held by this canvas (band height)           */
    int16_t origin_y;   /**< screen y of row 0 of the band                    */
    cgl_rect_t clip;     /**< clip window, screen coordinates, inclusive       */
    cgl_point_t pan;     /**< pan of this level; primitive coordinates get it added before clipping/pixel write */
    const cgl_warp_t *warp; /**< non-NULL = this subtree is drawn through the "suck in" deformation (see above) */
    cgl_rect_t src_clip; /**< clip window in **source coordinates**; only consulted while warp is active */

    /* --- group opacity (see cgl_widget_set_alpha / cgl_config.h) -------------
     * A scratch provided by the display layer with **exactly the same geometry**
     * as the band buffer, split into alpha_levels layers of level_stride pixels.
     * When the widget tree walker wants to render one subtree in isolation, all
     * it does is temporarily point buf at one of those layers -- and because the
     * geometry is identical, every coordinate system (pan / warp / clip /
     * src_clip) stays valid as-is, **even bare row-pointer users of
     * cgl_canvas_row() (fonts, custom widgets) follow along automatically**,
     * with no primitive changes at all.
     *
     * NULL = this canvas does not do group isolation (the value recorded by
     * cgl_widget_set_alpha() has no effect and is counted).
     * cgl_canvas_init() clears it to NULL; cgl_display_present() installs it once
     * per band.
     * -------------------------------------------------------------------- */
    cgl_color_t *alpha_scratch;
    uint32_t alpha_stride; /**< pixels per layer (>= width * height)           */
    uint8_t alpha_levels;  /**< how many layers are in the scratch            */
    uint8_t alpha_used;    /**< how many layers are currently nested in use   */

    /**
     * **Ink alpha**: the Q8 opacity every *blended* primitive draws at, 256 (the
     * default) meaning "as asked for". It is the per-primitive counterpart of
     * `alpha_scratch` above, and the two are deliberately different things:
     *
     *   - `cgl_widget_set_alpha()` (the scratch) makes a whole **subtree** one
     *     translucent group, which needs a scratch buffer and is what stops two
     *     overlapping children from being blended twice;
     *   - this makes **one primitive** translucent, needs nothing at all, and is what
     *     "draw this ring at 40 %" means.
     *
     * It is honoured by every path that goes through `canvas__blend()` /
     * `canvas__store()` -- i.e. `cgl_canvas_arc()`, `cgl_canvas_circle()`,
     * `cgl_canvas_disc()`, `cgl_canvas_line_thick()` (thickness > 1) and
     * `cgl_canvas_pixel_blend_q8()`. The **opaque** fast paths (`fill_rect`, `line`,
     * `hline`/`vline`, `dot`, `clear`, and `line_thick` with a 1 px pen, which is the
     * Bresenham walk) do not consult it: they are byte stores, and "draw a
     * translucent solid rectangle" already has a door of its own,
     * `cgl_canvas_fill_rect_blend_q8()`.
     *
     * WARNING: it is **state**, not an argument, and it is not saved and restored for
     * you. Set it, draw, set it back to `CGL_CANVAS_INK_OPAQUE` -- or use the
     * `cgl_arc_draw_*()` wrappers in cgl_arc.h, which do exactly that around one call.
     */
    uint16_t ink_alpha_q8;
} cgl_canvas_t;

/** `cgl_canvas_set_ink_alpha()` at full opacity: the default, and the value that
    takes the zero-cost path through every blended primitive. */
#define CGL_CANVAS_INK_OPAQUE 256u

/** Bind a band buffer to a canvas. origin_y is the screen row of band row 0. */
void cgl_canvas_init(cgl_canvas_t *c, cgl_color_t *buf,
                    int16_t width, int16_t height, int16_t origin_y);

/* --- clip / pan handling ------------------------------------------------- */
cgl_rect_t cgl_canvas_get_clip(const cgl_canvas_t *c);
void cgl_canvas_set_clip(cgl_canvas_t *c, cgl_rect_t clip);
void cgl_canvas_reset_clip(cgl_canvas_t *c);

/** Current pan (screen coordinate = primitive coordinate + pan). */
cgl_point_t cgl_canvas_get_pan(const cgl_canvas_t *c);

/* --- ink alpha (per-primitive opacity) ----------------------------------- */

/**
 * Set the opacity every **blended** primitive draws at, Q8: 0 draws nothing at all,
 * 256 (`CGL_CANVAS_INK_OPAQUE`, the default) draws exactly what was asked for.
 *
 * It exists so that "an arc at 40 %" is one call rather than a second rasteriser: the
 * coverage the primitive already computes is simply scaled before the blend, so the
 * edge stays antialiased and the result is `dst*(1-a*cov) + ink*a*cov` -- the same
 * formula as everything else in the framework, not a second one.
 *
 * Which primitives honour it, and why the opaque ones do not, is documented on the
 * `ink_alpha_q8` field of `cgl_canvas_t`. See also the `cgl_arc_draw_*()` wrappers in
 * cgl_arc.h, which set it, draw one arc/circle/disc and put it back.
 *
 * Values above 256 are clamped; there is no "over-opaque".
 */
void cgl_canvas_set_ink_alpha(cgl_canvas_t *c, uint16_t alpha_q8);

/** The current ink alpha (CGL_CANVAS_INK_OPAQUE when there is no canvas). */
uint16_t cgl_canvas_get_ink_alpha(const cgl_canvas_t *c);

/**
 * Set the pan. The caller is responsible for pairing it with a restore (the
 * walker uses save/restore rather than incremental changes).
 */
void cgl_canvas_set_pan(cgl_canvas_t *c, cgl_point_t pan);

/* --- warp (non-linear "suck in" deformation) ----------------------------- */

/** Install/uninstall the deformation descriptor (NULL = off). The walker installs it on entering a subtree that has a warp. */
void cgl_canvas_set_warp(cgl_canvas_t *c, const cgl_warp_t *w);
const cgl_warp_t *cgl_canvas_get_warp(const cgl_canvas_t *c);

/**
 * Source coordinate -> destination coordinate. Identity when no warp is installed.
 *
 * WARNING: what is passed in is a screen coordinate **with pan already added**
 * (the same coordinate system as clip), and map_x() needs the source row index y
 * that the pixel lies on -- the column mapping depends on the row.
 */
int16_t cgl_canvas_map_y(const cgl_canvas_t *c, int16_t y);
int16_t cgl_canvas_map_x(const cgl_canvas_t *c, int16_t x, int16_t y);

/**
 * The **conservative** bounding box of a source rectangle in the destination
 * (just itself when there is no warp).
 * The walker uses it for culling: the deformation is compressive, so the source
 * box and "the place it is actually drawn" may not overlap at all (when a page is
 * sucked to the other end of the screen), and culling with the source box would
 * wrongly cull the entire subtree.
 */
cgl_rect_t cgl_canvas_map_rect(const cgl_canvas_t *c, cgl_rect_t r);

/**
 * Source-coordinate clip window. **Only consulted while warp is active**, and it
 * tests the **source coordinates** of the primitive, before the mapping.
 *
 * Why a second clip window is unavoidable: `clip` is in destination coordinates,
 * and after the deformation a rectangular clip window no longer equals "where
 * this level may draw". The floor grid lines are the ready-made example: they are
 * projected from world coordinates and deliberately drawn outside the viewport,
 * relying on the viewport's CLIP to cut them off; if clipping only happened in
 * destination space, those lines would be let in by the "deformed bounding box"
 * and leave a few pixels of noise outside the funnel outline.
 *
 * Semantically this is the correct one anyway: the deformation is applied
 * **after** drawing, so clipping naturally belongs in source space.
 */
void cgl_canvas_set_src_clip(cgl_canvas_t *c, cgl_rect_t clip);
cgl_rect_t cgl_canvas_get_src_clip(const cgl_canvas_t *c);

/**
 * The **source-space window that can still reach this band** (inclusive, screen
 * coordinates): the intersection of the source clip window with the pre-image of
 * the clip window under the row mapping. Empty when nothing of this level can be
 * seen at all.
 *
 * A rasteriser that walks its own shape row by row -- the framework's own
 * `cgl_canvas_arc()` / `cgl_canvas_line_thick()` / `cgl_canvas_disc()`, and a
 * widget's custom one (a tapered watch hand, say) -- uses this to bind that walk.
 * Without a warp the answer is just the clip window, which is what cuts a 160 row
 * walk down to the 4 rows of the band.
 *
 * WARNING: **with a warp installed the answer is much taller than the band** and
 * this is the whole point of the function: the deformation compresses the subtree
 * towards the suction point, so a 4 row band receives source rows from far outside
 * it, and the only alternative to computing the pre-image is walking the *entire*
 * shape in every one of the 43 bands -- with a per-pixel warp mapping on top.
 * Measured on the watch face, one frame of an S_OUT transition cost 18.7 M host
 * cycles that way against 1.8 M for the same page flat (four frames for the whole
 * transition, i.e. the animation stutters).
 *
 * The window is a **superset** of the pixels that will be drawn: the destination
 * clip test, the coverage and the blend still belong to the rasteriser. What it
 * removes is only the part of the walk that provably cannot produce a pixel, and
 * with a warp it is bounded by the source clip, so the walk can still be bound.
 */
cgl_rect_t cgl_canvas_src_window(const cgl_canvas_t *c);

/**
 * Direct row access for fast rasterizers (text, sprites).
 *
 * WARNING: y is a **screen row index**, not a primitive coordinate -- this
 * function does **not add pan** (its return value is a bare buffer pointer, and
 * there is nowhere to "conveniently" translate it). A rasterizer that uses it must
 * add pan itself:
 *
 *      pan = cgl_canvas_get_pan(c);
 *      row = cgl_canvas_row(c, (int16_t)(local_y + pan.y));
 *      row[(int16_t)(local_x + pan.x)] = color;
 *
 * The consequence of forgetting this step is "the rectangle follows the subtree
 * while the text stays put" (cgl_font.c has a measured record of it at the top).
 *
 * Returns NULL when the row is outside the band. The returned row is *not*
 * clipped horizontally: the caller must honour cgl_canvas_get_clip() itself
 * (clip is also in screen coordinates).
 */
cgl_color_t *cgl_canvas_row(const cgl_canvas_t *c, int16_t y);

/* --- primitives (screen coordinates) ------------------------------------- */
void cgl_canvas_clear(cgl_canvas_t *c, cgl_color_t color);          /* whole band  */
void cgl_canvas_clear_clip(cgl_canvas_t *c, cgl_color_t color);     /* clip window */
void cgl_canvas_pixel(cgl_canvas_t *c, int16_t x, int16_t y, cgl_color_t color);
void cgl_canvas_hline(cgl_canvas_t *c, int16_t x0, int16_t x1, int16_t y, cgl_color_t color);
void cgl_canvas_vline(cgl_canvas_t *c, int16_t x, int16_t y0, int16_t y1, cgl_color_t color);
void cgl_canvas_line(cgl_canvas_t *c, int16_t x0, int16_t y0, int16_t x1, int16_t y1, cgl_color_t color);
void cgl_canvas_fill_rect(cgl_canvas_t *c, cgl_rect_t r, cgl_color_t color);
void cgl_canvas_rect(cgl_canvas_t *c, cgl_rect_t r, cgl_color_t color);
void cgl_canvas_disc(cgl_canvas_t *c, int16_t cx, int16_t cy, int16_t radius, cgl_color_t color);

/**
 * Small filled square "dot" (size 1..3), centred on (cx, cy).
 * Cheaper than cgl_canvas_disc() and keeps the negative space between
 * neighbouring dots open, which is what makes a dot matrix logo readable.
 */
void cgl_canvas_dot(cgl_canvas_t *c, int16_t cx, int16_t cy, uint8_t size, cgl_color_t color);

/* --- radial primitives (antialiased) ---------------------------------------
 *
 * A dial, a gauge, a speedometer needle and a radar sweep are all "a point on a
 * circle", and until these three existed the framework could not draw one: it had
 * `disc` (a filled circle) and `rect` (a rectangle outline) but no **outline of a
 * circle**, no way to draw a line thicker than one pixel, and no way to draw a
 * piece of a circle at all. They are primitives, not widgets on purpose: they
 * answer "which pixels", and anything that answers "when" or "how much" belongs in
 * a widget on top of them.
 *
 * **These three are antialiased, and the block primitives are not.** A rectangle or
 * a horizontal line has no jagged edge to fix; a circle, a hand or a ring has
 * nothing but jagged edges -- a staircase two or three pixels deep wherever the
 * tangent approaches 45 degrees, which on a 250 dpi panel reads as a burr rather
 * than as a curve. So the strokes here compute a per-pixel coverage (see the long
 * note in cgl_canvas.c: squared distances, one division per boundary pixel, no
 * sqrt) and blend it in, while `fill_rect` / `line` / `disc` / `dot` stay crisp
 * because they are the dot matrix / bar / panel hot path, where one pixel is
 * exactly one pixel.
 *
 * Consequences a caller can see:
 *   - the boundary pixels of a stroke are **blends** of the stroke colour and
 *     whatever was underneath, so "is this pixel the stroke colour?" tests have to
 *     ask about the interior, not the edge;
 *   - a stroke is drawn at its true sub-pixel position, so a slowly moving hand
 *     moves *by* a fraction of a pixel instead of stepping -- which is what stops a
 *     gauge from vibrating as it sweeps;
 *   - the cost is proportional to the stroke's **area** (one coverage per pixel,
 *     plus a divide for the pixels on the boundary), not to its bounding box.
 *
 * All three are round-capped ("pen") strokes and take **primitive** coordinates like
 * every other primitive here, so pan / warp / clip / src_clip are handled by the
 * same code path as `disc`, with no special cases.
 * ------------------------------------------------------------------------- */

/**
 * One pixel wide circle **outline**, antialiased, centred on (cx, cy) -- the circle
 * analogue of `cgl_canvas_rect()` and the counterpart of `cgl_canvas_disc()`.
 *
 * It is exactly "a one pixel stroke on this radius" (the annulus radius +- 0.5), so
 * it is implemented as `cgl_canvas_arc(c, cx, cy, radius, 1, 0, CGL_ANGLE_TURN)`.
 * A radius <= 0 draws a single antialiased pixel: the degenerate circle is a point,
 * not an error.
 *
 * For a thicker ring use `cgl_canvas_arc()` with `thickness > 1` and a full sweep.
 */
void cgl_canvas_circle(cgl_canvas_t *c, int16_t cx, int16_t cy, int16_t radius, cgl_color_t color);

/**
 * Thick line segment with **round caps**: the "pen stroke".
 *
 * @param thickness stroke width in pixels. The drawn width is exactly this, odd or
 *                  even (antialiasing has no reason to round to odd any more).
 *                  `thickness <= 1` takes the plain Bresenham path instead and is
 *                  **pixel for pixel identical to `cgl_canvas_line()`** -- the
 *                  crisp hairline is still available, and is what a caller wants for
 *                  a one pixel tick or a grid line.
 *
 * Cost is proportional to the stroke's area: the loop walks the **major axis** and
 * derives the minor range per step (a slanted thin stroke therefore does not pay for
 * the empty corners of its bounding box), and a pixel outside the pen's reach is
 * rejected by one squared-distance comparison.
 *
 * WARNING: the caps are *round*, so the stroke is up to `thickness / 2` pixels
 * longer than the segment at each end. A caller that has to fit inside a box
 * (rather than merely look right) shortens its own segment by that much.
 */
void cgl_canvas_line_thick(cgl_canvas_t *c, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                          int16_t thickness, cgl_color_t color);

/**
 * Thick circular arc with round caps, antialiased -- the ring / gauge /
 * radial-progress primitive.
 *
 * @param cx, cy    centre, primitive coordinates
 * @param radius    radius of the arc's **centre line** (the stroke is centred on it)
 * @param thickness stroke width in pixels, same meaning as in `cgl_canvas_line_thick()`
 * @param start     start angle; 0 = +x, growing clockwise on screen (see cgl_math.h)
 * @param sweep     length of the arc. A `uint32_t` **on purpose**: a `cgl_angle_t`
 *                  wraps at one turn, so "the whole ring" (CGL_ANGLE_TURN) would be
 *                  indistinguishable from "nothing at all". Sweeps larger than one
 *                  turn are legal and simply wrap.
 *
 * @note a full ring is `cgl_canvas_arc(c, cx, cy, r, w, 0u, CGL_ANGLE_TURN, color)`.
 *       It is drawn **without** an angular clip at all (a full turn needs none),
 *       which is both the common case and the cheapest.
 *
 * @note `radius <= 0` degenerates to an antialiased **dot** of diameter `thickness`:
 *       a stroke of width 2r on a radius of zero is a disc. That is how a widget
 *       draws an antialiased dot without another primitive.
 *
 * The shape is rasterised **per screen row** from the annulus' exact half-width
 * (`cgl_math_isqrt()`), and the angular limit is a cross product against the two
 * bounding directions. Nothing here is sampled along the angle, so the result is a
 * function of the geometry alone: growing the sweep only *extends* the ring, it does
 * not re-place the pixels already drawn. (The first implementation stamped discs
 * along the path with a step derived from the current sweep; as the arc grew, the
 * step changed and every stamp moved by a fraction of a pixel, which showed up as a
 * ring that shimmered from frame to frame.)
 */
void cgl_canvas_arc(cgl_canvas_t *c, int16_t cx, int16_t cy, int16_t radius, int16_t thickness,
                    cgl_angle_t start, uint32_t sweep, cgl_color_t color);

/**
 * One pixel, blended `cov_q8` of the way from what is already there to `color`
 * (`0` = leave it, `256` = replace it).
 *
 * This is the single-pixel form of `cgl_canvas_fill_rect_blend_q8()` and it is the
 * building block of the antialiased strokes above, exposed because a widget with its
 * own rasteriser (a sprite edge, a custom gauge, an antialiased corner) needs the
 * same coverage-to-pixel step and should not have to re-derive pan / warp / clip /
 * src_clip to get it.
 */
void cgl_canvas_pixel_blend_q8(cgl_canvas_t *c, int16_t x, int16_t y, cgl_color_t color,
                               uint16_t cov_q8);

void cgl_canvas_blit(cgl_canvas_t *c, int16_t x, int16_t y,
                    const cgl_color_t *src, int16_t w, int16_t h);

/** Alpha blending of a straight line: alpha_q8 = 0 leaves the destination unchanged, 256 becomes `color` entirely. */
void cgl_canvas_fill_rect_blend_q8(cgl_canvas_t *c, cgl_rect_t r,
                                  cgl_color_t color, uint16_t alpha_q8);

/** Fill the alpha blended (50/50) intersection of r with the clip window. */
void cgl_canvas_fill_rect_blend(cgl_canvas_t *c, cgl_rect_t r, cgl_color_t color);

/* --- group opacity (save-under layered compositing) ----------------------- */

/**
 * Install / uninstall (scratch = NULL) the layered buffer for group opacity.
 *
 * level_stride_px is the number of pixels **per layer** and must be >= width x
 * height (the presenter hands over one band at a time, so
 * CGL_CFG_SCREEN_W * CGL_CFG_BAND_H is enough and is also an upper bound for the
 * last, partially filled band). levels is the number of layers and decides how
 * deeply they may nest. It also resets used to zero.
 */
void cgl_canvas_set_alpha_scratch(cgl_canvas_t *c, cgl_color_t *scratch,
                                 uint8_t levels, uint32_t level_stride_px);

/** How many layers are still available for nesting (0 = one more level would have to give up alpha). */
uint8_t cgl_canvas_alpha_room(const cgl_canvas_t *c);

/**
 * Copy the pixels of the current buffer inside r into dst. dst must have **the
 * same geometry** as the current buffer (same band, same width/height/origin_y) --
 * that is the precondition the group opacity scratch relies on. r is clipped to
 * the band and to clip.
 */
void cgl_canvas_copy_rect(const cgl_canvas_t *c, cgl_color_t *dst, cgl_rect_t r);

/**
 * Per-pixel source blend: `dst = dst*(256-a) + src*a`, with src sharing the
 * current buffer's geometry.
 *
 * The only difference from fill_rect_blend_q8 is that the source is **a whole
 * block of pixels** instead of one constant colour. It is the last step of group
 * opacity -- compositing the "isolated rendered subtree" back onto the background
 * by alpha. Because both paths share cgl_color_blend_q8(), they cannot compute
 * different results.
 */
void cgl_canvas_blend_src_q8(cgl_canvas_t *c, cgl_rect_t r,
                            const cgl_color_t *src, uint16_t alpha_q8);

#endif /* CGL_CANVAS_H */
