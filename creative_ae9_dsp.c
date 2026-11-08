// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 3xHelix
// Contains code adapted from the Linux kernel's sound/pci/hda/patch_ca0132.c:
//   Copyright (c) 2011, Creative Technology Ltd.
//   Based on patch_ca0110.c, Copyright (c) 2008 Takashi Iwai <tiwai@suse.de>
// and from Creative's own ca0132 build ("0.73_kernel18", GPL).
//
// Creative AE-9 CA0132 DSP firmware download (ChipIO / DSPIO / SCP / dspxfr).
//
// Ported/adapted from the mainline sound/pci/hda/patch_ca0132.c DSP machinery.
// The verb transport uses our ICW immediate-command path (creative_ae9_icw_
// send_verb) on the main codec (CAD 1):
//   - ChipIO  via WIDGET_CHIP_CTRL (node 0x15)
//   - DSPIO   via WIDGET_DSP_CTRL  (node 0x16)
//
// Goal: push the extracted FLS firmware (ctefx-desktop.bin) into the DSP so the
// audio path (HDA stream -> DAC node 0x02 -> DSP -> output) actually works.
//

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <sound/hda_register.h>
#include <sound/hdaudio.h>

#include <linux/moduleparam.h>
#include "creative_ae9.h"
#include "ca0132_regs.h"

/*
 * 8051 PLL/PMU writes as the Windows capture does them (address via 0x70d,
 * data via 0x70c): 44=c8 43=cc 45=cb 40=c7 42=cd 41=ce 51=db, and only
 * 00=ff for clock enable. The old tables had address and data swapped
 * (c8=44 ...), plus 44=c2 and 05=0b/06=ff, none of which Windows writes.
 */

/*
 * SCP GET replies: Windows reads the reply header, then sends 0x704 p=0x02,
 * polls status and reads a count via 0xF04 before reading the data words.
 */
/*
 * 0x800000-0x8000fc holds the firmware's SCP queue pointers. It reads
 * 0x0bad0add on real hardware too, not only under QEMU. Zeroing it while
 * the firmware runs (as the old code did right after boot) wipes those
 * pointers. Windows never touches this region.
 */
unsigned int ae9_sentinel_clear;
module_param_named(sentinel_clear, ae9_sentinel_clear, uint, 0444);
MODULE_PARM_DESC(sentinel_clear, "Zero 0x800000-0x8000fc when it reads 0x0bad0add: 1=before download and after boot (old), 2=before download only, 0=never (default)");

/*
 * scp_event=1: handle SCP replies the way the Windows capture does. The
 * DSP announces each reply with an unsolicited response; Windows only
 * then reads it (0x702/0xF01/0xF02 header, 0x704 p=2/0xF01/0xF04 count,
 * 0x702/0xF01/0xF02 per data word). In the capture all 160 post-reads
 * follow an announcement; our old path sent thousands speculatively
 * (queue drain before every message, polling for GET replies).
 */
static unsigned int scp_event = 1;
module_param(scp_event, uint, 0444);
MODULE_PARM_DESC(scp_event, "Read SCP replies only after the DSP announces them, as Windows does (1=on, default)");

/*
 * stream_rings_off=1: stop the command rings while a boot stream runs
 * (firmware download, overlay transfer) and send commands through the
 * immediate interface meanwhile; restart the rings after each. On an
 * ASMedia-bridged card the rings running alongside any stream took the
 * card off the bus (bugzilla 219552 round 2), each alone was fine.
 */
static unsigned int stream_rings_off;
module_param(stream_rings_off, uint, 0444);
MODULE_PARM_DESC(stream_rings_off, "Stop the command rings during the boot streams (firmware, overlay) (1=yes; 0=no, default; diagnostic)");

/*
 * fw_posbuf=1: turn on the DMA position buffer during the firmware
 * download, so the card writes to memory while the stream reads. With
 * the rings off this tells whether any card write during a stream (the
 * RIRB is the card's other writer) takes an ASMedia bridge down.
 */
static unsigned int fw_posbuf;
module_param(fw_posbuf, uint, 0444);
MODULE_PARM_DESC(fw_posbuf, "DMA position buffer on during the firmware download (1=yes; 0=no, default; diagnostic)");

/* stream_rings_off: park the rings before a stream; true when parked */
static bool ae9_rings_park(struct creative_chip *chip, const char *what)
{
	unsigned int t;

	if (!stream_rings_off || !chip->corb_buf || chip->card_gone)
		return false;
	creative_ae9_corb_free(chip);
	/* wait for both ring engines to report stopped (RUN bits clear) */
	for (t = 0; t < 100; t++) {
		if (!(creative_hda_readb(chip, AZX_REG_CORBCTL) & 0x02) &&
		    !(creative_hda_readb(chip, AZX_REG_RIRBCTL) & 0x02))
			break;
		udelay(10);
	}
	/* re-arm the immediate interface the way the bring-up does before
	 * the rings first start: once the rings have run, it stays busy
	 * (IRS=0x5) until this is done again (tested on the IDT card) */
	creative_hda_writeb(chip, AZX_REG_IRS, 0x5);
	creative_hda_writeb(chip, AZX_REG_IRS, 0x0);
	creative_hda_writeb(chip, AZX_REG_IRS, 0x4);
	chip->last_irs_ack = 0x6;
	dev_info(chip->dev, "%s: command rings stopped for the stream (%u us; IRS now 0x%04x)\n",
		 what, t * 10, creative_hda_readw(chip, AZX_REG_IRS));
	return true;
}

static int ae9_rings_unpark(struct creative_chip *chip, const char *what)
{
	int ret;

	if (chip->card_gone)
		return -ENODEV;
	ret = creative_ae9_corb_init(chip);
	dev_info(chip->dev, "%s: command rings back after the stream: %d\n", what, ret);
	if (creative_ae9_gone(chip, what))
		return -ENODEV;
	return ret;
}

/*
 * fw_verify=1: after the firmware download and before starting the DSP,
 * read samples of every segment back through ChipIO and compare them with
 * the image. Done while the DSP is still halted, so data memory hasn't
 * been touched by running firmware yet.
 */
static unsigned int fw_verify;
module_param(fw_verify, uint, 0444);
MODULE_PARM_DESC(fw_verify, "Read back the downloaded firmware before starting the DSP and compare (1=on)");

#define DSPLOAD_BDL_BYTES	1024	/* room for 64 entries */

/*
 * overlay=1: during the boot handshake, after state 3, transfer the overlay
 * image (ctefx-ae9-overlay.bin, the second FLS image in CtxHda.sys; see
 * extract_overlay.py) the way the cold-boot capture does (seq 4468-4845).
 */
static unsigned int overlay = 1;
module_param(overlay, uint, 0444);
MODULE_PARM_DESC(overlay, "Transfer the DSP overlay image (ctefx-ae9-overlay.bin) during the boot handshake (1=on, default)");
#define AE9_OVERLAY_FILE	"ctefx-ae9-overlay.bin"

/* wait between a CA0113 command's value write and its commit: 20 ms for
 * the boot commands; volume changes (replay_quiet) use hu_cmd_settle_us */
#define CA0113_SETTLE_US	20000
static unsigned int hu_cmd_settle_us = 4000;
module_param(hu_cmd_settle_us, uint, 0644);
MODULE_PARM_DESC(hu_cmd_settle_us, "post_windows: CA0113 settle time for volume changes (us)");

#define CAD			1		/* main codec address */
#define WIDGET_CHIP_CTRL	0x15
#define WIDGET_DSP_CTRL		0x16

/* ChipIO vendor verbs */
#define VENDOR_CHIPIO_ADDRESS_LOW	0x000
#define VENDOR_CHIPIO_ADDRESS_HIGH	0x100
#define VENDOR_CHIPIO_DATA_LOW		0x300
#define VENDOR_CHIPIO_DATA_HIGH		0x400
#define VENDOR_CHIPIO_STATUS		0xF01
#define VENDOR_CHIPIO_HIC_POST_READ	0x702
#define VENDOR_CHIPIO_HIC_READ_DATA	0xF03
#define VENDOR_CHIPIO_FLAG_SET		0x70F
#define VENDOR_CHIPIO_PARAM_SET		0x710
#define VENDOR_CHIPIO_8051_DATA_WRITE	0x707
#define VENDOR_CHIPIO_PLL_PMU_WRITE	0x70C
#define VENDOR_CHIPIO_8051_ADDRESS_LOW	0x70D
#define VENDOR_CHIPIO_8051_ADDRESS_HIGH	0x70E
#define VENDOR_CHIPIO_CT_EXTENSIONS_ENABLE 0x70A
#define VENDOR_CHIPIO_STREAM_FORMAT	0x200
#define VENDOR_CHIPIO_PORT_ALLOC_CONFIG_SET 0x711
#define VENDOR_CHIPIO_PORT_ALLOC_SET	0x712
#define VENDOR_CHIPIO_PORT_ALLOC_GET	0xF12
#define VENDOR_CHIPIO_PORT_FREE_SET	0x713
#define MEM_CONNID_DSP			16
#define DMA_TRANSFER_FRAME_SIZE_NWORDS	8
#define INVALID_DMA_CHANNEL		(~0U)
#define INVALID_CHIP_ADDX		(~0U)

/* FLS image */
#define FLS_MAGIC	0x4c46584d
#define FLS_CHIP_ADDR_MAGIC	0xFFFFFF01

struct ae9_dsp_image_seg {
	u32 magic;
	u32 chip_addr;
	u32 count;
	u32 data[];
};

/*
 * Check a whole FLS image before anything walks it: every segment header
 * and its data must lie inside the file, every header must carry the
 * magic, and the image must end with a zero-count terminator. The segment
 * walks in the loaders trust count, so a short or corrupt file would
 * otherwise read past the end of the firmware buffer.
 */
static int fls_image_check(struct creative_chip *chip, const struct firmware *fw,
			   const char *name)
{
	size_t off = 0;

	while (off + sizeof(struct ae9_dsp_image_seg) <= fw->size) {
		const struct ae9_dsp_image_seg *p = (const void *)(fw->data + off);

		if (p->magic != FLS_MAGIC)
			break;
		if (p->count == 0)
			return 0;
		if (p->count > (fw->size - off - sizeof(*p)) / 4)
			break;
		off += sizeof(*p) + (size_t)p->count * 4;
	}
	dev_err(chip->dev, "%s: not a valid FLS image (size %zu, problem at offset %zu)\n",
		name, fw->size, off);
	return -EINVAL;
}

/* hda format field accessors */
#define get_hdafmt_chs(fmt)	((fmt) & 0xf)
#define get_hdafmt_rate(fmt)	(((fmt) >> 8) & 0x7f)

/* DSPIO vendor verbs */
#define VENDOR_DSPIO_SCP_WRITE_DATA_LOW	 0x000
#define VENDOR_DSPIO_SCP_WRITE_DATA_HIGH 0x100
#define VENDOR_DSPIO_STATUS		 0xF01
#define VENDOR_DSPIO_SCP_POST_READ_DATA	 0x702
#define VENDOR_DSPIO_SCP_READ_DATA	 0xF02
#define VENDOR_DSPIO_DSP_INIT		 0x703

/* status codes */
#define VENDOR_STATUS_CHIPIO_OK		0x00
#define VENDOR_STATUS_DSPIO_OK		0x00
#define VENDOR_STATUS_DSPIO_BUSY	0x01
#define VENDOR_STATUS_DSPIO_SCP_COMMAND_QUEUE_FULL   0x02
#define VENDOR_STATUS_DSPIO_SCP_RESPONSE_QUEUE_EMPTY 0x03

#define SCP_SET 0
#define SCP_GET 1
#define SCP_MAX_DATA_WORDS 16
#define MASTERCONTROL 0x80
#define MASTERCONTROL_ALLOC_DMA_CHAN 10

#define DESKTOP_EFX_FILE "ctefx-desktop.bin"
#define DESKTOP_EFX_SIZE 655436		/* the tested AE-9 image */
#define AE9_OVERLAY_SIZE 432

/*
 * Creative's DSP images live in creative-ae9/ (ae9-get-firmware 0.10.1+).
 * "ctefx-desktop.bin" in the firmware root is also the AE-5/AE-7 file in
 * linux-firmware (Kaiba, 2026-11-07: clashed on Arch), so the old location
 * is used only as a fallback, and only if it's the AE-9 image's size.
 */
