// SPDX-License-Identifier: GPL-2.0-only
//
// ALSA PCM device for Creative AE-9 HDA controller
//
// Standard HDA output stream 0 (SDO0) at BAR0+0x100 with BDL DMA.
// Creative extension registers at BAR2 enable stream routing.
//
// Reference: ae9_mmio_capture.txt lines 11030+, docs/history/Creative.md §5
//

#include <linux/dma-mapping.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/hda_register.h>

#include "creative_ae9.h"

/* Per the 0x0071 verb capture: Windows uses stream descriptor index 4
 * (BAR0+0x100) with stream tag 1, bound to DAC node 0x02 on codec 1.
 */
#define CREATIVE_PCM_STREAM_TAG	1
#define CREATIVE_PCM_STREAM_BASE	0x100	/* stream descriptor index 4 */
#define CREATIVE_PCM_CODEC_ADDR	1	/* main codec */
#define CREATIVE_PCM_DAC_NID	0x02	/* front DSP DAC */
#define CREATIVE_PCM_BDL_ENTRIES	2	/* double-buffer */
#define CREATIVE_PCM_BDL_SIZE		(CREATIVE_PCM_BDL_ENTRIES * 16)

/*
 * Build the 16-bit SDnFMT value (HD-Audio spec 3.7.1, same as the codec's
 * converter format):
 * Bit 14    : base rate (0 = 48 kHz, 1 = 44.1 kHz)
 * Bits 13:11: base multiplier minus 1 (x1..x4)
 * Bits 10:8 : base divisor minus 1 (/1../8)
 * Bits 6:4  : bits per sample (1 = 16, 2 = 20, 3 = 24, 4 = 32)
 * Bits 3:0  : number of channels minus 1
 * 2 ch, 24-bit, 48 kHz is 0x0031. The PCM only offers 48 kHz today; the
 * other rates are here for when the per-rate clock setup is added.
 */
static const struct {
	unsigned int rate;
	u16 bits;
} creative_ae9_rate_bits[] = {
	{   8000, (0 << 14) | (0 << 11) | (5 << 8) },	/* 48k / 6 */
	{  11025, (1 << 14) | (0 << 11) | (3 << 8) },	/* 44.1k / 4 */
	{  16000, (0 << 14) | (0 << 11) | (2 << 8) },	/* 48k / 3 */
	{  22050, (1 << 14) | (0 << 11) | (1 << 8) },	/* 44.1k / 2 */
	{  32000, (0 << 14) | (1 << 11) | (2 << 8) },	/* 48k * 2 / 3 */
	{  44100, (1 << 14) },
	{  48000, 0 },
	{  88200, (1 << 14) | (1 << 11) },		/* 44.1k * 2 */
	{  96000, (0 << 14) | (1 << 11) },		/* 48k * 2 */
	{ 176400, (1 << 14) | (3 << 11) },		/* 44.1k * 4 */
	{ 192000, (0 << 14) | (3 << 11) },		/* 48k * 4 */
};

static u16 creative_ae9_format_val(unsigned int channels, unsigned int bits,
				   unsigned int rate)
{
	u16 val = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(creative_ae9_rate_bits); i++)
		if (creative_ae9_rate_bits[i].rate == rate)
			break;
	if (i < ARRAY_SIZE(creative_ae9_rate_bits))
		val |= creative_ae9_rate_bits[i].bits;
	else
		WARN_ONCE(1, "creative-ae9: no SDnFMT encoding for %u Hz, using 48 kHz\n",
			  rate);
	switch (bits) {
	case 16: val |= (1 << 4); break;
	case 20: val |= (2 << 4); break;
	case 24: val |= (3 << 4); break;
	case 32: val |= (4 << 4); break;
	default: val |= (1 << 4); break;
	}
	val |= (channels - 1) & 0xf;

	return val;
}

static void creative_ae9_stream_reset(struct creative_chip *chip,
				       unsigned int base)
{
	u32 ctl;

	ctl = readl(chip->hda_regs + base + AZX_REG_SD_CTL);
	ctl &= ~SD_CTL_DMA_START;
	ctl |= SD_CTL_STREAM_RESET;
	writel((ctl) & SD_CTL_WMASK, chip->hda_regs + base + AZX_REG_SD_CTL);
	wmb();

	udelay(50);

	ctl &= ~SD_CTL_STREAM_RESET;
	writel((ctl) & SD_CTL_WMASK, chip->hda_regs + base + AZX_REG_SD_CTL);
	wmb();

	udelay(50);
}

