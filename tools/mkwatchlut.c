// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    mkwatchlut.c
  * @brief   Generate the watch module's two default dial tables.
  *
  *  `src/core/cgl_watch_dial.c` holds two arrays of RGB565 colours -- the default
  *  dial's face and the ring it is set into -- and a table like that rots the moment
  *  someone changes a palette stop: the code that *would* have produced the new
  *  colours is not the code that draws them, and the face goes on rendering the old
  *  ones while the constants claim otherwise. No rendering test can catch that,
  *  because the picture is still a perfectly good picture.
  *
  *  So the generator is the framework's own `cgl_arc_surface_bake()`, this file is a
  *  thin command-line wrapper around it, and `tests/watch_host_test.c` re-runs the
  *  same baker over every entry at test time. Nothing here re-derives the arithmetic;
  *  there is exactly one implementation of it and this is not it.
  *
  *  Build and run (from the repository root, any host compiler):
  *
  *      cc -std=c11 -DCGL_CFG_WATCH_DIAL_LUT=0 -Iinclude tools/mkwatchlut.c \
  *         src/core/cgl_arc.c src/core/cgl_watch.c src/core/cgl_math.c \
  *         src/core/cgl_canvas.c src/core/cgl_widget.c src/core/cgl_anim.c \
  *         src/core/cgl_font.c src/core/cgl_font_data.c \
  *         -o build/mkwatchlut && build/mkwatchlut
  *
  *  Paste its output over the two arrays in src/core/cgl_watch_dial.c.
  *
  *  WARNING: `-DCGL_CFG_WATCH_DIAL_LUT=0` is what makes this work: with the tables
  *  baked in, `cgl_watch_config_default()` points its surfaces at the very arrays
  *  this program is about to produce, and the link would need them before they
  *  exist. Building it that way leaves the two `lut` pointers NULL, which is exactly
  *  the configuration the baker wants anyway.
  ******************************************************************************
  */
#include <stdio.h>

#include <cytgl/cgl_watch.h>

static void print_table(const char *name, const char *n_macro, const uint16_t *v, uint16_t n,
                        const char *comment)
{
    uint16_t i;

    printf("/** %s */\n", comment);
    printf("const uint16_t %s[%s] = {\n", name, n_macro);
    for (i = 0u; i < n; i++)
    {
        if ((i % 12u) == 0u)
        {
            printf("    ");
        }
        printf("0x%04Xu,", (unsigned)v[i]);
        printf(((i % 12u) == 11u) ? "\n" : " ");
    }
    printf("\n};\n\n");
}

int main(void)
{
    cgl_watch_config_t cfg;
    cgl_arc_surface_t face;
    cgl_arc_surface_t well;
    uint16_t face_lut[CGL_ARC_FACE_LUT_N];
    uint16_t well_lut[CGL_ARC_RING_LUT_N];

    cgl_watch_config_default(&cfg);

    /* The two surfaces as `cgl_watch_config_default()` leaves them minus the tables:
       the same colours, geometry and texture amplitudes, shaded procedurally so that
       the bake below has something to bake *from*. */
    face = cfg.style.face;
    well = cfg.style.well;
    face.lut = NULL;
    face.lut_n = (uint16_t)CGL_ARC_FACE_LUT_N;
    well.lut = NULL;
    well.lut_n = (uint16_t)CGL_ARC_RING_LUT_N;

    cgl_arc_surface_bake(&face, face_lut, (uint16_t)CGL_ARC_FACE_LUT_N);
    cgl_arc_surface_bake(&well, well_lut, (uint16_t)CGL_ARC_RING_LUT_N);

    print_table("cgl_watch_face_lut_default", "CGL_ARC_FACE_LUT_N", face_lut,
                (uint16_t)CGL_ARC_FACE_LUT_N,
                "The default face: entry `i` is the colour of `sqrt(i/(N-1))` of the dial, "
                "with the circular brushing baked in.");
    print_table("cgl_watch_well_lut_default", "CGL_ARC_RING_LUT_N", well_lut,
                (uint16_t)CGL_ARC_RING_LUT_N,
                "The default ring: entry `i` is one step across the recessed well's "
                "cross-section.");
    return 0;
}
