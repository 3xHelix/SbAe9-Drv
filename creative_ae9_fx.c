// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 3xHelix
// Contains code adapted from the Linux kernel's sound/pci/hda/patch_ca0132.c:
//   Copyright (c) 2011, Creative Technology Ltd.
//   Based on patch_ca0110.c, Copyright (c) 2008 Takashi Iwai <tiwai@suse.de>
//
// Creative AE-9: SBX effects and equalizer (ALSA controls).
//
// Everything here is a DSP (SCP) message, as the Windows driver sends them
// when the Creative app's SBX page is used (captures 2026-11-03; see
// WINDOWS-CAPTURE-ANALYSIS.md §38):
//
//   module 0x96 (output effects), request:
//     0x00/0x01 Surround on, level     0x02/0x03 Dialog+ on, level
//     0x04/0x05 Smart Volume on, level 0x06      Smart Volume mode
//     0x07/0x08 Crystalizer on, level  0x18/0x19 Bass (X-Bass) on, level
//     0x17 Bass crossover (Hz)         0x09      EQ on
//     0x0a EQ pre-gain (dB)            0x0b-0x14 EQ bands 31 Hz-16 kHz (dB)
//     0x15 bass redirect, 0x1f use speaker EQ, 0x3a output mute
//   module 0x80 (MASTERCONTROL) request 0x04: speaker config, 8.0 in
//     Direct Mode, the "Speaker Config" control's value with Direct Mode off
//   module 0x8f (speaker EQ) request 0x01: bypass attenuation
//
// On/off values are 0.0/1.0, levels 0.0-1.0 (the app's 0-100), EQ in dB.
// Two stages, as in Windows. "Direct Mode" (default on) is Windows' Direct
// Mode: on, audio goes straight to the DAC at the stream's rate (up to
// 384 kHz) and the DSP's effects are bypassed; off, the DSP routing
// (creative_ae9_dsp_mode: host audio into the DSP, its output to the DAC at
// 96 kHz) puts the effects in the path. "SBX" is the SBX button: with Direct
// Mode off it switches the effects on (Windows' profile sequence: output
// mute, speaker config, speaker block, on/offs, values of what is on, EQ,
// unmute) or off. The effect and EQ controls only act with both stages on.
// With SBX off every effect and the EQ are off in the DSP; the controls
// keep their values for the next time SBX is turned on.

#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/moduleparam.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/tlv.h>

#include "creative_ae9.h"

#define FX_MID_OUT	0x96
#define FX_MID_MASTER	0x80
#define FX_MID_SPKEQ	0x8f

enum { FX_SURROUND, FX_DIALOG, FX_SMARTVOL, FX_CRYSTAL, FX_BASS, FX_COUNT };

static const char * const fx_names[FX_COUNT] = {
	"Surround", "Dialog Plus", "Smart Volume", "Crystalizer", "Bass",
};
static const u8 fx_req_on[FX_COUNT]  = { 0x00, 0x02, 0x04, 0x07, 0x18 };
static const u8 fx_req_lvl[FX_COUNT] = { 0x01, 0x03, 0x05, 0x08, 0x19 };

/* On/off as in Windows' default profile (capture 2: SBX on sent Surround,
 * Smart Volume and Crystalizer on); levels are Creative's stock values
 * (ca0132.c def_vals: 0.67, 0.5, 0.74, 0.65, 0.5). */
static const bool fx_def_on[FX_COUNT]  = { true, false, true, true, false };
static const u8   fx_def_lvl[FX_COUNT] = { 67, 50, 74, 65, 50 };

#define FX_EQ_VALUES	11	/* pre-gain + 10 bands */
static const char * const eq_names[FX_EQ_VALUES] = {
	"EQ Pre-gain", "EQ 31Hz", "EQ 62Hz", "EQ 125Hz", "EQ 250Hz",
	"EQ 500Hz", "EQ 1kHz", "EQ 2kHz", "EQ 4kHz", "EQ 8kHz", "EQ 16kHz",
};

