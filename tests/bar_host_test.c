// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    bar_host_test.c
  * @brief   Pixel-exact spec of the built-in bar widget (src/core/cgl_bar.c).
  *
  *  What this file is for
  *  ---------------------
  *  The bar used to be the demo's 85-line teaching example, and what it was
  *  tested by was a picture: "it attaches, it renders, the value clamps to 100".
  *  A picture cannot settle any of the things that make a bar a *widget of the
  *  framework* rather than a colour block, so they are measured here instead:
  *
  *   1. **The value -> fill-length mapping**, including the two cases that break
  *      a naive implementation: a signed range, and a range wide enough that
  *      `(off << 8) / span` overflows `int32_t` (`INT32_MIN..INT32_MAX`). The
  *      framework may not use `int64` for it -- that drags `__aeabi_ldivmod` in
  *      from libgcc (cgl_anim.h) -- so the fixed-point division is long
  *      division, and long division is exactly the kind of code that is right
  *      until it is not.
  *   2. **The end points are exact**: 0 at `vmin`, the full length at `vmax`, and
  *      a value outside the range clamps instead of drawing past the frame.
  *   3. **The widget clears its own background.** The band renderer has no
  *      previous frame to compare against, so a bar that only painted its filled
  *      part would leave the tail of a longer bar behind the moment the value
  *      drops. That is invisible in a snapshot of a rising bar and obvious here.
  *   4. **The border really is the padding** (the fill never overwrites the ring)
  *      and a bar thinner than the requested border is clamped rather than left
  *      with nothing to fill.
  *   5. **Orientation and direction**, all four combinations, because "vertical,
  *      filling upwards" is the one people get wrong by a pixel at the ends.
  *   6. **Segments** divide the whole bar, not just the filled part -- which is
  *      what makes `seg == track` a second, documented look ("the fill is cut
  *      into blocks") instead of a dead end.
  *   7. **A setter damages only when the drawn pixels can really change.** A
  *      progress bar is typically written every frame; if `set_value()` marked
  *      damage unconditionally, a static bar would repaint the screen forever.
  *   8. **Animation is the widget's own**, driven by `update`, starting from what
  *      is drawn rather than from the previous target, and not advancing at all
  *      without a tick.
  *
  *  How it renders
  *  --------------
  *  One full-screen buffer is bound as a **single band** canvas
  *  (`cgl_canvas_init(&c, fb, 320, 172, 0)`), so every pixel the widget paints is
  *  addressable directly and the assertions are plain colour comparisons. The
  *  buffer starts out filled with a sentinel colour that no widget colour equals,
  *  which is also how "the widget stayed inside its frame" is checked.
  *
  *  The last step writes that buffer out as `build/bar_test.ppm/.bmp`: a gallery
  *  of the shapes the assertions above are about, so the numbers have a picture
  *  next to them.
  *
  *  Build & run
  *  -----------
  *      cmake -B build/host -DCYTGL_BUILD_TESTS=ON
  *      cmake --build build/host
  *      ctest --test-dir build/host -R bar_test --output-on-failure
  ******************************************************************************
  */
#include <stdio.h>

#include <cytgl/cgl_bar.h>
#include <cytgl/cytgl_config.h>
#include "test_image_out.h"

#define FB_W CGL_CFG_SCREEN_W
#define FB_H CGL_CFG_SCREEN_H

#define PX(x, y) s_fb[((int32_t)(y) * (int32_t)FB_W) + (int32_t)(x)]

/* Widget colours, deliberately five distinct RGB565 values plus a sentinel. */
#define C_TRACK CGL_RGB565(36, 42, 58)   /* dark blue-grey */
#define C_FILL CGL_RGB565(255, 176, 32)  /* amber          */
#define C_BORD CGL_RGB565(238, 232, 213) /* cream          */
#define C_SEG CGL_RGB565(255, 0, 0)      /* red            */
#define C_OUT CGL_RGB565(0, 255, 0)      /* sentinel: "nothing was drawn here" */

/* The standard bar body used by most of the tests: 200 x 10 at (10, 10). */
#define BAR_X 10
#define BAR_Y 10
#define BAR_W 200
#define BAR_H 10

static cgl_color_t s_fb[FB_W * FB_H];
static cgl_canvas_t s_canvas;
static cgl_bar_t s_bar;

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
/* harness                                                                    */
/* -------------------------------------------------------------------------- */

static void scene_begin(void)
{
    int32_t i;

    for (i = 0; i < (int32_t)(FB_W * FB_H); i++)
    {
        s_fb[i] = C_OUT;
    }
}

static void render(cgl_widget_t *w)
{
    cgl_canvas_init(&s_canvas, s_fb, (int16_t)FB_W, (int16_t)FB_H, 0);
    cgl_widget_draw_tree(w, &s_canvas);
}

/** Pixels of `r` that are not `want`; prints the first offender. */
static uint32_t mismatch(cgl_rect_t r, cgl_color_t want, const char *what)
{
    uint32_t bad = 0u;
    int16_t x;
    int16_t y;

    for (y = r.y0; y <= r.y1; y++)
    {
        for (x = r.x0; x <= r.x1; x++)
        {
            if (PX(x, y) != want)
            {
                if (bad == 0u)
                {
                    printf("  (first offender of \"%s\": (%d,%d) = 0x%04X, want 0x%04X)\n",
                           what, (int)x, (int)y, (unsigned)PX(x, y), (unsigned)want);
                }
                bad++;
            }
        }
    }
    return bad;
}

/** Pixels outside `frame` that are no longer the sentinel. */
static uint32_t outside_bad(cgl_rect_t frame, const char *what)
{
    uint32_t bad = 0u;
    int16_t x;
    int16_t y;

    for (y = 0; y < (int16_t)FB_H; y++)
    {
        for (x = 0; x < (int16_t)FB_W; x++)
        {
            if (cgl_rect_contains_point(frame, x, y))
            {
                continue;
            }
            if (PX(x, y) != C_OUT)
            {
                if (bad == 0u)
                {
                    printf("  (first offender of \"%s\": (%d,%d) = 0x%04X, want 0x%04X)\n",
                           what, (int)x, (int)y, (unsigned)PX(x, y), (unsigned)C_OUT);
                }
                bad++;
            }
        }
    }
    return bad;
}

static void bar_reset(void)
{
    cgl_bar_init(&s_bar, "bar", cgl_rect_wh(BAR_X, BAR_Y, BAR_W, BAR_H), C_TRACK, C_FILL);
}

static cgl_rect_t body(void)
{
    return cgl_rect_wh(BAR_X, BAR_Y, BAR_W, BAR_H);
}

/* -------------------------------------------------------------------------- */
/* 1/2. the mapping, the end points and clamping                              */
/* -------------------------------------------------------------------------- */

static void test_endpoints_and_clamping(void)
{
    cgl_rect_t frame = body();

    printf("1) value -> pixels: end points, the halfway point, clamping\n");
    bar_reset();

    scene_begin();
    cgl_bar_set_value(&s_bar, 0);
    render(&s_bar.base);
    CHECK(mismatch(frame, C_TRACK, "0% is all track") == 0u,
          "at 0%% the bar is not entirely the track colour");
    CHECK(outside_bad(frame, "0% stayed inside its frame") == 0u,
          "the bar painted outside its frame");

    scene_begin();
    cgl_bar_set_value(&s_bar, 50);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(10, 10, 109, 19), C_FILL, "50% fills x 10..109") == 0u,
          "at 50%% the first half of the bar is not filled");
    CHECK(mismatch(cgl_rect(110, 10, 209, 19), C_TRACK, "50% leaves x 110..209") == 0u,
          "at 50%% the second half of the bar is not empty");

    scene_begin();
    cgl_bar_set_value(&s_bar, 100);
    render(&s_bar.base);
    CHECK(mismatch(frame, C_FILL, "100% is all fill") == 0u,
          "at 100%% the bar is not entirely filled");
    CHECK(outside_bad(frame, "100% stayed inside its frame") == 0u,
          "the bar painted outside its frame at 100%%");

    /* Out of range: clamped, and the logical value follows the clamp (the old
       teaching example kept `percent` at whatever was passed in and only
       defended inside draw(), so `percent` could read 150). */
    scene_begin();
    cgl_bar_set_value(&s_bar, 150);
    render(&s_bar.base);
    CHECK(mismatch(frame, C_FILL, "150% clamps to all fill") == 0u,
          "a value above the range was not clamped to the top");
    CHECK(cgl_bar_get_value(&s_bar) == 100,
          "the logical value is %ld after passing 150, expected 100",
          (long)cgl_bar_get_value(&s_bar));

    scene_begin();
    cgl_bar_set_value(&s_bar, -7);
    render(&s_bar.base);
    CHECK(mismatch(frame, C_TRACK, "-7 clamps to 0") == 0u,
          "a value below the range was not clamped to the bottom");
    CHECK(cgl_bar_get_value(&s_bar) == 0, "the logical value is not 0 after passing -7");

    /* Round to nearest, not truncate: half way up a 5 px bar must be 3 px (2.5
       rounds up), which is the case a truncating fixed-point multiply gets wrong
       -- and on a bar this short one pixel is 20% of the reading. */
    cgl_bar_init(&s_bar, "small", cgl_rect_wh(10, 10, 5, 4), C_TRACK, C_FILL);
    scene_begin();
    cgl_bar_set_value(&s_bar, 50);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(10, 10, 12, 13), C_FILL, "half of 5 px is 3 px") == 0u,
          "half of a 5 px bar is not 3 px: the fill length is being truncated");
    CHECK(mismatch(cgl_rect(13, 10, 14, 13), C_TRACK, "the other half of 5 px") == 0u,
          "the tail of a 5 px bar at 50%% is not empty");
}