static void creative_ae9_stream_start(struct creative_chip *chip,
				       unsigned int base)
{
	u32 ctl, intctl;

	/*
	 * Enable this stream's interrupt bit in INTCTL so that IOC
	 * completions propagate to INTSTS and reach our IRQ handler.
	 * Stream at base 0x100 = stream index 0 = INTCTL bit 0.
	 * Without this bit set, INTSTS never shows AZX_INT_ALL_STREAM,
	 * snd_pcm_period_elapsed() never fires, and aplay underruns instantly.
	 */
	{
		/* Derive stream bit from base offset: (base - 0x80) / 0x20.
		 * Playback and capture hold different stream locks, so the
		 * INTCTL read-modify-write takes chip->lock. */
		unsigned int stream_idx = (base - 0x80) / 0x20;
		unsigned long flags;

		spin_lock_irqsave(&chip->lock, flags);
		intctl = readl(chip->hda_regs + AZX_REG_INTCTL);
		intctl |= (1 << stream_idx);
		writel(intctl, chip->hda_regs + AZX_REG_INTCTL);
		spin_unlock_irqrestore(&chip->lock, flags);
	}

	ctl = readl(chip->hda_regs + base + AZX_REG_SD_CTL);
	ctl &= ~SD_CTL_STREAM_RESET;
	ctl |= SD_CTL_DMA_START | 0x04;  /* DMA_START + IOCE (period interrupt) */
	writel((ctl) & SD_CTL_WMASK, chip->hda_regs + base + AZX_REG_SD_CTL);
	wmb();
}

static void creative_ae9_stream_stop(struct creative_chip *chip,
				      unsigned int base)
{
	u32 ctl;

	ctl = readl(chip->hda_regs + base + AZX_REG_SD_CTL);
	ctl &= ~SD_CTL_DMA_START;
	writel((ctl) & SD_CTL_WMASK, chip->hda_regs + base + AZX_REG_SD_CTL);
	wmb();
}

/* ------------------------------------------------------------------ */
/* PCM open / close						  */
/* ------------------------------------------------------------------ */

static int creative_ae9_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct creative_chip *chip = snd_pcm_substream_chip(substream);

	chip->substream = substream;
	chip->playback_substream = substream;

	runtime->hw.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
			   SNDRV_PCM_INFO_INTERLEAVED;

	/* ALSA's default thresholds (start_threshold = 1 underran at once).
	 * Windows: node 0x02 format 0x31 (2 ch, 24-bit, 48 kHz) and a 2-entry
	 * BDL. HDA 24-bit samples sit in the top 24 bits of a 32-bit
	 * container, so S32_LE with 24 msbits (S24_LE was ~48 dB too quiet). */
	runtime->hw.formats = SNDRV_PCM_FMTBIT_S32_LE;
	runtime->hw.channels_min = 2;
	runtime->hw.channels_max = 2;
	runtime->hw.rates = SNDRV_PCM_RATE_48000;
	runtime->hw.rate_min = 48000;
	runtime->hw.rate_max = 48000;
	runtime->hw.buffer_bytes_max = 128 * 1024;
	runtime->hw.period_bytes_min = 4096;
	runtime->hw.period_bytes_max = 64 * 1024;
	runtime->hw.periods_min = 2;
	runtime->hw.periods_max = 2;
	snd_pcm_hw_constraint_msbits(runtime, 0, 32, 24);
	chip->pcm_start_sent = 0;

	return 0;
}

static int creative_ae9_pcm_close(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);

	chip->substream = NULL;
	chip->playback_substream = NULL;
	/* relay_engaged stays set — relay remains engaged until driver shutdown */
	return 0;
}

/* ------------------------------------------------------------------ */
/* hw_params / hw_free						  */
/* ------------------------------------------------------------------ */

