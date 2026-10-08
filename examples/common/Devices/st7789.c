// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    st7789.c
  * @brief   ST7789 panel driver implementation.
  *
  *  Init sequence taken from the module vendor example shipped in the
  *  docs/"1.47IPS ST7789..."/"05-..._SPI" example routine folder (both the
  *  folder and the file name are Chinese in the vendor package; kept here as
  *  a description to stay ASCII), reorganised around the lcd_bus_t transport
  *  and the cgl_display_drv_t device interface.
  ******************************************************************************
  */
#include "st7789.h"

/* -------------------------------------------------------------------------- */
/* controller commands                                                        */
/* -------------------------------------------------------------------------- */
#define ST7789_SWRESET 0x01u
#define ST7789_SLPOUT 0x11u
#define ST7789_NORON 0x13u
#define ST7789_INVON 0x21u
#define ST7789_DISPON 0x29u
#define ST7789_CASET 0x2Au
#define ST7789_RASET 0x2Bu
#define ST7789_RAMWR 0x2Cu
#define ST7789_MADCTL 0x36u
#define ST7789_COLMOD 0x3Au
#define ST7789_PORCTRL 0xB2u
#define ST7789_GCTRL 0xB7u
#define ST7789_VCOMS 0xBBu
#define ST7789_LCMCTRL 0xC0u
#define ST7789_VDVVRHEN 0xC2u
#define ST7789_VRHS 0xC3u
#define ST7789_VDVSET 0xC4u
#define ST7789_FRCTR2 0xC6u
#define ST7789_PWCTRL1 0xD0u
#define ST7789_PVGAMCTRL 0xE0u
#define ST7789_NVGAMCTRL 0xE1u

/* -------------------------------------------------------------------------- */
/* geometry per rotation (172 x 320 visible inside a 240 x 320 GRAM)          */
/* -------------------------------------------------------------------------- */
typedef struct
{
    int16_t width;
    int16_t height;
    int16_t x_off;
    int16_t y_off;
    uint8_t madctl;
} st7789_mode_t;

static const st7789_mode_t st7789_modes[4] = {
    {172, 320, 34, 0, 0x00u}, /* portrait            */
    {172, 320, 34, 0, 0xC0u}, /* portrait, rotated   */
    {320, 172, 0, 34, 0x70u}, /* landscape           */
    {320, 172, 0, 34, 0xA0u}, /* landscape, rotated  */
};

/* -------------------------------------------------------------------------- */
/* low level helpers                                                          */
/* -------------------------------------------------------------------------- */

static void st7789__cmd(st7789_t *p, uint8_t cmd, const uint8_t *args, uint32_t n)
{
    p->bus->dc(p->bus_ctx, 0);
    p->bus->write(p->bus_ctx, &cmd, 1u);
    if ((n > 0u) && (args != NULL))
    {
        p->bus->dc(p->bus_ctx, 1);
        p->bus->write(p->bus_ctx, args, n);
    }
}

static void st7789__cmd1(st7789_t *p, uint8_t cmd, uint8_t a0)
{
    st7789__cmd(p, cmd, &a0, 1u);
}

/* -------------------------------------------------------------------------- */
/* cgl_display_drv_t                                                           */
/* -------------------------------------------------------------------------- */

