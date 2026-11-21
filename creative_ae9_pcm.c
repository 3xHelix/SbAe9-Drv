// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 3xHelix
//
// ALSA PCM device for Creative AE-9 HDA controller
//
// Standard HDA output stream 0 (SDO0) at BAR0+0x100 with BDL DMA.
// Creative extension registers at BAR2 enable stream routing.
//
// Reference: ae9_mmio_capture.txt lines 11030+, docs/history/Creative.md §5
//

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/math64.h>
#include <linux/seq_file.h>
#include <linux/uio.h>
#include <sound/control.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/asoundef.h>
#include <sound/hda_register.h>

#include "creative_ae9.h"

/* Per the 0x0071 verb capture: Windows uses stream descriptor index 4
 * (BAR0+0x100) with stream tag 1, bound to DAC node 0x02 on codec 1.
 */
#define CREATIVE_PCM_STREAM_BASE	0x100	/* stream descriptor index 4 */
/* separate optical out: stream descriptor 6, tag 3, what Windows' rule
 * (lowest free output descriptor, tag = descriptor - 3) gives with SD4
 * (main playback, firmware/overlay load) and SD5 (headphone-profile
 * upload) taken; INTCTL/INTSTS bit 6 */
#define CREATIVE_OPT_STREAM_TAG	3
#define CREATIVE_OPT_STREAM_BASE	0x140
#define CREATIVE_PCM_CODEC_ADDR	1	/* main codec */
#define CREATIVE_PCM_DAC_NID	0x02	/* front DSP DAC */
#define CREATIVE_PCM_BDL_MAX		32	/* one entry per period */
#define CREATIVE_PCM_BDL_SIZE		(CREATIVE_PCM_BDL_MAX * 16)

/* playback buffer limit (preallocated per substream); also optical out and
 * the extra captures. The mic capture keeps its own 128 KB limit (its open). */
#define CREATIVE_PCM_BUF_MAX	(256 * 1024)

/*
 * Playback buffer layout: most periods per buffer (2-32, as snd_hda_intel
 * allows). PipeWire sizes the buffer as periods x its default period, not
 * from the app's quantum, so with 2 an app asking for a big quantum
 * (pw-play: 4096 at 96 kHz) could get one the size of the whole buffer and
 * errors. 8 (default since 2026-11-05) lets it pick a buffer several
 * quanta long; PipeWire keeps ~21 ms periods, so interrupts (46/s) and
 * latency stay the same. Windows uses 2 entries at 48 kHz stereo, up to
 * 24 for bigger streams. Writable: takes effect at the next stream open.
 */
static unsigned int pcm_periods = 8;
module_param(pcm_periods, uint, 0644);
MODULE_PARM_DESC(pcm_periods, "Most playback periods per buffer, 2-32 (default 8; 2 = Windows' 48 kHz layout)");

/* HD-Audio spec 3.3.35: after RUN is cleared, the DMA engine has stopped
 * only once RUN reads back 0. Wait up to 1 ms; log a stream that doesn't */
static bool ae9_sd_wait_stopped(struct creative_chip *chip, unsigned int base, const char *why)
{
	u32 c = 0;
	int t;

	for (t = 0; t < 100; t++) {
		c = readl(chip->hda_regs + base + AZX_REG_SD_CTL);
		/* all-ones: a card off the bus can't fetch anything (stopped);
		 * otherwise one bad read: once more, then "not stopped" without
		 * more reads of a maybe-dead card (this runs in trigger STOP
		 * too, with IRQs off) (ultrareview 2, #2) */
		if (c == 0xffffffff) {
			if (chip->card_gone)
				return true;
			c = readl(chip->hda_regs + base + AZX_REG_SD_CTL);
			if (c == 0xffffffff)
				return false;
		}
		if (!(c & SD_CTL_DMA_START)) {
			if (t)
				ae9_dbg(chip, "dma: SD 0x%03x stopped after ~%d us (%s)\n",
					base, t * 10, why);
			return true;
		}
		udelay(10);
	}
	dev_err(chip->dev, "dma: SD 0x%03x still running %s (ctl 0x%08x lpib 0x%x bdl 0x%08x)\n",
		base, why, c, readl(chip->hda_regs + base + AZX_REG_SD_LPIB),
		readl(chip->hda_regs + base + AZX_REG_SD_BDLPL));
	return false;
}

/*
 * Before a BDL is freed: a stream whose engine didn't stop could still
 * fetch it (and then write wherever the recycled page points: IOMMU faults,
 * or memory corruption without an IOMMU). Keep it then, as hpeq_upload and
 * xfer_stream_stop do (a stuck stream is left alone, no reset): a small
 * leak until the next reboot (ultrareview 2, #7). True = free it.
 * Only if the descriptor still points at THIS BDL: SD4 is also the DSP
 * firmware and overlay transfer stream (resume_work), and a playback closed
 * during a resume must not wait on or touch that transfer.
 * Process context only.
 */
static bool ae9_bdl_may_free(struct creative_chip *chip, unsigned int base,
			     dma_addr_t bdl, const char *what)
{
	void __iomem *sd = chip->hda_regs + base;
	u32 lo = readl(sd + AZX_REG_SD_BDLPL), hi = readl(sd + AZX_REG_SD_BDLPU);
	u32 want = lower_32_bits(bdl) & ~0x7fU, want_al = lower_32_bits(ALIGN(bdl, 128)) & ~0x7fU;

	if (lo == 0xffffffff || hi == 0xffffffff) {
		/* off the bus: nothing can fetch it any more; else a bad read
		 * (a bridge glitch): don't guess, keep it */
		if (creative_ae9_gone(chip, what))
			return true;
		dev_warn_ratelimited(chip->dev, "dma: SD 0x%03x registers unreadable (%s): its BDL is kept, not freed (%d kept)\n",
				     base, what, atomic_inc_return(&chip->bdl_leaks));
		creative_ae9_health_notify(chip);
		return false;
	} else if (hi != upper_32_bits(bdl) ||
		   ((lo & ~0x7fU) != want && (lo & ~0x7fU) != want_al)) {
		return true;	/* the descriptor doesn't use this BDL */
	} else if (ae9_sd_wait_stopped(chip, base, what)) {
		return true;
	}
	dev_warn_ratelimited(chip->dev, "dma: SD 0x%03x didn't stop (%s): its BDL is kept, not freed (%d kept)\n",
			     base, what, atomic_inc_return(&chip->bdl_leaks));
	creative_ae9_health_notify(chip);
	return false;
}

/*
 * debug=1: log every stream's buffer and BDL address at prepare, for
 * matching an IOMMU IO_PAGE_FAULT address to a stream (the 2026-11-13
 * rapid-SBX hang: 8-byte writes to an unmapped address ~70 ms after the
 * first command timeout; the dma_trace option until 0.12.15). The stop
 * check above and the dump below run always.
 */
static void ae9_dma_trace_prep(struct creative_chip *chip, const char *what, unsigned int base,
			       struct snd_pcm_substream *substream, dma_addr_t bdl)
{
	struct snd_pcm_runtime *rt = substream->runtime;
	size_t n = snd_pcm_lib_buffer_bytes(substream);

	if (!ae9_debug)
		return;
	dev_info(chip->dev, "dma: %s SD 0x%03x buffer 0x%llx-0x%llx (%zu bytes, %u ch, %u Hz), BDL 0x%llx\n",
		 what, base, (unsigned long long)rt->dma_addr,
		 (unsigned long long)rt->dma_addr + n - 1, n, rt->channels, rt->rate,
		 (unsigned long long)bdl);
}

static void ae9_dma_dump_bdl(struct creative_chip *chip, const char *name, void *bdl, dma_addr_t dma)
{
	if (!bdl)
		return;
	dev_err(chip->dev, "dma dump: %s BDL 0x%llx: entry 0 0x%llx+%u, entry 1 0x%llx+%u\n",
		name, (unsigned long long)ALIGN(dma, 128),
		(unsigned long long)le64_to_cpu(*(__le64 *)bdl), le32_to_cpu(*(__le32 *)(bdl + 8)),
		(unsigned long long)le64_to_cpu(*(__le64 *)(bdl + 16)), le32_to_cpu(*(__le32 *)(bdl + 24)));
}

/* stream descriptors from GCAP (input + output + bidirectional), capped at
 * the 30 the register map has room for; 0 if the controller reads all-ones
 * (ultrareview 2, #8: dma_dump and dma_quiesce shared this) */
static unsigned int ae9_sd_count(u16 g)
{
	if (g == 0xffff)
		return 0;
	return min_t(unsigned int, ((g >> 8) & 0xf) + ((g >> 12) & 0xf) + ((g >> 3) & 0x1f), 30);
}

/* every stream descriptor that runs or has a BDL, and the BDLs we hold:
 * called at the first command timeout and when the card is marked hung */
void creative_ae9_dma_dump(struct creative_chip *chip, const char *why)
{
	u16 g = readw(chip->hda_regs + AZX_REG_GCAP);
	unsigned int n, i;
	static const char * const xname[3] = { "optical in", "What U Hear", "rear RCA in" };

	if (g == 0xffff) {
		dev_err(chip->dev, "dma dump (%s): the controller reads all ones\n", why);
		return;
	}
	n = ae9_sd_count(g);
	dev_err(chip->dev, "dma dump (%s): GCAP 0x%04x, %u streams, DPLBASE 0x%08x, INTCTL 0x%08x, CORB 0x%llx, RIRB 0x%llx\n",
		why, g, n, readl(chip->hda_regs + AZX_REG_DPLBASE),
		readl(chip->hda_regs + AZX_REG_INTCTL),
		(unsigned long long)chip->corb_dma, (unsigned long long)chip->rirb_dma);
	for (i = 0; i < n; i++) {
		unsigned int base = 0x80 + i * 0x20;
		u32 ctl = readl(chip->hda_regs + base + AZX_REG_SD_CTL);
		u32 bdl = readl(chip->hda_regs + base + AZX_REG_SD_BDLPL);

		if (!(ctl & SD_CTL_DMA_START) && !bdl)
			continue;
		dev_err(chip->dev, "dma dump: SD 0x%03x ctl 0x%06x sts 0x%02x lpib 0x%x cbl 0x%x lvi %u bdl 0x%08x%08x%s\n",
			base, ctl & 0xffffff, ctl >> 24,
			readl(chip->hda_regs + base + AZX_REG_SD_LPIB),
			readl(chip->hda_regs + base + AZX_REG_SD_CBL),
			readw(chip->hda_regs + base + AZX_REG_SD_LVI) & 0xff,
			readl(chip->hda_regs + base + AZX_REG_SD_BDLPU), bdl,
			ctl & SD_CTL_DMA_START ? " RUN" : "");
	}
	ae9_dma_dump_bdl(chip, "playback", chip->playback_bdl, chip->playback_bdl_dma);
	ae9_dma_dump_bdl(chip, "optical out", chip->opt_bdl, chip->opt_bdl_dma);
	ae9_dma_dump_bdl(chip, "mic", chip->cap_bdl, chip->cap_bdl_dma);
	for (i = 0; i < 3; i++)
		ae9_dma_dump_bdl(chip, xname[i], chip->xcap[i].bdl, chip->xcap[i].bdl_dma);
}

/*
 * The card stopped answering (corb.c): stop every stream's DMA engine. A
 * crashed card's capture engines keep writing, to addresses it makes up
 * (uv65, 2026-11-14: three running captures wrote 8-byte frames to
 * unmapped addresses 4 s after the hang; the IOMMU blocked them). Clear
 * RUN and the stream interrupts on every descriptor; MMIO only, so it is
 * safe under corb_mutex. The PCMs are stopped from dead_work.
 */
void creative_ae9_dma_quiesce(struct creative_chip *chip)
{
	u16 g = readw(chip->hda_regs + AZX_REG_GCAP);
	unsigned int n, i, stopped = 0;
	u32 ctl;

	if (g == 0xffff)
		return;
	n = ae9_sd_count(g);
	for (i = 0; i < n; i++) {
		unsigned int base = 0x80 + i * 0x20;

		ctl = readl(chip->hda_regs + base + AZX_REG_SD_CTL);
		if (!(ctl & SD_CTL_DMA_START))
			continue;
		/* RUN, IOCE, FEIE, DEIE off */
		writel(ctl & SD_CTL_WMASK & ~(SD_CTL_DMA_START | 0x1c),
		       chip->hda_regs + base + AZX_REG_SD_CTL);
		ae9_sd_wait_stopped(chip, base, "at the hang");
		stopped |= 1u << i;
	}
	/* no stream interrupts either (keep CIE/GIE for the controller) */
	writel(readl(chip->hda_regs + AZX_REG_INTCTL) & 0xc0000000,
	       chip->hda_regs + AZX_REG_INTCTL);
	dev_err(chip->dev, "card stopped answering: stream DMA stopped (descriptors 0x%x)\n", stopped);
}

/*
 * Before a secondary bus reset (core.c, the bring-up): the Tsi381 manual
 * (10.1.2) wants no request pending when the reset bit is set, and a
 * running DMA engine keeps making them. So every stream descriptor that
 * runs and the CORB/RIRB DMA are stopped first, each waited for (HD-Audio
 * spec 3.3.35: stopped once RUN reads back 0; 4.4.1.3, 4.4.2 for the
 * rings), and the interrupts switched off. Nothing to do with the
 * controller in reset (GCTL CRST = 0 resets every DMA engine; Windows'
 * shutdown, win_shutdown(), leaves it so) or off the bus. Returns what it
 * stopped: bit n = stream descriptor n, bit 16 the CORB, bit 17 the RIRB.
 */
unsigned int creative_ae9_dma_stop_all(struct creative_chip *chip)
{
	u32 gctl = creative_hda_readl(chip, AZX_REG_GCTL);
	unsigned int n, i, t, stopped = 0;
	u32 ctl;

	if (gctl == ~0U || !(gctl & AZX_GCTL_RESET))
		return 0;
	n = ae9_sd_count(creative_hda_readw(chip, AZX_REG_GCAP));
	for (i = 0; i < n; i++) {
		unsigned int base = 0x80 + i * 0x20;

		ctl = creative_hda_readl(chip, base + AZX_REG_SD_CTL);
		if (!(ctl & SD_CTL_DMA_START))
			continue;
		/* RUN, IOCE, FEIE, DEIE off */
		creative_hda_writel(chip, base + AZX_REG_SD_CTL,
				    ctl & SD_CTL_WMASK & ~(SD_CTL_DMA_START | 0x1c));
		ae9_sd_wait_stopped(chip, base, "before the bus reset");
		stopped |= BIT(i);
	}
	if (creative_hda_readb(chip, AZX_REG_CORBCTL) & AZX_CORBCTL_RUN) {
		creative_hda_writeb(chip, AZX_REG_CORBCTL, 0);
		stopped |= BIT(16);
	}
	if (creative_hda_readb(chip, AZX_REG_RIRBCTL) & AZX_RBCTL_DMA_EN) {
		creative_hda_writeb(chip, AZX_REG_RIRBCTL, 0);
		stopped |= BIT(17);
	}
	/* up to 1 ms for both rings to report stopped (as corb_init) */
	for (t = 0; t < 100 && (stopped & (BIT(16) | BIT(17))); t++) {
		u8 corb = creative_hda_readb(chip, AZX_REG_CORBCTL);
		u8 rirb = creative_hda_readb(chip, AZX_REG_RIRBCTL);

		if (!(corb & AZX_CORBCTL_RUN) && !(rirb & AZX_RBCTL_DMA_EN))
			break;
		if (t == 99)
			dev_warn(chip->dev, "before the bus reset: the command rings didn't stop in 1 ms (CORBCTL 0x%02x, RIRBCTL 0x%02x)\n",
				 corb, rirb);
		udelay(10);
	}
	creative_hda_writel(chip, AZX_REG_INTCTL, 0);
	return stopped;
}

static void ae9_pcm_track(struct creative_chip *chip, struct snd_pcm *pcm)
{
	if (chip->npcms < ARRAY_SIZE(chip->pcms))
		chip->pcms[chip->npcms++] = pcm;
}

/* every open stream to DISCONNECTED: apps get -ENODEV, not a stream that
 * never moves; prepare and start refuse while the card is dead */
void creative_ae9_dead_work(struct work_struct *w)
{
	struct creative_chip *chip = container_of(w, struct creative_chip, dead_work);
	struct snd_pcm_substream *ss;
	unsigned int i, dir, n = 0;

	for (i = 0; i < chip->npcms; i++) {
		struct snd_pcm *pcm = chip->pcms[i];

		mutex_lock(&pcm->open_mutex);
		for (dir = 0; dir < 2; dir++)
			for (ss = pcm->streams[dir].substream; ss; ss = ss->next) {
				if (!ss->runtime)
					continue;
				snd_pcm_stream_lock_irq(ss);
				if (ss->runtime->state != SNDRV_PCM_STATE_OPEN &&
				    ss->runtime->state != SNDRV_PCM_STATE_DISCONNECTED &&
				    !snd_pcm_stop(ss, SNDRV_PCM_STATE_DISCONNECTED))
					n++;
				snd_pcm_stream_unlock_irq(ss);
			}
		mutex_unlock(&pcm->open_mutex);
	}
	dev_err(chip->dev, "card stopped answering: %u open stream(s) disconnected\n", n);
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
 * Playback offers 44.1-384 kHz with pcm_rates=2 (default); the other
 * entries are unused.
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
	ae9_sd_wait_stopped(chip, base, "after stop");
}

/* ------------------------------------------------------------------ */
/* PCM open / close						  */
/* ------------------------------------------------------------------ */

/*
 * pcm_rates: 0 = playback at 48 kHz only, as Windows' captured start
 * block. 1 = playback at 48, 96, 192 and 384 kHz, each set up the way
 * Windows does it (creative_ae9_playback_seq). 2 (default) = also 44.1
 * and 88.2 kHz, stereo, through the card's S/PDIF transmitter. These
 * played silent or garbled until the DAC got Windows' 44.1 kHz-family
 * commands (2026-11-06). PipeWire resamples 44.1 kHz files when
 * those rates are left out. Recording: 48 or 96 kHz (capture side).
 * pcm_bits: 24 (default, as Windows' default format) or 32: the samples'
 * full 32 bits go to the card (format 0x41..., which Windows also uses).
 * Both writable at runtime; they apply to streams opened afterwards.
 */
/* 44.1-384 kHz by default (Direct Mode; DSP mode takes 48/96 kHz): 48 kHz
 * family checked at 24 and 32 bit, switching back and forth, 2026-11-04;
 * 44.1/88.2 kHz at 32 bit, before and after 96 kHz, 2026-11-06 */
