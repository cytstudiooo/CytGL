// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    arc_host_test.c
  * @brief   Spec of the radial primitives (`cgl_math`, and `cgl_canvas_circle` /
  *          `_line_thick` / `_arc`) -- geometry **and** antialiasing.
  *
  *  What this file is for
  *  ---------------------
  *  "It looks like a circle" is not a specification, and both halves of these
  *  primitives fail in ways a snapshot cannot name. The geometry half is the easy
  *  one; the antialiasing half is the one that quietly does nothing (drawing a
  *  *thinner* crisp shape instead of a smooth one, which looks like a dimmer line
  *  rather than a bug), so each claim below is measurable:
  *
  *   1. **The trig is accurate to the table's own rounding** -- all 65536 angles
  *      against libm (the *test* may use libm; the framework may not), inside 1 LSB
  *      of Q8, which is what cgl_math.h claims. Plus the exact quadrant boundaries
  *      and the odd symmetry, because a mirroring bug is a kink at 90 degrees.
  *   2. **`cgl_math_isqrt` is the floor of the square root** over the whole range a
  *      320x172 panel can produce, and it is the function that lets the arc find a
  *      row's half-width without libm.
  *   3. **A circle is a circle**: every touched pixel is within `radius + 1` of the
  *      centre, it is closed (no direction is empty), and the pixels on the centre
  *      line are the **exact** colour (full coverage).
  *   4. **The antialiasing is real**: the boundary of a stroke carries **partial**
  *      coverage -- pixels that are neither the background nor the stroke colour --
  *      and the coverage falls off monotonically across the edge. A crisp shape
  *      fails this by having no such pixels at all; a "fatter crisp" shape fails it
  *      too. This is the assertion that would have caught "AA quietly not applied".
  *   5. **A thick line is the capsule it claims to be**: nothing outside
  *      `pen + 1`, the core is exact, the round cap is *softer* at its corner than
  *      on its axis (which is what "round" means once coverage exists), and the
  *      centre line is covered end to end -- the test that catches the gaps a
  *      per-column implementation leaves on a steep slope.
  *   6. **`thickness <= 1` really is plain Bresenham**, pixel for pixel: the crisp
  *      hairline is still there for ticks and grids, and the two must not drift.
  *   7. **The ring does not shimmer.** Two sweeps of the same arc, a fraction of a
  *      degree apart, must leave the **already-drawn part byte-identical** -- only
  *      the new sliver at the leading end may differ. (This is the regression test
  *      for the first implementation, which stamped discs along the path with a step
  *      derived from the *current* sweep: as the arc grew, the step changed and
  *      every stamp moved by a fraction of a pixel, so a gauge visibly vibrated.)
  *   8. **The sector is honoured**: an arc draws nothing outside its sweep (beyond
  *      the cap), and `sweep == 0` draws nothing at all.
  *   9. **Pan is applied once** and clipping holds -- the classic "drawn twice, or
  *      not at all" bugs of this canvas (see the warning at the top of
  *      cgl_canvas.c).
  *
  *  The last step writes `build/arc_test.ppm` / `.bmp`: a gallery built out of
  *  nothing but these primitives, so the numbers have a picture next to them.
  *
  *  Build & run
  *  -----------
  *      cmake -B build/host -DCYTGL_BUILD_TESTS=ON
  *      cmake --build build/host
  *      ctest --test-dir build/host -R arc_test --output-on-failure
  ******************************************************************************
  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <cytgl/cgl_canvas.h>
#include <cytgl/cgl_math.h>
#include <cytgl/cgl_arc.h> /* the alpha wrappers: cgl_arc_draw_arc() and friends */
#include <cytgl/cytgl_config.h>
#include "test_image_out.h"

#define FB_W CGL_CFG_SCREEN_W
#define FB_H CGL_CFG_SCREEN_H

#define PX(x, y) s_fb[((int32_t)(y) * (int32_t)FB_W) + (int32_t)(x)]

#define C_INK CGL_RGB565(240, 244, 250) /* whatever the geometry test draws */
#define C_OUT CGL_RGB565(0, 255, 0)     /* sentinel: "this pixel was not touched" */

static cgl_color_t s_fb[FB_W * FB_H];
static cgl_color_t s_other[FB_W * FB_H];
static cgl_canvas_t s_canvas;

static int s_fails = 0;

#define CHECK(cond, ...)                                  \
    do                                                    \
    {                                                     \
        if (!(cond))                                      \
        {                                                 \
            s_fails++;                                    \
            printf("  FAIL: ");                           \
            printf(__VA_ARGS__);                          \
            printf("   [%s:%d]\n", __FILE__, __LINE__);   \
        }                                                 \
    } while (0)

/* -------------------------------------------------------------------------- */
/* harness                                                                    */
/* -------------------------------------------------------------------------- */

static void scene_begin(void)
{
    int32_t i;

    for (i = 0; i < (int32_t)(FB_W * FB_H); i++)
    {
        s_fb[i] = C_OUT;
    }
    cgl_canvas_init(&s_canvas, s_fb, (int16_t)FB_W, (int16_t)FB_H, 0);
}

/**
 * How much of the stroke colour a pixel carries, 0..256, read back out of the
 * blend.
 *
 * The pen colour is near-white and the sentinel is pure green, so the **red**
 * channel is a monotone measure of the coverage: 0 for the sentinel, 30 (5 bit) for
 * a fully covered pixel. Deriving it instead of asserting on exact colours is what
 * lets the tests below talk about "the edge is softer than the core" -- i.e. about
 * the antialiasing itself -- rather than about 16 bit constants.
 */
static int32_t px_cov(cgl_color_t c)
{
    int32_t r5 = (int32_t)((c >> 11) & 0x1Fu);

    return (r5 * 256) / 30;
}

/** Coverage of the pixel at (x,y), 0 when it was never touched. */
static int32_t px_conv_none(int16_t x, int16_t y)
{
    cgl_color_t c = PX(x, y);

    return (c == C_OUT) ? 0 : px_cov(c);
}

/** Two RGB565 colours within one level of each channel (see the alpha test for why
    that, and not bit equality, is the right tolerance for a scaled coverage). */
static bool color_near(cgl_color_t a, cgl_color_t b)
{
    int32_t dr = (int32_t)((a >> 11) & 0x1Fu) - (int32_t)((b >> 11) & 0x1Fu);
    int32_t dg = (int32_t)((a >> 5) & 0x3Fu) - (int32_t)((b >> 5) & 0x3Fu);
    int32_t db = (int32_t)(a & 0x1Fu) - (int32_t)(b & 0x1Fu);

    if (dr < 0)
    {
        dr = -dr;
    }
    if (dg < 0)
    {
        dg = -dg;
    }
    if (db < 0)
    {
        db = -db;
    }
    return (dr <= 1) && (dg <= 1) && (db <= 1);
}

/** Distance from (x, y) to the segment (x0,y0)-(x1,y1). */
static double seg_dist(double x, double y, double x0, double y0, double x1, double y1)
{
    double dx = x1 - x0;
    double dy = y1 - y0;
    double len2 = (dx * dx) + (dy * dy);
    double t;
    double px;
    double py;

    if (len2 <= 0.0)
    {
        return hypot(x - x0, y - y0);
    }
    t = (((x - x0) * dx) + ((y - y0) * dy)) / len2;
    if (t < 0.0)
    {
        t = 0.0;
    }
    if (t > 1.0)
    {
        t = 1.0;
    }
    px = x0 + (t * dx);
    py = y0 + (t * dy);
    return hypot(x - px, y - py);
}

/* -------------------------------------------------------------------------- */
/* 1..2  cgl_math                                                             */
/* -------------------------------------------------------------------------- */

