// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_page.c
  * @brief   LCD UI framework - pages and the navigation stack.
  *
  *  Exactly one place in a frame changes the widget tree: the start of
  *  cgl_page_nav_frame(). Every other API only records an intent. That
  *  invariant is the whole guarantee that a page change never draws halfway
  *  through.
  ******************************************************************************
  */
#include <cytgl/cgl_page.h>

/* -------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* -------------------------------------------------------------------------- */

static cgl_page_t *nav__top(const cgl_page_nav_t *n)
{
    return (n->depth > 0u) ? n->stack[n->depth - 1u] : NULL;
}

static int nav__index_of(const cgl_page_nav_t *n, const cgl_page_t *p)
{
    uint8_t i;

    for (i = 0u; i < n->depth; i++)
    {
        if (n->stack[i] == p)
        {
            return (int)i;
        }
    }
    return -1;
}

static void nav__show(cgl_page_t *p, bool visible)
{
    if ((p != NULL) && (p->root != NULL))
    {
        cgl_widget_set_visible(p->root, visible);
    }
}

/** Whether this page declares "while I am pushed on top, keep drawing the page
    underneath" (a semi-transparent overlay). */
static bool nav__shows_below(const cgl_page_t *p)
{
    return (p != NULL) && ((p->flags & CGL_PAGE_F_SHOW_BELOW) != 0u);
}

static uint8_t nav__default_trans(uint8_t kind)
{
    switch (kind)
    {
    case CGL_NAV_PUSH:
        /* Forward = the new window "grows" out of the Dock */
        return (uint8_t)CGL_TRANS_GENIE_IN;
    case CGL_NAV_POP:
        /* Back = the current page is sucked back into the Dock, revealing the
           page below (macOS minimise) */
        return (uint8_t)CGL_TRANS_GENIE_OUT;
    case CGL_NAV_REPLACE:
        /* The old page is sucked away and the new one takes its place in situ */
        return (uint8_t)CGL_TRANS_GENIE_OUT;
    default:
        return (uint8_t)CGL_TRANS_NONE; /* RESET: clear the stack back to the main
                                           screen, an instant switch feels more
                                           natural */
    }
}

/** Whether this transition is a "suction" one (using warp instead of offset). */
static bool nav__is_genie(uint8_t trans)
{
    /* S_OUT / S_IN are the **same geometry** with a different suction point plus
       a lateral trajectory: they take exactly the same code path, lifecycle and
       layering rules, so they must be in this test together -- missing one means
       "this whole class of transitions neither pans nor warps", i.e. the page
       "acts out" the entire transition without moving at all. */
    return (trans == (uint8_t)CGL_TRANS_GENIE_OUT) || (trans == (uint8_t)CGL_TRANS_GENIE_IN) ||
           (trans == (uint8_t)CGL_TRANS_S_OUT) || (trans == (uint8_t)CGL_TRANS_S_IN);
}

/**
 * How "bulging" the lateral S trajectory of this class is, in Q8; 0 = not
 * bulging (a straight line, i.e. the two Genie ones).
 *
 * The amplitude is taken as "half the screen width": that is the lateral
 * distance the source centre really has to travel to the destination centre when
 * "the page is sucked to the right edge". It is both a geometric quantity and
 * exactly inside the safe monotonic region (see the derivation in
 * cgl_config.h), so no per-screen-width tuning is needed.
 */
static uint16_t nav__s_amp(int16_t src_w)
{
    uint32_t amp = ((uint32_t)src_w * 256u) / (2u * 256u);

    if (amp > (uint32_t)CGL_CFG_S_AMP_MAX_Q8)
    {
        amp = (uint32_t)CGL_CFG_S_AMP_MAX_Q8;
    }
    return (uint16_t)amp;
}

/** Duration of one transition class (milliseconds). An out-of-range kind (which
    should not happen) falls back to the global default instead of crashing on
    the spot. */
static uint16_t nav__duration(const cgl_page_nav_t *n, uint8_t trans)
{
    if (trans >= (uint8_t)CGL_TRANS_COUNT)
    {
        return (uint16_t)CGL_CFG_TRANSITION_MS;
    }
    return n->duration_ms[trans];
}

/**
 * Easing curve of one transition class. Like nav__duration() it is "the single
 * read port of the table" (plus a defence: a corrupted table falls back to the
 * global default rather than feeding an out-of-range enum to cgl_anim_start()).
 */
static cgl_ease_t nav__ease(const cgl_page_nav_t *n, uint8_t trans)
{
    if ((trans >= (uint8_t)CGL_TRANS_COUNT) || (n->ease[trans] >= (uint8_t)CGL_EASE_COUNT))
    {
        return (cgl_ease_t)CGL_CFG_TRANSITION_EASE;
    }
    return (cgl_ease_t)n->ease[trans];
}

/** Attach this page's root widget to the host, making sure it sits above the
    other pages and the mask sits above it in turn. */
static void nav__attach(cgl_page_nav_t *n, cgl_page_t *p, void *arg)
{
    p->nav = n;
    p->arg = arg;

    if ((p->flags & CGL_PAGE_F_CREATED) == 0u)
    {
        if ((p->vt != NULL) && (p->vt->on_create != NULL))
        {
            p->vt->on_create(p);
        }
        p->flags |= CGL_PAGE_F_CREATED;
    }
    if (p->root == NULL)
    {
        return; /* pages without widgets (e.g. logic-only pages) are allowed too */
    }

    /* Any offset/warp left over from the previous transition must be cleared,
       otherwise the page stops at the position it slid to halfway last time, or
       keeps being sucked. */
    cgl_widget_set_offset(p->root, 0, 0);
    cgl_widget_set_warp(p->root, NULL);
    nav__show(p, true);

    if (p->root->parent == NULL)
    {
        cgl_widget_add_child(&n->host, p->root);
    }
    cgl_widget_raise(p->root);

    /* The mask is always on top: a FADE transition has to cover the new page
       that was just attached. */
    cgl_widget_raise(&n->fade.base);
}

