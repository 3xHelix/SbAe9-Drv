// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 3xHelix
//
// Sound Blaster AE-7 (experimental): the card's BAR2 init,
// the output select, the levels, the DAC's mute gate and the shutdown, on
// creative_ae9_seq_run.c's senders (a block per run of verbs under
// replay_mutex, each DAC or switch-chip command its own list). #included at
// the end of creative_ae9_seq_run.c.
//
// References: the AE-7's Windows captures (C2B1: capture 2's cold boot,
// headphones; C1B3: capture 1's third bring-up, speakers; the restart
// shutdown at the end of C2B1) as tools/ae7golden has them.
//

#include "creative_ae7.h"

/* ------------------------------------------------------------------ */
/* BAR2 at start-up							  */
/* ------------------------------------------------------------------ */

/*
 * Windows' BAR2 init on the AE-7 (C2B1 bar2_init, its first 38 writes):
 * creative_ae9_bar2_init_windows()'s writes with BAR2 0x1c = 0x880680 in
 * its three places (the AE-9's 0x880480 with bit 9, "SDI2 off", set: the
 * AE-7 has one codec), the UART set up as on the AE-9 (configured, never
 * used: the ACM's knob is analog). Each write read back, as the AE-9's.
 */
void creative_ae7_bar2_init(struct creative_chip *chip)
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
	bar2_init_w(chip, AE9_BAR2_CTL, AE7_BAR2_CTL_1C);

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

	bar2_init_w(chip, AE9_BAR2_CTL, AE7_BAR2_CTL_1C & 0xffff);
	bar2_init_w(chip, AE9_BAR2_CTL, AE7_BAR2_CTL_1C);
	dev_info(chip->dev, "AE-7: BAR2 init done, BAR2+0x4=0x%08x (Windows: 0x00810009), 0x1c=0x%08x (Windows: 0x00880680)\n",
		 creative_ext_readl(chip, AE9_BAR2_ID), creative_ext_readl(chip, AE9_BAR2_CTL));
}

/* ------------------------------------------------------------------ */
/* The output select							  */
/* ------------------------------------------------------------------ */

/*
 * "Headphone Gain" = the DAC's master trim (48/11-14, low byte first):
 * Low -8 dB, Normal -4.5 dB, High 0 dB (Windows' "Headphone Load" Low /
 * Normal / High; C2B1 writes Low, then its saved Normal). The speakers
 * always get 0 dB.
 */
const u32 ae7_gain_trim[AE7_GAINS] = { 0x32f52cff, 0x4c3ea838, 0x7fffffff };
#define AE7_TRIM_0DB		0x7fffffff

/* "DAC Filter": K2M register 7, bit 7 set, bits 6:5 = 0 fast, 1 slow, 2 minimum phase */
const u8 ae7_filter_bits[AE7_FILTERS] = { 0x80, 0xa0, 0xc0 };

/*
 * The THD compensation's C3 high byte (48/19) per output, by the DAC row
 * BAR2+8 bits 4:2 pick (Windows' table; rows 3-7 = row 0). C2B1's card
 * (BAR2+8 = 0x8) is row 2: headphones 0x02, speakers 0x00 (C1B3).
 */
static const u8 ae7_thd_c3[3][2] = {
	{ 0xff, 0x02 }, { 0x01, 0x01 }, { 0x00, 0x02 },
};

/* the THD block: 48/0d (compensation on), C2 (0x16/0x17) and C3's low byte 0, C3's high byte */
static int ae7_thd(struct creative_chip *chip, bool hp)
{
	u8 row = chip->ae7.thd_row < ARRAY_SIZE(ae7_thd_c3) ? chip->ae7.thd_row : 0;
	int bad;

	bad = ae7_dac(chip, AE7_DAC, K2M_THD_BYPASS, 0x00);
	bad += ae7_dac(chip, AE7_DAC, K2M_THD_C2, 0x00);
	bad += ae7_dac(chip, AE7_DAC, K2M_THD_C2 + 1, 0x00);
	bad += ae7_dac(chip, AE7_DAC, K2M_THD_C3, 0x00);
	bad += ae7_dac(chip, AE7_DAC, K2M_THD_C3 + 1, ae7_thd_c3[row][hp]);
	return bad;
}

/* the master trim, low byte first */
static int ae7_trim(struct creative_chip *chip, u32 trim)
{
	int bad = 0;
	unsigned int i;

	for (i = 0; i < 4; i++)
		bad += ae7_dac(chip, AE7_DAC, K2M_MASTER_TRIM + i, (trim >> (8 * i)) & 0xff);
	return bad;
}

/* one GPIO pin, as the AE-9's hu_gpio() */
static void ae7_gpio(struct creative_chip *chip, u8 pin, bool on)
{
	hu_gpio(chip, pin, on);
}

/*
 * A block of the select's (and of what it calls: the pin loop, the
 * converters' mute) with replay_mutex taken here, or (locked) already held
 * by this caller (the fx bracket's preset, creative_ae7_preset_held()):
 * the same verbs and log line either way. locked is passed down, never
 * kept in the chip: a shutdown running beside a bracket takes the mutex.
 */
static void ae7_blk_begin(struct ae9_blk *b, struct creative_chip *chip, bool locked)
{
	if (!locked) {
		blk_begin(b, chip);
		return;
	}
	lockdep_assert_held(&chip->replay_mutex);
	*b = (struct ae9_blk){ .chip = chip };
}

static void ae7_blk_end(struct ae9_blk *b, const char *what, bool locked)
{
	struct creative_chip *chip = b->chip;

	if (!locked) {
		blk_end(b, what);
		return;
	}
	creative_dsp_chipio_addr_invalidate();
	if (b->errs || b->msg_err || !chip->dsp_done)
		dev_info(chip->dev, "%s, %u verbs (%u errors), %u SCP msgs (%u failed)\n",
			 what, b->verbs, b->errs, b->msgs, b->msg_err);
	else
		ae9_dbg(chip, "%s, %u verbs (%u errors), %u SCP msgs (%u failed)\n",
			what, b->verbs, b->errs, b->msgs, b->msg_err);
}

