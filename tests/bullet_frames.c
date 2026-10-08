// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    bullet_frames.c
  * @brief   Render the bullet time page to PPM/BMP snapshots (host only).
  *
  *  Same mock-panel trick as `render_host_test.c`, but for the bullet time page
  *  alone: it drives real frames through the real page and dumps the moments that
  *  matter, so the effect can be checked by eye (or by a script) without hardware.
  *
  *      bullet_frames [outdir]        default "build"
  *
  *  Snapshots: 00 the intact Logo, 01 the ball inbound, 02 the impact frame,
  *  03..05 the debris flying, 06..09 the freeze and the orbit at 0/90/180/270 deg,
  *  10 the release. Each frame's phase, time scale and yaw are printed next to the
  *  file so the picture can be tied to the state machine.
  ******************************************************************************
  */
#include <stdio.h>
#include <string.h>

#include <cytgl/cgl_display.h>
#include <cytgl/cgl_screen.h>
#include <cytgl/cgl_page.h>

#include "app_page_smash.h"
#include "app_page_bullet.h"
#include "test_image_out.h"

static cgl_color_t s_framebuffer[CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H];
static uint16_t s_row;
static uint16_t s_col;

static int mock__init(void *ctx) { (void)ctx; return 0; }

static void mock__set_window(void *ctx, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    (void)ctx;
    (void)x1;
    (void)y1;
    s_col = x0;
    s_row = y0;
}

static void mock__write_pixels(void *ctx, const cgl_color_t *px, uint32_t count)
{
    uint32_t i;

    (void)ctx;
    for (i = 0u; i < count; i++)
    {
        if ((s_row < CGL_CFG_SCREEN_H) && (s_col < CGL_CFG_SCREEN_W))
        {
            s_framebuffer[((uint32_t)s_row * CGL_CFG_SCREEN_W) + s_col] = px[i];
        }
        s_col++;
        if (s_col >= CGL_CFG_SCREEN_W)
        {
            s_col = 0u;
            s_row++;
        }
    }
}

static void mock__wait_idle(void *ctx) { (void)ctx; }
static void mock__backlight(void *ctx, uint8_t p) { (void)ctx; (void)p; }

static const cgl_display_drv_t s_mock_drv = {
    "mock", mock__init, mock__set_window, mock__write_pixels, mock__wait_idle, mock__backlight,
    CGL_CFG_SCREEN_W, CGL_CFG_SCREEN_H,
};

static cgl_display_t s_display;
static cgl_screen_t s_screen;
static cgl_page_nav_t s_nav;
static app_page_smash_t s_smash;
static app_page_bullet_t s_bullet;

static const char *const s_phase_name[] = {
    "BUILD", "FLY", "FREEZE", "ORBIT", "RELEASE", "SETTLE"
};

static int s_shot;
static const char *s_dir = "build";

static void dump(const char *tag)
{
    char path[256];
    const uint8_t ph = app_page_bullet_phase(&s_bullet);

    (void)snprintf(path, sizeof(path), "%s/bullet%02d.ppm", s_dir, s_shot);
    img_dump_ppm(path, s_shot, s_framebuffer, CGL_CFG_SCREEN_W, CGL_CFG_SCREEN_H);
    (void)snprintf(path, sizeof(path), "%s/bullet%02d.bmp", s_dir, s_shot);
    img_dump_bmp(path, s_shot, s_framebuffer, CGL_CFG_SCREEN_W, CGL_CFG_SCREEN_H);

    (void)printf("  bullet%02d  %-34s phase=%-7s scale=%3u yaw=%3u deg alive=%u\n",
                 s_shot, tag, s_phase_name[ph % 6u],
                 (unsigned)app_page_bullet_time_scale(&s_bullet),
                 (unsigned)app_page_bullet_yaw_deg(&s_bullet),
                 (unsigned)app_page_bullet_alive(&s_bullet));
    s_shot++;
}

int main(int argc, char **argv)
{
    int i;
    int seen_freeze = 0;
    int seen_release = 0;
    int seen_settle = 0;
    int done = 0;

    if (argc > 1)
    {
        s_dir = argv[1];
    }

    if (cgl_display_init(&s_display, &s_mock_drv, NULL) != 0)
    {
        return 1;
    }
    cgl_page_nav_init(&s_nav, "frames",
                      cgl_rect(0, 0, (int16_t)(CGL_CFG_SCREEN_W - 1), (int16_t)(CGL_CFG_SCREEN_H - 1)));
    /* One context for both pages (static: the pages keep a pointer to it). */
    static app_links_t links;
    static cgl_page_ctx_t ctx;

    links.smash = &s_smash;
    links.bullet = &s_bullet;
    links.info = NULL;
    links.menu = NULL;
    ctx.screen = &s_screen;
    ctx.nav = &s_nav;
    ctx.app = &links;

    app_page_smash_attach(&s_smash.base, &ctx);
    app_page_bullet_attach(&s_bullet.base, &ctx);
    if (cgl_screen_init(&s_screen, &s_display, &s_nav) != 0)
    {
        return 1;
    }
    (void)cgl_page_nav_reset(&s_nav, &s_bullet.base);
    cgl_screen_frame(&s_screen, 0u);

    dump("intact Logo (before any step)");

    /* Run the round at the firmware cadence and snapshot by phase + yaw.
       The capture points are **relative to the burst**, not to the frame index: the
       fly phase is 1.25 s long (`CGL_PARTICLES_TRIGGER_TICKS`), so an absolute frame number
       silently stops meaning "just after the impact" the moment that is retuned. */
    for (i = 0; i < 1400; i++)
    {
        cgl_screen_frame(&s_screen, 16u);

        switch (app_page_bullet_phase(&s_bullet))
        {
        case BULLET_PHASE_BUILD:
            if (i == 30)
            {
                dump("ball inbound");
            }
            break;
        case BULLET_PHASE_FLY:
            if (cgl_particles_tick(s_bullet.particles) == (CGL_PARTICLES_TRIGGER_TICKS / 2u))
            {
                dump("debris flying (half way to the freeze)");
            }
            break;
        case BULLET_PHASE_FREEZE:
            if (!seen_freeze)
            {
                seen_freeze = 1;
                dump("freeze ramping (camera already turning)");
            }
            break;
        case BULLET_PHASE_ORBIT:
            /* one snapshot per quarter turn */
            {
                static const uint8_t marks[4] = { 0u, 64u, 128u, 192u };
                static int seen[4];
                const uint8_t y = app_page_bullet_yaw(&s_bullet);
                int q;

                for (q = 0; q < 4; q++)
                {
                    const int d = (int)y - (int)marks[q];

                    if ((!seen[q]) && (d >= 0) && (d < 3))
                    {
                        char tag[48];

                        seen[q] = 1;
                        (void)snprintf(tag, sizeof(tag), "frozen, orbit at %d deg", marks[q] * 45 / 32);
                        dump(tag);
                    }
                }
            }
            break;
        case BULLET_PHASE_RELEASE:
            if (!seen_release)
            {
                seen_release = 1;
                dump("release, the camera comes home");
            }
            break;
        case BULLET_PHASE_SETTLE:
            if (!seen_settle)
            {
                seen_settle = 1;
                dump("settle: camera home, the ball leaves the stage");
                done = 1; /* the round is documented end to end; stop here */
            }
            break;
        default:
            break;
        }

        if (done)
        {
            break;
        }
    }

    (void)printf("\n%d snapshots written to %s/\n", s_shot, s_dir);
    return 0;
}
