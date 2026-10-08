// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  * @file    alpha_host_test.c
  * @brief   Specification test for group opacity (save-under layered compositing) -- per pixel.
  *
  *  Every check corresponds to a "what breaks if this is wrong":
  *
  *    A. default value      alpha never set = CGL_ALPHA_OPAQUE, the image is bit-identical to before
  *    B. formula check      pins the blend values hand-computed in the comments (truncation, not rounding)
  *    C. really works       a pixel inside the group = blend(background, group content, alpha)
  *    D. * no double blend  two **overlapping opaque** child widgets inside the group: the
  *                          overlap must equal the result of "only the top one". Multiplying
  *                          alpha per primitive would paint it fully opaque here -- this
  *                          assertion is what tells the two implementations apart
  *    E. no edge leak       pixels outside the group are bit-identical to the background; the
  *                          group spans 10 bands, so band and stride both have to be right
  *    F. no scratch left    restoring 255 after a translucent render must be bit-identical to the
  *                          baseline (catches "forgot to swap c->buf back")
  *    G. recursive nesting  an outer 128 around an inner 128 = two levels of isolated compositing,
  *                          the value is exactly predictable
  *    H. warp path          with warp p=0 (an exact identity) the result matches the unwarped case
  *    I. levels exhausted   once CGL_CFG_ALPHA_LEVELS are used up, alpha is ignored and counted,
  *                          and nothing crashes
  *
  *  Build / run: the canonical definition is tests/CMakeLists.txt -- this test
  *  links only the framework sources, no application code:
  *
  *    cmake -B build/host -DCYTGL_BUILD_TESTS=ON
  *    cmake --build build/host
  *    ctest --test-dir build/host -R alpha --output-on-failure
  */
#include <stdio.h>
#include <string.h>

#include <cytgl/cytgl_config.h>
#include <cytgl/cgl_types.h>
#include <cytgl/cgl_canvas.h>
#include <cytgl/cgl_widget.h>
#include <cytgl/cgl_panel.h>
#include <cytgl/cgl_display.h>

#define SW CGL_CFG_SCREEN_W
#define SH CGL_CFG_SCREEN_H

/* The three colours deliberately keep the channels apart, so the "blend result" differs from both endpoints*/
#define COL_BD CGL_RGB565(0, 0, 255)   /* background: pure blue 0x001F*/
#define COL_A  CGL_RGB565(255, 0, 0)   /* child widget A: pure red 0xF800*/
#define COL_B  CGL_RGB565(0, 255, 0)   /* child widget B: pure green 0x07E0*/

/* the group rectangle: 40 wide x 36 high, from y=5 to y=40, spanning 10 bands (BAND_H=4)*/
#define GRP_X0 10
#define GRP_Y0 5
#define GRP_X1 49
#define GRP_Y1 40

static int s_fails = 0;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            s_fails++;                                          \
            (void)printf("  FAIL: ");                           \
            (void)printf(__VA_ARGS__);                          \
            (void)printf("   [%s:%d]\n", __FILE__, __LINE__);   \
        }                                                       \
    } while (0)

/* ------------------------------------------------------------------------------- */
/* mock panel: assemble the bands into whole frames, then read back pixel by pixel */
/* ------------------------------------------------------------------------------- */

static cgl_color_t s_fb[SW * SH];
static uint16_t s_row;
static uint16_t s_col;

static int mock__init(void *ctx) { (void)ctx; return 0; }

static void mock__set_window(void *ctx, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    (void)ctx; (void)x1; (void)y1;
    s_col = x0;
    s_row = y0;
}

static void mock__write_pixels(void *ctx, const cgl_color_t *px, uint32_t count)
{
    uint32_t i;
    (void)ctx;
    for (i = 0u; i < count; i++)
    {
        if ((s_row < (uint16_t)SH) && (s_col < (uint16_t)SW))
        {
            s_fb[((uint32_t)s_row * SW) + s_col] = px[i];
        }
        s_col++;
        if (s_col >= (uint16_t)SW) { s_col = 0u; s_row++; }
    }
}

