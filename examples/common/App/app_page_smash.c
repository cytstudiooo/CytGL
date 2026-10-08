// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    app_page_smash.c
  * @brief   Implementation of the smash animation page.
  *
  *  Layout (the vertical axis is screen y, 0..171; apart from a few widgets
  *  everything spans the full screen width 0..319)
  *
  *          x=0                                                x=319
  *      y=0 +--------------------------------------------------------+
  *          | Logo Smash                    HOLD=BACK  <- hint label |  top_bar 0..17
  *     y=18 +--------------------------------------------------------+   top_sep (1 px)
  *          | ###################################################### |  animation viewport:
  *          | ###################################################### |  y 19..152
  *          | ###################################################### |  dot matrix + two balls,
  *          | ###################################################### |  both live in this world
  *          | ###################################################### |
  *          | ###################################################### |
  *          | ###################################################### |
  *    y=153 +--------------------------------------------------------+   bottom_sep
  *          | FPS 58   DOTS 480   SMASH 1    B 12                    |  bottom_bar 154..171
  *    y=172 +--------------------------------------------------------+
  *
  *  Three places that are easy to misread (measured data, not estimates):
  *
  *   1. The two animation widgets have **exactly the same** frame: both are a
  *      full-screen-width viewport at y 19..152, with no upper/lower split.
  *   2. The bounce ball's range of motion is **the whole viewport**, so it
  *      **passes through the dot matrix** and knocks away every dot in its path.
  *      That coupling is deliberately kept; to make the ball bounce only above
  *      the dot matrix, raise the playground's floor in `smash__ball_area()` below.
  *   3. The ball is wiped from the rigid body pool by the cgl_phys2d_reset() that the
  *      logo rebuilds every round. The `cgl_bounce` module heals that by itself
  *      (src/ext/cgl_bounce.c), independently of the add_child ordering, and it
  *      recognises its own body by identity rather than by index.
  *
  *  In pseudo-3D the bounce ball has **no widget of its own**: the painter's
  *  algorithm requires every object to be sorted in one sequence, while the widget
  *  tree is drawn in insertion order, so "one widget draws the dot matrix, another
  *  draws the ball" would make the ball always cover the dot matrix (the occlusion
  *  would be wrong when it flies behind it). The logo widget alone draws every rigid
  *  body in the world in depth order, and the ball is driven only through the
  *  module's `cgl_bounce_pre_step()` / `cgl_bounce_post_step()` pair in `on_step`.
  *  (`cgl_bounce` does ship a self-drawing `cgl_bounce_widget_t` for a ball that
  *  floats on its own; this page deliberately does not use it, see
  *  `include/ext/cgl_bounce_view.h`.)
  ******************************************************************************
  */
#include "app_fx_config.h" /* APP_FX_SMASH: is this page in this build? */
#include "app_page_smash.h"
#include <ext/cgl_debris_view.h> /* the module's canvas-facing half */
#include "logo_bitmap.h"     /* the bitmap is the application's; the module takes it as a callback */
#include <cytgl/cytgl_config.h>
#include "board_key.h"

#if APP_FX_SMASH


#define LOGO_SMASH_TRANS_IN CGL_TRANS_GENIE_IN
#define LOGO_SMASH_TRANS_OUT CGL_TRANS_GENIE_OUT
#define LOGO_SMASH_TRANS_IN_MS 560u
#define LOGO_SMASH_TRANS_OUT_MS 560u

#define LOGO_SMASH_TRANS_EASE CGL_EASE_OUT_QUAD


/**
 * Font used by both bars. Two sane choices, keep BAR_H = font height + 2:
 *
 *   &cgl_font_8x16   BAR_H 18   compact (default, more room for the animation)
 *   &cgl_font_12x24  BAR_H 26   chunky, easier to read on a 250 dpi panel
 */
#define SMASH_FONT (&cgl_font_8x16)
#define BAR_H 18

#define BAR_TOP_Y 0
#define BAR_BOTTOM_Y (CGL_CFG_SCREEN_H - BAR_H) /* 154 -> 154..171 */

#define COL_BAR CGL_RGB565(24, 28, 40)
#define COL_SEP CGL_RGB565(96, 106, 132)

/** Blink period of the hint text, in milliseconds. */
#define HINT_BLINK_MS 700u