static int creative_ae9_pcm_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *hw_params)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	size_t alloc_size;
	int ret;

	if (chip->playback_bdl_raw) {
		dma_free_coherent(chip->dev, chip->playback_bdl_size,
				  chip->playback_bdl_raw,
				  chip->playback_bdl_dma);
		chip->playback_bdl_raw = NULL;
		chip->playback_bdl = NULL;
	}

	ret = snd_pcm_lib_malloc_pages(substream,
				       params_buffer_bytes(hw_params));
	if (ret < 0)
		return ret;

	alloc_size = CREATIVE_PCM_BDL_SIZE + 128;
	chip->playback_bdl_raw = dma_alloc_coherent(chip->dev, alloc_size,
						    &chip->playback_bdl_dma,
						    GFP_KERNEL);
	if (!chip->playback_bdl_raw) {
		snd_pcm_lib_free_pages(substream);
		return -ENOMEM;
	}
	chip->playback_bdl_size = alloc_size;

	chip->playback_bdl = PTR_ALIGN(chip->playback_bdl_raw, 128);
	chip->playback_bdl_dma = ALIGN(chip->playback_bdl_dma, 128);

	chip->playback_period_size = params_period_bytes(hw_params);

	return 0;
}

/*
 * pcm_stop_block: send Windows' playback stop block when the stream is
 * released (1, default) or leave the DSP stream set up (0). Restarts
 * ~110-130 s after a stop came up silent (shorter and much longer gaps
 * were fine), as if the stop starts a timer inside the card; 0 tests
 * whether skipping the stop avoids it. Writable at runtime.
 */
static unsigned int pcm_stop_block = 1;
module_param(pcm_stop_block, uint, 0644);
MODULE_PARM_DESC(pcm_stop_block, "Send Windows' playback stop block at stream release (1=yes, default; 0=skip)");

static int creative_ae9_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);

	/* pcm_windows: Windows' stop block (unbind node 0x02 etc.) when the
	 * stream is released, not at every trigger stop (an xrun stop +
	 * re-prepare would otherwise leave node 0x02 unbound) */
	if (chip->pcm_start_sent) {
		if (pcm_stop_block)
			creative_ae9_cold_pcm_replay(chip, false, false);
		else
			dev_info(chip->dev, "PCM hw_free: stop block skipped (pcm_stop_block=0)\n");
		chip->pcm_start_sent = 0;
	}

	if (chip->playback_bdl_raw) {
		dma_free_coherent(chip->dev, chip->playback_bdl_size,
				  chip->playback_bdl_raw,
				  chip->playback_bdl_dma);
		chip->playback_bdl_raw = NULL;
		chip->playback_bdl = NULL;
	}

	return snd_pcm_lib_free_pages(substream);
}

/* ------------------------------------------------------------------ */
/* prepare — program BDL and stream registers			  */
/* ------------------------------------------------------------------ */

