// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 3xHelix
//
// Creative Sound Blaster AE-9: Windows' command sequences, written as code
// (as creative_ae9_seq.c, which has the start-up's): the ones sent as blocks
// while the card runs (recording, playback, DSP mode, the output switch, the
// shutdown) and the head unit's and BAR2's (their set-up after the DSP's
// start, the poll, the set-up resend, Windows' BAR2 init). tools/tablesc
// compares them with the tables in git history.
//

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <sound/hda_verbs.h>
#include "creative_ae9.h"
#include "ca0132_regs.h"
#include "creative_ae9_seq.h"

/* ------------------------------------------------------------------ */
/* Sending a block (the replay blocks' way)				  */
/* ------------------------------------------------------------------ */

/*
 * A block of Windows' commands sent as one, the way the driver replayed its
 * captured blocks (creative_ae9_verb_block_replay(), core.c until
 * 2026-11-16): under replay_mutex (a ChipIO access is several verbs another
 * block must not split), every verb waiting for its answer, DSP messages
 * through creative_dsp_scp_send() (which does the DSP's status waits itself;
 * Windows' status reads between the message words are not sent), the ChipIO
 * address cache invalidated at the end, one log line: every block at
 * start-up, failures afterwards (debug=1: all).
 */
struct ae9_blk {
	struct creative_chip *chip;
	unsigned int verbs, errs, msgs, msg_err;
};

static void blk_begin(struct ae9_blk *b, struct creative_chip *chip)
{
	*b = (struct ae9_blk){ .chip = chip };
	mutex_lock(&chip->replay_mutex);
}

/* A verb: blk_send() to codec 1, blk_send_c2() to codec 2 */
static void blk_send_cad(struct ae9_blk *b, u8 cad, u8 nid, u16 verb, u16 parm)
{
	u32 resp = 0;

	if (creative_ae9_corb_send_verb_sync(b->chip, creative_ae9_verb(cad, nid, verb, parm),
					     &resp) < 0)
		b->errs++;
	b->verbs++;
}

static void blk_send(struct ae9_blk *b, u8 nid, u16 verb, u16 parm)
{
	blk_send_cad(b, AE9_CAD, nid, verb, parm);
}

static void blk_send_c2(struct ae9_blk *b, u8 nid, u16 verb, u16 parm)
{
	blk_send_cad(b, AE9_CAD2, nid, verb, parm);
}

/*
 * A DSP message: SET request req of module mid, one data word. The header
 * as ca0132's make_scp_header() builds it: 1 data word, from source 0x20
 * (the host), as every message Windows sends.
 */
static void blk_scp_set(struct ae9_blk *b, u8 mid, u8 req, u32 val)
{
	u32 hdr = 1 << 27 | req << 17 | 0x20 << 8 | mid;

	if (creative_dsp_scp_send(b->chip, hdr, &val, 1) < 0)
		b->msg_err++;
	b->msgs++;
}

static void blk_end(struct ae9_blk *b, const char *what)
{
	struct creative_chip *chip = b->chip;

	creative_dsp_chipio_addr_invalidate();
	mutex_unlock(&chip->replay_mutex);
	if (b->errs || b->msg_err || !chip->dsp_done)
		dev_info(chip->dev, "%s, %u verbs (%u errors), %u SCP msgs (%u failed)\n",
			 what, b->verbs, b->errs, b->msgs, b->msg_err);
	else
		ae9_dbg(chip, "%s, %u verbs (%u errors), %u SCP msgs (%u failed)\n",
			what, b->verbs, b->errs, b->msgs, b->msg_err);
}

/* ------------------------------------------------------------------ */
/* Recording: the head unit's XLR/TRS input, codec 1 ADC 0x07		  */
/* ------------------------------------------------------------------ */

#define AE9_CAP_FMT_48K		0x0031	/* 48 kHz, 24-bit, 2 ch: Windows' format */

/* DSP levels are IEEE-754 floats, in dB (the kernel has no FPU: the bits) */
#define AE9_F_MINUS_90		0xc2b40000	/* -90.0 */
#define AE9_F_MINUS_18_74	0xc195eb85	/* -18.74 */
#define AE9_F_PLUS_8_99		0x410fd70a	/* 8.99 */
#define AE9_F_PLUS_9		0x41100000	/* 9.0 */

/*
 * One step of Windows' recording level, one channel (its KS volume setter,
 * CtxHda FUN_000218dc, then FUN_0002174c): ADC 0x07's fine step and input
 * amp (gain step = dB + 90, or muted), then the DSP's mic 1 level (module
 * 0x37): mute level 0 (not muted), left and right in dB, both every time.
 */
static void cap_level_step(struct ae9_blk *b, bool right, u8 fine, u8 amp,
			   u32 left_db, u32 right_db)
{
	blk_send(b, AE9_NID_ADC, right ? AE9_VERB_SET_FINE_VOL_R : AE9_VERB_SET_FINE_VOL_L,
		 fine);
	blk_send(b, AE9_NID_ADC, AC_VERB_SET_AMP_GAIN_MUTE,
		 AC_AMP_SET_INPUT | (right ? AC_AMP_SET_RIGHT : AC_AMP_SET_LEFT) | amp);
	blk_scp_set(b, 0x37, 0x01, 0);
	blk_scp_set(b, 0x37, 0x02, left_db);
	blk_scp_set(b, 0x37, 0x03, right_db);
}

/*
 * Windows' capture level block (XLR recording capture
 * vm_capture_coldboot_20261101_234608, tap seq 9136-9374): its Recording
 * level set a channel at a time, from the right channel's previous +8.99:
 * both muted, both -18.75 dB (amp -19 + 16/64), then left +9 dB, right
 * +8.98 dB (amp +8 + 63/64); the DSP gets -18.74, 9.0 and 8.99.
 */
static void ae9_cap_level_block(struct creative_chip *chip)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	cap_level_step(&b, false, 0, AC_AMP_MUTE, AE9_F_MINUS_90, AE9_F_PLUS_8_99);
	cap_level_step(&b, true, 0, AC_AMP_MUTE, AE9_F_MINUS_90, AE9_F_MINUS_90);
	cap_level_step(&b, false, 0x10, 71, AE9_F_MINUS_18_74, AE9_F_MINUS_90);
	cap_level_step(&b, true, 0x10, 71, AE9_F_MINUS_18_74, AE9_F_MINUS_18_74);
	cap_level_step(&b, false, 0, 99, AE9_F_PLUS_9, AE9_F_MINUS_18_74);
	cap_level_step(&b, true, 0x3f, 98, AE9_F_PLUS_9, AE9_F_PLUS_8_99);
	blk_end(&b, "capture: level block");
}

/*
 * The DSP's output mute off: module 0x96 (output effects), speaker tuning
 * mute (0x3a) = 0.0. Windows sends it whenever a stream pauses, at every
 * start and stop (CtxHda FUN_0001964c).
 */
static void cap_dsp_unmute(struct ae9_blk *b)
{
	blk_scp_set(b, 0x96, 0x3a, 0);
}

/*
 * Windows reads ChipIO control parameter 1, the voice-processor source, at
 * the bind (CtxHda FUN_000203a4 -> FUN_00016704: with Voice Clarity off it
 * reads 0 and does nothing more). The answer isn't used here.
 */
static void cap_vip_source_read(struct ae9_blk *b)
{
	blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_GET, CONTROL_PARAM_VIP_SOURCE);
}

/*
 * Windows' record start up to the ADC bind (XLR capture, tap seq
 * 7604-7618): the DSP unmuted, ADC 0x07's format, the VIP source read
 */
static void ae9_cap_start_block(struct creative_chip *chip, u16 format)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	cap_dsp_unmute(&b);
	blk_send(&b, AE9_NID_ADC, AC_VERB_SET_STREAM_FORMAT, format);
	cap_vip_source_read(&b);
	blk_end(&b, "capture: record start block");
}

/* The ADC bound to the capture stream, channel 0 (seq 7620) */
static void ae9_cap_bind(struct creative_chip *chip)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	blk_send(&b, AE9_NID_ADC, AC_VERB_SET_CHANNEL_STREAMID, CREATIVE_CAP_STREAM_TAG << 4);
	blk_end(&b, "capture: ADC bind");
}

/*
 * Windows' record stop (XLR capture, tap seq 12370-12384): the DSP
 * unmuted, ADC 0x07 unbound, the VIP source read (Windows read it twice
 * here; the driver once, as the table had it)
 */
static void ae9_cap_stop_block(struct creative_chip *chip)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	cap_dsp_unmute(&b);
	blk_send(&b, AE9_NID_ADC, AC_VERB_SET_CHANNEL_STREAMID, 0);
	cap_vip_source_read(&b);
	blk_end(&b, "capture: record stop block");
}

/*
 * Windows' record start, in Windows' order:
 * - the level block (node 0x07 amps + 18 messages to module 0x37), once
 *   per DSP load: Windows sends it once per stream setup (1-2 bursts per
 *   capture session against 8-15 ADC binds), never at a recording start;
 *   then the user's level (it overrides the block's +9 dB). A later level
 *   change sends itself (mic_level_put), the playback start block's 0x37
 *   messages re-apply it (pcm prepare).
 * - the start block, with this recording's format: Windows' block sets its
 *   48 kHz format, and the codec takes the rate at the bind, so the
 *   recording's own format goes there, before the bind (changing it after
 *   left a 96 kHz recording getting 48 kHz-rate data, 2026-11-06);
 * - route_vc: the voice processor in (Voice Clarity), then Windows' 60 ms
 *   (creative_ae9_cv_route). Windows routes in after the format and binds
 *   51-66 ms later with no DSP message in between (18 recording starts in
 *   4 captures, e.g. 2026-11-05 21:31); we used to route in first and send
 *   the 18 level messages + the start block's message within 1 ms of it:
 *   the DSP's command queue read full (queue-full kicks, lost level
 *   messages);
 * - the ADC bind.
 */
void creative_ae9_cap_start(struct creative_chip *chip, bool route_vc)
{
	u16 format = chip->cap_format ?: AE9_CAP_FMT_48K;

	if (!chip->corb_buf)
		return;
	if (!chip->cap_level_sent) {
		ae9_cap_level_block(chip);
		chip->cap_level_sent = true;
		if (chip->mic_level != 99)
			creative_ae9_mic_level_apply(chip);
	}
	ae9_cap_start_block(chip, format);
	if (route_vc)
		creative_ae9_cv_route(chip, true);
	ae9_cap_bind(chip);
}

/* Windows' record start (no Voice Clarity route) or stop */
void creative_ae9_capture_seq(struct creative_chip *chip, bool start)
{
	if (!chip->corb_buf)
		return;
	if (start)
		creative_ae9_cap_start(chip, false);
	else
		ae9_cap_stop_block(chip);
}

/* ------------------------------------------------------------------ */
/* Playback: host audio through codec 1 converter 0x02 to the DACs	  */
/* ------------------------------------------------------------------ */

/* the I2S descriptors at 44.1/48 kHz (Windows' I2S table, notes 08) */
#define AE9_I2S_CLK_48K		0x0001f100
#define AE9_I2S_RATE_48K	0x00008005

/* the card's sample-rate converter (CtxHda FUN_00039868) */
#define AE9_SRC_CTL		0x18a020	/* (rate multiple k << 6) | 2, then | 3 */
#define AE9_SRC_CFG		0x18a024
#define AE9_SRC_DIV		0x18a028

/*
 * Per-rate playback start, as Windows does it (capture 2026-11-03,
 * vm_capture_coldboot_20261103_192357; WINDOWS-CAPTURE-ANALYSIS §36). The
 * start block was captured at 48 kHz:
 *
 * 48 kHz family (48/96/192/384): the connection point's rate code,
 * converter 0x02's format and the I2S clock descriptors (0x189000/04/08,
 * 0x189024) per rate, and the router slots the 8051 gave the stream:
 * 2 x the rate multiple of them (creative_ae9_rt_direct_walk()). 384 kHz
 * runs the link at 192 kHz with 4 channels (two stereo samples per frame)
 * and sets DAC 48/0a = 0x12.
 *
 * 44.1 kHz family (44.1/88.2/176.4): no re-clock. DSP stream 0x05 goes to
 * connection point 0x91 (the card's sample-rate converter) at the stream's
 * rate code, the converter is set up (0x18a020/24/28 and its PLL/PMU
 * clock), and the block's router section is left out, as in Windows.
 *
 * Windows never writes BAR2 0x100, connection point 0x70 or the full PLL
 * program for a rate change.
 */
enum { AE9_ROUTE_DIRECT, AE9_ROUTE_SRC };
static const struct ae9_pcm_rate {
	unsigned int rate;
	u8 route;
	u8 cp_code;	/* connection point rate code (ca0132's enum) */
	u32 i2s_clk;	/* 0x189000/04/08 (direct route) */
	u32 i2s_rate;	/* 0x189024 (direct route) */
	u8 ports;	/* router slots per channel pair (direct route) */
	u8 dac0a;	/* DAC 48/0a */
	u32 src[5];	/* converter: 0x18a020, 0x18a020, 0x18a024, 0x18a028, 0x18a020 */
} ae9_pcm_rates[] = {
	{  44100, AE9_ROUTE_SRC,    0x08, 0, 0, 0, 0x02,
	   { 0x02, 0x02, 0x00101204, 0xfb, 0x03 } },
	{  48000, AE9_ROUTE_DIRECT, 0x09, AE9_I2S_CLK_48K, AE9_I2S_RATE_48K,  2, 0x02 },
	{  88200, AE9_ROUTE_SRC,    0x0a, 0, 0, 0, 0x02,
	   { 0x02, 0x42, 0x08101204, 0x7b, 0x43 } },
	{  96000, AE9_ROUTE_DIRECT, 0x0b, 0x0001f101, 0x00014004,  4, 0x02 },
	/*
	 * 176.4 kHz through the converter, from Windows' converter table
	 * (CtxHda.sys 6.0.105.0065: 0x18a020 k = 2, 0x0c101204 / 0x3b);
	 * rate code 0x0d = ca0132's SR_176_400. Windows never played it in
	 * our captures.
	 */
	{ 176400, AE9_ROUTE_SRC,    0x0d, 0, 0, 0, 0x02,
	   { 0x02, 0x82, 0x0c101204, 0x3b, 0x83 } },
	{ 192000, AE9_ROUTE_DIRECT, 0x0e, 0x0001f102, 0x00022003,  8, 0x02 },
	{ 384000, AE9_ROUTE_DIRECT, 0x0e, 0x0001f103, 0x00031002, 16, 0x12 },
};

