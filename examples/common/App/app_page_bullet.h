// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    app_page_bullet.h
  * @brief   Bullet time page: the Logo is smashed, time is frozen, and the camera
  *          orbits the frozen debris.
  *
  *  The effect, step by step
  *  ------------------------
  *      1. THE LOGO      a particle sits on every lit dot of the same bitmap the
  *                       smash page uses, so the two pages show the same glyph.
  *      2. THE BALL      a heavy ball flies in from the left at constant speed.
  *                       It is drawn **behind** the particles on purpose (see
  *                       "who draws what" below).
  *      3. IMPACT        when the ball's leading edge reaches the Logo's near face
  *                       the **sweep** starts: every logical step `cgl_particles_impact()`
  *                       releases only the dots the ball has reached, so the ball
  *                       carves a path and the Logo ahead of it stays exactly where it
  *                       was built. The ball keeps travelling to the right.
  *      4. FLY (1.25 s)  the debris the ball has passed sprays out around its path
  *                       while the time scale is 1.0: this is the phase that has to
  *                       look like a smash, see `BULLET_FLY_MS`.
  *      5. BULLET TIME   the time scale ramps to ~0, and the **camera** starts
  *                       orbiting the Logo about the world Y axis, one full turn in
  *                       about 13 s, with a slight dolly along the view axis.
  *      6. RELEASE       the time scale ramps back up, the debris finishes its
  *                       flight and the round is rebuilt.
  *
  *  The three things this page exists to demonstrate
  *  ------------------------------------------------
  *  1. **Time is a variable, not a constant.** `timeScale` is an integer Q8 in the
  *     state machine below, and "the world is frozen" is literally
  *     `timeScale == 0`: the particles are still in RAM, the camera is still
  *     moving, and not one simulation step runs. Nothing is paused, nothing is
  *     unwound, and no particle position is saved and restored.
  *  2. **Depth sorting with a moving camera.** `cgl_particles_sort()` re-runs the
  *     counting sort **every frame** while the orbit turns, because with a rotating
  *     camera the painter's order changes even though not one particle moves. This
  *     is the case that a "sort once per physics step" cache (the one `cgl_iso2d` uses)
  *     cannot express.
  *  3. **It costs no extra RAM.** The particle engine lives in the smash page's own
  *     world storage, see `smash_storage_t`. That is what makes the page fit at
  *     all on a part with 32 KB and 30.2 KB already spoken for.
  *
  *  Who draws what
  *  --------------
  *  Unlike the smash page -- where one widget has to draw the whole world because
  *  the painter's algorithm needs a single sequence -- here the ball is drawn
  *  **first**, by the page's own step machine, and the particles are drawn by a
  *  widget. That is not an oversight: the ball has a 5 px radius and the particles
  *  are 1..3 px dots, so interleaving them by depth would cost a second sorter for
  *  a 5 px disc whose own occlusion can never be read by eye. The particles among
  *  themselves **are** depth sorted, which is the part the effect is about.
  ******************************************************************************
  */
#ifndef APP_PAGE_BULLET_H
#define APP_PAGE_BULLET_H

#include <cytgl/cgl_types.h>
#include <cytgl/cgl_page.h>
#include <cytgl/cgl_panel.h>
#include <cytgl/cgl_label.h>
#include <cytgl/cgl_screen.h>

#include <ext/cgl_particles.h>
#include "app_ctx.h"

/**
 * Phases of the state machine. The ordering is the animation's ordering, and the
 * machine only ever moves forward (except for the wrap back to `BUILD`).
 */
typedef enum
{
    BULLET_PHASE_BUILD = 0, /**< the Logo is intact, the ball is inbound        */
    BULLET_PHASE_FLY,       /**< impact happened, debris flying, time scale 1   */
    BULLET_PHASE_FREEZE,    /**< time scale ramping down to 0                   */
    BULLET_PHASE_ORBIT,     /**< time stopped, camera orbiting                  */
    BULLET_PHASE_RELEASE,   /**< time scale ramping back up                     */
    BULLET_PHASE_SETTLE     /**< waiting before the next round                  */
} bullet_phase_t;

