/*
 * Copyright (c) 2022 Balázs Triszka <balika011@gmail.com>
 * Corona 16 MB / 4 GB stability edition, 2026.
 *
 * The USB command numbers, packet sizes and reported protocol version remain
 * compatible with PicoFlasher 3/J-Runner.
 *
 * GNU General Public License, version 2.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hardware/gpio.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "isd1200.h"
#include "mmc_defs.h"
#include "pins.h"
#include "sdio.h"
#include "xbox.h"

#ifdef BOARD_RP2040_ZERO
#include "hardware/pio.h"
#include "ws2812.pio.h"
#define WS2812_PIN 16
static PIO ws2812_pio = pio0;
static int ws2812_sm = -1;
#else
#define LED_PIN 25
#endif

#define GET_VERSION          0x00
#define GET_FLASH_CONFIG     0x01
#define READ_FLASH           0x02
#define WRITE_FLASH          0x03
#define READ_FLASH_STREAM    0x04

#define EMMC_DETECT          0x50
#define EMMC_INIT            0x51
#define EMMC_GET_CID         0x52
#define EMMC_GET_CSD         0x53
#define EMMC_GET_EXT_CSD     0x54
#define EMMC_READ            0x55
#define EMMC_READ_STREAM     0x56
#define EMMC_WRITE           0x57

#define ISD1200_INIT         0xA0
#define ISD1200_DEINIT       0xA1
#define ISD1200_READ_ID      0xA2
#define ISD1200_READ_FLASH   0xA3
#define ISD1200_ERASE_FLASH  0xA4
#define ISD1200_WRITE_FLASH  0xA5
#define ISD1200_PLAY_VOICE   0xA6
#define ISD1200_EXEC_MACRO   0xA7
#define ISD1200_RESET        0xA8
#define REBOOT_TO_BOOTLOADER 0xFE

#define NAND_FRAME_SIZE (0x200u + 0x10u)

#pragma pack(push, 1)
struct cmd
{
	uint8_t cmd;
	uint32_t lba;
};
#pragma pack(pop)

static bool emmc_detected;
static bool stream_emmc;
static bool do_stream;
static bool smc_stopped;
static uint32_t stream_offset;
static uint32_t stream_end;
static uint32_t last_activity_time;
static uint32_t blink_timer;
static bool blink_toggle;
static uint8_t command_buffer[NAND_FRAME_SIZE] __attribute__((aligned(4)));

static uint32_t millis(void)
{
	return to_ms_since_boot(get_absolute_time());
}

#ifdef BOARD_RP2040_ZERO
static void put_pixel(uint32_t pixel_grb)
{
	if (ws2812_sm >= 0)
		pio_sm_put_blocking(ws2812_pio, (uint)ws2812_sm,
				    pixel_grb << 8u);
}
#endif

static void set_led_activity(void)
{
	last_activity_time = millis();
}

static void process_led(void)
{
	uint32_t now = millis();
	bool active = now - last_activity_time < 150u;
	if (active)
	{
		if (now - blink_timer > 30u)
		{
			blink_timer = now;
			blink_toggle = !blink_toggle;
#ifdef BOARD_RP2040_ZERO
			put_pixel(blink_toggle ? 0x00FF00u : 0);
#else
			gpio_put(LED_PIN, blink_toggle);
#endif
		}
	}
	else
	{
#ifdef BOARD_RP2040_ZERO
		if (now - blink_timer > 100u)
		{
			blink_timer = now;
			put_pixel(0x0000FFu);
		}
#else
		gpio_put(LED_PIN, 1);
#endif
	}
}

static void ensure_smc_stopped(void)
{
	if (!smc_stopped)
	{
		xbox_stop_smc();
		smc_stopped = true;
	}
}

static void cdc_write_u32(uint32_t value)
{
	(void)tud_cdc_write(&value, sizeof(value));
}

static void cdc_write_result(bool success)
{
	uint8_t result = success ? 0 : 1;
	(void)tud_cdc_write(&result, sizeof(result));
}

static int ensure_emmc_initialized(void)
{
	if (sd_is_initialized())
		return SD_OK;

	ensure_smc_stopped();
	gpio_put(SMC_RST_XDK_N, 0);

	int result = sd_init();
	if (result)
	{
		/* One complete retry handles a card left out of phase by a previous
		 * interrupted stream or USB session. */
		sleep_ms(10);
		result = sd_init();
	}
	return result;
}

static int read_emmc_sector(void *buffer, uint32_t sector)
{
	int result = ensure_emmc_initialized();
	if (result)
		return result;
	return sd_readblocks_sync(buffer, sector, 1);
}

void tud_mount_cb(void)
{
	ensure_smc_stopped();
}

void tud_umount_cb(void)
{
	/*
	 * Force a fresh SMC/SPI handshake if USB reconnects without removing
	 * power from the RP2040.
	 */
	smc_stopped = false;
}

