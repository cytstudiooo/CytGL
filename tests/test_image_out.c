// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    test_image_out.c
  * @brief   Whole-frame image output for PC-side tests (PPM + BMP), implementation.
  *
  *  RGB565 -> 8-bit channels are expanded by high-bit alignment (the same way as
  *  the original dump_ppm in tests/render_host_test.c), so the exported images
  *  are slightly brighter than the panel's real 6-bit/5-bit precision, but that
  *  is perfectly good enough for checking layout and animation.
  ******************************************************************************
  */
#include "test_image_out.h"

#include <stdio.h>

/* --------------------------------------------------------------------------- */
/* Common bit helpers                                                          */
/* --------------------------------------------------------------------------- */

#define IMG_R5(c) ((uint8_t)(((c) >> 11) & 0x1Fu))
#define IMG_G6(c) ((uint8_t)(((c) >> 5) & 0x3Fu))
#define IMG_B5(c) ((uint8_t)((c) & 0x1Fu))

/* --------------------------------------------------------------------------- */
/* PPM (P6)                                                                    */
/* --------------------------------------------------------------------------- */

void img_dump_ppm(const char *path_fmt, int index,
                  const uint16_t *buf, int width, int height)
{
    char path[256];
    FILE *f;
    int i;

    if ((path_fmt == NULL) || (buf == NULL) || (width <= 0) || (height <= 0))
    {
        return;
    }
    (void)snprintf(path, sizeof(path), path_fmt, index);

    f = fopen(path, "wb");
    if (f == NULL)
    {
        (void)printf("  !! cannot open %s\n", path);
        return;
    }
    (void)fprintf(f, "P6\n%d %d\n255\n", width, height);

    for (i = 0; i < (width * height); i++)
    {
        uint16_t c = buf[i];
        unsigned char rgb[3];
        rgb[0] = (unsigned char)(IMG_R5(c) << 3);
        rgb[1] = (unsigned char)(IMG_G6(c) << 2);
        rgb[2] = (unsigned char)(IMG_B5(c) << 3);
        (void)fwrite(rgb, 1u, 3u, f);
    }
    (void)fclose(f);
    (void)printf("  %s\n", path);
}

/* --------------------------------------------------------------------------- */
/* BMP (24bpp, bottom-up, rows padded to 4 bytes)                              */
/* --------------------------------------------------------------------------- */

static void img__put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void img__put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

void img_dump_bmp(const char *path_fmt, int index,
                  const uint16_t *buf, int width, int height)
{
    char path[256];
    FILE *f;
    uint8_t file_hdr[14];
    uint8_t info_hdr[40];
    uint8_t row[3 * 1024]; /* At most 1024 px wide, enough for this project*/
    uint32_t stride;
    uint32_t image_size;
    int y;

    if ((path_fmt == NULL) || (buf == NULL) || (width <= 0) || (height <= 0))
    {
        return;
    }
    if (width > 1024)
    {
        (void)printf("  !! img_dump_bmp: width %d exceeds 1024, skipping\n", width);
        return;
    }
    /* Bytes per row padded to 4*/
    stride = (uint32_t)((((width * 3) + 3) / 4) * 4);
    image_size = stride * (uint32_t)height;

    (void)snprintf(path, sizeof(path), path_fmt, index);
    f = fopen(path, "wb");
    if (f == NULL)
    {
        (void)printf("  !! cannot open %s\n", path);
        return;
    }

    /* --- BITMAPFILEHEADER (14 B) --- */
    file_hdr[0] = 'B';
    file_hdr[1] = 'M';
    img__put_u32(&file_hdr[2], 14u + 40u + image_size); /* bfSize     */
    img__put_u16(&file_hdr[6], 0u);                     /* bfReserved1 */
    img__put_u16(&file_hdr[8], 0u);                     /* bfReserved2 */
    img__put_u32(&file_hdr[10], 14u + 40u);             /* bfOffBits  */

    /* --- BITMAPINFOHEADER (40 B) --- */
    img__put_u32(&info_hdr[0], 40u);                     /* biSize          */
    img__put_u32(&info_hdr[4], (uint32_t)width);         /* biWidth         */
    img__put_u32(&info_hdr[8], (uint32_t)height);        /* biHeight (+ = bottom-up)*/
    img__put_u16(&info_hdr[12], 1u);                     /* biPlanes        */
    img__put_u16(&info_hdr[14], 24u);                    /* biBitCount      */
    img__put_u32(&info_hdr[16], 0u);                     /* biCompression   */
    img__put_u32(&info_hdr[20], image_size);             /* biSizeImage     */
    img__put_u32(&info_hdr[24], 2835u);                  /* biXPelsPerMeter */
    img__put_u32(&info_hdr[28], 2835u);                  /* biYPelsPerMeter */
    img__put_u32(&info_hdr[32], 0u);                     /* biClrUsed       */
    img__put_u32(&info_hdr[36], 0u);                     /* biClrImportant  */

    (void)fwrite(file_hdr, 1u, sizeof(file_hdr), f);
    (void)fwrite(info_hdr, 1u, sizeof(info_hdr), f);

    /* Pixel data: BMP is BGR ordered and stored bottom-up*/
    for (y = height - 1; y >= 0; y--)
    {
        const uint16_t *src = buf + ((size_t)y * (size_t)width);
        uint32_t pad = stride - ((uint32_t)width * 3u);
        int x;

        for (x = 0; x < width; x++)
        {
            uint16_t c = src[x];
            row[x * 3 + 0] = (uint8_t)(IMG_B5(c) << 3);
            row[x * 3 + 1] = (uint8_t)(IMG_G6(c) << 2);
            row[x * 3 + 2] = (uint8_t)(IMG_R5(c) << 3);
        }
        for (x = 0; x < (int)pad; x++)
        {
            row[(width * 3) + x] = 0u;
        }
        (void)fwrite(row, 1u, (size_t)stride, f);
    }
    (void)fclose(f);
    (void)printf("  %s\n", path);
}