/**
 * Time scale, Q8: 256 = real time, 0 = stopped.
 *
 * The transition targets are compile time constants so that the ramp is a table
 * look-up and not a division: `app_bullet_time_scale()` returns one of exactly
 * three values, and "smooth" comes from the ramp counter, not from interpolating
 * the scale.
 */
#define BULLET_SCALE_ONE 256
#define BULLET_SCALE_FROZEN 0

/** Milliseconds the debris flies before the freeze: 78 logical steps of
    `CGL_CFG_CLOCK_STEP_MS`. The engine owns the step constant
    (`CGL_PARTICLES_TRIGGER_TICKS`), this is the same 1.25 s expressed in the page's own unit,
    and it is what the HUD prints.
 *
 *  WARNING: this is not a cosmetic duration. The debris velocity is an int8, i.e. at
 *     most 0.5 px per step, so `fly steps x 2^CGL_PARTICLES_DAMP_SHIFT` decides how far the
 *    cloud opens up before time stops -- the 0.5 s (31 step) value this used to be
 *    produced a 3.3 px mean spread and read as a ragged Logo instead of an
 *    explosion. See the WARNING on `CGL_PARTICLES_DAMP_SHIFT` in `src/ext/cgl_particles.c`. */
#define BULLET_FLY_MS 1248u

/** How long the time scale takes to fall to 0 (the "ramp"), ms. Short: the
    effect reads as a slam, not as a braking train. */
#define BULLET_FREEZE_RAMP_MS 220u

/** How long the time scale takes to come back, ms. Longer than the freeze ramp,
    so the release reads as "the world exhales" rather than as a snap. */
#define BULLET_RELEASE_RAMP_MS 560u

/** How long the camera orbits, ms, and the yaw advance per logical step.
 *
 * The two are a **pair and have to agree**: the step is applied once per tick and
 * the phase ends when `phase_ms` reaches this value, so the orbit lasts
 * `BULLET_ORBIT_MS / CGL_CFG_CLOCK_STEP_MS` ticks and the yaw covers
 * `ticks * (NUM / DEN)` angle units, which must come to 256 for one turn.
 *
 *     13600 ms / 16 ms = 850 ticks per orbit
 *     850 * 91/80      = 966.9 angle units = 3.78 turns
 *
 * so the camera turns **nearly four times** per phase, at 96.9 ticks per turn --
 * i.e. 2.64 deg of yaw per tick. That is the number that matters: the angle type has
 * 256 steps per turn, so a coarser rate would visibly quantise into ~256 discrete
 * stops and a finer one could not be represented at all. At the Logo's 116 px half
 * extent, 2.64 deg is under 5 px of edge motion per tick, about two dot diameters,
 * so the dots slide rather than jump.
 *
 * (The first version of this comment claimed one turn in 12800 ms; the host test
 * measures the ticks the phase actually lasts and prints the real figure, which is
 * why the pair is written out here rather than described.)
 *
 * Kept as a ratio so that changing the orbit duration only means changing this
 * pair, and so the accumulator that carries the fraction is explicit (see
 * `bullet__step`). */
#define BULLET_ORBIT_MS 2700//13600u
#define BULLET_YAW_STEP_NUM 91u
#define BULLET_YAW_STEP_DEN 80u

/** Minimum time the finished cloud is left on screen before the Logo is rebuilt.
    The round does not actually end here: it ends when the wrecking ball has **left
    the stage** as well (see `BULLET_SETTLE_MAX_MS` and `bullet__ball_gone()`), so the
    ball is never cut off in mid-air by the restart. */
#define BULLET_SETTLE_MS 700u

/** Hard bound on the SETTLE phase, so a round can never fail to end (the ball leaving
    is the normal exit; this is the belt and braces). */
#define BULLET_SETTLE_MAX_MS 4000u

/** How much of the yaw the camera unwinds per logical step on its way back to the
    front during RELEASE (`bullet__camera_home()`). 4 units per tick covers the worst
    case -- half a turn, 128 units -- inside the 35 tick release, which is what makes
    "the ball leaves the frame" a statement about a front-on view rather than about
    wherever the orbit happened to stop. */
