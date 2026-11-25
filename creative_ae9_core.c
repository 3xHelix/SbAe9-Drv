// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 3xHelix
// Contains code adapted from the Linux kernel's CA0132 driver
// (sound/hda/codecs/ca0132.c, sound/pci/hda/patch_ca0132.c in older kernels):
//   Copyright (c) 2011, Creative Technology Ltd.
//   Based on patch_ca0110.c, Copyright (c) 2008 Takashi Iwai <tiwai@suse.de>
// and from s3boun3t/ae9_build (driver/ca0132/ca0132.c, GPL-2.0-or-later):
//   the AE-9 init sequence (ca0132_mmio_init_ae9, ae9_acm_init).
//
// Creative Sound Blaster AE-9 / X-Fi (CA0132) HDA controller driver
//
// PCI BAR layout: BAR0 = HDA (16 KB), BAR2 = Creative ext (16 KB)
// Reference: docs/history/Creative.md, ae9_mmio_capture.txt
//

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/tlv.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/hda_register.h>

#include <sound/asoundef.h>
#include "creative_ae9.h"

MODULE_AUTHOR("3xHelix");
MODULE_DESCRIPTION(CREATIVE_AE9_DRV_DESC);
MODULE_LICENSE("GPL");
MODULE_VERSION(CREATIVE_AE9_DRV_VERSION);

/* seconds after a bring-up during which phantom power "on" is refused */
#define AE9_PHANTOM_HOLD_S	30

/*
 * Windows' head-unit start-up display: "AE-9" (sent with the head unit's
 * setup), then no text at all for 10 polls of 500 ms (AcmShowAE9TextMaxCounts,
 * CtxHda FUN_0003fe68), then "-SP-"/"-HP-" for 10 polls, then the volume.
 * Measured in 31 Windows boots: "AE-9" up 4.5-5.1 s, typically 4.6 s.
 */
#define HU_AE9_HOLD_MS	4600

/* SBX button held this long (its b1 01 01 press report) switches the display
 * off or on instead of SBX: Windows' AcmSBXLongPressMs (CtxHda FUN_0003f6xx,
 * default 5000), the same 5000 ms our f0 32 03 01 88 13 gives the head unit */
#define HU_SBX_LONG_MS	5000

void creative_ae9_hu_ae9_hold(struct creative_chip *chip)
{
	chip->hu_ae9_hold = true;
	chip->hu_ae9_until = jiffies + msecs_to_jiffies(HU_AE9_HOLD_MS);
}

/*
 * After the init passes, poll the head unit every acm_heartbeat_ms, as
 * Windows does (~500 ms, playback or not): its knob, buttons, plugs and
 * status come from these polls. Windows' cadence; that the head unit needs
 * them to stay alive is not shown. 0=off:
 * tools/verbdiff loads the driver with 0 so its verb logs stay deterministic.
 */
static unsigned int acm_heartbeat_ms = 500;
module_param(acm_heartbeat_ms, uint, 0444);
MODULE_PARM_DESC(acm_heartbeat_ms, "Head-unit heartbeat poll interval (ms; 0 = off, used by tools/verbdiff; default 500)");

/*
 * The start-up tail: Windows' codec-1 boot stream from the first verb after
 * its MASTERCONTROL boot handshake (entries 1334-3164 of the captured
 * stream) is code, creative_ae9_tail() (creative_ae9_seq.c), as the verbs
 * before the download are (creative_ae9_predownload()); our own download,
 * start and boot handshake replace Windows' (the stream's entries
 * 292-1333). See WINDOWS-CAPTURE-ANALYSIS.md, sections 1 and 4. (Until
 * 2026-11-16 the stream was a table, creative_ae9_init_verbs.h; git
 * history at 4bd24a5 has it whole.)
 */

/*
 * Windows reads the codec subsystem IDs ~0.5 ms after the controller reset
 * and gets 0x11020071/72; we read them 200 ms later and get 0x11020010.
 * CREATIVE_CODEC_PROBE_DELAY_MS sets our delay (the codec_probe_delay_ms
 * option until 0.12.12); the IDs are read again 200 ms later to show whether
 * they change after reset.
 */

/*
 * bus_reset: secondary bus reset of the card (through its Tsi381 bridge)
 * at probe. Verified 2026-11-01: after a bus reset the codecs report
 * subsystem 0x11020071/0x11020072, as under Windows (where starting the VM
 * bus-resets the card); after only a power-on reset, or a PM (D3hot) reset,
 * they report the generic 0x11020010.
 * Behind an ASMedia bridge it was skipped from 0.9.8 to 0.12.4 (it seemed to
 * take the bridge down; the real cause was the posted-write wedge that
 * posted_flush fixes). Without it a hot reload there starts on stale card
 * state (BAR2 0x304/0xc0c/0xc14) and stays silent; with it a reload, a warm
 * and a cold boot all work (Kaiba, 2026-11-09).
 */
static int bus_reset = -1;
module_param(bus_reset, int, 0444);
MODULE_PARM_DESC(bus_reset, "Secondary bus reset of the card at probe, as a VM start does (1=on, 0=off, -1=auto: on, every bridge; default)");

/*
 * Bring-up attempts if the DSP misses its handshake (probe and resume),
 * and the wait before each resume attempt / between retries (the
 * resume_tries / resume_delay_ms options until 0.12.12).
 */
#define AE9_RESUME_DELAY_MS	2000
#define AE9_RESUME_TRIES	3U

/*
 * verb_log: record every verb sent and every response read (ICW and CORB)
 * so our sequence can be decoded with decode_scp.py and diffed against the
 * Windows capture. Read it from debugfs creative_ae9/verblog, which prints
 * the same format as ae9_corb_capture_raw.log. Value = number of entries.
 * Responses to fire-and-forget CORB verbs are not read, so not logged.
 */
static unsigned int verb_log;
module_param(verb_log, uint, 0444);
MODULE_PARM_DESC(verb_log, "Log this many verbs/responses for debugfs creative_ae9/verblog (0=off)");

/*
 * ex: bit 31 response; bit 30 MMIO entry, then bit 29 write, bits 27:26
 * size-1 (log2), bit 24 region (0 = BAR0, 1 = BAR2), bits 15:0 offset.
 */
struct vlog_ent { u32 val; u32 ex; u64 ns; };	/* ns: ktime, for timing diffs */
static struct vlog_ent *vlog_buf;
static atomic_t vlog_n = ATOMIC_INIT(0);

void creative_ae9_vlog(u32 val, u32 ex, bool is_resp)
{
	int i;

	if (!vlog_buf)
		return;
	i = atomic_inc_return(&vlog_n) - 1;
	if (i >= (int)verb_log)
		return;
	vlog_buf[i].val = val;
	vlog_buf[i].ex = (ex & 0x3fffffff) | (is_resp ? 0x80000000 : 0);
	vlog_buf[i].ns = ktime_get_ns();
}

static unsigned int mmio_log;
module_param(mmio_log, uint, 0444);
MODULE_PARM_DESC(mmio_log, "Also log BAR0/BAR2 register accesses in verblog (1=on, needs verb_log)");

static void __iomem *vlog_bar0, *vlog_bar2;

void ae9_mmio_log(const volatile void __iomem *addr, u32 val, int size, bool write)
{
	unsigned long a = (unsigned long)addr;
	u32 ex;
	int i;

	if (!vlog_buf || !mmio_log)
		return;
	if (vlog_bar0 && a >= (unsigned long)vlog_bar0 &&
	    a < (unsigned long)vlog_bar0 + 0x4000)
		ex = a - (unsigned long)vlog_bar0;
	else if (vlog_bar2 && a >= (unsigned long)vlog_bar2 &&
		 a < (unsigned long)vlog_bar2 + 0x4000)
		ex = (a - (unsigned long)vlog_bar2) | BIT(24);
	else
		return;
	ex |= BIT(30) | (write ? BIT(29) : 0) | ((u32)ilog2(size) << 26);
	i = atomic_inc_return(&vlog_n) - 1;
	if (i >= (int)verb_log)
		return;
	vlog_buf[i].val = val;
	vlog_buf[i].ex = ex;
	vlog_buf[i].ns = ktime_get_ns();
}
EXPORT_SYMBOL_GPL(ae9_mmio_log);

static void *vlog_seq_start(struct seq_file *m, loff_t *pos)
{
	int n = min_t(int, atomic_read(&vlog_n), (int)verb_log);

	return (vlog_buf && *pos < n) ? pos : NULL;
}

static void *vlog_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return vlog_seq_start(m, pos);
}

static void vlog_seq_stop(struct seq_file *m, void *v)
{
}

static int vlog_seq_show(struct seq_file *m, void *v)
{
	loff_t i = *(loff_t *)v;
	const struct vlog_ent *e = &vlog_buf[i];
	u64 us = div_u64(e->ns, 1000);

	/* "<index> <us> ..." : the microsecond stamp (CLOCK_MONOTONIC) lets
	 * the timing be compared with the Windows capture's mmio_trace */
	if (e->ex & BIT(30)) {
		unsigned int sz = 1u << ((e->ex >> 26) & 3);
		unsigned int rg = (e->ex & BIT(24)) ? 2 : 0;

		if (e->ex & BIT(29))
			seq_printf(m, "%lld %llu vfio_region_write  (0000:06:00.0:region%u+0x%x, 0x%x, %u)\n",
				   i, us, rg, e->ex & 0xffff, e->val, sz);
		else
			seq_printf(m, "%lld %llu vfio_region_read  (0000:06:00.0:region%u+0x%x, %u) = 0x%x\n",
				   i, us, rg, e->ex & 0xffff, sz, e->val);
	} else if (e->ex & 0x80000000)
		seq_printf(m, "%lld %llu RIRB idx=  0 resp=0x%08x ex=0x%08x\n",
			   i, us, e->val, e->ex & 0x7fffffff);
	else
		seq_printf(m, "%lld %llu CORB idx=  0 verb=0x%08x\n", i, us, e->val);
	return 0;
}

static const struct seq_operations vlog_seq_sops = {
	.start = vlog_seq_start,
	.next = vlog_seq_next,
	.stop = vlog_seq_stop,
	.show = vlog_seq_show,
};
DEFINE_SEQ_ATTRIBUTE(vlog_seq);

/* ------------------------------------------------------------------ */
/* PCI device ID table						  */
/* ------------------------------------------------------------------ */
/*
 * AE-9 only: 1102:0010 with subsystem 1102:0071. The AE-7 shares the device
 * ID (subsystem 0x0081) and is not in the table either: it is experimental
 * and opt-in (a driver_override and ae7=1), so nothing loads or binds this
 * driver on an AE-7 by itself. The AE-5 and AE-5 Plus are 1102:0012
 * (subsystems 0x0051, 0x0191), as are the Recon3D, Z and ZxR (in-tree
 * snd_hda_intel + ca0132). None of them may get the AE-9 bus reset, BAR2
 * set-up and Windows replay: ae9_board_lookup() refuses them if a
 * driver_override or new_id sends one here anyway.
 *
 * Creative's CtxHda.inf also lists 1102:0012 with the AE-9 and AE-7
 * subsystems, but every AE-9 and AE-7 seen is 1102:0010. The INF's
 * DEV_0011/0013/0015 are HD-Audio codec IDs (HDAUDIO\FUNC_01&VEN_1102&...),
 * not PCI devices.
 */
static const struct pci_device_id creative_ae9_pci_tbl[] = {
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_CREATIVE, DEVICE_AE7_AE9,
			 PCI_VENDOR_ID_CREATIVE, CREATIVE_SUBSYS_AE9),
	  .driver_data = DEVICE_AE7_AE9 },
	{ }
};
MODULE_DEVICE_TABLE(pci, creative_ae9_pci_tbl);

/*
 * Probe guard. The id_table matches the AE-9 alone, but a driver_override
 * (sysfs, or config/00-ae9-override.rules edited for another card) or a
 * new_id binds the driver to any PCI device. pci_match_device() then passes
 * pci_device_id_any (driver_data 0); until 0.12.16 that picked an all-zero
 * BAR layout, which sent the AE-9 BAR2 set-up to BAR0. The probe checks the
 * IDs itself now, from the values the PCI core cached at enumeration (no
 * config read, no MMIO, nothing enabled or mapped yet), and takes the BAR
 * layout from here. Anything but the AE-9 gets one log line saying what it
 * is, and -ENODEV; the AE-7 (on 0x0010) is let through with ae7=1 (its
 * board, creative_ae7_core.c), else it gets one notice naming the opt-in.
 *
 * Names: Creative's CtxHda.inf (AE series) and the in-tree ca0132 quirk
 * table (Z, ZxR, Recon3D): subsystem vendor 0x1102, PCI device 0x0010 or
 * 0x0012.
 */
#define AE9_IN_TREE	"not an AE-9; snd_hda_intel with the in-tree ca0132 driver handles it"

static const struct {
	u16 subsys;
	const char *name, *why;
} ae9_other_boards[] = {
	{ 0x0073, "Sound Blaster AE-9PE", "not tested with this driver; please report it" },
	{ 0x0051, "Sound BlasterX AE-5", AE9_IN_TREE },
	{ 0x0191, "Sound BlasterX AE-5 Plus", AE9_IN_TREE },
	{ 0x0061, "Sound BlasterX AE-3", "not an AE-9" },
	{ 0x0010, "Sound Blaster Z", AE9_IN_TREE },
	{ 0x0023, "Sound Blaster Z", AE9_IN_TREE },
	{ 0x0027, "Sound Blaster Z", AE9_IN_TREE },
	{ 0x0033, "Sound Blaster ZxR", AE9_IN_TREE },
	{ 0x0013, "Sound Blaster Recon3D", AE9_IN_TREE },
	{ 0x0018, "Sound Blaster Recon3D", AE9_IN_TREE },
};

#define AE9_REFUSED "%s (PCI %04x:%04x, subsystem %04x:%04x): %s. Leaving it alone; " \
	"if a driver_override or new_id sent it here, remove that so its own driver can take it\n"

