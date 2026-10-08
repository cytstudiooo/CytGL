// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_widget.h
  * @brief   LCD UI framework - retained mode widget tree.
  *
  *  Widgets form a parent/child tree of absolutely positioned rectangles.
  *  A widget only has to know how to paint itself into a canvas; the tree
  *  walker takes care of visibility, band clipping and culling.
  *
  *  Invalidation goes through a damage sink so that a widget never needs to
  *  know whether it lives on a screen, in an off-screen buffer or nowhere.
  *
  *  offset -- translating a whole subtree
  *  -------------------------------------
  *  Every widget carries an offset (dx, dy); the tree walker adds that offset to
  *  the canvas pan on entry and restores it on exit. So "slide the whole page out
  *  to the left" is just setting an offset on the page's root widget -- no child
  *  frame has to change, no tree has to be rebuilt, and culling and hit testing
  *  stay correct automatically.
  *
  *  WARNING: offset affects "this one widget and its whole subtree", not just the
  *  children. A page root widget's frame is usually the whole screen, so when it
  *  slides its own background moves with it, which is the correct behaviour.
  ******************************************************************************
  */
#ifndef CGL_WIDGET_H
#define CGL_WIDGET_H

#include <cytgl/cgl_types.h>
#include <cytgl/cgl_canvas.h>

#define CGL_WF_VISIBLE 0x0001u /**< painted and traversed                    */
#define CGL_WF_CLIP 0x0002u    /**< children are clipped to this frame       */

/* Bit 0x0004 used to be CGL_WF_OPAQUE ("this widget paints its whole frame").
 * It was set by cgl_widget_init() and read by **nothing**: the band walker paints
 * every level of the tree from the root for every band and paints children in
 * insertion order, so "this level covers that one" could at best skip a sibling
 * painted *before* it -- which has already been painted by the time the flag is
 * known. Exploiting it for real needs a coverage mask, and the entire point of the
 * band pipeline is that there is nowhere to keep one. A flag that reads like a
 * promise and does nothing is worse than no flag, so it was removed rather than
 * left set: any code that still names it now fails to compile instead of getting a
 * silently wrong answer. The bit is deliberately left unassigned. */

/** Upper bound of `cgl_widget_set_alpha()`: fully opaque, and the default every widget is initialised with. */
#define CGL_ALPHA_OPAQUE 255u

typedef struct cgl_widget cgl_widget_t;

typedef struct
{
    const char *type;
    void (*draw)(cgl_widget_t *w, cgl_canvas_t *c);
    void (*update)(cgl_widget_t *w, uint32_t dt_ms);
    /**
     * Input event callback, optional (NULL = not handled).
     * @return true means "I consumed it" and the walk stops; false passes it on.
     *
     * WARNING: write the vtable with **designated initialisers** (.type = /
     * .draw = ...), never positional ones: this struct will grow (this round
     * already added event), and positional initialisers silently shift when a
     * member is added.
     */
    bool (*event)(cgl_widget_t *w, const cgl_event_t *ev);
} cgl_widget_vt_t;

struct cgl_widget
{
    const cgl_widget_vt_t *vt;
    const char *name;
    cgl_rect_t frame; /**< absolute screen rectangle                        */
    cgl_point_t offset; /**< translation of this subtree (transition / scroll / shake) */
    const cgl_warp_t *warp; /**< non-NULL = this subtree is drawn through the "suck in" deformation (used by transitions) */
    cgl_widget_t *parent;
    cgl_widget_t *first_child;
    cgl_widget_t *last_child;
    cgl_widget_t *next;
    uint16_t flags;
    /**
     * **Group opacity** of this subtree: 0 = fully transparent (not a single
     * pixel of the whole subtree is drawn), CGL_ALPHA_OPAQUE(255) = fully opaque
     * (the default).
     *
     * Placing it after `flags` is **deliberate**: that spot was already 2 bytes
     * of alignment padding, so this field costs 0 bytes (`ptype /o cgl_widget_t`
     * in `arm-none-eabi-gdb` confirms it is still 48 B, with the hole at offset
     * 42..43).
     *
     * The semantics are "group", not "per primitive": the whole widget (its own
     * draw plus the entire subtree) is rendered in isolation first and then
     * composited onto the background as a unit by alpha. So two overlapping child
     * widgets are **not** blended twice where they overlap -- that is exactly the
     * difference from "multiply an alpha into every primitive", and the reason
     * combinations like "a panel plus a translucent highlight inside it" look
     * right.
     *
     * WARNING: it does not inherit: setting alpha on a parent and then nesting a
     * child that also has alpha set gives **two** independent groups (each
     * consuming one scratch level, see CGL_CFG_ALPHA_LEVELS), not a product.
     *
     * WARNING: alpha = 0 only means "do not draw"; it does **not** affect
     * CGL_WF_VISIBLE, hit testing or event dispatch: a fully transparent widget
     * is still hit by hit_test. To take it out entirely, use
     * cgl_widget_set_visible(w, false).
     */
    uint8_t alpha_q8;
    void *user;
};

