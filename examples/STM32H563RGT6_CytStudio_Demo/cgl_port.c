// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_port.c
  * @brief   Board port for the STM32H563RGT6 board + 1.47" ST7789.
  *
  *  THE WORKING PORT for this board, and the sibling of the reference
  *  STM32G431CB port: the framework-facing half of the two files is identical,
  *  only the clock rate and the SPI/DMA instance behind BSP/board_lcd.c differ.
  *  For the empty skeleton see ../../src/port/cgl_port_template.c, and for the
  *  tutorial see chapter 4 of the CytGL manual.
  *
  *  Three things are wired together here and nothing else:
  *
  *    CLOCK   HAL_GetTick() -- CytGL is the single owner of time; this only
  *            reports it. Never accumulate a private counter here.
  *
  *    DISPLAY lcd_bus_t (BSP/board_lcd.c, SPI1 + GPDMA1 channel 0)
  *              -> st7789_display_drv (Devices/st7789.c)
  *            The panel geometry is validated against cytgl_config.h at bind
  *            time, so a geometry mismatch is a refused boot rather than a
  *            subtly torn picture.
  *
  *    INPUT   BSP/board_key.c polling -> cgl_event_t -> cgl_screen_post_event()
  *            The scan lives in port__poll_input(), which the framework calls
  *            once per frame with that frame's dt -- so the key's debounce and
  *            long-press timers are advanced by real elapsed time and neither
  *            the application nor app.c ever touches a key. Events are posted,
  *            never dispatched directly: they are queued and drained at the
  *            frame boundary, so a page is never entered or left in the middle
  *            of a frame.
  *
  *  The CubeMX-generated MX_GPIO_Init / MX_GPDMA1_Init / MX_SPI1_Init calls in
  *  main() already configure the pins, clocks and the DMA channel, so
  *  board_init here only has to bind the driver and let it run its own power-on
  *  sequence over the already-initialised bus.
  ******************************************************************************
  */
#include "cgl_port.h"

#include "board.h"     /* HAL headers + the board's pin map */
#include "board_key.h"
#include "board_lcd.h"
#include "st7789.h"

#include <cytgl/cgl_display.h>
#include <cytgl/cgl_screen.h>
#include <cytgl/cytgl_config.h>

/* -------------------------------------------------------------------------- */
/* port state                                                                 */
/* -------------------------------------------------------------------------- */

typedef struct
{
    /* --- device layer ---------------------------------------------------- */
    st7789_t panel; /**< panel instance; the display driver's ctx          */

    /* --- framework objects ------------------------------------------------ */
    cgl_port_ctx_t ctx; /**< owns the display and the screen (see cgl_port.h) */
    cgl_page_nav_t nav; /**< the navigation stack the application registers on */
    bool bound;
} cgl_port_board_t;

static cgl_port_board_t s_board;

/* -------------------------------------------------------------------------- */
/* the clock                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * Millisecond time source.
 *
 * HAL_GetTick() is driven by SysTick and is already monotonic, so there is
 * nothing to do but hand it over. The framework subtracts successive values
 * with unsigned arithmetic, so a single wraparound is harmless -- no clamping
 * or rebasing needed.
 */
static uint32_t port__tick_ms(void)
{
    return (uint32_t)HAL_GetTick();
}

uint32_t cgl_port_board_tick_ms(void)
{
    /* Exactly the clock the framework reads (port__tick_ms), so the application's
       pacing and the framework's dt can never disagree about what time it is. */
    return port__tick_ms();
}

/* -------------------------------------------------------------------------- */
/* board bring-up                                                             */
/* -------------------------------------------------------------------------- */

/**
 * Optional pre-display hook, called by cgl_port_bind() before the driver's
 * init(). Empty on this board: CubeMX already did the pin/clock/DMA work in
 * main(). It exists to show where such work belongs on a board that needs it.
 */
