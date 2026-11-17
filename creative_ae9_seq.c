// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 3xHelix
//
// Creative Sound Blaster AE-9: Windows' command sequences, written as code.
//
// These were recorded from Creative's Windows driver (QEMU CORB tap, the
// card passed through to a Windows VM) and used to be replayed from
// generated verb tables. Here each step says what it does; the card gets the
// same verbs in the same order, with the same waits (tools/tablesc compares
// both, branch by branch, with the tables in git history).
//
// Here: the start-up's, sent verb by verb (before the firmware download, the
// recovery cycle, the start-up tail). The ones sent as blocks while the card
// runs, and the head unit's and BAR2's, are in creative_ae9_seq_run.c; the
// names both use in creative_ae9_seq.h.
//

#include <linux/build_bug.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <sound/hda_verbs.h>
#include "creative_ae9.h"
#include "ca0132_regs.h"
#include "creative_ae9_seq.h"

/* ------------------------------------------------------------------ */
/* Sending, verb by verb (the start-up's way)				  */
/* ------------------------------------------------------------------ */

/*
 * One verb, waiting for its answer: through the CORB once it runs, by
 * immediate command before. The start-up's pre-download sequence and the
 * recovery cycle send this way, one verb at a time, nothing held.
 */
int creative_ae9_cold_verb(struct creative_chip *chip, u32 verb, u32 *resp)
{
	return chip->corb_buf ? creative_ae9_corb_send_verb_sync(chip, verb, resp) :
				creative_ae9_icw_send_verb(chip, verb, resp);
}

/*
 * A run of verb-by-verb sends: the start-up logs how many it sent and how
 * many failed (a failed verb stops nothing)
 */
struct ae9_cold {
	struct creative_chip *chip;
	unsigned int verbs, errs;
};

static int cold_count(struct ae9_cold *c, int ret)
{
	c->verbs++;
	if (ret < 0)
		c->errs++;
	return ret;
}

static int cold_verb(struct ae9_cold *c, u32 verb, u32 *resp)
{
	return cold_count(c, creative_ae9_cold_verb(c->chip, verb, resp));
}

/*
 * A verb whose answer isn't used (Windows' reads before its read-modify-
 * writes and its widget discovery included: sent, so the card sees what
 * it saw): cold_send() to codec 1, cold_send_c2() to codec 2
 */
static void cold_send_cad(struct ae9_cold *c, u8 cad, u8 nid, u16 verb, u16 parm)
{
	u32 resp = 0;

	cold_verb(c, creative_ae9_verb(cad, nid, verb, parm), &resp);
}

static void cold_send(struct ae9_cold *c, u8 nid, u16 verb, u16 parm)
{
	cold_send_cad(c, AE9_CAD, nid, verb, parm);
}

static void cold_send_c2(struct ae9_cold *c, u8 nid, u16 verb, u16 parm)
{
	cold_send_cad(c, AE9_CAD2, nid, verb, parm);
}

/* A node's parameter read (AC_PAR_*), codec 1 or 2; the answer isn't used */
static void cold_param(struct ae9_cold *c, u8 nid, u8 par)
{
	cold_send(c, nid, AC_VERB_PARAMETERS, par);
}

static void cold_param_c2(struct ae9_cold *c, u8 nid, u8 par)
{
	cold_send_c2(c, nid, AC_VERB_PARAMETERS, par);
}

/*
 * A pin's configuration default, byte 0 first (as mainline's set_pincfg()),
 * codec 1 or 2
 */
static void cold_pin_cfg_cad(struct ae9_cold *c, u8 cad, u8 nid, u32 cfg)
{
	unsigned int i;

	for (i = 0; i < 4; i++, cfg >>= 8)
		cold_send_cad(c, cad, nid, AC_VERB_SET_CONFIG_DEFAULT_BYTES_0 + i, cfg & 0xff);
}

static void cold_pin_cfg(struct ae9_cold *c, u8 nid, u32 cfg)
{
	cold_pin_cfg_cad(c, AE9_CAD, nid, cfg);
}

static void cold_pin_cfg_c2(struct ae9_cold *c, u8 nid, u32 cfg)
{
	cold_pin_cfg_cad(c, AE9_CAD2, nid, cfg);
}

/*
 * 8051 address (ChipIO 0x70d low byte, 0x70e high byte), as mainline's
 * chipio_8051_set_address()
 */
static void cold_8051_addr(struct ae9_cold *c, u16 addr)
{
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_ADDRESS_LOW, addr & 0xff);
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_ADDRESS_HIGH, addr >> 8);
}

/* One byte of the 8051's XRAM (mainline chipio_8051_write_exram()) */
static void cold_xram_write(struct ae9_cold *c, u16 addr, u8 val)
{
	cold_8051_addr(c, addr);
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_DATA_WRITE, val);
}

/*
 * Bytes of the 8051's XRAM from addr on: the address once, then the data
 * (the 8051 steps the address after each byte; mainline's
 * ca0132_init_verbs0 relies on it too)
 */
static void cold_xram_write_n(struct ae9_cold *c, u16 addr, const u8 *data, unsigned int n)
{
	unsigned int i;

	cold_8051_addr(c, addr);
	for (i = 0; i < n; i++)
		cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_DATA_WRITE, data[i]);
}

/* One byte of the 8051's XRAM read: *val = its low byte; < 0: the read failed */
static int cold_xram_read(struct ae9_cold *c, u16 addr, u32 *val)
{
	u32 resp = 0;
	int ret;

	cold_8051_addr(c, addr);
	ret = cold_verb(c, creative_ae9_verb(AE9_CAD, WIDGET_CHIP_CTRL,
					     VENDOR_CHIPIO_8051_DATA_READ, 0), &resp);
	*val = resp & 0xff;
	return ret;
}

/*
 * An 8051 special function register read (mainline's
 * VENDOR_CHIPIO_8051_READ_DIRECT); the answer isn't used
 */
static void cold_8051_sfr_read(struct ae9_cold *c, u8 sfr)
{
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_READ_DIRECT, sfr);
}

/* An 8051 special function register (mainline chipio_8051_write_direct()) */
static void cold_8051_sfr_write(struct ae9_cold *c, u8 sfr, u8 val)
{
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_WRITE_DIRECT | val, sfr);
}

/*
 * An entry of the 8051's C register file, through its special function
 * registers: 0xce = the entry, 0xc9 = the value (the 8051's own accessor,
 * bank 2 0xb60f; read back on the card, uv53)
 */
static void cold_8051_cfile_write(struct ae9_cold *c, u8 idx, u8 val)
{
	cold_8051_sfr_write(c, 0xce, idx);
	cold_8051_sfr_write(c, 0xc9, val);
}

/* One byte of the 8051's internal RAM, through the same address */
static void cold_iram_write(struct ae9_cold *c, u16 addr, u8 val)
{
	cold_8051_addr(c, addr);
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_IRAM_WRITE, val);
}

/*
 * An 8051 PLL/PMU register (mainline chipio_8051_write_pll_pmu(): the
 * address's low byte only)
 */
static void cold_pll_pmu_write(struct ae9_cold *c, u8 reg, u8 val)
{
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_ADDRESS_LOW, reg);
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PLL_PMU_WRITE, val);
}

/* ChipIO memory address, low then high half */
static void cold_chipio_addr(struct ae9_cold *c, u32 addr)
{
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_ADDRESS_LOW, addr & 0xffff);
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_ADDRESS_HIGH, addr >> 16);
}

/*
 * A ChipIO memory read as Windows sends it: address, post read (0x702),
 * status (F01), data (F03). The value isn't used here.
 */
static void cold_chipio_read(struct ae9_cold *c, u32 addr)
{
	cold_chipio_addr(c, addr);
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_HIC_POST_READ, 0);
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_STATUS, 0);
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_HIC_READ_DATA, 0);
}

/* A ChipIO memory write: address, data low half, data high half */
static void cold_chipio_write(struct ae9_cold *c, u32 addr, u32 data)
{
	cold_chipio_addr(c, addr);
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_DATA_LOW, data & 0xffff);
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_DATA_HIGH, data >> 16);
}

/*
 * A ChipIO control parameter in its one-verb form, id < 32 and value < 8
 * (mainline chipio_set_control_param(); larger values take 0x717/0x718)
 */
static void cold_ctl_param(struct ae9_cold *c, u8 id, u8 val)
{
	if (WARN_ON_ONCE(id > 31 || val > 7))
		return;
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_SET, val << 5 | id);
}

/*
 * A ChipIO control parameter in its two-verb form, as Windows sends it
 * (mainline chipio_set_control_param() for id > 31 or value > 7): the
 * ChipIO status read (its answer not checked: Windows' captured sequence
 * doesn't wait on it), the id, the value
 */
static void cold_ctl_param_ex(struct ae9_cold *c, u8 id, u8 val)
{
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_STATUS, 0);
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_EX_ID_SET, id);
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_EX_VALUE_SET, val);
}

/* A connection point's sample rate (mainline chipio_set_conn_rate()) */
static void cold_conn_rate(struct ae9_cold *c, u8 conn, u8 rate)
{
	cold_ctl_param_ex(c, CONTROL_PARAM_CONN_POINT_ID, conn);
	cold_ctl_param_ex(c, CONTROL_PARAM_CONN_POINT_SR, rate);
}

/* A ChipIO control flag (mainline chipio_set_control_flag()) */
static void cold_ctl_flag(struct ae9_cold *c, u8 id, bool on)
{
	if (WARN_ON_ONCE(id > 0x7f))
		return;
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_FLAG_SET, (on ? 0x80 : 0) | id);
}

/* ------------------------------------------------------------------ */
/* Windows' codec recovery cycle (a card that was running)		  */
/* ------------------------------------------------------------------ */

/*
 * The 7 analog ramp status bytes Windows checks before its firmware
 * download, in router XRAM (0x18c6 + 0xd * n, n = 1, 2, 3, 6, 8, 10, 11:
 * Dac1-3, PortA, PortBOut, PortD, PortG). Bit 2 set = "ramp failed": what a
 * card that was running shows. The start-up reads them after its 1.5 s
 * settle (Windows' entries 774-794), the recovery cycle again at its end.
 */
const u16 ae9_ramp_xram[] = { 0x18d3, 0x18e0, 0x18ed, 0x1914, 0x192e, 0x1948, 0x1955 };
static_assert(ARRAY_SIZE(ae9_ramp_xram) == AE9_RAMP_N);

/* A ramp status byte (-1: not read) says "ramp failed" */
bool ae9_ramp_failed(const int ramp[AE9_RAMP_N])
{
	unsigned int i;

	for (i = 0; i < AE9_RAMP_N; i++)
		if (ramp[i] >= 0 && (ramp[i] & 4))
			return true;
	return false;
}

/* The 7 ramp status bytes (-1: the read failed) */
static void cold_ramp_read(struct ae9_cold *c, int ramp[AE9_RAMP_N])
{
	unsigned int i;
	u32 val;

	for (i = 0; i < AE9_RAMP_N; i++)
		ramp[i] = cold_xram_read(c, ae9_ramp_xram[i], &val) < 0 ? -1 : (int)val;
}

/*
 * One pass of Windows' codec recovery cycle (CtxHda FUN_0001e0c0), sent at
 * start-up before the download prefix (Windows' entry 795) when a ramp
 * status byte reads "ramp failed" (creative_ae9_predownload(): up to 3
 * passes). The verbs are the 108 the warm Windows start sent there and the
 * cold one didn't (warm capture ae9_corb_capture_raw.log, tap seq
 * 1538-1752; Windows' ramp bytes read 04 04 00 00 00 04 00 before it, all
 * 00 after), with Windows' waits:
 * pins off, the AFG to D3cold (2 s), PLL/PMU register 0x0f bits 7 and 3
 * off then on (~100 ms each), the 8051 runs its own init again and is
 * polled until done, two port settings, the AFG back to D0 (2 s), pins
 * back with jack sense on, the ramp bytes read again.
 *
 * Several steps are Windows' read-modify-writes: it reads the pin controls,
 * PMU register 0x0f and chip register 0x18b0a4 and writes them back
 * changed. The reads are sent (their answers aren't used) and the writes
 * carry what Windows wrote in the capture.
 *
 * Every verb waits for its answer and none stops the pass. Fills ramp[]
 * with the bytes read at the end (-1: the read failed); returns true when
 * one still reads "ramp failed".
 */
