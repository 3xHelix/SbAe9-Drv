// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 3xHelix
//
// Sound Blaster AE-7 (experimental): SBX on the AE-7 and the
// DSP's output block per output, Speaker Config and path, on
// creative_ae9_fx.c's effects (fx_apply_all_locked() and its senders).
// #included at the end of creative_ae9_fx.c.
//
// What Windows sends on the AE-7 (the W2 trace, docs/dev/AE7.md): the same
// SBX / EQ / headphone-profile messages as on the AE-9, and inside every
// mute bracket (96/3a = 1.0 ... 0) the output select's preset again
// (creative_ae7_preset_held(): port map, 30/2e, 30/30, THD, trim, 96/29-2a,
// the pins on a change, 0x18b03c) and an output block for the path:
// - Direct Mode (W2 seq 18627-18773): 80/04 = 8.0, X-Bass, 17 = 80 Hz,
//   8f/01 = 0, 1f, 15 = 0;
// - headphones with Headphone Virtualization (W2 seq 23971-24473): the
//   AE-9's headphone block (levels and delays 0, 6+6: the DSP refuses
//   channels 7/8 on the AE-7 too, its error reply at seq 24294), 80/04 = 0;
// - speakers (W2 seq 25935-26431, and the Speaker Config changes): after an
//   output select the levels 21-26 = 0, delays 31-32 = 0 and 33-36 =
//   1.0e-4 s (the INF's CalibDelay, 100 us on Center/Sub/Rear), then 80/04
//   = the config's code, X-Bass and 17 = 80 Hz without a sub, 8f/01 = 0,
//   1f, 15 = 1.0 with a sub; after the bracket's unmute 1a-1d = 1.0 with a
//   sub.
// 96/18-19 carry the X-Bass state (on, level) in every block, as Windows'
// (W2: 19 = 0.5 before SBC's dump, 0.1 after it, 18 = 1.0 once X-Bass was
// on; design decision 2), not per-config constants.
//
// Locks: path_mutex -> replay_mutex -> ca0113_mutex. Every path that writes
// the preset holds ae7.path_mutex: a control's fx_apply_all() /
// fx_apply_spk() through the fx_lock op, the heartbeat and a stream's start
// on its path (they hold it already: creative_ae7_fx_apply()). So no preset
// of one output can land after the other output's select. The preset is
// the output selected (ae7.out_sent), never the one asked for: a switch
// still pending belongs to the heartbeat.
//

#include "creative_ae7.h"

/*
 * Speaker Config on the AE-7 (the AE-9's control and items, fx->spk):
 * MASTERCONTROL 0x80/0x04 as Windows sent it for each (W2 seq 21615,
 * 22173, 22713, 23253, 23793) and whether the config has a sub (bass
 * redirect 96/15 = 1.0, then 1a-1d). 2.0 is 8.0 on the AE-7 ("Stereo";
 * the AE-9's table has 1.0), the settled value of every Windows bracket.
 */
static const struct ae7_spk {
	u8 cfg;
	bool sub;
} ae7_spk[] = {
	{ 8, false },	/* 2.0 */
	{ 2, true },	/* 2.1 */
	{ 5, false },	/* 4.0 */
	{ 6, true },	/* 4.1 */
	{ 8, true },	/* 5.1 */
};

#define AE7_F_100US	0x38d1b717	/* 1.0e-4 s: the INF's CalibDelay on Center/Sub/Rear */
#define AE7_SPK_DEFAULT	0		/* Speaker Config 2.0 on the AE-7 (design decision 7) */

/* the X-Bass state every block carries: 18 = on (in the DSP's path only), 19 = its level */
static void ae7_fx_xbass(struct creative_chip *chip, bool lvl)
{
	struct creative_ae9_fx *fx = &chip->fx;

	if (lvl)
		fx_send(chip, FX_MID_OUT, 0x19, fx_f32(fx->level[FX_BASS], 100), false);
	fx_send(chip, FX_MID_OUT, 0x17, fx_f32(80, 1), false);
}

/*
 * The output block (the fx_out_block op: fx_apply_all_locked() after its
 * opening mute, replay_mutex held; ae7.path_mutex held by the caller): the
 * select's preset for the output selected, then the block for the path,
 * the output and the Speaker Config; the effects follow (fx.c). The block
 * is marked sent (the gate's condition in DSP mode). A preset that went
 * wrong leaves the DAC muted and the output unselected
 * (creative_ae7_preset_held()): the heartbeat selects again.
 */