static int port__board_init(void *user)
{
    (void)user;

    /* The transport is handed to the panel driver here rather than in
       MX_SPI1_Init(), because "which bus does this panel sit on" is a decision
       the port makes, not something CubeMX can express. */
    st7789_bind(&s_board.panel, board_lcd_bus(), NULL, ST7789_ROT_LANDSCAPE);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* the input source                                                           */
/* -------------------------------------------------------------------------- */

/**
 * Scan the key and post what happened, once per frame.
 *
 * Called by `cgl_port_frame()` with the dt that frame is about to use, so the
 * debounce and long-press timers inside board_key_poll() advance by real elapsed
 * time. Passing anything else (0, or a dt the caller guessed) silently freezes
 * the state machine: the key would then never reach BOARD_KEY_DEBOUNCE_MS.
 *
 * * One long press posts **two** events: the raw KEY_LONG + the logical
 *   CGL_EV_BACK. The reason is that the policy "long press = back" should
 *   **only take effect on the pages that want it**:
 *
 *      info page  on_event = NULL            -> nobody eats KEY_LONG
 *                                           -> the following CGL_EV_BACK pops the stack
 *      menu page  on_event eats KEY_LONG     -> it reads the long press as "enter
 *                                           the selected item" and also eats the
 *                                           following BACK (the bottom of the
 *                                           stack has nowhere to go)
 *
 *   If only CGL_EV_BACK were posted, the menu could not use a long press to
 *   confirm; if only KEY_LONG were posted, every page would have to write its own
 *   "go back". Posting both means a page only has to declare which one it cares
 *   about.
 */
static void port__poll_input(void *user, uint32_t dt_ms)
{
    board_key_event_t kev;
    cgl_event_t ev;

    (void)user;

    kev = board_key_poll(dt_ms);
    if (kev == BOARD_KEY_NONE)
    {
        return;
    }

    ev.key = 0u;
    ev.x = 0;
    ev.y = 0;
    ev.hold_ms = board_key_hold_ms();

    switch (kev)
    {
    case BOARD_KEY_DOWN:
        ev.kind = (uint8_t)CGL_EV_KEY_DOWN;
        break;
    case BOARD_KEY_REPEAT:
        ev.kind = (uint8_t)CGL_EV_KEY_REPEAT;
        break;
    case BOARD_KEY_UP:
        ev.kind = (uint8_t)CGL_EV_KEY_UP;
        break;
    case BOARD_KEY_LONG:
    default:
        ev.kind = (uint8_t)CGL_EV_KEY_LONG;
        break;
    }

    (void)cgl_port_post_event(&s_board.ctx, &ev);

    if (kev == BOARD_KEY_LONG)
    {
        ev.kind = (uint8_t)CGL_EV_BACK;
        (void)cgl_port_post_event(&s_board.ctx, &ev);
    }
}

/* -------------------------------------------------------------------------- */
/* published port descriptor                                                  */
/* -------------------------------------------------------------------------- */

/**
 * The whole hardware contract, in one static const object.
 *
 * `width`/`height` must equal CGL_CFG_SCREEN_W/H. st7789_display_drv reports
 * the panel's landscape geometry, so the two agree by construction -- and
 * cgl_display_init() enforces it anyway.
 */
static const cgl_port_t s_port = {
    .name = "h563rg-st7789",
    .display = &st7789_display_drv,
    .display_ctx = &s_board.panel,
    .tick_ms = port__tick_ms,
    .poll_input = port__poll_input,
    .input_ctx = NULL,
    .board_init = port__board_init,
    .board_ctx = NULL,
};

/* -------------------------------------------------------------------------- */
/* API                                                                        */
/* -------------------------------------------------------------------------- */

int cgl_port_board_init(void)
{
    cgl_port_board_t *b = &s_board;
    cgl_rect_t area;

    /* The page host covers the whole screen; it is the parent of every page
       root widget, and the reference rectangle for transition offsets and
       warps. cgl_port_bind() would create its own internal navigator if we
       passed NULL, but the application needs to register its pages against a
       known one, so we own it here. */
    area = cgl_rect(0, 0, (int16_t)(CGL_CFG_SCREEN_W - 1),
                    (int16_t)(CGL_CFG_SCREEN_H - 1));
    cgl_page_nav_init(&b->nav, "pages", area);

    /* Binding validates geometry, brings the panel up and builds the screen.
       A non-zero return is fatal: there is no useful degraded mode. */
    if (cgl_port_bind(&s_port, &b->ctx, &b->nav) != 0)
    {
        return -1;
    }

    /* NOTE: cgl_port_ctx_t owns cgl_display_t and cgl_screen_t by value (the
       framework never allocates), so there is nothing to copy out of it --
       the accessors below just hand out &b->ctx.display / &b->ctx.screen. Do
       not "helpfully" copy those structs: cgl_display_t carries the two band
       buffers and the opacity scratch (about 10 KB here). */
    b->bound = true;

    board_key_init();
    return 0;
}

uint32_t cgl_port_board_frame(void)
{
    /* One frame, nothing else: the framework computes dt from port__tick_ms(),
       pumps the key through port__poll_input(dt) and runs the whole frame
       (clock -> input -> navigation -> fixed steps -> frame callbacks -> widget
       animation -> band presentation). */
    return cgl_port_frame(&s_board.ctx);
}

bool cgl_port_board_post_event(const cgl_event_t *ev)
{
    return cgl_port_post_event(&s_board.ctx, ev);
}

cgl_screen_t *cgl_port_board_screen(void)
{
    return s_board.bound ? cgl_port_screen(&s_board.ctx) : NULL;
}

cgl_page_nav_t *cgl_port_board_nav(void)
{
    return s_board.bound ? &s_board.nav : NULL;
}

cgl_display_t *cgl_port_board_display(void)
{
    return s_board.bound ? cgl_port_display(&s_board.ctx) : NULL;
}

void cgl_port_board_set_backlight(uint8_t percent)
{
    if (s_board.bound)
    {
        cgl_display_set_backlight(cgl_port_display(&s_board.ctx), percent);
    }
}
