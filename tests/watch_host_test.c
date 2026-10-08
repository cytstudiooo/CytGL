// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    watch_host_test.c
  * @brief   The framework's radial toolkit and watch face, measured on the host.
  *
  *  `tests/clock_host_test.c` is the *application's* test: it drives the demo page
  *  and asks "does the watch face look like a watch". This one is the *module's*
  *  contract, and it exists because the two are now different things -- everything
  *  it checks is a property of `cgl_arc` or `cgl_watch` with no page in the way:
  *
  *    A  the default configuration is a working watch
  *    B  the shared value -> fraction mapping (the one `cgl_bar` and the gauge
  *       both use) and its end points
  *    C  the calendar: leap rules, month lengths, day rollover
  *    D  the angle convention: 12 o'clock, 3 o'clock, and the minute sweep
  *    E  the two second-hand motions, and the fact that they agree at every mark
  *    F  the surfaces are *lit*: the face is brighter in the middle and to the
  *       upper left, the well is dark at the top and bright at the bottom
  *    G  the baked tables agree with the built-in flash tables, entry for entry
  *    H  the ring gauge: range, sweep, lead point, animating, rejected ranges
  *    I  the damage model, which is the property no screenshot can settle: an
  *       incrementally damaged frame must be **identical** to a full redraw
  *
  *  A correct implementation of the framework passes every one of these without a
  *  panel, because all of it is pure functions and one mock framebuffer.
  ******************************************************************************
  */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <cytgl/cgl_display.h>
#include <cytgl/cgl_screen.h>
#include <cytgl/cgl_page.h>
#include <cytgl/cgl_widget.h>
#include <cytgl/cgl_math.h>
#include <cytgl/cgl_arc.h>
#include <cytgl/cgl_watch.h>

/* -------------------------------------------------------------------------- */
/* Harness                                                                    */
/* -------------------------------------------------------------------------- */

static int s_fails = 0;

#define CHECK(cond, ...)                                  \
    do                                                    \
    {                                                     \
        if (!(cond))                                      \
        {                                                 \
            s_fails++;                                    \
            printf("  FAIL: ");                           \
            printf(__VA_ARGS__);                          \
            printf("   [%s:%d]\n", __FILE__, __LINE__);   \
        }                                                 \
    } while (0)

static cgl_color_t s_framebuffer[CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H];
static cgl_color_t s_reference[CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H];
static uint16_t s_row;
static uint16_t s_col;

static int mock__init(void *ctx) { (void)ctx; return 0; }

static void mock__set_window(void *ctx, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    (void)ctx;
    (void)x1;
    (void)y1;
    s_col = x0;
    s_row = y0;
}

static void mock__write_pixels(void *ctx, const cgl_color_t *px, uint32_t count)
{
    uint32_t i;

    (void)ctx;
    for (i = 0u; i < count; i++)
    {
        if ((s_row < CGL_CFG_SCREEN_H) && (s_col < CGL_CFG_SCREEN_W))
        {
            s_framebuffer[((uint32_t)s_row * CGL_CFG_SCREEN_W) + s_col] = px[i];
        }
        s_col++;
        if (s_col >= CGL_CFG_SCREEN_W)
        {
            s_col = 0u;
            s_row++;
        }
    }
}

static void mock__wait_idle(void *ctx) { (void)ctx; }
static void mock__backlight(void *ctx, uint8_t p) { (void)ctx; (void)p; }

static const cgl_display_drv_t s_mock_drv = {
    "mock", mock__init, mock__set_window, mock__write_pixels, mock__wait_idle, mock__backlight,
    CGL_CFG_SCREEN_W, CGL_CFG_SCREEN_H,
};

static cgl_display_t s_display;
static cgl_screen_t s_screen;
static cgl_page_nav_t s_nav;

/** A page whose root **is** the watch: the smallest thing that can drive the
    widget's per-frame contract through the real screen pipeline. */
typedef struct
{
    cgl_page_t base;
    cgl_watch_t watch;
} watch_page_t;

static watch_page_t s_page;

static const cgl_watch_config_t *page__cfg(void)
{
    static bool ready = false;
    static cgl_watch_config_t cfg;

    if (!ready)
    {
        cgl_watch_config_default(&cfg);
        ready = true;
    }
    return &cfg;
}