/* -------------------------------------------------------------------------- */
/* 3. it clears its own background                                            */
/* -------------------------------------------------------------------------- */

static void test_self_clearing(void)
{
    cgl_rect_t frame = body();

    printf("2) the widget clears its own background\n");
    bar_reset();

    /* Rising then falling is the case a band renderer cannot fix for you: the
       tail of the 100% frame is only removed if the bar paints its whole frame
       before painting the fill. */
    scene_begin();
    cgl_bar_set_value(&s_bar, 100);
    render(&s_bar.base);
    CHECK(mismatch(frame, C_FILL, "100% before the drop") == 0u, "setup: 100%% did not fill");

    cgl_bar_set_value(&s_bar, 0);
    render(&s_bar.base);
    CHECK(mismatch(frame, C_TRACK, "0% after 100%") == 0u,
          "the tail of the previous 100%% frame survived the drop to 0%%");
    CHECK(outside_bad(frame, "the drop stayed inside the frame") == 0u,
          "the bar painted outside its frame");

    /* ...and a redraw at the same value is idempotent (draw must not depend on
       state it does not own). */
    scene_begin();
    cgl_bar_set_value(&s_bar, 30);
    render(&s_bar.base);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(10, 10, 69, 19), C_FILL, "30% twice, first 60 px") == 0u,
          "a repeated draw changed the filled part");
    CHECK(mismatch(cgl_rect(70, 10, 209, 19), C_TRACK, "30% twice, the rest") == 0u,
          "a repeated draw changed the empty part");
}

