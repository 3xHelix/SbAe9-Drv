// SPDX-License-Identifier: GPL-2.0-only
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

MODULE_AUTHOR("openco de");
MODULE_DESCRIPTION(CREATIVE_AE9_DRV_DESC);
MODULE_LICENSE("GPL");
MODULE_VERSION(CREATIVE_AE9_DRV_VERSION);

/*
 * init_mask: bitmask gating each BAR2-touching init stage, for bisecting
 * which write turns off the card LEDs.
 *   bit 0 (0x01): VFIO BAR2 init (Phase 1-5)
 *   bit 1 (0x02): HW_CTRL toggle (0x1c)
 *   bit 2 (0x04): stream enable (0x304)
 *   bit 3 (0x08): DSP I2C controller init (0xC00 area)
 *   bit 4 (0x10): ACM I2C init (29 transactions)
 * HDA reset, codec detect, D0 power, and GPIO verbs always run.
 * Default 0x00 = no BAR2 writes (LED-safe baseline).
 */
static unsigned int init_mask = 0x1f;
module_param(init_mask, uint, 0444);
MODULE_PARM_DESC(init_mask, "Bitmask of BAR2 init stages (see source)");

/*
 * vfio_mask: gates each phase of the VFIO BAR2 init (when init_mask bit 0 set):
 *   bit 0 (0x01): Phase 1 stream status clear (0x4xc/0x4x c)
 *   bit 1 (0x02): Phase 2 stream enable (0x400/0x408/0x40c/0x410)
 *   bit 2 (0x04): Phase 3 scratch 0x100 (0x600/0x60f/0x70f)
 *   bit 3 (0x08): Phase 4 DSP boot (0x830/0x86c/0x800=0x6b/0x89c)
 *   bit 4 (0x10): Phase 5 preserve (0x800/0x804/0x20c)
 * Default 0x1b = all phases EXCEPT P3 (0x100 scratch writes turn off
 * the card LEDs on the AE-9; 0x60f/0x70f are the culprits).
 */
static unsigned int vfio_mask = 0x1b;
module_param(vfio_mask, uint, 0444);
MODULE_PARM_DESC(vfio_mask, "Bitmask of VFIO BAR2 init phases (see source)");

/*
 * enable_pcm: register the PCM playback device. No HDA interrupts are used
 * (Windows runs playback with global interrupts disabled), avoiding the
 * level-triggered INTx storm that previously hard-locked the system.
 */
static unsigned int enable_pcm = 1;
module_param(enable_pcm, uint, 0444);
MODULE_PARM_DESC(enable_pcm, "Register PCM playback device (1=on, default)");

/*
 * full_init: replay the captured Windows codec init verb sequence (via ICW)
 * after our base init. Bit 0 = codec 1, bit 1 = codec 2. Default 0 (off) so
 * the known-good LED/display boot is unaffected until validated.
 */
static unsigned int full_init;
module_param(full_init, uint, 0444);
MODULE_PARM_DESC(full_init, "Replay captured codec init verbs (1=cad1, 2=cad2, 3=both)");

/*
 * load_fw: download the DSP firmware (ctefx-desktop.bin) during probe and
 * stand up the DSP (firmware -> codec verb replay -> DSP audio streams ->
 * output routing), in the order the DSP requires.
 */
static unsigned int load_fw = 1;
module_param(load_fw, uint, 0444);
MODULE_PARM_DESC(load_fw, "Download DSP firmware + stand up DSP at probe (1=on, default)");

/*
 * Head-unit MCU boots a few seconds after card power-up. The I2C-bridge
 * STS goes "ready" almost immediately (that's the bridge, not the MCU),
 * so we cannot poll for true MCU readiness. Instead, delay the first ACM
 * display init until acm_first_delay_ms after probe, then run acm_passes
 * total passes spaced acm_pass_gap_ms apart. Tunable without recompiling.
 */
static unsigned int acm_first_delay_ms = 12000;
module_param(acm_first_delay_ms, uint, 0444);
MODULE_PARM_DESC(acm_first_delay_ms, "Delay before first ACM head-unit init (ms)");

static unsigned int acm_pass_gap_ms = 4000;
module_param(acm_pass_gap_ms, uint, 0444);
MODULE_PARM_DESC(acm_pass_gap_ms, "Gap between ACM head-unit init passes (ms)");

static unsigned int acm_passes = 2;
module_param(acm_passes, uint, 0444);
MODULE_PARM_DESC(acm_passes, "Number of ACM head-unit init passes");

/*
 * After the init passes, poll the head unit every acm_heartbeat_ms to keep
 * the MCU/DAC alive (capture: ~500ms, runs during audio playback). 0=off.
 */
static unsigned int acm_heartbeat_ms = 500;
module_param(acm_heartbeat_ms, uint, 0444);
MODULE_PARM_DESC(acm_heartbeat_ms, "Head-unit heartbeat poll interval (ms, 0=off)");

