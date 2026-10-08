// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    st7789.h
  * @brief   ST7789 (1.47" 172x320 IPS) panel driver.
  *
  *  Written against lcd_bus.h and exposed to the UI framework as a
  *  cgl_display_drv_t, so the panel is a swappable component at both ends.
  ******************************************************************************
  */
#ifndef ST7789_H
#define ST7789_H

#include "lcd_bus.h"
#include <cytgl/cgl_display.h>

/** Panel orientation codes, matching the ubiquitous vendor MADCTL table. */
#define ST7789_ROT_PORTRAIT 0u  /* 172 x 320 */
#define ST7789_ROT_PORTRAIT_R 1u
#define ST7789_ROT_LANDSCAPE 2u /* 320 x 172, used by this project */
#define ST7789_ROT_LANDSCAPE_R 3u

typedef struct
{
    const lcd_bus_t *bus;
    void *bus_ctx;
    uint8_t rotation;
    int16_t width;
    int16_t height;
    int16_t x_off;
    int16_t y_off;
    uint8_t madctl;
} st7789_t;

/** Bind a transport to a panel instance and compute its geometry. */
void st7789_bind(st7789_t *panel, const lcd_bus_t *bus, void *bus_ctx, uint8_t rotation);

/** cgl_display_drv_t implementation. Pass the st7789_t* as the display ctx. */
extern const cgl_display_drv_t st7789_display_drv;

#endif /* ST7789_H */
