// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    clock_host_test.c
  * @brief   What the watch face page *means*, measured.
  *
  *  `clock_frames.c` writes pictures of the face; this file asserts the things a
  *  picture cannot: where the hands point, that the ring is a ring and not a
  *  decoration, that the entrance really assembles the face, that the printed date
  *  is a real calendar, and that the page keeps time while it is covered.
  *
  *  The claims, and why each one is worth a test
  *  --------------------------------------------
  *   A. **The time base.** `now = t0 + (clk->now_ms - t0_ms)`: a set time is exact
  *      after one frame, it advances by the frame time, and it survives the 32 bit
  *      millisecond rollover of the counter (the subtraction is unsigned for
  *      exactly that reason). A page that got this wrong would still *look* right
  *      for a minute and then be wrong forever.
  *   B. **The calendar.** Midnight, a month boundary, a leap year, and a year
  *      boundary, each from an anchored date -- the four branches of
  *      `clock__next_day()` that a 20 second test can actually reach.
  *   C. **The angle convention.** 0 = 3 o'clock, growing clockwise: the second hand
  *      at 15 s must put ink at +x and none at -x, and at 45 s the opposite. This
  *      is the one place a sign error is invisible in a single snapshot (a dial is
  *      symmetric) and obvious to a user (the clock runs backwards).
  *   D. **The seconds ring is a ring.** At 50 % of the minute the ring must be
  *      orange at 12 and at 6 o'clock and still the dark track at 9 -- i.e. the arc
  *      starts at 12 and sweeps clockwise, and it is a *stroke* (the centre of the
  *      dial stays empty).
  *   E. **The entrance draws the face on.** At reveal 0 the hands are at 12:00 and
  *      the chapter ring is empty; by reveal 256 every mark is there. That is the
  *      difference between "the animation plays" and "the animation is the face
  *      being assembled".
  *   F. **The trailing comet.** A mark the second hand has just passed is brighter
  *      than one it passed long ago -- measured as *colour*, because "there is a
  *      trail" is otherwise a claim about a picture.
  *   G. **The complications.** The digital time widget really moves its glyphs when
  *      the minute turns over (measured as the ink centroid during the roll versus
  *      after it), and the gauge's ring follows the seconds.
  *   H. **The covered page stops stepping but does not stop keeping time.** This
  *      is the page's answer to the demo's central claim: `frames` stops growing
  *      while another page is on top, and the *time* does not -- because the time is
  *      read from the global clock rather than accumulated by the page. A watch that
  *      lost 30 seconds every time you opened a menu would be a bug, and this is the
  *      test that says so.
  *
  *  Build & run
  *  -----------
  *      cmake -B build/host -DCYTGL_BUILD_TESTS=ON
  *      cmake --build build/host
  *      ctest --test-dir build/host -R clock_test --output-on-failure
  ******************************************************************************
  */
#include <stdio.h>
#include <math.h>
#include <string.h>

#include <cytgl/cgl_display.h>
#include <cytgl/cgl_screen.h>
#include <cytgl/cgl_page.h>
#include <cytgl/cgl_widget.h>
#include <cytgl/cgl_math.h>
#include <cytgl/cytgl_config.h>

#include "app_page_clock.h"
#include "app_page_clock_dial.h" /* the flash tables and their generators */

/* -------------------------------------------------------------------------- */
/* The page's palette, deliberately duplicated here                            */
/* -------------------------------------------------------------------------- */
/* A pixel test has to know the colours it is looking for, and duplicating them is
   the point: if the page's palette changes, these assertions are what tells the
   author "the picture you are about to change is the one these claims are about".
   WARNING: `COL_FACE` is deliberately **not** here any more. The dial's face is no
   longer one colour -- it is a radial gradient, a brushed texture and a specular
   highlight -- so there is no constant to duplicate. A test that wants a face pixel
   asks the page for it (`app_page_clock_face_color()`, which *is* the expression the
   rasterizer runs) and asserts the lighting instead; see test_face_lighting. */
#define COL_BG CGL_COLOR_BLACK
#define COL_TRACK CGL_RGB565(16, 20, 27)
#define COL_TICK CGL_RGB565(96, 104, 122)
#define COL_TICK_HOUR CGL_RGB565(214, 221, 232)
#define COL_HAND CGL_RGB565(240, 244, 250)
#define COL_ACCENT CGL_RGB565(255, 106, 0)
#define COL_TEXT CGL_RGB565(238, 242, 248)

/**
 * A pixel that is not the background the page says is there.
 *
 * WARNING: this used to be `!= COL_BG`, and it has to be a function of the *position*
 * now that the background is designed: a page whose background is a gradient over a
 * card makes every pixel "ink" under the old test, and every assertion built on it --
 * "the mark under the second hand is drawn at all", "the digital time drew something",
 * the ink centroid a digit roll is measured with -- would pass by itself. The
 * background is a pure function (`app_page_clock_bg_color()`), so this stays exact.
 */
#define IS_INK(x, y) (PX((x), (y)) != app_page_clock_bg_color((int16_t)(x), (int16_t)(y)))

/**
 * Is the mechanical tick compiled in? Section Q below is the tick's own test and it is
 * **deliberately silent in the sweep build** rather than absent from the file: the two
 * motions are one compile-time switch over one function (see CLOCK_SECOND_MODE in
 * app_page_clock.h), so a reader who flips it wants this suite to keep passing with the
 * other mode compiled in -- not to lose a translation unit.
 */
#define TICK_MODE (CLOCK_SECOND_MODE == CLOCK_SECOND_TICK)


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

/* -------------------------------------------------------------------------- */
/* Harness: a mock panel, a real screen, the real page                         */
/* -------------------------------------------------------------------------- */

static cgl_color_t s_framebuffer[CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H];
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
static app_page_clock_t s_clock;
static app_links_t s_links;
static cgl_page_ctx_t s_ctx;

/** A page that draws nothing, used only to cover the watch face. */
static cgl_page_t s_cover;

#define PX(x, y) s_framebuffer[((int32_t)(y) * (int32_t)CGL_CFG_SCREEN_W) + (int32_t)(x)]

/** Drive n frames at the firmware cadence. */
static void run(int n, uint32_t dt)
{
    int i;

    for (i = 0; i < n; i++)
    {
        cgl_screen_frame(&s_screen, dt);
    }
}

/**
 * Set the time, let the pending anchor land, and let the **minute pulse** decay.
 *
 * The settle is not cosmetic: a time change moves the minute, which fires the
 * 620 ms pulse, and while it runs the hands are blended toward the accent -- so a
 * test that samples a hand's exact colour right after `set_time()` is sampling the
 * pulse, not the hand. Settling costs 780 ms, so a caller that sets :15 may end up
 * checking :15.8, which is why the angle assertions below use a few pixels of slop.
 */
static void go_to(uint8_t h, uint8_t m, uint8_t s)
{
    app_page_clock_set_time(&s_clock, h, m, s);
    run(1, 0u);         /* apply the anchor (dt = 0: no time passes) */
    run(49, 16u);       /* one real frame plus the pulse's 620 ms */
}

/** Do the same for a date, without disturbing the time of day. */
static void date_to(uint16_t y, uint8_t mo, uint8_t d, uint8_t wd)
{
    app_page_clock_set_date(&s_clock, y, mo, d, wd);
    run(1, 0u);
    run(1, 16u);
}

static const clock_time_t *now(void)
{
    return app_page_clock_now(&s_clock);
}

/* -------------------------------------------------------------------------- */
/* A. the time base                                                           */
/* -------------------------------------------------------------------------- */

static void test_time_base(void)
{
    const clock_time_t *t;

    go_to(10u, 9u, 36u);
    t = now();
    CHECK((t->h == 10u) && (t->m == 9u) && (t->s == 36u),
          "set_time(10:09:36) read back as %02u:%02u:%02u", (unsigned)t->h, (unsigned)t->m,
          (unsigned)t->s);

    /* One second of frames must move it by one second -- not by a step count. */
    run(1000u / 16u, 16u);
    t = now();
    CHECK((t->h == 10u) && (t->m == 9u) && (t->s == 37u),
          "after ~1 s the face shows %02u:%02u:%02u, expected 10:09:37", (unsigned)t->h,
          (unsigned)t->m, (unsigned)t->s);

    /* A minute of frames: the minute rolls, the hour does not. */
    run(60u * 1000u / 16u, 16u);
    t = now();
    CHECK((t->m == 10u) && (t->h == 10u), "after ~61 s the face shows %02u:%02u", (unsigned)t->h,
          (unsigned)t->m);

    /* Wrap-safety: anchor at 23:59:58 and cross midnight. The unsigned subtraction
       in clock__sync is what makes this work when now_ms wraps, and the same
       subtraction is what carries the date. */
    go_to(23u, 59u, 58u);
    run(3u * 1000u / 16u, 16u);
    t = now();
    CHECK((t->h == 0u) && (t->m == 0u) && (t->s == 1u), "midnight crossing gave %02u:%02u:%02u",
          (unsigned)t->h, (unsigned)t->m, (unsigned)t->s);
    CHECK(t->day == 15u, "the date did not advance past midnight (day = %u)", (unsigned)t->day);
    CHECK(t->wday == 4u, "2026-01-15 is a Thursday, got wday = %u", (unsigned)t->wday);
}

static void test_seconds_of_day(void)
{
    go_to(1u, 2u, 3u);
    CHECK(app_page_clock_seconds_of_day(&s_clock) == 3723u, "seconds_of_day(01:02:03) = %u",
          (unsigned)app_page_clock_seconds_of_day(&s_clock));
}

/* -------------------------------------------------------------------------- */
/* B. the calendar                                                            */
/* -------------------------------------------------------------------------- */

