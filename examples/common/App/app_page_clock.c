// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    app_page_clock.c
  * @brief   Watch face page. See app_page_clock.h for what the page is, and for
  *          the honest statement about where its time comes from.
  *
  *  What is left in this file, and why
  *  ----------------------------------
  *  This page used to *be* the watch: 2 500 lines of dial, hands, ring, bezel,
  *  crystal, chapter marks and time base, all of it written here because the
  *  framework had no way to draw a watch. All of that is now framework code --
  *
  *      cgl_arc     a shaded radial surface (`cgl_arc_surface_t`), a tapered
  *                  antialiased blade (`cgl_arc_stroke_t`), and the ring gauge
  *      cgl_watch   the dial: its time base and calendar, its chapter ring, its
  *                  three hands and hub, its seconds indicator, its crystal, and
  *                  -- the part that matters most -- its **damage model**
  *
  *  -- and what remains here is what belongs to an application: the room the dial
  *  sits in, and the complications around it.
  *
  *      s_cfg              the dial's design: geometry, palette, the two surfaces,
  *                         the crystal, the hands, the tick, the start reading
  *      clock__bg_px()     the wall, the card and the panel's bevelled edges
  *      clock__draw_digits four digits that roll independently when the minute turns
  *      clock__draw_gauge  the number inside the framework's arc gauge
  *      the page lifecycle time, edges, damage for the two complications
  *
  *  The three ideas the dial was built on are now stated in cgl_arc.h and
  *  cgl_watch.h, where the code they explain lives:
  *
  *   1. **The dial is a pure function of the clock; only the *events* are
  *      tweened.** The page inherits this: its own three animations (the digit
  *      roll, the entrance and the minute flash) are all events, and the hands are
  *      not animated at all -- they are a function of the time.
  *   2. **Every element of the dial is "a point at an angle on a radius".**
  *   3. **Every surface is a pure function of the pixel.** The one surface this
  *      page still owns outright is its background, and it is pure for the same
  *      reason: `app_page_clock_bg_color()` is the expression the page paints with,
  *      so `clock_test` can assert the *design* -- "the wall is lit from the dial's
  *      own light", "the card is lifted off the wall", "the top edge catches the
  *      light" -- instead of photographing it.
  *
  *  The background: one light, one wall, one card
  *  --------------------------------------------
  *  The page's root used to paint nothing, on purpose: the navigator's host widget
  *  already floods every band it presents with CGL_CFG_BG_COLOR (which is what makes
  *  "a band handed to the panel is complete" true), so a page that fills the screen
  *  again doubles the single most expensive thing in a frame. That is a correct
  *  *frame* and a flat *screen*: everything outside the dial -- the corners around
  *  it, and the whole complication column -- was one value of black, so a dial with
  *  a metal ring under a glass cover still ended up looking like a sticker on a
  *  dark page.
  *
  *  The design is deliberately **one idea rather than four decorations: the screen
  *  is lit from the same place the dial is.** A single radial gradient centred up
  *  and to the left of the dial -- where the dial's own specular highlight and the
  *  crystal's reflection already come from -- leaves the wall brightest just behind
  *  the dial and nearly black in the far corner. The complications then sit on a
  *  raised glass card whose lit top edge is that same light a third time, and the
  *  panel's top and bottom rows are bevelled so the screen reads as set *into* a
  *  case.
  *
  *  WARNING: the host still fills the band before this runs, so the page really
  *  does paint its background twice -- exactly what the note on the root used to
  *  warn against. It is affordable for one measurable reason: **the damage is
  *  incremental.** The double fill costs 320 x 4 px per *damaged* band (about
  *  1 300 px in a typical frame) instead of a whole screen per frame. The wall
  *  itself is one multiply and a table lookup with no blend at all -- it is opaque
  *  and goes straight into the row -- and the pass skips the disc the dial covers,
  *  which is a third of the screen.
  ******************************************************************************
  */
#include "app_page_clock.h"
#include "app_page_clock_dial.h" /* the dial's two surfaces, as tables in flash */

#include <cytgl/cytgl_config.h>
#include <cytgl/cgl_canvas.h>
#include <cytgl/cgl_font.h>
#include <cytgl/cgl_math.h>

/* -------------------------------------------------------------------------- */
/* Palette                                                                    */
/* -------------------------------------------------------------------------- */

#define COL_BG CGL_COLOR_BLACK

/* The colours of the dial's two *computed* surfaces -- the face's gradient and the
   ring's cross-section -- live with the tables they are baked into, in
   app_page_clock_dial.h: nothing draws with them directly (what is drawn is the
   table), so a colour that is data belongs with the data. Everything below is a
   colour this file really paints with -- or hands to a surface as a parameter. */

/** The specular highlight on the dial. */
#define COL_SPECULAR CGL_RGB565(176, 194, 224)

#define COL_BEZEL CGL_RGB565(56, 62, 74)
#define COL_BEZEL_HI CGL_RGB565(150, 162, 184) /* the lit half of the bezel       */

/* --- the outer ring: the tilt of the well (see clock__well_px) --------------
   Its cross-section is in the dial header; these two are what the well's *walls*
   blend toward, and they are used at run time. */
#define COL_WELL_SHADE CGL_RGB565(7, 8, 11)      /* the wall facing away from light */
#define COL_WELL_LIGHT CGL_RGB565(168, 182, 204) /* the wall facing the light       */

/** The gauge's empty track. It is a **groove cut into the card** now, not a ring
    drawn on black: the complication column has a designed background (see "the
    background"), and the old dark grey was within two levels of it, i.e. invisible. */
#define COL_TRACK CGL_RGB565(16, 20, 27)
#define COL_TICK CGL_RGB565(96, 104, 122)
#define COL_TICK_HOUR CGL_RGB565(214, 221, 232)
#define COL_HAND CGL_RGB565(240, 244, 250)
#define COL_ACCENT CGL_RGB565(255, 106, 0) /* second hand / ring / gauge */
#define COL_ACCENT_HI CGL_RGB565(255, 186, 92)
#define COL_TRAIL CGL_RGB565(255, 170, 40) /* what a mark blends toward as it lights */
#define COL_TEXT CGL_RGB565(238, 242, 248)
#define COL_DIM CGL_RGB565(122, 130, 146)
#define COL_BRAND CGL_RGB565(64, 71, 86)

/** The reflected light of the crystal. See cgl_watch_glass_t. */
#define COL_GLASS CGL_RGB565(232, 240, 252)

#define CLOCK_FONT (&cgl_font_8x16)
#define CLOCK_BIG_FONT (&cgl_font_12x24)

/* --- the dial's texture parameters, which are the app's palette -------------
 *
 * These are the numbers behind the two table generators in
 * app_page_clock_dial.h: the face's sunburst, its specular highlight, and the
 * well's tilt. They live here rather than in the framework because they are
 * *this* dial's look -- an application that wants another one changes these, or
 * bakes its own tables with cgl_arc_surface_bake(). */

/** The sunburst: how strong the lit rays get at the rim, Q8, and the table index
    they start at. The unlit ones get a quarter of it. */
#define CLOCK_FACE_RAY_AMP 60u
#define CLOCK_FACE_RAY_MIN 30u

