// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    app_page_menu.c
  * @brief   Main menu page: the item list, the sliding highlight bar and the
  *          "enter the selected item" action.
  *
  *  See app_page_menu.h for why the page exists at all and for the rule that a
  *  page change is only ever started by an event handled here (`menu__event`).
  ******************************************************************************
  */
#include "app_page_menu.h"
#include "app_fx_config.h" /* APP_FX_PARTICLES: is the bullet time page in this build? */

#include <cytgl/cytgl_config.h>
#include "board_key.h"
/* This translation unit is the one that needs the **definitions** of the pages it
   pushes and reports on. They used to arrive as constructor parameters (and through
   app_page_menu.h); now they arrive through ctx->app, so these includes are explicit
   here instead of being transitive -- which is also why the menu header no longer has
   to include any other page header. */
#include "app_page_bullet.h"
#include "app_page_smash.h"
#include "app_page_info.h"
#include "app_page_clock.h"

/* -------------------------------------------------------------------------- */
/* Layout constants                                                           */
/* -------------------------------------------------------------------------- */

#define PAGE_FONT (&cgl_font_8x16)
#define PAGE_BAR_H 18

#define COL_BAR CGL_RGB565(24, 28, 40)
#define COL_SEP CGL_RGB565(96, 106, 132)
#define COL_DIM CGL_RGB565(150, 158, 176)
#define COL_SEL_BG CGL_CFG_ACCENT_COLOR
#define COL_SEL_FG CGL_RGB565(20, 22, 30)

#define MENU_ITEM_X 14
#define MENU_ITEM_W (CGL_CFG_SCREEN_W - (2 * MENU_ITEM_X)) /* 292 */
#define MENU_ITEM_H 20
#define MENU_ITEM_GAP 4
#define MENU_ITEM_Y0 34

/** The readout line and the footer below the four items (which end at y = 125). */
#define MENU_COUNTER_Y 130
#define MENU_FOOTER_Y 150

/** Highlight bar slide duration. Uses cgl_anim, no hand-written accumulator. */
#define MENU_HL_MS 140u

/** Easing curve of the highlight bar slide. Duration and curve are two different
    things: 140 ms is only 9 frames, and a constant speed looks like a "jump";
    OUT_CUBIC starts fast and eases off at the end, so the bar looks like it was
    pushed over and then stopped. (The page-transition curves are the nav->ease[]
    table, see src/core/cgl_page.h -- each widget has its own, it is not one
    global setting.) */
#define MENU_HL_EASE CGL_EASE_OUT_CUBIC

#define MENU_ITEM_Y(i) ((int16_t)(MENU_ITEM_Y0 + ((i) * (MENU_ITEM_H + MENU_ITEM_GAP))))
#define MENU_ITEM_RECT(i) cgl_rect_wh(MENU_ITEM_X, MENU_ITEM_Y(i), MENU_ITEM_W, MENU_ITEM_H)

/**
 * The menu labels, in item order. They must stay in step with `APP_MENU_ITEMS` and
 * with the order `menu__create()` wires `targets[]` in -- the array is the menu, and
 * there is deliberately no separate "name" field on each item for the two to drift
 * apart.
 */
static const char *const s_menu_names[APP_MENU_ITEMS] = {
    "Watch Face", "Logo Smash", "Bullet Time", "System Info"
};

static void menu__place_highlight(app_page_menu_t *p, int32_t y)
{
    cgl_rect_t f = p->highlight.base.frame;

    f.y0 = (int16_t)y;
    f.y1 = (int16_t)(y + MENU_ITEM_H - 1);
    cgl_widget_set_frame(&p->highlight.base, f);
}

static void menu__sync_labels(app_page_menu_t *p)
{
    uint8_t i;

    for (i = 0u; i < p->count; i++)
    {
        cgl_label_set_colors(&p->items[i],
                            (i == p->sel) ? COL_SEL_FG : CGL_COLOR_CREAM,
                            CGL_CFG_BG_COLOR, false);
    }
}

static void menu__sync_highlight(app_page_menu_t *p, bool animate)
{
    int32_t to = MENU_ITEM_Y(p->sel);

    if (!animate || !cgl_page_is_created(&p->base))
    {
        cgl_anim_init(&p->hl_anim);
        menu__place_highlight(p, to);
        return;
    }
    /* Tween y directly: the easing curve therefore shows up in "how the bar
       slides over", not only in the duration. */
    cgl_anim_start(&p->hl_anim, p->highlight.base.frame.y0, to, MENU_HL_MS,
                  MENU_HL_EASE);
}

