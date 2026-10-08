// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_bounce_host_test.c
  * @brief   PC-side test: the `cgl_bounce` engine, used as a module.
  *
  *  What this proves that `bounce_test` does not
  *  -------------------------------------------
  *  `bounce_test` drives the whole reference application (page, widget tree,
  *  unified clock, the shatter scene) and asserts on a ball that is one body among
  *  482. That is the right end-to-end test, and it is why it cannot say anything
  *  about the module's own contract: every failure it could show is already mixed
  *  with the page's.
  *
  *  This test links **`cytgl_ext`** and includes **only** `<ext/...>`: no framework
  *  header, no `App/` widget, no demo include path. So it is the file that notices
  *  when the engine's public API stops being self-contained -- the same job
  *  `ext_smoke_test.c` does for the physics pair.
  *
  *  The claims it pins, one section each:
  *
  *    1. the spawn is derived from the playground ("top left, tossed right"), so it
  *       survives the caller moving the playground;
  *    2. `vy += g` happens **before** the integration -- semi-implicit Euler. The
  *       assertion is exact (the position moved by the *new* velocity), because a
  *       "roughly stable" test cannot tell the two orderings apart;
  *    3. the energy does not grow over a long run (the failure mode in claim 2, seen
  *       as a number rather than as an ordering);
  *    4. **self-healing is by identity, not by index**: after a `cgl_phys2d_reset()`
  *       plus somebody else's bodies, the ball must re-add itself rather than take
  *       over a slot that now belongs to another body;
  *    5. an impossible playground is rejected, counted, and leaves the ball inert
  *       instead of half-alive;
  *    6. two balls in two worlds are bit-for-bit independent (the module keeps no
  *       mutable global state);
  *    7. the read path clamps, and the painter's read-only variant does not mutate.
  *
  *  Requires `-DCYTGL_BUILD_EXT=ON` (see tests/CMakeLists.txt).
  *
  *    ctest --test-dir build/host -R cgl_bounce --output-on-failure
  ******************************************************************************
  */
#include <stdio.h>
#include <stdbool.h>

#include <ext/cgl_bounce.h>

/** The reference playground (screen px, the ball's **centre** range): the demo's own,
    which is deliberately a little larger than "the frame inset by the radius". */
static const cgl_bounce_rect_t TEST_AREA = {3, 22, 316, 151};

/** Long enough that the ball crosses the playground many times, including several
    logo-free rebuilds' worth of steps in the reference scene. */
#define LONG_STEPS 900

/** Foreign bodies used by the hijack test: small, at rest, and far enough apart that
    the solver can never move one (40 px apart with an 8 px radius). */
#define FOREIGN_N 6
static const int16_t FOREIGN_R_Q4 = 8 * CGL_EXT_Q4_ONE;

static int s_fails = 0;

static void check(bool ok, const char *what)
{
    printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok)
    {
        s_fails++;
    }
}

/** One logical step of one ball, in the only order the module allows. */
static void step_once(cgl_bounce_t *b)
{
    cgl_bounce_pre_step(b);
    cgl_phys2d_step(b->world);
    cgl_bounce_post_step(b);
}

/**
 * Specific energy per unit mass, in px^2/step^2: `v^2 / 2 + g * height above the
 * floor`. Gravity acts on y only, so this is the quantity a wrong integration order
 * makes grow -- and the reason it is computed from the body rather than tracked by
 * the module is that the module deliberately has no opinion about energy.
 */
static double energy_of(const cgl_bounce_t *b)
{
    const cgl_phys2d_body_t *p = &b->world->body[b->index];
    const double vx = (double)p->vx / (double)CGL_EXT_Q4_ONE;
    const double vy = (double)p->vy / (double)CGL_EXT_Q4_ONE;
    const double y = (double)p->y / (double)CGL_EXT_Q4_ONE;
    const double g = (double)CGL_BOUNCE_GRAVITY_Q4 / (double)CGL_EXT_Q4_ONE;
    const double floor_y = (double)b->bot_q4 / (double)CGL_EXT_Q4_ONE;

    return (0.5 * ((vx * vx) + (vy * vy))) + (g * (floor_y - y));
}

