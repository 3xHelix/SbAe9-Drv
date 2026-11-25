// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 3xHelix
//
// Sound Blaster AE-7 (experimental): playback on stream
// descriptor 4 (tag 1, converter 0x02), on creative_ae9_pcm.c's stream
// helpers and pointer; since kit 10 recording from the Line-in / Mic-in
// jack (input stream descriptor 0, tag 1, ADC 0x07: the AE-9's capture)
// and What U Hear (PCM device 2: the AE-9's). #included at the end of
// creative_ae9_pcm.c.
//
// The channels: stereo (format 0x31: 48 kHz, 24 bits in 32) at level 2;
// 6 channels (0x35, Windows' DSP-mode format) at level 3; at level 4
// stereo, or 6 channels in DSP mode (kit 10, for the Rear and C/Sub
// jacks). On the AE-9 a DSP-mode stream with another channel count than
// the one before played static on the front until the DSP path was
// rebuilt (2026-11-04), and the AE-7 shares that DSP and router chip: a
// count change rebuilds the AE-7's DSP path, muted (core.c,
// ae7_dsp_rebuild()). Kit 5 played both on the AE-7, each in its own load,
// equally loud. DSP mode is 48 kHz; Direct Mode
// (level 4: 44.1/48/88.2/96/192/384 kHz) plays the stream's own
// rate, stereo (creative_ae7_play_start(), core.c). 176.4 and 352.8 kHz are
// never offered: ALSA's constraints refuse them (an app asking "near" gets
// 192 or 384 kHz), and hw_params checks the rate against the board's table
// again (creative_ae7_rate_row(): -EINVAL, logged).
//

#include "creative_ae7.h"

/*
 * The rates an open offers: 48 kHz in DSP mode; with the Direct Mode
 * switch on at the open (level 4) the Direct Mode rates. A
 * stream opened in DSP mode plays 48 kHz on either path.
 */
static unsigned int ae7_pcm_rates(struct creative_chip *chip, unsigned int *min, unsigned int *max)
{
	static const struct { unsigned int rate, bit; } r[] = {
		{ 44100, SNDRV_PCM_RATE_44100 }, { 48000, SNDRV_PCM_RATE_48000 },
		{ 88200, SNDRV_PCM_RATE_88200 }, { 96000, SNDRV_PCM_RATE_96000 },
		{ 192000, SNDRV_PCM_RATE_192000 }, { 384000, SNDRV_PCM_RATE_384000 },
	};
	bool direct = ae9_ae7_mode >= AE7_MODE_DIRECT && READ_ONCE(chip->ae7.direct_want);
	unsigned int i, bits = 0;

	*min = 48000;
	*max = 48000;
	for (i = 0; i < ARRAY_SIZE(r); i++) {
		if (r[i].rate != 48000 && !direct)
			continue;
		if (!creative_ae7_rate_row(r[i].rate, ae9_ae7_mode))
			continue;
		bits |= r[i].bit;
		*min = min(*min, r[i].rate);
		*max = max(*max, r[i].rate);
	}
	return bits;
}

/*
 * The channels a stream may play: the level's (2 at level 2, 6 at level
 * 3); at level 4 2, or 6 in DSP mode (kit 10, the Rear and C/Sub
 * jacks: 48 kHz with the Direct Mode switch off at the open; Direct Mode
 * is stereo)
 */
static bool ae7_pcm_ch_ok(struct creative_chip *chip, unsigned int rate, unsigned int ch)
{
	if (ae9_ae7_mode < AE7_MODE_DIRECT)
		return ch == chip->ae7.ch;
	return ch == 2 || (ch == 6 && rate == 48000);
}

/* the stream must be ready: the DSP up (not during a resume's bring-up), the card answering */
static int ae7_pcm_ready(struct creative_chip *chip)
{
	if (creative_ae9_dead(chip) || READ_ONCE(chip->unloading))
		return -ENODEV;
	if (READ_ONCE(chip->suspended) || !READ_ONCE(chip->dsp_done))
		return -EBUSY;
	return 0;
}

