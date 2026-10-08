// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_watch.h
  * @brief   LCD UI framework - an analog watch face: one widget, a time base, and
  *          a configuration struct that is the whole dial.
  *
  *  What this is
  *  ------------
  *  A complete analog face on a small landscape panel: a shaded disc for the dial,
  *  a recessed ring around it, a 60 mark chapter ring, a seconds indicator that
  *  hugs the bezel, three tapered hands and a hub -- plus, if you want them, a
  *  comet trail behind the second hand and a crystal cover whose reflection runs
  *  *across* the hands rather than stopping at their edges.
  *
  *      cgl_watch_config_t cfg;
  *      cgl_watch_t face;
  *
  *      cgl_watch_config_default(&cfg);       -- what a watch looks like
  *      cgl_watch_config_set_start(&cfg, 10, 9, 0);   -- 10:09, the hour of every
  *                                                       watch photograph
  *      cgl_watch_init(&face, "watch", cgl_rect_wh(0, 0, 176, 172), &cfg);
  *      cgl_widget_add_child(page_root, &face.base);
  *
  *      ... and once per frame, from the page's on_frame:
  *      cgl_watch_frame(&face, clk);
  *
  *  That is the whole contract. Everything else -- a different dial radius, a
  *  different palette, a sweep instead of a tick, no crystal, a 12 mark face, a
  *  hand that is 40 px long -- is a field of `cgl_watch_config_t`, and *every*
  *  field is reachable at run time from a setter, so a settings screen can offer
  *  all of it without a rebuild.
  *
  *  Why it is not called `cgl_clock`
  *  --------------------------------
  *  Because `cgl_clock_t` already exists: it is the framework's one frame clock
  *  (`cgl_time.h`), the thing that owns "what time is it" for the whole
  *  application. A framework with two different things called a clock is exactly
  *  the confusion `cgl_time.h` spends its header eliminating -- so the *face* is a
  *  `cgl_watch_t`, and the time it displays comes from the frame clock it is
  *  handed.
  *
  *  The time source, honestly stated
  *  --------------------------------
  *  **This framework does not read an RTC**, because it cannot: it depends on no
  *  HAL. A watch face therefore runs off the frame clock it is given:
  *  `t0` is the wall time at the anchor instant, `t0_ms` is `clk->now_ms` at that
  *  instant, and the displayed time is
  *
  *      now = t0 + (clk->now_ms - t0_ms)
  *
  *  which is exact, monotonic, drift-free, and keeps running **while the widget is
  *  not being drawn** -- the framework clock is global and is not part of the
  *  widget's state. Wiring a real time source is one call,
  *  `cgl_watch_set_time()`, applied at the top of the next `cgl_watch_frame()` so
  *  the anchor instant is exact rather than "as close as the low byte of a
  *  millisecond counter allowed".
  *
  *  WARNING: the displayed **date** is anchored at the configured start date and
  *  counted forward from there at one day per 86400 s, so it is a real calendar
  *  (month lengths and leap years included) -- just not one that knows what today
  *  is. `cgl_watch_set_date()` is the other half of the RTC hook.
  *
  *  The three design ideas worth knowing before reading the code
  *  -----------------------------------------------------------
  *  **1. The dial is a pure function of the clock; only the *events* are tweened.**
  *  A second hand does not need a state machine: its angle is
  *  `(s*1000 + ms) / 60000` of a turn or a stepped curve over `ms`, evaluated fresh
  *  every frame from the time. Nothing accumulates, so nothing can drift, and a
  *  dropped frame costs smoothness and nothing else. What *is* tweened (with
  *  `cgl_anim`) is the three things that are events rather than motion: the
  *  entrance sweep, the flash when the minute turns over, and (by the owner) a
  *  digit roll.
  *
  *  **2. Every element is "a point at an angle on a radius".** Not one part of the
  *  face is a special case: the ring is `cgl_canvas_arc()`, a hand is a
  *  `cgl_arc_stroke_t`, a tick is a two-point polar pair, and the dial and its
  *  well are two `cgl_arc_surface_t`s.
  *
  *  **3. Damage is the only thing that costs.** The dial is 166 rows of a 172 row
  *  panel, so invalidating it whole marks all 43 bands dirty -- measured on the
  *  reference board that was the difference between 13 fps and a smooth face. So
  *  `cgl_watch_frame()` marks damage the way a watch face actually changes: the
  *  second hand and its counterweight at the previous angle *and* the current one,
  *  the seconds ring's leading end, every chapter mark still carrying comet glow,
  *  the hour and minute hands **only on the frames where their integer endpoints
  *  really move** (they creep -- one pixel every ten seconds -- so between those
  *  frames their pixels are identical), and the whole dial only while the entrance
  *  or the pulse is running.
  *
  *  A missed region would leave a stale ghost on the panel, so the margins are
  *  generous and `tests/watch_host_test.c` has a case that forces a full redraw and
  *  requires the incrementally damaged frame to be **identical** to it.
  ******************************************************************************
  */