/* 44.1/88.2 kHz: send Windows' extra DAC commands (see ae9_pb_dac_start()). */
static bool dac_441 = true;
module_param(dac_441, bool, 0644);
MODULE_PARM_DESC(dac_441, "44.1/88.2 kHz: send Windows' DAC commands 48/01 = 0xc1, 48/0b = 0x20 (1 = yes, default; 0 = as before)");

/*
 * Windows reprograms the ES9038 DAC at every stream start (12 commands,
 * including its input setup 48/01 and master mode 48/0a), and the DAC
 * re-locks: a brief hiccup ~0.5 s into each stream (heard in VLC, 2026-11-
 * 05: 2 of 2 starts with the commands, 0 of 2 without). So they're sent
 * only when the rate or output changed, or anything else touched the DAC
 * (any CA0113 command or a resume clears dac_start_key). 0 = send them at
 * every start, as Windows does.
 */
static bool dac_start_skip = true;
module_param(dac_start_skip, bool, 0644);
MODULE_PARM_DESC(dac_start_skip, "Skip the DAC start commands when rate and output are unchanged (1 = yes, default; 0 = every start, as Windows)");

/* 8051 address, as cold_8051_addr() (mainline chipio_8051_set_address()) */
static void blk_8051_addr(struct ae9_blk *b, u16 addr)
{
	blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_ADDRESS_LOW, addr & 0xff);
	blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_ADDRESS_HIGH, addr >> 8);
}

/* One byte of the 8051's XRAM read; the answer isn't used */
static void blk_xram_read(struct ae9_blk *b, u16 addr)
{
	blk_8051_addr(b, addr);
	blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_DATA_READ, 0);
}

/*
 * A stream's Active byte read, as Windows reads it before it enables or
 * disables the stream. The driver sends the read and then what Windows
 * did in the capture, whatever the answer: it isn't used.
 */
static void blk_stream_active_read(struct ae9_blk *b, u8 sid)
{
	blk_xram_read(b, AE9_XRAM_STREAM_ACTIVE(sid));
}

/* An 8051 PLL/PMU register, as cold_pll_pmu_write() */
static void blk_pll_pmu_write(struct ae9_blk *b, u8 reg, u8 val)
{
	blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_ADDRESS_LOW, reg);
	blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PLL_PMU_WRITE, val);
}

/* A ChipIO memory write, as cold_chipio_write(): address, data low, data high */
static void blk_chipio_write(struct ae9_blk *b, u32 addr, u32 data)
{
	blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_ADDRESS_LOW, addr & 0xffff);
	blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_ADDRESS_HIGH, addr >> 16);
	blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_DATA_LOW, data & 0xffff);
	blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_DATA_HIGH, data >> 16);
}

/*
 * A ChipIO control parameter (mainline chipio_set_control_param()): one
 * verb when the id is below 32 and the value below 8; else the two-verb
 * form after a ChipIO status read (sent; Windows' captured sequence doesn't
 * wait on its answer). Every parameter in Windows' playback start and stop
 * follows this rule.
 */
static void blk_ctl_param(struct ae9_blk *b, u8 id, u8 val)
{
	if (id < 32 && val < 8) {
		blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_SET, val << 5 | id);
		return;
	}
	blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_STATUS, 0);
	blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_EX_ID_SET, id);
	blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_EX_VALUE_SET, val);
}

/*
 * A converter's output level, one channel, as Windows' KS volume setter
 * sends it (CtxHda FUN_000218dc): the fine step (1/64 dB), then the amp
 */
static void blk_out_level(struct ae9_blk *b, u8 nid, bool right, u8 fine, u8 amp)
{
	blk_send(b, nid, right ? AE9_VERB_SET_FINE_VOL_R : AE9_VERB_SET_FINE_VOL_L, fine);
	blk_send(b, nid, AC_VERB_SET_AMP_GAIN_MUTE,
		 AC_AMP_SET_OUTPUT | (right ? AC_AMP_SET_RIGHT : AC_AMP_SET_LEFT) | amp);
}

/*
 * Windows' playback start (cold-boot capture vm_capture_coldboot_20261101_160959,
 * tap seq 5694-6096), first part (seq 5694-5776): the stream set-up of its
 * stream state handler (CtxHda FUN_000331c8, state 2: FUN_0003354c), the
 * converter's format, the port map and the bind.
 * - Chip register 0x18b008 bit 3 (a hold bit) cleared at every stream start
 *   (FUN_00022778): the data read that ends Windows' read of it (its
 *   address, post read and status came just before the captured block,
 *   seq 5686-5692: never sent), then the value Windows wrote (0xf0).
 * - DSP mode's output stream 0x18 off where it's on (FUN_00039a48 ->
 *   FUN_00034250): its Active byte read; it was off in the capture, so
 *   nothing more.
 * - Stream 0x05 (host audio, connection point 0x43) to the front DAC output
 *   0xd0, or to the sample-rate converter 0x91 for the 44.1 kHz family; its
 *   channels; that connection point's rate (FUN_00027648, FUN_000278b4).
 * - Stream 0x14 (the direct path) from 0x43 to connection point 0, that
 *   point at 96 kHz, its channels (FUN_00027594, FUN_000278b4, FUN_00027504).
 * - Converter 0x02's format (the stream's: captured 0x0031), the port map
 *   (dac2port) for the output, converter 0x02 bound to the playback stream,
 *   channel 0.
 * six: a 6-channel stream (5.1), 2 otherwise; hp: the output is the
 * headphones.
 */
static void pb_stream_setup(struct ae9_blk *b, const struct ae9_pcm_rate *row, u16 format,
			    bool six, bool hp)
{
	u8 cp = row->route == AE9_ROUTE_SRC ? AE9_CP_SRC : AE9_CP_FRONT_DAC;
	u8 ch = six ? 6 : 2;

	blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_HIC_READ_DATA, 0);
	blk_chipio_write(b, 0x18b008, 0x000000f0);

	blk_stream_active_read(b, AE9_STREAM_DSP_OUT);

	blk_ctl_param(b, CONTROL_PARAM_STREAM_ID, AE9_STREAM_HOST);
	blk_ctl_param(b, CONTROL_PARAM_STREAM_SOURCE_CONN, AE9_CP_HOST);
	blk_ctl_param(b, CONTROL_PARAM_STREAM_DEST_CONN, cp);
	blk_ctl_param(b, CONTROL_PARAM_STREAMS_CHANNELS, ch);
	blk_ctl_param(b, CONTROL_PARAM_CONN_POINT_ID, cp);
	blk_ctl_param(b, CONTROL_PARAM_CONN_POINT_SR, row->cp_code);

	blk_ctl_param(b, CONTROL_PARAM_STREAM_ID, AE9_STREAM_DIRECT);
	blk_ctl_param(b, CONTROL_PARAM_STREAM_SOURCE_CONN, AE9_CP_HOST);
	blk_ctl_param(b, CONTROL_PARAM_STREAM_DEST_CONN, 0);
	blk_ctl_param(b, CONTROL_PARAM_CONN_POINT_ID, 0);
	blk_ctl_param(b, CONTROL_PARAM_CONN_POINT_SR, AE9_SR_96000);
	blk_ctl_param(b, CONTROL_PARAM_STREAM_ID, AE9_STREAM_DIRECT);
	blk_ctl_param(b, CONTROL_PARAM_STREAMS_CHANNELS, ch);

	blk_send(b, AE9_NID_DAC, AC_VERB_SET_STREAM_FORMAT, format);
	blk_ctl_param(b, AE9_CTL_PARAM_DAC2PORT, hp ? 0xa1 : 0xa4);
	blk_send(b, AE9_NID_DAC, AC_VERB_SET_CHANNEL_STREAMID, CREATIVE_PCM_STREAM_TAG << 4);
}

/*
 * The I2S to the DACs clocked for the rate (seq 5778-5834; Windows
 * FUN_000339ec -> FUN_00033bac): ASI 8, the 8051's chip pin functions
 * (PLL/PMU 0x40 = 0xc7: pin 0, the ES9038's data line = SDO0; 0x42 = 0xcd:
 * pin 2 = SDO1; 0x41 = 0xce: pin 1, function 0xe, meaning unknown), the
 * three clock descriptors, the rate descriptor and 0x189028 (0x2000f at
 * every rate). clk/rate: Windows' per-rate values; the 44.1 kHz family
 * keeps the 48 kHz ones.
 */
static void pb_i2s_clocks(struct ae9_blk *b, u32 clk, u32 rate)
{
	blk_ctl_param(b, CONTROL_PARAM_ASI, 8);
	blk_pll_pmu_write(b, 0x40, 0xc7);
	blk_pll_pmu_write(b, 0x42, 0xcd);
	blk_pll_pmu_write(b, 0x41, 0xce);
	blk_chipio_write(b, 0x189000, clk);
	blk_chipio_write(b, 0x189004, clk);
	blk_chipio_write(b, 0x189008, clk);
	blk_chipio_write(b, 0x189024, rate);
	blk_chipio_write(b, 0x189028, 0x0002000f);
}

/*
 * The 44.1 kHz family's sample-rate converter, in place of the block's
 * router section (Windows' captures 2026-11-03/05 at 44.1 and 88.2 kHz;
 * its converter table, FUN_00039868, for 176.4): enabled at the stream's
 * rate multiple, the converter clock (PLL/PMU 0x01 = 0x17, 0x40 = 0x06,
 * 0x04 = 0x80: bit 7 = the 44.1 kHz family; Windows, at both 44.1 and
 * 88.2), its configuration and divider, then running.
 */
static void pb_src_setup(struct ae9_blk *b, const struct ae9_pcm_rate *row)
{
	blk_chipio_write(b, AE9_SRC_CTL, row->src[0]);
	blk_pll_pmu_write(b, 0x01, 0x17);
	blk_pll_pmu_write(b, 0x40, 0x06);
	blk_pll_pmu_write(b, 0x04, 0x80);
	blk_chipio_write(b, AE9_SRC_CTL, row->src[1]);
	blk_chipio_write(b, AE9_SRC_CFG, row->src[2]);
	blk_chipio_write(b, AE9_SRC_DIV, row->src[3]);
	blk_chipio_write(b, AE9_SRC_CTL, row->src[4]);
}

/*
 * Stream 0x14 on (seq 5880-5900; FUN_000336d8, state 3: FUN_000341e4,
 * Active read, off in the capture: the stream selected, its control 1),
 * then ASI 0xf (FUN_000336d8's last step)
 */
static void pb_stream_on(struct ae9_blk *b)
{
	blk_stream_active_read(b, AE9_STREAM_DIRECT);
	blk_ctl_param(b, CONTROL_PARAM_STREAM_ID, AE9_STREAM_DIRECT);
	blk_ctl_param(b, CONTROL_PARAM_STREAM_CONTROL, 1);
	blk_ctl_param(b, CONTROL_PARAM_ASI, 0x0f);
}

/*
 * One channel of converter 0x02 at 0 dB, then the DSP's speaker volume
 * (module 0x32: left and right -36.19 dB, volume update; not in the AE-9's
 * output path, uv44), as Windows' volume setter sends it
 */
static void pb_dac_level_step(struct ae9_blk *b, bool right)
{
	blk_out_level(b, AE9_NID_DAC, right, 0, AE9_AMP_0DB);
	blk_scp_set(b, 0x32, 0x03, AE9_F_MINUS_36_19);
	blk_scp_set(b, 0x32, 0x04, AE9_F_MINUS_36_19);
	blk_scp_set(b, 0x32, 0x02, 0);
}

/*
 * The levels Windows sets as the stream starts (seq 5902-6096; its KS
 * volume setter, FUN_000218dc + FUN_0002174c): ADC 0x07 at +9 dB and the
 * DSP's mic 1 level (left, right: the recording level's last step), the
 * XLR pin's input amp (the user's Mic Gain, captured +10 dB), converter
 * 0x02 at 0 dB with the DSP's speaker volume, converters 0x03 and 0x04 at
 * 0 dB.
 */