/* the AE-9's BAR layout, or NULL (logged) for any other device */
static const struct creative_ae9_bar_layout *ae9_board_lookup(struct pci_dev *pdev)
{
	static const struct creative_ae9_bar_layout ae9_layout = {
		.hda_bar = 0, .ext_bar = 2, .ext_min_size = 0x4000,
	};
	const char *name = "Unknown device", *why = "not a Creative CA0132 sound card";
	bool known = false;
	unsigned int i;

	if (pdev->vendor == PCI_VENDOR_ID_CREATIVE &&
	    (pdev->device == DEVICE_AE7_AE9 || pdev->device == DEVICE_Z_AE5) &&
	    pdev->subsystem_vendor == PCI_VENDOR_ID_CREATIVE) {
		if (pdev->subsystem_device == CREATIVE_SUBSYS_AE9) {
			if (pdev->device == DEVICE_AE7_AE9)
				return &ae9_layout;
			/* in Creative's INF, but no such card seen: not claimed untested */
			name = "Sound Blaster AE-9";
			why = "never seen on PCI device 0x0012, so not claimed; please report it";
		} else if (pdev->subsystem_device == CREATIVE_SUBSYS_AE7) {
			if (pdev->device != DEVICE_AE7_AE9) {
				name = "Sound Blaster AE-7";
				why = "never seen on PCI device 0x0012, so not claimed; please report it";
			} else if (ae9_ae7_opt > 0) {
				/* experimental, opt-in: ae7=1 (creative_ae7_core.c) */
				return &ae9_layout;
			} else {
				dev_notice(&pdev->dev, "Sound Blaster AE-7 found: experimental support is off; see docs/AE7.md (run ae9-ae7-enable)\n");
				return NULL;
			}
		} else {
			name = "Unknown Creative CA0132 board";
			why = "not an AE-9";
		}
		for (i = 0; i < ARRAY_SIZE(ae9_other_boards); i++) {
			if (ae9_other_boards[i].subsys != pdev->subsystem_device)
				continue;
			name = ae9_other_boards[i].name;
			why = ae9_other_boards[i].why;
			known = true;
			break;
		}
	}
	if (known)
		dev_info(&pdev->dev, AE9_REFUSED, name, pdev->vendor, pdev->device,
			 pdev->subsystem_vendor, pdev->subsystem_device, why);
	else
		dev_warn(&pdev->dev, AE9_REFUSED, name, pdev->vendor, pdev->device,
			 pdev->subsystem_vendor, pdev->subsystem_device, why);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Hardware reset sequence					  */
/* docs/history/Creative.md §2.3, ae9_mmio_capture.txt:230			  */
/* ------------------------------------------------------------------ */
int creative_ae9_hw_reset(struct creative_chip *chip)
{
	u32 gctl;
	unsigned int timeout;

	/* Clear reset (GCTL = 0) */
	creative_hda_writel(chip, AZX_REG_GCTL, 0);
	msleep(10);

	/* Assert reset */
	creative_hda_writel(chip, AZX_REG_GCTL, AZX_GCTL_RESET);

	/* Poll until reset bit stays set (hardware confirms reset) */
	timeout = CREATIVE_RESET_TIMEOUT_MS;
	do {
		gctl = creative_hda_readl(chip, AZX_REG_GCTL);
		if (gctl & AZX_GCTL_RESET)
			break;
		msleep(1);
	} while (--timeout);

	if (!timeout) {
		dev_err(chip->dev, "HDA controller reset timed out (GCTL=0x%08x)\n",
			gctl);
		return -ETIMEDOUT;
	}

	dev_dbg(chip->dev, "Controller reset OK (GCTL=0x%08x)\n", gctl);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Immediate Command Write (ICW) protocol for Creative hardware	  */
/*								  */
/* Unlike standard HDA, this controller requires a trigger sequence  */
/* through the IRS register (0x68) before and after each verb:	  */
/*   1. Write ACK value to IRS (0x6 for first verb, last IRS value  */
/*      as ACK for subsequent verbs)				  */
/*   2. Write verb to ICW (0x60)					  */
/*   3. Poll IRS == 0x4 (hardware accepted the command)		  */
/*   4. Write 0x5 to IRS (fire — set BUSY)			  */
/*   5. Poll IRS for VALID (bit 1 set)				  */
/*   6. Read response from IR (0x64)				  */
/* ae9_mmio_capture.txt:141-179					  */
/* ------------------------------------------------------------------ */
int creative_ae9_icw_send_verb(struct creative_chip *chip,
			       u32 verb, u32 *resp)
{
	u16 irs;
	unsigned int timeout;

	if (chip->card_gone || chip->card_hung)
		return -ENODEV;

	/* Step 1: ACK previous response (or arm interface for first verb) */
	creative_hda_writew(chip, AZX_REG_IRS, chip->last_irs_ack);
	wmb();

	/* Step 2: Write verb to ICW */
	creative_ae9_vlog(verb, 0, false);
	creative_hda_writel(chip, AZX_REG_IC, verb);

	/* Step 3: Poll IRS == 0x4 (command accepted, ready to fire) */
	timeout = CREATIVE_IC_CMD_TIMEOUT_MS;
	do {
		irs = creative_hda_readw(chip, AZX_REG_IRS);
		if (irs == 0x4)
			break;
		udelay(10);
	} while (--timeout);

	if (!timeout) {
		if (irs == 0xffff && creative_ae9_gone(chip, "ICW"))
			return -ENODEV;
		dev_err(chip->dev, "ICW ready timeout (verb=0x%08x, IRS=0x%04x)\n",
			verb, irs);
		return -ETIMEDOUT;
	}

	/* Step 4: Fire the command — write 0x5 to IRS */
	creative_hda_writew(chip, AZX_REG_IRS, 0x5);

	/* Step 5: Poll for VALID bit */
	timeout = CREATIVE_IC_CMD_TIMEOUT_MS;
	do {
		irs = creative_hda_readw(chip, AZX_REG_IRS);
		if (irs & AZX_IRS_VALID)
			break;
		udelay(10);
	} while (--timeout);

	if (!timeout) {
		dev_dbg(chip->dev, "ICW response timeout (verb=0x%08x, IRS=0x%04x)\n",
			verb, irs);
		return -ETIMEDOUT;
	}

	/* Step 6: Read response from IR */
	{
		u32 r = creative_hda_readl(chip, AZX_REG_IR);

		creative_ae9_vlog(r, verb >> 28, true);
		if (resp)
			*resp = r;
	}

	/* Save current IRS as ACK value for next call */
	chip->last_irs_ack = irs;

	return 0;
}

/* ------------------------------------------------------------------ */
/* Build a standard HDA verb in ICW format				  */
/*   cad  : codec address (0-15)					  */
/*   nid  : node ID						  */
/*   verb_id : verb code (e.g. 0xF00 = GET_PARAM)			  */
/*   payload : verb payload (e.g. 0x00 = VENDOR_ID param)		  */
/* ------------------------------------------------------------------ */
u32 creative_ae9_verb(unsigned int cad, unsigned int nid,
		      unsigned int verb_id, unsigned int payload)
{
	return (cad << 28) | (nid << 20) | (verb_id << 8) | payload;
}

/* ------------------------------------------------------------------ */
/* Codec detection via immediate commands				  */
/* docs/history/Creative.md §3.3, ae9_mmio_capture.txt:147-179			  */
/* ------------------------------------------------------------------ */
static int creative_ae9_detect_codecs(struct creative_chip *chip)
{
	u16 statests;
	u32 resp;
	int ret;
	int cad;

	/* Read STATESTS to get codec presence bitmask */
	statests = readw(chip->hda_regs + AZX_REG_STATESTS);
	chip->codec_mask = statests & 0x0f;

	dev_info(chip->dev, "STATESTS = 0x%04x (codec mask 0x%x)\n",
		 statests, chip->codec_mask);

	if (!chip->codec_mask) {
		dev_warn(chip->dev, "No codecs detected\n");
		return 0;
	}

	/* Probe each detected codec via immediate commands (count afresh:
	 * this runs again on every resume) */
	chip->codec_count = 0;
	chip->codec_ok_mask = 0;
	for (cad = 0; cad < HDA_MAX_CODECS; cad++) {
		if (!(chip->codec_mask & BIT(cad)))
			continue;

		/* Get Vendor ID (root node, verb 0xF00, param 0x00) */
		ret = creative_ae9_icw_send_verb(chip,
			creative_ae9_verb(cad, 0, 0xF00, 0x00), &resp);
		if (ret < 0) {
			dev_err(chip->dev,
				"Codec %d: vendor ID probe failed (%d)\n",
				cad, ret);
			continue;
		}
		dev_info(chip->dev,
			 "Codec %d: vendor/device ID = 0x%08x\n", cad, resp);
		chip->codec_ok_mask |= BIT(cad);

		/* Get Subsystem ID (node 1, verb 0xF20, param 0x00) */
		ret = creative_ae9_icw_send_verb(chip,
			creative_ae9_verb(cad, 1, 0xF20, 0x00), &resp);
		if (ret < 0) {
			dev_dbg(chip->dev,
				"Codec %d: subsys ID probe failed (%d)\n",
				cad, ret);
			continue;
		}
		dev_info(chip->dev,
			 "Codec %d: subsystem ID = 0x%08x\n", cad, resp);

		chip->codec_count++;
	}

	dev_info(chip->dev, "Detected %u codec(s) on mask 0x%x\n",
		 chip->codec_count, chip->codec_mask);

	/* Re-read the subsystem IDs: do they change after reset? */
	msleep(200);
	for (cad = 0; cad < HDA_MAX_CODECS; cad++) {
		if (!(chip->codec_mask & BIT(cad)))
			continue;
		if (creative_ae9_icw_send_verb(chip,
				creative_ae9_verb(cad, 1, 0xF20, 0x00), &resp) == 0)
			dev_info(chip->dev,
				 "Codec %d: subsystem ID 200 ms later = 0x%08x\n",
				 cad, resp);
	}
	return chip->codec_count;
}

/*
 * Windows' Direct Mode switch (captures 2 and 3, WINDOWS-CAPTURE-ANALYSIS
 * §38). Off (capture 5, 22:56:39, with music playing): node 0x02 unbound,
 * DSP stream 0x14 off, stream 0x05 (host audio) to connection point 0x00
 * (the DSP's input), stream 0x18 carrying the DSP's output (0x09) to 0xd0 at
 * 96 kHz, 6 channels, and the I2S on the DSP's 96 kHz clocks (descriptors
 * and the full PLL program, as at startup). A stream then plays once node
 * 0x02 is bound again with only 0x18b008, its format and the bind (Windows'
 * next stream, 22:56:47) - replaying the start block would undo the
 * routing. On (= direct): stream 0x18 off; the next start block routes
 * direct again (Windows was silent until stop/play too). The blocks are
 * creative_ae9_seq_run.c's (section "DSP mode"): Windows' verbs as captured
 * (capture 6, vm_capture_coldboot_20261103_233115), ChipIO status reads
 * included.
 */

/*
 * The audio router (ChipIO 0x190000..; ca0132.c chipio_remap_stream): one
 * entry per 48 kHz channel, bits 17:16 state (1 active, 3 via the sample
 * rate converter), 15:8 destination port, 7:0 source port. Ports: 0x00-0x1f
 * HDA streams, 0x80-0xbf SRC, 0xc0-0xdf DSP DMA (assigned by the 8051),
 * 0xe0-0xff DAC/ADC; 0x40/0x41 are the front (ES9038) outputs. Windows'
 * Direct Mode start sets HDA 0x00/0x01 -> 0x40/0x41 in the slots the 8051
 * gave the stream (0x190060/64 when its list starts at slot 0x18; see
 * creative_ae9_rt_direct_walk). In DSP mode we still use 0x190060/64 for the
 * front pair (AE9_RT_FRONT_L/R). With Direct Mode off the 8051 has already
 * built the DSP path (HDA -> SRC -> DSP DMA in, DSP DMA out -> 0xe0/0xe1 at
 * 0x190048/4c, the processed front pair), so 0x40/0x41 have to take the
 * DSP's processed output instead (found 2026-11-04: effects and EQ became
 * audible the moment these two entries changed).
 */
#define AE9_RT_FRONT_L		0x190060
#define AE9_RT_FRONT_R		0x190064

/*
 * 5.1: the other DAC pairs. Windows' Direct Mode with 5.1 speakers
 * (capture 6, 23:39:16) writes 0x190060..74 = HDA 00..05 -> 40, 41, 50,
 * 51, 60, 61, in its channel order FL FR FC LFE BL BR: 0x50/0x51 = the
 * card's C/Sub jack, 0x60/0x61 = the card's Rear jack. With a 6-channel stream in
 * DSP mode the 8051 sends DSP outputs c2..c7 into SRCs that lead nowhere
 * (2026-11-04 dump), so like the front pair they're pointed at these
 * ports by hand, at 0x190068..74 as Windows does. surround_ports = the
 * targets for DSP outputs c2, c3, c4, c5 (guess: same order as the
 * input); writable, takes effect at the next stream start.
 */
#define AE9_RT_SURROUND		0x190068
static unsigned int surround_ports[4] = { 0x50, 0x51, 0x60, 0x61 };
static int surround_nports = 4;
module_param_array(surround_ports, uint, &surround_nports, 0644);
MODULE_PARM_DESC(surround_ports, "DSP mode: router targets for DSP outputs c2,c3,c4,c5 (default 0x50,0x51,0x60,0x61: center, LFE, rear L, rear R; each must be a DAC pair's first port 0x40-0x7f with bits 3:1 clear, else the default is used)");

/* a surround_ports value we can use: a DAC port whose +2 (the 96 kHz second
 * sample) is the same pair's other half; else the default */
static u8 ae9_surround_port(unsigned int i)
{
	static const u8 def[4] = { 0x50, 0x51, 0x60, 0x61 };
	unsigned int p = surround_ports[i];

	return p >= 0x40 && p < 0x80 && !(p & 0x0e) ? p : def[i];
}

#define AE9_RT_ENTRIES		128	/* 0x190000..0x1901fc (entries seen up to 0x19011c) */

/*
 * The other DSP outputs with no recording running: c2..cb -> these ports
 * (2026-11-04 dumps with SBX working). While a recording runs through the
 * DSP the 8051 sends them to 0xff on purpose (Discord's mic, SBX clean),
 * so 0xff is only logged; echoey audio seen with that layout came with
 * the stream 0x05 leak.
 */
static const u8 ae9_rt_dsp_out[] = {
	0x60, 0x61, 0x70, 0x71, 0x52, 0x53, 0x62, 0x63, 0x72, 0x73,
};

/*
 * Is the DSP executing? DBGCNTL (0x100e30) has a top half that changes on
 * every read while the DSP runs (2026-11-04, healthy DSP mode: 0x4e72,
 * 0x01f2, 0x004d, 0x20b5 over four reads 0.25 s apart; that's also why
 * each boot logs a different DBGCNTL). The guess is that it stops moving
 * on a stuck chip (silent DSP mode cured only by a PSU power-off). Logged
 * only until a stuck boot confirms it.
 */
#define AE9_DSP_ALIVE_READS	4

/* DBGCNTL sampled AE9_DSP_ALIVE_READS times; returns the changes seen,
 * or -1 if the reads failed */
static int ae9_dsp_alive_sample(struct creative_chip *chip, u32 *first, u32 *last)
{
	u32 v[AE9_DSP_ALIVE_READS];
	unsigned int i, n = 0, moved = 0;

	mutex_lock(&chip->replay_mutex);
	for (i = 0; i < AE9_DSP_ALIVE_READS; i++) {
		if (i)
			msleep(20);
		if (creative_dsp_chipio_read(chip, 0x100e30, &v[n]) < 0)
			continue;
		if (n && v[n] != v[n - 1])
			moved++;
		n++;
	}
	creative_dsp_chipio_addr_invalidate();
	mutex_unlock(&chip->replay_mutex);
	if (n < 2)
		return -1;
	*first = v[0];
	*last = v[n - 1];
	return moved;
}

/*
 * At boot this is only logged (the idle DSP has read NOT moving on a
 * boot that then played fine). With DSP mode on it means something:
 * 2026-11-04, both boots whose DSP mode played front-only or silence had
 * DBGCNTL stuck, one of them at the very first switch with a plain stereo
 * stream; every working boot had it moving. Checked twice, 0.5 s apart.
 */
static void creative_ae9_dsp_alive_log(struct creative_chip *chip, const char *when)
{
	bool dsp_on = strcmp(when, "boot") != 0;
	u32 first = 0, last = 0;
	int moved = ae9_dsp_alive_sample(chip, &first, &last);

	if (moved == 0 && dsp_on) {
		msleep(500);
		moved = ae9_dsp_alive_sample(chip, &first, &last);
	}
	if (moved < 0)
		dev_info(chip->dev, "DSP alive (%s): DBGCNTL read failed\n", when);
	else if (moved == 0 && dsp_on)
		dev_warn(chip->dev, "DSP alive (%s): DBGCNTL NOT moving (0x%08x): the DSP isn't running, DSP mode (Direct Mode off) will be silent or front-only. Direct Mode still works; power off at the PSU for ~20 s to clear it\n",
			 when, last);
	else if (!moved || !dsp_on || ae9_debug)
		dev_info(chip->dev, "DSP alive (%s): DBGCNTL %s (%d reads changed; first 0x%08x, last 0x%08x)\n",
			 when, moved ? "moving" : "NOT moving", moved, first, last);
}

/*
 * DSP outputs c2..cb sent to 0xff, logged once per bring-up for the record.
 * It is not a fault sign on its own (2026-11-04): a recording through the
 * DSP does it, the stuck chip that played silence until a PSU power-off
 * had it, and a healthy warm boot playing clean audio had it too (all of
 * c0..cb -> 0xff, c2..cb -> SRC chains, input via SRC 95/85). Earlier, a
 * boot with SRC chains like that was silent. So the router layout doesn't
 * tell a stuck chip from a good one; the silence comes from somewhere the
 * router doesn't show.
 *
 * Only entries 0x190000..0x1900fc are looked at, and a failed read just
 * shortens the scan (the 128-entry walk in router_front_dsp gives up on
 * any failed read, which is why its old 0xff count hardly ever fired).
 * Counted only once stream 0x05 runs: before that, a healthy 8051 still
 * has c2..cb -> 0xff for a moment (gone 170 ms later).
 */
#define AE9_RT_SCAN		64

/* c2..cb entries that are active and sent to 0xff; *seen = c2..cb found */
static unsigned int ae9_rt_dsp_out_ff(const u32 *e, unsigned int n,
				      unsigned int *seen)
{
	unsigned int i, ff = 0;

	*seen = 0;
	for (i = 0; i < n && i < AE9_RT_SCAN; i++) {
		u8 src = e[i] & 0xff, dst = (e[i] >> 8) & 0xff;

		if (!((e[i] >> 16) & 3) || src < 0xc2 ||
		    src >= 0xc2 + ARRAY_SIZE(ae9_rt_dsp_out))
			continue;
		if (dst == 0xff)
			ff++;
		else if (dst != ae9_rt_dsp_out[src - 0xc2])
			continue;	/* c2..cb -> e2..ed: the other half */
		(*seen)++;
	}
	return ff;
}

/* Log c2..cb -> 0xff once per bring-up (info only, see above). */
static void ae9_rt_ff_report(struct creative_chip *chip, unsigned int ff,
			     unsigned int seen)
{
	if (!ff || chip->rt_ff_logged)
		return;
	chip->rt_ff_logged = true;
	dev_info(chip->dev, "SBX: router: %u of %u DSP outputs to 0xff%s\n",
		 ff, seen, chip->cap_start_sent ? " (recording)" : "");
}

/*
 * ChipIO reads and writes while audio plays aren't reliable (2026-11-04):
 * 300 reads of a fixed router entry were all right with audio stopped,
 * but 6 came back wrong while playing (0, 0x1, or the NEXT entry's value),
 * with no verb timeouts and every status OK. The router snapshot showed
 * values from 0x38 further on too. So something on the chip moves the
 * ChipIO address between our address and data verbs while streaming,
 * most likely the 8051 itself, and no driver lock can stop that.
 *
 * Router reads: read until two in a row agree. Router writes: read back
 * and write once more if it didn't stick. A write that went astray may
 * have landed on another address, which we can't see; the retry count is
 * logged so we learn how often that happens.
 */
#define AE9_STABLE_TRIES	5

static int ae9_chipio_read_stable(struct creative_chip *chip, u32 a, u32 *v,
				  unsigned int *unsteady)
{
	u32 prev, cur;
	unsigned int i;

	if (creative_dsp_chipio_read(chip, a, &prev) < 0)
		return -EIO;
	for (i = 1; i < AE9_STABLE_TRIES; i++) {
		if (creative_dsp_chipio_read(chip, a, &cur) < 0)
			return -EIO;
		if (cur == prev) {
			*v = cur;
			return 0;
		}
		(*unsteady)++;
		prev = cur;
	}
	*v = cur;
	return 0;
}

static void ae9_rt_write_checked(struct creative_chip *chip, u32 a, u32 want,
				 unsigned int *unsteady, unsigned int *retried,
				 unsigned int *failed)
{
	u32 got = ~0U;
	unsigned int k;

	for (k = 0; k < 2; k++) {
		creative_dsp_chipio_write(chip, a, want);
		if (ae9_chipio_read_stable(chip, a, &got, unsteady) == 0 &&
		    got == want)
			return;
		if (!k)
			(*retried)++;
	}
	(*failed)++;
	dev_warn(chip->dev, "SBX: router write 0x%06x = 0x%05x didn't stick (reads 0x%x)\n",
		 a, want, got);
}

/*
 * A slot we may take: empty (0), or a DSP-mode output route that is off
 * (src 0xc0-0xdf, dst 0x40-0x7f, active bits clear). Every switch-off
 * here writes the entry with its active bits cleared (v & 0xffff), never
 * 0, and only "0" counted as free: each route moved to a free slot used one
 * up for good, and the router keeps its table until a power-off. After a
 * few rate/Direct Mode switches with the mic running there was none left:
 * "NOT routed (no free slot)", speakers silent in DSP mode (uv80,
 * 2026-11-15: control C6-C8 and M4, 12 of 12 refusals). An off DSP -> DAC
 * route is only ever ours or the 8051's own DSP-mode leftover, and DSP
 * mode is ours.
 */
static bool ae9_rt_slot_free(u32 v)
{
	u8 src = v & 0xff, dst = (v >> 8) & 0xff;

	return !v || (!((v >> 16) & 3) && (src & 0xe0) == 0xc0 &&
		      dst >= 0x40 && dst < 0x80);
}

/* a slot the free-slot search may hand out: past 0x190030, not our fixed
 * front pair; shared with the debugfs count (ultrareview 2, #6) */
static bool ae9_rt_slot_takeable(unsigned int i, u32 v)
{
	return i >= 0x0c && i < AE9_RT_ENTRIES && ae9_rt_slot_free(v) &&
	       0x190000 + 4 * i != AE9_RT_FRONT_L &&
	       0x190000 + 4 * i != AE9_RT_FRONT_R;
}

#define AE9_8051_STREAMS	37	/* stream ids 0x00-0x24: list heads XRAM 0x1578 + id */
static int ae9_8051_xram_byte_locked(struct creative_chip *chip, u16 addr);
static int ae9_rt_true_free(struct creative_chip *chip, bool force);

/*
 * Every router slot that sits in one of the 8051's stream lists (heads XRAM
 * 0x1578 + id, next 0x1478 + slot, 0xff = end; its allocator, ROM C171/
 * C19E/C1DB). A slot in no list is free; a slot the 8051 freed keeps its
 * old src/dst with the state bits clear, so "reads 0" undercounted the
 * free ones by about half (uv84 T0: 74-104 free, the old rule said 44-46).
 * Caller holds replay_mutex. Returns the listed slots from 0x0c on, or
 * -EIO when a read fails or a list makes no sense (a pointer out of range,
 * a slot seen twice, no end): a short list would hand out a listed slot,
 * so the caller falls back to the old rule (ultrareview of hf1).
 */
static int ae9_rt_list_map(struct creative_chip *chip, unsigned long *inl)
{
	unsigned int sid, k;
	int s, n = 0;

	bitmap_zero(inl, AE9_RT_ENTRIES);
	for (sid = 0; sid < AE9_8051_STREAMS; sid++) {
		s = ae9_8051_xram_byte_locked(chip, 0x1578 + sid);
		for (k = 0; k < AE9_RT_ENTRIES && s != 0xff; k++) {
			if (s < 0 || s >= AE9_RT_ENTRIES || test_and_set_bit(s, inl))
				return -EIO;
			if (s >= 0x0c)
				n++;
			s = ae9_8051_xram_byte_locked(chip, 0x1478 + s);
		}
		if (s != 0xff)
			return -EIO;	/* no end within 128 slots */
	}
	return n;
}

/* a slot we may take, given the 8051's lists: in none, nothing live in it
 * (our own moved routes are live and in no list), not our fixed pair */
static bool ae9_rt_slot_ok(unsigned int i, u32 v, const unsigned long *inl)
{
	if (!inl)
		return ae9_rt_slot_takeable(i, v);
	return i >= 0x0c && i < AE9_RT_ENTRIES && !test_bit(i, inl) && !((v >> 16) & 3) &&
	       0x190000 + 4 * i != AE9_RT_FRONT_L && 0x190000 + 4 * i != AE9_RT_FRONT_R;
}

/*
 * A free slot for a route we place ourselves (steering, feed repair). With
 * the 8051's lists known (inl), from the TOP down: the 8051 hands out the
 * lowest free slots first, so a route at the bottom was the next one it
 * would take over (uv80-82: the mic and DSP lists grew into 0x18-0x2f).
 * Without them (a read failed), as before.
 */
static int ae9_rt_free_slot(const u32 *e, int near, const unsigned long *inl)
{
	int i;

	/* the neighbours (without a map only: next to one of the 8051's own
	 * entries is just where it allocates next) too only if takeable:
	 * never our front pair or the low slots (ultrareview 2 follow-up) */
	if (!inl && near > 0 && ae9_rt_slot_ok(near - 1, e[near - 1], inl))
		return near - 1;
	if (!inl && near >= 0 && near + 1 < AE9_RT_ENTRIES &&
	    ae9_rt_slot_ok(near + 1, e[near + 1], inl))
		return near + 1;
	if (inl) {
		/* the router uses the whole table: uv83's lanes in 0x7e/0x7f
		 * played; the 8051 has never allocated above 0x53 (uv84) */
		for (i = AE9_RT_ENTRIES - 1; i >= 0x0c; i--)
			if (ae9_rt_slot_ok(i, e[i], inl))
				return i;
		return -1;
	}
	for (i = 0x0c; i < AE9_RT_ENTRIES; i++)	/* past 0x190030 */
		if (ae9_rt_slot_takeable(i, e[i]))
			return i;
	return -1;
}

/*
 * Our fixed DSP-mode slots (0x190060.., below) are the 8051's to give out
 * too: with What U Hear bound while no playback ran, it put the recording's
 * DSP cc/cd -> HDA capture 00/01 route in 0x190060/64 (uv11, 2026-11-09),
 * and overwriting that stalls the recording. An active entry into an HDA
 * capture port (dst < 0x20; in every dump and capture its source is an
 * SRC or a DSP output, and Windows never writes one) is left alone: our
 * route goes to a free slot instead, unless it is already on elsewhere.
 * So is the mic's feed one step earlier, a DSP output into a router SRC
 * (cc/cd/ce/cf -> 90/80, then 90/80 -> HDA capture 00/01): playback never
 * routes DSP -> SRC. After a 192 kHz stream the 8051 sometimes gives the
 * running mic slots 0x18-0x1d and the playback link 0x24 on: DSP mode on
 * then wrote c0/c1 -> 40/41 over the mic's cc -> 90 / cd -> 80 (the mic at
 * ~2x pitch, its channels a sample apart: the "chipmunk" Discord mic),
 * and the next mic start put its feeds back over ours (speakers silent in
 * DSP mode). uv79c/d, 2026-11-15: 2 of 3 trips after 192 kHz, slot list
 * head 0x24 in every bad one, 0x18 in every good one.
 * And the playback's own input: a stream started at 96 kHz in DSP mode
 * gets the host slots wired straight into the DSP (00..0b -> c0..cb) from
 * the slot list head on; with the head at 0x18 our c0/c1 -> 40/41 went over
 * its 00 -> c0 / 01 -> c1 and the DSP got no input: silent until Direct
 * Mode or 48 kHz (soak, 2026-11-15 16:18 and 16:19: head 0x18, 0x190068/6c
 * = 02 -> c2 / 03 -> c3 left of it; the same switch with head 0x24 played).
 * So the rule is the other way round: our fixed slots may only replace a
 * DAC feed (dst 0x40-0x7f: the Direct Mode routes, an old DSP-mode one) or
 * an idle DSP output (dst 0xff); any other active route belongs to a live
 * connection (capture, SRC, DSP input) and is left alone.
 * e[] must hold the n entries read; a relocation needs the full scan.
 */
static bool ae9_rt_in_use(u32 v)
{
	u8 dst = (v >> 8) & 0xff;

	if (!((v >> 16) & 3))
		return false;		/* inactive */
	return !(dst >= 0x40 && dst < 0x80) && dst != 0xff;
}

static void ae9_rt_moved_add(struct creative_chip *chip, unsigned int slot)
{
	unsigned int k;

	for (k = 0; k < chip->rt_moved_n; k++)
		if (chip->rt_moved[k] == slot)
			return;
	if (chip->rt_moved_n < ARRAY_SIZE(chip->rt_moved))
		chip->rt_moved[chip->rt_moved_n++] = slot;
}

/* returns the slot that now carries the route, or -1 */
/* the 8051's list map, built once per routing pass when first needed;
 * state 0 = not built yet, 1 = built, -1 = couldn't be read */
struct ae9_rt_map {
	DECLARE_BITMAP(inl, AE9_RT_ENTRIES);
	int state;
};

static const unsigned long *ae9_rt_map_get(struct creative_chip *chip, struct ae9_rt_map *m)
{
	if (!m)
		return NULL;
	if (!m->state)
		m->state = ae9_rt_list_map(chip, m->inl) >= 0 ? 1 : -1;
	return m->state > 0 ? m->inl : NULL;
}

static int ae9_rt_put(struct creative_chip *chip, u32 *e, unsigned int n,
		      struct ae9_rt_map *map, u32 a, u32 want, unsigned int *unsteady,
		      unsigned int *retried, unsigned int *failed)
{
	unsigned int i = (a - 0x190000) / 4, j;
	int at = -1;

	if (i >= n || !ae9_rt_in_use(e[i])) {
		ae9_rt_write_checked(chip, a, want, unsteady, retried, failed);
		if (i < n)
			e[i] = want;
		return i;
	}
	for (j = 0; j < n; j++)
		if (j != i && ((e[j] >> 16) & 3) && (e[j] & 0xffff) == (want & 0xffff))
			break;
	if (j == n && n == AE9_RT_ENTRIES)
		at = ae9_rt_free_slot(e, -1, ae9_rt_map_get(chip, map));
	dev_info(chip->dev, "SBX: router: 0x%06x is in use (0x%05x), left alone; 0x%04x %s\n",
		 a, e[i], want & 0xffff, j < n ? "already on elsewhere" :
		 at < 0 ? "NOT routed (no free slot)" : "goes to a free slot");
	/* either way the route now sits outside our fixed slots: remember
	 * it, so Direct Mode and shutdown switch it off again (left on, the
	 * DAC port got a second feed in Direct Mode: gated, metallic audio
	 * that only a power-off cleared, 2026-11-11) */
	if (j < n) {
		ae9_rt_moved_add(chip, j);
		return j;
	}
	if (at < 0)
		return -1;
	e[at] = want;
	ae9_rt_write_checked(chip, 0x190000 + 4 * at, want, unsteady, retried, failed);
	ae9_rt_moved_add(chip, at);
	return at;
}

/*
 * DSP -> DAC route clean-up (2026-11-11): a DSP-mode route moved to a free
 * slot (ae9_rt_put) stayed on into Direct Mode and through later loads, so
 * a DAC port had two feeds: gated, metallic audio at 48/192 kHz, 96 kHz
 * ~25 dB low, until a power-off; the input feed repair's 00 -> 90 /
 * 01 -> 80 stayed on the same way. Such routes are switched off (Direct
 * Mode on, the first Direct start, shutdown; extra feeds in DSP mode; the
 * rt_dac_clean option until 0.12.12).
 */

/* an active router entry from a DSP output (0xc0-0xdf) into a DAC port */
static bool ae9_rt_is_dsp_dac(u32 v)
{
	u8 src = v & 0xff, dst = (v >> 8) & 0xff;

	return ((v >> 16) & 3) && (src & 0xe0) == 0xc0 && dst >= 0x40 && dst < 0x80;
}

/*
 * A route the sweep switches off before a Direct start: a DSP output into
 * a DAC port, or a host slot (HDA 0x00-0x1f) into a router SRC
 * (0x80-0xbf), the DSP's input path (the feed repair adds 00 -> 90 /
 * 01 -> 80). Windows' Direct start block writes one 00 -> 90 / 01 -> 80
 * pair itself, after the sweep; a second, left-over pair plus a DSP ->
 * DAC route played gated, metallic audio (uv26 2026-11-11: reproduced by
 * planting the 2026-11-11 bad dump's four routes, cleared by this).
 */
static bool ae9_rt_is_dsp_only(u32 v)
{
	u8 src = v & 0xff, dst = (v >> 8) & 0xff;

	return ae9_rt_is_dsp_dac(v) ||
	       (((v >> 16) & 3) && src < 0x20 && dst >= 0x80 && dst < 0xc0);
}

/*
 * Switch off DSP-mode routes (ae9_rt_is_dsp_only) outside DSP mode.
 * all = false: only the slots we wrote outside our fixed ones (moved
 * front/surround routes, repaired input feeds; shutdown); all = true:
 * every one in the table (Direct Mode, which uses none: also the ones an
 * earlier load left, which the router keeps until power-off).
 */
static void ae9_rt_dsp_dac_off(struct creative_chip *chip, bool all, const char *why)
{
	unsigned int unsteady = 0, retried = 0, failed = 0, off = 0, i;
	u32 v;

	if (!all && !chip->rt_moved_n)
		return;
	mutex_lock(&chip->replay_mutex);
	creative_dsp_chipio_addr_invalidate();
	for (i = 0; i < (all ? AE9_RT_ENTRIES : chip->rt_moved_n); i++) {
		u32 a = 0x190000 + 4 * (all ? i : chip->rt_moved[i]);

		if (ae9_chipio_read_stable(chip, a, &v, &unsteady) < 0)
			break;
		if (!ae9_rt_is_dsp_only(v))
			continue;
		ae9_rt_write_checked(chip, a, v & 0xffff, &unsteady, &retried, &failed);
		off++;
	}
	if (off)
		creative_dsp_chipio_write(chip, AE9_RT_APPLY, 1);
	creative_dsp_chipio_addr_invalidate();
	mutex_unlock(&chip->replay_mutex);
	chip->rt_moved_n = 0;
	if (off)
		dev_info(chip->dev, "router: switched off %u DSP-mode route%s (%s)\n",
			 off, off == 1 ? "" : "s", why);
}

/* every one (all = true), for the playback start (creative_ae9_seq_run.c) */
void creative_ae9_rt_dsp_dac_off_all(struct creative_chip *chip, const char *why)
{
	ae9_rt_dsp_dac_off(chip, true, why);
}

/*
 * Front outputs from the DSP (Direct Mode off). The 8051 assigns the DSP
 * DMA ports itself and they move between boots (2026-11-04: c0/c1 -> e0/e1
 * at 0x190048 on one boot, c6/c7 -> e8/e9 there and c0..c3 -> 40..43 at
 * 0x190090 on the next), so look them up: prefer the 8051's own entries
 * that feed 0x40/0x41 from a DSP port, else its DSP-out -> 0xe0/0xe1 pair,
 * else c0/c1.
 *
 * Then repair the broken layout seen when the 8051 builds the DSP path
 * badly (with the usual c0/c1 front ports only), and report DSP outputs
 * c2..cb sent to 0xff (logged only; not a fault sign on its own,
 * see ae9_rt_ff_report):
 * - HDA 0x00 -> SRC 0x90 missing (after resume) while SRC 0x90 still feeds
 *   the DSP: only half the input arrives, crunched audio. Add it back
 *   (and HDA 0x01 -> SRC 0x80 likewise).
 */
/*
 * DSP mode's DSP -> DAC route is the 8051's own: stream 0x18 (DSP output
 * connection point 0x09 -> DAC 0xd0), in its own slot list, head XRAM
 * 0x1590, next XRAM 0x1478 + slot (the 8051 ROM's allocator, C171/C19E;
 * research 2026-11-15, project notes).
 * Windows writes no router entry at all in DSP mode: when the 8051's DAC
 * port allocation works it fills in the destinations itself, c0..c5 -> 40
 * 41 50 51 60 61 and, at 96 kHz, each channel's second sample c6..cb ->
 * 42 43 52 53 62 63 (dumps 2026-11-07). On our card that allocation fails
 * (an orphaned 3-pair allocation on connection point 0xd1 holds DAC groups
 * 1-3 until a power-off; uv84 T0): every destination reads 0xff. We wrote
 * c0/c1 -> 40/41 into fixed slots 0x190060/64, which were stream 0x18's
 * only by luck: elsewhere they belonged to the mic or the 96 kHz input
 * (the collisions of 4228a7d/49b7ca7), and c6/c7 never reached 42/43:
 * half the 96 kHz samples, DSP mode 6.0 dB below Direct (uv83: +c6->42,
 * c7->43 by hand: -0.2 dB). So set the destinations in stream 0x18's own
 * slots, as Windows' Direct Mode walk does for its stream (and Creative's
 * driver does for another card's stream 0x18, FUN_00037898): no fixed
 * slot, no collision possible (the 8051 never gives a connected link's
 * slots to anyone else). All twelve lanes, as the 8051's own layout; the
 * fixed slots stay only as the fallback when stream 0x18's list can't be
 * used. (rt_dsp_walk, until 0.12.14, could force the fixed slots or the
 * front lanes only.)
 */

#define AE9_XRAM_STREAM_REFS(id)	(0x0734 + 10 * (id))	/* record byte 5 */
#define AE9_XRAM_STREAM18_REFS	AE9_XRAM_STREAM_REFS(0x18)	/* 0x0824 */
#define AE9_XRAM_SLOT_NEXT	0x1478	/* + slot: next slot of its link, 0xff = end */
#define AE9_RT_FIRST_SLOT	0x0c	/* 0x190030; lowest head ever seen 0x18 */
#define AE9_XRAM_STREAM18_HEAD	0x1590	/* 0x1578 + stream id: first slot of stream 0x18 */
#define AE9_DSP_LANES		12	/* c0..cb: six channels, two samples each at 96 kHz */

static int ae9_8051_xram_byte_locked(struct creative_chip *chip, u16 addr);
static void ae9_8051_wait_idle(struct creative_chip *chip, const char *why);
static void creative_ae9_hu_amp_hold(struct creative_chip *chip);

/*
 * Walk stream 0x18's slot list (caller holds replay_mutex). Each slot is
 * read again (two agreeing reads) into e[] and must be a DSP output c0..cb
 * going to a DAC port or to 0xff: anything else (e.g. stream 0x0c's c0..cb
 * -> e0..ed, reached through a glitched next pointer) and the whole list is
 * refused. Fills slot[c - 0xc0]. Returns the lanes found, 0 if the list
 * can't be trusted (the caller falls back to fixed slots), -1 if stream
 * 0x18 isn't connected (nothing to route).
 */
static int ae9_rt_dsp_chain(struct creative_chip *chip, u32 *e, unsigned int n,
			    int *slot, int *head_out, unsigned int *unsteady)
{
	DECLARE_BITMAP(seen, AE9_RT_ENTRIES);
	unsigned int found = 0, k;
	int refs, s;

	for (k = 0; k < AE9_DSP_LANES; k++)
		slot[k] = -1;
	refs = ae9_8051_xram_byte_locked(chip, AE9_XRAM_STREAM18_REFS);
	s = *head_out = ae9_8051_xram_byte_locked(chip, AE9_XRAM_STREAM18_HEAD);
	if (refs < 0 || s < 0)
		return 0;
	if (!refs || s == 0xff)
		return -1;
	if (n < AE9_RT_ENTRIES)
		return 0;
	bitmap_zero(seen, AE9_RT_ENTRIES);
	for (k = 0; k < 2 * AE9_DSP_LANES && s >= 0 && s != 0xff; k++) {
		u32 v;
		u8 src, dst;

		if (s < AE9_RT_FIRST_SLOT || s >= AE9_RT_ENTRIES || test_and_set_bit(s, seen))
			return 0;	/* a glitched read: don't trust any of it */
		if (ae9_chipio_read_stable(chip, 0x190000 + 4 * s, &v, unsteady) < 0 ||
		    (v >> 18))
			return 0;
		src = v & 0xff;
		dst = (v >> 8) & 0xff;
		if (src < 0xc0 || src >= 0xc0 + AE9_DSP_LANES || slot[src - 0xc0] >= 0 ||
		    !(dst == 0xff || (dst >= 0x40 && dst < 0x80)))
			return 0;
		e[s] = v;
		slot[src - 0xc0] = s;
		found++;
		s = ae9_8051_xram_byte_locked(chip, AE9_XRAM_SLOT_NEXT + s);
	}
	return s == 0xff ? (int)found : 0;
}

static void creative_ae9_router_front_dsp(struct creative_chip *chip, bool judge)
{
	int l = -1, r = -1, el = -1, er = -1, inputs = 0, ins = 0;
	int feed_at[2] = { -1, -1 }, src_to_dsp[2] = { 0, 0 };
	static const u8 hda_src[2] = { 0x90, 0x80 };	/* HDA 0x00, 0x01 */
	u32 e[AE9_RT_ENTRIES];
	int keep[AE9_DSP_LANES];	/* slots of our DSP -> DAC routes */
	int lane[AE9_DSP_LANES], head18 = -1, chain = 0;
	struct ae9_rt_map rtmap = { .state = 0 };
	unsigned int i, n = 0, ff, seen, walked = 0, lanes_set = 0, wrote = 0;
	unsigned int lanes_ff = 0, lanes_parked = 0, lanes_stray = 0;
	unsigned int unsteady = 0, retried = 0, failed = 0;

	for (i = 0; i < ARRAY_SIZE(keep); i++)
		keep[i] = -1;
	/* stream 0x18's list is the 8051's: let it finish connecting first */
	ae9_8051_wait_idle(chip, "before the DSP route walk");
	mutex_lock(&chip->replay_mutex);
	creative_dsp_chipio_addr_invalidate();
	for (i = 0; i < AE9_RT_ENTRIES; i++) {
		u32 a = 0x190000 + 4 * i, v;
		u8 src, dst;

		/* two reads in a row must agree (see ae9_chipio_read_stable);
		 * the repairs below write into slots that read as empty */
		if (ae9_chipio_read_stable(chip, a, &v, &unsteady) < 0)
			break;
		e[i] = v;
		n++;
		if (a == AE9_RT_FRONT_L || a == AE9_RT_FRONT_R)
			continue;	/* ours */
		src = v & 0xff;
		dst = (v >> 8) & 0xff;
		if (!((v >> 16) & 3))
			continue;	/* inactive */
		if ((src & 0xe0) == 0xc0) {
			if (dst == 0x40 && l < 0)
				l = src;
			else if (dst == 0x41 && r < 0)
				r = src;
			else if (dst == 0xe0 && el < 0)
				el = src;
			else if (dst == 0xe1 && er < 0)
				er = src;
		}
		/*
		 * DSP inputs: at 48 kHz HDA -> SRC -> DSP (00 -> 91 -> c0 ...);
		 * at 96 kHz, the DSP's own rate, the 8051 wires HDA slots
		 * straight in (00..0b -> c0..cb, state 3; 2026-11-06 dumps).
		 */
		if ((dst & 0xe0) == 0xc0 && src < 0x20)
			inputs++;
		if ((dst & 0xe0) == 0xc0 && src >= 0x80 && src < 0xc0) {
			inputs++;
			if (src == 0x90)
				src_to_dsp[0] = 1;
			else if (src == 0x80)
				src_to_dsp[1] = 1;
		}
		if (src == 0x00 && dst == 0x90)
			feed_at[0] = i;
		else if (src == 0x01 && dst == 0x80)
			feed_at[1] = i;
	}
	if (l < 0 || r < 0) {
		l = el >= 0 ? el : 0xc0;
		r = er >= 0 ? er : 0xc1;
	}
	chain = ae9_rt_dsp_chain(chip, e, n, lane, &head18, &unsteady);
	/* one glitched pointer or slot read (partial or refused list, the list
	 * itself readable): once more */
	if ((chain > 0 && chain < AE9_DSP_LANES) ||
	    (chain == 0 && head18 >= 0 && n == AE9_RT_ENTRIES))
		chain = ae9_rt_dsp_chain(chip, e, n, lane, &head18, &unsteady);
	walked = chain > 0 ? chain : 0;
	if (walked) {
		DECLARE_BITMAP(ours, AE9_RT_ENTRIES);

		/* what the 8051 left in its lanes: all 0xff = its DAC port
		 * allocation failed (the "N of M DSP outputs to 0xff" state) */
		for (i = 0; i < AE9_DSP_LANES; i++)
			if (lane[i] >= 0 && ((e[lane[i]] >> 8) & 0xff) == 0xff)
				lanes_ff++;
		bitmap_zero(ours, AE9_RT_ENTRIES);
		for (i = 0; i < AE9_DSP_LANES; i++)
			if (lane[i] >= 0)
				__set_bit(lane[i], ours);
		/* every other DSP output on a DAC port is left over (old fixed
		 * or moved routes, an index-1 layout's c4 -> 70; the router keeps
		 * them until a power-off): off, so only stream 0x18 feeds the DACs */
		for (i = 0; i < n; i++) {
			if (test_bit(i, ours) || !ae9_rt_is_dsp_dac(e[i]))
				continue;
			ae9_rt_write_checked(chip, 0x190000 + 4 * i, e[i] & 0xffff,
					     &unsteady, &retried, &failed);
			e[i] &= 0xffff;
			lanes_stray++;
		}
	}
	if (walked) {
		/*
		 * Stream 0x18's own slots: set each DSP output's DAC port and
		 * switch it on (state 1, as the 8051 lays them out and as our
		 * fixed routes were). Lane k < 6 is channel k's first sample,
		 * k + 6 its second (96 kHz). Done before the feed repair below,
		 * so its free-slot search never sees one of these as free.
		 */
		for (i = 0; i < AE9_DSP_LANES; i++) {
			unsigned int ch = i % 6;
			u8 dst = ch < 2 ? 0x40 + ch : ae9_surround_port(ch - 2);
			int s2 = lane[i];
			u32 want;

			if (s2 < 0)
				continue;
			if (i >= 6)
				dst += 2;	/* the second sample: 42/43, 52/53, 62/63 */
			if (!((e[s2] >> 16) & 3))
				lanes_parked++;	/* the 8051 (or a Direct start) had it off */
			want = (e[s2] & 0xfffc00ff) | 0x10000 | (u32)dst << 8;
			if (want != e[s2]) {
				ae9_rt_write_checked(chip, 0x190000 + 4 * s2, want,
						     &unsteady, &retried, &failed);
				wrote++;
			}
			e[s2] = want;
			keep[i] = s2;
			lanes_set++;
		}
	}
	/* the feed repair is for the stereo layout (00 -> 90, 01 -> 80); in
	 * the 6-ch one those SRCs carry HDA 04/05 */
	/* not while the voice processor is in: its mic path uses these SRCs,
	 * and "repairing" them mixed the playback into the recording
	 * (cv-talk take 3, 2026-11-08; Windows writes no router entries at a
	 * playback start during Voice Clarity) */
	if (n == AE9_RT_ENTRIES && l == 0xc0 && r == 0xc1 && chip->in05_ch != 6 &&
	    !chip->cv.route_on &&
	    !(chip->pcm_start_sent && (chip->pcm_start_fmt & 0xf) == 5)) {
		for (i = 0; i < 2; i++) {
			u32 want = 0x10000 | hda_src[i] << 8 | i;
			int at;

			if (!src_to_dsp[i] || feed_at[i] >= 0)
				continue;
			at = ae9_rt_free_slot(e, feed_at[!i], ae9_rt_map_get(chip, &rtmap));
			if (at < 0)
				break;
			e[at] = want;
			ae9_rt_write_checked(chip, 0x190000 + 4 * at, want,
					     &unsteady, &retried, &failed);
			ae9_rt_moved_add(chip, at);	/* off again outside DSP mode */
			ins++;
			inputs++;
		}
	}
	/*
	 * Stale direct bypasses: in DSP mode host audio reaches the DACs only
	 * through the DSP, so an HDA slot wired straight to a DAC port is
	 * left over from an earlier Direct Mode stream (5.1, or above
	 * 48 kHz). Nothing removed them: 0x190068 = HDA 01 -> 0x41 played
	 * the right channel direct and through the DSP, a left/right echo
	 * (2026-11-06, 48 kHz stereo after a 5.1 direct stream). Switch them
	 * off; the 5.1 ports below are rewritten after this.
	 */
	{
		unsigned int stale = 0;

		for (i = 0; i < n; i++) {
			u32 a = 0x190000 + 4 * i;
			u8 sv = e[i] & 0xff, dv = (e[i] >> 8) & 0xff;

			/* the fixed slots too: with the stream 0x18 walk nothing
			 * overwrites a leftover HDA 00 -> 40 there any more; our
			 * own routes there have a DSP source, never < 0x20 */
			if (!((e[i] >> 16) & 3) || sv >= 0x20 ||
			    dv < 0x40 || dv >= 0x80)
				continue;
			ae9_rt_write_checked(chip, a, e[i] & 0xffff,
					     &unsteady, &retried, &failed);
			e[i] &= 0xffff;
			stale++;
		}
		if (stale)
			dev_info(chip->dev, "SBX: router: switched off %u stale direct bypass%s (HDA -> DAC)\n",
				 stale, stale == 1 ? "" : "es");
	}
	if (walked) {
		/* routed above, in stream 0x18's own slots */
	} else if (!(chain < 0 && !chip->pcm_start_sent)) {
		/* the fixed slots, as before 0.12.11: the list couldn't be
		 * trusted, or a stream plays but stream 0x18 isn't connected;
		 * with no stream there's nothing to route */
		keep[0] = ae9_rt_put(chip, e, n, &rtmap, AE9_RT_FRONT_L, 0x10000 | (0x40 << 8) | l,
				     &unsteady, &retried, &failed);
		keep[1] = ae9_rt_put(chip, e, n, &rtmap, AE9_RT_FRONT_R, 0x10000 | (0x41 << 8) | r,
				     &unsteady, &retried, &failed);
	}
	if (chip->in05_ch == 6) {
		/*
		 * Stale input feeds: DSP input cK is fed by HDA channel K through
		 * its SRC (stereo: 00 -> 90 -> c0; 6 ch: 04 -> 90 -> c4). Going
		 * from a 2-ch to a 6-ch stream the 8051 left the stereo feeds
		 * 00 -> 90 and 01 -> 80 on, so front L/R also reached the rear
		 * inputs c4/c5 (2026-11-04: front voices on the rear jack, quiet
		 * and tinny). Switch off HDA feeds into an SRC of another channel.
		 */
		u8 src_ch[0x40];
		unsigned int stale = 0;

		memset(src_ch, 0xff, sizeof(src_ch));
		for (i = 0; i < n; i++) {
			u8 sv = e[i] & 0xff, dv = (e[i] >> 8) & 0xff;

			if (((e[i] >> 16) & 3) && sv >= 0x80 && sv < 0xc0 &&
			    dv >= 0xc0 && dv < 0xc0 + 6)
				src_ch[sv - 0x80] = dv - 0xc0;
		}
		for (i = 0; i < n; i++) {
			u8 sv = e[i] & 0xff, dv = (e[i] >> 8) & 0xff;

			if (!((e[i] >> 16) & 3) || sv >= 0x20 || dv < 0x80 ||
			    dv >= 0xc0 || src_ch[dv - 0x80] == 0xff ||
			    src_ch[dv - 0x80] == sv)
				continue;
			ae9_rt_write_checked(chip, 0x190000 + 4 * i, e[i] & 0xffff,
					     &unsteady, &retried, &failed);
			stale++;
		}
		if (stale)
			dev_info(chip->dev, "SBX: router 5.1: switched off %u stale input feed%s\n",
				 stale, stale == 1 ? "" : "s");
		/*
		 * Extra DSP outputs on the DAC ports: sometimes the 8051 lays out
		 * c6..cb -> 52, 53, 62, 63, 72, 73 (its stereo-mode map) next to
		 * our c2..c5 -> 50, 51, 60, 61; 0x5x and 0x6x share the Center/Sub
		 * and Rear jacks, so front L/R came out of Center/Sub too and
		 * center/LFE out of the Rear (2026-11-04). Only c0..c5 stay on.
		 */
		stale = 0;
		for (i = 0; i < n && !walked; i++) {
			u8 sv = e[i] & 0xff, dv = (e[i] >> 8) & 0xff;

			if (!((e[i] >> 16) & 3) || sv < 0xc6 || sv >= 0xe0 ||
			    dv < 0x40 || dv >= 0x80)
				continue;
			ae9_rt_write_checked(chip, 0x190000 + 4 * i, e[i] & 0xffff,
					     &unsteady, &retried, &failed);
			stale++;
		}
		if (stale)
			dev_info(chip->dev, "SBX: router 5.1: switched off %u extra DSP output%s on the DACs\n",
				 stale, stale == 1 ? "" : "s");
		if (!walked) {
			for (i = 0; i < 4; i++)
				keep[2 + i] = ae9_rt_put(chip, e, n, &rtmap, AE9_RT_SURROUND + 4 * i,
							 0x10000 | ae9_surround_port(i) << 8 | (0xc2 + i),
							 &unsteady, &retried, &failed);
			dev_info(chip->dev, "SBX: router 5.1: DSP c2..c5 -> 0x%02x 0x%02x 0x%02x 0x%02x\n",
				 ae9_surround_port(0), ae9_surround_port(1),
				 ae9_surround_port(2), ae9_surround_port(3));
		}
	}
	/*
	 * One feed per DAC port: a DSP output into a port we just routed,
	 * from any other slot, is left over (a moved route from an earlier
	 * DSP-mode start, or one an earlier load left; the router keeps them
	 * until power-off). Two feeds on a port played gated, metallic audio
	 * (2026-11-11). Switch the extra ones off.
	 */
	{
		unsigned int dup = 0, k, x;

		for (i = 0; i < n; i++) {
			bool ours = false, port = false;

			if (!ae9_rt_is_dsp_dac(e[i]))
				continue;
			for (k = 0; k < ARRAY_SIZE(keep); k++) {
				if (keep[k] < 0 || keep[k] >= n)
					continue;
				if (keep[k] == i)
					ours = true;
				else if (((e[keep[k]] >> 8) & 0xff) == ((e[i] >> 8) & 0xff))
					port = true;
			}
			if (ours || !port)
				continue;
			ae9_rt_write_checked(chip, 0x190000 + 4 * i, e[i] & 0xffff,
					     &unsteady, &retried, &failed);
			e[i] &= 0xffff;
			dup++;
		}
		/* the same route twice (one left by an earlier start or
		 * load): keep the first */
		for (i = 0; i < n; i++) {
			unsigned int j2;

			if (!((e[i] >> 16) & 3))
				continue;
			for (j2 = 0; j2 < i; j2++)
				if (((e[j2] >> 16) & 3) && (e[j2] & 0xffff) == (e[i] & 0xffff))
					break;
			if (j2 == i)
				continue;
			/* ours stays: switch off the other one */
			x = i;
			for (k = 0; k < ARRAY_SIZE(keep); k++)
				if (keep[k] == i)
					x = j2;
			for (k = 0; k < ARRAY_SIZE(keep); k++)
				if (keep[k] == x)
					break;
			if (k < ARRAY_SIZE(keep))
				continue;	/* both ours */
			ae9_rt_write_checked(chip, 0x190000 + 4 * x, e[x] & 0xffff,
					     &unsteady, &retried, &failed);
			e[x] &= 0xffff;
			dup++;
		}
		if (dup)
			dev_info(chip->dev, "SBX: router: switched off %u extra DSP-mode route%s\n",
				 dup, dup == 1 ? "" : "s");
	}
	creative_dsp_chipio_write(chip, AE9_RT_APPLY, 1);
	creative_dsp_chipio_addr_invalidate();
	mutex_unlock(&chip->replay_mutex);
	ae9_dbg(chip, "SBX: router front L/R <- DSP ports 0x%02x/0x%02x, %d DSP input entr%s active\n",
		 l, r, inputs, inputs == 1 ? "y" : "ies");
	if (walked && (wrote || lanes_stray))
		dev_info(chip->dev, "SBX: router: DSP outputs routed in the router chip's own slots (stream 0x18 from 0x%02x): %u of %u lanes set (%u written; the router chip had %u at 0xff, %u off; %u other DSP route%s off)\n",
			 head18, lanes_set, walked, wrote, lanes_ff, lanes_parked,
			 lanes_stray, lanes_stray == 1 ? "" : "s");
	else if (walked)
		ae9_dbg(chip, "SBX: router: stream 0x18 from 0x%02x already routed (%u lanes)\n",
			head18, lanes_set);
	else if (chip->pcm_start_sent)
		dev_info_ratelimited(chip->dev, "SBX: router: stream 0x18's slot list %s (head %d): fixed slots, as before\n",
				     chain < 0 ? "not connected" : "not usable", head18);
	if (retried || (unsteady && ae9_debug))
		dev_info(chip->dev, "SBX: router: %u unsteady read%s, %u write%s retried, %u failed (the chip moved the ChipIO address)\n",
			 unsteady, unsteady == 1 ? "" : "s",
			 retried, retried == 1 ? "" : "s", failed);
	ff = ae9_rt_dsp_out_ff(e, n, &seen);
	if (judge)
		ae9_rt_ff_report(chip, ff, seen);
	if (ins)
		dev_warn(chip->dev, "SBX: router repaired: %d input feed%s (bad layout from the 8051)\n",
			 ins, ins == 1 ? "" : "s");
	if (!inputs)
		dev_warn(chip->dev, "SBX: the DSP's input path is not active in the router: no audio in DSP mode (try Direct Mode, or a PSU cold boot)\n");
}

/*
 * Playback start with Direct Mode off (seq 11066): 0x18b008, format, bind.
 * count_ff: DSP mode is fully up, so the router's 0xff count means
 * something (dsp_mode turns dsp_active on before the bind; it does its
 * own count afterwards).
 */
static unsigned int ae9_fmt_ch(u16 format)
{
	return (format & 0xf) + 1;
}

/*
 * Host enable of stream 0x05 (DarkNikGr's setup; Windows never sends it:
 * 0 stream 0x05 enables or disables in 28 captures, ~114 DSP-mode starts).
 * The node 0x02 bind already makes the 8051 connect stream 0x05 (count
 * 0 -> 1, plus a stream 0x0c reference), and ours always came after that
 * bind, so it never connected anything (the 8051 connects only on 0 -> 1):
 * it only added a second reference. That reference is what kept SRC 90/80
 * busy after Direct Mode on (2026-11-04, fixed then by a host stop there),
 * and what made stream 0x0c leak a reference whenever the last decrement
 * was our host stop instead of the unbind (project notes). Not sent
 * since 0.12.6 (the in05_host option until 0.12.12).
 */

/*
 * The router chip (the 8051 on ChipIO) takes stream binds and formats
 * (verbs 0x706 / 0x200 on node 0x02) at once but does the routing work
 * later, from its own queue, and that work reads the *current* stream
 * setting. A quick unbind -> format -> bind (PipeWire reopening the stream
 * at another bit depth) could reach it before the unbind was processed:
 * nothing was torn down and the link from host audio (connection point
 * 0x43) kept the old sample format, cached in its link record at XRAM
 * 0x0769 (0x31 24-bit / 0x41 32-bit). The DSP then read 32-bit samples as
 * 24-bit or the reverse: static on the right channel until the next
 * restart (2026-11-07; found by reading the 8051's ROM and catching its
 * tables while the static played: mismatch in both noisy states, match in
 * all seven clean ones). Fix: let the 8051 finish its queue before the
 * bind, and check the cached format after the start; on a mismatch, one
 * more unbind -> wait -> bind (the fmt_check option until 0.12.12).
 * Addresses are those of the tested firmware.
 */
#define AE9_8051_LINK_HOST_FMT	0x0769	/* link record 5 (0x43 -> 0x00), format */

/* ChipIO GET 0xf04: the 8051's work-queue depth (0 = idle). */
static int ae9_8051_queue_depth(struct creative_chip *chip)
{
	u32 resp = 0;

	if (creative_ae9_corb_send_verb_sync(chip, 0x115f0400, &resp) < 0)
		return -EIO;
	return resp & 0xff;
}

/*
 * Wait (up to ~100 ms) until the 8051 has worked through its queue. quiet:
 * a short wait goes to the debug log only (a giving up is always logged).
 */
static void ae9_8051_wait_idle_q(struct creative_chip *chip, const char *why, bool quiet)
{
	int d = 0, i;

	mutex_lock(&chip->replay_mutex);
	for (i = 0; i < 100; i++) {
		d = ae9_8051_queue_depth(chip);
		if (d <= 0)
			break;
		mutex_unlock(&chip->replay_mutex);
		usleep_range(1000, 1500);
		mutex_lock(&chip->replay_mutex);
	}
	mutex_unlock(&chip->replay_mutex);
	if (i && quiet && d <= 0)
		ae9_dbg(chip, "router chip: queue idle after %d ms (%s)\n", i, why);
	else if (i)
		dev_info(chip->dev, "router chip: queue idle after %d ms (%s)%s\n", i, why,
			d > 0 ? ", gave up" : "");
}

static void ae9_8051_wait_idle(struct creative_chip *chip, const char *why)
{
	ae9_8051_wait_idle_q(chip, why, false);
}

/* One byte of the 8051's XRAM (0x70d/0x70e address, 0xf07 read); reads
 * glitch while audio plays, so two in a row must agree. Caller holds
 * replay_mutex. */
static int ae9_8051_xram_byte_locked(struct creative_chip *chip, u16 addr)
{
	u32 r, prev = ~0U;
	int t, v = -EIO;

	for (t = 0; t < 6; t++) {
		r = 0;
		if (creative_ae9_corb_send_verb_sync(chip, 0x11570d00 | (addr & 0xff), &r) < 0 ||
		    creative_ae9_corb_send_verb_sync(chip, 0x11570e00 | (addr >> 8), &r) < 0 ||
		    creative_ae9_corb_send_verb_sync(chip, 0x115f0700, &r) < 0)
			break;
		r &= 0xff;
		if (r == prev) {
			v = r;
			break;
		}
		prev = r;
	}
	return v;
}

static int ae9_8051_xram_byte(struct creative_chip *chip, u16 addr)
{
	int v;

	mutex_lock(&chip->replay_mutex);
	v = ae9_8051_xram_byte_locked(chip, addr);
	creative_dsp_chipio_addr_invalidate();
	mutex_unlock(&chip->replay_mutex);
	return v;
}

/*
 * Stream 0x18 (host -> DSP) is reference-counted in the 8051: XRAM
 * 0x072f + 10 * id + 5 (0x0824), +1 per enable (param 28 = 1), -1 per
 * disable; it connects only on 0 -> 1 and tears down only at 0. The count
 * survives warm restarts (the AFG reset doesn't touch the 8051). We
 * enabled it at every DSP-mode switch without looking and disabled it
 * once, so it sat at 1 in Direct Mode and 2 in DSP mode, and after a warm
 * restart the new DSP-mode enable never connected: DSP mode and What U
 * Hear silent until a power cut (Kaiba, ASMedia, 0.12.4; project notes on
 * the router chip). Windows (FUN_000341e4 / FUN_000343d8) enables only at
 * count 0 and drains it to 0 at device stop. Release ours to 0 here.
 */
#define AE9_XRAM_STREAM0C_REFS	AE9_XRAM_STREAM_REFS(0x0c)	/* 0x07ac */
#define AE9_STREAM_DRAIN_MAX	8	/* stops per stream; a garbage read can't loop */

/*
 * Release stream id until its count reads 0 (host STREAM_CONTROL = 0; the
 * 8051 never takes a count below 0). *first gets the count before, the
 * return value the count after (< 0: the read failed).
 */
static int ae9_stream_drain(struct creative_chip *chip, u8 id, int *first)
{
	int v = ae9_8051_xram_byte(chip, AE9_XRAM_STREAM_REFS(id));
	unsigned int n = 0;

	if (first)
		*first = v;
	while (v > 0 && n++ < AE9_STREAM_DRAIN_MAX) {
		/* one stop, then 50 ms (Windows 48 ms) */
		creative_ae9_stream_release(chip, id);
		v = ae9_8051_xram_byte(chip, AE9_XRAM_STREAM_REFS(id));
	}
	return v;
}

static int ae9_stream18_drain(struct creative_chip *chip, const char *why)
{
	int first, v = ae9_stream_drain(chip, 0x18, &first);

	if (first != 0)
		dev_info(chip->dev, "router: stream 0x18 references %d -> %d (%s; stream 0x0c %d)\n",
			 first, v, why, ae9_8051_xram_byte(chip, AE9_XRAM_STREAM0C_REFS));
	return v;
}

/*
 * Stream 0x0c (DSP output -> 0x70) is counted the same way, and the 8051
 * also adds to it by itself: a converter bind that connects stream 0x05
 * (node 0x02, host audio) or 0x0b (node 0x0a, What U Hear) takes a 0x0c
 * reference, and only the disconnect that takes that stream to 0 gives it
 * back (bank 2 D529 / D69E; project notes). Our extra host reference on
 * 0x05 (until 0.12.6) could make the last decrement a host stop, which never
 * releases 0x0c, so 0x0c gained a reference in some sessions; shutdown
 * stopped it once, blindly, and the next start-up tail added +1 on top
 * (3xHelix 4 -> 5, Kaiba 3 -> 6). Windows reads and drains 0x18 and 0x0c at
 * every device stop (FUN_000343d8; 0x07ac read 1 at 26/26 stops) and never
 * enables 0x05. So: at shutdown and before the start-up tail, drain every
 * stream we or our binds can hold (0x05 first, it holds 0x0c; 0x0c last).
 * A load then starts as on a cold card: 0x0c = 1 and 0x18 = 1 after the
 * tail, 0x05 = 0. The tail itself enables 0x0c and 0x18, so this must run
 * BEFORE it (a cold card reads 0x18 = 1 after it; UVFindings1, uv6 run 1).
 * The old way, the stream_drain option until 0.12.12: one blind 0x0c stop
 * at shutdown, stream 0x18 only at start-up.
 */
static const u8 ae9_drain_ids[] = { 0x05, 0x0b, 0x0f, 0x14, 0x18, 0x0c };

/* 0 = all read 0 now; < 0 = a read failed (stopped there); > 0 = a count
 * still non-zero after AE9_STREAM_DRAIN_MAX stops */
static int ae9_streams_drain(struct creative_chip *chip, const char *why)
{
	char msg[160];
	size_t len = 0;
	int ret = 0, first, v;
	unsigned int i;

	msg[0] = 0;
	/* binds and unbinds are worked off from the 8051's queue: let the
	 * last unbind give its references back before reading the counts */
	ae9_8051_wait_idle(chip, why);
	for (i = 0; i < ARRAY_SIZE(ae9_drain_ids); i++) {
		v = ae9_stream_drain(chip, ae9_drain_ids[i], &first);
		if (first != 0 && len < sizeof(msg))
			len += scnprintf(msg + len, sizeof(msg) - len, "%s0x%02x %d -> %d",
					 len ? ", " : "", ae9_drain_ids[i], first, v);
		if (v < 0) {
			ret = v;
			break;
		}
		if (v > 0)
			ret = 1;
	}
	if (ret)
		dev_warn(chip->dev, "router: stream references %s (%s): %s\n",
			 ret < 0 ? "unreadable" : "NOT released", why, msg);
	else if (len)
		dev_info(chip->dev, "router: stream references released (%s): %s\n", why, msg);
	else
		dev_info(chip->dev, "router: stream references all 0 (%s)\n", why);
	return ret;
}

/* the same, for the shutdown (creative_ae9_seq_run.c) */
int creative_ae9_streams_drain(struct creative_chip *chip, const char *why)
{
	return ae9_streams_drain(chip, why);
}

static void ae9_dsp_mode_start(struct creative_chip *chip, u16 format,
			       bool count_ff)
{
	/*
	 * A new stream with another channel count (2 <-> 6) while DSP mode is
	 * up: restarting only stream 0x05 at the new count gave static on the
	 * front and silence elsewhere for the whole stream (2026-11-04, first
	 * 6-ch test after a stereo stream); a stream with the same count was
	 * fine. Windows never changes the count (always 6 ch into the DSP).
	 * Go through Direct Mode and back instead, the round trip every
	 * Direct Mode toggle uses; dsp_mode(true) binds this stream and opens
	 * 0x05 at its count.
	 */
	if (chip->in05_ch && chip->in05_ch != ae9_fmt_ch(format)) {
		dev_info(chip->dev, "SBX: stream now %u ch (was %u): DSP path rebuilt\n",
			 ae9_fmt_ch(format), chip->in05_ch);
		creative_ae9_dsp_mode(chip, false);
		/* Windows waits 144 ms after leaving DSP mode before the next
		 * command (capture 6, seq 11808); two rebuilds back to back
		 * without it (2 ch then 6 ch, 0.7 s apart) ended silent */
		msleep(150);
		/* so dsp_mode(true) binds this stream at its format (a fresh
		 * open still has pcm_start_sent at 0 here) */
		chip->pcm_start_fmt = format;
		chip->pcm_start_sent = 1;
		creative_ae9_dsp_mode(chip, true);
		return;
	}
	ae9_8051_wait_idle(chip, "before the bind");
	/*
	 * stream 0x05's channel count, a parameter only (no routing happens
	 * at this write): before the bind, so the connect the bind starts
	 * sees the stream's count
	 */
	creative_ae9_dsp_in_channels(chip, format);
	creative_ae9_dsp_bind(chip, format);	/* 0x18b008, the format, the bind */
	chip->in05_ch = ae9_fmt_ch(format);	/* the count stream 0x05 was bound at */
	/* the format the router chip cached (see AE9_8051_LINK_HOST_FMT) */
	{
		int got;

		ae9_8051_wait_idle(chip, "after the bind");
		got = ae9_8051_xram_byte(chip, AE9_8051_LINK_HOST_FMT);
		if (got >= 0 && got != (format & 0xff)) {
			dev_info(chip->dev, "router chip kept the old stream format (0x%02x, stream 0x%02x): unbind and bind again\n",
				 got, format & 0xff);
			creative_ae9_dsp_unbind(chip);
			ae9_8051_wait_idle(chip, "after the repair unbind");
			creative_ae9_dsp_rebind(chip, format);
			ae9_8051_wait_idle(chip, "after the repair bind");
			got = ae9_8051_xram_byte(chip, AE9_8051_LINK_HOST_FMT);
			dev_info(chip->dev, "router chip stream format now 0x%02x (%s)\n", got,
				 got == (format & 0xff) ? "fixed" : "STILL WRONG");
		} else {
			dev_info(chip->dev, "router chip stream format 0x%02x ok\n", got);
		}
	}
	creative_ae9_router_front_dsp(chip, count_ff);
}

void creative_ae9_dsp_mode_start(struct creative_chip *chip, u16 format)
{
	ae9_dsp_mode_start(chip, format, chip->dsp_active);
}

void creative_ae9_dsp_mode(struct creative_chip *chip, bool dsp)
{
	u32 zero = 0;	/* 0.0f: 0 dB, and "not muted" */
	int refs18;

	if (!chip->corb_buf || !chip->dsp_done)
		return;
	if (!dsp) {
		/* Windows' Direct Mode on (seq 11794-11812): node 0x02 unbound, stream 0x18 off */
		creative_ae9_dsp_route_off(chip);
		ae9_stream18_drain(chip, "Direct Mode on");
		/*
		 * Stream 0x05 must be torn down here: left running it keeps the
		 * HDA -> SRC 0x90/0x80 -> DSP path in use, and the next Direct
		 * Mode off got a second path on SRC 0x98/0x88 that never started:
		 * silence or crunched audio, until a PSU cold boot (2026-11-04
		 * router dumps; then caused by our own host enable). Without that
		 * enable the unbind above takes it to 0 (the 8051 disconnects it
		 * and gives 0x0c back). Anything still counted is left over:
		 * release it, and say so.
		 */
		{
			int first, v;

			/* the unbind is worked off from the 8051's queue: a host
			 * stop before it would take the count to 0 without giving
			 * 0x0c back (the leak this replaces) */
			ae9_8051_wait_idle(chip, "after the Direct Mode unbind");
			v = ae9_stream_drain(chip, 0x05, &first);

			if (first > 0)
				dev_info(chip->dev, "router: stream 0x05 still had %d reference%s after the unbind: released (-> %d)\n",
					 first, first == 1 ? "" : "s", v);
			chip->in05_ch = 0;
		}
		chip->dsp_active = false;
		ae9_rt_dsp_dac_off(chip, true, "Direct Mode on");
		chip->rt_direct_swept = true;
		return;
	}
	chip->dsp_active = true;
	chip->rt_direct_swept = false;
	/*
	 * Windows' Direct Mode off (seq 9940-10108): node 0x02 unbound, 0x14
	 * off, 0x05 -> 0x00, 0x18: 0x09 -> 0xd0 at 96 kHz 6 ch on, the 96 kHz
	 * I2S, the DAC re-lock, the I2S pins. Its block enables stream 0x18;
	 * Windows (FUN_000341e4) enables it only when its count is 0 (the
	 * start-up tail's connection still stands after a boot): with a count
	 * above 0 that extra reference is taken back, so DSP mode on/off
	 * leaves the count where it was.
	 */
	refs18 = ae9_8051_xram_byte(chip, AE9_XRAM_STREAM18_REFS);
	creative_ae9_dsp_route_on(chip, refs18 > 0);
	/* a stream already playing: bind it again, as a new one would be */
	if (chip->pcm_start_sent)
		ae9_dsp_mode_start(chip, chip->pcm_start_fmt, false);
	/*
	 * DSP speaker volume (module 0x32) to 0 dB; the start block leaves
	 * Windows' capture-time -36.19 dB there, and Windows keeps it in step
	 * with its volume. It isn't in our speaker path, though (2026-11-04:
	 * -20 dB and +6 dB there made no audible or measured difference), so
	 * this only keeps the DSP's state tidy. Our volume is the DAC's.
	 */
	mutex_lock(&chip->replay_mutex);
	creative_dsp_scp_send(chip, (3 << 17) | (0x20 << 8) | 0x32, &zero, 1);
	creative_dsp_scp_send(chip, (4 << 17) | (0x20 << 8) | 0x32, &zero, 1);
	creative_dsp_scp_send(chip, (2 << 17) | (0x20 << 8) | 0x32, &zero, 1);
	creative_dsp_chipio_addr_invalidate();
	mutex_unlock(&chip->replay_mutex);
	ae9_dbg(chip, "SBX: DSP speaker volume 0 dB (left, right), unmuted\n");
	/*
	 * Host-audio stream 0x05: the bind above has connected it, as on
	 * Windows (the host enable of DarkNikGr's setup, "routed but never
	 * STARTED before", went in 0.12.6; see "Host enable of stream
	 * 0x05" above). Then the front outputs from the DSP (also done by
	 * dsp_mode_start, for later streams).
	 */
	creative_ae9_router_front_dsp(chip, true);
	creative_ae9_dsp_alive_log(chip, "DSP mode on");
}

/*
 * How long a fresh direct stream plays before the switch to DSP mode.
 * 300 ms was too soon (2026-11-04: echoey, crunched audio that toggling
 * Direct Mode didn't clear); switching by hand seconds later works.
 * (The dsp_switch_ms option until 0.12.12.)
 */
#define AE9_DSP_SWITCH_MS	3000U

static void creative_ae9_dsp_switch_work(struct work_struct *w)
{
	struct creative_chip *chip = container_of(to_delayed_work(w),
						  struct creative_chip,
						  dsp_switch_work);

	if (READ_ONCE(chip->unloading) ||
	    !chip->dsp_switch_pending || chip->fx.direct || !chip->pcm_start_sent)
		return;
	chip->dsp_switch_pending = false;
	dev_info(chip->dev, "SBX: switching the running stream to DSP mode (after %u ms)\n",
		 AE9_DSP_SWITCH_MS);
	creative_ae9_fx_restore(chip);	/* DSP mode + the SBX profile */
}

/*
 * Direct Mode on while DSP mode played (fx.c fx_put): the stream, now
 * unbound, to XRUN so the app restarts it on the direct route. From a work,
 * not from fx_put: the substream can close (and its runtime be freed) at
 * any moment, and only the PCM's open_mutex, which close runs under,
 * keeps it; fx_put runs under the card's controls_rwsem, which the PCM
 * disconnect takes inside open_mutex, so taking open_mutex there could
 * deadlock. Here it can be taken, as dead_work does. A stream that closed
 * meanwhile is gone: nothing to stop; one opened since starts unrouted
 * anyway (pcm_start_sent was cleared), so the xrun is harmless.
 */
static void creative_ae9_pb_restart_work(struct work_struct *w)
{
	struct creative_chip *chip = container_of(w, struct creative_chip,
						  pb_restart_work);
	struct snd_pcm_substream *ss;

	if (READ_ONCE(chip->unloading) || !chip->pcm)
		return;
	mutex_lock(&chip->pcm->open_mutex);
	ss = chip->playback_substream;
	if (ss && ss->runtime)
		snd_pcm_stop_xrun(ss);
	mutex_unlock(&chip->pcm->open_mutex);
}

/* Playback trigger start (atomic): run a pending switch shortly after. */
void creative_ae9_dsp_switch_kick(struct creative_chip *chip)
{
	if (chip->dsp_switch_pending && !chip->fx.direct)
		queue_delayed_work(chip->wq, &chip->dsp_switch_work,
				   msecs_to_jiffies(AE9_DSP_SWITCH_MS));
}

/*
 * Direct Mode playback start: the HDA -> DAC routes go into the router
 * slots the 8051 gave the stream, not into fixed ones. The 8051 keeps a
 * slot list per link: XRAM 0x157d = the first slot of link 5 (host audio,
 * stream 0x05; 0x1578 + stream id), XRAM 0x1478 + slot = the next one,
 * 0xff = the end.
 * Windows (CtxHda.sys FUN_0003cdb4 stereo, FUN_00033f38 multichannel)
 * walks it after the bind and only sets each slot's destination; all 80
 * Direct starts in the captures do (2 to 24 slots, heads 0x18 to 0x30,
 * the list always ending right after the last slot).
 * Our block wrote 0x190060/64 (slots 0x18/0x19) whatever the head was.
 * After a stop the 8051 hands those slots to the next connection, and
 * with What U Hear bound then they held its DSP cc/cd -> HDA capture
 * 00/01 route: our start overwrote it and the recording stalled until a
 * reload (uv11, 2026-11-09: head 0x1a, slots 0x18/0x19 = 0x100cc/0x101cd).
 *
 * Destination of slot k: stereo 0x40 + k; 6 channels FL FR C LFE BL BR on
 * the DAC pairs 0x40, 0x50, 0x60, sample s of a channel on pair + 2s
 * (port51[k % 6] + 2 * (k / 6)). The source is the 8051's (always k in
 * the captures). A head that can't be read or makes no sense falls back
 * to the fixed slots as before; a chain that breaks later stops there,
 * as Windows does. Since 0.12.6 (the rt_chain_walk option until 0.12.12).
 * Called by the playback start (creative_ae9_seq_run.c) between the start
 * block's two halves; takes replay_mutex itself.
 */
#define AE9_XRAM_LINK5_HEAD	0x157d	/* first router slot of host audio -> DACs */
#define AE9_WALK_MAX		24	/* 5.1 at 192 kHz; stereo 384 kHz = 16 */

void creative_ae9_rt_direct_walk(struct creative_chip *chip, unsigned int nslots, bool six)
{
	static const u8 port51[6] = { 0x40, 0x41, 0x50, 0x51, 0x60, 0x61 };
	unsigned int k, unsteady = 0, retried = 0, failed = 0, nsrc = 0, parked = 0;
	DECLARE_BITMAP(seen, AE9_RT_ENTRIES);
	char slots[AE9_WALK_MAX * 3 + 1] = "";
	const char *why = NULL;
	int head, s, len = 0;

	nslots = min_t(unsigned int, nslots, AE9_WALK_MAX);
	bitmap_zero(seen, AE9_RT_ENTRIES);
	/*
	 * the list is the 8051's, built by the bind just sent: let it finish
	 * connecting first, as the DSP mode walk does (one 0xf04 read when it
	 * already has; a short wait goes to the debug log). Windows reads the
	 * head straight away, its list always complete in the captures: the
	 * wait is ours. Since 0.12.13.
	 */
	ae9_8051_wait_idle_q(chip, "before the stream slot walk", true);
	mutex_lock(&chip->replay_mutex);
	head = s = ae9_8051_xram_byte_locked(chip, AE9_XRAM_LINK5_HEAD);
	for (k = 0; k < nslots; k++) {
		u8 dst = six ? port51[k % 6] + 2 * (k / 6) : 0x40 + k;
		u32 a, v;

		if (s < 0) {
			why = "XRAM read failed";
			break;
		}
		if (s == 0xff) {
			why = "list ended early";
			break;
		}
		if (s < AE9_RT_FIRST_SLOT || s >= AE9_RT_ENTRIES ||
		    test_and_set_bit(s, seen)) {
			why = "bad slot number";
			break;
		}
		a = 0x190000 + 4 * s;
		/* 0x0bad0add = a read of nothing; upper bits are never set */
		if (ae9_chipio_read_stable(chip, a, &v, &unsteady) < 0 || (v >> 18)) {
			why = "slot read failed";
			break;
		}
		/* an HDA playback channel; anything else is a glitched read */
		if ((v & 0xff) >= 0x20) {
			v = (v & ~0xffU) | k;
			nsrc++;
		}
		/* always on (state 1) in the captures; kept as is, like Windows */
		if (!(v & 0x10000))
			parked++;
		ae9_rt_write_checked(chip, a, (u32)dst << 8 | (v & 0xfffd00ff),
				     &unsteady, &retried, &failed);
		len += scnprintf(slots + len, sizeof(slots) - len, " %02x", s);
		s = ae9_8051_xram_byte_locked(chip, AE9_XRAM_SLOT_NEXT + s);
	}
	if (!k)	/* nothing usable: the fixed slots, as before */
		for (k = 0; k < nslots; k++)
			ae9_rt_write_checked(chip, AE9_RT_FRONT_L + 4 * k,
					     0x10000 | (six ? port51[k % 6] + 2 * (k / 6) :
						        0x40 + k) << 8 | k,
					     &unsteady, &retried, &failed);
	creative_dsp_chipio_addr_invalidate();
	mutex_unlock(&chip->replay_mutex);

	if (!len)
		dev_info(chip->dev, "router: stream slot list unusable (head %d, %s): %u fixed slots from 0x190060, as before\n",
			 head, why, nslots);
	else if (why)
		dev_info(chip->dev, "router: stream slot list: %s after %u of %u slots (head 0x%02x:%s)\n",
			 why, k, nslots, head, slots);
	else
		ae9_dbg(chip, "router: stream slot list from 0x%02x:%s -> %s 0x40.. (%u slots)\n",
			head, slots, six ? "5.1" : "stereo", k);
	if (nsrc || parked || retried || failed)
		dev_info(chip->dev, "router: stream slot list: %u source%s fixed up, %u slot%s off, %u write%s retried, %u failed\n",
			 nsrc, nsrc == 1 ? "" : "s", parked, parked == 1 ? "" : "s",
			 retried, retried == 1 ? "" : "s", failed);
}

/*
 * One-line DSP health summary after the startup tail, for the log only.
 * Word 5 of the overlay's jump table at XRAM 0x3f3a0 is written only by the
 * overlay transfer and reads 0x04000086 when it landed (the value the other
 * AE-9 projects check; same overlay file). DBGCNTL 0x00003400 is what we see
 * when the DSP has stalled after repeated reloads (healthy boots show
 * 0x00470000 / 0x00490000 / 0x004d0000). Nothing acts on the result.
 */
#define AE9_OVL_CHECK_ADDR	0x3f3b4
#define AE9_OVL_CHECK_WANT	0x04000086
#define AE9_DBGCNTL_STALLED	0x00003400

static void creative_ae9_dsp_health(struct creative_chip *chip, u32 dbgcntl)
{
	unsigned int ovl = 0;
	bool ovl_ok, stalled = dbgcntl == AE9_DBGCNTL_STALLED;

	if (!chip->overlay_sent) {
		dev_info(chip->dev, "DSP health: overlay not sent, not checked; DBGCNTL 0x%08x%s\n",
			 dbgcntl, stalled ? " (stall pattern)" : "");
		return;
	}
	if (creative_dsp_chipio_read(chip, AE9_OVL_CHECK_ADDR, &ovl) < 0) {
		dev_warn(chip->dev, "DSP health: overlay check read failed; DBGCNTL 0x%08x%s\n",
			 dbgcntl, stalled ? " (stall pattern)" : "");
		return;
	}
	creative_dsp_chipio_addr_invalidate();
	ovl_ok = ovl == AE9_OVL_CHECK_WANT;
	if (ovl_ok && !stalled)
		dev_info(chip->dev, "DSP health: ok (overlay 0x%08x, DBGCNTL 0x%08x)\n",
			 ovl, dbgcntl);
	else
		dev_warn(chip->dev, "DSP health: overlay 0x%08x (%s), DBGCNTL 0x%08x (%s); if recording is dead, power off at the PSU\n",
			 ovl, ovl_ok ? "ok" : "want 0x04000086",
			 dbgcntl, stalled ? "stall pattern" : "ok");
}

/*
 * After our firmware is running: the MASTERCONTROL boot handshake, then
 * Windows' set-up after its own (the start-up tail, creative_ae9_tail() in
 * creative_ae9_seq.c), the summary line, the failed DSP messages once more,
 * the Direct Mode start and the head unit's set-up.
 */
static void creative_ae9_post_fw(struct creative_chip *chip)
{
	struct ae9_tail tail;
	unsigned int dbg = 0;
	int ret;

	AE9_HOOK(chip, post_fw, chip);
	/*
	 * No DBGCNTL writes here: the verb log shows dspload_image() already
	 * starts the DSP exactly as Windows does (read 0xfff83cf0, write
	 * 0xfff83c00, write 0xfff83c0f). Writing 0xfff83c00 again on the
	 * running DSP clears its run bits. While it runs, DBGCNTL reads back
	 * values like 0x004d0000; Windows never reads it at that point.
	 */
	creative_dsp_chipio_read(chip, 0x100e30, &dbg);
	dev_info(chip->dev, "fw_split: DBGCNTL (running) 0x%08x\n", dbg);

	if (chip->hs_done) {
		dev_info(chip->dev, "fw_split: boot handshake already run (hs_immediate)\n");
	} else {
		ret = creative_dsp_boot_handshake(chip);
		dev_info(chip->dev, "fw_split: boot handshake %s (%d)\n",
			 ret ? "INCOMPLETE" : "complete", ret);
	}

	/* the tail enables stream 0x18 once (tail_engine_streams()): a
	 * card that was running still holds references from the earlier run,
	 * which would swallow that enable (no connection, DSP mode and even
	 * Direct Mode silent: Kaiba 0.12.4). Release them first, so the tail
	 * connects from 0 as on a cold card (which reads 0 here). The same for
	 * 0x0c (the tail enables it blindly, as Windows) and the streams that
	 * hold 0x0c references (0x05, 0x0b): every count here is left over
	 * (the init table's params 2/0/1 = 0 already released the 8051's own,
	 * and nothing is bound yet), so this load starts at 0x0c = 1 */
	ae9_streams_drain(chip, "before the start-up tail, left by an earlier run");

	creative_ae9_tail(chip, &tail);
	creative_dsp_chipio_read(chip, 0x100e30, &dbg);
	dev_info(chip->dev,
		 "fw_split: tail replay done: %u SCP msgs (%u failed, %u skipped, %u dropped), %u verbs (%u failed); DBGCNTL 0x%08x\n",
		 tail.msgs, tail.msg_err, tail.msg_skipped, tail.msg_dropped, tail.verbs,
		 tail.verb_err, dbg);
	creative_ae9_dsp_health(chip, dbg);
	creative_ae9_dsp_alive_log(chip, "boot");
	creative_ae9_tail_resend(chip, &tail);

	/*
	 * Windows' Direct Mode start-up: its init connects stream 0x18 (DSP out
	 * -> DACs), then, with Direct Mode the saved setting, reads its count
	 * and stops it (FUN_00032ee8 -> FUN_00039a48(0) -> FUN_00034250; capture
	 * 6 seq 5184-5196, 19 of 19 Direct-mode boots). Our tail is Windows'
	 * DSP-mode boot path and every start-up of ours begins direct (DSP mode
	 * switches in with the first stream), so do the same; DSP mode on
	 * connects it again. Then the DAC-port allocator: an allocation left on
	 * connection point 0xd1 (stream 0x18's ROM default: a refused select at
	 * an earlier first load connected it there; the router busy poll stops that)
	 * holds DAC groups 1-3 until a power-off and makes the 8051's own
	 * DSP-mode routing fail. Only stream 0x18 can hold 0xd1, so with its
	 * count at 0 the 8051's own port free (0x713 0xd1: ROM E4A6 -> 656D ->
	 * 6FFF) releases it (uv87: cfg 0x19 -> 0x18, groups 0x0e -> 0x00, and
	 * the 8051 routed DSP mode itself after). Logged at every load.
	 */
	if (chip->corb_buf && !chip->dsp_active) {
		int d0, d1, g, d1a, ga, refs, k;
		u32 r;

		/* the tail's last 8051 jobs first, then the "before" bytes */
		ae9_8051_wait_idle(chip, "after the start-up tail");
		d0 = ae9_8051_xram_byte(chip, 0x1058);
		d1 = ae9_8051_xram_byte(chip, 0x1059);
		g = ae9_8051_xram_byte(chip, 0x1107);
		ae9_stream18_drain(chip, "start-up in Direct Mode, as Windows");
		refs = ae9_8051_xram_byte(chip, AE9_XRAM_STREAM18_REFS);
		d1a = ae9_8051_xram_byte(chip, 0x1059);
		ga = ae9_8051_xram_byte(chip, 0x1107);
		/* cp 0xc1 shares the config byte: only with nothing on 0xc0-0xcf */
		if (refs == 0 && d1a > 0 && (d1a & 3) &&
		    ae9_8051_xram_byte(chip, 0x1106) == 0) {
			int before = d1a, gb = ga;

			for (k = 0; k < 4 && d1a > 0 && (d1a & 3); k++) {
				ae9_8051_wait_idle(chip, "before the 0xd1 port free");
				creative_ae9_corb_send_verb_sync(chip, 0x115713d1, &r);
				d1a = ae9_8051_xram_byte(chip, 0x1059);
			}
			ga = ae9_8051_xram_byte(chip, 0x1107);
			if (d1a >= 0 && !(d1a & 3))
				dev_info(chip->dev, "router: freed a DAC allocation left on connection point 0xd1 (an earlier first load): cfg 0x%02x -> 0x%02x, DAC groups 0x%02x -> 0x%02x\n",
					 before, d1a, gb & 0xff, ga & 0xff);
			else
				dev_warn(chip->dev, "router: could not free the DAC allocation on connection point 0xd1 (cfg %d): DSP mode routes it itself (stream 0x18); a power-off clears it\n",
					 d1a);
		}
		/* -1 = a read failed; counters add up since the driver loaded */
		dev_info(chip->dev, "router: after the start-up tail: 0xd0 cfg %d, 0xd1 cfg %d, DAC groups %d; stream 0x18 drained to %d, now 0xd1 cfg %d, DAC groups %d (router chip param verbs since load: %u waited for it to be idle, %u refused and resent, %u lost)\n",
			 d0, d1, g, refs, d1a, ga, chip->r51_waited, chip->r51_refused, chip->r51_lost);
	}

	/* Windows' post-boot BAR2 activity: CA0113 commands, head-unit SPI
	 * frames (incl. the "-SP-" speaker label), GPIO writes */
	creative_ae9_post_dsp_setup(chip);
	/* the replay switched phantom power off (f0 05 03 02 01 00); hold
	 * "on" requests off for a while, see hu_phantom_put */
	chip->hu_phantom_req = -1;
	chip->hu_phantom_hold = jiffies + AE9_PHANTOM_HOLD_S * HZ;
	chip->rt_ff_logged = false;
	chip->rt_direct_swept = false;
}

/* ------------------------------------------------------------------ */
/* BAR2 extended register initialization				  */
/*								  */
/* Adapted from kernel patch_ca0132.c ca0113_mmio_init_address_ae5    */
/* and ca0113_mmio_init_data_ae5, with AE-7 tweaks (our 0x0071       */
/* variant is closest to QUIRK_AE7 = 0x0081).			  */
/*								  */
/* The kernel AE-5/7 init differs from our VFIO capture version:     */
/*   - Stream 0 enable (0x400=1) BEFORE status clears		   */
/*   - 0x1c = 0x00000680 -> 0x00880680 (AE-7 pre-writes; bit 9 = codec 2 off) */
/*   - Writes 0x804 = 0x57 (DSP config)				   */
/*   - Writes 0x20c = 0x00800001 (AE-7) / 0x00800000 (AE-5)	   */
/*   - Writes 0x830 = 0x0aff (not preserve)			   */
/*   - Additional init at 0xc00-0xc0c				   */
/*   - Second 0x800 = 0x6b write after 0x86c=1			   */
/* No explicit I2C/DSP init — the kernel driver relies on DSP        */
/* firmware (ctefx-desktop.bin) for ACM communication.		   */
/* ------------------------------------------------------------------ */
#define CREATIVE_BAR2_NUM_STREAMS	4
#define CREATIVE_BAR2_STREAM_STRIDE	0x40
#define CREATIVE_DSP_WAIT_MS		50

/* Windows' BAR2 init: creative_ae9_bar2_init_windows() (creative_ae9_seq_run.c) */

/* ------------------------------------------------------------------ */
/* BAR2 init: VFIO-capture-based (Phase 1-5)			  */
/* This is the sequence that previously produced ACM display output. */

/* ------------------------------------------------------------------ */
/* debugfs register-poke interface					  */
/* Commands written to /sys/kernel/debug/creative_ae9/cmd:	  */
/*   w2 <off> <val>  - write BAR2[off] = val			  */
/*   r2 <off>        - read BAR2[off]				  */
/*   (not 0x840, 0x854 or 0x860: the DAC I2C status registers clear  */
/*   on read, and r2 doesn't take ca0113_mutex: it races the driver  */
/*   and steals a DAC command's NACK; "dacabrt 1" and debugfs state  */
/*   show them instead)						  */
/*   w0 <off> <val>  - write BAR0[off] = val			  */
/*   r0 <off>        - read BAR0[off]				  */
/* Result readable from the same file.				  */
/* ------------------------------------------------------------------ */
static struct creative_chip *g_dbg_chip;
static char g_dbg_result[512];

static ssize_t creative_dbg_write(struct file *f, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	char buf[256];
	char op[8];
	unsigned int off, val;
	int n;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (!g_dbg_chip)
		return -ENODEV;

	/*
	 * Experiments without a rebuild:
	 *   "verb 1157171d"        one raw codec verb (CORB, response logged)
	 *   "scp 32 3 c1f00000"    one DSP SET: module, request, value (float bits)
	 */
	if (!strncmp(buf, "verb ", 5)) {
		u32 v, resp = 0;
		int ret;

		if (kstrtou32(strim(buf + 5), 16, &v))
			return -EINVAL;
		mutex_lock(&g_dbg_chip->replay_mutex);
		ret = creative_ae9_corb_send_verb_dbg(g_dbg_chip, v, &resp);
		creative_dsp_chipio_addr_invalidate();
		mutex_unlock(&g_dbg_chip->replay_mutex);
		dev_info(g_dbg_chip->dev, "dbg verb 0x%08x -> 0x%08x (%d)\n", v, resp, ret);
		return len;
	}
	/* "vlogreset": start the verb log over (log just one test) */
	if (!strncmp(buf, "vlogreset", 9)) {
		atomic_set(&vlog_n, 0);
		scnprintf(g_dbg_result, sizeof(g_dbg_result), "verb log reset\n");
		return len;
	}
	/* "scpprobe <mid> <req> <count> <wait_us>": raw reply timing probe */
	if (!strncmp(buf, "scpprobe ", 9)) {
		unsigned int mid, req, cnt, wus;
		int ret;

		if (sscanf(buf + 9, "%x %x %u %u", &mid, &req, &cnt, &wus) != 4 ||
		    cnt > 500 || wus > 20000)
			return -EINVAL;
		mutex_lock(&g_dbg_chip->replay_mutex);
		ret = creative_dsp_scp_probe(g_dbg_chip, mid, req & 0x7f, cnt, wus);
		creative_dsp_chipio_addr_invalidate();
		mutex_unlock(&g_dbg_chip->replay_mutex);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "scpprobe done (%d); results in the kernel log\n", ret);
		return len;
	}
	if (!strncmp(buf, "scp ", 4)) {
		unsigned int mid, req;
		u32 v;
		int ret;

		if (sscanf(buf + 4, "%x %x %x", &mid, &req, &v) != 3)
			return -EINVAL;
		mutex_lock(&g_dbg_chip->replay_mutex);
		ret = creative_dsp_scp_send(g_dbg_chip, ((req & 0x7f) << 17) |
					    (0x20 << 8) | (mid & 0xff), &v, 1);
		creative_dsp_chipio_addr_invalidate();
		mutex_unlock(&g_dbg_chip->replay_mutex);
		dev_info(g_dbg_chip->dev, "dbg scp 0x%02x/0x%02x = 0x%08x (%d)\n",
			 mid, req, v, ret);
		return len;
	}

	/* "dacr 48 1d": one DAC (CA0113 I2C) register read, Windows' read
	 * sequence, checked (dsp.c: the read's two 0x208 values, "not
	 * acknowledged" for an I2C NACK); "dacw 48 19 0": one DAC register
	 * write. Groups 0x48 (ES9038Q2M) and 0x49 (SABRE9006A) only. Also in
	 * the result (cat cmd). */
	if (!strncmp(buf, "dacr ", 5) || !strncmp(buf, "dacw ", 5)) {
		unsigned int grp = 0, reg = 0, v = 0;
		bool wr = buf[3] == 'w', ok;
		u32 raw[2] = { 0, 0 };
		int r;

		n = sscanf(buf + 5, "%x %x %x", &grp, &reg, &v);
		ok = grp == 0x48 || grp == 0x49;
#ifdef AE9_TEST
		/* a test build also reads (never writes) any other 7-bit
		 * address but 0x57 (likely an EEPROM) and the AE-7's 0x30:
		 * an absent device's NACK, for "dacabrt 1" (0.12.16) */
		ok |= !wr && grp >= 0x08 && grp <= 0x77 && grp != 0x57 && grp != 0x30;
#endif
		if (n != (wr ? 3 : 2) || !ok || reg > 0xff || v > 0xff)
			return -EINVAL;
		if (wr) {
			const u8 cmd[1][3] = { { grp, reg, v } };

			r = creative_dsp_ca0113_list_quiet(g_dbg_chip, cmd, 1);
			dev_info(g_dbg_chip->dev, "dbg dacw 0x%02x/0x%02x = 0x%02x%s\n", grp, reg, v,
				 r ? " (not acknowledged)" : "");
		} else {
			r = creative_dsp_ca0113_read_checked(g_dbg_chip, grp, reg, raw);
			if (r >= 0)
				scnprintf(g_dbg_result, sizeof(g_dbg_result),
					  "dacr 0x%02x/0x%02x = 0x%02x (0x208 0x%08x, then 0x%08x)\n",
					  grp, reg, r, raw[0], raw[1]);
			else
				scnprintf(g_dbg_result, sizeof(g_dbg_result),
					  "dacr 0x%02x/0x%02x: %s (0x208 0x%08x, then 0x%08x)\n",
					  grp, reg, r == -ENODEV ? "card off the bus" :
					  "not acknowledged (I2C NACK)", raw[0], raw[1]);
			dev_info(g_dbg_chip->dev, "dbg %s", g_dbg_result);
		}
		return len;
	}

	/* "dacabrt 1" / "dacabrt 0" (default): with each DAC command's status
	 * also read the I2C block's TX_ABRT_SOURCE (BAR2 0x880), before the
	 * reads that clear it: a NACK's cause (bit 0 address, bit 3 data) in
	 * debugfs state. Windows never reads it, so off by default (dsp.c). */
	if (!strncmp(buf, "dacabrt ", 8)) {
		unsigned int on;

		if (kstrtouint(buf + 8, 0, &on) || on > 1)
			return -EINVAL;
		mutex_lock(&g_dbg_chip->ca0113_mutex);
		g_dbg_chip->dac_i2c.abrt_src = on;
		mutex_unlock(&g_dbg_chip->ca0113_mutex);
		scnprintf(g_dbg_result, sizeof(g_dbg_result), "dacabrt %u\n", on);
		dev_info(g_dbg_chip->dev, "dbg dacabrt %u: TX_ABRT_SOURCE (0x880) %s\n", on,
			 on ? "read with each DAC command's status" : "not read (as Windows)");
		return len;
	}

	/* "hutext ABCD": hold text on the head-unit display (shown at the
	 * next poll, within ~0.5 s); "hutext" alone goes back to the normal
	 * volume/MUTE display. Printable ASCII only. The frame carries 8
	 * characters but the display has 4 cells (left-aligned, no scroll;
	 * dots ride on the cell before them), so only the first 4 show. */
	/*
	 * "huframe f0 85 01 02 f7": a raw head-unit frame (hex bytes), or
	 * several back to back (up to 48 bytes), each f0 <cmd> <len> <len
	 * bytes> f7. The heartbeat sends them on its next cycle, each right
	 * after the previous reply (the head unit's unlock, read and relock
	 * must come together), so they never collide with the poll; the
	 * replies are logged and shown by reading cmd.
	 */
	if (!strncmp(buf, "huframe", 7)) {
		u8 tx[48];
		unsigned long flags;
		unsigned int b;
		int k = 0, used, i = 7, p = 0;

		while (k < 48 && sscanf(buf + i, " %x%n", &b, &used) == 1) {
			tx[k++] = b & 0xff;
			i += used;
		}
		while (p < k) {		/* every frame: f0 .. f7, len + 4 bytes */
			if (p + 3 >= k || tx[p] != 0xf0 || p + tx[p + 2] + 4 > k ||
			    tx[p + tx[p + 2] + 3] != 0xf7)
				return -EINVAL;
			p += tx[p + 2] + 4;
		}
		if (!k)
			return -EINVAL;
		spin_lock_irqsave(&g_dbg_chip->lock, flags);
		memcpy(g_dbg_chip->hu_dbg_tx, tx, k);
		g_dbg_chip->hu_dbg_len = k;
		spin_unlock_irqrestore(&g_dbg_chip->lock, flags);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "huframe queued (%d bytes); read again for the reply\n", k);
		return len;
	}
	/*
	 * "hupause <ms>" (up to 30000): no head-unit poll, so no GPIO 0x320
	 * write (each poll sets pin 5, the head unit's power, high), for a
	 * test that powers the head unit off with "w2 320 005".
	 */
	if (!strncmp(buf, "hupause ", 8)) {
		unsigned int ms;

		if (kstrtouint(strim(buf + 8), 0, &ms) || ms > 30000)
			return -EINVAL;
		g_dbg_chip->hu_pause_until = jiffies + msecs_to_jiffies(ms);
		g_dbg_chip->hu_paused = ms > 0;
		dev_info(g_dbg_chip->dev, "head unit: poll paused for %u ms\n", ms);
		scnprintf(g_dbg_result, sizeof(g_dbg_result), "hupause %u ms\n", ms);
		return len;
	}
	if (!strncmp(buf, "hutext", 6)) {
		char t[9] = "";
		unsigned long flags;
		int i, k = 0;

		for (i = 6; buf[i] == ' '; i++)
			;
		for (; buf[i] && buf[i] != '\n' && k < 8; i++) {
			if (buf[i] < 0x20 || buf[i] > 0x7e)
				return -EINVAL;
			t[k++] = buf[i];
		}
		spin_lock_irqsave(&g_dbg_chip->lock, flags);	/* vs. hu_label */
		memcpy(g_dbg_chip->hu_text, t, sizeof(t));
		spin_unlock_irqrestore(&g_dbg_chip->lock, flags);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  k ? "hutext \"%s\"\n" : "hutext cleared\n", t);
		return len;
	}
	/* "hutx <reg> [b0 b1 ...]" — send arbitrary head-unit I2C transaction
	 * for live relay/amp probing. e.g. "hutx 0xb1 0x03"
	 */
	if (!strncmp(buf, "hutx", 4)) {
		unsigned int reg = 0, d[8] = {0};
		u8 data[8];
		int n, i;

		n = sscanf(buf, "%7s %x %x %x %x %x %x %x %x %x", op, &reg,
			   &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6],
			   &d[7]);
		if (n < 2)
			return -EINVAL;
		for (i = 0; i < n - 2; i++)
			data[i] = d[i] & 0xff;
		/* a jack frame by hand: see huframe */
		if ((reg & 0xff) == 0x03 && n > 2 && data[0] == 0x02)
			creative_ae9_hu_amp_hold(g_dbg_chip);
		creative_i2c_raw_tx(g_dbg_chip, reg & 0xff,
				    (n > 2) ? data : NULL, n - 2);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "hutx reg=0x%02x len=%d sent\n", reg & 0xff, n - 2);
		return len;
	}
	/* "chipior <hex_addx>" — ChipIO read */
	/* "xram 0824": one 8051 XRAM byte, read-only, two agreeing reads under
	 * replay_mutex (ae9_8051_xram_byte); for the router state report
	 * (stream reference counts 0x0824/0x07ac/0x0766, 0x0822, 0x00b6) */
	if (!strncmp(buf, "xram ", 5)) {
		unsigned int a;
		int v;

		if (sscanf(buf + 5, "%x", &a) != 1 || a > 0xffff)
			return -EINVAL;
		if (!g_dbg_chip->corb_buf)
			return -ENODEV;
		v = ae9_8051_xram_byte(g_dbg_chip, a);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  v < 0 ? "xram[0x%04x] read failed (%d)\n" : "xram[0x%04x]=0x%02x\n",
			  a, v);
		return len;
	}
	if (!strncmp(buf, "chipior", 7)) {
		unsigned int a, v = 0;
		int ret;

		if (sscanf(buf, "%7s %x", op, &a) != 2)
			return -EINVAL;
		/* serialized with the driver's own ChipIO use: unlocked reads
		 * raced it and returned 0 or another address's value */
		mutex_lock(&g_dbg_chip->replay_mutex);
		creative_dsp_chipio_addr_invalidate();
		ret = creative_dsp_chipio_read(g_dbg_chip, a, &v);
		creative_dsp_chipio_addr_invalidate();
		mutex_unlock(&g_dbg_chip->replay_mutex);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "chipio[0x%x]=0x%x ret=%d\n", a, v, ret);
		return len;
	}
	/* "chipiow <addr> <val>" — write 32-bit value via ChipIO */
	if (!strncmp(buf, "chipiow", 7)) {
		unsigned int a, v;
		int ret;

		if (sscanf(buf, "%7s %x %x", op, &a, &v) != 3)
			return -EINVAL;
		/* serialized with the driver's own ChipIO use, as chipior */
		mutex_lock(&g_dbg_chip->replay_mutex);
		creative_dsp_chipio_addr_invalidate();
		ret = creative_dsp_chipio_write(g_dbg_chip, a, v);
		creative_dsp_chipio_addr_invalidate();
		mutex_unlock(&g_dbg_chip->replay_mutex);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "chipiow[0x%x]=0x%x ret=%d\n", a, v, ret);
		return len;
	}

	/* "corbs <hex32>" — send a verb via CORB (synchronous) */
	if (!strncmp(buf, "corbs", 5)) {
		unsigned int v;
		u32 resp = 0;
		int ret;

		dev_info(g_dbg_chip->dev, "corbs handler: buf=\"%s\" len=%zu\n", buf, len);
		if (sscanf(buf, "%7s %x", op, &v) != 2) {
			dev_info(g_dbg_chip->dev, "corbs sscanf failed (op=\"%s\")\n", op);
			return -EINVAL;
		}
		dev_info(g_dbg_chip->dev, "corbs: v=0x%08x\n", v);
		ret = creative_ae9_corb_send_verb_dbg(g_dbg_chip, v, &resp);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "corbs 0x%08x -> ret=%d resp=0x%08x\n", v, ret, resp);
		return len;
	}

	/* "rawverb <hex32>" — send an arbitrary 32-bit ICW verb */
	if (!strncmp(buf, "rawverb", 7)) {
		unsigned int v;
		u32 resp = 0;
		int ret;

		if (sscanf(buf, "%7s %x", op, &v) != 2)
			return -EINVAL;
		ret = creative_ae9_icw_send_verb(g_dbg_chip, v, &resp);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "rawverb 0x%08x -> ret=%d resp=0x%08x\n", v, ret, resp);
		return len;
	}

	/* "icwverb <cad> <nid> <verbid> <payload>" — send one verb through the
	 * immediate command interface (the one-argument "verb <hex>" above goes
	 * through the CORB; this used to be "verb" too and could never run).
	 * Once the command rings run, the immediate interface stays busy
	 * (IRS 0x5): useful only before they start, otherwise it just times out. */
	if (!strncmp(buf, "icwverb", 7)) {
		unsigned int cad, nid, vid, pl;
		u32 resp = 0;
		int ret;

		if (sscanf(buf, "%7s %x %x %x %x", op, &cad, &nid, &vid, &pl) != 5)
			return -EINVAL;
		ret = creative_ae9_icw_send_verb(g_dbg_chip,
			creative_ae9_verb(cad, nid, vid, pl), &resp);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "icwverb cad=%x nid=%x v=%x p=%x -> ret=%d resp=0x%08x\n",
			  cad, nid, vid, pl, ret, resp);
		return len;
	}

	n = sscanf(buf, "%7s %x %x", op, &off, &val);
	if (n < 2)
		return -EINVAL;

	if (off >= 0x4000)
		return -EINVAL;

	if (!strcmp(op, "w2") && n == 3) {
		creative_ext_writel(g_dbg_chip, off, val);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "w2 0x%x <= 0x%x (rb 0x%x)\n", off, val,
			  creative_ext_readl(g_dbg_chip, off));
	} else if (!strcmp(op, "r2")) {
		val = creative_ext_readl(g_dbg_chip, off);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "r2 0x%x = 0x%x\n", off, val);
	} else if (!strcmp(op, "w0") && n == 3) {
		creative_hda_writel(g_dbg_chip, off, val);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "w0 0x%x <= 0x%x (rb 0x%x)\n", off, val,
			  creative_hda_readl(g_dbg_chip, off));
	} else if (!strcmp(op, "r0")) {
		val = creative_hda_readl(g_dbg_chip, off);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "r0 0x%x = 0x%x\n", off, val);
	} else {
		return -EINVAL;
	}

	return len;
}

