/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 3xHelix */
/*
 * Sound Blaster AE-7 (experimental): the names its sequence
 * files share (creative_ae7_seq.c, _run.c, _pcm.c, _core.c, each #included
 * at the end of the AE-9 file whose senders it uses). Values from the
 * AE-7's Windows captures (C2B1 = capture 2's cold boot, C1B3 = capture 1's
 * third bring-up: its speaker select; docs/dev/AE7.md) and the ESS
 * ES9018K2M's register map (the AE-7's DAC, 48/40 chip_id 4; the AE-5's
 * ES9016K2M has the same map). Its register 7 is not the AE-9 ES9038Q2M's:
 * never send it AE-9 filter values.
 */
#ifndef __CREATIVE_AE7_H
#define __CREATIVE_AE7_H

/* BAR2: the card's ID (upper 24 bits 0x008100 on the AE-7; low byte = board revision) */
#define AE7_BAR2_ID		0x004
#define AE7_BAR2_ID_MASK	0xffffff00
#define AE7_BAR2_ID_WANT	((u32)CREATIVE_SUBSYS_AE7 << 16)	/* ae9_board_ae7.bar2_id */
#define AE7_BAR2_REV		0x008	/* bits 4:2 pick the DAC's THD row (C2: 0x8, row 2) */
#define AE7_BAR2_CTL_1C		0x00880680	/* BAR2 0x1c: the AE-9's 0x880480 with bit 9 (SDI2 off) set */
#define AE7_CODEC_SSID		0x11020081	/* codec 1's subsystem ID, no bus reset needed */
#define AE7_CODEC_MASK		0x2	/* STATESTS: codec 1 only */

/* the I2C devices behind the CA0113 front end (BAR2 0x204-0x210) */
#define AE7_DAC			0x48	/* ES9018K2M: 48/40 reads chip_id 4 (the ES9016K2M is 6) */
#define AE7_SW			0x30	/* the switch chip: writes are 0x00 or 0x3f */
#define AE7_SW_ON		0x3f
/* the DAC's registers (K2M map) */
#define K2M_INPUT		0x01	/* 0x80 = 32-bit I2S, auto select off */
#define K2M_AUTOMUTE		0x04	/* 0 = automute off */
#define K2M_DEEMPH		0x06	/* 0x48 = de-emphasis bypassed */
#define K2M_FILTER_MUTE		0x07	/* bit 7 must be 1, bits 6:5 the filter, 1:0 the mute */
#define K2M_MUTE		0x03
#define K2M_CLOCK		0x0a	/* 0x05 = its default (slave) */
#define K2M_SPDIF_SRC		0x0b	/* 0x12 = S/PDIF source DATA2, normal L/R */
#define K2M_DPLL		0x0c	/* 0x5f: DPLL bandwidth, I2S 5 / DSD 0xf */
#define K2M_THD_BYPASS		0x0d	/* bit 6 = compensation off; 0x00 = on */
#define K2M_VOLUME_L		0x0f	/* 0.5 dB steps, 0xff = mute */
#define K2M_VOLUME_R		0x10
#define K2M_MASTER_TRIM		0x11	/* 0x11-0x14, low byte first: the headphone gain */
#define K2M_THD_C2		0x16	/* 0x16/0x17 */
#define K2M_THD_C3		0x18	/* 0x18/0x19: 0x19 = the row byte */
#define K2M_CHIP_ID		0x40	/* read only: bits 4:2 = 4 ES9018K2M, 6 ES9016K2M */
#define K2M_LOCKED		0x01	/* 0x40 bit 0: lock_status, the DAC locked to its input */
/* K2M register 1 (input) and 10 (clock) per path (phase 2: Direct Mode) */
#define K2M_IN_I2S		0x80	/* 32-bit I2S, auto select off (the DSP path, Direct 48 kHz family) */
#define K2M_IN_SPDIF		0x81	/* S/PDIF input (Direct 44.1/88.2 kHz), with 0x0b = 0x12 */
#define K2M_SPDIF_DATA2		0x12	/* register 0x0b: S/PDIF on DATA2, normal L/R */
#define K2M_CLK_SLAVE		0x05	/* register 10's default */
#define K2M_CLK_SYNC		0x15	/* with sync_mode (bit 4, MCLK = 128 fs): 384 kHz */
#define K2M_SYNC_MODE		0x10	/* register 10 bit 4: the DPLL bypassed (lock_status no measure) */
/* the switch chip's registers (Windows' item table) */
#define SW_INPUT		0x28	/* 0x00 mic, 0x3f line in */
#define SW_DSD			0x2b	/* 0x00 except in DSD (P4) */
#define SW_FRONT		0x2e	/* item 0xc Front_MutePin: 0x3f on headphones */
#define SW_HP			0x30	/* item 0xb HP_MutePin (inverted): 0x3f on headphones */
#define SW_MUX_OUT		0x32	/* item 4: follows pin 0x10 (0x00 headphones, 0x3f speakers) */

