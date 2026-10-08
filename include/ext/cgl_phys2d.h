// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_phys2d.h
  * @brief   Minimal fixed point 2D circle physics world.
  *
  *  Small engine sized for a 32 KB MCU. The dot matrix logo animation only
  *  needs a very narrow slice of a general physics engine, so this module
  *  implements exactly that slice instead of pulling in a full library (whose
  *  per body / per shape bookkeeping alone would not fit in RAM):
  *
  *    - circular shapes,
  *    - infinite moment of inertia (bodies never rotate),
  *    - identical mass for the dots and one infinitely heavy "wrecking ball",
  *    - zero gravity and zero friction, zero restitution.
  *
  *  That reduces to pure translational circle dynamics, which is what this
  *  module implements: uniform grid broad phase, circle/circle narrow phase
  *  with inelastic impulses, and infinitely heavy kinematic bodies.
  *
  *  Everything is Q12.4 fixed point (1 unit = 1/16 pixel) so the inner loops
  *  are integer only - no libm, no floating point context save.
  *
  *  2.5D vertical axis (z)
  *  ---------------------
  *  Each body also carries a height `z` and a vertical velocity `vz`. They are
  *  integrated with gravity and land on the plane z = 0, but they **never take
  *  part in collision**: the solver stays a pure 2D circle solver in the XY
  *  plane. Two reasons:
  *
  *    - correctness of the metaphor: XY is a *floor*, so debris flying overhead
  *      still knocks into whatever it passes over. A full 3D solver would make
  *      the wrecking ball miss dots it visually ploughs through.
  *    - cost: the broad phase grid, the impulse solve and their tuning all stay
  *      exactly as they are, so the smash behaviour is unchanged and testable.
  *
  *  The vertical axis is what makes the particles read as 3D: cgl_iso2d.h projects
  *  it into the screen y, and debris that skips across the floor is produced by
  *  the launch rule at the end of cgl_phys2d_step() (plane speed > a threshold ->
  *  it takes off, falls back, bounces, takes off again while it is still fast).
  ******************************************************************************
  */
#ifndef CGL_PHYS2D_H
#define CGL_PHYS2D_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Every tunable that depends on the panel size, the memory budget or the feel of
 * the simulation lives in `cgl_phys2d_config.h`, as an overridable `#ifndef` default
 * (see the header of that file). What is left here is only what is genuinely
 * **structural**: the fixed point format, the body flags and the derived
 * geometry.
 */
#include <ext/cgl_phys2d_config.h>
#include <ext/cgl_ext_basis.h>

/* The fixed point format itself is the module's, not this layer's: it lives in
   `cgl_ext_basis.h` because `cgl_iso2d` speaks the same Q12.4 and must be able to use it
   without including anything named after the physics layer. These two are the
   historical spelling of it, kept because the application and the tests are
   written in terms of `CGL_PHYS2D_ONE` ("one pixel, in the module's fixed point"). */
#define CGL_PHYS2D_SHIFT CGL_EXT_Q4_SHIFT /**< fixed point fraction bits (1/16 px)    */
#define CGL_PHYS2D_ONE CGL_EXT_Q4_ONE

/** Body never receives impulses (infinite mass), but still moves. */
#define CGL_PHYS2D_FLAG_KINEMATIC 0x0001u
/** Body takes part in the simulation. */
#define CGL_PHYS2D_FLAG_ACTIVE 0x0002u
/**
 * Render this body as a lit sphere with a ground shadow instead of a flat dot.
 *
 * Set on the wrecking balls. The renderer cannot tell a ball from a dot by
 * geometry alone (both are circles) and this keeps that decision in the data
 * rather than in a radius comparison.
 */
#define CGL_PHYS2D_FLAG_SPHERE 0x0004u

/* The magnitude knobs of the vertical (2.5D) axis -- gravity, launch gain,
   landing restitution, velocity ceiling -- are `CGL_PHYS2D_*` macros in
   `cgl_phys2d_config.h`; the derivation of each is written there, next to the value,
   so that "why 384 and not 256" can never drift away from the number. */

/* -------------------------------------------------------------------------- */
/* Derived geometry -- do not override these directly, override the knobs in   */
/* cgl_phys2d_config.h instead (they are all simple functions of those).           */
/* -------------------------------------------------------------------------- */

/* `CGL_PHYS2D_WORLD_X0/Y0/W/H` come from cgl_phys2d_config.h. At the reference geometry
   (320x172 panel, domain origin -40, size 400 x 256) that is exactly the
   documented world x -40..360, y -40..216. All four are plain literal copies of
   the knobs -- do not "simplify" the origin into a centred derivation, see the
   WARNING in cgl_phys2d_config.h. */

