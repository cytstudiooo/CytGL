// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_port.c
  * @brief   CytGL -- porting layer glue implementation.
  *
  *  There is NO hardware code at all in here: this file only wires the
  *  `cgl_port_t` descriptor up to `cgl_display_t` / `cgl_screen_t` and provides
  *  a frame function that computes dt on its own. The real board-level driver
  *  lives in examples/ or in the user's project (see the notes in cgl_port.h).
  *
  *  The frame function is also where the port's input source is pumped: dt is
  *  computed first, then `port->poll_input(dt)` runs, then the screen drains the
  *  events it posted. Keeping that order inside the framework means the port and
  *  the application never have to agree on "who polls the keys, and with which
  *  dt" -- there is exactly one frame entry point.
  ******************************************************************************
  */
#include <cytgl/cgl_port.h>

/* Instance used when the navigator is held statically inside the framework
   (call `cgl_port_bind(..., nav = NULL)`) */
static cgl_page_nav_t s_default_nav;
static bool s_default_nav_used = false;

int cgl_port_bind(const cgl_port_t *port, cgl_port_ctx_t *ctx, cgl_page_nav_t *nav)
{
    int rc;

    if ((port == NULL) || (ctx == NULL) || (port->display == NULL))
    {
        return -1;
    }

    ctx->port = port;
    ctx->last_tick = 0u;
    ctx->bound = false;

    /* (1) Board preparation (optional) */
    if ((port->board_init != NULL) && (port->board_init(port->board_ctx) != 0))
    {
        return -1;
    }

    /* (2) Bind the display driver -- a geometry mismatch fails right here */
    rc = cgl_display_init(&ctx->display, port->display, port->display_ctx);
    if (rc != 0)
    {
        return rc;
    }

    /* (3) With no external navigator, fall back to the framework's internal
       static instance */
    if (nav == NULL)
    {
        if (!s_default_nav_used)
        {
            s_default_nav_used = true;
        }
        nav = &s_default_nav;
        cgl_page_nav_init(nav, "default", cgl_rect_wh(0, 0,
                                                      CGL_CFG_SCREEN_W,
                                                      CGL_CFG_SCREEN_H));
    }

    /* (4) Create the screen */
    rc = cgl_screen_init(&ctx->screen, &ctx->display, nav);
    if (rc != 0)
    {
        return rc;
    }

    ctx->bound = true;
    if (port->tick_ms != NULL)
    {
        ctx->last_tick = port->tick_ms();
    }
    return 0;
}

/**
 * Pump the port's input source with the dt of the frame that is about to run.
 *
 * The events are only queued (cgl_screen_post_event), so the screen delivers them
 * at the frame boundary -- the port never touches the widget tree from here.
 */
static void port__poll_input(cgl_port_ctx_t *ctx, uint32_t dt_ms)
{
    const cgl_port_t *port = ctx->port;

    if (port->poll_input != NULL)
    {
        port->poll_input(port->input_ctx, dt_ms);
    }
}

uint32_t cgl_port_frame(cgl_port_ctx_t *ctx)
{
    uint32_t now;
    uint32_t dt;

    if ((ctx == NULL) || !ctx->bound || (ctx->port == NULL))
    {
        return 0u;
    }

    if (ctx->port->tick_ms == NULL)
    {
        /* No clock source: let the caller compute dt itself and go through
           cgl_screen_frame(). Here we advance one minimum step so that at least
           the picture still gets flushed. */
        port__poll_input(ctx, (uint32_t)CGL_CFG_CLOCK_STEP_MS);
        cgl_screen_frame(&ctx->screen, (uint32_t)CGL_CFG_CLOCK_STEP_MS);
        return (uint32_t)CGL_CFG_CLOCK_STEP_MS;
    }

    now = ctx->port->tick_ms();
    dt = (uint32_t)(now - ctx->last_tick); /* Unsigned subtraction: wrap-safe */
    ctx->last_tick = now;

    /* Input first, with this frame's dt: a key scanned now is dequeued by the
       very frame we are about to run, instead of one frame later. */
    port__poll_input(ctx, dt);

    cgl_screen_frame(&ctx->screen, dt);
    return dt;
}

cgl_display_t *cgl_port_display(cgl_port_ctx_t *ctx)
{
    if ((ctx == NULL) || !ctx->bound)
    {
        return NULL;
    }
    return &ctx->display;
}

cgl_screen_t *cgl_port_screen(cgl_port_ctx_t *ctx)
{
    if ((ctx == NULL) || !ctx->bound)
    {
        return NULL;
    }
    return &ctx->screen;
}

bool cgl_port_post_event(cgl_port_ctx_t *ctx, const cgl_event_t *ev)
{
    cgl_screen_t *s = cgl_port_screen(ctx);

    if ((s == NULL) || (ev == NULL))
    {
        return false;
    }
    return cgl_screen_post_event(s, ev);
}

void cgl_port_set_backlight(cgl_port_ctx_t *ctx, uint8_t percent)
{
    cgl_display_t *d = cgl_port_display(ctx);

    if (d != NULL)
    {
        cgl_display_set_backlight(d, percent);
    }
}
