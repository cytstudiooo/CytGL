// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    test_image_out.h
  * @brief   Whole-frame image output for PC-side tests (PPM + BMP); not built
  *          into the firmware.
  *
  *  PPM (P6) is a standard binary format that GIMP / IrfanView / VS Code
  *  extensions open directly; BMP is the fallback that Windows Paint (mspaint)
  *  shows on a double click. Both are zero-dependency, hand-written C headers
  *  that need no image library and no compression.
  *
  *  Usage (buf is RGB565 row-major, w*h pixels):
  *
  *      img_dump_ppm("build/frame%02d.ppm", shot, buf, w, h);
  *      img_dump_bmp("build/frame%02d.bmp", shot, buf, w, h);
  *
  *  Just compile tests/test_image_out.c into the test program.
  ******************************************************************************
  */
#ifndef TEST_IMAGE_OUT_H
#define TEST_IMAGE_OUT_H

#include <stdint.h>

/** Write a P6 binary PPM. width/height are pixel counts, buf is RGB565.*/
void img_dump_ppm(const char *path_fmt, int index,
                  const uint16_t *buf, int width, int height);

/** Write a 24bpp uncompressed BMP (rows padded to 4 bytes).*/
void img_dump_bmp(const char *path_fmt, int index,
                  const uint16_t *buf, int width, int height);

#endif /* TEST_IMAGE_OUT_H */
