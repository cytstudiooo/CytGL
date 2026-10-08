// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_widget.c
  * @brief   LCD UI framework - retained mode widget tree implementation.
  ******************************************************************************
  */
#include <cytgl/cgl_widget.h>
#include <cytgl/cytgl_config.h> /* CGL_CFG_WIDGET_MAX_CHILDREN, CGL_CFG_SCREEN_W/H */

static cgl_damage_fn s_damage_fn = NULL;
static void *s_damage_user = NULL;

/** Number of times alpha was requested but could not take effect (no scratch /
    nesting levels exhausted). For observation. */
static uint32_t s_alpha_dropped = 0u;

/** Number of times a child could not be attached because the parent was full
    (see CGL_CFG_WIDGET_MAX_CHILDREN). For observation. */
static uint32_t s_children_dropped = 0u;

void cgl_widget_set_damage_sink(cgl_damage_fn fn, void *user)
{
    s_damage_fn = fn;
    s_damage_user = user;
}

void cgl_widget_invalidate_rect(cgl_rect_t r)
{
    if ((s_damage_fn != NULL) && !cgl_rect_is_empty(r))
    {
        s_damage_fn(r, s_damage_user);
    }
}

void cgl_widget_invalidate(const cgl_widget_t *w)
{
    if (w != NULL)
    {
        cgl_widget_invalidate_rect(w->frame);
    }
}

void cgl_widget_init(cgl_widget_t *w, const cgl_widget_vt_t *vt,
                    const char *name, cgl_rect_t frame)
{
    w->vt = vt;
    w->name = name;
    w->frame = frame;
    w->offset.x = 0;
    w->offset.y = 0;
    w->warp = NULL;
    w->parent = NULL;
    w->first_child = NULL;
    w->last_child = NULL;
    w->next = NULL;
    w->flags = (uint16_t)(CGL_WF_VISIBLE | CGL_WF_CLIP);
    /* Fully opaque by default = the whole group-opacity path is skipped (at the
       cost of one comparison). */
    w->alpha_q8 = (uint8_t)CGL_ALPHA_OPAQUE;
    w->user = NULL;
}

bool cgl_widget_add_child(cgl_widget_t *parent, cgl_widget_t *child)
{
    uint16_t count = 0u;
    const cgl_widget_t *it;

    if ((parent == NULL) || (child == NULL) || (child->parent != NULL))
    {
        return false;
    }

    /* The limit is enforced, not documented: an attached child that silently fell
       off the end of the list is invisible in a band renderer (no framebuffer to
       compare against), so refusing loudly and counting is the only way the failure
       is observable. See the note in cgl_widget.h. */
    for (it = parent->first_child; it != NULL; it = it->next)
    {
        count++;
    }
    if (count >= (uint16_t)CGL_CFG_WIDGET_MAX_CHILDREN)
    {
        s_children_dropped++;
        return false;
    }

    child->parent = parent;
    child->next = NULL;
    if (parent->last_child == NULL)
    {
        parent->first_child = child;
    }
    else
    {
        parent->last_child->next = child;
    }
    parent->last_child = child;
    cgl_widget_invalidate(child);
    return true;
}

void cgl_widget_remove_child(cgl_widget_t *child)
{
    cgl_widget_t *p;
    cgl_widget_t *prev;

    if ((child == NULL) || (child->parent == NULL))
    {
        return;
    }
    p = child->parent;
    prev = NULL;
    for (cgl_widget_t *it = p->first_child; it != NULL; it = it->next)
    {
        if (it == child)
        {
            if (prev == NULL)
            {
                p->first_child = it->next;
            }
            else
            {
                prev->next = it->next;
            }
            if (p->last_child == it)
            {
                p->last_child = prev;
            }
            break;
        }
        prev = it;
    }
    child->parent = NULL;
    child->next = NULL;
    cgl_widget_invalidate(child);
}

void cgl_widget_set_frame(cgl_widget_t *w, cgl_rect_t frame)
{
    if (w == NULL)
    {
        return;
    }
    cgl_widget_invalidate(w);
    w->frame = frame;
    cgl_widget_invalidate(w);
}

