/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Creative Sound Blaster AE-9 / X-Fi (CA0132) HDA controller driver
 *
 * PCI BAR layout:
 *   BAR0 = standard HDA registers (16 KB)
 *   BAR2 = Creative extension registers (16 KB, via IDT Tsi381 bridge)
 *
 * Reference: docs/history/Creative.md, ae9_mmio_capture.txt
 */

#ifndef __CREATIVE_AE9_H
#define __CREATIVE_AE9_H

#include <linux/types.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/hda_register.h>

/*
 * MMIO trace hook (module param mmio_log, needs verb_log): every
 * readb/w/l and writeb/w/l in this driver goes through these wrappers, which
 * log accesses that fall inside the AE-9's BAR0/BAR2. debugfs verblog prints
 * them in QEMU's vfio_region_read/write trace format so they can be diffed
 * against a Windows capture's mmio_trace.log.
 */
void ae9_mmio_log(const volatile void __iomem *addr, u32 val, int size, bool write);

static inline u8 ae9_readb(const volatile void __iomem *a)
{ u8 v = ioread8((void __iomem *)a); ae9_mmio_log(a, v, 1, false); return v; }
static inline u16 ae9_readw(const volatile void __iomem *a)
{ u16 v = ioread16((void __iomem *)a); ae9_mmio_log(a, v, 2, false); return v; }
static inline u32 ae9_readl(const volatile void __iomem *a)
{ u32 v = ioread32((void __iomem *)a); ae9_mmio_log(a, v, 4, false); return v; }
static inline void ae9_writeb(u8 v, volatile void __iomem *a)
{ iowrite8(v, (void __iomem *)a); ae9_mmio_log(a, v, 1, true); }
static inline void ae9_writew(u16 v, volatile void __iomem *a)
{ iowrite16(v, (void __iomem *)a); ae9_mmio_log(a, v, 2, true); }
static inline void ae9_writel(u32 v, volatile void __iomem *a)
{ iowrite32(v, (void __iomem *)a); ae9_mmio_log(a, v, 4, true); }

#undef readb
#undef readw
#undef readl
#undef writeb
#undef writew
#undef writel
#define readb(a)	ae9_readb(a)
#define readw(a)	ae9_readw(a)
#define readl(a)	ae9_readl(a)
#define writeb(v, a)	ae9_writeb((v), (a))
#define writew(v, a)	ae9_writew((v), (a))
#define writel(v, a)	ae9_writel((v), (a))

#define CREATIVE_AE9_DRV_NAME		"snd-hda-codec-creative-ae9"
#define CREATIVE_AE9_DRV_VERSION	"0.6.8"
#define CREATIVE_AE9_DRV_DESC		"Creative Sound Blaster AE-9 HDA controller"

/* ------------------------------------------------------------------ */
/* PCI BAR assignment (varies by device variant)			  */
/* ------------------------------------------------------------------ */
#define CREATIVE_HDA_BAR		0
#define CREATIVE_EXT_BAR		2

/* ------------------------------------------------------------------ */
/* Creative extension register offsets (relative to BAR2)		  */
/* ------------------------------------------------------------------ */
#define CREATIVE_REG_HW_CTRL		0x1c
/* HW_CTRL bit fields */
#define   CREATIVE_HW_CTRL_INIT		0x880480   /* ae9_mmio_capture.txt:65 */

#define CREATIVE_REG_STREAM_CTRL	0x100	/* main stream parameter ctrl */
#define CREATIVE_REG_STREAM_ENABLE	0x304	/* stream enable bitmask */
#define CREATIVE_REG_STREAM_RESET	0x305	/* stream reset bitmask */
#define CREATIVE_REG_STREAM_INT_BASE	0x454	/* per-stream int base addr */

/* ------------------------------------------------------------------ */
/* CA0132 extended stream descriptor registers				  */
/* ------------------------------------------------------------------ */
#define CA0132_SD_FIFOS		0x20	/* FIFO size (bytes consumed) */