/** Detach a page completely: the widget tree leaves the host + on_destroy. After
    that the same block of static memory can be created again. */
static void nav__detach_destroy(cgl_page_nav_t *n, cgl_page_t *p)
{
    (void)n;

    if (p == NULL)
    {
        return;
    }
    if (p->root != NULL)
    {
        cgl_widget_set_offset(p->root, 0, 0);
        cgl_widget_set_warp(p->root, NULL);
        nav__show(p, false);
        cgl_widget_remove_child(p->root);
    }
    if ((p->flags & CGL_PAGE_F_CREATED) != 0u)
    {
        if ((p->vt != NULL) && (p->vt->on_destroy != NULL))
        {
            p->vt->on_destroy(p);
        }
        p->flags &= (uint16_t)~CGL_PAGE_F_CREATED;
    }
    p->nav = NULL;
    p->arg = NULL;
}

/* -------------------------------------------------------------------------- */
/* Transition layout                                                          */
/* -------------------------------------------------------------------------- */

/**
 * Compute the displacements of both pages for the current eased progress and
 * write them into the widgets' offset.
 *
 * * The displacement is written on the **page root widget**, and no child is
 *   touched at all -- the tree walker adds the offset to the canvas pan when it
 *   enters this level (see cgl_widget_draw_tree), so culling and hit testing
 *   follow along automatically. That is why "page slide" needs only a dozen
 *   lines.
 *
 * GENIE_* is the non-linear version of the same idea: the displacement becomes a
 * canvas warp and the whole subtree recomputes its landing rows (see the top of
 * cgl_canvas.h). The two paths are mutually exclusive, and both only touch the
 * "page root widget" level.
 */
static void nav__layout(cgl_page_nav_t *n)
{
    cgl_page_trans_t *t = &n->trans;
    int32_t p = cgl_anim_value(&t->anim); /* 0..256, already eased */
    int16_t w = cgl_rect_width(n->area);
    int16_t h = cgl_rect_height(n->area);
    int16_t fdx = 0;
    int16_t fdy = 0;
    int16_t tdx = 0;
    int16_t tdy = 0;

    if (nav__is_genie(t->trans))
    {
        /* OUT: the departing page is sucked in (progress 0 -> 256); IN: the
           arriving page pours out of the suction point (256 -> 0). Both
           directions share the same curve, so "playing it backwards" is exactly
           the inverse process.
           WARNING: the OUT family is **four types** (the two Genie ones + the two
           S-shaped ones): their "out" direction is not all `GENIE_OUT` -- writing
           `!= GENIE_OUT` would count S_IN as an "out" too, and the pouring
           animation would run the wrong way (the page would be sucked away as it
           enters). */
        uint16_t wp = ((t->trans == (uint8_t)CGL_TRANS_GENIE_OUT) ||
                       (t->trans == (uint8_t)CGL_TRANS_S_OUT))
                          ? (uint16_t)p
                          : (uint16_t)((int32_t)CGL_ANIM_ONE_Q8 - p);

        if (wp > (uint16_t)CGL_ANIM_ONE_Q8)
        {
            wp = (uint16_t)CGL_ANIM_ONE_Q8; /* OUT_BACK overshoots, clamp it here */
        }
        n->warp.progress_q8 = wp;

        if ((t->from != NULL) && (t->from->root != NULL))
        {
            cgl_widget_set_offset(t->from->root, 0, 0);
        }
        if ((t->to != NULL) && (t->to->root != NULL))
        {
            cgl_widget_set_offset(t->to->root, 0, 0);
        }
        return;
    }

    switch (t->trans)
    {
    case CGL_TRANS_SLIDE_LEFT:
        fdx = (int16_t)cgl_anim_lerp(0, -(int32_t)w, (uint16_t)p);
        tdx = (int16_t)cgl_anim_lerp(w, 0, (uint16_t)p);
        break;
    case CGL_TRANS_SLIDE_RIGHT:
        fdx = (int16_t)cgl_anim_lerp(0, (int32_t)w, (uint16_t)p);
        tdx = (int16_t)cgl_anim_lerp(-(int32_t)w, 0, (uint16_t)p);
        break;
    case CGL_TRANS_SLIDE_UP:
        fdy = (int16_t)cgl_anim_lerp(0, -(int32_t)h, (uint16_t)p);
        tdy = (int16_t)cgl_anim_lerp(h, 0, (uint16_t)p);
        break;
    case CGL_TRANS_SLIDE_DOWN:
        fdy = (int16_t)cgl_anim_lerp(0, (int32_t)h, (uint16_t)p);
        tdy = (int16_t)cgl_anim_lerp(-(int32_t)h, 0, (uint16_t)p);
        break;
    case CGL_TRANS_COVER_UP:
        tdy = (int16_t)cgl_anim_lerp(h, 0, (uint16_t)p);
        break;
    case CGL_TRANS_COVER_DOWN:
        tdy = (int16_t)cgl_anim_lerp(-(int32_t)h, 0, (uint16_t)p);
        break;
    case CGL_TRANS_FADE:
        /* The first half shows the old page and the second half the new one, and
           at the midpoint the whole screen is exactly the background colour
           (alpha=256), so the page switch is **invisible** -- that is why no
           real cross-fade is needed: a cross-fade would require blending two
           pages with varying weights, and the band renderer has no second
           full-screen buffer. */
        t->fade_q8 = (p < 128) ? (uint16_t)(2 * p) : (uint16_t)(2 * (256 - p));
        nav__show(t->from, p < 128);
        nav__show(t->to, p >= 128);
        break;
    default:
        break;
    }

    if ((t->from != NULL) && (t->from->root != NULL))
    {
        cgl_widget_set_offset(t->from->root, fdx, fdy);
    }
    if ((t->to != NULL) && (t->to->root != NULL))
    {
        cgl_widget_set_offset(t->to->root, tdx, tdy);
    }
}

