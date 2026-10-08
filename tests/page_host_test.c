// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    page_host_test.c
  * @brief   PC-side test: unified clock / tween / page stack / subtree panning (not built into the firmware)
  *
  *  This is the **specification test** for the capabilities added this round. It
  *  does not render real scenes; it asserts the framework's "must behave like
  *  this" properties directly:
  *
  *    A. cgl_clock    -- step count, dt clamping, no catch-up after a stall, per-frame step cap, alpha, FPS
  *    B. cgl_anim     -- exact easing endpoints, monotonicity, overshoot, delay, zero duration, no overflow for large values
  *    C. cgl_page_nav -- stack semantics, lifecycle order, applied at frame boundaries, last request wins,
  *                       no request lost during a transition, illegal requests rejected
  *    D. cgl_screen   -- the order within one frame: only the top page receives step/frame and a covered page is
  *                       completely frozen; input only reaches the top page, and keys pressed during a transition
  *                       are parked rather than dropped
  *    E. offset/pan  -- after a subtree is translated it draws on the right pixels, culling is right, hit testing is
  *                       right, and nothing is written outside the band
  *    F. cgl_font     -- text must follow the subtree translation too (regression for the real "forgot the pan" bug)
  *    G. warp        -- the mathematical properties of the Genie warp: p=0 is an exact identity, rows are monotonic
  *                      with a step of at most 1, map_rect is conservative, the funnel is continuous row by row with
  *                      no holes and 0 pixels outside the silhouette
  *    G2. S-shaped suck-in -- the shape of the horizontal S trajectory: tangent at both ends, a single peak in the
  *                      middle, adjacent source rows do not cross, the amplitude stays under the monotonicity bound,
  *                      map_rect stays conservative even on the "most bulged" frame
  *    H. Genie nav   -- who gets warped, who shows below, the layer order, warp fully cleared at the end
  *    H2. S-shaped nav -- S_OUT/S_IN really take the suck-in family path (they do not "stay in place"), the suck-in
  *                      point is at the bottom right, the direction is not reversed, warp is fully cleared at the end
  *    I. transition duration -- each type is independent (default suction 300 ms / others 220 ms); the duration
  *                      really decides "how many frames", and 0 means that type is instantaneous
  *    J. transition easing -- the curve is per-type too (default suction IN_QUAD / OUT_CUBIC, the rest OUT_CUBIC) and
  *                      out-of-range is clamped to LINEAR; * the progress at a given moment really does change with
  *                      the curve in the table (the curve is really handed to the tween, it is not decoration)
  *    K. page's own transition -- within one navigation the page that "moves" decides: the type/curve/duration a page
  *                      declares outranks the caller and the type table; POP/REPLACE read the leaving page's leave;
  *                      a declaration made in on_create applies to this very navigation; after the declaration is
  *                      withdrawn (AUTO) the fallback is as before; an out-of-range type makes the whole call have no
  *                      effect and an out-of-range curve is clamped to LINEAR
  *
  *  Build / run: the canonical definition is tests/CMakeLists.txt -- this test
  *  links only the framework sources, no application code:
  *
  *    cmake -B build/host -DCYTGL_BUILD_TESTS=ON
  *    cmake --build build/host
  *    ctest --test-dir build/host -R page --output-on-failure
  ******************************************************************************
  */
#include <stdio.h>
#include <string.h>

#include <cytgl/cytgl_config.h>
#include <cytgl/cgl_time.h>
#include <cytgl/cgl_anim.h>
#include <cytgl/cgl_canvas.h>
#include <cytgl/cgl_widget.h>
#include <cytgl/cgl_panel.h>
#include <cytgl/cgl_label.h>
#include <cytgl/cgl_page.h>
#include <cytgl/cgl_display.h>
#include <cytgl/cgl_screen.h>

static int s_fails;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            (void)printf("  FAIL: ");                                          \
            (void)printf(__VA_ARGS__);                                         \
            (void)printf("   [%s:%d]\n", __FILE__, __LINE__);                  \
            s_fails++;                                                         \
        }                                                                      \
    } while (0)

/* =========================================================================== */
/* A) unified clock                                                            */
/* =========================================================================== */

static void test_clock(void)
{
    cgl_clock_t c;
    uint32_t total = 0u;
    int i;

    (void)printf("\n== A) cgl_clock: exactly one place in the whole project accumulates time ==\n");

    /* --- the step count must be "total frame time / step", no more and no less ----*/
    cgl_clock_init(&c, 16u, 100u);
    for (i = 0; i < 100; i++) /* 100 frames x 17 ms = 1700 ms*/
    {
        uint8_t n = 0u;

        cgl_clock_begin(&c, 17u);
        while (cgl_clock_next_step(&c))
        {
            n++;
        }
        CHECK(n == c.steps, "the next_step iteration count (%u) does not match the step count the clock computed (%u)",
              (unsigned)n, (unsigned)c.steps);
        total += c.steps;
    }
    (void)printf("  100 frames x 17 ms -> %u steps accumulated (1700/16 = 106)\n", (unsigned)total);
    CHECK(total == 106u, "the total step count is wrong: %u (expected 106)", (unsigned)total);
    CHECK(c.step_total == total, "step_total (%u) does not match the iteration count (%u)",
          (unsigned)c.step_total, (unsigned)total);
    CHECK(c.frame == 100u, "the frame count should be 100, actually %u", (unsigned)c.frame);
    CHECK(c.now_ms == 1700u, "now_ms should be 1700, actually %u", (unsigned)c.now_ms);

    /* --- the per-frame step count must be capped by max_steps, and extra next_step calls must do nothing ----*/
    cgl_clock_init(&c, 16u, 100u);
    cgl_clock_begin(&c, 100u); /* 100/16 = 6 steps, cap 4*/
    {
        int n = 0;

        while (cgl_clock_next_step(&c))
        {
            n++;
        }
        (void)printf("  dt=100 ms in one frame -> %d steps (cap %u)\n", n, (unsigned)c.max_steps);
        CHECK(n == (int)c.max_steps, "the per-frame step count should be capped by max_steps (%d != %u)",
              n, (unsigned)c.max_steps);
        CHECK(!cgl_clock_next_step(&c), "once the steps are used up next_step must return false");
        CHECK(cgl_clock_steps_left(&c) == 0u, "steps_left should be 0");
    }

    /* --- a stall does not cause a catch-up run ----------------------- */
    cgl_clock_init(&c, 16u, 100u);
    cgl_clock_begin(&c, 5000u); /* clamped to 100 ms*/
    CHECK(c.dt_ms == 100u, "dt should be clamped to max_dt_ms=100, actually %u", (unsigned)c.dt_ms);
    {
        int n = 0;

        while (cgl_clock_next_step(&c))
        {
            n++;
        }
        CHECK(n <= (int)c.max_steps, "the first frame after the stall has %d steps, over the cap", n);
    }
    for (i = 0; i < 10; i++)
    {
        uint8_t n = 0u;

        cgl_clock_begin(&c, 17u);
        while (cgl_clock_next_step(&c))
        {
            n++;
        }
        CHECK(n <= 2u, "frame %d after the stall is still catching up (%u steps) -- the debt must be dropped", i, (unsigned)n);
    }

    /* --- step_ms == 0: frame time only, no logic steps ------- */
    cgl_clock_init(&c, 0u, 100u);
    cgl_clock_begin(&c, 100u);
    CHECK(c.steps == 0u, "with step_ms=0 there must be no logic steps");
    CHECK(!cgl_clock_next_step(&c), "with step_ms=0 next_step must return false");
    CHECK(c.dt_ms == 100u, "with step_ms=0 the frame time must still be computed");
    CHECK(c.now_ms == 100u, "with step_ms=0 now_ms must still accumulate");

    /* --- the alpha interpolation factor stays inside its legal range ----------*/
    cgl_clock_init(&c, 16u, 100u);
    for (i = 0; i < 40; i++)
    {
        cgl_clock_begin(&c, 5u);
        CHECK(cgl_clock_alpha_q8(&c) < 256u, "alpha_q8 must be < 256");
    }

    /* --- FPS: computed from accumulated time, and close to the real value -----*/
    cgl_clock_init(&c, 16u, 100u);
    for (i = 0; i < 40; i++) /* 40 x 20 ms = 800 ms -> 50 fps */
    {
        cgl_clock_begin(&c, 20u);
    }
    (void)printf("  40 frames x 20 ms -> fps = %u (expected 50)\n", (unsigned)c.fps);
    CHECK((c.fps >= 48u) && (c.fps <= 52u), "fps computed wrong: %u", (unsigned)c.fps);

    /* --- reset clears the state and keeps the configuration ------ */
    cgl_clock_reset(&c);
    CHECK((c.step_ms == 16u) && (c.frame == 0u) && (c.now_ms == 0u),
          "cgl_clock_reset should clear the counters and keep step_ms");
    CHECK(c.max_steps == (uint8_t)CGL_CFG_CLOCK_MAX_STEPS, "reset must not change max_steps");
}

/* =========================================================================== */
/* B) tween and easing                                                         */
/* =========================================================================== */

static void test_anim(void)
{
    cgl_anim_t a;
    cgl_ease_t e;
    int k;

    (void)printf("\n== B) cgl_anim: one curve drives one value ==\n");

    for (e = (cgl_ease_t)0; e < CGL_EASE_COUNT; e = (cgl_ease_t)(e + 1))
    {
        uint16_t peak = 0u;

        CHECK(cgl_anim_ease(e, 0u) == 0u, "ease %d at p=0 must return 0, actually %u",
              (int)e, (unsigned)cgl_anim_ease(e, 0u));
        CHECK(cgl_anim_ease(e, 256u) == 256u, "ease %d at p=256 must return 256, actually %u",
              (int)e, (unsigned)cgl_anim_ease(e, 256u));

        for (k = 0; k <= 256; k++)
        {
            uint16_t v = cgl_anim_ease(e, (uint16_t)k);

            if (v > peak)
            {
                peak = v;
            }
        }
        (void)printf("  ease %d peak %u/256\n", (int)e, (unsigned)peak);
        CHECK(peak <= 300u, "ease %d peak %u is too large (a Q8 output should be <= 300)", (int)e, (unsigned)peak);
    }

    /* --- everything except the back curves is monotonically non-decreasing -----*/
    for (e = (cgl_ease_t)0; e < CGL_EASE_COUNT; e = (cgl_ease_t)(e + 1))
    {
        uint16_t prev = 0u;
        bool mono = true;

        if (e == CGL_EASE_OUT_BACK)
        {
            continue; /* a back curve is supposed to overshoot first and then come back*/
        }
        for (k = 0; k <= 256; k++)
        {
            uint16_t v = cgl_anim_ease(e, (uint16_t)k);

            if (v < prev)
            {
                mono = false;
                break;
            }
            prev = v;
        }
        CHECK(mono, "ease %d is not monotonically non-decreasing", (int)e);
    }

    /* --- OUT_BACK really does overshoot ----------------------------- */
    {
        uint16_t peak = 0u;

        for (k = 0; k <= 256; k++)
        {
            uint16_t v = cgl_anim_ease(CGL_EASE_OUT_BACK, (uint16_t)k);

            if (v > peak)
            {
                peak = v;
            }
        }
        (void)printf("  OUT_BACK peak %u/256 (%.1f%% overshoot)\n",
                     (unsigned)peak, ((double)peak - 256.0) * 100.0 / 256.0);
        CHECK(peak > 256u, "OUT_BACK should overshoot, actual peak %u", (unsigned)peak);
    }

    /* --- curve shape: OUT is fast in the first half, IN is slow in the first half ----*/
    CHECK(cgl_anim_ease(CGL_EASE_OUT_QUAD, 128u) > 128u, "OUT_QUAD should move faster in the first half");
    CHECK(cgl_anim_ease(CGL_EASE_IN_QUAD, 128u) < 128u, "IN_QUAD should move slower in the first half");
    CHECK(cgl_anim_ease(CGL_EASE_IN_OUT_QUAD, 128u) == 128u, "IN_OUT_QUAD's midpoint should be 128");

    /* --- tick: once the duration is up it must land exactly on to -------------*/
    cgl_anim_init(&a);
    cgl_anim_start(&a, 10, 300, 100u, CGL_EASE_OUT_CUBIC);
    CHECK(cgl_anim_value(&a) == 10, "right after start, value should equal from");
    CHECK(cgl_anim_running(&a) && !cgl_anim_done(&a), "right after start it should be running");
    for (k = 0; k < 10; k++)
    {
        (void)cgl_anim_tick(&a, 10u);
    }
    CHECK(cgl_anim_done(&a), "after 100 ms it should be done");
    CHECK(cgl_anim_value(&a) == 300, "after done, value must be exactly to, actually %d",
          (int)cgl_anim_value(&a));
    CHECK(!cgl_anim_tick(&a, 10u), "after done, tick must not report a change again");
    CHECK(cgl_anim_progress_q8(&a) == 256u, "after done the progress must be 256");

    /* --- the delay phase produces no change ---------------------- */
    cgl_anim_init(&a);
    cgl_anim_start(&a, 0, 100, 50u, CGL_EASE_LINEAR);
    cgl_anim_set_delay(&a, 30u);
    CHECK(!cgl_anim_tick(&a, 20u), "no change should be reported during the delay");
    CHECK(cgl_anim_value(&a) == 0, "during the delay value should stay at from");
    CHECK(cgl_anim_running(&a), "during the delay it is still running");

    /* --- duration 0: the next frame goes straight to to --------- */
    cgl_anim_init(&a);
    cgl_anim_start(&a, 0, 7, 0u, CGL_EASE_LINEAR);
    (void)cgl_anim_tick(&a, 0u);
    CHECK(cgl_anim_done(&a) && (cgl_anim_value(&a) == 7), "with dur=0 it should converge to to immediately");

    /* --- a screen-sized displacement must not overflow --------- */
    cgl_anim_init(&a);
    cgl_anim_start(&a, -320, 320, 100u, CGL_EASE_OUT_BACK);
    for (k = 0; k < 12; k++)
    {
        int32_t v;

        (void)cgl_anim_tick(&a, 10u);
        v = cgl_anim_value(&a);
        /* span 640, overshoot ~11% -> peak about 390. Allowing +/-1000 still catches an int32 overflow.*/
        CHECK((v > -1000) && (v < 1000), "large-displacement tween overflow: %d", (int)v);
    }

    /* --- lerp endpoints and midpoint -------------------------------- */
    CHECK(cgl_anim_lerp(10, 20, 0u) == 10, "lerp at t=0 must be a");
    CHECK(cgl_anim_lerp(10, 20, 256u) == 20, "lerp at t=256 must be b");
    CHECK(cgl_anim_lerp(-320, 0, 128u) == -160, "lerp midpoint wrong: %d",
          (int)cgl_anim_lerp(-320, 0, 128u));
}

/* =========================================================================== */
/* C) page stack                                                               */
/* =========================================================================== */

#define LOG_MAX 40
static char s_log[LOG_MAX][28];
static int s_log_n;

static void rec(const char *what, const cgl_page_t *p)
{
    if (s_log_n < LOG_MAX)
    {
        (void)snprintf(s_log[s_log_n], sizeof(s_log[s_log_n]), "%s:%s",
                       what, (p != NULL) ? p->name : "-");
        s_log_n++;
    }
}

static void log_reset(void) { s_log_n = 0; }

static bool log_is(int i, const char *want)
{
    return (i < s_log_n) && (strcmp(s_log[i], want) == 0);
}

