// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cytgl_config.h
  * @brief   LCD UI framework - compile time configuration.
  *
  *  Every hardware / footprint related knob of the framework lives here so the
  *  framework itself stays portable. Change these values to retarget another
  *  panel size or to trade RAM against refresh rate.
  ******************************************************************************
  */
#ifndef CGL_CONFIG_H
#define CGL_CONFIG_H

/* The value of CGL_CFG_*_EASE is a cgl_ease_t (the easing curve enum). cgl_anim.h
   only depends on stdint/stdbool, so including it here cannot create a cycle. */
#include <cytgl/cgl_anim.h>

/* --------------------------------------------------------------------------
 * Panel geometry (the single source of truth for the whole application).
 * 1.47" ST7789 module: 172 x 320 visible pixels -> used in landscape.
 * ------------------------------------------------------------------------ */
#define CGL_CFG_SCREEN_W 320
#define CGL_CFG_SCREEN_H 172

/* --------------------------------------------------------------------------
 * Band (scanline strip) presenter.
 *
 * A full framebuffer would need 320*172*2 = 110 KB which does not fit in the
 * 32 KB of this MCU. The framework therefore renders horizontal strips of
 * CGL_CFG_BAND_H rows into a small scratch buffer and streams them to the
 * panel with DMA while the CPU renders the next strip.
 *
 * Smaller band  -> less RAM, more SPI window commands.
 * Bigger band  -> less overhead, more RAM (2 buffers are needed).
 * ------------------------------------------------------------------------ */
#define CGL_CFG_BAND_H 4

/* --------------------------------------------------------------------------
 * Group opacity (save-under layered compositing)
 *
 * Goal: make "one widget (including its whole subtree) entirely translucent"
 * possible -- not just the single "translucent solid rectangle" case of
 * `cgl_canvas_fill_rect_blend_q8()`.
 *
 * It works without a full-screen framebuffer by doing one isolated composite
 * **per band**:
 *
 *   1. Copy the region the widget covers in this band into scratch (= the
 *      background copy);
 *   2. Render the whole subtree into that scratch **as usual** -- overlaps
 *      between child widgets composite normally inside the scratch, so the
 *      overlap is **not blended twice**;
 *   3. Blend the scratch back into the band buffer by alpha:
 *      band = band*(1-a) + scratch*a.
 *
 * Step 3 is an identity transform for "pixels that were not drawn" (there the
 * scratch still equals the background, band*(1-a) + band*a == band), so **no
 * coverage mask is needed** -- that is why it saves RAM compared with "save the
 * background, draw, then blend back through a mask".
 *
 * Costs:
 *   RAM   : CGL_CFG_ALPHA_LEVELS x CGL_CFG_ALPHA_LEVEL_STRIDE x 2 B
 *           at 320x172 / BAND_H=4 each level is 320*4*2 = 2560 B
 *   CPU   : about 28 cycles per pixel to composite, **paid only while that
 *           widget is genuinely translucent and this band is genuinely
 *           repainted**. At alpha = 255 (the default) one branch skips it, so a
 *           UI that does not use the feature costs 0.
 *
 * When porting to another panel / another RAM budget:
 *   - Large panel / large RAM: raise CGL_CFG_ALPHA_LEVELS = more nested levels;
 *     raising CGL_CFG_BAND_H makes each level wider (but produces fewer SPI
 *     window commands per page change).
 *   - Small RAM: set CGL_CFG_ALPHA to 0 -- the display layer no longer allocates
 *     scratch, so cgl_widget_set_alpha() becomes "records the value but it has
 *     no effect" (it is counted; see cgl_widget_alpha_dropped()) and RAM goes
 *     back to 0 bytes.
 *   - CGL_CFG_ALPHA_BUDGET is the RAM ceiling you are willing to give it:
 *     exceeding it is a **compile time** error, not a runtime stack overflow.
 * ------------------------------------------------------------------------ */
#ifndef CGL_CFG_ALPHA
#define CGL_CFG_ALPHA 1u
#endif

/** Number of group levels that may be nested at once. 1 = only one level (a nested inner level degrades to opaque). */
#ifndef CGL_CFG_ALPHA_LEVELS
#define CGL_CFG_ALPHA_LEVELS 2u
#endif