/* -------------------------------------------------------------------------- */
/* 4. a signed range, and percent on top of it                                */
/* -------------------------------------------------------------------------- */

static void test_signed_range_and_percent(void)
{
    printf("3) a signed range (-50..50) and the percent mapping on top of it\n");
    bar_reset();
    CHECK(cgl_bar_set_range(&s_bar, -50, 50) == true, "a valid range was rejected");

    scene_begin();
    cgl_bar_set_value(&s_bar, 0);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(10, 10, 109, 19), C_FILL, "0 of -50..50 is half") == 0u,
          "the midpoint of a signed range is not half filled");
    CHECK(mismatch(cgl_rect(110, 10, 209, 19), C_TRACK, "0 of -50..50, second half") == 0u,
          "the midpoint of a signed range overfills");

    scene_begin();
    cgl_bar_set_value(&s_bar, -25);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(10, 10, 59, 19), C_FILL, "-25 of -50..50 is a quarter") == 0u,
          "a quarter of a signed range is not 50 px");
    CHECK(mismatch(cgl_rect(60, 10, 209, 19), C_TRACK, "-25 of -50..50, the rest") == 0u,
          "a quarter of a signed range overfills");

    scene_begin();
    cgl_bar_set_value(&s_bar, -50);
    render(&s_bar.base);
    CHECK(mismatch(body(), C_TRACK, "-50 is the bottom") == 0u,
          "the low end of a signed range is not empty");

    scene_begin();
    cgl_bar_set_value(&s_bar, 50);
    render(&s_bar.base);
    CHECK(mismatch(body(), C_FILL, "50 is the top") == 0u,
          "the high end of a signed range is not full");

    cgl_bar_set_value(&s_bar, 0);
    CHECK(cgl_bar_get_percent(&s_bar) == 50u, "0 of -50..50 reads as %u%%",
          (unsigned)cgl_bar_get_percent(&s_bar));
    cgl_bar_set_value(&s_bar, -25);
    CHECK(cgl_bar_get_percent(&s_bar) == 25u, "-25 of -50..50 reads as %u%%",
          (unsigned)cgl_bar_get_percent(&s_bar));
    cgl_bar_set_value(&s_bar, 25);
    CHECK(cgl_bar_get_percent(&s_bar) == 75u, "25 of -50..50 reads as %u%%",
          (unsigned)cgl_bar_get_percent(&s_bar));

    cgl_bar_set_percent(&s_bar, 25u);
    CHECK(cgl_bar_get_value(&s_bar) == -25, "25%% of -50..50 is %ld, expected -25",
          (long)cgl_bar_get_value(&s_bar));
    cgl_bar_set_percent(&s_bar, 0u);
    CHECK(cgl_bar_get_value(&s_bar) == -50, "0%% of -50..50 is not the bottom");
    cgl_bar_set_percent(&s_bar, 200u);
    CHECK(cgl_bar_get_value(&s_bar) == 50, "a percent above 100 was not clamped");

    /* The mapping is exact for a range that does not divide by 100 (255 is the
       classic 8-bit case: 50% is 127.5, which must not lose or gain a pixel). */
    CHECK(cgl_bar_set_range(&s_bar, 0, 255) == true, "range 0..255 rejected");
    cgl_bar_set_percent(&s_bar, 50u);
    CHECK(cgl_bar_get_value(&s_bar) == 127, "50%% of 0..255 is %ld, expected 127",
          (long)cgl_bar_get_value(&s_bar));
    cgl_bar_set_percent(&s_bar, 100u);
    CHECK(cgl_bar_get_value(&s_bar) == 255, "100%% of 0..255 is not the top");

    /* A degenerate or inverted range is rejected, counted and does not disturb
       the range in use. */
    {
        uint32_t before = cgl_bar_range_rejected();
        CHECK(cgl_bar_set_range(&s_bar, 10, 10) == false, "a zero-width range was accepted");
        CHECK(cgl_bar_set_range(&s_bar, 10, 9) == false, "an inverted range was accepted");
        CHECK(cgl_bar_range_rejected() == (before + 2u),
              "the two rejections were not counted (%u -> %u)", (unsigned)before,
              (unsigned)cgl_bar_range_rejected());
        CHECK(s_bar.vmax == 255, "the rejected calls changed the range in use");
    }
}

