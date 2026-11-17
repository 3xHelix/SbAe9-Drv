/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2026 3xHelix */
/*
 * Creative Sound Blaster AE-9: the names creative_ae9_seq.c and
 * creative_ae9_seq_run.c share (codecs, nodes, vendor verbs, DSP streams and
 * connection points, DSP floats). Private to those two files.
 */
#ifndef __CREATIVE_AE9_SEQ_H
#define __CREATIVE_AE9_SEQ_H

#define AE9_CAD			1	/* codec 1, the CA0132 */
#define AE9_CAD2		2	/* codec 2: optical in/out, the head unit's rear RCA in */
#define AE9_NID_AFG		0x01	/* its audio function group */
#define AE9_PWRST_D3COLD	0x04	/* HD-Audio power state D3cold */

/*
 * ChipIO control parameter 0: "dm" in ca0132-tools (a source select: 0
 * none, 1 mic 1, 2 mic 2, 3 connection point 0x82, per the 8051's bank 2
 * handler 0xccd4); not in mainline's enum
 */
#define AE9_CTL_PARAM_DM	0
#define AE9_SR_48000		0x09	/* connection-point rate code (mainline SR_48_000) */
/*
 * ChipIO 4-bit verb 6 (notes 44: payload = value << 6 | index): the 8051's
 * PLL/PMU settings by index
 */
#define AE9_VERB_CHIPIO_PMU_SET	0x600
/*
 * Creative's vendor verbs on a node's 16-bit word: read it, write its low
 * byte, its high byte (the 8051 applies it with the node's power state);
 * mainline's ca0132_gpio_init() writes the AFG's as constants
 */
#define AE9_VERB_GET_NODE_WORD	0xf93
#define AE9_VERB_SET_NODE_WORD_LO 0x793
#define AE9_VERB_SET_NODE_WORD_HI 0x794
/* another AFG vendor verb (mainline's ca0132_gpio_init(): meaning not given) */
#define AE9_VERB_GET_790	0xf90
#define AE9_VERB_SET_790	0x790
/*
 * Creative's extra-precision volume verbs (mainline ca0132_alt_vol_setup()):
 * the fraction of a dB, in 1/64, that the next amp write adds
 */
#define AE9_VERB_GET_FINE_VOL_L	0xf97
#define AE9_VERB_GET_FINE_VOL_R	0xf98
#define AE9_VERB_SET_FINE_VOL_L	0x797
#define AE9_VERB_SET_FINE_VOL_R	0x798

#define AE9_NID_ADC		0x07	/* the converter recordings come from */
#define AE9_NID_DAC		0x02	/* the converter playback goes through */
#define AE9_NID_MIC_XLR		0x12	/* the head unit's XLR/TRS input pin */
#define AE9_AMP_0DB		90	/* a converter's output amp: gain step = dB + 90 */
#define AE9_F_MINUS_36_19	0xc210c28f	/* -36.19 */

/* DSP streams (the 8051's stream ids, ca0132's numbering) */
#define AE9_STREAM_HOST		0x05	/* host audio (converter 0x02) */
#define AE9_STREAM_DIRECT	0x14	/* the direct path to the front DACs */
#define AE9_STREAM_DSP_OUT	0x18	/* the DSP's output to the DACs (DSP mode) */

/* connection points */
#define AE9_CP_HOST		0x43	/* host audio */
#define AE9_CP_DSP_OUT		0x09	/* the DSP's output */
#define AE9_CP_SRC		0x91	/* the card's sample-rate converter */
#define AE9_CP_FRONT_DAC	0xd0	/* the front DAC output */
#define AE9_SR_96000		0x0b	/* connection-point rate code (mainline SR_96_000) */

/*
 * ChipIO control parameter 13: "dac2port" in ca0132-tools (not in mainline's
 * enum): the 8051 writes it to chip 0x18b010, a port map of 4 x 2-bit
 * fields; 0xa4 on speakers, 0xa1 on headphones (bits 1:0 and 3:2 swap).
 * No measured effect on either output (uv35/uv36/uv44).
 */
#define AE9_CTL_PARAM_DAC2PORT	13

/*
 * The 8051's stream table (ca0132-tools: XRAM 0x072f + 10 * stream id, 10
 * bytes a stream), byte 5: "Active". Windows reads it before it enables a
 * stream (CtxHda FUN_000341e4: when it reads 0) or disables one
 * (FUN_00034250: when it doesn't): stream 0x14's is XRAM 0x07fc, 0x18's
 * 0x0824.
 */
#define AE9_XRAM_STREAM_ACTIVE(sid)	(0x072f + 10 * (sid) + 5)

/* more DSP floats */
#define AE9_F_HALF		0x3f000000	/* 0.5 */
#define AE9_F_ONE		0x3f800000	/* 1.0 */
#define AE9_F_EIGHT		0x41000000	/* 8.0 */
#define AE9_F_EIGHTY		0x42a00000	/* 80.0 */

/*
 * ChipIO verb 0x724: a PMU bit request (bit 7 on/off, bits 3:0 a code that
 * picks a bit of PMU register 0 or 1, the 8051's table 0xe7f3; 0x83 =
 * register 0 bit 2 on: bank 1 0xbbb8); not in mainline
 */
#define AE9_VERB_CHIPIO_PMU_BIT	0x724

#endif /* __CREATIVE_AE9_SEQ_H */