/* Engage the analog output path (DAC route + amp enable) after ACM init. */
static unsigned int enable_audio_path = 1;
module_param(enable_audio_path, uint, 0444);
MODULE_PARM_DESC(enable_audio_path, "Run DAC route + amp enable after ACM init (1=on)");

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
 * The Windows boot capture configures codec 2 (head unit, CORB #120-364)
 * before codec 1's main init and DSP setup (#365 onward). Another AE-9
 * driver effort (bugzilla 219552 #53) also needed codec 2 first.
 */
static unsigned int codec2_first = 1;
module_param(codec2_first, uint, 0444);
MODULE_PARM_DESC(codec2_first, "Replay codec 2 init verbs before codec 1, as Windows does (1=on, default)");

static unsigned int hu_boot_ms;
module_param(hu_boot_ms, uint, 0444);
MODULE_PARM_DESC(hu_boot_ms, "GPIO5 on + wait this many ms before audio_loop_start probes (0=off)");

/*
 * fw_split: ae9_initseq_codec1 is Windows' codec-1 boot stream, including
 * its firmware-download procedure, the DSP start (DBGCNTL run) and the
 * MASTERCONTROL boot handshake. Replaying it whole before our own download
 * starts the DSP with no firmware. With fw_split=1 only the pre-download
 * part is replayed at probe/self-reset; after our firmware is running we
 * run the boot handshake properly (polling the DSP's replies) and then
 * replay Windows' post-handshake setup (the table tail). See
 * WINDOWS-CAPTURE-ANALYSIS.md, sections 1 and 4.
 */
static unsigned int fw_split = 1;
module_param(fw_split, uint, 0444);
MODULE_PARM_DESC(fw_split, "Split the codec-1 replay around our firmware download + run the DSP boot handshake (1=on, default)");

/*
 * bar2_first: the cold-boot Windows capture (vm_capture_coldboot_20261101_
 * 160959) does the whole BAR2 init *before* the HDA controller reset, and
 * its codecs then report subsystem 0x11020071/0x11020072 (BAR2+0x4 reads
 * 0x7172000a just before the reset). We reset first and init BAR2 after,
 * and our codecs report the default 0x11020010.
 */
static unsigned int bar2_first = 1;
module_param(bar2_first, uint, 0444);
MODULE_PARM_DESC(bar2_first, "Run Windows' exact BAR2 init before the HDA controller reset (1=on, default)");

/*
 * cold_prefw: replay the cold-boot capture's pre-download verbs
 * (ae9_cold_prefw, both codecs interleaved as Windows sent them) instead
 * of the codec-2 table + warm codec-1 prefix, and skip our own AFG
 * power-up and GPIO verbs, which Windows doesn't send before the
 * download. Use with fw_split (post-download handshake + tail).
 */
static unsigned int cold_prefw = 1;
module_param(cold_prefw, uint, 0444);
MODULE_PARM_DESC(cold_prefw, "Replay the cold-boot Windows pre-download verbs instead of the warm tables (1=on, use with fw_split)");

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
static unsigned int bus_reset = 1;
module_param(bus_reset, uint, 0444);
MODULE_PARM_DESC(bus_reset, "Secondary bus reset of the card at probe, as a VM start does (1=on, default)");

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

/*
 * post_windows=1: after the boot handshake + tail, skip our own post-boot
 * setup (standup, DAC enable, CA0113 handshake, amp/route, ACM work's
 * second reset / HP output preset) and replay Windows' BAR2 activity from
 * the same point of the cold capture instead (CA0113 commands, head-unit
 * SPI frames incl. the "-SP-" speaker label, GPIO writes). The heartbeat
 * stays.
 */
static unsigned int post_windows = 1;
module_param(post_windows, uint, 0444);
MODULE_PARM_DESC(post_windows, "Replay Windows' post-boot BAR2 activity instead of our setup (1=on, default)");

/* bring-up attempts if the DSP misses its handshake (probe and resume),
 * and the wait before each resume attempt / between retries */
static unsigned int resume_delay_ms = 2000;
module_param(resume_delay_ms, uint, 0644);
MODULE_PARM_DESC(resume_delay_ms, "Wait before each resume bring-up attempt and between retries (ms)");
static unsigned int resume_tries = 3;
module_param(resume_tries, uint, 0644);
MODULE_PARM_DESC(resume_tries, "Bring-up attempts (probe and resume) if the DSP doesn't answer");

static unsigned int loop_start;
module_param(loop_start, uint, 0444);
MODULE_PARM_DESC(loop_start, "Run the post-firmware audio_loop_start SPI probes (0=skip, default; saves ~80s when they time out)");

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

	if (!fw_split)
		return false;
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
	}
	creative_dsp_chipio_addr_invalidate();
	if (!atomic) {
		mutex_unlock(&chip->replay_mutex);
		dev_info(chip->dev, "%s, %zu verbs (%u errors), %u SCP msgs (%u failed)\n",
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
 * Per-rate values for the playback start block (pcm_rates=1). The block
 * was captured at 48 kHz; for another rate the connection point 0xd0 rate
 * code, node 0x02's converter format and the I2S clock descriptors
 * (0x189000/04/08, 0x189024) are patched, and BAR2 0x100 bits 31:29 get the
 * I2S rate code. Descriptors and rate codes are from DarkNikGr/sb-ae9's
 * decompile of Windows' I2SM Start and their measurements; the 48 kHz row
 * is exactly what our capture writes, and the startup tail writes the
 * 96 kHz descriptors. Connection point codes are ca0132's sample-rate enum.
 * 176.4 kHz is left out (reported silent), 384 kHz can't cross the HDA link.
 * The 44.1/88.2 kHz rows play sharp and crackly on our card even with the
 * connection point 0x70 rate and full PLL tail below, so the PCM doesn't
 * offer them (pcm_rates); kept for when a capture shows the missing step.
 */
static const struct {
	unsigned int rate;
	u8 cp_code;	/* connection point sample-rate code */
	u32 i2s_clk;	/* 0x189000/04/08 */
	u32 i2s_rate;	/* 0x189024 */
	u8 bar2_code;	/* BAR2 0x100 bits 31:29 */
} ae9_pcm_rates[] = {
	{  44100, 0x08, 0x0001f100, 0x00008005, 1 },
	{  48000, 0x09, 0x0001f100, 0x00008005, 0 },
	{  88200, 0x0a, 0x0001f101, 0x00014004, 3 },
	{  96000, 0x0b, 0x0001f101, 0x00014004, 2 },
	{ 192000, 0x0e, 0x0001f102, 0x00022003, 4 },
};

/* ASI 8, connection point 0x70's rate (startup sets 96 kHz; entry 6 is
 * patched), Windows' full PLL/PMU program (as at boot), ASI 0xf. Verbs in
 * the captured block's own encoding: 0x717/0x718 = ChipIO param id/value
 * (0x1d select connection point, 0x1e its rate), 0x70d/0x70c = 8051
 * PLL/PMU address/data, 0xf01 = status poll. */
#define AE9_RATE_TAIL_CP70	6
static const u32 ae9_rate_pll_tail[] = {
	0x115f0100, 0x11571717, 0x11571808,
	0x1157171d, 0x11571870, 0x1157171e, 0x1157180b,
	0x11570d44, 0x11570cc8, 0x11570d43, 0x11570ccc,
	0x11570d45, 0x11570ccb, 0x11570d40, 0x11570cc7,
	0x11570d42, 0x11570ccd, 0x11570d41, 0x11570cce,
	0x11570d51, 0x11570cdb,
	0x115f0100, 0x11571717, 0x1157180f,
};

void creative_ae9_cold_pcm_replay(struct creative_chip *chip, bool start, bool atomic,
				  unsigned int rate)
{
	bool hp = chip->hu_output;

	if (!chip->corb_buf)
		return;
	if (start && !atomic && ae9_ca0113_windows) {
		u8 cmds[ARRAY_SIZE(ae9_cold_ca0113_pcm)][3];
		size_t i;

		memcpy(cmds, ae9_cold_ca0113_pcm, sizeof(cmds));
		for (i = 0; i < ARRAY_SIZE(cmds); i++) {
			if (cmds[i][0] == 0x48 && cmds[i][1] == 0x1d)
				cmds[i][2] = hp ? 0x80 : 0x40;
			else if (cmds[i][0] == 0x48 && cmds[i][1] == 0x19)
				cmds[i][2] = hp ? 0x01 : 0x00;
		}
		creative_dsp_ca0113_replay(chip, cmds, ARRAY_SIZE(cmds),
					   "playback-start commands");
	}
	if (start) {
		u32 *t = kmemdup(ae9_cold_pcm_start, sizeof(ae9_cold_pcm_start),
				 atomic ? GFP_ATOMIC : GFP_KERNEL);
		unsigned int r = 1;	/* the 48 kHz row: the block as captured */
		u32 addr = 0, data = 0;
		size_t i;
		u32 b100;

		if (!t)
			return;
		for (i = 0; i < ARRAY_SIZE(ae9_pcm_rates); i++)
			if (ae9_pcm_rates[i].rate == rate)
				r = i;
		if (ae9_pcm_rates[r].rate != rate && rate)
			dev_warn(chip->dev, "pcm_rates: no clock setup for %u Hz, using 48 kHz\n",
				 rate);
		b100 = creative_ext_readl(chip, 0x100);
		creative_ext_writel(chip, 0x100, (b100 & 0x1fffffff) |
				    ((u32)ae9_pcm_rates[r].bar2_code << 29));
		if (ae9_pcm_rates[r].rate != 48000)
			dev_info(chip->dev, "pcm_rates: playback at %u Hz (rate code 0x%02x, I2S 0x%05x/0x%05x, BAR2 0x100 0x%08x -> 0x%08x)\n",
				 ae9_pcm_rates[r].rate, ae9_pcm_rates[r].cp_code,
				 ae9_pcm_rates[r].i2s_clk, ae9_pcm_rates[r].i2s_rate,
				 b100, creative_ext_readl(chip, 0x100));
		for (i = 0; i < ARRAY_SIZE(ae9_cold_pcm_start); i++) {
			u32 v = t[i];

			/* ChipIO memory writes: address lo/hi, then data lo/hi */
			if ((v & 0xffff0000) == 0x11500000)
				addr = (addr & 0xffff0000) | (v & 0xffff);
			else if ((v & 0xffff0000) == 0x11510000)
				addr = (addr & 0xffff) | ((v & 0xffff) << 16);
			if (addr == 0x189000 || addr == 0x189004 || addr == 0x189008)
				data = ae9_pcm_rates[r].i2s_clk;
			else if (addr == 0x189024)
				data = ae9_pcm_rates[r].i2s_rate;
			else
				data = 0;
			if (data && (v & 0xffff0000) == 0x11530000) {
				t[i] = 0x11530000 | (data & 0xffff);
				continue;
			}
			if (data && (v & 0xffff0000) == 0x11540000) {
				t[i] = 0x11540000 | (data >> 16);
				continue;
			}
			/* node 0x02 converter format (captured 0x0031) */
			if (v == 0x10220031) {
				t[i] = 0x10220000 | creative_ae9_stream_format(2, 24,
						ae9_pcm_rates[r].rate);
				continue;
			}
			/* connection point 0xd0 rate code (captured 0x09) */
			if (i && v == 0x11571809 && t[i - 1] == 0x1157171e) {
				t[i] = 0x11571800 | ae9_pcm_rates[r].cp_code;
				continue;
			}

			/* ChipIO 0x718 data after 0x717 index 0x0d: output select */
			if (i && (v & 0xffffff00) == 0x11571800 &&
			    t[i - 1] == 0x1157170d)
				t[i] = (v & 0xffffff00) | (hp ? 0xa1 : 0xa4);
			/* node 0x12 (XLR) boost: keep the user's setting */
			else if ((v & 0xfffff0ff) == 0x11236001 ||
				 (v & 0xfffff0ff) == 0x11235001)
				t[i] = (v & 0xffffff00) | chip->mic_boost;
		}
		creative_ae9_verb_block_replay(chip, t, ARRAY_SIZE(ae9_cold_pcm_start),
					       atomic, hp ?
					       "pcm_windows: playback start block (headphones)" :
					       "pcm_windows: playback start block");
		kfree(t);
		/*
		 * The captured block only trims the PLL (0x40/0x42/0x41) before
		 * the descriptors. Windows' rate change ends with the rate code,
		 * the full PLL program and ASI on (I2SM Start step 8). Without it
		 * the 44.1 kHz family stays on the 48 kHz clock (sharp, crackly).
		 */
		if (ae9_pcm_rates[r].rate != 48000 || chip->pcm_reclocked) {
			u32 tail[ARRAY_SIZE(ae9_rate_pll_tail)];
			bool fresh = ae9_pcm_rates[r].rate == 48000;

			memcpy(tail, ae9_rate_pll_tail, sizeof(tail));
			/* back at 48 kHz: 0x70 to 96 kHz (0x0b), as at boot */
			tail[AE9_RATE_TAIL_CP70] = 0x11571800 |
				(fresh ? 0x0b : ae9_pcm_rates[r].cp_code);
			creative_ae9_verb_block_replay(chip, tail, ARRAY_SIZE(tail), atomic,
						       fresh ?
						       "pcm_rates: clocks back to the boot setup" :
						       "pcm_rates: connection point 0x70 + full PLL program");
			chip->pcm_reclocked = !fresh;
		}
	}
	else
		creative_ae9_verb_block_replay(chip, ae9_cold_pcm_stop,
					       ARRAY_SIZE(ae9_cold_pcm_stop), atomic,
					       "pcm_windows: playback stop block");
}

/* Windows' record start (+ capture level) or stop block, from the XLR
 * recording capture (creative_ae9_cap_verbs.h). */
void creative_ae9_cap_replay(struct creative_chip *chip, bool start)
{
	if (!chip->corb_buf)
		return;
	if (start) {
		creative_ae9_verb_block_replay(chip, ae9_cap_level,
					       ARRAY_SIZE(ae9_cap_level), false,
					       "capture: level block");
		creative_ae9_verb_block_replay(chip, ae9_cap_start,
					       ARRAY_SIZE(ae9_cap_start), false,
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

/* Replay both codecs' captured init verbs via ICW, in the codec2_first order. */
static void creative_ae9_replay_both(struct creative_chip *chip)
{
	size_t n1 = creative_ae9_codec1_prefw_count(chip);

	if (cold_prefw && creative_ae9_fw_split_ok(chip)) {
		creative_ae9_replay_cold(chip);
		return;
	}

	if (n1 != ARRAY_SIZE(ae9_initseq_codec1))
		dev_info(chip->dev, "fw_split: replaying codec 1 entries 0-%zu only (pre-download)\n",
			 n1 - 1);
	if (codec2_first) {
		dev_info(chip->dev, "replaying codec 2 before codec 1 (codec2_first=1)\n");
		creative_ae9_replay_init(chip, 2, ae9_initseq_codec2,
					 ARRAY_SIZE(ae9_initseq_codec2));
		creative_ae9_replay_init(chip, 1, ae9_initseq_codec1, n1);
	} else {
		creative_ae9_replay_init(chip, 1, ae9_initseq_codec1, n1);
		creative_ae9_replay_init(chip, 2, ae9_initseq_codec2,
					 ARRAY_SIZE(ae9_initseq_codec2));
	}
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

static void creative_ae9_post_fw(struct creative_chip *chip)
{
	struct tail_msg *retry;
	unsigned int nretry = 0;
	u32 words[1 + 32];
	unsigned int n = 0, msgs = 0, msg_err = 0, verbs = 0, verb_err = 0;
	unsigned int consec_err = 0, msg_skipped = 0;
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
					if (consec_err >= 8) {
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
		 "fw_split: tail replay done: %u SCP msgs (%u failed, %u skipped), %u verbs (%u failed); DBGCNTL 0x%08x\n",
		 msgs, msg_err, msg_skipped, verbs, verb_err, dbg);
	creative_ae9_dsp_health(chip, dbg);
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

	if (post_windows)
		creative_ae9_bar2_post_replay(chip);
	else if (ae9_ca0113_windows)
		creative_dsp_ca0113_replay(chip, ae9_cold_ca0113_boot,
					   ARRAY_SIZE(ae9_cold_ca0113_boot),
					   "boot commands");
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

static unsigned int creative_ae9_corb_replay(struct creative_chip *chip,
					     unsigned int cad,
					     const struct ae9_initverb *seq,
					     size_t count)
{
	unsigned int errs = 0;
	size_t i;

	for (i = 0; i < count; i++) {
		u32 resp = 0;
		u32 verb = creative_ae9_verb(cad, seq[i].nid, seq[i].verb,
					     seq[i].payload);
		if (creative_ae9_corb_send_verb_sync(chip, verb, &resp) < 0)
			errs++;
		udelay(40);
	}
	return errs;
}

/* Public wrapper — replay all init verbs for both codecs via CORB.
 * Used by the self-reset path after hw_reset + corb_init: at that
 * point CORB is already running so ICW (creative_ae9_replay_init) is
 * blocked (IRS stuck at 0x0005). Send via corb_send_verb_sync instead. */
void creative_ae9_replay_init_all(struct creative_chip *chip)
{
	unsigned int errs = 0;

	if (cold_prefw && creative_ae9_fw_split_ok(chip)) {
		dev_info(chip->dev, "self-reset: replaying cold pre-download verbs via CORB\n");
		creative_ae9_replay_cold(chip);
		return;
	}

	dev_info(chip->dev, "self-reset: replaying init verbs via CORB%s\n",
		 codec2_first ? " (codec 2 first)" : "");

	if (codec2_first)
		errs += creative_ae9_corb_replay(chip, 2, ae9_initseq_codec2,
						 ARRAY_SIZE(ae9_initseq_codec2));
	errs += creative_ae9_corb_replay(chip, 1, ae9_initseq_codec1,
					 creative_ae9_codec1_prefw_count(chip));
	if (!codec2_first)
		errs += creative_ae9_corb_replay(chip, 2, ae9_initseq_codec2,
						 ARRAY_SIZE(ae9_initseq_codec2));
	dev_info(chip->dev, "self-reset: init verb replay done (%u errors)\n",
		 errs);
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

/* AE-5/AE-7 initialization arrays from kernel patch_ca0132.c */
static const unsigned int ca0113_mmio_init_address_ae5[] = {
	0x400, 0x42c, 0x46c, 0x4ac, 0x4ec, 0x43c, 0x47c, 0x4bc, 0x4fc, 0x408,
	0x100, 0x410, 0x40c, 0x100, 0x100, 0x830, 0x86c, 0x800, 0x86c, 0x800,
	0x804, 0x20c, 0x01c, 0xc0c, 0xc00, 0xc04, 0xc0c, 0xc0c, 0xc0c, 0xc0c,
	0xc08, 0xc08, 0xc08, 0xc08, 0xc08, 0xc04, 0x01c
};

static const unsigned int ca0113_mmio_init_data_ae5[] = {
	0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000001,
	0x00000600, 0x00000014, 0x00000001, 0x0000060f, 0x0000070f,
	0x00000aff, 0x00000000, 0x0000006b, 0x00000001, 0x0000006b,
	0x00000057, 0x00800000, 0x00880680, 0x00000080, 0x00000030,
	0x00000000, 0x00000000, 0x00000003, 0x00000003, 0x00000003,
	0x00000001, 0x000000f1, 0x00000001, 0x000000c7, 0x000000c1,
	0x00000080, 0x00880680
};

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

static int creative_ae9_bar2_init(struct creative_chip *chip)
{
	dev_dbg(chip->dev, "BAR2 extended register init (AE-9 inline)\n");

	/*
	 * AE-9 inlined init — matches ca0132_mmio_init_ae9() exactly.
	 * Does NOT pre-write 0x01c before the table (unlike AE-7).
	 * Uses AE-9 specific values (confirmed from ae9_build + VM trace).
	 * No readl() on 0xC00-0xC7F (Windows never reads these).
	 */

	/* Indices 0-8: stream descriptors */
	creative_ext_writel(chip, 0x400, 0x00000001); creative_ext_readl(chip, 0x400);
	creative_ext_writel(chip, 0x42c, 0x00000000); creative_ext_readl(chip, 0x42c);
	creative_ext_writel(chip, 0x46c, 0x00000000); creative_ext_readl(chip, 0x46c);
	creative_ext_writel(chip, 0x4ac, 0x00000000); creative_ext_readl(chip, 0x4ac);
	creative_ext_writel(chip, 0x4ec, 0x00000000); creative_ext_readl(chip, 0x4ec);
	creative_ext_writel(chip, 0x43c, 0x00000000); creative_ext_readl(chip, 0x43c);
	creative_ext_writel(chip, 0x47c, 0x00000000); creative_ext_readl(chip, 0x47c);
	creative_ext_writel(chip, 0x4bc, 0x00000000); creative_ext_readl(chip, 0x4bc);
	creative_ext_writel(chip, 0x4fc, 0x00000000); creative_ext_readl(chip, 0x4fc);
	/* Indices 9-15: stream/clock control */
	creative_ext_writel(chip, 0x408, 0x00000001); creative_ext_readl(chip, 0x408);
	creative_ext_writel(chip, 0x100, 0x00000600); creative_ext_readl(chip, 0x100);
	creative_ext_writel(chip, 0x410, 0x00000014); creative_ext_readl(chip, 0x410);
	creative_ext_writel(chip, 0x40c, 0x00000001); creative_ext_readl(chip, 0x40c);
	creative_ext_writel(chip, 0x100, 0x0000060f); creative_ext_readl(chip, 0x100);
	creative_ext_writel(chip, 0x100, 0x0000070f); creative_ext_readl(chip, 0x100);
	creative_ext_writel(chip, 0x830, 0x00000aff); creative_ext_readl(chip, 0x830);
	/* Indices 16-20: DMA reset/enable + I2S clock + group */
	creative_ext_writel(chip, 0x86c, 0x00000000); creative_ext_readl(chip, 0x86c);
	creative_ext_writel(chip, 0x800, 0x0000006b); creative_ext_readl(chip, 0x800);
	creative_ext_writel(chip, 0x86c, 0x00000001); creative_ext_readl(chip, 0x86c);
	creative_ext_writel(chip, 0x800, 0x0000006b); creative_ext_readl(chip, 0x800);
	creative_ext_writel(chip, 0x804, 0x00000057); creative_ext_readl(chip, 0x804);

	/*
	 * AE-9: I2C bus init — MUST happen here, immediately after 0x804=0x57.
	 * ae9_build ca0132_mmio_init_ae9(): "Windows VFIO Phase 3 shows this
	 * sequence immediately after 0x804=0x57, BEFORE the 0x20c/0x1c writes."
	 * No readl() on 0xC00-0xC7F — Windows never reads these.
	 */
	creative_ext_writel(chip, 0xC0C, 0x80);
	creative_ext_writel(chip, 0xC00, 0x30);
	creative_ext_writel(chip, 0xC04, 0x00);
	creative_ext_writel(chip, 0xC0C, 0x00);
	creative_ext_writel(chip, 0xC0C, 0x03);
	creative_ext_writel(chip, 0xC0C, 0x03);
	creative_ext_writel(chip, 0xC0C, 0x03);
	creative_ext_writel(chip, 0xC08, 0x01);
	creative_ext_writel(chip, 0xC08, 0xf1);
	creative_ext_writel(chip, 0xC08, 0x01);
	creative_ext_writel(chip, 0xC08, 0xc7);
	creative_ext_writel(chip, 0xC08, 0xc1);
	creative_ext_writel(chip, 0xC04, 0x80);

	/* Index 21: 0x20c reset — AE-9 uses 0x00800000 (not AE-7's 0x00800001) */
	creative_ext_writel(chip, 0x20c, 0x00800000); creative_ext_readl(chip, 0x20c);
	/* Index 22: 0x01c I2S clock — AE-9 values 0x480/0x880480 (not AE-7's 0x680) */
	creative_ext_writel(chip, 0x01c, 0x00000480); creative_ext_readl(chip, 0x01c);
	creative_ext_writel(chip, 0x01c, 0x00880480); creative_ext_readl(chip, 0x01c);

	dev_dbg(chip->dev, "BAR2 init complete (AE-9 inline)\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* BAR2 init: VFIO-capture-based (Phase 1-5)			  */
/* This is the sequence that previously produced ACM display output. */
/* ------------------------------------------------------------------ */
void creative_ae9_hw_init(struct creative_chip *chip)
{
	u32 val;
	int s;
	unsigned int timeout;

	/* Phase 1: Stream status clear (4 streams, stride 0x40) */
	if (vfio_mask & 0x01) {
		dev_info(chip->dev, "vfio: P1 stream status clear\n");
		for (s = 0; s < CREATIVE_BAR2_NUM_STREAMS; s++) {
			unsigned int base = 0x400 + s * CREATIVE_BAR2_STREAM_STRIDE;

			val = creative_ext_readl(chip, base + 0x2c);
			if (val & BIT(0)) {
				creative_ext_writel(chip, base + 0x2c, 0x0);
				creative_ext_readl(chip, base + 0x2c);
			}
			creative_ext_writel(chip, base + 0x3c, 0x0);
			creative_ext_readl(chip, base + 0x3c);
		}
	}

	/* Phase 2: Stream 0 enable */
	if (vfio_mask & 0x02) {
		dev_info(chip->dev, "vfio: P2 stream enable\n");
		creative_ext_writel(chip, 0x400, 0x1);
		creative_ext_readl(chip, 0x400);
		creative_ext_writel(chip, 0x408, 0x1);
		creative_ext_readl(chip, 0x408);
		creative_ext_writel(chip, 0x40c, 0x1);
		creative_ext_readl(chip, 0x40c);
		creative_ext_writel(chip, 0x410, 0x14);
		creative_ext_readl(chip, 0x410);
	}

	/* Phase 3: Scratch register 0x100: 0x600 → 0x60f → 0x70f */
	if (vfio_mask & 0x04) {
		dev_info(chip->dev, "vfio: P3 scratch 0x100\n");
		creative_ext_writel(chip, 0x100, 0x600);
		creative_ext_readl(chip, 0x100);
		creative_ext_writel(chip, 0x100, 0x60f);
		creative_ext_readl(chip, 0x100);
		creative_ext_writel(chip, 0x100, 0x70f);
		creative_ext_readl(chip, 0x100);
	}

	/* Phase 4: DSP boot */
	if (vfio_mask & 0x08) {
		dev_info(chip->dev, "vfio: P4 DSP boot\n");
		val = creative_ext_readl(chip, 0x830);
		creative_ext_writel(chip, 0x830, val);
		val = creative_ext_readl(chip, 0x86c);
		if (val & BIT(0)) {
			creative_ext_writel(chip, 0x86c, 0x0);
			creative_ext_readl(chip, 0x86c);
		}
		creative_ext_writel(chip, 0x800, 0x6b);
		creative_ext_readl(chip, 0x800);

		timeout = CREATIVE_DSP_WAIT_MS * 100;
		do {
			val = creative_ext_readl(chip, 0x89c);
			if (val & BIT(0))
				break;
			udelay(10);
		} while (--timeout);

		creative_ext_writel(chip, 0x86c, 0x1);
		creative_ext_readl(chip, 0x86c);
	}

	/* Phase 5: Preserve 0x800, 0x804, 0x20c */
	if (vfio_mask & 0x10) {
		dev_info(chip->dev, "vfio: P5 preserve\n");
		val = creative_ext_readl(chip, 0x800);
		creative_ext_writel(chip, 0x800, val);
		val = creative_ext_readl(chip, 0x804);
		creative_ext_writel(chip, 0x804, val);
		val = creative_ext_readl(chip, 0x20c);
		creative_ext_writel(chip, 0x20c, val);
	}

	dev_dbg(chip->dev, "BAR2 VFIO init complete\n");
}

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

/* ------------------------------------------------------------------ */
/* Manual PCM stream test driven from debugfs (no ALSA, no auto-open).  */
/* Lets us bring up the DMA stream step-by-step; a hang pinpoints the   */
/* exact operation. Uses stream descriptor index 4 (BAR0+0x100),	  */
/* stream tag 1.							  */
/*									  */
/* KEY: node 0x15 (WIDGET_CHIP_CTRL) is the REAL DSP audio input —	  */
/* it has FIFO=0x4500. Node 0x02 has FIFO=0 (dummy/proxy). The 0x0071	  */
/* capture binds node 0x15 with format 0x47 (8ch/24bit) to the stream.	  */
/* Without this, samples never enter the DSP processing pipeline.	  */
/* ------------------------------------------------------------------ */
#define PT_BASE		0x100	/* stream descriptor index 4 */
#define PT_TAG		1
#define PT_DSP_NID	0x15	/* real DSP audio input (FIFO=0x4500) */
#define PT_DAC_NID	0x02	/* kept for secondary binding */
#define PT_BUF_SZ	0x18000	/* 96 KB ring */
/* Cold-boot VM capture with confirmed working audio:
 * MMIO shows SD_FMT=0x0041, nid=0x15 SET_FMT=0x0041 (playback),
 * nid=0x02 SET_FMT=0x0035 (DAC output, different from bus format).
 */
#define PT_FMT		0x0041	/* stream bus format — matches Windows SD_FMT */
#define PT_DSP_FMT	0x0041	/* DSP input (nid=0x15) — same as bus format */
#define PT_DAC_FMT	0x0035	/* DAC output (nid=0x02) — DSP-converted */
#define PT_CHANS	6

static void *pt_buf, *pt_bdl;
static dma_addr_t pt_buf_dma, pt_bdl_dma;

static int creative_pcmtest_setup(struct creative_chip *chip)
{
	__le32 *bdl;
	s32 *samples;
	unsigned int frames, f, c;
	int phase = 0;
	u32 ctl, resp;

	if (!pt_buf) {
		pt_buf = dma_alloc_coherent(chip->dev, PT_BUF_SZ,
					    &pt_buf_dma, GFP_KERNEL);
		if (!pt_buf)
			return -ENOMEM;
	}
	if (!pt_bdl) {
		pt_bdl = dma_alloc_coherent(chip->dev, 256,
					    &pt_bdl_dma, GFP_KERNEL);
		if (!pt_bdl)
			return -ENOMEM;
	}

	/* Fill buffer with a ~440Hz square wave, 6ch / 24-bit-in-32 @ 48kHz.
	 * Put the tone on all 6 channels so whichever the DSP routes is audible.
	 */
	samples = pt_buf;
	frames = PT_BUF_SZ / (PT_CHANS * 4);
	for (f = 0; f < frames; f++) {
		s32 v = (phase < 55) ? (0x400000) : -(0x400000); /* ~24-bit */
		for (c = 0; c < PT_CHANS; c++)
			samples[f * PT_CHANS + c] = v << 8; /* 24-bit in MSBs */
		if (++phase >= 109)
			phase = 0;
	}

	/* BDL: single entry covering the whole buffer, IOC off */
	bdl = pt_bdl;
	bdl[0] = cpu_to_le32(lower_32_bits(pt_buf_dma));
	bdl[1] = cpu_to_le32(upper_32_bits(pt_buf_dma));
	bdl[2] = cpu_to_le32(PT_BUF_SZ);
	bdl[3] = cpu_to_le32(0);
	wmb();

	/* Reset stream descriptor */
	ctl = readl(chip->hda_regs + PT_BASE + AZX_REG_SD_CTL);
	ctl &= ~SD_CTL_DMA_START;
	writel((ctl | SD_CTL_STREAM_RESET) & SD_CTL_WMASK, chip->hda_regs + PT_BASE + AZX_REG_SD_CTL);
	wmb();
	udelay(50);
	writel((ctl & ~SD_CTL_STREAM_RESET) & SD_CTL_WMASK, chip->hda_regs + PT_BASE + AZX_REG_SD_CTL);
	wmb();
	udelay(50);

	/* Stream tag in SD_CTL bits 23:20, no interrupt-enable bits.
	 * NOTE: SD_LVI (0x0c) and SD_FORMAT (0x12) are 16-bit registers;
	 * SD_FORMAT lives at the unaligned offset 0x112 — a 32-bit access
	 * there wedges the device (master-abort), so use 16-bit writes.
	 */
	ctl = (PT_TAG & 0xf) << 20;
	writel((ctl) & SD_CTL_WMASK, chip->hda_regs + PT_BASE + AZX_REG_SD_CTL);
	writel(PT_BUF_SZ, chip->hda_regs + PT_BASE + AZX_REG_SD_CBL);
	writew(0, chip->hda_regs + PT_BASE + AZX_REG_SD_LVI);
	writew(PT_FMT, chip->hda_regs + PT_BASE + AZX_REG_SD_FORMAT);
	writel(lower_32_bits(pt_bdl_dma), chip->hda_regs + PT_BASE + AZX_REG_SD_BDLPL);
	writel(upper_32_bits(pt_bdl_dma), chip->hda_regs + PT_BASE + AZX_REG_SD_BDLPU);
	wmb();

	/* SET_STREAM_FORMAT on node 0x15 (real DSP input) and node 0x02 (DAC).
	 * Stream bus format = 0x41 (48kHz/PCM), DAC format = 0x35 (6ch/24bit).
	 * Both must match what Windows binds for audio to flow.
	 */
	creative_ae9_corb_send_verb_sync(chip,
		(1 << 28) | (PT_DSP_NID << 20) | (0x2 << 16) | PT_DSP_FMT,
		&resp);
	creative_ae9_corb_send_verb_sync(chip,
		(1 << 28) | (PT_DAC_NID << 20) | (0x2 << 16) | PT_DAC_FMT,
		&resp);

	scnprintf(g_dbg_result, sizeof(g_dbg_result),
		  "pcmtest setup ok: buf=0x%llx bdl=0x%llx fmt=0x%x\n",
		  (u64)pt_buf_dma, (u64)pt_bdl_dma, PT_FMT);
	return 0;
}

static void creative_pcmtest_go(struct creative_chip *chip)
{
	u32 ctl, resp;

	/*
	 * Cold-boot VM capture with CONFIRMED WORKING audio on head-unit HP.
	 * Uses creative_dsp_pcm_start() which has the exact sequence from
	 * the CORB capture of real working audio.
	 */
	creative_dsp_pcm_start(chip, PT_TAG);

	/* Start DMA */
	ctl = readl(chip->hda_regs + PT_BASE + AZX_REG_SD_CTL);
	writel((ctl | SD_CTL_DMA_START) & SD_CTL_WMASK, chip->hda_regs + PT_BASE + AZX_REG_SD_CTL);
	wmb();
	scnprintf(g_dbg_result, sizeof(g_dbg_result), "pcmtest go (RUN set)\n");
}

static void creative_pcmtest_stop(struct creative_chip *chip)
{
	u32 ctl, resp;

	ctl = readl(chip->hda_regs + PT_BASE + AZX_REG_SD_CTL);
	writel((ctl & ~SD_CTL_DMA_START) & SD_CTL_WMASK, chip->hda_regs + PT_BASE + AZX_REG_SD_CTL);
	wmb();
	/* Unbind DAC node */
	creative_ae9_corb_send_verb_sync(chip,
		(1 << 28) | (PT_DAC_NID << 20) | (0x706 << 8) | 0, &resp);
	scnprintf(g_dbg_result, sizeof(g_dbg_result), "pcmtest stopped\n");
}

static void creative_pcmtest_pos(struct creative_chip *chip)
{
	u32 lpib = readl(chip->hda_regs + PT_BASE + AZX_REG_SD_LPIB);
	u32 ctl = readl(chip->hda_regs + PT_BASE + AZX_REG_SD_CTL);
	scnprintf(g_dbg_result, sizeof(g_dbg_result),
		  "pcmtest pos: LPIB=0x%x SD_CTL=0x%x\n", lpib, ctl);
}

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

	/* PCM manual test commands (no ALSA / no auto-open) */
	if (!strncmp(buf, "pcmtest", 7)) {
		int ret = creative_pcmtest_setup(g_dbg_chip);
		if (ret < 0)
			return ret;
		return len;
	}
	if (!strncmp(buf, "pcmgo", 5)) {
		creative_pcmtest_go(g_dbg_chip);
		return len;
	}
	if (!strncmp(buf, "pcmstop", 7)) {
		creative_pcmtest_stop(g_dbg_chip);
		return len;
	}
	if (!strncmp(buf, "pcmpos", 6)) {
		creative_pcmtest_pos(g_dbg_chip);
		return len;
	}

	/* "replay1" / "replay2" — re-run captured codec init verb sequence
	 * (used to apply DSP routing config AFTER the firmware is running).
	 */
	if (!strncmp(buf, "replay1", 7)) {
		creative_ae9_replay_init(g_dbg_chip, 1, ae9_initseq_codec1,
					 ARRAY_SIZE(ae9_initseq_codec1));
		scnprintf(g_dbg_result, sizeof(g_dbg_result), "replay1 done\n");
		return len;
	}
	if (!strncmp(buf, "replay2", 7)) {
		creative_ae9_replay_init(g_dbg_chip, 2, ae9_initseq_codec2,
					 ARRAY_SIZE(ae9_initseq_codec2));
		scnprintf(g_dbg_result, sizeof(g_dbg_result), "replay2 done\n");
		return len;
	}

	/* "acm" — head-unit display I2C init (DSP I2C ctrl + 29 ACM tx) */
	if (!strncmp(buf, "acm", 3)) {
		writeb(0x3f, g_dbg_chip->ext_regs + CREATIVE_REG_STREAM_ENABLE);
		wmb();
		creative_i2c_dsp_init(g_dbg_chip);
		creative_i2c_init(g_dbg_chip);
		scnprintf(g_dbg_result, sizeof(g_dbg_result), "acm init done\n");
		return len;
	}
	/* "dspae9" — full AE-9 post-firmware DSP audio-router standup */
	if (!strncmp(buf, "dspae9", 6)) {
		int ret = creative_dsp_ae9_standup(g_dbg_chip);

		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "dspae9 ret=%d\n", ret);
		return len;
	}
	/* "dspstreams" — start DSP internal audio router streams */
	if (!strncmp(buf, "dspstreams", 10)) {
		int ret = creative_dsp_start_audio_streams(g_dbg_chip);

		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "dspstreams ret=%d\n", ret);
		return len;
	}
	/* "dspout" — route DSP audio to rear line-out */
	if (!strncmp(buf, "dspout", 6)) {
		int ret = creative_dsp_select_lineout(g_dbg_chip);

		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "dspout ret=%d\n", ret);
		return len;
	}
	/* "dacroute" — replay capture's playback-time head-unit DAC routing
	 * (targets 0x48/0x49, the path that feeds the rear card RCA).
	 */
	if (!strncmp(buf, "dacroute", 8)) {
		int ret = creative_dsp_headunit_dac_route(g_dbg_chip);

		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "dacroute ret=%d\n", ret);
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
	/* "remapstream" — write audio router 0x190000 entries directly */
	if (!strncmp(buf, "remapstream", 11)) {
		creative_dsp_write_audio_router(g_dbg_chip);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "remapstream done\n");
		return len;
	}
	/* "scpreset" — flush SCP command queue via DSPIO_DSP_INIT (0x703) */
	if (!strncmp(buf, "scpreset", 8)) {
		int ret = creative_dsp_scp_reset(g_dbg_chip);

		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "scpreset ret=%d\n", ret);
		return len;
	}
	/* "ampeng" — AMP/RELAY ENGAGE: GPIO pin4 HIGH + ca0113 route (the click) */
	if (!strncmp(buf, "ampeng", 6)) {
		int ret = creative_dsp_amp_engage(g_dbg_chip);

		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "ampeng ret=%d\n", ret);
		return len;
	}
	/* "dacen" — enable codec DAC output amps/coefs (playback unmute) */
	if (!strncmp(buf, "dacen", 5)) {
		int ret = creative_dsp_enable_dac_output(g_dbg_chip);

		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "dacen ret=%d\n", ret);
		return len;
	}
	/* "initroute" — init-time head-unit DAC route (0x31 vals, arms DAC) */
	if (!strncmp(buf, "initroute", 9)) {
		int ret = creative_dsp_headunit_init_route(g_dbg_chip);

		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "initroute ret=%d\n", ret);
		return len;
	}
	/* "dsprun" — un-halt the DSP 8051 so it consumes SCP commands */
	if (!strncmp(buf, "dsprun", 6)) {
		int ret = creative_dsp_run(g_dbg_chip);

		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "dsprun ret=%d\n", ret);
		return len;
	}
	/* "ampen" — head-unit amp/DAC enable (0x83/0x85/0xb1 output-enable
	 * block from the capture's playback window; engages the DAC relay).
	 */
	if (!strncmp(buf, "ampen", 5)) {
		int ret = creative_i2c_amp_enable(g_dbg_chip);

		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "ampen ret=%d\n", ret);
		return len;
	}
	/* "dspfw" — download the DSP firmware (ctefx-desktop.bin) */
	if (!strncmp(buf, "dspfw", 5)) {
		int ret = creative_dsp_load_firmware(g_dbg_chip);

		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "dspfw ret=%d\n", ret);
		return len;
	}
	/* "dspprep" — bring up DSP clocks + CT extensions + sanity chipio read */
	if (!strncmp(buf, "dspprep", 7)) {
		int ret = creative_dsp_prepare(g_dbg_chip);

		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "dspprep ret=%d\n", ret);
		return len;
	}
	/* "chipior <hex_addx>" — ChipIO read */
	if (!strncmp(buf, "chipior", 7)) {
		unsigned int a, v = 0;
		int ret;

		if (sscanf(buf, "%7s %x", op, &a) != 2)
			return -EINVAL;
		ret = creative_dsp_chipio_read(g_dbg_chip, a, &v);
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
		ret = creative_dsp_chipio_write(g_dbg_chip, a, v);
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

	/* "verb <cad> <nid> <verbid> <payload>" — send one ICW verb */
	if (!strncmp(buf, "verb", 4)) {
		unsigned int cad, nid, vid, pl;
		u32 resp = 0;
		int ret;

		if (sscanf(buf, "%7s %x %x %x %x", op, &cad, &nid, &vid, &pl) != 5)
			return -EINVAL;
		ret = creative_ae9_icw_send_verb(g_dbg_chip,
			creative_ae9_verb(cad, nid, vid, pl), &resp);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "verb cad=%x nid=%x v=%x p=%x -> ret=%d resp=0x%08x\n",
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
		val = creative_ext_readl(g_dbg_chip, off);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "w2 0x%x <= 0x%x (rb 0x%x)\n", off, val, val);
	} else if (!strcmp(op, "r2")) {
		val = creative_ext_readl(g_dbg_chip, off);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "r2 0x%x = 0x%x\n", off, val);
	} else if (!strcmp(op, "w0") && n == 3) {
		creative_hda_writel(g_dbg_chip, off, val);
		val = creative_hda_readl(g_dbg_chip, off);
		scnprintf(g_dbg_result, sizeof(g_dbg_result),
			  "w0 0x%x <= 0x%x (rb 0x%x)\n", off, val, val);
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
/*
 * Deferred DSP standup — runs ~20s after probe (login-screen timing), when the
 * DSP and head-unit MCU are ready. Order: chip-init verb replay -> firmware
 * download -> AE-7 audio-router standup -> output select -> head-unit I2C init.
 */
static void creative_ae9_dsp_work(struct work_struct *work)
{
	struct creative_chip *chip =
		container_of(work, struct creative_chip, dsp_work.work);
	int fret;

	dev_info(chip->dev, "Deferred DSP standup starting\n");

	/* chip init BEFORE firmware (sets up DSP for the transfer) */
	creative_ae9_replay_both(chip);

	fret = creative_dsp_load_firmware(chip);
	dev_info(chip->dev, "Deferred DSP firmware load: %d\n", fret);
	if (fret == 0) {
		creative_ae9_post_fw(chip);
		creative_dsp_ae9_standup(chip);
		creative_dsp_select_lineout(chip);
		chip->dsp_done = 1;
	}

	/* creative_i2c_init NOT called here — see acm_work comment. */
	dev_info(chip->dev, "Deferred DSP standup done (dsp_done=%d)\n",
		 chip->dsp_done);
}

static void creative_ae9_acm_work(struct work_struct *work)
{
	struct creative_chip *chip =
		container_of(work, struct creative_chip, acm_work.work);

	/*
	 * creative_i2c_init (the 33-tx ACM I2C sequence) is NOT called here.
	 *
	 * Analysis of both MMIO captures (cold-boot + QEMU-reset) confirms:
	 * - The Windows driver NEVER sends any I2C init sequence to the ca0113
	 *   head unit. There are zero 0xC0C=0x83 (I2C mode) writes before PCM
	 *   prepare in either capture.
	 * - The decompiled Creative Linux driver (v0.73_kernel18) has no
	 *   head-unit I2C init function at all.
	 * - The ca0113 head unit self-initialises from its own MCU firmware:
	 *   "AE-9" display + knob light = MCU self-boot (no host involvement).
	 *   Relay click + "-HP-" = MCU reacts to audio clock from dac_route.
	 *
	 * Sending creative_i2c_init disrupts the ca0113 MCU's state machine,
	 * leaving it unable to respond to subsequent SPI probe frames.
	 * Symptoms: probe no ACK, relay3 n=0, 'E' display corruption, no audio.
	 *
	 * The only required host action is the heartbeat below, which sends
	 * the 0xb1/01/02/03 relay engage sequence and keeps the MCU/DAC alive.
	 * The relay engage at PCM prepare time is handled by pcm_relay_engage().
	 */
	if (post_windows) {
		creative_hb_skip_gpio4 = true;
		dev_info(chip->dev, "ACM work: post_windows, heartbeat only (no GPIO4 toggle)\n");
		goto heartbeat;
	}
	dev_info(chip->dev, "ACM work: GPIO5 DAC power ON + 3s delay + second reset\n");

	/*
	 * GPIO5 = ExternalDACReset_Pin: assert to release DAC from reset.
	 * ae9_build: "Must be set AFTER ae9_post_dsp_asi_setup(). Wait for
	 * ACM MCU to boot after GPIO5 released it. 3s is conservative minimum."
	 */
	creative_ext_writew(chip, 0x320, 0x0105); /* GPIO5 ON */
	msleep(3000);

	/*
	 * Second PLL + CA0113 engine reset — matching ae9_build's second reset
	 * block in ae9_setup_defaults(). "Without this, the I2S clock to the
	 * ACM never starts and the relay never clicks."
	 */
	if (enable_audio_path)
		creative_ae9_ca0113_second_reset(chip);

	/*
	 * Engage relay ONCE at card init time, not at PCM prepare time.
	 * Uses SPI ACM path (0xC00 writes) — works anytime after MCU alive.
	 * ae9_build: GPIO4 asserted right after ae9_acm_init(), then
	 * alt_select_out() sends the HP output preset CA0113 commands.
	 */
	creative_i2c_pcm_relay_engage(chip);
	chip->relay_engaged = 1;

	/*
	 * Phase 5: Output selection — HP output preset + DAC volume registers.
	 * ae9_build: called immediately after GPIO4 assert (relay click).
	 * Sends the final HP output preset CA0113 commands (group 0x48,
	 * regs 0x1d/0x0d/0x17/0x19) + headphone gain regs (0x11-0x14).
	 * Also includes the volume preset commands for both group 0x49 regs
	 * (0x04/0x05/0x02/0x03 = 0x1e, the post-relay DAC output level).
	 * Without these, the ESS Sabre DAC routes audio internally but the
	 * HP amplifier input stage gets no level — silent even if DMA runs.
	 */
	if (enable_audio_path) {
		/* HP output preset + DSP unmute (ae9_ca0113_output_presets HP) */
		creative_dsp_select_out_params(chip);
		/* Final ES9038 DAC output levels — post-relay (ae9_build line 10831) */
		creative_dsp_post_relay_dac_levels(chip);
	}

	/* ca0113_windows: the "second reset" above isn't from Windows and may
	 * reset the DAC, so send Windows' boot CA0113 commands again */
	if (ae9_ca0113_windows)
		creative_dsp_ca0113_replay(chip, ae9_cold_ca0113_boot,
					   ARRAY_SIZE(ae9_cold_ca0113_boot),
					   "boot commands (after ACM work)");

heartbeat:
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
	dev_info(chip->dev, "head unit: volume att 0x%02x (%s)\n", att,
		 chip->hu_mute ? "muted" : "on");
}

/* Knob turn: c2 reply f0 c2 04 <dir> <steps> .., dir 0 = clockwise, ~2
 * steps per click. Button: bit 7 of the 83 01 02 reply's status byte reads
 * 0 while held (b5 -> 35). Plugs, same byte, 0 = plugged in (checked on the
 * card 2026-11-03): bit 5 = 3.5 mm headphone jack, bit 4 = 3.5 mm mic jack.
 * The 6.3 mm headphone jack, the XLR/TRS combo jack (a TRS plug set bit 4
 * once in 4 tries) and XLR plugs are effectively not reported. Plug changes
 * are only logged; the driver never switches output on them. Returns the
 * signed step count. */
static int creative_ae9_hu_parse(struct creative_chip *chip, const u8 *r,
				 int n, int *press_ms)
{
	if (r[1] == 0xc2 && n >= 6 && r[4])
		return r[3] ? -r[4] : r[4];
	if (r[1] == 0x83 && n >= 6 && r[3] == 0x02) {
		bool down = !(r[4] & 0x80);
		u8 st = r[4] & 0x7f;
		u8 chg = chip->hu_status_valid ? st ^ chip->hu_status : 0x30;

		if (chg & 0x20)
			dev_info(chip->dev, "head unit: 3.5 mm headphone plug %s\n",
				 (st & 0x20) ? "out" : "in");
		if (chg & 0x10)
			dev_info(chip->dev, "head unit: mic/line plug %s\n",
				 (st & 0x10) ? "out" : "in");
		/* bit 6 (headphone amp) follows every output switch: not logged */
		if (chg & ~0x70)
			dev_info(chip->dev, "head unit: status 0x%02x (other bits changed: 0x%02x)\n",
				 r[4], chg & ~0x70);
		chip->hu_status = st;
		chip->hu_status_valid = true;

		if (down && !chip->hu_btn_down) {
			chip->hu_btn_since = jiffies;
		} else if (!down && chip->hu_btn_down) {
			*press_ms = jiffies_to_msecs(jiffies - chip->hu_btn_since);
			chip->hu_btn_seen = jiffies;
		}
		chip->hu_btn_down = down;
	}
	/* The b1 02 reply (full poll only) also reports presses, with the
	 * duration capped at 3000 ms; a long hold is reported at 3 s while
	 * still held. Use it only for a short press the fast poll missed:
	 * not while held, not right after a press the fast poll timed. */
	if (r[1] == 0xb1 && n >= 8 && r[3] == 0x02 && r[4] == 0x01 &&
	    !chip->hu_btn_down && (r[5] | (r[6] << 8)) < 1000 &&
	    time_after(jiffies, chip->hu_btn_seen + msecs_to_jiffies(1500)))
		*press_ms = r[5] | (r[6] << 8);
	return 0;
}

static void creative_ae9_hu_label(struct creative_chip *chip)
{
	u8 resp[24], label[13] = { 0xf0, 0x11, 0x09 };
	char text[9] = "";

	if (chip->hu_ticks < chip->hu_name_until)
		strscpy(text, chip->hu_output ? "-HP-" : "-SP-", sizeof(text));
	else if (chip->hu_mute)
		strscpy(text, "MUTE", sizeof(text));
	else
		snprintf(text, sizeof(text), "%s%u.%u", chip->hu_att ? "-" : "",
			 chip->hu_att / 2, (chip->hu_att & 1) * 5);
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
 * f0 22 02 01 01 / 00 on the head unit.
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
	creative_spi_frame_win(chip, f, sizeof(f), resp, sizeof(resp));
	dev_info(chip->dev, "head unit: knob light %s\n", on ? "on" : "off");
}

/* Act on knob input and pending control changes: display first (feels
 * immediate), then the DAC volume commands (~20 ms each). Holds: under
 * 1 s mutes, 1.5-3.5 s switches speakers/headphones, longer toggles the
 * knob light. */
static void creative_ae9_hu_update(struct creative_chip *chip, int steps,
				   int press_ms)
{
	u8 resp[24];

	if (chip->hu_switch_to >= 0) {
		int to = chip->hu_switch_to;

		chip->hu_switch_to = -1;
		creative_ae9_hu_switch(chip, to);
	}
	if (press_ms >= 1500 && press_ms < 3500) {
		creative_ae9_hu_switch(chip, !chip->hu_output);
		press_ms = -1;
	} else if (press_ms >= 3500) {
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

/* "XLR Gain Capture Volume": codec 1 node 0x12 (the XLR mic pin) input
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
	.name = "XLR Gain Capture Volume",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	.info = mic_boost_info,
	.get = mic_boost_get,
	.put = mic_boost_put,
	.tlv.p = mic_boost_tlv,
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

static void creative_ae9_hu_controls_add(struct creative_chip *chip)
{
	chip->hu_att = HU_ATT_DEFAULT;
	chip->hu_att_out[0] = chip->hu_att_out[1] = HU_ATT_DEFAULT;
	chip->hu_output = 0;		/* the boot replay selects speakers */
	chip->hu_name_until = HU_SP_TICKS;
	chip->hu_switch_to = -1;
	chip->hu_light = 1;
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
}

static void creative_ae9_hb_work(struct work_struct *work)
{
	struct creative_chip *chip =
		container_of(work, struct creative_chip, hb_work.work);

	if (post_windows && chip->dsp_done) {
		if (time_after_eq(jiffies, chip->hu_next_full)) {
			chip->hu_next_full = jiffies + msecs_to_jiffies(acm_heartbeat_ms);
			creative_ae9_hb_windows(chip);
			creative_dsp_keepalive(chip);
		} else {
			creative_ae9_hb_fast(chip);
		}
		if (acm_heartbeat_ms)
			queue_delayed_work(chip->wq, &chip->hb_work,
					      msecs_to_jiffies(min(hu_fast_ms, acm_heartbeat_ms)));
		return;
	}

	/*
	 * Only run the heartbeat (which sends the 0xb1 relay-engage sequence
	 * and displays "-HP-") AFTER DSP standup is complete.  Running it
	 * before dsp_done would show "-HP-" before the DSP is ready — the
	 * correct Windows sequence is: "AE-9" (MCU splash) → standup complete
	 * → HELLO ACK → click + "-HP-" → volume display.
	 */
	if (chip->dsp_done) {
		creative_i2c_heartbeat(chip);

		/* Same 500ms tick keeps the DSP 8051 from idling into a halt
		 * (which jams the SCP queue). Cheap; only acts if actually halted.
		 */
		creative_dsp_keepalive(chip);
	}

	/* Re-arm indefinitely until module removal cancels it. */
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
	int ret;

	creative_ae9_log_state(chip, "before bring-up");

	if (bus_reset && pdev->bus->self && !creative_ae9_alone_on_bus(pdev)) {
		dev_warn(&pdev->dev, "other devices behind %s: skipping the secondary bus reset\n",
			 pci_name(pdev->bus->self));
	} else if (bus_reset && pdev->bus->self) {
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


	if (bar2_first)
		creative_ae9_bar2_init_windows(chip);

	/* Reset HDA controller */
	ret = creative_ae9_hw_reset(chip);
	if (ret < 0)
		return ret;

	/* Enable unsolicited responses */
	creative_hda_writel(chip, AZX_REG_GCTL,
			    AZX_GCTL_RESET | AZX_GCTL_UNSOL);
	if (codec_probe_delay_ms)
		msleep(codec_probe_delay_ms);

	/*
	 * BAR2 init: full AE-9 specific MMIO register sequence.
	 * Matches ae9_build ca0132_mmio_init_ae9() exactly:
	 * AE-9 inlined table + I2C bus init + 0x20c=0x800000 + 0x1c=0x480.
	 * This supersedes both the old VFIO hw_init and the HW_CTRL toggle.
	 */
	if ((init_mask & 0x01) && !bar2_first) {
		dev_info(chip->dev, "init: AE-9 BAR2 inline init\n");
		creative_ae9_bar2_init(chip);
	}

	/* Legacy VFIO-capture-based phases (kept for reference, skipped by default) */
	if (init_mask & 0x80) {
		dev_info(chip->dev, "init: VFIO BAR2 legacy\n");
		creative_ae9_hw_init(chip);
	}

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

	/* Wake codecs from D3: send SET_POWER_STATE D0 to AFG (node 0x01)
	 * of each detected codec. ca0132's exit verbs put the codec in D3
	 * on unbind, killing the card LEDs / head unit. (verb 0x705, p=0)
	 */
	if (!cold_prefw) {
		int cad;
		for (cad = 0; cad < HDA_MAX_CODECS; cad++) {
			u32 resp;
			int pret;
			if (!(chip->codec_mask & BIT(cad)))
				continue;
			pret = creative_ae9_icw_send_verb(chip,
				creative_ae9_verb(cad, 0x01, 0x705, 0x00),
				&resp);
			dev_info(chip->dev,
				 "Codec %d AFG D0: ret=%d resp=0x%08x\n",
				 cad, pret, resp);
		}
		msleep(10);
	}

	/* GPIO init (ca0132 AE-5/AE-7 vendor verbs) to codec 1 node 0x01.
	 * These configure GPIO that may drive the card LEDs.
	 */
	if (!cold_prefw) {
		u32 resp;
		int g0, g1, g2;
		g0 = creative_ae9_icw_send_verb(chip,
			creative_ae9_verb(1, 0x01, 0x793, 0x00), &resp);
		g1 = creative_ae9_icw_send_verb(chip,
			creative_ae9_verb(1, 0x01, 0x794, 0x53), &resp);
		g2 = creative_ae9_icw_send_verb(chip,
			creative_ae9_verb(1, 0x01, 0x790, 0x23), &resp);
		dev_info(chip->dev, "GPIO init verbs: %d/%d/%d\n", g0, g1, g2);
	}

	/*
	 * DSP standup, in the order the CA0132 requires:
	 *   1. download firmware (DSP must be running before its config takes)
	 *   2. replay the captured codec init verbs (now applied to live DSP)
	 *   3. start the DSP internal audio-router streams
	 *   4. route DSP output to line-out
	 * If load_fw is not set, fall back to the bare verb replay (full_init).
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
			int cret;

			creative_hda_writel(chip, AZX_REG_GCTL,
					    AZX_GCTL_RESET | AZX_GCTL_UNSOL);
			cret = creative_ae9_corb_init(chip);
			dev_info(chip->dev,
				 "win_reset2: second controller reset %d, corb_init %d\n",
				 rret, cret);
		}

		creative_ae9_replay_both(chip);

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
		if (!win_reset2) {
			int cret = creative_ae9_corb_init(chip);
			dev_info(chip->dev, "early corb_init: %d\n", cret);
		}

		fret = creative_dsp_load_firmware(chip);
		dev_info(chip->dev, "DSP firmware load: %d\n", fret);
		if (fret == 0) {
			creative_ae9_post_fw(chip);

			/* post_windows: the replay left the head-unit link in
			 * Windows' mode; the 0x30 re-init below breaks it */
			if (post_windows)
				goto post_setup_done;

			/* DSP SPI init (CMD=0x30) — different from ACM I2C init
			 * (CMD=0x0d). From cold-boot capture L88-109 (t=0.05s):
			 * the first BAR2 init writes CMD=0x30 which puts the
			 * controller in DSP SPI mode for the probe frames.
			 * creative_i2c_dsp_init uses CMD=0x0d (ACM I2C mode).
			 */
			/* DSP SPI init sequence from cold-boot capture L87-115:
			 * C7C read, C0C: 0x80 init, C00=0x30 (DSP SPI mode),
			 * C04=0x00, C0C: 0x00 clear → 0x03×3 FIFO mode,
			 * then C08 FIFO enable: 0x01→0xf1→0x01→0xc7→0xc1,
			 * C04=0x80 trigger, 0x1c toggle → C14=0x40 idle.
			 */
			creative_ext_readl(chip,  0xC7C);
			creative_ext_readl(chip,  0xC0C);
			creative_ext_writel(chip, 0xC0C, 0x80);
			creative_ext_readl(chip,  0xC0C);
			creative_ext_writel(chip, 0xC00, 0x30);
			creative_ext_readl(chip,  0xC00);
			creative_ext_writel(chip, 0xC04, 0x00);
			creative_ext_readl(chip,  0xC04);
			creative_ext_readl(chip,  0xC0C);
			creative_ext_writel(chip, 0xC0C, 0x00);
			creative_ext_readl(chip,  0xC0C);
			creative_ext_readl(chip,  0xC7C);
			creative_ext_readl(chip,  0xC0C);
			creative_ext_writel(chip, 0xC0C, 0x03);
			creative_ext_readl(chip,  0xC0C);
			creative_ext_readl(chip,  0xC7C);
			creative_ext_readl(chip,  0xC0C);
			creative_ext_writel(chip, 0xC0C, 0x03);
			creative_ext_readl(chip,  0xC0C);
			creative_ext_readl(chip,  0xC7C);
			creative_ext_readl(chip,  0xC0C);
			creative_ext_writel(chip, 0xC0C, 0x03);
			creative_ext_readl(chip,  0xC0C);
			/*
			 * C08 FIFO enable sequence (capture L110-126).
			 * Full sequence: 0x01 → 0xf1 → 0x01 → 0xc7 → 0xc1
			 * Our old code stopped after 0xf1, leaving C14=0x60
			 * (controller not fully idle). The remaining writes
			 * are required to bring C14 to 0x40 (idle/ready).
			 */
			creative_ext_readl(chip,  0xC08);
			creative_ext_writel(chip, 0xC08, 0x01);
			creative_ext_readl(chip,  0xC08);
			creative_ext_readl(chip,  0xC08);
			creative_ext_writel(chip, 0xC08, 0xF1);
			creative_ext_readl(chip,  0xC08);
			creative_ext_readl(chip,  0xC08);
			creative_ext_writel(chip, 0xC08, 0x01);
			creative_ext_readl(chip,  0xC08);
			creative_ext_readl(chip,  0xC08);
			creative_ext_writel(chip, 0xC08, 0xC7);
			creative_ext_readl(chip,  0xC08);
			creative_ext_readl(chip,  0xC08);
			creative_ext_writel(chip, 0xC08, 0xC1);
			creative_ext_readl(chip,  0xC08);
			/*
			 * C04=0x80 DATA trigger (capture L126).
			 * Kicks the SPI controller into active mode.
			 */
			creative_ext_readl(chip,  0xC04);
			creative_ext_writel(chip, 0xC04, 0x80);
			creative_ext_readl(chip,  0xC04);
			/*
			 * 0x1c toggle: clear bit 23 (0x880480→0x480) then
			 * restore (capture L128-132). This is an interrupt-
			 * enable or DMA-ready latch that transitions C14
			 * from 0x60 to 0x40 (idle/ready).
			 */
			{
				u32 v = creative_ext_readl(chip, 0x1c);
				creative_ext_writel(chip, 0x1c, v & ~0x800000);
				creative_ext_readl(chip,  0x1c);
				creative_ext_readl(chip,  0x1c);
				creative_ext_writel(chip, 0x1c, v);
				creative_ext_readl(chip,  0x1c);
			}
			udelay(100);
			dev_info(chip->dev,
				 "DSP SPI init: C0C=0x%02x C08=0x%02x C14=0x%02x C7C=0x%02x\n",
				 creative_ext_readl(chip, 0xC0C) & 0xff,
				 creative_ext_readl(chip, 0xC08) & 0xff,
				 creative_ext_readl(chip, 0xC14) & 0xff,
				 creative_ext_readl(chip, 0xC7C) & 0xff);

			/* Cold-start the 8051 audio loop via BAR2 SPI.
			 * From the cold-boot VFIO capture: after firmware
			 * downloads, the driver probes the DSP via SPI
			 * MEM_WRITE 0x11111111 until ACK=0x81, then sends
			 * HELLO "-HP-" which triggers the relay click and
			 * head unit display. Without this, 0x801c stays 0
			 * and SCP commands are ignored.
			 */
			if (hu_boot_ms) {
				/* GPIO5 on, then give the head-unit MCU time
				 * to boot, as acm_work does before its probe. */
				creative_ext_writew(chip, 0x320, 0x0105);
				dev_info(chip->dev,
					 "audio_loop_start: GPIO5 on, waiting %u ms for head-unit MCU\n",
					 hu_boot_ms);
				msleep(hu_boot_ms);
			}
			if (loop_start) {
				int aret = creative_i2c_dsp_audio_loop_start(chip);
				dev_info(chip->dev,
					 "audio_loop_start: %d\n", aret);
			} else {
				dev_info(chip->dev, "audio_loop_start: skipped (loop_start=0)\n");
			}

			/*
			 * Allow the 8051 time to fully initialize its SCP
			 * command handler after the HELLO ACK. In the Windows
			 * driver, the ACM init sequence (AE-9 display, relay
			 * click) runs here and takes ~5 seconds, during which
			 * the 8051 settles. Without a delay, the SCP trigger
			 * commands (mod=0x18/0x19/0x15) in select_out hit a
			 * full command queue (sts=0x2) and 0x801c stays 0.
			 * 500ms matches the time needed for the 8051 to drain
			 * its startup queue and become ready for SCP commands.
			 */
			msleep(500);

			/* standup now includes audio-router write + select_lineout
			 * internally (at standup-end while SCP is healthy).
			 */
			creative_dsp_ae9_standup(chip);
			/* Enable the codec-side DAC output amps/coefs — the
			 * playback-unmute step (capture BOOT/INIT). Without it
			 * the DSP processes audio but the DAC amps stay muted.
			 */
			creative_dsp_enable_dac_output(chip);

			/*
			 * CA0113 handshake — MUST happen before any ca0113
			 * command_set/play_cmd calls. Sends the full init
			 * sequence (GPIO0 → engine reset → GPIO5 pulse →
			 * 0x210 sync → 0xffff→0x208 → ~50 DAC init cmds).
			 * Without this, ALL ca0113 group 0x48/0x49 commands
			 * are silently ignored (0x860 stays 0).
			 */
			if (enable_audio_path)
				creative_ae9_ca0113_handshake(chip);

			/* Legacy amp_engage/route paths — these now have a
			 * valid handshake ahead of them and should execute.
			 * amp_engage fires GPIO4 (relay) + initial unmute.
			 * init_route + dac_route configure ESS Sabre DAC.
			 */
			if (enable_audio_path)
				creative_dsp_amp_engage(chip);
			if (enable_audio_path) {
				creative_dsp_headunit_init_route(chip);
				creative_dsp_headunit_dac_route(chip);
			}
post_setup_done:
			chip->dsp_done = 1;
			/*
			 * MCU is confirmed up (HELLO ACK'd). Schedule ACM init
			 * with a short delay so "AE-9" appears on the display
			 * shortly after the relay click / "-HP-", not 12s later.
			 * The 12s default delay was for cold MCU boot; since
			 * audio_loop_start confirmed the MCU is alive, we only
			 * need ~500ms for the 8051 to settle after HELLO.
			 */
			chip->acm_retries = acm_passes;
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
	} else {
		if (full_init & 0x01)
			creative_ae9_replay_init(chip, 1, ae9_initseq_codec1,
						 ARRAY_SIZE(ae9_initseq_codec1));
		if (full_init & 0x02)
			creative_ae9_replay_init(chip, 2, ae9_initseq_codec2,
						 ARRAY_SIZE(ae9_initseq_codec2));
	}

	/* Stream enable */
	/* post_windows: the replay already set these up as Windows does;
	 * the DSP I2C init's CMD=0x30 re-init cuts off the head unit */
	if ((init_mask & 0x04) && !(post_windows && chip->dsp_done)) {
		dev_info(chip->dev, "init: stream enable\n");
		writeb(0x3f, chip->ext_regs + CREATIVE_REG_STREAM_ENABLE);
		wmb();
	}

	/* DSP I2C controller init */
	if ((init_mask & 0x08) && !(post_windows && chip->dsp_done)) {
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
	unsigned int attempt, tries = (post_windows && load_fw) ? max(resume_tries, 1U) : 1;
	int ret = 0;

	for (attempt = 1; attempt <= tries; attempt++) {
		if (attempt > 1) {
			cancel_delayed_work_sync(&chip->acm_work);
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
	INIT_DELAYED_WORK(&chip->dsp_work, creative_ae9_dsp_work);
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
	if (post_windows && load_fw && !(chip->dsp_done && chip->hs_ok)) {
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

	if (post_windows)
		creative_ae9_hu_controls_add(chip);
	if (enable_pcm) {
		chip->mic_boost = 1;
		snd_ctl_add(chip->card, snd_ctl_new1(&mic_boost_control, chip));
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

	if (!chip->corb_buf) {
		ret = creative_ae9_corb_init(chip);
		if (ret < 0) {
			dev_err(chip->dev, "CORB init failed: %d\n", ret);
			goto err_card;
		}
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

	if ((init_mask & 0x10) && !chip->dsp_done) {
		chip->acm_retries = acm_passes;
		dev_info(chip->dev,
			 "scheduling first ACM init in %u ms (%u passes, %u ms gap)\n",
			 acm_first_delay_ms, acm_passes, acm_pass_gap_ms);
		queue_delayed_work(chip->wq, &chip->acm_work,
				      msecs_to_jiffies(acm_first_delay_ms));
	}

	dev_info(chip->dev, "Init complete: card %s\n", chip->card->longname);
	return 0;

err_card:
	snd_card_free(chip->card);
	chip->card = NULL;
err_hw:
	/* works the bring-up scheduled (ACM/heartbeat) must not outlive chip */
	creative_ae9_stop_works(chip);
	if (post_windows && chip->corb_buf)
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

	/* input stream 0: capture */
	if (intsts & 0x1)
		creative_ae9_cap_irq(chip);

	/* Acknowledge stream interrupts and notify ALSA of period completion */
	if (intsts & AZX_INT_ALL_STREAM & ~0x1) {
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
static void creative_ae9_win_shutdown(struct creative_chip *chip, const char *why)
{
	u32 ctl;

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
	dev_info(chip->dev, "shutdown (%s): done\n", why);
}

static void creative_ae9_stop_works(struct creative_chip *chip)
{
	cancel_work_sync(&chip->resume_work);
	atomic_set(&chip->dma_running, 0);
	cancel_delayed_work_sync(&chip->hb_work);
	cancel_delayed_work_sync(&chip->acm_work);
	cancel_delayed_work_sync(&chip->dsp_work);
}

/* Reboot / power-off: same as Windows shutting down. */
static void creative_ae9_shutdown(struct pci_dev *pdev)
{
	struct creative_chip *chip = pci_get_drvdata(pdev);

	if (!chip || chip->suspended)
		return;
	creative_ae9_stop_works(chip);
	chip->suspended = true;		/* IRQ handler: hands off */
	if (post_windows)
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

	/* Windows' shutdown (post_windows; without it, nothing is sent) */
	if (post_windows && !chip->suspended)
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
	cancel_delayed_work_sync(&chip->dsp_work);
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
		chip->pcm_reclocked = false;	/* the bring-up redoes the clocks */
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
	if (!chip->hu_light)
		creative_ae9_hu_light(chip, 0);
	creative_ae9_mic_boost_apply(chip);

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