/* -------------------------------------------------------------------------- */
/* 5. the full int32 domain                                                   */
/* -------------------------------------------------------------------------- */

static void test_wide_range(void)
{
    printf("4) ranges wide enough to overflow the naive mapping\n");
    bar_reset();

    /* > 2^23: `(off << 8)` would already overflow here. */
    CHECK(cgl_bar_set_range(&s_bar, 0, 16777217) == true, "range 0..2^24+1 rejected");
    scene_begin();
    cgl_bar_set_value(&s_bar, 8388608);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(10, 10, 109, 19), C_FILL, "half of 0..2^24+1") == 0u,
          "half of a range wider than 2^23 is not half filled");
    CHECK(mismatch(cgl_rect(110, 10, 209, 19), C_TRACK, "half of 0..2^24+1, rest") == 0u,
          "half of a range wider than 2^23 overfills");

    /* The whole int32 domain: `vmax - vmin` is not representable in int32_t, so
       this is also the case that a signed implementation gets wrong by UB. */
    CHECK(cgl_bar_set_range(&s_bar, INT32_MIN, INT32_MAX) == true,
          "the full int32 range was rejected");
    scene_begin();
    cgl_bar_set_value(&s_bar, 0);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(10, 10, 109, 19), C_FILL, "0 of INT32_MIN..INT32_MAX") == 0u,
          "the midpoint of the full int32 range is not half filled");
    CHECK(mismatch(cgl_rect(110, 10, 209, 19), C_TRACK, "the rest of the full range") == 0u,
          "the midpoint of the full int32 range overfills");
    CHECK(cgl_bar_get_percent(&s_bar) == 50u, "0 of the full range reads as %u%%",
          (unsigned)cgl_bar_get_percent(&s_bar));

    scene_begin();
    cgl_bar_set_value(&s_bar, INT32_MIN);
    render(&s_bar.base);
    CHECK(mismatch(body(), C_TRACK, "INT32_MIN is the bottom") == 0u,
          "the lowest value of the full range is not empty");

    scene_begin();
    cgl_bar_set_value(&s_bar, INT32_MAX);
    render(&s_bar.base);
    CHECK(mismatch(body(), C_FILL, "INT32_MAX is the top") == 0u,
          "the highest value of the full range is not full");
}

/* -------------------------------------------------------------------------- */
/* 6. orientation and direction                                               */
/* -------------------------------------------------------------------------- */

static void test_orientation(void)
{
    printf("5) vertical, and both directions on both axes\n");
    bar_reset();

    /* Vertical level meter: a 20 x 100 body at (10, 10). */
    cgl_widget_set_frame(&s_bar.base, cgl_rect_wh(10, 10, 20, 100));
    cgl_bar_set_orient(&s_bar, CGL_BAR_VERTICAL);

    scene_begin();
    cgl_bar_set_value(&s_bar, 50);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(10, 60, 29, 109), C_FILL, "vertical 50% fills the bottom half") == 0u,
          "a vertical bar does not fill upwards from the bottom edge");
    CHECK(mismatch(cgl_rect(10, 10, 29, 59), C_TRACK, "vertical 50%, upper half") == 0u,
          "a vertical bar fills downwards");

    cgl_bar_set_reverse(&s_bar, true);
    scene_begin();
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(10, 10, 29, 59), C_FILL, "reversed vertical 50%") == 0u,
          "a reversed vertical bar does not fill downwards from the top edge");
    CHECK(mismatch(cgl_rect(10, 60, 29, 109), C_TRACK, "reversed vertical 50%, rest") == 0u,
          "a reversed vertical bar fills upwards");

    scene_begin();
    cgl_bar_set_value(&s_bar, 0);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(10, 10, 29, 109), C_TRACK, "vertical 0%") == 0u,
          "a vertical bar at the bottom of its range is not empty");
    scene_begin();
    cgl_bar_set_value(&s_bar, 100);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(10, 10, 29, 109), C_FILL, "vertical 100%") == 0u,
          "a vertical bar at the top of its range is not full");

    /* Back to horizontal, reversed: the fill hangs off the right edge. */
    cgl_bar_set_orient(&s_bar, CGL_BAR_HORIZONTAL);
    cgl_widget_set_frame(&s_bar.base, body());
    cgl_bar_set_reverse(&s_bar, true);

    scene_begin();
    cgl_bar_set_value(&s_bar, 50);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(110, 10, 209, 19), C_FILL, "reversed 50% fills from the right") == 0u,
          "a reversed horizontal bar does not fill from the right edge");
    CHECK(mismatch(cgl_rect(10, 10, 109, 19), C_TRACK, "reversed 50%, left part") == 0u,
          "a reversed horizontal bar fills from the left");

    scene_begin();
    cgl_bar_set_value(&s_bar, 25);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(160, 10, 209, 19), C_FILL, "reversed 25% is the right quarter") == 0u,
          "a reversed bar at 25%% is not the right-hand quarter");

    cgl_bar_set_reverse(&s_bar, false);
}

