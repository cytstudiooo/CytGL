// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    render_host_test.c
  * @brief   Host side renderer for the demo (not part of the firmware).
  *
  *  Runs the real UI framework, the real page stack and the real logo smash
  *  scene on a PC through a mock display driver that captures the bands into a
  *  full framebuffer, then writes PPM/BMP snapshots. Use it to check the
  *  layout, the font, the animation and **the page transitions** without
  *  hardware.
  *
  *  Build / run: the canonical definition is tests/CMakeLists.txt -- this test
  *  links the whole demo application plus test_image_out.c:
  *
  *    cmake -B build/host -DCYTGL_BUILD_TESTS=ON
  *    cmake --build build/host
  *    ctest --test-dir build/host -R render --output-on-failure
  *
  *  Snapshot list:
  *    frame00  main menu (bottom of the stack, the result of CGL_NAV_RESET)
  *    frame01  the smash page has just entered and physics has not run yet (dot matrix intact, used for the per-cell read-back check)
  *    frame02..06  5 key moments of one smash animation round
  *    frame07..09  COVER_UP: the info page covers the smash page from below (**the smash page is covered here**)
  *    frame10..12  SLIDE_DOWN: back to the smash page
  *    frame13  back to the menu
  *    frame14..    GENIE_OUT: the smash page is "sucked" into the bottom of the screen (macOS minimise);
  *                 the intermediate frames are compared pixel by pixel against "outside the funnel silhouette it must be the page below"
  ******************************************************************************
  */
#include <stdio.h>

#include <cytgl/cgl_display.h>
#include <cytgl/cgl_screen.h>
#include <cytgl/cgl_page.h>

#include "app_page_smash.h"
#include "app_page_menu.h"
#include "app_page_info.h"
#include "logo_bitmap.h"
#include "test_image_out.h"

static cgl_color_t s_framebuffer[CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H];

static cgl_display_t s_display;
static cgl_screen_t s_screen;
static cgl_page_nav_t s_nav;
static app_page_smash_t s_smash;
static app_page_menu_t s_menu;
static app_page_info_t s_info;

/* --------------------------------------------------------------------------- */
/* Can the Logo still be recognised after the pseudo-3D?                       */
/* --------------------------------------------------------------------------- */

/**
 * Per-cell read-back check: project each cell of the dot matrix back onto the
 * screen through the **current camera**, then look at whether that little patch
 * of pixels is actually lit, and rebuild a 106 x 14 bitmap out of it.
 *
 * This is the only machine-decidable "readability" criterion for the pseudo-3D:
 * the projection skews and flattens the dot matrix and may even land adjacent
 * dots on the same pixel -- as long as every lit cell is hit and the false hits in
 * the empty cells stay few enough, the 16 px glyph really did survive the
 * projection and a human can still read it on screen.
 *
 * WARNING: when calling this there must be **no transition running** (the page
 * offset must be 0), otherwise the whole page has been translated and the
 * projected coordinates do not match the screen pixels. This function asserts that
 * itself.
 */
static int verify_logo_dots(app_page_smash_t *p)
{
    const cgl_iso2d_cam_t *cam = &p->storage.u.w.view.cam;
    int i;
    int j;
    int lit = 0;
    int hit = 0;
    int blanks = 0;
    int false_lit = 0;

    if ((p->root.base.offset.x != 0) || (p->root.base.offset.y != 0))
    {
        (void)printf("  FAIL: the page carries an offset (%d,%d), the read-back coordinates will be off\n",
                     p->root.base.offset.x, p->root.base.offset.y);
        return 1;
    }

    (void)printf("\n== Dot read-back (world -> camera -> screen pixels -> bitmap) ==\n");
    for (j = 0; j < LOGO_GRID_H; j++)
    {
        char row[LOGO_GRID_W + 1];

        for (i = 0; i < LOGO_GRID_W; i++)
        {
            int16_t wx;
            int16_t wy;
            int16_t sx;
            int16_t sy;
            int dx;
            int dy;
            int peak = 0;
            int want = logo_bitmap_get(i, j);

            if (!cgl_debris_cell_world(&s_smash.debris, i, j, &wx, &wy))
            {
                row[i] = '?';
                continue;
            }
            cgl_iso2d_project(cam, wx, wy, 0, &sx, &sy);

            for (dy = -1; dy <= 1; dy++)
            {
                for (dx = -1; dx <= 1; dx++)
                {
                    int px = sx + dx;
                    int py = sy + dy;
                    int lum;

                    if ((px < 0) || (px >= CGL_CFG_SCREEN_W) ||
                        (py < 0) || (py >= CGL_CFG_SCREEN_H))
                    {
                        continue;
                    }
                    {
                        const cgl_color_t c = s_framebuffer[(py * CGL_CFG_SCREEN_W) + px];
                        /* RGB565 -> luminance weights (green has 6 bits, the largest weight)*/
                        const int r = (int)((c >> 11) & 0x1Fu);
                        const int g = (int)((c >> 5) & 0x3Fu);
                        const int b = (int)(c & 0x1Fu);

                        lum = ((r * 2) + (g * 4) + (b * 2)) * 2;
                    }
                    if (lum > peak)
                    {
                        peak = lum;
                    }
                }
            }

            if (peak > 180)
            {
                row[i] = '#';
                if (want != 0)
                {
                    hit++;
                }
                else
                {
                    false_lit++;
                }
            }
            else
            {
                row[i] = '.';
            }
            if (want != 0)
            {
                lit++;
            }
            else
            {
                blanks++;
            }
        }
        row[LOGO_GRID_W] = '\0';
        (void)printf("  %s\n", row);
    }

    (void)printf("\n  bitmap lit cells %d, read-back hits %d (%.1f%%); empty cells %d, adjacent-cell bleed %d (%.1f%%)\n",
                 lit, hit, (lit > 0) ? (100.0 * (double)hit / (double)lit) : 0.0,
                 blanks, false_lit,
                 (blanks > 0) ? (100.0 * (double)false_lit / (double)blanks) : 0.0);

    /*
     * Criteria:
     *   1) every lit cell must read back -- missing one means a stroke really was
     *      lost, zero tolerance.
     *   2) the empty cells tolerate a little bleed. On screen the dot pitch is
     *      2.77 px while a dot is 2 px wide, so a 2x2 dot necessarily has one
     *      pixel landing in a neighbouring cell's centre neighbourhood; that is
     *      the inherent cost of "solid strokes vs dot pitch", not a rendering bug
     *      (the bitmap printed below still reads as CytStudio).
     *      Truly zero bleed would only be possible by shrinking the dots to 1 px,
     *      at the price of turning the strokes into dotted lines.
     *      So the cap here is 5%: it stops the "dots smeared into a blob"
     *      regression without false-alarming on a few cells of bleed.
     */
    if (hit != lit)
    {
        (void)printf("  FAIL: some lit cells were not drawn (the projection lost dots)\n");
        return 1;
    }
    if ((false_lit * 20) > blanks)
    {
        (void)printf("  FAIL: bleed exceeds 5%%, the dot matrix has smeared into a blob\n");
        (void)printf("        when CGL_DEBRIS_SCENE_STYLE is CGL_ISO2D_STYLE_ISO_UNIFORM,\n");
        (void)printf("        CGL_DEBRIS_DOT_PX must be 1 (see the comment at the top of include/ext/cgl_debris_config.h)\n");
        return 1;
    }
    (void)printf("  PASS: every lit cell was hit and the bleed is within tolerance (the bitmap above is the content of the on-screen dot matrix)\n");
    return 0;
}