static ssize_t creative_dbg_read(struct file *f, char __user *ubuf,
				 size_t len, loff_t *ppos)
{
	return simple_read_from_buffer(ubuf, len, ppos,
				       g_dbg_result, strlen(g_dbg_result));
}

static const struct file_operations creative_dbg_fops = {
	.owner = THIS_MODULE,
	.write = creative_dbg_write,
	.read  = creative_dbg_read,
};

/*
 * debugfs "state": read-only snapshot for bug reports (ae9-full-dump).
 * First what the driver believes (no card access), then what the card
 * says: router stream reference counts, router queue, the Direct Mode
 * slot list and front router slots, the ChipIO flag word. Only reads;
 * each card read holds replay_mutex like the driver's own.
 */
/*
 * Every head-unit frame's outcome (creative_spi_frame_win), for debugfs
 * state: counted only, no register access and no log line. The poll's
 * occasional "1/15 poll frames unanswered" follows host load (2026-11-18,
 * project notes): most likely Windows' echo read after the f7 taking the
 * reply's f0 when we were held up. A miss record that says so: why "no f0",
 * the first skipped byte = the frame's command, the echo = f0.
 */
void creative_ae9_hu_frame_note(struct creative_chip *chip, int ret, u8 cmd, u8 why,
				u32 a, u32 b)
{
	unsigned int k;

	chip->hu_frames++;
	if (ret >= 0)
		return;
	if (why < ARRAY_SIZE(chip->hu_misses))
		chip->hu_misses[why]++;
	k = chip->hu_miss_pos++ % ARRAY_SIZE(chip->hu_miss_ring);
	chip->hu_miss_ring[k].at = jiffies ?: 1;
	chip->hu_miss_ring[k].cmd = cmd;
	chip->hu_miss_ring[k].why = why;
	chip->hu_miss_ring[k].a = a;
	chip->hu_miss_ring[k].b = b;
}

