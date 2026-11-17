// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 3xHelix
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
#define CORBCTL_RUN	0x03	/* run + memory-error irq */

/*
 * ring_order: 0 = our long-standing order: the run bits are set
 * before the ring base addresses, the ring sizes after the rings run.
 * 1 = Windows' effective order and the HD Audio spec's: both DMA engines
 * stopped (and seen stopped) while the base addresses and sizes are set,
 * then switched on. Windows writes the run bits too, but resets the
 * controller right after, which clears them (cold capture 2026-11-03,
 * 23:24:10.544-.608). For ASMedia-bridged cards (bugzilla 219552), where
 * a stray ring DMA before the bases are valid may be what takes the
 * bridge down. -1 (default) = 1 behind an ASMedia bridge (the order
 * Kaiba's card first played with, round 3d.1), 0 otherwise.
 */
static int ring_order = -1;
module_param(ring_order, int, 0444);
MODULE_PARM_DESC(ring_order, "Command-ring start order: -1 = auto (Windows' order behind an ASMedia bridge, default), 0 = as before, 1 = addresses and sizes while stopped, as Windows and the HDA spec");

static void rirb_ring_clear(struct creative_chip *chip);
static void rirb_ring_seeded(struct creative_chip *chip);

int creative_ae9_corb_init(struct creative_chip *chip)
{
	bool order = ring_order < 0 ? chip->asmedia : !!ring_order;
	u32 corb_addr_low, corb_addr_high;
	u32 rirb_addr_low, rirb_addr_high;
	u32 ctl;

	/* a new ring: whatever the router chip had selected is unknown now */
	creative_ae9_r51_reset(chip);

	/* Reuse the rings parked by corb_free() (suspend/resume) */
	if (!chip->corb_buf && chip->corb_keep) {
		chip->corb_buf = chip->corb_keep;
		chip->corb_dma = chip->corb_keep_dma;
		chip->rirb_buf = chip->rirb_keep;
		chip->rirb_dma = chip->rirb_keep_dma;
	}

	/* Allocate DMA-coherent buffer for CORB (256 * 4 = 1 KB) */
	if (!chip->corb_buf) {
		chip->corb_buf = dmam_alloc_coherent(chip->dev,
				ae9_dma_size(chip, CREATIVE_CORB_SIZE), &chip->corb_dma, GFP_KERNEL);
		if (!chip->corb_buf) {
			dev_err(chip->dev, "Failed to allocate CORB DMA buffer\n");
			return -ENOMEM;
		}
	}

	/* Allocate DMA-coherent buffer for RIRB (256 * 8 = 2 KB), or reuse
	 * the one an earlier init allocated (resume, ring restarts). */
	if (!chip->rirb_buf) {
		chip->rirb_buf = dmam_alloc_coherent(chip->dev,
				ae9_dma_size(chip, CREATIVE_RIRB_SIZE), &chip->rirb_dma, GFP_KERNEL);
		if (!chip->rirb_buf) {
			dev_err(chip->dev, "Failed to allocate RIRB DMA buffer\n");
			return -ENOMEM;
		}
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

	if (order) {
		unsigned int t;

		/* wait (up to 1 ms) for both DMA engines to report stopped */
		for (t = 0; t < 100; t++) {
			if (!(creative_hda_readb(chip, AZX_REG_CORBCTL) & 0x02) &&
			    !(creative_hda_readb(chip, AZX_REG_RIRBCTL) & 0x02))
				break;
			udelay(10);
		}
		rirb_ring_clear(chip);
	} else {
		rirb_ring_clear(chip);	/* the DMA runs again just below */
		/* Step 3: CORBCTL = 0x03 and RIRBCTL = 0x07 BEFORE base
		 * addresses, as the first two writes of the capture (which
		 * then resets the controller; see ring_order). */
		creative_hda_writeb(chip, AZX_REG_CORBCTL, CORBCTL_RUN);
		creative_hda_writeb(chip, AZX_REG_RIRBCTL, 0x07);
		wmb();
	}

	/* Step 3: write CORB and RIRB base addresses */
	corb_addr_low  = (u32)(chip->corb_dma & 0xffffffff);
	corb_addr_high = (u32)((chip->corb_dma >> 32) & 0xffffffff);
	creative_hda_writel(chip, AZX_REG_CORBUBASE, corb_addr_high);
	creative_hda_writel(chip, AZX_REG_CORBLBASE, corb_addr_low);

	rirb_addr_low  = (u32)(chip->rirb_dma & 0xffffffff);
	rirb_addr_high = (u32)((chip->rirb_dma >> 32) & 0xffffffff);
	creative_hda_writel(chip, AZX_REG_RIRBUBASE, rirb_addr_high);
	creative_hda_writel(chip, AZX_REG_RIRBLBASE, rirb_addr_low);

	/* ring_order=1: the sizes too while stopped (256 entries) */
	if (order) {
		creative_hda_writeb(chip, AZX_REG_CORBSIZE, 0x0a);
		creative_hda_writeb(chip, AZX_REG_RIRBSIZE, 0x0a);
		wmb();
	}

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

	/* rings running: unsolicited responses have somewhere to go again.
	 * Only when corb_free switched them off: at the first ring start the
	 * bring-up already has them on, and the extra GCTL read and write
	 * right after the RIRB DMA starts took an ASMedia-bridged card off
	 * the bus (bugzilla 219552, round 3 test 1). No read: write the
	 * value the bring-up uses. */
	if (chip->unsol_parked) {
		creative_hda_writel(chip, AZX_REG_GCTL,
				    AZX_GCTL_RESET | AZX_GCTL_UNSOL);
		chip->unsol_parked = false;
	}

	/* Do NOT reset CORBRP or RIRBWP.
	 * Windows capture shows RIRBWP is already non-zero (from prior ICW
	 * verb activity) when CORB starts — it is never reset to 0.
	 * Resetting RIRBWP confuses the CA0132 RIRB engine.
	 * Seed our software read pointer from the current hardware value.
	 */
	{
		u16 raw = readw(chip->hda_regs + AZX_REG_RIRBWP);

		if (raw == 0xffff) {	/* one bad read? (a dead card fails below) */
			udelay(10);
			raw = readw(chip->hda_regs + AZX_REG_RIRBWP);
		}
		chip->rirb_rp = raw == 0xffff ? 0 : raw & 0xff;
	}
	rirb_ring_seeded(chip);

	/* CORB/RIRB sizes: 256 entries (bits 3-2 = 10 = 0x0a).
	 * CA0132 appears to ignore CORBSIZE (live dump shows 0x00 with
	 * CORBWP=58 and no wrap), but we set it for spec compliance.
	 */
	if (!order) {
		creative_hda_writeb(chip, AZX_REG_CORBSIZE, 0x0a);
		creative_hda_writeb(chip, AZX_REG_RIRBSIZE, 0x0a);
	}

	/* Seed CORBWP from the current CORBRP so the engine starts cleanly */
	chip->corb_wp = readw(chip->hda_regs + AZX_REG_CORBRP) & 0xff;
	creative_hda_writew(chip, AZX_REG_CORBWP, chip->corb_wp);

	/* Clear any stale RIRB interrupt status (W1C) */
	creative_hda_writeb(chip, AZX_REG_RIRBSTS, 0x07);

	udelay(10);

	ctl = creative_hda_readb(chip, AZX_REG_CORBCTL);
	dev_info(chip->dev, "CORBCTL=0x%02x (expected 0x%02x)%s\n", ctl, 0x01,
		 order ? " (ring_order=1: addresses and sizes set while stopped)" : "");
	ctl = creative_hda_readb(chip, AZX_REG_RINTCNT);
	dev_info(chip->dev, "RINTCNT=0x%02x (expected 0xc0)\n", ctl);
	dev_info(chip->dev, "RIRBCTL=0x%02x (expected 0x%02x)\n",
		 creative_hda_readb(chip, AZX_REG_RIRBCTL), 0x07);
	dev_info(chip->dev, "INTCTL=0x%08x (expected 0x%08x)\n",
		 creative_hda_readl(chip, AZX_REG_INTCTL), 0xc0000000);

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
	/* no RIRB for unsolicited responses (the DSP's SCP reply
	 * announcements) to land in: switch them off until the rings
	 * start again, so they can't jam the immediate interface */
	if (!chip->card_gone) {
		creative_hda_writel(chip, AZX_REG_GCTL,
				    creative_hda_readl(chip, AZX_REG_GCTL) & ~AZX_GCTL_UNSOL);
		chip->unsol_parked = true;
	}

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
	writeb(CORBCTL_RUN, chip->hda_regs + AZX_REG_CORBCTL);
	wmb();

	/* Acknowledge any pending RIRB status (W1C, matches capture) */
	writeb(0x05, chip->hda_regs + AZX_REG_RIRBSTS);

	chip->corb_wp = next_wp;

	return 0;
}

/* ------------------------------------------------------------------ */
/* Send a verb via CORB and wait for the response			  */
/* Returns 0 on success, -ETIMEDOUT on timeout			  */
/*
 * One unsolicited RIRB entry: log it, and count it as a DSP reply
 * announcement only if it is one: codec 1, tag 0x16 (the DSP node; resp
 * 0x02c00001). Windows' handler filters on (resp >> 21) & 0x1f == 0x16
 * (decompile FUN_0001da88); counting every codec-1 entry would count a
 * jack event as a reply (2026-11-07).
 */
static void rirb_unsol(struct creative_chip *chip, u32 res, u32 fl)
{
	creative_ae9_vlog(res, fl, true);
	if ((fl & 0xf) == 1 && ((res >> 21) & 0x1f) == 0x16)
		atomic_inc(&chip->scp_unsol);
}

/*
 * RIRB entries that land after RIRBWP moves (2026-11-15, uv71-uv75). The
 * controller can advance RIRBWP before the entry it points at is in
 * memory; read at once, the slot still holds what was written there one
 * lap (256 entries) earlier. Linux has worked around the same thing on
 * Creative X-Fi controllers since 2010 (commit 14d34f16, "Add some workarounds
 * for Creative IBG": 80 us before every RIRB read; now under
 * AZX_DCAPS_CTX_WORKAROUND).
 * What it did here: a stale DSP notice (0x02c00001) read in place of an
 * answer made the real answer, which landed a moment later in the slot we
 * had passed, look lost: 100 ms timeout, command-ring reset, replies out
 * of step, then capture DMA to made-up addresses and a card that stopped
 * answering (uv71, after SBX toggles while recording). A stale 0 read as a
 * DSP reply header lost that reply (3-12 per 150 GETs), and stale entries
 * from the other codec came back as answers (27 in uv74).
 * So every slot read gets RIRB_MARK (all ones; real flags have bits 31:5
 * clear), and a slot RIRBWP says is new that still holds the mark is read
 * again until the entry lands, for up to RIRB_LATE_US. uv75 (SBX toggled
 * every 3 s, four captures): 1282 entries landed late, at most 6 us;
 * 0 timeouts, 0 lost headers, 0 wrong-codec answers (uv74: 12 and 27).
 */
#define RIRB_MARK	0xffffffffu
#define RIRB_LATE_US	200

/*
 * A slot given up on (still marked after RIRB_LATE_US) isn't forgotten
 * (ultrareview 2, #3 and #4, Batch C; research 2026-11-15,
 * batch-c-rirb-design.md): it goes into a watch set, and every pass over the
 * ring checks the watched slots first. One that has landed since is read and
 * marked like any other, so it can't come back a lap later as a "new" stale
 * entry (#4); if it's the answer to the verb in flight it is used (#3); if
 * it's a DSP notice it's counted (scp_unsol stays right). A watched slot
 * still marked when the ring has moved RIRB_WATCH_GUARD entries past it is
 * dropped from the watch (safe: it still holds the mark). Nothing here needs
 * the card to write its entries in order. Never seen on a real card so far
 * (rirb_never 0 in every run, worst lateness 8 us of the 200 waited);
 * rirb_inject exercises it.
 */
#define RIRB_DIST(a, b)		(((b) - (a)) & 0xff)
#define RIRB_WATCH_GUARD	128

static unsigned int rirb_inject;
module_param(rirb_inject, uint, 0644);
MODULE_PARM_DESC(rirb_inject, "Test (needs debug=1): after start-up, every Nth landed RIRB entry is hidden (its slot marked again) for rirb_inject_us and given up on, then put back, as an entry landing far too late: the late-entry watch must find it (0 = off)");
static unsigned int rirb_inject_us = 300;
module_param(rirb_inject_us, uint, 0644);
MODULE_PARM_DESC(rirb_inject_us, "Test: how long rirb_inject hides an entry (us, default 300; over 100000 = its verb times out; dropped, as never landed, once the ring has moved 128 entries past it)");
static unsigned int rirb_inject_n;

/* a slot seen landed: read it and mark it */
static u32 rirb_consume(struct creative_rirb_entry *rirb, u16 i)
{
	u32 res;

	rmb();
	res = le32_to_cpu(READ_ONCE(rirb[i].response));
	WRITE_ONCE(rirb[i].response, cpu_to_le32(RIRB_MARK));
	WRITE_ONCE(rirb[i].flags, cpu_to_le32(RIRB_MARK));
	return res;
}

/* rirb_inject: take a landed entry back out of its slot (marked again) */
static bool rirb_inject_hide(struct creative_chip *chip, struct creative_rirb_entry *rirb,
			     u16 i, u32 fl)
{
	if (likely(!rirb_inject) || !ae9_debug || !chip->dsp_done ||
	    fl == RIRB_MARK || chip->rirb_held >= 0)
		return false;
	if (++rirb_inject_n < rirb_inject)
		return false;
	rirb_inject_n = 0;
	chip->rirb_injected++;
	chip->rirb_held = i;
	chip->rirb_held_fl = fl;
	chip->rirb_held_res = rirb_consume(rirb, i);
	chip->rirb_held_until = ktime_add_us(ktime_get(), rirb_inject_us);
	return true;
}

/* ...and put it back once its time is up: it "lands" now */
static void rirb_inject_release(struct creative_chip *chip, struct creative_rirb_entry *rirb)
{
	int i = chip->rirb_held;

	if (likely(i < 0) || ktime_before(ktime_get(), chip->rirb_held_until))
		return;
	WRITE_ONCE(rirb[i].response, cpu_to_le32(chip->rirb_held_res));
	wmb();
	WRITE_ONCE(rirb[i].flags, cpu_to_le32(chip->rirb_held_fl));
	chip->rirb_held = -1;
}

/*
 * Ring init (every one: first, resume, ring restarts, the "already running"
 * re-init), with the RIRB DMA stopped: give a late entry still in flight
 * time to land (worst measured: 8 us), then mark the whole ring and forget
 * the watched slots. A re-init used to keep whatever the ring held.
 */
static void rirb_ring_clear(struct creative_chip *chip)
{
	u16 raw;

	udelay(20);
	mutex_lock(&chip->corb_mutex);
	memset(chip->rirb_buf, 0xff, CREATIVE_RIRB_SIZE);	/* RIRB_MARK */
	bitmap_zero(chip->rirb_watch, CREATIVE_RIRB_ENTRIES);
	chip->rirb_nwatch = 0;
	chip->rirb_send_wp = -1;
	chip->rirb_held = -1;
	raw = readw(chip->hda_regs + AZX_REG_RIRBWP);
	chip->rirb_wp0 = raw == 0xffff ? -1 : raw & 0xff;
	mutex_unlock(&chip->corb_mutex);
}

/*
 * rirb_rp was just seeded from RIRBWP. Entries the card wrote between the
 * mark and the seed (the RIRB DMA runs again before it: at once with
 * ring_order 0) are behind rirb_rp and nobody reads them; mark them, or
 * watch the ones still in flight, so none is read as new a lap later.
 */
static void rirb_ring_seeded(struct creative_chip *chip)
{
	struct creative_rirb_entry *rirb = chip->rirb_buf;
	u16 i;

	mutex_lock(&chip->corb_mutex);
	if (chip->rirb_wp0 >= 0) {
		for (i = chip->rirb_wp0; i != chip->rirb_rp;) {
			i = (i + 1) & 0xff;
			if (le32_to_cpu(READ_ONCE(rirb[i].flags)) != RIRB_MARK)
				rirb_consume(rirb, i);
			else if (!__test_and_set_bit(i, chip->rirb_watch))
				chip->rirb_nwatch++;
		}
	}
	chip->rirb_wp0 = -1;
	mutex_unlock(&chip->corb_mutex);
}

static bool rirb_take(struct creative_chip *chip, struct creative_rirb_entry *rirb,
		      u16 i, u32 *res, u32 *fl)
{
	unsigned int t;

	*fl = le32_to_cpu(READ_ONCE(rirb[i].flags));
	if (unlikely(rirb_inject_hide(chip, rirb, i, *fl)))
		goto give_up;	/* hidden: the watch must find it when it's back */
	if (*fl == RIRB_MARK) {
		for (t = 0; t < RIRB_LATE_US && *fl == RIRB_MARK; t++) {
			udelay(1);
			*fl = le32_to_cpu(READ_ONCE(rirb[i].flags));
		}
		if (*fl == RIRB_MARK) {
			chip->rirb_never++;
			creative_ae9_health_notify(chip);
			dev_info_ratelimited(chip->dev, "RIRB entry %u not landed after %u us: given up, watched\n",
					     i, RIRB_LATE_US);
			goto give_up;
		}
		chip->rirb_late++;
		if (t > chip->rirb_late_max_us)
			chip->rirb_late_max_us = t;
	}
	*res = rirb_consume(rirb, i);
	return true;

give_up:
	if (!__test_and_set_bit(i, chip->rirb_watch))
		chip->rirb_nwatch++;
	return false;
}

/* only an entry written after the verb in flight was sent can be its answer */
static bool rirb_after_send(struct creative_chip *chip, u16 z, u16 hw)
{
	unsigned int d;

	if (chip->rirb_send_wp < 0)
		return false;
	d = RIRB_DIST((u16)chip->rirb_send_wp, z);
	return d && d <= RIRB_DIST((u16)chip->rirb_send_wp, hw);
}

/*
 * The watched slots, against write pointer hw. want = the codec whose answer
 * we wait for (-1: none); true = res and fl hold it.
 */
static bool rirb_sweep(struct creative_chip *chip, u16 hw, int want, u32 *res, u32 *fl)
{
	struct creative_rirb_entry *rirb = chip->rirb_buf;
	unsigned int z;
	bool got = false;

	if (likely(!chip->rirb_nwatch))
		return false;
	rirb_inject_release(chip, rirb);
	for_each_set_bit(z, chip->rirb_watch, CREATIVE_RIRB_ENTRIES) {
		u32 f = le32_to_cpu(READ_ONCE(rirb[z].flags)), r;
		bool mine = false;

		if (f == RIRB_MARK) {
			/* how far the card has written past it: 0 = it's still
			 * the newest slot (the usual case: the answer waited for) */
			if (RIRB_DIST(z, hw) < RIRB_WATCH_GUARD)
				continue;	/* keep watching */
			chip->rirb_wexpired++;	/* still marked: safe as it is */
			if (z == chip->rirb_held)	/* rirb_inject: it "never landed" */
				chip->rirb_held = -1;
			goto drop;
		}
		r = rirb_consume(rirb, z);
		chip->rirb_wlanded++;
		if (f & 0x10) {
			rirb_unsol(chip, r, f);
		} else if (!got && want >= 0 && (f & 0xf) == want &&
			   rirb_after_send(chip, z, hw)) {
			*res = r;
			*fl = f;
			got = mine = true;
			chip->rirb_wanswer++;
		} else {
			creative_ae9_vlog(r, f, true);	/* an old answer: dropped */
		}
		dev_info_ratelimited(chip->dev, "RIRB entry %u landed after it was given up%s\n",
				     z, mine ? ": the answer waited for" : "");
drop:
		__clear_bit(z, chip->rirb_watch);
		chip->rirb_nwatch--;
	}
	return got;
}

/* RIRBWP's bits 15:8 read 0 on a live card: all-ones is a card off the
 * bus or one bad read (a bridge glitch); never use it as a write pointer.
 * Counted when the card is still there, to learn how often that happens. */
static bool rirb_wp_bad(struct creative_chip *chip, u16 raw, const char *where)
{
	if (raw != 0xffff)
		return false;
	if (!creative_ae9_gone(chip, where)) {
		chip->rirb_badwp++;
		creative_ae9_health_notify(chip);
		dev_warn_ratelimited(chip->dev, "RIRB: write pointer read all-ones (%s), card still there (%u so far)\n",
				     where, chip->rirb_badwp);
	}
	return true;
}

/*
 * Look for DSP reply announcements WITHOUT sending a verb: scan the RIRB
 * the DMA has written since our last look. Windows sends nothing to the
 * DSP between a GET (and its one trailing F01) and the announcement (606
 * of 606 GETs, 15 captures; notes 24); we used to poll F01 every
 * 0.2-0.4 ms there, the one place our reads differ from Windows'.
 * A solicited entry here can only be a late reply to a timed-out verb.
 */
void creative_ae9_corb_scan_unsol(struct creative_chip *chip)
{
	struct creative_rirb_entry *rirb;
	u16 raw, wp, i;

	if (chip->card_gone)
		return;
	mutex_lock(&chip->corb_mutex);
	rirb = chip->rirb_buf;	/* under the mutex: corb_free clears it */
	if (!rirb) {
		mutex_unlock(&chip->corb_mutex);
		return;
	}
	raw = readw(chip->hda_regs + AZX_REG_RIRBWP);
	/* all-ones (bits 15:8 read 0 on a live card): gone, or one bad read
	 * through a bridge; either way don't move rirb_rp to 0xff
	 * (ultrareview 2) */
	if (rirb_wp_bad(chip, raw, "RIRB scan")) {
		mutex_unlock(&chip->corb_mutex);
		return;
	}
	wp = raw & 0xff;
	rmb();
	rirb_sweep(chip, wp, -1, NULL, NULL);
	for (i = chip->rirb_rp; i != wp;) {
		u32 fl, res;

		i = (i + 1) & 0xff;
		if (rirb_take(chip, rirb, i, &res, &fl) && (fl & 0x10))
			rirb_unsol(chip, res, fl);
	}
	chip->rirb_rp = wp;
	mutex_unlock(&chip->corb_mutex);
}

/*
 * A card that stays on the bus but whose codecs stop answering (2026-11-13,
 * 13:29:45, rapid SBX on/off: one timeout, then the card wrote outside its
 * buffers and never answered again). The driver went on sending: 1420
 * timeouts, each 100 ms plus a ring reset, and 2 s DSP waits, for 4 minutes
 * until a reload; only the reload's bus reset brought the card back.
 * A healthy card has single timeouts (two that evening, each answered at
 * the next verb), so CARD_HUNG_RUN in a row on a running card (after the
 * bring-up, which has its own retries) means it has stopped: say so once and
 * fail every command at once until the next bring-up (reload, resume).
 */
#define CARD_HUNG_RUN	8

static unsigned int hung_inject;
module_param(hung_inject, uint, 0644);
MODULE_PARM_DESC(hung_inject, "Test (needs debug=1): the next N commands after start-up count as timed out without being sent, to exercise the 'card stopped answering' stop (counts down to 0)");

static void ae9_corb_hung_check(struct creative_chip *chip, u32 verb)
{
	if (!chip->dsp_done || chip->card_hung ||
	    ++chip->corb_to_run < CARD_HUNG_RUN)
		return;
	chip->card_hung = true;
	chip->hung_verb = verb;
	creative_ae9_dma_dump(chip, "card stopped answering");
	creative_ae9_dma_quiesce(chip);
	/* not while unloading: stop_works has cancelled dead_work and the
	 * PCMs are freed or about to be (ultrareview 2, #1: the shutdown's
	 * own timeouts re-queued it, use-after-free) */
	if (!chip->unloading)
		schedule_work(&chip->dead_work);
	creative_ae9_dead_notify(chip);
	dev_err(chip->dev, "card stopped answering: %u command timeouts in a row (last verb 0x%08x, %u in all); no more commands until the driver is reloaded (sudo modprobe -r snd-hda-codec-creative-ae9 && sudo modprobe snd-hda-codec-creative-ae9) or the PC is powered off\n",
		chip->corb_to_run, verb, chip->corb_timeouts);
}

/* ------------------------------------------------------------------ */
static int ae9_corb_send_verb_sync(struct creative_chip *chip, u32 verb, u32 *resp);

/*
 * The router chip (8051) takes its parameters through codec 1 node 0x15:
 * 0x717 selects a parameter, 0x718 writes its value (deferred job 7D87 reads
 * the selected id when it runs), 0x711-0x716 are its port and stream
 * requests. An accepted 0x712/0x713/0x715/0x716/0x718 sets its busy flag
 * (25h.1, ROM 2563) until that job ran; while it's set, 0x711-0x713 and
 * 0x715-0x718 are REFUSED: reply 1, the verb dropped (0x714 and 0x710 never
 * are). A refused 0x717 followed by an accepted 0x718 writes the value into
 * the PREVIOUS parameter. Windows waits for the busy status first
 * (FUN_0001b344 -> FUN_0001b270: F01 polled while it reads 1, up to 101
 * times) and every one of its 0x717/0x718 got reply 0 in 28 captures. We
 * sent the captured status reads but ignored their answers (F01 read busy
 * 162 times in 9000 in uv79's log): at the first load after power-on the
 * start-up tail's "select stream 0x18" was refused (verb log 2026-11-07,
 * reply 1 twice), stream 0x18 connected to its ROM default DAC point 0xd1
 * and its DAC groups stayed allocated until a power-off: the 8051's own
 * DSP-mode routing failed every time after (research 2026-11-15,
 * 8051-0xd1-leak.md; repaired by hand in uv87, after which the 8051 routed
 * DSP mode itself).
 * So: wait for the busy status to clear, check the reply and send again on
 * a refusal; after 0x712/0x713 wait for its job too (Windows reads the
 * port result only after that). A value write is never sent after a lost
 * select (it would land on the previous parameter): counted and refused
 * (-EBUSY). router_poll=0: as before.
 */
static bool router_poll = true;
module_param(router_poll, bool, 0644);
MODULE_PARM_DESC(router_poll, "Wait for the router chip's busy status before each of its parameter verbs and resend a refused one, as Windows (1 = yes, default; 0 = send blind, as before 0.12.11)");

#define AE9_R51_STATUS		0x115f0100	/* F01: router chip busy (1) or idle (0) */
#define AE9_R51_IDLE_TRIES	200		/* ~10 ms of status reads */
#define AE9_R51_TRIES		4

static bool ae9_r51_param_verb(u32 v)
{
	unsigned int id = (v >> 8) & 0xfff;

	return (v >> 20) == 0x115 && id >= 0x711 && id <= 0x718;
}

static void ae9_r51_wait_idle(struct creative_chip *chip)
{
	unsigned int i, n = chip->r51_stuck ? 1 : AE9_R51_IDLE_TRIES;
	u32 r = 0;

	for (i = 0; i < n; i++) {
		if (ae9_corb_send_verb_sync(chip, AE9_R51_STATUS, &r) < 0)
			return;
		if (!(r & 0xff)) {
			chip->r51_stuck = false;
			if (i)	/* it was busy: sent blind, this one could have been refused */
				chip->r51_waited++;
			return;
		}
	}
	if (chip->r51_stuck)
		return;
	/* its job queue overflowed and the flag stays set until a reset:
	 * one read per verb from now on, not 200 */
	chip->r51_stuck = true;
	chip->r51_never_idle++;
	dev_warn_ratelimited(chip->dev, "router chip: still busy after %u status reads (0x%02x)\n",
			     i, r & 0xff);
}

/* a new router chip (bring-up): nothing selected yet */
void creative_ae9_r51_reset(struct creative_chip *chip)
{
	chip->r51_sel = -1;
	chip->r51_owner = NULL;
	chip->r51_stuck = false;
}

static int ae9_r51_send(struct creative_chip *chip, u32 verb, unsigned int id, u32 *rp,
			bool any_task);

static int ae9_send_verb_sync(struct creative_chip *chip, u32 verb, u32 *resp, bool any_task)
{
	unsigned int id = (verb >> 8) & 0xfff;
	u32 r = 0;
	int ret;

	if (!router_poll || !ae9_r51_param_verb(verb))
		return ae9_corb_send_verb_sync(chip, verb, resp);
	mutex_lock(&chip->r51_mutex);
	ret = ae9_r51_send(chip, verb, id, &r, any_task);
	mutex_unlock(&chip->r51_mutex);
	if (resp)
		*resp = r;
	return ret;
}

int creative_ae9_corb_send_verb_sync(struct creative_chip *chip, u32 verb, u32 *resp)
{
	return ae9_send_verb_sync(chip, verb, resp, false);
}

/* debugfs: one verb per write, so a select and its value come from two
 * processes (two tee runs); whoever pokes the router chip by hand owns it */
int creative_ae9_corb_send_verb_dbg(struct creative_chip *chip, u32 verb, u32 *resp)
{
	return ae9_send_verb_sync(chip, verb, resp, true);
}

static int ae9_r51_send(struct creative_chip *chip, u32 verb, unsigned int id, u32 *rp,
			bool any_task)
{
	unsigned int t;
	u32 r = 0;
	int ret = 0;

	if (id == 0x718 && chip->r51_sel < 0) {
		/* its select was lost: the value would land on another one */
		chip->r51_lost++;
		dev_warn_ratelimited(chip->dev, "router chip: value 0x%08x not sent, its parameter select was lost\n",
				     verb);
		return -EBUSY;
	}
	/*
	 * A select and its value are two commands; the replays send theirs
	 * under replay_mutex, but probe, resume and debugfs paths don't share
	 * one lock. If another task's select came between, this value would
	 * land on that task's parameter: refuse it, counted (r51_crossed).
	 */
	if (id == 0x718 && !any_task && chip->r51_owner != current) {
		chip->r51_crossed++;
		chip->r51_lost++;
		dev_warn_ratelimited(chip->dev, "router chip: value 0x%08x not sent, another task selected parameter 0x%02x in between\n",
				     verb, chip->r51_sel);
		return -EBUSY;
	}
	for (t = 0; t < AE9_R51_TRIES; t++) {
		ae9_r51_wait_idle(chip);
		ret = ae9_corb_send_verb_sync(chip, verb, &r);
		if (ret < 0 || (r & 0xff) != 1)
			break;
		chip->r51_refused++;	/* reply 1: refused busy */
	}
	if (id == 0x717) {	/* taken: that's the selected id; else nothing is */
		chip->r51_sel = ret == 0 && (r & 0xff) != 1 ? (int)(verb & 0xff) : -1;
		chip->r51_owner = current;
	}
	if (ret == 0 && (r & 0xff) == 1) {
		chip->r51_lost++;
		dev_warn_ratelimited(chip->dev, "router chip: verb 0x%08x refused %u times (busy): dropped\n",
				     verb, AE9_R51_TRIES);
		ret = -EBUSY;
	} else if (ret == 0 && (id == 0x712 || id == 0x713)) {
		ae9_r51_wait_idle(chip);	/* its port job, before the result read */
	}
	*rp = r;
	return ret;
}

static int ae9_corb_send_verb_sync(struct creative_chip *chip, u32 verb, u32 *resp)
{
	u16 wp_start;
	u16 wp;
	unsigned int timeout;
	u32 got_res = 0, got_fl = 0;
	int ret;

	/* Serialise: only one CORB verb in flight at a time.
	 * Without this, concurrent callers (ACM heartbeat, PCM prepare,
	 * select_lineout) interleave verbs and lose responses.
	 */
	if (chip->card_gone || chip->card_hung)
		return -ENODEV;
	mutex_lock(&chip->corb_mutex);
	if (!chip->corb_buf || !chip->rirb_buf) {	/* rings parked (suspend) */
		mutex_unlock(&chip->corb_mutex);
		return -ENODEV;
	}
	if (hung_inject && ae9_debug && chip->dsp_done) {
		hung_inject--;
		dev_err(chip->dev, "CORB verb response timeout (verb=0x%08x, injected)\n", verb);
		chip->corb_timeouts++;
		creative_ae9_health_notify(chip);
		ae9_corb_hung_check(chip, verb);
		mutex_unlock(&chip->corb_mutex);
		return -ETIMEDOUT;
	}

	/* Drain any pre-existing RIRB entries by reading current WP. Count
	 * the unsolicited ones on the way: the DSP announces each SCP reply
	 * with one, and they often land between our commands. */
	wp_start = readw(chip->hda_regs + AZX_REG_RIRBWP);
	/* all-ones: a card off the bus (each marked slot would cost the full
	 * 200 us wait: ~51 ms under corb_mutex, and 255 false "never landed")
	 * or one bad read; read once more, then give up on this verb
	 * (ultrareview 2, #5) */
	if (wp_start == 0xffff) {
		udelay(10);
		wp_start = readw(chip->hda_regs + AZX_REG_RIRBWP);
	}
	if (rirb_wp_bad(chip, wp_start, "RIRB drain")) {
		ret = chip->card_gone || chip->card_hung ? -ENODEV : -EIO;
		if (ret == -EIO) {
			dev_warn_ratelimited(chip->dev, "RIRB: verb 0x%08x not sent (write pointer unreadable)\n", verb);
			/* counts like a timeout: a card whose registers stay
			 * unreadable still ends up "stopped answering" */
			chip->corb_timeouts++;
			creative_ae9_health_notify(chip);
			ae9_corb_hung_check(chip, verb);
		}
		mutex_unlock(&chip->corb_mutex);
		return ret;
	}
	wp_start &= 0xff;
	chip->rirb_send_wp = -1;
	rirb_sweep(chip, wp_start, -1, NULL, NULL);
	{
		struct creative_rirb_entry *rirb = chip->rirb_buf;
		u16 i = chip->rirb_rp;

		while (i != wp_start) {
			u32 fl, res;

			i = (i + 1) & 0xff;
			if (rirb_take(chip, rirb, i, &res, &fl) && (fl & 0x10))
				rirb_unsol(chip, res, fl);
		}
	}
	chip->rirb_rp = wp_start;
	writew(wp_start, chip->hda_regs + AZX_REG_RIRBWP);

	/* Send the verb via CORB; its answer can only be in a slot after this */
	chip->rirb_send_wp = wp_start;
	ret = creative_ae9_corb_send_verb(chip, verb);
	if (ret < 0) {
		chip->rirb_send_wp = -1;
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
			u16 raw_wp = readw(chip->hda_regs + AZX_REG_RIRBWP);
			u16 cur_wp = raw_wp & 0xff;

			/* all-ones: the card may have dropped off the bus;
			 * each read of a dead card can stall for a long time */
			if (rirb_wp_bad(chip, raw_wp, "RIRB poll")) {
				if (chip->card_gone || chip->card_hung)
					break;
				/* one bad read: don't take 0xff as the write pointer */
				udelay(10);
				continue;
			}
			/* a given-up slot may hold our answer by now (#3) */
			if (rirb_sweep(chip, cur_wp, verb >> 28, &got_res, &got_fl))
				goto got_response;
			while (cur_wp != wp) {
				u16 next = (wp + 1) & 0xff;
				u32 fl, res;

				wp = next;
				if (!rirb_take(chip, rirb, next, &res, &fl))
					continue;
				/* an answer from the other codec isn't ours (ultrareview
				 * 2). It narrows the stale-answer case, it doesn't close
				 * it: a late answer from the same codec still looks like
				 * ours; the marks keep those out by timing */
				if (!(fl & 0x10) && (fl & 0xf) != (verb >> 28)) {
					chip->rirb_stray++;
					creative_ae9_health_notify(chip);
					dev_warn_ratelimited(chip->dev, "RIRB: answer 0x%08x from codec %u while waiting for codec %u (verb 0x%08x): skipped\n",
							     res, fl & 0xf, verb >> 28, verb);
					continue;
				}
				if (!(fl & 0x10)) {
					got_res = res;
					got_fl = fl;
					goto got_response;
				}
				/* unsolicited — log/count it, keep scanning */
				rirb_unsol(chip, res, fl);
				raw_wp = readw(chip->hda_regs + AZX_REG_RIRBWP);
				if (raw_wp == 0xffff)
					break;	/* the outer pass re-reads it, guarded */
				cur_wp = raw_wp & 0xff;
			}
			udelay(10);
		} while (--timeout);
	}

	if (creative_ae9_gone(chip, "CORB verb")) {
		chip->rirb_send_wp = -1;
		mutex_unlock(&chip->corb_mutex);
		return -ENODEV;
	}
	dev_err(chip->dev,
		"CORB verb response timeout (verb=0x%08x)\n", verb);
	chip->corb_timeouts++;
	creative_ae9_health_notify(chip);
	if (chip->dsp_done && !chip->dma_dumped) {	/* the first timeout of this load */
		chip->dma_dumped = true;
		creative_ae9_dma_dump(chip, "first command timeout");
	}
	ae9_corb_hung_check(chip, verb);

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
		u16 cur_rirb_wp = readw(chip->hda_regs + AZX_REG_RIRBWP);
		int t;

		/* the timed-out verb's answer, if it lands now, is an old one */
		chip->rirb_send_wp = -1;

		/* all-ones: don't skip ahead to 0xff; stay at wp */
		if (rirb_wp_bad(chip, cur_rirb_wp, "RIRB reset"))
			cur_rirb_wp = wp;
		cur_rirb_wp &= 0xff;

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
		writeb(CORBCTL_RUN, chip->hda_regs + AZX_REG_CORBCTL);	/* run + error irq */

		/* skip responses that may still arrive for the dropped verb,
		 * but count the DSP reply announcements among them: skipping
		 * one left every later GET one reply behind */
		rirb_sweep(chip, cur_rirb_wp, -1, NULL, NULL);
		{
			struct creative_rirb_entry *rirb = chip->rirb_buf;
			u16 i = wp;

			while (i != cur_rirb_wp) {
				u32 fl, res;

				i = (i + 1) & 0xff;
				if (rirb_take(chip, rirb, i, &res, &fl) && (fl & 0x10))
					rirb_unsol(chip, res, fl);
			}
		}
		chip->rirb_rp = cur_rirb_wp;
		dev_warn(chip->dev, "CORB ring reset after timeout (RP=WP=0)\n");
	}

	mutex_unlock(&chip->corb_mutex);
	return -ETIMEDOUT;

got_response:
	/* the solicited response, taken from the RIRB in the poll loop */
	creative_ae9_vlog(got_res, got_fl, true);
	if (resp)
		*resp = got_res;

	chip->corb_to_run = 0;
	chip->rirb_send_wp = -1;

	/* Update RIRB read pointer to acknowledge consumption */
	chip->rirb_rp = wp;
	writew(wp, chip->hda_regs + AZX_REG_RIRBWP);
	wmb();

	mutex_unlock(&chip->corb_mutex);
	return 0;
}