#define BULLET_CAMERA_HOME_STEP 4u

/** Model of the wrecking ball, in engine world units (Q8.8). It is a separate
    little body from the smash page's: that one lives in a `cgl_phys2d` world which
    this page is using for the particle engine instead, so there is nowhere to put
    it -- and a ball at constant speed needs no physics engine, only the same
    1.5 px per logical step the smash page's ball uses.
 *
 *  It starts 210 px to the left of the origin, i.e. off the near side of the
 *  viewport, so the Logo is visible on its own for a moment before the impact -- and
 *  so the ball **enters** the frame rather than appearing on top of the Logo. It
 *  reaches the Logo's near face (`-CGL_PARTICLES_GRID_W * CGL_PARTICLES_CELL_PITCH_Q8 / 2` = -116 px)
 *  after about 63 logical steps, i.e. just over a second. */
#define BULLET_BALL_START_Q8 ((int32_t)(-CGL_PARTICLES_PX(210)))
#define BULLET_BALL_SPEED_Q8 (CGL_PARTICLES_PX(4) / 2) /**< 2 px per logical step */
#define BULLET_BALL_R_PX 5

/** Widget: the particle renderer. It owns no particle storage -- it is pointed at
    the page's borrowed engine by `bullet_widget_init()`.
 *
 *  The widget also carries the ball's state and draws it, rather than the page
 *  drawing it from `on_frame`: a widget may only put pixels on the screen while the
 *  band walker is inside its `draw`, so "the page draws the ball" is not actually
 *  expressible in this framework. `bullet__draw()` paints the sorted particles and
 *  then the ball, in that order -- see the file header for why that order is the
 *  right one here even though it is the wrong one on the smash page.
 */
typedef struct
{
    cgl_widget_t base;
    struct app_page_bullet *page;    /**< the owning page, **not** owned; set by
                                          `bullet_widget_init()`. Needed because the
                                          ball's position is page state and the
                                          widget is the only thing that can draw. */
    cgl_particles_t *particles; /**< borrowed from the smash page's storage */
    cgl_particles_cam_t cam;                    /**< the orbit camera, updated by the page  */
    bool frozen;                     /**< true while the time scale is 0 (bullet
                                          time palette)                          */
} bullet_widget_t;
void bullet_widget_init(bullet_widget_t *w, const char *name, cgl_rect_t frame,
                        cgl_particles_t *particles,
                        struct app_page_bullet *page);

/** The page. The whole thing is static storage (see `app_ui.c`'s `s_app`).
 *
 *  It is a **struct tag, not a typedef**, and that is deliberate: `app_page_menu.h`
 *  forward declares this page by tag (`struct app_page_bullet;`) because the menu
 *  only ever holds a **pointer** to it, and a forward typedef plus a second
 *  `typedef struct {...} app_page_bullet_t;` here would be two different types in
 *  C -- which is a "redeclared as different kind of symbol" that reads like a typo
 *  and is not one. The typedef below is the only declaration of the name. */
struct app_page_bullet
{
    cgl_page_t base; /**< <- must be the first member */

    /**
     * Cross-page links, from `ctx->app`. The **only** thing this page needs from
     * the smash page is the bytes of its storage (see smash_storage_t), which is
     * why that used to be a constructor parameter of its own; through `links` it no
     * longer needs one.
     */
    app_links_t *links;

    cgl_particles_t *particles; /**< == &host->storage.u.particles while live */
    cgl_particles_cam_t cam;

    /* --- state machine ---------------------------------------------------- */
    uint8_t phase;      /**< bullet_phase_t                                */
    uint8_t yaw_u8;     /**< camera yaw, top byte of a 16 bit angle        */
    uint8_t tick;       /**< logical steps run since the burst (see engine) */
    uint16_t phase_ms;  /**< ms accumulated in the current phase           */
    uint16_t ramp_ms;   /**< ms accumulated in the current time scale ramp  */
    uint16_t time_scale_q8; /**< 0..256, see BULLET_SCALE_*                */
    int16_t orbit_q8;   /**< dolly offset for this frame, Q8.8             */
    bool time_stopped;  /**< the HUD flag: time_scale_q8 reached 0 once    */

