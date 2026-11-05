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
#include <sound/control.h>
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
#define CREATIVE_PCM_BDL_MAX		32	/* one entry per period */
#define CREATIVE_PCM_BDL_SIZE		(CREATIVE_PCM_BDL_MAX * 16)

/* playback buffer limit (preallocated per substream; capture keeps its own
 * 64 KB limit in its hw constraints) */
#define CREATIVE_PCM_BUF_MAX	(256 * 1024)

/*
 * Playback buffer layout: most periods per buffer (2-32, as snd_hda_intel
 * allows). PipeWire sizes the buffer as periods x its default period, not
 * from the app's quantum, so with 2 an app asking for a big quantum
 * (pw-play: 4096 at 96 kHz) could get one the size of the whole buffer and
 * errors. 8 (default since 2026-11-05) lets it pick a buffer several
 * quanta long; PipeWire keeps ~21 ms periods, so interrupts (46/s) and
 * latency stay the same. Windows uses 2 entries at 48 kHz stereo, up to
 * 24 for bigger streams. Writable: takes effect at the next stream open. pcm_nowakeup=1 lets PipeWire turn
 * period interrupts off (SNDRV_PCM_INFO_NO_PERIOD_WAKEUP, as
 * snd_hda_intel does); it runs on its own timer anyway.
 */
static unsigned int pcm_periods = 8;
module_param(pcm_periods, uint, 0644);
MODULE_PARM_DESC(pcm_periods, "Most playback periods per buffer, 2-32 (default 8; 2 = Windows' 48 kHz layout)");
static bool pcm_nowakeup;
module_param(pcm_nowakeup, bool, 0644);
MODULE_PARM_DESC(pcm_nowakeup, "Let apps disable playback period interrupts (0 = no, default)");

/*
 * DMA position buffer (HD-Audio spec 3.3.32, DPLBASE): the controller
 * writes each stream's DMA read position to memory. LPIB, our pointer, is
 * the position going out on the link; the DMA has already fetched ahead
 * of it, and at tiny PipeWire quantums PipeWire wrote into audio the card
 * had already fetched (static, no xrun). The cure is PipeWire headroom
 * (README, scripts/ae9-headroom.sh). 1 = log the DMA-LPIB gap every 2 s;
 * the pointer always stays LPIB. Writable; takes effect at the next
 * stream start. Reporting the DMA position as the pointer was tried
 * (2026-11-05) and dropped: it moves in ~100-frame jumps, and with
 * headroom in place it only added underruns at 192/384 kHz.
 */
static unsigned int pcm_posbuf;
module_param(pcm_posbuf, uint, 0644);
MODULE_PARM_DESC(pcm_posbuf, "DMA position buffer: 0 off (default), 1 log how far the DMA reads ahead of LPIB");

static void ae9_posbuf_off(struct creative_chip *chip)
{
	if (!chip->posbuf)
		return;
	/* stop the controller writing before the memory goes away */
	creative_hda_writel(chip, AZX_REG_DPLBASE, 0);
	creative_hda_writel(chip, AZX_REG_DPUBASE, 0);
	creative_hda_readl(chip, AZX_REG_DPLBASE);
	dma_free_coherent(chip->dev, ae9_dma_size(chip, PAGE_SIZE), chip->posbuf, chip->posbuf_dma);
	chip->posbuf = NULL;
}

static void ae9_posbuf_on(struct creative_chip *chip)
{
	if (!pcm_posbuf) {
		ae9_posbuf_off(chip);
		return;
	}
	if (!chip->posbuf) {
		chip->posbuf = dma_alloc_coherent(chip->dev, ae9_dma_size(chip, PAGE_SIZE),
						  &chip->posbuf_dma, GFP_KERNEL);
		if (!chip->posbuf) {
			dev_warn(chip->dev, "posbuf: no memory, staying on LPIB\n");
			return;
		}
	}
	memset(chip->posbuf, 0, PAGE_SIZE);
	chip->pb_min = U32_MAX;
	chip->pb_max = chip->pb_n = chip->pb_last = chip->pb_moves = 0;
	chip->pb_sum = 0;
	chip->pb_next = jiffies + 2 * HZ;
	creative_hda_writel(chip, AZX_REG_DPUBASE, upper_32_bits(chip->posbuf_dma));
	creative_hda_writel(chip, AZX_REG_DPLBASE,
			    lower_32_bits(chip->posbuf_dma) | AZX_DPLBASE_ENABLE);
}