/*
 * EQ presets as the Creative app sends them for the AE-9 (capture 5,
 * 22:59-23:03, every preset in turn; Rock and Gaming match capture 3).
 * Listed A-Z, the game presets A-Z at the end, as asked (cleaner, and
 * closer to the app). Saved settings are restored by name, so the order
 * can change. 0.1 dB, pre-gain first. Pop and Stadium have finer
 * values (Pop 8 kHz 1.19624, Stadium 62 Hz 5.68521), rounded to 0.1 dB.
 * Night sends the same EQ as Concert, and the game presets (Project CARS,
 * Racing, Rocket League, RPG) a flat one: whatever else they do in Windows
 * isn't sent to the card. Other presets: set the bands.
 */
static const char * const eq_preset_names[] = {
	"Acoustic", "Cinema", "Classical", "Concert", "Country", "Dance",
	"Flat", "Gaming", "Hip Hop", "Jazz", "Night", "Pop",
	"R&B", "Rock", "Stadium", "Vocal", "Project CARS", "Racing",
	"Rocket League", "RPG",
};
#define EQ_PRESET_FLAT	6	/* index of "Flat" */
static const s16 eq_presets[][FX_EQ_VALUES] = {
	/* pre  31   62  125  250  500   1k   2k   4k   8k  16k */
	{   0,   0,   0,  20,   0,   0,  25,  20,  20,  20,  30 },	/* Acoustic */
	{   0,   0,   0,   0,   0,  20,  30,   0,   0,  10,  10 },	/* Cinema */
	{   0,   0,   0,  15,  20,  10,  30,  10,   0,  30,  30 },	/* Classical */
	{   0,   0,   0,   0,   0,   0,   0,   0, -10,  30, -50 },	/* Concert */
	{   0,   0,   0,   0,   0,  20,  25,  25,  20,  30,  30 },	/* Country */
	{   0,   0,   0,  10,  15,   5,  15,  20,  10,  25,  25 },	/* Dance */
	{   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0 },	/* Flat */
	{   0,   0,   0,  30, -20,   0,  30,   0,   0,  30,  20 },	/* Gaming */
	{   0,   0,   0,  20,  25,  15,  25,   0, -25, -10, -15 },	/* Hip Hop */
	{   0,   0,   0,   0,  25,  25,  30,  10,  10,  30,  30 },	/* Jazz */
	{   0,   0,   0,   0,   0,   0,   0,   0, -10,  30, -50 },	/* Night */
	{ -10,  17,  26,  21,  27,   7,  34,  12,  56,  60,  52 },	/* Pop */
	{   0,   0,   0,   0, -15,  25,  15,  20,   0,  20,  10 },	/* R&B */
	{   0,   0,   0,  20,  10,   0,  30,  10, -10,  30,  30 },	/* Rock */
	{   0,  39,  57,  43,  30,  36,  35,  33,  40,  60,  68 },	/* Stadium */
	{   0,   0,   0, -15,  -5,  30,  30,  30,   0,   0,  10 },	/* Vocal */
	{   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0 },	/* Project CARS */
	{   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0 },	/* Racing */
	{   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0 },	/* Rocket League */
	{   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0 },	/* RPG */
};

/* Scout Mode (capture 3): Smart Volume on at 0.9, mode 0, and this EQ. */
static const s16 eq_scout[FX_EQ_VALUES] = { 0, 20, 30, 30, -20, -20, 0, 30, 40, 30, 20 };

static const char * const sv_mode_names[] = { "Normal", "Loud", "Night" };

/*
 * MASTERCONTROL 0x04 (speaker config) with Direct Mode off, the "Speaker
 * Config" control. Windows sent 2.0 there in captures 5 and 6 (its speaker
 * setting on 2.1), and 8.0 in Direct Mode; ca0132's table is the same
 * (2.0 -> 1.0, 2.1 -> 2.0, 4.0 -> 5.0, 4.1 -> 6.0, 5.1 -> 8.0; capture
 * 6 sends exactly these). 4.0 and up need 6-channel streams to reach the
 * card's Rear and C/Sub jacks, so they're offered only with surround=1.
 */
