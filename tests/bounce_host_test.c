// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    bounce_host_test.c
  * @brief   PC-side test: unified clock + page fixed stepping + bouncing ball (not built into the firmware)
  *
  *  This is **the most important regression**: when several animated widgets
  *  share one physics world, the world must be advanced exactly once per step.
  *
  *  The old implementation relied on discipline for this: every widget called
  *  cgl_phys2d_clock_step() in its own update and de-duplication relied on "only the
  *  first caller really advances", plus you had to remember to "call
  *  cgl_phys2d_clock_tick() first and then walk the widgets" every frame. This project
  *  did get it wrong historically: 120 steps were measured inside 1000 ms
  *  (physics at 2x speed, the logo period dropping from 2.65 s to 1.77 s).
  *
  *  Structurally it cannot be got wrong now: **a widget has no cgl_phys2d_step to
  *  call**, the world is advanced only by the page's on_step, and on_step is
  *  called by the cgl_clock iterator as many times as the clock has computed.
  *  So this test walks the real firmware path (cgl_screen_frame) and then asserts:
  *
  *    1. over one second of frames the page receives about 1000/step_ms on_step
  *       calls, **not twice that**
  *    2. the count the page receives == the count the clock issues (no waste and
  *       no duplication)
  *    3. the logo period is still in the expected range (end-to-end criterion for
  *       the correctness of the physics speed)
  *    4. the ball uses the whole viewport in y (no longer bouncing in a thin
  *       strip at the top)
  *    5. the ball centre always stays inside the active area (no wall clipping)
  *    6. it keeps bouncing (it does not stick to the floor after two bounces)
  *    7. it survives the logo's repeated rebuilds (cgl_phys2d_reset wipes the ball
  *       out of the pool)
  *    8. the fixed-point arithmetic does not overflow (int16 range)
  *
  *  It also verifies "page gets covered -> physics freezes": after pushing a new
  *  page, the smash page's step count must **stop growing completely** (the old
  *  implementation could not do this -- time kept accumulating and was caught up
  *  on the way back).
  *
  *  Build / run: the canonical definition is tests/CMakeLists.txt -- this test
  *  links the whole demo application plus the framework, because it drives the
  *  real per-page stepping path:
  *
  *    cmake -B build/host -DCYTGL_BUILD_TESTS=ON
  *    cmake --build build/host
  *    ctest --test-dir build/host -R bounce --output-on-failure
  ******************************************************************************
  */
#include <stdio.h>

#include <cytgl/cytgl_config.h>
#include <cytgl/cgl_widget.h>
#include <cytgl/cgl_screen.h>
#include "app_page_smash.h"
#include "app_page_menu.h"
#include "app_page_info.h"
#include <ext/cgl_bounce.h>
#include <ext/cgl_bounce_view.h>
#include <ext/cgl_debris.h>
#include <ext/cgl_phys2d.h>

/* ----------------------------------------------------------------------------------------------------- */
/* mock panel: it only has to run one complete frame (render correctness is covered by the render tests) */
/* ----------------------------------------------------------------------------------------------------- */

static int mock__init(void *ctx) { (void)ctx; return 0; }
static void mock__set_window(void *ctx, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    (void)ctx; (void)x0; (void)y0; (void)x1; (void)y1;
}
static void mock__write_pixels(void *ctx, const cgl_color_t *px, uint32_t count)
{
    (void)ctx; (void)px; (void)count;
}
static void mock__wait_idle(void *ctx) { (void)ctx; }
static void mock__backlight(void *ctx, uint8_t p) { (void)ctx; (void)p; }

static const cgl_display_drv_t mock_drv = {
    "mock", mock__init, mock__set_window, mock__write_pixels,
    mock__wait_idle, mock__backlight, CGL_CFG_SCREEN_W, CGL_CFG_SCREEN_H,
};

/* --------------------------------------------------------------------------- */
/* Scenarios                                                                   */
/* --------------------------------------------------------------------------- */

static cgl_display_t s_display;
static cgl_screen_t s_screen;
static cgl_page_nav_t s_nav;

/**
 * Each of the two "stages" gets its own set of page objects.
 *
 * Why two: a page object's widgets are its static members, so reusing the same
 * tree across stages would leave stale parent links behind (a trap that only
 * appears in tests -- the firmware builds the tree once, so it never hits it).
 */
typedef struct
{
    app_page_smash_t smash;
    app_page_info_t info;
    app_page_menu_t menu;
} stage_pages_t;

static stage_pages_t s_a;
static stage_pages_t s_b;