static void log_dump(void)
{
    int i;

    (void)printf("  lifecycle:");
    for (i = 0; i < s_log_n; i++)
    {
        (void)printf(" %s", s_log[i]);
    }
    (void)printf("\n");
}

/* A minimal page used for testing: one root panel + one child panel.*/
typedef struct
{
    cgl_page_t base;
    cgl_panel_t root;
    int steps;
    int frames;
    int events;
    bool eat_events;
    bool have_root;
    /** CGL_TRANS_AUTO = do not declare it in on_create; any other value = declare enter with it in on_create.*/
    uint8_t create_trans;
} tpage_t;

static void tp_create(cgl_page_t *page)
{
    tpage_t *p = (tpage_t *)page;

    rec("create", page);
    if (p->create_trans != (uint8_t)CGL_TRANS_AUTO)
    {
        /* A declaration inside on_create must take effect too: the transition preference is read when the navigation is applied (nav__start),
           and the entering page's on_create runs before that.*/
        cgl_page_set_trans_enter(page, (cgl_trans_t)p->create_trans,
                                CGL_PAGE_FX_KEEP_EASE, CGL_PAGE_FX_KEEP_MS);
    }
    if (!p->have_root)
    {
        return; /* Deliberately make a page with no widgets; the framework must tolerate it*/
    }
    cgl_panel_init(&p->root, page->name, cgl_rect(0, 0, 319, 171),
                  CGL_CFG_BG_COLOR, CGL_CFG_BG_COLOR);
    cgl_page_set_root(page, &p->root.base);
}

static void tp_destroy(cgl_page_t *page)
{
    rec("destroy", page);
    cgl_page_set_root(page, NULL);
}

static void tp_enter(cgl_page_t *page, cgl_page_t *from, uint8_t kind)
{
    (void)from;
    (void)kind;
    rec("enter", page);
}

static void tp_exit(cgl_page_t *page, cgl_page_t *to)
{
    (void)to;
    rec("exit", page);
}

static void tp_pause(cgl_page_t *page, cgl_page_t *by)
{
    (void)by;
    rec("pause", page);
}

static void tp_resume(cgl_page_t *page, cgl_page_t *from)
{
    (void)from;
    rec("resume", page);
}

static void tp_step(cgl_page_t *page, uint32_t step_ms)
{
    (void)step_ms;
    ((tpage_t *)page)->steps++;
}

static void tp_frame(cgl_page_t *page, const cgl_clock_t *clk)
{
    (void)clk;
    ((tpage_t *)page)->frames++;
}

static bool tp_event(cgl_page_t *page, const cgl_event_t *ev)
{
    tpage_t *p = (tpage_t *)page;

    (void)ev;
    p->events++;
    return p->eat_events;
}

static const cgl_page_vt_t tp_vt = {
    .type = "tpage",
    .on_create = tp_create,
    .on_destroy = tp_destroy,
    .on_enter = tp_enter,
    .on_exit = tp_exit,
    .on_pause = tp_pause,
    .on_resume = tp_resume,
    .on_step = tp_step,
    .on_frame = tp_frame,
    .on_event = tp_event,
};

static cgl_clock_t s_clk;

static void tp_init(tpage_t *p, const char *name, bool have_root)
{
    cgl_page_init(&p->base, &tp_vt, name, NULL);
    p->steps = 0;
    p->frames = 0;
    p->events = 0;
    p->eat_events = true;
    p->have_root = have_root;
    p->create_trans = (uint8_t)CGL_TRANS_AUTO;
    cgl_page_set_root(&p->base, NULL);
}

static cgl_event_t tp_ev(cgl_event_kind_t kind, uint32_t hold)
{
    cgl_event_t ev;

    ev.kind = (uint8_t)kind;
    ev.key = 0u;
    ev.x = 0;
    ev.y = 0;
    ev.hold_ms = hold;
    return ev;
}

/** Advance one "frame": accumulate time first, then run the navigator's frame boundary (the same order as cgl_screen_frame).*/
static void tp_tick(cgl_page_nav_t *n, uint32_t dt)
{
    cgl_clock_begin(&s_clk, dt);
    cgl_page_nav_frame(n, &s_clk);
}

/** Run enough frames that the current transition (CGL_CFG_TRANSITION_MS) is certain to finish.*/
static void tp_settle(cgl_page_nav_t *n)
{
    int i;

    for (i = 0; i < 40; i++)
    {
        tp_tick(n, 16u);
    }
}

/**
 * Apply the navigation registered last time (and advance one frame).
 *
 * * A navigation request only takes effect **at a frame boundary** (it is applied inside
 *   `cgl_page_nav_frame()`): reading `nav.trans` or the page's `on_create` side effects
 *   right after registering gives you the **previous** transition -- this once turned
 *   section K red with 16 assertions at once, so it is a named helper now; do not
 *   hand-write bare `tp_tick` calls.
 */
static void tp_apply(cgl_page_nav_t *n)
{
    tp_tick(n, 16u);
}

static void test_pages(void)
{
    static cgl_page_nav_t nav;
    static tpage_t a_page; /* root*/
    static tpage_t b_page;
    static tpage_t c_page;
    static tpage_t d_page;
    static tpage_t fillers[CGL_CFG_PAGE_STACK_MAX];
    static const char *const filler_names[CGL_CFG_PAGE_STACK_MAX] = { "F0", "F1", "F2", "F3" };
    cgl_rect_t area = cgl_rect(0, 0, (CGL_CFG_SCREEN_W - 1), (CGL_CFG_SCREEN_H - 1));

    (void)printf("\n== C) cgl_page_nav: stack semantics and lifecycle ==\n");

    cgl_clock_init(&s_clk, 16u, 100u);
    cgl_page_nav_init(&nav, "test.pages", area);
    tp_init(&a_page, "A", true);
    tp_init(&b_page, "B", true);
    tp_init(&c_page, "C", true);
    tp_init(&d_page, "D", true);

    /* --- RESET: clear the stack, install the root page, and make it instantaneous ----*/
    log_reset();
    CHECK(cgl_page_nav_depth(&nav) == 0u, "a fresh navigator's stack should be empty");
    CHECK(cgl_page_nav_reset(&nav, &a_page.base), "RESET should be accepted");
    CHECK(cgl_page_nav_depth(&nav) == 0u, "registering must not change the stack (it is applied at the frame boundary)");
    CHECK(cgl_page_nav_pending(&nav), "there should be a pending navigation");
    tp_tick(&nav, 16u);
    CHECK(cgl_page_nav_depth(&nav) == 1u, "after it is applied the stack depth should be 1");
    CHECK(!cgl_page_nav_pending(&nav), "after it is applied the pending flag should be cleared");
    CHECK(cgl_page_nav_top(&nav) == &a_page.base, "the top should be A");
    CHECK(cgl_page_is_created(&a_page.base), "A should have been created");
    log_dump();
    CHECK(log_is(0, "create:A") && log_is(1, "enter:A") && (s_log_n == 2),
          "the RESET lifecycle should be create:A, enter:A");
    CHECK(!cgl_page_nav_busy(&nav), "RESET must not leave a transition behind");

    /* --- PUSH: create(new) -> pause(old) -> enter(new) ---------------- */
    log_reset();
    CHECK(cgl_page_nav_push(&nav, &b_page.base), "PUSH B should be accepted");
    tp_tick(&nav, 16u);
    CHECK(cgl_page_nav_depth(&nav) == 2u, "after PUSH the stack depth should be 2");
    CHECK(cgl_page_nav_top(&nav) == &b_page.base, "the top should be B");
    CHECK(cgl_page_nav_busy(&nav), "PUSH carries a transition by default, so it should be busy");
    log_dump();
    CHECK(log_is(0, "create:B") && log_is(1, "pause:A") && log_is(2, "enter:B"),
          "the PUSH lifecycle order should be create:B, pause:A, enter:B");

    /* --- during a transition: input is locked at the UI level ----------------*/
    {
        cgl_event_t ev = tp_ev(CGL_EV_KEY_DOWN, 0u);

        CHECK(!cgl_page_nav_event(&nav, &ev), "input must not be dispatched during a transition");
        CHECK(b_page.events == 0, "the top page must not receive events during a transition");
    }

    /* --- transition done: the leaving page is hidden but **not** destroyed -------*/
    tp_settle(&nav);
    CHECK(!cgl_page_nav_busy(&nav), "the transition should have finished");
    CHECK(cgl_page_is_created(&a_page.base), "PUSH must keep the old page (it is reused on the way back)");
    CHECK((a_page.root.base.flags & CGL_WF_VISIBLE) == 0u, "the covered page must be invisible");
    CHECK((b_page.root.base.flags & CGL_WF_VISIBLE) != 0u, "the top page must be visible");
    CHECK((b_page.root.base.offset.x == 0) && (b_page.root.base.offset.y == 0),
          "after the transition the offset must return to zero");

    /* --- input only reaches the top page --------------------------- */
    {
        cgl_event_t ev = tp_ev(CGL_EV_KEY_UP, 10u);

        CHECK(cgl_page_nav_event(&nav, &ev), "the top page consumed the event, so it should return true");
        CHECK(b_page.events == 1, "the top page should receive 1 event, actually %d", b_page.events);
        CHECK(a_page.events == 0, "the covered page must not receive events");
    }

    /* --- CGL_EV_BACK is interpreted by the navigator as "pop" ---- */
    {
        cgl_event_t ev = tp_ev(CGL_EV_BACK, 0u);

        log_reset();
        CHECK(cgl_page_nav_event(&nav, &ev), "BACK should be accepted by the navigator");
        tp_tick(&nav, 16u);
        CHECK(cgl_page_nav_depth(&nav) == 1u, "after BACK the stack depth should be 1");
        log_dump();
        CHECK(log_is(0, "resume:A") && log_is(1, "exit:B"),
              "POP should resume the lower page first, then exit the upper one");
        tp_settle(&nav);
        log_dump();
        CHECK(log_is(2, "destroy:B"), "POP's leaving page should be destroyed after the transition ends");
        CHECK(!cgl_page_is_created(&b_page.base), "B should have been destroyed");
        CHECK((a_page.root.base.flags & CGL_WF_VISIBLE) != 0u, "after going back the lower page must be visible again");
        CHECK(a_page.base.nav == &nav, "after going back the lower page should still belong to this navigator");
    }

    /* --- illegal requests are rejected (not silently dropped or overwriting the top) ----*/
    {
        uint32_t dropped0 = cgl_page_nav_dropped(&nav);

        CHECK(!cgl_page_nav_push(&nav, NULL), "push(NULL) should be rejected");
        CHECK(!cgl_page_nav_push(&nav, &a_page.base), "pushing a page that is already on the stack should be rejected");
        CHECK(!cgl_page_nav_back(&nav), "with only one page on the stack, back should be rejected");
        (void)printf("  illegal requests rejected %u times\n",
                     (unsigned)(cgl_page_nav_dropped(&nav) - dropped0));
        CHECK(cgl_page_nav_dropped(&nav) >= dropped0 + 3u, "the rejection count should be tracked");
    }

    /* --- several requests within one frame: only the last one takes effect -----*/
    log_reset();
    (void)cgl_page_nav_push(&nav, &b_page.base);
    (void)cgl_page_nav_push(&nav, &c_page.base);
    tp_tick(&nav, 16u);
    CHECK(cgl_page_nav_depth(&nav) == 2u, "after two pushes in the same frame the stack depth should still be 2");
    CHECK(cgl_page_nav_top(&nav) == &c_page.base, "the last request (C) should take effect");
    CHECK(!cgl_page_is_created(&b_page.base), "the overwritten request must produce no side effects");
    log_dump();

    /* --- a new request arrives mid-transition: the old transition finishes first and the leaving page is not left in the tree ----*/
    {
        /* at this point the A -> C transition is playing (not settled yet)*/
        CHECK(cgl_page_nav_busy(&nav), "it should still be transitioning");
        log_reset();
        (void)cgl_page_nav_request(&nav, CGL_NAV_REPLACE, &d_page.base, CGL_TRANS_SLIDE_LEFT, NULL);
        tp_tick(&nav, 16u);
        tp_settle(&nav);
        CHECK(cgl_page_nav_depth(&nav) == 2u, "after REPLACE the stack depth should still be 2");
        CHECK(cgl_page_nav_top(&nav) == &d_page.base, "the top should be D");
        CHECK(!cgl_page_is_created(&c_page.base), "C from the interrupted transition must be destroyed");
        CHECK(!cgl_page_nav_busy(&nav), "the transition should have finished");
        CHECK((d_page.root.base.offset.x == 0) && (d_page.root.base.offset.y == 0),
              "for a transition that was interrupted and then resumed, the final offset must return to zero");
    }

    /* --- push must be rejected when the stack is full ------------- */
    (void)cgl_page_nav_reset(&nav, &a_page.base);
    tp_settle(&nav);
    {
        int fi = 0;

        while ((cgl_page_nav_depth(&nav) < (uint8_t)CGL_CFG_PAGE_STACK_MAX) && (fi < 4))
        {
            tp_init(&fillers[fi], filler_names[fi], true);
            if (!cgl_page_nav_push(&nav, &fillers[fi].base))
            {
                break;
            }
            tp_settle(&nav);
            fi++;
        }
        (void)printf("  depth after filling = %u / %u\n",
                     (unsigned)cgl_page_nav_depth(&nav), (unsigned)CGL_CFG_PAGE_STACK_MAX);
        CHECK(cgl_page_nav_depth(&nav) == (uint8_t)CGL_CFG_PAGE_STACK_MAX, "the stack should be fillable");
        tp_init(&b_page, "B", true);
        CHECK(!cgl_page_nav_push(&nav, &b_page.base), "push must be rejected when the stack is full");
        CHECK(cgl_page_nav_depth(&nav) == (uint8_t)CGL_CFG_PAGE_STACK_MAX, "the stack must not change after a rejection");
    }

    /* --- REPLACE: the depth is unchanged and the old page is destroyed --------*/
    (void)cgl_page_nav_reset(&nav, &a_page.base);
    tp_settle(&nav);
    (void)cgl_page_nav_push(&nav, &b_page.base);
    tp_settle(&nav);
    log_reset();
    (void)cgl_page_nav_request(&nav, CGL_NAV_REPLACE, &c_page.base, CGL_TRANS_NONE, NULL);
    tp_tick(&nav, 16u);
    CHECK(cgl_page_nav_depth(&nav) == 2u, "REPLACE must not change the stack depth");
    CHECK(cgl_page_nav_top(&nav) == &c_page.base, "after REPLACE the top should be C");
    CHECK(!cgl_page_is_created(&b_page.base), "the replaced B must be destroyed");
    log_dump();
    CHECK(log_is(0, "exit:B") && log_is(1, "create:C") && log_is(2, "enter:C") &&
              log_is(3, "destroy:B"),
          "the REPLACE lifecycle should be exit:B, create:C, enter:C, destroy:B");

    /* --- a page with no widgets must work too (root == NULL) ----------------*/
    {
        static tpage_t no_root;
        cgl_event_t ev = tp_ev(CGL_EV_KEY_DOWN, 0u);

        tp_init(&no_root, "NOROOT", false);
        log_reset();
        (void)cgl_page_nav_request(&nav, CGL_NAV_REPLACE, &no_root.base, CGL_TRANS_NONE, NULL);
        tp_tick(&nav, 16u);
        CHECK(cgl_page_nav_top(&nav) == &no_root.base, "a widget-less page should be able to become the top");
        CHECK(cgl_page_is_created(&no_root.base), "a widget-less page should receive create too");
        (void)cgl_page_nav_event(&nav, &ev); /* it must not crash*/
        CHECK(no_root.events == 1, "a widget-less page should receive on_event too");
    }
}