/* --------------------------------------------------------------------------
 * The gravity ball's playground (`include/ext/cgl_bounce.h`)
 *
 * The motion is the module's; **where the ball may go is this page's**, because that
 * is a scene decision and not a physics one. The module's own
 * `cgl_bounce_view_area()` would inset the frame by the ball's radius on all four
 * sides, and this scene wants two things it does not:
 *
 *   - the ball must cross the viewport **vertically from wall to wall**, so that it
 *     ploughs through the dot matrix (matrix pixels are at screen y 64..105) instead
 *     of bouncing in the strip above it;
 *   - the floor sits one pixel above that inset's floor, so the disc's lower edge
 *     runs two pixels past the frame's last row and is trimmed by `CGL_WF_CLIP`.
 *     That is what makes the hit read as landing *on* the frame's floor line instead
 *     of floating above it.
 *
 * So the area is written out here rather than derived. It is the ball's **centre**
 * range (see `cgl_bounce_rect_t`), which is why three sides differ from the frame by
 * a radius and the floor differs by one pixel.
 * ------------------------------------------------------------------------ */
#define BALL_FLOOR_INSET_PX 1

static cgl_bounce_rect_t smash__ball_area(cgl_rect_t view)
{
    cgl_bounce_rect_t a;

    a.x0 = (int16_t)(view.x0 + CGL_BOUNCE_RADIUS_PX);
    a.y0 = (int16_t)(view.y0 + CGL_BOUNCE_RADIUS_PX);
    a.x1 = (int16_t)(view.x1 - CGL_BOUNCE_RADIUS_PX);
    a.y1 = (int16_t)(view.y1 - BALL_FLOOR_INSET_PX);
    return a;
}

/* -------------------------------------------------------------------------- */
/* Page lifecycle                                                             */
/* -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
 * The shatter scene (`include/ext/cgl_debris.h`)
 *
 * The whole of this application's side of the effect: the bitmap as a callback, a
 * palette, and one widget whose draw is one call into the module's painter. The
 * module owns no storage and no widget, so nothing here is module state.
 * ------------------------------------------------------------------------ */

/** The module asks "is this cell lit?"; the answer is the generated bitmap. */
static bool smash__is_lit(int i, int j)
{
    return logo_bitmap_get(i, j) != 0;
}

static const cgl_debris_src_t s_logo_src = {LOGO_GRID_W, LOGO_GRID_H, smash__is_lit};

/* Palette: the floor one notch brighter than the background and the shadow one notch
   darker than the floor -- on a dark background a shadow can only be seen if there
   is "ground", so the floor is the precondition that makes a shadow work. */
static const cgl_debris_palette_t s_palette = {
    .floor = CGL_RGB565(24, 30, 42),
    .grid = CGL_RGB565(38, 48, 66),
    .shadow = CGL_RGB565(8, 9, 13),
    .dot = CGL_COLOR_CREAM,
    .ball = CGL_COLOR_AMBER,
    .ball_other = CGL_COLOR_RED,
};

/** The module's "repaint me" callback: it must not know what a widget is, so the
    application hands it this. */
static void smash__invalidate(void *user)
{
    app_page_smash_t *p = (app_page_smash_t *)user;

    cgl_widget_invalidate(&p->logo_view);
}

/** **The one widget that paints the whole world.** The painter's algorithm needs a
    single sequence (see include/ext/cgl_debris_view.h), so every body -- the dot matrix, the
    heavy ball and the bounce ball -- is drawn from here, in depth order. */
static void smash__logo_draw(cgl_widget_t *w, cgl_canvas_t *c)
{
    app_page_smash_t *p = (app_page_smash_t *)w->user;

    if (p != NULL)
    {
        (void)cgl_debris_view_draw(&p->debris, c, c->origin_y,
                                   (int16_t)(c->origin_y + c->height), &s_palette);
    }
}