static void nav__finish(cgl_page_nav_t *n)
{
    cgl_page_trans_t *t = &n->trans;

    switch (t->kind)
    {
    case CGL_NAV_PUSH:
        /* The old page is merely stacked under: its widget tree is kept, and
           on_resume simply uncovers it again on the way back.
           * But if the **new page** declares "I want the page below to show
           through" (a semi-transparent overlay), it must **not** be hidden --
           hiding means the whole subtree is not drawn, and what showed through
           the overlay would be nothing but the host background colour.
           Note that this is only "keep drawing": the old page still receives no
           on_step (physics stays frozen as before). */
        if (!nav__shows_below(t->to))
        {
            nav__show(t->from, false);
        }
        break;
    case CGL_NAV_POP:
    case CGL_NAV_REPLACE:
        nav__detach_destroy(n, t->from);
        break;
    default:
        break;
    }

    if ((t->from != NULL) && (t->from->root != NULL))
    {
        cgl_widget_set_offset(t->from->root, 0, 0);
        cgl_widget_set_warp(t->from->root, NULL);
    }
    if (t->to != NULL)
    {
        if (t->to->root != NULL)
        {
            cgl_widget_set_offset(t->to->root, 0, 0);
            cgl_widget_set_warp(t->to->root, NULL);
            /* During a suction transition the sucked page is raised to the top;
               restore the layering when finishing, otherwise "the page below"
               would stay on top of "the page above" forever. */
            cgl_widget_raise(t->to->root);
        }
        nav__show(t->to, true);
    }
    n->warp.progress_q8 = 0u;
    cgl_widget_raise(&n->fade.base);

    t->from = NULL;
    t->to = NULL;
    t->active = false;
    t->fade_q8 = 0u;
    /* RESET destroys the departing page outright, so there is no "transition"
       to speak of; only count it when a transition really played. */
    if (t->trans != (uint8_t)CGL_TRANS_NONE)
    {
        n->transitions++;
    }

    cgl_widget_invalidate_rect(n->area);
}

/**
 * Set up the warp descriptor for one suction transition and attach it to the
 * page that should be sucked.
 *
 * Direction semantics:
 *   GENIE_OUT  the departing page is sucked into the suction point -- the page
 *              below must be **visible when the transition starts**, because it
 *              is "what gets uncovered once that page is gone".
 *   GENIE_IN   the arriving page pours out of the suction point -- the departing
 *              page just stays underneath as it was.
 *
 * Both directions must **raise the sucked page to the top**: it is the only
 * layer that moves, and it is only visible above the static page (on POP the
 * navigator raises "the page below" by default, which is exactly the opposite).
 */
static void nav__warp_setup(cgl_page_nav_t *n, cgl_page_t *from, cgl_page_t *to, uint8_t trans)
{
    bool out = (trans == (uint8_t)CGL_TRANS_GENIE_OUT) || (trans == (uint8_t)CGL_TRANS_S_OUT);
    bool s_shape = (trans == (uint8_t)CGL_TRANS_S_OUT) || (trans == (uint8_t)CGL_TRANS_S_IN);
    cgl_page_t *warped = out ? from : to;
    cgl_page_t *still = out ? to : from;
    int16_t tx = n->genie_x;
    int16_t ty = n->genie_y;

    if (s_shape)
    {
        /* * The suction point of the S shape is **part of the type**, not the
           `genie_x/y` cell: the latter is the tunable "where does the Genie suck
           to", whereas "suck to the bottom-right corner + take the S path" is
           the definition of this class. Separating the two would let
           `set_genie_target()` suck an S into the middle of the screen -- which
           is no longer an S trajectory, just "a weird thing that turns a
           corner".
           The centre column is area.x1: at the end the page centre lands exactly
           on the right edge (any further right and it is entirely off screen),
           so both directions share the same S -- one running forwards, one
           backwards. */
        tx = n->area.x1;
        ty = (int16_t)((int32_t)n->area.y1 + (int32_t)CGL_CFG_S_DROP_PX);
    }

    n->warp.src_y0 = n->area.y0;
    n->warp.src_h = cgl_rect_height(n->area);
    n->warp.cx = (int16_t)((int32_t)n->area.x0 + ((int32_t)cgl_rect_width(n->area) / 2));
    n->warp.tx = tx;
    n->warp.ty = ty;
    n->warp.narrow_q8 = (uint16_t)CGL_CFG_GENIE_NARROW_Q8;
    n->warp.s_amp_q8 = s_shape ? nav__s_amp(cgl_rect_width(n->area)) : 0u;
    n->warp.progress_q8 = out ? 0u : (uint16_t)CGL_ANIM_ONE_Q8;
    /* Normalised step per source row (Q8, i.e. 1/height). Round to nearest,
       otherwise the topmost row cannot reach 256 (the far-end width factor would
       be off by a hair -- invisible to the eye, but the test computes it). */
    n->warp.row_step_q8 = (uint16_t)((((uint32_t)256u * 256u) + ((uint32_t)n->warp.src_h / 2u)) /
                                     (uint32_t)n->warp.src_h);

    if (still != NULL)
    {
        nav__show(still, true);
    }
    if ((warped != NULL) && (warped->root != NULL))
    {
        cgl_widget_set_warp(warped->root, &n->warp);
        cgl_widget_raise(warped->root);
    }
    cgl_widget_raise(&n->fade.base);
}

