// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_port_template.c
  * @brief   CytGL -- porting layer template (empty shell, with TODOs).
  *
  *  ===========================================================================
  *  How to use this file
  *  ===========================================================================
  *
  *  1) COPY it into your project (do not modify this file):
  *
  *         cp src/port/cgl_port_template.c  my_project/my_port.c
  *
  *     WARNING: the board-level driver does NOT belong to the framework. The
  *        src/port/ directory in the framework repo holds only this template;
  *        the real SPI/DMA/pin code belongs in your product project, or in a
  *        sample project such as
  *        `examples/STM32G431_CytStudio_Demo/cgl_port.c`.
  *
  *  2) Replace `myport_` globally with your own prefix, then fill in the
  *     implementations following the TODOs.
  *
  *  3) In `main()`:
  *
  *         cgl_port_ctx_t g_port;
  *         if (cgl_port_bind(&myport_port, &g_port, NULL) != 0) { Error_Handler(); }
  *         for (;;) { cgl_port_frame(&g_port); }
  *
  *  ===========================================================================
  *  The four things you must provide
  *  ===========================================================================
  *
  *      (1) Panel init        panel_init()         -- power-on sequence + register setup
  *      (2) Window setup      panel_set_window()   -- the CASET/RASET/RAMWR trio
  *      (3) Async pixel push  panel_write_pixels() -- DMA send, MUST NOT block
  *      (4) Wait for idle     panel_wait_idle()    -- wait for the DMA started in (3)
  *
  *  (4) is the key to performance: the framework overlaps "the CPU rasterises
  *  the next band" with "the bus pushes this band". If your (3) blocks
  *  synchronously, (4) is an empty function: it works, but at half the frame
  *  rate.
  *
  *  On top of the display driver, `cgl_port_t` takes the optional input hook
  *  `myport__poll_input` (keys / touch, at the bottom of this file). The
  *  framework calls it once per frame with that frame's dt, so the port -- not
  *  the application -- owns debouncing and the key-to-event policy.
  *
  *  ===========================================================================
  *  About the write_pixels() buffer
  *  ===========================================================================
  *  The pointer the framework passes in points into `cgl_display_t.band[]` (one
  *  half of the double buffer). It must stay valid until `wait_idle()` returns
  *  -- so "start the DMA in (3) and return at once" is the correct and required
  *  pattern: the framework guarantees that (4) is called before the next buffer
  *  is pushed.
  *
  *  See chapter 4 of the CytGL manual for the full tutorial.
  ******************************************************************************
  */

/* --------------------------------------------------------------------------
 * Turn these includes on only when you actually do the port
 * ------------------------------------------------------------------------ */
/* #include "my_board.h" */ /* Your own pin / peripheral handles */
#include <cytgl/cytgl.h>

/* --------------------------------------------------------------------------
 * So that this template compiles standalone, every hardware-related part is
 * disabled with conditional compilation.
 * For a real port, just remove the CGL_PORT_TEMPLATE_STUB define.
 * ------------------------------------------------------------------------ */
#ifndef CGL_PORT_TEMPLATE_STUB
#define CGL_PORT_TEMPLATE_STUB 1
#endif

/* --------------------------------------------------------------------------
 * Panel geometry
 *
 * WARNING: the width/height filled in here MUST match CGL_CFG_SCREEN_W / _H in
 *    `cytgl_config.h`, otherwise `cgl_port_bind()` returns non-zero (better not
 *    to start at all than to show a misplaced picture).
 *    Landscape geometry: width > height.
 * ------------------------------------------------------------------------ */
#define MYPORT_PANEL_W 320
#define MYPORT_PANEL_H 172

/* ========================================================================== */
/* (1) Panel initialization                                                   */
/* ========================================================================== */

static int myport__panel_init(void *ctx)
{
    (void)ctx;

    /* TODO: power on the panel / release reset / wait 120 ms / send the
     * initialization command sequence.
     *
     * Typical: SWRESET -> delay -> SLPOUT -> delay -> MADCTL(orientation) ->
     * COLMOD(RGB565) -> a pile of vendor voltage/gamma parameters -> INVON
     * (mandatory for IPS) -> DISPON -> backlight on
     *
     * If your panel needs anything beyond "set window + write RAM" (for example
     * the SPI frame length has to switch between 8/16 bit), handle it in
     * set_window / write_pixels. */
    return 0; /* 0 = success, non-zero = failed to start */
}

/* ========================================================================== */
/* (2) Window setup -- coordinates are inclusive, panel coordinate system     */
/* ========================================================================== */

static void myport__panel_set_window(void *ctx, uint16_t x0, uint16_t y0,
                                    uint16_t x1, uint16_t y1)
{
    (void)ctx;
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;

    /* TODO: send three commands:
     *   CASET  0x2A + x0>>8, x0, x1>>8, x1
     *   RASET  0x2B + y0>>8, y0, y1>>8, y1
     *   RAMWR  0x2C
     *
     * WARNING: x1 / y1 are INCLUSIVE (both ends of the rectangle count), which
     * matches the semantics of ST7789/CASET. */
}

/* ========================================================================== */
/* (3) Asynchronous pixel push                                                */
/* ========================================================================== */

