// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 3xHelix
//
// Sound Blaster AE-7 (experimental): the board, its checks, its start
// after the firmware, its controls and their apply, its part of the AE-9
// lifecycle (shutdown, suspend, resume). #included at the end of
// creative_ae9_core.c, so it uses core.c's helpers (the router-chip waits,
// the stream drain, the DSP health line, the "Card Answering" and "Driver
// Health" controls) without exporting them.
//
// Opt-in: the id_table stays the AE-9's, so an AE-7 comes here only by a
// driver_override (the opt-in udev rule, ae9-ae7-enable), and is run only
// with the option ae7=1; with ae7=0 ae9_board_lookup() (core.c) refuses it.
//

#include "creative_ae7.h"

/*
 * ae7= (the probe guard reads it, the AE-7's bring-up sets its level from
 * it): 0 = an AE-7 sent here is refused, the default; 1 = run it; 2-5 =
 * the test kits' values (their levels: identify, DSP-mode stereo, 6
 * channels, Direct Mode; 5 = kit 8's), run as 1 with one log line.
 */
int ae9_ae7_opt;
module_param_named(ae7, ae9_ae7_opt, int, 0444);
MODULE_PARM_DESC(ae7, "Sound Blaster AE-7, experimental: 0 = leave an AE-7 alone (default), 1 = run it (docs/AE7.md; ae9-ae7-enable sets it); 2-5 (test kits) = 1");

/* the AE-7's level (AE7_MODE_*): set at each bring-up (creative_ae7_level()) */
int ae9_ae7_mode;

#ifdef AE9_TEST
/*
 * Test builds: the test kits' narrower levels on an AE-7 that ae7= runs
 * (1 identify, 2 DSP-mode stereo, 3 6 channels in DSP mode); 0 = ae7=1's
 */
static int ae7_test;
module_param(ae7_test, int, 0444);
MODULE_PARM_DESC(ae7_test, "Test build: an AE-7's test level (with ae7=1): 0 = as ae7=1 (default), 1 = identify it and stop, 2 = stereo in DSP mode only, 3 = 6 channels in DSP mode only");
#endif

/* the level ae7= (and a test build's ae7_test=) gives the card */
static int creative_ae7_level(void)
{
#ifdef AE9_TEST
	if (ae7_test >= AE7_MODE_IDENTIFY && ae7_test < AE7_MODE_DIRECT)
		return ae7_test;
#endif
	return AE7_MODE_DIRECT;
}

/*
 * ae7_lanes= (kit 10; an option of test builds only): the CA0132 DAC
 * ports for stream 0x0c's lanes c0..c5 (the DSP's processed output: front,
 * centre/LFE, rear by the AE-9's order), the second half (c6..cb, the 96
 * kHz second samples) the same + 8. Release builds keep the default. Default: Windows' map (AltDACMapping: e0,e1,e4,e5,e2,e3). Only
 * a permutation of e0..e5 with c0/c1 on e0/e1 is taken (the front lanes
 * never move, so a value can only move what the Rear and C/Sub jacks play
 * between them); anything else is ignored with a warning. 0644: a change
 * takes effect at the next DSP-mode stream start (the DAC muted and
 * checked, stream 0x0c's slots walked again, then the gate) or the next
 * bring-up; debugfs state's remap line says "as Windows" or "custom".
 */
static u8 ae7_lanes_want[6] = { 0xe0, 0xe1, 0xe4, 0xe5, 0xe2, 0xe3 };
static unsigned int ae7_lanes_gen;

/* six ports: c0/c1 on e0/e1, c2..c5 each of e2..e5 once */
static bool ae7_lanes_ok(const u8 *p)
{
	unsigned int k, seen = 0;

	if (p[0] != 0xe0 || p[1] != 0xe1)
		return false;
	for (k = 2; k < 6; k++) {
		if (p[k] < 0xe2 || p[k] > 0xe5 || (seen & BIT(p[k] - 0xe0)))
			return false;
		seen |= BIT(p[k] - 0xe0);
	}
	return true;
}

#ifdef AE9_TEST
/* "e0,e1,e4,e5,e2,e3" (hex, 0x optional): stored for the next start; -EINVAL: ignored */
static int ae7_lanes_store(const char *val)
{
	u8 p[6];
	char junk;

	if (sscanf(val, "%hhx,%hhx,%hhx,%hhx,%hhx,%hhx %c", &p[0], &p[1], &p[2], &p[3], &p[4],
		   &p[5], &junk) != 6 || !ae7_lanes_ok(p)) {
		pr_warn("snd-hda-codec-creative-ae9: ae7_lanes=%s ignored: six DAC ports for lanes c0..c5, a permutation of e0..e5 with c0/c1 on e0/e1 (Windows': e0,e1,e4,e5,e2,e3)\n",
			val);
		return -EINVAL;
	}
	memcpy(ae7_lanes_want, p, sizeof(p));
	WRITE_ONCE(ae7_lanes_gen, ae7_lanes_gen + 1);
	return 0;
}

static int ae7_lanes_set(const char *val, const struct kernel_param *kp)
{
	ae7_lanes_store(val);
	return 0;	/* ignored, not refused: a bad value must not stop the module's load */
}

static int ae7_lanes_get(char *buf, const struct kernel_param *kp)
{
	const u8 *p = ae7_lanes_want;

	return sysfs_emit(buf, "%02x,%02x,%02x,%02x,%02x,%02x\n", p[0], p[1], p[2], p[3], p[4], p[5]);
}

static const struct kernel_param_ops ae7_lanes_ops = {
	.set = ae7_lanes_set,
	.get = ae7_lanes_get,
};
module_param_cb(ae7_lanes, &ae7_lanes_ops, NULL, 0644);
MODULE_PARM_DESC(ae7_lanes, "Test build: AE-7, the CA0132 DAC ports of the DSP's lanes c0..c5 (Rear and C/Sub jacks), a permutation of e0..e5 with c0/c1 on e0/e1 (default Windows' e0,e1,e4,e5,e2,e3); takes effect at the next stream start in DSP mode");
#endif

/*
 * ae7_lanes= into the walk's table (both halves) when it changed since the
 * table was set; checked again (a write while copying: next time). true =
 * the table changed.
 */