bool creative_ae9_recovery_pass(struct creative_chip *chip, int ramp[AE9_RAMP_N])
{
	struct ae9_cold c = { .chip = chip };
	unsigned int polls;
	u32 val;
	int ret;

	/*
	 * pins off (Windows read 0x24 0x24 0x20 0x00 0xc0 0xc0 here): node
	 * 0x11 keeps its input, its vref off; 0x0b's EAPD off as well
	 */
	cold_send(&c, 0x12, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x11, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN);
	cold_send(&c, 0x12, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x13, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x13, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x0b, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x0b, AC_VERB_SET_EAPD_BTLENABLE, 0);
	cold_send(&c, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x0f, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x10, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);

	/* the audio function group to D3cold, 2 s */
	cold_send(&c, AE9_NID_AFG, AC_VERB_GET_POWER_STATE, 0);
	cold_send(&c, AE9_NID_AFG, AC_VERB_SET_POWER_STATE, AE9_PWRST_D3COLD);
	msleep(2000);

	/*
	 * PLL/PMU register 0x0f bits 7 and 3 off, then on again, ~100 ms
	 * each (meaning unknown; Windows read 0xff, wrote 0x77, then 0xff).
	 * Its read: the address, then PLL_PMU_READ with payload 1
	 */
	cold_send(&c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_8051_ADDRESS_LOW, 0x0f);
	cold_send(&c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PLL_PMU_READ, 0x01);
	cold_pll_pmu_write(&c, 0x0f, 0x77);
	msleep(110);
	cold_pll_pmu_write(&c, 0x0f, 0xff);
	msleep(110);

	/*
	 * The 8051 runs its own init routine again: busy flag XRAM 0xb6 = 1,
	 * its event-6 handler (0x1f07/0x1f08, normally a RET stub) pointed at
	 * 0xf972 (bank 1 0x9fd1, the firmware's init, which clears 0xb6 when
	 * done), then event 6 raised (internal RAM 0x7d = 0x40). Windows
	 * polls 0xb6 until it reads 0: up to 5 more reads, 100 ms apart.
	 */
	cold_xram_write(&c, 0x00b6, 0x01);
	cold_xram_write(&c, 0x1f07, 0xf9);
	cold_xram_write(&c, 0x1f08, 0x72);
	cold_iram_write(&c, 0x007d, 0x40);
	ret = cold_xram_read(&c, 0x00b6, &val);
	for (polls = 0; ret == 0 && val && polls < 5; polls++) {
		msleep(100);
		ret = cold_xram_read(&c, 0x00b6, &val);
	}

	/*
	 * chip register 0x18b0a4 bit 1 set (Windows read 0xc0, wrote 0xc2;
	 * it sets the same bit at codec start-up)
	 */
	cold_chipio_read(&c, 0x18b0a4);
	cold_chipio_write(&c, 0x18b0a4, 0x000000c2);

	/*
	 * ports A and D: 160 ohm gain 6, 10 kohm load on (Windows' codec
	 * set-up values)
	 */
	cold_ctl_param(&c, CONTROL_PARAM_PORTA_160OHM_GAIN, 6);
	cold_ctl_param(&c, CONTROL_PARAM_PORTD_160OHM_GAIN, 6);
	cold_ctl_flag(&c, CONTROL_FLAG_PORT_A_10KOHM_LOAD, true);
	cold_ctl_flag(&c, CONTROL_FLAG_PORT_D_10KOHM_LOAD, true);

	/* XRAM 0x1920 = 0 (meaning unknown, sent as captured) */
	cold_xram_write(&c, 0x1920, 0x00);

	/*
	 * the audio function group back to D0, 2 s (Windows read 0x244:
	 * D3cold reached)
	 */
	cold_send(&c, AE9_NID_AFG, AC_VERB_GET_POWER_STATE, 0);
	cold_send(&c, AE9_NID_AFG, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
	msleep(2000);

	/*
	 * pins back, each with jack sense on (unsolicited responses, tags
	 * 1-3), in two steps as Windows: inputs 0x11/0x12 with vref 80 %,
	 * node 0x13 input, 0x0b off, outputs 0x0f/0x10 on with their
	 * headphone amps
	 */
	cold_send(&c, 0x12, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 1);
	cold_send(&c, 0x12, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x11, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN);
	cold_send(&c, 0x12, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x12, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x11, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL,
		  AC_PINCTL_IN_EN | AC_PINCTL_VREF_80);
	cold_send(&c, 0x12, AC_VERB_SET_PIN_WIDGET_CONTROL,
		  AC_PINCTL_IN_EN | AC_PINCTL_VREF_80);

	cold_send(&c, 0x13, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 2);
	cold_send(&c, 0x13, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x13, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x13, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x13, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN);

	cold_send(&c, 0x0b, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 3);
	cold_send(&c, 0x0b, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x0b, AC_VERB_SET_EAPD_BTLENABLE, 0);
	cold_send(&c, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x0b, AC_VERB_GET_PIN_SENSE, 0);
	cold_send(&c, 0x0b, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x0b, AC_VERB_SET_EAPD_BTLENABLE, 0);
	cold_send(&c, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);

	cold_send(&c, 0x0f, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 3);
	cold_send(&c, 0x0f, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x0f, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL,
		  AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN);

	cold_send(&c, 0x10, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 3);
	cold_send(&c, 0x10, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x10, AC_VERB_GET_PIN_SENSE, 0);
	cold_send(&c, 0x10, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(&c, 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL,
		  AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN);

	/* the 7 ramp status bytes again */
	cold_ramp_read(&c, ramp);
	return ae9_ramp_failed(ramp);
}

/* ------------------------------------------------------------------ */
/* Start-up: Windows' sequence before its firmware download		  */
/* ------------------------------------------------------------------ */

/*
 * Windows' board-D0 GPIO step (CtxHda 0x364b0, 0x35de4, 0x35e58), which it
 * runs once, between codec 2's set-up and codec 1's first verb (sb-ae9's
 * capture ae9-20260903-210512, ops lines 1264-1288): pins 4-7 to GPIO and
 * all pins enabled, pin 5 = 1 (head-unit power), pin 0 = 0, ~16 ms, pin 0
 * = 1 ("AntiPop"), both DAC devices muted, pin 2 = 0, DAC 48/1d = 0x40,
 * pin 3 = 1. The post-boot replay is Windows' post-DSP block, which has no
 * pin 0 because Windows set it here. Without it, on a card behind an
 * ASM1083 rev 03 bridge neither DAC device answered (every read 0xff,
 * every DAC command lost, analog outputs silent) until pin 0 was pulsed
 * (2026-11-10). Then whether the DAC answers.
 */
static void creative_ae9_board_d0(struct creative_chip *chip)
{
	static const u8 mute[][3] = { { 0x48, 0x07, 0x81 }, { 0x49, 0x0a, 0x07 } };
	static const u8 front[][3] = { { 0x48, 0x1d, 0x40 } };
	u32 raw[2];
	u8 id;

	creative_ext_writel(chip, 0x100, creative_ext_readl(chip, 0x100) & ~0x0fU);
	creative_ext_writeb(chip, 0x304, 0xff);
	creative_ext_writew(chip, 0x320, 0x0105);	/* pin 5 = 1: head-unit power */
	creative_ext_writew(chip, 0x320, 0x0000);	/* pin 0 = 0 */
	msleep(16);	/* Windows' 1 ms wait comes out at ~16 ms (its timer tick) */
	creative_ext_writew(chip, 0x320, 0x0100);	/* pin 0 = 1 */
	creative_dsp_ca0113_list_quiet(chip, mute, ARRAY_SIZE(mute));
	creative_ext_writew(chip, 0x320, 0x0002);	/* pin 2 = 0 */
	creative_dsp_ca0113_list_quiet(chip, front, ARRAY_SIZE(front));
	creative_ext_writew(chip, 0x320, 0x0103);	/* pin 3 = 1 */
	id = creative_dsp_ca0113_read(chip, 0x48, 0x40, raw);	/* ES9038 chip ID, bits 7:2 */
	if (id >> 2 == 0x1c)
		dev_info(chip->dev, "board D0: GPIO pin 0 pulsed, the DAC answers (48/40 = 0x%02x)\n",
			 id);
	else
		dev_warn(chip->dev, "board D0: GPIO pin 0 pulsed, but the DAC does not answer (48/40 = 0x%02x): the analog outputs will be silent\n",
			 id);
}

/*
 * Codec 2's AFG asleep: our unload (and a reboot) leaves both AFGs in
 * D3hot. The start-up wakes codec 1's (ae9_codec1_wake()) but nothing woke
 * codec 2's: Windows' sequence only reads its power state (in codec 2's
 * set-up), and its function resets don't wake it. So after a driver reload
 * codec 2 stayed in D3 (F05 = 0x233), its widgets with it (actual D3
 * though set to D0), and the rear RCA input (ADC 0x08) never streamed:
 * "Input/output error" until a power-off. Waking it to D0 brought it back
 * at once (uv48, 2026-11-12). Optical in and out are on codec 2 as well.
 * Ours, not Windows': before codec 2's function resets.
 */
static void ae9_codec2_wake(struct creative_chip *chip)
{
	u32 get_power = creative_ae9_verb(AE9_CAD2, AE9_NID_AFG, AC_VERB_GET_POWER_STATE, 0);
	u32 resp = 0;

	if (creative_ae9_cold_verb(chip, get_power, &resp) < 0 || !(resp & 0xf))
		return;
	creative_ae9_cold_verb(chip, creative_ae9_verb(AE9_CAD2, AE9_NID_AFG,
						       AC_VERB_SET_POWER_STATE, AC_PWRST_D0),
			       &resp);
	msleep(2000);	/* as codec 1's wake (Windows' 2 s) */
	creative_ae9_cold_verb(chip, get_power, &resp);
	dev_info(chip->dev, "warm start: codec 2 was asleep, woken to D0 (now 0x%x) + 2 s\n", resp);
}

/*
 * Windows' first start-up check (CtxHda FUN_0001c7dc; notes 42/42v): codec
 * 1's AFG power state, read before its two function resets. A card that
 * was running is asleep (our shutdown left it in D3): Windows wakes it and
 * gives it 2 s before the resets, every time; so do we. f05 = the state
 * read (~0: the read failed: nothing done).
 */
static void ae9_codec1_wake(struct creative_chip *chip, u32 f05)
{
	u32 resp = 0;

	if (f05 == ~0U || !(f05 & 0xf))
		return;
	creative_ae9_cold_verb(chip, creative_ae9_verb(AE9_CAD, AE9_NID_AFG,
						       AC_VERB_SET_POWER_STATE, AC_PWRST_D0),
			       &resp);
	msleep(2000);
	creative_ae9_cold_verb(chip, creative_ae9_verb(AE9_CAD, AE9_NID_AFG,
						       AC_VERB_GET_POWER_STATE, 0), &resp);
	dev_info(chip->dev, "warm start: codec 1 was in D%u, woken (now 0x%x) + 2 s\n",
		 f05 & 0x7, resp);
}

/*
 * The DSP's init (DSPIO DSP_INIT). Windows' dsp_reset (CtxHda FUN_0001b024)
 * sends it again while the DSP answers 1 (busy); every capture answered 0
 * at once. Same here, capped at ~1 s, and logged if it was busy. Counted
 * as one verb.
 */
static void ae9_dsp_init(struct ae9_cold *c)
{
	u32 verb = creative_ae9_verb(AE9_CAD, WIDGET_DSP_CTRL, VENDOR_DSPIO_DSP_INIT, 0);
	u32 resp = 0;
	int ret;

	ret = creative_ae9_cold_verb(c->chip, verb, &resp);
	if (ret == 0 && resp == 1) {
		unsigned int n = 0;

		while (ret == 0 && resp == 1 && n++ < 50) {
			usleep_range(20000, 21000);
			ret = creative_ae9_cold_verb(c->chip, verb, &resp);
		}
		dev_info(c->chip->dev, "DSP_INIT at start-up: busy, %u more tries -> %u\n",
			 n, resp);
	}
	cold_count(c, ret);
}

/*
 * Windows' two start-up checks, as read (memory windows-bringup-branches,
 * notes 42/42v): codec 1's power state before its resets (ae9_codec1_wake())
 * and the 7 ramp status bytes before the download (bit 2 set in any:
 * Windows' recovery cycle, creative_ae9_recovery_pass())
 */
static void ae9_boot_checks_log(struct creative_chip *chip, u32 f05, const int ramp[AE9_RAMP_N])
{
	char buf[AE9_RAMP_N * 4 + 1];
	unsigned int i, n = 0;
	bool recover = false, unread = false;

	for (i = 0; i < AE9_RAMP_N; i++) {
		int v = ramp[i];

		if (v < 0)
			unread = true;
		else if (v & 4)
			recover = true;
		n += scnprintf(buf + n, sizeof(buf) - n, v < 0 ? " ??" : " %02x", v);
	}
	dev_info(chip->dev, "Windows' boot checks: codec 1 power 0x%x (%s), router XRAM%s (%s)\n",
		 f05, f05 == ~0U ? "unread" :
		 (f05 & 0xf) ? "not D0: woken + 2 s, as Windows" : "D0",
		 buf, recover ? "ramp failed: recovery cycle, as Windows" :
		      unread ? "some unread" : "no recovery");
}

/*
 * Windows' recovery cycle when a ramp byte reads "ramp failed" (a card
 * that was running: every warm re-start, never after a power-off): up to 3
 * passes, each checked again. Windows does it every time; we skipped it,
 * and each warm re-start got riskier until the DSP broke (2026-11-09: 0/34
 * bad on the 1st warm re-start, 50 % from the 6th;
 * .tmp/unicorn/reprobe/SUMMARY.md). In since 0.12.4 (the warm_reset option
 * until 0.12.12).
 */
static void ae9_recovery(struct creative_chip *chip, u32 f05, int ramp[AE9_RAMP_N])
{
	bool failed = ae9_ramp_failed(ramp);
	unsigned int pass;

	ae9_boot_checks_log(chip, f05, ramp);
	for (pass = 1; failed && pass <= 3; pass++) {
		failed = creative_ae9_recovery_pass(chip, ramp);
		dev_info(chip->dev, "warm start: recovery cycle %u: ramp status %02x %02x %02x %02x %02x %02x %02x (%s)\n",
			 pass, ramp[0] & 0xff, ramp[1] & 0xff, ramp[2] & 0xff, ramp[3] & 0xff,
			 ramp[4] & 0xff, ramp[5] & 0xff, ramp[6] & 0xff,
			 failed ? "still failed" : "clear");
	}
	creative_dsp_chipio_addr_invalidate();
}

/*
 * Both codecs found (Windows' HD-Audio bus driver): each codec's vendor,
 * revision and node count, its audio function group's subsystem, type and
 * subsystem ID; codec 2's identity again and its function group's power
 * states. Entries 0-14 (tap seq 0-28); the answers aren't used.
 */
static void pre_codecs_found(struct ae9_cold *c)
{
	/* codec 1 */
	cold_param(c, AC_NODE_ROOT, AC_PAR_VENDOR_ID);
	cold_param(c, AC_NODE_ROOT, AC_PAR_REV_ID);
	cold_param(c, AC_NODE_ROOT, AC_PAR_NODE_COUNT);
	cold_param(c, AE9_NID_AFG, AC_PAR_SUBSYSTEM_ID);
	cold_param(c, AE9_NID_AFG, AC_PAR_FUNCTION_TYPE);
	cold_send(c, AE9_NID_AFG, AC_VERB_GET_SUBSYSTEM_ID, 0);

	/* codec 2 */
	cold_param_c2(c, AC_NODE_ROOT, AC_PAR_VENDOR_ID);
	cold_param_c2(c, AC_NODE_ROOT, AC_PAR_REV_ID);
	cold_param_c2(c, AC_NODE_ROOT, AC_PAR_NODE_COUNT);
	cold_param_c2(c, AE9_NID_AFG, AC_PAR_SUBSYSTEM_ID);
	cold_param_c2(c, AE9_NID_AFG, AC_PAR_FUNCTION_TYPE);
	cold_send_c2(c, AE9_NID_AFG, AC_VERB_GET_SUBSYSTEM_ID, 0);

	/* codec 2 again, its function group's power states */
	cold_param_c2(c, AC_NODE_ROOT, AC_PAR_VENDOR_ID);
	cold_param_c2(c, AC_NODE_ROOT, AC_PAR_REV_ID);
	cold_param_c2(c, AE9_NID_AFG, AC_PAR_POWER_STATE);
}

/*
 * Codec 2's function group reset, twice: its own driver (CtxHdb.sys) starting.
 * Entries 15-16. ae9_codec2_wake() goes before it.
 */
static void pre_codec2_reset(struct ae9_cold *c)
{
	cold_send_c2(c, AE9_NID_AFG, AC_VERB_SET_CODEC_RESET, 0);
	cold_send_c2(c, AE9_NID_AFG, AC_VERB_SET_CODEC_RESET, 0);
}

/*
 * Both codec drivers read their codec's widgets at the same time (CtxHdb.sys
 * codec 2, CtxHda.sys codec 1), and the capture keeps the order their verbs
 * reached the card, so their lines alternate here (the _c2 helpers are codec
 * 2): codec 2's function group and every node's widget capabilities, then
 * converter by converter (each unused one to D3); codec 1's identity, every
 * node's widget capabilities, the ChipIO and DSPIO nodes, four 8051 special
 * function registers, and PLL/PMU register 0x44. Entries 17-120 (tap seq
 * 34-241); the answers aren't used.
 */
static void pre_probe_both(struct ae9_cold *c)
{
	/* codec 1: its identity again; codec 2: its function group's capabilities */
	cold_param(c, AC_NODE_ROOT, AC_PAR_VENDOR_ID);
	cold_param_c2(c, AE9_NID_AFG, AC_PAR_SUBSYSTEM_ID);
	cold_send(c, AE9_NID_AFG, AC_VERB_GET_SUBSYSTEM_ID, 0);
	cold_param_c2(c, AE9_NID_AFG, AC_PAR_AUDIO_FG_CAP);
	cold_param(c, AC_NODE_ROOT, AC_PAR_REV_ID);
	cold_param_c2(c, AE9_NID_AFG, AC_PAR_NODE_COUNT);
	cold_param(c, AE9_NID_AFG, AC_PAR_NODE_COUNT);

	/* codec 2: the function group's default widget capabilities */
	cold_param_c2(c, AE9_NID_AFG, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, AE9_NID_AFG, AC_PAR_PCM);
	cold_param_c2(c, AE9_NID_AFG, AC_PAR_STREAM);
	cold_param_c2(c, AE9_NID_AFG, AC_PAR_PIN_CAP);
	cold_param_c2(c, AE9_NID_AFG, AC_PAR_AMP_IN_CAP);
	cold_param_c2(c, AE9_NID_AFG, AC_PAR_CONNLIST_LEN);
	cold_param_c2(c, AE9_NID_AFG, AC_PAR_POWER_STATE);
	cold_param_c2(c, AE9_NID_AFG, AC_PAR_PROC_CAP);
	cold_param_c2(c, AE9_NID_AFG, AC_PAR_GPIO_CAP);
	cold_param_c2(c, AE9_NID_AFG, AC_PAR_AMP_OUT_CAP);
	cold_param_c2(c, AE9_NID_AFG, AC_PAR_VOL_KNB_CAP);

	/* codec 1: node 0x02's widget capabilities; codec 2: every node's */
	cold_param(c, 0x02, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x02, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x03, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x04, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x05, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x06, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x07, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x08, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x09, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x0a, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x0b, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x0c, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x0d, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x0e, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x0f, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x10, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x11, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x12, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x13, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x14, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x15, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x16, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x17, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x18, AC_PAR_AUDIO_WIDGET_CAP);

	/*
	 * codec 1: nodes 0x03-0x14's widget capabilities, alternating with codec 2's
	 * converters 0x02 and 0x03 (formats and power states supported, format,
	 * power, stream, SDI select, unsolicited response; then D3)
	 */
	cold_param(c, 0x03, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x02, AC_PAR_PCM);
	cold_param(c, 0x04, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x02, AC_PAR_STREAM);
	cold_param(c, 0x05, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x02, AC_PAR_POWER_STATE);
	cold_param(c, 0x06, AC_PAR_AUDIO_WIDGET_CAP);
	cold_send_c2(c, 0x02, AC_VERB_GET_STREAM_FORMAT, 0);
	cold_param(c, 0x07, AC_PAR_AUDIO_WIDGET_CAP);
	cold_send_c2(c, 0x02, AC_VERB_GET_POWER_STATE, 0);
	cold_param(c, 0x08, AC_PAR_AUDIO_WIDGET_CAP);
	cold_send_c2(c, 0x02, AC_VERB_GET_CONV, 0);
	cold_param(c, 0x09, AC_PAR_AUDIO_WIDGET_CAP);
	cold_send_c2(c, 0x02, AC_VERB_GET_SDI_SELECT, 0);
	cold_param(c, 0x0a, AC_PAR_AUDIO_WIDGET_CAP);
	cold_send_c2(c, 0x02, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	cold_param(c, 0x0b, AC_PAR_AUDIO_WIDGET_CAP);
	cold_send_c2(c, 0x02, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	cold_param(c, 0x0c, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x03, AC_PAR_PCM);
	cold_param(c, 0x0d, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x03, AC_PAR_STREAM);
	cold_param(c, 0x0e, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x03, AC_PAR_POWER_STATE);
	cold_param(c, 0x0f, AC_PAR_AUDIO_WIDGET_CAP);
	cold_send_c2(c, 0x03, AC_VERB_GET_STREAM_FORMAT, 0);
	cold_param(c, 0x10, AC_PAR_AUDIO_WIDGET_CAP);
	cold_send_c2(c, 0x03, AC_VERB_GET_POWER_STATE, 0);
	cold_param(c, 0x11, AC_PAR_AUDIO_WIDGET_CAP);
	cold_send_c2(c, 0x03, AC_VERB_GET_CONV, 0);
	cold_param(c, 0x12, AC_PAR_AUDIO_WIDGET_CAP);
	cold_send_c2(c, 0x03, AC_VERB_GET_SDI_SELECT, 0);
	cold_param(c, 0x13, AC_PAR_AUDIO_WIDGET_CAP);
	cold_send_c2(c, 0x03, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	cold_param(c, 0x14, AC_PAR_AUDIO_WIDGET_CAP);
	cold_send_c2(c, 0x03, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);

	/*
	 * codec 1: the ChipIO (0x15) and DSPIO (0x16) nodes (widget capabilities,
	 * vendor ID, power state) and nodes 0x17-0x18; codec 2: converter 0x04
	 */
	cold_param(c, WIDGET_CHIP_CTRL, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param_c2(c, 0x04, AC_PAR_PCM);
	cold_param(c, WIDGET_CHIP_CTRL, AC_PAR_VENDOR_ID);
	cold_param_c2(c, 0x04, AC_PAR_STREAM);
	cold_send(c, WIDGET_CHIP_CTRL, AC_VERB_GET_POWER_STATE, 0);
	cold_param_c2(c, 0x04, AC_PAR_POWER_STATE);
	cold_param(c, WIDGET_DSP_CTRL, AC_PAR_AUDIO_WIDGET_CAP);
	cold_send_c2(c, 0x04, AC_VERB_GET_STREAM_FORMAT, 0);
	cold_param(c, WIDGET_DSP_CTRL, AC_PAR_VENDOR_ID);
	cold_send_c2(c, 0x04, AC_VERB_GET_POWER_STATE, 0);
	cold_send(c, WIDGET_DSP_CTRL, AC_VERB_GET_POWER_STATE, 0);
	cold_send_c2(c, 0x04, AC_VERB_GET_CONV, 0);
	cold_param(c, 0x17, AC_PAR_AUDIO_WIDGET_CAP);
	cold_send_c2(c, 0x04, AC_VERB_GET_SDI_SELECT, 0);
	cold_param(c, 0x18, AC_PAR_AUDIO_WIDGET_CAP);
	cold_send_c2(c, 0x04, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);

	/*
	 * codec 1: 8051 special function registers 0xfc-0xff read (meaning
	 * unknown); codec 2: converter 0x04 to D3, converter 0x05's first reads
	 */
	cold_8051_sfr_read(c, 0xfc);
	cold_send_c2(c, 0x04, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	cold_8051_sfr_read(c, 0xfd);
	cold_param_c2(c, 0x05, AC_PAR_PCM);
	cold_8051_sfr_read(c, 0xfe);
	cold_param_c2(c, 0x05, AC_PAR_STREAM);
	cold_8051_sfr_read(c, 0xff);
	cold_param_c2(c, 0x05, AC_PAR_POWER_STATE);

	/* codec 1: PLL/PMU register 0x44 = 0xc8: chip pin 4 works as MCLK */
	cold_pll_pmu_write(c, 0x44, 0xc8);
}

/*
 * Codec 2's widgets one by one (CtxHdb.sys): each node's capabilities and
 * state read, each one it doesn't use set to D3; pin 0x0b's EAPD on and the
 * pin off, pins 0x0f and 0x10 off, the input amps of nodes 0x07 and 0x12 at
 * gain 0 (index 0, both channels). The S/PDIF converters and pins and the
 * head unit's rear RCA input come back to D0 in pre_codec2_setup(). Entries
 * 121-308 (tap seq 242-617); the answers aren't used.
 */
static void pre_codec2_widgets(struct ae9_cold *c)
{
	/* converter 0x05 (S/PDIF out), continued */
	cold_send_c2(c, 0x05, AC_VERB_GET_STREAM_FORMAT, 0);
	cold_send_c2(c, 0x05, AC_VERB_GET_DIGI_CONVERT_1, 0);
	cold_send_c2(c, 0x05, AC_VERB_GET_POWER_STATE, 0);
	cold_send_c2(c, 0x05, AC_VERB_GET_CONV, 0);
	cold_send_c2(c, 0x05, AC_VERB_GET_SDI_SELECT, 0);
	cold_send_c2(c, 0x05, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	cold_send_c2(c, 0x05, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);

	/* converter 0x06 */
	cold_param_c2(c, 0x06, AC_PAR_PCM);
	cold_param_c2(c, 0x06, AC_PAR_STREAM);
	cold_param_c2(c, 0x06, AC_PAR_POWER_STATE);
	cold_send_c2(c, 0x06, AC_VERB_GET_STREAM_FORMAT, 0);
	cold_send_c2(c, 0x06, AC_VERB_GET_DIGI_CONVERT_1, 0);
	cold_send_c2(c, 0x06, AC_VERB_GET_POWER_STATE, 0);
	cold_send_c2(c, 0x06, AC_VERB_GET_CONV, 0);
	cold_send_c2(c, 0x06, AC_VERB_GET_SDI_SELECT, 0);
	cold_send_c2(c, 0x06, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	cold_send_c2(c, 0x06, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);

	/* node 0x07 (with an input amp) */
	cold_param_c2(c, 0x07, AC_PAR_PCM);
	cold_param_c2(c, 0x07, AC_PAR_STREAM);
	cold_param_c2(c, 0x07, AC_PAR_AMP_IN_CAP);
	cold_param_c2(c, 0x07, AC_PAR_CONNLIST_LEN);
	cold_param_c2(c, 0x07, AC_PAR_POWER_STATE);
	cold_param_c2(c, 0x07, AC_PAR_CONNLIST_LEN);
	cold_send_c2(c, 0x07, AC_VERB_GET_STREAM_FORMAT, 0);
	cold_send_c2(c, 0x07, AC_VERB_GET_POWER_STATE, 0);
	cold_send_c2(c, 0x07, AC_VERB_GET_CONV, 0);
	cold_send_c2(c, 0x07, AC_VERB_GET_SDI_SELECT, 0);
	cold_send_c2(c, 0x07, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	cold_send_c2(c, 0x07, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x07, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x07, AC_VERB_GET_AMP_GAIN_MUTE, AC_AMP_GET_INPUT | AC_AMP_GET_LEFT);
	cold_send_c2(c, 0x07, AC_VERB_GET_AMP_GAIN_MUTE, AC_AMP_GET_INPUT | AC_AMP_GET_RIGHT);
	cold_send_c2(c, 0x07, AC_VERB_SET_AMP_GAIN_MUTE,
		     AC_AMP_SET_INPUT | AC_AMP_SET_LEFT | AC_AMP_SET_RIGHT);
	cold_send_c2(c, 0x07, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);

	/* converters 0x08 (the head unit's rear RCA input), 0x09 (S/PDIF in), 0x0a */
	cold_param_c2(c, 0x08, AC_PAR_PCM);
	cold_param_c2(c, 0x08, AC_PAR_STREAM);
	cold_param_c2(c, 0x08, AC_PAR_CONNLIST_LEN);
	cold_param_c2(c, 0x08, AC_PAR_POWER_STATE);
	cold_param_c2(c, 0x08, AC_PAR_CONNLIST_LEN);
	cold_send_c2(c, 0x08, AC_VERB_GET_STREAM_FORMAT, 0);
	cold_send_c2(c, 0x08, AC_VERB_GET_POWER_STATE, 0);
	cold_send_c2(c, 0x08, AC_VERB_GET_CONV, 0);
	cold_send_c2(c, 0x08, AC_VERB_GET_SDI_SELECT, 0);
	cold_send_c2(c, 0x08, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	cold_send_c2(c, 0x08, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x08, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x08, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	cold_param_c2(c, 0x09, AC_PAR_PCM);
	cold_param_c2(c, 0x09, AC_PAR_STREAM);
	cold_param_c2(c, 0x09, AC_PAR_CONNLIST_LEN);
	cold_param_c2(c, 0x09, AC_PAR_POWER_STATE);
	cold_param_c2(c, 0x09, AC_PAR_CONNLIST_LEN);
	cold_send_c2(c, 0x09, AC_VERB_GET_STREAM_FORMAT, 0);
	cold_send_c2(c, 0x09, AC_VERB_GET_DIGI_CONVERT_1, 0);
	cold_send_c2(c, 0x09, AC_VERB_GET_POWER_STATE, 0);
	cold_send_c2(c, 0x09, AC_VERB_GET_CONV, 0);
	cold_send_c2(c, 0x09, AC_VERB_GET_SDI_SELECT, 0);
	cold_send_c2(c, 0x09, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	cold_send_c2(c, 0x09, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x09, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x09, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	cold_param_c2(c, 0x0a, AC_PAR_PCM);
	cold_param_c2(c, 0x0a, AC_PAR_STREAM);
	cold_param_c2(c, 0x0a, AC_PAR_CONNLIST_LEN);
	cold_param_c2(c, 0x0a, AC_PAR_POWER_STATE);
	cold_param_c2(c, 0x0a, AC_PAR_CONNLIST_LEN);
	cold_send_c2(c, 0x0a, AC_VERB_GET_STREAM_FORMAT, 0);
	cold_send_c2(c, 0x0a, AC_VERB_GET_DIGI_CONVERT_1, 0);
	cold_send_c2(c, 0x0a, AC_VERB_GET_POWER_STATE, 0);
	cold_send_c2(c, 0x0a, AC_VERB_GET_CONV, 0);
	cold_send_c2(c, 0x0a, AC_VERB_GET_SDI_SELECT, 0);
	cold_send_c2(c, 0x0a, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	cold_send_c2(c, 0x0a, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x0a, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x0a, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);

	/* pin 0x0b */
	cold_param_c2(c, 0x0b, AC_PAR_PIN_CAP);
	cold_param_c2(c, 0x0b, AC_PAR_CONNLIST_LEN);
	cold_param_c2(c, 0x0b, AC_PAR_POWER_STATE);
	cold_param_c2(c, 0x0b, AC_PAR_CONNLIST_LEN);
	cold_send_c2(c, 0x0b, AC_VERB_GET_POWER_STATE, 0);
	cold_send_c2(c, 0x0b, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send_c2(c, 0x0b, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	cold_send_c2(c, 0x0b, AC_VERB_GET_EAPD_BTLENABLE, 0);
	cold_send_c2(c, 0x0b, AC_VERB_GET_CONFIG_DEFAULT, 0);
	cold_send_c2(c, 0x0b, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x0b, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x0b, AC_VERB_SET_EAPD_BTLENABLE, AC_EAPDBTL_EAPD);
	cold_send_c2(c, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);

	/* pins 0x0c (optical out), 0x0d, 0x0e (optical in) */
	cold_param_c2(c, 0x0c, AC_PAR_PIN_CAP);
	cold_param_c2(c, 0x0c, AC_PAR_CONNLIST_LEN);
	cold_param_c2(c, 0x0c, AC_PAR_POWER_STATE);
	cold_param_c2(c, 0x0c, AC_PAR_CONNLIST_LEN);
	cold_send_c2(c, 0x0c, AC_VERB_GET_POWER_STATE, 0);
	cold_send_c2(c, 0x0c, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send_c2(c, 0x0c, AC_VERB_GET_CONFIG_DEFAULT, 0);
	cold_send_c2(c, 0x0c, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x0c, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x0c, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	cold_param_c2(c, 0x0d, AC_PAR_PIN_CAP);
	cold_param_c2(c, 0x0d, AC_PAR_CONNLIST_LEN);
	cold_param_c2(c, 0x0d, AC_PAR_POWER_STATE);
	cold_param_c2(c, 0x0d, AC_PAR_CONNLIST_LEN);
	cold_send_c2(c, 0x0d, AC_VERB_GET_POWER_STATE, 0);
	cold_send_c2(c, 0x0d, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send_c2(c, 0x0d, AC_VERB_GET_CONFIG_DEFAULT, 0);
	cold_send_c2(c, 0x0d, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x0d, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x0d, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	cold_param_c2(c, 0x0e, AC_PAR_PIN_CAP);
	cold_param_c2(c, 0x0e, AC_PAR_POWER_STATE);
	cold_send_c2(c, 0x0e, AC_VERB_GET_POWER_STATE, 0);
	cold_send_c2(c, 0x0e, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send_c2(c, 0x0e, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	cold_send_c2(c, 0x0e, AC_VERB_GET_CONFIG_DEFAULT, 0);
	cold_send_c2(c, 0x0e, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);

	/* pins 0x0f and 0x10 */
	cold_param_c2(c, 0x0f, AC_PAR_PIN_CAP);
	cold_param_c2(c, 0x0f, AC_PAR_CONNLIST_LEN);
	cold_param_c2(c, 0x0f, AC_PAR_POWER_STATE);
	cold_param_c2(c, 0x0f, AC_PAR_CONNLIST_LEN);
	cold_send_c2(c, 0x0f, AC_VERB_GET_POWER_STATE, 0);
	cold_send_c2(c, 0x0f, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send_c2(c, 0x0f, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	cold_send_c2(c, 0x0f, AC_VERB_GET_CONFIG_DEFAULT, 0);
	cold_send_c2(c, 0x0f, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x0f, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send_c2(c, 0x0f, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	cold_param_c2(c, 0x10, AC_PAR_PIN_CAP);
	cold_param_c2(c, 0x10, AC_PAR_CONNLIST_LEN);
	cold_param_c2(c, 0x10, AC_PAR_POWER_STATE);
	cold_param_c2(c, 0x10, AC_PAR_CONNLIST_LEN);
	cold_send_c2(c, 0x10, AC_VERB_GET_POWER_STATE, 0);
	cold_send_c2(c, 0x10, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send_c2(c, 0x10, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	cold_send_c2(c, 0x10, AC_VERB_GET_CONFIG_DEFAULT, 0);
	cold_send_c2(c, 0x10, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x10, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send_c2(c, 0x10, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);

	/* pins 0x11 (the head unit's rear RCA input), 0x12, 0x13 */
	cold_param_c2(c, 0x11, AC_PAR_PIN_CAP);
	cold_param_c2(c, 0x11, AC_PAR_CONNLIST_LEN);
	cold_param_c2(c, 0x11, AC_PAR_POWER_STATE);
	cold_param_c2(c, 0x11, AC_PAR_CONNLIST_LEN);
	cold_send_c2(c, 0x11, AC_VERB_GET_POWER_STATE, 0);
	cold_send_c2(c, 0x11, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send_c2(c, 0x11, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	cold_send_c2(c, 0x11, AC_VERB_GET_CONFIG_DEFAULT, 0);
	cold_send_c2(c, 0x11, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x11, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x11, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	cold_param_c2(c, 0x12, AC_PAR_PIN_CAP);
	cold_param_c2(c, 0x12, AC_PAR_AMP_IN_CAP);
	cold_param_c2(c, 0x12, AC_PAR_POWER_STATE);
	cold_send_c2(c, 0x12, AC_VERB_GET_POWER_STATE, 0);
	cold_send_c2(c, 0x12, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send_c2(c, 0x12, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	cold_send_c2(c, 0x12, AC_VERB_GET_CONFIG_DEFAULT, 0);
	cold_send_c2(c, 0x12, AC_VERB_GET_AMP_GAIN_MUTE, AC_AMP_GET_INPUT | AC_AMP_GET_LEFT);
	cold_send_c2(c, 0x12, AC_VERB_GET_AMP_GAIN_MUTE, AC_AMP_GET_INPUT | AC_AMP_GET_RIGHT);
	cold_send_c2(c, 0x12, AC_VERB_SET_AMP_GAIN_MUTE,
		     AC_AMP_SET_INPUT | AC_AMP_SET_LEFT | AC_AMP_SET_RIGHT);
	cold_send_c2(c, 0x12, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	cold_param_c2(c, 0x13, AC_PAR_PIN_CAP);
	cold_param_c2(c, 0x13, AC_PAR_POWER_STATE);
	cold_send_c2(c, 0x13, AC_VERB_GET_POWER_STATE, 0);
	cold_send_c2(c, 0x13, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send_c2(c, 0x13, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	cold_send_c2(c, 0x13, AC_VERB_GET_CONFIG_DEFAULT, 0);
	cold_send_c2(c, 0x13, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);

	/* node 0x17 and pin 0x18 */
	cold_param_c2(c, 0x17, AC_PAR_PCM);
	cold_param_c2(c, 0x17, AC_PAR_STREAM);
	cold_param_c2(c, 0x17, AC_PAR_POWER_STATE);
	cold_send_c2(c, 0x17, AC_VERB_GET_STREAM_FORMAT, 0);
	cold_send_c2(c, 0x17, AC_VERB_GET_POWER_STATE, 0);
	cold_send_c2(c, 0x17, AC_VERB_GET_CONV, 0);
	cold_send_c2(c, 0x17, AC_VERB_GET_SDI_SELECT, 0);
	cold_send_c2(c, 0x17, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	cold_send_c2(c, 0x17, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	cold_param_c2(c, 0x18, AC_PAR_PIN_CAP);
	cold_param_c2(c, 0x18, AC_PAR_CONNLIST_LEN);
	cold_param_c2(c, 0x18, AC_PAR_POWER_STATE);
	cold_param_c2(c, 0x18, AC_PAR_CONNLIST_LEN);
	cold_send_c2(c, 0x18, AC_VERB_GET_POWER_STATE, 0);
	cold_send_c2(c, 0x18, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send_c2(c, 0x18, AC_VERB_GET_UNSOLICITED_RESPONSE, 0);
	cold_send_c2(c, 0x18, AC_VERB_GET_CONFIG_DEFAULT, 0);
	cold_send_c2(c, 0x18, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x18, AC_VERB_GET_CONNECT_LIST, 0);
	cold_send_c2(c, 0x18, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
}

/*
 * Codec 2: its function group's power state read (ae9_codec2_wake() acted on
 * it before the resets), then the configuration defaults of its 10 pins, as
 * CtxHdb.sys writes them. Entries 309-349 (tap seq 618-699).
 */
static void pre_codec2_pins(struct ae9_cold *c)
{
	cold_send_c2(c, AE9_NID_AFG, AC_VERB_GET_POWER_STATE, 0);
	cold_pin_cfg_c2(c, 0x0b, 0x410140f0);	/* no connection: line out, 1/8" */
	cold_pin_cfg_c2(c, 0x0c, 0x014521f0);	/* jack: S/PDIF out, optical */
	cold_pin_cfg_c2(c, 0x0d, 0x414570f0);	/* no connection: S/PDIF out, optical */
	cold_pin_cfg_c2(c, 0x0e, 0x01c511f0);	/* jack: S/PDIF in, optical */
	cold_pin_cfg_c2(c, 0x0f, 0x412000f0);	/* no connection: HP out */
	cold_pin_cfg_c2(c, 0x10, 0x422160f0);	/* no connection: HP out, 1/8" */
	cold_pin_cfg_c2(c, 0x11, 0x018471f0);	/* jack: line in, RCA */
	cold_pin_cfg_c2(c, 0x12, 0x41a791f0);	/* no connection: mic in, other analog */
	cold_pin_cfg_c2(c, 0x13, 0x50d000f0);	/* no connection: digital in */
	cold_pin_cfg_c2(c, 0x18, 0x500000f0);	/* no connection: line out */
}

/*
 * Codec 2's three streams set up (CtxHdb.sys), each converter and pin to D0,
 * the converter unbound: S/PDIF out, S/PDIF in, the head unit's rear RCA
 * input. Entries 350-382 (tap seq 700-765).
 */
static void pre_codec2_setup(struct ae9_cold *c)
{
	/*
	 * S/PDIF out: converter 0x05 (keep-alive, copyright, 48 kHz 16-bit, enabled
	 * with validity config, category 0x12, 2 channels) to pin 0x0c (out)
	 */
	cold_send_c2(c, 0x05, AC_VERB_SET_DIGI_CONVERT_3, AC_DIG3_KAE);
	cold_send_c2(c, 0x05, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
	cold_send_c2(c, 0x0c, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
	cold_send_c2(c, 0x05, AC_VERB_SET_CHANNEL_STREAMID, 0);
	cold_send_c2(c, 0x05, AC_VERB_SET_CHANNEL_STREAMID, 0);
	cold_send_c2(c, 0x05, AC_VERB_SET_DIGI_CONVERT_1, AC_DIG1_COPYRIGHT);
	cold_send_c2(c, 0x05, AC_VERB_SET_STREAM_FORMAT, AC_FMT_BITS_16);
	cold_send_c2(c, 0x05, AC_VERB_SET_STREAM_FORMAT, AC_FMT_BITS_16);
	cold_send_c2(c, 0x05, AC_VERB_SET_DIGI_CONVERT_1, AC_DIG1_ENABLE | AC_DIG1_COPYRIGHT);
	cold_send_c2(c, 0x05, AC_VERB_SET_DIGI_CONVERT_1,
		     AC_DIG1_ENABLE | AC_DIG1_VCFG | AC_DIG1_COPYRIGHT);
	cold_send_c2(c, 0x05, AC_VERB_SET_DIGI_CONVERT_2, 0x12);
	cold_send_c2(c, 0x05, AC_VERB_SET_CVT_CHAN_COUNT, 0x01);
	cold_send_c2(c, 0x05, AC_VERB_SET_DIGI_CONVERT_3, AC_DIG3_KAE);
	cold_send_c2(c, 0x0c, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_OUT_EN);
	cold_param_c2(c, 0x0c, AC_PAR_CONNLIST_LEN);

	/* S/PDIF in: converter 0x09 (48 kHz 16-bit, enabled) from pin 0x0e (in) */
	cold_send_c2(c, 0x09, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
	cold_send_c2(c, 0x0e, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
	cold_send_c2(c, 0x09, AC_VERB_SET_CHANNEL_STREAMID, 0);
	cold_send_c2(c, 0x09, AC_VERB_SET_CHANNEL_STREAMID, 0);
	cold_send_c2(c, 0x09, AC_VERB_SET_DIGI_CONVERT_1, AC_DIG1_COPYRIGHT);
	cold_send_c2(c, 0x09, AC_VERB_SET_STREAM_FORMAT, AC_FMT_BITS_16);
	cold_send_c2(c, 0x09, AC_VERB_SET_STREAM_FORMAT, AC_FMT_BITS_16);
	cold_send_c2(c, 0x09, AC_VERB_SET_DIGI_CONVERT_1, AC_DIG1_ENABLE | AC_DIG1_COPYRIGHT);
	cold_send_c2(c, 0x0e, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN);
	cold_param_c2(c, 0x09, AC_PAR_CONNLIST_LEN);

	/* the head unit's rear RCA input: ADC 0x08 from pin 0x11 (in, vref ground) */
	cold_send_c2(c, 0x08, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
	cold_send_c2(c, 0x11, AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
	cold_send_c2(c, 0x08, AC_VERB_SET_CHANNEL_STREAMID, 0);
	cold_send_c2(c, 0x08, AC_VERB_SET_CHANNEL_STREAMID, 0);
	cold_send_c2(c, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN | AC_PINCTL_VREF_GRD);
	cold_param_c2(c, 0x08, AC_PAR_CONNLIST_LEN);
	cold_send_c2(c, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN | AC_PINCTL_VREF_GRD);
	cold_send_c2(c, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN | AC_PINCTL_VREF_GRD);
}

/*
 * The 8051 code patches Windows (and mainline's ca0132_init_verbs0) writes
 * at start-up: at 0xfef0, MOV 0xD3,#0x09; ANL 0xD4,#0xEF; MOV 0xD3,#0x09;
 * LJMP 0x3778, reached from 0x3775; at 0xfee8, LJMP 0x6862, reached from
 * 0x6846
 */
static const u8 ae9_8051_patch_3775[] = {
	0x75, 0xd3, 0x09, 0x53, 0xd4, 0xef, 0x75, 0xd3, 0x09, 0x02, 0x37, 0x78
};

static const u8 ae9_8051_patch_6846[] = { 0x02, 0x68, 0x62 };

/*
 * Codec 1's init, Windows' codec start-up (CtxHda FUN_0001c7dc, the order
 * proven against the captures): the function group count, the AFG's power
 * state (Windows' first check, returned: ~0 when the read failed), the idle
 * flag off, chip register 0x18b0a4 bit 1 set (Windows reads it and writes
 * it back with bit 1 set: the value it wrote, 0xc2, is sent; the read's
 * answer isn't used). Entries 383-394 (tap seq 766-789).
 */
static u32 pre_codec1_start(struct ae9_cold *c)
{
	u32 f05 = 0;

	cold_param(c, AC_NODE_ROOT, AC_PAR_NODE_COUNT);
	if (cold_verb(c, creative_ae9_verb(AE9_CAD, AE9_NID_AFG, AC_VERB_GET_POWER_STATE, 0),
		      &f05) < 0)
		f05 = ~0U;
	cold_ctl_flag(c, CONTROL_FLAG_IDLE_ENABLE, false);
	cold_chipio_read(c, 0x18b0a4);
	cold_chipio_write(c, 0x18b0a4, 0x000000c2);
	return f05;
}

/*
 * Codec 1's function group reset, twice, then Creative's extensions on
 * (FUN_0001c7dc, then its ChipIO set-up FUN_0001b078). ae9_codec1_wake()
 * goes before it. Entries 395-397.
 */
static void pre_codec1_reset(struct ae9_cold *c)
{
	cold_send(c, AE9_NID_AFG, AC_VERB_SET_CODEC_RESET, 0);
	cold_send(c, AE9_NID_AFG, AC_VERB_SET_CODEC_RESET, 0);
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_CT_EXTENSIONS_ENABLE, 1);
}

/*
 * Codec 1's widgets read again (the function group's node count, every
 * node's capabilities) and its pins' configuration defaults written, each
 * after its node's read. Entries 398-462 (tap seq 796-925); the reads'
 * answers aren't used.
 */
static void pre_codec1_pins(struct ae9_cold *c)
{
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
	cold_pin_cfg(c, 0x0b, 0x01017010);	/* jack: line out, 1/8" */
	cold_param(c, 0x0c, AC_PAR_AUDIO_WIDGET_CAP);
	cold_pin_cfg(c, 0x0c, 0x414510f0);	/* no connection: S/PDIF out, optical */
	cold_param(c, 0x0d, AC_PAR_AUDIO_WIDGET_CAP);
	cold_pin_cfg(c, 0x0d, 0x414510f0);	/* no connection: S/PDIF out, optical */
	cold_param(c, 0x0e, AC_PAR_AUDIO_WIDGET_CAP);
	cold_pin_cfg(c, 0x0e, 0x01c520f0);	/* jack: S/PDIF in, optical */
	cold_param(c, 0x0f, AC_PAR_AUDIO_WIDGET_CAP);
	cold_pin_cfg(c, 0x0f, 0x01017114);	/* jack: line out, 1/8" */
	cold_param(c, 0x10, AC_PAR_AUDIO_WIDGET_CAP);
	cold_pin_cfg(c, 0x10, 0x01017011);	/* jack: line out, 1/8" */
	cold_param(c, 0x11, AC_PAR_AUDIO_WIDGET_CAP);
	cold_pin_cfg(c, 0x11, 0x41a170ff);	/* no connection: mic in, 1/8" */
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

/*
 * The 8051's set-up, the same bytes as mainline's ca0132_init_verbs0 and
 * ca0132_init_verbs1 (desktop cards): PLL/PMU register 0x49 = 0x88 (a pin
 * select: 0x88 selects none), two code patches with their entries in the
 * 8051's C register file (on the AE-9 both patches and their jumps are in
 * program memory at power-up already, uv68: these writes change nothing
 * here), then XRAM 0x1920, entry 0x39, XRAM 0x09b7, 0x09af-0x09b0 and 0x0973
 * (meaning unknown, sent as captured). Entries 463-506 (tap seq 926-1013).
 */
static void pre_8051_setup(struct ae9_cold *c)
{
	cold_pll_pmu_write(c, 0x49, 0x88);

	/*
	 * 12 bytes at 0xfef0: MOV 0xD3,#0x09; ANL 0xD4,#0xEF; MOV 0xD3,#0x09;
	 * LJMP 0x3778 (PMU register 9 bit 4 cleared, meaning open); C register file
	 * entries 0x3c/0x3d = 0x3775 with bit 7 set: the patch's jump site
	 */
	cold_xram_write_n(c, 0xfef0, ae9_8051_patch_3775, ARRAY_SIZE(ae9_8051_patch_3775));
	cold_8051_cfile_write(c, 0x3c, 0x75);
	cold_8051_cfile_write(c, 0x3d, 0xb7);

	/*
	 * 3 bytes at 0xfee8: LJMP 0x6862 (the handler at 0x682b skips 0x6849-0x6861);
	 * entries 0x3a/0x3b = 0x6846 with bit 7 set
	 */
	cold_xram_write_n(c, 0xfee8, ae9_8051_patch_6846, ARRAY_SIZE(ae9_8051_patch_6846));
	cold_8051_cfile_write(c, 0x3a, 0x46);
	cold_8051_cfile_write(c, 0x3b, 0xe8);

	/*
	 * mainline's list for desktop cards (ca0132_init_verbs1; meaning
	 * unknown; its last verb, ChipIO verb 6, Windows sends later)
	 */
	cold_xram_write(c, 0x1920, 0x00);
	cold_8051_cfile_write(c, 0x39, 0x46);
	cold_xram_write(c, 0x09b7, 0x10);
	cold_xram_write_n(c, 0x09af, (const u8[]){ 0x01, 0x05 }, 2);
	cold_xram_write(c, 0x0973, 0x14);
}

/*
 * Chip register 0x18b008 bit 3 (a hold bit) pulsed: Windows reads it, writes
 * it with bit 3 set, then clear (the values it wrote, 0xf8 and 0xf0, are
 * sent; it clears the bit again at every stream start, FUN_00022778). Then
 * the DSPIO node's unsolicited responses on, tag 0. Entries 507-520 (tap
 * seq 1014-1041).
 */
static void pre_codec1_chip(struct ae9_cold *c)
{
	cold_chipio_read(c, 0x18b008);
	cold_chipio_write(c, 0x18b008, 0x000000f8);
	cold_chipio_write(c, 0x18b008, 0x000000f0);
	cold_send(c, WIDGET_DSP_CTRL, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 0);
}

/*
 * Codec 1's ChipIO set-up after DSP_INIT (FUN_0001c7dc): control parameters
 * 2, 0 and 1 to 0 (S/PDIF 1 source, "dm", voice-processor source), ports A
 * and D 160 ohm gain 6 and 10 kohm load on; the PMR1 power register
 * 0x18b030 (Windows reads it and writes bits 3/4 back: the 0x27 it wrote is
 * sent; note 44 saw 0x21 read on some power-ups); PLL/PMU registers 5 and 6
 * (verb 6, index 4 = 0x3ff); the AFG's 16-bit vendor word (Windows reads
 * it, clears bits 13 and 11 and writes it back, low byte then high: 0x5300
 * in all 28 captures); S/PDIF 2 out off; connection points 0x0a (What U
 * Hear) and 0x0b at 48 kHz. Entries 522-554 (tap seq 1044-1109).
 */
static void pre_codec1_params(struct ae9_cold *c)
{
	cold_ctl_param(c, CONTROL_PARAM_SPDIF1_SOURCE, 0);
	cold_ctl_param(c, AE9_CTL_PARAM_DM, 0);
	cold_ctl_param(c, CONTROL_PARAM_VIP_SOURCE, 0);
	cold_ctl_param(c, CONTROL_PARAM_PORTA_160OHM_GAIN, 6);
	cold_ctl_param(c, CONTROL_PARAM_PORTD_160OHM_GAIN, 6);
	cold_ctl_flag(c, CONTROL_FLAG_PORT_A_10KOHM_LOAD, true);
	cold_ctl_flag(c, CONTROL_FLAG_PORT_D_10KOHM_LOAD, true);

	/* PMR1: Windows' read-modify-write of bits 3/4, the value it wrote */
	cold_chipio_read(c, 0x18b030);
	cold_chipio_write(c, 0x18b030, 0x00000027);

	/*
	 * PLL/PMU registers 6 = 0xff and 5 bits 1:0 = 3 (the register 5/6 half of
	 * mainline's chipio_enable_clocks(); the 8051 at 0x76e4 -> bank 1 0xb9a1)
	 */
	cold_send(c, WIDGET_CHIP_CTRL, AE9_VERB_CHIPIO_PMU_SET, 0x3ff << 6 | 4);

	/*
	 * the AFG's vendor word read, then 0x5300 written (Windows clears bits 13
	 * and 11 of what it read; mainline's ca0132_gpio_init() sends the same)
	 */
	cold_send(c, AE9_NID_AFG, AE9_VERB_GET_NODE_WORD, 0);
	cold_send(c, AE9_NID_AFG, AE9_VERB_SET_NODE_WORD_LO, 0);
	cold_send(c, AE9_NID_AFG, AE9_VERB_SET_NODE_WORD_HI, 0x53);

	/* S/PDIF 2 out off; connection points 0x0a (What U Hear) and 0x0b at 48 kHz */
	cold_ctl_flag(c, CONTROL_FLAG_SPDIF2OUT, false);
	cold_conn_rate(c, 0x0a, AE9_SR_48000);
	cold_conn_rate(c, 0x0b, AE9_SR_48000);
}

/*
 * Codec 1's widgets read a third time, as Windows' codec driver builds its
 * view of the codec: the node counts, every node's capabilities, then node
 * by node its connection list length and list and its power state; each
 * converter and pin the AE-9 doesn't use goes to D3 (pin 0x11 and ADC 0x08,
 * pin 0x0c and converter 0x05, pin 0x0d and converter 0x06, converter 0x04,
 * pin 0x18 and node 0x17). Entries 555-666 (tap seq 1110-1333); the reads'
 * answers aren't used.
 */
static void pre_codec1_widgets(struct ae9_cold *c)
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

	/* node by node: connection list length (and list), power state */
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
	cold_send(c, 0x11, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	cold_send(c, 0x08, AC_VERB_GET_POWER_STATE, 0);
	cold_send(c, 0x08, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
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
	cold_send(c, 0x0c, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
	cold_send(c, 0x05, AC_VERB_GET_POWER_STATE, 0);
	cold_send(c, 0x05, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
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
	cold_send(c, 0x04, AC_VERB_SET_POWER_STATE, AC_PWRST_D3);
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
 * Codec 1's inputs (Windows' pin set-up): the mic pins 0x11 and 0x12 off,
 * then 0x12 in with vref 80 % and 0x11's vref 80 % with its input off; ADC
 * 0x07's and pin 0x12's input amps read (with ADC 0x07's fine steps,
 * Creative's 0xf97/0xf98); pin 0x13 off, then in. Each pin control is read
 * before it is written (Windows' read-modify-writes: the values it wrote are
 * sent). Entries 667-703 (tap seq 1334-1407).
 */
static void pre_codec1_inputs(struct ae9_cold *c)
{
	cold_param(c, 0x07, AC_PAR_VENDOR_ID);
	cold_param(c, 0x12, AC_PAR_PIN_CAP);
	cold_param(c, 0x09, AC_PAR_VENDOR_ID);
	cold_param(c, 0x0a, AC_PAR_VENDOR_ID);
	cold_param(c, 0x02, AC_PAR_VENDOR_ID);
	cold_param(c, 0x12, AC_PAR_PIN_CAP);
	cold_param(c, 0x07, AC_PAR_PCM);
	cold_param(c, 0x07, AC_PAR_VENDOR_ID);
	cold_param(c, 0x12, AC_PAR_VENDOR_ID);
	cold_param(c, 0x12, AC_PAR_PIN_CAP);

	/* mic pins 0x11 and 0x12 */
	cold_send(c, 0x12, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x12, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x12, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_VREF_80);
	cold_send(c, 0x12, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN | AC_PINCTL_VREF_80);

	/* the input amps read */
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

	/* pin 0x13 (fixed line in) */
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
 * Codec 1's outputs: pin 0x0b EAPD and pin off (twice, a jack-sense read
 * between), pins 0x0f and 0x10 off, then out with their headphone amps;
 * converter 0x02's output amp read; unsolicited responses on for pins 0x0b
 * and 0x10 (tag 3). Entries 704-735 (tap seq 1408-1471).
 */
static void pre_codec1_outputs(struct ae9_cold *c)
{
	cold_param(c, 0x0b, AC_PAR_PIN_CAP);
	cold_param(c, 0x02, AC_PAR_PCM);
	cold_param(c, 0x02, AC_PAR_VENDOR_ID);
	cold_param(c, 0x04, AC_PAR_VENDOR_ID);
	cold_param(c, 0x03, AC_PAR_VENDOR_ID);
	cold_param(c, 0x0b, AC_PAR_VENDOR_ID);
	cold_param(c, 0x0b, AC_PAR_PIN_CAP);
	cold_send(c, 0x0b, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0b, AC_VERB_SET_EAPD_BTLENABLE, 0);
	cold_send(c, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0b, AC_VERB_GET_PIN_SENSE, 0);
	cold_send(c, 0x0b, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0b, AC_VERB_SET_EAPD_BTLENABLE, 0);
	cold_send(c, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);

	/* pins 0x0f and 0x10 */
	cold_param(c, 0x0f, AC_PAR_VENDOR_ID);
	cold_param(c, 0x0f, AC_PAR_PIN_CAP);
	cold_send(c, 0x0f, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0f, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN);
	cold_param(c, 0x10, AC_PAR_VENDOR_ID);
	cold_param(c, 0x10, AC_PAR_PIN_CAP);
	cold_send(c, 0x10, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x10, AC_VERB_GET_PIN_SENSE, 0);
	cold_send(c, 0x10, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN);

	/* converter 0x02's output amp; jack sense on pins 0x0b and 0x10 */
	cold_param(c, 0x0b, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x02, AC_PAR_AUDIO_WIDGET_CAP);
	cold_param(c, 0x02, AC_PAR_AMP_OUT_CAP);
	cold_send(c, 0x0b, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 3);
	cold_send(c, 0x10, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 3);
}

/*
 * Codec 1's AFG vendor verb 0x790 (read, then 0x23 written: mainline's
 * ca0132_gpio_init() writes the same after the AFG's word; meaning not
 * given), pin 0x12's input amp at step 1, then each pin set up again with
 * its jack sense on, in Windows' order: 0x12 (tag 1), 0x13 (tag 2), 0x0b,
 * 0x0f and 0x10 (tag 3); last, the AFG's power states read. Entries 736-773
 * (tap seq 1472-1547).
 */
static void pre_codec1_jacks(struct ae9_cold *c)
{
	cold_send(c, AE9_NID_AFG, AE9_VERB_GET_790, 0);
	cold_send(c, AE9_NID_AFG, AE9_VERB_SET_790, 0x23);

	/* pin 0x12's input amp, step 1 on both channels (+10 dB on its scale) */
	cold_send(c, 0x12, AC_VERB_SET_AMP_GAIN_MUTE, AC_AMP_SET_INPUT | AC_AMP_SET_LEFT | 1);
	cold_send(c, 0x12, AC_VERB_SET_AMP_GAIN_MUTE, AC_AMP_SET_INPUT | AC_AMP_SET_RIGHT | 1);

	/* the pins again, each with its jack sense on: mic pin 0x12 (tag 1) with 0x11 */
	cold_send(c, 0x12, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 1);
	cold_send(c, 0x12, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x12, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x12, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_VREF_80);
	cold_send(c, 0x12, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN | AC_PINCTL_VREF_80);

	/* pin 0x13 (tag 2) */
	cold_send(c, 0x13, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 2);
	cold_send(c, 0x13, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x13, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x13, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x13, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_IN_EN);

	/* pins 0x0b, 0x0f and 0x10 (tag 3) */
	cold_send(c, 0x0b, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 3);
	cold_send(c, 0x0b, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0b, AC_VERB_SET_EAPD_BTLENABLE, 0);
	cold_send(c, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0b, AC_VERB_GET_PIN_SENSE, 0);
	cold_send(c, 0x0b, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0b, AC_VERB_SET_EAPD_BTLENABLE, 0);
	cold_send(c, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0f, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 3);
	cold_send(c, 0x0f, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0f, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN);
	cold_send(c, 0x10, AC_VERB_SET_UNSOLICITED_ENABLE, AC_USRSP_EN | 3);
	cold_send(c, 0x10, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x10, AC_VERB_GET_PIN_SENSE, 0);
	cold_send(c, 0x10, AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN);

	/* the AFG's power states (Windows' codec init ends here; its 1.5 s timer follows) */
	cold_param(c, AE9_NID_AFG, AC_PAR_POWER_STATE);
}

/*
 * Windows' verbs at the top of every firmware download (CtxHda
 * FUN_0012d914, the DSP image's rate 96 kHz from its registry default):
 * PLL/PMU register 0 = 0xff (all its bits on: the first write of mainline's
 * chipio_enable_clocks()), the 96 kHz flags for the DSP, the DACs, ADCs B
 * and C and the sample-rate converter, connection points 0x0a (What U Hear)
 * and 0x0b at 48 kHz again, then a ChipIO status read (the download's first
 * verb, the ChipIO stream format, follows: dsp.c). Entries 795-814 (tap
 * seq 1590-1629).
 */
static void pre_dl_prefix(struct ae9_cold *c)
{
	cold_pll_pmu_write(c, 0x00, 0xff);
	cold_ctl_flag(c, CONTROL_FLAG_DSP_96KHZ, true);
	cold_ctl_flag(c, CONTROL_FLAG_DAC_96KHZ, true);
	cold_ctl_flag(c, CONTROL_FLAG_ADC_B_96KHZ, true);
	cold_ctl_flag(c, CONTROL_FLAG_ADC_C_96KHZ, true);
	cold_ctl_flag(c, CONTROL_FLAG_SRC_RATE_96KHZ, true);
	cold_conn_rate(c, 0x0a, AE9_SR_48000);
	cold_conn_rate(c, 0x0b, AE9_SR_48000);
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_STATUS, 0);
}

/*
 * The download prefix again, before a re-download (dsp.c's dsp_soft_reset(),
 * as Windows: its dsp_reset FUN_0001b024 and 0.5 s, then FUN_0012d914 from
 * the top). Every verb waits for its answer; none is checked.
 */
void creative_ae9_dl_prefix(struct creative_chip *chip)
{
	struct ae9_cold c = { .chip = chip };

	pre_dl_prefix(&c);
}

/*
 * Windows' start-up up to its firmware download, both codecs (cold-boot
 * capture vm_capture_coldboot_20261101_160959, tap seq 0-1629: the 815
 * verbs before the first download verb), with Windows' waits and checks
 * and steps of our own: codec 2's wake, the board-D0 GPIO step, codec 1's
 * wake, the DSP_INIT busy retry, the 1.5 s settle, the boot checks and the
 * recovery cycle. Every verb waits for its answer (CORB once it runs,
 * immediate commands before); none stops the sequence; the log line counts
 * Windows' verbs and the failed ones.
 */
void creative_ae9_predownload(struct creative_chip *chip)
{
	struct ae9_cold c = { .chip = chip };
	int ramp[AE9_RAMP_N];
	u32 f05;

	pre_codecs_found(&c);
	ae9_codec2_wake(chip);
	pre_codec2_reset(&c);
	pre_probe_both(&c);
	pre_codec2_widgets(&c);
	pre_codec2_pins(&c);
	pre_codec2_setup(&c);
	creative_ae9_board_d0(chip);
	f05 = pre_codec1_start(&c);
	ae9_codec1_wake(chip, f05);
	pre_codec1_reset(&c);
	pre_codec1_pins(&c);
	pre_8051_setup(&c);
	pre_codec1_chip(&c);
	ae9_dsp_init(&c);
	pre_codec1_params(&c);
	pre_codec1_widgets(&c);
	pre_codec1_inputs(&c);
	pre_codec1_outputs(&c);
	pre_codec1_jacks(&c);

	/*
	 * Windows' 1.5 s timer between the codec set-up and the ramp checks
	 * + download (26/26 captures)
	 */
	msleep(1500);

	/* the 7 ramp status bytes (the second check), the recovery cycle */
	cold_ramp_read(&c, ramp);
	ae9_recovery(chip, f05, ramp);

	pre_dl_prefix(&c);
	creative_dsp_chipio_addr_invalidate();
	dev_info(chip->dev, "cold_prefw: replayed %u pre-download verbs (%u errors)\n",
		 c.verbs, c.errs);
}

/* ------------------------------------------------------------------ */
/* The start-up tail: Windows' set-up after the DSP's boot handshake	  */
/* ------------------------------------------------------------------ */

/*
 * What follows Windows' boot handshake in its codec-1 stream, in the warm
 * capture (ae9_corb_capture_raw.log, the stream's entries 1334-3164; the
 * table ae9_initseq_tail until 2026-11-16): its DSP settings after the image
 * load, its playback engine's start, a stream's start and stop with its
 * levels, the speaker set-up, the effects' settings, then what the user did
 * while the capture ran (volume changes, SBX switched on and off, the output
 * switched to the headphones and back), each as Windows sent it. The driver
 * has sent it at every start-up since the first working one (2026-11-01).
 * Windows' reads in it (ChipIO status and data, the 8051's XRAM, connection
 * point rates, DSP replies) were left out with every read when the table was
 * made, and are not sent: where only their address set-up remains it is
 * sent, as the table had it.
 */

/* the DSP's modules the tail talks to (mainline ca0132 names where it has them) */
#define AE9_MID_MASTER		0x80	/* MASTERCONTROL */
#define AE9_MID_FX_OUT		0x96	/* output effects (SBX), speaker tuning, the output mute */
#define AE9_MID_FX_IN		0x95	/* input effects (CrystalVoice) */
#define AE9_MID_SPK_VOL		0x32	/* the DSP's speaker volume (no effect here, uv44) */
#define AE9_MID_MIC_LEVEL	0x37	/* the DSP's mic levels (monitor mixer, core 0) */
#define AE9_MID_SPK_EQ		0x8f	/* speaker EQ */

#define AE9_F_TWO		0x40000000	/* 2.0 */
#define AE9_F_THREE		0x40400000	/* 3.0 */

/*
 * ChipIO control parameter 3: "mch" in ca0132-tools (not in mainline): 3 at
 * the playback engine's start (CtxHda FUN_00032dc0), 0xcc on the 96 kHz DSP
 * path (notes 08)
 */
#define AE9_CTL_PARAM_MCH	3
/*
 * ChipIO verb 0x725: an ASI rate-class request (class = bits 2:0, bit 7 =
 * enable; Windows reads it back with 0xf25): 0x81 for 44.1-192 kHz, 0x80 for
 * 384 kHz and the sample-rate converter (notes 08); not in mainline
 */
#define AE9_VERB_CHIPIO_ASI_RATE 0x725
#define AE9_STREAM_DSP_PROC	0x0c	/* DSP-processed audio to the DACs (mainline AE-5/7) */
#define AE9_AMP_ADC_3DB		(AE9_AMP_0DB + 3)	/* ADC 0x07's input amp at +3 dB */

/*
 * The tail's sender. Verbs go one at a time, waited for, as the
 * pre-download start-up sends them (struct ae9_cold: CORB, or immediate
 * command before it runs; counted, a failure only counted; no lock: the
 * bring-up is the only user of the card). DSP messages (a header and one
 * data word each, source 0x20, as every message of the tail) go through
 * creative_dsp_scp_send(), counted, with the rules the tail always had:
 * once 8 messages in a row have failed, the DSP is taken to have stopped
 * taking commands and every later message is skipped (each would wait out
 * its timeout); failed and skipped messages, up to AE9_TAIL_RETRY_MAX, are
 * kept and sent once more by creative_ae9_tail_resend() after the summary
 * line (no list if it couldn't be allocated: nothing is sent again).
 */
struct ae9_tl {
	struct ae9_cold c;
	struct ae9_tail *t;
};

static void tl_keep(struct ae9_tail *t, u32 hdr, u32 val)
{
	if (!t->retry || t->nretry >= AE9_TAIL_RETRY_MAX)
		return;
	t->retry[t->nretry].hdr = hdr;
	t->retry[t->nretry].val = val;
	t->nretry++;
}

/* One DSP message: sent, or skipped after 8 failures in a row; kept if not taken */
static void tl_scp(struct ae9_tl *tl, u32 hdr, u32 val)
{
	struct ae9_tail *t = tl->t;

	if (t->consec_err >= 8) {
		t->msg_skipped++;
		tl_keep(t, hdr, val);
	} else if (creative_dsp_scp_send(tl->c.chip, hdr, &val, 1) < 0) {
		t->msg_err++;
		t->consec_err++;
		tl_keep(t, hdr, val);
	} else {
		t->consec_err = 0;
	}
	t->msgs++;
}

/*
 * A DSP message: SET request req of module mid, one data word, from source
 * 0x20 (the header as blk_scp_set()'s, mainline's make_scp_header())
 */
static void tl_set(struct ae9_tl *tl, u8 mid, u8 req, u32 val)
{
	tl_scp(tl, 1 << 27 | req << 17 | 0x20 << 8 | mid, val);
}

/*
 * A GET of request req of module mid (its data word 0). Windows read the
 * reply (0x702/0x704 on node 0x16: not sent, creative_dsp_scp_send() reads
 * a GET's reply itself)
 */
static void tl_get(struct ae9_tl *tl, u8 mid, u8 req)
{
	tl_scp(tl, 1 << 27 | req << 17 | 1 << 16 | 0x20 << 8 | mid, 0);
}

/*
 * A SET of Windows' that the AE-9's DSP rejects (an error reply at every
 * boot: features the card lacks): counted "dropped", not sent; it doesn't
 * end a run of failures. Six in the tail, named where they come.
 */
static void tl_dropped(struct ae9_tl *tl)
{
	tl->t->msg_dropped++;
	tl->t->msgs++;
}

/* DSP settings sent one after another: module, request, value */
struct ae9_dsp_set {
	u8 mid, req;
	u32 val;
};

static void tl_set_list(struct ae9_tl *tl, const struct ae9_dsp_set *l, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		tl_set(tl, l[i].mid, l[i].req, l[i].val);
}

/*
 * A ChipIO control parameter as mainline's chipio_set_control_param() picks
 * its form: one verb for an id below 32 and a value below 8, else 0x717 and
 * 0x718 (every parameter of the tail follows that rule). Windows read the
 * ChipIO status before the two-verb form; the table, and so the driver,
 * left that read out.
 */
static void tl_ctl_param(struct ae9_cold *c, u8 id, u8 val)
{
	if (id < 32 && val < 8) {
		cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_SET, val << 5 | id);
		return;
	}
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_EX_ID_SET, id);
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_PARAM_EX_VALUE_SET, val);
}

/* A connection point's sample rate (mainline chipio_set_conn_rate()) */
static void tl_conn_rate(struct ae9_cold *c, u8 cp, u8 rate)
{
	tl_ctl_param(c, CONTROL_PARAM_CONN_POINT_ID, cp);
	tl_ctl_param(c, CONTROL_PARAM_CONN_POINT_SR, rate);
}

/* A DSP stream selected, then its source and destination connection points */
static void tl_stream_route(struct ae9_cold *c, u8 sid, u8 src, u8 dst)
{
	tl_ctl_param(c, CONTROL_PARAM_STREAM_ID, sid);
	tl_ctl_param(c, CONTROL_PARAM_STREAM_SOURCE_CONN, src);
	tl_ctl_param(c, CONTROL_PARAM_STREAM_DEST_CONN, dst);
}

/* A DSP stream selected, then its channel count (6 = the 5.1 code) */
static void tl_stream_channels(struct ae9_cold *c, u8 sid, u8 ch)
{
	tl_ctl_param(c, CONTROL_PARAM_STREAM_ID, sid);
	tl_ctl_param(c, CONTROL_PARAM_STREAMS_CHANNELS, ch);
}

/* A DSP stream selected, then enabled */
static void tl_stream_on(struct ae9_cold *c, u8 sid)
{
	tl_ctl_param(c, CONTROL_PARAM_STREAM_ID, sid);
	tl_ctl_param(c, CONTROL_PARAM_STREAM_CONTROL, 1);
}

/*
 * The 8051's chip pin functions for the I2S to the DACs (decoded doc: pin 4
 * MCLK, 3 BCLK, 5 LRCK, 0 SDO0, 2 SDO1, pin 1 function 0xe) and the ASI
 * clock pin select 0x51 = 0xdb (BCLK pin 3, LRCK pin 5), in Windows' order
 */
static void tl_i2s_pins(struct ae9_cold *c)
{
	cold_pll_pmu_write(c, 0x44, 0xc8);
	cold_pll_pmu_write(c, 0x43, 0xcc);
	cold_pll_pmu_write(c, 0x45, 0xcb);
	cold_pll_pmu_write(c, 0x40, 0xc7);
	cold_pll_pmu_write(c, 0x42, 0xcd);
	cold_pll_pmu_write(c, 0x41, 0xce);
	cold_pll_pmu_write(c, 0x51, 0xdb);
}

/*
 * The 8051 address of stream 0x18's Active byte (its stream table, as
 * pb_stream_setup() reads it): Windows read it there (FUN_00034250, before
 * it enabled the stream); the read itself isn't in the table, the address is
 */
static void tl_stream18_active_addr(struct ae9_cold *c)
{
	cold_8051_addr(c, AE9_XRAM_STREAM_ACTIVE(AE9_STREAM_DSP_OUT));
}

/* the DSP's output mute (module 0x96 req 0x3a): 1.0 while Windows sets up the output, 0 after */
static void tl_dsp_mute(struct ae9_tl *tl, bool on)
{
	tl_set(tl, AE9_MID_FX_OUT, 0x3a, on ? AE9_F_ONE : 0);
}

/* the DSP's output unmuted and its mute read back (the GET's reply isn't used) */
static void tl_unmute_check(struct ae9_tl *tl)
{
	tl_dsp_mute(tl, false);
	tl_get(tl, AE9_MID_FX_OUT, 0x3a);
}

/*
 * A converter's output channel at 0 dB, as Windows' KS volume setter
 * (CtxHda FUN_000218dc) sends it: its fine step 0, the amp (muted at 0 dB
 * first when unmute: the stream's first level), the amp at 0 dB
 */
static void tl_out_level(struct ae9_cold *c, u8 nid, bool right, bool unmute)
{
	u16 ch = AC_AMP_SET_OUTPUT | (right ? AC_AMP_SET_RIGHT : AC_AMP_SET_LEFT);

	cold_send(c, nid, right ? AE9_VERB_SET_FINE_VOL_R : AE9_VERB_SET_FINE_VOL_L, 0);
	if (unmute)
		cold_send(c, nid, AC_VERB_SET_AMP_GAIN_MUTE, ch | AC_AMP_MUTE | AE9_AMP_0DB);
	cold_send(c, nid, AC_VERB_SET_AMP_GAIN_MUTE, ch | AE9_AMP_0DB);
}

/* both channels of a converter at 0 dB */
static void tl_out_0db(struct ae9_cold *c, u8 nid, bool unmute)
{
	tl_out_level(c, nid, false, unmute);
	tl_out_level(c, nid, true, unmute);
}

/*
 * The DSP's speaker volume (module 0x32): left and right in dB, then the
 * volume update (0, or 1.0 where Windows sent that)
 */
static void tl_spk_vol(struct ae9_tl *tl, u32 db, u32 update)
{
	tl_set(tl, AE9_MID_SPK_VOL, 0x03, db);
	tl_set(tl, AE9_MID_SPK_VOL, 0x04, db);
	tl_set(tl, AE9_MID_SPK_VOL, 0x02, update);
}

/* converter 0x02's channel at 0 dB, then the DSP's speaker volume */
static void tl_dac_level(struct ae9_tl *tl, bool right, u32 db, u32 update)
{
	tl_out_level(&tl->c, AE9_NID_DAC, right, false);
	tl_spk_vol(tl, db, update);
}

/*
 * One volume change of Windows' (its KS volume setter, CtxHda FUN_000218dc
 * + FUN_0002174c, for the volume the user set): converter 0x02 left and
 * right at 0 dB, each followed by the DSP's speaker volume at db (the head
 * unit's own volume is sent by the post-start-up set-up and the heartbeat),
 * converters 0x03 and 0x04 at 0 dB
 */
static void tl_volume(struct ae9_tl *tl, u32 db)
{
	tl_dac_level(tl, false, db, 0);
	tl_dac_level(tl, true, db, 0);
	tl_out_0db(&tl->c, 0x03, false);
	tl_out_0db(&tl->c, 0x04, false);
}

/*
 * ADC 0x07's input level, one channel (the recording level, Windows' volume
 * setter): its fine step 0, the amp (muted at 0 dB first when unmute), the
 * amp at +3 dB
 */
static void tl_adc_level(struct ae9_cold *c, bool right, bool unmute)
{
	u16 ch = AC_AMP_SET_INPUT | (right ? AC_AMP_SET_RIGHT : AC_AMP_SET_LEFT);

	cold_send(c, AE9_NID_ADC, right ? AE9_VERB_SET_FINE_VOL_R : AE9_VERB_SET_FINE_VOL_L, 0);
	if (unmute)
		cold_send(c, AE9_NID_ADC, AC_VERB_SET_AMP_GAIN_MUTE,
			  ch | AC_AMP_MUTE | AE9_AMP_0DB);
	cold_send(c, AE9_NID_ADC, AC_VERB_SET_AMP_GAIN_MUTE, ch | AE9_AMP_ADC_3DB);
}

/* the DSP's mic 1 level (module 0x37, CtxHda FUN_0002174c): not muted, left and right 3 dB */
static void tl_mic_level(struct ae9_tl *tl)
{
	tl_set(tl, AE9_MID_MIC_LEVEL, 0x01, 0);
	tl_set(tl, AE9_MID_MIC_LEVEL, 0x02, AE9_F_THREE);
	tl_set(tl, AE9_MID_MIC_LEVEL, 0x03, AE9_F_THREE);
}

/* the XLR pin's input amp, one channel: gain step 1 (+10 dB: the Mic Gain then) */
static void tl_xlr_gain(struct ae9_cold *c, bool right)
{
	cold_send(c, AE9_NID_MIC_XLR, AC_VERB_SET_AMP_GAIN_MUTE,
		  AC_AMP_SET_INPUT | (right ? AC_AMP_SET_RIGHT : AC_AMP_SET_LEFT) | 1);
}

/* ---- Windows' DSP settings after its image load ---- */

/*
 * Mic 1's configuration (LineMic1Config, CtxHda FUN_00026f44): connection
 * points 0x03 (MICIN1) and 0x0c (MICOUT1) at 96 kHz (the DSP image's rate),
 * then MASTERCONTROL req 0 = 3.0 (a mono mic: mainline's analog-mic set-up)
 */
static void tail_mic1_config(struct ae9_tl *tl)
{
	tl_conn_rate(&tl->c, 0x03, AE9_SR_96000);
	tl_conn_rate(&tl->c, 0x0c, AE9_SR_96000);
	tl_set(tl, AE9_MID_MASTER, 0x00, AE9_F_THREE);
}

/*
 * Windows' DSP settings once its DSP image is loaded (CtxHda FUN_00012d6c,
 * "DspLoaded"; entries 0-57):
 * - chip 0x100e34 = 0xf1000000: the DSP's register after DBGCNTL; the
 *   firmware's start-up list writes the same, nothing reads it (notes 44,
 *   uv27/uv28; meaning open);
 * - mic 1's configuration; mic 2's (LineMic2Config, FUN_00027140):
 *   connection points 0x05 (MICIN2) and 0x0e (MICOUT2) at 96 kHz,
 *   MASTERCONTROL req 1 = 0 (no mic 2); the mics' hardware monitors off
 *   (LineMic1Monitor/LineMic2Monitor, FUN_0002733c and FUN_000273d0: req
 *   0x0d, 0x0e);
 * - connection point 0x03 selected: the voice processor's check of its
 *   rate (VIPMasterControl, FUN_00016ca0; the read isn't in the table);
 * - mainline's *_setup_defaults(): the DSP's headroom 0 (0x96 req 0x3c),
 *   What U Hear's source 2.0 (module 0x31 req 0), the speaker source 2.0
 *   (0x32 req 0);
 * - mic 1's configuration once more (its caller after FUN_00012d6c's
 *   settings isn't traced).
 */
static void tail_dsp_settings(struct ae9_tl *tl)
{
	struct ae9_cold *c = &tl->c;

	cold_chipio_write(c, 0x100e34, 0xf1000000);
	tail_mic1_config(tl);
	tl_conn_rate(c, 0x05, AE9_SR_96000);
	tl_conn_rate(c, 0x0e, AE9_SR_96000);
	tl_set(tl, AE9_MID_MASTER, 0x01, 0);
	tl_set(tl, AE9_MID_MASTER, 0x0d, 0);
	tl_set(tl, AE9_MID_MASTER, 0x0e, 0);
	tl_ctl_param(c, CONTROL_PARAM_CONN_POINT_ID, 0x03);
	tl_set(tl, AE9_MID_FX_OUT, 0x3c, 0);
	tl_set(tl, 0x31, 0x00, AE9_F_TWO);
	tl_set(tl, AE9_MID_SPK_VOL, 0x00, AE9_F_TWO);
	tail_mic1_config(tl);
}

/* ---- Windows' playback engine's start ---- */

/*
 * Windows' playback engine starting (CtxHda FUN_00032dc0; entries 58-80):
 * "mch" = 3 with the ASI_96KHZ flag on; the PMU bit request 0x83 (register
 * 0 bit 2 on); ASI 0; node 0x17's word's high byte 0 (Creative's 0x794;
 * mainline's AE-7 ASI set-up sends the same); XRAM 0xfa92 = 0x22, a RET over
 * the 8051 main loop's re-apply of its ASI block (mainline AE-7's
 * ae7_post_dsp_asi_setup() writes the same, uv42); the I2S pins; the ASI
 * rate class 1 (0x81: 44.1-192 kHz).
 */
static void tail_engine_start(struct ae9_cold *c)
{
	tl_ctl_param(c, AE9_CTL_PARAM_MCH, 3);
	cold_ctl_flag(c, CONTROL_FLAG_ASI_96KHZ, true);
	cold_send(c, WIDGET_CHIP_CTRL, AE9_VERB_CHIPIO_PMU_BIT, 0x83);
	tl_ctl_param(c, CONTROL_PARAM_ASI, 0);
	cold_send(c, 0x17, AE9_VERB_SET_NODE_WORD_HI, 0);
	cold_xram_write(c, 0xfa92, 0x22);
	tl_i2s_pins(c);
	cold_send(c, WIDGET_CHIP_CTRL, AE9_VERB_CHIPIO_ASI_RATE, 0x81);
}

/*
 * The engine's DSP streams (FUN_00032dc0's FUN_00034134 and FUN_00039a48(1);
 * entries 81-112): connection point 0x70 at 96 kHz; stream 0x0c (the DSP's
 * processed audio) 6 channels and on; stream 0x05 (host audio) from 0x43 to
 * point 0; stream 0x18 (the DSP's output) from 0x09 to the front DAC 0xd0,
 * that point at 96 kHz, 6 channels, its Active byte's address, on. This is
 * the one enable of stream 0x18 the start-up drain (core.c) makes room for.
 */
static void tail_engine_streams(struct ae9_cold *c)
{
	tl_conn_rate(c, 0x70, AE9_SR_96000);
	tl_stream_channels(c, AE9_STREAM_DSP_PROC, 6);
	tl_stream_on(c, AE9_STREAM_DSP_PROC);
	tl_stream_route(c, AE9_STREAM_HOST, AE9_CP_HOST, 0);
	tl_stream_route(c, AE9_STREAM_DSP_OUT, AE9_CP_DSP_OUT, AE9_CP_FRONT_DAC);
	tl_conn_rate(c, AE9_CP_FRONT_DAC, AE9_SR_96000);
	tl_stream_channels(c, AE9_STREAM_DSP_OUT, 6);
	tl_stream18_active_addr(c);
	tl_stream_on(c, AE9_STREAM_DSP_OUT);
}

/*
 * The I2S to the DACs clocked for 96 kHz (Windows' I2S set-up, FUN_000339ec
 * -> FUN_00033bac, as pb_i2s_clocks(); entries 113-156): ASI 8, three pins,
 * the three clock descriptors 0x1f101 (the 88.2/96 kHz family), the rate
 * descriptor 0x14004, 0x189028, the pins again, ASI 0xf
 */
static void tail_engine_i2s(struct ae9_cold *c)
{
	tl_ctl_param(c, CONTROL_PARAM_ASI, 8);
	cold_pll_pmu_write(c, 0x40, 0xc7);
	cold_pll_pmu_write(c, 0x42, 0xcd);
	cold_pll_pmu_write(c, 0x41, 0xce);
	cold_chipio_write(c, 0x189000, 0x0001f101);
	cold_chipio_write(c, 0x189004, 0x0001f101);
	cold_chipio_write(c, 0x189008, 0x0001f101);
	cold_chipio_write(c, 0x189024, 0x00014004);
	cold_chipio_write(c, 0x189028, 0x0002000f);
	tl_i2s_pins(c);
	tl_ctl_param(c, CONTROL_PARAM_ASI, 0x0f);
}

/*
 * The routes once more (entries 157-191): stream 0x05 from 0x43 to point 0,
 * stream 0x18 from 0x09 to 0xd0, 0xd0 at 96 kHz, 6 channels, its Active
 * byte's address; the pins; ASI 0xf
 */
static void tail_engine_routes(struct ae9_cold *c)
{
	tl_stream_route(c, AE9_STREAM_HOST, AE9_CP_HOST, 0);
	tl_stream_route(c, AE9_STREAM_DSP_OUT, AE9_CP_DSP_OUT, AE9_CP_FRONT_DAC);
	tl_conn_rate(c, AE9_CP_FRONT_DAC, AE9_SR_96000);
	tl_stream_channels(c, AE9_STREAM_DSP_OUT, 6);
	tl_stream18_active_addr(c);
	tl_i2s_pins(c);
	tl_ctl_param(c, CONTROL_PARAM_ASI, 0x0f);
}

/* ---- A stream's start and stop, with its levels ---- */

/*
 * The recording level, one channel (Windows' volume setter): ADC 0x07
 * unmuted at +3 dB, then again, each followed by the DSP's mic 1 level; the
 * XLR pin's gain
 */
static void tail_mic_channel(struct ae9_tl *tl, bool right)
{
	tl_adc_level(&tl->c, right, true);
	tl_mic_level(tl);
	tl_adc_level(&tl->c, right, false);
	tl_mic_level(tl);
	tl_xlr_gain(&tl->c, right);
}

/*
 * A playback stream starting (entries 252-308): the DSP's output unmuted
 * (CtxHda FUN_0001964c, at every pause); chip 0x18b008 bit 3 (a hold bit)
 * cleared at the stream's RUN (FUN_00022778: its read, of which only the
 * address and the post-read 0x702 are in the table, then 0xf0); converter
 * 0x02's format (24-bit, 6 channels, 48 kHz), the port map 0xa4 (speakers);
 * converter 0x02's first levels (muted, then 0 dB), each with the DSP's
 * speaker volume -28.59 dB; the bind (tag 1); converters 0x03 and 0x04's
 * first levels
 */
static void tail_stream_start(struct ae9_tl *tl)
{
	struct ae9_cold *c = &tl->c;

	tl_dsp_mute(tl, false);
	cold_chipio_addr(c, 0x18b008);
	cold_send(c, WIDGET_CHIP_CTRL, VENDOR_CHIPIO_HIC_POST_READ, 0);
	cold_chipio_write(c, 0x18b008, 0x000000f0);
	cold_send(c, AE9_NID_DAC, AC_VERB_SET_STREAM_FORMAT, 0x0035);
	tl_ctl_param(c, AE9_CTL_PARAM_DAC2PORT, 0xa4);
	tl_out_level(c, AE9_NID_DAC, false, true);
	tl_spk_vol(tl, 0xc1e4b852, 0);			/* -28.59 dB */
	tl_out_level(c, AE9_NID_DAC, true, true);
	tl_spk_vol(tl, 0xc1e4b852, 0);
	cold_send(c, AE9_NID_DAC, AC_VERB_SET_CHANNEL_STREAMID, 1 << 4);
	tl_out_0db(c, 0x03, true);
	tl_out_0db(c, 0x04, true);
}

/*
 * The stream's levels again (entries 309-374), as Windows interleaved them:
 * ADC 0x07 left with the DSP's mic level, converter 0x02's left fine step,
 * ADC 0x07 right, converter 0x02's left amp, the mic level; the XLR pin's
 * gain; the DSP's speaker volume -28.6 dB (its message's words straddled
 * the two pin verbs: the pin verbs go first, as the table sent them);
 * converter 0x02 right with it; converters 0x03 and 0x04
 */
static void tail_stream_levels(struct ae9_tl *tl)
{
	struct ae9_cold *c = &tl->c;

	tl_adc_level(c, false, false);
	tl_mic_level(tl);
	cold_send(c, AE9_NID_DAC, AE9_VERB_SET_FINE_VOL_L, 0);
	tl_adc_level(c, true, false);
	cold_send(c, AE9_NID_DAC, AC_VERB_SET_AMP_GAIN_MUTE,
		  AC_AMP_SET_OUTPUT | AC_AMP_SET_LEFT | AE9_AMP_0DB);
	tl_mic_level(tl);
	tl_xlr_gain(c, false);
	tl_xlr_gain(c, true);
	tl_spk_vol(tl, 0xc1e4cccd, 0);			/* -28.6 dB */
	tl_dac_level(tl, true, 0xc1e4cccd, 0);
	tl_out_0db(c, 0x03, false);
	tl_out_0db(c, 0x04, false);
}

/*
 * The stream stopping (entries 375-415): the DSP's output unmuted (at the
 * pause), converter 0x02 unbound; a volume change to -28.6 dB
 */
static void tail_stream_stop(struct ae9_tl *tl)
{
	tl_dsp_mute(tl, false);
	cold_send(&tl->c, AE9_NID_DAC, AC_VERB_SET_CHANNEL_STREAMID, 0);
	tl_volume(tl, 0xc1e4cccd);			/* -28.6 dB */
}

/* ---- The speaker set-up and the effects' settings ---- */

/*
 * Windows' speaker set-up (its output set-up for the speakers, the same DSP
 * settings as the output switch's, sw_dsp_output(), without X-Bass's level
 * and frequency): the DSP's output muted, the port map 0xa4 (speakers),
 * speaker config 8.0 (5.1 / Direct Mode), X-Bass off, the speaker EQ's
 * bypass attenuation 0, no speaker EQ, bass redirect off, then the output
 * unmuted and its mute read back, twice
 */
static void tl_spk_setup(struct ae9_tl *tl)
{
	tl_dsp_mute(tl, true);
	tl_ctl_param(&tl->c, AE9_CTL_PARAM_DAC2PORT, 0xa4);
	tl_set(tl, AE9_MID_MASTER, 0x04, AE9_F_EIGHT);
	tl_set(tl, AE9_MID_FX_OUT, 0x18, 0);
	tl_set(tl, AE9_MID_SPK_EQ, 0x01, 0);
	tl_set(tl, AE9_MID_FX_OUT, 0x1f, 0);
	tl_set(tl, AE9_MID_FX_OUT, 0x15, 0);
	tl_unmute_check(tl);
	tl_unmute_check(tl);
}

/*
 * Bass management: every speaker pair "small" (0x96 req 0x1a-0x1d = 1.0:
 * front, center/sub, rear, surround)
 */
static void tl_spk_small(struct ae9_tl *tl)
{
	u8 req;

	for (req = 0x1a; req <= 0x1d; req++)
		tl_set(tl, AE9_MID_FX_OUT, req, AE9_F_ONE);
}

/*
 * SBX's settings (module 0x96, Creative's names): Surround 0.92, Dialog
 * Plus 0.78, Smart Volume 0.74 in its Normal mode, Crystalizer 0.88, the
 * equalizer off with its pre-gain -1 dB (Windows' float: -0.99999982) and
 * its ten bands, bass redirect off
 */
static const struct ae9_dsp_set ae9_tail_sbx[] = {
	{ AE9_MID_FX_OUT, 0x01, 0x3f6b851f },	/* Surround level 0.92 */
	{ AE9_MID_FX_OUT, 0x03, 0x3f47ae14 },	/* Dialog Plus level 0.78 */
	{ AE9_MID_FX_OUT, 0x05, 0x3f3d70a4 },	/* Smart Volume level 0.74 */
	{ AE9_MID_FX_OUT, 0x06, 0 },		/* Smart Volume mode: Normal */
	{ AE9_MID_FX_OUT, 0x08, 0x3f6147ae },	/* Crystalizer level 0.88 */
	{ AE9_MID_FX_OUT, 0x09, 0 },		/* equalizer off */
	{ AE9_MID_FX_OUT, 0x0a, 0xbf7ffffd },	/* its pre-gain -1 dB */
	{ AE9_MID_FX_OUT, 0x0b, 0x3fd9999b },	/* band 1 (31 Hz) 1.7 dB */
	{ AE9_MID_FX_OUT, 0x0c, 0x40266667 },	/* band 2 (62 Hz) 2.6 dB */
	{ AE9_MID_FX_OUT, 0x0d, 0x40066667 },	/* band 3 (125 Hz) 2.1 dB */
	{ AE9_MID_FX_OUT, 0x0e, 0x402cccce },	/* band 4 (250 Hz) 2.7 dB */
	{ AE9_MID_FX_OUT, 0x0f, 0x3f333336 },	/* band 5 (500 Hz) 0.7 dB */
	{ AE9_MID_FX_OUT, 0x10, 0x4059999b },	/* band 6 (1 kHz) 3.4 dB */
	{ AE9_MID_FX_OUT, 0x11, 0x3f991e88 },	/* band 7 (2 kHz) 1.196 dB */
	{ AE9_MID_FX_OUT, 0x12, 0x40b33334 },	/* band 8 (4 kHz) 5.6 dB */
	{ AE9_MID_FX_OUT, 0x13, 0x40c00001 },	/* band 9 (8 kHz) 6 dB */
	{ AE9_MID_FX_OUT, 0x14, 0x40a66667 },	/* band 10 (16 kHz) 5.2 dB */
	{ AE9_MID_FX_OUT, 0x15, 0 },		/* bass redirect off */
};

/*
 * Speaker tuning, every channel neutral (module 0x96): no speaker EQ, the
 * speaker EQ's bypass attenuation 0 (0x8f req 1), no center EQ, the six
 * channels' levels (0x21-0x26: front L/R, center, LFE, rear L/R) 0 dB, their
 * polarity not inverted (0x29-0x2e), their delays 0 (0x31-0x36)
 */
static const struct ae9_dsp_set ae9_tail_spk_tuning[] = {
	{ AE9_MID_FX_OUT, 0x1f, 0 }, { AE9_MID_SPK_EQ, 0x01, 0 }, { AE9_MID_FX_OUT, 0x20, 0 },
	{ AE9_MID_FX_OUT, 0x21, 0 }, { AE9_MID_FX_OUT, 0x22, 0 }, { AE9_MID_FX_OUT, 0x23, 0 },
	{ AE9_MID_FX_OUT, 0x24, 0 }, { AE9_MID_FX_OUT, 0x25, 0 }, { AE9_MID_FX_OUT, 0x26, 0 },
	{ AE9_MID_FX_OUT, 0x29, 0 }, { AE9_MID_FX_OUT, 0x2a, 0 }, { AE9_MID_FX_OUT, 0x2b, 0 },
	{ AE9_MID_FX_OUT, 0x2c, 0 }, { AE9_MID_FX_OUT, 0x2d, 0 }, { AE9_MID_FX_OUT, 0x2e, 0 },
	{ AE9_MID_FX_OUT, 0x31, 0 }, { AE9_MID_FX_OUT, 0x32, 0 }, { AE9_MID_FX_OUT, 0x33, 0 },
	{ AE9_MID_FX_OUT, 0x34, 0 }, { AE9_MID_FX_OUT, 0x35, 0 }, { AE9_MID_FX_OUT, 0x36, 0 },
};

/*
 * CrystalVoice's settings (module 0x95, Creative's names): Voice Morphing's
 * formants 400/1400/2000 Hz, pitch, envelope and pitch variability 1.0,
 * quiver and contour depth 0, VoiceFX off, Noise Reduction strength 0, Mic
 * Smart Volume strength 0.74; the mic EQ's eight bands: gains (0, 3, -2, -4,
 * -4, -2, 5, 7 dB), centres (62 Hz-4 kHz an octave apart, then 6 kHz),
 * widths (each centre / sqrt(2), then 2000 Hz)
 */
static const struct ae9_dsp_set ae9_tail_crystalvoice[] = {
	{ AE9_MID_FX_IN, 0x0b, 0x43c80000 },	/* formant 1: 400 Hz */
	{ AE9_MID_FX_IN, 0x0c, 0x44af0000 },	/* formant 2: 1400 Hz */
	{ AE9_MID_FX_IN, 0x0d, 0x44fa0000 },	/* formant 3: 2000 Hz */
	{ AE9_MID_FX_IN, 0x0e, AE9_F_ONE },	/* pitch */
	{ AE9_MID_FX_IN, 0x0f, AE9_F_ONE },	/* envelope */
	{ AE9_MID_FX_IN, 0x10, AE9_F_ONE },	/* pitch variability */
	{ AE9_MID_FX_IN, 0x11, 0 },		/* quiver depth */
	{ AE9_MID_FX_IN, 0x12, 0 },		/* contour depth */
	{ AE9_MID_FX_IN, 0x0a, 0 },		/* VoiceFX off */
	{ AE9_MID_FX_IN, 0x05, 0 },		/* Noise Reduction strength */
	{ AE9_MID_FX_IN, 0x2d, 0x3f3d70a4 },	/* Mic Smart Volume strength 0.74 */
	{ AE9_MID_FX_IN, 0x14, 0 },		/* mic EQ band 1 gain (dB) */
	{ AE9_MID_FX_IN, 0x15, AE9_F_THREE },	/* band 2 */
	{ AE9_MID_FX_IN, 0x16, 0xc0000000 },	/* band 3: -2 */
	{ AE9_MID_FX_IN, 0x17, 0xc0800000 },	/* band 4: -4 */
	{ AE9_MID_FX_IN, 0x18, 0xc0800000 },	/* band 5: -4 */
	{ AE9_MID_FX_IN, 0x19, 0xc0000000 },	/* band 6: -2 */
	{ AE9_MID_FX_IN, 0x1a, 0x40a00000 },	/* band 7: 5 */
	{ AE9_MID_FX_IN, 0x1b, 0x40e00000 },	/* band 8: 7 */
	{ AE9_MID_FX_IN, 0x1c, 0x42780000 },	/* band 1 centre: 62 Hz */
	{ AE9_MID_FX_IN, 0x1d, 0x42fa0000 },	/* 125 Hz */
	{ AE9_MID_FX_IN, 0x1e, 0x437a0000 },	/* 250 Hz */
	{ AE9_MID_FX_IN, 0x1f, 0x43fa0000 },	/* 500 Hz */
	{ AE9_MID_FX_IN, 0x20, 0x447a0000 },	/* 1000 Hz */
	{ AE9_MID_FX_IN, 0x21, 0x44fa0000 },	/* 2000 Hz */
	{ AE9_MID_FX_IN, 0x22, 0x457a0000 },	/* 4000 Hz */
	{ AE9_MID_FX_IN, 0x23, 0x45bb8000 },	/* 6000 Hz */
	{ AE9_MID_FX_IN, 0x24, 0x42300000 },	/* band 1 width: 44 Hz */
	{ AE9_MID_FX_IN, 0x25, 0x42b00000 },	/* 88 Hz */
	{ AE9_MID_FX_IN, 0x26, 0x43300000 },	/* 176 Hz */
	{ AE9_MID_FX_IN, 0x27, 0x43b08000 },	/* 353 Hz */
	{ AE9_MID_FX_IN, 0x28, 0x4430c000 },	/* 707 Hz */
	{ AE9_MID_FX_IN, 0x29, 0x44b0c000 },	/* 1414 Hz */
	{ AE9_MID_FX_IN, 0x2a, 0x4530c000 },	/* 2828 Hz */
	{ AE9_MID_FX_IN, 0x2b, 0x44fa0000 },	/* 2000 Hz */
};

/*
 * CrystalVoice's switches off: VoiceFX, Noise Reduction, Voice Clarity's
 * chain (module 0x47), Echo Cancellation, Mic Smart Volume, the mic EQ
 */
static const struct ae9_dsp_set ae9_tail_crystalvoice_off[] = {
	{ AE9_MID_FX_IN, 0x0a, 0 }, { AE9_MID_FX_IN, 0x04, 0 }, { 0x47, 0x00, 0 },
	{ AE9_MID_FX_IN, 0x00, 0 }, { AE9_MID_FX_IN, 0x2c, 0 }, { AE9_MID_FX_IN, 0x13, 0 },
};

/* SBX's switches: Surround, Dialog Plus off, Smart Volume off, Crystalizer */
static void tl_sbx_switches(struct ae9_tl *tl, bool surround, bool crystalizer)
{
	tl_set(tl, AE9_MID_FX_OUT, 0x00, surround ? AE9_F_ONE : 0);
	tl_set(tl, AE9_MID_FX_OUT, 0x02, 0);
	tl_set(tl, AE9_MID_FX_OUT, 0x04, 0);
	tl_set(tl, AE9_MID_FX_OUT, 0x07, crystalizer ? AE9_F_ONE : 0);
}

/*
 * The speaker set-up twice, each with the speakers small (entries 416-543),
 * then the effects' settings (Windows' saved values, sent at its start:
 * entries 544-952): connection point 0x03 selected (a rate read, not in the
 * table); SBX's settings; the bass redirect crossover 80 Hz (0x96 req 0x16:
 * dropped); the speakers small; the subwoofer gain (0x96 req 0x1e = 0:
 * dropped); speaker tuning; CrystalVoice's settings; the speaker set-up; the
 * effects' switches off: SBX's, CrystalVoice's
 */
static void tail_effects(struct ae9_tl *tl)
{
	tl_spk_setup(tl);
	tl_spk_small(tl);
	tl_spk_setup(tl);
	tl_spk_small(tl);
	tl_ctl_param(&tl->c, CONTROL_PARAM_CONN_POINT_ID, 0x03);
	tl_set_list(tl, ae9_tail_sbx, ARRAY_SIZE(ae9_tail_sbx));
	tl_dropped(tl);			/* 0x96 req 0x16 = 80.0: bass redirect crossover (Hz) */
	tl_spk_small(tl);
	tl_dropped(tl);			/* 0x96 req 0x1e = 0: subwoofer gain */
	tl_set_list(tl, ae9_tail_spk_tuning, ARRAY_SIZE(ae9_tail_spk_tuning));
	tl_set_list(tl, ae9_tail_crystalvoice, ARRAY_SIZE(ae9_tail_crystalvoice));
	tl_spk_setup(tl);
	tl_sbx_switches(tl, false, false);
	tl_set_list(tl, ae9_tail_crystalvoice_off, ARRAY_SIZE(ae9_tail_crystalvoice_off));
}

/* ---- What the user did while the capture ran ---- */

/*
 * The volume changed seven times (entries 953-1204: -28.6, -25.6, -24.6,
 * -24.1, -25.6, -27.1, -28.1 dB), the speaker set-up, SBX's Surround and
 * Crystalizer on (1205-1268); two volume changes to -29.1 dB, the first
 * with the right channel's update 1.0, the second with the left's
 * (1269-1340); the speaker set-up, SBX off (1341-1404); the volume to -29.1
 * / -29.6 dB, to -31.6 dB (its converter 0x03 left only), again -31.6 dB
 * (1405-1506)
 */
static void tail_user_volume_sbx(struct ae9_tl *tl)
{
	static const u32 vol[] = {
		0xc1e4cccd, 0xc1cccccd, 0xc1c4cccd, 0xc1c0cccd,	/* -28.6, -25.6, -24.6, -24.1 */
		0xc1cccccd, 0xc1d8cccd, 0xc1e0cccd,		/* -25.6, -27.1, -28.1 */
	};
	struct ae9_cold *c = &tl->c;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vol); i++)
		tl_volume(tl, vol[i]);
	tl_spk_setup(tl);
	tl_sbx_switches(tl, true, true);

	tl_dac_level(tl, false, 0xc1e8cccd, 0);			/* -29.1 dB */
	tl_dac_level(tl, true, 0xc1e8cccd, AE9_F_ONE);
	tl_out_0db(c, 0x03, false);
	tl_out_0db(c, 0x04, false);
	tl_dac_level(tl, false, 0xc1e8cccd, AE9_F_ONE);
	tl_dac_level(tl, true, 0xc1e8cccd, 0);
	tl_out_0db(c, 0x03, false);
	tl_out_0db(c, 0x04, false);
	tl_spk_setup(tl);
	tl_sbx_switches(tl, false, false);

	tl_dac_level(tl, false, 0xc1e8cccd, 0);			/* -29.1 dB */
	tl_dac_level(tl, true, 0xc1eccccd, 0);			/* -29.6 dB */
	tl_out_0db(c, 0x03, false);
	tl_out_0db(c, 0x04, false);
	tl_dac_level(tl, false, 0xc1fccccd, 0);			/* -31.6 dB */
	tl_dac_level(tl, true, 0xc1fccccd, 0);
	tl_out_level(c, 0x03, false, false);
	tl_volume(tl, 0xc1fccccd);
}

/*
 * The output switched to the headphones (entries 1507-1678; Creative's
 * software, as creative_ae9_out_switch() has it from a later capture): the
 * DSP's output muted, the port map 0xa1, a volume change to -24.5 dB; the
 * line out's pins off (0x0b's EAPD and pin, 0x0f), pin 0x10 out with its
 * headphone amp; each channel's speaker level and delay 0 (front L/R,
 * center, LFE, rear L/R), then the surround pair's level and delay (0x96 req
 * 0x27, 0x37, 0x28, 0x38: dropped); speaker config 0 (headphones), X-Bass
 * level 0.5, 80 Hz, off; the speaker EQ's bypass attenuation 0, no speaker
 * EQ, bass redirect off; the output unmuted and read back, twice
 */
static void tail_user_to_hp(struct ae9_tl *tl)
{
	struct ae9_cold *c = &tl->c;
	u8 ch;

	tl_dsp_mute(tl, true);
	tl_ctl_param(c, AE9_CTL_PARAM_DAC2PORT, 0xa1);
	tl_volume(tl, 0xc1c40000);				/* -24.5 dB */
	cold_send(c, 0x0b, AC_VERB_SET_EAPD_BTLENABLE, 0);
	cold_send(c, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN);
	for (ch = 0; ch < 6; ch++) {
		tl_set(tl, AE9_MID_FX_OUT, 0x21 + ch, 0);
		tl_set(tl, AE9_MID_FX_OUT, 0x31 + ch, 0);
	}
	tl_dropped(tl);			/* 0x96 req 0x27 = 0: surround L level */
	tl_dropped(tl);			/* 0x96 req 0x37 = 0: surround L delay */
	tl_dropped(tl);			/* 0x96 req 0x28 = 0: surround R level */
	tl_dropped(tl);			/* 0x96 req 0x38 = 0: surround R delay */
	tl_set(tl, AE9_MID_MASTER, 0x04, 0);
	tl_set(tl, AE9_MID_FX_OUT, 0x19, AE9_F_HALF);
	tl_set(tl, AE9_MID_FX_OUT, 0x17, AE9_F_EIGHTY);
	tl_set(tl, AE9_MID_FX_OUT, 0x18, 0);
	tl_set(tl, AE9_MID_SPK_EQ, 0x01, 0);
	tl_set(tl, AE9_MID_FX_OUT, 0x1f, 0);
	tl_set(tl, AE9_MID_FX_OUT, 0x15, 0);
	tl_unmute_check(tl);
	tl_unmute_check(tl);
}

/*
 * And back to the speakers (entries 1679-1830): the DSP's output muted, the
 * port map 0xa4; the line out's pins (pin 0x0f out), pin 0x10 out; the six
 * channels' levels 0, their delays 0; speaker config 8.0, X-Bass off, the
 * speaker EQ's bypass attenuation 0, no speaker EQ, bass redirect off; the
 * output unmuted and read back; a volume change to -32.1 dB, with an unmute
 * and its read-back between converter 0x03's channels; the speakers small
 */
static void tail_user_to_sp(struct ae9_tl *tl)
{
	struct ae9_cold *c = &tl->c;
	u8 ch;

	tl_dsp_mute(tl, true);
	tl_ctl_param(c, AE9_CTL_PARAM_DAC2PORT, 0xa4);
	cold_send(c, 0x0b, AC_VERB_SET_EAPD_BTLENABLE, 0);
	cold_send(c, 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0);
	cold_send(c, 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN);
	cold_send(c, 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, AC_PINCTL_HP_EN | AC_PINCTL_OUT_EN);
	for (ch = 0; ch < 6; ch++)
		tl_set(tl, AE9_MID_FX_OUT, 0x21 + ch, 0);
	for (ch = 0; ch < 6; ch++)
		tl_set(tl, AE9_MID_FX_OUT, 0x31 + ch, 0);
	tl_set(tl, AE9_MID_MASTER, 0x04, AE9_F_EIGHT);
	tl_set(tl, AE9_MID_FX_OUT, 0x18, 0);
	tl_set(tl, AE9_MID_SPK_EQ, 0x01, 0);
	tl_set(tl, AE9_MID_FX_OUT, 0x1f, 0);
	tl_set(tl, AE9_MID_FX_OUT, 0x15, 0);
	tl_unmute_check(tl);

	tl_dac_level(tl, false, 0xc2006666, 0);			/* -32.1 dB */
	tl_dac_level(tl, true, 0xc2006666, 0);
	tl_out_level(c, 0x03, false, false);
	tl_unmute_check(tl);
	tl_out_level(c, 0x03, true, false);
	tl_out_0db(c, 0x04, false);
	tl_spk_small(tl);
}

/*
 * Windows' set-up after its DSP's boot handshake (the start-up "tail"):
 * from creative_ae9_post_fw() (core.c) at every bring-up that loaded the
 * firmware (probe and its retries, resume), process context, sleeps (the
 * DSP messages wait on the DSP's queue). Fills *t: the counts for the
 * summary line (a good load: 340 DSP messages, 6 of them dropped, and 417
 * verbs) and the messages for creative_ae9_tail_resend(), which frees the
 * list. The ChipIO address cache is invalidated at the end.
 */
void creative_ae9_tail(struct creative_chip *chip, struct ae9_tail *t)
{
	struct ae9_tl tl = { .c.chip = chip, .t = t };

	*t = (struct ae9_tail){
		.retry = kcalloc(AE9_TAIL_RETRY_MAX, sizeof(*t->retry), GFP_KERNEL),
	};
	tail_dsp_settings(&tl);
	tail_engine_start(&tl.c);
	tail_engine_streams(&tl.c);
	tail_engine_i2s(&tl.c);
	tail_engine_routes(&tl.c);
	tail_mic_channel(&tl, false);	/* entries 192-221 */
	tail_mic_channel(&tl, true);	/* entries 222-251 */
	tail_stream_start(&tl);
	tail_stream_levels(&tl);
	tail_stream_stop(&tl);
	tail_effects(&tl);
	tail_user_volume_sbx(&tl);
	tail_user_to_hp(&tl);
	tail_user_to_sp(&tl);
	creative_dsp_chipio_addr_invalidate();
	t->verbs = tl.c.verbs;
	t->verb_err = tl.c.errs;
}

/* The tail's failed and skipped DSP messages sent once more; the list freed */
void creative_ae9_tail_resend(struct creative_chip *chip, struct ae9_tail *t)
{
	unsigned int k, ok = 0;

	if (t->nretry) {
		for (k = 0; k < t->nretry; k++)
			if (creative_dsp_scp_send(chip, t->retry[k].hdr, &t->retry[k].val, 1) == 0)
				ok++;
		dev_info(chip->dev, "fw_split: resent %u failed tail SCP msgs, %u went through\n",
			 t->nretry, ok);
	}
	kfree(t->retry);
	t->retry = NULL;
}