#ifndef CGL_WATCH_H
#define CGL_WATCH_H

#include <cytgl/cytgl_config.h>

#if CGL_CFG_WATCH

#include <cytgl/cgl_types.h>
#include <cytgl/cgl_widget.h>
#include <cytgl/cgl_anim.h>
#include <cytgl/cgl_arc.h>
#include <cytgl/cgl_font.h>
#include <cytgl/cgl_time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* What a watch shows                                                         */
/* -------------------------------------------------------------------------- */

/**
 * A wall clock reading, plus the calendar fields a face shows.
 *
 * `wday` is 0 = Sunday..6 = Saturday: the classic `struct tm` convention, so a real
 * RTC hookup has one less conversion to get wrong.
 *
 * WARNING: this is the *same* struct the demo page has always called `clock_time_t`
 * (that name is a typedef of this one in app_page_clock.h). It moved here because a
 * page that shows a time and a framework that draws one must agree about what a
 * time is -- and because a caller with an RTC should be able to fill in one struct
 * and hand it over.
 */
typedef struct
{
    uint8_t h;
    uint8_t m;
    uint8_t s;
    uint16_t ms;  /**< milliseconds into the current second, 0..999 */
    uint16_t year;
    uint8_t month; /**< 1..12  */
    uint8_t day;   /**< 1..31  */
    uint8_t wday;  /**< 0 = Sunday */
} cgl_watch_time_t;

/** How the second hand travels between two marks. Both are pure functions of the
    time, so the choice changes the motion and nothing else. */
typedef enum
{
    /** The classic quartz glide: `(s*1000 + ms) / 60 s` of a turn. The hand is a
        continuous function of the time, so it is automatically as smooth as the
        frame rate allows. */
    CGL_WATCH_SECOND_SWEEP = 0,
    /** A mechanical step: parked on the current second's mark, then, over the last
        `tick_ms` of the second, whipped to the next one through
        `cgl_watch_second_yank_ms()` -- overshooting and settling back. */
    CGL_WATCH_SECOND_TICK = 1
} cgl_watch_second_mode_t;

/** Marks in the comet trail (one per minute mark of a 60 mark face). */
#define CGL_WATCH_TRAIL_MARKS 60

/* --- what the face draws ---------------------------------------------------
 * Every one of these is a run-time flag: a face with `CGL_WATCH_F_GLASS` off draws
 * no crystal, and nothing in the binary changes. They exist so that "the same
 * widget, four looks" is four configurations rather than four builds. */
#define CGL_WATCH_F_BEZEL 0x0001u   /**< the two pixel case lip around the dial  */
#define CGL_WATCH_F_TICKS 0x0002u   /**< the chapter ring                        */
#define CGL_WATCH_F_RING 0x0004u    /**< the seconds indicator and its lead dot  */
#define CGL_WATCH_F_TRAIL 0x0008u   /**< the comet glow behind the second hand   */
#define CGL_WATCH_F_HUB 0x0010u     /**< the hub cap over the three arbors       */
#define CGL_WATCH_F_GLASS 0x0020u   /**< the crystal cover over everything       */
#define CGL_WATCH_F_BRAND 0x0040u   /**< the printed brand line                  */
#define CGL_WATCH_F_HOUR 0x0080u    /**< the hour hand                           */
#define CGL_WATCH_F_MINUTE 0x0100u  /**< the minute hand                         */
#define CGL_WATCH_F_SECOND 0x0200u  /**< the second hand and its counterweight   */
#define CGL_WATCH_F_PULSE 0x0400u   /**< the flash when the minute turns over    */
#define CGL_WATCH_F_REVEAL 0x0800u  /**< the entrance sweep from 12:00 to now    */

/** Everything `cgl_watch_config_default()` turns on: a plain working watch, with
    no brand line (that needs a string) and no comet trail (that needs RAM). */
#define CGL_WATCH_F_DEFAULT                                                                       \
    (CGL_WATCH_F_BEZEL | CGL_WATCH_F_TICKS | CGL_WATCH_F_RING | CGL_WATCH_F_HUB |                  \
     CGL_WATCH_F_GLASS | CGL_WATCH_F_HOUR | CGL_WATCH_F_MINUTE | CGL_WATCH_F_SECOND |              \
     CGL_WATCH_F_PULSE | CGL_WATCH_F_REVEAL)

/* -------------------------------------------------------------------------- */
/* Calendar helpers (public: a real time source needs the same arithmetic)     */
/* -------------------------------------------------------------------------- */

/** Is `year` a leap year? (`tests/watch_host_test.c` checks the 100/400 rules.) */
bool cgl_watch_is_leap(uint16_t year);