static bool ae7_lanes_apply(struct creative_chip *chip)
{
	unsigned int gen = READ_ONCE(ae7_lanes_gen), k;
	u8 p[6];

	if (gen == chip->ae7.lanes_gen)
		return false;
	memcpy(p, ae7_lanes_want, sizeof(p));
	if (!ae7_lanes_ok(p))
		return false;
	chip->ae7.lanes_gen = gen;
	for (k = 0; k < 6; k++) {
		chip->ae7.lane_port[k] = p[k];
		chip->ae7.lane_port[k + 6] = p[k] + 8;
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* Identify								  */
/* ------------------------------------------------------------------ */

static void ae7_plug_work(struct work_struct *w);
static void ae7_plug_controls_add(struct creative_chip *chip);
static void ae7_plugs_show(struct seq_file *m, struct creative_chip *chip);

/* the AE-7's settings at its first bring-up: the safe defaults */
static void creative_ae7_defaults(struct creative_chip *chip)
{
	if (chip->ae7.inited)
		return;
	chip->ae7.inited = true;
	mutex_init(&chip->ae7.path_mutex);
	INIT_DELAYED_WORK(&chip->ae7.plug_work, ae7_plug_work);
	chip->hu_att = AE7_ATT_DEFAULT;			/* -36 dB */
	chip->hu_att_out[0] = chip->hu_att_out[1] = AE7_ATT_DEFAULT;
	chip->hu_mute = 0;
	chip->ae7.out = 1;				/* headphones (s3boun3t's ACM) */
	chip->hu_output = 1;
	chip->ae7.gain = AE7_GAIN_DEFAULT;		/* Low */
	chip->ae7.filter = 0;				/* Fast, the K2M's power-on filter */
	chip->dac_filter = 0;
	chip->dac_filter_bits = ae7_filter_bits[0];
	chip->ae7.ch = ae9_ae7_mode == AE7_MODE_6CH ? 6 : 2;
	chip->ae7.direct_want = false;			/* DSP mode, the start-up's path */
	chip->ae7.dac01 = chip->ae7.dac0a = chip->ae7.dac0b = chip->ae7.dac40 = -1;
	chip->ae7.pin43 = chip->ae7.i2s_clk = chip->ae7.i2s_rate = chip->ae7.tx_ctl = -1;
	chip->ae7.out_sent = chip->ae7.gain_sent = chip->ae7.filter_sent = -1;
	chip->ae7.att_sent = -1;
	chip->ae7.pins_state = -1;
	memcpy(chip->ae7.lane_port, ae7_lane_win, sizeof(chip->ae7.lane_port));
}

#ifdef AE9_TEST
/*
 * Test level 1 (ae7_test=1): the transport check only. The controller
 * reset, the codec detect through the immediate-command interface
 * (STATESTS, codec 1's IDs), the controller back in reset. No BAR2 write,
 * no codec write, no DSP.
 */
static int creative_ae7_identify_only(struct creative_chip *chip)
{
	u32 ssid = 0;
	int ret;

	ret = creative_ae9_hw_reset(chip);
	if (ret < 0)
		goto out;
	creative_hda_writel(chip, AZX_REG_GCTL, AZX_GCTL_RESET | AZX_GCTL_UNSOL);
	msleep(CREATIVE_CODEC_PROBE_DELAY_MS);
	creative_hda_writeb(chip, AZX_REG_IRS, 0x5);
	creative_hda_writeb(chip, AZX_REG_IRS, 0x0);
	creative_hda_writeb(chip, AZX_REG_IRS, 0x4);
	chip->last_irs_ack = 0x6;
	creative_ae9_detect_codecs(chip);
	if (chip->codec_ok_mask & BIT(1))
		creative_ae9_icw_send_verb(chip, creative_ae9_verb(1, 1, AC_VERB_GET_SUBSYSTEM_ID, 0),
					   &ssid);
out:
	creative_hda_writel(chip, AZX_REG_GCTL, 0);	/* the controller back in reset */
	dev_info(chip->dev, "AE-7 identify only (ae7_test=1): BAR2+0x4 0x%08x, BAR2+0x8 0x%x, STATESTS mask 0x%x (want 0x%x), codec 1 subsystem 0x%08x (want 0x%08x)%s; nothing else done. Load without ae7_test to run it\n",
		 chip->bar2_id[2], chip->ae7.bar2_rev, chip->codec_mask, AE7_CODEC_MASK, ssid,
		 AE7_CODEC_SSID,
		 ret < 0 ? ", controller reset FAILED" :
		 chip->codec_mask == AE7_CODEC_MASK && ssid == AE7_CODEC_SSID ? ": an AE-7, answering" :
		 ": NOT as expected");
	return -ENODEV;
}
#endif

/*
 * The bring-up's first step on an AE-7 (hw_bringup()'s bringup_check op):
 * BAR2+0x4 must read 0x0081xx (Windows reads it too; s3boun3t's card
 * 0x00810009), before any write; BAR2+0x8 picks the DAC's THD row. No bus
 * reset unless bus_reset=1: the AE-7's codec reports its own subsystem
 * without one (the AE-9's doesn't), and s3boun3t's working driver never
 * does one. Test level 1 stops after the identify. The gate on the board
 * ID is this one (before anything is written); core.c's "card alive" check
 * (after the bus reset if bus_reset=1, else right after this) reads it
 * again into the same chip->bar2_id[2] and only confirms it against the board's bar2_id (0x0081), besides its
 * own DesignWare I2C ID check, which stops the bring-up as on the AE-9.
 */
static int creative_ae7_bringup_check(struct creative_chip *chip, bool *do_reset)
{
	u32 id = creative_ext_readl(chip, AE7_BAR2_ID);
	u32 rev = creative_ext_readl(chip, AE7_BAR2_REV);

	if (ae9_ae7_opt < 1 || ae9_ae7_opt > AE7_OPT_MAX) {
		dev_err(chip->dev, "AE-7: ae7=%d is not a valid value (0 off, 1 on): leaving the card alone\n",
			ae9_ae7_opt);
		return -ENODEV;
	}
	if (!chip->ae7.inited && ae9_ae7_opt > 1)
		dev_info(chip->dev, "ae7=%d is a test-kit value, treated as 1\n", ae9_ae7_opt);
	ae9_ae7_mode = creative_ae7_level();
	creative_ae7_defaults(chip);
	*do_reset = bus_reset > 0;
	chip->bar2_id[2] = id;		/* ae9_bar2_alive() reads it again (after the bus reset, if any) */
	chip->ae7.bar2_rev = rev;
	chip->ae7.thd_row = (rev >> 2) & 7;
	if ((id & AE7_BAR2_ID_MASK) != AE7_BAR2_ID_WANT) {
		dev_err(chip->dev, "AE-7: BAR2+0x4 reads 0x%08x, not an AE-7's 0x0081xxxx: leaving the card alone\n",
			id);
		return -ENODEV;
	}
	dev_info(chip->dev, "AE-7 (experimental%s): BAR2+0x4 0x%08x (board revision 0x%02x), BAR2+0x8 0x%x (DAC row %u), bus reset %s\n",
		 ae9_ae7_mode == AE7_MODE_DIRECT ? "" : ", a test level", id, id & 0xff, rev,
		 chip->ae7.thd_row, *do_reset ? "on (bus_reset=1)" : "off");
#ifdef AE9_TEST
	if (ae9_ae7_mode == AE7_MODE_IDENTIFY)
		return creative_ae7_identify_only(chip);
#endif
	return 0;
}

/* after the codec detect: codec 1 alone, with the AE-7's subsystem ID */
static int creative_ae7_codecs_check(struct creative_chip *chip)
{
	u32 ssid = 0;

	creative_ae9_icw_send_verb(chip, creative_ae9_verb(1, 1, AC_VERB_GET_SUBSYSTEM_ID, 0), &ssid);
	if (chip->codec_mask == AE7_CODEC_MASK && ssid == AE7_CODEC_SSID)
		return 0;
	dev_err(chip->dev, "AE-7: codec mask 0x%x (want 0x%x), codec 1 subsystem 0x%08x (want 0x%08x): stopping\n",
		chip->codec_mask, AE7_CODEC_MASK, ssid, AE7_CODEC_SSID);
	return -ENODEV;
}

/*
 * The card's jacks that sense a plug (kit 10; L0a on the tester's AE-7:
 * bit 31 of GET_PIN_SENSE on pins 0x0b, 0x0f, 0x11 and 0x12; the
 * Center / Subwoofer jack's pin 0x10 never senses), by Creative's names,
 * with the unsolicited tag the start-up gives each pin (creative_ae7_seq.c:
 * 0x12 tag 1, 0x11 tag 2, 0x0b / 0x0f / 0x10 tag 5). The Audio Control
 * Module's leads plug into the last two (its headphone lead into
 * Headphone / Headset-out, its mic lead into Line-in / Mic-in; its own
 * jacks are passive). Their controls are "... Plugged", never "... Jack":
 * PipeWire switches outputs on those.
 */
static const struct {
	u8 nid, tag;
	const char *name;	/* the log's and debugfs state's */
	const char *ctl;	/* its read-only control */
} ae7_jacks[AE7_PLUGS] = {
	{ 0x0b, 5, "Front", "Card Front Plugged" },
	{ 0x0f, 5, "Rear", "Card Rear Plugged" },
	{ 0x11, 2, "Headphone / Headset-out", "Card Headphone Headset-out Plugged" },
	{ 0x12, 1, "Line-in / Mic-in", "Card Line-in Mic-in Plugged" },
};

/* plug_work in AE7_PLUG_MS, while armed (from the start-up's end to a cancel) */
static void ae7_plug_kick(struct creative_chip *chip)
{
	if (READ_ONCE(chip->ae7.plug_armed) && !READ_ONCE(chip->unloading))
		queue_delayed_work(chip->wq, &chip->ae7.plug_work, msecs_to_jiffies(AE7_PLUG_MS));
}

/* ------------------------------------------------------------------ */
/* After the firmware							  */
/* ------------------------------------------------------------------ */

/*
 * The AE-7's start after its firmware (creative_ae9_post_fw()'s op): the
 * boot handshake if not done, the stream drain (ours: a warm card holds
 * the last run's references), Windows' AE-7 set-up (creative_ae7_tail()),
 * the summary and DSP health lines, the failed DSP messages again; then
 * the first output select, after the ASI pin group (the DAC answers from
 * there), the volume, the DAC's mute gate, the LED, BAR2 0x1c (Windows'
 * last write of its first select). Nothing of the AE-9's head unit or
 * Direct Mode start. The DSP's output block and the effects follow from
 * the heartbeat (kit 10: one bracket, creative_ae7_fx.c), and in DSP mode
 * the gate waits for them; on a resume the fx state is the user's again.
 */
static void creative_ae7_post_fw(struct creative_chip *chip)
{
	struct ae9_tail tail;
	unsigned int dbg = 0, out;
	int ret, bad;

	creative_dsp_chipio_read(chip, 0x100e30, &dbg);
	if (!chip->hs_done) {
		ret = creative_dsp_boot_handshake(chip);
		dev_info(chip->dev, "fw_split: boot handshake %s (%d)\n",
			 ret ? "INCOMPLETE" : "complete", ret);
	}
	ae9_streams_drain(chip, "before the AE-7 start-up, left by an earlier run");
	ae7_lanes_apply(chip);		/* ae7_lanes= for the start-up's walk */
	creative_ae7_tail(chip, &tail);
	creative_dsp_chipio_read(chip, 0x100e30, &dbg);
	dev_info(chip->dev,
		 "AE-7: start-up done: %u SCP msgs (%u failed, %u skipped), %u verbs (%u failed); DBGCNTL 0x%08x\n",
		 tail.msgs, tail.msg_err, tail.msg_skipped, tail.verbs, tail.verb_err, dbg);
	creative_ae9_dsp_health(chip, dbg);
	creative_ae9_tail_resend(chip, &tail);

	/* the start-up's path: DSP mode at 96 kHz, as Windows' (phase 2's
	 * switches start from here; a resume's bring-up too) */
	chip->ae7.direct = false;
	chip->ae7.path_rate = AE7_DSP_RATE;
	chip->ae7.spdif_reclock = false;
	chip->ae7.path_ok = true;
	chip->ae7.bound = false;
	chip->ae7.s14 = false;
	chip->ae7.walked = chip->ae7.walk_want = 0;
	chip->ae7.levels_set = false;
	chip->ae7.dsp_ch = 0;		/* the start-up's DSP path: no stream bound yet */
	/* the DSP's output block: from the heartbeat, after the probe's
	 * (probe_done) or, on a resume, now's fx state; the gate waits for it */
	chip->ae7.block_sent = false;
	if (chip->ae7.fx_armed) {
		mutex_lock(&chip->ae7.path_mutex);
		chip->fx.dead = chip->dsp_halted;
		chip->fx.fails = 0;
		creative_ae7_fx_direct(chip, false);
		chip->ae7.fx_pending = chip->ae7.cal_pending = true;
		mutex_unlock(&chip->ae7.path_mutex);
	}
	out = READ_ONCE(chip->ae7.out);
	bad = creative_ae7_select(chip, out, false);
	bad += creative_ae7_volume(chip, out);
	chip->ae7.filter_sent = chip->ae7.filter;	/* every 48/07 carries it (dsp.c) */
	/* the select wrote Low's trim (Windows' first); a saved gain other than
	 * Low comes from the heartbeat, and the gate after it */
	if (!bad && (chip->ae7.out_sent != 1 || chip->ae7.gain_sent == chip->ae7.gain))
		creative_ae7_unmute(chip);
	creative_ae7_led(chip, true);
	creative_ext_writel(chip, CREATIVE_REG_1C, AE7_BAR2_CTL_1C);
	/* the heartbeat applies whatever is still pending, and retries */
	queue_delayed_work(chip->wq, &chip->hb_work, msecs_to_jiffies(200));
	/* every jack read (at a resume: what changed while asleep), then on its events */
	atomic_set(&chip->ae7.plug_pending, BIT(AE7_PLUGS) - 1);
	WRITE_ONCE(chip->ae7.plug_armed, true);
	ae7_plug_kick(chip);
}

/* ------------------------------------------------------------------ */
/* Playback start (creative_ae7_pcm.c)					  */
/* ------------------------------------------------------------------ */

/*
 * The bind as the AE-9's DSP-mode start does it, without its router work
 * (0x18b008 is sent before, creative_ae7_hold_off(): in Direct Mode
 * Windows' stage 2 comes between the two): the router chip idle, the
 * format, the bind, idle again, the
 * format the router chip cached checked (the AE-9's 2026-11-07 fix: a quick
 * reopen at another format could leave the old one), repaired once. Returns
 * the format byte the router chip holds (< 0 unread).
 */
static int ae7_bind_checked(struct creative_chip *chip, u16 format)
{
	int got;

	ae9_8051_wait_idle(chip, "AE-7: before the bind");
	creative_ae7_bind(chip, format);
	chip->ae7.bound = true;
	ae9_8051_wait_idle(chip, "AE-7: after the bind");
	got = ae9_8051_xram_byte(chip, AE9_8051_LINK_HOST_FMT);
	if (got >= 0 && got != (format & 0xff)) {
		dev_info(chip->dev, "AE-7: router chip kept the old stream format (0x%02x, stream 0x%02x): unbind and bind again\n",
			 got, format & 0xff);
		creative_ae7_unbind(chip);
		ae9_8051_wait_idle(chip, "AE-7: after the repair unbind");
		creative_ae9_dsp_rebind(chip, format);
		ae9_8051_wait_idle(chip, "AE-7: after the repair bind");
		got = ae9_8051_xram_byte(chip, AE9_8051_LINK_HOST_FMT);
	}
	return got;
}

/*
 * A stream's start in DSP mode (once per open, from the prepare): the DSP's
 * output unmuted, 0x18b008, the bind checked (ae7_bind_checked()); capture
 * B's six DSP-mode starts, the first after Direct Mode off too, are these
 * four steps only (the switch itself had re-clocked the I2S)
 */
void creative_ae7_stream_start(struct creative_chip *chip, u16 format)
{
	int got;

	creative_ae7_stream_unmute(chip);
	creative_ae7_hold_off(chip);
	got = ae7_bind_checked(chip, format);
	dev_info(chip->dev, "AE-7: playback start, format 0x%04x (router chip 0x%02x%s)\n",
		 format, got & 0xff, got == (format & 0xff) ? "" : ", WRONG");
}

/* ------------------------------------------------------------------ */
/* Controls and their apply						  */
/* ------------------------------------------------------------------ */

/* the heartbeat applies a control change AE7_KICK_MS later (Windows' volume thread: 20 ms after a change) */
static void creative_ae7_kick(struct creative_chip *chip)
{
	if (!READ_ONCE(chip->unloading))
		mod_delayed_work(chip->wq, &chip->hb_work, msecs_to_jiffies(AE7_KICK_MS));
}

/* the heartbeat again in AE7_LOCK_POLL_MS: the gate waits for the DAC's lock (creative_ae7_unmute()) */
static void ae7_lock_poll(struct creative_chip *chip)
{
	if (!READ_ONCE(chip->unloading))
		queue_delayed_work(chip->wq, &chip->hb_work, msecs_to_jiffies(AE7_LOCK_POLL_MS));
}

/*
 * The gate at a start's end (the prepare, the stream not running yet), so
 * a path confirmed there plays from its first sample. Kit 8's first starts
 * unmuted ~110 ms after the path (the heartbeat 20 ms later, then the
 * gate's three DAC reads) with the stream already running: the login
 * sound lost its start. Only when the heartbeat has nothing to apply
 * before its gate (the output asked is the one selected, the volume and on
 * the headphones the gain acknowledged, the filter sent, the remap done,
 * the path confirmed, in DSP mode the output block sent); the gate's own
 * checks, unchanged, then decide. Not
 * so, refused, the level changed since it was sent (-ESTALE), or waiting
 * for the DAC's lock: the heartbeat, as before (its gate retries; the lock
 * poll).
 */
static void ae7_gate_at_start(struct creative_chip *chip)
{
	bool settled, stale;
	u8 want_att;
	int ret;

	if (chip->ae7.unmuted) {
		creative_ae7_kick(chip);
		return;
	}
	/* the output selected is the one picked, its level acknowledged, no
	 * Master write waiting (creative_ae7_want_att()) */
	want_att = creative_ae7_want_att(chip, chip->ae7.out_sent, NULL, &stale);
	settled = chip->ae7.out_sent >= 0 && !stale && chip->ae7.att_sent == want_att &&
		  (chip->ae7.out_sent != 1 || chip->ae7.gain_sent == chip->ae7.gain) &&
		  chip->ae7.filter_sent == chip->ae7.filter &&
		  chip->ae7.remapped == AE7_REMAP_SLOTS && chip->ae7.path_ok &&
		  (chip->ae7.direct || chip->ae7.block_sent);
	ret = settled ? creative_ae7_unmute(chip) : -EIO;
	if (ret == -EAGAIN)
		ae7_lock_poll(chip);
	else if (ret)
		creative_ae7_kick(chip);
}

/* ------------------------------------------------------------------ */
/* Phase 2: the playback path, Direct Mode or DSP (level 4)	  */
/* ------------------------------------------------------------------ */

/* a CA0132 read-back for the log and debugfs state: hex, "-" when not read */
static const char *ae7_rbw(char *buf, size_t n, int v)
{
	if (v < 0)
		return "-";
	snprintf(buf, n, "0x%x", v);
	return buf;
}

/*
 * A stream's start in Direct Mode (from its prepare: the stream not
 * running), in Windows' AE-7 order (capture B: 13 starts at 48 kHz, the
 * first after the switch with the clock, the 12 others without, all else
 * the same; capture Bw: 32 starts at all six rates, the first after each
 * rate change with the clock, the same-rate ones without; docs/dev/AE7.md
 * "Phase 2"), with ours around it:
 * - ours: a switch (into Direct Mode, or another rate) mutes the DAC first
 *   and confirms the mute by reading 48/07 back; unconfirmed, no switch
 *   (-EIO: the prepare fails, nothing changed). The gate
 *   (creative_ae7_unmute(): at the start's end, ae7_gate_at_start(), else
 *   from the heartbeat) unmutes only after all of the below went through,
 *   read back, and the DAC reports lock (waited for: AE7_LOCK_WAIT_MS);
 * - the generic stream start's first part: the DSP's output unmuted
 *   (Windows' parity), 0x18b008 (creative_ae7_hold_off());
 * - stage 2: the DSP's output stream 0x18 stopped where it runs (at every
 *   start, as Windows; Windows' SetDirect(1) stopped it at the switch,
 *   ours at the first start after it: creative_ae7_dsp_out_off()); ours,
 *   when it was stopped or the card wasn't in Direct Mode: the 8051 idle,
 *   no DSP route left on an I2S port (creative_ae7_ports_off()); stream
 *   0x05 to the DAC's I2S point (0x91, the S/PDIF transmitter, for the
 *   44.1 kHz family) and stream 0x14 (creative_ae7_direct_route());
 * - the generic start's second part: the format and the bind, checked
 *   (ae7_bind_checked());
 * - stage 3: the clocks and the DAC's input for the rate when it changed
 *   (creative_ae7_rate_clock()); the router walk of stream 0x05's slots
 *   (I2S rates; creative_ae7_direct_walk()); stream 0x14 on, ASI 7
 *   (creative_ae7_direct_on());
 * - ours: 48/01, 48/0a (48/0b), the CA0132's data pin and on I2S its
 *   clock descriptors read back (creative_ae7_path_check()); on S/PDIF the
 *   transmitter's control (creative_ae7_direct_on());
 * - into Direct Mode (from DSP mode): an fx bracket with the Direct block
 *   and the effects off (creative_ae7_fx_apply(), fx.direct on: kit 10).
 * A same-rate start that goes wrong mutes the DAC after (still before the
 * stream runs). Returns 0, or < 0 when nothing was switched.
 */
static int ae7_direct_start(struct creative_chip *chip, const struct ae7_rate *row, u16 format)
{
	bool change = !chip->ae7.direct || chip->ae7.path_rate != row->rate;
	unsigned int walked = 0, off = 0;
	int bad = 0, rb, got, s18;
	char p43[12], c1[12], c2[12], clk[40];

	if (change && creative_ae7_mute_checked(chip)) {
		chip->ae7.path_fails++;
		dev_warn(chip->dev, "AE-7: Direct Mode at %u Hz refused: the DAC's mute isn't confirmed (48/07); nothing switched\n",
			 row->rate);
		return -EIO;
	}
	chip->ae7.path_ok = false;
	if (change)
		chip->ae7.path_switches++;
	creative_ae7_stream_unmute(chip);
	creative_ae7_hold_off(chip);
	s18 = creative_ae7_dsp_out_off(chip, &bad);
	if (!chip->ae7.direct || s18) {
		if (chip->ae7.direct)
			dev_warn(chip->dev, "AE-7: Direct Mode: the DSP's output (stream 0x18) found running (Active 0x%02x): stopped\n",
				 s18 & 0xff);
		chip->ae7.direct = true;
		ae9_8051_wait_idle(chip, "AE-7: after stream 0x18's stop");
		mutex_lock(&chip->replay_mutex);
		off = creative_ae7_ports_off(chip, 0xc0, 0xe0);
		mutex_unlock(&chip->replay_mutex);
	}
	bad += creative_ae7_direct_route(chip, row);
	got = ae7_bind_checked(chip, format);
	bad += got != (format & 0xff);
	bad += creative_ae7_rate_clock(chip, row);
	chip->ae7.walk_want = row->spdif ? 0 : row->slots;
	if (!row->spdif) {
		mutex_lock(&chip->replay_mutex);
		walked = creative_ae7_direct_walk(chip, row->slots);
		mutex_unlock(&chip->replay_mutex);
	}
	chip->ae7.walked = walked;
	bad += creative_ae7_direct_on(chip, row);
	rb = creative_ae7_path_check(chip, row);
	chip->ae7.path_ok = !bad && !rb && walked == chip->ae7.walk_want;
	if (!chip->ae7.path_ok) {
		chip->ae7.path_fails++;
		if (!change)
			creative_ae7_mute_checked(chip);
	}
	if (row->spdif)
		snprintf(clk, sizeof(clk), "transmitter %s",
			 ae7_rbw(c1, sizeof(c1), chip->ae7.tx_ctl));
	else
		snprintf(clk, sizeof(clk), "I2S %s/%s", ae7_rbw(c1, sizeof(c1), chip->ae7.i2s_clk),
			 ae7_rbw(c2, sizeof(c2), chip->ae7.i2s_rate));
	dev_info(chip->dev, "AE-7: Direct Mode, %u Hz (%s), format 0x%04x: router chip 0x%02x, %u of %u slots, DAC 48/01 0x%02x 48/0a 0x%02x, pin 0x43 %s, %s%s, %u DSP route(s) left on a DAC port switched off; %s\n",
		 row->rate, row->spdif ? "S/PDIF input" : "I2S", format, got & 0xff, walked,
		 chip->ae7.walk_want, chip->ae7.dac01 & 0xff, chip->ae7.dac0a & 0xff,
		 ae7_rbw(p43, sizeof(p43), chip->ae7.pin43), clk, change ? ", switched" : "", off,
		 chip->ae7.path_ok ? "path confirmed" :
		 "path NOT confirmed: the DAC stays muted (stop and start playback to try again)");
	/* into Direct Mode: the DSP's Direct block, its effects off (Windows'
	 * SetDirect(1) and the block after it), the DAC still muted */
	if (!chip->fx.direct) {
		creative_ae7_fx_direct(chip, true);
		creative_ae7_fx_apply(chip);
	}
	/* the DAC's lock is waited for from here (creative_ae7_unmute()) */
	chip->ae7.lock_from = jiffies;
	ae7_gate_at_start(chip);
	return 0;
}

/*
 * Back to DSP mode (a 48 kHz stream's start with Direct Mode off, from its
 * prepare): the DAC muted and confirmed first (else -EIO, nothing
 * switched), stream 0x14 off if it still runs (stage 0), Windows'
 * SetDirect(0) (creative_ae7_dsp_path()), ours: the 8051 idle, any host
 * slot left on an I2S port switched off (creative_ae7_ports_off()), 48/01 =
 * 0x80, 48/0a = 0x05, PLL/PMU 0x43 = 0xc7 and the 96 kHz descriptors read
 * back; an fx bracket before (fx.direct still on: the effects zeroed) and
 * one after (DSP mode's output block, the effects back: kit 10). The gate
 * reopens through the heartbeat. Not
 * confirmed: the card counts as still in Direct Mode (muted), so the next
 * DSP-mode start leaves it again from the top.
 */
static int ae7_direct_leave(struct creative_chip *chip)
{
	unsigned int off;
	int bad, rb;

	if (creative_ae7_mute_checked(chip)) {
		chip->ae7.path_fails++;
		dev_warn(chip->dev, "AE-7: Direct Mode off refused: the DAC's mute isn't confirmed (48/07); nothing switched\n");
		return -EIO;
	}
	chip->ae7.path_ok = false;
	chip->ae7.path_switches++;
	/* Windows zeroes the effects before SetDirect(0) and sends the output
	 * block after it (W2): a bracket with fx.direct still on, one after */
	chip->ae7.block_sent = false;
	creative_ae7_fx_apply(chip);
	creative_ae7_direct_stop(chip);
	bad = creative_ae7_dsp_path(chip);
	ae9_8051_wait_idle(chip, "AE-7: after the DSP path");
	mutex_lock(&chip->replay_mutex);
	off = creative_ae7_ports_off(chip, 0x00, 0x20);
	mutex_unlock(&chip->replay_mutex);
	chip->ae7.walked = chip->ae7.walk_want = 0;
	/* the Direct route left stream 0x05 at 2 channels; stream 0x18 runs
	 * fresh (a 6-channel stream's start sets 0x05's count, no rebuild) */
	chip->in05_ch = 2;
	chip->ae7.dsp_ch = 0;
	rb = creative_ae7_path_check(chip, NULL);
	chip->ae7.path_ok = !bad && !rb;
	chip->ae7.direct = !chip->ae7.path_ok;
	if (!chip->ae7.path_ok)
		chip->ae7.path_fails++;
	if (!chip->ae7.direct) {
		creative_ae7_fx_direct(chip, false);
		creative_ae7_fx_apply(chip);
	}
	dev_info(chip->dev, "AE-7: Direct Mode off: DSP path at 96 kHz, DAC 48/01 0x%02x 48/0a 0x%02x, %u host slot(s) left on a DAC port switched off; %s\n",
		 chip->ae7.dac01 & 0xff, chip->ae7.dac0a & 0xff, off,
		 chip->ae7.path_ok ? "path confirmed" :
		 "path NOT confirmed: the DAC stays muted, the next stream leaves Direct Mode again");
	return 0;
}

/*
 * A DSP-mode stream with another channel count than the last one since the
 * DSP path was built (level 4: 2 <-> 6; kit 10): the AE-9's rule (restarting
 * stream 0x05 alone at the new count played static on the front,
 * 2026-11-04; its fix goes through Direct Mode and back) done the way a
 * Direct Mode round trip does it on the AE-7: the DAC (and the Rear/C-Sub
 * converters) muted and checked, else -EIO and nothing switched; the DSP's
 * output (stream 0x18) stopped, 150 ms (Windows' wait after leaving DSP
 * mode), the 8051 idle, no DSP route left on a DAC port, Windows'
 * SetDirect(0) (creative_ae7_dsp_path(): stream 0x05 to the DSP, stream
 * 0x18 started again), the path read back; the output block's bracket
 * again (as after Direct Mode off). The stream's start then sets stream
 * 0x05's count, binds, and the gate reopens. Not confirmed: muted, and the
 * next start rebuilds again.
 */
static int ae7_dsp_rebuild(struct creative_chip *chip, unsigned int ch)
{
	unsigned int off;
	int bad = 0, rb;

	if (creative_ae7_mute_checked(chip)) {
		chip->ae7.path_fails++;
		dev_warn(chip->dev, "AE-7: %u-channel stream refused: the DAC's mute isn't confirmed (48/07); nothing switched\n",
			 ch);
		return -EIO;
	}
	chip->ae7.path_ok = false;
	chip->ae7.path_switches++;
	creative_ae7_dsp_out_off(chip, &bad);
	msleep(150);
	ae9_8051_wait_idle(chip, "AE-7: after stream 0x18's stop (channel count)");
	mutex_lock(&chip->replay_mutex);
	off = creative_ae7_ports_off(chip, 0xc0, 0xe0);
	mutex_unlock(&chip->replay_mutex);
	bad += creative_ae7_dsp_path(chip);
	ae9_8051_wait_idle(chip, "AE-7: after the DSP path (channel count)");
	rb = creative_ae7_path_check(chip, NULL);
	chip->ae7.path_ok = !bad && !rb;
	if (!chip->ae7.path_ok)
		chip->ae7.path_fails++;
	chip->ae7.dsp_ch = chip->ae7.path_ok ? 0 : chip->ae7.dsp_ch;
	dev_info(chip->dev, "AE-7: stream now %u channels (was %u): DSP path rebuilt, %u DSP route(s) left on a DAC port switched off; %s\n",
		 ch, chip->in05_ch, off, chip->ae7.path_ok ? "path confirmed" :
		 "path NOT confirmed: the DAC stays muted, the next stream rebuilds again");
	/* the DSP's output block again, as after Direct Mode off */
	chip->ae7.block_sent = false;
	creative_ae7_fx_apply(chip);
	return 0;
}

/*
 * ae7_lanes= changed since the last walk (a DSP-mode start, kit 10): the
 * DAC (and the Rear/C-Sub converters) muted and checked, else -EIO and
 * nothing changed; the new table, stream 0x0c's slots walked again (the
 * heartbeat's walk: its checks, its 12-of-12 count for the gate, which
 * reopens after the start)
 */
static int ae7_lanes_rewalk(struct creative_chip *chip)
{
	unsigned int n;

	if (creative_ae7_mute_checked(chip)) {
		dev_warn(chip->dev, "AE-7: ae7_lanes= not applied: the DAC's mute isn't confirmed (48/07)\n");
		return -EIO;
	}
	if (!ae7_lanes_apply(chip))
		return 0;
	mutex_lock(&chip->replay_mutex);
	n = creative_ae7_remap(chip);
	mutex_unlock(&chip->replay_mutex);
	dev_info(chip->dev, "AE-7: ae7_lanes=%02x,%02x,%02x,%02x,%02x,%02x (%s): stream 0x0c's slots walked again, %u of %u\n",
		 chip->ae7.lane_port[0], chip->ae7.lane_port[1], chip->ae7.lane_port[2],
		 chip->ae7.lane_port[3], chip->ae7.lane_port[4], chip->ae7.lane_port[5],
		 memcmp(chip->ae7.lane_port, ae7_lane_win, AE7_REMAP_SLOTS) ? "custom" : "Windows'",
		 n, AE7_REMAP_SLOTS);
	return 0;
}

/*
 * A playback start (the prepare, once per open, rate and format): Direct
 * Mode at level 4 when its switch is on, or for any rate but 48 kHz (the
 * open offered those only with the switch on: a stream opened so stays in
 * Direct Mode until it stops); else DSP mode, leaving Direct Mode first if
 * the card is in it. A start sent before on this open (a restart after a
 * Direct Mode switch, or new hw_params) is stopped first. -EINVAL: a rate
 * the level refuses (creative_ae7_rate_row()). A 6-channel stream (level 4,
 * kit 10) is DSP mode's whatever the switch says (Direct Mode is stereo);
 * a DSP-mode stream with another count than the last since the DSP path
 * was built rebuilds it first (ae7_dsp_rebuild()).
 */
static int ae7_play_start(struct creative_chip *chip, const struct ae7_rate *row, u16 format)
{
	/* 6 channels: the link format's count (a DSP-mode stream: 48 kHz;
	 * 384 kHz stereo is 4 channels at 192 kHz on the link) */
	unsigned int ch = (format & 0xf) == 5 ? 6 : 2;
	bool direct = ae9_ae7_mode >= AE7_MODE_DIRECT && ch == 2 &&
		      (READ_ONCE(chip->ae7.direct_want) || row->rate != 48000);
	int err;

	creative_ae7_play_stop(chip);
	chip->ae7.stay_logged = false;
	chip->ae7.start_direct = direct;
	chip->ae7.lock_wait = false;
	/* every start gives the gate its full budget again (10 retries 200
	 * ms apart): a path that once ran it out (a DAC slow to lock) would
	 * otherwise get one try per start (R2's D3) */
	chip->ae7.gate_fails = 0;
	if (direct)
		return ae7_direct_start(chip, row, format);
	if (chip->ae7.direct) {
		err = ae7_direct_leave(chip);
		if (err < 0)
			return err;
	} else if (ae9_ae7_mode >= AE7_MODE_DIRECT && chip->ae7.dsp_ch && chip->ae7.dsp_ch != ch) {
		err = ae7_dsp_rebuild(chip, ch);
		if (err < 0)
			return err;
	}
	if (READ_ONCE(ae7_lanes_gen) != chip->ae7.lanes_gen) {
		err = ae7_lanes_rewalk(chip);
		if (err < 0)
			return err;
	}
	if ((format & 0xf) == 1 || chip->in05_ch == 2)
		creative_ae9_dsp_in_channels(chip, format);
	creative_ae7_stream_start(chip, format);
	chip->in05_ch = ch;
	chip->ae7.dsp_ch = ch;
	if (!chip->ae7.unmuted)
		ae7_gate_at_start(chip);
	return 0;
}

int creative_ae7_play_start(struct creative_chip *chip, unsigned int rate, u16 format)
{
	const struct ae7_rate *row = creative_ae7_rate_row(rate, ae9_ae7_mode);
	int err;

	if (!row) {
		dev_info(chip->dev, "AE-7: %u Hz refused (it plays %s)\n", rate,
			 ae9_ae7_mode >= AE7_MODE_DIRECT ? "44.1/48/88.2/96/192/384 kHz" : "48 kHz");
		return -EINVAL;
	}
	mutex_lock(&chip->ae7.path_mutex);
	err = ae7_play_start(chip, row, format);
	mutex_unlock(&chip->ae7.path_mutex);
	return err;
}

/*
 * A start's stop (the stream's release, a restart, the power-down):
 * converter 0x02 unbound (Windows at a stream's stop), and in Direct Mode
 * stream 0x14 stopped (stage 0)
 */
void creative_ae7_play_stop(struct creative_chip *chip)
{
	if (!chip->ae7.bound)
		return;
	creative_ae7_unbind(chip);
	chip->ae7.bound = false;
	if (chip->ae7.direct)
		creative_ae7_direct_stop(chip);
}

/*
 * The AE-7's heartbeat (creative_ae9_hb_work()'s op): only when something
 * is pending (no traffic when idle, as Windows). In order: an output
 * switch (the DAC muted first, then the new output's volume, then the
 * select), the headphone gain, the DSP's output block with the effects (a
 * bring-up's or a switch's: creative_ae7_fx.c), the filter, the volume,
 * the router remap
 * again while the start-up's was incomplete, a Direct Mode switch for a
 * started stream (level 4: the DAC muted, the stream restarted), then the
 * DAC's mute gate while it is shut. A refused command tries again 200 ms
 * later, up to 10 times in a row; a gate waiting for the DAC's lock (a
 * Direct start's first AE7_LOCK_WAIT_MS) polls every AE7_LOCK_POLL_MS
 * instead, outside that count; a gate that found the level changed since
 * it was sent (-ESTALE: Master written during the tick, an output picked
 * since) runs again AE7_KICK_MS later, no refusal counted. All of it under
 * ae7.path_mutex, which a stream's start on its path
 * (creative_ae7_play_start()) holds too. The levels are the outputs' own
 * (creative_ae7_want_att()): the switch sends the new output's, the
 * volume step the selected output's, read again after the bracket.
 *
 * Every switch mutes the Rear/C-Sub converters and the DAC (48/07 bits
 * 1:0) and shuts the gate before anything else: the new output plays only after the gate has seen its
 * volume (and on headphones the gain) acknowledged and read back, so a
 * refused 48/0f/10 can't leave the headphones at the line-out level. A
 * refused mute: no switch and no level this time (the old output keeps its
 * own, known level); the heartbeat tries again.
 */
static void ae7_tick(struct creative_chip *chip)
{
	unsigned long flags;
	int want_out, bad = 0;
	bool retry = false, poll = false, stale = false;
	u8 want_att;

	spin_lock_irqsave(&chip->lock, flags);
	want_out = chip->ae7.out;
	spin_unlock_irqrestore(&chip->lock, flags);
	/* a Master write is taken here, AE7_KICK_MS after it (the kick), for
	 * the output picked now, not after the bracket below */
	want_att = creative_ae7_want_att(chip, want_out, NULL, NULL);

	if (want_out != chip->ae7.out_sent) {
		/* the Rear/C-Sub converters first, then the DAC */
		if (creative_ae7_conv_mute(chip) || ae7_dac07(chip, 0, K2M_MUTE)) {
			bad++;
			goto done;
		}
		chip->ae7.unmuted = false;
		/* the new output's own level */
		if (chip->ae7.att_sent != want_att)
			bad += creative_ae7_volume(chip, want_out);
		bad += creative_ae7_select(chip, want_out, true);
		/* the new output's block (and the speakers' levels and delays) */
		chip->ae7.block_sent = false;
		chip->ae7.fx_pending = chip->ae7.cal_pending = true;
	}
	if (chip->ae7.out_sent == 1 && chip->ae7.gain_sent != chip->ae7.gain)
		bad += creative_ae7_gain(chip);
	/* the DSP's output block and the effects in one bracket (kit 10): after
	 * a bring-up (probe_done, a resume) or an output switch; DSP mode's
	 * gate waits for it. A preset that went wrong unselects the output:
	 * the next try selects again */
	if (!chip->ae7.direct && !chip->ae7.block_sent)
		chip->ae7.fx_pending = true;
	if (chip->ae7.fx_armed && chip->ae7.fx_pending && chip->ae7.out_sent >= 0) {
		creative_ae7_fx_apply(chip);
		bad += chip->ae7.out_sent < 0;
	}
	if (chip->ae7.filter_sent != chip->ae7.filter)
		bad += creative_ae7_filter(chip);
	/* read again here: a Master write during the bracket (~0.3 s) goes
	 * out in this tick, at the level the gate below wants (kit 10's load
	 * compared a reading from before the bracket with the gate's own: a
	 * Master restored during it was refused) */
	if (chip->ae7.att_sent != creative_ae7_want_att(chip, want_out, NULL, NULL))
		bad += creative_ae7_volume(chip, want_out);
	if (chip->ae7.remapped != AE7_REMAP_SLOTS) {
		/* the start-up's walk didn't find all of stream 0x0c's slots:
		 * again, while the gate's retries last (the list comes late) */
		mutex_lock(&chip->replay_mutex);
		creative_ae7_remap(chip);
		mutex_unlock(&chip->replay_mutex);
	}
	/*
	 * Phase 2: the Direct Mode switch moved while a stream is started for
	 * the other path (start_direct: what its start asked for, so a start
	 * whose switch failed isn't restarted again). The DAC muted (checked) and the stream stopped with
	 * an xrun (core.c's creative_ae9_pb_restart_work(), as the AE-9's
	 * Direct Mode switch does it): its player prepares it again and the
	 * prepare switches (creative_ae7_play_start()); the gate waits for that
	 * start. A stream above 48 kHz has no DSP path: it stays in Direct Mode
	 * until it stops; a 6-channel stream (kit 10) has no Direct path: it
	 * stays in DSP mode until it stops.
	 */
	if (ae9_ae7_mode >= AE7_MODE_DIRECT && chip->ae7.bound && chip->pcm_start_sent &&
	    READ_ONCE(chip->ae7.direct_want) != chip->ae7.start_direct) {
		if (!chip->ae7.direct_want && chip->pcm_start_rate != 48000) {
			if (!chip->ae7.stay_logged)
				dev_info(chip->dev, "AE-7: Direct Mode off: the stream plays at %u Hz, which only Direct Mode plays; it stays in Direct Mode until it stops\n",
					 chip->pcm_start_rate);
			chip->ae7.stay_logged = true;
		} else if (chip->ae7.direct_want && (chip->pcm_start_fmt & 0xf) == 5) {
			if (!chip->ae7.stay_logged)
				dev_info(chip->dev, "AE-7: Direct Mode on: the stream plays 6 channels, which only DSP mode plays; it stays in DSP mode until it stops\n");
			chip->ae7.stay_logged = true;
		} else if (creative_ae7_mute_checked(chip)) {
			bad++;
		} else {
			/* the old path stays as it is (and confirmed) until the
			 * re-prepare switches; the gate waits for that start */
			chip->pcm_start_sent = 0;
			dev_info(chip->dev, "AE-7: Direct Mode %s: DAC muted, the stream restarts on the new path\n",
				 chip->ae7.direct_want ? "on" : "off");
			if (!READ_ONCE(chip->unloading))
				queue_work(chip->wq, &chip->pb_restart_work);
		}
	}
	/* not while a restart for a path switch is pending (its start reopens it) */
	if (!chip->ae7.unmuted && !(chip->ae7.bound && !chip->pcm_start_sent)) {
		int ret = creative_ae7_unmute(chip);

		poll = ret == -EAGAIN;
		/* -EBUSY: waiting for the output block, which the fx step
		 * sends; -ESTALE: Master or the output changed since the
		 * volume was sent (not a refusal): the next tick sends it */
		stale = ret == -ESTALE;
		retry = ret < 0 && !poll && !stale && ret != -EBUSY;
	}
done:
	if (bad) {
		retry = true;
		if (chip->ae7.vol_fails++ == 10)
			dev_warn(chip->dev, "AE-7: DAC commands still not acknowledged after 10 tries; left until the next change\n");
	} else if (!retry) {
		chip->ae7.vol_fails = 0;
	}
	if (retry && chip->ae7.vol_fails <= 10 && chip->ae7.gate_fails <= 10)
		queue_delayed_work(chip->wq, &chip->hb_work, msecs_to_jiffies(AE7_GATE_RETRY_MS));
	else if (poll)
		ae7_lock_poll(chip);
	else if (stale && !bad)
		creative_ae7_kick(chip);
}

static void creative_ae7_tick(struct creative_chip *chip)
{
	if (!chip->dsp_done || chip->suspended || !chip->corb_buf || creative_ae9_dead(chip))
		return;
	mutex_lock(&chip->ae7.path_mutex);
	ae7_tick(chip);
	mutex_unlock(&chip->ae7.path_mutex);
}

/* "Master Playback Volume": 0 to -90 dB in 0.5 dB steps, the DAC's (48/0f, 48/10) */
static const DECLARE_TLV_DB_SCALE(ae7_vol_tlv, -9000, 50, 0);

static int ae7_vol_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	ui->count = 1;
	ui->value.integer.min = 0;
	ui->value.integer.max = AE7_ATT_MAX;
	return 0;
}

/*
 * Volume per output (kit 11; the AE-9's since 0.12.15): hu_att_out[] holds
 * each output's level, written only where the heartbeat and the gate read
 * it (creative_ae7_want_att()); an output switch never moves a level from
 * one output to the other. "Master Playback Volume" reads and writes
 * hu_att: the level of the output picked (ae7.out), or a write not taken
 * yet. A write is taken AE7_KICK_MS after it, for the output picked then;
 * a switch to the speakers within that time takes it along, a switch to
 * the headphones never does (the write stays the level of the output
 * left). So the headphones' level is only ever a Master value written
 * while they were picked, kept picked until it was taken. Kit 10 stored a
 * write for the output picked at once: alsactl restores Master before
 * "Head Unit Output" (the order below), so a state saved on Speakers put
 * the speakers' level into the headphones' (picked at load) and loaded
 * their -36 dB default for the speakers; a later switch to Headphones
 * played the speakers' level there (2026-11-23, kit 10 reload 1). A
 * script's Master, then Headphones (on the speakers) keeps the level the
 * speakers'. Only Master and the output are saved: after a load the
 * output not picked starts at -36 dB. chip->lock: the puts, the take and
 * the switch, one at a time.
 */
static int ae7_vol_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	unsigned long flags;

	spin_lock_irqsave(&chip->lock, flags);
	uv->value.integer.value[0] = AE7_ATT_MAX - min_t(unsigned int, chip->hu_att, AE7_ATT_MAX);
	spin_unlock_irqrestore(&chip->lock, flags);
	return 0;
}

