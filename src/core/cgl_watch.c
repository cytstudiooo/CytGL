// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_watch.c
  * @brief   LCD UI framework - the analog watch face widget: the time base, the
  *          calendar, the dial, and the damage model that makes it affordable.
  *
  *  What this file is
  *  -----------------
  *  The demo's watch dial (`examples/common/App/app_page_clock.c`) **moved**, not
  *  rewritten. The arithmetic below is the arithmetic that was measured on the
  *  reference panel -- the reveal's forward modulo, the seconds ring's unsigned
  *  leading-end subtraction, the trail's carried remainder, the damage margins --
  *  so that a page repointed at this module and this module draw the same pixels
  *  for ever. What changed is *where the numbers come from*: a `CLOCK_*` macro
  *  became a field of `cgl_watch_config_t`, a `clock__*` helper became a `static`
  *  here or a `cgl_arc_*` call there, and the page's complications (the digital
  *  time, the gauge, the date line) stayed with the page, because they are not
  *  part of a dial.
  *
  *  The three ideas worth knowing before reading the code
  *  -----------------------------------------------------
  *  **1. The dial is a pure function of the clock; only the *events* are tweened.**
  *  A second hand does not need a state machine: its angle is a fraction of a turn
  *  evaluated fresh every frame from the time (see `cgl_watch_second_angle()`), so
  *  nothing accumulates, nothing can drift, and a dropped frame costs smoothness
  *  and nothing else. What *is* tweened (with `cgl_anim`) is the two things that
  *  are events rather than motion:
  *
  *      reveal   0 -> 256 over cfg.anim.reveal_ms   the hands sweep from 12:00
  *      pulse  256 -> 0   over cfg.anim.pulse_ms    the flash as the minute turns
  *
  *  Note the direction of the pulse: it **decays** to zero, because its resting
  *  value has to be the "nothing is happening" value. A pulse that ramps 0 -> 256
  *  leaves every quantity it multiplies permanently tinted once the first minute
  *  has passed. (Measured on the reference page before the fix: the ring's track
  *  stayed half way blended toward the accent for ever.)
  *
  *  **2. Every element of the face is "a point at an angle on a radius".** Not one
  *  element is a special case: the ring is `cgl_canvas_arc()`, a hand is a
  *  `cgl_arc_stroke_t` -- one tapered quadrilateral, not a chain of thick lines,
  *  whose seams would bulge -- a tick is a two-point polar pair, the dial and its
  *  well are two `cgl_arc_surface_t`s evaluated per pixel and stored nowhere, and
  *  the crystal over all of it is one dot product per pixel plus a blend.
  *
  *  **3. Damage is the only thing that costs.** The dial is 166 rows of a 172 row
  *  panel, so invalidating it whole marks all 43 bands dirty -- measured on the
  *  reference board, that was the difference between 13 fps and a smooth face: a
  *  full-dial damage pushes the host's background fill over the whole screen on top
  *  of the face's own pixels. So `cgl_watch_frame()` marks damage the way a watch
  *  face actually changes: the second hand and its counterweight at the previous
  *  angle *and* the current one, the seconds ring's leading end, every chapter mark
  *  still carrying comet glow, the hour and minute hands **only on the frames where
  *  their integer endpoints really move** (they creep -- one pixel every ten seconds
  *  -- so between those frames their pixels are identical), and the whole widget
  *  only while the entrance or the pulse is running.
  *
  *  A missed region would leave a stale ghost on the panel, so the margins below are
  *  generous and `tests/watch_host_test.c` has a case that forces a full redraw and
  *  requires the incrementally damaged frame to be **identical** to it.
  *
  *  Cost
  *  ----
  *  The CPU side of a full dial is a 20 000 px face and a 3 800 px well, each a few
  *  dozen cycles a pixel (a table lookup, one division for the sunburst's direction,
  *  one blend for the highlight or the well's tilt), ~4 700 px of antialiased ring
  *  strokes, 60 marks, three hands drawn twice (shadow, then ink) over a per-row
  *  span of a dozen pixels instead of a bounding box, a cover that costs one dot
  *  product a pixel plus a blend where the reflection really falls, and a hand
  *  rasteriser bound to the band -- see the WARNING in `cgl_arc_stroke_draw()`: a
  *  stroke that walks its whole bounding box in every band pays 43 times for one
  *  picture, and measured on the reference page that alone was two thirds of a
  *  full-repaint frame. An *incremental* frame -- the common case -- repaints only
  *  the bands the second hand crosses.
  ******************************************************************************
  */
#include <cytgl/cgl_watch.h>
#include <cytgl/cytgl_config.h> /* the CGL_CFG_WATCH_* compile-time switches */
#include <cytgl/cgl_canvas.h>   /* the primitives every element is drawn with  */
#include <cytgl/cgl_math.h>     /* cgl_math_polar() / cgl_math_polar_q6()      */

#if CGL_CFG_WATCH

/* -------------------------------------------------------------------------- */
/* The handful of numbers the configuration deliberately has no field for      */
/* -------------------------------------------------------------------------- */

/** The seconds ring's **lead dot**: its radius at rest, and its widest -- which is
    during the minute pulse (`WATCH_DOT_R + pulse/64`, so 7 px at the peak).

    WARNING: neither is a field of `cgl_watch_config_t` (they are a property of the
    ring's own stroke: a 3 px stroke with a 3 px dot), and the *widest* value is not
    decoration -- it is what the damage margin has to cover. A box sized for the 3 px
    stroke alone is what this used to be, and the dot's own edge was then never
    repainted: measured against a full redraw, the panel kept a crescent of the
    previous dot's position. See `watch__damage_ring_end()`. */
#define WATCH_DOT_R 3
#define WATCH_DOT_R_MAX 7

/** The second hand's counterweight, as a `cgl_canvas_arc()` **thickness on radius
    0**: that is an antialiased dot of diameter 7, which is the whole reason the arc
    primitive is used here rather than `cgl_canvas_disc()` -- a 6 px round dot on the
    end of a needle wants a soft edge. */
#define WATCH_COUNTERWEIGHT_R 7

/** The hub's inner arbor dot, same trick: an antialiased dot of diameter 5, inside
    the `2 * geom.r_hub + 1` (11 px) cap the hub is drawn with. */
#define WATCH_HUB_DOT_W 5

/* -------------------------------------------------------------------------- */
/* Calendar                                                                   */
/* -------------------------------------------------------------------------- */

bool cgl_watch_is_leap(uint16_t year)
{
    return (((year % 4u) == 0u) && ((year % 100u) != 0u)) || ((year % 400u) == 0u);
}

uint8_t cgl_watch_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t len[12] = { 31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u };

    if ((month == 2u) && cgl_watch_is_leap(year))
    {
        return 29u;
    }
    return len[(uint8_t)((month - 1u) % 12u)];
}

/**
 * Advance a date by one day, including the month / year rollover.
 *
 * This is why the face can show a date at all: the framework has no RTC, so the date
 * is *counted* from the configured anchor at one day per 86400 s. It is only reached
 * when the count really crosses midnight, so the arithmetic here runs once a day
 * instead of once a frame (see `watch__sync()`).
 */
void cgl_watch_next_day(cgl_watch_time_t *t)
{
    if (t == NULL)
    {
        return;
    }

    t->wday = (uint8_t)((t->wday + 1u) % 7u);
    t->day++;
    if (t->day > cgl_watch_days_in_month(t->year, t->month))
    {
        t->day = 1u;
        t->month++;
        if (t->month > 12u)
        {
            t->month = 1u;
            t->year++;
        }
    }
}

uint32_t cgl_watch_seconds_of_day(const cgl_watch_time_t *t)
{
    if (t == NULL)
    {
        return 0u;
    }
    return ((uint32_t)t->h * 3600u) + ((uint32_t)t->m * 60u) + (uint32_t)t->s;
}

/* -------------------------------------------------------------------------- */
/* Time base                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * Derive `now` from the anchor and the frame clock: `now = t0 + (now_ms - t0_ms)`.
 *
 * The subtraction is unsigned on purpose: it is wrap-safe across the 49 day
 * rollover of a 32 bit millisecond counter, which is the same argument the port's
 * own clock makes (`dt = tick_ms() - last_tick`).
 *
 * The date is advanced by *whole days* rather than recomputed from scratch, so the
 * calendar arithmetic happens once per midnight and not once per frame.
 */
static void watch__sync(cgl_watch_t *w, uint32_t now_ms)
{
    uint32_t elapsed_ms = now_ms - w->t0_ms;
    uint32_t secs = elapsed_ms / CGL_CLOCK_MS_PER_S;
    uint32_t total = ((uint32_t)w->t0.h * 3600u) + ((uint32_t)w->t0.m * 60u) +
                     ((uint32_t)w->t0.s) + secs;
    uint32_t day = total / 86400u;
    uint32_t sod = total % 86400u;

    w->now.ms = (uint16_t)(elapsed_ms % CGL_CLOCK_MS_PER_S);
    w->now.h = (uint8_t)(sod / 3600u);
    w->now.m = (uint8_t)((sod % 3600u) / 60u);
    w->now.s = (uint8_t)(sod % 60u);

    /* WARNING: the date is **state, not a function of the anchor** -- it must not be
       recomputed from `t0` here. It was, in the first version of this function, and
       the result was a face that showed the right date for exactly one frame per
       midnight: the day is added by the loop below (which only runs while
       `elapsed_days < day`), so resetting the date from `t0` on the next frame threw
       the added day away again and the loop had nothing left to do. The anchor
       branch in `cgl_watch_frame()` installs the date when the anchor moves; from
       then on this loop is the only writer. */
    while (w->elapsed_days < day)
    {
        cgl_watch_next_day(&w->now);
        w->elapsed_days++;
    }
}

/** The second hand at this widget's configured motion: the framework's pure
    expression plus this face's mode, tick length and curve. WARNING: the motion is a
    *function of the time and the mode*, never a tween -- see the long note in
    `cgl_watch.h` for why that is what makes the hand survive a stall, a covered
    widget and any frame rate. */
CGL_AINLINE cgl_angle_t watch__second_angle(const cgl_watch_t *w, const cgl_watch_time_t *t)
{
    return cgl_watch_second_angle(t, w->cfg.anim.second_mode, w->cfg.anim.tick_ms,
                                  w->cfg.anim.tick_ease);
}

/**
 * The entrance: an angle that has swept from 12 o'clock towards `target` by
 * `reveal` (Q8) of the way.
 *
 * The delta is taken **forward** (modulo one turn), so the hands always sweep
 * clockwise the way a watch does; easing the tween with OUT_CUBIC gives the
 * "gather, then set" motion a watch face makes when it wakes up.
 */