/** Days in `month` (1..12) of `year`. An out-of-range month is reduced modulo 12,
    so the answer is always a real month length. */
uint8_t cgl_watch_days_in_month(uint16_t year, uint8_t month);

/**
 * Advance a date by one day, month and year rollovers included.
 *
 * This is why a face can show a date at all without an RTC: the date is *counted*
 * forward from the anchor at one day per 86400 s, and this is only reached when the
 * count really crosses midnight -- once a day, not once a frame.
 */
void cgl_watch_next_day(cgl_watch_time_t *t);

/** Seconds since midnight (cheap: derived state, for a HUD or a test). */
uint32_t cgl_watch_seconds_of_day(const cgl_watch_time_t *t);

/* -------------------------------------------------------------------------- */
/* The second hand's motion -- in the header, and why it has to be            */
/* -------------------------------------------------------------------------- */

/*
 * A build switch read in one translation unit is a trap, and this framework has
 * already been bitten by it: the first version of the demo page kept the tick in
 * the .c, a test compiled the page with `-DCLOCK_SECOND_MODE=...` and measured a
 * perfectly smooth sweep, and no compiler warning said so.
 *
 * The answer here is stronger than "put it in the header": the motion is a
 * `CGL_AINLINE` **pure function of the time and the mode**, so the dial, a test and
 * a settings screen all evaluate the same body, and the mode is an argument rather
 * than a macro. Inlining is what makes handing it over cost nothing -- it compiles
 * to exactly the instructions the .c would have had.
 */

/** `num / den` of a turn, as an angle. WARNING: the caller must guarantee
    `num <= 65535`, which is what keeps `num * CGL_ANGLE_TURN` inside a `uint32_t`
    (65535 * 65536 = 4.29e9, just inside it). A 64 bit product would drag
    `__aeabi_uldivmod` into the firmware for precision no screen can show. */
CGL_AINLINE cgl_angle_t cgl_watch_fraction_angle(uint32_t num, uint32_t den)
{
    return (cgl_angle_t)((num * CGL_ANGLE_TURN) / den);
}

/** The angle of a face reading: 12 o'clock (`CGL_ANGLE_UP`) plus a clockwise part.
    This is the whole of "the clock convention" in one line -- see cgl_math.h for
    why the framework's own convention is 0 = 3 o'clock. */
CGL_AINLINE cgl_angle_t cgl_watch_face_angle(uint32_t num, uint32_t den)
{
    return (cgl_angle_t)(CGL_ANGLE_UP + cgl_watch_fraction_angle(num, den));
}

/**
 * How far the second hand has been yanked past the last second boundary, in
 * **milliseconds of dial**: 0 for all of `1000 - tick_ms` (the hold), reaching
 * `tick_ms` -- a whole second of dial -- on the last frame of the step, and a few
 * percent over that at the curve's overshoot.
 *
 * The shape, and the three ways it was wrong first
 * ------------------------------------------------
 * Through the step the hand's **linear** share of the move is `offset`
 * milliseconds of dial, and the ease curve adds a **bump** on top of it that is
 * zero at both ends of the step and positive in the middle:
 *
 *      bump(offset) = 3 * offset * (eased - 256) / 2      in 128ths of a millisecond
 *      yank(offset) = offset + bump(offset) / 128
 *
 * WARNING: writing the bump as a deviation from linear -- that `- 256` -- is the
 * whole trick, and each of the three simpler forms was tried and is wrong in a way
 * a still frame does not show:
 *
 *   * mapping the curve onto the *hold* instead of the step runs the step nine
 *     times too slow and the hand creeps a sixth of the way to the next mark;
 *   * using the curve's own value *as* the step travels a fortieth of it and stops
 *     a whole second of dial short (a curve is ~1.0 at the end of its step -- that
 *     is what an ease curve is);
 *   * the right shape without the `- 256` lands the hand 0.4 % *past* the mark at
 *     every second boundary.
 *
 * WARNING: which end of the second the whip occupies *is* the character of the
 * motion, and the other way round is not the same animation. Running the curve
 * forwards from `ms = 0` (snap at the top of the second, then rest) gives a hand
 * that jumps the moment the second changes and then sits still -- the arriving and
 * the leaving are the same instant, so there is nothing to watch.
 *
 * The factor 3/2 sets the overshoot: `CGL_EASE_OUT_BACK` peaks ~4.3 % above linear
 * at its middle, so `bump` peaks at ~6.5 % of the offset there, i.e. the hand runs
 * ~3 % past the mark mid-step and comes back onto it. Both ends are exact by
 * construction, which is what makes this a clock and not an animation.
 *
 * WARNING: the largest product here is `3 * offset * eased` = `3 * 1000 * ~283`,
 * inside a `uint32_t` with three orders of magnitude to spare, and the only
 * divisions are by 256 and 128: no 64 bit product in the firmware.
 */