static int ae7_vol_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	long v = uv->value.integer.value[0];
	unsigned long flags;
	bool changed;

	if (creative_ae9_dead(chip))
		return -ENODEV;
	if (v < 0 || v > AE7_ATT_MAX)
		return -EINVAL;
	spin_lock_irqsave(&chip->lock, flags);
	changed = chip->hu_att != AE7_ATT_MAX - v;
	if (changed) {
		chip->hu_att = AE7_ATT_MAX - v;
		chip->ae7.att_new = true;
		chip->ae7.att_at = jiffies;
	}
	spin_unlock_irqrestore(&chip->lock, flags);
	if (!changed)
		return 0;
	creative_ae7_kick(chip);
	return 1;
}

static int ae7_mute_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	uv->value.integer.value[0] = !((struct creative_chip *)snd_kcontrol_chip(kc))->hu_mute;
	return 0;
}

static int ae7_mute_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	unsigned int mute = !uv->value.integer.value[0];
	unsigned long flags;
	bool changed;

	if (creative_ae9_dead(chip))
		return -ENODEV;
	spin_lock_irqsave(&chip->lock, flags);
	changed = mute != chip->hu_mute;
	chip->hu_mute = mute;
	spin_unlock_irqrestore(&chip->lock, flags);
	if (!changed)
		return 0;
	creative_ae7_kick(chip);
	return 1;
}

