// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_phys2d.c
  * @brief   Minimal fixed point 2D circle physics world.
  ******************************************************************************
  */
#include <ext/cgl_phys2d.h>

/* Q8 unity: used for unit vectors and inverse masses. */
#define CGL_PHYS2D_Q8 256

/* -------------------------------------------------------------------------- */
/* helpers                                                                    */
/* -------------------------------------------------------------------------- */

static int16_t cgl_phys2d__clamp(int32_t v)
{
    if (v > CGL_PHYS2D_MAX_VELOCITY)
    {
        return (int16_t)CGL_PHYS2D_MAX_VELOCITY;
    }
    if (v < -CGL_PHYS2D_MAX_VELOCITY)
    {
        return (int16_t)(-CGL_PHYS2D_MAX_VELOCITY);
    }
    return (int16_t)v;
}

/* Integer square root (Newton), only called on actual overlaps. */
static int32_t cgl_phys2d__isqrt(int32_t n)
{
    int32_t x;
    int32_t y;

    if (n <= 0)
    {
        return 0;
    }
    x = n;
    y = (x + 1) / 2;
    while (y < x)
    {
        x = y;
        y = (x + (n / x)) / 2;
    }
    return x;
}

static inline bool cgl_phys2d__inside_world(const cgl_phys2d_body_t *b)
{
    return (b->x >= CGL_PHYS2D_WORLD_X0) && (b->x < (CGL_PHYS2D_WORLD_X0 + CGL_PHYS2D_WORLD_W)) &&
           (b->y >= CGL_PHYS2D_WORLD_Y0) && (b->y < (CGL_PHYS2D_WORLD_Y0 + CGL_PHYS2D_WORLD_H));
}

static inline int32_t cgl_phys2d__cell_index(int16_t x, int16_t y)
{
    int32_t cx = (int32_t)(x - CGL_PHYS2D_WORLD_X0) >> CGL_PHYS2D_CELL_SHIFT;
    int32_t cy = (int32_t)(y - CGL_PHYS2D_WORLD_Y0) >> CGL_PHYS2D_CELL_SHIFT;

    if (cx < 0)
    {
        cx = 0;
    }
    if (cx >= CGL_PHYS2D_GRID_W)
    {
        cx = CGL_PHYS2D_GRID_W - 1;
    }
    if (cy < 0)
    {
        cy = 0;
    }
    if (cy >= CGL_PHYS2D_GRID_H)
    {
        cy = CGL_PHYS2D_GRID_H - 1;
    }
    return (cy * CGL_PHYS2D_GRID_W) + cx;
}

/* -------------------------------------------------------------------------- */
/* narrow phase                                                               */
/* -------------------------------------------------------------------------- */

/**
 * Resolve one circle/circle contact.
 * `n` points from a to b; the impulse is purely inelastic (restitution 0).
 */