static unsigned int pcm_rates = 2;
module_param(pcm_rates, uint, 0644);
MODULE_PARM_DESC(pcm_rates, "Playback sample rates: 0 = 48 kHz only, 1 = 48/96/192/384 kHz, 2 = also 44.1/88.2/176.4 kHz stereo (default)");

/* 176.4 kHz goes through the converter like 44.1/88.2 (stereo): Windows'
 * converter table has it; tested 2026-11-06 (pcm_176k4 until 0.12.14). */

/*
 * 5.1: 6-channel playback in both modes (Windows' order FL FR FC LFE RL RR
 * -> card's RCA, C/Sub jack, Rear jack; checked by ear 2026-11-04) and
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
 * 6 channels up to 192 kHz, and at 384 kHz only 2: Creative's 5.1 maximum
 * is 192 kHz and Windows never offers 5.1 at 384 kHz. Direct Mode off (DSP
 * mode) opens at 48 or 96 kHz.
 *
 * 5.1 at 96 kHz used to stutter more and more as it played: the start
 * block gave it the stereo router setup (front slots only) and the 64 KB
 * buffer was barely one quantum. Both fixed 2026-11-05: all six speakers
 * right, no errors over 2 min. The 96 kHz slot layout matches Windows'
 * exactly (capture 2026-11-05, 02:01:48); at 192 kHz Windows' layout is
 * our 96 kHz one with twice the slots (02:02:20). 96 kHz through the DSP,
 * as Windows offers: stereo and 5.1 with SBX, live Direct Mode switches,
 * no echo (2026-11-06). Options until 0.12.14 (surround_96k,
 * surround_192k, dsp_96k), always on.
 */
#define AE9_SURROUND_RATE_MAX	192000

/*
 * 5.1 rates: the 48 kHz family up to AE9_SURROUND_RATE_MAX. 44.1/88.2 kHz
 * go through the S/PDIF transmitter, which carries one stereo pair
 * (Windows offers them in stereo only, capture 2026-11-05 format probes).
 */
static unsigned int ae9_surround_rates(unsigned int *list)
{
	static const unsigned int r[] = { 48000, 96000, 192000 };
	unsigned int i, n = 0;

	for (i = 0; i < ARRAY_SIZE(r); i++)
		if (r[i] <= AE9_SURROUND_RATE_MAX)
			list[n++] = r[i];
	return n;
}

static int ae9_rule_rate_by_ch(struct snd_pcm_hw_params *params,
			       struct snd_pcm_hw_rule *rule)
{
	struct snd_interval *ch = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_interval *rate = hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	unsigned int list[3];

	if (ch->min <= 2)
		return 0;
	return snd_interval_list(rate, ae9_surround_rates(list), list, 0);
}

static int ae9_rule_ch_by_rate(struct snd_pcm_hw_params *params,
			       struct snd_pcm_hw_rule *rule)
{
	struct snd_interval *ch = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_interval *rate = hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval t;
	unsigned int list[3], i, n = ae9_surround_rates(list);

	for (i = 0; i < n; i++)
		if (snd_interval_test(rate, list[i]))
			return 0;	/* a 5.1 rate is still possible */
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

/* ------------------------------------------------------------------ */
/* Optical S/PDIF (codec 2, a plain HDA codec: Windows drives it with	  */
/* Microsoft's generic driver). Out: converter 0x05 -> pin 0x0c. In:	  */
/* pin 0x0e -> converter 0x09. The cold-boot replay already powers	  */
/* both up and enables them (DIGEN, keep-alive, pin out/in), so the	  */
/* port sends a locked, silent signal until a converter gets a stream.	  */
/* ------------------------------------------------------------------ */
#define AE9_SPDIF_CAD		2
#define AE9_SPDIF_OUT_NID	0x05
#define AE9_SPDIF_IN_NID	0x09
#define AE9_SPDIF_IN_PIN	0x0e

/* one verb to codec 2: 12-bit verb + 8-bit payload, or (verb < 0x10)
 * a 4-bit verb + 16-bit payload (0x2 = converter format) */
static int ae9_spdif_verb_resp(struct creative_chip *chip, unsigned int nid,
			       unsigned int verb, unsigned int payload, u32 *resp)
{
	u32 v = (AE9_SPDIF_CAD << 28) | (nid << 20);

	v |= verb < 0x10 ? (verb << 16) | (payload & 0xffff) :
			   (verb << 8) | (payload & 0xff);
	if (chip->corb_buf)
		return creative_ae9_corb_send_verb_sync(chip, v, resp);
	return creative_ae9_icw_send_verb(chip, v, resp);
}

static int ae9_spdif_verb(struct creative_chip *chip, unsigned int nid,
			  unsigned int verb, unsigned int payload)
{
	u32 resp;

	return ae9_spdif_verb_resp(chip, nid, verb, payload, &resp);
}

/*
 * Optical output (codec 2's S/PDIF converter 0x05). Two ways in:
 * - the separate optical PCM (device 4, "AE-9 Optical Out"; DarkNikGr):
 *   its own stream, rate and format, and IEC958 status (non-audio for
 *   AC-3/DTS passthrough); while it's prepared it owns 0x05
 * - otherwise 0x05 mirrors the main output, as since 0.9.0: HDA lets two
 *   converters take one stream, so 0x05 joins the playback's (same tag,
 *   same format)
 * S/PDIF carries stereo up to 24-bit and 192 kHz: 2-channel streams of up
 * to 24 bits only (384 kHz goes out as 4 channels, 5.1 as 6). At 32-bit
 * the converter garbles it (tested 2026-11-05), so a 32-bit main stream
 * isn't mirrored. Not through the DSP: the stream as sent, before SBX.
 */
static bool spdif_out = true;
module_param(spdif_out, bool, 0644);
/*
 * Mirror off by default since 0.12.1 (DarkNikGr: with several speakers on
 * different outputs, they shouldn't all play at once): optical plays only
 * what's sent to the optical PCM. "Optical Out Mirror Switch" (or
 * optical_mirror=1) brings back the 0.9-0.12.0 behaviour: the main output
 * on optical while the optical PCM is unused.
 */
static bool optical_mirror;
module_param(optical_mirror, bool, 0644);
MODULE_PARM_DESC(optical_mirror, "Optical out mirrors the main output while nothing plays to the optical PCM (0 = off, default since 0.12.1); also 'Optical Out Mirror Switch'");
MODULE_PARM_DESC(spdif_out, "Optical out on (1, default): the separate optical PCM, or else a mirror of the main output; also 'Optical Out Playback Switch'");

/* 0x70d, digital converter 1: b0 DigEn, b2 VCFG, b4 COPY, b5 /AUDIO
 * (non-audio). The boot replay leaves 0x15 (Windows' PCM value). */
#define AE9_SPDIF_DIG1_PCM	0x15
#define AE9_SPDIF_DIG1_NONAUDIO	0x20

/*
 * Bind 0x05 to what should play (caller holds spdif_mutex). A new binding
 * goes in Windows' order (captures 6/7, notes 32v/37v): 0x70d with DigEn
 * off, the format, 0x70d on (with the non-audio bit for passthrough), then
 * the stream tag. The same binding again only re-sends format and tag: a
 * resume's boot replay resets 0x05 behind our back.
 */
static void ae9_spdif_out_apply_locked(struct creative_chip *chip)
{
	bool opt = chip->opt_format != 0;
	u16 fmt = opt ? chip->opt_format : chip->spdif_mirror ? chip->pb_format : 0;
	unsigned int tag = opt ? CREATIVE_OPT_STREAM_TAG : CREATIVE_PCM_STREAM_TAG;
	bool nonaudio = opt && (chip->spdif_aes[0] & IEC958_AES0_NONAUDIO);
	u8 dig1 = AE9_SPDIF_DIG1_PCM | (nonaudio ? AE9_SPDIF_DIG1_NONAUDIO : 0);
	u32 want = 0;

	/* 2 channels, up to 32-bit, and not 176.4 kHz (44.1k x 4: base bit 14,
	 * multiplier 3 in bits 13:11), which converter 0x05 can't take; a
	 * 176.4 kHz main stream simply isn't mirrored */
	if (chip->spdif_out && fmt && (fmt & 0xf) == 1 && ((fmt >> 4) & 0x7) <= 3 &&
	    ((fmt >> 11) & 0xf) != 0xb)
		want = (u32)nonaudio << 24 | (u32)fmt << 8 | (tag << 4);
	if (want && want != chip->spdif_out_sent) {
		/* unbound first: Windows changes format and flag only on an
		 * unbound converter. Switching from the mirror (tag 1, 24-bit)
		 * straight to a 16-bit optical stream without it played the
		 * left channel right and the right as full-scale noise
		 * (2026-11-08) */
		if (chip->spdif_out_sent)
			ae9_spdif_verb(chip, AE9_SPDIF_OUT_NID, 0x706, 0);
		ae9_spdif_verb(chip, AE9_SPDIF_OUT_NID, 0x70d, dig1 & ~1);
		ae9_spdif_verb(chip, AE9_SPDIF_OUT_NID, 0x2, fmt);
		ae9_spdif_verb(chip, AE9_SPDIF_OUT_NID, 0x70d, dig1);
		ae9_spdif_verb(chip, AE9_SPDIF_OUT_NID, 0x706, tag << 4);
	} else if (want) {
		ae9_spdif_verb(chip, AE9_SPDIF_OUT_NID, 0x2, fmt);
		ae9_spdif_verb(chip, AE9_SPDIF_OUT_NID, 0x706, tag << 4);
	} else if (chip->spdif_out_sent) {
		ae9_spdif_verb(chip, AE9_SPDIF_OUT_NID, 0x706, 0);
		if (chip->spdif_out_sent >> 24)		/* back to PCM */
			ae9_spdif_verb(chip, AE9_SPDIF_OUT_NID, 0x70d, AE9_SPDIF_DIG1_PCM);
	}
	if (want != chip->spdif_out_sent)
		ae9_dbg(chip, "optical out: %s (format 0x%04x%s)\n",
			!want ? "no stream (silent)" : opt ? "separate stream" :
			"following the main stream", (want >> 8) & 0xffff,
			nonaudio ? ", non-audio" : "");
	chip->spdif_out_sent = want;
}

static void ae9_spdif_out_apply(struct creative_chip *chip)
{
	mutex_lock(&chip->spdif_mutex);
	ae9_spdif_out_apply_locked(chip);
	mutex_unlock(&chip->spdif_mutex);
}

static int spdif_out_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);

	uv->value.integer.value[0] = chip->spdif_out;
	return 0;
}

static int spdif_out_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	bool on = !!uv->value.integer.value[0];

	if (creative_ae9_dead(chip))
		return -ENODEV;

	if (on == chip->spdif_out)
		return 0;
	chip->spdif_out = on;
	spdif_out = on;
	ae9_spdif_out_apply(chip);	/* takes effect on a playing stream */
	return 1;
}

static int spdif_mirror_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);

	uv->value.integer.value[0] = chip->spdif_mirror;
	return 0;
}

static int spdif_mirror_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	bool on = !!uv->value.integer.value[0];

	if (creative_ae9_dead(chip))
		return -ENODEV;

	if (on == chip->spdif_mirror)
		return 0;
	mutex_lock(&chip->spdif_mutex);
	chip->spdif_mirror = on;
	optical_mirror = on;
	ae9_spdif_out_apply_locked(chip);	/* takes effect on a playing stream */
	mutex_unlock(&chip->spdif_mutex);
	return 1;
}

static const struct snd_kcontrol_new spdif_mirror_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Optical Out Mirror Switch",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info = snd_ctl_boolean_mono_info,
	.get = spdif_mirror_get,
	.put = spdif_mirror_put,
};

/* not "IEC958 ...": PipeWire's analog output paths switch that one off */
static const struct snd_kcontrol_new spdif_out_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Optical Out Playback Switch",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info = snd_ctl_boolean_mono_info,
	.get = spdif_out_get,
	.put = spdif_out_put,
};

/* ------------------------------------------------------------------ */
/* Native DSD64 (pcm_dsd, on by default since 0.12.15)		  */
/* ------------------------------------------------------------------ */

/*
 * How Creative's Windows driver plays DSD64 (its DSD ASIO driver
 * CtxDsd64.dll with CtxHda.sys 6.0.105.0065's DSD stream class, decompiled;
 * .tmp/notes/dsd-windows.md): natively, not as DoP, and never through the
 * DSP or the CA0132's I2S. The HDA playback stream runs at 88.2 kHz with
 * six 16-bit channels (format 0x4815) and the CA0113's own I2S block in
 * BAR2 takes it from the stream: its channel 2 the stream's channels 0-1
 * (left), channel 3 channels 2-3 (right), each channel's data on a GPIO pin
 * (6, 7) switched to its I2S function, 16-bit words at rate code 88.2 kHz:
 * 32 DSD bits per channel and 88.2 kHz frame, 2.8224 Mbit/s = DSD64. The
 * ES9038 takes DSD input (48/01 = 0xc3) and GPIO pin 1 goes high (it
 * switches the ES9038's data lines over [inferred]; uv44: pin 1 high
 * silenced PCM on the card's RCA out and the headphones). Only the ES9038's
 * outputs (the card's RCA out, the head unit's headphone jack) can play it.
 *
 * Here: playback device 0 also offers SNDRV_PCM_FORMAT_DSD_U32_BE, 2
 * channels at 88200 (ALSA's DSD64 rate for 32-bit words), in Direct Mode,
 * without mmap (each 8-byte ALSA frame becomes a 12-byte link frame: the
 * copy op below). Enter and leave follow Windows' order (ae9_dsd_enter(),
 * ae9_dsd_exit()). On by default since 0.12.15 (opt-in in 0.12.14, until
 * card-tested); pcm_dsd=0 turns it off. Read at stream open.
 */
static bool pcm_dsd = true;
module_param(pcm_dsd, bool, 0644);
MODULE_PARM_DESC(pcm_dsd, "Native DSD64 on playback device 0 (DSD_U32_BE, 2 ch, rate 88200, no mmap; Direct Mode only, card RCA out and headphones); 1 = on (default), 0 = off (DSD_U32_BE not offered). Read at stream open");

#define AE9_DSD_RATE		88200	/* ALSA's DSD64 rate for 32-bit DSD words */
#define AE9_DSD_FMT		0x4815	/* SDnFMT: 44.1 kHz x 2, 16 bit, 6 channels */
#define AE9_DSD_ALSA_FRAME	8	/* bytes: DSD_U32_BE, 2 channels */
#define AE9_DSD_HW_FRAME	12	/* bytes: the link frame, 6 x 16 bit */
/* a period: whole steps of 32 frames, 384 bytes on the link (3 x 128: every
 * BDL entry starts on the 128-byte boundary the HD-Audio spec requires) */
#define AE9_DSD_PERIOD_STEP	32
#define AE9_DSD_SILENCE		0x69	/* ALSA's DSD silence byte (pcm_misc.c) */
#define AE9_DSD_FILL		0x96	/* Windows' fill slots 4 and 5: 0x9696 */
/* after RUN and the I2S on: the DAC locks onto the DSD clock before the
 * unmute (ours; Windows unmutes without a wait) */
#define AE9_DSD_UNMUTE_MS	150

/* the front DAC, an ES9038Q2M (creative_ae9_seq_run.c has its other registers) */
#define AE9_DSD_DAC		0x48
#define ES9038_REG_INPUT	0x01	/* 0xc0 32-bit I2S, 0xc1 S/PDIF, 0xc3 DSD */
#define ES9038_REG_MUTE		0x07	/* bit 0 mute (bits 7:5 the filter: dsp.c keeps the user's) */
#define ES9038_REG_SYNC		0x0a
#define ES9038_REG_SPDIF	0x0b	/* S/PDIF input: 0x20 = DATA2 */

/*
 * BAR2 0x100/0x104: Creative's wrapper around the CA0113's I2S block
 * (Synopsys DW_apb_i2s at BAR2 0x400; Windows' CtxHdb "HdIs" and "HdGp"
 * interfaces, notes §4). 0x100: bits 3:0 GPIO pins 4-7 to their I2S
 * function (FUN_000138c0), bits 7:4 and 15:12 I2S channels 0-3 on (FUN_00014f24,
 * FUN_00014ea0), bit 8 the I2S output (CtxHda FUN_00035680; set at
 * CtxHdb's init, 0x700 while PCM plays), bit 28 24-bit words (FUN_0001506c),
 * bits 31:29 the rate (FUN_000150b8: 48k 0, 44.1k 1, 96k 2, 88.2k 3, 192k
 * 4). 0x104: byte n = the HDA stream tag << 4 | the stream's first channel
 * for I2S channel n (FUN_00015140).
 */
#define AE9_I2S_CTL		0x100
#define AE9_I2S_CTL_PINFN(pin)	BIT((pin) - 4)
#define AE9_I2S_CTL_CH_A(ch)	BIT(4 + (ch))
#define AE9_I2S_CTL_OUT		BIT(8)
#define AE9_I2S_CTL_CH_B(ch)	BIT(12 + (ch))
#define AE9_I2S_CTL_24BIT	BIT(28)
#define AE9_I2S_CTL_RATE	GENMASK(31, 29)
#define AE9_I2S_RATE_88K2	3
#define AE9_I2S_MAP		0x104
#define AE9_I2S_TFF(ch)		(0x454 + 0x40 * (ch))	/* DW_apb_i2s TFFn: transmit FIFO flush */
#define AE9_DSD_CH_L		2	/* CBoard +0x79, +0x7a: the I2S channels DSD uses */
#define AE9_DSD_CH_R		3
#define AE9_DSD_PIN(ch)		((ch) + 4)	/* their data pins: GPIO 6, 7 */

/*
 * The link frame (CtxDsd64.dll FUN_0040daa8, the DSD copy routine): six
 * little-endian 16-bit slots per 88.2 kHz frame: L bits t0-15, L t16-31, R
 * t0-15, R t16-31, then 0x9696 twice (BAR2 0x104 maps only slots 0-1 and
 * 2-3, so 4 and 5 are never played). In each slot t0, the oldest bit, is bit
 * 15: the DLL gets ASIO's DSDInt8LSB1 bytes (oldest bit = a byte's LSB) and
 * bit-reverses every 16-bit word (FUN_00411724: new = rev8(b1) | rev8(b0) <<
 * 8), which puts the first byte's oldest bit at bit 15 and the second byte's
 * newest at bit 0.
 * ALSA's DSD_U32_BE word is, per channel, b0 b1 b2 b3 in memory: b0 the
 * oldest byte, each byte MSB first in time (MPD, usb-audio). So slot a = b0
 * << 8 | b1 and slot b = b2 << 8 | b3, little-endian in memory: b1 b0 b3 b2.
 * ALSA's DSD silence (0x69 bytes) gives 0x6969 in the data slots, the same
 * as the DLL's 0x96 prefill after its bit reverse.
 */
