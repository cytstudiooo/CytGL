// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_anim.c
  * @brief   LCD UI framework - integer tweens and easing curves.
  ******************************************************************************
  */
#include <cytgl/cgl_anim.h>

/* -------------------------------------------------------------------------- */
/* Easing curves (all Q8 fixed point)                                         */
/* -------------------------------------------------------------------------- */

/**
 * Cubic term q^3, input and output are both Q8.
 *
 * * Why shift >>8 in two steps instead of computing q*q*q first and then >>16:
 *   q is at most 256, q*q*q = 16.7 M still fits in int32, but after multiplying
 *   by the coefficient c3=692 it is 1.16e10 -- int32 overflow. Doing it in steps
 *   keeps the intermediate result down at Q8 (<=256) the whole time: the
 *   coefficient product is at most 692*256 = 177 k, far from overflowing.
 */
static inline int32_t ease__cube_q8(int32_t q)
{
    int32_t q2 = (q * q) >> 8; /* q=256 -> 256; sign is preserved */
    return (q2 * q) >> 8;
}

uint16_t cgl_anim_ease(cgl_ease_t ease, uint16_t p_q8)
{
    int32_t p = (int32_t)p_q8;
    int32_t r;

    if (p < 0)
    {
        p = 0;
    }
    if (p > CGL_ANIM_ONE_Q8)
    {
        p = CGL_ANIM_ONE_Q8;
    }

    switch (ease)
    {
    case CGL_EASE_IN_QUAD:
        r = (p * p) >> 8;
        break;

    case CGL_EASE_OUT_QUAD:
        {
            int32_t q = CGL_ANIM_ONE_Q8 - p;
            r = CGL_ANIM_ONE_Q8 - ((q * q) >> 8);
        }
        break;

    case CGL_EASE_IN_OUT_QUAD:
        if (p < (CGL_ANIM_ONE_Q8 / 2))
        {
            r = (p * p) >> 7; /* first half: 0..128 */
        }
        else
        {
            int32_t q = CGL_ANIM_ONE_Q8 - p;
            r = CGL_ANIM_ONE_Q8 - ((q * q) >> 7);
        }
        break;

    case CGL_EASE_OUT_CUBIC:
        {
            int32_t q = CGL_ANIM_ONE_Q8 - p;
            r = CGL_ANIM_ONE_Q8 - ease__cube_q8(q);
        }
        break;

    case CGL_EASE_OUT_BACK:
        {
            /* Classic 1.70158 (Q8: 436). c3 = c1 + 1 = 2.70158 (Q8: 692).
             *
             * f(p) = 1 + c3*(p-1)^3 + c1*(p-1)^2
             *
             * WARNING: note that the cubic term is **subtracted**: here
             * q = 1-p is the positive way of writing (p-1), so
             * (p-1)^3 = -q^3. The first version used a plus sign and produced
             * 5.4 instead of 0 at p=0 (256 + 692 + 436), lifting the whole
             * curve -- the endpoint assertion caught it at once. */
            const int32_t c1 = 436;
            const int32_t c3 = 692;
            int32_t q = CGL_ANIM_ONE_Q8 - p;
            int32_t q2 = (q * q) >> 8;
            int32_t q3 = ease__cube_q8(q);

            r = CGL_ANIM_ONE_Q8 - ((c3 * q3) >> 8) + ((c1 * q2) >> 8);
        }
        break;

    case CGL_EASE_LINEAR:
    default:
        r = p;
        break;
    }

    if (r < 0)
    {
        r = 0;
    }
    return (uint16_t)r;
}

int32_t cgl_anim_lerp(int32_t a, int32_t b, uint16_t t_q8)
{
    int32_t span = b - a;

    /* Multiply before dividing, so that (span/256) is not truncated to 0 and
       the whole animation stays frozen. */
    return a + ((span * (int32_t)t_q8) / CGL_ANIM_ONE_Q8);
}