/* the read-ahead stats: DMA position vs LPIB for this stream */
static void ae9_posbuf_read(struct creative_chip *chip, u32 lpib, u32 bufsz,
			    unsigned int frame_bytes)
{
	unsigned int idx = (chip->playback_stream_base - 0x80) / 0x20;
	u32 dma = le32_to_cpu(READ_ONCE(((__le32 *)chip->posbuf)[idx * 2]));
	u32 gap;

	if (dma != chip->pb_last)
		chip->pb_moves++;
	chip->pb_last = dma;
	if (dma < bufsz && lpib < bufsz) {
		gap = (dma + bufsz - lpib) % bufsz;
		chip->pb_min = min(chip->pb_min, gap);
		chip->pb_max = max(chip->pb_max, gap);
		chip->pb_sum += gap;
		chip->pb_n++;
	}
	if (time_after(jiffies, chip->pb_next)) {
		if (chip->pb_n && frame_bytes)
			dev_info(chip->dev, "posbuf: DMA ahead of LPIB by %u-%u bytes (avg %llu) = %u-%u frames, %u samples, DMA position %s\n",
				 chip->pb_min, chip->pb_max, div_u64(chip->pb_sum, chip->pb_n),
				 chip->pb_min / frame_bytes, chip->pb_max / frame_bytes,
				 chip->pb_n, chip->pb_moves ? "updating" : "NOT updating");
		else
			dev_info(chip->dev, "posbuf: no samples (DMA position 0x%x, %s)\n",
				 dma, chip->pb_moves ? "updating" : "NOT updating");
		chip->pb_min = U32_MAX;
		chip->pb_max = chip->pb_n = chip->pb_moves = 0;
		chip->pb_sum = 0;
		chip->pb_next = jiffies + 2 * HZ;
	}
}

/*
 * Build the 16-bit SDnFMT value (HD-Audio spec 3.7.1, same as the codec's
 * converter format):
 * Bit 14    : base rate (0 = 48 kHz, 1 = 44.1 kHz)
 * Bits 13:11: base multiplier minus 1 (x1..x4)
 * Bits 10:8 : base divisor minus 1 (/1../8)
 * Bits 6:4  : bits per sample (1 = 16, 2 = 20, 3 = 24, 4 = 32)
 * Bits 3:0  : number of channels minus 1
 * 2 ch, 24-bit, 48 kHz is 0x0031. 384 kHz is beyond the link's x4
 * multiplier: Windows sends it as 192 kHz with twice the channels (two
 * stereo samples per frame, the same bytes in the same order), 0x1833.
 * Playback offers 44.1-384 kHz with pcm_rates=1; the other entries are
 * unused.
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
	{ 384000, (0 << 14) | (3 << 11) },		/* 192k, 2x channels */
};

u16 creative_ae9_stream_format(unsigned int channels, unsigned int bits,
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
	if (rate == 384000)
		channels *= 2;
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

/*
 * pcm_rates: 0 (default) = playback at 48 kHz only, as Windows' captured
 * start block. 1 = playback at 48, 96, 192 and 384 kHz, each set up the
 * way Windows does it (creative_ae9_cold_pcm_replay). 2 = also 44.1 and
 * 88.2 kHz through the card's sample-rate converter: EXPERIMENTAL, it plays
 * silent or garbled on our card so far (Windows played its first 44.1 kHz
 * stream straight after startup; the clock state before it seems to
 * matter). PipeWire resamples 44.1 kHz files when those rates are left out.
 * Recording stays at 48 kHz.
 * pcm_bits: 24 (default, as Windows' default format) or 32: the samples'
 * full 32 bits go to the card (format 0x41..., which Windows also uses).
 * Both writable at runtime; they apply to streams opened afterwards.
 */
/* 48/96/192/384 kHz by default (Direct Mode; DSP mode takes 48 kHz):
 * every rate checked at 24 and 32 bit, switching back and forth, 2026-11-04 */
static unsigned int pcm_rates = 1;
module_param(pcm_rates, uint, 0644);
MODULE_PARM_DESC(pcm_rates, "Playback sample rates: 0 = 48 kHz only, 1 = 48/96/192/384 kHz (default), 2 = also 44.1/88.2 kHz (experimental)");
/*
 * 5.1: 6-channel playback in both modes (Windows' order FL FR FC LFE RL RR
 * -> front RCA, Center/Sub jack, Rear jack; checked by ear 2026-11-04) and
 * the 4.0/4.1/5.1 speaker configs. PipeWire offers it as a profile with
 * config/creative-ae9.conf. Takes effect at the next stream open.
 */
unsigned int ae9_surround = 1;
module_param_named(surround, ae9_surround, uint, 0644);
MODULE_PARM_DESC(surround, "Offer 6-channel (5.1) playback and the 4.0/4.1/5.1 speaker configs (1=on, default; 0 = stereo only)");

static const unsigned int ae9_ch_list[] = { 2, 6 };
static const struct snd_pcm_hw_constraint_list ae9_ch_constraint = {
	.count = ARRAY_SIZE(ae9_ch_list),
	.list = ae9_ch_list,
};

/* Windows' 5.1 order (WAVEFORMATEXTENSIBLE): FL FR FC LFE BL BR */
static const struct snd_pcm_chmap_elem ae9_chmaps[] = {
	{ .channels = 2, .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR } },
	{ .channels = 6, .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR,
				  SNDRV_CHMAP_FC, SNDRV_CHMAP_LFE,
				  SNDRV_CHMAP_RL, SNDRV_CHMAP_RR } },
	{ }
};

