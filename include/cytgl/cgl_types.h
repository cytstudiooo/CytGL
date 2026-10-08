// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_types.h
  * @brief   LCD UI framework - basic types (colors, points, rects).
  *
  *  This module is hardware independent: it must not include any HAL header.
  ******************************************************************************
  */
#ifndef CGL_TYPES_H
#define CGL_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* --------------------------------------------------------------------------
 * CGL_AINLINE -- force-inline small hot-path functions
 *
 * The on-target firmware is compiled with `-fno-inline` (the call stack reads
 * better while debugging), yet on the pixel-level hot path one pixel crosses 5~7
 * such two-or-three-line functions (canvas__row / canvas__tx / canvas__ty /
 * canvas__wx / canvas__wy / cgl_rect_* ...). Measured under `-fno-inline`, that
 * chain alone eats the bulk of the whole frame's CPU (100+ cycles per pixel,
 * while the function bodies together are under 10 instructions).
 *
 * `always_inline` is **not affected by `-fno-inline` / `-O0`**, so it is declared
 * explicitly here instead of relying on build options -- the framework itself
 * guarantees the shape of the hot path, and changing compiler options cannot
 * quietly undo it. The semantics are completely unchanged (pure inlining, no
 * behaviour change), only the code size grows slightly.
 * ------------------------------------------------------------------------ */
#if defined(__GNUC__) || defined(__clang__)
#define CGL_AINLINE static inline __attribute__((always_inline))
#else
#define CGL_AINLINE static inline
#endif

/* --------------------------------------------------------------------------
 * Colors: native RGB565, 5-6-5. The physical bus/panel byte order is the
 * display driver's business, never the framework's.
 * ------------------------------------------------------------------------ */
typedef uint16_t cgl_color_t;

#define CGL_RGB565(r, g, b)                          \
    ((cgl_color_t)((((uint16_t)(r) & 0xF8u) << 8) |  \
                  (((uint16_t)(g) & 0xFCu) << 3) |  \
                  (((uint16_t)(b) & 0xF8u) >> 3)))

#define CGL_COLOR_BLACK   ((cgl_color_t)0x0000u)
#define CGL_COLOR_WHITE   ((cgl_color_t)0xFFFFu)
#define CGL_COLOR_RED     CGL_RGB565(255, 0, 0)
#define CGL_COLOR_GREEN   CGL_RGB565(0, 255, 0)
#define CGL_COLOR_BLUE    CGL_RGB565(0, 0, 255)
#define CGL_COLOR_YELLOW  CGL_RGB565(255, 255, 0)
#define CGL_COLOR_CYAN    CGL_RGB565(0, 255, 255)
#define CGL_COLOR_MAGENTA CGL_RGB565(255, 0, 255)
#define CGL_COLOR_GRAY    CGL_RGB565(128, 128, 128)
#define CGL_COLOR_DARK    CGL_RGB565(16, 18, 24)
#define CGL_COLOR_DIM     CGL_RGB565(64, 70, 84)
#define CGL_COLOR_AMBER   CGL_RGB565(255, 176, 32)
#define CGL_COLOR_CREAM   CGL_RGB565(238, 232, 213)

/* --------------------------------------------------------------------------
 * Alpha blending, one pixel -- the framework's ONLY blend formula.
 *
 * Every blend path goes through it: `cgl_canvas_fill_rect_blend_q8()` (constant
 * colour fill) and `cgl_canvas_blend_src_q8()` (per-pixel source, the group
 * opacity compositing step). It lives here rather than being reimplemented in
 * each place so that the two can never compute different results -- that formula
 * used to be copied three times inside the canvas (the warp branch and the
 * non-warp branch), and any change in one place would silently fork the two
 * paths.
 *
 * alpha_q8 is Q8: 0 = keep dst entirely, 256 = become src entirely. Both end
 * points are **exact** (dst*256>>8 == dst, src*256>>8 == src), so callers may
 * safely pass the end points in as well.
 *
 * Rounding is **truncation** (>>8), not round-to-nearest: that is the framework's
 * established behaviour, and changing it would alter the pixel values of every
 * existing image. The cost is an error of at most 1 LSB per level (3.2% of a
 * 5-bit channel), and at most 2 LSB when two levels are nested.
 * ------------------------------------------------------------------------ */
CGL_AINLINE cgl_color_t cgl_color_blend_q8(cgl_color_t dst, cgl_color_t src,
                                           uint16_t alpha_q8)
{
    uint32_t inv = (uint32_t)(256u - alpha_q8);
    uint32_t a = (uint32_t)alpha_q8;
    uint16_t r5 = (uint16_t)(((((dst >> 11) & 0x1Fu) * inv) +
                              (((src >> 11) & 0x1Fu) * a)) >> 8);
    uint16_t g6 = (uint16_t)(((((dst >> 5) & 0x3Fu) * inv) +
                              (((src >> 5) & 0x3Fu) * a)) >> 8);
    uint16_t b5 = (uint16_t)(((((dst & 0x1Fu) * inv) +
                               ((src & 0x1Fu) * a)) >> 8));
    return (cgl_color_t)((r5 << 11) | (g6 << 5) | b5);
}