/* ------------------------------------------------------------------ */
/* CORB / RIRB constants						  */
/* ------------------------------------------------------------------ */
#define CREATIVE_CORB_ENTRIES		256
#define CREATIVE_RIRB_ENTRIES		256
#define CREATIVE_CORB_SIZE		(CREATIVE_CORB_ENTRIES * 4)
#define CREATIVE_RIRB_SIZE		(CREATIVE_RIRB_ENTRIES * 8)
#define CREATIVE_RIRB_RESP_THRESHOLD	0xc0

/* RIRB response entry format */
struct creative_rirb_entry {
	u32 response;
	u32 flags;	/* bit 4: unsolicited, bits 3:0: codec addr */
};

/* ------------------------------------------------------------------ */
/* Maximum timeouts (milliseconds)					  */
/* ------------------------------------------------------------------ */
#define CREATIVE_RESET_TIMEOUT_MS	1000
#define CREATIVE_CODEC_PROBE_DELAY_MS	200
#define CREATIVE_IC_CMD_TIMEOUT_MS	100

/* ------------------------------------------------------------------ */
/* Vendor / device IDs for Creative HDA controllers			  */
/* ------------------------------------------------------------------ */
#define PCI_VENDOR_ID_CREATIVE		0x1102

enum { DEVICE_AE7_AE9 = 0x0010,	  /* AE-7 / AE-9 (CA0132 Sound Core3D) */
       DEVICE_RECON3D = 0x0012,	  /* Recon3D				  */
       DEVICE_AE9_0013 = 0x0013,  /* AE-9 (standalone)		  */
        DEVICE_AE9_0014 = 0x0014,  /* AE-9 (PCIe variant)		  */ };

/* SD_CTL is 24 bits; byte 3 of the same dword is SD_STS, whose bits clear
 * when written with 1. Mask it out of every SD_CTL read-modify-write, or
 * the write-back acks pending status (e.g. a period interrupt). */
#define SD_CTL_WMASK	0x00ffffff
#define CREATIVE_SUBSYS_AE9		0x0071

/* ------------------------------------------------------------------ */
/* Per-device BAR layout descriptor					  */
/* ------------------------------------------------------------------ */
struct creative_ae9_bar_layout {
	unsigned int	hda_bar;
	unsigned int	ext_bar;
	unsigned int	ext_min_size;
};

/* ------------------------------------------------------------------ */
/* Main driver data structure					  */
/* ------------------------------------------------------------------ */
struct creative_chip {
	struct pci_dev		*pci;
	struct device		*dev;
	struct snd_card		*card;

	void __iomem		*hda_regs;	/* BAR0 */
	void __iomem		*ext_regs;	/* BAR2 */

	spinlock_t		lock;
	struct mutex		corb_mutex;	/* serialise CORB verb sends */
	struct mutex		scp_mutex;	/* one SCP message at a time */
	struct mutex		replay_mutex;	/* one captured verb block at a time */
	struct mutex		ca0113_mutex;	/* one CA0113 command list at a time */

	unsigned int		codec_mask;	/* bitmask from STATESTS */
	unsigned int		codec_count;
	unsigned int		codec_ok_mask;	/* codecs that answered at detection */

	/* ICW protocol state */
	u16			last_irs_ack;	/* ACK value for next IRS write */

	/* CORB / RIRB DMA buffers */
	void			*corb_buf;	/* CPU virtual address */
	dma_addr_t		corb_dma;	/* DMA bus address */
	void			*rirb_buf;	/* CPU virtual address */
	dma_addr_t		rirb_dma;	/* DMA bus address */

	/* CORB write pointer (tracked in memory) */
	u16			corb_wp;
	/* Last RIRB read pointer (for response processing) */
	u16			rirb_rp;

	/* IRQ */
	int			irq;

