// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    app_page_bullet.c
  * @brief   Bullet time page implementation. See app_page_bullet.h for the effect,
  *          the phase list and the three things the page demonstrates.
  *
  *  The one idea in this file
  *  -------------------------
  *  **The time scale is a counter, not a multiplier.**
  *
  *  The obvious way to slow time down is
  *
  *      pos += vel * timeScale;          // scale every velocity
  *
  *  which is wrong on two counts here: it turns every fixed point accumulation into
  *  a scaled one (so the collision-free flight becomes frame-rate dependent), and
  *  it cannot express "time is exactly stopped" without special-casing 0.
  *
  *  This page does it the way every fixed-step engine does it, only with integers:
  *
  *      timeScale_q8 = 0..256            where 0 = stopped, 256 = real time
  *      accum_q8 += timeScale_q8         once per **physical** step (56.9 Hz)
  *      while (accum_q8 >= 256) { accum_q8 -= 256; cgl_particles_step(); }
  *
  *  So one physical step runs zero or one logical simulation steps depending on the
  *  scale, the particles only ever advance by **whole** steps (nothing is
  *  interpolated, nothing is scaled), and `timeScale == 0` is not a special case --
  *  it is just an accumulator that never reaches 256. At 256 the loop runs exactly
  *  once, so real time is bit-identical to "no time scale at all", which is what
  *  makes the ramp invisible when it is not running.
  *
  *  Why 8 bit and not 16: the ramp is 220 ms = 14 physical steps, so the scale only
  *  takes ~14 distinct values on the way down. Q8 (256 steps) resolves those 14
  *  values with 18 units between them; Q16 would be 4608 units between the same 14
  *  values, i.e. 15 wasted bits of a `uint16_t` and 2 more bytes of RAM for nothing.
  ******************************************************************************
  */
#include "app_fx_config.h" /* APP_FX_PARTICLES: whether this page exists at all */
#include "app_page_bullet.h"
#include <ext/cgl_particles_view.h> /* the painter: the module's canvas-facing half */
#include "app_page_smash.h" /* the page whose storage this one borrows, via ctx->app */

#include <cytgl/cytgl_config.h>
#include "board_key.h"
#include "logo_bitmap.h"

#if APP_FX_PARTICLES

/* -------------------------------------------------------------------------- */
/* Layout (identical to the smash page: same bars, same viewport, so switching
   between the two pages does not move anything on screen)                     */
/* -------------------------------------------------------------------------- */

#define BULLET_FONT (&cgl_font_8x16)
#define BAR_H 18
#define BAR_TOP_Y 0
#define BAR_BOTTOM_Y (CGL_CFG_SCREEN_H - BAR_H)

#define COL_BAR CGL_RGB565(24, 28, 40)
#define COL_SEP CGL_RGB565(96, 106, 132)

/** Blink period of the hint text, ms. Derived from the shared clock, not
    accumulated (same idiom as the smash page). */
#define HINT_BLINK_MS 700u

#define BULLET_TRANS_IN CGL_TRANS_S_IN
#define BULLET_TRANS_OUT CGL_TRANS_S_OUT
#define BULLET_TRANS_MS 560u

/* -------------------------------------------------------------------------- */
/* Engine world helpers                                                       */
/* -------------------------------------------------------------------------- */

/* The world fit itself (`CGL_PARTICLES_CELL_PITCH_Q8`, the grid size and the `cgl_particles_cell_x()` /
   `cgl_particles_cell_y()` pair) lives in `include/ext/cgl_particles.h`, because the engine owns
   the fit and the caller only supplies the lit test. The page used to re-derive the
   cell geometry from the pitch and got it wrong by a sign the moment the engine's
   origin convention changed -- the shared helper is what prevents that. */

/** Lit test handed to the engine, so that `src/ext/cgl_particles.c` does not have to
    include the demo's generated bitmap (it stays a host-testable integer module). */
static bool bullet__is_lit(int i, int j)
{
    return logo_bitmap_get(i, j) != 0;
}

/* -------------------------------------------------------------------------- */
/* Time scale                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * Advance a ramp towards `to` by `ms` and return the new scale, Q8.
 *
 * The ramp is **linear in time, not exponential**: `scale = 256 * (1 - elapsed /
 * ramp_ms)`. Linear is right for a "time slam" because the perceptual quantity is
 * the derivative of the scale, and an exponential ease would make the first frame
 * of the freeze nearly instantaneous and the last one imperceptible -- the exact
 * opposite of "the world visibly grinds to a halt".
 *
 * All integer: `ramp_ms` is 0..65535 and the product is bounded by 256 * 65535 =
 * 1.7e7, so int32 and no libgcc helper.
 */