static void test_calendar(void)
{
    const clock_time_t *t;

    /* A month with 31 days: 2026-01-31 -> 2026-02-01 (and Saturday -> Sunday). */
    date_to(2026u, 1u, 31u, 6u);
    go_to(23u, 59u, 58u);
    run(3u * 1000u / 16u, 16u);
    t = now();
    CHECK((t->day == 1u) && (t->month == 2u) && (t->year == 2026u),
          "2026-01-31 + midnight = %04u-%02u-%02u, expected 2026-02-01", (unsigned)t->year,
          (unsigned)t->month, (unsigned)t->day);
    CHECK(t->wday == 0u, "2026-02-01 is a Sunday, got wday = %u", (unsigned)t->wday);

    /* A leap year: 2028-02-28 -> 2028-02-29 (not 03-01). */
    date_to(2028u, 2u, 28u, 0u);
    go_to(23u, 59u, 58u);
    run(3u * 1000u / 16u, 16u);
    t = now();
    CHECK((t->day == 29u) && (t->month == 2u),
          "2028-02-28 + midnight = %02u-%02u, expected 02-29 (2028 is a leap year)",
          (unsigned)t->month, (unsigned)t->day);

    /* ...and the same date in a common year is March. */
    date_to(2027u, 2u, 28u, 6u);
    go_to(23u, 59u, 58u);
    run(3u * 1000u / 16u, 16u);
    t = now();
    CHECK((t->day == 1u) && (t->month == 3u) && (t->year == 2027u),
          "2027-02-28 + midnight = %04u-%02u-%02u, expected 2027-03-01", (unsigned)t->year,
          (unsigned)t->month, (unsigned)t->day);

    /* A year boundary. */
    date_to(2028u, 12u, 31u, 6u);
    go_to(23u, 59u, 58u);
    run(3u * 1000u / 16u, 16u);
    t = now();
    CHECK((t->day == 1u) && (t->month == 1u) && (t->year == 2029u),
          "2028-12-31 + midnight = %04u-%02u-%02u, expected 2029-01-01", (unsigned)t->year,
          (unsigned)t->month, (unsigned)t->day);

    /* Out of range inputs are clamped, not wrapped: "0 FEB" must be impossible. */
    date_to(2026u, 0u, 0u, 0u);
    t = now();
    CHECK((t->month == 1u) && (t->day == 1u), "month 0 / day 0 clamped to %02u-%02u",
          (unsigned)t->month, (unsigned)t->day);
}

/* -------------------------------------------------------------------------- */
/* C/D. the dial: the angle convention and the ring                           */
/* -------------------------------------------------------------------------- */

/** Is there a pixel of exactly `want` within `slop` of (x, y)? */
static bool near_exact(int16_t x, int16_t y, cgl_color_t want, int16_t slop)
{
    int16_t dx;
    int16_t dy;

    for (dy = (int16_t)-slop; dy <= slop; dy++)
    {
        for (dx = (int16_t)-slop; dx <= slop; dx++)
        {
            int16_t sx = (int16_t)(x + dx);
            int16_t sy = (int16_t)(y + dy);

            if ((sx < 0) || (sx >= (int16_t)CGL_CFG_SCREEN_W) || (sy < 0) ||
                (sy >= (int16_t)CGL_CFG_SCREEN_H))
            {
                continue;
            }
            if (PX(sx, sy) == want)
            {
                return true;
            }
        }
    }
    return false;
}

/** The dial's own coordinates, restated so the test fails loudly if they move. */
#define CX CLOCK_CX
#define CY CLOCK_CY

/* --- what the page says a pixel is -----------------------------------------
 *
 * Every surface of this dial is a pure function, and the crystal cover is a blend on
 * top of *all* of them (see app_page_clock_glass_over). So a test that wants to say
 * "this pixel of the panel is the dial, and nothing is drawn on it" has to compose
 * the two the same way the page does -- which is the point of both functions being
 * public: the alternative is asserting "roughly this colour", and that would pass for
 * a page with the lighting taken out.
 * -------------------------------------------------------------------------- */

/** The dial's background at this offset: the face, under the crystal. */
static cgl_color_t dial_background(int16_t dx, int16_t dy)
{
    return app_page_clock_glass_over(app_page_clock_face_color(dx, dy), dx, dy);
}

/** The outer ring's surface at this offset, under the crystal. */
static cgl_color_t ring_background(int16_t dx, int16_t dy)
{
    return app_page_clock_glass_over(app_page_clock_ring_color(dx, dy), dx, dy);
}

/**
 * Orange ink, seen through the crystal or not. The accent is (255, 106, 0); the cover
 * lifts it to about (232, 214, 91) at its strongest -- in RGB565 that is red 29, blue
 * **11** -- which is still red-dominant and blue-free by a wide margin relative to its
 * own red, while every other colour on the dial (the face, the well, the marks, the
 * hands, the bezel) is blue-dominant in every state the cover can leave it in.
 *
 * WARNING: the blue test is `blue * 2 < red`, **not** `blue <= 10`. The absolute form is
 * what this started as, and it is one level too strict for its own documented premise:
 * the fully-covered accent under the crystal measures blue 11, so a pixel of the second
 * hand's tip that the cover happens to catch was "not orange" and `test_second_hand_angle`
 * failed at :15, :30 and :45 while the hand was drawn perfectly well. A test predicate
 * has to describe the colour, and "red is more than twice the blue" is the description.
 */
static bool is_accent_ink(cgl_color_t c)
{
    unsigned r = (unsigned)((c >> 11) & 0x1Fu);
    unsigned b = (unsigned)(c & 0x1Fu);

    return (r >= 20u) && ((b * 2u) < r);
}