static void test_math_accuracy(void)
{
    uint32_t a;
    int32_t worst = 0;
    int32_t worst_at = 0;
    int16_t prev;

    for (a = 0u; a < 65536u; a++)
    {
        double ref = 256.0 * sin((2.0 * 3.14159265358979323846 * (double)a) / 65536.0);
        int32_t want = (int32_t)floor(ref + 0.5);
        int32_t got = (int32_t)cgl_sin_q8((cgl_angle_t)a);
        int32_t err = (got > want) ? (got - want) : (want - got);

        if (err > worst)
        {
            worst = err;
            worst_at = (int32_t)a;
        }
        ref = 256.0 * cos((2.0 * 3.14159265358979323846 * (double)a) / 65536.0);
        want = (int32_t)floor(ref + 0.5);
        got = (int32_t)cgl_cos_q8((cgl_angle_t)a);
        err = (got > want) ? (got - want) : (want - got);
        if (err > worst)
        {
            worst = err;
            worst_at = (int32_t)a;
        }
    }
    CHECK(worst <= 1, "sin/cos error %d LSB (Q8) at angle %d -- the header promises <= 1",
          (int)worst, (int)worst_at);

    CHECK(cgl_sin_q8(0u) == 0, "sin(0) = %d", (int)cgl_sin_q8(0u));
    CHECK(cgl_sin_q8(CGL_ANGLE_QUARTER) == 256, "sin(90) = %d", (int)cgl_sin_q8(CGL_ANGLE_QUARTER));
    CHECK(cgl_sin_q8(CGL_ANGLE_HALF) == 0, "sin(180) = %d", (int)cgl_sin_q8(CGL_ANGLE_HALF));
    CHECK(cgl_sin_q8(CGL_ANGLE_UP) == -256, "sin(270) = %d", (int)cgl_sin_q8(CGL_ANGLE_UP));
    CHECK(cgl_cos_q8(0u) == 256, "cos(0) = %d", (int)cgl_cos_q8(0u));
    CHECK(cgl_cos_q8(CGL_ANGLE_HALF) == -256, "cos(180) = %d", (int)cgl_cos_q8(CGL_ANGLE_HALF));

    for (a = 0u; a < 65536u; a++)
    {
        int16_t s0 = cgl_sin_q8((cgl_angle_t)a);
        int16_t s1 = cgl_sin_q8((cgl_angle_t)(a + (uint32_t)CGL_ANGLE_HALF));

        if (s1 != (int16_t)(-s0))
        {
            CHECK(false, "sin(a+180) != -sin(a) at a = %u (%d vs %d)", (unsigned)a, (int)s0, (int)s1);
            break;
        }
    }

    prev = cgl_sin_q8(0u);
    for (a = 0u; a <= (uint32_t)CGL_ANGLE_QUARTER; a += 16u)
    {
        int16_t v = cgl_sin_q8((cgl_angle_t)a);

        if (v < prev)
        {
            CHECK(false, "sin not monotone at angle %u (%d < %d)", (unsigned)a, (int)v, (int)prev);
            break;
        }
        prev = v;
    }
}

static void test_math_isqrt(void)
{
    uint32_t v;
    uint32_t worst_at = 0u;
    int32_t worst = 0;

    /* Every value a 320x172 panel can produce: a coordinate offset squared, twice,
       i.e. up to 2 * 320^2 ~ 205000, and the Q12 forms go higher -- so check the
       whole range up to 1 << 20 exhaustively (a million checks, no libm in the
       framework, libm here). */
    for (v = 0u; v < (1u << 20); v++)
    {
        uint32_t got = cgl_math_isqrt(v);
        uint32_t want = (uint32_t)sqrt((double)v);
        int32_t err = (int32_t)got - (int32_t)want;

        if ((err < 0) ? (-err > worst) : (err > worst))
        {
            worst = (err < 0) ? -err : err;
            worst_at = v;
        }
    }
    CHECK(worst == 0, "isqrt error %d at v = %u", (int)worst, (unsigned)worst_at);

    /* The boundaries, including the top of the range (where a shift-based
       implementation goes wrong). */
    CHECK(cgl_math_isqrt(0u) == 0u, "isqrt(0) = %u", (unsigned)cgl_math_isqrt(0u));
    CHECK(cgl_math_isqrt(1u) == 1u, "isqrt(1) = %u", (unsigned)cgl_math_isqrt(1u));
    CHECK(cgl_math_isqrt(2u) == 1u, "isqrt(2) = %u", (unsigned)cgl_math_isqrt(2u));
    CHECK(cgl_math_isqrt(3u) == 1u, "isqrt(3) = %u", (unsigned)cgl_math_isqrt(3u));
    CHECK(cgl_math_isqrt(4u) == 2u, "isqrt(4) = %u", (unsigned)cgl_math_isqrt(4u));
    CHECK(cgl_math_isqrt(0xFFFFFFFFu) == 65535u, "isqrt(2^32-1) = %u",
          (unsigned)cgl_math_isqrt(0xFFFFFFFFu));
    CHECK(cgl_math_isqrt(0x40000000u) == 32768u, "isqrt(2^30) = %u",
          (unsigned)cgl_math_isqrt(0x40000000u));
}

static void test_math_polar_and_deg(void)
{
    int16_t dx = 0;
    int16_t dy = 0;

    cgl_math_polar(0u, 100, &dx, &dy);
    CHECK((dx == 100) && (dy == 0), "polar(0,100) = (%d,%d)", (int)dx, (int)dy);
    cgl_math_polar(CGL_ANGLE_QUARTER, 100, &dx, &dy);
    CHECK((dx == 0) && (dy == 100), "polar(90,100) = (%d,%d) -- 6 o'clock", (int)dx, (int)dy);
    cgl_math_polar(CGL_ANGLE_HALF, 100, &dx, &dy);
    CHECK((dx == -100) && (dy == 0), "polar(180,100) = (%d,%d)", (int)dx, (int)dy);
    cgl_math_polar(CGL_ANGLE_UP, 100, &dx, &dy);
    CHECK((dx == 0) && (dy == -100), "polar(270,100) = (%d,%d) -- 12 o'clock", (int)dx, (int)dy);

    cgl_math_polar(CGL_ANGLE_UP, 50, NULL, &dy);
    CHECK(dy == -50, "polar with NULL dx = %d", (int)dy);

    CHECK(cgl_angle_deg(0u) == 0u, "deg(0) = %u", (unsigned)cgl_angle_deg(0u));
    CHECK(cgl_angle_deg(CGL_ANGLE_QUARTER) == 90u, "deg(90) = %u", (unsigned)cgl_angle_deg(CGL_ANGLE_QUARTER));
    CHECK(cgl_angle_deg(CGL_ANGLE_HALF) == 180u, "deg(180) = %u", (unsigned)cgl_angle_deg(CGL_ANGLE_HALF));
    CHECK(cgl_angle_deg(CGL_ANGLE_UP) == 270u, "deg(270) = %u", (unsigned)cgl_angle_deg(CGL_ANGLE_UP));
    CHECK(cgl_angle_deg((cgl_angle_t)65535u) == 359u, "deg(65535) = %u", (unsigned)cgl_angle_deg((cgl_angle_t)65535u));
    CHECK(CGL_ANGLE_DEG(90) == CGL_ANGLE_QUARTER, "CGL_ANGLE_DEG(90) = %u", (unsigned)CGL_ANGLE_DEG(90));
    CHECK(CGL_ANGLE_FROM_U8(64u) == CGL_ANGLE_QUARTER, "CGL_ANGLE_FROM_U8(64) = %u", (unsigned)CGL_ANGLE_FROM_U8(64u));
    CHECK(CGL_ANGLE_TO_U8(CGL_ANGLE_UP) == 192u, "CGL_ANGLE_TO_U8(270deg) = %u", (unsigned)CGL_ANGLE_TO_U8(CGL_ANGLE_UP));
}

/* -------------------------------------------------------------------------- */
/* 3..4  cgl_canvas_circle                                                    */
/* -------------------------------------------------------------------------- */

static void test_circle(void)
{
    const int16_t cx = 80;
    const int16_t cy = 60;
    const int16_t r = 37;
    int16_t x;
    int16_t y;
    uint32_t touched = 0u;
    uint32_t partial = 0u;
    uint32_t full = 0u;
    double worst = 0.0;

    scene_begin();
    cgl_canvas_circle(&s_canvas, cx, cy, r, C_INK);

    for (y = 0; y < (int16_t)FB_H; y++)
    {
        for (x = 0; x < (int16_t)FB_W; x++)
        {
            cgl_color_t c = PX(x, y);
            double d;

            if (c == C_OUT)
            {
                continue;
            }
            touched++;
            d = hypot((double)(x - cx), (double)(y - cy));
            if (fabs(d - (double)r) > worst)
            {
                worst = fabs(d - (double)r);
            }
            if (c == C_INK)
            {
                full++;
            }
            else
            {
                partial++;
            }
        }
    }

    /* Containment: an antialiased 1 px stroke on radius r touches pixels within
       r + 0.5 (the ramp) + the pixel's own half diagonal. */
    CHECK(worst <= 1.0, "circle pixel %.2f px away from the radius (AA allows 1.0)", worst);
    CHECK(touched > 180u, "circle r=%d touched only %u pixels", (int)r, (unsigned)touched);

    /* **The antialiasing claim**: a one pixel wide circular stroke has essentially
       no fully covered pixel at all -- every pixel it touches is on the edge and
       carries a fraction. A crisp implementation would have full == touched and
       partial == 0, and that is exactly the failure this asserts against. */
    CHECK(partial >= (touched / 2u),
          "the circle is not antialiased: %u of %u touched pixels are fully covered",
          (unsigned)full, (unsigned)touched);

    /* Closed: every direction has a touched pixel. */
    {
        uint32_t i;

        for (i = 0u; i < 2048u; i++)
        {
            cgl_angle_t a = (cgl_angle_t)(i * 32u);
            int16_t ox;
            int16_t oy;
            int16_t px;
            int16_t py;
            bool hit = false;
            int16_t ddx;
            int16_t ddy;

            cgl_math_polar(a, r, &ox, &oy);
            px = (int16_t)(cx + ox);
            py = (int16_t)(cy + oy);
            for (ddy = -2; (ddy <= 2) && !hit; ddy++)
            {
                for (ddx = -2; ddx <= 2; ddx++)
                {
                    int16_t sx = (int16_t)(px + ddx);
                    int16_t sy = (int16_t)(py + ddy);

                    if ((sx >= 0) && (sx < (int16_t)FB_W) && (sy >= 0) && (sy < (int16_t)FB_H) &&
                        (PX(sx, sy) != C_OUT))
                    {
                        hit = true;
                        break;
                    }
                }
            }
            if (!hit)
            {
                CHECK(false, "circle has a gap at %u degrees", (unsigned)cgl_angle_deg(a));
                break;
            }
        }
    }

    /* The degenerate circle is a point, not a crash and not nothing. */
    scene_begin();
    cgl_canvas_circle(&s_canvas, 10, 10, 0, C_INK);
    CHECK(PX(10, 10) == C_INK, "radius 0 did not draw its centre");
}