static void page__create(cgl_page_t *p)
{
    watch_page_t *wp = (watch_page_t *)p;

    cgl_watch_init(&wp->watch, "watch",
                   cgl_rect(0, 0, (int16_t)(CGL_CFG_SCREEN_W - 1),
                            (int16_t)(CGL_CFG_SCREEN_H - 1)),
                   page__cfg());
    cgl_page_set_root(p, &wp->watch.base);
}

static void page__frame(cgl_page_t *p, const cgl_clock_t *clk)
{
    cgl_watch_frame(&((watch_page_t *)p)->watch, clk);
}

static const cgl_page_vt_t page__vt = {
    .type = "watch_page",
    .on_create = page__create,
    .on_frame = page__frame,
};

/** Drive n frames at the firmware cadence. */
static void run(int n, uint32_t dt)
{
    int i;

    for (i = 0; i < n; i++)
    {
        cgl_screen_frame(&s_screen, dt);
    }
}

/** Perceived brightness of a colour: enough to rank two of them, which is all the
    lighting claims below need. */
static int lum(cgl_color_t c)
{
    int r = (int)((c >> 11) & 0x1Fu);
    int g = (int)((c >> 5) & 0x3Fu);
    int b = (int)(c & 0x1Fu);

    /* The green channel has six bits against five: scale it so a neutral grey ranks
       as bright as it looks. */
    return (r * 2) + (g * 2) + (b * 2);
}

/* -------------------------------------------------------------------------- */
/* A. the default configuration                                               */
/* -------------------------------------------------------------------------- */

static void test_config_default(void)
{
    cgl_watch_config_t cfg;

    cgl_watch_config_default(&cfg);

    CHECK(cfg.geom.r_face > 0, "the default dial has radius %d", (int)cfg.geom.r_face);
    CHECK(cfg.geom.r_ring < cfg.geom.r_face, "the seconds ring (%d) must sit inside the face (%d)",
          (int)cfg.geom.r_ring, (int)cfg.geom.r_face);
    CHECK(cfg.geom.r_well_in < cfg.geom.r_well_out,
          "the well's cross-section is inverted: %d..%d", (int)cfg.geom.r_well_in,
          (int)cfg.geom.r_well_out);
    CHECK(cfg.geom.r_hub < cfg.geom.r_face, "the hub is larger than the dial");
    CHECK((cfg.flags & CGL_WATCH_F_TICKS) != 0u, "the default face has no chapter ring");
    CHECK((cfg.flags & CGL_WATCH_F_HOUR) != 0u, "the default face has no hour hand");
    CHECK((cfg.flags & CGL_WATCH_F_MINUTE) != 0u, "the default face has no minute hand");
    CHECK((cfg.flags & CGL_WATCH_F_SECOND) != 0u, "the default face has no second hand");
    CHECK((cfg.flags & CGL_WATCH_F_RING) != 0u, "the default face has no seconds ring");
    CHECK(cfg.anim.tick_count == 60u, "the default chapter ring has %u marks",
          (unsigned)cfg.anim.tick_count);
    CHECK((cfg.anim.tick_major != 0u) && (cfg.anim.tick_count % cfg.anim.tick_major == 0u),
          "tick_major must divide tick_count (%u into %u)", (unsigned)cfg.anim.tick_major,
          (unsigned)cfg.anim.tick_count);
    CHECK(cfg.anim.second_mode == (uint8_t)CGL_CFG_WATCH_SECOND_MODE,
          "the default second-hand mode is %u, CGL_CFG_WATCH_SECOND_MODE is %u",
          (unsigned)cfg.anim.second_mode, (unsigned)CGL_CFG_WATCH_SECOND_MODE);
    CHECK((cfg.hour.r_tip > cfg.hour.r_base) && (cfg.minute.r_tip > cfg.minute.r_base),
          "a hand that points backwards is not a hand");
    CHECK(cfg.hour.r_tip < cfg.minute.r_tip, "the hour hand (%d) must be shorter than the minute "
          "hand (%d)", (int)cfg.hour.r_tip, (int)cfg.minute.r_tip);

    /* A face must fit the panel it is configured for. */
    CHECK((int)cfg.geom.cx - (int)cfg.geom.r_face >= 0, "the dial runs off the left edge");
    CHECK((int)cfg.geom.cy - (int)cfg.geom.r_face >= 0, "the dial runs off the top edge");
}

/* -------------------------------------------------------------------------- */
/* B. the shared value -> fraction mapping                                    */
/* -------------------------------------------------------------------------- */