static uint16_t bullet__ramp(uint16_t from, uint16_t to, uint16_t elapsed, uint16_t dur)
{
    int32_t v;

    if (dur == 0u)
    {
        return to;
    }
    if (elapsed >= dur)
    {
        return to;
    }
    v = (int32_t)from + ((((int32_t)to - (int32_t)from) * (int32_t)elapsed) / (int32_t)dur);
    if (v < 0)
    {
        v = 0;
    }
    if (v > BULLET_SCALE_ONE)
    {
        v = BULLET_SCALE_ONE;
    }
    return (uint16_t)v;
}

/* -------------------------------------------------------------------------- */
/* Phase machine                                                              */
/* -------------------------------------------------------------------------- */

/** Take the shared storage back for the particle engine and start a new round.
 *
 * This is the page's `on_enter` action, and it is also the only place that writes
 * `SMASH_STORAGE_PARTICLE`: everything downstream (`cgl_particles_init`, `cgl_particles_build`,
 * `cgl_particles_burst`, `cgl_particles_step`) then works on bytes that belong to this page. */
/** The smash page whose storage this page borrows, through `ctx->app`.
 *
 *  NULL is legal and means exactly one thing: this build has no smash page, so there
 *  is no storage to borrow and the particle engine must not be bound at all (rather
 *  than bound to nothing and read as garbage). Every use goes through here, so the
 *  "which page owns the bytes" question has one answer in this file. */
static app_page_smash_t *bullet__host(const app_page_bullet_t *p)
{
    return ((p->links != NULL) ? p->links->smash : NULL);
}

void app_page_bullet_restart(app_page_bullet_t *p)
{
    app_page_smash_t *host = ((p != NULL) ? bullet__host(p) : NULL);

    if (host == NULL)
    {
        return;
    }

    /* Announce the new owner **before** the first write: see smash_storage_t. */
    host->storage.mode = SMASH_STORAGE_PARTICLE;

    p->particles = cgl_particles_init(&host->storage.u.particles,
                           (uint32_t)sizeof(host->storage.u.particles));
    if (p->particles == NULL)
    {
        return;
    }
    cgl_particles_build(p->particles, (uint16_t)LOGO_DOT_COUNT, bullet__is_lit,
             cgl_particles_cell_x, cgl_particles_cell_y);

    p->phase = (uint8_t)BULLET_PHASE_BUILD;
    p->yaw_u8 = 0u;
    p->tick = 0u;
    p->phase_ms = 0u;
    p->ramp_ms = 0u;
    p->time_scale_q8 = BULLET_SCALE_ONE;
    p->time_stopped = false;
    p->orbit_q8 = 0;
    p->ball_x_q8 = BULLET_BALL_START_Q8;
    p->ball_hit = false;
    p->rounds++;

    /* The two rational accumulators are reset, **not** carried over: a remainder
       left from the previous round would make the new round start with a
       fractionally advanced yaw (the camera would jump by up to 1.1 angle units,
       i.e. 1.6 deg, on the first frame of the orbit) and a fractionally advanced
       time scale (the first logical step could come one physical step early). */
    p->time_accum_q8 = 0u;
    p->yaw_frac = 0u;

    /* The camera and the sort are built here, not left for the first step.
       `bullet__step` is what maintains them, but a frame can be **drawn** before it
       is **stepped** -- `cgl_screen_frame(dt)` applies a pending page change before
       it steps, and the `dt = 0` frame in `app_ui_build()` never steps at all -- and
       an unsorted engine makes `cgl_particles_view_draw_band()` return immediately, so the frame
       would come out empty. */
    cgl_particles_cam_look(&p->view.cam, p->yaw_u8, p->orbit_q8, cgl_particles_view_frame(p->view.base.frame));
    (void)cgl_particles_sort(p->particles, &p->view.cam);

    cgl_widget_invalidate(&p->root.base);
}