/* =============================================================================================== */
/* D) the order within one screen frame: only the top page receives step / frame, no input is lost */
/* =============================================================================================== */

static int mock__init(void *ctx) { (void)ctx; return 0; }
static void mock__set_window(void *ctx, uint16_t a, uint16_t b, uint16_t c, uint16_t d)
{
    (void)ctx; (void)a; (void)b; (void)c; (void)d;
}
static void mock__write_pixels(void *ctx, const cgl_color_t *p, uint32_t n)
{
    (void)ctx; (void)p; (void)n;
}
static void mock__wait_idle(void *ctx) { (void)ctx; }
static void mock__backlight(void *ctx, uint8_t p) { (void)ctx; (void)p; }

static const cgl_display_drv_t mock_drv = {
    "mock", mock__init, mock__set_window, mock__write_pixels,
    mock__wait_idle, mock__backlight, CGL_CFG_SCREEN_W, CGL_CFG_SCREEN_H,
};

static void test_screen_pipeline(void)
{
    static cgl_display_t display;
    static cgl_screen_t screen;
    static cgl_page_nav_t nav;
    static tpage_t root_page;
    static tpage_t top_page;
    int i;

    (void)printf("\n== D) cgl_screen_frame: the order and routing within one frame ==\n");

    if (cgl_display_init(&display, &mock_drv, NULL) != 0)
    {
        CHECK(false, "cgl_display_init failed");
        return;
    }
    cgl_page_nav_init(&nav, "screen.pages",
                     cgl_rect(0, 0, (CGL_CFG_SCREEN_W - 1), (CGL_CFG_SCREEN_H - 1)));
    tp_init(&root_page, "ROOT", true);
    tp_init(&top_page, "TOP", true);
    if (cgl_screen_init(&screen, &display, &nav) != 0)
    {
        CHECK(false, "cgl_screen_init failed (geometry or parameters inconsistent)");
        return;
    }

    (void)cgl_page_nav_reset(&nav, &root_page.base);
    cgl_screen_frame(&screen, 0u);

    /* --- with the bottom page running alone: both step and frame grow --------*/
    {
        int frames0 = root_page.frames;
        int steps0 = root_page.steps;

        for (i = 0; i < 10; i++)
        {
            cgl_screen_frame(&screen, 17u);
        }
        (void)printf("  running alone for 10 frames: ROOT steps +%d frames +%d (clock step_total=%u)\n",
                     root_page.steps - steps0, root_page.frames - frames0,
                     (unsigned)screen.clock.step_total);
        CHECK(root_page.frames - frames0 == 10,
              "exactly one on_frame per frame, actually +%d", root_page.frames - frames0);
        CHECK(root_page.steps > steps0, "the top page should receive on_step");
        /* * The step count the page receives must be strictly equal to the step count the clock issues:
           one more = physics ran fast, one fewer = time was swallowed.*/
        CHECK((uint32_t)root_page.steps == screen.clock.step_total,
              "the step count the page receives must equal the step count the clock issues (%d vs %u)",
              root_page.steps, (unsigned)screen.clock.step_total);
    }

    /* --- after pushing a new page: the old page is completely frozen ---------*/
    {
        int root_steps;
        int root_frames;

        (void)cgl_page_nav_push(&nav, &top_page.base);
        for (i = 0; i < 30; i++)
        {
            cgl_screen_frame(&screen, 17u); /* the transition should be long over*/
        }
        CHECK(!cgl_page_nav_busy(&nav), "the transition should have finished");

        root_steps = root_page.steps;
        root_frames = root_page.frames;
        for (i = 0; i < 30; i++)
        {
            cgl_screen_frame(&screen, 17u);
        }
        (void)printf("  30 frames after being covered: ROOT steps %d->%d frames %d->%d;"
                     "TOP steps=%d frames=%d\n",
                     root_steps, root_page.steps, root_frames, root_page.frames,
                     top_page.steps, top_page.frames);
        CHECK(root_page.steps == root_steps, "the covered page must not receive on_step again (grew by %d steps)",
              root_page.steps - root_steps);
        CHECK(root_page.frames == root_frames, "the covered page must not receive on_frame again");
        CHECK(top_page.frames > 0, "the top page should receive on_frame");
        CHECK(top_page.steps > 0, "the top page should receive on_step");
    }

    /* --- input posted during a transition must be kept and only take effect afterwards ----*/
    {
        cgl_event_t ev = tp_ev(CGL_EV_KEY_UP, 5u);
        int before = top_page.events;

        /* First pop the stack to expose ROOT (which starts the transition), then post an event while the transition is still running*/
        (void)cgl_page_nav_back(&nav);
        cgl_screen_frame(&screen, 16u); /* start the transition*/
        CHECK(cgl_page_nav_busy(&nav), "it should be transitioning");
        CHECK(cgl_screen_post_event(&screen, &ev), "the enqueue should succeed");
        CHECK(cgl_screen_dropped_events(&screen) == 0u, "the queue must not overflow");

        cgl_screen_frame(&screen, 16u); /* mid-transition: the event must still be waiting in the queue*/
        CHECK(top_page.events == before, "nothing may be dispatched during a transition (but nothing may be lost either)");

        for (i = 0; i < 40; i++)
        {
            cgl_screen_frame(&screen, 16u);
        }
        CHECK(!cgl_page_nav_busy(&nav), "the transition should have finished");
        (void)printf("  events posted during the transition: ROOT events=%d (expected 1)\n", root_page.events);
        CHECK(root_page.events == 1, "events queued up during a transition must be delivered to the new top page afterwards");
    }
}

/* =========================================================================== */
/* E) subtree translation (offset / pan)                                       */
/* =========================================================================== */

#define BAND_H 8
/** Sentinel value of the band buffer: any place that "must not be written" has to keep it untouched.*/
#define SENTINEL CGL_RGB565(200, 100, 50)

static cgl_color_t s_band[CGL_CFG_SCREEN_W * BAND_H];
static cgl_canvas_t s_canvas;
static int16_t s_origin_y; /**< which screen row the current canvas is bound to*/

/** Read directly by **buffer row index** (used to check whether bytes outside the band were touched).*/
static cgl_color_t sband(int16_t row, int16_t x)
{
    if ((x < 0) || (x >= CGL_CFG_SCREEN_W) || (row < 0) || (row >= BAND_H))
    {
        return 0u;
    }
    return s_band[((uint32_t)row * CGL_CFG_SCREEN_W) + (uint32_t)x];
}

/**
 * Read by **screen coordinates**.
 * WARNING: origin_y must be subtracted: canvas row 0 corresponds to screen row
 *    origin_y, and indexing the buffer with a screen y directly is the easiest
 *    mistake to make in banded rendering.
 */
static cgl_color_t px(int16_t x, int16_t screen_y)
{
    return sband((int16_t)(screen_y - s_origin_y), x);
}

static void band_clear(void)
{
    uint32_t i;

    for (i = 0u; i < (uint32_t)(CGL_CFG_SCREEN_W * BAND_H); i++)
    {
        s_band[i] = SENTINEL;
    }
}

static void band_bind(int16_t origin_y, int16_t height)
{
    s_origin_y = origin_y;
    cgl_canvas_init(&s_canvas, s_band, CGL_CFG_SCREEN_W, height, origin_y);
    cgl_canvas_set_clip(&s_canvas, cgl_rect(0, origin_y, (CGL_CFG_SCREEN_W - 1),
                                          (int16_t)(origin_y + height - 1)));
}

static void test_offset(void)
{
    static cgl_panel_t root;
    static cgl_panel_t child;
    static cgl_panel_t sib;
    cgl_event_t ev;

    (void)printf("\n== E) offset / pan: translating a whole subtree ==\n");

    cgl_widget_set_damage_sink(NULL, NULL); /* this test does not care about dirty rectangles*/

    cgl_panel_init(&root, "root", cgl_rect(0, 0, 319, (int16_t)(BAND_H - 1)),
                  CGL_COLOR_BLACK, CGL_COLOR_BLACK);
    cgl_panel_init(&child, "child", cgl_rect_wh(10, 2, 20, 4), CGL_COLOR_RED, CGL_COLOR_RED);
    cgl_widget_add_child(&root.base, &child.base);

    band_bind(0, BAND_H);

    /* --- no offset: drawn at the frame's original position ------- */
    band_clear();
    cgl_widget_draw_tree(&root.base, &s_canvas);
    CHECK(px(15, 3) == CGL_COLOR_RED, "with no offset (15,3) should be the child widget");
    CHECK(px(115, 3) == CGL_COLOR_BLACK, "with no offset (115,3) is the background, not the child widget");
    CHECK(cgl_widget_hit_test(&root.base, 15, 3) == &child.base, "hit testing should hit the child widget");
    CHECK(cgl_widget_hit_test(&root.base, 115, 3) == &root.base, "with no offset (115,3) should hit the root");
    CHECK(child.base.offset.x == 0, "the initial offset should be 0");

    /* --- translate by +100: both drawing and hit testing must follow ---------*/
    cgl_widget_set_offset(&child.base, 100, 0);
    band_clear();
    cgl_widget_draw_tree(&root.base, &s_canvas);
    CHECK(px(115, 3) == CGL_COLOR_RED, "after an offset of +100, (115,3) should be the child widget");
    CHECK(px(15, 3) == CGL_COLOR_BLACK, "after the offset only the background should be left in the old place (otherwise a ghost remains)");
    CHECK(cgl_widget_hit_test(&root.base, 115, 3) == &child.base,
          "after the offset hit testing must follow it");
    CHECK(cgl_widget_hit_test(&root.base, 15, 3) == &root.base,
          "after the offset the old place must no longer hit the child widget");

    /* --- moved off screen: the whole subtree is culled ------ */
    cgl_widget_set_offset(&child.base, -200, 0);
    band_clear();
    cgl_widget_draw_tree(&root.base, &s_canvas);
    CHECK(px(15, 3) == CGL_COLOR_BLACK, "after moving off screen it must not still draw in the old place");
    CHECK(px(0, 3) == CGL_COLOR_BLACK, "a subtree moved off screen must be culled entirely");
    CHECK(cgl_widget_hit_test(&root.base, 15, 3) == &root.base, "after moving off screen it must not be hit any more");
    cgl_widget_set_offset(&child.base, 0, 0);

    /* --- band clipping: draw only the rows inside this band and touch not one byte outside it ----*/
    {
        int16_t r;

        band_bind(4, 4); /* this band covers screen y 4..7 only, i.e. buffer rows 0..3*/
        band_clear();
        cgl_widget_draw_tree(&root.base, &s_canvas);

        /* the child widget occupies screen y 2..5, so only rows 4..5 fall inside this band*/
        CHECK(px(15, 4) == CGL_COLOR_RED, "the half inside the band should be drawn (y=4)");
        CHECK(px(15, 5) == CGL_COLOR_RED, "the half inside the band should be drawn (y=5)");

        /* the root panel fills the whole band black (its part inside the band), so buffer rows 0..3 should all be black*/
        CHECK(sband(0, 0) == CGL_COLOR_BLACK, "row 0 inside the band should have been painted by the root panel");
        CHECK(sband(3, 0) == CGL_COLOR_BLACK, "the last row inside the band should have been painted by the root panel");

        /* * Buffer rows outside the band (4..7) must keep the sentinel: writing there would corrupt other bands*/
        for (r = 4; r < BAND_H; r++)
        {
            CHECK(sband(r, 15) == SENTINEL, "buffer row %d outside the band must not be written (sentinel corrupted)", r);
            CHECK(sband(r, 0) == SENTINEL, "buffer row %d outside the band must not be written (sentinel corrupted)", r);
        }

        band_bind(0, BAND_H); /* restore the whole-band canvas*/
    }

    /* --- sibling occlusion: after raise both the draw and the hit winner must change ----*/
    cgl_panel_init(&sib, "sib", cgl_rect_wh(10, 2, 20, 4), CGL_COLOR_GREEN, CGL_COLOR_GREEN);
    cgl_widget_add_child(&root.base, &sib.base);
    CHECK(cgl_widget_hit_test(&root.base, 15, 3) == &sib.base,
          "a later sibling should sit on top of an earlier one (hit testing matches the draw order)");
    band_clear();
    cgl_widget_draw_tree(&root.base, &s_canvas);
    CHECK(px(15, 3) == CGL_COLOR_GREEN, "the one drawn first should be covered by the one drawn later");
    cgl_widget_raise(&child.base);
    CHECK(cgl_widget_hit_test(&root.base, 15, 3) == &child.base,
          "after raise the raised widget should take over hit testing");
    band_clear();
    cgl_widget_draw_tree(&root.base, &s_canvas);
    CHECK(px(15, 3) == CGL_COLOR_RED, "after raise the raised widget should be drawn on top");

    /* --- event dispatch: returns false when there is no handler --------------*/
    ev = tp_ev(CGL_EV_KEY_DOWN, 0u);
    CHECK(!cgl_widget_dispatch_event(&root.base, &ev), "it should return false when there is no handler");
}

/**
 * Text must follow the subtree translation too.
 *
 * * This is a regression test for a bug that was really hit: to save a per-pixel
 *   function call, cgl_font.c writes into the buffer through cgl_canvas_row()
 *   directly -- and cgl_canvas_row() wants a **screen row number** and will not add
 *   the pan for it. So in a slide transition "the rectangle moved but the text
 *   stayed put". The per-pixel transition checks in render_host_test.c caught it
 *   (430 shifted text pixels in the new page's top bar); this compresses it into a
 *   unit-level assertion that does not depend on the render pipeline.
 */
static void test_font_offset(void)
{
    static cgl_label_t lab;
    int16_t x;
    int16_t y;
    int lit_plain = 0;
    int lit_shifted = 0;

    (void)printf("\n== F) text follows the subtree translation (cgl_font must not bypass the pan) ==\n");

    cgl_widget_set_damage_sink(NULL, NULL);
    /* Opaque, so the position of the "text block" can be judged from the whole rectangle without background interference*/
    cgl_label_init(&lab, "txt", cgl_rect_wh(10, 1, 40, 16), &cgl_font_8x16, "AB",
                  CGL_COLOR_WHITE, CGL_COLOR_BLACK, true);

    band_bind(0, BAND_H);

    /* Count "white pixels falling inside the primitive coordinates [xa, xb)" -- over every row in the band*/
    /* no offset: the text should land inside the frame*/
    cgl_widget_set_offset(&lab.base, 0, 0);
    band_clear();
    cgl_widget_draw_tree(&lab.base, &s_canvas);
    for (y = 0; y < BAND_H; y++)
    {
        for (x = 10; x < 30; x++)
        {
            if (px(x, y) == CGL_COLOR_WHITE)
            {
                lit_plain++;
            }
        }
    }
    CHECK(lit_plain > 0, "with no offset the text should be drawn in the frame (measured 0 white pixels)");

    /* translate by +100: the whole text must follow*/
    cgl_widget_set_offset(&lab.base, 100, 0);
    band_clear();
    cgl_widget_draw_tree(&lab.base, &s_canvas);
    for (y = 0; y < BAND_H; y++)
    {
        for (x = 10; x < 30; x++)
        {
            if (px(x, y) == CGL_COLOR_WHITE)
            {
                lit_shifted++;
            }
        }
    }
    CHECK(lit_shifted == 0, "after the translation %d text pixels are still left in the old place (the text did not follow)",
          lit_shifted);

    lit_shifted = 0;
    for (y = 0; y < BAND_H; y++)
    {
        for (x = 110; x < 130; x++)
        {
            if (px(x, y) == CGL_COLOR_WHITE)
            {
                lit_shifted++;
            }
        }
    }
    CHECK(lit_shifted == lit_plain,
          "after the translation the text should appear unchanged in the new place (%d pixels, expected %d)",
          lit_shifted, lit_plain);
    (void)printf("  text pixels: %d in the old place, %d in the new place after translating, %d left behind in the old place\n",
                 lit_plain, lit_shifted, 0);
}

