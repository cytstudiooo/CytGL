// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    lcd_bus.h
  * @brief   Panel transport abstraction.
  *
  *  A panel driver (st7789.c) is written against this interface only, so the
  *  same driver can run on hardware SPI + DMA, bit banged GPIO, a parallel bus
  *  or even a simulator. Nothing below knows about panels.
  ******************************************************************************
  */
#ifndef LCD_BUS_H
#define LCD_BUS_H

#include <stdint.h>

typedef struct lcd_bus
{
    const char *name;

    /** Bring up clocks, pins and the transfer engine. Returns 0 on success. */
    int (*init)(void *ctx);

    /** Drive the panel hardware reset line (1 = released). */
    void (*reset)(void *ctx, int level);

    /** Command/data select: 0 = command, 1 = data. */
    void (*dc)(void *ctx, int level);

    /** Backlight, 0 = off, 100 = full. May be NULL. */
    void (*backlight)(void *ctx, uint8_t percent);

    /** Blocking delay. */
    void (*delay_ms)(void *ctx, uint32_t ms);

    /**
     * Blocking transfer of raw bytes (panel commands and parameters).
     * The implementation owns chip select and must leave the bus idle.
     */
    void (*write)(void *ctx, const uint8_t *data, uint32_t len);

    /**
     * Asynchronous transfer of `count` native RGB565 pixels.
     * The buffer must stay valid until wait_idle() returns.
     */
    void (*write_pixels)(void *ctx, const uint16_t *pixels, uint32_t count);

    /** Block until the pixel transfer started by write_pixels() completed. */
    void (*wait_idle)(void *ctx);
} lcd_bus_t;

#endif /* LCD_BUS_H */
