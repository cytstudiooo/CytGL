// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_bar.c
  * @brief   LCD UI framework - progress / level bar widget.
  *
  *  Layout, and the two things that are easy to get wrong
  *  ----------------------------------------------------
  *  The bar fills its **whole frame** with `track` first, then the fill, then
  *  the separators, then the border ring. In that order, and in one pass:
  *
  *    - the track is what makes the widget self-clearing. The framework does not
  *      clear anything for a widget (a band renderer has no previous frame to
  *      compare against), so a bar that only painted its filled part would leave
  *      the tail of a longer bar behind the moment the value drops;
  *    - the border goes **last** so the fill can never overwrite it, which is
  *      also why `border_w` needs no separate "inner inset" knob.
  *
  *  The value -> pixel mapping is `bar__frac_q8()` + a rounded multiply. See the
  *  header for why that is not the obvious two-line division (`int64` would drag
  *  `__aeabi_ldivmod` into the firmware, and `(off << 8)` overflows), and see
  *  tests/bar_host_test.c for the pixel-exact spec of the result.
  ******************************************************************************
  */
#include <cytgl/cgl_bar.h>
/* cgl_math_frac_q8() -- the shared value -> fraction mapping (see below). It
   arrives through cgl_bar.h -> cgl_widget.h -> cgl_canvas.h -> cgl_math.h, but
   this file names it directly, so it says so directly. */
#include <cytgl/cgl_math.h>

/* -------------------------------------------------------------------------- */
/* rejected-range counter (observable, same pattern as the widget counters)    */
/* -------------------------------------------------------------------------- */

static uint32_t s_range_rejected = 0u;

uint32_t cgl_bar_range_rejected(void)
{
    return s_range_rejected;
}

/* -------------------------------------------------------------------------- */
/* value -> fraction                                                          */
/* -------------------------------------------------------------------------- */

/**
 * `(value - vmin) / (vmax - vmin)` as Q8 (0..256), rounded to nearest.
 *
 * Everything is done in **unsigned** 32-bit arithmetic: `vmax - vmin` is not a
 * valid `int32_t` for a range such as `INT32_MIN..INT32_MAX`, but it is a
 * perfectly good `uint32_t`, and `value - vmin` is then a plain unsigned
 * offset. Signed subtraction here would be undefined behaviour on exactly the
 * extremes the type allows.
 *
 * The fractional bits come out of a shift/subtract long division instead of
 * `(off << 8) / span`, because the latter overflows for any range wider than
 * 2^23 and `int64` is banned in this project (cgl_anim.h explains: it drags
 * `__aeabi_ldivmod` in from libgcc). Nine iterations of compare/subtract/shift
 * per draw is nothing; per pixel it would be a disaster, which is why the
 * result is computed once and then multiplied by the length.
 *
 * Why **nine** iterations and a `(q + 1) >> 1`: eight give a truncating Q8, and
 * truncation costs up to a whole pixel of fill on a short bar (a value exactly
 * half way up a 5 px bar drew 2 px instead of 3). Carrying one extra bit and
 * rounding it off is round-to-nearest for the same price as the eight-bit
 * version. The end points do not depend on it -- `value <= vmin` and
 * `value >= vmax` are answered before the loop.
 *
 * WARNING: the body of this function moved to `cgl_math_frac_q8()` when
 * `cgl_arc_gauge` needed the same mapping for an angular sweep. A bar and a gauge
 * that disagreed about where half way is would be a bug nobody would find by
 * looking at either one, so there is exactly one implementation and this is now a
 * call to it. It used to be spelled out here, in full, for the reason the
 * framework spells out hot paths -- but this one runs once per *draw*, not once
 * per pixel, so the extra call is free and the duplication was not.
 */
static uint16_t bar__frac_q8(int32_t value, int32_t vmin, int32_t vmax)
{
    return cgl_math_frac_q8(value, vmin, vmax);
}

/** Rounded `len x frac / 256`: 0 at frac 0 and exactly `len` at frac 256. */
static int16_t bar__px(int16_t len, uint16_t frac_q8)
{
    return (int16_t)((((int32_t)len * (int32_t)frac_q8) + 128) >> 8);
}

/* -------------------------------------------------------------------------- */
/* value handling                                                             */
/* -------------------------------------------------------------------------- */

static int32_t bar__clamp(const cgl_bar_t *b, int32_t value)
{
    if (value < b->vmin)
    {
        return b->vmin;
    }
    if (value > b->vmax)
    {
        return b->vmax;
    }
    return value;
}