static int creative_ae9_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int base = chip->playback_stream_base;
	unsigned int buf_size = snd_pcm_lib_buffer_bytes(substream);
	unsigned int period_size = snd_pcm_lib_period_bytes(substream);
	u16 format;
	dma_addr_t addr;
	int i;

	creative_ae9_stream_reset(chip, base);

	/* Build BDL entries (keep as fallback) */
	memset(chip->playback_bdl, 0, CREATIVE_PCM_BDL_SIZE);
	addr = runtime->dma_addr;

	for (i = 0; i < CREATIVE_PCM_BDL_ENTRIES; i++) {
		__le64 *bdl_addr = chip->playback_bdl + i * 16;
		__le32 *bdl_len = chip->playback_bdl + i * 16 + 8;
		__le32 *bdl_ioc = chip->playback_bdl + i * 16 + 12;

		*bdl_addr = cpu_to_le64(addr);
		*bdl_len = cpu_to_le32(period_size);
		/* pcm_windows: interrupt per period; with IOC only on the last
		 * entry ALSA heard once per buffer, found it empty and xran */
		*bdl_ioc = cpu_to_le32(1);

		addr += period_size;
	}
	wmb();

	/* Standard HDA stream descriptor setup.
	 * SD_LVI (0x0c) and SD_FORMAT (0x12) are 16-bit registers; SD_FORMAT
	 * is at the unaligned offset base+0x12, where a 32-bit access wedges
	 * the device (master-abort -> bus hang). Use 16-bit writes.
	 */
	/* Set stream tag in SD_CTL [23:20]. Without this, tag=0 at trigger
	 * time and the codec gets bound to stream 0 (silence).
	 */
	{
		u32 ctl = readl(chip->hda_regs + base + AZX_REG_SD_CTL);

		ctl &= ~(0xf << 20);
		ctl |= ((CREATIVE_PCM_STREAM_TAG & 0xf) << 20);
		writel((ctl) & SD_CTL_WMASK, chip->hda_regs + base + AZX_REG_SD_CTL);
		wmb();
	}

	creative_hda_writel(chip, base + AZX_REG_SD_CBL, buf_size);
	creative_hda_writew(chip, base + AZX_REG_SD_LVI,
			    CREATIVE_PCM_BDL_ENTRIES - 1);

	/* pcm_windows: S32_LE container, 24-bit stream format (0x31) */
	format = creative_ae9_format_val(runtime->channels,
					 24,
					 runtime->rate);
	creative_hda_writew(chip, base + AZX_REG_SD_FORMAT, format);

	/* Write BDL pointer as fallback */
	creative_hda_writel(chip, base + AZX_REG_SD_BDLPL,
			    lower_32_bits(chip->playback_bdl_dma));
	creative_hda_writel(chip, base + AZX_REG_SD_BDLPU,
			    upper_32_bits(chip->playback_bdl_dma));

	/* Windows' playback-start block sets format 0x31 and binds node
	 * 0x02 to tag 1 itself. Once per open: xrun recovery re-prepares,
	 * and replaying the block (12 SCP messages) each time flooded the
	 * DSP. */
	if (!chip->pcm_start_sent) {
		creative_ae9_cold_pcm_replay(chip, true, false);
		chip->pcm_start_sent = 1;
		dev_info(chip->dev, "PCM prepare: SD_FMT=0x%04x, Windows start block sent\n",
			 format);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* trigger — start / stop / pause / resume				  */
/* ------------------------------------------------------------------ */

static int creative_ae9_pcm_trigger(struct snd_pcm_substream *substream,
				     int cmd)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	unsigned int base = chip->playback_stream_base;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		/* stream only; the start block went out at prepare */
		creative_ae9_stream_start(chip, base);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		creative_ae9_stream_stop(chip, base);	/* stop block at hw_free */
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* pointer — report current DMA position				  */
/* ------------------------------------------------------------------ */

static snd_pcm_uframes_t
creative_ae9_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	unsigned int base = chip->playback_stream_base;
	unsigned int pos;

	pos = readl(chip->hda_regs + base + AZX_REG_SD_LPIB);
	wmb();
	/* Mask to buffer size to prevent pos == buffer_size (invalid to ALSA) */
	if (pos >= snd_pcm_lib_buffer_bytes(substream))
		pos = 0;
	return bytes_to_frames(substream->runtime, pos);
}

/* ------------------------------------------------------------------ */
/* PCM operations table						  */
/* ------------------------------------------------------------------ */

static const struct snd_pcm_ops creative_ae9_pcm_ops = {
	.open		= creative_ae9_pcm_open,
	.close		= creative_ae9_pcm_close,
	.hw_params	= creative_ae9_pcm_hw_params,
	.hw_free	= creative_ae9_pcm_hw_free,
	.prepare	= creative_ae9_pcm_prepare,
	.trigger	= creative_ae9_pcm_trigger,
	.pointer	= creative_ae9_pcm_pointer,
};

/* ------------------------------------------------------------------ */
/* Capture: input stream 0 (BAR0+0x80), tag 1, from codec 1 node 0x07	  */
/* (the head unit's XLR input), as in the Windows recording capture.	  */
/* ------------------------------------------------------------------ */
#define CREATIVE_CAP_STREAM_BASE	0x80	/* input stream descriptor 0 */
#define CREATIVE_CAP_STREAM_TAG		1
#define CREATIVE_CAP_BDL_MAX		8