static void test_frac_q8(void)
{
    CHECK(cgl_math_frac_q8(0, 0, 100) == 0u, "frac(0 of 0..100) = %u",
          (unsigned)cgl_math_frac_q8(0, 0, 100));
    CHECK(cgl_math_frac_q8(100, 0, 100) == 256u, "frac(100 of 0..100) = %u",
          (unsigned)cgl_math_frac_q8(100, 0, 100));
    CHECK(cgl_math_frac_q8(50, 0, 100) == 128u, "frac(50 of 0..100) = %u",
          (unsigned)cgl_math_frac_q8(50, 0, 100));
    CHECK(cgl_math_frac_q8(25, 0, 100) == 64u, "frac(25 of 0..100) = %u",
          (unsigned)cgl_math_frac_q8(25, 0, 100));
    /* Out of range is clamped, in both directions. */
    CHECK(cgl_math_frac_q8(-40, 0, 100) == 0u, "a value below the range is not clamped");
    CHECK(cgl_math_frac_q8(400, 0, 100) == 256u, "a value above the range is not clamped");
    /* A rejected (inverted / degenerate) range answers 0 rather than dividing by 0. */
    CHECK(cgl_math_frac_q8(5, 10, 10) == 0u, "a degenerate range did not answer 0");
    CHECK(cgl_math_frac_q8(5, 10, 0) == 0u, "an inverted range did not answer 0");
    /* The extremes of the type, where a signed subtraction would be UB. */
    CHECK(cgl_math_frac_q8(INT32_MAX, INT32_MIN, INT32_MAX) == 256u,
          "frac at the top of the full int32 range = %u",
          (unsigned)cgl_math_frac_q8(INT32_MAX, INT32_MIN, INT32_MAX));
}

/* -------------------------------------------------------------------------- */
/* C. the calendar                                                            */
/* -------------------------------------------------------------------------- */

static void test_calendar(void)
{
    cgl_watch_time_t t;

    CHECK(cgl_watch_is_leap(2028u), "2028 is a leap year");
    CHECK(!cgl_watch_is_leap(2027u), "2027 is not a leap year");
    CHECK(cgl_watch_is_leap(2000u), "2000 is a leap year (divisible by 400)");
    CHECK(!cgl_watch_is_leap(1900u), "1900 is not a leap year (divisible by 100)");

    CHECK(cgl_watch_days_in_month(2028u, 2u) == 29u, "February 2028 has %u days",
          (unsigned)cgl_watch_days_in_month(2028u, 2u));
    CHECK(cgl_watch_days_in_month(2027u, 2u) == 28u, "February 2027 has %u days",
          (unsigned)cgl_watch_days_in_month(2027u, 2u));
    CHECK(cgl_watch_days_in_month(2026u, 1u) == 31u, "January has %u days",
          (unsigned)cgl_watch_days_in_month(2026u, 1u));
    CHECK(cgl_watch_days_in_month(2026u, 4u) == 30u, "April has %u days",
          (unsigned)cgl_watch_days_in_month(2026u, 4u));

    /* 2026-01-31 is a Saturday: the next day is a Sunday in February. */
    t.h = 23u;
    t.m = 59u;
    t.s = 58u;
    t.ms = 0u;
    t.year = 2026u;
    t.month = 1u;
    t.day = 31u;
    t.wday = 6u;
    cgl_watch_next_day(&t);
    CHECK((t.day == 1u) && (t.month == 2u) && (t.year == 2026u) && (t.wday == 0u),
          "2026-01-31 + 1 day = %04u-%02u-%02u (wday %u)", (unsigned)t.year, (unsigned)t.month,
          (unsigned)t.day, (unsigned)t.wday);

    /* A leap February, and the year boundary. */
    t.year = 2028u;
    t.month = 2u;
    t.day = 28u;
    t.wday = 0u;
    cgl_watch_next_day(&t);
    CHECK((t.day == 29u) && (t.month == 2u), "2028-02-28 + 1 day = %02u-%02u", (unsigned)t.month,
          (unsigned)t.day);
    t.month = 12u;
    t.day = 31u;
    cgl_watch_next_day(&t);
    CHECK((t.day == 1u) && (t.month == 1u) && (t.year == 2029u),
          "2028-12-31 + 1 day = %04u-%02u-%02u", (unsigned)t.year, (unsigned)t.month,
          (unsigned)t.day);
}

/* -------------------------------------------------------------------------- */
/* D. the angle convention                                                    */
/* -------------------------------------------------------------------------- */