#define CGL_PHYS2D_GRID_W (CGL_PHYS2D_WORLD_W >> CGL_PHYS2D_CELL_SHIFT)
#define CGL_PHYS2D_GRID_H (CGL_PHYS2D_WORLD_H >> CGL_PHYS2D_CELL_SHIFT)
#define CGL_PHYS2D_GRID_CELLS (CGL_PHYS2D_GRID_W * CGL_PHYS2D_GRID_H)

/* -------------------------------------------------------------------------- */
/* Consistency gates                                                          */
/*                                                                            */
/* Not decoration. The reference geometry is the contract: the physics tuning   */
/* in `cgl_phys2d.c` (the 5-neighbour broad phase scan, the "only neighbours are    */
/* ever offered" assumption in `cgl_phys2d__resolve`) was measured against a 50x32  */
/* grid on a 400x256 world. A macro that silently changes the grid does not     */
/* fail loudly -- it makes collisions stop happening, and the only symptom is   */
/* "the ball flies straight through the dot matrix". That is exactly what       */
/* happened once while this file was being parameterised, so the invariant is   */
/* asserted here rather than described in a comment.                            */
/* -------------------------------------------------------------------------- */

/* `CGL_PHYS2D_CELL_SHIFT` is the tunable and `CGL_PHYS2D_CELL_PX` is derived from it, so
   what needs guarding is the range, not a round trip: a shift outside 0..8 either
   collapses the grid or makes one cell larger than the whole domain. The reference
   value (8 px = shift 7) is pinned separately below, because the broad phase scan
   and the contact tuning in `cgl_phys2d.c` were measured against it. */
_Static_assert(CGL_PHYS2D_CELL_SHIFT >= 0 && CGL_PHYS2D_CELL_SHIFT <= 8,
               "CGL_PHYS2D_CELL_SHIFT must be 0..8 (one grid cell of 1..256 px)");

/* The pool is indexed by `int16_t` (`next[]`, and the index `cgl_phys2d_add()`
   returns) and counted by `uint16_t`, so it is the *index type* that decides the
   bound: 32767, not 65535. The looser counter bound would let a pool through that
   wraps to a negative chain link -- a broad phase list that walks out of the pool.
   With caller storage the equivalent bound is checked at run time by
   `cgl_phys2d_init_storage()`, because the capacity is not a compile time constant
   there. */
_Static_assert(CGL_PHYS2D_MAX_BODIES > 0 && CGL_PHYS2D_MAX_BODIES <= 32767,
               "CGL_PHYS2D_MAX_BODIES must be 1..32767: the pool indices are int16_t");

/* The Q12.4 world coordinates live in int16_t, so the domain must fit -- signed. */
_Static_assert((CGL_PHYS2D_WORLD_X0 + CGL_PHYS2D_WORLD_W) <= 32767,
               "the simulation domain does not fit int16_t Q12.4 coordinates: lower CGL_PHYS2D_WORLD_W_PX / CGL_PHYS2D_WORLD_X0_PX");
_Static_assert((CGL_PHYS2D_WORLD_Y0 + CGL_PHYS2D_WORLD_H) <= 32767,
               "the simulation domain does not fit int16_t Q12.4 coordinates: lower CGL_PHYS2D_WORLD_H_PX / CGL_PHYS2D_WORLD_Y0_PX");

_Static_assert(CGL_PHYS2D_GRID_W > 0, "CGL_PHYS2D_CELL_SHIFT is larger than the simulation domain");
_Static_assert(CGL_PHYS2D_GRID_H > 0, "CGL_PHYS2D_CELL_SHIFT is larger than the simulation domain");

/* The reference geometry, pinned. If a panel retarget or a knob change moves any
   of these, the physics tuning has to be revisited as well -- so the build stops
   here and says so, instead of losing collisions at runtime. Overriding the world
   size on purpose therefore means re-deriving these gates too, which is the
   point: the change has to be conscious. */
_Static_assert(CGL_PHYS2D_CELL_Q4 == (8 * CGL_PHYS2D_ONE),
               "the reference geometry uses 8 px broad phase cells; change this gate together with the physics tuning");
_Static_assert(CGL_PHYS2D_WORLD_W == (400 * CGL_PHYS2D_ONE),
               "the reference world is 400 px wide; retuning the physics is required for another width");
_Static_assert(CGL_PHYS2D_WORLD_H == (256 * CGL_PHYS2D_ONE),
               "the reference world is 256 px high; retuning the physics is required for another height");