/* -------------------------------------------------------------------------- */
/* 7. border                                                                  */
/* -------------------------------------------------------------------------- */

static void test_border(void)
{
    printf("6) the border ring (and the clamp that keeps a bar inside it)\n");
    bar_reset();
    cgl_bar_set_border(&s_bar, C_BORD, 2u);

    scene_begin();
    cgl_bar_set_value(&s_bar, 100);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(10, 10, 209, 11), C_BORD, "top of the ring") == 0u, "top ring missing");
    CHECK(mismatch(cgl_rect(10, 18, 209, 19), C_BORD, "bottom of the ring") == 0u, "bottom ring missing");
    CHECK(mismatch(cgl_rect(10, 12, 11, 17), C_BORD, "left of the ring") == 0u, "left ring missing");
    CHECK(mismatch(cgl_rect(208, 12, 209, 17), C_BORD, "right of the ring") == 0u, "right ring missing");
    CHECK(mismatch(cgl_rect(12, 12, 207, 17), C_FILL, "the fill inside the ring") == 0u,
          "the fill does not reach the inner edge of the ring");

    scene_begin();
    cgl_bar_set_value(&s_bar, 0);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(12, 12, 207, 17), C_TRACK, "the track inside the ring") == 0u,
          "the empty bar inside the ring is not the track");

    /* The border is the padding, and the fill is measured inside it: 196 px of
       inner width at 50% is 98 px, not 100. */
    scene_begin();
    cgl_bar_set_value(&s_bar, 50);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(12, 12, 109, 17), C_FILL, "50% inside a 2 px ring") == 0u,
          "the fill of a bordered bar is being measured against the outer frame");
    CHECK(mismatch(cgl_rect(110, 12, 207, 17), C_TRACK, "50% inside a 2 px ring, rest") == 0u,
          "the empty part inside a bordered bar is wrong");

    /* A border wider than the bar is clamped so that at least one pixel line of
       bar survives: on a 10 px tall bar the ring stops at 4. */
    cgl_bar_set_border(&s_bar, C_BORD, 200u);
    CHECK(s_bar.border_w == 4u, "a 200 px border on a 10 px bar was clamped to %u, expected 4",
          (unsigned)s_bar.border_w);
    scene_begin();
    cgl_bar_set_value(&s_bar, 100);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(10, 10, 209, 13), C_BORD, "4 px ring, top") == 0u, "clamped ring top wrong");
    CHECK(mismatch(cgl_rect(10, 16, 209, 19), C_BORD, "4 px ring, bottom") == 0u, "clamped ring bottom wrong");
    CHECK(mismatch(cgl_rect(14, 14, 205, 15), C_FILL, "the 2 px line left inside") == 0u,
          "the clamped ring left nothing (or too much) to fill");

    /* A 2 px bar cannot hold a ring at all: 0 is the honest answer. */
    cgl_widget_set_frame(&s_bar.base, cgl_rect_wh(10, 10, 20, 2));
    cgl_bar_set_border(&s_bar, C_BORD, 1u);
    CHECK(s_bar.border_w == 0u, "a border was kept on a 2 px bar (%u)", (unsigned)s_bar.border_w);

    /* The clamp is a promise about the frame the setter was called with. If the
       frame shrinks afterwards the inner rectangle can vanish -- and the bar is
       then an all-border one, not a bar that quietly lost its ring. */
    cgl_widget_set_frame(&s_bar.base, body());
    cgl_bar_set_border(&s_bar, C_BORD, 5u);                        /* 4 on a 10 px tall frame */
    cgl_widget_set_frame(&s_bar.base, cgl_rect_wh(10, 10, 4, 4));  /* 4 x 4: nothing left inside */
    scene_begin();
    cgl_bar_set_value(&s_bar, 100);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(10, 10, 13, 13), C_BORD, "a frame shrunk below its border") == 0u,
          "a frame that shrank below the border is neither all-border nor fillable");

    cgl_widget_set_frame(&s_bar.base, body());
    cgl_bar_set_border(&s_bar, C_BORD, 0u);
}

