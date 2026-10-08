// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    app_page_info.c
  * @brief   Info page: ten system readouts, drawn as a semi-transparent overlay.
  *
  *  See app_page_info.h for why the overlay is a real test of the framework and
  *  why this page deliberately handles no key at all.
  ******************************************************************************
  */
#include "app_page_info.h"

#include <cytgl/cytgl_config.h>
#include "app_fx_config.h" /* APP_FX_SMASH: is the page this one reports on in this build? */
#include "app_page_smash.h" /* the page this one reports on, through ctx->app */

/* -------------------------------------------------------------------------- */
/* Layout constants                                                           */
/* -------------------------------------------------------------------------- */

#define PAGE_FONT (&cgl_font_8x16)
#define PAGE_BAR_H 18

#define COL_BAR CGL_RGB565(24, 28, 40)
#define COL_SEP CGL_RGB565(96, 106, 132)

/** Refresh period of the readouts: derive the tick from the shared clock instead
    of redrawing ten lines of text every frame. */
#define INFO_REFRESH_MS 150u

static void info__create(cgl_page_t *page)
{
    app_page_info_t *p = (app_page_info_t *)page;
    cgl_rect_t screen = cgl_rect(0, 0, (int16_t)(CGL_CFG_SCREEN_W - 1), (int16_t)(CGL_CFG_SCREEN_H - 1));
    uint8_t i;

    cgl_panel_init(&p->root, "info.root", screen, CGL_CFG_BG_COLOR, CGL_CFG_BG_COLOR);
    p->root.base.flags |= CGL_WF_CLIP;
    cgl_page_set_root(page, &p->root.base);

    /* * This page is a **semi-transparent overlay** (which also makes it a
     *   real-world use case for group opacity):
     *
     *   1) Let the page below keep being drawn -- otherwise nav__finish() hides
     *      its whole subtree, and all that shows through this page is the host
     *      background colour, i.e. nothing at all;
     *   2) Give the root widget an alpha -- the whole page (background + all
     *      readouts) is rendered in isolation and then composited at that alpha,
     *      so the **frozen dot matrix** below shows through as if seen through
     *      frosted glass.
     *
     * Note that the page below (the smash page) is still physically frozen:
     * show_below only governs "draw or not", a buried page still receives no
     * on_step. So the per-frame cost of this effect is ~0 -- while the bottom
     * page is static it produces no damage, and only this page's own readout
     * refresh repaints those bands. */
    cgl_page_set_show_below(page, true);
    cgl_widget_set_alpha(&p->root.base, APP_INFO_OVERLAY_ALPHA);

    cgl_panel_init(&p->top_bar, "info.top_bar",
                  cgl_rect_wh(0, 0, CGL_CFG_SCREEN_W, PAGE_BAR_H), COL_BAR, COL_BAR);
    cgl_panel_init(&p->top_sep, "info.top_sep",
                  cgl_rect_wh(0, PAGE_BAR_H, CGL_CFG_SCREEN_W, 1), COL_SEP, COL_SEP);
    cgl_label_init(&p->title, "info.title", cgl_rect_wh(6, 0, 170, PAGE_BAR_H),
                  PAGE_FONT, "System Info", CGL_CFG_FG_COLOR, COL_BAR, false);
    cgl_label_init(&p->hint, "info.hint", cgl_rect_wh(190, 0, 124, PAGE_BAR_H),
                  PAGE_FONT, "HOLD=BACK", COL_SEP, COL_BAR, false);
    cgl_label_set_align(&p->hint, CGL_ALIGN_RIGHT);

    /* Two-column layout: the screen is 40 characters wide, and splitting it into
       20 + 20 fits more readouts than a single column, while an 18 px line pitch
       (16 px glyph height) does not read as cramped. */
    for (i = 0u; i < (uint8_t)APP_INFO_LINES; i++)
    {
        const bool right = ((i & 1u) != 0u);
        int16_t x = right ? 166 : 8;
        int16_t y = (int16_t)(24 + ((i >> 1) * 18));

        cgl_label_init(&p->lines[i], "info.line", cgl_rect_wh(x, y, 148, 16),
                      PAGE_FONT, "", CGL_COLOR_CREAM, CGL_CFG_BG_COLOR, false);
    }
    cgl_label_init(&p->physics, "info.physics",
                  cgl_rect_wh(8, 120, (int16_t)(CGL_CFG_SCREEN_W - 16), 16),
                  PAGE_FONT, "PHYSICS  --", CGL_COLOR_GREEN, CGL_CFG_BG_COLOR, false);

    cgl_widget_add_child(&p->root.base, &p->top_bar.base);
    cgl_widget_add_child(&p->root.base, &p->top_sep.base);
    cgl_widget_add_child(&p->root.base, &p->title.base);
    cgl_widget_add_child(&p->root.base, &p->hint.base);
    for (i = 0u; i < (uint8_t)APP_INFO_LINES; i++)
    {
        cgl_widget_add_child(&p->root.base, &p->lines[i].base);
    }
    cgl_widget_add_child(&p->root.base, &p->physics.base);

    p->shown_tick = 0xFFFFFFFFu;
}

