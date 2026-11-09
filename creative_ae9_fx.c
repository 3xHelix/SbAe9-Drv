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
//     0x15 bass redirect, 0x16 its crossover (Hz), 0x1e subwoofer boost (on/off)
//     0x1f use speaker EQ, 0x3a output mute
//     0x21-0x26 speaker level (dB), 0x31-0x36 speaker delay (seconds),
//               Front L, Front R, Center, Sub, Rear L, Rear R
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

#include <linux/ctype.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
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

/*
 * Speaker setup (Sound Blaster Command > Speakers; capture 2026-11-07,
 * notes 12). Windows sends these with Direct Mode off:
 * - Bass redirection on/off (0x96/0x15); its crossover 0x96/0x16 comes
 *   from the speaker type (Tower 80 Hz, Desktop 120 Hz); sub gain 0x96/0x1e.
 *   Our startup leaves 0x16/0x1e out (the DSP rejects them at boot);
 *   Windows sends them at run time, after the speaker config.
 * - Calibration: a level and a delay per speaker. Distance steps of 10 cm
 *   sent 0.000293867 s each (sound at 340.29 m/s), so the control is in cm.
 *   The levels look like dB offsets (0 = as is; a raise sent 1.0).
 * Sent with every speaker block (fx_apply_all_locked) and live.
 */
static const char * const spk_ch_names[6] = {
	"Front L", "Front R", "Center", "Sub", "Rear L", "Rear R",
};
#define SPK_LVL_MIN	(-12)	/* dB */
#define SPK_LVL_MAX	12
#define SPK_DIST_MAX	500	/* cm (14.7 ms) */
#define XOVER_MIN	50	/* Hz */
#define XOVER_MAX	200
#define XOVER_STEP	10
#define XOVER_DEF	80	/* Tower; Desktop is 120 */
#define SOUND_CM_S	34029	/* Windows' 10 cm = 0.000293867 s */

/*
 * Headphone profiles (Sound Blaster Command's headphone list; notes 13):
 * a speaker EQ image per model, uploaded into the DSP and switched on with
 * 0x96/0x1f ("use speaker EQ") while the output is headphones and Direct
 * Mode is off. The images are Creative's and not shipped: ae9-get-firmware
 * extracts them from Creative's installer (AE9HPEQ/<key>.reg) into
 * /lib/firmware/creative-ae9/hp/<key>.bin. Keys as in Creative's registry
 * files, names as Sound Blaster Command shows them.
 *
 * Four custom slots (not in Windows) at the end of the list, so saved
 * picks of Creative's profiles keep their numbers: the same format, two
 * 512-tap linear-phase FIRs at 48 kHz (left, right), made from a published
 * measurement by ae9-hp-profile. hp_custom names them ("DT990=Beyerdynamic
 * DT 990 PRO;DT880=..."): slot N loads hp/<KEY>.bin and shows the name; an
 * empty slot shows "Custom N" and loads hp/custom<N>.bin.
 */
static const struct { const char *key, *name; } hp_profiles[] = {
	{ NULL,		"Off" },
	{ "AKGM220",	"AKG M220" },
	{ "IP3EP",	"Apple Earphone" },
	{ "IP5EP",	"Apple Earpod" },
	{ "ATHM50",	"audio-technica ATH-M50" },
	{ "HS50",	"CORSAIR HS50" },
	{ "CTAVNA",	"Creative Aurvana Air" },
	{ "CTAVNG",	"Creative Aurvana Gold" },
	{ "CTAVNL",	"Creative Aurvana Live!" },
	{ "CTAVNL2",	"Creative Aurvana Live!2" },
	{ "CTEP3NC",	"Creative EP-3NC" },
	{ "CTFATGHS",	"Creative Fatal1ty Gaming Headset" },
	{ "CTFlex",	"Creative Flex" },
	{ "OUTLIER",	"Creative Outlier" },
	{ "OUTLIERB",	"Creative Outlier Black" },
	{ "CTSBINF",	"Creative SB Inferno Headset" },
	{ "SXFIAIR",	"Creative SXFI AIR" },
	{ "SXFIAIRC",	"Creative SXFI AIR C" },
	{ "EMUTEAK",	"E-MU Teak" },
	{ "WALNUT",	"E-MU Walnut" },
	{ "HXHSCF",	"HYPERX HX-HSCF" },
	{ "ETHERC",	"Mr Speakers ETHER C" },
	{ "PFX2HR",	"Philips Fidelio X2HR" },
	{ "GS3EP",	"Samsung Galaxy Headset" },
	{ "HD201",	"SENNHEISER HD 201" },
	{ "HD280Pro",	"SENNHEISER HD 280 Pro" },
	{ "HD457",	"SENNHEISER HD 457" },
	{ "MDR1R",	"SONY MDR-1R" },
	{ "SBBLAZE",	"Sound Blaster Blaze" },
	{ "H3",		"Sound BlasterX H3" },
	{ "H5",		"Sound BlasterX H5 TE" },
	{ "H7",		"Sound BlasterX H7 TE" },
	{ "P5",		"Sound BlasterX P5" },
	{ "custom1",	"Custom 1" },
	{ "custom2",	"Custom 2" },
	{ "custom3",	"Custom 3" },
	{ "custom4",	"Custom 4" },
};