/* GPIO (BAR2 0x320 = pin | level << 8) */
#define AE7_GPIO_AMP		0	/* headphone amp shutdown, inverted: high = amp on */
#define AE7_GPIO_LED		1	/* the on-card LED (Windows' HwInternalLed, default on) */

/* DSP stream 0x0c: the DSP's processed audio (its count: XRAM 0x07ac) */
#define AE7_STREAM_DSP_PROC	0x0c

/*
 * Stream 0x0c's router slots (its list: head XRAM 0x1578 + 0x0c, next
 * 0x1478 + slot, 0xff ends it; each slot a word at 0x190000 + 4 * slot,
 * 128 of them): the 8051 builds the list from its own queue some time
 * after the stream's start, the remap (Windows' AltDACMapping walk) then
 * sets the CA0132 DAC port of each of its 12 lanes
 */
#define AE7_XRAM_PROC_HEAD	(0x1578 + AE7_STREAM_DSP_PROC)	/* 0x1584 */
#define AE7_XRAM_SLOT_NEXT	0x1478
#define AE7_RT_SLOTS		128
#define AE7_REMAP_SLOTS		12

/* the port map (ChipIO param 13): 0x58 on the AE-7 in every mode */
#define AE7_DAC2PORT		0x58

/*
 * Phase 2: Direct Mode (level 4, ae7=1). Windows' AE-7 board
 * stages, captured on the AE-7 at all six rates (capture B at 48 kHz,
 * capture Bw at 44.1/48/88.2/96/192/384 kHz: docs/dev/AE7.md): stream 0x05
 * (host audio) straight to the DAC's I2S connection point 0xd0 (48 kHz
 * family) or to the S/PDIF transmitter 0x91 (44.1 kHz family, the DAC on
 * its S/PDIF input), stream 0x14 on, the clocks for the rate, stream 0x05's
 * router slots pointed at the I2S ports 0x40.. Stereo only. 176.4 and
 * 352.8 kHz are refused (Windows' format gate: capture Bw's players were
 * refused before the card saw any access): creative_ae7_rate_row() returns
 * NULL for them.
 */
#define AE7_VERB_ASI_CLASS	0x725	/* the ASI rate class: 0x81, 0x80 at 384 kHz and on S/PDIF */
#define AE7_XRAM_HOST_HEAD	(0x1578 + AE9_STREAM_HOST)	/* 0x157d: stream 0x05's first slot */
#define AE7_PLL_DAC_PIN		0x43	/* PLL/PMU: the DAC's data pin (I2S 0xc7, S/PDIF out 0x06) */
#define AE7_PIN_I2S		0xc7
#define AE7_PIN_SPDIF		0x06
#define AE7_DSP_RATE		96000	/* the DSP path's I2S rate (Windows' SetDirect(0)) */
#define AE7_I2S_CLK		0x189000	/* the I2S clock descriptors (0x189000, 0x189004) */
#define AE7_I2S_RATE		0x189024	/* the rate descriptor */
#define AE7_I2S_28		0x189028	/* 0x2000f at every rate */