static void cgl_phys2d__resolve(cgl_phys2d_body_t *a, cgl_phys2d_body_t *b)
{
    int32_t dx = (int32_t)b->x - (int32_t)a->x;
    int32_t dy = (int32_t)b->y - (int32_t)a->y;
    int32_t rsum = (int32_t)a->r + (int32_t)b->r;
    int32_t d2 = (dx * dx) + (dy * dy);
    int32_t d;
    int32_t nx;
    int32_t ny;
    int32_t vn;
    int32_t inv_a;
    int32_t inv_b;
    int32_t inv_sum;
    int32_t j;
    int32_t overlap;

    if ((dx > 1024) || (dx < -1024) || (dy > 1024) || (dy < -1024))
    {
        return; /* only neighbours are ever offered by the broad phase */
    }
    if (d2 >= (rsum * rsum))
    {
        return;
    }

    inv_a = ((a->flags & CGL_PHYS2D_FLAG_KINEMATIC) != 0u) ? 0 : CGL_PHYS2D_Q8;
    inv_b = ((b->flags & CGL_PHYS2D_FLAG_KINEMATIC) != 0u) ? 0 : CGL_PHYS2D_Q8;
    inv_sum = inv_a + inv_b;

    d = cgl_phys2d__isqrt(d2);
    if (d == 0)
    {
        /* perfectly coincident: pick an arbitrary axis so they separate */
        dx = 1;
        dy = 0;
        d = 1;
    }
    nx = (dx * CGL_PHYS2D_Q8) / d;
    ny = (dy * CGL_PHYS2D_Q8) / d;

    if (inv_sum > 0)
    {
        vn = ((((int32_t)b->vx - (int32_t)a->vx) * nx) +
              (((int32_t)b->vy - (int32_t)a->vy) * ny)) >> 8;

        if (vn < 0)
        {
            j = (-vn * CGL_PHYS2D_Q8) / inv_sum;
            if (inv_a != 0)
            {
                a->vx = cgl_phys2d__clamp((int32_t)a->vx - (((j * inv_a) * nx) >> 16));
                a->vy = cgl_phys2d__clamp((int32_t)a->vy - (((j * inv_a) * ny) >> 16));
            }
            if (inv_b != 0)
            {
                b->vx = cgl_phys2d__clamp((int32_t)b->vx + (((j * inv_b) * nx) >> 16));
                b->vy = cgl_phys2d__clamp((int32_t)b->vy + (((j * inv_b) * ny) >> 16));
            }
        }
    }

    /* positional correction: push the two bodies fully apart, weighted by
       inverse mass (a kinematic body never moves) */
    overlap = rsum - d;
    if (overlap > 0)
    {
        if (inv_sum == 0)
        {
            return;
        }
        {
            int32_t push_a = (overlap * inv_a) / inv_sum;
            int32_t push_b = (overlap * inv_b) / inv_sum;
            a->x = (int16_t)((int32_t)a->x - ((push_a * nx) >> 8));
            a->y = (int16_t)((int32_t)a->y - ((push_a * ny) >> 8));
            b->x = (int16_t)((int32_t)b->x + ((push_b * nx) >> 8));
            b->y = (int16_t)((int32_t)b->y + ((push_b * ny) >> 8));
        }
    }
}

/* -------------------------------------------------------------------------- */
/* broad phase                                                                */
/* -------------------------------------------------------------------------- */

static void cgl_phys2d__integrate(cgl_phys2d_t *w)
{
    uint16_t i;

    for (i = 0u; i < w->count; i++)
    {
        cgl_phys2d_body_t *b = &w->body[i];

        if ((b->flags & CGL_PHYS2D_FLAG_ACTIVE) == 0u)
        {
            continue;
        }
        b->x = (int16_t)((int32_t)b->x + b->vx);
        b->y = (int16_t)((int32_t)b->y + b->vy);

        /* vertical axis (2.5D): ballistic + floor. Never touches the solver. */
        b->vz = (int16_t)((int32_t)b->vz - CGL_PHYS2D_GRAVITY_Z_Q4);
        b->z = (int16_t)((int32_t)b->z + b->vz);
        if (b->z <= 0)
        {
            b->z = 0;
            if (b->vz < -(int32_t)CGL_PHYS2D_LAND_MIN_Q4)
            {
                b->vz = (int16_t)(-(((int32_t)b->vz * (int32_t)CGL_PHYS2D_LAND_REST_Q8) >> 8));
            }
            else
            {
                b->vz = 0; /* soft landing: sit perfectly still, no sub-pixel buzz */
            }
        }
    }
}

/**
 * Turn plane speed into a vertical hop.
 *
 * Runs *after* the contact solve, so it sees the post-impact velocities: a dot
 * the ball just hit is fast, and fast dots take off, arc and skip - which is
 * the whole point of the 2.5D axis. Kinematic bodies (the balls) are excluded:
 * they plough along the floor instead of leaping.
 *
 * Because there is no friction in this world, a body that keeps sliding keeps
 * hopping. That is deliberate: it reads as debris skipping across the floor,
 * and the domain cull removes it soon after anyway.
 */