/** Pixels per level of scratch: one band. Must be >= actual band width x band height. */
#ifndef CGL_CFG_ALPHA_LEVEL_STRIDE
#define CGL_CFG_ALPHA_LEVEL_STRIDE (CGL_CFG_SCREEN_W * CGL_CFG_BAND_H)
#endif

/** RAM ceiling this feature is allowed to use (bytes). */
#ifndef CGL_CFG_ALPHA_BUDGET
#define CGL_CFG_ALPHA_BUDGET 6144u
#endif

/** Actual RAM footprint of group opacity (bytes), computable at compile time -- and the figure quoted in the docs. */
#define CGL_CFG_ALPHA_BYTES \
    (CGL_CFG_ALPHA ? (CGL_CFG_ALPHA_LEVELS * CGL_CFG_ALPHA_LEVEL_STRIDE * 2u) : 0u)

_Static_assert((CGL_CFG_ALPHA_LEVELS >= 1u) && (CGL_CFG_ALPHA_LEVELS <= 8u),
               "CGL_CFG_ALPHA_LEVELS must be 1..8 (each level costs one band of RAM)");
_Static_assert(CGL_CFG_ALPHA_BYTES <= CGL_CFG_ALPHA_BUDGET,
               "group opacity exceeds CGL_CFG_ALPHA_BUDGET: lower CGL_CFG_ALPHA_LEVELS / CGL_CFG_BAND_H, or raise the budget");

/* --------------------------------------------------------------------------
 * Feature limits.
 *
 * CGL_CFG_LABEL_MAX_TEXT is the text buffer of **each** cgl_label_t (statically
 * allocated, no heap). 32 chars x 8 px char width = 256 px, enough for the
 * longest of the 9 lines of readings on a full screen; every extra 8 bytes x
 * about 25 labels is roughly 200 B, which is still worth saving inside 32 KB of
 * RAM (this project has no heap at all, every label is a static member; see
 * the CytGL manual, section 17.1).
 * ------------------------------------------------------------------------ */
#define CGL_CFG_LABEL_MAX_TEXT 32 /* including the terminating NUL */

/* Maximum number of **direct** children one widget may have. It is enforced:
   cgl_widget_add_child() returns false beyond it and counts the rejection in
   cgl_widget_children_dropped().

   The number is derived, not chosen: the reference application's widest page (the
   info page: 4 furniture widgets + 10 readout lines + 1 status line) has 15, so 16
   is the smallest value that fits it with a slot to spare.

   It used to be 8 and enforced by nothing, which is the worst of both: the demo
   pages exceeded it from the day the second animation page was added, and nothing
   anywhere said so. Raising it is free -- no RAM is spent on a child count (the
   list is walked instead, at tree-construction time only), the limit exists to turn
   "the widget is silently missing" into a countable event. */
#ifndef CGL_CFG_WIDGET_MAX_CHILDREN
#define CGL_CFG_WIDGET_MAX_CHILDREN 16
#endif