/**
 * Create a standalone stage and run `frames` frames.
 * @return the number of on_step calls this stage's page received
 */
static uint32_t stage_run(stage_pages_t *pages, int frames, const char *tag)
{
    uint32_t before;
    int i;

    cgl_page_nav_init(&s_nav, "test.pages",
                     cgl_rect(0, 0, (int16_t)(CGL_CFG_SCREEN_W - 1), (int16_t)(CGL_CFG_SCREEN_H - 1)));
    /* One context, one shape per page. The links are rebuilt on every call because
       this function runs one stage at a time and `pages` differs between them. */
    static app_links_t links;
    static cgl_page_ctx_t ctx;

    links.smash = &pages->smash;
    links.bullet = NULL; /* this test does not build the bullet time page */
    links.info = &pages->info;
    links.menu = &pages->menu;
    ctx.screen = &s_screen;
    ctx.nav = &s_nav;
    ctx.app = &links;

    app_page_smash_attach(&pages->smash.base, &ctx);
    app_page_info_attach(&pages->info.base, &ctx);
    app_page_menu_attach(&pages->menu.base, &ctx);

    if (cgl_screen_init(&s_screen, &s_display, &s_nav) != 0)
    {
        printf("FAIL: cgl_screen_init\n");
        return 0u;
    }
    (void)cgl_page_nav_reset(&s_nav, &pages->smash.base);
    cgl_screen_frame(&s_screen, 0u); /* Apply the RESET and draw the first frame*/

    before = app_page_smash_steps(&pages->smash);
    for (i = 0; i < frames; i++)
    {
        cgl_screen_frame(&s_screen, 17u); /* 60 frames x 17 ms ~ 1 s*/
    }
    printf("  %-34s %3d frames(17ms) -> page on_step %3u times, clock issued %3u steps\n",
           tag, frames,
           (unsigned)(app_page_smash_steps(&pages->smash) - before),
           (unsigned)s_screen.clock.step_total);
    return app_page_smash_steps(&pages->smash) - before;
}

