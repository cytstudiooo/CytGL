// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    app_page_smash.h
  * @brief   Smash animation page: Logo Smash + bounce ball + top/bottom bar stats.
  *
  *  This is the complete reference implementation of "one page", and the only
  *  place in the whole project that owns the physics world.
  *
  *  Division of labour between the page, the widgets and the clock (the core of
  *  this round of refactoring)
  *  -------------------------------------------
  *      page on_step(step_ms)   <- the only place that advances the physics:
  *                                  cgl_bounce_pre_step()
  *                                  cgl_phys2d_step(&world)          <- only here in the whole project
  *                                  cgl_bounce_post_step()
  *                                  cgl_debris_step()
  *      page on_frame(clock)    <- once per frame: refresh the bottom bar text,
  *                                  drive the hint blink
  *      widget update(dt_ms)    <- only for purely visual animation (neither of
  *                                  this page's two animation widgets needs it)
  *
  *  As a result:
  *    - "several widgets sharing a world -> the physics runs N times too fast"
  *      cannot happen (widgets have no cgl_phys2d_step to call)
  *    - while the page is covered by another page, on_step is no longer called ->
  *      the physics freezes automatically, and it **does not accumulate debt**
  *      (the clock's accumulator is global and does not catch up per page)
  *    - the whole project has exactly one time source, cgl_clock, and no second
  *      place that accumulates dt
  ******************************************************************************
  */
#ifndef APP_PAGE_SMASH_H
#define APP_PAGE_SMASH_H

#include <cytgl/cgl_types.h>
#include <cytgl/cgl_page.h>
#include <cytgl/cgl_panel.h>
#include <cytgl/cgl_label.h>
#include <cytgl/cgl_screen.h>

#include "app_ctx.h"
#include <ext/cgl_phys2d.h>
#include <ext/cgl_iso2d.h>
#include <ext/cgl_debris.h> /* the shatter scene, from the optional module */
#include <ext/cgl_bounce.h> /* the gravity ball, ditto */
#include <ext/cgl_particles.h>

/**
 * Who currently owns the shared storage, see `smash_storage_t`.
 *
 * The two values are not interchangable and not a hint: the first bytes of the
 * overlay are read as this byte by whichever subsystem is live, so a wrong value
 * means one engine reading another's memory.
 */
#define SMASH_STORAGE_WORLD 0xA1u /**< `cgl_phys2d_t` + `cgl_iso2d_view_t` are live */
#define SMASH_STORAGE_PARTICLE 0x5Eu /**< `cgl_particles_t` is live     */

/**
 * The RAM the smash page and the bullet time page share.
 *
 * **Why an overlay at all.** The demo has about 2.4 KB of .bss left (see
 * `examples/STM32G431CBU6_CytStudio_Demo/docs/application.md` sec. 1 and the `arm-none-eabi-size` line in the build
 * log). The particle engine needs 5.9 KB for 500 particles, their sort table, the
 * depth buckets and the LUT-independent state -- so a second, independent world
 * simply does not link. But the two pages can never be live at the same moment:
 * they are two entries of the same menu, and the navigator keeps exactly one
 * top-of-stack page running its `on_step`. The RAM is therefore not "shared", it
 * is **reused**, and the overlay is what makes the linker agree with the physics.
 *
 * **Why the order matters.** `cgl_phys2d_t world` comes first and `cgl_iso2d_view_t view`
 * second, and the particle engine is second. Which arm is live cannot be recovered
 * from the bytes (a `cgl_phys2d_t` whose first field happens to look like a particle
 * count is still a `cgl_phys2d_t`), so it is carried explicitly by `mode`.
 *
 * **Why `mode` is outside the union.** "Which arm is live" is exactly the invariant
 * the two `*_restart()` entry points have to respect: `app_page_smash_restart()` is
 * called from the menu and from the page's own `on_enter`, and if it ran while the
 * bullet page owned the storage it would overwrite 500 particles with a Logo.
 * Putting the flag **outside** the union (1 byte plus alignment in the parent
 * struct) is what makes that check possible; putting it inside would place it in the
 * overlapped memory, where whichever builder ran would destroy it.
 *
 * The `_Static_assert` below is the compile time half of the contract: if the
 * particle engine ever grows past the physics world, the build stops here instead of
 * the firmware corrupting its own camera at runtime.
 */
typedef struct
{
    union
    {
        struct
        {
            cgl_phys2d_t world;   /**< the shared rigid body pool (12.7 KB)     */
            cgl_iso2d_view_t view;/**< isometric camera + depth sorter (1.6 KB) */
        } w;
        cgl_particles_t particles; /**< the bullet time engine (5.9 KB) */
    } u;
    uint8_t mode; /**< SMASH_STORAGE_*, see above */
} smash_storage_t;

_Static_assert(sizeof(smash_storage_t) >= CGL_PARTICLES_STORAGE_BYTES(CGL_PARTICLES_MAX),
               "the smash page's physics world is smaller than the bullet time particle engine: "
               "raise CGL_PHYS2D_MAX_BODIES / CGL_ISO2D_MAX_POINTS or lower CGL_PARTICLES_MAX");

/* Tagged so that app_ctx.h can hold a typed pointer to it without including this
   header (which includes that one). */
typedef struct app_page_smash
{
    cgl_page_t base; /**< <- must be the first member */

    /**
     * The shared world RAM. It is `smash_storage_t` and not a `cgl_phys2d_t` so that
     * the bullet time page can borrow it -- see the type above for why, and for
     * which arm of the union is live when.
     *
     * The smash page reaches its own two members as `storage.u.w.world` and
     * `storage.u.w.view`. There is deliberately **no** short alias for them: the
     * whole point of the named union is that a name which aliases 5.9 KB of another
     * page's engine should be obvious at the call site.
     */
    smash_storage_t storage;

    cgl_debris_t debris;     /**< the shatter scene (`include/ext/cgl_debris.h`), owns no storage */
    cgl_widget_t logo_view;   /**< the one widget that paints the whole world (see the CytGL manual, section 15.6) */
    cgl_bounce_t ball;        /**< the gravity ball (`include/ext/cgl_bounce.h`); a body of the same world */

    cgl_panel_t root;
    cgl_panel_t top_bar;
    cgl_panel_t top_sep;
    cgl_panel_t bottom_bar;
    cgl_panel_t bottom_sep;

    cgl_label_t title;
    cgl_label_t hint;
    cgl_label_t fps;
    cgl_label_t dots;
    cgl_label_t smash;
    cgl_label_t bounce;

    /* cached status values, so a label only repaints when it really changed */
    uint32_t shown_fps;
    uint32_t shown_dots;
    uint32_t shown_smash;
    uint32_t shown_bounce;

    uint32_t frames; /**< number of on_frame calls this page received (does not grow while covered) */
    uint32_t steps;  /**< number of on_step  calls this page received (does not grow while covered) */
    bool hint_on;    /**< is the hint text currently lit                                          */
    bool paused;     /**< covered by another page (the physics is therefore frozen)               */

    cgl_screen_t *screen; /**< borrowed to read the clock/FPS, not owned */
} app_page_smash_t;

/**
 * Create the page (registration only; the widgets are built in `on_create`).
 *
 * The one uniform page entry point -- see `cgl_page_ctx_t` in cgl_page.h and the
 * note at the top of app_ctx.h for why every page has exactly this shape.
 */
void app_page_smash_attach(cgl_page_t *page, const cgl_page_ctx_t *ctx);

/** Rebuild the Logo immediately (equivalent to one short key press). */
void app_page_smash_restart(app_page_smash_t *p);

/** Current active dot count (the value shown as DOTS in the bottom bar). */
uint16_t app_page_smash_active_dots(const app_page_smash_t *p);

/** Cumulative number of smashed rounds. */
uint32_t app_page_smash_smashes(const app_page_smash_t *p);

/** Camera style (cgl_iso2d_style_t), for debugging. */
uint8_t app_page_smash_style(const app_page_smash_t *p);

/** Whether the page is covered. Covered = no on_step, **the physics world is completely frozen**. */
bool app_page_smash_is_paused(const app_page_smash_t *p);

/** Cumulative logic steps this page has run (the info page uses it to show "the page really did stop"). */
uint32_t app_page_smash_steps(const app_page_smash_t *p);

/** Cumulative number of frame callbacks this page received. */
uint32_t app_page_smash_frames(const app_page_smash_t *p);

#endif /* APP_PAGE_SMASH_H */