/* -------------------------------------------------------------------------- */
/* 5..6  cgl_canvas_line_thick                                                */
/* -------------------------------------------------------------------------- */

/** Containment, exact core and antialiased edge of one thick segment. */
static void stroke_spec(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t thickness,
                        const char *what)
{
    double pen = (double)thickness / 2.0;
    double allow = pen + 0.6; /* the ramp ends half a pixel outside the edge */
    int16_t x;
    int16_t y;
    uint32_t touched = 0u;
    uint32_t partial = 0u;

    scene_begin();
    cgl_canvas_line_thick(&s_canvas, x0, y0, x1, y1, thickness, C_INK);

    for (y = 0; y < (int16_t)FB_H; y++)
    {
        for (x = 0; x < (int16_t)FB_W; x++)
        {
            cgl_color_t c = PX(x, y);
            double d;

            if (c == C_OUT)
            {
                continue;
            }
            touched++;
            d = seg_dist((double)x, (double)y, (double)x0, (double)y0, (double)x1, (double)y1);
            if (d > allow)
            {
                CHECK(false, "%s: pixel (%d,%d) is outside the capsule (d=%.2f, allow %.2f)", what,
                      (int)x, (int)y, d, allow);
            }
            /* The core is exact: no blend survives anywhere more than half a pixel
               inside the edge. Skipped for a stroke too thin to have a core. */
            if ((d <= (pen - 0.5)) && (c != C_INK))
            {
                CHECK(false, "%s: the core pixel (%d,%d) is blended (%04X)", what, (int)x, (int)y,
                      (unsigned)c);
            }
            if ((c != C_INK) && (c != C_OUT))
            {
                partial++;
            }
        }
    }
    CHECK(touched > 0u, "%s: nothing was drawn", what);
    if (thickness >= 3)
    {
        CHECK(partial > 0u, "%s: the stroke has no antialiased edge at all", what);
    }

    /* Coverage: the centre line itself is drawn, at every pixel of the major axis.
       This is what catches the gaps a per-column implementation leaves on a steep
       slope (there the centre line passes between two columns). */
    {
        int16_t steps = (int16_t)((abs((int)(x1 - x0)) > abs((int)(y1 - y0))) ? abs((int)(x1 - x0))
                                                                             : abs((int)(y1 - y0)));
        int16_t i;

        if (steps == 0)
        {
            steps = 1;
        }
        for (i = 0; i <= steps; i++)
        {
            double t = (double)i / (double)steps;
            int16_t px = (int16_t)((double)x0 + (t * (double)(x1 - x0)) + 0.5);
            int16_t py = (int16_t)((double)y0 + (t * (double)(y1 - y0)) + 0.5);

            if ((px < 0) || (px >= (int16_t)FB_W) || (py < 0) || (py >= (int16_t)FB_H))
            {
                continue;
            }
            if (px_conv_none(px, py) == 0)
            {
                CHECK(false, "%s: gap on the centre line at (%d,%d)", what, (int)px, (int)py);
            }
        }
    }
}

static void test_line_thick(void)
{
    /* A horizontal 5 px stroke from (12,20) to (60,20): the round cap means the ink
       reaches x = 10 on the axis row and stops short in the corner rows -- which,
       with coverage, means "the corner is *softer*", not "the corner is empty". */
    scene_begin();
    cgl_canvas_line_thick(&s_canvas, 12, 20, 60, 20, 5, C_INK);
    CHECK(px_cov(PX(10, 20)) > 200, "the cap does not reach its own radius (x=10, cov %d)",
          (int)px_cov(PX(10, 20)));
    CHECK(PX(10, 18) != C_OUT, "the cap corner (10,18) is not drawn at all: the cap is not round");
    CHECK(px_cov(PX(10, 18)) < px_cov(PX(10, 20)),
          "the cap corner is as strong as the axis: the cap is square (%d vs %d)",
          (int)px_cov(PX(10, 18)), (int)px_cov(PX(10, 20)));
    CHECK(px_cov(PX(11, 18)) < px_cov(PX(12, 20)),
          "the cap is not a rounded falloff (corner %d vs axis %d)", (int)px_cov(PX(11, 18)),
          (int)px_cov(PX(12, 20)));
    /* The body is 5 px tall: rows 18..22 are touched, 17 and 23 are not. */
    CHECK(PX(36, 17) == C_OUT, "the body of the stroke is taller than 5 px (row 17)");
    CHECK(PX(36, 23) == C_OUT, "the body of the stroke is taller than 5 px (row 23)");
    CHECK(PX(36, 18) == C_INK, "row 18 of a 5 px stroke is not fully covered (%04X)",
          (unsigned)PX(36, 18));
    CHECK(PX(36, 22) == C_INK, "row 22 of a 5 px stroke is not fully covered (%04X)",
          (unsigned)PX(36, 22));
    CHECK(px_cov(PX(9, 20)) == 0, "the cap overshoots the pen radius (x=9)");

    stroke_spec(12, 20, 60, 20, 5, "horizontal 5px");
    stroke_spec(60, 20, 12, 20, 5, "horizontal 5px (reversed)");
    stroke_spec(30, 5, 30, 70, 5, "vertical 5px");
    stroke_spec(10, 10, 90, 55, 5, "diagonal 5px");
    stroke_spec(90, 55, 10, 10, 5, "diagonal 5px (reversed)");
    stroke_spec(10, 60, 90, 20, 3, "diagonal 3px");
    stroke_spec(10, 60, 90, 20, 9, "diagonal 9px");
    stroke_spec(10, 10, 200, 160, 4, "long shallow 4px");
    stroke_spec(40, 40, 41, 40, 5, "two pixel long 5px (caps only)");
    stroke_spec(40, 40, 40, 40, 5, "degenerate point 5px");
    stroke_spec(5, 100, 300, 101, 3, "wide and shallow 3px");

    /* thickness <= 1 is the plain Bresenham line, pixel for pixel. */
    {
        cgl_color_t a[FB_W * FB_H];
        int32_t i;
        bool same = true;

        scene_begin();
        cgl_canvas_line(&s_canvas, 10, 30, 120, 90, C_INK);
        for (i = 0; i < (int32_t)(FB_W * FB_H); i++)
        {
            a[i] = s_fb[i];
        }
        scene_begin();
        cgl_canvas_line_thick(&s_canvas, 10, 30, 120, 90, 1, C_INK);
        for (i = 0; i < (int32_t)(FB_W * FB_H); i++)
        {
            if (a[i] != s_fb[i])
            {
                same = false;
                break;
            }
        }
        CHECK(same, "line_thick(t=1) differs from cgl_canvas_line()");
    }

    /* An even width is drawn at its true width (antialiasing removed the odd-only
       restriction of the first implementation). */
    scene_begin();
    cgl_canvas_line_thick(&s_canvas, 12, 20, 60, 20, 4, C_INK);
    CHECK(PX(36, 20) == C_INK, "a 4 px stroke is not solid on its axis (%04X)",
          (unsigned)PX(36, 20));
    CHECK((px_cov(PX(36, 18)) > 100) && (px_cov(PX(36, 18)) < 156),
          "a 4 px stroke's top edge is not half covered (cov %d)", (int)px_cov(PX(36, 18)));
    CHECK((px_cov(PX(36, 22)) > 100) && (px_cov(PX(36, 22)) < 156),
          "a 4 px stroke's bottom edge is not half covered (cov %d)", (int)px_cov(PX(36, 22)));
    CHECK(px_cov(PX(36, 17)) == 0, "a 4 px stroke is more than 4 px tall (row 17 cov %d)",
          (int)px_cov(PX(36, 17)));
}