/* -------------------------------------------------------------------------- */
/* 8. segments                                                                */
/* -------------------------------------------------------------------------- */

static void test_segments(void)
{
    printf("7) segment separators\n");
    bar_reset();
    cgl_bar_set_segments(&s_bar, 4u, C_SEG);

    scene_begin();
    cgl_bar_set_value(&s_bar, 100);
    render(&s_bar.base);
    /* The separators sit at the start of slots 2, 3 and 4: x = 10 + 50, 100, 150. */
    CHECK(mismatch(cgl_rect(60, 10, 60, 19), C_SEG, "separator 1") == 0u, "first separator missing");
    CHECK(mismatch(cgl_rect(110, 10, 110, 19), C_SEG, "separator 2") == 0u, "second separator missing");
    CHECK(mismatch(cgl_rect(160, 10, 160, 19), C_SEG, "separator 3") == 0u, "third separator missing");
    CHECK(mismatch(cgl_rect(10, 10, 59, 19), C_FILL, "slot 1") == 0u, "slot 1 is not filled");
    CHECK(mismatch(cgl_rect(61, 10, 109, 19), C_FILL, "slot 2") == 0u, "slot 2 is not filled");
    CHECK(mismatch(cgl_rect(161, 10, 209, 19), C_FILL, "slot 4") == 0u, "slot 4 is not filled");

    /* Separators run across the whole bar, filled or not: that is what makes the
       picture read as "N slots, k of them filled". */
    scene_begin();
    cgl_bar_set_value(&s_bar, 50);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(110, 10, 110, 19), C_SEG, "separator over the track") == 0u,
          "a separator over the empty part of the bar vanished");
    CHECK(mismatch(cgl_rect(60, 10, 60, 19), C_SEG, "separator over the fill") == 0u,
          "a separator over the filled part of the bar vanished");

    /* `seg == track` is the documented second look: the fill is cut into blocks
       and the slots the value has not reached stay blank. */
    cgl_bar_set_segments(&s_bar, 4u, C_TRACK);
    scene_begin();
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(60, 10, 60, 19), C_TRACK, "block gap inside the fill") == 0u,
          "seg == track did not cut the fill into blocks");
    CHECK(mismatch(cgl_rect(110, 10, 110, 19), C_TRACK, "no gap over the track") == 0u,
          "seg == track drew something over the empty part");

    /* Back to one continuous bar. */
    cgl_bar_set_segments(&s_bar, 0u, C_SEG);
    CHECK(s_bar.segments == 1u, "segments(0) did not fall back to one continuous bar");
    scene_begin();
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(60, 10, 60, 19), C_FILL, "no separator at 1 segment") == 0u,
          "a separator survived segments == 1");

    /* A bar shorter than its segment count must not draw 254 separators over the
       same column, and must still show one separator per pixel of length. */
    cgl_widget_set_frame(&s_bar.base, cgl_rect_wh(10, 10, 4, 4));
    cgl_bar_set_segments(&s_bar, 200u, C_SEG);
    scene_begin();
    cgl_bar_set_value(&s_bar, 100);
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(11, 10, 13, 13), C_SEG, "a 4 px bar with 200 segments") == 0u,
          "a 4 px bar with 200 segments did not come out as one separator per column");
    CHECK(mismatch(cgl_rect(10, 10, 10, 13), C_FILL, "the first column of the 4 px bar") == 0u,
          "the first slot of a 4 px bar was overwritten by a separator");
    cgl_widget_set_frame(&s_bar.base, body());
    cgl_bar_set_segments(&s_bar, 1u, C_SEG);
}

/* -------------------------------------------------------------------------- */
/* 9. damage discipline                                                       */
/* -------------------------------------------------------------------------- */

static uint32_t s_damage_calls;

static void damage__sink(cgl_rect_t r, void *user)
{
    (void)r;
    (void)user;
    s_damage_calls++;
}

