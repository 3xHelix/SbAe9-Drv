// SPDX-License-Identifier: GPL-2.0-only
//
// Creative AE-9 DSP I2C / ACM (Audio Control Module) communication
//
// The AE-9 head unit (front-panel -AE9- display, ESS DAC, XAMP) is
// controlled via I2C through DSP registers in the extension BAR.
//
// Register map (all offsets relative to BAR2):
//   0xC00 = I2C_CMD   — write I2C transaction bytes here
//   0xC04 = I2C_DATA  — auxiliary data / trigger
//   0xC08 = I2C_FIFO  — FIFO writes for DSP init
//   0xC0C = I2C_CTRL  — control/status (0x03 = I2C mode, 0x80|mode = init)
//   0xC14 = I2C_RESP  — response data (polled after I2C stop)
//   0xC7C = DSP_STS   — DSP status (0x6 = ready, 0x3 = ready, 0x7 = busy)
//   0x320 = STREAM_PARAM — stream parameter (select I2C target?)
//
// Reference: ae9_mmio_capture.txt lines 68-108, 63986-64059+
//

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/moduleparam.h>

#include "creative_ae9.h"

/*
 * Per-transaction MCU settle budget (microseconds). After each ACM
 * transaction we wait up to this long for the head-unit MCU status to
 * return to idle (0x6/0x3) before sending the next one, so the slow MCU
 * does not drop transactions. Tunable without recompiling.
 */
unsigned int acm_tx_settle_us = 20000;
module_param(acm_tx_settle_us, uint, 0444);
MODULE_PARM_DESC(acm_tx_settle_us,
		 "Max per-transaction MCU completion wait for ACM init (us)");

unsigned int acm_i2c_debug;
module_param(acm_i2c_debug, uint, 0644);
MODULE_PARM_DESC(acm_i2c_debug, "Log per-transaction ACM I2C handshake (1=on)");

/* DSP I2C register offsets in extension BAR */
#define DSP_REG_CMD		0xC00
#define DSP_REG_DATA		0xC04
#define DSP_REG_FIFO		0xC08
#define DSP_REG_CTRL		0xC0C
#define DSP_REG_RESP		0xC14
#define DSP_REG_STS		0xC7C
#define DSP_REG_STREAM_PARAM	0x320

/* I2C transaction markers */
#define I2C_START	0xf0
#define I2C_STOP	0xf7

/* Timeouts */
#define I2C_RESP_TIMEOUT_US	10000

/* Forward declaration — creative_spi_frame is defined later in this file
 * but needed by creative_i2c_pcm_relay_engage which precedes it. */
static int creative_spi_frame(struct creative_chip *chip,
			      const u8 *tx, int tx_len,
			      u8 *rx, int rx_max);