/* -------------------------------------------------------------------------- */
/* 7..9  cgl_canvas_arc                                                       */
/* -------------------------------------------------------------------------- */

static void test_arc_geometry(void)
{
    const int16_t cx = 160;
    const int16_t cy = 86;
    const int16_t r = 74;
    const int16_t thickness = 5;
    const double pen = (double)thickness / 2.0;
    int16_t x;
    int16_t y;
    uint32_t touched = 0u;
    uint32_t partial = 0u;
    double worst = 0.0;

    scene_begin();
    cgl_canvas_arc(&s_canvas, cx, cy, r, thickness, 0u, CGL_ANGLE_TURN, C_INK);

    for (y = 0; y < (int16_t)FB_H; y++)
    {
        for (x = 0; x < (int16_t)FB_W; x++)
        {
            cgl_color_t c = PX(x, y);
            double d;

            if (c == C_OUT)
            {
                continue;
            }
            touched++;
            d = hypot((double)(x - cx), (double)(y - cy));
            if (fabs(d - (double)r) > worst)
            {
                worst = fabs(d - (double)r);
            }
            if ((c != C_INK) && (c != C_OUT))
            {
                partial++;
            }
        }
    }
    CHECK(worst <= (pen + 0.6), "ring pixel is %.2f px off the radius (pen %.1f)", worst, pen);
    /* A 5 px ring of radius 74 has an area of about 2*pi*74*5 = 2324 px. */
    CHECK(touched >= 2100u, "full ring touched only %u pixels", (unsigned)touched);
    /* It is antialiased: the two edges of the ring are ramps, so a substantial part
       of the touched pixels is partial (a crisp ring has only full pixels). */
    CHECK(partial >= 300u, "the ring has only %u partial pixels: it is not antialiased",
          (unsigned)partial);

    /* The centre line of the stroke is exactly the colour: the coverage ramp must
       not eat into the middle of a 5 px stroke. */
    {
        uint32_t i;

        for (i = 0u; i < 2048u; i++)
        {
            cgl_angle_t a = (cgl_angle_t)(i * 32u);
            int16_t ox;
            int16_t oy;

            cgl_math_polar(a, r, &ox, &oy);
            if (PX((int16_t)(cx + ox), (int16_t)(cy + oy)) != C_INK)
            {
                CHECK(false, "the ring's centre line is not solid at %u degrees (%04X)",
                      (unsigned)cgl_angle_deg(a),
                      (unsigned)PX((int16_t)(cx + ox), (int16_t)(cy + oy)));
                break;
            }
        }
    }

    /* No seam and no gap: every direction has a pixel within a pen radius. */
    {
        uint32_t i;

        for (i = 0u; i < 4096u; i++)
        {
            cgl_angle_t a = (cgl_angle_t)(i * 16u);
            int16_t ox;
            int16_t oy;
            int16_t px;
            int16_t py;
            bool hit = false;
            int16_t ddx;
            int16_t ddy;

            cgl_math_polar(a, r, &ox, &oy);
            px = (int16_t)(cx + ox);
            py = (int16_t)(cy + oy);
            for (ddy = -pen; (ddy <= pen) && !hit; ddy++)
            {
                for (ddx = -pen; ddx <= pen; ddx++)
                {
                    int16_t sx = (int16_t)(px + ddx);
                    int16_t sy = (int16_t)(py + ddy);

                    if ((sx >= 0) && (sx < (int16_t)FB_W) && (sy >= 0) && (sy < (int16_t)FB_H) &&
                        (PX(sx, sy) != C_OUT))
                    {
                        hit = true;
                        break;
                    }
                }
            }
            if (!hit)
            {
                CHECK(false, "the ring has a gap at %u degrees", (unsigned)cgl_angle_deg(a));
                break;
            }
        }
    }

    /* Every row the ring crosses is crossed twice: a row with a single pixel means
       one arm lost its pixels. */
    {
        int16_t row;

        for (row = (int16_t)(cy - r + pen + 1); row <= (int16_t)(cy + r - pen - 1); row++)
        {
            uint32_t n = 0u;

            for (x = 0; x < (int16_t)FB_W; x++)
            {
                if (PX(x, row) != C_OUT)
                {
                    n++;
                }
            }
            if (n < (uint32_t)(2 * thickness - 4))
            {
                CHECK(false, "row %d of the ring has only %u pixels", (int)row, (unsigned)n);
                break;
            }
        }
    }
}

/**
 * The ring must not shimmer: extending the sweep may only *add* pixels at the
 * leading end, never re-place the ones already drawn.
 *
 * This is the test for the first implementation's defect (discs stamped along the
 * path, with the step derived from the current sweep) -- it passed every geometric
 * assertion above and still visibly vibrated on the panel.
 */
static void test_arc_stability(void)
{
    const int16_t cx = 100;
    const int16_t cy = 86;
    const int16_t r = 60;
    const uint32_t sweep_a = 20000u;
    const uint32_t sweep_b = 20300u;
    int16_t x;
    int16_t y;
    uint32_t diff_far = 0u;
    uint32_t diff_near = 0u;

    scene_begin();
    cgl_canvas_arc(&s_canvas, cx, cy, r, 5, CGL_ANGLE_UP, sweep_a, C_INK);
    memcpy(s_other, s_fb, sizeof(s_fb));

    scene_begin();
    cgl_canvas_arc(&s_canvas, cx, cy, r, 5, CGL_ANGLE_UP, sweep_b, C_INK);

    for (y = 0; y < (int16_t)FB_H; y++)
    {
        for (x = 0; x < (int16_t)FB_W; x++)
        {
            int32_t dx = x - cx;
            int32_t dy = y - cy;
            double d = hypot((double)dx, (double)dy);

            if (fabs(d - (double)r) > 6.0)
            {
                continue; /* not on the ring: the caps of the two sweeps differ there by design */
            }
            {
                /* Angle of this pixel relative to 12 o'clock, increasing clockwise,
                   in the same units the arc uses, so that "near the leading end" can
                   be decided without guessing. */
                double ang = atan2((double)dx, (double)(-dy)); /* 0 at 12, clockwise */
                double lead = ((double)sweep_a * 2.0 * 3.14159265358979323846) / 65536.0;

                if (ang < 0.0)
                {
                    ang += 2.0 * 3.14159265358979323846;
                }
                if (ang > (lead - 0.09))
                {
                    diff_near++;
                    continue; /* within ~5 degrees of the leading end: expected to differ */
                }
                if (PX(x, y) != s_other[((int32_t)y * (int32_t)FB_W) + (int32_t)x])
                {
                    diff_far++;
                    if (diff_far == 1u)
                    {
                        printf("       first shimmer at (%d,%d)\n", (int)x, (int)y);
                    }
                }
            }
        }
    }
    CHECK(diff_far == 0u, "the ring shimmered: %u pixels changed away from the leading end",
          (unsigned)diff_far);
    CHECK(diff_near > 0u, "the stability test compared nothing near the leading end");
}

/**
 * The leading end of an arc must not run out past the ring and come back.
 *
 * Measured rather than reasoned about, because the failure is a *motion*: the stroke's
 * two ends are drawn as round caps at the analytic angle while the body is clipped to a
 * wedge, so a fraction of a pixel of disagreement between them makes the cap leave the
 * ring and return as the sweep grows. On the reference panel that is a head that pops
 * out and back, worst at the angles where neither dx nor dy is near zero -- which is
 * exactly where the two named cases below are, with the two axes for reference.
 *
 * The measurement: sweep the leading end through a full turn in fine steps and, at each
 * step, take the distance from the centre to the **outermost inked pixel centre**, in
 * Q6. The stroke's outer edge is a circle of radius `r + pen/2`, so that figure is a
 * constant; all that is legitimately left is the pixel grid, which can move it by half
 * a pixel, plus the one pixel coverage ramp, which adds half a pixel more. Anything
 * wider than that is the cap leaving the ring.
 *
 * WARNING: a full turn is deliberately **excluded** (`sweep < CGL_ANGLE_TURN`). A full
 * ring has no ends, so `cgl_canvas_arc()` draws no caps for it and the outermost ink
 * becomes the ring's own grid-quantised edge -- a different question, and one this test
 * would report as a failure.
 */