static void test_damage(void)
{
    uint32_t n;

    printf("8) a setter damages only when the drawn pixels can change\n");
    cgl_widget_set_damage_sink(damage__sink, NULL);
    bar_reset();
    cgl_bar_set_value(&s_bar, 42); /* reach a known state, ignore the damage */

    n = s_damage_calls;
    cgl_bar_set_value(&s_bar, 42);
    CHECK(s_damage_calls == n, "set_value() with the same value marked damage");

    cgl_bar_set_value(&s_bar, 43);
    CHECK(s_damage_calls == (n + 1u), "set_value() with a new value did not mark damage");

    n = s_damage_calls;
    cgl_bar_set_value(&s_bar, 1000); /* clamps to 100 */
    cgl_bar_set_value(&s_bar, 100);
    CHECK(s_damage_calls == (n + 1u),
          "a clamped value plus the value it clamps to marked %u damages, expected 1",
          (unsigned)(s_damage_calls - n));

    n = s_damage_calls;
    cgl_bar_set_percent(&s_bar, 100u); /* 100% == value 100: already there */
    CHECK(s_damage_calls == n, "set_percent() with a value that maps to the current one marked damage");

    n = s_damage_calls;
    CHECK(cgl_bar_set_range(&s_bar, 0, 100) == true, "re-setting the same range failed");
    CHECK(s_damage_calls == n, "re-setting the same range marked damage");

    n = s_damage_calls;
    CHECK(cgl_bar_set_range(&s_bar, 0, 200) == true, "widening the range failed");
    CHECK(s_damage_calls == (n + 1u),
          "a range change that moves the fill did not mark damage (the same value draws "
          "somewhere else once the range changes)");

    /* The configuration setters compare too, so a UI that re-applies its theme
       every frame does not repaint every frame. */
    n = s_damage_calls;
    cgl_bar_set_orient(&s_bar, CGL_BAR_HORIZONTAL);
    cgl_bar_set_reverse(&s_bar, false);
    cgl_bar_set_colors(&s_bar, C_TRACK, C_FILL);
    cgl_bar_set_border(&s_bar, C_TRACK, 0u);
    cgl_bar_set_segments(&s_bar, 1u, C_TRACK);
    CHECK(s_damage_calls == n, "re-applying the current configuration marked %u damages, expected 0",
          (unsigned)(s_damage_calls - n));

    n = s_damage_calls;
    cgl_bar_set_orient(&s_bar, CGL_BAR_VERTICAL);
    cgl_bar_set_reverse(&s_bar, true);
    cgl_bar_set_colors(&s_bar, C_BORD, C_FILL);
    cgl_bar_set_border(&s_bar, C_BORD, 1u);
    cgl_bar_set_segments(&s_bar, 3u, C_SEG);
    CHECK(s_damage_calls == (n + 5u), "the five real configuration changes marked %u damages, expected 5",
          (unsigned)(s_damage_calls - n));

    cgl_widget_set_damage_sink(NULL, NULL);
}

/* -------------------------------------------------------------------------- */
/* 10. animation                                                              */
/* -------------------------------------------------------------------------- */

static void test_animation(void)
{
    printf("9) cgl_bar_animate_to() is driven by update()\n");
    bar_reset();
    cgl_widget_set_damage_sink(NULL, NULL);

    CHECK(cgl_bar_animate_to(&s_bar, 100, 100u, CGL_EASE_LINEAR) == true, "the tween did not start");
    CHECK(cgl_bar_get_value(&s_bar) == 100, "the logical value did not move to the target at once");
    CHECK(cgl_bar_get_shown_value(&s_bar) == 0, "the drawn value jumped instead of tweening");
    CHECK(cgl_bar_is_animating(&s_bar) == true, "the bar does not report an animation in flight");

    /* Drawing must not advance it: the tween belongs to update(), which is step 6
       of a frame and is skipped for a widget that is not visible. */
    render(&s_bar.base);
    CHECK(cgl_bar_get_shown_value(&s_bar) == 0, "drawing advanced the animation");

    cgl_widget_update_tree(&s_bar.base, 50u);
    CHECK(cgl_bar_get_shown_value(&s_bar) == 50, "after 50 of 100 ms LINEAR the drawn value is %ld, expected 50",
          (long)cgl_bar_get_shown_value(&s_bar));
    CHECK(cgl_bar_get_value(&s_bar) == 100, "the logical value drifted during the animation");
    CHECK(cgl_bar_is_animating(&s_bar) == true, "the animation ended half way");

    scene_begin();
    render(&s_bar.base);
    CHECK(mismatch(cgl_rect(10, 10, 109, 19), C_FILL, "the fill at half of the tween") == 0u,
          "the fill does not follow the drawn (not the logical) value");

    cgl_widget_update_tree(&s_bar.base, 50u);
    CHECK(cgl_bar_get_shown_value(&s_bar) == 100, "the tween did not land exactly on the target");
    CHECK(cgl_bar_is_animating(&s_bar) == false, "the tween still reports running after it landed");

    /* Retargeting mid-flight continues from what is drawn. */
    cgl_bar_set_value(&s_bar, 0);
    CHECK(cgl_bar_animate_to(&s_bar, 100, 100u, CGL_EASE_LINEAR) == true, "the second tween did not start");
    cgl_widget_update_tree(&s_bar.base, 30u);
    CHECK(cgl_bar_get_shown_value(&s_bar) == 29, "after 30 ms the drawn value is %ld, expected 29",
          (long)cgl_bar_get_shown_value(&s_bar));
    CHECK(cgl_bar_animate_to(&s_bar, 20, 100u, CGL_EASE_LINEAR) == true,
          "retargeting mid-flight did not start a tween");
    cgl_widget_update_tree(&s_bar.base, 50u);
    CHECK(cgl_bar_get_shown_value(&s_bar) == 25,
          "retargeting restarted from the old target instead of the drawn value (%ld, expected 25)",
          (long)cgl_bar_get_shown_value(&s_bar));
    cgl_widget_update_tree(&s_bar.base, 100u);
    CHECK(cgl_bar_get_shown_value(&s_bar) == 20, "the retargeted tween did not land on 20");

    /* dur_ms == 0 is "no animation". */
    cgl_bar_set_value(&s_bar, 0);
    CHECK(cgl_bar_animate_to(&s_bar, 70, 0u, CGL_EASE_LINEAR) == false, "dur_ms 0 reported a tween");
    CHECK(cgl_bar_get_shown_value(&s_bar) == 70, "dur_ms 0 did not move the drawn value");
    CHECK(cgl_bar_is_animating(&s_bar) == false, "dur_ms 0 left an animation running");

    /* Already there: nothing to start. */
    CHECK(cgl_bar_animate_to(&s_bar, 70, 100u, CGL_EASE_LINEAR) == false,
          "animating to the value already drawn reported a tween");
    CHECK(cgl_bar_is_animating(&s_bar) == false, "animating to the current value started something");

    /* A target out of range is clamped, and the tween still lands exactly. */
    CHECK(cgl_bar_animate_to(&s_bar, 5000, 40u, CGL_EASE_OUT_CUBIC) == true, "the clamped tween did not start");
    CHECK(cgl_bar_get_value(&s_bar) == 100, "the clamped target is not the top of the range");
    cgl_widget_update_tree(&s_bar.base, 100u);
    CHECK(cgl_bar_get_shown_value(&s_bar) == 100, "the clamped tween did not land on the top");
}