/*
 * "Head Unit Output" (the AE-9's control and items): Speakers (the Front
 * jack, line out) / Headphones (the ACM); each keeps its volume (above:
 * a switch loads the new output's level into Master; a Master write not
 * taken yet goes along to the speakers, never to the headphones). Kits 1-7
 * called it "Output Select", an element PipeWire's ACP knows
 * (analog-output.conf.common: options Speakers and "Headphone"): with our
 * "Headphones" unknown to it, ACP set Speakers each time it set up the
 * path (kit 7's card, 2026-11-22)
 */
#define AE7_OUT_CTL		"Head Unit Output"
#define AE7_OUT_SPEAKERS	"Speakers"
#define AE7_OUT_HEADPHONES	"Headphones"

static int ae7_out_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	static const char * const names[] = { AE7_OUT_SPEAKERS, AE7_OUT_HEADPHONES };

	return snd_ctl_enum_info(ui, 1, 2, names);
}

static int ae7_out_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);

	uv->value.enumerated.item[0] = READ_ONCE(chip->ae7.out);
	return 0;
}

static int ae7_out_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	unsigned int v = uv->value.enumerated.item[0], cur;
	unsigned long flags;

	if (creative_ae9_dead(chip))
		return -ENODEV;
	if (v > 1)
		return -EINVAL;
	spin_lock_irqsave(&chip->lock, flags);
	cur = chip->ae7.out;
	if (v != cur) {
		/* a Master write not taken yet: a switch to the headphones
		 * (1) leaves it the level of the output left; one to the
		 * speakers takes it along (alsactl's Master, then Speakers) */
		if (chip->ae7.att_new && v == 1) {
			chip->hu_att_out[cur] = chip->hu_att;
			chip->ae7.att_new = false;
		}
		chip->ae7.out = v;
		if (!chip->ae7.att_new)
			chip->hu_att = chip->hu_att_out[v];
	}
	spin_unlock_irqrestore(&chip->lock, flags);
	if (v == cur)
		return 0;
	if (chip->hu_vol_ctl)
		snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &chip->hu_vol_ctl->id);
	creative_ae7_kick(chip);
	return 1;
}

