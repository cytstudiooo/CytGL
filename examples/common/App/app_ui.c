// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    app_ui.c
  * @brief   Application assembly layer: registers the pages on the board port
  *          and advances one frame.
  *
  *  This layer owns **no hardware and no time**. The board port in the project
  *  root (`cgl_port.c` / `cgl_port.h`) has already brought the panel up, bound
  *  the display driver, validated the geometry, created the screen with its
  *  navigator and armed the key scan before `app_ui_build()` gets to work; from
  *  then on this file does exactly two things:
  *
  *    1. Create the page objects from **one table** with **one call**
  *       (`cgl_page_register`), handing all of them the same `cgl_page_ctx_t` --
  *       the screen the port built (`cgl_port_board_screen()`), the navigator
  *       (`cgl_port_board_nav()`) and the application's cross-page links. Then put
  *       the main menu at the bottom of the port's navigation stack.
  *    2. Forward one frame to `cgl_port_board_frame()`.
  *
  *  It **no longer** shapes each page's constructor by hand. It used to call four
  *  differently shaped `*_init()` functions and then a fifth
  *  (`app_page_menu_attach_bullet`) for the page one of them had no parameter for;
  *  see the note at the top of app_ctx.h for what that cost a reader.
  *
  *  It **no longer** does any of these -- and they are why the file shrank:
  *
  *    - no longer creates `cgl_display_t` / `cgl_screen_t` / `cgl_page_nav_t`
  *      itself. That is `cgl_port_bind()`'s job (see the port's `board_init`), and
  *      doing it here is what gave the project two displays, two screens and two
  *      copies of the geometry check while only one of them was ever used.
  *    - no longer includes `board_lcd.h` / `st7789.h` / `board_key.h`: an
  *      application layer that names an SPI instance, a pin or a panel rotation
  *      cannot be moved to another board, and it has no business knowing any of
  *      them.
  *    - no longer translates keys into `cgl_event_t`. The port does that in its
  *      `poll_input` hook, which the framework calls with the frame's dt -- and
  *      dt is exactly what a key debounce needs, so the policy now lives where
  *      the time base is (see the note in `cgl_port.h`).
  *    - no longer computes or forwards `dt` either: the port owns the clock
  *      (`cgl_port_t::tick_ms`) and `cgl_port_board_frame()` derives dt from it.
  ******************************************************************************
  */
#include "app_ui.h"

#include "cgl_port.h"

#include <cytgl/cgl_screen.h>

#include "app_fx_config.h"
#include "app_page_smash.h"
#include "app_page_bullet.h"
#include "app_page_menu.h"
#include "app_page_info.h"
#include "app_page_clock.h"

typedef struct
{
    /* The four pages. The smash page holds the shared world RAM (a 12.7 KB cgl_phys2d
       world, and -- on the same bytes -- the bullet time page's 6.3 KB particle
       engine; see smash_storage_t). Everything is in static storage.

       The display, the screen and the navigator are deliberately NOT members:
       they live inside the port's `cgl_port_ctx_t` (cgl_port.c), which is static
       too. Copying them out would duplicate the band buffers (~5 KB) and hand the
       framework a second damage sink -- the framework has exactly one screen. */
#if APP_FX_SMASH
    app_page_smash_t smash;
#endif
#if APP_FX_PARTICLES
    app_page_bullet_t bullet;
#endif
    app_page_menu_t menu;
    app_page_info_t info;
    app_page_clock_t clock; /**< no ext module and no borrowed RAM: always present */
} app_ui_t;

static app_ui_t s_app;

/* -------------------------------------------------------------------------- */
/* The page table -- the one place that knows the set of pages                */
/* -------------------------------------------------------------------------- */

/**
 * The application's page links and registration context.
 *
 * Both are **static on purpose**: a page keeps a pointer to the context (in
 * `cgl_page_t::user`, so that `cgl_page_ctx()` works from any callback, including
 * `on_create`), so a stack temporary would dangle the moment this function returned.
 */
static app_links_t s_links;
static cgl_page_ctx_t s_ctx;

/**
 * **Adding a page is three edits, and this is the third one.**
 *
 *   1. one object member in `app_ui_t` (below) for its static storage;
 *   2. one `app_page_xxx_attach(cgl_page_t *, const cgl_page_ctx_t *)` function in
 *      its own file (`app_ctx.h` shows a complete one);
 *   3. one row here.
 *
 * Nothing else in the application changes -- in particular no constructor grows
 * another parameter, and the menu does not need to be told about a new page by a
 * separate call: it reads `ctx->app` when an item is activated
 * (`menu__target()`), so a page that a build leaves out simply becomes an item that
 * shows but does nothing.
 *
 * Order is free (a page's attach only stores what it needs and reads links at run
 * time); it is written home-page-first because that is the order a reader scans it
 * in.
 */