/**
 * The transition preference of the page that "moves": **within one navigation
 * only its declaration has a say**.
 *   PUSH          -> the enter preference of the arriving page (to)
 *   POP / REPLACE -> the leave preference of the departing page (from)
 *   RESET         -> none: RESET is "go back to the main screen" (a restart, not
 *                    a transition), and the framework pins it to NONE inside
 *                    nav__apply(), so page declarations take no part
 * Returning NULL = there is no page to ask this time (from/to is NULL, or
 * RESET).
 */
static const cgl_page_trans_cfg_t *nav__subject_fx(uint8_t kind, cgl_page_t *from, cgl_page_t *to)
{
    switch (kind)
    {
    case CGL_NAV_POP:
    case CGL_NAV_REPLACE:
        return (from == NULL) ? NULL : &from->leave;
    case CGL_NAV_PUSH:
        return (to == NULL) ? NULL : &to->enter;
    default:
        return NULL;
    }
}

/** Start a transition; when trans == NONE (or this class has duration 0) it
    finishes immediately. */
static void nav__start(cgl_page_nav_t *n, cgl_page_t *from, cgl_page_t *to,
                       uint8_t kind, uint8_t caller_trans)
{
    cgl_page_trans_t *t = &n->trans;
    /* The three transition decisions fall through in order: **the page's
       declaration -> what the caller passed -> the per-action default / type
       table**. No "per concrete type" branch is allowed here -- to change the
       feel of one class call cgl_page_nav_set_transition_{duration,ease}(), to
       change the feel of one page call cgl_page_set_trans_enter/leave(), and do
       not add an if here. */
    const cgl_page_trans_cfg_t *fx = nav__subject_fx(kind, from, to);
    uint8_t trans = caller_trans;
    uint16_t dur;
    cgl_ease_t ease;

    /* * The caller passing NONE **explicitly** = "no animation this time", and it
       is the only input that can override a page declaration.
       Why: NONE is not a "style" (it does not answer "which animation"), it is
       "no animation". Handing it to "the page declaration wins" as well would
       mean there is **no way at all to express "switch this page instantly"** --
       and that is a real need: snapshot tests need to capture a stable frame
       "without a transition", and an application laying out its first screen
       should not play a transition first either.
       A page that wants to pin itself to an instant switch can still declare
       fx->trans = NONE (which takes route 1 below). */
    if (trans != (uint8_t)CGL_TRANS_NONE)
    {
        if ((fx != NULL) && (fx->trans != (uint8_t)CGL_TRANS_AUTO) &&
            (fx->trans < (uint8_t)CGL_TRANS_COUNT))
        {
            trans = fx->trans; /* 1) the page declared which one to use itself */
        }
        else if (trans == (uint8_t)CGL_TRANS_AUTO)
        {
            trans = nav__default_trans(kind); /* 3) nobody said -> per-action default */
        }
    }

    dur = ((fx != NULL) && (fx->dur_ms != (uint16_t)CGL_PAGE_FX_KEEP_MS)) ? fx->dur_ms
                                                                        : nav__duration(n, trans);
    ease = ((fx != NULL) && (fx->ease < (uint8_t)CGL_EASE_COUNT))
               ? (cgl_ease_t)fx->ease
               : nav__ease(n, trans);

    t->from = from;
    t->to = to;
    t->kind = kind;
    t->trans = trans;
    t->fade_q8 = 0u;

    if ((trans == (uint8_t)CGL_TRANS_NONE) || (dur == 0u))
    {
        t->active = false;
        nav__finish(n);
        return;
    }

    if (nav__is_genie(trans))
    {
        /* Genie only does one thing more than sliding: install the warp
           descriptor. Duration and curve still come from the fall-through
           above. */
        nav__warp_setup(n, from, to, trans);
    }

    t->active = true;
    cgl_anim_init(&t->anim);
    /* Use the 0..256 "progress" as the tween quantity instead of tweening the
       displacements: one curve then drives both pages (one leaving, one
       arriving), which makes desynchronisation impossible. */
    cgl_anim_start(&t->anim, 0, (int32_t)CGL_ANIM_ONE_Q8, (uint32_t)dur, ease);
    nav__layout(n);
    cgl_widget_invalidate_rect(n->area);
}

/* -------------------------------------------------------------------------- */
/* Navigator public API                                                        */
/* -------------------------------------------------------------------------- */

static void fade__draw(cgl_widget_t *w, cgl_canvas_t *c);

static const cgl_widget_vt_t fade__vt = {
    .type = "page_fade",
    .draw = fade__draw,
    .update = NULL,
    .event = NULL,
};

static void fade__draw(cgl_widget_t *w, cgl_canvas_t *c)
{
    cgl_page_fade_t *f = (cgl_page_fade_t *)w;

    if ((f->nav == NULL) || !f->nav->trans.active ||
        (f->nav->trans.trans != (uint8_t)CGL_TRANS_FADE))
    {
        return; /* without a FADE transition this mask takes no part in drawing */
    }
    cgl_canvas_fill_rect_blend_q8(c, w->frame, CGL_CFG_BG_COLOR, f->nav->trans.fade_q8);
}

/** The host's draw: flood the whole area with the background colour; that is the
    "floor" revealed while sliding. */
static void host__draw(cgl_widget_t *w, cgl_canvas_t *c)
{
    cgl_canvas_fill_rect(c, w->frame, CGL_CFG_BG_COLOR);
}

static const cgl_widget_vt_t host__vt = {
    .type = "page_host",
    .draw = host__draw,
    .update = NULL,
    .event = NULL,
};

/** Clear a transition preference to "nothing declared". */
static void page__fx_clear(cgl_page_trans_cfg_t *c)
{
    c->trans = (uint8_t)CGL_TRANS_AUTO;
    c->ease = (uint8_t)CGL_PAGE_FX_KEEP_EASE;
    c->dur_ms = (uint16_t)CGL_PAGE_FX_KEEP_MS;
}