static int ae9_request_fw(struct creative_chip *chip, const struct firmware **fw,
			  const char *name, size_t size)
{
	char path[64];
	int ret;

	snprintf(path, sizeof(path), "creative-ae9/%s", name);
	if (!firmware_request_nowarn(fw, path, chip->dev))
		return 0;
	ret = request_firmware(fw, name, chip->dev);
	if (ret)
		return ret;
	if ((*fw)->size != size) {
		dev_err(chip->dev, "firmware %s is not the AE-9 image (%zu bytes, the AE-9's is %zu): probably the AE-5/AE-7 file from linux-firmware; run ae9-get-firmware\n",
			name, (*fw)->size, size);
		release_firmware(*fw);
		*fw = NULL;
		return -EINVAL;
	}
	dev_info_once(chip->dev, "firmware %s from its old location; ae9-get-firmware now installs it to creative-ae9/\n",
		      name);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Verb transport — CORB when available, ICW otherwise		  */
/* ------------------------------------------------------------------ */

/* Send one verb; route through CORB if chip->corb_buf is set,
 * fall back to ICW before corb_init runs.
 */
static int verb_send(struct creative_chip *chip, u32 verb, u32 *resp)
{
	if (chip->corb_buf)
		return creative_ae9_corb_send_verb_sync(chip, verb, resp);
	return creative_ae9_icw_send_verb(chip, verb, resp);
}

/* ------------------------------------------------------------------ */
/* ICW/CORB verb transport wrappers (return the codec response value) */
/* ------------------------------------------------------------------ */
static unsigned int dsp_read(struct creative_chip *chip, u8 nid,
			     u16 verb, u16 payload)
{
	u32 resp = 0;

	if (verb_send(chip, creative_ae9_verb(CAD, nid, verb, payload), &resp) < 0)
		return ~0U;
	return resp;
}

static void dsp_write(struct creative_chip *chip, u8 nid, u16 verb, u16 payload)
{
	u32 resp = 0;

	verb_send(chip, creative_ae9_verb(CAD, nid, verb, payload), &resp);
}

/* ------------------------------------------------------------------ */
/* ChipIO layer							  */
/* ------------------------------------------------------------------ */
static int chipio_send(struct creative_chip *chip, unsigned int reg,
		       unsigned int data)
{
	unsigned int res;
	unsigned long timeout;

	/*
	 * After standup the 8051 is in audio-loop mode and stops servicing
	 * CORB promptly. Retry loops here cause 1s+ timeouts per ChipIO op
	 * that cascade into CORB ring jams. Use a single fast attempt: if
	 * the chip is not immediately ready, return -EIO and let the caller
	 * proceed anyway — most ChipIO writes in the PCM start path are
	 * not strictly required to poll for completion.
	 */
	if (chip->standup_done) {
		res = dsp_read(chip, WIDGET_CHIP_CTRL, reg, data);
		return (res == VENDOR_STATUS_CHIPIO_OK) ? 0 : -EIO;
	}

	timeout = jiffies + msecs_to_jiffies(1000);
	do {
		res = dsp_read(chip, WIDGET_CHIP_CTRL, reg, data);
		if (res == VENDOR_STATUS_CHIPIO_OK)
			return 0;
		usleep_range(500, 1000);
	} while (time_before(jiffies, timeout));

	return -EIO;
}

static unsigned int curr_chip_addx;

/*
 * Many paths move the hardware ChipIO address behind this cache (raw verb
 * replays, bursts, AFG resets), and reads after firmware load have come
 * back with another register's or the previous read's data. Sending the
 * address on every access removes the cache from the picture.
 */

static int chipio_write_address(struct creative_chip *chip,
				unsigned int chip_addx)
{
	int res;

	res = chipio_send(chip, VENDOR_CHIPIO_ADDRESS_LOW, chip_addx & 0xffff);
	if (res != -EIO)
		res = chipio_send(chip, VENDOR_CHIPIO_ADDRESS_HIGH,
				  chip_addx >> 16);

	curr_chip_addx = (res < 0) ? ~0U : chip_addx;
	return res;
}

static int chipio_write_data(struct creative_chip *chip, unsigned int data)
{
	int res;

	res = chipio_send(chip, VENDOR_CHIPIO_DATA_LOW, data & 0xffff);
	if (res != -EIO)
		res = chipio_send(chip, VENDOR_CHIPIO_DATA_HIGH, data >> 16);

	curr_chip_addx = (res != -EIO) ? (curr_chip_addx + 4) : ~0U;
	return res;
}

static int chipio_read_data(struct creative_chip *chip, unsigned int *data)
{
	int res;

	res = chipio_send(chip, VENDOR_CHIPIO_HIC_POST_READ, 0);
	if (res != -EIO)
		res = chipio_send(chip, VENDOR_CHIPIO_STATUS, 0);
	if (res != -EIO)
		*data = dsp_read(chip, WIDGET_CHIP_CTRL,
				 VENDOR_CHIPIO_HIC_READ_DATA, 0);

	curr_chip_addx = (res != -EIO) ? (curr_chip_addx + 4) : ~0U;
	return res;
}

int creative_dsp_chipio_write(struct creative_chip *chip,
			      unsigned int addx, unsigned int data)
{
	int err = chipio_write_address(chip, addx);

	if (err < 0)
		return err;
	return chipio_write_data(chip, data);
}

int creative_dsp_chipio_read(struct creative_chip *chip,
			     unsigned int addx, unsigned int *data)
{
	int err = chipio_write_address(chip, addx);

	if (err < 0)
		return err;
	return chipio_read_data(chip, data);
}

/* ------------------------------------------------------------------ */
/* DSPIO + SCP layer						  */
/* ------------------------------------------------------------------ */
static int dspio_send(struct creative_chip *chip, unsigned int reg,
		      unsigned int data)
{
	int res;
	unsigned long timeout;

	/*
	 * Post-standup: single attempt only. The 8051 is in audio-loop mode;
	 * the retry loop's dsp_read() calls block on CORB for 1s each and
	 * fill the ring. If the SCP queue accepts the write on the first try,
	 * great; if not, return -EIO and let the caller proceed.
	 */
	if (chip->standup_done) {
		res = dsp_read(chip, WIDGET_DSP_CTRL,
			       reg | ((data >> 8) & 0xff),
			       data & 0xff);
		return (res >= 0 && res != VENDOR_STATUS_DSPIO_BUSY) ? res : -EIO;
	}

	/*
	 * HDA verb encoding: creative_ae9_verb(cad, nid, verb, payload) puts
	 * 'verb' in bits [19:8] (12 bits) and 'payload' in bits [7:0] (8 bits).
	 * For SCP writes, 'data' is a 16-bit value. Encoding only in payload
	 * would drop the upper 8 bits. The correct encoding — confirmed from
	 * cold-boot CORB capture — puts the upper byte in the verb field:
	 *   verb    = reg | (data >> 8)
	 *   payload = data & 0xff
	 * e.g. DATA_LOW(0x000) with value 0x2032: verb=0x020, payload=0x32.
	 * This matches exactly what the Windows driver sends (CORB L5201-5259).
	 */
	timeout = jiffies + msecs_to_jiffies(1000);
	do {
		res = dsp_read(chip, WIDGET_DSP_CTRL,
			       reg | ((data >> 8) & 0xff),
			       data & 0xff);
		if (res >= 0 && res != VENDOR_STATUS_DSPIO_BUSY)
			return res;
		usleep_range(500, 1000);
	} while (time_before(jiffies, timeout));

	return -EIO;
}

static void dspio_write_wait(struct creative_chip *chip)
{
	int status;
	unsigned long timeout;

	/*
	 * After standup the 8051 is in audio-loop mode. dsp_read() calls
	 * corb_send_verb_sync() which waits 1s for RIRB — multiple calls
	 * fill the CORB ring. Use a single fast attempt post-standup,
	 * same as chipio_send(). If not immediately ready, proceed anyway.
	 */
	if (chip->standup_done) {
		dsp_read(chip, WIDGET_DSP_CTRL, VENDOR_DSPIO_STATUS, 0);
		return;
	}

	/*
	 * Wait until the SCP command queue has space before writing the next
	 * word. Match the decompiled driver's dspio_scp pre-write poll:
	 *   - Exit on OK (0x00) or RESPONSE_QUEUE_EMPTY (0x03)
	 *   - Sleep 20ms between retries (decompiled uses msleep(1) in the
	 *     pre-send poll and msleep(20) in dspio_send; we use 20ms here
	 *     to give the 8051 enough time to drain queued commands)
	 *
	 * The previous version exited only on 0x00 (both queues clear), which
	 * often timed out because the response queue refilled continuously.
	 * Accepting 0x03 (response empty, command queue may have space) and
	 * using 20ms sleep matches the decompiled's timing and gives 0x32/0x00
	 * (source select = FLOAT_TWO) a clear command queue slot to land in.
	 */
	timeout = jiffies + msecs_to_jiffies(2000);
	do {
		status = dsp_read(chip, WIDGET_DSP_CTRL, VENDOR_DSPIO_STATUS, 0);
		if (status == VENDOR_STATUS_DSPIO_OK ||
		    status == VENDOR_STATUS_DSPIO_SCP_RESPONSE_QUEUE_EMPTY)
			break;
		msleep(20); /* match decompiled: give 8051 time to drain commands */
	} while (time_before(jiffies, timeout));
}

static void dspio_clear_response_queue(struct creative_chip *chip);
static int dsp_set_run_state(struct creative_chip *chip);
/* Forward declarations for functions called during standup */

static int dspio_write(struct creative_chip *chip, unsigned int scp_data)
{
	int status;
	unsigned long timeout;
	int kicked = 0;

	dspio_write_wait(chip);

	status = dspio_send(chip, VENDOR_DSPIO_SCP_WRITE_DATA_LOW,
			    scp_data & 0xffff);
	if (status < 0)
		return status;
	/*
	 * DATA_HIGH carries scp_data bits [31:16]; dspio_send() already puts
	 * the upper byte in the verb field. Windows sends header 0x08180080
	 * as verb 0x108 payload 0x18 (capture seq 4570). The Jun 3 change
	 * (bf7ab3d) took bits [23:20] instead and dropped bits [31:24]: the
	 * header's data size and flags, and the top byte of every data word.
	 */
	status = dspio_send(chip, VENDOR_DSPIO_SCP_WRITE_DATA_HIGH,
			    scp_data >> 16);
	if (status < 0)
		return status;

	/*
	 * If the SCP COMMAND queue is full, the DSP hasn't drained prior
	 * commands yet. Intermittently (slightly slower DSP) the standup's
	 * burst of dspio_set_uint_param calls overruns it, leaving the queue
	 * stuck full and every later SCP op failing -EIO. Wait for the DSP to
	 * drain the command queue (status != QUEUE_FULL) before returning,
	 * instead of giving up. Drains the response queue too to help it move.
	 */
	/*
	 * If the command queue is full, the DSP (8051) has stopped draining
	 * it. This happens intermittently when the 8051 falls into a halt
	 * state after the standup (DBGCNTL STATE!=0). Kick the DSP run-state
	 * to un-halt it so it resumes consuming the queue, then wait. Without
	 * this the queue stays full and every later SCP op fails -EIO
	 * (select_lineout etc).
	 */
	timeout = jiffies + msecs_to_jiffies(500);
	do {
		status = dsp_read(chip, WIDGET_DSP_CTRL,
				  VENDOR_DSPIO_STATUS, 0);
		if (status != VENDOR_STATUS_DSPIO_SCP_COMMAND_QUEUE_FULL)
			return 0;
		if (!kicked) {
			unsigned int dbg = ~0U;

			/* Un-halt the 8051 so it resumes draining the queue.
			 * Logged: this writes DBGCNTL, a suspect in the one
			 * Linux DSP halt (2026-11-07 22:25, note 52d). */
			creative_dsp_chipio_read(chip, DSP_DBGCNTL_INST_OFFSET, &dbg);
			dev_warn_ratelimited(chip->dev, "DSP command queue full (DBGCNTL 0x%08x): kicking the DSP's run state\n",
					     dbg);
			dsp_set_run_state(chip);
			kicked = 1;
		}
		usleep_range(50, 100);
	} while (time_before(jiffies, timeout));

	return -EIO;
}

static int dspio_write_multiple(struct creative_chip *chip,
				unsigned int *buffer, unsigned int size)
{
	int status = 0;
	unsigned int count = 0;

	if (!buffer)
		return -EINVAL;
	while (count < size) {
		status = dspio_write(chip, *buffer++);
		if (status != 0)
			break;
		count++;
	}
	return status;
}

static int dspio_read(struct creative_chip *chip, unsigned int *data)
{
	int status;

	status = dspio_send(chip, VENDOR_DSPIO_SCP_POST_READ_DATA, 0);
	if (status == -EIO)
		return status;
	status = dspio_send(chip, VENDOR_DSPIO_STATUS, 0);
	if (status == -EIO ||
	    status == VENDOR_STATUS_DSPIO_SCP_RESPONSE_QUEUE_EMPTY)
		return -EIO;

	*data = dsp_read(chip, WIDGET_DSP_CTRL, VENDOR_DSPIO_SCP_READ_DATA, 0);
	return 0;
}

static int dspio_read_multiple(struct creative_chip *chip, unsigned int *buffer,
			       unsigned int *buf_size, unsigned int size_count)
{
	int status = 0;
	unsigned int size = *buf_size;
	unsigned int count = 0, skip_count, dummy;

	if (!buffer)
		return -1;
	while (count < size && count < size_count) {
		status = dspio_read(chip, buffer++);
		if (status != 0)
			break;
		count++;
	}
	skip_count = count;
	if (status == 0) {
		while (skip_count < size) {
			status = dspio_read(chip, &dummy);
			if (status != 0)
				break;
			skip_count++;
		}
	}
	*buf_size = count;
	return 0;
}

/*
 * Drain any stale SCP responses so the queue isn't reported FULL (0x02) when
 * we start a new transaction. Mirrors mainline dspio_clear_response_queue.
 * Reads responses until the status reports the queue is empty (0x03) or we
 * time out. Live debugging showed the DSPIO status stuck at 0x02 (queue full)
 * after failed standup attempts; draining returns it to 0x00/0x03.
 */
static void scp_drain_announced(struct creative_chip *chip);

/* Recover from a late SCP reply instead of reading every later reply one
 * behind (see dspio_scp_unlocked). 0 = old behaviour, for comparison. */

/* The DSP announces SCP replies through the RIRB: without running rings
 * (corb_rings=0, or fw_rings_off=1 before the rings start) poll instead. */
static bool scp_ev(struct creative_chip *chip)
{
	return scp_event && chip->corb_buf;
}

static void dspio_clear_response_queue(struct creative_chip *chip)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(1000);
	unsigned int dummy = 0;
	int status = -1;

	if (scp_ev(chip)) {
		scp_drain_announced(chip);
		return;
	}

	do {
		status = dspio_read(chip, &dummy);
	} while (status == 0 && time_before(jiffies, timeout));
}

/* SCP header build/extract */
static unsigned int make_scp_header(unsigned int target_id,
				    unsigned int source_id,
				    unsigned int get_flag, unsigned int req,
				    unsigned int device_flag,
				    unsigned int resp_flag,
				    unsigned int error_flag,
				    unsigned int data_size)
{
	unsigned int header = 0;

	header = (data_size & 0x1f) << 27;
	header |= (error_flag & 0x01) << 26;
	header |= (resp_flag & 0x01) << 25;
	header |= (device_flag & 0x01) << 24;
	header |= (req & 0x7f) << 17;
	header |= (get_flag & 0x01) << 16;
	header |= (source_id & 0xff) << 8;
	header |= target_id & 0xff;
	return header;
}

static void extract_scp_header(unsigned int header, unsigned int *resp_flag,
			       unsigned int *error_flag,
			       unsigned int *data_size)
{
	if (data_size)
		*data_size = (header >> 27) & 0x1f;
	if (error_flag)
		*error_flag = (header >> 26) & 0x01;
	if (resp_flag)
		*resp_flag = (header >> 25) & 0x01;
}

struct scp_msg {
	unsigned int hdr;
	unsigned int data[SCP_MAX_DATA_WORDS];
};

/*
 * DSP reply reads, the Windows way (decompile FUN_00027ce0 / FUN_00027e9c /
 * FUN_0001dae8; notes 14-17, 2026-11-07). One word: post the read (0x702,
 * again while it answers 1 = busy), read the status (0xF01, again while 1),
 * then 3 = queue empty (stop), anything else = read the word (0xF02). The
 * old reader went straight to 0xF02: while the DSP was still busy that read
 * 0, the real word came in the next read and every GET after was one reply
 * behind (Kaiba's silent DSP; the address 0 that wrote the EQ over Y memory).
 * Windows never read an empty header in 670 replies (15 captures), and at
 * least 0.6 ms passed between its 0x702 and 0xF02; we keep that gap too in
 * case the status never shows busy at full speed.
 */
#define SCP_BUSY_MS	50	/* Windows: 5 tries 10 ms apart */

static int scp_wait_not_busy(struct creative_chip *chip, unsigned int verb,
			     unsigned int parm, unsigned int *res)
{
	unsigned long until = jiffies + msecs_to_jiffies(SCP_BUSY_MS);

	for (;;) {
		*res = dsp_read(chip, WIDGET_DSP_CTRL, verb, parm);
		if (*res != VENDOR_STATUS_DSPIO_BUSY)
			return *res == ~0U ? -EIO : 0;
		if (!time_before(jiffies, until) || chip->card_gone)
			return -EBUSY;
		usleep_range(500, 1000);
	}
}

static unsigned int scp_last_pr, scp_last_st;	/* for the logs */

static int scp_read_word(struct creative_chip *chip, u32 *w)
{
	unsigned int r;
	int ret;

	ret = scp_wait_not_busy(chip, VENDOR_DSPIO_SCP_POST_READ_DATA, 0, &r);
	scp_last_pr = r;
	if (ret)
		return ret;
	usleep_range(600, 800);	/* Windows' 0x702 -> 0xF02 was never under 0.6 ms */
	ret = scp_wait_not_busy(chip, VENDOR_DSPIO_STATUS, 0, &r);
	scp_last_st = r;
	if (ret)
		return ret;
	if (r == VENDOR_STATUS_DSPIO_SCP_RESPONSE_QUEUE_EMPTY)
		return -ENODATA;
	if (r == VENDOR_STATUS_DSPIO_SCP_COMMAND_QUEUE_FULL)
		return -EIO;
	*w = dsp_read(chip, WIDGET_DSP_CTRL, VENDOR_DSPIO_SCP_READ_DATA, 0);
	return 0;
}

/*
 * Read one reply from the DSP's reply queue: header, the count (logged
 * only; Windows doesn't use it), then the header's number of words.
 * Returns the words stored, -EAGAIN if the queue is empty, -EBADMSG for a
 * header that can't be a reply (no reply flag, all-ones, too long: a
 * failed read would otherwise pull 31 words out of the queue), other
 * negatives on a failed read.
 */
static unsigned int scp_queue_words(struct creative_chip *chip)
{
	unsigned int r;

	if (scp_wait_not_busy(chip, 0x704, 0x02, &r) ||
	    scp_wait_not_busy(chip, VENDOR_DSPIO_STATUS, 0, &r) ||
	    r == VENDOR_STATUS_DSPIO_SCP_RESPONSE_QUEUE_EMPTY)
		return 0;
	r = dsp_read(chip, WIDGET_DSP_CTRL, 0xf04, 0);
	return r == ~0U ? 0 : r;
}

static bool scp_hdr_ok(u32 h)
{
	return h != ~0U && (h & BIT(25)) && ((h >> 27) & 0x1f) <= SCP_MAX_DATA_WORDS;
}

static unsigned int scp_last_cnt;	/* queue words after the last header */

/*
 * Read one reply exactly as Windows does (FUN_00027ce0 / FUN_00027e9c):
 * header word, 704 p=2 / F01 / F04 (the words still queued: the rest of
 * this reply plus any later ones; Windows always saw 1), then the
 * header's number of words. No re-reads (Windows never re-reads F02:
 * 0 of 1324) and no skipping of strays (a data word like 0x3f800000
 * passes as a header). Returns the words stored, -EAGAIN if the queue
 * is empty, -EBADMSG for a word that can't be a header.
 */
static int scp_read_reply(struct creative_chip *chip, u32 *hdr, u32 *data,
			  unsigned int max)
{
	unsigned int size, i;
	u32 w;
	int ret;

	ret = scp_read_word(chip, hdr);
	if (ret == -ENODATA)
		return -EAGAIN;
	if (ret)
		return ret;
	if (!scp_hdr_ok(*hdr)) {
		/*
		 * The header read came back empty, but the header itself has
		 * left the queue: the next read gets this reply's data word
		 * (seen every time: hdr 0, then 0x1d5e; scpprobe and verb log,
		 * 2026-11-07). So discard what the queue count says is left of
		 * it, and the next GET starts in step. A count that reads 0
		 * skips this (the safe direction).
		 */
		unsigned int left = scp_queue_words(chip), k;

		for (k = 0; k < left && k < SCP_MAX_DATA_WORDS; k++)
			if (scp_read_word(chip, &w))
				break;
		dev_info_ratelimited(chip->dev, "scp: not a reply header 0x%08x (post-read 0x%x, status 0x%x); %u queued word(s) discarded\n",
				     *hdr, scp_last_pr, scp_last_st, k);
		return -EBADMSG;
	}
	size = (*hdr >> 27) & 0x1f;
	scp_last_cnt = scp_queue_words(chip);
	for (i = 0; i < size; i++) {
		ret = scp_read_word(chip, &w);
		if (ret) {
			dev_info_ratelimited(chip->dev, "scp: reply 0x%08x word %u of %u not read (%d)\n",
					     *hdr, i, size, ret);
			return ret == -ENODATA ? -EBADMSG : ret;
		}
		if (i < max)
			data[i] = w;
	}
	return min(size, max);
}

/*
 * Before a message, if the DSP announced replies nobody read: read out the
 * reply queue (replies nobody waited for, e.g. the DSP's error replies to some SETs), stopping
 * at "queue empty" like Windows' work item (FUN_0001dae8). Safe now that
 * every read waits out busy and stops on status 3 (the blind version of
 * this broke DSP mode, build E08FCE05). Then the announcement count is
 * level again, so a miscount can't carry over to the next GET.
 */
static void scp_drain_announced(struct creative_chip *chip)
{
	unsigned int guard = 16;
	u32 hdr, junk[SCP_MAX_DATA_WORDS];
	int n;

	/* only when the DSP announced something nobody read, as Windows
	 * (its work item runs on an announcement with no GET waiting) */
	if ((int)(atomic_read(&chip->scp_unsol) - chip->scp_unsol_done) <= 0)
		return;
	while (guard--) {
		n = scp_read_reply(chip, &hdr, junk, ARRAY_SIZE(junk));
		if (n == -EAGAIN)
			break;
		if (n < 0)
			break;
		dev_info_ratelimited(chip->dev,
			"scp: cleared a reply nobody waited for: hdr 0x%08x (%d words, first 0x%08x)\n",
			hdr, n, n > 0 ? junk[0] : 0);
	}
	chip->scp_unsol_done = atomic_read(&chip->scp_unsol);
}