/*
 * DMA fencing (s3boun3t's AE-7 driver): a stream descriptor that doesn't
 * stop could still fetch from the buffer ALSA is about to free or reuse.
 * Then bus mastering goes off and the card counts as stopped answering,
 * before the pages go (a reload brings it back).
 */
static int ae7_pcm_fence(struct creative_chip *chip, const char *why)
{
	if (chip->card_gone || ae9_sd_wait_stopped(chip, chip->playback_stream_base, why))
		return 0;
	pci_clear_master(chip->pci);
	WRITE_ONCE(chip->card_hung, true);
	creative_ae9_dead_notify(chip);
	dev_err(chip->dev, "AE-7: playback DMA didn't stop (%s): bus mastering off; reload the driver\n",
		why);
	return -EIO;
}

static int creative_ae7_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	int err = ae7_pcm_ready(chip);

	if (err < 0)
		return err;
	runtime->hw.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
			   SNDRV_PCM_INFO_INTERLEAVED;
	runtime->hw.formats = SNDRV_PCM_FMTBIT_S32_LE;
	runtime->hw.rates = ae7_pcm_rates(chip, &runtime->hw.rate_min, &runtime->hw.rate_max);
	runtime->hw.channels_min = chip->ae7.ch;
	runtime->hw.channels_max = chip->ae7.ch;
	/* level 4: 2 or 6 channels in DSP mode (the open offers 48 kHz only then) */
	if (ae9_ae7_mode >= AE7_MODE_DIRECT && !READ_ONCE(chip->ae7.direct_want)) {
		static const unsigned int chs[] = { 2, 6 };
		static const struct snd_pcm_hw_constraint_list ch_list = {
			.count = ARRAY_SIZE(chs),
			.list = chs,
		};

		runtime->hw.channels_max = 6;
		err = snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS, &ch_list);
		if (err < 0)
			return err;
	}
	runtime->hw.buffer_bytes_max = CREATIVE_PCM_BUF_MAX;
	runtime->hw.period_bytes_min = 4096;
	runtime->hw.period_bytes_max = 128 * 1024;
	runtime->hw.periods_min = 2;
	runtime->hw.periods_max = clamp(pcm_periods, 2U, (unsigned int)CREATIVE_PCM_BDL_MAX);
	snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 128);
	snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 128);
	err = ae9_whole_periods(runtime, 128, true);
	if (err < 0)
		return err;
	chip->pcm_bits = 24;
	snd_pcm_hw_constraint_msbits(runtime, 0, 32, 24);
	chip->pcm_start_sent = 0;
	chip->substream = substream;
	chip->playback_substream = substream;
	return 0;
}

static int creative_ae7_pcm_close(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);

	chip->substream = NULL;
	chip->playback_substream = NULL;
	return 0;
}

/*
 * hw_params: the rate and channels checked against the option once more
 * (ALSA's constraints from the open already refuse the others: this only
 * catches a stream whose open offered another set), then the DMA fence and
 * the AE-9's buffer set-up
 */
static int creative_ae7_pcm_hw_params(struct snd_pcm_substream *substream,
				      struct snd_pcm_hw_params *hw_params)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	unsigned int rate = params_rate(hw_params), ch = params_channels(hw_params);
	int err;

	if (!creative_ae7_rate_row(rate, ae9_ae7_mode) || !ae7_pcm_ch_ok(chip, rate, ch)) {
		dev_info_ratelimited(chip->dev, "AE-7: playback at %u Hz, %u channels refused (%s; rates: see the open's)\n",
				     rate, ch,
				     ae9_ae7_mode >= AE7_MODE_DIRECT ? "2 channels, or 6 at 48 kHz in DSP mode" :
				     chip->ae7.ch == 6 ? "6 channels" : "2 channels");
		return -EINVAL;
	}
	err = ae7_pcm_fence(chip, "AE-7 playback hw_params");
	return err < 0 ? err : creative_ae9_pcm_hw_params(substream, hw_params);
}