	/* PCM playback stream */
	struct snd_pcm		*pcm;
	struct snd_pcm_substream *substream;
	void			*playback_buf;	/* DMA audio buffer */
	dma_addr_t		playback_dma;
	void			*playback_bdl;	/* BDL descriptor list (aligned) */
	dma_addr_t		playback_bdl_dma; /* DMA address (aligned) */
	void			*playback_bdl_raw; /* raw alloc for free */
	size_t			playback_bdl_size;
	unsigned int		playback_buf_size;
	unsigned int		playback_period_size;
	unsigned int		playback_stream_tag;
	unsigned int		playback_stream_base; /* SDO offset in BAR0 */
	struct snd_pcm_substream *playback_substream; /* for period_elapsed */
	/* capture (input stream 0, XLR via codec 1 node 0x07) */
	struct snd_pcm_substream *capture_substream;
	void			*cap_bdl_raw;
	void			*cap_bdl;
	dma_addr_t		cap_bdl_dma;
	size_t			cap_bdl_size;
	unsigned int		cap_start_sent;
	unsigned int		mic_boost;	/* node 0x12 input amp, 10 dB steps */
	unsigned int		mic_level;	/* 0..99 = mute, -89..+9 dB (default 99) */
	unsigned int		dac_filter;	/* 0 boot default, 1 fast, 2 slow */
	u8			dac48_07;	/* last value written to DAC 48/07 */
	struct creative_ae9_fx {		/* SBX effects, creative_ae9_fx.c */
		bool		direct;		/* Windows' Direct Mode (default on) */
		bool		sbx;		/* the SBX button; acts with Direct Mode off */
		bool		on[5];		/* Surround, Dialog+, Smart Vol, Crystalizer, Bass */
		u8		level[5];	/* 0..100 */
		u8		sv_mode;	/* Smart Volume: Normal, Loud, Night */
		bool		eq_on;
		s16		eq[11];		/* 0.1 dB: pre-gain, 31 Hz .. 16 kHz */
		u8		eq_preset;
		u8		spk;		/* index into spk_cfgs (fx.c) */
		bool		scout;
		bool		dead;		/* DSP stopped answering */
		unsigned int	fails;
		bool		saved_sv_on, saved_eq_on;	/* before Scout Mode */
		u8		saved_sv_level, saved_sv_mode;
		s16		saved_eq[11];
		struct snd_kcontrol *kc[32];
		unsigned int	nkc;
	} fx;

	/* debugfs */
	struct dentry		*dbg_dir;

	/* Our own workqueue: the head-unit work sleeps (up to 600 ms on a
	 * knob hold), which doesn't belong on the shared system queue. */
	struct workqueue_struct	*wq;

	/* Delayed ACM head-unit re-init (MCU not ready at early boot) */
	struct delayed_work	acm_work;
	int			acm_retries;
	bool			acm_settled;

	/* Periodic head-unit heartbeat (keeps MCU/DAC alive) */
	struct delayed_work	hb_work;

	/* DSP DMA keepalive — fires every ~20ms during PCM playback.
	 * Sends the ChipIO/EXRAM verb sequence that drives the DSP DMA state
	 * machine (equivalent to the continuous CORB verb loop seen in Windows
	 * corb_during_play captures). Without it the DSP DMA freezes and
	 * 0x801c never increments. */
	atomic_t		dma_running;

	/* Deferred DSP standup (firmware + audio router + head unit), timed
	 * for ~20s so the DSP and head-unit MCU are ready.
	 */
	struct delayed_work	dsp_work;
	struct delayed_work	dsp_switch_work;	/* live switch to DSP mode */
	bool			dsp_switch_pending;
	bool			rt_ff_logged;	/* router 0xff line logged this bring-up */
	int			dsp_done;
	int			standup_done; /* set after ae9_standup completes — disables keepalive CORB reads */
	int			pcm_started;  /* set after first PCM start — skips one-time ChipIO setup */
	int			relay_engaged; /* set after relay fires — skip on repeated prepares */

