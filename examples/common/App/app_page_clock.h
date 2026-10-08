// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    app_page_clock.h
  * @brief   Watch face page: the demo's composition of the framework's
  *          `cgl_watch` dial with the complications a watch face carries.
  *
  *  What this page is now
  *  ---------------------
  *  This file used to describe 2 500 lines of dial: the face, the well, the
  *  chapter ring, the three hands, the hub, the crystal and the seconds ring were
  *  all implemented here, because the framework had no way to draw a watch. It has
  *  one now -- `cgl_arc` (shaded surfaces, tapered blades, the ring gauge) and
  *  `cgl_watch` (the dial, its time base and its damage model) -- so what is left
  *  here is exactly what belongs to an *application*:
  *
  *      +------------------------------+------------------+
  *      |   THE PAGE'S OWN WORK:       |   ,----------,   |  a raised card
  *      |   one radial wall lit from   |   |   10:09  |   |  the complications
  *      |   the dial's own light,      |   |  WEDNESDAY|  |  sit on
  *      |   bevelled edges, a card     |   |   14 JAN |   |
  *      |                              |   |  ( sec ) |   |
  *      |      ,--------------,        |   |          |   |
  *      |    /  cgl_watch_t:   \      |   `----------'   |
  *      |   |  face, well, ring, |     |                  |
  *      |    \ ticks, hands,   /      |                  |
  *      |      `-- glass --'          |                  |
  *      +------------------------------+------------------+
  *        dial centre (86, 86), r=80      column centre x = 246
  *
  *  The dial is a `cgl_watch_t` configured by `s_cfg` in the .c; the digital time
  *  (four digits that roll independently), the weekday and date labels, and the
  *  seconds gauge are this page's own widgets. The gauge is built on
  *  `cgl_arc_gauge_t`, so it is the framework's arc toolkit demonstrated rather
  *  than another hand-rolled ring.
  *
  *  The time source, honestly stated
  *  --------------------------------
  *  **This board has no RTC.** The CubeMX configuration brings up LSE on PC14/PC15
  *  but never instantiates the RTC peripheral, so there is no battery-backed wall
  *  clock to read. The page therefore runs off the framework's own clock, through
  *  `cgl_watch_t`: `t0` is the time at the anchor instant, `t0_ms` is `clk->now_ms`
  *  at that instant, and the displayed time is `t0 + (clk->now_ms - t0_ms)` -- exact,
  *  monotonic, drift-free, and still running while the page is covered (the frame
  *  clock is global and is not part of the page's state). The power-on time is
  *  `CLOCK_START_*` below: 10:09, the time every watch is photographed at, because
  *  at 10:09 the two visible hands frame the upper half of the dial symmetrically.
  *
  *  Wiring a real time source is one call, `app_page_clock_set_time()`, which
  *  re-anchors the dial at any instant (from an RTC ISR, from a host test, from a
  *  "set the time" screen) -- and nothing else in the page has to change.
  *
  *  WARNING: the displayed **date** is anchored at `CLOCK_START_Y/M/D` and counts
  *  forward from there at one day per 86400 s, so it is a real calendar (month
  *  lengths and leap years included) -- just not one that knows what today is.
  *
  *  The two second-hand motions
  *  ---------------------------
  *  `CLOCK_SECOND_MODE` still chooses between a continuous sweep and a
  *  one-step-per-second tick, exactly as before:
  *
  *      -DCLOCK_SECOND_MODE=CLOCK_SECOND_TICK     # or edit the default below
  *
  *  It is a **compile-time** switch here and a **run-time** field in the framework
  *  (`cgl_watch_set_second_mode()`), and both are deliberate. The framework's mode
  *  is an argument so that an application can offer both in a settings screen; this
  *  page keeps its macro because the demo is the thing that *demonstrates* the two
  *  motions side by side in two builds (`tests/clock_host_test.c` is compiled twice,
  *  once per mode). The two are kept from drifting apart by two `_Static_assert`s
  *  below: the page's numbers must equal the framework's enum.
  ******************************************************************************
  */
#ifndef APP_PAGE_CLOCK_H
#define APP_PAGE_CLOCK_H

#include <cytgl/cgl_types.h>
#include <cytgl/cgl_page.h>
#include <cytgl/cgl_panel.h>
#include <cytgl/cgl_label.h>
#include <cytgl/cgl_anim.h>
#include <cytgl/cgl_arc.h>
#include <cytgl/cgl_watch.h>
#include <cytgl/cgl_screen.h>

#include "app_ctx.h"

/* -------------------------------------------------------------------------- */
/* Layout -- public, because the host test asserts the geometry              */
/* -------------------------------------------------------------------------- */

/*
 * The dial's numbers are the framework's defaults (CGL_CFG_WATCH_*), and the page
 * names them because it also has to place things *around* the dial: the widget
 * frame, the complication column, the brand line. WARNING: they are aliases, not
 * copies -- 86 and 80 written out twice is how a design ends up half moved.
 *
 * The two numbers that belong to the *palette* rather than to the layout -- the
 * table resolutions and the well's radii -- stay in app_page_clock_dial.h, with the
 * tables they describe; see the note there.
 */

/** Dial centre and the radius of the face (the panel is 320 x 172, so 86 +- 80
    nearly touches both edges: this is as large as a dial may be). */
#define CLOCK_CX CGL_CFG_WATCH_CX
#define CLOCK_CY CGL_CFG_WATCH_CY
#define CLOCK_R_FACE CGL_CFG_WATCH_R_FACE

/** Seconds ring: a stroke centred on this radius, hugging the bezel. */
#define CLOCK_R_RING CGL_CFG_WATCH_R_RING
#define CLOCK_RING_W CGL_CFG_WATCH_RING_W

/** The **lead dot** on the ring's growing end: its radius at rest and its widest,
    which is during the minute pulse. The widest value is what the damage margin has
    to cover, or the dot's edge is never repainted and the panel keeps a crescent of
    where it used to be. */
#define CLOCK_DOT_R 3
#define CLOCK_DOT_R_MAX 7

/** Chapter ring: hour and minute marks, pointing outward from the dial centre. */
#define CLOCK_R_TICK_OUT CGL_CFG_WATCH_R_TICK_OUT
#define CLOCK_R_TICK_HOUR_IN CGL_CFG_WATCH_R_TICK_HOUR_IN
#define CLOCK_R_TICK_MIN_IN CGL_CFG_WATCH_R_TICK_MIN_IN

/** Hand lengths (tip radius) and the radius of the hub. */
#define CLOCK_R_HOUR CGL_CFG_WATCH_R_HOUR
#define CLOCK_R_MINUTE CGL_CFG_WATCH_R_MINUTE
#define CLOCK_R_SECOND CGL_CFG_WATCH_R_SECOND
#define CLOCK_R_TAIL CGL_CFG_WATCH_R_TAIL /**< how far the second hand's counterweight reaches back */
#define CLOCK_R_HUB CGL_CFG_WATCH_R_HUB

/** How far a hand's base reaches back **behind** the hub, and how wide it is there.
    A hand is a tapered blade, not a stick: it is at its widest at the arbor and
    narrows to the tip, and the wide end is hidden under the hub -- so the visible
    shape starts at the hub's edge already tapered, which is what makes it read as a
    watch hand instead of a rectangle. */
#define CLOCK_R_HOUR_BASE CGL_CFG_WATCH_R_HOUR_BASE
#define CLOCK_R_MINUTE_BASE CGL_CFG_WATCH_R_MINUTE_BASE

/** The specular highlight: a soft blob where a studio light would land on a domed
    crystal -- up and to the left of the centre, the same side the hands' shadow is
    *not* on. Public because it is the *geometry of the light*, and the test asserts
    that the upper left of the dial really is lighter than the lower right. */
#define CLOCK_SPEC_DX (-27)
#define CLOCK_SPEC_DY (-31)
#define CLOCK_SPEC_R 48

/** The hands' drop shadow: how far it is offset (down-right) and how dark it is,
    Q8. 30 % of full coverage is enough to read as "the hand floats above the dial"
    without turning into a second, black hand. */
#define CLOCK_SHADOW_PX CGL_CFG_WATCH_SHADOW_PX
#define CLOCK_SHADOW_ALPHA CGL_CFG_WATCH_SHADOW_ALPHA

/** The complication column. */
#define CLOCK_COL_CX 246
#define CLOCK_TIME_Y 12
#define CLOCK_WDAY_Y 42
#define CLOCK_DATE_Y 58
#define CLOCK_GAUGE_CY 124
#define CLOCK_GAUGE_R 28

/** Power-on wall time and the anchor date (see the file header: there is no RTC). */
#define CLOCK_START_H 10u
#define CLOCK_START_M 9u
#define CLOCK_START_S 0u
#define CLOCK_START_Y 2026u
#define CLOCK_START_MON 1u
#define CLOCK_START_D 14u
#define CLOCK_START_WDAY 3u /**< 0 = Sunday; 2026-01-14 is a Wednesday */

/* --- how the second hand moves ---------------------------------------------
 *
 *  The one choice on this page that is a matter of *taste* rather than of geometry,
 *  and therefore a compile-time switch:
 *
 *      CLOCK_SECOND_MODE = CLOCK_SECOND_SWEEP  a continuous glide (a quartz
 *                                              "sweep" movement)
 *      CLOCK_SECOND_MODE = CLOCK_SECOND_TICK   one step per second, with a whip and
 *                                              a small overshoot (a mechanical one)
 *
 *  Both are implemented once, in `cgl_watch_second_angle()` in cgl_watch.h, and
 *  both are **pure functions of the time**: the tick is a curve over `now.ms`, not a
 *  tween with state, so nothing accumulates and a dropped frame costs nothing but
 *  smoothness -- the same property the sweep has.
 *
 *  WARNING: the mode is deliberately a single number and not two independent
 *  `#define CLOCK_SECOND_TICK 1` style switches: "which one of the two" is the
 *  question, and two booleans would allow a third state that means nothing.
 *
 *  WARNING: these are plain integers and not aliases of the framework's enum, and
 *  they have to be: `#if CLOCK_SECOND_MODE == CLOCK_SECOND_TICK` is how the test
 *  suite picks its build, and the preprocessor reads an unknown identifier as 0 --
 *  an alias of `CGL_WATCH_SECOND_TICK` would make both modes compare equal. The two
 *  `_Static_assert`s below are what stops the numbers drifting apart.
 * -------------------------------------------------------------------------- */
#define CLOCK_SECOND_SWEEP 0 /**< the hand glides: angle = (s*1000 + ms) / 60 s      */
#define CLOCK_SECOND_TICK 1  /**< the hand steps: it holds, then snaps in CLOCK_TICK_MS */

/** Which of the two motions above the second hand uses. Override from the build
    (`-DCLOCK_SECOND_MODE=CLOCK_SECOND_TICK`) or by editing this line. */
#ifndef CLOCK_SECOND_MODE
#define CLOCK_SECOND_MODE CLOCK_SECOND_TICK
#endif

_Static_assert(CLOCK_SECOND_SWEEP == (int)CGL_WATCH_SECOND_SWEEP,
               "CLOCK_SECOND_SWEEP must equal CGL_WATCH_SECOND_SWEEP: the page and the "
               "framework would otherwise disagree about which motion is which");
_Static_assert(CLOCK_SECOND_TICK == (int)CGL_WATCH_SECOND_TICK,
               "CLOCK_SECOND_TICK must equal CGL_WATCH_SECOND_TICK");

/** How long the tick's snap takes, ms -- measured **backwards from the next
    second**: the hand rests on the current mark for `CGL_CLOCK_MS_PER_S -
    CLOCK_TICK_MS` and the last `CLOCK_TICK_MS` of every second is the whip that
    carries it onto the next one. See `cgl_watch_second_yank_ms()` for the shape and
    for the three ways it was wrong first. */
#define CLOCK_TICK_MS CGL_CFG_WATCH_TICK_MS

/** The tick's curve, and the reason a mechanical second hand reads as mechanical:
    `CGL_EASE_OUT_BACK` overshoots by about 10 % of the step and springs back, so the
    hand lands on its mark from *past* it -- the little bounce a jewelled lever
    actually makes. `CGL_EASE_IN_QUAD` is the calm alternative. */
#define CLOCK_TICK_EASE CGL_EASE_OUT_BACK

/** Entrance reveal: how long the hands take to sweep from 12:00 to now. */
#define CLOCK_REVEAL_MS CGL_CFG_WATCH_REVEAL_MS

/** The flash the dial gives when the minute turns over. */
#define CLOCK_PULSE_MS CGL_CFG_WATCH_PULSE_MS

/** How long a digit takes to roll to its next value. */
#define CLOCK_ROLL_MS 300u

/** Number of digits drawn for HH:MM (not counting the colon). */
#define CLOCK_DIGITS 4

/**
 * A wall-clock time, plus the calendar fields the face shows.
 *
 * WARNING: **this name now aliases the framework's `cgl_watch_time_t`**, which is
 * where the struct moved when the dial did. The layout and the field names are
 * unchanged, so every caller and every test that reads `t->h` / `t->wday` is
 * untouched -- but there is now exactly one definition of "a wall clock reading" in
 * the project instead of one per page that shows a time.
 */
typedef cgl_watch_time_t clock_time_t;

/** `num / den` of a turn, as an angle -- the framework's, so a clock face and a
    framework gauge cannot disagree about which way clockwise is. */
CGL_AINLINE cgl_angle_t clock__fraction_angle(uint32_t num, uint32_t den)
{
    return cgl_watch_fraction_angle(num, den);
}

/** The angle of a face reading: 12 o'clock (`CGL_ANGLE_UP`) plus a clockwise part. */
CGL_AINLINE cgl_angle_t clock__face_angle(uint32_t num, uint32_t den)
{
    return cgl_watch_face_angle(num, den);
}

/** How far the hand has been yanked past the last second boundary, in milliseconds
    of dial. The page's view of `cgl_watch_second_yank_ms()` with this build's
    chosen tick. */
CGL_AINLINE uint32_t clock__second_yank_ms(uint16_t ms)
{
    return cgl_watch_second_yank_ms(ms, (uint16_t)CLOCK_TICK_MS, (uint8_t)CLOCK_TICK_EASE);
}

/**
 * The second hand at a reading: this page's view of
 * `cgl_watch_second_angle()` with the mode this translation unit was compiled with.
 *
 * WARNING: it is `CGL_AINLINE` and it lives in the **header** on purpose, and that
 * has always been for the same reason: the motion is the one thing on this page a
 * build switch chooses between, and a switch read in one translation unit is a
 * trap. It happened -- the first version had the tick in the .c, the tick test
 * compiled the page with `-DCLOCK_SECOND_MODE=CLOCK_SECOND_TICK` and measured a
 * perfectly smooth sweep, and no compiler warning said so. As an inline in the
 * header, the page and any test that includes this header compile the *same* body
 * under the *same* macro, and the two can never disagree.
 */
CGL_AINLINE cgl_angle_t clock__second_angle(const clock_time_t *t)
{
    return cgl_watch_second_angle(t, (uint8_t)CLOCK_SECOND_MODE, (uint16_t)CLOCK_TICK_MS,
                                  (uint8_t)CLOCK_TICK_EASE);
}

/** The hour hand, continuous (it creeps, the way a real one does). */
CGL_AINLINE cgl_angle_t clock__hour_angle(const clock_time_t *t)
{
    return cgl_watch_hour_angle(t);
}

/** The minute hand, continuous for the same reason. */
CGL_AINLINE cgl_angle_t clock__minute_angle(const clock_time_t *t)
{
    return cgl_watch_minute_angle(t);
}

/** How much of the minute has run, in angle units (the seconds ring's sweep). */
CGL_AINLINE uint32_t clock__minute_sweep(const clock_time_t *t)
{
    return cgl_watch_minute_sweep(t);
}

/* --- the page's own two complications -------------------------------------- */

/**
 * The digital time. A widget rather than a `cgl_label` because each **digit** moves
 * independently while it rolls: a label can only draw a string at one position, and
 * "the tens of minutes rolls up while the units of hours stays put" is the whole
 * point of the animation.
 */
typedef struct
{
    cgl_widget_t base;
    struct app_page_clock *page;
} clock_digits_t;

/**
 * The number inside the seconds gauge. It is a widget of its own because the ring
 * is not: the ring is a `cgl_arc_gauge_t` (the framework's radial twin of
 * `cgl_bar`, configured in the .c), and this draws the reading it sweeps to --
 * "9" rather than "09", because one digit reads as a watch complication and two as
 * a stopwatch.
 *
 * WARNING: the frame is deliberately the same 64 x 64 box as the gauge's, so the
 * two are centred on the same point; the ring is added to the tree first and the
 * number second, which is what puts the number on top -- draw order is the order of
 * the child list.
 */
typedef struct
{
    cgl_widget_t base;
    struct app_page_clock *page;
} clock_gauge_t;

/**
 * The page. The whole thing is static storage (see `app_ui.c`'s `s_app`).
 *
 * A **struct tag plus a typedef**, never a second typedef elsewhere: the menu
 * forward declares this page by tag (`struct app_page_clock;`) because it only ever
 * holds a pointer. See the warning in app_page_bullet.h.
 */
struct app_page_clock
{
    cgl_page_t base; /**< <- must be the first member */

    app_links_t *links;    /**< cross-page links, from ctx->app; unused today but
                                kept so the page has the same shape as every other */
    cgl_screen_t *screen;  /**< borrowed: the frame clock and the FPS readout */

    /* --- the face: time base, hands, ring, chapter marks, crystal ----------
     * Everything that used to be this page's 2 000 lines of dial is now one
     * framework widget configured by `s_cfg` in the .c. The page owns the time it
     * shows only through it: `app_page_clock_now()` is a door onto
     * `cgl_watch_now()`.
     * -------------------------------------------------------------------- */
    cgl_watch_t watch;

    /* --- the digital complication (the page's own) ------------------------- */
    cgl_anim_t roll;                     /**< 0..256: the digit roll          */
    uint8_t roll_from[CLOCK_DIGITS + 1]; /**< characters leaving, incl. the colon slot */
    uint8_t roll_to[CLOCK_DIGITS + 1];   /**< characters arriving             */
    bool rolling;

    /* --- the seconds gauge: the framework's arc gauge, configured --------- */
    cgl_arc_gauge_t gauge;
    clock_gauge_t gauge_num;

    /* --- edge detection of the page's own complications -------------------
     * WARNING: the *dial's* edge detection (which hand moved, which mark lit up)
     * is the watch widget's, and it keeps its own `prev_*` fields. These four are
     * only about what the **page** draws around the dial: the digit roll, the date
     * line and the gauge. They are separate on purpose -- a page that watches the
     * time twice would run two slightly different notions of "the minute changed",
     * and the digit roll and the pulse would drift a frame apart.
     * -------------------------------------------------------------------- */
    bool primed;
    /** A time was set from outside and the page has not yet re-adopted a reading.
        See the note on `app_page_clock_set_time()`. */
    bool reprime;
    uint8_t prev_second;
    uint8_t prev_minute;
    uint8_t prev_day;

    /* --- widgets ------------------------------------------------------------ */
    cgl_widget_t root; /**< a container: the host paints the background (see the .c) */
    clock_digits_t digits;
    cgl_label_t wday;
    cgl_label_t date;

    /* --- readouts the menu / a test may ask for ----------------------------- */
    uint32_t frames; /**< on_frame calls received */
    uint32_t steps;  /**< on_step calls received  */
};

/* The only declaration of the name `app_page_clock_t`, here. */
typedef struct app_page_clock app_page_clock_t;

/**
 * Create the page (registration only; the widget tree is built in `on_create`).
 * See `cgl_page_ctx_t` and the note at the top of app_ctx.h.
 */
void app_page_clock_attach(cgl_page_t *page, const cgl_page_ctx_t *ctx);

/**
 * Re-anchor the dial at a wall time: from the next frame on, the face shows `h:m:s`
 * advancing from that instant. This is the RTC hook -- a battery-backed clock only
 * has to call it once, and again whenever the time is set.
 *
 * Values out of range are reduced modulo the next larger unit (s/m into minutes and
 * hours, the hour into 12/24 h), so a caller cannot put the dial into a state it
 * cannot draw.
 *
 * The request is parked and applied at the top of the next frame, where the frame
 * clock (the anchor instant) is actually available; calling it before the first
 * frame is therefore legal and exact.
 */
void app_page_clock_set_time(app_page_clock_t *p, uint8_t h, uint8_t m, uint8_t s);

/**
 * Re-anchor the dial at a calendar date: the other half of the RTC hook (a real
 * time source knows the date as well as the time of day).
 *
 * `wday` is 0 = Sunday..6 = Saturday and is **taken from the caller** rather than
 * computed: this page counts days forward from that anchor and cannot derive a
 * weekday from first principles without a full civil-calendar routine. Pass the
 * anchor's real weekday once and every rollover after it is correct.
 */
void app_page_clock_set_date(app_page_clock_t *p, uint16_t year, uint8_t month, uint8_t day,
                             uint8_t wday);

/** The time the face is showing right now (valid after the first frame). */
const clock_time_t *app_page_clock_now(const app_page_clock_t *p);

/**
 * The dial face's colour at an offset from its centre, in the **page's own
 * coordinates** (dx > 0 is to the right, dy > 0 is down).
 *
 * The whole background of the dial -- the radial gradient, the brushed texture and
 * the specular highlight -- is one pure function of (dx, dy): the rasterizer sums
 * nothing across frames and keeps no state, so this call is exactly the pixel the
 * dial puts down (when nothing is drawn on top of it). It exists so a test can
 * assert the *lighting* rather than "some dark pixel": "the centre is brighter than
 * the rim", "the upper left is brighter than the lower right" are claims about a
 * picture that a screenshot cannot settle.
 *
 * Answers for any offset, inside the dial or not (the caller decides whether the
 * pixel is on the face at all). Safe to call before the page has ever been drawn --
 * it reads the page's palette, which is a compile-time constant, and no page state.
 */
cgl_color_t app_page_clock_face_color(int16_t dx, int16_t dy);

/**
 * The **outer ring's** colour at an offset from the dial centre: the recessed
 * cylindrical well between the chapter ring and the case lip, as one pure function
 * of (dx, dy).
 *
 * It is a separate door from `app_page_clock_face_color()` because it is a separate
 * surface: the face is a *convex* dome lit from the upper left, and this well is
 * *concave* -- dark at the top, bright at the bottom, which is the shading that
 * makes it read as sunk into the dial rather than standing on it. A test asserts
 * exactly that, which is why the function is public.
 */
cgl_color_t app_page_clock_ring_color(int16_t dx, int16_t dy);

/**
 * The **background** at a screen coordinate: the wall the whole page sits on (one
 * radial gradient, lit from up and to the left of the dial -- the same place the
 * dial's highlight is), the raised card the complications sit on, and the panel's
 * two bevelled edge rows.
 *
 * Screen coordinates, not dial-relative: it is the *page* that is being designed
 * here, and it is the one surface this page still owns outright (`cgl_watch` draws
 * a dial, not a room). Pure, like the dial's surfaces, so a test can assert the
 * design -- "the wall is lit from the dial's own light", "the card is lifted off
 * the wall", "the top edge catches the light" -- instead of describing it.
 */
cgl_color_t app_page_clock_bg_color(int16_t x, int16_t y);

/**
 * What the **crystal cover** does to a colour at an offset from the dial centre: the
 * light it reflects -- a wide soft sweep, a bright crescent along the rim, and a
 * faint echo of that crescent opposite -- composited over `under`, which may be
 * anything: the dial, the ring, a mark, a hand.
 *
 * The cover is the last thing the dial draws, and it is a *blend* rather than a
 * colour, which is exactly what makes it read as a cover: a reflection that stops at
 * the edge of a hand is painted on the dial, and one that runs across the hand is in
 * front of it. It is public for the same reason the two surfaces are -- a test that
 * checks "the hand is still orange, and it is orange *through the glass*" has to be
 * able to compute what the glass does, or it has to weaken that to "roughly orange".
 */
cgl_color_t app_page_clock_glass_over(cgl_color_t under, int16_t dx, int16_t dy);

/** Seconds since midnight, for a test or a HUD (cheap: it is derived state). */
uint32_t app_page_clock_seconds_of_day(const app_page_clock_t *p);

/** How much of the entrance reveal has played, Q8 (256 = the face is fully up). */
uint16_t app_page_clock_reveal(const app_page_clock_t *p);

/**
 * Where the **second hand** points at a given second and millisecond, in the dial's
 * angle convention (0 = 3 o'clock, growing clockwise -- so a reading of :00 is
 * `CGL_ANGLE_UP`, 12 o'clock).
 *
 * This is the page's one motion that has a **compile-time choice** behind it
 * (`CLOCK_SECOND_MODE`: a continuous sweep or a one-step-per-second tick), and the
 * two are a *shape* rather than a colour or a position -- there is no pixel a
 * screenshot can point at and say "this is the tick". A shape needs measuring, and
 * measuring needs a handle on it: this is that handle, and it is the page's fourth
 * pure function (with the face, the ring and the glass).
 *
 * It is the same expression the dial draws with -- `clock__second_angle()`, which is
 * underlined by CLOCK_SECOND_MODE only -- so a test can state the claims the mode
 * makes ("at :04.4 the hand is still parked on :04", "at :04.9 it has snapped and is
 * past halfway to :05", "at :05.0 exactly it is on :05") exactly rather than
 * approximately. `(s, ms)` are taken modulo 60 s and 1000 ms, so out-of-range inputs
 * are still answerable.
 *
 * Safe to call before the page has ever been drawn: it reads no state.
 */
cgl_angle_t app_page_clock_second_angle(uint8_t s, uint16_t ms);

/** Frame callbacks this page received (does not grow while the page is covered). */
uint32_t app_page_clock_frames(const app_page_clock_t *p);

/** Logical steps this page received. */
uint32_t app_page_clock_steps(const app_page_clock_t *p);

#endif /* APP_PAGE_CLOCK_H */