/** The highlight's peak opacity, Q8. */
#define CLOCK_SPEC_ALPHA 54u

/** How dark the well's top wall gets and how bright its bottom one, Q8 at the
    extremes. */
#define CLOCK_WELL_TILT 120u

/* --- the crystal: three reflections, as parameters -------------------------
 *
 *  1. **the sweep**: a wide, very soft band of reflected light crossing the dial.
 *     It is a function of one dot product (the pixel's position along the light's
 *     axis), so it costs two multiplies and a table-free quadratic falloff -- and
 *     it is what makes the surface read as *glossy* rather than matte.
 *  2. **the crescent at the rim**: the glass curves steeply at its edge, so the
 *     reflection there compresses into a thin bright arc that follows the ring.
 *     This is the strongest cue of the three and the cheapest: an annulus test on
 *     r^2 plus the *same* dot product, windowed to the lit side.
 *  3. **the echo**: a much fainter second reflection, diametrically opposite, off
 *     the inside of the dome. It is the same two tests with the sign of the dot
 *     product flipped -- and it is the detail that stops the cover from looking
 *     like one airbrushed stripe.
 *
 * WARNING: the cover is **clipped to the dial** (`r <= r_face`). The case lip is
 * metal, not glass, and a reflection painted across it would read as a scratch. */

/** Where the light comes from, as a unit vector in Q8 -- the same direction as the
    specular highlight on the dial, because it is the same light. */
#define CLOCK_GLASS_UX (-166)
#define CLOCK_GLASS_UY (-195)

/** The sweep: how wide it is (half width, px), where it sits along the light axis --
    **positive, i.e. on the lit side**, because a reflection of a light up and to
    the left is seen up and to the left -- and how opaque it gets (Q8, ~16 %). */
#define CLOCK_GLASS_SWEEP_W 52
#define CLOCK_GLASS_SWEEP_AT 34
#define CLOCK_GLASS_SWEEP_ALPHA 40u

/** The rim crescent: the arc it follows, how far along the light axis it starts, how
    fast it comes up, and its peak. */
#define CLOCK_GLASS_EDGE_IN 66
#define CLOCK_GLASS_EDGE_OUT 76
#define CLOCK_GLASS_EDGE_AT 26
#define CLOCK_GLASS_EDGE_RAMP 54
#define CLOCK_GLASS_EDGE_ALPHA 74u

/** ... and its echo, opposite: smaller, further in, and much fainter, because it is
    the light that came off the *inside* of the dome. */
#define CLOCK_GLASS_ECHO_IN 30
#define CLOCK_GLASS_ECHO_OUT 48
#define CLOCK_GLASS_ECHO_AT 18
#define CLOCK_GLASS_ECHO_RAMP 30
#define CLOCK_GLASS_ECHO_ALPHA 18u

/* -------------------------------------------------------------------------- */
/* The dial, as configuration                                                 */
/* -------------------------------------------------------------------------- */

/*
 * `s_cfg` is the whole dial, and it is `static const` on purpose: the page's three
 * pure colour functions (`app_page_clock_face_color`, `_ring_color`,
 * `_glass_over`) are answerable **before the page has ever been created**, and a
 * configuration assembled inside `on_create` could not promise that. Nothing at run
 * time mutates it -- a caller that wants another dial fills in its own
 * `cgl_watch_config_t` and calls `cgl_watch_set_config()`.
 *
 * The tables are the ones in app_page_clock_dial.h, pointed at directly: they are
 * `static const` arrays in flash, so the surface costs no RAM at all, and the
 * generator that produced them is `cgl_arc_surface_bake()` -- which
 * `clock_test` re-runs against every entry, so a palette change that is not
 * followed by a re-bake is caught rather than merely looked at.
 */