static cgl_angle_t watch__revealed_angle(cgl_angle_t target, uint16_t reveal_q8)
{
    uint32_t delta = (uint32_t)(cgl_angle_t)(target - CGL_ANGLE_UP);

    return (cgl_angle_t)(CGL_ANGLE_UP + (uint32_t)((delta * (uint32_t)reveal_q8) >> 8));
}

/* -------------------------------------------------------------------------- */
/* Drawing helpers                                                            */
/* -------------------------------------------------------------------------- */

/** Point at `radius` and `a` around the dial centre, in whole pixels. */
static void watch__polar(const cgl_watch_t *w, int16_t radius, cgl_angle_t a, int16_t *x,
                         int16_t *y)
{
    int16_t dx;
    int16_t dy;

    cgl_math_polar(a, radius, &dx, &dy);
    *x = (int16_t)(w->cfg.geom.cx + dx);
    *y = (int16_t)(w->cfg.geom.cy + dy);
}

/** The same point in **Q6** (1/64 px): what a sweeping hand needs. The framework's
    `cgl_math_polar_q6()` is exactly this expression, so the sub-pixel part of a hand
    is not this file's business. */
static void watch__polar_q6(const cgl_watch_t *w, int16_t radius, cgl_angle_t a, int32_t *x_q6,
                            int32_t *y_q6)
{
    cgl_math_polar_q6(w->cfg.geom.cx, w->cfg.geom.cy, a, radius, x_q6, y_q6);
}

/* --- damage: what the hands and the ring actually change ------------------- */

/** The bbox of a hand (tip at `r_tip`, tail behind the centre at `r_tail`) at angle
    `a`, plus its pen and its drop shadow.
    WARNING: the shadow is part of the box, not decoration -- it puts ink
    `cfg.anim.shadow_off` further out than the geometry does, and forgetting it leaves
    a two pixel ghost of the shadow behind a hand that has moved, which is exactly the
    kind of bug `tests/watch_host_test.c`'s no-ghost case exists to catch. The
    framework's `cgl_arc_needle_damage()` is that box, so this file does not compute it
    a second time. */
CGL_AINLINE cgl_rect_t watch__damage_hand(const cgl_watch_t *w, cgl_rect_t dmg, cgl_angle_t a,
                                          int16_t r_tip, int16_t r_tail, int16_t half_w)
{
    return cgl_arc_needle_damage(dmg, w->cfg.geom.cx, w->cfg.geom.cy, a, r_tail, r_tip, half_w,
                                 w->cfg.anim.shadow_off);
}

/** The bbox of the seconds ring's leading end (the newly covered arc and its cap).

    WARNING: the end it is given has to be the **second hand's own angle**
    (`watch__second_angle()`), not a sweep computed from the time: in TICK mode the
    ring's leading end leaps by up to a whole second of dial inside one frame, and a
    box placed anywhere but where the arc really is leaves the leap as a stale orange
    streak on the panel. Same reason the *drawing* takes that angle (see
    `watch__seconds_ring()`).

    WARNING: the margin has to cover the **lead dot**, which is bigger than the stroke
    and grows during the minute pulse (`WATCH_DOT_R + pulse/64`, so `WATCH_DOT_R_MAX`
    at its widest). A box sized for the 3 px stroke alone is what this used to be, and
    the dot's own edge was then never repainted: measured against a full redraw the
    panel kept a crescent of the previous dot's position. */
static void watch__damage_ring_end(const cgl_watch_t *w, cgl_rect_t *dmg,
                                   const cgl_watch_time_t *t)
{
    const int16_t pad = (int16_t)((w->cfg.geom.ring_w / 2) + WATCH_DOT_R_MAX + 1);
    int16_t x;
    int16_t y;

    watch__polar(w, w->cfg.geom.r_ring, watch__second_angle(w, t), &x, &y);
    *dmg = cgl_rect_union(*dmg, cgl_rect_wh((int16_t)(x - pad), (int16_t)(y - pad),
                                            (int16_t)((2 * pad) + 1), (int16_t)((2 * pad) + 1)));
}

#if CGL_CFG_WATCH_TRAIL

/**
 * The trail slot of a mark.
 *
 * The array is `CGL_WATCH_TRAIL_MARKS` (60) entries -- one per *minute* mark of a 60
 * mark face -- and a face configured with more marks than that shares the last slot
 * between them: there is no per-element state for a 61st mark, and an index past the
 * end of the array would corrupt whatever follows it inside `cgl_watch_t`.
 */
CGL_AINLINE uint8_t watch__trail_slot(uint32_t mark)
{
    return (uint8_t)((mark < (uint32_t)CGL_WATCH_TRAIL_MARKS) ? mark
                                                              : ((uint32_t)CGL_WATCH_TRAIL_MARKS - 1u));
}

/**
 * Is the comet trail live?
 *
 * WARNING: `CGL_WATCH_F_TRAIL` set while `CGL_WATCH_F_TICKS` is clear is **nothing at
 * all**, not a hidden trail: the glow is carried by the chapter marks (there is no
 * other element it could be painted on), so with the marks off there is nothing to
 * light, nothing to age and nothing to damage. The two flags are therefore read
 * together everywhere the trail is touched -- the colouring in
 * `watch__chapter_ring()`, the re-light and the decay in `cgl_watch_frame()`, and the
 * damage. Reading only `CGL_WATCH_F_TRAIL` would leave a 60 byte state decaying for a
 * picture that does not exist, and -- worse -- would mark its damage in every frame.
 *
 * WARNING: `CGL_CFG_WATCH_TRAIL` is the *compile-time* switch and this is the runtime
 * one; with the compile switch off there is no `w->trail` at all and every trail path
 * above is inside `#if CGL_CFG_WATCH_TRAIL`, so a configuration that asks for a trail
 * in such a build is accepted and simply draws no comet.
 */
CGL_AINLINE bool watch__trail_live(const cgl_watch_t *w)
{
    return (w->cfg.flags & (uint16_t)(CGL_WATCH_F_TRAIL | CGL_WATCH_F_TICKS)) ==
           (uint16_t)(CGL_WATCH_F_TRAIL | CGL_WATCH_F_TICKS);
}

/** Every chapter mark that still carries comet glow (the trail fades slowly). */
static void watch__damage_trail(const cgl_watch_t *w, cgl_rect_t *dmg)
{
    uint8_t i;

    for (i = 0u; i < w->cfg.anim.tick_count; i++)
    {
        cgl_angle_t a;
        int16_t xa;
        int16_t ya;
        int16_t xb;
        int16_t yb;
        int16_t x0;
        int16_t y0;
        int16_t x1;
        int16_t y1;

        if (w->trail[watch__trail_slot(i)] == 0u)
        {
            continue;
        }
        a = cgl_watch_face_angle((uint32_t)i, (uint32_t)w->cfg.anim.tick_count);
        /* The box is sized from the **minor** mark's inner radius for every mark: a
           major one starts three pixels further in, and the three pixel pad below is
           what covers it. */
        watch__polar(w, w->cfg.geom.r_tick_minor_in, a, &xa, &ya);
        watch__polar(w, w->cfg.geom.r_tick_out, a, &xb, &yb);
        x0 = (int16_t)(((xa < xb) ? xa : xb) - 3);
        x1 = (int16_t)(((xa < xb) ? xb : xa) + 3);
        y0 = (int16_t)(((ya < yb) ? ya : yb) - 3);
        y1 = (int16_t)(((ya < yb) ? yb : ya) + 3);
        *dmg = cgl_rect_union(*dmg, cgl_rect_wh(x0, y0, (int16_t)(x1 - x0 + 1),
                                                (int16_t)(y1 - y0 + 1)));
    }
}

#endif /* CGL_CFG_WATCH_TRAIL */

/* -------------------------------------------------------------------------- */
/* Hands: a tapered antialiased blade, and the shadow it casts                */
/* -------------------------------------------------------------------------- */

/**
 * One hand: its shadow, then its ink. `bx/by` is the arbor end and `tx/ty` the tip,
 * both in Q6; the widths are full widths in whole pixels.
 *
 * The shadow is drawn **first and under the ink**, offset down and to the right,
 * which is where the light in this face does *not* come from (the dial's specular
 * highlight sits up and to the left). It is a blend rather than a second dark hand
 * for a reason worth stating: 30 % of `cfg.style.bg` over the dial *and over whatever
 * the hand crosses* is what makes the hand look like it is floating a millimetre
 * above the face, and a solid dark outline would look like a sticker.
 *
 * WARNING: the blade itself is `cgl_arc_stroke_*`, not a chain of
 * `cgl_canvas_line_thick()` calls. A thick line has a **constant** width, so a
 * tapered hand can only be faked out of several segments -- and the seams between
 * them bulge, which is the one thing a hand must not do. A stroke is a single
 * quadrilateral whose half width runs linearly from one end to the other, evaluated
 * per pixel from two dot products, so there is no seam to hide.
 */
static void watch__hand(const cgl_watch_t *w, cgl_canvas_t *c, int32_t bx_q6, int32_t by_q6,
                        int32_t tx_q6, int32_t ty_q6, int16_t w_base, int16_t w_tip,
                        cgl_color_t col)
{
    cgl_arc_stroke_t g;

    if (!cgl_arc_stroke_setup(&g, bx_q6, by_q6, tx_q6, ty_q6, w_base, w_tip))
    {
        return; /* a dot is not a hand, and the hub covers this case anyway */
    }
    cgl_arc_stroke_draw(c, &g, w->cfg.style.bg, w->cfg.anim.shadow_alpha, w->cfg.anim.shadow_off,
                        w->cfg.anim.shadow_off);
    cgl_arc_stroke_draw(c, &g, col, 256u, 0, 0);
}

/**
 * Lay a hand down at an angle, from `r_base` **behind** the centre (where the hub
 * covers it) out to `r_tip`, and return its tip in whole pixels.
 *
 * The tip comes back because the damage model needs exactly the points this drawing
 * used: `cgl_watch_frame()` marks the bbox of the tip and the tail, and if the two
 * ever disagree the panel keeps a ghost of the hand's previous position.
 *
 * WARNING: therefore the endpoints are **whole pixels** here, not the Q6 the stroke is
 * capable of -- see the note on creep in `cgl_watch_frame()`. The second hand is the
 * exception and uses `watch__polar_q6()` directly.
 */
static void watch__lay_hand(const cgl_watch_t *w, cgl_canvas_t *c, cgl_angle_t a, int16_t r_base,
                            int16_t r_tip, int16_t w_base, int16_t w_tip, cgl_color_t col,
                            cgl_point_t *tip)
{
    int16_t bx;
    int16_t by;

    watch__polar(w, r_tip, a, &tip->x, &tip->y);
    watch__polar(w, r_base, (cgl_angle_t)(a + CGL_ANGLE_HALF), &bx, &by);
    watch__hand(w, c, (int32_t)bx << 6, (int32_t)by << 6, (int32_t)tip->x << 6,
                (int32_t)tip->y << 6, w_base, w_tip, col);
}