static int creative_ae9_cap_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct creative_chip *chip = snd_pcm_substream_chip(substream);

	chip->capture_substream = substream;
	runtime->hw.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
			   SNDRV_PCM_INFO_INTERLEAVED;
	/* Windows: format 0x31 (2 ch, 24-bit in 32-bit containers, 48 kHz) */
	runtime->hw.formats = SNDRV_PCM_FMTBIT_S32_LE;
	runtime->hw.channels_min = 2;
	runtime->hw.channels_max = 2;
	runtime->hw.rates = SNDRV_PCM_RATE_48000;
	runtime->hw.rate_min = 48000;
	runtime->hw.rate_max = 48000;
	runtime->hw.buffer_bytes_max = 64 * 1024;
	runtime->hw.period_bytes_min = 1920;	/* 5 ms */
	runtime->hw.period_bytes_max = 32 * 1024;
	runtime->hw.periods_min = 2;
	runtime->hw.periods_max = CREATIVE_CAP_BDL_MAX;
	snd_pcm_hw_constraint_msbits(runtime, 0, 32, 24);
	snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	chip->cap_start_sent = 0;
	return 0;
}

static int creative_ae9_cap_close(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);

	chip->capture_substream = NULL;
	return 0;
}

static void creative_ae9_cap_bdl_free(struct creative_chip *chip)
{
	if (chip->cap_bdl_raw) {
		dma_free_coherent(chip->dev, chip->cap_bdl_size,
				  chip->cap_bdl_raw, chip->cap_bdl_dma);
		chip->cap_bdl_raw = NULL;
		chip->cap_bdl = NULL;
	}
}

static int creative_ae9_cap_hw_params(struct snd_pcm_substream *substream,
				      struct snd_pcm_hw_params *hw_params)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	dma_addr_t dma;
	int ret;

	creative_ae9_cap_bdl_free(chip);
	ret = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
	if (ret < 0)
		return ret;
	chip->cap_bdl_size = CREATIVE_CAP_BDL_MAX * 16 + 128;
	chip->cap_bdl_raw = dma_alloc_coherent(chip->dev, chip->cap_bdl_size,
					       &dma, GFP_KERNEL);
	if (!chip->cap_bdl_raw) {
		snd_pcm_lib_free_pages(substream);
		return -ENOMEM;
	}
	/* keep the raw handle for the free; use a 128-byte aligned view */
	chip->cap_bdl_dma = dma;
	chip->cap_bdl = PTR_ALIGN(chip->cap_bdl_raw, 128);
	return 0;
}

static int creative_ae9_cap_hw_free(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);

	if (chip->cap_start_sent) {
		creative_ae9_cap_replay(chip, false);
		chip->cap_start_sent = 0;
	}
	creative_ae9_cap_bdl_free(chip);
	return snd_pcm_lib_free_pages(substream);
}

static int creative_ae9_cap_prepare(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int base = CREATIVE_CAP_STREAM_BASE;
	unsigned int period = snd_pcm_lib_period_bytes(substream);
	unsigned int periods = runtime->periods;
	dma_addr_t bdl_dma = ALIGN(chip->cap_bdl_dma, 128);
	dma_addr_t addr = runtime->dma_addr;
	unsigned int i;
	u32 ctl;

	if (periods > CREATIVE_CAP_BDL_MAX)
		return -EINVAL;
	creative_ae9_stream_reset(chip, base);

	memset(chip->cap_bdl, 0, CREATIVE_CAP_BDL_MAX * 16);
	for (i = 0; i < periods; i++) {
		__le64 *bdl_addr = chip->cap_bdl + i * 16;
		__le32 *bdl_len = chip->cap_bdl + i * 16 + 8;
		__le32 *bdl_ioc = chip->cap_bdl + i * 16 + 12;

		*bdl_addr = cpu_to_le64(addr);
		*bdl_len = cpu_to_le32(period);
		*bdl_ioc = cpu_to_le32(1);	/* interrupt every period */
		addr += period;
	}
	wmb();

	ctl = readl(chip->hda_regs + base + AZX_REG_SD_CTL);
	ctl &= ~(0xf << 20);
	ctl |= (CREATIVE_CAP_STREAM_TAG & 0xf) << 20;
	writel((ctl) & SD_CTL_WMASK, chip->hda_regs + base + AZX_REG_SD_CTL);
	wmb();
	creative_hda_writel(chip, base + AZX_REG_SD_CBL, period * periods);
	creative_hda_writew(chip, base + AZX_REG_SD_LVI, periods - 1);
	creative_hda_writew(chip, base + AZX_REG_SD_FORMAT,
			    creative_ae9_format_val(runtime->channels, 24,
						    runtime->rate));
	creative_hda_writel(chip, base + AZX_REG_SD_BDLPL, lower_32_bits(bdl_dma));
	creative_hda_writel(chip, base + AZX_REG_SD_BDLPU, upper_32_bits(bdl_dma));

	/* Windows' start block once per open (re-prepares after an xrun
	 * must not resend it) */
	if (!chip->cap_start_sent) {
		creative_ae9_cap_replay(chip, true);
		chip->cap_start_sent = 1;
		dev_info(chip->dev, "capture prepare: SD_FMT=0x%04x\n",
			 creative_hda_readw(chip, base + AZX_REG_SD_FORMAT));
	}
	return 0;
}

