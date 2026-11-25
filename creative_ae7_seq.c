// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 3xHelix
//
// Sound Blaster AE-7 (experimental): Windows' start-up for the AE-7, verb
// by verb, on creative_ae9_seq.c's senders. #included at the end of
// creative_ae9_seq.c (never compiled on its own), so the AE-9 steps it
// shares are called as they are and stay static.
//
// The reference is the AE-7's Windows capture 2, first bring-up ("C2B1", a
// cold codec start; tools/ae7golden/golden-c2b1.txt has it as events, with
// the same step names as the comments here). Where a step is the AE-9's,
// the AE-9 function is called; where only a value differs, the AE-7 copy
// says which. Windows' reads whose answers it doesn't use and that the
// AE-9 tail leaves out too (ChipIO 0xf0f, 0xf10) are not sent here either.
//

#include "creative_ae7.h"

/* ------------------------------------------------------------------ */
/* Before the download						  */
/* ------------------------------------------------------------------ */

/*
 * The bus driver finding codec 1 (C2B1 codec_enum): pre_codecs_found()'s
 * codec-1 half; the AE-7 has no codec 2
 */
static void ae7_codecs_found(struct ae9_cold *c)
{
	cold_param(c, AC_NODE_ROOT, AC_PAR_VENDOR_ID);
	cold_param(c, AC_NODE_ROOT, AC_PAR_REV_ID);
	cold_param(c, AC_NODE_ROOT, AC_PAR_NODE_COUNT);
	cold_param(c, AE9_NID_AFG, AC_PAR_SUBSYSTEM_ID);
	cold_param(c, AE9_NID_AFG, AC_PAR_FUNCTION_TYPE);
	cold_send(c, AE9_NID_AFG, AC_VERB_GET_SUBSYSTEM_ID, 0);
}

/*
 * Codec 1's driver reading its widgets (C2B1 codec_enum, from the second
 * 0xf00 read): pre_probe_both()'s codec-1 half, without the AE-9's PLL/PMU
 * 0x44 = 0xc8 at its end (the AE-7 writes 0x44 = 0xc2 in its board power-up)
 */
static void ae7_probe(struct ae9_cold *c)
{
	u8 nid;

	cold_param(c, AC_NODE_ROOT, AC_PAR_VENDOR_ID);
	cold_send(c, AE9_NID_AFG, AC_VERB_GET_SUBSYSTEM_ID, 0);
	cold_param(c, AC_NODE_ROOT, AC_PAR_REV_ID);
	cold_param(c, AE9_NID_AFG, AC_PAR_NODE_COUNT);
	for (nid = 0x02; nid <= 0x14; nid++)
		cold_param(c, nid, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, WIDGET_CHIP_CTRL, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, WIDGET_CHIP_CTRL, AC_PAR_VENDOR_ID);
	cold_send(c, WIDGET_CHIP_CTRL, AC_VERB_GET_POWER_STATE, 0);
	cold_param(c, WIDGET_DSP_CTRL, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, WIDGET_DSP_CTRL, AC_PAR_VENDOR_ID);
	cold_send(c, WIDGET_DSP_CTRL, AC_VERB_GET_POWER_STATE, 0);
	cold_param(c, 0x17, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x18, AC_PAR_AUDIO_WIDGET_CAP);
	cold_8051_sfr_read(c, 0xfc);
	cold_8051_sfr_read(c, 0xfd);
	cold_8051_sfr_read(c, 0xfe);
	cold_8051_sfr_read(c, 0xff);
}

/*
 * An 8051 special function register read-modify-write: Windows reads it
 * and writes it back with bits cleared and set (unread: the captured value)
 */
static void ae7_sfr_rmw(struct ae9_cold *c, u8 sfr, u8 clear, u8 set, u8 captured)
{
	u32 v = 0;

	if (cold_verb(c, creative_ae9_verb(AE9_CAD, WIDGET_CHIP_CTRL,
					   VENDOR_CHIPIO_8051_READ_DIRECT, sfr), &v) < 0)
		v = captured;
	cold_8051_sfr_write(c, sfr, ((v & 0xff) & ~clear) | set);
}

/* BAR2's I2C master (Synopsys DW_apb_i2c): IC_CON, TAR, ENABLE, ENABLE_STATUS */
#define AE7_I2C_CON		0x800
#define AE7_I2C_TAR		0x804
#define AE7_I2C_ENABLE		0x86c
#define AE7_I2C_ENABLE_STATUS	0x89c
#define AE7_CA0113_CMD		0x20c
#define AE7_BAR2_PIN_FN		0x100	/* bits 3:0: pins 4-7 alternate function (1) or GPIO (0) */
#define AE7_BAR2_GPIO_OE	0x304

/* IC_ENABLE written, then IC_ENABLE_STATUS bit 0 polled until it follows (Windows: 12-16 ms) */
static void ae7_i2c_enable(struct creative_chip *chip, bool on)
{
	unsigned int t;

	creative_ext_writel(chip, AE7_I2C_ENABLE, on);
	for (t = 0; t < 25; t++) {
		u32 s = creative_ext_readl(chip, AE7_I2C_ENABLE_STATUS);

		if (s == ~0U || (s & 1) == on)
			break;
		usleep_range(1000, 1500);
	}
}

/*
 * The DACs' I2C master set up again (Windows' I2C object, its speed and
 * master configuration, then the CA0113's command register bit 0): off,
 * IC_CON 0x6b, on, 0x6b again, its target written back as it is (0x57
 * from the BAR2 init the first time, 0x30 the last switch-chip command's
 * later), 0x20c bit 0 set (0x800001 the first time, 0x800005 after DAC
 * commands)
 */
static void ae7_i2c_master(struct creative_chip *chip)
{
	ae7_i2c_enable(chip, false);
	creative_ext_writel(chip, AE7_I2C_CON, 0x6b);
	ae7_i2c_enable(chip, true);
	creative_ext_writel(chip, AE7_I2C_CON, 0x6b);
	creative_ext_writel(chip, AE7_I2C_TAR, creative_ext_readl(chip, AE7_I2C_TAR) & 0xff);
	creative_ext_writel(chip, AE7_CA0113_CMD, creative_ext_readl(chip, AE7_CA0113_CMD) | 1);
}

/*
 * Windows' AE-7 board power-up (C2B1 board_power; its board object's init
 * and the AE-7 hardware init), in its order:
 * - first: PLL/PMU 0x41 = 0xc8 (the board init);
 * - SFR 0x93 bit 4 set; PLL/PMU 0x44 = 0xc2;
 * - the GPIO pins one by one: pins 4-7 to GPIO (BAR2 0x100 bit pin - 4
 *   cleared), each enabled (0x304 bit pin set), byte read-modify-writes;
 * - the DACs' I2C master again (ae7_i2c_master());
 * - first (once per power-up: Windows re-arms it at D3): SFR 0x90 bit 4
 *   cleared, ~16 ms (Windows' 1 ms timer tick), set;
 * - first: the DAC muted (48/07 read, bits 1:0 set): this one is
 *   acknowledged (the DAC is clocked before the download); the second time
 *   it would fall in the DAC's pre-clock window, where Windows' copy is
 *   never acknowledged, so it isn't sent (docs/dev/AE7.md);
 * - the switch chip's 0x2e = 0x3f (item 0xc);
 * - first: BAR2 0x1c written again (0x880680).
 * The second run is Windows' start (after the download), before the ASI.
 */
