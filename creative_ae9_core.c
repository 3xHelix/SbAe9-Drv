// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 3xHelix
// Contains code adapted from the Linux kernel's sound/pci/hda/patch_ca0132.c:
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
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/tlv.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/hda_register.h>

#include "creative_ae9.h"
#include "creative_ae9_init_verbs.h"
#include "creative_ae9_cold_verbs.h"
#include "creative_ae9_cap_verbs.h"
#include "creative_ae9_switch_ops.h"
#include "creative_ae9_warm_verbs.h"

MODULE_AUTHOR("3xHelix");
MODULE_DESCRIPTION(CREATIVE_AE9_DRV_DESC);
MODULE_LICENSE("GPL");
MODULE_VERSION(CREATIVE_AE9_DRV_VERSION);

/* seconds after a bring-up during which phantom power "on" is refused */
#define AE9_PHANTOM_HOLD_S	30

/*
 * enable_pcm: register the PCM playback device. No HDA interrupts are used
 * (Windows runs playback with global interrupts disabled), avoiding the
 * level-triggered INTx storm that previously hard-locked the system.
 */
static unsigned int enable_pcm = 1;
module_param(enable_pcm, uint, 0444);
MODULE_PARM_DESC(enable_pcm, "Register PCM playback device (1=on, default)");

/*
 * load_fw: download the DSP firmware (ctefx-desktop.bin) during probe and
 * stand up the DSP (firmware -> codec verb replay -> DSP audio streams ->
 * output routing), in the order the DSP requires.
 */
static unsigned int load_fw = 1;
module_param(load_fw, uint, 0444);
MODULE_PARM_DESC(load_fw, "Download DSP firmware + stand up DSP at probe (1=on, default)");

/*
 * After the init passes, poll the head unit every acm_heartbeat_ms to keep
 * the MCU/DAC alive (capture: ~500ms, runs during audio playback). 0=off.
 */
static unsigned int acm_heartbeat_ms = 500;
module_param(acm_heartbeat_ms, uint, 0444);
MODULE_PARM_DESC(acm_heartbeat_ms, "Head-unit heartbeat poll interval (ms, 0=off)");

/*
 * PCI config register 0x40 on the Creative controller reads 0x8080 after a
 * cold power-on; the Windows driver leaves it at 0x0080 (bit 15 cleared).
 * Seen on both the ASMedia-bridge AE-9 (kernel bugzilla 219552 #63) and this
 * IDT-bridge card (dxrw/PCI-11020071.rw). Our VFIO captures never traced
 * config-space writes, so this was missing from every replay.
 */
static unsigned int pcicfg40_fix = 1;
module_param(pcicfg40_fix, uint, 0444);
MODULE_PARM_DESC(pcicfg40_fix, "PCI config reg 0x40 bit 15: 1=clear at probe, 2=set at probe and clear after codec detection, 0=leave");

/*
 * Split replay: ae9_initseq_codec1 is Windows' codec-1 boot stream, including
 * its firmware-download procedure, the DSP start (DBGCNTL run) and the
 * MASTERCONTROL boot handshake. Replaying it whole before our own download
 * starts the DSP with no firmware, so only the pre-download
 * part is replayed at probe/self-reset; after our firmware is running we
 * run the boot handshake properly (polling the DSP's replies) and then
 * replay Windows' post-handshake setup (the table tail). See
 * WINDOWS-CAPTURE-ANALYSIS.md, sections 1 and 4.
 */

/*
 * Windows reads the codec subsystem IDs ~0.5 ms after the controller reset
 * and gets 0x11020071/72; we read them 200 ms later and get 0x11020010.
 * codec_probe_delay_ms sets our delay; the IDs are read again 200 ms later
 * to show whether they change after reset.
 */
static unsigned int codec_probe_delay_ms = CREATIVE_CODEC_PROBE_DELAY_MS;
module_param(codec_probe_delay_ms, uint, 0444);
MODULE_PARM_DESC(codec_probe_delay_ms, "Delay between controller reset and codec detection, ms (default 200)");

/*
 * bus_reset: secondary bus reset of the card (through its Tsi381 bridge)
 * at probe. Verified 2026-11-01: after a bus reset the codecs report
 * subsystem 0x11020071/0x11020072, as under Windows (where starting the VM
 * bus-resets the card); after only a power-on reset, or a PM (D3hot) reset,
 * they report the generic 0x11020010.
 */
static int bus_reset = -1;
module_param(bus_reset, int, 0444);
MODULE_PARM_DESC(bus_reset, "Secondary bus reset of the card at probe, as a VM start does (1=on, 0=off, -1=auto: on, except behind an ASMedia bridge; default)");

/*
 * win_reset2: Windows (cold capture) does 4 ICW reads after the first
 * controller reset, then resets the controller a second time, starts the
 * CORB/RIRB, sets GCTL = 0x101 (unsolicited responses on) and sends every
 * other pre-download verb through the CORB. With win_reset2=1 we do the
 * same before the pre-download replay.
 */
static unsigned int win_reset2 = 1;
module_param(win_reset2, uint, 0444);
MODULE_PARM_DESC(win_reset2, "Second controller reset + CORB start before the pre-download replay, as Windows does (1=on, default)");

/* bring-up attempts if the DSP misses its handshake (probe and resume),
 * and the wait before each resume attempt / between retries */
static unsigned int resume_delay_ms = 2000;
module_param(resume_delay_ms, uint, 0644);
MODULE_PARM_DESC(resume_delay_ms, "Wait before each resume bring-up attempt and between retries (ms)");
static unsigned int resume_tries = 3;
module_param(resume_tries, uint, 0644);
MODULE_PARM_DESC(resume_tries, "Bring-up attempts (probe and resume) if the DSP doesn't answer");

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
struct vlog_ent { u32 val; u32 ex; };
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

	if (e->ex & BIT(30)) {
		unsigned int sz = 1u << ((e->ex >> 26) & 3);
		unsigned int rg = (e->ex & BIT(24)) ? 2 : 0;

		if (e->ex & BIT(29))
			seq_printf(m, "%lld vfio_region_write  (0000:06:00.0:region%u+0x%x, 0x%x, %u)\n",
				   i, rg, e->ex & 0xffff, e->val, sz);
		else
			seq_printf(m, "%lld vfio_region_read  (0000:06:00.0:region%u+0x%x, %u) = 0x%x\n",
				   i, rg, e->ex & 0xffff, sz, e->val);
	} else if (e->ex & 0x80000000)
		seq_printf(m, "%lld RIRB idx=  0 resp=0x%08x ex=0x%08x\n",
			   i, e->val, e->ex & 0x7fffffff);
	else
		seq_printf(m, "%lld CORB idx=  0 verb=0x%08x\n", i, e->val);
	return 0;
}

static const struct seq_operations vlog_seq_sops = {
	.start = vlog_seq_start,
	.next = vlog_seq_next,
	.stop = vlog_seq_stop,
	.show = vlog_seq_show,
};
DEFINE_SEQ_ATTRIBUTE(vlog_seq);

/* ae9_initseq_codec1 section boundaries (table indices) */
#define AE9_INITSEQ1_DL_START	292	/* {0x15, 0x200, 0x47}: download stream bind */
#define AE9_INITSEQ1_TAIL	1334	/* {0x15, 0x00e, 0x34}: first verb after the handshake */

/* ------------------------------------------------------------------ */
/* PCI device ID table						  */
/* ------------------------------------------------------------------ */
/*
 * AE-9 only: 1102:0010 with subsystem 1102:0071. The same device ID is used
 * by the AE-7 and AE-5 (other subsystem IDs); they must not get the AE-9
 * bus reset and Windows replay. Other Creative device IDs aren't claimed.
 */
static const struct pci_device_id creative_ae9_pci_tbl[] = {
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_CREATIVE, DEVICE_AE7_AE9,
			 PCI_VENDOR_ID_CREATIVE, CREATIVE_SUBSYS_AE9),
	  .driver_data = DEVICE_AE7_AE9 },
	{ }
};
MODULE_DEVICE_TABLE(pci, creative_ae9_pci_tbl);

/* Per-device BAR layout data */
static const struct creative_ae9_bar_layout creative_ae9_bar_layouts[] = {
	[DEVICE_AE7_AE9] = { .hda_bar = 0, .ext_bar = 2, .ext_min_size = 0x4000 },
	[DEVICE_RECON3D] = { .hda_bar = 0, .ext_bar = 2, .ext_min_size = 0x4000 },
	[DEVICE_AE9_0013] = { .hda_bar = 0, .ext_bar = 1, .ext_min_size = 0x20000 },
	[DEVICE_AE9_0014] = { .hda_bar = 0, .ext_bar = 1, .ext_min_size = 0x20000 },
};

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

	if (chip->card_gone)
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

	if (pcicfg40_fix == 2) {
		struct pci_dev *pdev = to_pci_dev(chip->dev);
		u16 v;

		pci_read_config_word(pdev, 0x40, &v);
		pci_write_config_word(pdev, 0x40, v & ~0x8000);
		dev_info(chip->dev, "PCI cfg 0x40: bit 15 cleared after codec detection (0x%04x)\n",
			 v & ~0x8000);
	}

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

/* ------------------------------------------------------------------ */
/* Replay the captured Windows codec init verb sequence via ICW.	  */
/* Faithful ordered replay (incl. the node 0x15 DSP byte streams).	  */
/* No DMA — safe. GET-equivalent vendor verbs just return state.	  */
/* ------------------------------------------------------------------ */
static void creative_ae9_replay_init(struct creative_chip *chip,
				     unsigned int cad,
				     const struct ae9_initverb *seq,
				     size_t count)
{
	size_t i;
	unsigned int errs = 0, skipped = 0;

	dev_info(chip->dev, "Replaying %zu init verbs to codec %u\n",
		 count, cad);

	for (i = 0; i < count; i++) {
		u32 resp = 0;
		int ret;

		/* Previously skipped nid=0x15 verb=0x100/0x702 thinking they
		 * flooded the SCP queue, but those verbs are ChipIO ADDRESS_HIGH
		 * and HIC_POST_READ — essential for completing ChipIO address/
		 * read transactions.  Skipping them broke ChipIO state during
		 * replay_init.  The actual SCP queue writes use nid=0x16.
		 * No skip needed here.
		 */

		ret = creative_ae9_icw_send_verb(chip,
			creative_ae9_verb(cad, seq[i].nid, seq[i].verb,
					  seq[i].payload), &resp);
		if (ret < 0)
			errs++;
		udelay(40);
	}

	dev_info(chip->dev,
		 "Codec %u init replay done (%u verb errors, %u SCP verbs skipped)\n",
		 cad, errs, skipped);
}

/* fw_split is only safe if the table still has the expected layout. */
static bool creative_ae9_fw_split_ok(struct creative_chip *chip)
{
	const struct ae9_initverb *dl = &ae9_initseq_codec1[AE9_INITSEQ1_DL_START];
	const struct ae9_initverb *tl = &ae9_initseq_codec1[AE9_INITSEQ1_TAIL];
	static bool warned;

	if (ARRAY_SIZE(ae9_initseq_codec1) > AE9_INITSEQ1_TAIL &&
	    dl->nid == 0x15 && dl->verb == 0x200 && dl->payload == 0x47 &&
	    tl->nid == 0x15 && tl->verb == 0x00e && tl->payload == 0x34)
		return true;
	if (!warned)
		dev_warn(chip->dev, "fw_split: init table layout changed, split disabled\n");
	warned = true;
	return false;
}

/* Number of codec-1 table entries to replay before our firmware download. */
static size_t creative_ae9_codec1_prefw_count(struct creative_chip *chip)
{
	return creative_ae9_fw_split_ok(chip) ? AE9_INITSEQ1_DL_START :
						ARRAY_SIZE(ae9_initseq_codec1);
}

/*
 * Replay a block of captured CORB verbs (full 32-bit, codec address kept).
 * Codec-1 nid 0x16 verbs are SCP traffic: each message is rebuilt from its
 * low/high word verbs and sent through creative_dsp_scp_send() (status
 * waits, announced replies); the recorded reply pops (0x702/0x704) and
 * status/read polls (0xFxx) are skipped. atomic: plain CORB sends only.
 */
static void creative_ae9_verb_block_replay(struct creative_chip *chip,
					   const u32 *t, size_t n, bool atomic,
					   const char *what)
{
	unsigned int errs = 0, nw = 0, msgs = 0, msg_err = 0;
	u32 words[1 + 32];
	u16 lo = 0;
	size_t i;

	/* whole blocks take turns: they hold multi-verb ChipIO accesses
	 * (address, then data) that another block must not split */
	if (!atomic)
		mutex_lock(&chip->replay_mutex);
	for (i = 0; i < n; i++) {
		u32 resp = 0, v = t[i];

		if (atomic) {
			creative_ae9_corb_send_verb(chip, v);
			continue;
		}
		if ((v >> 28) == 1 && ((v >> 20) & 0xff) == 0x16) {
			unsigned int cls = (v >> 16) & 0xf;
			u16 p16 = v & 0xffff;

			if (cls == 0x0) {
				lo = p16;
			} else if (cls == 0x1) {
				unsigned int size;

				words[nw++] = ((u32)p16 << 16) | lo;
				size = (words[0] >> 27) & 0x1f;
				if (nw == size + 1 || nw == ARRAY_SIZE(words)) {
					if (creative_dsp_scp_send(chip, words[0],
								  &words[1], nw - 1) < 0)
						msg_err++;
					msgs++;
					nw = 0;
				}
			}
			continue;
		}
		if (creative_ae9_corb_send_verb_sync(chip, v, &resp) < 0)
			errs++;
		/*
		 * Stream stop (ChipIO param 0x1c = 0): the 8051 tears the stream's
		 * routes down after it answers. Windows waits 48 ms here (capture
		 * 6, seq 9954 -> 9956) and 144 ms on the Direct Mode on stop
		 * (11808); going straight on left random router layouts
		 * (2026-11-04: no DSP input, half inputs, outputs to 0xff).
		 */
		if (v == 0x1157101c)
			msleep(50);
	}
	creative_dsp_chipio_addr_invalidate();
	if (!atomic) {
		mutex_unlock(&chip->replay_mutex);
		/* every block at startup; afterwards only failures (debug=1: all) */
		if (errs || msg_err || !chip->dsp_done)
			dev_info(chip->dev, "%s, %zu verbs (%u errors), %u SCP msgs (%u failed)\n",
				 what, n, errs, msgs, msg_err);
		else
			ae9_dbg(chip, "%s, %zu verbs (%u errors), %u SCP msgs (%u failed)\n",
				what, n, errs, msgs, msg_err);
	}
}