int main(void)
{
    cgl_phys2d_t world;
    cgl_phys2d_t world_b;
    cgl_bounce_t ball;
    cgl_bounce_t ball_b;
    cgl_bounce_rect_t area;
    int16_t x = 0;
    int16_t y = 0;
    int i;

    printf("=== cgl_bounce: the gravity ball engine, used as a module ===\n");
    printf("playground : centre x %d..%d  y %d..%d (screen px)\n",
           TEST_AREA.x0, TEST_AREA.x1, TEST_AREA.y0, TEST_AREA.y1);
    printf("ball r %d px, gravity %d/16 px/step^2, restitution %u/256\n\n",
           CGL_BOUNCE_RADIUS_PX, (int)CGL_BOUNCE_GRAVITY_Q4,
           (unsigned)CGL_BOUNCE_RESTITUTION_Q8);

    /* ----------------------------------------------------------------------- */
    printf("== 1) Init: the playground is accepted, the spawn is derived from it ==\n");
    /* ----------------------------------------------------------------------- */
    cgl_phys2d_reset(&world);
    check(cgl_bounce_init(&ball, &world, TEST_AREA), "a reference playground is accepted");
    check(cgl_bounce_area_rejected(&ball) == 0u, "and nothing was rejected");
    check(world.count == 1u, "the ball is exactly one body in the world");
    check(cgl_bounce_index(&ball) == 0u, "and it is that body's index");

    check(cgl_bounce_area_px(&ball, &area), "the walls can be read back");
    check((area.x0 == TEST_AREA.x0) && (area.x1 == TEST_AREA.x1) &&
          (area.y0 == TEST_AREA.y0) && (area.y1 == TEST_AREA.y1),
          "and they are the ones that were asked for (no silent inset, no silent clamp)");

    check(cgl_bounce_position_px(&ball, &x, &y), "the spawn position is readable");
    check(x == (int16_t)(TEST_AREA.x0 + CGL_BOUNCE_SPAWN_INSET_PX),
          "the spawn x is derived from the playground's left wall (not an absolute pixel)");
    check(y == TEST_AREA.y0, "and it hangs from the ceiling");
    check(cgl_bounce_bounces(&ball) == 0u, "a fresh ball has bounced zero times");

    /* ----------------------------------------------------------------------- */
    printf("\n== 2) One step: vy += g happens BEFORE the integration (semi-implicit) ==\n");
    /* ----------------------------------------------------------------------- */
    /* Free fall from the middle of the playground with no velocity at all, so no wall
       is involved and the first step can be predicted exactly. */
    cgl_bounce_set_spawn(&ball, 100, 60, 0, 0);
    cgl_bounce_restart(&ball);
    check(cgl_bounce_position_px(&ball, &x, &y) && (x == 100) && (y == 60),
          "restart() puts the ball back on the spawn point it was given");
    check(world.body[cgl_bounce_index(&ball)].vy == 0, "at rest");

    step_once(&ball);
    check(world.body[cgl_bounce_index(&ball)].vy == (int16_t)CGL_BOUNCE_GRAVITY_Q4,
          "one step adds exactly one gravity increment to vy");
    check(world.body[cgl_bounce_index(&ball)].y ==
              (int16_t)((60 * CGL_EXT_Q4_ONE) + CGL_BOUNCE_GRAVITY_Q4),
          "and y moved by the NEW vy, not the old one: gravity is applied before the step");
    printf("     y after one step: %d (Q12.4), i.e. %.4f px -- semi-implicit Euler\n",
           (int)world.body[cgl_bounce_index(&ball)].y,
           (double)world.body[cgl_bounce_index(&ball)].y / (double)CGL_EXT_Q4_ONE);

    /* The velocity clamp is part of the refleciton, so it has to hold after the
       reflection too -- this is the "did it blow up" number. */
    check(cgl_bounce_max_speed_q4(&ball) <= CGL_PHYS2D_MAX_VELOCITY,
          "the observed speed stays under the engine's ceiling");

    /* ----------------------------------------------------------------------- */
    printf("\n== 3) The floor catches it, and the energy never grows over %d steps ==\n", LONG_STEPS);
    /* ----------------------------------------------------------------------- */
    /* Back to the reference throw (off the ceiling, tossed right) so that the long run
       exercises the playground's **full height** -- the floor AND the ceiling -- and the
       "wall to wall" claim below has something to measure. */
    cgl_bounce_set_spawn(&ball, 100, TEST_AREA.y0, (int16_t)CGL_BOUNCE_SPAWN_VX_Q4,
                         (int16_t)CGL_BOUNCE_SPAWN_VY_Q4);
    cgl_bounce_restart(&ball);

    {
        double e_first_window = 0.0; /* highest energy seen in the first quarter   */
        double e_last_window = 0.0;  /* ... and in the last quarter                */
        int16_t min_y = 32000;
        int16_t max_y = -32000;
        int out_of_area = 0;

        for (i = 0; i < LONG_STEPS; i++)
        {
            const double e = energy_of(&ball);

            step_once(&ball);

            if ((i < (LONG_STEPS / 4)) && (e > e_first_window))
            {
                e_first_window = e;
            }
            if ((i >= ((LONG_STEPS * 3) / 4)) && (e > e_last_window))
            {
                e_last_window = e;
            }

            if (!cgl_bounce_position_px(&ball, &x, &y))
            {
                out_of_area = 1;
                break;
            }
            if ((x < area.x0) || (x > area.x1) || (y < area.y0) || (y > area.y1))
            {
                out_of_area++;
            }
            if (y < min_y) { min_y = y; }
            if (y > max_y) { max_y = y; }
        }

        printf("     bounces %u, read-position range y %d..%d (walls %d..%d)\n",
               (unsigned)cgl_bounce_bounces(&ball), min_y, max_y, area.y0, area.y1);
        printf("     energy: first quarter max %.3f, last quarter max %.3f px^2/step^2\n",
               e_first_window, e_last_window);

        check(!out_of_area, "no step ever exposed a position outside the playground");
        check(cgl_bounce_bounces(&ball) >= 10u,
              "the ball keeps bouncing instead of sticking to the floor");
        check((min_y <= (int16_t)(area.y0 + 4)) && (max_y >= (int16_t)(area.y1 - 4)),
              "and it uses the whole height of the playground, wall to wall");
        check(e_last_window < e_first_window,
              "the energy DECAYS across the run: the integrator is not energy-positive");
    }

    /* ----------------------------------------------------------------------- */
    printf("\n== 4) Self-healing is by identity: a rebuilt world cannot be hijacked ==\n");
    /* ----------------------------------------------------------------------- */
    /* The trap: cgl_phys2d_reset() only zeroes `count`. It frees nothing and tells
       nobody, so the ball's index can end up **inside** the new pool, pointing at a
       body that is not the ball. Checking `index < count` (which is what the effect
       did before it moved into the module) then writes the ball's state over somebody
       else's body: the dot is dragged around the screen and the ball's walls act on
       it. This section is that scenario, and the assertion is that the foreign body
       is byte-for-byte untouched. */
    cgl_phys2d_reset(&world);
    for (i = 0; i < 4; i++)
    {
        (void)cgl_phys2d_add(&world, (int16_t)((40 + (i * 40)) * CGL_EXT_Q4_ONE),
                             (int16_t)(120 * CGL_EXT_Q4_ONE), 0, 0, FOREIGN_R_Q4, 0u);
    }
    check(cgl_bounce_init(&ball, &world, TEST_AREA), "the ball joins a world that already has bodies");
    check(cgl_bounce_index(&ball) == 4u, "and lands at the end of the pool (index 4)");

    cgl_phys2d_reset(&world);
    for (i = 0; i < FOREIGN_N; i++)
    {
        (void)cgl_phys2d_add(&world, (int16_t)((40 + (i * 40)) * CGL_EXT_Q4_ONE),
                             (int16_t)(120 * CGL_EXT_Q4_ONE), 0, 0, FOREIGN_R_Q4, 0u);
    }
    {
        /* Remember what sits where the ball used to be. */
        const int16_t fx = world.body[4].x;
        const int16_t fy = world.body[4].y;
        const int16_t fr = world.body[4].r;
        const uint16_t ff = world.body[4].flags;

        step_once(&ball);

        check(cgl_bounce_index(&ball) == FOREIGN_N,
              "the ball re-added itself at the end of the pool instead of taking index 4");
        check(world.count == (uint16_t)(FOREIGN_N + 1),
              "which leaves exactly one more body than the foreign ones");
        check((world.body[4].x == fx) && (world.body[4].y == fy) &&
              (world.body[4].r == fr) && (world.body[4].flags == ff),
              "and the body that took its old index was left completely alone");
        check((world.body[cgl_bounce_index(&ball)].r == (int16_t)CGL_BOUNCE_R_Q4) &&
              ((world.body[cgl_bounce_index(&ball)].flags & CGL_PHYS2D_FLAG_SPHERE) != 0u),
              "the slot it did take is a sphere of the ball's own radius");
    }

    /* ----------------------------------------------------------------------- */
    printf("\n== 5) An impossible playground is rejected, counted, and leaves the ball inert ==\n");
    /* ----------------------------------------------------------------------- */
    {
        cgl_phys2d_t w;
        cgl_bounce_t bad;
        /* Inverted: no position could ever be inside it. */
        const cgl_bounce_rect_t inverted = {100, 100, 50, 200};
        /* Reaching past the engine's simulation domain (x < 360): the ball would stop
           entering the broad phase out there, i.e. stop colliding with anything. */
        const cgl_bounce_rect_t outside = {0, 0, 1000, 100};

        cgl_phys2d_reset(&w);
        check(!cgl_bounce_init(&bad, &w, inverted), "an inverted playground is rejected");
        check(cgl_bounce_area_rejected(&bad) == 1u, "the rejection is counted");
        check(w.count == 0u, "and nothing was added to the world");
        check(cgl_bounce_index(&bad) == CGL_BOUNCE_NO_INDEX, "the inert ball has no index");
        check(!cgl_bounce_position_px(&bad, NULL, NULL), "and reports no position");
        check(!cgl_bounce_area_px(&bad, &area), "and has no area to report");
        step_once(&bad); /* must be a no-op, not a crash */
        check((cgl_bounce_bounces(&bad) == 0u) && (w.count == 0u),
              "stepping an inert ball is a safe no-op");

        check(!cgl_bounce_init(&bad, &w, outside),
              "a playground reaching outside the simulation domain is rejected too");

        /* A missing world is a different kind of failure: a programming error rather than
           a bad value, so it must not spend the area-rejection count. */
        check(!cgl_bounce_init(&bad, NULL, TEST_AREA), "no world at all is refused");
        check(cgl_bounce_area_rejected(&bad) == 0u,
              "and that is not counted as a rejected playground (it is not one)");
        check(!cgl_bounce_position_px(&bad, NULL, NULL), "and that ball is inert as well");

        /* A rejected *resize* must not leave the ball with no walls at all. */
        check(!cgl_bounce_set_area(&ball, inverted), "a bad resize is rejected");
        check(cgl_bounce_area_px(&ball, &area) && (area.x0 == TEST_AREA.x0) &&
              (area.y1 == TEST_AREA.y1),
              "and the playground that was already in force stays in force");
        check(cgl_bounce_area_rejected(&ball) == 1u, "the resize rejection is counted on the ball");
    }

    /* ----------------------------------------------------------------------- */
    printf("\n== 6) Two balls in two worlds are independent (no mutable global state) ==\n");
    /* ----------------------------------------------------------------------- */
    cgl_phys2d_reset(&world);
    cgl_phys2d_reset(&world_b);
    check(cgl_bounce_init(&ball, &world, TEST_AREA) && cgl_bounce_init(&ball_b, &world_b, TEST_AREA),
          "two balls bound to two worlds");
    for (i = 0; i < 300; i++)
    {
        step_once(&ball);
        step_once(&ball_b);
    }
    {
        bool same = true;
        const cgl_phys2d_body_t *pa = &world.body[cgl_bounce_index(&ball)];
        const cgl_phys2d_body_t *pb = &world_b.body[cgl_bounce_index(&ball_b)];

        same = (pa->x == pb->x) && (pa->y == pb->y) && (pa->vx == pb->vx) && (pa->vy == pb->vy);
        same = same && (cgl_bounce_bounces(&ball) == cgl_bounce_bounces(&ball_b));
        same = same && (cgl_bounce_max_speed_q4(&ball) == cgl_bounce_max_speed_q4(&ball_b));
        check(same, "identical inputs give a bit-for-bit identical result: the module is deterministic");
        printf("     after 300 steps: pos (%d, %d) Q12.4, %u bounces\n",
               pa->x, pa->y, (unsigned)cgl_bounce_bounces(&ball));
    }

    /* ----------------------------------------------------------------------- */
    printf("\n== 7) The read path clamps; the painter's read-only variant does not mutate ==\n");
    /* ----------------------------------------------------------------------- */
    {
        int16_t dx = 0;
        int16_t dy = 0;
        uint16_t idx = cgl_bounce_index(&ball);

        /* Shove the body far outside the playground behind the module's back. */
        world.body[idx].x = (int16_t)((area.x0 - 40) * CGL_EXT_Q4_ONE);
        world.body[idx].y = (int16_t)((area.y1 + 40) * CGL_EXT_Q4_ONE);

        check(cgl_bounce_position_px(&ball, &x, &y), "an out-of-bounds body is still readable");
        check((x >= area.x0) && (x <= area.x1) && (y >= area.y0) && (y <= area.y1),
              "and what comes out is clamped inside the playground (the invariant is kept at the READ point)");
        check(cgl_bounce_draw_position_px(&ball, &dx, &dy) && (dx == x) && (dy == y),
              "the painter's read-only variant reports the same, clamped position");

        /* After a reset with no step since, the painter must not add a body: it runs
           inside a draw callback and adds to a world it does not own. The readout may. */
        cgl_phys2d_reset(&world);
        check(!cgl_bounce_draw_position_px(&ball, NULL, NULL),
              "the painter sees nothing in a freshly reset world (it never self-heals)");
        check(world.count == 0u, "and it did not add a body doing so");
        check(cgl_bounce_position_px(&ball, &x, &y),
              "the self-healing readout does heal, which is what the next step would have done");
        check((world.count == 1u) && (x == (int16_t)(TEST_AREA.x0 + CGL_BOUNCE_SPAWN_INSET_PX)),
              "back on the spawn point, as the pool's only body");
    }

    printf("\n");
    if (s_fails == 0)
    {
        printf("all passed.\n");
        return 0;
    }
    printf("%d failed.\n", s_fails);
    return 1;
}
