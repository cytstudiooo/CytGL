// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    app_ui.h
  * @brief   Application assembly layer: the four pages, on top of the board port.
  *
  *  Everything below this layer (panel, bus, display, screen, clock, navigator,
  *  key) belongs to `cgl_port.c`; everything above it is the pages. See
  *  `app_ui.c` for what that split buys.
  ******************************************************************************
  */
#ifndef APP_UI_H
#define APP_UI_H

#include <stdint.h>
#include <stdbool.h>

#include <cytgl/cgl_page.h>
#include "app_page_smash.h"

/**
 * Build the whole UI: bring the board up through the port
 * (`cgl_port_board_init()`), register the pages on the port's screen and
 * navigator, then paint the first frame. Returns 0 on success.
 */
int app_ui_build(void);

/**
 * Run one application frame. **This is the only function called per frame** --
 * the port scans the key and derives dt from its own clock, and the order of
 * time, input, page changes, physics and rendering is fixed inside
 * cgl_screen_frame() (see cgl_screen.h).
 */
void app_ui_tick(void);

/* --- Public navigation entry points (reserved for a future remote control / serial commands / more keys) --- */

/** Push one page. */
bool app_ui_push(cgl_page_t *page);
/** Go back one level. */
bool app_ui_back(void);
/** Clear the stack and return to the main menu. */
bool app_ui_home(void);

/** Current top page of the stack (may be NULL). */
cgl_page_t *app_ui_top(void);

/** The smash page object (for callers that want to call its API directly, e.g. to restart the animation). */
app_page_smash_t *app_ui_smash(void);

/** Restart the animation (equivalent to one short press on the smash page). */
void app_ui_restart(void);

#endif /* APP_UI_H */
