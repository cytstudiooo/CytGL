// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_iso2d_phys2d.h
  * @brief   The bridge from a `cgl_phys2d` world to the `cgl_iso2d` depth sort.
  *
  *  Why this file exists
  *  --------------------
  *  The module is two layers with a **one-way** dependency:
  *
  *      cgl_iso2d          positions + projection + depth sort; knows NO bodies
  *        |
  *      cgl_phys2d         bodies, broad phase grid, solver, vertical axis
  *
  *  `cgl_iso2d.h` deliberately contains no type, flag or macro of the physics layer:
  *  it sorts *positions* through
  *  the one accessor in `cgl_iso2d_src_t`, which is what makes it reusable for a
  *  sprite list as well. Knowing what a rigid body is belongs one layer up, and
  *  **this header is that layer** -- the only place in the module where the two
  *  halves meet, and therefore the only place allowed to include both.
  *
  *  A consumer that uses both halves needs exactly one extra include:
  *
  *      #include <ext/cgl_phys2d.h>
  *      #include <ext/cgl_iso2d.h>
  *      #include <ext/cgl_iso2d_phys2d.h>           // + cgl_iso2d_sort_bodies()
  *
  *      cgl_iso2d_sort_bodies(&view->sorter, &view->cam, world);
  *
  *  Everything here is inline (or a one-line wrapper around it): a `.c` file would
  *  add a translation unit to the firmware build for what amounts to an adapter,
  *  and the module's promise is that its cost is visible in the consumer's own
  *  object. Include it in the translation unit that draws.
  ******************************************************************************
  */
#ifndef CGL_ISO2D_CGL_PHYS2D_H
#define CGL_ISO2D_CGL_PHYS2D_H

#include <ext/cgl_phys2d.h>
#include <ext/cgl_iso2d.h>

/* -------------------------------------------------------------------------- */
/* The one thing two views of the same module cannot check for themselves      */
/*                                                                            */
/* The sort table is sized by `CGL_ISO2D_MAX_POINTS` (`cgl_iso2d_config.h`) and the     */
/* body pool by `CGL_PHYS2D_MAX_BODIES` (`cgl_phys2d_config.h`). The second is a knob   */
/* of the physics layer's RAM and the first a knob of the projection layer's,   */
/* so neither header can derive it from the other without recreating the        */
/* dependency this split just removed -- and a pool larger than the table means  */
/* bodies silently missing from the scene, which is exactly the class of failure */
/* this project turns into a build error instead of a mystery.                  */
/* -------------------------------------------------------------------------- */
_Static_assert(CGL_PHYS2D_MAX_BODIES <= CGL_ISO2D_MAX_POINTS,
               "the cgl_phys2d pool is larger than the cgl_iso2d sort table: raise CGL_ISO2D_MAX_POINTS (cgl_iso2d_config.h) or lower CGL_PHYS2D_MAX_BODIES (cgl_phys2d_config.h)");

/* -------------------------------------------------------------------------- */
/* Adapter: a world of bodies seen as a list of sortable positions            */
/* -------------------------------------------------------------------------- */

/**
 * `cgl_iso2d_src_t.read` for a physics world: element `i` is body `i`, and it takes
 * part in the scene while `CGL_PHYS2D_FLAG_ACTIVE` is set.
 *
 * A body that has been culled is reported as "not in the scene" rather than
 * sorted and then culled again when drawn: the sorter's `count` is what the
 * renderer iterates, so an inactive body must not occupy a slot in it. The
 * indices handed back are the world's own body indices, which is what lets the
 * renderer go straight from `sorter.order[k]` to `world->body[...]` with all its
 * fields (radius, flags, height) still there.
 */
CGL_EXT_INLINE bool cgl_iso2d_phys2d_read(const void *ctx, uint16_t i, cgl_iso2d_pos_t *out)
{
    const cgl_phys2d_t *w = (const cgl_phys2d_t *)ctx;
    const cgl_phys2d_body_t *b;

    if (i >= w->count)
    {
        return false;
    }
    b = &w->body[i];
    if ((b->flags & CGL_PHYS2D_FLAG_ACTIVE) == 0u)
    {
        return false;
    }
    out->x = b->x;
    out->y = b->y;
    out->z = b->z;
    return true;
}

/**
 * Describe a whole world as `cgl_iso2d` input.
 *
 * The stamp is `world->step`: the physics step *is* the identity of the position
 * set, which is the semantics the sort has always had ("sort once per step, not
 * once per 4 px band"). `cgl_phys2d_reset()` zeroes `step`, so a rebuilt world must
 * be followed by `cgl_iso2d_sorter_reset()` -- the warning in the module plan, still
 * true here.
 */
CGL_EXT_INLINE void cgl_iso2d_phys2d_src(cgl_iso2d_src_t *src, const cgl_phys2d_t *w)
{
    src->read = cgl_iso2d_phys2d_read;
    src->ctx = (const void *)w;
    src->count = w->count;
    src->stamp = w->step;
}

/** Depth-sort every active body of `w` into `s` (far -> near). The one-liner the renderer wants. */
CGL_EXT_INLINE void cgl_iso2d_sort_bodies(cgl_iso2d_sorter_t *s, const cgl_iso2d_cam_t *cam,
                                  const cgl_phys2d_t *w)
{
    cgl_iso2d_src_t src;

    cgl_iso2d_phys2d_src(&src, w);
    cgl_iso2d_sort(s, cam, &src);
}

#endif /* CGL_ISO2D_CGL_PHYS2D_H */