/*
 * Windows' playback start (prepare) or stop (trigger, atomic) block.
 * The start block and its CA0113 commands come from a speaker-mode
 * capture: they set ChipIO 0x0d = 0xa4, CA0113 48/1d = 0x40, 48/19 = 0 and
 * mic boost +10 dB. Patch those to the current output (headphones: 0xa1,
 * 0x80, 1, as in Windows' switch) and boost, or every new stream would
 * undo a headphone switch.
 */
/*
 * Per-rate playback start (pcm_rates=1), as Windows does it (capture
 * 2026-11-03, vm_capture_coldboot_20261103_192357; WINDOWS-CAPTURE-ANALYSIS
 * §36). The block was captured at 48 kHz:
 *
 * 48 kHz family (48/96/192/384): patch connection point 0xd0's rate code,
 * node 0x02's converter format and the I2S clock descriptors
 * (0x189000/04/08, 0x189024), and write port remaps 0x190060 + 4k =
 * 0x00014000 | k << 8 | k for k < 2 x the rate multiple (the block has
 * k = 0, 1) before the 0x19042c trigger. 384 kHz runs the link at 192 kHz
 * with 4 channels (two stereo samples per frame) and sets DAC 48/0a = 0x12.
 *
 * 44.1 kHz family (44.1/88.2): no re-clock. DSP stream 0x05 goes to
 * connection point 0x91 (the card's sample-rate converter) at the stream's
 * rate code, the converter is set up (0x18a020/24/28 and its PLL/PMU
 * clock: 0x01 = 0x17, 0x40 = 0x06, 0x04 = 0x80), and the block's
 * ASI 8 ... 0x19042c section is left out, as in Windows.
 *
 * Windows never writes BAR2 0x100, connection point 0x70 or the full PLL
 * program for a rate change.
 */
enum { AE9_ROUTE_DIRECT, AE9_ROUTE_SRC };
static const struct {
	unsigned int rate;
	u8 route;
	u8 cp_code;	/* connection point rate code (ca0132's enum) */
	u32 i2s_clk;	/* 0x189000/04/08 (direct route) */
	u32 i2s_rate;	/* 0x189024 (direct route) */
	u8 ports;	/* port remaps (direct route) */
	u8 dac0a;	/* DAC 48/0a */
	u32 src[5];	/* converter: 0x18a020, 0x18a020, 0x18a024, 0x18a028, 0x18a020 */
} ae9_pcm_rates[] = {
	{  44100, AE9_ROUTE_SRC,    0x08, 0, 0, 0, 0x02,
	   { 0x02, 0x02, 0x00101204, 0xfb, 0x03 } },
	{  48000, AE9_ROUTE_DIRECT, 0x09, 0x0001f100, 0x00008005,  2, 0x02 },
	{  88200, AE9_ROUTE_SRC,    0x0a, 0, 0, 0, 0x02,
	   { 0x02, 0x42, 0x08101204, 0x7b, 0x43 } },
	{  96000, AE9_ROUTE_DIRECT, 0x0b, 0x0001f101, 0x00014004,  4, 0x02 },
	/* 176.4 kHz through the converter, from Windows' converter table
	 * (CtxHda.sys 6.0.105.0065: 0x18a020 k = 2, 0x0c101204 / 0x3b);
	 * rate code 0x0d = ca0132's SR_176_400. Windows never played it in
	 * our captures. */
	{ 176400, AE9_ROUTE_SRC,    0x0d, 0, 0, 0, 0x02,
	   { 0x02, 0x82, 0x0c101204, 0x3b, 0x83 } },
	{ 192000, AE9_ROUTE_DIRECT, 0x0e, 0x0001f102, 0x00022003,  8, 0x02 },
	{ 384000, AE9_ROUTE_DIRECT, 0x0e, 0x0001f103, 0x00031002, 16, 0x12 },
};