#define HP_CUSTOM	4
#define HP_CUSTOM0	(ARRAY_SIZE(hp_profiles) - HP_CUSTOM)
#define HP_KEY_MAX	24
#define HP_NAME_MAX	40

static struct {
	char key[HP_KEY_MAX + 1], name[HP_NAME_MAX + 1];
} hp_custom_slot[HP_CUSTOM];
static char hp_custom_val[HP_CUSTOM * (HP_KEY_MAX + HP_NAME_MAX + 2) + 1];
static DEFINE_MUTEX(hp_custom_lock);
static struct creative_chip *hp_custom_chip;	/* for the names' change event */
static struct snd_kcontrol *hp_custom_kc;

static bool hp_key_ok(const char *k)
{
	if (!*k || *k == '.')
		return false;
	for (; *k; k++)
		if (!isalnum(*k) && *k != '_' && *k != '-' && *k != '.')
			return false;
	return true;
}

/* "KEY=Name;KEY=Name;;KEY=Name": slot by position, empty = unused */
static int hp_custom_set(const char *val, const struct kernel_param *kp)
{
	typeof(hp_custom_slot) s = {};
	char *buf, *p, *ent;
	unsigned int n = 0;
	int ret = 0;

	if (strlen(val) >= sizeof(hp_custom_val))
		return -ENOSPC;
	buf = kstrdup(val, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	p = strim(buf);
	while (p && !ret) {
		char *eq;

		ent = strsep(&p, ";");
		ent = strim(ent);
		if (n >= HP_CUSTOM) {
			ret = *ent ? -E2BIG : 0;
			continue;
		}
		if (*ent) {
			eq = strchr(ent, '=');
			if (eq)
				*eq++ = '\0';
			ent = strim(ent);
			eq = eq ? strim(eq) : ent;
			if (strlen(ent) > HP_KEY_MAX || !hp_key_ok(ent) ||
			    strlen(eq) > HP_NAME_MAX || !*eq) {
				ret = -EINVAL;
				continue;
			}
			strscpy(s[n].key, ent, sizeof(s[n].key));
			strscpy(s[n].name, eq, sizeof(s[n].name));
		}
		n++;
	}
	kfree(buf);
	if (ret)
		return ret;
	mutex_lock(&hp_custom_lock);
	memcpy(hp_custom_slot, s, sizeof(s));
	hp_custom_val[0] = '\0';	/* the same, tidied: "KEY=Name;;KEY=Name" */
	for (n = 0; n < HP_CUSTOM; n++) {
		size_t l = strlen(hp_custom_val);

		if (n)
			l += scnprintf(hp_custom_val + l, sizeof(hp_custom_val) - l, ";");
		if (s[n].key[0])
			scnprintf(hp_custom_val + l, sizeof(hp_custom_val) - l, "%s=%s",
				  s[n].key, s[n].name);
	}
	if (hp_custom_chip && hp_custom_kc)	/* SbAe9-SBC & co. re-read the names */
		snd_ctl_notify(hp_custom_chip->card, SNDRV_CTL_EVENT_MASK_INFO,
			       &hp_custom_kc->id);
	mutex_unlock(&hp_custom_lock);
	return 0;
}

static int hp_custom_get(char *buf, const struct kernel_param *kp)
{
	int n;

	mutex_lock(&hp_custom_lock);
	n = scnprintf(buf, PAGE_SIZE, "%s\n", hp_custom_val);
	mutex_unlock(&hp_custom_lock);
	return n;
}

static const struct kernel_param_ops hp_custom_ops = {
	.set = hp_custom_set,
	.get = hp_custom_get,
};
module_param_cb(hp_custom, &hp_custom_ops, NULL, 0644);
MODULE_PARM_DESC(hp_custom, "Custom headphone profiles, by slot: \"KEY=Name;KEY=Name;...\" (up to 4; slot N loads creative-ae9/hp/<KEY>.bin and shows Name); written by ae9-hp-profile");

/* firmware key (hp/<key>.bin) and shown name of profile e */
static void hp_profile_id(unsigned int e, char *key, size_t klen,
			  char *name, size_t nlen)
{
	unsigned int c = e - HP_CUSTOM0;

	if (e < HP_CUSTOM0) {
		if (key)
			strscpy(key, hp_profiles[e].key ?: "", klen);
		if (name)
			strscpy(name, hp_profiles[e].name, nlen);
		return;
	}
	mutex_lock(&hp_custom_lock);
	if (key)
		strscpy(key, hp_custom_slot[c].key[0] ? hp_custom_slot[c].key :
			hp_profiles[e].key, klen);
	if (name)
		strscpy(name, hp_custom_slot[c].name[0] ? hp_custom_slot[c].name :
			hp_profiles[e].name, nlen);
	mutex_unlock(&hp_custom_lock);
}

void creative_ae9_fx_remove(struct creative_chip *chip)
{
	mutex_lock(&hp_custom_lock);
	if (hp_custom_chip == chip) {
		hp_custom_chip = NULL;
		hp_custom_kc = NULL;
	}
	mutex_unlock(&hp_custom_lock);
}

static unsigned int fx_spk_cfg = 2;
module_param(fx_spk_cfg, uint, 0444);
MODULE_PARM_DESC(fx_spk_cfg, "Speaker Config at load, Direct Mode off (DSP MASTERCONTROL 0x04): 1 = 2.0, 2 = 2.1 (default, as Windows)");

static bool hp_live_upload;
module_param(hp_live_upload, bool, 0644);
MODULE_PARM_DESC(hp_live_upload, "Upload headphone profiles while audio streams run (default off: the upload waits until every stream has stopped; uploads during playback lost DSP replies and broke the DSP until a power-off)");

static bool eq_headroom = true;
module_param(eq_headroom, bool, 0644);
MODULE_PARM_DESC(eq_headroom, "With a headphone profile and the EQ both on, send the DSP the EQ pre-gain 12 dB lower, not below -12 dB (default on; stops the DSP's overload, the \"metallic\" sound)");

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
static int __fx_send(struct creative_chip *chip, u8 mid, u8 req, u32 val,
		     bool log, bool soft)
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
	} else if (soft) {
		/* a setting the DSP may refuse in some states: not a sign of
		 * a stuck DSP */
	} else if (++fx->fails >= 3) {
		fx->dead = true;
		dev_warn(chip->dev, "fx: the DSP stopped answering; no more effect changes until the next bring-up (sleep/wake or reboot)\n");
	}
	return ret;
}

