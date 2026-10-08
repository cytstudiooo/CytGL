// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_debris.c
  * @brief   cgl_debris - the scene: bitmap -> rigid body cloud, camera fit, cycle.
  *
  *  Framework free on purpose: this file includes `cgl_phys2d`, `cgl_iso2d` and its
  *  own config, and nothing that knows what a canvas or a widget is. The drawing is
  *  in `cgl_debris_view.c`; the split is what lets this half be built and
  *  unit-tested with no UI framework in the graph at all.
  *
  *  It never steps the physics: the caller does (see the note in cgl_debris.h).
  ******************************************************************************
  */
#include <ext/cgl_debris.h>

#if CGL_CFG_DEBRIS

/** Tell the application that something has to be repainted. The module cannot call
    `cgl_widget_invalidate()` itself -- that header is on the other side of the
    seam -- so the application hands in this callback instead. */
static void cgl_debris__changed(cgl_debris_t *l)
{
    if ((l != NULL) && (l->on_change != NULL))
    {
        l->on_change(l->on_change_user);
    }
}

static void cgl_debris__build(cgl_debris_t *l);

void cgl_debris_init(cgl_debris_t *d, cgl_phys2d_t *world, cgl_iso2d_view_t *view,
                     const cgl_debris_src_t *src, cgl_debris_rect_t frame)
{
    if ((d == NULL) || (world == NULL) || (src == NULL))
    {
        return;
    }
    d->world = world;
    d->view = view;
    d->src = src;
    d->frame = frame;
    d->on_change = NULL;
    d->on_change_user = NULL;
    d->dot_count = 0u;
    d->ball_index = 0u;
    d->active = 0u;
    d->phase = (uint8_t)CGL_DEBRIS_PHASE_SMASH;
    d->settle_ms = 0u;
    d->restarts = 0u;
    d->style = (uint8_t)CGL_DEBRIS_SCENE_STYLE;

    if (view != NULL)
    {
        cgl_iso2d_view_init(view);
        cgl_debris_set_style(d, (cgl_iso2d_style_t)CGL_DEBRIS_SCENE_STYLE);
    }
    cgl_debris__build(d);
}
/* -------------------------------------------------------------------------- */
/* world construction                                                         */
/* -------------------------------------------------------------------------- */

static void cgl_debris__build(cgl_debris_t *l)
{
    const cgl_debris_src_t *s = l->src;
    cgl_phys2d_t *w = l->world;
    int32_t j;
    uint16_t dots = 0u;

    cgl_phys2d_reset(w);
    /* after the world is rebuilt step starts over from the beginning, so the
       sorter must be invalidated, otherwise the next frame may hit a stale stamp */
    if (l->view != NULL)
    {
        cgl_iso2d_sorter_reset(&l->view->sorter);
    }

    for (j = 0; j < s->grid_h; j++)
    {
        int32_t i;
        for (i = 0; i < s->grid_w; i++)
        {
            int16_t x;
            int16_t y;

            if (s->is_lit(i, j) == 0)
            {
                continue;
            }

            /* The original scatters its balls by 0.05 world units of random
               jitter. Here the grid is left perfectly regular on purpose: the
               pitch is an exact 3 px, so every dot lands on the same pixel
               phase and the matrix stays crisp. The 2/16 px gap between the
               collision radii keeps neighbouring dots out of contact, so
               nothing jostles at start up. */
            x = (int16_t)(CGL_DEBRIS_X0_Q4(s->grid_w) + (i * CGL_DEBRIS_STEP_Q4));
            y = (int16_t)(CGL_DEBRIS_Y0_Q4(s->grid_h) + (j * CGL_DEBRIS_STEP_Q4));

            if (cgl_phys2d_add(w, x, y, 0, 0, CGL_DEBRIS_DOT_R_Q4, 0u) < 0)
            {
                break;
            }
            dots++;
        }
    }
    l->dot_count = dots;

    /* the heavy ball, kinematic so it is never pushed around.
       FLAG_SPHERE makes the renderer draw it as a "ball standing on the floor"
       (lifted by one radius + a shadow) instead of a dot glued to the ground. */
    l->ball_index = (uint16_t)cgl_phys2d_add(w, CGL_DEBRIS_BALL_START_X_Q4,
                                         (int16_t)(CGL_DEBRIS_CENTER_Y_Q4 + CGL_DEBRIS_BALL_START_Y_OFFSET_Q4),
                                         CGL_DEBRIS_BALL_SPEED_Q4, 0, CGL_DEBRIS_BALL_R_Q4,
                                         (uint16_t)(CGL_PHYS2D_FLAG_KINEMATIC |
                                                    CGL_PHYS2D_FLAG_SPHERE));
    l->phase = (uint8_t)CGL_DEBRIS_PHASE_SMASH;
    l->settle_ms = 0u;
    l->active = dots;
    w->active = dots;         /* the whole logo starts inside the domain,   */
    w->active_dynamic = dots; /* the next step recomputes both anyway       */
}

