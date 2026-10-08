// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_phys2d_storage_host_test.c
  * @brief   `CGL_PHYS2D_USER_STORAGE=1`: a world running entirely on caller memory.
  *
  *  This is the one test that cannot go through the `cytgl_ext` target: the mode
  *  changes the *layout* of `cgl_phys2d_t` (pointers instead of the three embedded
  *  arrays), so it has to be compiled as its own translation unit with
  *  `-DCGL_PHYS2D_USER_STORAGE=1` and linked against a `cgl_phys2d.c` built the same way.
  *  Mixing the two layouts in one binary would be undefined behaviour, which is
  *  exactly why the test that exercises the mode owns its own copy of the source.
  *
  *  What it pins down, in the order the phase asks for it:
  *
  *    1. the documented storage arithmetic (`CGL_PHYS2D_STORAGE_BYTES`) matches the
  *       struct a caller would actually declare, byte for byte;
  *    2. a world runs -- integrate, collide, cull -- with a pool **smaller than
  *       `CGL_PHYS2D_MAX_BODIES`** (8 bodies against the embedded 528);
  *    3. `cgl_phys2d_add()` refuses beyond that capacity instead of writing past it,
  *       and says so with -1;
  *    4. rejected storage leaves an *inert* world (no null dereference) rather
  *       than a live-looking one;
  *    5. two worlds on two storages are independent.
  *
  *  It is also the minimal worked example of the mode: everything between here and
  *  "the world steps" is the four lines a caller has to write.
  ******************************************************************************
  */
#include <stdio.h>
#include <stdbool.h>

#include <ext/cgl_phys2d.h>

#define SMALL_CAP 8

static int s_fails = 0;

static void check(bool ok, const char *what)
{
    printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok)
    {
        s_fails++;
    }
}

/* --- the caller's storage: three plain arrays, no heap, no framework --------- */

static struct
{
    cgl_phys2d_body_t body[SMALL_CAP];
    int16_t next[SMALL_CAP];
    int16_t cell_head[CGL_PHYS2D_GRID_CELLS];
} s_store;

/* The arithmetic the header documents has to agree with what a caller naturally
   writes -- if these ever diverge, "size it with CGL_PHYS2D_STORAGE_BYTES" is a lie. */
_Static_assert(sizeof(s_store.body) == (SMALL_CAP * sizeof(cgl_phys2d_body_t)),
               "CGL_PHYS2D_STORAGE_BYTES: wrong body element size");
_Static_assert(sizeof(s_store) == CGL_PHYS2D_STORAGE_BYTES(SMALL_CAP),
               "CGL_PHYS2D_STORAGE_BYTES does not match the storage a caller declares");