/*
 * * The refresh tick is derived directly from the shared clock (now_ms / 150),
 *   instead of **accumulating dt ourselves**. That way "how often we refresh"
 *   and "how often physics takes a step" use the same single timeline, and no
 *   extra state variable is needed. This is the practical benefit of "the whole
 *   project has exactly one clock".
 */
static void info__frame(cgl_page_t *page, const cgl_clock_t *clk)
{
    app_page_info_t *p = (app_page_info_t *)page;
    cgl_screen_t *s = p->screen;
    const app_page_smash_t *smash = ((p->links != NULL) ? p->links->smash : NULL);
    uint32_t tick = clk->now_ms / INFO_REFRESH_MS;
#if APP_FX_SMASH
    uint32_t steps = (smash != NULL) ? app_page_smash_steps(smash) : 0u;
    bool frozen = (smash != NULL) && app_page_smash_is_paused(smash);
#else
    /* No smash page in this build: the two readouts have nothing to report. */
    uint32_t steps = 0u;
    bool frozen = false;
    (void)smash;
#endif
    uint32_t dropped;

    if ((tick == p->shown_tick) || (s == NULL))
    {
        return; /* already refreshed during this tick */
    }
    p->shown_tick = tick;

    dropped = cgl_page_nav_dropped(s->nav) + cgl_screen_dropped_events(s);

    /* Left column */
    cgl_label_set_int(&p->lines[0], "FPS       ", (int32_t)cgl_screen_fps(s), NULL);
    /* The duration is printed as fixed point scaled by x10, hence "12.3 s" and not "12 s" */
    cgl_label_set_fixed1(&p->lines[2], "UPTIME    ", (int32_t)(clk->now_ms / 100u), " s");
    cgl_label_set_int(&p->lines[4], "STEPS     ", (int32_t)clk->step_total, NULL);
    cgl_label_set_int(&p->lines[6], "NAV DEPTH ", (int32_t)cgl_page_nav_depth(s->nav), " / 4");
    /* * The number of steps the smash page actually ran. While we are parked on
       this page it **no longer grows**, whereas STEPS above (the global clock)
       keeps rising -- this is the visual proof of "a buried page -> physics
       frozen". In the old implementation the two always rose together, because
       a hidden page kept running its catch-up pass. */
    cgl_label_set_int(&p->lines[8], "SMASH STP ", (int32_t)steps, NULL);

    /* Right column */
    cgl_label_set_int(&p->lines[1], "FRAME     ", (int32_t)clk->dt_ms, " ms");
    cgl_label_set_int(&p->lines[3], "FRAMES    ", (int32_t)clk->frame, NULL);
    cgl_label_set_int(&p->lines[5], "STEP MS   ", (int32_t)clk->step_ms, NULL);
    cgl_label_set_int(&p->lines[7], "TRANS     ", (int32_t)cgl_page_nav_transitions(s->nav), NULL);
    cgl_label_set_int(&p->lines[9], "DROPPED   ", (int32_t)dropped, NULL);

    if (frozen)
    {
        cgl_label_set_text(&p->physics, "PHYSICS  FROZEN (page below)");
        cgl_label_set_colors(&p->physics, CGL_COLOR_AMBER, CGL_CFG_BG_COLOR, false);
    }
    else
    {
        cgl_label_set_text(&p->physics, "PHYSICS  RUNNING");
        cgl_label_set_colors(&p->physics, CGL_COLOR_GREEN, CGL_CFG_BG_COLOR, false);
    }
}

static const cgl_page_vt_t info__vt = {
    .type = "page_info",
    .on_create = info__create,
    .on_destroy = NULL,
    .on_enter = NULL,
    .on_exit = NULL,
    .on_pause = NULL,
    .on_resume = NULL,
    .on_step = NULL,
    .on_frame = info__frame,
    /* on_event = NULL: this page consumes no keys, so the CGL_EV_BACK after
       KEY_LONG lands in the navigator's hands -> it pops the stack
       automatically. This is what a page looks like when it does not write its
       own back logic. */
    .on_event = NULL,
};

void app_page_info_attach(cgl_page_t *page, const cgl_page_ctx_t *ctx)
{
    app_page_info_t *p = (app_page_info_t *)page;

    cgl_page_init(page, &info__vt, "info", (void *)ctx);
    p->screen = (ctx != NULL) ? ctx->screen : NULL;
    p->links = ((ctx != NULL) ? (app_links_t *)ctx->app : NULL);

    /* This page's transition is declared on the page itself: secondary
       information, so it "covers" the page below from bottom to top.
       No curve/duration declared -> falls back to nav's type table
       (COVER_UP = OUT_CUBIC / 220 ms).
       To make this page faster or add a little bounce, fill in the last two
       parameters on this line, for example
           cgl_page_set_trans_enter(&p->base, CGL_TRANS_COVER_UP, CGL_EASE_OUT_BACK, 180u);
       -- it only affects this page, other pages using COVER_UP are unaffected. */
    cgl_page_set_trans_enter(&p->base, CGL_TRANS_COVER_UP, CGL_PAGE_FX_KEEP_EASE, CGL_PAGE_FX_KEEP_MS);
}