/** One **logical** simulation step (the unit the time scale gates). */
static void bullet__logical_step(app_page_bullet_t *p)
{
    /* The ball is outside the particle engine on purpose (see the header): it needs
       a constant speed and no depth sorting, so it is 2 lines of arithmetic here
       instead of a second physics world. */
    p->ball_x_q8 += BULLET_BALL_SPEED_Q8;

    if (!p->ball_hit)
    {
        /*
         * **First contact is the ball's leading edge reaching the Logo's near face,
         * not its centre.** `CGL_PARTICLES_GRID_W * CGL_PARTICLES_CELL_PITCH_Q8 / 2` is the Logo's
         * half width, so the near face is at `x = -half` and the ball's edge touches
         * it when `ball_x + BALL_R == -half`. That is what starts the round: the
         * phase machine leaves BUILD (and the engine's step counter, which
         * `cgl_particles_tick()` exposes and the freeze is timed against, starts at 0 there
         * because the engine is not stepped before this point).
         *
         * What happens at that moment is **not** a burst of the whole sheet: from
         * here on `cgl_particles_impact()` releases the dots the ball has actually reached, one
         * contact at a time, so the ball carves a path and the Logo to the right of
         * it stays exactly as it was built. The first version released all 500
         * particles from the near face at once, which on the panel read as "the whole
         * Logo dissolves before the ball even touches it" -- the ball was still short
         * of most of the sheet when those dots flew away.
         */
        const int32_t face_x = -((CGL_PARTICLES_GRID_W * CGL_PARTICLES_CELL_PITCH_Q8) / 2);

        if ((p->ball_x_q8 + (BULLET_BALL_R_PX * CGL_PARTICLES_ONE)) >= face_x)
        {
            p->ball_hit = true;
            p->phase = (uint8_t)BULLET_PHASE_FLY;
            p->tick = 0u;
            p->phase_ms = 0u;
        }
    }

    if (p->ball_hit)
    {
        /* Release what the ball has reached, then simulate: the dots it has just
           touched move in the same step, which is what makes the contact read as a
           hit rather than as a delayed reaction. */
        (void)cgl_particles_impact(p->particles, p->ball_x_q8, BULLET_BALL_R_PX * CGL_PARTICLES_ONE);
        cgl_particles_step(p->particles);
    }
}

/* -------------------------------------------------------------------------- */
/* The ball and the stage                                                     */
/* -------------------------------------------------------------------------- */

/**
 * Walk the camera back to the front (yaw 0) the **short way round**, one call per
 * tick, so that the release ends head-on and the ball's exit is visible.
 *
 * The signed offset to 0 is `(int8_t)(0 - yaw)`: the byte difference wraps modulo 256
 * and reinterpreting it as signed gives the offset in -128..127, i.e. the short way,
 * in one cast and no branches beyond the two below.
 */
static void bullet__camera_home(app_page_bullet_t *p)
{
    const int16_t off = (int16_t)(int8_t)(0u - p->yaw_u8);

    if (off > (int16_t)BULLET_CAMERA_HOME_STEP)
    {
        p->yaw_u8 = (uint8_t)(p->yaw_u8 + BULLET_CAMERA_HOME_STEP);
    }
    else if (off < -(int16_t)BULLET_CAMERA_HOME_STEP)
    {
        p->yaw_u8 = (uint8_t)(p->yaw_u8 - BULLET_CAMERA_HOME_STEP);
    }
    else
    {
        p->yaw_u8 = 0u;
    }
}

/**
 * Has the wrecking ball left the stage? Asked of the **projection**, not of a world
 * coordinate: "x > 120 px" is not the same statement as "off the screen" once the
 * camera has moved, and the ball is off the screen that matters.
 */
static bool bullet__ball_gone(const app_page_bullet_t *p)
{
    int32_t sx;
    int32_t sy;
    int32_t depth;

    cgl_particles_project(&p->view.cam, p->ball_x_q8, 0, 0, &sx, &sy, &depth);
    return (sx / CGL_PARTICLES_ONE) > (int32_t)(CGL_CFG_SCREEN_W + BULLET_BALL_R_PX);
}


/* -------------------------------------------------------------------------- */
/* Widget callbacks                                                           */
/* -------------------------------------------------------------------------- */