void app_page_menu_select(app_page_menu_t *p, uint8_t index)
{
    if ((p == NULL) || (p->count == 0u))
    {
        return;
    }
    index = (uint8_t)(index % p->count);
    if (index == p->sel)
    {
        return;
    }
    p->sel = index;
    menu__sync_labels(p);
    menu__sync_highlight(p, true);
}

static void menu__create(cgl_page_t *page)
{
    app_page_menu_t *p = (app_page_menu_t *)page;
    cgl_rect_t screen = cgl_rect(0, 0, (int16_t)(CGL_CFG_SCREEN_W - 1), (int16_t)(CGL_CFG_SCREEN_H - 1));
    uint8_t i;

    cgl_panel_init(&p->root, "menu.root", screen, CGL_CFG_BG_COLOR, CGL_CFG_BG_COLOR);
    p->root.base.flags |= CGL_WF_CLIP;
    cgl_page_set_root(page, &p->root.base);

    cgl_panel_init(&p->top_bar, "menu.top_bar",
                  cgl_rect_wh(0, 0, CGL_CFG_SCREEN_W, PAGE_BAR_H), COL_BAR, COL_BAR);
    cgl_panel_init(&p->top_sep, "menu.top_sep",
                  cgl_rect_wh(0, PAGE_BAR_H, CGL_CFG_SCREEN_W, 1), COL_SEP, COL_SEP);
    cgl_label_init(&p->title, "menu.title", cgl_rect_wh(6, 0, 150, PAGE_BAR_H),
                  PAGE_FONT, "UI Framework", CGL_CFG_FG_COLOR, COL_BAR, false);
    cgl_label_init(&p->hint, "menu.hint", cgl_rect_wh(150, 0, 164, PAGE_BAR_H),
                  PAGE_FONT, "TAP=NEXT HOLD=OK", COL_SEP, COL_BAR, false);
    cgl_label_set_align(&p->hint, CGL_ALIGN_RIGHT);

    /* The highlight bar is inserted **before** the menu item labels, so it is
       drawn underneath them (draw order = insertion order). */
    cgl_panel_init(&p->highlight, "menu.highlight", MENU_ITEM_RECT(0), COL_SEL_BG, COL_SEL_BG);

    for (i = 0u; i < (uint8_t)APP_MENU_ITEMS; i++)
    {
        cgl_rect_t box = MENU_ITEM_RECT(i);

        cgl_label_init(&p->items[i], "menu.item",
                      cgl_rect_wh((int16_t)(box.x0 + 10), box.y0,
                                 (int16_t)(MENU_ITEM_W - 20), MENU_ITEM_H),
                      PAGE_FONT, s_menu_names[i], CGL_COLOR_CREAM, CGL_CFG_BG_COLOR, false);
    }

    cgl_label_init(&p->counter, "menu.counter",
                  cgl_rect_wh(MENU_ITEM_X, MENU_COUNTER_Y, MENU_ITEM_W, 16),
                  PAGE_FONT, "SMASH STEPS 0", COL_DIM, CGL_CFG_BG_COLOR, false);
    cgl_label_init(&p->footer, "menu.footer",
                  cgl_rect_wh(MENU_ITEM_X, MENU_FOOTER_Y, MENU_ITEM_W, 16),
                  PAGE_FONT, "TAP=NEXT HOLD=OK", COL_SEP,
                  CGL_CFG_BG_COLOR, false);

    cgl_widget_add_child(&p->root.base, &p->top_bar.base);
    cgl_widget_add_child(&p->root.base, &p->top_sep.base);
    cgl_widget_add_child(&p->root.base, &p->title.base);
    cgl_widget_add_child(&p->root.base, &p->hint.base);
    cgl_widget_add_child(&p->root.base, &p->highlight.base);
    for (i = 0u; i < (uint8_t)APP_MENU_ITEMS; i++)
    {
        cgl_widget_add_child(&p->root.base, &p->items[i].base);
    }
    cgl_widget_add_child(&p->root.base, &p->counter.base);
    cgl_widget_add_child(&p->root.base, &p->footer.base);

    p->count = (uint8_t)APP_MENU_ITEMS;
    p->sel = 0u;
    p->shown = 0xFFFFFFFFu;
    p->shown_kind = 0xFFu;
    menu__sync_highlight(p, false);
    menu__sync_labels(p);
}