static void test_arc_cap_stays_on_the_ring(void)
{
    const int16_t cx = 100;
    const int16_t cy = 86;
    const int16_t r = 60;
    const int16_t thick = 3;
    const int32_t ideal_q6 = ((int32_t)r * 64) + ((int32_t)thick * 32);
    static const uint16_t starts_deg[5] = { 0u, 45u, 135u, 270u, 300u };
    uint8_t si;

    for (si = 0u; si < 5u; si++)
    {
        const cgl_angle_t start = CGL_ANGLE_DEG(starts_deg[si]);
        int32_t r_lo = 1 << 20;
        int32_t r_hi = 0;
        uint32_t sweep;
        uint32_t sweep_lo = 0u;
        uint32_t sweep_hi = 0u;

        /* WARNING: the sweep starts at a sixteenth of a turn, not at the smallest angle
           the API accepts. A 1.4 degree arc is a blob four pixels across, and with so few
           pixels on the outer edge the best one it can land on is up to a pixel and a half
           short of the ideal radius purely because the pixel grid is where it is -- noise
           that would swamp the measurement. From 22.5 degrees up, hundreds of pixels sample
           the edge and the figure is steady. */
        for (sweep = 4096u; sweep < (uint32_t)CGL_ANGLE_TURN; sweep += 256u)
        {
            int16_t x;
            int16_t y;
            int32_t over = 0; /* the outermost inked pixel of **this** sweep */

            scene_begin();
            cgl_canvas_arc(&s_canvas, cx, cy, r, thick, start, sweep, C_INK);

            for (y = 0; y < (int16_t)FB_H; y++)
            {
                for (x = 0; x < (int16_t)FB_W; x++)
                {
                    if (PX(x, y) != C_OUT)
                    {
                        /* Pixel **centres**: the rasteriser measures from the centre,
                           and comparing corner distances would fold the grid's own
                           half-pixel offset into the answer. */
                        int32_t dx_q6 = (((int32_t)x - cx) * 64) + 32;
                        int32_t dy_q6 = (((int32_t)y - cy) * 64) + 32;
                        int32_t d = (int32_t)cgl_math_isqrt(
                            (uint32_t)((dx_q6 * dx_q6) + (dy_q6 * dy_q6)));

                        if (d > over)
                        {
                            over = d;
                        }
                    }
                }
            }

            /* WARNING: the figure that has to be steady is "**how far out** this sweep
               reached", so the running min and max are taken over that, and never over
               the distance of individual pixels. Folding the latter in mixes the stroke's
               *inner* edge (r - pen/2, about 3667 Q6 here) into a quantity that is meant
               to be its outer one, and the difference then reads as five pixels of "cap
               leaves the ring" that is really the width of the pen. */
            if (over > 0)
            {
                if (over < r_lo)
                {
                    r_lo = over;
                    sweep_lo = sweep;
                }
                if (over > r_hi)
                {
                    r_hi = over;
                    sweep_hi = sweep;
                }
            }
        }

        /* The claim that matters, and the one the panel shows when it is broken: with the
           stroke's outer edge at `ideal_q6`, a pixel centre may legitimately carry ink up
           to half a pixel outside it (the grid) plus the width of the coverage ramp (half
           a pixel more), i.e. 64 Q6 -- 96 Q6 with rounding. Anything beyond that is a cap
           drawn **off the ring**, which is what "the head runs out of the arc" is.
           WARNING: 4034 Q6 (ideal + 98) is what the corner-vs-centre bug in
           `canvas__disc_aa()` measured here before it was fixed, at 300 and 270 degrees
           but not at 0 or 45 -- a reach that came and went with the angle. */
        CHECK(r_hi <= (ideal_q6 + 96),
              "start %u deg: the ink reaches %d Q6, %d Q6 past the stroke's own outer edge "
              "(worst at sweep %u) -- the cap is drawn off the ring",
              (unsigned)starts_deg[si], (int)r_hi, (int)(r_hi - ideal_q6), (unsigned)sweep_hi);

        /* ... and the symmetric one: the end must still **reach** the edge. A cap that
           vanished (or was placed inside the ring) would leave the arc short, which is the
           same defect seen from the other side. */
        CHECK(r_lo >= (ideal_q6 - 96),
              "start %u deg: the outermost ink only reaches %d Q6 against an outer edge of %d "
              "(shortest at sweep %u) -- the end is missing",
              (unsigned)starts_deg[si], (int)r_lo, (int)ideal_q6, (unsigned)sweep_lo);
    }
}

static void test_arc_partial(void)
{
    const int16_t cx = 100;
    const int16_t cy = 86;
    const int16_t r = 60;
    const double pen = 2.5;
    const cgl_angle_t start = CGL_ANGLE_UP;             /* 12 o'clock */
    const uint32_t sweep = (uint32_t)CGL_ANGLE_QUARTER;  /* a quarter turn, clockwise */
    int16_t x;
    int16_t y;
    uint32_t touched = 0u;

    scene_begin();
    cgl_canvas_arc(&s_canvas, cx, cy, r, 5, start, sweep, C_INK);

    for (y = 0; y < (int16_t)FB_H; y++)
    {
        for (x = 0; x < (int16_t)FB_W; x++)
        {
            cgl_color_t c = PX(x, y);

            if (c == C_OUT)
            {
                continue;
            }
            touched++;
            {
                double d = hypot((double)(x - cx), (double)(y - cy));
                bool on_ring = fabs(d - (double)r) <= (pen + 0.6);
                bool in_cap = (seg_dist((double)x, (double)y, (double)cx, (double)(cy - r),
                                        (double)cx, (double)(cy - r)) <= (pen + 0.6)) ||
                              (seg_dist((double)x, (double)y, (double)(cx + r), (double)cy,
                                        (double)(cx + r), (double)cy) <= (pen + 0.6));

                if (!on_ring && !in_cap)
                {
                    CHECK(false, "partial arc pixel (%d,%d) is neither on the ring nor in a cap",
                          (int)x, (int)y);
                }
                if ((y > (int16_t)((double)cy + pen + 1.0)) && !in_cap)
                {
                    CHECK(false, "partial arc drew below the centre: (%d,%d)", (int)x, (int)y);
                }
            }
        }
    }
    CHECK(touched >= 400u, "quarter arc touched only %u pixels", (unsigned)touched);

    /* A zero sweep draws nothing at all (the caller's "empty gauge" state). */
    scene_begin();
    cgl_canvas_arc(&s_canvas, cx, cy, r, 5, start, 0u, C_INK);
    {
        uint32_t n = 0u;

        for (y = 0; y < (int16_t)FB_H; y++)
        {
            for (x = 0; x < (int16_t)FB_W; x++)
            {
                if (PX(x, y) != C_OUT)
                {
                    n++;
                }
            }
        }
        CHECK(n == 0u, "sweep = 0 drew %u pixels", (unsigned)n);
    }

    /* A radius of 0 is the antialiased dot the header promises. */
    scene_begin();
    cgl_canvas_arc(&s_canvas, cx, cy, 0, 9, 0u, CGL_ANGLE_TURN, C_INK);
    CHECK(PX(cx, cy) == C_INK, "a zero radius arc did not draw its centre");
    CHECK(PX((int16_t)(cx + 4), cy) == C_INK, "a 9 px dot is thinner than asked (cov %d)",
          (int)px_cov(PX((int16_t)(cx + 4), cy)));
    CHECK(PX((int16_t)(cx + 5), cy) == C_OUT, "a 9 px dot reached 5 px out");
}

/* -------------------------------------------------------------------------- */
/* pan and clip                                                               */
/* -------------------------------------------------------------------------- */

/** Drawing with pan (dx,dy) must equal the un-panned drawing shifted by exactly that. */
static void test_pan_applied_once(void)
{
    cgl_color_t a[FB_W * FB_H];
    const int16_t dx = 7;
    const int16_t dy = 5;
    int16_t x;
    int16_t y;
    uint32_t missed = 0u;

    scene_begin();
    cgl_canvas_arc(&s_canvas, 90, 60, 40, 5, CGL_ANGLE_UP, 40000u, C_INK);
    cgl_canvas_circle(&s_canvas, 20, 30, 12, C_INK);
    cgl_canvas_line_thick(&s_canvas, 40, 100, 120, 130, 5, C_INK);
    for (y = 0; y < (int16_t)FB_H; y++)
    {
        for (x = 0; x < (int16_t)FB_W; x++)
        {
            a[((int32_t)y * (int32_t)FB_W) + x] = PX(x, y);
        }
    }

    scene_begin();
    {
        cgl_point_t pan;

        pan.x = dx;
        pan.y = dy;
        cgl_canvas_set_pan(&s_canvas, pan);
    }
    cgl_canvas_arc(&s_canvas, 90, 60, 40, 5, CGL_ANGLE_UP, 40000u, C_INK);
    cgl_canvas_circle(&s_canvas, 20, 30, 12, C_INK);
    cgl_canvas_line_thick(&s_canvas, 40, 100, 120, 130, 5, C_INK);

    for (y = 0; y < (int16_t)FB_H; y++)
    {
        for (x = 0; x < (int16_t)FB_W; x++)
        {
            /* The panned drawing is the un-panned one moved by (+dx, +dy), so the
               source of b[x][y] is a[x-dx][y-dy]. Only pixels whose source is really
               inside the buffer can be compared: content that *enters* the frame at
               the top-left has no counterpart in `a` at all. */
            int16_t sx = (int16_t)(x - dx);
            int16_t sy = (int16_t)(y - dy);

            if ((sx < 0) || (sy < 0))
            {
                continue;
            }
            if (s_fb[((int32_t)y * (int32_t)FB_W) + x] != a[((int32_t)sy * (int32_t)FB_W) + sx])
            {
                missed++;
            }
        }
    }
    CHECK(missed == 0u, "pan is not a pure translation: %u pixels differ", (unsigned)missed);
}