static const char * const spk_names[] = { "2.0", "2.1", "4.0", "4.1", "5.1" };
static const u8 spk_cfgs[] = { 1, 2, 5, 6, 8 };	/* MASTERCONTROL 0x04 value */
/* 4.0 and up only with surround=1 (5.1 in DSP mode, experimental) */
#define SPK_STEREO	2
static unsigned int spk_count(void)
{
	return ae9_surround ? ARRAY_SIZE(spk_names) : SPK_STEREO;
}

/* the config in use: a saved 4.0+ falls back to 2.1 without surround */
static unsigned int spk_idx(const struct creative_ae9_fx *fx)
{
	return fx->spk < spk_count() ? fx->spk : 1;
}

static unsigned int fx_spk_cfg = 2;
module_param(fx_spk_cfg, uint, 0444);
MODULE_PARM_DESC(fx_spk_cfg, "Speaker Config at load, Direct Mode off (DSP MASTERCONTROL 0x04): 1 = 2.0, 2 = 2.1 (default, as Windows)");

/* num/den as IEEE-754 single bits, without the FPU. |num/den| >= 2^-16. */
static u32 fx_f32(s32 num, u32 den)
{
	u64 q;
	u32 sign = num < 0 ? BIT(31) : 0, mant;
	int p, e;

	if (!num)
		return 0;
	q = div_u64((u64)abs(num) << 40, den);	/* value * 2^40 */
	p = fls64(q) - 1;			/* value in [2^(p-40), 2^(p-39)) */
	e = p - 40;
	/* 24 significant bits, rounded to nearest */
	if (p > 23)
		mant = (u32)((q + (1ULL << (p - 24))) >> (p - 23));
	else
		mant = (u32)(q << (23 - p));
	if (mant >> 24) {			/* rounding carried over */
		mant >>= 1;
		e++;
	}
	return sign | ((u32)(e + 127) << 23) | (mant & 0x7fffff);
}

/*
 * One DSP message, paced (Windows leaves ~3 ms inside a profile switch and
 * ~150 ms between slider steps). Three failures in a row mean the DSP has
 * stopped answering (seen once, 2026-11-03: 747 timeouts until a resume);
 * then nothing more is sent until the next bring-up (creative_ae9_fx_restore).
 */
static int fx_send(struct creative_chip *chip, u8 mid, u8 req, u32 val,
		   bool log)
{
	struct creative_ae9_fx *fx = &chip->fx;
	int ret;

	if (fx->dead)
		return -EIO;
	usleep_range(5000, 6000);
	ret = creative_dsp_scp_send(chip, ((u32)req << 17) | (0x20 << 8) | mid,
				    &val, 1);
	if (ret < 0)
		dev_info(chip->dev, "fx: 0x%02x/0x%02x = 0x%08x FAILED\n", mid, req, val);
	else if (log)
		ae9_dbg(chip, "fx: 0x%02x/0x%02x = 0x%08x\n", mid, req, val);
	if (ret >= 0) {
		fx->fails = 0;
	} else if (++fx->fails >= 3) {
		fx->dead = true;
		dev_warn(chip->dev, "fx: the DSP stopped answering; no more effect changes until the next bring-up (sleep/wake or reboot)\n");
	}
	return ret;
}

/* Effects run only with Direct Mode off and SBX on. */
static bool fx_eff(const struct creative_ae9_fx *fx)
{
	return !fx->direct && fx->sbx;
}

static bool fx_ready(struct creative_chip *chip)
{
	return chip->corb_buf && chip->dsp_done;
}

