/*
 * Copyright (c) 2022 Balázs Triszka <balika011@gmail.com>
 * Stability and error-handling improvements, 2026.
 *
 * GNU General Public License, version 2.
 */

#include <limits.h>
#include <string.h>
#include "pico/stdlib.h"
#include "isd1200.h"
#include "nuvuton_spi.h"

#define CMD_PLAY_VP       0xA6
#define CMD_EXE_VM        0xB0
#define CMD_READ_STATUS   0x40
#define CMD_READ_INT      0x46
#define CMD_READ_ID       0x48
#define CMD_DIG_READ      0xA2
#define CMD_DIG_WRITE     0xA0
#define CMD_CHIP_ERASE    0x26
#define CMD_PWR_UP        0x10
#define CMD_PWR_DN        0x12
#define CMD_RESET         0x14

#define ISD_READY_TIMEOUT_US   2000000ull
#define ISD_COMMAND_TIMEOUT_US 5000000ull
#define ISD_ERASE_TIMEOUT_US   120000000ull

#define ISD_INTERRUPT_ERRORS \
	(INTERRUPT_STATUS_OVF_ERR | INTERRUPT_STATUS_CMD_ERR | \
	 INTERRUPT_STATUS_MPT_ERR)

static uint8_t dev_id;
static bool bus_ready;
static bool device_ready;

static bool isd_transfer(uint8_t *buffer, size_t length)
{
	return bus_ready && nuvoton_spi_transfer(buffer, (uint32_t)length);
}

static bool wait_status(uint8_t set_mask, uint8_t clear_mask, uint64_t timeout_us)
{
	const uint64_t deadline = time_us_64() + timeout_us;
	while (time_us_64() < deadline)
	{
		uint8_t status = isd1200_read_status();
		if ((status & set_mask) == set_mask && !(status & clear_mask))
			return true;
		tight_loop_contents();
	}
	return false;
}

bool isd1200_init(void)
{
	if (bus_ready)
		isd1200_deinit();

	dev_id = 0;
	device_ready = false;
	bus_ready = nuvoton_spi_init();
	if (!bus_ready || !isd1200_power_up())
		goto fail;

	uint8_t buffer[] = {CMD_READ_ID, 0, 0, 0, 0};
	if (!isd_transfer(buffer, sizeof(buffer)))
		goto fail;

	/* PART_ID, manufacturer (Winbond) and memory type from the ISD2100. */
	if (buffer[1] != 0x03 || buffer[2] != 0xEF || buffer[3] != 0x20)
		goto fail;

	dev_id = buffer[4];
	device_ready = true;
	return true;

fail:
	if (bus_ready)
	{
		(void)isd1200_power_down();
		nuvoton_spi_deinit();
	}
	bus_ready = false;
	return false;
}

void isd1200_deinit(void)
{
	if (!bus_ready)
		return;
	(void)isd1200_power_down();
	nuvoton_spi_deinit();
	bus_ready = false;
	device_ready = false;
}

bool isd1200_power_up(void)
{
	uint8_t buffer[] = {CMD_PWR_UP};
	if (!isd_transfer(buffer, sizeof(buffer)))
		return false;

	/* DBUF_RDY must become set and the voice macro engine must be idle. */
	return wait_status(STATUS_DBUF_RDY, STATUS_VM_BSY,
			   ISD_READY_TIMEOUT_US);
}

uint8_t isd1200_read_status(void)
{
	uint8_t buffer[] = {CMD_READ_STATUS, 0};
	if (!isd_transfer(buffer, sizeof(buffer)))
		return 0;
	return buffer[0];
}

uint8_t isd1200_read_interrupt_status(void)
{
	uint8_t buffer[] = {CMD_READ_INT, 0};
	if (!isd_transfer(buffer, sizeof(buffer)))
		return ISD_INTERRUPT_ERRORS;
	return buffer[1];
}

bool isd1200_power_down(void)
{
	uint8_t buffer[] = {CMD_PWR_DN};
	return isd_transfer(buffer, sizeof(buffer));
}

bool isd1200_reset(void)
{
	uint8_t buffer[] = {CMD_RESET};
	if (!device_ready || !isd_transfer(buffer, sizeof(buffer)))
		return false;
	return wait_status(STATUS_DBUF_RDY, STATUS_CMD_BSY | STATUS_VM_BSY,
			   ISD_READY_TIMEOUT_US);
}

uint8_t isd1200_read_id(void)
{
	return dev_id;
}

bool isd1200_play_vp(uint16_t index)
{
	uint8_t buffer[] = {CMD_PLAY_VP, (uint8_t)(index >> 8), (uint8_t)index};
	return device_ready && isd_transfer(buffer, sizeof(buffer));
}

bool isd1200_exe_vm(uint16_t index)
{
	uint8_t buffer[] = {CMD_EXE_VM, (uint8_t)(index >> 8), (uint8_t)index};
	return device_ready && isd_transfer(buffer, sizeof(buffer));
}

bool isd1200_flash_read(uint32_t page, uint8_t *output)
{
	if (!device_ready || !output || page > UINT32_MAX / 512u)
		return false;

	const uint32_t address = page * 512u;
	uint8_t buffer[1 + 3 + 512] = {
		CMD_DIG_READ,
		(uint8_t)(address >> 16),
		(uint8_t)(address >> 8),
		(uint8_t)address
	};

	if (!isd_transfer(buffer, sizeof(buffer)))
		return false;

	/*
	 * DIG_READ has exactly three address bytes.  The first data byte is
	 * buffer[4]; using buffer[5] loses one byte and reads one past the frame.
	 */
	memcpy(output, &buffer[4], 512);
	return true;
}

bool isd1200_chip_erase(void)
{
	if (!device_ready)
		return false;

	(void)isd1200_read_interrupt_status();
	uint8_t buffer[] = {CMD_CHIP_ERASE, 0x01};
	if (!isd_transfer(buffer, sizeof(buffer)))
		return false;

	if (!wait_status(0, STATUS_CMD_BSY, ISD_ERASE_TIMEOUT_US))
		return false;

	return !(isd1200_read_interrupt_status() & ISD_INTERRUPT_ERRORS);
}

bool isd1200_flash_write(uint32_t page, const uint8_t *input)
{
	if (!device_ready || !input || page > UINT32_MAX / 16u)
		return false;

	const uint32_t address = page * 16u;
	uint8_t buffer[1 + 3 + 16] = {
		CMD_DIG_WRITE,
		(uint8_t)(address >> 16),
		(uint8_t)(address >> 8),
		(uint8_t)address
	};
	memcpy(&buffer[4], input, 16);

	/* Clear stale completion/error flags before issuing the write. */
	(void)isd1200_read_interrupt_status();
	if (!isd_transfer(buffer, sizeof(buffer)) ||
	    !wait_status(0, STATUS_CMD_BSY, ISD_COMMAND_TIMEOUT_US))
		return false;

	const uint64_t deadline = time_us_64() + ISD_COMMAND_TIMEOUT_US;
	while (time_us_64() < deadline)
	{
		uint8_t interrupt = isd1200_read_interrupt_status();
		if (interrupt & ISD_INTERRUPT_ERRORS)
			return false;
		if (interrupt & INTERRUPT_STATUS_WR_FIN)
			return true;
		tight_loop_contents();
	}
	return false;
}