/** Validate (trans, ease, dur_ms) and write them into one transition preference.
    The three parameters describe the complete preference together. */
static void page__fx_set(cgl_page_trans_cfg_t *c, cgl_trans_t trans, cgl_ease_t ease, uint32_t dur_ms)
{
    if ((uint8_t)trans >= (uint8_t)CGL_TRANS_COUNT)
    {
        return; /* out-of-range type: leave the whole call alone, do not leave a
                   "half-changed" preference behind */
    }
    if ((uint8_t)ease > (uint8_t)CGL_EASE_COUNT)
    {
        ease = CGL_EASE_LINEAR; /* out-of-range curve: clamp exactly like the nav
                                   type table and cgl_anim_start() do */
    }
    if (dur_ms > 0xFFFFu)
    {
        dur_ms = 0xFFFFu;
    }
    c->trans = (uint8_t)trans;
    c->ease = (uint8_t)ease;
    c->dur_ms = (uint16_t)dur_ms;
}

void cgl_page_init(cgl_page_t *p, const cgl_page_vt_t *vt, const char *name, void *user)
{
    if (p == NULL)
    {
        return;
    }
    p->vt = vt;
    p->name = name;
    p->root = NULL;
    p->nav = NULL;
    p->user = user;
    p->arg = NULL;
    p->flags = 0u;
    /* Transition preferences default to "nothing declared": so the behaviour is
       exactly the same as without this mechanism (the type comes from the caller
       / the per-action default, and the feel from the navigator's two type
       tables). */
    page__fx_clear(&p->enter);
    page__fx_clear(&p->leave);
}

void cgl_page_set_trans_enter(cgl_page_t *p, cgl_trans_t trans, cgl_ease_t ease, uint32_t dur_ms)
{
    if (p != NULL)
    {
        page__fx_set(&p->enter, trans, ease, dur_ms);
    }
}

void cgl_page_set_trans_leave(cgl_page_t *p, cgl_trans_t trans, cgl_ease_t ease, uint32_t dur_ms)
{
    if (p != NULL)
    {
        page__fx_set(&p->leave, trans, ease, dur_ms);
    }
}

void cgl_page_set_root(cgl_page_t *p, cgl_widget_t *root)
{
    if (p != NULL)
    {
        p->root = root;
    }
}

uint8_t cgl_page_register(const cgl_page_desc_t *pages, uint8_t count,
                          const cgl_page_ctx_t *ctx)
{
    uint8_t i;
    uint8_t created = 0u;

    if ((pages == NULL) || (ctx == NULL))
    {
        return 0u;
    }

    /* In table order, deliberately: a page's attach only *stores* what it needs
       (the context, and cross-page links it reads at run time), so no page depends
       on another having been created first. Keeping the order meaningful -- "the
       home page first" -- is worth more than any parallelism, because that is the
       order a reader scans the table in. */
    for (i = 0u; i < count; i++)
    {
        if ((pages[i].page == NULL) || (pages[i].attach == NULL))
        {
            continue;
        }
        pages[i].attach(pages[i].page, ctx);
        created++;
    }
    return created;
}

const cgl_page_ctx_t *cgl_page_ctx(const cgl_page_t *p)
{
    return (p == NULL) ? NULL : (const cgl_page_ctx_t *)p->user;
}

void cgl_page_set_show_below(cgl_page_t *p, bool show)
{
    if (p == NULL)
    {
        return;
    }
    if (show)
    {
        p->flags |= (uint16_t)CGL_PAGE_F_SHOW_BELOW;
    }
    else
    {
        p->flags &= (uint16_t)~CGL_PAGE_F_SHOW_BELOW;
    }
}

bool cgl_page_get_show_below(const cgl_page_t *p)
{
    return (p != NULL) && ((p->flags & CGL_PAGE_F_SHOW_BELOW) != 0u);
}