/* "Headphone Gain": Windows' "Headphone Load" (Low 16-31, Normal 32-149, High 150-600 ohm) */
static int ae7_gain_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	static const char * const names[AE7_GAINS] = { "Low", "Normal", "High" };

	return snd_ctl_enum_info(ui, 1, AE7_GAINS, names);
}

static int ae7_gain_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	uv->value.enumerated.item[0] = ((struct creative_chip *)snd_kcontrol_chip(kc))->ae7.gain;
	return 0;
}

static int ae7_gain_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	unsigned int v = uv->value.enumerated.item[0];

	if (creative_ae9_dead(chip))
		return -ENODEV;
	if (v >= AE7_GAINS)
		return -EINVAL;
	if (v == chip->ae7.gain)
		return 0;
	chip->ae7.gain = v;
	creative_ae7_kick(chip);
	return 1;
}

/* "DAC Filter": the K2M's three (bits 6:5 of register 7) */
static int ae7_filter_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	static const char * const names[AE7_FILTERS] = {
		"Fast Roll Off", "Slow Roll Off", "Minimum Phase",
	};

	return snd_ctl_enum_info(ui, 1, AE7_FILTERS, names);
}

static int ae7_filter_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	uv->value.enumerated.item[0] = ((struct creative_chip *)snd_kcontrol_chip(kc))->ae7.filter;
	return 0;
}