static void bullet__draw(cgl_widget_t *w, cgl_canvas_t *c)
{
    bullet_widget_t *v = (bullet_widget_t *)w;
    int16_t band_y0 = c->origin_y;
    int16_t band_y1 = (int16_t)(c->origin_y + c->height); /* exclusive */

    if (v->particles == NULL)
    {
        return;
    }

    /*
     * **The depth sort happens in `bullet__step`, not here.** This is the one thing
     * in this widget that is easy to get subtly, catastrophically wrong, so the
     * reasoning is worth keeping:
     *
     * `cgl_widget_draw_tree()` is called once per **band**, and the band walker
     * walks the screen in `CGL_CFG_BAND_H`-row steps aligned to the **screen** --
     * 0..3, 4..7, 8..11, ... Its first band overlapping this viewport is therefore
     * rows **16..19**, while the viewport starts at row **19**. The obvious way to
     * say "once per frame, not once per band" is
     *
     *      if (band_y0 == w->frame.y0) { cgl_particles_sort(...); }
     *
     * and it is **wrong**: `band_y0` is always a multiple of 4 and `frame.y0` is 19,
     * so the condition is never true, the sorter never runs, `s->sorted` stays false,
     * and `cgl_particles_view_draw_band()` returns immediately for every band. The whole viewport
     * renders **nothing**, with no error anywhere -- it looks exactly like "the
     * particle engine was never initialised", which is what made it take two rounds
     * to find.
     *
     * Sorting in the step instead is also simply the right layering: the sort depends
     * on the camera, the camera is updated by the step, and the draw callback is
     * called once per band and must not do per-frame work. `cgl_particles_sort()` is
     * cheap enough to run at the step rate (3 passes over 500 particles, 62.5 times a
     * second) and it is idempotent, so a frame that draws 34 bands still sorts once.
     */

    (void)cgl_particles_view_draw_band(v->particles, &v->cam, c, band_y0, band_y1,
                                          CGL_COLOR_CREAM);

    /*
     * The wrecking ball, drawn **after** the particles so it is on top of them.
     *
     * This is the one place the page deliberately gives up an occlusion: a 5 px
     * amber disc versus 1..3 px dots, so the only wrong frame is one where a dot
     * should sit in front of the ball, and at this dot size that is not readable by
     * eye anyway. Doing it "properly" would mean sorting the ball into the same
     * sequence, which means a second pass over the whole cloud for one body.
     *
     * The ball is **not** part of the particle world window: it starts 210 px off the
     * near side of the stage and runs off the far side of it, so its `ball_x_q8` is
     * passed to the projection as it is (int32) instead of being clamped into the
     * +-128 px Q8.8 particle window. Clamping it was what pinned the ball to the edge:
     * `cgl_particles_clamp_i16()` saturates at +-128 px, i.e. barely past the screen centre at
     * this camera scale, so the ball could never cross the frame.
     */
    if (v->page != NULL)
    {
        const app_page_bullet_t *p = v->page;
        const cgl_particles_cam_t *cam = &v->cam;
        int32_t sx;
        int32_t sy;
        int32_t depth;
        int32_t px;

        cgl_particles_project(cam, p->ball_x_q8, 0, 0, &sx, &sy, &depth);
        px = sx / CGL_PARTICLES_ONE;

        if ((px > -(BULLET_BALL_R_PX + 2)) && (px < (CGL_CFG_SCREEN_W + BULLET_BALL_R_PX + 2)))
        {
            const int32_t py = sy / CGL_PARTICLES_ONE;

            if ((py > -(BULLET_BALL_R_PX + 4)) && (py < (CGL_CFG_SCREEN_H + BULLET_BALL_R_PX + 4)))
            {
                cgl_canvas_disc(c, (int16_t)px, (int16_t)py, BULLET_BALL_R_PX, CGL_COLOR_AMBER);
                cgl_canvas_dot(c, (int16_t)(px - 1), (int16_t)(py - 1), 2, CGL_COLOR_CREAM);
            }
        }
    }
}

static const cgl_widget_vt_t bullet__vt = {
    .type = "bullet_particles",
    .draw = bullet__draw,
    /* update = NULL: like the smash page's widgets, this one has no animation that
       "follows the frame". The logical stepping is the page's `on_step`, and the
       camera is advanced there too -- a widget that needs no dt should not be handed
       a dt. */
    .update = NULL,
    .event = NULL,
};

void bullet_widget_init(bullet_widget_t *w, const char *name, cgl_rect_t frame,
                        cgl_particles_t *particles,
                        struct app_page_bullet *page)
{
    cgl_widget_init(&w->base, &bullet__vt, name, frame);
    w->base.flags |= CGL_WF_CLIP;
    w->particles = particles;
    w->page = page;
    w->frozen = false;
    /* The camera is built **here, not left zeroed**: the widget draws with it from
       the first frame, and a zeroed `cgl_particles_cam_t` has `dist_q8 = 0` and
       `focal_q8 = 0`, which makes every depth negative and every projection
       degenerate. `bullet__step` refreshes it, but a frame can be drawn before the
       first step (that is exactly what the `dt = 0` frame in `app_ui_build` does). */
    cgl_particles_cam_look(&w->cam, 0u, 0, cgl_particles_view_frame(frame));
}

/* -------------------------------------------------------------------------- */
/* Page lifecycle                                                             */
/* -------------------------------------------------------------------------- */