/* simplified SCP send: we only need SET (no reply) and basic GET for dma_chan */
static int dspio_scp_unlocked(struct creative_chip *chip, int mod_id,
			      int src_id, int req, int dir, const void *data,
			      unsigned int len, void *reply,
			      unsigned int *reply_len);

/*
 * One SCP message at a time: playback start, capture start, the head-unit
 * heartbeat and control changes can all send from different threads, and
 * interleaved messages crossed their replies (failed sends, "drained
 * unexpected reply", multi-second stalls).
 */
static int dspio_scp(struct creative_chip *chip, int mod_id, int src_id,
		     int req, int dir, const void *data, unsigned int len,
		     void *reply, unsigned int *reply_len)
{
	int ret;

	mutex_lock(&chip->scp_mutex);
	ret = dspio_scp_unlocked(chip, mod_id, src_id, req, dir, data, len,
				 reply, reply_len);
	/*
	 * The first failed message since the DSP started, in full: later
	 * failures are usually knock-on effects, the first one says how it
	 * began (note 52d: the DSP core halted, the replies were fine).
	 */
	if (ret < 0 && ret != -ENODEV && !chip->scp_fail_logged) {
		unsigned int dbg = ~0U, sts;

		chip->scp_fail_logged = true;
		sts = dsp_read(chip, WIDGET_DSP_CTRL, VENDOR_DSPIO_STATUS, 0);
		creative_dsp_chipio_read(chip, DSP_DBGCNTL_INST_OFFSET, &dbg);
		creative_dsp_chipio_addr_invalidate();
		dev_warn(chip->dev, "DSP: first failed message since start: %s mid 0x%02x src 0x%02x req 0x%02x (%u data words) -> %d; DSPIO status 0x%x, DBGCNTL 0x%08x, verb timeouts %u, replies announced %d/read %u\n",
			 dir == SCP_GET ? "GET" : "SET", mod_id, src_id, req,
			 len / 4, ret, sts, dbg, chip->corb_timeouts,
			 atomic_read(&chip->scp_unsol), chip->scp_unsol_done);
	}
	mutex_unlock(&chip->scp_mutex);
	return ret;
}

static int dspio_scp_unlocked(struct creative_chip *chip, int mod_id,
			      int src_id, int req, int dir, const void *data,
			      unsigned int len, void *reply,
			      unsigned int *reply_len)
{
	if (chip->card_gone)
		return -ENODEV;
	struct scp_msg scp_send, scp_reply;
	unsigned int data_size = len / sizeof(unsigned int);
	unsigned int scp_send_size, i;
	int status;
	unsigned int resp_flag, error_flag, reply_data_size;

	memset(&scp_send, 0, sizeof(scp_send));
	memset(&scp_reply, 0, sizeof(scp_reply));

	if ((len != 0 && !data) || data_size > SCP_MAX_DATA_WORDS)
		return -EINVAL;

	/* Drain stale responses so the command queue isn't stuck FULL. */
	if (scp_ev(chip))
		scp_drain_announced(chip);
	else
		dspio_clear_response_queue(chip);

	scp_send.hdr = make_scp_header(mod_id, src_id, (dir == SCP_GET), req,
				       0, 0, 0, data_size);
	if (data && len)
		memcpy(scp_send.data, data, min_t(unsigned int,
						  sizeof(scp_send.data), len));

	scp_send_size = data_size + 1;	/* header + data words */

	/* arm wait for reply if GET */
	chip->dsp_wait_scp_header = scp_send.hdr & 0xffff0000;
	chip->dsp_wait_scp_header |= ((scp_send.hdr & 0xff) << 8) |
				     ((scp_send.hdr >> 8) & 0xff);
	chip->dsp_wait_scp = (dir == SCP_GET);

	status = dspio_write_multiple(chip, (unsigned int *)&scp_send,
				      scp_send_size);
	if (status < 0) {
		chip->dsp_wait_scp = 0;
		return status;
	}

	if (dir != SCP_GET)
		return 0;

	if (scp_ev(chip)) {
		unsigned long timeout = jiffies + msecs_to_jiffies(1000);
		unsigned long until;
		unsigned int dropped = 0;
		int got = -EAGAIN;

		chip->dsp_wait_scp = 0;
		/*
		 * Wait for the announcement. Some replies come without one
		 * (seen 2026-11-07 in DSP mode with music: 2 of 50 GETs; and
		 * DarkNikGr's AE-9 driver polls the queue for that reason),
		 * and an unannounced reply left in the queue piles up until
		 * the DSP stops answering. So after 200 ms without one, look in
		 * the queue too (status-checked reads, a few at most; the loop
		 * below stops at the first empty or bad header).
		 */
		{
			unsigned long quiet = jiffies + msecs_to_jiffies(200);
			unsigned int peeks = 0;

			while ((int)(atomic_read(&chip->scp_unsol) - chip->scp_unsol_done) <= 0) {
				if (time_after(jiffies, timeout)) {
					dev_info_ratelimited(chip->dev,
						"scp GET mid=0x%02x req=0x%02x: no reply announced (queue looked at %u times; post-read 0x%x, status 0x%x)\n",
						mod_id, req, peeks, scp_last_pr, scp_last_st);
					return -EIO;
				}
				if (creative_ae9_gone(chip, "SCP reply wait"))
					return -ENODEV;
				if (time_after(jiffies, quiet) && peeks < 3) {
					u32 h;

					peeks++;
					quiet = jiffies + msecs_to_jiffies(200);
					got = scp_read_reply(chip, &h, scp_reply.data,
							     SCP_MAX_DATA_WORDS);
					if (got == -EAGAIN)
						continue;
					if (got < 0)
						return -EIO;
					scp_reply.hdr = h;
					if (!((h ^ chip->dsp_wait_scp_header) & 0x00ffffff)) {
						dev_info_ratelimited(chip->dev,
							"scp GET mid=0x%02x req=0x%02x: reply found in the queue without an announcement\n",
							mod_id, req);
						goto have_reply;
					}
					dropped++;
					dev_info_ratelimited(chip->dev,
						"scp GET mid=0x%02x req=0x%02x: dropped an unannounced reply for another request (hdr 0x%08x)\n",
						mod_id, req, h);
					continue;
				}
				/* no verb to the DSP while it prepares the reply */
				creative_ae9_corb_scan_unsol(chip);
				usleep_range(100, 200);
			}
		}
		/*
		 * Give the DSP 1.5 ms after the announcement before the first
		 * read: in DSP mode with music, reading at once lost 4 of 50
		 * headers, reading 1.5 ms later lost none (scpprobe, build
		 * E94FD65B, 2026-11-07 14:37). The verb log showed the same
		 * timing for good and lost reads, so it's odds, not a cliff.
		 */
		usleep_range(1500, 1700);
		/*
		 * Read replies until ours (header match), for up to 50 ms. A
		 * reply for another request (late, or an error reply to a SET)
		 * is dropped with its words and the next one read. Every read
		 * is status-checked, so nothing is read blind.
		 */
		until = jiffies + msecs_to_jiffies(50);
		for (;;) {
			got = scp_read_reply(chip, &scp_reply.hdr, scp_reply.data,
					     SCP_MAX_DATA_WORDS);
			if (got >= 0 &&
			    !((scp_reply.hdr ^ chip->dsp_wait_scp_header) & 0x00ffffff))
				break;
			if (got >= 0) {
				dropped++;
				dev_info_ratelimited(chip->dev,
					"scp GET mid=0x%02x req=0x%02x: dropped a reply for another request (hdr 0x%08x)\n",
					mod_id, req, scp_reply.hdr);
			} else if (got != -EAGAIN) {
				break;
			}
			if (!time_before(jiffies, until) || dropped >= 8 ||
			    creative_ae9_gone(chip, "SCP reply read"))
				break;
			if (got == -EAGAIN)
				usleep_range(200, 400);
		}
have_reply:
		chip->scp_unsol_done = atomic_read(&chip->scp_unsol);
		if (got < 0 ||
		    ((scp_reply.hdr ^ chip->dsp_wait_scp_header) & 0x00ffffff)) {
			dev_info_ratelimited(chip->dev,
				"scp GET mid=0x%02x req=0x%02x: our reply not read (%d, last hdr 0x%08x, %u dropped; post-read 0x%x, status 0x%x)\n",
				mod_id, req, got, got >= 0 ? scp_reply.hdr : 0, dropped,
				scp_last_pr, scp_last_st);
			return -EIO;
		}
		/* words still queued after our reply = replies nobody read:
		 * read them out to "queue empty", as Windows' work item does,
		 * so nothing old is left for the next GET */
		if (scp_last_cnt > (unsigned int)got && scp_last_cnt <= 32) {
			u32 h, junk[SCP_MAX_DATA_WORDS];
			unsigned int k, left = scp_last_cnt - got;

			for (k = 0; k < 8; k++) {
				int n = scp_read_reply(chip, &h, junk, ARRAY_SIZE(junk));

				if (n < 0)
					break;
				dev_info_ratelimited(chip->dev,
					"scp GET mid=0x%02x req=0x%02x: cleared a leftover reply 0x%08x (%u word(s) were queued after ours)\n",
					mod_id, req, h, left);
			}
			chip->scp_unsol_done = atomic_read(&chip->scp_unsol);
		}
		extract_scp_header(scp_reply.hdr, &resp_flag, &error_flag,
				   &reply_data_size);
		if (!resp_flag || error_flag)
			return -EIO;
		if (reply && reply_len) {
			unsigned int rl = min(*reply_len,
					      (unsigned int)got * (unsigned int)sizeof(u32));
			memcpy(reply, scp_reply.data, rl);
			*reply_len = rl;
		}
		return 0;
	}

	/* read the reply */
	{
		unsigned long timeout = jiffies + msecs_to_jiffies(1000);
		unsigned int hdr = 0;

		unsigned int reads = 0, last = 0;
		bool matched = false;

		do {
			if (creative_ae9_gone(chip, "SCP reply poll"))
				break;
			if (dspio_read(chip, &hdr) == 0) {
				reads++;
				last = hdr;
				if ((hdr & 0x00ffffff) == chip->dsp_wait_scp_header) {
					matched = true;
					break;
				}
			}
			msleep(20);
		} while (time_before(jiffies, timeout));

		chip->dsp_wait_scp = 0;
		if (!matched)
			dev_info_ratelimited(chip->dev,
				"scp GET mid=0x%02x req=0x%02x: no reply (want 0x%06x, %u reads, last 0x%08x)\n",
				mod_id, req, chip->dsp_wait_scp_header, reads, last);
		scp_reply.hdr = hdr;
		reply_data_size = hdr >> 27;
		if (reply_data_size > SCP_MAX_DATA_WORDS)
			reply_data_size = SCP_MAX_DATA_WORDS;
		if (matched && reply_data_size) {
			/* Windows: 0x704 p=0x02, status, count via 0xF04 */
			unsigned int cnt;

			dsp_read(chip, WIDGET_DSP_CTRL, 0x704, 0x02);
			dsp_read(chip, WIDGET_DSP_CTRL, VENDOR_DSPIO_STATUS, 0);
			cnt = dsp_read(chip, WIDGET_DSP_CTRL, 0xf04, 0);
			if (cnt != reply_data_size)
				dev_info_ratelimited(chip->dev,
					"scp GET mid=0x%02x req=0x%02x: count 0x%x, header says %u\n",
					mod_id, req, cnt, reply_data_size);
		}
		{
			unsigned int rsz = reply_data_size;

			dspio_read_multiple(chip, scp_reply.data, &rsz,
					    reply_data_size);
		}
	}

	extract_scp_header(scp_reply.hdr, &resp_flag, &error_flag,
			   &reply_data_size);
	if (!resp_flag || error_flag)
		return -EIO;

	if (reply && reply_len) {
		unsigned int rl = min(*reply_len,
				      reply_data_size * (unsigned int)sizeof(u32));
		memcpy(reply, scp_reply.data, rl);
		*reply_len = rl;
	}
	(void)i;
	return 0;
}

/*
 * Debug probe (debugfs "scpprobe <mid> <req> <count> <wait_us>"): send the
 * same GET <count> times and log the raw facts of each reply, bypassing the
 * normal resync: time to the announcement, the status read right before the
 * header read, header, 0xf04 count, first data word. After an empty header
 * it watches 50 ms for a second announcement and logs the status values seen
 * meanwhile (without reading). Caller holds replay_mutex. For finding why
 * replies go one behind (2026-11-07).
 */
int creative_dsp_scp_probe(struct creative_chip *chip, unsigned int mid,
			   unsigned int req, unsigned int count,
			   unsigned int wait_us)
{
	unsigned int i, ok = 0, empty = 0, other = 0, none = 0, bad = 0;
	/* Windows: src 0x00 for MASTERCONTROL, 0x20 for the effect modules */
	unsigned int src = mid == MASTERCONTROL ? 0 : 0x20;

	if (!scp_ev(chip) || !chip->dsp_done)
		return -ENODEV;
	mutex_lock(&chip->scp_mutex);
	scp_drain_announced(chip);
	for (i = 0; i < count && !chip->card_gone; i++) {
		struct scp_msg m = { 0 };
		u32 want, hdr = 0, data[SCP_MAX_DATA_WORDS] = { 0 };
		unsigned int polls = 0;
		ktime_t t0;
		s64 t_ann;
		int n, tries = 0;

		m.hdr = make_scp_header(mid, src, 1, req, 0, 0, 0, 0);
		want = (m.hdr & 0xffff0000) | ((m.hdr & 0xff) << 8) | ((m.hdr >> 8) & 0xff);
		t0 = ktime_get();
		if (dspio_write_multiple(chip, &m.hdr, 1) < 0) {
			dev_info(chip->dev, "scpprobe %u: send failed\n", i);
			break;
		}
		while ((int)(atomic_read(&chip->scp_unsol) - chip->scp_unsol_done) <= 0 &&
		       ktime_us_delta(ktime_get(), t0) < 500000) {
			creative_ae9_corb_scan_unsol(chip);	/* no verb, as Windows */
			polls++;
			usleep_range(100, 200);
		}
		t_ann = (int)(atomic_read(&chip->scp_unsol) - chip->scp_unsol_done) > 0 ?
			ktime_us_delta(ktime_get(), t0) : -1;
		if (t_ann < 0)
			none++;
		if (wait_us)
			usleep_range(wait_us, wait_us + 100);
		/* read with the driver's reader; unannounced: look anyway */
		do {
			n = scp_read_reply(chip, &hdr, data, ARRAY_SIZE(data));
			tries++;
			if (n == -EAGAIN)
				usleep_range(1000, 1500);
		} while (n == -EAGAIN && tries < 20);
		chip->scp_unsol_done = atomic_read(&chip->scp_unsol);
		if (n == -EAGAIN) {
			empty++;
		} else if (n < 0) {
			bad++;
		} else if (!((hdr ^ want) & 0x00ffffff)) {
			ok++;
			if (t_ann >= 0 && tries == 1)
				continue;
		} else {
			other++;
		}
		dev_info(chip->dev, "scpprobe %u: %s, hdr 0x%08x data 0x%08x, announced %s%lld us, %d read tries, post-read 0x%x status 0x%x\n",
			 i, n == -EAGAIN ? "QUEUE EMPTY" : n < 0 ? "READ FAILED" :
			 (hdr ^ want) & 0x00ffffff ? "NOT OURS" : "ok",
			 hdr, data[0], t_ann < 0 ? "NOT (" : "after ", t_ann, tries,
			 scp_last_pr, scp_last_st);
	}
	mutex_unlock(&chip->scp_mutex);
	dev_info(chip->dev, "scpprobe 0x%02x/0x%02x src 0x%02x x%u wait %u us: %u ok, %u queue empty, %u not ours, %u read failed, %u unannounced\n",
		 mid, req, src, count, wait_us, ok, empty, other, bad, none);
	return 0;
}

/*
 * Send one SCP message given as a raw header word plus data words, as
 * recorded in the Windows capture. GET replies are read and discarded.
 */
int creative_dsp_scp_send(struct creative_chip *chip, u32 hdr,
			  const u32 *data, unsigned int count)
{
	unsigned int mid = hdr & 0xff, src = (hdr >> 8) & 0xff;
	unsigned int req = (hdr >> 17) & 0x7f;
	bool get = (hdr >> 16) & 1;
	u32 reply[SCP_MAX_DATA_WORDS];
	unsigned int rl = sizeof(reply);

	return dspio_scp(chip, mid, src, req, get ? SCP_GET : SCP_SET,
			 count ? data : NULL, count * sizeof(u32),
			 get ? reply : NULL, get ? &rl : NULL);
}

/* Raw ChipIO verb replays move the hardware address behind our cache. */
void creative_dsp_chipio_addr_invalidate(void)
{
	curr_chip_addx = ~0U;
}

static int scp_get_u32(struct creative_chip *chip, unsigned int mid,
		       unsigned int req, u32 *val)
{
	unsigned int rl = sizeof(*val);

	*val = ~0U;
	return dspio_scp(chip, mid, 0, req, SCP_GET, NULL, 0, val, &rl);
}

static int scp_set_u32(struct creative_chip *chip, unsigned int mid,
		       unsigned int req, u32 val)
{
	return dspio_scp(chip, mid, 0, req, SCP_SET, &val, sizeof(val),
			 NULL, NULL);
}