static void ae7_board_power(struct ae9_cold *c, bool first)
{
	struct creative_chip *chip = c->chip;
	unsigned int pin;

	if (first)
		cold_pll_pmu_write(c, 0x41, 0xc8);
	ae7_sfr_rmw(c, 0x93, 0, 0x10, 0x10);
	cold_pll_pmu_write(c, 0x44, 0xc2);
	for (pin = 0; pin < 8; pin++) {
		if (pin >= 4)
			creative_ext_writeb(chip, AE7_BAR2_PIN_FN,
					    readb(chip->ext_regs + AE7_BAR2_PIN_FN) & ~BIT(pin - 4));
		creative_ext_writeb(chip, AE7_BAR2_GPIO_OE,
				    readb(chip->ext_regs + AE7_BAR2_GPIO_OE) | BIT(pin));
	}
	ae7_i2c_master(chip);
	if (first) {
		ae7_sfr_rmw(c, 0x90, 0x10, 0, 0x10);
		msleep(16);
		ae7_sfr_rmw(c, 0x90, 0, 0x10, 0x00);
		if (ae7_dac07(chip, 0, K2M_MUTE))
			dev_warn(chip->dev, "AE-7: the DAC did not acknowledge its mute before the download (48/07, I2C NACK)\n");
	}
	ae7_dac(chip, AE7_SW, SW_FRONT, AE7_SW_ON);
	if (first)
		creative_ext_writel(chip, CREATIVE_REG_1C, AE7_BAR2_CTL_1C);
}

/*
 * Codec 1's widgets read again and its pins' configuration defaults (C2B1
 * codec1_pins): pre_codec1_pins() with the AE-7's three different bytes
 * (Windows' AE-7 INF and the in-tree driver's ae7_pincfgs): pin 0x0c
 * 0x014510f0 (AE-9 0x414510f0), pin 0x11 0x018170ff (AE-9 0x41a170ff)
 */
static void ae7_codec1_pins(struct ae9_cold *c)
{
	u8 nid;

	cold_param(c, AE9_NID_AFG, AC_PAR_NODE_COUNT);
	for (nid = 0x02; nid <= 0x0b; nid++)
		cold_param(c, nid, AC_PAR_AUDIO_WIDGET_CAP);
	cold_pin_cfg(c, 0x0b, 0x01017010);	/* jack: line out, 1/8" (Front) */
	cold_param(c, 0x0c, AC_PAR_AUDIO_WIDGET_CAP);
	cold_pin_cfg(c, 0x0c, 0x014510f0);	/* jack: S/PDIF out, optical */
	cold_param(c, 0x0d, AC_PAR_AUDIO_WIDGET_CAP);
	cold_pin_cfg(c, 0x0d, 0x414510f0);	/* no connection: S/PDIF out, optical */
	cold_param(c, 0x0e, AC_PAR_AUDIO_WIDGET_CAP);
	cold_pin_cfg(c, 0x0e, 0x01c520f0);	/* jack: S/PDIF in, optical */
	cold_param(c, 0x0f, AC_PAR_AUDIO_WIDGET_CAP);
	cold_pin_cfg(c, 0x0f, 0x01017114);	/* jack: line out, 1/8" */
	cold_param(c, 0x10, AC_PAR_AUDIO_WIDGET_CAP);
	cold_pin_cfg(c, 0x10, 0x01017011);	/* jack: line out, 1/8" */
	cold_param(c, 0x11, AC_PAR_AUDIO_WIDGET_CAP);
	cold_pin_cfg(c, 0x11, 0x018170ff);	/* jack: line in, 1/8" */
	cold_param(c, 0x12, AC_PAR_AUDIO_WIDGET_CAP);
	cold_pin_cfg(c, 0x12, 0x01a170f0);	/* jack: mic in, 1/8" */
	cold_param(c, 0x13, AC_PAR_AUDIO_WIDGET_CAP);
	cold_pin_cfg(c, 0x13, 0x908700f0);	/* fixed: line in, other analog */
	cold_param(c, 0x14, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, WIDGET_CHIP_CTRL, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, WIDGET_DSP_CTRL, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x17, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x18, AC_PAR_AUDIO_WIDGET_CAP);
	cold_send(c, 0x18, AC_VERB_GET_CONFIG_DEFAULT, 0);
	cold_pin_cfg(c, 0x18, 0x500000f0);	/* no connection: line out */
}

/* a ChipIO register's value, for a read-modify-write (unread: what Windows read in C2B1) */
static u32 ae7_chipio_value(struct ae9_cold *c, u32 addr, u32 captured)
{
	unsigned int v = captured;

	cold_count(c, creative_dsp_chipio_read(c->chip, addr, &v));
	creative_dsp_chipio_addr_invalidate();
	return v;
}

/*
 * Codec 1's ChipIO set-up after DSP_INIT (C2B1 codec1_params):
 * pre_codec1_params() with the PMR1 power register 0x18b030 read and
 * written back as read (a cold card reads 0x20; the AE-9 writes Windows'
 * 0x27, which came from its warm state)
 */
static void ae7_codec1_params(struct ae9_cold *c)
{
	cold_ctl_param(c, CONTROL_PARAM_SPDIF1_SOURCE, 0);
	cold_ctl_param(c, AE9_CTL_PARAM_DM, 0);
	cold_ctl_param(c, CONTROL_PARAM_VIP_SOURCE, 0);
	cold_ctl_param(c, CONTROL_PARAM_PORTA_160OHM_GAIN, 6);
	cold_ctl_param(c, CONTROL_PARAM_PORTD_160OHM_GAIN, 6);
	cold_ctl_flag(c, CONTROL_FLAG_PORT_A_10KOHM_LOAD, true);
	cold_ctl_flag(c, CONTROL_FLAG_PORT_D_10KOHM_LOAD, true);
	cold_chipio_write(c, 0x18b030, ae7_chipio_value(c, 0x18b030, 0x20));
	cold_send(c, WIDGET_CHIP_CTRL, AE9_VERB_CHIPIO_PMU_SET, 0x3ff << 6 | 4);
	cold_send(c, AE9_NID_AFG, AE9_VERB_GET_NODE_WORD, 0);
	cold_send(c, AE9_NID_AFG, AE9_VERB_SET_NODE_WORD_LO, 0);
	cold_send(c, AE9_NID_AFG, AE9_VERB_SET_NODE_WORD_HI, 0x53);
	cold_ctl_flag(c, CONTROL_FLAG_SPDIF2OUT, false);
	cold_conn_rate(c, 0x0a, AE9_SR_48000);
	cold_conn_rate(c, 0x0b, AE9_SR_48000);
}

/*
 * Codec 1's widgets read a third time (C2B1 codec1_widgets, part 1): the
 * node counts, every node's capabilities, then node by node its connection
 * list and power state. Unlike the AE-9, the AE-7 keeps pin 0x11 (its line
 * in) and ADC 0x08, pin 0x0c and converter 0x05 (optical out) and
 * converter 0x04 (the rear and C/Sub jacks) powered; only pin 0x0d,
 * converter 0x06, pin 0x18 and node 0x17 go to D3. The reads' answers
 * aren't used. Sent as captured: read repeats included.
 */