/* -------------------------------------------------------------------------- */
/* The dial: the chapter ring and the seconds indicator                       */
/* -------------------------------------------------------------------------- */

/**
 * The chapter ring: `cfg.anim.tick_count` marks, every `cfg.anim.tick_major`-th of
 * them longer, thicker and brighter (60 and 5 = one per second and the hours).
 *
 * `reveal` draws the ring **mark by mark**, so the face assembles itself during the
 * entrance instead of simply appearing. A mark that still carries comet glow is
 * blended toward `cfg.style.trail` first.
 *
 * WARNING: the mark count and the major interval are runtime fields, and both have a
 * degenerate case that would be a division by zero or a loop that never ends if they
 * were used as written: `tick_major == 0` here means "no mark is a major one" (there
 * is no sensible "every zeroth mark"), and `tick_count == 0` draws no ring at all.
 */
static void watch__chapter_ring(const cgl_watch_t *w, cgl_canvas_t *c, uint16_t reveal)
{
    const uint8_t count = w->cfg.anim.tick_count;
    uint32_t lit = ((uint32_t)reveal * (uint32_t)count) >> 8;
    uint8_t i;

    for (i = 0u; i < count; i++)
    {
        bool major = (w->cfg.anim.tick_major != 0u) && ((i % w->cfg.anim.tick_major) == 0u);
        cgl_angle_t a = cgl_watch_face_angle((uint32_t)i, (uint32_t)count);
        int16_t xa;
        int16_t ya;
        int16_t xb;
        int16_t yb;
        cgl_color_t col = major ? w->cfg.style.tick_major : w->cfg.style.tick;

        if ((uint32_t)i >= lit)
        {
            break;
        }
#if CGL_CFG_WATCH_TRAIL
        if (watch__trail_live(w))
        {
            uint8_t glow = w->trail[watch__trail_slot((uint32_t)i)];

            if (glow != 0u)
            {
                col = cgl_color_blend_q8(col, w->cfg.style.trail, (uint16_t)glow);
            }
        }
#endif
        watch__polar(w, major ? w->cfg.geom.r_tick_major_in : w->cfg.geom.r_tick_minor_in, a, &xa,
                     &ya);
        watch__polar(w, w->cfg.geom.r_tick_out, a, &xb, &yb);
        /* A 1 px radial tick used to be a Bresenham line, which is crisp but has a
           visible staircase along its *sides* at a shallow angle. At two pixels with
           coverage it costs a few multiplies and reads as a clean tick. The hour marks
           are one pixel wider than the minute ones -- that and their extra length are
           the whole of "this is an hour". */
        cgl_canvas_line_thick(c, xa, ya, xb, yb,
                              (int16_t)(major ? w->cfg.anim.tick_w_major
                                              : w->cfg.anim.tick_w_minor),
                              col);
    }
}

/**
 * The seconds indicator: the accent arc running along the middle of the outer ring's
 * well, and the bright dot that leads it.
 *
 * There is no separate dark track. The well **is** the track: it is a machined metal
 * band with a lit crown at exactly `cfg.geom.r_ring` (see the `style.well` surface), so
 * a second stroke of a flat colour on top of it would only cover the cross-section the
 * ring is made of -- and the progress reads perfectly well without one, because "accent
 * from 12 o'clock round to here" is the whole message. WARNING: that is also why this
 * is drawn with `cgl_canvas_arc()` directly and not through `cgl_arc_gauge`: a gauge
 * always draws a track, and this ring has none.
 *
 * The dot is the cheapest piece of "expensive looking" detail on the face: during the
 * minute pulse it grows a few pixels, which reads as the ring taking a breath.
 *
 * WARNING: the ring's leading end is **the second hand's own angle**, not a sweep
 * computed from the time. That is deliberate and it is what keeps the two motions
 * honest: in SWEEP mode the two are the same number (so nothing changed), and in TICK
 * mode the arc, the cap and the hand step *together* -- a ring that went on gliding
 * while the hand ticked would read as the hand having come loose from its own seconds
 * track. The subtraction below is unsigned for the same reason `watch__sync()`'s is:
 * the hand's angle wraps through 0 at 12 o'clock every minute, and a signed difference
 * would turn that wrap into a whole negative turn of arc.
 */
static void watch__seconds_ring(const cgl_watch_t *w, cgl_canvas_t *c, uint16_t reveal,
                                cgl_angle_t a_sec)
{
    uint32_t full = (uint32_t)(cgl_angle_t)(a_sec - CGL_ANGLE_UP);
    uint32_t sweep = (full * (uint32_t)reveal) >> 8;
    uint16_t pulse = (uint16_t)cgl_anim_value(&w->pulse);
    int16_t x;
    int16_t y;

    if (sweep > 0u)
    {
        cgl_canvas_arc(c, w->cfg.geom.cx, w->cfg.geom.cy, w->cfg.geom.r_ring, w->cfg.geom.ring_w,
                       CGL_ANGLE_UP, sweep, w->cfg.style.accent);
        /* The cap sits **on the leading end of the arc**, and it is not drawn at all
           while the ring is empty -- there it would sit alone at 12 o'clock and read
           as a stray dot. */
        watch__polar(w, w->cfg.geom.r_ring, (cgl_angle_t)(CGL_ANGLE_UP + (cgl_angle_t)sweep), &x,
                     &y);
        cgl_canvas_disc(c, x, y, (int16_t)(WATCH_DOT_R + (pulse >> 6)), w->cfg.style.accent_hi);
    }
}

/**
 * The case lip: the antialiased ring the face and the well are set into, plus the lit
 * half of it. The lit segment is the same idea as the dial's own highlight -- one
 * light, in the upper left -- and it is what stops the outer edge from reading as a
 * uniform outline: metal has a bright side and a dark side.
 *
 * The dark hairline that used to run at `r_face - 5` is gone: that radius is the middle
 * of the well now, and a flat line across its lit crown would cut the cylinder in half.
 * The seam between the well and the dial is the first entry of the cross-section
 * instead, which is where it belongs.
 */
static void watch__bezel(const cgl_watch_t *w, cgl_canvas_t *c)
{
    cgl_canvas_arc(c, w->cfg.geom.cx, w->cfg.geom.cy, w->cfg.geom.r_face,
                   (int16_t)w->cfg.style.bezel_w, 0u, CGL_ANGLE_TURN, w->cfg.style.bezel);
    /* From 150 degrees (below 9 o'clock) round to 270 degrees (12 o'clock): in this
       convention 0 is 3 o'clock and the angle grows clockwise. */
    cgl_canvas_arc(c, w->cfg.geom.cx, w->cfg.geom.cy, w->cfg.geom.r_face,
                   (int16_t)w->cfg.style.bezel_w, w->cfg.style.bezel_lit_start,
                   w->cfg.style.bezel_lit_sweep, w->cfg.style.bezel_hi);
}

/* -------------------------------------------------------------------------- */
/* The crystal: a glass cover over the whole dial                             */
/* -------------------------------------------------------------------------- */

/*
 * Why the cover is the *last* thing drawn, and why it is an alpha composite
 * --------------------------------------------------------------------------
 * A dial can be shaded into something that looks like metal without ever saying what
 * it is *behind*. What says "there is a crystal over this" is not another surface --
 * it is that the light bouncing off the crystal lies **on top of everything**,
 * including the hands: a reflection that stops at the edge of a hand is painted on
 * the dial, a reflection that runs across the hand is in front of it. So the cover is
 * the final pass, and it has to be a *blend*, because what is underneath may be the
 * dial, the well, a mark, a hand or the hub -- four different colours at four
 * different luminance levels, none of which an opaque primitive can lighten
 * correctly.
 *
 * What the eye actually reads a crystal by
 * ----------------------------------------
 *  1. **The sweep**: a wide, very soft band of reflected light crossing the dial.
 *     It is a function of one dot product (the pixel's position along the light's
 *     axis), so it costs two multiplies and a table-free quadratic falloff -- and it
 *     is what makes the surface read as *glossy* rather than matte.
 *  2. **The crescent at the rim**: the glass curves steeply at its edge, so the
 *     reflection there compresses into a thin bright arc that follows the ring. This
 *     is the strongest cue of the three and the cheapest: an annulus test on `r^2`
 *     plus the *same* dot product, windowed to the lit side.
 *  3. **The echo**: a much fainter second reflection, diametrically opposite, off the
 *     inside of the dome. It is the same two tests with the sign of the dot product
 *     flipped, which is why it is nearly free -- and it is the detail that stops the
 *     cover from looking like one airbrushed stripe.
 *
 * WARNING: the reflections are the *whole* of the cover here; the clip that used to
 * end it at the dial (`r <= r_face`) is a property of a *pass* and not of the light,
 * and a `cgl_watch_glass_t` carries the light and nothing else. See
 * `cgl_watch_glass_shade()`.
 */

#if CGL_CFG_WATCH_GLASS

/**
 * The three divisions a `cgl_watch_glass_t` implies, plus the dial to clip against.
 *
 * They are per *pass*, not per pixel, and that is the point of having them here: the
 * demo's were compile-time constants (`CLOCK_GLASS_SWEEP_K` and friends), and the
 * arithmetic is the same expression -- `(alpha << 16) / w^2` and `(alpha << 8) / ramp`
 * -- evaluated once instead of once per pixel.
 *
 * WARNING: the two denominators are runtime fields now and a zero one is a division by
 * zero. The guards below are not cosmetic: at the reference values (`sweep_w` 52,
 * `edge_ramp` 54, `echo_ramp` 30) they never fire, so the reference cover is the same
 * numbers it always was, and a degenerate parameter set draws no reflection instead of
 * trapping.
 */
typedef struct
{
    uint32_t sweep_k; /**< the sweep's curvature, Q16 of the alpha                    */
    uint32_t edge_k;  /**< the rim crescent's ramp                                    */
    uint32_t echo_k;  /**< ... and the echo's                                        */
    int32_t r2_clip;  /**< the dial's squared radius, or 0 for "no dial to clip to"   */
} watch__glass_k_t;

CGL_AINLINE void watch__glass_prepare(const cgl_watch_glass_t *gl, int32_t r_face,
                                      watch__glass_k_t *k)
{
    const int32_t sweep_w = (int32_t)gl->sweep_w;
    const int32_t edge_ramp = (int32_t)gl->edge_ramp;
    const int32_t echo_ramp = (int32_t)gl->echo_ramp;

    k->sweep_k = (sweep_w > 0)
                     ? (uint32_t)(((uint32_t)gl->sweep_alpha << 16) /
                                  ((uint32_t)sweep_w * (uint32_t)sweep_w))
                     : 0u;
    k->edge_k = (edge_ramp > 0)
                    ? (uint32_t)(((uint32_t)gl->edge_alpha << 8) / (uint32_t)edge_ramp)
                    : 0u;
    k->echo_k = (echo_ramp > 0)
                    ? (uint32_t)(((uint32_t)gl->echo_alpha << 8) / (uint32_t)echo_ramp)
                    : 0u;
    k->r2_clip = (r_face > 0) ? (r_face * r_face) : 0;
}