static void mock__wait_idle(void *ctx) { (void)ctx; }
static void mock__backlight(void *ctx, uint8_t p) { (void)ctx; (void)p; }

static const cgl_display_drv_t s_drv = {
    "mock", mock__init, mock__set_window, mock__write_pixels,
    mock__wait_idle, mock__backlight, SW, SH,
};

/* -------------------------------------------------------------------------------------------------------- */
/* widgets: root(background) -> group(a container that draws nothing itself) -> several solid colour blocks */
/* -------------------------------------------------------------------------------------------------------- */

static cgl_display_t s_disp;
static cgl_panel_t s_root;
static cgl_widget_t s_group;
static cgl_widget_t s_inner;

typedef struct
{
    cgl_widget_t base;
    cgl_color_t col;
} fill_t;

static void fill__draw(cgl_widget_t *w, cgl_canvas_t *c)
{
    fill_t *f = (fill_t *)w;
    cgl_canvas_fill_rect(c, w->frame, f->col);
}

static const cgl_widget_vt_t fill__vt = {
    .type = "test_fill", .draw = fill__draw, .update = NULL, .event = NULL,
};

static void fill_init(fill_t *f, const char *name, cgl_rect_t r, cgl_color_t col)
{
    cgl_widget_init(&f->base, &fill__vt, name, r);
    f->col = col;
}

/* container: draw = NULL. Only with this can the group's own isolation be checked*/
static const cgl_widget_vt_t group__vt = {
    .type = "test_group", .draw = NULL, .update = NULL, .event = NULL,
};

static fill_t s_a;
static fill_t s_b;
static fill_t s_full;

static void damage__sink(cgl_rect_t r, void *user)
{
    (void)user;
    cgl_display_damage(&s_disp, r);
}

static void render_root(cgl_canvas_t *c, void *user)
{
    (void)user;
    cgl_widget_draw_tree(&s_root.base, c);
}

static void render_all(void)
{
    cgl_display_damage_all(&s_disp);
    (void)cgl_display_present(&s_disp, render_root, NULL);
}

static cgl_color_t px(int16_t x, int16_t y)
{
    return s_fb[((uint32_t)y * SW) + (uint32_t)x];
}

/* -------------------------------------------------------------------------- */