CGL_AINLINE uint32_t cgl_watch_second_yank_ms(uint16_t ms, uint16_t tick_ms, uint8_t tick_ease)
{
    const uint32_t sec = (uint32_t)CGL_CLOCK_MS_PER_S;
    const uint32_t step = (uint32_t)tick_ms;
    uint32_t m = (uint32_t)ms % sec;
    uint32_t hold;
    uint32_t offset;
    uint32_t p;
    uint32_t eased;
    uint32_t bump;

    if ((step == 0u) || (step >= sec))
    {
        return 0u; /* a degenerate step is no step at all: the hand holds all second */
    }
    hold = sec - step;

    if (m < hold)
    {
        return 0u; /* parked on this second's mark: this is most of the second */
    }

    offset = m - hold; /* 0 .. tick_ms - 1 */

    /* Progress through the step, in Q8, rounded to nearest so the last frame lands
       on 255 and not on 254. */
    p = (((offset * CGL_ANIM_ONE_Q8) + (step / 2u)) / step);
    if (p > 255u)
    {
        p = 255u;
    }

    eased = (uint32_t)cgl_anim_ease((cgl_ease_t)tick_ease, (uint16_t)p);

    /* The ease curve is >= 256 for the second half of its step (it overshoots), so
       this difference is 0 or positive and is exactly 0 at both ends. */
    bump = (256u < eased) ? (eased - 256u) : 0u;

    return offset + (((3u * offset * bump) + 64u) >> 7);
}

/**
 * The second hand: **the one hand whose motion is a choice**, and here the choice
 * is an argument rather than a build switch.
 *
 * Both modes are pure functions of the time and both agree at every whole second,
 * so the face is a real clock either way -- the mode changes *how* the hand travels
 * between the marks and nothing else.
 *
 * WARNING: the base term in TICK mode is `t->s * 1000`, **not** `t->s * 1000 +
 * t->ms`. That looks like a typo and it is not: the hand's whole position between
 * two marks is the `yank` (0 all through the hold, a full second of dial by the next
 * mark), so adding `t->ms` as well would leave the hand creeping round at 1.09
 * degrees per second *during* the hold -- the tick would read as "sweeps slowly,
 * then jerks", which is exactly the failure this mode exists to rule out and which
 * took a frame-by-frame trace of the rendered panel to find.
 *
 * WARNING: the *fixup* is not decoration either. The yank is added to the
 * millisecond count first and the fraction is taken afterwards, so the overshoot
 * lands on the far side of the *mark* and the settle comes back to it -- add the
 * yank to the finished angle instead and the hand would overshoot past 60 s and
 * snap back to :00 from the wrong side, i.e. anticlockwise, once a minute.
 */
CGL_AINLINE cgl_angle_t cgl_watch_second_angle(const cgl_watch_time_t *t, uint8_t mode,
                                               uint16_t tick_ms, uint8_t tick_ease)
{
    if (mode == (uint8_t)CGL_WATCH_SECOND_TICK)
    {
        /* Parked on this second's mark, plus however far the step has carried it. */
        return cgl_watch_face_angle(((uint32_t)t->s * CGL_CLOCK_MS_PER_S) +
                                        cgl_watch_second_yank_ms(t->ms, tick_ms, tick_ease),
                                    60000u);
    }
    return cgl_watch_face_angle(((uint32_t)t->s * CGL_CLOCK_MS_PER_S) + (uint32_t)t->ms, 60000u);
}

/** The hour hand, **continuous**: it creeps a fraction of a degree per second, the
    way a real hour hand does, rather than snapping at the hour. */
CGL_AINLINE cgl_angle_t cgl_watch_hour_angle(const cgl_watch_time_t *t)
{
    return cgl_watch_face_angle(((uint32_t)(t->h % 12u) * 3600u) + ((uint32_t)t->m * 60u) +
                                    (uint32_t)t->s,
                                43200u);
}

/** The minute hand, continuous for the same reason. */
CGL_AINLINE cgl_angle_t cgl_watch_minute_angle(const cgl_watch_time_t *t)
{
    return cgl_watch_face_angle(((uint32_t)t->m * 60u) + (uint32_t)t->s, 3600u);
}

/**
 * How much of the minute has run, in angle units -- the seconds indicator's sweep.
 *
 * WARNING: 65536 / 60 = 1092.27, and 1092 units per second is 0.02 % short: 0.08
 * degrees of ring at the worst case, well under a pixel at r = 75. The alternative
 * (multiplying the second count by 65536 first) reaches 59999 * 65536 = 3.9e9 and
 * would sit right at the uint32 edge for no visible gain.
 */
CGL_AINLINE uint32_t cgl_watch_minute_sweep(const cgl_watch_time_t *t)
{
    return ((uint32_t)t->s * 1092u) + (((uint32_t)t->ms * 1092u) / CGL_CLOCK_MS_PER_S);
}