typedef void (*cgl_damage_fn)(cgl_rect_t r, void *user);

/** Install the single damage sink used by cgl_widget_invalidate(). */
void cgl_widget_set_damage_sink(cgl_damage_fn fn, void *user);

/** Report a rectangle that has to be repainted. */
void cgl_widget_invalidate_rect(cgl_rect_t r);

/**
 * Report a widget frame that has to be repainted.
 *
 * WARNING: what is reported is the **untranslated frame** (a widget does not know
 * its ancestors' offset). Subtrees with an offset rely on the caller to mark
 * damage as a fallback during animation -- the navigator marks the whole screen
 * dirty every frame while changing page, so the framework itself is fine; anyone
 * writing their own offset animation has to mark the whole active area dirty
 * themselves.
 */
void cgl_widget_invalidate(const cgl_widget_t *w);

void cgl_widget_init(cgl_widget_t *w, const cgl_widget_vt_t *vt,
                    const char *name, cgl_rect_t frame);

/**
 * Append `child` at the end of `parent`'s child list -- that is, draw it **on top**
 * of its earlier siblings. Draw order = the order of the child list.
 *
 * @return false when the child was **not** attached: a NULL argument, a child that
 *         already has a parent (detach it with `cgl_widget_remove_child()` first --
 *         silently re-parenting would corrupt the old list), or a parent that
 *         already has `CGL_CFG_WIDGET_MAX_CHILDREN` children.
 *
 * The limit is not advisory. It used to be (`CGL_CFG_WIDGET_MAX_CHILDREN` was
 * defined in `cytgl_config.h` and read by nothing), which meant a page that
 * outgrew it linked, ran and simply lost its last widgets with no diagnostic at
 * all -- exactly the class of failure the band pipeline makes hard to see. Every
 * rejected attach is counted in `cgl_widget_children_dropped()`.
 *
 * The child list is a singly linked list built by appending, so the limit is
 * checked by walking it: O(n) with n <= 8..16, on tree construction only (a page
 * builds its tree once), and it costs no RAM -- a stored child count would have to
 * grow `cgl_widget_t`, which is a per-widget price paid by every consumer.
 */
bool cgl_widget_add_child(cgl_widget_t *parent, cgl_widget_t *child);

void cgl_widget_remove_child(cgl_widget_t *child);
void cgl_widget_set_frame(cgl_widget_t *w, cgl_rect_t frame);
void cgl_widget_set_visible(cgl_widget_t *w, bool visible);

/**
 * Set the translation of this subtree. It marks both the area covered "before the
 * move" and "after the move" as damaged, so calling it alone is enough (as long
 * as the parent offset is 0 -- see the note on cgl_widget_invalidate).
 */
void cgl_widget_set_offset(cgl_widget_t *w, int16_t dx, int16_t dy);

/**
 * Set the **deformation** of this subtree (the macOS Genie style "suck in").
 * Pass NULL to go back to ordinary drawing.
 *
 * Like offset it applies to "the whole subtree": the walker installs the
 * descriptor into the canvas on entering this level and restores it on exit. The
 * difference is that offset only changes a linear displacement, whereas the
 * deformation recomputes the landing point per row non-linearly -- so culling can
 * no longer use the source frame (when a page is sucked to the other end of the
 * screen the two do not overlap at all); the walker uses the canvas's conservative
 * destination bounding box instead.
 *
 * WARNING: the descriptor itself is held by the caller (during a page transition
 * it is a member of the navigator); the widget only stores a pointer.
 */
void cgl_widget_set_warp(cgl_widget_t *w, const cgl_warp_t *warp);

/**
 * Move `w` to the last position among its siblings -- that is, draw it **on top**.
 * Draw order = the order of the child list, so "whoever is last is on top".
 */
void cgl_widget_raise(cgl_widget_t *w);