/* a pin's control read (Windows reads it before each write: answer not used), then written */
static void ae7_blk_pin(struct ae9_blk *b, u8 nid, u8 ctl)
{
	blk_send(b, nid, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	blk_send(b, nid, AC_VERB_SET_PIN_WIDGET_CONTROL, ctl);
}

/*
 * Windows' pin loop for an output (its pin-apply, each pin only when its
 * state changes; Windows tracks all three together): headphones = pin
 * 0x0b's EAPD 0 and pin off, 0x0f off, the switch chip's 0x32 = 0, 0x10
 * out with its headphone amp; speakers = 0x0b EAPD 2 and out, 0x0f out
 * with its headphone amp, 0x32 = 0x3f, 0x10 out with its headphone amp
 * too. 0x32 goes between pins 0x0f and 0x10 (C2: mmio 43018 < 43046 <
 * 43059). Pin 0x10 (the C/Sub jack with 0x32 = 0x3f) is 0xc0 on both
 * outputs: capture A's speaker select (02:36:27.461) reads it 0xc0 and
 * writes 0xc0, its headphone select (02:37:28.234) the same, capture Bw's
 * headphone selects too; it goes to 0 only at the stop. (Kits 1-7 wrote 0
 * on the speakers: the decompile's reading, refuted by capture A.)
 */
static void ae7_pins(struct creative_chip *chip, bool hp, const char *what, bool locked)
{
	struct ae9_blk b;

	if (chip->ae7.pins_state == hp)
		return;
	ae7_blk_begin(&b, chip, locked);
	blk_send(&b, 0x0b, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	blk_send(&b, 0x0b, AC_VERB_SET_EAPD_BTLENABLE, hp ? 0x00 : 0x02);
	blk_send(&b, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, hp ? 0 : AC_PINCTL_OUT_EN);
	ae7_blk_pin(&b, 0x0f, hp ? 0 : AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN);
	ae7_blk_end(&b, what, locked);
	ae7_dac(chip, AE7_SW, SW_MUX_OUT, hp ? 0 : AE7_SW_ON);
	ae7_blk_begin(&b, chip, locked);
	ae7_blk_pin(&b, 0x10, AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN);
	ae7_blk_end(&b, what, locked);
	chip->ae7.pins_state = hp;
}

/*
 * The Rear and C/Sub jacks' converters (0x03, 0x04: the CA0132's own DACs)
 * play only while the front DAC plays and the output is the speakers. They
 * are muted (amp bit 7, their level kept) before every 48/07 mute (an output
 * switch, a path switch, the shutdown, a held headphone amp: ae7_conv_mute()),
 * every level written while gated carries the mute bit
 * (creative_ae7_volume()), and only the gate unmutes them, after 48/07
 * (creative_ae7_unmute(): ae7_conv_levels()). On the headphones they stay
 * muted: Windows keeps them at the headphones' level there (W2), but pin
 * 0x10 drives the C/Sub jack on both outputs, so a speaker left on it
 * would play the DSP's C/Sub lane while the headphones play (a deliberate
 * difference from Windows).
 */
static bool ae7_conv_gated(struct creative_chip *chip)
{
	return !chip->ae7.unmuted || chip->hu_output == 1;
}

/* both converters, both channels muted at their level; returns the verbs not answered */
static int ae7_conv_mute(struct creative_chip *chip, bool locked)
{
	struct ae9_blk b;
	u8 nid;

	ae7_blk_begin(&b, chip, locked);
	for (nid = 0x03; nid <= 0x04; nid++) {
		blk_send(&b, nid, AC_VERB_SET_AMP_GAIN_MUTE, AC_AMP_SET_OUTPUT | AC_AMP_SET_LEFT |
			 AC_AMP_MUTE | chip->ae7.conv_amp);
		blk_send(&b, nid, AC_VERB_SET_AMP_GAIN_MUTE, AC_AMP_SET_OUTPUT | AC_AMP_SET_RIGHT |
			 AC_AMP_MUTE | chip->ae7.conv_amp);
	}
	ae7_blk_end(&b, "AE-7: Rear/C-Sub converters muted", locked);
	chip->ae7.conv_on = false;
	return b.errs;
}

int creative_ae7_conv_mute(struct creative_chip *chip)
{
	return ae7_conv_mute(chip, false);
}

/*
 * GPIO pin 0 high (the headphone amp on), only with the DAC's mute
 * confirmed: 48/07 read back checked, bits 1:0 = 3. The ES9018K2M powers up
 * unmuted at 0 dB; an amp switched on in front of a DAC whose mute (or
 * volume) wasn't acknowledged could play at full level. The start-up's
 * routing turns the amp on after the DAC init (its mute just written); a
 * held amp (ae7.amp_hold) is muted again first, then checked (the output
 * selects: creative_ae7_select()). Unconfirmed: the amp stays off,
 * amp_hold set, a warning, -EIO. 0 = the amp on. locked: replay_mutex
 * held by this caller (the bracket's preset).
 */
static int ae7_amp_on(struct creative_chip *chip, bool locked)
{
	u32 raw[2];
	int v;

	if (chip->ae7.amp_hold) {
		chip->ae7.unmuted = false;
		ae7_conv_mute(chip, locked);
		ae7_dac07(chip, 0, K2M_MUTE);
	}
	v = creative_dsp_ca0113_read_checked(chip, AE7_DAC, K2M_FILTER_MUTE, raw);
	if (v < 0 || (v & K2M_MUTE) != K2M_MUTE) {
		if (!chip->ae7.amp_hold && v < 0)
			dev_warn(chip->dev, "AE-7: DAC mute not confirmed (48/07 not read back, %d): headphone amp left off\n",
				 v);
		else if (!chip->ae7.amp_hold)
			dev_warn(chip->dev, "AE-7: DAC mute not confirmed (48/07 reads 0x%02x): headphone amp left off\n",
				 v);
		chip->ae7.amp_hold = true;
		return -EIO;
	}
	if (chip->ae7.amp_hold)
		dev_info(chip->dev, "AE-7: DAC mute confirmed (48/07 0x%02x): headphone amp on\n", v);
	chip->ae7.amp_hold = false;
	ae7_gpio(chip, AE7_GPIO_AMP, true);
	return 0;
}

int creative_ae7_amp_on(struct creative_chip *chip)
{
	return ae7_amp_on(chip, false);
}

/* the select's amp step: on as it is, a held amp only through the check (1: still held) */
static int ae7_amp(struct creative_chip *chip, bool locked)
{
	if (chip->ae7.amp_hold)
		return ae7_amp_on(chip, locked) ? 1 : 0;
	ae7_gpio(chip, AE7_GPIO_AMP, true);
	return 0;
}

/* a ChipIO register read in a block (its value; unread: fallback) */
static u32 ae7_blk_chipio_value(struct ae9_blk *b, u32 addr, u32 fallback)
{
	unsigned int v = fallback;

	if (creative_dsp_chipio_read(b->chip, addr, &v) < 0)
		b->errs++;
	b->verbs++;
	creative_dsp_chipio_addr_invalidate();
	return v;
}

/*
 * Windows' AE-7 output select (C2B1 hp_select, C1B3's speaker select; its
 * items, pin loop and SetSpeakerMode), in its order:
 * - the DSP's output muted while it switches (runtime selects; Windows'
 *   audio service around every later select);
 * - the port map 0x58 (ChipIO param 13: 0x58 in every mode on the AE-7);
 * - headphones: item 0xc (0x2e = 0x3f), item 0xb = 0: the headphone path
 *   on (0x30 = 0x3f), the THD block (headphone row byte), the trim = the
 *   headphone gain, DSP 0x96 req 0x29/0x2a = 1.0; item 0xf: GPIO pin 0
 *   high (the headphone amp on);
 * - speakers: item 0xc (0x2e = 0), item 0xf (GPIO pin 0 high), item 0xb =
 *   1: 0x30 = 0, the THD block (speaker row byte), trim 0 dB, 0x29/0x2a =
 *   0.0;
 * - the pin loop (ae7_pins());
 * - 0x18b03c read, bit 7 cleared, written back;
 * - the DSP unmuted (runtime).
 * The volume isn't touched (Windows' volume thread writes it separately).
 * The start-up's select (runtime false) writes Low's trim on the
 * headphones, as Windows' first select; the heartbeat then writes the
 * saved gain (creative_ae7_gain(): Windows ~0.6 s later), and the gate
 * waits for it.
 * Returns the DAC/switch commands not acknowledged (plus 1 when the
 * headphone amp is held off: creative_ae7_amp_on()); with any, the output
 * counts as not selected (out_sent -1): the gate stays shut and the
 * heartbeat selects again (a refused 30/30 could leave the headphone path
 * on behind the speakers' level). The body runs as the select
 * (creative_ae7_select()) or, with replay_mutex held and without the
 * DSP's mute, as the fx bracket's preset (creative_ae7_preset_held()).
 */
static int ae7_select_body(struct creative_chip *chip, bool hp, bool runtime, u8 gain,
			   const char *what, bool locked)
{
	struct ae9_blk b;
	int bad = 0, held = 0;

	ae7_blk_begin(&b, chip, locked);
	if (runtime)
		blk_scp_set(&b, 0x96, 0x3a, AE7_F_ONE);
	blk_ctl_param(&b, AE9_CTL_PARAM_DAC2PORT, AE7_DAC2PORT);
	ae7_blk_end(&b, what, locked);

	bad += ae7_dac(chip, AE7_SW, SW_FRONT, hp ? AE7_SW_ON : 0);
	if (!hp)
		held = ae7_amp(chip, locked);
	bad += ae7_dac(chip, AE7_SW, SW_HP, hp ? AE7_SW_ON : 0);
	bad += ae7_thd(chip, hp);
	bad += ae7_trim(chip, hp ? ae7_gain_trim[gain] : AE7_TRIM_0DB);
	ae7_blk_begin(&b, chip, locked);
	blk_scp_set(&b, 0x96, 0x29, hp ? AE7_F_ONE : 0);
	blk_scp_set(&b, 0x96, 0x2a, hp ? AE7_F_ONE : 0);
	ae7_blk_end(&b, what, locked);
	if (hp)
		held = ae7_amp(chip, locked);

	ae7_pins(chip, hp, what, locked);

	ae7_blk_begin(&b, chip, locked);
	blk_chipio_write(&b, 0x18b03c, ae7_blk_chipio_value(&b, 0x18b03c, 0) & ~0x80U);
	if (runtime)
		blk_scp_set(&b, 0x96, 0x3a, 0);
	ae7_blk_end(&b, what, locked);

	chip->hu_output = hp;
	/* a refused command or the amp held: not selected (the gate stays shut) */
	chip->ae7.out_sent = bad || held ? -1 : hp;
	chip->ae7.gain_sent = hp && !bad ? gain : -1;
	chip->ae7.selects++;
	if (bad)
		dev_warn(chip->dev, "%s: %d DAC/switch command(s) not acknowledged (I2C NACK)\n",
			 what, bad);
	if (held)
		dev_warn(chip->dev, "%s: headphone amp held off (the DAC's mute not confirmed)\n", what);
	if (!bad && !held && !locked)
		dev_info(chip->dev, "%s%s\n", what,
			 hp ? (gain == 0 ? ", gain Low" : gain == 1 ? ", gain Normal" : ", gain High") : "");
	return bad + held;
}

int creative_ae7_select(struct creative_chip *chip, bool hp, bool runtime)
{
	u8 gain = runtime && chip->ae7.gain < AE7_GAINS ? chip->ae7.gain : AE7_GAIN_DEFAULT;

	return ae7_select_body(chip, hp, runtime, gain,
			       hp ? "AE-7: output to headphones" : "AE-7: output to speakers", false);
}

/*
 * The output select's preset again inside an fx bracket (Windows re-runs it
 * in every SBX / Speaker Config / output bracket: W2), replay_mutex held by
 * the caller (fx_apply_all_locked(), creative_ae7_fx.c): the select's body
 * without the DSP's mute (the bracket's own), for the output selected,
 * the trim the headphone gain asked now. It never unmutes the DAC (48/07
 * is written only to mute it: a held headphone amp, creative_ae7_amp_on()).
 * A command not acknowledged, or the amp held: the DAC (and the Rear/C-Sub
 * converters) muted, the output not selected (out_sent -1, the gate shut):
 * the heartbeat selects again from the top. Returns as the select.
 */
int creative_ae7_preset_held(struct creative_chip *chip, bool hp)
{
	u8 gain = chip->ae7.gain < AE7_GAINS ? chip->ae7.gain : AE7_GAIN_DEFAULT;
	int bad;

	lockdep_assert_held(&chip->replay_mutex);
	bad = ae7_select_body(chip, hp, false, gain, hp ? "AE-7: output block, the headphones' preset" :
			      "AE-7: output block, the speakers' preset", true);
	if (bad) {
		chip->ae7.unmuted = false;
		ae7_conv_mute(chip, true);
		ae7_dac07(chip, 0, K2M_MUTE);
		dev_warn(chip->dev, "AE-7: the output block's preset went wrong: DAC muted, the heartbeat selects again\n");
	}
	return bad;
}

/* ------------------------------------------------------------------ */
/* Levels								  */
/* ------------------------------------------------------------------ */

/*
 * One converter channel's level, as Windows' volume setter: the fine step
 * (1/64 dB), then the amp; the first level since the bring-up muted first
 * (at 0 dB), as Windows' first
 */
static void ae7_level(struct ae9_blk *b, u8 nid, bool right, u8 fine, u8 amp, bool first)
{
	u16 ch = AC_AMP_SET_OUTPUT | (right ? AC_AMP_SET_RIGHT : AC_AMP_SET_LEFT);

	blk_send(b, nid, right ? AE9_VERB_SET_FINE_VOL_R : AE9_VERB_SET_FINE_VOL_L, fine);
	if (first)
		blk_send(b, nid, AC_VERB_SET_AMP_GAIN_MUTE, ch | AC_AMP_MUTE | AE9_AMP_0DB);
	blk_send(b, nid, AC_VERB_SET_AMP_GAIN_MUTE, ch | amp);
}

/* the DSP's speaker volume (module 0x32): left, right in dB, then the mute (1.0 muted) */
static void ae7_dsp_vol(struct ae9_blk *b, u32 db, bool mute)
{
	blk_scp_set(b, 0x32, 0x03, db);
	blk_scp_set(b, 0x32, 0x04, db);
	blk_scp_set(b, 0x32, 0x02, mute ? AE7_F_ONE : 0);
}

/*
 * The volume for output out (0 speakers, 1 headphones; -1: the output
 * picked), the one reading the heartbeat's volume step, the select's and
 * the gate share (kit 11), under chip->lock: hu_att_out[] holds each
 * output's level; "Master Playback Volume" (hu_att, core.c's ae7_vol_put())
 * becomes the level of the output picked (ae7.out) once it has stood
 * AE7_KICK_MS, here. A switch to the speakers within AE7_KICK_MS takes it
 * along (alsactl restores Master before "Head Unit Output", microseconds
 * apart: a state saved on Speakers), a switch to the headphones leaves it
 * the level of the output left (core.c's ae7_out_put()); an output switch
 * never moves a level taken from one output to the other. Returns the DAC
 * byte (0xff: "Master Playback Switch" off); *att the level (not NULL);
 * *stale (not NULL): a Master write not taken yet, or another output
 * picked than out.
 */
u8 creative_ae7_want_att(struct creative_chip *chip, int out, unsigned int *att, bool *stale)
{
	unsigned long flags;
	unsigned int pick, a;
	bool mute, st;

	spin_lock_irqsave(&chip->lock, flags);
	pick = chip->ae7.out;
	if (chip->ae7.att_new &&
	    !time_before(jiffies, chip->ae7.att_at + msecs_to_jiffies(AE7_KICK_MS))) {
		chip->hu_att_out[pick] = chip->hu_att;
		chip->ae7.att_new = false;
	}
	if (out < 0)
		out = pick;
	a = min_t(unsigned int, chip->hu_att_out[out], AE7_ATT_MAX);
	mute = chip->hu_mute;
	st = chip->ae7.att_new || (unsigned int)out != pick;
	spin_unlock_irqrestore(&chip->lock, flags);
	if (att)
		*att = a;
	if (stale)
		*stale = st;
	return mute ? 0xff : a;
}

/*
 * The volume of output out (creative_ae7_want_att()), as Windows' AE-7
 * volume thread writes a change (C1B3 at -10.5 dB, C2B1 at 0 dB), channel
 * by channel:
 * - front left: DAC 48/0f = the attenuation in 0.5 dB steps (0xff mute),
 *   converter 0x02 left at 0 dB (Windows forces it on the AE-7's front
 *   channels), DSP 0x32 left/right = the level in dB, req 2 = mute;
 * - front right: 48/10, converter 0x02 right, DSP 0x32 again;
 * - the other channels (the CA0132's own DACs: Rear, C/Sub jacks):
 *   converters 0x03 and 0x04 at the level as Windows (FUN_000218dc): v =
 *   (9000 + L) * 64 / 100, L in 1/100 dB capped at the INF's
 *   LineOutEx2-5_MaxVolume (-1 dB); amp = v >> 6, fine = v & 0x3f; on the
 *   0.5 dB grid v = 5760 - 32 * max(att, 2) (C1B3: -10.5 dB = 0x4f + 0x20;
 *   W2: -1.94 dB = 0x58 + 0x03; 0 dB = 0x59 + 0), muted with the master
 *   switch and at the range floor (-90 dB); muted too while the gate is
 *   shut or the output is the headphones (ae7_conv_gated()).
 * DSP 0x32 is not in the AE-9's speaker path (uv44; same firmware): the
 * attenuation is the DAC's alone, Windows' messages are parity. Returns the
 * DAC commands not acknowledged (0: chip->ae7.att_sent = the level).
 */
int creative_ae7_volume(struct creative_chip *chip, int out)
{
	unsigned int att;
	u8 dac = creative_ae7_want_att(chip, out, &att, NULL);
	bool mute = dac == 0xff;
	u32 db = ae7_att_f32(att);
	unsigned int v = 5760 - 32 * max(att, 2u);	/* att <= AE7_ATT_MAX */
	u8 fine = v & 0x3f, amp = v >> 6;
	bool first = !chip->ae7.levels_set;
	struct ae9_blk b;
	int bad;

	chip->ae7.conv_amp = amp;
	chip->ae7.conv_fine = fine;
	chip->ae7.conv_mute = mute || att >= AE7_ATT_MAX;
	if (chip->ae7.conv_mute || ae7_conv_gated(chip))
		amp |= AC_AMP_MUTE;
	bad = ae7_dac(chip, AE7_DAC, K2M_VOLUME_L, dac);
	blk_begin(&b, chip);
	ae7_level(&b, AE9_NID_DAC, false, 0, AE9_AMP_0DB, first);
	ae7_dsp_vol(&b, db, mute);
	blk_end(&b, "AE-7: volume");
	bad += ae7_dac(chip, AE7_DAC, K2M_VOLUME_R, dac);
	blk_begin(&b, chip);
	ae7_level(&b, AE9_NID_DAC, true, 0, AE9_AMP_0DB, first);
	ae7_dsp_vol(&b, db, mute);
	ae7_level(&b, 0x03, false, fine, amp, first);
	ae7_level(&b, 0x03, true, fine, amp, first);
	ae7_level(&b, 0x04, false, fine, amp, first);
	ae7_level(&b, 0x04, true, fine, amp, first);
	blk_end(&b, "AE-7: volume");
	chip->ae7.conv_on = !(amp & AC_AMP_MUTE) && !b.errs;
	chip->ae7.levels_set = true;
	chip->ae7.att_sent = bad ? -1 : dac;
	ae9_dbg(chip, "AE-7: volume att 0x%02x%s%s\n", att, mute ? " (muted)" : "",
		bad ? ", NOT acknowledged" : "");
	return bad;
}

/*
 * The converters at their level again (the gate's, after 48/07's unmute):
 * on the speakers only, the level of the last volume (muted with the master
 * switch or at the floor). A verb not answered leaves a channel muted (the
 * safe side). Returns the verbs not answered.
 */
static int ae7_conv_levels(struct creative_chip *chip)
{
	u8 amp = chip->ae7.conv_amp | (chip->ae7.conv_mute ? AC_AMP_MUTE : 0);
	struct ae9_blk b;

	if (ae7_conv_gated(chip))
		return 0;
	blk_begin(&b, chip);
	ae7_level(&b, 0x03, false, chip->ae7.conv_fine, amp, false);
	ae7_level(&b, 0x03, true, chip->ae7.conv_fine, amp, false);
	ae7_level(&b, 0x04, false, chip->ae7.conv_fine, amp, false);
	ae7_level(&b, 0x04, true, chip->ae7.conv_fine, amp, false);
	blk_end(&b, "AE-7: Rear/C-Sub converters at the level");
	chip->ae7.conv_on = !(amp & AC_AMP_MUTE) && !b.errs;
	return b.errs;
}

/*
 * The headphone gain (Windows' slot: the trim, only while the headphones
 * are selected; the speakers keep 0 dB). Returns the commands not
 * acknowledged.
 */
int creative_ae7_gain(struct creative_chip *chip)
{
	u8 gain = chip->ae7.gain < AE7_GAINS ? chip->ae7.gain : AE7_GAIN_DEFAULT;
	int bad;

	if (chip->ae7.out_sent != 1)
		return 0;
	bad = ae7_trim(chip, ae7_gain_trim[gain]);
	chip->ae7.gain_sent = bad ? -1 : gain;
	dev_info(chip->dev, "AE-7: headphone gain %s%s\n",
		 gain == 0 ? "Low" : gain == 1 ? "Normal" : "High",
		 bad ? " NOT acknowledged (I2C NACK)" : "");
	return bad;
}

/*
 * The DAC filter: 48/07 with the new filter bits and the mute bits kept
 * (Windows reads 48/07 first; dsp.c's list puts chip->dac_filter_bits in
 * bits 7:5 of every 48/07 it sends, the mute bits from our last write)
 */
int creative_ae7_filter(struct creative_chip *chip)
{
	int bad = ae7_dac(chip, AE7_DAC, K2M_FILTER_MUTE,
			  chip->dac48_07 ? chip->dac48_07 : chip->dac_filter_bits | K2M_MUTE);

	chip->ae7.filter_sent = bad ? -1 : chip->ae7.filter;
	return bad;
}

/*
 * The DAC's mute gate. Windows unmutes the AE-7's DAC at the end of its
 * start without a look; ours only when it is known to hold what we sent:
 * an output selected, the volume (and on headphones the gain) acknowledged
 * since, and both volume registers read back checked (one read each, as
 * Windows reads since 0.12.17; both of its phases acknowledged) equal to it. Then 48/07 bits 1:0 cleared. A refusal
 * leaves the DAC muted and says why; the heartbeat tries again. 0 =
 * unmuted. Before all of it, the start-up's router remap must have set all
 * 12 of stream 0x0c's slots (ae7_remap(): a start-up that isn't Windows'
 * doesn't play; the heartbeat walks again), and the playback path must be
 * confirmed (phase 2, ae7.path_ok: the start-up's DSP path, or the last
 * Direct Mode / DSP path switch acknowledged and its DAC input and clock
 * read back, the CA0132's data pin and clock descriptors too); in Direct
 * Mode the DAC must also report lock on its input (48/40 bit 0), but in
 * its sync mode (384 kHz, 48/0a bit 4 read back set): there the K2M's DPLL
 * is bypassed and its lock bit measures nothing (the ES9038Q2M's datasheet
 * says it reads 1 then; Windows never reads it and played 384 kHz audibly
 * in capture Bw), so it is read and logged, and the clock descriptors read
 * back at 0x1f103 / 0x31002 (path_ok) stand in for it.
 *
 * The lock comes some time after the path (kit 8: 0.11-0.4 s on the S/PDIF
 * input): for AE7_LOCK_WAIT_MS after the start confirmed its path
 * (ae7.lock_from), a lock bit read 0 is a wait, not a refusal
 * (ae7_lock_waiting(): one info line, -EAGAIN; the heartbeat then reads the
 * lock bit alone every AE7_LOCK_POLL_MS and runs the whole gate again once
 * it reads 1); past that it is a refusal as before. The lock bit is still
 * the last read before the unmute, and a lock bit not read (-EIO) is never
 * a wait. In DSP mode the gate waits (quietly, -EBUSY, not a refusal)
 * until the output block for the output and path was sent since the last
 * bring-up or output switch (ae7.block_sent, creative_ae7_fx.c), as
 * Windows sends it before its unmute. The level is the selected output's
 * (creative_ae7_want_att()); a level acknowledged but no longer the one
 * wanted (Master written since it was sent, a write not taken yet, another
 * output picked) is not a refusal either (kit 11): quiet, -ESTALE, not
 * counted; the caller has the heartbeat send it (kit 10's load counted a
 * Master restored during the bracket as "volume not acknowledged"). 0 =
 * unmuted, -EAGAIN = waiting for the lock, -EBUSY = waiting for the block,
 * -ESTALE = the level changed since it was sent, -EIO = refused.
 */
static unsigned int ae7_lock_age(struct creative_chip *chip)
{
	return jiffies_to_msecs(jiffies - chip->ae7.lock_from);
}

static bool ae7_lock_waiting(struct creative_chip *chip)
{
	unsigned int ms = ae7_lock_age(chip);

	if (ms >= AE7_LOCK_WAIT_MS)
		return false;
	if (!chip->ae7.lock_wait) {
		chip->ae7.lock_wait = true;
		chip->ae7.lock_waits++;
		dev_info(chip->dev, "AE-7: waiting for the DAC to lock to its %s input (48/40 0x%02x, %u ms after the path; up to %u ms)\n",
			 chip->ae7.dac01 == K2M_IN_SPDIF ? "S/PDIF" : "I2S", chip->ae7.dac40 & 0xff, ms,
			 AE7_LOCK_WAIT_MS);
	} else {
		dev_dbg(chip->dev, "AE-7: DAC not locked yet (48/40 0x%02x, %u ms after the path)\n",
			chip->ae7.dac40 & 0xff, ms);
	}
	return true;
}

/* a DAC byte for the log: "0x%02x", or "-" for none (not acknowledged, not read) */
static const char *ae7_hexb(char *buf, size_t n, int v)
{
	if (v < 0)
		return "-";
	snprintf(buf, n, "0x%02x", v & 0xff);
	return buf;
}

int creative_ae7_unmute(struct creative_chip *chip)
{
	int out = chip->ae7.out_sent;
	const char *why = NULL;
	bool sync = false, stale, changed = false;
	u8 want = creative_ae7_want_att(chip, out, NULL, &stale);
	char s[8], rl[8], rr[8];
	u32 raw[2];
	int l = -1, r = -1;

	/* DSP mode: the output block first (creative_ae7_fx.c), quietly: the
	 * heartbeat's fx step sends it, then runs the gate again */
	if (!chip->ae7.direct && !chip->ae7.block_sent) {
		ae9_dbg(chip, "AE-7: DAC kept muted until the DSP's output block is sent\n");
		return -EBUSY;
	}
	if (chip->ae7.remapped != AE7_REMAP_SLOTS)
		why = "router remap incomplete";
	else if (!chip->ae7.path_ok)
		why = "playback path not confirmed";
	else if (out < 0)
		why = "no output selected";
	else if (chip->ae7.att_sent < 0)
		why = "volume not acknowledged";
	else if (stale || chip->ae7.att_sent != want)
		changed = true;
	else if (out == 1 && chip->ae7.gain_sent != chip->ae7.gain)
		why = "headphone gain not acknowledged";
	if (changed) {
		ae9_dbg(chip, "AE-7: DAC kept muted: Master or the output changed since the volume was sent (want 0x%02x, sent 0x%02x%s); the heartbeat sends it\n",
			want, chip->ae7.att_sent & 0xff, stale ? ", a change pending" : "");
		return -ESTALE;
	}
	/* waiting for the lock since an earlier try of this start: the lock
	 * bit alone (one read a poll) until it reads 1, then the whole gate */
	if (!why && chip->ae7.direct && chip->ae7.lock_wait && ae7_lock_age(chip) < AE7_LOCK_WAIT_MS) {
		chip->ae7.dac40 = creative_dsp_ca0113_read_checked(chip, AE7_DAC, K2M_CHIP_ID, raw);
		if (chip->ae7.dac40 >= 0 && !(chip->ae7.dac40 & K2M_LOCKED) && ae7_lock_waiting(chip))
			return -EAGAIN;
	}
	if (!why) {
		l = creative_dsp_ca0113_read_checked(chip, AE7_DAC, K2M_VOLUME_L, raw);
		r = creative_dsp_ca0113_read_checked(chip, AE7_DAC, K2M_VOLUME_R, raw);
		if (l != want || r != want)
			why = "volume read-back differs";
	}
	/* Direct Mode: the DAC locked to the input it was switched to (48/40
	 * bit 0); read, not required, in sync mode */
	if (!why && chip->ae7.direct) {
		sync = chip->ae7.dac0a >= 0 && (chip->ae7.dac0a & K2M_SYNC_MODE);
		chip->ae7.dac40 = creative_dsp_ca0113_read_checked(chip, AE7_DAC, K2M_CHIP_ID, raw);
		if (chip->ae7.dac40 >= 0 && !sync && !(chip->ae7.dac40 & K2M_LOCKED) &&
		    ae7_lock_waiting(chip))
			return -EAGAIN;
		if (chip->ae7.dac40 < 0 || (!sync && !(chip->ae7.dac40 & K2M_LOCKED))) {
			why = chip->ae7.lock_wait ? "DAC not locked to its input (48/40 bit 0) after the lock wait" :
			      "DAC not locked to its input (48/40 bit 0)";
			/* the wait ran out: counted once (its first refusal) */
			chip->ae7.lock_late += chip->ae7.lock_wait && !chip->ae7.gate_fails;
		}
	}
	if (!why && ae7_dac(chip, AE7_DAC, K2M_FILTER_MUTE,
			    (chip->dac48_07 ? chip->dac48_07 : chip->dac_filter_bits) & ~K2M_MUTE))
		why = "unmute not acknowledged";
	if (why) {
		chip->ae7.gate_blocks++;
		if (chip->ae7.gate_fails++ < 3)
			dev_warn(chip->dev, "AE-7: DAC left muted: %s (want 0x%02x, sent %s, read %s/%s)\n",
				 why, want, ae7_hexb(s, sizeof(s), chip->ae7.att_sent),
				 ae7_hexb(rl, sizeof(rl), l), ae7_hexb(rr, sizeof(rr), r));
		return -EIO;
	}
	chip->ae7.unmuted = true;
	chip->ae7.gate_fails = 0;
	if (ae7_conv_levels(chip))
		dev_warn(chip->dev, "AE-7: the Rear/C-Sub converters' level not answered: left muted\n");
	if (sync)
		dev_info(chip->dev, "AE-7: DAC unmuted (volume 0x%02x read back on both channels; sync mode: 48/40 0x%02x, its lock bit not required)\n",
			 want, chip->ae7.dac40 & 0xff);
	else if (chip->ae7.direct && chip->ae7.lock_wait)
		dev_info(chip->dev, "AE-7: DAC unmuted (volume 0x%02x read back on both channels; locked %u ms after the path)\n",
			 want, chip->ae7.lock_ms = jiffies_to_msecs(jiffies - chip->ae7.lock_from));
	else
		dev_info(chip->dev, "AE-7: DAC unmuted (volume 0x%02x read back on both channels)\n", want);
	chip->ae7.lock_wait = false;
	return 0;
}

/* GPIO pin 1: the on-card LED */
void creative_ae7_led(struct creative_chip *chip, bool on)
{
	ae7_gpio(chip, AE7_GPIO_LED, on);
}

/* ------------------------------------------------------------------ */
/* Playback start and stop (creative_ae7_pcm.c)			  */
/* ------------------------------------------------------------------ */

/*
 * A stream's start, Windows' generic part, in both modes (C2B1
 * stream_start; capture B, 13 Direct Mode and 6 DSP-mode starts, all the
 * same): the DSP's output unmuted (0x96 req 0x3a = 0;
 * creative_ae7_stream_unmute()), 0x18b008 read, bit 3 (a hold bit)
 * cleared, written back (creative_ae7_hold_off()); in Direct Mode the
 * board's stage 2 then; converter 0x02's format and the bind
 * (creative_ae7_bind()); in Direct Mode stage 3 then.
 */
void creative_ae7_stream_unmute(struct creative_chip *chip)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	blk_scp_set(&b, 0x96, 0x3a, 0);
	blk_end(&b, "AE-7: DSP output unmuted");
}

void creative_ae7_hold_off(struct creative_chip *chip)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	blk_chipio_write(&b, 0x18b008, ae7_blk_chipio_value(&b, 0x18b008, 0xf0) & ~0x08U);
	blk_end(&b, "AE-7: playback start, 0x18b008");
}