static int fx_send(struct creative_chip *chip, u8 mid, u8 req, u32 val,
		   bool log)
{
	return __fx_send(chip, mid, req, val, log, false);
}

/* Bass redirection's crossover and sub gain: the DSP rejects them at boot,
 * so a refusal doesn't count towards "stopped answering". */
static void fx_send_redirect(struct creative_chip *chip, bool log)
{
	struct creative_ae9_fx *fx = &chip->fx;

	__fx_send(chip, FX_MID_OUT, 0x16, fx_f32(fx->xover, 1), log, true);
	__fx_send(chip, FX_MID_OUT, 0x1e, fx_f32(fx->sub_gain, 1), log, true);
}

/* One speaker's calibration: level 0x21+i (dB), delay 0x31+i (seconds). */
static u32 spk_level_val(const struct creative_ae9_fx *fx, unsigned int i)
{
	return fx->direct ? 0 : fx_f32(fx->spk_level[i], 1);
}

static u32 spk_delay_val(const struct creative_ae9_fx *fx, unsigned int i)
{
	return fx->direct ? 0 : fx_f32(fx->spk_dist[i], SOUND_CM_S);
}

/*
 * Headphone profile picks are debounced. Each upload is a few hundred
 * commands to the card; seven in 3 s (a fast scroll through the list)
 * choked the command ring, the DSP's DMA arm reply was lost and the DSP
 * stopped answering until a power-off (2026-11-07, 12:42:34). So: upload
 * only the pick still standing after HP_SETTLE_MS, at most one upload per
 * HP_GAP_MS, and retry once an upload that failed before arming anything.
 */
#define HP_SETTLE_MS	700
#define HP_GAP_MS	2000

/* EQ is in the DSP's path: Direct Mode off, SBX on, EQ on. */
static bool fx_eq_live(const struct creative_ae9_fx *fx)
{
	return !fx->direct && fx->sbx && fx->eq_on;
}

/*
 * The EQ pre-gain the DSP gets. With the headphone profile's EQ ("use
 * speaker EQ", 0x1f) and the graphic EQ both on, a hot signal overloads
 * the DSP: it drops every other 64-frame block, the "metallic" sound
 * (Windows captures, notes 52/53: pre-gain 0 dB, 91% of snapshots bad;
 * -12 dB, 0 of 253). Windows sends the setting as is. Here it goes 12 dB
 * lower while both are on, but not below -12 dB (the slider's floor; the
 * DSP's range below it is untested). The control keeps the user's value.
 * Every pre-gain send goes through here.
 */
static s32 fx_pregain_tenths(const struct creative_ae9_fx *fx)
{
	s32 v = fx->eq[0];

	if (eq_headroom && fx->hp_on)
		v = max(v - 120, -120);
	return v;
}

static u32 fx_pregain(const struct creative_ae9_fx *fx)
{
	return fx_f32(fx_pregain_tenths(fx), 10);
}

/*
 * "Use speaker EQ" on or off, with the pre-gain moved first on the way in
 * and after on the way out, so the two EQs are never both on at the full
 * pre-gain. Always sends 0x1f: an output switch's replay turns it off in
 * the DSP behind our back.
 */
static void fx_hp_set_locked(struct creative_chip *chip, bool on, bool log)
{
	struct creative_ae9_fx *fx = &chip->fx;
	bool was = fx->hp_on;
	bool eq = fx_eq_live(fx);	/* not eq_headroom: switching it off must restore */

	if (on) {
		fx->hp_on = true;
		if (eq)
			fx_send(chip, FX_MID_OUT, 0x0a, fx_pregain(fx), log);
		fx_send(chip, FX_MID_OUT, 0x1f, fx_f32(1, 1), log);
	} else {
		fx_send(chip, FX_MID_OUT, 0x1f, 0, log);
		fx->hp_on = false;
		if (eq && was)
			fx_send(chip, FX_MID_OUT, 0x0a, fx_pregain(fx), log);
	}
	if (eq && eq_headroom && was != on) {
		s32 a = fx_pregain_tenths(fx), b = fx->eq[0];

		dev_info(chip->dev, "EQ headroom: pre-gain %s%d.%d dB to the DSP (setting %s%d.%d dB)%s\n",
			 a < 0 ? "-" : "", abs(a) / 10, abs(a) % 10,
			 b < 0 ? "-" : "", abs(b) / 10, abs(b) % 10,
			 on ? " while the headphone profile and the EQ are both on" : "");
	}
}

