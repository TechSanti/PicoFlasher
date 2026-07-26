/*
 * Copyright (c) 2022 Balázs Triszka <balika011@gmail.com>
 * Stability and read-consensus improvements, 2026.
 *
 * GNU General Public License, version 2.
 */

#include <string.h>
#include "pico/stdlib.h"
#include "pins.h"
#include "spiex.h"
#include "xbox.h"

#define XBOX_NAND_PAGE_SIZE       0x200u
#define XBOX_NAND_SPARE_SIZE      0x10u
#define XBOX_NAND_FRAME_SIZE      (XBOX_NAND_PAGE_SIZE + XBOX_NAND_SPARE_SIZE)
#define XBOX_NAND_READ_TIMEOUT_MS 250u
#define XBOX_NAND_WRITE_TIMEOUT_MS 2000u
#define XBOX_NAND_ERASE_TIMEOUT_MS 5000u
#define XBOX_NAND_UNSTABLE_READ   0x9001
#define XBOX_FLASH_CONFIG_SAMPLES 5u
#define XBOX_FLASH_CONFIG_RECOVERY_CYCLES 3u
#define XBOX_FLASH_CONFIG_SAMPLE_DELAY_US 500u
#define XBOX_SPI_SETTLE_TIME_MS 10u

static bool flash_config_valid;
static uint32_t cached_flash_config;

void xbox_init(void)
{
	gpio_init(SMC_DBG_EN);
	gpio_put(SMC_DBG_EN, 1);
	gpio_set_dir(SMC_DBG_EN, GPIO_OUT);

	gpio_init(SMC_RST_XDK_N);
	gpio_put(SMC_RST_XDK_N, 1);
	gpio_set_dir(SMC_RST_XDK_N, GPIO_OUT);

	gpio_init(SPI_SS_N);
	gpio_put(SPI_SS_N, 1);
	gpio_set_dir(SPI_SS_N, GPIO_OUT);
}

void xbox_start_smc(void)
{
	spiex_deinit();
	flash_config_valid = false;

	gpio_put(SMC_DBG_EN, 0);
	gpio_put(SMC_RST_XDK_N, 0);
	sleep_ms(50);
	gpio_put(SMC_RST_XDK_N, 1);
}

void xbox_stop_smc(void)
{
	/*
	 * Reinitialize SPI on every recovery cycle. Repeating only the GPIO
	 * sequence can preserve stale peripheral/FIFO state until power cycling.
	 */
	spiex_deinit();
	gpio_init(SPI_SS_N);
	gpio_put(SPI_SS_N, 1);
	gpio_set_dir(SPI_SS_N, GPIO_OUT);

	gpio_put(SMC_DBG_EN, 0);
	sleep_ms(50);
	gpio_put(SPI_SS_N, 0);
	gpio_put(SMC_RST_XDK_N, 0);
	sleep_ms(50);
	gpio_put(SMC_DBG_EN, 1);
	gpio_put(SMC_RST_XDK_N, 1);
	sleep_ms(50);
	gpio_put(SPI_SS_N, 1);
	sleep_ms(50);

	flash_config_valid = false;
	if (spiex_init())
		sleep_ms(XBOX_SPI_SETTLE_TIME_MS);
}

static bool flash_config_sample_valid(uint32_t value)
{
	return value != 0 && value != UINT32_MAX;
}

static uint32_t read_flash_config_consensus(void)
{
	uint32_t samples[XBOX_FLASH_CONFIG_SAMPLES];
	for (uint i = 0; i < XBOX_FLASH_CONFIG_SAMPLES; ++i)
	{
		samples[i] = spiex_read_reg(0);
		if (flash_config_sample_valid(samples[i]))
		{
			for (uint previous = 0; previous < i; ++previous)
			{
				if (samples[previous] == samples[i])
					return samples[i];
			}
		}
		sleep_us(XBOX_FLASH_CONFIG_SAMPLE_DELAY_US);
	}
	return UINT32_MAX;
}

uint32_t xbox_get_flash_config(void)
{
	if (flash_config_valid)
		return cached_flash_config;

	for (uint cycle = 0; cycle < XBOX_FLASH_CONFIG_RECOVERY_CYCLES; ++cycle)
	{
		uint32_t consensus = read_flash_config_consensus();
		if (consensus != UINT32_MAX)
		{
			cached_flash_config = consensus;
			flash_config_valid = true;
			return consensus;
		}

		/*
		 * Automatic equivalent of reconnecting the RP2040: reset the SMC
		 * handshake, reset SPI/FIFOs, settle, and sample again.
		 */
		if (cycle + 1u < XBOX_FLASH_CONFIG_RECOVERY_CYCLES)
			xbox_stop_smc();
	}
	return UINT32_MAX;
}

static uint16_t xbox_nand_get_status(void)
{
	return (uint16_t)spiex_read_reg(0x04);
}

static void xbox_nand_clear_status(void)
{
	spiex_write_reg(0x04, spiex_read_reg(0x04));
}