static void ae9_dsd_pack(u8 *dst, const u8 *src, unsigned long frames)
{
	for (; frames; frames--, src += AE9_DSD_ALSA_FRAME, dst += AE9_DSD_HW_FRAME) {
		dst[0] = src[1];	/* L slot a: b0 << 8 | b1 */
		dst[1] = src[0];
		dst[2] = src[3];	/* L slot b: b2 << 8 | b3 */
		dst[3] = src[2];
		dst[4] = src[5];	/* R slot a */
		dst[5] = src[4];
		dst[6] = src[7];	/* R slot b */
		dst[7] = src[6];
		memset(dst + 8, AE9_DSD_FILL, 4);
	}
}

static void ae9_dsd_silence(u8 *dst, unsigned long frames)
{
	for (; frames; frames--, dst += AE9_DSD_HW_FRAME) {
		memset(dst, AE9_DSD_SILENCE, 8);
		memset(dst + 8, AE9_DSD_FILL, 4);
	}
}

static bool ae9_pcm_is_dsd(const struct snd_pcm_runtime *runtime)
{
	return runtime->format == SNDRV_PCM_FORMAT_DSD_U32_BE;
}

/*
 * The copy and silence ops (the DSD ops table only, so only streams opened
 * with pcm_dsd on use them): DSD repacked into the link frame at 12/8 the
 * offset; anything else as ALSA's own default_write_copy() and silence.
 */
static int creative_ae9_pcm_copy(struct snd_pcm_substream *substream, int channel,
				 unsigned long pos, struct iov_iter *iter, unsigned long bytes)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	u8 buf[32 * AE9_DSD_ALSA_FRAME];
	u8 *dst;

	if (!ae9_pcm_is_dsd(runtime))
		return copy_from_iter(runtime->dma_area + pos, bytes, iter) == bytes ? 0 : -EFAULT;
	if (pos % AE9_DSD_ALSA_FRAME || bytes % AE9_DSD_ALSA_FRAME)
		return -EINVAL;
	dst = runtime->dma_area + pos / AE9_DSD_ALSA_FRAME * AE9_DSD_HW_FRAME;
	while (bytes) {
		unsigned long n = min_t(unsigned long, bytes, sizeof(buf));

		if (copy_from_iter(buf, n, iter) != n)
			return -EFAULT;
		ae9_dsd_pack(dst, buf, n / AE9_DSD_ALSA_FRAME);
		dst += n / AE9_DSD_ALSA_FRAME * AE9_DSD_HW_FRAME;
		bytes -= n;
	}
	return 0;
}

static int creative_ae9_pcm_fill_silence(struct snd_pcm_substream *substream, int channel,
					 unsigned long pos, unsigned long bytes)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	if (!ae9_pcm_is_dsd(runtime))
		return snd_pcm_format_set_silence(runtime->format, runtime->dma_area + pos,
						  bytes_to_samples(runtime, bytes));
	ae9_dsd_silence(runtime->dma_area + pos / AE9_DSD_ALSA_FRAME * AE9_DSD_HW_FRAME,
			bytes / AE9_DSD_ALSA_FRAME);
	return 0;
}

/* the format mask holds DSD and not PCM: the DSD rules apply */
static bool ae9_dsd_only(const struct snd_pcm_hw_params *params)
{
	const struct snd_mask *f = hw_param_mask_c(params, SNDRV_PCM_HW_PARAM_FORMAT);

	return snd_mask_test_format(f, SNDRV_PCM_FORMAT_DSD_U32_BE) &&
	       !snd_mask_test_format(f, SNDRV_PCM_FORMAT_S32_LE);
}

/*
 * DSD: all 32 bits of a DSD_U32_BE word are DSD data. The open's msbits
 * constraint (24 PCM bits) goes by the sample width alone, and DSD_U32_BE
 * is 32 bits wide too, so DSD streams said 24 significant bits (aplay -v,
 * mpd). Once the format is DSD only, no msbits: the core then reports the
 * format's width, 32. Added after that constraint and on the same
 * SAMPLE_BITS dependency, so it runs after it on every pass it does, and
 * again when the format narrows; PCM and a mixed format mask keep the
 * constraint's value.
 */
static int ae9_rule_dsd_msbits(struct snd_pcm_hw_params *params, struct snd_pcm_hw_rule *rule)
{
	if (ae9_dsd_only(params))
		params->msbits = 0;
	return 0;
}

/* DSD: no other rate or channel count (rule->private: 88200 or 2) */
static int ae9_rule_dsd_fix(struct snd_pcm_hw_params *params, struct snd_pcm_hw_rule *rule)
{
	struct snd_interval t = { .integer = 1 };

	if (!ae9_dsd_only(params))
		return 0;
	t.min = t.max = (unsigned long)rule->private;
	return snd_interval_refine(hw_param_interval(params, rule->var), &t);
}

/* DSD: periods in whole AE9_DSD_PERIOD_STEPs */
static int ae9_rule_dsd_period(struct snd_pcm_hw_params *params, struct snd_pcm_hw_rule *rule)
{
	struct snd_interval *p = hw_param_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
	struct snd_interval t;
	unsigned int lo, hi;

	if (!ae9_dsd_only(params))
		return 0;
	lo = p->min + (p->openmin ? 1 : 0);
	hi = p->max - (p->openmax && p->max ? 1 : 0);
	if (lo > UINT_MAX - AE9_DSD_PERIOD_STEP)
		return -EINVAL;
	snd_interval_any(&t);
	t.min = roundup(lo, AE9_DSD_PERIOD_STEP);
	t.max = rounddown(hi, AE9_DSD_PERIOD_STEP);
	t.integer = 1;
	return snd_interval_refine(p, &t);
}

/*
 * DSD: a buffer of 12-byte link frames that fits the preallocated one, in
 * whole AE9_DSD_PERIOD_STEPs at both ends: the buffer is a whole number of
 * periods, so any other size can't be met, and an end that can't be met
 * (21845 frames, from the size cap alone) made mpd's
 * snd_pcm_hw_params_set_buffer_time_near() fail with EINVAL when it asked
 * for more than the maximum (card test 2026-11-19).
 */
static int ae9_rule_dsd_buffer(struct snd_pcm_hw_params *params, struct snd_pcm_hw_rule *rule)
{
	struct snd_interval *b = hw_param_interval(params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
	struct snd_interval t;
	unsigned int lo, hi;

	if (!ae9_dsd_only(params))
		return 0;
	/* the cap itself in 8 x 32 frames, so the largest buffer splits into
	 * 2, 4 or 8 whole periods: 21824 (32 x 682) only made 2, and mpd, asking
	 * for 4 (buffer_time / 4), got 2 of 124 ms, fed a quarter buffer per
	 * wakeup and ran it to 30 frames from empty: a click every period (card
	 * test 2026-11-19). 21760 = 8 x 2720. For every upper end above it, not
	 * only the preallocated size: an end between 21761 and 21844 (a buffer
	 * limit set before the format) rounded down to 21792 (3 periods) or
	 * 21824 (2) (review 2026-11-20). */
	lo = b->min + (b->openmin ? 1 : 0);
	hi = min_t(unsigned int, b->max - (b->openmax && b->max ? 1 : 0),
		   rounddown(CREATIVE_PCM_BUF_MAX / AE9_DSD_HW_FRAME, AE9_DSD_PERIOD_STEP * 8));
	if (lo > UINT_MAX - AE9_DSD_PERIOD_STEP)
		return -EINVAL;
	snd_interval_any(&t);
	t.min = roundup(lo, AE9_DSD_PERIOD_STEP);
	t.max = rounddown(hi, AE9_DSD_PERIOD_STEP);
	t.integer = 1;
	return snd_interval_refine(b, &t);
}

/* DSD: read/write access only (the copy op repacks every frame; no mmap) */
static int ae9_rule_dsd_access(struct snd_pcm_hw_params *params, struct snd_pcm_hw_rule *rule)
{
	struct snd_mask m;

	if (!ae9_dsd_only(params))
		return 0;
	snd_mask_none(&m);
	snd_mask_set(&m, (__force unsigned int)SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	return snd_mask_refine(hw_param_mask(params, SNDRV_PCM_HW_PARAM_ACCESS), &m);
}

/* no DSD where it can't be: another rate, more than 2 channels, mmap */
static int ae9_rule_dsd_format(struct snd_pcm_hw_params *params, struct snd_pcm_hw_rule *rule)
{
	struct snd_mask *f = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);
	const struct snd_interval *rate = hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_RATE);
	const struct snd_interval *ch = hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	const struct snd_mask *acc = hw_param_mask_c(params, SNDRV_PCM_HW_PARAM_ACCESS);
	struct snd_mask m;

	if (!snd_mask_test_format(f, SNDRV_PCM_FORMAT_DSD_U32_BE))
		return 0;
	if (snd_interval_test(rate, AE9_DSD_RATE) && snd_interval_test(ch, 2) &&
	    snd_mask_test(acc, (__force unsigned int)SNDRV_PCM_ACCESS_RW_INTERLEAVED))
		return 0;
	m = *f;
	snd_mask_reset(&m, (__force unsigned int)SNDRV_PCM_FORMAT_DSD_U32_BE);
	return snd_mask_refine(f, &m);
}

/*
 * Whole periods, for every stream with a BDL (playback, optical out, the
 * captures): the buffer is periods x period, one BDL entry per period, each
 * a whole number of rule->private bytes (128: the boundary the HD-Audio spec
 * wants a BDL entry's address on). So the buffer sizes that can be met are
 * a sparse lattice, not a range. ALSA's own rule sees BUFFER_SIZE =
 * PERIOD_SIZE x PERIODS only as the product of two ranges, and while both
 * are open an end of the buffer range can sit on a size no split reaches
 * (S32_LE stereo: 17648 frames = 16 x 1103, 1103 prime).
 * snd_pcm_hw_params_set_buffer_time_near() and _size_near() take an end of
 * the range, so they failed with EINVAL: 108 of 471 buffer times from 12 to
 * 247 ms at 88.2 kHz (111 in DSD), mpd's buffer_time 200000 among them
 * (card test 2026-11-19).
 *
 * This rule moves each end of the buffer range inward to the nearest size
 * that a whole number of periods inside the current period and periods
 * ranges makes, so near() lands on the closest buffer that can be met. It
 * only removes sizes nothing can make, so a request that negotiated before
 * gets the same result, with two exceptions (harness, 2026-11-20): just
 * below the largest buffer near() now sees one size above the request and
 * takes it outright (alsa-lib does that when that side is a single value:
 * 32737-32743 frames give 32768, not 32736), and a client that reads a
 * range end reads one it can get (What U Hear 6 ch offered 10922 frames,
 * which no split makes). The period and periods ends need no such rule:
 * they are quotients, and the integer rounding of BUFFER_SIZE / PERIODS and
 * BUFFER_SIZE / PERIOD_SIZE already settles them on values a split reaches.
 *
 * The period step in frames: the alignment over the frame size's largest
 * power-of-two factor (S32_LE 2 or 6 ch: 16 frames; S16_LE stereo: 32), and
 * AE9_DSD_PERIOD_STEP for DSD (powers of two, so the larger is a multiple
 * of both). 1 while the frame size is still open: still correct, less tight.
 *
 * Playback (AE9_WP_PLAYBACK) also steers buffer-first clients to 4 periods.
 * mpd asks for a buffer time, then a quarter of what it got as the period;
 * at 44.1 kHz its default 500 ms landed on 22048 frames, which only 2
 * periods make (22048 / 16 = 1378 = 2 x 13 x 53), so it got 2 of 250 ms,
 * and at times ran the buffer down to 29 frames from empty. The DMA reads
 * ~0.7 ms ahead of the position the pointer reports, so below that it plays
 * audio it fetched before mpd wrote it: a crackle and no xrun (card test
 * 2026-11-20; the DSD64 clicks the 21760 cap fixed were the same). 3
 * periods leaves such a client a third of the buffer at the wakeup it
 * counts on, and mpd and aplay ask for 4, so the target is 4.
 *
 * So an end that the buffer range's own bound sets (a buffer request, or
 * the size cap at the top) moves on to the nearest size 4 periods make, if
 * that is less than one 4-period step away (4 x the period step: 64 frames
 * for S32_LE stereo): near() looks at the ends either side of the request,
 * so it lands on a 4-period size, and the quarter period mpd asks for next
 * is exactly one. Further than a step, the end stays where it was: below
 * 4 x the smallest period (4096 bytes: 46 ms at 44.1 kHz S32_LE stereo, up
 * to 93 ms for S16_LE optical) the nearest 4-period size is far above a
 * small request, and moving the end to it made near() take a 2-period size
 * under the request (48 kHz buffer_size_near(1920): 1888 = 2 x 944 instead
 * of 1920 = 3 x 640). Small buffers negotiate as without this.
 *
 * Moving an end removes the sizes it passes over from the range for good
 * (a refine never widens it again): up to 4 period steps per end, so an
 * explicit set_buffer_size_min()/_max() or _time_min()/_max() pair that
 * leaves only such sizes, which a near() probe can't be told from inside a
 * refine, can now fail with EINVAL. At 6 ch the cap moves the top from
 * 10912 (2 x 5456) to 10880 (4 x 2720), so 10881-10912 are gone outright:
 * set_buffer_size(10912) and set_period_size(5456) fail there. No client
 * modelled (PipeWire, PulseAudio, mpd, aplay, speaker-test, Kodi,
 * GStreamer, mpv, ffmpeg, RetroArch, snd_pcm_set_params, JACK, SDL, OpenAL
 * Soft, Wine) calls in such an order. Sizes between the ends stay. It is
 * left out where it would take a count or a period away from a client:
 * - the periods range already below 4 (set_periods 2, PipeWire's
 *   period-num=2 and its IRQ mode's 3, pcm_periods 2 or 3);
 * - an end that the period and periods ranges set (b_lo at the smallest
 *   period x the fewest periods, b_hi at the largest x the most): moving it
 *   would cut periods off a client that picked its period first (PipeWire's
 *   1024 frames then period-num=2 must still give 2 x 1024);
 * - a move that would narrow the periods range ALSA derives from the end.
 * Exactly 4, not 4 or more: a size only 5 or 7 periods make left mpd's
 * period_time_near() a single value above its request, which alsa-lib takes
 * outright, and it ended on 2. The same shortcut works the other way for a
 * client that sets the buffer, then an explicit period: a 4-period buffer
 * often has only 2 x as the next split above a period between B/4 and B/2
 * (PulseAudio with tsched=0 and default-fragments=3, 25 ms: 2 periods on 21
 * of 25 streams, 4 before; its default timer-based path is unaffected).
 * Captures keep the plain rule (the read-ahead is a playback matter).
 */

/* rule->private of ae9_rule_whole_periods: the alignment in bytes (a power
 * of two, at most 128), plus AE9_WP_PLAYBACK on the playback streams */
#define AE9_WP_ALIGN		0xffffUL
#define AE9_WP_PLAYBACK		0x10000UL
#define AE9_PLAY_PERIODS	4

/* the smallest buffer of at least b_lo and the largest of at most b_hi that
 * k_lo..k_hi whole periods of p_lo..p_hi frames (in @step steps) make; *lo
 * > *hi when there is none */
static void ae9_split_ends(u64 b_lo, u64 b_hi, u64 p_lo, u64 p_hi, u64 step,
			   unsigned int k_lo, unsigned int k_hi, u64 *lo, u64 *hi)
{
	unsigned int k;

	*lo = U64_MAX;
	*hi = 0;
	/* per period count k: the smallest period that makes at least b_lo
	 * and the largest that keeps to b_hi, both in whole steps */
	for (k = k_lo; k <= k_hi; k++) {
		u64 small = max(p_lo, round_up(div_u64(b_lo + k - 1, k), step));
		u64 big = min(p_hi, round_down(div_u64(b_hi, k), step));

		if (small > big)
			continue;
		*lo = min(*lo, small * k);
		*hi = max(*hi, big * k);
	}
}

/* the fewest periods (@least) a buffer of @b frames needs with periods up to
 * @p frames, or the most it takes with periods of at least @p (0: no
 * limit), inside the current count limit @n: what ALSA's PERIODS =
 * BUFFER_SIZE / PERIOD_SIZE rule makes of a buffer range end */
static unsigned int ae9_counts(u64 b, u64 p, unsigned int n, bool least)
{
	if (least)
		return max_t(u64, n, div_u64(b + p - 1, (u32)p));
	return p ? min_t(u64, n, div_u64(b, (u32)p)) : n;
}