void cgl_page_nav_init(cgl_page_nav_t *n, const char *name, cgl_rect_t area)
{
    uint8_t i;

    if (n == NULL)
    {
        return;
    }

    cgl_widget_init(&n->host, &host__vt, name, area);
    n->host.flags |= CGL_WF_CLIP; /* page content must not draw off screen */
    n->area = area;

    for (i = 0u; i < (uint8_t)CGL_CFG_PAGE_STACK_MAX; i++)
    {
        n->stack[i] = NULL;
    }
    n->depth = 0u;

    n->req_page = NULL;
    n->req_arg = NULL;
    n->req_kind = 0u;
    n->req_trans = 0u;
    n->req_pending = false;
    n->req_dropped = 0u;

    n->trans.from = NULL;
    n->trans.to = NULL;
    n->trans.fade_q8 = 0u;
    n->trans.kind = 0u;
    n->trans.trans = 0u;
    n->trans.active = false;
    cgl_anim_init(&n->trans.anim);

    /* Per-class default durations: slide / cover / fade use the global value,
       suction uses its own (longer, because the warp itself needs to be seen
       clearly), and the instant-switch class is 0. */
    for (i = 0u; i < (uint8_t)CGL_TRANS_COUNT; i++)
    {
        n->duration_ms[i] = (uint16_t)CGL_CFG_TRANSITION_MS;
    }
    n->duration_ms[(uint8_t)CGL_TRANS_NONE] = 0u;
    n->duration_ms[(uint8_t)CGL_TRANS_GENIE_OUT] = (uint16_t)CGL_CFG_GENIE_MS;
    n->duration_ms[(uint8_t)CGL_TRANS_GENIE_IN] = (uint16_t)CGL_CFG_GENIE_MS;
    /* The S-shaped suction runs at the same speed as Genie: the difference in
       feel between the two suctions lies entirely in the **geometry** (suction
       point + S trajectory), not in time. Tuning them separately is of course
       allowed (set_transition_duration), but the defaults must not make the user
       feel that page changes suddenly run fast or slow just because "I switched
       the suction animation". */
    n->duration_ms[(uint8_t)CGL_TRANS_S_OUT] = (uint16_t)CGL_CFG_GENIE_MS;
    n->duration_ms[(uint8_t)CGL_TRANS_S_IN] = (uint16_t)CGL_CFG_GENIE_MS;

    /* The default curve table corresponds one-to-one with the duration table:
       slide / cover / fade share the global value, and the two suction
       directions override it individually (wind up then fling vs come to a firm
       stop, reasons in cgl_config.h). NONE is given the unshaped LINEAR -- it
       has no animation at all, and like duration_ms[NONE] = 0 this cell exists
       for observation only. */
    for (i = 0u; i < (uint8_t)CGL_TRANS_COUNT; i++)
    {
        n->ease[i] = (uint8_t)CGL_CFG_TRANSITION_EASE;
    }
    n->ease[(uint8_t)CGL_TRANS_NONE] = (uint8_t)CGL_EASE_LINEAR;
    n->ease[(uint8_t)CGL_TRANS_GENIE_OUT] = (uint8_t)CGL_CFG_GENIE_OUT_EASE;
    n->ease[(uint8_t)CGL_TRANS_GENIE_IN] = (uint8_t)CGL_CFG_GENIE_IN_EASE;
    n->ease[(uint8_t)CGL_TRANS_S_OUT] = (uint8_t)CGL_CFG_S_OUT_EASE;
    n->ease[(uint8_t)CGL_TRANS_S_IN] = (uint8_t)CGL_CFG_S_IN_EASE;

    n->transitions = 0u;

    /* Default suction point: just below the midpoint of the bottom edge, which
       reads exactly like "sucked away by the Dock".
       Being off screen is deliberate -- on the last frame the page leaves the
       visible area completely, so there is no ghost left when it finishes. */
    n->warp.src_y0 = area.y0;
    n->warp.src_h = cgl_rect_height(area);
    n->warp.cx = (int16_t)((int32_t)area.x0 + ((int32_t)cgl_rect_width(area) / 2));
    n->warp.tx = n->warp.cx;
    n->warp.ty = (int16_t)(area.y1 + CGL_CFG_GENIE_DROP_PX);
    n->warp.narrow_q8 = (uint16_t)CGL_CFG_GENIE_NARROW_Q8;
    n->warp.s_amp_q8 = 0u;
    n->warp.progress_q8 = 0u;
    n->warp.row_step_q8 = (uint16_t)((((uint32_t)256u * 256u) + ((uint32_t)n->warp.src_h / 2u)) /
                                     (uint32_t)n->warp.src_h);
    n->genie_x = n->warp.tx;
    n->genie_y = n->warp.ty;

    /* The mask hangs under the host and is always kept on the top layer (attach
       raises it). */
    cgl_widget_init(&n->fade.base, &fade__vt, "page_fade", area);
    n->fade.nav = n;
    cgl_widget_add_child(&n->host, &n->fade.base);
}

cgl_widget_t *cgl_page_nav_host(cgl_page_nav_t *n)
{
    return (n == NULL) ? NULL : &n->host;
}

cgl_page_t *cgl_page_nav_top(const cgl_page_nav_t *n)
{
    return (n == NULL) ? NULL : nav__top(n);
}

uint8_t cgl_page_nav_depth(const cgl_page_nav_t *n)
{
    return (n == NULL) ? 0u : n->depth;
}

bool cgl_page_nav_busy(const cgl_page_nav_t *n)
{
    return (n == NULL) ? false : n->trans.active;
}

bool cgl_page_nav_pending(const cgl_page_nav_t *n)
{
    return (n == NULL) ? false : n->req_pending;
}

void cgl_page_nav_set_duration(cgl_page_nav_t *n, uint32_t ms)
{
    uint8_t i;

    if (n == NULL)
    {
        return;
    }
    if (ms > 0xFFFFu)
    {
        ms = 0xFFFFu; /* uint16_t table; 65 s is already far beyond any watchable transition */
    }
    /* Start at 1: skip CGL_TRANS_NONE -- "no transition" is always instantaneous
       and must not be changed. (nav__start() tests NONE first as well, so this
       cell is only an observed value; do not let it lie.) */
    for (i = 1u; i < (uint8_t)CGL_TRANS_COUNT; i++)
    {
        n->duration_ms[i] = (uint16_t)ms;
    }
}

void cgl_page_nav_set_transition_duration(cgl_page_nav_t *n, cgl_trans_t trans, uint32_t ms)
{
    if (n == NULL)
    {
        return;
    }
    if (trans == CGL_TRANS_AUTO)
    {
        cgl_page_nav_set_duration(n, ms); /* AUTO is not "one class", treat it as "all" */
        return;
    }
    if ((uint8_t)trans >= (uint8_t)CGL_TRANS_COUNT)
    {
        return; /* out of range: ignore silently, do not corrupt memory */
    }
    if (ms > 0xFFFFu)
    {
        ms = 0xFFFFu;
    }
    n->duration_ms[(uint8_t)trans] = (uint16_t)ms;
}

uint32_t cgl_page_nav_duration(const cgl_page_nav_t *n, cgl_trans_t trans)
{
    if ((n == NULL) || ((uint8_t)trans >= (uint8_t)CGL_TRANS_COUNT))
    {
        return 0u;
    }
    return (uint32_t)n->duration_ms[(uint8_t)trans];
}