static void myport__panel_write_pixels(void *ctx, const cgl_color_t *pixels,
                                      uint32_t count)
{
    (void)ctx;
    (void)pixels;
    (void)count;

    /* TODO: kick off one ASYNCHRONOUS transfer (HAL_SPI_Transmit_DMA / linked
     * DMA descriptors ...).
     *
     * WARNING: three points you must get right:
     *
     *   a) Drive DC HIGH (data mode) before sending; only command/parameter
     *      bytes use DC low.
     *   b) Keep CS asserted until the whole block has been transferred -- since
     *      this function returns immediately, CS must NOT be deasserted here.
     *      The cleanup goes into wait_idle().
     *   c) One DMA request moves ONE RGB565 pixel: use a 16 bit frame with
     *      HalfWord alignment, and do not send byte-wise (a byte stream makes
     *      the DMA arbitrate the bus once per byte, roughly halving the pixel
     *      rate). That is why `count` is a PIXEL count, not a byte count.
     *
     * If your transfer is synchronous (poll until done, then return), that is
     * acceptable, but then (4) becomes an empty function and the frame rate is
     * halved -- the whole point of band rendering is the overlap. */
}

/* ========================================================================== */
/* (4) Wait for the bus to go idle                                            */
/* ========================================================================== */

static void myport__panel_wait_idle(void *ctx)
{
    (void)ctx;

    /* TODO: block until the transfer started in (3) has completed (for example
     *       while (hspi.State != HAL_SPI_STATE_READY) {})
     *       then release CS HERE. */
}

/* ========================================================================== */
/* Backlight (optional)                                                       */
/* ========================================================================== */

static void myport__panel_set_backlight(void *ctx, uint8_t percent)
{
    (void)ctx;
    (void)percent;

    /* TODO: backlight GPIO or PWM duty cycle. With no backlight control, leave
     *       this function empty -- the framework safely skips it when it finds
     *       it NULL in the driver. */
}

/* ========================================================================== */
/* Display driver vtable                                                      */
/* ========================================================================== */

static const cgl_display_drv_t myport_display_drv = {
    .name = "myport",
    .init = myport__panel_init,
    .set_window = myport__panel_set_window,
    .write_pixels = myport__panel_write_pixels,
    .wait_idle = myport__panel_wait_idle,
    .set_backlight = myport__panel_set_backlight,
    .width = MYPORT_PANEL_W, /* Must == CGL_CFG_SCREEN_W */
    .height = MYPORT_PANEL_H, /* Must == CGL_CFG_SCREEN_H */
};

/* ========================================================================== */
/* Millisecond clock                                                          */
/* ========================================================================== */

static uint32_t myport__tick_ms(void)
{
    /* TODO: return a monotonically increasing millisecond time.
     *
     *   STM32 HAL : return (uint32_t)HAL_GetTick();
     *   SysTick   : return (uint32_t)(uwTick);
     *   Linux/PC  : clock_gettime(CLOCK_MONOTONIC) -> ms
     *
     * WARNING: do not accumulate a counter of your own here; use the system
     * tick. The framework uses it to compute the per-frame dt. */
    return 0u;
}

/* ========================================================================== */
/* Input source (optional)                                                    */
/* ========================================================================== */

static void myport__poll_input(void *user, uint32_t dt_ms)
{
    (void)user;
    (void)dt_ms;

    /* TODO: scan your keys / touch controller, translate what you find into
     * cgl_event_t and POST it -- never call a page or the navigator directly:
     *
     *     cgl_event_t ev = { .kind = CGL_EV_KEY_DOWN, .hold_ms = my_hold_ms() };
     *     (void)cgl_port_post_event(&g_port, &ev);
     *
     * WARNING: this runs once per frame with that frame's dt, which is the only
     *    time base your debounce / long-press state machine should use -- do not
     *    read a clock yourself (see the CytGL manual, section 4.5). The demo's
     *    port shows the one-key case, including the "long press = KEY_LONG plus
     *    BACK" policy: examples/STM32G431_CytStudio_Demo/cgl_port.c.
     *
     * No input at all? Delete this function and the two descriptor fields
     * below; the framework then simply never pumps anything. */
}

/* ========================================================================== */
/* Board preparation (optional)                                               */
/* ========================================================================== */

static int myport__board_init(void *user)
{
    (void)user;

    /* TODO: pin muxing, SPI/DMA clocks, backlight PWM startup ...
     *
     * Note: a CubeMX-generated project has usually already done this in
     * MX_xxx_Init(), in which case just return 0 here (that is what the sample
     * project does, see examples/STM32G431_CytStudio_Demo/cgl_port.c). */
    return 0;
}

/* ========================================================================== */
/* Publish the porting layer                                                  */
/* ========================================================================== */

const cgl_port_t myport_port = {
    .name = "myport",
    .display = &myport_display_drv,
    .display_ctx = 0, /* TODO: &my_panel or similar */
    .tick_ms = myport__tick_ms,
    .poll_input = myport__poll_input, /* optional; NULL = no input source */
    .input_ctx = 0,
    .board_init = myport__board_init,
    .board_ctx = 0,
};

/* ========================================================================== */
/* Compiler idle guard: unused static warns without CGL_PORT_TEMPLATE_STUB.   */
/* ========================================================================== */
#if CGL_PORT_TEMPLATE_STUB
/** Template placeholder: keeps -Wunused-function / -Wmissing-prototypes from
 *  blowing up. Delete this block when you do the real port. */
const cgl_port_t *cgl_port_template_placeholder(void);

const cgl_port_t *cgl_port_template_placeholder(void)
{
    return &myport_port;
}
#endif