/* the DACs' I2C status counters (dsp.c ca0113_status(); no card access) */
static void creative_ae9_dac_i2c_show(struct seq_file *m, struct creative_chip *chip)
{
	const struct ae9_dac_i2c *s = &chip->dac_i2c;

	seq_printf(m, "DAC I2C: 0x48 %u/%u not acknowledged, 0x49 %u/%u, 0x30 %u/%u, other %u/%u; after the bring-up %u; retried %u (got through %u); unfinished %u; card off the bus %u; reads not acknowledged %u",
		   s->nack[0], s->cmds[0], s->nack[1], s->cmds[1], s->nack[2], s->cmds[2],
		   s->nack[3], s->cmds[3], s->late, s->retried, s->recovered, s->unfinished,
		   s->gone, s->read_bad);
	if (s->last_at && s->last_rd)
		seq_printf(m, "; last NACK read %02x/%02x %lu s ago",
			   s->last_grp, s->last_reg, (jiffies - s->last_at) / HZ);
	else if (s->last_at)
		seq_printf(m, "; last NACK write %02x/%02x = 0x%02x %lu s ago",
			   s->last_grp, s->last_reg, s->last_val, (jiffies - s->last_at) / HZ);
	if (s->last_at && s->last_src)
		seq_printf(m, ", 0x880 = 0x%x", s->last_src);
	if (s->abrt_src)
		seq_puts(m, "; dacabrt on");
	seq_putc(m, '\n');
}

static void creative_ae9_hu_miss_show(struct seq_file *m, struct creative_chip *chip)
{
	static const char *const why[] = { "transmit FIFO full", "no reply", "no f0", "short" };
	unsigned int i, n = min_t(unsigned int, chip->hu_miss_pos, ARRAY_SIZE(chip->hu_miss_ring));

	seq_printf(m, "head unit frames: %llu sent, unanswered: %u no reply, %u no f0, %u short, %u transmit FIFO full\n",
		   chip->hu_frames, chip->hu_misses[AE9_HU_MISS_NONE], chip->hu_misses[AE9_HU_MISS_NOF0],
		   chip->hu_misses[AE9_HU_MISS_SHORT], chip->hu_misses[AE9_HU_MISS_TXFULL]);
	for (i = 0; i < n; i++) {
		unsigned int k = (chip->hu_miss_pos - 1 - i) % ARRAY_SIZE(chip->hu_miss_ring);
		u32 a = chip->hu_miss_ring[k].a, b = chip->hu_miss_ring[k].b;

		if (chip->hu_miss_ring[k].why == AE9_HU_MISS_TXFULL)
			seq_printf(m, "  %lu s ago: frame %02x, %s after %u bytes, status 0x%02x\n",
				   (jiffies - chip->hu_miss_ring[k].at) / HZ, chip->hu_miss_ring[k].cmd,
				   why[0], a, b & 0xff);
		else
			seq_printf(m, "  %lu s ago: frame %02x, %s: %u bytes kept, %u skipped (first %02x), echo after f7 %02x, status %02x%s\n",
				   (jiffies - chip->hu_miss_ring[k].at) / HZ, chip->hu_miss_ring[k].cmd,
				   why[min_t(u8, chip->hu_miss_ring[k].why, 3)], a & 0xff, (a >> 8) & 0xff,
				   (a >> 16) & 0xff, a >> 24, b & 0xff,
				   b >> 8 ? ", line error" : "");
	}
}

/*
 * The bridge's discard timer (both bridges' manuals: Tsi381 p154 and 14.9.12,
 * ASM1083 p20 and p37): the card's delayed read whose completion
 * the bridge threw away because the card didn't come back for it in time
 * (Tsi381: 2^15 PCI clocks, ~1 ms). Bridge Control (0x3e) bit 10,
 * DISCARD_STAT, write-1-to-clear; and in a PCIe-to-PCI bridge's AER
 * capability the secondary uncorrectable error status (+0x2c), bit 10
 * "Delayed Transaction Discard Timer Expired" (ASM1083 0x22c, Tsi381
 * 0x12c). An expired discard timer means a DMA read of the card's that
 * never got its data.
 */
#define AE9_BRIDGE_CTL_DISCARD_STAT	0x0400
#define AE9_AER_SEC_UNCOR_STATUS	0x2c
#define AE9_AER_SEC_DISCARD		BIT(10)

/* the bridge's AER secondary uncorrectable status; false without one */
static bool ae9_bridge_aer_sec(struct pci_dev *br, u32 *val)
{
	int pos;

	if (!br || !pci_is_pcie(br) || pci_pcie_type(br) != PCI_EXP_TYPE_PCI_BRIDGE)
		return false;
	pos = pci_find_ext_capability(br, PCI_EXT_CAP_ID_ERR);
	if (!pos)
		return false;
	return !pci_read_config_dword(br, pos + AE9_AER_SEC_UNCOR_STATUS, val);
}

/* that status's bits (PCI Express to PCI/PCI-X Bridge spec, secondary
 * uncorrectable error status; bit 4 reserved) */
static const char *const ae9_aer_sec_names[] = {
	"target abort on a split completion", "master abort on a split completion",
	"received target abort", "received master abort", NULL,
	"unexpected split completion", "split completion data error",
	"data error", "attribute error", "address error", "discard timer expired",
	"PERR#", "SERR#", "internal bridge error",
};

/* @val's set bits by name into @buf, ", " between them */
static void ae9_aer_sec_decode(u32 val, char *buf, size_t len)
{
	unsigned int b;
	size_t n = 0;

	buf[0] = '\0';
	if (val == U32_MAX) {
		strscpy(buf, "all ones: the bridge doesn't answer", len);
		return;
	}
	for (b = 0; b < 32; b++) {
		if (!(val & BIT(b)))
			continue;
		if (b < ARRAY_SIZE(ae9_aer_sec_names) && ae9_aer_sec_names[b])
			n += scnprintf(buf + n, len - n, "%s%s", n ? ", " : "",
				       ae9_aer_sec_names[b]);
		else
			n += scnprintf(buf + n, len - n, "%sbit %u", n ? ", " : "", b);
	}
}

/*
 * The PCIe-to-PCI bridge on the card, by its PCI ID: a Tundra Tsi381
 * (10e3:8111; Tundra was bought by IDT, so "the IDT bridge" in older notes)
 * or an ASMedia ASM1083/1085 (1b21:1080).
 */
static const char *ae9_bridge_name(struct creative_chip *chip)
{
	struct pci_dev *br = chip->pci->bus->self;

	if (chip->asmedia)
		return "ASMedia ASM1083/1085";
	if (br && br->vendor == 0x10e3 && br->device == 0x8111)
		return "Tundra Tsi381";
	return "unknown";
}