static int ae9_rule_whole_periods(struct snd_pcm_hw_params *params, struct snd_pcm_hw_rule *rule)
{
	struct snd_interval *b = hw_param_interval(params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
	const struct snd_interval *p = hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
	const struct snd_interval *n = hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_PERIODS);
	const struct snd_interval *fb = hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_FRAME_BITS);
	unsigned long how = (unsigned long)rule->private;
	unsigned int align_bits = (how & AE9_WP_ALIGN) * 8;
	unsigned int n_lo, n_hi;
	u64 step = 1, p_lo, p_hi, b_lo, b_hi, lo, hi;
	struct snd_interval t;

	if (snd_interval_single(fb) && snd_interval_value(fb) > 0) {
		unsigned int bits = snd_interval_value(fb);

		step = align_bits / min(bits & -bits, align_bits);
	}
	if (ae9_dsd_only(params))
		step = max_t(u64, step, AE9_DSD_PERIOD_STEP);

	n_lo = max(n->min + n->openmin, 1U);
	n_hi = n->max - (n->openmax && n->max ? 1 : 0);
	if (n_hi < n_lo || n_hi - n_lo > 2 * CREATIVE_PCM_BDL_MAX)
		return 0;	/* not narrowed to the hardware's counts yet */
	p_lo = round_up((u64)p->min + p->openmin, step);
	p_hi = round_down((u64)p->max - (p->openmax && p->max ? 1 : 0), step);
	b_lo = (u64)b->min + b->openmin;
	b_hi = (u64)b->max - (b->openmax && b->max ? 1 : 0);

	ae9_split_ends(b_lo, b_hi, p_lo, p_hi, step, n_lo, n_hi, &lo, &hi);
	if (lo > hi)
		return -EINVAL;	/* no split at all */

	/* playback, while the periods range still reaches 4: an end that the
	 * buffer range's own bound sets moves on to the nearest size 4 periods
	 * make, if it is less than one 4-period step away and leaves the
	 * periods range as the plain end does (see above) */
	if ((how & AE9_WP_PLAYBACK) && n_lo < AE9_PLAY_PERIODS && n_hi >= AE9_PLAY_PERIODS &&
	    p_hi) {
		u64 lo4, hi4;

		ae9_split_ends(b_lo, b_hi, p_lo, p_hi, step, AE9_PLAY_PERIODS,
			       AE9_PLAY_PERIODS, &lo4, &hi4);
		if (lo4 <= hi4) {
			if (b_lo > p_lo * n_lo && lo4 - lo < AE9_PLAY_PERIODS * step &&
			    ae9_counts(lo4, p_hi, n_lo, true) == ae9_counts(lo, p_hi, n_lo, true))
				lo = lo4;
			if (b_hi < p_hi * n_hi && hi - hi4 < AE9_PLAY_PERIODS * step &&
			    ae9_counts(hi4, p_lo, n_hi, false) == ae9_counts(hi, p_lo, n_hi, false))
				hi = hi4;
		}
	}
	snd_interval_any(&t);
	t.min = lo;
	t.max = hi;
	t.integer = 1;
	return snd_interval_refine(b, &t);
}

/* the rule above, for a stream whose periods are whole @align-byte steps
 * (@align a power of two); @playback: steer buffer-first clients to 4 */
static int ae9_whole_periods(struct snd_pcm_runtime *runtime, unsigned int align, bool playback)
{
	unsigned long how = align | (playback ? AE9_WP_PLAYBACK : 0);

	return snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
				   ae9_rule_whole_periods, (void *)how,
				   SNDRV_PCM_HW_PARAM_BUFFER_SIZE, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
				   SNDRV_PCM_HW_PARAM_PERIODS, SNDRV_PCM_HW_PARAM_FRAME_BITS, -1);
}

static int ae9_dsd_rules(struct snd_pcm_runtime *runtime)
{
	int err;

	err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_FORMAT, ae9_rule_dsd_format, NULL,
				  SNDRV_PCM_HW_PARAM_RATE, SNDRV_PCM_HW_PARAM_CHANNELS,
				  SNDRV_PCM_HW_PARAM_ACCESS, -1);
	if (!err)
		err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_RATE, ae9_rule_dsd_fix,
					  (void *)(unsigned long)AE9_DSD_RATE,
					  SNDRV_PCM_HW_PARAM_FORMAT, -1);
	if (!err)
		err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS, ae9_rule_dsd_fix,
					  (void *)2UL, SNDRV_PCM_HW_PARAM_FORMAT, -1);
	if (!err)
		err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_ACCESS, ae9_rule_dsd_access,
					  NULL, SNDRV_PCM_HW_PARAM_FORMAT, -1);
	if (!err)
		err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
					  ae9_rule_dsd_period, NULL, SNDRV_PCM_HW_PARAM_FORMAT,
					  SNDRV_PCM_HW_PARAM_PERIOD_SIZE, -1);
	if (!err)
		err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
					  ae9_rule_dsd_buffer, NULL, SNDRV_PCM_HW_PARAM_FORMAT,
					  SNDRV_PCM_HW_PARAM_BUFFER_SIZE, -1);
	/* the whole-periods rule again when the format narrows to DSD (its
	 * 32-frame step; the open's copy has no room for a fifth dependency) */
	if (!err)
		err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
					  ae9_rule_whole_periods, (void *)(128UL | AE9_WP_PLAYBACK),
					  SNDRV_PCM_HW_PARAM_FORMAT, -1);
	return err;
}

/*
 * One BAR2 read-modify-write of the I2S wrapper, under chip->lock: the
 * trigger (atomic) turns the I2S output (0x100 bit 8) on and off, process
 * context the rest
 */
static u32 ae9_i2s_rmw(struct creative_chip *chip, unsigned int reg, u32 clear, u32 set)
{
	unsigned long flags;
	u32 v;

	spin_lock_irqsave(&chip->lock, flags);
	v = (creative_ext_readl(chip, reg) & ~clear) | set;
	creative_ext_writel(chip, reg, v);
	spin_unlock_irqrestore(&chip->lock, flags);
	return v;
}

/*
 * CtxHdb FUN_00014fa8 (ch, 1): I2S channel ch's transmit FIFO flushed with
 * its stream tag in 0x104 cleared around it (0x104 saved, the tag nibble
 * cleared, TFFn = 1, 0x104 back)
 */
static void ae9_i2s_flush(struct creative_chip *chip, unsigned int ch)
{
	unsigned long flags;
	u32 map;

	spin_lock_irqsave(&chip->lock, flags);
	map = creative_ext_readl(chip, AE9_I2S_MAP);
	creative_ext_writel(chip, AE9_I2S_MAP, map & ~(0xf0U << (8 * ch)));
	creative_ext_writel(chip, AE9_I2S_TFF(ch),
			    creative_ext_readl(chip, AE9_I2S_TFF(ch)) | 1);
	creative_ext_writel(chip, AE9_I2S_MAP, map);
	spin_unlock_irqrestore(&chip->lock, flags);
}

/* CtxHdb FUN_00015140: I2S channel ch takes the stream with tag from its channel slot */
static void ae9_i2s_map(struct creative_chip *chip, unsigned int ch, u8 tag, u8 slot)
{
	ae9_i2s_rmw(chip, AE9_I2S_MAP, 0xffU << (8 * ch), (u32)(tag << 4 | slot) << (8 * ch));
}

/*
 * one front-DAC command (dsp.c's list: ca0113_mutex, settle, a NACKed
 * command sent once more; clears dac_start_key): 0 = acknowledged
 */
static int ae9_dsd_dac(struct creative_chip *chip, u8 reg, u8 val, const char *what)
{
	const u8 cmd[1][3] = { { AE9_DSD_DAC, reg, val } };

	return creative_dsp_ca0113_list(chip, cmd, 1, what);
}

/*
 * DAC 48/07 bit 0 (the filter bits stay: dsp.c puts the user's in); sent
 * also when dac48_07 already says so but the last 48/07 write wasn't
 * acknowledged (dac_i2c.d07_bad: the DAC may not have it)
 */
static int ae9_dsd_dac_mute(struct creative_chip *chip, bool mute)
{
	if (!!(chip->dac48_07 & 1) == mute && !chip->dac_i2c.d07_bad)
		return 0;
	return ae9_dsd_dac(chip, ES9038_REG_MUTE, (chip->dac48_07 & ~1) | mute,
			   mute ? "DSD64: DAC muted" : "DSD64: DAC unmuted");
}

/*
 * The DAC's mute for the stream's state (the work below, under dsd_mutex):
 * unmuted while the stream runs, unless it was muted before DSD or a DAC
 * command of the switch to DSD wasn't acknowledged (dsd_dac_bad, 0.12.16:
 * a refused 48/01 = 0xc3 leaves the DAC on its PCM input, which would play
 * the DSD bit stream as noise). Then it stays muted, warned once per start.
 */
static void ae9_dsd_mute_sync(struct creative_chip *chip)
{
	bool mute = !READ_ONCE(chip->dsd_running) || chip->dsd_dac_muted;

	if (!mute && chip->dsd_dac_bad) {
		mute = true;
		if (!chip->dsd_dac_warned) {
			chip->dsd_dac_warned = true;
			dev_warn(chip->dev, "DSD64: the DAC did not acknowledge its switch to DSD (I2C NACK): kept muted; stop and start playback to try again\n");
		}
	}
	ae9_dsd_dac_mute(chip, mute);
}

/*
 * The I2S output (BAR2 0x100 bit 8): on right after the stream's RUN, off
 * before its DMA stops (Windows' DSD SetState FUN_0012ff98: RUN ... then
 * FUN_0002d878 -> FUN_00035680(1); STOP: FUN_00035680(0) first). From the
 * trigger (atomic); the DAC's unmute (after the lock-in wait) or mute
 * follows from the work below.
 */
static void ae9_dsd_run(struct creative_chip *chip, bool on)
{
	WRITE_ONCE(chip->dsd_running, on);
	ae9_i2s_rmw(chip, AE9_I2S_CTL, on ? 0 : AE9_I2S_CTL_OUT, on ? AE9_I2S_CTL_OUT : 0);
	mod_delayed_work(chip->wq, &chip->dsd_mute_work,
			 on ? msecs_to_jiffies(AE9_DSD_UNMUTE_MS) : 0);
}

void creative_ae9_dsd_mute_work(struct work_struct *w)
{
	struct creative_chip *chip = container_of(to_delayed_work(w), struct creative_chip,
						  dsd_mute_work);

	mutex_lock(&chip->dsd_mutex);
	if (chip->dsd_active && !creative_ae9_dead(chip) && !chip->suspended)
		ae9_dsd_mute_sync(chip);
	mutex_unlock(&chip->dsd_mutex);
}

/*
 * DSD on (prepare, under dsd_mutex), in Windows' order: its DSD SetState RUN
 * (CtxHda FUN_0012ff98) -> run prep FUN_0002d694 with isDSD = 1: board
 * SetFormat FUN_00032f3c, stage 2 FUN_0003354c; the converter's format and
 * bind (FUN_000206ec, FUN_000203a4), which runs stage 3 FUN_000336d8. RUN
 * itself and the I2S output come at the trigger (ae9_dsd_run()).
 */
static void ae9_dsd_enter(struct creative_chip *chip)
{
	bool hp = chip->hu_output;
	u8 tag = chip->playback_stream_tag;
	unsigned long flags;
	int bad;

	/* a PCM start on this open (hw_params again without hw_free): its
	 * stop block first; on Windows a DSD stream is a stream of its own */
	if (chip->pcm_start_sent && !chip->pcm_start_dsd)
		creative_ae9_playback_seq(chip, false, 0, 0);
	chip->pcm_start_sent = 0;
	/* the first Direct Mode start since the load: no DSP route may feed a
	 * DAC port (as ae9_pb_start() does it) */
	if (!chip->rt_direct_swept) {
		creative_ae9_rt_dsp_dac_off_all(chip, "first Direct Mode start");
		chip->rt_direct_swept = true;
	}

	spin_lock_irqsave(&chip->lock, flags);
	chip->dsd_bar2_100 = creative_ext_readl(chip, AE9_I2S_CTL);
	chip->dsd_bar2_104 = creative_ext_readl(chip, AE9_I2S_MAP);
	spin_unlock_irqrestore(&chip->lock, flags);
	/*
	 * Ours: the DAC muted across the switch (Windows only unmutes, at its
	 * stage 4, a DAC it finds muted), and the I2S output off until the
	 * stream runs. Windows' DSD stop leaves bit 8 off, so its second and
	 * later DSD starts all run this way; only its first after boot has it
	 * on from CtxHdb's init.
	 */
	chip->dsd_dac_muted = chip->dac48_07 & 1;
	bad = ae9_dsd_dac_mute(chip, true);
	ae9_i2s_rmw(chip, AE9_I2S_CTL, AE9_I2S_CTL_OUT, 0);

	/* SetFormat, isDSD = 1: the ES9038 to DSD input (FUN_00032460 ->
	 * FUN_000325c8(1, 0xc3)), GPIO pin 1 high (CtxHdb FUN_00013d10) */
	bad += ae9_dsd_dac(chip, ES9038_REG_INPUT, 0xc3, "DSD64: DAC input DSD");
	creative_ae9_gpio_set(chip, 1, true);

	/* stage 2, the format and bind, stage 3's clocks */
	creative_ae9_dsd_start_block(chip, AE9_DSD_FMT, hp);

	/* stage 3, FUN_000339ec at 88.2 kHz in DSD: no 48/01 = 0xc1 and no
	 * 48/0b; 48/0a = 0x02 (FUN_00032260); the I2S block at 16-bit words
	 * (FUN_0001506c) and rate code 88.2 kHz (FUN_000150b8). (Windows skips
	 * these two when its last stream was 88.2 kHz PCM; here always.) */
	bad += ae9_dsd_dac(chip, ES9038_REG_SYNC, 0x02, "DSD64: DAC sync");
	ae9_i2s_rmw(chip, AE9_I2S_CTL, AE9_I2S_CTL_24BIT, 0);
	ae9_i2s_rmw(chip, AE9_I2S_CTL, AE9_I2S_CTL_RATE,
		    FIELD_PREP(AE9_I2S_CTL_RATE, AE9_I2S_RATE_88K2));
	/* stage 3: the converter re-enabled (0x18a020 read, written back) */
	creative_ae9_dsd_rearm_block(chip);

	/* stage 3, "enter DSD routing" (FUN_00033848): pins 6/7 to their I2S
	 * function (CtxHdb FUN_000138c0; Windows writes the byte, here the
	 * word), both channels' FIFOs flushed, 0x104: channel 2 <- stream
	 * channels 0-1, channel 3 <- 2-3, the channels on (0x100 bits 6/7,
	 * then 14/15), the DAC's DSD input again */
	ae9_i2s_rmw(chip, AE9_I2S_CTL, 0, AE9_I2S_CTL_PINFN(AE9_DSD_PIN(AE9_DSD_CH_L)));
	ae9_i2s_rmw(chip, AE9_I2S_CTL, 0, AE9_I2S_CTL_PINFN(AE9_DSD_PIN(AE9_DSD_CH_R)));
	ae9_i2s_flush(chip, AE9_DSD_CH_L);
	ae9_i2s_flush(chip, AE9_DSD_CH_R);
	ae9_i2s_map(chip, AE9_DSD_CH_L, tag, 0);
	ae9_i2s_map(chip, AE9_DSD_CH_R, tag, 2);
	ae9_i2s_rmw(chip, AE9_I2S_CTL, 0, AE9_I2S_CTL_CH_A(AE9_DSD_CH_L));
	ae9_i2s_rmw(chip, AE9_I2S_CTL, 0, AE9_I2S_CTL_CH_A(AE9_DSD_CH_R));
	ae9_i2s_rmw(chip, AE9_I2S_CTL, 0, AE9_I2S_CTL_CH_B(AE9_DSD_CH_L));
	ae9_i2s_rmw(chip, AE9_I2S_CTL, 0, AE9_I2S_CTL_CH_B(AE9_DSD_CH_R));
	bad += ae9_dsd_dac(chip, ES9038_REG_INPUT, 0xc3, "DSD64: DAC input DSD");

	/* stage 3's end: no stream 0x14 enable in DSD, ASI 0x0f */
	creative_ae9_dsd_asi_block(chip);

	/* any of the DAC's commands above not acknowledged: no unmute
	 * (ae9_dsd_mute_sync()) */
	chip->dsd_dac_bad = bad > 0;
	chip->dsd_dac_warned = false;
	WRITE_ONCE(chip->dsd_active, true);	/* the trigger reads it */
	chip->dsd_running = false;
	chip->pcm_start_sent = 1;
	chip->pcm_start_dsd = true;
	chip->pcm_start_rate = AE9_DSD_RATE;
	chip->pcm_start_fmt = AE9_DSD_FMT;
	dev_info(chip->dev, "DSD64: native DSD on (DSD_U32_BE 2 ch at %u Hz = 2.8224 MHz, link format 0x%04x, %s); BAR2 0x100=0x%08x 0x104=0x%08x\n",
		 AE9_DSD_RATE, AE9_DSD_FMT, hp ? "headphones" : "speakers",
		 creative_ext_readl(chip, AE9_I2S_CTL), creative_ext_readl(chip, AE9_I2S_MAP));
}

/*
 * DSD off (under dsd_mutex; the stream stopped): Windows' DSD STOP (I2S
 * output off, converter unbound -> stage 0: "leave DSD routing",
 * FUN_0003c62c), then what its next PCM stream's SetFormat (isDSD = 0) does
 * (the DAC's input, GPIO pin 1 low), and ours around it: the DAC muted
 * across, BAR2's I2S output bit, word size and rate and 0x104's channel 2/3
 * map back to what they were before DSD (Windows leaves them; PCM has only
 * ever run with them so), the DAC's mute back.
 */
static void ae9_dsd_exit(struct creative_chip *chip, const char *why)
{
	const u32 keep = AE9_I2S_CTL_OUT | AE9_I2S_CTL_24BIT | AE9_I2S_CTL_RATE;
	/* the input after DSD: the clocks and pins DSD leaves are 88.2 kHz
	 * PCM's (CA0132 pin 0 = the converter's S/PDIF output), so S/PDIF on
	 * DATA2 as after an 88.2 kHz stream; 0xc0 there would read that bit
	 * stream as I2S data. The next PCM start sends its own (these clear
	 * dac_start_key). */
	static const u8 dac_pcm[][3] = {
		{ AE9_DSD_DAC, ES9038_REG_INPUT, 0xc1 },
		{ AE9_DSD_DAC, ES9038_REG_SPDIF, 0x20 },
	};

	WRITE_ONCE(chip->dsd_active, false);
	WRITE_ONCE(chip->dsd_running, false);
	chip->pcm_start_sent = 0;
	chip->pcm_start_dsd = false;
	if (creative_ae9_dead(chip) || chip->suspended || !chip->corb_buf) {
		dev_info(chip->dev, "DSD64: native DSD off (%s; card not reachable, nothing sent)\n", why);
		return;
	}
	ae9_dsd_dac_mute(chip, true);
	ae9_i2s_rmw(chip, AE9_I2S_CTL, AE9_I2S_CTL_OUT, 0);	/* FUN_00035680(0) */
	creative_ae9_dsd_stop_block(chip);

	/* FUN_0003c62c: both FIFOs flushed, the channels off (bits 6/7, 14/15),
	 * pins 6/7 back to GPIO (CtxHdb FUN_000138c0) and low (FUN_00013d10) */
	ae9_i2s_flush(chip, AE9_DSD_CH_L);
	ae9_i2s_flush(chip, AE9_DSD_CH_R);
	ae9_i2s_rmw(chip, AE9_I2S_CTL, AE9_I2S_CTL_CH_A(AE9_DSD_CH_L), 0);
	ae9_i2s_rmw(chip, AE9_I2S_CTL, AE9_I2S_CTL_CH_A(AE9_DSD_CH_R), 0);
	ae9_i2s_rmw(chip, AE9_I2S_CTL, AE9_I2S_CTL_CH_B(AE9_DSD_CH_L), 0);
	ae9_i2s_rmw(chip, AE9_I2S_CTL, AE9_I2S_CTL_CH_B(AE9_DSD_CH_R), 0);
	ae9_i2s_rmw(chip, AE9_I2S_CTL, AE9_I2S_CTL_PINFN(AE9_DSD_PIN(AE9_DSD_CH_L)), 0);
	ae9_i2s_rmw(chip, AE9_I2S_CTL, AE9_I2S_CTL_PINFN(AE9_DSD_PIN(AE9_DSD_CH_R)), 0);
	creative_ae9_gpio_set(chip, AE9_DSD_PIN(AE9_DSD_CH_L), false);
	creative_ae9_gpio_set(chip, AE9_DSD_PIN(AE9_DSD_CH_R), false);

	/* ours: BAR2 as before DSD */
	ae9_i2s_rmw(chip, AE9_I2S_CTL, keep, chip->dsd_bar2_100 & keep);
	ae9_i2s_rmw(chip, AE9_I2S_MAP, 0xffff0000, chip->dsd_bar2_104 & 0xffff0000);

	/* the next PCM SetFormat's (FUN_00032f3c, isDSD = 0): the DAC's
	 * input (FUN_00032460 -> FUN_000321dc), then GPIO pin 1 low */
	creative_dsp_ca0113_list(chip, dac_pcm, ARRAY_SIZE(dac_pcm), "DSD64: DAC input back to S/PDIF");
	creative_ae9_gpio_set(chip, 1, false);
	ae9_dsd_dac_mute(chip, chip->dsd_dac_muted);
	dev_info(chip->dev, "DSD64: native DSD off (%s); BAR2 0x100=0x%08x 0x104=0x%08x\n",
		 why, creative_ext_readl(chip, AE9_I2S_CTL), creative_ext_readl(chip, AE9_I2S_MAP));
}

