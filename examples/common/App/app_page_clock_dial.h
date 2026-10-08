// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    app_page_clock_dial.h
  * @brief   The dial's two *computed* surfaces and the wall behind them, as data in
  *          **flash**.
  *
  *  Why this file exists
  *  --------------------
  *  The dial's face, the outer ring and the wall the whole page sits on are all "one
  *  colour per pixel, out of a table" (see "the face", "the outer ring" and "the
  *  background" in app_page_clock.c). Those tables are **constant**: nothing in the
  *  firmware ever writes them, they are rebuilt identically on every boot, and in RAM
  *  they cost 706 bytes of a part that had 8 bytes of RAM to spare (the
  *  STM32G431CBU6's 32 KB, which is already carrying the whole demo's 19 KB of static
  *  state). Flash, meanwhile, is 128 KB and mostly empty. So they live here, in
  *  `.rodata`, and the RAM went back to the linker.
  *
  *  Why the generators are here too
  *  -------------------------------
  *  A table like this rots the moment someone changes the palette: the code that
  *  *would* have produced the new colours is not the code that draws them, and the
  *  page goes on rendering the old ones while the constants claim otherwise. The
  *  answer is to keep the generator **next to the data** as `static inline`
  *  functions -- the definition of the table, in the same file, using the same
  *  constants -- and to have `clock_test` re-derive every entry and compare. The
  *  firmware pays nothing for them: an unused `static inline` function emits no code
  *  at all.
  *
  *  Regenerating (after a deliberate palette or parameter change):
  *      the mismatches are reported by `clock_test`, which prints the whole
  *      regenerated array in pasteable form the moment one entry disagrees.
  *
  *  What the two tables say, and why the parameters are what they are, is documented
  *  at the generator that produces each one.
  ******************************************************************************
  */
#ifndef APP_PAGE_CLOCK_DIAL_H
#define APP_PAGE_CLOCK_DIAL_H

#include <cytgl/cgl_types.h>
#include <cytgl/cgl_math.h>

/* -------------------------------------------------------------------------- */
/* The palette of the two computed surfaces                                   */
/* -------------------------------------------------------------------------- */
/* These colours exist *only* as the inputs of the generators below: nothing draws
   with them directly (except the two flat fallbacks the warp path uses), because
   what is drawn is the table. That is why they live here and not with the drawing
   palette in the .c -- a colour that is baked into data belongs with the data.
   WARNING: the dial is dark *but not black*, and that is a hardware constraint, not
   a taste one: on this panel `CGL_RGB565(12, 15, 20)` is one level of red and two of
   blue, and cgl_color_blend_q8() truncates, so there is no room under it for a
   gradient -- a 25 % brush stroke toward black takes red to zero and leaves a
   green speckle where a dial used to be. The stops below span four levels of red and
   eleven of green: enough for a visible falloff and enough for the texture. */
#define COL_FACE_HI CGL_RGB565(58, 68, 84)   /* the lit centre of the gradient     */
#define COL_FACE CGL_RGB565(34, 40, 52)      /* the middle of the gradient         */
#define COL_FACE_EDGE CGL_RGB565(12, 15, 20) /* ... and its darkened rim           */
#define COL_BRUSH_HI CGL_RGB565(120, 136, 162) /* a brushed ridge catching light   */
#define COL_WELL_EDGE CGL_RGB565(16, 19, 25)   /* where the ring turns away        */
#define COL_WELL_LIT CGL_RGB565(126, 138, 158) /* ... and its lit crown            */

/* --- the wall the whole page sits on: one light, two ends ------------------- */
/* The page is lit from a single place -- up and to the left of the dial, where the
   dial's own specular highlight and the crystal's reflection already come from -- so
   the wall is a radial gradient out of that point: brightest behind the dial, nearly
   black in the far corner. Two stops are all it takes, because the table is indexed by
   r^2 and a *linear* ramp across the index is already a quadratic falloff in radius. */
#define COL_WALL_NEAR CGL_RGB565(30, 37, 50) /* behind the dial, where the light is */
#define COL_WALL_FAR CGL_RGB565(4, 5, 7)     /* ... and the far corner              */

/* -------------------------------------------------------------------------- */
/* The tables' geometry and texture parameters                                */
/* -------------------------------------------------------------------------- */

/** Resolution of the face table. It is indexed by `r^2` (see clock__face_px), so the
    entries are not evenly spaced in radius: at the rim one entry is about 0.16 px of
    radius, and in the middle it is several. */
#define CLOCK_FACE_LUT_N 256

/** Where the gradient turns over, as a fraction of the face radius in Q8 (0.52). */
#define CLOCK_FACE_KNEE 132u