static int creative_dbg_state_show(struct seq_file *m, void *v)
{
	struct creative_chip *chip = g_dbg_chip;
	static const u32 slots[] = { 0x190060, 0x190064, 0x190068, 0x19006c,
				     0x190070, 0x190074 };
	u32 flags = 0, val;
	int i, x;

	if (!chip)
		return -ENODEV;

	seq_printf(m, "driver %s, %s bridge (%04x:%04x)\n", CREATIVE_AE9_DRV_VERSION,
		   ae9_bridge_name(chip),
		   chip->pci->bus->self ? chip->pci->bus->self->vendor : 0,
		   chip->pci->bus->self ? chip->pci->bus->self->device : 0);
	/* the bridge's discard timer (config space only; the bring-up's log
	 * lines clear DISCARD_STAT and count it here) */
	if (chip->pci->bus->self) {
		struct pci_dev *br = chip->pci->bus->self;
		u16 ctl = 0;

		pci_read_config_word(br, PCI_BRIDGE_CONTROL, &ctl);
		seq_printf(m, "bridge discard timer: control 0x%04x (expired: %s), seen by %u bring-up log line(s)",
			   ctl, ctl & AE9_BRIDGE_CTL_DISCARD_STAT ? "YES" : "no", chip->br_discards);
		if (ae9_bridge_aer_sec(br, &val)) {
			char bits[160];

			/* bit 10 has its own words below */
			ae9_aer_sec_decode(val == U32_MAX ? val : val & ~AE9_AER_SEC_DISCARD,
					   bits, sizeof(bits));
			seq_printf(m, "; AER secondary uncorrectable 0x%08x (%s%sdiscard timer expired: %s)\n",
				   val, bits, bits[0] ? "; " : "",
				   val != U32_MAX && (val & AE9_AER_SEC_DISCARD) ? "YES" : "no");
		} else {
			seq_puts(m, "; no AER capability\n");
		}
	}
	seq_printf(m, "BAR2 IDs at the bring-up: 0x8fc=0x%08x (DAC I2C, want 0x44570140), 0xcfc=0x%08x (UART, 0x44570110), 0x4=0x%08x (board, revision 0x%02x)\n",
		   chip->bar2_id[0], chip->bar2_id[1], chip->bar2_id[2], chip->bar2_id[2] & 0xff);
	AE9_CALL(chip, state_show, m, chip);
	/* fx.direct and dsp_active are the AE-9's Direct Mode and DSP-mode
	 * routing; a board without Direct Mode (the AE-7) plays in
	 * DSP mode alone and never reads them */
	seq_printf(m, "mode: %s, SBX %s, DSP routing %s, primed %d, halted %d, handshake %s\n",
		   ae9_has(chip, AE9_CAP_DIRECT) && chip->fx.direct ? "Direct" : "DSP",
		   chip->fx.sbx ? "on" : "off",
		   !ae9_has(chip, AE9_CAP_DIRECT) ? "n/a" :
		   chip->dsp_active ? "up" : "down", chip->dsp_primed,
		   chip->dsp_halted, chip->hs_ok ? "ok" : "not ok");
	/* playback: the link format the open stream was prepared with and
	 * its sample bits (latched at open); native DSD keeps pb_format 0 (no
	 * optical mirror of it) and has its own link format */
	seq_puts(m, "streams: playback ");
	if (chip->dsd_active)
		seq_printf(m, "DSD64 (link fmt 0x%04x, 6 x 16 bit carrying DSD), ",
			   chip->pcm_start_fmt);
	else if (chip->pb_format)
		seq_printf(m, "fmt 0x%04x (%u bit), ", chip->pb_format, chip->pcm_bits);
	else
		seq_puts(m, "fmt 0x0000 (none), ");
	seq_printf(m, "optical out fmt 0x%04x, "
		   "mic %s, optical in %s, What U Hear %s, rear RCA %s\n",
		   chip->opt_format,
		   chip->capture_substream ? "open" : "-",
		   chip->xcap[0].substream ? "open" : "-",
		   chip->xcap[1].substream ? "open" : "-",
		   chip->xcap[2].substream ? "open" : "-");
	seq_printf(m, "start: rate %u fmt 0x%04x, DAC key 0x%x, stream 0x05 channels %u\n",
		   chip->pcm_start_rate, chip->pcm_start_fmt, chip->dac_start_key,
		   chip->in05_ch);
	creative_ae9_pcm_fifo_show(m, chip);	/* pcm.c: SDnFIFOS */
	seq_printf(m, "output: %s, headphone profile %u (DSP holds %u), Voice Clarity %s, monitor %s\n",
		   chip->hu_output ? "headphones" : "speakers", chip->fx.hp_profile,
		   chip->fx.hp_loaded, chip->cv.on ? "on" : "off",
		   chip->cv.monitor ? "on" : "off");
	seq_printf(m, "counters: verb timeouts %u, DSP words full %u / not sent %u, "
		   "DSP fails %u%s, profile tries %u, DMA leaks %u\n",
		   chip->corb_timeouts, chip->scp_full_words, chip->scp_busy_words,
		   chip->fx.fails, chip->fx.dead ? " (DSP not answering)" : "",
		   chip->hp_tries, chip->hp_leaks);
	if (chip->card_hung)
		seq_printf(m, "card: STOPPED ANSWERING (last verb 0x%08x); reload the driver\n",
			   chip->hung_verb);
	seq_printf(m, "DSP output unmute: resent %u, stayed muted %u\n",
		   chip->unmute_resends, chip->unmute_stuck);
	creative_ae9_dac_i2c_show(m, chip);
	seq_printf(m, "RIRB entries that landed late: %u (longest wait %u us), never landed %u, other codec %u, unreadable write pointer %u\n",
		   chip->rirb_late, chip->rirb_late_max_us, chip->rirb_never, chip->rirb_stray,
		   chip->rirb_badwp);
	seq_printf(m, "RIRB given-up slots: %u landed later (%u the answer waited for), %u never did, %u watched now, %u injected (%s)\n",
		   chip->rirb_wlanded, chip->rirb_wanswer, chip->rirb_wexpired,
		   chip->rirb_nwatch, chip->rirb_injected, chip->rirb_held >= 0 ? "one held" : "none held");
	seq_printf(m, "BDLs kept (stream didn't stop): %d\n", atomic_read(&chip->bdl_leaks));
	seq_printf(m, "optical in: %s; since load: started without a signal %u, signal came in %u, signal lost %u, stream stopped moving %u\n",
		   !chip->xcap[0].substream ? "closed" :
		   chip->xcap[0].no_signal ? "recording silence (no signal)" : "recording the signal",
		   chip->xcap[0].n_nosig, chip->xcap[0].n_came, chip->xcap[0].n_lost,
		   chip->xcap[0].n_stopped);
	seq_printf(m, "router chip param verbs: %u waited for it to be idle, %u refused busy and resent, %u lost, %u busy waits that never cleared\n",
		   chip->r51_waited, chip->r51_refused, chip->r51_lost, chip->r51_never_idle);
	seq_printf(m, "router chip values refused (another task's select came between): %u\n",
		   chip->r51_crossed);
	seq_printf(m, "head unit: set up again %u time(s)%s\n", chip->hu_reattaches,
		   chip->hu_ae9_hold ? ", AE-9 shown" : "");
	seq_printf(m, "head unit boot frames: %u, without reply %u",
		   chip->hu_boot_frames, chip->hu_noreply_n);
	for (i = 0; i < min_t(int, chip->hu_noreply_n, ARRAY_SIZE(chip->hu_noreply_idx)); i++)
		seq_printf(m, "%s#%u (%02x %02x)", i ? ", " : ": ",
			   chip->hu_noreply_idx[i], chip->hu_noreply_head[i] >> 8,
			   chip->hu_noreply_head[i] & 0xff);
	seq_printf(m, "%s\n", chip->hu_noreply_n > ARRAY_SIZE(chip->hu_noreply_idx) ? ", ..." : "");
	creative_ae9_hu_miss_show(m, chip);

	if (chip->suspended || chip->card_gone || chip->card_hung || !chip->corb_buf) {
		seq_puts(m, "card: not readable now (suspended, gone or not up)\n");
		return 0;
	}

	/* BAR2 GPIO: 0x300 = the pins' real levels, 0x304 = output enable,
	 * 0x320 = the last write (pin in the low nibble, bit 8 = value).
	 * Pin 0 is probably the DAC reset/enable (DeskWork.md A). */
	val = creative_ext_readl(chip, 0x300);
	seq_printf(m, "GPIO: pin levels 0x300=0x%02x (pin 0 %s), outputs 0x304=0x%02x, last write 0x320=0x%03x\n",
		   val & 0xff, val & 1 ? "high" : "LOW", readb(chip->ext_regs + 0x304),
		   creative_ext_readw(chip, 0x320));
	/* native DSD64 (pcm.c, pcm_dsd): on or off; BAR2 0x100/0x104, the
	 * CA0113 I2S wrapper it uses (0x100 reads 0x00000700 while PCM plays;
	 * Windows' DSD predicts 0x6000c7cc and 0x104 bytes 2-3 = 0x12 0x10) */
	seq_printf(m, "DSD64 active: %s, BAR2 0x100=0x%08x 0x104=0x%08x\n",
		   !chip->dsd_active ? "no" : chip->dsd_running ? "yes (stream running)" :
		   "yes (stream stopped)",
		   creative_ext_readl(chip, 0x100), creative_ext_readl(chip, 0x104));

	seq_puts(m, "router stream references:");
	for (i = 0; i < ARRAY_SIZE(ae9_drain_ids); i++) {
		x = ae9_8051_xram_byte(chip, AE9_XRAM_STREAM_REFS(ae9_drain_ids[i]));
		if (x < 0)
			seq_printf(m, " 0x%02x=?", ae9_drain_ids[i]);
		else
			seq_printf(m, " 0x%02x=%d", ae9_drain_ids[i], x);
	}
	mutex_lock(&chip->replay_mutex);
	x = ae9_8051_queue_depth(chip);
	mutex_unlock(&chip->replay_mutex);
	seq_printf(m, "\nrouter queue depth: %d\n", x);

	x = ae9_8051_xram_byte(chip, 0x157d);
	seq_printf(m, "router slot list head (XRAM 0x157d): ");
	seq_printf(m, x < 0 ? "?\n" : "0x%02x\n", x);
	/* stream 0x18 (DSP outputs -> DACs, the DSP route walk): refs, head, lanes */
	{
		int refs = ae9_8051_xram_byte(chip, AE9_XRAM_STREAM18_REFS);
		int s = ae9_8051_xram_byte(chip, AE9_XRAM_STREAM18_HEAD);
		int g6 = ae9_8051_xram_byte(chip, 0x1106), g7 = ae9_8051_xram_byte(chip, 0x1107);
		int c0 = ae9_8051_xram_byte(chip, 0x1058), c1 = ae9_8051_xram_byte(chip, 0x1059);
		unsigned int k;

		seq_printf(m, "router stream 0x18: refs %d, head ", refs);
		seq_printf(m, s < 0 ? "?" : "0x%02x", s);
		seq_printf(m, ", DAC groups 0x1106=%d 0x1107=", g6);
		seq_printf(m, g7 < 0 ? "?" : "0x%02x", g7);
		seq_puts(m, ", cfg 0xd0=");
		seq_printf(m, c0 < 0 ? "?" : "0x%02x", c0);
		seq_puts(m, " 0xd1=");
		seq_printf(m, c1 < 0 ? "?" : "0x%02x", c1);
		seq_puts(m, "; lanes:");
		for (k = 0; k < 2 * AE9_DSP_LANES && s >= AE9_RT_FIRST_SLOT && s < AE9_RT_ENTRIES; k++) {
			u32 v = 0;
			int ret;

			mutex_lock(&chip->replay_mutex);
			creative_dsp_chipio_addr_invalidate();
			ret = creative_dsp_chipio_read(chip, 0x190000 + 4 * s, &v);
			creative_dsp_chipio_addr_invalidate();
			mutex_unlock(&chip->replay_mutex);
			if (ret < 0) {
				seq_printf(m, " %02x:?", s);
				break;
			}
			seq_printf(m, " %02x:%02x->%02x%s", s, v & 0xff, (v >> 8) & 0xff,
				   (v >> 16) & 3 ? "" : "(off)");
			s = ae9_8051_xram_byte(chip, AE9_XRAM_SLOT_NEXT + s);
		}
		seq_puts(m, "\n");
	}
	seq_puts(m, "router slots:");
	for (i = 0; i < ARRAY_SIZE(slots); i++) {
		int ret;

		val = 0;
		mutex_lock(&chip->replay_mutex);
		creative_dsp_chipio_addr_invalidate();
		ret = creative_dsp_chipio_read(chip, slots[i], &val);
		creative_dsp_chipio_addr_invalidate();
		mutex_unlock(&chip->replay_mutex);
		if (ret < 0)
			seq_printf(m, " %06x=?", slots[i]);
		else
			seq_printf(m, " %06x=0x%x", slots[i], val);
	}

	/* Active router entries into the DAC ports (dst 0x40-0x7f). Two
	 * feeds on one port, or a DSP output (0xc0-0xdf) feeding one in
	 * Direct Mode, is the 2026-11-11 stuck state: a DSP route moved to a
	 * free slot (ae9_rt_put) and never switched off; the DAC then plays
	 * gated, metallic audio until a power-off clears the router. */
	{
		u8 cnt[0x40] = { 0 };
		unsigned int unsteady = 0, bad = 0, feeds = 0, nsrc = 0, nfree = 0;
		u16 srcs[16];

		seq_puts(m, "\nDAC port feeds:");
		mutex_lock(&chip->replay_mutex);
		creative_dsp_chipio_addr_invalidate();
		for (i = 0; i < AE9_RT_ENTRIES; i++) {
			u8 src, dst;

			if (ae9_chipio_read_stable(chip, 0x190000 + 4 * i, &val, &unsteady) < 0) {
				seq_puts(m, " (read failed)");
				break;
			}
			src = val & 0xff;
			dst = (val >> 8) & 0xff;
			if (ae9_rt_slot_takeable(i, val))
				nfree++;
			if (!((val >> 16) & 3))
				continue;
			/* a host slot into a router SRC twice (Windows' Direct
			 * start has one, 00 -> 90 / 01 -> 80; a second one left
			 * over gave the gated sound, uv26 2026-11-11) */
			if (src < 0x20 && dst >= 0x80 && dst < 0xc0) {
				u16 r16 = val & 0xffff;
				unsigned int k;

				for (k = 0; k < nsrc && srcs[k] != r16; k++)
					;
				if (k < nsrc) {
					seq_printf(m, " %06x=%02x->%02x(SRC, twice)", 0x190000 + 4 * i, src, dst);
					feeds++;
					bad++;
				} else if (nsrc < ARRAY_SIZE(srcs)) {
					srcs[nsrc++] = r16;
				}
				continue;
			}
			if (dst < 0x40 || dst > 0x7f)
				continue;
			cnt[dst - 0x40]++;
			feeds++;
			seq_printf(m, " %06x=%02x->%02x", 0x190000 + 4 * i, src, dst);
			if (ae9_has(chip, AE9_CAP_DIRECT) && chip->fx.direct &&
			    (src & 0xe0) == 0xc0)
				bad++;
		}
		creative_dsp_chipio_addr_invalidate();
		mutex_unlock(&chip->replay_mutex);
		for (i = 0; i < ARRAY_SIZE(cnt); i++)
			if (cnt[i] > 1)
				bad++;
		/* no host feed into a DAC port is right for two kinds of
		 * playback: say which, from the driver's own state */
		if (!feeds && chip->dsd_active)
			seq_puts(m, " none (native DSD64: the I2S wrapper feeds the DAC, no router)");
		else if (!feeds && chip->pb_format && ae9_has(chip, AE9_CAP_DIRECT) &&
			 chip->fx.direct && chip->pcm_start_rate % 11025 == 0)
			seq_printf(m, " none (%u Hz: the 44.1 kHz family plays through the S/PDIF transmitter, not a DAC port)",
				   chip->pcm_start_rate);
		else if (!feeds)
			seq_puts(m, " none");
		seq_puts(m, bad ? "  STALE: a port has two feeds, a route is on twice, or a DSP feed in Direct Mode (power-off clears it)" : "  ok");
		seq_printf(m, "\nrouter free slots: %d in no router-chip list (%u by the old rule)",
			   ae9_rt_true_free(chip, true), nfree);
	}

	/* codec 1 node 0x15 GET 0xf0f: bit N = ChipIO control flag N
	 * (Windows reads 0x01f31fcb on every boot, note 63f) */
	mutex_lock(&chip->replay_mutex);
	x = creative_ae9_corb_send_verb_sync(chip, 0x115f0f00, &flags);
	mutex_unlock(&chip->replay_mutex);
	if (x < 0)
		seq_puts(m, "\nchip flag word: ?\n");
	else
		seq_printf(m, "\nchip flag word: 0x%08x%s\n", flags,
			   flags == 0x01f31fcb ? " (as Windows)" : "");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(creative_dbg_state);

/* ------------------------------------------------------------------ */
/* Delayed ACM head-unit re-init					  */
/* The head unit's microcontroller is not ready at early boot (~2s),  */
/* so the initial ACM I2C init doesn't light the display. Re-run it    */
/* a few times with delays until the head unit responds.	  */
/* ------------------------------------------------------------------ */
static void creative_ae9_acm_work(struct work_struct *work)
{
	struct creative_chip *chip =
		container_of(work, struct creative_chip, acm_work.work);

	if (READ_ONCE(chip->unloading))
		return;

	/*
	 * The head unit sets itself up from its own MCU firmware; Windows
	 * sends it no I2C init (both MMIO captures), and ours disrupted it.
	 * Windows' post-boot replay (creative_ae9_post_dsp_setup) did the
	 * rest, so only the heartbeat runs here, without the GPIO4 toggle.
	 */
	dev_info(chip->dev, "ACM work: post_windows, heartbeat only (no GPIO4 toggle)\n");
	if (acm_heartbeat_ms) {
		queue_delayed_work(chip->wq, &chip->hb_work,
				      msecs_to_jiffies(acm_heartbeat_ms));
	}
}

static bool hu_auto_switch;
module_param(hu_auto_switch, bool, 0444);
MODULE_PARM_DESC(hu_auto_switch, "Start with \"Head Unit Auto Switch\" on: plugging into the head unit's 3.5 mm headphone jack switches to headphones, unplugging back to speakers (default off: plugs only log)");

#define HU_PLUG_SETTLE_MS	1500	/* plug must hold still this long */
#define HU_AMP_SETTLE_MS	1000	/* amp bit must disagree this long (2+ full polls) */
#define HU_AMP_TRIES		3	/* jack frame resends before giving up */
#define HU_VOL_RESENDS		3	/* volume lists the DACs refused: resends */

unsigned int ae9_debug;
module_param_named(debug, ae9_debug, uint, 0644);
MODULE_PARM_DESC(debug, "Log detail: 0 startup, actions and problems (default), 1 also routine per-stream/per-step lines, 2 also raw head-unit button state");

/*
 * Knob/button poll interval between Windows' full 500 ms polls (the
 * hu_fast_ms option until 0.12.12).
 */
#define HU_FAST_MS		40U

#define HU_ATT_DEFAULT	0x48	/* Windows' boot volume, -36.0 dB */
#define HU_SP_TICKS	10	/* Windows shows "-SP-" ~5 s, then the volume */

/*
 * Volume = the same attenuation on six registers of the card's DACs
 * (front 0x48 ES9038Q2M, rear 0x49 SABRE9006A), in the
 * order Windows writes them on a volume change; 0xff mutes.
 *
 * 0.12.16: a command the DACs didn't acknowledge (I2C NACK) was already
 * sent once more (dsp.c); still refused, the heartbeat sends the volume
 * again, up to HU_VOL_RESENDS times (creative_ae9_hu_vol_resend()), then
 * warns and leaves it until a volume list gets through. (After a switch
 * to headphones that would leave the speakers' louder attenuation; turning
 * the head unit's headphone amp off over the UART then is possible but
 * not done: not agreed yet.)
 */
static void creative_ae9_hu_apply_volume(struct creative_chip *chip)
{
	static const u8 regs[6][2] = {
		{ 0x48, 0x0f }, { 0x48, 0x10 }, { 0x49, 0x04 },
		{ 0x49, 0x05 }, { 0x49, 0x02 }, { 0x49, 0x03 },
	};
	u8 att = chip->hu_mute ? 0xff : chip->hu_att;
	u8 cmds[6][3];
	int i;

	for (i = 0; i < 6; i++) {
		cmds[i][0] = regs[i][0];
		cmds[i][1] = regs[i][1];
		cmds[i][2] = att;
	}
	if (!creative_dsp_ca0113_list_quiet(chip, cmds, 6))
		chip->hu_vol_fails = 0;
	else if (chip->hu_vol_fails <= HU_VOL_RESENDS && ++chip->hu_vol_fails > HU_VOL_RESENDS)
		dev_warn(chip->dev, "head unit: the DACs did not acknowledge volume att 0x%02x (I2C NACK), %u resends; left until a volume change gets through\n",
			 att, HU_VOL_RESENDS);
	/* mute changes always; each volume step only with debug=1 */
	if (chip->hu_mute != chip->hu_mute_logged) {
		chip->hu_mute_logged = chip->hu_mute;
		dev_info(chip->dev, "head unit: %s\n", chip->hu_mute ? "muted" : "unmuted");
	}
	ae9_dbg(chip, "head unit: volume att 0x%02x (%s)\n", att,
		 chip->hu_mute ? "muted" : "on");
}

/* the heartbeat: the last volume list refused, sent again (above); not to a
 * card that is gone or stopped answering (not a NACK: nothing to resend) */
static void creative_ae9_hu_vol_resend(struct creative_chip *chip)
{
	if (!chip->hu_vol_fails || chip->hu_vol_fails > HU_VOL_RESENDS || creative_ae9_dead(chip))
		return;
	ae9_dbg(chip, "head unit: volume not acknowledged by the DACs: sent again (%u of %u)\n",
		chip->hu_vol_fails, HU_VOL_RESENDS);
	creative_ae9_hu_apply_volume(chip);
}

/*
 * Headphone amp safety net (0.12.13). Status bit 6 reads back the head
 * unit's register 2 bit 6, which our jack frame f0 03 03 02 <40/00> 40 f7
 * sets: on for headphones on the head unit's jack, off for speakers and for
 * Line Out Virtualization. In the Windows captures it never disagreed with
 * the last frame (91423 status reads, plugs and phantom power included);
 * it did once with us, from a set-up built from stale state (uv56: status
 * 0x95, headphones without their amp). If it disagrees for over 1 s of
 * status reads and two full polls since whose attach-marker checks both
 * read attached, the heartbeat sends the jack frame again
 * (creative_ae9_hu_update).
 * Not while the head unit is unattached (its status then isn't ours: the
 * full poll's marker check and the re-attach decide), not on the first read
 * after a (re-)set-up; 3 resends without it agreeing, then left alone until
 * it agrees again.
 */
static void creative_ae9_hu_amp_track(struct creative_chip *chip, u8 st)
{
	bool want = chip->hu_output && !chip->lo_virt_phys;

	if (!chip->hu_status_valid || chip->hu_unattached || chip->hu_reattach_try ||
	    !!(st & 0x40) == want) {
		chip->hu_amp_since = 0;
		if (chip->hu_status_valid && !!(st & 0x40) == want)
			chip->hu_amp_try = 0;
		return;
	}
	if (!chip->hu_amp_since) {
		chip->hu_amp_since = jiffies | 1;	/* 0 = agrees */
		chip->hu_amp_tick = chip->hu_ticks;
		return;
	}
	/* 1 s, and two full polls since whose marker checks read attached */
	if (time_before(jiffies, chip->hu_amp_since + msecs_to_jiffies(HU_AMP_SETTLE_MS)) ||
	    (int)(chip->hu_att_tick - chip->hu_amp_tick) < 2)
		return;
	chip->hu_amp_since = 0;			/* a new 1 s after a resend */
	if (chip->hu_amp_try < HU_AMP_TRIES) {
		chip->hu_amp_fix = true;
	} else if (chip->hu_amp_try == HU_AMP_TRIES) {
		chip->hu_amp_try++;		/* said once */
		dev_warn(chip->dev, "head unit: headphone amp still reads %s after %u jack frame resends; leaving it until it agrees again\n",
			 want ? "off" : "on", HU_AMP_TRIES);
	}
}

/* a jack frame sent by hand (debugfs): no resends until the amp agrees again */
static void creative_ae9_hu_amp_hold(struct creative_chip *chip)
{
	chip->hu_amp_since = 0;
	chip->hu_amp_fix = false;
	chip->hu_amp_try = HU_AMP_TRIES + 1;	/* as after the warning, without it */
}

/* the jack frame again for the current output (see creative_ae9_hu_amp_track) */
static void creative_ae9_hu_amp_fix(struct creative_chip *chip)
{
	bool want = chip->hu_output && !chip->lo_virt_phys;
	u8 f[7] = { 0xf0, 0x03, 0x03, 0x02, want ? 0x40 : 0x00, 0x40, 0xf7 }, resp[24];

	chip->hu_amp_fix = false;
	chip->hu_amp_try++;
	creative_spi_frame_win(chip, f, sizeof(f), resp, sizeof(resp));
	dev_info(chip->dev, "head unit: headphone amp read %s for over %u ms on %s: jack frame sent again (%u of %u)\n",
		 want ? "off" : "on", HU_AMP_SETTLE_MS,
		 want ? "headphones" : chip->hu_output ?
		 "headphones on the card's RCA out" : "speakers",
		 chip->hu_amp_try, HU_AMP_TRIES);
}

/* Knob turn: c2 reply f0 c2 04 <dir> <steps> .., dir 0 = clockwise, ~2
 * steps per click. Button: bit 7 of the 83 01 02 reply's status byte reads
 * 0 while held (b5 -> 35). Plugs, same byte, 0 = plugged in (checked on the
 * card 2026-11-03): bit 5 = 3.5 mm headphone jack, bit 4 = 3.5 mm mic jack.
 * Bit 3 = SBX button, 1 while held; bit 1 = SBX light, 1 while lit (our
 * f0 22 02 01 <on> echoed back) (2026-11-04).
 * The 1/4" (6.3 mm) headphone jack and the XLR/TRS jack are not reported
 * (checked 2026-11-06: bit 4 follows the 3.5 mm jack only). Plug changes
 * are logged; the output only follows the 3.5 mm headphone plug with
 * "Head Unit Auto Switch" on (off by default). Returns the
 * signed step count. */
static int creative_ae9_hu_parse(struct creative_chip *chip, const u8 *r,
				 int n, int *press_ms)
{
	if (r[1] == 0xc2 && n >= 6 && r[4])
		return r[3] ? -r[4] : r[4];
	if (r[1] == 0x83 && n >= 6 && r[3] == 0x02) {
		bool down = !(r[4] & 0x80);

		if (ae9_debug >= 2 && (!chip->hu_status_valid ||
				 r[4] != (chip->hu_status | (chip->hu_btn_down ? 0 : 0x80))))
			dev_info(chip->dev, "hu_debug: 83 02 status 0x%02x\n", r[4]);
		u8 st = r[4] & 0x7f;
		u8 chg = chip->hu_status_valid ? st ^ chip->hu_status : 0x31;

		if (chg & 0x20)
			dev_info(chip->dev, "head unit: 3.5 mm headphone plug %s\n",
				 (st & 0x20) ? "out" : "in");
		if (chg & 0x10)
			dev_info(chip->dev, "head unit: 3.5 mm mic plug %s\n",
				 (st & 0x10) ? "out" : "in");
		/* headphone plug change: the auto switch (when on) acts on it
		 * once it has held still. Not on the first read at bring-up. */
		if ((chg & 0x20) && chip->hu_status_valid) {
			chip->hu_plug_pending = true;
			chip->hu_plug_at = jiffies;
		}
		/* tell the plug controls' readers */
		if ((chg & 0x20) && chip->hu_plug_ctl[0])
			snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &chip->hu_plug_ctl[0]->id);
		if ((chg & 0x10) && chip->hu_plug_ctl[1])
			snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &chip->hu_plug_ctl[1]->id);
		/* bit 0 clears while phantom power is on: the head unit's own
		 * button, no host command (WINDOWS-CAPTURE-ANALYSIS §29) */
		if (chg & 0x01) {
			dev_info(chip->dev, "head unit: phantom power %s\n",
				 (st & 0x01) ? "off" : "on");
			if (chip->hu_plug_ctl[2])
				snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
					       &chip->hu_plug_ctl[2]->id);
		}
		/* bit 6 (headphone amp) follows every output switch: not logged */
		if (chg & ~0x7b)
			dev_info(chip->dev, "head unit: status 0x%02x (other bits changed: 0x%02x)\n",
				 r[4], chg & ~0x7b);
		/* bit 3 = SBX button, 1 while held (b5 -> bd, found live
		 * 2026-11-04): act on the press */
		/* SBX acts on the press report (b1 01 01, below), on release,
		 * as Windows: a 5 s hold switches the display instead */
		if (chip->hu_status_valid && (chg & 0x08) && (st & 0x08))
			ae9_dbg(chip, "head unit: SBX button down\n");
		creative_ae9_hu_amp_track(chip, st);
		chip->hu_status = st;
		chip->hu_status_valid = true;

		if (chip->hu_bit7_stuck) {
			/* presses come from the b1 reports (below) until bit 7
			 * reads "not held" again; that isn't a release */
			if (!down) {
				chip->hu_bit7_stuck = false;
				chip->hu_btn_down = false;
				dev_info(chip->dev, "head unit: knob press bit back to normal\n");
			}
		} else {
			if (down && !chip->hu_btn_down) {
				chip->hu_btn_since = jiffies;
			} else if (!down && chip->hu_btn_down) {
				*press_ms = jiffies_to_msecs(jiffies - chip->hu_btn_since);
				chip->hu_btn_seen = jiffies;
			}
			chip->hu_btn_down = down;
		}
	}
	/* The b1 02 reply (full poll only) also reports presses, with the
	 * duration capped at 3000 ms; a long hold is reported at 3 s while
	 * still held. Use it only for a short press the fast poll missed:
	 * not while held, not right after a press the fast poll timed. */
	/*
	 * On headphones with the SBX light on, bit 7 can stick at "held"
	 * after a knob press (2026-11-04: 0x77 for 22 s while the b1 reports
	 * kept coming, 104/109/183 ms presses; it came back at the next SBX
	 * press, or on speakers). b1 reports a press when it ends, and the
	 * full poll reads 83 02 before b1, so a real release is always seen
	 * first: a b1 report while bit 7 still says held means it's stuck.
	 * Then take presses from b1 until bit 7 reads "not held" again. b1
	 * caps at 3000 ms and fires at 3 s while still held: a 3000 report
	 * means "held 3 s", the knob-light hold, acted on right then.
	 */
	if (r[1] == 0xb1 && n >= 8 && r[3] == 0x02 && r[4] == 0x01) {
		unsigned int ms = r[5] | (r[6] << 8);

		if (!chip->hu_bit7_stuck && chip->hu_btn_down && ms < 3000 &&
		    jiffies_to_msecs(jiffies - chip->hu_btn_since) + 100 >= ms) {
			chip->hu_bit7_stuck = true;
			dev_info(chip->dev, "head unit: knob press bit stuck at held; using the press reports\n");
		}
		if (chip->hu_bit7_stuck) {
			*press_ms = ms;
		} else if (!chip->hu_btn_down && ms < 1000 &&
			   time_after(jiffies, chip->hu_btn_seen + msecs_to_jiffies(1500))) {
			*press_ms = ms;
		}
	}
	/*
	 * Item 01 is SBX: Windows lights its LED with f0 22 02 01 <on> f7
	 * (capture 6, at each SBX toggle; item 02 is the knob's button and
	 * light). The button itself is bit 3 of the 83 01 02 status byte
	 * (above); the b1 reply for item 01 also reports each press, as for
	 * the knob: f0 b1 04 01 01 <ms lo> <ms hi> f7 (seen 2026-11-04, not
	 * needed). Item 03's b1 reply is logged, to learn what it is.
	 */
	if (r[1] == 0xb1 && n >= 8 && r[3] == 0x01 && r[4] == 0x01) {
		unsigned int ms = r[5] | (r[6] << 8);

		if (ms >= HU_SBX_LONG_MS)
			chip->hu_display_to = !chip->hu_display;
		else
			chip->hu_sbx_press = true;
		ae9_dbg(chip, "head unit: SBX button %u ms\n", ms);
	}
	if (ae9_debug >= 2 && r[1] == 0xb1 && n >= 8 && r[4])
		dev_info(chip->dev, "hu_debug: b1 item %u press %u ms\n",
			 r[3], r[5] | (r[6] << 8));
	if (r[1] == 0xb1 && n >= 8 && r[3] == 0x03 && r[4])
		dev_info(chip->dev, "head unit: b1 item 3 reports a press (%u ms, reply %*ph)\n",
			 r[5] | (r[6] << 8), min(n, 9), r);
	/* 83 01 07 reply (f0 83 02 07 <byte> f7, 0x0a in every capture) */
	if (r[1] == 0x83 && n >= 6 && r[3] == 0x07) {
		if (chip->hu_st07_valid && r[4] != chip->hu_st07)
			dev_info(chip->dev, "head unit: 83 07 status 0x%02x -> 0x%02x\n",
				 chip->hu_st07, r[4]);
		chip->hu_st07 = r[4];
		chip->hu_st07_valid = true;
	}
	return 0;
}

static void creative_ae9_hu_label(struct creative_chip *chip)
{
	u8 resp[24], label[13] = { 0xf0, 0x11, 0x09 };
	char text[9] = "";
	unsigned long flags;

	spin_lock_irqsave(&chip->lock, flags);	/* debugfs "hutext" writes it */
	strscpy(text, chip->hu_text, sizeof(text));
	spin_unlock_irqrestore(&chip->lock, flags);
	if (!text[0] && !chip->hu_display)
		return;			/* off: Windows sends no text either */
	if (!text[0] && chip->hu_ae9_hold) {
		/* Windows sends no text while "AE-9" is held (its text
		 * setter refuses too); then the output name, 10 polls */
		if (time_before(jiffies, chip->hu_ae9_until))
			return;
		chip->hu_ae9_hold = false;
		chip->hu_name_until = chip->hu_ticks + HU_SP_TICKS;
	}
	if (!text[0]) {			/* no "hutext" test text held */
		if (chip->hu_ticks < chip->hu_name_until)
			strscpy(text, chip->hu_output ? "-HP-" : "-SP-",
				sizeof(text));
		else if (chip->hu_mute)
			strscpy(text, "MUTE", sizeof(text));
		else
			snprintf(text, sizeof(text), "%s%u.%u",
				 chip->hu_att ? "-" : "",
				 chip->hu_att / 2, (chip->hu_att & 1) * 5);
	}
	memcpy(label + 3, text, strlen(text));
	label[12] = 0xf7;
	creative_spi_frame_win(chip, label, sizeof(label), resp, sizeof(resp));
}

/*
 * Switch the output between the card's RCA jacks (speakers) and the head
 * unit's headphone jack, as Windows does (creative_ae9_out_switch(),
 * creative_ae9_seq_run.c), then restore that output's own volume. Windows also
 * brackets the change with f0 22 02 01 01 / 00 on the head unit, which is
 * the SBX light (item 01): it flashes, then ends off. The heartbeat puts the
 * real state back.
 */
/*
 * "Apply Headphones Virtualization to Line Out" (Sound Blaster Command;
 * CtxHda property 0x75 HeadphoneOutput = 1, FUN_00020bdc): with Headphones
 * selected and Direct Mode off, the card stays in headphone mode (map 0xa1,
 * DSP speaker config 0, the headphone SBX/EQ/profile) but the head unit
 * plays it out of the card's RCA out, so the headphone jack is silent.
 * Switching to Headphones with it on (capture 4 s11 00:58:27.54 #16040-
 * 16500): the speaker switch, except ChipIO 0x0d = 0xa1, pin 0x0f pinctl
 * 0x00 and no GPIO 0x320 write. Toggling it while on Headphones (00:57:53,
 * 00:58:37, 00:59:43, 01:00:11): only the jack select (creative_ae9_hu_jack).
 */
static bool ae9_lo_virt_on(struct creative_chip *chip)
{
	return chip->lo_virt && !chip->fx.direct;
}

static void creative_ae9_hu_route(struct creative_chip *chip, bool hp)
{
	chip->hu_amp_since = 0;		/* a fresh jack frame: the amp check starts over */
	chip->hu_amp_fix = false;
	chip->lo_virt_phys = hp && ae9_lo_virt_on(chip);
	creative_ae9_out_switch(chip, hp, chip->lo_virt_phys);
}

/*
 * Windows' HeadphoneOutput toggle while on Headphones: the jack select only
 * (creative_ae9_out_jack(), creative_ae9_seq_run.c).
 */
static void creative_ae9_hu_jack(struct creative_chip *chip, bool lo)
{
	chip->hu_amp_since = 0;		/* a fresh jack frame: the amp check starts over */
	chip->hu_amp_fix = false;
	chip->lo_virt_phys = lo;
	creative_ae9_out_jack(chip, lo);
}

/*
 * Volume per output (0.12.15). hu_att is the live volume, the one of output
 * hu_att_for; hu_att_out[] keeps the other output's. "Master Playback
 * Volume" addresses the output the user picked last: a pending switch's
 * (hu_switch_to), else hu_att_for. Until 0.12.14 a volume put while a
 * switch was pending changed the live (old) output, and the switch then
 * stored that as the old output's level and loaded the new output's stale
 * one: alsactl restores "Head Unit Output" before the volume, so after a
 * reload the headphones' level went to the speakers (2026-11-20: speakers
 * at att 0x0c = -6 dB) and the headphones got the boot default. Taking the
 * pending switch, the swap below and a put's slot choice all hold
 * chip->lock, so a put lands on one side of a switch, never between.
 */
static unsigned int *creative_ae9_hu_att_slot(struct creative_chip *chip)
{
	int to = chip->hu_switch_to;	/* caller holds chip->lock */

	if (to >= 0 && (unsigned int)to != chip->hu_att_for)
		return &chip->hu_att_out[to];
	return &chip->hu_att;
}

/* the live volume becomes output hp's (caller holds chip->lock) */
static void creative_ae9_hu_att_swap(struct creative_chip *chip, unsigned int hp)
{
	if (hp == chip->hu_att_for)
		return;
	chip->hu_att_out[chip->hu_att_for] = chip->hu_att;
	chip->hu_att = chip->hu_att_out[hp];
	chip->hu_att_for = hp;
}

static void creative_ae9_hu_switch(struct creative_chip *chip, unsigned int hp)
{
	unsigned long flags;
	u8 resp[24];

	hp = !!hp;
	if (hp == chip->hu_output)
		return;
	/* the volume first (hu_update already did it for a pending switch;
	 * the knob's switch comes here directly) */
	spin_lock_irqsave(&chip->lock, flags);
	creative_ae9_hu_att_swap(chip, hp);
	spin_unlock_irqrestore(&chip->lock, flags);
	/* native DSD (pcm_dsd): stopped and off first; the app's restart
	 * brings it up again on the new output, once the switch is done (its
	 * prepare waits on dsd_switching: it used to start DSD on the old
	 * output while the route below changed under it) */
	WRITE_ONCE(chip->dsd_switching, true);
	creative_ae9_dsd_abort(chip, "output switch");
	creative_ae9_hu_route(chip, hp);
	chip->hu_output = hp;
	creative_ae9_fx_output_changed(chip);	/* headphone profile back on */
	chip->hu_name_until = chip->hu_ticks + HU_SP_TICKS;
	creative_ae9_hu_label(chip);
	creative_ae9_hu_apply_volume(chip);
	creative_spi_frame_win(chip, (const u8[]){ 0xf0, 0x21, 0x03, 0x02, 0x00, 0x00, 0xf7 },
			       7, resp, sizeof(resp));
	creative_spi_frame_win(chip, (const u8[]){ 0xf0, 0x22, 0x02, 0x01, 0x01, 0xf7 },
			       6, resp, sizeof(resp));
	msleep(600);
	creative_spi_frame_win(chip, (const u8[]){ 0xf0, 0x22, 0x02, 0x01, 0x00, 0xf7 },
			       6, resp, sizeof(resp));
	chip->hu_sbx_led = -1;		/* left off: heartbeat resends it */
	if (chip->hu_vol_ctl)
		snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->hu_vol_ctl->id);
	if (chip->hu_out_ctl)
		snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->hu_out_ctl->id);
	dev_info(chip->dev, "head unit: output %s, volume att 0x%02x\n",
		 hp ? "headphones" : "speakers", chip->hu_att);
	WRITE_ONCE(chip->dsd_switching, false);
	wake_up_all(&chip->dsd_wq);
}