/* -------------------------------------------------------------------------- */
/* Tween                                                                      */
/* -------------------------------------------------------------------------- */

void cgl_anim_init(cgl_anim_t *a)
{
    if (a == NULL)
    {
        return;
    }
    a->from = 0;
    a->to = 0;
    a->value = 0;
    a->dur_ms = 0u;
    a->elapsed_ms = 0u;
    a->delay_ms = 0u;
    a->ease = (uint8_t)CGL_EASE_LINEAR;
    a->flags = 0u;
}

void cgl_anim_start(cgl_anim_t *a, int32_t from, int32_t to, uint32_t dur_ms, cgl_ease_t ease)
{
    if (a == NULL)
    {
        return;
    }
    a->from = from;
    a->to = to;
    a->value = from;
    a->dur_ms = dur_ms;
    a->elapsed_ms = 0u;
    a->delay_ms = 0u;
    a->ease = (uint8_t)((ease < CGL_EASE_COUNT) ? ease : CGL_EASE_LINEAR);
    a->flags = CGL_ANIM_F_RUNNING;

    /* Zero duration: the next tick converges to `to` immediately (rather than
       jumping here, so that the caller can still observe one "value changed"
       and therefore produce one repaint). */
}

void cgl_anim_set_delay(cgl_anim_t *a, uint32_t delay_ms)
{
    if (a != NULL)
    {
        a->delay_ms = delay_ms;
    }
}

bool cgl_anim_tick(cgl_anim_t *a, uint32_t dt_ms)
{
    int32_t before;

    if ((a == NULL) || ((a->flags & CGL_ANIM_F_RUNNING) == 0u))
    {
        return false;
    }

    before = a->value;

    /* Delay phase: value stays at `from` and produces no change */
    if (a->delay_ms > 0u)
    {
        if (dt_ms < a->delay_ms)
        {
            a->delay_ms -= dt_ms;
            return false;
        }
        dt_ms -= a->delay_ms;
        a->delay_ms = 0u;
    }

    if (a->dur_ms == 0u)
    {
        a->elapsed_ms = 0u;
        a->value = a->to;
        a->flags = (uint8_t)CGL_ANIM_F_DONE;
        return (a->value != before);
    }

    a->elapsed_ms += dt_ms;
    if (a->elapsed_ms >= a->dur_ms)
    {
        a->elapsed_ms = a->dur_ms;
        a->value = a->to;
        a->flags = (uint8_t)CGL_ANIM_F_DONE;
    }
    else
    {
        uint16_t p = (uint16_t)((a->elapsed_ms * (uint32_t)CGL_ANIM_ONE_Q8) / a->dur_ms);
        uint16_t e = cgl_anim_ease((cgl_ease_t)a->ease, p);

        a->value = a->from + (int32_t)(((a->to - a->from) * (int32_t)e) / CGL_ANIM_ONE_Q8);
    }

    return (a->value != before) || ((a->flags & CGL_ANIM_F_DONE) != 0u);
}

void cgl_anim_finish(cgl_anim_t *a)
{
    if (a == NULL)
    {
        return;
    }
    a->elapsed_ms = a->dur_ms;
    a->delay_ms = 0u;
    a->value = a->to;
    a->flags = (uint8_t)CGL_ANIM_F_DONE;
}

void cgl_anim_stop(cgl_anim_t *a)
{
    if (a != NULL)
    {
        a->flags = (uint8_t)(a->flags & (uint8_t)~CGL_ANIM_F_RUNNING);
    }
}

uint16_t cgl_anim_progress_q8(const cgl_anim_t *a)
{
    uint32_t p;

    if ((a == NULL) || (a->dur_ms == 0u))
    {
        return CGL_ANIM_ONE_Q8;
    }
    if (a->elapsed_ms >= a->dur_ms)
    {
        return CGL_ANIM_ONE_Q8;
    }
    p = (a->elapsed_ms * (uint32_t)CGL_ANIM_ONE_Q8) / a->dur_ms;
    return (uint16_t)p;
}