/* =========================================================================== */
/* G) Genie warp                                                               */
/* =========================================================================== */

/*
 * A warp transition is a **geometric** warp (there is no full-screen framebuffer to
 * resample), so its correctness rests entirely on three mathematical properties,
 * which are asserted one by one here:
 *
 *   1. at p = 0 it must be an **exact identity** -- otherwise the first frame of
 *      every page change would jitter.
 *   2. the row mapping must be monotonic with a step of at most 1 -- the only
 *      guarantee that scattered writes (target row computed from the source row)
 *      leave no holes, and that the occlusion order is intact after the warp.
 *   3. the conservative bounding box must really cover every mapped point -- the
 *      traverser culls with it, and one missing column cuts a piece off the page.
 *
 * It also draws "the whole funnel filled, not a pixel outside it" and verifies that
 * directly (item 4), because the three properties above are only necessary
 * conditions and do not by themselves guarantee that the combination is right.
 */

#define GENIE_BAND_H 4

static cgl_color_t s_gband[CGL_CFG_SCREEN_W * GENIE_BAND_H];
static cgl_canvas_t s_gcanvas;

static void genie_warp_s(cgl_warp_t *w, uint16_t p, uint16_t s_amp_q8);

static void genie_warp(cgl_warp_t *w, uint16_t p)
{
    genie_warp_s(w, p, 0u);
}

/**
 * The same funnel with the horizontal S trajectory optional: `s_amp_q8 = 0` is the
 * old Genie that sucks in vertically. The mathematical properties must hold
 * **item by item** for both modes (see test_genie_mapping / test_s_curve).
 */
static void genie_warp_s(cgl_warp_t *w, uint16_t p, uint16_t s_amp_q8)
{
    w->src_y0 = 0;
    w->src_h = CGL_CFG_SCREEN_H;
    w->cx = (int16_t)(CGL_CFG_SCREEN_W / 2);
    w->tx = w->cx;
    w->ty = (int16_t)(CGL_CFG_SCREEN_H - 1 + CGL_CFG_GENIE_DROP_PX);
    w->progress_q8 = p;
    w->narrow_q8 = (uint16_t)CGL_CFG_GENIE_NARROW_Q8;
    w->s_amp_q8 = s_amp_q8;
    w->row_step_q8 = (uint16_t)((((uint32_t)256u * 256u) + (uint32_t)w->src_h / 2u) /
                                (uint32_t)w->src_h);
}

/** The S-shaped descriptor: the suck-in point is moved to the bottom right (matching the one nav__warp_setup() produces).*/
static void genie_warp_corner_s(cgl_warp_t *w, uint16_t p, uint16_t s_amp_q8)
{
    genie_warp_s(w, p, s_amp_q8);
    w->tx = (int16_t)(CGL_CFG_SCREEN_W - 1);
    w->ty = (int16_t)(CGL_CFG_SCREEN_H - 1 + CGL_CFG_S_DROP_PX);
}

static void genie_warp_corner(cgl_warp_t *w, uint16_t p)
{
    genie_warp_corner_s(w, p, (uint16_t)CGL_CFG_S_AMP_MAX_Q8);
}

static void genie_bind(int16_t origin_y)
{
    cgl_canvas_init(&s_gcanvas, s_gband, CGL_CFG_SCREEN_W, GENIE_BAND_H, origin_y);
    cgl_canvas_set_clip(&s_gcanvas, cgl_rect(0, origin_y, (CGL_CFG_SCREEN_W - 1),
                                           (int16_t)(origin_y + GENIE_BAND_H - 1)));
}

static void genie_clear(void)
{
    uint32_t i;

    for (i = 0u; i < (uint32_t)(CGL_CFG_SCREEN_W * GENIE_BAND_H); i++)
    {
        s_gband[i] = SENTINEL;
    }
}

/** Probe canvas: only for calling cgl_canvas_map_*().*/
static cgl_canvas_t s_gprobe;

static void genie_probe(const cgl_warp_t *w)
{
    cgl_canvas_init(&s_gprobe, s_gband, CGL_CFG_SCREEN_W, GENIE_BAND_H, 0);
    cgl_canvas_set_warp(&s_gprobe, w);
}

static void test_genie_mapping(void)
{
    cgl_warp_t w;
    uint16_t p;

    (void)printf("\n== G) Genie: the mathematical properties of the warp ==\n");

    for (p = 0u; p <= (uint16_t)CGL_ANIM_ONE_Q8; p = (uint16_t)(p + 8u))
    {
        int16_t y;
        int16_t x;
        bool monotone = true;
        bool step_ok = true;

        genie_warp(&w, p);
        genie_probe(&w);

        /* --- 1) p = 0 must be an exact identity ------------------- */
        if (p == 0u)
        {
            for (y = 0; y < CGL_CFG_SCREEN_H; y++)
            {
                CHECK(cgl_canvas_map_y(&s_gprobe, y) == y,
                      "at p=0 the row mapping must be the identity (y=%d -> %d)", y, cgl_canvas_map_y(&s_gprobe, y));
            }
            for (y = 0; y < CGL_CFG_SCREEN_H; y += 17)
            {
                for (x = 0; x < CGL_CFG_SCREEN_W; x++)
                {
                    CHECK(cgl_canvas_map_x(&s_gprobe, x, y) == x,
                          "at p=0 the column mapping must be the identity (x=%d y=%d -> %d)", x, y,
                          cgl_canvas_map_x(&s_gprobe, x, y));
                }
            }
        }

        /* --- 2) rows monotonic with step in {0,1}; columns monotonic with step <= 1 ----*/
        for (y = 0; y < (int16_t)(CGL_CFG_SCREEN_H - 1); y++)
        {
            int32_t a = cgl_canvas_map_y(&s_gprobe, y);
            int32_t b = cgl_canvas_map_y(&s_gprobe, (int16_t)(y + 1));

            if (b < a)
            {
                monotone = false;
            }
            if ((b - a) > 1)
            {
                step_ok = false; /* a skipped row = a row of the target that is never written (a hole)*/
            }
        }
        CHECK(monotone, "p=%u: the row mapping is not monotonic", (unsigned)p);
        CHECK(step_ok, "p=%u: the row mapping step exceeds 1 (it would leave holes)", (unsigned)p);

        for (y = 0; y < CGL_CFG_SCREEN_H; y += 23)
        {
            int16_t prev = cgl_canvas_map_x(&s_gprobe, 0, y);

            for (x = 1; x < CGL_CFG_SCREEN_W; x++)
            {
                int16_t cur = cgl_canvas_map_x(&s_gprobe, x, y);

                if (cur < prev)
                {
                    monotone = false;
                }
                prev = cur;
            }
        }
        CHECK(monotone, "p=%u: the column mapping is not monotonic (the x order gets scrambled)", (unsigned)p);

        /* --- 3) the conservative bounding box must cover every mapped point ------*/
        {
            cgl_rect_t r = cgl_rect(0, 0, (CGL_CFG_SCREEN_W - 1), (CGL_CFG_SCREEN_H - 1));
            cgl_rect_t box = cgl_canvas_map_rect(&s_gprobe, r);
            bool inside = true;

            for (y = 0; y < CGL_CFG_SCREEN_H; y += 7)
            {
                for (x = 0; x < CGL_CFG_SCREEN_W; x += 7)
                {
                    if (!cgl_rect_contains_point(box, cgl_canvas_map_x(&s_gprobe, x, y),
                                                cgl_canvas_map_y(&s_gprobe, y)))
                    {
                        inside = false;
                    }
                }
            }
            CHECK(inside, "p=%u: map_rect does not contain the mapped points (culling would cut content off)", (unsigned)p);
            if (p == 0u)
            {
                CHECK((box.x0 == r.x0) && (box.x1 == r.x1) &&
                      (box.y0 == r.y0) && (box.y1 == r.y1),
                      "at p=0 map_rect must return the rectangle unchanged");
            }
        }
    }
    (void)printf("   p = 0..256 step 8: identity / monotonic / step<=1 / bounding box all hold\n");

    /* --- 4) the funnel fills the screen, draws nothing outside it, and only shrinks ----*/
    {
        static cgl_panel_t root;
        int16_t prev_rows = -1;
        bool shrinking = true;
        int shape;

        cgl_widget_set_damage_sink(NULL, NULL);
        cgl_panel_init(&root, "genie.root", cgl_rect(0, 0, (CGL_CFG_SCREEN_W - 1),
                                                   (CGL_CFG_SCREEN_H - 1)),
                      CGL_COLOR_WHITE, CGL_COLOR_WHITE);

        /* * Both suck-in modes are run: vertical (s_amp = 0) and the S shape (bottom right + a horizontal trajectory).
           "Filled / continuous / 0 pixels outside the silhouette" are **geometric**
           properties, independent of where the page is sucked into -- so the S shape
           has to pass them item by item as well; testing only the old one is not enough.*/
        for (shape = 0; shape < 2; shape++)
        {
            prev_rows = -1;
            shrinking = true;

        for (p = 0u; p <= (uint16_t)CGL_ANIM_ONE_Q8; p = (uint16_t)(p + 16u))
        {
            int16_t y;
            int16_t rows = 0;

            if (shape == 0)
            {
                genie_warp(&w, p);
            }
            else
            {
                genie_warp_corner(&w, p);
            }
            genie_probe(&w);
            cgl_widget_set_warp(&root.base, &w);

            for (y = 0; y < CGL_CFG_SCREEN_H; y += GENIE_BAND_H)
            {
                int16_t band_h = GENIE_BAND_H;
                int16_t rr;

                if ((int16_t)(y + band_h) > CGL_CFG_SCREEN_H)
                {
                    band_h = (int16_t)(CGL_CFG_SCREEN_H - y);
                }
                genie_bind(y);
                cgl_canvas_set_clip(&s_gcanvas,
                                   cgl_rect(0, y, (CGL_CFG_SCREEN_W - 1),
                                           (int16_t)(y + band_h - 1)));
                genie_clear();
                cgl_widget_draw_tree(&root.base, &s_gcanvas);

                for (rr = y; rr < (int16_t)(y + band_h); rr++)
                {
                    int16_t x;
                    int16_t first = -1;
                    int16_t last = -1;
                    int16_t lit = 0;
                    uint32_t base = (uint32_t)(rr - y) * (uint32_t)CGL_CFG_SCREEN_W;
                    bool want;

                    /*  The **exact** criterion for "this row should have content": map every source row of
                        the whole page and see whether any source row lands on row rr and its
                        mapped horizontal span **intersects** the screen. Dropping the
                        intersection half gives false alarms: the S shape pushes the rows of
                        the middle section to the right, and at p=176 the spans of rows 150..158
                        already lie in x=[321,399] (outside the right edge of the screen), so
                        that row is supposed to draw nothing at all.
                        WARNING: do not use "rr lies inside [map_y(top), map_y(bottom)]" as the
                        criterion either -- that is the old vertical formulation and would raise
                        59 false alarms on the S shape as well.*/
                    {
                        int16_t sy;
                        bool xa_set = false;
                        int16_t xa = 0;
                        int16_t xb = 0;
                        int16_t alo;
                        int16_t ahi;

                        for (sy = w.src_y0; sy < (int16_t)(w.src_y0 + w.src_h); sy++)
                        {
                            int16_t lo;
                            int16_t hi;

                            if (cgl_canvas_map_y(&s_gprobe, sy) != rr)
                            {
                                continue;
                            }
                            lo = cgl_canvas_map_x(&s_gprobe, 0, sy);
                            hi = cgl_canvas_map_x(&s_gprobe, (int16_t)(CGL_CFG_SCREEN_W - 1), sy);
                            if (hi > xb || !xa_set)
                            {
                                xb = hi;
                            }
                            if ((lo < xa) || !xa_set)
                            {
                                xa = lo;
                            }
                            xa_set = true;
                        }

                        alo = (xa < 0) ? 0 : xa;
                        ahi = (xb > (int16_t)(CGL_CFG_SCREEN_W - 1))
                                  ? (int16_t)(CGL_CFG_SCREEN_W - 1)
                                  : xb;
                        want = xa_set && (alo <= ahi);
                    }

                    for (x = 0; x < CGL_CFG_SCREEN_W; x++)
                    {
                        if (s_gband[base + (uint32_t)x] != SENTINEL)
                        {
                            if (first < 0)
                            {
                                first = x;
                            }
                            last = x;
                            lit++;
                        }
                    }

                    if (want)
                    {
                        /* rows that should show up on screen: they must have pixels and must not break in the middle*/
                        CHECK(lit > 0, "p=%u row %d should have content but not a single pixel was drawn",
                              (unsigned)p, rr);
                        CHECK((lit == 0) || ((last - first + 1) == lit),
                              "p=%u row %d has a hole (only %d px drawn across %d px)",
                              (unsigned)p, rr, last - first + 1, lit);
                        rows++;
                    }
                    else
                    {
                        CHECK(lit == 0, "p=%u row %d drew %d pixels outside the silhouette",
                              (unsigned)p, rr, lit);
                    }
                }
            }

            if ((prev_rows >= 0) && (rows > prev_rows))
            {
                shrinking = false;
            }
            prev_rows = rows;

            if (p == 0u)
            {
                CHECK(rows == CGL_CFG_SCREEN_H, "at p=0 the whole screen should be inside the funnel, measured %d rows", rows);
            }
            if (p == (uint16_t)CGL_ANIM_ONE_Q8)
            {
                CHECK(rows == 0, "at p=256 the page should have left the screen completely, measured %d rows left", rows);
            }
        }
        cgl_widget_set_warp(&root.base, NULL);
        CHECK(shrinking, "the funnel can only shrink as it sucks (the row count is monotonically non-increasing in p)");
        (void)printf("  %s: p=0 fills the whole screen; every row is continuous with no holes and 0 pixels outside the silhouette\n",
                     (shape == 0) ? "vertical suck-in" : "S shape (bottom right)");
        }
    }
}

/**
 * The difference between the S shape and a "non-bulging S" (same suck-in point,
 * s_amp = 0) for the same source column and source row.
 *
 * That is S(y) itself. Why it has to be measured this way: for p > 0 every point of
 * the page is moving anyway (the funnel narrows + the centre travels towards the
 * suck-in point), so a criterion like "the top row's map_x is the identity" only
 * holds at p = 0; only **subtracting the two variants** isolates the horizontal S
 * trajectory.
 *
 * WARNING: the reference descriptor must be the one with the **same suck-in point**,
 * just with s_amp = 0. Using the "vertical suck-in" one (suck-in point on the bottom
 * centre line) as the reference subtracts the sum of two things -- "the S trajectory
 * + the suck-in point moving house" -- and it then looks as if S were non-zero at
 * both ends (measured difference: 100 px).
 */
