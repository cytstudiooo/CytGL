// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    ext_smoke_test.c
  * @brief   Smoke test for the optional `cytgl_ext` target (cgl_phys2d + cgl_iso2d).
  *
  *  What this proves that the other physics tests do not
  *  ---------------------------------------------------
  *  `cgl_phys2d_host_test.c` / `cgl_iso2d_host_test.c` compile the module's `.c` files
  *  directly (as loose sources), so they prove the *code* is correct. This test
  *  proves the **target** is usable: it includes only the module's own public
  *  headers and links `cytgl_ext`, with no `App/` widget and no `src/` framework
  *  header anywhere in its include graph. If the target's public include
  *  directory, or its transitive link to `cytgl`, were ever wrong, this is the
  *  only test that notices.
  *
  *  It deliberately asserts almost nothing about the simulation itself -- the
  *  tuning is `logo_test`'s and `cgl_iso2d_test`'s business. What it pins down are
  *  the two facts without which the module would be useless:
  *
  *    1. a world actually advances (one step integrates positions and stamps),
  *    2. `cgl_iso2d_sort()` returns a true permutation of the active bodies.
  *
  *  Requires `-DCYTGL_BUILD_EXT=ON` (see tests/CMakeLists.txt).
  ******************************************************************************
  */
#include <stdio.h>
#include <stdbool.h>

#include <ext/cgl_phys2d.h>
#include <ext/cgl_iso2d.h>
/* The module's own bridge from a world to the sorter; still no `App/` widget and
   still no framework header in the include graph. */
#include <ext/cgl_iso2d_phys2d.h>

#define SMOKE_BODIES 3
#define SMOKE_STEPS  10

static int s_fails = 0;

static void check(bool ok, const char *what)
{
    printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok)
    {
        s_fails++;
    }
}

int main(void)
{
    cgl_phys2d_t world;
    cgl_iso2d_view_t view;
    uint8_t seen[SMOKE_BODIES];
    int16_t x_before;
    int i;
    bool perm = true;

    printf("=== cytgl_ext smoke test: the module used on its own ===\n");
    printf("module: pool %d, %u B per body, sizeof(cgl_phys2d_t) %u B, %d depth buckets\n",
           CGL_PHYS2D_MAX_BODIES, (unsigned)sizeof(cgl_phys2d_body_t),
           (unsigned)sizeof(cgl_phys2d_t), CGL_ISO2D_DEPTH_BUCKETS);

    cgl_phys2d_reset(&world);
    /* Far enough apart that they cannot touch, and slow enough to stay inside the
       domain for all SMOKE_STEPS (2 px/step * 10 = 20 px of travel at most). */
    (void)cgl_phys2d_add(&world, (int16_t)(40 * CGL_PHYS2D_ONE), (int16_t)(40 * CGL_PHYS2D_ONE),
                     (int16_t)(2 * CGL_PHYS2D_ONE), 0, (int16_t)(2 * CGL_PHYS2D_ONE), 0u);
    (void)cgl_phys2d_add(&world, (int16_t)(150 * CGL_PHYS2D_ONE), (int16_t)(120 * CGL_PHYS2D_ONE),
                     0, (int16_t)CGL_PHYS2D_ONE, (int16_t)(2 * CGL_PHYS2D_ONE), 0u);
    (void)cgl_phys2d_add(&world, (int16_t)(260 * CGL_PHYS2D_ONE), (int16_t)(40 * CGL_PHYS2D_ONE),
                     (int16_t)(-CGL_PHYS2D_ONE), (int16_t)CGL_PHYS2D_ONE,
                     (int16_t)(2 * CGL_PHYS2D_ONE), 0u);
    check(world.count == SMOKE_BODIES, "three bodies were added to a freshly reset world");

    x_before = world.body[0].x;
    for (i = 0; i < SMOKE_STEPS; i++)
    {
        cgl_phys2d_step(&world);
    }
    check(world.step == (uint32_t)SMOKE_STEPS, "cgl_phys2d_step() counted every step it simulated");
    check(world.body[0].x == (int16_t)(x_before + (SMOKE_STEPS * 2 * CGL_PHYS2D_ONE)),
          "a step integrates: the first body moved by exactly its velocity x steps");
    check(world.active == SMOKE_BODIES,
          "no body was culled: an undisturbed world is still complete after 10 steps");

    cgl_iso2d_view_init(&view);
    cgl_iso2d_cam_fit(&view.cam, CGL_ISO2D_STYLE_ISO,
                  (int16_t)CGL_PHYS2D_WORLD_X0, (int16_t)CGL_PHYS2D_WORLD_Y0,
                  (int16_t)CGL_PHYS2D_WORLD_W, (int16_t)CGL_PHYS2D_WORLD_H,
                  0, 0, (int16_t)CGL_PHYS2D_SCREEN_W, (int16_t)CGL_PHYS2D_SCREEN_H);

    cgl_iso2d_sort_bodies(&view.sorter, &view.cam, &world);
    check(view.sorter.count == SMOKE_BODIES, "the depth sort covers every active body");

    for (i = 0; i < SMOKE_BODIES; i++)
    {
        seen[i] = 0u;
    }
    for (i = 0; i < (int)view.sorter.count; i++)
    {
        const int16_t idx = view.sorter.order[i];

        if ((idx < 0) || (idx >= SMOKE_BODIES) || (seen[idx] != 0u))
        {
            perm = false;
            break;
        }
        seen[idx] = 1u;
    }
    check(perm, "cgl_iso2d_sort() returned a true permutation (every active body exactly once)");

    /* The step stamp is part of the public contract: the renderer calls the sort
       once per 4 px band (43 times a frame), so a repeat within one physics step
       must be a no-op rather than redone work. */
    cgl_iso2d_sort_bodies(&view.sorter, &view.cam, &world);
    check(view.sorter.stamp == world.step, "a repeated sort within one step is stamped, not redone");

    printf("\n");
    if (s_fails == 0)
    {
        printf("all passed.\n");
        return 0;
    }
    printf("%d failed.\n", s_fails);
    return 1;
}