static void ae9_dsd_leave(struct creative_chip *chip, const char *why)
{
	mutex_lock(&chip->dsd_mutex);
	if (chip->dsd_active)
		ae9_dsd_exit(chip, why);
	mutex_unlock(&chip->dsd_mutex);
}

/*
 * Something DSD can't stay on through (Direct Mode off, the output switch,
 * suspend, unload, reboot): a DSD stream stopped as an xrun (the app
 * prepares it again: DSD again, or refused with Direct Mode off), then DSD
 * taken down. Process context; sleeps.
 *
 * Under dsd_mutex only, not the PCM's open_mutex: Direct Mode off calls this
 * from a control put, under the card's controls_rwsem, and ALSA's PCM
 * disconnect takes open_mutex, then controls_rwsem (to remove the chmap
 * control). dsd_mutex is enough to keep the stream's runtime: DSD on means
 * a prepare of chip->playback_substream turned it on, and that runtime is
 * freed only after its hw_free, which takes dsd_mutex (ae9_dsd_leave())
 * before DSD can go off. Lock order: open_mutex (release) -> dsd_mutex ->
 * the stream lock; nothing that holds dsd_mutex takes open_mutex or
 * controls_rwsem.
 */
void creative_ae9_dsd_abort(struct creative_chip *chip, const char *why)
{
	struct snd_pcm_substream *ss;

	if (!chip->pcm)
		return;
	/* under the mutex: a prepare entering DSD now holds it; Direct Mode
	 * off, the output switch and the shutdown set their state before this,
	 * and the prepare checks it under the mutex */
	mutex_lock(&chip->dsd_mutex);
	if (!chip->dsd_active) {
		mutex_unlock(&chip->dsd_mutex);
		return;
	}
	ss = chip->playback_substream;
	if (ss && ss->runtime && ae9_pcm_is_dsd(ss->runtime)) {
		/*
		 * Running, draining (an app's end of playback), paused or only
		 * prepared: all to XRUN (snd_pcm_stop_xrun() covers only the
		 * first two; a prepared stream then started with DSD off:
		 * silence and no error). DSD reads off for the trigger in the
		 * same locked section: a prepare that entered DSD before this
		 * and whose stream wasn't PREPARED yet, started now, gets
		 * -EPIPE instead of running while DSD goes down below.
		 */
		snd_pcm_stream_lock_irq(ss);
		if (snd_pcm_running(ss) ||
		    ss->runtime->state == SNDRV_PCM_STATE_PREPARED ||
		    ss->runtime->state == SNDRV_PCM_STATE_PAUSED)
			snd_pcm_stop(ss, SNDRV_PCM_STATE_XRUN);
		WRITE_ONCE(chip->dsd_active, false);
		snd_pcm_stream_unlock_irq(ss);
	}
	ae9_dsd_exit(chip, why);
	mutex_unlock(&chip->dsd_mutex);
}

/* DSD prepare, after the stream descriptor: the buffer DSD silence, DSD on
 * once per open (as the PCM start block), no optical mirror */
static int ae9_dsd_prepare(struct creative_chip *chip, struct snd_pcm_runtime *runtime)
{
	unsigned long until = jiffies + msecs_to_jiffies(3000);

	ae9_dsd_silence(runtime->dma_area, runtime->buffer_size);
	wmb();
	/*
	 * An output switch stopped this stream: start again once it's done (it
	 * takes ~1 s), on the new output. Checked again under dsd_mutex: Direct
	 * Mode off, an output switch and the shutdown set their state before
	 * their abort takes this mutex, so either they see DSD on and take it
	 * down, or this sees them. At most 3 s in all, so a stuck switch can't
	 * hold the app (-EBUSY then, as with Direct Mode off; the app's next
	 * prepare tries again), and a signal ends the wait (the prepare is
	 * restarted, or the app gets -EINTR): this holds ALSA's link rwsem.
	 */
	for (;;) {
		if (READ_ONCE(chip->dsd_switching)) {
			long left = (long)(until - jiffies);

			if (left > 0 &&
			    wait_event_interruptible_timeout(chip->dsd_wq,
							     !READ_ONCE(chip->dsd_switching),
							     left) < 0)
				return -ERESTARTSYS;
		}
		mutex_lock(&chip->dsd_mutex);
		if (!READ_ONCE(chip->fx.direct) || READ_ONCE(chip->unloading)) {
			mutex_unlock(&chip->dsd_mutex);
			return -EBUSY;
		}
		if (!READ_ONCE(chip->dsd_switching))
			break;
		mutex_unlock(&chip->dsd_mutex);
		if (time_after_eq(jiffies, until))
			return -EBUSY;
	}
	/* on, but the start marked unsent since (a bring-up, a mode switch):
	 * off first, so the enter saves the state from before DSD again */
	if (chip->dsd_active && !(chip->pcm_start_sent && chip->pcm_start_dsd))
		ae9_dsd_exit(chip, "start again");
	if (!chip->dsd_active)
		ae9_dsd_enter(chip);
	mutex_unlock(&chip->dsd_mutex);
	chip->pb_format = 0;
	ae9_spdif_out_apply(chip);	/* a mirror of the previous PCM stream off */
	return 0;
}

static const struct snd_pcm_ops creative_ae9_pcm_ops, creative_ae9_pcm_dsd_ops;

static int creative_ae9_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	/* DSD64: pcm_dsd, Direct Mode, and 88.2 kHz offered */
	bool dsd = pcm_dsd && chip->fx.direct && pcm_rates == 2;
	int err;

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
	if (pcm_rates == 2) {
		runtime->hw.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
				    SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 |
				    SNDRV_PCM_RATE_176400 |
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
	 * Direct Mode off = through the DSP: 48 and 96 kHz.
	 * Windows offers up to 96 kHz there. Our first 96 kHz try played the
	 * stream twice, direct and through the DSP a little later (an echo;
	 * 2026-11-04): most likely stale direct bypass entries (HDA 02/03 ->
	 * DAC 0x42/0x43) from an earlier 96 kHz+ Direct Mode stream, which
	 * DSP mode now switches off (creative_ae9_router_front_dsp). Windows
	 * sends nothing rate-specific for 96 kHz in DSP mode (capture
	 * 2026-11-05, 01:55:36). PipeWire resamples other rates.
	 */
	if (!chip->fx.direct) {
		runtime->hw.rates &= SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_96000;
		runtime->hw.rate_min = max(runtime->hw.rate_min, 48000U);
		runtime->hw.rate_max = min(runtime->hw.rate_max, 96000U);
	}
	/*
	 * 5.1 (surround=1), both modes: Windows plays 6 channels in DSP mode
	 * (captures 5, 6: format 0x35) and in Direct Mode with 5.1 speakers
	 * (capture 6, 23:39:16). 6 channels up to 192 kHz (see
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
	/* and the buffer range's ends only on sizes such a split makes */
	err = ae9_whole_periods(runtime, 128, true);
	if (err < 0)
		goto fail;
	chip->pcm_bits = pcm_bits == 32 ? 32 : 24;
	snd_pcm_hw_constraint_msbits(runtime, 0, 32, chip->pcm_bits);
	chip->pcm_start_sent = 0;

	/*
	 * DSD64 (pcm_dsd): the format and its rules, and the ops with the copy
	 * and silence ops; mmap stays offered for PCM (the access rule keeps
	 * DSD read/write). Without pcm_dsd the stream keeps the plain ops.
	 */
	if (dsd) {
		substream->ops = &creative_ae9_pcm_dsd_ops;
		runtime->hw.formats |= SNDRV_PCM_FMTBIT_DSD_U32_BE;
		err = ae9_dsd_rules(runtime);
		if (err < 0)
			goto fail;
		err = snd_pcm_hw_rule_add(runtime, 0, -1, ae9_rule_dsd_msbits, NULL,
					  SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, -1);
		if (err < 0)
			goto fail;
		ae9_dbg(chip, "DSD64: playback open offers DSD_U32_BE (pcm_dsd on)\n");
	} else {
		substream->ops = &creative_ae9_pcm_ops;
	}

	return 0;

fail:
	chip->substream = NULL;
	chip->playback_substream = NULL;
	return err;
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
		if (ae9_bdl_may_free(chip, chip->playback_stream_base, chip->playback_bdl_dma,
				     "playback hw_params"))
			dma_free_coherent(chip->dev, chip->playback_bdl_size,
					  chip->playback_bdl_raw,
					  chip->playback_bdl_dma);
		chip->playback_bdl_raw = NULL;
		chip->playback_bdl = NULL;
	}

	/* DSD: the buffer holds 12-byte link frames, not ALSA's 8-byte ones */
	if (params_format(hw_params) == SNDRV_PCM_FORMAT_DSD_U32_BE)
		ret = snd_pcm_lib_malloc_pages(substream,
					       params_buffer_size(hw_params) * AE9_DSD_HW_FRAME);
	else
		ret = snd_pcm_lib_malloc_pages(substream,
					       params_buffer_bytes(hw_params));
	if (ret < 0)
		return ret;
	/* with a copy op ALSA doesn't clear the buffer after hw_params: here
	 * (DSD silence for DSD; prepare fills it again) */
	if (substream->ops->copy) {
		if (params_format(hw_params) == SNDRV_PCM_FORMAT_DSD_U32_BE)
			ae9_dsd_silence(substream->runtime->dma_area, params_buffer_size(hw_params));
		else {
			/* as snd_pcm_hw_params() does without a copy op: whole
			 * pages for mmap, so a client sees no earlier stream's
			 * audio past dma_bytes. The refined params' info, not
			 * runtime->info: the core sets that only after this op
			 * returns (0 at an open's first hw_params) */
			struct snd_pcm_runtime *rt = substream->runtime;
			size_t n = rt->dma_bytes;

			if (hw_params->info & SNDRV_PCM_INFO_MMAP)
				n = PAGE_ALIGN(n);
			if (rt->dma_buffer_p)
				n = min_t(size_t, n, rt->dma_buffer_p->bytes);
			memset(rt->dma_area, 0, n);
		}
	}

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
 * Release: Windows' playback stop block. Restarts ~110-130 s after a stop
 * came up silent (shorter and much longer gaps were fine), as if the stop
 * block starts a timer inside the card (2026-11-02); leaving the DSP
 * stream set up at release instead, to test that, was the pcm_stop_block
 * option until 0.12.12.
 */
static int creative_ae9_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);

	/* pcm_windows: Windows' stop block (unbind node 0x02 etc.) when the
	 * stream is released, not at every trigger stop (an xrun stop +
	 * re-prepare would otherwise leave node 0x02 unbound) */
	chip->pb_format = 0;
	ae9_spdif_out_apply(chip);	/* mirror off (unless optical has its own) */
	/* DSD64: its own stop (clears pcm_start_sent: no PCM stop block).
	 * Always through dsd_mutex, also with DSD off: an abort in progress
	 * uses this stream's runtime under it (creative_ae9_dsd_abort()) */
	ae9_dsd_leave(chip, "stream released");
	if (chip->pcm_start_sent) {
		creative_ae9_playback_seq(chip, false, 0, 0);
		chip->pcm_start_sent = 0;
	}

	if (chip->playback_bdl_raw) {
		if (ae9_bdl_may_free(chip, chip->playback_stream_base, chip->playback_bdl_dma,
				     "playback hw_free"))
			dma_free_coherent(chip->dev, chip->playback_bdl_size,
					  chip->playback_bdl_raw,
					  chip->playback_bdl_dma);
		chip->playback_bdl_raw = NULL;
		chip->playback_bdl = NULL;
	}

	return snd_pcm_lib_free_pages(substream);
}

/* ------------------------------------------------------------------ */
/* DMA FIFO: SDnFIFOS per playback stream (debugfs only)		  */
/* ------------------------------------------------------------------ */
/*
 * The card's DMA reads a playback stream into the stream's FIFO before the
 * link sends it out; LPIB, which the pointer reports, is the link's
 * position. SDnFIFOS (HD-Audio spec 3.3.40) is the most bytes fetched but
 * not yet sent, less one. It depends on the format, and it holds the
 * format's value only while RUN is set: read after the format and before
 * RUN it was 0x107, the register's reset value, at all 31 formats measured
 * on the card (2026-11-21). So it's read right after RUN, for debugfs
 * state's "DMA FIFO" line and "readahead" (below): 264 bytes at 44.1/48 kHz
 * 24/32-bit stereo, 392 at 88.2/96 kHz 24-bit, 520 at 88.2/96 kHz 32-bit,
 * 776 at 176.4/192 kHz 24-bit and for DSD's 6 x 16 bit, 1544 at 384 kHz
 * (192 kHz x 4 ch), 3080 at 5.1 192 kHz 32-bit, 264 for 16-bit optical up
 * to 96 kHz (176.4 kHz reads as 192 kHz).
 *
 * Counting part of that FIFO into the pointer (0.12.17's pcm_fifo option,
 * card-tested and dropped: docs/dev/DMA-READAHEAD-0.12.17.md) isn't safe on
 * this card: behind its PCI bridge the DMA position moves when a read is
 * requested, before the data arrives, so nothing says how much is really
 * fetched. Even a quarter of the FIFO spliced stale audio. The pointer stays
 * LPIB; the 4-period buffer rule (above) and the WirePlumber headroom rule
 * (config/) cover the read-ahead.
 */

/* SDnFIFOS (16 bits at descriptor + 0x10); 0 = unreadable */
static u16 ae9_fifo_read(struct creative_chip *chip, unsigned int base)
{
	u16 v = creative_hda_readw(chip, base + AZX_REG_SD_FIFOSIZE);

	return v == 0xffff ? 0 : v;
}

/* prepare, after the format is set: what debugfs shows the reading for
 * (no card access: SDnFIFOS before RUN is the register's reset value) */
static void ae9_fifo_prepare(struct ae9_fifo *f, u16 fmt, unsigned int frame_bytes,
			     unsigned int rate)
{
	f->fifos_run = 0;
	f->fmt = fmt;
	f->frame_bytes = frame_bytes;
	f->rate = rate;
}

/* trigger START, RUN just set: SDnFIFOS for the format (valid while RUN) */
static void ae9_fifo_start(struct creative_chip *chip, struct ae9_fifo *f, unsigned int base)
{
	f->fifos_run = ae9_fifo_read(chip, base);
}

/* ------------------------------------------------------------------ */
/* prepare — program BDL and stream registers			  */
/* ------------------------------------------------------------------ */

/*
 * A headphone profile upload (fx.c) checks that no stream runs, then
 * holds replay_mutex for ~100 ms. A stream prepared in between waits for
 * it here instead of starting under the upload (review B11, note 56a).
 */
static void ae9_wait_hp_upload(struct creative_chip *chip)
{
	mutex_lock(&chip->replay_mutex);
	mutex_unlock(&chip->replay_mutex);
}