static int ae7_filter_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	unsigned int v = uv->value.enumerated.item[0];

	if (creative_ae9_dead(chip))
		return -ENODEV;
	if (v >= AE7_FILTERS)
		return -EINVAL;
	if (v == chip->ae7.filter)
		return 0;
	chip->ae7.filter = v;
	chip->dac_filter = v;
	chip->dac_filter_bits = ae7_filter_bits[v];
	creative_ae7_kick(chip);
	return 1;
}

/*
 * "Direct Mode Switch" (level 4, ae7=1; the AE-9's control name): on, the
 * next stream plays straight to the DAC at its own rate, stereo; a
 * started stream is restarted on the new path by the heartbeat
 * (creative_ae7_tick()). Off at the first load (DSP mode, the start-up's
 * path); alsactl restores it after.
 */
static int ae7_direct_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	uv->value.integer.value[0] = READ_ONCE(((struct creative_chip *)snd_kcontrol_chip(kc))->ae7.direct_want);
	return 0;
}

static int ae7_direct_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	bool v = !!uv->value.integer.value[0];

	if (creative_ae9_dead(chip))
		return -ENODEV;
	if (v == chip->ae7.direct_want)
		return 0;
	WRITE_ONCE(chip->ae7.direct_want, v);
	dev_info(chip->dev, "AE-7: Direct Mode Switch %s (%s)\n", v ? "on" : "off",
		 chip->ae7.bound ? "the playing stream restarts on it" : "from the next stream");
	creative_ae7_kick(chip);
	return 1;
}

static const struct snd_kcontrol_new ae7_direct_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Direct Mode Switch",
	.info = snd_ctl_boolean_mono_info,
	.get = ae7_direct_get,
	.put = ae7_direct_put,
};

static const struct snd_kcontrol_new ae7_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Master Playback Volume",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
		.info = ae7_vol_info,
		.get = ae7_vol_get,
		.put = ae7_vol_put,
		.tlv.p = ae7_vol_tlv,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Master Playback Switch",
		.info = snd_ctl_boolean_mono_info,
		.get = ae7_mute_get,
		.put = ae7_mute_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = AE7_OUT_CTL,
		.info = ae7_out_info,
		.get = ae7_out_get,
		.put = ae7_out_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Headphone Gain",
		.info = ae7_gain_info,
		.get = ae7_gain_get,
		.put = ae7_gain_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "DAC Filter",
		.info = ae7_filter_info,
		.get = ae7_filter_get,
		.put = ae7_filter_put,
	},
};

/*
 * The AE-7's controls (creative_ae9_hu_controls_add()'s op): Master
 * volume and switch, Head Unit Output, Headphone Gain, DAC Filter, the
 * AE-9's read-only Card Answering and Driver Health, and the jacks'
 * read-only "Card ... Plugged". Nothing of the head unit, SBX, CrystalVoice
 * or the inputs (probe gates those by board).
 */
static void creative_ae7_controls_add(struct creative_chip *chip)
{
	struct snd_kcontrol *kc[ARRAY_SIZE(ae7_controls)];
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ae7_controls); i++) {
		kc[i] = snd_ctl_new1(&ae7_controls[i], chip);
		if (snd_ctl_add(chip->card, kc[i]) < 0)
			kc[i] = NULL;
	}
	chip->hu_vol_ctl = kc[0];
	chip->hu_mute_ctl = kc[1];
	chip->ae7.out_ctl = kc[2];
	chip->ae7.gain_ctl = kc[3];
	chip->ae7.filter_ctl = kc[4];
	if (ae9_ae7_mode >= AE7_MODE_DIRECT) {
		chip->ae7.direct_ctl = snd_ctl_new1(&ae7_direct_control, chip);
		if (snd_ctl_add(chip->card, chip->ae7.direct_ctl) < 0)
			chip->ae7.direct_ctl = NULL;
	}
	chip->answer_ctl = snd_ctl_new1(&answer_control, chip);
	if (snd_ctl_add(chip->card, chip->answer_ctl) < 0)
		chip->answer_ctl = NULL;
	chip->rt_free_true = -1;
	chip->health_ctl = snd_ctl_new1(&health_control, chip);
	if (snd_ctl_add(chip->card, chip->health_ctl) < 0)
		chip->health_ctl = NULL;
	ae7_plug_controls_add(chip);
}

/* ------------------------------------------------------------------ */
/* Plug reporting (kit 10)						  */
/* ------------------------------------------------------------------ */

/*
 * One unsolicited RIRB entry (the unsol op: rirb_unsol(), corb_mutex held,
 * so no verb from here): codec 1's, its tag in bits 31:26 (Windows got
 * 0x0a200001, tag 2, and 0x06400001, tag 1, on plugs, W3 part 1; the DSP's
 * reply notices 0x02c00001 are tag 0). A jack's tag marks its pins for
 * plug_work; anything else is left alone.
 */
static void creative_ae7_unsol(struct creative_chip *chip, u32 res, u32 fl)
{
	unsigned int tag = res >> 26, bits = 0, i;

	if ((fl & 0xf) != 1)
		return;
	for (i = 0; i < AE7_PLUGS; i++)
		if (ae7_jacks[i].tag == tag)
			bits |= BIT(i);
	if (!bits)
		return;
	chip->ae7.plug_events++;
	chip->ae7.plug_last = res;
	atomic_or(bits, &chip->ae7.plug_pending);
	ae7_plug_kick(chip);
}

/*
 * A RIRB response interrupt (the rirb_irq op, hard IRQ). Only a verb's
 * sender reads the ring, and an idle AE-7 sends none (no polling, as
 * Windows), so a jack's unsolicited entry would wait there for the next
 * verb: plug_work scans it.
 */
static void creative_ae7_rirb_irq(struct creative_chip *chip)
{
	ae7_plug_kick(chip);
}

/*
 * The pending jacks' pins read (GET_PIN_SENSE bit 31; no trigger needed:
 * pin caps bit 1 clear on all four) and each change reported: the
 * control's value and event, one log line. Nothing else follows a plug: no
 * output, pin, switch-chip or GPIO write (Windows answers tag 2 with its
 * headset probe, which changes nothing used here, and tag 1 with nothing).
 * A read that fails keeps its jack pending for the next run.
 */
static void ae7_plugs_read(struct creative_chip *chip)
{
	unsigned int bits = atomic_xchg(&chip->ae7.plug_pending, 0), i;

	for (i = 0; i < AE7_PLUGS; i++) {
		struct snd_kcontrol *kc;
		bool now, first;
		u32 r = 0;
		int ret;

		if (!(bits & BIT(i)))
			continue;
		mutex_lock(&chip->replay_mutex);
		ret = creative_ae9_corb_send_verb_sync(chip,
			creative_ae9_verb(1, ae7_jacks[i].nid, AC_VERB_GET_PIN_SENSE, 0), &r);
		mutex_unlock(&chip->replay_mutex);
		if (ret < 0) {
			atomic_or(BIT(i), &chip->ae7.plug_pending);
			continue;
		}
		chip->ae7.plug_reads++;
		now = r & AC_PINSENSE_PRESENCE;
		first = !(chip->ae7.plugs_read & BIT(i));
		if (!first && now == !!(chip->ae7.plugs & BIT(i)))
			continue;
		chip->ae7.plugs_read |= BIT(i);
		if (now)
			chip->ae7.plugs |= BIT(i);
		else
			chip->ae7.plugs &= ~BIT(i);
		dev_info(chip->dev, "AE-7: card %s jack: %s\n", ae7_jacks[i].name,
			 first ? (now ? "plugged (at load)" : "empty (at load)") :
			 now ? "plugged" : "unplugged");
		kc = READ_ONCE(chip->ae7.plug_ctl[i]);
		if (kc)
			snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &kc->id);
	}
}