static void bullet__create(cgl_page_t *page)
{
    app_page_bullet_t *p = (app_page_bullet_t *)page;
    cgl_rect_t screen = cgl_rect(0, 0, (int16_t)(CGL_CFG_SCREEN_W - 1),
                                (int16_t)(CGL_CFG_SCREEN_H - 1));
    int16_t view_y0 = (int16_t)(BAR_TOP_Y + BAR_H + 1); /* 19  */
    int16_t view_y1 = (int16_t)(BAR_BOTTOM_Y - 2);      /* 152 */
    cgl_rect_t view = cgl_rect_wh(0, view_y0, CGL_CFG_SCREEN_W,
                                 (int16_t)(view_y1 - view_y0 + 1));

    cgl_panel_init(&p->root, "bullet.root", screen, CGL_CFG_BG_COLOR, CGL_CFG_BG_COLOR);
    p->root.base.flags |= CGL_WF_CLIP;
    cgl_page_set_root(page, &p->root.base);

    cgl_panel_init(&p->top_bar, "bullet.top_bar",
                  cgl_rect_wh(0, BAR_TOP_Y, CGL_CFG_SCREEN_W, BAR_H), COL_BAR, COL_BAR);
    cgl_panel_init(&p->top_sep, "bullet.top_sep",
                  cgl_rect_wh(0, BAR_TOP_Y + BAR_H, CGL_CFG_SCREEN_W, 1), COL_SEP, COL_SEP);
    cgl_label_init(&p->title, "bullet.title", cgl_rect_wh(6, BAR_TOP_Y, 150, BAR_H),
                  BULLET_FONT, "Bullet Time", CGL_CFG_FG_COLOR, COL_BAR, false);
    cgl_label_init(&p->hint, "bullet.hint", cgl_rect_wh(150, BAR_TOP_Y, 164, BAR_H),
                  BULLET_FONT, "TAP=REDO HOLD=BACK", COL_SEP, COL_BAR, false);
    cgl_label_set_align(&p->hint, CGL_ALIGN_RIGHT);

    /* The particle viewport. It is created **before** the first restart, so the
       engine pointer it is handed is the one the restart binds, and it is handed
       `p` itself because the ball it draws is page state. */
    {
        /* The bytes are borrowed through ctx->app (see bullet__host). A build with no
           smash page hands the widget NULL; the widget then draws nothing rather
           than reading memory that belongs to another page. */
        app_page_smash_t *host = bullet__host(p);

        bullet_widget_init(&p->view, "bullet.view", view,
                           (host != NULL) ? &host->storage.u.particles : NULL, p);
    }

    cgl_panel_init(&p->bottom_sep, "bullet.bottom_sep",
                  cgl_rect_wh(0, BAR_BOTTOM_Y - 1, CGL_CFG_SCREEN_W, 1), COL_SEP, COL_SEP);
    cgl_panel_init(&p->bottom_bar, "bullet.bottom_bar",
                  cgl_rect_wh(0, BAR_BOTTOM_Y, CGL_CFG_SCREEN_W, BAR_H), COL_BAR, COL_BAR);
    cgl_label_init(&p->fps, "bullet.fps", cgl_rect_wh(4, BAR_BOTTOM_Y, 60, BAR_H),
                  BULLET_FONT, "FPS --", CGL_CFG_ACCENT_COLOR, COL_BAR, false);
    cgl_label_init(&p->pcount, "bullet.pcount", cgl_rect_wh(72, BAR_BOTTOM_Y, 88, BAR_H),
                  BULLET_FONT, "P ----", CGL_COLOR_CREAM, COL_BAR, false);
    cgl_label_init(&p->state, "bullet.state", cgl_rect_wh(164, BAR_BOTTOM_Y, 74, BAR_H),
                  BULLET_FONT, "BUILD", CGL_COLOR_GREEN, COL_BAR, false);
    cgl_label_init(&p->yaw, "bullet.yaw", cgl_rect_wh(244, BAR_BOTTOM_Y, 72, BAR_H),
                  BULLET_FONT, "Y 0", CGL_COLOR_CYAN, COL_BAR, false);
    cgl_label_set_align(&p->yaw, CGL_ALIGN_RIGHT);

    cgl_widget_add_child(&p->root.base, &p->top_bar.base);
    cgl_widget_add_child(&p->root.base, &p->top_sep.base);
    cgl_widget_add_child(&p->root.base, &p->view.base);
    cgl_widget_add_child(&p->root.base, &p->bottom_sep.base);
    cgl_widget_add_child(&p->root.base, &p->bottom_bar.base);
    cgl_widget_add_child(&p->root.base, &p->title.base);
    cgl_widget_add_child(&p->root.base, &p->hint.base);
    cgl_widget_add_child(&p->root.base, &p->fps.base);
    cgl_widget_add_child(&p->root.base, &p->pcount.base);
    cgl_widget_add_child(&p->root.base, &p->state.base);
    cgl_widget_add_child(&p->root.base, &p->yaw.base);

    /* Force the first statistics refresh to paint the labels. */
    p->shown_fps = 0xFFFFFFFFu;
    p->shown_pcount = 0xFFFFFFFFu;
    p->shown_state = 0xFFFFFFFFu;
    p->shown_yaw = 0xFFFFFFFFu;
    p->frames = 0u;
    p->steps = 0u;
    p->rounds = 0u;
    p->hint_on = false;
    p->paused = false;

    app_page_bullet_restart(p);
}

static void bullet__enter(cgl_page_t *page, cgl_page_t *from, uint8_t kind)
{
    app_page_bullet_t *p = (app_page_bullet_t *)page;
    (void)from;
    (void)kind;

    app_page_bullet_restart(p);
    p->paused = false;
    cgl_widget_invalidate(&p->root.base);
}

