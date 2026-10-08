// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_bounce.c
  * @brief   cgl_bounce - gravity, four walls, restitution, self-healing.
  *
  *  The whole effect is three pieces of arithmetic and one invariant:
  *
  *      pre_step    vy += g                       (semi-implicit Euler)
  *      <caller>    cgl_phys2d_step()             (the ONE integration)
  *      post_step   clamp + reflect + count       (the engine has no walls)
  *
  *  and the invariant is "there is a sphere of `CGL_BOUNCE_R_Q4` in the pool that
  *  belongs to this struct". `cgl_bounce__ensure()` re-establishes it on **every**
  *  entry point, because the pool is not ours: `cgl_phys2d_reset()` zeroes `count`
  *  without freeing anything, so an index that was valid a moment ago can point at
  *  somebody else's body (or past the end) at any time. See the note on
  *  `cgl_bounce__owned()` for why the test is by identity and not by index.
  ******************************************************************************
  */
#include <ext/cgl_bounce.h>

#if CGL_CFG_BOUNCE

/* -------------------------------------------------------------------------- */
/* The body: is the one at `index` still ours?                                */
/* -------------------------------------------------------------------------- */

/**
 * Is `world->body[index]` the sphere this struct put there?
 *
 * The original application checked `< world->count` and nothing else, which is
 * correct only by the accident of *when* the ball was added: the reference scene
 * rebuilds its 480 dots first and adds the ball last, so after a reset the ball's
 * old index (481) is past the new `count` (480) and the staleness is caught. A
 * consumer with ten dots and the ball added fifth gets the other answer -- index 5
 * is in range and now holds a dot, and the ball silently becomes that dot: its
 * walls teleport it, and the dot is dragged around the screen.
 *
 * The fix is to ask about the body, not about the index. `r` and the sphere flag are
 * both written by this module and by nothing else (the debris dots carry their own
 * radius and no sphere flag), so a slot that fails either test was not, or is no
 * longer, ours -- and re-adding is the right answer in both cases. It costs two
 * comparisons and no extra state.
 *
 * LIMITATION, stated rather than hidden: two `cgl_bounce_t` in one world are
 * indistinguishable by this test, so a reset can leave them sharing a slot. One
 * gravity ball per world is the supported configuration (it is also the only one the
 * reference scene has); two of them want two worlds.
 */
static bool cgl_bounce__owned(const cgl_bounce_t *b, uint16_t index)
{
    const cgl_phys2d_body_t *p;

    if ((b->world == NULL) || (index >= b->world->count))
    {
        return false;
    }
    p = &b->world->body[index];
    return (p->r == (int16_t)CGL_BOUNCE_R_Q4) &&
           ((p->flags & CGL_PHYS2D_FLAG_SPHERE) != 0u);
}

/* -------------------------------------------------------------------------- */
/* The active area                                                            */
/* -------------------------------------------------------------------------- */

/**
 * Validate an active area and convert it to Q12.4.
 *
 * Two things are rejected, and both of them are "the caller asked for something that
 * cannot work", not "the caller asked for something unusual":
 *
 *   - an **inverted or empty** rectangle (`x0 > x1`): there is no position the ball
 *     could ever be inside, so every step would clamp it to one of two crossed walls
 *     and it would buzz in a corner;
 *   - a rectangle that reaches outside the engine's **simulation domain**
 *     (`[WORLD_X0, WORLD_X0+W)` x `[WORLD_Y0, WORLD_Y0+H)`, half-open, exactly as
 *     `cgl_phys2d.c` tests it). A kinematic body is allowed to leave the domain --
 *     the engine keeps integrating it so it can come back -- but while it is out
 *     there it is **not in the broad phase**, so it stops colliding with everything
 *     else. A playground that reaches out there is a ball that silently stops
 *     ploughing through the scenery at the edges, which is precisely the kind of
 *     failure that gets diagnosed as "the physics is flaky".
 *
 * The spawn point is deliberately **not** validated here: it is the caller's business
 * and a ball that starts outside its own playground is clamped back on the first
 * `cgl_bounce_post_step()`.
 */
static bool cgl_bounce__area_q4(const cgl_bounce_rect_t *a, int16_t *left, int16_t *right,
                                int16_t *top, int16_t *bot)
{
    int32_t lq;
    int32_t rq;
    int32_t tq;
    int32_t bq;

    if ((a == NULL) || (a->x0 > a->x1) || (a->y0 > a->y1))
    {
        return false;
    }

    lq = (int32_t)a->x0 * CGL_EXT_Q4_ONE;
    rq = (int32_t)a->x1 * CGL_EXT_Q4_ONE;
    tq = (int32_t)a->y0 * CGL_EXT_Q4_ONE;
    bq = (int32_t)a->y1 * CGL_EXT_Q4_ONE;

    if ((lq < CGL_PHYS2D_WORLD_X0) || (rq >= (CGL_PHYS2D_WORLD_X0 + CGL_PHYS2D_WORLD_W)))
    {
        return false;
    }
    if ((tq < CGL_PHYS2D_WORLD_Y0) || (bq >= (CGL_PHYS2D_WORLD_Y0 + CGL_PHYS2D_WORLD_H)))
    {
        return false;
    }

    *left = (int16_t)lq;
    *right = (int16_t)rq;
    *top = (int16_t)tq;
    *bot = (int16_t)bq;
    return true;
}