/*
 * 6 channels up to 96 kHz (48 kHz with surround_96k=0), and above that
 * only 2. Direct Mode only: DSP mode opens at 48 kHz. Windows' 5.1
 * captures are all 48 kHz.
 */
#define AE9_SURROUND_RATE_MAX	(surround_96k ? 96000 : 48000)

/* 5.1 at 96 kHz used to stutter more and more as it played: the start
 * block gave it the stereo router setup (front slots only) and the 64 KB
 * buffer was barely one quantum. Both fixed 2026-11-05: all six speakers
 * right, no errors over 2 min; on by default since. The 96 kHz slot
 * layout is our guess (no Windows capture of 5.1 above 48 kHz); 0 goes
 * back to 5.1 at 48 kHz only. */
static unsigned int surround_96k = 1;
module_param(surround_96k, uint, 0644);
MODULE_PARM_DESC(surround_96k, "Allow 5.1 at 96 kHz in Direct Mode (1 = yes, default; 0 = 5.1 at 48 kHz only)");
static int ae9_rule_rate_by_ch(struct snd_pcm_hw_params *params,
			       struct snd_pcm_hw_rule *rule)
{
	struct snd_interval *ch = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_interval *rate = hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval t;

	if (ch->min <= 2)
		return 0;
	snd_interval_any(&t);
	t.max = AE9_SURROUND_RATE_MAX;
	return snd_interval_refine(rate, &t);
}

static int ae9_rule_ch_by_rate(struct snd_pcm_hw_params *params,
			       struct snd_pcm_hw_rule *rule)
{
	struct snd_interval *ch = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_interval *rate = hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval t;

	if (rate->min <= AE9_SURROUND_RATE_MAX)
		return 0;
	snd_interval_any(&t);
	t.max = 2;
	return snd_interval_refine(ch, &t);
}

/*
 * Bits the card takes the stream as (the format's bits field; Windows: 24
 * or 32 by source). ALSA can't negotiate it: both are S32_LE, 24-bit
 * audio sitting in the top bits. Also the "PCM Bit Depth" control. Takes
 * effect at the next stream open.
 */
static unsigned int pcm_bits = 24;
module_param(pcm_bits, uint, 0644);
MODULE_PARM_DESC(pcm_bits, "Playback sample bits sent to the card: 24 (default) or 32; also the 'PCM Bit Depth' control");

static int pcm_bits_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	static const char * const names[] = { "24-bit", "32-bit" };

	return snd_ctl_enum_info(ui, 1, ARRAY_SIZE(names), names);
}

static int pcm_bits_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	uv->value.enumerated.item[0] = pcm_bits == 32;
	return 0;
}

static int pcm_bits_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	unsigned int want = uv->value.enumerated.item[0] ? 32 : 24;

	if (uv->value.enumerated.item[0] > 1)
		return -EINVAL;
	if (want == (pcm_bits == 32 ? 32 : 24))
		return 0;
	pcm_bits = want;	/* the next stream opens with it */
	return 1;
}

static const struct snd_kcontrol_new pcm_bits_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "PCM Bit Depth",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info = pcm_bits_info,
	.get = pcm_bits_get,
	.put = pcm_bits_put,
};

