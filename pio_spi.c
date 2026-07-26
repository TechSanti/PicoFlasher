/*
 * Copyright (c) 2022 Balázs Triszka <balika011@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pio_spi.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"

#define PIO_SPI_TIMEOUT_US 1000000u

// Just 8 bit functions provided here. The PIO program supports any frame size
// 1...32, but the software to do the necessary FIFO shuffling is left as an
// exercise for the reader :)
//
// Likewise we only provide MSB-first here. To do LSB-first, you need to
// - Do shifts when reading from the FIFO, for general case n != 8, 16, 32
// - Do a narrow read at a one halfword or 3 byte offset for n == 16, 8
// in order to get the read data correctly justified.

bool pio_spi_init(pio_spi_inst_t *spi, PIO pio, int requested_sm,
				  uint32_t freq_hz, uint n_bits, pio_spi_order_t order,
				  bool cpha, bool cpol, uint pin_ss, uint pin_mosi,
				  uint pin_miso)
{
	if (!spi || !freq_hz || n_bits < 2 || n_bits > 32)
		return false;

	int sm = requested_sm;
	if (sm < 0)
		sm = pio_claim_unused_sm(pio, false);
	else if (pio_sm_is_claimed(pio, (uint)sm))
		return false;
	else
		pio_sm_claim(pio, (uint)sm);

	if (sm < 0)
		return false;

	const pio_program_t *program = cpha ? &spi_cpha1_cs_program
						   : &spi_cpha0_cs_program;
	if (!pio_can_add_program(pio, program))
	{
		pio_sm_unclaim(pio, (uint)sm);
		return false;
	}

	spi->pio = pio;
	spi->sm = (uint)sm;
	spi->order = order;
	spi->cpha = cpha;
	spi->initialized = false;

	spi->prog = pio_add_program(spi->pio, program);

	/* Each SPI bit consumes four PIO clock cycles in spi.pio. */
	float clockdiv = (float)clock_get_hz(clk_sys) / (4.0f * (float)freq_hz);
	if (clockdiv < 1.0f)
		clockdiv = 1.0f;

	pio_spi_cs_init(pio, (uint)sm, spi->prog, n_bits, clockdiv, cpha, cpol,
			pin_ss, pin_mosi, pin_miso, order);
	spi->initialized = true;
	return true;
}

void pio_spi_deinit(pio_spi_inst_t *spi)
{
	if (!spi || !spi->initialized)
		return;

	pio_sm_set_enabled(spi->pio, spi->sm, false);
	pio_sm_clear_fifos(spi->pio, spi->sm);
	pio_remove_program(spi->pio,
			   spi->cpha ? &spi_cpha1_cs_program : &spi_cpha0_cs_program,
			   spi->prog);
	pio_sm_unclaim(spi->pio, spi->sm);
	spi->initialized = false;
}

static bool pio_spi_timed_out(uint64_t deadline)
{
	return time_us_64() >= deadline;
}

bool __time_critical_func(pio_spi_write8_blocking)(const pio_spi_inst_t *spi, const uint8_t *src, size_t len)
{
	if (!spi || !spi->initialized || (!src && len))
		return false;

	size_t tx_remain = len, rx_remain = len;
	uint64_t deadline = time_us_64() + PIO_SPI_TIMEOUT_US;
	// Do 8 bit accesses on FIFO, so that write data is byte-replicated. This
	// gets us the left-justification for free (for MSB-first shift-out)
	io_rw_8 *txfifo = (io_rw_8 *)&spi->pio->txf[spi->sm];
	io_rw_8 *rxfifo = (io_rw_8 *)&spi->pio->rxf[spi->sm];
	while (tx_remain || rx_remain)
	{
		bool progressed = false;
		if (tx_remain && !pio_sm_is_tx_fifo_full(spi->pio, spi->sm))
		{
			*txfifo = *src++;
			--tx_remain;
			progressed = true;
		}
		if (rx_remain && !pio_sm_is_rx_fifo_empty(spi->pio, spi->sm))
		{
			(void)*rxfifo;
			--rx_remain;
			progressed = true;
		}
		if (progressed)
			deadline = time_us_64() + PIO_SPI_TIMEOUT_US;
		else if (pio_spi_timed_out(deadline))
			return false;
	}
	return true;
}

bool __time_critical_func(pio_spi_read8_blocking)(const pio_spi_inst_t *spi, uint8_t *dst, size_t len)
{
	if (!spi || !spi->initialized || (!dst && len))
		return false;

	size_t tx_remain = len, rx_remain = len;
	uint64_t deadline = time_us_64() + PIO_SPI_TIMEOUT_US;
	io_rw_8 *txfifo = (io_rw_8 *)&spi->pio->txf[spi->sm];
	io_rw_8 *rxfifo = (io_rw_8 *)&spi->pio->rxf[spi->sm];
	if (spi->order == SPI_LSB_FIRST)
		rxfifo += 3;
	while (tx_remain || rx_remain)
	{
		bool progressed = false;
		if (tx_remain && !pio_sm_is_tx_fifo_full(spi->pio, spi->sm))
		{
			*txfifo = 0;
			--tx_remain;
			progressed = true;
		}
		if (rx_remain && !pio_sm_is_rx_fifo_empty(spi->pio, spi->sm))
		{
			*dst++ = *rxfifo;
			--rx_remain;
			progressed = true;
		}
		if (progressed)
			deadline = time_us_64() + PIO_SPI_TIMEOUT_US;
		else if (pio_spi_timed_out(deadline))
			return false;
	}
	return true;
}

bool __time_critical_func(pio_spi_write8_read8_blocking)(const pio_spi_inst_t *spi,
							 const uint8_t *src,
							 uint8_t *dst, size_t len)
{
	if (!spi || !spi->initialized || ((!src || !dst) && len))
		return false;

	size_t tx_remain = len, rx_remain = len;
	uint64_t deadline = time_us_64() + PIO_SPI_TIMEOUT_US;
	io_rw_8 *txfifo = (io_rw_8 *)&spi->pio->txf[spi->sm];
	io_rw_8 *rxfifo = (io_rw_8 *)&spi->pio->rxf[spi->sm];
	if (spi->order == SPI_LSB_FIRST)
		rxfifo += 3;
	while (tx_remain || rx_remain)
	{
		bool progressed = false;
		if (tx_remain && !pio_sm_is_tx_fifo_full(spi->pio, spi->sm))
		{
			*txfifo = *src++;
			--tx_remain;
			progressed = true;
		}
		if (rx_remain && !pio_sm_is_rx_fifo_empty(spi->pio, spi->sm))
		{
			*dst++ = *rxfifo;
			--rx_remain;
			progressed = true;
		}
		if (progressed)
			deadline = time_us_64() + PIO_SPI_TIMEOUT_US;
		else if (pio_spi_timed_out(deadline))
			return false;
	}
	return true;
}