/* converter 0x02's format, the bind (the playback stream's tag, channel 0) */
void creative_ae7_bind(struct creative_chip *chip, u16 format)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	blk_send(&b, AE9_NID_DAC, AC_VERB_SET_STREAM_FORMAT, format);
	blk_send(&b, AE9_NID_DAC, AC_VERB_SET_CHANNEL_STREAMID, CREATIVE_PCM_STREAM_TAG << 4);
	blk_end(&b, "AE-7: playback start");
}

/* converter 0x02 unbound (a stream's release; Windows: 706 = 0 at stop) */
void creative_ae7_unbind(struct creative_chip *chip)
{
	struct ae9_blk b;

	blk_begin(&b, chip);
	blk_send(&b, AE9_NID_DAC, AC_VERB_SET_CHANNEL_STREAMID, 0);
	blk_end(&b, "AE-7: playback stop");
}

/* ------------------------------------------------------------------ */
/* Shutdown								  */
/* ------------------------------------------------------------------ */

/* one byte of the 8051's XRAM read in a block (its value, -1 unread) */
static int ae7_blk_xram(struct ae9_blk *b, u16 addr)
{
	u32 r = 0;

	blk_8051_addr(b, addr);
	b->verbs++;
	if (creative_ae9_corb_send_verb_sync(b->chip,
					     creative_ae9_verb(AE9_CAD, WIDGET_CHIP_CTRL,
							       VENDOR_CHIPIO_8051_DATA_READ, 0),
					     &r) < 0) {
		b->errs++;
		return -1;
	}
	return r & 0xff;
}