static void cgl_phys2d__launch(cgl_phys2d_t *w)
{
    uint16_t i;

    for (i = 0u; i < w->count; i++)
    {
        cgl_phys2d_body_t *b = &w->body[i];
        int32_t ax;
        int32_t ay;
        int32_t speed;
        int32_t vz;

        if ((b->flags & (CGL_PHYS2D_FLAG_ACTIVE | CGL_PHYS2D_FLAG_KINEMATIC)) !=
            CGL_PHYS2D_FLAG_ACTIVE)
        {
            continue; /* inactive, or a kinematic ball */
        }
        /*
         * "On the floor right now" is the whole test. It must NOT also require
         * vz <= 0: the landing restitution above deliberately leaves a resting
         * body with a small positive vz (it is what keeps it from buzzing at
         * sub-pixel heights), and requiring vz <= 0 would therefore block the
         * launch forever. Overwriting that residual vz with the launch speed is
         * exactly what should happen.
         */
        if (b->z != 0)
        {
            continue; /* airborne */
        }

        ax = (b->vx < 0) ? -(int32_t)b->vx : (int32_t)b->vx;
        ay = (b->vy < 0) ? -(int32_t)b->vy : (int32_t)b->vy;
        speed = (ax > ay) ? ax : ay;
        if (speed <= (int32_t)CGL_PHYS2D_LAUNCH_MIN_Q4)
        {
            continue;
        }

        vz = ((speed - (int32_t)CGL_PHYS2D_LAUNCH_MIN_Q4) *
              (int32_t)CGL_PHYS2D_LAUNCH_GAIN_Q8) >> 8;
        if (vz > (int32_t)CGL_PHYS2D_MAX_VZ)
        {
            vz = (int32_t)CGL_PHYS2D_MAX_VZ;
        }
        b->vz = (int16_t)vz;
    }
}

static void cgl_phys2d__build_grid(cgl_phys2d_t *w)
{
    uint16_t i;
    int32_t c;

    for (c = 0; c < CGL_PHYS2D_GRID_CELLS; c++)
    {
        w->cell_head[c] = -1;
    }

    w->active = 0u;
    w->active_dynamic = 0u;
    for (i = 0u; i < w->count; i++)
    {
        cgl_phys2d_body_t *b = &w->body[i];

        if ((b->flags & CGL_PHYS2D_FLAG_ACTIVE) == 0u)
        {
            continue;
        }
        if (!cgl_phys2d__inside_world(b))
        {
            if ((b->flags & CGL_PHYS2D_FLAG_KINEMATIC) == 0u)
            {
                b->flags &= (uint16_t)~CGL_PHYS2D_FLAG_ACTIVE;
                b->vx = 0;
                b->vy = 0;
                continue;
            }
            /* kinematic bodies keep running so they can re-enter the world */
            continue;
        }
        c = cgl_phys2d__cell_index(b->x, b->y);
        w->next[i] = w->cell_head[c];
        w->cell_head[c] = (int16_t)i;
        w->active++;
        if ((b->flags & CGL_PHYS2D_FLAG_KINEMATIC) == 0u)
        {
            w->active_dynamic++;
        }
    }
}

/**
 * Forward neighbour offsets.
 *
 * Scanning only the "forward" half of the 3x3 neighbourhood (plus the cell
 * itself) visits every unordered pair of cells exactly once, which halves the
 * inner work compared to scanning all nine cells and rejecting duplicates.
 */
static const int8_t cgl_phys2d__offsets[5][2] = {
    {0, 0}, {1, -1}, {1, 0}, {1, 1}, {0, 1}
};