/* One ChipIO memory write in the block's verb encoding. */
static size_t ae9_chipio_w(u32 *t, size_t n, u32 addr, u32 data)
{
	t[n++] = 0x11500000 | (addr & 0xffff);
	t[n++] = 0x11510000 | (addr >> 16);
	t[n++] = 0x11530000 | (data & 0xffff);
	t[n++] = 0x11540000 | (data >> 16);
	return n;
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
 * direct again (Windows was silent until stop/play too).
 */
/*
 * The three sequences below are Windows' raw verbs (capture 6,
 * vm_capture_coldboot_20261103_233115), status reads included: ChipIO
 * takes a parameter only after a status read (0x115f0100 / 0x115f0300, and
 * the 8051 reads 0x70d/0x70e/0xf07). Sent without them, as an earlier
 * version did, the routing parameters are dropped and the audio keeps the
 * direct path (2026-11-04: no DSP stream or route change had any effect).
 */

/*
 * The audio router (ChipIO 0x190000..; ca0132.c chipio_remap_stream): one
 * entry per 48 kHz channel, bits 17:16 state (1 active, 3 via the sample
 * rate converter), 15:8 destination port, 7:0 source port. Ports: 0x00-0x1f
 * HDA streams, 0x80-0xbf SRC, 0xc0-0xdf DSP DMA (assigned by the 8051),
 * 0xe0-0xff DAC/ADC; 0x40/0x41 are the front (ES9038) outputs. Windows'
 * Direct Mode start writes 0x190060/64 = HDA 0x00/0x01 -> 0x40/0x41, a
 * manual overwrite that keeps feeding the front DAC straight from the bus
 * whatever the DSP streams do. With Direct Mode off the 8051 has already
 * built the DSP path (HDA -> SRC -> DSP DMA in, DSP DMA out -> 0xe0/0xe1 at
 * 0x190048/4c, the processed front pair), so 0x40/0x41 have to take the
 * DSP's processed output instead (found 2026-11-04: effects and EQ became
 * audible the moment these two entries changed).
 */
#define AE9_RT_FRONT_L		0x190060
#define AE9_RT_FRONT_R		0x190064
#define AE9_RT_APPLY		0x19042c

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
MODULE_PARM_DESC(surround_ports, "5.1 in DSP mode: router targets for DSP outputs c2,c3,c4,c5 (default 0x50,0x51,0x60,0x61: center, LFE, rear L, rear R)");

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

static int ae9_rt_free_slot(const u32 *e, int near)
{
	unsigned int i;

	if (near > 0 && !e[near - 1])
		return near - 1;
	if (near >= 0 && near + 1 < AE9_RT_ENTRIES && !e[near + 1])
		return near + 1;
	for (i = 0x0c; i < AE9_RT_ENTRIES; i++)	/* past 0x190030 */
		if (!e[i] && 0x190000 + 4 * i != AE9_RT_FRONT_L &&
		    0x190000 + 4 * i != AE9_RT_FRONT_R)
			return i;
	return -1;
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
static void creative_ae9_router_front_dsp(struct creative_chip *chip, bool judge)
{
	int l = -1, r = -1, el = -1, er = -1, inputs = 0, ins = 0;
	int feed_at[2] = { -1, -1 }, src_to_dsp[2] = { 0, 0 };
	static const u8 hda_src[2] = { 0x90, 0x80 };	/* HDA 0x00, 0x01 */
	u32 e[AE9_RT_ENTRIES];
	unsigned int i, n = 0, ff, seen;
	unsigned int unsteady = 0, retried = 0, failed = 0;

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
	/* the feed repair is for the stereo layout (00 -> 90, 01 -> 80); in
	 * the 6-ch one those SRCs carry HDA 04/05 */
	if (n == AE9_RT_ENTRIES && l == 0xc0 && r == 0xc1 && chip->in05_ch != 6 &&
	    !(chip->pcm_start_sent && (chip->pcm_start_fmt & 0xf) == 5)) {
		for (i = 0; i < 2; i++) {
			u32 want = 0x10000 | hda_src[i] << 8 | i;
			int at;

			if (!src_to_dsp[i] || feed_at[i] >= 0)
				continue;
			at = ae9_rt_free_slot(e, feed_at[!i]);
			if (at < 0)
				break;
			e[at] = want;
			ae9_rt_write_checked(chip, 0x190000 + 4 * at, want,
					     &unsteady, &retried, &failed);
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

			if (a == AE9_RT_FRONT_L || a == AE9_RT_FRONT_R ||
			    !((e[i] >> 16) & 3) || sv >= 0x20 ||
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
	ae9_rt_write_checked(chip, AE9_RT_FRONT_L, 0x10000 | (0x40 << 8) | l,
			     &unsteady, &retried, &failed);
	ae9_rt_write_checked(chip, AE9_RT_FRONT_R, 0x10000 | (0x41 << 8) | r,
			     &unsteady, &retried, &failed);
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
		for (i = 0; i < n; i++) {
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
		for (i = 0; i < 4; i++)
			ae9_rt_write_checked(chip, AE9_RT_SURROUND + 4 * i,
					     0x10000 | (surround_ports[i] & 0xff) << 8 | (0xc2 + i),
					     &unsteady, &retried, &failed);
		dev_info(chip->dev, "SBX: router 5.1: DSP c2..c5 -> 0x%02x 0x%02x 0x%02x 0x%02x\n",
			 surround_ports[0] & 0xff, surround_ports[1] & 0xff,
			 surround_ports[2] & 0xff, surround_ports[3] & 0xff);
	}
	creative_dsp_chipio_write(chip, AE9_RT_APPLY, 1);
	creative_dsp_chipio_addr_invalidate();
	mutex_unlock(&chip->replay_mutex);
	ae9_dbg(chip, "SBX: router front L/R <- DSP ports 0x%02x/0x%02x, %d DSP input entr%s active\n",
		 l, r, inputs, inputs == 1 ? "y" : "ies");
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
 * count_ff: stream 0x05 is already on, so the router's 0xff count means
 * something (dsp_mode turns dsp_active on before it starts 0x05).
 */
/* Stream 0x05's channel param for a stream format: 0xdb = 6 ch (Windows'
 * DSP-mode streams, capture 6), 0x5b = 2 ch. */
static u32 ae9_in05_ch_verb(u16 format)
{
	return (format & 0xf) == 5 ? 0x115710db : 0x1157105b;
}

static unsigned int ae9_fmt_ch(u16 format)
{
	return (format & 0xf) + 1;
}

static void ae9_in05_on(struct creative_chip *chip, u16 format)
{
	u32 in05[] = {
		0x115f0100, 0x11571718, 0x11571805, 0x1157105b,	/* 0x05: channels */
		0x115f0100, 0x11571718, 0x11571805, 0x1157103c,	/* 0x05: on */
		0x115f0100,
	};

	in05[3] = ae9_in05_ch_verb(format);
	creative_ae9_verb_block_replay(chip, in05, ARRAY_SIZE(in05), false,
				       ae9_fmt_ch(format) == 6 ?
				       "SBX: DSP input stream 0x05 on, 6 ch" :
				       "SBX: DSP input stream 0x05 on");
	chip->in05_ch = ae9_fmt_ch(format);
}

static void ae9_in05_off(struct creative_chip *chip)
{
	static const u32 in05_off[] = {
		0x115f0100, 0x11571718, 0x11571805, 0x1157101c,	/* 0x05: off */
		0x115f0100,
	};

	creative_ae9_verb_block_replay(chip, in05_off, ARRAY_SIZE(in05_off),
				       false, "SBX: DSP input stream 0x05 off");
	chip->in05_ch = 0;
}

static void ae9_dsp_mode_start(struct creative_chip *chip, u16 format,
			       bool count_ff)
{
	u32 t[] = {
		0x115f0300, 0x1150b008, 0x11510018, 0x115300f0, 0x11540000,
		0x10220031,		/* node 0x02 converter format (patched) */
		0x10270610,		/* node 0x02 bound to tag 1, ch 0 */
	};

	t[5] = 0x10220000 | format;
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
	creative_ae9_verb_block_replay(chip, t, ARRAY_SIZE(t), false,
				       "SBX: playback start in DSP mode");
	creative_ae9_router_front_dsp(chip, count_ff);
}

void creative_ae9_dsp_mode_start(struct creative_chip *chip, u16 format)
{
	ae9_dsp_mode_start(chip, format, chip->dsp_active);
}

void creative_ae9_dsp_mode(struct creative_chip *chip, bool dsp)
{
	/* Direct Mode off (seq 9940-10118): node 0x02 unbound, 0x14 off,
	 * 0x05 -> 0x00, 0x18: 0x09 -> 0xd0 at 96 kHz 6 ch on, ASI 8, PLL
	 * trim, 96 kHz I2S descriptors ... */
	static const u32 head[] = {
		0x10270600, 0x11570dfc, 0x11570e07, 0x115f0700, 0x115f0100,
		0x11571718, 0x11571814, 0x1157101c, 0x115710b8, 0x115f0100,
		0x11571719, 0x11571843, 0x1157101a, 0x115f0100, 0x11571718,
		0x11571818, 0x115f0100, 0x11571719, 0x11571809, 0x115f0100,
		0x1157171a, 0x115718d0, 0x115f0100, 0x1157171d, 0x115718d0,
		0x115f0100, 0x1157171e, 0x1157180b, 0x115f0100, 0x11571718,
		0x11571818, 0x115710db, 0x11570d24, 0x11570e08, 0x115f0700,
		0x115f0100, 0x11571718, 0x11571818, 0x1157103c, 0x115f0100,
		0x11571717, 0x11571808, 0x11570d40, 0x11570cc7, 0x11570d42,
		0x11570ccd, 0x11570d41, 0x11570cce,
		0x11509000, 0x11510018, 0x1153f101, 0x11540001,
		0x11509004, 0x11510018, 0x1153f101, 0x11540001,
		0x11509008, 0x11510018, 0x1153f101, 0x11540001,
		0x11509024, 0x11510018, 0x11534004, 0x11540001,
		0x11509028, 0x11510018, 0x1153000f, 0x11540002,
	};
	/* ... (DAC re-lock here) ... full PLL program, ASI 0xf */
	static const u32 tail[] = {
		0x11570d44, 0x11570cc8, 0x11570d43, 0x11570ccc, 0x11570d45,
		0x11570ccb, 0x11570d40, 0x11570cc7, 0x11570d42, 0x11570ccd,
		0x11570d41, 0x11570cce, 0x11570d51, 0x11570cdb, 0x115f0100,
		0x11571717, 0x1157180f,
	};
	static const u8 dac_relock[][3] = {
		{ 0x48, 0x01, 0xc0 },	/* input configuration */
		{ 0x48, 0x0a, 0x02 },	/* master mode */
	};
	/* Direct Mode on (seq 11794-11812): node 0x02 unbound, stream 0x18 off */
	static const u32 off[] = {
		0x10270600, 0x11570d24, 0x11570e08, 0x115f0700, 0x115f0100,
		0x11571718, 0x11571818, 0x1157101c, 0x115f0100,
	};
	u32 zero = 0;	/* 0.0f: 0 dB, and "not muted" */

	if (!chip->corb_buf || !chip->dsp_done)
		return;
	if (!dsp) {
		creative_ae9_verb_block_replay(chip, off, ARRAY_SIZE(off), false,
					       "SBX: direct mode (DSP stream 0x18 off)");
		/*
		 * Stop stream 0x05 too: we start it in DSP mode (Windows doesn't),
		 * and left running it keeps the HDA -> SRC 0x90/0x80 -> DSP path
		 * in use. The next Direct Mode off then got a second path on SRC
		 * 0x98/0x88 that never started: silence or crunched audio, until
		 * a PSU cold boot (2026-11-04 router dumps).
		 */
		ae9_in05_off(chip);
		chip->dsp_active = false;
		return;
	}
	chip->dsp_active = true;
	creative_ae9_verb_block_replay(chip, head, ARRAY_SIZE(head), false,
				       "SBX: DSP mode (host audio -> DSP -> DAC at 96 kHz)");
	/* the DAC follows the new I2S clock: Windows sends these right after
	 * the descriptors (captures 5, 6), as every playback start does */
	creative_dsp_ca0113_replay_quiet(chip, dac_relock, ARRAY_SIZE(dac_relock));
	msleep(20);	/* Windows: 17 ms between descriptors and PLL (seq 10076) */
	creative_ae9_verb_block_replay(chip, tail, ARRAY_SIZE(tail), false,
				       "SBX: DSP mode, full PLL program");
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
	/* host-audio stream 0x05 on, stereo (DarkNikGr's working AE-9 setup:
	 * "routed but never STARTED before"), then the front outputs from the
	 * DSP (also done by dsp_mode_start, for later streams) */
	ae9_in05_on(chip, chip->pcm_start_sent ? chip->pcm_start_fmt : 0x31);
	creative_ae9_router_front_dsp(chip, true);
	creative_ae9_dsp_alive_log(chip, "DSP mode on");
}

/*
 * How long a fresh direct stream plays before the switch to DSP mode.
 * 300 ms was too soon (2026-11-04: echoey, crunched audio that toggling
 * Direct Mode didn't clear); switching by hand seconds later works.
 */
static unsigned int dsp_switch_ms = 3000;
module_param(dsp_switch_ms, uint, 0644);
MODULE_PARM_DESC(dsp_switch_ms,
	"ms a fresh stream plays direct before the switch to DSP mode (default 3000)");

static void creative_ae9_dsp_switch_work(struct work_struct *w)
{
	struct creative_chip *chip = container_of(to_delayed_work(w),
						  struct creative_chip,
						  dsp_switch_work);

	if (!chip->dsp_switch_pending || chip->fx.direct || !chip->pcm_start_sent)
		return;
	chip->dsp_switch_pending = false;
	dev_info(chip->dev, "SBX: switching the running stream to DSP mode (after %u ms)\n",
		 dsp_switch_ms);
	creative_ae9_fx_restore(chip);	/* DSP mode + the SBX profile */
}

/* Playback trigger start (atomic): run a pending switch shortly after. */
void creative_ae9_dsp_switch_kick(struct creative_chip *chip)
{
	if (chip->dsp_switch_pending && !chip->fx.direct)
		queue_delayed_work(chip->wq, &chip->dsp_switch_work,
				   msecs_to_jiffies(dsp_switch_ms));
}

/*
 * Windows reprograms the ES9038 DAC at every stream start (12 commands,
 * including its input setup 48/01 and master mode 48/0a), and the DAC
 * re-locks: a brief hiccup ~0.5 s into each stream (heard in VLC, 2026-11-
 * 05: 2 of 2 starts with the commands, 0 of 2 without). So they're sent
 * only when the rate or output changed, or anything else touched the DAC
 * (any CA0113 command or a resume clears dac_start_key). 0 = send them at
 * every start, as Windows does.
 */
/* 44.1/88.2 kHz: clock the I2S as at 48 kHz before the converter (see
 * creative_ae9_cold_pcm_replay); 0 = as before (silent after a 384 kHz
 * stream). */
static bool src_reclock = true;
module_param(src_reclock, bool, 0644);
MODULE_PARM_DESC(src_reclock, "44.1/88.2 kHz: set the I2S to its 48 kHz clocks before the sample-rate converter (1 = yes, default; 0 = as before)");

/* 44.1/88.2 kHz: send Windows' extra DAC commands (see below). */
static bool dac_441 = true;
module_param(dac_441, bool, 0644);
MODULE_PARM_DESC(dac_441, "44.1/88.2 kHz: send Windows' DAC commands 48/01 = 0xc1, 48/0b = 0x00, 0x20 (1 = yes, default; 0 = as before)");

static bool dac_start_skip = true;
module_param(dac_start_skip, bool, 0644);
MODULE_PARM_DESC(dac_start_skip, "Skip the DAC start commands when rate and output are unchanged (1 = yes, default; 0 = every start, as Windows)");

void creative_ae9_cold_pcm_replay(struct creative_chip *chip, bool start, bool atomic,
				  unsigned int rate, u16 format)
{
	bool hp = chip->hu_output;
	unsigned int r = 1;	/* the 48 kHz row: the block as captured */
	size_t i;

	if (!chip->corb_buf)
		return;
	for (i = 0; i < ARRAY_SIZE(ae9_pcm_rates); i++)
		if (ae9_pcm_rates[i].rate == rate)
			r = i;
	if (start && rate && ae9_pcm_rates[r].rate != rate)
		dev_warn(chip->dev, "pcm_rates: no setup for %u Hz, using 48 kHz\n", rate);
	/* Direct Mode off: Windows' start is only the bind (see dsp_mode). The
	 * DSP routing needs what a full start block sets up (silent otherwise,
	 * e.g. Direct Mode off restored at boot): send one first. */
	if (start && !chip->fx.direct && !atomic && chip->dsp_primed &&
	    !chip->dsp_switch_pending) {
		creative_ae9_dsp_mode_start(chip, format);
		return;
	}
	if (start && !atomic &&
	    dac_start_skip && chip->dac_start_key == ((r + 1) | hp << 8)) {
		ae9_dbg(chip, "dac_start_skip: DAC already set for %u Hz, start commands skipped\n",
			ae9_pcm_rates[r].rate);
	} else if (start && !atomic) {
		/* the captured 12, plus two for the 44.1 kHz family */
		u8 cmds[ARRAY_SIZE(ae9_cold_ca0113_pcm) + 2][3];
		bool fam441 = ae9_pcm_rates[r].route == AE9_ROUTE_SRC && dac_441;
		size_t n = 0;

		for (i = 0; i < ARRAY_SIZE(ae9_cold_ca0113_pcm); i++) {
			u8 *c = cmds[n];

			memcpy(c, ae9_cold_ca0113_pcm[i], 3);
			if (c[0] == 0x48 && c[1] == 0x1d)
				c[2] = hp ? 0x80 : 0x40;
			else if (c[0] == 0x48 && c[1] == 0x19)
				c[2] = hp ? 0x01 : 0x00;
			else if (c[0] == 0x48 && c[1] == 0x01 && fam441)
				c[2] = 0xc1;
			else if (c[0] == 0x48 && c[1] == 0x0a) {
				/*
				 * 44.1/88.2 kHz: Windows sends 48/01 = 0xc1
				 * (0xc0 for the 48 kHz family) and 48/0b =
				 * 0x00 then 0x20 before 48/0a, at every such
				 * start (all captures; 2026-11-05 01:59:56
				 * and 02:00:18). Without them our 44.1 kHz
				 * played garbled or silent.
				 */
				if (fam441) {
					static const u8 b0[3] = { 0x48, 0x0b, 0x00 };
					static const u8 b20[3] = { 0x48, 0x0b, 0x20 };

					memcpy(cmds[n++], b0, 3);
					memcpy(cmds[n++], b20, 3);
					c = cmds[n];
					memcpy(c, ae9_cold_ca0113_pcm[i], 3);
				}
				c[2] = ae9_pcm_rates[r].dac0a;
			}
			n++;
		}
		creative_dsp_ca0113_replay(chip, cmds, n,
					   fam441 ? "playback-start commands (44.1 kHz family)" :
						    "playback-start commands");
		chip->dac_start_key = (r + 1) | hp << 8;
	}
	if (start) {
		bool src = ae9_pcm_rates[r].route == AE9_ROUTE_SRC;
		/*
		 * A 6-channel stream (5.1) in Direct Mode, at up to 192 kHz. Not
		 * for the DSP-mode lead-in (Direct Mode off, switch pending): a
		 * lead-in started this way left the DSP with only the front pair
		 * after the switch (2026-11-04, try 1); started as before
		 * (stereo setup, front only for those 3 s) it was fine.
		 */
		/* ports <= 8: 48, 96 and 192 kHz (this said <= 2 until
		 * 2026-11-05, so 5.1 at 96 kHz got the stereo setup: front
		 * slots only, the other channels went wherever the router
		 * pointed last) */
		bool six = (format & 0xf) == 5 && ae9_pcm_rates[r].ports <= 8 &&
			   chip->fx.direct;
		const u32 *in = ae9_cold_pcm_start;
		size_t nin = ARRAY_SIZE(ae9_cold_pcm_start);
		/* room for the added writes: 5.1 at 192 kHz is 4 x 7 words
		 * for slots 2..5 plus 18 x 4 for slots 6..23 = 100 words;
		 * stereo 384 kHz 14 x 4 = 56; the converter 26 */
		u32 *t = kmalloc_array(nin + 32 * 4, sizeof(u32),
				       atomic ? GFP_ATOMIC : GFP_KERNEL);
		u32 addr = 0, data;
		size_t n = 0, k;
		int skip = 0;

		if (!t)
			return;
		if (ae9_pcm_rates[r].rate != 48000)
			dev_info(chip->dev, "pcm_rates: playback at %u Hz, format 0x%04x (%s, rate code 0x%02x)\n",
				 ae9_pcm_rates[r].rate, format,
				 src ? "via the sample-rate converter" : "direct",
				 ae9_pcm_rates[r].cp_code);
		for (i = 0; i < nin; i++) {
			u32 v = in[i];

			/* ChipIO memory writes: address lo/hi, then data lo/hi */
			if ((v & 0xffff0000) == 0x11500000)
				addr = (addr & 0xffff0000) | (v & 0xffff);
			else if ((v & 0xffff0000) == 0x11510000)
				addr = (addr & 0xffff) | ((v & 0xffff) << 16);

			/* SRC route: skip from ASI 8 to the end of the 0x19042c
			 * write, and set up the converter there instead */
			if (src && !skip && v == 0x115f0100 && i + 2 < nin &&
			    in[i + 1] == 0x11571717 && in[i + 2] == 0x11571808) {
				/*
				 * Clock the I2S as at 48 kHz first: the block's
				 * own ASI 8, PLL and I2S descriptor writes (up to
				 * 0x189028), unpatched. After a 384 kHz stream the
				 * I2S stayed on its 384 kHz clocks (0x1f103), and
				 * the converter's enable (0x18a020 = 3) didn't take:
				 * silent, until a 48 kHz-family stream re-clocked it
				 * (2026-11-06). Windows' own I2S setup has a 44.1 kHz
				 * entry with these 48 kHz values (0x1f100/0x8005).
				 */
				if (src_reclock) {
					u32 a2 = 0;

					for (k = i; k < nin; k++) {
						u32 w = in[k];

						if ((w & 0xffff0000) == 0x11500000)
							a2 = (a2 & 0xffff0000) | (w & 0xffff);
						else if ((w & 0xffff0000) == 0x11510000)
							a2 = (a2 & 0xffff) | ((w & 0xffff) << 16);
						t[n++] = w;
						if (a2 == 0x189028 && (w & 0xffff0000) == 0x11540000)
							break;
					}
				}
				for (k = 0; k < 5; k++) {
					n = ae9_chipio_w(t, n, k == 2 ? 0x18a024 :
							 k == 3 ? 0x18a028 : 0x18a020,
							 ae9_pcm_rates[r].src[k]);
					if (k)
						continue;
					/* converter clock, PLL/PMU 0x01 = 0x17,
					 * 0x40 = 0x06, 0x04 = 0x80 (Windows, at
					 * both 44.1 and 88.2) */
					t[n++] = 0x11570d01;
					t[n++] = 0x11570c17;
					t[n++] = 0x11570d40;
					t[n++] = 0x11570c06;
					t[n++] = 0x11570d04;
					t[n++] = 0x11570c80;
				}
				skip = 1;
			}
			if (skip == 1) {
				if (addr == 0x19042c && (v & 0xffff0000) == 0x11540000)
					skip = 2;
				continue;
			}

			/* direct route: more port remaps before the trigger */
			if (!src && v == 0x1150042c && !six)
				for (k = 2; k < ae9_pcm_rates[r].ports; k++)
					n = ae9_chipio_w(t, n, 0x190060 + 4 * k,
							 0x00014000 | (k << 8) | k);
			/*
			 * 5.1 direct (capture 6, 23:39:16): HDA 02..05 -> the
			 * Center/Sub (0x50/0x51) and Rear (0x60/0x61) DACs, each
			 * followed by 0x70d 0x90+k / 0x70e 0x14 / 0xf07 as for
			 * the front pair.
			 */
			if (!src && v == 0x1150042c && six) {
				static const u8 port51[6] = {
					0x40, 0x41, 0x50, 0x51, 0x60, 0x61 };

				for (k = 2; k < 6; k++) {
					n = ae9_chipio_w(t, n, 0x190060 + 4 * k,
							 0x00010000 | (port51[k] << 8) | k);
					t[n++] = 0x11570d90 + k;
					t[n++] = 0x11570e14;
					t[n++] = 0x115f0700;
				}
				/*
				 * Above 48 kHz each channel takes one slot per
				 * 48 kHz tick: 6 x the rate multiple slots. Stereo
				 * maps slot k to 0x40 + k (FL 40, 42, ..; FR 41,
				 * 43, ..), so sample s of a channel goes to its
				 * DAC pair's input + 2s. Without these, 5.1 at
				 * 96 kHz sent every second sample to whatever the
				 * slot held last (front inputs left by a 384 kHz
				 * stream): tinny, stuttering (2026-11-05). Same as
				 * Windows, slot for slot: 12 slots at 96 kHz, 24 at
				 * 192 kHz (capture 2026-11-05, 02:01:48 / 02:02:20).
				 */
				for (k = 6; k < 3 * ae9_pcm_rates[r].ports; k++)
					n = ae9_chipio_w(t, n, 0x190060 + 4 * k,
							 0x00010000 |
							 ((port51[k % 6] + 2 * (k / 6)) << 8) | k);
			}
			/* stream channel param: 0xdb = 6 ch (captured 0x5b) */
			if (six && v == 0x1157105b)
				v = 0x115710db;

			data = 0;
			if (addr == 0x189000 || addr == 0x189004 || addr == 0x189008)
				data = ae9_pcm_rates[r].i2s_clk;
			else if (addr == 0x189024)
				data = ae9_pcm_rates[r].i2s_rate;
			if (data && (v & 0xffff0000) == 0x11530000)
				v = 0x11530000 | (data & 0xffff);
			else if (data && (v & 0xffff0000) == 0x11540000)
				v = 0x11540000 | (data >> 16);
			/* node 0x02 converter format (captured 0x0031) */
			else if (v == 0x10220031)
				v = 0x10220000 | format;
			/* stream 0x05 destination and its connection point
			 * (captured 0xd0, rate code 0x09) */
			else if (i && v == 0x115718d0 &&
				 (in[i - 1] == 0x1157171a || in[i - 1] == 0x1157171d))
				v = 0x11571800 | (src ? 0x91 : 0xd0);
			else if (i && v == 0x11571809 && in[i - 1] == 0x1157171e)
				v = 0x11571800 | ae9_pcm_rates[r].cp_code;
			/* ChipIO 0x718 data after 0x717 index 0x0d: output select */
			else if (i && (v & 0xffffff00) == 0x11571800 &&
				 in[i - 1] == 0x1157170d)
				v = (v & 0xffffff00) | (hp ? 0xa1 : 0xa4);
			/* node 0x12 (XLR) boost: keep the user's setting */
			else if ((v & 0xfffff0ff) == 0x11236001 ||
				 (v & 0xfffff0ff) == 0x11235001)
				v = (v & 0xffffff00) | chip->mic_boost;
			t[n++] = v;
		}
		if (src && skip != 2)
			dev_warn(chip->dev, "pcm_rates: start block layout not as expected (skip %d)\n",
				 skip);
		creative_ae9_verb_block_replay(chip, t, n, atomic, hp ?
					       "pcm_windows: playback start block (headphones)" :
					       "pcm_windows: playback start block");
		kfree(t);
		chip->dsp_primed = true;
		/*
		 * First start in DSP mode after a bring-up (or a switch made
		 * while nothing played): start as a direct stream, then switch
		 * live once it runs (creative_ae9_dsp_switch_work). Switching
		 * before any stream has run leaves the 8051's DSP input path
		 * broken (2026-11-04: inactive or half-built router entries,
		 * silence or crunched audio); the live switch from a running
		 * direct stream works every time.
		 */
		if (!chip->fx.direct && !atomic)
			chip->dsp_switch_pending = true;
	} else {
		creative_ae9_verb_block_replay(chip, ae9_cold_pcm_stop,
					       ARRAY_SIZE(ae9_cold_pcm_stop), atomic,
					       "pcm_windows: playback stop block");
	}
}

/* Windows' record start (+ capture level) or stop block, from the XLR
 * recording capture (creative_ae9_cap_verbs.h). */
void creative_ae9_cap_replay(struct creative_chip *chip, bool start)
{
	if (!chip->corb_buf)
		return;
	if (start) {
		/* Windows' start block sets node 0x07 to its 48 kHz format
		 * (0x10720031) and then binds it (0x10770610). The codec takes
		 * the rate at the bind, so the recording's own format goes in
		 * that slot, before the bind: changing it after left a 96 kHz
		 * recording getting 48 kHz-rate data (2026-11-06). */
		u32 start_blk[ARRAY_SIZE(ae9_cap_start)];
		size_t i;

		for (i = 0; i < ARRAY_SIZE(ae9_cap_start); i++)
			start_blk[i] = ae9_cap_start[i] == 0x10720031 && chip->cap_format ?
				       0x10720000 | chip->cap_format : ae9_cap_start[i];
		creative_ae9_verb_block_replay(chip, ae9_cap_level,
					       ARRAY_SIZE(ae9_cap_level), false,
					       "capture: level block");
		creative_ae9_verb_block_replay(chip, start_blk,
					       ARRAY_SIZE(start_blk), false,
					       "capture: record start block");
	} else {
		creative_ae9_verb_block_replay(chip, ae9_cap_stop,
					       ARRAY_SIZE(ae9_cap_stop), false,
					       "capture: record stop block");
	}
}

/*
 * warm_reset: Windows' warm-DSP reset block (creative_ae9_warm_verbs.h),
 * which Windows sends before the download only when the DSP had been
 * running: 0 never, 1 when the DSP reads "dead" (0x801c = 0, as after our
 * shutdown or a failed start; a cold card reads junk there), 2 always.
 */
static unsigned int warm_reset;
module_param(warm_reset, uint, 0644);
MODULE_PARM_DESC(warm_reset, "Windows' warm-DSP reset before download: 0=never (default), 1=if DSP reads dead, 2=always");

/* Replay the cold-boot pre-download verbs; via CORB once it is running. */
static void creative_ae9_replay_cold(struct creative_chip *chip)
{
	unsigned int errs = 0;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(ae9_cold_prefw); i++) {
		u32 resp = 0;
		int ret;

		/* at the point where Windows' warm start has its extra block */
		if (warm_reset && i == AE9_WARM_RESET_POS) {
			unsigned int loop = ~0U;
			bool warm = warm_reset == 2;

			if (warm_reset == 1)
				warm = creative_dsp_chipio_read(chip, 0x801c, &loop) == 0 &&
				       loop == 0;
			dev_info(chip->dev, "warm_reset: 0x801c=0x%x, %s\n", loop,
				 warm ? "warm DSP: Windows' reset block goes in" :
					"cold DSP: no reset block");
			if (warm)
				creative_ae9_verb_block_replay(chip, ae9_warm_reset,
							       ARRAY_SIZE(ae9_warm_reset), false,
							       "warm_reset: Windows' warm-DSP reset");
		}
		ret = chip->corb_buf ?
			creative_ae9_corb_send_verb_sync(chip, ae9_cold_prefw[i], &resp) :
			creative_ae9_icw_send_verb(chip, ae9_cold_prefw[i], &resp);

		if (ret < 0)
			errs++;
	}
	creative_dsp_chipio_addr_invalidate();
	dev_info(chip->dev, "cold_prefw: replayed %zu pre-download verbs (%u errors)\n",
		 ARRAY_SIZE(ae9_cold_prefw), errs);
}

/* Replay both codecs' captured init verbs (codec 2 first, as Windows). */
static void creative_ae9_replay_both(struct creative_chip *chip)
{
	size_t n1 = creative_ae9_codec1_prefw_count(chip);

	if (creative_ae9_fw_split_ok(chip)) {
		creative_ae9_replay_cold(chip);
		return;
	}

	if (n1 != ARRAY_SIZE(ae9_initseq_codec1))
		dev_info(chip->dev, "fw_split: replaying codec 1 entries 0-%zu only (pre-download)\n",
			 n1 - 1);
	/* fallback when the table layout changed: the warm tables */
	creative_ae9_replay_init(chip, 2, ae9_initseq_codec2,
				 ARRAY_SIZE(ae9_initseq_codec2));
	creative_ae9_replay_init(chip, 1, ae9_initseq_codec1, n1);
}

/*
 * After our firmware is running: the MASTERCONTROL boot handshake, then
 * Windows' post-handshake setup from the table tail. SCP messages in the
 * tail are rebuilt from their low/high word verbs and sent through
 * dspio_scp() (which waits on the DSP queue and reads GET replies); the
 * recorded reply-pop verbs (0x702/0x704 on nid 0x16) are skipped since
 * dspio_scp() reads replies itself. Everything else goes out via CORB.
 */
static void creative_ae9_bar2_post_replay(struct creative_chip *chip);

/* tail replay: messages that failed (DSP briefly busy) get one more try */
#define TAIL_RETRY_MAX	64
struct tail_msg { u32 w[1 + 8]; u8 n; };

static void retry_add(struct tail_msg *retry, unsigned int *nretry,
		      const u32 *words, unsigned int n)
{
	if (!retry || *nretry >= TAIL_RETRY_MAX || n > ARRAY_SIZE(retry->w))
		return;
	memcpy(retry[*nretry].w, words, n * sizeof(u32));
	retry[*nretry].n = n;
	(*nretry)++;
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
 * The startup tail comes from the old warm capture. Six of its SET messages
 * are ones Windows' own startup never sends and the DSP rejects (error
 * replies 0x0e4e9620 etc. at every boot): output effects 0x96 requests
 * 0x16 (bass redirect crossover), 0x1e (sub gain), 0x27/0x28 (surround L/R
 * level), 0x37/0x38 (surround L/R delay). They're left out, so the
 * effects module sees what Windows' startup sends.
 */

static bool ae9_tail_rejected(u32 hdr)
{
	unsigned int mid = hdr & 0xff, req = (hdr >> 17) & 0x7f;

	return !((hdr >> 16) & 1) && mid == 0x96 &&
	       (req == 0x16 || req == 0x1e || req == 0x27 || req == 0x28 ||
		req == 0x37 || req == 0x38);
}

static void creative_ae9_post_fw(struct creative_chip *chip)
{
	struct tail_msg *retry;
	unsigned int nretry = 0;
	u32 words[1 + 32];
	unsigned int n = 0, msgs = 0, msg_err = 0, verbs = 0, verb_err = 0;
	unsigned int consec_err = 0, msg_skipped = 0, msg_dropped = 0;
	unsigned int dbg = 0;
	u16 lo = 0;
	size_t i;
	int ret;

	if (!creative_ae9_fw_split_ok(chip))
		return;
	retry = kcalloc(TAIL_RETRY_MAX, sizeof(*retry), GFP_KERNEL);

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

	for (i = AE9_INITSEQ1_TAIL; i < ARRAY_SIZE(ae9_initseq_codec1); i++) {
		const struct ae9_initverb *e = &ae9_initseq_codec1[i];
		u16 p16 = ((e->verb & 0xff) << 8) | e->payload;
		u32 resp = 0;

		if (e->nid == 0x16) {
			unsigned int size;

			switch (e->verb >> 8) {
			case 0x0:		/* SCP write data low */
				lo = p16;
				break;
			case 0x1:		/* SCP write data high: word complete */
				words[n++] = ((u32)p16 << 16) | lo;
				size = (words[0] >> 27) & 0x1f;
				if (n == size + 1 || n == ARRAY_SIZE(words)) {
					/* A DSP that stops taking commands makes
					 * every send wait out its timeout; stop
					 * sending SCP after 8 failures in a row. */
					if (ae9_tail_rejected(words[0])) {
						msg_dropped++;
					} else if (consec_err >= 8) {
						msg_skipped++;
						retry_add(retry, &nretry, words, n);
					} else if (creative_dsp_scp_send(chip, words[0],
									 &words[1], n - 1) < 0) {
						msg_err++;
						consec_err++;
						retry_add(retry, &nretry, words, n);
					} else {
						consec_err = 0;
					}
					msgs++;
					n = 0;
				}
				break;
			default:		/* recorded reply pops: skip */
				break;
			}
			continue;
		}

		{
			u32 v = creative_ae9_verb(1, e->nid, e->verb, e->payload);

			ret = chip->corb_buf ?
				creative_ae9_corb_send_verb_sync(chip, v, &resp) :
				creative_ae9_icw_send_verb(chip, v, &resp);
			if (ret < 0)
				verb_err++;
			verbs++;
		}
	}
	creative_dsp_chipio_addr_invalidate();

	creative_dsp_chipio_read(chip, 0x100e30, &dbg);
	dev_info(chip->dev,
		 "fw_split: tail replay done: %u SCP msgs (%u failed, %u skipped, %u dropped), %u verbs (%u failed); DBGCNTL 0x%08x\n",
		 msgs, msg_err, msg_skipped, msg_dropped, verbs, verb_err, dbg);
	creative_ae9_dsp_health(chip, dbg);
	creative_ae9_dsp_alive_log(chip, "boot");
	if (nretry) {
		unsigned int k, ok = 0;

		for (k = 0; k < nretry; k++)
			if (creative_dsp_scp_send(chip, retry[k].w[0], &retry[k].w[1],
						  retry[k].n - 1) == 0)
				ok++;
		dev_info(chip->dev, "fw_split: resent %u failed tail SCP msgs, %u went through\n",
			 nretry, ok);
	}
	kfree(retry);

	/* Windows' post-boot BAR2 activity: CA0113 commands, head-unit SPI
	 * frames (incl. the "-SP-" speaker label), GPIO writes */
	creative_ae9_bar2_post_replay(chip);
	/* the replay switched phantom power off (f0 05 03 02 01 00); hold
	 * "on" requests off for a while, see hu_phantom_put */
	chip->hu_phantom_req = -1;
	chip->hu_phantom_hold = jiffies + AE9_PHANTOM_HOLD_S * HZ;
	chip->rt_ff_logged = false;
}

static void creative_ae9_bar2_post_replay(struct creative_chip *chip)
{
	unsigned int frames = 0, noreply = 0, cmds = 0, writes = 0;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(ae9_cold_bar2_post); i++) {
		const struct ae9_bar2_op *o = &ae9_cold_bar2_post[i];
		u8 resp[32];

		switch (o->type) {
		case 'S':
			if (creative_spi_frame_win(chip, o->data, o->len,
						   resp, sizeof(resp)) < 0)
				noreply++;
			frames++;
			break;
		case 'C': {
			u8 cmd[1][3] = { { o->off, o->val >> 8, o->val & 0xff } };

			creative_dsp_ca0113_replay_quiet(chip, cmd, 1);
			cmds++;
			break;
		}
		default:
			if (o->size == 1)
				creative_ext_writeb(chip, o->off, o->val);
			else if (o->size == 2)
				creative_ext_writew(chip, o->off, o->val);
			else
				creative_ext_writel(chip, o->off, o->val);
			writes++;
			break;
		}
	}
	dev_info(chip->dev,
		 "post_windows: %u SPI frames (%u without reply), %u CA0113 commands, %u writes\n",
		 frames, noreply, cmds, writes);
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
/*   - HW_CTRL (0x1c) = 0x00000680 -> 0x00880680 (AE-7 pre-writes)  */
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

/* Windows' BAR2 init writes, in capture order (cold-boot trace lines 1-114) */
static const struct { u16 reg; u32 val; } ae9_bar2_init_windows[] = {
	{ 0x400, 0x1 }, { 0x42c, 0x0 }, { 0x46c, 0x0 }, { 0x4ac, 0x0 },
	{ 0x4ec, 0x0 }, { 0x43c, 0x0 }, { 0x47c, 0x0 }, { 0x4bc, 0x0 },
	{ 0x4fc, 0x0 }, { 0x408, 0x1 }, { 0x100, 0x600 }, { 0x410, 0x14 },
	{ 0x40c, 0x1 }, { 0x100, 0x60f }, { 0x100, 0x70f }, { 0x830, 0xaff },
	{ 0x86c, 0x0 }, { 0x800, 0x6b }, { 0x86c, 0x1 }, { 0x800, 0x6b },
	{ 0x804, 0x57 }, { 0x20c, 0x800000 }, { 0x01c, 0x880480 },
	{ 0xc0c, 0x80 }, { 0xc00, 0x30 }, { 0xc04, 0x0 }, { 0xc0c, 0x0 },
	{ 0xc0c, 0x3 }, { 0xc0c, 0x3 }, { 0xc0c, 0x3 },
	{ 0xc08, 0x1 }, { 0xc08, 0xf1 }, { 0xc08, 0x1 }, { 0xc08, 0xc7 },
	{ 0xc08, 0xc1 }, { 0xc04, 0x80 },
	{ 0x01c, 0x480 }, { 0x01c, 0x880480 },
};

static void creative_ae9_bar2_init_windows(struct creative_chip *chip)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(ae9_bar2_init_windows); i++) {
		creative_ext_writel(chip, ae9_bar2_init_windows[i].reg,
				    ae9_bar2_init_windows[i].val);
		creative_ext_readl(chip, ae9_bar2_init_windows[i].reg);
	}
	dev_info(chip->dev, "bar2_first: Windows BAR2 init done, BAR2+0x4=0x%08x (Windows: 0x7172000a)\n",
		 creative_ext_readl(chip, 0x4));
}

/* ------------------------------------------------------------------ */
/* BAR2 init: VFIO-capture-based (Phase 1-5)			  */
/* This is the sequence that previously produced ACM display output. */

/* ------------------------------------------------------------------ */
/* debugfs register-poke interface					  */
/* Commands written to /sys/kernel/debug/creative_ae9/cmd:	  */
/*   w2 <off> <val>  - write BAR2[off] = val			  */
/*   r2 <off>        - read BAR2[off]				  */
/*   w0 <off> <val>  - write BAR0[off] = val			  */
/*   r0 <off>        - read BAR0[off]				  */
/* Result readable from the same file.				  */
/* ------------------------------------------------------------------ */
static struct creative_chip *g_dbg_chip;
static char g_dbg_result[128];

static ssize_t creative_dbg_write(struct file *f, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	char buf[64];
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
		ret = creative_ae9_corb_send_verb_sync(g_dbg_chip, v, &resp);
		creative_dsp_chipio_addr_invalidate();
		mutex_unlock(&g_dbg_chip->replay_mutex);
		dev_info(g_dbg_chip->dev, "dbg verb 0x%08x -> 0x%08x (%d)\n", v, resp, ret);
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

	/* "hutext ABCD": hold text on the head-unit display (shown at the
	 * next poll, within ~0.5 s); "hutext" alone goes back to the normal
	 * volume/MUTE display. Printable ASCII only. The frame carries 8
	 * characters but the display has 4 cells (left-aligned, no scroll;
	 * dots ride on the cell before them), so only the first 4 show. */
	/*
	 * "huframe f0 85 01 02 f7": one raw head-unit frame (hex bytes, up to
	 * 16). The heartbeat sends it on its next cycle, so it never collides
	 * with the poll; the reply is logged and shown by reading cmd.
	 */
	if (!strncmp(buf, "huframe", 7)) {
		u8 tx[16];
		unsigned long flags;
		unsigned int b;
		int k = 0, used, i = 7;

		while (k < 16 && sscanf(buf + i, " %x%n", &b, &used) == 1) {
			tx[k++] = b & 0xff;
			i += used;
		}
		if (k < 3 || tx[0] != 0xf0 || tx[k - 1] != 0xf7)
			return -EINVAL;
		spin_lock_irqsave(&g_dbg_chip->lock, flags);
		memcpy(g_dbg_chip->hu_dbg_tx, tx, k);
		g_dbg_chip->hu_dbg_len = k;
		spin_unlock_irqrestore(&g_dbg_chip->lock, flags);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "huframe queued (%d bytes); read again for the reply\n", k);
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
		creative_i2c_raw_tx(g_dbg_chip, reg & 0xff,
				    (n > 2) ? data : NULL, n - 2);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "hutx reg=0x%02x len=%d sent\n", reg & 0xff, n - 2);
		return len;
	}
	/* "chipior <hex_addx>" — ChipIO read */
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
		ret = creative_ae9_corb_send_verb_sync(g_dbg_chip, v, &resp);
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
	 * (IRS 0x5): useful with corb_rings=0, otherwise it just times out. */
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

	/*
	 * The head unit sets itself up from its own MCU firmware; Windows
	 * sends it no I2C init (both MMIO captures), and ours disrupted it.
	 * Windows' post-boot replay (creative_ae9_bar2_post_replay) did the
	 * rest, so only the heartbeat runs here, without the GPIO4 toggle.
	 */
	dev_info(chip->dev, "ACM work: post_windows, heartbeat only (no GPIO4 toggle)\n");
	if (acm_heartbeat_ms) {
		queue_delayed_work(chip->wq, &chip->hb_work,
				      msecs_to_jiffies(acm_heartbeat_ms));
	}
}

/*
 * Windows' head-unit poll, one cycle every ~500 ms (cold capture): identify,
 * status queries, output mode, then the display text. Replies below are
 * what Windows always got; the knob was never touched in the capture, so
 * any reply that changes is logged to find the knob/button events.
 */
static const struct { u8 len; u8 d[10]; } hu_poll[] = {
	{ 4, { 0xf0, 0x81, 0x00, 0xf7 } },
	{ 8, { 0xf0, 0x54, 0x04, 0x41, 0x63, 0x6d, 0x31, 0xf7 } },
	{ 8, { 0xf0, 0x54, 0x04, 0x31, 0x6d, 0x63, 0x41, 0xf7 } },
	{ 7, { 0xf0, 0xd5, 0x03, 0x00, 0x20, 0x04, 0xf7 } },
	{ 8, { 0xf0, 0x54, 0x04, 0x11, 0x11, 0x11, 0x11, 0xf7 } },
	{ 4, { 0xf0, 0xc2, 0x00, 0xf7 } },
	{ 5, { 0xf0, 0x83, 0x01, 0x02, 0xf7 } },
	{ 5, { 0xf0, 0x83, 0x01, 0x02, 0xf7 } },
	{ 5, { 0xf0, 0x83, 0x01, 0x07, 0xf7 } },
	{ 5, { 0xf0, 0x85, 0x01, 0x02, 0xf7 } },
	{ 5, { 0xf0, 0x83, 0x01, 0x02, 0xf7 } },
	{ 5, { 0xf0, 0x83, 0x01, 0x07, 0xf7 } },
	{ 5, { 0xf0, 0xb1, 0x01, 0x01, 0xf7 } },
	{ 5, { 0xf0, 0xb1, 0x01, 0x02, 0xf7 } },
	{ 5, { 0xf0, 0xb1, 0x01, 0x03, 0xf7 } },
};

/*
 * Before each SBX light change Windows sends the output-mode frame
 * f0 03 03 02 <00 speakers / 40 headphones> 40 f7 (capture 6, ~0.4 s
 * before each f0 22 02 01); we do the same. (It was a guess for the knob
 * freeze, which turned out to be the stuck press bit; see hu_parse.)
 */
static unsigned int hu_sbx_mode = 1;
module_param(hu_sbx_mode, uint, 0644);
MODULE_PARM_DESC(hu_sbx_mode, "post_windows: send the output-mode frame before each SBX light change, as Windows (1=on, default)");

static bool hu_auto_switch;
module_param(hu_auto_switch, bool, 0444);
MODULE_PARM_DESC(hu_auto_switch, "Start with \"Head Unit Auto Switch\" on: plugging into the head unit's 3.5 mm headphone jack switches to headphones, unplugging back to speakers (default off: plugs only log)");

#define HU_PLUG_SETTLE_MS	1500	/* plug must hold still this long */

/* Log raw head-unit button bits (83 01 02 byte changes, b1 press reports). */
static unsigned int hu_debug;
module_param(hu_debug, uint, 0644);
MODULE_PARM_DESC(hu_debug, "post_windows: log raw head-unit button state (1=on)");

/*
 * Stop a bring-up right after codec detection when codec 1 is silent (1,
 * default). 0 runs the whole bring-up anyway, as before v0.6.5: slow (2-3
 * min of timeouts) but that path once brought codec 1 back (15:19 on
 * 2026-11-04); for finding out which of its steps does it.
 */
static unsigned int codec1_failfast = 1;
module_param(codec1_failfast, uint, 0644);
MODULE_PARM_DESC(codec1_failfast, "Stop a bring-up early when codec 1 is silent (1=on, default; 0 = full bring-up anyway, slow, for testing)");

unsigned int ae9_debug;
module_param_named(debug, ae9_debug, uint, 0644);
MODULE_PARM_DESC(debug, "Log detail: 0 startup, actions and problems (default), 1 also routine per-stream/per-step lines, 2 also raw head-unit button state");

/* knob/button poll interval between Windows' full 500 ms polls */
static unsigned int hu_fast_ms = 40;
module_param(hu_fast_ms, uint, 0444);
MODULE_PARM_DESC(hu_fast_ms, "post_windows: knob poll interval (ms)");

/* hu_master=1: name the head-unit volume/mute "Master Playback Volume" /
 * "Switch", so PipeWire/PulseAudio use them as the hardware volume and the
 * desktop slider and the knob move together. 0: "Head Unit Playback ...". */
static unsigned int hu_master = 1;
module_param(hu_master, uint, 0444);
MODULE_PARM_DESC(hu_master, "post_windows: expose the head-unit volume as Master (1=on, default)");

#define HU_ATT_DEFAULT	0x48	/* Windows' boot volume, -36.0 dB */
#define HU_SP_TICKS	10	/* Windows shows "-SP-" ~5 s, then the volume */

/* Volume = the same attenuation on six head-unit DAC registers, in the
 * order Windows writes them on a volume change; 0xff mutes. */
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
	creative_dsp_ca0113_replay_quiet(chip, cmds, 6);
	/* mute changes always; each volume step only with debug=1 */
	if (chip->hu_mute != chip->hu_mute_logged) {
		chip->hu_mute_logged = chip->hu_mute;
		dev_info(chip->dev, "head unit: %s\n", chip->hu_mute ? "muted" : "unmuted");
	}
	ae9_dbg(chip, "head unit: volume att 0x%02x (%s)\n", att,
		 chip->hu_mute ? "muted" : "on");
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

		if ((hu_debug || ae9_debug >= 2) && (!chip->hu_status_valid ||
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
		if (chip->hu_status_valid && (chg & 0x08) && (st & 0x08)) {
			chip->hu_sbx_press = true;
			ae9_dbg(chip, "head unit: SBX button\n");
		}
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
	if ((hu_debug || ae9_debug >= 2) && r[1] == 0xb1 && n >= 8 && r[4])
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

/* Replay one of Windows' switch op lists (creative_ae9_switch_ops.h). */
static void creative_ae9_ops_replay(struct creative_chip *chip,
				    const struct ae9_bar2_op *ops, size_t n,
				    const char *what)
{
	u32 run[64];
	size_t i, nr = 0;
	u8 resp[24];

	for (i = 0; i <= n; i++) {
		const struct ae9_bar2_op *o = i < n ? &ops[i] : NULL;

		if (o && o->type == 'V' && nr < ARRAY_SIZE(run)) {
			run[nr++] = o->val;
			continue;
		}
		if (nr) {
			creative_ae9_verb_block_replay(chip, run, nr, false, what);
			nr = 0;
		}
		if (!o)
			break;
		switch (o->type) {
		case 'V':		/* run buffer was full */
			run[nr++] = o->val;
			break;
		case 'C': {
			u8 cmd[1][3] = { { o->off, o->val >> 8, o->val & 0xff } };

			creative_dsp_ca0113_replay_quiet(chip, cmd, 1);
			break;
		}
		case 'S':
			creative_spi_frame_win(chip, o->data, o->len, resp, sizeof(resp));
			break;
		default:
			if (o->size == 1)
				creative_ext_writeb(chip, o->off, o->val);
			else if (o->size == 2)
				creative_ext_writew(chip, o->off, o->val);
			else
				creative_ext_writel(chip, o->off, o->val);
			break;
		}
	}
}

/*
 * Switch the output between the card's RCA jacks (speakers) and the head
 * unit's headphone jack, as Windows does (switch op lists), then restore
 * that output's own volume. Windows also brackets the change with
 * f0 22 02 01 01 / 00 on the head unit, which is the SBX light (item 01):
 * it flashes, then ends off. The heartbeat puts the real state back.
 */
static void creative_ae9_hu_switch(struct creative_chip *chip, unsigned int hp)
{
	u8 resp[24];

	hp = !!hp;
	if (hp == chip->hu_output)
		return;
	chip->hu_att_out[chip->hu_output] = chip->hu_att;
	if (hp)
		creative_ae9_ops_replay(chip, ae9_sw_to_hp, ARRAY_SIZE(ae9_sw_to_hp),
					"head unit: switch to headphones");
	else
		creative_ae9_ops_replay(chip, ae9_sw_to_sp, ARRAY_SIZE(ae9_sw_to_sp),
					"head unit: switch to speakers");
	chip->hu_output = hp;
	chip->hu_att = chip->hu_att_out[hp];
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

/* Act on knob input and pending control changes: display first (feels
 * immediate), then the DAC volume commands (~20 ms each). Holds: under
 * 1 s mutes, 1.5-3 s switches speakers/headphones, 3 s or more toggles
 * the knob light (3 s, not 3.5: the b1 report, our fallback when the
 * press bit sticks, can't count past 3000 ms). */
static void creative_ae9_hu_update(struct creative_chip *chip, int steps,
				   int press_ms)
{
	u8 resp[24];
	int sbx_led;

	if (chip->hu_sbx_press) {
		chip->hu_sbx_press = false;
		creative_ae9_fx_sbx_toggle(chip);
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
		u8 mode[7] = { 0xf0, 0x03, 0x03, 0x02,
			       chip->hu_output ? 0x40 : 0x00, 0x40, 0xf7 };

		if (hu_sbx_mode)
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

		chip->hu_plug_pending = false;
		if (chip->hu_auto_hp && chip->hu_switch_to < 0 &&
		    want != (int)chip->hu_output) {
			dev_info(chip->dev, "head unit: auto switch to %s (3.5 mm headphone plug %s)\n",
				 want ? "headphones" : "speakers", want ? "in" : "out");
			chip->hu_switch_to = want;
		}
	}
	if (chip->hu_switch_to >= 0) {
		int to = chip->hu_switch_to;

		chip->hu_switch_to = -1;
		creative_ae9_hu_switch(chip, to);
	}
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
		chip->hu_att = clamp_t(int, (int)chip->hu_att - steps, 0, 254);
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
	if (!chip->hu_dirty)
		return;
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
	if (n > 0)
		steps += creative_ae9_hu_parse(chip, resp, min_t(int, n, sizeof(resp)), &press_ms);
	creative_ae9_hu_update(chip, steps, press_ms);
}

/* Windows' full poll cycle, then the display text. */
static void creative_ae9_hb_windows(struct creative_chip *chip)
{
	int i, n, fails = 0, steps = 0, press_ms = -1;
	u8 resp[24];

	creative_ext_writew(chip, 0x320, 0x105);
	for (i = 0; i < ARRAY_SIZE(hu_poll); i++) {
		n = creative_spi_frame_win(chip, hu_poll[i].d, hu_poll[i].len,
					   resp, sizeof(resp));
		if (n < 0) {
			fails++;
			continue;
		}
		n = min_t(int, n, sizeof(resp));
		steps += creative_ae9_hu_parse(chip, resp, n, &press_ms);
	}
	if (fails)
		dev_info_ratelimited(chip->dev, "head unit: %d/%zu poll frames unanswered\n",
				     fails, ARRAY_SIZE(hu_poll));
	creative_ae9_hu_update(chip, steps, press_ms);
	creative_ae9_hu_label(chip);
	chip->hu_ticks++;
}

/* ALSA controls: "Head Unit Playback Volume" (0..254 = -127.0..0 dB) and
 * "Head Unit Playback Switch" (renamed to "Master Playback ..." with
 * hu_master=1). The heartbeat applies changes. */
static const DECLARE_TLV_DB_SCALE(hu_vol_tlv, -12700, 50, 0);

static int hu_vol_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	ui->count = 1;
	ui->value.integer.min = 0;
	ui->value.integer.max = 254;
	return 0;
}

static int hu_vol_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);

	uv->value.integer.value[0] = 254 - chip->hu_att;
	return 0;
}

static int hu_vol_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	long v = clamp_t(long, uv->value.integer.value[0], 0, 254);

	if (254 - v == chip->hu_att)
		return 0;
	chip->hu_att = 254 - v;
	chip->hu_dirty = 1;
	return 1;
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
 * Not called "Mic Boost" on purpose: PipeWire's mixer paths know that name
 * and would tie it to the mic slider. With this name PipeWire leaves it
 * alone and the slider is software gain only. */
static const DECLARE_TLV_DB_SCALE(mic_boost_tlv, 0, 1000, 0);

static int mic_boost_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	ui->count = 1;
	ui->value.integer.min = 0;
	ui->value.integer.max = 2;
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
	unsigned int v = clamp_t(long, uv->value.integer.value[0], 0, 2);

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
 * "DAC Filter": the ES9038's digital filter, as the Creative app sets it
 * (capture 2026-11-03): Fast = DAC 48/07 bits 7:5 = 000 and DAC 49/0e
 * bit 0 = 1; Slow = 001 and bit 0 = 0. "Default" leaves Windows' boot value
 * (48/07 = 0x80). The choice is kept in every later 48/07 write
 * (ca0113_list) and re-applied after a resume.
 */
static void creative_ae9_dac_filter_apply(struct creative_chip *chip)
{
	u8 cmds[2][3];

	if (!chip->corb_buf || !chip->dsp_done || !chip->dac_filter)
		return;
	cmds[0][0] = 0x48;
	cmds[0][1] = 0x07;
	cmds[0][2] = chip->dac48_07;	/* ca0113_list puts the filter in */
	cmds[1][0] = 0x49;
	cmds[1][1] = 0x0e;
	cmds[1][2] = chip->dac_filter == 1 ? 0x01 : 0x00;
	creative_dsp_ca0113_replay_quiet(chip, (const u8 (*)[3])cmds, 2);
}

static int dac_filter_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	static const char * const names[] = { "Default", "Fast", "Slow" };

	return snd_ctl_enum_info(ui, 1, 3, names);
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
	u8 cmd[1][3] = { { 0x48, 0x07, 0 } };

	if (v > 2 || v == chip->dac_filter)
		return 0;
	if (!v) {
		/* back to Windows' boot filter (bits 7:5 = 100) */
		chip->dac_filter = 0;
		cmd[0][2] = (chip->dac48_07 & 0x1f) | 0x80;
		if (chip->corb_buf && chip->dsp_done)
			creative_dsp_ca0113_replay_quiet(chip,
							 (const u8 (*)[3])cmd, 1);
		return 1;
	}
	chip->dac_filter = v;
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

	uv->value.enumerated.item[0] = chip->hu_switch_to >= 0 ?
				       chip->hu_switch_to : chip->hu_output;
	return 0;
}