static int creative_ae9_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct creative_chip *chip = snd_pcm_substream_chip(substream);

	chip->substream = substream;
	chip->playback_substream = substream;

	runtime->hw.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
			   SNDRV_PCM_INFO_INTERLEAVED;
	if (pcm_nowakeup)
		runtime->hw.info |= SNDRV_PCM_INFO_NO_PERIOD_WAKEUP;

	/* ALSA's default thresholds (start_threshold = 1 underran at once).
	 * Windows: node 0x02 format 0x31 (2 ch, 24-bit, 48 kHz) and a 2-entry
	 * BDL. HDA 24-bit samples sit in the top 24 bits of a 32-bit
	 * container, so S32_LE with 24 msbits (S24_LE was ~48 dB too quiet). */
	runtime->hw.formats = SNDRV_PCM_FMTBIT_S32_LE;
	runtime->hw.channels_min = 2;
	runtime->hw.channels_max = 2;
	if (pcm_rates == 2) {
		runtime->hw.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
				    SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 |
				    SNDRV_PCM_RATE_192000 | SNDRV_PCM_RATE_384000;
		runtime->hw.rate_min = 44100;
		runtime->hw.rate_max = 384000;
	} else if (pcm_rates) {
		runtime->hw.rates = SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_96000 |
				    SNDRV_PCM_RATE_192000 | SNDRV_PCM_RATE_384000;
		runtime->hw.rate_min = 48000;
		runtime->hw.rate_max = 384000;
	} else {
		runtime->hw.rates = SNDRV_PCM_RATE_48000;
		runtime->hw.rate_min = 48000;
		runtime->hw.rate_max = 48000;
	}
	/*
	 * Direct Mode off = through the DSP: 48 kHz only. Windows offers up to
	 * 96 kHz there, but our DSP-mode start only sets up a 48 kHz input:
	 * at 96 kHz the router came up with no DSP input path while the direct
	 * route stayed live, so the stream played twice, direct and through
	 * the DSP a little later (an echo; 2026-11-04). PipeWire resamples
	 * other rates to 48 kHz in DSP mode; Direct Mode keeps them all.
	 */
	if (!chip->fx.direct) {
		runtime->hw.rates &= SNDRV_PCM_RATE_48000;
		runtime->hw.rate_min = max(runtime->hw.rate_min, 48000U);
		runtime->hw.rate_max = min(runtime->hw.rate_max, 48000U);
	}
	/*
	 * 5.1 (surround=1), both modes: Windows plays 6 channels in DSP mode
	 * (captures 5, 6: format 0x35) and in Direct Mode with 5.1 speakers
	 * (capture 6, 23:39:16). 6 channels up to 96 kHz (see
	 * AE9_SURROUND_RATE_MAX).
	 */
	if (ae9_surround) {
		runtime->hw.channels_max = 6;
		snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS,
					   &ae9_ch_constraint);
		snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
				    ae9_rule_rate_by_ch, NULL,
				    SNDRV_PCM_HW_PARAM_CHANNELS, -1);
		snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS,
				    ae9_rule_ch_by_rate, NULL,
				    SNDRV_PCM_HW_PARAM_RATE, -1);
	}
	/* Room for PipeWire's default quantum at every rate: it scales 1024
	 * frames at 48 kHz up with the rate (8192 at 384 kHz = 64 KB stereo),
	 * and the old 64 KB buffer made one quantum the whole buffer there
	 * (stutter, and PipeWire's headroom clamped to 0). */
	runtime->hw.buffer_bytes_max = CREATIVE_PCM_BUF_MAX;
	runtime->hw.period_bytes_min = 4096;
	runtime->hw.period_bytes_max = 128 * 1024;
	runtime->hw.periods_min = 2;
	runtime->hw.periods_max = clamp(pcm_periods, 2U, (unsigned int)CREATIVE_PCM_BDL_MAX);
	/* whole periods only (the BDL has one entry per period, and the
	 * buffer length must be their sum), each starting on the 128-byte
	 * boundary the HD-Audio spec requires of a BDL entry's address */
	snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 128);
	snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 128);
	chip->pcm_bits = pcm_bits == 32 ? 32 : 24;
	snd_pcm_hw_constraint_msbits(runtime, 0, 32, chip->pcm_bits);
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

	alloc_size = ae9_dma_size(chip, CREATIVE_PCM_BDL_SIZE + 128);
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
			creative_ae9_cold_pcm_replay(chip, false, false, 0, 0);
		else
			dev_info(chip->dev, "PCM hw_free: stop block skipped (pcm_stop_block=0)\n");
		chip->pcm_start_sent = 0;
	}

	ae9_posbuf_off(chip);
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

	for (i = 0; i < runtime->periods; i++) {
		__le64 *bdl_addr = chip->playback_bdl + i * 16;
		__le32 *bdl_len = chip->playback_bdl + i * 16 + 8;
		__le32 *bdl_ioc = chip->playback_bdl + i * 16 + 12;

		*bdl_addr = cpu_to_le64(addr);
		*bdl_len = cpu_to_le32(period_size);
		/* pcm_windows: interrupt per period; with IOC only on the last
		 * entry ALSA heard once per buffer, found it empty and xran.
		 * None at all when the app asked for no period wakeups. */
		*bdl_ioc = cpu_to_le32(runtime->no_period_wakeup ? 0 : 1);

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
	creative_hda_writew(chip, base + AZX_REG_SD_LVI, runtime->periods - 1);

	/* S32_LE container; 24-bit (0x31 at 48 kHz) or 32-bit (0x41) */
	format = creative_ae9_stream_format(runtime->channels, chip->pcm_bits,
					    runtime->rate);
	creative_hda_writew(chip, base + AZX_REG_SD_FORMAT, format);

	/* Write BDL pointer as fallback */
	creative_hda_writel(chip, base + AZX_REG_SD_BDLPL,
			    lower_32_bits(chip->playback_bdl_dma));
	creative_hda_writel(chip, base + AZX_REG_SD_BDLPU,
			    upper_32_bits(chip->playback_bdl_dma));
	ae9_posbuf_on(chip);

	/* Windows' playback-start block sets format 0x31 and binds node
	 * 0x02 to tag 1 itself. Once per open: xrun recovery re-prepares,
	 * and replaying the block (12 SCP messages) each time flooded the
	 * DSP. */
	if (!chip->pcm_start_sent || chip->pcm_start_rate != runtime->rate ||
	    chip->pcm_start_fmt != format) {
		creative_ae9_cold_pcm_replay(chip, true, false, runtime->rate, format);
		if (chip->mic_level != 99)
			creative_ae9_mic_level_apply(chip);
		chip->pcm_start_sent = 1;
		chip->pcm_start_rate = runtime->rate;
		chip->pcm_start_fmt = format;
		ae9_dbg(chip, "PCM prepare: SD_FMT=0x%04x, Windows start block sent\n",
			 format);
		creative_ae9_fx_spk_follow(chip, runtime->channels);
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
		creative_ae9_dsp_switch_kick(chip);
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
	if (chip->posbuf)	/* pcm_posbuf: read-ahead stats only */
		ae9_posbuf_read(chip, pos, snd_pcm_lib_buffer_bytes(substream),
				frames_to_bytes(substream->runtime, 1));
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
	chip->cap_bdl_size = ae9_dma_size(chip, CREATIVE_CAP_BDL_MAX * 16 + 128);
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
			    creative_ae9_stream_format(runtime->channels, 24,
						    runtime->rate));
	creative_hda_writel(chip, base + AZX_REG_SD_BDLPL, lower_32_bits(bdl_dma));
	creative_hda_writel(chip, base + AZX_REG_SD_BDLPU, upper_32_bits(bdl_dma));

	/* Windows' start block once per open (re-prepares after an xrun
	 * must not resend it) */
	if (!chip->cap_start_sent) {
		creative_ae9_cap_replay(chip, true);
		if (chip->mic_level != 99)
			creative_ae9_mic_level_apply(chip);
		chip->cap_start_sent = 1;
		ae9_dbg(chip, "capture prepare: SD_FMT=0x%04x\n",
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
		ae9_dma_size(chip, CREATIVE_PCM_BUF_MAX),
		ae9_dma_size(chip, CREATIVE_PCM_BUF_MAX));

	chip->pcm = pcm;
	pcm->private_free = creative_ae9_pcm_free;
	ret = snd_pcm_add_chmap_ctls(pcm, SNDRV_PCM_STREAM_PLAYBACK, ae9_chmaps,
				     6, 0, NULL);
	if (ret < 0)
		dev_warn(chip->dev, "channel map control not added (%d)\n", ret);
	ret = snd_ctl_add(chip->card, snd_ctl_new1(&pcm_bits_control, chip));
	if (ret < 0)
		dev_warn(chip->dev, "PCM Bit Depth control not added (%d)\n", ret);

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