void cgl_widget_set_visible(cgl_widget_t *w, bool visible)
{
    if (w == NULL)
    {
        return;
    }
    if (visible)
    {
        w->flags |= CGL_WF_VISIBLE;
    }
    else
    {
        w->flags &= (uint16_t)~CGL_WF_VISIBLE;
    }
    cgl_widget_invalidate(w);
}

void cgl_widget_set_offset(cgl_widget_t *w, int16_t dx, int16_t dy)
{
    cgl_rect_t before;
    cgl_rect_t after;

    if (w == NULL)
    {
        return;
    }
    before = cgl_rect_translate(w->frame, w->offset);
    w->offset.x = dx;
    w->offset.y = dy;
    after = cgl_rect_translate(w->frame, w->offset);

    /* Both the old and the new position must be repainted: marking only the new
       one leaves a ghost behind at the old spot (the panel has no framebuffer,
       so pixels that were written do not disappear by themselves). */
    cgl_widget_invalidate_rect(cgl_rect_union(before, after));
}

void cgl_widget_set_warp(cgl_widget_t *w, const cgl_warp_t *warp)
{
    if (w == NULL)
    {
        return;
    }
    if (w->warp == warp)
    {
        return;
    }
    /* The warp amount changes every frame, so "tracing the outline" is left to
       the caller damaging the whole screen each frame (that is what transitions
       do); here we only damage this subtree once on an on/off flip, to avoid a
       ghost still being left after the warp is removed. */
    cgl_widget_invalidate(w);
    w->warp = warp;
    cgl_widget_invalidate(w);
}

void cgl_widget_raise(cgl_widget_t *w)
{
    cgl_widget_t *p;

    if ((w == NULL) || (w->parent == NULL))
    {
        return;
    }
    p = w->parent;
    if (p->last_child == w)
    {
        return; /* already on top */
    }
    cgl_widget_remove_child(w);
    /* Cannot be refused: removing `w` just freed a slot in this very list. */
    (void)cgl_widget_add_child(p, w);
}

void cgl_widget_set_alpha(cgl_widget_t *w, uint8_t alpha)
{
    if ((w == NULL) || (w->alpha_q8 == alpha))
    {
        return;
    }
    w->alpha_q8 = alpha;
    /* The affected range is exactly this widget's own level (see the notes in
       cgl_widget.h). */
    cgl_widget_invalidate(w);
}

uint8_t cgl_widget_get_alpha(const cgl_widget_t *w)
{
    return (w == NULL) ? (uint8_t)CGL_ALPHA_OPAQUE : w->alpha_q8;
}

uint32_t cgl_widget_alpha_dropped(void)
{
    return s_alpha_dropped;
}

uint32_t cgl_widget_children_dropped(void)
{
    return s_children_dropped;
}