/* -------------------------------------------------------------------------- */
/* 11. the snapshot gallery                                                   */
/* -------------------------------------------------------------------------- */

#define GALLERY_N 9
static cgl_bar_t s_gallery[GALLERY_N];

static void gallery_build(void)
{
    uint8_t i;

    for (i = 0u; i < GALLERY_N; i++)
    {
        cgl_bar_init(&s_gallery[i], "gallery", cgl_rect_wh(0, 0, 1, 1), C_TRACK, C_FILL);
    }

    /* Four horizontal reads of the same value across different configurations. */
    cgl_widget_set_frame(&s_gallery[0].base, cgl_rect_wh(10, 6, 300, 12));
    cgl_bar_set_value(&s_gallery[0], 65);

    cgl_widget_set_frame(&s_gallery[1].base, cgl_rect_wh(10, 22, 300, 12));
    (void)cgl_bar_set_range(&s_gallery[1], -50, 50);
    cgl_bar_set_value(&s_gallery[1], -10);

    cgl_widget_set_frame(&s_gallery[2].base, cgl_rect_wh(10, 38, 300, 12));
    cgl_bar_set_border(&s_gallery[2], C_BORD, 2u);
    cgl_bar_set_segments(&s_gallery[2], 4u, C_BORD);
    cgl_bar_set_value(&s_gallery[2], 40);

    cgl_widget_set_frame(&s_gallery[3].base, cgl_rect_wh(10, 54, 300, 12));
    cgl_bar_set_reverse(&s_gallery[3], true);
    cgl_bar_set_value(&s_gallery[3], 65);

    /* Five vertical level meters, straight up, at 0/25/50/75/100%. */
    for (i = 0u; i < 5u; i++)
    {
        cgl_bar_t *b = &s_gallery[4u + i];

        cgl_widget_set_frame(&b->base, cgl_rect_wh((int16_t)(10 + (i * 26)), 72, 20, 88));
        cgl_bar_set_orient(b, CGL_BAR_VERTICAL);
        cgl_bar_set_border(b, C_BORD, 1u);
        cgl_bar_set_segments(b, 5u, C_BORD);
        cgl_bar_set_value(b, (int32_t)(i * 25u));
    }
}

static void gallery_dump(void)
{
    cgl_canvas_t c;
    uint8_t i;

    cgl_canvas_init(&c, s_fb, (int16_t)FB_W, (int16_t)FB_H, 0);
    cgl_canvas_clear(&c, CGL_CFG_BG_COLOR);
    for (i = 0u; i < GALLERY_N; i++)
    {
        cgl_widget_draw_tree(&s_gallery[i].base, &c);
    }
    img_dump_ppm("build/bar_test.ppm", 0, s_fb, (int)FB_W, (int)FB_H);
    img_dump_bmp("build/bar_test.bmp", 0, s_fb, (int)FB_W, (int)FB_H);
}

/* -------------------------------------------------------------------------- */

int main(void)
{
    printf("cgl_bar: value mapping, orientation, border, segments, damage, tween\n\n");

    test_endpoints_and_clamping();
    test_self_clearing();
    test_signed_range_and_percent();
    test_wide_range();
    test_orientation();
    test_border();
    test_segments();
    test_damage();
    test_animation();

    gallery_build();
    gallery_dump();
    printf("\nwrote build/bar_test.ppm / build/bar_test.bmp (gallery)\n");

    if (s_fails == 0)
    {
        printf("all passed.\n");
        return 0;
    }
    printf("%d failed.\n", s_fails);
    return 1;
}