/*
 * The release: the start's stop (converter 0x02 unbound, Windows at a
 * stream's stop; in Direct Mode stream 0x14 stopped), the BDL and pages
 * freed
 */
static int creative_ae7_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	int err = ae7_pcm_fence(chip, "AE-7 playback hw_free");

	chip->pb_format = 0;
	if (!err && !ae7_pcm_ready(chip))
		creative_ae7_play_stop(chip);
	chip->pcm_start_sent = 0;
	if (chip->playback_bdl_raw) {
		if (!err && ae9_bdl_may_free(chip, chip->playback_stream_base, chip->playback_bdl_dma,
					     "AE-7 playback hw_free"))
			dma_free_coherent(chip->dev, chip->playback_bdl_size,
					  chip->playback_bdl_raw, chip->playback_bdl_dma);
		chip->playback_bdl_raw = NULL;
		chip->playback_bdl = NULL;
	}
	snd_pcm_lib_free_pages(substream);
	return err;
}

/*
 * The stream descriptor programmed as the AE-9's prepare does (stream
 * reset, one BDL entry per period with its interrupt, tag 1, length, last
 * index, format, BDL address), then once per open, rate and format the
 * stream's start on its path (creative_ae7_play_start(): DSP mode, or
 * Direct Mode at level 4; a path or rate switch mutes the DAC first, the
 * gate unmutes it: at the start's end when nothing else is pending and the
 * DAC has locked, before the trigger, else the heartbeat's). A start the card refused (the DAC's mute
 * not confirmed) fails the prepare: nothing was switched.
 *
 * The DMA FIFO readout (ae9_fifo_*() above, debugfs only): the format
 * noted here, SDnFIFOS read at the trigger's START, as the AE-9's main
 * playback, so debugfs state's "DMA FIFO" line and "readahead" show the
 * AE-7's playback too. The pointer is the AE-9's (creative_ae9_pcm_pointer():
 * LPIB).
 */
static int creative_ae7_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int base = chip->playback_stream_base;
	unsigned int buf_size = snd_pcm_lib_buffer_bytes(substream);
	unsigned int period_size = snd_pcm_lib_period_bytes(substream);
	u16 format = creative_ae9_stream_format(runtime->channels, 24, runtime->rate);
	dma_addr_t addr;
	u32 ctl;
	int i, err;

	err = ae7_pcm_ready(chip);
	if (err < 0)
		return err;
	err = ae7_pcm_fence(chip, "AE-7 playback prepare");
	if (err < 0)
		return err;
	creative_ae9_stream_reset(chip, base);

	memset(chip->playback_bdl, 0, CREATIVE_PCM_BDL_SIZE);
	addr = runtime->dma_addr;
	for (i = 0; i < runtime->periods; i++) {
		__le64 *bdl_addr = chip->playback_bdl + i * 16;
		__le32 *bdl_len = chip->playback_bdl + i * 16 + 8;
		__le32 *bdl_ioc = chip->playback_bdl + i * 16 + 12;

		*bdl_addr = cpu_to_le64(addr);
		*bdl_len = cpu_to_le32(period_size);
		*bdl_ioc = cpu_to_le32(runtime->no_period_wakeup ? 0 : 1);
		addr += period_size;
	}
	wmb();

	ctl = readl(chip->hda_regs + base + AZX_REG_SD_CTL);
	ctl &= ~(0xf << 20);
	ctl |= (CREATIVE_PCM_STREAM_TAG & 0xf) << 20;
	writel(ctl & SD_CTL_WMASK, chip->hda_regs + base + AZX_REG_SD_CTL);
	wmb();
	creative_hda_writel(chip, base + AZX_REG_SD_CBL, buf_size);
	creative_hda_writew(chip, base + AZX_REG_SD_LVI, runtime->periods - 1);
	creative_hda_writew(chip, base + AZX_REG_SD_FORMAT, format);
	ae9_fifo_prepare(&chip->pb_fifo, format, frames_to_bytes(runtime, 1), runtime->rate);
	creative_hda_writel(chip, base + AZX_REG_SD_BDLPL, lower_32_bits(chip->playback_bdl_dma));
	creative_hda_writel(chip, base + AZX_REG_SD_BDLPU, upper_32_bits(chip->playback_bdl_dma));
	ae9_dma_trace_prep(chip, "AE-7 playback", base, substream, chip->playback_bdl_dma);

	if (!chip->pcm_start_sent || chip->pcm_start_fmt != format ||
	    chip->pcm_start_rate != runtime->rate) {
		err = creative_ae7_play_start(chip, runtime->rate, format);
		if (err < 0)
			return err;
		chip->pcm_start_sent = 1;
		chip->pcm_start_rate = runtime->rate;
		chip->pcm_start_fmt = format;
	}
	chip->pb_format = format;
	return 0;
}