/* plug_work: the ring scanned (no verb sent), then the pending jacks read */
static void ae7_plug_scan(struct creative_chip *chip)
{
	if (!READ_ONCE(chip->ae7.plug_armed) || READ_ONCE(chip->unloading) || !chip->dsp_done ||
	    chip->suspended || !chip->corb_buf || creative_ae9_dead(chip))
		return;
	creative_ae9_corb_scan_unsol(chip);
	if (atomic_read(&chip->ae7.plug_pending))
		ae7_plugs_read(chip);
}

static void ae7_plug_work(struct work_struct *w)
{
	ae7_plug_scan(container_of(to_delayed_work(w), struct creative_chip, ae7.plug_work));
}

/* ae9_cancel_timed_works()'s op (suspend, shutdown, unload, a failed probe) */
static void creative_ae7_cancel_works(struct creative_chip *chip)
{
	if (!chip->ae7.inited)
		return;
	WRITE_ONCE(chip->ae7.plug_armed, false);
	cancel_delayed_work_sync(&chip->ae7.plug_work);
}

/* "Card ... Plugged" (read-only): the jack's pin sensed a plug at the last read */
static int ae7_plug_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);

	uv->value.integer.value[0] = !!(READ_ONCE(chip->ae7.plugs) & BIT(kc->private_value));
	return 0;
}

static void ae7_plug_controls_add(struct creative_chip *chip)
{
	unsigned int i;

	for (i = 0; i < AE7_PLUGS; i++) {
		const struct snd_kcontrol_new kn = {
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
			.name = ae7_jacks[i].ctl,
			.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
			.info = snd_ctl_boolean_mono_info,
			.get = ae7_plug_get,
			.private_value = i,
		};
		struct snd_kcontrol *kc = snd_ctl_new1(&kn, chip);

		if (snd_ctl_add(chip->card, kc) < 0)
			kc = NULL;
		WRITE_ONCE(chip->ae7.plug_ctl[i], kc);
	}
}

/* debugfs state: the jacks as last read, the events */
static void ae7_plugs_show(struct seq_file *m, struct creative_chip *chip)
{
	unsigned int i;

	seq_puts(m, "AE-7 jacks:");
	for (i = 0; i < AE7_PLUGS; i++)
		seq_printf(m, "%s %s %s", i ? "," : "", ae7_jacks[i].name,
			   !(chip->ae7.plugs_read & BIT(i)) ? "not read" :
			   chip->ae7.plugs & BIT(i) ? "plugged" : "empty");
	seq_printf(m, " (Center / Subwoofer: no sense); %u jack events (last 0x%08x), %u reads%s\n",
		   chip->ae7.plug_events, chip->ae7.plug_last, chip->ae7.plug_reads,
		   chip->ae7.plug_armed ? "" : "; not armed");
}

/* ------------------------------------------------------------------ */
/* Lifecycle								  */
/* ------------------------------------------------------------------ */

/* a stream descriptor's DMA and its interrupt off, its status cleared */
static void ae7_sd_off(struct creative_chip *chip, unsigned int base)
{
	unsigned long flags;
	u32 ctl;

	ctl = readl(chip->hda_regs + base + AZX_REG_SD_CTL);
	writel((ctl & ~SD_CTL_DMA_START) & SD_CTL_WMASK, chip->hda_regs + base + AZX_REG_SD_CTL);
	spin_lock_irqsave(&chip->lock, flags);
	writel(readl(chip->hda_regs + AZX_REG_INTCTL) & ~BIT((base - 0x80) / 0x20),
	       chip->hda_regs + AZX_REG_INTCTL);
	spin_unlock_irqrestore(&chip->lock, flags);
	writeb(SD_INT_DESC_ERR | SD_INT_FIFO_ERR | SD_INT_COMPLETE,
	       chip->hda_regs + base + AZX_REG_SD_STS);
}

/*
 * Before a power-down or a sleep (creative_ae9_streams_down()'s op): the
 * playback stream's DMA and its interrupt off (no period interrupt can
 * reach a handler that no longer acknowledges it), the start's stop
 * (converter 0x02 unbound; in Direct Mode stream 0x14 stopped); a
 * recording's the same way (kit 10: input stream 0, then Windows' record
 * stop, ADC 0x07 unbound; the AE-9's part of this step); kit 11: then
 * Voice Clarity's route out and the mic connections back at 96 kHz
 * (creative_ae9_cv_down(), as the AE-9's step). The DSP streams are the
 * power-down's (Windows' stop).
 */
static void creative_ae7_streams_down(struct creative_chip *chip)
{
	unsigned int base = chip->playback_stream_base;

	if (base)
		ae7_sd_off(chip, base);
	creative_ae7_play_stop(chip);
	chip->pcm_start_sent = 0;
	if (chip->cap_start_sent) {
		ae7_sd_off(chip, AE7_CAP_SD);
		creative_ae9_capture_seq(chip, false);
		chip->cap_start_sent = 0;
	}
	creative_ae9_cv_down(chip);
}

/*
 * Suspend (the quiesce op, before the rings go): Windows' D3, the same
 * stop as a restart's (the DAC muted, the switch chip off, the LED and the
 * headphone amp off, the pins, the AFG to D3hot). The resume brings the
 * card up from the start, from resume_work (not in the PM callback), and
 * its start re-applies output, volume, gain and filter. Untested on an
 * AE-7 (docs/dev/AE7.md).
 */
static void creative_ae7_quiesce(struct creative_chip *chip)
{
	if (chip->corb_buf && chip->dsp_done && !creative_ae9_dead(chip))
		creative_ae7_power_down(chip, false);
}

/*
 * resume_work() after a good bring-up: the start already re-applied the
 * output side; the mic's boost and level as the AE-9's resume sends them
 * (the start-up set Windows' boot values: step 1, +3 dB; the next
 * recording's level block then ends at the user's level); kit 11:
 * CrystalVoice as the AE-9's resume (the start-up sent its boot table,
 * switches off; resume_work() marked the presets unsent and the route out)
 */
static void creative_ae7_resumed(struct creative_chip *chip)
{
	creative_ae9_mic_boost_apply(chip);
	if (chip->mic_level != 99)
		creative_ae9_mic_level_apply(chip);
	if (chip->cv.on)
		creative_ae9_cv_apply(chip);	/* takes path_mutex (the fx_lock op) */
	snd_power_change_state(chip->card, SNDRV_CTL_POWER_D0);
	dev_info(chip->dev, "AE-7: resume done, output %s, volume att 0x%02x%s, DAC %s\n",
		 chip->hu_output ? "headphones" : "speakers", chip->hu_att,
		 chip->hu_mute ? " (muted)" : "", chip->ae7.unmuted ? "unmuted" : "still muted");
}

/*
 * debugfs state, kit 11: CrystalVoice as the controls hold it (the Mic EQ
 * and Voice Morphing items by number: dsp.c has the names), whether an
 * effect routes the mic through the voice processor (16 kHz connections),
 * the route as last set, the presets the DSP holds (-1: sent again at the
 * next apply)
 */
static void ae7_cv_show(struct seq_file *m, struct creative_chip *chip)
{
	const struct creative_ae9_cv *cv = &chip->cv;

	seq_printf(m, "AE-7 CrystalVoice: Voice Clarity %s (Echo Cancellation %d, Noise Reduction %d, Smart Volume %d), Mic EQ item %u, Voice Morphing item %u; %s; voice processor %s the mic route; DSP holds Mic EQ %d, Voice Morphing %d\n",
		   cv->on ? "on" : "off", cv->fx[0], cv->fx[1], cv->fx[2], cv->miceq, cv->voicefx,
		   AE9_CV_WANT(chip) ? "an effect on (recordings at 48 kHz, 8 kHz band)" : "no effect on",
		   cv->route_on ? "in" : "out of", cv->sent_miceq, cv->sent_voicefx);
}

/*
 * debugfs state: the router remap, the last walk's count, and stream
 * 0x0c's slots as the card holds them now (its list walked again, each
 * slot read once), against the map in effect (Windows', or ae7_lanes=)
 */
static void creative_ae7_remap_show(struct seq_file *m, struct creative_chip *chip)
{
	const u8 *want = chip->ae7.lane_port;
	bool custom = memcmp(want, ae7_lane_win, AE7_REMAP_SLOTS) != 0;
	unsigned int k, same = 0;
	int s;

	seq_printf(m, "AE-7 router remap: %u of %u slots at the last walk (head 0x%02x, %u ms after the stream's start; %u walk(s) since load); stream 0x0c now:",
		   chip->ae7.remapped, AE7_REMAP_SLOTS, chip->ae7.remap_head,
		   chip->ae7.remap_ms, chip->ae7.remap_walks);
	s = ae9_8051_xram_byte(chip, AE7_XRAM_PROC_HEAD);
	for (k = 0; k <= AE7_REMAP_SLOTS && s >= 0 && s < AE7_RT_SLOTS; k++) {
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
		seq_printf(m, " %02x:%02x->%02x", s, v & 0xff, (v >> 8) & 0xff);
		if (k < AE7_REMAP_SLOTS && (v & 0xff) == 0xc0 + k && ((v >> 8) & 0xff) == want[k])
			same++;
		s = ae9_8051_xram_byte(chip, AE7_XRAM_SLOT_NEXT + s);
	}
	if (!k)
		seq_printf(m, s < 0 ? " head unread" : " no list (head 0x%02x)", s);
	if (!custom)
		seq_printf(m, "  %s\n", same == AE7_REMAP_SLOTS ? "as Windows" :
			   "NOT as Windows (c0..cb -> e0 e1 e4 e5 e2 e3 e8 e9 ec ed ea eb)");
	else
		seq_printf(m, "  %s (ae7_lanes=%02x,%02x,%02x,%02x,%02x,%02x; Windows': e0,e1,e4,e5,e2,e3)\n",
			   same == AE7_REMAP_SLOTS ? "custom" : "NOT as asked, custom",
			   want[0], want[1], want[2], want[3], want[4], want[5]);
}

/* a DAC read-back for debugfs state: hex, "-" when not read (or not answered) */
static const char *ae7_rb(char *buf, size_t n, int v)
{
	if (v < 0)
		return "-";
	snprintf(buf, n, "0x%02x", v & 0xff);
	return buf;
}

/*
 * debugfs state: codec 1's output pins as the card holds them now, each
 * read once (codec 1, GET_PIN_WIDGET_CONTROL, under replay_mutex): Windows' sets,
 * speakers 0x0b 0x40, 0x0f 0xc0, 0x10 0xc0 (the C/Sub jack's pin on since
 * kit 8), headphones 0x00, 0x00, 0xc0
 */