/* ------------------------------------------------------------------ */
/* I2C transaction setup (before each sequence)			  */
/* ae9_mmio_capture.txt:63986-64003					  */
/* ------------------------------------------------------------------ */
static int creative_i2c_setup(struct creative_chip *chip)
{
	/* ae9_mmio_capture.txt:63986-64003 */
	/* Exact sequence from Windows driver */

	/* Read DSP status */
	creative_ext_readl(chip, DSP_REG_STS);

	/* Read I2C control */
	creative_ext_readl(chip, DSP_REG_CTRL);

	/* Write 0x83 to CTRL (init mode) */
	creative_ext_writel(chip, DSP_REG_CTRL, 0x83);

	/* Confirm CTRL */
	creative_ext_readl(chip, DSP_REG_CTRL);

	/* Write 0x0d to CMD */
	creative_ext_writel(chip, DSP_REG_CMD, 0x0d);

	/* Confirm CMD */
	creative_ext_readl(chip, DSP_REG_CMD);

	/* Write 0x00 to DATA */
	creative_ext_writel(chip, DSP_REG_DATA, 0x00);

	/* Confirm DATA */
	creative_ext_readl(chip, DSP_REG_DATA);

	/* Read CTRL again */
	creative_ext_readl(chip, DSP_REG_CTRL);

	/* Clear init bit */
	creative_ext_writel(chip, DSP_REG_CTRL, 0x3);

	/* Confirm CTRL */
	creative_ext_readl(chip, DSP_REG_CTRL);

	/* Read STREAM_PARAM (initial value 0x1) */
	creative_ext_readw(chip, DSP_REG_STREAM_PARAM);

	/* Write 0x105 to STREAM_PARAM (16-bit register at 0x320) */
	creative_ext_writew(chip, DSP_REG_STREAM_PARAM, 0x105);

	/* Confirm STREAM_PARAM twice */
	creative_ext_readw(chip, DSP_REG_STREAM_PARAM);
	creative_ext_readw(chip, DSP_REG_STREAM_PARAM);

	/* Read RESP — should return 0x40 (acknowledgment) */
	creative_ext_readl(chip, DSP_REG_RESP);

	/* Read STS twice */
	creative_ext_readl(chip, DSP_REG_STS);
	creative_ext_readl(chip, DSP_REG_STS);

	return 0;
}

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
		unsigned int t2 = acm_tx_settle_us;
		u32 sts_last = 0;
		int frame_bytes = 0;
		u32 last_c00 = 0;
		u8 frame_buf[16] = {0};

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
				frame_buf[frame_bytes] = last_c00;
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

		if (acm_i2c_debug)
			pr_info("ae9-i2c: reg=0x%02x frame=%d [%*ph] resp=0x%x sts=0x%x\n",
				count > 1 ? bytes[1] : 0, frame_bytes,
				min(frame_bytes, 12), frame_buf, r, sts_last);
		if (0)
			pr_info("ae9-i2c: reg=0x%02x frame=%d lastc00=0x%x resp=0x%x p2left=%u sts=0x%x\n",
				count > 1 ? bytes[1] : 0, frame_bytes,
				last_c00, r, t2, sts_last);

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
	for (waited = 0; waited < acm_tx_settle_us / 20; waited++) {
		sts = creative_ext_readl(chip, DSP_REG_STS) & 0xff;
		if (sts == 0x6 || sts == 0x3)
			break;
		udelay(20);
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* 29 unique I2C transactions from Windows driver init sequence	  */
/* history/ae9_init_unique_transactions.txt					  */
/* ------------------------------------------------------------------ */

/*
 * Full init sequence sent by Windows driver.
 * Each entry is: { reg, len, data... }
 * A NULL data pointer means zero-filled of length len.
 */
struct ae9_i2c_init_entry {
	u8 reg;
	u8 len;
	const u8 *data;
};

/*
 * EXACT head-unit ACM init sequence, transcribed byte-for-byte from the
 * Windows driver MMIO capture (ae9_mmio_capture.txt lines 64180-70200,
 * reconstructed by framing on 0xf0 START / 0xf7 STOP writes to 0xC00).
 *
 * The previous hand-built table was wrong: it had NULL/zero data where
 * real payloads were required (#3, #7), was missing the repeated
 * "Acm1"/"1mcA" pair (#5,#6), and appended fabricated "-HP-"/"-24.5"
 * transactions that the real driver never sends. The genuine boot
 * display is simply "AE-9" written once (#17). This is why only the
 * first glyph ever rendered.
 */
static const struct ae9_i2c_init_entry ae9_i2c_init_seq[] = {
	/* #1  "Acm1" */
	{ 0x54, 4, (const u8[]){ 0x41, 0x63, 0x6d, 0x31 } },
	/* #2  "1mcA" (reversed) */
	{ 0x54, 4, (const u8[]){ 0x31, 0x6d, 0x63, 0x41 } },
	/* #3  control */
	{ 0xd5, 3, (const u8[]){ 0x00, 0x20, 0x04 } },
	/* #4  four 0x11 */
	{ 0x54, 4, (const u8[]){ 0x11, 0x11, 0x11, 0x11 } },
	/* #5  "Acm1" (repeated) */
	{ 0x54, 4, (const u8[]){ 0x41, 0x63, 0x6d, 0x31 } },
	/* #6  "1mcA" (repeated) */
	{ 0x54, 4, (const u8[]){ 0x31, 0x6d, 0x63, 0x41 } },
	/* #7  longer control */
	{ 0x55, 7, (const u8[]){ 0x00, 0x20, 0x04, 0xde, 0xc0, 0xad, 0xde } },
	/* #8  four 0x11 */
	{ 0x54, 4, (const u8[]){ 0x11, 0x11, 0x11, 0x11 } },
	/* #9  mode set */
	{ 0x03, 3, (const u8[]){ 0x05, 0x03, 0x03 } },
	/* #10 register write */
	{ 0x32, 3, (const u8[]){ 0x02, 0xb8, 0x0b } },
	/* #11 control */
	{ 0x43, 4, (const u8[]){ 0x64, 0x00, 0xf4, 0x01 } },
	/* #12 register write */
	{ 0x32, 3, (const u8[]){ 0x01, 0x88, 0x13 } },
	/* #13 control */
	{ 0x05, 3, (const u8[]){ 0x02, 0x01, 0x00 } },
	/* #14 MODE subaddr=1 = 0x00 (cold-boot capture: MCU should be in 0xb1 mode) */
	{ 0x22, 2, (const u8[]){ 0x01, 0x00 } },
	/* #15 MODE subaddr=2 = 0x01 */
	{ 0x22, 2, (const u8[]){ 0x02, 0x01 } },
	/* #16 mode set (enable display) */
	{ 0x03, 3, (const u8[]){ 0x02, 0x00, 0x40 } },
	/* #17 "AE-9" display string */
	{ 0x11, 9, (const u8[]){ 0x41, 0x45, 0x2d, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	/* #18 register write (commit) */
	{ 0x21, 3, (const u8[]){ 0x02, 0x00, 0x00 } },
	/*
	 * #19-33: the AMP/RELAY ENABLE, inline in the real init flow right
	 * after the display commit (capture lines 70398-73294). This is what
	 * engages the head-unit DAC/amp (the relay "click") — it happens at
	 * INIT, not at playback. It must run as one continuous flow with the
	 * display init above (running it as a separate batch later NAKs).
	 * #21-25 re-handshake (0x81 identify + Acm1/1mcA) before the amp block.
	 */
	/* #19-20 amp pre-enable */
	{ 0x83, 1, (const u8[]){ 0x02 } },
	{ 0x83, 1, (const u8[]){ 0x07 } },
	/* #21-25 re-handshake */
	{ 0x81, 0, NULL },
	{ 0x54, 4, (const u8[]){ 0x41, 0x63, 0x6d, 0x31 } },
	{ 0x54, 4, (const u8[]){ 0x31, 0x6d, 0x63, 0x41 } },
	{ 0xd5, 3, (const u8[]){ 0x00, 0x20, 0x04 } },
	{ 0x54, 4, (const u8[]){ 0x11, 0x11, 0x11, 0x11 } },
	/* #26-33 amp/relay enable block */
	{ 0xc2, 0, NULL },
	{ 0x83, 1, (const u8[]){ 0x02 } },
	{ 0x83, 1, (const u8[]){ 0x02 } },
	{ 0x83, 1, (const u8[]){ 0x07 } },
	{ 0x85, 1, (const u8[]){ 0x02 } },
	{ 0x83, 1, (const u8[]){ 0x02 } },
	{ 0x83, 1, (const u8[]){ 0x07 } },
	{ 0xb1, 1, (const u8[]){ 0x01 } },
};

/* ------------------------------------------------------------------ */
/* Public API: send the full init sequence				  */
/* ------------------------------------------------------------------ */

int creative_i2c_init(struct creative_chip *chip)
{
	int i;
	int ret;

	dev_info(chip->dev, "Initializing DSP I2C / ACM head unit\n");

	/*
	 * Gate on head-unit MCU readiness before sending any ACM
	 * transaction. Firing blind while the MCU is still in its
	 * power-on self-test (STS=0x7 busy) corrupts the display into
	 * partial garbage ("2", "4.", "A |- |-"). Wait for STS to settle
	 * to a ready value (0x3 or 0x6) instead of relying on boot timing.
	 */
	bool mcu_ready = false;
	{
		u32 sts = 0;
		int waited = 0;

		for (waited = 0; waited < 2000; waited++) {	/* up to ~1s */
			sts = creative_ext_readl(chip, DSP_REG_STS) & 0xff;
			if (sts == 0x3 || sts == 0x6) {
				mcu_ready = true;
				break;
			}
			usleep_range(450, 550);
		}
		dev_info(chip->dev,
			 "head-unit MCU STS=0x%x after %d polls (%s)\n",
			 sts, waited, mcu_ready ? "ready" : "TIMEOUT/busy");
	}

	/* Setup DSP I2C interface */
	ret = creative_i2c_setup(chip);
	if (ret < 0) {
		dev_err(chip->dev, "DSP I2C setup failed: %d\n", ret);
		return ret;
	}

	/* Send init sequence */
	/* TX #1 continuation: I2C transaction 0xf0 0x81 0x00 0xf7 */
	{
		u8 first_tx[] = { I2C_START, 0x81, 0x00, I2C_STOP };
		u32 resp = 0;
		ret = creative_i2c_send_tx(chip, first_tx, 4, &resp);
		if (ret < 0) {
			dev_err(chip->dev, "I2C TX #1 failed: %d\n", ret);
			return ret;
		}
		dev_dbg(chip->dev, "I2C TX #1: resp=0x%08x\n", resp);
	}

	for (i = 0; i < ARRAY_SIZE(ae9_i2c_init_seq); i++) {
		const struct ae9_i2c_init_entry *e = &ae9_i2c_init_seq[i];
		u32 resp = 0;

		ret = creative_i2c_send_raw(chip, e->reg, e->len,
					     e->data, &resp);
		if (ret < 0) {
			dev_err(chip->dev,
				"I2C TX #%d failed: %d\n", i + 2, ret);
			return ret;
		}

		dev_dbg(chip->dev,
			 "I2C TX #%d: reg=0x%02x len=%d resp=0x%08x\n",
			 i + 2, e->reg, e->len, resp);
	}

	/* Display is initialized by the init sequence above. */

	dev_info(chip->dev, "ACM head unit initialization complete\n");

	/*
	 * Report success only if the MCU was actually ready when we sent
	 * the sequence. If it was still busy (timeout), return -EBUSY so
	 * the caller's retry loop runs again later.
	 */
	return mcu_ready ? 0 : -EBUSY;
}

/* ------------------------------------------------------------------ */
/* Periodic head-unit heartbeat (capture: every ~500ms, 381 times,	  */
/* continues during audio playback). A lightweight 2-transaction poll:	  */
/*   select target 0x320=0x105, then {0x81,0} identify + {0x54,"Acm1"}.	  */
/* This keeps the head-unit MCU (the output DAC) alive while firmware	  */
/* is running and audio is streaming. Returns 0 if the MCU acked.	  */
/* ------------------------------------------------------------------ */
/* Set with post_windows: Windows writes GPIO 0x320=0x104 only twice in
 * the whole cold capture, never per heartbeat. */
bool creative_hb_skip_gpio4;

int creative_i2c_heartbeat(struct creative_chip *chip)
{
	/*
	 * The real ~500ms heartbeat is NOT just an identify ping — it re-runs
	 * the full AMP/OUTPUT-MODE block every cycle (capture: repeats from
	 * ~73500 onward). Each cycle drives the 0xb1 output-mode register
	 * 0x01 -> 0x02 -> 0x03; reaching 0x03 ENGAGES the output relay (the
	 * "click") and the head unit then shows "-HP-"/"-SP-". Holding the
	 * amp on requires this to keep running. Block (per capture cycle):
	 *   0xc2, 0x83(02,02,07), 0x85(02), 0x83(02,07), 0xb1(01,02,03)
	 */
	const struct ae9_i2c_init_entry hb_seq[] = {
		{ 0xc2, 0, NULL },
		{ 0x83, 1, (const u8[]){ 0x02 } },
		{ 0x83, 1, (const u8[]){ 0x02 } },
		{ 0x83, 1, (const u8[]){ 0x07 } },
		{ 0x85, 1, (const u8[]){ 0x02 } },
		{ 0x83, 1, (const u8[]){ 0x02 } },
		{ 0x83, 1, (const u8[]){ 0x07 } },
		{ 0xb1, 1, (const u8[]){ 0x01 } },	/* mode step 1 */
		{ 0xb1, 1, (const u8[]){ 0x02 } },	/* mode step 2 */
		{ 0xb1, 1, (const u8[]){ 0x03 } },	/* mode = output ON (relay) */
	};
	u32 sts, resp = 0;
	u8 query_tx[] = { I2C_START, 0x81, 0x00, I2C_STOP };
	int i, ret;

	/* Re-select the head-unit I2C target (pin 5). */
	creative_ext_writew(chip, DSP_REG_STREAM_PARAM, 0x105);
	creative_ext_readw(chip, DSP_REG_STREAM_PARAM);

	/*
	 * Do NOT gate on STS here. The head-unit STS frequently reads 0xf
	 * (busy) at the start of a cycle, but the per-transaction frame-drain
	 * handshake settles each transaction to 0x6. The capture's heartbeat
	 * fires unconditionally every ~500ms. Gating on STS=0x6/0x3 made the
	 * heartbeat bail every cycle (STS stuck 0xf) so the 0xb1 output-mode
	 * commands were NEVER sent -> relay never engaged.
	 */
	sts = creative_ext_readl(chip, DSP_REG_STS) & 0xff;
	(void)sts;

	/* identify + handshake */
	creative_i2c_send_tx(chip, query_tx, 4, &resp);
	creative_i2c_send_raw(chip, 0x54, 4,
			      (const u8[]){ 0x41, 0x63, 0x6d, 0x31 }, &resp);

	/*
	 * GPIO pin4 HIGH immediately before the 0xb1 relay sequence.
	 *
	 * From cold-boot MMIO capture (L39566, 01:28:07.513):
	 *   0x320 <- 0x104  (GPIO pin4 HIGH = amp power enable)
	 *   [ca0113 cmds: 0x800005->0x48/0x07, 0x800003->0x204=0x07]
	 *   0x320 <- 0x105  (I2C target re-select, 2ms later)
	 *   ... heartbeat fires 0xb1->0x01->0x02->0x03 (click, 193ms later)
	 *
	 * Windows sets pin4 ONCE then never again — the amp power rail
	 * latches at the hardware level. Writing 0x105 for I2C does NOT
	 * un-latch it. We must set pin4 each heartbeat cycle because our
	 * hardware may not latch it (or we reset it elsewhere). Setting it
	 * here right before 0xb1 ensures it's HIGH when the relay fires.
	 */
	if (!creative_hb_skip_gpio4) {
		creative_ext_writew(chip, DSP_REG_STREAM_PARAM, 0x104); /* pin4 HIGH */
		udelay(2000); /* 2ms stabilization (confirmed from capture timing) */
		creative_ext_writew(chip, DSP_REG_STREAM_PARAM, 0x105); /* back to I2C */
	}

	/* full amp/output-mode block (engages + holds the relay) */
	for (i = 0; i < ARRAY_SIZE(hb_seq); i++) {
		ret = creative_i2c_send_raw(chip, hb_seq[i].reg, hb_seq[i].len,
					    hb_seq[i].data, &resp);
	}
	return ret;
}

/*
 * Relay engagement sequence for PCM prepare time.
 *
 * Byte-exact from cold-boot MMIO capture (01:28:07.433-707), which runs
 * at PCM prepare time (immediately after SET_CONVERTER_STREAM on nid=0x02).
 *
 * The MCU requires a WAKE + probe sequence to enter "relay-ready" state
 * before it will accept the 0xb1 relay engagement frames. Simply sending
 * the hb_seq (creative_i2c_heartbeat) without the preceding probe handshake
 * results in the 0xb1 commands being silently discarded.
 *
 * Sequence (creative_spi_frame path, byte-exact from MMIO):
 *   1. SPI re-init: 0xC0C=0x83, 0xC00=0x0d, 0xC0C=0x03  [01:28:07.433]
 *   2. 0x320=0x105 + SYS_INFO (f0 81 00 f7)              [01:28:07.434]
 *   3. MEM_WRITE "Acm1" + "1mcA"                          [01:28:07.435]
 *   4. DMA_CTRL (f0 d5 03 00 20 04 f7)                   [01:28:07.548]
 *   5. WAKE (f0 21 03 02 00 00 f7)                        [01:28:07.509]
 *   6. probe (f0 54 04 11 11 11 11 f7) → check for 0x81  [01:28:07.688]
 *   7. hb_seq: c2, 83(02)×2, 83(07), 85(02), 83(02)×2, 83(07)
 *              b1(01), b1(02), b1(03)                      [01:28:07.690-707]
 *   8. GPIO pin4 HIGH (0x320=0x104) before step 7         [01:28:07.513]
 *
 * Note on ordering: the MMIO shows GPIO pin4 HIGH (0x320=0x104) at .513,
 * between the MEM_WRITEs (.435) and the WAKE (.509). We place it just before
 * the hb_seq for safety (pin4 must be HIGH when relay fires).
 */
int creative_i2c_pcm_relay_engage(struct creative_chip *chip)
{
	/*
	 * Stop the heartbeat before touching the SPI bus.
	 *
	 * The heartbeat (hb_work) fires every 500ms and writes to BAR2+0xC00
	 * (same bus as our SPI frames). If a heartbeat fires mid-probe, the
	 * MCU receives interleaved bytes from both and gets stuck sending 0x01
	 * (busy/error) on MISO — causing echo0=0x01 and probe no ACK.
	 *
	 * cancel_delayed_work_sync waits for any in-progress heartbeat to
	 * complete, then prevents new ones. We reschedule after relay frames
	 * are sent so the heartbeat resumes and keeps the relay engaged.
	 *
	 * This is safe: pcm_prepare runs in process context (sleepable).
	 */
	cancel_delayed_work_sync(&chip->hb_work);

	/*
	 * SPI frames — byte-exact from cold-boot MMIO (01:28:07.433-707).
	 *
	 * The full sequence decoded from the capture:
	 *
	 * Step 1: SPI re-init (0xC0C=0x83/0xC00=0x0d/0xC0C=0x03)
	 *
	 * Step 2: ae9_i2c_init_seq #1-17 via SPI frames (NOT I2C byte-write path):
	 *   #1-4:  Acm1, 1mcA, DMA_CTRL, probe (initial handshake)
	 *   #5-8:  Acm1, 1mcA, long-DMA, probe (repeated)
	 *   #9-16: ca0113 MCU register configuration (mode, display enable)
	 *   #17:   AE-9 display string → MCU shows "AE-9" + lights knob!
	 *
	 * Step 3: WAKE + MSG_SEND (prepare MCU for probe ACK)
	 * Step 4: GPIO pin4 HIGH (amp relay power)
	 * Step 5: SYS_INFO + Acm1 + 1mcA + DMA_CTRL (probe handshake round 2)
	 * Step 6: probe 0x11111111 → 0x81 ACK
	 * Step 7: hb_seq (c2, MSG drains, 0xb1/01/02/03) → RELAY CLICK!
	 *
	 * KEY INSIGHT: ae9_i2c_init_seq fires via SPI path at PCM prepare time,
	 * NOT via acm_work. Sending it from acm_work (I2C path, asynchronously)
	 * disrupts the MCU. Sending it here (SPI path, synchronously) matches
	 * exactly what the cold-boot capture shows.
	 */

	/* SPI frames */
	static const u8 f_acm1[]    = { 0xf0, 0x54, 0x04,
					0x41, 0x63, 0x6d, 0x31, 0xf7 };
	static const u8 f_1mca[]    = { 0xf0, 0x54, 0x04,
					0x31, 0x6d, 0x63, 0x41, 0xf7 };
	static const u8 f_dmactrl[] = { 0xf0, 0xd5, 0x03,
					0x00, 0x20, 0x04, 0xf7 };
	static const u8 f_probe[]   = { 0xf0, 0x54, 0x04,
					0x11, 0x11, 0x11, 0x11, 0xf7 };
	/* ae9_i2c_init_seq #7: long DMA variant (f0 55 07 00 20 04 de c0 ad de f7) */
	static const u8 f_dma_long[] = { 0xf0, 0x55, 0x07,
					 0x00, 0x20, 0x04,
					 0xde, 0xc0, 0xad, 0xde, 0xf7 };
	/* ae9_i2c_init_seq #9-16: MCU register configuration frames */
	static const u8 f_mode1[]   = { 0xf0, 0x03, 0x03, 0x05, 0x03, 0x03, 0xf7 };
	static const u8 f_reg10[]   = { 0xf0, 0x32, 0x03, 0x02, 0xb8, 0x0b, 0xf7 };
	static const u8 f_reg11[]   = { 0xf0, 0x43, 0x04, 0x64, 0x00, 0xf4, 0x01, 0xf7 };
	static const u8 f_reg12[]   = { 0xf0, 0x32, 0x03, 0x01, 0x88, 0x13, 0xf7 };
	static const u8 f_reg13[]   = { 0xf0, 0x05, 0x03, 0x02, 0x01, 0x00, 0xf7 };
	static const u8 f_mode14[]  = { 0xf0, 0x22, 0x02, 0x01, 0x00, 0xf7 };
	static const u8 f_mode15[]  = { 0xf0, 0x22, 0x02, 0x02, 0x01, 0xf7 };
	static const u8 f_disp_en[] = { 0xf0, 0x03, 0x03, 0x02, 0x00, 0x40, 0xf7 };
	/* ae9_i2c_init_seq #17: "AE-9" display string */
	static const u8 f_ae9[]     = { 0xf0, 0x11, 0x09,
					0x41, 0x45, 0x2d, 0x39,
					0x00, 0x00, 0x00, 0x00, 0x00, 0xf7 };
	/* Phase 3 frames */
	static const u8 f_sysinfo[] = { 0xf0, 0x81, 0x00, 0xf7 };
	static const u8 f_wake[]    = { 0xf0, 0x21, 0x03,
					0x02, 0x00, 0x00, 0xf7 };
	/* hb_seq relay frames */
	static const u8 f_sysctrl[] = { 0xf0, 0xc2, 0x00, 0xf7 };
	static const u8 f_msg_q2[]  = { 0xf0, 0x83, 0x01, 0x02, 0xf7 };
	static const u8 f_msg_q7[]  = { 0xf0, 0x83, 0x01, 0x07, 0xf7 };
	static const u8 f_msg_sts[] = { 0xf0, 0x85, 0x01, 0x02, 0xf7 };
	static const u8 f_relay1[]  = { 0xf0, 0xb1, 0x01, 0x01, 0xf7 };
	static const u8 f_relay2[]  = { 0xf0, 0xb1, 0x01, 0x02, 0xf7 };
	static const u8 f_relay3[]  = { 0xf0, 0xb1, 0x01, 0x03, 0xf7 };

	u8 resp[16];
	int n, i;
	bool probe_ack = false;

	dev_info(chip->dev, "pcm_relay_engage: SPI re-init + init_seq + probe + relay\n");

	/* Step 1: SPI controller re-init */
	creative_ext_writel(chip, 0xC0C, 0x83); /* init mode */
	creative_ext_writel(chip, 0xC00, 0x0d); /* CMD = I2C target select */
	creative_ext_writel(chip, 0xC0C, 0x03); /* normal mode */
	creative_ext_writew(chip, DSP_REG_STREAM_PARAM, 0x105);

	/*
	 * Step 2: ae9_i2c_init_seq #1-17 — sent as raw bytes to 0xC00.
	 *
	 * The cold-boot MMIO shows all 17 frames fire in <30ms (01:28:07.434-464)
	 * with no per-frame polling. The ca0113 MCU processes them asynchronously.
	 * Using creative_spi_frame() (with 200ms pre-TX polling per frame) takes
	 * 22+ seconds for 17 frames — far too long for a pcm_prepare callback.
	 *
	 * Write bytes directly to 0xC00, matching the cold boot's rapid byte output.
	 * Helper macro sends one complete SPI frame as raw bytes with no polling.
	 */
/*
 * SPI_RAW: send one SPI frame as raw 0xC00 byte writes, paced to the cold-boot
 * timing (~1.5ms between frames, ~60µs between bytes within a frame).
 *
 * The SPI controller has a small TX FIFO. Writing bytes at CPU speed overflows
 * it: subsequent bytes are dropped, the MCU receives garbled data and echoes
 * back the last good byte (0x04) for all subsequent frames. The cold boot writes
 * at ~60µs/byte, which matches the SPI clock rate and keeps the FIFO in sync.
 *
 * We wait for STS=0x06 (controller idle) after the last byte of each frame
 * before sending the next frame. This matches the cold boot's inter-frame gap
 * (~1.5ms observed in mmio_coldboot.txt) and ensures each frame is fully
 * processed before the next one arrives.
 */
#define SPI_RAW(buf) do { \
	int _j; \
	u32 _sts; \
	unsigned int _t; \
	for (_j = 0; _j < (int)ARRAY_SIZE(buf); _j++) { \
		creative_ext_writel(chip, 0xC00, buf[_j]); \
		udelay(60); /* ~60µs/byte: SPI clock rate pacing */ \
	} \
	/* Wait for controller idle (STS=0x06) before next frame */ \
	for (_t = 0; _t < 5000; _t++) { \
		_sts = creative_ext_readl(chip, DSP_REG_STS) & 0xff; \
		if (_sts == 0x06 || _sts == 0x03) \
			break; \
		udelay(1); \
	} \
} while (0)

	/* #1-4: initial handshake */
	SPI_RAW(f_acm1);
	SPI_RAW(f_1mca);
	SPI_RAW(f_dmactrl);
	SPI_RAW(f_probe);

	/* #5-8: repeated handshake */
	SPI_RAW(f_acm1);
	SPI_RAW(f_1mca);
	SPI_RAW(f_dma_long);
	SPI_RAW(f_probe);

	/* #9-16: MCU register configuration */
	SPI_RAW(f_mode1);
	SPI_RAW(f_reg10);
	SPI_RAW(f_reg11);
	SPI_RAW(f_reg12);
	SPI_RAW(f_reg13);
	SPI_RAW(f_mode14);
	SPI_RAW(f_mode15);
	SPI_RAW(f_disp_en);

	/* #17: AE-9 display string → MCU shows "AE-9" + lights knob */
	SPI_RAW(f_ae9);
	dev_info(chip->dev, "pcm_relay_engage: AE-9 display sent\n");

	/* Step 3: WAKE + MSG_SEND x2 (raw — no response needed) */
	SPI_RAW(f_wake);
	SPI_RAW(f_msg_q2);
	SPI_RAW(f_msg_q2);

	/* Step 4: GPIO pin4 HIGH (amp relay power), then back */
	creative_ext_writew(chip, DSP_REG_STREAM_PARAM, 0x104); /* pin4 HIGH */
	udelay(2000);
	creative_ext_writew(chip, DSP_REG_STREAM_PARAM, 0x105); /* back to I2C */

	/*
	 * Steps 4b–5: Interleaved ca0113 0x204 writes and SPI frames,
	 * byte-exact from cold-boot MMIO 01:28:07.514–564:
	 *
	 *   01:28:07.514  0x204=0x0007  trigger reg 7 (relay ack)
	 *   01:28:07.516  SPI: SYS_INFO (f0 81 00 f7)
	 *   01:28:07.519  SPI: Acm1
	 *   01:28:07.534  SPI: 1mcA
	 *   01:28:07.546  0x204=0x8007  unmute: write 0x80 → reg 7
	 *   01:28:07.547  SPI: DMA_CTRL
	 *   01:28:07.564  0x204=0x060a  clock: write 0x06 → reg 0x0a
	 *
	 * CRITICAL: 0x8007 fires BETWEEN 1mcA and DMA_CTRL — NOT before SYS_INFO.
	 * 0x060a fires BETWEEN DMA_CTRL and probe — NOT before SYS_INFO.
	 * All three were previously grouped before SYS_INFO (wrong order).
	 */

	/* 4b-a: trigger reg 7 immediately after GPIO */
	creative_dsp_ca0113_trigger7(chip); /* 0x204=0x0007 */

	/* 5a: SYS_INFO + Acm1 + 1mcA */
	SPI_RAW(f_sysinfo);
	SPI_RAW(f_acm1);
	SPI_RAW(f_1mca);

	/* 5b: unmute (fires between 1mcA and DMA_CTRL) */
	creative_dsp_ca0113_unmute_enable(chip); /* 0x204=0x8007 */
	dev_info(chip->dev, "pcm_relay_engage: ca0113 unmute (0x07=0x80) sent\n");

	/* 5c: DMA_CTRL */
	SPI_RAW(f_dmactrl);

	/* 5d: clock disable (fires between DMA_CTRL and probe) */
	creative_dsp_ca0113_clock_off(chip); /* 0x204=0x060a */

	/* Step 6: WAKE (raw) — puts MCU in probe-ready state */
	SPI_RAW(f_wake);


	/* Step 6: probe frame — check for 0x81 ACK (up to 3 attempts).
	 *
	 * TIMING CRITICAL: cold-boot shows probe→SYS_CTRL gap is 2ms.
	 * Our creative_spi_frame() has pre/post-TX polling (200ms max each),
	 * creating a 2-SECOND gap that causes the MCU to lose its ready state.
	 *
	 * Use SPI_RAW for probe too, then check if 0x81 appeared in the
	 * SPI controller RX buffer (RESP=0x41). This keeps the timing tight.
	 * If 0x81 is found we proceed; if not, we send hb_seq anyway —
	 * the cold boot always fires hb_seq regardless of probe result.
	 */
	for (i = 0; i < 3; i++) {
		int _j;
		u32 _sts;
		/* Send probe via raw writes */
		for (_j = 0; _j < (int)ARRAY_SIZE(f_probe); _j++) {
			creative_ext_writel(chip, 0xC00, f_probe[_j]);
			udelay(60);
		}
		/* Quick drain: check for 0x81 in RX buffer */
		{
			int k;
			unsigned int t;
			for (t = 0; t < 5000; t++) {
				u32 r = creative_ext_readl(chip, DSP_REG_RESP) & 0xff;
				if (r == 0x41) {
					u8 b = (u8)(creative_ext_readl(chip, DSP_REG_CMD) & 0xff);
					if (b == 0x81) {
						probe_ack = true;
						break;
					}
				}
				if (r == 0x40 && t > 0)
					break;
				udelay(1);
			}
			/* Drain remaining bytes */
			for (k = 0; k < 16; k++) {
				u32 r = creative_ext_readl(chip, DSP_REG_RESP) & 0xff;
				if (r != 0x41) break;
				u8 b = (u8)(creative_ext_readl(chip, DSP_REG_CMD) & 0xff);
				if (b == 0x81) probe_ack = true;
				if (b == 0xf7) break;
			}
		}
		if (probe_ack)
			break;
		udelay(5000);
	}
	dev_info(chip->dev, "pcm_relay_engage: probe %s\n",
		 probe_ack ? "ACK'd (0x81)" : "no ACK");

	/* Step 7: GPIO pin4 HIGH before relay frames (raw — immediate after probe) */
	creative_ext_writew(chip, DSP_REG_STREAM_PARAM, 0x105); /* GPIO5 = I2C target */

	/*
	 * Step 8: Full ACM init sequence — ae9_build approach.
	 *
	 * Based on ae9_build-main/build/ca0132.c ae9_acm_init() which produces
	 * a confirmed relay click. Key differences from our previous approach:
	 *
	 * 1. NO READS on BAR2+0xC00-0xC7F between writes. ae9_build comment:
	 *    "Windows VFIO trace shows ZERO reads on this range. Reads may be
	 *    interpreted as commands by the I2C controller." Our old
	 *    creative_i2c_send_tx() read after every byte — this may have been
	 *    confusing the MCU and preventing relay3 from engaging.
	 *
	 * 2. Full 29-packet sequence before relay commands, including the
	 *    authentication KEY packet (0x55 0x07 ... 0xde 0xc0 0xad 0xde)
	 *    which may unlock the MCU to accept 0xb1 relay commands.
	 *
	 * 3. GPIO4 HIGH set AFTER relay3 (hp amp power), matching ae9_build
	 *    where ca0113_mmio_gpio_set(4, true) follows ae9_acm_init().
	 *
	 * 4. 500µs between packets (ae9_build usleep_range(500, 1000)).
	 *
	 * Packet format: writel(0xf0), writel(data[i]...), writel(0xf7)
	 * Each writel goes to BAR2+0xC00 (DSP_REG_CMD = 0xC00).
	 */
#define ACM_SEND(data) do {						\
	int _i;								\
	creative_ext_writel(chip, DSP_REG_CMD, 0xf0);			\
	for (_i = 0; _i < (int)ARRAY_SIZE(data); _i++)			\
		creative_ext_writel(chip, DSP_REG_CMD, (data)[_i]);	\
	creative_ext_writel(chip, DSP_REG_CMD, 0xf7);			\
	usleep_range(500, 1000);					\
} while (0)

	/* Phase 1: handshake + key (unlocks MCU for relay commands) */
	{
		static const u8 tx_reset[]  = {0x81,0x00};
		static const u8 tx_hs_fwd[] = {0x54,0x04,0x41,0x63,0x6d,0x31};
		static const u8 tx_hs_rev[] = {0x54,0x04,0x31,0x6d,0x63,0x41};
		static const u8 tx_cfg[]    = {0xd5,0x03,0x00,0x20,0x04};
		static const u8 tx_stchk[]  = {0x54,0x04,0x11,0x11,0x11,0x11};
		static const u8 tx_key[]    = {
			0x55,0x07,0x00,0x20,0x04,0xde,0xc0,0xad,0xde};
		ACM_SEND(tx_reset);
		ACM_SEND(tx_hs_fwd);
		ACM_SEND(tx_hs_rev);
		ACM_SEND(tx_cfg);
		ACM_SEND(tx_stchk);
		ACM_SEND(tx_key);
		usleep_range(5000, 10000); /* ae9_build: 5ms gap after key */
	}

	/* Phase 2: audio config */
	{
		static const u8 tx_gpio1[]  = {0x03,0x03,0x05,0x03,0x03};
		static const u8 tx_sr48[]   = {0x32,0x03,0x02,0xb8,0x0b};
		static const u8 tx_vol[]    = {0x43,0x04,0x64,0x00,0xf4,0x01};
		static const u8 tx_sr96[]   = {0x32,0x03,0x01,0x88,0x13};
		static const u8 tx_ch[]     = {0x05,0x03,0x02,0x01,0x00};
		static const u8 tx_unmute[] = {0x22,0x02,0x01,0x00};
		static const u8 tx_mute2[]  = {0x22,0x02,0x02,0x01};
		static const u8 tx_gpio2[]  = {0x03,0x03,0x02,0x00,0x40};
		ACM_SEND(tx_gpio1);
		ACM_SEND(tx_sr48);
		ACM_SEND(tx_vol);
		ACM_SEND(tx_sr96);
		ACM_SEND(tx_ch);
		ACM_SEND(tx_unmute);
		ACM_SEND(tx_mute2);
		ACM_SEND(tx_gpio2);
	}

	/* Phase 3: DAC routing → relay sequence → relay click */
	{
		static const u8 tx_name[]   = {
			0x11,0x09,0x41,0x45,0x2d,0x39,
			0x00,0x00,0x00,0x00,0x00};
		static const u8 tx_pwr[]    = {0x21,0x03,0x02,0x00,0x00};
		static const u8 tx_out1[]   = {0x83,0x01,0x02};
		static const u8 tx_out2[]   = {0x83,0x01,0x07};
		static const u8 tx_reset2[] = {0x81,0x00};
		static const u8 tx_query[]  = {0xc2,0x00};
		static const u8 tx_insel[]  = {0x85,0x01,0x02};
		static const u8 tx_dac1[]   = {0xb1,0x01,0x01};
		static const u8 tx_dac2[]   = {0xb1,0x01,0x02};
		static const u8 tx_dac3[]   = {0xb1,0x01,0x03}; /* relay click */
		static const u8 tx_lbl_hp[] = {
			0x11,0x09,0x2d,0x48,0x50,0x2d,
			0x00,0x00,0x00,0x00,0x00};
		/* TX#26: label "-24.5" — intermediate volume display */
		static const u8 tx_lbl_245[] = {
			0x11,0x09,0x2d,0x32,0x34,0x2e,0x35,
			0x00,0x00,0x00,0x00};
		static const u8 tx_gpio3[]  = {0x03,0x03,0x02,0x40,0x40};
		/* TX#28: label "-15.0" — final volume display (completes MCU state) */
		static const u8 tx_lbl_150[] = {
			0x11,0x09,0x2d,0x31,0x35,0x2e,0x30,
			0x00,0x00,0x00,0x00};
		static const u8 tx_unmute2[]= {0x22,0x02,0x01,0x01};
		ACM_SEND(tx_name);
		ACM_SEND(tx_pwr);
		ACM_SEND(tx_out1);
		ACM_SEND(tx_out2);
		ACM_SEND(tx_reset2);
		usleep_range(2000, 5000); /* ae9_build: 2ms gap before query */
		ACM_SEND(tx_query);
		ACM_SEND(tx_insel);
		ACM_SEND(tx_dac1);
		ACM_SEND(tx_dac2);
		ACM_SEND(tx_dac3); /* → MCU engages HP output relay */
		ACM_SEND(tx_lbl_hp);
		ACM_SEND(tx_lbl_245); /* -24.5 intermediate volume */
		ACM_SEND(tx_gpio3);
		ACM_SEND(tx_lbl_150); /* -15.0 final volume → completes MCU state */
		ACM_SEND(tx_unmute2);
	}

	n = 3; /* ae9_build doesn't read relay3 response — set n=3 to indicate sent */
	dev_info(chip->dev, "pcm_relay_engage: relay frames sent (ae9_build path)\n");

	/*
	 * GPIO4 HIGH — HP amp power enable, AFTER relay sequence.
	 * ae9_build: ca0113_mmio_gpio_set(4, true) fires AFTER ae9_acm_init().
	 * This releases the HP amp from shutdown and produces the audible click.
	 */
	creative_ext_writew(chip, DSP_REG_STREAM_PARAM, 0x104); /* GPIO4 HIGH */
	msleep(20); /* ae9_build: msleep(20) after GPIO4 */

	/*
	 * Step 9: ca0113 DAC route AFTER relay click.
	 *
	 * Cold-boot MMIO shows the 0x3b burst (0x0f/10/04/05/02/03 = 0x3b)
	 * fires at 01:28:07.756 — 49ms after relay3 at 01:28:07.707.
	 * This is the final PCM format commit that routes audio to the output.
	 *
	 * Previously this ran at probe time (~15s before PCM prepare) and was
	 * stale by the time audio started. Running it here, immediately after
	 * the relay click, matches the cold-boot timing exactly.
	 *
	 * headunit_dac_route() sends 35 ca0113_play_cmd calls to 0x48/0x49
	 * via the command bus (0x204/0x20c), including the 0x3b burst:
	 *   0x48/0x0f=0x3b, 0x48/0x10=0x3b
	 *   0x49/0x04=0x3b, 0x49/0x05=0x3b
	 *   0x49/0x02=0x3b, 0x49/0x03=0x3b
	 */
	creative_dsp_headunit_dac_route(chip);
	dev_info(chip->dev, "pcm_relay_engage: dac_route sent post-relay\n");

#undef SPI_RAW

	/* Restart the heartbeat so it keeps the relay engaged during playback. */
	queue_delayed_work(chip->wq, &chip->hb_work, 0);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Head-unit amp/DAC enable block — the transactions the Windows driver	  */
/* sends to the head unit WHEN AUDIO STARTS (capture playback window),	  */
/* which our 18-tx init does NOT. The 0x83/0x85/0xb1 register writes are	  */
/* the per-stage output enable/unmute that engage the DAC relay (the	  */
/* "click") and the amp. Without these, digital routing is correct but	  */
/* the analog output stays in standby (silent, no click).		  */
/* ------------------------------------------------------------------ */
int creative_i2c_amp_enable(struct creative_chip *chip)
{
	const struct ae9_i2c_init_entry amp_seq[] = {
		{ 0x81, 0, NULL },	/* identify */
		{ 0x54, 4, (const u8[]){ 0x41, 0x63, 0x6d, 0x31 } }, /* Acm1 */
		{ 0x54, 4, (const u8[]){ 0x31, 0x6d, 0x63, 0x41 } }, /* 1mcA */
		{ 0xd5, 3, (const u8[]){ 0x00, 0x20, 0x04 } },
		{ 0x54, 4, (const u8[]){ 0x11, 0x11, 0x11, 0x11 } },
		{ 0xc2, 0, NULL },
		{ 0x83, 1, (const u8[]){ 0x02 } },
		{ 0x83, 1, (const u8[]){ 0x02 } },
		{ 0x83, 1, (const u8[]){ 0x07 } },
		{ 0x85, 1, (const u8[]){ 0x02 } },
		{ 0x83, 1, (const u8[]){ 0x02 } },
		{ 0x83, 1, (const u8[]){ 0x07 } },
		{ 0xb1, 1, (const u8[]){ 0x01 } },	/* output enable L */
		{ 0xb1, 1, (const u8[]){ 0x02 } },	/* output enable R */
		{ 0xb1, 1, (const u8[]){ 0x03 } },	/* output enable / unmute */
	};
	u32 resp;
	int i, ret;

	creative_ext_writew(chip, DSP_REG_STREAM_PARAM, 0x105);
	creative_ext_readw(chip, DSP_REG_STREAM_PARAM);

	dev_info(chip->dev, "head-unit amp enable: %zu transactions\n",
		 ARRAY_SIZE(amp_seq));

	for (i = 0; i < ARRAY_SIZE(amp_seq); i++) {
		resp = 0;
		ret = creative_i2c_send_raw(chip, amp_seq[i].reg,
					    amp_seq[i].len, amp_seq[i].data,
					    &resp);
		if (ret < 0)
			dev_info(chip->dev,
				 "amp enable tx#%d reg=0x%02x failed %d\n",
				 i, amp_seq[i].reg, ret);
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
 * Send one SPI frame byte-by-byte via BAR2+0xC00.
 * Pattern from capture: write byte, read back, read status.
 * Returns the response byte read back after each write.
 */
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
 * Send one SPI frame and receive the response (full-duplex).
 *
 * The AE-9 SPI controller is full-duplex: MISO carries the MCU's
 * response frame while MOSI clocks out the TX frame simultaneously.
 * Response bytes become available (C14=0x41) during and after TX.
 *
 * Protocol confirmed from cold-boot capture (L36940-L40560):
 *
 * PRE-TX: poll until C7C=0x06 twice (fully idle, no pending MISO)
 *
 * TX phase: for each byte:
 *   W C00 = byte
 *   R C00  (echo — discard, used only for debug)
 *   R C7C  (status)
 *   If C14=0x41 (MISO byte ready): drain it into resp[] immediately
 *
 * POST-TX: after last byte, keep draining C14=0x41 until C14=0x40
 *   and C7C=0x06 (fully idle), collecting remaining response bytes.
 *   Stop when 0xf7 (frame terminator) received.
 */
static int creative_spi_frame(struct creative_chip *chip,
			      const u8 *tx, int tx_len,
			      u8 *resp, int resp_len)
{
	int i, n = 0;
	u32 r, sts;
	unsigned int timeout;
	u8 first_echo = 0xff;

	/*
	 * PRE-TX: wait for controller fully idle.
	 * Capture pattern: read C7C twice both returning 0x06 before TX.
	 * This drains any stale MISO bytes from previous frame.
	 */
	timeout = 200000;
	while (timeout > 0) {
		sts = creative_ext_readl(chip, DSP_REG_STS)  & 0xff;
		r   = creative_ext_readl(chip, DSP_REG_RESP) & 0xff;
		if (sts == 0x06 && r == 0x40) {
			/* confirm with second read (capture does two C7C reads) */
			sts = creative_ext_readl(chip, DSP_REG_STS) & 0xff;
			if (sts == 0x06)
				break;
		}
		udelay(1);
		timeout--;
	}

	/* TX phase: send each byte, collect any MISO bytes that arrive */
	for (i = 0; i < tx_len; i++) {
		u32 echo;

		creative_ext_writel(chip, DSP_REG_CMD, tx[i]);
		echo = creative_ext_readl(chip, DSP_REG_CMD) & 0xff;
		sts  = creative_ext_readl(chip, DSP_REG_STS) & 0xff;

		if (i == 0)
			first_echo = (u8)echo;

		/* Drain any MISO byte that became ready during this TX byte */
		r = creative_ext_readl(chip, DSP_REG_RESP) & 0xff;
		if (r == 0x41 && n < resp_len) {
			u8 b = (u8)(creative_ext_readl(chip, DSP_REG_CMD) & 0xff);
			creative_ext_readl(chip, DSP_REG_STS);
			if (resp)
				resp[n] = b;
			n++;
			if (b == I2C_STOP)
				goto done;
		}
	}

	/* POST-TX: drain remaining MISO bytes until frame end or idle */
	timeout = 100000; /* 100ms */
	while (n < resp_len && timeout > 0) {
		r = creative_ext_readl(chip, DSP_REG_RESP) & 0xff;
		if (r == 0x41) {
			u8 b = (u8)(creative_ext_readl(chip, DSP_REG_CMD) & 0xff);
			creative_ext_readl(chip, DSP_REG_STS);
			if (resp)
				resp[n] = b;
			n++;
			if (b == I2C_STOP)
				break;
			timeout = 100000; /* reset on each byte */
		} else if (r == 0x40 && n > 0) {
			break; /* frame done, controller idle */
		} else {
			udelay(1);
			timeout--;
		}
	}

done:
	/* Compact trace */
	if (tx_len >= 2) {
		char rbuf[32] = "";
		int k;
		for (k = 0; k < n && k < 8; k++)
			snprintf(rbuf + k*3, 4, "%02x ", resp ? resp[k] : 0);
		dev_info(chip->dev,
			 "SPI cmd=0x%02x echo0=0x%02x rx[%d]=%s\n",
			 tx[1], first_echo, n, rbuf);
	}

	return n;
}

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
	int i, n = 0;

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

			if (resp && n < resp_len)
				resp[n] = b;
			n++;
			if (b == I2C_STOP && n > 1)
				return n;
			idle = 0;
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

/* Public: send one raw SPI frame via the DSP SPI controller (BAR2+0xC00).
 * Used for the head-unit shutdown frame during driver remove.
 */
void creative_spi_frame_send(struct creative_chip *chip,
			     const u8 *tx, int tx_len)
{
	creative_spi_frame(chip, tx, tx_len, NULL, 0);
}

/*
 * Start the 8051 audio loop via BAR2 SPI (region2+0xC00).
 *
 * Handles both cold boot (0x801c=0 after firmware) and warm boot
 * (0x801c non-zero = loop already running from prior session).
 *
 * Cold path (0x801c=0):
 *   Phase 1 — Wake MCU (<12ms, no delays):
 *     f0 21 03 02 00 00 f7  → wake/ping (MCU echoes f0 21 01 00 f7)
 *     f0 83 01 07 f7        → MSG_SEND queue=7
 *     f0 85 01 02 f7        → MSG_STATUS queue=2
 *     f0 83 01 02 f7        → MSG_SEND queue=2
 *     f0 83 01 07 f7        → MSG_SEND queue=7
 *     f0 b1 01 01/02/03 f7  → PARAM_SET id=1,2,3
 *   Phase 2 — Probe loop (up to 8× at 450ms):
 *     f0 81 00 f7           → SYS_INFO
 *     f0 54 04 "Acm1" f7   → MEM_WRITE
 *     f0 54 04 "1mcA" f7   → MEM_WRITE
 *     f0 d5 03 ... f7       → DMA_CTRL
 *     f0 54 04 11111111 f7  → probe (ACK=0x81 = MCU ready)
 *   Phase 3 — Trigger (immediately after probe ACK):
 *     f0 c2 00 f7           → SYS_CTRL
 *     MSG_SEND/STATUS drain sequence
 *     PARAM_SET x3
 *     f0 11 09 "-HP-..." f7 → HELLO (ACK=0x00 = audio loop started)
 * Warm path (0x801c≠0): skip all phases, send HELLO directly.
 *
 * Sequence from vm_capture_coldboot_20260603_124746 (MOSI+MISO confirmed).
 */
int creative_i2c_dsp_audio_loop_start(struct creative_chip *chip)
{
	/* Phase 1: wake/ping frame — MUST be first; MCU ignores all else */
	static const u8 f_wake[]     = { 0xf0, 0x21, 0x03,
					  0x02, 0x00, 0x00, 0xf7 };
	/* Phase 2: probe loop frames */
	static const u8 f_sysinfo[]  = { 0xf0, 0x81, 0x00, 0xf7 };
	static const u8 f_acm1[]     = { 0xf0, 0x54, 0x04,
					  0x41, 0x63, 0x6d, 0x31, 0xf7 };
	static const u8 f_1mca[]     = { 0xf0, 0x54, 0x04,
					  0x31, 0x6d, 0x63, 0x41, 0xf7 };
	static const u8 f_dmactrl[]  = { 0xf0, 0xd5, 0x03,
					  0x00, 0x20, 0x04, 0xf7 };
	static const u8 f_probe[]    = { 0xf0, 0x54, 0x04,
					  0x11, 0x11, 0x11, 0x11, 0xf7 };
	/* Phase 3: trigger frames */
	static const u8 f_sysctrl[]  = { 0xf0, 0xc2, 0x00, 0xf7 };
	static const u8 f_msg_q2[]   = { 0xf0, 0x83, 0x01, 0x02, 0xf7 };
	static const u8 f_msg_q7[]   = { 0xf0, 0x83, 0x01, 0x07, 0xf7 };
	static const u8 f_msg_sts[]  = { 0xf0, 0x85, 0x01, 0x02, 0xf7 };
	static const u8 f_param1[]   = { 0xf0, 0xb1, 0x01, 0x01, 0xf7 };
	static const u8 f_param2[]   = { 0xf0, 0xb1, 0x01, 0x02, 0xf7 };
	static const u8 f_param3[]   = { 0xf0, 0xb1, 0x01, 0x03, 0xf7 };
	static const u8 f_hello[]    = { 0xf0, 0x11, 0x09,
					  0x2d, 0x48, 0x50, 0x2d,
					  0x00, 0x00, 0x00, 0x00, 0x00, 0xf7 };
	u8 resp[16];
	unsigned int loop_val = 0;
	int attempt, n;
	bool warm_boot;

	/* ── Hardware state dump ─────────────────────────────────────────── */
	dev_info(chip->dev,
		 "audio_loop_start: C0C=0x%02x C14=0x%02x C7C=0x%02x\n",
		 creative_ext_readl(chip, DSP_REG_CTRL) & 0xff,
		 creative_ext_readl(chip, DSP_REG_RESP) & 0xff,
		 creative_ext_readl(chip, DSP_REG_STS)  & 0xff);

	/*
	 * Run i2c_setup before any SPI frame.
	 *
	 * The cold-boot capture shows that creative_i2c_setup()
	 * (C0C=0x83, C00=0x0d, C04=0x00, C0C=0x03, 0x320=0x105) runs
	 * immediately before EVERY probe/SPI frame sequence, including the
	 * first f0 81 probe (capture L36590) and the f0 21 wake frame
	 * (capture L40120+). The CMD=0x0d write is what routes the SPI
	 * controller to the correct mux path — without it, writes to C00
	 * are received by the controller (C7C transitions) but the MCU
	 * never clocks them out (echo0=0x00, C14 never reaches 0x41).
	 */
	creative_i2c_setup(chip);
	dev_info(chip->dev,
		 "audio_loop_start: after i2c_setup: C0C=0x%02x C14=0x%02x C7C=0x%02x\n",
		 creative_ext_readl(chip, DSP_REG_CTRL) & 0xff,
		 creative_ext_readl(chip, DSP_REG_RESP) & 0xff,
		 creative_ext_readl(chip, DSP_REG_STS)  & 0xff);

	/* ── Check 0x801c — already alive? ──────────────────────────────── */
	creative_dsp_chipio_read(chip, 0x801c, &loop_val);
	/* Treat QEMU sentinel as cold boot — not a real audio counter */
	if (loop_val == 0x0bad0add)
		loop_val = 0;
	warm_boot = (loop_val != 0);
	dev_info(chip->dev,
		 "audio_loop_start: 0x801c=0x%08x (%s boot)\n",
		 loop_val, warm_boot ? "WARM" : "COLD");

	if (warm_boot) {
		/* Warm path: audio loop already running, just resend HELLO
		 * to re-trigger relay/display without full probe loop.
		 */
		dev_info(chip->dev,
			 "audio_loop_start: warm boot — sending HELLO directly\n");
		n = creative_spi_frame(chip, f_hello, ARRAY_SIZE(f_hello),
				       resp, 16);
		{
			int k;
			bool ackd = false;
			for (k = 0; k < n; k++)
				if (resp[k] == 0x00) { ackd = true; break; }
			dev_info(chip->dev,
				 "audio_loop_start: warm HELLO n=%d resp=%02x %02x %02x %s\n",
				 n, n>0?resp[0]:0xff, n>1?resp[1]:0xff,
				 n>2?resp[2]:0xff, ackd ? "ACK'd" : "not ACK'd");
			return ackd ? 0 : -EIO;
		}
	}

	/*
	 * ── Phase 1: Wake MCU ────────────────────────────────────────────
	 *
	 * The MCU boots into a low-power "waiting for SPI" state. It will
	 * silently ignore SYS_INFO and all probe frames until it receives
	 * the wake/ping frame f0 21 03 02 00 00 f7. The MCU responds with
	 * f0 21 01 00 f7 and is then ready to process probe commands.
	 *
	 * All Phase 1 frames must be sent without delays (<12ms total) —
	 * the MCU has an ~80ms ready window after firmware boot, and the
	 * full Phase 1 must fit inside it.
	 *
	 * Source: cold-boot capture L58961-L59200, timing ~80ms after FW.
	 */
	dev_info(chip->dev, "audio_loop_start: Phase 1 — waking MCU\n");

	n = creative_spi_frame(chip, f_wake, ARRAY_SIZE(f_wake), resp, 16);
	dev_info(chip->dev,
		 "  wake: n=%d resp=[%02x %02x %02x %02x %02x] (expect f0 21 01 00 f7)\n",
		 n,
		 n>0?resp[0]:0, n>1?resp[1]:0, n>2?resp[2]:0,
		 n>3?resp[3]:0, n>4?resp[4]:0);

	/* MSG/PARAM handshake immediately after wake — no delays */
	creative_spi_frame(chip, f_msg_q7,  ARRAY_SIZE(f_msg_q7),  resp, 16);
	creative_spi_frame(chip, f_msg_sts, ARRAY_SIZE(f_msg_sts), resp, 16);
	creative_spi_frame(chip, f_msg_q2,  ARRAY_SIZE(f_msg_q2),  resp, 16);
	creative_spi_frame(chip, f_msg_q7,  ARRAY_SIZE(f_msg_q7),  resp, 16);
	creative_spi_frame(chip, f_param1,  ARRAY_SIZE(f_param1),  resp, 16);
	creative_spi_frame(chip, f_param2,  ARRAY_SIZE(f_param2),  resp, 16);
	creative_spi_frame(chip, f_param3,  ARRAY_SIZE(f_param3),  resp, 16);

	/*
	 * ── Phase 2: Probe loop ───────────────────────────────────────────
	 *
	 * Poll MCU readiness: send SYS_INFO + MEM_WRITEs and look for
	 * ACK=0x81 in the probe response. Up to 8 attempts, 450ms apart.
	 * MCU transitions to ready state typically after 1-3 attempts.
	 *
	 * Source: cold-boot capture L59300-L79460.
	 */
	dev_info(chip->dev, "audio_loop_start: Phase 2 — probe loop\n");

	for (attempt = 0; attempt < 32; attempt++) {
		dev_info(chip->dev,
			 "audio_loop_start: probe attempt %d\n", attempt);

		n = creative_spi_frame(chip, f_sysinfo,
				       ARRAY_SIZE(f_sysinfo), resp, 16);
		dev_info(chip->dev, "  sysinfo: n=%d [%02x %02x %02x %02x %02x]\n",
			 n, n>0?resp[0]:0, n>1?resp[1]:0, n>2?resp[2]:0,
			 n>3?resp[3]:0, n>4?resp[4]:0);
		/* The probe response sometimes arrives one cycle late — during
		 * the *next* SYS_INFO TX instead of the probe TX itself.
		 * Check here too so we don't miss the ACK.
		 */
		{
			int k;
			for (k = 0; k < n; k++)
				if (resp[k] == 0x81) goto dsp_ready;
		}

		/* Also scan acm1/1mca responses — the probe ACK sometimes
		 * arrives here due to the one-frame pipeline delay.
		 */
		n = creative_spi_frame(chip, f_acm1, ARRAY_SIZE(f_acm1), resp, 16);
		{ int k; for (k = 0; k < n; k++) if (resp[k] == 0x81) goto dsp_ready; }

		n = creative_spi_frame(chip, f_1mca, ARRAY_SIZE(f_1mca), resp, 16);
		{ int k; for (k = 0; k < n; k++) if (resp[k] == 0x81) goto dsp_ready; }

		n = creative_spi_frame(chip, f_dmactrl,
				       ARRAY_SIZE(f_dmactrl), resp, 16);
		dev_info(chip->dev, "  dmactrl: n=%d [%02x %02x %02x %02x %02x %02x %02x]\n",
			 n, n>0?resp[0]:0, n>1?resp[1]:0, n>2?resp[2]:0,
			 n>3?resp[3]:0, n>4?resp[4]:0, n>5?resp[5]:0, n>6?resp[6]:0);
		{ int k; for (k = 0; k < n; k++) if (resp[k] == 0x81) goto dsp_ready; }

		n = creative_spi_frame(chip, f_probe,
				       ARRAY_SIZE(f_probe), resp, 16);
		{
			/* Probe ready response: f0 54 01 81 f7 — 0x81 at [3].
			 * Scan all bytes in case frame shifts slightly.
			 */
			bool ack = false;
			int k;
			for (k = 0; k < n; k++)
				if (resp[k] == 0x81) { ack = true; break; }
			dev_info(chip->dev,
				 "  probe: n=%d resp=[%02x %02x %02x %02x %02x] ACK=%s\n",
				 n, n>0?resp[0]:0, n>1?resp[1]:0, n>2?resp[2]:0,
				 n>3?resp[3]:0, n>4?resp[4]:0,
				 ack ? "0x81 READY!" : "not ready");
			if (ack)
				goto dsp_ready;
		}

		/*
		 * No inter-attempt delay. The cold-boot capture shows the
		 * probe loop runs continuously — all 3 cycles complete in
		 * ~100ms total (t=44.310s to t=44.410s). A 450ms delay was
		 * wrong and kept the MCU in a busy/not-ready state (0xaa).
		 * The MCU transitions to ready (~0x81) quickly under rapid
		 * continuous probing. Use a short yield only.
		 */
		usleep_range(5000, 10000); /* 5-10ms — let MCU breathe */
	}

	dev_warn(chip->dev, "audio_loop_start: DSP never became ready\n");
	return -ETIMEDOUT;

dsp_ready:
	/*
	 * ── Phase 3: Trigger ─────────────────────────────────────────────
	 *
	 * MCU is ready. Send SYS_CTRL + MSG queue drain + PARAM_SET x3
	 * then HELLO "-HP-". MCU responds with ACK=0x00 in resp[2] to
	 * confirm the audio loop has started. Relay clicks, HP displayed.
	 *
	 * Source: cold-boot capture L79460-L80327.
	 *
	 * Allow 500ms for the MCU to fully transition from "probe ready"
	 * to "audio loop ready" state before sending Phase 3. If Phase 3
	 * arrives too quickly after 0x81, the MCU returns only f7 (frame
	 * end) to HELLO instead of the 0x00 ACK. The cold-boot capture
	 * shows ~200-400ms between the last 0x81 and the first Phase 3 frame.
	 */
	msleep(1000);
	dev_info(chip->dev, "audio_loop_start: Phase 3 — trigger\n");

	/*
	 * HELLO retry loop — attempt up to 3 times with 300ms between.
	 * The MCU timing is variable; sometimes 0x81 arrives early and
	 * the audio engine needs more time. Retrying the full Phase 3
	 * sequence gives the MCU multiple chances to ACK.
	 */
	{
		int hello_try;
		for (hello_try = 0; hello_try < 3; hello_try++) {
			int k;
			bool has_ack = false, has_busy = false;

			if (hello_try > 0) {
				dev_info(chip->dev,
					 "audio_loop_start: HELLO retry %d\n",
					 hello_try);
				msleep(300);
			}

			creative_spi_frame(chip, f_sysctrl,
					   ARRAY_SIZE(f_sysctrl), resp, 16);
			creative_spi_frame(chip, f_msg_q2,
					   ARRAY_SIZE(f_msg_q2),  resp, 16);
			creative_spi_frame(chip, f_msg_q2,
					   ARRAY_SIZE(f_msg_q2),  resp, 16);
			creative_spi_frame(chip, f_msg_q7,
					   ARRAY_SIZE(f_msg_q7),  resp, 16);
			creative_spi_frame(chip, f_msg_sts,
					   ARRAY_SIZE(f_msg_sts), resp, 16);
			creative_spi_frame(chip, f_msg_q2,
					   ARRAY_SIZE(f_msg_q2),  resp, 16);
			creative_spi_frame(chip, f_msg_q2,
					   ARRAY_SIZE(f_msg_q2),  resp, 16);
			creative_spi_frame(chip, f_msg_q7,
					   ARRAY_SIZE(f_msg_q7),  resp, 16);
			creative_spi_frame(chip, f_param1,
					   ARRAY_SIZE(f_param1),  resp, 16);
			creative_spi_frame(chip, f_param2,
					   ARRAY_SIZE(f_param2),  resp, 16);
			creative_spi_frame(chip, f_param3,
					   ARRAY_SIZE(f_param3),  resp, 16);

			n = creative_spi_frame(chip, f_hello,
					       ARRAY_SIZE(f_hello), resp, 16);
			dev_info(chip->dev,
				 "audio_loop_start: HELLO n=%d resp=[%02x %02x %02x %02x]\n",
				 n, n>0?resp[0]:0, n>1?resp[1]:0,
				 n>2?resp[2]:0, n>3?resp[3]:0);

			for (k = 0; k < n; k++) {
				if (resp[k] == 0x00) has_ack = true;
				if (resp[k] == 0x81) has_busy = true;
			}
			if (has_ack && !has_busy) {
				dev_info(chip->dev,
					 "audio_loop_start: HELLO ACK'd — 8051 audio loop started!\n");
				return 0;
			}
		}
	}

	dev_warn(chip->dev,
		 "audio_loop_start: HELLO not ACK'd after retries\n");
	return -EIO;
}
