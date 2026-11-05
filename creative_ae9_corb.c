// SPDX-License-Identifier: GPL-2.0-only
//
// CORB / RIRB ring buffer management for Creative AE-9 HDA controller
//
// Reference: ae9_mmio_capture.txt lines 180-330, docs/history/Creative.md §3.4
//

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <sound/hda_register.h>

#include "creative_ae9.h"

/* ------------------------------------------------------------------ */
/* CORB / RIRB initialization					  */
/* ae9_mmio_capture.txt:180-330					  */
/* ------------------------------------------------------------------ */
int creative_ae9_corb_init(struct creative_chip *chip)
{
	u32 corb_addr_low, corb_addr_high;
	u32 rirb_addr_low, rirb_addr_high;
	u32 ctl;

	/* Reuse the rings parked by corb_free() (suspend/resume) */
	if (!chip->corb_buf && chip->corb_keep) {
		chip->corb_buf = chip->corb_keep;
		chip->corb_dma = chip->corb_keep_dma;
		chip->rirb_buf = chip->rirb_keep;
		chip->rirb_dma = chip->rirb_keep_dma;
		memset(chip->rirb_buf, 0, CREATIVE_RIRB_SIZE);
	}

	/* Allocate DMA-coherent buffer for CORB (256 * 4 = 1 KB) */
	if (!chip->corb_buf) {
		chip->corb_buf = dmam_alloc_coherent(chip->dev,
				CREATIVE_CORB_SIZE, &chip->corb_dma, GFP_KERNEL);
		if (!chip->corb_buf) {
			dev_err(chip->dev, "Failed to allocate CORB DMA buffer\n");
			return -ENOMEM;
		}
	}

	/* Allocate DMA-coherent buffer for RIRB (256 * 8 = 2 KB).
	 * May already be allocated by probe (pre-alloc for ICW priming).
	 * Reuse it so RIRBWP pre-seeded from ICW responses is preserved.
	 */
	if (!chip->rirb_buf) {
		chip->rirb_buf = dmam_alloc_coherent(chip->dev,
				CREATIVE_RIRB_SIZE, &chip->rirb_dma, GFP_KERNEL);
		if (!chip->rirb_buf) {
			dev_err(chip->dev, "Failed to allocate RIRB DMA buffer\n");
			return -ENOMEM;
		}
		memset(chip->rirb_buf, 0, CREATIVE_RIRB_SIZE);
	}

	memset(chip->corb_buf, 0, CREATIVE_CORB_SIZE);

	chip->corb_wp = 0;

	/* Init sequence verified against three sources:
	 *   1. mmio_coldboot.txt  — VFIO MMIO trace of Windows driver
	 *   2. M0000000082C00000.rw — live RWEverything BAR0 dump (working state)
	 *   3. CtxHda.sys bytecode tables (6 confirmed RIRBCTL=0xc0 entries)
	 *
	 * Register values in the working Windows state (standard HDA layout):
	 *   CORBCTL (0x4c) = 0x03        bit 1 CORBRUN + bit 0 CMEIE (memory
	 *                                error interrupt enable)
	 *   RINTCNT (0x5a) = 0xc0        raise a response interrupt every 192
	 *                                responses
	 *   RIRBCTL (0x5c) = 0x07        bit 0 RINTCTL (response interrupt),
	 *                                bit 1 DMAEN, bit 2 RIRBOIC (overrun irq)
	 *   INTCTL  (0x20) = 0xc0000000  bit 31 GIE + bit 30 CIE (controller
	 *                                interrupts: RIRB, CORB error, codec wake)
	 * CIE is on, so RIRB interrupts do reach the CPU; the IRQ handler only
	 * clears RIRBSTS and leaves the ring to the verb code.
	 */
#define CA0132_ICS      0x68   /* Immediate Command Status (word)         */

	/* Step 1: ICS handoff from ICW mode to CORB mode.
	 * Fresh cold-boot capture (mmio_fresh.txt lines 148-153):
	 *   write byte 0x68 = 0x05  (set ICB — abort any in-flight ICW)
	 *   write byte 0x68 = 0x00  (clear ICB)
	 *   write byte 0x68 = 0x04  (set IRCE)
	 *   read  word 0x68 × 2     (confirm IRCE=1, ICB=0)
	 *   write word 0x68 = 0x06  (IRCE=1 + ICB=0 → arm CORB mode)
	 * This transitions the CA0132 response path from ICR (0x64) to the
	 * RIRB DMA ring buffer.  Without it, RIRBWP never advances.
	 * The ACK of any pending ICW response (IRV bit) happens implicitly
	 * when ICS is written.
	 */
	creative_hda_writeb(chip, CA0132_ICS, 0x05);
	udelay(1);
	creative_hda_writeb(chip, CA0132_ICS, 0x00);
	udelay(1);
	creative_hda_writeb(chip, CA0132_ICS, 0x04);
	udelay(10);
	writew(0x06, chip->hda_regs + CA0132_ICS);
	wmb();
	udelay(10);

	/* Step 2: disable CORB and RIRB staged-enable */
	creative_hda_writeb(chip, AZX_REG_CORBCTL, 0x00);
	creative_hda_writeb(chip, AZX_REG_RIRBCTL, 0x00);

	/* Step 3: CORBCTL = 0x03 and RIRBCTL = 0x07 BEFORE base addresses.
	 * Fresh capture confirms this order (lines 187-188).
	 */
	creative_hda_writeb(chip, AZX_REG_CORBCTL, 0x03);
	creative_hda_writeb(chip, AZX_REG_RIRBCTL, 0x07);
	wmb();

	/* Step 3: write CORB and RIRB base addresses */
	corb_addr_low  = (u32)(chip->corb_dma & 0xffffffff);
	corb_addr_high = (u32)((chip->corb_dma >> 32) & 0xffffffff);
	creative_hda_writel(chip, AZX_REG_CORBUBASE, corb_addr_high);
	creative_hda_writel(chip, AZX_REG_CORBLBASE, corb_addr_low);

	rirb_addr_low  = (u32)(chip->rirb_dma & 0xffffffff);
	rirb_addr_high = (u32)((chip->rirb_dma >> 32) & 0xffffffff);
	creative_hda_writel(chip, AZX_REG_RIRBUBASE, rirb_addr_high);
	creative_hda_writel(chip, AZX_REG_RIRBLBASE, rirb_addr_low);

	/* Step 4: INTCTL — CIE (bit 30), then GIE (bit 31) + CIE, as Windows
	 * does. Stream interrupt enables (bits 0-29) are set per stream in
	 * creative_ae9_stream_start.
	 */
	creative_hda_writel(chip, AZX_REG_INTCTL, 0x40000000);
	creative_hda_writel(chip, AZX_REG_INTCTL, 0xc0000000);

	/* Step 5: RINTCNT (0x5a) = 0xc0: response interrupt every 192
	 * responses (CtxHda.sys tables and the live MMIO dump both use 0xc0).
	 */
	creative_hda_writeb(chip, AZX_REG_RINTCNT, 0xc0);

	/* Step 6: RIRBCTL (0x5c) staged enable: 0x01 -> 0x05 -> 0x07, as in
	 * mmio_coldboot.txt lines 241-246 and 280-285:
	 *   0x01 = response interrupt (RINTCTL)
	 *   0x05 = + overrun interrupt (RIRBOIC)
	 *   0x07 = + RIRB DMA run (DMAEN)
	 */
	creative_hda_writeb(chip, AZX_REG_RIRBCTL, 0x01);
	udelay(10);
	creative_hda_writeb(chip, AZX_REG_RIRBCTL, 0x05);
	udelay(10);
	creative_hda_writeb(chip, AZX_REG_RIRBCTL, 0x07);
	wmb();

	/* Step 7: CORBCTL = 0x01 (memory-error interrupt only, CORB DMA not
	 * running yet). The base-address write leaves CORBCTL at 0x00 (fresh
	 * capture readback); the first corb_send_verb sets 0x03, which starts
	 * the CORB DMA (CORBRUN).
	 */
	creative_hda_writeb(chip, AZX_REG_CORBCTL, 0x01);
	wmb();

	/* Do NOT reset CORBRP or RIRBWP.
	 * Windows capture shows RIRBWP is already non-zero (from prior ICW
	 * verb activity) when CORB starts — it is never reset to 0.
	 * Resetting RIRBWP confuses the CA0132 RIRB engine.
	 * Seed our software read pointer from the current hardware value.
	 */
	chip->rirb_rp = readw(chip->hda_regs + AZX_REG_RIRBWP) & 0xff;

	/* CORB/RIRB sizes: 256 entries (bits 3-2 = 10 = 0x0a).
	 * CA0132 appears to ignore CORBSIZE (live dump shows 0x00 with
	 * CORBWP=58 and no wrap), but we set it for spec compliance.
	 */
	creative_hda_writeb(chip, AZX_REG_CORBSIZE, 0x0a);
	creative_hda_writeb(chip, AZX_REG_RIRBSIZE, 0x0a);

	/* Seed CORBWP from the current CORBRP so the engine starts cleanly */
	chip->corb_wp = readw(chip->hda_regs + AZX_REG_CORBRP) & 0xff;
	creative_hda_writew(chip, AZX_REG_CORBWP, chip->corb_wp);

	/* Clear any stale RIRB interrupt status (W1C) */
	creative_hda_writeb(chip, AZX_REG_RIRBSTS, 0x07);

	udelay(10);

	ctl = creative_hda_readb(chip, AZX_REG_CORBCTL);
	dev_info(chip->dev, "CORBCTL=0x%02x (expected 0x01)\n", ctl);
	ctl = creative_hda_readb(chip, AZX_REG_RINTCNT);
	dev_info(chip->dev, "RINTCNT=0x%02x (expected 0xc0)\n", ctl);
	dev_info(chip->dev, "RIRBCTL=0x%02x (expected 0x07)\n",
		 creative_hda_readb(chip, AZX_REG_RIRBCTL));
	dev_info(chip->dev, "INTCTL=0x%08x (expected 0xc0000000)\n",
		 creative_hda_readl(chip, AZX_REG_INTCTL));

	dev_info(chip->dev, "CORB at DMA 0x%llx, RIRB at DMA 0x%llx\n",
		 (unsigned long long)chip->corb_dma,
		 (unsigned long long)chip->rirb_dma);

	if (creative_ae9_gone(chip, "CORB/RIRB start"))
		return -ENODEV;
	return 0;
}