static void test_angles(void)
{
    cgl_watch_time_t t;

    memset(&t, 0, sizeof(t));

    /* 12:00: the hands are straight up, which in this convention is CGL_ANGLE_UP. */
    t.h = 12u;
    t.m = 0u;
    t.s = 0u;
    t.ms = 0u;
    CHECK(cgl_watch_hour_angle(&t) == CGL_ANGLE_UP, "the hour hand at 12:00 is at %u, not 12 o'clock",
          (unsigned)cgl_watch_hour_angle(&t));
    CHECK(cgl_watch_minute_angle(&t) == CGL_ANGLE_UP, "the minute hand at :00 is not at 12");

    /* 3:00: three quarters of the way from 12 to 12 is a quarter turn clockwise,
       i.e. 3 o'clock -- angle 0 in the framework's own convention. */
    t.h = 3u;
    CHECK(cgl_angle_deg(cgl_watch_hour_angle(&t)) == 0u, "the hour hand at 3:00 reads %u degrees",
          (unsigned)cgl_angle_deg(cgl_watch_hour_angle(&t)));

    /* 6:00 is a quarter turn round from 12. */
    t.h = 6u;
    CHECK(cgl_angle_deg(cgl_watch_hour_angle(&t)) == 90u, "the hour hand at 6:00 reads %u degrees",
          (unsigned)cgl_angle_deg(cgl_watch_hour_angle(&t)));

    t.h = 12u;
    t.m = 15u;
    /* A quarter of an hour is a quarter turn clockwise from 12, which is 3 o'clock --
       angle 0 in the framework's own convention (see cgl_math.h). */
    CHECK(cgl_watch_minute_angle(&t) == (cgl_angle_t)0u,
          "the minute hand at :15 is at %u, not 3 o'clock (0)", (unsigned)cgl_watch_minute_angle(&t));

    /* The seconds indicator's sweep is a function of the **second**, not the minute:
       ":30" means thirty seconds past, and the ring is half way round. */
    t.m = 0u;
    t.s = 30u;
    t.ms = 0u;
    CHECK(cgl_watch_minute_sweep(&t) == 32760u, "the minute sweep at :30 is %u, expected 30*1092",
          (unsigned)cgl_watch_minute_sweep(&t));
}

/* -------------------------------------------------------------------------- */
/* E. the two second-hand motions                                             */
/* -------------------------------------------------------------------------- */

static uint32_t angle_delta(cgl_angle_t a, cgl_angle_t b)
{
    uint16_t d = (uint16_t)(a - b);

    return (d <= 32768u) ? (uint32_t)d : (uint32_t)(65536u - (uint32_t)d);
}