/*
 * Windows' AE-7 stop (C2B1's restart shutdown; D3 / D3cold power state and
 * the restart and power-off paths are the same up to the AFG), in order:
 * - the generic stream 0x0c stop (selected, its control read, stopped);
 *   ASI 0;
 * - the board's stops: stream 0x18 while its Active byte (0x0824) reads
 *   non-zero, stream 0x0c while its count (0x07ac) does, each stop 50 ms
 *   (the 8051 tears routes down after answering), until both read 0;
 * - the switch chip's 0x2b = 0; the PMU bit request 0x83;
 * - ours: the Rear/C-Sub converters muted (creative_ae7_conv_mute());
 * - the DAC muted twice (48/07 read, bits 1:0 set), the headphone path
 *   off (0x30 = 0), 0x2e = 0, GPIO pin 1 (LED) off, pin 0 (the headphone
 *   amp) off;
 * - Windows' generic part, ~0.75 s later: the pins off (0x11 to input
 *   only, 0x12, 0x13, 0x0c, 0x0b with its EAPD, 0x0f, 0x32 = 0x3f, 0x10),
 *   the AFG's vendor word 0x5300, the DAC muted again, the AFG to D3hot
 *   (restart, unload) or D3cold (power-off: cold).
 * Unlike the AE-9's, pin 0 here is the headphone amp, not the DAC's reset:
 * the DAC keeps answering after it (both AE-7 captures acknowledge the
 * last 48/07), so nothing here is an expected NACK.
 */