void cgl_page_nav_set_ease(cgl_page_nav_t *n, cgl_ease_t ease)
{
    uint8_t i;

    if (n == NULL)
    {
        return;
    }
    if ((uint8_t)ease >= (uint8_t)CGL_EASE_COUNT)
    {
        ease = CGL_EASE_LINEAR; /* same clamp as cgl_anim_start(): the safe default is constant speed */
    }
    /* Start at 1: skip CGL_TRANS_NONE, same reason as set_duration() -- NONE has
       no animation, that cell is only an observed value, do not let it
       contradict itself. */
    for (i = 1u; i < (uint8_t)CGL_TRANS_COUNT; i++)
    {
        n->ease[i] = (uint8_t)ease;
    }
}

void cgl_page_nav_set_transition_ease(cgl_page_nav_t *n, cgl_trans_t trans, cgl_ease_t ease)
{
    if (n == NULL)
    {
        return;
    }
    if (trans == CGL_TRANS_AUTO)
    {
        cgl_page_nav_set_ease(n, ease); /* AUTO is not "one class", treat it as "all" */
        return;
    }
    if ((uint8_t)trans >= (uint8_t)CGL_TRANS_COUNT)
    {
        return; /* out of range: ignore silently, do not corrupt memory */
    }
    if ((uint8_t)ease >= (uint8_t)CGL_EASE_COUNT)
    {
        ease = CGL_EASE_LINEAR;
    }
    n->ease[(uint8_t)trans] = (uint8_t)ease;
}

cgl_ease_t cgl_page_nav_ease(const cgl_page_nav_t *n, cgl_trans_t trans)
{
    if ((n == NULL) || ((uint8_t)trans >= (uint8_t)CGL_TRANS_COUNT))
    {
        return CGL_EASE_LINEAR;
    }
    return (cgl_ease_t)n->ease[(uint8_t)trans];
}

void cgl_page_nav_set_genie_target(cgl_page_nav_t *n, int16_t x, int16_t y)
{
    if (n != NULL)
    {
        n->genie_x = x;
        n->genie_y = y;
        /* The descriptor may be in use right now (changing the target point
           during a transition is allowed too): both must change together to stay
           consistent. */
        n->warp.tx = x;
        n->warp.ty = y;
    }
}

const cgl_warp_t *cgl_page_nav_warp(const cgl_page_nav_t *n)
{
    if ((n == NULL) || !n->trans.active || !nav__is_genie(n->trans.trans))
    {
        return NULL;
    }
    return &n->warp;
}

bool cgl_page_nav_request(cgl_page_nav_t *n, cgl_nav_kind_t kind, cgl_page_t *page,
                         cgl_trans_t trans, void *arg)
{
    if (n == NULL)
    {
        return false;
    }

    /* --- Validate at **request** time instead of at commit time -------------
       Because at most one request can be committed per frame, the pending slot
       can never let the state go stale, so the verdict here is the same as the
       one at commit time; validating early is what lets the caller get a clear
       return value. */
    switch (kind)
    {
    case CGL_NAV_PUSH:
    case CGL_NAV_REPLACE:
        if (page == NULL)
        {
            n->req_dropped++;
            return false;
        }
        if (nav__index_of(n, page) >= 0)
        {
            n->req_dropped++; /* already on the stack: pushing it again would make
                                 two entries share one widget tree */
            return false;
        }
        if ((kind == CGL_NAV_PUSH) && (n->depth >= (uint8_t)CGL_CFG_PAGE_STACK_MAX))
        {
            n->req_dropped++;
            return false; /* stack full. The caller should back() first -- silently
                             overwriting the top of the stack is worse */
        }
        break;

    case CGL_NAV_POP:
        if (n->depth <= 1u)
        {
            n->req_dropped++; /* only the root page is on the stack, there is no level above */
            return false;
        }
        break;

    case CGL_NAV_RESET:
        break;

    default:
        n->req_dropped++;
        return false;
    }

    /* The last request wins: pressing back twice in a row loses nothing and does
       not queue up a stack overflow. */
    n->req_page = page;
    n->req_arg = arg;
    n->req_kind = (uint8_t)kind;
    n->req_trans = (uint8_t)trans;
    n->req_pending = true;
    return true;
}

bool cgl_page_nav_push(cgl_page_nav_t *n, cgl_page_t *page)
{
    return cgl_page_nav_request(n, CGL_NAV_PUSH, page, CGL_TRANS_AUTO, NULL);
}

bool cgl_page_nav_replace(cgl_page_nav_t *n, cgl_page_t *page)
{
    return cgl_page_nav_request(n, CGL_NAV_REPLACE, page, CGL_TRANS_AUTO, NULL);
}

bool cgl_page_nav_reset(cgl_page_nav_t *n, cgl_page_t *page)
{
    return cgl_page_nav_request(n, CGL_NAV_RESET, page, CGL_TRANS_AUTO, NULL);
}

bool cgl_page_nav_back(cgl_page_nav_t *n)
{
    return cgl_page_nav_request(n, CGL_NAV_POP, NULL, CGL_TRANS_AUTO, NULL);
}

/* -------------------------------------------------------------------------- */
/* Frame boundary                                                             */
/* -------------------------------------------------------------------------- */

/** Commit one pending navigation. **This is the only place in the whole
    navigator that changes the widget tree.** */
