// SPDX-License-Identifier: GPL-2.0-only
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

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/jiffies.h>
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
static unsigned int pll_windows = 1;
module_param(pll_windows, uint, 0444);
MODULE_PARM_DESC(pll_windows, "Use the Windows capture's 8051 PLL/PMU writes (1=on, 0=old swapped tables)");

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
 * creative_dsp_select_lineout() opens with a hand transcription of
 * Windows' pre-run and firmware-download steps (8051 patches, three
 * DSPIO_DSP_INIT resets, download DMA setup, the overlay bind, and raw
 * SCP half-words). In Windows those run before the firmware starts; run
 * after it, they reset the DSP (verb_log: DBGCNTL back to 0xfff83cf0).
 * lineout_prerun=0 skips that part (use with fw_split, whose pre-download
 * replay covers it) and starts at the amp/ASI/effects setup.
 */
static unsigned int lineout_prerun;
module_param(lineout_prerun, uint, 0444);
MODULE_PARM_DESC(lineout_prerun, "select_lineout: run the transcribed pre-run/download steps (1=on, 0=skip them, default)");

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
 * prep_windows=1 (use with fw_split): skip creative_dsp_prepare()'s codec
 * verbs and dspload_image()'s extra DSPIO_DSP_INIT. The fw_split
 * pre-download replay already sends all of them in Windows' order
 * (CT extensions, clock enable, control flags, 0x18b0a4, codec-1 AFG reset
 * x2 at seq 752-754, then 0x708 p=0x80 + 0x703 at seq 1002-1004).
 * prepare() repeated the AFG reset *after* 0x708 p=0x80, and dspload
 * sent a second 0x703; Windows does neither.
 */
static unsigned int prep_windows = 1;
module_param(prep_windows, uint, 0444);
MODULE_PARM_DESC(prep_windows, "Skip the duplicate pre-download codec verbs (AFG reset, flags, DSP_INIT) that fw_split's replay already sends (1=on, default)");

/*
 * dl_windows=1: firmware download the way the cold-boot capture does it:
 * stream descriptor 0x100 with tag 1 (Windows uses the same stream for the
 * download and later for playback; ours used 0x120/tag 7), ChipIO stream
 * bind before port allocation, stream reset after the transfer, and no
 * 0x40004/0x801c pre-writes.
 */
static unsigned int dl_windows = 1;
module_param(dl_windows, uint, 0444);
MODULE_PARM_DESC(dl_windows, "Firmware download on stream 0x100/tag 1 in Windows' order, reset after (1=on, default)");

/*
 * hs_immediate=1: send the MASTERCONTROL boot handshake straight after the
 * DBGCNTL run writes, as Windows does (its SET 0x80/0x0c=3 follows within
 * a few verbs), instead of after our ~35 ms of load-flag polling and
 * register reads. Also skips the DBGCNTL read right after the run.
 */
static unsigned int hs_immediate = 1;
module_param(hs_immediate, uint, 0444);
MODULE_PARM_DESC(hs_immediate, "Run the DSP boot handshake immediately after starting the firmware (1=on, default)");

/*
 * fw_verify=1: after the firmware download and before starting the DSP,
 * read samples of every segment back through ChipIO and compare them with
 * the image. Done while the DSP is still halted, so data memory hasn't
 * been touched by running firmware yet.
 */
static unsigned int fw_verify;
module_param(fw_verify, uint, 0444);
MODULE_PARM_DESC(fw_verify, "Read back the downloaded firmware before starting the DSP and compare (1=on)");

/*
 * dl_reset_chunks (default 1): reset and reprogram the firmware-download
 * stream before every DMA chunk. Without it the stream's read position
 * carried on from the previous chunk (log: chunk 2 "LPIB 31ac0->..."), so
 * only the first chunk arrived intact: fw_verify read segment 1 back 677/677
 * correct but segments 2-3 about half wrong. Windows resets the stream per
 * chunk; upstream ca0132 resets its DMA engine per chunk too.
 */
static unsigned int dl_reset_chunks = 1;
module_param(dl_reset_chunks, uint, 0444);
MODULE_PARM_DESC(dl_reset_chunks, "Reset the download stream before each firmware DMA chunk (1=on, default)");

/*
 * dl_bdl64 (default 1): describe the 256 KB download buffer with 64 BDL
 * entries of 4 KB (LVI 63), as Windows does. We used a single entry
 * (LVI 0), which the HDA spec doesn't allow (at least two entries), and
 * the tail of every partial DMA chunk then failed to arrive.
 */
static unsigned int dl_bdl64 = 1;
module_param(dl_bdl64, uint, 0444);
MODULE_PARM_DESC(dl_bdl64, "Firmware download buffer as 64 BDL entries of 4 KB, as Windows (1=on, default)");

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

/*
 * ca0113_windows=1: send the CA0113 (head-unit DAC) commands Windows sends,
 * exactly as in the cold-boot capture, with its protocol (0x20c = 0x800005,
 * 0x800005, 0x800004 around each command), and make our own CA0113 command
 * calls no-ops (GPIO writes stay). Ours used the AE-5/AE-7 protocol
 * (0x800001/0x800003/0x800002) for most commands, which Windows never
 * uses, and several values differ (e.g. 48/0d, 48/1d, 48/07, 49/02-05).
 */
unsigned int ae9_ca0113_windows = 1;
module_param_named(ca0113_windows, ae9_ca0113_windows, uint, 0444);
MODULE_PARM_DESC(ca0113_windows, "Head-unit DAC (CA0113) commands exactly as in the Windows capture (1=on, default)");

/* wait between a CA0113 command's value write and its commit: 20 ms for
 * the boot commands; volume changes (replay_quiet) use hu_cmd_settle_us */
#define CA0113_SETTLE_US	20000
static unsigned int hu_cmd_settle_us = 4000;
module_param(hu_cmd_settle_us, uint, 0644);
MODULE_PARM_DESC(hu_cmd_settle_us, "post_windows: CA0113 settle time for volume changes (us)");

static unsigned int scp_win_read = 1;
module_param(scp_win_read, uint, 0444);
MODULE_PARM_DESC(scp_win_read, "Read SCP GET replies with the Windows 0x704/0xF04 step (1=on, default)");

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

/*
 * dsp_write_nowait - fire-and-forget verb, no RIRB wait.
 *
 * Used only for verbs where the codec may not generate an RIRB response
 * promptly — specifically the 8051 PLL/PMU writes (0x70C/0x70D/0x70E)
 * which cause the 8051 to enter a PLL relock cycle and stop responding.
 * All other writes use dsp_write() which waits for the RIRB response to
 * confirm the verb was accepted before proceeding.
 */
static void dsp_write_nowait(struct creative_chip *chip, u8 nid, u16 verb,
			     u16 payload)
{
	u32 v = creative_ae9_verb(CAD, nid, verb, payload);

	if (chip->corb_buf)
		creative_ae9_corb_send_verb(chip, v);
	else
		creative_ae9_icw_send_verb(chip, v, NULL);
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
static unsigned int chipio_nocache = 1;
module_param(chipio_nocache, uint, 0444);
MODULE_PARM_DESC(chipio_nocache, "Send the ChipIO address on every access instead of caching it (1=on, default)");

static int chipio_write_address(struct creative_chip *chip,
				unsigned int chip_addx)
{
	int res;

	if (!chipio_nocache && curr_chip_addx == chip_addx)
		return 0;

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

static void chipio_set_control_flag(struct creative_chip *chip,
				    int flag_id, bool flag_state)
{
	unsigned int val = ((flag_state ? 1 : 0) << 7) | (flag_id & 0x7f);

	dsp_write(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_FLAG_SET, val);
}

/* 8051 / PLL-PMU */
static void chipio_8051_set_address(struct creative_chip *chip, unsigned int a)
{
	/* 8051 address writes: no RIRB wait — 8051 may be mid-PLL-relock */
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_ADDRESS_LOW,
			 a & 0xff);
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_ADDRESS_HIGH,
			 (a >> 8) & 0xff);
}

static void chipio_8051_set_data_pll(struct creative_chip *chip, unsigned int d)
{
	/* PLL write: no RIRB wait — 8051 enters relock, stops responding */
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PLL_PMU_WRITE,
			 d & 0xff);
}

static void chipio_8051_write_pll_pmu(struct creative_chip *chip,
				      unsigned int addr, unsigned int data)
{
	chipio_8051_set_address(chip, addr & 0xff);
	chipio_8051_set_data_pll(chip, data);
}