/** Put the drawn value exactly on `target` with no tween in flight. */
static void bar__jump(cgl_bar_t *b, int32_t target)
{
    /* Public-API only, deliberately: a caller may hand a bar to code that also
       drives tweens, and poking at cgl_anim_t internals here would be the one
       place that assumes nobody else touches `anim`. */
    cgl_anim_start(&b->anim, target, target, 0u, CGL_EASE_LINEAR);
    cgl_anim_finish(&b->anim);
}

/** `percent` (0..100) of the range, as a value. Exact, overflow free. */
static int32_t bar__value_at_percent(const cgl_bar_t *b, uint8_t percent)
{
    uint32_t span = (uint32_t)b->vmax - (uint32_t)b->vmin;
    uint32_t step = span / 100u;
    uint32_t rem = span % 100u;

    /* step x percent + (rem x percent) / 100 never exceeds span: step x 100 + rem
       == span, and (rem x percent) / 100 <= rem because percent <= 100. */
    return (int32_t)((uint32_t)b->vmin + (step * (uint32_t)percent) +
                     ((rem * (uint32_t)percent) / 100u));
}

/* -------------------------------------------------------------------------- */
/* drawing                                                                    */
/* -------------------------------------------------------------------------- */

/** The area the fill and the separators live in: the frame minus the border. */
static cgl_rect_t bar__inner(const cgl_bar_t *b)
{
    return cgl_rect((int16_t)(b->base.frame.x0 + b->border_w),
                    (int16_t)(b->base.frame.y0 + b->border_w),
                    (int16_t)(b->base.frame.x1 - b->border_w),
                    (int16_t)(b->base.frame.y1 - b->border_w));
}

static void bar__draw_fill(const cgl_bar_t *b, cgl_canvas_t *c, cgl_rect_t inner,
                           int16_t len, uint16_t frac_q8)
{
    int16_t px;
    cgl_rect_t r;

    px = bar__px(len, frac_q8);
    if (px <= 0)
    {
        return; /* at the low end of the range there is only the track */
    }

    if (b->orient == (uint8_t)CGL_BAR_VERTICAL)
    {
        if (b->reverse != 0u)
        {
            r = cgl_rect(inner.x0, inner.y0, inner.x1, (int16_t)(inner.y0 + px - 1));
        }
        else
        {
            r = cgl_rect(inner.x0, (int16_t)(inner.y1 - px + 1), inner.x1, inner.y1);
        }
    }
    else
    {
        if (b->reverse != 0u)
        {
            r = cgl_rect((int16_t)(inner.x1 - px + 1), inner.y0, inner.x1, inner.y1);
        }
        else
        {
            r = cgl_rect(inner.x0, inner.y0, (int16_t)(inner.x0 + px - 1), inner.y1);
        }
    }
    cgl_canvas_fill_rect(c, r, b->fill);
}

static void bar__draw_segments(const cgl_bar_t *b, cgl_canvas_t *c, cgl_rect_t inner,
                               int16_t len, uint8_t vertical)
{
    uint8_t i;
    int16_t n;

    if (b->segments <= 1u)
    {
        return;
    }
    /* A bar shorter than its segment count would draw the same line over and
       over, so cap the work at one separator per pixel. */
    n = (int16_t)b->segments;
    if (n > len)
    {
        n = len;
    }
    for (i = 1u; i < (uint8_t)n; i++)
    {
        int16_t at = (int16_t)(((int32_t)i * (int32_t)len) / (int32_t)n);
        if (vertical != 0u)
        {
            cgl_canvas_hline(c, inner.x0, inner.x1, (int16_t)(inner.y0 + at), b->seg);
        }
        else
        {
            cgl_canvas_vline(c, (int16_t)(inner.x0 + at), inner.y0, inner.y1, b->seg);
        }
    }
}

static void bar__draw_border(const cgl_bar_t *b, cgl_canvas_t *c)
{
    uint8_t i;

    for (i = 0u; i < b->border_w; i++)
    {
        cgl_rect_t r = cgl_rect((int16_t)(b->base.frame.x0 + i),
                                (int16_t)(b->base.frame.y0 + i),
                                (int16_t)(b->base.frame.x1 - i),
                                (int16_t)(b->base.frame.y1 - i));
        if (cgl_rect_is_empty(r))
        {
            break;
        }
        cgl_canvas_rect(c, r, b->border);
    }
}