/* -------------------------------------------------------------------------- */
/* The dial, as data                                                          */
/* -------------------------------------------------------------------------- */

/** One hand: how far it points, how far back it reaches, and how wide it is.
    A watch hand is a tapered blade -- wide where it is pivoted, a sliver where it
    points -- and the wide end is hidden under the hub. */
typedef struct
{
    int16_t r_base;  /**< how far the blade reaches back **behind** the centre */
    int16_t r_tip;   /**< how far it points                                    */
    uint8_t w_base;  /**< full width in pixels at the arbor                    */
    uint8_t w_tip;   /**< full width in pixels at the tip                      */
    cgl_color_t color;
} cgl_watch_hand_t;

/** Where everything on the face is. All of it is a designer's knob; the defaults
    are a dial that nearly touches a 320 x 172 panel's edges. */
typedef struct
{
    int16_t cx;   /**< dial centre, **screen** coordinates                   */
    int16_t cy;
    int16_t r_face;      /**< the dial's radius                              */
    int16_t r_ring;      /**< the seconds indicator's radius                 */
    int16_t ring_w;      /**< its stroke width                               */
    int16_t r_well_in;   /**< the recessed well's cross-section              */
    int16_t r_well_out;
    int16_t r_tick_out;      /**< where a chapter mark ends                  */
    int16_t r_tick_major_in; /**< where a major (hour) mark starts           */
    int16_t r_tick_minor_in; /**< where a minor (minute) mark starts         */
    int16_t r_hub;           /**< the hub cap's radius                       */
    int16_t r_tail;          /**< how far the second hand's counterweight
                                  reaches back                                 */
} cgl_watch_geom_t;

/** The crystal cover, as parameters: a wide soft sweep of reflected light, a bright
    crescent where the glass curves at the rim, and a faint echo of it opposite.
    See `cgl_watch_glass_over()` for what they do to a pixel. */
typedef struct
{
    cgl_color_t tint;   /**< the reflected light (near white: a reflection only
                             ever *adds* light)                                 */
    int16_t ux;         /**< where the light comes from, a unit vector in Q8    */
    int16_t uy;
    int16_t sweep_w;    /**< the sweep's half width, px                         */
    int16_t sweep_at;   /**< where it sits along the light axis, px             */
    uint8_t sweep_alpha;/**< its peak opacity, Q8                               */
    int16_t edge_in;    /**< the rim crescent's annulus                         */
    int16_t edge_out;
    int16_t edge_at;    /**< how far along the light axis it starts             */
    int16_t edge_ramp;  /**< how fast it comes up                               */
    uint8_t edge_alpha; /**< its peak opacity, Q8                               */
    int16_t echo_in;    /**< ... and its echo, opposite                         */
    int16_t echo_out;
    int16_t echo_at;
    int16_t echo_ramp;
    uint8_t echo_alpha;
} cgl_watch_glass_t;

/** The palette and the two surfaces. */
typedef struct
{
    cgl_color_t bg;       /**< what a hand's shadow blends toward, and the flat
                               colour a deformed (warped) face falls back to     */
    cgl_color_t bezel;    /**< the case lip                                       */
    cgl_color_t bezel_hi; /**< its lit half                                       */
    uint8_t bezel_w;      /**< the lip's stroke width                             */
    cgl_angle_t bezel_lit_start; /**< where the lit half begins                   */
    uint32_t bezel_lit_sweep;    /**< and how far it runs                         */

    cgl_color_t tick;       /**< a minor chapter mark                           */
    cgl_color_t tick_major; /**< an hour mark                                   */
    cgl_color_t trail;      /**< what a mark blends toward as the comet lights it */
    cgl_color_t hand;       /**< the hour and minute hands                      */
    cgl_color_t hub;        /**< the hub cap                                    */
    cgl_color_t accent;     /**< the second hand, the seconds ring, the lead dot */
    cgl_color_t accent_hi;  /**< what the minute pulse blends the hands toward  */

    cgl_arc_surface_t face; /**< the dial                                        */
    cgl_arc_surface_t well; /**< the ring it is set into                         */
    cgl_watch_glass_t glass;
} cgl_watch_style_t;

/** The chapter ring's counting, and the three animations. */
typedef struct
{
    uint8_t tick_count;   /**< marks around the face (60 = one per second)      */
    uint8_t tick_major;   /**< every n-th mark is a major one (5 = the hours)   */
    uint8_t tick_w_minor; /**< stroke width of a minor mark                     */
    uint8_t tick_w_major; /**< stroke width of a major mark                     */

    uint8_t second_mode; /**< cgl_watch_second_mode_t                           */
    uint16_t tick_ms;    /**< how long the tick's snap takes; see the mode      */
    uint8_t tick_ease;   /**< cgl_ease_t for the tick (OUT_BACK overshoots)     */

    uint16_t reveal_ms;  /**< the entrance sweep (12:00 -> now)                 */
    uint8_t reveal_ease;
    uint16_t pulse_ms;   /**< the flash when the minute turns over              */
    uint8_t pulse_ease;
    uint16_t trail_ms;   /**< how long the comet glow lasts                     */
    int16_t shadow_off;  /**< a hand's drop shadow offset, px                   */
    uint8_t shadow_alpha;/**< and its opacity, Q8                               */
} cgl_watch_anim_t;