void creative_dsp_enable_clocks(struct creative_chip *chip)
{
	chipio_8051_write_pll_pmu(chip, 0x00, 0xff);
	if (pll_windows)
		return;
	chipio_8051_write_pll_pmu(chip, 0x05, 0x0b);
	chipio_8051_write_pll_pmu(chip, 0x06, 0xff);
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
void creative_dsp_write_audio_router(struct creative_chip *chip);
int creative_dsp_select_lineout(struct creative_chip *chip);

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
			/* Un-halt the 8051 so it resumes draining the queue */
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

/* Read one announced SCP reply, Windows-style. Returns words read. */
static int scp_read_announced(struct creative_chip *chip, u32 *hdr,
			      u32 *data, unsigned int max)
{
	unsigned int size, i, cnt;

	dsp_read(chip, WIDGET_DSP_CTRL, VENDOR_DSPIO_SCP_POST_READ_DATA, 0);
	dsp_read(chip, WIDGET_DSP_CTRL, VENDOR_DSPIO_STATUS, 0);
	*hdr = dsp_read(chip, WIDGET_DSP_CTRL, VENDOR_DSPIO_SCP_READ_DATA, 0);
	size = (*hdr >> 27) & 0x1f;

	dsp_read(chip, WIDGET_DSP_CTRL, 0x704, 0x02);
	dsp_read(chip, WIDGET_DSP_CTRL, VENDOR_DSPIO_STATUS, 0);
	cnt = dsp_read(chip, WIDGET_DSP_CTRL, 0xf04, 0);

	for (i = 0; i < size; i++) {
		u32 w;

		dsp_read(chip, WIDGET_DSP_CTRL, VENDOR_DSPIO_SCP_POST_READ_DATA, 0);
		dsp_read(chip, WIDGET_DSP_CTRL, VENDOR_DSPIO_STATUS, 0);
		w = dsp_read(chip, WIDGET_DSP_CTRL, VENDOR_DSPIO_SCP_READ_DATA, 0);
		if (i < max)
			data[i] = w;
	}
	chip->scp_unsol_done++;
	dev_dbg(chip->dev, "scp reply hdr=0x%08x count=0x%x size=%u\n",
		*hdr, cnt, size);
	return min(size, max);
}

/* Read out (and discard) replies the DSP has announced but we haven't read. */
static void scp_drain_announced(struct creative_chip *chip)
{
	unsigned int guard = 16;
	u32 hdr, junk[SCP_MAX_DATA_WORDS];

	while (guard-- &&
	       (int)(atomic_read(&chip->scp_unsol) - chip->scp_unsol_done) > 0) {
		int n = scp_read_announced(chip, &hdr, junk, ARRAY_SIZE(junk));

		if (ae9_debug)
			dev_info_ratelimited(chip->dev,
			"scp: drained unexpected reply hdr=0x%08x (%d words: %08x %08x %08x %08x)\n",
			hdr, n, n > 0 ? junk[0] : 0, n > 1 ? junk[1] : 0,
			n > 2 ? junk[2] : 0, n > 3 ? junk[3] : 0);
	}
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
	mutex_unlock(&chip->scp_mutex);
	return ret;
}

static int dspio_scp_unlocked(struct creative_chip *chip, int mod_id,
			      int src_id, int req, int dir, const void *data,
			      unsigned int len, void *reply,
			      unsigned int *reply_len)
{
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
		unsigned int got;

		chip->dsp_wait_scp = 0;
		/* status polls run the RIRB scan that counts announcements */
		while ((int)(atomic_read(&chip->scp_unsol) - chip->scp_unsol_done) <= 0) {
			if (time_after(jiffies, timeout)) {
				dev_info_ratelimited(chip->dev,
					"scp GET mid=0x%02x req=0x%02x: no reply announced\n",
					mod_id, req);
				return -EIO;
			}
			if (creative_ae9_gone(chip, "SCP reply wait"))
				return -ENODEV;
			dsp_read(chip, WIDGET_DSP_CTRL, VENDOR_DSPIO_STATUS, 0);
			usleep_range(200, 400);
		}
		got = scp_read_announced(chip, &scp_reply.hdr, scp_reply.data,
					 SCP_MAX_DATA_WORDS);
		if ((scp_reply.hdr & 0x00ffffff) !=
		    (chip->dsp_wait_scp_header & 0x00ffffff))
			dev_info_ratelimited(chip->dev,
				"scp GET mid=0x%02x req=0x%02x: reply hdr 0x%08x (want 0x%06x)\n",
				mod_id, req, scp_reply.hdr, chip->dsp_wait_scp_header);
		extract_scp_header(scp_reply.hdr, &resp_flag, &error_flag,
				   &reply_data_size);
		if (!resp_flag || error_flag)
			return -EIO;
		if (reply && reply_len) {
			unsigned int rl = min(*reply_len,
					      got * (unsigned int)sizeof(u32));
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
		if (matched && scp_win_read && reply_data_size) {
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

/* Program and start the overlay stream over the prepared buffer. */
static void ovl_stream_run(struct creative_chip *chip, dma_addr_t bdl_dma)
{
	void __iomem *sd = chip->hda_regs + OVL_SD;

	writel(SD_CTL_STREAM_RESET, sd + AZX_REG_SD_CTL);
	udelay(50);
	writel(0, sd + AZX_REG_SD_CTL);
	udelay(50);
	writel(1 << 20, sd + AZX_REG_SD_CTL);		/* tag 1 */
	writel(OVL_BUF_BYTES, sd + AZX_REG_SD_CBL);
	writew(OVL_BDL_ENTRIES - 1, sd + AZX_REG_SD_LVI);
	writew(OVL_FMT, sd + AZX_REG_SD_FORMAT);
	writel(lower_32_bits(bdl_dma), sd + AZX_REG_SD_BDLPL);
	writel(upper_32_bits(bdl_dma), sd + AZX_REG_SD_BDLPU);
	wmb();
	writel((1 << 20) | SD_CTL_DMA_START, sd + AZX_REG_SD_CTL);
	wmb();
}

static void ovl_stream_stop(struct creative_chip *chip)
{
	void __iomem *sd = chip->hda_regs + OVL_SD;

	writel(1 << 20, sd + AZX_REG_SD_CTL);
	udelay(50);
	writel(SD_CTL_STREAM_RESET, sd + AZX_REG_SD_CTL);
	udelay(50);
	writel(0, sd + AZX_REG_SD_CTL);
	wmb();
}

static int dsp_overlay_xfer(struct creative_chip *chip, u32 *chan_out)
{
	const struct firmware *fw;
	const struct ae9_dsp_image_seg *seg;
	void *buf = NULL;
	__le32 *bdl = NULL;
	dma_addr_t buf_dma = 0, bdl_dma = 0;
	unsigned int port, i, segno = 0, rl;
	u32 chan = ~0U, resp;
	int ret;

	ret = request_firmware(&fw, AE9_OVERLAY_FILE, chip->dev);
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
		ovl_stream_run(chip, bdl_dma);
		for (t = 0; t < 200; t++) {
			creative_dsp_chipio_read(chip, DSPDMAC_CHNLSTART_INST_OFFSET, &v);
			if (!(v & 0xfff))
				break;
			usleep_range(1000, 2000);
		}
		ovl_stream_stop(chip);
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
			msleep(20);
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

static int dsp_reset(struct creative_chip *chip)
{
	unsigned int res;
	int retry = 20;

	do {
		res = dspio_send(chip, VENDOR_DSPIO_DSP_INIT, 0);
		retry--;
	} while (res == (unsigned int)-EIO && retry);
	return retry ? 0 : -EIO;
}

static int dsp_set_run_state(struct creative_chip *chip)
{
	unsigned int reg, halt_state;
	int err;

	err = creative_dsp_chipio_read(chip, DSP_DBGCNTL_INST_OFFSET, &reg);
	if (err < 0)
		return err;
	halt_state = (reg & DSP_DBGCNTL_STATE_MASK) >> DSP_DBGCNTL_STATE_LOBIT;
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

/* Public: kick the DSP out of halt (un-halt the 8051 so it runs its main
 * loop and consumes SCP commands). DBGCNTL STATE!=0 means halted. */
int creative_dsp_run(struct creative_chip *chip)
{
	unsigned int reg = 0;
	int err;

	err = creative_dsp_chipio_read(chip, DSP_DBGCNTL_INST_OFFSET, &reg);
	dev_info(chip->dev, "dsp_run: DBGCNTL=0x%x (state=0x%x) ret=%d\n",
		 reg, (reg & DSP_DBGCNTL_STATE_MASK) >> DSP_DBGCNTL_STATE_LOBIT,
		 err);
	err = dsp_set_run_state(chip);
	creative_dsp_chipio_read(chip, DSP_DBGCNTL_INST_OFFSET, &reg);
	dev_info(chip->dev, "dsp_run: after kick DBGCNTL=0x%x ret=%d\n",
		 reg, err);
	return err;
}

/*
 * Lightweight periodic DSP keepalive (called from the 500ms heartbeat).
 * Quietly checks the 8051 run state and un-halts it if it has fallen into a
 * halt state (DBGCNTL STATE != 0), which intermittently jams the SCP queue.
 * No logging unless it actually had to act. Sub-millisecond; safe to call
 * often. Does nothing if the DSP isn't loaded/responding.
 */
/* Public: flush the SCP command queue via DSPIO_DSP_INIT (0x703).
 * Use when SCP status reads 0x2 (queue full) and won't drain.
 */
int creative_dsp_scp_reset(struct creative_chip *chip)
{
	int ret = dsp_reset(chip);

	dev_info(chip->dev, "DSPIO reset (SCP flush): ret=%d\n", ret);
	return ret;
}

/*
 * hu_dsp_keepalive: 0 = off (default; Windows never touches DBGCNTL while
 * running), 1 = read DBGCNTL each heartbeat and force run state if its
 * state bits are set (old behaviour), 2 = read and log only. With the
 * Windows setup the old standup never runs, so the standup_done guard
 * below never applied and the heartbeat poked the DSP every 500 ms;
 * suspected of leaving it silent after a minute or two of idle.
 */
static unsigned int hu_dsp_keepalive;
module_param(hu_dsp_keepalive, uint, 0644);
MODULE_PARM_DESC(hu_dsp_keepalive, "Heartbeat DSP keepalive: 0=off (default), 1=un-halt (old), 2=log only");

void creative_dsp_keepalive(struct creative_chip *chip)
{
	static unsigned int last = ~0U;
	unsigned int reg = 0;
	int err;

	if (!chip->dsp_done || !hu_dsp_keepalive)
		return;

	/*
	 * Read DBGCNTL via ChipIO (sync CORB read). After HELLO ACK the 8051
	 * is in audio-loop mode and stops servicing CORB promptly, causing 1s
	 * timeouts. The 500ms heartbeat fires this every 500ms — 2 timeouts/s
	 * that block PCM prepare for several seconds.
	 *
	 * The keepalive's job is to un-halt the 8051 if it enters a halt state.
	 * In practice the 8051 does not halt during normal playback (DBGCNTL
	 * shows 0x4d0000 throughout). Skip the read entirely post-standup —
	 * the standup checkpoints (halt=0 at each stage) already confirm health,
	 * and any real halt would be caught by the self-reset mechanism on the
	 * next module load.
	 */
	if (chip->standup_done)
		return;

	err = creative_dsp_chipio_read(chip, DSP_DBGCNTL_INST_OFFSET, &reg);
	if (err < 0)
		return;

	if (reg != last) {
		dev_info(chip->dev, "dsp keepalive: DBGCNTL 0x%x -> 0x%x%s\n", last, reg,
			 (reg & DSP_DBGCNTL_STATE_MASK) ? " (state bits set)" : "");
		last = reg;
	}
	if ((reg & DSP_DBGCNTL_STATE_MASK) != 0 && hu_dsp_keepalive == 1) {
		dsp_set_run_state(chip);
		dev_info(chip->dev, "dsp keepalive: forced run state (DBGCNTL=0x%x)\n",
			 reg);
	}
}

/*
 * creative_dsp_audio_config — DSP effects engine pipeline configuration.
 *
 * 1183-verb NID16 DSPIO SCP sequence extracted byte-exact from coldboot
 * CORB log entries [4157-5339]. Windows sends this between select_out_params
 * and the start_trigger. It configures DSP module 0x96 (effects engine) with
 * gain coefficients, EQ, routing tables, sample rates, and enables the DSP
 * audio processing loop. Without it 0x801c stays zero permanently.
 *
 * NID breakdown:
 *   NID0x16 (DSPIO): 1133 verbs — DSP module 0x96 SCP configuration
 *   NID0x12:           12 verbs — DMA poll pairs (0x360/0x350)
 *   NID0x15 (ChipIO):  16 verbs — PARAM_EX + EXRAM access
 *   NID0x02/0x03/0x04: 22 verbs — audio widget configuration
 *
 * Split into 6 sub-arrays of ≤200 verbs to avoid compiler limits.
 * All sent under a single corb_mutex lock as a fire-and-forget burst.
 */
void creative_dsp_audio_config(struct creative_chip *chip)
{
	int i;

	if (!chip->standup_done || !chip->corb_buf)
		return;

	dev_warn(chip->dev, "dsp_audio_config: sending 1183-verb DSP pipeline config\n");

	mutex_lock(&chip->corb_mutex);

	/* Sub-array 1/6: verbs [0–199] (log [4157–4356]) */
	{
		static const u32 cfg[200] = {
			/* [   0] */ 0x11602032, /* NID16 SCP_LOW p=0x32 */
			/* [   1] */ 0x11610806, /* NID16 SCP_HIGH/DATA_HIGH req=0x06 */
			/* [   2] */ 0x116f0100, /* NID16 STATUS */
			/* [   3] */ 0x1160147b, /* NID16 v=0x014 p=0x7b */
			/* [   4] */ 0x1161c0a6, /* NID16 SCP_HIGH/DATA_HIGH req=0xa6 */
			/* [   5] */ 0x116f0100, /* NID16 STATUS */
			/* [   6] */ 0x11602032, /* NID16 SCP_LOW p=0x32 */
			/* [   7] */ 0x11610808, /* NID16 SCP_HIGH/DATA_HIGH req=0x08 */
			/* [   8] */ 0x116f0100, /* NID16 STATUS */
			/* [   9] */ 0x1160147b, /* NID16 v=0x014 p=0x7b */
			/* [  10] */ 0x1161c0a6, /* NID16 SCP_HIGH/DATA_HIGH req=0xa6 */
			/* [  11] */ 0x116f0100, /* NID16 STATUS */
			/* [  12] */ 0x11602032, /* NID16 SCP_LOW p=0x32 */
			/* [  13] */ 0x11610804, /* NID16 SCP_HIGH/DATA_HIGH req=0x04 */
			/* [  14] */ 0x116f0100, /* NID16 STATUS */
			/* [  15] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [  16] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [  17] */ 0x116f0100, /* NID16 STATUS */
			/* [  18] */ 0x10279800, /* NID02 v=0x798 p=0x00 */
			/* [  19] */ 0x1023905a, /* NID02 v=0x390 p=0x5a */
			/* [  20] */ 0x11602032, /* NID16 SCP_LOW p=0x32 */
			/* [  21] */ 0x11610806, /* NID16 SCP_HIGH/DATA_HIGH req=0x06 */
			/* [  22] */ 0x116f0100, /* NID16 STATUS */
			/* [  23] */ 0x1160147b, /* NID16 v=0x014 p=0x7b */
			/* [  24] */ 0x1161c0a6, /* NID16 SCP_HIGH/DATA_HIGH req=0xa6 */
			/* [  25] */ 0x116f0100, /* NID16 STATUS */
			/* [  26] */ 0x11602032, /* NID16 SCP_LOW p=0x32 */
			/* [  27] */ 0x11610808, /* NID16 SCP_HIGH/DATA_HIGH req=0x08 */
			/* [  28] */ 0x116f0100, /* NID16 STATUS */
			/* [  29] */ 0x1160147b, /* NID16 v=0x014 p=0x7b */
			/* [  30] */ 0x1161c0a6, /* NID16 SCP_HIGH/DATA_HIGH req=0xa6 */
			/* [  31] */ 0x116f0100, /* NID16 STATUS */
			/* [  32] */ 0x11602032, /* NID16 SCP_LOW p=0x32 */
			/* [  33] */ 0x11610804, /* NID16 SCP_HIGH/DATA_HIGH req=0x04 */
			/* [  34] */ 0x116f0100, /* NID16 STATUS */
			/* [  35] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [  36] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [  37] */ 0x116f0100, /* NID16 STATUS */
			/* [  38] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [  39] */ 0x11610842, /* NID16 SCP_HIGH/DATA_HIGH req=0x42 */
			/* [  40] */ 0x116f0100, /* NID16 STATUS */
			/* [  41] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [  42] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [  43] */ 0x10379700, /* NID03 v=0x797 p=0x00 */
			/* [  44] */ 0x116f0100, /* NID16 STATUS */
			/* [  45] */ 0x1033a05a, /* NID03 v=0x3a0 p=0x5a */
			/* [  46] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [  47] */ 0x11610862, /* NID16 SCP_HIGH/DATA_HIGH req=0x62 */
			/* [  48] */ 0x116f0100, /* NID16 STATUS */
			/* [  49] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [  50] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [  51] */ 0x116f0100, /* NID16 STATUS */
			/* [  52] */ 0x10379800, /* NID03 v=0x798 p=0x00 */
			/* [  53] */ 0x1033905a, /* NID03 v=0x390 p=0x5a */
			/* [  54] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [  55] */ 0x11610844, /* NID16 SCP_HIGH/DATA_HIGH req=0x44 */
			/* [  56] */ 0x116f0100, /* NID16 STATUS */
			/* [  57] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [  58] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [  59] */ 0x116f0100, /* NID16 STATUS */
			/* [  60] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [  61] */ 0x11610864, /* NID16 SCP_HIGH/DATA_HIGH req=0x64 */
			/* [  62] */ 0x116f0100, /* NID16 STATUS */
			/* [  63] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [  64] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [  65] */ 0x116f0100, /* NID16 STATUS */
			/* [  66] */ 0x10479700, /* NID04 v=0x797 p=0x00 */
			/* [  67] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [  68] */ 0x1043a05a, /* NID04 v=0x3a0 p=0x5a */
			/* [  69] */ 0x11610846, /* NID16 SCP_HIGH/DATA_HIGH req=0x46 */
			/* [  70] */ 0x116f0100, /* NID16 STATUS */
			/* [  71] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [  72] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [  73] */ 0x116f0100, /* NID16 STATUS */
			/* [  74] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [  75] */ 0x11610866, /* NID16 SCP_HIGH/DATA_HIGH req=0x66 */
			/* [  76] */ 0x116f0100, /* NID16 STATUS */
			/* [  77] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [  78] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [  79] */ 0x116f0100, /* NID16 STATUS */
			/* [  80] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [  81] */ 0x11610848, /* NID16 SCP_HIGH/DATA_HIGH req=0x48 */
			/* [  82] */ 0x116f0100, /* NID16 STATUS */
			/* [  83] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [  84] */ 0x10479800, /* NID04 v=0x798 p=0x00 */
			/* [  85] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [  86] */ 0x1043905a, /* NID04 v=0x390 p=0x5a */
			/* [  87] */ 0x116f0100, /* NID16 STATUS */
			/* [  88] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [  89] */ 0x11610868, /* NID16 SCP_HIGH/DATA_HIGH req=0x68 */
			/* [  90] */ 0x116f0100, /* NID16 STATUS */
			/* [  91] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [  92] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [  93] */ 0x116f0100, /* NID16 STATUS */
			/* [  94] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [  95] */ 0x1161084a, /* NID16 SCP_HIGH/DATA_HIGH req=0x4a */
			/* [  96] */ 0x116f0100, /* NID16 STATUS */
			/* [  97] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [  98] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [  99] */ 0x116f0100, /* NID16 STATUS */
			/* [ 100] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 101] */ 0x1161086a, /* NID16 SCP_HIGH/DATA_HIGH req=0x6a */
			/* [ 102] */ 0x116f0100, /* NID16 STATUS */
			/* [ 103] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 104] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 105] */ 0x116f0100, /* NID16 STATUS */
			/* [ 106] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 107] */ 0x1161084c, /* NID16 SCP_HIGH/DATA_HIGH req=0x4c */
			/* [ 108] */ 0x116f0100, /* NID16 STATUS */
			/* [ 109] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 110] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 111] */ 0x116f0100, /* NID16 STATUS */
			/* [ 112] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 113] */ 0x1161086c, /* NID16 SCP_HIGH/DATA_HIGH req=0x6c */
			/* [ 114] */ 0x116f0100, /* NID16 STATUS */
			/* [ 115] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 116] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 117] */ 0x116f0100, /* NID16 STATUS */
			/* [ 118] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 119] */ 0x1161084e, /* NID16 SCP_HIGH/DATA_HIGH req=0x4e */
			/* [ 120] */ 0x116f0100, /* NID16 STATUS */
			/* [ 121] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 122] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 123] */ 0x116f0100, /* NID16 STATUS */
			/* [ 124] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 125] */ 0x116f0100, /* NID16 STATUS */
			/* [ 126] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 127] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 128] */ 0x116f0100, /* NID16 STATUS */
			/* [ 129] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 130] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 131] */ 0x116f0100, /* NID16 STATUS */
			/* [ 132] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 133] */ 0x1161086e, /* NID16 SCP_HIGH/DATA_HIGH req=0x6e */
			/* [ 134] */ 0x116f0100, /* NID16 STATUS */
			/* [ 135] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 136] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 137] */ 0x116f0100, /* NID16 STATUS */
			/* [ 138] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 139] */ 0x116f0100, /* NID16 STATUS */
			/* [ 140] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 141] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 142] */ 0x116f0100, /* NID16 STATUS */
			/* [ 143] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 144] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 145] */ 0x116f0100, /* NID16 STATUS */
			/* [ 146] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 147] */ 0x11610850, /* NID16 SCP_HIGH/DATA_HIGH req=0x50 */
			/* [ 148] */ 0x116f0100, /* NID16 STATUS */
			/* [ 149] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 150] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 151] */ 0x116f0100, /* NID16 STATUS */
			/* [ 152] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 153] */ 0x116f0100, /* NID16 STATUS */
			/* [ 154] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 155] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 156] */ 0x116f0100, /* NID16 STATUS */
			/* [ 157] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 158] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 159] */ 0x116f0100, /* NID16 STATUS */
			/* [ 160] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 161] */ 0x11610870, /* NID16 SCP_HIGH/DATA_HIGH req=0x70 */
			/* [ 162] */ 0x116f0100, /* NID16 STATUS */
			/* [ 163] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 164] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 165] */ 0x116f0100, /* NID16 STATUS */
			/* [ 166] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 167] */ 0x116f0100, /* NID16 STATUS */
			/* [ 168] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 169] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 170] */ 0x116f0100, /* NID16 STATUS */
			/* [ 171] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 172] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 173] */ 0x116f0100, /* NID16 STATUS */
			/* [ 174] */ 0x11602080, /* NID16 SCP_LOW p=0x80 */
			/* [ 175] */ 0x11610808, /* NID16 SCP_HIGH/DATA_HIGH req=0x08 */
			/* [ 176] */ 0x116f0100, /* NID16 STATUS */
			/* [ 177] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 178] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 179] */ 0x116f0100, /* NID16 STATUS */
			/* [ 180] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 181] */ 0x11610832, /* NID16 SCP_HIGH/DATA_HIGH req=0x32 */
			/* [ 182] */ 0x116f0100, /* NID16 STATUS */
			/* [ 183] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 184] */ 0x11613e80, /* NID16 SCP_HIGH/DATA_HIGH req=0x80 */
			/* [ 185] */ 0x116f0100, /* NID16 STATUS */
			/* [ 186] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 187] */ 0x1161082e, /* NID16 SCP_HIGH/DATA_HIGH req=0x2e */
			/* [ 188] */ 0x116f0100, /* NID16 STATUS */
			/* [ 189] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 190] */ 0x116142a0, /* NID16 SCP_HIGH/DATA_HIGH req=0xa0 */
			/* [ 191] */ 0x116f0100, /* NID16 STATUS */
			/* [ 192] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 193] */ 0x11610830, /* NID16 SCP_HIGH/DATA_HIGH req=0x30 */
			/* [ 194] */ 0x116f0100, /* NID16 STATUS */
			/* [ 195] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 196] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 197] */ 0x116f0100, /* NID16 STATUS */
			/* [ 198] */ 0x1160208f, /* NID16 SCP_LOW p=0x8f */
			/* [ 199] */ 0x11610802  /* NID16 SCP_HIGH/DATA_HIGH req=0x02 */
		};
		for (i = 0; i < ARRAY_SIZE(cfg); i++)
			creative_ae9_corb_send_verb_wait(chip, cfg[i]);
	}

	/* Give 8051 time to drain previous 1 chunks before next burst */
	msleep(15);

	/* Sub-array 2/6: verbs [200–399] (log [4357–4556]) */
	{
		static const u32 cfg[200] = {
			/* [ 200] */ 0x116f0100, /* NID16 STATUS */
			/* [ 201] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 202] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 203] */ 0x116f0100, /* NID16 STATUS */
			/* [ 204] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 205] */ 0x1161083e, /* NID16 SCP_HIGH/DATA_HIGH req=0x3e */
			/* [ 206] */ 0x116f0100, /* NID16 STATUS */
			/* [ 207] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 208] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 209] */ 0x116f0100, /* NID16 STATUS */
			/* [ 210] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 211] */ 0x1161082a, /* NID16 SCP_HIGH/DATA_HIGH req=0x2a */
			/* [ 212] */ 0x116f0100, /* NID16 STATUS */
			/* [ 213] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 214] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 215] */ 0x116f0100, /* NID16 STATUS */
			/* [ 216] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 217] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [ 218] */ 0x116f0100, /* NID16 STATUS */
			/* [ 219] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 220] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 221] */ 0x116f0100, /* NID16 STATUS */
			/* [ 222] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 223] */ 0x11610875, /* NID16 SCP_HIGH/DATA_HIGH req=0x75 */
			/* [ 224] */ 0x116f0100, /* NID16 STATUS */
			/* [ 225] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 226] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 227] */ 0x116f0100, /* NID16 STATUS */
			/* [ 228] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 229] */ 0x116f0100, /* NID16 STATUS */
			/* [ 230] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 231] */ 0x11670402, /* NID16 PORT_ALLOC p=0x02 */
			/* [ 232] */ 0x116f0100, /* NID16 STATUS */
			/* [ 233] */ 0x116f0400, /* NID16 GET_ALLOC */
			/* [ 234] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 235] */ 0x116f0100, /* NID16 STATUS */
			/* [ 236] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 237] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 238] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [ 239] */ 0x116f0100, /* NID16 STATUS */
			/* [ 240] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 241] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 242] */ 0x116f0100, /* NID16 STATUS */
			/* [ 243] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 244] */ 0x11610875, /* NID16 SCP_HIGH/DATA_HIGH req=0x75 */
			/* [ 245] */ 0x116f0100, /* NID16 STATUS */
			/* [ 246] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 247] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 248] */ 0x116f0100, /* NID16 STATUS */
			/* [ 249] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 250] */ 0x116f0100, /* NID16 STATUS */
			/* [ 251] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 252] */ 0x11670402, /* NID16 PORT_ALLOC p=0x02 */
			/* [ 253] */ 0x116f0100, /* NID16 STATUS */
			/* [ 254] */ 0x116f0400, /* NID16 GET_ALLOC */
			/* [ 255] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 256] */ 0x116f0100, /* NID16 STATUS */
			/* [ 257] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 258] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 259] */ 0x11610802, /* NID16 SCP_HIGH/DATA_HIGH req=0x02 */
			/* [ 260] */ 0x116f0100, /* NID16 STATUS */
			/* [ 261] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 262] */ 0x11613f00, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 263] */ 0x116f0100, /* NID16 STATUS */
			/* [ 264] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 265] */ 0x11610810, /* NID16 SCP_HIGH/DATA_HIGH req=0x10 */
			/* [ 266] */ 0x116f0100, /* NID16 STATUS */
			/* [ 267] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 268] */ 0x11613f40, /* NID16 SCP_HIGH/DATA_HIGH req=0x40 */
			/* [ 269] */ 0x116f0100, /* NID16 STATUS */
			/* [ 270] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 271] */ 0x1161080a, /* NID16 SCP_HIGH/DATA_HIGH req=0x0a */
			/* [ 272] */ 0x116f0100, /* NID16 STATUS */
			/* [ 273] */ 0x1160851f, /* NID16 v=0x085 p=0x1f */
			/* [ 274] */ 0x11613eeb, /* NID16 SCP_HIGH/DATA_HIGH req=0xeb */
			/* [ 275] */ 0x116f0100, /* NID16 STATUS */
			/* [ 276] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 277] */ 0x11610806, /* NID16 SCP_HIGH/DATA_HIGH req=0x06 */
			/* [ 278] */ 0x116f0100, /* NID16 STATUS */
			/* [ 279] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 280] */ 0x11613f00, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 281] */ 0x116f0100, /* NID16 STATUS */
			/* [ 282] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 283] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [ 284] */ 0x116f0100, /* NID16 STATUS */
			/* [ 285] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 286] */ 0x11613f80, /* NID16 SCP_HIGH/DATA_HIGH req=0x80 */
			/* [ 287] */ 0x116f0100, /* NID16 STATUS */
			/* [ 288] */ 0x115f0100, /* NID15 STATUS */
			/* [ 289] */ 0x1157170d, /* NID15 PARAM_EX_ID=0x0d */
			/* [ 290] */ 0x115718a1, /* NID15 PARAM_EX_VAL=0xa1 */
			/* [ 291] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 292] */ 0x11610842, /* NID16 SCP_HIGH/DATA_HIGH req=0x42 */
			/* [ 293] */ 0x116f0100, /* NID16 STATUS */
			/* [ 294] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 295] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 296] */ 0x116f0100, /* NID16 STATUS */
			/* [ 297] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 298] */ 0x11610862, /* NID16 SCP_HIGH/DATA_HIGH req=0x62 */
			/* [ 299] */ 0x116f0100, /* NID16 STATUS */
			/* [ 300] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 301] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 302] */ 0x116f0100, /* NID16 STATUS */
			/* [ 303] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 304] */ 0x11610844, /* NID16 SCP_HIGH/DATA_HIGH req=0x44 */
			/* [ 305] */ 0x116f0100, /* NID16 STATUS */
			/* [ 306] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 307] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 308] */ 0x116f0100, /* NID16 STATUS */
			/* [ 309] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 310] */ 0x11610864, /* NID16 SCP_HIGH/DATA_HIGH req=0x64 */
			/* [ 311] */ 0x116f0100, /* NID16 STATUS */
			/* [ 312] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 313] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 314] */ 0x116f0100, /* NID16 STATUS */
			/* [ 315] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 316] */ 0x11610846, /* NID16 SCP_HIGH/DATA_HIGH req=0x46 */
			/* [ 317] */ 0x116f0100, /* NID16 STATUS */
			/* [ 318] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 319] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 320] */ 0x116f0100, /* NID16 STATUS */
			/* [ 321] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 322] */ 0x11610866, /* NID16 SCP_HIGH/DATA_HIGH req=0x66 */
			/* [ 323] */ 0x116f0100, /* NID16 STATUS */
			/* [ 324] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 325] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 326] */ 0x116f0100, /* NID16 STATUS */
			/* [ 327] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 328] */ 0x11610848, /* NID16 SCP_HIGH/DATA_HIGH req=0x48 */
			/* [ 329] */ 0x116f0100, /* NID16 STATUS */
			/* [ 330] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 331] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 332] */ 0x116f0100, /* NID16 STATUS */
			/* [ 333] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 334] */ 0x11610868, /* NID16 SCP_HIGH/DATA_HIGH req=0x68 */
			/* [ 335] */ 0x116f0100, /* NID16 STATUS */
			/* [ 336] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 337] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 338] */ 0x116f0100, /* NID16 STATUS */
			/* [ 339] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 340] */ 0x1161084a, /* NID16 SCP_HIGH/DATA_HIGH req=0x4a */
			/* [ 341] */ 0x116f0100, /* NID16 STATUS */
			/* [ 342] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 343] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 344] */ 0x116f0100, /* NID16 STATUS */
			/* [ 345] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 346] */ 0x1161086a, /* NID16 SCP_HIGH/DATA_HIGH req=0x6a */
			/* [ 347] */ 0x116f0100, /* NID16 STATUS */
			/* [ 348] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 349] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 350] */ 0x116f0100, /* NID16 STATUS */
			/* [ 351] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 352] */ 0x1161084c, /* NID16 SCP_HIGH/DATA_HIGH req=0x4c */
			/* [ 353] */ 0x116f0100, /* NID16 STATUS */
			/* [ 354] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 355] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 356] */ 0x116f0100, /* NID16 STATUS */
			/* [ 357] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 358] */ 0x1161086c, /* NID16 SCP_HIGH/DATA_HIGH req=0x6c */
			/* [ 359] */ 0x116f0100, /* NID16 STATUS */
			/* [ 360] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 361] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 362] */ 0x116f0100, /* NID16 STATUS */
			/* [ 363] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 364] */ 0x1161084e, /* NID16 SCP_HIGH/DATA_HIGH req=0x4e */
			/* [ 365] */ 0x116f0100, /* NID16 STATUS */
			/* [ 366] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 367] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 368] */ 0x116f0100, /* NID16 STATUS */
			/* [ 369] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 370] */ 0x116f0100, /* NID16 STATUS */
			/* [ 371] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 372] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 373] */ 0x116f0100, /* NID16 STATUS */
			/* [ 374] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 375] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 376] */ 0x116f0100, /* NID16 STATUS */
			/* [ 377] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 378] */ 0x1161086e, /* NID16 SCP_HIGH/DATA_HIGH req=0x6e */
			/* [ 379] */ 0x116f0100, /* NID16 STATUS */
			/* [ 380] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 381] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 382] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 383] */ 0x116f0100, /* NID16 STATUS */
			/* [ 384] */ 0x116f0100, /* NID16 STATUS */
			/* [ 385] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 386] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 387] */ 0x116f0100, /* NID16 STATUS */
			/* [ 388] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 389] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 390] */ 0x116f0100, /* NID16 STATUS */
			/* [ 391] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 392] */ 0x11610850, /* NID16 SCP_HIGH/DATA_HIGH req=0x50 */
			/* [ 393] */ 0x116f0100, /* NID16 STATUS */
			/* [ 394] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 395] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 396] */ 0x116f0100, /* NID16 STATUS */
			/* [ 397] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 398] */ 0x116f0100, /* NID16 STATUS */
			/* [ 399] */ 0x116f0200  /* NID16 GET_PORT */
		};
		for (i = 0; i < ARRAY_SIZE(cfg); i++)
			creative_ae9_corb_send_verb_wait(chip, cfg[i]);
	}

	/* Give 8051 time to drain previous 2 chunks before next burst */
	msleep(15);

	/* Sub-array 3/6: verbs [400–599] (log [4557–4756]) */
	{
		static const u32 cfg[200] = {
			/* [ 400] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 401] */ 0x116f0100, /* NID16 STATUS */
			/* [ 402] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 403] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 404] */ 0x116f0100, /* NID16 STATUS */
			/* [ 405] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 406] */ 0x11610870, /* NID16 SCP_HIGH/DATA_HIGH req=0x70 */
			/* [ 407] */ 0x116f0100, /* NID16 STATUS */
			/* [ 408] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 409] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 410] */ 0x116f0100, /* NID16 STATUS */
			/* [ 411] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 412] */ 0x116f0100, /* NID16 STATUS */
			/* [ 413] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 414] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 415] */ 0x116f0100, /* NID16 STATUS */
			/* [ 416] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 417] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 418] */ 0x116f0100, /* NID16 STATUS */
			/* [ 419] */ 0x11602080, /* NID16 SCP_LOW p=0x80 */
			/* [ 420] */ 0x11610808, /* NID16 SCP_HIGH/DATA_HIGH req=0x08 */
			/* [ 421] */ 0x116f0100, /* NID16 STATUS */
			/* [ 422] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 423] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 424] */ 0x116f0100, /* NID16 STATUS */
			/* [ 425] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 426] */ 0x11610832, /* NID16 SCP_HIGH/DATA_HIGH req=0x32 */
			/* [ 427] */ 0x116f0100, /* NID16 STATUS */
			/* [ 428] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 429] */ 0x11613e80, /* NID16 SCP_HIGH/DATA_HIGH req=0x80 */
			/* [ 430] */ 0x116f0100, /* NID16 STATUS */
			/* [ 431] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 432] */ 0x1161082e, /* NID16 SCP_HIGH/DATA_HIGH req=0x2e */
			/* [ 433] */ 0x116f0100, /* NID16 STATUS */
			/* [ 434] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 435] */ 0x116142a0, /* NID16 SCP_HIGH/DATA_HIGH req=0xa0 */
			/* [ 436] */ 0x116f0100, /* NID16 STATUS */
			/* [ 437] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 438] */ 0x11610830, /* NID16 SCP_HIGH/DATA_HIGH req=0x30 */
			/* [ 439] */ 0x116f0100, /* NID16 STATUS */
			/* [ 440] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 441] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 442] */ 0x116f0100, /* NID16 STATUS */
			/* [ 443] */ 0x1160208f, /* NID16 SCP_LOW p=0x8f */
			/* [ 444] */ 0x11610802, /* NID16 SCP_HIGH/DATA_HIGH req=0x02 */
			/* [ 445] */ 0x116f0100, /* NID16 STATUS */
			/* [ 446] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 447] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 448] */ 0x116f0100, /* NID16 STATUS */
			/* [ 449] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 450] */ 0x1161083e, /* NID16 SCP_HIGH/DATA_HIGH req=0x3e */
			/* [ 451] */ 0x116f0100, /* NID16 STATUS */
			/* [ 452] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 453] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 454] */ 0x116f0100, /* NID16 STATUS */
			/* [ 455] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 456] */ 0x1161082a, /* NID16 SCP_HIGH/DATA_HIGH req=0x2a */
			/* [ 457] */ 0x116f0100, /* NID16 STATUS */
			/* [ 458] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 459] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 460] */ 0x116f0100, /* NID16 STATUS */
			/* [ 461] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 462] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [ 463] */ 0x116f0100, /* NID16 STATUS */
			/* [ 464] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 465] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 466] */ 0x116f0100, /* NID16 STATUS */
			/* [ 467] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 468] */ 0x11610875, /* NID16 SCP_HIGH/DATA_HIGH req=0x75 */
			/* [ 469] */ 0x116f0100, /* NID16 STATUS */
			/* [ 470] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 471] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 472] */ 0x116f0100, /* NID16 STATUS */
			/* [ 473] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 474] */ 0x116f0100, /* NID16 STATUS */
			/* [ 475] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 476] */ 0x11670402, /* NID16 PORT_ALLOC p=0x02 */
			/* [ 477] */ 0x116f0100, /* NID16 STATUS */
			/* [ 478] */ 0x116f0400, /* NID16 GET_ALLOC */
			/* [ 479] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 480] */ 0x116f0100, /* NID16 STATUS */
			/* [ 481] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 482] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 483] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [ 484] */ 0x116f0100, /* NID16 STATUS */
			/* [ 485] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 486] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 487] */ 0x116f0100, /* NID16 STATUS */
			/* [ 488] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 489] */ 0x11610875, /* NID16 SCP_HIGH/DATA_HIGH req=0x75 */
			/* [ 490] */ 0x116f0100, /* NID16 STATUS */
			/* [ 491] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 492] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 493] */ 0x116f0100, /* NID16 STATUS */
			/* [ 494] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 495] */ 0x116f0100, /* NID16 STATUS */
			/* [ 496] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 497] */ 0x11670402, /* NID16 PORT_ALLOC p=0x02 */
			/* [ 498] */ 0x116f0100, /* NID16 STATUS */
			/* [ 499] */ 0x116f0400, /* NID16 GET_ALLOC */
			/* [ 500] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 501] */ 0x116f0100, /* NID16 STATUS */
			/* [ 502] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 503] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 504] */ 0x11610800, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 505] */ 0x116f0100, /* NID16 STATUS */
			/* [ 506] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 507] */ 0x11613f80, /* NID16 SCP_HIGH/DATA_HIGH req=0x80 */
			/* [ 508] */ 0x116f0100, /* NID16 STATUS */
			/* [ 509] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 510] */ 0x11610804, /* NID16 SCP_HIGH/DATA_HIGH req=0x04 */
			/* [ 511] */ 0x116f0100, /* NID16 STATUS */
			/* [ 512] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 513] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 514] */ 0x116f0100, /* NID16 STATUS */
			/* [ 515] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 516] */ 0x11610808, /* NID16 SCP_HIGH/DATA_HIGH req=0x08 */
			/* [ 517] */ 0x116f0100, /* NID16 STATUS */
			/* [ 518] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 519] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 520] */ 0x116f0100, /* NID16 STATUS */
			/* [ 521] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 522] */ 0x1161080e, /* NID16 SCP_HIGH/DATA_HIGH req=0x0e */
			/* [ 523] */ 0x116f0100, /* NID16 STATUS */
			/* [ 524] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 525] */ 0x11613f80, /* NID16 SCP_HIGH/DATA_HIGH req=0x80 */
			/* [ 526] */ 0x116f0100, /* NID16 STATUS */
			/* [ 527] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 528] */ 0x11610830, /* NID16 SCP_HIGH/DATA_HIGH req=0x30 */
			/* [ 529] */ 0x116f0100, /* NID16 STATUS */
			/* [ 530] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 531] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 532] */ 0x116f0100, /* NID16 STATUS */
			/* [ 533] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 534] */ 0x11610800, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 535] */ 0x116f0100, /* NID16 STATUS */
			/* [ 536] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 537] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 538] */ 0x116f0100, /* NID16 STATUS */
			/* [ 539] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 540] */ 0x1161080e, /* NID16 SCP_HIGH/DATA_HIGH req=0x0e */
			/* [ 541] */ 0x116f0100, /* NID16 STATUS */
			/* [ 542] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 543] */ 0x11613f80, /* NID16 SCP_HIGH/DATA_HIGH req=0x80 */
			/* [ 544] */ 0x116f0100, /* NID16 STATUS */
			/* [ 545] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 546] */ 0x11610830, /* NID16 SCP_HIGH/DATA_HIGH req=0x30 */
			/* [ 547] */ 0x116f0100, /* NID16 STATUS */
			/* [ 548] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 549] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 550] */ 0x116f0100, /* NID16 STATUS */
			/* [ 551] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 552] */ 0x11610808, /* NID16 SCP_HIGH/DATA_HIGH req=0x08 */
			/* [ 553] */ 0x116f0100, /* NID16 STATUS */
			/* [ 554] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 555] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 556] */ 0x116f0100, /* NID16 STATUS */
			/* [ 557] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 558] */ 0x11610804, /* NID16 SCP_HIGH/DATA_HIGH req=0x04 */
			/* [ 559] */ 0x116f0100, /* NID16 STATUS */
			/* [ 560] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 561] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 562] */ 0x116f0100, /* NID16 STATUS */
			/* [ 563] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 564] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [ 565] */ 0x116f0100, /* NID16 STATUS */
			/* [ 566] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 567] */ 0x11613f80, /* NID16 SCP_HIGH/DATA_HIGH req=0x80 */
			/* [ 568] */ 0x116f0100, /* NID16 STATUS */
			/* [ 569] */ 0x115f0100, /* NID15 STATUS */
			/* [ 570] */ 0x1157170d, /* NID15 PARAM_EX_ID=0x0d */
			/* [ 571] */ 0x115718a1, /* NID15 PARAM_EX_VAL=0xa1 */
			/* [ 572] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 573] */ 0x11610842, /* NID16 SCP_HIGH/DATA_HIGH req=0x42 */
			/* [ 574] */ 0x116f0100, /* NID16 STATUS */
			/* [ 575] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 576] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 577] */ 0x116f0100, /* NID16 STATUS */
			/* [ 578] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 579] */ 0x11610862, /* NID16 SCP_HIGH/DATA_HIGH req=0x62 */
			/* [ 580] */ 0x116f0100, /* NID16 STATUS */
			/* [ 581] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 582] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 583] */ 0x116f0100, /* NID16 STATUS */
			/* [ 584] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 585] */ 0x11610844, /* NID16 SCP_HIGH/DATA_HIGH req=0x44 */
			/* [ 586] */ 0x116f0100, /* NID16 STATUS */
			/* [ 587] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 588] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 589] */ 0x116f0100, /* NID16 STATUS */
			/* [ 590] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 591] */ 0x11610864, /* NID16 SCP_HIGH/DATA_HIGH req=0x64 */
			/* [ 592] */ 0x116f0100, /* NID16 STATUS */
			/* [ 593] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 594] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 595] */ 0x116f0100, /* NID16 STATUS */
			/* [ 596] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 597] */ 0x11610846, /* NID16 SCP_HIGH/DATA_HIGH req=0x46 */
			/* [ 598] */ 0x116f0100, /* NID16 STATUS */
			/* [ 599] */ 0x11600000  /* NID16 DATA_LOW p=0x00 */
		};
		for (i = 0; i < ARRAY_SIZE(cfg); i++)
			creative_ae9_corb_send_verb_wait(chip, cfg[i]);
	}

	/* Give 8051 time to drain previous 3 chunks before next burst */
	msleep(15);

	/* Sub-array 4/6: verbs [600–799] (log [4757–4956]) */
	{
		static const u32 cfg[200] = {
			/* [ 600] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 601] */ 0x116f0100, /* NID16 STATUS */
			/* [ 602] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 603] */ 0x11610866, /* NID16 SCP_HIGH/DATA_HIGH req=0x66 */
			/* [ 604] */ 0x116f0100, /* NID16 STATUS */
			/* [ 605] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 606] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 607] */ 0x116f0100, /* NID16 STATUS */
			/* [ 608] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 609] */ 0x11610848, /* NID16 SCP_HIGH/DATA_HIGH req=0x48 */
			/* [ 610] */ 0x116f0100, /* NID16 STATUS */
			/* [ 611] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 612] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 613] */ 0x116f0100, /* NID16 STATUS */
			/* [ 614] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 615] */ 0x11610868, /* NID16 SCP_HIGH/DATA_HIGH req=0x68 */
			/* [ 616] */ 0x116f0100, /* NID16 STATUS */
			/* [ 617] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 618] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 619] */ 0x116f0100, /* NID16 STATUS */
			/* [ 620] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 621] */ 0x1161084a, /* NID16 SCP_HIGH/DATA_HIGH req=0x4a */
			/* [ 622] */ 0x116f0100, /* NID16 STATUS */
			/* [ 623] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 624] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 625] */ 0x116f0100, /* NID16 STATUS */
			/* [ 626] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 627] */ 0x1161086a, /* NID16 SCP_HIGH/DATA_HIGH req=0x6a */
			/* [ 628] */ 0x116f0100, /* NID16 STATUS */
			/* [ 629] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 630] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 631] */ 0x116f0100, /* NID16 STATUS */
			/* [ 632] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 633] */ 0x1161084c, /* NID16 SCP_HIGH/DATA_HIGH req=0x4c */
			/* [ 634] */ 0x116f0100, /* NID16 STATUS */
			/* [ 635] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 636] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 637] */ 0x116f0100, /* NID16 STATUS */
			/* [ 638] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 639] */ 0x1161086c, /* NID16 SCP_HIGH/DATA_HIGH req=0x6c */
			/* [ 640] */ 0x116f0100, /* NID16 STATUS */
			/* [ 641] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 642] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 643] */ 0x116f0100, /* NID16 STATUS */
			/* [ 644] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 645] */ 0x1161084e, /* NID16 SCP_HIGH/DATA_HIGH req=0x4e */
			/* [ 646] */ 0x116f0100, /* NID16 STATUS */
			/* [ 647] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 648] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 649] */ 0x116f0100, /* NID16 STATUS */
			/* [ 650] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 651] */ 0x116f0100, /* NID16 STATUS */
			/* [ 652] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 653] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 654] */ 0x116f0100, /* NID16 STATUS */
			/* [ 655] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 656] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 657] */ 0x116f0100, /* NID16 STATUS */
			/* [ 658] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 659] */ 0x1161086e, /* NID16 SCP_HIGH/DATA_HIGH req=0x6e */
			/* [ 660] */ 0x116f0100, /* NID16 STATUS */
			/* [ 661] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 662] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 663] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 664] */ 0x116f0100, /* NID16 STATUS */
			/* [ 665] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 666] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 667] */ 0x116f0100, /* NID16 STATUS */
			/* [ 668] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 669] */ 0x116f0100, /* NID16 STATUS */
			/* [ 670] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 671] */ 0x116f0100, /* NID16 STATUS */
			/* [ 672] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 673] */ 0x11610850, /* NID16 SCP_HIGH/DATA_HIGH req=0x50 */
			/* [ 674] */ 0x116f0100, /* NID16 STATUS */
			/* [ 675] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 676] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 677] */ 0x116f0100, /* NID16 STATUS */
			/* [ 678] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 679] */ 0x116f0100, /* NID16 STATUS */
			/* [ 680] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 681] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 682] */ 0x116f0100, /* NID16 STATUS */
			/* [ 683] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 684] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 685] */ 0x116f0100, /* NID16 STATUS */
			/* [ 686] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 687] */ 0x11610870, /* NID16 SCP_HIGH/DATA_HIGH req=0x70 */
			/* [ 688] */ 0x116f0100, /* NID16 STATUS */
			/* [ 689] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 690] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 691] */ 0x116f0100, /* NID16 STATUS */
			/* [ 692] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 693] */ 0x116f0100, /* NID16 STATUS */
			/* [ 694] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 695] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 696] */ 0x116f0100, /* NID16 STATUS */
			/* [ 697] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 698] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 699] */ 0x116f0100, /* NID16 STATUS */
			/* [ 700] */ 0x11602080, /* NID16 SCP_LOW p=0x80 */
			/* [ 701] */ 0x11610808, /* NID16 SCP_HIGH/DATA_HIGH req=0x08 */
			/* [ 702] */ 0x116f0100, /* NID16 STATUS */
			/* [ 703] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 704] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 705] */ 0x116f0100, /* NID16 STATUS */
			/* [ 706] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 707] */ 0x11610832, /* NID16 SCP_HIGH/DATA_HIGH req=0x32 */
			/* [ 708] */ 0x116f0100, /* NID16 STATUS */
			/* [ 709] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 710] */ 0x11613e80, /* NID16 SCP_HIGH/DATA_HIGH req=0x80 */
			/* [ 711] */ 0x116f0100, /* NID16 STATUS */
			/* [ 712] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 713] */ 0x1161082e, /* NID16 SCP_HIGH/DATA_HIGH req=0x2e */
			/* [ 714] */ 0x116f0100, /* NID16 STATUS */
			/* [ 715] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 716] */ 0x116142a0, /* NID16 SCP_HIGH/DATA_HIGH req=0xa0 */
			/* [ 717] */ 0x116f0100, /* NID16 STATUS */
			/* [ 718] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 719] */ 0x11610830, /* NID16 SCP_HIGH/DATA_HIGH req=0x30 */
			/* [ 720] */ 0x116f0100, /* NID16 STATUS */
			/* [ 721] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 722] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 723] */ 0x116f0100, /* NID16 STATUS */
			/* [ 724] */ 0x1160208f, /* NID16 SCP_LOW p=0x8f */
			/* [ 725] */ 0x11610802, /* NID16 SCP_HIGH/DATA_HIGH req=0x02 */
			/* [ 726] */ 0x116f0100, /* NID16 STATUS */
			/* [ 727] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 728] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 729] */ 0x116f0100, /* NID16 STATUS */
			/* [ 730] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 731] */ 0x1161083e, /* NID16 SCP_HIGH/DATA_HIGH req=0x3e */
			/* [ 732] */ 0x116f0100, /* NID16 STATUS */
			/* [ 733] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 734] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 735] */ 0x116f0100, /* NID16 STATUS */
			/* [ 736] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 737] */ 0x1161082a, /* NID16 SCP_HIGH/DATA_HIGH req=0x2a */
			/* [ 738] */ 0x116f0100, /* NID16 STATUS */
			/* [ 739] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 740] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 741] */ 0x116f0100, /* NID16 STATUS */
			/* [ 742] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 743] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [ 744] */ 0x116f0100, /* NID16 STATUS */
			/* [ 745] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 746] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 747] */ 0x116f0100, /* NID16 STATUS */
			/* [ 748] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 749] */ 0x11610875, /* NID16 SCP_HIGH/DATA_HIGH req=0x75 */
			/* [ 750] */ 0x116f0100, /* NID16 STATUS */
			/* [ 751] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 752] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 753] */ 0x116f0100, /* NID16 STATUS */
			/* [ 754] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 755] */ 0x116f0100, /* NID16 STATUS */
			/* [ 756] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 757] */ 0x11670402, /* NID16 PORT_ALLOC p=0x02 */
			/* [ 758] */ 0x116f0100, /* NID16 STATUS */
			/* [ 759] */ 0x116f0400, /* NID16 GET_ALLOC */
			/* [ 760] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 761] */ 0x116f0100, /* NID16 STATUS */
			/* [ 762] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 763] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 764] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [ 765] */ 0x116f0100, /* NID16 STATUS */
			/* [ 766] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 767] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 768] */ 0x116f0100, /* NID16 STATUS */
			/* [ 769] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 770] */ 0x11610875, /* NID16 SCP_HIGH/DATA_HIGH req=0x75 */
			/* [ 771] */ 0x116f0100, /* NID16 STATUS */
			/* [ 772] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 773] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 774] */ 0x116f0100, /* NID16 STATUS */
			/* [ 775] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 776] */ 0x116f0100, /* NID16 STATUS */
			/* [ 777] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 778] */ 0x11670402, /* NID16 PORT_ALLOC p=0x02 */
			/* [ 779] */ 0x116f0100, /* NID16 STATUS */
			/* [ 780] */ 0x116f0400, /* NID16 GET_ALLOC */
			/* [ 781] */ 0x11670200, /* NID16 PORT_FREE */
			/* [ 782] */ 0x116f0100, /* NID16 STATUS */
			/* [ 783] */ 0x116f0200, /* NID16 GET_PORT */
			/* [ 784] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 785] */ 0x11610800, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 786] */ 0x116f0100, /* NID16 STATUS */
			/* [ 787] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 788] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 789] */ 0x116f0100, /* NID16 STATUS */
			/* [ 790] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 791] */ 0x11610804, /* NID16 SCP_HIGH/DATA_HIGH req=0x04 */
			/* [ 792] */ 0x116f0100, /* NID16 STATUS */
			/* [ 793] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 794] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 795] */ 0x116f0100, /* NID16 STATUS */
			/* [ 796] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 797] */ 0x11610808, /* NID16 SCP_HIGH/DATA_HIGH req=0x08 */
			/* [ 798] */ 0x116f0100, /* NID16 STATUS */
			/* [ 799] */ 0x11600000  /* NID16 DATA_LOW p=0x00 */
		};
		for (i = 0; i < ARRAY_SIZE(cfg); i++)
			creative_ae9_corb_send_verb_wait(chip, cfg[i]);
	}

	/* Give 8051 time to drain previous 4 chunks before next burst */
	msleep(15);

	/* Sub-array 5/6: verbs [800–999] (log [4957–5156]) */
	{
		static const u32 cfg[200] = {
			/* [ 800] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 801] */ 0x116f0100, /* NID16 STATUS */
			/* [ 802] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 803] */ 0x1161080e, /* NID16 SCP_HIGH/DATA_HIGH req=0x0e */
			/* [ 804] */ 0x116f0100, /* NID16 STATUS */
			/* [ 805] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 806] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 807] */ 0x116f0100, /* NID16 STATUS */
			/* [ 808] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 809] */ 0x11610830, /* NID16 SCP_HIGH/DATA_HIGH req=0x30 */
			/* [ 810] */ 0x116f0100, /* NID16 STATUS */
			/* [ 811] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 812] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 813] */ 0x116f0100, /* NID16 STATUS */
			/* [ 814] */ 0x11236001, /* NID12 v=0x360 p=0x01 */
			/* [ 815] */ 0x11235001, /* NID12 v=0x350 p=0x01 */
			/* [ 816] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 817] */ 0x11610814, /* NID16 SCP_HIGH/DATA_HIGH req=0x14 */
			/* [ 818] */ 0x116f0100, /* NID16 STATUS */
			/* [ 819] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 820] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 821] */ 0x116f0100, /* NID16 STATUS */
			/* [ 822] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 823] */ 0x11610816, /* NID16 SCP_HIGH/DATA_HIGH req=0x16 */
			/* [ 824] */ 0x116f0100, /* NID16 STATUS */
			/* [ 825] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 826] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 827] */ 0x116f0100, /* NID16 STATUS */
			/* [ 828] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 829] */ 0x11610818, /* NID16 SCP_HIGH/DATA_HIGH req=0x18 */
			/* [ 830] */ 0x116f0100, /* NID16 STATUS */
			/* [ 831] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 832] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 833] */ 0x116f0100, /* NID16 STATUS */
			/* [ 834] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 835] */ 0x1161081a, /* NID16 SCP_HIGH/DATA_HIGH req=0x1a */
			/* [ 836] */ 0x116f0100, /* NID16 STATUS */
			/* [ 837] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 838] */ 0x11613f80, /* NID16 SCP_HIGH/DATA_HIGH req=0x80 */
			/* [ 839] */ 0x116f0100, /* NID16 STATUS */
			/* [ 840] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 841] */ 0x1161081c, /* NID16 SCP_HIGH/DATA_HIGH req=0x1c */
			/* [ 842] */ 0x116f0100, /* NID16 STATUS */
			/* [ 843] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 844] */ 0x11613f80, /* NID16 SCP_HIGH/DATA_HIGH req=0x80 */
			/* [ 845] */ 0x116f0100, /* NID16 STATUS */
			/* [ 846] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 847] */ 0x1161081e, /* NID16 SCP_HIGH/DATA_HIGH req=0x1e */
			/* [ 848] */ 0x116f0100, /* NID16 STATUS */
			/* [ 849] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 850] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 851] */ 0x116f0100, /* NID16 STATUS */
			/* [ 852] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 853] */ 0x11610820, /* NID16 SCP_HIGH/DATA_HIGH req=0x20 */
			/* [ 854] */ 0x116f0100, /* NID16 STATUS */
			/* [ 855] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 856] */ 0x11614020, /* NID16 SCP_HIGH/DATA_HIGH req=0x20 */
			/* [ 857] */ 0x116f0100, /* NID16 STATUS */
			/* [ 858] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 859] */ 0x11610822, /* NID16 SCP_HIGH/DATA_HIGH req=0x22 */
			/* [ 860] */ 0x116f0100, /* NID16 STATUS */
			/* [ 861] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 862] */ 0x11613f00, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 863] */ 0x116f0100, /* NID16 STATUS */
			/* [ 864] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 865] */ 0x11610824, /* NID16 SCP_HIGH/DATA_HIGH req=0x24 */
			/* [ 866] */ 0x116f0100, /* NID16 STATUS */
			/* [ 867] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 868] */ 0x11613f00, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 869] */ 0x116f0100, /* NID16 STATUS */
			/* [ 870] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 871] */ 0x11610826, /* NID16 SCP_HIGH/DATA_HIGH req=0x26 */
			/* [ 872] */ 0x116f0100, /* NID16 STATUS */
			/* [ 873] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 874] */ 0x11614040, /* NID16 SCP_HIGH/DATA_HIGH req=0x40 */
			/* [ 875] */ 0x116f0100, /* NID16 STATUS */
			/* [ 876] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 877] */ 0x11610828, /* NID16 SCP_HIGH/DATA_HIGH req=0x28 */
			/* [ 878] */ 0x116f0100, /* NID16 STATUS */
			/* [ 879] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 880] */ 0x11614040, /* NID16 SCP_HIGH/DATA_HIGH req=0x40 */
			/* [ 881] */ 0x116f0100, /* NID16 STATUS */
			/* [ 882] */ 0x11236001, /* NID12 v=0x360 p=0x01 */
			/* [ 883] */ 0x11235001, /* NID12 v=0x350 p=0x01 */
			/* [ 884] */ 0x11236001, /* NID12 v=0x360 p=0x01 */
			/* [ 885] */ 0x11235001, /* NID12 v=0x350 p=0x01 */
			/* [ 886] */ 0x11236001, /* NID12 v=0x360 p=0x01 */
			/* [ 887] */ 0x11235001, /* NID12 v=0x350 p=0x01 */
			/* [ 888] */ 0x11236001, /* NID12 v=0x360 p=0x01 */
			/* [ 889] */ 0x11235001, /* NID12 v=0x350 p=0x01 */
			/* [ 890] */ 0x11236001, /* NID12 v=0x360 p=0x01 */
			/* [ 891] */ 0x11235001, /* NID12 v=0x350 p=0x01 */
			/* [ 892] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 893] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [ 894] */ 0x116f0100, /* NID16 STATUS */
			/* [ 895] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 896] */ 0x11613f80, /* NID16 SCP_HIGH/DATA_HIGH req=0x80 */
			/* [ 897] */ 0x116f0100, /* NID16 STATUS */
			/* [ 898] */ 0x11570d24, /* NID15 ADDR_LOW=0x24 */
			/* [ 899] */ 0x11570e08, /* NID15 ADDR_HIGH=0x08 */
			/* [ 900] */ 0x115f0700, /* NID15 READ_EXRAM */
			/* [ 901] */ 0x115f0100, /* NID15 STATUS */
			/* [ 902] */ 0x11571718, /* NID15 PARAM_EX_ID=0x18 */
			/* [ 903] */ 0x11571818, /* NID15 PARAM_EX_VAL=0x18 */
			/* [ 904] */ 0x1157101c, /* NID15 PARAM_SET=0x1c */
			/* [ 905] */ 0x115f0100, /* NID15 STATUS */
			/* [ 906] */ 0x1157170d, /* NID15 PARAM_EX_ID=0x0d */
			/* [ 907] */ 0x115718a1, /* NID15 PARAM_EX_VAL=0xa1 */
			/* [ 908] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 909] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [ 910] */ 0x116f0100, /* NID16 STATUS */
			/* [ 911] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 912] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 913] */ 0x116f0100, /* NID16 STATUS */
			/* [ 914] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 915] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [ 916] */ 0x116f0100, /* NID16 STATUS */
			/* [ 917] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 918] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 919] */ 0x116f0100, /* NID16 STATUS */
			/* [ 920] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 921] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [ 922] */ 0x116f0100, /* NID16 STATUS */
			/* [ 923] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 924] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 925] */ 0x116f0100, /* NID16 STATUS */
			/* [ 926] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 927] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [ 928] */ 0x116f0100, /* NID16 STATUS */
			/* [ 929] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 930] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 931] */ 0x116f0100, /* NID16 STATUS */
			/* [ 932] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 933] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [ 934] */ 0x116f0100, /* NID16 STATUS */
			/* [ 935] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 936] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 937] */ 0x116f0100, /* NID16 STATUS */
			/* [ 938] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 939] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [ 940] */ 0x116f0100, /* NID16 STATUS */
			/* [ 941] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 942] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 943] */ 0x116f0100, /* NID16 STATUS */
			/* [ 944] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 945] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [ 946] */ 0x116f0100, /* NID16 STATUS */
			/* [ 947] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 948] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 949] */ 0x116f0100, /* NID16 STATUS */
			/* [ 950] */ 0x10279700, /* NID02 v=0x797 p=0x00 */
			/* [ 951] */ 0x1023a05a, /* NID02 v=0x3a0 p=0x5a */
			/* [ 952] */ 0x11602032, /* NID16 SCP_LOW p=0x32 */
			/* [ 953] */ 0x11610806, /* NID16 SCP_HIGH/DATA_HIGH req=0x06 */
			/* [ 954] */ 0x116f0100, /* NID16 STATUS */
			/* [ 955] */ 0x1160147b, /* NID16 v=0x014 p=0x7b */
			/* [ 956] */ 0x1161c0a6, /* NID16 SCP_HIGH/DATA_HIGH req=0xa6 */
			/* [ 957] */ 0x116f0100, /* NID16 STATUS */
			/* [ 958] */ 0x11602032, /* NID16 SCP_LOW p=0x32 */
			/* [ 959] */ 0x11610808, /* NID16 SCP_HIGH/DATA_HIGH req=0x08 */
			/* [ 960] */ 0x116f0100, /* NID16 STATUS */
			/* [ 961] */ 0x1160147b, /* NID16 v=0x014 p=0x7b */
			/* [ 962] */ 0x1161c0a6, /* NID16 SCP_HIGH/DATA_HIGH req=0xa6 */
			/* [ 963] */ 0x116f0100, /* NID16 STATUS */
			/* [ 964] */ 0x11602032, /* NID16 SCP_LOW p=0x32 */
			/* [ 965] */ 0x11610804, /* NID16 SCP_HIGH/DATA_HIGH req=0x04 */
			/* [ 966] */ 0x116f0100, /* NID16 STATUS */
			/* [ 967] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 968] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 969] */ 0x116f0100, /* NID16 STATUS */
			/* [ 970] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 971] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [ 972] */ 0x116f0100, /* NID16 STATUS */
			/* [ 973] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 974] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 975] */ 0x116f0100, /* NID16 STATUS */
			/* [ 976] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [ 977] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [ 978] */ 0x116f0100, /* NID16 STATUS */
			/* [ 979] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [ 980] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [ 981] */ 0x116f0100, /* NID16 STATUS */
			/* [ 982] */ 0x10279800, /* NID02 v=0x798 p=0x00 */
			/* [ 983] */ 0x1023905a, /* NID02 v=0x390 p=0x5a */
			/* [ 984] */ 0x11602032, /* NID16 SCP_LOW p=0x32 */
			/* [ 985] */ 0x11610806, /* NID16 SCP_HIGH/DATA_HIGH req=0x06 */
			/* [ 986] */ 0x116f0100, /* NID16 STATUS */
			/* [ 987] */ 0x1160147b, /* NID16 v=0x014 p=0x7b */
			/* [ 988] */ 0x1161c0a6, /* NID16 SCP_HIGH/DATA_HIGH req=0xa6 */
			/* [ 989] */ 0x116f0100, /* NID16 STATUS */
			/* [ 990] */ 0x11602032, /* NID16 SCP_LOW p=0x32 */
			/* [ 991] */ 0x11610808, /* NID16 SCP_HIGH/DATA_HIGH req=0x08 */
			/* [ 992] */ 0x116f0100, /* NID16 STATUS */
			/* [ 993] */ 0x1160147b, /* NID16 v=0x014 p=0x7b */
			/* [ 994] */ 0x1161c0a6, /* NID16 SCP_HIGH/DATA_HIGH req=0xa6 */
			/* [ 995] */ 0x116f0100, /* NID16 STATUS */
			/* [ 996] */ 0x11602032, /* NID16 SCP_LOW p=0x32 */
			/* [ 997] */ 0x11610804, /* NID16 SCP_HIGH/DATA_HIGH req=0x04 */
			/* [ 998] */ 0x116f0100, /* NID16 STATUS */
			/* [ 999] */ 0x11600000  /* NID16 DATA_LOW p=0x00 */
		};
		for (i = 0; i < ARRAY_SIZE(cfg); i++)
			creative_ae9_corb_send_verb_wait(chip, cfg[i]);
	}

	/* Give 8051 time to drain previous 5 chunks before next burst */
	msleep(15);

	/* Sub-array 6/6: verbs [1000–1182] (log [5157–5339]) */
	{
		static const u32 cfg[183] = {
			/* [1000] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1001] */ 0x116f0100, /* NID16 STATUS */
			/* [1002] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1003] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [1004] */ 0x116f0100, /* NID16 STATUS */
			/* [1005] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1006] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1007] */ 0x116f0100, /* NID16 STATUS */
			/* [1008] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1009] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [1010] */ 0x116f0100, /* NID16 STATUS */
			/* [1011] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1012] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1013] */ 0x116f0100, /* NID16 STATUS */
			/* [1014] */ 0x10379700, /* NID03 v=0x797 p=0x00 */
			/* [1015] */ 0x1033a05a, /* NID03 v=0x3a0 p=0x5a */
			/* [1016] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1017] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [1018] */ 0x116f0100, /* NID16 STATUS */
			/* [1019] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1020] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1021] */ 0x116f0100, /* NID16 STATUS */
			/* [1022] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1023] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [1024] */ 0x116f0100, /* NID16 STATUS */
			/* [1025] */ 0x10379800, /* NID03 v=0x798 p=0x00 */
			/* [1026] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1027] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1028] */ 0x116f0100, /* NID16 STATUS */
			/* [1029] */ 0x1033905a, /* NID03 v=0x390 p=0x5a */
			/* [1030] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1031] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [1032] */ 0x116f0100, /* NID16 STATUS */
			/* [1033] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1034] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1035] */ 0x116f0100, /* NID16 STATUS */
			/* [1036] */ 0x10479700, /* NID04 v=0x797 p=0x00 */
			/* [1037] */ 0x1043a05a, /* NID04 v=0x3a0 p=0x5a */
			/* [1038] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1039] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [1040] */ 0x116f0100, /* NID16 STATUS */
			/* [1041] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1042] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1043] */ 0x116f0100, /* NID16 STATUS */
			/* [1044] */ 0x10479800, /* NID04 v=0x798 p=0x00 */
			/* [1045] */ 0x1043905a, /* NID04 v=0x390 p=0x5a */
			/* [1046] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1047] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [1048] */ 0x116f0100, /* NID16 STATUS */
			/* [1049] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1050] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1051] */ 0x116f0100, /* NID16 STATUS */
			/* [1052] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1053] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [1054] */ 0x116f0100, /* NID16 STATUS */
			/* [1055] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1056] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1057] */ 0x116f0100, /* NID16 STATUS */
			/* [1058] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1059] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [1060] */ 0x116f0100, /* NID16 STATUS */
			/* [1061] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1062] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1063] */ 0x116f0100, /* NID16 STATUS */
			/* [1064] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1065] */ 0x11610842, /* NID16 SCP_HIGH/DATA_HIGH req=0x42 */
			/* [1066] */ 0x116f0100, /* NID16 STATUS */
			/* [1067] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1068] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1069] */ 0x116f0100, /* NID16 STATUS */
			/* [1070] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1071] */ 0x11610844, /* NID16 SCP_HIGH/DATA_HIGH req=0x44 */
			/* [1072] */ 0x116f0100, /* NID16 STATUS */
			/* [1073] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1074] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1075] */ 0x116f0100, /* NID16 STATUS */
			/* [1076] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1077] */ 0x11610846, /* NID16 SCP_HIGH/DATA_HIGH req=0x46 */
			/* [1078] */ 0x116f0100, /* NID16 STATUS */
			/* [1079] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1080] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1081] */ 0x116f0100, /* NID16 STATUS */
			/* [1082] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1083] */ 0x11610848, /* NID16 SCP_HIGH/DATA_HIGH req=0x48 */
			/* [1084] */ 0x116f0100, /* NID16 STATUS */
			/* [1085] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1086] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1087] */ 0x116f0100, /* NID16 STATUS */
			/* [1088] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1089] */ 0x1161084a, /* NID16 SCP_HIGH/DATA_HIGH req=0x4a */
			/* [1090] */ 0x116f0100, /* NID16 STATUS */
			/* [1091] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1092] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1093] */ 0x116f0100, /* NID16 STATUS */
			/* [1094] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1095] */ 0x1161084c, /* NID16 SCP_HIGH/DATA_HIGH req=0x4c */
			/* [1096] */ 0x116f0100, /* NID16 STATUS */
			/* [1097] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1098] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1099] */ 0x116f0100, /* NID16 STATUS */
			/* [1100] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1101] */ 0x11610862, /* NID16 SCP_HIGH/DATA_HIGH req=0x62 */
			/* [1102] */ 0x116f0100, /* NID16 STATUS */
			/* [1103] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1104] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1105] */ 0x116f0100, /* NID16 STATUS */
			/* [1106] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1107] */ 0x11610864, /* NID16 SCP_HIGH/DATA_HIGH req=0x64 */
			/* [1108] */ 0x116f0100, /* NID16 STATUS */
			/* [1109] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1110] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1111] */ 0x116f0100, /* NID16 STATUS */
			/* [1112] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1113] */ 0x11610866, /* NID16 SCP_HIGH/DATA_HIGH req=0x66 */
			/* [1114] */ 0x116f0100, /* NID16 STATUS */
			/* [1115] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1116] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1117] */ 0x116f0100, /* NID16 STATUS */
			/* [1118] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1119] */ 0x11610868, /* NID16 SCP_HIGH/DATA_HIGH req=0x68 */
			/* [1120] */ 0x116f0100, /* NID16 STATUS */
			/* [1121] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1122] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1123] */ 0x116f0100, /* NID16 STATUS */
			/* [1124] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1125] */ 0x1161086a, /* NID16 SCP_HIGH/DATA_HIGH req=0x6a */
			/* [1126] */ 0x116f0100, /* NID16 STATUS */
			/* [1127] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1128] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1129] */ 0x116f0100, /* NID16 STATUS */
			/* [1130] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1131] */ 0x1161086c, /* NID16 SCP_HIGH/DATA_HIGH req=0x6c */
			/* [1132] */ 0x116f0100, /* NID16 STATUS */
			/* [1133] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1134] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1135] */ 0x116f0100, /* NID16 STATUS */
			/* [1136] */ 0x11602080, /* NID16 SCP_LOW p=0x80 */
			/* [1137] */ 0x11610808, /* NID16 SCP_HIGH/DATA_HIGH req=0x08 */
			/* [1138] */ 0x116f0100, /* NID16 STATUS */
			/* [1139] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1140] */ 0x11614100, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1141] */ 0x116f0100, /* NID16 STATUS */
			/* [1142] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1143] */ 0x11610832, /* NID16 SCP_HIGH/DATA_HIGH req=0x32 */
			/* [1144] */ 0x116f0100, /* NID16 STATUS */
			/* [1145] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1146] */ 0x11613e80, /* NID16 SCP_HIGH/DATA_HIGH req=0x80 */
			/* [1147] */ 0x116f0100, /* NID16 STATUS */
			/* [1148] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1149] */ 0x1161082e, /* NID16 SCP_HIGH/DATA_HIGH req=0x2e */
			/* [1150] */ 0x116f0100, /* NID16 STATUS */
			/* [1151] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1152] */ 0x116142a0, /* NID16 SCP_HIGH/DATA_HIGH req=0xa0 */
			/* [1153] */ 0x116f0100, /* NID16 STATUS */
			/* [1154] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1155] */ 0x11610830, /* NID16 SCP_HIGH/DATA_HIGH req=0x30 */
			/* [1156] */ 0x116f0100, /* NID16 STATUS */
			/* [1157] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1158] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1159] */ 0x116f0100, /* NID16 STATUS */
			/* [1160] */ 0x1160208f, /* NID16 SCP_LOW p=0x8f */
			/* [1161] */ 0x11610802, /* NID16 SCP_HIGH/DATA_HIGH req=0x02 */
			/* [1162] */ 0x116f0100, /* NID16 STATUS */
			/* [1163] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1164] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1165] */ 0x116f0100, /* NID16 STATUS */
			/* [1166] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1167] */ 0x1161083e, /* NID16 SCP_HIGH/DATA_HIGH req=0x3e */
			/* [1168] */ 0x116f0100, /* NID16 STATUS */
			/* [1169] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1170] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1171] */ 0x116f0100, /* NID16 STATUS */
			/* [1172] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1173] */ 0x1161082a, /* NID16 SCP_HIGH/DATA_HIGH req=0x2a */
			/* [1174] */ 0x116f0100, /* NID16 STATUS */
			/* [1175] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1176] */ 0x11610000, /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
			/* [1177] */ 0x116f0100, /* NID16 STATUS */
			/* [1178] */ 0x11602096, /* NID16 SCP_LOW p=0x96 */
			/* [1179] */ 0x11610874, /* NID16 SCP_HIGH/DATA_HIGH req=0x74 */
			/* [1180] */ 0x116f0100, /* NID16 STATUS */
			/* [1181] */ 0x11600000, /* NID16 DATA_LOW p=0x00 */
			/* [1182] */ 0x11610000  /* NID16 SCP_HIGH/DATA_HIGH req=0x00 */
		};
		for (i = 0; i < ARRAY_SIZE(cfg); i++)
			creative_ae9_corb_send_verb_wait(chip, cfg[i]);
	}

	mutex_unlock(&chip->corb_mutex);
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
#define DSPLOAD_STREAM_BASE	(dl_windows ? 0x100 : 0x120)
#define DSPLOAD_STREAM_TAG	(dl_windows ? 1 : 7)
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
	writew(dl_bdl64 ? 63 : 0, sd + AZX_REG_SD_LVI);
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
	if (dl_bdl64) {
		unsigned int i, n = 64, len = dma->buf_size / 64;

		for (i = 0; i < n; i++) {
			dma_addr_t a = dma->buf_dma + (dma_addr_t)i * len;

			bdl[i * 4 + 0] = cpu_to_le32(lower_32_bits(a));
			bdl[i * 4 + 1] = cpu_to_le32(upper_32_bits(a));
			bdl[i * 4 + 2] = cpu_to_le32(len);
			bdl[i * 4 + 3] = cpu_to_le32(0);
		}
	} else {
		bdl[0] = cpu_to_le32(lower_32_bits(dma->buf_dma));
		bdl[1] = cpu_to_le32(upper_32_bits(dma->buf_dma));
		bdl[2] = cpu_to_le32(dma->buf_size);
		bdl[3] = cpu_to_le32(0);
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
	writew(dl_bdl64 ? 63 : 0,
	       chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_LVI);
	writew(dma->hda_format,
	       chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_FORMAT);
	writel(lower_32_bits(dma->bdl_dma),
	       chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_BDLPL);
	writel(upper_32_bits(dma->bdl_dma),
	       chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_BDLPU);
	wmb();
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
	if (dl_windows) {
		/* Windows resets the download stream right after the transfer */
		writel(SD_CTL_STREAM_RESET,
		       chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_CTL);
		wmb();
		udelay(50);
		writel(0, chip->hda_regs + DSPLOAD_STREAM_BASE + AZX_REG_SD_CTL);
		wmb();
		udelay(50);
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
		if (dl_reset_chunks)
			dma_program(dma);	/* read position back to 0 */
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
	if (dl_windows)
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

	/* bind node 0x15 to the load stream tag */
	if (!dl_windows)
		verb_send(chip,
			creative_ae9_verb(CAD, WIDGET_CHIP_CTRL, 0x706,
					  (dma.stream_tag << 4)), &response);

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
	int status;

	while (channels > 16) {
		sample_rate *= 2;
		channels /= 2;
	}

	if (!prep_windows) {
		status = dsp_reset(chip);
		if (status < 0)
			return status;
	}
	status = dspxfr_image(chip, fls, sample_rate, channels);
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
	if (hs_immediate) {
		int r = dsp_set_run_state(chip);
		int hs = creative_dsp_boot_handshake(chip);

		chip->hs_done = 1;
		chip->hs_ok = !hs;
		dev_info(chip->dev,
			 "dspload: run ret=%d, immediate boot handshake %s (%d)\n",
			 r, hs ? "INCOMPLETE" : "complete", hs);
		if (hs && chip->resuming)
			return -EIO;
		return r;
	}
	{
		unsigned int dbgcntl = 0;
		int r = dsp_set_run_state(chip);
		creative_dsp_chipio_read(chip, DSP_DBGCNTL_INST_OFFSET, &dbgcntl);
		dev_info(chip->dev,
			 "dspload: dsp_set_run_state ret=%d DBGCNTL=0x%x (halt_state=0x%x)\n",
			 r, dbgcntl,
			 (dbgcntl & DSP_DBGCNTL_STATE_MASK) >> DSP_DBGCNTL_STATE_LOBIT);
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

static int dspio_set_uint_param(struct creative_chip *chip, int mod_id,
				int req, unsigned int data)
{
	int ret = dspio_scp(chip, mod_id, 0x20, req, SCP_SET, &data,
			    sizeof(data), NULL, NULL);
	if (ret < 0) {
		u32 sts = dsp_read(chip, WIDGET_DSP_CTRL,
				   VENDOR_DSPIO_STATUS, 0);
		dev_info(chip->dev,
			 "set_uint_param mid=0x%x req=0x%x FAILED ret=%d sts=0x%x\n",
			 mod_id, req, ret, sts);
	}
	return ret;
}

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

static void chipio_set_control_param(struct creative_chip *chip,
				     int param_id, int param_val)
{
	if (param_id < 32 && param_val < 8) {
		dsp_write(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_SET,
			  (param_val << 5) | param_id);
	} else if (chipio_send(chip, VENDOR_CHIPIO_STATUS, 0) == 0) {
		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_PARAM_EX_ID_SET, param_id);
		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_PARAM_EX_VALUE_SET, param_val);
	}
}

static void chipio_set_stream_channels(struct creative_chip *chip,
				int streamid, unsigned int channels)
{
	chipio_set_control_param(chip, CONTROL_PARAM_STREAM_ID, streamid);
	chipio_set_control_param(chip, CONTROL_PARAM_STREAMS_CHANNELS, channels);
}

static void chipio_set_stream_control(struct creative_chip *chip,
			       int streamid, int enable)
{
	chipio_set_control_param(chip, CONTROL_PARAM_STREAM_ID, streamid);
	chipio_set_control_param(chip, CONTROL_PARAM_STREAM_CONTROL, enable);
}

static void chipio_set_stream_source_dest(struct creative_chip *chip,
					  int streamid, int src, int dst)
{
	chipio_set_control_param(chip, CONTROL_PARAM_STREAM_ID, streamid);
	chipio_set_control_param(chip, CONTROL_PARAM_STREAM_SOURCE_CONN, src);
	chipio_set_control_param(chip, CONTROL_PARAM_STREAM_DEST_CONN, dst);
}

static void chipio_set_conn_rate(struct creative_chip *chip, int connid,
				 int rate)
{
	chipio_set_control_param(chip, CONTROL_PARAM_CONN_POINT_ID, connid);
	chipio_set_control_param(chip, CONTROL_PARAM_CONN_POINT_SR, rate);
}

static void chipio_8051_set_addr(struct creative_chip *chip, unsigned int a)
{
	dsp_write(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_ADDRESS_LOW,
		  a & 0xff);
	dsp_write(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_ADDRESS_HIGH,
		  (a >> 8) & 0xff);
}

static void chipio_8051_read_exram(struct creative_chip *chip,
				   unsigned int addr, unsigned int *data)
{
	chipio_8051_set_addr(chip, addr);
	*data = dsp_read(chip, WIDGET_CHIP_CTRL, 0xF07, 0); /* 8051 DATA_READ */
}

/* BAR2 ca0113 command interface (group/target/value).
 * Handshake values (0x20c 0x800001/0x800003/0x800002) taken from the
 * 0x0071 VFIO capture (differ from AE-7 mainline's 0x800005/0x800004).
 */
static void ca0113_mmio_command_set(struct creative_chip *chip,
				    unsigned int group, unsigned int target,
				    unsigned int value)
{
	unsigned int write_val;

	if (ae9_ca0113_windows)
		return;

	creative_ext_writel(chip, 0x210, 0x0000007e);
	creative_ext_readl(chip, 0x210);
	creative_ext_writel(chip, 0x210, 0x0000005a);
	creative_ext_readl(chip, 0x210);
	creative_ext_readl(chip, 0x210);
	creative_ext_writel(chip, 0x20c, 0x00800001);
	creative_ext_writel(chip, 0x804, group);
	creative_ext_writel(chip, 0x20c, 0x00800003);
	write_val = (target & 0xff) | (value << 8);
	creative_ext_writel(chip, 0x204, write_val);
	msleep(20);
	creative_ext_readl(chip, 0x860);
	creative_ext_readl(chip, 0x854);
	creative_ext_readl(chip, 0x840);
	creative_ext_writel(chip, 0x20c, 0x00800002);
	creative_ext_writel(chip, 0x210, 0x00000000);
	creative_ext_readl(chip, 0x210);
	creative_ext_readl(chip, 0x210);
}

static void ca0113_mmio_command_set_type2(struct creative_chip *chip,
					  unsigned int group, unsigned int target,
					  unsigned int value)
{
	unsigned int write_val;

	if (ae9_ca0113_windows)
		return;

	creative_ext_writel(chip, 0x210, 0x0000007e);
	creative_ext_readl(chip, 0x210);
	creative_ext_writel(chip, 0x210, 0x0000005a);
	creative_ext_readl(chip, 0x210);
	creative_ext_readl(chip, 0x210);
	creative_ext_writel(chip, 0x20c, 0x00800003);
	creative_ext_writel(chip, 0x804, group);
	creative_ext_writel(chip, 0x20c, 0x00800003);
	write_val = (target & 0xff) | (value << 8);
	creative_ext_writel(chip, 0x204, write_val);
	msleep(20);
	creative_ext_readl(chip, 0x860);
	creative_ext_readl(chip, 0x854);
	creative_ext_readl(chip, 0x840);
	creative_ext_writel(chip, 0x20c, 0x00800002);
	creative_ext_writel(chip, 0x210, 0x00000000);
	creative_ext_readl(chip, 0x210);
	creative_ext_readl(chip, 0x210);
}

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
			if (chip->dac_filter)
				val = (val & 0x1f) |
				      (chip->dac_filter == 2 ? 0x20 : 0x00);
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

static void ca0113_mmio_gpio_set(struct creative_chip *chip,
				 unsigned int pin, bool enable)
{
	u16 v = (pin & 0xf) | ((enable << 8) & 0x100);

	creative_ext_writew(chip, 0x320, v);
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

/* Old (non-Windows) call sites; with ca0113_windows they send nothing. */
static void ca0113_play_cmd(struct creative_chip *chip, u8 group,
			    u8 reg, u8 val)
{
	if (ae9_ca0113_windows)
		return;
	mutex_lock(&chip->ca0113_mutex);
	ca0113_cmd(chip, group, reg, val, CA0113_SETTLE_US);
	mutex_unlock(&chip->ca0113_mutex);
}

/*
 * creative_ae9_ca0113_handshake — full AE-9 CA0113 init handshake.
 *
 * Must be called AFTER DSP firmware is downloaded and BEFORE any
 * ca0113_mmio_command_set() or ca0113_play_cmd() calls.
 *
 * Matches ae9_build ae9_setup_defaults() CA0113 handshake section exactly:
 *   1. GPIO0 enable (0x320=0x0100)
 *   2. CA0113 engine reset (0x86c=0 → 0x800=0x6b → 0x86c=1)
 *   3. Group 0x57 + Mode 1 (0x804=0x57, 0x20c=0x800001)
 *   4. GPIO5 pulse (DAC power cycle: ON → ALL OFF → GPIO0 ON)
 *   5. 0x210 sync: 0x7e → 0x5a → expect 0xaa
 *   6. Mode 1 rewrite → Group 0x48 → Mode 3 → Target 0x07
 *   7. Token: 0xffff to 0x208, end handshake (mode 2)
 *   8. Re-sync → first dummy cmd (reg 0x28 = 0x00)
 *   9. ~50 CA0113 DAC init commands (all groups 0x48 + 0x49)
 *
 * CRITICAL: Windows sends ALL ca0113 DAC init commands immediately
 * after the handshake, before any HDA verb traffic sets bit 16 of
 * 0x20c. Once bit 16 is set, 0x48/0x49 group commands silently fail.
 */
int creative_ae9_ca0113_handshake(struct creative_chip *chip)
{
	u32 sync_val;

	dev_info(chip->dev, "CA0113 handshake: starting (0x20c=0x%08x)\n",
		 creative_ext_readl(chip, 0x20c));

	/* Step 1: GPIO pin 0 enable */
	creative_ext_writew(chip, 0x320, 0x0100);
	creative_ext_readw(chip, 0x320);

	/* Step 1b: CA0113 engine reset (2nd reset, matching ae9_build) */
	creative_ext_writel(chip, 0x830, 0x00000aff);
	creative_ext_readl(chip, 0x830);
	creative_ext_writel(chip, 0x86c, 0x00000000);
	creative_ext_readl(chip, 0x86c);
	msleep(14);
	creative_ext_writel(chip, 0x800, 0x0000006b);
	creative_ext_readl(chip, 0x800);
	creative_ext_writel(chip, 0x86c, 0x00000001);
	creative_ext_readl(chip, 0x86c);
	msleep(16);
	creative_ext_writel(chip, 0x800, 0x0000006b);
	creative_ext_readl(chip, 0x800);

	/* Step 2: Group 0x57 + Mode 1 (go straight to mode 1, not mode 0) */
	creative_ext_writel(chip, 0x804, 0x57);
	creative_ext_readl(chip, 0x804);
	creative_ext_writel(chip, 0x20c, 0x00800001);
	creative_ext_readl(chip, 0x20c);

	/*
	 * Step 3: GPIO5 pulse — DAC power cycle.
	 * ae9_build: "Wakes up the I2C bus to the DACs via CA0113 bridge.
	 * Without it, commands to group 0x48/0x49 are silently lost."
	 */
	creative_ext_readw(chip, 0x320);
	creative_ext_writew(chip, 0x320, 0x0105);  /* GPIO5 ON */
	creative_ext_readw(chip, 0x320);
	creative_ext_readw(chip, 0x320);
	creative_ext_readw(chip, 0x320);
	creative_ext_writew(chip, 0x320, 0x0000);  /* ALL OFF */
	creative_ext_readw(chip, 0x320);
	creative_ext_readw(chip, 0x320);
	msleep(16);
	creative_ext_readw(chip, 0x320);
	creative_ext_writew(chip, 0x320, 0x0100);  /* GPIO0 ON only */
	creative_ext_readw(chip, 0x320);
	creative_ext_readw(chip, 0x320);

	/* Step 4: Sync 0x7e/0x5a → expect 0xaa */
	creative_ext_writel(chip, 0x210, 0x7e);
	sync_val = creative_ext_readl(chip, 0x210);
	creative_ext_writel(chip, 0x210, 0x5a);
	sync_val = creative_ext_readl(chip, 0x210);
	if (sync_val != 0xaa)
		sync_val = creative_ext_readl(chip, 0x210);

	dev_info(chip->dev, "CA0113 handshake: sync=0x%02x (want 0xaa)\n",
		 sync_val & 0xff);

	if ((sync_val & 0xff) != 0xaa) {
		dev_warn(chip->dev,
			 "CA0113 handshake: sync FAILED, restoring GPIO5 and continuing\n");
		creative_ext_writew(chip, 0x320, 0x0105);
		goto send_dac_cmds;  /* still attempt DAC commands */
	}

	/* Step 5: Mode 1 rewrite */
	creative_ext_writel(chip, 0x20c, 0x00800001);
	creative_ext_readl(chip, 0x20c);

	/* Step 5b: Group 0x48 */
	creative_ext_writel(chip, 0x804, 0x48);
	creative_ext_readl(chip, 0x804);

	/* Step 6: Mode 3 */
	creative_ext_writel(chip, 0x20c, 0x00800003);
	creative_ext_readl(chip, 0x20c);

	/* Step 6b: Target reg 0x07 */
	creative_ext_writel(chip, 0x204, 0x07);
	creative_ext_readl(chip, 0x204);

	/* Step 7: Mode 3 rewrite */
	creative_ext_writel(chip, 0x20c, 0x00800003);
	creative_ext_readl(chip, 0x20c);

	/* Step 8: Token write (0x208=0xffff), then end handshake */
	creative_ext_writel(chip, 0x208, 0xffff);
	creative_ext_readl(chip, 0x208);  /* returns 0xffffff80 — normal */
	msleep(16);
	dev_info(chip->dev, "CA0113 handshake: 0x208=0x%08x after token\n",
		 creative_ext_readl(chip, 0x208));

	/* Step 9: End handshake (mode 2), clear sync */
	creative_ext_writel(chip, 0x20c, 0x00800002);
	creative_ext_readl(chip, 0x20c);
	creative_ext_writel(chip, 0x210, 0x00);
	creative_ext_readl(chip, 0x210);

	/* Step 10: Re-sync for first real command */
	creative_ext_writel(chip, 0x210, 0x7e);
	creative_ext_readl(chip, 0x210);
	creative_ext_writel(chip, 0x210, 0x5a);
	sync_val = creative_ext_readl(chip, 0x210);
	if (sync_val != 0xaa)
		sync_val = creative_ext_readl(chip, 0x210);

	dev_info(chip->dev, "CA0113 handshake: re-sync=0x%02x\n",
		 sync_val & 0xff);

	if ((sync_val & 0xff) == 0xaa) {
		/* Dummy first command to verify CA0113 executes */
		creative_ext_writel(chip, 0x20c, 0x00800003);
		creative_ext_readl(chip, 0x20c);
		creative_ext_readl(chip, 0x804);
		creative_ext_writel(chip, 0x804, 0x48);
		creative_ext_readl(chip, 0x804);
		creative_ext_writel(chip, 0x20c, 0x00800005);
		creative_ext_readl(chip, 0x20c);
		creative_ext_writel(chip, 0x204, 0x0028); /* reg 0x28 = 0x00, harmless */
		creative_ext_readl(chip, 0x204);
		msleep(20);
		dev_info(chip->dev,
			 "CA0113 handshake: first cmd 0x20c=0x%08x 0x860=%d 0x854=%d\n",
			 creative_ext_readl(chip, 0x20c),
			 creative_ext_readl(chip, 0x860),
			 creative_ext_readl(chip, 0x854));
		creative_ext_writel(chip, 0x20c, 0x00800004);
		creative_ext_readl(chip, 0x20c);
		creative_ext_writel(chip, 0x210, 0x00);
		creative_ext_readl(chip, 0x210);
	}

	/* Restore GPIO5 for DAC power */
	creative_ext_writew(chip, 0x320, 0x0105);

	dev_info(chip->dev,
		 "CA0113 handshake: complete 0x20c=0x%08x 0x208=0x%08x\n",
		 creative_ext_readl(chip, 0x20c),
		 creative_ext_readl(chip, 0x208));

send_dac_cmds:
	/*
	 * Send ALL CA0113 DAC init commands immediately after handshake.
	 * MUST happen before any HDA verb traffic sets bit 16 of 0x20c.
	 * Once bit 16 is set, group 0x48/0x49 commands silently fail.
	 *
	 * Exact sequence from ae9_build ae9_setup_defaults() post-handshake
	 * DAC init block (confirmed from ae9_build + Windows VFIO trace).
	 */
	dev_info(chip->dev, "CA0113 handshake: sending DAC init commands\n");

	ca0113_mmio_gpio_set(chip, 3, true);
	ca0113_mmio_gpio_set(chip, 3, false);
	ca0113_mmio_command_set_type2(chip, 0x48, 0x07, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x07, 0x01);
	ca0113_mmio_command_set(chip, 0x49, 0x0a, 0x07);
	ca0113_mmio_gpio_set(chip, 2, false);
	ca0113_mmio_command_set(chip, 0x48, 0x1d, 0x40);
	ca0113_mmio_gpio_set(chip, 3, true);
	ca0113_mmio_gpio_set(chip, 1, false);

	/* Volume/filter init */
	ca0113_mmio_command_set(chip, 0x48, 0x01, 0xc0);
	ca0113_mmio_command_set(chip, 0x48, 0x0a, 0x02);
	ca0113_mmio_command_set(chip, 0x48, 0x0b, 0x20);
	ca0113_mmio_command_set(chip, 0x48, 0x04, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x04, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x06, 0x40);
	ca0113_mmio_command_set(chip, 0x48, 0x08, 0xff);
	ca0113_mmio_command_set(chip, 0x48, 0x1d, 0x40);
	ca0113_mmio_command_set(chip, 0x48, 0x0a, 0x06);
	ca0113_mmio_command_set(chip, 0x48, 0x0c, 0x5f);
	ca0113_mmio_command_set_type2(chip, 0x48, 0x0e, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x0e, 0x8a);
	ca0113_mmio_command_set_type2(chip, 0x48, 0x10, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x07, 0x81);
	ca0113_mmio_command_set(chip, 0x48, 0x0f, 0x00);
	ca0113_mmio_command_set(chip, 0x49, 0x04, 0x0b);
	ca0113_mmio_command_set(chip, 0x48, 0x10, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x0a, 0x02);
	ca0113_mmio_command_set(chip, 0x49, 0x05, 0x0b);
	ca0113_mmio_command_set(chip, 0x49, 0x18, 0xff);
	ca0113_mmio_command_set(chip, 0x49, 0x19, 0xff);
	ca0113_mmio_command_set(chip, 0x49, 0x02, 0x0b);
	ca0113_mmio_command_set(chip, 0x49, 0x1a, 0x00);
	ca0113_mmio_command_set(chip, 0x49, 0x1b, 0x70);
	ca0113_mmio_command_set(chip, 0x49, 0x03, 0x0b);
	ca0113_mmio_command_set(chip, 0x49, 0x00, 0xff);
	ca0113_mmio_command_set(chip, 0x49, 0x01, 0xff);
	ca0113_mmio_command_set(chip, 0x49, 0x06, 0xff);
	ca0113_mmio_command_set(chip, 0x49, 0x07, 0xff);
	ca0113_mmio_command_set(chip, 0x49, 0x10, 0x7f);
	ca0113_mmio_command_set(chip, 0x49, 0x0a, 0x07);
	/* Unmute DAC */
	ca0113_mmio_command_set_type2(chip, 0x48, 0x07, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x07, 0x80);
	ca0113_mmio_command_set(chip, 0x49, 0x0a, 0x06);
	/* Final config */
	ca0113_mmio_command_set_type2(chip, 0x48, 0x07, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x07, 0x00);
	ca0113_mmio_command_set_type2(chip, 0x49, 0x0e, 0x00);
	ca0113_mmio_command_set(chip, 0x49, 0x0e, 0x01);
	ca0113_mmio_command_set(chip, 0x48, 0x0f, 0x0b);
	ca0113_mmio_command_set(chip, 0x48, 0x10, 0x0b);

	/* Windows playback output preset (HP path) */
	ca0113_mmio_command_set(chip, 0x48, 0x1d, 0x80);
	ca0113_mmio_command_set(chip, 0x48, 0x0d, 0x40);
	ca0113_mmio_command_set(chip, 0x48, 0x16, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x17, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x18, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x19, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x11, 0xff);
	ca0113_mmio_command_set(chip, 0x48, 0x12, 0xff);
	ca0113_mmio_command_set(chip, 0x48, 0x13, 0xff);
	ca0113_mmio_command_set(chip, 0x48, 0x14, 0x7f);

	dev_info(chip->dev,
		 "CA0113 handshake: DAC init done 0x20c=0x%08x 0x860=%d\n",
		 creative_ext_readl(chip, 0x20c),
		 creative_ext_readl(chip, 0x860));
	return 0;
}

int creative_dsp_amp_engage(struct creative_chip *chip)
{
	static const u8 seq[][2] = {
		{ 0x07, 0x80 }, { 0x0a, 0x06 },		/* note grp below */
	};
	(void)seq;

	dev_info(chip->dev, "amp engage: GPIO pin4 HIGH + ca0113 route\n");

	/* 1. amp/relay power-enable GPIO pin 4 HIGH */
	ca0113_mmio_gpio_set(chip, 4, true);

	/* 2. first command uses the 0x800003 type2 handshake */
	ca0113_mmio_command_set_type2(chip, 0x48, 0x07, 0x00);

	/* 3. the rest use the 0x800005 handshake (ca0113_play_cmd) */
	ca0113_play_cmd(chip, 0x48, 0x07, 0x80);
	ca0113_play_cmd(chip, 0x49, 0x0a, 0x06);
	ca0113_play_cmd(chip, 0x48, 0x0f, 0x31);
	ca0113_play_cmd(chip, 0x48, 0x10, 0x31);
	ca0113_play_cmd(chip, 0x49, 0x04, 0x31);
	ca0113_play_cmd(chip, 0x49, 0x05, 0x31);
	ca0113_play_cmd(chip, 0x49, 0x02, 0x31);
	ca0113_play_cmd(chip, 0x49, 0x03, 0x31);

	return 0;
}

/*
 * ca0113 DAC unmute sequence — three writes that fire at 01:28:07.514–564
 * in the cold-boot MMIO, immediately after GPIO 0x104→0x105 and before
 * the SYS_INFO/probe frames:
 *
 *   0x204=0x0007: trigger reg 0x07 (relay acknowledge / clock pulse)
 *   0x204=0x8007: write 0x80 → reg 0x07 (DAC unmute / output enable)
 *   0x204=0x060a: write 0x06 → reg 0x0a (audio clock source = 0x06)
 *
 * These are identical to the first three commands in creative_dsp_amp_engage()
 * but must also fire at PCM prepare time (called from pcm_relay_engage).
 */
/* 0x204=0x0007: trigger reg 7 — fires at 01:28:07.514, right after GPIO 0x105 */
int creative_dsp_ca0113_trigger7(struct creative_chip *chip)
{
	ca0113_mmio_command_set_type2(chip, 0x48, 0x07, 0x00);
	return 0;
}

/* 0x204=0x8007: write 0x80 → reg 7 (unmute) — fires at 01:28:07.546, between 1mcA and DMA_CTRL */
int creative_dsp_ca0113_unmute_enable(struct creative_chip *chip)
{
	ca0113_play_cmd(chip, 0x48, 0x07, 0x80);
	return 0;
}

/* 0x204=0x060a: write 0x06 → reg 0x0a (clock off) — fires at 01:28:07.564, between DMA_CTRL and probe */
int creative_dsp_ca0113_clock_off(struct creative_chip *chip)
{
	ca0113_play_cmd(chip, 0x48, 0x0a, 0x06);
	return 0;
}

/* Combined (kept for backwards compat / probe-time amp_engage) */
int creative_dsp_ca0113_unmute(struct creative_chip *chip)
{
	creative_dsp_ca0113_trigger7(chip);
	creative_dsp_ca0113_unmute_enable(chip);
	creative_dsp_ca0113_clock_off(chip);
	return 0;
}

/*
 * ca0132_alt_select_out() equivalent for AE-9 SPEAKER path.
 *
 * The decompiled driver's ca0132_alt_select_out() (line 20185) sends a set
 * of DSP params that open the audio path through the DSP's speaker-tuning
 * module to the analog output. Our driver was missing this entirely.
 *
 * The critical gate is SPEAKER_TUNING_MUTE (mod=0x96, req=0x3a):
 *   - Set to FLOAT_ONE (0x3f800000) at function entry  = mute during switch
 *   - Set to FLOAT_ZERO (0x00000000) at function exit  = unmute = audio ON
 *
 * Without the final unmute the DSP's speaker-tuning stage stays muted and
 * no audio reaches the output even with DMA running and the relay clicked.
 *
 * Sequence matches ca0132_alt_select_out() lines 20241–20497 for AE-9
 * jack_present=0 (speaker / line-out path).
 */

/*
 * creative_ae9_ca0113_second_reset — second PLL + CA0113 engine reset.
 *
 * Called from acm_work after GPIO5 DAC power-on + msleep(3000).
 * Matches ae9_build ae9_setup_defaults() second reset block exactly:
 *   1. Pre-reset DAC commands (group 0x48: 0x13=0xff, 0x14=0x7f, 0x1d=0x80)
 *   2. GPIO sequence (GPIO3+0 ON → GPIO3 OFF)
 *   3. PLL reset: 0x304=0xff×9, 0x100=0x0×4 interleaved
 *   4. Engine reset: 0x86c=0 → 0x800=0x6b → 0x86c=1
 *   5. Second handshake: 0x800=0x6b, 0x804=0x48, 0x20c=0x800005
 *   6. GPIO5, sync 0x7e/0x5a, mode 3 + token → mode 2 + re-sync
 *   7. Post-handshake DAC commands (groups 0x48 + 0x49)
 *
 * "Without this, the I2S clock to the ACM never starts and the relay
 * never clicks." — ae9_build comment at the second reset block.
 */
int creative_ae9_ca0113_second_reset(struct creative_chip *chip)
{
	dev_info(chip->dev, "CA0113 second reset: starting\n");

	/* Step 1: Pre-reset DAC commands */
	ca0113_mmio_command_set(chip, 0x48, 0x13, 0xff);
	ca0113_mmio_command_set(chip, 0x48, 0x14, 0x7f);
	ca0113_mmio_command_set(chip, 0x48, 0x1d, 0x80);

	/* Step 2: GPIO sequence */
	ca0113_mmio_gpio_set(chip, 3, true);
	ca0113_mmio_gpio_set(chip, 0, true);   /* 0x103 */
	ca0113_mmio_gpio_set(chip, 3, false);  /* 0x003 */

	/* Step 3: PLL reset — exact VFIO pattern */
	writeb(0xff, chip->ext_regs + 0x304);
	writeb(0xff, chip->ext_regs + 0x304);
	writeb(0xff, chip->ext_regs + 0x304);
	writeb(0xff, chip->ext_regs + 0x304);
	writeb(0xff, chip->ext_regs + 0x304);
	writeb(0x00, chip->ext_regs + 0x100);
	writeb(0xff, chip->ext_regs + 0x304);
	writeb(0x00, chip->ext_regs + 0x100);
	writeb(0xff, chip->ext_regs + 0x304);
	writeb(0x00, chip->ext_regs + 0x100);
	writeb(0xff, chip->ext_regs + 0x304);
	writeb(0x00, chip->ext_regs + 0x100);
	writeb(0xff, chip->ext_regs + 0x304);

	/* Step 4: Engine reset */
	creative_ext_writel(chip, 0x86c, 0x00);
	msleep(7);
	creative_ext_writel(chip, 0x800, 0x6b);
	creative_ext_writel(chip, 0x86c, 0x01);
	msleep(16);

	/* Step 5: New handshake start */
	creative_ext_writel(chip, 0x800, 0x6b);
	creative_ext_writel(chip, 0x804, 0x48);
	creative_ext_writel(chip, 0x20c, 0x00800005);

	/* GPIO5 in middle of handshake */
	ca0113_mmio_gpio_set(chip, 5, true);

	/* Sync */
	creative_ext_writel(chip, 0x210, 0x7e);
	creative_ext_writel(chip, 0x210, 0x5a);
	msleep(1);
	dev_info(chip->dev, "CA0113 second reset: sync=0x%02x\n",
		 creative_ext_readl(chip, 0x210) & 0xff);

	/* Mode 3 + token */
	creative_ext_writel(chip, 0x20c, 0x00800005);
	creative_ext_writel(chip, 0x804, 0x48);
	creative_ext_writel(chip, 0x20c, 0x00800003);
	creative_ext_writel(chip, 0x204, 0x07);
	msleep(16);
	dev_info(chip->dev, "CA0113 second reset: 0x860=%d\n",
		 creative_ext_readl(chip, 0x860));

	creative_ext_writel(chip, 0x20c, 0x00800003);
	creative_ext_writel(chip, 0x208, 0xffff);
	msleep(16);

	/* Step 6: End handshake, re-sync */
	creative_ext_writel(chip, 0x20c, 0x00800002);
	creative_ext_writel(chip, 0x210, 0x00);
	creative_ext_writel(chip, 0x210, 0x7e);
	creative_ext_writel(chip, 0x210, 0x5a);
	msleep(1);

	/* Step 7: Post-handshake DAC commands — group 0x48 */
	ca0113_mmio_command_set(chip, 0x48, 0x07, 0x81);
	ca0113_mmio_command_set(chip, 0x48, 0x1d, 0x40);
	ca0113_mmio_command_set(chip, 0x48, 0x01, 0xc0);
	ca0113_mmio_command_set(chip, 0x48, 0x0a, 0x02);
	ca0113_mmio_command_set_type2(chip, 0x48, 0x0b, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x0b, 0x20);
	ca0113_mmio_command_set(chip, 0x48, 0x04, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x06, 0x40);
	ca0113_mmio_command_set(chip, 0x48, 0x08, 0xff);
	ca0113_mmio_command_set(chip, 0x48, 0x1d, 0x40);
	ca0113_mmio_command_set(chip, 0x48, 0x0a, 0x06);
	ca0113_mmio_command_set(chip, 0x48, 0x0c, 0x5f);
	ca0113_mmio_command_set(chip, 0x48, 0x11, 0xff);
	ca0113_mmio_command_set(chip, 0x48, 0x12, 0xff);
	ca0113_mmio_command_set(chip, 0x48, 0x13, 0xff);
	ca0113_mmio_command_set(chip, 0x48, 0x14, 0x7f);
	ca0113_mmio_command_set_type2(chip, 0x48, 0x0e, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x0e, 0x8a);
	ca0113_mmio_command_set_type2(chip, 0x48, 0x07, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x07, 0x81);
	ca0113_mmio_command_set(chip, 0x48, 0x0f, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x10, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x0a, 0x02);
	/* Group 0x49 */
	ca0113_mmio_command_set(chip, 0x49, 0x18, 0xff);
	ca0113_mmio_command_set(chip, 0x49, 0x19, 0xff);
	ca0113_mmio_command_set(chip, 0x49, 0x1a, 0x00);
	ca0113_mmio_command_set(chip, 0x49, 0x1b, 0x70);
	ca0113_mmio_command_set(chip, 0x49, 0x00, 0xff);
	ca0113_mmio_command_set(chip, 0x49, 0x01, 0xff);
	ca0113_mmio_command_set(chip, 0x49, 0x06, 0xff);
	ca0113_mmio_command_set(chip, 0x49, 0x07, 0xff);
	ca0113_mmio_command_set(chip, 0x49, 0x10, 0x7f);
	ca0113_mmio_command_set(chip, 0x49, 0x0a, 0x07);

	dev_info(chip->dev, "CA0113 second reset: complete\n");
	return 0;
}

int creative_dsp_select_out_params(struct creative_chip *chip)
{
	dev_info(chip->dev, "select_out_params: configuring DSP output path\n");

	/*
	 * Step 0: ae9_ca0113_output_presets — 4 ca0113 commands for speaker path.
	 * Extracted from binary .rodata at offset 0x2940 (sym ae9_ca0113_output_presets).
	 * ca0132_alt_select_out lines 20279-20323: loop over presets[jack_present=0].
	 *
	 * struct ae_ca0113_output_set { uint group[6], target[6], vals[2][6] }
	 * Headphone (jack=1) values — card shows -HP- display, HP jack in use:
	 *   [0] group=0x48 target=0x1d val=0x80
	 *   [1] group=0x48 target=0x0d val=0x40
	 *   [2] group=0x48 target=0x17 val=0x00
	 *   [3] group=0x48 target=0x19 val=0x00
	 */
	ca0113_mmio_command_set(chip, 0x48, 0x1d, 0x80); /* HP path */
	ca0113_mmio_command_set(chip, 0x48, 0x0d, 0x40); /* HP path */
	ca0113_mmio_command_set(chip, 0x48, 0x17, 0x00);
	ca0113_mmio_command_set(chip, 0x48, 0x19, 0x00);

	/*
	 * Step 0b: HDA codec pin configuration for headphone output path.
	 * ca0132_alt_select_out headphone branch (jack_present=1):
	 *
	 * For AE-9 (quirk 9), out_pins[] = {0x0b, 0x11, 0x10, 0x0f}.
	 * Card shows -HP- on display → headphone jack in use.
	 *
	 * HP path pin config (from decompiled ca0132_alt_select_out HP branch):
	 *   nid=0x0b: EAPD=1, pin_ctl=0x00 (HP amp off for speaker, but keep EAPD)
	 *   nid=0x10: pin_ctl=0x00 (line out disabled)
	 *   nid=0x0f: pin_ctl=0x00 (line out disabled)
	 *   nid=0x11: pin_ctl=0xc0 (HP+OUT enabled — this is the front HP jack)
	 *
	 * chipio_set_control_param(0xd, 0xa1) for HP path (from quirk_out_set_data[9])
	 */

	/* EAPD enable: nid=0x0b */
	dsp_write(chip, 0x0b, 0x70c, 1);
	dev_info(chip->dev, "select_out_params: EAPD enabled on nid=0x0b\n");

	/* Pin widget control: HP path */
	dsp_write(chip, 0x0b, 0x707, 0x00); /* speaker DAC off for HP mode */
	dsp_write(chip, 0x10, 0x707, 0x00); /* line out disabled */
	dsp_write(chip, 0x0f, 0x707, 0x00); /* line out disabled */
	dsp_write(chip, 0x11, 0x707, 0xc0); /* HP jack ENABLED (OUT+HP bits) */
	dev_info(chip->dev, "select_out_params: pin_ctl set HP path (0x11=0xc0)\n");

	/* ChipIO control param 0xd = 0xa1 (AE-9 HP path routing) */
	chipio_set_control_param(chip, 0x0d, 0xa1);
	dev_info(chip->dev, "select_out_params: chipio param 0xd=0xa1 sent\n");

	/*
	 * Step 1: Mute speaker-tuning module during reconfiguration.
	 * ca0132_alt_select_out line 20241: dspio_set_uint_param(0x96, 0x3a, FLOAT_ONE)
	 */
	dspio_set_uint_param(chip, 0x96, SPEAKER_TUNING_MUTE, FLOAT_ONE);

	/*
	 * Step 2: Channel configuration — 2.0 stereo output path.
	 * ca0132_alt_select_out line ~20451: dspio_set_uint_param(0x80, 4, 0x41000000)
	 * 0x41000000 = IEEE 754 float 8.0 = SPEAKER_CHANNELS_2_0
	 */
	dspio_set_uint_param(chip, 0x80, 0x04, 0x41000000u);

	/*
	 * Step 3: Disable ChromaFX / surround processing.
	 * ca0132_alt_select_out line 20485: dspio_set_uint_param(0x8f, 1, 0)
	 */
	dspio_set_uint_param(chip, 0x8f, 0x01, FLOAT_ZERO);

	/*
	 * Step 4: Disable speaker EQ inversion.
	 * ca0132_alt_select_out line 20486: dspio_set_uint_param(0x96, 0x1f, 0)
	 */
	dspio_set_uint_param(chip, 0x96, SPEAKER_TUNING_USE_SPEAKER_EQ, FLOAT_ZERO);

	/*
	 * Step 5: Disable bass redirection.
	 * ca0132_alt_surround_set_bass_redirection(false) line 17994:
	 *   dspio_set_uint_param(0x96, 0x15, 0)
	 */
	dspio_set_uint_param(chip, 0x96, 0x15, FLOAT_ZERO);

	/*
	 * Step 6: UNMUTE — this is the gate that opens the audio path.
	 * ca0132_alt_select_out line 20494: dspio_set_uint_param(0x96, 0x3a, 0)
	 * FLOAT_ZERO = unmuted = audio flows to the output.
	 */
	dspio_set_uint_param(chip, 0x96, SPEAKER_TUNING_MUTE, FLOAT_ZERO);

	/*
	 * Step 7: Full-range speaker — AE-9 specific.
	 * ca0132_alt_set_full_range_speaker() line 18106 for quirk IDs 7–9:
	 *   dspio_set_uint_param(0x96, 0x1d, FLOAT_ONE)
	 * Enables full-range output on the AE-9 speaker path.
	 */
	dspio_set_uint_param(chip, 0x96, 0x1d, FLOAT_ONE);

	/*
	 * Step 8: Set playback volume to 0 dB (full volume).
	 * ca0132_alt_volume_put() sends these when the user adjusts the knob.
	 * ca0132_alt_vol_ctls[0]: mid=0x80, reqs=[0x32, 0x03, 0x04]
	 * float_vol_db_lookup[90] = 0x00000000 = 0.0 dB = FLOAT_ZERO
	 * Without these, the DSP volume output stage may be at 0 (muted).
	 */
	dspio_set_uint_param(chip, 0x80, 0x32, FLOAT_ZERO); /* left vol  = 0dB */
	dspio_set_uint_param(chip, 0x80, 0x03, FLOAT_ZERO); /* right vol = 0dB */
	dspio_set_uint_param(chip, 0x80, 0x04, FLOAT_ZERO); /* third req = 0   */

	dev_info(chip->dev, "select_out_params: DSP output path open\n");
	return 0;
}

int creative_dsp_headunit_dac_route(struct creative_chip *chip)
{
	/*
	 * Byte-exact from cold-boot MMIO capture (01:28:07.0-08.12).
	 * Two bursts decoded from region2+0x204 writes with group context
	 * from region2+0x804:
	 *
	 * Burst 1 (stream bind time): programs the head-unit DAC routing,
	 *   gain, and output enable. Key fix: channel regs 0x49/0x0a=0x07
	 *   (not 0x06), 0x48/0x1d=0x40 (not 0x80), 0x48/0x0c=0x5f (not 0x40).
	 *
	 * Burst 2 (150-600ms after): sets DAC output level to 0x3b on all
	 *   six output channel regs (0x48/0x0f,0x10 and 0x49/0x04,0x05,0x02,0x03).
	 *   CRITICAL: previous value 0x1e was too low — no audible output.
	 *   Cold boot uses 0x3b, QEMU-reset uses 0xff; 0x3b confirmed correct.
	 */
	static const u8 seq[][3] = {
		/* Burst 1 */
		{ 0x48, 0x08, 0xff },
		{ 0x48, 0x1d, 0x40 },
		{ 0x48, 0x0a, 0x06 },
		{ 0x48, 0x0c, 0x5f },
		{ 0x48, 0x11, 0xff }, { 0x48, 0x12, 0xff },
		{ 0x48, 0x13, 0xff }, { 0x48, 0x14, 0x7f },
		{ 0x48, 0x0e, 0x00 }, { 0x48, 0x0e, 0x8a },
		{ 0x48, 0x07, 0x00 }, { 0x48, 0x07, 0x81 },
		{ 0x48, 0x0f, 0x00 }, { 0x48, 0x10, 0x00 },
		{ 0x48, 0x0a, 0x02 },
		{ 0x49, 0x18, 0xff }, { 0x49, 0x19, 0xff },
		{ 0x49, 0x1a, 0x00 }, { 0x49, 0x1b, 0x70 },
		{ 0x49, 0x00, 0xff }, { 0x49, 0x01, 0xff },
		{ 0x49, 0x06, 0xff }, { 0x49, 0x07, 0xff },
		{ 0x49, 0x10, 0x7f },
		{ 0x49, 0x0a, 0x07 },
		{ 0x48, 0x07, 0x00 }, { 0x48, 0x07, 0x80 },
		{ 0x49, 0x0a, 0x06 },
		/* Burst 2: DAC channel output level = 0x3b */
		{ 0x48, 0x0f, 0x3b }, { 0x48, 0x10, 0x3b },
		{ 0x49, 0x04, 0x3b }, { 0x49, 0x05, 0x3b },
		{ 0x49, 0x02, 0x3b }, { 0x49, 0x03, 0x3b },
	};
	int i;

	dev_info(chip->dev, "head-unit DAC route: replaying %zu cmds\n",
		 ARRAY_SIZE(seq));
	for (i = 0; i < ARRAY_SIZE(seq); i++)
		ca0113_play_cmd(chip, seq[i][0], seq[i][1], seq[i][2]);
	return 0;
}

/*
 * INIT-TIME head-unit DAC route — byte-exact from cold-boot MMIO capture
 * (01:28:06.562 to 01:28:07.433), the full target=0x48/0x49 command set
 * that runs between firmware load and the first 0x83[01,07]=0x0a response.
 * This is what makes the head unit MCU report 0x0a (serial clock detected)
 * and enables the relay to click.
 *
 * Previously we sent 0x49/0x0a=0x06 (wrong) and were missing most of the
 * 0x49 channel registers. Capture shows 0x49/0x0a=0x07 and many more regs.
 */
int creative_dsp_headunit_init_route(struct creative_chip *chip)
{
	static const u8 seq[][3] = {
		/* 0x48 control setup (01:28:06.562-701) */
		{ 0x48, 0x0d, 0x40 },
		{ 0x48, 0x16, 0x00 }, { 0x48, 0x17, 0x00 },
		{ 0x48, 0x18, 0x00 }, { 0x48, 0x19, 0x00 },
		{ 0x48, 0x11, 0xff }, { 0x48, 0x12, 0xff },
		{ 0x48, 0x13, 0xff }, { 0x48, 0x14, 0x7f },
		{ 0x48, 0x1d, 0x40 },  /* confirmed 0x40 from cold-boot playback */
		/* 0x49 enable + channel setup (01:28:06.828) */
		{ 0x49, 0x0a, 0x07 },
		{ 0x48, 0x1d, 0x40 },
		/* More 0x48 control (01:28:07.002-129) */
		{ 0x48, 0x02, 0x00 }, { 0x48, 0x04, 0x00 },
		{ 0x48, 0x06, 0x40 }, { 0x48, 0x08, 0xff },
		{ 0x48, 0x01, 0xc0 },
		{ 0x48, 0x0b, 0x00 }, { 0x48, 0x02, 0x00 },
		{ 0x48, 0x0c, 0x5f },  /* confirmed 0x5f from cold-boot playback */
		/* 0x48 volume regs (01:28:07.065-129) */
		{ 0x48, 0x11, 0xff }, { 0x48, 0x12, 0xff },
		{ 0x48, 0x13, 0xff }, { 0x48, 0x14, 0x7f },
		{ 0x48, 0x0e, 0x8a },
		{ 0x48, 0x07, 0x00 }, { 0x48, 0x07, 0x81 },
		{ 0x48, 0x0f, 0x00 }, { 0x48, 0x10, 0x00 },
		{ 0x48, 0x02, 0x00 },
		/* 0x49 channel volume regs (01:28:07.274-417) */
		{ 0x49, 0x18, 0xff }, { 0x49, 0x19, 0xff },
		{ 0x49, 0x1a, 0x00 }, { 0x49, 0x1b, 0x70 },
		{ 0x49, 0x00, 0xff }, { 0x49, 0x01, 0xff },
		{ 0x49, 0x06, 0xff }, { 0x49, 0x07, 0xff },
		{ 0x49, 0x10, 0x7f },
		{ 0x49, 0x0a, 0x07 },  /* repeated with confirmed 0x07 */
	};
	int i;

	dev_info(chip->dev, "head-unit INIT DAC route: %zu cmds\n",
		 ARRAY_SIZE(seq));
	for (i = 0; i < ARRAY_SIZE(seq); i++)
		ca0113_play_cmd(chip, seq[i][0], seq[i][1], seq[i][2]);
	return 0;
}

/*
 * creative_dsp_post_relay_dac_levels — ES9038 DAC output levels after relay.
 *
 * ae9_build ae9_setup_defaults() line 10831: sends these 4 commands after
 * ae9_acm_init() + GPIO4 + alt_select_out(). These are the final ES9038B
 * DAC output-level registers that set the analog output amplitude.
 *
 * Windows MMIO trace shows 0x1e for the post-relay value (not 0x3b which
 * is the playback burst level). ae9_build comment: "Windows MMIO trace
 * shows 0x1e here (not 0x37). These are ES9038B volume/filter preset regs."
 */
int creative_dsp_post_relay_dac_levels(struct creative_chip *chip)
{
	dev_info(chip->dev, "post-relay DAC levels: 0x49/ch=0x1e\n");
	ca0113_play_cmd(chip, 0x49, 0x04, 0x1e);
	ca0113_play_cmd(chip, 0x49, 0x05, 0x1e);
	ca0113_play_cmd(chip, 0x49, 0x02, 0x1e);
	ca0113_play_cmd(chip, 0x49, 0x03, 0x1e);
	return 0;
}

/* stream port remap data (mainline stream_remap_data[1], for 0x0c) */
static void chipio_remap_stream_0c(struct creative_chip *chip)
{
	static const unsigned int offset[] = {
		0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c,
		0x20, 0x24, 0x28, 0x2c };
	static const unsigned int value[] = {
		0x0001e0c0, 0x0001e1c1, 0x0001e4c2, 0x0001e5c3,
		0x0001e2c4, 0x0001e3c5, 0x0001e8c6, 0x0001e9c7,
		0x0001ecc8, 0x0001edc9, 0x0001eaca, 0x0001ebcb };
	unsigned int i, stream_offset = 0xff;

	for (i = 0; i < 5; i++) {
		chipio_8051_read_exram(chip, 0x1578 + 0x0c, &stream_offset);
		/* Valid offset is a single byte (0x00-0xfe); 0xff = not set.
		 * Values > 0xff = read error / uninitialised exram.
		 */
		if (stream_offset <= 0xfe)
			break;
		msleep(25);
	}
	dev_info(chip->dev, "remap_stream_0c: raw exram=0x%x\n", stream_offset);
	if (stream_offset > 0xfe) {
		dev_warn(chip->dev,
			 "remap_stream_0c: firmware did not set stream offset (got 0x%x) — writing at fixed address\n",
			 stream_offset);
		/* Fall through with offset=0 to write at router base directly */
		stream_offset = 0;
	}
	/* Offset is in 32-bit words; convert to byte offset + audio-router base */
	stream_offset *= 0x04;
	stream_offset += 0x190000;	/* audio-router base (was wrongly 0x40000) */
	dev_info(chip->dev, "remap_stream_0c: writing to 0x%x\n", stream_offset);
	for (i = 0; i < ARRAY_SIZE(offset); i++)
		creative_dsp_chipio_write(chip, stream_offset + offset[i],
					  value[i]);
	/* Trigger the router to update its configuration (mainline: 0x19042c=1) */
	creative_dsp_chipio_write(chip, 0x19042c, 0x00000001);
}

/* Public: write the audio router directly for live testing / fallback.
 * Used when firmware doesn't set the 8051 stream-offset table.
 *
 * From Windows cold-boot capture (L10970-L11068): the router entry
 * format is {enable[31:16], flags[15:8], dst[7:0]}.
 *   - enable = 0x0001 always
 *   - flags  = 0x00 (not src_port as we previously assumed)
 *   - dst    = FIXED 0x14 (L) / 0x15 (R) — NOT from EXRAM 0x1490/0x1491
 *             (those EXRAM values are DMA channel IDs, not router dst)
 *   - trigger = 0x19002c = 1 (not 0x19042c as we previously used)
 */
void creative_dsp_write_audio_router(struct creative_chip *chip)
{
	unsigned int port_offset = 0xff;
	unsigned int router_addr, port_map = 0;

	dsp_allocate_router_ports(chip, 2, 1, 0, &port_map);

	chipio_8051_read_exram(chip, 0x157d, &port_offset);
	port_offset &= 0xff;

	if (port_offset > 0xfe) {
		dev_warn(chip->dev,
			 "audio router: EXRAM port_offset not ready (0x%x)\n",
			 port_offset);
		return;
	}

	router_addr = 0x190000 + port_offset * 4;

	creative_dsp_chipio_write(chip, router_addr,     0x00010014);
	creative_dsp_chipio_write(chip, router_addr + 4, 0x00010015);
	creative_dsp_chipio_write(chip, 0x19002c,        0x00000001);

	dev_info(chip->dev,
		 "audio router: wrote to 0x%06x {dst=0x14,0x15} trigger=0x19002c=1\n",
		 router_addr);
}

/*
 * Start the DSP's internal audio-router streams (mainline
 * ca0132_alt_start_dsp_audio_streams). Stream 0x0c is the 6-channel main
 * playback path; 0x03/0x04 are the others. Without these started, audio
 * never flows through the DSP to the analog outputs.
 */
int creative_dsp_start_audio_streams(struct creative_chip *chip)
{
	unsigned int dbgcntl = 0, halt;
	unsigned int active_chans = 0;

	/*
	 * Pre-cleanup — matching decompiled ca0132_alt_start_dsp_audio_streams.
	 *
	 * For each stream (0x0c, 3, 4): check if a DMA channel is assigned
	 * (verb 0xf10 p=0x1c). If so:
	 *   1. chipio_set_stream_control(stream, 0) — stop the stream
	 *   2. dspio_scp(mod=0x80, src_id=0x0a, req=0) — tell 8051 to free
	 *      the DMA channel from its internal allocation table
	 *
	 * Without step 2, the 8051's internal table still shows the channel
	 * allocated, and re-enabling the stream causes halt=1 (firmware hits
	 * an assert because DMA channel is "already in use").
	 *
	 * The SCP call must be sent AFTER HELLO ACK with the 8051 running.
	 * At this point (inside creative_dsp_start_audio_streams, called from
	 * standup after HELLO ACK + 500ms settle), the 8051 audio loop is
	 * active and will process the SCP command.
	 */
	{
		static const int stream_ids[] = { 0x0c, 3, 4 };
		int i;
		for (i = 0; i < 3; i++) {
			int dma;
			chipio_set_control_param(chip, CONTROL_PARAM_STREAM_ID,
						 stream_ids[i]);
			dma = dsp_read(chip, WIDGET_CHIP_CTRL, 0xf10, 0x1c);
			if (dma) {
				unsigned int chan_data = (unsigned int)dma;
				dev_info(chip->dev,
					 "streams: freeing stream 0x%x DMA chan %d\n",
					 stream_ids[i], dma);
				int t;
				unsigned int sts;
				chipio_set_stream_control(chip, stream_ids[i], 0);
				/* Tell 8051 the DMA channel is now free.
				 * mod=0x80, src_id=0x0a, req=0, SCP_SET.
				 * Poll until 8051 consumes the command before
				 * re-enabling the stream. */
				dspio_scp(chip, 0x80, 0x0a, 0, SCP_SET,
					  &chan_data, sizeof(chan_data),
					  NULL, NULL);
				/* Wait for 8051 to process: poll SCP status */
				for (t = 0; t < 100; t++) {
					msleep(5);
					sts = dsp_read(chip, WIDGET_DSP_CTRL,
						       VENDOR_DSPIO_STATUS, 0);
					if (sts == VENDOR_STATUS_DSPIO_OK ||
					    sts == VENDOR_STATUS_DSPIO_SCP_RESPONSE_QUEUE_EMPTY)
						break;
				}
				dev_info(chip->dev,
					 "streams: free DMA chan %d done (sts=0x%x t=%d)\n",
					 dma, sts, t);
				dspio_clear_response_queue(chip);
			}
		}
	}

	if (creative_dsp_chipio_read(chip, 0xff0, &active_chans) == 0)
		dev_info(chip->dev, "streams: 0xff0=0x%x after cleanup\n",
			 active_chans);

	/*
	 * Enable all three streams — matching decompiled
	 * ca0132_alt_start_dsp_audio_streams() exactly (lines 18637–18705):
	 *
	 *   chipio_set_stream_channels(0x0c, 6)
	 *   chipio_set_stream_control(0x0c, 1)
	 *   msleep(75)
	 *   chipio_set_stream_control(3, 1)
	 *   msleep(75)
	 *   chipio_set_stream_control(4, 1)
	 *   msleep(75)
	 *
	 * Streams 3 and 4 are mic-input/processing paths. The 8051 audio loop
	 * requires all three streams active before it starts its per-frame
	 * processing counter (0x801c). Without streams 3 and 4, 0x801c stays
	 * at 0 and no audio reaches the output even with DMA running.
	 *
	 * The previous concern about halt=1 from enabling stream 0x0c here
	 * was specific to the old pre-HELLO-ACK timing. At this point the
	 * 8051 audio loop is running (post-HELLO-ACK) and ae9_post_dsp_asi_setup
	 * will re-enable 0x0c anyway — enabling it here first matches the
	 * decompiled order exactly.
	 */
	/*
	 * Stream 0x0c: set channels only — do NOT enable here.
	 * ae9_post_dsp_asi_setup() enables it after PLL/ASI setup.
	 * Double-enabling causes halt=1 (8051 debug breakpoint).
	 */
	chipio_set_stream_channels(chip, 0x0c, 6);
	msleep(75);

	/*
	 * Streams 3 and 4: restore source/dest routing before enabling.
	 *
	 * From stream_data_d1.txt (Windows active playback state):
	 *   Stream 3: SourceConnID=0x64, DestConnID=0x03
	 *   Stream 4: SourceConnID=0x63, DestConnID=0x05
	 *
	 * ConnIDs 0x64/0x63 are the HDA playback DMA output connection points
	 * that feed audio data from the host into the DSP.  ConnIDs 0x03/0x05
	 * are the DSP DMA input ports.
	 *
	 * The decompiled driver relies on the init_verbs replay to set these
	 * before standup.  After our AFG hardware reset (verb 0x7ff) the 8051
	 * SCP state and ChipIO stream registers may be cleared, so we set them
	 * explicitly here to guarantee correct routing regardless of reset state.
	 *
	 * Without this, streams 3+4 are enabled with no defined source and no
	 * audio data flows into the DSP — 0x801c stays zero permanently.
	 */
	chipio_set_stream_source_dest(chip, 3, 0x64, 0x03);
	chipio_set_stream_source_dest(chip, 4, 0x63, 0x05);
	chipio_set_stream_control(chip, 3, 1);
	msleep(75);
	chipio_set_stream_control(chip, 4, 1);
	msleep(75);

	/* Log current DBGCNTL */
	creative_dsp_chipio_read(chip, DSP_DBGCNTL_INST_OFFSET, &dbgcntl);
	halt = (dbgcntl & DSP_DBGCNTL_STATE_MASK) >> DSP_DBGCNTL_STATE_LOBIT;
	dev_info(chip->dev,
		 "streams: after channel set DBGCNTL=0x%x halt=%u\n",
		 dbgcntl, halt);

	dev_info(chip->dev, "DSP audio streams: 0x0c+3+4 enabled\n");
	return 0;
}

/*
 * AE-9 post-firmware DSP standup.
 * Sequence from decompiled Creative CA0132 v0.73_kernel18 ae9_setup_defaults:
 *   1. Control flags (DSP/DAC 96KHz → 0)
 *   2. Start stream 0x0c (6ch)
 *   3. select_lineout → dspio params + ae9_post_dsp_asi_setup
 */
/* Log SCP status (node 0x16 verb 0xf01) at a named checkpoint to find where
 * the SCP command queue jams during standup. 0x0=ok, 0x2=queue full. */
static void scp_ckpt(struct creative_chip *chip, const char *where)
{
	u32 sts = dsp_read(chip, WIDGET_DSP_CTRL, VENDOR_DSPIO_STATUS, 0);
	unsigned int loop = 0, dbgcntl = 0;
	unsigned int halt;

	/*
	 * Only issue ChipIO reads (CORB verbs) when SCP status is healthy.
	 * If the 8051 is halted, ChipIO reads never get RIRB responses and
	 * jam the CORB ring — causing all subsequent verb traffic to fail.
	 * SCP=0x3 means the queue is idle and the 8051 is servicing commands.
	 */
	if (sts == 0x3) {
		creative_dsp_chipio_read(chip, 0x801c, &loop);
		creative_dsp_chipio_read(chip, DSP_DBGCNTL_INST_OFFSET, &dbgcntl);
	}
	halt = (dbgcntl & DSP_DBGCNTL_STATE_MASK) >> DSP_DBGCNTL_STATE_LOBIT;
	dev_info(chip->dev,
		 "ckpt [%s]: SCP=0x%x%s 0x801c=0x%x%s halt=%d\n",
		 where, sts,
		 sts == 0x2 ? "(FULL)" : "",
		 loop,
		 loop ? " <-- AUDIO LOOP ALIVE" : " (dead)",
		 halt);
}

/* Forward declaration — ae9_set_sample_rate_low is defined below ae9_post_dsp_asi_setup
 * but called from creative_dsp_ae9_standup which precedes both. */
static void ae9_set_sample_rate_low(struct creative_chip *chip);

int creative_dsp_ae9_standup(struct creative_chip *chip)
{
	/*
	 * Clear any residual halt state (e.g. halt=9 step-halt left by a
	 * previous boot's self-reset) BEFORE any ChipIO operations. If the
	 * 8051 is halted when scp_ckpt or chipio_set_conn_rate issues a CORB
	 * verb, the verb never gets a RIRB response and jams the CORB ring.
	 */
	dsp_set_run_state(chip);
	dspio_clear_response_queue(chip);

	scp_ckpt(chip, "standup-entry");

	/*
	 * ae9_post_dsp_mmio_commands — 8 ca0113 group 0x48 pulses.
	 * First step of decompiled ae9_setup_defaults (lines 26027-26034),
	 * runs BEFORE ca0132_alt_init_analog_mics. Initializes head-unit DAC
	 * control registers from a known state. Registers 0x0d, 0x17, 0x19
	 * are never written anywhere else in our driver.
	 */
	ca0113_play_cmd(chip, 0x48, 0x0d, 0x00);
	ca0113_play_cmd(chip, 0x48, 0x17, 0x00);
	ca0113_play_cmd(chip, 0x48, 0x19, 0x00);
	ca0113_play_cmd(chip, 0x48, 0x11, 0xff);
	ca0113_play_cmd(chip, 0x48, 0x12, 0xff);
	ca0113_play_cmd(chip, 0x48, 0x13, 0xff);
	ca0113_play_cmd(chip, 0x48, 0x14, 0x7f);
	ca0113_play_cmd(chip, 0x48, 0x1d, 0x80);
	dev_info(chip->dev, "standup: ae9_post_dsp_mmio_commands done\n");

	/*
	 * No control flag manipulation here.
	 *
	 * The decompiled CA0132 v0.73_kernel18 ae9_setup_defaults does NOT
	 * set any chipio_set_control_flag calls. The 96KHz flags (DSP_96KHZ,
	 * DAC_96KHZ) are set to TRUE in ca0132_codec_init before firmware
	 * download — handled in creative_dsp_prepare() below. The other flags
	 * (SPDIF2OUT, PORT_D/A_10KOHM_LOAD) are not set in the AE-9 path at all.
	 * CONTROL_FLAG_IDLE_ENABLE is defined in the enum but never called
	 * anywhere in the decompiled driver.
	 */

	scp_ckpt(chip, "after-control-flags");

	/*
	 * ca0132_alt_init_analog_mics — from decompiled ae9_setup_defaults
	 * (line 26035), called as step 2 BEFORE ca0132_alt_start_dsp_audio_streams.
	 *
	 * Sets four connection point sample rates to SR_96_000 and sends the
	 * initial output configuration params. For AE-9 (quirk != 5):
	 *   chipio_set_conn_rate(3, SR_96_000)
	 *   chipio_set_conn_rate(0xc, SR_96_000)
	 *   dspio_set_uint_param(0x80, 0, FLOAT_THREE)  (3.0f = 0x40400000)
	 *   chipio_set_conn_rate(5, SR_96_000)
	 *   chipio_set_conn_rate(0xe, SR_96_000)
	 *   dspio_set_uint_param(0x80, 1, 0)
	 *
	 * Without this, connection points 3/5/c/e have no sample rate set,
	 * which may prevent the DSP audio path from routing samples to the 8051.
	 */
	chipio_set_conn_rate(chip, 3,    SR_96_000);
	chipio_set_conn_rate(chip, 0x0c, SR_96_000);
	dspio_set_uint_param(chip, 0x80, 0x00, 0x40400000u /* FLOAT_THREE */);
	chipio_set_conn_rate(chip, 5,    SR_96_000);
	chipio_set_conn_rate(chip, 0x0e, SR_96_000);
	dspio_set_uint_param(chip, 0x80, 0x01, FLOAT_ZERO);
	dev_info(chip->dev, "standup: ca0132_alt_init_analog_mics done\n");

	/* Start main DSP audio stream 0x0c (6 channels).
	 * Decompiled AE-9 driver starts only 0x0c here — streams 3 and 4
	 * are handled inside ae9_post_dsp_asi_setup if needed. */
	creative_dsp_start_audio_streams(chip);

	scp_ckpt(chip, "after-audio-streams");

	/*
	 * Call dsp_set_run_state() unconditionally after stream 3+4 enable.
	 *
	 * Observation from boot log comparison (session 19):
	 *   - Boot where halt=1 occurred during stream enable → 0x801c=1 (DSP ran)
	 *   - Boot where halt=0 (clean, no breakpoint) → 0x801c=0 (DSP silent)
	 *
	 * When the 8051 hits HALT (state=1) during stream enable, it is polling
	 * for a DMA channel to become available. Un-halting it causes it to
	 * resume via a specific code path that primes the audio output loop.
	 * When it runs the same code without hitting HALT (DMA immediately free),
	 * the audio loop is not primed.
	 *
	 * Option 1 fix: call dsp_set_run_state() unconditionally. If the 8051
	 * is already running (halt=0), the function is a no-op. If it halted,
	 * this resumes it. Either way we then wait 500ms for the SCP queue to
	 * drain — matching the halt=1 recovery timing that gave us 0x801c=1.
	 *
	 * The write to DBGCNTL via dsp_set_run_state() may itself have a
	 * priming effect on the 8051's audio loop state machine regardless of
	 * whether it was actually halted.
	 */
	dsp_set_run_state(chip);
	/* Give the 8051 time to drain its SCP queue after the stream-enable
	 * operations. 500ms matches the timing that gave us 0x801c=1. */
	msleep(500);
	dspio_clear_response_queue(chip);
	dspio_write_wait(chip);
	dspio_clear_response_queue(chip);
	dev_info(chip->dev, "standup: dsp_set_run_state called (unconditional, halt=1 or halt=0)\n");

	/* select_lineout handles the AE-9 dspio params and ae9_post_dsp_asi_setup
	 * from the decompiled driver. */
	creative_dsp_select_lineout(chip);

	/*
	 * Pre-initialise the audio router, conn-point, and stream remap here
	 * during standup while ChipIO is still responsive (before standup_done
	 * disables chipio_send retries). These are ChipIO-heavy (sync CORB
	 * reads for status polling) and must not run post-standup when the
	 * 8051 is in audio-loop mode. pcm_started is set so PCM start skips
	 * them on the first and all subsequent calls.
	 */
	creative_dsp_write_audio_router(chip);
	chipio_set_conn_rate(chip, 0x18, 0x14);
	chipio_set_control_param(chip, CONTROL_PARAM_CONN_POINT_ID, 0x17);
	chipio_set_control_param(chip, CONTROL_PARAM_CONN_POINT_SR, 0x0f);
	chipio_remap_stream_0c(chip);
	chip->pcm_started = 1; /* mark done so PCM start skips these */

	/*
	 * ca0132_alt_init_speaker_tuning() — 20 DSPIO writes to module 0x96.
	 * Decompiled driver calls this after ae9_post_dsp_asi_setup (line 26049).
	 * Sets EQ and delay defaults for the speaker tuning module.
	 * Send in two batches of 10 with a drain between to avoid queue overflow.
	 */
	{
		int r;
		/* Batch 1: regs 0x20-0x2e (13 writes) */
		dspio_set_uint_param(chip, 0x96, 0x20, FLOAT_ZERO);
		for (r = 0x21; r <= 0x26; r++)
			dspio_set_uint_param(chip, 0x96, r, FLOAT_ZERO);
		dspio_write_wait(chip);
		dspio_clear_response_queue(chip);
		for (r = 0x29; r <= 0x2e; r++)
			dspio_set_uint_param(chip, 0x96, r, FLOAT_ZERO);
		dspio_write_wait(chip);
		dspio_clear_response_queue(chip);
		/* Batch 2: regs 0x31-0x36 (6 writes) */
		for (r = 0x31; r <= 0x36; r++)
			dspio_set_uint_param(chip, 0x96, r, FLOAT_ZERO);
		dspio_write_wait(chip);
		dspio_clear_response_queue(chip);
	}
	dev_info(chip->dev, "standup: speaker_tuning defaults set\n");

	/*
	 * 6 final ca0113 pulses from ae9_setup_defaults (decompiled lines
	 * 26080-26085), sent after ca0132_alt_init_speaker_tuning.
	 * Followed by ae9_set_sample_rate_low (1 call) as the very last step
	 * of ae9_setup_defaults (line 26086).
	 */
	ca0113_play_cmd(chip, 0x48, 0x0f, 0);
	ca0113_play_cmd(chip, 0x48, 0x10, 0);
	ca0113_play_cmd(chip, 0x49, 4, 0);
	ca0113_play_cmd(chip, 0x49, 5, 0);
	ca0113_play_cmd(chip, 0x49, 2, 0);
	ca0113_play_cmd(chip, 0x49, 3, 0);
	ae9_set_sample_rate_low(chip);

	/* Drain fully before select_out_params to ensure 0x96/0x3a lands. */
	dspio_write_wait(chip);
	dspio_clear_response_queue(chip);
	msleep(100);
	dspio_clear_response_queue(chip);

	/*
	 * ca0132_alt_select_out() equivalent — open the DSP audio output path.
	 * Must run HERE, before standup_done, while SCP/ChipIO are responsive.
	 * All 7 dspio_set_uint_param calls use dsp_read() (sync CORB); after
	 * standup_done the fast-path returns -EIO and nothing goes through.
	 */
	creative_dsp_select_out_params(chip);

	/*
	 * DSP effects engine pipeline configuration — 1183-verb NID16 SCP block
	 * (coldboot CORB [4157-5339]). Must run after select_out_params and before
	 * standup_done so the burst can use the sync CORB path.
	 *
	 * Configures DSP module 0x96 (the audio effects engine) with gain/EQ/routing.
	 * Without this the DSP audio loop never activates and 0x801c stays zero.
	 */
	chip->standup_done = 1; /* set early so burst guard passes */
	creative_dsp_audio_config(chip);
	chip->standup_done = 0; /* reset until we're truly done */

	/*
	 * Mark standup complete. This disables the keepalive CORB read in
	 * creative_dsp_keepalive() and enables the chipio_send fast path
	 * (single attempt, no retry) for post-standup operations.
	 */
	chip->standup_done = 1;
	dev_info(chip->dev, "AE-9 DSP standup complete\n");
	return 0;
}

/*
 * Enable the codec-side DAC output amps + coefficients. This is the
 * playback-enable / unmute step the real AE-9 driver does at init (capture
 * BOOT/INIT lines 3636+), right after the DSP router wiring. Without it the
 * DSP processes audio but the DAC output amps are not gained/unmuted, so no
 * analog signal — even with routing + DAC2PORT + DSP unmute all correct.
 *
 * Byte-exact from the 0x0071 verb transcript. Per DAC node (0x02/0x03/0x04):
 *   VENDOR(0x797)=0x00, COEF(0x3a0)=0xda then 0x5a,
 *   VENDOR(0x798)=0x00, COEF(0x390)=0xda then 0x5a.
 * Node 0x15 chip-ctrl: SET_AMP_GAIN_MUTE(0x300)=0xf0.
 * Node 0x07 (What U Hear): VENDOR(0x797)=0, COEF(0x360)=0x5d,
 *   VENDOR(0x798)=0, COEF(0x350)=0x5d.
 */
int creative_dsp_enable_dac_output(struct creative_chip *chip)
{
	static const u8 dac_nodes[] = { 0x02, 0x03, 0x04 };
	int i;

	dev_info(chip->dev, "enabling DAC output amps/coefs\n");

	/*
	 * All verbs here are SET_* writes (0x797/0x798 = vendor SET,
	 * 0x3a0/0x390/0x360/0x350 = SET_COEFF_DATA writes, 0x300 = SET_AMP).
	 * Use dsp_write_nowait() — after standup the 8051 is in audio-loop
	 * mode and may not service CORB promptly, causing verb_send() to time
	 * out and jam the CORB ring. Write-only verbs don't need RIRB confirmation.
	 */

	/* chip-ctrl widget unmute */
	dsp_write_nowait(chip, 0x15, 0x300, 0xf0);

	for (i = 0; i < ARRAY_SIZE(dac_nodes); i++) {
		u8 n = dac_nodes[i];

		dsp_write_nowait(chip, n, 0x797, 0x00);
		dsp_write_nowait(chip, n, 0x3a0, 0xda);
		dsp_write_nowait(chip, n, 0x3a0, 0x5a);
		dsp_write_nowait(chip, n, 0x798, 0x00);
		dsp_write_nowait(chip, n, 0x390, 0xda);
		dsp_write_nowait(chip, n, 0x390, 0x5a);
	}

	/* node 0x07 (What U Hear / secondary) */
	dsp_write_nowait(chip, 0x07, 0x797, 0x00);
	dsp_write_nowait(chip, 0x07, 0x360, 0x5d);
	dsp_write_nowait(chip, 0x07, 0x798, 0x00);
	dsp_write_nowait(chip, 0x07, 0x350, 0x5d);

	return 0;
}

/*
 * ae9_post_dsp_asi_setup — implemented from decompiled CA0132 v0.73_kernel18
 * lines 19749-19900. AE-9 specific. Configures:
 *  - BAR2 GPIO/amp registers
 *  - 8051 PLL (via chipio_8051_write_pll_pmu) — twice
 *  - Stream 0x18: ASI serial path source=conn_pt 9, dest=conn_pt 0xd0, 6ch
 *  - ca0113 group 0x48 (amp/headphone control) — ~18 commands
 *  - ca0113 group 0x49 (DAC channel control) — ~10 commands
 *  - SPI controller re-init (BAR2 0xC0C/0xC00/0xC04)
 *  - ae9_set_sample_rate_low (GPIO 0x320=0x105 + SPI SYS_INFO) — 4 times
 *
 * The function also calls ae5_post_dsp_param_setup internally — that is
 * renamed here to ae9_dsp_param_setup since it is part of the AE-9 path.
 *
 * Decompiled variants used:
 *  ca0113_mmio_command_set_pulse  → ca0113_play_cmd    (0x800005/0x800004)
 *  ca0113_mmio_command_set        → ca0113_mmio_command_set (0x800001/0x800003/0x800002)
 *  ca0113_mmio_command_set_pulse2 → ca0113_mmio_command_set_type2 (0x800003/0x800002)
 */
static void ae9_dsp_param_setup(struct creative_chip *chip)
{
	/* chipio_set_control_param(SPDIF1_SOURCE|VIP_SOURCE=3, 0) */
	chipio_set_control_param(chip, 3, 0);
	/* VENDOR_CHIPIO_FLAG_SET p=0x96 */
	dsp_write(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_FLAG_SET, 0x96);
	/* verb 0x724 p=0x83 */
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x724, 0x83);
	/* chipio_set_control_param(ASI, 0) */
	chipio_set_control_param(chip, CONTROL_PARAM_ASI, 0);
	/* chipio_8051_write_exram(0xfa92, 0x22) via verb 0x707 */
	chipio_8051_set_addr(chip, 0xfa92);
	dsp_write(chip, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_DATA_WRITE, 0x22);
}

static void ae9_set_sample_rate_low(struct creative_chip *chip)
{
	/* BAR2+0x320 = 0x105 (SPI/I2C mode select) */
	creative_ext_writew(chip, 0x320, 0x105);
	/* Send SPI SYS_INFO frame: f0 81 00 f7 via BAR2+0xC00 */
	creative_ext_writel(chip, 0xC00, 0xf0);
	creative_ext_writel(chip, 0xC00, 0x81);
	creative_ext_writel(chip, 0xC00, 0x00);
	creative_ext_writel(chip, 0xC00, 0xf7);
}

static void ae9_post_dsp_asi_setup(struct creative_chip *chip)
{
	/* PLL re-init (addrs 0xc8/0xcc/0xcb/0xc7/0xcd/0xce/0xdb) */
	static const u8 pll_old_a[] = {
		0xc8, 0xcc, 0xcb, 0xc7, 0xcd, 0xce, 0xdb
	};
	static const u8 pll_old_d[] = {
		0x44, 0x43, 0x45, 0x40, 0x42, 0x41, 0x51
	};
	/* Windows: address 0x44 data 0xc8, ... (capture seq 5384-5410) */
	const u8 *pll_addrs = pll_windows ? pll_old_d : pll_old_a;
	const u8 *pll_data = pll_windows ? pll_old_a : pll_old_d;
	unsigned int i;

	dev_info(chip->dev, "ae9_post_dsp_asi_setup: start\n");

	/* BAR2+0x320 = 0x103 x2 */
	creative_ext_writew(chip, 0x320, 0x103);
	creative_ext_writew(chip, 0x320, 0x103);

	/* verb 0x510/0x93 on nid=0x15 */
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x510, 0x93);

	/* chipio_8051_write_pll_pmu(0x44, 0xc2): not in the Windows capture */
	if (!pll_windows) {
		chipio_8051_write_pll_pmu(chip, 0x44, 0xc2);
		msleep(20); /* PLL relock — 8051 stops servicing CORB during relock */
	}

	/* BAR2 GPIO sequence: 0x304=0xff x5 interleaved with 0x100=0 x4 */
	creative_ext_writeb(chip, 0x304, 0xff);
	creative_ext_writeb(chip, 0x304, 0xff);
	creative_ext_writeb(chip, 0x304, 0xff);
	creative_ext_writeb(chip, 0x304, 0xff);
	creative_ext_writeb(chip, 0x100, 0x00);
	creative_ext_writeb(chip, 0x304, 0xff);
	creative_ext_writeb(chip, 0x100, 0x00);
	creative_ext_writeb(chip, 0x304, 0xff);
	creative_ext_writeb(chip, 0x100, 0x00);
	creative_ext_writeb(chip, 0x304, 0xff);
	creative_ext_writeb(chip, 0x100, 0x00);
	creative_ext_writeb(chip, 0x304, 0xff);

	/* BAR2: 0x86c=0, 0x800=0x6b, 0x86c=1, 0x800=0x6b, 0x804=0x48, 0x20c=5 */
	creative_ext_writel(chip, 0x86c, 0);
	creative_ext_writel(chip, 0x800, 0x6b);
	creative_ext_writel(chip, 0x86c, 1);
	creative_ext_writel(chip, 0x800, 0x6b);
	creative_ext_writel(chip, 0x804, 0x48);
	creative_ext_writel(chip, 0x20c, 5);


	/* BAR2+0x320 = 0x105 */
	creative_ext_writew(chip, 0x320, 0x105);

	/* ca0113_mmio_command_set_pulse2(7, 0x105, 1) */
	ca0113_mmio_command_set_type2(chip, 7, 0x105, 1);
	/* ca0113_mmio_command_set(0x48, 7, 0x81) */
	ca0113_mmio_command_set(chip, 0x48, 7, 0x81);
	/* ca0113_mmio_command_set_pulse(0x49, 10, 7) */
	ca0113_play_cmd(chip, 0x49, 10, 7);


	/* BAR2+0x320 = 2 */
	creative_ext_writew(chip, 0x320, 2);
	/* ca0113_mmio_command_set_pulse(0x48, 0x1d, 0x40) */
	ca0113_play_cmd(chip, 0x48, 0x1d, 0x40);

	/* BAR2+0x320 = 0x103, then 1 */
	creative_ext_writew(chip, 0x320, 0x103);
	creative_ext_writew(chip, 0x320, 1);

	/* ae5_post_dsp_param_setup equivalent (AE-9 path) */
	ae9_dsp_param_setup(chip);


	/* ae9_post_dsp_pll_setup (1st call) */
	for (i = 0; i < ARRAY_SIZE(pll_old_a); i++)
		chipio_8051_write_pll_pmu(chip, pll_addrs[i], pll_data[i]);
	msleep(20); /* PLL relock settle */
	dev_info(chip->dev, "ae9_post_dsp_pll_setup (1st)\n");

	/* verb 0x725/0x81 */
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x725, 0x81);

	/* chipio_set_conn_rate(0x70, SR_96_000) */
	chipio_set_conn_rate(chip, 0x70, SR_96_000);

	/* chipio_set_stream_channels(0x0c, 6) + chipio_set_stream_control(0x0c, 1) */
	chipio_set_stream_channels(chip, 0x0c, 6);
	chipio_set_stream_control(chip, 0x0c, 1);


	dev_info(chip->dev, "ae9_post_dsp_stream_setup\n");

	/* chipio_set_stream_source_dest(5, 0x43, 0) */
	chipio_set_stream_source_dest(chip, 5, 0x43, 0);
	/* chipio_set_stream_source_dest(0x18, 9, 0xd1) — dest=0xd1 confirmed from
	 * ae9_build-main/docs/stream_data_d1.txt (Windows active playback state).
	 * Previously used 0xd0 which was wrong. */
	chipio_set_stream_source_dest(chip, 0x18, 9, 0xd1);
	/* chipio_set_conn_rate(0xd1, SR_96_000) */
	chipio_set_conn_rate(chip, 0xd1, SR_96_000);
	/* chipio_set_stream_channels(0x18, 6) */
	chipio_set_stream_channels(chip, 0x18, 6);

	/*
	 * Activate ChipIO Stream 0x11 — the DSP DMA output pipeline.
	 *
	 * From ae9_build-main/docs/stream_data_d1.txt (Windows active playback):
	 *   Stream 0x11: SourceConnID=0x40 (DSP DMA output bus)
	 *                DestConnID=0x10   (DAC input bus)
	 *                Channels=0x08, Type=0x04, HDA Node=0x15, Active=1
	 *                HDA StreamID=5, HDA StreamFmt=0x0047
	 *
	 * This stream connects the DSP DMA engine output to the DAC input.
	 * Without it Active=1, 0x801c never increments (DSP DMA frozen).
	 * The decompiled driver never explicitly calls chipio_set_stream_control(0x11, 1)
	 * — it is activated implicitly by the ca0113 0x8107 command via the SCP bus.
	 * We explicitly activate it here to ensure it is wired correctly.
	 */
	chipio_set_stream_channels(chip, 0x11, 8);
	chipio_set_stream_source_dest(chip, 0x11, 0x40, 0x10);
	chipio_set_stream_control(chip, 0x11, 1);
	dev_info(chip->dev, "ae9_post_dsp_stream_setup: stream 0x11 activated (DSP DMA output pipeline)\n");

	/* ca0113_mmio_command_set_pulse(0x48, 1, 0xc0) */
	ca0113_play_cmd(chip, 0x48, 1, 0xc0);
	/* ca0113_mmio_command_set_pulse(0x48, 10, 2) */
	ca0113_play_cmd(chip, 0x48, 10, 2);

	/* chipio_set_stream_control(0x18, 1) */
	chipio_set_stream_control(chip, 0x18, 1);


	/* chipio_set_control_param(ASI, 8) */
	chipio_set_control_param(chip, CONTROL_PARAM_ASI, 8);


	/* 8051 PLL: addrs 0x40/0x42/0x41, data 199/0xcd/0xce */
	chipio_8051_set_address(chip, 0x40);
	chipio_8051_set_data_pll(chip, 199);
	chipio_8051_set_address(chip, 0x42);
	chipio_8051_set_data_pll(chip, 0xcd);
	chipio_8051_set_address(chip, 0x41);
	chipio_8051_set_data_pll(chip, 0xce);
	msleep(20); /* PLL relock settle */

	/* ca0113_mmio_command_set_pulse2(0xb, target, 2) — target unresolved, use 0 */
	ca0113_mmio_command_set_type2(chip, 0x0b, 0, 2);
	/* ca0113_mmio_command_set(0x48, 0x0b, 0x20) */
	ca0113_mmio_command_set(chip, 0x48, 0x0b, 0x20);
	/* ca0113_mmio_command_set_pulse(0x48, 4, 0) */
	ca0113_play_cmd(chip, 0x48, 4, 0);
	ca0113_play_cmd(chip, 0x48, 6, 0x40);
	ca0113_play_cmd(chip, 0x48, 8, 0xff);
	ca0113_play_cmd(chip, 0x48, 0x1d, 0x40);
	ca0113_play_cmd(chip, 0x48, 10, 6);
	ca0113_play_cmd(chip, 0x48, 0x0c, 0x5f);
	ca0113_play_cmd(chip, 0x48, 0x11, 0xff);
	ca0113_play_cmd(chip, 0x48, 0x12, 0xff);
	ca0113_play_cmd(chip, 0x48, 0x13, 0xff);
	ca0113_play_cmd(chip, 0x48, 0x14, 0x7f);


	/* ca0113_mmio_command_set_pulse2(0xe, target, 0x7f) */
	ca0113_mmio_command_set_type2(chip, 0x0e, 0, 0x7f);
	/* ca0113_mmio_command_set(0x48, 0xe, 0x8a) */
	ca0113_mmio_command_set(chip, 0x48, 0x0e, 0x8a);
	/* ca0113_mmio_command_set_pulse2(7, target, 0x8a) */
	ca0113_mmio_command_set_type2(chip, 7, 0, 0x8a);
	/* ca0113_mmio_command_set(0x48, 7, 0x81) */
	ca0113_mmio_command_set(chip, 0x48, 7, 0x81);


	/* ae9_post_dsp_pll_setup (2nd call) */
	for (i = 0; i < ARRAY_SIZE(pll_old_a); i++)
		chipio_8051_write_pll_pmu(chip, pll_addrs[i], pll_data[i]);
	msleep(20); /* PLL relock settle */
	dev_info(chip->dev, "ae9_post_dsp_pll_setup (2nd)\n");

	/* chipio_set_control_param(ASI, 0xf) */
	chipio_set_control_param(chip, CONTROL_PARAM_ASI, 0xf);


	ca0113_play_cmd(chip, 0x48, 0x0f, 0);
	ca0113_play_cmd(chip, 0x48, 0x10, 0);
	ca0113_play_cmd(chip, 0x48, 10, 2);

	/* Group 0x49 — DAC channel routing */
	ca0113_play_cmd(chip, 0x49, 0x18, 0xff);
	ca0113_play_cmd(chip, 0x49, 0x19, 0xff);
	ca0113_play_cmd(chip, 0x49, 0x1a, 0);
	ca0113_play_cmd(chip, 0x49, 0x1b, 0x70);
	ca0113_play_cmd(chip, 0x49, 0, 0xff);
	ca0113_play_cmd(chip, 0x49, 1, 0xff);
	ca0113_play_cmd(chip, 0x49, 6, 0xff);
	ca0113_play_cmd(chip, 0x49, 7, 0xff);
	ca0113_play_cmd(chip, 0x49, 0x10, 0x7f);
	ca0113_play_cmd(chip, 0x49, 10, 7);


	/*
	 * Decompiled sequence continues:
	 *   SPI re-init: 0xC0C=0x83, 0xC00=0x0d, 0xC04=0, 0xC0C=0x03
	 *   ae9_set_sample_rate_low() ×1    (SYS_INFO SPI frame)
	 *   BAR2+0x320=0x104 + ca0113_mmio_command_set_pulse2(7, 0x104, 7)
	 *   ae9_set_sample_rate_low() ×1
	 *   ca0113_mmio_command_set(0x48, 7, 0x80)
	 *   ca0113_mmio_command_set_pulse(0x49, 10, 6)
	 *   6 final pulses: 0x48/0xf,0x10  0x49/4,5,2,3
	 *   ae9_set_sample_rate_low() ×1
	 *   msleep(20)
	 *   ae9_set_sample_rate_low() ×1
	 *
	 * The SPI re-init (0xC0C=0x83, 0xC00=0x0d) is skipped — writing 0x0d
	 * to 0xC00 routes the controller to init mode and disrupts our running
	 * SPI audio loop (causes fatal halt=0xf). The 0x320=0x105 at the start
	 * of ae9_set_sample_rate_low() restores the correct SPI mode before
	 * sending the SYS_INFO frame, so the frames themselves are safe to send.
	 */

	/* 1st ae9_set_sample_rate_low */
	ae9_set_sample_rate_low(chip);

	/* BAR2+0x320=0x104 + pulse2 */
	creative_ext_writew(chip, 0x320, 0x104);
	ca0113_mmio_command_set_type2(chip, 7, 0x104, 7);

	/* 2nd ae9_set_sample_rate_low */
	ae9_set_sample_rate_low(chip);

	/* ca0113 finalization */
	ca0113_mmio_command_set(chip, 0x48, 7, 0x80);
	ca0113_play_cmd(chip, 0x49, 10, 6);

	/* 6 final pulses (also repeated in ae9_setup_defaults after speaker_tuning) */
	ca0113_play_cmd(chip, 0x48, 0x0f, 0);
	ca0113_play_cmd(chip, 0x48, 0x10, 0);
	ca0113_play_cmd(chip, 0x49, 4, 0);
	ca0113_play_cmd(chip, 0x49, 5, 0);
	ca0113_play_cmd(chip, 0x49, 2, 0);
	ca0113_play_cmd(chip, 0x49, 3, 0);

	/* 3rd ae9_set_sample_rate_low */
	ae9_set_sample_rate_low(chip);
	msleep(20);
	/* 4th ae9_set_sample_rate_low */
	ae9_set_sample_rate_low(chip);

	dev_info(chip->dev, "ae9_post_dsp_asi_setup: done\n");
}