static const cgl_watch_config_t s_cfg = {
    .geom = {
        .cx = CLOCK_CX,
        .cy = CLOCK_CY,
        .r_face = CLOCK_R_FACE,
        .r_ring = CLOCK_R_RING,
        .ring_w = CLOCK_RING_W,
        .r_well_in = CLOCK_WELL_IN,
        .r_well_out = CLOCK_WELL_OUT,
        .r_tick_out = CLOCK_R_TICK_OUT,
        .r_tick_major_in = CLOCK_R_TICK_HOUR_IN,
        .r_tick_minor_in = CLOCK_R_TICK_MIN_IN,
        .r_hub = CLOCK_R_HUB,
        .r_tail = CLOCK_R_TAIL,
    },
    .anim = {
        .tick_count = 60u,
        .tick_major = 5u,
        .tick_w_minor = 2u,
        .tick_w_major = 3u,
        .second_mode = (uint8_t)CLOCK_SECOND_MODE,
        .tick_ms = (uint16_t)CLOCK_TICK_MS,
        .tick_ease = (uint8_t)CLOCK_TICK_EASE,
        .reveal_ms = (uint16_t)CLOCK_REVEAL_MS,
        .reveal_ease = (uint8_t)CGL_EASE_OUT_CUBIC,
        .pulse_ms = (uint16_t)CLOCK_PULSE_MS,
        .pulse_ease = (uint8_t)CGL_EASE_OUT_QUAD,
        .trail_ms = 2400u,
        .shadow_off = CLOCK_SHADOW_PX,
        .shadow_alpha = (uint8_t)CLOCK_SHADOW_ALPHA,
    },
    .style = {
        .bg = COL_BG,
        .bezel = COL_BEZEL,
        .bezel_hi = COL_BEZEL_HI,
        .bezel_w = 2u,
        /* From 150 degrees (below 9 o'clock) round to 270 degrees (12 o'clock): in
           this convention 0 is 3 o'clock and the angle grows clockwise. */
        .bezel_lit_start = CGL_ANGLE_DEG(150),
        .bezel_lit_sweep = (uint32_t)CGL_ANGLE_DEG(120),
        .tick = COL_TICK,
        .tick_major = COL_TICK_HOUR,
        .trail = COL_TRAIL,
        .hand = COL_HAND,
        /* The hub cap is the hands' own colour, and it pulses with them -- see
           cgl_watch.h's CGL_WATCH_F_PULSE. */
        .hub = COL_HAND,
        .accent = COL_ACCENT,
        .accent_hi = COL_ACCENT_HI,
        .face = {
            .kind = (uint8_t)CGL_ARC_FACE,
            .cx = CLOCK_CX,
            .cy = CLOCK_CY,
            .r_in = 0,
            .r_out = CLOCK_R_FACE,
            .c_hi = COL_FACE_HI,
            .c_mid = COL_FACE,
            .c_lo = COL_FACE_EDGE,
            .knee_q8 = (uint8_t)CLOCK_FACE_KNEE,
            .tex = COL_BRUSH_HI,
            .tex_amp = (uint8_t)CLOCK_FACE_BRUSH_AMP,
            .tex_density = (uint8_t)CLOCK_FACE_BRUSH_DENSITY,
            .ray_amp = (uint8_t)CLOCK_FACE_RAY_AMP,
            .ray_min = (uint16_t)CLOCK_FACE_RAY_MIN,
            .spec = COL_SPECULAR,
            .light_dx = CLOCK_SPEC_DX,
            .light_dy = CLOCK_SPEC_DY,
            .light_r = CLOCK_SPEC_R,
            .light_alpha = (uint8_t)CLOCK_SPEC_ALPHA,
            /* Unused on a FACE; stated so that the struct is fully initialised. */
            .tilt_dark = COL_WELL_SHADE,
            .tilt_light = COL_WELL_LIGHT,
            .tilt = 0u,
            .lut = s_face_lut,
            .lut_n = (uint16_t)CLOCK_FACE_LUT_N,
        },
        .well = {
            .kind = (uint8_t)CGL_ARC_RING,
            .cx = CLOCK_CX,
            .cy = CLOCK_CY,
            .r_in = CLOCK_WELL_IN,
            .r_out = CLOCK_WELL_OUT,
            .c_lo = COL_WELL_EDGE,
            .c_mid = COL_WELL_EDGE,
            .c_hi = COL_WELL_LIT,
            .knee_q8 = 0u,
            .tex = COL_WELL_LIT,
            .tex_amp = 0u,
            .tex_density = 0u,
            .ray_amp = 0u,
            .ray_min = 0u,
            .spec = COL_WELL_LIT,
            .light_dx = 0,
            .light_dy = 0,
            .light_r = 0,
            .light_alpha = 0u,
            .tilt_dark = COL_WELL_SHADE,
            .tilt_light = COL_WELL_LIGHT,
            .tilt = (uint8_t)CLOCK_WELL_TILT,
            .lut = s_well_lut,
            .lut_n = (uint16_t)CLOCK_WELL_N,
        },
        .glass = {
            .tint = COL_GLASS,
            .ux = CLOCK_GLASS_UX,
            .uy = CLOCK_GLASS_UY,
            .sweep_w = CLOCK_GLASS_SWEEP_W,
            .sweep_at = CLOCK_GLASS_SWEEP_AT,
            .sweep_alpha = (uint8_t)CLOCK_GLASS_SWEEP_ALPHA,
            .edge_in = CLOCK_GLASS_EDGE_IN,
            .edge_out = CLOCK_GLASS_EDGE_OUT,
            .edge_at = CLOCK_GLASS_EDGE_AT,
            .edge_ramp = CLOCK_GLASS_EDGE_RAMP,
            .edge_alpha = (uint8_t)CLOCK_GLASS_EDGE_ALPHA,
            .echo_in = CLOCK_GLASS_ECHO_IN,
            .echo_out = CLOCK_GLASS_ECHO_OUT,
            .echo_at = CLOCK_GLASS_ECHO_AT,
            .echo_ramp = CLOCK_GLASS_ECHO_RAMP,
            .echo_alpha = (uint8_t)CLOCK_GLASS_ECHO_ALPHA,
        },
    },
    .hour = {
        .r_base = CLOCK_R_HOUR_BASE,
        .r_tip = (int16_t)(CLOCK_R_HOUR - 2),
        .w_base = CGL_CFG_WATCH_HOUR_W_BASE,
        .w_tip = CGL_CFG_WATCH_HOUR_W_TIP,
        .color = COL_HAND,
    },
    .minute = {
        .r_base = CLOCK_R_MINUTE_BASE,
        .r_tip = (int16_t)(CLOCK_R_MINUTE - 2),
        .w_base = CGL_CFG_WATCH_MINUTE_W_BASE,
        .w_tip = CGL_CFG_WATCH_MINUTE_W_TIP,
        .color = COL_HAND,
    },
    .second = {
        .r_base = CLOCK_R_TAIL,
        .r_tip = CLOCK_R_SECOND,
        .w_base = CGL_CFG_WATCH_SECOND_W,
        .w_tip = CGL_CFG_WATCH_SECOND_W,
        .color = COL_ACCENT,
    },
    .start = {
        .h = (uint8_t)CLOCK_START_H,
        .m = (uint8_t)CLOCK_START_M,
        .s = (uint8_t)CLOCK_START_S,
        .ms = 0u,
        .year = CLOCK_START_Y,
        .month = (uint8_t)CLOCK_START_MON,
        .day = (uint8_t)CLOCK_START_D,
        .wday = (uint8_t)CLOCK_START_WDAY,
    },
    .flags = (uint16_t)(CGL_WATCH_F_BEZEL | CGL_WATCH_F_TICKS | CGL_WATCH_F_RING |
                        CGL_WATCH_F_TRAIL | CGL_WATCH_F_HUB | CGL_WATCH_F_GLASS |
                        CGL_WATCH_F_BRAND | CGL_WATCH_F_HOUR | CGL_WATCH_F_MINUTE |
                        CGL_WATCH_F_SECOND | CGL_WATCH_F_PULSE | CGL_WATCH_F_REVEAL),
    .brand = "CYTGL",
    .brand_font = CLOCK_FONT,
    .brand_dx = -20,
    .brand_dy = 40,
    .brand_color = COL_BRAND,
};

/** The dial's widget frame: the left 176 px. The face is centred at x = 86 with
    r = 80, so it reaches x = 166 and the frame spans the whole panel height. */
#define CLOCK_DIAL_W 176

/* -------------------------------------------------------------------------- */
/* Calendar                                                                   */
/* -------------------------------------------------------------------------- */

/** Month names as the date line spells them. */
static const char *const s_month[12] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

/** Weekday names, 0 = Sunday (`cgl_watch_time_t::wday`). */
static const char *const s_wday[7] = {
    "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"
};

/* -------------------------------------------------------------------------- */
/* The background: one light, one wall, one card                              */
/* -------------------------------------------------------------------------- */

/** Where the light is: up and to the left of the dial centre, the same place the
    dial's specular highlight sits and the crystal's reflection comes from. */
#define CLOCK_BG_LIGHT_X (CLOCK_CX - 30)
#define CLOCK_BG_LIGHT_Y (CLOCK_CY - 30)

/** `(N-1) / R_MAX^2` in Q16: the wall table index of a pixel's r^2. */
#define CLOCK_BG_LUT_SCALE \
    ((uint32_t)(((uint32_t)(CLOCK_BG_LUT_N - 1) << 16) / CLOCK_BG_R2_MAX))

/** The panel's own edges: a lit top row and a dark bottom one. They are the cheapest
    thing on this page and they do most of the work of "this is a device". */
#define COL_EDGE_TOP CGL_RGB565(48, 57, 74)
#define COL_EDGE_BOT CGL_RGB565(2, 2, 4)

/** The complication card: a raised glass panel behind the right hand column. Its
    rectangle is the column's, and every complication sits inside it with a margin --
    including the two labels, whose frames are wider than their centred text.
    WARNING: the card is **one flat colour**, and that is a consequence of the panel,
    not a lack of ambition. Measured on the first version, which did carry a vertical
    gradient: the card came out in horizontal bands 6 to 23 rows tall, because
    `CGL_RGB565(46,56,72)` and `CGL_RGB565(24,30,41)` differ by *three* levels of red
    and seven of green, so a 160 px gradient can ever only be eight flat steps. The
    "raised" reading does not need the gradient anyway -- the 1 px edge and the lit
    top row carry it -- and a flat panel is what a machined insert actually looks
    like. (The wall behind it *is* a gradient, and it is dithered; see clock__dither.) */