/** Is there accent ink within `slop` of (x, y)? */
static bool near_accent(int16_t x, int16_t y, int16_t slop)
{
    int16_t dx;
    int16_t dy;

    for (dy = (int16_t)-slop; dy <= slop; dy++)
    {
        for (dx = (int16_t)-slop; dx <= slop; dx++)
        {
            int16_t sx = (int16_t)(x + dx);
            int16_t sy = (int16_t)(y + dy);

            if ((sx < 0) || (sx >= (int16_t)CGL_CFG_SCREEN_W) || (sy < 0) ||
                (sy >= (int16_t)CGL_CFG_SCREEN_H))
            {
                continue;
            }
            if (is_accent_ink(PX(sx, sy)))
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * Is there a pixel within `slop` of (x, y) that is `want` **as the crystal cover leaves
 * it**? This is `near_exact()` for anything drawn under the glass.
 */
static bool near_glass(int16_t x, int16_t y, cgl_color_t want, int16_t slop)
{
    int16_t dx;
    int16_t dy;

    for (dy = (int16_t)-slop; dy <= slop; dy++)
    {
        for (dx = (int16_t)-slop; dx <= slop; dx++)
        {
            int16_t sx = (int16_t)(x + dx);
            int16_t sy = (int16_t)(y + dy);

            if ((sx < 0) || (sx >= (int16_t)CGL_CFG_SCREEN_W) || (sy < 0) ||
                (sy >= (int16_t)CGL_CFG_SCREEN_H))
            {
                continue;
            }
            if (PX(sx, sy) ==
                app_page_clock_glass_over(want, (int16_t)(sx - CX), (int16_t)(sy - CY)))
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * Where the second hand's ink actually reaches, as a **bearing** (0 = 12 o'clock,
 * clockwise), measured from the panel rather than assumed.
 *
 * The hand's tip is found as the farthest accent pixel from the dial centre, which is the
 * blade's own end -- and *not* "whatever pixel the angle arithmetic names", because those
 * two are not the same pixel. The geometric tip lands on the stroke's antialiased end cap
 * (the blade is 3 px wide and the cap is rounded), so the pixel at the exact geometric tip
 * is a partial blend of the accent and the dial, and asserting on it is asserting on a
 * sub-pixel rounding. That is what this replaced: three of these checks failed with the
 * tip drawn perfectly well, one pixel of end-cap blend away from the named point.
 *
 * WARNING: the radius is limited to the blade (`CLOCK_R_SECOND + 4`) so that the ring's
 * lead dot -- which sits at `CLOCK_R_RING`, further out and is also accent ink -- cannot
 * win the "farthest" race. The calipers (steps of 15 units of angle) are 0.08 degrees, far
 * below anything a hand can be wrong by and still look like a hand.
 */
static int hand_bearing_deg(void)
{
    int step;
    int best_deg = -1;
    int32_t best_d2 = 0;
    int32_t lim2 = ((int32_t)CLOCK_R_SECOND - 2) * ((int32_t)CLOCK_R_SECOND - 2);

    for (step = 0; step < 65536; step += 15)
    {
        int16_t dx;
        int16_t dy;
        int r;
        int16_t x;
        int16_t y;
        int32_t d2;

        cgl_math_polar((cgl_angle_t)step, (int16_t)CLOCK_R_SECOND, &dx, &dy);
        x = (int16_t)(CX + dx);
        y = (int16_t)(CY + dy);
        if ((x < 0) || (x >= 176) || (y < 0) || (y >= (int16_t)CGL_CFG_SCREEN_H))
        {
            continue;
        }
        /* Only the blade: an accent pixel closer to the centre than the counterweight
           would be the tail, and one outside `lim2` is the ring. Both radial neighbours
           are tried, so the calipers do not care whether the sampling circle falls just
           inside or just outside the blade. */
        for (r = -2; r <= 2; r++)
        {
            int16_t px = (int16_t)(x + ((step < 32768) ? r : -r));
            int16_t py = (int16_t)(y + ((step < 32768) ? -r : r));
            cgl_color_t c = PX(px, py);

            if (!is_accent_ink(c))
            {
                continue;
            }
            /* ...and it has to be *solid* accent, not the one-pixel antialiased edge of
               the ring's lead dot or of the blade's own side: a half-covered pixel measures
               its colour from a blend, and would otherwise win "the farthest accent pixel"
               from a radius further out than the blade reaches. Red at 29 of 31 is the
               accent's own core ((255,106,0) quantises to 31, and through the glass it measures 29)
               surface of this dial stays that red -- through the glass it lands at 29. */
            if (((c >> 11) & 0x1Fu) < 26u)
            {
                continue;
            }
            d2 = ((int32_t)(px - CX) * (px - CX)) + ((int32_t)(py - CY) * (py - CY));
            if ((d2 > best_d2) && (d2 <= lim2))
            {
                best_d2 = d2;
                /* The bearing of the pixel from the dial centre, 0 = 12 o'clock, growing
                   clockwise -- the dial's own convention (`CGL_ANGLE_UP` is 12 o'clock) and
                   the one the expected reading is stated in. Taken from the pixel's own
                   position rather than from the polar step that found it: the radial
                   neighbours below mean the pixel is *near* the sampling circle, not on it,
                   and 2 px of radius is half a degree here.
                   WARNING: screen y grows *down*, so the bearing is
                   `atan2(dx, -dy)` -- the first version of this line was `90 - atan2(dy,
                   dx)`, which is the same angle mirrored, and it read :30 as 0 degrees. */
                best_deg = (int)((180.0 / 3.14159265358979) *
                                 atan2((double)(px - CX), (double)(CY - py)));
                if (best_deg < 0)
                {
                    best_deg += 360;
                }
            }
        }
    }
    return best_deg;
}

static void test_second_hand_angle(void)
{
    /* Three poses a quarter turn apart. In each: the hand points where the time says it
       does, and the opposite radius is free of accent ink -- which is the "the clock runs
       clockwise" half of the claim. */
    static const uint8_t seconds[3] = { 15u, 30u, 45u };
    int k;

    for (k = 0; k < 3; k++)
    {
        int16_t dx;
        int16_t dy;
        int want;
        int got;

        go_to(12u, 0u, seconds[k]);
        /* The same "where does the time say" arithmetic as `second_tip_pixel()`, in
           degrees, so the comparison is a bearing against a bearing. */
        {
            const clock_time_t *t = now();
            uint32_t num = ((uint32_t)t->s * CGL_CLOCK_MS_PER_S) + (uint32_t)t->ms;

            want = (int)((num * 360u) / 60000u);
        }
        got = hand_bearing_deg();
        CHECK(got >= 0, "at :%u there is no second-hand ink on the dial at all",
              (unsigned)seconds[k]);
        CHECK((got >= (want - 9)) && (got <= (want + 9)),
              "at :%u the second hand points %d degrees but the time says %d",
              (unsigned)seconds[k], got, want);

        /* The opposite direction, sampled at a radius **inside** the ring: further out
           this check would find the ring's own start cap at 12 o'clock, which is accent
           ink for an entirely good reason. */
        cgl_math_polar((cgl_angle_t)(CGL_ANGLE_UP +
                                     ((uint32_t)seconds[k] * CGL_ANGLE_TURN) / 60u +
                                     CGL_ANGLE_HALF),
                       (int16_t)(CLOCK_R_HOUR + 6), &dx, &dy);
        CHECK(!near_accent((int16_t)(CX + dx), (int16_t)(CY + dy), 3),
              "at :%u there is accent ink on the opposite radius: the sweep runs anticlockwise",
              (unsigned)seconds[k]);
    }

    /* The counterweight: at :30 the hand points at 6 o'clock, so the tail is at 12. */
    go_to(12u, 0u, 30u);
    CHECK(near_glass(CX, (int16_t)(CY - CLOCK_R_TAIL), COL_ACCENT, 3),
          "at :30 the counterweight tail is not at 12 o'clock");
}

static void test_ring_is_a_ring(void)
{
    int16_t dx;
    int16_t dy;

    go_to(12u, 0u, 30u);

    /* The stroke itself: orange at the start of the sweep (12 o'clock) and, at a
       point well inside the sweep but away from its leading cap, still orange --
       through the crystal, which is a blend over the arc and not a colour. */
    CHECK(PX(CX, (int16_t)(CY - CLOCK_R_RING)) ==
              app_page_clock_glass_over(COL_ACCENT, 0, (int16_t)-CLOCK_R_RING),
          "the ring is not drawn at 12 o'clock (got %04X)",
          (unsigned)PX(CX, (int16_t)(CY - CLOCK_R_RING)));
    cgl_math_polar((cgl_angle_t)(CGL_ANGLE_UP + CGL_ANGLE_QUARTER + (CGL_ANGLE_QUARTER / 2)),
                   (int16_t)CLOCK_R_RING, &dx, &dy); /* 4:30 -- 37.5 % round */
    CHECK(PX((int16_t)(CX + dx), (int16_t)(CY + dy)) ==
              app_page_clock_glass_over(COL_ACCENT, dx, dy),
          "the ring is not filled at 4:30 after 30 s (got %04X)",
          (unsigned)PX((int16_t)(CX + dx), (int16_t)(CY + dy)));
    /* ...and at 9 o'clock -- which the sweep has not reached -- the **outer ring's
       own surface**: the well's cross-section at the hour that is neither top nor
       bottom, so the tilt contributes nothing there and this is the lit crown the
       seconds arc runs along. There is no flat track between the two any more: the
       ring *is* the track (see clock__well_px). */
    CHECK(PX((int16_t)(CX - CLOCK_R_RING), CY) == ring_background((int16_t)-CLOCK_R_RING, 0),
          "the outer ring is not its own surface at 9 o'clock (got %04X, ring is %04X)",
          (unsigned)PX((int16_t)(CX - CLOCK_R_RING), CY),
          (unsigned)ring_background((int16_t)-CLOCK_R_RING, 0));
    CHECK(!near_accent((int16_t)(CX - CLOCK_R_RING), CY, 1),
          "the ring is already filled at 9 o'clock after 30 s");

    /* It is a ring, not a disc: the middle of the dial is the face, and the hands
       are the only ink near the centre. The face is a function now, so the expected
       pixel is the page's own answer for that offset rather than a constant --
       test_face_lighting is where the *shape* of that function is asserted, and
       test_crystal_cover is where the cover over it is. */
    CHECK(PX((int16_t)(CX + 30), (int16_t)(CY + 30)) == dial_background(30, 30),
          "the dial's lower right quadrant is not empty face (got %04X, face is %04X)",
          (unsigned)PX((int16_t)(CX + 30), (int16_t)(CY + 30)),
          (unsigned)dial_background(30, 30));
}

/* -------------------------------------------------------------------------- */
/* E. the entrance                                                            */
/* -------------------------------------------------------------------------- */

static void test_reveal(void)
{
    /* Restart the reveal the way a user does: a short press. (A `set_time()` does not
       restart it -- the entrance is tied to arriving at the page, not to the time.)
       The event is posted and dispatched at the next frame boundary, and with dt = 0
       that frame leaves the reveal at exactly 0. */
    {
        cgl_event_t ev;

        memset(&ev, 0, sizeof(ev));
        ev.kind = (uint8_t)CGL_EV_KEY_UP;
        CHECK(cgl_screen_post_event(&s_screen, &ev), "the key event was dropped");
    }
    app_page_clock_set_time(&s_clock, 3u, 0u, 20u);
    run(1, 0u);

    CHECK(app_page_clock_reveal(&s_clock) == 0u, "the reveal did not restart at 0 (got %u)",
          (unsigned)app_page_clock_reveal(&s_clock));

    /* At reveal 0 the minute hand points up (12:00) even though the time is 03:20,
       and the chapter ring is empty. */
    CHECK(near_glass(CX, (int16_t)(CY - 40), COL_HAND, 3) ||
              near_glass(CX, (int16_t)(CY - 40), COL_ACCENT, 3),
          "at reveal 0 the minute hand is not at 12 o'clock");
    CHECK(!near_exact((int16_t)(CX + 40), CY, COL_HAND, 3),
          "at reveal 0 there is hand ink at 3 o'clock: the hands did not start at 12");
    CHECK(PX(CX, (int16_t)(CY + CLOCK_R_TICK_OUT)) ==
              dial_background(0, (int16_t)CLOCK_R_TICK_OUT),
          "a chapter mark is already drawn at reveal 0 (got %04X)",
          (unsigned)PX(CX, (int16_t)(CY + CLOCK_R_TICK_OUT)));

    /* Let it finish: 760 ms, with frames to spare. Then let the pulse from the
       set_time() above decay as well (another 620 ms). */
    run(90, 16u);
    CHECK(app_page_clock_reveal(&s_clock) == 256u, "the reveal did not finish (got %u)",
          (unsigned)app_page_clock_reveal(&s_clock));

    /* At 03:20:21 the second hand is at 4 o'clock, so the 12 o'clock radius is free
       for the minute hand and 3 o'clock for the hour hand. */
    CHECK((now()->s >= 20u) && (now()->s <= 23u), "the settle moved the second too far (%u)",
          (unsigned)now()->s);
    CHECK(near_glass(CX, (int16_t)(CY - (CLOCK_R_MINUTE - 4)), COL_HAND, 2),
          "at 03:20 the minute hand is not at 12 o'clock");
    CHECK(near_glass((int16_t)(CX + (CLOCK_R_HOUR - 3)), CY, COL_HAND, 2) ||
              near_glass((int16_t)(CX + (CLOCK_R_HOUR - 6)), CY, COL_HAND, 2),
          "at 03:20 the hour hand is not at 3 o'clock");
    /* The 6 o'clock chapter mark is there now -- whatever its exact colour, because
       the comet trail may still be glowing on it. */
    CHECK(IS_INK(CX, (int16_t)(CY + CLOCK_R_TICK_OUT)) &&
              (PX(CX, (int16_t)(CY + CLOCK_R_TICK_OUT)) !=
               dial_background(0, (int16_t)CLOCK_R_TICK_OUT)),
          "the 6 o'clock hour mark is missing at full reveal (got %04X)",
          (unsigned)PX(CX, (int16_t)(CY + CLOCK_R_TICK_OUT)));
}

/* -------------------------------------------------------------------------- */
/* F. the comet trail                                                         */
/* -------------------------------------------------------------------------- */

/** The screen pixel of a chapter mark, straight from the page's own polar helper. */
static void mark_pixel(uint8_t index, int16_t *x, int16_t *y)
{
    cgl_angle_t a = (cgl_angle_t)(CGL_ANGLE_UP + ((uint32_t)index * CGL_ANGLE_TURN) / 60u);
    int16_t dx;
    int16_t dy;

    cgl_math_polar(a, (int16_t)CLOCK_R_TICK_OUT, &dx, &dy);
    *x = (int16_t)(CX + dx);
    *y = (int16_t)(CY + dy);
}

static void test_trail(void)
{
    uint8_t s;
    int16_t hx;
    int16_t hy;
    int16_t ox;
    int16_t oy;
    cgl_color_t hot;
    cgl_color_t old;

    /* Twelve seconds in: the mark under the hand has just been lit, the one the hand
       passed five seconds ago is long dark. */
    go_to(0u, 0u, 0u);
    run(12u * 1000u / 16u, 16u);
    s = now()->s;
    CHECK(s >= 12u, "not enough seconds elapsed to test the trail (%u)", (unsigned)s);

    mark_pixel(s, &hx, &hy);
    mark_pixel((uint8_t)((s + 30u) % 60u), &ox, &oy); /* half a turn away: untouched */

    hot = PX(hx, hy);
    old = PX(ox, oy);

    CHECK(IS_INK(hx, hy), "the mark under the second hand is not drawn at all (%04X)",
          (unsigned)hot);
    CHECK(hot != (((s % 5u == 0u) ? COL_TICK_HOUR : COL_TICK)),
          "the mark under the second hand has no comet glow (%04X)", (unsigned)hot);
    /* Half a turn away, the trail is long gone: the mark is exactly its own colour --
       *through the crystal*, which lies over the marks too. That is what says the glow
       is a blend on the mark and not a repaint of it. */
    CHECK(old == app_page_clock_glass_over(((((s + 30u) % 60u) % 5u == 0u) ? COL_TICK_HOUR
                                                                          : COL_TICK),
                                           (int16_t)(ox - CX), (int16_t)(oy - CY)),
          "a mark half a turn away is not the plain mark colour (%04X, expected %04X)",
          (unsigned)old,
          (unsigned)app_page_clock_glass_over(((((s + 30u) % 60u) % 5u == 0u) ? COL_TICK_HOUR
                                                                              : COL_TICK),
                                              (int16_t)(ox - CX), (int16_t)(oy - CY)));
}

/* -------------------------------------------------------------------------- */
/* G. the complications                                                       */
/* -------------------------------------------------------------------------- */

/** Mean y of the ink inside a rectangle (used to watch the digit roll move). */
static int16_t ink_centroid_y(cgl_rect_t r)
{
    int32_t sum = 0;
    int32_t n = 0;
    int16_t x;
    int16_t y;

    for (y = r.y0; y <= r.y1; y++)
    {
        for (x = r.x0; x <= r.x1; x++)
        {
            if (IS_INK(x, y))
            {
                sum += y;
                n++;
            }
        }
    }
    return (n == 0) ? -1 : (int16_t)(sum / n);
}

/** Copy the pixels of a rectangle out of the framebuffer. */
static void grab(cgl_rect_t r, cgl_color_t *dst)
{
    int16_t x;
    int16_t y;
    int32_t at = 0;

    for (y = r.y0; y <= r.y1; y++)
    {
        for (x = r.x0; x <= r.x1; x++)
        {
            dst[at] = PX(x, y);
            at++;
        }
    }
}

static void test_digital_roll(void)
{
    cgl_rect_t digits = cgl_rect_wh((int16_t)(CLOCK_COL_CX - 40), (int16_t)CLOCK_TIME_Y, 80, 24);
    cgl_color_t steady[80 * 24];
    cgl_color_t mid[80 * 24];
    cgl_color_t after[80 * 24];
    int i;
    int differences;

    /* What "10:10" looks like when it is simply set, with no roll at all -- the
       reference the roll has to land on. */
    go_to(10u, 10u, 5u);
    grab(digits, steady);

    /* Now the real thing: 10:09:59 rolled over the minute. */
    go_to(10u, 9u, 59u);
    CHECK(ink_centroid_y(digits) > 0, "the digital time drew nothing");

    for (i = 0; i < 200; i++)
    {
        cgl_screen_frame(&s_screen, 16u);
        if (now()->m == 10u)
        {
            break;
        }
    }
    CHECK(now()->m == 10u, "the minute never turned over");

    /* Three frames (48 ms) into a 300 ms roll: caught in flight. */
    run(3, 16u);
    grab(digits, mid);
    differences = 0;
    for (i = 0; i < (int)(80 * 24); i++)
    {
        if (mid[i] != steady[i])
        {
            differences++;
        }
    }
    CHECK(differences > 40, "the digits did not move while the minute rolled (%d pixels differ)",
          differences);

    /* Let it finish, then compare with the reference: the roll has to land exactly on
       the glyphs a plain set_time() produces, or the digits end up displaced by
       whatever the tween was interrupted at. */
    run(30, 16u);
    grab(digits, after);
    differences = 0;
    for (i = 0; i < (int)(80 * 24); i++)
    {
        if (after[i] != steady[i])
        {
            differences++;
        }
    }
    CHECK(differences == 0, "after the roll the digits are displaced (%d pixels differ)",
          differences);
}

static void test_gauge(void)
{
    cgl_rect_t gauge = cgl_rect_wh((int16_t)(CLOCK_COL_CX - 32), (int16_t)(CLOCK_GAUGE_CY - 32),
                                   64, 64);
    int16_t cx = (int16_t)(gauge.x0 + 32);
    int16_t cy = (int16_t)(gauge.y0 + 32);

    /* A tenth of the way round: 3 o'clock on the gauge ring is still the track. (go_to
       settles the minute pulse, which otherwise blends the whole track toward the
       accent -- see go_to.) */
    go_to(5u, 0u, 6u);
    run(20, 16u);
    CHECK((now()->s < 15u), "the settle pushed the gauge past a quarter minute (%u)",
          (unsigned)now()->s);
    CHECK(PX((int16_t)(cx + CLOCK_GAUGE_R), cy) == COL_TRACK,
          "the gauge ring is filled at 3 o'clock after 6 s (got %04X)",
          (unsigned)PX((int16_t)(cx + CLOCK_GAUGE_R), cy));

    /* Two thirds of the way round: 3 o'clock is inside the sweep, 12 is the start. */
    go_to(5u, 0u, 40u);
    CHECK((now()->s >= 40u) && (now()->s <= 45u), "the settle moved the gauge too far (%u)",
          (unsigned)now()->s);
    CHECK(PX((int16_t)(cx + CLOCK_GAUGE_R), cy) == COL_ACCENT,
          "the gauge ring is not filled at 3 o'clock after 40 s (got %04X)",
          (unsigned)PX((int16_t)(cx + CLOCK_GAUGE_R), cy));
    CHECK(PX(cx, (int16_t)(cy - CLOCK_GAUGE_R)) == COL_ACCENT,
          "the gauge ring does not start at 12 o'clock");
    /* The value inside is drawn: the centre of the gauge is ink, the ring's own
       middle radius is not. */
    CHECK(ink_centroid_y(gauge) > 0, "the gauge drew no value");
}

/* -------------------------------------------------------------------------- */
/* H. covered: no frames, but time still passes                               */
/* -------------------------------------------------------------------------- */

static void test_covered(void)
{
    uint32_t frames_before;
    uint32_t steps_before;
    uint32_t sod_before;
    uint32_t sod_after;

    go_to(1u, 0u, 0u);
    sod_before = app_page_clock_seconds_of_day(&s_clock);
    frames_before = app_page_clock_frames(&s_clock);
    steps_before = app_page_clock_steps(&s_clock);

    /* Cover the face with a page that draws nothing, and let five seconds pass. */
    (void)cgl_page_nav_push(&s_nav, &s_cover);
    run(5u * 1000u / 16u, 16u);
    CHECK(app_page_clock_frames(&s_clock) == frames_before,
          "the covered clock page received %u frames",
          (unsigned)(app_page_clock_frames(&s_clock) - frames_before));
    CHECK(app_page_clock_steps(&s_clock) == steps_before,
          "the covered clock page received %u steps",
          (unsigned)(app_page_clock_steps(&s_clock) - steps_before));

    (void)cgl_page_nav_back(&s_nav);
    run(2, 16u);

    sod_after = app_page_clock_seconds_of_day(&s_clock);
    CHECK(sod_after >= (sod_before + 5u),
          "the face lost time while covered: %u s -> %u s", (unsigned)sod_before,
          (unsigned)sod_after);
    /* ...and it is on top again, so frames resume. */
    CHECK(app_page_clock_frames(&s_clock) > frames_before, "the face did not resume after pop");
}

/* -------------------------------------------------------------------------- */

static void cover__create(cgl_page_t *p) { cgl_page_set_root(p, NULL); }

static const cgl_page_vt_t cover__vt = {
    .type = "cover",
    .on_create = cover__create,
};

/* -------------------------------------------------------------------------- */
/* I. incremental damage leaves no ghosts                                     */
/* -------------------------------------------------------------------------- */

/**
 * The page damages only what moved (see the note in clock__frame) because a
 * full-dial damage costs the panel 43 band pushes per frame. The price of that
 * optimisation is that a region it forgets keeps the *previous* frame's pixels --
 * a ghost. So: drive a few hundred real frames, snapshot the panel, ask the screen
 * for a full repaint **at the same instant** (dt = 0, so nothing moves), and require
 * the two pictures to be identical. Any forgotten rectangle fails here, and the
 * failure names the pixel.
 */
static void test_no_ghosts(void)
{
    static cgl_color_t incremental[CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H];
    int32_t bad = 0;
    int32_t i;

    go_to(10u, 9u, 0u);
    run(260, 16u); /* ~4 s: the second hand sweeps, the caps move, the trail fades */
    memcpy(incremental, s_framebuffer, sizeof(incremental));

    cgl_screen_redraw_all(&s_screen);
    run(1, 0u); /* a full repaint of the same instant */

    for (i = 0; i < (int32_t)(CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H); i++)
    {
        if (incremental[i] != s_framebuffer[i])
        {
            if (bad == 0)
            {
                printf("       first ghost at (%d,%d): incremental %04X, full repaint %04X\n",
                       (int)(i % CGL_CFG_SCREEN_W), (int)(i / CGL_CFG_SCREEN_W),
                       (unsigned)incremental[i], (unsigned)s_framebuffer[i]);
            }
            bad++;
        }
    }
    CHECK(bad == 0, "%d pixels differ between incremental and fully repainted frames", (int)bad);

    /* And once more across a minute rollover, where the digits and the whole ring
       change at once. */
    run(60u * 1000u / 16u, 16u);
    memcpy(incremental, s_framebuffer, sizeof(incremental));
    cgl_screen_redraw_all(&s_screen);
    run(1, 0u);
    bad = 0;
    for (i = 0; i < (int32_t)(CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H); i++)
    {
        if (incremental[i] != s_framebuffer[i])
        {
            bad++;
        }
    }
    CHECK(bad == 0, "%d pixels differ after a minute rollover", (int)bad);
}

/* -------------------------------------------------------------------------- */
/* J. the face: a lit surface, not a fill                                     */
/* -------------------------------------------------------------------------- */

/** Perceived brightness of an RGB565 colour, 0..255 (the usual 0.30/0.59/0.11). */
static int lum(cgl_color_t c)
{
    int r = (int)((c >> 11) & 0x1Fu) * 8;
    int g = (int)((c >> 5) & 0x3Fu) * 4;
    int b = (int)(c & 0x1Fu) * 8;

    return ((r * 30) + (g * 59) + (b * 11)) / 100;
}

/**
 * The dial's background is a **function of the pixel**, and what this asserts is the
 * shape of that function: a radial falloff, a single light up and to the left, and a
 * texture on top. None of the three is visible in any single pixel, and all three
 * are the difference between a watch face and a printed circle -- which is why they
 * are measured here rather than photographed.
 *
 * WARNING: these assertions ask `app_page_clock_face_color()`, not the panel. That
 * is the point of the function existing, and it is also the honest thing to do: the
 * panel's pixel is that function *when nothing is drawn over it*, so asking the page
 * removes the "is there ink here?" question from a test about lighting.
 */
static void test_face_lighting(void)
{
    cgl_color_t seen[24];
    int distinct = 0;
    int i;

    /* --- the radial gradient: lit in the middle, darker at the rim ---------
     * Sampled along +x, which is 180 degrees away from the specular highlight, so
     * what these three measure is the gradient and not the light. */
    {
        int c20 = lum(app_page_clock_face_color(20, 0));
        int c41 = lum(app_page_clock_face_color(41, 0));
        int c70 = lum(app_page_clock_face_color(70, 0));

        CHECK(c20 > (c41 + 2), "the dial is not lit in the middle: 20 px out is %d, 41 px out is %d",
              c20, c41);
        CHECK(c41 > (c70 + 2), "the dial does not fall off toward the rim: 41 px out is %d, 70 px out is %d",
              c41, c70);
    }

    /* --- the specular highlight: one light, up and to the left -------------
     * Both points are 57 px from the centre, so the gradient is the same at both and
     * the only thing that can differ is where the light is. */
    {
        int ul = lum(app_page_clock_face_color(-40, -40));
        int lr = lum(app_page_clock_face_color(40, 40));

        CHECK(ul > (lr + 8), "the upper left of the dial is not lit: (-40,-40) is %d, (40,40) is %d",
              ul, lr);
    }

    /* --- the texture: the face is not a constant on a ring ------------------
     * Along a circle the gradient contributes one value, so *any* variation is the
     * brushing and the sunburst. The samples run over the right half only (from 12
     * o'clock, clockwise, half a turn), which keeps the specular highlight -- which
     * would answer this question by itself -- out of the sample. */
    for (i = 0; i < 24; i++)
    {
        cgl_angle_t a = (cgl_angle_t)(CGL_ANGLE_UP + ((uint32_t)i * (CGL_ANGLE_TURN / 48u)));
        int16_t dx;
        int16_t dy;
        cgl_color_t c;
        int k;
        bool dup = false;

        cgl_math_polar(a, 65, &dx, &dy);
        c = app_page_clock_face_color(dx, dy);
        for (k = 0; k < distinct; k++)
        {
            if (seen[k] == c)
            {
                dup = true;
                break;
            }
        }
        if (!dup)
        {
            seen[distinct++] = c;
        }
    }
    CHECK(distinct >= 4, "the face is a flat wash at r = 65: only %d distinct colours round the ring",
          distinct);
}

/* -------------------------------------------------------------------------- */
/* K. the hands: a tapered blade, and the shadow under it                     */
/* -------------------------------------------------------------------------- */

/**
 * 09:15, plus the settle `go_to()` costs. At a quarter past nine the **minute** hand
 * lies exactly along +x while the hour hand is at 9.25, i.e. half a turn away from
 * it, so the only orange ink inside the chapter ring is the second hand -- which is
 * what makes the two pixels 4 px above and below that column comparable: the same
 * radius, so the same gradient, and 5.7 degrees apart, so very nearly the same
 * sunburst.
 *
 * The hand is ~3 px half width there, and its shadow is offset two pixels down and
 * to the right: the lower sample is therefore in shadow and the upper one is bare
 * face. That is the entire claim -- a hand that floats above the dial darkens what is
 * under it, and if the shadow is ever dropped this is the assertion that notices.
 */
static void test_hand_shadow(void)
{
    int above;
    int below;

    go_to(9u, 15u, 0u);
    CHECK((now()->m == 15u) && (now()->s <= 4u), "the settle moved 09:15:00 to %02u:%02u:%02u",
          (unsigned)now()->h, (unsigned)now()->m, (unsigned)now()->s);

    /* Measured **relative to the background the page says is there**, because the
       crystal cover lies over both samples and its gradient across those 8 px would
       otherwise be read as shadow (or hide one). */
    above = lum(PX((int16_t)(CX + 30), (int16_t)(CY - 4))) - lum(dial_background(30, -4));
    below = lum(PX((int16_t)(CX + 30), (int16_t)(CY + 4))) - lum(dial_background(30, 4));

    CHECK(below < (above - 4),
          "the minute hand casts no shadow: 4 px above it is %+d against the dial, 4 px below "
          "it is %+d", above, below);
}

/* -------------------------------------------------------------------------- */
/* L. no hand is ever broken, at any angle                                    */
/* -------------------------------------------------------------------------- */

/** The dial's angles, restated from the page (which keeps them private). */
static cgl_angle_t ang_second(void)
{
    const clock_time_t *t = now();

    return (cgl_angle_t)(CGL_ANGLE_UP +
                         (cgl_angle_t)((((uint32_t)t->s * CGL_CLOCK_MS_PER_S) + (uint32_t)t->ms) *
                                       CGL_ANGLE_TURN / 60000u));
}

static cgl_angle_t ang_minute(void)
{
    const clock_time_t *t = now();

    return (cgl_angle_t)(CGL_ANGLE_UP +
                         (cgl_angle_t)((((uint32_t)t->m * 60u) + (uint32_t)t->s) *
                                       CGL_ANGLE_TURN / 3600u));
}

static cgl_angle_t ang_hour(void)
{
    const clock_time_t *t = now();
    uint32_t num = ((uint32_t)(t->h % 12u) * 3600u) + ((uint32_t)t->m * 60u) + (uint32_t)t->s;

    return (cgl_angle_t)(CGL_ANGLE_UP + (cgl_angle_t)((num * CGL_ANGLE_TURN) / 43200u));
}

/**
 * How many pixels of *hand* a scan across a hand finds, at radius `r`.
 *
 * "Ink" here means **brighter than the background the page says is there** -- the
 * surface *under the crystal cover* -- and both halves of that matter:
 *   - asking the page for its own background is what makes the test survive the
 *     minute pulse (which recolours the hands), the specular highlight (which is part
 *     of the face) and the cover's reflection (which is part of neither): none of them
 *     is ink;
 *   - requiring *brighter* rather than merely "different" keeps a hand's own shadow
 *     out of the count. The shadow is ink too, and a hand cut down to a hairline still
 *     has its shadow lying beside it: counting both made the thinnest hour hand
 *     measure 2 px when it should have measured 0, which is exactly the bug this test
 *     exists to catch.
 *
 * A hand is only ever *added* to by anything else that qualifies (the brand text,
 * another hand crossing it), and this test only asks for a minimum, so a false
 * positive can only make it lenient -- never green when it should be red.
 */
static int ink_across_hand(cgl_angle_t a, int16_t r)
{
    int16_t dx;
    int16_t dy;
    int16_t px;
    int16_t py;
    int u;
    int n = 0;

    cgl_math_polar(a, r, &dx, &dy);
    cgl_math_polar((cgl_angle_t)(a + CGL_ANGLE_QUARTER), 1, &px, &py);
    for (u = -8; u <= 8; u++)
    {
        int16_t x = (int16_t)(CX + dx + (u * px));
        int16_t y = (int16_t)(CY + dy + (u * py));

        if (lum(PX(x, y)) > (lum(dial_background((int16_t)(x - CX), (int16_t)(y - CY))) + 8))
        {
            n++;
        }
    }
    return n;
}

/**
 * The stroke rasterizer walks a hand **row by row**, narrowing each row to the strip
 * the axis passes through -- and that strip has to be the *projection of the pen onto
 * the row*, which for a hand lying almost horizontally is far wider than the pen is
 * thick (`half width / |sin|`: a 3 px needle seen at 11 degrees crosses a single row
 * over 16 px). Getting that wrong does not look like a bug at 45 degrees. It looks
 * like a hand that slowly turns into a dotted line as it approaches three o'clock,
 * vanishes, and then snaps back the moment it is exactly horizontal -- which is what
 * 10-20 s and 40-50 s of every minute looked like on hardware, and what the minute
 * and hour hands did every time their own sweep passed through those angles.
 *
 * So: drive each hand through a full turn -- the second hand one pose per second, the
 * minute hand one per minute, the hour hand in half degree steps -- and at each pose
 * require at least two pixels of ink across the blade at a radius where it is the
 * only thing there. The radius is picked per hand: 40 for the needle, 45 for the
 * minute hand (past the hour hand's 40 px tip) and 28 for the hour hand (inside it,
 * where the minute hand can only ever *add* ink).
 */
static void test_hands_are_unbroken(void)
{
    int thin;
    int worst;
    uint8_t i;
    uint8_t j;

    /* Let anything the previous test left running finish: this test measures shape. */
    run(10, 16u);

    /* --- the second hand: 60 poses, one per second -------------------------
     * 09:15 keeps the minute hand horizontal and the hour hand half a turn away from
     * it, so a scan at r = 40 crosses the needle and nothing else. */
    thin = 0;
    worst = 99;
    go_to(9u, 15u, 0u);
    for (i = 0u; i < 60u; i++)
    {
        int w;

        app_page_clock_set_time(&s_clock, 9u, 15u, i);
        run(2, 16u);
        w = ink_across_hand(ang_second(), 40);
        if (w < worst)
        {
            worst = w;
        }
        if (w < 2)
        {
            if (thin < 4)
            {
                printf("       second hand is %d px across at :%02u\n", w, (unsigned)i);
            }
            thin++;
        }
    }
    CHECK(thin == 0, "the second hand thins to nothing at %d of 60 seconds (thinnest %d px)", thin,
          worst);

    /* --- the minute hand: 60 poses, one per minute ------------------------- */
    thin = 0;
    worst = 99;
    for (i = 0u; i < 60u; i++)
    {
        int w;

        /* The second hand is parked a quarter turn away from the minute hand so that
           it cannot cover it. */
        app_page_clock_set_time(&s_clock, 0u, i, (uint8_t)((i + 15u) % 60u));
        run(2, 16u);
        w = ink_across_hand(ang_minute(), 45);
        if (w < worst)
        {
            worst = w;
        }
        if (w < 2)
        {
            if (thin < 4)
            {
                printf("       minute hand is %d px across at :%02u\n", w, (unsigned)i);
            }
            thin++;
        }
    }
    CHECK(thin == 0, "the minute hand thins to nothing at %d of 60 minutes (thinnest %d px)", thin,
          worst);

    /* --- the hour hand: 96 poses, half a degree apart ---------------------- */
    thin = 0;
    worst = 99;
    for (i = 0u; i < 12u; i++)
    {
        for (j = 0u; j < 8u; j++)
        {
            int w;

            app_page_clock_set_time(&s_clock, i, (uint8_t)(j * 7u),
                                    (uint8_t)(((i * 5u) + 15u) % 60u));
            run(2, 16u);
            w = ink_across_hand(ang_hour(), 28);
            if (w < worst)
            {
                worst = w;
            }
            if (w < 2)
            {
                if (thin < 4)
                {
                    printf("       hour hand is %d px across at %02u:%02u\n", w, (unsigned)i,
                           (unsigned)(j * 7u));
                }
                thin++;
            }
        }
    }
    CHECK(thin == 0, "the hour hand thins to nothing at %d of 96 poses (thinnest %d px)", thin,
          worst);
}

/* -------------------------------------------------------------------------- */
/* M. the outer ring is a cylinder, and it is sunk                            */
/* -------------------------------------------------------------------------- */

/**
 * The outer ring used to be a flat stroke -- one colour, which is a ring with no
 * *surface*: nothing in it says whether the band is proud of the dial, level with it
 * or cut into it. What it is now is a cross-section (dark where it meets the dial,
 * lit in the middle, dark again where it meets the case lip) plus the shading of the
 * well it sits in, and both halves are asserted here because neither is visible in
 * any single pixel.
 *
 * The two ends of the profile are sampled on the horizontal axis, where the well's
 * tilt is exactly zero, so what they measure is the cross-section alone. The tilt is
 * then measured on its own -- and it runs **the other way round** from the dial's own
 * highlight (test_face_lighting): a convex surface lit from above is bright at the
 * top, a concave one is dark there, and a bezel shaded like the face is what makes a
 * ring look like a sticker.
 */
static void test_ring_surface(void)
{
    int at_mid = lum(app_page_clock_ring_color(-CLOCK_R_RING, 0));
    int at_inner = lum(app_page_clock_ring_color(-(CLOCK_WELL_IN + 1), 0));
    int at_outer = lum(app_page_clock_ring_color(-(CLOCK_WELL_OUT - 1), 0));
    int top = lum(app_page_clock_ring_color(0, -CLOCK_R_RING));
    int bottom = lum(app_page_clock_ring_color(0, CLOCK_R_RING));

    CHECK(at_mid > (at_inner + 8),
          "the ring does not fall away inward: the middle of the cross-section is %d, its inner edge %d",
          at_mid, at_inner);
    CHECK(at_mid > (at_outer + 8),
          "the ring does not fall away outward: the middle of the cross-section is %d, its outer edge %d",
          at_mid, at_outer);
    CHECK(bottom > (top + 8),
          "the well is not sunk: its top is %d and its bottom is %d (a recess is dark above and "
          "bright below)", top, bottom);
}

/* -------------------------------------------------------------------------- */
/* N. there is a crystal over the dial                                        */
/* -------------------------------------------------------------------------- */

/** How much the crystal lifts a surface, in luminance levels, at an offset. */
static int cover_lift(int16_t dx, int16_t dy)
{
    return lum(dial_background(dx, dy)) - lum(app_page_clock_face_color(dx, dy));
}

/**
 * The cover is the one part of this page that is not a surface: it composites over
 * whatever is under it -- the dial, the ring, a mark, a hand or the hub -- and it is
 * the *last* thing drawn, which is the whole reason it reads as a cover (a reflection
 * that stops at the edge of a hand is painted on the dial; one that runs across the
 * hand is in front of it).
 *
 * Three claims, each a way a "glass cover" can fail while still being some bright
 * pixels somewhere:
 *   1. the **lit quadrant** reflects far more than the one opposite it -- a cover that
 *      lifts everything equally is a milky film, not a reflection. (Measured as the
 *      strongest lift over a grid of samples in each quadrant rather than at two
 *      hand-picked offsets, because a reflection is a *shape*: a single sample would
 *      be asserting where its edge happens to fall.)
 *   2. the reflection is strongest at the **rim**, where the glass curves away, and
 *      not in the middle: that is the difference between a curved cover and a flat
 *      one.
 *   3. it reaches the rim at all, i.e. it is bright enough to be *seen* rather than
 *      merely detected.
 *
 * That the cover really lies over the hands is asserted where the hands are, and
 * *through* it: `near_glass()` in test_second_hand_angle would fail if the glass were
 * drawn before them instead of after.
 */
static void test_crystal_cover(void)
{
    int lit_max = 0;
    int far_max = 0;
    int rim;
    int mid;
    int dx;
    int dy;

    for (dx = -66; dx <= 66; dx += 11)
    {
        for (dy = -66; dy <= 66; dy += 11)
        {
            int lift;

            if (((dx * dx) + (dy * dy)) > (66 * 66))
            {
                continue;
            }
            lift = cover_lift((int16_t)dx, (int16_t)dy);
            if ((dx < 0) && (dy < 0))
            {
                if (lift > lit_max)
                {
                    lit_max = lift;
                }
            }
            else if ((dx > 0) && (dy > 0) && (lift > far_max))
            {
                far_max = lift;
            }
        }
    }

    /* 1. the lit corner against the opposite one */
    CHECK(lit_max > (far_max + 8),
          "the cover is a uniform film rather than a reflection: its strongest lift is %d levels "
          "up and to the left and %d down and to the right", lit_max, far_max);

    /* 2. the crescent at the rim (r = 72, on the light's axis) against the middle */
    rim = cover_lift(-47, -55);
    mid = cover_lift(0, 0);
    CHECK(rim > (mid + 8),
          "the reflection is not concentrated at the rim: it lifts the edge by %d levels and the "
          "centre by %d", rim, mid);

    /* 3. ... and it is bright enough to see */
    CHECK(rim > 12, "the crystal's edge reflection is too faint to read (%d levels)", rim);
}

/* -------------------------------------------------------------------------- */
/* O. the background is a design, not a fill                                  */
/* -------------------------------------------------------------------------- */

/**
 * The page used to be drawn on the host's flat background colour: everything outside
 * the dial and around the complications was one value of black, which is a *correct*
 * frame and a flat screen. This asserts the three things that make the replacement a
 * design rather than a decoration, and each of them is a way it can quietly degrade:
 *
 *   1. **one light for the whole page**: the wall is brighter on the upper left, where
 *      the dial's own specular highlight and the crystal's reflection come from, and
 *      darker toward the far corner -- so the dial and the screen agree about where
 *      the light is (compare test_face_lighting);
 *   2. **the card is lifted off the wall**: the complications sit on a panel that is
 *      lighter than the wall around it, with a brighter top edge -- a raised surface
 *      lit from above, the counterpart of the *sunken* ring (test_ring_surface);
 *   3. and the picture agrees with the function: the background is written one pixel at
 *      a time, so a rendered pixel off the dial is exactly `app_page_clock_bg_color()`.
 */
static void test_background_design(void)
{
    /* 1. the light. Two points the same distance from the dial's highlight, one up and
       left of the light and one down and right of it -- measured against the *wall*
       only, i.e. away from the card, which would answer for it. */
    {
        int lit = lum(app_page_clock_bg_color(20, 20));
        int dark = lum(app_page_clock_bg_color(170, 150));

        CHECK(lit > (dark + 4),
              "the background has no light in it: (20,20) is %d and (170,150) is %d", lit, dark);
    }

    /* 2. the card, at the same height, inside it and outside it */
    {
        int card = lum(app_page_clock_bg_color(200, 120));
        int wall = lum(app_page_clock_bg_color(176, 120));

        CHECK(card > (wall + 4), "the complication card does not read as raised: inside it %d, "
              "on the wall beside it %d", card, wall);
    }

    /* 2b. ... and its top edge catches the light */
    {
        int top = lum(app_page_clock_bg_color(248, 6));
        int bottom = lum(app_page_clock_bg_color(248, 166));

        CHECK(top > (bottom + 20), "the card has no lit top edge: its top row is %d and its "
              "bottom row %d", top, bottom);
    }

    /* 2c. the panel's own bevel */
    {
        int top = lum(app_page_clock_bg_color(60, 0));
        int bot = lum(app_page_clock_bg_color(60, (int16_t)(CGL_CFG_SCREEN_H - 1)));

        CHECK(top > (bot + 20), "the panel has no bevelled edges: its top row is %d and its "
              "bottom row %d", top, bot);
    }

    /* 3. the picture agrees with the function, everywhere the dial does not cover: the
       screen's four corners and the gap between the dial and the card. */
    {
        static const int16_t px[6] = { 2, 2, 316, 316, 172, 200 };
        static const int16_t py[6] = { 2, 168, 2, 168, 40, 100 };
        int k;

        go_to(10u, 9u, 0u);
        for (k = 0; k < 6; k++)
        {
            CHECK(PX(px[k], py[k]) == app_page_clock_bg_color(px[k], py[k]),
                  "the background at (%d,%d) is %04X, the page says %04X", (int)px[k], (int)py[k],
                  (unsigned)PX(px[k], py[k]),
                  (unsigned)app_page_clock_bg_color(px[k], py[k]));
        }
    }
}

/* -------------------------------------------------------------------------- */
/* P. the flash tables are exactly what their generator says                  */
/* -------------------------------------------------------------------------- */

/**
 * The dial's two tables are data in flash, and **data drifts**: the moment someone
 * changes a palette stop or a brush amplitude, the code that would have produced the
 * new colours is not the code that draws them, and the page goes on rendering the old
 * ones while the constants claim otherwise. No rendering test can catch that -- the
 * picture is still a perfectly good picture -- which is why the generator lives next
 * to the data in app_page_clock_dial.h and this case re-derives every entry.
 *
 * On a mismatch it prints the whole regenerated array, because that is the fix: paste
 * it back. (A palette stop moves *every* entry after it, so hunting for "the wrong
 * one" is the wrong instinct.)
 */
static void test_dial_tables_match_generator(void)
{
    int bad_face = 0;
    int bad_well = 0;
    int bad_bg = 0;
    int i;

    for (i = 0; i < CLOCK_FACE_LUT_N; i++)
    {
        uint16_t want = (uint16_t)clock__face_lut_entry((uint16_t)i);

        if (s_face_lut[i] != want)
        {
            if (bad_face == 0)
            {
                printf("       face[%d] is 0x%04X, the generator says 0x%04X\n", i,
                       (unsigned)s_face_lut[i], (unsigned)want);
            }
            bad_face++;
        }
    }
    for (i = 0; i < CLOCK_WELL_N; i++)
    {
        uint16_t want = (uint16_t)clock__well_lut_entry((uint16_t)i);

        if (s_well_lut[i] != want)
        {
            if (bad_well == 0)
            {
                printf("       well[%d] is 0x%04X, the generator says 0x%04X\n", i,
                       (unsigned)s_well_lut[i], (unsigned)want);
            }
            bad_well++;
        }
    }

    for (i = 0; i < CLOCK_BG_LUT_N; i++)
    {
        uint16_t want = (uint16_t)clock__bg_lut_entry((uint16_t)i);

        if (s_bg_lut[i] != want)
        {
            if (bad_bg == 0)
            {
                printf("       wall[%d] is 0x%04X, the generator says 0x%04X\n", i,
                       (unsigned)s_bg_lut[i], (unsigned)want);
            }
            bad_bg++;
        }
    }

    if ((bad_face != 0) || (bad_well != 0) || (bad_bg != 0))
    {
        printf("       --- regenerate app_page_clock_dial.h from this ---\n");
        for (i = 0; i < CLOCK_FACE_LUT_N; i++)
        {
            printf("    0x%04Xu,%s", (unsigned)clock__face_lut_entry((uint16_t)i),
                   ((i % 12) == 11) ? "\n" : " ");
        }
        printf("\n");
        for (i = 0; i < CLOCK_WELL_N; i++)
        {
            printf("    0x%04Xu,%s", (unsigned)clock__well_lut_entry((uint16_t)i),
                   ((i % 12) == 11) ? "\n" : " ");
        }
        printf("\n");
        for (i = 0; i < CLOCK_BG_LUT_N; i++)
        {
            printf("    0x%04Xu,%s", (unsigned)clock__bg_lut_entry((uint16_t)i),
                   ((i % 12) == 11) ? "\n" : " ");
        }
        printf("\n");
    }

    CHECK(bad_face == 0, "the face table disagrees with its generator at %d of %d entries",
          bad_face, (int)CLOCK_FACE_LUT_N);
    CHECK(bad_well == 0, "the well table disagrees with its generator at %d of %d entries",
          bad_well, (int)CLOCK_WELL_N);
    CHECK(bad_bg == 0, "the wall table disagrees with its generator at %d of %d entries",
          bad_bg, (int)CLOCK_BG_LUT_N);
}


/* -------------------------------------------------------------------------- */
/* Q. the mechanical tick (CLOCK_SECOND_MODE == CLOCK_SECOND_TICK)             */
/* -------------------------------------------------------------------------- */

/** Drive real frames until the fraction of the second is inside [lo, hi]. Returns
    the `ms` it landed on, or 0 if the window was never reached. */
static uint16_t seek_ms(uint16_t lo, uint16_t hi)
{
    const clock_time_t *t;
    int guard;

    for (guard = 0; guard < 200; guard++)
    {
        run(1, 16u);
        t = now();
        if (((uint32_t)t->ms >= (uint32_t)lo) && ((uint32_t)t->ms <= (uint32_t)hi))
        {
            return t->ms;
        }
    }
    return 0u;
}

/**
 * The angle of second `s` at `ms`, from the **page's own inline arithmetic**
 * (`clock__second_angle` in app_page_clock.h, which is the expression the dial draws
 * with).
 *
 * WARNING: this deliberately does **not** go through `app_page_clock_second_angle()`.
 * That accessor is an ordinary (non-inline) function in the page's translation unit, so
 * a test that calls it is making a cross-translation-unit call *inside the one function
 * whose answer depends on `CLOCK_SECOND_MODE`*. The motion lives in the header as
 * `CGL_AINLINE` precisely so that both sides see the same definition; this is the test
 * doing what the dial does, and the accessor is checked against it at the end of this
 * section, where a disagreement is the failure.
 */
static cgl_angle_t tick_angle(uint8_t s, uint16_t ms)
{
    clock_time_t t;

    t.s = (uint8_t)(s % 60u);
    t.ms = (uint16_t)(ms % (uint16_t)CGL_CLOCK_MS_PER_S);
    return clock__second_angle(&t);
}

/** Where the hand would be if the sweep were compiled in instead: the pure
    `(s*1000 + ms) / 60 s` reading, with no tick curve anywhere near it. */
static cgl_angle_t sweep_angle(uint8_t s, uint16_t ms)
{
    return (cgl_angle_t)(CGL_ANGLE_UP + (cgl_angle_t)((((uint32_t)s * CGL_CLOCK_MS_PER_S) +
                                                        (uint32_t)ms) *
                                                       CGL_ANGLE_TURN / 60000u));
}

/** The smaller of the two ways round from `a` to `b`, in angle units (0..32768). */
static uint32_t angle_gap(cgl_angle_t a, cgl_angle_t b)
{
    uint32_t d = (uint32_t)(cgl_angle_t)(a - b) & 0xFFFFu;

    return (d > 32768u) ? (65536u - d) : d;
}

/** How far from its own mark the hand may point and still count as "holding", in angle
    units: a tenth of a second (0.6 degrees), well under a pixel of tip at r = 69. */
#define TICK_HOLD_SLOP 3277u

/** The hold phase, in ms: everything before the step starts. */
#define TICK_HOLD_END ((uint32_t)CGL_CLOCK_MS_PER_S - (uint32_t)CLOCK_TICK_MS)

/** Where the second hand's tip is drawn at `(s, ms)`, from the page's own angle. */
static void tick_tip(uint8_t s, uint16_t ms, int16_t *x, int16_t *y)
{
    int16_t dx;
    int16_t dy;

    cgl_math_polar(tick_angle(s, ms), (int16_t)(CLOCK_R_SECOND - 2), &dx, &dy);
    *x = (int16_t)(CX + dx);
    *y = (int16_t)(CY + dy);
}

/**
 * **The tick holds, then snaps into the next second**, and neither half is visible in a
 * still picture -- which is why the page exposes the motion as a pure function and this
 * section measures its *shape* rather than a pose:
 *
 *   1. the **marks**: at every whole second (all 60 of them) the hand is exactly on the
 *      second, so whatever the curve does in between, this is a clock and not an effect;
 *   2. the **hold**: for the first `CGL_CLOCK_MS_PER_S - CLOCK_TICK_MS` of every second
 *      the angle is *identical* to the mark, where a sweep would be a third of a turn
 *      further round by the end of it;
 *   3. the **snap**: in the last `CLOCK_TICK_MS` it has moved, by a large fraction of a
 *      second of dial and no more than the step plus its overshoot;
 *   4. **forwards, once per second**: the curve never steps back inside a second, which
 *      is what keeps the damage model's "this frame union last frame" box honest.
 *
 * Each claim is checked twice -- against the page's own arithmetic (sharp, and it would
 * catch a curve that drifts) and against the **panel** (the accent ink the crystal
 * leaves at that radius) -- so the angle cannot quietly stop being the thing that gets
 * drawn.
 *
 * WARNING: the probes are driven with real `dt = 16 ms` frames, and the time is set
 * **twice** with one frame in between. A single `set_time()` anchors at the next frame
 * boundary, so `ms` would be whatever the frame clock happened to say; the frame
 * between the two sets advances the clock without the time having been re-anchored
 * against it, which lands the probes on a whole second.
 */
static void test_tick_mode(void)
{
#if TICK_MODE
    uint16_t at;
    uint16_t at_mid;
    int16_t px;
    int16_t py;
    int k;

    /* --- 0. the marks, before anything moves ------------------------------ */
    for (k = 0; k < 60; k++)
    {
        cgl_angle_t want =
            (cgl_angle_t)(CGL_ANGLE_UP + (cgl_angle_t)(((uint32_t)k * CGL_ANGLE_TURN) / 60u));

        CHECK(tick_angle((uint8_t)k, 0u) == want,
              "at :%02d.0 the ticked hand is not exactly on the second (got %u, want %u)", k,
              (unsigned)tick_angle((uint8_t)k, 0u), (unsigned)want);
    }

    /* --- 1. the hold, on the panel ----------------------------------------
     * Half a second after the mark, with another two thirds of the hold still to run:
     * the hand is exactly where it was, and the panel agrees. */
    app_page_clock_set_time(&s_clock, 12u, 0u, 4u);
    run(1, 0u); /* the anchor lands here, at :04.000 */
    app_page_clock_set_time(&s_clock, 12u, 0u, 4u);
    at = seek_ms(350u, 550u);

    if (at == 0u)
    {
        CHECK(0, "the hold probe never arrived (ms = %u)", (unsigned)now()->ms);
    }
    else
    {
        uint32_t gap = angle_gap(tick_angle(4u, at), tick_angle(4u, 0u));
        uint32_t raw = angle_gap(tick_angle(4u, at), sweep_angle(4u, at));

        CHECK(now()->s == 4u, "the parked probe drifted to :%02u (ms = %u)", (unsigned)now()->s,
              (unsigned)at);
        CHECK(gap <= TICK_HOLD_SLOP,
              "at :04.%u the hand points %.2f degrees off :04: it is gliding, not ticking",
              (unsigned)at, (double)((gap * 360.0) / 65536.0));
        /* ...and that is *not* where the stroke would be without the tick: this is what
           makes the check a test of the tick rather than of "the hand is somewhere". */
        CHECK((raw - TICK_HOLD_SLOP) > 0u,
              "at :04.%u the tick and a plain sweep are the same angle (%u units off the mark, "
              "which a sweep would also be)",
              (unsigned)at, (unsigned)raw);
        tick_tip(4u, at, &px, &py);
        CHECK(near_glass(px, py, COL_ACCENT, 2),
              "the page says the hand is parked at :04.%u but there is no accent ink at (%d,%d)",
              (unsigned)at, (int)px, (int)py);
    }

    /* --- 2. the snap ------------------------------------------------------
     * In the last `CLOCK_TICK_MS` the hand has left the mark, by at least a fifth of a
     * second of dial and no more than the step plus its overshoot. Both bounds are
     * needed: the first is what separates a tick from a *parked* hand (which is what the
     * hold above just asserted) and the second is what separates it from a jump cut to
     * 60 degrees.
     *
     * WARNING: "has moved" is measured **from :04**, not toward :05. The tick overshoots
     * (`CLOCK_TICK_EASE` is EASE_OUT_BACK), so late in the step the hand is a little
     * *past* :05 and "how far is it from :05" stops meaning "has it moved".
     *
     * WARNING: the two lower bounds are written as `(x - limit) > 0u` rather than
     * `x > limit`. MSVC 17.14 at /O2 miscompiles the direct form here -- measured in this
     * very test, with both operands printed from the same registers: `0x551 > 0xCCD` came
     * out false while `0x551 < 0xCCD` came out true. The subtraction is the same
     * predicate and compiles correctly. */
    app_page_clock_set_time(&s_clock, 12u, 0u, 4u);
    run(1, 0u);
    app_page_clock_set_time(&s_clock, 12u, 0u, 4u);
    at = seek_ms((uint16_t)(TICK_HOLD_END + 40u), (uint16_t)(CGL_CLOCK_MS_PER_S - 8u));

    if (at == 0u)
    {
        CHECK(0, "the snap probe never arrived (ms = %u)", (unsigned)now()->ms);
    }
    else
    {
        uint32_t moved = angle_gap(tick_angle(4u, at), tick_angle(4u, 0u));

        CHECK((moved - TICK_HOLD_SLOP) > 0u,
              "at :04.%u the hand has not left :04 at all: the tick never snapped (moved %u)",
              (unsigned)at, (unsigned)moved);
        CHECK((moved - (65536u / 300u)) > 0u,
              "at :04.%u the hand has moved only %.2f seconds of dial: the snap is not a snap",
              (unsigned)at, (double)((moved * 60.0) / 65536.0));
        CHECK(moved < ((65536u * 13u) / 600u),
              "at :04.%u the hand is %.2f seconds of dial past :04: the tick overshot its step",
              (unsigned)at, (double)((moved * 60.0) / 65536.0));
        tick_tip(4u, at, &px, &py);
        CHECK(near_glass(px, py, COL_ACCENT, 2),
              "the page says the hand is at :04.%u but there is no accent ink at (%d,%d)",
              (unsigned)at, (int)px, (int)py);
    }

    /* --- 3. forwards, once per second --------------------------------------
     * Sampled at the frame cadence, which is what keeps the damage model honest: the box
     * is the union of the hand at this frame's angle and at the last one's, so a curve
     * that overshot, settled and then stepped *back* before the next mark -- or that
     * jumped most of a turn in one frame -- would leave a ghost of an angle no frame ever
     * damaged.
     *
     * One backward step is legal and is the whole point of `EASE_OUT_BACK`: the settle
     * from the overshoot onto the mark. Anything at or past half a turn is not. */
    {
        uint8_t s;

        for (s = 0u; s < 60u; s++)
        {
            uint8_t i;
            cgl_angle_t prev = tick_angle(s, 0u);

            for (i = 1u; i < 63u; i++)
            {
                uint16_t ms = (uint16_t)((uint32_t)i * 16u);
                cgl_angle_t cur = tick_angle(s, ms);

                if (cur != prev)
                {
                    CHECK(angle_gap(cur, prev) < 32768u,
                          "the tick goes backwards at :%02u.%03u", (unsigned)s, (unsigned)ms);
                    CHECK(angle_gap(cur, prev) < 7000u,
                          "the tick jumps %.2f degrees between %u ms and %u ms: more than a whole "
                          "second of dial in one frame",
                          (double)((angle_gap(cur, prev) * 360.0) / 65536.0), (unsigned)(ms - 16u),
                          (unsigned)ms);
                }
                prev = cur;
            }
        }
    }

    /* --- 4. the public accessor agrees ------------------------------------
     * `app_page_clock_second_angle()` is the door a test (or a HUD) is meant to use, and
     * it is a *cross-translation-unit* call into the page -- which is exactly how a build
     * that reads `CLOCK_SECOND_MODE` in one translation unit and not the other would end
     * up measuring the wrong motion. Mid-hold and mid-step. */
    app_page_clock_set_time(&s_clock, 12u, 0u, 4u);
    run(1, 0u);
    app_page_clock_set_time(&s_clock, 12u, 0u, 4u);
    at = seek_ms(350u, 550u);
    at_mid = seek_ms((uint16_t)(TICK_HOLD_END + 40u), (uint16_t)(CGL_CLOCK_MS_PER_S - 8u));
    CHECK(app_page_clock_second_angle(4u, at) == tick_angle(4u, at),
          "the accessor and the page's arithmetic disagree mid-hold at :04.%u (%u vs %u)",
          (unsigned)at, (unsigned)app_page_clock_second_angle(4u, at), (unsigned)tick_angle(4u, at));
    /* ...and mid-hold it is on the mark, which is the property the accessor is being
       asked about: the *page's* own motion, not a third one. */
    CHECK(angle_gap(app_page_clock_second_angle(4u, at), tick_angle(4u, 0u)) <= TICK_HOLD_SLOP,
          "the accessor's hand is %.2f degrees off :04 at :04.%u, which is inside the hold",
          (double)(angle_gap(app_page_clock_second_angle(4u, at), tick_angle(4u, 0u)) * 360.0 /
                   65536.0),
          (unsigned)at);
    CHECK(app_page_clock_second_angle(4u, at_mid) == tick_angle(4u, at_mid),
          "the accessor and the page's arithmetic disagree mid-step at :04.%u (%u vs %u)",
          (unsigned)at_mid, (unsigned)app_page_clock_second_angle(4u, at_mid),
          (unsigned)tick_angle(4u, at_mid));
    CHECK(app_page_clock_second_angle(5u, 0u) ==
              (cgl_angle_t)(CGL_ANGLE_UP + (cgl_angle_t)((5u * CGL_ANGLE_TURN) / 60u)),
          "the accessor is not on :05.0 exactly (got %u)",
          (unsigned)app_page_clock_second_angle(5u, 0u));

#else
    (void)0; /* the sweep build: nothing here applies (see TICK_MODE) */
#endif
}

int main(void)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));

    printf("watch face page: time base, calendar, dial geometry, animations, coverage\n\n");

    if (cgl_display_init(&s_display, &s_mock_drv, NULL) != 0)
    {
        printf("display init failed\n");
        return 1;
    }
    cgl_page_nav_init(&s_nav, "clock_test",
                      cgl_rect(0, 0, (int16_t)(CGL_CFG_SCREEN_W - 1),
                               (int16_t)(CGL_CFG_SCREEN_H - 1)));
    s_links.smash = NULL;
    s_links.bullet = NULL;
    s_links.info = NULL;
    s_links.menu = NULL;
    s_links.clock = &s_clock;
    s_ctx.screen = &s_screen;
    s_ctx.nav = &s_nav;
    s_ctx.app = &s_links;

    app_page_clock_attach(&s_clock.base, &s_ctx);
    cgl_page_init(&s_cover, &cover__vt, "cover", NULL);
    if (cgl_screen_init(&s_screen, &s_display, &s_nav) != 0)
    {
        printf("screen init failed\n");
        return 1;
    }
    (void)cgl_page_nav_reset(&s_nav, &s_clock.base);
    cgl_screen_frame(&s_screen, 0u);

    /* The page must have built exactly the widgets it says it has, and the widget
       tree must not have silently dropped any of them (see the child limit in
       cgl_widget_add_child). */
    CHECK(s_clock.base.root != NULL, "the page has no widget tree");
    CHECK(cgl_page_root(&s_clock.base) != NULL, "cgl_page_root disagrees with the page");
    CHECK(cgl_widget_children_dropped() == 0u, "the clock page dropped %u widgets",
          (unsigned)cgl_widget_children_dropped());

    printf("A  time base\n");
    test_time_base();
    printf("B  calendar\n");
    test_calendar();
    test_seconds_of_day();
    printf("C  second hand angle\n");
    test_second_hand_angle();
    printf("D  the ring\n");
    test_ring_is_a_ring();
    printf("E  entrance reveal\n");
    test_reveal();
    printf("F  comet trail\n");
    test_trail();
    printf("G  complications\n");
    test_digital_roll();
    test_gauge();
    printf("H  covered page\n");
    test_covered();
    printf("I  incremental damage\n");
    test_no_ghosts();
    printf("J  the face: gradient, highlight, texture\n");
    test_face_lighting();
    printf("K  the hands: the shadow under one\n");
    test_hand_shadow();
    printf("L  no hand is ever broken\n");
    test_hands_are_unbroken();
    printf("M  the outer ring: a cylinder, sunk\n");
    test_ring_surface();
    printf("N  the crystal cover\n");
    test_crystal_cover();
    printf("O  the background: a design, not a fill\n");
    test_background_design();
    printf("P  the flash tables against their generator\n");
    test_dial_tables_match_generator();
    if (TICK_MODE)
    {
        printf("Q  the second hand: hold, snap, and the marks at both ends\n");
    }
    test_tick_mode();

    if (s_fails == 0)
    {
        printf("all passed.\n");
        return 0;
    }
    printf("%d failed.\n", s_fails);
    return 1;
}