/*
 * Route DSP audio to the rear line-out (SPEAKER_OUT, 2.0, no effects).
 * Mirrors mainline ca0132_alt_select_out for the SPEAKER_OUT path.
 */
int creative_dsp_select_lineout(struct creative_chip *chip)
{
	u32 resp;
	int err;

	if (!lineout_prerun) {
		dev_info(chip->dev,
			 "select_lineout: skipping transcribed pre-run/download steps (lineout_prerun=0)\n");
		goto post_prerun;
	}

	/*
	 * AE-9 boot init sequence from cold-boot VM CORB capture where audio
	 * was CONFIRMED WORKING on the head-unit headphone jack. This sequence
	 * arms the analog output stage (the relay "click") and must run before
	 * any stream binding. Byte-for-byte from the capture:
	 *
	 * Phase 1 (L723/743): DSP_PARAM(0x0b0)=0xa4 x2 with STREAM_ID=0
	 *   The 0xa4 value arms the DSP's internal amp/routing — this is
	 *   what triggers the relay click and enables analog output.
	 *
	 * Phase 2 (L1023-1041): DSP_PARAM(0x0b0)=0x08 x3 with STREAM_ID=0
	 *   Processing trigger — fires 3 times before dsp_reset.
	 *
	 * Phase 3 (L1051): DSPIO_DSP_INIT — flush/reset SCP queue.
	 *
	 * Phase 4 (L1067/1077): DSP_PARAM(0x0b0)=0x30 x2 with STREAM_ID=0
	 *   Post-reset config.
	 *
	 * Phase 5 (L1099-1117): Conn-points 0x0a and 0x0b at SR_48_000.
	 *
	 * Phase 6 (L1641-1643): nid=0x15 FMT=0x47 + BIND tag=1
	 *   Node 0x15 (DSP widget, FIFO=0x4500) gets format+stream bound.
	 */

	/* Phase 1: DSP_PARAM(0x0b0)=0xa4 x2 — amp/relay arm */
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x0b0, 0xa4);
	chipio_set_control_param(chip, CONTROL_PARAM_STREAM_ID, 0);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x0b0, 0xa4);
	chipio_set_control_param(chip, CONTROL_PARAM_STREAM_ID, 0);

	/* Phase 2: cad=2 head-unit node unbind (clears state) */
	verb_send(chip,
		creative_ae9_verb(2, 0x05, 0x706, 0x00), &resp);
	verb_send(chip,
		creative_ae9_verb(2, 0x09, 0x706, 0x00), &resp);
	verb_send(chip,
		creative_ae9_verb(2, 0x08, 0x706, 0x00), &resp);
	verb_send(chip,
		creative_ae9_verb(2, 0x06, 0x706, 0x00), &resp);
	verb_send(chip,
		creative_ae9_verb(2, 0x07, 0x706, 0x00), &resp);

	/*
	 * EXRAM patches — applied via ChipIO 8051 ADDR_LO/HI + DATA_WRITE.
	 * From coldboot CORB capture lines 930-1008 (before first BIND).
	 * These patch 8051 code/data RAM to enable the audio processing loop.
	 * All return resp=0x00000000 (normal ACK) — no running 8051 required.
	 *
	 * Previous note saying "requires running 8051" was WRONG — these fire
	 * via ChipIO which works regardless of 8051 execution state.
	 */
	{
		/* Helper: write sequential bytes to 8051 EXRAM starting at addr */
		static const u8 patch_fef0[] = {
			0x75, 0xd3, 0x09, 0x53, 0xd4, 0xef,
			0x75, 0xd3, 0x09, 0x02, 0x37, 0x78
		};
		static const u8 patch_fee8[] = { 0x02, 0x68, 0x62 };
		unsigned int i;

		/* Write 12 bytes at 0xfef0 */
		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_8051_ADDRESS_LOW,  0xf0);
		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_8051_ADDRESS_HIGH, 0xfe);
		for (i = 0; i < ARRAY_SIZE(patch_fef0); i++)
			dsp_write(chip, WIDGET_CHIP_CTRL,
				  VENDOR_CHIPIO_8051_DATA_WRITE, patch_fef0[i]);

		/* Write 3 bytes at 0xfee8 */
		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_8051_ADDRESS_LOW,  0xe8);
		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_8051_ADDRESS_HIGH, 0xfe);
		for (i = 0; i < ARRAY_SIZE(patch_fee8); i++)
			dsp_write(chip, WIDGET_CHIP_CTRL,
				  VENDOR_CHIPIO_8051_DATA_WRITE, patch_fee8[i]);

		/* Single-byte patches */
		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_8051_ADDRESS_LOW,  0x20);
		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_8051_ADDRESS_HIGH, 0x19);
		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_8051_DATA_WRITE, 0x00);

		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_8051_ADDRESS_LOW,  0xb7);
		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_8051_ADDRESS_HIGH, 0x09);
		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_8051_DATA_WRITE, 0x10);

		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_8051_ADDRESS_LOW,  0xaf);
		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_8051_ADDRESS_HIGH, 0x09);
		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_8051_DATA_WRITE, 0x01);
		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_8051_DATA_WRITE, 0x05);

		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_8051_ADDRESS_LOW,  0x73);
		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_8051_ADDRESS_HIGH, 0x09);
		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_8051_DATA_WRITE, 0x14);

		dev_info(chip->dev, "EXRAM patches applied\n");
	}

	/* Phase 3: DSP_PARAM(0x0b0)=0x08 x3 — processing trigger */
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x0b0, 0x08);
	chipio_set_control_param(chip, CONTROL_PARAM_STREAM_ID, 0);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x0b0, 0x08);
	chipio_set_control_param(chip, CONTROL_PARAM_STREAM_ID, 0);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x0b0, 0x08);
	chipio_set_control_param(chip, CONTROL_PARAM_STREAM_ID, 0);

	/*
	 * Pre-SCP activation sequence from CORB capture (CORB idx 131-134).
	 * These 4 verbs must fire BEFORE the FLAG2 trigger and before
	 * dsp_reset(), as they prime the SCP queue to accept commands.
	 * Without them, DSPIO_STATUS returns 0x02 (CMD_QUEUE_FULL) for
	 * every set_uint_param call.
	 *
	 * Decoded from vm_capture_qemureset_20260604_011038/ae9_corb.log:
	 *   [131] nid=0x15 v=0x70f p=0x02 = CHIPIO_DATA_SET(0x02)
	 *         at ChipIO addr 0x44 = CONTROL_PARAM_STREAMS_CHANNELS
	 *         → chipio_set_control_param(STREAMS_CHANNELS, 2)
	 *   [132] nid=0x15 v=0x0b0 p=0xa4 = DSP_PARAM(0x0b0) = 0xa4
	 *         → DAC2PORT = 0xa4 (AE-9 specific)
	 *   [133] nid=0x15 v=0x100 p=0x18 = SCP_DATA_HIGH = 0x18
	 *         → dspio_send(SCP_WRITE_DATA_HIGH, 0x18)
	 *   [134] nid=0x15 v=0x702 p=0x00 = SCP_POST_READ
	 *         → dspio_send(SCP_POST_READ_DATA, 0)
	 * After [134], DSPIO_STATUS returns 0x00 (OK) at [135].
	 */
	chipio_set_control_param(chip, CONTROL_PARAM_STREAMS_CHANNELS, 2);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x0b0, 0xa4);
	dspio_send(chip, VENDOR_DSPIO_SCP_WRITE_DATA_HIGH, 0x18);
	dspio_send(chip, VENDOR_DSPIO_SCP_POST_READ_DATA, 0);

	/* Phase 4: DSPIO_DSP_INIT — flush SCP queue */
	dsp_reset(chip);

	/*
	 * Phase 4b: CHIPIO_PARAM_SET (0x710) x5 + CHIPIO_FLAG_SET (0x70f) x2
	 * — ChipIO routing/mode configuration written immediately after
	 * dsp_reset(), before the 0x0b0=0x30 post-reset writes.
	 *
	 * Source: cold-boot CORB log L1054-L1066, cad=1 nid=0x15.
	 *   Identical in the QEMU-reset capture (seq 1050-1062).
	 *   Both captures: immediately after nid=0x16 v=0x703 (dsp_reset).
	 *
	 * These are ChipIO internal parameter and flag register writes —
	 * NOT an audio-loop trigger. They configure the ChipIO codec
	 * routing state after the DSP queue flush.
	 *
	 * v=0x710 = VENDOR_CHIPIO_PARAM_SET  (sets a control parameter)
	 * v=0x70f = VENDOR_CHIPIO_FLAG_SET   (sets a flag register)
	 */
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x710, 0x02);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x710, 0x00);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x710, 0x01);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x710, 0xc8);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x710, 0xca);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x70f, 0x94);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x70f, 0x95);

	/* Phase 5: DSP_PARAM(0x0b0)=0x30 x2 — post-reset config */
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x0b0, 0x30);
	chipio_set_control_param(chip, CONTROL_PARAM_STREAM_ID, 0);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x0b0, 0x30);
	chipio_set_control_param(chip, CONTROL_PARAM_STREAM_ID, 0);

	/* Phase 6: Conn-points 0x0a and 0x0b at SR_48_000 (x2 per capture) */
	chipio_set_conn_rate(chip, 0x0a, SR_48_000);
	chipio_set_conn_rate(chip, 0x0b, SR_48_000);
	chipio_set_conn_rate(chip, 0x0a, SR_48_000);
	chipio_set_conn_rate(chip, 0x0b, SR_48_000);

	/*
	 * 9 SCP output config params — moved here (BEFORE nid=0x15 binds).
	 *
	 * Root cause of 0x32/0x00 (source select = FLOAT_TWO) always failing:
	 * These params were previously sent AFTER the nid=0x15 binds (Phase 7
	 * and Phase 7d). Each bind triggers an 8051 SCP response cascade that
	 * keeps the command queue full for seconds. No amount of draining
	 * cleared it fast enough before 0x32/0x00 fired.
	 *
	 * The decompiled ae9_setup_defaults sends these params BEFORE any
	 * nid=0x15 bind. At this point in select_lineout the SCP queue is
	 * clean (only Phase 5/6 ChipIO writes preceded, no binds). The 9 params
	 * land cleanly in sequence.
	 *
	 * From decompiled ae9_setup_defaults (lines 26037-26046):
	 *   0x96/0x29 = 0, 0x96/0x2a = 0, 0x80/0x0d = 0, 0x80/0x0e = 0,
	 *   0x37/0x08 = 1.0f, 0x37/0x10 = 1.0f, 0x96/0x3c = 0,
	 *   0x31/0x00 = 2.0f, chipio(10,SR_48), 0x32/0x00 = 2.0f
	 */
	dspio_set_uint_param(chip, 0x96, 0x29, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x96, 0x2a, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x80, 0x0d, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x80, 0x0e, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x37, 0x08, FLOAT_ONE);
	dspio_set_uint_param(chip, 0x37, 0x10, FLOAT_ONE);
	dspio_set_uint_param(chip, 0x96, 0x3c, FLOAT_ZERO);
	dspio_write_wait(chip);
	dspio_clear_response_queue(chip);
	dspio_set_uint_param(chip, 0x31, 0x00, 0x40000000u /* FLOAT_TWO */);
	chipio_set_conn_rate(chip, 10, SR_48_000);
	dspio_write_wait(chip);
	dspio_clear_response_queue(chip);
	dspio_set_uint_param(chip, 0x32, 0x00, 0x40000000u /* FLOAT_TWO */);
	{
		unsigned int sts = dsp_read(chip, WIDGET_DSP_CTRL,
					    VENDOR_DSPIO_STATUS, 0);
		dev_info(chip->dev, "select_lineout: 9-param block done, SCP=0x%x\n", sts);
	}

	/* Phase 7: nid=0x15 (DSP widget) SET_FMT=0x47, BIND tag=1
	 * From cold-boot capture L1641/L1643 — first bind, starts DSP engine.
	 * FMT=0x47 = 8ch/16bit/48kHz.
	 * BIND via CORB (ICW does not work for vendor widget 0x706),
	 * with ICW fallback if CORB is not yet functional.
	 */
	/* Phase 7: SET_FORMAT=0x0047 (6ch/16bit/48kHz) + BIND tag=1.
	 * Capture CORB idx=159/160: format 0x0047, BIND stream=1 chan=0.
	 */
	verb_send(chip,
		creative_ae9_verb(CAD, 0x15, 0x200, 0x0047), &resp);
	{
		u32 bind_resp = 0;
		int bind_ret = creative_ae9_corb_send_verb_sync(chip,
			creative_ae9_verb(CAD, 0x15, 0x706, 0x10), &bind_resp);
		if (bind_ret < 0) {
			dev_warn(chip->dev,
				"select_out ph7: CORB BIND failed (%d)\n",
				bind_ret);
			verb_send(chip,
				creative_ae9_verb(CAD, 0x15, 0x706, 0x10),
				&bind_resp);
		}
		dev_info(chip->dev,
			"select_out ph7: BIND ret=%d resp=0x%08x\n",
			bind_ret, bind_resp);
	}

	/* Phase 7a: Post-BIND verb sequence — exactly matching CORB capture
	 * idx=161-177 (ae9_corb.log lines 1865-1898).
	 *
	 * idx=161  v=0xf01/0x00  — GET_CONVERTER_STREAM drain
	 * idx=162  v=0x711/0x07  — CHIPIO_ADDRESS_HIGH=0x07
	 * idx=163  v=0x712/0x10  — CHIPIO_DATA_LOW=0x10
	 * idx=164  v=0xf01/0x00  — drain
	 * idx=165  v=0xf12/0x00  — GET_PROC_COEFF
	 * idx=166  v=0xf01/0x00  — drain
	 * idx=167  v=0x713/0x10  — CHIPIO_DATA_HIGH=0x10
	 * idx=168  v=0xf01/0x00  — drain
	 * idx=169  nid=0x16 v=0x703/0x00 — DSP_INIT flush on nid=0x16
	 * idx=170  v=0x70d/0x00  — CHIPIO_STREAM_FORMAT_SET
	 * idx=171  v=0x70c/0xff  — CHIPIO_STREAM_FORMAT
	 * idx=172  v=0x70f/0x89  — CHIPIO_DATA_SET 0x89
	 * idx=173  v=0x70f/0x88  — CHIPIO_DATA_SET 0x88
	 * idx=174  v=0x70f/0x86  — CHIPIO_DATA_SET 0x86
	 * idx=175  v=0x70f/0x87  — CHIPIO_DATA_SET 0x87
	 * idx=176  v=0x70f/0x8b  — CHIPIO_DATA_SET 0x8b
	 * idx=177  v=0xf01/0x00  — drain
	 *
	 * NOTE: Windows does NOT issue v=0xf06 (GET_CONVERTER_STREAM) on
	 * nid=0x15. The previous readback was meaningless on a vendor widget
	 * and has been removed.
	 */
	dsp_read(chip,  WIDGET_CHIP_CTRL, 0xf01, 0x00); /* drain after BIND */
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x711, 0x07); /* CHIPIO_ADDRESS_HIGH=7 */
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x712, 0x10); /* CHIPIO_DATA_LOW=0x10 */
	dsp_read(chip,  WIDGET_CHIP_CTRL, 0xf01, 0x00); /* drain */
	dsp_read(chip,  WIDGET_CHIP_CTRL, 0xf12, 0x00); /* GET_PROC_COEFF */
	dsp_read(chip,  WIDGET_CHIP_CTRL, 0xf01, 0x00); /* drain */
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x713, 0x10); /* CHIPIO_DATA_HIGH=0x10 */
	dsp_read(chip,  WIDGET_CHIP_CTRL, 0xf01, 0x00); /* drain */
	/* nid=0x16 DSP_INIT flush — as seen in capture idx=169 */
	verb_send(chip, creative_ae9_verb(CAD, 0x16, 0x703, 0x00), &resp);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x70d, 0x00); /* CHIPIO_STREAM_FORMAT_SET */
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x70c, 0xff); /* CHIPIO_STREAM_FORMAT */
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x70f, 0x89); /* CHIPIO_DATA_SET */
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x70f, 0x88);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x70f, 0x86);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x70f, 0x87);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x70f, 0x8b);
	dsp_read(chip,  WIDGET_CHIP_CTRL, 0xf01, 0x00); /* drain */

	/* Phase 7a (continued): remaining pipeline writes from capture.
	 * These follow idx=177 and continue to idx=221 (end of first BIND
	 * setup block). Pipeline/coefficient writes that set up the DSP
	 * audio processing chain for the output path.
	 */
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x00f, 0xf0);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x100, 0x11);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x300, 0x00);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x400, 0x01);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x00f, 0xf8);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x100, 0x11);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x300, 0x00);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x4f0, 0x01);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x300, 0x00);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x400, 0x00);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x00f, 0xc0);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x100, 0x11);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x300, 0xff);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x400, 0x00);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x00f, 0x0c);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x100, 0x11);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x3ff, 0xff);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x4ff, 0xff);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x00f, 0x00);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x100, 0x11);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x300, 0x08);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x400, 0x00);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x300, 0x00);

	/* Phase 7b: 2.3s wait — DSP engine starts during this window */
	msleep(2300);
	dev_info(chip->dev, "select_out: DSP engine wait complete\n");

	/* Phase 7c: Unbind nid=0x15 (capture L3875) */
	verb_send(chip,
		creative_ae9_verb(CAD, 0x15, 0x706, 0x00), &resp);

	/* Phase 7c+: pre-second-bind block.
	 * v=0x713 = VENDOR_CHIPIO_PORT_FREE_SET p=0x10
	 * port-alloc config: 0x33c=0x00, 0x4ff=0xf8, then 0x33c=0x0f, 0x4ff=0xf8
	 *
	 * NOTE: The cold-boot CORB capture also shows dsp_read(0xf0f) (vendor GET)
	 * and 3x chipio_read(0x0e30) (DBGCNTL) here. These are read verbs that
	 * require RIRB responses. In our driver the 8051 is in the SPI audio loop
	 * at this point and stops servicing CORB reads, causing timeout → CORB jam.
	 * Removed — they were diagnostic/polling reads with no functional effect.
	 */
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x713, 0x10); /* PORT_FREE_SET */
	/* port-alloc config: 0x33c=0x00, 0x4ff=0xf8, then 0x33c=0x0f, 0x4ff=0xf8 */
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x33c, 0x00);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x4ff, 0xf8);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x33c, 0x0f);
	dsp_write(chip, WIDGET_CHIP_CTRL, 0x4ff, 0xf8);

	/*
	 * SCP trigger (mod=0x18/0x19/0x15) moved to creative_dsp_pcm_start().
	 *
	 * Root cause analysis (2026-06-04): the cold-boot CORB capture shows
	 * Windows fires these SCP triggers at CORB lines ~3896-3970 — during
	 * ACTIVE PCM playback, after nid=0x02 (DAC) is bound to the HDA stream.
	 * Firing them here at standup time (before any PCM stream exists) means
	 * the 8051 acknowledges them (0x02c00001) but the DSP DMA engine can't
	 * start because there is no audio stream to DMA from. DSPDMAC_ACTIVE
	 * stays 0 and 0x801c never increments.
	 *
	 * The triggers now fire in pcm_start after stream setup and BINDs.
	 */
	/* Flush SCP queue before the second bind */
	dsp_reset(chip);
	dspio_clear_response_queue(chip);

	/* Phase 7d: Second BIND — FMT=0x41 + BIND tag=1.
	 *
	 * Cold-boot CORB capture (line 3960-3963):
	 *   idx=187  nid=0x15 0xf01/0x00  drain before BIND
	 *   idx=188  nid=0x15 0x200/0x41  SET_FMT (0x41, not 0x47 as in 1st BIND)
	 *   idx=189  nid=0x15 0x706/0x10  BIND tag=1
	 *
	 * Post-second-BIND (capture idx=190-192, lines 3964-3969):
	 *   idx=190  nid=0x16 0x000/0x80  SET_POWER_STATE D0
	 *   idx=191  nid=0x16 0x100/0x15  SET_CHANNEL_COUNT (or amplifier)
	 *   idx=192  nid=0x16 0xf01/0x00  drain
	 *
	 * NOTE: The 0x70d/0x70c/0x70f×5 (stream-format + control flags
	 * 0x89/0x88/0x86/0x87/0x8b) were previously placed here by mistake.
	 * They belong BEFORE the first BIND (capture lines 1594-1634, idx=29-50)
	 * and were being re-sent here with the 8051 already running, causing
	 * the fatal halt=0xf. They are correctly present only in Phase 7a.
	 *
	 * NOTE: The 0x711/0x712/0x713 + 0xf12 address/proc-coeff sequence
	 * (previously here as "idx=192-208") was also misattributed — those
	 * verbs appear in Phase 7a context, not after the second BIND.
	 */
	/* drain before 2nd BIND — nowait: 8051 in audio loop, won't RIRB */
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL, 0xf01, 0x00);
	{
		u32 fmt_resp = 0, bind_resp = 0;
		int fmt_ret, bind_ret;
		fmt_ret  = verb_send(chip,
			creative_ae9_verb(CAD, 0x15, 0x200, 0x0041), &fmt_resp);
		bind_ret = creative_ae9_corb_send_verb_sync(chip,
			creative_ae9_verb(CAD, 0x15, 0x706, 0x10), &bind_resp);
		if (bind_ret < 0) {
			dev_warn(chip->dev,
				"select_out ph7d: CORB BIND failed (%d), "
				"falling back to ICW\n", bind_ret);
			bind_ret = verb_send(chip,
				creative_ae9_verb(CAD, 0x15, 0x706, 0x10),
				&bind_resp);
		}
		dev_info(chip->dev,
			 "select_out: bind fmt_ret=%d(%08x) bind_ret=%d(%08x)\n",
			 fmt_ret, fmt_resp, bind_ret, bind_resp);
	}
	/* Post-second-BIND: nid=0x16 power + channel */
	verb_send(chip, creative_ae9_verb(CAD, 0x16, 0x000, 0x80), &resp); /* SET_POWER D0 */
	verb_send(chip, creative_ae9_verb(CAD, 0x16, 0x100, 0x15), &resp); /* SET_CHAN_COUNT */
	/* drain nowait — 8051 in audio loop, won't respond to ChipIO status reads */
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL, 0xf01, 0x00);

	dev_info(chip->dev, "select_out: amp init sequence complete\n");