void creative_ae7_power_down(struct creative_chip *chip, bool cold)
{
	const char *what = cold ? "AE-7: shutdown (D3cold)" : "AE-7: shutdown (D3hot)";
	struct ae9_blk b;
	unsigned int n;
	int a18 = 0, a0c = 0;

	blk_begin(&b, chip);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_ID, AE7_STREAM_DSP_PROC);
	blk_send(&b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_GET, CONTROL_PARAM_STREAM_CONTROL);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_ID, AE7_STREAM_DSP_PROC);
	blk_stream_stop(&b);
	blk_ctl_param(&b, CONTROL_PARAM_ASI, 0);
	for (n = 0; n < 8; n++) {
		a18 = ae7_blk_xram(&b, AE9_XRAM_STREAM_ACTIVE(AE9_STREAM_DSP_OUT));
		if (a18 > 0) {
			blk_ctl_param(&b, CONTROL_PARAM_STREAM_ID, AE9_STREAM_DSP_OUT);
			blk_stream_stop(&b);
		}
		a0c = ae7_blk_xram(&b, AE9_XRAM_STREAM_ACTIVE(AE7_STREAM_DSP_PROC));
		if (a0c > 0) {
			blk_ctl_param(&b, CONTROL_PARAM_STREAM_ID, AE7_STREAM_DSP_PROC);
			blk_stream_stop(&b);
		}
		if (a18 <= 0 && a0c <= 0)
			break;
	}
	blk_end(&b, what);
	if (a18 != 0 || a0c != 0)
		dev_warn(chip->dev, "%s: streams 0x18/0x0c still counted (%d/%d)\n", what, a18, a0c);

	ae7_dac(chip, AE7_SW, SW_DSD, 0);
	blk_begin(&b, chip);
	blk_send(&b, WIDGET_CHIP_CTRL, AE9_VERB_CHIPIO_PMU_BIT, 0x83);
	blk_end(&b, what);
	chip->ae7.unmuted = false;
	creative_ae7_conv_mute(chip);
	ae7_dac07(chip, 0, K2M_MUTE);
	ae7_dac07(chip, 0, K2M_MUTE);
	ae7_dac(chip, AE7_SW, SW_HP, 0);
	ae7_dac(chip, AE7_SW, SW_FRONT, 0);
	ae7_gpio(chip, AE7_GPIO_LED, false);
	ae7_gpio(chip, AE7_GPIO_AMP, false);

	msleep(750);	/* Windows: 0.74 s from the GPIO to the generic part (C2B1) */
	blk_begin(&b, chip);
	blk_send(&b, 0x12, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	blk_send(&b, 0x11, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	blk_send(&b, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN);
	blk_send(&b, 0x12, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	ae7_blk_pin(&b, 0x13, 0);
	ae7_blk_pin(&b, 0x0c, 0);
	blk_send(&b, 0x0b, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	blk_send(&b, 0x0b, AC_VERB_SET_EAPD_BTLENABLE, 0);
	blk_send(&b, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	ae7_blk_pin(&b, 0x0f, 0);
	blk_end(&b, what);
	ae7_dac(chip, AE7_SW, SW_MUX_OUT, AE7_SW_ON);
	blk_begin(&b, chip);
	ae7_blk_pin(&b, 0x10, 0);
	blk_send(&b, AE9_NID_AFG, AE9_VERB_GET_NODE_WORD, 0);
	blk_send(&b, AE9_NID_AFG, AE9_VERB_SET_NODE_WORD_LO, 0x00);
	blk_send(&b, AE9_NID_AFG, AE9_VERB_SET_NODE_WORD_HI, 0x53);
	blk_end(&b, what);
	ae7_dac07(chip, 0, K2M_MUTE);
	blk_begin(&b, chip);
	blk_send(&b, AE9_NID_AFG, AC_VERB_GET_POWER_STATE, 0);
	blk_send(&b, AE9_NID_AFG, AC_VERB_SET_POWER_STATE, cold ? AE9_PWRST_D3COLD : AC_PWRST_D3);
	blk_end(&b, what);
	chip->ae7.out_sent = -1;
	chip->ae7.gain_sent = -1;
	chip->ae7.filter_sent = -1;
	chip->ae7.att_sent = -1;
	chip->ae7.levels_set = false;
	chip->ae7.pins_state = -1;
	/* the path: unknown until the next start-up sets its own (post_fw) */
	chip->ae7.direct = false;
	chip->ae7.path_ok = false;
	chip->ae7.path_rate = 0;
	chip->ae7.spdif_reclock = false;
	chip->ae7.s14 = false;
	dev_info(chip->dev, "%s: done\n", what);
}

/* ------------------------------------------------------------------ */
/* Phase 2: Direct Mode and its rates (level 4)			  */
/* ------------------------------------------------------------------ */

/*
 * Direct Mode's rates, from Windows' AE-7 board (CtxHda 0065, the build
 * captures A, B and Bw ran: its format gate, SetFormat, stages 2 and 3),
 * every value captured on the AE-7 (capture B: 13 starts at 48 kHz; capture
 * Bw: 32 starts at all six rates, the files' own, every rate change but
 * 44.1 <-> 88.2; docs/dev/AE7.md "Phase 2"; tools/ae7golden
 * golden-capbw-rates):
 * - the 48 kHz family on I2S: the clock descriptors 0x189000/04 and the
 *   rate descriptor 0x189024, the DAC on its I2S input (48/01 = 0x80),
 *   register 10 = 0x05, the ASI rate class 0x81; 384 kHz with 48/0a = 0x15
 *   (the K2M's sync mode) and class 0x80, the link at 192 kHz with 4
 *   channels (creative_ae9_stream_format()), stream 0x05's connection
 *   point at 192 kHz;
 * - the 44.1 kHz family through the CA0132's S/PDIF transmitter (its
 *   0x18a020-28 words, the AE-9's), the DAC's data pin to it (PLL/PMU 0x43
 *   = 0x06), the DAC on its S/PDIF input (48/01 = 0x81, 48/0b = 0x12),
 *   class 0x80.
 * The class is written only when it changes (384 kHz after 44.1 / 88.2:
 * none). Router slots of stream 0x05: (rate / 48 kHz) x 2 (I2S rates;
 * capture Bw: 2/4/8/16, the 8051's words already on the DAC's ports at 48
 * and 96 kHz, the first 4 at 192 kHz, 384 kHz's lanes 2/3/6/7 on 0x50-0x53
 * and 8-15 unrouted: the walk sets all). All six at level 4 since kit 9
 * (kit 8 played 44.1/88.2/384 kHz on the tester's AE-7, clean and at the
 * level of the others). 176.4 and 352.8 kHz are not in Windows'
 * gate, not here: refused.
 */
static const struct ae7_rate ae7_rates[] = {
	{  44100, true,  0x08, 0x80, K2M_CLK_SLAVE,  0, 0, 0,
	   { 0x02, 0x00101204, 0xfb, 0x03 } },
	{  48000, false, AE9_SR_48000, 0x81, K2M_CLK_SLAVE,  2, 0x0001f100, 0x00008005 },
	{  88200, true,  0x0a, 0x80, K2M_CLK_SLAVE,  0, 0, 0,
	   { 0x42, 0x08101204, 0x7b, 0x43 } },
	{  96000, false, AE9_SR_96000, 0x81, K2M_CLK_SLAVE,  4, 0x0001f101, 0x00014004 },
	{ 192000, false, 0x0e, 0x81, K2M_CLK_SLAVE,  8, 0x0001f102, 0x00022003 },
	{ 384000, false, 0x0e, 0x80, K2M_CLK_SYNC,  16, 0x0001f103, 0x00031002 },
};

static const struct ae7_rate *ae7_rate_find(unsigned int rate)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ae7_rates); i++)
		if (ae7_rates[i].rate == rate)
			return &ae7_rates[i];
	return NULL;
}

/*
 * A playback rate's row as the level offers it, NULL when refused: the
 * DSP-mode-only test levels 2 and 3 48 kHz only; level 4 (ae7=1) all six.
 * 176.4 / 352.8 kHz (and anything else) never.
 */
const struct ae7_rate *creative_ae7_rate_row(unsigned int rate, int mode)
{
	const struct ae7_rate *row = ae7_rate_find(rate);

	if (!row || (mode < AE7_MODE_DIRECT && rate != 48000))
		return NULL;
	return row;
}

/*
 * The DAC muted and the mute confirmed (48/07 bits 1:0 set, acknowledged,
 * read back set), the Rear/C-Sub converters muted first (their verbs
 * answered), before any path or rate switch: the gate shuts whatever the
 * answer (it reopens only through creative_ae7_unmute()). 0 = muted.
 */
int creative_ae7_mute_checked(struct creative_chip *chip)
{
	u32 raw[2];
	int v, conv;

	chip->ae7.unmuted = false;
	conv = creative_ae7_conv_mute(chip);
	if (ae7_dac07(chip, 0, K2M_MUTE))
		return -EIO;
	v = creative_dsp_ca0113_read_checked(chip, AE7_DAC, K2M_FILTER_MUTE, raw);
	return !conv && v >= 0 && (v & K2M_MUTE) == K2M_MUTE ? 0 : -EIO;
}

/* a verb's answer in a block (fallback when it isn't answered) */
static u32 ae7_blk_read(struct ae9_blk *b, u8 nid, u16 verb, u16 parm, u32 fallback)
{
	u32 r = 0;

	b->verbs++;
	if (creative_ae9_corb_send_verb_sync(b->chip, creative_ae9_verb(AE9_CAD, nid, verb, parm),
					     &r) < 0) {
		b->errs++;
		return fallback;
	}
	return r;
}

/*
 * ChipIO 0x725, the ASI rate class (Windows' FUN_0003ced0): written only
 * when it changes, then read back (0xf25) until it holds, up to 11 reads,
 * as the start-up's. Not held: an error of the block (the gate stays
 * shut) and not cached, so the next set-up writes it again.
 */
static void ae7_blk_asi_class(struct ae9_blk *b, u8 cls)
{
	struct creative_chip *chip = b->chip;
	unsigned int i;
	u32 r = ~0U;

	if (chip->ae7.asi_class == cls)
		return;
	blk_send(b, WIDGET_CHIP_CTRL, AE7_VERB_ASI_CLASS, cls);
	for (i = 0; i < 11; i++) {
		r = ae7_blk_read(b, WIDGET_CHIP_CTRL, 0xf00 | (AE7_VERB_ASI_CLASS & 0xff), 0, ~0U);
		if ((r & 0xff) == cls)
			break;
		usleep_range(1000, 1500);
	}
	if ((r & 0xff) != cls) {
		b->errs++;
		chip->ae7.asi_class = 0;
		dev_warn(chip->dev, "AE-7: ASI rate class reads 0x%02x, not 0x%02x\n", r & 0xff, cls);
		return;
	}
	chip->ae7.asi_class = cls;
}

/* a stream started unless its Active byte says it runs (Windows' FUN_000341e4) */
static int ae7_blk_stream_start(struct ae9_blk *b, u8 sid)
{
	int a = ae7_blk_xram(b, AE9_XRAM_STREAM_ACTIVE(sid));

	if (a == 0) {
		blk_ctl_param(b, CONTROL_PARAM_STREAM_ID, sid);
		blk_ctl_param(b, CONTROL_PARAM_STREAM_CONTROL, 1);
	}
	return a;
}

/*
 * a stream stopped if its Active byte says it runs, or can't be read
 * (FUN_00034250), 50 ms; returns the Active byte (< 0 unread)
 */
static int ae7_blk_stream_stop(struct ae9_blk *b, u8 sid)
{
	int a = ae7_blk_xram(b, AE9_XRAM_STREAM_ACTIVE(sid));

	if (a != 0) {
		blk_ctl_param(b, CONTROL_PARAM_STREAM_ID, sid);
		blk_stream_stop(b);
	}
	return a;
}

/*
 * The DSP's output to the DAC, stream 0x18, stopped where its Active byte
 * (0x0824) says it runs (or can't be read): the first step of Windows'
 * stage 2 at every Direct Mode start (FUN_0003c318 -> FUN_00039a48(0);
 * capture B: 13 starts, each read 0, nothing more), and all of SetDirect(1)
 * (FUN_0003be30; capture A's four switches on: read 1, stopped; golden-
 * capa-direct), which ours runs at the first Direct start. The 8051 tears
 * its routes down after answering: blk_stream_stop()'s 50 ms. Stream 0x0c
 * (the DSP to the CA0132's own DACs) is left as it is, as Windows does.
 * Returns the Active byte (< 0 unread: stopped all the same); *bad += the
 * verbs not answered.
 */
int creative_ae7_dsp_out_off(struct creative_chip *chip, int *bad)
{
	struct ae9_blk b;
	int a;

	blk_begin(&b, chip);
	a = ae7_blk_stream_stop(&b, AE9_STREAM_DSP_OUT);
	blk_end(&b, a ? "AE-7: Direct Mode: the DSP's output (stream 0x18) stopped" :
			"AE-7: Direct Mode: the DSP's output (stream 0x18) not running");
	*bad += b.errs;
	return a;
}

/*
 * Windows' AE-7 stage 2 (FUN_0003c318) for a stream in Direct Mode: stream
 * 0x05 (host audio, connection point 0x43) to the DAC's I2S connection
 * point 0xd0, or to the S/PDIF transmitter 0x91 for the 44.1 kHz family,
 * 2 channels, that point at the stream's rate (192 kHz for 384); stream
 * 0x14 from 0x43 to connection point 0, that point at 96 kHz, 2 channels
 * (the AE-9's stage 2 gives 0x14 the stream's count; the AE-7's always 2).
 * The parameters as the AE-9's captured start block sends them
 * (blk_ctl_param(): the two-verb form after a ChipIO status read).
 * Captured at 48 kHz (capture B, 13 starts, verb for verb); after stream
 * 0x18's stop (creative_ae7_dsp_out_off()), before the bind. Returns the
 * verbs not answered.
 */
int creative_ae7_direct_route(struct creative_chip *chip, const struct ae7_rate *row)
{
	u8 dst = row->spdif ? AE9_CP_SRC : AE9_CP_FRONT_DAC;
	struct ae9_blk b;

	blk_begin(&b, chip);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_ID, AE9_STREAM_HOST);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_SOURCE_CONN, AE9_CP_HOST);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_DEST_CONN, dst);
	blk_ctl_param(&b, CONTROL_PARAM_STREAMS_CHANNELS, 2);
	blk_ctl_param(&b, CONTROL_PARAM_CONN_POINT_ID, dst);
	blk_ctl_param(&b, CONTROL_PARAM_CONN_POINT_SR, row->cp_code);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_ID, AE9_STREAM_DIRECT);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_SOURCE_CONN, AE9_CP_HOST);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_DEST_CONN, 0);
	blk_ctl_param(&b, CONTROL_PARAM_CONN_POINT_ID, 0);
	blk_ctl_param(&b, CONTROL_PARAM_CONN_POINT_SR, AE9_SR_96000);
	blk_ctl_param(&b, CONTROL_PARAM_STREAM_ID, AE9_STREAM_DIRECT);
	blk_ctl_param(&b, CONTROL_PARAM_STREAMS_CHANNELS, 2);
	blk_end(&b, row->spdif ? "AE-7: Direct Mode route (stream 0x05 to the S/PDIF transmitter)" :
				 "AE-7: Direct Mode route (stream 0x05 to the DAC)");
	return b.errs;
}

