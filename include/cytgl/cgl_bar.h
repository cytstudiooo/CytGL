// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_bar.h
  * @brief   LCD UI framework - progress / level bar widget.
  *
  *  A bar answers one question -- "how far along is this value between two
  *  limits" -- in pixels. It is the third built-in widget next to `cgl_panel`
  *  (a rectangle) and `cgl_label` (text), and it was promoted here from the
  *  demo's teaching example (`App/widgets/cgl_bar.[ch]`), which is why it
  *  deliberately keeps the shape that example documented: the struct starts
  *  with `cgl_widget_t base`, the vtable is written with designated
  *  initialisers, and a setter marks damage **only when the drawn pixels can
  *  really change**.
  *
  *  What the teaching example did not have, and what a real bar needs
  *  ---------------------------------------------------------------
  *   - an **arbitrary range** (`vmin..vmax`, signed) instead of a bare 0..100.
  *     A bar is asked for "ADC 0..4095", "temperature -20..60", "+/-100 mV"
  *     far more often than for a percentage; `cgl_bar_set_percent()` stays as
  *     the convenience setter on top of the range.
  *   - **vertical** orientation (a level meter fills upwards from the bottom
  *     edge), plus a `reverse` flag for the two directions that are not the
  *     default (right-to-left, top-to-bottom).
  *   - a **border ring**, so a bar can be framed like `cgl_panel` instead of
  *     being a bare colour block.
  *   - **segments**: the track divided into N equal slots, which is how a
  *     "battery blocks" or "signal strength" readout is drawn.
  *   - **smooth value changes** through `cgl_anim` (`cgl_bar_animate_to()`),
  *     driven by the widget's own `update` callback. This is purely visual
  *     animation and therefore belongs to the widget, not to the page's
  *     `on_step` (the CytGL manual, section 3.3).
  *
  *  No floating point, no `int64`, no libm
  *  --------------------------------------
  *  The value -> pixel mapping is a 32-bit fixed-point division. The obvious
  *  `(off << 8) / span` overflows as soon as the range is wider than 2^23, and
  *  `int64` is not an option in this project: it drags `__aeabi_ldivmod` in
  *  from libgcc (see the note in cgl_anim.h). So the fraction is produced by a
  *  nine-step shift/subtract long division (eight fractional bits plus the bit
  *  that rounds them), which covers the full `int32_t` domain at the cost of
  *  ~9 iterations **per draw**, not per pixel.
  *
  *  Geometry (the border is the padding)
  *  ------------------------------------
  *      frame            filled with `track`
  *       +-- inner       frame inset by `border_w`; the fill lives here
  *       |    +-- fill   `fill`, along the orientation axis
  *       +-- ring        `border`, drawn last so it is never overdrawn
  *
  *  `border_w` therefore doubles as the bar's padding: there is no second
  *  "inset" knob, because the two would always be set to the same number.
  *
  *  The fill length is `round(len x fraction)`: 0 at `vmin` and exactly `len` at
  *  `vmax` (round to nearest, so a value half way up a 6 px bar draws 3 px
  *  rather than 2). The fraction itself is quantised to 1/256 of the range,
  *  which is under one pixel of error on any bar that fits on this panel.
  ******************************************************************************
  */
#ifndef CGL_BAR_H
#define CGL_BAR_H

#include <cytgl/cgl_widget.h>
#include <cytgl/cgl_anim.h>

/** Which screen axis the bar grows along. */
typedef enum
{
    CGL_BAR_HORIZONTAL = 0, /**< grows along x (the default)                     */
    CGL_BAR_VERTICAL = 1    /**< grows along y: a level meter filling upwards     */
} cgl_bar_orient_t;

typedef struct
{
    cgl_widget_t base;   /**< <- must be first, see the CytGL manual, section 16.1 */
    cgl_color_t track;   /**< the unfilled background (also the widget's own background) */
    cgl_color_t fill;    /**< the completed part                               */
    cgl_color_t border;  /**< the ring colour; only drawn while `border_w > 0`  */
    cgl_color_t seg;     /**< separator colour; only drawn while `segments > 1` */
    /**
     * Timeline of the **drawn** value. `anim.value` is what is on screen,
     * `value` below is what the caller asked for; they are equal whenever no
     * animation is running.
     */
    cgl_anim_t anim;
    int32_t value;       /**< the logical (requested) value, always inside [vmin, vmax] */
    int32_t vmin;        /**< low end of the range (0 by default)              */
    int32_t vmax;        /**< high end of the range (100 by default)           */
    uint8_t orient;      /**< cgl_bar_orient_t                                 */
    uint8_t border_w;    /**< border ring thickness in pixels, 0 = no border    */
    uint8_t segments;    /**< number of equal slots; 1 = one continuous bar     */
    uint8_t reverse;     /**< 0 = fill from x0 / y1 (default), 1 = from x1 / y0 */
} cgl_bar_t;

/**
 * Create a horizontal bar covering `vmin..vmax = 0..100`, value 0, no border
 * and no segments.
 *
 * `track` is also used as the widget's background: like every widget, the bar
 * has to paint its whole frame itself, because the framework never clears the
 * background (a band renderer has nowhere to keep the previous frame, see
 * the CytGL manual, section 9.6).
 */
void cgl_bar_init(cgl_bar_t *b, const char *name, cgl_rect_t frame,
                  cgl_color_t track, cgl_color_t fill);