/* Knob light: f0 22 02 02 <0 off / 1 on> f7 (Windows, 3 s+ hold). */
static void creative_ae9_hu_light(struct creative_chip *chip, unsigned int on)
{
	u8 f[6] = { 0xf0, 0x22, 0x02, 0x02, !!on, 0xf7 }, resp[24];

	chip->hu_light = !!on;
	chip->hu_light_sent = !!on;
	creative_spi_frame_win(chip, f, sizeof(f), resp, sizeof(resp));
	dev_info(chip->dev, "head unit: knob light %s\n", on ? "on" : "off");
}

/* Display off (8 spaces, then no text) or on (the current text at once);
 * the knob light stays as it is (Windows: SBX held 5 s, FUN_0003f6xx). */
static void creative_ae9_hu_display(struct creative_chip *chip, bool on)
{
	static const u8 blank[13] = { 0xf0, 0x11, 0x09, ' ', ' ', ' ', ' ',
				      ' ', ' ', ' ', ' ', 0x00, 0xf7 };
	u8 resp[24];

	chip->hu_display = on;
	chip->hu_ae9_hold = false;
	if (on)
		creative_ae9_hu_label(chip);
	else
		creative_spi_frame_win(chip, blank, sizeof(blank), resp, sizeof(resp));
	if (chip->hu_disp_ctl)
		snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->hu_disp_ctl->id);
	dev_info(chip->dev, "head unit: display %s\n", on ? "on" : "off");
}

/* Act on knob input and pending control changes: display first (feels
 * immediate), then the DAC volume commands (~20 ms each). Holds: under
 * 1 s mutes, 1.5-3 s switches speakers/headphones, 3 s or more toggles
 * the knob light (3 s, not 3.5: the b1 report, our fallback when the
 * press bit sticks, can't count past 3000 ms). */
static void creative_ae9_hu_update(struct creative_chip *chip, int steps,
				   int press_ms)
{
	unsigned long flags;
	u8 resp[24];
	int sbx_led, to;

	if (chip->hu_sbx_press) {
		chip->hu_sbx_press = false;
		creative_ae9_fx_sbx_toggle(chip);
	}
	if (chip->hu_display_to >= 0) {
		bool on = chip->hu_display_to;

		chip->hu_display_to = -1;
		if (on != chip->hu_display)
			creative_ae9_hu_display(chip, on);
	}
	/* knob light, when it isn't what the head unit last got (after a
	 * resume): sent from here, the heartbeat, so its frames can't
	 * interleave with the poll's */
	if (chip->hu_light_sent != (int)chip->hu_light)
		creative_ae9_hu_light(chip, chip->hu_light);
	/* SBX LED: lit while the effects can run (Direct Mode off, SBX on) */
	sbx_led = !chip->fx.direct && chip->fx.sbx;
	if (sbx_led != chip->hu_sbx_led) {
		u8 f[6] = { 0xf0, 0x22, 0x02, 0x01, sbx_led, 0xf7 };
		/* the jack in use: Line Out Virtualization = speakers (00) */
		u8 mode[7] = { 0xf0, 0x03, 0x03, 0x02,
			       chip->hu_output && !chip->lo_virt_phys ? 0x40 : 0x00,
			       0x40, 0xf7 };

		/*
		 * Before each SBX light change Windows sends the output-mode
		 * frame f0 03 03 02 <00 speakers / 40 headphones> 40 f7
		 * (capture 6, ~0.4 s before each f0 22 02 01); we do the same.
		 * (It was a guess for the knob freeze, which turned out to be
		 * the stuck press bit; see hu_parse. The hu_sbx_mode option
		 * until 0.12.12.)
		 */
		creative_spi_frame_win(chip, mode, sizeof(mode), resp, sizeof(resp));
		if (creative_spi_frame_win(chip, f, sizeof(f), resp, sizeof(resp)) > 0) {
			chip->hu_sbx_led = sbx_led;
			ae9_dbg(chip, "head unit: SBX light %s\n",
				 sbx_led ? "on" : "off");
		}
	}

	/* auto switch: once per plug change, after it settles; a knob or
	 * control switch made afterwards stands until the next plug change */
	if (chip->hu_plug_pending &&
	    time_after_eq(jiffies, chip->hu_plug_at + msecs_to_jiffies(HU_PLUG_SETTLE_MS))) {
		int want = !(chip->hu_status & 0x20);
		bool sw = false;

		chip->hu_plug_pending = false;
		spin_lock_irqsave(&chip->lock, flags);
		if (chip->hu_auto_hp && chip->hu_switch_to < 0 &&
		    want != (int)chip->hu_output) {
			chip->hu_switch_to = want;
			sw = true;
		}
		spin_unlock_irqrestore(&chip->lock, flags);
		if (sw)
			dev_info(chip->dev, "head unit: auto switch to %s (3.5 mm headphone plug %s)\n",
				 want ? "headphones" : "speakers", want ? "in" : "out");
	}
	/* a pending switch: taken, and the volume swapped to its output, in
	 * one go (see creative_ae9_hu_att_slot) */
	spin_lock_irqsave(&chip->lock, flags);
	to = chip->hu_switch_to;
	chip->hu_switch_to = -1;
	if (to >= 0)
		creative_ae9_hu_att_swap(chip, to);
	spin_unlock_irqrestore(&chip->lock, flags);
	if (to >= 0) {
		chip->hu_reroute = false;	/* the switch routes it */
		creative_ae9_hu_switch(chip, to);
	}
	/* Line Out Virtualization or Direct Mode changed while on headphones:
	 * route the headphone mode to the jack it now belongs on */
	if (chip->hu_reroute) {
		chip->hu_reroute = false;
		if (chip->hu_output &&
		    chip->lo_virt_phys != ae9_lo_virt_on(chip)) {
			creative_ae9_hu_jack(chip, ae9_lo_virt_on(chip));
			creative_ae9_fx_output_changed(chip);	/* Windows' pass B */
			dev_info(chip->dev, "head unit: headphones on the %s\n",
				 chip->lo_virt_phys ? "card's RCA out (Line Out Virtualization)" :
				 "headphone jack");
		}
	}
	/* the headphone amp disagreed with our jack frame (hu_amp_track) */
	if (chip->hu_amp_fix)
		creative_ae9_hu_amp_fix(chip);
	/* a knob turn or button press ends "AE-9" at once (Windows keeps
	 * it; showing the change feels better) */
	if (steps || press_ms >= 0)
		chip->hu_ae9_hold = false;
	if (press_ms >= 1500 && press_ms < 3000) {
		creative_ae9_hu_switch(chip, !chip->hu_output);
		press_ms = -1;
	} else if (press_ms >= 3000) {
		creative_ae9_hu_light(chip, !chip->hu_light);
		press_ms = -1;
	} else if (press_ms >= 1000) {
		press_ms = -1;	/* between mute and switch: ignore */
	}

	if (steps) {
		/* the knob turns the output playing now (hu_att): a switch
		 * pending at this tick already ran above */
		spin_lock_irqsave(&chip->lock, flags);
		chip->hu_att = clamp_t(int, (int)chip->hu_att - steps, 0, 254);
		spin_unlock_irqrestore(&chip->lock, flags);
		chip->hu_mute = 0;
		if (chip->hu_vol_ctl)
			snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &chip->hu_vol_ctl->id);
	}
	if (press_ms >= 0)
		chip->hu_mute = !chip->hu_mute;
	if (steps || press_ms >= 0) {
		if (chip->hu_mute_ctl)
			snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &chip->hu_mute_ctl->id);
		chip->hu_dirty = 1;
	}
	if (!chip->hu_dirty) {
		creative_ae9_hu_vol_resend(chip);
		return;
	}
	chip->hu_dirty = 0;
	chip->hu_name_until = 0;
	creative_ae9_hu_label(chip);
	creative_ae9_hu_apply_volume(chip);
	/* Windows sends this right after a volume change */
	creative_spi_frame_win(chip, (const u8[]){ 0xf0, 0x21, 0x03, 0x02, 0x00, 0x00, 0xf7 },
			       7, resp, sizeof(resp));
}

/* Between full polls: only the knob and button queries (~3 ms). */
static void creative_ae9_hb_fast(struct creative_chip *chip)
{
	static const u8 q_knob[] = { 0xf0, 0xc2, 0x00, 0xf7 };
	static const u8 q_btn[] = { 0xf0, 0x83, 0x01, 0x02, 0xf7 };
	int steps = 0, press_ms = -1, n;
	u8 resp[24];

	creative_ext_writew(chip, 0x320, 0x105);
	n = creative_spi_frame_win(chip, q_knob, sizeof(q_knob), resp, sizeof(resp));
	if (n > 0)
		steps += creative_ae9_hu_parse(chip, resp, min_t(int, n, sizeof(resp)), &press_ms);
	n = creative_spi_frame_win(chip, q_btn, sizeof(q_btn), resp, sizeof(resp));
	if (n > 0 && !chip->hu_unattached && !chip->hu_quiet)
		steps += creative_ae9_hu_parse(chip, resp, min_t(int, n, sizeof(resp)), &press_ms);
	creative_ae9_hu_update(chip, steps, press_ms);
}

/*
 * The head unit lost its setup: it powered up again (GPIO pin 5 dropped) or
 * got the stop frame (uv55: both reset the 0x2000 marker to 0xaaaaaaaa; it
 * keeps answering, status 0xd5 -> 0x85). Windows then sends its setup again
 * (FUN_0003fe68, only when the marker reads 0xaaaaaaaa) and marks it
 * attached. Ours: the same frames, built from the current state (uv56: the
 * boot copy's "speakers" jack frame left headphones without their amp, status
 * 0x95), "AE-9" with its hold, then the volume. Phantom power goes off, as at
 * every start-up. Since 0.12.8 (the hu_reattach option until 0.12.12).
 */
#define HU_REATTACH_TRIES	3

static void creative_ae9_hu_reattach(struct creative_chip *chip)
{
	u8 resp[24];
	int lost;

	if (chip->hu_reattach_try >= HU_REATTACH_TRIES)
		return;		/* said so once; until the next bring-up */
	if (++chip->hu_reattach_try == HU_REATTACH_TRIES) {
		dev_warn(chip->dev, "head unit: still not set up after %u tries; leaving it until the next reload or sleep/wake\n",
			 HU_REATTACH_TRIES - 1);
		return;
	}
	/* the frames (creative_ae9_seq_run.c), "AE-9" and its hold only with the display on */
	lost = creative_ae9_hu_setup_again(chip, chip->hu_light,
					   chip->hu_output && !chip->lo_virt_phys,
					   chip->hu_display);
	creative_ae9_hu_apply_volume(chip);
	creative_spi_frame_win(chip, (const u8[]){ 0xf0, 0x21, 0x03, 0x02, 0x00, 0x00, 0xf7 },
			       7, resp, sizeof(resp));
	chip->hu_sbx_led = -1;		/* the poll sends it again */
	chip->hu_light_sent = chip->hu_light;
	chip->hu_phantom_req = -1;
	chip->hu_status_valid = false;	/* the next status read is the new base */
	chip->hu_amp_since = 0;		/* and the set-up sent the jack frame */
	chip->hu_amp_fix = false;
	chip->hu_reattaches++;
	dev_info(chip->dev, "head unit: powered up again (marker 0xaaaaaaaa): setup sent again (%s, volume att 0x%02x%s, phantom power off%s)\n",
		 chip->hu_output ? (chip->lo_virt_phys ? "headphones on the RCA out" : "headphones") : "speakers",
		 chip->hu_att, chip->hu_mute ? ", muted" : "",
		 lost ? ", some frames unanswered" : "");
}

/* the marker read's reply, complete: f0 d5 07 00 20 04 <4 bytes> f7 */
static int creative_ae9_hu_marker(const u8 *r, int n)
{
	static const u8 head[6] = { 0xf0, 0xd5, 0x07, 0x00, 0x20, 0x04 };

	if (n != 11 || memcmp(r, head, 6) || r[10] != 0xf7)
		return -1;			/* not a clean reply: no verdict */
	return r[6] == 0xaa && r[7] == 0xaa && r[8] == 0xaa && r[9] == 0xaa;
}

/* Windows' full poll cycle, then the display text. */
static void creative_ae9_hb_windows(struct creative_chip *chip)
{
	int i, n, fails = 0, steps = 0, press_ms = -1;
	u8 resp[24];

	creative_ext_writew(chip, 0x320, 0x105);	/* GPIO pin 5 on: the head unit's power */
	/* Windows' poll frames (creative_ae9_seq_run.c, shared with the start-up's polls) */
	for (i = 0; i < AE9_HU_POLL_FRAMES; i++) {
		const struct ae9_hu_frame *f = creative_ae9_hu_poll[i];

		n = creative_spi_frame_win(chip, f->d, f->len, resp, sizeof(resp));
		if (n < 0) {
			fails++;
			continue;
		}
		n = min_t(int, n, sizeof(resp));
		if (f->d[1] == 0xd5) {
			int aa = creative_ae9_hu_marker(resp, n);

			if (aa >= 0)
				chip->hu_quiet = false;	/* answering again */

			if (aa == 0) {
				chip->hu_reattach_try = 0;	/* attached */
				chip->hu_att_tick = chip->hu_ticks + 1;
			}
			/* given up: read the status as usual */
			if (aa >= 0)
				chip->hu_unattached = aa &&
					chip->hu_reattach_try < HU_REATTACH_TRIES;
			continue;
		}
		/*
		 * Its status bits while unattached aren't the jacks'; nor after it
		 * went quiet, until a marker answers: a head unit just powered up
		 * (re-power, hupause) can answer status 2 alone with 0x80, which
		 * reads as "mic plug in, phantom power on" (rc1 soak, journal
		 * entry 10). Windows: no 0x80 in 90117 status reads (36 captures).
		 * A lone marker miss (~1% of Windows' marker reads) changes nothing.
		 */
		if ((chip->hu_unattached || chip->hu_quiet) && resp[1] == 0x83)
			continue;
		steps += creative_ae9_hu_parse(chip, resp, n, &press_ms);
	}
	/*
	 * A dark head unit (unplugged, powering up) misses most frames of
	 * every poll, twice a second: say so once, and once when it answers
	 * again, instead of a line per poll (~50 lines for a 25 s unplug).
	 * A few misses in an answering poll (~1%, host load) keep their line.
	 */
	if (fails > AE9_HU_POLL_FRAMES / 2) {
		if (!chip->hu_dark_polls++)
			dev_info(chip->dev, "head unit: not answering (%d/%zu poll frames); waiting for it\n",
				 fails, (size_t)AE9_HU_POLL_FRAMES);
		chip->hu_quiet = true;		/* dark or powering up */
	} else {
		if (chip->hu_dark_polls)
			dev_info(chip->dev, "head unit: answering again after %u poll(s) without\n",
				 chip->hu_dark_polls);
		chip->hu_dark_polls = 0;
		if (fails)
			dev_info_ratelimited(chip->dev, "head unit: %d/%zu poll frames unanswered\n",
					     fails, (size_t)AE9_HU_POLL_FRAMES);
	}
	if (chip->hu_unattached) {
		creative_ae9_hu_reattach(chip);
		chip->hu_unattached = false;	/* the next read says */
	}
	creative_ae9_hu_update(chip, steps, press_ms);
	creative_ae9_hu_label(chip);
	chip->hu_ticks++;
}

/* ALSA controls: "Head Unit Playback Volume" (0..254 = -127.0..0 dB) and
 * "Head Unit Playback Switch", renamed to "Master Playback ..." when they
 * are added. The heartbeat applies changes. */
static const DECLARE_TLV_DB_SCALE(hu_vol_tlv, -12700, 50, 0);

static int hu_vol_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	ui->count = 1;
	ui->value.integer.min = 0;
	ui->value.integer.max = 254;
	return 0;
}

/* get and put address the output picked last: with a switch pending, the
 * new output's volume, which the switch then loads (creative_ae9_hu_att_slot) */
static int hu_vol_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	unsigned long flags;

	spin_lock_irqsave(&chip->lock, flags);
	uv->value.integer.value[0] = 254 - *creative_ae9_hu_att_slot(chip);
	spin_unlock_irqrestore(&chip->lock, flags);
	return 0;
}

static int hu_vol_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	unsigned int att = 254 - clamp_t(long, uv->value.integer.value[0], 0, 254);
	unsigned int *slot;
	unsigned long flags;
	int changed = 0;

	if (creative_ae9_dead(chip))
		return -ENODEV;

	spin_lock_irqsave(&chip->lock, flags);
	slot = creative_ae9_hu_att_slot(chip);
	if (*slot != att) {
		*slot = att;
		/* the live volume: the heartbeat sends it; a pending
		 * output's: its switch does, nothing goes out now */
		if (slot == &chip->hu_att)
			chip->hu_dirty = 1;
		changed = 1;
	}
	spin_unlock_irqrestore(&chip->lock, flags);
	return changed;
}

static int hu_mute_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);

	uv->value.integer.value[0] = !chip->hu_mute;
	return 0;
}

static int hu_mute_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	unsigned int mute = !uv->value.integer.value[0];

	if (creative_ae9_dead(chip))
		return -ENODEV;

	if (mute == chip->hu_mute)
		return 0;
	chip->hu_mute = mute;
	chip->hu_dirty = 1;
	return 1;
}

static const struct snd_kcontrol_new hu_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Head Unit Playback Volume",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
			  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
		.info = hu_vol_info,
		.get = hu_vol_get,
		.put = hu_vol_put,
		.tlv.p = hu_vol_tlv,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Head Unit Playback Switch",
		.info = snd_ctl_boolean_mono_info,
		.get = hu_mute_get,
		.put = hu_mute_put,
	},
};

/* "Mic Gain Capture Volume" (was "XLR Gain"): codec 1 node 0x12 input
 * (the head unit's input pin: XLR/TRS jack and 3.5 mm mic jack, both
 * boosted, checked 2026-11-06)
 * amp, 0/1/2 = 0/+10/+20 dB, both channels, exactly as Windows sets it.
 * The amp itself goes to +30 dB (input amp caps 0x00270300: 10 dB steps,
 * 0-3; read 2026-11-11, uv29); Windows never uses step 3, so it's only
 * offered with mic_boost_30=1 (card-tested uv30: +10 dB on voice and noise
 * alike, but normal speech clips there; for quiet mics).
 * Not called "Mic Boost" on purpose: PipeWire's mixer paths know that name
 * and would tie it to the mic slider. With this name PipeWire leaves it
 * alone and the slider is software gain only. */
static const DECLARE_TLV_DB_SCALE(mic_boost_tlv, 0, 1000, 0);

static bool mic_boost_30;
module_param(mic_boost_30, bool, 0444);
MODULE_PARM_DESC(mic_boost_30, "Mic Gain: also offer +30 dB (the amp's top step; Windows stops at +20 dB; for quiet mics, normal speech clips) (0 = no, default; 1 = yes)");

/* the AE-7's amp stops at step 2 (pin 0x12's input amp caps 0x00270200) */
static unsigned int mic_boost_max(struct creative_chip *chip)
{
	return mic_boost_30 && ae9_has(chip, AE9_CAP_AE9_MIC) ? 3 : 2;
}

static int mic_boost_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	ui->count = 1;
	ui->value.integer.min = 0;
	ui->value.integer.max = mic_boost_max(snd_kcontrol_chip(kc));
	return 0;
}

static int mic_boost_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);

	uv->value.integer.value[0] = chip->mic_boost;
	return 0;
}

static void creative_ae9_mic_boost_apply(struct creative_chip *chip)
{
	u32 resp;

	if (!chip->corb_buf)
		return;
	creative_ae9_corb_send_verb_sync(chip, 0x11236000 | chip->mic_boost, &resp);
	creative_ae9_corb_send_verb_sync(chip, 0x11235000 | chip->mic_boost, &resp);
}

static int mic_boost_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	unsigned int v = clamp_t(long, uv->value.integer.value[0], 0, mic_boost_max(chip));

	if (creative_ae9_dead(chip))
		return -ENODEV;

	if (v == chip->mic_boost)
		return 0;
	chip->mic_boost = v;
	creative_ae9_mic_boost_apply(chip);
	return 1;
}

static const struct snd_kcontrol_new mic_boost_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Mic Gain Capture Volume",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	.info = mic_boost_info,
	.get = mic_boost_get,
	.put = mic_boost_put,
	.tlv.p = mic_boost_tlv,
};

/*
 * "Capture Volume": the mic level, as Windows' Recording -> Levels slider
 * sets it (capture 2026-11-03): codec 1 node 0x07's input amp (L, R) at
 * step = dB + 90 (0x63 = +9 dB; 0x80 = mute), and DSP module 0x37 (mic
 * volume) req 1 = 0 (unmute), req 2/3 = the same dB as a float, L and R.
 * The 0x797/0x798 verb before each amp write carries the fraction of a dB
 * in 1/64 steps (0 here, whole dB). 0 = mute, 1..99 = -89..+9 dB. The
 * capture level and playback start blocks set +9 dB, so this is re-applied
 * after them.
 */
static const DECLARE_TLV_DB_SCALE(mic_level_tlv, -9000, 100, 1);

/* Whole numbers as IEEE-754 single bits (no FPU in the kernel). */
static u32 ae9_int_f32(int v)
{
	u32 m = abs(v), e;

	if (!m)
		return 0;
	e = fls(m) - 1;
	return (v < 0 ? BIT(31) : 0) | ((e + 127) << 23) |
	       ((m << (23 - e)) & 0x7fffff);
}

void creative_ae9_mic_level_apply(struct creative_chip *chip)
{
	int db = (int)chip->mic_level - 90;
	u32 amp = chip->mic_level ? chip->mic_level : 0x80;
	u32 resp, f = ae9_int_f32(chip->mic_level ? db : -90), zero = 0;

	if (!chip->corb_buf || !chip->dsp_done)
		return;
	mutex_lock(&chip->replay_mutex);
	creative_ae9_corb_send_verb_sync(chip, 0x10779700, &resp);
	creative_ae9_corb_send_verb_sync(chip, 0x10736000 | amp, &resp);
	creative_ae9_corb_send_verb_sync(chip, 0x10779800, &resp);
	creative_ae9_corb_send_verb_sync(chip, 0x10735000 | amp, &resp);
	creative_dsp_scp_send(chip, (1 << 17) | (0x20 << 8) | 0x37, &zero, 1);
	creative_dsp_scp_send(chip, (2 << 17) | (0x20 << 8) | 0x37, &f, 1);
	creative_dsp_scp_send(chip, (3 << 17) | (0x20 << 8) | 0x37, &f, 1);
	creative_dsp_chipio_addr_invalidate();
	mutex_unlock(&chip->replay_mutex);
}

static int mic_level_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	ui->count = 1;
	ui->value.integer.min = 0;
	ui->value.integer.max = 99;
	return 0;
}

static int mic_level_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);

	uv->value.integer.value[0] = chip->mic_level;
	return 0;
}

static int mic_level_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	unsigned int v = clamp_t(long, uv->value.integer.value[0], 0, 99);

	if (creative_ae9_dead(chip))
		return -ENODEV;

	if (v == chip->mic_level)
		return 0;
	chip->mic_level = v;
	creative_ae9_mic_level_apply(chip);
	return 1;
}

static const struct snd_kcontrol_new mic_level_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Capture Volume",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	.info = mic_level_info,
	.get = mic_level_get,
	.put = mic_level_put,
	.tlv.p = mic_level_tlv,
};

/*
 * "DAC Filter": the ES9038Q2M's digital filter.  Register 7 bits 7:5
 * select it; 0x80 (Apodizing Fast) is Windows' boot filter.  The seven
 * modes from the DarkNikGr driver (PR #2), with Creative's names. Each pick also sets the second DAC (SABRE9006A)
 * register 14 "FIR Rolloff" (0x49/0x0e) bit 0: 0 only for Slow Roll Off,
 * 1 (its power-on value) for the rest. Verified in a Windows capture of
 * all seven, 2026-11-07 (capture 6, section 14; notes 33/33v): per click
 * Windows reads and writes 48/07, then reads and writes 49/0e.
 */
#define DAC_FILTER_MASK			0xe0
/*
 * Item 0 is the default, then the Creative app's order. The old control
 * was Default/Fast/Slow; alsactl restores a saved name that no longer
 * exists as item 0 (seen 2026-11-08: "Default" came back as item 0), so
 * item 0 must be Windows' filter.
 */
#define DAC_FILTER_DEFAULT_INDEX	0	/* 0x80, Windows' boot filter */

static const char * const dac_filter_names[] = {
	"Apodizing Fast Roll Off",
	"Fast Roll Off",
	"Slow Roll Off",
	"Fast Roll Off - Minimum Phase",
	"Slow Roll Off - Minimum Phase",
	"Hybrid Fast Roll Off",
	"Brick Wall",
};

static const u8 dac_filter_values[] = {
	0x80, 0x00, 0x20, 0x40, 0x60, 0xc0, 0xe0,
};

#define DAC_FILTER_SLOW_BITS		0x20	/* the one with 49/0e = 0 */

static void creative_ae9_dac_filter_apply(struct creative_chip *chip)
{
	u8 cmd[2][3];

	if (!chip->corb_buf || !chip->dsp_done)
		return;
	cmd[0][0] = 0x48;
	cmd[0][1] = 0x07;
	cmd[0][2] = (chip->dac48_07 & ~DAC_FILTER_MASK) |
		    chip->dac_filter_bits;
	cmd[1][0] = 0x49;
	cmd[1][1] = 0x0e;
	cmd[1][2] = chip->dac_filter_bits == DAC_FILTER_SLOW_BITS ? 0x00 : 0x01;
	creative_dsp_ca0113_list_quiet(chip, (const u8 (*)[3])cmd, 2);
}

static int dac_filter_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	return snd_ctl_enum_info(ui, 1, ARRAY_SIZE(dac_filter_names),
				 dac_filter_names);
}

static int dac_filter_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);

	uv->value.enumerated.item[0] = chip->dac_filter;
	return 0;
}

static int dac_filter_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	unsigned int v = uv->value.enumerated.item[0];

	if (creative_ae9_dead(chip))
		return -ENODEV;

	if (v >= ARRAY_SIZE(dac_filter_values))
		return -EINVAL;
	if (v == chip->dac_filter)
		return 0;
	chip->dac_filter = v;
	chip->dac_filter_bits = dac_filter_values[v];
	creative_ae9_dac_filter_apply(chip);
	return 1;
}

static const struct snd_kcontrol_new dac_filter_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "DAC Filter",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info = dac_filter_info,
	.get = dac_filter_get,
	.put = dac_filter_put,
};

/* "Head Unit Output": Speakers (card RCA) / Headphones (head unit). */
static int hu_out_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	static const char * const names[] = { "Speakers", "Headphones" };

	return snd_ctl_enum_info(ui, 1, 2, names);
}

static int hu_out_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);

	unsigned long flags;

	spin_lock_irqsave(&chip->lock, flags);
	uv->value.enumerated.item[0] = chip->hu_switch_to >= 0 ?
				       chip->hu_switch_to : chip->hu_att_for;
	spin_unlock_irqrestore(&chip->lock, flags);
	return 0;
}

static int hu_out_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	unsigned int v = uv->value.enumerated.item[0], cur;
	unsigned long flags;

	if (creative_ae9_dead(chip))
		return -ENODEV;

	if (v > 1)
		return -EINVAL;
	spin_lock_irqsave(&chip->lock, flags);
	cur = chip->hu_switch_to >= 0 ? chip->hu_switch_to : chip->hu_att_for;
	if (v != cur)
		/* the heartbeat does the switch; back to the live output
		 * before it did = no switch */
		chip->hu_switch_to = v == chip->hu_att_for ? -1 : v;
	spin_unlock_irqrestore(&chip->lock, flags);
	if (v == cur)
		return 0;
	/* the volume control now reads the picked output's level */
	if (chip->hu_vol_ctl)
		snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->hu_vol_ctl->id);
	return 1;
}

static const struct snd_kcontrol_new hu_out_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Head Unit Output",
	.info = hu_out_info,
	.get = hu_out_get,
	.put = hu_out_put,
};

/*
 * "Card Answering" (read-only): off once the card dropped off the bus or
 * stopped answering, with a change event, so apps that only listen (AudioMon
 * follows controls with `amixer events`) can let go of the card: an open
 * control device keeps "modprobe -r" from unloading the driver. A reload
 * brings a new card where it reads on.
 */
/*
 * Router slots in no router-chip list (ae9_rt_list_map, about 80 XRAM
 * reads): measured by the heartbeat at most every 30 s, or by debugfs
 * (force); the "Driver Health" read only returns the cached value (no card
 * I/O in a control read). -1 until measured; a failed walk keeps the old
 * value and its age, so the next heartbeat tries again.
 */
static int ae9_rt_true_free(struct creative_chip *chip, bool force)
{
	DECLARE_BITMAP(inl, AE9_RT_ENTRIES);
	int n;

	if (!chip->corb_buf || !chip->dsp_done || chip->suspended ||
	    READ_ONCE(chip->unloading) || creative_ae9_dead(chip))
		return chip->rt_free_true;
	mutex_lock(&chip->replay_mutex);
	if (!force && chip->rt_free_at &&
	    time_before(jiffies, chip->rt_free_at + 30 * HZ)) {
		mutex_unlock(&chip->replay_mutex);
		return chip->rt_free_true;
	}
	n = ae9_rt_list_map(chip, inl);
	creative_dsp_chipio_addr_invalidate();
	if (n >= 0)
		chip->rt_free_at = jiffies ?: 1;
	mutex_unlock(&chip->replay_mutex);
	if (n >= 0 && n <= AE9_RT_ENTRIES - 0x0c &&
	    chip->rt_free_true != AE9_RT_ENTRIES - 0x0c - n) {
		chip->rt_free_true = AE9_RT_ENTRIES - 0x0c - n;
		creative_ae9_health_notify(chip);
	}
	return chip->rt_free_true;
}

/*
 * "Driver Health" (read-only, 9 integers), for apps that can't read
 * debugfs (root only), e.g. AudioMon: [0] verb timeouts, [1] RIRB entries
 * never landed, [2] answers from the other codec, [3] unreadable RIRB
 * write pointer, [4] BDLs kept (stream didn't stop) — all 0 on a healthy
 * card, a change event when one moves; [5] RIRB entries that landed late,
 * [6] the longest wait for one (us) — information, no events; [7] router
 * slots in no router-chip list (-1 = not measured; the heartbeat measures
 * it every 30 s, an event when it changes); [8] DAC writes the DAC didn't
 * acknowledge after the bring-up (I2C NACK, dac_i2c.late; dsp.c: each
 * counted once, whether its resend got through or not): 0 on a healthy
 * card, an event when it rises (0.12.17; AudioMon 0.4.11+ takes 8 or more
 * values, older AudioMon shows n/a).
 */
static int health_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	ui->count = 9;
	ui->value.integer.min = -1;
	ui->value.integer.max = INT_MAX - 1;	/* max - min fits a 32-bit long */
	return 0;
}

static int health_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	long *v = uv->value.integer.value;

	v[0] = chip->corb_timeouts;
	v[1] = chip->rirb_never;
	v[2] = chip->rirb_stray;
	v[3] = chip->rirb_badwp;
	v[4] = atomic_read(&chip->bdl_leaks);
	v[5] = chip->rirb_late;
	v[6] = chip->rirb_late_max_us;
	v[7] = READ_ONCE(chip->rt_free_true);	/* the heartbeat measures it */
	v[8] = READ_ONCE(chip->dac_i2c.late);
	return 0;
}

static const struct snd_kcontrol_new health_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Driver Health",
	.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.info = health_info,
	.get = health_get,
};

void creative_ae9_health_notify(struct creative_chip *chip)
{
	if (chip->health_ctl && chip->card && !chip->unloading)
		snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->health_ctl->id);
}

static int answer_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	uv->value.integer.value[0] = !creative_ae9_dead(snd_kcontrol_chip(kc));
	return 0;
}

static const struct snd_kcontrol_new answer_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Card Answering",
	.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.info = snd_ctl_boolean_mono_info,
	.get = answer_get,
};

/*
 * "Card Model" (0.13.0): read-only, on every card, one item naming the
 * board (its card shortname: "Sound Blaster AE-9", "Sound Blaster AE-7"),
 * value always 0, so tools tell the boards apart from the control alone.
 * Added last in the probe: no older control's numid moves.
 */
static int card_model_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *uinfo)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	const char *name = chip->board->name;

	return snd_ctl_enum_info(uinfo, 1, 1, &name);
}

static int card_model_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	uv->value.enumerated.item[0] = 0;
	return 0;
}

static const struct snd_kcontrol_new card_model_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_CARD,
	.name = "Card Model",
	.access = SNDRV_CTL_ELEM_ACCESS_READ,
	.info = card_model_info,
	.get = card_model_get,
};

void creative_ae9_dead_notify(struct creative_chip *chip)
{
	if (chip->answer_ctl && chip->card && !chip->unloading)
		snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->answer_ctl->id);
}

/* "Head Unit Display": the display on/off, as the SBX button's 5 s hold */
static int hu_disp_get_cur(struct creative_chip *chip)
{
	return chip->hu_display_to >= 0 ? chip->hu_display_to : chip->hu_display;
}

static int hu_disp_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);

	uv->value.integer.value[0] = hu_disp_get_cur(chip);
	return 0;
}

static int hu_disp_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	int on = !!uv->value.integer.value[0];

	if (creative_ae9_dead(chip))
		return -ENODEV;

	if (on == hu_disp_get_cur(chip))
		return 0;
	chip->hu_display_to = on;	/* the heartbeat applies it */
	return 1;
}

static const struct snd_kcontrol_new hu_disp_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Head Unit Display",
	.info = snd_ctl_boolean_mono_info,
	.get = hu_disp_get,
	.put = hu_disp_put,
};

/* pending re-route for the heartbeat (head unit traffic runs only there) */
void creative_ae9_hu_reroute_request(struct creative_chip *chip)
{
	chip->hu_reroute = true;
}

static int lo_virt_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);

	uv->value.integer.value[0] = chip->lo_virt;
	return 0;
}

static int lo_virt_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	bool on = !!uv->value.integer.value[0];

	if (creative_ae9_dead(chip))
		return -ENODEV;

	if (on == chip->lo_virt)
		return 0;
	chip->lo_virt = on;
	dev_info(chip->dev, "Line Out Virtualization %s%s\n", on ? "on" : "off",
		 !chip->hu_output ? " (takes effect on headphones)" :
		 chip->fx.direct ? " (takes effect with Direct Mode off)" : "");
	creative_ae9_hu_reroute_request(chip);
	return 1;
}

