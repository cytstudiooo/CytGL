// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    clock_frames.c
  * @brief   Render the watch face page to PPM/BMP snapshots (host only).
  *
  *  Same mock-panel trick as `render_host_test.c` / `bullet_frames.c`, for the
  *  clock page: it drives **real frames** through the **real page** (the real band
  *  walker, the real damage tracking, the real tweens) and dumps the moments that
  *  matter, so the face can be looked at without hardware.
  *
  *      clock_frames [outdir]        default "build"
  *
  *  Snapshots:
  *      00..02   the entrance reveal at ~0 %, 25 % and 60 %: the hands sweeping up
  *               from 12:00 and the chapter ring being drawn mark by mark
  *      03       the money shot: 10:09:36, the pose every watch is photographed in
  *      04..05   half a minute later, so the comet trail behind the second hand is
  *               at its longest
  *      06       the minute rollover, caught mid-roll (the digital digits moving)
  *      07..09   other times of day, because "the hands point where they should" is
  *               easiest to check against a clock you can read
  *      10       the complication gauge on its own second, close to a minute
  *
  *  A picture cannot assert geometry, and this file does not try to: it prints the
  *  time and the reveal next to every file so a human (or a diff) can tie the image
  *  to the state. The geometry assertions live in `clock_host_test.c`.
  ******************************************************************************
  */
#include <stdio.h>
#include <string.h>

#include <cytgl/cgl_display.h>
#include <cytgl/cgl_screen.h>
#include <cytgl/cgl_page.h>

#include "app_page_clock.h"
#include "test_image_out.h"

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

static int s_shot;
static const char *s_dir = "build";

static void dump(const char *tag)
{
    const clock_time_t *t = app_page_clock_now(&s_clock);
    char path[256];

    (void)snprintf(path, sizeof(path), "%s/clock%02d.ppm", s_dir, s_shot);
    img_dump_ppm(path, s_shot, s_framebuffer, CGL_CFG_SCREEN_W, CGL_CFG_SCREEN_H);
    (void)snprintf(path, sizeof(path), "%s/clock%02d.bmp", s_dir, s_shot);
    img_dump_bmp(path, s_shot, s_framebuffer, CGL_CFG_SCREEN_W, CGL_CFG_SCREEN_H);

    (void)printf("  clock%02d  %-44s %02u:%02u:%02u reveal=%3u frames=%u\n",
                 s_shot, tag, (unsigned)t->h, (unsigned)t->m, (unsigned)t->s,
                 (unsigned)app_page_clock_reveal(&s_clock),
                 (unsigned)app_page_clock_frames(&s_clock));
    s_shot++;
}

/** Drive n frames at the firmware cadence (16 ms, CGL_CFG_CLOCK_STEP_MS). */
static void run(int n)
{
    int i;

    for (i = 0; i < n; i++)
    {
        cgl_screen_frame(&s_screen, 16u);
    }
}