static int hu_out_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	unsigned int v = uv->value.enumerated.item[0];

	if (v > 1)
		return -EINVAL;
	if (v == chip->hu_output && chip->hu_switch_to < 0)
		return 0;
	chip->hu_switch_to = v;		/* the heartbeat does the switch */
	return 1;
}

static const struct snd_kcontrol_new hu_out_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Head Unit Output",
	.info = hu_out_info,
	.get = hu_out_get,
	.put = hu_out_put,
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

	if (!chip->hu_status_valid)
		return -EAGAIN;
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

	chip->hu_att = HU_ATT_DEFAULT;
	chip->hu_att_out[0] = chip->hu_att_out[1] = HU_ATT_DEFAULT;
	chip->hu_output = 0;		/* the boot replay selects speakers */
	chip->hu_name_until = HU_SP_TICKS;
	chip->hu_switch_to = -1;
	chip->hu_light = 1;
	chip->hu_light_sent = 1;	/* the head unit starts lit */
	chip->hu_sbx_led = -1;
	chip->hu_out_ctl = snd_ctl_new1(&hu_out_control, chip);
	if (snd_ctl_add(chip->card, chip->hu_out_ctl) < 0)
		chip->hu_out_ctl = NULL;
	chip->hu_vol_ctl = snd_ctl_new1(&hu_controls[0], chip);
	chip->hu_mute_ctl = snd_ctl_new1(&hu_controls[1], chip);
	if (hu_master && chip->hu_vol_ctl && chip->hu_mute_ctl) {
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

	if (chip->dsp_done) {
		if (chip->hu_dbg_len) {
			u8 tx[16], rx[16];
			unsigned long flags;
			int k, n, j, o = 0;
			char hex[16 * 3 + 1] = "";

			spin_lock_irqsave(&chip->lock, flags);
			k = chip->hu_dbg_len;
			memcpy(tx, chip->hu_dbg_tx, k);
			chip->hu_dbg_len = 0;
			spin_unlock_irqrestore(&chip->lock, flags);
			n = creative_spi_frame_win(chip, tx, k, rx, sizeof(rx));
			for (j = 0; j < min(n, 16); j++)
				o += scnprintf(hex + o, sizeof(hex) - o, " %02x", rx[j]);
			dev_info(chip->dev, "huframe: sent %*ph, reply%s (%d)\n",
				 k, tx, n > 0 ? hex : " none", n);
			scnprintf(g_dbg_result, sizeof(g_dbg_result),
				  "huframe reply:%s (%d)\n", n > 0 ? hex : " none", n);
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
					      msecs_to_jiffies(min(hu_fast_ms, acm_heartbeat_ms)));
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
		 "state %s: BAR2 0x320=0x%04x 0x304=0x%02x 0x20c=0x%08x 0x4=0x%08x 0xc0c=0x%02x 0xc14=0x%02x\n",
		 when, creative_ext_readw(chip, 0x320),
		 readb(chip->ext_regs + 0x304),
		 creative_ext_readl(chip, 0x20c), creative_ext_readl(chip, 0x4),
		 creative_ext_readl(chip, 0xc0c) & 0xff,
		 creative_ext_readl(chip, 0xc14) & 0xff);
}

