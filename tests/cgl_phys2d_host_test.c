// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_phys2d_host_test.c
  * @brief   Host side harness for the physics engine (not part of the firmware).
  *
  *  Compiles the real application physics on a PC so the tuning can be checked
  *  without hardware:
  *
  *    gcc -O2 -Wall -Iinclude -Iexamples/STM32G431_CytStudio_Demo/App \
  *        -o build/logo_test tests/cgl_phys2d_host_test.c \
  *        src/ext/cgl_phys2d.c src/ext/cgl_debris.c \
  *        src/core/cgl_widget.c src/core/cgl_canvas.c
  *  It reports the simulation cycle: how long the logo survives, how long the
  *  wrecking ball takes to cross, when the wake clears and whether any body
  *  escapes the fixed point range.
  ******************************************************************************
  */
#include <stdio.h>

#include <cytgl/cgl_widget.h>
#include <ext/cgl_debris.h>
#include "logo_bitmap.h" /* the bitmap is the caller''s: the scene takes it as a callback */
#include <ext/cgl_phys2d.h>

static cgl_phys2d_t s_world;
static cgl_debris_t s_logo;

/** The scene's view of the bitmap: the module is handed callbacks, not a header. */
static bool test__is_lit(int i, int j)
{
    return logo_bitmap_get(i, j) != 0;
}

static const cgl_debris_src_t s_src = {LOGO_GRID_W, LOGO_GRID_H, test__is_lit};

/**
 * Milliseconds of one logic step -- kept the same as CGL_CFG_CLOCK_STEP_MS on
 * the target.
 *
 * This test **no longer drives the shared clock**: physics stepping is now the
 * sole responsibility of the page's on_step, so the test simply spells out the
 * two steps -- integrate once, let the widget observe once. The old
 * "cgl_phys2d_clock_tick() first, then update()" ordering trap no longer has a
 * place to exist.
 */
#define TEST_STEP_MS 16u

static const char *phase_name(uint8_t p)
{
    return (p == (uint8_t)CGL_DEBRIS_PHASE_SMASH) ? "smash" : "settle";
}