	/* DSP SCP messaging state */
	unsigned int		dsp_wait_scp;
	unsigned int		dsp_wait_scp_header;
	/* SCP replies announced by the DSP (unsolicited RIRB entries from
	 * codec 1) and how many of them we have read out */
	atomic_t		scp_unsol;
	unsigned int		hs_done;	/* boot handshake already run */
	unsigned int		pcm_start_sent;	/* pcm_windows start block sent since open */
	unsigned int		pcm_start_rate;	/* stream rate that block was sent for */
	u16			pcm_start_fmt;	/* and its stream format */
	unsigned int		in05_ch;	/* DSP input stream 0x05 channels, 0 = off */
	bool			dsp_primed;
	bool			dsp_active;	/* DSP mode routing is up */	/* a full start block ran since bring-up */
	unsigned int		pcm_bits;	/* 24 or 32, latched at open */
	unsigned int		scp_unsol_done;
	/* head unit (post_windows): volume as attenuation in 0.5 dB steps,
	 * mute, pending apply, heartbeat ticks, last reply per poll frame */
	unsigned int		hu_att;
	unsigned int		hu_mute;
	unsigned int		hu_dirty;
	unsigned int		hu_ticks;
	struct snd_kcontrol	*hu_vol_ctl;
	struct snd_kcontrol	*hu_mute_ctl;
	void			*corb_keep, *rirb_keep;	/* rings parked over suspend */
	dma_addr_t		corb_keep_dma, rirb_keep_dma;
	struct work_struct	resume_work;
	bool			suspended;
	bool			resuming;	/* bring-up from resume: fail fast */
	bool			hs_ok;		/* last boot handshake completed */
	unsigned int		hu_output;	/* 0 speakers (RCA), 1 headphones */
	unsigned int		hu_att_out[2];	/* remembered volume per output */
	unsigned int		hu_name_until;	/* show "-SP-"/"-HP-" until this tick */
	int			hu_switch_to;	/* pending output switch, -1 none */
	unsigned int		hu_light;	/* knob light on */
	int			hu_sbx_led;	/* SBX LED as last sent, -1 unknown */
	bool			hu_sbx_press;	/* SBX button press to act on */
	bool			hu_st07_valid;
	u8			hu_st07;	/* 83 01 07 reply byte, logged on change */
	struct snd_kcontrol	*hu_out_ctl;
	unsigned int		hu_btn_down;
	unsigned long		hu_btn_since;
	unsigned long		hu_btn_seen;	/* last press seen by the fast poll */
	bool			hu_mute_logged;	/* mute state last logged */
	bool			hu_bit7_stuck;	/* 83 02 bit 7 stuck at held: use b1 reports */
	/* 83 01 02 status byte without bit 7 (knob button), for the plug
	 * log. Logged only; output switching stays manual. */
	bool			hu_status_valid;
	u8			hu_status;
	bool			overlay_sent;	/* overlay transfer ran this bring-up */
	unsigned long		hu_next_full;
};

/* ------------------------------------------------------------------ */
/* Function declarations						  */
/* ------------------------------------------------------------------ */

/* creative_ae9_corb.c */
int creative_ae9_corb_init(struct creative_chip *chip);
void creative_ae9_corb_free(struct creative_chip *chip);
int  creative_ae9_corb_send_verb(struct creative_chip *chip, u32 verb);
void creative_ae9_corb_send_verb_wait(struct creative_chip *chip, u32 verb);
int creative_ae9_corb_send_verb_sync(struct creative_chip *chip,
				     u32 verb, u32 *resp);

/* creative_ae9_i2c.c */
int creative_i2c_dsp_init(struct creative_chip *chip);
int creative_i2c_dsp_audio_loop_start(struct creative_chip *chip);
void creative_spi_frame_send(struct creative_chip *chip,
			     const u8 *tx, int tx_len);
extern bool creative_hb_skip_gpio4;
int creative_spi_frame_win(struct creative_chip *chip, const u8 *tx,
			   int tx_len, u8 *resp, int resp_len);