static void menu__enter(cgl_page_t *page, cgl_page_t *from, uint8_t kind)
{
    app_page_menu_t *p = (app_page_menu_t *)page;
    (void)from;
    (void)kind;

    p->sel = 0u;
    menu__sync_highlight(p, false);
    menu__sync_labels(p);
}

/**
 * The page menu item `index` pushes, or NULL when this build has no such page.
 *
 * The table lives here, in one place, and reads the **link** rather than a copy of
 * the pointer: a page that is absent from the build (`links->bullet == NULL`, which
 * is what a host test that does not build the bullet page has) automatically becomes
 * a "selectable but dead" item. That used to need a second call
 * (`app_page_menu_attach_bullet`) purely to fill in the third slot.
 */
static cgl_page_t *menu__target(const app_page_menu_t *p, uint8_t index)
{
    const app_links_t *l = p->links;

    if (l == NULL)
    {
        return NULL;
    }
    switch (index)
    {
    case APP_MENU_ITEM_CLOCK:
        return (l->clock != NULL) ? &l->clock->base : NULL;
    case APP_MENU_ITEM_SMASH:
        return (l->smash != NULL) ? &l->smash->base : NULL;
    case APP_MENU_ITEM_BULLET:
        return (l->bullet != NULL) ? &l->bullet->base : NULL;
    case APP_MENU_ITEM_INFO:
        return (l->info != NULL) ? &l->info->base : NULL;
    default:
        return NULL;
    }
}

static void menu__activate(app_page_menu_t *p)
{
    cgl_page_t *target = menu__target(p, p->sel);

    if ((p->base.nav == NULL) || (p->sel >= p->count) || (target == NULL))
    {
        return;
    }
    /* Pass AUTO: use the transition **declared by the target page itself**
       (smash page = GENIE_IN, info page = COVER_UP, see their respective
       *_attach()). We no longer keep a duplicate "menu item -> transition" table
       here -- a transition belongs to the page, not to the call site. To force
       one type on a particular navigation, pass CGL_TRANS_* in explicitly. */
    (void)cgl_page_nav_request(p->base.nav, CGL_NAV_PUSH, target,
                              CGL_TRANS_AUTO, NULL);
}

static void menu__frame(cgl_page_t *page, const cgl_clock_t *clk)
{
    app_page_menu_t *p = (app_page_menu_t *)page;
    const app_page_smash_t *smash = ((p->links != NULL) ? p->links->smash : NULL);
#if APP_FX_PARTICLES
    struct app_page_bullet *bullet = ((p->links != NULL) ? p->links->bullet : NULL);
#else
    struct app_page_bullet *bullet = NULL; /* this build has no bullet time page */
#endif
#if APP_FX_SMASH
    uint32_t steps = (smash != NULL) ? app_page_smash_steps(smash) : 0u;
#else
    uint32_t steps = 0u; /* no smash page in this build (app_fx_config.h) */
    (void)smash;
#endif
#if APP_FX_PARTICLES
    uint32_t scale = (bullet != NULL) ? (uint32_t)app_page_bullet_time_scale(bullet) : 0u;
#else
    uint32_t scale = 0u;
#endif
    /* The watch face page: no build switch, so this link is the one that is always
       there (a host test may still pass NULL, which simply makes the item dead). */
    struct app_page_clock *clkpage = ((p->links != NULL) ? p->links->clock : NULL);
    const clock_time_t *now = (clkpage != NULL) ? app_page_clock_now(clkpage) : NULL;
    uint32_t shown = 0u;
    uint8_t kind = 0u;

    /* Highlight bar slide: a typical use of cgl_anim_t -- "a quantity that
       changes continuously with time" is handed over to it */
    if (cgl_anim_tick(&p->hl_anim, clk->dt_ms))
    {
        menu__place_highlight(p, cgl_anim_value(&p->hl_anim));
    }

    /* * The counter line shows a different quantity depending on what the selection
     *   is hovering over, because there is only one spare line on a 172 px screen and
     *   each page has a number worth watching:
     *
     *     "Watch Face"  -> TIME, the time the face is showing, live. It is the page's
     *                      own reading, so it also proves the page keeps its time
     *                      while it is not the top page.
     *     "Logo Smash"  -> SMASH STEPS, which stops growing while the page is buried
     *                      -- the most direct evidence that "a covered page receives
     *                      no on_step"
     *     "Bullet Time" -> TIME, the bullet time page's Q8 time scale. "x1.00" is
     *                      real time, "FROZEN" is scale 0, and watching it ramp is
     *                      the whole effect in one number.
     *
     *   `kind` is what makes "the line changed" distinguishable from "the line now
     *   shows a different quantity": the readouts share one label, so without it a
     *   step count that happens to equal a time of day would leave the line stale.
     */
    switch (p->sel)
    {
    case APP_MENU_ITEM_CLOCK:
        if (now != NULL)
        {
            kind = 2u;
            shown = app_page_clock_seconds_of_day(clkpage);
        }
        break;
    case APP_MENU_ITEM_BULLET:
        if (bullet != NULL)
        {
            kind = 1u;
            shown = scale;
        }
        break;
    default:
        kind = 0u;
        shown = steps;
        break;
    }

    if ((kind != p->shown_kind) || (shown != p->shown))
    {
        p->shown_kind = kind;
        p->shown = shown;
        switch (kind)
        {
        case 2u:
        {
            /* "TIME HH:MM:SS", hand-built: the label API formats one integer, and
               there is no printf in this project's dependency set. */
            char t[14];
            uint32_t sod = shown;

            t[0] = 'T';
            t[1] = 'I';
            t[2] = 'M';
            t[3] = 'E';
            t[4] = ' ';
            t[5] = (char)('0' + (uint8_t)(((sod / 3600u) % 24u) / 10u));
            t[6] = (char)('0' + (uint8_t)((sod / 3600u) % 10u));
            t[7] = ':';
            t[8] = (char)('0' + (uint8_t)(((sod / 60u) % 60u) / 10u));
            t[9] = (char)('0' + (uint8_t)((sod / 60u) % 10u));
            t[10] = ':';
            t[11] = (char)('0' + (uint8_t)((sod % 60u) / 10u));
            t[12] = (char)('0' + (uint8_t)(sod % 10u));
            t[13] = '\0';
            cgl_label_set_text(&p->counter, t);
            break;
        }
        case 1u:
            if (shown == 0u)
            {
                cgl_label_set_text(&p->counter, "TIME STOPPED");
            }
            else
            {
                /* Q8 -> x10 fixed point: 256 -> 10, i.e. "1.0" via set_fixed1 */
                cgl_label_set_fixed1(&p->counter, "TIME x",
                                     (int32_t)((shown * 10u) / 256u), NULL);
            }
            break;
        default:
            cgl_label_set_int(&p->counter, "SMASH STEPS ", (int32_t)shown, NULL);
            break;
        }
    }
}

