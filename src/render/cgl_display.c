// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_display.c
  * @brief   LCD UI framework - band presenter implementation.
  ******************************************************************************
  */
#include <cytgl/cgl_display.h>

int cgl_display_init(cgl_display_t *d, const cgl_display_drv_t *drv, void *ctx)
{
    if ((d == NULL) || (drv == NULL))
    {
        return -1;
    }

    d->drv = drv;
    d->ctx = ctx;
    d->frame_id = 0u;
    d->bands = 0u;
    cgl_display_damage_all(d);

    if (drv->init != NULL)
    {
        return drv->init(ctx);
    }
    return 0;
}

void cgl_display_damage(cgl_display_t *d, cgl_rect_t r)
{
    if (cgl_rect_is_empty(r))
    {
        return;
    }
    d->damage = cgl_rect_union(d->damage, r);
}

void cgl_display_damage_all(cgl_display_t *d)
{
    if (d->drv != NULL)
    {
        d->damage = cgl_rect(0, 0, (int16_t)(d->drv->width - 1), (int16_t)(d->drv->height - 1));
    }
}

bool cgl_display_has_damage(const cgl_display_t *d)
{
    return !cgl_rect_is_empty(d->damage);
}

uint16_t cgl_display_present(cgl_display_t *d, cgl_render_fn render, void *user)
{
    const cgl_display_drv_t *drv = d->drv;
    cgl_rect_t job;
    uint16_t bands = 0u;
    int16_t y;
    uint8_t cur = 0u;
    bool pending = false;

    if ((drv == NULL) || (render == NULL))
    {
        return 0u;
    }

    job = cgl_rect_intersect(d->damage,
                            cgl_rect(0, 0, (int16_t)(drv->width - 1), (int16_t)(drv->height - 1)));
    if (cgl_rect_is_empty(job))
    {
        d->damage = cgl_rect_none();
        return 0u;
    }

    /* damage is fully processed once the frame has been pushed */
    d->damage = cgl_rect_none();

    for (y = (int16_t)(job.y0 - (job.y0 % CGL_CFG_BAND_H)); y <= job.y1; y = (int16_t)(y + CGL_CFG_BAND_H))
    {
        cgl_canvas_t canvas;
        int16_t band_h = CGL_CFG_BAND_H;
        cgl_rect_t band;

        if ((int16_t)(y + band_h) > drv->height)
        {
            band_h = (int16_t)(drv->height - y);
        }
        band = cgl_rect(0, y, (int16_t)(drv->width - 1), (int16_t)(y + band_h - 1));

        /* The whole band is repainted, never just the damaged part: the panel
           has no framebuffer, so a band that gets pushed must be complete.
           It is rasterised while the previous band is still on the wire. */
        cgl_canvas_init(&canvas, d->band[cur], drv->width, band_h, y);
        cgl_canvas_set_clip(&canvas, band);
#if CGL_CFG_ALPHA
        /* Group-opacity scratch: only one band is rasterised at a time, so it is
           loaded once per band. Note that it is written **by the CPU only**
           (during rendering) and is never handed to the DMA -- unlike band[],
           which the DMA is still reading during the next round, so that buffer
           cannot be used as scratch. */
        cgl_canvas_set_alpha_scratch(&canvas, d->alpha,
                                   (uint8_t)CGL_CFG_ALPHA_LEVELS,
                                   (uint32_t)CGL_CFG_ALPHA_LEVEL_STRIDE);
#endif
        render(&canvas, user);
        bands++;

        /* The bus must be free before the window command can be issued. */
        if (pending && (drv->wait_idle != NULL))
        {
            drv->wait_idle(d->ctx);
        }
        if (drv->set_window != NULL)
        {
            drv->set_window(d->ctx, (uint16_t)band.x0, (uint16_t)band.y0,
                            (uint16_t)band.x1, (uint16_t)band.y1);
        }
        if (drv->write_pixels != NULL)
        {
            drv->write_pixels(d->ctx, d->band[cur],
                              (uint32_t)drv->width * (uint32_t)band_h);
            pending = (drv->wait_idle != NULL);
        }
        cur ^= 1u;
    }

    if (pending && (drv->wait_idle != NULL))
    {
        drv->wait_idle(d->ctx);
    }
    d->frame_id++;
    d->bands += bands;
    return bands;
}

void cgl_display_set_backlight(cgl_display_t *d, uint8_t percent)
{
    if ((d != NULL) && (d->drv != NULL) && (d->drv->set_backlight != NULL))
    {
        d->drv->set_backlight(d->ctx, percent);
    }
}