static int creative_ae9_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int base = chip->playback_stream_base;
	unsigned int buf_size = snd_pcm_lib_buffer_bytes(substream);
	unsigned int period_size = snd_pcm_lib_period_bytes(substream);
	bool dsd = ae9_pcm_is_dsd(runtime);
	u16 format;
	dma_addr_t addr;
	int i;

	/* the card stopped answering, or the driver is going away (reboot,
	 * unload: the player's restart after the DSD abort mustn't start
	 * DSD again beside the shutdown) */
	if (creative_ae9_dead(chip) || READ_ONCE(chip->unloading))
		return -ENODEV;
	/* DSD64: Direct Mode only (it was on at open; it went off since) */
	if (dsd && !READ_ONCE(chip->fx.direct)) {
		dev_info_ratelimited(chip->dev, "DSD64: refused, Direct Mode is off (native DSD plays in Direct Mode only)\n");
		return -EBUSY;
	}

	ae9_wait_hp_upload(chip);
	creative_ae9_stream_reset(chip, base);

	/* DSD64: the link carries 12-byte frames (6 x 16 bit) */
	if (dsd) {
		buf_size = runtime->buffer_size * AE9_DSD_HW_FRAME;
		period_size = runtime->period_size * AE9_DSD_HW_FRAME;
	}

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

	/* S32_LE container; 24-bit (0x31 at 48 kHz) or 32-bit (0x41); DSD64
	 * as Windows sends it: 6 x 16 bit at 88.2 kHz (0x4815) */
	if (dsd)
		format = AE9_DSD_FMT;
	else
		format = creative_ae9_stream_format(runtime->channels, chip->pcm_bits,
						    runtime->rate);
	creative_hda_writew(chip, base + AZX_REG_SD_FORMAT, format);
	ae9_fifo_prepare(&chip->pb_fifo, format,
			 dsd ? AE9_DSD_HW_FRAME : frames_to_bytes(runtime, 1), runtime->rate);

	/* Write BDL pointer as fallback */
	creative_hda_writel(chip, base + AZX_REG_SD_BDLPL,
			    lower_32_bits(chip->playback_bdl_dma));
	creative_hda_writel(chip, base + AZX_REG_SD_BDLPU,
			    upper_32_bits(chip->playback_bdl_dma));
	ae9_dma_trace_prep(chip, "playback", base, substream, chip->playback_bdl_dma);

	if (dsd)
		return ae9_dsd_prepare(chip, runtime);
	/* DSD64 on from this open's last stream: off before any PCM start */
	if (chip->dsd_active)
		ae9_dsd_leave(chip, "PCM stream");

	/* Windows' playback-start block sets format 0x31 and binds node
	 * 0x02 to tag 1 itself. Once per open: xrun recovery re-prepares,
	 * and replaying the block (12 SCP messages) each time flooded the
	 * DSP. A DSD start (88.2 kHz too) is never this block. */
	if (!chip->pcm_start_sent || chip->pcm_start_rate != runtime->rate ||
	    chip->pcm_start_fmt != format || chip->pcm_start_dsd) {
		creative_ae9_playback_seq(chip, true, runtime->rate, format);
		if (chip->mic_level != 99)
			creative_ae9_mic_level_apply(chip);
		chip->pcm_start_sent = 1;
		chip->pcm_start_rate = runtime->rate;
		chip->pcm_start_fmt = format;
		chip->pcm_start_dsd = false;
		ae9_dbg(chip, "PCM prepare: SD_FMT=0x%04x, Windows start block sent\n",
			 format);
		/* Speaker Config follows the stream, but only one that's still
		 * playing 1.5 s later: PipeWire's probe of the card profile
		 * opened 2 and 6 channels back and forth at login and every
		 * open re-sent the whole effects block (Kaiba: 12 flips in
		 * 19 s, note 60) */
		chip->spk_follow_ch = runtime->channels;
		mod_delayed_work(chip->wq, &chip->spk_follow_work, msecs_to_jiffies(1500));
	}
	chip->pb_format = format;
	ae9_spdif_out_apply(chip);	/* after the start block: mirror */
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
	/* DSD64: the I2S output follows the stream (ae9_dsd_run()) */
	bool dsd = ae9_pcm_is_dsd(substream->runtime) && READ_ONCE(chip->dsd_active);

	if (cmd == SNDRV_PCM_TRIGGER_START && creative_ae9_dead(chip))
		return -ENODEV;
	/* a DSD stream whose DSD went off after its prepare (an abort raced
	 * it): an xrun, so the app prepares again, instead of silence */
	if ((cmd == SNDRV_PCM_TRIGGER_START || cmd == SNDRV_PCM_TRIGGER_PAUSE_RELEASE ||
	     cmd == SNDRV_PCM_TRIGGER_RESUME) &&
	    ae9_pcm_is_dsd(substream->runtime) && !dsd)
		return -EPIPE;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		/* stream only; the start block went out at prepare */
		creative_ae9_stream_start(chip, base);
		if (cmd == SNDRV_PCM_TRIGGER_START)
			ae9_fifo_start(chip, &chip->pb_fifo, base);
		if (dsd)
			ae9_dsd_run(chip, true);
		creative_ae9_dsp_switch_kick(chip);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		if (dsd)
			ae9_dsd_run(chip, false);
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
/*
 * LPIB is the position going out on the link; the DMA has already fetched
 * ahead of it (measured with the DMA position buffer, HD-Audio spec
 * 3.3.32, through the pcm_posbuf option, removed in 0.12.12), and at tiny
 * PipeWire quantums PipeWire wrote into audio the card had already fetched
 * (static, no xrun). The cure is PipeWire headroom (README,
 * scripts/ae9-headroom.sh). Reporting the DMA position as the pointer was
 * tried (2026-11-05) and dropped: it moves in ~100-frame jumps, and with
 * headroom in place it only added underruns at 192/384 kHz.
 */

static snd_pcm_uframes_t
creative_ae9_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	unsigned int base = chip->playback_stream_base;
	unsigned int pos;

	pos = readl(chip->hda_regs + base + AZX_REG_SD_LPIB);
	wmb();
	/* DSD64: LPIB counts 12-byte link frames */
	if (ae9_pcm_is_dsd(substream->runtime)) {
		if (pos >= substream->runtime->buffer_size * AE9_DSD_HW_FRAME)
			pos = 0;
		return pos / AE9_DSD_HW_FRAME;
	}
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

/* the same with the DSD copy and silence ops: streams opened with pcm_dsd on */
static const struct snd_pcm_ops creative_ae9_pcm_dsd_ops = {
	.open		= creative_ae9_pcm_open,
	.close		= creative_ae9_pcm_close,
	.hw_params	= creative_ae9_pcm_hw_params,
	.hw_free	= creative_ae9_pcm_hw_free,
	.prepare	= creative_ae9_pcm_prepare,
	.trigger	= creative_ae9_pcm_trigger,
	.pointer	= creative_ae9_pcm_pointer,
	.copy		= creative_ae9_pcm_copy,
	.fill_silence	= creative_ae9_pcm_fill_silence,
};

/* ------------------------------------------------------------------ */
/* Separate optical playback: PCM device 4, stream base 0x140, tag 3,  */
/* codec 2 converter 0x05; while it isn't prepared, 0x05 mirrors main.	  */
/* ------------------------------------------------------------------ */
static const struct snd_pcm_chmap_elem ae9_opt_chmaps[] = {
	{ .channels = 2, .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR } },
	{ }
};

static int creative_ae9_opt_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	int err;

	chip->opt_substream = substream;
	runtime->hw.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
			   SNDRV_PCM_INFO_INTERLEAVED;
	runtime->hw.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S32_LE;
	runtime->hw.channels_min = 2;
	runtime->hw.channels_max = 2;
	/* no 176.4 kHz: converter 0x05's rate list (param 0x0a = 0x000a05e0,
	 * Windows' boot read) has 44.1/48/88.2/96/192 */
	runtime->hw.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
			    SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 |
			    SNDRV_PCM_RATE_192000;
	runtime->hw.rate_min = 44100;
	runtime->hw.rate_max = 192000;
	runtime->hw.buffer_bytes_max = CREATIVE_PCM_BUF_MAX;
	runtime->hw.period_bytes_min = 4096;
	runtime->hw.period_bytes_max = 128 * 1024;
	runtime->hw.periods_min = 2;
	runtime->hw.periods_max = clamp(pcm_periods, 2U, (unsigned int)CREATIVE_PCM_BDL_MAX);
	snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 128);
	snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 128);
	snd_pcm_hw_constraint_msbits(runtime, 0, 32, 24);
	err = ae9_whole_periods(runtime, 128, true);	/* as playback */
	if (err < 0)
		chip->opt_substream = NULL;
	return err;
}

static int creative_ae9_opt_close(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);

	chip->opt_substream = NULL;
	return 0;
}

static int creative_ae9_opt_hw_params(struct snd_pcm_substream *substream,
				      struct snd_pcm_hw_params *hw_params)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	size_t alloc_size;
	int ret;

	if (chip->opt_bdl_raw) {
		if (ae9_bdl_may_free(chip, CREATIVE_OPT_STREAM_BASE, chip->opt_bdl_dma,
				     "optical out hw_params"))
			dma_free_coherent(chip->dev, chip->opt_bdl_size,
					  chip->opt_bdl_raw, chip->opt_bdl_dma);
		chip->opt_bdl_raw = NULL;
		chip->opt_bdl = NULL;
	}
	ret = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
	if (ret < 0)
		return ret;
	alloc_size = ae9_dma_size(chip, CREATIVE_PCM_BDL_SIZE + 128);
	chip->opt_bdl_raw = dma_alloc_coherent(chip->dev, alloc_size,
					       &chip->opt_bdl_dma, GFP_KERNEL);
	if (!chip->opt_bdl_raw) {
		snd_pcm_lib_free_pages(substream);
		return -ENOMEM;
	}
	chip->opt_bdl_size = alloc_size;
	chip->opt_bdl = PTR_ALIGN(chip->opt_bdl_raw, 128);
	chip->opt_bdl_dma = ALIGN(chip->opt_bdl_dma, 128);
	chip->opt_period_size = params_period_bytes(hw_params);
	return 0;
}

static int creative_ae9_opt_hw_free(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);

	chip->opt_format = 0;
	ae9_spdif_out_apply(chip);
	if (chip->opt_bdl_raw) {
		if (ae9_bdl_may_free(chip, CREATIVE_OPT_STREAM_BASE, chip->opt_bdl_dma,
				     "optical out hw_free"))
			dma_free_coherent(chip->dev, chip->opt_bdl_size,
					  chip->opt_bdl_raw, chip->opt_bdl_dma);
		chip->opt_bdl_raw = NULL;
		chip->opt_bdl = NULL;
	}
	return snd_pcm_lib_free_pages(substream);
}

static int creative_ae9_opt_prepare(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int base = CREATIVE_OPT_STREAM_BASE;
	unsigned int buf_size = snd_pcm_lib_buffer_bytes(substream);
	unsigned int period_size = snd_pcm_lib_period_bytes(substream);
	u16 format;
	dma_addr_t addr;
	int i;

	if (creative_ae9_dead(chip))	/* the card stopped answering */
		return -ENODEV;

	ae9_wait_hp_upload(chip);
	creative_ae9_stream_reset(chip, base);
	memset(chip->opt_bdl, 0, CREATIVE_PCM_BDL_SIZE);
	addr = runtime->dma_addr;
	for (i = 0; i < runtime->periods; i++) {
		__le64 *bdl_addr = chip->opt_bdl + i * 16;
		__le32 *bdl_len = chip->opt_bdl + i * 16 + 8;
		__le32 *bdl_ioc = chip->opt_bdl + i * 16 + 12;

		*bdl_addr = cpu_to_le64(addr);
		*bdl_len = cpu_to_le32(period_size);
		*bdl_ioc = cpu_to_le32(runtime->no_period_wakeup ? 0 : 1);
		addr += period_size;
	}
	wmb();
	{
		u32 ctl = readl(chip->hda_regs + base + AZX_REG_SD_CTL);

		ctl &= ~(0xf << 20);
		ctl |= ((CREATIVE_OPT_STREAM_TAG & 0xf) << 20);
		writel((ctl) & SD_CTL_WMASK, chip->hda_regs + base + AZX_REG_SD_CTL);
		wmb();
	}
	creative_hda_writel(chip, base + AZX_REG_SD_CBL, buf_size);
	creative_hda_writew(chip, base + AZX_REG_SD_LVI, runtime->periods - 1);
	format = creative_ae9_stream_format(2,
			runtime->format == SNDRV_PCM_FORMAT_S16_LE ? 16 : 24,
			runtime->rate);
	creative_hda_writew(chip, base + AZX_REG_SD_FORMAT, format);
	ae9_fifo_prepare(&chip->opt_fifo, format, frames_to_bytes(runtime, 1), runtime->rate);
	creative_hda_writel(chip, base + AZX_REG_SD_BDLPL,
			    lower_32_bits(chip->opt_bdl_dma));
	creative_hda_writel(chip, base + AZX_REG_SD_BDLPU,
			    upper_32_bits(chip->opt_bdl_dma));
	ae9_dma_trace_prep(chip, "optical out", base, substream, chip->opt_bdl_dma);
	chip->opt_format = format;
	ae9_spdif_out_apply(chip);
	return 0;
}

static int creative_ae9_opt_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	unsigned int base = CREATIVE_OPT_STREAM_BASE;

	if (cmd == SNDRV_PCM_TRIGGER_START && creative_ae9_dead(chip))
		return -ENODEV;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		creative_ae9_stream_start(chip, base);
		if (cmd == SNDRV_PCM_TRIGGER_START)
			ae9_fifo_start(chip, &chip->opt_fifo, base);
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

static snd_pcm_uframes_t
creative_ae9_opt_pointer(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	unsigned int pos;

	pos = readl(chip->hda_regs + CREATIVE_OPT_STREAM_BASE + AZX_REG_SD_LPIB);
	wmb();
	if (pos >= snd_pcm_lib_buffer_bytes(substream))
		pos = 0;
	return bytes_to_frames(substream->runtime, pos);
}

void creative_ae9_opt_irq(struct creative_chip *chip)
{
	unsigned int base = CREATIVE_OPT_STREAM_BASE;
	u8 sts = readb(chip->hda_regs + base + AZX_REG_SD_STS);

	if (!sts)
		return;
	writeb(sts & SD_INT_MASK, chip->hda_regs + base + AZX_REG_SD_STS);
	if ((sts & SD_INT_COMPLETE) && chip->opt_substream)
		snd_pcm_period_elapsed(chip->opt_substream);
}

static const struct snd_pcm_ops creative_ae9_opt_pcm_ops = {
	.open		= creative_ae9_opt_open,
	.close		= creative_ae9_opt_close,
	.hw_params	= creative_ae9_opt_hw_params,
	.hw_free	= creative_ae9_opt_hw_free,
	.prepare	= creative_ae9_opt_prepare,
	.trigger	= creative_ae9_opt_trigger,
	.pointer	= creative_ae9_opt_pointer,
};

/*
 * "IEC958 Playback Default" / "Con Mask" on the optical PCM (iface PCM,
 * device 4; not mixer elements, so PipeWire's analog paths leave them
 * alone). Of the channel status only AES0's non-audio bit is acted on: it
 * sets 0x70d's /AUDIO bit for AC-3/DTS passthrough (IEC 61937), applied at
 * once if the optical stream is bound. Off by default: on PCM data it
 * makes receivers mute or play noise.
 */
static int ae9_iec958_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	ui->type = SNDRV_CTL_ELEM_TYPE_IEC958;
	ui->count = 1;
	return 0;
}

static int ae9_iec958_default_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	unsigned int i;

	mutex_lock(&chip->spdif_mutex);
	for (i = 0; i < 4; i++)
		uv->value.iec958.status[i] = chip->spdif_aes[i];
	mutex_unlock(&chip->spdif_mutex);
	return 0;
}

static int ae9_iec958_default_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct creative_chip *chip = snd_kcontrol_chip(kc);
	unsigned int i;
	int changed = 0;

	if (creative_ae9_dead(chip))
		return -ENODEV;

	mutex_lock(&chip->spdif_mutex);
	for (i = 0; i < 4; i++) {
		if (chip->spdif_aes[i] != uv->value.iec958.status[i])
			changed = 1;
		chip->spdif_aes[i] = uv->value.iec958.status[i];
	}
	if (changed)
		ae9_spdif_out_apply_locked(chip);
	mutex_unlock(&chip->spdif_mutex);
	return changed;
}

static int ae9_iec958_mask_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	uv->value.iec958.status[0] = IEC958_AES0_NONAUDIO | IEC958_AES0_CON_NOT_COPYRIGHT;
	return 0;
}

static void creative_ae9_opt_iec958_new(struct creative_chip *chip, int device)
{
	struct snd_kcontrol_new kn[] = {
		{ .iface = SNDRV_CTL_ELEM_IFACE_PCM, .device = device,
		  .name = SNDRV_CTL_NAME_IEC958("", PLAYBACK, DEFAULT),
		  .info = ae9_iec958_info, .get = ae9_iec958_default_get,
		  .put = ae9_iec958_default_put },
		{ .iface = SNDRV_CTL_ELEM_IFACE_PCM, .device = device,
		  .access = SNDRV_CTL_ELEM_ACCESS_READ,
		  .name = SNDRV_CTL_NAME_IEC958("", PLAYBACK, CON_MASK),
		  .info = ae9_iec958_info, .get = ae9_iec958_mask_get },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(kn); i++)
		if (snd_ctl_add(chip->card, snd_ctl_new1(&kn[i], chip)) < 0)
			dev_warn(chip->dev, "optical: %s not added\n", kn[i].name);
}

static void creative_ae9_opt_new(struct creative_chip *chip)
{
	struct snd_pcm *pcm;
	int ret;

	ret = snd_pcm_new(chip->card, "AE-9 Optical Out", 4, 1, 0, &pcm);
	if (ret < 0) {
		dev_warn(chip->dev, "AE-9 Optical Out device not added (%d)\n", ret);
		return;
	}
	pcm->private_data = chip;
	ae9_pcm_track(chip, pcm);
	strscpy(pcm->name, "AE-9 Optical Out", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK,
			&creative_ae9_opt_pcm_ops);
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV,
					      chip->dev,
					      ae9_dma_size(chip, CREATIVE_PCM_BUF_MAX),
					      ae9_dma_size(chip, CREATIVE_PCM_BUF_MAX));
	ret = snd_pcm_add_chmap_ctls(pcm, SNDRV_PCM_STREAM_PLAYBACK,
				     ae9_opt_chmaps, 2, 0, NULL);
	if (ret < 0)
		dev_warn(chip->dev, "optical channel map not added (%d)\n", ret);
	creative_ae9_opt_iec958_new(chip, pcm->device);
}

/* ------------------------------------------------------------------ */
/* Capture: input stream 0 (BAR0+0x80), tag 1, from codec 1 node 0x07	  */
/* (the head unit's input: its XLR/TRS jack and its 3.5 mm mic jack	  */
/* share it; Windows lists them as Microphone and Aux), as in the	  */
/* Windows recording capture. 48 or 96 kHz (Creative: mic up to 96 kHz); */
/* Windows' 96 kHz recording is the stream format 0x831 and the same	  */
/* format on node 0x07, nothing else (VM capture 2026-11-05).		  */
/* ------------------------------------------------------------------ */
#define CREATIVE_CAP_STREAM_BASE	0x80	/* input stream descriptor 0 */
#define CREATIVE_CAP_BDL_MAX		8

/*
 * cap_bits: the recording's sample bits, 24 (default) or 32, as Windows'
 * recording format choice (VM captures 2026-11-03: node 0x07 format 0x41 /
 * 0x841 at 32-bit). Same S32_LE container either way; taken at the next
 * recording start, like "PCM Bit Depth" for playback.
 */
static unsigned int cap_bits = 24;
module_param(cap_bits, uint, 0644);
MODULE_PARM_DESC(cap_bits, "Recording sample bits: 24 (default) or 32; also the 'Capture Bit Depth' control");

static int cap_bits_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	uv->value.enumerated.item[0] = cap_bits == 32;
	return 0;
}

