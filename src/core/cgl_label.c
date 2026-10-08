// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_label.c
  * @brief   LCD UI framework - text label widget.
  ******************************************************************************
  */
#include <cytgl/cgl_label.h>

/* -------------------------------------------------------------------------- */
/* tiny string helpers (deliberately libc free)                               */
/* -------------------------------------------------------------------------- */

/* Append into a bounded buffer. Returns the new write offset. */
static uint8_t str__append(char *dst, uint8_t at, const char *src)
{
    while ((*src != '\0') && (at < (CGL_CFG_LABEL_MAX_TEXT - 1u)))
    {
        dst[at] = *src;
        at++;
        src++;
    }
    dst[at] = '\0';
    return at;
}

/* Append an unsigned decimal number, returns the new write offset. */
static uint8_t str__append_u32(char *dst, uint8_t at, uint32_t value)
{
    char tmp[10];
    uint8_t n = 0u;

    if (value == 0u)
    {
        return str__append(dst, at, "0");
    }
    while ((value > 0u) && (n < sizeof(tmp)))
    {
        tmp[n] = (char)('0' + (value % 10u));
        value /= 10u;
        n++;
    }
    while ((n > 0u) && (at < (CGL_CFG_LABEL_MAX_TEXT - 1u)))
    {
        n--;
        dst[at] = tmp[n];
        at++;
        dst[at] = '\0';
    }
    return at;
}

static uint8_t str__append_i32(char *dst, uint8_t at, int32_t value)
{
    if (value < 0)
    {
        at = str__append(dst, at, "-");
        return str__append_u32(dst, at, (uint32_t)(-value));
    }
    return str__append_u32(dst, at, (uint32_t)value);
}

/* -------------------------------------------------------------------------- */
/* widget                                                                     */
/* -------------------------------------------------------------------------- */

static int16_t label__text_x(const cgl_label_t *l)
{
    int16_t w = cgl_font_text_width(l->font, l->text);
    int16_t fw = cgl_rect_width(l->base.frame);

    switch (l->align)
    {
    case CGL_ALIGN_CENTER:
        return (int16_t)(l->base.frame.x0 + ((fw - w) / 2));
    case CGL_ALIGN_RIGHT:
        return (int16_t)(l->base.frame.x1 - w + 1);
    case CGL_ALIGN_LEFT:
    default:
        return l->base.frame.x0;
    }
}

static void label__draw(cgl_widget_t *w, cgl_canvas_t *c)
{
    cgl_label_t *l = (cgl_label_t *)w;
    int16_t ty = (int16_t)(w->frame.y0 + ((cgl_rect_height(w->frame) - (int16_t)l->font->height) / 2));

    if (ty < w->frame.y0)
    {
        ty = w->frame.y0;
    }
    if (l->opaque)
    {
        cgl_canvas_fill_rect(c, w->frame, l->bg);
    }
    cgl_font_draw_text(c, l->font, label__text_x(l), ty, l->text, l->fg);
}

static const cgl_widget_vt_t label__vt = {
    /* Use designated initialisers: cgl_widget_vt_t grows members as the framework
       evolves (this round added .event), and positional initialisation silently
       shifts out of place when a member is added. */
    .type = "label",
    .draw = label__draw,
    .update = NULL,
    .event = NULL,
};

void cgl_label_init(cgl_label_t *l, const char *name, cgl_rect_t frame,
                   const cgl_font_t *font, const char *text,
                   cgl_color_t fg, cgl_color_t bg, bool opaque)
{
    cgl_widget_init(&l->base, &label__vt, name, frame);
    l->font = (font != NULL) ? font : &cgl_font_8x16;
    l->fg = fg;
    l->bg = bg;
    l->opaque = opaque ? 1u : 0u;
    l->align = (uint8_t)CGL_ALIGN_LEFT;
    l->text[0] = '\0';
    if (text != NULL)
    {
        (void)str__append(l->text, 0u, text);
    }
}

static void label__commit(cgl_label_t *l, const char *candidate)
{
    uint8_t i = 0u;
    bool changed = false;

    while ((l->text[i] != '\0') || (candidate[i] != '\0'))
    {
        if (l->text[i] != candidate[i])
        {
            changed = true;
            break;
        }
        i++;
    }
    if (!changed)
    {
        return;
    }
    for (i = 0u; i < CGL_CFG_LABEL_MAX_TEXT; i++)
    {
        l->text[i] = candidate[i];
        if (candidate[i] == '\0')
        {
            break;
        }
    }
    l->text[CGL_CFG_LABEL_MAX_TEXT - 1u] = '\0';
    cgl_widget_invalidate(&l->base);
}

void cgl_label_set_text(cgl_label_t *l, const char *text)
{
    char tmp[CGL_CFG_LABEL_MAX_TEXT];

    if ((l == NULL) || (text == NULL))
    {
        return;
    }
    tmp[0] = '\0';
    (void)str__append(tmp, 0u, text);
    label__commit(l, tmp);
}

void cgl_label_set_int(cgl_label_t *l, const char *prefix, int32_t value, const char *suffix)
{
    char tmp[CGL_CFG_LABEL_MAX_TEXT];
    uint8_t at = 0u;

    if (l == NULL)
    {
        return;
    }
    tmp[0] = '\0';
    if (prefix != NULL)
    {
        at = str__append(tmp, at, prefix);
    }
    at = str__append_i32(tmp, at, value);
    if (suffix != NULL)
    {
        at = str__append(tmp, at, suffix);
    }
    tmp[at] = '\0';
    label__commit(l, tmp);
}

void cgl_label_set_fixed1(cgl_label_t *l, const char *prefix, int32_t value_x10, const char *suffix)
{
    char tmp[CGL_CFG_LABEL_MAX_TEXT];
    uint8_t at = 0u;
    uint32_t mag;

    if (l == NULL)
    {
        return;
    }
    tmp[0] = '\0';
    if (prefix != NULL)
    {
        at = str__append(tmp, at, prefix);
    }
    if (value_x10 < 0)
    {
        at = str__append(tmp, at, "-");
        mag = (uint32_t)(-value_x10);
    }
    else
    {
        mag = (uint32_t)value_x10;
    }
    at = str__append_u32(tmp, at, mag / 10u);
    at = str__append(tmp, at, ".");
    at = str__append_u32(tmp, at, mag % 10u);
    if (suffix != NULL)
    {
        at = str__append(tmp, at, suffix);
    }
    tmp[at] = '\0';
    label__commit(l, tmp);
}

void cgl_label_set_align(cgl_label_t *l, cgl_align_t align)
{
    l->align = (uint8_t)align;
    cgl_widget_invalidate(&l->base);
}

void cgl_label_set_colors(cgl_label_t *l, cgl_color_t fg, cgl_color_t bg, bool opaque)
{
    l->fg = fg;
    l->bg = bg;
    l->opaque = opaque ? 1u : 0u;
    cgl_widget_invalidate(&l->base);
}