/*
 * The clocks for a rate and the DAC's input and clock register (Windows'
 * FUN_0003c8cc, its stage 3's first step; only when the rate differs from
 * the one the card holds, ae7.path_rate, as Windows caches it):
 * - I2S rates (FUN_0003ca78): ASI 4, the DAC's data pin to I2S (PLL/PMU
 *   0x43 = 0xc7), the ASI rate class, the clock and rate descriptors,
 *   0x189028; then 48/01 = 0x80 and 48/0a;
 * - the 44.1 kHz family (FUN_0003cbcc): the rate class 0x80; the S/PDIF
 *   transmitter off (0x18a020 = 2), its clock (PLL/PMU 0x01 = 0x17), the
 *   DAC's data pin to the transmitter (0x43 = 0x06), PLL/PMU 0x04 bit 7
 *   (read, set, written: the 44.1 kHz family), its configuration and
 *   divider, on; then 48/01 = 0x81, 48/0b = 0x12, 48/0a = 0x05. No ASI 4,
 *   and the I2S descriptors left as they are: capture Bw's 384 -> 44.1 kHz
 *   start (#36) goes from 0x1f103 straight to the transmitter, which reads
 *   back enabled (0x18a020 = 0x03) and played. Ours adds the AE-9's re-clock
 *   (ASI 4, the 48 kHz descriptors, its card-proven fix for a transmitter
 *   that wouldn't enable on the 384 kHz clock, 2026-11-06) only after a
 *   start whose transmitter didn't enable (ae7.spdif_reclock,
 *   creative_ae7_direct_on()).
 * Captured on the AE-7 at every rate, verb for verb (capture Bw: the first
 * start after each rate change; a same-rate start sends none of it).
 * The DAC is muted (the callers' creative_ae7_mute_checked()) through all
 * of it: between the pin and the DAC's input the DAC reads the wrong kind
 * of signal. Returns the verbs not answered plus the DAC commands not
 * acknowledged; ae7.path_rate = the rate only when none.
 */