static void bar__draw(cgl_widget_t *w, cgl_canvas_t *c)
{
    cgl_bar_t *b = (cgl_bar_t *)w;
    cgl_rect_t inner;

    /* The track is the whole frame, border included: it is also this widget's
       background, and the border ring is painted over it below. */
    cgl_canvas_fill_rect(c, w->frame, b->track);

    inner = bar__inner(b);
    if (!cgl_rect_is_empty(inner))
    {
        uint8_t vertical = (uint8_t)((b->orient == (uint8_t)CGL_BAR_VERTICAL) ? 1u : 0u);
        int16_t len = (vertical != 0u) ? cgl_rect_height(inner) : cgl_rect_width(inner);
        uint16_t frac = bar__frac_q8(b->anim.value, b->vmin, b->vmax);

        bar__draw_fill(b, c, inner, len, frac);
        bar__draw_segments(b, c, inner, len, vertical);
    }

    /* Painted last, and also when there is no inner rectangle left (the frame was
       shrunk below `border_w` after the border was set): what is left then is an
       all-border bar, which is what `cgl_bar_set_border()` documents. Skipping
       the ring here would leave a bar that silently has no border at all. */
    bar__draw_border(b, c);
}

static void bar__update(cgl_widget_t *w, uint32_t dt_ms)
{
    cgl_bar_t *b = (cgl_bar_t *)w;

    if (!cgl_anim_running(&b->anim))
    {
        return;
    }
    /* The tween returns true when the drawn value changed, which is exactly the
       condition under which the pixels can change. */
    if (cgl_anim_tick(&b->anim, dt_ms))
    {
        cgl_widget_invalidate(w);
    }
}

/* -------------------------------------------------------------------------- */
/* vtable                                                                     */
/* -------------------------------------------------------------------------- */

static const cgl_widget_vt_t bar__vt = {
    /* Designated initialisers, never positional: cgl_widget_vt_t grows members
       as the framework evolves and positional initialisation silently shifts. */
    .type = "bar",        /* so a debugger can tell what widget this is */
    .draw = bar__draw,    /* called every frame (once per band)          */
    .update = bar__update,/* drives cgl_bar_animate_to()                 */
    .event = NULL,        /* a bar is an output only, it takes no input  */
};

/* -------------------------------------------------------------------------- */
/* API                                                                        */
/* -------------------------------------------------------------------------- */

void cgl_bar_init(cgl_bar_t *b, const char *name, cgl_rect_t frame,
                  cgl_color_t track, cgl_color_t fill)
{
    cgl_widget_init(&b->base, &bar__vt, name, frame);
    b->track = track;
    b->fill = fill;
    b->border = track; /* "no border", and the value the ring would use if added */
    b->seg = track;    /* "no separators", likewise */
    b->border_w = 0u;
    b->segments = 1u;
    b->orient = (uint8_t)CGL_BAR_HORIZONTAL;
    b->reverse = 0u;
    b->vmin = 0;
    b->vmax = 100;
    b->value = 0;
    cgl_anim_init(&b->anim); /* value 0: already consistent with `value` above */
}

bool cgl_bar_set_range(cgl_bar_t *b, int32_t vmin, int32_t vmax)
{
    int32_t target;
    bool changed;

    if (b == NULL)
    {
        return false;
    }
    if (vmax <= vmin)
    {
        /* Rejected and counted rather than "fixed up": a bar whose range is
           silently something else draws a plausible-looking wrong fraction,
           which is the hardest kind of bug to see. */
        s_range_rejected++;
        return false;
    }

    changed = (b->vmin != vmin) || (b->vmax != vmax);
    b->vmin = vmin;
    b->vmax = vmax;
    target = bar__clamp(b, b->value);
    if (changed || (target != b->anim.value))
    {
        /* The same value draws somewhere else once the range changed, so a
           range change always damages, even when the value itself did not. */
        b->value = target;
        bar__jump(b, target);
        cgl_widget_invalidate(&b->base);
    }
    return true;
}

void cgl_bar_set_value(cgl_bar_t *b, int32_t value)
{
    int32_t target;

    if (b == NULL)
    {
        return;
    }
    target = bar__clamp(b, value);
    b->value = target;
    if ((b->anim.value == target) && !cgl_anim_running(&b->anim))
    {
        return; /* the value did not change, so do not mark damage */
    }
    bar__jump(b, target);
    cgl_widget_invalidate(&b->base);
}