void cgl_widget_draw_tree(cgl_widget_t *w, cgl_canvas_t *c)
{
    cgl_rect_t saved_clip;
    cgl_rect_t saved_src_clip;
    cgl_point_t saved_pan;
    cgl_point_t pan;
    cgl_rect_t on_screen;
    cgl_widget_t *child;

    if ((w == NULL) || ((w->flags & CGL_WF_VISIBLE) == 0u))
    {
        return;
    }

    saved_clip = cgl_canvas_get_clip(c);
    saved_src_clip = cgl_canvas_get_src_clip(c);
    saved_pan = cgl_canvas_get_pan(c);

    /* Accumulated pan: screen coordinates = widget-local coordinates + the
       offsets of every ancestor (including this widget itself) */
    pan.x = (int16_t)(saved_pan.x + w->offset.x);
    pan.y = (int16_t)(saved_pan.y + w->offset.y);
    cgl_canvas_set_pan(c, pan);

    /* * The warp takes effect **after** the pan: the warp consumes "the
       coordinates where this level would sit on screen while static". It is
       installed on entering this level and restored on leaving -- exactly
       symmetric with the pan. */
    {
        const cgl_warp_t *saved_warp = cgl_canvas_get_warp(c);
        cgl_rect_t src_base = saved_src_clip;
        bool warped;

        if (w->warp != NULL)
        {
            cgl_canvas_set_warp(c, w->warp);
            /* Source space restarts at this level: the "source" of the warp is
               this widget's frame. However far its children draw outside that
               frame, they are still clipped in source space -- consistent with
               the CLIP semantics without a warp (the projected floor grid lines
               rely on exactly this). */
            src_base = cgl_rect_translate(w->frame, pan);
            cgl_canvas_set_src_clip(c, src_base);
        }
        warped = (cgl_canvas_get_warp(c) != NULL);

        /* Culling uses "where this widget lands **after** being warped".
           Culling with the source frame is wrong: when a page is sucked to the
           other end of the screen, the source box and the destination box can
           have no overlap at all (measured: culling with the source box makes a
           whole page draw only a few lines at the very top). map_rect returns a
           conservative bounding box. */
        on_screen = cgl_canvas_map_rect(c, cgl_rect_translate(w->frame, pan));
        if (cgl_rect_is_empty(cgl_rect_intersect(on_screen, saved_clip)))
        {
            cgl_canvas_set_src_clip(c, saved_src_clip);
            cgl_canvas_set_warp(c, saved_warp);
            cgl_canvas_set_pan(c, saved_pan);
            return;
        }

        if ((w->flags & CGL_WF_CLIP) != 0u)
        {
            if (warped)
            {
                /* Inside a warped subtree the clip must stay in **source** space
                   (see src_clip in cgl_canvas.h) */
                cgl_canvas_set_src_clip(c, cgl_rect_intersect(src_base,
                                                            cgl_rect_translate(w->frame, pan)));
            }
            else
            {
                cgl_canvas_set_clip(c, cgl_rect_intersect(saved_clip, on_screen));
            }
        }

        /* ---- Group opacity ------------------------------------------------
         * Approach: rasterise this level (its own draw + the whole subtree) in
         * isolation into scratch, then blend the whole thing back over the
         * background with alpha. All four steps live in the block below, and on
         * exit buf / alpha_used are guaranteed to be restored -- otherwise the
         * whole picture would be drawn into scratch and lost.
         *
         * With alpha = CGL_ALPHA_OPAQUE (the default) the whole section is
         * skipped, at the cost of one comparison.
         * ---------------------------------------------------------------- */
        {
            cgl_color_t *group_scratch = NULL;
            cgl_color_t *saved_buf = NULL;
            cgl_rect_t group_rect = cgl_rect_none();
            bool skip_subtree = false;

            if (w->alpha_q8 == 0u)
            {
                /* Fully transparent = not a single pixel of the whole subtree is
                   drawn, without even touching scratch. */
                skip_subtree = true;
            }
            else if (w->alpha_q8 < (uint8_t)CGL_ALPHA_OPAQUE)
            {
                if ((c->alpha_scratch != NULL) && (c->alpha_used < c->alpha_levels))
                {
                    /* The isolated region is "where this level **can** put
                       pixels": with a clipping constraint (CGL_WF_CLIP, on by
                       default, or a live warp) it is the conservative bounding
                       box of the frame intersected with clip -- tight and always
                       sufficient; without one the widget is allowed to draw
                       outside its frame, so the whole clip has to be taken
                       conservatively. */
                    group_rect = ((warped || ((w->flags & CGL_WF_CLIP) != 0u))
                                      ? cgl_rect_intersect(on_screen, cgl_canvas_get_clip(c))
                                      : cgl_canvas_get_clip(c));
                }

                if (cgl_rect_is_empty(group_rect))
                {
                    s_alpha_dropped++; /* no scratch / nesting exhausted / nowhere
                                          to draw */
                }
                else
                {
                    group_scratch = c->alpha_scratch +
                                    ((uint32_t)c->alpha_used * c->alpha_stride);
                    /* * Copy the "background" into scratch first instead of
                       clearing it:
                       (a) semi-transparent blending between child widgets must
                           blend over the **background**; clearing would blend
                           over black instead;
                       (b) after the copy, the pixels no child drew on still
                           equal the background, and the final blend step is the
                           identity transform for them -- so **no coverage mask
                           is needed**. */
                    cgl_canvas_copy_rect(c, group_scratch, group_rect);
                    saved_buf = c->buf;
                    c->buf = group_scratch;
                    c->alpha_used++;
                }
            }

            if (!skip_subtree)
            {
                if ((w->vt != NULL) && (w->vt->draw != NULL))
                {
                    w->vt->draw(w, c);
                }
                for (child = w->first_child; child != NULL; child = child->next)
                {
                    cgl_widget_draw_tree(child, c);
                }
            }

            if (group_scratch != NULL)
            {
                c->alpha_used--;
                c->buf = saved_buf;
                cgl_canvas_blend_src_q8(c, group_rect, group_scratch, w->alpha_q8);
            }
        }

        cgl_canvas_set_clip(c, saved_clip);
        cgl_canvas_set_src_clip(c, saved_src_clip);
        cgl_canvas_set_warp(c, saved_warp);
    }
    cgl_canvas_set_pan(c, saved_pan);
}