/** Clipping: nothing outside the clip window, and the clip is not the frame. */
static void test_clip(void)
{
    int16_t x;
    int16_t y;
    cgl_rect_t clip = cgl_rect_wh(10, 10, 80, 70);

    scene_begin();
    cgl_canvas_set_clip(&s_canvas, clip);
    cgl_canvas_arc(&s_canvas, 50, 45, 30, 5, 0u, CGL_ANGLE_TURN, C_INK);
    cgl_canvas_circle(&s_canvas, 50, 45, 40, C_INK);
    cgl_canvas_line_thick(&s_canvas, 0, 12, 319, 12, 5, C_INK);

    for (y = 0; y < (int16_t)FB_H; y++)
    {
        for (x = 0; x < (int16_t)FB_W; x++)
        {
            if ((PX(x, y) != C_OUT) && !cgl_rect_contains_point(clip, x, y))
            {
                CHECK(false, "clip leak at (%d,%d)", (int)x, (int)y);
            }
        }
    }
    CHECK(PX(50, 45) == C_OUT, "an outline/ring must not fill its centre");
    CHECK(PX(50, 15) == C_INK, "the clipped ring lost its top");
}

/**
 * The shape set both band-consistency checks draw: a full ring, a partial ring
 * (which also puts down the two round caps, i.e. `canvas__disc_aa()`), a circle
 * outline and two thick lines at very different angles.
 */
static void band_scene(void)
{
    cgl_canvas_arc(&s_canvas, 90, 90, 60, 5, 0u, CGL_ANGLE_TURN, C_INK);
    cgl_canvas_arc(&s_canvas, 150, 60, 40, 4, CGL_ANGLE_UP, 30000u, C_INK);
    cgl_canvas_circle(&s_canvas, 60, 60, 30, C_INK);
    cgl_canvas_line_thick(&s_canvas, 20, 20, 300, 150, 7, C_INK);
    cgl_canvas_line_thick(&s_canvas, 250, 20, 60, 160, 5, C_INK);
}

/**
 * One 4-row band of the scene, exactly as the presenter renders it: the canvas
 * carries **that band's buffer** (`origin_y = y` is the screen row of buffer row
 * 0, so the buffer must start at the band's first row -- handing it the whole
 * framebuffer instead puts every band into the top four rows and the comparison
 * below then "fails" for a reason that has nothing to do with the clamp).
 */
static void band_render(int16_t y, cgl_point_t pan, cgl_rect_t src_clip, const cgl_warp_t *w)
{
    int16_t h = (int16_t)(((y + 4) <= (int16_t)FB_H) ? 4 : ((int16_t)FB_H - y));

    cgl_canvas_init(&s_canvas, &s_fb[(int32_t)y * (int32_t)FB_W], (int16_t)FB_W, h, y);
    cgl_canvas_set_clip(&s_canvas, cgl_rect(0, y, (int16_t)(FB_W - 1), (int16_t)(y + h - 1)));
    cgl_canvas_set_pan(&s_canvas, pan);
    if (w != NULL)
    {
        cgl_canvas_set_src_clip(&s_canvas, src_clip);
        cgl_canvas_set_warp(&s_canvas, w);
    }
    band_scene();
}

/**
 * **A shape drawn band by band must equal the same shape drawn in one pass.**
 *
 * This is the test for the band clamp every scanline loop now carries: the presenter
 * renders a widget once per 4-row band, so a primitive that walks its whole geometry
 * in every band pays for one picture 43 times. The clamp fixes that, and a clamp that
 * is off by one row -- or that forgets that `clip` is in *screen* space while the
 * geometry is in *primitive* space (which happens the moment a widget is translated,
 * i.e. during every page transition) -- deletes rows from the middle of a stroke. On
 * the panel that looks like a clock whose hands are broken into fragments; here it is
 * a byte comparison.
 *
 * Pan is deliberately non-zero in the second half of the check, because that is the
 * case the first version of the clamp got wrong.
 */
static void test_band_consistency(void)
{
    static const int16_t pans[2][2] = { { 0, 0 }, { 5, -3 } };
    uint8_t pi;

    for (pi = 0u; pi < 2u; pi++)
    {
        cgl_color_t one[FB_W * FB_H];
        int16_t y;
        int32_t i;
        int32_t bad = 0;
        int32_t ink = 0;

        /* One pass, over the whole screen. */
        scene_begin();
        cgl_canvas_set_pan(&s_canvas, (cgl_point_t){ pans[pi][0], pans[pi][1] });
        band_scene();
        memcpy(one, s_fb, sizeof(one));

        /* The same, one 4-row band at a time, exactly as the presenter does it. */
        scene_begin();
        for (y = 0; y < (int16_t)FB_H; y = (int16_t)(y + 4))
        {
            band_render(y, (cgl_point_t){ pans[pi][0], pans[pi][1] }, cgl_rect_none(), NULL);
        }

        /* WARNING: the index must be **wider than int16_t**: FB_W * FB_H is 55 040, so
           `for (y = 0; y < (int16_t)(FB_W * FB_H); y++)` wraps negative and the body
           never runs -- the comparison passes and proves nothing. (That, plus the band
           buffer above, is what this check used to do.) */
        for (i = 0; i < (int32_t)FB_W * (int32_t)FB_H; i++)
        {
            if (one[i] != s_fb[i])
            {
                if (bad == 0)
                {
                    printf("       first difference at (%d,%d): one pass %04X, bands %04X\n",
                           (int)(i % FB_W), (int)(i / FB_W), (unsigned)one[i], (unsigned)s_fb[i]);
                }
                bad++;
            }
            if (one[i] != C_OUT)
            {
                ink++;
            }
        }
        CHECK(bad == 0, "pan (%d,%d): %d pixels differ between one pass and band by band",
              (int)pans[pi][0], (int)pans[pi][1], (int)bad);
        CHECK(ink > 500, "pan (%d,%d): only %d pixels drawn, the comparison is vacuous",
              (int)pans[pi][0], (int)pans[pi][1], (int)ink);
    }
}

/**
 * The same byte comparison **with a warp installed** -- the case that makes the band
 * clamp hard, and the reason `cgl_canvas_src_window()` exists.
 *
 * A suction transition compresses the subtree towards the suction point, so the source
 * row that lands in a given 4-row band is *not* in that band: a rasteriser that bound
 * its walk to the band itself would delete most of the picture (the page would be
 * sucked away in fragments), while one that bound nothing at all re-walks every shape
 * in all 43 bands *and* maps each pixel of those wasted rows through the deformation
 * (measured on the watch face: 18.7 M host cycles for one frame against 1.8 M for the
 * same page flat -- four frames for a whole transition). The bound every scanline loop
 * uses is therefore the band's **pre-image**, and this is what proves that pre-image is
 * complete: rendered band by band, the warped shapes must still be byte for byte what a
 * single whole-screen pass draws. A window one row too narrow shows up here as missing
 * pixels; a window that is merely too wide costs time and nothing else.
 *
 * The S-shaped variant (`s_amp_q8 != 0`) is included because the lateral trajectory is
 * the part of the deformation that moves pixels *out* of the band horizontally, which
 * is what the source clip window is there to bound.
 */