static bool seg_is_last(const struct ae9_dsp_image_seg *p);
static const struct ae9_dsp_image_seg *seg_next(const struct ae9_dsp_image_seg *p);

#define OVL_BUF_BYTES	0x8000		/* Windows: CBL 0x8000 */
#define OVL_BDL_ENTRIES	8		/* Windows: LVI 7 */
#define OVL_SD		0x100		/* stream descriptor 4, as Windows */
#define OVL_FMT		0x41		/* 2 ch, 32-bit, 48 kHz */

/* Program and start a DSP transfer stream (descriptor at BAR0 + sdoff,
 * stream tag) over the prepared buffer. */
static void xfer_stream_run(struct creative_chip *chip, unsigned int sdoff,
			    unsigned int tag, dma_addr_t bdl_dma)
{
	void __iomem *sd = chip->hda_regs + sdoff;

	writel(SD_CTL_STREAM_RESET, sd + AZX_REG_SD_CTL);
	udelay(50);
	writel(0, sd + AZX_REG_SD_CTL);
	udelay(50);
	writel(tag << 20, sd + AZX_REG_SD_CTL);
	writel(OVL_BUF_BYTES, sd + AZX_REG_SD_CBL);
	writew(OVL_BDL_ENTRIES - 1, sd + AZX_REG_SD_LVI);
	writew(OVL_FMT, sd + AZX_REG_SD_FORMAT);
	writel(lower_32_bits(bdl_dma), sd + AZX_REG_SD_BDLPL);
	writel(upper_32_bits(bdl_dma), sd + AZX_REG_SD_BDLPU);
	wmb();
	writel((tag << 20) | SD_CTL_DMA_START, sd + AZX_REG_SD_CTL);
	wmb();
}

/*
 * Stop a transfer stream the way the HDA spec asks: clear RUN and wait
 * until it reads 0 (the engine finishes its fetch), then reset and wait
 * for SRST to read back 1 and then 0, then clear the BDL pointer. The
 * old stop used fixed 50 us pauses with no read-back; after a headphone
 * profile upload the card then wrote to memory we had already freed
 * (IOMMU page faults, 2026-11-07 12:42:34 and 12:50:17) and the DSP died.
 */
static bool xfer_stream_stop(struct creative_chip *chip, unsigned int sdoff,
			     unsigned int tag)
{
	void __iomem *sd = chip->hda_regs + sdoff;
	unsigned int i;
	u32 ctl = 0;

	writel(tag << 20, sd + AZX_REG_SD_CTL);
	for (i = 0; i < 1000; i++) {
		ctl = readl(sd + AZX_REG_SD_CTL);
		if (!(ctl & SD_CTL_DMA_START))
			break;
		udelay(10);
	}
	if (ctl & SD_CTL_DMA_START) {
		/* still fetching: leave it alone; the caller must not free
		 * its buffers (2026-11-07 14:45:14: freed under a running
		 * stream, the card wrote into them, IOMMU faults, dead DSP) */
		dev_warn(chip->dev, "transfer stream 0x%x: RUN still set after 10 ms (ctl 0x%x)\n",
			 sdoff, ctl);
		return false;
	}
	writel(SD_CTL_STREAM_RESET, sd + AZX_REG_SD_CTL);
	for (i = 0; i < 100 && !(readl(sd + AZX_REG_SD_CTL) & SD_CTL_STREAM_RESET); i++)
		udelay(10);
	writel(0, sd + AZX_REG_SD_CTL);
	for (i = 0; i < 100 && (readl(sd + AZX_REG_SD_CTL) & SD_CTL_STREAM_RESET); i++)
		udelay(10);
	writel(0, sd + AZX_REG_SD_BDLPL);
	writel(0, sd + AZX_REG_SD_BDLPU);
	readl(sd + AZX_REG_SD_CTL);	/* flush the posted writes */
	return true;
}

static int dsp_overlay_xfer_once(struct creative_chip *chip, u32 *chan_out);

static int dsp_overlay_xfer(struct creative_chip *chip, u32 *chan_out)
{
	bool parked;
	int ret, u;

	if (chip->card_gone)
		return -ENODEV;
	parked = ae9_rings_park(chip, "overlay");
	ret = dsp_overlay_xfer_once(chip, chan_out);
	if (parked) {
		u = ae9_rings_unpark(chip, "overlay");
		if (u == -ENODEV)
			return u;
	}
	return ret;
}

static int dsp_overlay_xfer_once(struct creative_chip *chip, u32 *chan_out)
{
	const struct firmware *fw;
	const struct ae9_dsp_image_seg *seg;
	void *buf = NULL;
	__le32 *bdl = NULL;
	dma_addr_t buf_dma = 0, bdl_dma = 0;
	unsigned int port, i, segno = 0, rl;
	u32 chan = ~0U, resp;
	int ret;

	ret = ae9_request_fw(chip, &fw, AE9_OVERLAY_FILE, AE9_OVERLAY_SIZE);
	if (ret) {
		dev_warn(chip->dev, "overlay: /lib/firmware/%s not available (%d), skipped; extract it with extract_overlay.py\n",
			 AE9_OVERLAY_FILE, ret);
		return ret;
	}
	if (fls_image_check(chip, fw, AE9_OVERLAY_FILE)) {
		release_firmware(fw);
		return -EINVAL;
	}
	buf = dma_alloc_coherent(chip->dev, ae9_dma_size(chip, OVL_BUF_BYTES), &buf_dma, GFP_KERNEL);
	bdl = dma_alloc_coherent(chip->dev, ae9_dma_size(chip, 16 * OVL_BDL_ENTRIES), &bdl_dma, GFP_KERNEL);
	if (!buf || !bdl) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < OVL_BDL_ENTRIES; i++) {
		dma_addr_t a = buf_dma + (dma_addr_t)i * (OVL_BUF_BYTES / OVL_BDL_ENTRIES);

		bdl[i * 4 + 0] = cpu_to_le32(lower_32_bits(a));
		bdl[i * 4 + 1] = cpu_to_le32(upper_32_bits(a));
		bdl[i * 4 + 2] = cpu_to_le32(OVL_BUF_BYTES / OVL_BDL_ENTRIES);
		bdl[i * 4 + 3] = 0;
	}

	/* ChipIO converter on tag 1, DMA channel, port (cold capture 4468-4505) */
	dsp_write(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_STREAM_FORMAT, OVL_FMT);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x706, 0x10);
	rl = sizeof(chan);
	ret = dspio_scp(chip, MASTERCONTROL, 0, 0x0a, SCP_GET, NULL, 0, &chan, &rl);
	dev_info(chip->dev, "overlay: alloc DMA chan ret=%d chan=0x%x\n", ret, chan);
	if (ret)
		goto unbind;
	dsp_write(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PORT_ALLOC_CONFIG_SET, 0x01);
	dsp_write(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PORT_ALLOC_SET, 0x10);
	port = dsp_read(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PORT_ALLOC_GET, 0);
	/*
	 * 0xff now and then (2026-11-04: on both boots whose DSP mode came up
	 * with a strange router, never on the clean ones): the 8051 hadn't
	 * answered yet. 0 too (2026-11-04, 7.2.15: port 0 taken, the overlay
	 * never landed, DMA took 30-44 polls instead of 1). Every good boot
	 * and Windows read 3. Give it a moment and ask again before guessing.
	 */
	for (i = 0; (port == 0 || port > 0xf) && i < 10; i++) {
		msleep(5);
		port = dsp_read(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PORT_ALLOC_GET, 0);
	}
	dev_info(chip->dev, "overlay: port read 0x%x (Windows: 3)%s\n", port,
		 i ? ((port == 0 || port > 0xf) ? ", still bad after retries" :
		      ", after a retry") : "");
	if (port == 0 || port > 0xf) {
		/* read came back as 0xff000000 once; Windows always got 3 */
		port = 3;
		dev_info(chip->dev, "overlay: implausible port, using 3\n");
	}

	for (seg = (const void *)fw->data;
	     (const u8 *)seg + sizeof(*seg) <= fw->data + fw->size && !seg_is_last(seg);
	     seg = seg_next(seg), segno++) {
		bool code = UC_RANGE(seg->chip_addr, 1);
		unsigned int rem = seg->count % 2;	/* 2 words per frame */
		unsigned int n = seg->count - rem;
		u32 a3a[2] = { port, code ? 0xffff8a00 : 0 };
		u32 a3b[2] = { (n << 16) | (((seg->chip_addr / 4) + rem) & 0xffff),
			       code ? 0x80000000 : 0 };
		u32 r = 0, v;
		unsigned int t;

		if (n * 4 > OVL_BUF_BYTES) {
			ret = -EINVAL;
			break;
		}
		memset(buf, 0, OVL_BUF_BYTES);
		memcpy(buf, &seg->data[rem], n * 4);
		wmb();

		/*
		 * GET 0x3a and 0x3b arm the DSP's DMA (afterwards its DMA
		 * registers read exactly as in Windows, channel started); then
		 * run the stream until the channel finishes. Running it before
		 * 0x3b left the channel waiting for data that had already gone.
		 */
		rl = sizeof(r);
		ret = dspio_scp(chip, MASTERCONTROL, 0, 0x3a, SCP_GET, a3a,
				sizeof(a3a), &r, &rl);
		rl = sizeof(r);
		ret |= dspio_scp(chip, MASTERCONTROL, 0, 0x3b, SCP_GET, a3b,
				 sizeof(a3b), &r, &rl);
		xfer_stream_run(chip, OVL_SD, 1, bdl_dma);
		for (t = 0; t < 200; t++) {
			creative_dsp_chipio_read(chip, DSPDMAC_CHNLSTART_INST_OFFSET, &v);
			if (!(v & 0xfff))
				break;
			usleep_range(1000, 2000);
		}
		xfer_stream_stop(chip, OVL_SD, 1);
		/* leftover word(s) go in directly (Windows: 0x03f3a0 = 0x00800082) */
		for (i = 0; i < rem; i++)
			creative_dsp_chipio_write(chip, seg->chip_addr + 4 * i,
						  seg->data[i]);
		{
			unsigned int act = 0, f00 = 0, f04 = 0, f08 = 0;

			creative_dsp_chipio_read(chip, 0x110ff8, &act);
			creative_dsp_chipio_read(chip, 0x110f00, &f00);
			creative_dsp_chipio_read(chip, 0x110f04, &f04);
			creative_dsp_chipio_read(chip, 0x110f08, &f08);
			dev_info(chip->dev,
				 "overlay: seg%u chip 0x%06x %u words: 0x3a [%x %08x] 0x3b [%08x %08x] ret=%d, %s after %u polls (0x110ff0=0x%x ff8=0x%x f00=0x%x f04=0x%x f08=0x%x)\n",
				 segno, seg->chip_addr, seg->count, a3a[0], a3a[1],
				 a3b[0], a3b[1], ret, (v & 0xfff) ? "DMA NOT done" : "DMA done",
				 t, v, act, f00, f04, f08);
		}
	}

	dsp_write(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PORT_FREE_SET, 0x10);
unbind:
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x706, 0x00);
	creative_dsp_chipio_addr_invalidate();
	*chan_out = chan;
out:
	if (buf)
		dma_free_coherent(chip->dev, ae9_dma_size(chip, OVL_BUF_BYTES), buf, buf_dma);
	if (bdl)
		dma_free_coherent(chip->dev, ae9_dma_size(chip, 16 * OVL_BDL_ENTRIES), bdl, bdl_dma);
	release_firmware(fw);
	(void)resp;
	return ret;
}

/*
 * Headphone profile (Sound Blaster Command's headphone list): a speaker EQ
 * image for the DSP's Y memory, uploaded the way Windows does it at run
 * time (capture 2026-11-07, 00:58:52; notes 12/13): "use speaker EQ" off,
 * GET 0x80/0x3c = where the speaker EQ lives (Y word address; 0x1d5e with
 * our firmware), a converter on stream tag 2 (tag 1 is the playback),
 * a DMA channel (GET 0x0a), a port, GET 0x3a/0x3b to arm the DSP's DMA,
 * the data over stream descriptor 5, then port, converter and channel
 * free (SET 0x0a = channel). The image is Creative's (extracted from its
 * installer): one FLS segment of 1024 words at chip address 0x40000 (the
 * start of Y memory). The second words of 0x3a/0x3b carry OUR channel
 * (Windows' builders, decompile FUN_00028b5c/FUN_00028cc0: 0x3a word 1 =
 * code << 31 | chan, 0x3b word 1 = code << 31 | Y memory << 30 | chan;
 * bits 4-29 there are uninitialised stack, hence Windows' 0x7fff9482).
 * Copying Windows' 0x40000002 armed channel 2, one of the playback's,
 * while ours was 4: all audio stalled until a power-off (2026-11-07).
 * The caller turns "use speaker EQ" on after.
 * Caller holds replay_mutex. The caller waits for the streams to stop
 * first (hp_live_upload=0, fx.c); with it on, a playback may be running
 * (its DSP DMA channels keep going, so only our channel's bit is waited on).
 *
 * Returns 0 when the image is in Y memory (read back), -EREMOTEIO when it
 * failed after GET 0x3a/0x3b armed (or may have armed) the DSP's DMA: the
 * DSP's state is unknown then, so the caller stops uploading until the
 * next bring-up. Any other error: nothing was armed, a retry is safe.
 */
#define HPEQ_SD		0x120	/* stream descriptor 5, as Windows */
#define HPEQ_TAG	2
#define HPEQ_WORDS	1024
#define HPEQ_3B_YMEM	BIT(30)	/* 0x3b word 1: the target is Y memory */

static int dsp_dma_stop(struct creative_chip *chip, unsigned int dma_chan);

/* Every stream descriptor with RUN set (bit n = SD n; GCAP 0x6400: 4 in,
 * 6 out). Our own transfer streams count too, but none runs between uploads. */
u32 creative_ae9_streams_running(struct creative_chip *chip)
{
	u32 mask = 0;
	unsigned int n;

	if (!chip->hda_regs || chip->card_gone)
		return 0;
	for (n = 0; n < 10; n++)
		if (readl(chip->hda_regs + 0x80 + n * 0x20 + AZX_REG_SD_CTL) &
		    SD_CTL_DMA_START)
			mask |= BIT(n);
	return mask;
}

/*
 * After a failed upload that got as far as GET 0x3a/0x3b: the channel
 * may sit armed on our port. Freeing it like that left an armed DSP DMA
 * channel behind (note 54; Kaiba 2026-11-08: the next uploads didn't land
 * and the DSP's output stayed chopped until a power-off). Stop it first.
 */
static bool hpeq_chnlstart(struct creative_chip *chip, u32 *v)
{
	unsigned int k;

	for (k = 0; k < 2; k++) {
		*v = ~0U;
		if (creative_dsp_chipio_read(chip, DSPDMAC_CHNLSTART_INST_OFFSET, v) >= 0 &&
		    !(*v & ~0xfffU))
			return true;	/* a valid read: bits 0-11 only */
	}
	return false;
}

/* Returns true when the channel is off (safe to free with its port), false
 * when it may still be armed: then both stay allocated until the next
 * bring-up (note 54: a leak beats a live channel on a reused port). */
static bool hpeq_disarm(struct creative_chip *chip, unsigned int chan)
{
	u32 v, after;

	if (!hpeq_chnlstart(chip, &v)) {
		dev_warn(chip->dev, "headphone profile: DSP channel %u state unreadable; keeping it allocated\n",
			 chan);
		return false;
	}
	if (!(v & BIT(chan)))
		return true;
	dsp_dma_stop(chip, chan);
	usleep_range(1000, 1500);
	if (!hpeq_chnlstart(chip, &after) || (after & BIT(chan))) {
		dev_warn(chip->dev, "headphone profile: DSP DMA channel %u won't stop (CHNLSTART 0x%x -> 0x%x); keeping it allocated\n",
			 chan, v, after);
		return false;
	}
	dev_warn(chip->dev, "headphone profile: stopped DSP DMA channel %u (CHNLSTART 0x%x -> 0x%x)\n",
		 chan, v, after);
	return true;
}

/*
 * The port answer is a bit mask of the ports handed out. Every upload
 * that read back right got two neighbouring ports: 0x300 (Joe), 0x30
 * (Kaiba), 0x3 (the boot overlay, Windows too). Both single-port
 * answers (0x10, 0x1; Kaiba 2026-11-08) never landed.
 */
static bool hpeq_port_ok(unsigned int port)
{
	return port && port != ~0U && hweight32(port) == 2 && (port & (port >> 1));
}

/* Words checked after an upload. Word 0 was left out because Apple
 * Earphone's read 0 on two cards; that was the cut-short transfer (note
 * 60): with the fixed end check it reads right (2026-11-08), so it's back. */
static const u16 hpeq_check_words[] = { 0, 1, 147, 293, 439, 585, 731, 877, 1023 };