#define CLOCK_CARD_X0 182
#define CLOCK_CARD_Y0 6
#define CLOCK_CARD_X1 314
#define CLOCK_CARD_Y1 166
#define CLOCK_CARD_R 10
#define COL_CARD_FACE CGL_RGB565(30, 37, 49)
#define COL_CARD_EDGE CGL_RGB565(58, 70, 92)
#define COL_CARD_EDGE_TOP CGL_RGB565(104, 122, 152)

/** The dial's opaque disc (r <= 79: the face, the well and the lip all cover it), as
    a squared radius, so the background pass can skip a third of the screen. */
#define CLOCK_BG_DIAL_COVER2 ((int32_t)79 * 79)

/* --- the dither, and why a wall gradient needs one -------------------------
 *
 * A 4x4 ordered (Bayer) matrix, and the reason it is not an optional refinement:
 *
 * **This panel cannot draw a smooth dark gradient.** A colour is five levels of red
 * and blue, six of green; the wall runs from `(30,37,50)` at the light to
 * `(4,5,7)` in the far corner, which is *three* levels of red, nine of green and six
 * of blue over 290 px of radius. Measured on the undithered version, the wall came
 * out in flat bands 11 to 89 rows tall -- the eye reads those as stripes on the
 * screen, not as a gradient.
 *
 * The cure is the one every low-colour-depth display has always used: nudge the
 * sample *along the gradient* by a fixed pattern, so that where the quantisation
 * would step, the pixels alternate between the two neighbouring levels and the step
 * becomes a fine texture instead. Ordered rather than random on purpose -- a hash
 * dither on a large flat area reads as sensor noise, while a Bayer pattern reads as
 * a woven texture.
 *
 * The amplitude is in **table entries** (the index is what is nudged, which is why
 * this is one add and a clamp per pixel): +-2 entries is +-6 % of the gradient, i.e.
 * a little over half of one quantisation step of its coarsest channel. That is
 * enough to smear a band edge over a couple of dozen pixels, while the middle of a
 * band is left completely alone -- there the whole dither range still lands on the
 * same colour, which is what keeps the texture out of the flat areas (measured: 4
 * entries was already swinging the green channel by two levels, which reads as grain
 * rather than as a gradient).
 */
static const uint8_t s_bayer4[16] = { 0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u,
                                      3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u };
#define CLOCK_BG_DITHER 2

/** The ordered dither's nudge for a pixel, in wall table entries (see s_bayer4). */
CGL_AINLINE int32_t clock__dither(int16_t x, int16_t y)
{
    uint32_t at = ((((uint32_t)y & 3u) << 2) | ((uint32_t)x & 3u));

    return (((int32_t)s_bayer4[at] - 8) * CLOCK_BG_DITHER) / 8;
}

/** The card's own colour: one flat face, its edge ring, and the lit top edge. */
CGL_AINLINE cgl_color_t clock__card_face(int32_t x, int32_t y)
{
    cgl_color_t face = COL_CARD_FACE;

    if ((x == CLOCK_CARD_X0) || (x == CLOCK_CARD_X1) || (y == CLOCK_CARD_Y1))
    {
        face = COL_CARD_EDGE;
    }
    else if (y == CLOCK_CARD_Y0)
    {
        face = COL_CARD_EDGE_TOP;
    }
    return face;
}

/**
 * Coverage of a pixel by the card's rounded rectangle, Q8.
 *
 * Only the four corner boxes cost anything: everywhere else the answer is "inside" or
 * "outside" after two comparisons, which is what makes it affordable to draw the card
 * a pixel at a time. The rounding uses the **same** 128 entry ramp the framework's
 * needles use (`cgl_arc_cov_q6`), so the card's corners are as smooth as a pointer's
 * tip -- one opinion about what "half covered" means for the whole page.
 */
CGL_AINLINE uint16_t clock__card_cov(int32_t x, int32_t y)
{
    bool left = (x < (CLOCK_CARD_X0 + CLOCK_CARD_R));
    bool right = (x > (CLOCK_CARD_X1 - CLOCK_CARD_R));
    bool top = (y < (CLOCK_CARD_Y0 + CLOCK_CARD_R));
    bool bottom = (y > (CLOCK_CARD_Y1 - CLOCK_CARD_R));
    int32_t dx;
    int32_t dy;

    /* WARNING: the radius only applies where **both** axes are in a corner box.
       Clamping each axis on its own -- which is what this did first -- rounds the
       middle of the card's long edges too, and a 1 px border drawn at half coverage
       is not a border: the top row came out as a soft 50 % blend of the edge colour
       and the wall. */
    if (!(left || right) || !(top || bottom))
    {
        if ((x < CLOCK_CARD_X0) || (x > CLOCK_CARD_X1) || (y < CLOCK_CARD_Y0) ||
            (y > CLOCK_CARD_Y1))
        {
            return 0u; /* outside the rectangle */
        }
        return 256u; /* the straight part of the edge, or the interior */
    }
    if ((x < CLOCK_CARD_X0) || (x > CLOCK_CARD_X1) || (y < CLOCK_CARD_Y0) ||
        (y > CLOCK_CARD_Y1))
    {
        return 0u; /* in a corner box but outside the rectangle */
    }

    dx = x - (left ? (CLOCK_CARD_X0 + CLOCK_CARD_R) : (CLOCK_CARD_X1 - CLOCK_CARD_R));
    dy = y - (top ? (CLOCK_CARD_Y0 + CLOCK_CARD_R) : (CLOCK_CARD_Y1 - CLOCK_CARD_R));
    /* `isqrt(d^2 << 12)` is the distance in Q6 (a Q12 argument yields a Q6 root), and
       `cgl_arc_cov_q6()` takes a signed distance *inside* the shape. */
    return cgl_arc_cov_q6(((int32_t)CLOCK_CARD_R << 6) -
                          (int32_t)cgl_math_isqrt((uint32_t)(((dx * dx) + (dy * dy)) << 12)));
}

/**
 * The page's background at a **screen** coordinate: the wall, then the card, then the
 * panel's two bevelled edges.
 *
 * Pure, like the dial's surfaces: same coordinate, same colour, for ever, with no
 * state read and none written. `app_page_clock_bg_color()` is the public door to it,
 * so a test can assert the *design* -- the wall is lit from the dial's own light, the
 * card is lifted off the wall, the top edge catches the light -- instead of
 * describing it.
 */
CGL_AINLINE cgl_color_t clock__bg_px(int16_t x, int16_t y)
{
    int32_t dx = (int32_t)x - CLOCK_BG_LIGHT_X;
    int32_t dy = (int32_t)y - CLOCK_BG_LIGHT_Y;
    int32_t i = (int32_t)(((uint32_t)((dx * dx) + (dy * dy)) * CLOCK_BG_LUT_SCALE) >> 16);
    cgl_color_t col;

    /* The dither rides on the *index*, which is what makes it one add: see s_bayer4
       for why a wall gradient cannot be drawn without one. */
    i += clock__dither(x, y);
    if (i < 0)
    {
        i = 0;
    }
    else if (i > (int32_t)(CLOCK_BG_LUT_N - 1))
    {
        i = (int32_t)(CLOCK_BG_LUT_N - 1);
    }
    col = s_bg_lut[(uint32_t)i];

    if ((x >= CLOCK_CARD_X0) && (x <= CLOCK_CARD_X1) && (y >= CLOCK_CARD_Y0) &&
        (y <= CLOCK_CARD_Y1))
    {
        uint16_t cov = clock__card_cov(x, y);

        if (cov == 256u)
        {
            col = clock__card_face(x, y); /* opaque interior: no blend at all */
        }
        else if (cov != 0u)
        {
            col = cgl_color_blend_q8(col, clock__card_face(x, y), cov);
        }
    }

    if (y == 0)
    {
        col = COL_EDGE_TOP;
    }
    else if (y == (int16_t)(CGL_CFG_SCREEN_H - 1))
    {
        col = COL_EDGE_BOT;
    }
    return col;
}

