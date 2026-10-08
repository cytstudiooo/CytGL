// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_page.h
  * @brief   LCD UI framework - pages and the navigation stack.
  *
  *  Design goals (and how this differs from "the old kind of page scheduler")
  *  -------------------------------------------------------------------------
  *  The reference implementation
  *  (E:/Workspace/lvgl_test_components/components/Urdr) works like this: one
  *  global singleton pageManagerOS, a page = a numeric ID plus a malloc'ed table
  *  of callbacks, push/pop on a uint8 stack, and in pageRunning() an
  *  if (NewPage != OldPage) just calls exit/setup directly -- an instant switch,
  *  no transition, and no "page" object either.
  *
  *  That is deliberately not copied here, for four reasons, every one of them a
  *  real defect of that version:
  *
  *  1. **A page is an object, not a number.** A numeric ID needs a table indexed
  *     by ID and out-of-range access can only be blocked by IS_PAGE(); but a
  *     "page" has state of its own (its widgets, scroll position, animations), so
  *     here a page is an object with a vtable, the same pattern as a widget (the
  *     base goes in the first member). Incidentally: **no malloc**, the stack is
  *     a fixed-length array -- this project has only about 5.3 KB of RAM left, and
  *     the rest of the framework (the fixed-length text of cgl_label) does not use
  *     the heap anyway.
  *
  *  2. **A page change must have a transition.** This is a graphical UI, and an
  *     "instant cut" amounts to hiding the page change: what the user sees is the
  *     picture suddenly becoming a different picture. A transition makes the
  *     "hierarchy relationship" visible (a new page coming in from the right =
  *     going in; sliding away to the right = going back). Transitions are built
  *     from the framework's own cgl_anim plus the widget offset; see cgl_widget.h
  *     / cgl_canvas.h.
  *
  *     The default transition is **Genie ("suck in")**: on POP the current page is
  *     sucked into a suction point below the screen, revealing the page underneath
  *     (macOS minimise); on PUSH the new page pours back out of the suction point.
  *     It is not a displacement but a **non-linear deformation** (cgl_warp_t):
  *     installed on the page's root widget and removed when the transition ends --
  *     exactly the same seam as offset, so the invariant "the widget tree only
  *     changes at this one level" is not broken either.
  *     The same deformation also has a version with a **horizontal S trajectory**
  *     (`CGL_TRANS_S_OUT` / `S_IN`): the suction point is in the **bottom-right
  *     corner**, and the page does not fall straight down but first arches to the
  *     right and then tucks into the corner -- the route is an S curve. The two
  *     share **one code path** with Genie; the only difference is the suction
  *     point chosen by `nav__warp_setup()` and that one cell,
  *     `cgl_warp_t.s_amp_q8`. Which one is used, how fast, and with what curve can
  *     be written on **the page itself** (`cgl_page_set_trans_enter/leave`, see
  *     `cgl_page_trans_cfg_t`); the navigator only falls back in the order
  *     "page declaration -> caller-specified -> action default -> type table".
  *
  *  3. **Navigation takes effect at the frame boundary, never changing the widget
  *     tree in the middle of a frame.** Urdr uses IsPageBusy and **simply discards**
  *     a request while busy (press back twice and the second press is gone). Here a
  *     request is stored in a pending slot and committed in
  *     cgl_page_nav_frame(): repeated presses are not lost (the last one wins), and
  *     the widget tree is guaranteed not to change during present() -- otherwise
  *     you would get "the tree changed halfway through drawing".
  *
  *  4. **The lifecycle distinguishes "leaving" from "destroying".** Urdr only has
  *     Setup/Exit, and a page held down in the stack is neither destroyed nor
  *     told about it; here on_exit (start leaving, the transition's start point)
  *     is distinguished from on_destroy (truly discarded, the transition's end
  *     point), as are on_pause / on_resume (held down and revealed again). A page
  *     object holds its own widgets (as static members), so destroy -> create can
  *     cycle over the same block of static memory repeatedly -- no heap and no
  *     leak.
  *
  *  The page time contract (together with cgl_time.h)
  *  ------------------------------------------------
  *      on_step(step_ms)   only the **top** page receives it, 0..N times per
  *                         frame, fixed step. Physics/logic goes here. A page
  *                         held down does not receive it -- so "a paused page's
  *                         clock is still running" simply cannot happen.
  *      on_frame(clk)      only the top page receives it, exactly once per frame.
  *                         Tweens, state machines and label refreshes go here.
  *      on_event(ev)       only the top page receives it, and **input is locked
  *                         during a transition** (otherwise the user could stack
  *                         three levels of navigation halfway through).
  ******************************************************************************
  */
#ifndef CGL_PAGE_H
#define CGL_PAGE_H

#include <cytgl/cgl_widget.h>
#include <cytgl/cgl_time.h>
#include <cytgl/cgl_anim.h>
#include <cytgl/cytgl_config.h>

/* -------------------------------------------------------------------------- */
/* navigation actions and transitions                                         */
/* -------------------------------------------------------------------------- */

typedef enum
{
    CGL_NAV_PUSH = 1, /**< push: the old page stays below (on_pause) and gets on_resume when going back */
    CGL_NAV_POP,      /**< pop: the top page is discarded and the one below is revealed again */
    CGL_NAV_REPLACE,  /**< replace the top: the old page is discarded, the depth is unchanged */
    CGL_NAV_RESET     /**< clear the stack and keep only the new page (used to return home); no transition */
} cgl_nav_kind_t;

typedef enum
{
    CGL_TRANS_NONE = 0,   /**< instant cut. When the caller passes it explicitly it **overrides the page declaration** (see below) */
    CGL_TRANS_SLIDE_LEFT, /**< new page enters from the right, old exits left -- the default for forward */
    CGL_TRANS_SLIDE_RIGHT,/**< the reverse -- the default for going back            */
    CGL_TRANS_SLIDE_UP,   /**< new page enters from below, old exits upwards        */
    CGL_TRANS_SLIDE_DOWN, /**< the reverse                                          */
    CGL_TRANS_COVER_UP,   /**< only the new page moves, covering the old one from below (menu/popup semantics) */
    CGL_TRANS_COVER_DOWN, /**< only the new page moves, covering from above         */
    CGL_TRANS_FADE,       /**< fades through the background colour: the old page fades out in the first half, the new one fades in in the second */
    CGL_TRANS_GENIE_OUT,  /**< the old page is "sucked" into the suction point, revealing the new one (macOS minimise) */
    CGL_TRANS_GENIE_IN,   /**< the new page "pours out" of the suction point (macOS restore from the Dock) */
    CGL_TRANS_S_OUT,      /**< like GENIE_OUT, but the suction point is bottom-right and the route is an S trajectory */
    CGL_TRANS_S_IN,       /**< like GENIE_IN, but out of the bottom-right corner and the S is walked backwards */
    CGL_TRANS_AUTO,       /**< leave it to the navigator to pick the default for the action (recommended) */
    CGL_TRANS_COUNT
} cgl_trans_t;

/** Page flags. */
#define CGL_PAGE_F_CREATED 0x0001u /**< on_create has already run (internal use) */
#define CGL_PAGE_F_SHOW_BELOW 0x0002u /**< while this page is on top, the page below keeps being drawn */

typedef struct cgl_page cgl_page_t;
typedef struct cgl_page_nav cgl_page_nav_t;

/* Forward declaration only: cgl_screen.h includes THIS header (the screen stitches
   display + clock + pages + input together), so the dependency cannot go the other
   way. C11 allows a typedef to be repeated with the same type, which is what makes
   this legal once cgl_screen.h defines it for real. */
typedef struct cgl_screen cgl_screen_t;

/**
 * Page vtable. **Every member may be NULL** (write it with designated
 * initialisers; whatever is missing is automatically NULL).
 *
 * Lifecycle order (using PUSH as the example):
 *      on_create(new) -> on_pause(old) -> on_enter(new) -> [transition] -> old page hidden
 * POP:
 *      on_resume(lower) -> on_exit(upper) -> [transition] -> on_destroy(upper)
 */
typedef struct
{
    const char *type;

    /** Build the widget tree on first entry (once only). No heap: the widgets are static members of this page. */
    void (*on_create)(cgl_page_t *p);
    /** Tear down the widget tree when the page is truly discarded. The same static memory may then be created once more. */
    void (*on_destroy)(cgl_page_t *p);

    /** Start entering (the transition's start point). from == NULL means this is the first page at the bottom of the stack. */
    void (*on_enter)(cgl_page_t *p, cgl_page_t *from, uint8_t kind);
    /** Start leaving (the transition's start point). to == NULL means there is no next page. */
    void (*on_exit)(cgl_page_t *p, cgl_page_t *to);

    /** Held down underneath (PUSH/REPLACE). This page no longer receives on_step/on_frame/on_event. */
    void (*on_pause)(cgl_page_t *p, cgl_page_t *by);
    /** The page above popped away and this one is revealed again. */
    void (*on_resume)(cgl_page_t *p, cgl_page_t *from);

    /** Fixed-step logic advance; only the top page receives it. */
    void (*on_step)(cgl_page_t *p, uint32_t step_ms);
    /** Once per frame; only the top page receives it. */
    void (*on_frame)(cgl_page_t *p, const cgl_clock_t *clk);

    /** Input event; only the top page receives it, and it does not receive it during a transition. true = handled. */
    bool (*on_event)(cgl_page_t *p, const cgl_event_t *ev);
} cgl_page_vt_t;

/**
 * A page's own transition preference: **the page that "moves" in one navigation
 * has the last word**.
 *
 * The three fields are independent, and "not declared" is expressed by a sentinel
 * value; only the item that was not declared falls back to the layer below:
 *
 * | field    | falls back to, when not declared |
 * |---|---|
 * | `trans`  | the type passed by the caller to `cgl_page_nav_request()` -> if still none, the per-action default (PUSH->GENIE_IN etc., see `nav__default_trans()` in cgl_page.c) |
 * | `ease`   | `nav->ease[final type]` -- that is, the table changed by `cgl_page_nav_set_ease()` / `set_transition_ease()` |
 * | `dur_ms` | `nav->duration_ms[final type]` -- likewise, the table of `cgl_page_nav_set_duration()` |
 *
 * Who uses `enter` and who uses `leave` (which page "moves"):
 *
 * | action | the page that has the last word | which one |
 * |---|---|---|
 * | PUSH | the **entering page** (the new page `to`) | `enter` |
 * | POP / REPLACE | the **leaving page** (the page `from` being popped / replaced) | `leave` |
 * | RESET | none -- the framework forces an instant cut (returning home is "starting over", not a transition) | -- |
 *
 * WARNING: there is a single exception -- **when the caller explicitly passes
 * `CGL_TRANS_NONE` the page declaration does not participate**. `NONE` is not a
 * "style" (it does not answer "which animation") but "no animation"; it is the
 * only means for snapshot tests and for application initialisation to lay out the
 * first screen, so it must be able to override the page declaration. A page that
 * wants a fixed instant cut declares `trans = CGL_TRANS_NONE` itself.
 *
 * WARNING: do not confuse this with `cgl_page_trans_t`: that is the **transition
 * currently playing in the navigator** (runtime state), whereas this is the
 * preference **declared** by a page (static configuration).
 */
typedef struct
{
    uint8_t trans;   /**< cgl_trans_t; `CGL_TRANS_AUTO` = no type declared         */
    uint8_t ease;    /**< cgl_ease_t; `CGL_PAGE_FX_KEEP_EASE` = no curve declared  */
    uint16_t dur_ms; /**< milliseconds; `CGL_PAGE_FX_KEEP_MS` = no duration declared */
} cgl_page_trans_cfg_t;

/** The "this item is not declared" sentinel of `cgl_page_trans_cfg_t` (`trans` uses `CGL_TRANS_AUTO`). */
#define CGL_PAGE_FX_KEEP_EASE CGL_EASE_COUNT /**< curve: falls back to `nav->ease[]`        */
#define CGL_PAGE_FX_KEEP_MS 0u              /**< duration: falls back to `nav->duration_ms[]` */

struct cgl_page
{
    const cgl_page_vt_t *vt; /**< first member, so it can be "inherited" like a widget */
    const char *name;
    cgl_widget_t *root;      /**< root of this page's widget tree; bound with set_root in on_create */
    cgl_page_nav_t *nav;     /**< the navigator it belongs to; filled in by the navigator on push */
    void *user;             /**< the page's own context (used by the app layer)   */
    void *arg;              /**< argument of this push, see cgl_page_nav_request  */
    uint16_t flags;

    /* This page's own transition preference. Both default to "not declared"
       (cleared in cgl_page_init), so a page that never touches them behaves
       exactly as if this mechanism did not exist. */
    cgl_page_trans_cfg_t enter; /**< when this page **appears** (the entering page of a PUSH) */
    cgl_page_trans_cfg_t leave; /**< when this page **disappears** (the leaving page of a POP / REPLACE) */
};

/* -------------------------------------------------------------------------- */
/* page                                                                       */
/* -------------------------------------------------------------------------- */

/**
 * Initialise a page object (it does not build the widget tree -- that is
 * on_create's job).
 * @param user the page's own context pointer; the framework does not touch it
 */
void cgl_page_init(cgl_page_t *p, const cgl_page_vt_t *vt, const char *name, void *user);

/* -------------------------------------------------------------------------- */
/* registering pages: one context, one call per page, one table              */
/* -------------------------------------------------------------------------- */

/**
 * **The** environment a page is created in: what it borrows, in one object.
 *
 * Why this exists. Before it, every page was constructed with a slightly different
 * argument list -- one took the screen, the next took the screen plus the page whose
 * RAM it shares, the third took the screen plus two other pages, and the menu needed
 * a *second* call to attach the page it had no parameter for. Each signature was
 * defensible on its own and the set of them was not: a reader could not tell what
 * "adding a page" means, and adding a fifth page meant inventing a sixth signature.
 *
 * So: **every page is created by exactly one function of exactly one shape**,
 *
 * ```c
 * void mypage_attach(cgl_page_t *page, const cgl_page_ctx_t *ctx);
 * ```
 *
 * and the page table below is the only place that knows the set of them. A page
 * that needs a sibling reaches it through `ctx->app` (the application's own links
 * object) instead of through its own extra parameter.
 *
 * Everything here is **borrowed**: the caller (the board port / the test harness)
 * owns the screen and the navigator, and `app` points at storage the application
 * owns. The context must outlive the pages -- give it static storage, which is what
 * `app_ui.c` and the host tests do. The framework keeps a pointer to it in
 * `cgl_page_t::user`, so a page can read it back at any time with
 * `cgl_page_ctx()` -- including inside `on_create`, which is where the widget trees
 * are built.
 *
 * This costs no RAM: the pointer goes into `cgl_page_t::user`, a field that already
 * existed. (A page that wants a context of its own puts it inside its own struct.)
 */
typedef struct
{
    cgl_screen_t *screen; /**< borrowed; the frame clock, FPS and the display  */
    cgl_page_nav_t *nav;  /**< borrowed; the navigator these pages join        */
    void *app;            /**< the application's own context (its page links); the framework never dereferences it */
} cgl_page_ctx_t;

/**
 * One entry of a page table: a page, and the single function that creates it.
 *
 * @param name   for diagnostics and for the HUD; the page's own `name` is set from
 *               it by the attach function (through `cgl_page_init`).
 * @param page   caller-owned storage whose **first member is `cgl_page_t`** -- the
 *               usual "inherit a page like a widget" pattern. May not be NULL.
 * @param attach the page's one creation entry point; see `cgl_page_ctx_t`. May be
 *               NULL, which registers the storage without creating it (useful for a
 *               page a build configuration leaves out).
 */
typedef struct
{
    const char *name;
    cgl_page_t *page;
    void (*attach)(cgl_page_t *page, const cgl_page_ctx_t *ctx);
} cgl_page_desc_t;

/**
 * Create every page in a table, in order, with the same context.
 *
 * This is the whole "adding a page" story: write one `*_attach()` function of the
 * standard shape, add one object member for its storage, add one row to the table.
 * Nothing anywhere else in the application has to change -- in particular no
 * constructor grows another parameter.
 *
 * @return the number of pages that were actually created (a NULL `page` or a NULL
 *         `attach` is skipped rather than dereferenced).
 *
 * The pages are only **created**: nothing is pushed onto the navigation stack. The
 * first page is installed by `cgl_page_nav_reset()` (or `..._push()`), which is also
 * what runs the first `on_create`.
 */
uint8_t cgl_page_register(const cgl_page_desc_t *pages, uint8_t count,
                          const cgl_page_ctx_t *ctx);

/** The context a page was created with (what `cgl_page_init` was given), or NULL. */
const cgl_page_ctx_t *cgl_page_ctx(const cgl_page_t *p);

/** Bind this page's widget tree root in on_create. Passing NULL means this page draws nothing. */
void cgl_page_set_root(cgl_page_t *p, cgl_widget_t *root);

/**
 * Declare this page to be a **translucent overlay**: after it comes up
 * (`CGL_NAV_PUSH`), **the page below keeps being drawn**.
 *
 * Why this switch is needed: by default `nav__finish()` calls
 * `cgl_widget_set_visible(false)` on the leaving page when the PUSH transition
 * ends -- and "hidden" means **not a single pixel of the whole subtree is drawn**.
 * For an opaque page that is right (a free saved pass); but for a translucent
 * overlay it means only the host background colour remains underneath, and what
 * `cgl_widget_set_alpha()` reveals is the background, not that page's content.
 *
 * Typical use (**two things have to be done together**; neither alone is enough):
 *
 * ```c
 * cgl_page_set_show_below(&overlay.base, true);      // 1) let the page below keep drawing
 * cgl_widget_set_alpha(&overlay.root.base, 208u);    // 2) give the overlay an alpha
 * ```
 *
 * WARNING: it **only governs whether to draw**:
 *   - The page below still does not receive `on_step` -- physics/logic stays
 *     frozen as before (this is a core invariant of the page model; do not touch
 *     it to make an effect work);
 *   - The cost is decided by **the content complexity of the page below**: every
 *     band now has to repaint it. With a static page on top it is nearly free (no
 *     damage means no repaint), but on top of an animating page it is expensive.
 *
 * Chaining works too: if A translucently covers B and C translucently covers A, B
 * keeps drawing only if A also declared it.
 */
void cgl_page_set_show_below(cgl_page_t *p, bool show);

/** Whether this page declared "the page below keeps drawing" (returns false if never set). */
bool cgl_page_get_show_below(const cgl_page_t *p);

/**
 * Declare which transition and what feel "when this page **appears**". The three
 * parameters together describe the **complete** preference (they are not "only
 * change the one passed in" -- to keep an existing type, pass it again).
 *
 * ```c
 * cgl_page_set_trans_enter(&p->base, CGL_TRANS_COVER_UP,          // both type and feel
 *                         CGL_EASE_OUT_BACK, 180u);
 * cgl_page_set_trans_enter(&p->base, CGL_TRANS_GENIE_IN,          // type only
 *                         CGL_PAGE_FX_KEEP_EASE, CGL_PAGE_FX_KEEP_MS);
 * cgl_page_set_trans_enter(&p->base, CGL_TRANS_AUTO,              // withdraw the declaration (back to default)
 *                         CGL_PAGE_FX_KEEP_EASE, CGL_PAGE_FX_KEEP_MS);
 * ```
 *
 * @param trans  `CGL_TRANS_AUTO` = no type declared (falls back to what the
 *               caller passed / to the per-action default); an out-of-range value
 *               >= `CGL_TRANS_COUNT` makes **the whole call** take no effect (no
 *               "half-changed" preference is left behind).
 * @param ease   an out-of-range value (> `CGL_EASE_COUNT`) is clamped to
 *               `CGL_EASE_LINEAR` -- the same clamping as `cgl_anim_start()` and
 *               `cgl_page_nav_set_ease()`; `CGL_PAGE_FX_KEEP_EASE` = not declared.
 * @param dur_ms clamped to 65535; `CGL_PAGE_FX_KEEP_MS`(0) = not declared.
 *
 * WARNING: `RESET` (returning home) is always an instant cut; declaring `enter`
 *          has no effect on it.
 * WARNING: a `dur_ms` of 0 means "not declared"; to express "this page cuts
 *          instantly" declare `trans = CGL_TRANS_NONE`.
 * WARNING: declaring it inside `on_create` is also in time: the transition
 *          preference is only read when the navigation is committed
 *          (`nav__start()`), and the entering page's `on_create` runs before that.
 */
void cgl_page_set_trans_enter(cgl_page_t *p, cgl_trans_t trans, cgl_ease_t ease, uint32_t dur_ms);

/** Declare the transition "when this page **disappears**" (the leaving page of a POP / REPLACE). Parameter semantics are the same as set_trans_enter. */
void cgl_page_set_trans_leave(cgl_page_t *p, cgl_trans_t trans, cgl_ease_t ease, uint32_t dur_ms);

static inline bool cgl_page_is_created(const cgl_page_t *p) { return (p->flags & CGL_PAGE_F_CREATED) != 0u; }
static inline cgl_widget_t *cgl_page_root(const cgl_page_t *p) { return p->root; }

/* -------------------------------------------------------------------------- */
/* navigator                                                                  */
/* -------------------------------------------------------------------------- */

/** One transition currently playing (or just finished). */
typedef struct
{
    cgl_page_t *from;   /**< leaving page (only hidden on PUSH, destroyed on POP/REPLACE) */
    cgl_page_t *to;     /**< entering page                                   */
    cgl_anim_t anim;    /**< 0..256 eased progress (from=0, to=256)          */
    uint16_t fade_q8;  /**< mask opacity used by CGL_TRANS_FADE              */
    uint8_t kind;      /**< cgl_nav_kind_t                                  */
    uint8_t trans;     /**< cgl_trans_t                                     */
    bool active;
} cgl_page_trans_t;

/** Small internal widget of the navigator: the background-colour mask covering the whole screen during a FADE transition. */
typedef struct
{
    cgl_widget_t base;
    cgl_page_nav_t *nav;
} cgl_page_fade_t;

struct cgl_page_nav
{
    cgl_widget_t host;  /**< page host: the parent of every page root widget (whole screen, clipped) */
    cgl_rect_t area;    /**< the host's screen rectangle; transition displacement is relative to it */

    cgl_page_t *stack[CGL_CFG_PAGE_STACK_MAX];
    uint8_t depth;

    /* pending navigation: committed only at the frame boundary, so the widget tree is constant within a frame */
    cgl_page_t *req_page;
    void *req_arg;
    uint8_t req_kind;
    uint8_t req_trans;
    bool req_pending;
    uint32_t req_dropped; /**< requests rejected because the target was illegal (for observation) */

    cgl_page_trans_t trans;
    cgl_page_fade_t fade;
    /**
     * Duration of each transition class (milliseconds, indexed by cgl_trans_t).
     * 0 = that class cuts instantly. Costs 22 bytes of RAM more than "one global
     * value" and buys mixes like "suction 300 ms, slide 220 ms".
     * WARNING: it is a uint16_t: 65 s is already far beyond any watchable
     * transition, and the setter clamps to that ceiling.
     * WARNING: this is a **fallback layer**: when a page has declared its own
     * duration with `cgl_page_set_trans_enter/leave()`, that page does not listen
     * to this table (the same holds for the curve table).
     */
    uint16_t duration_ms[CGL_TRANS_COUNT];
    /**
     * Easing curve of each transition class (indexed by cgl_trans_t, the value is
     * a `cgl_ease_t`). Same pattern as duration_ms: 11 bytes buy the mix of "one
     * curve for slides, another for suction".
     * WARNING: it is stored as uint8_t only for compactness; read it through
     * cgl_page_nav_ease() (or nav__ease() in cgl_page.c), which block
     * out-of-range values -- reading this array directly is unprotected.
     * WARNING: it is likewise a **fallback layer**: a page that declared its own
     * curve does not look at this table.
     */
    uint8_t ease[CGL_TRANS_COUNT];
    uint32_t transitions; /**< number of completed transitions (for observation) */

    /* deformation descriptor used by the GENIE_* transitions: it lives in the
       navigator (static, no heap), is attached to the root widget of the page
       being sucked when the transition starts and removed when it ends.
       At the start of every transition it is rewritten in full by
       nav__warp_setup() (the suction point and the S amplitude are both computed
       from the final type), so what is stored here is only "what the last one
       looked like". */
    cgl_warp_t warp;
    int16_t genie_x;     /**< suction point centre x (default = the centre of area; GENIE_* only) */
    int16_t genie_y;     /**< suction point y (default = a little below the bottom of area, like the Dock) */
};

/**
 * Initialise the navigator. area is usually the whole screen.
 * It only initialises; no page is pushed -- the first page is installed by
 * cgl_page_nav_reset().
 */
void cgl_page_nav_init(cgl_page_nav_t *n, const char *name, cgl_rect_t area);

/** The host widget (attach it to the root of the display/scene). */
cgl_widget_t *cgl_page_nav_host(cgl_page_nav_t *n);

/** The top page, or NULL when the stack is empty. */
cgl_page_t *cgl_page_nav_top(const cgl_page_nav_t *n);

/** Current stack depth. */
uint8_t cgl_page_nav_depth(const cgl_page_nav_t *n);

/** A transition is in progress (input is locked and the tree must not be changed externally). */
bool cgl_page_nav_busy(const cgl_page_nav_t *n);

/** Whether there is a navigation request not yet committed. */
bool cgl_page_nav_pending(const cgl_page_nav_t *n);

/**
 * Set the duration of **all** transitions (milliseconds). 0 = everything cuts
 * instantly.
 * To change only one class (say, only make the suction slower), use the next one.
 */
void cgl_page_nav_set_duration(cgl_page_nav_t *n, uint32_t ms);

/**
 * Set the duration of **one class** of transition (milliseconds). 0 = that class
 * cuts instantly.
 *
 * @param trans a specific transition type; passing `CGL_TRANS_AUTO` is equivalent
 *              to set_duration() (changes all of them).
 *
 * Default table: `CGL_TRANS_NONE` = 0, `GENIE_IN/OUT` = `CGL_CFG_GENIE_MS`
 * (300 ms), everything else = `CGL_CFG_TRANSITION_MS` (220 ms). The value is
 * clamped to 65535 ms.
 *
 * The feel is not decided by the duration alone -- the curve is another
 * one-to-one table, see set_transition_ease().
 * WARNING: both tables are a **fallback layer**: when a page has declared its own
 * duration with `cgl_page_set_trans_enter/leave()`, that page's transitions of
 * this class do not listen to this table. Use this setter only to change "all
 * pages".
 */
void cgl_page_nav_set_transition_duration(cgl_page_nav_t *n, cgl_trans_t trans, uint32_t ms);

/** Read the current duration of one transition class (milliseconds). For observation/tests (0 = that class cuts instantly).
    Note that it reads the **type table**, not "the value some page will end up using" -- a page may have declared its own. */
uint32_t cgl_page_nav_duration(const cgl_page_nav_t *n, cgl_trans_t trans);

/**
 * Set the easing curve of **all** transitions. An out-of-range ease is clamped to
 * `CGL_EASE_LINEAR` (constant speed).
 * Like setting the duration, it skips `CGL_TRANS_NONE` -- "no transition" has no
 * curve to speak of.
 */
void cgl_page_nav_set_ease(cgl_page_nav_t *n, cgl_ease_t ease);

/**
 * Set the easing curve of **one class** of transition. This is where you get
 * mixes like "suction gathers then flings, slides land softly".
 *
 * @param trans a specific transition type; passing `CGL_TRANS_AUTO` is equivalent
 *              to set_ease() (changes all of them).
 * @param ease  an out-of-range value (>= `CGL_EASE_COUNT`) is clamped to
 *              `CGL_EASE_LINEAR` -- the same clamping as `cgl_anim_start()`: the
 *              "safe default" for a curve is constant speed.
 *
 * Default table: `NONE` = LINEAR (no shaping, used for observation only),
 * `GENIE_OUT` = `CGL_CFG_GENIE_OUT_EASE` (IN_QUAD), `GENIE_IN` =
 * `CGL_CFG_GENIE_IN_EASE` (OUT_CUBIC), everything else =
 * `CGL_CFG_TRANSITION_EASE` (OUT_CUBIC).
 *
 * WARNING: `CGL_EASE_OUT_BACK` makes the progress briefly exceed 256 (explained
 * in `cgl_anim.h`). The slide / cover classes rely on exactly that overshoot for
 * their "overshoot then spring back" feel; Genie goes through a geometric
 * deformation, so read the CytGL manual, section 12.2 before pairing it
 * with one.
 * WARNING: this is likewise the **type table (fallback layer)**: a page that
 * declared its own curve does not listen to it.
 */
void cgl_page_nav_set_transition_ease(cgl_page_nav_t *n, cgl_trans_t trans, cgl_ease_t ease);

/** Read the current curve of one transition class (for observation/tests). An out-of-range value returns `CGL_EASE_LINEAR`.
    Like cgl_page_nav_duration(), it reads the type table and not "the value some page will end up using". */
cgl_ease_t cgl_page_nav_ease(const cgl_page_nav_t *n, cgl_trans_t trans);

/**
 * Set the target point of the Genie (suction) transition. The default is the
 * midpoint of the bottom edge of `area` and then `CGL_CFG_GENIE_DROP_PX` pixels
 * further down -- an "off-screen Dock".
 *
 * Passing a point off the screen (say y = screen height + 6) makes the page
 * **leave the screen completely** on its last frame, which finishes more cleanly
 * than "stopping at the edge of the screen".
 *
 * @param x **the centre column of the suction point** (not its top-left corner).
 *          So "suck to the bottom-right corner" passes `area.x1` (the page centre
 *          then lands exactly on the right edge at p=256), not `area.x1 - width/2`.
 * @note This cell only affects `GENIE_OUT/IN`. `S_OUT/S_IN` have their own
 *       bottom-right suction point (`area.x1, area.y1 + CGL_CFG_S_DROP_PX`),
 *       which is **part of the type** -- "an S-shaped route" and "sucked to where"
 *       are the same thing to begin with and should not be split into two knobs
 *       that can each be set wrong.
 */
void cgl_page_nav_set_genie_target(cgl_page_nav_t *n, int16_t x, int16_t y);

/**
 * The deformation descriptor currently playing (returns NULL when none is). For
 * observation/tests: feed it to cgl_canvas_map_y() and you can work out for
 * yourself "what this frame's page has been sucked into".
 */
const cgl_warp_t *cgl_page_nav_warp(const cgl_page_nav_t *n);

/**
 * Request one navigation. **It only registers the request; it does not change the
 * widget tree** -- the real effect happens in cgl_page_nav_frame().
 *
 * Multiple requests within the same frame: only **the last** takes effect
 * (repeated back presses are not lost, and do not pile up into a stack overflow).
 *
 * @param kind  CGL_NAV_PUSH / POP / REPLACE / RESET
 * @param page  the target page (ignored on POP, may be NULL)
 * @param trans CGL_TRANS_AUTO means take the default transition for the action
 * @param arg   stored into the target page's cgl_page_t.arg
 * @return false = the request was rejected as illegal (NULL target / already on the
 *         stack / only one page on the stack for a POP, etc.)
 */
bool cgl_page_nav_request(cgl_page_nav_t *n, cgl_nav_kind_t kind, cgl_page_t *page,
                         cgl_trans_t trans, void *arg);

bool cgl_page_nav_push(cgl_page_nav_t *n, cgl_page_t *page);
bool cgl_page_nav_replace(cgl_page_nav_t *n, cgl_page_t *page);
bool cgl_page_nav_reset(cgl_page_nav_t *n, cgl_page_t *page);
/** Go back one level. Returns false when the stack holds only one page (nowhere to go). */
bool cgl_page_nav_back(cgl_page_nav_t *n);

/**
 * Frame boundary: commit the pending navigation and advance the transition. Called
 * once per frame by cgl_screen_frame(), **before on_step**. This is the only entry
 * point through which the navigator modifies the widget tree.
 */
void cgl_page_nav_frame(cgl_page_nav_t *n, const cgl_clock_t *clk);

/**
 * Hand one input event to the top page (on_event first; if it is not consumed,
 * then the widget tree dispatch).
 * Returns false and dispatches nothing during a transition -- input lock.
 */
bool cgl_page_nav_event(cgl_page_nav_t *n, const cgl_event_t *ev);

/** Number of completed transitions. */
uint32_t cgl_page_nav_transitions(const cgl_page_nav_t *n);

/** Number of requests rejected as illegal. */
uint32_t cgl_page_nav_dropped(const cgl_page_nav_t *n);

#endif /* CGL_PAGE_H */