static bool menu__event(cgl_page_t *page, const cgl_event_t *ev)
{
    app_page_menu_t *p = (app_page_menu_t *)page;

    switch (ev->kind)
    {
    case CGL_EV_KEY_REPEAT:
        /* On this page a long press has no other meaning, so it serves as
           auto-repeat */
        app_page_menu_select(p, (uint8_t)(p->sel + 1u));
        return true;

    case CGL_EV_KEY_UP:
        if (ev->hold_ms < BOARD_KEY_LONG_MS)
        {
            app_page_menu_select(p, (uint8_t)(p->sel + 1u));
        }
        return true;

    case CGL_EV_KEY_LONG:
        /* * Only this page swallows KEY_LONG ("enter the selected item"). Other
           pages do not swallow it, so the CGL_EV_BACK that immediately follows
           is interpreted by the navigator as "go back". This is why
           "long press = back" only takes effect on the pages that need it,
           instead of every page writing it out again. */
        menu__activate(p);
        return true;

    case CGL_EV_BACK:
        return true; /* already the bottom of the stack, there is nowhere to go back to */

    default:
        return false;
    }
}

static const cgl_page_vt_t menu__vt = {
    .type = "page_menu",
    .on_create = menu__create,
    .on_destroy = NULL,
    .on_enter = menu__enter,
    .on_exit = NULL,
    .on_pause = NULL,
    .on_resume = NULL,
    .on_step = NULL, /* the menu has no physics: receiving on_step leaves nothing to do */
    .on_frame = menu__frame,
    .on_event = menu__event,
};

void app_page_menu_attach(cgl_page_t *page, const cgl_page_ctx_t *ctx)
{
    app_page_menu_t *p = (app_page_menu_t *)page;

    cgl_page_init(page, &menu__vt, "menu", (void *)ctx);
    p->screen = (ctx != NULL) ? ctx->screen : NULL;
    /* The items' targets are read from `links` at activation time (menu__target),
       so nothing has to be filled in per item any more and an absent page simply
       produces a dead item. This is what removed app_page_menu_attach_bullet(). */
    p->links = ((ctx != NULL) ? (app_links_t *)ctx->app : NULL);
}
