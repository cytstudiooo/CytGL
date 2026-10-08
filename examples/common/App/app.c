// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    app.c
  * @brief   Application lifecycle: UI bring up then the frame loop.
  ******************************************************************************
  */
#include "app.h"
#include "app_ui.h"

/* The only thing this file needs from the board is the millisecond clock, and it
   asks the *port* for it rather than including a chip HAL header: App/ is the
   layer that must not know which STM32 (or which timer, or which HAL family) is
   underneath, and this file is shared by every board's demo
   (examples/common/App). */
#include "cgl_port.h"

int app_init(void)
{
    return app_ui_build();
}

void app_run(void)
{
    uint32_t last = cgl_port_board_tick_ms();

    for (;;)
    {
        uint32_t now = cgl_port_board_tick_ms();

        /* * This loop no longer computes dt: the port owns the clock
           (cgl_port_t::tick_ms, handed to the app as cgl_port_board_tick_ms())
           and cgl_port_board_frame() derives dt from it,
           scans the key with it and advances the frame. All that is left here is
           pacing -- without it the super loop would spin thousands of pointless
           frames inside one millisecond, and "one frame per millisecond" is this
           demo's frame budget.

           * dt is deliberately not clamped anywhere in the application. Stall
           protection is cgl_clock's job (CGL_CLOCK_DEFAULT_MAX_DT_MS), because it
           also has to protect the fixed step accumulator -- clamping a second
           time in the loop would only turn "who is responsible for limiting time"
           into two places' business. */
        if (now == last)
        {
            continue; /* wait for the next millisecond tick */
        }
        last = now;

        app_ui_tick();
    }
}