/** The whole dial: geometry, palette, counting, animations, the initial reading,
    and which parts are drawn. */
typedef struct
{
    cgl_watch_geom_t geom;
    cgl_watch_anim_t anim;
    cgl_watch_style_t style;

    cgl_watch_hand_t hour;
    cgl_watch_hand_t minute;
    cgl_watch_hand_t second;

    /** The reading the face starts at (the anchor), and its date. */
    cgl_watch_time_t start;

    /** CGL_WATCH_F_* */
    uint16_t flags;

    /** The printed line, only drawn when `flags & CGL_WATCH_F_BRAND`. */
    const char *brand;
    const cgl_font_t *brand_font;
    int16_t brand_dx; /**< the line's position, offset from the dial centre */
    int16_t brand_dy;
    cgl_color_t brand_color;
} cgl_watch_config_t;

/* -------------------------------------------------------------------------- */
/* The widget                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * The watch face. The whole thing is static storage -- no heap, ever -- so a caller
 * declares one and hands it to `cgl_watch_init()`.
 *
 * The struct is public because a caller must be able to declare it (see
 * the CytGL manual, section 16.1); nothing outside this module should write to a
 * field directly, because the damage model depends on the bookkeeping below being
 * consistent.
 */
typedef struct
{
    cgl_widget_t base; /**< <- must be first */

    cgl_watch_config_t cfg;

    /* --- the time base ----------------------------------------------------- */
    cgl_watch_time_t t0;   /**< the wall time at the anchor instant         */
    uint32_t t0_ms;        /**< `clk->now_ms` at the anchor instant         */
    cgl_watch_time_t now;  /**< derived every frame from the two above      */
    uint32_t elapsed_days; /**< whole days added to `t0`'s date so far      */

    /** A time set from outside waits here for the next frame: the *anchor* has to
        be a `clk->now_ms`, and that value only exists inside `cgl_watch_frame()`. */
    bool set_pending;
    cgl_watch_time_t set_to;

    /* --- edge detection (a change is what starts an animation) ------------- */
    uint8_t primed; /**< 0 until the first frame has adopted a reading */
    uint8_t prev_second;
    uint8_t prev_minute;
    uint8_t prev_day;
    /** Last frame's reading: a hand is damaged at this angle **and** at the new
        one, which is what makes a rotating hand's damage exact. */
    cgl_watch_time_t prev_time;
    /** The minute and hour hands' tips as they were last drawn. They creep, so
        their large damage boxes are only marked on the frames where these really
        change -- otherwise every frame would carry their band range as well. */
    cgl_point_t prev_minute_pt;
    cgl_point_t prev_hour_pt;

    /* --- animations -------------------------------------------------------- */
    cgl_anim_t reveal; /**< 0..256: the entrance sweep (12:00 -> now) */
    cgl_anim_t pulse;  /**< 0..256: the flash when the minute turns over */

    /* --- the comet trail --------------------------------------------------- */
#if CGL_CFG_WATCH_TRAIL
    /**
     * One Q8 brightness per minute mark, decayed every frame and re-lit when the
     * second hand crosses it. 60 bytes, and it is the **only** per-element state a
     * watch face keeps -- everything else is a function of the time.
     */
    uint8_t trail[CGL_WATCH_TRAIL_MARKS];
    /** Sub-unit remainder of the trail decay: the decay per frame is a fraction of
        one brightness unit, and dropping it would make the comet either four
        seconds long or immortal. */
    uint16_t trail_accum;
#endif

    /* --- readouts a caller or a test may ask for ---------------------------- */
    uint32_t frames; /**< `cgl_watch_frame()` calls received */
} cgl_watch_t;

/**
 * Fill `cfg` with a complete, working, reference watch: the demo page's dial --
 * geometry, palette, surfaces, hands, tick, and the initial reading 10:09:00 on
 * 2026-01-14 (a Wednesday).
 *
 * WARNING: `cfg.style.face.lut` / `well.lut` are pointed at the module's own
 * **flash** tables when `CGL_CFG_WATCH_DIAL_LUT` is 1, and left NULL (the
 * procedural path, no flash) when it is 0. Either way the result is a working
 * watch; a caller with its own palette replaces the two `cgl_arc_surface_t`s
 * wholesale, which is what the demo page does.
 */
void cgl_watch_config_default(cgl_watch_config_t *cfg);