static int32_t s_gain(const cgl_warp_t *corner, const cgl_warp_t *plain, int16_t x, int16_t y)
{
    int32_t a;
    int32_t b;

    genie_probe(plain);
    a = cgl_canvas_map_x(&s_gprobe, x, y);
    genie_probe(corner);
    b = cgl_canvas_map_x(&s_gprobe, x, y);
    return b - a;
}

/**
 * The horizontal trajectory of the S-shaped suck-in: **S is the curve itself**, not
 * "sucking diagonally".
 *
 * Four properties (without any one of them it is not an S):
 *   1. tangent at both ends: the horizontal offset is 0 at both ends of the source
 *      range -- the shape of the first and the last frame is still straight and only
 *      the process bends (this is also the precondition for the exact identity at p=0);
 *   2. a single peak: the contribution of S rises along the source rows to a peak and
 *      only falls after it, and the peak lies inside the source range;
 *   3. adjacent source rows do not cross: x is monotonic in y -- the precondition for
 *      map_rect being conservative and for "no folding fan";
 *   4. the amplitude is bounded: the horizontal offset does not exceed half the screen
 *      width, and s_amp_q8 must not cross the monotonicity bound.
 */
static void test_s_curve(void)
{
    cgl_warp_t w;
    cgl_warp_t plain;
    const int16_t cx = (int16_t)(CGL_CFG_SCREEN_W / 2);
    const int16_t y_top = 0;
    const int16_t y_bot = (int16_t)(CGL_CFG_SCREEN_H - 1);
    uint16_t p = 160u;

    (void)printf("\n== G2) S-shaped suck-in: the shape of the horizontal trajectory ==\n");

    /* --- 0) the amplitude cap is itself the "monotonicity bound": a macro that is too large must be caught by an assertion ----*/
    CHECK((uint32_t)CGL_CFG_S_AMP_MAX_Q8 <= 680u,
          "CGL_CFG_S_AMP_MAX_Q8 = %u exceeds the monotonicity bound 682: the slope bound of the offset function is 6/256, "
          "and any larger amplitude would let adjacent source rows cross",
          (unsigned)CGL_CFG_S_AMP_MAX_Q8);

    genie_warp_corner(&w, p);
    /* Reference: the **same suck-in point**, just without the bulge (s_amp = 0). The only difference left is the horizontal trajectory.*/
    genie_warp_corner_s(&plain, p, 0u);

    /* --- 1) tangent at both ends, bulge in the middle, peak inside and no more than half the screen ----*/
    {
        int32_t d_top = s_gain(&w, &plain, cx, y_top);
        int32_t d_bot = s_gain(&w, &plain, cx, y_bot);
        int16_t y;
        int16_t peak_y = -1;
        int32_t peak_d = -1;

        for (y = y_top; y <= y_bot; y++)
        {
            int32_t d = s_gain(&w, &plain, cx, y);

            if (d > peak_d)
            {
                peak_d = d;
                peak_y = y;
            }
        }
        genie_probe(&w);

        CHECK(d_top == 0, "S must be 0 at the far end of the source range (measured deviation %d px)", (int)d_top);
        /*  The end near the suck-in point may be off by +/-1 px: the rounding direction of the
           two mappings that differ only in s_amp is exactly opposite here (the truncation
           point of k(y) differs), so the difference lands in -1..1. That is integer
           rounding noise, not "the trajectory failed to return to zero".*/
        CHECK((d_bot >= -1) && (d_bot <= 1),
              "S must return to zero at the end near the suck-in point (measured deviation %d px, a +/-1 rounding noise is allowed)", (int)d_bot);
        CHECK(peak_d > 0, "the middle section of S must bulge out (measured peak %d px)", (int)peak_d);
        CHECK(peak_d <= (int32_t)(CGL_CFG_SCREEN_W / 2),
              "the maximum offset of S must not exceed half the screen (measured %d px)", (int)peak_d);
        CHECK((peak_y > y_top) && (peak_y < y_bot),
              "the peak of S must fall **inside** the source range (measured row %d)", (int)peak_y);

        /* The end near the suck-in point has already pulled into the bottom right corner: k has narrowed to its minimum and xc has travelled close to tx*/
        CHECK(cgl_canvas_map_x(&s_gprobe, 0, y_bot) > (int16_t)(CGL_CFG_SCREEN_W / 2),
              "the end near the suck-in point should already have pulled into the right half (actual x=%d)",
              (int)cgl_canvas_map_x(&s_gprobe, 0, y_bot));
    }

    /* --- 2) the contribution of S along the source rows is a single peak: rise, then only fall ----*/
    {
        int16_t y;
        int32_t prev_d = 0;
        int16_t peak_y = -1;
        int32_t peak_d = -1;
        bool started = false;
        bool falling = false;
        bool unimodal = true;

        for (y = y_top; y <= y_bot; y++)
        {
            int32_t d = s_gain(&w, &plain, cx, y);

            /* * The criterion needs a 1 px tolerance; strict monotonicity must not be demanded.
                The reason is not "S is not monotonic" but that **the two mappings round
                independently**: the main term (x-cx)*k is rounded to nearest while the S
                term is truncated, so when s_amp changes the truncation point of k(y)
                changes and the two sides can round in opposite directions on the same row
                -- hence the difference curve occasionally "rises by 1" inside the falling
                section (measured at the shipped amp = 160 over every p: the largest rise is
                1 px, at the end of the section).
                What really has to be rejected is a "second peak": a rise of more than
                2 px, or a further rise after such a rise.*/
            if (started)
            {
                if (!falling)
                {
                    if (d < prev_d)
                    {
                        falling = true; /* past the peak. Only falling is allowed afterwards*/
                    }
                }
                else if (d > (prev_d + 2))
                {
                    unimodal = false; /* a rise of more than 3 px inside the falling section = a real second peak*/
                }
            }
            if (d > peak_d)
            {
                peak_d = d;
                peak_y = y;
            }
            prev_d = d;
            started = true;
        }
        genie_probe(&w);

        CHECK(unimodal, "the contribution of S must \"rise, then only fall\" -- a rise of more than 2 px in the falling section is a second peak");
        /* The peak falls in the middle of the source rows: in warp__s the extremum of f is at
           t ~ 107/256 ~ 42% (see the derivation of f in cgl_canvas.c), which maps back to
           source row 42% of 172 ~ row 72.
           Here only "it falls in the middle section (20%..60%)" is asserted -- the exact row
           number varies with the screen height and the rounding of row_step, and the subject
           of this case is the **shape** (a single peak, both ends at zero).*/
        CHECK((peak_y >= (int16_t)(CGL_CFG_SCREEN_H / 5)) &&
              (peak_y <= (int16_t)(CGL_CFG_SCREEN_H * 3 / 5)),
              "the peak of S should fall in the middle of the source range (actual row %d, expected %d..%d)", (int)peak_y,
              (int)(CGL_CFG_SCREEN_H / 5), (int)(CGL_CFG_SCREEN_H * 3 / 5));
    }

    /* --- 3) adjacent source rows do not cross: for one source column, x is monotonic in the source row (rounding tolerance) ----*/
    {
        /* Use the **left edge** (x = 0) because it is the most sensitive: the centre column
           stays flat for a long time when it is sucked to the right edge (it does not fall,
           so it cannot false-alarm), whereas on the left edge "S pulls inwards" and "the
           funnel travels right" are two opposite forces and a crossing shows up here first.

           WARNING: the tolerance is 5 px, CALIBRATED by measurement rather than guessed.
           The row mapping rounds in two places (rows toward -inf, columns to nearest), so
           even at s_amp_q8 = 0 the worst step is already 2 px; adding the S term at the
           shipped amp = 160 raises the measured worst step to 4 px over a sweep of every p
           and every column. 5 leaves one pixel of headroom.
           The old tolerance of 2 px was never actually true -- it was 4 px at amp = 156
           too, and the coarse sampling used back then (x += 5) simply never landed on the
           worst column. The loop below therefore samples EVERY column.

           Note what this tolerance does and does not measure: it bounds the *rounding*
           staircase, not "is there a real crossing". A real crossing is excluded
           analytically by the warp__s() slope bound (|S'| < 1 <=> amp <= 682; see
           CGL_CFG_S_AMP_MAX_Q8 in cytgl_config.h), which at amp <= 160 has a wide margin.
           A genuine folding fan would show up as rows moving in *opposite* directions --
           steps on the order of the whole page width -- not as a single-digit pixel step. */
        int16_t x;
        int16_t worst = 0;

        for (x = 0; x < CGL_CFG_SCREEN_W; x += 1)
        {
            int16_t y;
            int16_t prev_x = cgl_canvas_map_x(&s_gprobe, x, y_top);

            for (y = (int16_t)(y_top + 1); y <= y_bot; y++)
            {
                int16_t cur = cgl_canvas_map_x(&s_gprobe, x, y);

                if ((int16_t)(prev_x - cur) > worst)
                {
                    worst = (int16_t)(prev_x - cur);
                }
                prev_x = cur;
            }
        }
        CHECK(worst <= 5, "S makes adjacent source rows cross (worst step -%d px, tolerance 5) -- is s_amp_q8 tuned too large?",
              (int)worst);
    }

    /* --- 4) bounding box: the whole page must be contained on every most-bulged frame, and p=0 must be unchanged ----*/
    {
        cgl_rect_t r = cgl_rect(0, 0, (CGL_CFG_SCREEN_W - 1), (CGL_CFG_SCREEN_H - 1));

        for (p = 0u; p <= (uint16_t)CGL_ANIM_ONE_Q8; p = (uint16_t)(p + 8u))
        {
            cgl_rect_t box;
            bool inside = true;
            int16_t y;
            int16_t x;

            genie_warp_corner(&w, p);
            genie_probe(&w);
            box = cgl_canvas_map_rect(&s_gprobe, r);
            for (y = 0; y < CGL_CFG_SCREEN_H; y++)
            {
                for (x = 0; x < CGL_CFG_SCREEN_W; x += 3)
                {
                    if (!cgl_rect_contains_point(box, cgl_canvas_map_x(&s_gprobe, x, y),
                                                cgl_canvas_map_y(&s_gprobe, y)))
                    {
                        inside = false;
                    }
                }
            }
            CHECK(inside, "p=%u: the S-shaped map_rect does not contain the mapped points", (unsigned)p);
            if (p == 0u)
            {
                /* p=0 is an exact identity (p is a factor of the S term), so even when there is margin
                   outside, the **content itself** must land on the original rectangle. What is
                   asserted here is that "the content is not fuzzy": the origin is inside the
                   box, the box is not empty, and it must not shrink inside the original rectangle.*/
                CHECK((box.x0 <= r.x0) && (box.x1 >= r.x1) && (box.y0 == r.y0) &&
                      (box.y1 == r.y1),
                      "at p=0 the S-shaped map_rect must cover the original rectangle (measured [%d,%d]x[%d,%d])",
                      (int)box.x0, (int)box.x1, (int)box.y0, (int)box.y1);
                CHECK((box.x1 - box.x0) <= (int16_t)(r.x1 - r.x0 + 64),
                      "at p=0 the margin must not exceed twice one sampling step (measured width %d)",
                      (int)(box.x1 - box.x0));
            }
        }
    }

    (void)printf("  tangent at both ends / a single peak in the middle / adjacent rows do not cross / a conservative bounding box all hold\n");
}

/**
 * Whether the two directions of the same S trajectory inside the **navigator** use one
 * and the same geometry.
 *
 * S_IN runs backwards, so the frozen shape at its start (progress = 256) must be
 * exactly the same as S_OUT at its end (progress = 256) -- any difference at all and
 * "pouring out" is not the inverse of "sucking in", and the two directions would feel
 * different. So the map_rect of the two moments is compared point by point.
 */
static void test_s_roundtrip(void)
{
    static cgl_page_nav_t nav;
    static tpage_t host_page;
    static tpage_t guest_page;
    cgl_rect_t area = cgl_rect(0, 0, (int16_t)(CGL_CFG_SCREEN_W - 1), (int16_t)(CGL_CFG_SCREEN_H - 1));
    const cgl_warp_t *w_out;
    const cgl_warp_t *w_in;
    cgl_warp_t snap_out;
    cgl_warp_t snap_in;
    cgl_warp_t probe_out;
    cgl_warp_t probe_in;
    cgl_canvas_t canvas;
    static cgl_color_t buf[CGL_CFG_SCREEN_W * 4];
    int16_t x;
    int16_t y;
    bool same = true;

    (void)printf("\n== H3) S shape: running backwards must be the inverse of running forwards ==\n");

    cgl_clock_init(&s_clk, 16u, 100u);
    cgl_page_nav_init(&nav, "test.sround", area);
    tp_init(&host_page, "H", true);
    tp_init(&guest_page, "G", true);

    (void)cgl_page_nav_reset(&nav, &host_page.base);
    tp_settle(&nav);

    /* S_OUT: run to the very end and copy down the descriptor of the progress = 256 frame*/
    CHECK(cgl_page_nav_request(&nav, CGL_NAV_PUSH, &guest_page.base, CGL_TRANS_NONE, NULL),
          "PUSH G (instant)");
    tp_settle(&nav);
    CHECK(cgl_page_nav_request(&nav, CGL_NAV_POP, NULL, CGL_TRANS_S_OUT, NULL), "POP (S_OUT)");
    tp_apply(&nav);
    w_out = cgl_page_nav_warp(&nav);
    CHECK(w_out != NULL, "S_OUT should provide a warp descriptor");
    if (w_out == NULL)
    {
        return;
    }
    /* Force the progress to p = 256 by hand (really reaching that frame would finish the transition and remove the warp first) and copy the whole descriptor down*/
    nav.warp.progress_q8 = (uint16_t)CGL_ANIM_ONE_Q8;
    snap_out = nav.warp;
    tp_settle(&nav);

    (void)cgl_page_nav_reset(&nav, &host_page.base);
    tp_settle(&nav);
    CHECK(cgl_page_nav_request(&nav, CGL_NAV_PUSH, &guest_page.base, CGL_TRANS_S_IN, NULL),
          "PUSH G (S_IN)");
    tp_apply(&nav);
    w_in = cgl_page_nav_warp(&nav);
    CHECK(w_in != NULL, "S_IN should provide a warp descriptor");
    if (w_in == NULL)
    {
        return;
    }
    nav.warp.progress_q8 = (uint16_t)CGL_ANIM_ONE_Q8;
    snap_in = nav.warp;
    tp_settle(&nav);

    /* the static fields of the two descriptors must match (one and the same geometry)*/
    CHECK((snap_out.cx == snap_in.cx) && (snap_out.tx == snap_in.tx) &&
          (snap_out.ty == snap_in.ty) && (snap_out.src_h == snap_in.src_h) &&
          (snap_out.s_amp_q8 == snap_in.s_amp_q8) && (snap_out.narrow_q8 == snap_in.narrow_q8),
          "S_OUT and S_IN must share the same geometry (same cx/tx/ty/amplitude)");

    /* the point-by-point mapping must match at the same progress*/
    cgl_canvas_init(&canvas, buf, CGL_CFG_SCREEN_W, 4, 0);
    probe_out = snap_out;
    probe_in = snap_in;
    for (y = 0; y < (int16_t)CGL_CFG_SCREEN_H; y += 7)
    {
        for (x = 0; x < (int16_t)CGL_CFG_SCREEN_W; x += 11)
        {
            int16_t a;
            int16_t b;

            cgl_canvas_set_warp(&canvas, &probe_out);
            a = cgl_canvas_map_x(&canvas, x, y);
            cgl_canvas_set_warp(&canvas, &probe_in);
            b = cgl_canvas_map_x(&canvas, x, y);
            if (a != b)
            {
                same = false;
            }
        }
    }
    CHECK(same, "at the same progress the S_OUT and S_IN mappings must match point by point (otherwise playing backwards is not the inverse)");

    (void)printf("  the S_OUT final frame and the S_IN start frame are one and the same geometry, identical point by point\n");
}