static void pb_levels(struct ae9_blk *b, unsigned int mic_boost)
{
	cap_level_step(b, false, 0, 99, AE9_F_PLUS_9, AE9_F_PLUS_9);
	cap_level_step(b, true, 0, 99, AE9_F_PLUS_9, AE9_F_PLUS_9);
	blk_send(b, AE9_NID_MIC_XLR, AC_VERB_SET_AMP_GAIN_MUTE,
		 AC_AMP_SET_INPUT | AC_AMP_SET_LEFT | mic_boost);
	blk_send(b, AE9_NID_MIC_XLR, AC_VERB_SET_AMP_GAIN_MUTE,
		 AC_AMP_SET_INPUT | AC_AMP_SET_RIGHT | mic_boost);
	pb_dac_level_step(b, false);
	pb_dac_level_step(b, true);
	blk_out_level(b, 0x03, false, 0, AE9_AMP_0DB);
	blk_out_level(b, 0x03, true, 0, AE9_AMP_0DB);
	blk_out_level(b, 0x04, false, 0, AE9_AMP_0DB);
	blk_out_level(b, 0x04, true, 0, AE9_AMP_0DB);
}

/*
 * Windows' playback start block (seq 5694-6096), for the rate row, the
 * stream format and the output:
 * - the 48 kHz family (direct route): the stream set-up and the I2S clocks
 *   as one block; then the router: Windows walks the slot list the 8051
 *   gave the stream (its 0x157d read, slot reads and writes, seq
 *   5836-5870) and so do we, in core.c (creative_ae9_rt_direct_walk(),
 *   which takes replay_mutex itself: never inside a block); then a second
 *   block: the router table applied (0x19042c = 1), stream 0x14 on, the
 *   levels;
 * - the 44.1 kHz family: one block: the stream set-up, the I2S on its 48
 *   kHz clocks (after a 384 kHz stream the I2S stayed on its 384 kHz clocks
 *   (0x1f103) and the converter's enable didn't take: silent until a 48
 *   kHz-family stream re-clocked it, 2026-11-06; Windows' own I2S table
 *   has a 44.1 kHz entry with the 48 kHz values), the converter in place
 *   of the router section, stream 0x14 on, the levels.
 * The block's captured values patched: converter 0x02's format, stream
 * 0x05's destination and rate, the I2S clocks, 6 channels for a 5.1
 * stream, the port map for headphones, the Mic Gain.
 */
static void ae9_pb_start_block(struct creative_chip *chip, const struct ae9_pcm_rate *row,
			       u16 format, bool six, bool hp)
{
	const char *what = hp ? "pcm_windows: playback start block (headphones)" :
				"pcm_windows: playback start block";
	struct ae9_blk b;

	blk_begin(&b, chip);
	pb_stream_setup(&b, row, format, six, hp);
	if (row->route == AE9_ROUTE_SRC) {
		pb_i2s_clocks(&b, AE9_I2S_CLK_48K, AE9_I2S_RATE_48K);
		pb_src_setup(&b, row);
	} else {
		pb_i2s_clocks(&b, row->i2s_clk, row->i2s_rate);
		blk_end(&b, what);
		creative_ae9_rt_direct_walk(chip, six ? 3 * row->ports : row->ports, six);
		blk_begin(&b, chip);
		what = "pcm_windows: playback start block, after the router";
		blk_chipio_write(&b, AE9_RT_APPLY, 1);
	}
	pb_stream_on(&b);
	pb_levels(&b, chip->mic_boost);
	blk_end(&b, what);
}

/*
 * The head unit's front DAC, an ESS ES9038Q2M, behind the CA0113: group
 * 0x48. Its registers (ESS datasheet; docs/DECODED-SEQUENCES.md):
 */
#define AE9_DAC_FRONT		0x48
#define ES9038_INPUT_SELECT	0x01	/* 0xc0 32-bit I2S, 0xc1 S/PDIF (on DATA2) */
#define ES9038_SYNC_CONFIG	0x0a	/* bits 3:0 lock speed, bit 4 128 fs mode */
#define ES9038_SPDIF_SELECT	0x0b	/* S/PDIF input: 0x20 = DATA2 */
#define ES9038_THD_BYPASS	0x0d	/* bit 6: THD compensation off */
#define ES9038_MASTER_TRIM	0x11	/* 0x11-0x14, low byte first: 0x7fffffff = 0 dB */
#define ES9038_THD_C2		0x16	/* 0x16/0x17: THD compensation C2, low byte first */
#define ES9038_THD_C3		0x18	/* 0x18/0x19: C3 */
#define ES9038_GPIO_INVERT	0x1d	/* bit 7: GPIO2 (mutes the card's RCA out), bit 6: GPIO1 */

static void pb_dac(u8 (*cmds)[3], unsigned int *n, u8 reg, u8 val)
{
	cmds[*n][0] = AE9_DAC_FRONT;
	cmds[*n][1] = reg;
	cmds[*n][2] = val;
	(*n)++;
}

/*
 * Windows' DAC commands at a playback start (cold-boot capture, CORB writes
 * #2844-2874), as one list (one ca0113_mutex section, dsp.c): the output's
 * GPIO and THD set-up, the master trim at 0 dB, the input and the sync.
 * Captured on speakers; for the headphone jack Windows' output switch sets
 * 48/1d = 0x80 and 48/19 = 1, so a stream started on headphones gets
 * those, or it would turn the headphones off. With Line Out Virtualization
 * the line out carries the headphone mode (map 0xa1): the speaker values.
 * hp: the head unit's output is the headphones; sync: 48/0a for the rate
 * (0x12 at 384 kHz: 128 fs).
 *
 * fam441: the 44.1 kHz family through the sample-rate converter (with
 * dac_441). Windows sends 48/01 = 0xc1 (S/PDIF input; 0xc0 for the 48 kHz
 * family) and 48/0b = 0x20 before 48/0a, at every such start (all
 * captures; 2026-11-05 01:59:56 and 02:00:18). Without them our 44.1 kHz
 * played garbled or silent. Windows READS 48/0b first (0x20c = 0x800003);
 * that read was decoded as "48/0b = 0x00" and sent as a write until
 * 2026-11-08 (25 of 25 captures: never a 0x00 write); it isn't sent.
 */
static void ae9_pb_dac_start(struct creative_chip *chip, bool hp, bool fam441, u8 sync)
{
	bool hp_mode = hp && !chip->lo_virt_phys;
	u8 cmds[13][3];	/* 12, 13 for the 44.1 kHz family */
	unsigned int n = 0;

	pb_dac(cmds, &n, ES9038_GPIO_INVERT, hp_mode ? 0x80 : 0x40);
	/* Windows' per-output THD table, 5 bytes */
	pb_dac(cmds, &n, ES9038_THD_BYPASS, 0x00);
	pb_dac(cmds, &n, ES9038_THD_C2, 0x00);
	pb_dac(cmds, &n, ES9038_THD_C2 + 1, 0x00);
	pb_dac(cmds, &n, ES9038_THD_C3, 0x00);
	pb_dac(cmds, &n, ES9038_THD_C3 + 1, hp_mode ? 0x01 : 0x00);
	pb_dac(cmds, &n, ES9038_MASTER_TRIM, 0xff);
	pb_dac(cmds, &n, ES9038_MASTER_TRIM + 1, 0xff);
	pb_dac(cmds, &n, ES9038_MASTER_TRIM + 2, 0xff);
	pb_dac(cmds, &n, ES9038_MASTER_TRIM + 3, 0x7f);
	pb_dac(cmds, &n, ES9038_INPUT_SELECT, fam441 ? 0xc1 : 0xc0);
	if (fam441)
		pb_dac(cmds, &n, ES9038_SPDIF_SELECT, 0x20);
	pb_dac(cmds, &n, ES9038_SYNC_CONFIG, sync);
	creative_dsp_ca0113_list(chip, cmds, n,
				 fam441 ? "playback-start commands (44.1 kHz family)" :
					  "playback-start commands");
}

/*
 * Windows' playback stop (cold-boot capture, tap seq 6110-6128; its stream
 * state handler CtxHda FUN_000331c8, state 0): converter 0x02 unbound;
 * stream 0x14 disabled (FUN_00034250: Active read, then the stream
 * selected and its control 0); converter 0x02's left level at 0 dB
 * (Windows' volume setter).
 *
 * After the stream stop the 8051 tears the stream's routes down. Windows
 * waits 48 ms there (capture 6, seq 9954 -> 9956) and 144 ms on the Direct
 * Mode on stop (11808); going straight on left random router layouts
 * (2026-11-04: no DSP input, half inputs, outputs to 0xff): 50 ms, inside
 * the block.
 */
static void ae9_pb_stop_block(struct creative_chip *chip)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	blk_send(&b, AE9_NID_DAC, AC_VERB_SET_CHANNEL_STREAMID, 0);
	blk_stream_active_read(&b, AE9_STREAM_DIRECT);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_ID, AE9_STREAM_DIRECT);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_CONTROL, 0);
	msleep(50);
	blk_out_level(&b, AE9_NID_DAC, false, 0, AE9_AMP_0DB);
	blk_end(&b, "pcm_windows: playback stop block");
}

/*
 * A playback start at prepare (pcm.c, once per open, rate or format):
 * - rate: its row of ae9_pcm_rates (none: the 48 kHz one, the block as
 *   captured, with a warning);
 * - Direct Mode off, a full start block already run (and no switch to DSP
 *   mode pending): Windows' start is only converter 0x02's format and bind
 *   (creative_ae9_dsp_mode_start(), core.c), and the DSP's output unmute;
 * - the DAC commands, unless rate and output are what they were last sent
 *   for (dac_start_skip);
 * - Direct Mode, the first start since the load or the last DSP mode: no
 *   DSP route may feed a DAC port (the router keeps an earlier load's
 *   until power-off): swept (core.c);
 * - the start block (ae9_pb_start_block()); a 5.1 stream in Direct Mode up
 *   to 192 kHz gets 6 channels and its router slots (not for the DSP-mode
 *   lead-in: one started that way left the DSP with only the front pair
 *   after the switch, 2026-11-04; this said up to 96 kHz until 2026-11-05,
 *   and 5.1 at 192 kHz got the stereo set-up);
 * - with Direct Mode off, the first start (or one made while the switch
 *   is pending) runs as a direct stream and then switches live once it
 *   runs (creative_ae9_dsp_switch_work(), core.c): switching before any
 *   stream had run left the 8051's DSP input path broken (2026-11-04:
 *   inactive or half-built router entries, silence or crunched audio); the
 *   live switch from a running direct stream works every time.
 */
static void ae9_pb_start(struct creative_chip *chip, unsigned int rate, u16 format)
{
	bool hp = chip->hu_output;
	unsigned int r = 1;	/* the 48 kHz row: the block as captured */
	const struct ae9_pcm_rate *row;
	unsigned int i;
	bool six;

	for (i = 0; i < ARRAY_SIZE(ae9_pcm_rates); i++)
		if (ae9_pcm_rates[i].rate == rate)
			r = i;
	row = &ae9_pcm_rates[r];
	if (rate && row->rate != rate)
		dev_warn(chip->dev, "pcm_rates: no setup for %u Hz, using 48 kHz\n", rate);
	if (!chip->fx.direct && chip->dsp_primed && !chip->dsp_switch_pending) {
		creative_ae9_dsp_mode_start(chip, format);
		creative_ae9_fx_unmute(chip);	/* Windows: at every stream start */
		return;
	}
	if (dac_start_skip && chip->dac_start_key == ((r + 1) | hp << 8)) {
		ae9_dbg(chip, "dac_start_skip: DAC already set for %u Hz, start commands skipped\n",
			row->rate);
	} else {
		ae9_pb_dac_start(chip, hp, row->route == AE9_ROUTE_SRC && dac_441, row->dac0a);
		chip->dac_start_key = (r + 1) | hp << 8;
	}
	six = (format & 0xf) == 5 && row->ports <= 8 && chip->fx.direct;
	if (chip->fx.direct && !chip->rt_direct_swept) {
		creative_ae9_rt_dsp_dac_off_all(chip, "first Direct Mode start");
		chip->rt_direct_swept = true;
	}
	if (row->rate != 48000)
		dev_info(chip->dev, "pcm_rates: playback at %u Hz, format 0x%04x (%s, rate code 0x%02x)\n",
			 row->rate, format,
			 row->route == AE9_ROUTE_SRC ? "via the sample-rate converter" : "direct",
			 row->cp_code);
	ae9_pb_start_block(chip, row, format, six, hp);
	chip->dsp_primed = true;
	if (!chip->fx.direct)
		chip->dsp_switch_pending = true;
}

/*
 * Windows' playback start (prepare) or stop (hw_free). Both run in process
 * context and sleep (replay_mutex, the stop's 50 ms, the router walk, the
 * DAC list's mutex): never from the trigger callback.
 */
void creative_ae9_playback_seq(struct creative_chip *chip, bool start,
			       unsigned int rate, u16 format)
{
	if (!chip->corb_buf)
		return;
	if (start) {
		ae9_pb_start(chip, rate, format);
		return;
	}
	ae9_pb_stop_block(chip);
	creative_ae9_fx_unmute(chip);	/* Windows: and stop */
}

/* ------------------------------------------------------------------ */
/* DSP mode: host audio through the DSP (Direct Mode off) and back	  */
/* ------------------------------------------------------------------ */

/*
 * Windows' Direct Mode switch (capture 6, vm_capture_coldboot_20261103_233115;
 * capture 5, 22:56:39, with music playing; WINDOWS-CAPTURE-ANALYSIS §38) and
 * the blocks the driver adds around it. core.c's creative_ae9_dsp_mode() and
 * ae9_dsp_mode_start() decide when (Direct Mode off or on, a stream's start in
 * DSP mode, stream 0x18's count, the drains, the format check) and keep the
 * state; these are the blocks they send, each one replay_mutex section where
 * the driver's lists were (until 2026-11-16, replayed by
 * creative_ae9_verb_block_replay()). Windows' ChipIO status reads are sent:
 * the 8051 takes a parameter's two-verb form only after one (sent without
 * them, as an early version did, the routing parameters were dropped and the
 * audio kept the direct path, 2026-11-04).
 */