/**
 * Paint the background for this band, straight into the row: one table lookup per
 * pixel and no blend, except at the card's four corners.
 *
 * WARNING: like the dial's own surfaces, this is **skipped while a warp is
 * installed**. During a suction transition the clip is in destination space, and
 * mapping a whole screen per pixel for a page that is being squashed into a corner
 * is not worth a frame rate; the host's flat fill IS the background for those
 * 280 ms.
 */
static void clock__bg(cgl_widget_t *w, cgl_canvas_t *c)
{
    int16_t x0;
    int16_t x1;
    int16_t y0;
    int16_t y1;
    int16_t y;

    (void)w;

    if (c->warp != NULL)
    {
        return;
    }

    x0 = (int16_t)(c->clip.x0 - c->pan.x);
    x1 = (int16_t)(c->clip.x1 - c->pan.x);
    y0 = (int16_t)(c->clip.y0 - c->pan.y);
    y1 = (int16_t)(c->clip.y1 - c->pan.y);
    if (y0 < (int16_t)(c->origin_y - c->pan.y))
    {
        y0 = (int16_t)(c->origin_y - c->pan.y);
    }
    if (y1 > (int16_t)(c->origin_y + c->height - 1 - c->pan.y))
    {
        y1 = (int16_t)(c->origin_y + c->height - 1 - c->pan.y);
    }
    if (x0 > x1)
    {
        return;
    }

    for (y = y0; y <= y1; y++)
    {
        int32_t dy = (int32_t)y - CLOCK_CY;
        cgl_color_t *row = cgl_canvas_row(c, (int16_t)(y + c->pan.y));
        int32_t x;

        if (row == NULL)
        {
            continue;
        }
        for (x = x0; x <= x1; x++)
        {
            int32_t dx = x - (int32_t)CLOCK_CX;

            /* The dial is drawn over this a moment later and it is opaque out to r =
               79 (face, well and lip): a third of the screen that never has to be
               painted. The pixels between 79 and 81 are the bezel's antialiased outer
               edge, which blends with the wall, so the cut is at 79 and not at 81. */
            if (((dx * dx) + (dy * dy)) <= CLOCK_BG_DIAL_COVER2)
            {
                continue;
            }
            row[(int16_t)(x + c->pan.x)] = clock__bg_px((int16_t)x, (int16_t)y);
        }
    }
}

static const cgl_widget_vt_t clock__group_vt = {
    .type = "clock_group",
    .draw = clock__bg,
    .update = NULL,
    .event = NULL,
};

/* -------------------------------------------------------------------------- */
/* The digital time complication                                              */
/* -------------------------------------------------------------------------- */

/** Slot indices of "HH:MM"; slot 2 is the colon and never rolls. */
#define CLOCK_SLOT_COLON 2u
#define CLOCK_SLOTS 5u

/** Write "HH:MM" (6 bytes including the NUL) from a time. */
static void clock__time_text(const clock_time_t *t, char *out)
{
    out[0] = (char)('0' + (t->h / 10u));
    out[1] = (char)('0' + (t->h % 10u));
    out[2] = ':';
    out[3] = (char)('0' + (t->m / 10u));
    out[4] = (char)('0' + (t->m % 10u));
    out[5] = '\0';
}

/**
 * One glyph at a y offset. A one-character string is all the font API needs, and it
 * is what lets every slot move on its own -- `cgl_label` could only draw the whole
 * string at one position, which is why this is a widget of its own.
 */
static void clock__glyph(cgl_canvas_t *c, int16_t x, int16_t y, char ch, cgl_color_t col)
{
    char one[2];

    one[0] = ch;
    one[1] = '\0';
    (void)cgl_font_draw_text(c, CLOCK_BIG_FONT, x, y, one, col);
}

static void clock__draw_digits(cgl_widget_t *w, cgl_canvas_t *c)
{
    clock_digits_t *dw = (clock_digits_t *)w;
    const app_page_clock_t *p = dw->page;
    const clock_time_t *t;
    uint16_t roll;
    int16_t glyph_w;
    int16_t glyph_h;
    int16_t x;
    int16_t y;
    uint8_t i;

    if (p == NULL)
    {
        return;
    }
    t = app_page_clock_now(p);
    if (t == NULL)
    {
        return;
    }

    roll = (uint16_t)cgl_anim_value(&p->roll);
    glyph_w = (int16_t)CLOCK_BIG_FONT->width;
    glyph_h = (int16_t)CLOCK_BIG_FONT->height;

    x = (int16_t)(w->frame.x0 +
                  ((cgl_rect_width(w->frame) - (int16_t)(CLOCK_SLOTS * glyph_w)) / 2));
    y = (int16_t)(w->frame.y0 + ((cgl_rect_height(w->frame) - glyph_h) / 2));

    for (i = 0u; i < CLOCK_SLOTS; i++)
    {
        if (i == CLOCK_SLOT_COLON)
        {
            clock__glyph(c, x, y, ':', COL_TEXT);
        }
        else if (p->rolling && (p->roll_from[i] != p->roll_to[i]) && (roll < 256u))
        {
            /* The digit rolls: the old glyph leaves through the top of the cell while
               the new one arrives from below, both clipped to this widget's frame
               (CGL_WF_CLIP is on by default), which is what makes it read as a window
               in the face rather than as text sliding over the panel. */
            int16_t off = (int16_t)(((uint32_t)roll * (uint32_t)glyph_h) >> 8);

            clock__glyph(c, x, (int16_t)(y - off), (char)p->roll_from[i], COL_TEXT);
            clock__glyph(c, x, (int16_t)(y + glyph_h - off), (char)p->roll_to[i], COL_TEXT);
        }
        else
        {
            clock__glyph(c, x, y, (char)p->roll_to[i], COL_TEXT);
        }
        x = (int16_t)(x + glyph_w);
    }
}

static const cgl_widget_vt_t clock__digits_vt = {
    .type = "clock_digits",
    .draw = clock__draw_digits,
    .update = NULL,
    .event = NULL,
};

/* -------------------------------------------------------------------------- */
/* The gauge complication                                                     */
/* -------------------------------------------------------------------------- */

/**
 * The reading inside the seconds gauge: the one number the digital time line leaves
 * out, and the reason the face still has something counting when the dial is read at
 * a glance.
 *
 * The **ring** is not drawn here any more -- it is a `cgl_arc_gauge_t`, configured in
 * `clock__create()` and driven from `clock__frame()`, which is the framework's arc
 * toolkit doing the job this page used to hand-roll. What is left is the part the
 * gauge has no opinion about: two digits, but no leading zero, because "9" reads as a
 * watch complication and "09" as a stopwatch.
 */