static void test_band_consistency_warped(void)
{
    static const struct
    {
        uint16_t p;
        uint16_t amp;
    } cases[] = {
        { 1u, 0u },   { 32u, 0u },  { 96u, 0u },   { 160u, 0u },
        { 224u, 0u }, { 256u, 0u }, { 64u, 160u }, { 128u, 160u }, { 200u, 160u }
    };
    const uint8_t n = (uint8_t)(sizeof(cases) / sizeof(cases[0]));
    cgl_rect_t src = cgl_rect(0, 0, (int16_t)(FB_W - 1), (int16_t)(FB_H - 1));
    uint8_t ci;

    for (ci = 0u; ci < n; ci++)
    {
        cgl_color_t one[FB_W * FB_H];
        cgl_warp_t w;
        int16_t y;
        int32_t i;
        int32_t bad = 0;
        int32_t ink = 0;

        memset(&w, 0, sizeof(w));
        w.src_y0 = 0;
        w.src_h = (int16_t)FB_H;
        w.cx = (int16_t)(FB_W / 2);
        w.tx = (int16_t)(FB_W / 2);
        w.ty = (int16_t)(FB_H + 12); /* the suction point: below the panel, like S_OUT */
        w.progress_q8 = cases[ci].p;
        w.narrow_q8 = (uint16_t)CGL_CFG_GENIE_NARROW_Q8;
        w.s_amp_q8 = cases[ci].amp;
        /* The same "one row per source row, rounded" step the navigator derives
           (nav__warp_setup): a test that wants the real deformation must not invent a
           different one. */
        w.row_step_q8 =
            (uint16_t)((((uint32_t)256u * 256u) + ((uint32_t)FB_H / 2u)) / (uint32_t)FB_H);

        /* --- one pass, whole screen --- */
        scene_begin();
        cgl_canvas_set_src_clip(&s_canvas, src);
        cgl_canvas_set_warp(&s_canvas, &w);
        band_scene();
        memcpy(one, s_fb, sizeof(one));

        /* --- the same, one 4-row band at a time (clip = band, src_clip = screen) --- */
        scene_begin();
        for (y = 0; y < (int16_t)FB_H; y = (int16_t)(y + 4))
        {
            band_render(y, (cgl_point_t){ 0, 0 }, src, &w);
        }

        for (i = 0; i < (int32_t)FB_W * (int32_t)FB_H; i++)
        {
            if (one[i] != s_fb[i])
            {
                if (bad == 0)
                {
                    printf("       first difference at (%d,%d): one pass %04X, bands %04X\n",
                           (int)(i % FB_W), (int)(i / FB_W), (unsigned)one[i], (unsigned)s_fb[i]);
                }
                bad++;
            }
            if (one[i] != C_OUT)
            {
                ink++;
            }
        }
        CHECK(bad == 0, "warp p=%u amp=%u: %d pixels differ between one pass and band by band",
              (unsigned)cases[ci].p, (unsigned)cases[ci].amp, (int)bad);
        /* Without this the check above would pass on an all-sentinel frame: a source
           window that is empty (or a warp that moved the whole scene off screen) would
           "agree" with itself and prove nothing. At p = 256 the collapse is complete --
           every source row lands on the suction point, which is below the panel -- so
           nothing *should* be drawn, and that is the assertion there. */
        if (cases[ci].p >= 256u)
        {
            CHECK(ink == 0, "warp p=%u: the page has fully collapsed onto an off-screen "
                            "suction point, yet %d pixels were drawn",
                  (unsigned)cases[ci].p, (int)ink);
        }
        else
        {
            CHECK(ink > 0, "warp p=%u amp=%u: only %d pixels drawn, the comparison is vacuous",
                  (unsigned)cases[ci].p, (unsigned)cases[ci].amp, (int)ink);
        }
    }
}

/**
 * Transparency: an arc, a ring, a circle and a disc must be able to be drawn at less
 * than full opacity, and drawing one must not leave the canvas translucent for
 * whatever comes next.
 *
 * The three claims, in the order they matter:
 *   1. **alpha 256 is the plain primitive**, to the pixel. If a wrapper changed the
 *      shape at all -- a thinner ring because it scaled the geometry instead of the
 *      coverage -- this is where it shows.
 *   2. **alpha 128 is exactly half way** between the background and the ink: the
 *      framework's one blend formula applied to the coverage the rasteriser already
 *      computed, so "40 % ink" reads as `dst + (ink - dst) * 0.4` and not as something
 *      a second, slightly different rasteriser decided.
 *   3. **alpha 0 draws nothing at all**, and the canvas is back to opaque afterwards.
 *      Ink alpha is *state*: a wrapper that forgot to restore it would leave the rest
 *      of the page faint, which is the failure mode this whole API exists to avoid.
 */
static void test_arc_alpha(void)
{
    const int16_t cx = 100;
    const int16_t cy = 86;
    const int16_t r = 60;
    cgl_color_t plain[FB_W * FB_H];
    int16_t x;
    int16_t y;
    int32_t inked = 0;
    int32_t half_ok = 0;
    int32_t soft = 0;
    int32_t wrong = 0;

    CHECK(cgl_canvas_get_ink_alpha(&s_canvas) == CGL_CANVAS_INK_OPAQUE,
          "a fresh canvas is not opaque (alpha %u)", (unsigned)cgl_canvas_get_ink_alpha(&s_canvas));

    /* --- 1. the plain call, remembered ------------------------------------ */
    scene_begin();
    cgl_canvas_arc(&s_canvas, cx, cy, r, 3, CGL_ANGLE_UP, 20000u, C_INK);
    memcpy(plain, s_fb, sizeof(plain));

    /* --- 2. the same shape through the wrapper at full opacity ------------ */
    scene_begin();
    cgl_arc_draw_arc(&s_canvas, cx, cy, r, 3, CGL_ANGLE_UP, 20000u, C_INK, 256u);
    for (y = 0; y < (int16_t)FB_H; y++)
    {
        for (x = 0; x < (int16_t)FB_W; x++)
        {
            CHECK(PX(x, y) == plain[((int32_t)y * FB_W) + x],
                  "alpha 256 is not the plain primitive: (%d,%d) is %04X against %04X", (int)x,
                  (int)y, (unsigned)PX(x, y), (unsigned)plain[((int32_t)y * FB_W) + x]);
            if (PX(x, y) != C_OUT)
            {
                inked++;
            }
        }
    }
    CHECK(inked > 0, "the alpha-256 arc inked nothing, so the comparison above is vacuous");

    /* --- 3. half opacity -------------------------------------------------- */
    /* The **ring** (a full turn), not the 305 degree arc above: it has to be compared
       against the same shape at full opacity, or every pixel the one covers and the
       other does not reads as "the half-alpha pass inked something new". */
    scene_begin();
    cgl_arc_draw_ring(&s_canvas, cx, cy, r, 3, C_INK, 256u);
    memcpy(plain, s_fb, sizeof(plain));

    scene_begin();
    cgl_arc_draw_ring(&s_canvas, cx, cy, r, 3, C_INK, 128u);
    for (y = 0; y < (int16_t)FB_H; y++)
    {
        for (x = 0; x < (int16_t)FB_W; x++)
        {
            cgl_color_t got = PX(x, y);
            cgl_color_t full = plain[((int32_t)y * FB_W) + x];

            if (full == C_OUT)
            {
                CHECK(got == C_OUT, "the half-alpha ring inked (%d,%d), which the full one did not",
                      (int)x, (int)y);
                continue;
            }
            if (full == C_INK)
            {
                /* An interior pixel: full ink before, so "half" has exactly one
                   meaning and it is the framework's own blend at 128. This is the
                   claim; the edge pixels below are checked for softness instead, since
                   halving an already-truncated coverage is not bit-reproducible from
                   the *readback* of it.

                   WARNING: one level of tolerance per channel, and it is not slack. The
                   alpha scales the **coverage** (`(cov * 128) >> 8`), so a pixel whose
                   full coverage was 255 rather than 256 lands on 127 rather than 128 and
                   blends one level differently -- and one level of a 5 bit channel is 3 %
                   of it. Testing for bit equality here would be testing that no pixel on
                   the stroke has a coverage of 255. */
                cgl_color_t want = cgl_color_blend_q8(C_OUT, C_INK, 128u);

                if (color_near(got, want))
                {
                    half_ok++;
                }
                else
                {
                    if (wrong == 0)
                    {
                        printf("       (%d,%d): half %04X, expected %04X\n", (int)x, (int)y,
                               (unsigned)got, (unsigned)want);
                    }
                    wrong++;
                }
            }
            else
            {
                /* An edge pixel of the stroke. What has to hold is that half the ink
                   never makes it **brighter** than all of it, and that some edge pixels
                   are still both visible and dimmer -- which is the statement that the
                   edge is still antialiased and the alpha scaled it rather than
                   truncating it. A pixel whose coverage rounds to nothing at half alpha
                   is the correct answer for a very faint edge, not a failure, so it is
                   not asserted to be visible. */
                if (px_cov(got) <= px_cov(full))
                {
                    half_ok++;
                }
                else
                {
                    if (wrong == 0)
                    {
                        printf("       (%d,%d): half is brighter than full (%04X against %04X)\n",
                               (int)x, (int)y, (unsigned)got, (unsigned)full);
                    }
                    wrong++;
                }
                if ((px_cov(got) > 0) && (px_cov(got) < px_cov(full)))
                {
                    soft++;
                }
            }
        }
    }
    CHECK(half_ok > 0, "the half-alpha ring inked nothing");
    CHECK(soft > 0, "no edge pixel is both visible and dimmer at half alpha: the ring is not "
          "antialiased, the alpha quantised it");
    CHECK(wrong == 0, "%d of %d inked pixels are not a half-opacity version of the full ring",
          (int)wrong, (int)(wrong + half_ok));

    /* --- 4. alpha 0, and the canvas put back ------------------------------ */
    scene_begin();
    cgl_arc_draw_arc(&s_canvas, cx, cy, r, 3, CGL_ANGLE_UP, 20000u, C_INK, 0u);
    for (y = 0; y < (int16_t)FB_H; y++)
    {
        for (x = 0; x < (int16_t)FB_W; x++)
        {
            if (PX(x, y) != C_OUT)
            {
                CHECK(false, "alpha 0 inked (%d,%d) = %04X", (int)x, (int)y, (unsigned)PX(x, y));
                y = (int16_t)FB_H; /* one report is enough */
                break;
            }
        }
    }
    CHECK(cgl_canvas_get_ink_alpha(&s_canvas) == CGL_CANVAS_INK_OPAQUE,
          "the wrapper left the canvas at alpha %u", (unsigned)cgl_canvas_get_ink_alpha(&s_canvas));

    /* --- 5. a caller's own alpha is restored, not overwritten ------------- */
    cgl_canvas_set_ink_alpha(&s_canvas, 99u);
    cgl_arc_draw_disc(&s_canvas, 40, 40, 10, C_INK, 200u);
    CHECK(cgl_canvas_get_ink_alpha(&s_canvas) == 99u,
          "the wrapper clobbered a caller's ink alpha (now %u)",
          (unsigned)cgl_canvas_get_ink_alpha(&s_canvas));
    CHECK(cgl_canvas_get_ink_alpha(NULL) == CGL_CANVAS_INK_OPAQUE, "get_ink_alpha(NULL) is not opaque");
    cgl_canvas_set_ink_alpha(&s_canvas, 4000u);
    CHECK(cgl_canvas_get_ink_alpha(&s_canvas) == CGL_CANVAS_INK_OPAQUE,
          "an over-opaque alpha (%u) was not clamped", (unsigned)cgl_canvas_get_ink_alpha(&s_canvas));
}