static int creative_ae7_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	unsigned int base = chip->playback_stream_base;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (creative_ae9_dead(chip) || !READ_ONCE(chip->pb_format))
			return -ENODEV;
		creative_ae9_stream_start(chip, base);
		if (cmd == SNDRV_PCM_TRIGGER_START)
			ae9_fifo_start(chip, &chip->pb_fifo, base);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		creative_ae9_stream_stop(chip, base);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct snd_pcm_ops creative_ae7_pcm_ops = {
	.open		= creative_ae7_pcm_open,
	.close		= creative_ae7_pcm_close,
	.hw_params	= creative_ae7_pcm_hw_params,
	.hw_free	= creative_ae7_pcm_hw_free,
	.prepare	= creative_ae7_pcm_prepare,
	.trigger	= creative_ae7_pcm_trigger,
	.pointer	= creative_ae9_pcm_pointer,
};

/*
 * Recording (kit 10): the Line-in / Mic-in jack (pin 0x12, its only
 * connection ADC 0x07; the Audio Control Module's mic lead plugs in there
 * too), Windows' AE-7 recording verb for verb (W3 part 1, 32 starts): the
 * AE-9's capture as it is (creative_ae9_cap_ops: input stream 0, tag 1;
 * creative_ae9_cap_start(): the level block once per DSP load, then the
 * user's level, the DSP's output unmuted, ADC 0x07's format, the VIP source
 * read, the bind; the stop: unmuted, unbound, read). Windows' AE-7 level
 * block has the AE-9's steps and ends at the user's level; the AE-9's ends
 * where Windows' AE-7 slider at 100 % sets it (left +9 dB, right +8.98 dB,
 * W3 part 2: "Capture Volume" 99, the default), and a lower level is sent
 * after it. The rate is ADC 0x07's format word only:
 * 44.1, 48 and 96 kHz as Windows recorded them; 16 and 88.2 kHz offered
 * untested (both in ADC 0x07's rate list, param 0x0a = 0x000a01e4).
 * Kit 11, CrystalVoice (the AE-9's code: Windows' AE-7 sends the AE-9's
 * messages, W3 part 2): an effect on at the open offers 48 kHz only, as
 * the AE-9 (Windows records 0x31 with it); the prepare routes the voice
 * processor in before the bind, the release out ~30 ms after the unbind
 * (creative_ae9_cap_start(), creative_ae9_cap_hw_free()), both under
 * path_mutex, as a control's change (creative_ae9_cv_apply(), the fx_lock
 * op). No mic monitor (AE9_CAP_MIC_MON): nothing recorded reaches an
 * output.
 */
#define AE7_CAP_RATES	(SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 | \
			 SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000)

static int creative_ae7_cap_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	int err = ae7_pcm_ready(chip);

	if (err < 0)
		return err;
	err = creative_ae9_cap_open(substream);
	if (err < 0 || AE9_CV_WANT(chip))
		return err;
	runtime->hw.rates = AE7_CAP_RATES;
	runtime->hw.rate_min = 16000;
	runtime->hw.rate_max = 96000;
	return 0;
}