static void bullet__pause(cgl_page_t *page, cgl_page_t *by)
{
    app_page_bullet_t *p = (app_page_bullet_t *)page;
    (void)by;
    p->paused = true;
}

static void bullet__resume(cgl_page_t *page, cgl_page_t *from)
{
    app_page_bullet_t *p = (app_page_bullet_t *)page;
    (void)from;
    p->paused = false;
}

/**
 * One fixed logic step. **The only place this page advances anything.**
 *
 * Order, and why:
 *   1. the phase machine decides the time scale for this step,
 *   2. the time scale gates how many **logical** steps run (0 or 1 at these rates),
 *   3. while bullet time is active the **camera** advances regardless of the time
 *      scale -- that is the entire effect: the world stopped, the camera did not.
 */
static void bullet__step(cgl_page_t *page, uint32_t step_ms)
{
    app_page_bullet_t *p = (app_page_bullet_t *)page;
    bool orbiting = false;
    bool yaw_turn = false;

    if (p->particles == NULL)
    {
        return;
    }

    p->steps++;

    switch ((bullet_phase_t)p->phase)
    {
    case BULLET_PHASE_BUILD:
        /* The ball is inbound. Time is real; the "phase" ends when the ball reaches
           the Logo, which `bullet__logical_step()` detects. */
        p->time_scale_q8 = BULLET_SCALE_ONE;
        break;

    case BULLET_PHASE_FLY:
        p->phase_ms = (uint16_t)(p->phase_ms + step_ms);
        p->time_scale_q8 = BULLET_SCALE_ONE;
        /* The engine counts its own logical steps (`cgl_particles_tick`), and the trigger is
           the engine's published constant -- so "how long the debris flies" is
           tuned in one place (include/ext/cgl_particles.h) and not duplicated here as a
           millisecond count that could drift away from it. */
        p->tick = cgl_particles_tick(p->particles);
        if (p->tick >= CGL_PARTICLES_TRIGGER_TICKS)
        {
            p->phase = (uint8_t)BULLET_PHASE_FREEZE;
            p->ramp_ms = 0u;
        }
        break;

    case BULLET_PHASE_FREEZE:
        p->ramp_ms = (uint16_t)(p->ramp_ms + step_ms);
        p->time_scale_q8 = bullet__ramp(BULLET_SCALE_ONE, BULLET_SCALE_FROZEN,
                                        p->ramp_ms, BULLET_FREEZE_RAMP_MS);
        /* **The camera starts turning during the ramp, not after it.** Starting it
           only once the scale reaches 0 makes the freeze read as two separate
           events ("stop", then "start turning"); overlapping them is what makes it
           read as one. */
        orbiting = true;
        yaw_turn = true;
        if (p->ramp_ms >= BULLET_FREEZE_RAMP_MS)
        {
            p->phase = (uint8_t)BULLET_PHASE_ORBIT;
            p->phase_ms = 0u;
        }
        break;

    case BULLET_PHASE_ORBIT:
        p->phase_ms = (uint16_t)(p->phase_ms + step_ms);
        p->time_scale_q8 = BULLET_SCALE_FROZEN;
        orbiting = true;
        yaw_turn = true;
        if (p->phase_ms >= BULLET_ORBIT_MS)
        {
            p->phase = (uint8_t)BULLET_PHASE_RELEASE;
            p->ramp_ms = 0u;
        }
        break;

    case BULLET_PHASE_RELEASE:
        p->ramp_ms = (uint16_t)(p->ramp_ms + step_ms);
        p->time_scale_q8 = bullet__ramp(BULLET_SCALE_FROZEN, BULLET_SCALE_ONE,
                                        p->ramp_ms, BULLET_RELEASE_RAMP_MS);
        /* The camera **comes back to the front while time resumes**, instead of
           continuing to orbit (see `bullet__camera_home()`): the ball has to finish
           its run and leave the stage, and "which way the ball goes" only means
           anything head-on -- with the orbit left wherever it stopped, an edge-on
           final yaw moves the ball in *depth*, so it would never cross the frame at
           all. The dolly stays live while the yaw walks home. */
        orbiting = true;
        bullet__camera_home(p);
        if (p->ramp_ms >= BULLET_RELEASE_RAMP_MS)
        {
            p->phase = (uint8_t)BULLET_PHASE_SETTLE;
            p->phase_ms = 0u;
        }
        break;

    case BULLET_PHASE_SETTLE:
    default:
        p->phase_ms = (uint16_t)(p->phase_ms + step_ms);
        p->time_scale_q8 = BULLET_SCALE_ONE;
        /* The round ends when the wrecking ball has **left the stage** (and the cloud
           has had at least `BULLET_SETTLE_MS` to rest): restarting on a fixed timer
           while the ball was still crossing the frame is what made it disappear in
           mid-air. `BULLET_SETTLE_MAX_MS` is the belt-and-braces bound so a round can
           never fail to end. */
        if ((p->phase_ms >= BULLET_SETTLE_MS) &&
            (bullet__ball_gone(p) || (p->phase_ms >= BULLET_SETTLE_MAX_MS)))
        {
            app_page_bullet_restart(p);
            return;
        }
        break;
    }

    if (p->time_scale_q8 == 0u)
    {
        p->time_stopped = true;
    }

    /* --- 2. the time scale gates the simulation ---------------------------
       `time_accum_q8` is in the same 0..256 units as the scale: one physical step
       adds the scale, one logical step costs 256. At scale 256 exactly one logical
       step runs per physical step -- bit identical to no time scale at all, which
       is what makes the ramp invisible while it is not running. At scale 0 the
       accumulator never reaches 256 and **not one simulation step runs**, which is
       what "time is stopped" means here. The remainder is carried, so a scale of
       128 gives exactly one logical step every other physical step rather than
       rounding to "never". */
    p->time_accum_q8 = (uint16_t)(p->time_accum_q8 + p->time_scale_q8);
    while (p->time_accum_q8 >= BULLET_SCALE_ONE)
    {
        p->time_accum_q8 = (uint16_t)(p->time_accum_q8 - BULLET_SCALE_ONE);
        bullet__logical_step(p);
    }

    /* --- 3. the camera is not gated --------------------------------------- */
    if (yaw_turn)
    {
        /*
         * Yaw advances by BULLET_YAW_STEP_NUM / BULLET_YAW_STEP_DEN of an angle unit
         * per tick, carried in an explicit fraction accumulator instead of a
         * pre-multiplied constant so that the ratio is visible and retunable: at
         * 91/80 the yaw advances 1.1375 units per tick, i.e. 256 / 1.1375 = 225
         * ticks per turn.
         */
        uint16_t whole;

        p->yaw_frac = (uint16_t)(p->yaw_frac + BULLET_YAW_STEP_NUM);
        whole = (uint16_t)(p->yaw_frac / BULLET_YAW_STEP_DEN);
        p->yaw_frac = (uint16_t)(p->yaw_frac % BULLET_YAW_STEP_DEN);
        p->yaw_u8 = (uint8_t)(p->yaw_u8 + whole);
    }

    if (orbiting)
    {
        /*
         * The dolly: a cosine of the yaw, so the camera breathes in and out **twice
         * per orbit**, at the two moments it is edge-on to the Logo and the scene
         * would otherwise look smallest. `cgl_particles_cos_q8` is the same table the rotation
         * uses, so the dolly costs one look-up and holds no state of its own --
         * because it is a function of the yaw, restarting the orbit needs no reset
         * of it.
         */
        p->orbit_q8 = (int16_t)((CGL_PARTICLES_CAM_DOLLY_Q8 * cgl_particles_cos_q8(p->yaw_u8)) / CGL_PARTICLES_ONE);
    }
    else
    {
        p->orbit_q8 = 0;
    }

    /* The widget reads the camera from here, so it is refreshed every step even
       while the orbit is off (the ball needs it, and a page re-entered mid-round
       must not paint with a stale matrix). */
    cgl_particles_cam_look(&p->view.cam, p->yaw_u8, p->orbit_q8, cgl_particles_view_frame(p->view.base.frame));
    p->view.frozen = (p->time_scale_q8 == 0u);

    /* **The depth sort runs here**, once per logical step, right after the camera
       that it depends on. See the long note in `bullet__draw()` for why it is not
       in the draw callback: doing it per band is 34x the work, and the obvious
       "first band of the frame" test does not fire at all because the band walker
       aligns its bands to the screen while the viewport starts at a non-multiple. */
    (void)cgl_particles_sort(p->particles, &p->view.cam);

    cgl_widget_invalidate(&p->view.base);
}