/* Sound Blaster Command: "Apply Headphones Virtualization to Line Out" */
static const struct snd_kcontrol_new lo_virt_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Line Out Virtualization Switch",
	.info = snd_ctl_boolean_mono_info,
	.get = lo_virt_get,
	.put = lo_virt_put,
};

/* "Head Unit 3.5mm Phones Plugged" (head unit's 3.5 mm headphone jack,
 * status bit 5), "Head Unit 3.5mm Mic Plugged" (head unit's 3.5 mm mic
 * jack, bit 4; not the XLR/TRS jack) and "Head Unit Phantom Power" (48 V
 * on the XLR/TRS jack, bit 0; its button, or this control: see
 * hu_phantom_put). The plug controls are read-only, for apps to show. Each bit reads 0 when on/plugged. Named
 * without "Jack" on purpose: PipeWire switches outputs on "... Jack"
 * controls; switching is left to "Head Unit Auto Switch". */
static int hu_plug_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);

	uv->value.integer.value[0] = chip->hu_status_valid &&
				     !(chip->hu_status & kc->private_value);
	return 0;
}

/*
 * "Head Unit Phantom Power" is also a switch (2026-11-06): the head unit
 * takes f0 05 03 02 01 <0|1> f7 (01 01 = on, 01 00 = off; with 00 as the
 * first byte phantom stays off), and reports it back in 85 01 02 and status
 * bit 0. Windows only sends 01 00 (off) at boot, in the post-boot replay, so
 * phantom always comes up off. "On" is refused for the first
 * AE9_PHANTOM_HOLD_S after a bring-up so a restored mixer state (alsactl at
 * boot) can't put 48 V on the XLR/TRS jack by itself; apps should warn.
 */

static int hu_phantom_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	int on = !!uv->value.integer.value[0];
	unsigned long flags;

	if (creative_ae9_dead(chip))
		return -ENODEV;

	/*
	 * No status read yet (just loaded, or the head unit was just set up
	 * again): phantom power is off, our own set-up sent it off, and
	 * the control reads off. "Off" is then no change (alsactl restore
	 * at boot failed on the -EAGAIN this returned until 0.12.14);
	 * "on" is refused like during the hold below, never queued for
	 * later.
	 */
	if (!chip->hu_status_valid) {
		if (!on)
			return 0;
		dev_info(chip->dev, "head unit: phantom power on refused, head unit status not read yet\n");
		return -EBUSY;
	}
	if (on == !(chip->hu_status & 0x01))
		return 0;
	if (on && time_before(jiffies, chip->hu_phantom_hold)) {
		dev_info(chip->dev, "head unit: phantom power on refused, too soon after start-up (%u s)\n",
			 AE9_PHANTOM_HOLD_S);
		return -EBUSY;
	}
	spin_lock_irqsave(&chip->lock, flags);
	chip->hu_phantom_req = on;
	spin_unlock_irqrestore(&chip->lock, flags);
	dev_info(chip->dev, "head unit: phantom power %s requested\n", on ? "on" : "off");
	return 1;
}

static const struct snd_kcontrol_new hu_plug_controls[3] = {
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Head Unit 3.5mm Phones Plugged",
	  .access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	  .info = snd_ctl_boolean_mono_info, .get = hu_plug_get, .private_value = 0x20 },
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Head Unit 3.5mm Mic Plugged",
	  .access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	  .info = snd_ctl_boolean_mono_info, .get = hu_plug_get, .private_value = 0x10 },
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = "Head Unit Phantom Power",
	  .access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	  .info = snd_ctl_boolean_mono_info, .get = hu_plug_get, .put = hu_phantom_put,
	  .private_value = 0x01 },
};

/* "Head Unit Auto Switch": off (default) = plugs only log; on = the 3.5 mm
 * headphone plug switches headphones in and out. */
static int hu_auto_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);

	uv->value.integer.value[0] = chip->hu_auto_hp;
	return 0;
}

static int hu_auto_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	bool on = !!uv->value.integer.value[0];

	if (creative_ae9_dead(chip))
		return -ENODEV;

	if (on == chip->hu_auto_hp)
		return 0;
	chip->hu_auto_hp = on;
	dev_info(chip->dev, "head unit: auto switch %s\n", on ? "on" : "off");
	return 1;
}

static const struct snd_kcontrol_new hu_auto_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Head Unit Auto Switch",
	.info = snd_ctl_boolean_mono_info,
	.get = hu_auto_get,
	.put = hu_auto_put,
};

static void creative_ae9_hu_controls_add(struct creative_chip *chip)
{
	int i;

	AE9_HOOK(chip, controls_add, chip);
	chip->hu_att = HU_ATT_DEFAULT;
	chip->hu_att_out[0] = chip->hu_att_out[1] = HU_ATT_DEFAULT;
	chip->hu_output = 0;		/* the boot replay selects speakers */
	chip->hu_att_for = 0;
	chip->hu_name_until = HU_SP_TICKS;
	chip->hu_switch_to = -1;
	chip->hu_display = true;
	chip->hu_display_to = -1;
	chip->hu_light = 1;
	chip->hu_light_sent = 1;	/* the head unit starts lit */
	chip->hu_sbx_led = -1;
	snd_ctl_add(chip->card, snd_ctl_new1(&lo_virt_control, chip));
	chip->hu_out_ctl = snd_ctl_new1(&hu_out_control, chip);
	if (snd_ctl_add(chip->card, chip->hu_out_ctl) < 0)
		chip->hu_out_ctl = NULL;
	chip->answer_ctl = snd_ctl_new1(&answer_control, chip);
	if (snd_ctl_add(chip->card, chip->answer_ctl) < 0)
		chip->answer_ctl = NULL;
	chip->rt_free_true = -1;
	chip->health_ctl = snd_ctl_new1(&health_control, chip);
	if (snd_ctl_add(chip->card, chip->health_ctl) < 0)
		chip->health_ctl = NULL;
	chip->hu_disp_ctl = snd_ctl_new1(&hu_disp_control, chip);
	if (snd_ctl_add(chip->card, chip->hu_disp_ctl) < 0)
		chip->hu_disp_ctl = NULL;
	chip->hu_vol_ctl = snd_ctl_new1(&hu_controls[0], chip);
	chip->hu_mute_ctl = snd_ctl_new1(&hu_controls[1], chip);
	/* named "Master Playback Volume" / "Switch", so PipeWire/PulseAudio
	 * use them as the hardware volume and the desktop slider and the knob
	 * move together (the hu_master option until 0.12.15: 0 kept the
	 * "Head Unit Playback ..." names) */
	if (chip->hu_vol_ctl && chip->hu_mute_ctl) {
		strscpy(chip->hu_vol_ctl->id.name, "Master Playback Volume",
			sizeof(chip->hu_vol_ctl->id.name));
		strscpy(chip->hu_mute_ctl->id.name, "Master Playback Switch",
			sizeof(chip->hu_mute_ctl->id.name));
	}
	if (snd_ctl_add(chip->card, chip->hu_vol_ctl) < 0)
		chip->hu_vol_ctl = NULL;
	if (snd_ctl_add(chip->card, chip->hu_mute_ctl) < 0)
		chip->hu_mute_ctl = NULL;
	chip->hu_auto_hp = hu_auto_switch;
	chip->hu_auto_ctl = snd_ctl_new1(&hu_auto_control, chip);
	if (snd_ctl_add(chip->card, chip->hu_auto_ctl) < 0)
		chip->hu_auto_ctl = NULL;
	for (i = 0; i < ARRAY_SIZE(hu_plug_controls); i++) {
		chip->hu_plug_ctl[i] = snd_ctl_new1(&hu_plug_controls[i], chip);
		if (snd_ctl_add(chip->card, chip->hu_plug_ctl[i]) < 0)
			chip->hu_plug_ctl[i] = NULL;
	}
}

static void creative_ae9_hb_work(struct work_struct *work)
{
	struct creative_chip *chip =
		container_of(work, struct creative_chip, hb_work.work);

	if (READ_ONCE(chip->unloading))
		return;
	AE9_HOOK(chip, tick, chip);
	/* "Driver Health" [7]: at most every 30 s (cheap no-op otherwise) */
	if (chip->dsp_done && (!chip->rt_free_at ||
			       time_after(jiffies, chip->rt_free_at + 30 * HZ)))
		ae9_rt_true_free(chip, false);

	if (chip->dsp_done && chip->hu_paused &&
	    time_before(jiffies, chip->hu_pause_until)) {
		queue_delayed_work(chip->wq, &chip->hb_work, msecs_to_jiffies(100));
		return;
	}
	if (chip->hu_paused) {
		chip->hu_paused = false;
		chip->hu_quiet = true;		/* it may have powered off meanwhile */
		dev_info(chip->dev, "head unit: poll resumed\n");
	}
	if (chip->dsp_done) {
		if (chip->hu_dbg_len) {
			u8 tx[48], rx[16];
			unsigned long flags;
			int k, n, j, p, f, o, r = 0;
			char hex[16 * 3 + 1];

			spin_lock_irqsave(&chip->lock, flags);
			k = chip->hu_dbg_len;
			memcpy(tx, chip->hu_dbg_tx, k);
			chip->hu_dbg_len = 0;
			spin_unlock_irqrestore(&chip->lock, flags);
			/* the frames back to back, each after the previous reply */
			for (p = 0; p < k; p += f) {
				f = tx[p + 2] + 4;	/* checked when queued */
				/* a jack frame by hand (the uv33-36 method): the amp
				 * safety net leaves it until the amp agrees again */
				if (tx[p + 1] == 0x03 && f >= 5 && tx[p + 3] == 0x02)
					creative_ae9_hu_amp_hold(chip);
				n = creative_spi_frame_win(chip, tx + p, f, rx, sizeof(rx));
				hex[0] = '\0';
				for (j = 0, o = 0; j < min(n, 16); j++)
					o += scnprintf(hex + o, sizeof(hex) - o, " %02x", rx[j]);
				dev_info(chip->dev, "huframe: sent %*ph, reply%s (%d)\n",
					 f, tx + p, n > 0 ? hex : " none", n);
				r += scnprintf(g_dbg_result + r, sizeof(g_dbg_result) - r,
					       "huframe reply:%s (%d)\n", n > 0 ? hex : " none", n);
			}
		}
		if (chip->hu_phantom_req >= 0) {
			u8 tx[7] = { 0xf0, 0x05, 0x03, 0x02, 0x01, 0x00, 0xf7 };
			u8 rx[8];
			unsigned long flags;

			spin_lock_irqsave(&chip->lock, flags);
			tx[5] = chip->hu_phantom_req;
			chip->hu_phantom_req = -1;
			spin_unlock_irqrestore(&chip->lock, flags);
			creative_spi_frame_win(chip, tx, sizeof(tx), rx, sizeof(rx));
		}
		if (time_after_eq(jiffies, chip->hu_next_full)) {
			chip->hu_next_full = jiffies + msecs_to_jiffies(acm_heartbeat_ms);
			creative_ae9_hb_windows(chip);
		} else {
			creative_ae9_hb_fast(chip);
		}
		if (acm_heartbeat_ms)
			queue_delayed_work(chip->wq, &chip->hb_work,
					      msecs_to_jiffies(min(HU_FAST_MS, acm_heartbeat_ms)));
		return;
	}
	/* DSP not up yet: look again later */
	if (acm_heartbeat_ms)
		queue_delayed_work(chip->wq, &chip->hb_work,
				      msecs_to_jiffies(acm_heartbeat_ms));
}

/* forward declaration — defined after probe */
static irqreturn_t creative_ae9_irq_handler(int irq, void *dev_id);

/*
 * Hardware bring-up, from a powered-up but unconfigured card to a running
 * DSP with the head unit live: bus reset, PCI cfg 0x40, Windows' BAR2 init,
 * controller reset, codec detect, pre-download replay, firmware, boot
 * handshake, post-boot replay. Runs at probe and again on resume (the card
 * loses all of this in suspend).
 */
static void creative_ae9_resume_work(struct work_struct *work);

/* Card registers a cold power-on reads as 0 (GPIO 0x320) or known values;
 * logged at each bring-up to see what survives a suspend or a reload. */
static void creative_ae9_log_state(struct creative_chip *chip, const char *when)
{
	dev_info(chip->dev,
		 "state %s: BAR2 0x320=0x%04x 0x304=0x%02x 0x20c=0x%08x 0x4=0x%08x 0x1c=0x%08x 0xc0c=0x%02x 0xc14=0x%02x\n",
		 when, creative_ext_readw(chip, 0x320),
		 readb(chip->ext_regs + 0x304),
		 creative_ext_readl(chip, 0x20c), creative_ext_readl(chip, 0x4),
		 creative_ext_readl(chip, 0x1c),
		 creative_ext_readl(chip, 0xc0c) & 0xff,
		 creative_ext_readl(chip, 0xc14) & 0xff);
}

/* The secondary bus reset hits everything behind the bridge: only do it
 * when the card (any of its functions) is alone there, as on the AE-9's
 * own PCIe-to-PCI bridge. */
/*
 * The PCIe-to-PCI bridge above the card (Tundra Tsi381 here, ASMedia
 * ASM1083/1085 1b21:1080 on other AE-9s). An ASMedia card drops off the
 * bus during bring-up (bugzilla 219552 #81, stock driver too, #56), so log
 * the bridge's status at each step: a DMA or config cycle the card
 * botched shows up as master/target abort or SERR on the secondary side.
 * The error bits are write-1-to-clear and cleared after each read, so
 * every line shows only what happened since the one before.
 */
/*
 * dma_pad: spare space after every DMA buffer, in KB. The ASMedia ASM1083
 * reads ahead past the end of a buffer (bugzilla 219552: IOMMU faults one
 * byte past the 256 KB firmware buffer). -1 = auto: 64 KB behind an
 * ASMedia bridge, none otherwise.
 */
static int dma_pad = -1;

/* posted_flush: -1 = auto (on behind an ASMedia bridge), 0 = off, 1 = on;
 * see creative_ae9.h */
bool ae9_posted_flush;
static int posted_flush = -1;
module_param(posted_flush, int, 0444);
MODULE_PARM_DESC(posted_flush, "Read back after every register write, as Windows: -1 = auto (ASMedia bridge, default), 0 = off, 1 = on");
module_param(dma_pad, int, 0444);
MODULE_PARM_DESC(dma_pad, "Spare KB after every DMA buffer (-1=auto: 64 behind an ASMedia bridge, else 0; default)");

static void creative_ae9_bridge_log(struct creative_chip *chip, const char *step)
{
	struct pci_dev *br = chip->pci->bus->self;
	u16 st = 0, sec = 0, ctl = 0, cmd = 0;
	u32 aer = 0;
	bool has_aer;
	const u16 errs = PCI_STATUS_DETECTED_PARITY | PCI_STATUS_SIG_SYSTEM_ERROR |
			 PCI_STATUS_REC_MASTER_ABORT | PCI_STATUS_REC_TARGET_ABORT |
			 PCI_STATUS_SIG_TARGET_ABORT | PCI_STATUS_PARITY;

	if (!br)
		return;
	pci_read_config_word(br, PCI_STATUS, &st);
	pci_read_config_word(br, PCI_SEC_STATUS, &sec);
	pci_read_config_word(br, PCI_BRIDGE_CONTROL, &ctl);
	pci_read_config_word(chip->pci, PCI_COMMAND, &cmd);
	has_aer = ae9_bridge_aer_sec(br, &aer);
	dev_info(chip->dev,
		 "bridge %s %04x:%04x %s: status 0x%04x, secondary 0x%04x%s%s%s%s%s, control 0x%04x%s; card command 0x%04x%s\n",
		 pci_name(br), br->vendor, br->device, step, st, sec,
		 sec & PCI_STATUS_REC_MASTER_ABORT ? " <MAbort" : "",
		 sec & PCI_STATUS_REC_TARGET_ABORT ? " <TAbort" : "",
		 sec & PCI_STATUS_SIG_TARGET_ABORT ? " >TAbort" : "",
		 sec & PCI_STATUS_SIG_SYSTEM_ERROR ? " <SERR" : "",
		 sec & (PCI_STATUS_DETECTED_PARITY | PCI_STATUS_PARITY) ? " parity" : "",
		 ctl, ctl & AE9_BRIDGE_CTL_DISCARD_STAT ? " <discard timer expired" : "", cmd,
		 has_aer && aer != U32_MAX && (aer & AE9_AER_SEC_DISCARD) ?
		 "; AER: secondary discard timer expired" : "");
	/*
	 * The whole AER secondary status: sticky, and left to the kernel's AER
	 * driver, so a bit set before the load (an AE-9's bridge had received
	 * master abort, 0x00000008, already set) would repeat at every step.
	 * Logged at the first reading that has a bit other than the discard
	 * timer's (that one has its own words above), then only when it
	 * changes, with what it was.
	 */
	if (has_aer && (chip->br_aer_seen ? aer != chip->br_aer : aer & ~AE9_AER_SEC_DISCARD)) {
		char bits[160];

		ae9_aer_sec_decode(aer, bits, sizeof(bits));
		if (chip->br_aer_seen)
			dev_info(chip->dev, "bridge %s %s: AER secondary uncorrectable status now 0x%08x (%s), was 0x%08x\n",
				 pci_name(br), step, aer, aer ? bits : "clear", chip->br_aer);
		else
			dev_info(chip->dev, "bridge %s %s: AER secondary uncorrectable status 0x%08x (%s; sticky: logged again only when it changes)\n",
				 pci_name(br), step, aer, bits);
	}
	if (has_aer) {
		chip->br_aer = aer;
		chip->br_aer_seen = true;
	}
	if (st & errs)
		pci_write_config_word(br, PCI_STATUS, st & errs);
	if (sec & errs)
		pci_write_config_word(br, PCI_SEC_STATUS, sec & errs);
	/*
	 * DISCARD_STAT cleared like the status bits above, so each line shows
	 * only what happened since the one before: written back with the rest
	 * of the register as read (nothing else changes; bit 6, the bus reset,
	 * reads 0 here), and only when set, so a normal load writes nothing.
	 * Never when the read came back all-ones (bridge unreachable): that
	 * would set the bus-reset bit and every enable; bit 6 masked anyway.
	 * The AER status is left alone: the kernel's AER driver owns it.
	 */
	if (ctl != 0xffff && (ctl & AE9_BRIDGE_CTL_DISCARD_STAT)) {
		if (chip->br_discards < U8_MAX)
			chip->br_discards++;
		pci_write_config_word(br, PCI_BRIDGE_CONTROL, ctl & ~PCI_BRIDGE_CTL_BUS_RESET);
	}
}

static bool creative_ae9_alone_on_bus(struct pci_dev *pdev)
{
	struct pci_dev *d;

	list_for_each_entry(d, &pdev->bus->devices, bus_list)
		if (PCI_SLOT(d->devfn) != PCI_SLOT(pdev->devfn))
			return false;
	return true;
}

/*
 * Before the secondary bus reset: the Tsi381 manual (10.1.2) wants no
 * request pending when the reset bit is set (pcm.c,
 * creative_ae9_dma_stop_all()). At the first probe nothing of ours has run
 * (no rings yet) and nothing is read: the card's first access stays
 * Windows' (the state log, then the BAR2 init). On a bring-up retry or a
 * resume something may run, unless the shutdown before it left the
 * controller in reset.
 */
static void ae9_bus_reset_quiesce(struct creative_chip *chip)
{
	unsigned int st;

	if (!chip->corb_buf && !chip->corb_keep)
		return;
	st = creative_ae9_dma_stop_all(chip);
	if (st)
		dev_info(chip->dev, "before the bus reset: DMA stopped (streams 0x%x%s%s)\n",
			 st & 0xffff, st & BIT(16) ? ", CORB" : "", st & BIT(17) ? ", RIRB" : "");
}

/*
 * "Card alive": after the bus reset (the AE-7: none by default, so at the
 * start of its bring-up), before the BAR2 init, the card's
 * read-only ID registers (BAR2 is a Synopsys DesignWare APB cluster, notes
 * 31): 0x8fc the DAC I2C block's IC_COMP_TYPE, "DW" + 0x0140 (DW_apb_i2c;
 * its version would be COMP_VERSION at 0x8f8, not read); 0xcfc the head-unit UART's CTR, 0x44570110 (DW_apb_uart); 0x4
 * the board ID, 0x7172 in the high half on the AE-9, its low byte the
 * board revision (0x0a, 0x0b on an ASMedia-bridged card). No side effects.
 * The I2C ID decides: read up to 10 times 10 ms apart until it's right; a
 * card whose BAR2 reads all-ones is off the bus (as creative_ae9_gone()),
 * one with another value isn't driven (-EIO: no BAR2 writes to registers
 * that aren't the ones we know). The UART and board IDs are logged.
 */
#define AE9_BAR2_I2C_COMP_TYPE	0x8fc
#define AE9_BAR2_UART_CTR	0xcfc
#define AE9_BAR2_BOARD_ID	0x004
#define AE9_DW_I2C_ID		0x44570140
#define AE9_DW_UART_ID		0x44570110
#define AE9_BOARD_ID_AE9	0x7172

static int ae9_bar2_alive(struct creative_chip *chip)
{
	u32 *id = chip->bar2_id;
	unsigned int tries = 1;

	id[0] = creative_ext_readl(chip, AE9_BAR2_I2C_COMP_TYPE);
	while (id[0] != AE9_DW_I2C_ID && tries < 10) {
		msleep(10);
		id[0] = creative_ext_readl(chip, AE9_BAR2_I2C_COMP_TYPE);
		tries++;
	}
	id[1] = creative_ext_readl(chip, AE9_BAR2_UART_CTR);
	id[2] = creative_ext_readl(chip, AE9_BAR2_BOARD_ID);
	if (id[0] == ~0U && id[1] == ~0U && id[2] == ~0U) {
		chip->card_gone = true;
		creative_ae9_dead_notify(chip);
		/* a board with its own ID (the AE-7) does no bus reset by default */
		if (chip->board && chip->board->bar2_id != AE9_BOARD_ID_AE9) {
			dev_err(chip->dev, "card dropped off the bus (BAR2 ID check at bring-up): its registers read all-ones %u times; stopping\n",
				tries);
			return -ENODEV;
		}
		dev_err(chip->dev, "card dropped off the bus (BAR2 ID check after the bus reset): its registers read all-ones %u times; stopping\n",
			tries);
		return -ENODEV;
	}
	if (id[0] != AE9_DW_I2C_ID) {
		dev_err(chip->dev, "BAR2 ID check: 0x8fc = 0x%08x %u times, not the DAC I2C block's 0x%08x (0xcfc = 0x%08x, 0x4 = 0x%08x): the card's registers don't read right; not driving it\n",
			id[0], tries, AE9_DW_I2C_ID, id[1], id[2]);
		return -EIO;
	}
	dev_info(chip->dev, "BAR2 alive: 0x8fc = 0x%08x (DW_apb_i2c)%s, 0xcfc = 0x%08x, board ID 0x4 = 0x%08x (revision 0x%02x)\n",
		 id[0], tries > 1 ? " after a wait" : "", id[1], id[2], id[2] & 0xff);
	if (id[1] != AE9_DW_UART_ID)
		dev_warn(chip->dev, "BAR2 ID check: 0xcfc = 0x%08x, not the head-unit UART's 0x%08x\n",
			 id[1], AE9_DW_UART_ID);
	/*
	 * Another board's own ID (the AE-7's 0x0081....): its bringup_check
	 * gated on it before the bus reset; here it is only confirmed. The
	 * I2C and UART IDs above are the DesignWare blocks' constants, the
	 * same on every board with them (the AE-7's Windows start writes both
	 * blocks).
	 */
	if (chip->board && chip->board->bar2_id != AE9_BOARD_ID_AE9) {
		if (id[2] >> 16 != chip->board->bar2_id)
			dev_warn(chip->dev, "BAR2 ID check: board ID 0x%08x, not the %s's (0x%04x....)\n",
				 id[2], chip->board->name, chip->board->bar2_id);
		return 0;
	}
	if (id[2] >> 16 != AE9_BOARD_ID_AE9)
		dev_warn(chip->dev, "BAR2 ID check: board ID 0x%08x, not an AE-9's (0x%04x....)\n",
			 id[2], AE9_BOARD_ID_AE9);
	return 0;
}