int main(void)
{
    cgl_phys2d_t world;
    cgl_phys2d_storage_t st;
    int32_t y_before;
    int i;

    printf("=== cgl_phys2d on caller-provided storage (CGL_PHYS2D_USER_STORAGE=1) ===\n");
    printf("pool %d bodies (embedded form would be %d), grid %d cells, sizeof(cgl_phys2d_t) %u B\n",
           SMALL_CAP, CGL_PHYS2D_MAX_BODIES, CGL_PHYS2D_GRID_CELLS, (unsigned)sizeof(cgl_phys2d_t));
    printf("storage: %u B = %u B bodies + %u B next + %u B cell head\n",
           (unsigned)sizeof(s_store),
           (unsigned)sizeof(s_store.body), (unsigned)sizeof(s_store.next),
           (unsigned)sizeof(s_store.cell_head));

    check(sizeof(cgl_phys2d_t) < CGL_PHYS2D_STORAGE_BYTES(SMALL_CAP),
          "with caller storage the world itself is smaller than the memory it runs on");

    cgl_phys2d_storage(&st, s_store.body, s_store.next, s_store.cell_head, SMALL_CAP);
    check(cgl_phys2d_init_storage(&world, &st), "the world bound to the caller's storage");
    check(cgl_phys2d_capacity(&world) == SMALL_CAP, "the runtime capacity is the one the caller declared");

    /* --- 2) fill it exactly, and 3) refuse the one past the end -------------- */
    cgl_phys2d_reset(&world);
    for (i = 0; i < SMALL_CAP; i++)
    {
        /* A row of touching-free dots, 6 px apart, so the world is not empty and
           nothing depends on a collision to be interesting. */
        const int rc = cgl_phys2d_add(&world,
                                  (int16_t)((60 + (i * 6)) * CGL_PHYS2D_ONE),
                                  (int16_t)(100 * CGL_PHYS2D_ONE),
                                  0, (int16_t)(2 * CGL_PHYS2D_ONE),
                                  (int16_t)(2 * CGL_PHYS2D_ONE), 0u);
        if (rc != i)
        {
            printf("FAIL: cgl_phys2d_add() returned %d for body %d\n", rc, i);
            s_fails++;
        }
    }
    check(world.count == SMALL_CAP, "the pool filled to exactly its capacity");
    check(cgl_phys2d_add(&world, 0, 0, 0, 0, (int16_t)CGL_PHYS2D_ONE, 0u) == -1,
          "cgl_phys2d_add() refuses the body past the capacity and returns -1");
    check(world.count == SMALL_CAP, "the refused body did not change the count");

    /* --- 2) it really simulates -------------------------------------------- */
    y_before = world.body[0].y;
    for (i = 0; i < 30; i++)
    {
        cgl_phys2d_step(&world);
    }
    check(world.step == 30u, "30 steps ran on the caller's storage");
    check(world.body[0].y == (int16_t)(y_before + (30 * 2 * CGL_PHYS2D_ONE)),
          "a body integrated exactly as it does in the embedded form (30 steps x 2 px)");
    check(world.active == SMALL_CAP, "every body is still inside the domain and active");

    /* --- 4) rejected storage is inert, not dangerous ------------------------ */
    {
        cgl_phys2d_t bad;
        cgl_phys2d_storage_t bad_st;

        cgl_phys2d_storage(&bad_st, NULL, s_store.next, s_store.cell_head, SMALL_CAP);
        check(!cgl_phys2d_init_storage(&bad, &bad_st), "storage with a null array is rejected");
        check(cgl_phys2d_add(&bad, 0, 0, 0, 0, (int16_t)CGL_PHYS2D_ONE, 0u) == -1,
              "an unbound world accepts no bodies");
        cgl_phys2d_step(&bad); /* must not dereference anything */
        check(bad.step == 0u, "stepping an unbound world is a no-op instead of a crash");

        cgl_phys2d_storage(&bad_st, s_store.body, s_store.next, s_store.cell_head, 0u);
        check(!cgl_phys2d_init_storage(&bad, &bad_st), "a zero-capacity storage is rejected");
    }

    /* --- 5) one storage per world ------------------------------------------ */
    {
        static struct
        {
            cgl_phys2d_body_t body[SMALL_CAP];
            int16_t next[SMALL_CAP];
            int16_t cell_head[CGL_PHYS2D_GRID_CELLS];
        } s_other;
        cgl_phys2d_t other;
        cgl_phys2d_storage_t other_st;

        cgl_phys2d_storage(&other_st, s_other.body, s_other.next, s_other.cell_head, SMALL_CAP);
        check(cgl_phys2d_init_storage(&other, &other_st), "a second world bound to its own storage");
        (void)cgl_phys2d_add(&other, (int16_t)(100 * CGL_PHYS2D_ONE), (int16_t)(100 * CGL_PHYS2D_ONE),
                         0, 0, (int16_t)CGL_PHYS2D_ONE, 0u);
        check((other.count == 1u) && (world.count == SMALL_CAP),
              "the second world starts empty: the two do not share a pool");
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