post_prerun:

	/* Drain response queue only — do NOT dsp_reset() here as that
	 * would clear the nid=0x15 stream binding we just set above. */
	dspio_clear_response_queue(chip);
	{
		unsigned int sts = dsp_read(chip, WIDGET_DSP_CTRL,
					    VENDOR_DSPIO_STATUS, 0);
		dev_info(chip->dev,
			 "select_out: pre-SCP SCP=0x%x\n", sts);
	}

	/* 9 SCP params moved before Phase 7 — see insertion point above */
	err = 0;

	/*
	 * ae9_post_dsp_asi_setup — AE-9 specific physical audio path setup.
	 * Called here after the dspio params, matching the decompiled
	 * ae9_setup_defaults sequence (line 26047):
	 *   dspio params → ae9_post_dsp_asi_setup
	 *
	 * The decompiled driver's ae9_post_dsp_asi_setup ends with a 20ms
	 * sleep + 4× ae9_set_sample_rate_low (SPI writes) inside the mutex —
	 * ~80-100ms of natural gap before the next SCP writes. Our version
	 * omits the SPI writes, so we add an explicit msleep(100) to give the
	 * 8051 the same amount of time to drain any pending SCP commands.
	 */
	ae9_post_dsp_asi_setup(chip);
	msleep(100); /* replicate the natural gap from the omitted SPI tail */

	/*
	 * ca0132_effects default parameter initialisation.
	 *
	 * The decompiled ae9_setup_defaults iterates ca0132_effects[0..10]
	 * (11 entries) and sends every effect's params via dspio_set_uint_param.
	 * Values extracted from binary .rodata at symbol ca0132_effects (0x3060):
	 *
	 *  [0] mid=0x96: req=0x00 val=0x3f800000 (1.0f), req=0x01 val=0x3f2b851f
	 *  [1] mid=0x96: req=0x07 val=0x3f800000 (1.0f), req=0x08 val=0x3f266666
	 *  [2] mid=0x96: req=0x02 val=0x00000000 (0.0f), req=0x03 val=0x3f000000 (0.5f)
	 *  [3] mid=0x96: req=0x04 val=0x3f800000, req=0x05 val=0x3f3d70a4, req=0x06 val=0x0
	 *  [4] mid=0x96: req=0x18 val=0x3f800000, req=0x17 val=0x42a00000, req=0x19 val=0x3f000000
	 *  [5] mid=0x96: req=0x09..0x14 all 0x0  (12 params)
	 *  [6] mid=0x95: req=0x00 val=0x0, req=0x01 val=0x3f3a9692, req=0x02 val=0x0, req=0x03 val=0x0
	 *  [7] mid=0x95: req=0x06 val=0x3f800000, req=0x07 val=0x3d7df3b6,
	 *                req=0x08 val=0x41f00000, req=0x09 val=0x41f00000
	 *  [8] mid=0x95: req=0x2c val=0x0, req=0x2d val=0x3f3d70a4
	 *  [9] mid=0x95: req=0x04 val=0x3f800000, req=0x05 val=0x3f000000
	 * [10] mid=0x95: req=0x0a val=0x0, req=0x0b val=0x43c80000,
	 *                req=0x0c val=0x44af0000, req=0x0d val=0x44fa0000,
	 *                req=0x0e val=0x3f800000, req=0x0f val=0x3f800000,
	 *                req=0x10 val=0x3f800000, req=0x11 val=0x0, req=0x12 val=0x0
	 *
	 * Batched into groups separated by a drain to avoid SCP queue overflow.
	 */
	/* effects[0..3]: mid=0x96 gain/level defaults */
	dspio_set_uint_param(chip, 0x96, 0x00, 0x3f800000u); /* 1.0f */
	dspio_set_uint_param(chip, 0x96, 0x01, 0x3f2b851fu);
	dspio_set_uint_param(chip, 0x96, 0x07, 0x3f800000u);
	dspio_set_uint_param(chip, 0x96, 0x08, 0x3f266666u);
	dspio_set_uint_param(chip, 0x96, 0x02, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x96, 0x03, 0x3f000000u); /* 0.5f */
	dspio_set_uint_param(chip, 0x96, 0x04, 0x3f800000u);
	dspio_set_uint_param(chip, 0x96, 0x05, 0x3f3d70a4u);
	dspio_set_uint_param(chip, 0x96, 0x06, FLOAT_ZERO);
	dspio_write_wait(chip);
	dspio_clear_response_queue(chip);
	/* effects[4]: mid=0x96 surround */
	dspio_set_uint_param(chip, 0x96, 0x18, 0x3f800000u);
	dspio_set_uint_param(chip, 0x96, 0x17, 0x42a00000u); /* 80.0f */
	dspio_set_uint_param(chip, 0x96, 0x19, 0x3f000000u);
	/* effects[5]: mid=0x96 eq band 0x09-0x14 all zero */
	dspio_set_uint_param(chip, 0x96, 0x09, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x96, 0x0a, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x96, 0x0b, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x96, 0x0c, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x96, 0x0d, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x96, 0x0e, FLOAT_ZERO);
	dspio_write_wait(chip);
	dspio_clear_response_queue(chip);
	dspio_set_uint_param(chip, 0x96, 0x0f, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x96, 0x10, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x96, 0x11, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x96, 0x12, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x96, 0x13, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x96, 0x14, FLOAT_ZERO);
	dspio_write_wait(chip);
	dspio_clear_response_queue(chip);
	/* effects[6..7]: mid=0x95 reverb/crystalizer */
	dspio_set_uint_param(chip, 0x95, 0x00, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x95, 0x01, 0x3f3a9692u);
	dspio_set_uint_param(chip, 0x95, 0x02, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x95, 0x03, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x95, 0x06, 0x3f800000u);
	dspio_set_uint_param(chip, 0x95, 0x07, 0x3d7df3b6u);
	dspio_set_uint_param(chip, 0x95, 0x08, 0x41f00000u); /* 30.0f */
	dspio_set_uint_param(chip, 0x95, 0x09, 0x41f00000u);
	dspio_write_wait(chip);
	dspio_clear_response_queue(chip);
	/* effects[8..10]: mid=0x95 misc */
	dspio_set_uint_param(chip, 0x95, 0x2c, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x95, 0x2d, 0x3f3d70a4u);
	dspio_set_uint_param(chip, 0x95, 0x04, 0x3f800000u);
	dspio_set_uint_param(chip, 0x95, 0x05, 0x3f000000u);
	dspio_set_uint_param(chip, 0x95, 0x0a, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x95, 0x0b, 0x43c80000u); /* 400.0f */
	dspio_set_uint_param(chip, 0x95, 0x0c, 0x44af0000u); /* 1400.0f */
	dspio_set_uint_param(chip, 0x95, 0x0d, 0x44fa0000u); /* 2000.0f */
	dspio_set_uint_param(chip, 0x95, 0x0e, 0x3f800000u);
	dspio_write_wait(chip);
	dspio_clear_response_queue(chip);
	dspio_set_uint_param(chip, 0x95, 0x0f, 0x3f800000u);
	dspio_set_uint_param(chip, 0x95, 0x10, 0x3f800000u);
	dspio_set_uint_param(chip, 0x95, 0x11, FLOAT_ZERO);
	dspio_set_uint_param(chip, 0x95, 0x12, FLOAT_ZERO);
	dspio_write_wait(chip);
	dspio_clear_response_queue(chip);
	dev_info(chip->dev, "select_out: ca0132_effects defaults sent\n");

	dev_info(chip->dev, "select_out: lineout configured (err %d)\n", err);
	return 0;
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

	ret = request_firmware(&fw, DESKTOP_EFX_FILE, chip->dev);
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
	if (!dl_windows) {
		creative_dsp_chipio_write(chip, 0x40004, 0);
		creative_dsp_chipio_write(chip, 0x801c,  0);
	}
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

		if (request_firmware(&fw2, DESKTOP_EFX_FILE, chip->dev) == 0) {
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
	unsigned int val = 0;
	int err = -EIO;
	u32 resp;
	int attempt;

	/*
	 * Force-reset the firmware DMA stream (at 0x120) before use.
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

	if (prep_windows) {
		dev_info(chip->dev,
			 "DSP prepare: codec verbs skipped (prep_windows=1, replay already sent them)\n");
		return 0;
	}

	/*
	 * Bad-boot auto-recovery: on some cold boots the codec/ChipIO channel
	 * comes up wedged (codec IDs read garbage, chipio_read returns -EIO).
	 * Retry the CT-extensions-enable + clock-enable a few times until
	 * ChipIO responds, so a flaky boot self-heals instead of needing a
	 * manual power-cycle.
	 */
	for (attempt = 0; attempt < 5; attempt++) {
		curr_chip_addx = ~0U;

		/* Enable CT extensions on the chip control widget */
		dsp_write(chip, WIDGET_CHIP_CTRL,
			  VENDOR_CHIPIO_CT_EXTENSIONS_ENABLE, 1);
		creative_dsp_enable_clocks(chip);

		err = creative_dsp_chipio_read(chip, 0x801c, &val);
		if (err == 0)
			break;

		dev_warn(chip->dev,
			 "DSP prepare: ChipIO wedged (attempt %d, ret=%d), retrying\n",
			 attempt + 1, err);
		msleep(50);
	}

	if (err < 0) {
		dev_err(chip->dev,
			"DSP prepare: ChipIO not responding after retries\n");
		return err;
	}

	/*
	 * Set 96KHz clock flags for AE-9 before firmware download.
	 *
	 * Decompiled CA0132 v0.73_kernel18 ca0132_codec_init (line 24145):
	 * For AE-series cards (is_ae_series bit 0 = 0, which includes AE-9),
	 * DSP_96KHZ and DAC_96KHZ are set to TRUE before firmware download.
	 * This is NOT in ae9_setup_defaults — it runs in codec_init, matching
	 * the timing here (before firmware). Our standup previously set these
	 * to FALSE, which was wrong both in value and timing.
	 */
	chipio_set_control_flag(chip, CONTROL_FLAG_DSP_96KHZ, 1);
	chipio_set_control_flag(chip, CONTROL_FLAG_DAC_96KHZ, 1);

	/*
	 * Disable 8051 IDLE mode before firmware download.
	 *
	 * The decompiled driver does not explicitly set IDLE_ENABLE, but our
	 * SPI HELLO path puts the 8051 in a different operational mode than
	 * the pure Linux codec path. Without IDLE_ENABLE=0, the firmware's
	 * IDLE logic halts the 8051 (halt=1) between audio processing steps,
	 * preventing continuous audio frame processing. Setting it here (before
	 * firmware download, matching the timing of other pre-firmware flag
	 * setup) keeps the 8051 running during playback.
	 */
	chipio_set_control_flag(chip, CONTROL_FLAG_IDLE_ENABLE, 0);

	/*
	 * Windows-style chip reset BEFORE firmware download.
	 */
	creative_dsp_chipio_write(chip, 0x18b0a4, 0x000000c2);
	/* Reset codec AFG (node 0x01) twice, as Windows does. */
	verb_send(chip,
		creative_ae9_verb(CAD, 0x01, 0x7ff, 0x00), &resp);
	verb_send(chip,
		creative_ae9_verb(CAD, 0x01, 0x7ff, 0x00), &resp);

	/*
	 * Force-clear DBGCNTL (0x100e30) to 0 after the AFG double-reset.
	 *
	 * Problem: DBGCNTL is a ChipIO register — it is NOT reset by GCTL
	 * toggle. After bare-metal Windows shutdown, DBGCNTL=0xfff93c60
	 * (halt_state=0xf = fatal exception) survives through our self-reset
	 * sequence. When firmware DMA finishes, dsp_set_run_state() inside
	 * dspload_image cannot clear halt=0xf (STATE bits [13:10] are set by
	 * the 8051 hardware exception mechanism and can only be cleared by
	 * putting the DSP core into a clean reset state).
	 *
	 * The two AFG resets (verb 0x7ff × 2) above put the DSP core into
	 * hardware reset. At this point the STATE bits in DBGCNTL become
	 * clearable. Writing 0 unconditionally clears all halt/exception bits,
	 * giving the 8051 a clean slate to boot from the incoming firmware.
	 *
	 * Confirmed from decompiled Creative CA0132 v0.73_kernel18 (DWARF5):
	 * the decompiled driver never encounters halt=0xf before firmware load
	 * (it always loads from cold), so it never needs this explicit clear.
	 * Our self-reset path does encounter it, hence the explicit write here.
	 */
	{
		unsigned int dbg_before = 0, dbg_after = 0;
		creative_dsp_chipio_read(chip, DSP_DBGCNTL_INST_OFFSET,
					 &dbg_before);
		creative_dsp_chipio_write(chip, DSP_DBGCNTL_INST_OFFSET, 0);
		creative_dsp_chipio_read(chip, DSP_DBGCNTL_INST_OFFSET,
					 &dbg_after);
		dev_info(chip->dev,
			 "DSP prepare: DBGCNTL 0x%x→0x%x (halt %u→%u) after force-clear\n",
			 dbg_before, dbg_after,
			 (dbg_before & DSP_DBGCNTL_STATE_MASK) >>
				DSP_DBGCNTL_STATE_LOBIT,
			 (dbg_after & DSP_DBGCNTL_STATE_MASK) >>
				DSP_DBGCNTL_STATE_LOBIT);
	}

	/* sanity: read a DSP debug register via ChipIO */
	err = creative_dsp_chipio_read(chip, 0x801c, &val);
	dev_info(chip->dev, "DSP prepare: chipio_read(0x801c)=0x%x ret=%d\n",
		 val, err);
	return err;
}