/**
 * How much light the crystal reflects at an offset from the dial centre, Q8.
 *
 * One dot product with the light's direction carries all three terms: it *is* the
 * sweep's position (after a quadratic falloff), and it is also the crescent's angular
 * window -- the lit side of the rim is "positive", the echo's side "negative". That is
 * the whole reason the cover is affordable: a per-pixel `atan2` would cost more than
 * every other pixel of the dial put together.
 *
 * `k->r2_clip <= 0` means "no dial to clip against", which is what the parameter-only
 * form of the cover uses (see `cgl_watch_glass_shade()`); the band pass passes the
 * real radius, exactly as the demo's `clock__glass_cov()` tested `CLOCK_R_FACE`. The
 * test can only ever fire through the public accessors -- the pass' own row span is
 * `sqrt(r_face^2 - dy^2)`, which keeps every pixel it asks about on the dial by
 * construction -- so no drawn pixel depends on it either way.
 *
 * Pure, like the rest of the face: same offset, same value, always.
 */
CGL_AINLINE uint16_t watch__glass_cov_q8(const cgl_watch_glass_t *gl, const watch__glass_k_t *k,
                                        int32_t dx, int32_t dy)
{
    int32_t d2 = (dx * dx) + (dy * dy);
    int32_t s;
    int32_t t;
    uint32_t cov = 0u;

    if ((k->r2_clip > 0) && (d2 > k->r2_clip))
    {
        return 0u; /* the cover ends with the dial */
    }

    s = ((dx * (int32_t)gl->ux) + (dy * (int32_t)gl->uy)) >> 8;

    /* --- the sweep --- */
    t = s - (int32_t)gl->sweep_at;
    if (t < 0)
    {
        t = -t;
    }
    if (t < (int32_t)gl->sweep_w)
    {
        uint32_t kk = (uint32_t)((int32_t)gl->sweep_w - t);

        cov = ((kk * kk) * k->sweep_k) >> 16;
    }

    /* --- the crescent at the rim, or its echo --- */
    if ((d2 >= ((int32_t)gl->edge_in * (int32_t)gl->edge_in)) &&
        (d2 <= ((int32_t)gl->edge_out * (int32_t)gl->edge_out)) && (s > (int32_t)gl->edge_at))
    {
        uint32_t c = ((uint32_t)(s - (int32_t)gl->edge_at) * k->edge_k) >> 8;

        if (c > cov)
        {
            cov = c;
        }
    }
    else if ((d2 >= ((int32_t)gl->echo_in * (int32_t)gl->echo_in)) &&
             (d2 <= ((int32_t)gl->echo_out * (int32_t)gl->echo_out)) &&
             (s < -(int32_t)gl->echo_at))
    {
        uint32_t c = ((uint32_t)(-s - (int32_t)gl->echo_at) * k->echo_k) >> 8;

        if (c > cov)
        {
            cov = c;
        }
    }

    return (cov > 255u) ? 255u : (uint16_t)cov;
}

/**
 * Lay the crystal over everything drawn so far: the dial's rows for this band, the
 * cover's alpha per pixel, and one blend. The row pointer and the clip are resolved
 * once per row for the same reason the surfaces resolve them -- this pass covers every
 * pixel of the dial, so the per-pixel cost is the whole cost.
 *
 * WARNING: like a surface, this is skipped while a **warp** is installed. During a
 * suction transition the clip is in destination space, and paying a per-pixel
 * `cgl_color_blend_q8()` for a reflection nobody can see is not worth a frame rate.
 */
static void watch__glass(const cgl_watch_t *w, cgl_canvas_t *c)
{
    const cgl_watch_glass_t *gl = &w->cfg.style.glass;
    watch__glass_k_t k;
    int16_t x0 = (int16_t)(w->cfg.geom.cx - w->cfg.geom.r_face);
    int16_t x1 = (int16_t)(w->cfg.geom.cx + w->cfg.geom.r_face);
    int16_t y0 = (int16_t)(w->cfg.geom.cy - w->cfg.geom.r_face);
    int16_t y1 = (int16_t)(w->cfg.geom.cy + w->cfg.geom.r_face);
    const int32_t r2 = (int32_t)w->cfg.geom.r_face * (int32_t)w->cfg.geom.r_face;
    int16_t y;

    if (c->warp != NULL)
    {
        return;
    }

    watch__glass_prepare(gl, w->cfg.geom.r_face, &k);

    if (x0 < (int16_t)(c->clip.x0 - c->pan.x))
    {
        x0 = (int16_t)(c->clip.x0 - c->pan.x);
    }
    if (x1 > (int16_t)(c->clip.x1 - c->pan.x))
    {
        x1 = (int16_t)(c->clip.x1 - c->pan.x);
    }
    if (y0 < (int16_t)(c->clip.y0 - c->pan.y))
    {
        y0 = (int16_t)(c->clip.y0 - c->pan.y);
    }
    if (y1 > (int16_t)(c->clip.y1 - c->pan.y))
    {
        y1 = (int16_t)(c->clip.y1 - c->pan.y);
    }
    if (y0 < (int16_t)(c->origin_y - c->pan.y))
    {
        y0 = (int16_t)(c->origin_y - c->pan.y);
    }
    if (y1 > (int16_t)(c->origin_y + c->height - 1 - c->pan.y))
    {
        y1 = (int16_t)(c->origin_y + c->height - 1 - c->pan.y);
    }
    if (x0 > x1)
    {
        return;
    }

    for (y = y0; y <= y1; y++)
    {
        int32_t dy = (int32_t)y - w->cfg.geom.cy;
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
        span = (int32_t)cgl_math_isqrt((uint32_t)(r2 - d2r));
        xa = (int32_t)w->cfg.geom.cx - span;
        xb = (int32_t)w->cfg.geom.cx + span;
        if (xa < (int32_t)x0)
        {
            xa = (int32_t)x0;
        }
        if (xb > (int32_t)x1)
        {
            xb = (int32_t)x1;
        }
        for (x = xa; x <= xb; x++)
        {
            uint16_t cov = watch__glass_cov_q8(gl, &k, x - (int32_t)w->cfg.geom.cx, dy);

            if (cov != 0u)
            {
                int16_t sx = (int16_t)(x + c->pan.x);

                row[sx] = (cov >= 256u) ? gl->tint : cgl_color_blend_q8(row[sx], gl->tint, cov);
            }
        }
    }
}

#endif /* CGL_CFG_WATCH_GLASS */

/* -------------------------------------------------------------------------- */
/* The dial widget                                                            */
/* -------------------------------------------------------------------------- */

/** The minute pulse's tint: the whole face blinks once as the minute turns over, and
    the hands and the hub cap are what carries it. `pulse` is Q8 and the blend is by
    half of it, because the pulse's own value is used for the ring's dot as well and a
    full-strength tint would wash the hands out entirely.
    WARNING: this is gated by `CGL_WATCH_F_PULSE` and not only by the tween, exactly as
    the demo gated it: with the flag off the tween never starts, so the value is 0 and
    this is the identity -- but a caller that sets the tween's value directly (there is
    no public door, deliberately) still gets an untinted face. */
CGL_AINLINE cgl_color_t watch__pulse_tint(const cgl_watch_t *w, cgl_color_t base, uint16_t pulse)
{
    if ((pulse == 0u) || ((w->cfg.flags & CGL_WATCH_F_PULSE) == 0u))
    {
        return base;
    }
    return cgl_color_blend_q8(base, w->cfg.style.accent_hi, (uint16_t)(pulse >> 1));
}