/* Windows' SBX profile sequence; with SBX off, everything off. */
static void fx_apply_all_locked(struct creative_chip *chip)
{
	struct creative_ae9_fx *fx = &chip->fx;
	unsigned int i;

	bool on;

	/* Windows' order (capture 3, 19:56:08): mute, speaker config, speaker
	 * block, on/offs, then values only for what is on, EQ on last. */
	fx_send(chip, FX_MID_OUT, 0x3a, fx_f32(1, 1), false);
	fx_send(chip, FX_MID_MASTER, 0x04,
		fx_f32(fx->direct ? 8 : spk_cfgs[spk_idx(fx)], 1), true);
	fx_send(chip, FX_MID_OUT, 0x19, fx_f32(fx->level[FX_BASS], 100), false);
	fx_send(chip, FX_MID_OUT, 0x17, fx_f32(80, 1), false);
	fx_send(chip, FX_MID_OUT, 0x18, 0, false);
	fx_send(chip, FX_MID_SPKEQ, 0x01, 0, false);
	fx_send(chip, FX_MID_OUT, 0x1f, 0, false);
	fx_send(chip, FX_MID_OUT, 0x15, 0, false);
	/* full range on every pair (front, center/LFE, rear, surround), sent
	 * by Windows after each speaker config change in DSP mode (captures 5
	 * and 6); otherwise the DSP redirects the bass to the LFE channel,
	 * which the AE-9's stereo outputs don't have */
	for (i = 0; i < 4; i++)
		fx_send(chip, FX_MID_OUT, 0x1a + i, fx_f32(1, 1), false);
	for (i = 0; i < FX_COUNT; i++)
		fx_send(chip, FX_MID_OUT, fx_req_on[i],
			fx_f32(fx_eff(fx) && fx->on[i], 1), false);
	for (i = 0; fx_eff(fx) && i < FX_COUNT; i++) {
		if (!fx->on[i])
			continue;
		if (i == FX_SMARTVOL)
			fx_send(chip, FX_MID_OUT, 0x06, fx_f32(fx->sv_mode, 1), false);
		fx_send(chip, FX_MID_OUT, fx_req_lvl[i],
			fx_f32(fx->level[i], 100), false);
	}
	on = fx_eff(fx) && fx->eq_on;
	for (i = 1; on && i < FX_EQ_VALUES; i++)
		fx_send(chip, FX_MID_OUT, 0x0a + i, fx_f32(fx->eq[i], 10), false);
	if (on)
		fx_send(chip, FX_MID_OUT, 0x0a, fx_f32(fx->eq[0], 10), false);
	fx_send(chip, FX_MID_OUT, 0x09, fx_f32(on, 1), false);
	fx_send(chip, FX_MID_OUT, 0x3a, 0, false);
}

static void fx_apply_all(struct creative_chip *chip)
{
	if (!fx_ready(chip))
		return;
	mutex_lock(&chip->replay_mutex);
	fx_apply_all_locked(chip);
	creative_dsp_chipio_addr_invalidate();
	mutex_unlock(&chip->replay_mutex);
	dev_info(chip->dev, "Direct Mode %s, SBX %s (Surround %d, Dialog+ %d, Smart Volume %d, Crystalizer %d, Bass %d, EQ %d%s)\n",
		 chip->fx.direct ? "on" : "off", chip->fx.sbx ? "on" : "off",
		 chip->fx.on[FX_SURROUND],
		 chip->fx.on[FX_DIALOG], chip->fx.on[FX_SMARTVOL],
		 chip->fx.on[FX_CRYSTAL], chip->fx.on[FX_BASS], chip->fx.eq_on,
		 chip->fx.scout ? ", Scout Mode" : "");
}

/*
 * Messages for one control change. Like Windows (whose app greys out what
 * is off), a value only goes to the DSP while its effect, or the EQ, is on;
 * turning one on sends its stored values with it. Sending EQ values with
 * the EQ off is the one difference we know of from the DSP hang above.
 */
static void fx_live_begin(struct creative_chip *chip)
{
	mutex_lock(&chip->replay_mutex);
}

static void fx_live_end(struct creative_chip *chip)
{
	creative_dsp_chipio_addr_invalidate();
	mutex_unlock(&chip->replay_mutex);
}

static bool fx_live(struct creative_chip *chip)
{
	/* not before the switch to DSP mode: the DSP is still in its startup
	 * state (boot-time control restore); the switch sends the profile */
	return fx_eff(&chip->fx) && fx_ready(chip) && !chip->fx.dead &&
	       !chip->dsp_switch_pending;
}

static void fx_live_effect(struct creative_chip *chip, unsigned int i)
{
	struct creative_ae9_fx *fx = &chip->fx;

	fx_live_begin(chip);
	fx_send(chip, FX_MID_OUT, fx_req_on[i], fx_f32(fx->on[i], 1), true);
	if (fx->on[i]) {
		if (i == FX_SMARTVOL)
			fx_send(chip, FX_MID_OUT, 0x06, fx_f32(fx->sv_mode, 1), true);
		fx_send(chip, FX_MID_OUT, fx_req_lvl[i],
			fx_f32(fx->level[i], 100), true);
	}
	fx_live_end(chip);
}