static int cap_bits_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	unsigned int want = uv->value.enumerated.item[0] ? 32 : 24;

	if (uv->value.enumerated.item[0] > 1)
		return -EINVAL;
	if (want == (cap_bits == 32 ? 32 : 24))
		return 0;
	cap_bits = want;	/* the next recording opens with it */
	return 1;
}

static const struct snd_kcontrol_new cap_bits_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Capture Bit Depth",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info = pcm_bits_info,		/* "24-bit", "32-bit" */
	.get = cap_bits_get,
	.put = cap_bits_put,
};

static unsigned int cap_bits_now(void)
{
	return cap_bits == 32 ? 32 : 24;
}

static int creative_ae9_cap_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	int err;

	chip->capture_substream = substream;
	runtime->hw.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
			   SNDRV_PCM_INFO_INTERLEAVED;
	/* Windows: format 0x31 (2 ch, 24-bit in 32-bit containers, 48 kHz),
	 * 0x831 at 96 kHz */
	runtime->hw.formats = SNDRV_PCM_FMTBIT_S32_LE;
	runtime->hw.channels_min = 2;
	runtime->hw.channels_max = 2;
	runtime->hw.rates = SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_96000;
	runtime->hw.rate_min = 48000;
	runtime->hw.rate_max = 96000;
	/* Voice Clarity: Windows records at 48 kHz (format 0x31) with it on */
	if (AE9_CV_WANT(chip)) {
		runtime->hw.rates = SNDRV_PCM_RATE_48000;
		runtime->hw.rate_max = 48000;
	}
	runtime->hw.buffer_bytes_max = 128 * 1024;	/* ~170 ms at 96 kHz */
	runtime->hw.period_bytes_min = 1920;	/* 5 ms */
	runtime->hw.period_bytes_max = 32 * 1024;
	runtime->hw.periods_min = 2;
	runtime->hw.periods_max = CREATIVE_CAP_BDL_MAX;
	snd_pcm_hw_constraint_msbits(runtime, 0, 32, cap_bits_now());
	snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	/* whole periods (any frame count here): buffer ends as playback */
	err = ae9_whole_periods(runtime, 1, false);
	if (err < 0) {
		chip->capture_substream = NULL;
		return err;
	}
	chip->cap_start_sent = 0;
	return 0;
}

static int creative_ae9_cap_close(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);

	chip->capture_substream = NULL;
	if (chip->cv.monitor)	/* the monitor keeps the mic input after it */
		creative_ae9_mic_monitor_apply(chip);
	return 0;
}

static void creative_ae9_cap_bdl_free(struct creative_chip *chip)
{
	if (chip->cap_bdl_raw) {
		if (ae9_bdl_may_free(chip, CREATIVE_CAP_STREAM_BASE, chip->cap_bdl_dma, "mic BDL free"))
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
		creative_ae9_capture_seq(chip, false);
		chip->cap_start_sent = 0;
	}
	chip->cv.mon_bound = false;	/* the stop block unbound it */
	if (chip->cv.route_on) {	/* Windows: ~30 ms after the ADC unbind */
		msleep(30);
		creative_ae9_cv_route(chip, false);
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

	if (creative_ae9_dead(chip))	/* the card stopped answering */
		return -ENODEV;

	if (periods > CREATIVE_CAP_BDL_MAX)
		return -EINVAL;
	ae9_wait_hp_upload(chip);
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
			    creative_ae9_stream_format(runtime->channels, cap_bits_now(),
						    runtime->rate));
	creative_hda_writel(chip, base + AZX_REG_SD_BDLPL, lower_32_bits(bdl_dma));
	creative_hda_writel(chip, base + AZX_REG_SD_BDLPU, upper_32_bits(bdl_dma));
	ae9_dma_trace_prep(chip, "mic", base, substream, bdl_dma);

	/*
	 * Windows' start block once per open (re-prepares after an xrun
	 * must not resend it), with node 0x07 given this recording's format
	 * before its bind (see creative_ae9_capture_seq)
	 */
	chip->cap_format = creative_ae9_stream_format(runtime->channels, cap_bits_now(),
						      runtime->rate);
	/* Voice Clarity: the voice processor in BEFORE Windows' start block
	 * binds the ADC (Windows: format, route, bind; it never moves the mic
	 * route while 0x07 is bound - unicorn router report, 25 captures) */
	/* the mic monitor holds 0x07 bound (no recording until now): the
	 * recording takes it over. Unbind first, as cv_apply does, so the
	 * route and the new format never land on a bound ADC */
	if (!chip->cap_start_sent && chip->cv.mon_bound) {
		u32 resp;

		creative_ae9_corb_send_verb_sync(chip, 0x10770600, &resp);
		msleep(20);	/* Windows: ~20 ms after the unbind */
		chip->cv.mon_bound = false;
	}
	/* Voice Clarity: the route-in goes inside the start block, after the
	 * format and before the bind, as Windows (creative_ae9_cap_start) */
	if (!chip->cap_start_sent) {
		creative_ae9_cap_start(chip, AE9_CV_WANT(chip) && !chip->cv.route_on);
		chip->cap_start_sent = 1;
		ae9_dbg(chip, "capture prepare: SD_FMT=0x%04x\n",
			 creative_hda_readw(chip, base + AZX_REG_SD_FORMAT));
	}
	return 0;
}

static int creative_ae9_cap_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);

	if (cmd == SNDRV_PCM_TRIGGER_START && creative_ae9_dead(chip))
		return -ENODEV;

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
/* Extra capture devices, each on its own input stream so they record	  */
/* alongside the XLR mic:						  */
/*   PCM device 1 "AE-9 Optical": codec 2's S/PDIF input converter	  */
/*     0x09 (pin 0x0e), input stream 1 (BAR0+0xa0), tag 2, 44.1-192 kHz.*/
/*     S/PDIF input doesn't resample: open it at the sender's rate.	  */
/*   PCM device 2 "AE-9 What U Hear": codec 1's loopback ADC 0x0a (pin	  */
/*     0x13, "What U Hear In"), input stream 2 (BAR0+0xc0), tag 3. What */
/*     the card plays, both with Direct Mode on and off (with SBX off it */
/*     is the stream as sent: a 440 Hz test tone came back level-exact  */
/*     on both channels). 48 or 96 kHz.				  */
/*   PCM device 3 "AE-9 Rear RCA In": the head unit's rear RCA L/R	  */
/*     jacks = codec 2 pin 0x11 -> ADC 0x08 (24-bit up to 192 kHz),	  */
/*     input stream 3 (BAR0+0xe0), tag 4. Proven by hand 2026-11-07: a  */
/*     440 Hz tone into the jacks came back only while 0x08 was on the  */
/*     capture stream. Windows calls it "Aux" (codec 2 on MS hdaudio).  */
/*     Pin config and pin control (0x22) come from the cold table.	  */
/* ------------------------------------------------------------------ */
struct ae9_xcap {
	const char	*name;
	unsigned int	base, tag, cad, nid;
	unsigned int	rates, rate_min, rate_max;
	unsigned int	signal_pin;	/* codec pin whose sense says "signal", 0 none */
};

static const struct ae9_xcap ae9_xcaps[3] = {
	/* no 176.4 kHz: converter 0x09's rate list (param 0x0a = 0x000a05f0,
	 * Windows' boot read, capture 2026-11-09) has 32/44.1/48/88.2/96/192 */
	{ "AE-9 Optical", 0xa0, 2, AE9_SPDIF_CAD, AE9_SPDIF_IN_NID,
	  SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 |
	  SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_192000,
	  44100, 192000, AE9_SPDIF_IN_PIN },
	{ "AE-9 What U Hear", 0xc0, 3, 1, 0x0a,
	  SNDRV_PCM_RATE_48000, 48000, 48000, 0 },
	/* no 176.4 kHz: ADC 0x08's rate list (param 0x0a = 0x000a05e4, also in
	 * Windows' boot read) has
	 * 44.1/48/88.2/96/192 but not 176.4 (bit 9) */
	{ "AE-9 Rear RCA In", 0xe0, 4, 2, 0x08,
	  SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 |
	  SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_192000,
	  44100, 192000, 0 },
};
#define AE9_XCAP_WUH	1

/*
 * What U Hear at 96 kHz: only the stream and ADC 0x0a format change, as
 * the mic's 96 kHz recording. Tested 2026-11-06: a 440 Hz tone comes back
 * at 440 Hz and the same level (-23.1 dBFS, L == R) with the card playing
 * at 48 or 96 kHz. Also moving its DSP connection point (ca0132's
 * MEM_CONNID_WUH = 0x0a, 48 kHz from Windows' boot table) to 96 kHz was
 * wrong: ~810 Hz, 3 dB low. Leave it at 48 kHz. (wuh_96k, until 0.12.14,
 * could limit What U Hear to 48 kHz.)
 */



/* PCM device n (1, 2, 3) -> table index */
static unsigned int ae9_xcap_idx(struct snd_pcm_substream *substream)
{
	return substream->pcm->device - 1;
}

/* one verb to a capture device's codec: 12-bit verb + 8-bit payload, or
 * (verb < 0x10) a 4-bit verb + 16-bit payload */
static int ae9_xcap_verb(struct creative_chip *chip, const struct ae9_xcap *x,
			 unsigned int nid, unsigned int verb, unsigned int payload,
			 u32 *resp)
{
	u32 v = (x->cad << 28) | (nid << 20), r;

	v |= verb < 0x10 ? (verb << 16) | (payload & 0xffff) :
			   (verb << 8) | (payload & 0xff);
	if (chip->corb_buf)
		return creative_ae9_corb_send_verb_sync(chip, v, resp ? resp : &r);
	return creative_ae9_icw_send_verb(chip, v, resp ? resp : &r);
}

static int creative_ae9_xcap_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	unsigned int i = ae9_xcap_idx(substream);
	const struct ae9_xcap *x = &ae9_xcaps[i];
	int err;

	chip->xcap[i].substream = substream;
	runtime->hw.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
			   SNDRV_PCM_INFO_INTERLEAVED;
	/* 24-bit in 32-bit containers, as the XLR capture */
	runtime->hw.formats = SNDRV_PCM_FMTBIT_S32_LE;
	runtime->hw.channels_min = 2;
	runtime->hw.channels_max = 2;
	runtime->hw.rates = x->rates;
	runtime->hw.rate_min = x->rate_min;
	runtime->hw.rate_max = x->rate_max;
	if (i == AE9_XCAP_WUH) {
		runtime->hw.rates |= SNDRV_PCM_RATE_96000;
		runtime->hw.rate_max = 96000;
	}
	if (i == AE9_XCAP_WUH) {
		/* 2 or 6 channels: Windows records the card's mix as 48 kHz /
		 * 24-bit / 6 ch from this converter for its PC-side encoder
		 * loop (capture 6, memory optical-spdif); a software AC-3
		 * encoder needs the 5.1 mix the same way */
		static const unsigned int wuh_ch[] = { 2, 6 };
		static const struct snd_pcm_hw_constraint_list wuh_ch_list = {
			.count = ARRAY_SIZE(wuh_ch), .list = wuh_ch,
		};

		runtime->hw.channels_max = 6;
		snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS,
					   &wuh_ch_list);
	}
	runtime->hw.buffer_bytes_max = CREATIVE_PCM_BUF_MAX;
	runtime->hw.period_bytes_min = 1024;
	runtime->hw.period_bytes_max = CREATIVE_PCM_BUF_MAX / 2;
	runtime->hw.periods_min = 2;
	runtime->hw.periods_max = CREATIVE_CAP_BDL_MAX;
	snd_pcm_hw_constraint_msbits(runtime, 0, 32, 24);
	snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	/* whole periods; BDL entries want 128-byte aligned addresses */
	snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 128);
	err = ae9_whole_periods(runtime, 128, false);	/* buffer ends as playback */
	if (err < 0)
		chip->xcap[i].substream = NULL;
	return err;
}

static int creative_ae9_xcap_close(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);

	chip->xcap[ae9_xcap_idx(substream)].substream = NULL;
	return 0;
}

static void creative_ae9_xcap_bdl_free(struct creative_chip *chip, unsigned int i)
{
	struct ae9_xcap_state *st = &chip->xcap[i];

	if (st->bdl_raw) {
		if (ae9_bdl_may_free(chip, ae9_xcaps[i].base, st->bdl_dma, "capture BDL free"))
			dma_free_coherent(chip->dev, st->bdl_size, st->bdl_raw, st->bdl_dma);
		st->bdl_raw = NULL;
		st->bdl = NULL;
	}
}

static int creative_ae9_xcap_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *hw_params)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	unsigned int i = ae9_xcap_idx(substream);
	struct ae9_xcap_state *st = &chip->xcap[i];
	int ret;

	creative_ae9_xcap_bdl_free(chip, i);
	ret = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
	if (ret < 0)
		return ret;
	st->bdl_size = ae9_dma_size(chip, CREATIVE_CAP_BDL_MAX * 16 + 128);
	st->bdl_raw = dma_alloc_coherent(chip->dev, st->bdl_size, &st->bdl_dma,
					 GFP_KERNEL);
	if (!st->bdl_raw) {
		snd_pcm_lib_free_pages(substream);
		return -ENOMEM;
	}
	st->bdl = PTR_ALIGN(st->bdl_raw, 128);
	return 0;
}

static int creative_ae9_xcap_hw_free(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	unsigned int i = ae9_xcap_idx(substream);
	const struct ae9_xcap *x = &ae9_xcaps[i];

	if (x->signal_pin) {
		chip->xcap[i].sil_running = false;
		hrtimer_cancel(&chip->xcap[i].sil_timer);
		cancel_delayed_work_sync(&chip->xcap[i].sense_work);
	}
	ae9_xcap_verb(chip, x, x->nid, 0x706, 0, NULL);	/* off the stream */
	creative_ae9_xcap_bdl_free(chip, i);
	return snd_pcm_lib_free_pages(substream);
}

static int creative_ae9_xcap_prepare(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int i = ae9_xcap_idx(substream);
	const struct ae9_xcap *x = &ae9_xcaps[i];
	struct ae9_xcap_state *st = &chip->xcap[i];
	unsigned int base = x->base;
	unsigned int period = snd_pcm_lib_period_bytes(substream);
	unsigned int periods = runtime->periods;
	dma_addr_t bdl_dma = ALIGN(st->bdl_dma, 128);
	dma_addr_t addr = runtime->dma_addr;
	unsigned int k;
	u16 format;
	u32 ctl;

	if (creative_ae9_dead(chip))	/* the card stopped answering */
		return -ENODEV;

	if (periods > CREATIVE_CAP_BDL_MAX)
		return -EINVAL;
	ae9_wait_hp_upload(chip);
	/*
	 * Optical: with no S/PDIF signal the codec sends nothing and the
	 * stream never moves. That used to fail here (-EIO), which also made
	 * PipeWire drop the source. Now the recording runs as silence on a
	 * timer until a signal comes in (pin sense bit 31), then restarts on
	 * the real stream (sense_work, an xrun PipeWire recovers from).
	 */
	if (x->signal_pin) {
		u32 sense = 0;

		st->no_signal = ae9_xcap_verb(chip, x, x->signal_pin, 0xf09, 0, &sense) == 0 &&
				!(sense & 0x80000000);
		if (st->no_signal) {
			st->n_nosig++;
			dev_info_ratelimited(chip->dev,
				"optical in: no signal; recording silence until one comes in\n");
			memset(runtime->dma_area, 0, snd_pcm_lib_buffer_bytes(substream));
			st->sil_period = ns_to_ktime(div_u64((u64)runtime->period_size * NSEC_PER_SEC,
							     runtime->rate));
		}
	}
	creative_ae9_stream_reset(chip, base);

	memset(st->bdl, 0, CREATIVE_CAP_BDL_MAX * 16);
	for (k = 0; k < periods; k++) {
		__le64 *bdl_addr = st->bdl + k * 16;
		__le32 *bdl_len = st->bdl + k * 16 + 8;
		__le32 *bdl_ioc = st->bdl + k * 16 + 12;

		*bdl_addr = cpu_to_le64(addr);
		*bdl_len = cpu_to_le32(period);
		*bdl_ioc = cpu_to_le32(1);
		addr += period;
	}
	wmb();

	ctl = readl(chip->hda_regs + base + AZX_REG_SD_CTL);
	ctl &= ~(0xf << 20);
	ctl |= (x->tag & 0xf) << 20;
	writel(ctl & SD_CTL_WMASK, chip->hda_regs + base + AZX_REG_SD_CTL);
	wmb();
	format = creative_ae9_stream_format(runtime->channels, 24, runtime->rate);
	creative_hda_writel(chip, base + AZX_REG_SD_CBL, period * periods);
	creative_hda_writew(chip, base + AZX_REG_SD_LVI, periods - 1);
	creative_hda_writew(chip, base + AZX_REG_SD_FORMAT, format);
	creative_hda_writel(chip, base + AZX_REG_SD_BDLPL, lower_32_bits(bdl_dma));
	creative_hda_writel(chip, base + AZX_REG_SD_BDLPU, upper_32_bits(bdl_dma));
	ae9_dma_trace_prep(chip, x->name, base, substream, bdl_dma);

	/* the converter onto the stream: same format, tag, channel 0 */
	ae9_xcap_verb(chip, x, x->nid, 0x2, format, NULL);
	ae9_xcap_verb(chip, x, x->nid, 0x706, x->tag << 4, NULL);
	ae9_dbg(chip, "%s prepare: SD_FMT=0x%04x\n", x->name, format);
	return 0;
}