/**
 * The navigator layer of the S-shaped suck-in: it must be treated as a member of the
 * "suck-in family", not as an empty shell that neither translates nor warps.
 *
 * * What is locked here is a **wiring** property: every place in `nav__is_genie()` /
 *   `nav__warp_setup()` / `nav__layout()` that enumerates "suck-in" must recognise
 *   these two new types. Missing any one of them shows up as "the page plays the whole
 *   transition without moving" (or with the direction reversed), and sections G/G2
 *   only test geometry, so they cannot catch a wiring error like this.
 */
static void test_s_nav(void)
{
    static cgl_page_nav_t nav;
    static tpage_t a_page;
    static tpage_t b_page;
    cgl_rect_t area = cgl_rect(0, 0, (CGL_CFG_SCREEN_W - 1), (CGL_CFG_SCREEN_H - 1));
    const cgl_warp_t *wp;

    (void)printf("\n== H2) S-shaped suck-in: navigation wiring and default target point ==\n");

    /* --- the default table: the same duration/curve as Genie, but each in its own slot ----*/
    cgl_clock_init(&s_clk, 16u, 100u);
    cgl_page_nav_init(&nav, "test.scurve", area);
    tp_init(&a_page, "A", true);
    tp_init(&b_page, "B", true);

    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_S_OUT) == (uint32_t)CGL_CFG_GENIE_MS,
          "the default S_OUT duration should be the same as Genie (%u ms), actually %u",
          (unsigned)CGL_CFG_GENIE_MS, (unsigned)cgl_page_nav_duration(&nav, CGL_TRANS_S_OUT));
    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_S_IN) == (uint32_t)CGL_CFG_GENIE_MS,
          "the default S_IN duration should be the same as Genie");
    CHECK(cgl_page_nav_ease(&nav, CGL_TRANS_S_OUT) == (cgl_ease_t)CGL_CFG_S_OUT_EASE,
          "the default S_OUT curve should be CGL_CFG_S_OUT_EASE");
    CHECK(cgl_page_nav_ease(&nav, CGL_TRANS_S_IN) == (cgl_ease_t)CGL_CFG_S_IN_EASE,
          "the default S_IN curve should be CGL_CFG_S_IN_EASE");
    /* changing one type must not touch the neighbouring slot*/
    cgl_page_nav_set_transition_duration(&nav, CGL_TRANS_S_OUT, 48u);
    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_S_IN) == (uint32_t)CGL_CFG_GENIE_MS,
          "changing S_OUT must not change S_IN");
    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_GENIE_OUT) == (uint32_t)CGL_CFG_GENIE_MS,
          "changing S_OUT must not change GENIE_OUT");
    cgl_page_nav_set_transition_duration(&nav, CGL_TRANS_S_OUT, (uint32_t)CGL_CFG_GENIE_MS);

    (void)cgl_page_nav_reset(&nav, &a_page.base);
    tp_settle(&nav);

    /* --- S_OUT: it sucks the **leaving page**, the page below stays visible, and the suck-in point is at the bottom right ----*/
    CHECK(cgl_page_nav_request(&nav, CGL_NAV_PUSH, &b_page.base, CGL_TRANS_NONE, NULL),
          "PUSH B (instant)");
    tp_settle(&nav);
    CHECK(cgl_page_nav_request(&nav, CGL_NAV_POP, NULL, CGL_TRANS_S_OUT, NULL),
          "POP (explicit S_OUT)");
    tp_apply(&nav);
    CHECK(nav.trans.trans == (uint8_t)CGL_TRANS_S_OUT, "the type should be S_OUT, actually %u",
          (unsigned)nav.trans.trans);
    wp = cgl_page_nav_warp(&nav);
    CHECK(wp != NULL, "a warp descriptor must be available during S_OUT (which shows it is treated as part of the suck-in family)");
    if (wp != NULL)
    {
        CHECK(b_page.root.base.warp == wp, "S_OUT should warp the **leaving page**");
        CHECK(a_page.root.base.warp == NULL, "S_OUT must not touch the page below");
        CHECK(wp->s_amp_q8 != 0u, "the horizontal S amplitude of S_OUT must not be 0 (that would be the vertical suck-in)");
        CHECK((uint32_t)wp->s_amp_q8 <= (uint32_t)CGL_CFG_S_AMP_MAX_Q8,
              "the S amplitude must be clamped inside the upper bound (actual %u, bound %u)",
              (unsigned)wp->s_amp_q8, (unsigned)CGL_CFG_S_AMP_MAX_Q8);
        CHECK(wp->tx >= (int16_t)(CGL_CFG_SCREEN_W / 2),
              "the S_OUT suck-in point should be in the right half (tx=%d)", (int)wp->tx);
        CHECK(wp->ty > (int16_t)(CGL_CFG_SCREEN_H - 1),
              "the S_OUT suck-in point should already be below the bottom edge of the screen (ty=%d)", (int)wp->ty);
        CHECK(wp->progress_q8 == 0u, "the S_OUT progress must start at 0 (a reversed direction would start at 256)");
    }
    /* The other direction: S_IN must count back from 256, and it warps the **entering page***/
    tp_settle(&nav);
    CHECK(cgl_page_nav_request(&nav, CGL_NAV_PUSH, &b_page.base, CGL_TRANS_S_IN, NULL),
          "PUSH B (explicit S_IN)");
    tp_apply(&nav);
    wp = cgl_page_nav_warp(&nav);
    CHECK(wp != NULL, "a warp descriptor must be available during S_IN");
    if (wp != NULL)
    {
        CHECK(b_page.root.base.warp == wp, "S_IN should warp the **entering page**");
        CHECK(a_page.root.base.warp == NULL, "S_IN must not touch the leaving page");
        CHECK(wp->progress_q8 > 0u, "the S_IN progress should count back from 256 (a reversed direction would start at 0)");
        CHECK(wp->progress_q8 <= (uint16_t)(CGL_ANIM_ONE_Q8 - 16),
              "the frame that applies the request already advances the progress (actual progress %u, starting from 256)",
              (unsigned)wp->progress_q8);
        CHECK(wp->s_amp_q8 != 0u, "S_IN must have a horizontal S amplitude as well");
    }

    /* --- finishing: the warp must be removed cleanly, not leaving a "being sucked" state for the next frame ----*/
    tp_settle(&nav);
    CHECK(cgl_page_nav_warp(&nav) == NULL, "the warp must be invalid after the S-shaped transition ends");
    CHECK(b_page.root.base.warp == NULL, "the entering page's warp must be removed after the S-shaped transition ends");
    CHECK(a_page.root.base.warp == NULL, "the page below must not carry a warp after the S-shaped transition ends either");

    (void)cgl_page_nav_reset(&nav, &a_page.base);
    tp_settle(&nav);
    (void)printf("  S_OUT/S_IN take the same path as the suck-in family; the suck-in point is at the bottom right and the warp is fully cleared at the end\n");
}

/**
 * The lifecycle of a GENIE transition inside the navigator: who gets warped, who
 * shows below, and how the layering is restored.
 */
static void test_genie_nav(void)
{
    static cgl_page_nav_t nav;
    static tpage_t a_page;
    static tpage_t b_page;
    cgl_rect_t area = cgl_rect(0, 0, (CGL_CFG_SCREEN_W - 1), (CGL_CFG_SCREEN_H - 1));
    const cgl_warp_t *wp;

    (void)printf("\n== H) Genie: layering and lifecycle during a transition ==\n");

    cgl_clock_init(&s_clk, 16u, 100u);
    cgl_page_nav_init(&nav, "test.genie", area);
    tp_init(&a_page, "A", true);
    tp_init(&b_page, "B", true);

    (void)cgl_page_nav_reset(&nav, &a_page.base);
    tp_settle(&nav);
    CHECK(!cgl_page_nav_busy(&nav), "it must not still be busy after RESET");

    /* --- PUSH's default transition is "pour out of the suck-in point" ---------*/
    CHECK(cgl_page_nav_push(&nav, &b_page.base), "PUSH B should be accepted");
    tp_tick(&nav, 16u);
    CHECK(nav.trans.trans == (uint8_t)CGL_TRANS_GENIE_IN,
          "PUSH's default transition should be GENIE_IN, actually %u", (unsigned)nav.trans.trans);
    wp = cgl_page_nav_warp(&nav);
    CHECK(wp != NULL, "a warp descriptor should be available during GENIE_IN");
    CHECK(b_page.root.base.warp == wp, "GENIE_IN should warp the **entering page**");
    CHECK(a_page.root.base.warp == NULL, "GENIE_IN must not touch the leaving page");
    CHECK(b_page.root.base.flags & CGL_WF_VISIBLE, "the entering page must be visible");
    CHECK(a_page.root.base.flags & CGL_WF_VISIBLE,
          "during GENIE_IN the page below must stay visible (otherwise the window pouring out is empty underneath)");
    if (wp != NULL)
    {
        CHECK(wp->progress_q8 > 0u, "the GENIE_IN progress should count back from 256");
        CHECK(wp->ty > (int16_t)(CGL_CFG_SCREEN_H - 1),
              "the default suck-in point should be below the bottom of the screen (ty=%d)", wp->ty);
    }
    tp_settle(&nav);
    CHECK(cgl_page_nav_warp(&nav) == NULL, "the warp descriptor must be invalid after the transition ends");
    CHECK(b_page.root.base.warp == NULL, "the entering page's warp must be removed after the transition ends");
    CHECK((b_page.root.base.offset.x == 0) && (b_page.root.base.offset.y == 0),
          "a warp transition must not leave an offset behind");
    CHECK(nav.host.last_child != NULL, "the host should still have a child widget");

    /* --- POP's default transition is "sucked into the suck-in point" ---------*/
    CHECK(cgl_page_nav_back(&nav), "POP should be accepted");
    tp_tick(&nav, 16u);
    CHECK(nav.trans.trans == (uint8_t)CGL_TRANS_GENIE_OUT,
          "POP's default transition should be GENIE_OUT, actually %u", (unsigned)nav.trans.trans);
    wp = cgl_page_nav_warp(&nav);
    CHECK(wp != NULL, "a warp descriptor should be available during GENIE_OUT");
    CHECK(b_page.root.base.warp == wp, "GENIE_OUT should warp the **leaving page**");
    CHECK(a_page.root.base.warp == NULL, "GENIE_OUT must not touch the page below");
    CHECK(a_page.root.base.flags & CGL_WF_VISIBLE, "while the leaving page is being sucked away, the page below must stay visible the whole time");
    if (wp != NULL)
    {
        /* The page being sucked in must sit above the static one, otherwise it would be covered as soon as it moves*/
        cgl_widget_t *it;
        int seen_warped = 0;
        int seen_still = 0;
        int idx = 0;

        for (it = nav.host.first_child; it != NULL; it = it->next)
        {
            idx++;
            if (it == &b_page.root.base)
            {
                seen_warped = idx;
            }
            if (it == &a_page.root.base)
            {
                seen_still = idx;
            }
        }
        CHECK((seen_warped > 0) && (seen_still > 0) && (seen_warped > seen_still),
              "the page being sucked in must be drawn after the page below (a wrong layer order makes the warp invisible)");
    }
    tp_settle(&nav);
    CHECK(cgl_page_nav_warp(&nav) == NULL, "the warp must be removed after GENIE_OUT ends");
    CHECK(b_page.root.base.warp == NULL, "the leaving page's warp must be removed after GENIE_OUT ends");
    CHECK(cgl_page_nav_depth(&nav) == 1u, "after POP the stack depth should be 1");
    if (nav.host.first_child != NULL)
    {
        /* Once it has finished, the top page should be back on top (only it is left above the fade mask)*/
        CHECK(nav.host.last_child != NULL, "the host's child chain must not be empty");
    }
    (void)printf("  default transitions: PUSH -> GENIE_IN, POP -> GENIE_OUT; warp/offset fully cleared at the end\n");
}

/**
 * Transition durations: each type is independent, and the duration really decides
 * "how many frames".
 *
 * * Assert the frame count rather than the field: a wrong field only means a wrong
 *   value was stored, but a wrong frame count means the duration was never used at
 *   all (for example some path reads a global again).
 */
static int tp_frames_of(cgl_page_nav_t *n)
{
    int frames = 0;

    do
    {
        tp_tick(n, 16u); /* the first frame is the one that "applies the request"*/
        frames++;
    } while (cgl_page_nav_busy(n) && (frames < 400));

    return frames;
}

