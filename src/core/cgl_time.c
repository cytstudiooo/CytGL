// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    cgl_time.c
  * @brief   LCD UI framework - the one and only clock.
  ******************************************************************************
  */
#include <cytgl/cgl_time.h>
#include <cytgl/cytgl_config.h>

/** FPS averaging window, in milliseconds. It is only computed when the window
    closes, so this is an *average* frame rate, not an instantaneous value. */
#define CGL_TIME_FPS_WINDOW_MS 500u

void cgl_clock_init(cgl_clock_t *c, uint32_t step_ms, uint32_t max_dt_ms)
{
    if (c == NULL)
    {
        return;
    }
    c->now_ms = 0u;
    c->frame = 0u;
    c->dt_ms = 0u;

    c->step_ms = step_ms;
    c->accum_ms = 0u;
    c->max_dt_ms = (max_dt_ms == 0u) ? CGL_CLOCK_DEFAULT_MAX_DT_MS : max_dt_ms;
    c->step_total = 0u;
    c->max_steps = (uint8_t)CGL_CFG_CLOCK_MAX_STEPS;
    c->steps = 0u;
    c->step_index = 0u;

    c->fps = 0u;
    c->_fps_ms = 0u;
    c->_fps_frames = 0u;
}

void cgl_clock_reset(cgl_clock_t *c)
{
    uint32_t step_ms;
    uint32_t max_dt_ms;
    uint8_t max_steps;

    if (c == NULL)
    {
        return;
    }
    /* Configuration is a "property", the counters are "state": a reset only
       clears state. */
    step_ms = c->step_ms;
    max_dt_ms = c->max_dt_ms;
    max_steps = c->max_steps;
    cgl_clock_init(c, step_ms, max_dt_ms);
    c->max_steps = max_steps;
}

void cgl_clock_begin(cgl_clock_t *c, uint32_t dt_ms)
{
    uint32_t due;

    if (c == NULL)
    {
        return;
    }

    /* Stall guard: when J-Link halts for 3 seconds and then resumes, those 3
       seconds must be dropped. Not "caught up slowly" but never fed into the
       accumulator at all -- see the modulo below. */
    if (dt_ms > c->max_dt_ms)
    {
        dt_ms = c->max_dt_ms;
    }
    c->dt_ms = dt_ms;
    c->now_ms += dt_ms;
    c->frame++;

    /* --- FPS: derived from accumulated time ---------------------------------
       Uses "frames in the window / real milliseconds in the window" rather than
       the HAL_GetTick delta, so it shares one time base with the physics and
       there is no illusion of "58 fps displayed but physics running at 60 Hz". */
    c->_fps_ms += dt_ms;
    c->_fps_frames++;
    if (c->_fps_ms >= CGL_TIME_FPS_WINDOW_MS)
    {
        c->fps = (c->_fps_frames * CGL_CLOCK_MS_PER_S) / c->_fps_ms;
        c->_fps_ms = 0u;
        c->_fps_frames = 0u;
    }

    /* --- Fixed step: turn this frame's time budget into "how many steps run
       this frame" ---------------------------------------------------------- */
    c->steps = 0u;
    c->step_index = 0u;
    if (c->step_ms == 0u)
    {
        return; /* frame time only, no logic step */
    }

    c->accum_ms += dt_ms;
    due = c->accum_ms / c->step_ms;

    if (due > (uint32_t)c->max_steps)
    {
        /* * Key point: discard the **whole-step time that was due** in one go
           (the modulo), instead of only subtracting the number of steps that
           actually run. Subtracting only the steps leaves the debt behind after
           a stall, and the next few hundred frames all have to catch up -- which
           looks like "the animation suddenly fast-forwards after a hiccup".
           The price is that physics time runs slightly behind wall time (the
           discarded steps); that is deliberate: the simulation would rather skip
           a few steps than fast-forward. */
        c->accum_ms %= c->step_ms;
        due = (uint32_t)c->max_steps;
    }
    else
    {
        c->accum_ms -= due * c->step_ms;
    }

    c->steps = (uint8_t)due;
}

bool cgl_clock_next_step(cgl_clock_t *c)
{
    if (c == NULL)
    {
        return false;
    }
    if (c->step_index >= c->steps)
    {
        return false;
    }
    c->step_index++;
    c->step_total++;
    return true;
}

uint8_t cgl_clock_steps_left(const cgl_clock_t *c)
{
    if (c == NULL)
    {
        return 0u;
    }
    return (uint8_t)(c->steps - c->step_index);
}

uint16_t cgl_clock_alpha_q8(const cgl_clock_t *c)
{
    uint32_t q;

    if ((c == NULL) || (c->step_ms == 0u))
    {
        return 0u;
    }
    q = (c->accum_ms * 256u) / c->step_ms;
    if (q > 255u)
    {
        q = 255u;
    }
    return (uint16_t)q;
}

uint32_t cgl_clock_fps(const cgl_clock_t *c)
{
    return (c == NULL) ? 0u : c->fps;
}
