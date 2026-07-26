/*
 * Copyright (c) 2022 Balázs Triszka <balika011@gmail.com>
 * Modificado para Sistema Híbrido (Pico Padrão / Pico Zero)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#ifndef __PINS_H__
#define __PINS_H__

// O compilador vai verificar se a placa escolhida é o RP2040-Zero
#ifdef BOARD_RP2040_ZERO

    // ==========================================
    // PINAGEM PARA O RP2040-ZERO
    // ==========================================
    
    // Pinos para 16MB (SPI)
    #define SPI_MISO 0
    #define SPI_SS_N 1
    #define SPI_CLK 2
    #define SPI_MOSI 3
    #define SMC_DBG_EN 4
    #define SMC_RST_XDK_N 5

    // Pinos para 4GB (eMMC / SDIO)
    #define MMC_DAT0_PIN 26
    #define MMC_CMD_PIN 27
    #define MMC_CLK_PIN 28
    #define MMC_RST_PIN 29

#else

    // ==========================================
    // PINAGEM PARA O RASPBERRY PI PICO PADRÃO
    // ==========================================
    
    // Pinos para 16MB (SPI)
    #define SPI_MISO 16
    #define SPI_SS_N 17
    #define SPI_CLK 18
    #define SPI_MOSI 19
    #define SMC_DBG_EN 20
    #define SMC_RST_XDK_N 21

    // Pinos para 4GB (eMMC / SDIO)
    #define MMC_DAT0_PIN 6
    #define MMC_CMD_PIN 7
    #define MMC_CLK_PIN 8
    #define MMC_RST_PIN 9

#endif

// ==========================================
// PINOS DO CHIP DE ÁUDIO (Nuvoton/Sonus 360)
// Mantidos como padrão para ambos
// ==========================================
#define NUVOTON_SPI_RDY 11  // FT2V4
#define NUVOTON_SPI_MISO 12 // FT2R7
#define NUVOTON_SPI_SS_N 13 // FT2R6
#define NUVOTON_SPI_CLK 14  // FT2T4
#define NUVOTON_SPI_MOSI 15 // FT2T5

#endif