static void test_transition_duration(void)
{
    static cgl_page_nav_t nav;
    static tpage_t a_page;
    static tpage_t b_page;
    cgl_rect_t area = cgl_rect(0, 0, (CGL_CFG_SCREEN_W - 1), (CGL_CFG_SCREEN_H - 1));

    (void)printf("\n== I) Transition duration: each type is independent ==\n");

    cgl_clock_init(&s_clk, 16u, 100u);
    cgl_page_nav_init(&nav, "test.duration", area);
    tp_init(&a_page, "A", true);
    tp_init(&b_page, "B", true);

    /* --- the default table: instant 0, suction uses CGL_CFG_GENIE_MS, the rest CGL_CFG_TRANSITION_MS ---*/
    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_NONE) == 0u, "NONE's duration should be 0 (instant)");
    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_GENIE_OUT) == (uint32_t)CGL_CFG_GENIE_MS,
          "GENIE_OUT should default to CGL_CFG_GENIE_MS (%u), actually %u",
          (unsigned)CGL_CFG_GENIE_MS, (unsigned)cgl_page_nav_duration(&nav, CGL_TRANS_GENIE_OUT));
    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_GENIE_IN) == (uint32_t)CGL_CFG_GENIE_MS,
          "GENIE_IN should default to CGL_CFG_GENIE_MS");
    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_SLIDE_LEFT) == (uint32_t)CGL_CFG_TRANSITION_MS,
          "SLIDE_LEFT should default to CGL_CFG_TRANSITION_MS");
    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_FADE) == (uint32_t)CGL_CFG_TRANSITION_MS,
          "FADE should default to CGL_CFG_TRANSITION_MS");
    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_COUNT) == 0u, "an out-of-range index should return 0 instead of reading garbage");

    /* --- changing one type does not affect the others --------- */
    cgl_page_nav_set_transition_duration(&nav, CGL_TRANS_GENIE_OUT, 48u);
    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_GENIE_OUT) == 48u, "GENIE_OUT should be changed to 48");
    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_GENIE_IN) == (uint32_t)CGL_CFG_GENIE_MS,
          "changing GENIE_OUT must not change GENIE_IN");
    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_SLIDE_LEFT) == (uint32_t)CGL_CFG_TRANSITION_MS,
          "changing suction must not change the slide");

    /* --- duration = how many frames (16 ms/frame) -----------------------
       WARNING: the frame that applies the request **eats one dt itself** (nav__frame
       applies and immediately ticks), so "ceil(ms/16)" is the frame count.*/
    CHECK(cgl_page_nav_reset(&nav, &a_page.base), "RESET A");
    (void)tp_frames_of(&nav);
    CHECK(!cgl_page_nav_busy(&nav), "it must not be busy after RESET");

    cgl_page_nav_set_transition_duration(&nav, CGL_TRANS_GENIE_IN, 32u);
    cgl_page_nav_set_transition_duration(&nav, CGL_TRANS_SLIDE_LEFT, 16u);
    cgl_page_nav_set_transition_duration(&nav, CGL_TRANS_GENIE_OUT, 48u);

    CHECK(cgl_page_nav_push(&nav, &b_page.base), "PUSH B (default GENIE_IN = 32 ms)");
    {
        int f = tp_frames_of(&nav);
        CHECK(f == 2, "32 ms / 16 ms should take 2 frames, actually %d frames", f);
    }

    CHECK(cgl_page_nav_back(&nav), "POP (GENIE_OUT = 48 ms)");
    {
        int f = tp_frames_of(&nav);
        CHECK(f == 3, "48 ms / 16 ms should take 3 frames, actually %d frames", f);
    }

    CHECK(cgl_page_nav_request(&nav, CGL_NAV_PUSH, &b_page.base, CGL_TRANS_SLIDE_LEFT, NULL),
          "PUSH B (explicit SLIDE_LEFT = 16 ms)");
    {
        int f = tp_frames_of(&nav);
        CHECK(f == 1, "16 ms / 16 ms should take 1 frame, actually %d frames", f);
    }

    /* --- 0 ms = this type is instant (the request is still applied at a frame boundary, it just has no animation) ----*/
    cgl_page_nav_set_transition_duration(&nav, CGL_TRANS_GENIE_OUT, 0u);
    CHECK(cgl_page_nav_back(&nav), "POP (GENIE_OUT = 0 ms)");
    {
        int f = tp_frames_of(&nav);
        CHECK(f == 1, "0 ms should take only the frame that applies the request, actually %d frames", f);
    }
    CHECK(!cgl_page_nav_busy(&nav), "it must not still be busy after an instant transition");
    CHECK(cgl_page_nav_warp(&nav) == NULL, "an instant suction transition must not leave a warp behind");

    /* --- AUTO means "all"; set_duration() is all too (neither touches NONE) ----*/
    cgl_page_nav_set_transition_duration(&nav, CGL_TRANS_AUTO, 64u);
    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_SLIDE_LEFT) == 64u, "AUTO should be equivalent to changing all of them");
    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_GENIE_IN) == 64u, "AUTO should be equivalent to changing all of them");
    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_NONE) == 0u,
          "changing all must not change NONE away from 0 (NONE has no animation; that slot is only there for observation)");

    cgl_page_nav_set_duration(&nav, 300u);
    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_FADE) == 300u, "set_duration should change all of them");
    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_NONE) == 0u, "set_duration must not change NONE either");

    /* clamped to the uint16_t maximum, not truncated to a fraction*/
    cgl_page_nav_set_transition_duration(&nav, CGL_TRANS_FADE, 100000u);
    CHECK(cgl_page_nav_duration(&nav, CGL_TRANS_FADE) == 65535u,
          "an over-range value should be clamped to 65535, actually %u", (unsigned)cgl_page_nav_duration(&nav, CGL_TRANS_FADE));

    (void)printf("  defaults: suction %u ms / others %u ms; changing one type does not affect the others;"
                 "32/48/16/0 ms take 2/3/1/1 frames respectively\n",
                 (unsigned)CGL_CFG_GENIE_MS, (unsigned)CGL_CFG_TRANSITION_MS);
}

/**
 * Transition easing: each type is independent, and the curve **really acts on the
 * animation**.
 *
 * * Assert "the progress at a given moment" rather than only the field: a wrong field
 *   only means a wrong value was stored, but a wrong progress means the curve was
 *   never used at all (for example nav__start() writes a constant back again).
 */
static void test_transition_ease(void)
{
    static cgl_page_nav_t nav;
    static tpage_t a_page;
    static tpage_t b_page;
    cgl_rect_t area = cgl_rect(0, 0, (CGL_CFG_SCREEN_W - 1), (CGL_CFG_SCREEN_H - 1));
    int32_t linear_v;
    int32_t in_quad_v;

    (void)printf("\n== J) Transition easing: each type is independent (and really acts on the animation) ==\n");

    cgl_clock_init(&s_clk, 16u, 100u);
    cgl_page_nav_init(&nav, "test.ease", area);
    tp_init(&a_page, "A", true);
    tp_init(&b_page, "B", true);

    /* --- the default table: NONE does not shape, the two suction directions have their own, the rest share the global value ----*/
    CHECK(cgl_page_nav_ease(&nav, CGL_TRANS_NONE) == CGL_EASE_LINEAR,
          "NONE has no animation, so its default curve should be LINEAR (no shaping)");
    CHECK(cgl_page_nav_ease(&nav, CGL_TRANS_GENIE_OUT) == (cgl_ease_t)CGL_CFG_GENIE_OUT_EASE,
          "GENIE_OUT should default to CGL_CFG_GENIE_OUT_EASE (%d), actually %d",
          (int)CGL_CFG_GENIE_OUT_EASE, (int)cgl_page_nav_ease(&nav, CGL_TRANS_GENIE_OUT));
    CHECK(cgl_page_nav_ease(&nav, CGL_TRANS_GENIE_IN) == (cgl_ease_t)CGL_CFG_GENIE_IN_EASE,
          "GENIE_IN should default to CGL_CFG_GENIE_IN_EASE (%d), actually %d",
          (int)CGL_CFG_GENIE_IN_EASE, (int)cgl_page_nav_ease(&nav, CGL_TRANS_GENIE_IN));
    CHECK(cgl_page_nav_ease(&nav, CGL_TRANS_SLIDE_LEFT) == (cgl_ease_t)CGL_CFG_TRANSITION_EASE,
          "SLIDE_LEFT should default to CGL_CFG_TRANSITION_EASE");
    CHECK(cgl_page_nav_ease(&nav, CGL_TRANS_FADE) == (cgl_ease_t)CGL_CFG_TRANSITION_EASE,
          "FADE should default to CGL_CFG_TRANSITION_EASE");
    CHECK(cgl_page_nav_ease(&nav, CGL_TRANS_COUNT) == CGL_EASE_LINEAR,
          "an out-of-range index should return LINEAR instead of reading garbage");

    /* --- changing one type does not affect the others --------- */
    cgl_page_nav_set_transition_ease(&nav, CGL_TRANS_GENIE_OUT, CGL_EASE_OUT_BACK);
    CHECK(cgl_page_nav_ease(&nav, CGL_TRANS_GENIE_OUT) == CGL_EASE_OUT_BACK,
          "GENIE_OUT should be changed to OUT_BACK, actually %d",
          (int)cgl_page_nav_ease(&nav, CGL_TRANS_GENIE_OUT));
    CHECK(cgl_page_nav_ease(&nav, CGL_TRANS_GENIE_IN) == (cgl_ease_t)CGL_CFG_GENIE_IN_EASE,
          "changing GENIE_OUT must not change GENIE_IN");
    CHECK(cgl_page_nav_ease(&nav, CGL_TRANS_SLIDE_LEFT) == (cgl_ease_t)CGL_CFG_TRANSITION_EASE,
          "changing suction must not change the slide");

    /* --- out of range: the curve is clamped to LINEAR and an illegal type is silently ignored (no memory corruption) ----*/
    cgl_page_nav_set_transition_ease(&nav, CGL_TRANS_FADE, (cgl_ease_t)CGL_EASE_COUNT);
    CHECK(cgl_page_nav_ease(&nav, CGL_TRANS_FADE) == CGL_EASE_LINEAR,
          "an out-of-range curve should be clamped to LINEAR, actually %d", (int)cgl_page_nav_ease(&nav, CGL_TRANS_FADE));
    cgl_page_nav_set_transition_ease(&nav, CGL_TRANS_COUNT, CGL_EASE_OUT_BACK);
    CHECK(cgl_page_nav_ease(&nav, CGL_TRANS_GENIE_OUT) == CGL_EASE_OUT_BACK,
          "an out-of-range type must not write into other slots");

    /* --- AUTO means "all"; set_ease() is all too (neither touches NONE) ----*/
    cgl_page_nav_set_transition_ease(&nav, CGL_TRANS_AUTO, CGL_EASE_IN_OUT_QUAD);
    CHECK(cgl_page_nav_ease(&nav, CGL_TRANS_SLIDE_LEFT) == CGL_EASE_IN_OUT_QUAD,
          "AUTO should be equivalent to changing all of them");
    CHECK(cgl_page_nav_ease(&nav, CGL_TRANS_GENIE_IN) == CGL_EASE_IN_OUT_QUAD,
          "AUTO should be equivalent to changing all of them");
    CHECK(cgl_page_nav_ease(&nav, CGL_TRANS_NONE) == CGL_EASE_LINEAR,
          "changing all must not change NONE away from LINEAR");

    cgl_page_nav_set_ease(&nav, CGL_EASE_OUT_BACK);
    CHECK(cgl_page_nav_ease(&nav, CGL_TRANS_FADE) == CGL_EASE_OUT_BACK, "set_ease should change all of them");
    CHECK(cgl_page_nav_ease(&nav, CGL_TRANS_NONE) == CGL_EASE_LINEAR,
          "set_ease must not change NONE either");

    /* --- * the curve really acts on the animation: the progress at a given moment must differ ----
       Use a 160 ms slide transition (= 10 frames) and read the progress after 2 frames:
         LINEAR  -> p = 32/160 = 0.2       -> value ~ 51
         IN_QUAD -> 0.2^2 = 0.04           -> value ~ 10
       Assert the inequality rather than a concrete value: the exact value of the curve
       is the business of the cgl_anim section; what is locked here is only "the
       navigator passes the curve from the table down".*/
    cgl_page_nav_set_transition_duration(&nav, CGL_TRANS_SLIDE_LEFT, 160u);

    cgl_page_nav_set_transition_ease(&nav, CGL_TRANS_SLIDE_LEFT, CGL_EASE_LINEAR);
    CHECK(cgl_page_nav_reset(&nav, &a_page.base), "RESET A");
    tp_settle(&nav);
    CHECK(cgl_page_nav_request(&nav, CGL_NAV_PUSH, &b_page.base, CGL_TRANS_SLIDE_LEFT, NULL),
          "PUSH B (SLIDE_LEFT / LINEAR)");
    tp_tick(&nav, 16u);
    CHECK(nav.trans.anim.ease == (uint8_t)CGL_EASE_LINEAR,
          "the transition tween should hold LINEAR, actually %u", (unsigned)nav.trans.anim.ease);
    tp_tick(&nav, 16u);
    linear_v = cgl_anim_value(&nav.trans.anim);
    tp_settle(&nav);

    cgl_page_nav_set_transition_ease(&nav, CGL_TRANS_SLIDE_LEFT, CGL_EASE_IN_QUAD);
    CHECK(cgl_page_nav_back(&nav), "POP back to A");
    tp_settle(&nav);
    CHECK(cgl_page_nav_request(&nav, CGL_NAV_PUSH, &b_page.base, CGL_TRANS_SLIDE_LEFT, NULL),
          "PUSH B (SLIDE_LEFT / IN_QUAD)");
    tp_tick(&nav, 16u);
    CHECK(nav.trans.anim.ease == (uint8_t)CGL_EASE_IN_QUAD,
          "the transition tween should hold IN_QUAD, actually %u", (unsigned)nav.trans.anim.ease);
    tp_tick(&nav, 16u);
    in_quad_v = cgl_anim_value(&nav.trans.anim);
    tp_settle(&nav);

    CHECK(linear_v > 0,
          "after two frames the LINEAR progress should be > 0 (actual %d) -- otherwise the inequality below is meaningless",
          (int)linear_v);
    CHECK(linear_v > in_quad_v,
          "IN_QUAD must be slower than LINEAR early on: LINEAR=%d vs IN_QUAD=%d (is the curve not used?)",
          (int)linear_v, (int)in_quad_v);

    (void)printf("  defaults: NONE=LINEAR, GENIE_OUT=%d, GENIE_IN=%d, others=%d;"
                 "after 2 frames LINEAR=%d vs IN_QUAD=%d\n",
                 (int)CGL_CFG_GENIE_OUT_EASE, (int)CGL_CFG_GENIE_IN_EASE,
                 (int)CGL_CFG_TRANSITION_EASE, (int)linear_v, (int)in_quad_v);
}

/**
 * A page's own transition: **within one navigation the page that "moves" decides**.
 *
 * * Assert "it really was used in the end" (`nav.trans.trans` / `anim.ease` /
 *   `anim.dur_ms`, plus the progress at a given moment), not only the field on the
 *   page: writing the field correctly while the navigator never reads it is as good
 *   as configuring nothing.
 *
 * Every small scenario starts with RESET -- RESET destroys the whole stack, so the
 * same pages can be created/destroyed repeatedly (which also locks down "a
 * declaration survives destroy -> create").
 */
