// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_math.c
  * @brief   LCD UI framework - integer trigonometry.
  *
  *  The table, and why it is a quadrant and not a circle
  *  ----------------------------------------------------
  *  65 entries cover 0..90 degrees in steps of `65536 / 256 = 256` angle units
  *  (1.40625 degrees), which is exactly the resolution an 8 bit module angle has --
  *  i.e. a `CGL_ANGLE_FROM_U8()` angle lands **on** a table entry and needs no
  *  interpolation at all. A full circle at the same step would be 257 entries for
  *  three quarters of redundant memory; the mirroring below costs three comparisons
  *  instead.
  *
  *  Interpolation: index Q14, value Q8
  *  ----------------------------------
  *      num = q * 64        where q is the position in the quadrant, 0..16384
  *      i   = num >> 14     table index, 0..64
  *      f   = num & 16383   fraction between entry i and i+1, Q14
  *
  *  `(v1 - v0) * f` is at most 256 * 16383 = 4.19e6 and fits int32 with room to
  *  spare, so there is no 64 bit multiply and therefore no `__aeabi_lmul` in the
  *  firmware (see the note on 64 bit arithmetic in cgl_anim.h).
  *
  *  WARNING: the interpolation result is rounded to nearest (the `+ 8192` before
  *  the shift), not truncated. Truncating would add up to one further LSB of error
  *  on top of the table's own half-LSB, which would make the accuracy claim in
  *  cgl_math.h ("within 1 LSB") false. `tests/arc_host_test.c` compares all 65536
  *  angles against libm and asserts that bound.
  *
  *  WARNING: inside the first quadrant the sine is monotonically increasing, so
  *  "v1 >= v0" always holds and the rounding term cannot be fooled by a negative
  *  difference. The mirroring in cgl_sin_q8() negates the *result* rather than
  *  walking the table backwards, which is what keeps that true in all four
  *  quadrants. Anyone extending this file with a decreasing table (a cosine
  *  quadrant, a tangent) has to think about which way the shift rounds.
  ******************************************************************************
  */
#include <cytgl/cgl_math.h>

/** round(256 * sin(i * pi / 128)), i = 0..64 (one quadrant, inclusive of 90 deg). */
static const int16_t s_sin_q8[65] = {
       0,    6,   13,   19,   25,   31,   38,   44,
      50,   56,   62,   68,   74,   80,   86,   92,
      98,  104,  109,  115,  121,  126,  132,  137,
     142,  147,  152,  157,  162,  167,  172,  177,
     181,  185,  190,  194,  198,  202,  206,  209,
     213,  216,  220,  223,  226,  229,  231,  234,
     237,  239,  241,  243,  245,  247,  248,  250,
     251,  252,  253,  254,  255,  255,  256,  256,
     256,
};

/** Sine of a position inside the first quadrant, Q8. `q` is 0..16384 for 0..90 deg. */
static int16_t sin__quadrant(uint16_t q)
{
    uint32_t num = (uint32_t)q * 64u; /* 0..1048576 */
    uint32_t i = (num >> 14);         /* 0..64 */
    int32_t v0;
    int32_t v1;
    int32_t f;

    if (i >= 64u)
    {
        return s_sin_q8[64];
    }
    f = (int32_t)(num & 0x3FFFu);
    v0 = (int32_t)s_sin_q8[i];
    v1 = (int32_t)s_sin_q8[i + 1u];
    /* Round to nearest: 8192 is half of the Q14 fraction. */
    return (int16_t)(v0 + ((((v1 - v0) * f) + 8192) >> 14));
}

int16_t cgl_sin_q8(cgl_angle_t a)
{
    uint16_t q = (uint16_t)(a & (uint16_t)0x3FFFu);

    switch ((uint16_t)(a >> 14))
    {
    case 0u: /* 0..90: the table itself */
        return sin__quadrant(q);
    case 1u: /* 90..180: mirror around 90 (sin(180-x) == sin(x)) */
        return sin__quadrant((uint16_t)(CGL_ANGLE_QUARTER - q));
    case 2u: /* 180..270: the table negated */
        return (int16_t)(-sin__quadrant(q));
    default: /* 270..360: mirrored and negated */
        return (int16_t)(-sin__quadrant((uint16_t)(CGL_ANGLE_QUARTER - q)));
    }
}

int16_t cgl_cos_q8(cgl_angle_t a)
{
    /* The addition cannot overflow into a wrong quadrant: cgl_angle_t wraps at one
       turn, and a quarter turn ahead of any angle is still an angle. */
    return cgl_sin_q8((cgl_angle_t)(a + CGL_ANGLE_QUARTER));
}

/**
 * Q8 -> whole pixels, rounding to nearest.
 *
 * `(v + 128) >> 8` is round-half-up and is correct for **both** signs even though
 * the shift floors: for a negative v the +128 first moves the value up by half a
 * unit and the floor then lands on the nearest integer. (The tempting
 * `v + (v < 0 ? -128 : 128)` is wrong for an exact negative multiple: it turns
 * -100.0 into -101.)
 */
