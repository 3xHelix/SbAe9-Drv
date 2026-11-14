// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 3xHelix
// Contains code adapted from the Linux kernel's sound/pci/hda/patch_ca0132.c:
//   Copyright (c) 2011, Creative Technology Ltd.
//   Based on patch_ca0110.c, Copyright (c) 2008 Takashi Iwai <tiwai@suse.de>
// and from s3boun3t/ae9_build (driver/ca0132/ca0132.c, GPL-2.0-or-later):
//   the AE-9 init sequence (ca0132_mmio_init_ae9, ae9_acm_init).
//
// Creative AE-9 ACM (Audio Control Module, the head unit) communication
//
// The head unit (display, knob, buttons, headphone amp) talks to the card
// over a serial port: a 16550-style UART in BAR2, with 32-bit registers at a
// stride of 4. Messages are MIDI-style sysex frames, 0xf0 <cmd> <len> [data]
// 0xf7. Early in this project the link was taken for I2C and then SPI; the
// function and constant names (creative_spi_frame*, I2C_START, DSP_REG_*)
// still say so. The UART layout comes from the sb-ae9 projects' decompile
// of Windows' CtxHdb.sys and matches every access in our captures.
//
// Register map (BAR2 offsets):
//   0xC00 = data (THR on write, RBR on read)        our DSP_REG_CMD
//   0xC04 = interrupt enable (IER)                   our DSP_REG_DATA
//   0xC08 = FIFO control (FCR)                       our DSP_REG_FIFO
//   0xC0C = line control (LCR): 0x83 = divisor-latch access (baud setup),
//           0x03 = 8 data bits, no parity, 1 stop   our DSP_REG_CTRL
//   0xC14 = line status (LSR): 0x40 = transmitter empty, bit 0 = byte
//           received. The "0x40 ACK / 0x41 byte ready" handshake below is
//           these two bits.                          our DSP_REG_RESP
//   0xC7C = not a UART register; polled by Windows as a busy/ready status
//   0x320 = card GPIO (not part of the UART)
//
// Reference: ae9_mmio_capture.txt lines 68-108, 63986-64059+;
// DarkNikGr/sb-ae9 docs/reverse-engineering/ctxhda-acm-analysis.md §3.2.
//

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/moduleparam.h>

#include "creative_ae9.h"

/*
 * Per-transaction MCU settle budget (microseconds). After each transaction
 * (the old I2C-style sender: debugfs hutx, and the no-firmware init) we wait
 * up to this long for the head-unit MCU status to return to idle (0x6/0x3)
 * before sending the next one, so the slow MCU does not drop transactions.
 */
#define ACM_TX_SETTLE_US	20000

/* Head-unit UART registers in BAR2 (historical names, see the top) */
#define DSP_REG_CMD		0xC00
#define DSP_REG_DATA		0xC04
#define DSP_REG_FIFO		0xC08
#define DSP_REG_CTRL		0xC0C
#define DSP_REG_RESP		0xC14
#define DSP_REG_STS		0xC7C
#define DSP_REG_STREAM_PARAM	0x320

/* Sysex frame start / end bytes (historical names) */
#define I2C_START	0xf0
#define I2C_STOP	0xf7

/* Timeouts */
#define I2C_RESP_TIMEOUT_US	10000