    /**
     * The two rational accumulators of the page, in the page and not in function
     * `static`s.
     *
     * `time_accum_q8` is the time scale integrator: one physical step adds
     * `time_scale_q8`, one logical step costs 256, so its remainder is what keeps
     * the logical step rate correct at a scale of 128 (one logical step every other
     * physical step) and what makes a scale of 0 mean "the accumulator never
     * reaches 256".
     *
     * `yaw_frac` is the fractional part of the yaw advance, for the same reason:
     * 91/80 of an angle unit per tick cannot be stored in `yaw_u8` alone.
     *
     * Both were function-local `static`s in the first draft. They are members now
     * because a function-local static is invisible state: it survives the page being
     * destroyed and re-created, it cannot be reset by `app_page_bullet_restart()`,
     * and it makes the page un-instantiable twice. Two bytes each.
     */
    uint16_t time_accum_q8;
    uint16_t yaw_frac;

    /* --- the wrecking ball (2D, drawn behind the particles) ---------------- */
    int32_t ball_x_q8;  /**< Q8.8, engine world x                          */
    bool ball_hit;      /**< the burst has already been fired              */

    /* --- widgets ---------------------------------------------------------- */
    bullet_widget_t view; /**< the particle renderer                          */

    cgl_panel_t root;
    cgl_panel_t top_bar;
    cgl_panel_t top_sep;
    cgl_panel_t bottom_bar;
    cgl_panel_t bottom_sep;

    cgl_label_t title;
    cgl_label_t hint;
    cgl_label_t fps;
    cgl_label_t pcount;
    cgl_label_t state;
    cgl_label_t yaw;

    /* cached readouts, so a label only repaints when it really changed */
    uint32_t shown_fps;
    uint32_t shown_pcount;
    uint32_t shown_state;
    uint32_t shown_yaw;

    uint32_t frames; /**< on_frame calls received (does not grow while covered) */
    uint32_t steps;  /**< on_step  calls received (does not grow while covered) */
    uint32_t rounds; /**< completed smashes                                    */
    bool hint_on;
    bool paused;

    cgl_screen_t *screen; /**< borrowed to read the clock/FPS, not owned */
};

/* The only declaration of the name `app_page_bullet_t`, here. `app_page_menu.h`
   reaches this page through the struct tag instead; do not add a second typedef. */
typedef struct app_page_bullet app_page_bullet_t;

/**
 * Create the page (registration only; the widget tree is built in `on_create`).
 * See `cgl_page_ctx_t` and the note at the top of app_ctx.h.
 */
void app_page_bullet_attach(cgl_page_t *page, const cgl_page_ctx_t *ctx);

/** Restart the round (intact Logo + inbound ball). */
void app_page_bullet_restart(struct app_page_bullet *p);

/** Current time scale, Q8 (256 = real time, 0 = frozen). The HUD readout, and the
    thing the whole page is a demonstration of. */
uint16_t app_page_bullet_time_scale(const struct app_page_bullet *p);

/** Phase (`bullet_phase_t`), for the info page and the host test. */
uint8_t app_page_bullet_phase(const struct app_page_bullet *p);

/** Camera yaw as a top byte of a 16 bit angle (0..255 = one turn). */
uint8_t app_page_bullet_yaw(const struct app_page_bullet *p);

/** Camera yaw in whole degrees, 0..359, for the HUD. */
uint16_t app_page_bullet_yaw_deg(const struct app_page_bullet *p);

/** Particles alive. */
uint16_t app_page_bullet_alive(const struct app_page_bullet *p);

/** Completed rounds. */
uint32_t app_page_bullet_rounds(const struct app_page_bullet *p);

/** Whether the page is covered (and therefore frozen). */
bool app_page_bullet_is_paused(const struct app_page_bullet *p);

/** Logical steps this page has run. */
uint32_t app_page_bullet_steps(const struct app_page_bullet *p);

/** Frame callbacks this page received. */
uint32_t app_page_bullet_frames(const struct app_page_bullet *p);

#endif /* APP_PAGE_BULLET_H */
