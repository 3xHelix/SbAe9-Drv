/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 3xHelix */
/*
 * Creative Sound Blaster AE-9 driver: the board layer (since 0.13.0 in
 * every build). chip->board is the card's board: the AE-9, or the AE-7
 * (experimental, only with the option ae7=1; creative_ae7_core.c).
 *
 * The AE-7's sequences are in creative_ae7_*.c, #included at the end of
 * the AE-9 file whose senders they use (seq.c, seq_run.c, pcm.c, fx.c,
 * core.c), so no AE-9 function becomes non-static. The AE-9 board has no
 * ops and every capability: on an AE-9 every hook falls through to the
 * AE-9 code (docs/dev/AE7.md has the per-function compare with 0.12.17).
 */
#ifndef __CREATIVE_AE9_BOARD_H
#define __CREATIVE_AE9_BOARD_H

/* What a board has (ae9_has()): each gates AE-9 code an AE-7 must not reach */
#define AE9_CAP_HEAD_UNIT	BIT(0)	/* the head unit on BAR2's UART, its controls and polls */
#define AE9_CAP_CODEC2		BIT(1)	/* codec 2 (optical in/out, the head unit's rear RCA in) */
#define AE9_CAP_AE9_DIAG	BIT(2)	/* the AE-9's own start-up reads (0x800000, 0x801c) */
#define AE9_CAP_INPUTS		BIT(3)	/* recording: mic, What U Hear, their controls */
#define AE9_CAP_SBX		BIT(4)	/* SBX and its controls (CrystalVoice: AE9_CAP_CV) */
#define AE9_CAP_ES9038		BIT(5)	/* the ES9038Q2M's filters (DAC Filter, 7 items) */
/* stubs for the AE-7's later phases (docs/dev/AE7.md): nothing checks them yet */
#define AE9_CAP_DIRECT		BIT(6)	/* Direct Mode and its rates */
#define AE9_CAP_DSD		BIT(7)	/* native DSD64 */
#define AE9_CAP_OPTICAL		BIT(8)	/* optical out */
#define AE9_CAP_SURROUND	BIT(9)	/* 5.1: the Rear and C/Sub outputs */
/* kit 10: AE9_CAP_SBX alone no longer brings CrystalVoice */
#define AE9_CAP_CV		BIT(10)	/* CrystalVoice (Voice Clarity, Voice Morphing, Mic EQ), its controls */
#define AE9_CAP_SPK_SETUP	BIT(11)	/* Bass Redirect and the speakers' levels / distances */
#define AE9_CAP_AE9_MIC		BIT(12)	/* the AE-9's mic amp: Mic Gain's +30 dB step (mic_boost_30) */
/* kit 11: CrystalVoice on the AE-7 without the AE-9's DSP mic monitor (0x80/0x0d, uncaptured there) */
#define AE9_CAP_MIC_MON		BIT(13)	/* the mic monitor (DSP 0x80/0x0d), its controls */
/* 0.13.0: Headphone Virtualization off on the headphones (the AE-9's speaker
 * block there); without it the switch stays on (the AE-7: Windows' headphone
 * block only, fx.c hp_virt_put()) */
#define AE9_CAP_HP_VIRT_OFF	BIT(14)
#define AE9_CAP_ALL		(AE9_CAP_HEAD_UNIT | AE9_CAP_CODEC2 | AE9_CAP_AE9_DIAG | \
				 AE9_CAP_INPUTS | AE9_CAP_SBX | AE9_CAP_ES9038 | \
				 AE9_CAP_DIRECT | AE9_CAP_DSD | AE9_CAP_OPTICAL | AE9_CAP_SURROUND | \
				 AE9_CAP_CV | AE9_CAP_SPK_SETUP | AE9_CAP_AE9_MIC | AE9_CAP_MIC_MON | \
				 AE9_CAP_HP_VIRT_OFF)
/*
 * The AE-7 board's (ae9_board_ae7; tools/tablesc's glue the same): SBX and
 * recording / What U Hear (kit 10), CrystalVoice (kit 11); not the AE-9's
 * mic monitor (AE9_CAP_MIC_MON) or the rest
 */