static int st7789__init(void *ctx)
{
    st7789_t *p = (st7789_t *)ctx;
    static const uint8_t porctrl[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    static const uint8_t pwctrl1[] = {0xA4, 0xA1};
    static const uint8_t pvgam[] = {0xF0, 0x00, 0x04, 0x04, 0x04, 0x05, 0x29, 0x33,
                                    0x3E, 0x38, 0x12, 0x12, 0x28, 0x30};
    static const uint8_t nvgam[] = {0xF0, 0x07, 0x0A, 0x0D, 0x0B, 0x07, 0x28, 0x33,
                                    0x3E, 0x36, 0x14, 0x14, 0x29, 0x32};

    if ((p == NULL) || (p->bus == NULL))
    {
        return -1;
    }
    if ((p->bus->init != NULL) && (p->bus->init(p->bus_ctx) != 0))
    {
        return -1;
    }

    /* Start dark: the panel is about to be reset and re-initialised, so nothing
       should be visible until the application has something to show. 0 always
       means "off", whichever way the board switches the LED (see lcd_bus_t). */
    if (p->bus->backlight != NULL)
    {
        p->bus->backlight(p->bus_ctx, 0u);
    }

    /* hardware reset */
    if (p->bus->reset != NULL)
    {
        p->bus->reset(p->bus_ctx, 0);
        p->bus->delay_ms(p->bus_ctx, 30u);
        p->bus->reset(p->bus_ctx, 1);
        p->bus->delay_ms(p->bus_ctx, 120u);
    }

    st7789__cmd(p, ST7789_SWRESET, NULL, 0u);
    p->bus->delay_ms(p->bus_ctx, 120u);
    st7789__cmd1(p, ST7789_SLPOUT, 0x00);
    p->bus->delay_ms(p->bus_ctx, 120u);

    /* memory access control: orientation */
    st7789__cmd1(p, ST7789_MADCTL, p->madctl);

    /* 16 bit / RGB565 */
    st7789__cmd1(p, ST7789_COLMOD, 0x05);

    st7789__cmd(p, ST7789_PORCTRL, porctrl, sizeof(porctrl));
    st7789__cmd1(p, ST7789_GCTRL, 0x35);
    st7789__cmd1(p, ST7789_VCOMS, 0x35);
    st7789__cmd1(p, ST7789_LCMCTRL, 0x2C);
    st7789__cmd1(p, ST7789_VDVVRHEN, 0x01);
    st7789__cmd1(p, ST7789_VRHS, 0x13);
    st7789__cmd1(p, ST7789_VDVSET, 0x20);
    st7789__cmd1(p, ST7789_FRCTR2, 0x0F);
    st7789__cmd(p, ST7789_PWCTRL1, pwctrl1, sizeof(pwctrl1));
    st7789__cmd(p, ST7789_PVGAMCTRL, pvgam, sizeof(pvgam));
    st7789__cmd(p, ST7789_NVGAMCTRL, nvgam, sizeof(nvgam));

    /* IPS panel: inversion is required for correct colours */
    st7789__cmd(p, ST7789_INVON, NULL, 0u);
    st7789__cmd(p, ST7789_NORON, NULL, 0u);
    p->bus->delay_ms(p->bus_ctx, 10u);
    st7789__cmd(p, ST7789_DISPON, NULL, 0u);
    p->bus->delay_ms(p->bus_ctx, 20u);

    /* Deliberately left dark. Initialising the panel is not the same as having
       something to show, and a backlight switched on here would reveal whatever
       the GRAM happened to hold. The application turns it on once its first frame
       is on the glass -- see CGL_PORT_BOOT_BACKLIGHT_PERCENT in the project's
       cgl_port.h and app_ui_build() in App/app_ui.c. */
    if (p->bus->backlight != NULL)
    {
        p->bus->backlight(p->bus_ctx, 0u);
    }
    return 0;
}

static void st7789__set_window(void *ctx, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    st7789_t *p = (st7789_t *)ctx;
    uint8_t xs[4];
    uint8_t ys[4];
    uint16_t cx0 = (uint16_t)(x0 + (uint16_t)p->x_off);
    uint16_t cx1 = (uint16_t)(x1 + (uint16_t)p->x_off);
    uint16_t cy0 = (uint16_t)(y0 + (uint16_t)p->y_off);
    uint16_t cy1 = (uint16_t)(y1 + (uint16_t)p->y_off);

    xs[0] = (uint8_t)(cx0 >> 8);
    xs[1] = (uint8_t)(cx0 & 0xFFu);
    xs[2] = (uint8_t)(cx1 >> 8);
    xs[3] = (uint8_t)(cx1 & 0xFFu);
    ys[0] = (uint8_t)(cy0 >> 8);
    ys[1] = (uint8_t)(cy0 & 0xFFu);
    ys[2] = (uint8_t)(cy1 >> 8);
    ys[3] = (uint8_t)(cy1 & 0xFFu);

    st7789__cmd(p, ST7789_CASET, xs, 4u);
    st7789__cmd(p, ST7789_RASET, ys, 4u);
    st7789__cmd(p, ST7789_RAMWR, NULL, 0u);
}

static void st7789__write_pixels(void *ctx, const cgl_color_t *scratch, uint32_t count)
{
    st7789_t *p = (st7789_t *)ctx;

    p->bus->dc(p->bus_ctx, 1);
    p->bus->write_pixels(p->bus_ctx, scratch, count);
}

static void st7789__wait_idle(void *ctx)
{
    st7789_t *p = (st7789_t *)ctx;

    if (p->bus->wait_idle != NULL)
    {
        p->bus->wait_idle(p->bus_ctx);
    }
}

static void st7789__set_backlight(void *ctx, uint8_t percent)
{
    st7789_t *p = (st7789_t *)ctx;

    if (p->bus->backlight != NULL)
    {
        p->bus->backlight(p->bus_ctx, percent);
    }
}

const cgl_display_drv_t st7789_display_drv = {
    "st7789",
    st7789__init,
    st7789__set_window,
    st7789__write_pixels,
    st7789__wait_idle,
    st7789__set_backlight,
    (int16_t)320,
    (int16_t)172,
};

void st7789_bind(st7789_t *panel, const lcd_bus_t *bus, void *bus_ctx, uint8_t rotation)
{
    const st7789_mode_t *m = &st7789_modes[rotation & 0x03u];

    panel->bus = bus;
    panel->bus_ctx = bus_ctx;
    panel->rotation = (uint8_t)(rotation & 0x03u);
    panel->width = m->width;
    panel->height = m->height;
    panel->x_off = m->x_off;
    panel->y_off = m->y_off;
    panel->madctl = m->madctl;
}