_Static_assert(CGL_PHYS2D_GRID_W == 50, "the reference broad phase grid is 50 x 32 cells");
_Static_assert(CGL_PHYS2D_GRID_H == 32, "the reference broad phase grid is 50 x 32 cells");

typedef struct
{
    int16_t x;     /**< position, Q12.4                                      */
    int16_t y;     /**< position, Q12.4                                      */
    int16_t vx;    /**< velocity, Q12.4 per step                             */
    int16_t vy;    /**< velocity, Q12.4 per step                             */
    int16_t r;     /**< collision radius, Q12.4                              */
    int16_t z;     /**< height above the floor plane, Q12.4 (2.5D, never <0) */
    int16_t vz;    /**< vertical velocity, Q12.4 per step                    */
    uint16_t flags;
} cgl_phys2d_body_t;

/**
 * Sizing of the world, for the reference geometry (320x172 panel, 40 px margin,
 * 8 px cells, CGL_PHYS2D_MAX_BODIES = 528). Every entry is a product of the knobs in
 * `cgl_phys2d_config.h`; change a knob there and recompute with this table rather
 * than editing the numbers here.
 *
 *   528  * 16 B (bodies)    =  8448 B
 *   528  *  2 B (grid next) =  1056 B
 *   1600 *  2 B (cell head) =  3200 B        (50 x 32 cells)
 *                    total  ~ 12.4 KB  (was ~14.7 KB before the pool was trimmed)
 *
 * `CGL_PHYS2D_MAX_BODIES` and `CGL_PHYS2D_CELL_PX` themselves are documented in
 * `cgl_phys2d_config.h` -- that is also where the "do not trim the pool below ~500"
 * warning lives, because it is a property of the knob, not of this struct.
 *
 * Where the storage lives
 * ----------------------
 * By default it is **inside the struct**, as above, and the pool size is the
 * compile time `CGL_PHYS2D_MAX_BODIES`. A consumer that would rather place the
 * storage itself -- and choose the pool size at run time -- builds with
 * `-DCGL_PHYS2D_USER_STORAGE=1`; `cgl_phys2d_t` then holds pointers instead of the three
 * arrays and is bound to caller memory with `cgl_phys2d_init_storage()`. The two
 * forms are a compile time choice, because a world must have exactly one layout:
 * a translation unit that sees a *different* `CGL_PHYS2D_USER_STORAGE` value than the
 * one that owns the world would read the struct wrongly. It is a build-wide knob,
 * like `CGL_PHYS2D_MAX_BODIES`.
 */

#ifndef CGL_PHYS2D_USER_STORAGE
#define CGL_PHYS2D_USER_STORAGE 0
#endif

#if CGL_PHYS2D_USER_STORAGE
/**
 * The world on **caller-provided storage**: the pool size is chosen at run time.
 *
 * Nothing is allocated here -- no heap, now or ever (see the module's non-goals).
 * The caller declares the arrays (statically or on its own stack) and hands them
 * over once. `cgl_phys2d_capacity()` reports the resulting pool size and every
 * function works exactly as in the embedded form.
 */
typedef struct
{
    cgl_phys2d_body_t *body;   /**< `capacity` entries -- body pool                */
    int16_t *next;         /**< `capacity` entries -- broad phase chain links  */
    int16_t *cell_head;    /**< `CGL_PHYS2D_GRID_CELLS` entries -- one per cell    */
    uint16_t capacity;     /**< bodies the storage can hold, 1..32767          */
} cgl_phys2d_storage_t;

/**
 * Bytes a caller has to provide for `cap` bodies, **as one expression** so that
 * the three element counts and their sizes can never be guessed wrong:
 *
 *     static struct {
 *         cgl_phys2d_body_t body[CAP];
 *         int16_t       next[CAP];
 *         int16_t       cell_head[CGL_PHYS2D_GRID_CELLS];
 *     } s_store;
 *     _Static_assert(sizeof(s_store) == CGL_PHYS2D_STORAGE_BYTES(CAP), "...");
 *
 * The three parts ("body" x 16 B, "next" x 2 B, "cell_head" x 2 B) are exactly
 * the table at the top of the `cgl_phys2d_t` comment. The caller is free to split
 * them across three separate arrays instead; what it may not do is give fewer
 * entries than it declares as `capacity`, or fewer than `CGL_PHYS2D_GRID_CELLS`
 * cells.
 */
#define CGL_PHYS2D_STORAGE_BYTES(cap) \
    ((uint32_t)(cap) * (uint32_t)sizeof(cgl_phys2d_body_t) + \
     (uint32_t)(cap) * (uint32_t)sizeof(int16_t) + \
     (uint32_t)CGL_PHYS2D_GRID_CELLS * (uint32_t)sizeof(int16_t))