static void clock__draw_gauge(cgl_widget_t *w, cgl_canvas_t *c)
{
    clock_gauge_t *gw = (clock_gauge_t *)w;
    const app_page_clock_t *p = gw->page;
    const clock_time_t *t;
    int16_t cx;
    int16_t cy;
    char num[3];
    int16_t tw;

    if (p == NULL)
    {
        return;
    }
    t = app_page_clock_now(p);
    if (t == NULL)
    {
        return;
    }

    cx = (int16_t)(w->frame.x0 + (cgl_rect_width(w->frame) / 2));
    cy = (int16_t)(w->frame.y0 + (cgl_rect_height(w->frame) / 2));

    if (t->s >= 10u)
    {
        num[0] = (char)('0' + (t->s / 10u));
        num[1] = (char)('0' + (t->s % 10u));
        num[2] = '\0';
    }
    else
    {
        num[0] = (char)('0' + t->s);
        num[1] = '\0';
        num[2] = '\0';
    }
    tw = cgl_font_text_width(CLOCK_BIG_FONT, num);
    (void)cgl_font_draw_text(c, CLOCK_BIG_FONT, (int16_t)(cx - (tw / 2)),
                             (int16_t)(cy - ((int16_t)CLOCK_BIG_FONT->height / 2)), num,
                             COL_TEXT);
}

static const cgl_widget_vt_t clock__gauge_vt = {
    .type = "clock_gauge",
    .draw = clock__draw_gauge,
    .update = NULL,
    .event = NULL,
};

/** The gauge's ring: the frame is the same 64 x 64 box as the reading's, so the two
    are centred on the same point, and the stroke width is the one the design has
    always used. */
#define CLOCK_GAUGE_W 5
#define CLOCK_GAUGE_BOX 64

static cgl_rect_t clock__gauge_frame(void)
{
    return cgl_rect_wh((int16_t)(CLOCK_COL_CX - (CLOCK_GAUGE_BOX / 2)),
                       (int16_t)(CLOCK_GAUGE_CY - (CLOCK_GAUGE_BOX / 2)), CLOCK_GAUGE_BOX,
                       CLOCK_GAUGE_BOX);
}

/* -------------------------------------------------------------------------- */
/* Page lifecycle                                                             */
/* -------------------------------------------------------------------------- */

/**
 * Invalidate a widget, unless the frame cannot have moved anything.
 *
 * A frame with `dt == 0` (the application's very first frame, see app_ui_build)
 * redraws an identical picture: skipping it saves a whole screen of SPI for no
 * visible difference. Anything the page has just *changed* is invalidated through
 * its own setter anyway (a label, or an explicit call in on_enter).
 */
static void clock__touch(cgl_widget_t *w, uint32_t dt_ms)
{
    if (dt_ms > 0u)
    {
        cgl_widget_invalidate(w);
    }
}

/**
 * Start a digit roll: record what is on screen, compute what should be, and run the
 * tween. Only the slots whose character really changed will move.
 */
static void clock__begin_roll(app_page_clock_t *p, const clock_time_t *t)
{
    char next[6];
    uint8_t i;

    clock__time_text(t, next);
    for (i = 0u; i < CLOCK_SLOTS; i++)
    {
        p->roll_from[i] = p->roll_to[i];
        p->roll_to[i] = (uint8_t)next[i];
    }
    p->rolling = true;
    cgl_anim_start(&p->roll, 0, 256, CLOCK_ROLL_MS, CGL_EASE_OUT_CUBIC);
}

/** Adopt a reading without animating: used on the first frame and after set_time. */
static void clock__prime(app_page_clock_t *p)
{
    char text[6];
    uint8_t i;

    clock__time_text(app_page_clock_now(p), text);
    for (i = 0u; i < CLOCK_SLOTS; i++)
    {
        p->roll_from[i] = (uint8_t)text[i];
        p->roll_to[i] = (uint8_t)text[i];
    }
    p->rolling = false;
}

/** Write the two text complications from the current date. */
static void clock__refresh_date(app_page_clock_t *p)
{
    const clock_time_t *t = app_page_clock_now(p);
    char buf[8];
    uint8_t mon;

    if (t == NULL)
    {
        return;
    }
    mon = (uint8_t)((t->month - 1u) % 12u);

    cgl_label_set_text(&p->wday, s_wday[t->wday]);
    if (t->day < 10u)
    {
        buf[0] = ' ';
        buf[1] = (char)('0' + t->day);
    }
    else
    {
        buf[0] = (char)('0' + (t->day / 10u));
        buf[1] = (char)('0' + (t->day % 10u));
    }
    buf[2] = ' ';
    buf[3] = s_month[mon][0];
    buf[4] = s_month[mon][1];
    buf[5] = s_month[mon][2];
    buf[6] = '\0';
    cgl_label_set_text(&p->date, buf);
}

static void clock__create(cgl_page_t *page)
{
    app_page_clock_t *p = (app_page_clock_t *)page;
    cgl_rect_t screen = cgl_rect(0, 0, (int16_t)(CGL_CFG_SCREEN_W - 1),
                                 (int16_t)(CGL_CFG_SCREEN_H - 1));
    const clock_time_t *t;
    char text[8];
    uint8_t mon;
    uint8_t i;

    cgl_widget_init(&p->root, &clock__group_vt, "clock.root", screen);
    p->root.flags |= CGL_WF_CLIP;
    cgl_page_set_root(page, &p->root);

    /* The dial is a `cgl_watch_t`: geometry, palette, surfaces, hands, chapter ring,
       crystal and the damage model all come from `s_cfg`. */
    cgl_watch_init(&p->watch, "clock.dial",
                   cgl_rect_wh(0, 0, CLOCK_DIAL_W, CGL_CFG_SCREEN_H), &s_cfg);

    cgl_widget_init(&p->digits.base, &clock__digits_vt, "clock.digits",
                    cgl_rect_wh((int16_t)(CLOCK_COL_CX - 40), (int16_t)CLOCK_TIME_Y, 80, 24));
    p->digits.page = p;

    /* The gauge: the framework's ring, then this page's reading on top of it. */
    cgl_arc_gauge_init(&p->gauge, "clock.ring", clock__gauge_frame(), CLOCK_GAUGE_R,
                       CLOCK_GAUGE_W);
    cgl_arc_gauge_set_span(&p->gauge, CGL_ANGLE_UP, CGL_ANGLE_TURN, CGL_ARC_CW);
    cgl_arc_gauge_set_colors(&p->gauge, COL_TRACK, COL_ACCENT);
    cgl_arc_gauge_set_pulse_color(&p->gauge, COL_ACCENT_HI);

    cgl_widget_init(&p->gauge_num.base, &clock__gauge_vt, "clock.gauge", clock__gauge_frame());
    p->gauge_num.page = p;

    cgl_label_init(&p->wday, "clock.wday",
                   cgl_rect_wh((int16_t)(CLOCK_COL_CX - 70), (int16_t)CLOCK_WDAY_Y, 140, 16),
                   CLOCK_FONT, s_wday[CLOCK_START_WDAY], COL_DIM, COL_BG, false);
    cgl_label_set_align(&p->wday, CGL_ALIGN_CENTER);

    /* "14 JAN" -- built here rather than with printf: neither the framework nor this
       application links a printf, and a two digit number does not need one. */
    mon = (uint8_t)(CLOCK_START_MON - 1u);
    text[0] = (char)('0' + (CLOCK_START_D / 10u));
    text[1] = (char)('0' + (CLOCK_START_D % 10u));
    text[2] = ' ';
    text[3] = s_month[mon][0];
    text[4] = s_month[mon][1];
    text[5] = s_month[mon][2];
    text[6] = '\0';
    cgl_label_init(&p->date, "clock.date",
                   cgl_rect_wh((int16_t)(CLOCK_COL_CX - 70), (int16_t)CLOCK_DATE_Y, 140, 16),
                   CLOCK_FONT, text, COL_TEXT, COL_BG, false);
    cgl_label_set_align(&p->date, CGL_ALIGN_CENTER);

    (void)cgl_widget_add_child(&p->root, &p->watch.base);
    (void)cgl_widget_add_child(&p->root, &p->digits.base);
    (void)cgl_widget_add_child(&p->root, &p->gauge.base);
    (void)cgl_widget_add_child(&p->root, &p->gauge_num.base);
    (void)cgl_widget_add_child(&p->root, &p->wday.base);
    (void)cgl_widget_add_child(&p->root, &p->date.base);

    /* `cgl_watch_init()` deliberately does **not** clear a parked `set_pending`: a
       caller may set the time before the page has ever been created -- "the RTC was
       read at boot, show it when the face first comes up" -- and an init that wiped
       the request would silently start the watch at CLOCK_START_* instead. So a page
       that was created *after* a set_time adopts it on its first frame, through
       `p->reprime` below. */
    if (p->watch.set_pending)
    {
        p->reprime = true;
    }

    t = app_page_clock_now(p);
    if (t != NULL)
    {
        clock__prime(p);
        clock__refresh_date(p);
    }

    cgl_anim_init(&p->roll);

    for (i = 0u; i < CLOCK_SLOTS; i++)
    {
        p->roll_from[i] = 0u;
        p->roll_to[i] = 0u;
    }
    p->rolling = false;

    /* 0xFF = "no reading yet": the first frame refreshes the date line and primes the
       roll instead of rolling, and it never fires a phantom minute pulse. */
    p->prev_second = 0xFFu;
    p->prev_minute = 0xFFu;
    p->prev_day = 0xFFu;
}