/** Brushing: how strong it gets at the rim, Q8, and how many ridges there are over
    the radius (~1 px apart at the rim, which is as fine as the table can carry at the
    rim and as fine as the panel can show anywhere). */
#define CLOCK_FACE_BRUSH_AMP 44u
#define CLOCK_FACE_BRUSH_DENSITY 84u

/** Resolution of the ring's cross-section, and the two radii it spans: the well
    between the chapter ring and the case lip (see CLOCK_WELL_IN/OUT's public note in
    app_page_clock.h for what the ring *is*). */
#define CLOCK_WELL_N 33
#define CLOCK_WELL_IN 71
#define CLOCK_WELL_OUT 79

/** Resolution of the wall's gradient, and the radius at which it has run all the way
    out. 287 px is the distance from the light (up and to the left of the dial centre,
    see CLOCK_BG_LIGHT_X/Y in the .c) to the far corner of a 320 x 172 panel, so the
    gradient reaches its dark end exactly where the panel does and the corner is the
    darkest pixel on the screen. */
#define CLOCK_BG_LUT_N 32
#define CLOCK_BG_R_MAX 287
#define CLOCK_BG_R2_MAX ((uint32_t)CLOCK_BG_R_MAX * (uint32_t)CLOCK_BG_R_MAX)

/* -------------------------------------------------------------------------- */
/* The generators -- the definition of the data above                        */
/* -------------------------------------------------------------------------- */

/**
 * xorshift32 -- the whole of "the texture is procedural".
 *
 * Called once per table entry and once per sunburst ray, never once per pixel, so
 * the texture costs a lookup in the inner loop. What it buys is that no two ridges
 * and no two rays have the same strength: a sunburst with 128 *identical* rays reads
 * as a printed grating, and one with 128 slightly different ones reads as metal.
 */