static void watch__draw(cgl_widget_t *widget, cgl_canvas_t *c)
{
    cgl_watch_t *w = (cgl_watch_t *)widget;
    const uint16_t reveal = (uint16_t)cgl_anim_value(&w->reveal);
    const uint16_t pulse = (uint16_t)cgl_anim_value(&w->pulse);
    const cgl_color_t hand_col = watch__pulse_tint(w, w->cfg.style.hand, pulse);
    const cgl_color_t hub_col = watch__pulse_tint(w, w->cfg.style.hub, pulse);
    cgl_angle_t a_hour;
    cgl_angle_t a_min;
    cgl_angle_t a_sec;
    cgl_point_t tip;
    int32_t tip_x_q6;
    int32_t tip_y_q6;
    int32_t tail_x_q6;
    int32_t tail_y_q6;

    /* --- the face and the ring it is set into -----------------------------
     * Two surfaces, both computed per pixel and neither stored: the *dial* (a radial
     * gradient, a brushed texture and the specular highlight) and the *well* the
     * outer ring turns in (a cylindrical cross-section, shaded top-dark /
     * bottom-bright). WARNING: both are drawn from the surface's **own** centre
     * (`style.face.cx/cy`), which `cgl_watch_set_center()` moves with the geometry --
     * a caller that edits `geom.cx` by hand has to move them too.
     * --------------------------------------------------------------------- */
    cgl_arc_surface_paint(c, &w->cfg.style.face);
    cgl_arc_surface_paint(c, &w->cfg.style.well);

    if ((w->cfg.flags & CGL_WATCH_F_BEZEL) != 0u)
    {
        watch__bezel(w, c);
    }

    if ((w->cfg.flags & CGL_WATCH_F_RING) != 0u)
    {
        const cgl_angle_t ring_end =
            watch__revealed_angle(watch__second_angle(w, &w->now), reveal);

        watch__seconds_ring(w, c, reveal, ring_end);
    }

    if ((w->cfg.flags & CGL_WATCH_F_TICKS) != 0u)
    {
        watch__chapter_ring(w, c, reveal);
    }

    /* --- the brand, low in the dial like a printed logo ------------------- */
    if ((w->cfg.flags & CGL_WATCH_F_BRAND) != 0u)
    {
        if ((w->cfg.brand != NULL) && (w->cfg.brand_font != NULL))
        {
            (void)cgl_font_draw_text(c, w->cfg.brand_font,
                                     (int16_t)(w->cfg.geom.cx + w->cfg.brand_dx),
                                     (int16_t)(w->cfg.geom.cy + w->cfg.brand_dy), w->cfg.brand,
                                     w->cfg.brand_color);
        }
    }

    /* --- the hands --------------------------------------------------------
     * Order matters: hour first, then the minute over it, then the second hand on
     * top, then the hub over all three. That is how a movement stacks them, it is
     * what hides the three wide bases at the centre, and it is what makes each hand's
     * shadow fall on the hand below it -- which is the detail that sells the depth,
     * because it is the one a photograph of a real watch shows.
     * --------------------------------------------------------------------- */
    a_hour = watch__revealed_angle(cgl_watch_hour_angle(&w->now), reveal);
    a_min = watch__revealed_angle(cgl_watch_minute_angle(&w->now), reveal);
    a_sec = watch__revealed_angle(watch__second_angle(w, &w->now), reveal);

    if ((w->cfg.flags & CGL_WATCH_F_HOUR) != 0u)
    {
        watch__lay_hand(w, c, a_hour, w->cfg.hour.r_base, w->cfg.hour.r_tip, w->cfg.hour.w_base,
                        w->cfg.hour.w_tip, hand_col, &tip);
    }
    if ((w->cfg.flags & CGL_WATCH_F_MINUTE) != 0u)
    {
        watch__lay_hand(w, c, a_min, w->cfg.minute.r_base, w->cfg.minute.r_tip,
                        w->cfg.minute.w_base, w->cfg.minute.w_tip, hand_col, &tip);
    }

    if ((w->cfg.flags & CGL_WATCH_F_SECOND) != 0u)
    {
        /* The second hand: a needle with a counterweight tail, drawn as one blade
           through the centre. Its endpoints are the only ones on the face in Q6, which
           is what lets it move by *less than a pixel* between frames -- the glide of
           the sweep, and (in TICK mode) the smoothness of the whip itself, which would
           come out as a staircase of six discrete poses at whole-pixel endpoints. */
        watch__polar_q6(w, w->cfg.second.r_tip, a_sec, &tip_x_q6, &tip_y_q6);
        watch__polar_q6(w, w->cfg.geom.r_tail, (cgl_angle_t)(a_sec + CGL_ANGLE_HALF), &tail_x_q6,
                        &tail_y_q6);
        watch__hand(w, c, tail_x_q6, tail_y_q6, tip_x_q6, tip_y_q6, w->cfg.second.w_base,
                    w->cfg.second.w_tip, w->cfg.style.accent);
        /* The counterweight is a dot: a stroke of width 2r on a radius of 0 (see
           `cgl_canvas_arc`). It is the same thing as `cgl_canvas_disc()` except that
           its edge is antialiased, which is what a 6 px round dot wants. */
        cgl_canvas_arc(c, (int16_t)(tail_x_q6 >> 6), (int16_t)(tail_y_q6 >> 6), 0,
                       WATCH_COUNTERWEIGHT_R, 0u, CGL_ANGLE_TURN, w->cfg.style.accent);
    }

    if ((w->cfg.flags & CGL_WATCH_F_HUB) != 0u)
    {
        cgl_canvas_arc(c, w->cfg.geom.cx, w->cfg.geom.cy, 0, (int16_t)((2 * w->cfg.geom.r_hub) + 1),
                       0u, CGL_ANGLE_TURN, hub_col);
        cgl_canvas_arc(c, w->cfg.geom.cx, w->cfg.geom.cy, 0, WATCH_HUB_DOT_W, 0u, CGL_ANGLE_TURN,
                       w->cfg.style.accent);
    }

    /* --- and last of all, the crystal over the lot ------------------------
     * Everything above is *behind* the glass: the reflection has to cross the hands,
     * not stop at their edges.
     * --------------------------------------------------------------------- */
#if CGL_CFG_WATCH_GLASS
    if ((w->cfg.flags & CGL_WATCH_F_GLASS) != 0u)
    {
        watch__glass(w, c);
    }
#endif
}

