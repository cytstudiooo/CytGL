// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_screen.c
  * @brief   LCD UI framework - screen implementation (the frame pipeline).
  ******************************************************************************
  */
#include <cytgl/cgl_screen.h>

static void screen__damage_sink(cgl_rect_t r, void *user)
{
    cgl_screen_t *s = (cgl_screen_t *)user;

    if ((s != NULL) && (s->display != NULL))
    {
        cgl_display_damage(s->display, r);
    }
}

static void screen__render(cgl_canvas_t *canvas, void *user)
{
    cgl_screen_t *s = (cgl_screen_t *)user;
    cgl_widget_t *root;

    if (s == NULL)
    {
        return;
    }
    root = cgl_page_nav_host(s->nav);
    if (root != NULL)
    {
        cgl_widget_draw_tree(root, canvas);
    }
}

/** Frame boundary: hand all input queued since the last frame to the top page
    in one go. */
static void screen__drain_input(cgl_screen_t *s)
{
    while (s->q_head != s->q_tail)
    {
        cgl_event_t ev;

        /* * Input lock during a transition: do **not consume** the queue head,
           keep it until the transition finishes so it can be delivered. If it
           were popped here and dropped instead ("dispatch returned false, so
           forget it"), a key the user pressed during those 200 ms of transition
           would really be lost -- exactly the feel problem we want to avoid. */
        if (cgl_page_nav_busy(s->nav))
        {
            return;
        }

        ev = s->queue[s->q_head];
        s->q_head = (uint8_t)((s->q_head + 1u) & (uint8_t)(CGL_CFG_INPUT_QUEUE - 1u));
        (void)cgl_page_nav_event(s->nav, &ev);
    }
}

int cgl_screen_init(cgl_screen_t *s, cgl_display_t *d, cgl_page_nav_t *nav)
{
    if ((s == NULL) || (d == NULL) || (nav == NULL) || (d->drv == NULL))
    {
        return -1;
    }
    /* Panel geometry must match the compile-time configuration, otherwise the
       transition offsets are computed wrongly and the damage rectangles go out
       of bounds. */
    if ((d->drv->width != CGL_CFG_SCREEN_W) || (d->drv->height != CGL_CFG_SCREEN_H) ||
        (nav->area.x0 != 0) || (nav->area.y0 != 0) ||
        (cgl_rect_width(nav->area) != CGL_CFG_SCREEN_W) ||
        (cgl_rect_height(nav->area) != CGL_CFG_SCREEN_H))
    {
        return -1;
    }

    s->display = d;
    s->nav = nav;
    s->q_head = 0u;
    s->q_tail = 0u;
    s->q_dropped = 0u;

    cgl_clock_init(&s->clock, CGL_CFG_CLOCK_STEP_MS, CGL_CLOCK_DEFAULT_MAX_DT_MS);

    /* The widget layer reports damage through this global sink, so it must be
       installed before the first frame. */
    cgl_widget_set_damage_sink(screen__damage_sink, s);
    return 0;
}

cgl_clock_t *cgl_screen_clock(cgl_screen_t *s)
{
    return (s == NULL) ? NULL : &s->clock;
}

uint32_t cgl_screen_fps(const cgl_screen_t *s)
{
    return (s == NULL) ? 0u : s->clock.fps;
}

cgl_widget_t *cgl_screen_root(cgl_screen_t *s)
{
    return (s == NULL) ? NULL : cgl_page_nav_host(s->nav);
}

cgl_page_nav_t *cgl_screen_nav(cgl_screen_t *s)
{
    return (s == NULL) ? NULL : s->nav;
}

cgl_page_t *cgl_screen_top(const cgl_screen_t *s)
{
    return (s == NULL) ? NULL : cgl_page_nav_top(s->nav);
}

bool cgl_screen_post_event(cgl_screen_t *s, const cgl_event_t *ev)
{
    uint8_t next;

    if ((s == NULL) || (ev == NULL))
    {
        return false;
    }
    next = (uint8_t)((s->q_tail + 1u) & (uint8_t)(CGL_CFG_INPUT_QUEUE - 1u));
    if (next == s->q_head)
    {
        s->q_dropped++;
        return false;
    }
    s->queue[s->q_tail] = *ev;
    s->q_tail = next;
    return true;
}

uint32_t cgl_screen_dropped_events(const cgl_screen_t *s)
{
    return (s == NULL) ? 0u : s->q_dropped;
}

void cgl_screen_frame(cgl_screen_t *s, uint32_t dt_ms)
{
    cgl_page_t *top;

    if ((s == NULL) || (s->nav == NULL))
    {
        return;
    }

    /* (1) Time: the only place in the whole project that accumulates dt */
    cgl_clock_begin(&s->clock, dt_ms);

    /* (2) Input: dispatched once at the frame boundary, the tree is never
       changed in the middle of a frame */
    screen__drain_input(s);

    /* (3) Navigation: commit pending page changes + advance the transition.
           This is the **only moment at which the widget tree changes**, and it
           runs before on_step / present, so what this frame sees is always a
           stable state. */
    cgl_page_nav_frame(s->nav, &s->clock);

    top = cgl_page_nav_top(s->nav);

    /* (4) Logic: fixed step. The loop count is decided by the clock, so a page
       can neither run extra steps nor skip any. */
    while (cgl_clock_next_step(&s->clock))
    {
        if ((top != NULL) && (top->vt != NULL) && (top->vt->on_step != NULL))
        {
            top->vt->on_step(top, cgl_clock_step_ms(&s->clock));
        }
    }

    /* (5) Frame-level callbacks: tweens, state machines, text refresh */
    if ((top != NULL) && (top->vt != NULL) && (top->vt->on_frame != NULL))
    {
        top->vt->on_frame(top, &s->clock);
    }

    /* (6) Pure visual animation (the widgets' own cgl_anim_t) */
    cgl_widget_update_tree(cgl_page_nav_host(s->nav), s->clock.dt_ms);

    /* (7) Present: repaint only the whole horizontal bands the damaged region
       covers */
    if (s->display != NULL)
    {
        (void)cgl_display_present(s->display, screen__render, s);
    }
}

void cgl_screen_redraw_all(cgl_screen_t *s)
{
    if ((s != NULL) && (s->display != NULL))
    {
        cgl_display_damage_all(s->display);
    }
}