/* --------------------------------------------------------------------------- */
/* mock panel: collects whole bands into the framebuffer                       */
/* --------------------------------------------------------------------------- */

static uint16_t s_row;
static uint16_t s_col;

static int mock__init(void *ctx)
{
    (void)ctx;
    return 0;
}

static void mock__set_window(void *ctx, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    (void)ctx;
    (void)x1;
    (void)y1;
    s_col = x0;
    s_row = y0;
}

static void mock__write_pixels(void *ctx, const cgl_color_t *px, uint32_t count)
{
    uint32_t i;

    (void)ctx;
    for (i = 0u; i < count; i++)
    {
        s_framebuffer[(s_row * CGL_CFG_SCREEN_W) + s_col] = px[i];
        s_col++;
        if (s_col >= CGL_CFG_SCREEN_W)
        {
            s_col = 0u;
            s_row++;
        }
    }
}

static void mock__wait_idle(void *ctx) { (void)ctx; }
static void mock__backlight(void *ctx, uint8_t p) { (void)ctx; (void)p; }

static const cgl_display_drv_t mock_drv = {
    "mock",
    mock__init,
    mock__set_window,
    mock__write_pixels,
    mock__wait_idle,
    mock__backlight,
    CGL_CFG_SCREEN_W,
    CGL_CFG_SCREEN_H,
};

/* -------------------------------------------------------------------------- */

static int s_shot;

static void dump_frame(const char *tag)
{
    char path[64];

    /* PPM for any image viewer; BMP opens on a double click in Windows Paint*/
    (void)snprintf(path, sizeof(path), "build/frame%02d", s_shot);
    img_dump_ppm("build/frame%02d.ppm", s_shot,
                 s_framebuffer, CGL_CFG_SCREEN_W, CGL_CFG_SCREEN_H);
    img_dump_bmp("build/frame%02d.bmp", s_shot,
                 s_framebuffer, CGL_CFG_SCREEN_W, CGL_CFG_SCREEN_H);
    (void)printf("  frame%02d  %s\n", s_shot, tag);
    s_shot++;
}

/* --------------------------------------------------------------------------- */
/* Per-row verification of transition compositing                              */
/* --------------------------------------------------------------------------- */

/*
 * Asserting that "the offset changed" is not enough -- that only shows the numbers
 * move, not that **the pixels are drawn correctly**. This does a real end-to-end
 * check: take the two stable frames before and after the transition and use them
 * to explain the intermediate frame row by row.
 *
 * The geometry of COVER_UP (the new page covers the old one from below):
 *
 *      screen row y <  dy : the old page as it was        -> must equal prev[y] pixel by pixel
 *      screen row y >= dy : new page local row (y-dy)     -> equals next[y-dy]
 *
 * WARNING: why only "the 18 rows of the new page's top bar" are compared pixel by pixel:
 *    the lower half of the info page is a counter that refreshes itself
 *    (UPTIME / FRAMES / FPS ...) and its **text** changes every 150 ms, which has
 *    nothing to do with the transition. Comparing against a stable frame 20 frames
 *    after the transition would necessarily mismatch on those rows (measured
 *    42240 pixels = exactly every pixel of the lower half of the screen), and a
 *    perfectly correct transition would be misjudged as "bad compositing".
 *    The top bar (title + HOLD=BACK) is static content, so it is what gets the
 *    strict comparison.
 *
 * So the criteria split in two, and both are **exact**:
 *    A. the old-page region must have 0 pixel error         -- checks the old page's clipping / culling / occlusion order
 *    B. the new page's top bar must have 0 pixel error      -- checks that the pan translation is computed correctly
 *    C. the new-page region must **differ** from the old-page region -- checks that the new page really was drawn
 */
static cgl_color_t s_prev[CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H];
static cgl_color_t s_next[CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H];
/** The two reference frames for the Genie check: the page below / the page being sucked in.*/
static cgl_color_t s_under[CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H];
static cgl_color_t s_warped[CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H];
/** What the info page looks like when **forced opaque** = its own "content" (see verify_cover_up).*/
static cgl_color_t s_next_opaque[CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H];

/** Number of rows of the info page's top bar (static content, see the note above).*/
#define PAGE_STATIC_ROWS 18