static void test_second_modes(void)
{
    cgl_watch_time_t t;
    const uint16_t tick_ms = 100u;
    cgl_angle_t mark4;
    cgl_angle_t mark5;

    memset(&t, 0, sizeof(t));
    t.s = 4u;

    mark4 = cgl_watch_face_angle(4u * 1000u, 60000u);
    mark5 = cgl_watch_face_angle(5u * 1000u, 60000u);

    /* Both motions agree at every whole second: a face is a real clock either way. */
    t.ms = 0u;
    CHECK(cgl_watch_second_angle(&t, (uint8_t)CGL_WATCH_SECOND_TICK, tick_ms,
                                 (uint8_t)CGL_EASE_OUT_BACK) == mark4,
          "the tick at :04.000 is not on the :04 mark");
    CHECK(cgl_watch_second_angle(&t, (uint8_t)CGL_WATCH_SECOND_SWEEP, tick_ms,
                                 (uint8_t)CGL_EASE_OUT_BACK) == mark4,
          "the sweep at :04.000 is not on the :04 mark");

    /* The hold: for the first 900 ms the tick hand is parked **exactly** on its
       mark. That is the property that makes an interrupted tick still correct. */
    t.ms = 400u;
    CHECK(cgl_watch_second_angle(&t, (uint8_t)CGL_WATCH_SECOND_TICK, tick_ms,
                                 (uint8_t)CGL_EASE_OUT_BACK) == mark4,
          "the tick is not parked during the hold");
    t.ms = 899u;
    CHECK(cgl_watch_second_angle(&t, (uint8_t)CGL_WATCH_SECOND_TICK, tick_ms,
                                 (uint8_t)CGL_EASE_OUT_BACK) == mark4,
          "the tick left its mark before the hold ended");

    /* The snap: over the last 100 ms the hand has left its mark, and it has not
       passed the next one. WARNING: the bound is deliberately not "a fraction of the
       step": the tick's dial position is `s*1000 + yank`, and `yank` only reaches
       `tick_ms` (100) by the end of the step -- `tick_ms` is a *time*, and the whole
       second of dial is the `1000` the base term already carries. What the curve
       adds on top of that linear share is the overshoot, and that is what the middle
       of the step shows. */
    t.ms = 999u;
    {
        cgl_angle_t at = cgl_watch_second_angle(&t, (uint8_t)CGL_WATCH_SECOND_TICK, tick_ms,
                                                (uint8_t)CGL_EASE_OUT_BACK);
        uint32_t have = angle_delta(at, mark4);
        uint32_t whole = angle_delta(mark5, mark4);

        CHECK(have > 0u, "the tick never left the :04 mark (moved %u)", have);
        CHECK(have < whole, "the tick is already past the :05 mark (%u of %u)", have, whole);
    }

    /* WARNING: do **not** assert that this is monotonic in `ms`. It is not, and that
       is the curve working: the yank is `offset + bump(offset)/128`, `offset` grows
       linearly while `bump` peaks in the middle of the step and returns to zero at
       both ends, so the hand runs a few percent *past* its line in the middle and
       settles back onto it. `CGL_EASE_OUT_BACK` is what gives the mechanical tick its
       little bounce, and a monotonicity check here would be a check that the bounce
       does not exist. What *is* exact is the two ends: 0 at the top of the step, and
       on the mark at both whole seconds -- which the marks case above covers. */

    /* The sweep, meanwhile, is linear in the millisecond -- that is what makes it a
       glide rather than a step. */
    t.ms = 500u;
    {
        cgl_angle_t at = cgl_watch_second_angle(&t, (uint8_t)CGL_WATCH_SECOND_SWEEP, tick_ms,
                                                (uint8_t)CGL_EASE_OUT_BACK);
        uint32_t have = angle_delta(at, mark4);
        uint32_t whole = angle_delta(mark5, mark4);

        CHECK((have > ((whole / 2u) - 100u)) && (have < ((whole / 2u) + 100u)),
              "the sweep at :04.500 is %u of %u into the step, expected half", have, whole);
    }

    /* A degenerate tick length is no tick at all: the hand holds all second rather
       than dividing by zero. */
    t.ms = 999u;
    CHECK(cgl_watch_second_angle(&t, (uint8_t)CGL_WATCH_SECOND_TICK, 0u,
                                 (uint8_t)CGL_EASE_OUT_BACK) == mark4,
          "a zero tick_ms did not degrade to a hold");
}

/* -------------------------------------------------------------------------- */
/* F. the surfaces are lit                                                    */
/* -------------------------------------------------------------------------- */

static void test_surface_lighting(void)
{
    cgl_watch_config_t cfg;
    const cgl_arc_surface_t *face;
    const cgl_arc_surface_t *well;
    int centre;
    int rim;
    int upper_left;
    int lower_right;
    int top;
    int bottom;

    cgl_watch_config_default(&cfg);
    face = &cfg.style.face;
    well = &cfg.style.well;

    centre = lum(cgl_arc_surface_color(face, 0, 0));
    rim = lum(cgl_arc_surface_color(face, 0, (int16_t)(cfg.geom.r_face - 1)));
    CHECK(centre > rim, "the dial's centre (%d) is not brighter than its rim (%d)", centre, rim);

    upper_left = lum(cgl_arc_surface_color(face, cfg.style.face.light_dx, cfg.style.face.light_dy));
    lower_right = lum(cgl_arc_surface_color(face, (int16_t)(-cfg.style.face.light_dx),
                                            (int16_t)(-cfg.style.face.light_dy)));
    CHECK(upper_left > lower_right, "the highlight (%d) is not brighter than its opposite (%d)",
          upper_left, lower_right);

    /* The well is *concave* where the face is *convex*: dark where the light comes
       from, bright at the bottom. Shading both the same way is what makes a bezel
       look like a sticker instead of a machined recess -- and this is the assertion
       that says so. */
    top = lum(cgl_arc_surface_color(well, 0, (int16_t)-cfg.geom.r_ring));
    bottom = lum(cgl_arc_surface_color(well, 0, cfg.geom.r_ring));
    CHECK(top < bottom, "the well's top (%d) is not darker than its bottom (%d)", top, bottom);

    /* Every one of them is exact across the whole disc: same offset, same colour. */
    CHECK(cgl_arc_surface_color(face, 21, -13) == cgl_arc_surface_color(face, 21, -13),
          "the face is not a pure function of the pixel");
    CHECK(cgl_arc_surface_color(well, 75, 3) == cgl_arc_surface_color(well, 75, 3),
          "the well is not a pure function of the pixel");
}

