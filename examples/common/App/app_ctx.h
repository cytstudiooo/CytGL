// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    app_ctx.h
  * @brief   What a page may reach: the application's page links, behind one
  *          uniform registration context.
  *
  *  Why this file exists
  *  --------------------
  *  Before it, every page was created by a differently shaped function:
  *
  *      app_page_smash_init (p, screen);
  *      app_page_info_init  (p, screen, smash);
  *      app_page_bullet_init(p, screen, smash);
  *      app_page_menu_init  (p, screen, smash, info);
  *      app_page_menu_attach_bullet(p, bullet);        <- and a second call for the
  *                                                        page the list had no room for
  *
  *  Each signature was defensible alone; the set of them was not. The question a
  *  reader actually has -- "what do I write to add a page?" -- had no single answer,
  *  and a fifth page would have invented a sixth signature.
  *
  *  From here on there is exactly one answer:
  *
  *      void mypage_attach(cgl_page_t *page, const cgl_page_ctx_t *ctx);
  *
  *  plus one row in the page table in `app_ui.c`. Cross-page references do not go
  *  through extra parameters any more; they go through **this** object, reached as
  *  `ctx->app` (see `cgl_page_ctx_t` in cgl_page.h).
  *
  *  Ownership
  *  ---------
  *  Every pointer here is **borrowed** and points at the application's own static
  *  pages (`s_app` in app_ui.c) -- or, in a host test, at that test's own static
  *  pages. A NULL entry means "this build/config does not have that page", which is
  *  a legal state: the menu shows the item as selectable-but-dead, and nothing
  *  crashes.
  *
  *  The whole object is reachable from a page through the context it was registered
  *  with, so no page needs to keep its own copy of any of these pointers:
  *
  *      const app_links_t *links = (const app_links_t *)cgl_page_ctx(&p->base)->app;
  *
  *  which is what `app_links()` below wraps.
  ******************************************************************************
  */
#ifndef APP_CTX_H
#define APP_CTX_H

#include <cytgl/cgl_page.h>

/* Forward declarations by struct tag: this header is included by every page
   header, so it must not include any of them. Every page struct is tagged for
   exactly this reason (see the note in app_page_bullet.h, which was the first). */
struct app_page_smash;
struct app_page_bullet;
struct app_page_info;
struct app_page_menu;
struct app_page_clock;

/**
 * The application's page links. One static instance, filled once before the pages
 * are registered, read by the pages at run time.
 */
typedef struct
{
    struct app_page_smash *smash;
    struct app_page_bullet *bullet;
    struct app_page_info *info;
    struct app_page_menu *menu;
    /** The watch face page. It has no dependencies of its own (no ext module, no
        borrowed RAM), so unlike `smash` / `bullet` it is never NULL in a real
        build -- but it is a link like any other, so a host test that does not build
        the page gets a "selectable but dead" menu item instead of a crash. */
    struct app_page_clock *clock;
} app_links_t;

/** The links a page was registered with (`ctx->app`), or NULL when it was not
    registered through a context. Never NULL in this application, but the accessor
    does not assume it -- a page that is constructed directly (a unit test that only
    wants the widget tree) is a legal caller. */
static inline app_links_t *app_links(const cgl_page_t *page)
{
    const cgl_page_ctx_t *ctx = cgl_page_ctx(page);

    return (ctx == NULL) ? NULL : (app_links_t *)ctx->app;
}

/** The screen a page was registered with (`ctx->screen`), or NULL. */
static inline cgl_screen_t *app_page_screen(const cgl_page_t *page)
{
    const cgl_page_ctx_t *ctx = cgl_page_ctx(page);

    return (ctx == NULL) ? NULL : ctx->screen;
}

#endif /* APP_CTX_H */
