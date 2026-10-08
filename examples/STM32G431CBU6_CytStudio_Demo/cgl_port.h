// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_port.h
  * @brief   Board port for the WeAct STM32G431CB core board + 1.47" ST7789.
  *
  *  This is the file to read when porting CytGL to your own board. Everything
  *  board specific lives behind it; nothing above it knows which SPI instance,
  *  which DMA channel or which panel controller is in use.
  *
  *  The port implements the framework contract from <cytgl/cgl_port.h>:
  *
  *      cgl_port_t                       descriptor (display + clock + input)
  *      cgl_port_bind()                  validate, power up, build the screen
  *      cgl_port_frame()                 one frame: dt from tick_ms, key scan,
  *                                       then render
  *
  *  Layer stack of this example (see ../../src/port/cgl_port_template.c for the
  *  empty skeleton, and chapter 4 of the CytGL manual for the tutorial):
  *
  *      App/                pages, physics, scene        (knows nothing below)
  *        |
  *      cgl_port.c          THIS FILE: binds the board to the framework
  *        |                 and is its input source (key -> cgl_event_t)
  *        |
  *      Devices/st7789.c    panel driver: commands, window, init sequence
  *        |                 talks only to lcd_bus_t
  *      BSP/board_lcd.c     SPI1 + DMA1_Channel2 transport, CS/DC/RST/BLK pins
  *        |                 talks only to CubeMX handles
  *      Core/               MX_SPI1_Init / MX_DMA_Init / MX_GPIO_Init
  *
  *  Why the split matters: the panel driver is reusable across transports
  *  (hardware SPI, bit-banged GPIO, a parallel bus, or a PC simulator), and the
  *  BSP is reusable across panels. Replacing the panel touches one file.
  *
  *  WARNING about the include guard below: it must NOT be CGL_PORT_H. The
  *  framework already owns that guard for <cytgl/cgl_port.h>, and this file is
  *  found first when the demo directory is on the include path. Sharing the
  *  guard makes <cytgl/cgl_port.h> expand to nothing, so cgl_port_t and
  *  cgl_port_ctx_t vanish and the compiler reports
  *  "unknown type name 'cgl_screen_t'" a few lines further down.
  ******************************************************************************
  */
#ifndef CGL_BOARD_PORT_H
#define CGL_BOARD_PORT_H

#include <stdint.h>

#include <cytgl/cgl_types.h>
#include <cytgl/cgl_port.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bind the board to CytGL and build the screen.
 *
 * Does, in order:
 *   1. board_init  -> the bus + panel driver bring themselves up
 *   2. cgl_display_init -> binds the driver and powers the panel up
 *   3. cgl_screen_init  -> validates the geometry against cytgl_config.h,
 *                          installs the damage sink and starts the frame clock
 *   4. board_key_init() -> resets the key state machine; the actual scan runs
 *                          once per frame inside cgl_port_board_frame()
 *
 * @return 0 on success; non-zero means "do not continue" (wrong geometry, panel
 *         not responding, or the SPI/DMA instance failed to initialise).
 *
 * Call this once, after the CubeMX MX_*_Init() calls in main().
 */
int cgl_port_board_init(void);

/**
 * Advance one frame.
 *
 * Computes dt from the millisecond clock, scans the key with that dt and posts
 * what it finds, then runs the whole frame:
 * clock -> input -> navigation -> fixed-step logic -> frame callbacks ->
 * widget animation -> band presentation.
 *
 * @return the dt used, in milliseconds (0 if the port was not bound).
 */
uint32_t cgl_port_board_frame(void);

/**
 * Post an input event from outside the frame.
 *
 * The on-board key does NOT come through here: it is scanned by the port's
 * `poll_input` hook inside cgl_port_board_frame(). This entry point is for any
 * other producer (a serial command, an IR remote, a second key scanned by a
 * timer) -- it only enqueues, so calling it from an ISR is safe.
 */
bool cgl_port_board_post_event(const cgl_event_t *ev);

/** The screen built by cgl_port_board_init(); NULL before it succeeds. */
cgl_screen_t *cgl_port_board_screen(void);

/**
 * The navigation stack.
 *
 * Exposed because the application registers its page objects against it
 * (cgl_page_nav_init is called by the port, so the app must not call it again).
 */
cgl_page_nav_t *cgl_port_board_nav(void);

/** The display instance, for cgl_display_set_backlight() and friends. */
cgl_display_t *cgl_port_board_display(void);

/** Backlight convenience wrapper, 0..100. */
void cgl_port_board_set_backlight(uint8_t percent);

/**
 * Backlight level, 0..100, that the demo comes up at once its first frame is on
 * the glass (app_ui_build()).
 *
 * The panel driver deliberately leaves the backlight off through its reset and
 * init sequence so that nothing uninitialised is ever visible, which makes this
 * the value that decides how bright the demo boots. It is board policy, which is
 * why it lives here and not in App/ (App/ is shared by every board's demo,
 * examples/common/App):
 *
 *   G431 (WeAct core board)   100  -- BLK is a plain GPIO, so the panel is
 *                                    either on or off, and it looks best at
 *                                    full brightness.
 *   H563 (STM32H563RGT6)       18  -- LEDA is driven by the TIM4_CH2 PWM, so the
 *                                    demo can come up dimmed instead of at full
 *                                    blast.
 */
#define CGL_PORT_BOOT_BACKLIGHT_PERCENT 100u

/**
 * The port's millisecond clock: the same source the framework reads through
 * `cgl_port_t::tick_ms`.
 *
 * Exposed so that the application's super loop can pace itself without naming a
 * chip HAL header -- App/app.c is shared by every board's demo, so it must not
 * `#include "stm32g4xx_hal.h"` or call HAL_GetTick() itself.
 */
uint32_t cgl_port_board_tick_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* CGL_BOARD_PORT_H */