/** Convenience: set the initial reading (the anchor) in `cfg`. Out-of-range values
    are reduced modulo the next larger unit, so a caller cannot put the dial into a
    state it cannot draw. */
void cgl_watch_config_set_start(cgl_watch_config_t *cfg, uint8_t h, uint8_t m, uint8_t s);

/** The other half: the anchor's calendar date. `wday` is 0 = Sunday. */
void cgl_watch_config_set_start_date(cgl_watch_config_t *cfg, uint16_t year, uint8_t month,
                                     uint8_t day, uint8_t wday);

/**
 * Create the widget: copy `cfg` (or the defaults when it is NULL), bind the frame
 * and build nothing else. The dial is drawn from the configuration every frame, so
 * there is no tree to build underneath it.
 *
 * WARNING: the widget does **not** paint a background. Its frame should be clipped
 * (`CGL_WF_CLIP`, which `cgl_widget_init()` sets) and whatever surrounds the dial is
 * the page's business -- the host widget has already flooded the band before this
 * draws, which is what makes "a band handed to the panel is complete" true.
 */
void cgl_watch_init(cgl_watch_t *w, const char *name, cgl_rect_t frame,
                    const cgl_watch_config_t *cfg);

/**
 * One frame: keep time, run the entrance and the pulse, age the comet trail, and
 * mark **only the damage the face really changed**.
 *
 * This is the whole per-frame contract, and it wants the framework's frame clock
 * rather than a `dt`: the anchor has to be a `clk->now_ms`, and taking the time
 * from the global clock is what makes the face keep time while it is covered and
 * not being drawn.
 *
 * `clk == NULL` is a no-op (there is no time to show and nothing that could have
 * moved), which makes "call it from `on_frame` with whatever you were handed"
 * safe.
 */
void cgl_watch_frame(cgl_watch_t *w, const cgl_clock_t *clk);

/**
 * Re-anchor the face at a wall time: from the next frame on, it shows `h:m:s`
 * advancing from that instant. This is the RTC hook -- a battery-backed clock only
 * has to call it once, and again whenever the time is set.
 *
 * Values out of range are reduced modulo the next larger unit (s and m into
 * minutes and hours, the hour into 24 h), so a caller cannot put the dial into a
 * state it cannot draw. The request is parked and applied at the top of the next
 * `cgl_watch_frame()`, so calling it before the first frame is legal **and exact**.
 */
void cgl_watch_set_time(cgl_watch_t *w, uint8_t h, uint8_t m, uint8_t s);

/**
 * Re-anchor the face at a calendar date, the other half of the RTC hook.
 *
 * `wday` is 0 = Sunday..6 = Saturday and is **taken from the caller** rather than
 * computed: the face counts days forward from the anchor and cannot derive a
 * weekday from first principles without a full civil-calendar routine. Pass the
 * anchor's real weekday once and every rollover after it is correct.
 *
 * Out of range is **clamped**, not wrapped: a month of 13 or a day of 0 is a caller
 * bug, and silently rendering "0 FEB" would hide it. (The *time* of day wraps,
 * because `h = 25` meaning 1 am is a real thing a caller does by accident and the
 * result is still unambiguous; a date has no such convention.)
 */
void cgl_watch_set_date(cgl_watch_t *w, uint16_t year, uint8_t month, uint8_t day, uint8_t wday);

/** The time the face is showing right now (valid after the first frame; before it,
    the configured start reading). NULL for a NULL widget. */
const cgl_watch_time_t *cgl_watch_now(const cgl_watch_t *w);

/** Seconds since midnight, of the reading the face is showing. */
uint32_t cgl_watch_seconds_of_day_now(const cgl_watch_t *w);

/**
 * Play the entrance: the hands sweep from 12:00 to the current time over
 * `cfg.anim.reveal_ms`, and the chapter ring assembles itself mark by mark.
 *
 * It is called automatically, **once**, by `cgl_watch_init()` when
 * `CGL_WATCH_F_REVEAL` is set -- so a face animates itself up out of the box. A page
 * that wants the sweep to replay every time it is entered calls this from its own
 * `on_enter`, which is exactly what the demo page does, because arriving at a watch
 * face is the one moment that animation belongs to.
 *
 * WARNING: nothing else replays it, and in particular **a time set does not**.
 * `cgl_watch_set_time()` re-anchors the reading; if it also restarted the entrance,
 * the hands would leave for 12:00 and creep back, and for eight tenths of a second
 * after every set the face would show a pose between twelve and the time it was
 * asked for.
 */
void cgl_watch_reveal(cgl_watch_t *w);

/** How much of the entrance has played, Q8 (256 = the face is fully up). */
uint16_t cgl_watch_reveal_value(const cgl_watch_t *w);

/** The minute pulse's current level, Q8 (256 = the flash is at its peak, 0 = the
    resting "nothing is happening" value). Handy for tinting something the watch
    does not own -- a complication ring, say -- with the same flash. */