static void cgl_phys2d__solve_grid(cgl_phys2d_t *w)
{
    int32_t cy;

    for (cy = 0; cy < CGL_PHYS2D_GRID_H; cy++)
    {
        int32_t cx;
        for (cx = 0; cx < CGL_PHYS2D_GRID_W; cx++)
        {
            int32_t c = (cy * CGL_PHYS2D_GRID_W) + cx;
            int16_t ia;

            for (ia = w->cell_head[c]; ia >= 0; ia = w->next[ia])
            {
                const cgl_phys2d_body_t *a = &w->body[ia];
                /* Hoist the fields of `a`: the compiler cannot prove that
                   resolve() does not alias them, so it would reload all three
                   for every neighbour otherwise. */
                int32_t ax = a->x;
                int32_t ay = a->y;
                int32_t ar = a->r;
                uint8_t k;

                for (k = 0u; k < 5u; k++)
                {
                    int32_t nx = cx + cgl_phys2d__offsets[k][0];
                    int32_t ny = cy + cgl_phys2d__offsets[k][1];
                    int32_t nc;
                    int16_t ib;

                    if ((nx < 0) || (nx >= CGL_PHYS2D_GRID_W) || (ny < 0) || (ny >= CGL_PHYS2D_GRID_H))
                    {
                        continue;
                    }
                    nc = (ny * CGL_PHYS2D_GRID_W) + nx;

                    for (ib = w->cell_head[nc]; ib >= 0; ib = w->next[ib])
                    {
                        const cgl_phys2d_body_t *b = &w->body[ib];
                        int32_t dx;
                        int32_t dy;
                        int32_t rsum;

                        if ((k == 0u) && (ib <= ia))
                        {
                            continue; /* same cell: only handle ib > ia once */
                        }

                        /* Cheap inline reject: most neighbour pairs never touch,
                           so resolve() is only entered for real contacts. */
                        dx = (int32_t)b->x - ax;
                        dy = (int32_t)b->y - ay;
                        rsum = ar + (int32_t)b->r;
                        if (((dx * dx) + (dy * dy)) < (rsum * rsum))
                        {
                            cgl_phys2d__resolve(&w->body[ia], &w->body[ib]);
                            /* resolve() may have moved a, refresh the cache */
                            ax = w->body[ia].x;
                            ay = w->body[ia].y;
                        }
                    }
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* public API                                                                 */
/* -------------------------------------------------------------------------- */

void cgl_phys2d_reset(cgl_phys2d_t *w)
{
    w->count = 0u;
    w->active = 0u;
    w->active_dynamic = 0u;
    w->step = 0u;
}

#if CGL_PHYS2D_USER_STORAGE
bool cgl_phys2d_init_storage(cgl_phys2d_t *w, const cgl_phys2d_storage_t *st)
{
    if ((w == NULL) || (st == NULL))
    {
        return false;
    }
    /* Clear the world first: if the storage is rejected, the world must be inert
       rather than holding stale pointers into somebody else's memory. */
    w->body = NULL;
    w->next = NULL;
    w->cell_head = NULL;
    w->capacity = 0u;
    cgl_phys2d_reset(w);

    /* The bound is the index type, not the counter type: see the `_Static_assert`
       in cgl_phys2d.h. `capacity` is a uint16_t, so > 32767 is the only thing left to
       reject here. */
    if ((st->body == NULL) || (st->next == NULL) || (st->cell_head == NULL) ||
        (st->capacity == 0u) || (st->capacity > 32767u))
    {
        return false;
    }

    w->body = st->body;
    w->next = st->next;
    w->cell_head = st->cell_head;
    w->capacity = st->capacity;
    /* An empty world has no active bodies and no valid chain, but every array is
       reachable, so a `cgl_phys2d_add()` on it behaves exactly like the embedded form
       (the grid is rebuilt at the start of every step anyway). */
    return true;
}
#endif

int cgl_phys2d_add(cgl_phys2d_t *w, int16_t x, int16_t y, int16_t vx, int16_t vy,
               int16_t r, uint16_t flags)
{
    cgl_phys2d_body_t *b;

    if (w->count >= cgl_phys2d_capacity(w))
    {
        return -1;
    }
    b = &w->body[w->count];
    b->x = x;
    b->y = y;
    b->vx = vx;
    b->vy = vy;
    b->r = r;
    b->z = 0;
    b->vz = 0;
    b->flags = (uint16_t)(flags | CGL_PHYS2D_FLAG_ACTIVE);
    w->count++;
    return (int)w->count - 1;
}

void cgl_phys2d_step(cgl_phys2d_t *w)
{
#if CGL_PHYS2D_USER_STORAGE
    /* A world whose `cgl_phys2d_init_storage()` was rejected -- or which was never
       bound at all -- has no storage to simulate, so it is inert rather than a
       null dereference. (The embedded form has nothing to check: the storage is
       the struct.) */
    if ((w->body == NULL) || (w->cell_head == NULL))
    {
        return;
    }
#endif
    cgl_phys2d__integrate(w);
    cgl_phys2d__build_grid(w);
    cgl_phys2d__solve_grid(w);
    cgl_phys2d__launch(w); /* after the solve: post-impact speeds drive the hop */
    w->step++;
}