static void nav__apply(cgl_page_nav_t *n)
{
    uint8_t kind = n->req_kind;
    uint8_t trans = n->req_trans;
    cgl_page_t *page = n->req_page;
    void *arg = n->req_arg;
    cgl_page_t *from;
    cgl_page_t *to;

    n->req_pending = false;
    n->req_page = NULL;
    n->req_arg = NULL;

    /* The commit of AUTO ("the page's declaration -> what the caller passed ->
       the per-action default") happens inside nav__start() -- that is the only
       place that knows who from/to are for this navigation, and the page
       preference has to be asked of the page that "moves". */

    switch (kind)
    {
    case CGL_NAV_PUSH:
        from = nav__top(n);
        to = page;
        nav__attach(n, to, arg);
        /* * Push onto the stack before the callback: that way the page is
           "already the top of the stack" inside its own on_enter, which is what
           makes the semantics of cgl_page_nav_top() / back() / a further
           request() self-consistent. */
        n->stack[n->depth] = to;
        n->depth++;
        if ((from != NULL) && (from->vt != NULL) && (from->vt->on_pause != NULL))
        {
            from->vt->on_pause(from, to);
        }
        if ((to->vt != NULL) && (to->vt->on_enter != NULL))
        {
            to->vt->on_enter(to, from, kind);
        }
        break;

    case CGL_NAV_POP:
        from = n->stack[n->depth - 1u];
        n->depth--;
        n->stack[n->depth] = NULL;
        to = nav__top(n);
        if (to != NULL)
        {
            /* Make it visible first, then on_resume: otherwise a callback that
               wants to invalidate / measure would run into an "invisible"
               subtree (both update and draw are blocked by CGL_WF_VISIBLE). */
            nav__show(to, true);
            if (to->root != NULL)
            {
                cgl_widget_raise(to->root); /* the arriving page sits above the departing one */
            }
            cgl_widget_raise(&n->fade.base);
            if ((to->vt != NULL) && (to->vt->on_resume != NULL))
            {
                to->vt->on_resume(to, from);
            }
        }
        if ((from->vt != NULL) && (from->vt->on_exit != NULL))
        {
            from->vt->on_exit(from, to);
        }
        break;

    case CGL_NAV_REPLACE:
        from = nav__top(n);
        if (from != NULL)
        {
            n->depth--;
            n->stack[n->depth] = NULL;
            if ((from->vt != NULL) && (from->vt->on_exit != NULL))
            {
                from->vt->on_exit(from, page);
            }
        }
        to = page;
        nav__attach(n, to, arg);
        n->stack[n->depth] = to;
        n->depth++;
        if ((to->vt != NULL) && (to->vt->on_enter != NULL))
        {
            to->vt->on_enter(to, from, kind);
        }
        break;

    case CGL_NAV_RESET:
    default:
        from = NULL; /* everything is destroyed, so on_enter is not given a
                        "source page" that has already been torn down */
        while (n->depth > 0u)
        {
            cgl_page_t *dead = n->stack[n->depth - 1u];
            n->depth--;
            n->stack[n->depth] = NULL;
            if ((dead->vt != NULL) && (dead->vt->on_exit != NULL))
            {
                dead->vt->on_exit(dead, page);
            }
            nav__detach_destroy(n, dead);
        }
        /* RESET is "back to the main screen", and there is no departing page to
           animate (the old ones are already destroyed). */
        trans = (uint8_t)CGL_TRANS_NONE;
        if (page == NULL)
        {
            cgl_widget_invalidate_rect(n->area);
            return;
        }
        to = page;
        nav__attach(n, to, arg);
        n->stack[n->depth] = to;
        n->depth++;
        if ((to->vt != NULL) && (to->vt->on_enter != NULL))
        {
            to->vt->on_enter(to, from, kind);
        }
        break;
    }

    nav__start(n, from, to, kind, trans);
}

void cgl_page_nav_frame(cgl_page_nav_t *n, const cgl_clock_t *clk)
{
    if ((n == NULL) || (clk == NULL))
    {
        return;
    }

    /* * If a new request arrives while a transition is still running, first run
       the old transition to completion **immediately**.
       It must not simply be overwritten: nav__finish() is what "hides /
       destroys the departing page and zeroes the offset", and skipping it would
       leave that page in the tree forever (and carrying a non-zero offset).
       Finish it first, then commit the new request, and the state is clean. */
    if (n->trans.active && n->req_pending)
    {
        cgl_anim_finish(&n->trans.anim);
        nav__finish(n);
    }

    if (n->req_pending)
    {
        nav__apply(n);
    }

    if (!n->trans.active)
    {
        return;
    }

    /* During a transition damage the whole screen every frame: both pages are
       moving, so the union of their damage rectangles is the whole screen
       anyway, and accounting in detail is pointless (the band presenter only
       pushes whole bands either way). */
    cgl_anim_tick(&n->trans.anim, clk->dt_ms);
    nav__layout(n);
    cgl_widget_invalidate_rect(n->area);

    if (cgl_anim_done(&n->trans.anim))
    {
        nav__finish(n);
    }
}

bool cgl_page_nav_event(cgl_page_nav_t *n, const cgl_event_t *ev)
{
    cgl_page_t *top;

    if ((n == NULL) || (ev == NULL))
    {
        return false;
    }
    if (n->trans.active)
    {
        return false; /* input lock: no operations are accepted during a transition,
                         so navigation cannot pile up */
    }
    top = nav__top(n);
    if (top == NULL)
    {
        return false;
    }

    /* "Back" is a **navigation semantic**, not a page semantic: so the navigator
       handles it itself, and pages do not each have to implement "long-press
       back / side-key back".
       Only at the bottom of the stack (depth == 1) is it handed to the page --
       at that point it may want to "exit if pressed again". */
    if ((ev->kind == CGL_EV_BACK) && (n->depth > 1u))
    {
        return cgl_page_nav_request(n, CGL_NAV_POP, NULL, (cgl_trans_t)CGL_TRANS_AUTO, NULL);
    }

    if ((top->vt != NULL) && (top->vt->on_event != NULL) && top->vt->on_event(top, ev))
    {
        return true;
    }
    if (top->root != NULL)
    {
        return cgl_widget_dispatch_event(top->root, ev);
    }
    return false;
}

uint32_t cgl_page_nav_transitions(const cgl_page_nav_t *n)
{
    return (n == NULL) ? 0u : n->transitions;
}

uint32_t cgl_page_nav_dropped(const cgl_page_nav_t *n)
{
    return (n == NULL) ? 0u : n->req_dropped;
}
