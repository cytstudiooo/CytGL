// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    app_page_menu.h
  * @brief   Main menu page: the item list, the sliding highlight bar and the
  *          "enter the selected item" action.
  *
  *  Why this page (and the info page) are worth their own files
  *  ----------------------------------------------------------
  *  A page system cannot be validated with a single page. These two "boring"
  *  pages, plus the smash / bullet time pages, happen to cover every shape of the
  *  stack, and they are **verifiable with the naked eye**:
  *
  *      menu (bottom of the stack, installed by CGL_NAV_RESET)
  *        |- "Watch Face"   -> PUSH, COVER_DOWN (declared by the clock page)
 *        |- "Logo Smash"   -> PUSH, GENIE_IN   (declared by the smash page)
  *        |- "Bullet Time"  -> PUSH, GENIE_IN   (declared by the bullet time page)
  *        `- "System Info"  -> PUSH, COVER_UP   (declared by the info page)
  *
  *  The evidence of "a buried page is frozen" is the counter line on this page
  *  and the readouts on the info page: while a page is covered they stop growing,
  *  and they keep going after you return (see app_page_smash.c's on_step/on_pause).
  *
  *  Navigation is event-driven, and only event-driven
  *  ------------------------------------------------
  *  This page changes the stack in exactly one place -- `menu__event()`, on a long
  *  press -- and it passes CGL_TRANS_AUTO so that the **target page's own
  *  declaration** decides the transition. There is deliberately no idle timer, no
  *  script and no state machine that would navigate "by itself": a page change can
  *  always be traced back to a key.
  ******************************************************************************
  */
#ifndef APP_PAGE_MENU_H
#define APP_PAGE_MENU_H

#include <cytgl/cgl_types.h>
#include <cytgl/cgl_page.h>
#include <cytgl/cgl_panel.h>
#include <cytgl/cgl_label.h>
#include <cytgl/cgl_anim.h>
#include <cytgl/cgl_screen.h>

#include "app_ctx.h"

/** Number of menu items. */
#define APP_MENU_ITEMS 4

/**
 * Item order. The watch face leads because it is the one page a build **always**
 * contains: `APP_FX_SMASH 0` / `APP_FX_PARTICLES 0` (see app_fx_config.h) turn the
 * two effect pages into items that show but do nothing, and a menu whose first
 * entries are dead reads as broken. The effects are the interesting pages; the
 * watch face is the reliable one, so it goes on top.
 */
#define APP_MENU_ITEM_CLOCK 0u
#define APP_MENU_ITEM_SMASH 1u
#define APP_MENU_ITEM_BULLET 2u
#define APP_MENU_ITEM_INFO 3u

/* The menu's pages come from `links` (ctx->app), not from constructor parameters.
   This header used to forward declare `struct app_page_bullet` and include the other
   two page headers purely to name them in its parameter list; with one context
   object it needs neither, which is the decoupling the uniform page API buys. */

typedef struct app_page_menu
{
    cgl_page_t base; /**< <- must be the first member */

    cgl_screen_t *screen;

    cgl_panel_t root;
    cgl_panel_t top_bar;
    cgl_panel_t top_sep;
    cgl_panel_t highlight; /**< highlight bar that slides along with the selection */

    cgl_label_t title;
    cgl_label_t hint;
    cgl_label_t items[APP_MENU_ITEMS];
    cgl_label_t counter; /**< accumulated logical steps of the smash page (stops growing while buried) */
    cgl_label_t footer;

    /** Cross-page links, from `ctx->app`: the pages the items go to, and the two
        the readouts report on. NULL entries are legal -- the item then shows but
        does nothing, which is what a host test that does not build that page
        wants. */
    app_links_t *links;
    uint8_t count;
    uint8_t sel;

    uint32_t shown;

    /**
     * Which readout `shown` is encoding, so that "the line changed" and "the line
     * now shows something else" cannot be confused: 0 = smash steps, 1 = the bullet
     * time scale, 2 = the watch page's time of day. It replaced a single encoded
     * counter, which could only work while two readouts shared the line.
     */
    uint8_t shown_kind;

    /**
     * Highlight bar tween: it **tweens the y coordinate directly**, so the easing
     * curve shows up in "how it slides over".
     */
    cgl_anim_t hl_anim;
} app_page_menu_t;

/**
 * Create the page (registration only; see `cgl_page_ctx_t` and app_ctx.h).
 *
 * This used to take two page pointers and then need a **second** call
 * (`app_page_menu_attach_bullet`) for the third, because a widenable parameter list
 * would have broken every existing caller. With one context object there is one
 * shape for every page and no second call: the item targets are read from
 * `ctx->app` at run time.
 */
void app_page_menu_attach(cgl_page_t *page, const cgl_page_ctx_t *ctx);

/** Select item number index directly (does not trigger entering). Useful for a
    programmatic caller (test harness, remote command) that wants to pre-select an
    item before the user long-presses. */
void app_page_menu_select(app_page_menu_t *p, uint8_t index);

#endif /* APP_PAGE_MENU_H */