struct ae7_rate {
	unsigned int rate;
	bool spdif;		/* 44.1 kHz family: through the S/PDIF transmitter */
	u8 cp_code;		/* stream 0x05's connection point rate (192 kHz for 384) */
	u8 asi_class;		/* ChipIO 0x725 */
	u8 dac0a;		/* K2M register 10 */
	u8 slots;		/* stream 0x05's router slots: (rate / 48 kHz) x 2 (I2S rates) */
	u32 clk, i2s_rate;	/* 0x189000/04 and 0x189024 (I2S rates) */
	u32 src[4];		/* S/PDIF transmitter: 0x18a020 (k << 6 | 2), 0x18a024, 0x18a028, 0x18a020 (| 3) */
};

/*
 * The AE-7's level (ae9_ae7_mode, set at each bring-up by
 * creative_ae7_level()): AE7_MODE_DIRECT whenever ae7= runs the card, the
 * only one release builds have; a test build's ae7_test=1-3 picks one of
 * the test kits' narrower ones. Comments say "level N".
 */
#define AE7_MODE_IDENTIFY	1	/* test: identify the card and stop */
#define AE7_MODE_STEREO		2	/* test: stereo, DSP mode only (kit 6's default) */
#define AE7_MODE_6CH		3	/* test: 6 channels, DSP mode only (kit 5's) */
#define AE7_MODE_DIRECT		4	/* ae7=1: stereo or 6 channels in DSP mode, stereo in
					 * Direct Mode at 44.1/48/88.2/96/192/384 kHz */
#define AE7_OPT_MAX		5	/* ae7=2-5 (the test kits' values) run as ae7=1 */

/* DSP floats */
#define AE7_F_ONE		0x3f800000
#define AE7_F_TWO		0x40000000
#define AE7_F_THREE		0x40400000

/* volume range: 0 to -90 dB in 0.5 dB steps (Windows' board range) */
#define AE7_ATT_MAX		180
#define AE7_ATT_DEFAULT		0x48	/* -36 dB at load: the safe default */

/*
 * The DAC's lock in Direct Mode (creative_ae7_unmute()): a wait, not a
 * fault, for up to AE7_LOCK_WAIT_MS after a start confirmed its path (kit 8
 * on the tester's AE-7: 0.11-0.4 s at 44.1/88.2 kHz, the S/PDIF input; the
 * I2S rates locked by the first read), the lock bit polled alone every
 * AE7_LOCK_POLL_MS (one DAC read, ~26 ms, each). The gate's other refusals
 * retry every AE7_GATE_RETRY_MS, up to 10 in a row.
 */
#define AE7_LOCK_WAIT_MS	1500
#define AE7_LOCK_POLL_MS	30
#define AE7_GATE_RETRY_MS	200

/*
 * A control change reaches the heartbeat this long after it (its kick;
 * Windows' volume thread runs 20 ms after a change). A "Master Playback
 * Volume" write becomes the level of the output picked once it has stood
 * this long (creative_ae7_want_att()).
 */
#define AE7_KICK_MS		20

/* a jack event (or a RIRB response interrupt) to plug_work's scan and sense reads */
#define AE7_PLUG_MS		50

/* recording (kit 10): input stream descriptor 0 (pcm.c's CREATIVE_CAP_STREAM_BASE) */
#define AE7_CAP_SD		0x80

/* "Headphone Gain": the DAC's master trim (48/11-14), Low/Normal/High */
#define AE7_GAINS		3
#define AE7_GAIN_DEFAULT	0	/* Low (-8 dB), as Windows at load */
/* "DAC Filter": K2M register 7 bits 6:5 with bit 7 set */
#define AE7_FILTERS		3