int creative_i2c_init(struct creative_chip *chip);
int creative_i2c_heartbeat(struct creative_chip *chip);
int creative_i2c_amp_enable(struct creative_chip *chip);
int creative_i2c_raw_tx(struct creative_chip *chip, u8 reg,
			const u8 *data, u8 len);

/* creative_ae9_pcm.c */
int creative_ae9_pcm_init(struct creative_chip *chip);

/* creative_ae9_core.c */
/* IRQ handler is static in core.c — not exported */
int creative_ae9_icw_send_verb(struct creative_chip *chip, u32 verb, u32 *resp);
u32 creative_ae9_verb(unsigned int cad, unsigned int nid,
		      unsigned int verb_id, unsigned int payload);
int creative_ae9_hw_reset(struct creative_chip *chip);
void creative_ae9_hw_init(struct creative_chip *chip);
void creative_ae9_replay_init_all(struct creative_chip *chip);

/* creative_ae9_dsp.c */
int creative_dsp_chipio_write(struct creative_chip *chip,
			      unsigned int addx, unsigned int data);
int creative_dsp_chipio_read(struct creative_chip *chip,
			     unsigned int addx, unsigned int *data);
void creative_dsp_enable_clocks(struct creative_chip *chip);
int creative_dsp_prepare(struct creative_chip *chip);
int creative_dsp_load_firmware(struct creative_chip *chip);
int creative_dsp_select_lineout(struct creative_chip *chip);
int creative_dsp_start_audio_streams(struct creative_chip *chip);
int creative_dsp_ae9_standup(struct creative_chip *chip);
int creative_dsp_headunit_dac_route(struct creative_chip *chip);
int creative_dsp_headunit_init_route(struct creative_chip *chip);
void creative_dsp_audio_config(struct creative_chip *chip);
int creative_dsp_ca0113_trigger7(struct creative_chip *chip);
int creative_dsp_ca0113_unmute_enable(struct creative_chip *chip);
int creative_dsp_ca0113_clock_off(struct creative_chip *chip);
int creative_dsp_ca0113_unmute(struct creative_chip *chip);
int creative_i2c_pcm_relay_engage(struct creative_chip *chip);
int creative_dsp_run(struct creative_chip *chip);
int creative_dsp_scp_reset(struct creative_chip *chip);
void creative_dsp_keepalive(struct creative_chip *chip);
void creative_dsp_write_audio_router(struct creative_chip *chip);
void creative_dsp_pcm_start(struct creative_chip *chip, unsigned int stream_tag);
int creative_dsp_enable_dac_output(struct creative_chip *chip);
int creative_dsp_amp_engage(struct creative_chip *chip);
int creative_ae9_ca0113_handshake(struct creative_chip *chip);
int creative_ae9_ca0113_second_reset(struct creative_chip *chip);
int creative_dsp_post_relay_dac_levels(struct creative_chip *chip);
int creative_dsp_select_out_params(struct creative_chip *chip);
void creative_ae9_vlog(u32 val, u32 ex, bool is_resp);
void creative_ae9_cold_pcm_replay(struct creative_chip *chip, bool start, bool atomic,
				  unsigned int rate, u16 format);
u16 creative_ae9_stream_format(unsigned int channels, unsigned int bits,
				unsigned int rate);
void creative_ae9_cap_replay(struct creative_chip *chip, bool start);
void creative_ae9_cap_irq(struct creative_chip *chip);
extern unsigned int ae9_sentinel_clear;
int creative_dsp_scp_send(struct creative_chip *chip, u32 hdr,
			  const u32 *data, unsigned int count);
void creative_dsp_chipio_addr_invalidate(void);
int creative_dsp_boot_handshake(struct creative_chip *chip);
extern unsigned int ae9_ca0113_windows;