/* --------------------------------------------------------------------------
 * Radial widgets: the arc toolkit (cgl_arc.h) and the watch face (cgl_watch.h)
 *
 * Both modules started life inside the demo's watch-face page
 * (examples/common/App/app_page_clock.c, 2 500 lines of dial, hands, ring, glass
 * and gauge), and both are now framework code. They cost flash and, for the watch
 * face, a small amount of RAM, so each is switchable -- exactly like CGL_CFG_ALPHA
 * above, and for the same reason: a UI that draws no circles should not pay for a
 * dial it never draws.
 *
 *   CGL_CFG_ARC    the radial toolkit: a shaded circular surface, a tapered
 *                  antialiased needle rasteriser, and the ring-gauge widget.
 *                  Turning it off removes all of it from the link.
 *
 *   CGL_CFG_WATCH  the watch-face widget: time base + calendar, chapter ring,
 *                  three hands, hub, crystal cover. **Requires CGL_CFG_ARC** (the
 *                  dial is two arc surfaces and the hands are arc strokes); the
 *                  static assert below turns "I turned the toolkit off but left
 *                  the watch on" into a compile error instead of a link error.
 *
 *   CGL_CFG_WATCH_DIAL_LUT
 *                  bake the two **default** dial tables (a 256 entry face and a
 *                  33 entry well, 578 B of flash) so that a caller can configure
 *                  a face without owning a table. Nothing allocates them unless
 *                  they are named, so this is a flash-cost-only switch. Set it to 0
 *                  and the tables are gone; a surface whose `lut` is NULL is then
 *                  shaded procedurally instead (see cgl_arc_surface_color), which
 *                  is a little slower per pixel and costs no flash at all.
 *
 *   CGL_CFG_WATCH_TRAIL
 *                  the second hand's comet trail: 60 bytes of per-mark brightness
 *                  plus a decay accumulator, inside cgl_watch_t. This is the only
 *                  per-element state a watch face keeps -- everything else is a
 *                  function of the clock -- which is why it is the one thing worth
 *                  being able to compile out of a RAM-tight build.
 *
 *   CGL_CFG_WATCH_GLASS
 *                  the crystal cover pass: a reflection composited *over* the
 *                  hands. Pure flash (no RAM), but it is a whole extra full-dial
 *                  pass per frame, so a build that wants the last few percent of
 *                  frame time back can drop it.
 *
 * WARNING: these are **compile-time** switches and the runtime ones are the
 * CGL_WATCH_F_* flags in cgl_watch.h. That is not two ways of doing the same
 * thing: this one decides what exists in the binary, the runtime flag decides
 * whether one particular widget draws it. A build with CGL_CFG_WATCH_GLASS = 0
 * has no glass code at all and CGL_WATCH_F_GLASS then does nothing (it is
 * accepted and ignored, so a configuration struct stays portable between builds).
 *
 * CGL_CFG_ARC stands **on its own**, and that is the point of the module: a caller
 * who wants a ring gauge, a shaded dial of their own design, a tapered needle or an
 * antialiased arc must not get a watch face, a calendar, a damage model and 578
 * bytes of dial tables with it. Turning the watch off is one line:
 *
 *      -DCGL_CFG_WATCH=0     the arc toolkit alone -- no watch, no dial tables, no
 *                            comet trail, no crystal. `cgl_arc.h` is unaffected.
 *
 * The reverse is the one combination that cannot be built, because the watch is
 * *made of* the toolkit (its dial is two `cgl_arc_surface_t`s and its hands are
 * `cgl_arc_stroke_t`s). That is the static assert below, not a link error about a
 * missing `cgl_arc_*` symbol.
 *
 * WARNING: the two switches are **independent**, deliberately. `CGL_CFG_WATCH` used
 * to default to `CGL_CFG_ARC`, which reads as "the watch is part of the arc module"
 * and is the wrong story: the arc toolkit is the general facility and the watch is
 * one application of it. `-DCGL_CFG_ARC=0` on its own now stops on the assert with
 * a message naming the switch to set as well, which is a better outcome than a
 * default that silently drags a whole watch in behind a toolkit.
 * ------------------------------------------------------------------------ */
#ifndef CGL_CFG_ARC
#define CGL_CFG_ARC 1u
#endif

#ifndef CGL_CFG_WATCH
#define CGL_CFG_WATCH 1u
#endif

/* The watch's own sub-switches derive from the watch, because there is no reading
   of "bake the watch's dial tables without the watch" that means anything. */
#ifndef CGL_CFG_WATCH_DIAL_LUT
#define CGL_CFG_WATCH_DIAL_LUT CGL_CFG_WATCH
#endif

#ifndef CGL_CFG_WATCH_TRAIL
#define CGL_CFG_WATCH_TRAIL CGL_CFG_WATCH
#endif

#ifndef CGL_CFG_WATCH_GLASS
#define CGL_CFG_WATCH_GLASS CGL_CFG_WATCH
#endif

_Static_assert(CGL_CFG_WATCH_DIAL_LUT <= CGL_CFG_WATCH,
               "CGL_CFG_WATCH_DIAL_LUT bakes the watch module's default tables: it cannot be on with CGL_CFG_WATCH off");
_Static_assert(CGL_CFG_WATCH <= CGL_CFG_ARC,
               "CGL_CFG_WATCH draws its dial out of cgl_arc: it cannot be on with CGL_CFG_ARC off");