static void creative_ae7_pins_show(struct seq_file *m, struct creative_chip *chip)
{
	static const u8 nid[] = { 0x0b, 0x0f, 0x10 };
	unsigned int i;

	seq_puts(m, "AE-7 pins (read now):");
	for (i = 0; i < ARRAY_SIZE(nid); i++) {
		u32 r = 0;
		int ret = -ENODEV;

		if (chip->corb_buf && !creative_ae9_dead(chip)) {
			mutex_lock(&chip->replay_mutex);
			ret = creative_ae9_corb_send_verb_sync(chip,
				creative_ae9_verb(1, nid[i], AC_VERB_GET_PIN_WIDGET_CONTROL, 0), &r);
			mutex_unlock(&chip->replay_mutex);
		}
		if (ret < 0)
			seq_printf(m, " 0x%02x -", nid[i]);
		else
			seq_printf(m, " 0x%02x 0x%02x", nid[i], r & 0xff);
	}
	seq_puts(m, " (0x0b Front, 0x0f Rear, 0x10 C/Sub; Windows: speakers 0x40 0xc0 0xc0, headphones 0x00 0x00 0xc0)\n");
}

/* debugfs state: the AE-7 line */
static void creative_ae7_state_show(struct seq_file *m, struct creative_chip *chip)
{
	char b01[8], b0a[8], b0b[8], b40[8], p43[12], clk[12], rate[12], tx[12];

	static const char * const gain[] = { "Low", "Normal", "High" };
	static const char * const filt[] = { "Fast", "Slow", "Minimum Phase" };

	seq_printf(m, "board: Sound Blaster AE-7 (experimental, ae7=%d, level %d), BAR2+0x4 0x%08x, BAR2+0x8 0x%x (DAC row %u)\n",
		   ae9_ae7_opt, ae9_ae7_mode, chip->bar2_id[2], chip->ae7.bar2_rev, chip->ae7.thd_row);
	seq_printf(m, "AE-7: output %s (sent %d, pins %d), gain %s (sent %d), filter %s (sent %d), volume att 0x%02x%s (sent %d; speakers 0x%02x, headphones 0x%02x%s), DAC %s (gate refusals %u)%s, selects %u\n",
		   chip->hu_output ? "headphones" : "speakers", chip->ae7.out_sent,
		   chip->ae7.pins_state, gain[chip->ae7.gain % AE7_GAINS], chip->ae7.gain_sent,
		   filt[chip->ae7.filter % AE7_FILTERS], chip->ae7.filter_sent, chip->hu_att,
		   chip->hu_mute ? " muted" : "", chip->ae7.att_sent, chip->hu_att_out[0],
		   chip->hu_att_out[1], chip->ae7.att_new ? "; a Master write pending" : "",
		   chip->ae7.unmuted ? "unmuted" : "MUTED", chip->ae7.gate_blocks,
		   chip->ae7.amp_hold ? ", headphone amp HELD OFF" : "", chip->ae7.selects);
	seq_printf(m, "AE-7 path: %s, clocks at %u Hz, ASI class 0x%02x, %s; Direct Mode Switch %s; playback %u ch (DSP path's last %u ch); stream 0x05 slots %u of %u, stream 0x14 %s; DAC 48/01 %s 48/0a %s 48/0b %s 48/40 %s (last read-backs); switches %u, failed %u\n",
		   chip->ae7.direct ? "Direct Mode" : "DSP mode", chip->ae7.path_rate,
		   chip->ae7.asi_class, chip->ae7.path_ok ? "confirmed" : "NOT confirmed",
		   ae9_ae7_mode < AE7_MODE_DIRECT ? "n/a (a test level)" :
		   chip->ae7.direct_want ? "on" : "off",
		   chip->pcm_start_sent ? (chip->pcm_start_fmt & 0xf) + 1U : chip->ae7.ch,
		   chip->ae7.dsp_ch, chip->ae7.walked,
		   chip->ae7.walk_want, chip->ae7.s14 ? "on" : "off",
		   ae7_rb(b01, sizeof(b01), chip->ae7.dac01), ae7_rb(b0a, sizeof(b0a), chip->ae7.dac0a),
		   ae7_rb(b0b, sizeof(b0b), chip->ae7.dac0b), ae7_rb(b40, sizeof(b40), chip->ae7.dac40),
		   chip->ae7.path_switches, chip->ae7.path_fails);
	seq_printf(m, "AE-7 output block: %s (last: %s; %u since load)%s%s; Speaker Config code 0x80/0x04 per item 2.0 8, 2.1 2, 4.0 5, 4.1 6, 5.1 8\n",
		   !chip->ae7.fx_armed ? "not armed yet" :
		   chip->ae7.block_sent ? "sent" : "NOT sent (the gate waits in DSP mode)",
		   chip->ae7.block_kind ? chip->ae7.block_kind : "none", chip->ae7.blocks,
		   chip->ae7.fx_pending ? ", a bracket pending" : "",
		   chip->ae7.cal_pending ? ", the speakers' levels and delays pending" : "");
	seq_printf(m, "AE-7 Rear/C-Sub converters 0x03/0x04: %s, level amp 0x%02x fine 0x%02x%s\n",
		   chip->ae7.conv_on ? "unmuted" :
		   chip->hu_output ? "muted (headphones)" : "muted (the DAC's gate)",
		   chip->ae7.conv_amp, chip->ae7.conv_fine,
		   chip->ae7.conv_mute ? " (muted: master switch or -90 dB)" : "");
	seq_printf(m, "AE-7 lock (Direct Mode): %s; last wait %u ms; waits %u (%u past %u ms)\n",
		   !chip->ae7.direct ? "n/a (DSP mode)" :
		   chip->ae7.lock_wait ? "WAITING" : "not waiting", chip->ae7.lock_ms,
		   chip->ae7.lock_waits, chip->ae7.lock_late, AE7_LOCK_WAIT_MS);
	seq_printf(m, "AE-7 clocks: PLL/PMU 0x43 %s, I2S 0x189000 %s 0x189024 %s, S/PDIF transmitter 0x18a020 %s (last read-backs)%s\n",
		   ae7_rbw(p43, sizeof(p43), chip->ae7.pin43), ae7_rbw(clk, sizeof(clk), chip->ae7.i2s_clk),
		   ae7_rbw(rate, sizeof(rate), chip->ae7.i2s_rate), ae7_rbw(tx, sizeof(tx), chip->ae7.tx_ctl),
		   chip->ae7.spdif_reclock ? "; the next S/PDIF start re-clocks the I2S first" : "");
	creative_ae7_pins_show(m, chip);
	ae7_plugs_show(m, chip);
	creative_ae7_remap_show(m, chip);
	ae7_cv_show(m, chip);
}

/* ------------------------------------------------------------------ */
/* The boards							  */
/* ------------------------------------------------------------------ */

static const struct ae9_board_ops ae7_ops = {
	.bringup_check	= creative_ae7_bringup_check,
	.codecs_check	= creative_ae7_codecs_check,
	.bar2_init	= creative_ae7_bar2_init,
	.predownload	= creative_ae7_predownload,
	.post_fw	= creative_ae7_post_fw,
	.power_down	= creative_ae7_power_down,
	.streams_down	= creative_ae7_streams_down,
	.quiesce	= creative_ae7_quiesce,
	.tick		= creative_ae7_tick,
	.controls_add	= creative_ae7_controls_add,
	.pcm_init	= creative_ae7_pcm_init,
	.resumed	= creative_ae7_resumed,
	.state_show	= creative_ae7_state_show,
	.fx_out_block	= creative_ae7_fx_out_block,
	.fx_out_done	= creative_ae7_fx_out_done,
	.fx_lock	= creative_ae7_fx_lock,
	.probe_done	= creative_ae7_probe_done,
	.unsol		= creative_ae7_unsol,
	.rirb_irq	= creative_ae7_rirb_irq,
	.cancel_works	= creative_ae7_cancel_works,
};

/* the AE-9: no ops, every capability (the code as in a release build) */
const struct ae9_board ae9_board_ae9 = {
	.subsys = CREATIVE_SUBSYS_AE9,
	.name = "Sound Blaster AE-9",
	.caps = AE9_CAP_ALL,
	.bar2_id = AE9_BOARD_ID_AE9,
};

/*
 * Later phases (caps off, none of it reachable now); what each needs, from
 * the Windows driver's AE-7 paths (project notes, not published) and its
 * predictions for the paths no capture has yet (kit 10's: recording from
 * the Line-in / Mic-in jack, AE9_CAP_INPUTS, and What U Hear):
 * Phase 2 (Direct Mode, its rates) is in, at level 4 and without
 *	AE9_CAP_DIRECT (that cap turns on the AE-9's fx.direct readers): the
 *	AE-7's own path state is ae7.direct, debugfs state's "AE-7 path" line.
 * TODO(ae7 P4): DSD64, AE9_CAP_DSD: the AE-9's I2S-wrapper path with 48/01 =
 *	0x83 and 30/2b = 0x3f in place of GPIO pin 1 (report 4.9 DSD, P8).
 * TODO(ae7 P3): Line-In on the combo jack ("Multipurpose Input Select"):
 *	30/28 = 0x3f, pin 0x11 -> ADC 0x08 (P10; W3.5/3.6 not captured);
 *	optical out, codec 1 converter 0x05 -> pin 0x0c, AE9_CAP_OPTICAL (P12).
 * Kit 10: SBX, the EQ, the headphone profiles and Speaker Config on the
 *	AE-9's controls (AE9_CAP_SBX; creative_ae7_fx.c: the output block and
 *	the select's preset in every bracket); the Rear and C/Sub converters
 *	behind the DAC's gate. Not yet: AE9_CAP_SURROUND (the jack map, L5),
 *	AE9_CAP_SPK_SETUP (Bass Redirect, speaker levels and distances: not
 *	captured).
 * Kit 11: CrystalVoice on the AE-9's controls and code (AE9_CAP_CV; its
 *	boot table in creative_ae7_tail()). Not yet: AE9_CAP_MIC_MON (the
 *	DSP's mic monitor, 0x80/0x0d: Windows' AE-7 "listen" is a host loop,
 *	the DSP monitor never captured).
 * TODO(ae7 P3): an LED control (GPIO pin 1, P17); the pin 0x11 headset probe (4.11, P15).
 */

/*
 * The AE-7 (phases 1 and 2, kit 10): DSP-mode playback on headphones and
 * the Front jack; Direct Mode with its rates (level 4); SBX / EQ /
 * headphone profiles / Speaker Config (AE9_CAP_SBX); recording from the
 * Line-in / Mic-in jack and What U Hear (AE9_CAP_INPUTS: "Capture Volume",
 * "Mic Gain Capture Volume" up to +20 dB); CrystalVoice (AE9_CAP_CV, kit
 * 11). Not yet: DSD64, Line-In, optical out, the mic monitor; their caps
 * stay off (docs/dev/AE7.md lists what each needs). AE7_BOARD_CAPS
 * (creative_ae9_board.h): tools/tablesc's glue uses the same.
 */
const struct ae9_board ae9_board_ae7 = {
	.subsys = CREATIVE_SUBSYS_AE7,
	.name = "Sound Blaster AE-7",
	.caps = AE7_BOARD_CAPS,
	.bar2_id = CREATIVE_SUBSYS_AE7,
	.ops = &ae7_ops,
};