static void bullet__frame(cgl_page_t *page, const cgl_clock_t *clk)
{
    app_page_bullet_t *p = (app_page_bullet_t *)page;
    uint32_t fps = (p->screen != NULL) ? cgl_screen_fps(p->screen) : 0u;
    uint32_t alive = (uint32_t)app_page_bullet_alive(p);
    uint32_t phase = (uint32_t)p->phase;
    uint32_t yaw_deg = (uint32_t)app_page_bullet_yaw_deg(p);
    bool hint_on;

    p->frames++;

    if (fps != p->shown_fps)
    {
        cgl_label_set_int(&p->fps, "FPS ", (int32_t)fps, NULL);
        p->shown_fps = fps;
    }
    if (alive != p->shown_pcount)
    {
        cgl_label_set_int(&p->pcount, "P ", (int32_t)alive, NULL);
        p->shown_pcount = alive;
    }
    if (phase != p->shown_state)
    {
        /* The four labels are 8 characters wide at most ("RELEASE"), because the
           bottom bar is only 320 px and four readouts have to fit. */
        static const char *const names[] = { "BUILD", "FLY", "FREEZE", "ORBIT", "RELEASE", "SETTLE" };
        const cgl_color_t col = (p->time_scale_q8 == 0u) ? CGL_COLOR_AMBER
                                                         : ((phase == 0u) ? CGL_COLOR_CREAM
                                                                          : CGL_COLOR_GREEN);

        cgl_label_set_text(&p->state, names[phase % 6u]);
        cgl_label_set_colors(&p->state, col, COL_BAR, false);
        p->shown_state = phase;
    }
    if (yaw_deg != p->shown_yaw)
    {
        cgl_label_set_int(&p->yaw, "Y ", (int32_t)yaw_deg, NULL);
        p->shown_yaw = yaw_deg;
    }

    hint_on = (((clk->now_ms / HINT_BLINK_MS) & 1u) == 0u);
    if (hint_on != p->hint_on)
    {
        p->hint_on = hint_on;
        cgl_label_set_colors(&p->hint, hint_on ? CGL_CFG_ACCENT_COLOR : COL_SEP, COL_BAR, false);
    }
}