static void fx_live_eq(struct creative_chip *chip, bool values, bool onoff)
{
	struct creative_ae9_fx *fx = &chip->fx;
	unsigned int i;

	fx_live_begin(chip);
	for (i = 1; values && fx->eq_on && i < FX_EQ_VALUES; i++)
		fx_send(chip, FX_MID_OUT, 0x0a + i, fx_f32(fx->eq[i], 10), true);
	if (values && fx->eq_on)
		fx_send(chip, FX_MID_OUT, 0x0a, fx_f32(fx->eq[0], 10), true);
	if (onoff)
		fx_send(chip, FX_MID_OUT, 0x09, fx_f32(fx->eq_on, 1), true);
	fx_live_end(chip);
}

static void fx_live_one(struct creative_chip *chip, u8 req, u32 val)
{
	fx_live_begin(chip);
	fx_send(chip, FX_MID_OUT, req, val, true);
	fx_live_end(chip);
}

/* After a bring-up (probe or resume) the DSP is back at its startup state:
 * effects off, speaker config 8. Only SBX on needs sending again. */
void creative_ae9_fx_restore(struct creative_chip *chip)
{
	chip->fx.dead = false;
	chip->fx.fails = 0;
	if (chip->fx.direct)
		return;
	creative_ae9_dsp_mode(chip, true);
	fx_apply_all(chip);
}

/*
 * After a bring-up with Direct Mode off: the DSP is back at its startup
 * state. Don't switch now (no stream has run yet, see dsp_switch_work):
 * the next stream starts direct and is switched live once it runs.
 */
void creative_ae9_fx_defer(struct creative_chip *chip)
{
	chip->fx.dead = false;
	chip->fx.fails = 0;
	if (!chip->fx.direct)
		chip->dsp_switch_pending = true;
}

void creative_ae9_fx_init(struct creative_chip *chip)
{
	struct creative_ae9_fx *fx = &chip->fx;
	unsigned int i;

	memset(fx, 0, sizeof(*fx));
	fx->eq_preset = EQ_PRESET_FLAT;	/* the EQ values above are flat (0) */
	fx->direct = true;
	fx->spk = fx_spk_cfg == 1 ? 0 : 1;
	for (i = 0; i < FX_COUNT; i++) {
		fx->on[i] = fx_def_on[i];
		fx->level[i] = fx_def_lvl[i];
	}
}

/* ------------------------------------------------------------------ */
/* Controls: private_value = kind << 8 | index                         */
/* ------------------------------------------------------------------ */

enum { K_SBX, K_ON, K_LVL, K_SVMODE, K_EQON, K_EQ, K_PRESET, K_SCOUT, K_DIRECT, K_SPK };
#define PV(k, i)	((k) << 8 | (i))
#define PV_K(pv)	((pv) >> 8)
#define PV_I(pv)	((pv) & 0xff)

static const DECLARE_TLV_DB_SCALE(eq_tlv, -1200, 100, 0);

static int fx_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	switch (PV_K(kc->private_value)) {
	case K_SBX: case K_ON: case K_EQON: case K_SCOUT: case K_DIRECT:
		return snd_ctl_boolean_mono_info(kc, ui);
	case K_SVMODE:
		return snd_ctl_enum_info(ui, 1, ARRAY_SIZE(sv_mode_names),
					 sv_mode_names);
	case K_PRESET:
		return snd_ctl_enum_info(ui, 1, ARRAY_SIZE(eq_preset_names),
					 eq_preset_names);
	case K_SPK:
		return snd_ctl_enum_info(ui, 1, spk_count(), spk_names);
	}
	ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	ui->count = 1;
	ui->value.integer.min = 0;
	ui->value.integer.max = PV_K(kc->private_value) == K_EQ ? 24 : 100;
	return 0;
}