void tud_suspend_cb(bool remote_wakeup_en)
{
	(void)remote_wakeup_en;
}

void tud_resume_cb(void)
{
	ensure_smc_stopped();
}

static void stream_next_block(void)
{
	if (!do_stream)
		return;
	if (stream_offset >= stream_end)
	{
		do_stream = false;
		return;
	}

	const uint32_t payload_size = stream_emmc ? 0x200u : NAND_FRAME_SIZE;
	if (tud_cdc_write_available() < sizeof(uint32_t) + payload_size)
		return;

	if (stream_emmc)
	{
		static uint8_t packet[sizeof(uint32_t) + 0x200u]
			__attribute__((aligned(4)));
		int result = read_emmc_sector(&packet[sizeof(uint32_t)],
					      stream_offset);
		memcpy(packet, &result, sizeof(result));
		if (!result)
		{
			(void)tud_cdc_write(packet, sizeof(packet));
			++stream_offset;
		}
		else
		{
			(void)tud_cdc_write(packet, sizeof(result));
			do_stream = false;
		}
	}
	else
	{
		static uint8_t packet[sizeof(uint32_t) + NAND_FRAME_SIZE]
			__attribute__((aligned(4)));
		int result = xbox_nand_read_block(
			stream_offset, &packet[sizeof(uint32_t)],
			&packet[sizeof(uint32_t) + 0x200u]);
		memcpy(packet, &result, sizeof(result));
		if (!result)
		{
			(void)tud_cdc_write(packet, sizeof(packet));
			++stream_offset;
		}
		else
		{
			(void)tud_cdc_write(packet, sizeof(result));
			do_stream = false;
		}
	}

	/* Streaming runs outside the RX callback, so it must flush explicitly. */
	(void)tud_cdc_write_flush();
	set_led_activity();
}

static uint32_t command_payload_size(uint8_t command)
{
	switch (command)
	{
	case WRITE_FLASH:
		return NAND_FRAME_SIZE;
	case EMMC_WRITE:
		return SD_SECTOR_SIZE;
	case ISD1200_WRITE_FLASH:
		return 16;
	default:
		return 0;
	}
}

static bool detect_emmc_cmd_line(void)
{
	gpio_init(MMC_CMD_PIN);
	gpio_set_function(MMC_CMD_PIN, GPIO_FUNC_SIO);
	gpio_set_dir(MMC_CMD_PIN, GPIO_IN);
	gpio_pull_down(MMC_CMD_PIN);
	busy_wait_us_32(500);

	uint high_samples = 0;
	for (uint i = 0; i < 7; ++i)
	{
		high_samples += gpio_get(MMC_CMD_PIN) ? 1u : 0u;
		busy_wait_us_32(50);
	}
	gpio_disable_pulls(MMC_CMD_PIN);
	return high_samples >= 5;
}