static void clock__enter(cgl_page_t *page, cgl_page_t *from, uint8_t kind)
{
    app_page_clock_t *p = (app_page_clock_t *)page;
    (void)from;
    (void)kind;

    /* Every entry replays the reveal: the face sweeps up from 12:00 to the current
       time, which is the one animation that makes arriving at a watch face feel like
       waking a watch up. The time itself is **not** reset -- it comes from the
       framework clock, so it kept running while the page was covered. */
    cgl_watch_reveal(&p->watch);
    cgl_widget_invalidate(&p->root);
}

/**
 * One fixed logic step. **This page has nothing to step**: the time is a function of
 * the global frame clock, not an accumulator (see the file header of
 * app_page_clock.h). The callback exists anyway, because its counter is the
 * observation point the info page uses to show that a covered page stops stepping --
 * it has to behave exactly like the other pages' counter for that comparison to mean
 * anything.
 */
static void clock__step(cgl_page_t *page, uint32_t step_ms)
{
    app_page_clock_t *p = (app_page_clock_t *)page;
    (void)step_ms;
    p->steps++;
}

/** A point at `radius` and `a` around an arbitrary centre (the gauge is not centred
    on the dial). */
static void clock__polar2(int16_t cx, int16_t cy, int16_t radius, cgl_angle_t a, int16_t *x,
                          int16_t *y)
{
    int16_t dx;
    int16_t dy;

    cgl_math_polar(a, radius, &dx, &dy);
    *x = (int16_t)(cx + dx);
    *y = (int16_t)(cy + dy);
}

static void clock__frame(cgl_page_t *page, const cgl_clock_t *clk)
{
    app_page_clock_t *p = (app_page_clock_t *)page;
    const clock_time_t *t;
    bool second_changed;
    bool minute_changed;
    bool day_changed;

    if (clk == NULL)
    {
        return; /* no clock, no time: there is nothing this page could draw */
    }
    p->frames++;

    /* --- the dial ---------------------------------------------------------
     * One call does the whole of what this page used to do by hand: adopt a parked
     * time set, derive `now` from the frame clock, detect the minute and the day,
     * run the entrance and the minute flash, age the comet trail, and mark the
     * damage the *dial* changed. It has to run first: everything below reads the
     * reading it just adopted.
     * -------------------------------------------------------------------- */
    cgl_watch_frame(&p->watch, clk);

    t = app_page_clock_now(p);
    if (t == NULL)
    {
        return;
    }

    /* WARNING: a time set from outside re-anchors the **page's** own edge detection
       as well as the dial's. The dial resets its own `prev_*` inside
       `cgl_watch_frame()`; these four are the page's, and the page is what knows
       whether a set is pending -- hence `p->reprime`, which the setters raise and
       this is the only place that lowers it. Without it the first frame after a
       set_time() would fire a phantom digit roll (the old reading against the new
       one) instead of priming, which is a visible glitch on the panel. */
    if (p->reprime)
    {
        p->prev_second = 0xFFu;
        p->prev_minute = 0xFFu;
        p->prev_day = 0xFFu;
        p->primed = false;
        p->reprime = false;
    }

    second_changed = (t->s != p->prev_second);
    minute_changed = (t->m != p->prev_minute);
    day_changed = (t->day != p->prev_day);

    if (!p->primed)
    {
        clock__prime(p);
        clock__refresh_date(p);
        p->primed = true;
    }
    else
    {
        if (minute_changed)
        {
            clock__begin_roll(p, t);
        }
        if (day_changed)
        {
            clock__refresh_date(p);
        }
    }

    /* --- the two complications -------------------------------------------
     * The gauge **steps once per second** rather than sweeping: its ring, its
     * reading and its lead end all change at the second boundary and nothing in
     * between. That is a deliberate trade. A smoothly sweeping complication ring
     * damages a 64 x 64 box every frame, and the panel's damage model is **one**
     * rectangle, so that box would union with the dial's into a band range covering
     * the whole screen (measured on the reference board: 39 bands pushed per frame
     * instead of 22, and the pushed bands are what the SPI budget is made of). A
     * ticking complication is also what a watch does; the three dial hands keep
     * sweeping, because the *dial* knows its own damage down to the pixel.
     * -------------------------------------------------------------------- */
    cgl_arc_gauge_set_sweep(&p->gauge, clock__minute_sweep(t));
    cgl_arc_gauge_set_scale(&p->gauge, cgl_watch_reveal_value(&p->watch));
    cgl_arc_gauge_set_pulse(&p->gauge, cgl_watch_pulse_value(&p->watch));

    if (second_changed)
    {
        cgl_rect_t g = p->gauge.base.frame;
        int16_t cx = (int16_t)(g.x0 + (cgl_rect_width(g) / 2));
        int16_t cy = (int16_t)(g.y0 + (cgl_rect_height(g) / 2));
        int16_t ox;
        int16_t oy;

        /* The lead end is placed from the sweep the gauge is drawing, exactly as the
           design draws it -- by `CGL_ANGLE_UP` plus the minute's own angle, and
           *without* the reveal's scale, because a frame inside the entrance already
           damages the whole page. */
        clock__polar2(cx, cy, CLOCK_GAUGE_R,
                      (cgl_angle_t)(CGL_ANGLE_UP + (cgl_angle_t)clock__minute_sweep(t)), &ox, &oy);
        cgl_widget_invalidate_rect(cgl_rect_wh((int16_t)(ox - 5), (int16_t)(oy - 5), 11, 11));
        cgl_widget_invalidate_rect(cgl_rect_wh((int16_t)(cx - 20), (int16_t)(cy - 14), 41, 29));
    }

    /* --- the page's own tweens -------------------------------------------- */
    if (cgl_anim_tick(&p->roll, clk->dt_ms))
    {
        clock__touch(&p->digits.base, clk->dt_ms);
        if (cgl_anim_done(&p->roll))
        {
            p->rolling = false;
        }
    }

    p->prev_second = t->s;
    p->prev_minute = t->m;
    p->prev_day = t->day;
}

