/*
 * Copyright (c) 2022 Balázs Triszka <balika011@gmail.com>
 * Versão Definitiva V16: Clock eMMC unificado (12,5 MHz) para máxima estabilidade
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico/time.h"

#include "tusb.h"
#include "xbox.h"
#include "isd1200.h"
#include "sdio.h"
#include "pins.h"
#include "mmc_defs.h"

// ====================================================================
// FUNÇÕES DE TEMPO E SISTEMA DE LEDS UNIFICADO
// ====================================================================
uint32_t millis() {
    return to_ms_since_boot(get_absolute_time());
}

#ifdef BOARD_RP2040_ZERO
    #include "hardware/pio.h"
    #include "ws2812.pio.h"
    #define WS2812_PIN 16
    static PIO ws2812_pio = pio0;
    static int ws2812_sm = -1;

    static inline void put_pixel(uint32_t pixel_grb) {
        pio_sm_put_blocking(ws2812_pio, (uint)ws2812_sm, pixel_grb << 8u);
    }
#else
    #define LED_PIN 25
#endif

static uint32_t last_activity_time = 0;
static uint32_t blink_timer = 0;
static bool blink_toggle = false;

void set_led_activity() {
    last_activity_time = millis();
}

void process_led() {
    uint32_t now = millis();
    bool is_active = (now - last_activity_time < 150);

    if (is_active) {
        if (now - blink_timer > 30) {
            blink_timer = now;
            blink_toggle = !blink_toggle;
            #ifdef BOARD_RP2040_ZERO
                put_pixel(blink_toggle ? 0x00FF00 : 0x000000);
            #else
                gpio_put(LED_PIN, blink_toggle);
            #endif
        }
    } else {
        #ifdef BOARD_RP2040_ZERO
            if(now - blink_timer > 100) {
                blink_timer = now;
                put_pixel(0x0000FF);
            }
        #else
            gpio_put(LED_PIN, 1);
        #endif
    }
}
// ====================================================================

static bool smc_stopped = false;

static void ensure_smc_stopped(void) {
    if (!smc_stopped) {
        xbox_stop_smc();
        smc_stopped = true;
    }
}

void tud_mount_cb(void) {
    xbox_stop_smc();
    smc_stopped = true;
}

void tud_umount_cb(void) { /* SMC nunca é religado */ }
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; }
void tud_resume_cb(void) {
    xbox_stop_smc();
    smc_stopped = true;
}

#define GET_VERSION 0x00
#define GET_FLASH_CONFIG 0x01
#define READ_FLASH 0x02
#define WRITE_FLASH 0x03
#define READ_FLASH_STREAM 0x04

#define EMMC_DETECT 0x50
#define EMMC_INIT 0x51
#define EMMC_GET_CID 0x52
#define EMMC_GET_CSD 0x53
#define EMMC_GET_EXT_CSD 0x54
#define EMMC_READ 0x55
#define EMMC_READ_STREAM 0x56
#define EMMC_WRITE 0x57

#define ISD1200_INIT 0xA0
#define ISD1200_DEINIT 0xA1
#define ISD1200_READ_ID 0xA2
#define ISD1200_READ_FLASH 0xA3
#define ISD1200_ERASE_FLASH 0xA4
#define ISD1200_WRITE_FLASH 0xA5
#define ISD1200_PLAY_VOICE 0xA6
#define ISD1200_EXEC_MACRO 0xA7
#define ISD1200_RESET 0xA8
#define REBOOT_TO_BOOTLOADER 0xFE

#pragma pack(push, 1)
struct cmd {
    uint8_t cmd;
    uint32_t lba;
};
#pragma pack(pop)

bool emmc_detected = false;
bool stream_emmc = false;
bool do_stream = false;
uint32_t stream_offset = 0;
uint32_t stream_end = 0;

void stream() {
    if (do_stream) {
        if (stream_offset >= stream_end) {
            do_stream = false;
            return;
        }

        if (tud_cdc_write_available() < 4 + (stream_emmc ? 0x200 : 0x210))
            return;

        if (!stream_emmc) {
            static uint8_t buffer[4 + 0x210];
            uint32_t ret = xbox_nand_read_block(stream_offset, &buffer[4], &buffer[4 + 0x200]);
            *(uint32_t *)buffer = ret;
            if (ret == 0) {
                tud_cdc_write(buffer, sizeof(buffer));
                ++stream_offset;
            } else {
                tud_cdc_write(&ret, 4);
                do_stream = false;
            }
        } else {
            static uint8_t buffer[4 + 0x200];
            int ret = sd_readblocks_sync(&buffer[4], stream_offset, 1);
            *(uint32_t *)buffer = ret;
            if (ret == 0) {
                tud_cdc_write(buffer, sizeof(buffer));
                ++stream_offset;
            } else {
                tud_cdc_write(&ret, 4);
                do_stream = false;
            }
        }
    }
}