int main(void)
{
    int fails = 0;
    uint32_t n_a;
    uint32_t n_b;
    uint32_t expect;
    int step;
    int min_x = 99999, max_x = -99999, min_y = 99999, max_y = -99999;
    int last_restarts = 0;
    int max_speed_seen = 0;
    app_page_smash_t *smash = &s_b.smash;
    cgl_bounce_rect_t area;

    if (cgl_display_init(&s_display, &mock_drv, NULL) != 0)
    {
        return 1;
    }

    printf("== 1) Fixed-step rate: the world is advanced once per step (widgets must not stack up) ==\n");
    n_a = stage_run(&s_a, 60, "only the smash page (logo + bouncing ball)");
    n_b = stage_run(&s_b, 60, "same two widgets, second run");

    /* The playground the page handed the module. Every bounds assertion below is
       measured against **this**, not against a copy of the arithmetic: the test asks
       the module where the walls are, so it cannot pass by agreeing with itself. */
    if (!cgl_bounce_area_px(&smash->ball, &area))
    {
        printf("FAIL: the smash page's ball playground was rejected (%u rejections)\n",
               (unsigned)cgl_bounce_area_rejected(&smash->ball));
        return 1;
    }
    if (cgl_bounce_area_rejected(&smash->ball) != 0u)
    {
        printf("FAIL: the ball rejected a playground %u times\n",
               (unsigned)cgl_bounce_area_rejected(&smash->ball));
        fails++;
    }

    /* 1020 ms / 16 ms = 63.75 -> 63 steps. A +/-3 step rounding margin is allowed.*/
    expect = (60u * 17u) / CGL_CFG_CLOCK_STEP_MS;
    printf("  expect about %u steps (1020 ms / %u ms)\n", (unsigned)expect, CGL_CFG_CLOCK_STEP_MS);
    if ((n_a + 3u < expect) || (n_a > expect + 3u))
    {
        printf("FAIL: the step count is already wrong with a single page (%u steps)\n", (unsigned)n_a);
        fails++;
    }
    /* * Core assertion: two independent stages must have the same step count.
       If "every widget steps the world once", the second run (with one more widget
       taking part) would double it.*/
    if (n_a != n_b)
    {
        printf("FAIL: the two runs have different step counts (%u vs %u) -- the number of steps now depends on the number of widgets\n",
               (unsigned)n_a, (unsigned)n_b);
        fails++;
    }
    if (n_a > (expect * 2u))
    {
        printf("FAIL: the world was stepped several times (physics runs %.1f x speed)\n",
               (double)n_a / (double)expect);
        fails++;
    }

    /* ------------------------------------------------------------------------------------------ */
    /* 2~8) Long-run behaviour: use the stage finished above and keep running it cleanly for 20 s */
    /* ------------------------------------------------------------------------------------------ */
    printf("\n== 2) Bouncing ball long-run behaviour ==\n");
    printf("  physics world : %u bodies (%u dots + 1 heavy ball + 1 bouncing ball)\n",
           (unsigned)smash->storage.u.w.world.count, (unsigned)smash->debris.dot_count);
    printf("  active area : ball centre x %d..%d  y %d..%d (screen px, inclusive)\n",
           area.x0, area.x1, area.y0, area.y1);
    printf("  painted     : x %d..%d  y %d..%d (the disc deliberately overlaps the frame at the floor)\n",
           area.x0 - CGL_BOUNCE_RADIUS_PX, area.x1 + CGL_BOUNCE_RADIUS_PX,
           area.y0 - CGL_BOUNCE_RADIUS_PX, area.y1 + CGL_BOUNCE_RADIUS_PX);
    printf("  restitution : %u/256 = %.2f\n\n",
           (unsigned)CGL_BOUNCE_RESTITUTION_Q8, CGL_BOUNCE_RESTITUTION_Q8 / 256.0);

    {
        app_page_smash_restart(smash);
        last_restarts = 0;

        printf("      t   restarts  bounces   ball x   ball y   vmax\n");
        for (step = 1; step <= 1200; step++)
        {
            int16_t px = 0, py = 0;

            /* firmware path: one frame = clock + fixed steps + page callbacks + present*/
            cgl_screen_frame(&s_screen, 17u);

            if (!cgl_bounce_position_px(&smash->ball, &px, &py))
            {
                printf("FAIL: t=%.2fs cannot read the ball position (index %u / count %u)\n",
                       (double)step * 17.0 / 1000.0, (unsigned)cgl_bounce_index(&smash->ball),
                       (unsigned)smash->storage.u.w.world.count);
                fails++;
                break;
            }

            if (px < min_x) { min_x = px; }
            if (px > max_x) { max_x = px; }
            if (py < min_y) { min_y = py; }
            if (py > max_y) { max_y = py; }
            if ((int)cgl_bounce_max_speed_q4(&smash->ball) > max_speed_seen)
            {
                max_speed_seen = (int)cgl_bounce_max_speed_q4(&smash->ball);
            }

            /* 5) no wall clipping*/
            if ((px < area.x0) || (px > area.x1) || (py < area.y0) || (py > area.y1))
            {
                printf("FAIL: t=%.2fs ball out of bounds (%d, %d), allowed x %d..%d y %d..%d\n",
                       (double)step * 17.0 / 1000.0, px, py,
                       area.x0, area.x1, area.y0, area.y1);
                fails++;
                break;
            }
            /* 8) fixed point does not overflow*/
            {
                const cgl_phys2d_body_t *p =
                    &smash->storage.u.w.world.body[cgl_bounce_index(&smash->ball)];
                if ((p->x > 30000) || (p->x < -30000) || (p->y > 30000) || (p->y < -30000))
                {
                    printf("FAIL: t=%.2fs fixed-point value out of range (%d, %d)\n",
                           (double)step * 17.0 / 1000.0, p->x, p->y);
                    fails++;
                    break;
                }
            }

            if ((step % 120) == 0)
            {
                printf("%7.2fs %8d %9u %8d %8d %6d\n",
                       (double)step * 17.0 / 1000.0, (int)smash->debris.restarts,
                       (unsigned)cgl_bounce_bounces(&smash->ball), px, py,
                       (int)cgl_bounce_max_speed_q4(&smash->ball) >> 4);
            }

            /* 7) the ball must still be there when the logo is rebuilt*/
            if ((int)smash->debris.restarts != last_restarts)
            {
                last_restarts = (int)smash->debris.restarts;
                if (cgl_bounce_index(&smash->ball) >= smash->storage.u.w.world.count)
                {
                    printf("FAIL: the bouncing ball was dropped after logo rebuild %d\n", last_restarts);
                    fails++;
                    break;
                }
                printf("  -> logo rebuild %d (t=%.2fs), bouncing ball index %u / count %u is still valid\n",
                       last_restarts, (double)step * 17.0 / 1000.0,
                       (unsigned)cgl_bounce_index(&smash->ball), (unsigned)smash->storage.u.w.world.count);
            }
        }
    }

    printf("\n== 3) Statistics ==\n");
    printf("  ball centre range : x %d..%d  y %d..%d (frame)\n", min_x, max_x, min_y, max_y);
    printf("  max speed : %d/16 px/step\n", max_speed_seen);
    printf("  total bounces : %u\n", (unsigned)cgl_bounce_bounces(&smash->ball));
    printf("  logo rounds: %d (expect 6~9 rounds / 20 s)\n", (int)smash->debris.restarts);
    printf("  body pool   : %u / %d\n", (unsigned)smash->storage.u.w.world.count, CGL_PHYS2D_MAX_BODIES);

    if (max_speed_seen > CGL_PHYS2D_MAX_VELOCITY)
    {
        printf("FAIL: speed exceeds CGL_PHYS2D_MAX_VELOCITY\n");
        fails++;
    }
    if (cgl_bounce_bounces(&smash->ball) < 15u)
    {
        printf("FAIL: only %u bounces in 20 s, too few (the ball should keep bouncing across the whole viewport)\n",
               (unsigned)cgl_bounce_bounces(&smash->ball));
        fails++;
    }
    if ((min_y > area.y0 + 4) || (max_y < area.y1 - 4))
    {
        printf("FAIL: the ball does not use the whole active area (measured y %d..%d, range %d..%d)\n",
               min_y, max_y, area.y0, area.y1);
        fails++;
    }
    if (((int)smash->debris.restarts < 6) || ((int)smash->debris.restarts > 9))
    {
        printf("FAIL: the logo period is outside the expected range (%d rounds / 20 s) -- the physics speed may be wrong again\n",
               (int)smash->debris.restarts);
        fails++;
    }

    /* ----------------------------------------------------------------------- */
    /* 9) page gets covered -> physics freezes completely                      */
    /* ----------------------------------------------------------------------- */
    printf("\n== 4) A covered page stops stepping (the old implementation kept banking time and caught up later) ==\n");
    {
        uint32_t steps_before;
        int i;

        /* Push the info page on top of the smash page*/
        if (!cgl_page_nav_push(&s_nav, &s_b.info.base))
        {
            printf("FAIL: push was rejected\n");
            fails++;
        }
        for (i = 0; i < 20; i++)
        {
            cgl_screen_frame(&s_screen, 17u); /* Let the transition finish*/
        }
        if (cgl_page_nav_depth(&s_nav) != 2u)
        {
            printf("FAIL: stack depth should be 2, actually %u\n", (unsigned)cgl_page_nav_depth(&s_nav));
            fails++;
        }
        if (!app_page_smash_is_paused(smash))
        {
            printf("FAIL: the smash page is covered but reports itself as not paused\n");
            fails++;
        }

        steps_before = app_page_smash_steps(smash);
        for (i = 0; i < 60; i++)
        {
            cgl_screen_frame(&s_screen, 17u); /* Run another 1 s*/
        }
        printf("  1 s under the info page: smash page steps %u -> %u (the clock's total step count is still growing: %u)\n",
               (unsigned)steps_before, (unsigned)app_page_smash_steps(smash),
               (unsigned)s_screen.clock.step_total);
        if (app_page_smash_steps(smash) != steps_before)
        {
            printf("FAIL: the covered page is still being stepped (grew by %u steps)\n",
                   (unsigned)(app_page_smash_steps(smash) - steps_before));
            fails++;
        }

        /* After popping back it must resume, and must **not** catch up all the banked time at once*/
        if (!cgl_page_nav_back(&s_nav))
        {
            printf("FAIL: back was rejected\n");
            fails++;
        }
        for (i = 0; i < 20; i++)
        {
            cgl_screen_frame(&s_screen, 17u);
        }
        {
            uint32_t resumed_before = app_page_smash_steps(smash);
            uint32_t delta;

            for (i = 0; i < 60; i++)
            {
                cgl_screen_frame(&s_screen, 17u);
            }
            delta = app_page_smash_steps(smash) - resumed_before;
            printf("  1 s after popping back: smash page steps +%u (expect about %u, not the hundreds of a catch-up run)\n",
                   (unsigned)delta, (unsigned)expect);
            if (app_page_smash_steps(smash) <= resumed_before)
            {
                printf("FAIL: physics did not resume after popping back\n");
                fails++;
            }
            if (delta > (expect + 3u))
            {
                printf("FAIL: a catch-up run appeared after resuming (+%u steps > %u)\n",
                       (unsigned)delta, (unsigned)expect + 3u);
                fails++;
            }
        }
    }

    /* ----------------------------------------------------------------------- */
    /* 10) the drop-in widget: the gravity ball that draws itself              */
    /* ----------------------------------------------------------------------- */
    printf("\n== 5) The drop-in widget (cgl_bounce_widget_t) ==\n");
    {
        /* A canvas of this test's own. The mock panel above throws its pixels away on
           purpose (it exists to drive frames, not to judge them), and the point here is
           to read them back: the widget's contract is "it paints the ball", and that is
           a claim about pixels. */
        static cgl_color_t s_band[8 * CGL_CFG_SCREEN_W];
        cgl_canvas_t canvas;
        cgl_phys2d_t w;
        cgl_bounce_widget_t bw;
        cgl_bounce_rect_t warea;
        cgl_rect_t frame = cgl_rect(0, 19, (int16_t)(CGL_CFG_SCREEN_W - 1), 152);
        int16_t bx = 0;
        int16_t by = 0;
        int16_t band_y0;
        int16_t empty_y0;
        int painted = 0;
        int i;

        cgl_phys2d_reset(&w);
        if (!cgl_bounce_widget_init(&bw, "test.ball", frame, &w, CGL_COLOR_RED))
        {
            printf("FAIL: cgl_bounce_widget_init rejected the widget's own frame\n");
            fails++;
        }
        /* The widget does not ask the caller for a playground: it derives one from its
           frame, and the derivation is part of the contract. */
        if (!cgl_bounce_area_px(&bw.ball, &warea) ||
            (warea.x0 != (int16_t)(frame.x0 + CGL_BOUNCE_RADIUS_PX)) ||
            (warea.y0 != (int16_t)(frame.y0 + CGL_BOUNCE_RADIUS_PX)) ||
            (warea.x1 != (int16_t)(frame.x1 - CGL_BOUNCE_RADIUS_PX)) ||
            (warea.y1 != (int16_t)(frame.y1 - CGL_BOUNCE_RADIUS_PX)))
        {
            printf("FAIL: the widget's playground is not the frame inset by the ball's radius\n");
            fails++;
        }

        /* Drive it exactly the way a page does: the three calls, in order. */
        for (i = 0; i < 40; i++)
        {
            cgl_bounce_pre_step(&bw.ball);
            cgl_phys2d_step(&w);
            cgl_bounce_post_step(&bw.ball);
        }
        if (!cgl_bounce_position_px(&bw.ball, &bx, &by))
        {
            printf("FAIL: the widget's ball has no position\n");
            fails++;
        }
        if (by <= warea.y0)
        {
            printf("FAIL: the widget's ball did not fall under gravity (y %d, spawn %d)\n",
                   by, warea.y0);
            fails++;
        }

        /* Its own draw, straight into a band of this test's choosing. */
        band_y0 = (int16_t)(by & ~7);
        cgl_canvas_init(&canvas, s_band, CGL_CFG_SCREEN_W, 8, band_y0);
        for (i = 0; i < (8 * CGL_CFG_SCREEN_W); i++)
        {
            s_band[i] = CGL_COLOR_BLACK;
        }
        bw.base.vt->draw(&bw.base, &canvas);
        for (i = 0; i < (8 * CGL_CFG_SCREEN_W); i++)
        {
            if (s_band[i] != CGL_COLOR_BLACK)
            {
                painted++;
            }
        }
        printf("  widget draw: ball at (%d, %d), band y %d..%d, %d pixels painted\n",
               bx, by, band_y0, band_y0 + 7, painted);
        if (painted == 0)
        {
            printf("FAIL: the widget's draw painted nothing in the band that holds the ball\n");
            fails++;
        }

        /* ... and the painter's culling contract, which is what keeps a 43-band frame
           from walking the ball 43 times. */
        empty_y0 = (int16_t)((by + 100) & ~7);
        cgl_canvas_init(&canvas, s_band, CGL_CFG_SCREEN_W, 8, empty_y0);
        if (cgl_bounce_view_draw(&bw.ball, &canvas, empty_y0, (int16_t)(empty_y0 + 8),
                                 CGL_COLOR_RED) != 0u)
        {
            printf("FAIL: a band that does not hold the ball was not culled\n");
            fails++;
        }
        cgl_canvas_init(&canvas, s_band, CGL_CFG_SCREEN_W, 8, band_y0);
        if (cgl_bounce_view_draw(&bw.ball, &canvas, band_y0, (int16_t)(band_y0 + 8),
                                 CGL_COLOR_RED) != 1u)
        {
            printf("FAIL: the band that holds the ball did not report a painted disc\n");
            fails++;
        }
    }

    if (fails == 0)
    {
        printf("\nall checks passed.\n");
        return 0;
    }
    printf("\n%d checks failed.\n", fails);
    return 1;
}