int main(void)
{
    static cgl_color_t ref[SW * SH];
    static cgl_warp_t warp;
    cgl_color_t expect_a;
    cgl_color_t expect_b;
    int16_t y;

    (void)printf("=== group opacity: save-under layered compositing (per pixel) ===\n");
    (void)printf("screen %dx%d  band height %d  levels %u  scratch %u B\n\n",
                 SW, SH, CGL_CFG_BAND_H, (unsigned)CGL_CFG_ALPHA_LEVELS,
                 (unsigned)CGL_CFG_ALPHA_BYTES);

#if !CGL_CFG_ALPHA
    (void)printf("CGL_CFG_ALPHA = 0: the display layer provides no scratch, this test does not apply (skipped).\n");
    return 0;
#else

    if (cgl_display_init(&s_disp, &s_drv, NULL) != 0) { return 1; }
    cgl_widget_set_damage_sink(damage__sink, &s_disp);

    cgl_panel_init(&s_root, "root", cgl_rect(0, 0, SW - 1, SH - 1), COL_BD, COL_BD);
    cgl_widget_init(&s_group, &group__vt, "group",
                   cgl_rect(GRP_X0, GRP_Y0, GRP_X1, GRP_Y1));
    cgl_widget_add_child(&s_root.base, &s_group);

    /* --- A) default value + baseline --------------------------------- */
    (void)printf("== A) Default value and baseline ==\n");
    CHECK(cgl_widget_get_alpha(&s_group) == (uint8_t)CGL_ALPHA_OPAQUE,
          "a freshly created widget's alpha should be CGL_ALPHA_OPAQUE, actually %u",
          (unsigned)cgl_widget_get_alpha(&s_group));

    fill_init(&s_a, "A", cgl_rect(GRP_X0, GRP_Y0, GRP_X1, GRP_Y1), COL_A);
    fill_init(&s_b, "B", cgl_rect(30, GRP_Y0, GRP_X1, GRP_Y1), COL_B);
    cgl_widget_add_child(&s_group, &s_a.base);   /* A first*/
    cgl_widget_add_child(&s_group, &s_b.base);   /* then B -> B is on top*/

    render_all();
    memcpy(ref, s_fb, sizeof(ref));
    CHECK(px(20, 20) == COL_A, "baseline: the group's left half should be A's solid colour, actually 0x%04X", px(20, 20));
    CHECK(px(40, 20) == COL_B, "baseline: the overlap should be the topmost B, actually 0x%04X", px(40, 20));
    CHECK(px(200, 100) == COL_BD, "baseline: outside the group should be the background, actually 0x%04X", px(200, 100));
    (void)printf("   group left 0x%04X  overlap 0x%04X  outside 0x%04X\n", px(20, 20), px(40, 20), px(200, 100));

    /* Setting 255 explicitly must be bit-identical to "never set"*/
    cgl_widget_set_alpha(&s_group, (uint8_t)CGL_ALPHA_OPAQUE);
    render_all();
    CHECK(memcmp(ref, s_fb, sizeof(ref)) == 0,
          "alpha = CGL_ALPHA_OPAQUE must be bit-identical to the default");

    /* --- B) formula check --------------------------------------------- */
    (void)printf("\n== B) Blend formula (Q8 truncation) ==\n");
    (void)printf("   blend(0x%04X,0x%04X,128) = 0x%04X\n", COL_BD, COL_A,
                 cgl_color_blend_q8(COL_BD, COL_A, 128u));
    (void)printf("   blend(0x%04X,0x%04X,128) = 0x%04X\n", COL_BD, COL_B,
                 cgl_color_blend_q8(COL_BD, COL_B, 128u));
    CHECK(cgl_color_blend_q8(COL_BD, COL_A, 128u) == 0x780Fu,
          "blend(blue,red,128) should be 0x780F");
    CHECK(cgl_color_blend_q8(COL_BD, COL_B, 128u) == 0x03EFu,
          "blend(blue,green,128) should be 0x03EF");
    CHECK(cgl_color_blend_q8(COL_BD, COL_A, 256u) == COL_A, "alpha=256 must be exactly the source");
    CHECK(cgl_color_blend_q8(COL_BD, COL_A, 0u) == COL_BD, "alpha=0 must be exactly the destination");
    expect_a = cgl_color_blend_q8(COL_BD, COL_A, 128u);
    expect_b = cgl_color_blend_q8(COL_BD, COL_B, 128u);

    /* --- C/D/E) translucent: blend value / no double blend / edges --------*/
    (void)printf("\n== C/D/E) alpha = 128 ==\n");
    cgl_widget_set_alpha(&s_group, 128u);
    render_all();
    (void)printf("   group left 0x%04X (expect 0x%04X)  overlap 0x%04X (expect 0x%04X)\n",
                 px(20, 20), expect_a, px(40, 20), expect_b);

    CHECK(px(20, 20) == expect_a,
          "group left should be blend(background,A,128)=0x%04X, actually 0x%04X", expect_a, px(20, 20));
    CHECK(px(20, 20) != COL_A, "after the translucent pass it must not still be A's solid colour");

    /* * This is the key case: the overlap must be "composited" exactly once*/
    CHECK(px(40, 20) == expect_b,
          "the overlap should be blend(background,B,128)=0x%04X, actually 0x%04X", expect_b, px(40, 20));
    CHECK(cgl_color_blend_q8(cgl_color_blend_q8(COL_BD, COL_A, 128u), COL_B, 128u) != expect_b,
          "the counter-example has gone stale: the per-primitive version computes the same value, so this test would catch no regression");

    CHECK(px(200, 100) == COL_BD, "pixels outside the group must stay bit-identical to the background");
    CHECK(px(GRP_X0 - 1, 20) == COL_BD, "just outside the group's left edge must stay bit-identical to the background");
    CHECK(px(GRP_X1 + 1, 20) == COL_BD, "just outside the group's right edge must stay bit-identical to the background");
    CHECK(px(20, GRP_Y0 - 1) == COL_BD, "just outside the group's top edge must stay bit-identical to the background");
    CHECK(px(20, GRP_Y1 + 1) == COL_BD, "just outside the group's bottom edge must stay bit-identical to the background");

    /* Across bands: the group's 36 rows are spread over 10 bands, every row has to be right*/
    {
        int bad_rows = 0;
        for (y = GRP_Y0; y <= GRP_Y1; y++)
        {
            if (px(20, y) != expect_a) { bad_rows++; }
        }
        (void)printf("   across bands: %d of %d rows are wrong\n", (int)(GRP_Y1 - GRP_Y0 + 1), bad_rows);
        CHECK(bad_rows == 0, "%d rows inside the group have wrong pixels (band / stride arithmetic is off)", bad_rows);
    }

    /* --- F) restoring 255 must return bit-identically to the baseline -------*/
    (void)printf("\n== F) After restoring 255 everything is bit-identical to the baseline ==\n");
    cgl_widget_set_alpha(&s_group, (uint8_t)CGL_ALPHA_OPAQUE);
    render_all();
    CHECK(memcmp(ref, s_fb, sizeof(ref)) == 0,
          "restoring 255 after a translucent render must be bit-identical to the baseline (scratch residue / c->buf not swapped back)");
    (void)printf("   %s\n", (memcmp(ref, s_fb, sizeof(ref)) == 0) ? "identical" : "different");

    /* --- G) recursive nesting ----------------------------------------- */
    cgl_widget_remove_child(&s_a.base);
    cgl_widget_remove_child(&s_b.base);
    cgl_widget_init(&s_inner, &group__vt, "inner",
                   cgl_rect(GRP_X0, GRP_Y0, GRP_X1, GRP_Y1));
    cgl_widget_add_child(&s_group, &s_inner);
    fill_init(&s_full, "full", cgl_rect(GRP_X0, GRP_Y0, GRP_X1, GRP_Y1), COL_A);
    cgl_widget_add_child(&s_inner, &s_full.base);

#if CGL_CFG_ALPHA_LEVELS >= 2
    (void)printf("\n== G) Two nested levels (128 each) ==\n");
    cgl_widget_set_alpha(&s_group, 128u);
    cgl_widget_set_alpha(&s_inner, 128u);
    render_all();
    {
        /* two levels of isolated compositing = blend(background, blend(background, A, 128), 128)*/
        cgl_color_t want = cgl_color_blend_q8(COL_BD,
                                            cgl_color_blend_q8(COL_BD, COL_A, 128u), 128u);
        (void)printf("   inside the group 0x%04X (hand-computed 0x3817 / formula 0x%04X)\n", px(20, 20), want);
        CHECK(want == 0x3817u, "the formula value for two 128 levels should be 0x3817 (truncation, not rounding)");
        CHECK(px(20, 20) == want,
              "a pixel inside the two nested levels should be 0x%04X, actually 0x%04X", want, px(20, 20));
        CHECK(px(200, 100) == COL_BD, "after nesting, pixels outside the group must still be bit-identical to the background");
    }
#else
    (void)printf("\n== G) levels %u < 2, skipping the nesting case ==\n", (unsigned)CGL_CFG_ALPHA_LEVELS);
    cgl_widget_set_alpha(&s_inner, 128u);
#endif

    /* --- H) warp path: p = 0 is an exact identity --------------- */
    (void)printf("\n== H) warp p=0 (identity) ==\n");
    {
        cgl_color_t before_warp = px(20, 20);

        warp.src_y0 = 0;
        warp.src_h = SH;
        warp.cx = (int16_t)(SW / 2);
        warp.tx = (int16_t)(SW / 2);
        warp.ty = SH;
        warp.progress_q8 = 0u;
        warp.narrow_q8 = 64u;
        warp.s_amp_q8 = 0u; /* This exercises the "suck in vertically" path: the horizontal S trajectory is switched off*/
        warp.row_step_q8 = (uint16_t)((((uint32_t)256u * 256u) + ((uint32_t)SH / 2u)) /
                                      (uint32_t)SH);
        cgl_widget_set_warp(&s_group, &warp);
        render_all();
        (void)printf("   without warp 0x%04X  with warp 0x%04X\n", before_warp, px(20, 20));
        CHECK(px(20, 20) == before_warp, "warp p=0 is the identity, so the group opacity result must match");
        CHECK(px(200, 100) == COL_BD, "with the warp on, pixels outside the group must still be bit-identical to the background");
        cgl_widget_set_warp(&s_group, NULL);
    }

    /* --- I) full-screen bit scan: only the group interior should change ---------*/
    (void)printf("\n== I) Full-screen bit scan (alpha 255 -> 96, not one bit outside the group may move) ==\n");
    cgl_widget_set_alpha(&s_inner, (uint8_t)CGL_ALPHA_OPAQUE);
    cgl_widget_set_alpha(&s_group, (uint8_t)CGL_ALPHA_OPAQUE);
    render_all();
    memcpy(ref, s_fb, sizeof(ref));
    cgl_widget_set_alpha(&s_group, 96u);
    render_all();
    {
        int bad = 0;
        int16_t x;
        for (y = 0; y < SH; y++)
        {
            for (x = 0; x < SW; x++)
            {
                bool in = (x >= GRP_X0) && (x <= GRP_X1) && (y >= GRP_Y0) && (y <= GRP_Y1);
                if (!in && (px(x, y) != ref[((uint32_t)y * SW) + (uint32_t)x]))
                {
                    bad++;
                }
            }
        }
        (void)printf("   pixels changed outside the group = %d (must be 0)\n", bad);
        CHECK(bad == 0, "%d pixels outside the group were changed -- the isolated region is computed too large or not clipped cleanly", bad);
        CHECK(px(20, 20) != ref[(20u * SW) + 20u], "nothing changed inside the group, so alpha had no effect at all");
    }

    /* --- J) levels exhausted: alpha is ignored and counted ----- */
#if CGL_CFG_ALPHA_LEVELS == 2
    (void)printf("\n== J) Nesting a third level (only %u levels available) ==\n", (unsigned)CGL_CFG_ALPHA_LEVELS);
    {
        cgl_widget_t deepest;
        uint32_t before = cgl_widget_alpha_dropped();

        cgl_widget_init(&deepest, &group__vt, "deepest",
                       cgl_rect(GRP_X0, GRP_Y0, GRP_X1, GRP_Y1));
        cgl_widget_add_child(&s_inner, &deepest);
        cgl_widget_set_alpha(&deepest, 128u);
        /* All three levels set: group takes level 1 and inner takes level 2, so deepest has nowhere left*/
        cgl_widget_set_alpha(&s_inner, 128u);
        cgl_widget_set_alpha(&s_group, 128u);
        render_all();
        (void)printf("   alpha was ignored %u times (counted per band per pass, so it is not a widget count)\n",
                     (unsigned)(cgl_widget_alpha_dropped() - before));
        CHECK(cgl_widget_alpha_dropped() > before,
              "the third nesting level must be counted as having no effect, not silently painted opaque");
        CHECK(px(200, 100) == COL_BD, "running out of levels must not corrupt the image");
    }
#else
    (void)printf("\n== J) levels %u != 2, skipping the exhausted case ==\n",
                 (unsigned)CGL_CFG_ALPHA_LEVELS);
#endif

    (void)printf("\n%s (%d checks failed)\n", s_fails ? "with failures" : "all passed", s_fails);
    return (s_fails != 0) ? 1 : 0;
#endif /* CGL_CFG_ALPHA */
}