static const cgl_widget_vt_t smash__logo_vt = {
    .type = "smash.logo",
    .draw = smash__logo_draw,
    /* update = NULL: no visual animation of its own; the world is advanced by the
       page's on_step, which is the only place in the project that steps physics. */
    .update = NULL,
    .event = NULL,
};
static void smash__create(cgl_page_t *page)
{
    app_page_smash_t *p = (app_page_smash_t *)page;
    cgl_rect_t screen = cgl_rect(0, 0, (int16_t)(CGL_CFG_SCREEN_W - 1), (int16_t)(CGL_CFG_SCREEN_H - 1));
    int16_t view_y0 = (int16_t)(BAR_TOP_Y + BAR_H + 1); /* 19 */
    int16_t view_y1 = (int16_t)(BAR_BOTTOM_Y - 2);      /* 152 */
    cgl_rect_t view = cgl_rect_wh(0, view_y0, CGL_CFG_SCREEN_W, (int16_t)(view_y1 - view_y0 + 1));

    /* root: paints the whole background and clips its children */
    cgl_panel_init(&p->root, "smash.root", screen, CGL_CFG_BG_COLOR, CGL_CFG_BG_COLOR);
    p->root.base.flags |= CGL_WF_CLIP;
    cgl_page_set_root(page, &p->root.base);

    /* The shared RAM is handed out for the first time. Announcing the owner before
       anything touches it means `app_page_smash_restart()` -- which the menu can
       reach at any moment -- can tell "the world is live" from "the bullet time
       page's particles are live" and refuse to clobber the other page's engine. */
    p->storage.mode = SMASH_STORAGE_WORLD;

    /* top bar */
    cgl_panel_init(&p->top_bar, "smash.top_bar",
                  cgl_rect_wh(0, BAR_TOP_Y, CGL_CFG_SCREEN_W, BAR_H), COL_BAR, COL_BAR);
    cgl_panel_init(&p->top_sep, "smash.top_sep",
                  cgl_rect_wh(0, BAR_TOP_Y + BAR_H, CGL_CFG_SCREEN_W, 1), COL_SEP, COL_SEP);
    cgl_label_init(&p->title, "smash.title", cgl_rect_wh(6, BAR_TOP_Y, 160, BAR_H),
                  SMASH_FONT, "Logo Smash", CGL_CFG_FG_COLOR, COL_BAR, false);
    cgl_label_init(&p->hint, "smash.hint", cgl_rect_wh(150, BAR_TOP_Y, 164, BAR_H),
                  SMASH_FONT, "TAP=REDO HOLD=BACK", COL_SEP, COL_BAR, false);
    cgl_label_set_align(&p->hint, CGL_ALIGN_RIGHT);

    /* animation viewport: logo and bounce ball share the same frame and world */
    cgl_debris_init(&p->debris, &p->storage.u.w.world, &p->storage.u.w.view,
                    &s_logo_src, cgl_debris_view_frame(view));
    p->debris.on_change = smash__invalidate;
    p->debris.on_change_user = p;
    cgl_widget_init(&p->logo_view, &smash__logo_vt, "smash.logo", view);
    p->logo_view.flags |= CGL_WF_CLIP;
    p->logo_view.user = p;

    /* The ball joins that world as a kinematic rigid body (inverse mass 0: the dot
       matrix never pushes it, it ploughs the matrix apart). It gets **no widget** --
       the logo widget above already draws every body of the world in depth order, see
       the file header. The playground is this scene's own, see smash__ball_area(). */
    (void)cgl_bounce_init(&p->ball, &p->storage.u.w.world, smash__ball_area(view));

    /* bottom bar */
    cgl_panel_init(&p->bottom_sep, "smash.bottom_sep",
                  cgl_rect_wh(0, BAR_BOTTOM_Y - 1, CGL_CFG_SCREEN_W, 1), COL_SEP, COL_SEP);
    cgl_panel_init(&p->bottom_bar, "smash.bottom_bar",
                  cgl_rect_wh(0, BAR_BOTTOM_Y, CGL_CFG_SCREEN_W, BAR_H), COL_BAR, COL_BAR);
    cgl_label_init(&p->fps, "smash.fps", cgl_rect_wh(4, BAR_BOTTOM_Y, 60, BAR_H),
                  SMASH_FONT, "FPS --", CGL_CFG_ACCENT_COLOR, COL_BAR, false);
    cgl_label_init(&p->dots, "smash.dots", cgl_rect_wh(72, BAR_BOTTOM_Y, 92, BAR_H),
                  SMASH_FONT, "DOTS ----", CGL_COLOR_CREAM, COL_BAR, false);
    cgl_label_init(&p->smash, "smash.smash", cgl_rect_wh(168, BAR_BOTTOM_Y, 74, BAR_H),
                  SMASH_FONT, "SMASH 0", CGL_COLOR_CREAM, COL_BAR, false);
    cgl_label_init(&p->bounce, "smash.bounce", cgl_rect_wh(244, BAR_BOTTOM_Y, 72, BAR_H),
                  SMASH_FONT, "B 0", CGL_COLOR_RED, COL_BAR, false);
    cgl_label_set_align(&p->bounce, CGL_ALIGN_RIGHT);

    /* assemble: draw order is insertion order */
    cgl_widget_add_child(&p->root.base, &p->top_bar.base);
    cgl_widget_add_child(&p->root.base, &p->top_sep.base);
    cgl_widget_add_child(&p->root.base, &p->logo_view);
    cgl_widget_add_child(&p->root.base, &p->bottom_sep.base);
    cgl_widget_add_child(&p->root.base, &p->bottom_bar.base);
    cgl_widget_add_child(&p->root.base, &p->title.base);
    cgl_widget_add_child(&p->root.base, &p->hint.base);
    cgl_widget_add_child(&p->root.base, &p->fps.base);
    cgl_widget_add_child(&p->root.base, &p->dots.base);
    cgl_widget_add_child(&p->root.base, &p->smash.base);
    cgl_widget_add_child(&p->root.base, &p->bounce.base);

    /* force the first statistics refresh to paint the labels */
    p->shown_fps = 0xFFFFFFFFu;
    p->shown_dots = 0xFFFFFFFFu;
    p->shown_smash = 0xFFFFFFFFu;
    p->shown_bounce = 0xFFFFFFFFu;
    p->frames = 0u;
    p->steps = 0u;
    p->hint_on = false;
    p->paused = false;
}