enum hp_sync { HP_IDLE, HP_WAIT, HP_LOADED, HP_RETRY, HP_BLOCKED };

/*
 * Uploads that lost a reply or landed wrong broke the DSP until a
 * power-off (our crash 2026-11-08, note 54; Kaiba's 0.10.1 report, note
 * 55): most went wrong while audio played. So an upload waits until every
 * stream has stopped (hp_live_upload=1 skips the wait), and one that
 * failed after the DSP's DMA was armed, or after 2 channels went missing,
 * stops uploads until the next bring-up.
 */
static bool fx_hp_must_wait(struct creative_chip *chip)
{
	return !hp_live_upload && creative_ae9_streams_running(chip);
}

static void fx_hp_block(struct creative_chip *chip, const char *why)
{
	chip->fx.hp_blocked = true;
	chip->fx.hp_waiting = false;
	dev_warn(chip->dev, "headphone profile: %s; no more profile uploads until the next start (sleep/wake or reboot; power the PC off if the sound is broken)\n",
		 why);
}

/*
 * The headphone profile's part of a speaker block: upload the image if the
 * DSP doesn't hold it yet (after a bring-up it holds nothing), then "use
 * speaker EQ" on or off. On with Direct Mode off, headphones out and a
 * profile picked. A failed or postponed upload leaves it off. Caller holds
 * replay_mutex.
 */
static enum hp_sync fx_hp_sync_locked(struct creative_chip *chip, bool log)
{
	struct creative_ae9_fx *fx = &chip->fx;
	bool want = !fx->direct && chip->hu_output && fx->hp_profile;
	enum hp_sync st = HP_IDLE;
	char name[64];
	int ret;