/*
 * Start PCM playback with the exact sequence captured from a cold VM boot
 * where audio was confirmed working on the head-unit headphone jack.
 *
 * CORB capture analysis (cold-boot, confirmed working audio):
 *   L5261-5287: Normal 6ch/24bit audio sequence:
 *     1. DSP_PARAM(0x0b0)=0x08 x2, ChipIO STREAM_ID=0 between each
 *     2. nid=0x02 SET_STREAM_FORMAT 0x35 (6ch/24bit/48kHz)
 *     3. ChipIO DAC2PORT(0x0d)=0xa4 (extended param, just before bind)
 *     4. nid=0x02 SET_CHANNEL_STREAMID tag=stream_tag
 *
 * Note: cad=2 (head-unit codec) bindings only appear in direct/headphone
 * mode, NOT in normal 6ch mode. The head-unit DAC receives audio via
 * the DSP's ASI serial port, not via an HDA stream binding.
 */
void creative_dsp_pcm_start(struct creative_chip *chip, unsigned int stream_tag)
{
	dev_info(chip->dev, "PCM start: tag=%u (captured sequence)\n",
		 stream_tag);

	/*
	 * Audio router + conn-point setup (from cold-boot capture L10970-L11080).
	 * Router writes fire at playback time, NOT during standup.
	 *
	 * Router data format (confirmed from capture):
	 *   {enable[31:16], flags[15:8], dst[7:0]}
	 *   enable=0x0001, flags=0x00, dst=0x14(L)/0x15(R) — fixed ports
	 * Trigger: 0x19002c=1 (not 0x19042c)
	 *
	 * Conn-point setup (L11069-11079):
	 *   CONN_POINT_ID=0x18, SR=0x14
	 *   PARAM_SET(0x3c = param_id=28, val=1)
	 *   CONN_POINT_ID=0x17, SR=0x0f
	 *
	 * Bind order from capture (L3970-L5288):
	 *   nid=0x15 first (DSP input gets stream), then DSP PARAM,
	 *   then nid=0x02 (DAC output).
	 */
	/*
	 * Audio router, conn-point setup, and stream remap are ChipIO-heavy
	 * (they use sync CORB reads via chipio_send/dsp_read). After standup
	 * the 8051 is in audio-loop mode and these reads time out, eventually
	 * filling the CORB ring. Run them only on the first PCM start.
	 */
	if (!chip->pcm_started) {
		chip->pcm_started = 1;
		creative_dsp_write_audio_router(chip);

		/* Conn-point setup from L11069-11079 */
		chipio_set_conn_rate(chip, 0x18, 0x14);
		chipio_set_control_param(chip, CONTROL_PARAM_CONN_POINT_ID, 0x17);
		chipio_set_control_param(chip, CONTROL_PARAM_CONN_POINT_SR, 0x0f);

		/* Stream 0x0c port mapping */
		chipio_remap_stream_0c(chip);
	}

	/*
	 * Windows-only SCP triggers (mod=0x18/0x19/0x15) removed.
	 * These commands do not appear in the decompiled Creative Linux driver
	 * (v0.73_kernel18) and cause the 8051 to enter halt=1 when sent during
	 * PCM playback. They were derived from Windows CORB captures and are
	 * not part of the Linux driver's audio activation sequence.
	 */

	/*
	 * All verbs in the PCM start path are write-only — use fire-and-forget
	 * (dsp_write_nowait / corb_send_verb) throughout. After standup the
	 * 8051 is in audio-loop mode and stops servicing CORB promptly; any
	 * sync read (dsp_read / chipio_send for status) causes 1s timeouts
	 * that cascade into CORB ring jams blocking all subsequent verbs.
	 *
	 * The verbs reach the codec hardware regardless of whether we wait
	 * for RIRB confirmation.
	 */

	/* Step 1: Bind DSP input (nid=0x15) to the HDA stream tag. */
	creative_ae9_corb_send_verb(chip,
		creative_ae9_verb(CAD, 0x15, 0x200, 0x0041));
	creative_ae9_corb_send_verb(chip,
		creative_ae9_verb(CAD, 0x15, 0x706,
				  (stream_tag & 0xf) << 4));

	/* Step 2: 0x0b0=0x08 writes */
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL, 0x0b0, 0x08);
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL, 0x0b0, 0x08);

	/* Step 3: SET_STREAM_FORMAT on DAC node 0x02 */
	creative_ae9_corb_send_verb(chip,
		creative_ae9_verb(CAD, 0x02, 0x200, 0x0035));

	/* Step 3b: DAC2PORT — ChipIO PARAM_EX 0x0d=0xa4 */
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL,
			 VENDOR_CHIPIO_PARAM_EX_ID_SET, 0x0d);
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL,
			 VENDOR_CHIPIO_PARAM_EX_VALUE_SET, 0xa4);

	/* Step 4: SET_CHANNEL_STREAMID — bind DAC node 0x02 to stream tag */
	creative_ae9_corb_send_verb(chip,
		creative_ae9_verb(CAD, 0x02, 0x706,
				  (stream_tag & 0xf) << 4));

	/*
	 * Step 5: Post-BIND activation sequence.
	 * a) Re-allocate DSP router ports (drain read replaced with nowait write)
	 * b) Three DIGI_CONVERT pairs — ChipIO stream format config
	 * c) Five ChipIO writes to 0x180000 region (DMA channel activation)
	 * d) Two final DIGI_CONVERT writes
	 */
	/* a) Re-allocate DSP router ports (skip sync drain) */
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL, 0x717, 0x17); /* 8ch, 2 ports/ch */
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL, 0x718, 0x08); /* MEM_CONNID_DSP */

	/* b) Three DIGI_CONVERT pairs */
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL, 0x70d, 0x40);
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL, 0x70c, 0xc7);
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL, 0x70d, 0x42);
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL, 0x70c, 0xcd);
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL, 0x70d, 0x41);
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL, 0x70c, 0xce);

	/* c) Five ChipIO writes to 0x180000 region (DMA channel activation).
	 * Use dsp_write_nowait for addr/data verbs — creative_dsp_chipio_write
	 * polls chipio status (sync CORB) which times out post-standup. */