static int creative_ae9_cap_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		creative_ae9_stream_start(chip, CREATIVE_CAP_STREAM_BASE);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		creative_ae9_stream_stop(chip, CREATIVE_CAP_STREAM_BASE);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static snd_pcm_uframes_t
creative_ae9_cap_pointer(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	unsigned int pos;

	pos = readl(chip->hda_regs + CREATIVE_CAP_STREAM_BASE + AZX_REG_SD_LPIB);
	if (pos >= snd_pcm_lib_buffer_bytes(substream))
		pos = 0;
	return bytes_to_frames(substream->runtime, pos);
}

static const struct snd_pcm_ops creative_ae9_cap_ops = {
	.open		= creative_ae9_cap_open,
	.close		= creative_ae9_cap_close,
	.hw_params	= creative_ae9_cap_hw_params,
	.hw_free	= creative_ae9_cap_hw_free,
	.prepare	= creative_ae9_cap_prepare,
	.trigger	= creative_ae9_cap_trigger,
	.pointer	= creative_ae9_cap_pointer,
};

/* Called from the IRQ handler: acknowledge input stream 0, notify ALSA. */
void creative_ae9_cap_irq(struct creative_chip *chip)
{
	unsigned int base = CREATIVE_CAP_STREAM_BASE;
	u8 sts = readb(chip->hda_regs + base + AZX_REG_SD_STS);

	if (!sts)
		return;
	writeb(sts & SD_INT_MASK, chip->hda_regs + base + AZX_REG_SD_STS);
	if ((sts & SD_INT_COMPLETE) && chip->capture_substream)
		snd_pcm_period_elapsed(chip->capture_substream);
}

/* ------------------------------------------------------------------ */
/* PCM device init / free						  */
/* ------------------------------------------------------------------ */

static void creative_ae9_pcm_free(struct snd_pcm *pcm);

int creative_ae9_pcm_init(struct creative_chip *chip)
{
	struct snd_pcm *pcm;
	int ret;

	chip->playback_stream_tag = CREATIVE_PCM_STREAM_TAG;
	chip->playback_stream_base = CREATIVE_PCM_STREAM_BASE;

	ret = snd_pcm_new(chip->card, "AE-9 Analog", 0, 1, 1, &pcm);
	if (ret < 0) {
		dev_err(chip->dev, "snd_pcm_new failed: %d\n", ret);
		return ret;
	}

	pcm->private_data = chip;
	strscpy(pcm->name, "AE-9 Analog", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK,
			&creative_ae9_pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &creative_ae9_cap_ops);

	snd_pcm_lib_preallocate_pages_for_all(pcm,
		SNDRV_DMA_TYPE_DEV, chip->dev,
		64 * 1024, 64 * 1024);

	chip->pcm = pcm;
	pcm->private_free = creative_ae9_pcm_free;

	dev_info(chip->dev, "PCM device initialized (playback + capture)\n");
	return 0;
}

/* pcm->private_free: hw_free normally releases the BDLs already; this
 * catches any left over when the card goes away. */
static void creative_ae9_pcm_free(struct snd_pcm *pcm)
{
	struct creative_chip *chip = pcm->private_data;

	if (chip->playback_bdl_raw) {
		dma_free_coherent(chip->dev, chip->playback_bdl_size,
				  chip->playback_bdl_raw,
				  chip->playback_bdl_dma);
		chip->playback_bdl_raw = NULL;
		chip->playback_bdl = NULL;
	}
	creative_ae9_cap_bdl_free(chip);
	chip->pcm = NULL;
}