void tud_cdc_rx_cb(uint8_t itf) {
    (void)itf;
    set_led_activity();

    uint32_t avilable_data = tud_cdc_available();
    uint32_t needed_data = sizeof(struct cmd);
    {
        uint8_t cmd;
        tud_cdc_peek(&cmd);
        if (cmd == WRITE_FLASH)
            needed_data += 0x210;
        if (cmd == ISD1200_WRITE_FLASH)
            needed_data += 16;
    }

    if (avilable_data >= needed_data) {
        struct cmd cmd;
        uint32_t count = tud_cdc_read(&cmd, sizeof(cmd));
        if (count != sizeof(cmd))
            return;

        if (cmd.cmd == GET_VERSION) {
            uint32_t ver = 3;
            tud_cdc_write(&ver, 4);
        }
        else if (cmd.cmd == GET_FLASH_CONFIG) {
            uint32_t fc;
            if (emmc_detected) {
                uint32_t spi_config = xbox_get_flash_config();
                if (spi_config != 0xFFFFFFFF) {
                    emmc_detected = false;
                    fc = spi_config;
                } else {
                    fc = 0x00000000;
                }
            } else {
                ensure_smc_stopped();
                fc = xbox_get_flash_config();
            }
            tud_cdc_write(&fc, 4);
        }
        else if (cmd.cmd == READ_FLASH) {
            ensure_smc_stopped();
            uint8_t buffer[0x210];
            uint32_t ret = xbox_nand_read_block(cmd.lba, buffer, &buffer[0x200]);
            tud_cdc_write(&ret, 4);
            if (ret == 0)
                tud_cdc_write(buffer, sizeof(buffer));
        }
        else if (cmd.cmd == WRITE_FLASH) {
            ensure_smc_stopped();
            uint8_t buffer[0x210];
            uint32_t count = tud_cdc_read(&buffer, sizeof(buffer));
            if (count != sizeof(buffer)) return;
            uint32_t ret = xbox_nand_write_block(cmd.lba, buffer, &buffer[0x200]);
            tud_cdc_write(&ret, 4);
        }
        else if (cmd.cmd == READ_FLASH_STREAM) {
            ensure_smc_stopped();
            stream_emmc = false;
            do_stream = true;
            stream_offset = 0;
            stream_end = cmd.lba;
        }
        else if (cmd.cmd == ISD1200_INIT) {
            uint8_t ret = isd1200_init() ? 0 : 1;
            tud_cdc_write(&ret, 1);
        }
        else if (cmd.cmd == ISD1200_DEINIT) {
            isd1200_deinit();
            uint8_t ret = 0;
            tud_cdc_write(&ret, 1);
        }
        else if (cmd.cmd == ISD1200_READ_ID) {
            uint8_t dev_id = isd1200_read_id();
            tud_cdc_write(&dev_id, 1);
        }
        else if (cmd.cmd == ISD1200_READ_FLASH) {
            uint8_t buffer[512];
            isd1200_flash_read(cmd.lba, buffer);
            tud_cdc_write(buffer, sizeof(buffer));
        }
        else if (cmd.cmd == ISD1200_ERASE_FLASH) {
            isd1200_chip_erase();
            uint8_t ret = 0;
            tud_cdc_write(&ret, 1);
        }
        else if (cmd.cmd == ISD1200_WRITE_FLASH) {
            uint8_t buffer[16];
            uint32_t count = tud_cdc_read(&buffer, sizeof(buffer));
            if (count != sizeof(buffer)) return;
            isd1200_flash_write(cmd.lba, buffer);
            uint32_t ret = 0;
            tud_cdc_write(&ret, 4);
        }
        else if (cmd.cmd == ISD1200_PLAY_VOICE) {
            isd1200_play_vp(cmd.lba);
            uint8_t ret = 0;
            tud_cdc_write(&ret, 1);
        }
        else if (cmd.cmd == ISD1200_EXEC_MACRO) {
            isd1200_exe_vm(cmd.lba);
            uint8_t ret = 0;
            tud_cdc_write(&ret, 1);
        }
        else if (cmd.cmd == ISD1200_RESET) {
            isd1200_reset();
            uint8_t ret = 0;
            tud_cdc_write(&ret, 1);
        }
        else if (cmd.cmd == REBOOT_TO_BOOTLOADER) {
            reset_usb_boot(0, 0);
        }
        else if (cmd.cmd == EMMC_DETECT) {
            gpio_init(MMC_CMD_PIN);
            gpio_set_dir(MMC_CMD_PIN, GPIO_IN);
            gpio_pull_down(MMC_CMD_PIN);
            busy_wait_us(500);
            emmc_detected = gpio_get(MMC_CMD_PIN);
            gpio_disable_pulls(MMC_CMD_PIN);
            tud_cdc_write(&emmc_detected, 1);
        }
        else if (cmd.cmd == EMMC_INIT) {
            gpio_init(SMC_RST_XDK_N);
            gpio_set_dir(SMC_RST_XDK_N, GPIO_OUT);
            gpio_put(SMC_RST_XDK_N, 0);

            gpio_set_input_hysteresis_enabled(MMC_CMD_PIN, true);
            gpio_set_input_hysteresis_enabled(MMC_DAT0_PIN, true);

            uint32_t ret = sd_init();

            // 🔹 Clock eMMC unificado em 12,5 MHz – estabilidade máxima
            sd_set_clock_divider(10);

            gpio_set_drive_strength(MMC_CLK_PIN, GPIO_DRIVE_STRENGTH_4MA);
            gpio_set_drive_strength(MMC_CMD_PIN, GPIO_DRIVE_STRENGTH_4MA);
            gpio_set_drive_strength(MMC_DAT0_PIN, GPIO_DRIVE_STRENGTH_4MA);
            gpio_set_slew_rate(MMC_CLK_PIN, GPIO_SLEW_RATE_SLOW);
            gpio_set_slew_rate(MMC_CMD_PIN, GPIO_SLEW_RATE_SLOW);
            gpio_set_slew_rate(MMC_DAT0_PIN, GPIO_SLEW_RATE_SLOW);
            gpio_pull_up(MMC_CMD_PIN);
            gpio_pull_up(MMC_DAT0_PIN);
            gpio_disable_pulls(MMC_CLK_PIN);

            tud_cdc_write(&ret, 4);
        }
        else if (cmd.cmd == EMMC_GET_CID) {
            uint8_t cid_raw[16];
            sd_read_cid(cid_raw);
            tud_cdc_write(cid_raw, sizeof(cid_raw));
        }
        else if (cmd.cmd == EMMC_GET_CSD) {
            uint8_t csd_raw[16];
            sd_read_csd(csd_raw);
            tud_cdc_write(csd_raw, sizeof(csd_raw));
        }
        else if (cmd.cmd == EMMC_GET_EXT_CSD) {
            uint8_t ext_csd[512];
            sd_read_ext_csd(ext_csd);
            tud_cdc_write(ext_csd, sizeof(ext_csd));
        }
        else if (cmd.cmd == EMMC_READ) {
            uint8_t buffer[0x200];
            int ret = sd_readblocks_sync(buffer, cmd.lba, 1);
            tud_cdc_write(&ret, 4);
            if (ret == 0)
                tud_cdc_write(buffer, sizeof(buffer));
        }
        else if (cmd.cmd == EMMC_READ_STREAM) {
            stream_emmc = true;
            do_stream = true;
            stream_offset = 0;
            stream_end = cmd.lba;
        }
        else if (cmd.cmd == EMMC_WRITE) {
            uint8_t buffer[0x200];
            uint32_t count = tud_cdc_read(&buffer, sizeof(buffer));
            if (count != sizeof(buffer)) return;
            uint32_t ret = sd_writeblocks_sync(buffer, cmd.lba, 1);
            tud_cdc_write(&ret, 4);
        }

        tud_cdc_write_flush();
    }
}

void tud_cdc_tx_complete_cb(uint8_t itf) {
    (void)itf;
    set_led_activity();
}

int main(void) {
    uint32_t freq = clock_get_hz(clk_sys);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, freq, freq);

    stdio_init_all();

    #ifdef BOARD_RP2040_ZERO
        ws2812_sm = pio_claim_unused_sm(ws2812_pio, true);
        uint offset = pio_add_program(ws2812_pio, &ws2812_program);
        ws2812_program_init(ws2812_pio, (uint)ws2812_sm, offset, WS2812_PIN, 800000, false);
    #else
        gpio_init(LED_PIN);
        gpio_set_dir(LED_PIN, GPIO_OUT);
    #endif

    xbox_init();

    #ifdef BOARD_RP2040_ZERO
        gpio_pull_up(SPI_MISO); 
        gpio_pull_up(SPI_MOSI); 
        gpio_set_function(SPI_MISO, GPIO_FUNC_SPI);
        gpio_set_function(SPI_CLK,  GPIO_FUNC_SPI);
        gpio_set_function(SPI_MOSI, GPIO_FUNC_SPI);
    #endif

    tusb_init();

    while (1) {
        tud_task();
        stream();
        process_led();
    }

    return 0;
}