/* ------------------------------------------------------------------ */
/* CORB / RIRB cleanup						  */
/* ------------------------------------------------------------------ */
void creative_ae9_corb_free(struct creative_chip *chip)
{
	/* Disable CORB DMA */
	creative_hda_writeb(chip, AZX_REG_CORBCTL, 0);
	/* Disable RIRB DMA */
	creative_hda_writeb(chip, AZX_REG_RIRBCTL, 0);

	/* park the (device-managed) buffers for the next corb_init; with
	 * corb_buf NULL every verb path falls back to immediate commands */
	if (chip->corb_buf) {
		chip->corb_keep = chip->corb_buf;
		chip->corb_keep_dma = chip->corb_dma;
		chip->rirb_keep = chip->rirb_buf;
		chip->rirb_keep_dma = chip->rirb_dma;
	}
	chip->corb_buf = NULL;
	chip->corb_dma = 0;
	chip->rirb_buf = NULL;
	chip->rirb_dma = 0;
}

/* ------------------------------------------------------------------ */
/* Send a verb via CORB						  */
/* Returns 0 on success (verb queued), negative on error		  */
/* ae9_mmio_capture.txt:290-293					  */
/* ------------------------------------------------------------------ */
int creative_ae9_corb_send_verb(struct creative_chip *chip, u32 verb)
{
	u16 wp;
	u16 next_wp;
	u16 rp;
	u32 *corb;

	/* rings parked (suspend) or not set up: nothing to send through */
	if (!chip->corb_buf || chip->card_gone)
		return -ENODEV;

	wp = readw(chip->hda_regs + AZX_REG_CORBWP);
	rp = readw(chip->hda_regs + AZX_REG_CORBRP);

	next_wp = (wp + 1) & (CREATIVE_CORB_ENTRIES - 1);

	/* Check for CORB-full condition */
	if (next_wp == rp) {
		dev_err_once(chip->dev, "CORB full (WP=%u, RP=%u)\n", wp, rp);
		return -EBUSY;
	}

	/* Write verb to the NEXT slot (next_wp), then advance CORBWP to it.
	 * HDA spec: CORBWP=N means "entry N is the newest valid entry".
	 * The controller reads the entry AT CORBWP, so the verb must be
	 * placed at next_wp before CORBWP is updated to next_wp.
	 */
	corb = (u32 *)chip->corb_buf;
	WRITE_ONCE(corb[next_wp], cpu_to_le32(verb));

	/* Ensure verb is written to DMA buffer before updating WP */
	wmb();

	creative_ae9_vlog(verb, 0, false);

	/* Update CORB write pointer — this triggers the controller */
	writew(next_wp, chip->hda_regs + AZX_REG_CORBWP);
	wmb();

	/* CORBCTL = 0x03 (CORBRUN + memory-error interrupt) after every
	 * CORBWP advance, as the Windows capture does. Only the first one
	 * changes anything (it starts the CORB DMA); later ones are no-ops.
	 */
	writeb(0x03, chip->hda_regs + AZX_REG_CORBCTL);
	wmb();

	/* Acknowledge any pending RIRB status (W1C, matches capture) */
	writeb(0x05, chip->hda_regs + AZX_REG_RIRBSTS);

	chip->corb_wp = next_wp;

	return 0;
}