/* ------------------------------------------------------------------ */
/* Send a complete I2C transaction (array of bytes)			  */
/* ae9_mmio_capture.txt:64004-64059					  */
/* ------------------------------------------------------------------ */
static int creative_i2c_send_tx(struct creative_chip *chip,
				 const u8 *bytes, int count,
				 u32 *resp)
{
	int i;
	u32 r;

	for (i = 0; i < count; i++) {
		/* Write byte to CMD register */
		creative_ext_writel(chip, DSP_REG_CMD, bytes[i]);

		/* Read back — confirm write, register returns 0 */
		creative_ext_readl(chip, DSP_REG_CMD);

		/*
		 * After STOP byte, read response reg instead of status.
		 * Capture pattern: write(0xf7)→read(0xc00)→read(0xc14)→read(0xc7c)
		 */
		if (bytes[i] == I2C_STOP && i == count - 1) {
			/* Read response reg first (may return 0 initially) */
			r = creative_ext_readl(chip, DSP_REG_RESP);
		} else {
			/* Read DSP status */
			creative_ext_readl(chip, DSP_REG_STS);
		}

		udelay(5);
	}

	/* After the complete transaction, poll for response.
	 *
	 * The capture shows the real handshake is two-phase: RESP (0xC14)
	 * first reads back to 0x0 (the previous transaction's 0x40 ACK is
	 * consumed / cleared), then the MCU finishes processing THIS
	 * transaction and RESP becomes 0x40 again. The old code returned on
	 * the first non-zero RESP, which was usually the STALE 0x40 left
	 * over from the previous transaction — so it never actually waited
	 * for the MCU and fired the next transaction into a busy MCU, which
	 * silently dropped it. That is why only the first glyph rendered.
	 *
	 * Fix: (1) wait for RESP to clear to 0x0, then (2) wait for a FRESH
	 * 0x40 completion before returning.
	 */
	if (count > 0 && bytes[count - 1] == I2C_STOP) {
		unsigned int t2 = ACM_TX_SETTLE_US;
		u32 sts_last = 0;
		int frame_bytes = 0;
		u32 last_c00 = 0;

		/*
		 * Mirror the capture's EXACT post-STOP response-frame protocol
		 * (ae9_mmio_capture.txt L70411-70579). The MCU returns a
		 * response frame "0xf0 <reg> <data...> 0xf7" through C00, and
		 * EACH byte must be read out, gated on the RESP (C14)
		 * oscillation: RESP rises to 0x40, then 0x41 (byte ready) —
		 * read one C00 byte — repeat until the 0xf7 terminator, after
		 * which STS (C7C) settles to 0x6.
		 *
		 * Reading the WHOLE frame is required: our old code read one
		 * byte and stopped, so the MCU FIFO backed up (STS stuck 0xf)
		 * and every subsequent transaction NAK'd (0x41). This drains
		 * the full frame so the MCU settles for the next transaction.
		 */
		while (t2 > 0) {
			r = creative_ext_readl(chip, DSP_REG_RESP) & 0xff;
			sts_last = creative_ext_readl(chip, DSP_REG_STS) & 0xff;

			/* When the MCU signals a byte is ready (RESP=0x41 after
			 * a 0x40), read the next frame byte from C00.
			 */
			if (r == 0x41 && frame_bytes < 16) {
				last_c00 = creative_ext_readl(chip,
							      DSP_REG_CMD) & 0xff;
				frame_bytes++;
				/* terminator + idle status => settled */
				if (last_c00 == I2C_STOP) {
					/* confirm settle */
					sts_last = creative_ext_readl(chip,
							DSP_REG_STS) & 0xff;
					if (sts_last == 0x6)
						break;
				}
			} else if (r == 0x40 && sts_last == 0x6 &&
				   frame_bytes > 0) {
				/* already drained + settled */
				break;
			}

			udelay(5);
			t2 -= 5;
		}


		if (resp)
			*resp = r;
		return 0;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Build a byte array for an I2C transaction				  */
/* Format: 0xf0 <reg> <len> [data...] 0xf7				  */
/* ------------------------------------------------------------------ */

/* Helper: send 0xf0 <reg> <len> [data...] 0xf7 */
static int creative_i2c_send_raw(struct creative_chip *chip,
				  u8 reg, u8 len,
				  const u8 *data, u32 *resp)
{
	/* Max transaction: 1(f0) + 1(reg) + 1(len) + len + 1(f7) */
	u8 buf[64];
	int idx = 0;

	int ret;
	u32 sts;
	int waited;

	buf[idx++] = I2C_START;
	buf[idx++] = reg;
	buf[idx++] = len;
	if (data && len > 0)
		memcpy(buf + idx, data, len);
	idx += len;
	buf[idx++] = I2C_STOP;

	ret = creative_i2c_send_tx(chip, buf, idx, resp);
	if (ret < 0)
		return ret;

	/*
	 * Wait for the head-unit MCU to finish consuming this transaction
	 * before sending the next. The bridge ACKs (RESP) almost
	 * immediately, but the MCU is slow: if we blast the next
	 * transaction while it is still busy (STS != 0x6 idle) it silently
	 * drops it. That is why only the first glyph ("A") rendered and the
	 * rest of "AE-9"/"-HP-"/version never appeared. Gate each
	 * transaction on STS returning to the idle/ready value 0x6.
	 */
	for (waited = 0; waited < ACM_TX_SETTLE_US / 20; waited++) {
		sts = creative_ext_readl(chip, DSP_REG_STS) & 0xff;
		if (sts == 0x6 || sts == 0x3)
			break;
		udelay(20);
	}

	return 0;
}

/* Public: send one arbitrary head-unit ACM transaction (for live probing).
 * Selects the head-unit I2C target first. Logs the response frame.
 */
int creative_i2c_raw_tx(struct creative_chip *chip, u8 reg,
			const u8 *data, u8 len)
{
	u32 resp = 0;
	int ret;

	creative_ext_writew(chip, DSP_REG_STREAM_PARAM, 0x105);
	creative_ext_readw(chip, DSP_REG_STREAM_PARAM);

	ret = creative_i2c_send_raw(chip, reg, len, data, &resp);
	dev_info(chip->dev, "hutx reg=0x%02x len=%d -> ret=%d resp=0x%x\n",
		 reg, len, ret, resp);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Send the DSP FIFO init sequence (from first I2C transaction)	  */
/* ae9_mmio_capture.txt:68-107					  */
/* ------------------------------------------------------------------ */

int creative_i2c_dsp_init(struct creative_chip *chip)
{
	u32 val;

	dev_dbg(chip->dev, "DSP I2C controller init\n");

	/* Read DSP status - expect 0x6 */
	val = creative_ext_readl(chip, DSP_REG_STS);

	/* Read I2C control */
	val = creative_ext_readl(chip, DSP_REG_CTRL);

	/* Write 0x80 to CTRL (init mode) */
	creative_ext_writel(chip, DSP_REG_CTRL, 0x80);

	/* Write 0x30 to CMD */
	creative_ext_writel(chip, DSP_REG_CMD, 0x30);

	/* Write 0x00 to DATA */
	creative_ext_writel(chip, DSP_REG_DATA, 0x00);

	/* Read CTRL back */
	creative_ext_readl(chip, DSP_REG_CTRL);

	/* Write 0x00 to CTRL (clear mode) */
	creative_ext_writel(chip, DSP_REG_CTRL, 0x0);

	/* Read DSP status */
	creative_ext_readl(chip, DSP_REG_STS);

	/* Read I2C control */
	val = creative_ext_readl(chip, DSP_REG_CTRL);

	/* Write 0x03 to CTRL (FIFO mode) */
	creative_ext_writel(chip, DSP_REG_CTRL, 0x3);

	/* Read DSP status again */
	val = creative_ext_readl(chip, DSP_REG_STS);

	/* Check + re-write 0x03 three times (as seen in capture) */
	val = creative_ext_readl(chip, DSP_REG_CTRL);
	creative_ext_writel(chip, DSP_REG_CTRL, 0x3);
	val = creative_ext_readl(chip, DSP_REG_CTRL);

	val = creative_ext_readl(chip, DSP_REG_STS);
	val = creative_ext_readl(chip, DSP_REG_CTRL);
	creative_ext_writel(chip, DSP_REG_CTRL, 0x3);
	val = creative_ext_readl(chip, DSP_REG_CTRL);

	val = creative_ext_readl(chip, DSP_REG_STS);
	val = creative_ext_readl(chip, DSP_REG_CTRL);
	creative_ext_writel(chip, DSP_REG_CTRL, 0x3);
	val = creative_ext_readl(chip, DSP_REG_CTRL);

	/* Write FIFO bytes: 0x1, 0xf1, 0x1, 0xc7, 0xc1 */
	/*
	 * Note: each FIFO byte is written to DSP_REG_FIFO.
	 * Reading FIFO returns 0xc1 repeatedly (status?).
	 */
	creative_ext_readl(chip, DSP_REG_FIFO);	/* read = 0x1 */
	creative_ext_writel(chip, DSP_REG_FIFO, 0x1);
	creative_ext_readl(chip, DSP_REG_FIFO);	/* read = 0xc1 */
	creative_ext_readl(chip, DSP_REG_FIFO);	/* read = 0xc1 */
	creative_ext_writel(chip, DSP_REG_FIFO, 0xf1);
	creative_ext_readl(chip, DSP_REG_FIFO);	/* read = 0xc1 */
	creative_ext_readl(chip, DSP_REG_FIFO);	/* read = 0xc1 */
	creative_ext_writel(chip, DSP_REG_FIFO, 0x1);
	creative_ext_readl(chip, DSP_REG_FIFO);	/* read = 0xc1 */
	creative_ext_readl(chip, DSP_REG_FIFO);	/* read = 0xc1 */
	creative_ext_writel(chip, DSP_REG_FIFO, 0xc7);
	creative_ext_readl(chip, DSP_REG_FIFO);	/* read = 0xc1 */
	creative_ext_readl(chip, DSP_REG_FIFO);	/* read = 0xc1 */
	creative_ext_writel(chip, DSP_REG_FIFO, 0xc1);
	creative_ext_readl(chip, DSP_REG_FIFO);	/* read = 0xc1 */

	/* Read DATA reg = 0x0 */
	val = creative_ext_readl(chip, DSP_REG_DATA);

	/* Write 0x80 to DATA (trigger) */
	creative_ext_writel(chip, DSP_REG_DATA, 0x80);

	/* Read DATA back to confirm */
	val = creative_ext_readl(chip, DSP_REG_DATA);

	dev_dbg(chip->dev, "DSP I2C controller init done (final STS=0x%x)\n",
		creative_ext_readl(chip, DSP_REG_STS));

	return 0;
}

/* ------------------------------------------------------------------ */
/* DSP audio loop cold-start via BAR2 SPI (region2+0xC00)		  */
/*								  */
/* From cold-boot VFIO capture (vm_capture_coldboot_20260603_124746): */
/* The 8051 audio loop starts via a SPI probe+trigger sequence on     */
/* BAR2+0xC00, NOT via HDA verbs.  The sequence:                     */
/*   1. Retry loop (~8x, 450ms apart):                                */
/*      Send probe frames until MEM_WRITE 0x11111111 → ACK=0x81      */
/*   2. On ACK: send SYS_CTRL + MSG_SEND/STATUS + PARAM_SET×3        */
/*   3. Send HELLO "-HP-" → 8051 ACKs → relay clicks + HP display    */
/*								  */
/* After this, continue sending HELLO as heartbeat in the ACM work.  */
/* ------------------------------------------------------------------ */

/*
 * Send a SPI frame and receive the response frame.
 *
 * From cold-boot capture analysis (L79469-79560):
 *
 * SEND phase: for each TX byte:
 *   W BAR2+0xC00 = byte    (write to FIFO)
 *   R BAR2+0xC00            (echo readback — discard)
 *   R BAR2+0xC7C            (status poll — discard)
 *
 * RECEIVE phase: after last TX byte, poll BAR2+0xC14 (RESP register):
 *   Poll until 0xC14 == 0x41 (byte ready), then R 0xC00 to get byte.
 *   Repeat until 0xF7 (frame end) or 0xC14 goes to 0x40 (idle).
 *
 * resp[] receives the response frame bytes; returns frame length.
 */

/*
 * Send one head-unit SPI frame exactly as Windows does (cold capture):
 * check C14/C7C idle, then per byte write C00, read C00, read C7C; after
 * the f7 terminator poll C14 and pop a reply byte from C00 whenever it
 * reads 0x41, until the reply's own f7 arrives. ~1.5 ms per frame.
 * Returns the number of reply bytes, or -ETIMEDOUT if no reply ended.
 */
int creative_spi_frame_win(struct creative_chip *chip, const u8 *tx,
			   int tx_len, u8 *resp, int resp_len)
{
	ktime_t deadline;
	unsigned int idle = 0;
	int i, n = 0, total = 0;

	creative_ext_readl(chip, DSP_REG_RESP);
	creative_ext_readl(chip, DSP_REG_STS);
	creative_ext_readl(chip, DSP_REG_STS);
	for (i = 0; i < tx_len; i++) {
		creative_ext_writel(chip, DSP_REG_CMD, tx[i]);
		creative_ext_readl(chip, DSP_REG_CMD);
		creative_ext_readl(chip, DSP_REG_STS);
	}
	/*
	 * Wait up to 20 ms for the reply. Poll tightly for the first ~100 us
	 * (and again right after each reply byte, since the rest of the reply
	 * follows quickly), then sleep between polls instead of burning the
	 * CPU: the head-unit poll runs every 40 ms.
	 */
	deadline = ktime_add_us(ktime_get(), 20000);
	for (;;) {
		if ((creative_ext_readl(chip, DSP_REG_RESP) & 0xff) == 0x41) {
			u8 b = creative_ext_readl(chip, DSP_REG_CMD) & 0xff;

			idle = 0;
			/* a reply starts at f0: anything before it is a byte an
			 * earlier, cut-short reply left behind */
			if (!n && b != I2C_START)
				continue;
			if (resp && n < resp_len)
				resp[n] = b;
			n++;
			if (n == 3)
				total = b + 4;
			/*
			 * A reply is f0 <cmd> <len> <len bytes> f7: it ends at
			 * byte len + 4, not at the first f7. Status 0xf7 (nothing
			 * plugged, SBX light on, phantom off) is data; stopping
			 * there cut the reply short and shifted the next one by
			 * a byte (uv42 2026-11-12: a plug-out logged 8 s late and
			 * a false "knob press bit stuck"). All 478018 replies in
			 * 28 Windows traces follow len + 4, 7840 of them with an
			 * f7 data byte. A reply shorter than its length (never
			 * seen) times out; a longer one ends at the first f7 past its length.
			 */
			if (b == I2C_STOP && n > 1 && (!total || n >= total))
				return n;
			continue;
		}
		if (ktime_after(ktime_get(), deadline))
			return -ETIMEDOUT;
		creative_ext_readl(chip, DSP_REG_STS);
		if (++idle < 100)
			udelay(1);
		else
			usleep_range(20, 40);
	}
}