int creative_dsp_hpeq_upload(struct creative_chip *chip, const char *name)
{
	const struct firmware *fw;
	const struct ae9_dsp_image_seg *seg;
	void *buf = NULL;
	__le32 *bdl = NULL;
	dma_addr_t buf_dma = 0, bdl_dma = 0;
	u32 addr = ~0U, chan = ~0U, r, v = 0, v0 = 0, a3a[2], a3b[2], zero = 0, busy = ~0U;
	bool stopped = true;
	unsigned int to0, bad = 0;
	u32 yfirst = 0, ylast = 0, port2 = ~0U;
	unsigned int port, rl, i, t, seen, match = 0, unread = 0;
	bool rb_bad = false, leak = false;
	u32 lpib = 0, matched = 0;
	int ret;

	if (!chip->dsp_done || chip->card_gone)
		return -ENODEV;
	to0 = chip->corb_timeouts;
	ret = request_firmware(&fw, name, chip->dev);
	if (ret) {
		dev_warn(chip->dev, "headphone profile: /lib/firmware/%s not available (%d); extract the profiles with ae9-get-firmware\n",
			 name, ret);
		return ret;
	}
	seg = (const void *)fw->data;
	if (fls_image_check(chip, fw, name) || seg->count != HPEQ_WORDS ||
	    seg->chip_addr != 0x40000) {
		dev_err(chip->dev, "headphone profile %s: unexpected image (chip 0x%x, %u words)\n",
			name, fw->size >= sizeof(*seg) ? seg->chip_addr : 0,
			fw->size >= sizeof(*seg) ? seg->count : 0);
		ret = -EINVAL;
		goto out;
	}
	buf = dma_alloc_coherent(chip->dev, ae9_dma_size(chip, OVL_BUF_BYTES), &buf_dma, GFP_KERNEL);
	bdl = dma_alloc_coherent(chip->dev, ae9_dma_size(chip, 16 * OVL_BDL_ENTRIES), &bdl_dma, GFP_KERNEL);
	if (!buf || !bdl) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < OVL_BDL_ENTRIES; i++) {
		dma_addr_t a = buf_dma + (dma_addr_t)i * (OVL_BUF_BYTES / OVL_BDL_ENTRIES);

		bdl[i * 4 + 0] = cpu_to_le32(lower_32_bits(a));
		bdl[i * 4 + 1] = cpu_to_le32(upper_32_bits(a));
		bdl[i * 4 + 2] = cpu_to_le32(OVL_BUF_BYTES / OVL_BDL_ENTRIES);
		bdl[i * 4 + 3] = 0;
	}
	memset(buf, 0, OVL_BUF_BYTES);
	memcpy(buf, seg->data, HPEQ_WORDS * 4);
	wmb();

	/* use speaker EQ: off, from src 0x20 as Windows (all 4 uploads) */
	dspio_scp(chip, 0x96, 0x20, 0x1f, SCP_SET, &zero, sizeof(zero), NULL, NULL);
	/*
	 * Ask twice; both answers must agree and be a real address. Once the
	 * DSP answered 0 with a correct header (2026-11-07, 12:29:41; every
	 * good answer before was 0x1d5e): 1024 words went over the start of
	 * Y memory and the DSP stopped answering until a power-off.
	 */
	ret = scp_get_u32(chip, MASTERCONTROL, 0x3c, &addr);
	if (!ret) {
		u32 again = ~0U;

		ret = scp_get_u32(chip, MASTERCONTROL, 0x3c, &again);
		if (!ret && again != addr) {
			dev_err(chip->dev, "headphone profile: speaker EQ address answers differ (0x%x, 0x%x)\n",
				addr, again);
			ret = -EIO;
		}
	}
	if (ret || addr == 0 || addr > 0x10000 - HPEQ_WORDS) {
		dev_err(chip->dev, "headphone profile: speaker EQ address query failed (ret %d, 0x%x); not uploading\n",
			ret, addr);
		ret = ret ?: -EIO;
		goto out;
	}
	dsp_write(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_STREAM_FORMAT, OVL_FMT);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x706, HPEQ_TAG << 4);
	/*
	 * The DSP channels already running (the playback's). Once GET 0x0a
	 * "gave" channel 0, the playback's own, because its data word read
	 * as 0 (2026-11-07 14:45:14): the profile went into the playback's
	 * channel and the DSP died. A data word of 0 can't be caught by its
	 * value, so check it against what the card says is busy.
	 */
	if (!hpeq_chnlstart(chip, &busy)) {
		/* can't tell which channels are the playback's (review B9) */
		dev_err(chip->dev, "headphone profile: DSP channel state unreadable (0x%x); not uploading\n",
			busy);
		ret = -EIO;
		goto unbind;
	}
	ret = scp_get_u32(chip, MASTERCONTROL, 0x0a, &chan);
	if (!ret && chan <= 7 && (busy & BIT(chan))) {
		/* not ours to free: whatever we really got stays allocated
		 * until the next bring-up (a leak beats freeing the playback's) */
		chip->hp_leaks++;
		dev_err(chip->dev, "headphone profile: DSP gave channel %u, which is busy (0x%x); not uploading (%u channel(s) lost so far)\n",
			chan, busy, chip->hp_leaks);
		ret = -EBUSY;
		goto unbind;
	}
	if (ret || chan > 7) {
		/* the DSP may have handed one out that we never saw: it stays
		 * allocated until the next bring-up (the DSP has 8) */
		chip->hp_leaks++;
		dev_err(chip->dev, "headphone profile: no DSP DMA channel (ret %d, 0x%x; %u channel(s) lost so far)\n",
			ret, chan, chip->hp_leaks);
		ret = ret ?: -EBUSY;
		goto unbind;
	}
	dsp_write(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PORT_ALLOC_CONFIG_SET, 0x01);
	dsp_write(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PORT_ALLOC_SET, 0x10);
	port = dsp_read(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PORT_ALLOC_GET, 0);
	for (i = 0; (port == 0 || port == ~0U) && i < 10; i++) {
		msleep(5);
		port = dsp_read(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PORT_ALLOC_GET, 0);
	}
	/* twice, and the two must agree: one bad read here sends the DMA to
	 * the wrong port */
	if (hpeq_port_ok(port))
		port2 = dsp_read(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PORT_ALLOC_GET, 0);
	if (!hpeq_port_ok(port) || port2 != port) {
		dev_err(chip->dev, "headphone profile: ChipIO port 0x%x (again 0x%x) isn't a port pair; not uploading\n",
			port, port2);
		dsp_write(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PORT_FREE_SET, 0x10);
		ret = -EBUSY;
		goto free_chan;
	}
	a3a[0] = port;
	a3a[1] = chan & 0xf;
	a3b[0] = (HPEQ_WORDS << 16) | (addr & 0xffff);
	a3b[1] = HPEQ_3B_YMEM | (chan & 0xf);
	/*
	 * GET 0x3a and 0x3b arm the DSP's DMA. Both must be answered before
	 * the data goes out. The first builds sent anyway (an empty 0x3a
	 * header left later GETs one reply behind, which the busy-aware reader now
	 * handles), but with the 0x3b reply lost while the command ring was
	 * choking, sending anyway ended in the card's DMA reading unmapped
	 * memory (IOMMU page fault) and a dead DSP (2026-11-07, 12:42:34).
	 */
	rl = sizeof(r);
	ret = dspio_scp(chip, MASTERCONTROL, 0, 0x3a, SCP_GET, a3a, sizeof(a3a), &r, &rl);
	if (!ret) {
		rl = sizeof(r);
		ret = dspio_scp(chip, MASTERCONTROL, 0, 0x3b, SCP_GET, a3b, sizeof(a3b), &r, &rl);
	}
	if (ret) {
		/* the DSP may have taken 0x3a and/or 0x3b anyway (note 54:
		 * it did, only the reply was lost) */
		dev_warn(chip->dev, "headphone profile: the DSP didn't confirm the transfer (0x3a/0x3b, %d; chan %u, port 0x%x); not sending\n",
			 ret, chan, port);
		leak = !hpeq_disarm(chip, chan);
		ret = -EREMOTEIO;
		goto free_port;
	}
	/*
	 * Windows reads the channel's DMA state back before RUN, and the
	 * channel's start bit was already on in all 4 of its uploads (0x3,
	 * 0xf, 0x3, 0x7; notes 20/25). If it isn't, the DSP didn't take the
	 * 0x3a/0x3b setup: don't start a transfer into nothing.
	 */
	if (!hpeq_chnlstart(chip, &v) || !(v & BIT(chan))) {
		dev_warn(chip->dev, "headphone profile: DSP channel %u not armed (0x%x, port 0x%x); not sending\n",
			 chan, v, port);
		leak = !hpeq_disarm(chip, chan);
		ret = -EREMOTEIO;
		goto free_port;
	}
	xfer_stream_run(chip, HPEQ_SD, HPEQ_TAG, bdl_dma);
	/*
	 * Wait for our channel's start bit to come on, then to go off. 4 KB
	 * at the stream's rate takes ~11 ms; Windows saw the bit on at its
	 * first read and off 13 ms later (0x7 -> 0x3). The first build
	 * stopped as soon as the bit read off, i.e. before the DSP had taken
	 * the channel: the transfer was cut and the DSP's DMA stalled (all
	 * audio silent until a power-off, 2026-11-07).
	 */
	/*
	 * Only VALID reads count (the register uses bits 0-11; 0x9000f
	 * after a verb timeout was taken as "done" after 2 polls, the
	 * stream was stopped mid-transfer and a half-written EQ went on:
	 * right channel silent, left metallic, 2026-11-07 14:51:11).
	 * Windows always saw the bit on for 5-9 polls (~12 ms).
	 */
	/*
	 * A read only counts if the channels busy before the upload are still
	 * set: a glitched read of 0 (they happen, note 58) took "our bit off"
	 * for the end of the transfer and the stream was stopped after ~4 ms
	 * of the ~11 the 4 KB need (Kaiba: HD 280 Pro 3 of 3, Joe: P5,
	 * ATH-M50; note 60). Done = our bit off in 2 valid reads in a row,
	 * at least 11 ms after RUN, and the stream past the whole buffer.
	 */
	seen = 0;
	{
		u32 others = busy & ~BIT(chan);
		ktime_t t_run = ktime_get();
		unsigned int off = 0;

		for (t = 0; t < 300; t++) {
			if (creative_dsp_chipio_read(chip, DSPDMAC_CHNLSTART_INST_OFFSET, &v) < 0 ||
			    (v & ~0xfffU) || (v & others) != others) {
				bad++;
				usleep_range(500, 1000);
				continue;
			}
			if (t == 0)
				v0 = v;
			if (v & BIT(chan)) {
				seen++;
				off = 0;
			} else if (seen >= 3 && ++off >= 2 &&
				   ktime_ms_delta(ktime_get(), t_run) >= 11 &&
				   readl(chip->hda_regs + HPEQ_SD + AZX_REG_SD_LPIB) >= HPEQ_WORDS * 4) {
				break;
			}
			usleep_range(500, 1000);
		}
		lpib = readl(chip->hda_regs + HPEQ_SD + AZX_REG_SD_LPIB);
	}
	stopped = xfer_stream_stop(chip, HPEQ_SD, HPEQ_TAG);
	if (seen < 3 || (v & BIT(chan)) || (v & ~0xfffU))
		ret = -ETIMEDOUT;
	/* any verb timeout during the upload: don't trust it, keep the EQ off */
	if (!ret && chip->corb_timeouts != to0) {
		dev_warn(chip->dev, "headphone profile: %u verb timeout(s) during the upload; not using it\n",
			 chip->corb_timeouts - to0);
		ret = -EIO;
	}
	if (ret)
		leak = !hpeq_disarm(chip, chan);
	/*
	 * Read words back from Y memory (chip address = Y base + 4 * word).
	 * "done" with the wrong data in place happened 5 times in one session
	 * (Kaiba, 2026-11-08: all 0, the previous profile's last word, a
	 * single-port upload) and the EQ went on anyway. Every checked word
	 * must match; a word that won't read is tried once more.
	 */
	if (!ret) {
		creative_dsp_chipio_read(chip, 0x40000 + 4 * addr, &yfirst);
		for (i = 0; i < ARRAY_SIZE(hpeq_check_words); i++) {
			unsigned int w = hpeq_check_words[i], k;
			u32 want = le32_to_cpu(((__le32 *)seg->data)[w]), got = ~want;
			bool ok = false, wrong = false;

			/* two tries; one good read that differs = wrong (B5) */
			for (k = 0; k < 2 && !ok; k++) {
				got = ~want;
				if (creative_dsp_chipio_read(chip, 0x40000 + 4 * (addr + w), &got) < 0)
					continue;
				if (got == want)
					ok = true;
				else
					wrong = true;
			}
			if (ok) {
				match++;
				matched |= BIT(i);
			}
			else if (!wrong)
				unread++;
			if (w == HPEQ_WORDS - 1)
				ylast = got;
		}
		dev_info(chip->dev, "headphone profile: Y read-back %u/%zu words match%s (first 0x%08x, image 0x%08x; last 0x%08x, image 0x%08x)%s\n",
			 match, ARRAY_SIZE(hpeq_check_words),
			 unread ? " (some unreadable)" : "",
			 yfirst, le32_to_cpu(((__le32 *)seg->data)[0]),
			 ylast, le32_to_cpu(((__le32 *)seg->data)[HPEQ_WORDS - 1]),
			 bad ? " (some status reads were bad)" : "");
		rb_bad = match != ARRAY_SIZE(hpeq_check_words);
		if (rb_bad) {
			/* a cut transfer can leave the channel waiting for the
			 * rest: stop it before it's freed (note 60) */
			leak = !hpeq_disarm(chip, chan);
			dev_warn(chip->dev, "headphone profile: words matched 0x%03x (bit n = check word n), stream at %u of %u bytes\n",
				 matched, lpib, HPEQ_WORDS * 4);
		}
		if (rb_bad)
			ret = unread == ARRAY_SIZE(hpeq_check_words) - match ?
			      -EIO : -EREMOTEIO;
	} else {
		ret = -EREMOTEIO;
	}
	dev_info(chip->dev, "headphone profile %s: %u words to Y 0x%x, chan %u, port 0x%x, %s after %u polls (first 0x%x, last 0x%x, channel on for %u)\n",
		 name, HPEQ_WORDS, addr, chan, port,
		 !ret ? "done" : rb_bad ? "FAILED (read-back differs)" :
		 seen ? "FAILED (still running)" : "FAILED (channel never started)",
		 t, v0, v, seen);
free_port:
	if (!leak)
		dsp_write(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PORT_FREE_SET, 0x10);
free_chan:
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x706, 0x00);
	if (!leak) {
		scp_set_u32(chip, MASTERCONTROL, 0x0a, chan);	/* free the channel */
	} else {
		chip->hp_leaks++;
		dev_warn(chip->dev, "headphone profile: DSP channel %u and port 0x%x left allocated (%u lost so far)\n",
			 chan, port, chip->hp_leaks);
	}
	goto inval;
unbind:
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x706, 0x00);
inval:
	creative_dsp_chipio_addr_invalidate();
out:
	if (!stopped) {
		/* the card may still read/write them: never free (small leak) */
		dev_err(chip->dev, "headphone profile: transfer stream didn't stop; keeping its buffers\n");
		/* armed and possibly still moving: not a "retry is safe" case (B3) */
		ret = -EREMOTEIO;
	} else {
		if (buf)
			dma_free_coherent(chip->dev, ae9_dma_size(chip, OVL_BUF_BYTES), buf, buf_dma);
		if (bdl)
			dma_free_coherent(chip->dev, ae9_dma_size(chip, 16 * OVL_BDL_ENTRIES), bdl, bdl_dma);
	}
	release_firmware(fw);
	return ret;
}

/*
 * Did the overlay land? Word 5 of its jump table at 0x3f3a0 reads
 * 0x04000086 once it has (see creative_ae9_dsp_health). ChipIO reads can
 * come back wrong now and then, so two reads must agree; a few tries.
 */
#define OVL_CHECK_ADDR	0x3f3b4
#define OVL_CHECK_WANT	0x04000086

static bool dsp_overlay_landed(struct creative_chip *chip, u32 *got)
{
	u32 a, b = 0;
	unsigned int i;

	creative_dsp_chipio_addr_invalidate();
	for (i = 0; i < 4; i++) {
		if (creative_dsp_chipio_read(chip, OVL_CHECK_ADDR, &a) < 0 ||
		    creative_dsp_chipio_read(chip, OVL_CHECK_ADDR, &b) < 0)
			continue;
		if (a == b)
			break;
	}
	creative_dsp_chipio_addr_invalidate();
	*got = b;
	return b == OVL_CHECK_WANT;
}

/*
 * MASTERCONTROL boot handshake, run by Windows straight after starting the
 * freshly downloaded firmware (capture seq 4576-5163, see
 * WINDOWS-CAPTURE-ANALYSIS.md). The overlay transfer Windows does between
 * the DMA channel alloc and free (GET 0x3a/0x3b) is not reproduced: its
 * payload isn't in the capture.
 */