static int fx_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	struct creative_ae9_fx *fx = &chip->fx;
	unsigned int i = PV_I(kc->private_value);
	long *v = uv->value.integer.value;

	switch (PV_K(kc->private_value)) {
	case K_DIRECT:	*v = fx->direct; break;
	case K_SBX:	*v = fx->sbx; break;
	case K_ON:	*v = fx->on[i]; break;
	case K_LVL:	*v = fx->level[i]; break;
	case K_EQON:	*v = fx->eq_on; break;
	case K_EQ:	*v = DIV_ROUND_CLOSEST(fx->eq[i], 10) + 12; break;	/* whole dB */
	case K_SCOUT:	*v = fx->scout; break;
	case K_SVMODE:	uv->value.enumerated.item[0] = fx->sv_mode; break;
	case K_PRESET:	uv->value.enumerated.item[0] = fx->eq_preset; break;
	case K_SPK:	uv->value.enumerated.item[0] = spk_idx(fx); break;
	}
	return 0;
}

static void fx_scout(struct creative_chip *chip, bool on)
{
	struct creative_ae9_fx *fx = &chip->fx;

	if (on) {
		fx->saved_sv_on = fx->on[FX_SMARTVOL];
		fx->saved_sv_level = fx->level[FX_SMARTVOL];
		fx->saved_sv_mode = fx->sv_mode;
		fx->saved_eq_on = fx->eq_on;
		memcpy(fx->saved_eq, fx->eq, sizeof(fx->eq));
		fx->on[FX_SMARTVOL] = true;
		fx->level[FX_SMARTVOL] = 90;
		fx->sv_mode = 0;
		fx->eq_on = true;
		memcpy(fx->eq, eq_scout, sizeof(fx->eq));
	} else {
		fx->on[FX_SMARTVOL] = fx->saved_sv_on;
		fx->level[FX_SMARTVOL] = fx->saved_sv_level;
		fx->sv_mode = fx->saved_sv_mode;
		fx->eq_on = fx->saved_eq_on;
		memcpy(fx->eq, fx->saved_eq, sizeof(fx->eq));
	}
	fx->scout = on;
}

/*
 * Two stages, as in Windows: Direct Mode off makes the SBX switch usable,
 * SBX on makes the effect and EQ controls usable. The rest are marked
 * inactive (greyed out in mixers); writes to them are kept for later but
 * don't reach the DSP.
 */
static void fx_update_active(struct creative_chip *chip)
{
	struct creative_ae9_fx *fx = &chip->fx;
	unsigned int i;

	for (i = 0; i < fx->nkc; i++) {
		struct snd_kcontrol *kc = fx->kc[i];
		unsigned int k = PV_K(kc->private_value), old = kc->vd[0].access;
		bool active = k == K_DIRECT ? true :
			      k == K_SBX || k == K_SPK ? !fx->direct : fx_eff(fx);

		if (active)
			kc->vd[0].access &= ~SNDRV_CTL_ELEM_ACCESS_INACTIVE;
		else
			kc->vd[0].access |= SNDRV_CTL_ELEM_ACCESS_INACTIVE;
		if (kc->vd[0].access != old)
			snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_INFO, &kc->id);
	}
}

/* A preset or Scout Mode changed other controls' values: tell the desktop. */
static void fx_notify_others(struct creative_chip *chip, struct snd_kcontrol *self)
{
	unsigned int i;

	for (i = 0; i < chip->fx.nkc; i++)
		if (chip->fx.kc[i] != self)
			snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &chip->fx.kc[i]->id);
}

