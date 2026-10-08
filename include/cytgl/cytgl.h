// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cytgl.h
  * @brief   CytGL -- the single entry point.
  *
  *  One `#include <cytgl/cytgl.h>` pulls in the whole public API of the
  *  framework:
  *
  *      cgl_types.h      Basic types: RGB565 colors, points, rectangles, input events
  *      cgl_math.h       Integer trigonometry, polar maps, fixed-point helpers
  *      cgl_canvas.h     Band canvas + software rasteriser (including warp)
  *      cgl_font.h       Dot-matrix fonts and text rendering
  *      cgl_widget.h     Retained-mode widget tree
  *      cgl_panel.h      Built-in widget: background / border container
  *      cgl_label.h      Built-in widget: text label
  *      cgl_bar.h        Built-in widget: progress / level bar
  *      cgl_arc.h        Radial toolkit: shaded surfaces, tapered needles, ring gauge
  *      cgl_watch.h      Built-in widget: an analog watch face (needs cgl_arc)
  *      cgl_display.h    Display device abstraction + band presenter pipeline
  *      cgl_time.h       The one and only clock (frame time + fixed-step scheduling + FPS)
  *      cgl_anim.h       Integer tweens + Q8 easing curves
  *      cgl_page.h       Page objects, navigation stack, transitions
  *      cgl_screen.h     Stitches the pieces above into "one frame"
  *
  *  cgl_arc.h and cgl_watch.h are compile-time optional (CGL_CFG_ARC /
  *  CGL_CFG_WATCH in cytgl_config.h); when they are off, the two headers expand to
  *  nothing and neither module is linked.
  *
  *  For the porting layer (the part the user must implement) see `cgl_port.h`.
  *
  *  All compile-time configuration lives in `cytgl_config.h` and can be
  *  overridden with `-D` from the build system.
  *
  *  Dependency policy: this framework depends ONLY on the C99 standard library
  *  (stdint / stdbool / stddef). It depends on no HAL, no libm and no malloc,
  *  and it does not assume that a full-screen framebuffer exists.
  ******************************************************************************
  */
#ifndef CGL_CYTGL_H
#define CGL_CYTGL_H

/** Version: major.minor.patch -- semantic versioning; bump the minor number
    when the public API changes. */
#define CGL_VERSION_MAJOR 0
#define CGL_VERSION_MINOR 1
#define CGL_VERSION_PATCH 0

#define CGL_VERSION_STRING "0.1.0"

#ifdef __cplusplus
extern "C" {
#endif

#include <cytgl/cytgl_config.h>
#include <cytgl/cgl_types.h>
#include <cytgl/cgl_canvas.h>
#include <cytgl/cgl_font.h>
#include <cytgl/cgl_widget.h>
#include <cytgl/cgl_panel.h>
#include <cytgl/cgl_label.h>
#include <cytgl/cgl_bar.h>
#include <cytgl/cgl_arc.h>
#include <cytgl/cgl_watch.h>
#include <cytgl/cgl_display.h>
#include <cytgl/cgl_time.h>
#include <cytgl/cgl_anim.h>
#include <cytgl/cgl_page.h>
#include <cytgl/cgl_screen.h>
#include <cytgl/cgl_port.h>

#ifdef __cplusplus
}
#endif

#endif /* CGL_CYTGL_H */
