// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_display.h
  * @brief   LCD UI framework - display device abstraction and band presenter.
  *
  *  The framework talks to hardware exclusively through the cgl_display_drv_t
  *  vtable. A driver only has to be able to:
  *     - initialise itself,
  *     - program a rectangular write window,
  *     - push a run of RGB565 pixels (asynchronously, DMA style),
  *     - report when the bus is idle.
  *
  *  Presentation is band based (see cgl_config.h): the display renders the
  *  screen in horizontal strips, double buffered, so the CPU rasterises strip
  *  N+1 while strip N is still being shifted out to the panel.
  ******************************************************************************
  */
#ifndef CGL_DISPLAY_H
#define CGL_DISPLAY_H

#include <cytgl/cgl_types.h>
#include <cytgl/cgl_canvas.h>
#include <cytgl/cytgl_config.h>

/* -------------------------------------------------------------------------- */
/* driver interface                                                           */
/* -------------------------------------------------------------------------- */

typedef struct cgl_display_drv
{
    const char *name;

    /** Bring the panel up. Returns 0 on success. */
    int (*init)(void *ctx);

    /** Program the write window. Coordinates are inclusive, panel space. */
    void (*set_window)(void *ctx, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

    /**
     * Push `count` native RGB565 pixels to the active window.
     * The call is asynchronous; `scratch` stays owned by the caller until
     * wait_idle() returns.
     */
    void (*write_pixels)(void *ctx, const cgl_color_t *scratch, uint32_t count);

    /** Block until the panel bus finished the last transfer. */
    void (*wait_idle)(void *ctx);

    /** 0..100, backlight level (may be NULL). */
    void (*set_backlight)(void *ctx, uint8_t percent);

    int16_t width;  /**< panel width  in pixels, panel must be in landscape   */
    int16_t height; /**< panel height in pixels                               */
} cgl_display_drv_t;

/* -------------------------------------------------------------------------- */
/* display instance                                                           */
/* -------------------------------------------------------------------------- */

typedef struct
{
    const cgl_display_drv_t *drv;
    void *ctx;

    cgl_rect_t damage; /**< pending redraw region, screen coordinates          */

    /* band double buffer: 2 x CGL_CFG_SCREEN_W x CGL_CFG_BAND_H pixels         */
    cgl_color_t band[2][CGL_CFG_SCREEN_W * CGL_CFG_BAND_H];

#if CGL_CFG_ALPHA
    /**
     * save-under layering buffer for group opacity (see cgl_widget_set_alpha /
     * cgl_config.h).
     *
     * It is the "background copy" handed to the canvas for **each band**, one
     * layer per CGL_CFG_ALPHA_LEVEL_STRIDE pixels. It lives in the display layer
     * because only here is it known who provides a band; and because this memory,
     * exactly like the band buffer, must be touched by the **CPU only** and must
     * never go to DMA.
     *
     * When CGL_CFG_ALPHA = 0 this member does not exist at all (RAM drops to
     * zero), the canvas keeps alpha_scratch NULL, and alpha is ignored and
     * counted.
     */
    cgl_color_t alpha[CGL_CFG_ALPHA_LEVELS * CGL_CFG_ALPHA_LEVEL_STRIDE];
#endif

    uint32_t frame_id; /**< frames pushed to the panel (**not** a frame rate;       */
                       /**<   the frame rate lives in cgl_clock)                   */
    uint32_t bands;    /**< total horizontal bands pushed (for observation)        */
} cgl_display_t;

/** Callback used to paint one band. `user` is the pointer given to present. */
typedef void (*cgl_render_fn)(cgl_canvas_t *canvas, void *user);

/** Bind a driver to the display instance. Returns 0 on success. */
int cgl_display_init(cgl_display_t *d, const cgl_display_drv_t *drv, void *ctx);

/** Mark a screen rectangle as needing a repaint. */
void cgl_display_damage(cgl_display_t *d, cgl_rect_t r);

/** Mark the whole screen as needing a repaint (first frame / mode change). */
void cgl_display_damage_all(cgl_display_t *d);

/** True when something is waiting to be repainted. */
bool cgl_display_has_damage(const cgl_display_t *d);

/** Render and push every damaged band. Returns the number of bands pushed. */
uint16_t cgl_display_present(cgl_display_t *d, cgl_render_fn render, void *user);

/** Backlight convenience wrapper (safe when the driver has no control). */
void cgl_display_set_backlight(cgl_display_t *d, uint8_t percent);

#endif /* CGL_DISPLAY_H */