static bool bullet__event(cgl_page_t *page, const cgl_event_t *ev)
{
    app_page_bullet_t *p = (app_page_bullet_t *)page;

    switch (ev->kind)
    {
    case CGL_EV_KEY_UP:
        /* Short press = restart the round. A long press does not reach here:
           app_ui translates it into CGL_EV_BACK and the navigator pops the stack. */
        if (ev->hold_ms < BOARD_KEY_LONG_MS)
        {
            app_page_bullet_restart(p);
        }
        return true;

    case CGL_EV_KEY_DOWN:
    case CGL_EV_KEY_REPEAT:
        return true; /* eat the remaining keys so they do not leak elsewhere */

    default:
        return false;
    }
}

static const cgl_page_vt_t bullet__page_vt = {
    .type = "page_bullet",
    .on_create = bullet__create,
    .on_destroy = NULL, /* every widget is a static member; nothing to free */
    .on_enter = bullet__enter,
    .on_exit = NULL,
    .on_pause = bullet__pause,
    .on_resume = bullet__resume,
    .on_step = bullet__step,
    .on_frame = bullet__frame,
    .on_event = bullet__event,
};

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void app_page_bullet_attach(cgl_page_t *page, const cgl_page_ctx_t *ctx)
{
    app_page_bullet_t *p = (app_page_bullet_t *)page;

    p->particles = NULL;
    p->screen = (ctx != NULL) ? ctx->screen : NULL;
    p->links = ((ctx != NULL) ? (app_links_t *)ctx->app : NULL);
    p->paused = false;

    cgl_page_init(page, &bullet__page_vt, "bullet", (void *)ctx);
    cgl_page_set_trans_enter(&p->base, BULLET_TRANS_IN, CGL_EASE_OUT_QUAD, BULLET_TRANS_MS);
    cgl_page_set_trans_leave(&p->base, BULLET_TRANS_OUT, CGL_EASE_OUT_QUAD, BULLET_TRANS_MS);
}

uint16_t app_page_bullet_time_scale(const app_page_bullet_t *p)
{
    return (p != NULL) ? p->time_scale_q8 : 0u;
}

uint8_t app_page_bullet_phase(const app_page_bullet_t *p)
{
    return (p != NULL) ? p->phase : 0u;
}

uint8_t app_page_bullet_yaw(const app_page_bullet_t *p)
{
    return (p != NULL) ? p->yaw_u8 : 0u;
}

uint16_t app_page_bullet_yaw_deg(const app_page_bullet_t *p)
{
    /* yaw_u8 is 1/256 of a turn, and a turn is 360 deg: deg = yaw * 360 / 256 =
       yaw * 45 / 32. Integer, and exact enough that the HUD steps by 2 deg. */
    return (uint16_t)(((uint32_t)app_page_bullet_yaw(p) * 45u) / 32u);
}

uint16_t app_page_bullet_alive(const app_page_bullet_t *p)
{
    return (p != NULL) ? cgl_particles_active(p->particles) : 0u;
}

uint32_t app_page_bullet_rounds(const app_page_bullet_t *p)
{
    return (p != NULL) ? p->rounds : 0u;
}

bool app_page_bullet_is_paused(const app_page_bullet_t *p)
{
    return (p != NULL) && p->paused;
}

uint32_t app_page_bullet_steps(const app_page_bullet_t *p)
{
    return (p != NULL) ? p->steps : 0u;
}

uint32_t app_page_bullet_frames(const app_page_bullet_t *p)
{
    return (p != NULL) ? p->frames : 0u;
}

#endif /* APP_FX_PARTICLES */