/* The secondary bus reset hits everything behind the bridge: only do it
 * when the card (any of its functions) is alone there, as on the AE-9's
 * own PCIe-to-PCI bridge. */
/*
 * The PCIe-to-PCI bridge above the card (IDT Tsi381 here, ASMedia
 * ASM1083/1085 1b21:1080 on other AE-9s). An ASMedia card drops off the
 * bus during bring-up (bugzilla 219552 #81, stock driver too, #56), so log
 * the bridge's status at each step: a DMA or config cycle the card
 * botched shows up as master/target abort or SERR on the secondary side.
 * The error bits are write-1-to-clear and cleared after each read, so
 * every line shows only what happened since the one before.
 */
static unsigned int bridge_log = 1;
module_param(bridge_log, uint, 0644);
MODULE_PARM_DESC(bridge_log, "Log the PCI bridge's status at each bring-up step (1=on, default)");

/*
 * corb_rings=0: never start the CORB/RIRB command rings; every command goes
 * through the immediate command interface (slow, one at a time). For
 * telling whether the card's first DMA reads (the rings) are what knocks
 * an ASMedia card off the bus. Diagnostic only: much of the bring-up
 * expects the rings.
 */
static unsigned int corb_rings = 1;
module_param(corb_rings, uint, 0444);
MODULE_PARM_DESC(corb_rings, "Start the CORB/RIRB command rings (1=yes, default; 0 = immediate commands only, diagnostic)");