bool cgl_bounce_set_area(cgl_bounce_t *b, cgl_bounce_rect_t area)
{
    int16_t lq;
    int16_t rq;
    int16_t tq;
    int16_t bq;

    if (b == NULL)
    {
        return false;
    }
    if (!cgl_bounce__area_q4(&area, &lq, &rq, &tq, &bq))
    {
        b->area_rejected = (uint16_t)(b->area_rejected + 1u);
        return false; /* the previous area stays in force -- a rejected resize must
                         not leave the ball with no walls at all */
    }

    b->left_q4 = lq;
    b->right_q4 = rq;
    b->top_q4 = tq;
    b->bot_q4 = bq;
    b->has_area = 1u;

    if (b->on_change != NULL)
    {
        b->on_change(b->on_change_user);
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* Throw / reset                                                              */
/* -------------------------------------------------------------------------- */

/**
 * Put the ball back at its spawn point, adding it to the pool if it is not there.
 *
 * Two cases, and the difference between them is the whole reason this function
 * exists rather than a `memcpy` of the spawn state:
 *
 *   A. the sphere at `index` is **ours** -- the normal frame, thousands of times a
 *      run. Only the state is rewritten.
 *   B. it is not (a `cgl_phys2d_reset()` wiped it, or somebody else's body took the
 *      slot) -- the ball is **appended** to the pool, and `index` is updated to
 *      wherever it landed. Never overwrite the body that is there: that is how a
 *      gravity ball eats a debris dot.
 */
static void cgl_bounce__throw(cgl_bounce_t *b, bool reset_counter)
{
    cgl_phys2d_body_t *p;

    if (b->world == NULL)
    {
        return;
    }
    if (reset_counter)
    {
        b->bounces = 0u;
    }

    if (cgl_bounce__owned(b, b->index))
    {
        p = &b->world->body[b->index];
    }
    else
    {
        int idx = cgl_phys2d_add(b->world, b->start_x_q4, b->start_y_q4, b->start_vx_q4,
                                 b->start_vy_q4, (int16_t)CGL_BOUNCE_R_Q4,
                                 (uint16_t)(CGL_PHYS2D_FLAG_KINEMATIC | CGL_PHYS2D_FLAG_SPHERE));

        b->index = (idx < 0) ? (uint16_t)CGL_BOUNCE_NO_INDEX : (uint16_t)idx;
        return; /* cgl_phys2d_add() already wrote the state */
    }

    p->x = b->start_x_q4;
    p->y = b->start_y_q4;
    p->vx = b->start_vx_q4;
    p->vy = b->start_vy_q4;
    p->r = (int16_t)CGL_BOUNCE_R_Q4;
    p->flags = (uint16_t)(CGL_PHYS2D_FLAG_KINEMATIC | CGL_PHYS2D_FLAG_ACTIVE |
                          CGL_PHYS2D_FLAG_SPHERE);
}

/**
 * Make sure the ball is in the pool, and put it back if it is not.
 * @return false only when there is no usable world or the pool is exhausted.
 */
static bool cgl_bounce__ensure(cgl_bounce_t *b)
{
    if ((b == NULL) || (b->world == NULL) || (b->has_area == 0u))
    {
        return false;
    }
    if (!cgl_bounce__owned(b, b->index))
    {
        cgl_bounce__throw(b, false);
    }
    return cgl_bounce__owned(b, b->index);
}

/* -------------------------------------------------------------------------- */
/* The two halves of a step                                                   */
/* -------------------------------------------------------------------------- */

void cgl_bounce_pre_step(cgl_bounce_t *b)
{
    cgl_phys2d_body_t *p;

    if (!cgl_bounce__ensure(b))
    {
        return;
    }
    p = &b->world->body[b->index];

    /* Added before the integration, never after: that is what makes the integrator
       semi-implicit Euler. With the addition after the step the ball climbs a little
       on every arc (explicit Euler is energy-positive at the apex). */
    p->vy = (int16_t)(p->vy + (int16_t)CGL_BOUNCE_GRAVITY_Q4);
}

/**
 * Reflect and damp one velocity component: `-(v * CGL_BOUNCE_RESTITUTION_Q8) >> 8`,
 * clamped to the engine's velocity ceiling.
 *
 * The clamp is not decoration. The reflection is the only thing that bounds the
 * ball's speed, and it runs once per wall *hit* -- so a step large enough to carry the
 * ball through a corner can reflect the same component twice, and the Q8 multiply is
 * on the way up in that moment. Without the clamp a pathological step (a huge
 * `CGL_BOUNCE_GRAVITY_Q4`) saturates the `int16_t` velocity into a sign flip, which
 * looks like the ball teleporting across the screen.
 */
static int16_t cgl_bounce__reflect(int16_t v)
{
    int32_t m = -((int32_t)v * (int32_t)CGL_BOUNCE_RESTITUTION_Q8);

    m >>= 8;
    if (m > CGL_PHYS2D_MAX_VELOCITY)
    {
        m = CGL_PHYS2D_MAX_VELOCITY;
    }
    if (m < -CGL_PHYS2D_MAX_VELOCITY)
    {
        m = -CGL_PHYS2D_MAX_VELOCITY;
    }
    return (int16_t)m;
}

void cgl_bounce_post_step(cgl_bounce_t *b)
{
    cgl_phys2d_body_t *p;
    int32_t ax;
    int32_t ay;
    int32_t m;

    if (!cgl_bounce__ensure(b))
    {
        return;
    }
    p = &b->world->body[b->index];

    /* Position AND velocity, always together. Reflecting the velocity alone leaves
       the ball out of range for one step, and "bouncing" from a position it never
       occupied reads as a stutter -- the wall appears to push it. */
    if (p->y > b->bot_q4)
    {
        p->y = b->bot_q4;
        p->vy = cgl_bounce__reflect(p->vy);
        b->bounces = (uint16_t)(b->bounces + 1u);
    }
    else if (p->y < b->top_q4)
    {
        p->y = b->top_q4;
        p->vy = cgl_bounce__reflect(p->vy);
        b->bounces = (uint16_t)(b->bounces + 1u);
    }

    if (p->x > b->right_q4)
    {
        p->x = b->right_q4;
        p->vx = cgl_bounce__reflect(p->vx);
        b->bounces = (uint16_t)(b->bounces + 1u);
    }
    else if (p->x < b->left_q4)
    {
        p->x = b->left_q4;
        p->vx = cgl_bounce__reflect(p->vx);
        b->bounces = (uint16_t)(b->bounces + 1u);
    }

    /* Observation: the historical maximum speed. A test asserts on it ("the fixed
       point never blew up"), which is the only way to catch an integrator that is
       quietly gaining speed between two wall hits. */
    ax = (p->vx < 0) ? -(int32_t)p->vx : (int32_t)p->vx;
    ay = (p->vy < 0) ? -(int32_t)p->vy : (int32_t)p->vy;
    m = (ax > ay) ? ax : ay;
    if (m > (int32_t)b->max_speed_q4)
    {
        b->max_speed_q4 = (uint16_t)m;
    }

    /* The position changes every step, so this is a bare "it moved" notification and
       not a change test: the ball is the one widget whose pixels are never the same
       twice. */
    if (b->on_change != NULL)
    {
        b->on_change(b->on_change_user);
    }
}

/* -------------------------------------------------------------------------- */
/* Position readout                                                           */
/* -------------------------------------------------------------------------- */

/**
 * Clamp a body's centre back into the active area (Q12.4).
 *
 * Called from the read path, not from the step: see the note on
 * `cgl_bounce_position_px()`. It is the same arithmetic as the walls in `post_step`,
 * but with no reflection and no counting -- it restores the **invariant** ("the
 * position you are shown is legal") without pretending to have resolved a collision
 * that the wall pass may already have handled.
 */
static void cgl_bounce__clamp(const cgl_bounce_t *b, cgl_phys2d_body_t *p)
{
    if (p->y > b->bot_q4)
    {
        p->y = b->bot_q4;
    }
    else if (p->y < b->top_q4)
    {
        p->y = b->top_q4;
    }
    if (p->x > b->right_q4)
    {
        p->x = b->right_q4;
    }
    else if (p->x < b->left_q4)
    {
        p->x = b->left_q4;
    }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

bool cgl_bounce_init(cgl_bounce_t *b, cgl_phys2d_t *world, cgl_bounce_rect_t area)
{
    if (b == NULL)
    {
        return false;
    }

    b->world = world;
    b->index = (uint16_t)CGL_BOUNCE_NO_INDEX;
    b->bounces = 0u;
    b->max_speed_q4 = 0u;
    b->area_rejected = 0u;
    b->has_area = 0u;
    b->on_change = NULL;
    b->on_change_user = NULL;

    /* Leave sane bounds behind even if the area is about to be rejected, so that a
       later read on an inert ball has something to clamp to instead of reading
       uninitialised members. `has_area` is what keeps those bounds from being taken
       for a real playground -- see its comment in the header. */
    b->left_q4 = 0;
    b->right_q4 = 0;
    b->top_q4 = 0;
    b->bot_q4 = 0;

    /* No world is a programming error, not a bad value: fail without spending the
       area-rejection count on it (see the header). Everything above has already run,
       so the ball is fully initialised and inert. */
    if (world == NULL)
    {
        return false;
    }

    if (!cgl_bounce_set_area(b, area))
    {
        return false; /* area_rejected is already 1; the ball stays inert */
    }

    /* "Top left, tossed to the right and slightly up", expressed relative to the area
       rather than in absolute pixels -- so it survives the caller moving the
       playground. */
    b->start_x_q4 = (int16_t)(b->left_q4 + (CGL_BOUNCE_SPAWN_INSET_PX * CGL_EXT_Q4_ONE));
    b->start_y_q4 = b->top_q4;
    b->start_vx_q4 = (int16_t)CGL_BOUNCE_SPAWN_VX_Q4;
    b->start_vy_q4 = (int16_t)CGL_BOUNCE_SPAWN_VY_Q4;

    cgl_bounce__throw(b, false);
    return true;
}

void cgl_bounce_set_spawn(cgl_bounce_t *b, int16_t x_px, int16_t y_px, int16_t vx_q4, int16_t vy_q4)
{
    if (b == NULL)
    {
        return;
    }
    b->start_x_q4 = (int16_t)(x_px * CGL_EXT_Q4_ONE);
    b->start_y_q4 = (int16_t)(y_px * CGL_EXT_Q4_ONE);
    b->start_vx_q4 = vx_q4;
    b->start_vy_q4 = vy_q4;
    /* Deliberately does not move the ball: this is where the *next* throw starts, and
       a setter that also teleported the ball in flight would be a surprise. */
}

void cgl_bounce_restart(cgl_bounce_t *b)
{
    if (b == NULL)
    {
        return;
    }
    b->max_speed_q4 = 0u;
    cgl_bounce__throw(b, true);
    if (b->on_change != NULL)
    {
        b->on_change(b->on_change_user);
    }
}

bool cgl_bounce_position_px(cgl_bounce_t *b, int16_t *x, int16_t *y)
{
    cgl_phys2d_body_t tmp;

    if (!cgl_bounce__ensure(b))
    {
        return false;
    }
    tmp = b->world->body[b->index];
    cgl_bounce__clamp(b, &tmp);
    if (x != NULL)
    {
        *x = cgl_phys2d_to_px(tmp.x);
    }
    if (y != NULL)
    {
        *y = cgl_phys2d_to_px(tmp.y);
    }
    return true;
}

bool cgl_bounce_draw_position_px(const cgl_bounce_t *b, int16_t *x, int16_t *y)
{
    cgl_phys2d_body_t tmp;

    /* No self-healing here on purpose: see the header. A painter reads the ball or
       draws nothing; it never adds a body to a world it does not own. */
    if ((b == NULL) || (b->has_area == 0u) || (b->world == NULL) ||
        (b->index >= b->world->count))
    {
        return false;
    }
    if ((b->world->body[b->index].flags & CGL_PHYS2D_FLAG_ACTIVE) == 0u)
    {
        return false;
    }

    tmp = b->world->body[b->index];
    cgl_bounce__clamp(b, &tmp);
    if (x != NULL)
    {
        *x = cgl_phys2d_to_px(tmp.x);
    }
    if (y != NULL)
    {
        *y = cgl_phys2d_to_px(tmp.y);
    }
    return true;
}

uint16_t cgl_bounce_bounces(const cgl_bounce_t *b)
{
    return (b == NULL) ? 0u : b->bounces;
}

bool cgl_bounce_area_px(const cgl_bounce_t *b, cgl_bounce_rect_t *area)
{
    if ((b == NULL) || (area == NULL) || (b->has_area == 0u))
    {
        return false;
    }
    area->x0 = cgl_phys2d_to_px(b->left_q4);
    area->y0 = cgl_phys2d_to_px(b->top_q4);
    area->x1 = cgl_phys2d_to_px(b->right_q4);
    area->y1 = cgl_phys2d_to_px(b->bot_q4);
    return true;
}

uint16_t cgl_bounce_max_speed_q4(const cgl_bounce_t *b)
{
    return (b == NULL) ? 0u : b->max_speed_q4;
}

uint16_t cgl_bounce_index(const cgl_bounce_t *b)
{
    return (b == NULL) ? (uint16_t)CGL_BOUNCE_NO_INDEX : b->index;
}

uint16_t cgl_bounce_area_rejected(const cgl_bounce_t *b)
{
    return (b == NULL) ? 0u : b->area_rejected;
}

#endif /* CGL_CFG_BOUNCE */