static int creative_ae9_xcap_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	unsigned int i = ae9_xcap_idx(substream);
	const struct ae9_xcap *x = &ae9_xcaps[i];
	struct ae9_xcap_state *st = &chip->xcap[i];
	unsigned int base = x->base;
	bool sil = x->signal_pin && st->no_signal;

	if (cmd == SNDRV_PCM_TRIGGER_START && creative_ae9_dead(chip))
		return -ENODEV;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (sil) {
			st->sil_t0 = ktime_get();
			st->sil_running = true;
			hrtimer_start(&st->sil_timer, st->sil_period, HRTIMER_MODE_REL_SOFT);
		} else {
			creative_ae9_stream_start(chip, base);
		}
		if (x->signal_pin) {
			st->last_lpib = ~0U;
			st->last_irqs = READ_ONCE(st->irqs) - 1;
			queue_delayed_work(chip->wq, &st->sense_work, HZ);
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		if (sil) {
			/* atomic here: no waiting; the tick checks sil_running */
			st->sil_running = false;
			hrtimer_try_to_cancel(&st->sil_timer);
		} else {
			creative_ae9_stream_stop(chip, base);
		}
		if (x->signal_pin)
			cancel_delayed_work(&st->sense_work);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/* Silence clock: one period per tick, as the stream's interrupt would. */
static enum hrtimer_restart ae9_sil_tick(struct hrtimer *t)
{
	struct ae9_xcap_state *st = container_of(t, struct ae9_xcap_state, sil_timer);
	struct snd_pcm_substream *ss = st->substream;

	if (!ss || !st->sil_running)
		return HRTIMER_NORESTART;
	snd_pcm_period_elapsed(ss);
	if (!st->sil_running)
		return HRTIMER_NORESTART;
	hrtimer_forward_now(t, st->sil_period);
	return HRTIMER_RESTART;
}

/*
 * Optical in, once a second while it records: a signal came in during
 * silence, or the signal went (or the stream stopped moving) during a real
 * recording -> an xrun; PipeWire (or the app) prepares again, and prepare
 * picks the mode. Process context: the sense read is one codec-2 verb.
 */
static void ae9_sense_work(struct work_struct *w)
{
	struct ae9_xcap_state *st = container_of(to_delayed_work(w),
						 struct ae9_xcap_state, sense_work);
	struct creative_chip *chip = st->chip;
	struct snd_pcm_substream *ss = st->substream;
	const struct ae9_xcap *x = &ae9_xcaps[0];
	u32 sense = 0, lpib;
	bool sig, restart = false;

	if (!ss || !ss->runtime || ss->runtime->state != SNDRV_PCM_STATE_RUNNING)
		return;		/* stopped: the next start queues this again */
	if (chip->card_gone || chip->card_hung ||
	    ae9_xcap_verb(chip, x, x->signal_pin, 0xf09, 0, &sense) < 0)
		goto again;
	sig = sense & 0x80000000;
	if (st->no_signal && sig) {
		dev_info(chip->dev, "optical in: signal came in; restarting the recording on it\n");
		st->n_came++;
		restart = true;
	} else if (!st->no_signal) {
		/*
		 * Stopped = no period interrupt AND the same buffer position
		 * since the last check. The position alone gave false stops:
		 * the delayed work often runs ~1024 ms apart (timer-wheel
		 * rounding), exactly 6 laps of a 16384-frame buffer at 96 kHz,
		 * so a moving stream read the same LPIB (PS3 on optical in, a
		 * restart and a click about once a minute). The position
		 * stays for periods longer than the 1 s check.
		 */
		unsigned int irqs = READ_ONCE(st->irqs);

		lpib = readl(chip->hda_regs + x->base + AZX_REG_SD_LPIB);
		if (!sig || (lpib == st->last_lpib && irqs == st->last_irqs)) {
			dev_info(chip->dev, "optical in: %s; recording silence until a signal comes in\n",
				 sig ? "the stream stopped moving" : "signal lost");
			if (sig)
				st->n_stopped++;
			else
				st->n_lost++;
			restart = true;
		}
		st->last_lpib = lpib;
		st->last_irqs = irqs;
	}
	if (restart) {
		snd_pcm_stop_xrun(ss);
		return;		/* the next start queues this again */
	}
again:
	queue_delayed_work(chip->wq, &st->sense_work, HZ);
}

static snd_pcm_uframes_t
creative_ae9_xcap_pointer(struct snd_pcm_substream *substream)
{
	struct creative_chip *chip = snd_pcm_substream_chip(substream);
	unsigned int i = ae9_xcap_idx(substream);
	unsigned int base = ae9_xcaps[i].base;
	struct ae9_xcap_state *st = &chip->xcap[i];
	unsigned int pos;

	if (ae9_xcaps[i].signal_pin && st->no_signal) {
		/* from elapsed time, not whole periods: PipeWire's timer
		 * scheduling reads the position between periods */
		u64 ns = ktime_to_ns(ktime_sub(ktime_get(), st->sil_t0));
		u64 frames = mul_u64_u32_div(ns, substream->runtime->rate, NSEC_PER_SEC);

		return do_div(frames, substream->runtime->buffer_size);
	}

	pos = readl(chip->hda_regs + base + AZX_REG_SD_LPIB);
	if (pos >= snd_pcm_lib_buffer_bytes(substream))
		pos = 0;
	return bytes_to_frames(substream->runtime, pos);
}

static const struct snd_pcm_ops creative_ae9_xcap_ops = {
	.open		= creative_ae9_xcap_open,
	.close		= creative_ae9_xcap_close,
	.hw_params	= creative_ae9_xcap_hw_params,
	.hw_free	= creative_ae9_xcap_hw_free,
	.prepare	= creative_ae9_xcap_prepare,
	.trigger	= creative_ae9_xcap_trigger,
	.pointer	= creative_ae9_xcap_pointer,
};

/* Called from the IRQ handler: acknowledge extra input stream i, notify ALSA. */
void creative_ae9_xcap_irq(struct creative_chip *chip, unsigned int i)
{
	unsigned int base = ae9_xcaps[i].base;
	u8 sts = readb(chip->hda_regs + base + AZX_REG_SD_STS);

	if (!sts)
		return;
	writeb(sts & SD_INT_MASK, chip->hda_regs + base + AZX_REG_SD_STS);
	if ((sts & SD_INT_COMPLETE) && chip->xcap[i].substream) {
		WRITE_ONCE(chip->xcap[i].irqs, chip->xcap[i].irqs + 1);
		snd_pcm_period_elapsed(chip->xcap[i].substream);
	}
}

static void creative_ae9_xcap_pcm_free(struct snd_pcm *pcm)
{
	creative_ae9_xcap_bdl_free(pcm->private_data, pcm->device - 1);
}

/* PCM device i+1 for ae9_xcaps[i] */
static void creative_ae9_xcap_new(struct creative_chip *chip, unsigned int i)
{
	const struct ae9_xcap *x = &ae9_xcaps[i];
	struct snd_pcm *pcm;
	int ret;

	ret = snd_pcm_new(chip->card, x->name, i + 1, 0, 1, &pcm);
	if (ret < 0) {
		dev_warn(chip->dev, "%s device not added (%d)\n", x->name, ret);
		return;
	}
	pcm->private_data = chip;
	ae9_pcm_track(chip, pcm);
	pcm->private_free = creative_ae9_xcap_pcm_free;
	chip->xcap[i].chip = chip;
	if (x->signal_pin) {
#ifdef AE9_HAVE_HRTIMER_SETUP	/* Makefile: from the kernel's header */
		hrtimer_setup(&chip->xcap[i].sil_timer, ae9_sil_tick, CLOCK_MONOTONIC,
			      HRTIMER_MODE_REL_SOFT);
#else		/* before 6.13 (Ubuntu 24.04: 6.8) */
		hrtimer_init(&chip->xcap[i].sil_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
		chip->xcap[i].sil_timer.function = ae9_sil_tick;
#endif
		INIT_DELAYED_WORK(&chip->xcap[i].sense_work, ae9_sense_work);
	}
	strscpy(pcm->name, x->name, sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &creative_ae9_xcap_ops);
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV, chip->dev,
					      ae9_dma_size(chip, CREATIVE_PCM_BUF_MAX),
					      ae9_dma_size(chip, CREATIVE_PCM_BUF_MAX));
}

/* ------------------------------------------------------------------ */
/* PCM device init / free						  */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* debugfs: the playback DMA FIFOs					  */
/* ------------------------------------------------------------------ */

/* one stream for state's line: SDnFIFOS read at the last start (RUN set) in
 * bytes, frames and time (no card access) */
static void ae9_fifo_show_one(struct seq_file *m, const char *name, const struct ae9_fifo *f)
{
	unsigned int fb = f->frame_bytes, size = f->fifos_run + 1U;

	if (!fb || !f->rate) {
		seq_printf(m, "%s none prepared", name);
		return;
	}
	seq_printf(m, "%s fmt 0x%04x: ", name, f->fmt);
	if (f->fifos_run)
		seq_printf(m, "FIFOS 0x%03x at RUN = %u bytes = %u.%u frames = %llu us",
			   f->fifos_run, size, size / fb, size * 10 / fb % 10,
			   div_u64((u64)size * 1000000, fb * f->rate));
	else
		seq_puts(m, "FIFOS not read yet (at the start)");
}

void creative_ae9_pcm_fifo_show(struct seq_file *m, struct creative_chip *chip)
{
	seq_puts(m, "DMA FIFO: ");
	ae9_fifo_show_one(m, "playback", &chip->pb_fifo);
	ae9_fifo_show_one(m, "; optical out", &chip->opt_fifo);
	seq_putc(m, '\n');
}

/*
 * debugfs "readahead": how far the DMA is ahead of LPIB right now on the
 * playback streams that run, from the DMA position buffer (HD-Audio spec
 * 3.3.32: the controller writes each stream's DMA position to memory),
 * switched on for the ~30 ms this takes and off again. On demand only (the
 * pcm_posbuf option measured it until 0.12.12): it checks SDnFIFOS against
 * the read-ahead at any format. Per stream: the least, mean and most bytes
 * ahead (samples where LPIB moved during the sample are left out), how many
 * samples were above FIFOS + 1, and how the DMA position moves (by how much
 * at a time, and how far LPIB went between). Behind the card's bridge the
 * DMA position counts reads requested, not data arrived (above), so this is
 * an upper bound of what's fetched.
 */
#define AE9_RA_SAMPLES	4000

struct ae9_ra {
	unsigned int base, cbl, idx, size;
	const struct ae9_fifo *f;
	bool run;
	unsigned int n, steady, above, moves;
	u32 gmin, gmax, smin, smax, lmin, lmax, last_d, last_l;
	u64 gsum;
};

static void ae9_ra_sample(struct creative_chip *chip, struct ae9_ra *r, const __le32 *posbuf)
{
	u32 l1, l2, d, gap, step, lmove;

	l1 = readl(chip->hda_regs + r->base + AZX_REG_SD_LPIB);
	d = le32_to_cpu(READ_ONCE(posbuf[r->idx * 2]));
	l2 = readl(chip->hda_regs + r->base + AZX_REG_SD_LPIB);
	r->n++;
	if (l1 != l2 || l1 >= r->cbl || d >= r->cbl)
		return;
	r->steady++;
	gap = (d + r->cbl - l1) % r->cbl;
	r->gmin = min(r->gmin, gap);
	r->gmax = max(r->gmax, gap);
	r->gsum += gap;
	if (gap > r->size)
		r->above++;
	if (d == r->last_d)
		return;
	if (r->moves) {
		step = (d + r->cbl - r->last_d) % r->cbl;
		lmove = (l1 + r->cbl - r->last_l) % r->cbl;
		r->smin = min(r->smin, step);
		r->smax = max(r->smax, step);
		r->lmin = min(r->lmin, lmove);
		r->lmax = max(r->lmax, lmove);
	}
	r->moves++;
	r->last_d = d;
	r->last_l = l1;
}

static void ae9_ra_report(struct seq_file *m, const char *name, const struct ae9_ra *r)
{
	unsigned int fb = r->f->frame_bytes ?: 1;

	if (!r->run) {
		seq_printf(m, "%s: not running\n", name);
		return;
	}
	seq_printf(m, "%s: fmt 0x%04x, %u bytes/frame, buffer %u bytes; FIFOS + 1 = %u bytes (%u.%u frames)\n",
		   name, r->f->fmt, fb, r->cbl, r->size, r->size / fb, r->size * 10 / fb % 10);
	if (!r->steady) {
		seq_printf(m, "  no steady sample of %u (DMA position %s)\n", r->n,
			   r->last_d == U32_MAX ? "never written" : "outside the buffer");
		return;
	}
	seq_printf(m, "  DMA ahead of LPIB: least %u, mean %llu, most %u bytes = %u.%u / %llu / %u.%u frames (%u of %u samples steady)\n",
		   r->gmin, div_u64(r->gsum, r->steady), r->gmax,
		   r->gmin / fb, r->gmin * 10 / fb % 10, div_u64(r->gsum, r->steady * fb),
		   r->gmax / fb, r->gmax * 10 / fb % 10, r->steady, r->n);
	seq_printf(m, "  above FIFOS + 1: %u samples\n", r->above);
	if (r->moves > 1)
		seq_printf(m, "  DMA position moved %u times, by %u-%u bytes; LPIB moved %u-%u bytes between\n",
			   r->moves, r->smin, r->smax, r->lmin, r->lmax);
	else
		seq_printf(m, "  DMA position moved %u times\n", r->moves);
}

static int ae9_readahead_show(struct seq_file *m, void *v)
{
	static DEFINE_MUTEX(ra_mutex);	/* one reader at a time: DPLBASE is global */
	struct creative_chip *chip = m->private;
	struct ae9_ra r[2] = {
		{ .base = chip->playback_stream_base, .f = &chip->pb_fifo },
		{ .base = CREATIVE_OPT_STREAM_BASE, .f = &chip->opt_fifo },
	};
	size_t sz = ae9_dma_size(chip, PAGE_SIZE);
	dma_addr_t dma;
	__le32 *posbuf;
	unsigned int i, k;
	int ret = 0;

	if (chip->suspended || chip->card_gone || chip->card_hung || !chip->corb_buf) {
		seq_puts(m, "card not readable now (suspended, gone or not up)\n");
		return 0;
	}
	mutex_lock(&ra_mutex);
	if (readl(chip->hda_regs + AZX_REG_DPLBASE) & AZX_DPLBASE_ENABLE) {
		seq_puts(m, "the DMA position buffer is on already: not touched\n");
		goto out;
	}
	for (k = 0; k < 2; k++) {
		const struct ae9_fifo *f = r[k].f;

		r[k].run = readl(chip->hda_regs + r[k].base + AZX_REG_SD_CTL) & SD_CTL_DMA_START;
		r[k].cbl = readl(chip->hda_regs + r[k].base + AZX_REG_SD_CBL);
		r[k].idx = (r[k].base - 0x80) / 0x20;
		r[k].size = f->fifos_run + 1U;
		r[k].gmin = U32_MAX;
		r[k].smin = U32_MAX;
		r[k].lmin = U32_MAX;
		r[k].last_d = U32_MAX;
		if (!r[k].cbl || r[k].cbl == U32_MAX || !f->fifos_run)
			r[k].run = false;
	}
	if (!r[0].run && !r[1].run) {
		seq_puts(m, "no playback stream runs: start one, then read this again\n");
		goto out;
	}
	posbuf = dma_alloc_coherent(chip->dev, sz, &dma, GFP_KERNEL);
	if (!posbuf) {
		ret = -ENOMEM;
		goto out;
	}
	memset(posbuf, 0xff, PAGE_SIZE);
	creative_hda_writel(chip, AZX_REG_DPUBASE, upper_32_bits(dma));
	creative_hda_writel(chip, AZX_REG_DPLBASE, lower_32_bits(dma) | AZX_DPLBASE_ENABLE);
	usleep_range(2000, 3000);
	for (i = 0; i < AE9_RA_SAMPLES; i++) {
		for (k = 0; k < 2; k++)
			if (r[k].run)
				ae9_ra_sample(chip, &r[k], posbuf);
		udelay(4);
		if (!(i % 128))
			cond_resched();
	}
	/* off before the memory goes: the read-back completes after any
	 * position write the controller made before it (PCI ordering) */
	creative_hda_writel(chip, AZX_REG_DPLBASE, 0);
	creative_hda_writel(chip, AZX_REG_DPUBASE, 0);
	creative_hda_readl(chip, AZX_REG_DPLBASE);
	usleep_range(1000, 2000);
	dma_free_coherent(chip->dev, sz, posbuf, dma);
	ae9_ra_report(m, "playback", &r[0]);
	ae9_ra_report(m, "optical out", &r[1]);
out:
	mutex_unlock(&ra_mutex);
	return ret;
}
DEFINE_SHOW_ATTRIBUTE(ae9_readahead);

void creative_ae9_pcm_debugfs(struct creative_chip *chip)
{
	debugfs_create_file("readahead", 0400, chip->dbg_dir, chip, &ae9_readahead_fops);
}

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
	ae9_pcm_track(chip, pcm);
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
	ret = snd_ctl_add(chip->card, snd_ctl_new1(&cap_bits_control, chip));
	if (ret < 0)
		dev_warn(chip->dev, "Capture Bit Depth control not added (%d)\n", ret);

	/* optical: out follows the main stream, in is PCM device 1 */
	chip->spdif_out = spdif_out;
	ret = snd_ctl_add(chip->card, snd_ctl_new1(&spdif_out_control, chip));
	if (ret < 0)
		dev_warn(chip->dev, "Optical Out control not added (%d)\n", ret);
	chip->spdif_mirror = optical_mirror;
	ret = snd_ctl_add(chip->card, snd_ctl_new1(&spdif_mirror_control, chip));
	if (ret < 0)
		dev_warn(chip->dev, "Optical Out Mirror control not added (%d)\n", ret);
	creative_ae9_xcap_new(chip, 0);	/* optical in */
	creative_ae9_xcap_new(chip, 1);	/* What U Hear */
	creative_ae9_xcap_new(chip, 2);	/* head unit rear RCA in */
	creative_ae9_opt_new(chip);	/* separate optical out */

	dev_info(chip->dev, "PCM devices: 0 analog, 1 optical in, 2 What U Hear, 3 rear RCA in, 4 optical out\n");
	return 0;
}

/* pcm->private_free: hw_free normally releases the BDLs already; this
 * catches any left over when the card goes away. */
static void creative_ae9_pcm_free(struct snd_pcm *pcm)
{
	struct creative_chip *chip = pcm->private_data;

	if (chip->playback_bdl_raw) {
		if (ae9_bdl_may_free(chip, chip->playback_stream_base, chip->playback_bdl_dma,
				     "playback at card free"))
			dma_free_coherent(chip->dev, chip->playback_bdl_size,
					  chip->playback_bdl_raw,
					  chip->playback_bdl_dma);
		chip->playback_bdl_raw = NULL;
		chip->playback_bdl = NULL;
	}
	if (chip->opt_bdl_raw) {
		if (ae9_bdl_may_free(chip, CREATIVE_OPT_STREAM_BASE, chip->opt_bdl_dma,
				     "optical out at card free"))
			dma_free_coherent(chip->dev, chip->opt_bdl_size,
					  chip->opt_bdl_raw, chip->opt_bdl_dma);
		chip->opt_bdl_raw = NULL;
		chip->opt_bdl = NULL;
	}
	creative_ae9_cap_bdl_free(chip);
	chip->pcm = NULL;
}