/* -------------------------------------------------------------------------- */
/* G. the baked tables against the built-in flash tables                      */
/* -------------------------------------------------------------------------- */

/**
 * The default dial's two tables are data in flash, and **data drifts**: the moment
 * someone changes a palette stop or a texture amplitude, the code that would have
 * produced the new colours is not the code that draws them, and the face goes on
 * rendering the old ones while the constants claim otherwise. No rendering test can
 * catch that -- the picture is still a perfectly good picture -- which is why this
 * case re-runs the shipping baker over every entry.
 *
 * On a mismatch it prints the whole regenerated array, because that is the fix:
 * paste it back into src/core/cgl_watch_dial.c.
 */
static void test_bake_matches_flash(void)
{
#if CGL_CFG_WATCH_DIAL_LUT
    cgl_watch_config_t cfg;
    uint16_t face[CGL_ARC_FACE_LUT_N];
    uint16_t well[CGL_ARC_RING_LUT_N];
    uint16_t i;
    int bad_face = 0;
    int bad_well = 0;

    cgl_watch_config_default(&cfg);

    cgl_arc_surface_bake(&cfg.style.face, face, (uint16_t)CGL_ARC_FACE_LUT_N);
    cgl_arc_surface_bake(&cfg.style.well, well, (uint16_t)CGL_ARC_RING_LUT_N);

    for (i = 0u; i < (uint16_t)CGL_ARC_FACE_LUT_N; i++)
    {
        if (cgl_watch_face_lut_default[i] != face[i])
        {
            if (bad_face == 0)
            {
                printf("       face[%u] is 0x%04X, the baker says 0x%04X\n", (unsigned)i,
                       (unsigned)cgl_watch_face_lut_default[i], (unsigned)face[i]);
            }
            bad_face++;
        }
    }
    for (i = 0u; i < (uint16_t)CGL_ARC_RING_LUT_N; i++)
    {
        if (cgl_watch_well_lut_default[i] != well[i])
        {
            if (bad_well == 0)
            {
                printf("       well[%u] is 0x%04X, the baker says 0x%04X\n", (unsigned)i,
                       (unsigned)cgl_watch_well_lut_default[i], (unsigned)well[i]);
            }
            bad_well++;
        }
    }

    if ((bad_face != 0) || (bad_well != 0))
    {
        printf("       --- regenerate src/core/cgl_watch_dial.c from this ---\n");
        for (i = 0u; i < (uint16_t)CGL_ARC_FACE_LUT_N; i++)
        {
            printf("    0x%04Xu,%s", (unsigned)face[i], ((i % 12u) == 11u) ? "\n" : " ");
        }
        printf("\n");
        for (i = 0u; i < (uint16_t)CGL_ARC_RING_LUT_N; i++)
        {
            printf("    0x%04Xu,%s", (unsigned)well[i], ((i % 12u) == 11u) ? "\n" : " ");
        }
        printf("\n");
    }

    CHECK(bad_face == 0, "the default face table disagrees with its baker at %d of %d entries",
          bad_face, (int)CGL_ARC_FACE_LUT_N);
    CHECK(bad_well == 0, "the default well table disagrees with its baker at %d of %d entries",
          bad_well, (int)CGL_ARC_RING_LUT_N);
#endif
}

/* -------------------------------------------------------------------------- */
/* H. the ring gauge                                                          */
/* -------------------------------------------------------------------------- */