/*
 * dma_pad: spare space after every DMA buffer, in KB. The ASMedia ASM1083
 * reads ahead past the end of a buffer (bugzilla 219552: IOMMU faults one
 * byte past the 256 KB firmware buffer). -1 = auto: 64 KB behind an
 * ASMedia bridge, none otherwise.
 */
static int dma_pad = -1;
module_param(dma_pad, int, 0444);
MODULE_PARM_DESC(dma_pad, "Spare KB after every DMA buffer (-1=auto: 64 behind an ASMedia bridge, else 0; default)");

/*
 * fw_rings_off=1: keep the CORB/RIRB command rings off while the firmware
 * streams to the DSP (commands go through the immediate interface), and
 * start them once the stream has stopped, before the DSP boot handshake.
 * On an ASMedia card the rings running alongside the firmware stream took
 * the card off the bus (bugzilla 219552); the firmware alone got through.
 */
static unsigned int fw_rings_off;
module_param(fw_rings_off, uint, 0444);
MODULE_PARM_DESC(fw_rings_off, "Start the command rings only after the firmware download (1=yes; 0=no, default; diagnostic)");

/* the rings start at the usual point of the bring-up (the default) */
static bool ae9_rings_early(void)
{
	return corb_rings && !fw_rings_off;
}

static void creative_ae9_bridge_log(struct creative_chip *chip, const char *step)
{
	struct pci_dev *br = chip->pci->bus->self;
	u16 st = 0, sec = 0, ctl = 0, cmd = 0;
	const u16 errs = PCI_STATUS_DETECTED_PARITY | PCI_STATUS_SIG_SYSTEM_ERROR |
			 PCI_STATUS_REC_MASTER_ABORT | PCI_STATUS_REC_TARGET_ABORT |
			 PCI_STATUS_SIG_TARGET_ABORT | PCI_STATUS_PARITY;

	if (!bridge_log || !br)
		return;
	pci_read_config_word(br, PCI_STATUS, &st);
	pci_read_config_word(br, PCI_SEC_STATUS, &sec);
	pci_read_config_word(br, PCI_BRIDGE_CONTROL, &ctl);
	pci_read_config_word(chip->pci, PCI_COMMAND, &cmd);
	dev_info(chip->dev,
		 "bridge %s %04x:%04x %s: status 0x%04x, secondary 0x%04x%s%s%s%s%s, control 0x%04x; card command 0x%04x\n",
		 pci_name(br), br->vendor, br->device, step, st, sec,
		 sec & PCI_STATUS_REC_MASTER_ABORT ? " <MAbort" : "",
		 sec & PCI_STATUS_REC_TARGET_ABORT ? " <TAbort" : "",
		 sec & PCI_STATUS_SIG_TARGET_ABORT ? " >TAbort" : "",
		 sec & PCI_STATUS_SIG_SYSTEM_ERROR ? " <SERR" : "",
		 sec & (PCI_STATUS_DETECTED_PARITY | PCI_STATUS_PARITY) ? " parity" : "",
		 ctl, cmd);
	if (st & errs)
		pci_write_config_word(br, PCI_STATUS, st & errs);
	if (sec & errs)
		pci_write_config_word(br, PCI_SEC_STATUS, sec & errs);
}

