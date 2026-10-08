// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_port.h
  * @brief   CytGL -- porting layer contract (the single seam for hardware).
  *
  *  The framework has no idea what the screen is, where time comes from, or
  *  where the buttons are. It only asks you to provide the three things below:
  *
  *      (1) A display driver (cgl_display_drv_t) -- how pixels reach the panel
  *      (2) A millisecond clock (cgl_port_tick_ms) -- where time comes from
  *      (3) An input source -- generates cgl_event_t and posts it with
  *          cgl_screen_post_event()
  *
  *  All three are handed to `cgl_port_bind()` in one go through `cgl_port_t`
  *  (the input source as the optional `poll_input` hook, which the framework
  *  calls once per frame with that frame's dt); afterwards you retrieve the
  *  framework objects with `cgl_port_display()` / `cgl_port_screen()`.
  *
  *  ---------------------------------------------------------------------------
  *  Minimal port (three steps)
  *  ---------------------------------------------------------------------------
  *
  *  1) Implement a `cgl_display_drv_t`. The driver only has to do four things:
  *
  *         int  init(void *ctx);                                    // power on
  *         void set_window(void *ctx, x0, y0, x1, y1);              // window setup (inclusive)
  *         void write_pixels(void *ctx, const cgl_color_t*, count); // async pixel push
  *         void wait_idle(void *ctx);                               // wait for bus idle
  *         void set_backlight(void *ctx, uint8_t percent);          // may be NULL
  *
  *     WARNING: `write_pixels()` MUST be ASYNCHRONOUS (DMA style): the framework
  *        gets its frame rate by overlapping "the CPU rasterises the next band"
  *        with "the bus pushes this band". A synchronous blocking implementation
  *        still runs, but at half the frame rate.
  *
  *  2) Write a function returning a monotonic millisecond time (HAL_GetTick() /
  *     SysTick / clock_gettime).
  *
  *  3) Put the three into `cgl_port_t`:
  *
  *         static const cgl_port_t s_port = {
  *             .name    = "my-board",
  *             .display = &my_display_drv,
  *             .display_ctx = &my_panel,
  *             .tick_ms = my_tick_ms,
  *             .poll_input = my_poll_input,    // optional: keys / touch
  *         };
  *         cgl_screen_t screen;
  *         cgl_port_bind(&s_port, &screen, NULL);       // register + create the screen
  *
  *  After that, every frame is just:
  *
  *         cgl_port_frame(&ctx);
  *
  *  including the input scan: `cgl_port_frame()` computes dt and then calls
  *  `poll_input(dt)` before the screen consumes its queue, so the application
  *  layer never has to see a key, a millisecond or a panel again.
  *
  *  ---------------------------------------------------------------------------
  *  Complete porting example
  *  ---------------------------------------------------------------------------
  *  See `examples/STM32G431_CytStudio_Demo/cgl_port.c` (SPI1 + DMA1 driving an
  *  ST7789), and `src/port/cgl_port_template.c` (an empty shell with TODOs:
  *  copy it, rename it, start working).
  *  Full tutorial in chapter 4 of the CytGL manual.
  ******************************************************************************
  */
#ifndef CGL_PORT_H
#define CGL_PORT_H

#include <stdint.h>
#include <stdbool.h>

/* cgl_display.h must be fully included, not merely forward declared:
   cgl_port_ctx_t below embeds a cgl_display_t by value (the framework never
   allocates, so the display's band buffers live inside this struct). */
#include <cytgl/cgl_types.h>
#include <cytgl/cgl_display.h>
#include <cytgl/cgl_screen.h>

/* The remaining types this header mentions only through pointers.
   WARNING: do NOT replace the two includes above with <cytgl/cytgl.h>.
   cytgl.h includes THIS file, so the include guard would already be set by the
   time we got here and cytgl.h would expand to nothing -- leaving
   cgl_screen_t and cgl_page_nav_t undefined. (That failure looks like
   "unknown type name 'cgl_screen_t'" at the declarations below.) */
struct cgl_page_nav;

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Porting layer descriptor                                                   */
/* -------------------------------------------------------------------------- */

/**
 * Porting layer descriptor for one board.
 *
 * This is the single contract between the framework and the hardware: give it
 * these three things and the framework can run on its own.
 */