	if (!want || fx->hp_loaded == fx->hp_profile)
		fx->hp_waiting = false;
	if (want && fx->hp_loaded != fx->hp_profile) {
		if (fx->hp_blocked || fx->dead) {
			st = HP_BLOCKED;
		} else if (fx_hp_must_wait(chip)) {
			if (!fx->hp_waiting)
				dev_info(chip->dev, "headphone profile: loads when playback and recording stop (uploads during playback can break the DSP)\n");
			fx->hp_waiting = true;
			queue_delayed_work(chip->wq, &chip->hp_work,
					   msecs_to_jiffies(HP_GAP_MS));
			st = HP_WAIT;
		} else {
			fx->hp_waiting = false;
			char key[HP_KEY_MAX + 1];

			hp_profile_id(fx->hp_profile, key, sizeof(key), NULL, 0);
			snprintf(name, sizeof(name), "creative-ae9/hp/%s.bin", key);
			ret = creative_dsp_hpeq_upload(chip, name);
			fx->hp_loaded = ret ? 0 : fx->hp_profile;
			st = ret ? HP_RETRY : HP_LOADED;
			if (ret == -EREMOTEIO) {
				fx_hp_block(chip, "the upload failed after the DSP's DMA was set up");
				st = HP_BLOCKED;
			} else if (ret && chip->hp_leaks >= 2) {
				fx_hp_block(chip, "2 DSP DMA channels went missing");
				st = HP_BLOCKED;
			}
		}
	}
	/* while a new pick waits for a pause, the profile already loaded stays
	 * on (Kaiba: it went off at once, audibly); the upload turns "use
	 * speaker EQ" off itself before it writes, and a failure leaves it off */
	fx_hp_set_locked(chip, want && fx->hp_loaded &&
			 (fx->hp_loaded == fx->hp_profile || st == HP_WAIT), log);
	return st;
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
/*
 * Headphone virtualization ("Virtual 7.1" in Creative's guide): with the
 * output on headphones and Direct Mode off, Windows tells the DSP it is
 * driving headphones, speaker config 0 (MASTERCONTROL 0x80/0x04 = 0), and
 * SBX Surround becomes the headphone virtualizer; there is no other switch
 * (Sound Blaster Command: one headphone config on the AE-9). Its headphone
 * block (capture 4 s11, 00:58:27.54 #16040-16500; CtxHda FUN_00019534):
 * levels and delays of all 8 channels 0, interleaved; config 0; X-Bass at
 * 0x17 80 Hz; bass redirect off; no full-range pairs 0x1a-0x1d. We sent
 * the speaker config (2.0..5.1) on headphones until 2026-11-09.
 */
static bool fx_hp_virt(struct creative_chip *chip)
{
	return !chip->fx.direct && chip->hu_output && chip->fx.hp_virt;
}

static void fx_apply_all_locked(struct creative_chip *chip)
{
	struct creative_ae9_fx *fx = &chip->fx;
	unsigned int i;

	bool on;

	if (fx_hp_virt(chip)) {
		fx_send(chip, FX_MID_OUT, 0x3a, fx_f32(1, 1), false);
		/* Windows sends 8+8; the DSP rejects channels 7/8 (0x27/0x28/
		 * 0x37/0x38, error replies in all 36 Windows sets), so 6+6 */
		for (i = 0; i < 6; i++) {
			fx_send(chip, FX_MID_OUT, 0x21 + i, 0, false);
			fx_send(chip, FX_MID_OUT, 0x31 + i, 0, false);
			usleep_range(10000, 11000);	/* CtxHda FUN_00019534: 10 ms */
		}
		fx_send(chip, FX_MID_MASTER, 0x04, 0, true);
		/* X-Bass crossover gain: always 0.1 in Windows' headphone block
		 * (every headphone + DSP block in all captures) */
		fx_send(chip, FX_MID_OUT, 0x19, fx_f32(1, 10), false);
		fx_send(chip, FX_MID_OUT, 0x17, fx_f32(80, 1), false);
		fx_send(chip, FX_MID_OUT, 0x18, 0, false);
		fx_send(chip, FX_MID_SPKEQ, 0x01, 0, false);
		fx_hp_sync_locked(chip, false);
		fx_send(chip, FX_MID_OUT, 0x15, 0, false);
		goto effects;
	}

	/* Windows' order (capture 3, 19:56:08): mute, speaker config, speaker
	 * block, on/offs, then values only for what is on, EQ on last. */
	fx_send(chip, FX_MID_OUT, 0x3a, fx_f32(1, 1), false);
	/* speaker calibration first, as Windows' speaker block (capture
	 * 2026-11-07, section 12): levels, then delays */
	for (i = 0; i < ARRAY_SIZE(fx->spk_level); i++)
		fx_send(chip, FX_MID_OUT, 0x21 + i, spk_level_val(fx, i), false);
	for (i = 0; i < ARRAY_SIZE(fx->spk_dist); i++)
		fx_send(chip, FX_MID_OUT, 0x31 + i, spk_delay_val(fx, i), false);
	fx_send(chip, FX_MID_MASTER, 0x04,
		fx_f32(fx->direct ? 8 : spk_cfgs[spk_idx(fx)], 1), true);
	fx_send(chip, FX_MID_OUT, 0x19, fx_f32(fx->level[FX_BASS], 100), false);
	fx_send(chip, FX_MID_OUT, 0x17, fx_f32(80, 1), false);
	fx_send(chip, FX_MID_OUT, 0x18, 0, false);
	fx_send(chip, FX_MID_SPKEQ, 0x01, 0, false);
	fx_hp_sync_locked(chip, false);
	fx_send(chip, FX_MID_OUT, 0x15,
		fx_f32(!fx->direct && fx->bass_redirect, 1), false);
	if (!fx->direct && fx->bass_redirect)
		fx_send_redirect(chip, false);
	/* full range on every pair (front, center/LFE, rear, surround), sent
	 * by Windows after each speaker config change in DSP mode (captures 5
	 * and 6); otherwise the DSP redirects the bass to the LFE channel,
	 * which the AE-9's stereo outputs don't have */
	for (i = 0; i < 4; i++)
		fx_send(chip, FX_MID_OUT, 0x1a + i, fx_f32(1, 1), false);
effects:
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
		fx_send(chip, FX_MID_OUT, 0x0a, fx_pregain(fx), false);
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
	dev_info(chip->dev, "Direct Mode %s, SBX %s%s (Surround %d, Dialog+ %d, Smart Volume %d, Crystalizer %d, Bass %d, EQ %d%s)\n",
		 chip->fx.direct ? "on" : "off", chip->fx.sbx ? "on" : "off",
		 fx_hp_virt(chip) ? ", headphone virtualization" : "",
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
		fx_send(chip, FX_MID_OUT, 0x0a, fx_pregain(fx), true);
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

/* The DSP routing was set up again (switch to DSP mode, bring-up): its
 * "use speaker EQ" is off until the profile block sends it. */
static void creative_ae9_fx_hp_reset(struct creative_chip *chip)
{
	chip->fx.hp_on = false;
	chip->fx.hp_waiting = false;
	chip->hp_tries = 0;
}

/*
 * The DSP firmware is about to be (re)loaded (hw_bringup): a fresh DSP,
 * so a blocked upload may be tried again and lost channels are back. Only
 * here: a Direct Mode round trip is router verbs only, the DSP keeps
 * whatever a failed upload left (review B1, note 56a).
 */
void creative_ae9_fx_dsp_reset(struct creative_chip *chip)
{
	chip->fx.hp_on = false;
	chip->fx.hp_loaded = 0;
	chip->fx.hp_blocked = false;
	chip->fx.hp_waiting = false;
	chip->hp_leaks = 0;
	chip->hp_tries = 0;
	chip->scp_fail_logged = false;
}

/* After a bring-up (probe or resume) the DSP is back at its startup state:
 * effects off, speaker config 8. Only SBX on needs sending again. */
void creative_ae9_fx_restore(struct creative_chip *chip)
{
	chip->fx.dead = false;
	chip->fx.fails = 0;
	chip->fx.hp_loaded = 0;		/* the DSP was reset */
	creative_ae9_fx_hp_reset(chip);
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
	chip->fx.hp_loaded = 0;		/* the DSP was reset */
	creative_ae9_fx_hp_reset(chip);
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
	fx->xover = XOVER_DEF;
	fx->hp_virt = true;	/* Windows: always on with headphones, DSP mode */
	for (i = 0; i < FX_COUNT; i++) {
		fx->on[i] = fx_def_on[i];
		fx->level[i] = fx_def_lvl[i];
	}
}

/* ------------------------------------------------------------------ */
/* Controls: private_value = kind << 8 | index                         */
/* ------------------------------------------------------------------ */

enum { K_SBX, K_ON, K_LVL, K_SVMODE, K_EQON, K_EQ, K_PRESET, K_SCOUT, K_DIRECT, K_SPK,
       K_REDIR, K_XOVER, K_SUBGAIN, K_SPKLVL, K_SPKDIST, K_HPPROF };
#define PV(k, i)	((k) << 8 | (i))
#define PV_K(pv)	((pv) >> 8)
#define PV_I(pv)	((pv) & 0xff)

static const DECLARE_TLV_DB_SCALE(eq_tlv, -1200, 100, 0);
static const DECLARE_TLV_DB_SCALE(spk_tlv, SPK_LVL_MIN * 100, 100, 0);

/* Speaker setup: changes go to the DSP with Direct Mode off, effects or not */
static bool fx_live_spk(struct creative_chip *chip)
{
	return !chip->fx.direct && fx_ready(chip) && !chip->fx.dead &&
	       !chip->dsp_switch_pending;
}

void creative_ae9_fx_hp_work(struct work_struct *w)
{
	struct creative_chip *chip = container_of(to_delayed_work(w),
						  struct creative_chip, hp_work);
	struct creative_ae9_fx *fx = &chip->fx;
	unsigned long next = chip->hp_last + msecs_to_jiffies(HP_GAP_MS);
	bool upload;
	enum hp_sync st;

	if (!fx_live_spk(chip))
		return;
	if (chip->hp_last && time_before(jiffies, next)) {
		queue_delayed_work(chip->wq, &chip->hp_work, next - jiffies);
		return;
	}
	upload = fx->hp_profile && fx->hp_loaded != fx->hp_profile &&
		 chip->hu_output && !fx->hp_blocked;
	/* waiting for the streams to stop: look again later, no DSP traffic */
	if (upload && fx->hp_waiting && fx_hp_must_wait(chip)) {
		queue_delayed_work(chip->wq, &chip->hp_work,
				   msecs_to_jiffies(HP_GAP_MS));
		return;
	}
	fx_live_begin(chip);
	if (fx_live_spk(chip)) {
		st = fx_hp_sync_locked(chip, true);
		if (st == HP_LOADED || st == HP_RETRY || st == HP_BLOCKED)
			chip->hp_last = jiffies ?: 1;
		if (st == HP_RETRY && !fx->dead && chip->hp_tries++ < 1)
			queue_delayed_work(chip->wq, &chip->hp_work,
					   msecs_to_jiffies(HP_GAP_MS));
	}
	fx_live_end(chip);
}

static int fx_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	switch (PV_K(kc->private_value)) {
	case K_SBX: case K_ON: case K_EQON: case K_SCOUT: case K_DIRECT:
	case K_REDIR: case K_SUBGAIN:
		return snd_ctl_boolean_mono_info(kc, ui);
	case K_XOVER: case K_SPKLVL: case K_SPKDIST: {
		static const struct { s16 min, max, step; } r[] = {
			[K_XOVER - K_XOVER]	= { XOVER_MIN, XOVER_MAX, XOVER_STEP },
			[K_SUBGAIN - K_XOVER]	= { SPK_LVL_MIN, SPK_LVL_MAX, 1 },
			[K_SPKLVL - K_XOVER]	= { SPK_LVL_MIN, SPK_LVL_MAX, 1 },
			[K_SPKDIST - K_XOVER]	= { 0, SPK_DIST_MAX, 1 },
		};
		unsigned int k = PV_K(kc->private_value) - K_XOVER;

		ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
		ui->count = 1;
		ui->value.integer.min = r[k].min;
		ui->value.integer.max = r[k].max;
		ui->value.integer.step = r[k].step;
		return 0;
	}
	case K_SVMODE:
		return snd_ctl_enum_info(ui, 1, ARRAY_SIZE(sv_mode_names),
					 sv_mode_names);
	case K_PRESET:
		return snd_ctl_enum_info(ui, 1, ARRAY_SIZE(eq_preset_names),
					 eq_preset_names);
	case K_SPK:
		return snd_ctl_enum_info(ui, 1, spk_count(), spk_names);
	case K_HPPROF: {
		static const char *names[ARRAY_SIZE(hp_profiles)];
		static char cname[HP_CUSTOM][HP_NAME_MAX + 1];
		static DEFINE_MUTEX(info_lock);	/* names[] is shared */
		unsigned int i;
		int ret;

		mutex_lock(&info_lock);
		for (i = 0; i < ARRAY_SIZE(hp_profiles); i++)
			names[i] = hp_profiles[i].name;
		for (i = 0; i < HP_CUSTOM; i++) {
			hp_profile_id(HP_CUSTOM0 + i, NULL, 0, cname[i], sizeof(cname[i]));
			names[HP_CUSTOM0 + i] = cname[i];
		}
		ret = snd_ctl_enum_info(ui, 1, ARRAY_SIZE(hp_profiles), names);
		mutex_unlock(&info_lock);
		return ret;
	}
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
	case K_REDIR:	*v = fx->bass_redirect; break;
	case K_XOVER:	*v = fx->xover; break;
	case K_SUBGAIN:	*v = fx->sub_gain; break;
	case K_SPKLVL:	*v = fx->spk_level[i]; break;
	case K_SPKDIST:	*v = fx->spk_dist[i]; break;
	case K_HPPROF:	uv->value.enumerated.item[0] = fx->hp_profile; break;
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
			      k == K_SBX || k >= K_SPK ? !fx->direct : fx_eff(fx);

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
		} else if (!fx->direct && chip->pcm_start_rate != 48000 &&
			   !(ae9_dsp_96k && chip->pcm_start_rate == 96000)) {
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
		} else if (!chip->dsp_active) {
			/*
			 * DSP mode never started (the switch was still pending:
			 * the stream plays direct, e.g. at 44.1 kHz). Nothing to
			 * undo. Replaying the "DSP off" block anyway disturbed
			 * the running direct stream and nothing restarted it:
			 * silent until the player reopened (2026-11-07).
			 */
			chip->dsp_switch_pending = false;
			dev_info(chip->dev, "Direct Mode on (DSP mode hadn't started; the stream stays as it is)\n");
		} else {
			bool was_dsp = chip->dsp_active;
			struct snd_pcm_substream *ss = READ_ONCE(chip->playback_substream);

			chip->dsp_switch_pending = false;
			/* effects off (speaker config 8), then route direct */
			fx_apply_all(chip);
			creative_ae9_dsp_mode(chip, false);
			/*
			 * The running stream is now unbound and silent. Windows
			 * stops it too and leaves the restart to the app
			 * (capture 2026-11-05, 02:07:01). Report it stopped so
			 * PipeWire restarts it, and the normal start block routes
			 * it direct. Does nothing if no stream is running.
			 * The start block normally goes out once per open, so
			 * mark it unsent or the restart plays unrouted (first
			 * test, 2026-11-06: silent until the stream closed).
			 * The DAC start commands go out again too: DSP mode
			 * changed the I2S clock under them.
			 */
			if (was_dsp && ss) {
				dev_info(chip->dev, "Direct Mode on: restarting the running stream\n");
				chip->pcm_start_sent = 0;
				chip->dac_start_key = 0;
				snd_pcm_stop_xrun(ss);
			}
		}
		/* Line Out Virtualization is DSP mode only: re-route after
		 * the mode switch, from the heartbeat */
		if (chip->lo_virt && chip->hu_output)
			creative_ae9_hu_reroute_request(chip);
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
		if (fx_live(chip) && fx->eq_on) {
			/* the pre-gain under the lock: an upload or output
			 * switch may be turning the profile on (review B4) */
			fx_live_begin(chip);
			fx_send(chip, FX_MID_OUT, 0x0a + i,
				i ? fx_f32(v, 1) : fx_pregain(fx), true);
			fx_live_end(chip);
		}
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
	case K_REDIR:
		if (!!v == fx->bass_redirect)
			return 0;
		fx->bass_redirect = !!v;
		if (fx_live_spk(chip)) {
			fx_live_begin(chip);
			fx_send(chip, FX_MID_OUT, 0x15, fx_f32(fx->bass_redirect, 1), true);
			if (fx->bass_redirect)
				fx_send_redirect(chip, true);
			fx_live_end(chip);
		}
		return 1;
	case K_XOVER:
		v = clamp_t(long, rounddown(v, XOVER_STEP), XOVER_MIN, XOVER_MAX);
		if (v == fx->xover)
			return 0;
		fx->xover = v;
		if (fx_live_spk(chip) && fx->bass_redirect) {
			fx_live_begin(chip);
			__fx_send(chip, FX_MID_OUT, 0x16, fx_f32(v, 1), true, true);
			fx_live_end(chip);
		}
		return 1;
	case K_SUBGAIN:	/* Creative: SubwooferBoost, on/off (0x96/0x1e = 0 / 1.0) */
		v = !!v;
		if (v == fx->sub_gain)
			return 0;
		fx->sub_gain = v;
		if (fx_live_spk(chip) && fx->bass_redirect) {
			fx_live_begin(chip);
			__fx_send(chip, FX_MID_OUT, 0x1e, fx_f32(v, 1), true, true);
			fx_live_end(chip);
		}
		return 1;
	case K_SPKLVL:
		v = clamp_t(long, v, SPK_LVL_MIN, SPK_LVL_MAX);
		if (v == fx->spk_level[i])
			return 0;
		fx->spk_level[i] = v;
		if (fx_live_spk(chip))
			fx_live_one(chip, 0x21 + i, spk_level_val(fx, i));
		return 1;
	case K_SPKDIST:
		v = clamp_t(long, v, 0, SPK_DIST_MAX);
		if (v == fx->spk_dist[i])
			return 0;
		fx->spk_dist[i] = v;
		if (fx_live_spk(chip))
			fx_live_one(chip, 0x31 + i, spk_delay_val(fx, i));
		return 1;
	case K_HPPROF:
		if (e >= ARRAY_SIZE(hp_profiles) || e == fx->hp_profile)
			return 0;
		fx->hp_profile = e;
		{
			char n[HP_NAME_MAX + 1];

			hp_profile_id(e, NULL, 0, n, sizeof(n));
			dev_info(chip->dev, "Headphone Profile: %s\n", n);
		}
		/* upload once the picker has been still for a moment */
		chip->hp_tries = 0;
		if (fx_live_spk(chip))
			mod_delayed_work(chip->wq, &chip->hp_work,
					 msecs_to_jiffies(HP_SETTLE_MS));
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
void creative_ae9_fx_spk_follow_work(struct work_struct *w)
{
	struct creative_chip *chip = container_of(to_delayed_work(w),
						  struct creative_chip, spk_follow_work);
	struct snd_pcm_substream *ss = chip->playback_substream;

	/* only a stream that's still there and running */
	if (!ss || !ss->runtime || ss->runtime->state != SNDRV_PCM_STATE_RUNNING ||
	    ss->runtime->channels != chip->spk_follow_ch)
		return;
	creative_ae9_fx_spk_follow(chip, chip->spk_follow_ch);
}

void creative_ae9_fx_spk_follow(struct creative_chip *chip, unsigned int channels)
{
	struct creative_ae9_fx *fx = &chip->fx;
	unsigned int want = fx->spk, i;

	if (!ae9_surround || fx_hp_virt(chip))
		return;	/* headphone mode: the DSP downmixes 5.1 itself */
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
 * After a speakers/headphones switch with Direct Mode off. Windows' switch
 * sequence (replayed from its op lists) also sets the DSP: speaker config
 * 8, X-Bass off, bass redirect off, "use speaker EQ" off. Windows' app
 * sends its own profile again within seconds; we sent only the headphone
 * profile back, and the DSP stayed in the switch state: it then output
 * every other 64-frame block, music at half speed with a 375 Hz buzz
 * (2026-11-08 09:44 and 10:12, notes 56b/56c; cleared by the next SBX
 * toggle). So: the whole profile again, inside its mute (fx_apply_all).
 */
void creative_ae9_fx_output_changed(struct creative_chip *chip)
{
	if (!fx_live_spk(chip))
		return;
	fx_apply_all(chip);
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

	if (kind == K_EQ || kind == K_SPKLVL) {
		kn.access |= SNDRV_CTL_ELEM_ACCESS_TLV_READ;
		kn.tlv.p = kind == K_EQ ? eq_tlv : spk_tlv;
	}
	kc = snd_ctl_new1(&kn, chip);
	err = snd_ctl_add(chip->card, kc);	/* frees kc on error */
	if (!err && chip->fx.nkc < ARRAY_SIZE(chip->fx.kc))
		chip->fx.kc[chip->fx.nkc++] = kc;
	if (!err && kind == K_HPPROF) {	/* for the custom names' change event */
		mutex_lock(&hp_custom_lock);
		hp_custom_chip = chip;
		hp_custom_kc = kc;
		mutex_unlock(&hp_custom_lock);
	}
	return err;
}

/*
 * Names avoid PipeWire's mixer-path element names ("Surround", "Bass",
 * "Treble" ...), so the desktop leaves them alone.
 */
static int hp_virt_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);

	uv->value.integer.value[0] = chip->fx.hp_virt;
	return 0;
}

static int hp_virt_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	bool on = !!uv->value.integer.value[0];

	if (on == chip->fx.hp_virt)
		return 0;
	chip->fx.hp_virt = on;
	dev_info(chip->dev, "Headphone Virtualization %s\n", on ? "on" : "off");
	if (!chip->fx.direct && chip->hu_output && !chip->dsp_switch_pending)
		fx_apply_all(chip);
	return 1;
}

static const struct snd_kcontrol_new hp_virt_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Headphone Virtualization Switch",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info = snd_ctl_boolean_mono_info,
	.get = hp_virt_get,
	.put = hp_virt_put,
};

int creative_ae9_fx_controls_add(struct creative_chip *chip)
{
	snd_ctl_add(chip->card, snd_ctl_new1(&hp_virt_control, chip));
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
	/* speaker setup; names stay clear of PipeWire's path elements */
	if (!err)
		err = fx_add(chip, "Bass Redirect Switch", K_REDIR, 0);
	if (!err)
		err = fx_add(chip, "Bass Redirect Crossover", K_XOVER, 0);
	if (!err)
		err = fx_add(chip, "Bass Redirect Sub Boost Switch", K_SUBGAIN, 0);
	for (i = 0; !err && i < ARRAY_SIZE(spk_ch_names); i++) {
		snprintf(name, sizeof(name), "Speaker Level %s Volume", spk_ch_names[i]);
		err = fx_add(chip, name, K_SPKLVL, i);
		if (err)
			break;
		snprintf(name, sizeof(name), "Speaker Distance %s", spk_ch_names[i]);
		err = fx_add(chip, name, K_SPKDIST, i);
	}
	if (!err)
		err = fx_add(chip, "Headphone Profile", K_HPPROF, 0);
	fx_update_active(chip);
	return err;
}