static int fx_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	struct creative_ae9_fx *fx = &chip->fx;
	unsigned int i = PV_I(kc->private_value);
	long v = uv->value.integer.value[0];
	unsigned int e = uv->value.enumerated.item[0];

	switch (PV_K(kc->private_value)) {
	case K_DIRECT:
		if (!!v == fx->direct)
			return 0;
		fx->direct = !!v;
		if (!fx->direct && !(chip->pcm_start_sent && chip->dsp_primed)) {
			/* nothing playing: switch at the next stream (see
			 * creative_ae9_dsp_switch_work) */
			chip->dsp_switch_pending = true;
		} else if (!fx->direct && chip->pcm_start_rate != 48000) {
			/* a direct stream above 48 kHz can't go through the DSP
			 * (see pcm_open): it plays on direct, and the next
			 * stream opens at 48 kHz and switches */
			chip->dsp_switch_pending = true;
			dev_info(chip->dev, "Direct Mode off: the stream plays at %u Hz, so it stays direct; DSP mode starts with the next stream (48 kHz)\n",
				 chip->pcm_start_rate);
		} else if (!fx->direct) {
			/* Windows' Direct Mode off, live: DSP routing, profile */
			chip->dsp_switch_pending = false;
			creative_ae9_dsp_mode(chip, true);
			fx_apply_all(chip);
		} else {
			chip->dsp_switch_pending = false;
			/* effects off (speaker config 8), then route direct */
			fx_apply_all(chip);
			creative_ae9_dsp_mode(chip, false);
		}
		fx_update_active(chip);
		return 1;
	case K_SBX:
		if (!!v == fx->sbx)
			return 0;
		/* locked with Direct Mode on, like the head unit's SBX button:
		 * no change, and a notify so mixers snap back */
		if (fx->direct) {
			snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE, &kc->id);
			return 0;
		}
		fx->sbx = !!v;
		if (!chip->dsp_switch_pending)
			fx_apply_all(chip);
		fx_update_active(chip);
		return 1;
	case K_ON:
		if (!!v == fx->on[i])
			return 0;
		fx->on[i] = !!v;
		if (fx_live(chip))
			fx_live_effect(chip, i);
		return 1;
	case K_LVL:
		v = clamp_t(long, v, 0, 100);
		if (v == fx->level[i])
			return 0;
		fx->level[i] = v;
		if (fx_live(chip) && fx->on[i])
			fx_live_one(chip, fx_req_lvl[i], fx_f32(v, 100));
		return 1;
	case K_SVMODE:
		if (e >= ARRAY_SIZE(sv_mode_names) || e == fx->sv_mode)
			return 0;
		fx->sv_mode = e;
		if (fx_live(chip) && fx->on[FX_SMARTVOL])
			fx_live_one(chip, 0x06, fx_f32(e, 1));
		return 1;
	case K_EQON:
		if (!!v == fx->eq_on)
			return 0;
		fx->eq_on = !!v;
		if (fx_live(chip))
			fx_live_eq(chip, fx->eq_on, true);
		return 1;
	case K_EQ:
		v = clamp_t(long, v, 0, 24) - 12;
		if (v == DIV_ROUND_CLOSEST(fx->eq[i], 10))
			return 0;	/* also keeps a preset's 0.1 dB value */
		fx->eq[i] = v * 10;
		if (fx_live(chip) && fx->eq_on)
			fx_live_one(chip, 0x0a + i, fx_f32(v, 1));
		return 1;
	case K_PRESET:
		if (e >= ARRAY_SIZE(eq_preset_names))
			return 0;
		fx->eq_preset = e;
		memcpy(fx->eq, eq_presets[e], sizeof(fx->eq));
		if (fx_live(chip))
			fx_live_eq(chip, true, false);
		fx_notify_others(chip, kc);
		return 1;
	case K_SPK:
		if (e >= spk_count() || e == fx->spk)
			return 0;
		fx->spk = e;
		if (!fx->direct && !chip->dsp_switch_pending)
			fx_apply_all(chip);	/* speaker config + full range, as Windows */
		return 1;
	case K_SCOUT:
		if (!!v == fx->scout)
			return 0;
		fx_scout(chip, !!v);
		if (fx_eff(fx) && !chip->dsp_switch_pending)
			fx_apply_all(chip);
		fx_notify_others(chip, kc);
		return 1;
	}
	return 0;
}

/*
 * Speaker Config follows the stream (surround=1): picking the 5.1 profile
 * in the desktop reopens the card at 6 channels, so a stereo layout (2.0,
 * 2.1) becomes 5.1; back on a 2-channel stream, 5.1 becomes 2.1. 4.0 and
 * 4.1 are the user's choice and stay. Mixers and SbAe9-SBC get notified.
 */