/**
 * Set the group opacity of this subtree (0..255, see the note on `alpha_q8`).
 *
 * Passing `CGL_ALPHA_OPAQUE` simply means "back to fully opaque" and takes the
 * zero-cost path (one comparison), so a UI that does not use this API pays
 * nothing in time.
 *
 * The region rendered in isolation is "where this level may put pixels" (its own
 * frame when `CGL_WF_CLIP` is set, otherwise the parent's clip window), so setting
 * alpha on a widget that does **not** have `CGL_WF_CLIP` makes the scratch cover
 * the whole clip window -- correct, but more expensive. To save, keep
 * `CGL_WF_CLIP` (it is on by default in `cgl_widget_init()`).
 *
 * WARNING: marking damage only covers `w->frame` (like the framework's other
 * setters). If this widget deliberately draws outside its frame (an overlay that
 * cleared `CGL_WF_CLIP`), it has to mark damage itself once more after changing
 * alpha.
 *
 * WARNING: it only takes effect when the canvas has scratch (`CGL_CFG_ALPHA` +
 * `cgl_display_present()`). With no scratch, or when the nesting levels are
 * exhausted, alpha is **ignored** and counted in `cgl_widget_alpha_dropped()` --
 * nothing crashes, it is just drawn opaque.
 */
void cgl_widget_set_alpha(cgl_widget_t *w, uint8_t alpha);

/** Current alpha (returns CGL_ALPHA_OPAQUE when there is no widget). */
uint8_t cgl_widget_get_alpha(const cgl_widget_t *w);

/**
 * How many times in total "alpha was set but could not take effect" (no scratch /
 * nesting levels exhausted).
 *
 * WARNING: the unit of measure is "**per band x per traversal**": the widget tree
 * is repainted once per band, so a widget spanning 10 bands adds 10, not 1. What
 * it answers is "was alpha dropped", not "how many widgets were dropped".
 *
 * Same pattern as `cgl_page_nav_dropped()` / `cgl_screen_dropped_events()`:
 * observable, so that "the effect did not show up" does not have to be guessed by
 * eye. Under a normal configuration it should always be 0.
 */
uint32_t cgl_widget_alpha_dropped(void);

/**
 * How many `cgl_widget_add_child()` calls were **rejected** because the parent
 * already had `CGL_CFG_WIDGET_MAX_CHILDREN` children (see the note on
 * `cgl_widget_add_child`).
 *
 * Same pattern as `cgl_widget_alpha_dropped()`: observable, so "a widget of mine is
 * missing" does not have to be guessed. Under a normal configuration it is 0.
 */
uint32_t cgl_widget_children_dropped(void);

/** Paint `w` and its subtree into the band canvas. */
void cgl_widget_draw_tree(cgl_widget_t *w, cgl_canvas_t *c);

/** Advance animations of `w` and its subtree. */
void cgl_widget_update_tree(cgl_widget_t *w, uint32_t dt_ms);

/**
 * Deepest visible widget containing the point, NULL when none.
 *
 * Two things this walker deliberately does **not** answer for, both now stated
 * instead of being silently wrong:
 *
 *  - **Clipping is honoured.** A widget with `CGL_WF_CLIP` constrains its children
 *    to its frame, so a descendant that draws outside an ancestor's clip window is
 *    not hit there either. (It used to be ignored: a child could draw outside its
 *    parent's clip and still be hit, so "what you click is not what you see".)
 *    A widget **without** `CGL_WF_CLIP` may draw outside its frame, and its
 *    children stay reachable there -- that is the documented difference between the
 *    two, not an oversight.
 *  - **A deformed (`cgl_widget_set_warp`) subtree is not hit at all.** The warp
 *    moves pixels somewhere else and this walker has no inverse of it; reporting a
 *    hit from the undeformed frame would be a hit on pixels that are not on screen.
 *    During a page transition input is locked anyway (`cgl_page_nav_event()`), so
 *    today this is a safety net rather than a behaviour anyone should rely on.
 */
cgl_widget_t *cgl_widget_hit_test(cgl_widget_t *w, int16_t x, int16_t y);

/**
 * Depth-first dispatch of one input event (parent before child, siblings in
 * insertion order). The first widget that returns true "consumes" the event.
 * Returning false means the whole subtree left it unhandled.
 */
bool cgl_widget_dispatch_event(cgl_widget_t *w, const cgl_event_t *ev);

#endif /* CGL_WIDGET_H */
