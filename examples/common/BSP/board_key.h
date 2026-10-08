// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    board_key.h
  * @brief   On-board user key (PC13): debounce + short/long press state machine.
  *
  *  The pin, its pull resistor and which level means "pressed" are board wiring,
  *  so they live in the project's BSP/board.h (BOARD_KEY_PORT / BOARD_KEY_PIN /
  *  BOARD_KEY_PRESSED_LEVEL). Nothing here is board specific: this is the same
  *  state machine on every board.
  *
  *  Why "polling" instead of an EXTI interrupt
  *  ------------------------------
  *  This project has no RTOS, and the main loop is naturally speed limited by
  *  SPI bandwidth (~40..60 Hz). On that cadence a key with 20 ms of debounce
  *  is seen at most 1..2 frames late, which the eye cannot notice; an
  *  interrupt scheme would in addition have to deal with NVIC priorities,
  *  contention with the DMA interrupt, and the problem of "touching UI state
  *  inside an interrupt" -- the gain is 0 and the risk is not small.
  *
  *  * But events are still **queued first and dispatched only at a frame
  *  boundary** (cgl_screen_post_event): the key scan may happen anywhere in
  *  the loop, while the UI sees events only at a fixed point within a frame.
  *  Once those two things are separated, "handling a key press halfway through
  *  drawing" becomes impossible.
  *
  *  Usage (in the app's frame loop, before cgl_screen_frame):
  *
  *      for (;;) {
  *          uint32_t dt = ...;
  *          board_key_event_t ev = board_key_poll(dt);
  *          if (ev != BOARD_KEY_NONE) cgl_screen_post_event(&screen, &(cgl_event_t){...});
  *          cgl_screen_frame(&screen, dt);
  *      }
  *
  *  Events (the complete timeline of one key press):
  *
  *      press    DOWN --(600 ms)--> LONG --(every 200 ms)--> REPEAT --> ...
  *      release  UP
  *
  *  Short press = DOWN + UP; long press = DOWN + LONG (+ REPEAT...) + UP.
  *  Pages use these four events to implement single-key interaction such as
  *  "short press moves, long press confirms".
  ******************************************************************************
  */
#ifndef BOARD_KEY_H
#define BOARD_KEY_H

#include <stdint.h>
#include <stdbool.h>

/** The result of one scan. At most one event is returned per frame (the key's
    physical state machine does not need more). */
typedef enum
{
    BOARD_KEY_NONE = 0,  /**< no new event this frame                          */
    BOARD_KEY_DOWN,      /**< debounce confirmed a press                       */
    BOARD_KEY_LONG,      /**< held longer than BOARD_KEY_LONG_MS, sent once     */
    BOARD_KEY_REPEAT,    /**< repeat while the long press is held, once every
                              BOARD_KEY_REPEAT_MS                              */
    BOARD_KEY_UP         /**< released                                         */
} board_key_event_t;

/** Debounce window: the level must stay stable this long to be believed. */
#define BOARD_KEY_DEBOUNCE_MS 20u
/** How long a press has to be held to count as a long press. */
#define BOARD_KEY_LONG_MS 600u
/** Repeat period after a long press. */
#define BOARD_KEY_REPEAT_MS 200u

/** Reset the state machine. The pin configuration is done by CubeMX (the
    .ioc); no hardware initialisation happens here. */
void board_key_init(void);

/**
 * Scan once. **Call once per frame period**; dt_ms is the number of
 * milliseconds since the previous call.
 * @return the event that happened this frame, or BOARD_KEY_NONE
 */
board_key_event_t board_key_poll(uint32_t dt_ms);

/** Current debounced key state (true = held down). */
bool board_key_is_down(void);

/** How long the current press has lasted (milliseconds); when the key is not
    held it keeps the duration of the previous press. */
uint32_t board_key_hold_ms(void);

/** Total number of presses since power-up (for observation, and usable as a
    random seed). */
uint32_t board_key_presses(void);

/** Reset the state machine (for tests; not needed on the real target). */
void board_key_reset_state(void);

#endif /* BOARD_KEY_H */