int creative_dsp_boot_handshake(struct creative_chip *chip)
{
	static const struct { u8 req; u32 val; } states[] = {
		{ 0x0c, 3 }, { 0x0c, 4 }, { 0x0c, 5 }, { 0x0c, 0 },
	};
	unsigned int i, tries;
	int ret, err = 0;
	u32 v, chan;

	for (i = 0; i < ARRAY_SIZE(states); i++) {
		if (chip->card_gone)
			return -ENODEV;
		if (i == 1) {
			/* Between states 3 and 4: DMA channel alloc, overlay
			 * transfer (overlay=1), channel free, then 0x0b = 1,
			 * as Windows does. */
			ret = -ENOENT;
			chan = ~0U;
			bool landed = false;

			if (overlay) {
				u32 chk = 0;

				ret = dsp_overlay_xfer(chip, &chan);
				landed = dsp_overlay_landed(chip, &chk);
				if (!landed) {
					/* free the channel, then go again */
					dev_warn(chip->dev,
						 "overlay: didn't land (ret=%d, check word 0x%08x), sending it again\n",
						 ret, chk);
					if (chan != ~0U)
						scp_set_u32(chip, MASTERCONTROL, 0x0a, chan);
					chan = ~0U;
					ret = dsp_overlay_xfer(chip, &chan);
					landed = dsp_overlay_landed(chip, &chk);
					dev_info(chip->dev, "overlay: second try ret=%d, %s (check word 0x%08x)\n",
						 ret, landed ? "landed" : "STILL NOT landed", chk);
				}
			}
			/* an SCP error with the data in place still counts */
			chip->overlay_sent = ret == 0 || landed;
			if (ret && chan == ~0U) {
				ret = scp_get_u32(chip, MASTERCONTROL, 0x0a, &chan);
				dev_info(chip->dev,
					 "boot handshake: alloc DMA chan ret=%d chan=0x%x\n",
					 ret, chan);
			}
			if (chan != ~0U)
				scp_set_u32(chip, MASTERCONTROL, 0x0a, chan);
			ret = scp_set_u32(chip, MASTERCONTROL, 0x0b, 1);
			dev_info(chip->dev, "boot handshake: 0x0b=1 ret=%d\n", ret);
		}

		ret = scp_set_u32(chip, MASTERCONTROL, states[i].req,
				  states[i].val);
		v = ~0U;
		/* Each unanswered GET already waits up to 1s in dspio_scp(). */
		for (tries = 0; tries < 10; tries++) {
			if (scp_get_u32(chip, MASTERCONTROL, states[i].req,
					&v) == 0 && v == states[i].val)
				break;
			/* Windows: the first GET reads 1, the next ~110 ms
			 * later reads 3 (15 of 15 captures) */
			msleep(i == 0 ? 105 : 20);
		}
		dev_info(chip->dev,
			 "boot handshake: 0x%02x=%u set ret=%d, DSP reports 0x%x after %u polls%s\n",
			 states[i].req, states[i].val, ret, v, tries,
			 v == states[i].val ? "" : " (NO MATCH)");
		if (v != states[i].val) {
			err = -ETIMEDOUT;
			/* resume retries the whole bring-up instead of
			 * sitting out every remaining timeout */
			if (chip->resuming && i == 0)
				return err;
		}
	}
	return err;
}

/* ------------------------------------------------------------------ */
/* DSP DMA register layer (all via ChipIO)				  */
/* ------------------------------------------------------------------ */
static unsigned int dsp_chip_to_dsp_addx(unsigned int chip_addx,
					 bool *code, bool *yram)
{
	*code = *yram = false;
	if (UC_RANGE(chip_addx, 1)) {
		*code = true;
		return UC_OFF(chip_addx);
	} else if (X_RANGE_ALL(chip_addx, 1)) {
		return X_OFF(chip_addx);
	} else if (Y_RANGE_ALL(chip_addx, 1)) {
		*yram = true;
		return Y_OFF(chip_addx);
	}
	return INVALID_CHIP_ADDX;
}

static bool dsp_is_dma_active(struct creative_chip *chip, unsigned int dma_chan)
{
	unsigned int reg = 0;

	creative_dsp_chipio_read(chip, DSPDMAC_CHNLSTART_INST_OFFSET, &reg);
	return (reg & (1 << (DSPDMAC_CHNLSTART_EN_LOBIT + dma_chan))) != 0;
}

/* Read the channel's active bit again 1 ms later, to confirm a first read. */
static bool dsp_dma_active_again(struct creative_chip *chip,
				 unsigned int dma_chan)
{
	usleep_range(1000, 1500);
	return dsp_is_dma_active(chip, dma_chan);
}

static int dsp_dma_setup_common(struct creative_chip *chip,
				unsigned int chip_addx, unsigned int dma_chan,
				unsigned int port_map_mask)
{
	unsigned int chnl_prop, active, dsp_addx;
	bool code, yram;
	int status;

	if (dma_chan >= DSPDMAC_DMA_CFG_CHANNEL_COUNT)
		return -EINVAL;
	if (dsp_is_dma_active(chip, dma_chan))
		return -EBUSY;
	dsp_addx = dsp_chip_to_dsp_addx(chip_addx, &code, &yram);
	if (dsp_addx == INVALID_CHIP_ADDX)
		return -ENXIO;

	chnl_prop = DSPDMAC_CHNLPROP_AC_MASK;
	active = 0;
	if (!code)
		chnl_prop &= ~(1 << (DSPDMAC_CHNLPROP_MSPCE_LOBIT + dma_chan));
	else
		chnl_prop |=  (1 << (DSPDMAC_CHNLPROP_MSPCE_LOBIT + dma_chan));
	chnl_prop &= ~(1 << (DSPDMAC_CHNLPROP_DCON_LOBIT + dma_chan));
	status = creative_dsp_chipio_write(chip, DSPDMAC_CHNLPROP_INST_OFFSET,
					   chnl_prop);
	if (status < 0)
		return status;

	active &= (~(1 << (DSPDMAC_ACTIVE_AAR_LOBIT + dma_chan))) &
		  DSPDMAC_ACTIVE_AAR_MASK;
	status = creative_dsp_chipio_write(chip, DSPDMAC_ACTIVE_INST_OFFSET,
					   active);
	if (status < 0)
		return status;
	status = creative_dsp_chipio_write(chip,
			DSPDMAC_AUDCHSEL_INST_OFFSET(dma_chan), port_map_mask);
	if (status < 0)
		return status;
	return creative_dsp_chipio_write(chip,
			DSPDMAC_IRQCNT_INST_OFFSET(dma_chan),
			DSPDMAC_IRQCNT_BICNT_MASK | DSPDMAC_IRQCNT_CICNT_MASK);
}

static int dsp_dma_setup(struct creative_chip *chip, unsigned int chip_addx,
			 unsigned int count, unsigned int dma_chan)
{
	bool code, yram;
	unsigned int dsp_addx, addr_field, incr_field = 0, dma_cfg, adr_ofs;
	unsigned int base_cnt, cur_cnt, xfr_cnt;
	const unsigned int max_dma_count = 1 <<
		(DSPDMAC_XFRCNT_BCNT_HIBIT - DSPDMAC_XFRCNT_BCNT_LOBIT + 1);
	int status;

	if (count > max_dma_count)
		return -EINVAL;
	dsp_addx = dsp_chip_to_dsp_addx(chip_addx, &code, &yram);
	if (dsp_addx == INVALID_CHIP_ADDX)
		return -ENXIO;

	addr_field = dsp_addx << DSPDMAC_DMACFG_DBADR_LOBIT;
	if (!code) {
		addr_field <<= 1;
		if (yram)
			addr_field |= (1 << DSPDMAC_DMACFG_DBADR_LOBIT);
		incr_field = (1 << DSPDMAC_DMACFG_AINCR_LOBIT);
	}
	dma_cfg = addr_field + incr_field;
	status = creative_dsp_chipio_write(chip,
			DSPDMAC_DMACFG_INST_OFFSET(dma_chan), dma_cfg);
	if (status < 0)
		return status;

	adr_ofs = (count - 1) << (DSPDMAC_DSPADROFS_BOFS_LOBIT + (code ? 0 : 1));
	status = creative_dsp_chipio_write(chip,
			DSPDMAC_DSPADROFS_INST_OFFSET(dma_chan), adr_ofs);
	if (status < 0)
		return status;

	base_cnt = (count - 1) << DSPDMAC_XFRCNT_BCNT_LOBIT;
	cur_cnt  = (count - 1) << DSPDMAC_XFRCNT_CCNT_LOBIT;
	xfr_cnt = base_cnt | cur_cnt;
	return creative_dsp_chipio_write(chip,
			DSPDMAC_XFRCNT_INST_OFFSET(dma_chan), xfr_cnt);
}

static int dsp_dma_start(struct creative_chip *chip, unsigned int dma_chan)
{
	return creative_dsp_chipio_write(chip, DSPDMAC_CHNLSTART_INST_OFFSET,
			1 << (dma_chan + DSPDMAC_CHNLSTART_EN_LOBIT));
}

static int dsp_dma_stop(struct creative_chip *chip, unsigned int dma_chan)
{
	return creative_dsp_chipio_write(chip, DSPDMAC_CHNLSTART_INST_OFFSET,
			1 << (dma_chan + DSPDMAC_CHNLSTART_DIS_LOBIT));
}

/*
 * DBGCNTL read for a write-back: three reads must agree on the control
 * bits (EXEC/SS/STATE, the low 16; the top half is a moving counter).
 * ChipIO reads glitch while audio plays (0.3-2.7% on two cards, 2026-11-08:
 * 0, 1, a neighbouring router entry, a stream format word). One bad read
 * here was written straight back into the DSP's control register: 0x1e1c1
 * (router entry 0x190034) reads as halt state 8 and became a 0x1e149
 * write (note 58; suspect for the 2026-11-07 22:25 DSP halt). A real
 * halt reads the same every time.
 */
static int dsp_dbgcntl_read_stable(struct creative_chip *chip, unsigned int *reg)
{
	unsigned int v[3] = { ~0U, ~0U, ~0U }, tries, i;

	for (tries = 0; tries < 4; tries++) {
		for (i = 0; i < 3; i++)
			if (creative_dsp_chipio_read(chip, DSP_DBGCNTL_INST_OFFSET, &v[i]) < 0)
				break;
		if (i == 3 && !((v[0] ^ v[1]) & 0xffff) && !((v[1] ^ v[2]) & 0xffff)) {
			*reg = v[2];
			return 0;
		}
	}
	dev_warn_ratelimited(chip->dev, "DBGCNTL reads disagree (0x%08x 0x%08x 0x%08x): DSP run state left alone\n",
			     v[0], v[1], v[2]);
	return -EIO;
}

static int dsp_set_run_state(struct creative_chip *chip)
{
	unsigned int reg, halt_state;
	int err;

	err = dsp_dbgcntl_read_stable(chip, &reg);
	if (err < 0)
		return err;
	halt_state = (reg & DSP_DBGCNTL_STATE_MASK) >> DSP_DBGCNTL_STATE_LOBIT;
	if (halt_state != 0)
		dev_info(chip->dev, "DSP halt state %u (DBGCNTL 0x%08x): setting it running\n",
			 halt_state, reg);
	if (halt_state != 0) {
		reg &= ~((halt_state << DSP_DBGCNTL_SS_LOBIT) &
			 DSP_DBGCNTL_SS_MASK);
		err = creative_dsp_chipio_write(chip, DSP_DBGCNTL_INST_OFFSET,
						reg);
		if (err < 0)
			return err;
		reg |= (halt_state << DSP_DBGCNTL_EXEC_LOBIT) &
		       DSP_DBGCNTL_EXEC_MASK;
		err = creative_dsp_chipio_write(chip, DSP_DBGCNTL_INST_OFFSET,
						reg);
		if (err < 0)
			return err;
	}
	return 0;
}

/* Router ports (ChipIO) */
static int dsp_allocate_router_ports(struct creative_chip *chip,
				     unsigned int num_chans,
				     unsigned int ports_per_channel,
				     unsigned int start_device,
				     unsigned int *port_map)
{
	int status, res;
	u8 val;

	status = chipio_send(chip, VENDOR_CHIPIO_STATUS, 0);
	if (status < 0)
		return status;
	val = start_device << 6;
	val |= (ports_per_channel - 1) << 4;
	val |= num_chans - 1;
	dsp_write(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PORT_ALLOC_CONFIG_SET, val);
	dsp_write(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PORT_ALLOC_SET, MEM_CONNID_DSP);
	status = chipio_send(chip, VENDOR_CHIPIO_STATUS, 0);
	if (status < 0)
		return status;
	res = dsp_read(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PORT_ALLOC_GET, 0);
	*port_map = res;
	return 0;
}

static int dsp_allocate_ports_format(struct creative_chip *chip,
				     unsigned short fmt, unsigned int *port_map)
{
	unsigned int num_chans = get_hdafmt_chs(fmt) + 1;
	unsigned int sample_rate_div = ((get_hdafmt_rate(fmt) >> 0) & 3) + 1;
	unsigned int sample_rate_mul = ((get_hdafmt_rate(fmt) >> 3) & 3) + 1;
	unsigned int rate_multi = sample_rate_mul / sample_rate_div;

	if (rate_multi != 1 && rate_multi != 2 && rate_multi != 4)
		return -EINVAL;
	return dsp_allocate_router_ports(chip, num_chans, rate_multi, 0,
					 port_map);
}

static void dspload_post_setup(struct creative_chip *chip)
{
	/* use_alt_functions is true for AE-series; mainline skips this block.
	 * Leave it out to match the AE path.
	 */
}

/* ------------------------------------------------------------------ */
/* Minimal stream-based DMA engine for the firmware transfer.	  */
/* Uses HDA output stream descriptor index 5 (BAR0+0x120), tag 7.	  */
/* ------------------------------------------------------------------ */
/* stream descriptor 5 / tag 7, or Windows' descriptor 4 / tag 1 */
#define DSPLOAD_STREAM_BASE	0x100	/* Windows: descriptor 4, as for playback */
#define DSPLOAD_STREAM_TAG	1
#define DSPLOAD_BUF_SIZE	(256 * 1024)

struct ae9_dma_engine {
	struct creative_chip *chip;
	void *buf;
	dma_addr_t buf_dma;
	void *bdl;
	dma_addr_t bdl_dma;
	size_t buf_size;
	unsigned short hda_format;
	unsigned char stream_tag;
	void *posbuf;		/* fw_posbuf=1 */
	dma_addr_t posbuf_dma;
};

/* Reset the download stream (read position back to 0) and reprogram it,
 * the same register steps as dma_prepare(). */
static void dma_program(struct ae9_dma_engine *dma)
{
	struct creative_chip *chip = dma->chip;
	void __iomem *sd = chip->hda_regs + DSPLOAD_STREAM_BASE;
	u32 ctl;

	ctl = readl(sd + AZX_REG_SD_CTL) & ~SD_CTL_DMA_START;
	writel((ctl | SD_CTL_STREAM_RESET) & SD_CTL_WMASK, sd + AZX_REG_SD_CTL);
	wmb();
	udelay(50);
	writel((ctl & ~SD_CTL_STREAM_RESET) & SD_CTL_WMASK, sd + AZX_REG_SD_CTL);
	wmb();
	udelay(50);

	writel((dma->stream_tag & 0xf) << 20, sd + AZX_REG_SD_CTL);
	writel(dma->buf_size, sd + AZX_REG_SD_CBL);
	writew(63, sd + AZX_REG_SD_LVI);	/* 64 BDL entries, as Windows */
	writew(dma->hda_format, sd + AZX_REG_SD_FORMAT);
	writel(lower_32_bits(dma->bdl_dma), sd + AZX_REG_SD_BDLPL);
	writel(upper_32_bits(dma->bdl_dma), sd + AZX_REG_SD_BDLPU);
	wmb();
}

static int dma_prepare(struct ae9_dma_engine *dma)
{
	struct creative_chip *chip = dma->chip;
	__le32 *bdl;
	u32 ctl;

	dma->buf = dma_alloc_coherent(chip->dev, ae9_dma_size(chip, DSPLOAD_BUF_SIZE),
				      &dma->buf_dma, GFP_KERNEL);
	if (!dma->buf)
		return -ENOMEM;
	dma->bdl = dma_alloc_coherent(chip->dev, ae9_dma_size(chip, DSPLOAD_BDL_BYTES), &dma->bdl_dma,
				      GFP_KERNEL);
	if (!dma->bdl) {
		dma_free_coherent(chip->dev, ae9_dma_size(chip, DSPLOAD_BUF_SIZE), dma->buf,
				  dma->buf_dma);
		dma->buf = NULL;
		return -ENOMEM;
	}
	dma->buf_size = DSPLOAD_BUF_SIZE;
	dma->stream_tag = DSPLOAD_STREAM_TAG;

	bdl = dma->bdl;
	/* 64 entries of 4 KB, as Windows (one entry broke partial chunks) */
	{
		unsigned int i, n = 64, len = dma->buf_size / 64;

		for (i = 0; i < n; i++) {
			dma_addr_t a = dma->buf_dma + (dma_addr_t)i * len;

			bdl[i * 4 + 0] = cpu_to_le32(lower_32_bits(a));
			bdl[i * 4 + 1] = cpu_to_le32(upper_32_bits(a));
			bdl[i * 4 + 2] = cpu_to_le32(len);
			bdl[i * 4 + 3] = cpu_to_le32(0);
		}
	}
	wmb();

	/* reset stream descriptor */
	ctl = readl(chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_CTL) & SD_CTL_WMASK;
	ctl &= ~SD_CTL_DMA_START;
	writel(ctl | SD_CTL_STREAM_RESET,
	       chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_CTL);
	wmb();
	udelay(50);
	writel(ctl & ~SD_CTL_STREAM_RESET,
	       chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_CTL);
	wmb();
	udelay(50);

	/* program descriptor: tag, CBL, LVI, format (16-bit!), BDL ptr */
	writel((dma->stream_tag & 0xf) << 20,
	       chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_CTL);
	writel(dma->buf_size,
	       chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_CBL);
	writew(63, chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_LVI);
	writew(dma->hda_format,
	       chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_FORMAT);
	writel(lower_32_bits(dma->bdl_dma),
	       chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_BDLPL);
	writel(upper_32_bits(dma->bdl_dma),
	       chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_BDLPU);
	wmb();
	if (fw_posbuf && !chip->posbuf) {
		dma->posbuf = dma_alloc_coherent(chip->dev, ae9_dma_size(chip, PAGE_SIZE),
						 &dma->posbuf_dma, GFP_KERNEL);
		if (dma->posbuf) {
			writel(upper_32_bits(dma->posbuf_dma), chip->hda_regs + AZX_REG_DPUBASE);
			writel(lower_32_bits(dma->posbuf_dma) | 1, chip->hda_regs + AZX_REG_DPLBASE);
			dev_info(chip->dev, "fw_posbuf: DMA position buffer on at 0x%llx\n",
				 (u64)dma->posbuf_dma);
		}
	}
	return 0;
}