/* The default dial's geometry, as a compile-time description -- **not** a limit.
 * cgl_watch_geometry_default() fills a cgl_watch_geom_t from these, and a caller
 * that wants a 120 px dial on a 240 px panel edits its own struct and never
 * touches this. They exist so that "show me a watch" is one call, and so that the
 * reference panel's numbers are stated in exactly one place.
 *
 * The reference is a 320 x 172 landscape panel, so the dial is as large as it may
 * be: centred at (86, 86) with a face radius of 80 it nearly touches both edges. */
#define CGL_CFG_WATCH_CX 86
#define CGL_CFG_WATCH_CY 86
#define CGL_CFG_WATCH_R_FACE 80

/* Seconds ring: a stroke centred on this radius, hugging the bezel. */
#define CGL_CFG_WATCH_R_RING 75
#define CGL_CFG_WATCH_RING_W 3

/* The recessed well the ring runs in: the two radii of its cross-section. */
#define CGL_CFG_WATCH_R_WELL_IN 71
#define CGL_CFG_WATCH_R_WELL_OUT 79

/* Chapter ring: where the hour and minute marks start and end. */
#define CGL_CFG_WATCH_R_TICK_OUT 70
#define CGL_CFG_WATCH_R_TICK_HOUR_IN 64
#define CGL_CFG_WATCH_R_TICK_MIN_IN 67

/* Hand tip radii, the second hand's counterweight tail, and the hub. */
#define CGL_CFG_WATCH_R_HOUR 42
#define CGL_CFG_WATCH_R_MINUTE 62
#define CGL_CFG_WATCH_R_SECOND 71
#define CGL_CFG_WATCH_R_TAIL 14
#define CGL_CFG_WATCH_R_HUB 5

/* How far a hand reaches back behind the hub, where it is at its widest (a watch
 * hand is a tapered blade, and the wide end is hidden under the hub -- which is
 * what makes the visible shape start already tapered). */
#define CGL_CFG_WATCH_R_HOUR_BASE 14
#define CGL_CFG_WATCH_R_MINUTE_BASE 16

/* Hand widths, full width in pixels at the arbor and at the tip. */
#define CGL_CFG_WATCH_HOUR_W_BASE 13
#define CGL_CFG_WATCH_HOUR_W_TIP 4
#define CGL_CFG_WATCH_MINUTE_W_BASE 11
#define CGL_CFG_WATCH_MINUTE_W_TIP 3
#define CGL_CFG_WATCH_SECOND_W 3

/* The hands' drop shadow: how far it is offset (down-right) and how dark it is,
 * Q8. 30 % of full coverage reads as "the hand floats above the dial" without
 * turning into a second, black hand. */
#define CGL_CFG_WATCH_SHADOW_PX 2
#define CGL_CFG_WATCH_SHADOW_ALPHA 77u

/* Time-of-day resolution of the second hand's **tick** motion (see
 * cgl_watch_second_mode_t): the hand rests on its mark for the rest of the second
 * and the last CGL_CFG_WATCH_TICK_MS of it is the whip that carries it onto the
 * next one. 100 ms is short enough to read as one discrete step and long enough
 * for the eye to see the whip rather than a jump cut -- below about 40 ms the hand
 * is simply teleporting at 16 ms frames, and at 1000 ms it is the sweep this mode
 * exists to be an alternative to. */
#define CGL_CFG_WATCH_TICK_MS 100u

/* How long the entrance sweep takes (12:00 -> now), the flash when the minute
 * turns over, and how long the comet trail behind the second hand glows. The last
 * one has to outlast one tick interval several times over or the "comet" is a
 * single lit mark: at 2400 ms the hand carries a tail of three marks. */
#define CGL_CFG_WATCH_REVEAL_MS 760u
#define CGL_CFG_WATCH_PULSE_MS 620u
#define CGL_CFG_WATCH_TRAIL_MS 2400u

/* The watch's second-hand motion, as the compile-time default for
 * cgl_watch_anim_default(): 1 = a one-step-per-second mechanical tick, 0 = a
 * continuous quartz sweep. WARNING: unlike the demo page -- whose one taste-shaped
 * choice had to be a build switch because it was a single number read in one
 * translation unit -- **the framework's mode is a runtime field of
 * cgl_watch_anim_t** (`cgl_watch_set_second_mode()`), so an application can offer
 * both motions in a settings screen. This macro only decides what a configuration
 * starts out as, and it is a plain 0/1 rather than the enum so that this header
 * keeps depending on nothing but cgl_anim.h. */