static bool creative_ae9_alone_on_bus(struct pci_dev *pdev)
{
	struct pci_dev *d;

	list_for_each_entry(d, &pdev->bus->devices, bus_list)
		if (PCI_SLOT(d->devfn) != PCI_SLOT(pdev->devfn))
			return false;
	return true;
}

static int creative_ae9_hw_bringup(struct creative_chip *chip)
{
	struct pci_dev *pdev = chip->pci;
	bool do_reset = bus_reset > 0 || (bus_reset < 0 && !chip->asmedia);
	int ret;

	creative_ae9_log_state(chip, "before bring-up");
	chip->card_gone = false;
	creative_ae9_bridge_log(chip, "before bring-up");
	if (bus_reset < 0 && chip->asmedia)
		dev_info(&pdev->dev, "ASMedia bridge: skipping the secondary bus reset (it takes the bridge down at ring start; bus_reset=1 forces it)\n");

	if (do_reset && pdev->bus->self && !creative_ae9_alone_on_bus(pdev)) {
		dev_warn(&pdev->dev, "other devices behind %s: skipping the secondary bus reset\n",
			 pci_name(pdev->bus->self));
	} else if (do_reset && pdev->bus->self) {
		int rret;

		/*
		 * pci_reset_bus() would take the device lock we already hold
		 * during probe; toggle the bridge's secondary bus reset
		 * directly and restore our config space around it.
		 */
		u32 id = ~0U;
		int waited;

		pci_save_state(pdev);
		rret = pci_bridge_secondary_bus_reset(pdev->bus->self);
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
		pci_set_master(pdev);
		dev_info(&pdev->dev, "secondary bus reset via %s: %d, ready after %d ms (id 0x%08x)\n",
			 pci_name(pdev->bus->self), rret, waited, id);
		creative_ae9_log_state(chip, "after bus reset");
	}

	{
		u16 cfg40, after;

		pci_read_config_word(pdev, 0x40, &cfg40);
		if (pcicfg40_fix == 1 && (cfg40 & 0x8000))
			pci_write_config_word(pdev, 0x40, cfg40 & ~0x8000);
		else if (pcicfg40_fix == 2)
			/* cold-boot state (0x8080) until the codecs are detected */
			pci_write_config_word(pdev, 0x40, cfg40 | 0x8000);
		pci_read_config_word(pdev, 0x40, &after);
		dev_info(&pdev->dev, "PCI cfg 0x40: 0x%04x -> 0x%04x (pcicfg40_fix=%u)\n",
			 cfg40, after, pcicfg40_fix);
	}

	creative_ae9_bridge_log(chip, "after bus reset");
	if (creative_ae9_gone(chip, "after the bus reset"))
		return -ENODEV;

	/* Windows' BAR2 init, before the controller reset as in its cold boot */
	creative_ae9_bar2_init_windows(chip);

	/* Reset HDA controller */
	ret = creative_ae9_hw_reset(chip);
	creative_ae9_bridge_log(chip, "after controller reset");
	if (creative_ae9_gone(chip, "after the controller reset"))
		return -ENODEV;
	if (ret < 0)
		return ret;

	/* Enable unsolicited responses (only once the rings run, when
	 * they start late: without a RIRB they jam the immediate interface) */
	creative_hda_writel(chip, AZX_REG_GCTL, AZX_GCTL_RESET |
			    (ae9_rings_early() ? AZX_GCTL_UNSOL : 0));
	chip->unsol_parked = !ae9_rings_early();	/* late rings switch them on */
	if (codec_probe_delay_ms)
		msleep(codec_probe_delay_ms);

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
	 * the full old bring-up with codec1_failfast=0).
	 */
	if (load_fw && codec1_failfast &&
	    !(chip->codec_ok_mask & BIT(1))) {
		dev_warn(chip->dev, "codec 1 (DSP) not answering after the bus reset; trying the bring-up again\n");
		return 0;
	}

	/*
	 * DSP standup, in the order the CA0132 requires:
	 *   1. download firmware (DSP must be running before its config takes)
	 *   2. replay the captured codec init verbs (now applied to live DSP)
	 *   3. start the DSP internal audio-router streams
	 *   4. route DSP output to line-out
	 * Without load_fw only the old stream-enable and DSP I2C init below run.
	 */
	if (load_fw) {
		int fret;

		/* Chip init BEFORE firmware (replay sets up the DSP clocks/
		 * state the transfer needs; firmware-first fails -EIO).
		 * Done at probe — the DSP is freshly booted here and responds
		 * to ChipIO (at +20s it has gone idle and reads 0).
		 *
		 * CRITICAL: Clear QEMU SCP sentinel BEFORE replay_init.
		 * After QEMU device reset, EXRAM 0x800000-0x8000fc is filled
		 * with 0x0bad0add. The init verbs include SCP queue writes
		 * that rely on valid queue pointers at those addresses.
		 * If we don't clear first, the init verbs corrupt the queue.
		 * Windows clears this implicitly via its pre-init sequence.
		 */
		{
			unsigned int sentinel = 0;
			creative_dsp_chipio_read(chip, 0x800000, &sentinel);
			if (sentinel == 0x0bad0add && ae9_sentinel_clear) {
				unsigned int i;
				dev_info(chip->dev,
					 "Pre-replay: clearing QEMU SCP sentinel 0x800000-0x8000fc\n");
				for (i = 0; i < 64; i++)
					creative_dsp_chipio_write(chip,
						0x800000 + i * 4, 0x00000000);
			}
		}

		if (win_reset2) {
			int rret = creative_ae9_hw_reset(chip);
			int cret = 0;

			creative_ae9_bridge_log(chip, "after second controller reset");
			if (creative_ae9_gone(chip, "after the second controller reset"))
				return -ENODEV;
			creative_hda_writel(chip, AZX_REG_GCTL, AZX_GCTL_RESET |
					    (ae9_rings_early() ? AZX_GCTL_UNSOL : 0));
			chip->unsol_parked = !ae9_rings_early();	/* late rings switch them on */
			chip->rings_deferred = corb_rings && fw_rings_off;
			if (corb_rings && !fw_rings_off) {
				cret = creative_ae9_corb_init(chip);
				creative_ae9_bridge_log(chip, "after CORB/RIRB start");
			}
			dev_info(chip->dev,
				 "win_reset2: second controller reset %d, corb_init %d%s\n",
				 rret, cret, !corb_rings ? " (rings off: corb_rings=0)" :
				 fw_rings_off ? " (rings after the firmware: fw_rings_off=1)" : "");
			if (chip->card_gone)
				return -ENODEV;
		}

		creative_ae9_replay_both(chip);
		if (creative_ae9_gone(chip, "after the pre-download replay"))
			return -ENODEV;

		/* Check 0x801c after replay_init — in Windows capture the
		 * audio loop starts during or right after init verbs via ICW.
		 */
		{
			unsigned int loop_val = 0;
			creative_dsp_chipio_read(chip, 0x801c, &loop_val);
			dev_info(chip->dev,
				 "0x801c after replay_init: 0x%x%s\n",
				 loop_val,
				 loop_val ? " <-- AUDIO LOOP ALIVE!" : " (dead)");
		}

		/* Start CORB/RIRB here — after all ICW-based init verbs
		 * (replay_init above) but before firmware load and standup.
		 * Once CORB is running, dsp_read/dsp_write route through
		 * CORB (ICW is blocked by IRS=0x5 after CORB starts).
		 * This allows standup/select_lineout to send ChipIO verbs
		 * successfully via CORB rather than timing out via ICW.
		 */
		if (!win_reset2 && corb_rings && fw_rings_off)
			chip->rings_deferred = true;
		if (!win_reset2 && corb_rings && !fw_rings_off) {
			int cret = creative_ae9_corb_init(chip);

			creative_ae9_bridge_log(chip, "after CORB/RIRB start");
			dev_info(chip->dev, "early corb_init: %d\n", cret);
			if (chip->card_gone)
				return -ENODEV;
		}

		fret = creative_dsp_load_firmware(chip);
		if (chip->rings_deferred && !chip->card_gone) {
			/* the load failed before the stream finished */
			int cret = creative_ae9_corb_init(chip);

			chip->rings_deferred = false;
			dev_info(chip->dev, "deferred corb_init (load failed early): %d\n", cret);
		}
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
	}

	/* Stream enable */
	/* post_windows: the replay already set these up as Windows does;
	 * the DSP I2C init's CMD=0x30 re-init cuts off the head unit */
	if (!chip->dsp_done) {
		dev_info(chip->dev, "init: stream enable\n");
		writeb(0x3f, chip->ext_regs + CREATIVE_REG_STREAM_ENABLE);
		wmb();
	}

	/* DSP I2C controller init */
	if (!chip->dsp_done) {
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
 * resume_tries times. Only for the post_windows path with firmware.
 */
static void creative_ae9_win_shutdown(struct creative_chip *chip, const char *why);
static void creative_ae9_stop_works(struct creative_chip *chip);

static int creative_ae9_hw_bringup_retry(struct creative_chip *chip, const char *what)
{
	unsigned int attempt, tries = load_fw ? max(resume_tries, 1U) : 1;
	int ret = 0;

	for (attempt = 1; attempt <= tries; attempt++) {
		if (attempt > 1) {
			cancel_delayed_work_sync(&chip->acm_work);
			/* nothing to shut down if codec 1 never answered */
			if (chip->codec_ok_mask & BIT(1))
				creative_ae9_win_shutdown(chip, "bring-up retry");
			msleep(resume_delay_ms);
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
	else if (load_fw && !(chip->codec_ok_mask & BIT(1)))
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

	/* Look up BAR layout for this device variant */
	if (ent->driver_data >= ARRAY_SIZE(creative_ae9_bar_layouts)) {
		dev_err(&pdev->dev, "Unknown device variant %lu\n",
			(unsigned long)ent->driver_data);
		return -ENODEV;
	}
	layout = &creative_ae9_bar_layouts[ent->driver_data];

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

	/* 30-bit DMA mask — the CA0132 DSP may have only a 30-bit
	 * address bus (1 GB limit) for DMA.  The CORB/RIRB buffers
	 * must reside in the lowest 1 GB of system memory.
	 */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(30));
	if (ret < 0) {
		dev_err(&pdev->dev, "No suitable DMA mask available\n");
		return ret;
	}
	dev_info(&pdev->dev, "DMA: 30-bit mask, %s\n",
		 device_iommu_mapped(&pdev->dev) ? "through the IOMMU" : "no IOMMU translation");

	ret = pcim_iomap_regions(pdev, BIT(layout->hda_bar) | BIT(layout->ext_bar),
				 CREATIVE_AE9_DRV_NAME);
	if (ret < 0) {
		dev_err(&pdev->dev, "BAR mapping failed: %d\n", ret);
		return ret;
	}

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->pci = pdev;
	chip->dev = &pdev->dev;
	if (pdev->bus->self && pdev->bus->self->vendor == PCI_VENDOR_ID_ASMEDIA &&
	    pdev->bus->self->device == 0x1080)
		chip->asmedia = true;
	chip->dma_pad = (dma_pad < 0 ? (chip->asmedia ? 64 : 0) : dma_pad) * 1024;
	if (chip->asmedia || chip->dma_pad)
		dev_info(&pdev->dev, "bridge %s%s: %zu KB spare after each DMA buffer\n",
			 pdev->bus->self ? pci_name(pdev->bus->self) : "-",
			 chip->asmedia ? " (ASMedia ASM1083/1085)" : "",
			 chip->dma_pad / 1024);
	chip->hu_phantom_req = -1;	/* nothing to send */
	chip->hda_regs = pcim_iomap_table(pdev)[layout->hda_bar];
	chip->ext_regs = pcim_iomap_table(pdev)[layout->ext_bar];
	vlog_bar0 = chip->hda_regs;
	vlog_bar2 = chip->ext_regs;
	spin_lock_init(&chip->lock);
	mutex_init(&chip->corb_mutex);
	mutex_init(&chip->scp_mutex);
	mutex_init(&chip->replay_mutex);
	mutex_init(&chip->ca0113_mutex);

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
	 * Previously these were at the bottom of probe (after the load_fw block).
	 * The load_fw path calls queue_delayed_work(chip->wq, &chip->acm_work, 500ms)
	 * inside audio_loop_start/standup, then probe fell through to
	 * INIT_DELAYED_WORK which re-initialised (and silently CANCELLED) the
	 * already-queued timer. The ACM init never ran — no AE-9 display,
	 * no relay click, no audio. Moving INIT here fixes the race.
	 */
	INIT_DELAYED_WORK(&chip->dsp_switch_work, creative_ae9_dsp_switch_work);
	INIT_DELAYED_WORK(&chip->acm_work, creative_ae9_acm_work);
	INIT_DELAYED_WORK(&chip->hb_work, creative_ae9_hb_work);
	INIT_WORK(&chip->resume_work, creative_ae9_resume_work);
	atomic_set(&chip->dma_running, 0);

	dev_info(chip->dev,
		 "BAR%d (HDA)  at 0x%p, BAR%d (ext) at 0x%p\n",
		 layout->hda_bar, chip->hda_regs,
		 layout->ext_bar, chip->ext_regs);

	ret = creative_ae9_hw_bringup_retry(chip, "probe");
	if (ret < 0)
		goto err_hw;
	/* a DSP that never answered its boot handshake isn't a usable card */
	if (load_fw && !(chip->dsp_done && chip->hs_ok)) {
		dev_err(chip->dev, "DSP did not start after %u attempt(s); not registering the card (reload the driver or power-cycle)\n",
			max(resume_tries, 1U));
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
	if (pdev->subsystem_device == CREATIVE_SUBSYS_AE9) {
		strscpy(chip->card->shortname, "Sound Blaster AE-9",
			sizeof(chip->card->shortname));
		snprintf(chip->card->longname, sizeof(chip->card->longname),
			 "Creative Labs Sound Blaster AE-9 at 0x%llx irq %d",
			 (unsigned long long)pci_resource_start(pdev, 0),
			 pdev->irq);
	} else {
		strscpy(chip->card->shortname, "HDA Creative",
			sizeof(chip->card->shortname));
		snprintf(chip->card->longname, sizeof(chip->card->longname),
			 "Creative Sound Blaster AE-9 at 0x%llx irq %d",
			 (unsigned long long)pci_resource_start(pdev, 0),
			 pdev->irq);
	}

	/* PCM playback device (no interrupts — Windows-style) */
	if (enable_pcm) {
		ret = creative_ae9_pcm_init(chip);
		if (ret < 0)
			dev_warn(chip->dev, "PCM init failed: %d\n", ret);
	}

	creative_ae9_hu_controls_add(chip);
	if (enable_pcm) {
		chip->mic_boost = 1;
		snd_ctl_add(chip->card, snd_ctl_new1(&mic_boost_control, chip));
		chip->mic_level = 99;	/* +9 dB, as Windows' capture blocks */
		snd_ctl_add(chip->card, snd_ctl_new1(&mic_level_control, chip));
		snd_ctl_add(chip->card, snd_ctl_new1(&dac_filter_control, chip));
		creative_ae9_fx_init(chip);
		creative_ae9_fx_controls_add(chip);
	}

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

	if (!chip->corb_buf && corb_rings && !chip->card_gone) {
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

	ret = snd_card_register(chip->card);
	if (ret < 0) {
		dev_err(chip->dev, "snd_card_register failed: %d\n", ret);
		goto err_card;
	}

	g_dbg_chip = chip;
	chip->dbg_dir = debugfs_create_dir("creative_ae9", NULL);
	debugfs_create_file("cmd", 0600, chip->dbg_dir, NULL,
			    &creative_dbg_fops);
	if (vlog_buf)
		debugfs_create_file("verblog", 0400, chip->dbg_dir, NULL,
				    &vlog_seq_fops);

	dev_info(chip->dev, "Init complete: card %s\n", chip->card->longname);
	return 0;

err_card:
	snd_card_free(chip->card);
	chip->card = NULL;
err_hw:
	/* works the bring-up scheduled (ACM/heartbeat) must not outlive chip */
	creative_ae9_stop_works(chip);
	if (chip->corb_buf && !chip->card_gone)
		creative_ae9_win_shutdown(chip, "probe failure");
	else
		creative_ae9_corb_free(chip);
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
		}
		if (rirbsts & RIRB_INT_OVERRUN) {
			/* Clear overrun, then resync RIRB */
			writeb(rirbsts | RIRB_INT_OVERRUN,
			       chip->hda_regs + AZX_REG_RIRBSTS);
			wmb();
		}
	}

	/* input streams: 0 capture (XLR), 1 optical in, 2 What U Hear */
	if (intsts & 0x1)
		creative_ae9_cap_irq(chip);
	if (intsts & 0x2)
		creative_ae9_xcap_irq(chip, 0);
	if (intsts & 0x4)
		creative_ae9_xcap_irq(chip, 1);

	/* Acknowledge stream interrupts and notify ALSA of period completion */
	if (intsts & AZX_INT_ALL_STREAM & ~0x7) {
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
	if (chip->cap_start_sent) {
		creative_ae9_cap_replay(chip, false);
		chip->cap_start_sent = 0;
	}
	if (chip->dsp_active)
		creative_ae9_dsp_mode(chip, false);	/* streams 0x18, 0x05 off */
}

static void creative_ae9_win_shutdown(struct creative_chip *chip, const char *why)
{
	u32 ctl;

	creative_ae9_streams_down(chip);
	dev_info(chip->dev, "shutdown (%s): Windows power-down sequence\n", why);
	/* stop capture (SD0) and playback (SD4) DMA */
	ctl = readl(chip->hda_regs + 0x80 + AZX_REG_SD_CTL);
	writel((ctl & ~SD_CTL_DMA_START) & SD_CTL_WMASK, chip->hda_regs + 0x80 + AZX_REG_SD_CTL);
	ctl = readl(chip->hda_regs + 0x100 + AZX_REG_SD_CTL);
	writel((ctl & ~SD_CTL_DMA_START) & SD_CTL_WMASK, chip->hda_regs + 0x100 + AZX_REG_SD_CTL);

	if (chip->corb_buf) {
		creative_ae9_ops_replay(chip, ae9_shutdown_a, ARRAY_SIZE(ae9_shutdown_a),
					"shutdown: power-down");
		msleep(100);
		creative_ae9_ops_replay(chip, ae9_shutdown_b, ARRAY_SIZE(ae9_shutdown_b),
					"shutdown: codecs to D3");
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

static void creative_ae9_stop_works(struct creative_chip *chip)
{
	cancel_work_sync(&chip->resume_work);
	atomic_set(&chip->dma_running, 0);
	cancel_delayed_work_sync(&chip->hb_work);
	cancel_delayed_work_sync(&chip->acm_work);
	cancel_delayed_work_sync(&chip->dsp_switch_work);
}

/* Reboot / power-off: same as Windows shutting down. */
static void creative_ae9_shutdown(struct pci_dev *pdev)
{
	struct creative_chip *chip = pci_get_drvdata(pdev);

	if (!chip || chip->suspended)
		return;
	creative_ae9_stop_works(chip);
	chip->suspended = true;		/* IRQ handler: hands off */
	creative_ae9_win_shutdown(chip, "reboot/power-off");
}

static void creative_ae9_remove(struct pci_dev *pdev)
{
	struct creative_chip *chip = pci_get_drvdata(pdev);

	if (!chip)
		return;

	creative_ae9_stop_works(chip);
	debugfs_remove_recursive(chip->dbg_dir);
	g_dbg_chip = NULL;
	vlog_bar0 = vlog_bar2 = NULL;
	vfree(vlog_buf);
	vlog_buf = NULL;

	/* card first: it closes open streams while the hardware still runs */
	if (chip->card) {
		snd_card_free(chip->card);
		chip->card = NULL;
	}

	/* Windows' shutdown */
	if (!chip->suspended)
		creative_ae9_win_shutdown(chip, "unload");

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
	snd_power_change_state(chip->card, SNDRV_CTL_POWER_D3hot);
	atomic_set(&chip->dma_running, 0);
	cancel_delayed_work_sync(&chip->hb_work);
	cancel_delayed_work_sync(&chip->acm_work);
	cancel_delayed_work_sync(&chip->dsp_switch_work);
	creative_ae9_streams_down(chip);
	chip->dsp_active = false;
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
	unsigned int want_hp = chip->hu_output, attempt;
	int ret = -EIO;

	chip->dac_start_key = 0;	/* the card lost power */

	/* the boot replay brings the head unit up on speakers */
	chip->hu_att_out[chip->hu_output] = chip->hu_att;
	chip->hu_output = 0;
	chip->hu_att = chip->hu_att_out[0];

	for (attempt = 1; attempt <= max(resume_tries, 1U); attempt++) {
		if (attempt > 1) {
			/* undo the failed attempt with Windows' shutdown: a
			 * reload (shutdown + bring-up) recovers a DSP that
			 * missed its handshake; a bare second bring-up didn't */
			cancel_delayed_work_sync(&chip->acm_work);
			cancel_delayed_work_sync(&chip->hb_work);
			chip->suspended = true;
			creative_ae9_win_shutdown(chip, "resume retry");
		}
		msleep(resume_delay_ms);
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

	/* the heartbeat (started by the bring-up's ACM work) applies these */
	chip->hu_dirty = 1;
	if (want_hp)
		chip->hu_switch_to = 1;
	/* the head unit wakes lit; if the light was off, the heartbeat turns
	 * it off again (sending it from here raced the heartbeat's frames) */
	chip->hu_light_sent = 1;
	chip->hu_sbx_led = -1;		/* heartbeat sends it again */
	creative_ae9_mic_boost_apply(chip);
	if (chip->mic_level != 99)
		creative_ae9_mic_level_apply(chip);
	creative_ae9_dac_filter_apply(chip);
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