/* ------------------------------------------------------------------ */
/* Send a verb via CORB, spinning until CORB space is available.	  */
/* Used for large bulk bursts (e.g. 1183-verb DSP audio config) that  */
/* would overflow the 256-entry CORB ring if fired at CPU speed.	  */
/* Caller must hold corb_mutex.					  */
/* ------------------------------------------------------------------ */
void creative_ae9_corb_send_verb_wait(struct creative_chip *chip, u32 verb)
{
	unsigned int timeout = 5000; /* ~5ms max spin at 1µs per iteration */

	if (!chip->corb_buf)
		return;
	while (timeout--) {
		if (creative_ae9_corb_send_verb(chip, verb) == 0 || chip->card_gone)
			return;
		/* CORB full: spin 1µs and retry */
		udelay(1);
	}
	/* If still full after 5ms, drop the verb — better than deadlock */
	dev_err_ratelimited(chip->dev, "CORB still full after 5ms, dropping verb 0x%08x\n", verb);
}

/* ------------------------------------------------------------------ */
/* Send a verb via CORB and wait for the response			  */
/* Returns 0 on success, -ETIMEDOUT on timeout			  */
/* ------------------------------------------------------------------ */
int creative_ae9_corb_send_verb_sync(struct creative_chip *chip,
				     u32 verb, u32 *resp)
{
	u16 wp_start;
	u16 wp;
	unsigned int timeout;
	int ret;

	/* Serialise: only one CORB verb in flight at a time.
	 * Without this, concurrent callers (ACM heartbeat, PCM prepare,
	 * select_lineout) interleave verbs and lose responses.
	 */
	if (chip->card_gone)
		return -ENODEV;
	mutex_lock(&chip->corb_mutex);
	if (!chip->corb_buf || !chip->rirb_buf) {	/* rings parked (suspend) */
		mutex_unlock(&chip->corb_mutex);
		return -ENODEV;
	}

	/* Drain any pre-existing RIRB entries by reading current WP. Count
	 * the unsolicited ones on the way: the DSP announces each SCP reply
	 * with one, and they often land between our commands. */
	wp_start = readw(chip->hda_regs + AZX_REG_RIRBWP);
	wp_start &= 0xff;
	{
		struct creative_rirb_entry *rirb = chip->rirb_buf;
		u16 i = chip->rirb_rp;

		while (i != wp_start) {
			u32 fl;

			i = (i + 1) & 0xff;
			fl = le32_to_cpu(READ_ONCE(rirb[i].flags));
			if (fl & 0x10) {
				creative_ae9_vlog(le32_to_cpu(READ_ONCE(rirb[i].response)),
						  fl, true);
				if ((fl & 0xf) == 1)
					atomic_inc(&chip->scp_unsol);
			}
		}
	}
	chip->rirb_rp = wp_start;
	writew(wp_start, chip->hda_regs + AZX_REG_RIRBWP);

	/* Send the verb via CORB */
	ret = creative_ae9_corb_send_verb(chip, verb);
	if (ret < 0) {
		mutex_unlock(&chip->corb_mutex);
		return ret;
	}

	/* Poll for our verb's RIRB response, skipping unsolicited entries.
	 * The 8051 sends unsolicited responses (RIRB flags bit 4 set) when
	 * it starts the audio loop.  Consume and skip them so we don't
	 * mistake them for our solicited verb response.
	 */
	{
		struct creative_rirb_entry *rirb = chip->rirb_buf;
		timeout = CREATIVE_IC_CMD_TIMEOUT_MS * 100;
		wp = wp_start;
		do {
			u16 cur_wp = readw(chip->hda_regs + AZX_REG_RIRBWP) & 0xff;
			while (cur_wp != wp) {
				u16 next = (wp + 1) & 0xff;
				u32 fl = le32_to_cpu(READ_ONCE(rirb[next].flags));
				wp = next;
				if (!(fl & 0x10))
					goto got_response;
				/* unsolicited — log/count it, keep scanning */
				creative_ae9_vlog(le32_to_cpu(READ_ONCE(rirb[next].response)),
						  fl, true);
				if ((fl & 0xf) == 1)
					atomic_inc(&chip->scp_unsol);
				cur_wp = readw(chip->hda_regs + AZX_REG_RIRBWP) & 0xff;
			}
			udelay(10);
		} while (--timeout);
	}

	if (creative_ae9_gone(chip, "CORB verb")) {
		mutex_unlock(&chip->corb_mutex);
		return -ENODEV;
	}
	dev_err(chip->dev,
		"CORB verb response timeout (verb=0x%08x)\n", verb);

	/*
	 * Recover the command ring after a timeout, the HD-Audio spec way.
	 * The timed-out verb is still pending (CORBRP behind CORBWP). Setting
	 * CORBRP bit 15 resets the read pointer to 0; the value written in the
	 * low bits is ignored. So stop the CORB, reset the read pointer
	 * (waiting for the controller to show the bit set, then clear), and
	 * also set CORBWP to 0: with both at 0 the ring is truly empty and
	 * nothing old is re-run. The next verb goes to entry 1. The previous
	 * code wrote RP=WP with the reset bit, which left RP at 0 and WP ahead,
	 * so the controller would re-send stale entries.
	 */
	{
		u16 cur_rirb_wp = readw(chip->hda_regs + AZX_REG_RIRBWP) & 0xff;
		int t;

		writeb(0x00, chip->hda_regs + AZX_REG_CORBCTL);	/* stop */
		for (t = 0; t < 100 && (readb(chip->hda_regs + AZX_REG_CORBCTL) & 0x02); t++)
			udelay(10);
		writew(0x8000, chip->hda_regs + AZX_REG_CORBRP);	/* RP reset */
		for (t = 0; t < 100 && !(readw(chip->hda_regs + AZX_REG_CORBRP) & 0x8000); t++)
			udelay(10);
		writew(0x0000, chip->hda_regs + AZX_REG_CORBRP);
		for (t = 0; t < 100 && (readw(chip->hda_regs + AZX_REG_CORBRP) & 0x8000); t++)
			udelay(10);
		writew(0x0000, chip->hda_regs + AZX_REG_CORBWP);	/* WP = 0 */
		chip->corb_wp = 0;
		writeb(0x03, chip->hda_regs + AZX_REG_CORBCTL);	/* run + error irq */

		/* skip responses that may still arrive for the dropped verb */
		chip->rirb_rp = cur_rirb_wp;
		dev_warn(chip->dev, "CORB ring reset after timeout (RP=WP=0)\n");
	}

	mutex_unlock(&chip->corb_mutex);
	return -ETIMEDOUT;

got_response:
	/* Read solicited response from RIRB buffer */
	{
		struct creative_rirb_entry *rirb = chip->rirb_buf;
		u32 r = le32_to_cpu(READ_ONCE(rirb[wp].response));

		creative_ae9_vlog(r, le32_to_cpu(READ_ONCE(rirb[wp].flags)), true);
		if (resp)
			*resp = r;
	}

	/* Update RIRB read pointer to acknowledge consumption */
	chip->rirb_rp = wp;
	writew(wp, chip->hda_regs + AZX_REG_RIRBWP);
	wmb();

	mutex_unlock(&chip->corb_mutex);
	return 0;
}