int main(void)
{
    cgl_rect_t frame = cgl_rect(0, 0, 319, 153);
    int step;
    int last_restarts = 0;
    int cycle_start_step = 1;
    int min_x = 40000;
    int max_x = -40000;
    int min_y = 40000;
    int max_y = -40000;
    unsigned i;
    /* 2.5D: did "leaving the ground" actually happen during the whole 20 s, and how big was it*/
    int32_t max_z_all = 0;
    int max_airborne = 0;
    int z_fail = 0;

    /* view = NULL: this is a pure physics test, so no camera / sorter is needed (rendering is covered by the render tests)*/
    cgl_debris_init(&s_logo, &s_world, NULL, &s_src, (cgl_debris_rect_t){frame.x0, frame.y0, frame.x1, frame.y1});

    printf("logo dots      : %u (body pool %d)\n",
           (unsigned)cgl_debris_dot_count(&s_logo), CGL_PHYS2D_MAX_BODIES);
    printf("broad phase    : %d cells (%d x %d), cell size %d/16 px\n",
           CGL_PHYS2D_GRID_CELLS, CGL_PHYS2D_GRID_W, CGL_PHYS2D_GRID_H, 1 << CGL_PHYS2D_CELL_SHIFT);
    printf("world domain   : x %d..%d  y %d..%d px\n",
           CGL_PHYS2D_WORLD_X0 >> 4, (CGL_PHYS2D_WORLD_X0 + CGL_PHYS2D_WORLD_W) >> 4,
           CGL_PHYS2D_WORLD_Y0 >> 4, (CGL_PHYS2D_WORLD_Y0 + CGL_PHYS2D_WORLD_H) >> 4);

    for (i = 0u; i < s_world.count; i++)
    {
        const cgl_phys2d_body_t *b = &s_world.body[i];
        if (b->x < min_x) { min_x = b->x; }
        if (b->x > max_x) { max_x = b->x; }
        if (b->y < min_y) { min_y = b->y; }
        if (b->y > max_y) { max_y = b->y; }
    }
    printf("logo extent    : x %d..%d  y %d..%d px\n\n",
           min_x >> 4, max_x >> 4, min_y >> 4, max_y >> 4);

    printf("     t     phase   dots   ball x   ball y    vx\n");
    for (step = 1; step <= (20 * 1000 / (int)TEST_STEP_MS); step++)
    {
        cgl_phys2d_t *w = s_logo.world;
        const cgl_phys2d_body_t *ball;

        /* Drive the loop exactly like the framework does: one fixed logic step.
           The render pass is irrelevant here.

           Firmware path: app_ui_tick() -> cgl_port_board_frame() ->
                          cgl_screen_frame() -> page->on_step()
                          -> cgl_phys2d_step() + cgl_debris_step().
           This test once omitted the "feed the shared clock first" line, so the
           heavy ball never moved from its spawn point while the test kept
           reporting PASS (see git history). Stepping is one function call now, so
           it can neither be forgotten nor done twice.*/
        cgl_phys2d_step(&s_world);
        cgl_debris_step(&s_logo, TEST_STEP_MS);
        ball = &w->body[s_logo.ball_index];

        if ((step % 15) == 0)
        {
            int moving = 0;
            int fastest = 0;
            int dmin_x = 40000;
            int dmax_x = -40000;
            int dmin_y = 40000;
            int dmax_y = -40000;
            for (i = 0u; i < w->count; i++)
            {
                const cgl_phys2d_body_t *b = &w->body[i];
                int v;
                if (i == s_logo.ball_index)
                {
                    continue;
                }
                if (b->x < dmin_x) { dmin_x = b->x; }
                if (b->x > dmax_x) { dmax_x = b->x; }
                if (b->y < dmin_y) { dmin_y = b->y; }
                if (b->y > dmax_y) { dmax_y = b->y; }
                v = (b->vx < 0) ? -b->vx : b->vx;
                if ((b->vy < 0 ? -b->vy : b->vy) > v) { v = (b->vy < 0) ? -b->vy : b->vy; }
                if (v > 0) { moving++; }
                if (v > fastest) { fastest = v; }
            }
            printf("%6.2fs %-6s dots=%4u moving=%4d vmax=%3d  bbox x %4d..%4d y %4d..%4d  ball=%4d\n",
                   (double)step * (double)TEST_STEP_MS / 1000.0, phase_name(s_logo.phase),
                   (unsigned)s_logo.active, moving, fastest,
                   dmin_x >> 4, dmax_x >> 4, dmin_y >> 4, dmax_y >> 4, ball->x >> 4);
        }

        if ((int)s_logo.restarts != last_restarts)
        {
            printf("  -> cycle %d finished: %d steps = %.2f s\n\n",
                   last_restarts, step - cycle_start_step,
                   (double)(step - cycle_start_step) * (double)TEST_STEP_MS / 1000.0);
            last_restarts = (int)s_logo.restarts;
            cycle_start_step = step;
        }

        /* sanity: nothing may leave the signed 16 bit fixed point range */
        {
            int airborne = 0;

            for (i = 0u; i < w->count; i++)
            {
                if ((w->body[i].x > 30000) || (w->body[i].x < -30000) ||
                    (w->body[i].y > 30000) || (w->body[i].y < -30000))
                {
                    printf("FAIL: body %u out of range at step %d (%d,%d)\n",
                           i, step, w->body[i].x, w->body[i].y);
                    return 1;
                }
                /* 2.5D vertical axis invariants: z is never negative and never overflows*/
                if ((w->body[i].z < 0) || (w->body[i].z > (32 * CGL_PHYS2D_ONE)))
                {
                    printf("FAIL: body %u z out of range at step %d (%d)\n",
                           i, step, w->body[i].z);
                    z_fail++;
                }
                if (w->body[i].z > max_z_all)
                {
                    max_z_all = w->body[i].z;
                }
                if (w->body[i].z > 0)
                {
                    airborne++;
                }
            }
            if (airborne > max_airborne)
            {
                max_airborne = airborne;
            }
        }
        if (z_fail != 0)
        {
            return 1;
        }
    }

    printf("\n2.5D vertical axis: max height reached in 20 s is %d/16 px, at most %d dots airborne at once\n",
           (int)max_z_all, max_airborne);
    if (max_z_all <= 0)
    {
        printf("FAIL: the heavy ball hit for 20 s and did not knock a single dot up (the 2.5D axis is not working)\n");
        return 1;
    }
    if (max_airborne < 10)
    {
        printf("FAIL: only %d dots are airborne at the same time, the pseudo-3D look will not show\n", max_airborne);
        return 1;
    }
    printf("PASS: the heavy ball really does knock dots into the air (%.1f px high, %d on screen)\n",
           (double)max_z_all / 16.0, max_airborne);

    printf("\n%d full cycles in 20 s of simulated time\n", (int)s_logo.restarts);
    return 0;
}