/*
 * The record start and stop send the DSP's output unmute (0x96/0x3a = 0);
 * under ae7.path_mutex it never lands inside an output switch's own mute
 * (creative_ae7_select() from the heartbeat, which holds it). The prepare:
 * the AE-9's (its start block once per open), with the DSP up.
 */
static int creative_ae7_cap_prepare(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	int err = ae7_pcm_ready(chip);

	if (err < 0)
		return err;
	mutex_lock(&chip->ae7.path_mutex);
	err = creative_ae9_cap_prepare(substream);
	mutex_unlock(&chip->ae7.path_mutex);
	return err;
}

/* the release: the AE-9's (the stop block when a start was sent) */
static int creative_ae7_cap_hw_free(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	int err;

	mutex_lock(&chip->ae7.path_mutex);
	err = creative_ae9_cap_hw_free(substream);
	mutex_unlock(&chip->ae7.path_mutex);
	return err;
}

static const struct snd_pcm_ops creative_ae7_cap_ops = {
	.open		= creative_ae7_cap_open,
	.close		= creative_ae9_cap_close,
	.hw_params	= creative_ae9_cap_hw_params,
	.hw_free	= creative_ae7_cap_hw_free,
	.prepare	= creative_ae7_cap_prepare,
	.trigger	= creative_ae9_cap_trigger,
	.pointer	= creative_ae9_cap_pointer,
};

/*
 * What U Hear (kit 10): PCM device 2, as the AE-9's (hw:AE7,2), on the
 * AE-9's extra-capture code and row (ae9_xcaps[AE9_XCAP_WUH]: codec 1's
 * ADC 0x0a, input stream 2, tag 3). Windows' AE-7 What U Hear (W3 part 2,
 * seven streams, DSP and Direct Mode): the DSP's output unmuted (0x96/0x3a
 * = 0), ADC 0x0a's format, the bind; the stop: unmuted, unbound. No VIP
 * read, no router, ChipIO, connection-point or DSP set-up in either mode.
 * 44.1 and 48 kHz (both recorded on the AE-7), stereo. In DSP mode it is
 * the DSP's output after SBX (Windows' with Smart Volume on: +13.5 dB on a
 * -30 dBFS tone); in Direct Mode the stream as played, level-exact (W3.20:
 * -30.1 dBFS for -30).
 */
static int creative_ae7_wuh_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int err = ae7_pcm_ready(snd_pcm_substream_chip(substream));

	if (err < 0)
		return err;
	err = creative_ae9_xcap_open(substream);
	if (err < 0)
		return err;
	runtime->hw.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000;
	runtime->hw.rate_min = 44100;
	runtime->hw.rate_max = 48000;
	runtime->hw.channels_max = 2;
	return 0;
}

/* the DSP's output unmuted, as the recording's (under path_mutex: creative_ae7_cap_prepare()) */
static void ae7_wuh_unmute(struct creative_chip *chip)
{
	mutex_lock(&chip->ae7.path_mutex);
	creative_ae7_stream_unmute(chip);
	mutex_unlock(&chip->ae7.path_mutex);
}

/* Windows' start: the DSP's output unmuted, then the AE-9's (ADC 0x0a's format, the bind) */
static int creative_ae7_wuh_prepare(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	int err = ae7_pcm_ready(chip);

	if (err < 0)
		return err;
	ae7_wuh_unmute(chip);
	return creative_ae9_xcap_prepare(substream);
}

/* Windows' stop: the DSP's output unmuted, then the AE-9's release (ADC 0x0a unbound) */
static int creative_ae7_wuh_hw_free(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);

	if (!ae7_pcm_ready(chip))
		ae7_wuh_unmute(chip);
	return creative_ae9_xcap_hw_free(substream);
}