static void dma_cleanup(struct ae9_dma_engine *dma)
{
	struct creative_chip *chip = dma->chip;
	u32 ctl;

	ctl = readl(chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_CTL) & SD_CTL_WMASK;
	writel(ctl & ~SD_CTL_DMA_START,
	       chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_CTL);
	wmb();
	/* Windows resets the download stream right after the transfer */
	writel(SD_CTL_STREAM_RESET,
	       chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_CTL);
	wmb();
	udelay(50);
	writel(0, chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_CTL);
	wmb();
	udelay(50);
	if (dma->posbuf) {
		writel(0, chip->hda_regs + AZX_REG_DPLBASE);
		writel(0, chip->hda_regs + AZX_REG_DPUBASE);
		readl(chip->hda_regs + AZX_REG_DPLBASE);
		{
			/* which slot this controller uses for the download
			 * stream isn't known: log the busiest one */
			__le32 *pb = dma->posbuf;
			unsigned int i, best = 0;

			for (i = 0; i < 16; i += 2)
				if (le32_to_cpu(pb[i]) > le32_to_cpu(pb[best]))
					best = i;
			dev_info(chip->dev, "fw_posbuf: slot %u position 0x%x after the download\n",
				 best / 2, le32_to_cpu(pb[best]));
		}
		dma_free_coherent(chip->dev, ae9_dma_size(chip, PAGE_SIZE), dma->posbuf,
				  dma->posbuf_dma);
		dma->posbuf = NULL;
	}
	if (dma->buf)
		dma_free_coherent(chip->dev, ae9_dma_size(chip, DSPLOAD_BUF_SIZE), dma->buf,
				  dma->buf_dma);
	if (dma->bdl)
		dma_free_coherent(chip->dev, ae9_dma_size(chip, DSPLOAD_BDL_BYTES), dma->bdl,
				  dma->bdl_dma);
	dma->buf = dma->bdl = NULL;
}

static void dma_run(struct ae9_dma_engine *dma, bool run)
{
	struct creative_chip *chip = dma->chip;
	u32 ctl = readl(chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_CTL);

	if (run)
		ctl |= SD_CTL_DMA_START;
	else
		ctl &= ~SD_CTL_DMA_START;
	writel((ctl) & SD_CTL_WMASK, chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_CTL);
	wmb();
}

/* hda_format for DSP load: channels @ 32-bit @ sample_rate */
static unsigned short ae9_hda_format(unsigned int sample_rate,
				     unsigned short channels)
{
	return (unsigned short)snd_hdac_stream_format(channels, 32, sample_rate);
}

/* ------------------------------------------------------------------ */
/* dspxfr — transfer one FLS image (HCI + DMA segments)		  */
/* ------------------------------------------------------------------ */
static int dspxfr_hci_write(struct creative_chip *chip,
			    const struct ae9_dsp_image_seg *fls)
{
	const u32 *data;
	unsigned int count;
	int status;

	if (!fls || fls->chip_addr != FLS_CHIP_ADDR_MAGIC)
		return -EINVAL;
	count = fls->count;
	data = fls->data;
	while (count >= 2) {
		status = creative_dsp_chipio_write(chip, data[0], data[1]);
		if (status < 0)
			return status;
		count -= 2;
		data += 2;
	}
	return 0;
}

static bool seg_is_hci(const struct ae9_dsp_image_seg *p)
{
	return p->chip_addr == FLS_CHIP_ADDR_MAGIC;
}

static bool seg_is_last(const struct ae9_dsp_image_seg *p)
{
	return p->count == 0;
}

static const struct ae9_dsp_image_seg *
seg_next(const struct ae9_dsp_image_seg *p)
{
	return (const struct ae9_dsp_image_seg *)
		((const u8 *)p + sizeof(*p) + p->count * 4);
}

static int dspxfr_one_seg(struct creative_chip *chip,
			  const struct ae9_dsp_image_seg *fls,
			  struct ae9_dma_engine *dma, unsigned int dma_chan,
			  unsigned int port_map_mask)
{
	const u32 *data;
	unsigned int chip_addx, words_to_write, buffer_size_words;
	unsigned int hda_frame_size_words, remainder_words, run_size_words;
	const u32 *data_remainder;
	u32 chip_addx_remainder;
	const struct ae9_dsp_image_seg *hci_write = NULL;
	unsigned long timeout;
	bool comm_done = false;
	int status;

	if (!fls)
		return -EINVAL;
	if (seg_is_hci(fls)) {
		hci_write = fls;
		fls = seg_next(fls);
	}
	if (hci_write && (!fls || seg_is_last(fls)))
		return dspxfr_hci_write(chip, hci_write);
	if (!fls || !port_map_mask)
		return -EINVAL;

	data = fls->data;
	chip_addx = fls->chip_addr;
	words_to_write = fls->count;
	if (!words_to_write)
		return hci_write ? dspxfr_hci_write(chip, hci_write) : 0;

	if (!UC_RANGE(chip_addx, words_to_write) &&
	    !X_RANGE_ALL(chip_addx, words_to_write) &&
	    !Y_RANGE_ALL(chip_addx, words_to_write))
		return -EINVAL;

	buffer_size_words = dma->buf_size / sizeof(u32);
	hda_frame_size_words = DMA_TRANSFER_FRAME_SIZE_NWORDS; /* 8ch @ rate1 */
	buffer_size_words = min(buffer_size_words,
				(unsigned int)(UC_RANGE(chip_addx, 1) ?
				65536 : 32768));
	buffer_size_words -= buffer_size_words % hda_frame_size_words;
	if (buffer_size_words < hda_frame_size_words)
		return -EINVAL;

	remainder_words = words_to_write % hda_frame_size_words;
	data_remainder = data;
	chip_addx_remainder = chip_addx;
	data += remainder_words;
	chip_addx += remainder_words * sizeof(u32);
	words_to_write -= remainder_words;

	while (words_to_write != 0) {
		u32 lpib0, lpib1;
		unsigned int flicker;

		run_size_words = min(buffer_size_words, words_to_write);
		dma_program(dma);	/* read position back to 0 (Windows resets per chunk) */
		memcpy(dma->buf, data, run_size_words * sizeof(u32));
		if (!comm_done) {
			status = dsp_dma_stop(chip, dma_chan);
			if (status < 0)
				return status;
			status = dsp_dma_setup_common(chip, chip_addx, dma_chan,
						      port_map_mask);
			if (status < 0)
				return status;
			comm_done = true;
		}
		status = dsp_dma_setup(chip, chip_addx, run_size_words,
				       dma_chan);
		if (status < 0)
			return status;
		status = dsp_dma_start(chip, dma_chan);
		if (status < 0)
			return status;
		if (!dsp_is_dma_active(chip, dma_chan)) {
			dev_warn(chip->dev, "dspxfr: DSP DMA did not start\n");
			return -EIO;
		}
		lpib0 = readl(chip->hda_regs + DSPLOAD_STREAM_BASE +
			      AZX_REG_SD_LPIB);
		dma_run(dma, true);
		usleep_range(2000, 3000);
		lpib1 = readl(chip->hda_regs + DSPLOAD_STREAM_BASE +
			      AZX_REG_SD_LPIB);
		dev_info(chip->dev,
			 "dspxfr chunk: addx=0x%x words=%u LPIB %x->%x sd_ctl=%x\n",
			 chip_addx, run_size_words, lpib0, lpib1,
			 readl(chip->hda_regs + DSPLOAD_STREAM_BASE +
			       AZX_REG_SD_CTL));
		if (creative_ae9_gone(chip, "firmware chunk start"))
			return -ENODEV;

		if (remainder_words != 0) {
			unsigned int i;

			for (i = 0; i < remainder_words; i++) {
				status = creative_dsp_chipio_write(chip,
					chip_addx_remainder + i * 4,
					data_remainder[i]);
				if (status < 0)
					return status;
			}
			remainder_words = 0;
		}
		if (hci_write) {
			status = dspxfr_hci_write(chip, hci_write);
			if (status < 0)
				return status;
			hci_write = NULL;
		}

		/*
		 * Wait for the DSP DMA channel to go idle. A single "idle" read
		 * can be wrong while the chunk is still running (seen on resume:
		 * the loop stopped after 21 ms and the next read said busy), so
		 * only trust "idle" when a second read 1 ms later agrees.
		 */
		timeout = jiffies + msecs_to_jiffies(2000);
		flicker = 0;
		do {
			if (creative_ae9_gone(chip, "firmware chunk"))
				return -ENODEV;
			if (!dsp_is_dma_active(chip, dma_chan)) {
				if (!dsp_dma_active_again(chip, dma_chan))
					break;
				flicker++;
			}
			msleep(20);
		} while (time_before(jiffies, timeout));
		if (flicker)
			dev_info(chip->dev,
				 "dspxfr: chunk addx=0x%x: %u idle read(s) not confirmed, kept waiting\n",
				 chip_addx, flicker);

		dma_run(dma, false);
		/*
		 * A chunk the DSP never finished is a partial firmware image:
		 * the DSP then boots but runs broken (seen as a clipped,
		 * full-scale XLR capture after a resume). Fail the load so the
		 * bring-up retry starts over, rather than carrying on. Two busy
		 * reads in a row, for the same reason as above.
		 */
		if (dsp_is_dma_active(chip, dma_chan) &&
		    dsp_dma_active_again(chip, dma_chan)) {
			dev_warn(chip->dev,
				 "dspxfr: chunk addx=0x%x (%u words) did not complete (LPIB %x->%x); failing the firmware load\n",
				 chip_addx, run_size_words, lpib0, lpib1);
			return -ETIMEDOUT;
		}
		data += run_size_words;
		chip_addx += run_size_words * sizeof(u32);
		words_to_write -= run_size_words;
	}

	if (remainder_words != 0) {
		unsigned int i;

		for (i = 0; i < remainder_words; i++)
			creative_dsp_chipio_write(chip,
				chip_addx_remainder + i * 4, data_remainder[i]);
	}
	return 0;
}

static int dspxfr_image(struct creative_chip *chip,
			const struct ae9_dsp_image_seg *fls_data,
			unsigned int sample_rate, unsigned short channels)
{
	struct ae9_dma_engine dma;
	unsigned short hda_format;
	unsigned int response, port_map_mask = 0;
	unsigned int dma_chan = 0;
	int status;

	memset(&dma, 0, sizeof(dma));
	dma.chip = chip;
	hda_format = ae9_hda_format(sample_rate, channels);
	dma.hda_format = hda_format;

	/* set node 0x15 (chip ctrl) converter format */
	verb_send(chip,
		creative_ae9_verb(CAD, WIDGET_CHIP_CTRL,
				  VENDOR_CHIPIO_STREAM_FORMAT,
				  hda_format & 0xffff), &response);

	status = dma_prepare(&dma);
	dev_info(chip->dev, "dspxfr: dma_prepare ret=%d\n", status);
	if (status < 0)
		return status;

	/* Windows binds node 0x15 to the load stream before allocating ports */
	verb_send(chip,
		  creative_ae9_verb(CAD, WIDGET_CHIP_CTRL, 0x706,
				    (dma.stream_tag << 4)), &response);

	status = dsp_allocate_ports_format(chip, hda_format, &port_map_mask);
	dev_info(chip->dev, "dspxfr: alloc_ports ret=%d port_map_mask=0x%x\n",
		 status, port_map_mask);
	if (status < 0)
		goto exit;
	/*
	 * If the DSP is cold (no firmware running yet), port allocation
	 * returns 0 because the ChipIO port allocator isn't initialised.
	 * The firmware-download DMA writes to UC/X/Y memory (not audio
	 * channels), so AUDCHSEL being 0 doesn't break the transfer.
	 * Use a non-zero fallback so dspxfr_one_seg doesn't bail early.
	 */
	if (!port_map_mask) {
		dev_info(chip->dev,
			 "dspxfr: port_map_mask=0 (DSP cold), using fallback 0xff\n");
		port_map_mask = 0xff;
	}

	/* walk all FLS segments */
	while (fls_data && !seg_is_last(fls_data)) {
		if (fls_data->magic != FLS_MAGIC) {
			dev_err(chip->dev, "FLS seg check fail\n");
			status = -EINVAL;
			break;
		}
		status = dspxfr_one_seg(chip, fls_data, &dma, dma_chan,
					port_map_mask);
		if (status < 0)
			break;
		if (seg_is_hci(fls_data))
			fls_data = seg_next(fls_data);
		if (fls_data && !seg_is_last(fls_data))
			fls_data = seg_next(fls_data);
	}

	/* free the ports, then unbind, as Windows does (capture seq 4534-4538)
	 * and upstream ca0132's dspxfr_image does */
	verb_send(chip,
		creative_ae9_verb(CAD, WIDGET_CHIP_CTRL,
				  VENDOR_CHIPIO_PORT_FREE_SET, 0x10), &response);
	verb_send(chip,
		creative_ae9_verb(CAD, WIDGET_CHIP_CTRL, 0x706, 0), &response);
exit:
	dma_cleanup(&dma);
	return status;
}

/*
 * Where ChipIO reads an image word. The image addresses the DSP's aux RAM
 * the way the DSP does, straight after main X / Y RAM (X_END..X_EXT,
 * Y_END..Y_EXT), and the download's DSP DMA writes it there; ChipIO maps
 * that aux RAM elsewhere (AXRAM_CHIP_OFFSET, AYRAM_CHIP_OFFSET). Reading
 * the image address itself gave the old "27 + 24 mismatches", all in
 * those two ranges (WINDOWS-CAPTURE-ANALYSIS §21).
 */
static unsigned int dsp_verify_addr(unsigned int addr, bool *aux)
{
	*aux = true;
	if (addr >= X_END && addr < X_EXT)
		return AXRAM_CHIP_OFFSET + (addr - X_END);
	if (addr >= Y_END && addr < Y_EXT)
		return AYRAM_CHIP_OFFSET + (addr - Y_END);
	*aux = false;
	return addr;
}

static void dsp_verify_image(struct creative_chip *chip,
			     const struct ae9_dsp_image_seg *fls)
{
	unsigned int seg = 0;

	for (; fls && !seg_is_last(fls); fls = seg_next(fls), seg++) {
		unsigned int i, ok = 0, bad = 0, shown = 0, aux_ok = 0, aux_bad = 0;

		if (seg_is_hci(fls))
			continue;
		for (i = 0; i < fls->count; i = (i + 97 < fls->count ||
						 i == fls->count - 1) ?
						i + 97 : fls->count - 1) {
			unsigned int got = 0, img = fls->chip_addr + 4 * i, addr;
			bool aux;

			addr = dsp_verify_addr(img, &aux);
			creative_dsp_chipio_addr_invalidate();
			creative_dsp_chipio_read(chip, addr, &got);
			if (got == fls->data[i]) {
				ok++;
				aux_ok += aux;
			} else {
				bad++;
				aux_bad += aux;
				if (shown++ < 5)
					dev_info(chip->dev,
						 "fw_verify: seg%u word %u addr 0x%06x%s got 0x%08x want 0x%08x\n",
						 seg, i, img, aux ? " (aux)" : "", got, fls->data[i]);
			}
			if (i == fls->count - 1)
				break;
		}
		dev_info(chip->dev,
			 "fw_verify: seg%u chip 0x%06x %u words: %u match, %u mismatch (aux RAM: %u match, %u mismatch)\n",
			 seg, fls->chip_addr, fls->count, ok, bad, aux_ok, aux_bad);
	}
}

static int dspload_image(struct creative_chip *chip,
			 const struct ae9_dsp_image_seg *fls)
{
	unsigned int sample_rate = 48000;
	unsigned short channels = DMA_TRANSFER_FRAME_SIZE_NWORDS;
	bool parked;
	int status;

	while (channels > 16) {
		sample_rate *= 2;
		channels /= 2;
	}

	parked = ae9_rings_park(chip, "firmware");
	status = dspxfr_image(chip, fls, sample_rate, channels);
	if (parked) {
		int u = ae9_rings_unpark(chip, "firmware");

		if (u == -ENODEV)
			return u;
	}
	if (chip->rings_deferred && !chip->card_gone) {
		/* fw_rings_off=1: the firmware stream has stopped; start the
		 * command rings now, before the boot handshake */
		int cret = creative_ae9_corb_init(chip);

		chip->rings_deferred = false;
		dev_info(chip->dev, "corb_init after the firmware stream: %d\n", cret);
		if (creative_ae9_gone(chip, "CORB/RIRB start after the firmware"))
			return -ENODEV;
	}
	if (status < 0)
		return status;
	dspload_post_setup(chip);
	if (fw_verify)
		dsp_verify_image(chip, fls);
	/* the MASTERCONTROL boot handshake straight after the run, as Windows */
	{
		int r = dsp_set_run_state(chip);
		int hs;

		/*
		 * Windows waits 250 ms after starting the DSP before the first
		 * handshake message (decompile FUN_0002859c:
		 * KeDelayExecutionThread -2500000 = 250 ms; 251-266 ms of
		 * silence in all 15 cold-boot captures). We sent it at once,
		 * and the first bring-up failed on 5 of 13 boots (2026-11-07).
		 */
		msleep(250);
		hs = creative_dsp_boot_handshake(chip);

		chip->hs_done = 1;
		chip->hs_ok = !hs;
		dev_info(chip->dev,
			 "dspload: run ret=%d, immediate boot handshake %s (%d)\n",
			 r, hs ? "INCOMPLETE" : "complete", hs);
		if (hs && chip->resuming)
			return -EIO;
		return r;
	}
}