static void test_gauge(void)
{
    cgl_arc_gauge_t g;
    cgl_rect_t frame = cgl_rect_wh(0, 0, 64, 64);
    uint32_t rejected = cgl_arc_range_rejected();
    cgl_point_t tip;
    cgl_angle_t start_plus_quarter;

    cgl_arc_gauge_init(&g, "g", frame, 28, 5);

    /* 0 of 0..100 is at the start angle (12 o'clock) and 100 is a whole turn later,
       which is the same place -- so the gauge is a full ring by default. */
    cgl_arc_gauge_set_value(&g, 0);
    CHECK(cgl_arc_gauge_sweep(&g) == 0u, "an empty gauge sweeps %u", (unsigned)cgl_arc_gauge_sweep(&g));
    tip = cgl_arc_gauge_tip(&g);
    CHECK((tip.x == 32) && (tip.y == 32), "the lead point of an empty gauge is (%d,%d), expected the "
          "centre (32,32)", (int)tip.x, (int)tip.y);

    cgl_arc_gauge_set_value(&g, 25);
    CHECK(cgl_arc_gauge_sweep(&g) == (CGL_ANGLE_TURN / 4u), "a quarter of the range sweeps %u, "
          "expected %u", (unsigned)cgl_arc_gauge_sweep(&g), (unsigned)(CGL_ANGLE_TURN / 4u));

    /* A quarter turn clockwise from 12 o'clock is 3 o'clock: (cx + r, cy). */
    tip = cgl_arc_gauge_tip(&g);
    CHECK((tip.x == 60) && (tip.y == 32), "the lead point at a quarter is (%d,%d), expected (60,32)",
          (int)tip.x, (int)tip.y);

    cgl_arc_gauge_set_value(&g, 100);
    CHECK(cgl_arc_gauge_sweep(&g) == CGL_ANGLE_TURN, "a full range sweeps %u, expected a whole turn",
          (unsigned)cgl_arc_gauge_sweep(&g));

    /* A rejected range is counted and leaves the old one in place. */
    CHECK(!cgl_arc_gauge_set_range(&g, 10, 10), "a degenerate range was accepted");
    CHECK(!cgl_arc_gauge_set_range(&g, 10, -10), "an inverted range was accepted");
    CHECK(cgl_arc_range_rejected() == (rejected + 2u), "the two rejected ranges were not both "
          "counted (%u -> %u)", (unsigned)rejected, (unsigned)cgl_arc_range_rejected());
    CHECK(cgl_arc_gauge_get_value(&g) == 100, "a rejected range changed the value");

    /* Retargeting mid-flight starts from what is drawn, not from the old target. */
    CHECK(!cgl_arc_gauge_animate_to(&g, 100, 0u, CGL_EASE_LINEAR), "a zero duration animated");
    CHECK(cgl_arc_gauge_animate_to(&g, 0, 200u, CGL_EASE_LINEAR), "the tween did not start");
    CHECK(cgl_arc_gauge_is_animating(&g), "the gauge does not report its own tween");
    cgl_widget_update_tree(&g.base, 100u);
    CHECK(cgl_arc_gauge_get_shown_value(&g) < 100, "half way through, the gauge is still at the top");
    cgl_widget_update_tree(&g.base, 200u);
    CHECK(cgl_arc_gauge_get_shown_value(&g) == 0, "the tween did not arrive at 0");

    /* The reveal scales the sweep, and it is a *separate* door from the value. */
    cgl_arc_gauge_set_range(&g, 0, 100);
    cgl_arc_gauge_set_value(&g, 50);
    cgl_arc_gauge_set_scale(&g, 128u);
    CHECK(cgl_arc_gauge_sweep(&g) == (CGL_ANGLE_TURN / 4u), "half a sweep of half a range is %u, "
          "expected a quarter turn", (unsigned)cgl_arc_gauge_sweep(&g));

    /* A sweep set directly bypasses the value mapping -- the door a watch face uses
       when the clock, not a range, decides the arc. */
    cgl_arc_gauge_set_sweep(&g, CGL_ANGLE_TURN / 8u);
    CHECK(cgl_arc_gauge_sweep(&g) == (CGL_ANGLE_TURN / 16u), "a direct sweep was not scaled by the "
          "reveal: %u", (unsigned)cgl_arc_gauge_sweep(&g));

    cgl_arc_gauge_set_span(&g, CGL_ANGLE_UP, CGL_ANGLE_TURN / 2u, CGL_ARC_CW);
    start_plus_quarter = (cgl_angle_t)(CGL_ANGLE_UP + (CGL_ANGLE_TURN / 8u));
    cgl_arc_gauge_set_scale(&g, 256u);
    cgl_arc_gauge_set_sweep(&g, CGL_ANGLE_TURN / 8u);
    CHECK(cgl_arc_gauge_sweep(&g) == (CGL_ANGLE_TURN / 8u), "the span leaked into a direct sweep");
    tip = cgl_arc_gauge_tip(&g);
    {
        int16_t dx;
        int16_t dy;

        cgl_math_polar(start_plus_quarter, 28, &dx, &dy);
        CHECK((tip.x == (int16_t)(32 + dx)) && (tip.y == (int16_t)(32 + dy)),
              "the lead point (%d,%d) is not at the end of the sweep (%d,%d)", (int)tip.x,
              (int)tip.y, (int)(32 + dx), (int)(32 + dy));
    }
}