#define chipio_write_nowait(chip, addr, data) do { \
	dsp_write_nowait((chip), WIDGET_CHIP_CTRL, \
			 VENDOR_CHIPIO_ADDRESS_LOW,  (addr) & 0xffff); \
	dsp_write_nowait((chip), WIDGET_CHIP_CTRL, \
			 VENDOR_CHIPIO_ADDRESS_HIGH, (addr) >> 16);    \
	dsp_write_nowait((chip), WIDGET_CHIP_CTRL, \
			 VENDOR_CHIPIO_DATA_LOW,     (data) & 0xffff); \
	dsp_write_nowait((chip), WIDGET_CHIP_CTRL, \
			 VENDOR_CHIPIO_DATA_HIGH,    (data) >> 16);    \
} while (0)
	chipio_write_nowait(chip, 0x180000, 0x00000001);
	chipio_write_nowait(chip, 0x180004, 0x00000001);
	chipio_write_nowait(chip, 0x180008, 0x00000001);
	chipio_write_nowait(chip, 0x180024, 0x00000005);
	chipio_write_nowait(chip, 0x180028, 0x0000020f);
#undef chipio_write_nowait

	/* d) Final DIGI_CONVERT writes */
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL, 0x70d, 0x7d);
	dsp_write_nowait(chip, WIDGET_CHIP_CTRL, 0x70e, 0x15);

	/* Log PCM stream LPIB (BAR0 direct read — no CORB) and 0x801c.
	 * Give activation verbs 100ms to land before sampling. */
	msleep(100);
	{
		unsigned int base = chip->playback_stream_base;
		u32 lpib = readl(chip->hda_regs + base + AZX_REG_SD_LPIB);
		unsigned int counter = 0;
		int r = creative_dsp_chipio_read(chip, 0x801c, &counter);
		dev_info(chip->dev,
			 "PCM start: LPIB=0x%x 0x801c=0x%x (ret=%d)\n",
			 lpib, counter, r);
	}
}