#endif /* CGL_PHYS2D_USER_STORAGE */

typedef struct
{
#if CGL_PHYS2D_USER_STORAGE
    cgl_phys2d_body_t *body;     /**< caller storage, `capacity` entries            */
    int16_t *next;           /**< caller storage, `capacity` entries            */
    int16_t *cell_head;      /**< caller storage, CGL_PHYS2D_GRID_CELLS entries     */
    uint16_t capacity;       /**< pool size, chosen by the caller               */
#else
    cgl_phys2d_body_t body[CGL_PHYS2D_MAX_BODIES];
    int16_t next[CGL_PHYS2D_MAX_BODIES];
    int16_t cell_head[CGL_PHYS2D_GRID_CELLS];
#endif
    uint16_t count;  /**< bodies ever added, 0..cgl_phys2d_capacity(w)          */
    uint16_t active; /**< bodies still inside the simulation domain         */
    uint16_t active_dynamic; /**< active bodies that are not kinematic      */
    uint32_t step;   /**< number of steps simulated                         */
} cgl_phys2d_t;

/**
 * Size of the body pool: the run time `capacity` with caller storage, the compile
 * time `CGL_PHYS2D_MAX_BODIES` otherwise.
 *
 * This is a function and not a struct member on purpose: in the embedded form the
 * member would be a constant stored in RAM, and `sizeof(cgl_phys2d_t)` is part of the
 * module's published cost. `CGL_EXT_INLINE` + a literal folds to the same constant the
 * old `w->count >= CGL_PHYS2D_MAX_BODIES` comparison used, so the embedded form
 * compiles to exactly the same code as before this phase.
 */
CGL_EXT_INLINE uint16_t cgl_phys2d_capacity(const cgl_phys2d_t *w)
{
#if CGL_PHYS2D_USER_STORAGE
    return w->capacity;
#else
    (void)w;
    return (uint16_t)CGL_PHYS2D_MAX_BODIES;
#endif
}

#if CGL_PHYS2D_USER_STORAGE
/**
 * Pack a caller-declared storage block into the descriptor `cgl_phys2d_init_storage()`
 * wants. `body`/`next`/`cell_head` must have `cap` / `cap` / `CGL_PHYS2D_GRID_CELLS`
 * entries; see `CGL_PHYS2D_STORAGE_BYTES()`.
 */
CGL_EXT_INLINE void cgl_phys2d_storage(cgl_phys2d_storage_t *st, cgl_phys2d_body_t *body,
                               int16_t *next, int16_t *cell_head, uint16_t cap)
{
    st->body = body;
    st->next = next;
    st->cell_head = cell_head;
    st->capacity = cap;
}

/**
 * Bind a world to caller storage: the only way to get a usable world in this
 * mode (there are no embedded arrays to fall back on).
 *
 * Returns false, and leaves the world unusable so that a later `cgl_phys2d_step()` on
 * it does nothing, when the storage cannot hold a world: a null array, a zero
 * capacity, or a capacity beyond what the `int16_t` pool indices can address.
 *
 * **The storage has to outlive the world**, and one storage belongs to exactly one
 * world: the world holds pointers into it, and two worlds sharing one storage
 * would corrupt each other silently.
 */
bool cgl_phys2d_init_storage(cgl_phys2d_t *w, const cgl_phys2d_storage_t *st);

#endif /* CGL_PHYS2D_USER_STORAGE */

/** Reset the world to empty (keeps the storage). */
void cgl_phys2d_reset(cgl_phys2d_t *w);

/** Add a body, returns its index or -1 when the pool is full. */
int cgl_phys2d_add(cgl_phys2d_t *w, int16_t x, int16_t y, int16_t vx, int16_t vy,
               int16_t r, uint16_t flags);

/** Integrate one fixed step (1/60 s) and resolve all contacts. */
void cgl_phys2d_step(cgl_phys2d_t *w);

/** Convert a fixed point value to pixels (truncating).
    CGL_EXT_INLINE: the renderer calls this twice per body per band, and a call
    per conversion measurably costs more than the shift itself. The macro (and
    what it degrades to on an unfamiliar compiler) is documented in
    `cgl_ext_basis.h`. */
CGL_EXT_INLINE int16_t cgl_phys2d_to_px(int16_t q)
{
    return (int16_t)(q >> CGL_PHYS2D_SHIFT);
}

#endif /* CGL_PHYS2D_H */