static const cgl_widget_vt_t watch__vt = {
    .type = "cgl_watch",
    .draw = watch__draw,
    .update = NULL, /* nothing to animate: cgl_watch_frame() decides what moves */
    .event = NULL,
};

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * Invalidate the widget, unless the frame cannot have moved anything.
 *
 * A frame with `dt == 0` (the application's very first frame) redraws an identical
 * picture: skipping it saves a whole screen of SPI for no visible difference.
 * Anything the widget has just *changed* is invalidated through its own setter anyway.
 */
static void watch__touch(cgl_widget_t *widget, uint32_t dt_ms)
{
    if (dt_ms > 0u)
    {
        cgl_widget_invalidate(widget);
    }
}

/**
 * Start the entrance tween, if the flag allows it.
 *
 * WARNING: this deliberately does **not** invalidate anything, and the two callers
 * want it that way for opposite reasons. A page's `on_enter` (through
 * `cgl_watch_reveal()`) has to repaint unconditionally -- it has just changed what the
 * face looks like and the band walker has no previous frame to compare against -- while
 * the *first frame* must not: the application's very first frame has `dt == 0` and
 * redraws an identical picture, so `watch__touch()` skips it and saves a whole screen
 * of SPI (see the note there). Starting the tween and marking the damage are therefore
 * two separate things here, which is exactly how the demo had them.
 */
static void watch__start_reveal(cgl_watch_t *w)
{
    if ((w->cfg.flags & CGL_WATCH_F_REVEAL) == 0u)
    {
        return;
    }
    cgl_anim_start(&w->reveal, 0, 256, w->cfg.anim.reveal_ms, (cgl_ease_t)w->cfg.anim.reveal_ease);
}

void cgl_watch_init(cgl_watch_t *w, const char *name, cgl_rect_t frame,
                    const cgl_watch_config_t *cfg)
{
    if (w == NULL)
    {
        return;
    }

    cgl_widget_init(&w->base, &watch__vt, name, frame);
    w->base.flags |= CGL_WF_CLIP;
    /* The watch **is** the widget: there is no child widget carrying a back pointer,
       and the dial is drawn from the configuration, so there is no tree to build. */
    w->base.user = NULL;

    if (cfg != NULL)
    {
        w->cfg = *cfg;
    }
    else
    {
        cgl_watch_config_default(&w->cfg);
    }

    w->t0 = w->cfg.start;
    w->t0_ms = 0u;
    w->elapsed_days = 0u;
    /* WARNING: `set_pending` / `set_to` are deliberately **not** cleared here. A caller
       may set the time before the widget is created -- "the RTC was read at boot, show
       it when the face first comes up" -- and an init that wiped the parked request
       would silently start the watch at `cfg.start` instead. (`cgl_watch_set_time()` is
       the door for a widget that already exists; a request parked that early is written
       into `set_to`/`set_pending` directly, because a `cgl_watch_t` is public.) */
    w->now = w->t0;
    w->prev_time = w->t0;

    /* 0xFF = "no frame yet": the first frame adopts the reading instead of firing a
       phantom minute pulse, and `primed` is what says so. */
    w->primed = 0u;
    w->prev_second = 0xFFu;
    w->prev_minute = 0xFFu;
    w->prev_day = 0xFFu;
    /* There is no "as drawn last" before the first frame. A tip of (0, 0) is not a
       point any hand can occupy, so the first creeping frame marks both hands damaged
       once -- a repaint, never a ghost -- and the demo's static storage was zeroed the
       same way. */
    w->prev_minute_pt.x = 0;
    w->prev_minute_pt.y = 0;
    w->prev_hour_pt.x = 0;
    w->prev_hour_pt.y = 0;

    cgl_anim_init(&w->reveal);
    cgl_anim_init(&w->pulse);

    /* The entrance, started **once**, here, and not from `cgl_watch_frame()`.
     *
     * WARNING: this used to hang off "the first frame that has to adopt a reading",
     * which is also the first frame after every `cgl_watch_set_time()`. That made a
     * time set replay the whole 760 ms sweep: the hands left for 12:00 and crept back,
     * so for eight tenths of a second after any `set_time()` the face showed a *pose
     * between twelve and the time it was asked for*. It is not a subtle bug -- the
     * demo's "no hand is ever broken" case drives the second hand to all 60 marks and
     * measures its thickness, and 53 of the 60 came out at nought pixels because the
     * hand was still on its way round.
     *
     * The demo had it right and this is where it differs: `on_create` did **not** start
     * the reveal, `on_enter` did, and `set_time` did not. A widget has no `on_enter`,
     * so the honest equivalent is this: `init()` plays it once (when the flag asks for
     * it), `cgl_watch_reveal()` replays it, and nothing else touches it. A caller that
     * wants the sweep on every page entry calls `cgl_watch_reveal()` from its own
     * `on_enter` -- which is exactly what the demo page does, because arriving at a
     * watch face is the one moment that animation belongs to. */
    watch__start_reveal(w);

#if CGL_CFG_WATCH_TRAIL
    {
        uint8_t i;

        for (i = 0u; i < CGL_WATCH_TRAIL_MARKS; i++)
        {
            w->trail[i] = 0u;
        }
    }
    w->trail_accum = 0u;
#endif

    w->frames = 0u;
}

/**
 * One frame: keep time, run the entrance and the pulse, age the comet trail, and mark
 * **only the damage the face really changed**.
 *
 * The order below is the demo's and it is not arbitrary: the anchor is applied before
 * the time is derived from it, the events are detected before the tween that answers
 * them is started, the trail is re-lit before it is aged (so a mark lit this frame is
 * not decayed in the same frame), and the damage is marked after everything that could
 * have moved this frame has moved.
 */
void cgl_watch_frame(cgl_watch_t *w, const cgl_clock_t *clk)
{
    bool primed;
    bool minute_changed;
#if CGL_CFG_WATCH_TRAIL
    bool second_changed;
#endif

    /* There is no time to show and nothing that could have moved. This is what makes
       "call it from on_frame with whatever you were handed" safe. */
    if ((w == NULL) || (clk == NULL))
    {
        return;
    }

    w->frames++;

    primed = (w->primed != 0u);

    /* A time set from outside (an RTC, another page, a test) is anchored **here**,
       where the frame clock is actually available: the widget is never handed
       `now_ms` outside this callback. */
    if (w->set_pending)
    {
        w->t0 = w->set_to;
        w->t0_ms = clk->now_ms;
        w->elapsed_days = 0u;
        w->set_pending = false;
        /* The anchor also installs the date: from here on `watch__sync()` only ever
           moves it **forward**, one day at a time (see the warning there). */
        w->now.year = w->set_to.year;
        w->now.month = w->set_to.month;
        w->now.day = w->set_to.day;
        w->now.wday = w->set_to.wday;
        w->prev_second = 0xFFu;
        w->prev_minute = 0xFFu;
        w->prev_day = 0xFFu;
        w->primed = 0u;
        primed = false;
    }

    watch__sync(w, clk->now_ms);

    /* The integer second only starts an animation when the comet trail is compiled in
       (it is what re-lights a mark): with `CGL_CFG_WATCH_TRAIL == 0` there is no trail
       state to key off it, so the test is not even computed. */
#if CGL_CFG_WATCH_TRAIL
    second_changed = (w->now.s != w->prev_second);
#endif
    minute_changed = (w->now.m != w->prev_minute);

    if (!primed)
    {
        /* The first frame (or the first after an anchor) **adopts** the reading: no
           minute pulse is fired for a minute that did not really turn over, and the
           whole widget is invalidated by the damage block below -- through
           `watch__touch()`, so a `dt == 0` first frame really does redraw nothing.
           `prev_day` is kept up to date with the rest of the edge-detection state, but
           nothing on this dial reacts to it: the date line is the page's.
           WARNING: the entrance is **not** started here -- see the long note in
           `cgl_watch_init()`. Re-priming after a `set_time()` must not replay it. */
        w->primed = 1u;
    }
    else
    {
        if (minute_changed && ((w->cfg.flags & CGL_WATCH_F_PULSE) != 0u))
        {
            /* The pulse runs **256 -> 0**, not 0 -> 256: its resting value has to be
               the "nothing is happening" value, or every draw that multiplies by it
               stays tinted for the rest of the day after the first minute change. */
            cgl_anim_start(&w->pulse, 256, 0, w->cfg.anim.pulse_ms,
                           (cgl_ease_t)w->cfg.anim.pulse_ease);
        }
    }

#if CGL_CFG_WATCH_TRAIL
    /* --- the second hand's comet trail ------------------------------------ */
    if (watch__trail_live(w))
    {
        if (second_changed)
        {
            /* The trail is indexed by the **mark**, not by the second: on a 60 mark
               face the two are the same number (which is why this was `trail[now.s]`),
               and on any other face the mark is where the hand really is. The array
               has 60 entries; `watch__trail_slot()` is what keeps a face with more
               marks than that inside it. */
            uint32_t mark = ((uint32_t)w->now.s * (uint32_t)w->cfg.anim.tick_count) / 60u;

            w->trail[watch__trail_slot(mark)] = 255u;
        }

        /* One decay rate for all the marks: a mark that would go negative is simply
           cleared. The loop is 60 byte operations, i.e. nothing next to the ring it
           lights.
           WARNING: the remainder is **carried** in `trail_accum`, because the decay per
           frame is a fraction of a unit: at 16 ms frames and a 2400 ms trail the exact
           rate is 1.7 units per frame, and a bare `(dt * 256) / trail_ms` truncates
           that to 1 (the trail then lasts 4 s instead of 2.4) or, below 9.4 ms per
           frame, to 0 -- a trail that never fades at all.
           WARNING: `trail_ms == 0` is the degenerate "no lifetime to measure" case and
           the division below is what would trap: with it the glow is simply left alone,
           which is the caller asking for a mark that stays lit. Turning
           `CGL_WATCH_F_TRAIL` off is how a caller asks for no comet at all. */
        if (w->cfg.anim.trail_ms != 0u)
        {
            uint32_t accum = (uint32_t)w->trail_accum + ((uint32_t)clk->dt_ms * 256u);
            uint32_t decay = accum / (uint32_t)w->cfg.anim.trail_ms;
            uint8_t i;

            w->trail_accum = (uint16_t)(accum % (uint32_t)w->cfg.anim.trail_ms);

            if (decay > 0u)
            {
                /* Every slot is aged, not just the marks this face draws: a face with
                   more marks than slots shares the last ones, and a slot that is left
                   out of the decay would stay lit for ever. */
                for (i = 0u; i < CGL_WATCH_TRAIL_MARKS; i++)
                {
                    if (w->trail[i] > decay)
                    {
                        w->trail[i] = (uint8_t)(w->trail[i] - decay);
                    }
                    else
                    {
                        w->trail[i] = 0u;
                    }
                }
            }
        }
    }
#endif /* CGL_CFG_WATCH_TRAIL */

    /* --- the tweens ------------------------------------------------------- */
    if (cgl_anim_tick(&w->reveal, clk->dt_ms))
    {
        watch__touch(&w->base, clk->dt_ms);
    }
    if (cgl_anim_tick(&w->pulse, clk->dt_ms))
    {
        watch__touch(&w->base, clk->dt_ms);
    }

    /* --- the dial: **only what moved** ------------------------------------
     *
     * The dial is 176 x 172, so invalidating it whole marks all 43 bands of the panel
     * dirty -- and a band that is pushed costs the host's background fill plus every
     * primitive that crosses it. Measured on the reference board that was the
     * difference between 13 fps and a smooth face.
     *
     * So this marks damage the way a watch face actually changes:
     *
     *   - the second hand **and the counterweight tail**, at the previous angle (to
     *     erase it) and at the current one (to draw it). One bbox covers a rotating
     *     hand exactly, because the bbox of its two endpoints contains the segment;
     *   - the seconds ring's **leading end**, which is where its cap dot and its newly
     *     covered arc are (the ring's cap sits under the second hand, so the two
     *     usually share bands);
     *   - every chapter mark still carrying comet glow, because the trail *fades* for
     *     seconds after the hand has passed;
     *   - the minute and hour hands, but only on the frames where their integer
     *     endpoints really change (they creep: the minute hand moves a pixel every ten
     *     seconds, and until it does, its pixels are identical);
     *   - and the whole widget while the entrance reveal or the minute pulse is
     *     running, which is a handful of frames per entry and per minute.
     *
     * The damage is deliberately **not** gated by the `CGL_WATCH_F_*` flags: a face
     * that has just turned its second hand off still has to erase the ink the previous
     * frame put down, and an extra band is a repaint, while a missing one is a ghost.
     * -------------------------------------------------------------------- */
    {
        cgl_rect_t dmg = cgl_rect_none();
        bool full = (!primed) || (cgl_anim_running(&w->reveal)) || (cgl_anim_running(&w->pulse));

        if (full)
        {
            watch__touch(&w->base, clk->dt_ms);
        }
        else
        {
            /* The half widths below are the **widest** half width of each hand (at its
               base) plus a pixel of antialiasing; `cgl_arc_needle_damage()` adds the
               shadow's offset on top.
               WARNING: the hour and minute hands are damaged at the radius they are
               drawn to **plus two pixels**. The demo sized them from `CLOCK_R_HOUR` /
               `CLOCK_R_MINUTE` -- the hands' *nominal* lengths, two pixels longer than
               the `... - 2` they are actually drawn to -- and that slack is what the
               original margin was made of. The configuration carries the drawn tip, so
               the two pixels are spelled out here rather than lost. */
            const int16_t minute_dmg_r = (int16_t)(w->cfg.minute.r_tip + 2);
            const int16_t hour_dmg_r = (int16_t)(w->cfg.hour.r_tip + 2);
            const int16_t second_half_w = (int16_t)((w->cfg.second.w_base / 2) + 1);

            dmg = watch__damage_hand(w, dmg, watch__second_angle(w, &w->prev_time),
                                     w->cfg.second.r_tip, w->cfg.geom.r_tail, second_half_w);
            dmg = watch__damage_hand(w, dmg, watch__second_angle(w, &w->now), w->cfg.second.r_tip,
                                     w->cfg.geom.r_tail, second_half_w);
            /* The minute and hour hands **creep**: at 0.1 degrees per second a 62 px
               hand moves one pixel about every ten seconds, and in between its pixels
               are identical. Damaging their two large boxes every frame would add
               their band range to every frame's damage for nothing, so they are only
               marked on the frames where their integer endpoints really changed. */
            {
                int16_t mx;
                int16_t my;
                int16_t hx;
                int16_t hy;

                watch__polar(w, minute_dmg_r, cgl_watch_minute_angle(&w->now), &mx, &my);
                watch__polar(w, hour_dmg_r, cgl_watch_hour_angle(&w->now), &hx, &hy);
                if ((mx != w->prev_minute_pt.x) || (my != w->prev_minute_pt.y) ||
                    (hx != w->prev_hour_pt.x) || (hy != w->prev_hour_pt.y))
                {
                    dmg = watch__damage_hand(w, dmg, cgl_watch_minute_angle(&w->prev_time),
                                             minute_dmg_r, w->cfg.minute.r_base,
                                             (int16_t)((w->cfg.minute.w_base / 2) + 1));
                    dmg = watch__damage_hand(w, dmg, cgl_watch_minute_angle(&w->now), minute_dmg_r,
                                             w->cfg.minute.r_base,
                                             (int16_t)((w->cfg.minute.w_base / 2) + 1));
                    dmg = watch__damage_hand(w, dmg, cgl_watch_hour_angle(&w->prev_time),
                                             hour_dmg_r, w->cfg.hour.r_base,
                                             (int16_t)((w->cfg.hour.w_base / 2) + 1));
                    dmg = watch__damage_hand(w, dmg, cgl_watch_hour_angle(&w->now), hour_dmg_r,
                                             w->cfg.hour.r_base,
                                             (int16_t)((w->cfg.hour.w_base / 2) + 1));
                    w->prev_minute_pt.x = mx;
                    w->prev_minute_pt.y = my;
                    w->prev_hour_pt.x = hx;
                    w->prev_hour_pt.y = hy;
                }
            }
            watch__damage_ring_end(w, &dmg, &w->prev_time);
            watch__damage_ring_end(w, &dmg, &w->now);
#if CGL_CFG_WATCH_TRAIL
            if (watch__trail_live(w))
            {
                watch__damage_trail(w, &dmg);
            }
#endif
            if (clk->dt_ms > 0u)
            {
                cgl_widget_invalidate_rect(dmg);
            }
        }
    }

    w->prev_time = w->now;
    w->prev_second = w->now.s;
    w->prev_minute = w->now.m;
    w->prev_day = w->now.day;
}

void cgl_watch_reveal(cgl_watch_t *w)
{
    if ((w == NULL) || ((w->cfg.flags & CGL_WATCH_F_REVEAL) == 0u))
    {
        /* With `CGL_WATCH_F_REVEAL` clear this does **nothing** -- the dial is simply
           fully up, and the first frame does not start the tween either. That is the
           whole of the flag: there is no half-revealed state to get into. */
        return;
    }

    /* Every call replays the sweep: the face sweeps up from 12:00 to the current time,
       which is the one animation that makes arriving at a watch face feel like waking a
       watch up. The time itself is **not** reset -- it comes from the framework clock,
       so it kept running while the widget was covered. */
    watch__start_reveal(w);
    cgl_widget_invalidate(&w->base);
}

/* -------------------------------------------------------------------------- */
/* The reference configuration                                                */
/* -------------------------------------------------------------------------- */

void cgl_watch_config_default(cgl_watch_config_t *cfg)
{
    if (cfg == NULL)
    {
        return;
    }

    /* --- where everything is ------------------------------------------------
     * The reference panel is 320 x 172, so the dial is as large as it may be:
     * centred at (86, 86) with a face radius of 80 it nearly touches both edges. */
    cfg->geom.cx = CGL_CFG_WATCH_CX;
    cfg->geom.cy = CGL_CFG_WATCH_CY;
    cfg->geom.r_face = CGL_CFG_WATCH_R_FACE;
    cfg->geom.r_ring = CGL_CFG_WATCH_R_RING;
    cfg->geom.ring_w = CGL_CFG_WATCH_RING_W;
    cfg->geom.r_well_in = CGL_CFG_WATCH_R_WELL_IN;
    cfg->geom.r_well_out = CGL_CFG_WATCH_R_WELL_OUT;
    cfg->geom.r_tick_out = CGL_CFG_WATCH_R_TICK_OUT;
    cfg->geom.r_tick_major_in = CGL_CFG_WATCH_R_TICK_HOUR_IN;
    cfg->geom.r_tick_minor_in = CGL_CFG_WATCH_R_TICK_MIN_IN;
    cfg->geom.r_hub = CGL_CFG_WATCH_R_HUB;
    cfg->geom.r_tail = CGL_CFG_WATCH_R_TAIL;

    /* --- the three hands ----------------------------------------------------
     * `r_tip` is where the ink really ends: the hour and minute hands are drawn two
     * pixels short of their nominal radius (`CGL_CFG_WATCH_R_HOUR - 2`), which is what
     * leaves a visible gap inside the chapter ring. The second hand reaches its own
     * radius exactly, and its counterweight is `geom.r_tail` -- the same field the
     * damage model reads, so the two cannot disagree about where the tail is.
     *
     * The colour each hand carries is the palette's own value for it; the dial draws
     * the hands from `style.hand` / `style.accent` (see `watch__draw`), so the two are
     * equal here by construction and a caller that changes one of them owns the
     * difference. */
    cfg->hour.r_base = CGL_CFG_WATCH_R_HOUR_BASE;
    cfg->hour.r_tip = (int16_t)(CGL_CFG_WATCH_R_HOUR - 2);
    cfg->hour.w_base = CGL_CFG_WATCH_HOUR_W_BASE;
    cfg->hour.w_tip = CGL_CFG_WATCH_HOUR_W_TIP;
    cfg->hour.color = CGL_RGB565(240, 244, 250);

    cfg->minute.r_base = CGL_CFG_WATCH_R_MINUTE_BASE;
    cfg->minute.r_tip = (int16_t)(CGL_CFG_WATCH_R_MINUTE - 2);
    cfg->minute.w_base = CGL_CFG_WATCH_MINUTE_W_BASE;
    cfg->minute.w_tip = CGL_CFG_WATCH_MINUTE_W_TIP;
    cfg->minute.color = CGL_RGB565(240, 244, 250);

    cfg->second.r_base = CGL_CFG_WATCH_R_TAIL;
    cfg->second.r_tip = CGL_CFG_WATCH_R_SECOND;
    cfg->second.w_base = CGL_CFG_WATCH_SECOND_W;
    cfg->second.w_tip = CGL_CFG_WATCH_SECOND_W;
    cfg->second.color = CGL_RGB565(255, 106, 0);

    /* --- the counting and the three animations ------------------------------ */
    cfg->anim.tick_count = 60u;      /* one mark per second            */
    cfg->anim.tick_major = 5u;       /* every fifth one is the hours   */
    cfg->anim.tick_w_minor = 2u;
    cfg->anim.tick_w_major = 3u;
    cfg->anim.second_mode = (uint8_t)CGL_CFG_WATCH_SECOND_MODE;
    cfg->anim.tick_ms = CGL_CFG_WATCH_TICK_MS;
    cfg->anim.tick_ease = (uint8_t)CGL_EASE_OUT_BACK; /* overshoots, then settles */
    cfg->anim.reveal_ms = CGL_CFG_WATCH_REVEAL_MS;
    cfg->anim.reveal_ease = (uint8_t)CGL_EASE_OUT_CUBIC;
    cfg->anim.pulse_ms = CGL_CFG_WATCH_PULSE_MS;
    cfg->anim.pulse_ease = (uint8_t)CGL_EASE_OUT_QUAD;
    cfg->anim.trail_ms = CGL_CFG_WATCH_TRAIL_MS;
    cfg->anim.shadow_off = CGL_CFG_WATCH_SHADOW_PX;
    cfg->anim.shadow_alpha = CGL_CFG_WATCH_SHADOW_ALPHA;

    /* --- the palette -------------------------------------------------------- */
    cfg->style.bg = CGL_COLOR_BLACK;
    cfg->style.bezel = CGL_RGB565(56, 62, 74);
    cfg->style.bezel_hi = CGL_RGB565(150, 162, 184); /* the lit half of the bezel */
    cfg->style.bezel_w = 2u;
    cfg->style.bezel_lit_start = CGL_ANGLE_DEG(150); /* below 9 o'clock ...        */
    cfg->style.bezel_lit_sweep = (uint32_t)CGL_ANGLE_DEG(120); /* ... round to 12  */
    cfg->style.tick = CGL_RGB565(96, 104, 122);
    cfg->style.tick_major = CGL_RGB565(214, 221, 232);
    cfg->style.trail = CGL_RGB565(255, 170, 40); /* what a mark blends toward as it lights */
    cfg->style.hand = CGL_RGB565(240, 244, 250);
    cfg->style.hub = CGL_RGB565(240, 244, 250);
    cfg->style.accent = CGL_RGB565(255, 106, 0); /* second hand / ring / lead dot */
    cfg->style.accent_hi = CGL_RGB565(255, 186, 92);

    /* --- the two surfaces ---------------------------------------------------
     * Their geometry is the dial's, so it is stated here rather than left at zero:
     * a surface owns its own centre and `cgl_watch_set_center()` moves all three
     * together. The tables are the module's flash ones when the build bakes them, and
     * NULL (the procedural shade, no flash) when it does not. */
    cgl_arc_surface_default_face(&cfg->style.face);
    cfg->style.face.cx = CGL_CFG_WATCH_CX;
    cfg->style.face.cy = CGL_CFG_WATCH_CY;
    cfg->style.face.r_in = 0;
    cfg->style.face.r_out = CGL_CFG_WATCH_R_FACE;
    cgl_arc_surface_default_ring(&cfg->style.well);
    cfg->style.well.cx = CGL_CFG_WATCH_CX;
    cfg->style.well.cy = CGL_CFG_WATCH_CY;
    cfg->style.well.r_in = CGL_CFG_WATCH_R_WELL_IN;
    cfg->style.well.r_out = CGL_CFG_WATCH_R_WELL_OUT;

#if CGL_CFG_WATCH_DIAL_LUT
    cfg->style.face.lut = cgl_watch_face_lut_default;
    cfg->style.face.lut_n = CGL_ARC_FACE_LUT_N;
    cfg->style.well.lut = cgl_watch_well_lut_default;
    cfg->style.well.lut_n = CGL_ARC_RING_LUT_N;
#else
    cfg->style.face.lut = NULL;
    cfg->style.face.lut_n = 0u;
    cfg->style.well.lut = NULL;
    cfg->style.well.lut_n = 0u;
#endif

    /* --- the crystal --------------------------------------------------------
     * The reflected light is a cool sky white and **near white on purpose**: a
     * reflection only ever *adds* light, and an alpha blend toward a mid grey would
     * therefore do the one thing a reflection cannot -- take light away from the
     * brightest things under it, which on this dial means the hands and the marks
     * going slightly grey exactly where the reflection crosses them. At
     * (232, 240, 252) every surface that is already brighter than the reflection is
     * left alone (the blend is the identity there), while everything darker is lifted,
     * which is what the eye expects. */
    cfg->style.glass.tint = CGL_RGB565(232, 240, 252);
    /* Where the light comes from, as a unit vector in Q8 -- the same direction as the
       specular highlight on the dial, because it is the same light. */
    cfg->style.glass.ux = -166;
    cfg->style.glass.uy = -195;
    /* The sweep: how wide it is (half width, px), where it sits along the light axis
       -- **positive, i.e. on the lit side**, because a reflection of a light up and to
       the left is seen up and to the left -- and how opaque it gets (Q8, ~16 %). */
    cfg->style.glass.sweep_w = 52;
    cfg->style.glass.sweep_at = 34;
    cfg->style.glass.sweep_alpha = 40u;
    /* The rim crescent: the arc it follows, how far along the light axis it starts,
       how fast it comes up, and its peak. */
    cfg->style.glass.edge_in = 66;
    cfg->style.glass.edge_out = 76;
    cfg->style.glass.edge_at = 26;
    cfg->style.glass.edge_ramp = 54;
    cfg->style.glass.edge_alpha = 74u;
    /* ... and its echo, opposite: smaller, further in, and much fainter, because it is
       the light that came off the *inside* of the dome. */
    cfg->style.glass.echo_in = 30;
    cfg->style.glass.echo_out = 48;
    cfg->style.glass.echo_at = 18;
    cfg->style.glass.echo_ramp = 30;
    cfg->style.glass.echo_alpha = 18u;

    /* --- the anchor reading, and what is drawn ------------------------------
     * 10:09, the time every watch is photographed at, on 2026-01-14 (a Wednesday), and
     * everything the reference face has except the two things that need a caller: a
     * brand string and 60 bytes of trail RAM. */
    cfg->start.h = 10u;
    cfg->start.m = 9u;
    cfg->start.s = 0u;
    cfg->start.ms = 0u;
    cfg->start.year = 2026u;
    cfg->start.month = 1u;
    cfg->start.day = 14u;
    cfg->start.wday = 3u; /* 0 = Sunday; 2026-01-14 is a Wednesday */

    cfg->flags = (uint16_t)CGL_WATCH_F_DEFAULT;

    cfg->brand = NULL;
    cfg->brand_font = NULL;
    cfg->brand_dx = -20; /* low in the dial, like a printed logo */
    cfg->brand_dy = 40;
    cfg->brand_color = CGL_RGB565(64, 71, 86);
}

void cgl_watch_config_set_start(cgl_watch_config_t *cfg, uint8_t h, uint8_t m, uint8_t s)
{
    uint32_t total;

    if (cfg == NULL)
    {
        return;
    }

    /* Reduce modulo the next unit up, so a caller cannot hand the dial a state it
       cannot draw: h = 30, m = 75, s = 90 are all legal inputs here. The date is left
       exactly as it was -- this is the time of day, and a date is a separate call. */
    total = ((uint32_t)(h % 24u) * 3600u) + ((uint32_t)(m % 60u) * 60u) + ((uint32_t)(s % 60u));

    cfg->start.h = (uint8_t)(total / 3600u);
    cfg->start.m = (uint8_t)((total % 3600u) / 60u);
    cfg->start.s = (uint8_t)(total % 60u);
    cfg->start.ms = 0u;
}

void cgl_watch_config_set_start_date(cgl_watch_config_t *cfg, uint16_t year, uint8_t month,
                                     uint8_t day, uint8_t wday)
{
    uint8_t m;
    uint8_t dim;

    if (cfg == NULL)
    {
        return;
    }

    /* Clamp rather than wrap: a month of 13 or a day of 0 is a caller bug, and silently
       rendering "0 FEB" would hide it. (The *time* of day wraps, because h = 25 meaning
       1 am is a real thing a caller does by accident and the result is still
       unambiguous; a date has no such convention.) */
    m = (uint8_t)((month < 1u) ? 1u : ((month > 12u) ? 12u : month));
    dim = cgl_watch_days_in_month(year, m);

    cfg->start.year = year;
    cfg->start.month = m;
    cfg->start.day = (uint8_t)((day < 1u) ? 1u : ((day > dim) ? dim : day));
    cfg->start.wday = (uint8_t)(wday % 7u);
}

/* -------------------------------------------------------------------------- */
/* The RTC hook                                                               */
/* -------------------------------------------------------------------------- */

void cgl_watch_set_time(cgl_watch_t *w, uint8_t h, uint8_t m, uint8_t s)
{
    uint32_t total;

    if (w == NULL)
    {
        return;
    }

    total = ((uint32_t)(h % 24u) * 3600u) + ((uint32_t)(m % 60u) * 60u) + ((uint32_t)(s % 60u));

    w->set_to.h = (uint8_t)(total / 3600u);
    w->set_to.m = (uint8_t)((total % 3600u) / 60u);
    w->set_to.s = (uint8_t)(total % 60u);
    w->set_to.ms = 0u;
    /* The date is untouched: `set_time` moves the time of day, and a caller that wants
       both calls both, in either order (see `cgl_watch_set_date`). */
    w->set_to.year = w->now.year;
    w->set_to.month = w->now.month;
    w->set_to.day = w->now.day;
    w->set_to.wday = w->now.wday;
    w->set_pending = true;

    cgl_widget_invalidate(&w->base);
}

void cgl_watch_set_date(cgl_watch_t *w, uint16_t year, uint8_t month, uint8_t day, uint8_t wday)
{
    uint8_t m;

    if (w == NULL)
    {
        return;
    }

    m = (uint8_t)((month < 1u) ? 1u : ((month > 12u) ? 12u : month));

    /* Keep the time of day a pending set_time already asked for, if there is one. */
    if (!w->set_pending)
    {
        w->set_to = w->now;
        w->set_pending = true;
    }
    w->set_to.year = year;
    w->set_to.month = m;
    w->set_to.day = (uint8_t)((day < 1u)
                                  ? 1u
                                  : ((day > cgl_watch_days_in_month(year, m))
                                         ? cgl_watch_days_in_month(year, m)
                                         : day));
    w->set_to.wday = (uint8_t)(wday % 7u);

    cgl_widget_invalidate(&w->base);
}

/* -------------------------------------------------------------------------- */
/* Readouts                                                                   */
/* -------------------------------------------------------------------------- */

const cgl_watch_time_t *cgl_watch_now(const cgl_watch_t *w)
{
    return (w != NULL) ? &w->now : NULL;
}

uint32_t cgl_watch_seconds_of_day_now(const cgl_watch_t *w)
{
    return (w != NULL) ? cgl_watch_seconds_of_day(&w->now) : 0u;
}

uint16_t cgl_watch_reveal_value(const cgl_watch_t *w)
{
    return (w != NULL) ? (uint16_t)cgl_anim_value(&w->reveal) : 0u;
}

uint16_t cgl_watch_pulse_value(const cgl_watch_t *w)
{
    return (w != NULL) ? (uint16_t)cgl_anim_value(&w->pulse) : 0u;
}

uint32_t cgl_watch_frames(const cgl_watch_t *w)
{
    return (w != NULL) ? w->frames : 0u;
}

/* -------------------------------------------------------------------------- */
/* The pure doors: the surfaces and the crystal                               */
/* -------------------------------------------------------------------------- */

/*
 * These three answer for any offset, on the dial or not -- the caller decides whether
 * the pixel is on it -- and they are **pure**: same offset, same colour, for ever, with
 * no state read and none written. That is what makes a test able to assert the *design*
 * ("the centre is brighter than the rim", "the upper left is brighter than the lower
 * right", "the ring's top is darker than its bottom", "the hand is still orange, and it
 * is orange *through the glass*") instead of photographing it.
 *
 * WARNING: they must not touch `w->now`, the tweens or anything else the frame loop
 * writes. A caller may hold a **static configuration** and ask these questions before
 * any widget exists (the demo page hands exactly these functions to its test suite),
 * which is only possible while the answer depends on the configuration alone.
 */

cgl_color_t cgl_watch_face_color(const cgl_watch_t *w, int16_t dx, int16_t dy)
{
    /* The table it reads is in flash, built at compile time: there is nothing to set up
       and nothing that can be "not ready yet", so this is answerable from the moment the
       widget exists -- before its first frame, before its first draw. */
    return (w != NULL) ? cgl_arc_surface_color(&w->cfg.style.face, dx, dy) : CGL_COLOR_BLACK;
}

cgl_color_t cgl_watch_ring_color(const cgl_watch_t *w, int16_t dx, int16_t dy)
{
    return (w != NULL) ? cgl_arc_surface_color(&w->cfg.style.well, dx, dy) : CGL_COLOR_BLACK;
}

cgl_color_t cgl_watch_glass_shade(const cgl_watch_glass_t *gl, cgl_color_t under, int16_t dx,
                                  int16_t dy)
{
#if CGL_CFG_WATCH_GLASS
    watch__glass_k_t k;
    uint16_t cov;

    if (gl == NULL)
    {
        return under;
    }

    /* No dial radius is available here -- a `cgl_watch_glass_t` carries the light and
       nothing else -- so this answers for every offset. The band pass, which *does*
       know the dial, bounds itself with the same `r_face` by construction (its row span
       is `sqrt(r_face^2 - dy^2)`), so the pixels it draws are the same either way. */
    watch__glass_prepare(gl, 0, &k);
    cov = watch__glass_cov_q8(gl, &k, (int32_t)dx, (int32_t)dy);

    return (cov == 0u) ? under : cgl_color_blend_q8(under, gl->tint, cov);
#else
    /* `CGL_CFG_WATCH_GLASS == 0`: there is no cover in this build, so a reflection is
       the identity. The parameters are still accepted, exactly like the runtime flag,
       so a configuration struct stays portable between builds. */
    (void)gl;
    (void)dx;
    (void)dy;
    return under;
#endif
}

cgl_color_t cgl_watch_glass_over(const cgl_watch_t *w, cgl_color_t under, int16_t dx, int16_t dy)
{
    return (w != NULL) ? cgl_watch_glass_shade(&w->cfg.style.glass, under, dx, dy) : under;
}

/* -------------------------------------------------------------------------- */
/* Runtime setters                                                            */
/* -------------------------------------------------------------------------- */

void cgl_watch_set_second_mode(cgl_watch_t *w, cgl_watch_second_mode_t mode)
{
    if ((w == NULL) || (w->cfg.anim.second_mode == (uint8_t)mode))
    {
        return; /* a mode that is already set costs one comparison */
    }
    w->cfg.anim.second_mode = (uint8_t)mode;
    /* The hand jumps: the two motions agree at every whole second and nowhere else. */
    cgl_widget_invalidate(&w->base);
}

void cgl_watch_set_tick(cgl_watch_t *w, uint16_t tick_ms, cgl_ease_t ease)
{
    if ((w == NULL) || ((w->cfg.anim.tick_ms == tick_ms) && (w->cfg.anim.tick_ease == (uint8_t)ease)))
    {
        return;
    }
    w->cfg.anim.tick_ms = tick_ms;
    w->cfg.anim.tick_ease = (uint8_t)ease;
    cgl_widget_invalidate(&w->base);
}

void cgl_watch_set_flags(cgl_watch_t *w, uint16_t flags)
{
    if ((w == NULL) || (w->cfg.flags == flags))
    {
        return;
    }
    w->cfg.flags = flags;
    cgl_widget_invalidate(&w->base);
}

void cgl_watch_set_feature(cgl_watch_t *w, uint16_t flag, bool on)
{
    uint16_t flags;

    if (w == NULL)
    {
        return;
    }

    flags = on ? (uint16_t)(w->cfg.flags | flag) : (uint16_t)(w->cfg.flags & (uint16_t)~flag);
    if (flags == w->cfg.flags)
    {
        return;
    }
    w->cfg.flags = flags;
    cgl_widget_invalidate(&w->base);
}

bool cgl_watch_has_flags(const cgl_watch_t *w, uint16_t flags)
{
    return (w != NULL) && ((w->cfg.flags & flags) == flags);
}

void cgl_watch_set_center(cgl_watch_t *w, int16_t cx, int16_t cy)
{
    int16_t dx;
    int16_t dy;
    cgl_point_t delta;

    if (w == NULL)
    {
        return;
    }

    dx = (int16_t)(cx - w->cfg.geom.cx);
    dy = (int16_t)(cy - w->cfg.geom.cy);
    if ((dx == 0) && (dy == 0))
    {
        return;
    }

    w->cfg.geom.cx = cx;
    w->cfg.geom.cy = cy;
    /* The two surfaces are data and own their own centre (see the WARNING on
       `cgl_arc_surface_t::cx`), but the dial is one thing: the module moves all three
       so that "move the dial" cannot leave the face behind. */
    w->cfg.style.face.cx = (int16_t)(w->cfg.style.face.cx + dx);
    w->cfg.style.face.cy = (int16_t)(w->cfg.style.face.cy + dy);
    w->cfg.style.well.cx = (int16_t)(w->cfg.style.well.cx + dx);
    w->cfg.style.well.cy = (int16_t)(w->cfg.style.well.cy + dy);

    /* ... and the widget frame by the same delta, which is what the header promises:
       the geometry is in screen coordinates and the frame is not consulted for it, so
       anything that moves the frame alone would draw the dial somewhere else.
       `cgl_widget_set_frame()` marks both the old and the new rectangle damaged. */
    delta.x = dx;
    delta.y = dy;
    cgl_widget_set_frame(&w->base, cgl_rect_translate(w->base.frame, delta));
}

void cgl_watch_set_config(cgl_watch_t *w, const cgl_watch_config_t *cfg)
{
    if ((w == NULL) || (cfg == NULL))
    {
        return;
    }

    w->cfg = *cfg;

    /* Re-anchor at the new start reading -- and park it, exactly as
       `cgl_watch_set_time()` does, because the anchor has to be a `clk->now_ms` and
       that value only exists inside `cgl_watch_frame()`. Anchoring `t0` here would
       leave it paired with the *previous* anchor's `t0_ms`.
       WARNING: the two tweens are deliberately **left alone**: a running reveal is
       "the face is half way up", and re-initialising it would collapse the dial to
       12:00 until something started it again -- a palette change would blink the face
       away. Only the anchor and the flags move here. */
    w->set_to = w->cfg.start;
    w->set_pending = true;

    cgl_widget_invalidate(&w->base);
}

const cgl_watch_config_t *cgl_watch_get_config(const cgl_watch_t *w)
{
    return (w != NULL) ? &w->cfg : NULL;
}

#endif /* CGL_CFG_WATCH */