/*
 * A ChipIO control parameter in its two-verb form whatever its value (the
 * status read, sent, its answer not checked; the id; the value): the form
 * the driver's own blocks select a stream in (mainline and Windows take it
 * only for an id above 31 or a value above 7: blk_ctl_param())
 */
static void blk_ctl_param_ex(struct ae9_blk *b, u8 id, u8 val)
{
	blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_STATUS, 0);
	blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_EX_ID_SET, id);
	blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_EX_VALUE_SET, val);
}

/*
 * The selected stream stopped (its control 0), then the wait while the 8051
 * tears its routes down (Windows' 48 ms; see ae9_pb_stop_block()): 50 ms,
 * inside the block, whether the stop was answered or not
 */
static void blk_stream_stop(struct ae9_blk *b)
{
	blk_ctl_param(b, CONTROL_PARAM_STREAM_CONTROL, 0);
	msleep(50);
}

/*
 * The 8051's chip pin functions for the I2S to the DACs, in Windows' order
 * (tl_i2s_pins(): pin 4 MCLK, 3 BCLK, 5 LRCK, 0 SDO0, 2 SDO1, pin 1
 * function 0xe; the ASI clock pin select 0x51 = 0xdb)
 */
static void blk_i2s_pins(struct ae9_blk *b)
{
	blk_pll_pmu_write(b, 0x44, 0xc8);
	blk_pll_pmu_write(b, 0x43, 0xcc);
	blk_pll_pmu_write(b, 0x45, 0xcb);
	blk_pll_pmu_write(b, 0x40, 0xc7);
	blk_pll_pmu_write(b, 0x42, 0xcd);
	blk_pll_pmu_write(b, 0x41, 0xce);
	blk_pll_pmu_write(b, 0x51, 0xdb);
}

/*
 * Windows' Direct Mode off, up to its DAC re-lock (capture 6, tap seq
 * 9940-10074): converter 0x02 unbound; the direct stream 0x14 disabled as
 * Windows disables a stream (CtxHda FUN_00034250: its Active byte read, the
 * answer not used; the stream selected, stopped, the 50 ms); stream 0x05
 * (host audio, 0x43) to connection point 0, the DSP's input; stream 0x18
 * from the DSP's output (0x09) to the front DAC 0xd0, 0xd0 at 96 kHz, 6
 * channels, its Active byte read, enabled (FUN_000341e4); the I2S on the
 * DSP's 96 kHz clocks (pb_i2s_clocks(): ASI 8, three pin functions, the
 * 88.2/96 kHz descriptors 0x1f101 and 0x14004, 0x189028)
 */
static void dm_route_dsp(struct creative_chip *chip)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	blk_send(&b, AE9_NID_DAC, AC_VERB_SET_CHANNEL_STREAMID, 0);
	blk_stream_active_read(&b, AE9_STREAM_DIRECT);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_ID, AE9_STREAM_DIRECT);
	blk_stream_stop(&b);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_ID, AE9_STREAM_HOST);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_SOURCE_CONN, AE9_CP_HOST);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_DEST_CONN, 0);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_ID, AE9_STREAM_DSP_OUT);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_SOURCE_CONN, AE9_CP_DSP_OUT);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_DEST_CONN, AE9_CP_FRONT_DAC);
	blk_ctl_param(&b, CONTROL_PARAM_CONN_POINT_ID, AE9_CP_FRONT_DAC);
	blk_ctl_param(&b, CONTROL_PARAM_CONN_POINT_SR, AE9_SR_96000);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_ID, AE9_STREAM_DSP_OUT);
	blk_ctl_param(&b, CONTROL_PARAM_STREAMS_CHANNELS, 6);
	blk_stream_active_read(&b, AE9_STREAM_DSP_OUT);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_ID, AE9_STREAM_DSP_OUT);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_CONTROL, 1);
	pb_i2s_clocks(&b, 0x0001f101, 0x00014004);
	blk_end(&b, "SBX: DSP mode (host audio -> DSP -> DAC at 96 kHz)");
}

/*
 * One reference of a DSP stream given back (the driver's, not a Windows
 * list): the stream selected in the two-verb form, stopped, the 50 ms. The
 * 8051 counts a stream's enables and disables, never below 0, and tears
 * the stream down at 0 (see core.c's ae9_stream_drain()).
 */
static void dm_stream_release(struct creative_chip *chip, u8 sid, const char *what)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	blk_ctl_param_ex(&b, CONTROL_PARAM_STREAM_ID, sid);
	blk_stream_stop(&b);
	blk_end(&b, what);
}

/*
 * The rest of Windows' Direct Mode off (capture 6, tap seq 10076-10108,
 * after its DAC re-lock): the I2S pins, ASI 0xf
 */
static void dm_i2s_pins(struct creative_chip *chip)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	blk_i2s_pins(&b);
	blk_ctl_param(&b, CONTROL_PARAM_ASI, 0x0f);
	blk_end(&b, "SBX: DSP mode, full PLL program");
}

/*
 * Direct Mode off (core.c creative_ae9_dsp_mode(chip, true)): host audio
 * into the DSP, the DSP's output to the DACs at 96 kHz.
 * - The route block (dm_route_dsp());
 * - release18: stream 0x18's count read above 0 before it (the start-up
 *   tail's connection still stands after a bring-up): the block's enable
 *   is one too many (Windows, FUN_000341e4, enables it only at count 0),
 *   so one reference is given back, and DSP mode on/off leaves the count
 *   where it was;
 * - the front DAC follows the new I2S clock: Windows sends 48/01 and 48/0a
 *   right after the descriptors (captures 5 and 6), as every playback
 *   start does (ae9_pb_dac_start()): 0xc0 (32-bit I2S input) and 0x02 (the
 *   sync configuration, master mode), one list (one ca0113_mutex section);
 * - Windows' 17 ms between the descriptors and the pins (seq 10076): 20 ms;
 * - the I2S pins and ASI 0xf (dm_i2s_pins()).
 */
void creative_ae9_dsp_route_on(struct creative_chip *chip, bool release18)
{
	static const u8 relock[][3] = {
		{ AE9_DAC_FRONT, ES9038_INPUT_SELECT, 0xc0 },
		{ AE9_DAC_FRONT, ES9038_SYNC_CONFIG, 0x02 },
	};

	dm_route_dsp(chip);
	if (release18)
		dm_stream_release(chip, AE9_STREAM_DSP_OUT, "stream 0x18: already connected");
	creative_dsp_ca0113_list_quiet(chip, relock, ARRAY_SIZE(relock));
	msleep(20);
	dm_i2s_pins(chip);
}

/*
 * Windows' Direct Mode on (capture 6, tap seq 11794-11812; core.c
 * creative_ae9_dsp_mode(chip, false)): converter 0x02 unbound; stream 0x18
 * disabled (its Active byte read, the stream selected, stopped, the 50 ms);
 * then a ChipIO status read, the capture's last verb of the range (sent,
 * its answer not used). The next start block routes direct again (Windows
 * was silent until stop/play too).
 */
void creative_ae9_dsp_route_off(struct creative_chip *chip)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	blk_send(&b, AE9_NID_DAC, AC_VERB_SET_CHANNEL_STREAMID, 0);
	blk_stream_active_read(&b, AE9_STREAM_DSP_OUT);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_ID, AE9_STREAM_DSP_OUT);
	blk_stream_stop(&b);
	blk_send(&b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_STATUS, 0);
	blk_end(&b, "SBX: direct mode (DSP stream 0x18 off)");
}

/* One reference of DSP stream sid given back (core.c's stream drain) */
void creative_ae9_stream_release(struct creative_chip *chip, u8 sid)
{
	dm_stream_release(chip, sid, "stream release");
}

/*
 * Stream 0x05's channel count before a DSP-mode bind (ours, 2026-11-11; see
 * core.c ae9_dsp_mode_start()): the stream selected in the two-verb form,
 * its channels (6 for a 5.1 stream, as Windows' DSP-mode streams, capture 6;
 * else 2), a ChipIO status read (sent, its answer not used). A parameter
 * only: no routing happens at this write.
 */
void creative_ae9_dsp_in_channels(struct creative_chip *chip, u16 format)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	blk_ctl_param_ex(&b, CONTROL_PARAM_STREAM_ID, AE9_STREAM_HOST);
	blk_ctl_param(&b, CONTROL_PARAM_STREAMS_CHANNELS, (format & 0xf) == 5 ? 6 : 2);
	blk_send(&b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_STATUS, 0);
	blk_end(&b, "SBX: DSP input stream 0x05 channels");
}

/*
 * A playback start with Direct Mode off (capture 6, tap seq 11066: Windows'
 * next stream after its Direct Mode off, capture 5, 22:56:47): 0x18b008 bit
 * 3 cleared as at every stream start (Windows' read's last verb, then 0xf0;
 * pb_stream_setup()), converter 0x02's format, the bind (the playback
 * stream, channel 0). Replaying the start block here would undo the
 * routing.
 */
void creative_ae9_dsp_bind(struct creative_chip *chip, u16 format)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	blk_send(&b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_HIC_READ_DATA, 0);
	blk_chipio_write(&b, 0x18b008, 0x000000f0);
	blk_send(&b, AE9_NID_DAC, AC_VERB_SET_STREAM_FORMAT, format);
	blk_send(&b, AE9_NID_DAC, AC_VERB_SET_CHANNEL_STREAMID, CREATIVE_PCM_STREAM_TAG << 4);
	blk_end(&b, "SBX: playback start in DSP mode");
}

/*
 * The format check's repair (ours, 2026-11-07; core.c ae9_dsp_mode_start():
 * the router chip kept the previous stream's format): converter 0x02
 * unbound; once the 8051's queue is idle, its format and the bind again
 */
void creative_ae9_dsp_unbind(struct creative_chip *chip)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	blk_send(&b, AE9_NID_DAC, AC_VERB_SET_CHANNEL_STREAMID, 0);
	blk_end(&b, "SBX: unbind (format repair)");
}

void creative_ae9_dsp_rebind(struct creative_chip *chip, u16 format)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	blk_send(&b, AE9_NID_DAC, AC_VERB_SET_STREAM_FORMAT, format);
	blk_send(&b, AE9_NID_DAC, AC_VERB_SET_CHANNEL_STREAMID, CREATIVE_PCM_STREAM_TAG << 4);
	blk_end(&b, "SBX: bind again (format repair)");
}

/* ------------------------------------------------------------------ */
/* The head unit and the DACs: the output switch			  */
/* ------------------------------------------------------------------ */

/*
 * The card's GPIO (BAR2 0x320: bits 3:0 the pin, bit 8 its level; pin 0 the
 * DACs' reset/enable, 1 DSD mode, 2 unknown, 3 the surround outputs' enable,
 * 4 the card's LED, 5 the head unit's power; docs/DECODED-SEQUENCES.md)
 */
#define AE9_BAR2_GPIO		0x320

static void hu_gpio(struct creative_chip *chip, u8 pin, bool on)
{
	creative_ext_writew(chip, AE9_BAR2_GPIO, (on ? 0x100 : 0) | pin);
}

/* One DAC command, as a list of its own (dsp.c: its own ca0113_mutex section, 4 ms settle) */
static void hu_dac(struct creative_chip *chip, u8 group, u8 reg, u8 val)
{
	const u8 cmd[1][3] = { { group, reg, val } };

	creative_dsp_ca0113_list_quiet(chip, cmd, 1);
}

/*
 * A head-unit frame (struct ae9_hu_frame, creative_ae9.h): f0 <command>
 * <length> <length bytes> f7, its UART protocol (i2c.c; the commands are
 * decoded in docs/DECODED-SEQUENCES.md). One frame and its reply (not used
 * here); < 0: no reply.
 */
static int hu_frame(struct creative_chip *chip, const struct ae9_hu_frame *f)
{
	u8 resp[24];

	return creative_spi_frame_win(chip, f->d, f->len, resp, sizeof(resp));
}

/* the knob LED's blink period = 0: steady (Windows sends it after every volume change) */
static const struct ae9_hu_frame hu_blink_off = { 7, { 0xf0, 0x21, 0x03, 0x02, 0x00, 0x00, 0xf7 } };

/*
 * The DSP's output mute (module 0x96, speaker tuning mute 0x3a): 1.0 while
 * Windows switches, 0 after
 */
static void sw_dsp_mute(struct ae9_blk *b, bool on)
{
	blk_scp_set(b, 0x96, 0x3a, on ? AE9_F_ONE : 0);
}

/* the DSP speaker volume's update (module 0x32 req 2) */
static void sw_dsp_vol_update(struct ae9_blk *b)
{
	blk_scp_set(b, 0x32, 0x02, 0);
}

/*
 * The DSP's output settings, the same in both directions: speaker out
 * config 8.0 (MASTERCONTROL 0x80 req 4: 5.1 speakers / Direct Mode), X-Bass
 * level 0.5, frequency 80 Hz, off (module 0x96 req 0x19, 0x17, 0x18), the
 * speaker EQ's bypass attenuation 0 (0x8f req 1), no speaker EQ (0x96 req
 * 0x1f), bass redirect off (0x96 req 0x15)
 */