uint16_t cgl_watch_pulse_value(const cgl_watch_t *w);

/* --- runtime setters -------------------------------------------------------
 * Every field of the configuration has a door, so a settings screen can change the
 * face without a rebuild. Each one repaints (or not) exactly as documented, and the
 * cheap ones are cheap: a mode that is already set costs one comparison.
 * ------------------------------------------------------------------------ */

/** Choose between the sweep and the tick. Repaints: the hand jumps. */
void cgl_watch_set_second_mode(cgl_watch_t *w, cgl_watch_second_mode_t mode);

/** How long the tick's snap takes, ms, and its curve. Repaints. */
void cgl_watch_set_tick(cgl_watch_t *w, uint16_t tick_ms, cgl_ease_t ease);

/** Turn face elements on and off (`CGL_WATCH_F_*`). Repaints. */
void cgl_watch_set_flags(cgl_watch_t *w, uint16_t flags);

/** Turn one element on or off, leaving the others alone. Repaints. */
void cgl_watch_set_feature(cgl_watch_t *w, uint16_t flag, bool on);

/** Are all of `flags` currently on? */
bool cgl_watch_has_flags(const cgl_watch_t *w, uint16_t flags);

/**
 * Move the dial. The geometry is in screen coordinates and the widget frame is
 * **not** consulted for it, so a caller that moves the frame must move the geometry
 * too -- or use this, which does both by the same delta.
 */
void cgl_watch_set_center(cgl_watch_t *w, int16_t cx, int16_t cy);

/** Replace the whole configuration (a settings screen's "apply"). Re-anchors the
    time to the new start reading and repaints. */
void cgl_watch_set_config(cgl_watch_t *w, const cgl_watch_config_t *cfg);

/** The configuration in force (read-only; use the setters to change it). */
const cgl_watch_config_t *cgl_watch_get_config(const cgl_watch_t *w);

/**
 * The dial's colour at an offset from its centre, straight out of the configured
 * face surface. Pure: same offset, same colour, for ever, and answerable before the
 * widget has ever been drawn -- which is what makes the *design* assertable ("the
 * centre is brighter than the rim", "the upper left is brighter than the lower
 * right") instead of describable.
 */
cgl_color_t cgl_watch_face_color(const cgl_watch_t *w, int16_t dx, int16_t dy);

/** The same, for the recessed ring: the well is *concave* where the face is
    *convex*, so it is dark at the top and bright at the bottom -- the opposite way
    round from the dial's highlight, which is what makes it read as sunk in. */
cgl_color_t cgl_watch_ring_color(const cgl_watch_t *w, int16_t dx, int16_t dy);

/**
 * What the crystal cover does to a colour at an offset from the centre: the light
 * it reflects, composited over `under`, which may be anything -- the dial, the
 * ring, a mark, a hand.
 *
 * It is public for the same reason the two surfaces are: a test that checks "the
 * hand is still orange, and it is orange *through the glass*" has to be able to
 * compute what the glass does, or it has to weaken that to "roughly orange".
 *
 * `cgl_watch_face_color()` and `cgl_watch_ring_color()` deliberately do **not**
 * include it: they are the *surfaces*, and this is what lies over them.
 */
cgl_color_t cgl_watch_glass_over(const cgl_watch_t *w, cgl_color_t under, int16_t dx, int16_t dy);

/**
 * The same function over the **parameters** rather than a widget instance.
 *
 * It exists because the glass is a pure function and a caller may legitimately want
 * it without a widget: the demo page hands it to a test as
 * `app_page_clock_glass_over(under, dx, dy)` -- a free function with no page, called
 * before the page has ever been created -- and a caller laying a second crystal
 * over its own dial wants the same. `cgl_watch_glass_over()` is the convenience
 * wrapper around this one, so the two can never disagree.
 */
cgl_color_t cgl_watch_glass_shade(const cgl_watch_glass_t *gl, cgl_color_t under, int16_t dx,
                                  int16_t dy);

/** How many `cgl_watch_frame()` calls the widget received. */
uint32_t cgl_watch_frames(const cgl_watch_t *w);

/** Default dial tables, in flash, when `CGL_CFG_WATCH_DIAL_LUT` is 1. They are the
    data `cgl_watch_config_default()` points its two surfaces at; a caller with a
    different palette either bakes its own with `cgl_arc_surface_bake()` or leaves
    `lut` NULL and pays for a procedural shade instead. */
#if CGL_CFG_WATCH_DIAL_LUT
extern const uint16_t cgl_watch_face_lut_default[CGL_ARC_FACE_LUT_N];
extern const uint16_t cgl_watch_well_lut_default[CGL_ARC_RING_LUT_N];
#endif

#ifdef __cplusplus
}
#endif

#endif /* CGL_CFG_WATCH */

#endif /* CGL_WATCH_H */
