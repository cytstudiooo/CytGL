// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 CytStudio <cyte_studio@foxmail.com>
/**
  ******************************************************************************
  * @file    board_lcd.h
  * @brief   Board level LCD transport accessor.
  ******************************************************************************
  */
#ifndef BOARD_LCD_H
#define BOARD_LCD_H

#include "lcd_bus.h"

/** The board's SPI + DMA transport for the on-board LCD connector.
 *
 *  The instance behind it is chosen per board in BSP/board_lcd.c and described
 *  in that board's BSP/board.h (SPI1 + DMA1_Channel2 on the G431 board,
 *  SPI1 + GPDMA1 channel 0 on the H563 board); the caller only ever sees
 *  lcd_bus_t. */
const lcd_bus_t *board_lcd_bus(void);

#endif /* BOARD_LCD_H */