static void sw_dsp_output(struct ae9_blk *b)
{
	blk_scp_set(b, 0x80, 0x04, AE9_F_EIGHT);
	blk_scp_set(b, 0x96, 0x19, AE9_F_HALF);
	blk_scp_set(b, 0x96, 0x17, AE9_F_EIGHTY);
	blk_scp_set(b, 0x96, 0x18, 0);
	blk_scp_set(b, 0x8f, 0x01, 0);
	blk_scp_set(b, 0x96, 0x1f, 0);
	blk_scp_set(b, 0x96, 0x15, 0);
}

/*
 * The switch's first block: the DSP's output muted, then the port map
 * (dac2port: 0xa1 headphones, 0xa4 speakers) in the two-verb form without
 * the ChipIO status read before it: the switch lists carry none (their
 * generator left Windows' reads out), and the driver sends them as they are
 */
static void sw_begin(struct creative_chip *chip, u8 map, const char *what)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	sw_dsp_mute(&b, true);
	blk_send(&b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_EX_ID_SET, AE9_CTL_PARAM_DAC2PORT);
	blk_send(&b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_EX_VALUE_SET, map);
	blk_end(&b, what);
}

/* DAC 48/1d: 0x80 (GPIO2 inverted: the card's RCA out muted) for the headphones, 0x40 */
static void sw_gpio_invert(struct creative_chip *chip, bool hp)
{
	hu_dac(chip, AE9_DAC_FRONT, ES9038_GPIO_INVERT, hp ? 0x80 : 0x40);
}

/* the head unit's headphone jack on or off: set its register 2, value, mask 0x40 (bit 6) */
static void sw_jack(struct creative_chip *chip, bool on)
{
	const struct ae9_hu_frame f = {
		7, { 0xf0, 0x03, 0x03, 0x02, on ? 0x40 : 0x00, 0x40, 0xf7 }
	};

	hu_frame(chip, &f);
}

/* Windows' per-output THD compensation table: 5 bytes, C3's high byte 1 in headphone mode */
static void sw_thd(struct creative_chip *chip, bool hp)
{
	hu_dac(chip, AE9_DAC_FRONT, ES9038_THD_BYPASS, 0x00);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_THD_C2, 0x00);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_THD_C2 + 1, 0x00);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_THD_C3, 0x00);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_THD_C3 + 1, hp ? 0x01 : 0x00);
}

/* the master trim at 0 dB (0x7fffffff, low byte first) */
static void sw_trim_0db(struct creative_chip *chip)
{
	hu_dac(chip, AE9_DAC_FRONT, ES9038_MASTER_TRIM, 0xff);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_MASTER_TRIM + 1, 0xff);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_MASTER_TRIM + 2, 0xff);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_MASTER_TRIM + 3, 0x7f);
}

/* one converter channel at 0 dB, as a block of its own */
static void sw_level(struct creative_chip *chip, u8 nid, bool right, const char *what)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	blk_out_level(&b, nid, right, 0, AE9_AMP_0DB);
	blk_end(&b, what);
}

/* the line out's pins: EAPD off, pin 0x0b off, pin 0x0f off or out with its headphone amp */
static void sw_line_pins(struct ae9_blk *b, bool on)
{
	blk_send(b, 0x0b, AC_VERB_SET_EAPD_BTLENABLE, 0);
	blk_send(b, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	blk_send(b, 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL,
		 on ? AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN : 0);
}

/*
 * Windows' switch to the headphones (XLR capture
 * vm_capture_coldboot_20261101_234608, 23:52:33.690-34.880, after a knob
 * hold), in its order: the DSP muted and the map 0xa1; DAC 48/1d = 0x80, the
 * head unit's headphone jack on, the headphone THD table; converters 0x02,
 * 0x03, 0x04 at 0 dB (Windows' volume setter, CtxHda FUN_000218dc) with the
 * master trim's four bytes and the line out's pins between them, GPIO pin 3
 * (the surround outputs) off, pin 0x10 out; the knob LED steady; the DSP's
 * output settings, the DSP unmuted. The volume registers and the DSP volume
 * Windows also sent are left out: the driver restores the output's own
 * volume after the switch. Each run of verbs is a block (replay_mutex),
 * each DAC command its own list, as the switch list was replayed.
 */
static void sw_to_hp(struct creative_chip *chip)
{
	const char *what = "head unit: switch to headphones";
	struct ae9_blk b;

	sw_begin(chip, 0xa1, what);
	sw_gpio_invert(chip, true);
	sw_jack(chip, true);
	sw_thd(chip, true);

	blk_begin(&b, chip);
	blk_out_level(&b, AE9_NID_DAC, false, 0, AE9_AMP_0DB);
	sw_dsp_vol_update(&b);
	blk_end(&b, what);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_MASTER_TRIM, 0xff);
	sw_level(chip, AE9_NID_DAC, true, what);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_MASTER_TRIM + 1, 0xff);
	sw_level(chip, 0x03, false, what);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_MASTER_TRIM + 2, 0xff);
	sw_level(chip, 0x03, true, what);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_MASTER_TRIM + 3, 0x7f);

	blk_begin(&b, chip);
	sw_line_pins(&b, false);
	blk_out_level(&b, 0x04, false, 0, AE9_AMP_0DB);
	blk_end(&b, what);
	hu_gpio(chip, 3, false);
	blk_begin(&b, chip);
	blk_send(&b, 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN);
	blk_out_level(&b, 0x04, true, 0, AE9_AMP_0DB);
	blk_end(&b, what);
	hu_frame(chip, &hu_blink_off);

	blk_begin(&b, chip);
	sw_dsp_output(&b);
	sw_dsp_mute(&b, false);
	blk_end(&b, what);
}

/*
 * Windows' switch to the speakers (XLR capture, 23:52:56.080-56.700, from
 * Creative's software), in its order: the DSP muted and the map 0xa4; DAC
 * 48/1d = 0x40, the headphone jack off, the speaker THD table, the master
 * trim at 0 dB; the line out's pins (pin 0x0f out), GPIO pin 3 on, pin 0x10
 * out, the DSP's output settings, converters 0x02-0x04 at 0 dB with the DSP
 * volume updates and unmutes Windows interleaved; the knob LED steady.
 *
 * lo_virt: Line Out Virtualization's headphone mode on the card's RCA out
 * (Sound Blaster Command's "Apply Headphones Virtualization to Line Out",
 * CtxHda property 0x75, FUN_00020bdc; capture 4 s11 00:58:27.54): this list
 * with the headphone map 0xa1, pin 0x0f off and no GPIO write, so the two
 * runs of verbs around that write are one block.
 */
static void sw_to_sp(struct creative_chip *chip, bool lo_virt, const char *what)
{
	struct ae9_blk b;

	sw_begin(chip, lo_virt ? 0xa1 : 0xa4, what);
	sw_gpio_invert(chip, false);
	sw_jack(chip, false);
	sw_thd(chip, false);
	sw_trim_0db(chip);

	blk_begin(&b, chip);
	sw_line_pins(&b, !lo_virt);
	if (!lo_virt) {
		blk_end(&b, what);
		hu_gpio(chip, 3, true);
		blk_begin(&b, chip);
	}
	blk_send(&b, 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN);
	sw_dsp_output(&b);
	blk_out_level(&b, AE9_NID_DAC, false, 0, AE9_AMP_0DB);
	sw_dsp_vol_update(&b);
	sw_dsp_mute(&b, false);
	blk_out_level(&b, AE9_NID_DAC, true, 0, AE9_AMP_0DB);
	sw_dsp_vol_update(&b);
	blk_out_level(&b, 0x03, false, 0, AE9_AMP_0DB);
	blk_out_level(&b, 0x03, true, 0, AE9_AMP_0DB);
	sw_dsp_mute(&b, false);
	blk_out_level(&b, 0x04, false, 0, AE9_AMP_0DB);
	blk_out_level(&b, 0x04, true, 0, AE9_AMP_0DB);
	blk_end(&b, what);
	hu_frame(chip, &hu_blink_off);
}

/*
 * Windows' output switch: to the headphones (hp), with Line Out
 * Virtualization to the card's RCA out (lo_virt), or to the speakers. From
 * the heartbeat (core.c creative_ae9_hu_route(), which decides lo_virt);
 * process context, sleeps (replay_mutex per block, ca0113_mutex per DAC
 * command, the head unit's replies).
 *
 * What each step does, measured on the card (2026-11-12, uv33-uv36: a tone
 * in Direct Mode, the headphone jack and then the card's front RCA out
 * looped into a USB interface, one step changed at a time):
 *  - the head-unit frame f0 03 03 02 <40 on / 00 off> 40 f7 alone switches
 *    the headphone jack (off: -30 -> -108 dBFS);
 *  - DAC 48/1d bit 7 (ES9038 GPIO2) mutes the card's RCA out (-41 dB);
 *    bit 6 (GPIO1) changed nothing on either output;
 *  - no measurable change on either output (level, 2nd/3rd harmonic) from
 *    48/0d or 48/19 (THD compensation), codec 1 pins 0x0b/0x0f/0x10,
 *    GPIO pin 3 (0x300: high on speakers) or the ChipIO 0x0d map a1/a4
 *    (map not tested in DSP mode). We still send them all, as Windows.
 * No codec pin senses the head unit's jacks; only the head unit reports
 * the 3.5 mm one.
 */
void creative_ae9_out_switch(struct creative_chip *chip, bool hp, bool lo_virt)
{
	if (hp && !lo_virt)
		sw_to_hp(chip);
	else if (!hp)
		sw_to_sp(chip, false, "head unit: switch to speakers");
	else
		sw_to_sp(chip, true,
			 "head unit: headphone mode on the card's RCA out (Line Out Virtualization)");
}

/*
 * Windows' HeadphoneOutput toggle while on Headphones (capture 4 s11
 * 00:57:53.650, 00:58:37.991, 00:59:43.788, 01:00:11.723): the jack select
 * only (CtxHda FUN_000357fc index 0xb): DAC 48/1d, the map 0xa1 (one
 * block), the headphone jack (off for the card's RCA out), the THD table
 * of the mode it plays in, the master trim at 0 dB. No pins, GPIO, volume
 * or switch list: a whole switch list would also send its mid-list DSP
 * messages (config 8, unmute), the switch-table state of the output-switch
 * fault. lo: to the card's RCA out (Line Out Virtualization on).
 */
void creative_ae9_out_jack(struct creative_chip *chip, bool lo)
{
	const char *what = lo ? "head unit: headphone mode to the card's RCA out" :
				"head unit: headphone mode to the headphone jack";
	struct ae9_blk b;

	sw_gpio_invert(chip, !lo);
	blk_begin(&b, chip);
	blk_send(&b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_EX_ID_SET, AE9_CTL_PARAM_DAC2PORT);
	blk_send(&b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_EX_VALUE_SET, 0xa1);
	blk_end(&b, what);
	sw_jack(chip, !lo);
	sw_thd(chip, !lo);
	sw_trim_0db(chip);
}

/* ------------------------------------------------------------------ */
/* The head unit and the DACs: Windows' shutdown			  */
/* ------------------------------------------------------------------ */

/*
 * The front DAC's other registers (ES9038Q2M) and the rear DAC, an ESS
 * SABRE9006A (8 channels: 2/3 the card's Rear jack, 4/5 its C/Sub jack,
 * uv53), group 0x49 (ESS datasheets; docs/DECODED-SEQUENCES.md)
 */
#define ES9038_AUTOMUTE_TIME	0x04	/* 0 = automute off */
#define ES9038_DEEMPH_RAMP	0x06	/* de-emphasis, DoP, volume ramp rate */
#define ES9038_FILTER_MUTE	0x07	/* bits 7:5 filter (dsp.c keeps the user's), bit 0 mute */
#define ES9038_GPIO_CONFIG	0x08	/* 0xff: GPIO1/2 forced-high outputs (48/1d inverts them) */
#define ES9038_DPLL		0x0c	/* DPLL bandwidth */
#define ES9038_SOFT_START	0x0e
#define ES9038_VOLUME_L		0x0f	/* 0.5 dB steps, 0xff = -127.5 dB */
#define ES9038_VOLUME_R		0x10
#define AE9_DAC_REAR		0x49
#define SABRE9006_VOLUME(ch)	(ch)	/* channels 0-7, 0.5 dB steps */
#define SABRE9006_MODE_CTL	0x0a	/* bit 0: mute all (0x07 muted, 0x06 not) */
#define SABRE9006_VOLUME_CONFIG	0x10
#define SABRE9006_MASTER_TRIM	0x18	/* 0x18-0x1b, low byte first */

/* the head unit's stop frame (acknowledged f0 52 01 00 f7) */
static const struct ae9_hu_frame hu_stop = { 6, { 0xf0, 0x52, 0x02, 0xcc, 0xdd, 0xf7 } };

/* both DACs muted: the front one (48/07 bit 0, with the filter bits) and the rear one */
static void pd_dacs_mute(struct creative_chip *chip)
{
	hu_dac(chip, AE9_DAC_FRONT, ES9038_FILTER_MUTE, 0x01);
	hu_dac(chip, AE9_DAC_REAR, SABRE9006_MODE_CTL, 0x07);
}

/*
 * The power-down's start: the head unit's stop frame (it keeps answering but
 * drops its set-up: the 0x2000 marker back to 0xaaaaaaaa, status 0xd5 ->
 * 0x85, uv55), then ASI 0 (one block)
 */
static void pd_stop(struct creative_chip *chip)
{
	struct ae9_blk b;

	hu_frame(chip, &hu_stop);
	blk_begin(&b, chip);
	blk_ctl_param(&b, CONTROL_PARAM_ASI, 0);
	blk_end(&b, "shutdown: power-down");
}