static bool clock__event(cgl_page_t *page, const cgl_event_t *ev)
{
    app_page_clock_t *p = (app_page_clock_t *)page;

    switch (ev->kind)
    {
    case CGL_EV_KEY_UP:
        /* A short press replays the entrance sweep. It is the only thing this page
           can be asked to do -- it has no state to change -- and it makes the reveal
           inspectable without leaving and re-entering the page. */
        cgl_watch_reveal(&p->watch);
        cgl_widget_invalidate(&p->root);
        return true;

    case CGL_EV_KEY_DOWN:
    case CGL_EV_KEY_REPEAT:
        return true; /* eat them, so nothing leaks to a page below */

    default:
        /* KEY_LONG and BACK are deliberately **not** consumed: the navigator turns
           them into "pop the stack", which is how every page here is left. */
        return false;
    }
}

static const cgl_page_vt_t clock__page_vt = {
    .type = "page_clock",
    .on_create = clock__create,
    .on_destroy = NULL, /* every widget is a static member; nothing to free */
    .on_enter = clock__enter,
    .on_exit = NULL,
    .on_pause = NULL,
    .on_resume = NULL,
    .on_step = clock__step,
    .on_frame = clock__frame,
    .on_event = clock__event,
};

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void app_page_clock_attach(cgl_page_t *page, const cgl_page_ctx_t *ctx)
{
    app_page_clock_t *p = (app_page_clock_t *)page;

    p->links = ((ctx != NULL) ? (app_links_t *)ctx->app : NULL);
    p->screen = ((ctx != NULL) ? ctx->screen : NULL);

    p->frames = 0u;
    p->steps = 0u;
    p->primed = false;
    p->reprime = false;
    p->rolling = false;
    p->prev_second = 0xFFu;
    p->prev_minute = 0xFFu;
    p->prev_day = 0xFFu;

    cgl_page_init(page, &clock__page_vt, "clock", (void *)ctx);

    /* S_IN / S_OUT: the face is sucked in and poured back out along the S path, so
       the page declares its own transition and the menu only has to pass
       CGL_TRANS_AUTO (see menu__activate). */
    cgl_page_set_trans_enter(&p->base, CGL_TRANS_S_IN, CGL_EASE_OUT_QUAD, 560u);
    cgl_page_set_trans_leave(&p->base, CGL_TRANS_S_OUT, CGL_EASE_OUT_QUAD, 560u);
}

void app_page_clock_set_time(app_page_clock_t *p, uint8_t h, uint8_t m, uint8_t s)
{
    if (p == NULL)
    {
        return;
    }

    cgl_watch_set_time(&p->watch, h, m, s);

    /* The page's own edge detection has to re-adopt the reading too -- see the
       warning in clock__frame. Raised here and lowered there, in one place. */
    p->reprime = true;

    if (cgl_page_is_created(&p->base))
    {
        cgl_widget_invalidate(cgl_page_root(&p->base));
    }
}

void app_page_clock_set_date(app_page_clock_t *p, uint16_t year, uint8_t month, uint8_t day,
                             uint8_t wday)
{
    if (p == NULL)
    {
        return;
    }

    cgl_watch_set_date(&p->watch, year, month, day, wday);
    p->reprime = true;

    if (cgl_page_is_created(&p->base))
    {
        cgl_widget_invalidate(cgl_page_root(&p->base));
    }
}

const clock_time_t *app_page_clock_now(const app_page_clock_t *p)
{
    if (p == NULL)
    {
        return NULL;
    }
    /* A sane reading before the first frame: a page that is registered but never
       pushed still answers with something drawable, which is what the configured
       start reading is. Once a frame has run, the dial's derived reading is the
       answer -- and it is the *only* one, so nothing can disagree with it. */
    if (p->watch.frames == 0u)
    {
        return &s_cfg.start;
    }
    return cgl_watch_now(&p->watch);
}

cgl_color_t app_page_clock_face_color(int16_t dx, int16_t dy)
{
    /* The table it reads is in flash, built at compile time: there is nothing to set
       up and nothing that can be "not ready yet", so this is answerable from the
       moment the page exists -- before its first frame, before its first draw. */
    return cgl_arc_surface_color(&s_cfg.style.face, dx, dy);
}

cgl_color_t app_page_clock_bg_color(int16_t x, int16_t y)
{
    return clock__bg_px(x, y);
}

cgl_color_t app_page_clock_ring_color(int16_t dx, int16_t dy)
{
    return cgl_arc_surface_color(&s_cfg.style.well, dx, dy);
}

cgl_color_t app_page_clock_glass_over(cgl_color_t under, int16_t dx, int16_t dy)
{
    return cgl_watch_glass_shade(&s_cfg.style.glass, under, dx, dy);
}

uint32_t app_page_clock_seconds_of_day(const app_page_clock_t *p)
{
    const clock_time_t *t = app_page_clock_now(p);

    if (t == NULL)
    {
        return 0u;
    }
    return ((uint32_t)t->h * 3600u) + ((uint32_t)t->m * 60u) + (uint32_t)t->s;
}

uint16_t app_page_clock_reveal(const app_page_clock_t *p)
{
    return (p != NULL) ? cgl_watch_reveal_value(&p->watch) : 0u;
}

cgl_angle_t app_page_clock_second_angle(uint8_t s, uint16_t ms)
{
    clock_time_t t;

    /* The arithmetic is `clock__second_angle()`, which lives in the **header** with
       the switch it obeys -- see "the second hand's motion" in app_page_clock.h for
       why it cannot live here. Only these two fields of a `clock_time_t` are read,
       and the page keeps no state that could make the answer depend on anything
       else, which is the property this accessor exists to expose. */
    t.s = (uint8_t)(s % 60u);
    t.ms = (uint16_t)(ms % (uint16_t)CGL_CLOCK_MS_PER_S);
    return clock__second_angle(&t);
}

uint32_t app_page_clock_frames(const app_page_clock_t *p)
{
    return (p != NULL) ? p->frames : 0u;
}

uint32_t app_page_clock_steps(const app_page_clock_t *p)
{
    return (p != NULL) ? p->steps : 0u;
}