void cgl_debris_restart(cgl_debris_t *l)
{
    if (l == NULL)
    {
        return;
    }
    cgl_debris__build(l);
    l->restarts++;
    cgl_debris__changed(l);
}

/* -------------------------------------------------------------------------- */
/* camera                                                                     */
/* -------------------------------------------------------------------------- */

/** Bounding box of the dot matrix in the world (Q12.4), including half a cell of
    margin. The macros themselves are `CGL_DEBRIS_WORLD_*` in cgl_debris_config.h;
    they take the grid size, because that is the caller's. */
/**
 * The camera's fitting range: the bounding box of **the dots that are actually
 * lit**, not the whole 106 x 14 grid.
 *
 * The bitmap keeps blank margins on all four sides (the Logo glyphs do not reach
 * the grid border), so fitting to the whole grid treats the blank margin as
 * content as well and measurably wastes about 40% of the screen -- fitting to
 * the lit range makes the dot matrix noticeably bigger.
 * The 1484 cells are scanned only once at init / on a style switch, so the cost
 * is negligible.
 */
static void cgl_debris__fit_rect(const cgl_debris_src_t *s, int16_t *x0, int16_t *y0, int16_t *ww, int16_t *wh)
{
    int32_t j;
    int32_t i0 = s->grid_w;
    int32_t j0 = s->grid_h;
    int32_t i1 = -1;
    int32_t j1 = -1;
    bool any = false;

    for (j = 0; j < s->grid_h; j++)
    {
        int32_t i;
        for (i = 0; i < s->grid_w; i++)
        {
            if (s->is_lit(i, j) == 0)
            {
                continue;
            }
            any = true;
            if (i < i0) { i0 = i; }
            if (i > i1) { i1 = i; }
            if (j < j0) { j0 = j; }
            if (j > j1) { j1 = j; }
        }
    }

    if (!any)
    {
        /* empty bitmap: fall back to the whole grid, so at least a zero-sized
           camera is never produced */
        *x0 = (int16_t)CGL_DEBRIS_WORLD_X0_Q4(s->grid_w);
        *y0 = (int16_t)CGL_DEBRIS_WORLD_Y0_Q4(s->grid_h);
        *ww = (int16_t)CGL_DEBRIS_WORLD_W_Q4(s->grid_w);
        *wh = (int16_t)CGL_DEBRIS_WORLD_H_Q4(s->grid_h);
        return;
    }

    *x0 = (int16_t)(CGL_DEBRIS_X0_Q4(s->grid_w) + (i0 * CGL_DEBRIS_STEP_Q4) - (CGL_DEBRIS_STEP_Q4 / 2));
    *y0 = (int16_t)(CGL_DEBRIS_Y0_Q4(s->grid_h) + (j0 * CGL_DEBRIS_STEP_Q4) - (CGL_DEBRIS_STEP_Q4 / 2));
    *ww = (int16_t)(((i1 - i0) * CGL_DEBRIS_STEP_Q4) + CGL_DEBRIS_STEP_Q4);
    *wh = (int16_t)(((j1 - j0) * CGL_DEBRIS_STEP_Q4) + CGL_DEBRIS_STEP_Q4);
}

