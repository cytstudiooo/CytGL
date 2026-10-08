// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    app_page_info.h
  * @brief   Info page: ten system readouts, drawn as a semi-transparent overlay.
  *
  *  This page is the framework's real-world use case for **group opacity** and
  *  for `cgl_page_set_show_below()`:
  *
  *    - the page below (the smash page) keeps being drawn, so its frozen dot
  *      matrix shows through;
  *    - this whole page is rendered in isolation and composited at
  *      APP_INFO_OVERLAY_ALPHA, as if seen through frosted glass.
  *
  *  It is also the page that **writes no back logic**: `on_event` is NULL, so the
  *  CGL_EV_BACK that `port__poll_input()` posts after a long press (see cgl_port.c)
  *  lands in the navigator's hands and pops the stack. A page only handles
  *  navigation itself when it has a key of its own to interpret (see
  *  app_page_menu.c).
  *
  *  The readouts are the visible evidence of the framework's two central claims:
  *  while this page is on top, `STEPS` (the global clock) keeps rising whereas
  *  `SMASH STP` (the steps the buried smash page received) does not move at all
  *  -- "a covered page is frozen, and it does not accumulate debt".
  ******************************************************************************
  */
#ifndef APP_PAGE_INFO_H
#define APP_PAGE_INFO_H

#include <cytgl/cgl_types.h>
#include <cytgl/cgl_page.h>
#include <cytgl/cgl_panel.h>
#include <cytgl/cgl_label.h>
#include <cytgl/cgl_screen.h>

#include "app_ctx.h"

/** Number of readout lines on the info page (two-column layout, see app_page_info.c). */
#define APP_INFO_LINES 10

/**
 * Opacity of the info page when it is used as a **semi-transparent overlay**
 * (0..255, 255 = fully opaque).
 *
 * When the info page is stacked on top, the page below it (smash page / main
 * menu) keeps drawing, and this whole page is composited on top at this alpha
 * -- so "a buried page is physically frozen" is not just written in the
 * readouts, it is **visible with the naked eye**: the dot matrix below stops
 * right there (see `cgl_page_set_show_below()` and `cgl_widget_set_alpha()`).
 *
 * 208 ~ 81%: the readouts stay crisp and the dot matrix below is faintly
 * visible. Want it more "frosted glass"? Turn it down (160 is already quite
 * noticeable, any lower and the readouts start getting hard to read); to switch
 * the effect off completely set `CGL_ALPHA_OPAQUE`(255) -- that path costs
 * nothing, not one extra pixel is computed.
 */
#define APP_INFO_OVERLAY_ALPHA 190u

typedef struct app_page_info
{
    cgl_page_t base; /**< <- must be the first member */

    cgl_screen_t *screen;
    /** Cross-page links, from `ctx->app`. Read to report the smash page's state;
        NULL is legal (a build without that page), so every use is guarded. */
    app_links_t *links;

    cgl_panel_t root;
    cgl_panel_t top_bar;
    cgl_panel_t top_sep;

    cgl_label_t title;
    cgl_label_t hint;
    cgl_label_t lines[APP_INFO_LINES]; /**< alternating left/right columns: even = left, odd = right */
    cgl_label_t physics; /**< its own line, its own colour: whether physics is running */

    uint32_t shown_tick; /**< "clock tick" used for the last refresh, see INFO_REFRESH_MS */
} app_page_info_t;

/** Create the page (registration only; see `cgl_page_ctx_t` and app_ctx.h). */
void app_page_info_attach(cgl_page_t *page, const cgl_page_ctx_t *ctx);

#endif /* APP_PAGE_INFO_H */