static void smash__enter(cgl_page_t *page, cgl_page_t *from, uint8_t kind)
{
    app_page_smash_t *p = (app_page_smash_t *)page;
    (void)from;
    (void)kind;

    /* The page is on top again, so the shared RAM has to come back to the physics
       world -- it may currently hold the bullet time page's particle engine, which
       was the previous top of the stack (see smash_storage_t). Taking it back here,
       **before** the restart, is what makes the mode flag consistent with the bytes:
       `cgl_debris_restart()` below writes a `cgl_phys2d_t` over whatever was there. */
    p->storage.mode = SMASH_STORAGE_WORLD;

    /* Every return to this page starts a fresh round, so that "the physics
       really is frozen while the page is covered" is obvious at a glance. */
    app_page_smash_restart(p);
    p->paused = false;
    cgl_widget_invalidate(&p->root.base);
}

static void smash__pause(cgl_page_t *page, cgl_page_t *by)
{
    app_page_smash_t *p = (app_page_smash_t *)page;
    (void)by;

    /* Covered = this page no longer receives on_step = the physics is frozen.
       Record the state for the info page to display (app_page_info.c); do not draw
       here -- the whole page is about to be hidden, so nobody would see it. */
    p->paused = true;
}

static void smash__resume(cgl_page_t *page, cgl_page_t *from)
{
    app_page_smash_t *p = (app_page_smash_t *)page;
    (void)from;
    p->paused = false;
}

/**
 * One fixed logic step. **The only place in the whole project that advances the
 * physics world.**
 *
 * The order must not change:
 *   1) apply forces (gravity) -- must come before integration, semi-implicit Euler
 *   2) cgl_phys2d_step() -- the one and only integration + collision
 *   3) wrap up (wall reflection, counting) -- must come after integration
 *   4) observation phase (SMASH / SETTLE / rebuild)
 *
 * Because there is exactly one call site, "several widgets each step once ->
 * the physics runs N times too fast" cannot happen.
 */
static void smash__step(cgl_page_t *page, uint32_t step_ms)
{
    app_page_smash_t *p = (app_page_smash_t *)page;

    cgl_bounce_pre_step(&p->ball);
    cgl_phys2d_step(&p->storage.u.w.world);
    cgl_bounce_post_step(&p->ball);
    cgl_debris_step(&p->debris, step_ms);

    p->steps++;
}

static void smash__frame(cgl_page_t *page, const cgl_clock_t *clk)
{
    app_page_smash_t *p = (app_page_smash_t *)page;
    uint32_t fps = (p->screen != NULL) ? cgl_screen_fps(p->screen) : 0u;
    uint32_t dots = (uint32_t)cgl_debris_active_dots(&p->debris);
    uint32_t smash = p->debris.restarts;
    uint32_t bounce = (uint32_t)cgl_bounce_bounces(&p->ball);
    bool hint_on;

    p->frames++;

    /* The numbers on the bars are only redrawn when they **really changed**:
       label's set_int compares anyway, and the extra guard here is because
       formatting the string is the most expensive part of this section. */
    if (fps != p->shown_fps)
    {
        cgl_label_set_int(&p->fps, "FPS ", (int32_t)fps, NULL);
        p->shown_fps = fps;
    }
    if (dots != p->shown_dots)
    {
        cgl_label_set_int(&p->dots, "DOTS ", (int32_t)dots, NULL);
        p->shown_dots = dots;
    }
    if (smash != p->shown_smash)
    {
        cgl_label_set_int(&p->smash, "SMASH ", (int32_t)smash, NULL);
        p->shown_smash = smash;
    }
    if (bounce != p->shown_bounce)
    {
        /* use the short prefix B, because the bottom bar is only 320 px and
           four statistics have to fit in */
        cgl_label_set_int(&p->bounce, "B ", (int32_t)bounce, NULL);
        p->shown_bounce = bounce;
    }

    /* Hint blink: **derived directly from the shared clock**, no dt accumulated
       locally. This is the practical value of the "unified clock" -- the blink
       and the physics and the FPS all use the same time, and no extra state
       variable is needed. */
    hint_on = (((clk->now_ms / HINT_BLINK_MS) & 1u) == 0u);
    if (hint_on != p->hint_on)
    {
        p->hint_on = hint_on;
        cgl_label_set_colors(&p->hint,
                            hint_on ? CGL_CFG_ACCENT_COLOR : COL_SEP, COL_BAR, false);
    }
}