void cgl_debris_set_style(cgl_debris_t *l, cgl_iso2d_style_t style)
{
    cgl_debris_rect_t f;
    int16_t rx;
    int16_t ry;
    int16_t rw;
    int16_t rh;

    if ((l == NULL) || (l->view == NULL))
    {
        return;
    }
    f = l->frame;
    cgl_debris__fit_rect(l->src, &rx, &ry, &rw, &rh);
    cgl_iso2d_cam_fit(&l->view->cam, style,
                  rx, ry, rw, rh,
                  (int16_t)(f.x0 + CGL_DEBRIS_SCENE_MARGIN_PX),
                  (int16_t)(f.y0 + CGL_DEBRIS_SCENE_MARGIN_PX),
                  (int16_t)((f.x1 - f.x0 + 1) - (2 * CGL_DEBRIS_SCENE_MARGIN_PX)),
                  (int16_t)((f.y1 - f.y0 + 1) - (2 * CGL_DEBRIS_SCENE_MARGIN_PX)));
    cgl_iso2d_sorter_reset(&l->view->sorter);
    l->style = (uint8_t)style;
    cgl_debris__changed(l);
}
void cgl_debris_step(cgl_debris_t *l, uint32_t step_ms)
{
    cgl_phys2d_t *world;

    if (l == NULL)
    {
        return;
    }
    world = l->world;
    if (world == NULL)
    {
        return;
    }

    /* * The world has already been stepped once by the **page**
       (cgl_page_vt_t.on_step); here we only observe the result.
       Do not call cgl_phys2d_step() here: that is exactly where the old
       "whoever calls it first does the stepping" problem begins. */
    l->active = world->active_dynamic;
    cgl_debris__changed(l);

    if (l->ball_index >= world->count)
    {
        return;
    }

    {
        const cgl_phys2d_body_t *ball = &world->body[l->ball_index];

        if (l->phase == (uint8_t)CGL_DEBRIS_PHASE_SMASH)
        {
            if (cgl_phys2d_to_px(ball->x) > (CGL_DEBRIS_SCREEN_W + 30))
            {
                l->phase = (uint8_t)CGL_DEBRIS_PHASE_SETTLE;
                l->settle_ms = 0u;
                /* stop the (kinematic) ball so it cannot run away from the
                   simulation domain while the wake settles */
                world->body[l->ball_index].vx = 0;
                world->body[l->ball_index].vy = 0;
            }
            else if (l->active == 0u)
            {
                /* every dot has been swept out of the domain already */
                l->phase = (uint8_t)CGL_DEBRIS_PHASE_SETTLE;
                l->settle_ms = CGL_DEBRIS_SETTLE_MS / 2u;
            }
        }

        if (l->phase == (uint8_t)CGL_DEBRIS_PHASE_SETTLE)
        {
            l->settle_ms = (uint16_t)(l->settle_ms + step_ms);
            if (l->settle_ms >= CGL_DEBRIS_SETTLE_MS)
            {
                cgl_debris__build(l);
                l->restarts++;
            }
        }
    }
}
uint16_t cgl_debris_dot_count(const cgl_debris_t *l)
{
    return l->dot_count;
}

bool cgl_debris_cell_world(const cgl_debris_t *l, int i, int j, int16_t *x, int16_t *y)
{
    const cgl_debris_src_t *s = ((l != NULL) ? l->src : NULL);

    if (s == NULL)
    {
        return false;
    }
    if ((i < 0) || (i >= s->grid_w) || (j < 0) || (j >= s->grid_h))
    {
        return false;
    }
    if (x != NULL)
    {
        *x = (int16_t)(CGL_DEBRIS_X0_Q4(s->grid_w) + (i * CGL_DEBRIS_STEP_Q4));
    }
    if (y != NULL)
    {
        *y = (int16_t)(CGL_DEBRIS_Y0_Q4(s->grid_h) + (j * CGL_DEBRIS_STEP_Q4));
    }
    return true;
}

uint16_t cgl_debris_active_dots(const cgl_debris_t *l)
{
    return l->active;
}

#endif /* CGL_CFG_DEBRIS */