/* creative_ae7_seq.c (creative_ae9_seq.c's senders) */
extern const u8 ae7_lane_win[AE7_REMAP_SLOTS];
void creative_ae7_predownload(struct creative_chip *chip);
void creative_ae7_tail(struct creative_chip *chip, struct ae9_tail *t);
int creative_ae7_remap(struct creative_chip *chip);
unsigned int creative_ae7_direct_walk(struct creative_chip *chip, unsigned int nslots);
unsigned int creative_ae7_ports_off(struct creative_chip *chip, u8 src_lo, u8 src_hi);
/* creative_ae7_run.c (creative_ae9_seq_run.c's senders) */
const struct ae7_rate *creative_ae7_rate_row(unsigned int rate, int mode);
int creative_ae7_mute_checked(struct creative_chip *chip);
int creative_ae7_conv_mute(struct creative_chip *chip);
int creative_ae7_dsp_out_off(struct creative_chip *chip, int *bad);
int creative_ae7_direct_route(struct creative_chip *chip, const struct ae7_rate *row);
int creative_ae7_rate_clock(struct creative_chip *chip, const struct ae7_rate *row);
int creative_ae7_direct_on(struct creative_chip *chip, const struct ae7_rate *row);
void creative_ae7_direct_stop(struct creative_chip *chip);
int creative_ae7_dsp_path(struct creative_chip *chip);
int creative_ae7_path_check(struct creative_chip *chip, const struct ae7_rate *row);
void creative_ae7_bar2_init(struct creative_chip *chip);
int creative_ae7_select(struct creative_chip *chip, bool hp, bool runtime);
int creative_ae7_preset_held(struct creative_chip *chip, bool hp);
u8 creative_ae7_want_att(struct creative_chip *chip, int out, unsigned int *att, bool *stale);
int creative_ae7_volume(struct creative_chip *chip, int out);
int creative_ae7_gain(struct creative_chip *chip);
int creative_ae7_filter(struct creative_chip *chip);
int creative_ae7_unmute(struct creative_chip *chip);
int creative_ae7_amp_on(struct creative_chip *chip);
void creative_ae7_led(struct creative_chip *chip, bool on);
void creative_ae7_power_down(struct creative_chip *chip, bool cold);
void creative_ae7_stream_unmute(struct creative_chip *chip);
void creative_ae7_hold_off(struct creative_chip *chip);
void creative_ae7_bind(struct creative_chip *chip, u16 format);
void creative_ae7_unbind(struct creative_chip *chip);
extern const u32 ae7_gain_trim[AE7_GAINS];
extern const u8 ae7_filter_bits[AE7_FILTERS];
/* creative_ae7_pcm.c */
int creative_ae7_pcm_init(struct creative_chip *chip);
/* creative_ae7_fx.c (creative_ae9_fx.c's senders) */
void creative_ae7_fx_out_block(struct creative_chip *chip);
void creative_ae7_fx_out_done(struct creative_chip *chip);
void creative_ae7_fx_lock(struct creative_chip *chip, bool lock);
void creative_ae7_fx_apply(struct creative_chip *chip);
void creative_ae7_fx_direct(struct creative_chip *chip, bool direct);
void creative_ae7_probe_done(struct creative_chip *chip);
/* creative_ae7_core.c */
void creative_ae7_stream_start(struct creative_chip *chip, u16 format);
int creative_ae7_play_start(struct creative_chip *chip, unsigned int rate, u16 format);
void creative_ae7_play_stop(struct creative_chip *chip);

/*
 * One DAC or switch-chip command, a list of its own (dsp.c: its own
 * ca0113_mutex section, NACK detection and one retry outside the
 * expected window). 0 = acknowledged.
 */
static inline int ae7_dac(struct creative_chip *chip, u8 grp, u8 reg, u8 val)
{
	const u8 cmd[1][3] = { { grp, reg, val } };

	return creative_dsp_ca0113_list_quiet(chip, cmd, 1);
}

/* the 48/07 read-modify-write Windows does (its read checked; unread: our last write) */
static inline int ae7_dac07(struct creative_chip *chip, u8 clear, u8 set)
{
	u32 raw[2];
	int v = creative_dsp_ca0113_read_checked(chip, AE7_DAC, K2M_FILTER_MUTE, raw);

	if (v < 0)
		v = chip->dac48_07 ? chip->dac48_07 : chip->dac_filter_bits;
	return ae7_dac(chip, AE7_DAC, K2M_FILTER_MUTE, (v & ~clear) | set);
}

/*
 * A DSP float from an attenuation in 0.5 dB steps: -att / 2 dB, exact
 * (Windows sends DSP 0x32 the level in dB; -10.5 dB = 0xc1280000)
 */
static inline u32 ae7_att_f32(unsigned int att)
{
	unsigned int e;

	if (!att)
		return 0;
	e = fls(att) - 1;
	return 0x80000000 | (e + 126) << 23 | ((att << (23 - e)) & 0x7fffff);
}

#endif /* __CREATIVE_AE7_H */