int creative_ae7_rate_clock(struct creative_chip *chip, const struct ae7_rate *row)
{
	struct ae9_blk b;
	int bad;

	if (chip->ae7.path_rate == row->rate)
		return 0;
	chip->ae7.path_rate = 0;	/* unknown until all of it went through */
	blk_begin(&b, chip);
	if (row->spdif) {
		u32 p04;

		if (chip->ae7.spdif_reclock) {
			blk_ctl_param(&b, CONTROL_PARAM_ASI, 4);
			blk_chipio_write(&b, AE7_I2S_CLK, AE9_I2S_CLK_48K);
			blk_chipio_write(&b, AE7_I2S_CLK + 4, AE9_I2S_CLK_48K);
			blk_chipio_write(&b, AE7_I2S_RATE, AE9_I2S_RATE_48K);
			blk_chipio_write(&b, AE7_I2S_28, 0x0002000f);
			dev_info(chip->dev, "AE-7: the I2S re-clocked to 48 kHz before the S/PDIF transmitter (it didn't enable last time)\n");
		}
		ae7_blk_asi_class(&b, row->asi_class);
		blk_chipio_write(&b, AE9_SRC_CTL, 0x02);
		blk_pll_pmu_write(&b, 0x01, 0x17);
		blk_pll_pmu_write(&b, AE7_PLL_DAC_PIN, AE7_PIN_SPDIF);
		blk_send(&b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_ADDRESS_LOW, 0x04);
		p04 = ae7_blk_read(&b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PLL_PMU_READ, 0x01, 0);
		blk_pll_pmu_write(&b, 0x04, (p04 & 0xff) | 0x80);
		blk_chipio_write(&b, AE9_SRC_CTL, row->src[0]);
		blk_chipio_write(&b, AE9_SRC_CFG, row->src[1]);
		blk_chipio_write(&b, AE9_SRC_DIV, row->src[2]);
		blk_chipio_write(&b, AE9_SRC_CTL, row->src[3]);
	} else {
		blk_ctl_param(&b, CONTROL_PARAM_ASI, 4);
		blk_pll_pmu_write(&b, AE7_PLL_DAC_PIN, AE7_PIN_I2S);
		ae7_blk_asi_class(&b, row->asi_class);
		blk_chipio_write(&b, AE7_I2S_CLK, row->clk);
		blk_chipio_write(&b, AE7_I2S_CLK + 4, row->clk);
		blk_chipio_write(&b, AE7_I2S_RATE, row->i2s_rate);
		blk_chipio_write(&b, AE7_I2S_28, 0x0002000f);
	}
	blk_end(&b, row->spdif ? "AE-7: S/PDIF transmitter clock" : "AE-7: I2S clock");
	bad = b.errs;
	if (row->spdif) {
		bad += ae7_dac(chip, AE7_DAC, K2M_INPUT, K2M_IN_SPDIF);
		bad += ae7_dac(chip, AE7_DAC, K2M_SPDIF_SRC, K2M_SPDIF_DATA2);
	} else {
		bad += ae7_dac(chip, AE7_DAC, K2M_INPUT, K2M_IN_I2S);
	}
	bad += ae7_dac(chip, AE7_DAC, K2M_CLOCK, row->dac0a);
	if (!bad)
		chip->ae7.path_rate = row->rate;
	return bad;
}