#define AE7_BOARD_CAPS		(AE9_CAP_SBX | AE9_CAP_INPUTS | AE9_CAP_CV)

struct creative_chip;
struct seq_file;
struct snd_kcontrol;

#define CREATIVE_SUBSYS_AE7	0x0081
#define AE7_PLUGS		4	/* the AE-7's jacks that sense a plug (struct ae7_state) */

/*
 * A board's ops: NULL (the table or a member) = the AE-9 code. Each hook
 * site names the AE-9 function it stands in for.
 */
struct ae9_board_ops {
	/* hw_bringup(), first: the board's ID checks; < 0 stops the bring-up */
	int (*bringup_check)(struct creative_chip *chip, bool *bus_reset);
	/* hw_bringup(), after the codec detect: < 0 stops the bring-up */
	int (*codecs_check)(struct creative_chip *chip);
	void (*bar2_init)(struct creative_chip *chip);		/* creative_ae9_bar2_init_windows() */
	void (*predownload)(struct creative_chip *chip);	/* creative_ae9_predownload() */
	void (*post_fw)(struct creative_chip *chip);		/* creative_ae9_post_fw() */
	void (*power_down)(struct creative_chip *chip, bool cold); /* creative_ae9_power_down() */
	void (*streams_down)(struct creative_chip *chip);	/* creative_ae9_streams_down() */
	void (*quiesce)(struct creative_chip *chip);		/* suspend: before the rings go */
	void (*tick)(struct creative_chip *chip);		/* creative_ae9_hb_work() */
	void (*controls_add)(struct creative_chip *chip);	/* creative_ae9_hu_controls_add() */
	int (*pcm_init)(struct creative_chip *chip);		/* creative_ae9_pcm_init() */
	void (*resumed)(struct creative_chip *chip);		/* resume_work(), after the bring-up */
	void (*state_show)(struct seq_file *m, struct creative_chip *chip); /* debugfs state */
	/*
	 * Kit 10's output side (fx.c's SBX on a board; core.c's probe):
	 * fx_out_block: fx_apply_all_locked() after its opening mute, in place
	 * of the AE-9's speaker / headphone block (replay_mutex held), then the
	 * effects; fx_out_done: its end, after the closing unmute
	 * (fx_unmute_verify()); fx_lock: fx_apply_all() and fx_apply_spk()
	 * around their replay_mutex, kit 11 also creative_ae9_cv_apply()
	 * (true: take, false: release; a board lock ordered before
	 * replay_mutex); probe_done: creative_ae9_probe() before
	 * snd_card_register(), after fx_init() and the controls.
	 */
	void (*fx_out_block)(struct creative_chip *chip);
	void (*fx_out_done)(struct creative_chip *chip);
	void (*fx_lock)(struct creative_chip *chip, bool lock);
	void (*probe_done)(struct creative_chip *chip);
	/* kit 10, the input side */
	/* rirb_unsol() (corb.c), corb_mutex held: one unsolicited RIRB entry */
	void (*unsol)(struct creative_chip *chip, u32 res, u32 fl);
	/* the IRQ handler (hard IRQ): a RIRB response interrupt, after its status is cleared */
	void (*rirb_irq)(struct creative_chip *chip);
	/* ae9_cancel_timed_works(): the board's own works, after the AE-9's */
	void (*cancel_works)(struct creative_chip *chip);
};

struct ae9_board {
	u16 subsys;
	const char *name;		/* card shortname; ALSA makes the id from its last word */
	u32 caps;			/* AE9_CAP_* */
	u16 bar2_id;			/* BAR2+0x4's high half (core.c ae9_bar2_alive(),
					 * the AE-7's bringup_check): 0x7172 AE-9, 0x0081 AE-7 */
	const struct ae9_board_ops *ops;
};

extern const struct ae9_board ae9_board_ae9, ae9_board_ae7;
/*
 * creative_ae7_core.c: the option ae7= (0 leave an AE-7 alone, the
 * default; 1 run it; 2-5, the test kits' values, run it as 1) and the
 * AE-7's level it sets at the bring-up (AE7_MODE_*, creative_ae7.h)
 */
extern int ae9_ae7_opt;
extern int ae9_ae7_mode;