void creative_ae9_fx_spk_follow(struct creative_chip *chip, unsigned int channels)
{
	struct creative_ae9_fx *fx = &chip->fx;
	unsigned int want = fx->spk, i;

	if (!ae9_surround)
		return;
	if (channels == 6 && spk_idx(fx) <= 1)
		want = 4;		/* 5.1 */
	else if (channels == 2 && spk_idx(fx) == 4)
		want = 1;		/* 2.1 */
	if (want == fx->spk)
		return;
	fx->spk = want;
	dev_info(chip->dev, "Speaker Config %s (follows the %u-channel stream)\n",
		 spk_names[want], channels);
	if (!fx->direct && !chip->dsp_switch_pending)
		fx_apply_all(chip);
	for (i = 0; i < fx->nkc; i++)
		if (PV_K(fx->kc[i]->private_value) == K_SPK)
			snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &fx->kc[i]->id);
}

/*
 * The head unit's SBX button: the same as flipping "SBX Switch", and only
 * with Direct Mode off. With Direct Mode on it does nothing.
 */
void creative_ae9_fx_sbx_toggle(struct creative_chip *chip)
{
	struct creative_ae9_fx *fx = &chip->fx;
	unsigned int i;

	if (fx->direct) {
		ae9_dbg(chip, "SBX button ignored: Direct Mode is on\n");
		return;
	}
	fx->sbx = !fx->sbx;
	if (!chip->dsp_switch_pending)
		fx_apply_all(chip);
	else
		dev_info(chip->dev, "SBX %s (button; applied when the DSP switch runs)\n",
			 fx->sbx ? "on" : "off");
	fx_update_active(chip);
	for (i = 0; i < fx->nkc; i++)
		if (PV_K(fx->kc[i]->private_value) == K_SBX)
			snd_ctl_notify(chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &fx->kc[i]->id);
}

static int fx_add(struct creative_chip *chip, const char *name,
		  unsigned int kind, unsigned int idx)
{
	struct snd_kcontrol_new kn = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = name,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info = fx_info,
		.get = fx_get,
		.put = fx_put,
		.private_value = PV(kind, idx),
	};

	struct snd_kcontrol *kc;
	int err;

	if (kind == K_EQ) {
		kn.access |= SNDRV_CTL_ELEM_ACCESS_TLV_READ;
		kn.tlv.p = eq_tlv;
	}
	kc = snd_ctl_new1(&kn, chip);
	err = snd_ctl_add(chip->card, kc);	/* frees kc on error */
	if (!err && chip->fx.nkc < ARRAY_SIZE(chip->fx.kc))
		chip->fx.kc[chip->fx.nkc++] = kc;
	return err;
}

/*
 * Names avoid PipeWire's mixer-path element names ("Surround", "Bass",
 * "Treble" ...), so the desktop leaves them alone.
 */
int creative_ae9_fx_controls_add(struct creative_chip *chip)
{
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	unsigned int i;
	int err;

	err = fx_add(chip, "Direct Mode Switch", K_DIRECT, 0);
	if (!err)
		err = fx_add(chip, "SBX Switch", K_SBX, 0);
	if (!err)
		err = fx_add(chip, "Speaker Config", K_SPK, 0);
	for (i = 0; !err && i < FX_COUNT; i++) {
		snprintf(name, sizeof(name), "FX %s Switch", fx_names[i]);
		err = fx_add(chip, name, K_ON, i);
		if (err)
			break;
		snprintf(name, sizeof(name), "FX %s Volume", fx_names[i]);
		err = fx_add(chip, name, K_LVL, i);
	}
	if (!err)
		err = fx_add(chip, "FX Smart Volume Mode", K_SVMODE, 0);
	if (!err)
		err = fx_add(chip, "EQ Switch", K_EQON, 0);
	if (!err)
		err = fx_add(chip, "EQ Preset", K_PRESET, 0);
	for (i = 0; !err && i < FX_EQ_VALUES; i++) {
		snprintf(name, sizeof(name), "%s Volume", eq_names[i]);
		err = fx_add(chip, name, K_EQ, i);
	}
	if (!err)
		err = fx_add(chip, "Scout Mode Switch", K_SCOUT, 0);
	fx_update_active(chip);
	return err;
}