/*
 * Windows' stage 3 after the clock and the walk (FUN_0003c4a4): for the
 * 44.1 kHz family the transmitter's control read and its low byte written
 * back (movzx: capture Bw reads it as written after a set-up, 0x03 / 0x43,
 * and with bit 8 set at every same-rate start, 0x103 / 0x143, and writes
 * 0x03 / 0x43); stream 0x14 started (unless running); ASI 7. Ours: the
 * transmitter's read-back checked (R2's C2; Windows doesn't look): not
 * enabled (the AE-9's 384 kHz trap reads 0x02), the path isn't confirmed
 * (the DAC stays muted), the rate isn't cached and the next start sets it
 * up again, re-clocking the I2S first (ae7.spdif_reclock). Returns the
 * verbs not answered (plus 1 for a transmitter not enabled).
 */
int creative_ae7_direct_on(struct creative_chip *chip, const struct ae7_rate *row)
{
	struct ae9_blk b;
	bool tx_off = false;
	u32 v = 0;

	blk_begin(&b, chip);
	chip->ae7.tx_ctl = -1;
	if (row->spdif) {
		unsigned int errs = b.errs;

		v = ae7_blk_chipio_value(&b, AE9_SRC_CTL, row->src[3]);
		blk_chipio_write(&b, AE9_SRC_CTL, v & 0xff);
		if (b.errs == errs)
			chip->ae7.tx_ctl = v & 0xffff;
		tx_off = (v & 0xff) != row->src[3];
	}
	chip->ae7.s14 = ae7_blk_stream_start(&b, AE9_STREAM_DIRECT) >= 0;
	blk_ctl_param(&b, CONTROL_PARAM_ASI, 7);
	blk_end(&b, "AE-7: Direct Mode, stream 0x14 on");
	if (tx_off) {
		chip->ae7.path_rate = 0;
		chip->ae7.spdif_reclock = true;
		dev_warn(chip->dev, "AE-7: S/PDIF transmitter not enabled (0x18a020 reads 0x%02x, wrote 0x%02x): the DAC stays muted; the next start re-clocks the I2S first\n",
			 v & 0xff, row->src[3]);
	} else if (row->spdif) {
		chip->ae7.spdif_reclock = false;
	}
	return b.errs + tx_off;
}

/* Windows' stage 0 (FUN_0003c0f4(0)) at a Direct stream's stop: stream 0x14 stopped */
void creative_ae7_direct_stop(struct creative_chip *chip)
{
	struct ae9_blk b;

	if (!chip->ae7.s14)
		return;
	blk_begin(&b, chip);
	ae7_blk_stream_stop(&b, AE9_STREAM_DIRECT);
	blk_end(&b, "AE-7: Direct Mode, stream 0x14 off");
	chip->ae7.s14 = false;
}

/*
 * Direct Mode off (Windows' SetDirect(0), FUN_0003be30: FUN_00039a48(1)
 * and FUN_0003c574): the DSP path as the start-up left it: stream 0x05 to
 * the DSP, stream 0x18 (its output) to the DAC's 0xd0 at 96 kHz, 6
 * channels, started unless running; the I2S back at 96 kHz with the DAC
 * on I2S (creative_ae7_rate_clock(), skipped if the card is already
 * there); the AE-7's pin group; ASI 7. Captured: capture A's four switches
 * off, verb for verb, the clock left out as here (no Direct stream had
 * changed it), and capture B's after 48 kHz Direct streams, the 96 kHz
 * clock, 48/01 and 48/0a included. Returns the verbs not answered plus the
 * DAC commands not acknowledged.
 */
int creative_ae7_dsp_path(struct creative_chip *chip)
{
	struct ae9_blk b;
	int bad;

	blk_begin(&b, chip);
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
	ae7_blk_stream_start(&b, AE9_STREAM_DSP_OUT);
	blk_end(&b, "AE-7: Direct Mode off: the DSP's path");
	bad = b.errs;
	bad += creative_ae7_rate_clock(chip, ae7_rate_find(AE7_DSP_RATE));
	blk_begin(&b, chip);
	blk_pll_pmu_write(&b, 0x41, 0xc8);
	blk_pll_pmu_write(&b, 0x45, 0xcc);
	blk_pll_pmu_write(&b, 0x40, 0xcb);
	blk_pll_pmu_write(&b, 0x43, 0xc7);
	blk_pll_pmu_write(&b, 0x51, 0x8d);
	blk_ctl_param(&b, CONTROL_PARAM_ASI, 7);
	blk_end(&b, "AE-7: Direct Mode off: the pin group, ASI 7");
	return bad + b.errs;
}

/*
 * One CA0132 register checked in a block: a ChipIO word read until it
 * equals want, up to 3 reads (a ChipIO read can return another address's
 * value while the card streams: the AE-9's router rule, 2026-11-04); its
 * last value, -1 unread
 */
static int ae7_blk_chipio_check(struct ae9_blk *b, u32 addr, u32 want)
{
	int v = -1;
	unsigned int i;

	for (i = 0; i < 3 && v != (int)want; i++) {
		unsigned int errs = b->errs;
		u32 r = ae7_blk_chipio_value(b, addr, 0);

		if (b->errs == errs)
			v = r & 0x7fffffff;
		else
			b->errs = errs;		/* a retry, not a block error */
	}
	return v;
}

/* a PLL/PMU register (0x70d address, 0xf0c 1, as Windows reads 0x04), up to 2 reads; -1 unread */
static int ae7_blk_pll_check(struct ae9_blk *b, u8 reg, u8 want)
{
	int v = -1;
	unsigned int i;

	for (i = 0; i < 2 && v != want; i++) {
		unsigned int errs = b->errs;
		u32 r;

		blk_send(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_ADDRESS_LOW, reg);
		r = ae7_blk_read(b, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PLL_PMU_READ, 0x01, ~0U);
		v = b->errs == errs ? (int)(r & 0xff) : -1;
		b->errs = errs;
	}
	return v;
}

/*
 * The path read back after a switch or a start (ours; Windows doesn't
 * look), row NULL = the DSP path (96 kHz): the DAC's input and clock
 * register, 48/01 = 0x80 (I2S) or 0x81 (S/PDIF, with 48/0b = 0x12), 48/0a
 * as the rate wants (0x15, sync mode, at 384 kHz); the CA0132's side
 * (R2's C4/C6): the DAC's data pin (PLL/PMU 0x43: 0xc7 on I2S, 0x06 to
 * the transmitter: the one state the DAC's lock can't catch is I2S on the
 * DAC with the transmitter still on its pin, full-scale noise, locked) and
 * on I2S the clock and rate descriptors (0x189000, 0x189024: at 384 kHz
 * they stand in for the lock bit, creative_ae7_unmute()). Kept for debugfs
 * state; returns the registers that differ or weren't read.
 */
int creative_ae7_path_check(struct creative_chip *chip, const struct ae7_rate *row)
{
	struct ae9_blk b;
	u32 raw[2];
	int bad = 0;

	if (!row)
		row = ae7_rate_find(AE7_DSP_RATE);
	chip->ae7.dac01 = creative_dsp_ca0113_read_checked(chip, AE7_DAC, K2M_INPUT, raw);
	bad += chip->ae7.dac01 != (row->spdif ? K2M_IN_SPDIF : K2M_IN_I2S);
	chip->ae7.dac0a = creative_dsp_ca0113_read_checked(chip, AE7_DAC, K2M_CLOCK, raw);
	bad += chip->ae7.dac0a != row->dac0a;
	chip->ae7.dac0b = -1;
	if (row->spdif) {
		chip->ae7.dac0b = creative_dsp_ca0113_read_checked(chip, AE7_DAC, K2M_SPDIF_SRC, raw);
		bad += chip->ae7.dac0b != K2M_SPDIF_DATA2;
	}
	blk_begin(&b, chip);
	chip->ae7.pin43 = ae7_blk_pll_check(&b, AE7_PLL_DAC_PIN, row->spdif ? AE7_PIN_SPDIF : AE7_PIN_I2S);
	bad += chip->ae7.pin43 != (row->spdif ? AE7_PIN_SPDIF : AE7_PIN_I2S);
	chip->ae7.i2s_clk = chip->ae7.i2s_rate = -1;
	if (!row->spdif) {
		chip->ae7.i2s_clk = ae7_blk_chipio_check(&b, AE7_I2S_CLK, row->clk);
		bad += chip->ae7.i2s_clk != (int)row->clk;
		chip->ae7.i2s_rate = ae7_blk_chipio_check(&b, AE7_I2S_RATE, row->i2s_rate);
		bad += chip->ae7.i2s_rate != (int)row->i2s_rate;
	}
	blk_end(&b, "AE-7: the path read back (PLL/PMU 0x43, the I2S descriptors)");
	return bad;
}