static int16_t polar__round(int32_t value_q8)
{
    return (int16_t)((value_q8 + 128) >> 8);
}

void cgl_math_polar(cgl_angle_t a, int16_t radius, int16_t *dx, int16_t *dy)
{
    /* WARNING: round to nearest, do **not** truncate. Truncating `>> 8` on each axis
       independently biases *both* axes toward the origin, so a point meant to sit on
       radius r lands 1.4 px inside it -- and since it is a bias rather than noise it
       does not average out: a ring drawn from these points is visibly smaller than
       its nominal radius and a hand's tip is short by a pixel at every angle.
       (Measured on the 74 px gauge ring of tests/arc_host_test.c before the fix:
       3.57 px of radial error instead of 2.95.) */
    if (dx != NULL)
    {
        *dx = polar__round((int32_t)radius * (int32_t)cgl_cos_q8(a));
    }
    if (dy != NULL)
    {
        *dy = polar__round((int32_t)radius * (int32_t)cgl_sin_q8(a));
    }
}

void cgl_math_polar_q6(int16_t cx, int16_t cy, cgl_angle_t a, int16_t radius,
                       int32_t *x_q6, int32_t *y_q6)
{
    /* `radius * cos_q8` is `radius * cos * 256`, and Q6 needs `* 64`: one shift of
       two, not a divide. The result is truncated rather than rounded, which is
       correct here and wrong in cgl_math_polar(): a Q6 quantity is about to be
       rasterised with a one-pixel-wide coverage ramp (see cgl_arc_cov_q6), and
       rounding twice would bias the endpoint by half a ramp step. */
    if (x_q6 != NULL)
    {
        *x_q6 = ((int32_t)cx << 6) + (((int32_t)radius * (int32_t)cgl_cos_q8(a)) >> 2);
    }
    if (y_q6 != NULL)
    {
        *y_q6 = ((int32_t)cy << 6) + (((int32_t)radius * (int32_t)cgl_sin_q8(a)) >> 2);
    }
}

uint16_t cgl_math_frac_q8(int32_t value, int32_t vmin, int32_t vmax)
{
    uint32_t span;
    uint32_t off;
    uint16_t q = 0u;
    uint8_t i;

    if (vmax <= vmin)
    {
        return 0u; /* a rejected range draws empty rather than dividing by 0 */
    }
    span = (uint32_t)vmax - (uint32_t)vmin;
    if (value <= vmin)
    {
        return 0u;
    }
    if (value >= vmax)
    {
        return 256u;
    }
    off = (uint32_t)value - (uint32_t)vmin; /* strictly inside (0, span) */

    /* The loop doubles `off` (which is < span), so it needs span <= 2^31 - 1 to
       stay inside uint32. Squeezing a wider range by one bit costs 1/2^32 of the
       result -- orders of magnitude below the 1/256 it can express at all. */
    if (span > 0x7FFFFFFFu)
    {
        span >>= 1;
        off >>= 1;
        if (off >= span)
        {
            return 256u; /* the very top of the range: rounding, not an error */
        }
    }

    /* Eight bits of quotient plus one carried bit, which is rounded off below:
       truncating instead costs up to a whole pixel of fill on a short bar (a value
       exactly half way up a 5 px bar drew 2 px instead of 3). */
    for (i = 0u; i < 9u; i++)
    {
        q = (uint16_t)(q << 1);
        off <<= 1; /* off < span <= 2^31 - 1, so this cannot wrap */
        if (off >= span)
        {
            off -= span;
            q |= 1u;
        }
    }
    return (uint16_t)((q + 1u) >> 1);
}

uint16_t cgl_angle_deg(cgl_angle_t a)
{
    /* 360 / 65536 == 45 / 8192, so one multiply and one shift give whole degrees
       without a division. Truncation: 65535 -> 359, never 360. */
    return (uint16_t)(((uint32_t)a * 45u) >> 13);
}

uint32_t cgl_math_isqrt(uint32_t v)
{
    /* Digit-by-digit (restoring) square root: one bit of the root per iteration.
       `bit` starts at the largest power of four that is <= v, and each step decides
       whether the next lower pair of bits contributes to the root.
       WARNING: the decision is written **branchless** (a mask out of the comparison)
       on purpose. This runs twice per screen row for every annulus the rasterizer
       draws, and the comparison is data dependent, so a branch here mispredicts
       about half the time: on the reference build the branchy version cost ~170 host
       cycles per call and dominated the watch face's frame time. `& mask` /
       `res >> 1` cost the same on every core the framework targets. */
    uint32_t res = 0u;
    uint32_t bit = 1u << 30;

    while (bit > v)
    {
        bit >>= 2;
    }
    while (bit != 0u)
    {
        uint32_t cand = res + bit;
        uint32_t mask = (v >= cand) ? 0xFFFFFFFFu : 0u;

        v -= (cand & mask);
        res = (res >> 1) + (bit & mask);
        bit >>= 2;
    }
    return res;
}