/*
 * The AE-7's own state (creative_ae7_*.c). The volume and output live in
 * the AE-9's fields (hu_att, hu_mute, hu_output, hu_att_out[]); since kit 11
 * hu_att_out[] is each output's level and hu_att "Master Playback Volume"
 * (creative_ae7_want_att(), creative_ae7_core.c's ae7_vol_put()).
 */
struct ae7_state {
	u32 bar2_rev;		/* BAR2+0x8 (BAR2+0x4 is chip->bar2_id[2]) */
	u8 thd_row;		/* (BAR2+0x8 >> 2) & 7: Windows' DAC revision row */
	u8 gain;		/* "Headphone Gain": 0 Low, 1 Normal, 2 High */
	u8 filter;		/* "DAC Filter": 0 Fast, 1 Slow, 2 Minimum Phase */
	u8 out;			/* "Head Unit Output": 0 speakers, 1 headphones */
	bool inited;		/* defaults set (first bring-up) */
	bool levels_set;	/* the converters' first level since the bring-up was sent */
	u8 conv_amp, conv_fine;	/* the Rear/C-Sub converters' level (0x03/0x04, the last
				 * volume's; unmuted form) */
	bool conv_mute;		/* that level is muted (master switch, -90 dB) */
	bool conv_on;		/* the converters unmuted now (the gate opened them, speakers) */
	bool unmuted;		/* the DAC unmuted after the gate (48/07 bits 1:0 clear) */
	bool amp_hold;		/* the headphone amp held off: the DAC's mute not confirmed
				 * (creative_ae7_amp_on()) */
	u8 ch;			/* playback channels: 2 (levels 2, 4) or 6 (level 3); level 4 also 6 in DSP mode */
	u8 dsp_ch;		/* the last DSP-mode stream's count since the DSP path was built
				 * (0: none; level 4 rebuilds it at a change: core.c) */
	u8 lane_port[12];	/* stream 0x0c's lanes c0..cb -> CA0132 DAC ports (the remap's;
				 * Windows' ae7_lane_win, or ae7_lanes=) */
	unsigned int lanes_gen;	/* the ae7_lanes= write lane_port holds */
	int pins_state;		/* pins 0x0b/0x0f/0x10 as Windows tracks them: 0 speaker set,
				 * 1 headphone set, -1 unknown (written at the next select) */
	int out_sent;		/* output last selected: 0 speakers, 1 headphones, -1 none */
	int gain_sent;		/* gain whose trim the DAC holds (headphones), -1 none */
	int filter_sent;	/* filter last written, -1 none */
	int att_sent;		/* attenuation last acknowledged, 0xff mute, -1 none */
	bool att_new;		/* "Master Playback Volume" written (hu_att), not yet taken into
				 * hu_att_out[] (chip->lock; creative_ae7_want_att()) */
	unsigned long att_at;	/* jiffies: that write */
	unsigned int gate_fails;	/* unmute gate refusals in a row */
	unsigned int vol_fails;		/* volume lists refused in a row */
	u8 remapped;		/* stream 0x0c's slots remapped at the last walk (of
				 * AE7_REMAP_SLOTS; the gate wants all of them) */
	u8 remap_head;		/* that walk's first slot (0xff: the list wasn't there) */
	u16 remap_ms;		/* how long the walk waited for the list after the start */
	unsigned int remap_walks;	/* walks since load (the start-ups' and the heartbeat's) */
	unsigned int selects, gate_blocks;	/* since load (debugfs state) */
	/*
	 * Phase 2: the playback path (level 4 switches it; DSP mode otherwise).
	 * The DAC is muted (checked) before every path or rate switch; the
	 * gate (creative_ae7_unmute()) wants path_ok, and in Direct Mode the
	 * DAC locked to its input. path_mutex: the heartbeat (its gate, its
	 * filter and output writes to 48/07) and a stream's start on its path
	 * one at a time, so no unmute or 48/07 write lands inside a switch.
	 */
	struct mutex path_mutex;
	bool direct_want;	/* "Direct Mode Switch" */
	bool direct;		/* the path in effect: stream 0x05 straight to the DAC (Direct) */
	bool start_direct;	/* the path the started stream asked for (the heartbeat's restart) */
	bool path_ok;		/* the last path set-up acknowledged and read back */
	bool bound;		/* converter 0x02 bound to the playback stream */
	bool s14;		/* stream 0x14 started (Windows' stage 3; its stage 0 stops it) */
	bool stay_logged;	/* "stays in Direct Mode" said for this stream */
	u8 asi_class;		/* ChipIO 0x725 as last written (0x81 from the start-up) */
	u8 walked, walk_want;	/* stream 0x05's slots set at the last Direct start, of walk_want */
	u32 path_rate;		/* the rate the card's clocks hold: 96000 (DSP path) or Direct's */
	bool spdif_reclock;	/* the S/PDIF transmitter didn't enable: the next 44.1 kHz-family
				 * set-up re-clocks the I2S to 48 kHz first (the AE-9's fix) */
	int dac01, dac0a, dac0b, dac40;	/* the last read-backs (-1: not read) */
	int pin43, i2s_clk, i2s_rate, tx_ctl;	/* the CA0132's: PLL/PMU 0x43, 0x189000,
						 * 0x189024, 0x18a020 (-1: not read) */
	unsigned int path_switches, path_fails;	/* since load */
	/* the DAC's lock in Direct Mode (creative_ae7_unmute()) */
	unsigned long lock_from;	/* jiffies: the last Direct start's path confirmed */
	bool lock_wait;		/* this start's lock bit read 0: polled alone until it is 1 */
	unsigned int lock_ms;	/* the last wait for the lock, from lock_from (0: none) */
	unsigned int lock_waits, lock_late;	/* since load: waits, waits past AE7_LOCK_WAIT_MS */
	struct snd_kcontrol *gain_ctl, *out_ctl, *filter_ctl, *direct_ctl;
	/* kit 10: SBX and the DSP's output block (creative_ae7_fx.c) */
	bool fx_armed;		/* the probe's fx state is in (probe_done): brackets may run */
	bool fx_pending;	/* the heartbeat sends a bracket (a bring-up, an output switch) */
	bool cal_pending;	/* the next speaker block sends the speakers' levels and delays */
	bool block_sent;	/* the output block sent since the last bring-up or output switch */
	bool sub_1a1d;		/* the block's config has a sub: 1a-1d after the bracket's unmute */
	const char *block_kind;	/* the last block's (debugfs) */
	unsigned int blocks;	/* output blocks since load */
	/*
	 * Kit 10: the card's jacks that sense a plug (creative_ae7_core.c,
	 * ae7_jacks[]): their pins read after a jack's unsolicited response,
	 * reported by read-only controls. plug_work scans the RIRB (after a
	 * response interrupt, the only news an idle card gives) and reads the
	 * pending pins; plug_armed from the start-up's end to the next cancel
	 * (cancel_works: suspend, shutdown, unload).
	 */
	struct delayed_work plug_work;
	atomic_t plug_pending;	/* jacks (bit i = ae7_jacks[i]) whose pin to read */
	bool plug_armed;
	u8 plugs;		/* plugged at the last read (bit per jack) */
	u8 plugs_read;		/* read at least once since load (bit per jack) */
	unsigned int plug_events, plug_reads;	/* since load: jack events, sense reads */
	u32 plug_last;		/* the last jack event's response word */
	struct snd_kcontrol *plug_ctl[AE7_PLUGS];
};

/* a chip without a board (tools/tablesc's AE-9 tests) is an AE-9 */
#define ae9_has_op(chip, op)	((chip)->board && (chip)->board->ops && (chip)->board->ops->op)
#define AE9_HOOK(chip, op, ...) do {						\
		if (unlikely(ae9_has_op(chip, op))) {				\
			(chip)->board->ops->op(__VA_ARGS__);			\
			return;							\
		}								\
	} while (0)
#define AE9_CALL(chip, op, ...) do {						\
		if (unlikely(ae9_has_op(chip, op)))				\
			(chip)->board->ops->op(__VA_ARGS__);			\
	} while (0)
#define ae9_has(chip, cap)	(!(chip)->board || ((chip)->board->caps & (cap)) != 0)

#endif /* __CREATIVE_AE9_BOARD_H */