typedef struct cgl_port
{
    const char *name; /**< Board / porting layer name (for debugging and logs) */

    /** Display driver. MUST be provided. */
    const cgl_display_drv_t *display;

    /** Context passed to the driver callbacks (usually your panel object).
        May be NULL. */
    void *display_ctx;

    /**
     * Monotonic millisecond clock. MUST be provided (unless you always compute
     * dt yourself and call cgl_screen_frame()).
     *
     * Must increase monotonically and never wrap into the past; 1 ms
     * resolution is plenty.
     * Typical implementation: `return (uint32_t)HAL_GetTick();`
     */
    uint32_t (*tick_ms)(void);

    /**
     * Input source: scan the board's keys / touch controller and POST what you
     * find as `cgl_event_t` (see `cgl_port_post_event()`). May be NULL.
     *
     * Called by `cgl_port_frame()` exactly once per frame, with the `dt_ms` that
     * frame is about to use (the fixed `CGL_CFG_CLOCK_STEP_MS` in the degenerate
     * case of a port without `tick_ms`), BEFORE the screen drains its input queue
     * -- so an event produced here is handled by this very frame. That is the
     * whole point of the hook: the port owns the clock, so the port is also the
     * only place that can debounce a key against real elapsed time.
     *
     * Two rules, both inherited from the input queue in cgl_screen.h:
     *
     * WARNING: POST, do not dispatch. `cgl_screen_post_event()` only enqueues;
     *    the screen delivers at the frame boundary. Never call a page or the
     *    navigator from here.
     * WARNING: keep it non-blocking and short -- it runs inside the frame, and
     *    the pixels are waiting.
     *
     * Not needed when the application posts its own events (touch IRQ, a
     * simulator, remote commands) -- leave it NULL and use
     * `cgl_port_post_event()` on your own schedule.
     */
    void (*poll_input)(void *user, uint32_t dt_ms);

    /** Argument passed to `poll_input`. */
    void *input_ctx;

    /**
     * Power-on initialization hook, may be NULL.
     *
     * Called inside `cgl_port_bind()`, BEFORE the display driver's `init()`.
     * The right place for board-level preparation such as pin configuration,
     * bus clocks or backlight PWM startup.
     */
    int (*board_init)(void *user);

    /** Argument passed to `board_init`. */
    void *board_ctx;
} cgl_port_t;

/* -------------------------------------------------------------------------- */
/* Framework-side porting layer instance                                      */
/* -------------------------------------------------------------------------- */

/**
 * All the state the framework side holds after a single bind.
 *
 * The caller treats it as an OPAQUE structure (the members are exposed only to
 * avoid heap allocation: the framework is entirely static, there is no malloc).
 */
typedef struct cgl_port_ctx
{
    const cgl_port_t *port; /**< Bound porting layer descriptor                    */
    cgl_display_t display;  /**< Display instance (see cytgl_config.h)             */
    cgl_screen_t screen;    /**< Screen: display + clock + navigator + input queue */
    uint32_t last_tick;     /**< Timestamp of the previous frame                   */
    bool bound;             /**< Whether the bind has succeeded                    */
} cgl_port_ctx_t;

/* -------------------------------------------------------------------------- */
/* API                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * Bind the porting layer: calls `board_init` -> binds the display driver ->
 * initializes the screen.
 *
 * @param port   Porting layer descriptor (MUST remain valid during and after the
 *               bind, usually a static const)
 * @param ctx    Framework-side instance (provided by the caller, usually a static
 *               member as well -- no heap)
 * @param nav    Navigator; pass NULL and the framework holds it STATICALLY
 *               (see cgl_screen.h)
 * @return 0 on success; non-zero = board_init / display init / geometry check failed
 *
 * WARNING: geometry check: the `width/height` reported by the driver must match
 *    `CGL_CFG_SCREEN_W/H` in `cytgl_config.h` exactly, otherwise this returns
 *    non-zero (better not to start at all than to "run with a misplaced
 *    picture").
 */
int cgl_port_bind(const cgl_port_t *port, cgl_port_ctx_t *ctx, cgl_page_nav_t *nav);

/**
 * Advance one frame -- the least effort entry point for a port.
 *
 * Internally: computes dt since the previous frame from `tick_ms()` (dt = 0 on
 * the first call), then calls `cgl_screen_frame()`.
 *
 * @return the dt used this time (milliseconds); 0 when not bound
 */
uint32_t cgl_port_frame(cgl_port_ctx_t *ctx);

/** Display instance; returns NULL when not bound. */
cgl_display_t *cgl_port_display(cgl_port_ctx_t *ctx);

/** Screen instance; returns NULL when not bound. */
cgl_screen_t *cgl_port_screen(cgl_port_ctx_t *ctx);

/** Convenience wrapper: post one input event (equivalent to
    `cgl_screen_post_event(cgl_port_screen(ctx), ev)`). */
bool cgl_port_post_event(cgl_port_ctx_t *ctx, const cgl_event_t *ev);

/** Convenience wrapper: set the backlight (0..100; a safe no-op when the driver
    does not implement it). */
void cgl_port_set_backlight(cgl_port_ctx_t *ctx, uint8_t percent);

#ifdef __cplusplus
}
#endif

#endif /* CGL_PORT_H */