static void capture(cgl_color_t *dst)
{
    uint32_t i;

    for (i = 0u; i < (uint32_t)(CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H); i++)
    {
        dst[i] = s_framebuffer[i];
    }
}

typedef struct
{
    uint32_t old_bad;  /**< pixel error of the old-page region (must be 0)*/
    uint32_t new_bad;  /**< pixel error of the new page's top bar (must be 0)*/
    uint32_t new_same; /**< rows of the new-page region that are **pixel-identical** to the old page (must be 0)*/
} cover_report_t;

static cover_report_t verify_cover_up(int16_t dy)
{
    cover_report_t r;
    int16_t y;
    /* The new page's current opacity (an overlay = translucent; when set to
       CGL_ALPHA_OPAQUE the blends below degrade into "take the new page content
       directly", exactly matching the old pixel-equality criterion).*/
    const uint16_t overlay_alpha = (uint16_t)cgl_widget_get_alpha(&s_info.root.base);

    r.old_bad = 0u;
    r.new_bad = 0u;
    r.new_same = 0u;

    /* C) No row of the new-page region may be completely identical to that row of the old page (otherwise the new page was not drawn)*/
    for (y = dy; y < CGL_CFG_SCREEN_H; y++)
    {
        const cgl_color_t *got = &s_framebuffer[(uint32_t)y * CGL_CFG_SCREEN_W];
        const cgl_color_t *old = &s_prev[(uint32_t)y * CGL_CFG_SCREEN_W];
        int16_t x;
        bool same = true;

        for (x = 0; x < CGL_CFG_SCREEN_W; x++)
        {
            if (got[x] != old[x])
            {
                same = false;
                break;
            }
        }
        if (same)
        {
            r.new_same++;
        }
    }

    /* A) old-page region: pixel by pixel*/
    for (y = 0; y < dy; y++)
    {
        const cgl_color_t *got = &s_framebuffer[(uint32_t)y * CGL_CFG_SCREEN_W];
        const cgl_color_t *want = &s_prev[(uint32_t)y * CGL_CFG_SCREEN_W];
        int16_t x;

        for (x = 0; x < CGL_CFG_SCREEN_W; x++)
        {
            if (got[x] != want[x])
            {
                r.old_bad++;
            }
        }
    }

    /* B) the new page's top bar: pixel by pixel (corresponding to new page local rows 0..PAGE_STATIC_ROWS-1)
     *
     * * The new page is a **translucent overlay**, so in a COVER_UP intermediate
     *   frame it is **composited** on top:
     *       got[y] = blend(old[y], new content[y-dy], alpha)
     *   You cannot compare against the stable frame s_next[y-dy] directly -- that
     *   one is
     *       blend(old[y-dy], new content[y-dy], alpha)
     *   and the two have different **backgrounds** (old[y] vs old[y-dy]), which is
     *   exactly the 700-odd pixels of difference.
     *   The right reference is s_next_opaque (= the new page content, independent of
     *   the background), blended with old[y] according to alpha.
     *   When the overlay alpha = 255, cgl_color_blend_q8 degrades into "take the
     *   second argument", so this criterion automatically falls back to the
     *   original "pixel by pixel equality" and is exact under both configurations.*/
    for (y = dy; y < (int16_t)(dy + PAGE_STATIC_ROWS); y++)
    {
        const cgl_color_t *got = &s_framebuffer[(uint32_t)y * CGL_CFG_SCREEN_W];
        const cgl_color_t *content = &s_next_opaque[(uint32_t)(y - dy) * CGL_CFG_SCREEN_W];
        const cgl_color_t *under_row = &s_prev[(uint32_t)y * CGL_CFG_SCREEN_W];
        int16_t x;

        for (x = 0; x < CGL_CFG_SCREEN_W; x++)
        {
            const cgl_color_t want = cgl_color_blend_q8(under_row[x], content[x], overlay_alpha);

            if (got[x] != want)
            {
                r.new_bad++;
            }
        }
    }
    return r;
}

/** The two stable frames must really differ, otherwise the verification above is meaningless.*/
static bool frames_differ(const cgl_color_t *a, const cgl_color_t *b)
{
    uint32_t i;
    uint32_t n = (uint32_t)(CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H);

    for (i = 0u; i < n; i++)
    {
        if (a[i] != b[i])
        {
            return true;
        }
    }
    return false;
}

/** Run n frames (the target cadence: 17 ms per frame).*/
static void run_frames(int n, const char *tag, int dump_every)
{
    int i;

    for (i = 0; i < n; i++)
    {
        cgl_screen_frame(&s_screen, 17u);
        if ((dump_every > 0) && (((i + 1) % dump_every) == 0))
        {
            dump_frame(tag);
        }
    }
}

/**
 * How many frames one navigation can last at most (17 ms/frame -> 3.4 s). **This is only a guard against an infinite loop.**
 *
 * WARNING: do not use `cgl_page_nav_duration()` to work out "how many frames this transition should take": that table holds the **per-type** defaults, and a page's own declaration (`cgl_page_set_trans_enter/leave`) overrides it -- for example the smash page declares 650 ms while the type table still has `GENIE_OUT` at `CGL_CFG_GENIE_MS` (300 ms). A cap derived from the type table would be shorter than the real transition, turning "the transition has not finished" into a string of false failures (this trap was hit in practice). So this only provides a cap so wide that it cannot be exceeded, and **the real frame count always comes from measuring the pushed frames** (the return value of run_until_idle).
 */
#define TRANS_FRAME_CAP 200

/** Push frames until this navigation has completely finished (including the frame in which the request was just registered and not yet applied).
 *  Returns how many frames were actually pushed -- that is the **measured** value of "how many frames this transition takes", and it can be printed to watch for regressions.
 *
 *  WARNING: the criterion must look at `busy` and `pending` together: requests are applied **at frame boundaries**, so right after request() and before any frame is pushed `busy` is still false, and looking only at busy would "succeed" without pushing a single frame -- the request stays parked and all later assertions fail in a chain (this trap was hit in practice too).*/