#define CGL_CFG_WATCH_SECOND_MODE 1u

/* --------------------------------------------------------------------------
 * Time: the single frame clock (src/core/cgl_time.h).
 *
 * CGL_CFG_CLOCK_STEP_MS is the logical (simulation) step. 16 ms is used
 * instead of the mathematically exact 16.67 ms because the clock counts whole
 * milliseconds; that is 62.5 steps/s rather than 60, a 4 % difference that no
 * tuning constant in this project can notice, and it keeps every counter in
 * cheap integer arithmetic.
 *
 * CGL_CFG_CLOCK_MAX_STEPS caps how many steps one frame may run, so a stall
 * (J-Link halt, a slow band) can never make the simulation "fast forward".
 * ------------------------------------------------------------------------ */
#define CGL_CFG_CLOCK_STEP_MS 16u
#define CGL_CFG_CLOCK_MAX_STEPS 4u

/* --------------------------------------------------------------------------
 * Pages and the navigation stack (src/core/cgl_page.h).
 *
 * The stack is a fixed array: this project has no heap anywhere, so the depth
 * is a compile time budget. 4 entries (home + two levels + a spare) costs
 * 4 pointers.
 * ------------------------------------------------------------------------ */
#define CGL_CFG_PAGE_STACK_MAX 4

/* Default duration of a page transition, in milliseconds. 220 ms is about 9
   frames at 40 fps -- short enough not to drag, long enough for the eye to see
   "where it comes from and where it goes". 0 = all transitions degrade to an
   instant cut.
   This is the duration for the **slide / cover / fade** families; Genie has its
   own default, see below. */
#define CGL_CFG_TRANSITION_MS 220u

/* Default duration of Genie ("suck in"), in milliseconds.
   Being a little longer than a slide is deliberate: the suck is a **geometric
   deformation**, and the shape itself has to be legible (the funnel narrowing
   from wide to a final vertical line). 220 ms is only 13 frames at 17 ms/frame;
   at 300 ms it is about 18 frames, and only then does the "yanked away" moment
   stop looking like a flash. */
#define CGL_CFG_GENIE_MS 320u

/* Default **easing curve** of a page transition (`cgl_ease_t`). Like the
   duration, the curve is stored "separately per transition type"
   (`nav->ease[cgl_trans_t]`); what is given here is the default for that table:
   slide / cover / fade share OUT_CUBIC -- quick off the mark, soft landing,
   reading as "pushed over and then stopped" rather than "constant-speed pan
   followed by a hard brake". */
#define CGL_CFG_TRANSITION_EASE CGL_EASE_OUT_CUBIC

/* The default curves for the two directions of Genie ("suck in").
   OUT (being sucked away) uses IN_QUAD: gather first, then fling -- the first
   half of the time covers only about 25% of the distance, so only the last
   moment has the force of being yanked away. IN (pouring back out) uses
   OUT_CUBIC: after popping out it has to come to a firm stop. */
#define CGL_CFG_GENIE_OUT_EASE CGL_EASE_IN_QUAD
#define CGL_CFG_GENIE_IN_EASE CGL_EASE_OUT_CUBIC

/* Default curves for the two directions of S-warp suction (CGL_TRANS_S_OUT /
   CGL_TRANS_S_IN). They are **exactly the same** as the two vertical suction
   ones: the difference in route is geometric (an S-shaped path plus a
   bottom-right suction point), not temporal -- the feel is the same dish under
   a new name, so a page never runs at a different speed just because a
   different kind of suction was chosen. */
#define CGL_CFG_S_OUT_EASE CGL_CFG_GENIE_OUT_EASE
#define CGL_CFG_S_IN_EASE CGL_CFG_GENIE_IN_EASE

/* --------------------------------------------------------------------------
 * Shape parameters of the Genie ("suck in") transition (cgl_warp_t in
 * src/core/cgl_canvas.h)
 *
 * CGL_CFG_GENIE_NARROW_Q8 is the width (Q8) of **the end nearest the suction
 * point** once fully sucked in: 64 = 25%. It sets how "sharp" the funnel is --
 * bigger looks more like a horn, smaller more like an awl. The width of the
 * middle section is filled in by a quadratic curve, so both sides are concave
 * (straight lines would make a triangular cone, which does not look like a
 * sprite).
 *
 * CGL_CFG_GENIE_DROP_PX is how many pixels **below** the bottom edge of the page
 * the suction point sits. Put outside the screen, the page's last frame leaves
 * the visible area completely and the finish leaves no line at the edge.
 * ------------------------------------------------------------------------ */