static void execute_command(const struct cmd *command)
{
	switch (command->cmd)
	{
	case GET_VERSION:
		cdc_write_u32(3);
		break;

	case GET_FLASH_CONFIG:
	{
		uint32_t flash_config;
		if (emmc_detected)
		{
			uint32_t spi_config = xbox_get_flash_config();
			if (spi_config != UINT32_MAX)
			{
				emmc_detected = false;
				flash_config = spi_config;
			}
			else
				flash_config = 0;
		}
		else
		{
			ensure_smc_stopped();
			flash_config = xbox_get_flash_config();
		}
		cdc_write_u32(flash_config);
		break;
	}

	case READ_FLASH:
	{
		ensure_smc_stopped();
		int result = xbox_nand_read_block(command->lba, command_buffer,
						  &command_buffer[0x200]);
		cdc_write_u32((uint32_t)result);
		if (!result)
			(void)tud_cdc_write(command_buffer, NAND_FRAME_SIZE);
		break;
	}

	case WRITE_FLASH:
	{
		ensure_smc_stopped();
		if (tud_cdc_read(command_buffer, NAND_FRAME_SIZE) !=
		    NAND_FRAME_SIZE)
			break;
		int result = xbox_nand_write_block(
			command->lba, command_buffer, &command_buffer[0x200]);
		cdc_write_u32((uint32_t)result);
		break;
	}

	case READ_FLASH_STREAM:
		ensure_smc_stopped();
		stream_emmc = false;
		do_stream = true;
		stream_offset = 0;
		stream_end = command->lba;
		break;

	case ISD1200_INIT:
		cdc_write_result(isd1200_init());
		break;
	case ISD1200_DEINIT:
		isd1200_deinit();
		cdc_write_result(true);
		break;
	case ISD1200_READ_ID:
	{
		uint8_t id = isd1200_read_id();
		(void)tud_cdc_write(&id, sizeof(id));
		break;
	}
	case ISD1200_READ_FLASH:
	{
		memset(command_buffer, 0, 512);
		(void)isd1200_flash_read(command->lba, command_buffer);
		(void)tud_cdc_write(command_buffer, 512);
		break;
	}
	case ISD1200_ERASE_FLASH:
		cdc_write_result(isd1200_chip_erase());
		break;
	case ISD1200_WRITE_FLASH:
	{
		if (tud_cdc_read(command_buffer, 16) != 16)
			break;
		cdc_write_u32(
			isd1200_flash_write(command->lba, command_buffer) ? 0u : 1u);
		break;
	}
	case ISD1200_PLAY_VOICE:
		cdc_write_result(isd1200_play_vp((uint16_t)command->lba));
		break;
	case ISD1200_EXEC_MACRO:
		cdc_write_result(isd1200_exe_vm((uint16_t)command->lba));
		break;
	case ISD1200_RESET:
		cdc_write_result(isd1200_reset());
		break;

	case EMMC_DETECT:
		emmc_detected = detect_emmc_cmd_line();
		(void)tud_cdc_write(&emmc_detected, sizeof(emmc_detected));
		break;

	case EMMC_INIT:
	{
		ensure_smc_stopped();
		gpio_put(SMC_RST_XDK_N, 0);
		int result = sd_init();
		emmc_detected = result == SD_OK;
		cdc_write_u32((uint32_t)result);
		break;
	}

	case EMMC_GET_CID:
	{
		uint8_t cid[16] = {0};
		if (sd_is_initialized())
			sd_read_cid(cid);
		(void)tud_cdc_write(cid, sizeof(cid));
		break;
	}
	case EMMC_GET_CSD:
	{
		uint8_t csd[16] = {0};
		if (sd_is_initialized())
			sd_read_csd(csd);
		(void)tud_cdc_write(csd, sizeof(csd));
		break;
	}
	case EMMC_GET_EXT_CSD:
	{
		memset(command_buffer, 0, SD_SECTOR_SIZE);
		if (!ensure_emmc_initialized())
			(void)sd_read_ext_csd(command_buffer);
		(void)tud_cdc_write(command_buffer, SD_SECTOR_SIZE);
		break;
	}
	case EMMC_READ:
	{
		int result = read_emmc_sector(command_buffer, command->lba);
		cdc_write_u32((uint32_t)result);
		if (!result)
			(void)tud_cdc_write(command_buffer, SD_SECTOR_SIZE);
		break;
	}
	case EMMC_READ_STREAM:
		stream_emmc = true;
		do_stream = true;
		stream_offset = 0;
		stream_end = command->lba;
		break;
	case EMMC_WRITE:
	{
		if (tud_cdc_read(command_buffer, SD_SECTOR_SIZE) !=
		    SD_SECTOR_SIZE)
			break;
		int result = sd_writeblocks_sync(command_buffer, command->lba, 1);
		cdc_write_u32((uint32_t)result);
		break;
	}

	case REBOOT_TO_BOOTLOADER:
		(void)tud_cdc_write_flush();
		reset_usb_boot(0, 0);
		break;
	default:
		break;
	}
}

void tud_cdc_rx_cb(uint8_t itf)
{
	(void)itf;
	set_led_activity();

	/*
	 * A write command is processed only after its entire payload is already
	 * buffered.  This fixes the old EMMC_WRITE packet desynchronization.
	 */
	while (tud_cdc_available() >= sizeof(struct cmd))
	{
		uint8_t command_byte;
		if (!tud_cdc_peek(&command_byte))
			return;
		uint32_t required = sizeof(struct cmd) +
				    command_payload_size(command_byte);
		if (tud_cdc_available() < required)
			return;

		struct cmd command;
		if (tud_cdc_read(&command, sizeof(command)) != sizeof(command))
			return;

		execute_command(&command);
		(void)tud_cdc_write_flush();
	}
}

void tud_cdc_tx_complete_cb(uint8_t itf)
{
	(void)itf;
	set_led_activity();
}

int main(void)
{
	stdio_init_all();

#ifdef BOARD_RP2040_ZERO
	ws2812_sm = pio_claim_unused_sm(ws2812_pio, false);
	if (ws2812_sm >= 0 &&
	    pio_can_add_program(ws2812_pio, &ws2812_program))
	{
		uint offset = pio_add_program(ws2812_pio, &ws2812_program);
		ws2812_program_init(ws2812_pio, (uint)ws2812_sm, offset,
				    WS2812_PIN, 800000, false);
	}
	else if (ws2812_sm >= 0)
	{
		pio_sm_unclaim(ws2812_pio, (uint)ws2812_sm);
		ws2812_sm = -1;
	}
#else
	gpio_init(LED_PIN);
	gpio_set_dir(LED_PIN, GPIO_OUT);
#endif

	xbox_init();
	/*
	 * Prepare NAND before USB enumeration. The PC may issue
	 * GET_FLASH_CONFIG immediately after opening CDC.
	 */
	xbox_stop_smc();
	smc_stopped = true;
	tusb_init();

	while (true)
	{
		tud_task();
		stream_next_block();
		process_led();
		tight_loop_contents();
	}
}
