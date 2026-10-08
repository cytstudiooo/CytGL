// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    board_key.c
  * @brief   On-board user key (PC13): debounce + short/long press state machine.
  *
  *  The state machine has only two levels (stable / raw_last) and three
  *  timers, all advanced by poll(dt_ms) -- no interrupts, no HAL_Delay, no
  *  blocking of the main loop.
  *
  *  PC13's electrical wiring and pin configuration both live in the project's
  *  .ioc (the pull resistor and the active level are board wiring) and are
  *  configured by the CubeMX-generated MX_GPIO_Init(); BOARD_KEY_PRESSED_LEVEL
  *  in the project's BSP/board.h carries the polarity. This file only handles
  *  the state machine, so it is the same code on every board.
  ******************************************************************************
  */
#include "board_key.h"
#include "board.h"

typedef struct
{
    uint8_t stable;     /**< debounced state: 1 = held down                    */
    uint8_t raw_last;   /**< the previous raw level                            */
    uint32_t raw_ms;    /**< how long the current raw level has been held      */
    uint32_t hold_ms;   /**< time since the debounced press was confirmed      */
    uint32_t repeat_ms; /**< time since the last repeat                        */
    uint8_t long_sent;  /**< LONG has already been sent (once per press)       */
    uint32_t presses;
} key_state_t;

static key_state_t s_key;

void board_key_reset_state(void)
{
    s_key.stable = 0u;
    s_key.raw_last = 0u;
    s_key.raw_ms = 0u;
    s_key.hold_ms = 0u;
    s_key.repeat_ms = 0u;
    s_key.long_sent = 0u;
    s_key.presses = 0u;
}

void board_key_init(void)
{
    /* PC13 has already been configured by the CubeMX-generated MX_GPIO_Init()
       (see the .ioc, and the schematic note in the project's BSP/board.h); this
       does not touch any pin configuration, it only resets the state machine. */
    board_key_reset_state();
}

/** Raw level: 1 = held down. */
static uint8_t key__raw(void)
{
    return (HAL_GPIO_ReadPin(BOARD_KEY_PORT, BOARD_KEY_PIN) == BOARD_KEY_PRESSED_LEVEL) ? 1u : 0u;
}

board_key_event_t board_key_poll(uint32_t dt_ms)
{
    board_key_event_t ev = BOARD_KEY_NONE;
    uint8_t raw = key__raw();

    /* --- 1) debounce: the level has to stay stable for BOARD_KEY_DEBOUNCE_MS
       before it is believed ------------------------------------------------
       This uses "hold duration" rather than "sample count", so when the upper
       layer drops the frame rate from 60 to 20 the debounce window is still
       20 ms and no retuning is needed. */
    if (raw == s_key.raw_last)
    {
        s_key.raw_ms += dt_ms;
    }
    else
    {
        s_key.raw_last = raw;
        s_key.raw_ms = 0u;
    }

    if ((s_key.raw_ms >= BOARD_KEY_DEBOUNCE_MS) && (s_key.stable != raw))
    {
        s_key.stable = raw;
        if (raw != 0u)
        {
            s_key.presses++;
            s_key.hold_ms = 0u;
            s_key.repeat_ms = 0u;
            s_key.long_sent = 0u;
            ev = BOARD_KEY_DOWN;
        }
        else
        {
            ev = BOARD_KEY_UP;
        }
    }

    /* --- 2) timing while held: long press + repeat ------------------------- */
    if (s_key.stable != 0u)
    {
        s_key.hold_ms += dt_ms;

        if (s_key.long_sent == 0u)
        {
            if (s_key.hold_ms >= BOARD_KEY_LONG_MS)
            {
                s_key.long_sent = 1u;
                s_key.repeat_ms = 0u;
                ev = BOARD_KEY_LONG;
            }
        }
        else
        {
            s_key.repeat_ms += dt_ms;
            if (s_key.repeat_ms >= BOARD_KEY_REPEAT_MS)
            {
                s_key.repeat_ms = 0u;
                ev = BOARD_KEY_REPEAT;
            }
        }
    }

    return ev;
}

bool board_key_is_down(void)
{
    return s_key.stable != 0u;
}

uint32_t board_key_hold_ms(void)
{
    return s_key.hold_ms;
}

uint32_t board_key_presses(void)
{
    return s_key.presses;
}