#define CGL_CFG_GENIE_NARROW_Q8 64u
#define CGL_CFG_GENIE_DROP_PX 6

/* --------------------------------------------------------------------------
 * Shape parameters of S-warp suction (CGL_TRANS_S_OUT / CGL_TRANS_S_IN)
 *
 * S-warp = the vertical-suction geometry (the same warp funnel) **with a
 * different suction point and a horizontal trajectory**: the suction point is
 * the **bottom-right corner** of the screen, and the page being sucked does not
 * drop straight down but first arches to the right and then tucks into the
 * corner -- horizontally it follows a bell-shaped curve (tangent to the screen's
 * centre axis at both sides, bulging out the most in the middle; see warp__s()
 * in cgl_canvas.c).
 *
 * CGL_CFG_S_AMP_MAX_Q8 is the upper bound on that curve's **maximum offset**, in
 * Q8 (256 = 1 pixel/pixel). It is the amplitude knob and also the **monotonicity
 * bound**.
 *
 * THIS MACRO OWNS THAT DERIVATION. Three documents used to quote three different
 * limits for the same quantity (170 in cgl_canvas.h, 682 here and in
 * cgl_canvas.c); 682 is the intended one. If the number ever changes, change it
 * here and let the other files refer to this macro instead of restating a value.
 *
 * Why 682, and what it does and does not promise:
 *   - The S term is S(y) = 3*amp*phase*g(t)/65536, with g(t) = 256u^2(1-u)^2 and
 *     u = t/256. Bounding the offset function's slope by 6/256 gives
 *     |S'| <= 3*amp*6/65536, which is < 1 pixel per source row exactly while
 *     amp <= 682. Below that, the lateral offset cannot on its own move a row
 *     far enough to overtake its neighbour, so the page cannot fold.
 *   - What it does NOT promise: strict per-row monotonicity of the *rounded*
 *     output. The column term rounds to nearest while the S term truncates, so
 *     even at tiny amplitudes the rounded mapping shows occasional 1-2 px rises
 *     between adjacent rows. That is rounding, not a crossing, and
 *     tests/page_host_test.c accounts for it with a tolerance. At amp = 0 (S
 *     switched off entirely) the funnel mapping is exactly monotone.
 *   - A genuine fold would show up as steps on the order of the page width, not
 *     as a single-digit pixel step.
 *
 * The shipped default is 160, which is exactly what nav__s_amp() derives for a
 * 320 px panel (half the screen width -- the horizontal distance the source
 * centre actually has to travel when the page is sucked to the right edge), so
 * the cap and the derived value agree by construction. It also sits far below
 * 682, leaving room for the rounding described above. Do not set it above 680;
 * a test asserts this row by row (section G2 of tests/page_host_test.c).
 *
 * CGL_CFG_S_DROP_PX is how many pixels **below** the bottom-right corner the
 * suction point sits, for the same reason as the Genie one: put outside the
 * screen, the last frame leaves no ghost.
 * ------------------------------------------------------------------------ */
#define CGL_CFG_S_AMP_MAX_Q8 160u
#define CGL_CFG_S_DROP_PX CGL_CFG_GENIE_DROP_PX

/* --------------------------------------------------------------------------
 * Input queue (src/core/cgl_screen.h).
 *
 * Events are queued at the frame boundary instead of being dispatched the
 * instant they happen: a page is never entered or left in the middle of a
 * frame. Must be a power of two.
 * ------------------------------------------------------------------------ */
#define CGL_CFG_INPUT_QUEUE 8

/* --------------------------------------------------------------------------
 * Default palette used by the built in widgets.
 * ------------------------------------------------------------------------ */
#define CGL_CFG_BG_COLOR CGL_COLOR_DARK
#define CGL_CFG_FG_COLOR CGL_COLOR_CREAM
#define CGL_CFG_ACCENT_COLOR CGL_COLOR_AMBER

#endif /* CGL_CONFIG_H */