static int xbox_nand_wait_ready(uint32_t timeout_ms)
{
	const uint64_t deadline = time_us_64() + (uint64_t)timeout_ms * 1000u;
	do
	{
		if (!(xbox_nand_get_status() & 0x01u))
			return 0;
		sleep_us(10);
	} while (time_us_64() < deadline);
	return 1;
}

static int xbox_nand_read_block_once(uint32_t lba, uint8_t *frame)
{
	xbox_nand_clear_status();
	spiex_write_reg(0x0C, lba << 9);
	spiex_write_reg(0x08, 0x03);

	if (xbox_nand_wait_ready(XBOX_NAND_READ_TIMEOUT_MS))
		return 0x8000 | xbox_nand_get_status();

	spiex_write_reg(0x0C, 0);
	for (size_t offset = 0; offset < XBOX_NAND_FRAME_SIZE; offset += 4)
	{
		uint32_t value;
		spiex_write_reg(0x08, 0x00);
		value = spiex_read_reg(0x10);
		memcpy(frame + offset, &value, sizeof(value));
	}
	return 0;
}

int xbox_nand_read_block(uint32_t lba, uint8_t *buffer, uint8_t *spare)
{
	static uint8_t first[XBOX_NAND_FRAME_SIZE];
	static uint8_t second[XBOX_NAND_FRAME_SIZE];
	static uint8_t third[XBOX_NAND_FRAME_SIZE];

	if (!buffer || !spare)
		return XBOX_NAND_UNSTABLE_READ;

	int rc = xbox_nand_read_block_once(lba, first);
	if (rc)
		return rc;
	rc = xbox_nand_read_block_once(lba, second);
	if (rc)
		return rc;

	const uint8_t *stable = NULL;
	if (!memcmp(first, second, sizeof(first)))
		stable = first;
	else
	{
		rc = xbox_nand_read_block_once(lba, third);
		if (rc)
			return rc;
		if (!memcmp(first, third, sizeof(first)))
			stable = first;
		else if (!memcmp(second, third, sizeof(first)))
			stable = second;
	}

	if (!stable)
		return XBOX_NAND_UNSTABLE_READ;

	memcpy(buffer, stable, XBOX_NAND_PAGE_SIZE);
	memcpy(spare, stable + XBOX_NAND_PAGE_SIZE, XBOX_NAND_SPARE_SIZE);
	return 0;
}

int xbox_nand_erase_block(uint32_t lba)
{
	xbox_nand_clear_status();
	spiex_write_reg(0x00, spiex_read_reg(0x00) | 0x08);
	spiex_write_reg(0x0C, lba << 9);
	spiex_write_reg(0x08, 0xAA);
	spiex_write_reg(0x08, 0x55);
	spiex_write_reg(0x08, 0x05);

	if (xbox_nand_wait_ready(XBOX_NAND_ERASE_TIMEOUT_MS))
		return 0x8000 | xbox_nand_get_status();
	return 0;
}

int xbox_nand_write_block(uint32_t lba, const uint8_t *buffer,
			  const uint8_t *spare)
{
	if (!buffer || !spare)
		return XBOX_NAND_UNSTABLE_READ;

	uint32_t flash_config = xbox_get_flash_config();
	if (flash_config == UINT32_MAX)
		return XBOX_NAND_UNSTABLE_READ;

	int major = (flash_config >> 17) & 3;
	int minor = (flash_config >> 4) & 3;
	int blocksize = 0x4000;
	if (major >= 1)
	{
		if (minor == 2)
			blocksize = 0x20000;
		else if (minor == 3)
			blocksize = 0x40000;
	}

	int sectors_in_block = blocksize / (int)XBOX_NAND_PAGE_SIZE;
	if (lba % (uint32_t)sectors_in_block == 0)
	{
		int rc = xbox_nand_erase_block(lba);
		if (rc)
			return rc;
	}

	xbox_nand_clear_status();
	spiex_write_reg(0x0C, 0);

	for (size_t offset = 0; offset < XBOX_NAND_PAGE_SIZE; offset += 4)
	{
		uint32_t value;
		memcpy(&value, buffer + offset, sizeof(value));
		spiex_write_reg(0x10, value);
		spiex_write_reg(0x08, 0x01);
	}
	for (size_t offset = 0; offset < XBOX_NAND_SPARE_SIZE; offset += 4)
	{
		uint32_t value;
		memcpy(&value, spare + offset, sizeof(value));
		spiex_write_reg(0x10, value);
		spiex_write_reg(0x08, 0x01);
	}

	if (xbox_nand_wait_ready(XBOX_NAND_WRITE_TIMEOUT_MS))
		return 0x8000 | xbox_nand_get_status();
	spiex_write_reg(0x0C, lba << 9);
	if (xbox_nand_wait_ready(XBOX_NAND_WRITE_TIMEOUT_MS))
		return 0x8000 | xbox_nand_get_status();

	spiex_write_reg(0x08, 0x55);
	spiex_write_reg(0x08, 0xAA);
	spiex_write_reg(0x08, 0x04);
	if (xbox_nand_wait_ready(XBOX_NAND_WRITE_TIMEOUT_MS))
		return 0x8000 | xbox_nand_get_status();
	return 0;
}