static inline uint32_t clock__hash(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/** The face's radial gradient itself: three stops, at a radius in Q8 of the face. */
static inline cgl_color_t clock__face_gradient(uint16_t r_q8)
{
    if (r_q8 < CLOCK_FACE_KNEE)
    {
        return cgl_color_blend_q8(COL_FACE_HI, COL_FACE,
                                  (uint16_t)(((uint32_t)r_q8 << 8) / CLOCK_FACE_KNEE));
    }
    return cgl_color_blend_q8(COL_FACE, COL_FACE_EDGE,
                              (uint16_t)((((uint32_t)r_q8 - CLOCK_FACE_KNEE) << 8) /
                                         (256u - CLOCK_FACE_KNEE)));
}

/**
 * Entry `i` of the face table: the radial gradient plus the circular brushing, at
 * the **real** radius that entry stands for.
 *
 * The entries are uniform in `r^2` (what the inner loop can afford to index by), so
 * `r = sqrt(i / (N-1))` -- a square root per *entry*, not per pixel. That is what
 * makes the curve the designed one rather than the one a squared radius happens to
 * describe.
 *
 * The brush amplitude is faded by `r` (through `amp`), which does two jobs at once:
 * near the middle one entry spans ~8 px of radius, where a 1 px ridge would alias
 * into a moire rosette; at the rim one entry spans 0.16 px, where the ridges are
 * finally fine enough to be seen. Fading with `r` therefore both prevents the
 * artefact and produces what circular brushing actually looks like -- soft in the
 * middle, fine and bright at the edge.
 */
static inline cgl_color_t clock__face_lut_entry(uint16_t i)
{
    uint32_t q16 = ((uint32_t)i << 16) / (uint32_t)(CLOCK_FACE_LUT_N - 1);
    /* sqrt of a Q16 fraction is its Q8 root (see cgl_math_isqrt): the radius is then
       `r_q8 / 256` of the face, with no further scaling. */
    uint16_t r_q8 = (uint16_t)cgl_math_isqrt(q16);
    cgl_color_t col = clock__face_gradient(r_q8);
    uint32_t amp = ((uint32_t)r_q8 * CLOCK_FACE_BRUSH_AMP) >> 8;
    uint32_t h = clock__hash(((uint32_t)r_q8 * CLOCK_FACE_BRUSH_DENSITY) >> 8);
    uint32_t k = (amp * (1u + (h & 3u))) >> 2;

    return cgl_color_blend_q8(col, COL_BRUSH_HI, (uint16_t)k);
}

/**
 * Entry `i` of the outer ring's cross-section: a half sine across the well, from its
 * inner edge to its outer one.
 *
 * The profile is sampled at the entry's **sub-pixel** radius: `isqrt(d2 << 16)` is
 * the radius in Q8 (a Q16 argument yields a Q8 root, see cgl_math_isqrt), not at its
 * whole-pixel one. The difference is the whole shading of the cylinder: the ring is
 * 8 px thick, so a profile sampled at integer radii can only ever produce 9 distinct
 * values and the lit crown comes out as a staircase with flat tops, while sampling it
 * in Q8 gives every one of the 33 entries its own value and the crown curves.
 */
static inline cgl_color_t clock__well_lut_entry(uint16_t i)
{
    const uint32_t r2_in = (uint32_t)CLOCK_WELL_IN * (uint32_t)CLOCK_WELL_IN;
    const uint32_t r2_out = (uint32_t)CLOCK_WELL_OUT * (uint32_t)CLOCK_WELL_OUT;
    uint32_t d2 = r2_in + (((r2_out - r2_in) * (uint32_t)i) / (uint32_t)(CLOCK_WELL_N - 1));
    uint32_t r_q8 = cgl_math_isqrt(d2 << 16); /* the radius in Q8 */
    uint32_t t = ((r_q8 - ((uint32_t)CLOCK_WELL_IN << 8)) * (uint32_t)(CLOCK_WELL_N - 1)) /
                 (((uint32_t)CLOCK_WELL_OUT - (uint32_t)CLOCK_WELL_IN) << 8);
    uint16_t k = (uint16_t)cgl_sin_q8(
        (cgl_angle_t)(t * ((uint32_t)CGL_ANGLE_HALF / (uint32_t)(CLOCK_WELL_N - 1))));

    return cgl_color_blend_q8(COL_WELL_EDGE, COL_WELL_LIT, k);
}

/**
 * Entry `i` of the wall behind the page: two stops of one light.
 *
 * The table is indexed by `r^2` from the light (uniformly), so the ramp across the
 * *index* is what the pixel loop's single multiply buys -- and because `i` is already
 * proportionate to `r^2`, a linear ramp across it is a quadratic falloff in radius,
 * which is what a wall lit by one distant source looks like. Two stops are therefore
 * the whole gradient; a third would only be decoration.
 */
static inline cgl_color_t clock__bg_lut_entry(uint16_t i)
{
    return cgl_color_blend_q8(COL_WALL_NEAR, COL_WALL_FAR,
                              (uint16_t)(((uint32_t)i << 8) / (uint32_t)(CLOCK_BG_LUT_N - 1)));
}

/* -------------------------------------------------------------------------- */
/* The tables                                                                 */
/* -------------------------------------------------------------------------- */

/** `clock__face_lut_entry(i)` for every `i`, in flash. */
static const uint16_t s_face_lut[CLOCK_FACE_LUT_N] = {
    0x3A2Au,     0x3209u,     0x31E9u,     0x31E9u,     0x31E9u,     0x31E8u,     0x31C8u,     0x31C8u,     0x29C8u,     0x29C8u,     0x29C8u,     0x29C8u,
    0x29C8u,     0x29A8u,     0x29A8u,     0x29A8u,     0x29A8u,     0x29A8u,     0x29A7u,     0x29A7u,     0x29A7u,     0x29A7u,     0x29A7u,     0x2987u,
    0x2987u,     0x29A7u,     0x29A7u,     0x2987u,     0x2987u,     0x2987u,     0x2987u,     0x2187u,     0x2187u,     0x21A7u,     0x21A7u,     0x21A7u,
    0x2187u,     0x2187u,     0x2187u,     0x2186u,     0x2186u,     0x2186u,     0x2186u,     0x2166u,     0x2166u,     0x2166u,     0x2166u,     0x2166u,
    0x2186u,     0x2186u,     0x2187u,     0x2167u,     0x2167u,     0x2146u,     0x2146u,     0x2146u,     0x2146u,     0x2146u,     0x2166u,     0x2166u,
    0x2166u,     0x2167u,     0x2167u,     0x2167u,     0x2146u,     0x2146u,     0x2146u,     0x2146u,     0x2166u,     0x1945u,     0x1945u,     0x1945u,
    0x1945u,     0x1945u,     0x1945u,     0x2166u,     0x2166u,     0x2166u,     0x1925u,     0x1925u,     0x1925u,     0x1945u,     0x1945u,     0x1945u,
    0x1945u,     0x2166u,     0x2166u,     0x2166u,     0x1926u,     0x1926u,     0x1926u,     0x1926u,     0x1925u,     0x1925u,     0x1925u,     0x1905u,
    0x1905u,     0x1905u,     0x1905u,     0x2146u,     0x2146u,     0x2146u,     0x2146u,     0x1946u,     0x1946u,     0x1945u,     0x1924u,     0x1924u,
    0x1924u,     0x1924u,     0x18E4u,     0x18E4u,     0x18E4u,     0x18E4u,     0x2145u,     0x2145u,     0x2145u,     0x2145u,     0x1925u,     0x1925u,
    0x1925u,     0x1925u,     0x1104u,     0x1104u,     0x1104u,     0x1104u,     0x1104u,     0x10E4u,     0x10E4u,     0x10E4u,     0x10E4u,     0x1945u,
    0x1945u,     0x1945u,     0x1945u,     0x1905u,     0x1905u,     0x1905u,     0x1905u,     0x10E5u,     0x10E5u,     0x10E5u,     0x10E5u,     0x10E5u,
    0x10C4u,     0x10C4u,     0x10C4u,     0x10C4u,     0x10C3u,     0x10C3u,     0x10C3u,     0x10C3u,     0x10C3u,     0x10C3u,     0x10C3u,     0x10E4u,
    0x10E4u,     0x10E4u,     0x10E4u,     0x1904u,     0x1904u,     0x1904u,     0x18E4u,     0x18E4u,     0x1905u,     0x1905u,     0x1905u,     0x1905u,
    0x1905u,     0x10A3u,     0x10A3u,     0x10A3u,     0x10C3u,     0x10C3u,     0x10E4u,     0x10E4u,     0x10E4u,     0x10E4u,     0x10E4u,     0x1904u,
    0x1104u,     0x1104u,     0x1104u,     0x1104u,     0x1925u,     0x1925u,     0x1925u,     0x1925u,     0x1925u,     0x08C3u,     0x08C3u,     0x08A3u,
    0x08A3u,     0x08A3u,     0x10C4u,     0x10C4u,     0x10C4u,     0x10C4u,     0x10C4u,     0x10E3u,     0x10E3u,     0x10E4u,     0x10E4u,     0x10E4u,
    0x10E4u,     0x1904u,     0x1904u,     0x1904u,     0x1904u,     0x1904u,     0x08A2u,     0x08A2u,     0x08A2u,     0x08A2u,     0x08A2u,     0x10C3u,
    0x10C3u,     0x10C3u,     0x10C3u,     0x10C3u,     0x10C3u,     0x10E4u,     0x10E4u,     0x10C4u,     0x10C4u,     0x10C4u,     0x18E4u,     0x18E4u,
    0x18E4u,     0x18E4u,     0x18E4u,     0x18E4u,     0x10A3u,     0x10A3u,     0x10A3u,     0x10A3u,     0x10A3u,     0x10A3u,     0x0882u,     0x0882u,
    0x0882u,     0x0882u,     0x0882u,     0x0882u,     0x1904u,     0x1904u,     0x1905u,     0x1905u,     0x1905u,     0x1905u,     0x10C4u,     0x10C4u,
    0x10C4u,     0x10C4u,     0x10C4u,     0x10A3u, 
};

/** `clock__well_lut_entry(i)` for every `i`, in flash. */
static const uint16_t s_well_lut[CLOCK_WELL_N] = {
    0x1083u,     0x18C4u,     0x2126u,     0x2987u,     0x31E9u,     0x424Au,     0x4A8Bu,     0x52CDu,     0x5B2Eu,     0x636Fu,     0x6390u,     0x6BD1u,
    0x73F1u,     0x7412u,     0x7432u,     0x7432u,     0x7C53u,     0x7432u,     0x7432u,     0x7412u,     0x73F1u,     0x6BD1u,     0x6390u,     0x636Fu,
    0x5B2Eu,     0x52CDu,     0x4A8Bu,     0x424Au,     0x31E9u,     0x2987u,     0x2126u,     0x18C4u,     0x1083u, 
};

/** `clock__bg_lut_entry(i)` for every `i`, in flash: the wall. */
static const uint16_t s_bg_lut[CLOCK_BG_LUT_N] = {
    0x1926u,     0x1105u,     0x1105u,     0x1105u,     0x10E5u,     0x10E5u,     0x10E4u,     0x10E4u,     0x10C4u,     0x10C4u,     0x10C4u,     0x08C3u,
    0x08A3u,     0x08A3u,     0x08A3u,     0x08A3u,     0x0882u,     0x0882u,     0x0882u,     0x0882u,     0x0862u,     0x0061u,     0x0061u,     0x0061u,
    0x0041u,     0x0041u,     0x0040u,     0x0040u,     0x0020u,     0x0020u,     0x0020u,     0x0020u, 
};

#endif /* APP_PAGE_CLOCK_DIAL_H */