static bool smash__event(cgl_page_t *page, const cgl_event_t *ev)
{
    app_page_smash_t *p = (app_page_smash_t *)page;

    switch (ev->kind)
    {
    case CGL_EV_KEY_UP:
        /* A short press = restart a round immediately. A long press does not
           reach "restart" here: app_ui translates it into CGL_EV_BACK and the
           navigator pops the stack to return. */
        if (ev->hold_ms < BOARD_KEY_LONG_MS)
        {
            app_page_smash_restart(p);
        }
        return true;

    case CGL_EV_KEY_DOWN:
    case CGL_EV_KEY_REPEAT:
        return true; /* this page eats the remaining keys so they do not leak elsewhere */

    default:
        return false;
    }
}

static const cgl_page_vt_t smash__vt = {
    .type = "page_smash",
    .on_create = smash__create,
    .on_destroy = NULL, /* the widgets are all static members of this page, nothing to free */
    .on_enter = smash__enter,
    .on_exit = NULL,
    .on_pause = smash__pause,
    .on_resume = smash__resume,
    .on_step = smash__step,
    .on_frame = smash__frame,
    .on_event = smash__event,
};

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void app_page_smash_attach(cgl_page_t *page, const cgl_page_ctx_t *ctx)
{
    app_page_smash_t *p = (app_page_smash_t *)page;

    /* The context goes into `cgl_page_t::user`, which is what makes it readable
       again from any callback through cgl_page_ctx() -- including on_create, where
       the widget tree is built. It costs no RAM: the field already existed. */
    cgl_page_init(page, &smash__vt, "smash", (void *)ctx);
    p->screen = (ctx != NULL) ? ctx->screen : NULL;
    p->paused = false;

    cgl_page_set_trans_enter(&p->base, LOGO_SMASH_TRANS_IN, LOGO_SMASH_TRANS_EASE,
                            LOGO_SMASH_TRANS_IN_MS);
    cgl_page_set_trans_leave(&p->base, LOGO_SMASH_TRANS_OUT, LOGO_SMASH_TRANS_EASE,
                            LOGO_SMASH_TRANS_OUT_MS);
}

void app_page_smash_restart(app_page_smash_t *p)
{
    /* The storage may currently hold the **bullet time page's** particle engine
       (see smash_storage_t). Rebuilding the Logo here would overwrite 500 live
       particles with a rigid body pool, and the bullet page -- which is the one on
       screen when this can happen -- would then render garbage: a cgl_phys2d_t read as
       a cgl_particles_t is 500 particles of pool bytes.
       The bullet page re-takes the storage itself on its own on_enter, so refusing
       here loses nothing. */
    if (p->storage.mode != SMASH_STORAGE_WORLD)
    {
        return;
    }
    cgl_debris_restart(&p->debris);
    cgl_bounce_restart(&p->ball);
}

uint16_t app_page_smash_active_dots(const app_page_smash_t *p)
{
    return cgl_debris_active_dots(&p->debris);
}

uint32_t app_page_smash_smashes(const app_page_smash_t *p)
{
    return p->debris.restarts;
}

uint8_t app_page_smash_style(const app_page_smash_t *p)
{
    return p->debris.style;
}

bool app_page_smash_is_paused(const app_page_smash_t *p)
{
    return p->paused;
}

uint32_t app_page_smash_steps(const app_page_smash_t *p)
{
    return p->steps;
}

uint32_t app_page_smash_frames(const app_page_smash_t *p)
{
    return p->frames;
}

#endif /* APP_FX_SMASH */