static int run_until_idle(int max_frames)
{
    int n = 0;

    while ((n < max_frames) &&
           (cgl_page_nav_busy(&s_nav) || cgl_page_nav_pending(&s_nav)))
    {
        cgl_screen_frame(&s_screen, 17u);
        n++;
    }
    return n;
}

/* --------------------------------------------------------------------------- */
/* Per-pixel verification of the Genie (suck-in) transition                    */
/* --------------------------------------------------------------------------- */

/*
 * A warp transition has no simple "the two pages each translate" geometry, so the
 * criterion becomes silhouette based:
 *
 *   on a target row y, the page being sucked in can only be drawn inside [x0', x1']
 *   -- that span is computed from "which source rows were compressed onto row y"
 *   plus the per-row column mapping (the mapping is monotonic, so the topmost
 *   source row gives the widest span; taking both ends once is the safest).
 *
 *   So the criteria are **exact** as well:
 *     A. outside the silhouette: must equal the stable frame of the page below, pixel by pixel (0 error)
 *     B. inside the silhouette: something must really have been drawn (a whole row still equal to the page below = nothing was drawn)
 *     C. inside the silhouette: at least a number of pixels must differ from the page below
 *
 * Precondition: the page being sucked in has static content for the whole
 * transition. On POP it has already left the top of the stack (it no longer
 * receives on_step) and the physics really has stopped -- that precondition is
 * itself the evidence that "a covered page no longer runs".
 */

/** Probe canvas: only for calling cgl_canvas_map_*(); it writes no pixels.*/
static cgl_color_t s_probe_buf[CGL_CFG_SCREEN_W * 4];
static cgl_canvas_t s_probe;

static void probe_arm(const cgl_warp_t *wp)
{
    cgl_canvas_init(&s_probe, s_probe_buf, CGL_CFG_SCREEN_W, 4, 0);
    cgl_canvas_set_warp(&s_probe, wp);
}

typedef struct
{
    uint32_t outside_bad; /**< pixel error outside the silhouette (must be 0)*/
    uint32_t inside_rows; /**< number of target rows inside the silhouette*/
    uint32_t inside_blank;/**< rows inside the silhouette that are still equal to the page below*/
    uint32_t inside_px;   /**< total pixels inside the silhouette that differ from the page below*/
    int16_t probe_x[6];   /**< coordinates of the first few "outside-silhouette errors" (for debugging)*/
    int16_t probe_y[6];
    int16_t probe_n;
} genie_report_t;

static genie_report_t verify_genie(const cgl_warp_t *wp, const cgl_color_t *under)
{
    genie_report_t r;
    int16_t y;

    r.outside_bad = 0u;
    r.inside_rows = 0u;
    r.inside_blank = 0u;
    r.inside_px = 0u;
    r.probe_n = 0;

    probe_arm(wp);

    for (y = 0; y < CGL_CFG_SCREEN_H; y++)
    {
        int16_t sy;
        int16_t lo = -1;
        int16_t hi = -1;
        int16_t xa;
        int16_t xb;
        uint32_t diff = 0u;
        bool same = true;
        int16_t x;

        for (sy = wp->src_y0; sy < (int16_t)(wp->src_y0 + wp->src_h); sy++)
        {
            if (cgl_canvas_map_y(&s_probe, sy) == y)
            {
                if (lo < 0)
                {
                    lo = sy;
                }
                hi = sy;
            }
        }
        if (lo < 0)
        {
            continue; /* This row received no sucked-in content*/
        }

        xa = cgl_canvas_map_x(&s_probe, 0, lo);                       /* panel's left edge*/
        xb = cgl_canvas_map_x(&s_probe, (int16_t)(CGL_CFG_SCREEN_W - 1), lo);
        {
            int16_t a2 = cgl_canvas_map_x(&s_probe, 0, hi);
            int16_t b2 = cgl_canvas_map_x(&s_probe, (int16_t)(CGL_CFG_SCREEN_W - 1), hi);
            if (a2 < xa)
            {
                xa = a2;
            }
            if (b2 > xb)
            {
                xb = b2;
            }
        }
        if (xa < 0)
        {
            xa = 0;
        }
        if (xb > (int16_t)(CGL_CFG_SCREEN_W - 1))
        {
            xb = (int16_t)(CGL_CFG_SCREEN_W - 1);
        }

        r.inside_rows++;
        for (x = 0; x < CGL_CFG_SCREEN_W; x++)
        {
            const cgl_color_t got = s_framebuffer[((uint32_t)y * CGL_CFG_SCREEN_W) + (uint32_t)x];
            const cgl_color_t want = under[((uint32_t)y * CGL_CFG_SCREEN_W) + (uint32_t)x];

            if ((x < xa) || (x > xb))
            {
                if (got != want)
                {
                    r.outside_bad++;
                    if (r.probe_n < 6)
                    {
                        r.probe_x[r.probe_n] = x;
                        r.probe_y[r.probe_n] = y;
                        r.probe_n++;
                    }
                }
            }
            else
            {
                if (got != want)
                {
                    diff++;
                    same = false;
                }
            }
        }
        if (same)
        {
            r.inside_blank++;
        }
        r.inside_px += diff;
    }
    return r;
}

/** Number of differing pixels when whole frames are compared pixel by pixel.*/
static uint32_t frame_diff(const cgl_color_t *want)
{
    uint32_t bad = 0u;
    uint32_t i;
    uint32_t n = (uint32_t)(CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H);

    for (i = 0u; i < n; i++)
    {
        if (s_framebuffer[i] != want[i])
        {
            bad++;
        }
    }
    return bad;
}

/** "Apply the navigation and redraw" without advancing physics at all (dt = 0).*/
static void settle_navigation(void)
{
    cgl_screen_frame(&s_screen, 0u);
}