void cgl_bar_set_percent(cgl_bar_t *b, uint8_t percent)
{
    if (b == NULL)
    {
        return;
    }
    if (percent > 100u)
    {
        percent = 100u;
    }
    cgl_bar_set_value(b, bar__value_at_percent(b, percent));
}

bool cgl_bar_animate_to(cgl_bar_t *b, int32_t value, uint32_t dur_ms, cgl_ease_t ease)
{
    int32_t target;

    if (b == NULL)
    {
        return false;
    }
    target = bar__clamp(b, value);
    b->value = target;
    if ((dur_ms == 0u) || (target == b->anim.value))
    {
        /* Both cases mean "there is nothing to tween": fall through to the
           immediate path so the damage decision stays in exactly one place. */
        cgl_bar_set_value(b, target);
        return false;
    }
    /* Start from what is drawn, not from the previous target: retargeting in
       mid-flight continues from the current pixel instead of jumping back. */
    cgl_anim_start(&b->anim, b->anim.value, target, dur_ms, ease);
    return true;
}

void cgl_bar_set_orient(cgl_bar_t *b, cgl_bar_orient_t orient)
{
    uint8_t v;

    if (b == NULL)
    {
        return;
    }
    v = (uint8_t)((orient == CGL_BAR_VERTICAL) ? CGL_BAR_VERTICAL : CGL_BAR_HORIZONTAL);
    if (b->orient == v)
    {
        return;
    }
    b->orient = v;
    cgl_widget_invalidate(&b->base);
}

void cgl_bar_set_reverse(cgl_bar_t *b, bool reverse)
{
    uint8_t v;

    if (b == NULL)
    {
        return;
    }
    v = reverse ? 1u : 0u;
    if (b->reverse == v)
    {
        return;
    }
    b->reverse = v;
    cgl_widget_invalidate(&b->base);
}

void cgl_bar_set_colors(cgl_bar_t *b, cgl_color_t track, cgl_color_t fill)
{
    if (b == NULL)
    {
        return;
    }
    if ((b->track == track) && (b->fill == fill))
    {
        return;
    }
    b->track = track;
    b->fill = fill;
    cgl_widget_invalidate(&b->base);
}

void cgl_bar_set_border(cgl_bar_t *b, cgl_color_t color, uint8_t width)
{
    int16_t side;

    if (b == NULL)
    {
        return;
    }
    side = cgl_rect_width(b->base.frame);
    if (cgl_rect_height(b->base.frame) < side)
    {
        side = cgl_rect_height(b->base.frame);
    }
    if (side < 1)
    {
        side = 1;
    }
    /* Leave at least one pixel line of bar between the two sides of the ring:
       "all border, no bar" is never what a caller means, and `bar__draw()` would
       then have no inner rectangle to fill. A bar thinner than 3 px therefore
       gets no border at all, which is the honest answer. */
    if (width > (uint8_t)((side - 1) / 2))
    {
        width = (uint8_t)((side - 1) / 2);
    }
    if ((b->border == color) && (b->border_w == width))
    {
        return;
    }
    b->border = color;
    b->border_w = width;
    cgl_widget_invalidate(&b->base);
}

void cgl_bar_set_segments(cgl_bar_t *b, uint8_t segments, cgl_color_t color)
{
    if (b == NULL)
    {
        return;
    }
    if (segments < 1u)
    {
        segments = 1u;
    }
    if ((b->segments == segments) && (b->seg == color))
    {
        return;
    }
    b->segments = segments;
    b->seg = color;
    cgl_widget_invalidate(&b->base);
}

int32_t cgl_bar_get_value(const cgl_bar_t *b)
{
    return (b != NULL) ? b->value : 0;
}

int32_t cgl_bar_get_shown_value(const cgl_bar_t *b)
{
    return (b != NULL) ? b->anim.value : 0;
}

uint8_t cgl_bar_get_percent(const cgl_bar_t *b)
{
    uint16_t frac;

    if (b == NULL)
    {
        return 0u;
    }
    frac = bar__frac_q8(b->value, b->vmin, b->vmax);
    return (uint8_t)(((uint32_t)frac * 100u + 128u) >> 8);
}

bool cgl_bar_is_animating(const cgl_bar_t *b)
{
    return (b != NULL) && cgl_anim_running(&b->anim);
}
