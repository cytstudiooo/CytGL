// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_screen.h
  * @brief   LCD UI framework - screen: display + clock + page navigator.
  *
  *  The screen owns the frame. `cgl_screen_frame(dt)` is the *whole* frame:
  *
  *      1  cgl_clock_begin()          -- the only place time is accumulated
  *      2  dispatch input, frame boundary -- the widget tree is fixed within a frame
  *      3  cgl_page_nav_frame()       -- commits page changes, advances transitions
  *      4  while (cgl_clock_next_step()) page->on_step()   -- fixed-step logic
  *      5  page->on_frame()           -- per-frame tween / state machine
  *      6  cgl_widget_update_tree()   -- pure visual animation
  *      7  cgl_display_present()      -- band-by-band rasterise + DMA
  *
  *  * The caller supplies one dt; the framework guarantees the order of the
  *  other six steps. The old trap -- "you had to call phys2d_clock_tick()
  *  before cgl_screen_tick(), and with the order reversed the physics froze" --
  *  simply no longer exists at the interface level, because there is only one
  *  function you can call.
  ******************************************************************************
  */
#ifndef CGL_SCREEN_H
#define CGL_SCREEN_H

#include <cytgl/cgl_widget.h>
#include <cytgl/cgl_display.h>
#include <cytgl/cgl_page.h>
#include <cytgl/cytgl_config.h>

/* Tagged on purpose: cgl_page.h (which this file includes) forward-declares
   `cgl_screen_t` so that `cgl_page_ctx_t` can carry a screen pointer without the
   two headers including each other. C11 allows a typedef to be repeated with the
   same type, but that requires the tag to be the same one -- with an anonymous
   struct here the forward declaration would be a *different* type and every
   translation unit that includes both headers would fail to compile. */
typedef struct cgl_screen
{
    cgl_display_t *display;  /**< not owned                                     */
    cgl_page_nav_t *nav;     /**< page navigator, not owned (the app layer holds it) */
    cgl_clock_t clock;       /**< the one and only clock in the project         */

    /* input ring queue: events are enqueued first and dispatched at the frame boundary */
    cgl_event_t queue[CGL_CFG_INPUT_QUEUE];
    uint8_t q_head;
    uint8_t q_tail;
    uint32_t q_dropped;     /**< events dropped because the queue was full (for observation) */
} cgl_screen_t;

/**
 * Bind a display and a page navigator to the screen, install the damage sink and
 * arm the clock with CGL_CFG_CLOCK_STEP_MS.
 *
 * WARNING: the whole framework has exactly one global damage-rectangle exit, so
 * **only one cgl_screen may be active** at a time (see the note on "unwritten
 * seams" in the CytGL manual, section 3.5). This project has a single screen.
 *
 * @return 0 on success; -1 if an argument is NULL or the panel geometry does not
 *         match CGL_CFG_*
 */
int cgl_screen_init(cgl_screen_t *s, cgl_display_t *d, cgl_page_nav_t *nav);

/** The clock (readable and writable). Changing step_ms must happen before the first frame. */
cgl_clock_t *cgl_screen_clock(cgl_screen_t *s);

/** Frame rate over the most recent statistics window. */
uint32_t cgl_screen_fps(const cgl_screen_t *s);

/** The host widget (= the navigator's host); scenes are attached to it. */
cgl_widget_t *cgl_screen_root(cgl_screen_t *s);

/** The navigator (forwarded so the caller does not have to keep its own copy). */
cgl_page_nav_t *cgl_screen_nav(cgl_screen_t *s);

/** The current page. */
cgl_page_t *cgl_screen_top(const cgl_screen_t *s);

/**
 * Post one input event. **It only enqueues, it does not dispatch immediately** --
 * the event takes effect at the frame boundary of the next frame, so calling it
 * from a DMA interrupt / key scan is safe (but it is simplest when there is only
 * one producer; see the note in board_key.h).
 * @return false = the queue was full and the event was dropped
 */
bool cgl_screen_post_event(cgl_screen_t *s, const cgl_event_t *ev);

/** Number of events dropped because the queue was full. */
uint32_t cgl_screen_dropped_events(const cgl_screen_t *s);

/** Advance one frame and present it. dt_ms is the number of milliseconds since the last frame. */
void cgl_screen_frame(cgl_screen_t *s, uint32_t dt_ms);

/** Force a full repaint on the next tick. */
void cgl_screen_redraw_all(cgl_screen_t *s);

#endif /* CGL_SCREEN_H */