void cgl_widget_update_tree(cgl_widget_t *w, uint32_t dt_ms)
{
    cgl_widget_t *child;

    if ((w == NULL) || ((w->flags & CGL_WF_VISIBLE) == 0u))
    {
        return;
    }
    if ((w->vt != NULL) && (w->vt->update != NULL))
    {
        w->vt->update(w, dt_ms);
    }
    for (child = w->first_child; child != NULL; child = child->next)
    {
        cgl_widget_update_tree(child, dt_ms);
    }
}

static cgl_widget_t *widget__hit_test(cgl_widget_t *w, cgl_point_t pan,
                                     cgl_rect_t clip, int16_t x, int16_t y)
{
    cgl_widget_t *child;
    cgl_widget_t *found = NULL;
    cgl_point_t p;
    cgl_rect_t box;
    cgl_rect_t inner = clip;

    if ((w == NULL) || ((w->flags & CGL_WF_VISIBLE) == 0u))
    {
        return NULL;
    }

    /* A deformed subtree is painted somewhere else entirely (see the note in
       cgl_widget.h). Answering "hit" from the undeformed frame would be a hit on
       pixels that are not on screen, so the walker declines the whole subtree
       instead of guessing. Input is locked during a transition, which is the only
       thing in the framework that installs a warp, so this is a safety net. */
    if (w->warp != NULL)
    {
        return NULL;
    }

    p.x = (int16_t)(pan.x + w->offset.x);
    p.y = (int16_t)(pan.y + w->offset.y);
    box = cgl_rect_translate(w->frame, p);

    if ((w->flags & CGL_WF_CLIP) != 0u)
    {
        /* With CGL_WF_CLIP, neither this widget nor its subtree can put a pixel
           outside this frame -- so the region the subtree may be hit in shrinks to
           the intersection with the clip window inherited from the ancestors. */
        inner = cgl_rect_intersect(clip, box);
        if (!cgl_rect_contains_point(inner, x, y))
        {
            return NULL;
        }
    }

    /* Without CGL_WF_CLIP the widget is allowed to draw outside its own frame, so
       the point does not have to be inside `box` for its children to be hit --
       only for the widget itself (handled by the final return below). */

    /* The **last** hit among the siblings wins: the draw order is the insertion
       order and the last one drawn is on top, so hit testing must agree with it;
       otherwise what you click is not the widget you see. */
    for (child = w->first_child; child != NULL; child = child->next)
    {
        cgl_widget_t *hit = widget__hit_test(child, p, inner, x, y);
        if (hit != NULL)
        {
            found = hit;
        }
    }
    if (found != NULL)
    {
        return found;
    }
    return cgl_rect_contains_point(box, x, y) ? w : NULL;
}

cgl_widget_t *cgl_widget_hit_test(cgl_widget_t *w, int16_t x, int16_t y)
{
    cgl_point_t zero;
    cgl_rect_t screen;

    zero.x = 0;
    zero.y = 0;
    /* The root's clip window is the whole panel: hit testing is in screen
       coordinates (see cgl_widget_dispatch_event()). */
    screen = cgl_rect_wh(0, 0, (int16_t)CGL_CFG_SCREEN_W, (int16_t)CGL_CFG_SCREEN_H);
    return widget__hit_test(w, zero, screen, x, y);
}

bool cgl_widget_dispatch_event(cgl_widget_t *w, const cgl_event_t *ev)
{
    cgl_widget_t *child;

    if ((w == NULL) || (ev == NULL) || ((w->flags & CGL_WF_VISIBLE) == 0u))
    {
        return false;
    }
    if ((w->vt != NULL) && (w->vt->event != NULL) && w->vt->event(w, ev))
    {
        return true;
    }
    for (child = w->first_child; child != NULL; child = child->next)
    {
        if (cgl_widget_dispatch_event(child, ev))
        {
            return true;
        }
    }
    return false;
}