static void test_page_transition(void)
{
    static cgl_page_nav_t nav;
    static tpage_t root_page;
    static tpage_t a_page;
    static tpage_t b_page;
    static tpage_t c_page;
    cgl_rect_t area = cgl_rect(0, 0, (CGL_CFG_SCREEN_W - 1), (CGL_CFG_SCREEN_H - 1));
    int32_t linear_v;
    int32_t in_quad_v;

    (void)printf("\n== K) A page's own transition: the page that moves decides ==\n");

    cgl_clock_init(&s_clk, 16u, 100u);
    cgl_page_nav_init(&nav, "test.page_fx", area);
    tp_init(&root_page, "R", true);
    tp_init(&a_page, "A", true);
    tp_init(&b_page, "B", true);
    tp_init(&c_page, "C", true);

    /* --- default: nothing is declared -> exactly the same as without this mechanism ----*/
    CHECK(a_page.base.enter.trans == (uint8_t)CGL_TRANS_AUTO, "a new page's enter type should be \"not declared\"");
    CHECK(a_page.base.enter.ease == (uint8_t)CGL_PAGE_FX_KEEP_EASE, "a new page's enter curve should be \"not declared\"");
    CHECK(a_page.base.enter.dur_ms == (uint16_t)CGL_PAGE_FX_KEEP_MS, "a new page's enter duration should be \"not declared\"");
    CHECK(a_page.base.leave.trans == (uint8_t)CGL_TRANS_AUTO, "a new page's leave should be \"not declared\" too");

    CHECK(cgl_page_nav_reset(&nav, &root_page.base), "RESET to the root page");
    tp_settle(&nav);
    CHECK(cgl_page_nav_push(&nav, &a_page.base), "PUSH A (nobody declares anything)");
    tp_apply(&nav);
    CHECK(nav.trans.trans == (uint8_t)CGL_TRANS_GENIE_IN, "the type should fall back to the per-action default GENIE_IN, actually %u",
          (unsigned)nav.trans.trans);
    CHECK(nav.trans.anim.ease == (uint8_t)CGL_CFG_GENIE_IN_EASE, "the curve should fall back to the type table, actually %u",
          (unsigned)nav.trans.anim.ease);
    CHECK(nav.trans.anim.dur_ms == (uint32_t)CGL_CFG_GENIE_MS, "the duration should fall back to the type table, actually %u",
          (unsigned)nav.trans.anim.dur_ms);
    tp_settle(&nav);

    /* --- the page declares a type (but not the feel -> falls back to the type table) ----*/
    cgl_page_set_trans_enter(&b_page.base, CGL_TRANS_SLIDE_LEFT, CGL_PAGE_FX_KEEP_EASE, CGL_PAGE_FX_KEEP_MS);
    CHECK(cgl_page_nav_reset(&nav, &root_page.base), "RESET to the root page");
    tp_settle(&nav);
    CHECK(b_page.base.enter.trans == (uint8_t)CGL_TRANS_SLIDE_LEFT,
          "the declaration should be written into the fields (it survives destroy -> create)");
    CHECK(cgl_page_nav_push(&nav, &b_page.base), "PUSH B (the page declares SLIDE_LEFT)");
    tp_apply(&nav);
    CHECK(nav.trans.trans == (uint8_t)CGL_TRANS_SLIDE_LEFT, "the page-declared SLIDE_LEFT should be applied, actually %u",
          (unsigned)nav.trans.trans);
    CHECK(nav.trans.anim.ease == (uint8_t)CGL_CFG_TRANSITION_EASE, "no curve declared -> falls back to the type table");
    CHECK(nav.trans.anim.dur_ms == (uint32_t)CGL_CFG_TRANSITION_MS, "no duration declared -> falls back to the type table");
    tp_settle(&nav);

    /* --- the page declares the feel (curve + duration) ------- */
    cgl_page_set_trans_enter(&c_page.base, CGL_TRANS_COVER_UP, CGL_EASE_OUT_BACK, 48u);
    CHECK(cgl_page_nav_reset(&nav, &root_page.base), "RESET to the root page");
    tp_settle(&nav);
    CHECK(cgl_page_nav_push(&nav, &c_page.base), "PUSH C (the page declares COVER_UP / OUT_BACK / 48 ms)");
    tp_apply(&nav);
    CHECK(nav.trans.trans == (uint8_t)CGL_TRANS_COVER_UP, "the type should be the page-declared COVER_UP");
    CHECK(nav.trans.anim.ease == (uint8_t)CGL_EASE_OUT_BACK, "the curve should be the page-declared OUT_BACK, actually %u",
          (unsigned)nav.trans.anim.ease);
    CHECK(nav.trans.anim.dur_ms == 48u, "the duration should be the page-declared 48 ms, actually %u",
          (unsigned)nav.trans.anim.dur_ms);
    tp_settle(&nav);

    /* --- a page declaration outranks the type the caller passes explicitly ----*/
    cgl_page_set_trans_enter(&c_page.base, CGL_TRANS_FADE, CGL_PAGE_FX_KEEP_EASE, CGL_PAGE_FX_KEEP_MS);
    CHECK(cgl_page_nav_reset(&nav, &root_page.base), "RESET to the root page");
    tp_settle(&nav);
    CHECK(cgl_page_nav_request(&nav, CGL_NAV_PUSH, &c_page.base, CGL_TRANS_COVER_UP, NULL),
          "PUSH C (the caller passes COVER_UP explicitly, the page declares FADE)");
    tp_apply(&nav);
    CHECK(nav.trans.trans == (uint8_t)CGL_TRANS_FADE,
          "the page declaration should take precedence over the type the caller passes explicitly, actually %u", (unsigned)nav.trans.trans);
    tp_settle(&nav);

    /* --- after the declaration is withdrawn, the caller's explicit type takes effect again ----*/
    cgl_page_set_trans_enter(&c_page.base, CGL_TRANS_AUTO, CGL_PAGE_FX_KEEP_EASE, CGL_PAGE_FX_KEEP_MS);
    CHECK(c_page.base.enter.trans == (uint8_t)CGL_TRANS_AUTO, "AUTO should withdraw the declaration");
    CHECK(cgl_page_nav_reset(&nav, &root_page.base), "RESET to the root page");
    tp_settle(&nav);
    CHECK(cgl_page_nav_request(&nav, CGL_NAV_PUSH, &c_page.base, CGL_TRANS_COVER_DOWN, NULL),
          "PUSH C (declaration withdrawn, the caller passes COVER_DOWN explicitly)");
    tp_apply(&nav);
    CHECK(nav.trans.trans == (uint8_t)CGL_TRANS_COVER_DOWN,
          "with no page declaration the caller should win, actually %u", (unsigned)nav.trans.trans);
    tp_settle(&nav);

    /* --- POP: it reads the **leaving page's** leave (not the enter of the page revealed) ----*/
    cgl_page_set_trans_enter(&b_page.base, CGL_TRANS_FADE, CGL_PAGE_FX_KEEP_EASE, CGL_PAGE_FX_KEEP_MS);
    cgl_page_set_trans_leave(&c_page.base, CGL_TRANS_SLIDE_DOWN, CGL_EASE_IN_OUT_QUAD, 32u);
    CHECK(cgl_page_nav_reset(&nav, &root_page.base), "RESET to the root page");
    tp_settle(&nav);
    CHECK(cgl_page_nav_push(&nav, &b_page.base), "PUSH B (the page declares FADE)");
    tp_settle(&nav);
    CHECK(cgl_page_nav_push(&nav, &c_page.base), "PUSH C");
    tp_settle(&nav);
    CHECK(cgl_page_nav_back(&nav), "POP C (C declared leave = SLIDE_DOWN / IN_OUT_QUAD / 32 ms)");
    tp_apply(&nav);
    CHECK(nav.trans.trans == (uint8_t)CGL_TRANS_SLIDE_DOWN,
          "POP should read the leaving page C's leave (SLIDE_DOWN), actually %u", (unsigned)nav.trans.trans);
    CHECK(nav.trans.anim.ease == (uint8_t)CGL_EASE_IN_OUT_QUAD, "the curve should be C's declared IN_OUT_QUAD, actually %u",
          (unsigned)nav.trans.anim.ease);
    CHECK(nav.trans.anim.dur_ms == 32u, "the duration should be C's declared 32 ms, actually %u",
          (unsigned)nav.trans.anim.dur_ms);
    tp_settle(&nav);

    /* --- REPLACE: it reads the leaving page's leave too ---------- */
    CHECK(cgl_page_nav_reset(&nav, &root_page.base), "RESET to the root page");
    tp_settle(&nav);
    CHECK(cgl_page_nav_push(&nav, &c_page.base), "PUSH C");
    tp_settle(&nav);
    CHECK(cgl_page_nav_replace(&nav, &a_page.base), "REPLACE C -> A");
    tp_apply(&nav);
    CHECK(nav.trans.trans == (uint8_t)CGL_TRANS_SLIDE_DOWN,
          "REPLACE should read the leaving page C's leave too, actually %u", (unsigned)nav.trans.trans);
    tp_settle(&nav);

    /* --- declaring inside on_create is early enough -------------- */
    a_page.create_trans = (uint8_t)CGL_TRANS_SLIDE_UP;
    CHECK(cgl_page_nav_reset(&nav, &root_page.base), "RESET to the root page (A gets destroyed)");
    tp_settle(&nav);
    CHECK(cgl_page_nav_push(&nav, &a_page.base), "PUSH A (declares SLIDE_UP inside on_create)");
    tp_apply(&nav);
    CHECK(a_page.base.enter.trans == (uint8_t)CGL_TRANS_SLIDE_UP, "a declaration made inside on_create should be written into the fields");
    CHECK(nav.trans.trans == (uint8_t)CGL_TRANS_SLIDE_UP,
          "a declaration made inside on_create should take effect for **this** navigation, actually %u", (unsigned)nav.trans.trans);
    tp_settle(&nav);
    a_page.create_trans = (uint8_t)CGL_TRANS_AUTO;

    /* --- * the page-declared curve really acts on the animation (the progress at a given moment must differ) ----*/
    cgl_page_set_trans_enter(&c_page.base, CGL_TRANS_SLIDE_LEFT, CGL_EASE_LINEAR, 160u);
    CHECK(cgl_page_nav_reset(&nav, &root_page.base), "RESET to the root page");
    tp_settle(&nav);
    CHECK(cgl_page_nav_push(&nav, &c_page.base), "PUSH C (the page declares LINEAR / 160 ms)");
    tp_apply(&nav);
    tp_tick(&nav, 16u);
    linear_v = cgl_anim_value(&nav.trans.anim);
    tp_settle(&nav);

    cgl_page_set_trans_enter(&c_page.base, CGL_TRANS_SLIDE_LEFT, CGL_EASE_IN_QUAD, 160u);
    CHECK(cgl_page_nav_reset(&nav, &root_page.base), "RESET to the root page");
    tp_settle(&nav);
    CHECK(cgl_page_nav_push(&nav, &c_page.base), "PUSH C (the page declares IN_QUAD / 160 ms)");
    tp_apply(&nav);
    tp_tick(&nav, 16u);
    in_quad_v = cgl_anim_value(&nav.trans.anim);
    tp_settle(&nav);

    CHECK(linear_v > 0, "after two frames the LINEAR progress should be > 0 (actual %d) -- otherwise the inequality below is meaningless",
          (int)linear_v);
    CHECK(linear_v > in_quad_v,
          "the page-declared IN_QUAD must be slower than LINEAR early on: LINEAR=%d vs IN_QUAD=%d (is the declaration not used?)",
          (int)linear_v, (int)in_quad_v);

    /* --- out of range and clamping ----------------------------------- */
    cgl_page_set_trans_enter(&a_page.base, CGL_TRANS_COVER_UP, (cgl_ease_t)(CGL_EASE_COUNT + 3u), 100000u);
    CHECK(a_page.base.enter.ease == (uint8_t)CGL_EASE_LINEAR, "an out-of-range curve should be clamped to LINEAR, actually %u",
          (unsigned)a_page.base.enter.ease);
    CHECK(a_page.base.enter.dur_ms == 65535u, "an over-range duration should be clamped to 65535, actually %u",
          (unsigned)a_page.base.enter.dur_ms);
    CHECK(a_page.base.enter.trans == (uint8_t)CGL_TRANS_COVER_UP, "the type must not be affected by this change");

    cgl_page_set_trans_enter(&a_page.base, (cgl_trans_t)CGL_TRANS_COUNT, CGL_EASE_OUT_QUAD, 96u);
    CHECK(a_page.base.enter.trans == (uint8_t)CGL_TRANS_COVER_UP,
          "an out-of-range type should make the whole call have no effect (leaving no \"half-changed\" preference), actually %u",
          (unsigned)a_page.base.enter.trans);
    CHECK(a_page.base.enter.ease == (uint8_t)CGL_EASE_LINEAR,
          "with an out-of-range type the curve must not be changed either, actually %u", (unsigned)a_page.base.enter.ease);

    cgl_page_set_trans_enter(NULL, CGL_TRANS_FADE, CGL_EASE_LINEAR, 100u); /* it must not crash*/
    cgl_page_set_trans_leave(NULL, CGL_TRANS_FADE, CGL_EASE_LINEAR, 100u);

    (void)printf("  default = nothing declared (falls back to GENIE_IN + the type table); once a page declares, type/curve/duration all follow the page\n"
                 "  POP/REPLACE read the leaving page's leave; a declaration inside on_create takes effect this time;"
                 "after 2 frames LINEAR=%d vs IN_QUAD=%d\n",
                 (int)linear_v, (int)in_quad_v);
}

/* --------------------------------------------------------------------------
 * L) Registering pages: one context, one call, one table
 *
 * The uniform "adding a page" story (see cgl_page_ctx_t in cgl_page.h). What is
 * locked down here is what a caller relies on:
 *   - every complete row is created, in table order, once;
 *   - an incomplete row (no storage, or no attach) is **skipped**, not
 *     dereferenced -- that is what lets a build leave a page out by making its row
 *     conditional;
 *   - the context is readable **back** from the page afterwards (cgl_page_ctx),
 *     because on_create runs much later and is where a page actually needs it.
 * ------------------------------------------------------------------------ */

typedef struct
{
    cgl_page_t base;
    uint8_t attach_calls;
} reg_page_t;

static uint8_t s_reg_order;
static uint8_t s_reg_first;
static uint8_t s_reg_second;

static const cgl_page_vt_t reg__vt = {.type = "reg"};

static void reg__attach(cgl_page_t *page, const cgl_page_ctx_t *ctx)
{
    reg_page_t *p = (reg_page_t *)page;

    cgl_page_init(page, &reg__vt, "reg", (void *)ctx);
    p->attach_calls++;
    if (s_reg_order == 0u)
    {
        s_reg_first = 1u;
    }
    else if (s_reg_order == 1u)
    {
        s_reg_second = 1u;
    }
    s_reg_order++;
}

static void test_page_register(void)
{
    static reg_page_t one;
    static reg_page_t two;
    static reg_page_t three;
    static cgl_page_ctx_t ctx;
    static int app_sentinel;
    const cgl_page_desc_t table[] = {
        {"one", &one.base, reg__attach},
        {"no storage", NULL, reg__attach},
        {"no attach", &two.base, NULL},
        {"three", &three.base, reg__attach},
    };
    uint8_t created;

    (void)printf("\n== L) Registering pages: one context, one call, one table ==\n");

    ctx.screen = NULL;
    ctx.nav = NULL;
    ctx.app = &app_sentinel;

    s_reg_order = 0u;
    s_reg_first = 0u;
    s_reg_second = 0u;
    created = cgl_page_register(table, 4u, &ctx);
    CHECK(created == 2u, "register should create exactly the two complete rows, actually %u", (unsigned)created);
    CHECK(one.attach_calls == 1u, "the first complete row should be created once, actually %u", (unsigned)one.attach_calls);
    CHECK(three.attach_calls == 1u, "the last complete row should be created once, actually %u", (unsigned)three.attach_calls);
    CHECK(two.attach_calls == 0u, "a row with no attach must not be created");
    CHECK(two.base.vt == NULL, "a skipped row must be left untouched (this is how a build leaves a page out)");
    CHECK((s_reg_first != 0u) && (s_reg_second != 0u), "the rows should be created in table order");

    /* The context has to be readable back later: on_create is where a page needs
       it, and that runs on the first push, long after registration. */
    CHECK(one.base.user == (void *)&ctx, "the context should be stored in the page's user field");
    CHECK(cgl_page_ctx(&one.base) == &ctx, "cgl_page_ctx should return what was registered");
    CHECK(cgl_page_ctx(&one.base)->app == &app_sentinel, "the application pointer must survive registration");
    CHECK(one.base.name != NULL, "the page should keep the name it was created with");
    CHECK(cgl_page_ctx(NULL) == NULL, "cgl_page_ctx(NULL) must return NULL rather than crash");

    /* Robustness: a NULL table or a NULL context registers nothing (no crash). */
    CHECK(cgl_page_register(NULL, 4u, &ctx) == 0u, "a NULL table should register nothing");
    CHECK(cgl_page_register(table, 4u, NULL) == 0u, "a NULL context should register nothing");
    CHECK(cgl_page_register(table, 0u, &ctx) == 0u, "an empty table should register nothing");

    (void)printf("  2 of 4 rows created (incomplete rows skipped), the context reads back from the page\n");
}

/* ========================================================================== */

int main(void)
{
    test_clock();
    test_anim();
    test_pages();
    test_screen_pipeline();
    test_offset();
    test_font_offset();
    test_genie_mapping();
    test_s_curve();
    test_genie_nav();
    test_s_nav();
    test_s_roundtrip();
    test_transition_duration();
    test_transition_ease();
    test_page_transition();
    test_page_register();

    if (s_fails == 0)
    {
        (void)printf("\nall checks passed.\n");
        return 0;
    }
    (void)printf("\n%d checks failed.\n", s_fails);
    return 1;
}