/*
 * debug=0 (default): startup, user actions and problems only. debug=1:
 * also the routine per-stream/per-step detail (command blocks, router,
 * volume steps). debug=2: plus the raw head-unit button state. Writable
 * at runtime: /sys/module/snd_hda_codec_creative_ae9/parameters/debug
 */
extern unsigned int ae9_debug;
extern unsigned int ae9_surround;	/* 5.1 (default on) */
#define ae9_dbg(chip, fmt, ...) do {					\
		if (ae9_debug)						\
			dev_info((chip)->dev, fmt, ##__VA_ARGS__);	\
	} while (0)
void creative_ae9_mic_level_apply(struct creative_chip *chip);
void creative_ae9_fx_init(struct creative_chip *chip);
int creative_ae9_fx_controls_add(struct creative_chip *chip);
void creative_ae9_fx_restore(struct creative_chip *chip);
void creative_ae9_fx_defer(struct creative_chip *chip);
void creative_ae9_fx_sbx_toggle(struct creative_chip *chip);
void creative_ae9_fx_spk_follow(struct creative_chip *chip, unsigned int channels);
void creative_ae9_dsp_switch_kick(struct creative_chip *chip);
void creative_ae9_dsp_mode(struct creative_chip *chip, bool dsp);
void creative_ae9_dsp_mode_start(struct creative_chip *chip, u16 format);
void creative_dsp_ca0113_replay(struct creative_chip *chip, const u8 (*cmds)[3],
				size_t n, const char *what);
void creative_dsp_ca0113_replay_quiet(struct creative_chip *chip,
				      const u8 (*cmds)[3], size_t n);

/* ------------------------------------------------------------------ */
/* MMIO accessor helpers (with mandated memory barriers)		  */
/* ------------------------------------------------------------------ */

static inline void creative_hda_writel(struct creative_chip *chip,
				       unsigned int reg, u32 val)
{
	writel(val, chip->hda_regs + reg);
	wmb(); /* docs/history/Creative.md §2.2: barrier after every write */
}

static inline u32 creative_hda_readl(struct creative_chip *chip,
				     unsigned int reg)
{
	u32 val = readl(chip->hda_regs + reg);
	rmb(); /* barrier after read for status polling */
	return val;
}

static inline void creative_ext_writel(struct creative_chip *chip,
				       unsigned int reg, u32 val)
{
	writel(val, chip->ext_regs + reg);
	wmb(); /* docs/history/Creative.md §2.2: barrier after every write */
}

static inline u32 creative_ext_readl(struct creative_chip *chip,
				     unsigned int reg)
{
	u32 val = readl(chip->ext_regs + reg);
	rmb(); /* barrier after read for status polling */
	return val;
}

static inline void creative_ext_writeb(struct creative_chip *chip,
				       unsigned int reg, u8 val)
{
	writeb(val, chip->ext_regs + reg);
	wmb();
}

static inline u16 creative_ext_readw(struct creative_chip *chip,
				     unsigned int reg)
{
	u16 val = readw(chip->ext_regs + reg);
	rmb();
	return val;
}

static inline void creative_ext_writew(struct creative_chip *chip,
				       unsigned int reg, u16 val)
{
	writew(val, chip->ext_regs + reg);
	wmb();
}

static inline void creative_hda_writeb(struct creative_chip *chip,
				       unsigned int reg, u8 val)
{
	writeb(val, chip->hda_regs + reg);
	wmb();
}

static inline u8 creative_hda_readb(struct creative_chip *chip,
				    unsigned int reg)
{
	u8 val = readb(chip->hda_regs + reg);
	rmb();
	return val;
}

static inline void creative_hda_writew(struct creative_chip *chip,
				       unsigned int reg, u16 val)
{
	writew(val, chip->hda_regs + reg);
	wmb();
}

static inline u16 creative_hda_readw(struct creative_chip *chip,
				     unsigned int reg)
{
	u16 val = readw(chip->hda_regs + reg);
	rmb();
	return val;
}

#endif /* __CREATIVE_AE9_H */