static const cgl_page_desc_t s_pages[] = {
    { "menu", (cgl_page_t *)&s_app.menu, app_page_menu_attach },
#if APP_FX_SMASH
    { "smash", (cgl_page_t *)&s_app.smash, app_page_smash_attach },
#endif
#if APP_FX_PARTICLES
    { "bullet", (cgl_page_t *)&s_app.bullet, app_page_bullet_attach },
#endif
    { "clock", (cgl_page_t *)&s_app.clock, app_page_clock_attach },
    { "info", (cgl_page_t *)&s_app.info, app_page_info_attach },
};

#define APP_PAGE_COUNT ((uint8_t)(sizeof(s_pages) / sizeof(s_pages[0])))

/* -------------------------------------------------------------------------- */
/* Assembly                                                                   */
/* -------------------------------------------------------------------------- */

int app_ui_build(void)
{
    app_ui_t *a = &s_app;
    cgl_screen_t *screen;
    cgl_page_nav_t *nav;

    /* (1) The board: panel + bus up, driver bound, geometry validated against
           cytgl_config.h (a mismatch returns non-zero right here, which is the
           check this file used to spell out by hand), damage sink installed,
           frame clock armed, key state machine reset. It is the only place in the
           project that knows the panel is an ST7789 on SPI1. */
    if (cgl_port_board_init() != 0)
    {
        return -1;
    }

    screen = cgl_port_board_screen();
    nav = cgl_port_board_nav();
    if ((screen == NULL) || (nav == NULL))
    {
        return -1;
    }

    /* (2) Fill the links, then create every page with **one** call of **one** shape.
           Each widget tree is still only built in its own on_create (that is when it
           is really pushed onto the stack); this step only binds the pages to the
           screen, the navigator and to each other.

           The links are what replaced the per-page parameter lists: the bullet page
           borrows the smash page's RAM (`smash_storage_t`), the info page reports the
           smash page's frozen/running state, and the menu pushes all three -- all of
           them by reading this one object at run time. */
#if APP_FX_SMASH
    s_links.smash = &a->smash;
#else
    s_links.smash = NULL; /* this build has no smash page (app_fx_config.h) */
#endif
#if APP_FX_PARTICLES
    s_links.bullet = &a->bullet;
#else
    s_links.bullet = NULL; /* this build has no bullet time page (app_fx_config.h) */
#endif
    s_links.info = &a->info;
    s_links.menu = &a->menu;
    s_links.clock = &a->clock;

    s_ctx.screen = screen;
    s_ctx.nav = nav;
    s_ctx.app = &s_links;

    (void)cgl_page_register(s_pages, APP_PAGE_COUNT, &s_ctx);

    /* (3) The main menu goes to the bottom of the stack. RESET has no transition,
           so the first frame is already a stable menu screen. */
    (void)cgl_page_nav_reset(nav, &a->menu.base);

    /* (4) One frame to commit the RESET above and paint the whole screen. Its dt
           is 0 (or 1 ms, if a millisecond boundary falls between the bind and
           this call): that runs no physics step and does not bias the FPS
           statistics. */
    (void)cgl_port_board_frame();

    /* (5) Only now does the panel light up. The driver comes up dark (see
           st7789__init()) so the reset/init sequence is never visible, and the
           level is board policy -- see CGL_PORT_BOOT_BACKLIGHT_PERCENT in
           cgl_port.h for why the two boards pick different values. */
    cgl_port_board_set_backlight(CGL_PORT_BOOT_BACKLIGHT_PERCENT);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Per frame                                                                  */
/* -------------------------------------------------------------------------- */

void app_ui_tick(void)
{
    /* Everything in one frame: the port scans the key with this frame's dt and
       posts what it finds, then cgl_screen_frame() runs

           clock -> input -> page change -> fixed-step physics -> frame
           callbacks -> widget animation -> present

       This layer never sees the dt: the port reads its own clock. */
    (void)cgl_port_board_frame();
}

/* -------------------------------------------------------------------------- */
/* Public navigation entry points                                             */
/* -------------------------------------------------------------------------- */

bool app_ui_push(cgl_page_t *page)
{
    cgl_page_nav_t *nav = cgl_port_board_nav();

    return (nav != NULL) && cgl_page_nav_push(nav, page);
}

bool app_ui_back(void)
{
    cgl_page_nav_t *nav = cgl_port_board_nav();

    return (nav != NULL) && cgl_page_nav_back(nav);
}

bool app_ui_home(void)
{
    cgl_page_nav_t *nav = cgl_port_board_nav();

    return (nav != NULL) && cgl_page_nav_reset(nav, &s_app.menu.base);
}

cgl_page_t *app_ui_top(void)
{
    cgl_page_nav_t *nav = cgl_port_board_nav();

    return (nav == NULL) ? NULL : cgl_page_nav_top(nav);
}

app_page_smash_t *app_ui_smash(void)
{
#if APP_FX_SMASH
    return &s_app.smash;
#else
    return NULL; /* no smash page in this build (app_fx_config.h) */
#endif
}

void app_ui_restart(void)
{
#if APP_FX_SMASH
    app_page_smash_restart(&s_app.smash);
#endif
}