/**
 * Set the range. `vmax` must be strictly greater than `vmin`; a degenerate or
 * inverted range is **rejected** (returns false, counts in
 * `cgl_bar_range_rejected()` and leaves the old range in place) rather than
 * silently turned into something arbitrary. The current value is re-clamped and
 * the bar is repainted, because the same value draws at a different position
 * once the range changes.
 *
 * @return true when the range was accepted.
 */
bool cgl_bar_set_range(cgl_bar_t *b, int32_t vmin, int32_t vmax);

/**
 * Set the value and draw it **immediately** (any running animation is snapped
 * to the new value). Clamped into the range.
 *
 * Repaints only when the drawn pixels can really change: setting the value it
 * already has costs one comparison and no damage.
 */
void cgl_bar_set_value(cgl_bar_t *b, int32_t value);

/**
 * Convenience setter: `percent` (0..100, clamped) mapped onto the range, so on
 * the default 0..100 range it is exactly `cgl_bar_set_value()`. The mapping is
 * exact integer arithmetic (it cannot overflow for the full `int32_t` domain)
 * and always lands on a representable value of the range.
 */
void cgl_bar_set_percent(cgl_bar_t *b, uint8_t percent);

/**
 * Move to `value` over `dur_ms`, using the tween in `b->anim`. The bar advances
 * itself from its own `update` callback, so the caller only has to keep calling
 * `cgl_widget_update_tree()` -- which `cgl_screen_frame()` already does once per
 * frame (step 6).
 *
 * The tween starts from **what is currently drawn**, not from the previous
 * target, so retargeting mid-flight is smooth rather than a jump back.
 *
 * `dur_ms == 0` means "no animation": the call degrades to
 * `cgl_bar_set_value()` and returns false.
 *
 * @return true when a tween was really started (false = the value is already
 *         there, or `dur_ms` was 0, or `b` is NULL).
 */
bool cgl_bar_animate_to(cgl_bar_t *b, int32_t value, uint32_t dur_ms, cgl_ease_t ease);

/** Which screen axis the bar grows along. Repaints only when it really changed. */
void cgl_bar_set_orient(cgl_bar_t *b, cgl_bar_orient_t orient);

/**
 * `false` (the default) = grow from the left edge (horizontal) / from the
 * bottom edge (vertical); `true` = from the right edge / from the top edge.
 * Repaints only when it really changed.
 */
void cgl_bar_set_reverse(cgl_bar_t *b, bool reverse);

/** Replace the track and fill colours. Repaints only when one really changed. */
void cgl_bar_set_colors(cgl_bar_t *b, cgl_color_t track, cgl_color_t fill);

/**
 * Draw a `width` pixel border ring in `color`; `width == 0` (the default)
 * removes the border.
 *
 * `width` is clamped so that the ring always leaves **at least one pixel line
 * of bar** between its two sides, which means a bar thinner than 3 px gets no
 * border at all. If the frame shrinks afterwards (via `cgl_widget_set_frame()`),
 * the ring is simply drawn over the whole bar and the fill disappears -- it does
 * not underflow, but the setter's clamp is a promise about the frame it was
 * called with, not about every future one.
 *
 * Repaints only when the colour or the clamped width really changed.
 */
void cgl_bar_set_border(cgl_bar_t *b, cgl_color_t color, uint8_t width);

/**
 * Divide the bar into `segments` equal slots, drawing the `segments - 1`
 * separators between them in `color`. `segments <= 1` (the default) turns the
 * separators off again.
 *
 * The separators run across the **whole** inner rectangle, over the fill and
 * over the track alike, so the picture reads as "N equal slots, of which the
 * first k are filled" -- that is the battery / signal-strength look.
 *
 * Two consequences worth knowing:
 *   - passing the **track colour** as the separator colour flips it into the
 *     other reading, "the fill is cut into blocks": the slots the value has not
 *     reached have their separator drawn in the colour that is already there,
 *     so it disappears. Both looks come out of the same code path;
 *   - a bar shorter than `segments` pixels would put several separators on one
 *     column, so the counting is capped at one separator per pixel (a 4 px bar
 *     cannot cost 254 vlines, and the picture is the same either way).
 *
 * Repaints only when the count or the colour really changed.
 */
void cgl_bar_set_segments(cgl_bar_t *b, uint8_t segments, cgl_color_t color);

/** The logical (requested) value -- the animation target while one is running. */
int32_t cgl_bar_get_value(const cgl_bar_t *b);

/** What is drawn right now (equal to `cgl_bar_get_value()` when not animating). */
int32_t cgl_bar_get_shown_value(const cgl_bar_t *b);

/**
 * The current value as 0..100 of the range (truncating, so it is the inverse of
 * `cgl_bar_set_percent()` up to one percent).
 */
uint8_t cgl_bar_get_percent(const cgl_bar_t *b);

/** Is a `cgl_bar_animate_to()` tween still running? */
bool cgl_bar_is_animating(const cgl_bar_t *b);

/**
 * How many `cgl_bar_set_range()` calls were rejected because `vmax <= vmin`.
 *
 * Same pattern as `cgl_widget_children_dropped()` / `cgl_widget_alpha_dropped()`:
 * observable, so "my bar shows the wrong fraction" does not have to be guessed.
 * Under a normal configuration it is 0.
 */
uint32_t cgl_bar_range_rejected(void);

#endif /* CGL_BAR_H */