/* --------------------------------------------------------------------------
 * Geometry. All coordinates are signed and expressed in screen pixels.
 * Rectangles are inclusive on both edges: width == x1 - x0 + 1.
 * ------------------------------------------------------------------------ */
typedef struct
{
    int16_t x;
    int16_t y;
} cgl_point_t;

typedef struct
{
    int16_t x0;
    int16_t y0;
    int16_t x1;
    int16_t y1;
} cgl_rect_t;

CGL_AINLINE cgl_rect_t cgl_rect(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    cgl_rect_t r;
    r.x0 = x0;
    r.y0 = y0;
    r.x1 = x1;
    r.y1 = y1;
    return r;
}

CGL_AINLINE cgl_rect_t cgl_rect_wh(int16_t x, int16_t y, int16_t w, int16_t h)
{
    return cgl_rect(x, y, (int16_t)(x + w - 1), (int16_t)(y + h - 1));
}

/* A rectangle that is guaranteed to be empty (x0 > x1). */
CGL_AINLINE cgl_rect_t cgl_rect_none(void)
{
    return cgl_rect(0, 0, -1, -1);
}

CGL_AINLINE bool cgl_rect_is_empty(cgl_rect_t r)
{
    return (r.x0 > r.x1) || (r.y0 > r.y1);
}

CGL_AINLINE int16_t cgl_rect_width(cgl_rect_t r)
{
    return (int16_t)(r.x1 - r.x0 + 1);
}

CGL_AINLINE int16_t cgl_rect_height(cgl_rect_t r)
{
    return (int16_t)(r.y1 - r.y0 + 1);
}

CGL_AINLINE bool cgl_rect_contains_point(cgl_rect_t r, int16_t x, int16_t y)
{
    return (x >= r.x0) && (x <= r.x1) && (y >= r.y0) && (y <= r.y1);
}

CGL_AINLINE bool cgl_rect_contains_rect(cgl_rect_t r, cgl_rect_t o)
{
    if (cgl_rect_is_empty(o))
    {
        return true;
    }
    return (o.x0 >= r.x0) && (o.x1 <= r.x1) && (o.y0 >= r.y0) && (o.y1 <= r.y1);
}

CGL_AINLINE cgl_rect_t cgl_rect_intersect(cgl_rect_t a, cgl_rect_t b)
{
    cgl_rect_t r;
    r.x0 = (a.x0 > b.x0) ? a.x0 : b.x0;
    r.y0 = (a.y0 > b.y0) ? a.y0 : b.y0;
    r.x1 = (a.x1 < b.x1) ? a.x1 : b.x1;
    r.y1 = (a.y1 < b.y1) ? a.y1 : b.y1;
    return r;
}

CGL_AINLINE cgl_rect_t cgl_rect_union(cgl_rect_t a, cgl_rect_t b)
{
    cgl_rect_t r;
    if (cgl_rect_is_empty(a))
    {
        return b;
    }
    if (cgl_rect_is_empty(b))
    {
        return a;
    }
    r.x0 = (a.x0 < b.x0) ? a.x0 : b.x0;
    r.y0 = (a.y0 < b.y0) ? a.y0 : b.y0;
    r.x1 = (a.x1 > b.x1) ? a.x1 : b.x1;
    r.y1 = (a.y1 > b.y1) ? a.y1 : b.y1;
    return r;
}

/** Translate a rectangle (page-slide and subtree offset both use it to turn local coordinates into screen coordinates). */
CGL_AINLINE cgl_rect_t cgl_rect_translate(cgl_rect_t r, cgl_point_t d)
{
    cgl_rect_t o;
    o.x0 = (int16_t)(r.x0 + d.x);
    o.y0 = (int16_t)(r.y0 + d.y);
    o.x1 = (int16_t)(r.x1 + d.x);
    o.y1 = (int16_t)(r.y1 + d.y);
    return o;
}

/* --------------------------------------------------------------------------
 * Input events.
 *
 * An event is a "one-shot action" and is unrelated to geometry, so it does not
 * belong to the widget tree; the widget tree is only responsible for
 * **distributing** the event downwards (cgl_widget_dispatch_event) and the page
 * decides who is allowed to receive it (cgl_page.h). It lives here to avoid
 * opening another header just for 5 fields.
 * ------------------------------------------------------------------------ */
typedef enum
{
    CGL_EV_NONE = 0,
    CGL_EV_KEY_DOWN,   /**< key was just pressed                            */
    CGL_EV_KEY_REPEAT, /**< auto-repeat while held                          */
    CGL_EV_KEY_LONG,   /**< held past the threshold, emitted **once only**   */
    CGL_EV_KEY_UP,     /**< released (hold_ms = the full duration held)     */
    CGL_EV_TOUCH_DOWN,
    CGL_EV_TOUCH_MOVE,
    CGL_EV_TOUCH_UP,
    CGL_EV_BACK        /**< logical "back" (encoder side key / gesture / remote) */
} cgl_event_kind_t;

typedef struct
{
    uint8_t kind;    /**< cgl_event_kind_t                                 */
    uint8_t key;     /**< key index; always 0 on a single-key board        */
    int16_t x;       /**< touch coordinate (0 for keyboard events)         */
    int16_t y;
    uint32_t hold_ms;/**< duration from press to this event                */
} cgl_event_t;

#endif /* CGL_TYPES_H */