static bool dspload_is_loaded(struct creative_chip *chip)
{
	unsigned int data = 0;

	if (creative_dsp_chipio_read(chip, 0x40004, &data) < 0)
		return false;
	return data == 1;
}

/* ------------------------------------------------------------------ */
/* DSP parameter set (SCP) + output routing				  */
/* ------------------------------------------------------------------ */
#define FLOAT_ZERO	0x00000000
#define FLOAT_ONE	0x3f800000
#define FLOAT_EIGHT	0x41000000
#define SPEAKER_TUNING_USE_SPEAKER_EQ	0x1f
#define SPEAKER_TUNING_MUTE		0x3a

/* ChipIO control-param + stream control */
#define VENDOR_CHIPIO_PARAM_SET2	0x710	/* (alias of PARAM_SET) */
/* Extended chipio_set_control_param verbs — confirmed from decompiled driver
 * and cold-boot CORB capture (0x717 p=param_id, 0x718 p=param_val).
 * NOTE: 0x711/0x712 are PORT_ALLOC_CONFIG_SET/PORT_ALLOC_SET (different ops).
 */
#define VENDOR_CHIPIO_PARAM_EX_ID_SET		0x717
#define VENDOR_CHIPIO_PARAM_EX_VALUE_SET	0x718
#define VENDOR_CHIPIO_8051_WRITE_DIRECT	0x500
#define CONTROL_PARAM_ASI		23
#define CONTROL_PARAM_STREAM_ID		24
#define CONTROL_PARAM_STREAM_SOURCE_CONN	25
#define CONTROL_PARAM_STREAM_DEST_CONN	26
#define CONTROL_PARAM_STREAMS_CHANNELS	27
#define CONTROL_PARAM_STREAM_CONTROL	28
#define CONTROL_PARAM_CONN_POINT_ID	29	/* confirmed: decompiled enum + coldboot CORB */
#define CONTROL_PARAM_CONN_POINT_SR	30	/* confirmed: decompiled enum + coldboot CORB */
#define CONTROL_FLAG_DSP_96KHZ		9
#define CONTROL_FLAG_DAC_96KHZ		8
#define CONTROL_FLAG_ADC_B_96KHZ	6
#define CONTROL_FLAG_ADC_C_96KHZ	7
#define CONTROL_FLAG_SRC_RATE_96KHZ	11
#define CONTROL_FLAG_IDLE_ENABLE	2
#define CONTROL_FLAG_SPDIF2OUT		4
#define CONTROL_FLAG_PORT_A_10KOHM_LOAD	20
#define CONTROL_FLAG_PORT_D_10KOHM_LOAD	21
#define CONTROL_FLAG_ASI_96KHZ		22
#define SR_96_000			0x0b
#define SR_48_000			0x09
#define MEM_CONNID_WUH			10

static void ca0113_cmd(struct creative_chip *chip, u8 group, u8 reg, u8 val,
		       unsigned int settle_us);

/*
 * Send a list of {group, reg, value} CA0113 commands, Windows protocol.
 * The whole list runs under ca0113_mutex: playback start (PipeWire's
 * thread) and the heartbeat's volume changes used to share an unlocked
 * "replaying" flag and settle time, so one path could drop the rest of the
 * other's list (output select included) or land a command between the
 * other's register write and commit.
 */
static void ca0113_list(struct creative_chip *chip, const u8 (*cmds)[3],
			size_t n, unsigned int settle_us)
{
	size_t i;

	mutex_lock(&chip->ca0113_mutex);
	for (i = 0; i < n; i++) {
		u8 val = cmds[i][2];

		/* DAC 48/07: bits 7:5 filter, bit 0 mute. Keep the user's
		 * "DAC Filter" choice through every replayed table. */
		if (cmds[i][0] == 0x48 && cmds[i][1] == 0x07) {
			val = (val & 0x1f) | chip->dac_filter_bits;
			chip->dac48_07 = val;
		}
		ca0113_cmd(chip, cmds[i][0], cmds[i][1], val, settle_us);
	}
	mutex_unlock(&chip->ca0113_mutex);
}

void creative_dsp_ca0113_replay(struct creative_chip *chip, const u8 (*cmds)[3],
				size_t n, const char *what)
{
	ca0113_list(chip, cmds, n, CA0113_SETTLE_US);
	if (chip->dsp_done)
		ae9_dbg(chip, "ca0113_windows: %s, %zu commands\n", what, n);
	else
		dev_info(chip->dev, "ca0113_windows: %s, %zu commands\n", what, n);
}

/* Same without the log line and with the short settle time (volume). */
void creative_dsp_ca0113_replay_quiet(struct creative_chip *chip,
				      const u8 (*cmds)[3], size_t n)
{
	ca0113_list(chip, cmds, n, hu_cmd_settle_us);
}

/*
 * Head-unit DAC routing, replayed byte-exact from the 0x0071 capture's
 * AUDIO-PLAYBACK window (ae9_mmio_capture.txt ~195s). These program the
 * head-unit audio router/DAC (targets 0x48 control + 0x49 DAC channels).
 * The channel regs on target 0x49 set to 0x1e are what actually route the
 * 6 channels into the head-unit DAC — the rear card RCA is fed from it.
 * Our AE-7 standup never touched target 0x49, so RCA stayed silent.
 *
 * Handshake here is the playback variant: 0x800005 (set) / 0x800004 (commit),
 * distinct from the standup's 0x800001/0x800002.
 */
static void ca0113_cmd(struct creative_chip *chip, u8 group, u8 reg, u8 val,
		       unsigned int settle_us)
{
	chip->dac_start_key = 0;	/* DAC touched: next start sends all */
	creative_ext_writel(chip, 0x210, 0x0000007e);
	creative_ext_readl(chip, 0x210);
	creative_ext_writel(chip, 0x210, 0x0000005a);
	creative_ext_readl(chip, 0x210);
	creative_ext_writel(chip, 0x20c, 0x00800005);
	creative_ext_writel(chip, 0x804, group);
	creative_ext_writel(chip, 0x20c, 0x00800005);
	creative_ext_writel(chip, 0x204, (reg & 0xff) | (val << 8));
	usleep_range(settle_us, settle_us + 500);
	creative_ext_readl(chip, 0x860);
	creative_ext_readl(chip, 0x854);
	creative_ext_readl(chip, 0x840);
	creative_ext_writel(chip, 0x20c, 0x00800004);
	creative_ext_writel(chip, 0x210, 0x00000000);
	creative_ext_readl(chip, 0x210);
}

/* ------------------------------------------------------------------ */
/* Public: download the DSP firmware					  */
/* ------------------------------------------------------------------ */
int creative_dsp_load_firmware(struct creative_chip *chip)
{
	const struct firmware *fw;
	const struct ae9_dsp_image_seg *fls;
	unsigned long timeout;
	int ret;

	/* If the DSP already has firmware running (e.g. warm reboot — the DSP
	 * isn't power-cycled), don't try to reload over it (that crashes it).
	 * Check both 0x40004==1 (standard loaded flag) and 0x801c!=0 (audio
	 * loop counter — non-zero means the 8051 is already processing audio,
	 * which is an even stronger signal the DSP is live). Transferring
	 * firmware over a running DSP halts it (DBGCNTL halt_state=0xf) and
	 * then 0x40004 no longer reads 1, causing a spurious -ETIMEDOUT.
	 */
	{
		unsigned int loop_val = 0, dbgcntl = 0;
		int halt_state;

		creative_dsp_chipio_read(chip, 0x801c, &loop_val);
		creative_dsp_chipio_read(chip, DSP_DBGCNTL_INST_OFFSET, &dbgcntl);
		halt_state = (dbgcntl & DSP_DBGCNTL_STATE_MASK) >>
			     DSP_DBGCNTL_STATE_LOBIT;

		if (loop_val && loop_val == 0x0bad0add) {
			/* 0x801c contains the QEMU sentinel — not a real counter.
			 * Treat as cold boot and fall through to firmware download.
			 */
			dev_info(chip->dev,
				 "0x801c=0x%x is QEMU sentinel, treating as cold boot\n",
				 loop_val);
			loop_val = 0;
		}

		/* halt=0xf with junk in 0x801c is what every cold card shows
		 * here; download normally (the old self-reset is gone) */
		if (loop_val && halt_state != 0) {
			dev_info(chip->dev, "halt=0x%x 0x801c=0x%x: downloading normally\n",
				 halt_state, loop_val);
			loop_val = 0;
		}

		if (loop_val && halt_state == 0) {
			/* Audio loop running and DSP not halted — genuinely
			 * warm, skip firmware download entirely.
			 */
			dev_info(chip->dev,
				 "DSP audio loop running (0x801c=0x%x halt=0); skipping firmware download\n",
				 loop_val);
			return 0;
		}
	}
	if (dspload_is_loaded(chip)) {
		/* 0x40004==1 is unreliable after QEMU device reset: QEMU zeroes
		 * EXRAM but leaves the loaded flag set. Detect this by checking
		 * 0x800000 for the QEMU sentinel 0x0bad0add — if present, EXRAM
		 * was wiped and we must re-download firmware despite 0x40004==1.
		 */
		unsigned int exram_check = 0;
		creative_dsp_chipio_read(chip, 0x800000, &exram_check);
		if (exram_check == 0x0bad0add) {
			dev_info(chip->dev,
				 "DSP firmware flag set but EXRAM zeroed (QEMU reset, 0x800000=0x%x) — re-downloading\n",
				 exram_check);
			/* fall through to firmware download */
		} else {
			dev_info(chip->dev, "DSP firmware already loaded; skipping download\n");
			return 0;
		}
	}

	ret = ae9_request_fw(chip, &fw, DESKTOP_EFX_FILE, DESKTOP_EFX_SIZE);
	if (ret < 0) {
		dev_err(chip->dev, "firmware %s not found: %d\n",
			DESKTOP_EFX_FILE, ret);
		return ret;
	}

	if (fls_image_check(chip, fw, DESKTOP_EFX_FILE)) {
		release_firmware(fw);
		return -EINVAL;
	}
	fls = (const struct ae9_dsp_image_seg *)fw->data;

	dev_info(chip->dev, "downloading DSP firmware (%zu bytes)...\n",
		 fw->size);

	creative_dsp_prepare(chip);

	/*
	 * Clear the DSP firmware "loaded" flag at 0x40004 before downloading.
	 * After a self-reset the old Windows firmware left stale values in EXRAM
	 * (including 0x40004 from a previous session). The new firmware writes
	 * 0x40004=1 when it fully initialises. If the stale value is already 1,
	 * dspload_is_loaded() would return immediately — wrong state. More
	 * importantly, clearing it forces us to wait for the genuine firmware
	 * write, ensuring the 8051 actually started clean.
	 *
	 * Also clear 0x801c so the audio-loop counter starts at 0.
	 */
	dev_info(chip->dev, "dsp_load: cleared 0x40004 and 0x801c before download\n");

	dev_info(chip->dev, "dsp_load: calling dspload_image\n");
	ret = dspload_image(chip, fls);
	dev_info(chip->dev, "dsp_load: dspload_image ret=%d\n", ret);
	release_firmware(fw);
	if (ret < 0) {
		dev_err(chip->dev, "DSP load failed: %d\n", ret);
		return ret;
	}

	timeout = jiffies + msecs_to_jiffies(8000); /* 8s: clean card needs more time */
	do {
		if (dspload_is_loaded(chip)) {
			dev_info(chip->dev, "DSP firmware downloaded and running\n");

			{
				unsigned int loop_val = 0, dbgcntl = 0;
				creative_dsp_chipio_read(chip, 0x801c, &loop_val);
				creative_dsp_chipio_read(chip, DSP_DBGCNTL_INST_OFFSET, &dbgcntl);
				dev_info(chip->dev,
					 "0x801c right after firmware: 0x%x%s DBGCNTL=0x%x halt=0x%x\n",
					 loop_val,
					 loop_val ? " (ALIVE!)" : " (dead)",
					 dbgcntl,
					 (dbgcntl & DSP_DBGCNTL_STATE_MASK) >> DSP_DBGCNTL_STATE_LOBIT);
			}

			/*
			 * If EXRAM was zeroed by QEMU (0x800000=0x0bad0add),
			 * the SCP queue pointers at 0x800000 are corrupt.
			 * The firmware doesn't clear this region on boot.
			 * Zero it explicitly so the 8051 SCP queue works.
			 */
			{
				unsigned int sentinel = 0;
				creative_dsp_chipio_read(chip, 0x800000,
							 &sentinel);
				if (sentinel == 0x0bad0add && ae9_sentinel_clear != 1)
					dev_info(chip->dev,
						 "0x800000=0x0bad0add left alone after boot (sentinel_clear=%u)\n",
						 ae9_sentinel_clear);
				if (sentinel == 0x0bad0add && ae9_sentinel_clear == 1) {
					unsigned int i;
					dev_info(chip->dev,
						 "Clearing QEMU SCP sentinel region 0x800000-0x8000fc\n");
					/* QEMU zeros EXRAM with 0x0bad0add across
					 * the entire SCP queue area. The 8051
					 * firmware doesn't clear this region.
					 * Zero all 64 DWORDs (256 bytes). */
					for (i = 0; i < 64; i++)
						creative_dsp_chipio_write(chip,
							0x800000 + i * 4,
							0x00000000);
				}
			}
			/*
			 * Wait for the 8051 to fully initialize its EXRAM
			 * SCP state after firmware boot. When EXRAM was zeroed
			 * (QEMU reset: 0x800000=0x0bad0add), the 8051 needs
			 * extra time to set up the SCP command/response queues.
			 * Poll VENDOR_DSPIO_STATUS until ready (up to 2s),
			 * then drain any stale responses.
			 */
			{
				unsigned int dspio_sts = 0;
				int t;
				for (t = 0; t < 200; t++) {
					msleep(10);
					dspio_sts = dsp_read(chip,
						WIDGET_DSP_CTRL,
						VENDOR_DSPIO_STATUS, 0);
					if (dspio_sts == VENDOR_STATUS_DSPIO_OK ||
					    dspio_sts == VENDOR_STATUS_DSPIO_SCP_RESPONSE_QUEUE_EMPTY)
						break;
				}
				dev_info(chip->dev,
					 "SCP ready after %dms (sts=0x%x)\n",
					 t * 10, dspio_sts);
			}
			dspio_clear_response_queue(chip);
			{
				unsigned int loop_val = 0, dspio_sts;
				creative_dsp_chipio_read(chip, 0x801c, &loop_val);
				dspio_sts = dsp_read(chip, WIDGET_DSP_CTRL,
						     VENDOR_DSPIO_STATUS, 0);
				dev_info(chip->dev,
					 "0x801c after SCP ready: 0x%x SCP=0x%x\n",
					 loop_val, dspio_sts);
			}
			return 0;
		}
		msleep(20);
	} while (time_before(jiffies, timeout));

	/*
	 * 0x40004 never became 1 — the 8051 started at a wrong PC due to a
	 * race in the CORB init verb replay during self-reset. The GCTL state
	 * is already clean from the first attempt, so a simple re-run of
	 * dsp_prepare + dspload_image will succeed reliably.
	 */
	dev_warn(chip->dev,
		 "DSP firmware load not confirmed — retrying once\n");
	{
		const struct firmware *fw2;
		const struct ae9_dsp_image_seg *fls2;

		if (ae9_request_fw(chip, &fw2, DESKTOP_EFX_FILE, DESKTOP_EFX_SIZE) == 0) {
			fls2 = (const struct ae9_dsp_image_seg *)fw2->data;
			if (!fls_image_check(chip, fw2, DESKTOP_EFX_FILE)) {
				unsigned long t2;

				creative_dsp_prepare(chip);
				creative_dsp_chipio_write(chip, 0x40004, 0);
				creative_dsp_chipio_write(chip, 0x801c,  0);
				if (dspload_image(chip, fls2) == 0) {
					t2 = jiffies + msecs_to_jiffies(8000);
					do {
						if (dspload_is_loaded(chip)) {
							dev_info(chip->dev,
								 "DSP firmware confirmed on retry\n");
							release_firmware(fw2);
							dspio_clear_response_queue(chip);
							return 0;
						}
						msleep(20);
					} while (time_before(jiffies, t2));
				}
			}
			release_firmware(fw2);
		}
	}
	dev_warn(chip->dev, "DSP firmware load not confirmed (0x40004 != 1)\n");
	return -ETIMEDOUT;
}

/* ------------------------------------------------------------------ */
/* Public: bring up DSP clocks + extensions (foundation test)	  */
/* ------------------------------------------------------------------ */
int creative_dsp_prepare(struct creative_chip *chip)
{
	/*
	 * Force-reset the firmware DMA stream before use.
	 * If a previous session crashed mid-transfer, the stream descriptor
	 * may be in an invalid state causing dma_prepare() to fail with
	 * -EINVAL. Explicitly stop and reset it here regardless of state.
	 */
	{
		u32 ctl = readl(chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_CTL) & SD_CTL_WMASK;
		/* Stop DMA */
		writel(ctl & ~SD_CTL_DMA_START,
		       chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_CTL);
		wmb();
		udelay(50);
		/* Assert stream reset */
		writel(ctl | SD_CTL_STREAM_RESET,
		       chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_CTL);
		wmb();
		udelay(100);
		/* Deassert stream reset */
		writel(ctl & ~(SD_CTL_DMA_START | SD_CTL_STREAM_RESET),
		       chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_CTL);
		wmb();
		udelay(50);
	}

	/* the pre-download replay already sent the codec verbs in Windows'
	 * order (CT extensions, clocks, flags, AFG resets, DSP init) */
	return 0;
}