void creative_ae7_fx_out_block(struct creative_chip *chip)
{
	struct creative_ae9_fx *fx = &chip->fx;
	const struct ae7_spk *spk = &ae7_spk[spk_idx(fx)];
	unsigned int i;

	BUILD_BUG_ON(ARRAY_SIZE(ae7_spk) != ARRAY_SIZE(spk_cfgs));
	lockdep_assert_held(&chip->ae7.path_mutex);
	lockdep_assert_held(&chip->replay_mutex);
	chip->ae7.sub_1a1d = false;
	if (chip->ae7.out_sent >= 0)
		creative_ae7_preset_held(chip, chip->ae7.out_sent == 1);
	if (fx->direct) {
		chip->ae7.block_kind = "Direct Mode";
		fx_send(chip, FX_MID_MASTER, 0x04, fx_f32(8, 1), true);
		ae7_fx_xbass(chip, true);
	} else if (fx_hp_virt(chip)) {
		chip->ae7.block_kind = "headphones, DSP mode";
		for (i = 0; i < 6; i++) {
			fx_send(chip, FX_MID_OUT, 0x21 + i, 0, false);
			fx_send(chip, FX_MID_OUT, 0x31 + i, 0, false);
			usleep_range(10000, 11000);
		}
		fx_send(chip, FX_MID_MASTER, 0x04, 0, true);
		ae7_fx_xbass(chip, true);
	} else {
		chip->ae7.block_kind = chip->hu_output ? "headphones without virtualization, DSP mode" :
				       "speakers, DSP mode";
		if (chip->ae7.cal_pending) {
			for (i = 0; i < 6; i++)
				fx_send(chip, FX_MID_OUT, 0x21 + i, 0, false);
			for (i = 0; i < 6; i++)
				fx_send(chip, FX_MID_OUT, 0x31 + i, i < 2 ? 0 : AE7_F_100US, false);
			chip->ae7.cal_pending = false;
		}
		fx_send(chip, FX_MID_MASTER, 0x04, fx_f32(spk->cfg, 1), true);
		if (!spk->sub)
			ae7_fx_xbass(chip, true);
	}
	fx_send(chip, FX_MID_OUT, 0x18, fx_f32(fx_eff(fx) && fx->on[FX_BASS], 1), false);
	fx_send(chip, FX_MID_SPKEQ, 0x01, 0, false);
	fx_hp_sync_locked(chip, false);
	fx_send(chip, FX_MID_OUT, 0x15,
		fx_f32(!fx->direct && !fx_hp_virt(chip) && spk->sub, 1), false);
	chip->ae7.sub_1a1d = !fx->direct && !fx_hp_virt(chip) && spk->sub;
	chip->ae7.block_sent = true;
	chip->ae7.blocks++;
}

/*
 * The bracket's end (the fx_out_done op, after fx_unmute_verify()): with a
 * sub, the full-range pairs 96/1a-1d = 1.0 (W2: after the unmute, seq
 * 26395 after 26352; sent after every sub block, a superset of Windows')
 */
void creative_ae7_fx_out_done(struct creative_chip *chip)
{
	unsigned int i;

	if (!chip->ae7.sub_1a1d)
		return;
	chip->ae7.sub_1a1d = false;
	for (i = 0; i < 4; i++)
		fx_send(chip, FX_MID_OUT, 0x1a + i, fx_f32(1, 1), false);
}

/*
 * The fx_lock op: a control's fx_apply_all() / fx_apply_spk() take
 * ae7.path_mutex before replay_mutex. A preset that went wrong in it
 * (the output not selected now) wakes the heartbeat to select again.
 */
void creative_ae7_fx_lock(struct creative_chip *chip, bool lock)
{
	if (lock) {
		mutex_lock(&chip->ae7.path_mutex);
		return;
	}
	if (chip->ae7.out_sent < 0 && !READ_ONCE(chip->unloading))
		mod_delayed_work(chip->wq, &chip->hb_work, msecs_to_jiffies(20));
	mutex_unlock(&chip->ae7.path_mutex);
}

/*
 * One bracket (fx_apply_all() without the fx_lock op: the caller holds
 * ae7.path_mutex): the preset, the output block, the effects, the unmute
 * and its check, 1a-1d. From the heartbeat (a bring-up, an output switch)
 * and a stream's start on its path (Direct Mode on and off).
 */
void creative_ae7_fx_apply(struct creative_chip *chip)
{
	lockdep_assert_held(&chip->ae7.path_mutex);
	chip->ae7.fx_pending = false;
	if (!fx_ready(chip))
		return;
	mutex_lock(&chip->replay_mutex);
	fx_apply_all_locked(chip);
	creative_dsp_chipio_addr_invalidate();
	mutex_unlock(&chip->replay_mutex);
	dev_info(chip->dev, "AE-7: output block (%s), Speaker Config %s, SBX %s%s\n",
		 chip->ae7.block_kind, spk_names[spk_idx(&chip->fx)], chip->fx.sbx ? "on" : "off",
		 chip->ae7.out_sent < 0 ? "; the output not selected (the heartbeat selects again)" : "");
}

/*
 * fx.direct mirrors the AE-7's path (ae7.direct, under path_mutex): the
 * SBX controls are inactive in Direct Mode, as on the AE-9
 */
void creative_ae7_fx_direct(struct creative_chip *chip, bool direct)
{
	lockdep_assert_held(&chip->ae7.path_mutex);
	if (chip->fx.direct == direct)
		return;
	WRITE_ONCE(chip->fx.direct, direct);
	fx_update_active(chip);
}

/*
 * The probe_done op (creative_ae9_probe(), before snd_card_register(),
 * after creative_ae9_fx_init()'s memset and the controls): the fx state
 * for the AE-7 (its path: DSP mode at load; Speaker Config 2.0; the DSP
 * as the start-up left it, as creative_ae9_fx_restore() without the AE-9's
 * routing), then the first output block from the heartbeat (fx_pending).
 */
void creative_ae7_probe_done(struct creative_chip *chip)
{
	struct creative_ae9_fx *fx = &chip->fx;

	mutex_lock(&chip->ae7.path_mutex);
	fx->direct = chip->ae7.direct;
	fx->spk = AE7_SPK_DEFAULT;
	fx->dead = chip->dsp_halted;
	fx->fails = 0;
	fx->hp_loaded = 0;
	fx->hp_on = false;
	chip->hp_tries = 0;
	fx_update_active(chip);
	chip->ae7.fx_armed = true;
	chip->ae7.fx_pending = true;
	chip->ae7.cal_pending = true;
	mutex_unlock(&chip->ae7.path_mutex);
	if (!READ_ONCE(chip->unloading))
		mod_delayed_work(chip->wq, &chip->hb_work, msecs_to_jiffies(20));
}