static const struct snd_pcm_ops creative_ae7_wuh_ops = {
	.open		= creative_ae7_wuh_open,
	.close		= creative_ae9_xcap_close,
	.hw_params	= creative_ae9_xcap_hw_params,
	.hw_free	= creative_ae7_wuh_hw_free,
	.prepare	= creative_ae7_wuh_prepare,
	.trigger	= creative_ae9_xcap_trigger,
	.pointer	= creative_ae9_xcap_pointer,
};

/* PCM device 2, What U Hear (creative_ae9_xcap_new()'s, with the AE-7's name and ops) */
static void creative_ae7_wuh_new(struct creative_chip *chip)
{
	struct snd_pcm *pcm;
	int ret;

	ret = snd_pcm_new(chip->card, "AE-7 What U Hear", AE9_XCAP_WUH + 1, 0, 1, &pcm);
	if (ret < 0) {
		dev_warn(chip->dev, "AE-7: What U Hear device not added (%d)\n", ret);
		return;
	}
	pcm->private_data = chip;
	ae9_pcm_track(chip, pcm);
	pcm->private_free = creative_ae9_xcap_pcm_free;
	chip->xcap[AE9_XCAP_WUH].chip = chip;
	strscpy(pcm->name, "AE-7 What U Hear", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &creative_ae7_wuh_ops);
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV, chip->dev,
					      ae9_dma_size(chip, CREATIVE_PCM_BUF_MAX),
					      ae9_dma_size(chip, CREATIVE_PCM_BUF_MAX));
	dev_info(chip->dev, "AE-7: PCM device 2: What U Hear (ADC 0x0a) at 44.1/48 kHz\n");
}

/*
 * The AE-7's PCM devices (creative_ae9_pcm_init()'s, the board's pcm_init
 * op): device 0, playback and (kit 10) recording, with the AE-9's
 * "Capture Bit Depth"; device 2, What U Hear (kit 10). Optical out, DSD
 * and 5.1 on the Rear and Center / Subwoofer jacks are later phases
 * (docs/dev/AE7.md).
 */
int creative_ae7_pcm_init(struct creative_chip *chip)
{
	struct snd_pcm *pcm;
	int ret;

	ret = snd_pcm_new(chip->card, "AE-7 Analog", 0, 1, 1, &pcm);
	if (ret < 0) {
		dev_err(chip->dev, "snd_pcm_new failed: %d\n", ret);
		return ret;
	}
	pcm->private_data = chip;
	ae9_pcm_track(chip, pcm);
	strscpy(pcm->name, "AE-7 Analog", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &creative_ae7_pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &creative_ae7_cap_ops);
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV, chip->dev,
					      ae9_dma_size(chip, CREATIVE_PCM_BUF_MAX),
					      ae9_dma_size(chip, CREATIVE_PCM_BUF_MAX));
	chip->pcm = pcm;
	pcm->private_free = creative_ae9_pcm_free;
	ret = snd_pcm_add_chmap_ctls(pcm, SNDRV_PCM_STREAM_PLAYBACK, ae9_chmaps, 6, 0, NULL);
	if (ret < 0)
		dev_warn(chip->dev, "channel map control not added (%d)\n", ret);
	ret = snd_ctl_add(chip->card, snd_ctl_new1(&cap_bits_control, chip));
	if (ret < 0)
		dev_warn(chip->dev, "Capture Bit Depth control not added (%d)\n", ret);
	dev_info(chip->dev, "AE-7: PCM device 0: recording from Line-in / Mic-in (ADC 0x07) at 44.1/48/96 kHz (16 and 88.2 kHz: test)\n");
	dev_info(chip->dev, "AE-7: PCM device 0: playback, %s, DSP mode at 48 kHz%s\n",
		 ae9_ae7_mode >= AE7_MODE_DIRECT ? "2 or 6 channels (6: DSP mode)" :
		 chip->ae7.ch == 6 ? "6 channels" : "2 channels",
		 ae9_ae7_mode >= AE7_MODE_DIRECT ?
		 "; Direct Mode at 44.1/48/88.2/96/192/384 kHz" : "");
	creative_ae7_wuh_new(chip);
	return 0;
}
