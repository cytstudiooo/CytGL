// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_iso2d.h
  * @brief   Isometric camera + painter's algorithm depth sort.
  *
  *  In one sentence: **the physics layer still only collides in the XY plane, and
  *  cgl_iso2d is only responsible for drawing that plane as a 3D space with height**.
  *  Physics and projection are separate, so the collision feel, the tuning and
  *  the tests all stay exactly the same.
  *
  *  Projection (world Q12.4 -> screen px, all integer maths, never touches libm):
  *
  *      ground_x = ((m00 * x + m01 * y) >> 12) + ox
  *      ground_y = ((m10 * x + m11 * y) >> 12) + oy     <- also the depth key
  *      screen_y = ground_y - ((kz * z) >> 12)
  *
  *  30 deg isometric is only one of the possible choices (m00 = +kx, m01 = -kx,
  *  m10 = ky, m11 = ky):
  *
  *      screen_x = (world_x - world_y) * cos30
  *      screen_y = (world_x + world_y) * sin30 - z * height
  *
  *  Why the depth key is not y
  *  --------------------------
  *  "Sort by y" is wrong under an isometric projection. The front-to-back
  *  relation on screen is decided by the **camera depth**, and in this
  *  projection the camera depth is the projected ground y, i.e. m10*x + m11*y.
  *  The dot matrix logo happens to be a long strip (106 x 14 cells): x differs
  *  by 300 px while y differs by only a few dozen px -- if you sort by y alone,
  *  two dots 300 px apart in the same row are judged to be at the same depth and
  *  which one occludes the other is random.
  *
  *  This module uniformly takes **ground_y as the depth key**. It has two
  *  benefits:
  *    - in the isometric styles it is automatically the weighted sum of the two
  *      world axes;
  *    - in flat mode m10=0, so the key degenerates to y, matching the "sort by y"
  *      intuition.
  *  A single piece of code is correct for both, no two sort variants are needed.
  *
  *  Why counting sort instead of qsort
  *  ----------------------------------
  *  The depth key is a **bounded screen coordinate** (0..171), so a bucket sort
  *  is O(n + 256) and fully deterministic: no comparisons, no recursion, no 2 KB
  *  comparison-sort scratch area, and no worst-case degradation either.
  *  The sorted result is ordered "far to near" (bucket 0 = top of the screen =
  *  farthest), so drawing it straight through is all that is needed -- no
  *  z-buffer, because the band renderer has nowhere to put a z-buffer anyway.
  *
  *  The sort runs only once per physics step
  *  ----------------------------------------
  *  The renderer is called **per 4 px high band** (a 320x172 screen = 43 bands);
  *  sorting once per band would be 43x wasted work. So the sort result carries a
  *  stamp (`s->stamp == src->stamp` returns immediately), and it does not matter
  *  who triggered the step. What that stamp *is* belongs to the caller: for a
  *  physics world it is `world->step`, for a sprite list it is whatever the
  *  caller calls "one update".
  *
  *  This layer knows no rigid bodies
  *  --------------------------------
  *  `cgl_iso2d` sorts **positions**, handed to it through one accessor plus a count
  *  (`cgl_iso2d_src_t`), and it projects them. It contains no type, no flag and no
  *  macro of the physics layer -- not even in a comment, which is why this header
  *  spells that layer out in prose: "give me N projected, depth-sorted positions"
  *  is useful to anyone with a sprite list, while "give me the sorted order of a
  *  rigid body pool" is useful to one demo. The adapter that turns a physics
  *  world into that accessor form lives one layer up, in the module's bridge
  *  header (whose name, unlike this file's, is allowed to say what it bridges).
  ******************************************************************************
  */
#ifndef CGL_ISO2D_H
#define CGL_ISO2D_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h> /* NULL */

#include <ext/cgl_iso2d_config.h>
#include <ext/cgl_ext_basis.h>

/* -------------------------------------------------------------------------- */
/* Tunables                                                                   */
/*                                                                            */
/* CGL_ISO2D_COS30_Q8 / CGL_ISO2D_SIN30_Q8 / CGL_ISO2D_SQRT3_Q8 / CGL_ISO2D_LIFT_Q8 /           */
/* CGL_ISO2D_DEPTH_BUCKETS / CGL_ISO2D_MAX_POINTS all live in `cgl_iso2d_config.h`: they    */
/* are panel/footprint knobs, and the module's two layers keep their knobs in   */
/* their own file so that neither has to be configured by the other's.          */
/* -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- */
/* Camera                                                                     */
/* -------------------------------------------------------------------------- */

typedef struct
{
    int16_t m00; /**< screen x coefficient, Q8; combined with Q12.4 world coords >>12 gives px */
    int16_t m01;
    int16_t m10; /**< depth (= ground screen y) coefficient, Q8; also the sort key      */
    int16_t m11;
    int16_t kz;  /**< height lift coefficient, Q8                                     */
    int16_t ox;  /**< screen x offset, px                                             */
    int16_t oy;  /**< screen y offset, px                                             */
} cgl_iso2d_cam_t;

typedef enum
{
    /**
     * The floor is viewed at a slant and the vertical compression of the two
     * world axes is **solved separately**, the goal being to maximise the
     * minimum spacing of the projected dot matrix (the airiest dot matrix).
     * This is the project default; the reasoning is in cgl_iso2d_cam_fit().
     */
    CGL_ISO2D_STYLE_ISO = 0,
    /**
     * Textbook 30 deg isometric: both world axes get the same vertical
     * coefficient (m10 == m11). On a square world it is the optimum and the dot
     * matrix lands on an equilateral triangle lattice; but the more "strip-like"
     * the world is, the flatter it squeezes the dot matrix (see the derivation
     * in cgl_iso2d_cam_fit()).
     */
    CGL_ISO2D_STYLE_ISO_UNIFORM = 1,
    /** Identity matrix: the XY plane is laid on the screen 1:1 (the old flat look, no pseudo-3D). */
    CGL_ISO2D_STYLE_FLAT = 2
} cgl_iso2d_style_t;

/**
 * Make the contents of the "world rectangle" land exactly inside the "viewport
 * rectangle" (both centred), and derive the projection coefficients from that.
 *
 * **Guarantee**: after it returns, the projections of the four corners of the
 * world rectangle always land inside [vx, vx+vw-1] x [vy, vy+vh-1] (internally
 * the projected bounding box is measured and shrunk as needed, because fixed
 * point rounding turns "computed to fit exactly" into leaking out by one or two
 * pixels). The one case it cannot fix is a viewport smaller than the world even
 * at unit scale: it then stops shrinking and returns the smallest camera it has
 * rather than failing.
 *
 * @param cam         output camera
 * @param style       see cgl_iso2d_style_t
 * @param wx0,wy0,ww,wh   world rectangle, Q12.4
 * @param vx,vy,vw,vh      viewport rectangle, px (the caller keeps its own margin)
 */
void cgl_iso2d_cam_fit(cgl_iso2d_cam_t *cam, cgl_iso2d_style_t style,
                   int16_t wx0, int16_t wy0, int16_t ww, int16_t wh,
                   int16_t vx, int16_t vy, int16_t vw, int16_t vh);

/** Low level entry point: hand over the matrix directly, handy for trying angles out on the real hardware. */
void cgl_iso2d_cam_set(cgl_iso2d_cam_t *cam, int16_t m00, int16_t m01,
                   int16_t m10, int16_t m11, int16_t kz,
                   int16_t ox, int16_t oy);

/**
 * Screen bounding box of the projected world rectangle (z=0). The four corners
 * are projected and min/max taken -- under this projection the rectangle turns
 * into a parallelogram, so the bounding box must be computed from all four
 * corners and cannot just look at the diagonal.
 */
void cgl_iso2d_project_rect(const cgl_iso2d_cam_t *cam,
                        int16_t wx0, int16_t wy0, int16_t ww, int16_t wh,
                        int16_t *x0, int16_t *y0, int16_t *x1, int16_t *y1);

/* -------------------------------------------------------------------------- */
/* Projection (hot path, all force-inlined -- see cgl_ext_basis.h for what that    */
/* degrades to on a compiler without the extension)                            */
/* -------------------------------------------------------------------------- */

/** Screen x of the ground projection (px). */
CGL_EXT_INLINE int32_t cgl_iso2d_ground_x(
    const cgl_iso2d_cam_t *c, int16_t wx, int16_t wy)
{
    return (((int32_t)c->m00 * wx) + ((int32_t)c->m01 * wy)) >> 12;
}

/** Screen y of the ground projection (px). **This is the depth key**: the larger it is, the closer to the observer. */
CGL_EXT_INLINE int32_t cgl_iso2d_ground_y(
    const cgl_iso2d_cam_t *c, int16_t wx, int16_t wy)
{
    return (((int32_t)c->m10 * wx) + ((int32_t)c->m11 * wy)) >> 12;
}

/**
 * Full 3D projection: world (x, y, z) -> screen (sx, sy).
 * World coordinates are Q12.4 (z is Q12.4 as well), the output is pixels.
 */
CGL_EXT_INLINE void cgl_iso2d_project(
    const cgl_iso2d_cam_t *c, int16_t wx, int16_t wy, int16_t wz,
    int16_t *sx, int16_t *sy)
{
    const int32_t gy = cgl_iso2d_ground_y(c, wx, wy);

    *sx = (int16_t)(cgl_iso2d_ground_x(c, wx, wy) + c->ox);
    *sy = (int16_t)(gy - (((int32_t)c->kz * wz) >> 12) + c->oy);
}

/* -------------------------------------------------------------------------- */
/* Depth sort (painter's algorithm)                                           */
/* -------------------------------------------------------------------------- */

/**
 * One thing to sort: a position in the module's Q12.4 world convention.
 *
 * Flat by value on purpose -- the accessor below fills it, and it is three
 * `int16_t`, so passing it around costs nothing and it keeps the sort free of
 * any assumption about the caller's storage layout (an array of structs, a pool
 * with holes, a sprite table: all the same to this layer).
 *
 * `z` is carried for the *renderer* (`cgl_iso2d_project()`); the sort itself only
 * looks at `x` and `y`, because the depth key is the projected ground y.
 */
typedef struct
{
    int16_t x; /**< world x, Q12.4                                            */
    int16_t y; /**< world y, Q12.4                                            */
    int16_t z; /**< height above the floor plane, Q12.4 (never negative)      */
} cgl_iso2d_pos_t;

/**
 * The sort's input: "give me element `i`", plus how many there are.
 *
 * `read` is the whole contract. It must fill `*out` from element `i` and return
 * true, or return false when element `i` is not part of the scene and has to be
 * skipped (for a physics world: the body has been culled). It is called for
 * `i` in `0 .. count-1` only, in increasing order, twice per sort -- once to
 * count per depth bucket and once to place -- and it must answer the same way
 * both times.
 *
 * COST: one indirect call per element per pass, ~2 per element per sort. The
 * sort itself runs at most once per `stamp` (not once per band), so this is
 * ~2 calls per element per simulated step. The measured firmware cost of replacing
 * the direct body access with it is **112 B of `.text` and 0 B of RAM** (see the
 * cost table in the CytGL manual, section 15.9).
 */
typedef struct
{
    /** Fill `*out` from element `i`; false = element `i` is not in the scene. */
    bool (*read)(const void *ctx, uint16_t i, cgl_iso2d_pos_t *out);
    /** Handed straight back to `read`; the caller's own state (e.g. the world). */
    const void *ctx;
    /** Elements offered to `read`: `0 .. count-1`. Larger than the table is fine;
        see `CGL_ISO2D_MAX_POINTS` for what happens then. */
    uint16_t count;
    /**
     * Identity of the current position set, for the "sort once per step"
     * optimisation above.
     *
     * **Any change to any position must come with a different stamp**, or the
     * sorter will go on serving the previous order until something else forces a
     * re-sort. `cgl_iso2d_sorter_reset()` exists for exactly that ("this is a new
     * scene, do not trust the stamp").
     */
    uint32_t stamp;
} cgl_iso2d_src_t;

/**
 * Sorter: one index permutation table plus one bucket count table.
 *
 * RAM: order 2 B per point (= `CGL_ISO2D_MAX_POINTS`), bucket 2 B x
 * `CGL_ISO2D_DEPTH_BUCKETS`. By comparison: a comparison sort needs 2 x 2 B per
 * element for the scratch area alone, and in the worst case it can degrade to
 * O(n^2).
 *
 * `order[]` holds **the caller's element indices**, so it is only meaningful
 * together with the `cgl_iso2d_src_t` that produced it.
 */
typedef struct
{
    int16_t order[CGL_ISO2D_MAX_POINTS]; /**< element indices, far -> near, drawn straight through */
    uint16_t bucket[CGL_ISO2D_DEPTH_BUCKETS];
    uint16_t count; /**< number of valid elements in order                       */
    uint32_t stamp; /**< the `src->stamp` the last sort corresponds to           */
    bool valid;     /**< when false the next sort is guaranteed to re-sort       */
} cgl_iso2d_sorter_t;

/* Consistency gates. `CGL_ISO2D_MAX_POINTS` is a new knob, so it is gated rather
   than trusted: the bucket table holds one count per element in a uint16_t, and
   the order table holds one index in an int16_t (which the old gate could not
   see, because that table used to be sized by another layer's body pool). */
_Static_assert(CGL_ISO2D_DEPTH_BUCKETS > 0,
               "CGL_ISO2D_DEPTH_BUCKETS must be positive");
_Static_assert(CGL_ISO2D_MAX_POINTS > 0,
               "CGL_ISO2D_MAX_POINTS must be positive");
_Static_assert(CGL_ISO2D_MAX_POINTS <= 32767,
               "cgl_iso2d_sorter_t.order[] holds int16_t indices: CGL_ISO2D_MAX_POINTS must be <= 32767");

/** Force the next cgl_iso2d_sort() to re-sort (call it after a world rebuild or a camera change). */
void cgl_iso2d_sorter_reset(cgl_iso2d_sorter_t *s);

/**
 * Sort the positions described by `src` into draw order by depth (far -> near).
 *
 * Calling it again with the same `src->stamp` is a no-op, so every band of a
 * frame can call it safely. Elements past `CGL_ISO2D_MAX_POINTS` are ignored (the
 * table cannot hold them) -- size the knob for the scene, and note the compile
 * time gate in the module's physics bridge, which catches that case for a body
 * pool.
 */
void cgl_iso2d_sort(cgl_iso2d_sorter_t *s, const cgl_iso2d_cam_t *cam, const cgl_iso2d_src_t *src);

/* -------------------------------------------------------------------------- */
/* Camera + sorter: owned once per scene, shared by several widgets           */
/* -------------------------------------------------------------------------- */

typedef struct
{
    cgl_iso2d_cam_t cam;
    cgl_iso2d_sorter_t sorter;
} cgl_iso2d_view_t;

void cgl_iso2d_view_init(cgl_iso2d_view_t *v);

#endif /* CGL_ISO2D_H */