/*
 * Windows' stream 0x0c stop, sent blind: the stream selected (the two-verb
 * form without the status read before it, as the list had it: its generator
 * left Windows' reads out, the stream's XRAM reads included) and its control
 * 0; then 50 ms inside the block: the 8051 tears the stream's routes down
 * after it answers (Windows: 48 ms, capture 6 seq 9954 -> 9956)
 */
static void pd_stream_0c_off(struct creative_chip *chip)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	blk_send(&b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_EX_ID_SET, CONTROL_PARAM_STREAM_ID);
	blk_send(&b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_EX_VALUE_SET, 0x0c);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_CONTROL, 0);
	msleep(50);
	blk_end(&b, "shutdown: stream 0x0c off");
}

/*
 * The power-down proper: GPIO pin 1 off; the PMU bit request 0x83 (one
 * block); both DACs muted, twice; GPIO pins 4 (the LED), 5 (the head unit's
 * power) and 3 (the surround outputs) off; the front DAC's master trim 0,
 * its volume -127.5 dB both, muted, soft start 0x0a; the rear DAC's four
 * wired channels at -127.5 dB (Rear L, C/Sub L, Rear R, C/Sub R), muted;
 * GPIO pin 0 off (the DACs' reset/enable: low, they stop answering)
 */
static void pd_power_down(struct creative_chip *chip)
{
	struct ae9_blk b;

	hu_gpio(chip, 1, false);
	blk_begin(&b, chip);
	blk_send(&b, WIDGET_CHIP_CTRL, AE9_VERB_CHIPIO_PMU_BIT, 0x83);
	blk_end(&b, "shutdown: power-down");
	pd_dacs_mute(chip);
	pd_dacs_mute(chip);
	hu_gpio(chip, 4, false);
	hu_gpio(chip, 5, false);
	hu_gpio(chip, 3, false);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_MASTER_TRIM, 0x00);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_MASTER_TRIM + 1, 0x00);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_MASTER_TRIM + 2, 0x00);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_MASTER_TRIM + 3, 0x00);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_VOLUME_L, 0xff);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_VOLUME_R, 0xff);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_FILTER_MUTE, 0x01);
	hu_dac(chip, AE9_DAC_FRONT, ES9038_SOFT_START, 0x0a);
	hu_dac(chip, AE9_DAC_REAR, SABRE9006_VOLUME(2), 0xff);
	hu_dac(chip, AE9_DAC_REAR, SABRE9006_VOLUME(4), 0xff);
	hu_dac(chip, AE9_DAC_REAR, SABRE9006_VOLUME(3), 0xff);
	hu_dac(chip, AE9_DAC_REAR, SABRE9006_VOLUME(5), 0xff);
	hu_dac(chip, AE9_DAC_REAR, SABRE9006_MODE_CTL, 0x07);
	hu_gpio(chip, 0, false);
}

/*
 * The codecs to D3 (XLR capture, 23:53:44.330-44.407): codec 2's function
 * group (d3), codec 1's mic pins 0x11/0x12 off, codec 2's S/PDIF out
 * (converter 0x05: its keep-alive off, D3), pin 0x13 off, codec 2's pins
 * 0x0c and 0x09 to D3, the line out's EAPD off, codec 2's 0x0e to D3, pin
 * 0x0b off, codec 2's 0x08 and 0x11 to D3, pins 0x0f and 0x10 off, codec
 * 1's function group word (0x793/0x794 = 0x5300, Windows' value) - one block;
 * both DACs muted; codec 1's function group (d3) - one block. d3: D3cold
 * at power-off, else D3hot (so the next start can wake the card; since
 * 0.12.4).
 */
