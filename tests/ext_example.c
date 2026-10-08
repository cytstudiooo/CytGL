// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    ext_example.c
  * @brief   The optional module's "hello world": a tiny world, projected and drawn.
  *
  *  Nothing from the framework appears here -- no widget, no canvas, no display
  *  driver, no framework header. "Drawing" is a 96x48 character screen held
  *  in memory, so the whole program is the module's two `.c` files plus this one:
  *
  *      gcc -std=c11 -O2 -Wall -Wextra -Iinclude \
  *          -o build/ext_example tests/ext_example.c src/ext/cgl_phys2d.c src/ext/cgl_iso2d.c
  *
  *  What it demonstrates, in the order a real renderer does it:
  *
  *    1. the caller owns the world (here: one static, so it never touches the stack),
  *    2. the caller owns the clock -- `cgl_phys2d_step()` is exactly one step,
  *    3. one camera for the whole scene, fitted once to the world rectangle,
  *    4. depth-sort far -> near and then draw straight through: that *is* the
  *       occlusion, there is no z-buffer,
  *    5. the height `z` lifts the dot on screen, and its shadow stays on the floor.
  *
  *  The contract this example obeys (and the one-renderer rule it implies) is in
  *  the CytGL manual, section 15.10.
  ******************************************************************************
  */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include <ext/cgl_phys2d.h>
#include <ext/cgl_iso2d.h>
#include <ext/cgl_iso2d_phys2d.h> /* cgl_iso2d_sort_bodies(): the world -> sorter bridge */

#define MOCK_W 96
#define MOCK_H 48

static char s_screen[MOCK_H][MOCK_W + 1];

static void put(int32_t x, int32_t y, char ch)
{
    if ((x >= 0) && (x < MOCK_W) && (y >= 0) && (y < MOCK_H)) { s_screen[y][x] = ch; }
}

int main(void)
{
    static cgl_phys2d_t world; /* ~12.4 KB of pool: static, not on the stack */
    cgl_iso2d_view_t view;
    int i;
    uint16_t k;

    memset(s_screen, ' ', sizeof(s_screen));
    for (i = 0; i < MOCK_H; i++) { s_screen[i][MOCK_W] = '\0'; }

    cgl_phys2d_reset(&world);
    for (i = 0; i < 8; i++) /* two rows of four dots, drifting right at three different speeds */
    {
        (void)cgl_phys2d_add(&world,
                         (int16_t)((60 + ((i % 4) * 40)) * CGL_PHYS2D_ONE),
                         (int16_t)((90 + ((i / 4) * 30)) * CGL_PHYS2D_ONE),
                         (int16_t)((2 + (i % 3)) * CGL_PHYS2D_ONE), 0,
                         (int16_t)(3 * CGL_PHYS2D_ONE), 0u);
    }
    (void)cgl_phys2d_add(&world, /* one heavy kinematic ball, ploughing in from the left */
                     (int16_t)(-60 * CGL_PHYS2D_ONE), (int16_t)(140 * CGL_PHYS2D_ONE),
                     (int16_t)(4 * CGL_PHYS2D_ONE), 0, (int16_t)(5 * CGL_PHYS2D_ONE),
                     CGL_PHYS2D_FLAG_KINEMATIC | CGL_PHYS2D_FLAG_SPHERE);

    for (i = 0; i < 20; i++) { cgl_phys2d_step(&world); }

    cgl_iso2d_view_init(&view);
    cgl_iso2d_cam_fit(&view.cam, CGL_ISO2D_STYLE_ISO,
                  (int16_t)CGL_PHYS2D_WORLD_X0, (int16_t)CGL_PHYS2D_WORLD_Y0,
                  (int16_t)CGL_PHYS2D_WORLD_W, (int16_t)CGL_PHYS2D_WORLD_H, 0, 0, MOCK_W, MOCK_H);
    cgl_iso2d_sort_bodies(&view.sorter, &view.cam, &world);

    for (k = 0u; k < view.sorter.count; k++) /* far -> near, so later dots cover earlier ones */
    {
        const cgl_phys2d_body_t *b = &world.body[view.sorter.order[k]];
        const bool sphere = ((b->flags & CGL_PHYS2D_FLAG_SPHERE) != 0u);
        const int32_t gx = cgl_iso2d_ground_x(&view.cam, b->x, b->y) + view.cam.ox;
        const int32_t gy = cgl_iso2d_ground_y(&view.cam, b->x, b->y) + view.cam.oy;
        int32_t sy = gy - (((int32_t)view.cam.kz * b->z) >> 12);

        if (sphere) { sy -= cgl_phys2d_to_px(b->r); } /* a ball stands on the floor: lift it by its radius */
        put(gx, gy, 'o');                        /* the shadow stays on the floor ... */
        put(gx, sy, sphere ? 'O' : '.');          /* ... the body rises above it     */
    }

    for (i = 0; i < MOCK_H; i++) { puts(s_screen[i]); }
    printf("%u bodies in the world, %u drawn after the depth sort\n", world.count, view.sorter.count);
    return (view.sorter.count == world.count) ? 0 : 1;
}