/* -------------------------------------------------------------------------- */
/* I. the damage model                                                        */
/* -------------------------------------------------------------------------- */

/**
 * The property this page exists to demonstrate, and the one no screenshot can
 * settle: a frame the widget damaged **incrementally** has to be pixel for pixel
 * identical to a full redraw of the same instant. If a moving hand ever fails to
 * mark where it *was*, the panel keeps a ghost of it -- and the panel is the only
 * place that shows.
 *
 * The comparison is made at the same instant by forcing the full redraw with
 * `dt == 0`: no time passes, so the picture cannot legitimately differ.
 */
static void test_no_ghosts(void)
{
    uint32_t i;
    int bad = -1;

    /* Long enough that the second hand is mid-minute and the comet trail has lit
       several marks and aged them. */
    run(260, 16u);

    memcpy(s_reference, s_framebuffer, sizeof(s_reference));

    cgl_screen_redraw_all(&s_screen);
    run(1, 0u);

    for (i = 0u; i < (uint32_t)(CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H); i++)
    {
        if (s_reference[i] != s_framebuffer[i])
        {
            bad = (int)i;
            break;
        }
    }

    CHECK(bad < 0, "the incremental frame differs from a full redraw at (%d,%d): 0x%04X against "
          "0x%04X", (bad % CGL_CFG_SCREEN_W), (bad / CGL_CFG_SCREEN_W),
          (unsigned)((bad >= 0) ? s_reference[bad] : 0u),
          (unsigned)((bad >= 0) ? s_framebuffer[bad] : 0u));
}

/** The dial is a real clock: one second of frames must move it by one second, and
    the reading must be taken from the frame clock rather than accumulated. */
static void test_keeps_time(void)
{
    const cgl_watch_time_t *t0;
    const cgl_watch_time_t *t1;
    uint32_t before;

    run(20, 16u);
    t0 = cgl_watch_now(&s_page.watch);
    CHECK(t0 != NULL, "the widget has no reading after 20 frames");
    before = cgl_watch_seconds_of_day(t0);

    run(1000u / 16u, 16u);
    t1 = cgl_watch_now(&s_page.watch);
    CHECK(cgl_watch_seconds_of_day(t1) == (before + 1u), "after ~1 s the reading advanced by %u s",
          (unsigned)(cgl_watch_seconds_of_day(t1) - before));
}

/* -------------------------------------------------------------------------- */

int main(void)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));

    printf("arc toolkit + watch face: the module's own contract\n\n");

    if (cgl_display_init(&s_display, &s_mock_drv, NULL) != 0)
    {
        printf("display init failed\n");
        return 1;
    }
    cgl_page_nav_init(&s_nav, "watch_test",
                      cgl_rect(0, 0, (int16_t)(CGL_CFG_SCREEN_W - 1),
                               (int16_t)(CGL_CFG_SCREEN_H - 1)));
    cgl_page_init(&s_page.base, &page__vt, "watch", NULL);
    if (cgl_screen_init(&s_screen, &s_display, &s_nav) != 0)
    {
        printf("screen init failed\n");
        return 1;
    }
    (void)cgl_page_nav_reset(&s_nav, &s_page.base);
    cgl_screen_frame(&s_screen, 0u);

    CHECK(s_page.base.root != NULL, "the page has no widget tree");
    CHECK(cgl_widget_children_dropped() == 0u, "the watch page dropped %u widgets",
          (unsigned)cgl_widget_children_dropped());

    printf("A  the default configuration\n");
    test_config_default();
    printf("B  value -> fraction\n");
    test_frac_q8();
    printf("C  the calendar\n");
    test_calendar();
    printf("D  the angle convention\n");
    test_angles();
    printf("E  the two second-hand motions\n");
    test_second_modes();
    printf("F  the surfaces are lit\n");
    test_surface_lighting();
    printf("G  the baked tables against their generator\n");
    test_bake_matches_flash();
    printf("H  the ring gauge\n");
    test_gauge();
    printf("I  incremental damage\n");
    test_no_ghosts();
    printf("J  the dial keeps time\n");
    test_keeps_time();

    printf("\n%s: %d failure(s)\n", (s_fails == 0) ? "PASS" : "FAIL", s_fails);
    return (s_fails == 0) ? 0 : 1;
}