static void ae7_codec1_widgets(struct ae9_cold *c)
{
	cold_param(c, AC_NODE_ROOT, AC_PAR_NODE_COUNT);
	cold_param(c, AE9_NID_AFG, AC_PAR_NODE_COUNT);
	cold_param(c, 0x02, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x03, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x04, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x05, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x06, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x07, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x08, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x09, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x0a, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x0b, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x0c, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x0d, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x0e, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x0f, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x10, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x11, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x12, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x13, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x14, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, WIDGET_CHIP_CTRL, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, WIDGET_DSP_CTRL, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x17, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x18, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x02, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x03, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x04, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x05, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x06, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x07, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x07, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x07, AC_PAR_CONNLIST_LEN);
	cold_send(c, 0x07, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send(c, 0x12, AC_VERB_GET_POWER_STATE, 0);
	cold_send(c, 0x07, AC_VERB_GET_POWER_STATE, 0);
	cold_param(c, 0x08, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x08, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x08, AC_PAR_CONNLIST_LEN);
	cold_send(c, 0x08, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send(c, 0x11, AC_VERB_GET_POWER_STATE, 0);
	cold_send(c, 0x08, AC_VERB_GET_POWER_STATE, 0);
	cold_param(c, 0x09, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x09, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x09, AC_PAR_CONNLIST_LEN);
	cold_send(c, 0x09, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send(c, 0x0e, AC_VERB_GET_POWER_STATE, 0);
	cold_send(c, 0x09, AC_VERB_GET_POWER_STATE, 0);
	cold_param(c, 0x0a, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x0a, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x0a, AC_PAR_CONNLIST_LEN);
	cold_send(c, 0x0a, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send(c, 0x13, AC_VERB_GET_POWER_STATE, 0);
	cold_send(c, 0x0a, AC_VERB_GET_POWER_STATE, 0);
	cold_param(c, 0x0b, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x0b, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x0b, AC_PAR_CONNLIST_LEN);
	cold_send(c, 0x0b, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send(c, 0x0b, AC_VERB_GET_POWER_STATE, 0);
	cold_send(c, 0x02, AC_VERB_GET_POWER_STATE, 0);
	cold_param(c, 0x0c, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x0c, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x0c, AC_PAR_CONNLIST_LEN);
	cold_send(c, 0x0c, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send(c, 0x0c, AC_VERB_GET_POWER_STATE, 0);
	cold_send(c, 0x05, AC_VERB_GET_POWER_STATE, 0);
	cold_param(c, 0x0d, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x0d, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x0d, AC_PAR_CONNLIST_LEN);
	cold_send(c, 0x0d, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send(c, 0x0d, AC_VERB_GET_POWER_STATE, 0);
	cold_send(c, 0x0d, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	cold_send(c, 0x06, AC_VERB_GET_POWER_STATE, 0);
	cold_send(c, 0x06, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	cold_param(c, 0x0e, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x0f, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x0f, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x0f, AC_PAR_CONNLIST_LEN);
	cold_send(c, 0x0f, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send(c, 0x0f, AC_VERB_GET_POWER_STATE, 0);
	cold_send(c, 0x04, AC_VERB_GET_POWER_STATE, 0);
	cold_param(c, 0x10, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x10, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x10, AC_PAR_CONNLIST_LEN);
	cold_send(c, 0x10, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send(c, 0x10, AC_VERB_GET_POWER_STATE, 0);
	cold_send(c, 0x03, AC_VERB_GET_POWER_STATE, 0);
	cold_param(c, 0x11, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x11, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x11, AC_PAR_CONNLIST_LEN);
	cold_send(c, 0x11, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send(c, 0x11, AC_VERB_GET_POWER_STATE, 0);
	cold_send(c, 0x04, AC_VERB_GET_POWER_STATE, 0);
	cold_param(c, 0x12, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x13, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x14, AC_PAR_CONNLIST_LEN);
	cold_param(c, WIDGET_CHIP_CTRL, AC_PAR_CONNLIST_LEN);
	cold_param(c, WIDGET_DSP_CTRL, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x17, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x18, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x18, AC_PAR_CONNLIST_LEN);
	cold_param(c, 0x18, AC_PAR_CONNLIST_LEN);
	cold_send(c, 0x18, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send(c, 0x18, AC_VERB_GET_POWER_STATE, 0);
	cold_send(c, 0x18, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	cold_send(c, 0x17, AC_VERB_GET_POWER_STATE, 0);
	cold_send(c, 0x17, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
}

/*
 * The inputs (C2B1 codec1_widgets, part 2; Windows' pin set-up): mic pin
 * 0x12 with ADC 0x07, then the line in pin 0x11 with ADC 0x08 (the AE-9
 * has none), each pin control read before it is written; pin 0x13
 */
static void ae7_codec1_inputs(struct ae9_cold *c)
{
	cold_param(c, 0x07, AC_PAR_VENDOR_ID);
	cold_param(c, 0x12, AC_PAR_PIN_CAP);
	cold_param(c, 0x08, AC_PAR_VENDOR_ID);
	cold_param(c, 0x11, AC_PAR_PIN_CAP);
	cold_param(c, 0x09, AC_PAR_VENDOR_ID);
	cold_param(c, 0x0a, AC_PAR_VENDOR_ID);
	cold_param(c, 0x02, AC_PAR_VENDOR_ID);
	cold_param(c, 0x05, AC_PAR_VENDOR_ID);
	cold_param(c, 0x12, AC_PAR_PIN_CAP);
	cold_param(c, 0x07, AC_PAR_PCM);
	cold_param(c, 0x07, AC_PAR_VENDOR_ID);
	cold_param(c, 0x12, AC_PAR_VENDOR_ID);
	cold_param(c, 0x12, AC_PAR_PIN_CAP);
	cold_send(c, 0x12, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x12, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x12, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_VREF_80);
	cold_send(c, 0x12, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN | AC_PINCTL_VREF_80);
	cold_param(c, 0x12, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x12, AC_PAR_AMP_IN_CAP);
	cold_param(c, 0x07, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x07, AC_PAR_AMP_IN_CAP);
	cold_send(c, 0x07, AE9_VERB_GET_FINE_VOL_L, 0);
	cold_send(c, 0x07, AC_VERB_GET_AMP_GAIN_MUTE, AC_AMP_GET_INPUT | AC_AMP_GET_LEFT);
	cold_send(c, 0x07, AE9_VERB_GET_FINE_VOL_R, 0);
	cold_send(c, 0x07, AC_VERB_GET_AMP_GAIN_MUTE, AC_AMP_GET_INPUT | AC_AMP_GET_RIGHT);
	cold_send(c, 0x12, AC_VERB_GET_AMP_GAIN_MUTE, AC_AMP_GET_INPUT | AC_AMP_GET_LEFT);
	cold_send(c, 0x12, AC_VERB_GET_AMP_GAIN_MUTE, AC_AMP_GET_INPUT | AC_AMP_GET_RIGHT);
	cold_param(c, 0x11, AC_PAR_PIN_CAP);
	cold_param(c, 0x08, AC_PAR_PCM);
	cold_param(c, 0x08, AC_PAR_VENDOR_ID);
	cold_param(c, 0x11, AC_PAR_VENDOR_ID);
	cold_param(c, 0x11, AC_PAR_PIN_CAP);
	cold_send(c, 0x11, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x12, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_VREF_80);
	cold_send(c, 0x11, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x12, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN | AC_PINCTL_VREF_80);
	cold_param(c, 0x11, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x11, AC_PAR_AMP_IN_CAP);
	cold_param(c, 0x08, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x08, AC_PAR_AMP_IN_CAP);
	cold_send(c, 0x08, AE9_VERB_GET_FINE_VOL_L, 0);
	cold_send(c, 0x08, AC_VERB_GET_AMP_GAIN_MUTE, AC_AMP_GET_INPUT | AC_AMP_GET_LEFT);
	cold_send(c, 0x08, AE9_VERB_GET_FINE_VOL_R, 0);
	cold_send(c, 0x08, AC_VERB_GET_AMP_GAIN_MUTE, AC_AMP_GET_INPUT | AC_AMP_GET_RIGHT);
	cold_send(c, 0x11, AC_VERB_GET_AMP_GAIN_MUTE, AC_AMP_GET_INPUT | AC_AMP_GET_LEFT);
	cold_send(c, 0x11, AC_VERB_GET_AMP_GAIN_MUTE, AC_AMP_GET_INPUT | AC_AMP_GET_RIGHT);
	cold_send(c, 0x11, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 2);
	cold_param(c, 0x13, AC_PAR_PIN_CAP);
	cold_param(c, 0x0a, AC_PAR_PCM);
	cold_param(c, 0x0a, AC_PAR_VENDOR_ID);
	cold_param(c, 0x13, AC_PAR_VENDOR_ID);
	cold_param(c, 0x13, AC_PAR_PIN_CAP);
	cold_send(c, 0x13, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x13, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x13, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x13, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN);
}

/*
 * Optical out (C2B1 codec1_widgets, part 3): converter 0x05's S/PDIF
 * settings (digital 1 = 0x15, 2 = 0x12, 3 = 0x80; the AE-9's optical is on
 * codec 2), pin 0x0c out
 */
static void ae7_codec1_spdif(struct ae9_cold *c)
{
	cold_param(c, 0x0c, AC_PAR_PIN_CAP);
	cold_send(c, 0x05, AC_VERB_SET_DIGI_CONVERT_1, 0x15);
	cold_send(c, 0x05, AC_VERB_SET_DIGI_CONVERT_2, 0x12);
	cold_send(c, 0x05, AC_VERB_SET_DIGI_CONVERT_3, 0x80);
	cold_param(c, 0x05, AC_PAR_PCM);
	cold_param(c, 0x05, AC_PAR_VENDOR_ID);
	cold_param(c, 0x0c, AC_PAR_VENDOR_ID);
	cold_param(c, 0x0c, AC_PAR_PIN_CAP);
	cold_send(c, 0x0c, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0c, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0c, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0c, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_OUT_EN);
	cold_param(c, 0x0c, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x05, AC_PAR_AUDIO_WIDGET_CAP);
}

/*
 * The switch chip's 0x32 (item 4, FPHPCenterLFE_SelPin) as the output the
 * "Head Unit Output" control holds: 0x3f speakers, 0x00 headphones
 * (Windows' start-up writes the saved output's value: golden C1B3 3f on
 * the speakers, C2B1 00 on the headphones; kit 9 wrote 00 on both)
 */
static u8 ae7_mux_out(struct creative_chip *chip)
{
	return chip->ae7.out ? 0x00 : AE7_SW_ON;
}

/*
 * The outputs (C2B1 codec1_widgets, part 4): pin 0x0b (the Front jack:
 * line out) EAPD 2 and out, pin 0x0f out with its headphone amp, pin 0x10
 * off then out, the switch chip's 0x32 = the output's value before each of
 * pin 0x10's writes (its item 4 follows that pin, ae7_mux_out());
 * converters 0x03/0x04's levels read; jack sense on pins 0x0b and 0x10
 * (tag 5)
 */
static void ae7_codec1_outputs(struct ae9_cold *c)
{
	cold_param(c, 0x0b, AC_PAR_PIN_CAP);
	cold_param(c, 0x02, AC_PAR_PCM);
	cold_param(c, 0x02, AC_PAR_VENDOR_ID);
	cold_param(c, 0x04, AC_PAR_VENDOR_ID);
	cold_param(c, 0x03, AC_PAR_VENDOR_ID);
	cold_param(c, 0x0b, AC_PAR_VENDOR_ID);
	cold_param(c, 0x0b, AC_PAR_PIN_CAP);
	cold_send(c, 0x0b, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0b, AC_VERB_SET_EAPD_BTLENABLE, 0x00);
	cold_send(c, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0b, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0b, AC_VERB_SET_EAPD_BTLENABLE, 0x02);
	cold_send(c, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_OUT_EN);
	cold_param(c, 0x0f, AC_PAR_VENDOR_ID);
	cold_param(c, 0x0f, AC_PAR_PIN_CAP);
	cold_send(c, 0x0f, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0f, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN);
	cold_param(c, 0x10, AC_PAR_VENDOR_ID);
	cold_param(c, 0x10, AC_PAR_PIN_CAP);
	ae7_dac(c->chip, AE7_SW, SW_MUX_OUT, ae7_mux_out(c->chip));
	cold_send(c, 0x10, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	ae7_dac(c->chip, AE7_SW, SW_MUX_OUT, ae7_mux_out(c->chip));
	cold_send(c, 0x10, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN);
	cold_param(c, 0x0b, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x02, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x02, AC_PAR_AMP_OUT_CAP);
	cold_send(c, 0x03, AE9_VERB_GET_FINE_VOL_L, 0);
	cold_send(c, 0x03, AC_VERB_GET_AMP_GAIN_MUTE, AC_AMP_GET_OUTPUT | AC_AMP_GET_LEFT);
	cold_send(c, 0x03, AE9_VERB_GET_FINE_VOL_R, 0);
	cold_send(c, 0x03, AC_VERB_GET_AMP_GAIN_MUTE, AC_AMP_GET_OUTPUT | AC_AMP_GET_RIGHT);
	cold_send(c, 0x04, AE9_VERB_GET_FINE_VOL_L, 0);
	cold_send(c, 0x04, AC_VERB_GET_AMP_GAIN_MUTE, AC_AMP_GET_OUTPUT | AC_AMP_GET_LEFT);
	cold_send(c, 0x04, AE9_VERB_GET_FINE_VOL_R, 0);
	cold_send(c, 0x04, AC_VERB_GET_AMP_GAIN_MUTE, AC_AMP_GET_OUTPUT | AC_AMP_GET_RIGHT);
	cold_send(c, 0x0b, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 5);
	cold_send(c, 0x10, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 5);
}

/*
 * The pins once more with their jack sense (C2B1 codec1_widgets, part 5):
 * the AFG's vendor verb 0x790, pin 0x12's input amp at step 1, pins 0x12
 * (tag 1), 0x11 (tag 2), 0x13 (tag 3), 0x0c, converter 0x05's S/PDIF
 * settings twice, pins 0x0b, 0x0f and 0x10 (tag 5, 0x32 = the output's
 * value around pin 0x10 again); last, the AFG's power states (Windows' 1.5 s
 * timer follows)
 */
static void ae7_codec1_jacks(struct ae9_cold *c)
{
	cold_send(c, AE9_NID_AFG, AE9_VERB_GET_790, 0);
	cold_send(c, AE9_NID_AFG, AE9_VERB_SET_790, 0x23);
	cold_send(c, 0x12, AC_VERB_SET_AMP_GAIN_MUTE, AC_AMP_SET_INPUT | AC_AMP_SET_LEFT | 0x01);
	cold_send(c, 0x12, AC_VERB_SET_AMP_GAIN_MUTE, AC_AMP_SET_INPUT | AC_AMP_SET_RIGHT | 0x01);
	cold_send(c, 0x12, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 1);
	cold_send(c, 0x12, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN);
	cold_send(c, 0x12, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x12, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN | AC_PINCTL_VREF_80);
	cold_send(c, 0x12, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN | AC_PINCTL_VREF_80);
	cold_send(c, 0x11, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 2);
	cold_send(c, 0x11, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x12, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_VREF_80);
	cold_send(c, 0x11, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_SET_AMP_GAIN_MUTE, AC_AMP_SET_INPUT | AC_AMP_SET_LEFT | 0x00);
	cold_send(c, 0x11, AC_VERB_SET_AMP_GAIN_MUTE, AC_AMP_SET_INPUT | AC_AMP_SET_RIGHT | 0x00);
	cold_send(c, 0x12, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN | AC_PINCTL_VREF_80);
	cold_send(c, 0x13, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 3);
	cold_send(c, 0x13, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x13, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x13, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x13, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN);
	cold_send(c, 0x0c, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0c, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0c, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0c, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_OUT_EN);
	cold_send(c, 0x05, AC_VERB_SET_DIGI_CONVERT_1, 0x15);
	cold_send(c, 0x05, AC_VERB_SET_DIGI_CONVERT_2, 0x12);
	cold_send(c, 0x05, AC_VERB_SET_DIGI_CONVERT_3, 0x80);
	cold_send(c, 0x05, AC_VERB_SET_DIGI_CONVERT_1, 0x15);
	cold_send(c, 0x05, AC_VERB_SET_DIGI_CONVERT_2, 0x12);
	cold_send(c, 0x05, AC_VERB_SET_DIGI_CONVERT_3, 0x80);
	cold_send(c, 0x0b, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 5);
	cold_send(c, 0x0b, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0b, AC_VERB_SET_EAPD_BTLENABLE, 0x00);
	cold_send(c, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0b, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0b, AC_VERB_SET_EAPD_BTLENABLE, 0x02);
	cold_send(c, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_OUT_EN);
	cold_send(c, 0x0f, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 5);
	cold_send(c, 0x0f, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0f, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN);
	cold_send(c, 0x10, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 5);
	ae7_dac(c->chip, AE7_SW, SW_MUX_OUT, ae7_mux_out(c->chip));
	cold_send(c, 0x10, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	ae7_dac(c->chip, AE7_SW, SW_MUX_OUT, ae7_mux_out(c->chip));
	cold_send(c, 0x10, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN);
	cold_param(c, AE9_NID_AFG, AC_PAR_POWER_STATE);
}

/*
 * Windows' AE-7 start-up up to its firmware download (C2B1: codec_enum to
 * dl_prefix), with its waits and checks: codec 1's wake (Windows' AFG
 * power check: 705 D0 + 2 s when it isn't in D0), the DSP_INIT busy retry,
 * the 1.5 s timer, the ramp checks and the recovery cycle (the AE-9's: the
 * same generic code in Windows; never seen on an AE-7). The AE-9 steps are
 * called as they are where C2B1 has them byte for byte (pre_codec1_start,
 * pre_codec1_reset, pre_8051_setup, pre_codec1_chip, ae9_dsp_init,
 * cold_ramp_read, pre_dl_prefix).
 */
void creative_ae7_predownload(struct creative_chip *chip)
{
	struct ae9_cold c = { .chip = chip };
	int ramp[AE9_RAMP_N];
	u32 f05;

	ae7_codecs_found(&c);
	ae7_probe(&c);
	ae7_board_power(&c, true);
	f05 = pre_codec1_start(&c);
	ae9_codec1_wake(chip, f05);
	pre_codec1_reset(&c);
	ae7_codec1_pins(&c);
	pre_8051_setup(&c);
	pre_codec1_chip(&c);
	ae9_dsp_init(&c);
	ae7_codec1_params(&c);
	ae7_codec1_widgets(&c);
	ae7_codec1_inputs(&c);
	ae7_codec1_spdif(&c);
	ae7_codec1_outputs(&c);
	ae7_codec1_jacks(&c);
	/* Windows tracks the pins as in their speaker set from here (C1B3's
	 * speaker select at load writes none, C2B1's headphone select all) */
	chip->ae7.pins_state = 0;

	msleep(1500);		/* Windows' 1.5 s timer (C2B1: 1.475 s) */
	cold_ramp_read(&c, ramp);
	if (ae9_ramp_failed(ramp))
		chip->ae7.pins_state = -1;	/* the recovery cycle sets the AE-9's pins */
	ae9_recovery(chip, f05, ramp);

	pre_dl_prefix(&c);
	creative_dsp_chipio_addr_invalidate();
	dev_info(chip->dev, "AE-7: replayed %u pre-download verbs (%u errors)\n", c.verbs, c.errs);
}

/* ------------------------------------------------------------------ */
/* After the DSP's boot handshake					  */
/* ------------------------------------------------------------------ */

/* mic 2's configuration (LineMic2Config): connection points 0x05 and 0x0e at 96 kHz, MASTERCONTROL req 1 = 0 */
static void ae7_mic2_config(struct ae9_tl *tl)
{
	tl_conn_rate(&tl->c, 0x05, AE9_SR_96000);
	tl_conn_rate(&tl->c, 0x0e, AE9_SR_96000);
	tl_set(tl, AE9_MID_MASTER, 0x01, 0);
}

/*
 * How long the remap waits for stream 0x0c's slot list: Windows' 10 ms
 * after the stream's start (C2B1: its head read 17.6 ms after the start,
 * 0x0c), then up to 20 more reads 5 ms apart while the head still reads
 * 0xff. Kit 4 on s3boun3t's AE-7 read the head 0.2 ms after the start:
 * 0xff, no slot remapped; the list (0x0c..0x17, as Windows') was there
 * when debugfs read it 0.4 s later.
 */
#define AE7_REMAP_WAIT_MS	10
#define AE7_REMAP_POLL_US	5000
#define AE7_REMAP_POLLS		20

/*
 * Windows' map of stream 0x0c's lanes c0..cb to the CA0132's DAC ports
 * (AltDACMapping, C2B1): the default of the walk's table
 * (chip->ae7.lane_port, ae7_lanes= changes it: creative_ae7_core.c) and
 * debugfs state's "as Windows"
 */
const u8 ae7_lane_win[AE7_REMAP_SLOTS] = {
	0xe0, 0xe1, 0xe4, 0xe5, 0xe2, 0xe3, 0xe8, 0xe9, 0xec, 0xed, 0xea, 0xeb,
};

/*
 * The router slots the 8051 gave the DSP's processed output (stream 0x0c):
 * their destinations remapped, Windows' AltDACMapping walk (C2B1 post_dsp):
 * the first slot from XRAM 0x1584, the next from XRAM 0x1478 + slot (0xff
 * ends it), each slot's word read and written back with bits 15:8 = the
 * CA0132 DAC port of its lane (e0 e1 e4 e5 e2 e3, then the 96 kHz second
 * samples e8 e9 ec ed ea eb), then the table applied (0x19042c = 1). The
 * 8051's own order is e0..e5, e8..ed: lanes c2/c3 (the DSP's centre/LFE,
 * as on the AE-9) and c4/c5 (its rear) on each other's DAC pair, the
 * AE-7's Rear and C/Sub jacks swapped (inferred: the CA0132's converters
 * 0x03/0x04 behind e2/e3 and e4/e5); c0/c1 -> e0/e1 is the same in both.
 * The headphones and the Front jack (the ES9018 on I2S ports 0x40/0x41,
 * stream 0x18) don't go through these slots. The ports come from
 * chip->ae7.lane_port: Windows' map, or a test build's ae7_lanes=
 * (lanes c2..c5 only).
 * Windows waits 10 ms after the stream's start and reads the head once;
 * ours waits as long, then polls the head while it reads 0xff (wait:
 * the start-up's walk; the heartbeat's retry reads it once). Windows
 * writes whatever it walks; ours checks each slot first (a slot number in
 * range and not seen before, its word a lane of the DSP's output in order:
 * source c0 + lane, nothing above bit 17) and stops at the first that
 * isn't, the rest left alone. Returns the slots remapped (all 12 = as
 * Windows); creative_ae7_unmute() keeps the DAC muted until they are.
 */
static unsigned int ae7_remap(struct ae9_cold *c, bool wait)
{
	struct creative_chip *chip = c->chip;
	const u8 *dst = chip->ae7.lane_port;
	u8 seen[AE7_REMAP_SLOTS];
	const char *why = NULL;
	unsigned int i, polls = 0, done = 0, prev;
	u32 head = 0xff, slot, w = 0;

	if (wait)
		msleep(AE7_REMAP_WAIT_MS);
	for (;;) {
		if (cold_xram_read(c, AE7_XRAM_PROC_HEAD, &head) < 0)
			head = 0xff;
		if (head != 0xff || !wait || polls == AE7_REMAP_POLLS)
			break;
		usleep_range(AE7_REMAP_POLL_US, AE7_REMAP_POLL_US + 1000);
		polls++;
	}
	slot = head;
	for (i = 0; i < AE7_REMAP_SLOTS; i++) {
		unsigned int k, v = 0;

		w = 0;		/* the warning's word: this slot's, once read */
		if (slot == 0xff) {
			why = i ? "the list ended early" : "no list yet (head 0xff)";
			break;
		}
		for (k = 0; k < i && seen[k] != slot; k++)
			;
		if (slot >= AE7_RT_SLOTS || k < i) {
			why = "a bad slot number";
			break;
		}
		seen[i] = slot;
		if (creative_dsp_chipio_read(chip, 0x190000 + 4 * slot, &v) < 0) {
			creative_dsp_chipio_addr_invalidate();
			why = "a slot not read";
			break;
		}
		w = v;
		if ((w >> 18) || (w & 0xff) != 0xc0 + i) {
			creative_dsp_chipio_addr_invalidate();
			why = "a slot that isn't the next lane";
			break;
		}
		cold_chipio_write(c, 0x190000 + 4 * slot, (w & ~0xff00U) | dst[i] << 8);
		done++;
		creative_dsp_chipio_addr_invalidate();
		if (cold_xram_read(c, AE7_XRAM_SLOT_NEXT + slot, &slot) < 0)
			slot = 0xff;
	}
	cold_chipio_write(c, AE9_RT_APPLY, 1);
	prev = chip->ae7.remapped;
	chip->ae7.remapped = done;
	chip->ae7.remap_head = head;
	chip->ae7.remap_ms = wait ? AE7_REMAP_WAIT_MS + polls * (AE7_REMAP_POLL_US / 1000) : 0;
	chip->ae7.remap_walks++;
	if (done == AE7_REMAP_SLOTS)
		dev_info(chip->dev, "AE-7: router remap, %u of %u slots (stream 0x0c's list from slot 0x%02x, %u ms after its start%s)\n",
			 done, AE7_REMAP_SLOTS, head, chip->ae7.remap_ms,
			 wait ? "" : ", the heartbeat's retry");
	else if (wait || done != prev)	/* a retry that got no further: quiet */
		dev_warn(chip->dev, "AE-7: router remap, %u of %u slots (Windows: %u): %s at lane %u (slot 0x%02x, word 0x%x, head 0x%02x after %u ms); the DAC stays muted until all are done\n",
			 done, AE7_REMAP_SLOTS, AE7_REMAP_SLOTS, why, i, slot & 0xff, w, head,
			 chip->ae7.remap_ms);
	return done;
}

/* the heartbeat's retry while the start-up's walk is incomplete (caller holds replay_mutex) */
int creative_ae7_remap(struct creative_chip *chip)
{
	struct ae9_cold c = { .chip = chip };

	return ae7_remap(&c, false);
}

/*
 * Direct Mode's router walk (phase 2; Windows' AE-7 stage 3 FUN_0003cdb4,
 * the walk the AE-9's creative_ae9_rt_direct_walk() does for its board):
 * stream 0x05's slots, the first from XRAM 0x157d, the next from XRAM
 * 0x1478 + slot (0xff ends it), each slot's word written back with bits
 * 15:8 = the I2S port 0x40 + lane and bit 17 cleared, nslots of them
 * ((rate / 48 kHz) x 2), then the table applied (0x19042c = 1). Checked as
 * the remap is (ae7_remap()): a slot number in range and not seen before,
 * its word the next lane of the host stream (source = lane, nothing above
 * bit 17); the walk stops at the first that isn't and leaves the rest. No
 * fallback to fixed slots (the AE-9's walk has one: on the AE-7 those are
 * other streams' slots). Returns the slots set; the caller's gate keeps the
 * DAC muted unless all were. Caller holds replay_mutex, after the bind that
 * built the list (and the 8051's idle wait after it).
 */
unsigned int creative_ae7_direct_walk(struct creative_chip *chip, unsigned int nslots)
{
	struct ae9_cold c = { .chip = chip };
	u8 seen[16];
	const char *why = NULL;
	unsigned int i, done = 0;
	u32 head = 0xff, slot, w = 0;

	nslots = min_t(unsigned int, nslots, ARRAY_SIZE(seen));
	if (cold_xram_read(&c, AE7_XRAM_HOST_HEAD, &head) < 0)
		head = 0xff;
	slot = head;
	for (i = 0; i < nslots; i++) {
		unsigned int k, v = 0;

		w = 0;
		if (slot == 0xff) {
			why = i ? "the list ended early" : "no list (head 0xff)";
			break;
		}
		for (k = 0; k < i && seen[k] != slot; k++)
			;
		if (slot >= AE7_RT_SLOTS || k < i) {
			why = "a bad slot number";
			break;
		}
		seen[i] = slot;
		if (creative_dsp_chipio_read(chip, 0x190000 + 4 * slot, &v) < 0) {
			creative_dsp_chipio_addr_invalidate();
			why = "a slot not read";
			break;
		}
		w = v;
		if ((w >> 18) || (w & 0xff) != i) {
			creative_dsp_chipio_addr_invalidate();
			why = "a slot that isn't the host stream's next lane";
			break;
		}
		cold_chipio_write(&c, 0x190000 + 4 * slot, (0x40 + i) << 8 | (w & 0xfffd00ff));
		done++;
		creative_dsp_chipio_addr_invalidate();
		if (cold_xram_read(&c, AE7_XRAM_SLOT_NEXT + slot, &slot) < 0)
			slot = 0xff;
	}
	cold_chipio_write(&c, AE9_RT_APPLY, 1);
	creative_dsp_chipio_addr_invalidate();
	if (done == nslots)
		dev_info(chip->dev, "AE-7: Direct Mode router walk, %u of %u slots (stream 0x05's list from slot 0x%02x)\n",
			 done, nslots, head);
	else
		dev_warn(chip->dev, "AE-7: Direct Mode router walk, %u of %u slots: %s at lane %u (slot 0x%02x, word 0x%x, head 0x%02x); the DAC stays muted\n",
			 done, nslots, why, i, slot & 0xff, w, head);
	return done;
}

/*
 * The DAC's I2S ports (0x40-0x7f) fed by nothing but the path in effect
 * (phase 2, ours; Windows leaves it to the 8051): every active router slot
 * with a source in [src_lo, src_hi) and an I2S port as its destination is
 * switched off (state bits cleared, the AE-9's way), the table applied if
 * any was. Direct Mode on: the DSP's outputs (0xc0-0xdf: stream 0x18's
 * lanes, stopped just before), the AE-9's 2026-11-11 rule (a DSP route left
 * on a port in Direct Mode: two feeds, gated metallic audio until a
 * power-off). Direct Mode off: the host's (0x00-0x1f: stream 0x05's Direct
 * slots), the AE-9's 2026-11-06 rule (an HDA 01 -> 0x41 slot left from a
 * Direct stream played the right channel direct and through the DSP, an
 * echo). Not touched: stream 0x0c's routes to the CA0132's DACs (0xe0..),
 * the host's feeds into the DSP (0x80-0xbf). Returns how many; caller
 * holds replay_mutex, the 8051 idle.
 */
unsigned int creative_ae7_ports_off(struct creative_chip *chip, u8 src_lo, u8 src_hi)
{
	struct ae9_cold c = { .chip = chip };
	unsigned int i, off = 0;

	for (i = 0; i < AE7_RT_SLOTS; i++) {
		unsigned int v = 0;
		u8 src, dst;

		if (creative_dsp_chipio_read(chip, 0x190000 + 4 * i, &v) < 0)
			break;
		src = v & 0xff;
		dst = (v >> 8) & 0xff;
		if ((v >> 18) || !((v >> 16) & 3) || src < src_lo || src >= src_hi ||
		    dst < 0x40 || dst >= 0x80)
			continue;
		cold_chipio_write(&c, 0x190000 + 4 * i, v & 0xffff);
		off++;
	}
	if (off)
		cold_chipio_write(&c, AE9_RT_APPLY, 1);
	creative_dsp_chipio_addr_invalidate();
	return off;
}

/*
 * Windows' DSP settings once its image is loaded (C2B1 post_dsp: generic,
 * "DspLoaded", run for the AE-7), with CrystalVoice off: the AE-9 tail's
 * mic values (connection points 0x03/0x0c at 96 kHz, MASTERCONTROL req 0 =
 * 3.0) where C2B1, CrystalVoice on, had 16 kHz and 1.0, and without its
 * voice-processor lines (MASTERCONTROL req 5 and 0x3d = 1.0, VIP source 1).
 * In order: 0x100e34; mics 1 and 2; stream 0x0c 6 channels and on; the
 * router remap; item 0xb = 1 (speakers) without its DAC half (the DAC
 * isn't clocked here: Windows' 48/0d-19 and 11-14 are never acknowledged):
 * the switch chip's 0x30 = 0, DSP 0x96 req 0x29/0x2a = 0; item 0xc =
 * 1 (0x2e = 0x3f); item 0xf = 1: GPIO pin 0 low (the headphone amp off);
 * the mic monitors off; the CrystalVoice slot in its CV-off form (capture
 * B, CrystalVoice off, seq 5194-5196: connection point 0x03 selected, its
 * rate read; kit 9 sent mic 1's configuration again here); the DSP's
 * headroom 0, What U Hear's and the speakers' source 2.0; the input select
 * (0x28 = 0: mic); mics 1 and 2 again; item 4 (0x32) 0x3f, then the
 * output's value (ae7_mux_out()).
 */
static void ae7_dsp_loaded(struct ae9_tl *tl)
{
	struct ae9_cold *c = &tl->c;
	struct creative_chip *chip = c->chip;

	cold_chipio_write(c, 0x100e34, 0xf1000000);
	tail_mic1_config(tl);
	ae7_mic2_config(tl);
	tl_stream_channels(c, AE9_STREAM_DSP_PROC, 6);
	tl_stream_on(c, AE9_STREAM_DSP_PROC);
	ae7_remap(c, true);
	ae7_dac(chip, AE7_SW, SW_HP, 0);
	tl_set(tl, AE9_MID_FX_OUT, 0x29, 0);
	tl_set(tl, AE9_MID_FX_OUT, 0x2a, 0);
	ae7_dac(chip, AE7_SW, SW_FRONT, AE7_SW_ON);
	creative_ae9_gpio_set(chip, AE7_GPIO_AMP, false);
	tl_set(tl, AE9_MID_MASTER, 0x0d, 0);
	tl_set(tl, AE9_MID_MASTER, 0x0e, 0);
	tl_ctl_param(c, CONTROL_PARAM_CONN_POINT_ID, 0x03);
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_GET, CONTROL_PARAM_CONN_POINT_SR);
	tl_set(tl, AE9_MID_FX_OUT, 0x3c, 0);
	tl_set(tl, 0x31, 0x00, AE9_F_TWO);
	tl_set(tl, AE9_MID_SPK_VOL, 0x00, AE9_F_TWO);
	ae7_dac(chip, AE7_SW, SW_INPUT, 0);
	tail_mic1_config(tl);
	ae7_mic2_config(tl);
	ae7_dac(chip, AE7_SW, SW_MUX_OUT, AE7_SW_ON);
	ae7_dac(chip, AE7_SW, SW_MUX_OUT, ae7_mux_out(chip));
}

/*
 * The AE-7's I2S pin group (its board's PMU pin bytes 0/1/3/5): PLL/PMU
 * 0x41 = 0xc8, 0x45 = 0xcc, 0x40 = 0xcb, 0x43 = 0xc7, 0x51 = 0x8d; the
 * AE-9's tl_i2s_pins() has its own
 */
static void ae7_pll_group(struct ae9_cold *c)
{
	cold_pll_pmu_write(c, 0x41, 0xc8);
	cold_pll_pmu_write(c, 0x45, 0xcc);
	cold_pll_pmu_write(c, 0x40, 0xcb);
	cold_pll_pmu_write(c, 0x43, 0xc7);
	cold_pll_pmu_write(c, 0x51, 0x8d);
}

/* the ASI rate class read back until it holds 0x81 (Windows: up to 11 reads) */
static void ae7_asi_rate_wait(struct ae9_cold *c)
{
	unsigned int i;
	u32 r = 0;

	for (i = 0; i < 11; i++) {
		if (cold_verb(c, creative_ae9_verb(AE9_CAD, WIDGET_CHIP_CTRL,
						   0xf00 | (AE9_VERB_CHIPIO_ASI_RATE & 0xff), 0),
			      &r) == 0 && (r & 0xff) == 0x81)
			return;
		usleep_range(1000, 1500);
	}
	dev_warn(c->chip->dev, "AE-7: ASI rate class reads 0x%02x, not 0x81\n", r & 0xff);
}

/*
 * Windows' AE-7 start, up to its ASI (C2B1 asi_pll): the board power-up
 * again (without the SFR 0x90 toggle and, in the DAC's pre-clock window,
 * without its 48/07 mute); the playback engine's start as the AE-9's
 * tail_engine_start() but with the AE-7's pin group; the ASI rate class 1
 * (0x81) and its read-back; the switch chip's 0x2b = 0 (DSD route off).
 * The DAC answers from the pin group on: the expected-NACK window ends.
 */
static void ae7_asi_pll(struct ae9_tl *tl)
{
	struct ae9_cold *c = &tl->c;
	struct creative_chip *chip = c->chip;

	ae7_board_power(c, false);
	tl_ctl_param(c, AE9_CTL_PARAM_MCH, 3);
	cold_ctl_flag(c, CONTROL_FLAG_ASI_96KHZ, true);
	cold_send(c, WIDGET_CHIP_CTRL, AE9_VERB_CHIPIO_PMU_BIT, 0x83);
	tl_ctl_param(c, CONTROL_PARAM_ASI, 0);
	cold_send(c, 0x17, AE9_VERB_SET_NODE_WORD_HI, 0);
	cold_xram_write(c, 0xfa92, 0x22);
	ae7_pll_group(c);
	chip->dac_i2c.expect = false;
	cold_send(c, WIDGET_CHIP_CTRL, AE9_VERB_CHIPIO_ASI_RATE, 0x81);
	ae7_asi_rate_wait(c);
	chip->ae7.asi_class = 0x81;	/* phase 2's rate switches write it only when it changes */
	ae7_dac(chip, AE7_SW, SW_DSD, 0);
}

/*
 * Stream 0x05 (host audio) to the DSP, stream 0x18 (its output) to the
 * front DAC 0xd0 at 96 kHz, 6 channels, then enabled only if its Active
 * byte reads 0 (as Windows: C2B1 routing reads 0 and enables it, its second
 * routing block reads 1 and doesn't)
 */
static void ae7_routes(struct ae9_cold *c)
{
	u32 active = 1;

	tl_stream_route(c, AE9_STREAM_HOST, AE9_CP_HOST, 0);
	tl_stream_route(c, AE9_STREAM_DSP_OUT, AE9_CP_DSP_OUT, AE9_CP_FRONT_DAC);
	tl_conn_rate(c, AE9_CP_FRONT_DAC, AE9_SR_96000);
	tl_stream_channels(c, AE9_STREAM_DSP_OUT, 6);
	if (cold_xram_read(c, AE9_XRAM_STREAM_ACTIVE(AE9_STREAM_DSP_OUT), &active) == 0 &&
	    !active)
		tl_stream_on(c, AE9_STREAM_DSP_OUT);
}

/*
 * The I2S to the DAC clocked for 96 kHz (Windows' AE-7 clock set-up): ASI
 * 4, PLL/PMU 0x43 = 0xc7, the clock descriptors 0x1f101 (0x189000/04; the
 * AE-7 has no 0x189008), the rate descriptor 0x14004, 0x189028; the DAC's
 * input (48/01 = 0x80: 32-bit I2S) and clock (48/0a = 0x05); the pin
 * group; ASI 7
 */
static void ae7_i2s_96k(struct ae9_cold *c)
{
	static const u8 dac_in[][3] = {
		{ AE7_DAC, K2M_INPUT, 0x80 }, { AE7_DAC, K2M_CLOCK, 0x05 },
	};

	tl_ctl_param(c, CONTROL_PARAM_ASI, 4);
	cold_pll_pmu_write(c, 0x43, 0xc7);
	cold_chipio_write(c, 0x189000, 0x0001f101);
	cold_chipio_write(c, 0x189004, 0x0001f101);
	cold_chipio_write(c, 0x189024, 0x00014004);
	cold_chipio_write(c, 0x189028, 0x0002000f);
	creative_dsp_ca0113_list_quiet(c->chip, dac_in, ARRAY_SIZE(dac_in));
	ae7_pll_group(c);
	tl_ctl_param(c, CONTROL_PARAM_ASI, 7);
}

/*
 * Windows' AE-7 DAC init (C2B1 routing): S/PDIF source DATA2, automute
 * off, de-emphasis bypassed, clock default, DPLL bandwidth 0x5f; the
 * master trim at 0 dB; muted (48/07 read, bits 1:0 set); the volume. Ours:
 * the volume is the user's (at load the safe -36 dB), where Windows writes
 * 0 dB and its endpoint volume after; the DAC stays muted until the output
 * select and the gate (creative_ae7_unmute()).
 */
static void ae7_dac_init(struct creative_chip *chip)
{
	static const u8 init[][3] = {
		{ AE7_DAC, K2M_SPDIF_SRC, 0x12 }, { AE7_DAC, K2M_AUTOMUTE, 0x00 },
		{ AE7_DAC, K2M_DEEMPH, 0x48 }, { AE7_DAC, K2M_CLOCK, 0x05 },
		{ AE7_DAC, K2M_DPLL, 0x5f },
		{ AE7_DAC, K2M_MASTER_TRIM, 0xff }, { AE7_DAC, K2M_MASTER_TRIM + 1, 0xff },
		{ AE7_DAC, K2M_MASTER_TRIM + 2, 0xff }, { AE7_DAC, K2M_MASTER_TRIM + 3, 0x7f },
	};
	u8 att = chip->hu_mute ? 0xff : chip->hu_att;
	int bad;

	bad = creative_dsp_ca0113_list_quiet(chip, init, ARRAY_SIZE(init));
	bad += ae7_dac07(chip, 0, K2M_MUTE);
	bad += ae7_dac(chip, AE7_DAC, K2M_VOLUME_L, att);
	bad += ae7_dac(chip, AE7_DAC, K2M_VOLUME_R, att);
	chip->ae7.unmuted = false;
	chip->ae7.amp_hold = false;	/* the routing checks the mute just written */
	chip->ae7.att_sent = -1;	/* the volume step proper sends it again, checked */
	if (bad)
		dev_warn(chip->dev, "AE-7: %d DAC init command(s) not acknowledged (I2C NACK)\n", bad);
}

/*
 * Windows' AE-7 routing (C2B1 routing): connection point 0x70 at 96 kHz,
 * stream 0x0c 6 channels and on; the routes (ae7_routes()); the I2S at 96
 * kHz; the DAC init; GPIO pin 0 high (the headphone amp on, the DAC still
 * muted) and pin 1 (the LED); 0x18b03c read, bit 7 cleared, written back
 * (Windows' SetSpeakerMode); the routes once more (Windows' SetDirect(0):
 * stream 0x18 is left as it is), the pin group, ASI 7. Windows unmutes the
 * DAC between the GPIO and 0x18b03c; ours waits for the output select.
 * Ours also reads 48/07 back before the amp goes on (creative_ae7_amp_on():
 * Windows doesn't): an unconfirmed mute leaves the amp off and counts as a
 * failed verb of the start-up; the output select then fails too (the gate
 * stays shut) and the heartbeat mutes, checks and selects again.
 */
static void ae7_routing(struct ae9_tl *tl)
{
	struct ae9_cold *c = &tl->c;
	struct creative_chip *chip = c->chip;

	tl_conn_rate(c, 0x70, AE9_SR_96000);
	tl_stream_channels(c, AE9_STREAM_DSP_PROC, 6);
	tl_stream_on(c, AE9_STREAM_DSP_PROC);
	ae7_routes(c);
	ae7_i2s_96k(c);
	ae7_dac_init(chip);
	if (creative_ae7_amp_on(chip) < 0)
		c->errs++;
	creative_ae9_gpio_set(chip, AE7_GPIO_LED, true);
	cold_chipio_write(c, 0x18b03c, ae7_chipio_value(c, 0x18b03c, 0) & ~0x80U);
	ae7_routes(c);
	ae7_pll_group(c);
	tl_ctl_param(c, CONTROL_PARAM_ASI, 7);
}

/* converter 0x05's S/PDIF settings, four times (C2B1 spdif_bits) */
static void ae7_spdif_bits(struct ae9_cold *c)
{
	unsigned int i;

	for (i = 0; i < 4; i++) {
		cold_send(c, 0x05, AC_VERB_SET_DIGI_CONVERT_1, 0x15);
		cold_send(c, 0x05, AC_VERB_SET_DIGI_CONVERT_2, 0x12);
		cold_send(c, 0x05, AC_VERB_SET_DIGI_CONVERT_3, 0x80);
	}
}

/*
 * CrystalVoice's settings (DSP module 0x95, kit 11), as Windows' AE-7
 * sends them at every start (its audio service, after the first select:
 * W3 part 2 seq 7462-7942; captures A, B, Bw, W2 and W3 part 1 the same,
 * word for word): the AE-9 tail's (ae9_tail_crystalvoice: Voice
 * Morphing's neutral formants, VoiceFX off, Noise Reduction strength 0,
 * Mic Smart Volume strength 0.74, the mic EQ's centres and widths) but for
 * the mic EQ's eight gains, Windows' saved preset: Preset 1 (-3, -4, 0,
 * 2, 3, -3, 4, 5 dB) in every AE-7 capture, Preset 7 in the AE-9's; then
 * the switches off (ae9_tail_crystalvoice_off: VoiceFX, Noise Reduction,
 * 0x47/0x00, Echo Cancellation, Mic Smart Volume, the mic EQ). The user's
 * switches and presets go after it (creative_ae9_cv_apply(): a control's
 * change, alsactl's restore, a resume).
 */
#define AE7_CV_GAINS	11	/* ae9_tail_crystalvoice[11..18]: the mic EQ gains, 0x95 req 0x14-0x1b */

static const struct ae9_dsp_set ae7_cv_gains[] = {
	{ AE9_MID_FX_IN, 0x14, 0xc0400000 },	/* band 1: -3 dB */
	{ AE9_MID_FX_IN, 0x15, 0xc0800000 },	/* band 2: -4 */
	{ AE9_MID_FX_IN, 0x16, 0 },		/* band 3: 0 */
	{ AE9_MID_FX_IN, 0x17, 0x40000000 },	/* band 4: 2 */
	{ AE9_MID_FX_IN, 0x18, AE9_F_THREE },	/* band 5: 3 */
	{ AE9_MID_FX_IN, 0x19, 0xc0400000 },	/* band 6: -3 */
	{ AE9_MID_FX_IN, 0x1a, 0x40800000 },	/* band 7: 4 */
	{ AE9_MID_FX_IN, 0x1b, 0x40a00000 },	/* band 8: 5 */
};

static void ae7_crystalvoice(struct ae9_tl *tl)
{
	const unsigned int rest = AE7_CV_GAINS + ARRAY_SIZE(ae7_cv_gains);

	tl_set_list(tl, ae9_tail_crystalvoice, AE7_CV_GAINS);
	tl_set_list(tl, ae7_cv_gains, ARRAY_SIZE(ae7_cv_gains));
	tl_set_list(tl, ae9_tail_crystalvoice + rest, ARRAY_SIZE(ae9_tail_crystalvoice) - rest);
	tl_set_list(tl, ae9_tail_crystalvoice_off, ARRAY_SIZE(ae9_tail_crystalvoice_off));
}

/*
 * Windows' AE-7 set-up after the DSP's boot handshake (C2B1 post_dsp to
 * spdif_bits; the output select follows: creative_ae7_select(), from
 * core.c's creative_ae7_post_fw()), as creative_ae9_tail() for the AE-9:
 * the same sender, counts and resend list; kit 11: then CrystalVoice's
 * settings (ae7_crystalvoice(); Windows' audio service sends them after
 * its first select). The DAC isn't clocked from the DSP's start until the
 * ASI pin group: no DAC command goes there, and dac_i2c.expect marks the
 * window (a NACK in it is counted, not retried).
 */
void creative_ae7_tail(struct creative_chip *chip, struct ae9_tail *t)
{
	struct ae9_tl tl = { .c.chip = chip, .t = t };

	*t = (struct ae9_tail){
		.retry = kcalloc(AE9_TAIL_RETRY_MAX, sizeof(*t->retry), GFP_KERNEL),
	};
	chip->dac_i2c.expect = true;
	chip->ae7.remapped = 0;		/* ae7_dsp_loaded()'s walk sets it */
	ae7_dsp_loaded(&tl);
	ae7_asi_pll(&tl);		/* ends the window at its pin group */
	ae7_routing(&tl);
	tail_mic_channel(&tl, false);	/* C2B1 mic_levels: the AE-9's, byte for byte */
	tail_mic_channel(&tl, true);
	ae7_spdif_bits(&tl.c);
	ae7_crystalvoice(&tl);
	creative_dsp_chipio_addr_invalidate();
	t->verbs = tl.c.verbs;
	t->verb_err = tl.c.errs;
}