static int creative_ae9_hw_bringup(struct creative_chip *chip)
{
	struct pci_dev *pdev = chip->pci;
	bool do_reset = bus_reset != 0;
	int ret, fret;

	if (ae9_has_op(chip, bringup_check)) {
		ret = chip->board->ops->bringup_check(chip, &do_reset);
		if (ret < 0)
			return ret;
	}
	creative_ae9_log_state(chip, "before bring-up");
	chip->card_gone = false;
	chip->card_hung = false;
	chip->corb_to_run = 0;
	chip->hu_unattached = false;
	chip->hu_reattach_try = 0;
	chip->hu_amp_since = 0;		/* the set-up sends the jack frame */
	chip->hu_amp_fix = false;
	chip->hu_amp_try = 0;
	chip->hu_att_tick = 0;		/* a marker check after it says */
	chip->hu_quiet = false;
	chip->hu_dark_polls = 0;
	creative_ae9_bridge_log(chip, "before bring-up");

	if (do_reset && pdev->bus->self && !creative_ae9_alone_on_bus(pdev)) {
		dev_warn(&pdev->dev, "other devices behind %s: skipping the secondary bus reset\n",
			 pci_name(pdev->bus->self));
	} else if (do_reset && pdev->bus->self) {
		struct pci_dev *bridge = pdev->bus->self;
		int rret;

		/*
		 * pci_reset_bus() would take the device lock we already hold
		 * during probe; toggle the bridge's secondary bus reset
		 * directly and restore our config space around it.
		 */
		u32 id = ~0U;
		int waited;

		ae9_bus_reset_quiesce(chip);
		pci_save_state(pdev);
		/*
		 * The PCI core's own bus resets (pci_bus_lock()) hold the config
		 * access lock of the bridge and of every device below it:
		 * userspace config reads (lspci, sysfs "config") wait instead
		 * of reading a device in reset. Since 6.10 the reset warns
		 * "unlocked secondary bus reset" without the bridge's. The same
		 * here, the card's until its config space is back. Only that
		 * lock: the core's device locks would mean taking the bridge's
		 * while probe holds ours, the core's order reversed. Kernel
		 * config accessors (the wait and restore below) don't wait on it.
		 */
		pci_cfg_access_lock(bridge);
		pci_cfg_access_lock(pdev);
		rret = pci_bridge_secondary_bus_reset(bridge);
		/*
		 * A conventional PCI device may need up to 1 s after reset
		 * before it responds properly (the helper doesn't wait for a
		 * PCIe-to-PCI bridge's secondary bus). Without this the BAR2
		 * init and codec reads that follow went nowhere.
		 */
		msleep(1000);
		for (waited = 1000; waited < 3000; waited += 50) {
			pci_read_config_dword(pdev, PCI_VENDOR_ID, &id);
			if (id != ~0U && id != 0)
				break;
			msleep(50);
		}
		pci_restore_state(pdev);
		pci_cfg_access_unlock(pdev);
		pci_cfg_access_unlock(bridge);
		pci_set_master(pdev);
		dev_info(&pdev->dev, "secondary bus reset via %s: %d, ready after %d ms (id 0x%08x)\n",
			 pci_name(bridge), rret, waited, id);
		creative_ae9_log_state(chip, "after bus reset");
	}

	/*
	 * PCI config register 0x40 on the Creative controller reads 0x8080
	 * after a cold power-on; the Windows driver leaves it at 0x0080 (bit 15
	 * cleared). Seen on both the ASMedia-bridge AE-9 (kernel bugzilla
	 * 219552 #63) and this Tsi381-bridge card (dxrw/PCI-11020071.rw). Our VFIO
	 * captures never traced config-space writes, so this was missing from
	 * every replay. Cleared here, at every bring-up.
	 */
	{
		u16 cfg40, after;

		pci_read_config_word(pdev, 0x40, &cfg40);
		if (cfg40 & 0x8000)
			pci_write_config_word(pdev, 0x40, cfg40 & ~0x8000);
		pci_read_config_word(pdev, 0x40, &after);
		dev_info(&pdev->dev, "PCI cfg 0x40: 0x%04x -> 0x%04x\n",
			 cfg40, after);
	}

	creative_ae9_bridge_log(chip, "after bus reset");
	if (creative_ae9_gone(chip, "after the bus reset"))
		return -ENODEV;
	ret = ae9_bar2_alive(chip);
	if (ret < 0)
		return ret;

	/* Windows' BAR2 init, before the controller reset as in its cold boot */
	creative_ae9_bar2_init_windows(chip);

	/* Reset HDA controller */
	ret = creative_ae9_hw_reset(chip);
	creative_ae9_bridge_log(chip, "after controller reset");
	if (creative_ae9_gone(chip, "after the controller reset"))
		return -ENODEV;
	if (ret < 0)
		return ret;

	/* Enable unsolicited responses */
	creative_hda_writel(chip, AZX_REG_GCTL, AZX_GCTL_RESET | AZX_GCTL_UNSOL);
	chip->unsol_parked = false;
	msleep(CREATIVE_CODEC_PROBE_DELAY_MS);

	/* CORBSIZE init: bits 3-2 = 10 (256 entries), preserve cap bits 1-0 */
	creative_hda_writeb(chip, AZX_REG_CORBSIZE, 0x0a);

	/* ICW init + codec detect BEFORE CORB engine start.
	 * Starting the CORB engine (corb_init) before ICW causes the
	 * immediate-command BUSY bit to hang (IRS=0x5) — the hardware
	 * command bus gets stuck when CORB DMA is active. Do ICW first,
	 * then start CORB+RIRB for runtime verb submission.
	 *
	 * RINTCNT and RIRBCTL are initialised in corb_init, after all ICW.
	 */
	creative_hda_writeb(chip, AZX_REG_IRS, 0x5);
	creative_hda_writeb(chip, AZX_REG_IRS, 0x0);
	creative_hda_writeb(chip, AZX_REG_IRS, 0x4);
	chip->last_irs_ack = 0x6;

	/* Codec detect via ICW */
	ret = creative_ae9_detect_codecs(chip);
	if (ret < 0)
		return ret;
	/*
	 * Codec 1 (the CA0132 with the DSP) silent after the bus reset: every
	 * later command would time out, ~2-3 min of them, holding up the
	 * boot (2026-11-04, after warm reboots, card lights off). Stop here;
	 * the retries cost a few seconds. Once (15:19) codec 1 answered on
	 * the second attempt, but on the dark boot of 18:22 nothing short of
	 * a PSU power-off brought it back (bus resets, reloads 16 min later,
	 * the full old bring-up with codec1_failfast=0, an option removed in
	 * 0.12.12).
	 */
	if (!(chip->codec_ok_mask & BIT(1))) {
		dev_warn(chip->dev, "codec 1 (DSP) not answering after the bus reset; trying the bring-up again\n");
		return 0;
	}
	if (ae9_has_op(chip, codecs_check)) {
		ret = chip->board->ops->codecs_check(chip);
		if (ret < 0)
			return ret;
	}

	/*
	 * DSP standup, in the order the CA0132 requires:
	 *   1. download firmware (DSP must be running before its config takes)
	 *   2. replay the captured codec init verbs (now applied to live DSP)
	 *   3. start the DSP internal audio-router streams
	 *   4. route DSP output to line-out
	 */
	/* Chip init BEFORE firmware (replay sets up the DSP clocks/
	 * state the transfer needs; firmware-first fails -EIO).
	 * Done at probe — the DSP is freshly booted here and responds
	 * to ChipIO (at +20s it has gone idle and reads 0).
	 *
	 * 0x800000-0x8000fc, the firmware's SCP queue pointers, reads
	 * 0x0bad0add here (on the card too, not only under QEMU). It is
	 * left alone, as Windows does; the old code zeroed it here and
	 * after the boot (sentinel_clear, removed in 0.12.12; see
	 * WINDOWS-CAPTURE-ANALYSIS.md sections 10-11). The read stays, so
	 * the start-up sends the same verbs as before.
	 */
	if (ae9_has(chip, AE9_CAP_AE9_DIAG)) {
		unsigned int sentinel = 0;

		creative_dsp_chipio_read(chip, 0x800000, &sentinel);
	}

	/*
	 * Windows (cold capture) does 4 ICW reads after the first controller
	 * reset, then resets the controller a second time, starts the
	 * CORB/RIRB, sets GCTL = 0x101 (unsolicited responses on) and sends
	 * every other pre-download verb through the CORB. We do the same
	 * before the pre-download replay ("win_reset2" in the log: the
	 * option that switched it, removed in 0.12.12).
	 */
	{
		int rret = creative_ae9_hw_reset(chip);
		int cret = 0;

		creative_ae9_bridge_log(chip, "after second controller reset");
		if (creative_ae9_gone(chip, "after the second controller reset"))
			return -ENODEV;
		creative_hda_writel(chip, AZX_REG_GCTL,
				    AZX_GCTL_RESET | AZX_GCTL_UNSOL);
		chip->unsol_parked = false;
		cret = creative_ae9_corb_init(chip);
		creative_ae9_bridge_log(chip, "after CORB/RIRB start");
		dev_info(chip->dev,
			 "win_reset2: second controller reset %d, corb_init %d\n",
			 rret, cret);
		if (chip->card_gone)
			return -ENODEV;
	}

	creative_ae9_predownload(chip);
	if (creative_ae9_gone(chip, "after the pre-download replay"))
		return -ENODEV;

	/* Check 0x801c after replay_init — in Windows capture the
	 * audio loop starts during or right after init verbs via ICW.
	 */
	if (ae9_has(chip, AE9_CAP_AE9_DIAG)) {
		unsigned int loop_val = 0;
		creative_dsp_chipio_read(chip, 0x801c, &loop_val);
		dev_info(chip->dev,
			 "0x801c after replay_init: 0x%x%s\n",
			 loop_val,
			 loop_val ? " <-- AUDIO LOOP ALIVE!" : " (dead)");
	}

	creative_ae9_fx_dsp_reset(chip);	/* fresh DSP: upload latch, first-failure log */
	chip->cv.mon_used = false;	/* fresh DSP: the mic tap route is fresh too */
	fret = creative_dsp_load_firmware(chip);
	creative_ae9_bridge_log(chip, "after firmware download");
	dev_info(chip->dev, "DSP firmware load: %d\n", fret);
	if (creative_ae9_gone(chip, "during the firmware download"))
		return -ENODEV;
	if (fret == 0) {
		creative_ae9_post_fw(chip);

		/* Windows' post-boot replay (in post_fw) has set up the
		 * head unit and DAC; our old setup (DSP SPI 0x30 re-init,
		 * standup, CA0113 handshake, amp/route) is gone: it broke
		 * the head-unit link */
		chip->dsp_done = 1;
		/*
		 * MCU is confirmed up (HELLO ACK'd). Schedule ACM init
		 * with a short delay so "AE-9" appears on the display
		 * shortly after the relay click / "-HP-", not 12s later.
		 * The 12s default delay was for cold MCU boot; since
		 * audio_loop_start confirmed the MCU is alive, we only
		 * need ~500ms for the 8051 to settle after HELLO.
		 */
		if (ae9_has(chip, AE9_CAP_HEAD_UNIT))
			queue_delayed_work(chip->wq, &chip->acm_work,
					   msecs_to_jiffies(500));
	}
	/*
	 * Capture-mined fact: the host driver owns the head-unit I2C
	 * the ENTIRE time — there is no handoff to the DSP 8051. The
	 * real driver runs the head-unit ACM init *after* firmware
	 * stream setup, then a ~500ms heartbeat that continues during
	 * audio playback. So we DO drive the head unit with firmware,
	 * via the same deferred ACM work scheduled below.
	 */

	/* Stream enable */
	/* post_windows: the replay already set these up as Windows does;
	 * the DSP I2C init's CMD=0x30 re-init cuts off the head unit */
	if (!chip->dsp_done && ae9_has(chip, AE9_CAP_HEAD_UNIT)) {
		dev_info(chip->dev, "init: stream enable\n");
		writeb(0x3f, chip->ext_regs + CREATIVE_REG_STREAM_ENABLE);
		wmb();
	}

	/* DSP I2C controller init */
	if (!chip->dsp_done && ae9_has(chip, AE9_CAP_HEAD_UNIT)) {
		dev_info(chip->dev, "init: DSP I2C\n");
		creative_i2c_dsp_init(chip);
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* PCI probe							  */
/* ------------------------------------------------------------------ */
/*
 * Bring-up with recovery: if the DSP misses its boot handshake, run
 * Windows' shutdown and try again (as a manual reload does), up to
 * AE9_RESUME_TRIES times. Only for the post_windows path with firmware.
 */
static void creative_ae9_win_shutdown(struct creative_chip *chip, const char *why);
static void creative_ae9_stop_works(struct creative_chip *chip);
static void creative_ae9_set_unloading(struct creative_chip *chip);

static int creative_ae9_hw_bringup_retry(struct creative_chip *chip, const char *what)
{
	unsigned int attempt, tries = AE9_RESUME_TRIES;
	int ret = 0;

	for (attempt = 1; attempt <= tries; attempt++) {
		if (attempt > 1) {
			cancel_delayed_work_sync(&chip->acm_work);
			/* nothing to shut down if codec 1 never answered */
			if (chip->codec_ok_mask & BIT(1))
				creative_ae9_win_shutdown(chip, "bring-up retry");
			msleep(AE9_RESUME_DELAY_MS);
			chip->dsp_done = 0;
			chip->hs_done = 0;
			chip->hs_ok = false;
			atomic_set(&chip->scp_unsol, 0);
			chip->scp_unsol_done = 0;
			creative_dsp_chipio_addr_invalidate();
			dev_info(chip->dev, "%s: bring-up attempt %u\n", what, attempt);
		}
		chip->resuming = attempt < tries;	/* fail fast unless last */
		ret = creative_ae9_hw_bringup(chip);
		chip->resuming = false;
		if (ret < 0 || tries == 1 || (chip->dsp_done && chip->hs_ok))
			break;
		dev_warn(chip->dev, "%s: bring-up attempt %u failed (dsp_done %u, handshake %s)\n",
			 what, attempt, chip->dsp_done, chip->hs_ok ? "ok" : "failed");
	}
	if (chip->card_gone)
		dev_err(chip->dev, "%s: the card dropped off the bus; nothing more to try. Power off at the PSU before the next attempt (logs welcome: bugzilla 219552)\n",
			what);
	else if (!(chip->codec_ok_mask & BIT(1)))
		dev_err(chip->dev, "%s: codec 1 (DSP) never answered in %u attempts: power off at the PSU for ~20 s\n",
			what, tries);
	return ret;
}

static void creative_ae9_destroy_wq(void *wq)
{
	destroy_workqueue(wq);
}

static int creative_ae9_probe(struct pci_dev *pdev,
			      const struct pci_device_id *ent)
{
	const struct creative_ae9_bar_layout *layout;
	struct creative_chip *chip;
	int ret;

	/* the AE-9 only, before anything is enabled or mapped */
	layout = ae9_board_lookup(pdev);
	if (!layout)
		return -ENODEV;

	ret = pcim_enable_device(pdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "pci_enable_device failed: %d\n", ret);
		return ret;
	}

	pci_set_master(pdev);

	if (verb_log && !vlog_buf) {
		vlog_buf = vzalloc(array_size(verb_log, sizeof(*vlog_buf)));
		atomic_set(&vlog_n, 0);
		dev_info(&pdev->dev, "verb_log: %s (%u entries)\n",
			 vlog_buf ? "on" : "allocation failed", verb_log);
	}

	/*
	 * 32-bit DMA mask, as stock CTHDA (AZX_DCAPS_NO_64BIT): GCAP 0x6400
	 * has 64OK clear, and Windows ran every CORB, RIRB and BDL (DSP
	 * loads included) at ~0x7e99xxxx with a zero upper word (30
	 * captures, 8664 address writes). Card-tested 2026-11-17 at bus
	 * addresses 0xfffff000 (IOMMU) and in passthrough. Until 0.12.13
	 * this was 30-bit, from an unproven "may" comment; without IOMMU
	 * translation that confined every buffer to the lowest 1 GB, with
	 * the 16 MB DMA zone as the fallback.
	 */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret < 0) {
		dev_err(&pdev->dev, "No suitable DMA mask available\n");
		return ret;
	}
	{
		struct iommu_domain *dom = iommu_get_domain_for_dev(&pdev->dev);

		dev_info(&pdev->dev, "DMA: 32-bit mask, %s\n",
			 !dom ? "no IOMMU translation" :
			 dom->type == IOMMU_DOMAIN_IDENTITY ?
			 "IOMMU in passthrough (physical addresses)" : "through the IOMMU");
	}

	ret = pcim_iomap_regions(pdev, BIT(layout->hda_bar) | BIT(layout->ext_bar),
				 CREATIVE_AE9_DRV_NAME);
	if (ret < 0) {
		dev_err(&pdev->dev, "BAR mapping failed: %d\n", ret);
		return ret;
	}

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->board = pdev->subsystem_device == CREATIVE_SUBSYS_AE7 ? &ae9_board_ae7 :
		      &ae9_board_ae9;
	chip->r51_sel = -1;	/* no router-chip parameter selected yet */
	chip->rirb_send_wp = -1;
	chip->rirb_wp0 = -1;
	chip->rirb_held = -1;

	chip->pci = pdev;
	chip->dev = &pdev->dev;
	if (pdev->bus->self && pdev->bus->self->vendor == PCI_VENDOR_ID_ASMEDIA &&
	    pdev->bus->self->device == 0x1080)
		chip->asmedia = true;
	chip->dma_pad = (dma_pad < 0 ? (chip->asmedia ? 64 : 0) : dma_pad) * 1024;
	ae9_posted_flush = posted_flush < 0 ? chip->asmedia : !!posted_flush;
	if (ae9_posted_flush)
		dev_info(&pdev->dev, "posted_flush: every register write is read back (%s)\n",
			 posted_flush < 0 ? "ASMedia bridge" : "posted_flush=1");
	if (chip->asmedia || chip->dma_pad)
		dev_info(&pdev->dev, "bridge %s%s: %zu KB spare after each DMA buffer\n",
			 pdev->bus->self ? pci_name(pdev->bus->self) : "-",
			 chip->asmedia ? " (ASMedia ASM1083/1085)" : "",
			 chip->dma_pad / 1024);
	chip->hu_phantom_req = -1;	/* nothing to send */
	/* before the bring-up: its post-boot replay sends "AE-9" only with the
	 * display on (set later, in hu_controls_add, it blanked "AE-9" at
	 * every load: 0.12.8) */
	chip->hu_display = true;
	chip->hu_display_to = -1;
	chip->hda_regs = pcim_iomap_table(pdev)[layout->hda_bar];
	chip->ext_regs = pcim_iomap_table(pdev)[layout->ext_bar];
	vlog_bar0 = chip->hda_regs;
	vlog_bar2 = chip->ext_regs;
	spin_lock_init(&chip->lock);
	mutex_init(&chip->corb_mutex);
	mutex_init(&chip->r51_mutex);
	mutex_init(&chip->scp_mutex);
	mutex_init(&chip->replay_mutex);
	mutex_init(&chip->ca0113_mutex);
	mutex_init(&chip->spdif_mutex);
	mutex_init(&chip->dsd_mutex);
	init_waitqueue_head(&chip->dsd_wq);
	chip->spdif_aes[0] = IEC958_AES0_CON_NOT_COPYRIGHT;	/* as 0x70d COPY */
	chip->dac_filter = DAC_FILTER_DEFAULT_INDEX;
	chip->dac_filter_bits = dac_filter_values[DAC_FILTER_DEFAULT_INDEX];

	pci_set_drvdata(pdev, chip);

	chip->wq = alloc_workqueue("creative-ae9", WQ_UNBOUND, 0);
	if (!chip->wq)
		return -ENOMEM;
	ret = devm_add_action_or_reset(&pdev->dev, creative_ae9_destroy_wq, chip->wq);
	if (ret)
		return ret;

	/*
	 * Initialise all delayed-work items HERE, immediately after chip
	 * allocation, before ANY code path that might queue one.
	 *
	 * Previously these were at the bottom of probe (after the firmware block).
	 * The firmware path calls queue_delayed_work(chip->wq, &chip->acm_work, 500ms)
	 * inside audio_loop_start/standup, then probe fell through to
	 * INIT_DELAYED_WORK which re-initialised (and silently CANCELLED) the
	 * already-queued timer. The ACM init never ran — no AE-9 display,
	 * no relay click, no audio. Moving INIT here fixes the race.
	 */
	INIT_DELAYED_WORK(&chip->dsp_switch_work, creative_ae9_dsp_switch_work);
	INIT_WORK(&chip->pb_restart_work, creative_ae9_pb_restart_work);
	INIT_DELAYED_WORK(&chip->hp_work, creative_ae9_fx_hp_work);
	INIT_DELAYED_WORK(&chip->spk_follow_work, creative_ae9_fx_spk_follow_work);
	INIT_DELAYED_WORK(&chip->acm_work, creative_ae9_acm_work);
	INIT_DELAYED_WORK(&chip->hb_work, creative_ae9_hb_work);
	INIT_DELAYED_WORK(&chip->dsd_mute_work, creative_ae9_dsd_mute_work);
	INIT_WORK(&chip->resume_work, creative_ae9_resume_work);
	INIT_WORK(&chip->dead_work, creative_ae9_dead_work);
	atomic_set(&chip->dma_running, 0);

	dev_info(chip->dev,
		 "BAR%d (HDA)  at 0x%p, BAR%d (ext) at 0x%p\n",
		 layout->hda_bar, chip->hda_regs,
		 layout->ext_bar, chip->ext_regs);

	ret = creative_ae9_hw_bringup_retry(chip, "probe");
	if (ret < 0)
		goto err_hw;
	/* a DSP that never answered its boot handshake isn't a usable card */
	if (!(chip->dsp_done && chip->hs_ok)) {
		dev_err(chip->dev, "DSP did not start after %u attempt(s); not registering the card (reload the driver or power-cycle)\n",
			AE9_RESUME_TRIES);
		ret = -EIO;
		goto err_hw;
	}

	chip->acm_settled = false;

	ret = snd_card_new(chip->dev, SNDRV_DEFAULT_IDX1,
			   SNDRV_DEFAULT_STR1, THIS_MODULE, 0, &chip->card);
	if (ret < 0) {
		dev_err(chip->dev, "snd_card_new failed: %d\n", ret);
		goto err_hw;
	}

	strscpy(chip->card->driver, CREATIVE_AE9_DRV_NAME,
		sizeof(chip->card->driver));
	/* the board ae9_board_lookup() let through (an AE-7 only with ae7=1) */
	strscpy(chip->card->shortname, chip->board->name, sizeof(chip->card->shortname));
	snprintf(chip->card->longname, sizeof(chip->card->longname),
		 "Creative Labs %s at 0x%llx irq %d", chip->board->name,
		 (unsigned long long)pci_resource_start(pdev, 0), pdev->irq);

	/* PCM playback device (no interrupts — Windows-style) */
	ret = creative_ae9_pcm_init(chip);
	if (ret < 0)
		dev_warn(chip->dev, "PCM init failed: %d\n", ret);

	creative_ae9_hu_controls_add(chip);
	chip->mic_boost = 1;
	if (ae9_has(chip, AE9_CAP_INPUTS))
		snd_ctl_add(chip->card, snd_ctl_new1(&mic_boost_control, chip));
	chip->mic_level = 99;	/* +9 dB, as Windows' capture blocks */
	if (ae9_has(chip, AE9_CAP_INPUTS))
		snd_ctl_add(chip->card, snd_ctl_new1(&mic_level_control, chip));
	if (ae9_has(chip, AE9_CAP_ES9038))
		snd_ctl_add(chip->card, snd_ctl_new1(&dac_filter_control, chip));
	creative_ae9_fx_init(chip);
	if (ae9_has(chip, AE9_CAP_SBX))
		creative_ae9_fx_controls_add(chip);
	chip->cv.sent_voicefx = chip->cv.sent_miceq = -1;
	if (ae9_has(chip, AE9_CAP_CV))
		creative_ae9_cv_controls_add(chip);

	/* IRQ before the card goes public: playback/capture need period
	 * interrupts, so a failure is fatal; sync_irq lets ALSA synchronise
	 * with the handler when a stream closes */
	ret = devm_request_irq(&pdev->dev, pdev->irq, creative_ae9_irq_handler,
			       IRQF_SHARED, CREATIVE_AE9_DRV_NAME, chip);
	if (ret < 0) {
		dev_err(chip->dev, "IRQ %d request failed: %d\n", pdev->irq, ret);
		goto err_card;
	}
	chip->card->sync_irq = pdev->irq;
	dev_info(chip->dev, "IRQ %d registered\n", pdev->irq);

	if (!chip->corb_buf && !chip->card_gone) {
		ret = creative_ae9_corb_init(chip);
		if (ret < 0) {
			dev_err(chip->dev, "CORB init failed: %d\n", ret);
			goto err_card;
		}
	}
	if (chip->card_gone) {
		ret = -ENODEV;
		goto err_card;
	}

	AE9_CALL(chip, probe_done, chip);
	ret = snd_ctl_add(chip->card, snd_ctl_new1(&card_model_control, chip));
	if (ret < 0)
		dev_warn(chip->dev, "Card Model control not added (%d)\n", ret);
	ret = snd_card_register(chip->card);
	if (ret < 0) {
		dev_err(chip->dev, "snd_card_register failed: %d\n", ret);
		goto err_card;
	}

	g_dbg_chip = chip;
	chip->dbg_dir = debugfs_create_dir("creative_ae9", NULL);
	debugfs_create_file("cmd", 0600, chip->dbg_dir, NULL,
			    &creative_dbg_fops);
	debugfs_create_file("state", 0400, chip->dbg_dir, NULL,
			    &creative_dbg_state_fops);
	creative_ae9_pcm_debugfs(chip);	/* "readahead" (pcm.c) */
	if (vlog_buf)
		debugfs_create_file("verblog", 0400, chip->dbg_dir, NULL,
				    &vlog_seq_fops);

	dev_info(chip->dev, "Init complete: card %s\n", chip->card->longname);
	return 0;

err_card:
	creative_ae9_set_unloading(chip);
	creative_ae9_fx_remove(chip);
	/* works first: dead_work walks the PCMs snd_card_free frees */
	creative_ae9_stop_works(chip);
	snd_card_free(chip->card);
	chip->card = NULL;
	chip->answer_ctl = NULL;
	chip->health_ctl = NULL;
	chip->fx.nkc = 0;
err_hw:
	creative_ae9_set_unloading(chip);
	/* works the bring-up scheduled (ACM/heartbeat) must not outlive chip */
	creative_ae9_stop_works(chip);
	if (chip->corb_buf && !chip->card_gone)
		creative_ae9_win_shutdown(chip, "probe failure");
	else
		creative_ae9_corb_free(chip);
	creative_ae9_stop_works(chip);	/* anything win_shutdown queued */
	chip->suspended = true;		/* IRQ handler: hands off */
	return ret;
}

static irqreturn_t creative_ae9_irq_handler(int irq, void *dev_id)
{
	struct creative_chip *chip = dev_id;
	u32 intsts;
	u8 rirbsts;

	if (chip->suspended)
		return IRQ_NONE;
	intsts = readl(chip->hda_regs + AZX_REG_INTSTS);

	if (!intsts || intsts == 0xffffffff)
		return IRQ_NONE;

	/* RIRB interrupt */
	if (intsts & AZX_INT_CTRL_EN) {
		rirbsts = readb(chip->hda_regs + AZX_REG_RIRBSTS);
		if (rirbsts & RIRB_INT_RESPONSE) {
			/*
			 * Clear the status only. The verb code reads the RIRB
			 * itself (it waits for solicited responses and counts
			 * the DSP's unsolicited "SCP reply ready" entries);
			 * draining it here lost those, which broke the DSP
			 * boot handshake on resume (at probe the IRQ isn't
			 * registered yet, so it never showed).
			 */
			writeb(rirbsts | RIRB_INT_RESPONSE,
			       chip->hda_regs + AZX_REG_RIRBSTS);
			wmb();
			AE9_CALL(chip, rirb_irq, chip);
		}
		if (rirbsts & RIRB_INT_OVERRUN) {
			/* Clear overrun, then resync RIRB */
			writeb(rirbsts | RIRB_INT_OVERRUN,
			       chip->hda_regs + AZX_REG_RIRBSTS);
			wmb();
		}
	}

	/* input streams: 0 capture (XLR), 1 optical in, 2 What U Hear,
	 * 3 rear RCA in */
	if (intsts & 0x1)
		creative_ae9_cap_irq(chip);
	if (intsts & 0x2)
		creative_ae9_xcap_irq(chip, 0);
	if (intsts & 0x4)
		creative_ae9_xcap_irq(chip, 1);
	if (intsts & 0x8)
		creative_ae9_xcap_irq(chip, 2);

	/* separate optical playback: stream descriptor 6 (0x140, bit 6) */
	if (intsts & BIT(6))
		creative_ae9_opt_irq(chip);

	/*
	 * Main playback: its own descriptor's bit only (0x100 = bit 4). This
	 * used to take any output-stream bit as the main stream's, which the
	 * optical stream's interrupts would have hit too. The firmware and
	 * headphone-profile transfers (SD4/SD5) set no INTCTL bit.
	 */
	if (intsts & BIT((chip->playback_stream_base - 0x80) / 0x20)) {
		u8 sd_sts = readb(chip->hda_regs +
				  chip->playback_stream_base + AZX_REG_SD_STS);
		if (sd_sts & SD_INT_COMPLETE) {
			/* W1C: clear completion interrupt */
			writeb(SD_INT_COMPLETE, chip->hda_regs +
			       chip->playback_stream_base + AZX_REG_SD_STS);
			wmb();
			/* Notify ALSA that a period has completed */
			if (chip->playback_substream)
				snd_pcm_period_elapsed(chip->playback_substream);
		}
		/* Clear any error bits */
		if (sd_sts & (SD_INT_FIFO_ERR | SD_INT_DESC_ERR)) {
			writeb(sd_sts & SD_INT_MASK, chip->hda_regs +
			       chip->playback_stream_base + AZX_REG_SD_STS);
			wmb();
		}
	}

	return IRQ_HANDLED;
}

/* --------------------------------------------------------- */
/* PCI remove							  */
/* ------------------------------------------------------------------ */
/*
 * Windows' shutdown (both cold-boot captures end with it): head-unit
 * power-off frame, ChipIO / head-unit DAC / GPIO power-down, codec D3
 * verbs, then CORB/RIRB off, interrupts off and the controller held in
 * reset. Replaces our older sequence, which left the DSP in a state the
 * next bring-up couldn't recover from without a power cut.
 */
/*
 * Before a power-down or a sleep: stop what we started in the 8051, as the
 * streams' own stop paths would. Its routes survive a warm reboot (and the
 * 8051 its record of them): a reboot with Direct Mode off and a recording
 * through the DSP left a scrambled router on the next boot (2026-11-04).
 */
static void creative_ae9_streams_down(struct creative_chip *chip)
{
	if (!chip->corb_buf || !chip->dsp_done)
		return;
	AE9_HOOK(chip, streams_down, chip);
	/* a recording, or the mic monitor's hold of ADC 0x07 (no recording) */
	if (chip->cap_start_sent || chip->cv.mon_bound) {
		creative_ae9_capture_seq(chip, false);
		chip->cap_start_sent = 0;
		chip->cv.mon_bound = false;
	}
	creative_ae9_cv_down(chip);	/* Voice Clarity's route and mic connections */
	if (chip->dsp_active)
		creative_ae9_dsp_mode(chip, false);	/* streams 0x18, 0x05 off */
	else
		ae9_stream18_drain(chip, "shutdown");	/* Windows: drained at device stop */
}

static void creative_ae9_win_shutdown(struct creative_chip *chip, const char *why)
{
	u32 ctl;

	/*
	 * Marked "stopped answering" (corb.c): ask once before skipping the
	 * power-down. A card that answers again (a test's hung_inject, or a
	 * hang that cleared) still gets its full power-down: unloaded without
	 * it, with streams running (uv60: 5 open for AudioMon's meters), the
	 * next load found the DSP busy for good, until a power-off.
	 */
	if (chip->card_hung && !chip->card_gone && chip->corb_buf) {
		u32 id = 0;

		chip->card_hung = false;
		chip->corb_to_run = 0;
		if (creative_ae9_corb_send_verb_sync(chip, 0x100f0000, &id) == 0 &&
		    id == 0x11020011) {
			dev_info(chip->dev, "shutdown (%s): the card answers again; normal power-down\n", why);
		} else {
			chip->card_hung = true;
			dev_warn(chip->dev, "shutdown (%s): the card still doesn't answer; power-down skipped\n", why);
		}
	}
	creative_ae9_streams_down(chip);
	dev_info(chip->dev, "shutdown (%s): Windows power-down sequence\n", why);
	/* stop capture (SD0) and playback (SD4) DMA */
	ctl = readl(chip->hda_regs + 0x80 + AZX_REG_SD_CTL);
	writel((ctl & ~SD_CTL_DMA_START) & SD_CTL_WMASK, chip->hda_regs + 0x80 + AZX_REG_SD_CTL);
	ctl = readl(chip->hda_regs + 0x100 + AZX_REG_SD_CTL);
	writel((ctl & ~SD_CTL_DMA_START) & SD_CTL_WMASK, chip->hda_regs + 0x100 + AZX_REG_SD_CTL);

	if (chip->corb_buf) {
		/* the AFGs go to D3cold (705 p=4) only at power-off; Windows
		 * uses D3hot (p=3) for a reboot, and so do we for an unload
		 * and the bring-up retries (CtxHda: the system power action) */
		bool cold = system_state == SYSTEM_POWER_OFF;

		/* our moved DSP -> DAC routes: the router would keep them
		 * through the next load */
		ae9_rt_dsp_dac_off(chip, false, "shutdown");
		/* Windows' shutdown (creative_ae9_seq_run.c) */
		creative_ae9_power_down(chip, cold);
	} else {
		dev_info(chip->dev, "shutdown: command ring not running, skipping the sequence\n");
	}
	writel(0x40000000, chip->hda_regs + AZX_REG_INTCTL);
	creative_ae9_corb_free(chip);
	writel(0x100, chip->hda_regs + AZX_REG_GCTL);	/* controller in reset */
	chip->dsp_done = 0;
	chip->dsp_active = false;
	dev_info(chip->dev, "shutdown (%s): done\n", why);
}

/*
 * From here on no "card stopped answering" handling queues dead_work or
 * notifies the card (ae9_corb_hung_check, dead_notify): the PCMs and the
 * card are freed before win_shutdown's verbs, which still run with
 * dsp_done set (ultrareview 2, #1). Under corb_mutex, so it is ordered
 * against a hung check in a verb that is running now.
 */
static void creative_ae9_set_unloading(struct creative_chip *chip)
{
	mutex_lock(&chip->corb_mutex);
	chip->unloading = true;
	mutex_unlock(&chip->corb_mutex);
}

/*
 * The timed works, producers before what they queue (ultrareview 2): acm_work
 * starts hb_work; dsp_switch_work and spk_follow_work re-send the effects,
 * which can queue hp_work. Cancelled the other way round, a running producer
 * left its consumer pending after the cancel.
 */
static void ae9_cancel_timed_works(struct creative_chip *chip)
{
	cancel_delayed_work_sync(&chip->acm_work);
	cancel_delayed_work_sync(&chip->hb_work);
	cancel_delayed_work_sync(&chip->dsp_switch_work);
	cancel_delayed_work_sync(&chip->spk_follow_work);
	cancel_delayed_work_sync(&chip->hp_work);
	cancel_delayed_work_sync(&chip->dsd_mute_work);	/* queued by the trigger */
	AE9_CALL(chip, cancel_works, chip);
}

static void creative_ae9_stop_works(struct creative_chip *chip)
{
	cancel_work_sync(&chip->resume_work);
	cancel_work_sync(&chip->dead_work);
	cancel_work_sync(&chip->pb_restart_work);
	atomic_set(&chip->dma_running, 0);
	ae9_cancel_timed_works(chip);
}

/* Reboot / power-off: same as Windows shutting down. */
static void creative_ae9_shutdown(struct pci_dev *pdev)
{
	struct creative_chip *chip = pci_get_drvdata(pdev);

	if (!chip || chip->suspended)
		return;
	creative_ae9_set_unloading(chip);
	creative_ae9_stop_works(chip);
	creative_ae9_dsd_abort(chip, "reboot/power-off");	/* native DSD (pcm_dsd) off */
	chip->suspended = true;		/* IRQ handler: hands off */
	creative_ae9_win_shutdown(chip, "reboot/power-off");
}

static void creative_ae9_remove(struct pci_dev *pdev)
{
	struct creative_chip *chip = pci_get_drvdata(pdev);

	if (!chip)
		return;

	creative_ae9_set_unloading(chip);
	/* fx_remove first: after it an hp_custom write can't queue hp_work */
	creative_ae9_fx_remove(chip);
	creative_ae9_stop_works(chip);
	debugfs_remove_recursive(chip->dbg_dir);
	g_dbg_chip = NULL;
	vlog_bar0 = vlog_bar2 = NULL;
	vfree(vlog_buf);
	vlog_buf = NULL;

	/* card first: it closes open streams while the hardware still runs */
	if (chip->card) {
		/* native DSD (pcm_dsd) off while the CORB still runs */
		if (!chip->suspended)
			creative_ae9_dsd_abort(chip, "unload");
		snd_card_free(chip->card);
		chip->card = NULL;
		chip->answer_ctl = NULL;	/* freed with the card */
		chip->health_ctl = NULL;
		chip->fx.nkc = 0;		/* its kcontrols too */
	}
	/* anything an ALSA callback re-armed before the card's disconnect
	 * (an unbind with streams open): not into win_shutdown */
	creative_ae9_stop_works(chip);

	/* Windows' shutdown */
	if (!chip->suspended)
		creative_ae9_win_shutdown(chip, "unload");

	/* and anything win_shutdown's own effect re-sends queued */
	creative_ae9_stop_works(chip);
	creative_ae9_corb_free(chip);
	chip->suspended = true;		/* IRQ handler: hands off */
	pci_clear_master(pdev);
}

/* ------------------------------------------------------------------ */
/* PCI driver structure						  */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* Suspend / resume							  */
/* ------------------------------------------------------------------ */
/*
 * The card loses everything in suspend (firmware, DSP state, head-unit
 * setup), so resume runs the whole hardware bring-up again, from a work
 * item so it doesn't hold up the system resume (firmware load, ~10 s).
 * Streams are suspended by the ALSA core and re-prepared by applications;
 * the head unit gets its output, volume, mute and knob light back.
 */
static int creative_ae9_suspend(struct device *dev)
{
	struct creative_chip *chip = dev_get_drvdata(dev);

	if (!chip)
		return 0;
	cancel_work_sync(&chip->resume_work);
	cancel_work_sync(&chip->dead_work);
	snd_power_change_state(chip->card, SNDRV_CTL_POWER_D3hot);
	/* native DSD (pcm_dsd) off: the stream is suspended already; the
	 * app's prepare after the resume brings DSD up again */
	creative_ae9_dsd_abort(chip, "suspend");
	atomic_set(&chip->dma_running, 0);
	ae9_cancel_timed_works(chip);
	creative_ae9_streams_down(chip);
	/* streams_down re-sends the effects, which can queue hp_work */
	cancel_delayed_work_sync(&chip->hp_work);
	chip->dsp_active = false;
	AE9_CALL(chip, quiesce, chip);
	chip->suspended = true;
	writel(0, chip->hda_regs + AZX_REG_INTCTL);
	creative_ae9_corb_free(chip);
	/* GPIO back to its cold power-on value (speakers mode leaves 0x103,
	 * headphones 0x003); in case the card keeps it through the sleep */
	creative_ae9_log_state(chip, "at suspend");
	creative_ext_writew(chip, 0x320, 0x0000);
	chip->dsp_done = 0;
	chip->hs_done = 0;
	chip->standup_done = 0;
	dev_info(chip->dev, "suspend: output %s, volume att 0x%02x%s\n",
		 chip->hu_output ? "headphones" : "speakers", chip->hu_att,
		 chip->hu_mute ? ", muted" : "");
	return 0;
}

static void creative_ae9_resume_work(struct work_struct *work)
{
	struct creative_chip *chip =
		container_of(work, struct creative_chip, resume_work);
	unsigned int want_hp, attempt;
	unsigned long flags;
	int ret = -EIO;

	chip->dac_start_key = 0;	/* the card lost power */
	chip->spdif_out_sent = 0;	/* and the boot replay reset 0x05 */
	chip->cv.route_on = false;	/* the bring-up came back with the plain route */
	chip->cv.sent_voicefx = chip->cv.sent_miceq = -1;	/* and the DSP's defaults */
	chip->cv.mon_bound = false;	/* and ADC 0x07 unbound */

	/* the boot replay brings the head unit up on speakers; the heartbeat
	 * then switches to the output picked last (a switch still pending
	 * at suspend included: its volume waits in hu_att_out[]) */
	spin_lock_irqsave(&chip->lock, flags);
	want_hp = chip->hu_switch_to >= 0 ? chip->hu_switch_to : chip->hu_att_for;
	chip->hu_switch_to = -1;
	creative_ae9_hu_att_swap(chip, 0);
	chip->hu_output = 0;
	spin_unlock_irqrestore(&chip->lock, flags);
	chip->lo_virt_phys = false;

	for (attempt = 1; attempt <= AE9_RESUME_TRIES; attempt++) {
		if (attempt > 1) {
			/* undo the failed attempt with Windows' shutdown: a
			 * reload (shutdown + bring-up) recovers a DSP that
			 * missed its handshake; a bare second bring-up didn't */
			cancel_delayed_work_sync(&chip->acm_work);
			cancel_delayed_work_sync(&chip->hb_work);
			chip->suspended = true;
			creative_ae9_win_shutdown(chip, "resume retry");
		}
		msleep(AE9_RESUME_DELAY_MS);
		dev_info(chip->dev, "resume: bringing the card back up (attempt %u)\n",
			 attempt);
		chip->dsp_done = 0;
		chip->hs_done = 0;
		chip->hs_ok = false;
		chip->standup_done = 0;
		chip->pcm_start_sent = 0;
		chip->cap_start_sent = 0;
		chip->dsp_primed = false;
		atomic_set(&chip->scp_unsol, 0);
		chip->scp_unsol_done = 0;
		creative_dsp_chipio_addr_invalidate();
		chip->hu_ticks = 0;
		chip->hu_name_until = HU_SP_TICKS;
		chip->hu_btn_down = 0;
		chip->hu_next_full = jiffies;
		chip->suspended = false;
		chip->resuming = true;

		ret = creative_ae9_hw_bringup(chip);
		chip->resuming = false;
		if (ret == 0 && chip->dsp_done && chip->hs_ok)
			break;
		dev_warn(chip->dev, "resume: attempt %u failed (ret %d, dsp_done %u, handshake %s)\n",
			 attempt, ret, chip->dsp_done, chip->hs_ok ? "ok" : "failed");
	}
	if (!(ret == 0 && chip->dsp_done && chip->hs_ok)) {
		dev_err(chip->dev, "resume: card did not come back; a reload or cold boot may be needed\n");
		snd_power_change_state(chip->card, SNDRV_CTL_POWER_D0);
		return;
	}
	AE9_HOOK(chip, resumed, chip);

	/* the heartbeat (started by the bring-up's ACM work) applies these */
	chip->hu_dirty = 1;
	if (want_hp) {
		spin_lock_irqsave(&chip->lock, flags);
		if (chip->hu_switch_to < 0)	/* none picked since */
			chip->hu_switch_to = 1;
		spin_unlock_irqrestore(&chip->lock, flags);
	}
	/* the head unit wakes lit; if the light was off, the heartbeat turns
	 * it off again (sending it from here raced the heartbeat's frames) */
	chip->hu_light_sent = 1;
	chip->hu_sbx_led = -1;		/* heartbeat sends it again */
	creative_ae9_mic_boost_apply(chip);
	if (chip->mic_level != 99)
		creative_ae9_mic_level_apply(chip);
	creative_ae9_dac_filter_apply(chip);
	/* Voice Clarity and the mic monitor: the bring-up reset the DSP to
	 * its defaults, so only now (before it, dsp_done was 0 and nothing
	 * was sent) */
	if (chip->cv.on)
		creative_ae9_cv_apply(chip);
	if (chip->cv.monitor)
		creative_ae9_mic_monitor_apply(chip);
	creative_ae9_fx_defer(chip);

	snd_power_change_state(chip->card, SNDRV_CTL_POWER_D0);
	dev_info(chip->dev, "resume: done after %u attempt(s), restoring output %s\n",
		 attempt, want_hp ? "headphones" : "speakers");
}

static int creative_ae9_resume(struct device *dev)
{
	struct creative_chip *chip = dev_get_drvdata(dev);

	if (chip)
		queue_work(chip->wq, &chip->resume_work);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(creative_ae9_pm, creative_ae9_suspend,
				creative_ae9_resume);

static struct pci_driver creative_ae9_driver = {
	.name		= CREATIVE_AE9_DRV_NAME,
	.id_table	= creative_ae9_pci_tbl,
	.probe		= creative_ae9_probe,
	.remove		= creative_ae9_remove,
	.shutdown	= creative_ae9_shutdown,
	.driver.pm	= pm_sleep_ptr(&creative_ae9_pm),
};

module_pci_driver(creative_ae9_driver);

/* the AE-7 (experimental): its core part, on this file's helpers */
#include "creative_ae7_core.c"