int main(int argc, char **argv)
{
    int i;
    int got25 = 0;
    int got60 = 0;
    int got_full = 0;
    int minute_seen = 0;

    if (argc > 1)
    {
        s_dir = argv[1];
    }

    if (cgl_display_init(&s_display, &s_mock_drv, NULL) != 0)
    {
        return 1;
    }
    cgl_page_nav_init(&s_nav, "frames",
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
    if (cgl_screen_init(&s_screen, &s_display, &s_nav) != 0)
    {
        return 1;
    }
    /* Anchor at 10:09:36 before the first frame: the classic watch pose (the hour
       hand at 10 and the minute hand at 9 frame the upper half of the dial
       symmetrically), and a second that is not zero so the seconds ring is real.
       set_time parks the request and the first frame applies it, which is why this
       is legal before any frame has run. */
    app_page_clock_set_time(&s_clock, 10u, 9u, 36u);
    (void)cgl_page_nav_reset(&s_nav, &s_clock.base);
    cgl_screen_frame(&s_screen, 0u);

    /* --- the entrance ----------------------------------------------------- */
    dump("reveal 0 % (the face is up, the hands are still at 12:00)");

    for (i = 0; i < 120; i++)
    {
        cgl_screen_frame(&s_screen, 16u);
        if (!got25 && (app_page_clock_reveal(&s_clock) >= 64u))
        {
            got25 = 1;
            dump("reveal ~25 % (hands sweeping, ring filling, marks appearing)");
        }
        if (!got60 && (app_page_clock_reveal(&s_clock) >= 154u))
        {
            got60 = 1;
            dump("reveal ~60 %");
        }
        if (!got_full && (app_page_clock_reveal(&s_clock) >= 256u))
        {
            got_full = 1;
            dump("10:09:36 complete -- the pose every watch is photographed in");
        }
    }

    /* --- half a minute later: the comet trail is at its longest ----------- */
    run(30 * 1000 / 16);
    dump("30 s later: the comet trail behind the second hand");

    /* --- the minute rollover, caught in the middle of the digit roll ------ */
    app_page_clock_set_time(&s_clock, 10u, 9u, 57u);
    run(4);
    dump("reset to 10:09:57, just before the minute turns");
    minute_seen = app_page_clock_now(&s_clock)->m;
    for (i = 0; i < 400; i++)
    {
        cgl_screen_frame(&s_screen, 16u);
        if (app_page_clock_now(&s_clock)->m != (uint8_t)minute_seen)
        {
            /* Six frames is about 100 ms into a 300 ms roll: the outgoing digit is
               a third of the way out of its cell. */
            run(6);
            break;
        }
    }
    dump("mid-roll: the digital digits are moving to 10:10");

    /* --- other times of day ----------------------------------------------- */
    { /* 03:45 -- hour and minute hands in the lower half, a good legibility test */
        app_page_clock_set_time(&s_clock, 3u, 45u, 0u);
        run(3);
        dump("03:45");
    }
    { /* 12:00 -- both hands straight up, where a rounding bug shows worst */
        app_page_clock_set_time(&s_clock, 12u, 0u, 0u);
        run(3);
        dump("12:00:00");
    }
    { /* 18:30 -- the hour hand between 6 and 7, the gauge near the half minute */
        app_page_clock_set_time(&s_clock, 18u, 30u, 29u);
        run(3);
        dump("18:30:29");
    }

#if CLOCK_SECOND_MODE == CLOCK_SECOND_TICK
    /* --- the mechanical tick, step by step ---------------------------------
     * Only in a TICK build (`-DCLOCK_SECOND_MODE=CLOCK_SECOND_TICK`), because in a sweep
     * build there is no step to photograph. Four frames across one snap: parked at :20.4,
     * then twice through the `CLOCK_TICK_MS` whip, and the mark it lands on at :21.0. It
     * is the sequence, not any one frame, that says whether the motion reads as a
     * mechanical second hand.
     *
     * WARNING: the times are **even** milliseconds on purpose. The clock advances by the
     * frame step and `now.ms` is `(now_ms - t0_ms) % 1000`, so on a 16 ms cadence only
     * multiples of 16 are ever reached, and a target of 920 with no matching frame would
     * leave this loop running to its guard (or, worse, past the second it means to
     * photograph -- which is what the first version of this section did, dumping the same
     * "landed" picture three times). */
    app_page_clock_set_time(&s_clock, 12u, 0u, 20u);
    run(1);
    {
        static const uint16_t at_ms[4] = { 400u, 928u, 992u, 0u };
        static const char *const what[4] = {
            "tick :20.4 -- parked on the mark",
            "tick: the second hand mid-snap",
            "tick: the second hand nearly there",
            "tick :21.0 -- landed on the mark"
        };
        int kk;

        for (kk = 0; kk < 4; kk++)
        {
            int guard;
            bool last = (at_ms[kk] == 0u);

            for (guard = 0; guard < 90; guard++)
            {
                const clock_time_t *t = app_page_clock_now(&s_clock);

                if ((t->s != 20u) && (last || (t->ms == at_ms[kk])))
                {
                    break; /* reached the next second (and, for the last, its mark) */
                }
                if (!last && (t->ms == at_ms[kk]))
                {
                    break;
                }
                run(1);
            }
            dump(what[kk]);
        }
    }
#endif

    (void)printf("\nwrote %d clock snapshots to %s/ (clock00.ppm ... clock%02d.bmp)\n",
                 s_shot, s_dir, s_shot - 1);
    return 0;
}