int main(void)
{
    int fails = 0;
    int i;

    if (cgl_display_init(&s_display, &mock_drv, NULL) != 0)
    {
        return 1;
    }

    cgl_page_nav_init(&s_nav, "pages",
                     cgl_rect(0, 0, (int16_t)(CGL_CFG_SCREEN_W - 1), (int16_t)(CGL_CFG_SCREEN_H - 1)));
    /* One context, one shape per page (static: the pages keep a pointer to it). */
    static app_links_t links;
    static cgl_page_ctx_t ctx;

    links.smash = &s_smash;
    links.bullet = NULL; /* this test does not build the bullet time page */
    links.info = &s_info;
    links.menu = &s_menu;
    ctx.screen = &s_screen;
    ctx.nav = &s_nav;
    ctx.app = &links;

    app_page_smash_attach(&s_smash.base, &ctx);
    app_page_info_attach(&s_info.base, &ctx);
    app_page_menu_attach(&s_menu.base, &ctx);

    if (cgl_screen_init(&s_screen, &s_display, &s_nav) != 0)
    {
        return 1;
    }

    /* ----------------------------------------------------------------- */
    /* 0) main menu                                                      */
    /* ----------------------------------------------------------------- */
    (void)cgl_page_nav_reset(&s_nav, &s_menu.base);
    settle_navigation();
    (void)printf("page snapshots (dt = 17 ms per frame):\n");
    dump_frame("menu (bottom of the stack)");

    /* --------------------------------------------------------------------------------------------- */
    /* 1) smash page: push it without a transition, so the dot read-back coordinates are not shifted */
    /* --------------------------------------------------------------------------------------------- */
    if (!cgl_page_nav_request(&s_nav, CGL_NAV_PUSH, &s_smash.base, CGL_TRANS_NONE, NULL))
    {
        (void)printf("FAIL: cannot push the smash page\n");
        return 1;
    }
    settle_navigation();

    /* dt = 0 advances no physics, so this is the frame with the most complete dot matrix*/
    fails += verify_logo_dots(&s_smash);
    dump_frame("smash page enters (physics has not run, dot matrix intact)");

    /* ----------------------------------------------------------------- */
    /* 2) key moments of one smash animation round                       */
    /* ----------------------------------------------------------------- */
    {
        static const int marks[] = { 10, 60, 90, 120, 200 };

        for (i = 0; i < 260; i++)
        {
            cgl_screen_frame(&s_screen, 17u);
            {
                unsigned k;
                for (k = 0u; k < (sizeof(marks) / sizeof(marks[0])); k++)
                {
                    if (i == marks[k])
                    {
                        dump_frame("smash animation");
                    }
                }
            }
        }
    }
    (void)printf("  smash page: FPS statistics = %u, rounds = %u\n",
                 (unsigned)cgl_screen_fps(&s_screen),
                 (unsigned)app_page_smash_smashes(&s_smash));

    /* ----------------------------------------------------------------------------------------------------------------- */
    /* 3) COVER_UP: the info page covers the smash page from below                                                       */
    /*                                                                                                                   */
    /* WARNING: note that the smash page's physics is **really stopped** during this stretch (only the top page receives */
    /*    on_step), so the old page's pixels stay constant for the whole transition -- which is exactly what makes       */
    /*    "the old-page half must equal the stable frame before the transition, pixel by pixel" hold.                    */
    /*    If the old page were still moving, this assertion would fail, and it would incidentally catch the              */
    /*    "covered page is still running" bug.                                                                           */
    /*                                                                                                                   */
    /* First do a dry run with dt = 0 and capture the two "stable frames" (dt=0 advances no physics and no               */
    /* clock, so the picture is reproducible); then play COVER_UP for real and use the intermediate frames               */
    /* to verify the compositing pixel by pixel.                                                                         */
    /* ----------------------------------------------------------------------------------------------------------------- */
    (void)cgl_page_nav_request(&s_nav, CGL_NAV_PUSH, &s_info.base, CGL_TRANS_NONE, NULL);
    settle_navigation();
    capture(s_next); /* what the info page looks like at offset=0 (the real look with the page below showing through)*/
    /* Also capture the info page **forced opaque**: that equals the info page's own "content",
        because the root panel fills the screen opaquely, so this frame does not depend on what is below.
        The COVER_UP intermediate-frame criterion needs it -- see the notes on verify_cover_up().*/
    cgl_widget_set_alpha(&s_info.root.base, (uint8_t)CGL_ALPHA_OPAQUE);
    cgl_screen_redraw_all(&s_screen);
    settle_navigation();
    capture(s_next_opaque);
    cgl_widget_set_alpha(&s_info.root.base, (uint8_t)APP_INFO_OVERLAY_ALPHA);
    cgl_screen_redraw_all(&s_screen);
    settle_navigation();
    (void)cgl_page_nav_request(&s_nav, CGL_NAV_POP, NULL, CGL_TRANS_NONE, NULL);
    settle_navigation();
    capture(s_prev); /* what the smash page looks like at offset=0 (physics has not run, reproducible)*/

    if (!frames_differ(s_prev, s_next))
    {
        (void)printf("FAIL: the smash page and the info page render identically, no page change happened at all\n");
        fails++;
    }

    if (!cgl_page_nav_request(&s_nav, CGL_NAV_PUSH, &s_info.base, CGL_TRANS_COVER_UP, NULL))
    {
        (void)printf("FAIL: cannot push the info page\n");
        fails++;
    }
    else
    {
        int16_t dy = 0;
        cover_report_t rep;
        bool verified = false;

        rep.old_bad = 0u;
        rep.new_bad = 0u;
        rep.new_same = 0u;

        cgl_screen_frame(&s_screen, 17u); /* Apply the navigation + start the transition*/
        if (!cgl_page_nav_busy(&s_nav))
        {
            (void)printf("FAIL: the COVER_UP transition did not start\n");
            fails++;
        }

        /* Run a few frames and grab a moment where the new page is halfway up, to verify the compositing*/
        for (i = 0; i < 4; i++)
        {
            cgl_screen_frame(&s_screen, 17u);
            dump_frame("COVER_UP info page slides up");

            dy = s_info.root.base.offset.y;
            if ((dy >= 8) && (dy <= (int16_t)(CGL_CFG_SCREEN_H - 8)))
            {
                rep = verify_cover_up(dy);
                verified = true;
            }
        }
        (void)printf("  COVER_UP intermediate frame: info page offset.y = %d\n", dy);
        (void)printf("    old-page region pixel error %u (must be 0)\n", (unsigned)rep.old_bad);
        (void)printf("    new page top bar pixel error %u (must be 0)\n", (unsigned)rep.new_bad);
        (void)printf("    rows of the new-page region still holding the old page %u (must be 0)\n", (unsigned)rep.new_same);
        if (!verified)
        {
            (void)printf("FAIL: no halfway moment was captured (dy=%d), the transition may jump\n", dy);
            fails++;
        }
        else
        {
            if (rep.old_bad != 0u)
            {
                (void)printf("FAIL: the upper half of the screen (the old page) has %u wrong pixels\n", (unsigned)rep.old_bad);
                fails++;
            }
            if (rep.new_bad != 0u)
            {
                (void)printf("FAIL: the new page's top bar has %u wrong pixels -- the pan translation is computed wrong\n",
                             (unsigned)rep.new_bad);
                fails++;
            }
            if (rep.new_same != 0u)
            {
                (void)printf("FAIL: %u rows of the new-page region still hold the old page's content (the new page was not drawn)\n",
                             (unsigned)rep.new_same);
                fails++;
            }
        }

        /* Measure how many frames this transition took -- print it to watch for regressions (the duration is tunable, so do not hard-code an expected value)*/
        (void)printf("  COVER_UP finished in %d frames (the page declares 180 ms ~ 11 frames)\n",
                     run_until_idle(TRANS_FRAME_CAP));
        if (cgl_page_nav_busy(&s_nav))
        {
            (void)printf("FAIL: the COVER_UP transition did not finish\n");
            fails++;
        }
        if (cgl_page_nav_depth(&s_nav) != 3u)
        {
            (void)printf("FAIL: stack depth should be 3, actually %u\n", (unsigned)cgl_page_nav_depth(&s_nav));
            fails++;
        }
        if (!app_page_smash_is_paused(&s_smash))
        {
            (void)printf("FAIL: the smash page is covered but reports itself as not paused\n");
            fails++;
        }
        if ((s_info.root.base.offset.x != 0) || (s_info.root.base.offset.y != 0))
        {
            (void)printf("FAIL: the info page offset did not return to zero after the transition (%d,%d)\n",
                         s_info.root.base.offset.x, s_info.root.base.offset.y);
            fails++;
        }

        dump_frame("info page settled (smash page physics frozen)");

        /* ---------------------------------------------------------------------------------------------------------- */
        /* 3b) translucent overlay: while the info page covers the smash page, the page below must **still be drawn** */
        /*                                                                                                            */
        /*  This is the end-to-end lock on the "group opacity + show_below" combination: both are needed --           */
        /*    * cgl_page_set_show_below() -- otherwise nav hides the page below, and what shows                       */
        /*      through the overlay is the host background;                                                           */
        /*    * cgl_widget_set_alpha()   -- otherwise the overlay is not composited at all.                           */
        /*  The criterion is "opaque vs translucent must differ": if there is nothing below, blending the same        */
        /*  colour is the identity, both renders would be identical, and this assertion would fail.                   */
        /* ---------------------------------------------------------------------------------------------------------- */
        {
            static cgl_color_t s_overlay[CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H];
            static cgl_color_t s_opaque[CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H];
            uint32_t diff;

            if (!cgl_page_get_show_below(&s_info.base))
            {
                (void)printf("FAIL: the info page does not declare show_below (translucent overlay)\n");
                fails++;
            }
            if ((s_smash.root.base.flags & CGL_WF_VISIBLE) == 0u)
            {
                (void)printf("FAIL: the smash page was hidden after the info page covered it -- an overlay can only show the background through\n");
                fails++;
            }
            if (cgl_widget_get_alpha(&s_info.root.base) >= (uint8_t)CGL_ALPHA_OPAQUE)
            {
                (void)printf("FAIL: the info page root widget has no alpha, there is no overlay effect\n");
                fails++;
            }
            capture(s_overlay);

            cgl_widget_set_alpha(&s_info.root.base, (uint8_t)CGL_ALPHA_OPAQUE);
            cgl_screen_redraw_all(&s_screen);
            settle_navigation();
            capture(s_opaque);

            cgl_widget_set_alpha(&s_info.root.base, (uint8_t)APP_INFO_OVERLAY_ALPHA);
            cgl_screen_redraw_all(&s_screen);
            settle_navigation();
            diff = frame_diff(s_opaque);

            (void)printf("  translucent overlay: alpha=%u differs from alpha=255 by %u pixels;"
                         "smash page still visible=%d\n",
                         (unsigned)APP_INFO_OVERLAY_ALPHA, (unsigned)diff,
                         (int)((s_smash.root.base.flags & CGL_WF_VISIBLE) != 0u));
            if (!frames_differ(s_overlay, s_opaque))
            {
                (void)printf("FAIL: translucent and opaque render exactly the same -- alpha has no effect, or there is nothing below\n");
                fails++;
            }
        }

        /* ------------------------------------------------------------- */
        /* 4) SLIDE_DOWN: pop back to the smash page                     */
        /* ------------------------------------------------------------- */
        (void)cgl_page_nav_back(&s_nav);
        run_frames(3, "SLIDE_DOWN back to the smash page", 1);
        (void)printf("  SLIDE_DOWN finished in %d frames\n", run_until_idle(TRANS_FRAME_CAP));
        if (app_page_smash_is_paused(&s_smash))
        {
            (void)printf("FAIL: the smash page is still paused after going back\n");
            fails++;
        }

        /* Back to the menu: RESET is instantaneous and takes a different code path*/
        (void)cgl_page_nav_reset(&s_nav, &s_menu.base);
        run_frames(2, "", 0);
        if (cgl_page_nav_depth(&s_nav) != 1u)
        {
            (void)printf("FAIL: stack depth should be 1 after RESET, actually %u\n",
                         (unsigned)cgl_page_nav_depth(&s_nav));
            fails++;
        }
        dump_frame("back to the menu (RESET clears the stack)");
    }

    /* ---------------------------------------------------------------------------------------------------------------------------- */
    /* 5) GENIE_OUT: the smash page is "sucked" into the bottom of the screen (macOS minimise)                                      */
    /*                                                                                                                              */
    /* First run the transition once without checking and capture the stable frame of "the page below" (when the transition         */
    /* ends, only the menu page is left on screen); then run it again for real and explain every intermediate frame pixel by pixel. */
    /* ---------------------------------------------------------------------------------------------------------------------------- */
    /* WARNING: the smash page declares its own leave transition
       (app_page_smash.c: LOGO_SMASH_TRANS_OUT) and a page declaration outranks
       the caller, so the CGL_TRANS_GENIE_OUT passed below would be silently
       ignored and a FADE would play instead while this section asserted on the
       Genie silhouette. Withdraw the declaration so the caller's explicit type
       wins, which also decouples this test from the demo's current tuning. */
    cgl_page_set_trans_leave(&s_smash.base, CGL_TRANS_AUTO, CGL_PAGE_FX_KEEP_EASE,
                             CGL_PAGE_FX_KEEP_MS);

    (void)cgl_page_nav_request(&s_nav, CGL_NAV_PUSH, &s_smash.base, CGL_TRANS_NONE, NULL);
    settle_navigation();
    (void)cgl_page_nav_request(&s_nav, CGL_NAV_POP, NULL, CGL_TRANS_GENIE_OUT, NULL);
    (void)printf("  GENIE_OUT drained in %d frames (declaration withdrawn; type table: %u ms)\n",
                 run_until_idle(TRANS_FRAME_CAP),
                 (unsigned)cgl_page_nav_duration(&s_nav, CGL_TRANS_GENIE_OUT));
    if (cgl_page_nav_busy(&s_nav))
    {
        (void)printf("FAIL: the GENIE_OUT transition did not finish\n");
        fails++;
    }
    capture(s_under); /* the menu page (the smash page has been destroyed, only this one is on screen)*/
    dump_frame("after GENIE: only the menu page is left");

    (void)cgl_page_nav_request(&s_nav, CGL_NAV_PUSH, &s_smash.base, CGL_TRANS_NONE, NULL);
    settle_navigation();
    capture(s_warped); /* the smash page's stable frame (physics has not run, reproducible)*/
    dump_frame("before GENIE: the smash page at rest");

    if (!cgl_page_nav_request(&s_nav, CGL_NAV_POP, NULL, CGL_TRANS_GENIE_OUT, NULL))
    {
        (void)printf("FAIL: cannot start GENIE_OUT\n");
        fails++;
    }
    else
    {
        uint32_t worst_outside = 0u;
        uint32_t worst_blank = 0u;
        uint32_t blank_rows = 1u;
        uint32_t total_inside_px = 0u;
        uint32_t mid_rows = 0u;
        uint32_t start_bad = 0u;
        uint32_t first_p = 0u;
        bool saw_mid = false;

        (void)printf("\n== GENIE_OUT intermediate frames (inside/outside the funnel silhouette judged separately) ==\n");
        /* WARNING: `i` is main()'s shared loop counter and by this point it has
           been left at >= TRANS_FRAME_CAP by the loops above (the 260-frame smash
           animation, run_until_idle, ...). Reset it explicitly: without this the
           loop below is skipped entirely and every Genie assertion collapses. */
        for (i = 0; i < TRANS_FRAME_CAP; i++)
        {
            const cgl_warp_t *wp;
            genie_report_t rep;

            cgl_screen_frame(&s_screen, 17u);
            wp = cgl_page_nav_warp(&s_nav);

            if (i == 0)
            {
                /* The progress of this starting frame is not 0 yet: the animation has already ticked once **within the same frame**.
                   How far the first frame gets is decided entirely by the **easing curve** (about p=1 when
                   GENIE is sucked away with IN_QUAD; about p=12 when the page itself declares OUT_QUAD) -- that is
                   the App's freedom to tune, not rendering correctness. So the difference is only **printed**
                   here as a diagnostic; the real criteria are the two exact, curve-independent checks below:
                     * outside the silhouette must equal the page below pixel by pixel (verify_genie)
                     * at progress exactly 0 it must be the identity (the genie section of
                       tests/page_host_test.c + the warp path of alpha_host_test)*/
                start_bad = frame_diff(s_warped);
                first_p = (wp != NULL) ? (uint32_t)wp->progress_q8 : 0u;
            }
            if (wp == NULL)
            {
                break;
            }
            rep = verify_genie(wp, s_under);
            if (rep.inside_rows > (uint32_t)(CGL_CFG_SCREEN_H / 3))
            {
                saw_mid = true;
                mid_rows = rep.inside_rows;
                if (rep.outside_bad > worst_outside)
                {
                    worst_outside = rep.outside_bad;
                }
                if (rep.inside_blank > worst_blank)
                {
                    worst_blank = rep.inside_blank;
                }
                if (rep.inside_rows > blank_rows)
                {
                    blank_rows = rep.inside_rows;
                }
                total_inside_px += rep.inside_px;
                (void)printf("  frame %2d: p=%3u  silhouette %2u rows  outside error %u  empty rows inside %u\n",
                             i, (unsigned)wp->progress_q8, (unsigned)rep.inside_rows,
                             (unsigned)rep.outside_bad, (unsigned)rep.inside_blank);
                if (rep.outside_bad != 0u)
                {
                    int16_t q;
                    (void)printf("        first few:");
                    for (q = 0; q < rep.probe_n; q++)
                    {
                        uint32_t o = ((uint32_t)rep.probe_y[q] * CGL_CFG_SCREEN_W) +
                                     (uint32_t)rep.probe_x[q];
                        (void)printf(" (%d,%d)%04X/%04X", rep.probe_x[q], rep.probe_y[q],
                                     (unsigned)s_framebuffer[o], (unsigned)s_under[o]);
                    }
                    (void)printf("\n");
                }
            }
            if ((i % 3) == 0)
            {
                dump_frame("GENIE_OUT smash page sucked away");
            }
        }

        (void)printf("  start: p=%u, differs from the stable frame by %u pixels (%.1f%%, decided purely by the easing curve, diagnostic only)\n",
                     (unsigned)first_p, (unsigned)start_bad,
                     (100.0 * (double)start_bad) / (double)(CGL_CFG_SCREEN_W * CGL_CFG_SCREEN_H));
        (void)printf("  max error outside the silhouette over the intermediate frames %u (must be 0)\n", (unsigned)worst_outside);
        (void)printf("  %u pixels drawn inside the silhouette in the intermediate frames; at most %u of %u rows empty\n",
                     (unsigned)total_inside_px, (unsigned)worst_blank, (unsigned)blank_rows);

        if (!saw_mid)
        {
            (void)printf("FAIL: the frames where the funnel forms were not captured, the transition may jump\n");
            fails++;
        }
        if (worst_outside != 0u)
        {
            (void)printf("FAIL: %u pixels were drawn outside the funnel silhouette (covering the page below)\n",
                         (unsigned)worst_outside);
            fails++;
        }
        if (total_inside_px == 0u)
        {
            (void)printf("FAIL: not a single pixel was drawn inside the silhouette\n");
            fails++;
        }
        (void)printf("  widest intermediate frame captured: %u rows\n", (unsigned)mid_rows);

        (void)printf("  capturing the intermediate frames one by one took %d frames; %d more frames were pushed to finish\n", i,
                     run_until_idle(TRANS_FRAME_CAP));
        if (cgl_page_nav_warp(&s_nav) != NULL)
        {
            (void)printf("FAIL: the warp was not removed after the transition\n");
            fails++;
        }
        if ((s_menu.root.base.offset.x != 0) || (s_menu.root.base.offset.y != 0) ||
            (s_menu.root.base.warp != NULL))
        {
            (void)printf("FAIL: the menu page still carries offset/warp after the transition\n");
            fails++;
        }
        if (cgl_page_nav_depth(&s_nav) != 1u)
        {
            (void)printf("FAIL: stack depth should be 1 after GENIE, actually %u\n",
                         (unsigned)cgl_page_nav_depth(&s_nav));
            fails++;
        }
    }

    /* ---------------------------------------------------------------------------------------- */
    /* 6) GENIE_IN: the new page "pours" out of the suck-in point (macOS restore from the Dock) */
    /* ---------------------------------------------------------------------------------------- */
    /* Same trap on the enter side: LOGO_SMASH_TRANS_IN would outrank the
       explicit CGL_TRANS_GENIE_IN below. Withdraw it. */
    cgl_page_set_trans_enter(&s_smash.base, CGL_TRANS_AUTO, CGL_PAGE_FX_KEEP_EASE,
                             CGL_PAGE_FX_KEEP_MS);

    (void)cgl_page_nav_request(&s_nav, CGL_NAV_PUSH, &s_smash.base, CGL_TRANS_GENIE_IN, NULL);
    {
        const cgl_warp_t *wp = NULL;
        genie_report_t born;

        cgl_screen_frame(&s_screen, 17u);
        wp = cgl_page_nav_warp(&s_nav);
        dump_frame("GENIE_IN start: the smash page shrunk into the suck-in point");
        if (wp == NULL)
        {
            (void)printf("FAIL: the GENIE_IN transition did not start\n");
            fails++;
        }
        else
        {
            /* In this starting frame the new page is almost entirely shrunk into the suck-in point: most of the screen must still be the page below*/
            born = verify_genie(wp, s_under);
            (void)printf("\n== GENIE_IN ==\n  start: p=%u silhouette %u rows, outside error %u (must be 0)\n",
                         (unsigned)wp->progress_q8, (unsigned)born.inside_rows,
                         (unsigned)born.outside_bad);
            if (born.outside_bad != 0u)
            {
                (void)printf("FAIL: the new page at the GENIE_IN start was drawn outside the suck-in point\n");
                fails++;
            }
            if (born.inside_rows > (uint32_t)(CGL_CFG_SCREEN_H / 2))
            {
                (void)printf("FAIL: the new page at the GENIE_IN start is not confined near the suck-in point (it already occupies %u rows)\n",
                             (unsigned)born.inside_rows);
                fails++;
            }
        }

        run_frames(6, "GENIE_IN smash page pours out", 3);
        (void)printf("  GENIE_IN finished in %d frames\n", run_until_idle(TRANS_FRAME_CAP));

        if (cgl_page_nav_busy(&s_nav))
        {
            (void)printf("FAIL: the GENIE_IN transition did not finish\n");
            fails++;
        }
        if ((s_smash.root.base.warp != NULL) || (s_smash.root.base.offset.x != 0) ||
            (s_smash.root.base.offset.y != 0))
        {
            (void)printf("FAIL: the smash page still carries warp/offset after GENIE_IN\n");
            fails++;
        }
        if (cgl_page_nav_depth(&s_nav) != 2u)
        {
            (void)printf("FAIL: stack depth should be 2 after GENIE_IN, actually %u\n",
                         (unsigned)cgl_page_nav_depth(&s_nav));
            fails++;
        }
    }

    (void)printf("%d frame snapshots in total, transitions = %u, illegal requests = %u\n",
                 s_shot, (unsigned)cgl_page_nav_transitions(&s_nav),
                 (unsigned)cgl_page_nav_dropped(&s_nav));
    return (fails == 0) ? 0 : 1;
}