static void pd_codecs_d3(struct creative_chip *chip, bool cold)
{
	const char *what = cold ? "shutdown: codecs to D3cold" : "shutdown: codecs to D3hot";
	u8 d3 = cold ? AE9_PWRST_D3COLD : AC_PWRST_D3;
	struct ae9_blk b;

	blk_begin(&b, chip);
	blk_send_c2(&b, AE9_NID_AFG, AC_VERB_SET_POWER_STATE, d3);
	blk_send(&b, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	blk_send(&b, 0x12, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	blk_send_c2(&b, 0x05, AC_VERB_SET_DIGI_CONVERT_3, 0);
	blk_send_c2(&b, 0x05, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	blk_send(&b, 0x13, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	blk_send_c2(&b, 0x0c, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	blk_send_c2(&b, 0x09, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	blk_send(&b, 0x0b, AC_VERB_SET_EAPD_BTLENABLE, 0);
	blk_send_c2(&b, 0x0e, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	blk_send(&b, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	blk_send_c2(&b, 0x08, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	blk_send_c2(&b, 0x11, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	blk_send(&b, 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	blk_send(&b, 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	blk_send(&b, AE9_NID_AFG, AE9_VERB_SET_NODE_WORD_LO, 0x00);
	blk_send(&b, AE9_NID_AFG, AE9_VERB_SET_NODE_WORD_HI, 0x53);
	blk_end(&b, what);
	pd_dacs_mute(chip);
	blk_begin(&b, chip);
	blk_send(&b, AE9_NID_AFG, AC_VERB_SET_POWER_STATE, d3);
	blk_end(&b, what);
}

/*
 * Windows' shutdown (both cold-boot captures end with it; the XLR capture's,
 * 23:53:33.350-33.720 and 23:53:44.330-44.407), with our stream drain in
 * place of its stream stops:
 * - the head unit's stop frame and ASI 0;
 * - every stream reference we or our binds can hold released
 *   (creative_ae9_streams_drain(), core.c; Windows reads and drains 0x18 and
 *   0x0c at every device stop, FUN_000343d8); when the 8051 doesn't answer
 *   the counts, Windows' blind stream 0x0c stop (the old way);
 * - the power-down (GPIO, PMU, the DACs muted and at -127.5 dB, the head
 *   unit's power off);
 * - >= 1.08 s, as Windows between the two halves;
 * - the codecs to D3 (cold: D3cold, power-off; else D3hot).
 * From creative_ae9_win_shutdown() (core.c: unload, reboot/power-off, a
 * probe failure, bring-up and resume retries), with the CORB running;
 * process context, sleeps.
 */
void creative_ae9_power_down(struct creative_chip *chip, bool cold)
{
	pd_stop(chip);
	if (creative_ae9_streams_drain(chip, "shutdown") < 0)
		pd_stream_0c_off(chip);
	pd_power_down(chip);
	msleep(1100);	/* Windows: >= 1.08 s between the two halves */
	pd_codecs_d3(chip, cold);
}

/* ------------------------------------------------------------------ */
/* The head unit and the DACs: the set-up after the DSP's start	  */
/* ------------------------------------------------------------------ */

/*
 * Windows' BAR2 activity after the DSP's boot handshake and before its first
 * playback (cold-boot capture vm_capture_coldboot_20261101_160959, CORB
 * writes #2480-2842): 333 steps, each a DAC command, a BAR2 write or a
 * head-unit frame. The driver checks before each step that the card is still
 * on the bus (an ASMedia-bridge card went quiet here, bugzilla 219552 round
 * 3c) and names the step it stopped at; struct ae9_post counts them (and
 * checks the count at the end against AE9_POST_STEPS, the number its log
 * line prints).
 */
#define AE9_POST_STEPS		333

/* BAR2 registers of the set-up (docs/DECODED-SEQUENCES.md) */
#define AE9_BAR2_PIN_FN		0x100	/* bits 3:0: pins 4-7 alternate function (1) or GPIO (0) */
#define AE9_BAR2_CA0113_CMD	0x20c	/* the CA0113 command register (dsp.c ca0113_cmd()) */
#define AE9_BAR2_GPIO_OE	0x304	/* GPIO output enable, a bit per pin */
#define AE9_BAR2_I2C_CON	0x800	/* the DACs' I2C master (Synopsys DW_apb_i2c): IC_CON */
#define AE9_BAR2_I2C_TAR	0x804	/* IC_TAR: the DAC's address = the CA0113 group */
#define AE9_BAR2_I2C_ENABLE	0x86c	/* IC_ENABLE */
#define AE9_BAR2_UART_DATA	0xc00	/* the head unit's UART (DW_apb_uart): data; divisor low */
#define AE9_BAR2_UART_IER	0xc04	/* interrupt enable; divisor high */
#define AE9_BAR2_UART_LSR	0xc14	/* line status: bit 0 = a byte received */
#define AE9_BAR2_UART_LCR	0xc0c	/* line control: bit 7 = divisor latch */

/*
 * the head unit's first frame: up to ~1 s of tries, 100 ms apart (a card
 * without a head unit pays this once at load)
 */
#define AE9_HU_FIRST_TRIES	10

struct ae9_post {
	struct creative_chip *chip;
	unsigned int step;		/* steps sent */
	char type;			/* the last one: 'C' DAC command, 'W' write, 'S' frame */
	u16 off;			/* its DAC group or BAR2 offset (0 for a frame) */
	u32 val;			/* its register << 8 | value, or the value written */
	bool gone;
	unsigned int frames, noreply, cmds, writes;
};

/* the next step may go: false once the card is gone (said once, naming the step) */
static bool post_step(struct ae9_post *p, char type, u16 off, u32 val)
{
	if (p->gone)
		return false;
	if (creative_ae9_gone(p->chip, "post-boot head-unit replay")) {
		p->gone = true;
		if (!p->step) {	/* "last step sent": the first one's own, as before */
			p->type = type;
			p->off = off;
			p->val = val;
		}
		dev_err(p->chip->dev,
			"post_windows: card gone before step %u of %u; last step sent: %c 0x%x = 0x%x\n",
			p->step + 1, AE9_POST_STEPS, p->type, p->off, p->val);
		return false;
	}
	p->step++;
	p->type = type;
	p->off = off;
	p->val = val;
	return true;
}

/* a DAC command step */
static void post_dac(struct ae9_post *p, u8 group, u8 reg, u8 val)
{
	if (!post_step(p, 'C', group, reg << 8 | val))
		return;
	hu_dac(p->chip, group, reg, val);
	p->cmds++;
}

/* BAR2 write steps: 8, 16, 32 bits */
static void post_write8(struct ae9_post *p, u16 off, u8 val)
{
	if (!post_step(p, 'W', off, val))
		return;
	creative_ext_writeb(p->chip, off, val);
	p->writes++;
}

static void post_write32(struct ae9_post *p, u16 off, u32 val)
{
	if (!post_step(p, 'W', off, val))
		return;
	creative_ext_writel(p->chip, off, val);
	p->writes++;
}

/* a GPIO step (16 bits, as hu_gpio()) */
static void post_gpio(struct ae9_post *p, u8 pin, bool on)
{
	u16 val = (on ? 0x100 : 0) | pin;

	if (!post_step(p, 'W', AE9_BAR2_GPIO, val))
		return;
	creative_ext_writew(p->chip, AE9_BAR2_GPIO, val);
	p->writes++;
}

/*
 * Bytes waiting in the head-unit UART before its first frame (with the
 * board-D0 step the head unit is powered seconds earlier, as in Windows):
 * read them out (0xc14 bit 0 = a byte received, 0xc00 the byte), so that
 * the first reply is the head unit's answer. Returns the count.
 */
static unsigned int hu_drain(struct creative_chip *chip)
{
	unsigned int n = 0;

	while (n < 64 && (creative_ext_readl(chip, AE9_BAR2_UART_LSR) & 0x01)) {
		creative_ext_readl(chip, AE9_BAR2_UART_DATA);
		n++;
	}
	return n;
}

/*
 * A head-unit frame step. Before the first: the UART read out. The first
 * frame (the head unit's identify) waits until the head unit answers, before
 * its set-up frames: without the board-D0 step the first 10 frames, the
 * attach frame 03 03 05 03 03 among them, went unanswered on one card (0.12.4
 * on 2026-11-10, a 0.12.5 cold boot on 2026-11-12): register 5 stayed 00 and
 * the XLR mic silent until the attach frame was resent. With the step the
 * head unit answered at the first try; this is the fallback. A frame without
 * a reply is remembered (its number and command, the first 8: debugfs
 * "state"). Returns whether the step went.
 */
static bool post_frame(struct ae9_post *p, const struct ae9_hu_frame *f)
{
	struct creative_chip *chip = p->chip;
	int n;

	if (!post_step(p, 'S', 0, 0))
		return false;
	if (!p->frames) {
		unsigned int stale = hu_drain(chip);

		if (stale)
			dev_info(chip->dev, "post_windows: %u byte%s waiting from the head unit, read out\n",
				 stale, stale == 1 ? "" : "s");
	}
	n = hu_frame(chip, f);
	if (!p->frames && n < 0) {
		unsigned int tries = 1;

		while (n < 0 && tries < AE9_HU_FIRST_TRIES) {
			msleep(100);
			n = hu_frame(chip, f);
			tries++;
		}
		dev_info(chip->dev, "post_windows: head unit %s its first frame after %u tries\n",
			 n < 0 ? "did not answer" : "answered", tries);
	}
	if (n < 0) {
		if (p->noreply < ARRAY_SIZE(chip->hu_noreply_idx)) {
			chip->hu_noreply_idx[p->noreply] = p->frames + 1;
			chip->hu_noreply_head[p->noreply] = f->d[1] << 8 | f->d[2];
		}
		p->noreply++;
	}
	p->frames++;
	return true;
}

/*
 * The head unit's frames of Windows' set-up and poll (meanings: its UART
 * protocol, decoded in docs/DECODED-SEQUENCES.md; Windows' names from CtxHda)
 */
static const struct ae9_hu_frame hu_identify = { 4, { 0xf0, 0x81, 0x00, 0xf7 } };

/* memory unlock, two parts ("Acm1", "1mcA"), and the relock */
static const struct ae9_hu_frame hu_unlock1 = {
	8, { 0xf0, 0x54, 0x04, 'A', 'c', 'm', '1', 0xf7 }
};

static const struct ae9_hu_frame hu_unlock2 = {
	8, { 0xf0, 0x54, 0x04, '1', 'm', 'c', 'A', 0xf7 }
};

static const struct ae9_hu_frame hu_relock = {
	8, { 0xf0, 0x54, 0x04, 0x11, 0x11, 0x11, 0x11, 0xf7 }
};

/* the attach marker at 0x2000: read 4 bytes; write 0xdeadc0de (attached) */
static const struct ae9_hu_frame hu_marker_read = {
	7, { 0xf0, 0xd5, 0x03, 0x00, 0x20, 0x04, 0xf7 }
};

static const struct ae9_hu_frame hu_marker_write = {
	11, { 0xf0, 0x55, 0x07, 0x00, 0x20, 0x04, 0xde, 0xc0, 0xad, 0xde, 0xf7 }
};

/* attach: register 5 bits 0-1 set (Windows sends it once, when it finds the head unit) */
static const struct ae9_hu_frame hu_attach = {
	7, { 0xf0, 0x03, 0x03, 0x05, 0x03, 0x03, 0xf7 }
};

/*
 * the knob's long press 3000 ms (AcmEncoderLongPressMs), its filter window
 * 100..500 (AcmEncoderFilterWinMin/Max), the SBX button's long press 5000 ms
 * (AcmSBXLongPressMs)
 */
static const struct ae9_hu_frame hu_knob_long = {
	7, { 0xf0, 0x32, 0x03, 0x02, 0xb8, 0x0b, 0xf7 }
};

static const struct ae9_hu_frame hu_knob_filter = {
	8, { 0xf0, 0x43, 0x04, 0x64, 0x00, 0xf4, 0x01, 0xf7 }
};

static const struct ae9_hu_frame hu_sbx_long = {
	7, { 0xf0, 0x32, 0x03, 0x01, 0x88, 0x13, 0xf7 }
};

/* phantom power off; the SBX light off, the knob light on; the headphone jack off */
static const struct ae9_hu_frame hu_phantom_off = {
	7, { 0xf0, 0x05, 0x03, 0x02, 0x01, 0x00, 0xf7 }
};

static const struct ae9_hu_frame hu_sbx_light_off = { 6, { 0xf0, 0x22, 0x02, 0x01, 0x00, 0xf7 } };
static const struct ae9_hu_frame hu_knob_light_on = { 6, { 0xf0, 0x22, 0x02, 0x02, 0x01, 0xf7 } };
static const struct ae9_hu_frame hu_jack_off = { 7, { 0xf0, 0x03, 0x03, 0x02, 0x00, 0x40, 0xf7 } };

/* the display text: "AE-9", or 8 spaces (blank) */
static const struct ae9_hu_frame hu_label_ae9 = {
	13, { 0xf0, 0x11, 0x09, 'A', 'E', '-', '9', 0, 0, 0, 0, 0, 0xf7 }
};

static const struct ae9_hu_frame hu_label_blank = {
	13, { 0xf0, 0x11, 0x09, ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 0x00, 0xf7 }
};

/* the poll's queries: knob, status 2 and 7, phantom power, press reports 1-3 */
static const struct ae9_hu_frame hu_knob_query = { 4, { 0xf0, 0xc2, 0x00, 0xf7 } };
static const struct ae9_hu_frame hu_status2 = { 5, { 0xf0, 0x83, 0x01, 0x02, 0xf7 } };
static const struct ae9_hu_frame hu_status7 = { 5, { 0xf0, 0x83, 0x01, 0x07, 0xf7 } };
static const struct ae9_hu_frame hu_status85 = { 5, { 0xf0, 0x85, 0x01, 0x02, 0xf7 } };
static const struct ae9_hu_frame hu_press1 = { 5, { 0xf0, 0xb1, 0x01, 0x01, 0xf7 } };
static const struct ae9_hu_frame hu_press2 = { 5, { 0xf0, 0xb1, 0x01, 0x02, 0xf7 } };
static const struct ae9_hu_frame hu_press3 = { 5, { 0xf0, 0xb1, 0x01, 0x03, 0xf7 } };

/*
 * Windows' head-unit poll (the cold capture: one every ~500 ms): identify;
 * the attach marker read between the unlock and the relock; the knob,
 * status 2, 2, 7, phantom power, status 2, 7; the press reports of items
 * 1-3. The set-up after the DSP's start sends it 15 times (post_poll()),
 * the heartbeat every 500 ms (core.c creative_ae9_hb_windows(), which reads
 * the replies: knob, buttons, plugs, the marker). Windows always got the
 * same replies with the knob untouched, so a reply that changes is an event.
 */
const struct ae9_hu_frame *const creative_ae9_hu_poll[] = {
	&hu_identify,
	&hu_unlock1, &hu_unlock2, &hu_marker_read, &hu_relock,
	&hu_knob_query, &hu_status2, &hu_status2, &hu_status7, &hu_status85,
	&hu_status2, &hu_status7,
	&hu_press1, &hu_press2, &hu_press3,
};

/* every user loops to the count: a frame left out would be a NULL one */
static_assert(ARRAY_SIZE(creative_ae9_hu_poll) == AE9_HU_POLL_FRAMES);

/* the poll's frames 1-4: the marker read with its unlock and relock */
#define AE9_HU_POLL_MARKER	1
#define AE9_HU_POLL_MARKER_N	4

/*
 * Steps 1-37 (#2480-): the front DAC's speaker THD table, the master trim
 * at 0 dB, DAC 48/1d = 0x80; GPIO pin 3 on (twice), every GPIO pin an
 * output (four times), pins 4-7 to GPIO with the output enable again (four
 * times); the DACs' I2C master (Synopsys DW_apb_i2c) off, master mode,
 * on, master mode, target 0x48, the CA0113 command register idle-set;
 * GPIO pin 5 on (the head unit's power); both DACs muted; pin 2 off, DAC
 * 48/1d = 0x40, pin 3 on, pin 1 off
 */
static void post_gpio_i2c(struct ae9_post *p)
{
	unsigned int i;

	post_dac(p, AE9_DAC_FRONT, ES9038_THD_BYPASS, 0x00);
	post_dac(p, AE9_DAC_FRONT, ES9038_THD_C2, 0x00);
	post_dac(p, AE9_DAC_FRONT, ES9038_THD_C2 + 1, 0x00);
	post_dac(p, AE9_DAC_FRONT, ES9038_THD_C3, 0x00);
	post_dac(p, AE9_DAC_FRONT, ES9038_THD_C3 + 1, 0x00);
	post_dac(p, AE9_DAC_FRONT, ES9038_MASTER_TRIM, 0xff);
	post_dac(p, AE9_DAC_FRONT, ES9038_MASTER_TRIM + 1, 0xff);
	post_dac(p, AE9_DAC_FRONT, ES9038_MASTER_TRIM + 2, 0xff);
	post_dac(p, AE9_DAC_FRONT, ES9038_MASTER_TRIM + 3, 0x7f);
	post_dac(p, AE9_DAC_FRONT, ES9038_GPIO_INVERT, 0x80);
	post_gpio(p, 3, true);
	post_gpio(p, 3, true);
	for (i = 0; i < 4; i++)
		post_write8(p, AE9_BAR2_GPIO_OE, 0xff);
	for (i = 0; i < 4; i++) {
		post_write8(p, AE9_BAR2_PIN_FN, 0x00);
		post_write8(p, AE9_BAR2_GPIO_OE, 0xff);
	}
	post_write32(p, AE9_BAR2_I2C_ENABLE, 0);
	post_write32(p, AE9_BAR2_I2C_CON, 0x6b);
	post_write32(p, AE9_BAR2_I2C_ENABLE, 1);
	post_write32(p, AE9_BAR2_I2C_CON, 0x6b);
	post_write32(p, AE9_BAR2_I2C_TAR, AE9_DAC_FRONT);
	post_write32(p, AE9_BAR2_CA0113_CMD, 0x00800005);
	post_gpio(p, 5, true);
	post_dac(p, AE9_DAC_FRONT, ES9038_FILTER_MUTE, 0x81);
	post_dac(p, AE9_DAC_REAR, SABRE9006_MODE_CTL, 0x07);
	post_gpio(p, 2, false);
	post_dac(p, AE9_DAC_FRONT, ES9038_GPIO_INVERT, 0x40);
	post_gpio(p, 3, true);
	post_gpio(p, 1, false);
}

/*
 * Steps 38-65: the DACs' set-up. The front DAC (ES9038Q2M): 32-bit I2S
 * input, sync 0x02, S/PDIF input DATA2, automute off, de-emphasis/volume
 * ramp 0x40, both GPIOs forced-high outputs, 48/1d = 0x40, sync 0x06 (init
 * lock speed), DPLL bandwidth 0x5f, master trim 0 dB, soft start 0x8a,
 * muted with the start-up filter (0x81), volume 0 dB both, sync 0x02. The
 * rear DAC (SABRE9006A): master trim ff ff 00 70 (-1.16 dB), channels 0, 1,
 * 6, 7 at -127.5 dB (not wired to a jack, uv53), volume config 0x7f, muted.
 */
static void post_dac_setup(struct ae9_post *p)
{
	post_dac(p, AE9_DAC_FRONT, ES9038_INPUT_SELECT, 0xc0);
	post_dac(p, AE9_DAC_FRONT, ES9038_SYNC_CONFIG, 0x02);
	post_dac(p, AE9_DAC_FRONT, ES9038_SPDIF_SELECT, 0x20);
	post_dac(p, AE9_DAC_FRONT, ES9038_AUTOMUTE_TIME, 0x00);
	post_dac(p, AE9_DAC_FRONT, ES9038_DEEMPH_RAMP, 0x40);
	post_dac(p, AE9_DAC_FRONT, ES9038_GPIO_CONFIG, 0xff);
	post_dac(p, AE9_DAC_FRONT, ES9038_GPIO_INVERT, 0x40);
	post_dac(p, AE9_DAC_FRONT, ES9038_SYNC_CONFIG, 0x06);
	post_dac(p, AE9_DAC_FRONT, ES9038_DPLL, 0x5f);
	post_dac(p, AE9_DAC_FRONT, ES9038_MASTER_TRIM, 0xff);
	post_dac(p, AE9_DAC_FRONT, ES9038_MASTER_TRIM + 1, 0xff);
	post_dac(p, AE9_DAC_FRONT, ES9038_MASTER_TRIM + 2, 0xff);
	post_dac(p, AE9_DAC_FRONT, ES9038_MASTER_TRIM + 3, 0x7f);
	post_dac(p, AE9_DAC_FRONT, ES9038_SOFT_START, 0x8a);
	post_dac(p, AE9_DAC_FRONT, ES9038_FILTER_MUTE, 0x81);
	post_dac(p, AE9_DAC_FRONT, ES9038_VOLUME_L, 0x00);
	post_dac(p, AE9_DAC_FRONT, ES9038_VOLUME_R, 0x00);
	post_dac(p, AE9_DAC_FRONT, ES9038_SYNC_CONFIG, 0x02);
	post_dac(p, AE9_DAC_REAR, SABRE9006_MASTER_TRIM, 0xff);
	post_dac(p, AE9_DAC_REAR, SABRE9006_MASTER_TRIM + 1, 0xff);
	post_dac(p, AE9_DAC_REAR, SABRE9006_MASTER_TRIM + 2, 0x00);
	post_dac(p, AE9_DAC_REAR, SABRE9006_MASTER_TRIM + 3, 0x70);
	post_dac(p, AE9_DAC_REAR, SABRE9006_VOLUME(0), 0xff);
	post_dac(p, AE9_DAC_REAR, SABRE9006_VOLUME(1), 0xff);
	post_dac(p, AE9_DAC_REAR, SABRE9006_VOLUME(6), 0xff);
	post_dac(p, AE9_DAC_REAR, SABRE9006_VOLUME(7), 0xff);
	post_dac(p, AE9_DAC_REAR, SABRE9006_VOLUME_CONFIG, 0x7f);
	post_dac(p, AE9_DAC_REAR, SABRE9006_MODE_CTL, 0x07);
}

/*
 * Steps 66-70: the head unit's UART (Synopsys DW_apb_uart, 16550 layout):
 * the divisor latch on, divisor 0x000d (24 MHz / (16 x 13) = 115384 baud),
 * 8 data bits, no parity, 1 stop; GPIO pin 5 on
 */
static void post_uart(struct ae9_post *p)
{
	post_write32(p, AE9_BAR2_UART_LCR, 0x83);
	post_write32(p, AE9_BAR2_UART_DATA, 0x0d);
	post_write32(p, AE9_BAR2_UART_IER, 0x00);
	post_write32(p, AE9_BAR2_UART_LCR, 0x03);
	post_gpio(p, 5, true);
}

/*
 * Steps 71-91: Windows' head-unit set-up when it finds the head unit
 * (FUN_0003fe68 pushes it when the 0x2000 marker reads 0xaaaaaaaa; the
 * replay always sends it): identify; unlock, the marker read, relock; unlock,
 * the marker set to 0xdeadc0de, relock; attach; the knob's and SBX button's
 * long-press times and the knob filter; phantom power off; the SBX light
 * off, the knob light on; the headphone jack off (speakers); "AE-9" (blank
 * when the display was switched off before a sleep; else held 4.6 s, as
 * Windows); the knob LED steady; status queries 2 and 7
 */
static void post_hu_setup(struct ae9_post *p)
{
	bool display = p->chip->hu_display;

	post_frame(p, &hu_identify);
	post_frame(p, &hu_unlock1);
	post_frame(p, &hu_unlock2);
	post_frame(p, &hu_marker_read);
	post_frame(p, &hu_relock);
	post_frame(p, &hu_unlock1);
	post_frame(p, &hu_unlock2);
	post_frame(p, &hu_marker_write);
	post_frame(p, &hu_relock);
	post_frame(p, &hu_attach);
	post_frame(p, &hu_knob_long);
	post_frame(p, &hu_knob_filter);
	post_frame(p, &hu_sbx_long);
	post_frame(p, &hu_phantom_off);
	post_frame(p, &hu_sbx_light_off);
	post_frame(p, &hu_knob_light_on);
	post_frame(p, &hu_jack_off);
	if (post_frame(p, display ? &hu_label_ae9 : &hu_label_blank) && display)
		creative_ae9_hu_ae9_hold(p->chip);
	post_frame(p, &hu_blink_off);
	post_frame(p, &hu_status2);
	post_frame(p, &hu_status7);
}

/*
 * Steps 92-101: GPIO pin 4 on (the card's LED); both DACs unmuted (the
 * filter bits are dsp.c's: the user's "DAC Filter"), the six channels'
 * volume at -36 dB (0x48: the front pair, the rear DAC's 4/5 = C/Sub and
 * 2/3 = Rear, uv53); the knob LED steady
 */
static void post_unmute(struct ae9_post *p)
{
	post_gpio(p, 4, true);
	post_dac(p, AE9_DAC_FRONT, ES9038_FILTER_MUTE, 0x80);
	post_dac(p, AE9_DAC_REAR, SABRE9006_MODE_CTL, 0x06);
	post_dac(p, AE9_DAC_FRONT, ES9038_VOLUME_L, 0x48);
	post_dac(p, AE9_DAC_FRONT, ES9038_VOLUME_R, 0x48);
	post_dac(p, AE9_DAC_REAR, SABRE9006_VOLUME(4), 0x48);
	post_dac(p, AE9_DAC_REAR, SABRE9006_VOLUME(5), 0x48);
	post_dac(p, AE9_DAC_REAR, SABRE9006_VOLUME(2), 0x48);
	post_dac(p, AE9_DAC_REAR, SABRE9006_VOLUME(3), 0x48);
	post_frame(p, &hu_blink_off);
}

/*
 * One of Windows' head-unit polls (creative_ae9_hu_poll[], as the heartbeat
 * sends it): GPIO pin 5 on, then its frames; without marker, the marker read
 * and its unlock and relock left out (Windows skipped them in 2 of the 15
 * polls of this window). Windows ended its polls 9-15 with the output's name
 * ("-SP-", once its 4.6 s "AE-9" hold was over): those 7 frames are left out
 * (the driver's table already left them out; the heartbeat shows the name
 * after its own hold; sent back to back here they hid "AE-9" at once).
 */
static void post_poll(struct ae9_post *p, bool marker)
{
	unsigned int i;

	post_gpio(p, 5, true);
	for (i = 0; i < AE9_HU_POLL_FRAMES; i++) {
		if (!marker && i >= AE9_HU_POLL_MARKER &&
		    i < AE9_HU_POLL_MARKER + AE9_HU_POLL_MARKER_N)
			continue;
		post_frame(p, creative_ae9_hu_poll[i]);
	}
}

/*
 * The head unit's set-up sent again (core.c creative_ae9_hu_reattach(): the
 * attach marker read 0xaaaaaaaa in a poll, the head unit powered up again or
 * got the stop frame, uv55), as Windows sends it then (FUN_0003fe68, only
 * when the marker reads 0xaaaaaaaa), from post_hu_setup()'s marker write on,
 * built from the current state (uv56: the start-up's "speakers" jack frame
 * left headphones without their amp): unlock, the marker set, relock;
 * attach; the long-press times and the knob filter; phantom power off; the
 * SBX light off, the knob light as it is (light); the headphone jack, on
 * only for headphones without Line Out Virtualization (jack_on); "AE-9" with
 * its 4.6 s hold only with the display on (Windows). Returns the number of
 * frames without a reply.
 */
int creative_ae9_hu_setup_again(struct creative_chip *chip, bool light, bool jack_on,
				bool display)
{
	const struct ae9_hu_frame knob_light = {
		6, { 0xf0, 0x22, 0x02, 0x02, light ? 1 : 0, 0xf7 }
	};
	const struct ae9_hu_frame jack = {
		7, { 0xf0, 0x03, 0x03, 0x02, jack_on ? 0x40 : 0x00, 0x40, 0xf7 }
	};
	int lost = 0;

	lost += hu_frame(chip, &hu_unlock1) < 0;
	lost += hu_frame(chip, &hu_unlock2) < 0;
	lost += hu_frame(chip, &hu_marker_write) < 0;
	lost += hu_frame(chip, &hu_relock) < 0;
	lost += hu_frame(chip, &hu_attach) < 0;
	lost += hu_frame(chip, &hu_knob_long) < 0;
	lost += hu_frame(chip, &hu_knob_filter) < 0;
	lost += hu_frame(chip, &hu_sbx_long) < 0;
	lost += hu_frame(chip, &hu_phantom_off) < 0;
	lost += hu_frame(chip, &hu_sbx_light_off) < 0;
	lost += hu_frame(chip, &knob_light) < 0;
	lost += hu_frame(chip, &jack) < 0;
	if (display) {
		lost += hu_frame(chip, &hu_label_ae9) < 0;
		creative_ae9_hu_ae9_hold(chip);
	}
	return lost;
}

/*
 * Windows' head unit and DAC set-up after the DSP's boot handshake, as one
 * sequence (the post-boot "replay"; Windows' post-DSP block): the GPIO and
 * the DACs' I2C master, the DACs' set-up, the head unit's UART, its set-up,
 * the DACs unmuted, then the 15 polls Windows made before its first
 * playback. Then the summary line; the frame count and the unanswered
 * frames stay for debugfs "state". From creative_ae9_post_fw() (core.c) at
 * every bring-up that loaded the firmware; process context, sleeps (the DAC
 * commands, the head unit's replies, the first frame's retries).
 */
void creative_ae9_post_dsp_setup(struct creative_chip *chip)
{
	struct ae9_post p = { .chip = chip };
	unsigned int i;

	post_gpio_i2c(&p);
	post_dac_setup(&p);
	post_uart(&p);
	post_hu_setup(&p);
	post_unmute(&p);
	for (i = 0; i < 15; i++)
		post_poll(&p, i != 9 && i != 12);
	if (p.gone)
		return;
	WARN_ON_ONCE(p.step != AE9_POST_STEPS);
	chip->hu_boot_frames = p.frames;
	chip->hu_noreply_n = p.noreply;
	dev_info(chip->dev,
		 "post_windows: %u SPI frames (%u without reply), %u CA0113 commands, %u writes\n",
		 p.frames, p.noreply, p.cmds, p.writes);
}

/* ------------------------------------------------------------------ */
/* The card's BAR2 at start-up: Windows' init				  */
/* ------------------------------------------------------------------ */

/* more BAR2 registers (docs/DECODED-SEQUENCES.md, "BAR2 init") */
#define AE9_BAR2_ID		0x004	/* the card's ID register (Windows reads 0x7172000a) */
/*
 * bit 9: codec 2 (SDI2) off, must stay 0 on the AE-9 (set, the next link
 * reset drops codec 2, uv62); bits 23:16: Creative's MX_BSIZE byte 0x80 | 8
 * (registry HW_CTL_MX_BSIZE); bit 7: the UART's enable; reads 0x880480 on
 * every AE-9 seen, before and after
 */
#define AE9_BAR2_CTL		0x01c
#define AE9_BAR2_I2S		0x400	/* the I2S block (Synopsys DW_apb_i2s v1.04a, uv42) */
#define DW_I2S_IER		0x00	/* its enable */
#define DW_I2S_ITER		0x08	/* transmitter block enable */
#define DW_I2S_CER		0x0c	/* clock enable */
#define DW_I2S_CCR		0x10	/* clock configuration */
#define DW_I2S_TER(ch)		(0x2c + 0x40 * (ch))	/* channel ch's transmit enable */
#define DW_I2S_IMR(ch)		(0x3c + 0x40 * (ch))	/* channel ch's interrupt mask */
#define AE9_BAR2_I2C_INTR_MASK	0x830	/* the DACs' I2C master: IC_INTR_MASK */
#define AE9_BAR2_UART_FCR	0xc08	/* the head unit's UART: FIFO control */

/* one write of Windows' BAR2 init, read back (as the driver always has: the value isn't used) */
static void bar2_init_w(struct creative_chip *chip, u16 reg, u32 val)
{
	creative_ext_writel(chip, reg, val);
	creative_ext_readl(chip, reg);
}

/*
 * Windows' BAR2 init (the cold-boot trace's first writes, lines 1-114;
 * WINDOWS-CAPTURE-ANALYSIS §12-13), at every bring-up before the controller
 * reset, as Windows does it (creative_ae9_hw_bringup(), core.c): the I2S
 * block on with its four channels' transmit and interrupts off, the
 * transmitter on; pins 4-7 to GPIO (BAR2 0x100 = 0x600); the I2S clock
 * configured and on; 0x100 = 0x60f, 0x70f; the DACs' I2C master: its
 * interrupt mask 0xaff, off, master mode 0x6b, on, 0x6b again, target 0x57
 * (mainline AE-5's init value); the CA0113 command register idle; 0x1c; the
 * head unit's UART at 31250 baud (divisor 0x30) then 8N1 (three times), its
 * FIFO control (the values Windows wrote: results of its read-modify-writes
 * of what reads as 0xc1), its interrupt enable 0x80; 0x1c's MX_BSIZE byte
 * pulsed 0 and back (before the codec IDs are read). Each write read back.
 * Then the log line with the card's ID register and 0x1c.
 */
void creative_ae9_bar2_init_windows(struct creative_chip *chip)
{
	unsigned int ch;

	bar2_init_w(chip, AE9_BAR2_I2S + DW_I2S_IER, 1);
	for (ch = 0; ch < 4; ch++)
		bar2_init_w(chip, AE9_BAR2_I2S + DW_I2S_TER(ch), 0);
	for (ch = 0; ch < 4; ch++)
		bar2_init_w(chip, AE9_BAR2_I2S + DW_I2S_IMR(ch), 0);
	bar2_init_w(chip, AE9_BAR2_I2S + DW_I2S_ITER, 1);
	bar2_init_w(chip, AE9_BAR2_PIN_FN, 0x600);
	bar2_init_w(chip, AE9_BAR2_I2S + DW_I2S_CCR, 0x14);
	bar2_init_w(chip, AE9_BAR2_I2S + DW_I2S_CER, 1);
	bar2_init_w(chip, AE9_BAR2_PIN_FN, 0x60f);
	bar2_init_w(chip, AE9_BAR2_PIN_FN, 0x70f);

	bar2_init_w(chip, AE9_BAR2_I2C_INTR_MASK, 0xaff);
	bar2_init_w(chip, AE9_BAR2_I2C_ENABLE, 0);
	bar2_init_w(chip, AE9_BAR2_I2C_CON, 0x6b);
	bar2_init_w(chip, AE9_BAR2_I2C_ENABLE, 1);
	bar2_init_w(chip, AE9_BAR2_I2C_CON, 0x6b);
	bar2_init_w(chip, AE9_BAR2_I2C_TAR, 0x57);
	bar2_init_w(chip, AE9_BAR2_CA0113_CMD, 0x00800000);
	bar2_init_w(chip, AE9_BAR2_CTL, 0x00880480);

	bar2_init_w(chip, AE9_BAR2_UART_LCR, 0x80);
	bar2_init_w(chip, AE9_BAR2_UART_DATA, 0x30);
	bar2_init_w(chip, AE9_BAR2_UART_IER, 0x00);
	bar2_init_w(chip, AE9_BAR2_UART_LCR, 0x00);
	bar2_init_w(chip, AE9_BAR2_UART_LCR, 0x03);
	bar2_init_w(chip, AE9_BAR2_UART_LCR, 0x03);
	bar2_init_w(chip, AE9_BAR2_UART_LCR, 0x03);
	bar2_init_w(chip, AE9_BAR2_UART_FCR, 0x01);
	bar2_init_w(chip, AE9_BAR2_UART_FCR, 0xf1);
	bar2_init_w(chip, AE9_BAR2_UART_FCR, 0x01);
	bar2_init_w(chip, AE9_BAR2_UART_FCR, 0xc7);
	bar2_init_w(chip, AE9_BAR2_UART_FCR, 0xc1);
	bar2_init_w(chip, AE9_BAR2_UART_IER, 0x80);

	bar2_init_w(chip, AE9_BAR2_CTL, 0x00000480);
	bar2_init_w(chip, AE9_BAR2_CTL, 0x00880480);
	/*
	 * 0x1c: Windows keeps the low half (bit 9 set lost codec 2 on one
	 * AE-9); every AE-9 seen reads 0x880480 before and after
	 */
	dev_info(chip->dev, "bar2_first: Windows BAR2 init done, BAR2+0x4=0x%08x (Windows: 0x7172000a), 0x1c=0x%08x (Windows: 0x00880480)\n",
		 creative_ext_readl(chip, AE9_BAR2_ID), creative_ext_readl(chip, AE9_BAR2_CTL));
}