/** `cgl_canvas_pixel_blend_q8` is a real primitive: pan, clip, coverage. */
static void test_pixel_blend(void)
{
    scene_begin();
    cgl_canvas_pixel_blend_q8(&s_canvas, 20, 20, C_INK, 0u);
    cgl_canvas_pixel_blend_q8(&s_canvas, 21, 20, C_INK, 128u);
    cgl_canvas_pixel_blend_q8(&s_canvas, 22, 20, C_INK, 256u);

    CHECK(PX(20, 20) == C_OUT, "coverage 0 changed the pixel");
    CHECK((PX(21, 20) != C_OUT) && (PX(21, 20) != C_INK), "coverage 128 was not a blend (%04X)",
          (unsigned)PX(21, 20));
    CHECK(PX(22, 20) == C_INK, "coverage 256 did not write the colour");
    CHECK((px_cov(PX(21, 20)) > 100) && (px_cov(PX(21, 20)) < 156),
          "coverage 128 produced %d/256", (int)px_cov(PX(21, 20)));
}

/* -------------------------------------------------------------------------- */
/* gallery                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * A miniature dial built out of nothing but these primitives -- the "does it
 * actually look like something" check that the assertions above cannot make, and
 * the reference for how a radial widget is written: polar() for the endpoints,
 * line_thick() for the ticks and hands, arc() for the rings and the hub.
 */
static void gallery_dial(void)
{
    const int16_t cx = 86;
    const int16_t cy = 86;
    const int16_t R = 78;
    cgl_color_t bezel = CGL_RGB565(70, 78, 92);
    cgl_color_t face = CGL_RGB565(14, 16, 22);
    cgl_color_t dim = CGL_RGB565(78, 86, 102);
    cgl_color_t bright = CGL_RGB565(235, 240, 246);
    cgl_color_t accent = CGL_RGB565(255, 106, 0);
    int16_t i;

    cgl_canvas_reset_clip(&s_canvas);
    cgl_canvas_clear(&s_canvas, CGL_COLOR_BLACK);
    cgl_canvas_arc(&s_canvas, cx, cy, 0, (int16_t)(2 * R), 0u, CGL_ANGLE_TURN, face);
    cgl_canvas_arc(&s_canvas, cx, cy, R, 2, 0u, CGL_ANGLE_TURN, bezel);
    cgl_canvas_arc(&s_canvas, cx, cy, (int16_t)(R - 6), 5, 0u, CGL_ANGLE_TURN, CGL_RGB565(26, 30, 38));

    for (i = 0; i < 60; i++)
    {
        cgl_angle_t a = (cgl_angle_t)((((uint32_t)i * CGL_ANGLE_TURN) + 30u) / 60u);
        bool hour = ((i % 5) == 0);
        int16_t ox;
        int16_t oy;
        int16_t ix;
        int16_t iy;

        cgl_math_polar(a, hour ? (int16_t)66 : (int16_t)70, &ox, &oy);
        cgl_math_polar(a, (int16_t)72, &ix, &iy);
        cgl_canvas_line_thick(&s_canvas, (int16_t)(cx + ox), (int16_t)(cy + oy),
                             (int16_t)(cx + ix), (int16_t)(cy + iy), hour ? 3 : 2,
                             hour ? bright : dim);
    }

    /* The seconds ring: a dim track with a bright progress arc and a cap dot. */
    cgl_canvas_arc(&s_canvas, cx, cy, (int16_t)(R - 14), 5, 0u, CGL_ANGLE_TURN, CGL_RGB565(40, 44, 56));
    cgl_canvas_arc(&s_canvas, cx, cy, (int16_t)(R - 14), 5, CGL_ANGLE_UP, 40000u, accent);

    /* Hands: hour 42 px at 10, minute 62 px at 9, second 70 px sweeping. */
    for (i = 0; i < 3; i++)
    {
        cgl_angle_t a = (i == 0) ? (cgl_angle_t)(CGL_ANGLE_UP + (CGL_ANGLE_TURN * 2u) / 12u)
                                 : ((i == 1) ? (cgl_angle_t)(CGL_ANGLE_UP + (CGL_ANGLE_TURN * 9u) / 60u)
                                             : (cgl_angle_t)(CGL_ANGLE_UP + (CGL_ANGLE_TURN * 37u) / 60u));
        int16_t len = (i == 0) ? 42 : ((i == 1) ? 62 : 70);
        int16_t ox;
        int16_t oy;
        int16_t bx;
        int16_t by;

        cgl_math_polar(a, len, &ox, &oy);
        cgl_math_polar((cgl_angle_t)(a + CGL_ANGLE_HALF), (int16_t)(i == 2 ? 14 : 0), &bx, &by);
        cgl_canvas_line_thick(&s_canvas, (int16_t)(cx + bx), (int16_t)(cy + by),
                             (int16_t)(cx + ox), (int16_t)(cy + oy),
                             (i == 0) ? 9 : ((i == 1) ? 7 : 3),
                             (i == 2) ? accent : bright);
    }
    /* The hub: an antialiased dot is a zero radius arc of the wanted diameter. */
    cgl_canvas_arc(&s_canvas, cx, cy, 0, 11, 0u, CGL_ANGLE_TURN, bright);
    cgl_canvas_arc(&s_canvas, cx, cy, 0, 5, 0u, CGL_ANGLE_TURN, accent);
}

/* -------------------------------------------------------------------------- */

int main(void)
{
    printf("cgl_math + radial primitives: trig, isqrt, circle, thick line, arc, AA\n\n");

    printf("1-2 trig / isqrt\n");
    test_math_accuracy();
    test_math_isqrt();
    test_math_polar_and_deg();

    printf("3-4 circle outline (antialiased)\n");
    test_circle();

    printf("5-6 thick line (antialiased, round caps)\n");
    test_line_thick();

    printf("7-9 arc (antialiased, stable, sector)\n");
    test_arc_geometry();
    test_arc_stability();
    test_arc_cap_stays_on_the_ring();
    test_arc_partial();

    printf("   pan / clip / band consistency / blend\n");
    test_pan_applied_once();
    test_clip();
    test_band_consistency();
    test_band_consistency_warped();
    test_arc_alpha();
    test_pixel_blend();

    printf("   gallery\n");
    gallery_dial();
    img_dump_ppm("build/arc_test.ppm", 0, s_fb, (int)FB_W, (int)FB_H);
    img_dump_bmp("build/arc_test.bmp", 0, s_fb, (int)FB_W, (int)FB_H);
    printf("\nwrote build/arc_test.ppm / build/arc_test.bmp (a miniature dial)\n");

    if (s_fails == 0)
    {
        printf("all passed.\n");
        return 0;
    }
    printf("%d failed.\n", s_fails);
    return 1;
}
