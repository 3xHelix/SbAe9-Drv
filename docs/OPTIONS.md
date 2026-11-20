# Module options

The driver follows Windows' cold boot with no options needed; everything here
is tuning and diagnostics. Back to the [README](../README.md).

Set an option at load in `/etc/modprobe.d/` (the source tree has a commented
example, `config/ae9-options.conf`):

```
options snd-hda-codec-creative-ae9 hu_auto_switch=1
```

Options marked "writable at runtime" can also be changed while the driver
runs, for example:
`echo 1 | sudo tee /sys/module/snd_hda_codec_creative_ae9/parameters/debug`.
The others are read at load only. `modinfo -p snd-hda-codec-creative-ae9`
lists every option with a one-line description.

The options fall into three groups: settings you may want, overrides for the
card's PCIe bridge, and debugging. (Sound Blaster Command reads `pcm_rates`,
`pcm_bits` and `hp_custom`, listed in their groups.)

## Settings you may want

### Playback

- `pcm_rates` (2, writable at runtime): playback rates: 2 = 44.1-384 kHz,
  1 = 48/96/192/384 kHz (no 44.1/88.2/176.4 kHz; PipeWire resamples
  those), 0 = 48 kHz only. Applies to streams opened afterwards.
- `pcm_bits` (24, writable at runtime; also the "PCM Bit Depth" control):
  sample bits the card takes the stream as, 24 or 32 (all 32 bits, a format
  Windows also uses). ALSA sees S32_LE either way. Applies to streams opened
  afterwards.
- `pcm_periods` (8, writable at runtime, applied at the next stream): most
  periods per playback buffer, 2-32. 2 is Windows' 48 kHz layout, but with
  2 an app asking for a big quantum could fill the whole buffer.
- `dac_start_skip` (1, writable at runtime): sends the DAC's start commands
  only when the rate or output changed (each send makes it re-lock, a brief
  hiccup); 0 sends them at every start, as Windows.
- `surround` (1, writable at runtime, applied at the next stream): 6-channel
  (5.1) playback and the 4.0/4.1/5.1 speaker configs; 0 = stereo only.
- `surround_ports` (0x50,0x51,0x60,0x61, writable at runtime, applied at the
  next stream start): the router targets for the DSP's center, LFE, rear
  L/R outputs in DSP mode (stereo streams too since 0.12.11: Bass Redirect's
  sub, the rears of 4.0/4.1); each must be a DAC pair's first port
  (0x40-0x7f with bits 3:1 clear), else the default is used.
- `pcm_dsd` (1, writable at runtime, applied at the next stream open):
  native DSD64 on playback device 0, the way Creative's Windows DSD ASIO
  driver plays it (the CA0113's own I2S to the front DAC in DSD mode); on by
  default since 0.12.15 (opt-in in 0.12.14; card-tested with aplay and
  MPD). The device also offers `DSD_U32_BE`, 2 channels at rate 88200
  (ALSA's DSD64 rate for 32-bit words), in Direct Mode only (with
  `pcm_rates=2`) and read/write access only (no mmap: each frame is
  repacked for the card). Plays on the card's RCA out and the head unit's
  headphones. Direct Mode off, an output switch, suspend and unload stop a
  DSD stream (the player restarts it, or gets an error in DSP mode). PCM
  playback is the same either way. 0 = off: `DSD_U32_BE` isn't offered.
  `tools/dsd/dsf2raw.py` turns a `.dsf` file into raw data for `aplay`. How
  to use it: [USING.md](USING.md#native-dsd64).

### Direct Mode, SBX and headphone profiles

- `hp_custom` (empty, writable at runtime): the four custom slots of
  "Headphone Profile", `KEY=Name;KEY=Name;...` (slot N loads
  `creative-ae9/hp/<KEY>.bin` and shows Name; an empty slot shows "Custom
  N"). `ae9-hp-profile` writes it, in `/etc/modprobe.d/creative-ae9-hp.conf`
  and in the running driver.

### Recording and optical

- `cap_bits` (24, writable at runtime; also the "Capture Bit Depth"
  control): 24- or 32-bit recording samples, taken at the next recording
  start.
- `mic_boost_30` (0): 1 = Mic Gain also goes to +30 dB, the mic preamp's top
  step (Windows stops at +20 dB). It adds a clean 10 dB, but normal speech
  into a typical mic clips there: it's for quiet mics.
- `spdif_out` (1, set at load; at runtime use the "Optical Out Playback
  Switch" control, which this option mirrors): optical out on; 0 keeps it
  silent.
- `optical_mirror` (0, set at load; at runtime use the "Optical Out Mirror
  Switch" control, which this option mirrors): 1 = optical plays the main output while nothing plays to optical
  out (device 4), as 0.9-0.12.0 did.

### Head unit

The driver replays Windows' post-boot head-unit setup and runs Windows'
head-unit poll, volume display and knob.

- `hu_auto_switch` (0): start with "Head Unit Auto Switch" on: plugging into
  the head unit's 3.5 mm headphone jack switches to headphones, unplugging
  back to speakers. Off, plugs only log.

## For the card's PCIe bridge

Automatic choices that depend on the PCIe bridge in front of the card (an
ASMedia ASM1083 on some boards). Leave them at auto; they're here so a card
that misbehaves behind some other bridge can be tried the other way. If you
need one, please report it.

- `posted_flush` (-1 = auto): reads back after every register write, as
  Windows does; an ASMedia bridge wedges after a burst of writes with no
  read between them. -1 = on behind an ASMedia bridge only, 0 = never,
  1 = always.
- `dma_pad` (-1 = auto): spare KB after every DMA buffer, for the ASMedia
  bridge's read-ahead past a buffer's end (IOMMU faults). -1 = 64 behind an
  ASMedia bridge, else 0; a number sets it for every card.
- `ring_order` (-1 = auto): order the command rings are started in. 1 =
  Windows' and the HD Audio spec's order (addresses and sizes set while the
  rings are stopped); 0 = the driver's older order. -1 = 1 behind an ASMedia
  bridge, 0 otherwise.
- `bus_reset` (-1 = auto): secondary bus reset of the card at load (codec
  subsystem IDs only read 0x11020071/72 after one; a VM start does the
  same). -1 and 1 = on, on every card, also behind an ASMedia bridge (since
  0.12.5: without it a reload there starts on stale card state and stays
  silent); it's skipped anyway when other devices share the card's bus.
  0 = off.

## For debugging and tests

- `debug` (0, writable at runtime): log detail. 0: startup, your actions
  (Direct Mode/SBX, output switches, mutes) and problems. 1: also the routine
  per-stream and per-step lines (command blocks, router, volume steps; each
  stream's buffer and BDL addresses when it's prepared, for matching an
  `IO_PAGE_FAULT` address to a stream). 2: plus the raw head-unit button
  state.
- `verb_log` (0): keep the first this many card commands and replies after
  load (the log stops when full) for debugfs
  `creative_ae9/verblog` (in the capture log's format, for diffing against
  Windows captures and for `tools/verbdiff`); 0 = off.
- `mmio_log` (0): 1 = also log the BAR0/BAR2 register accesses in the
  verblog (needs `verb_log`).
- `acm_heartbeat_ms` (500): the head-unit heartbeat (Windows' cadence; it
  reads the knob, buttons, plugs and status, and drives the volume display
  and every volume/mute change); 0 = off, which also stops the knob and leaves volume
  and mute changes unsent: only for `tools/verbdiff`, which loads with 0 so
  its verb logs stay deterministic. Leave it at 500 otherwise.

### Test hooks (test builds only)

These exist only in a test build of the driver: `make AE9_TEST=1` in the
source tree (or `AE9_TEST=1` added to a kbuild `make ... M=<dir> modules`).
A release build (DKMS, the packages, a plain `make`) has neither the options
nor their code (since 0.12.15). Each acts only with `debug` at 1 or more (so
a stray setting can't break a normal start) and is writable at runtime.

- `dsp_fail_inject` (0, clears itself): fails the next DSP load once (1 =
  download, 2 = handshake) to exercise the soft retry (Windows' DSP_INIT +
  0.5 s, up to 3 downloads per bring-up).
- `hung_inject` (0, counts down): the next N commands after start-up count
  as timed out without being sent, to exercise the "card stopped
  answering" stop.
- `scp_full_inject` (0 = off): every Nth DSP word is treated as refused
  ("queue full") once, to exercise the resend.
- `rirb_inject` (0 = off): after start-up every Nth reply is hidden for
  `rirb_inject_us` and given up on, then put back, as a reply landing far
  too late: the late-reply watch must find it.
- `rirb_inject_us` (300): how long `rirb_inject` hides a reply, in µs; over
  100000 its command times out. A hidden reply is dropped, as never landed,
  once the reply ring has moved 128 entries past it.

## Removed in 0.12.15

Fixed at their defaults now. An old line naming one is ignored, with an
"unknown parameter" line in the kernel log; the driver still loads. Remove
it at your convenience.

- `hu_master`: the head-unit volume and mute are always "Master Playback
  Volume" / "Master Playback Switch", so the desktop slider and the knob
  move together.
- `eq_headroom`: always on. With a headphone profile and the EQ both on, the
  DSP gets the EQ pre-gain 12 dB lower (not below -12 dB); without it a hot
  signal overloads the DSP and it drops audio blocks: the "metallic" sound.
  The control keeps your value; only what the DSP gets changes.
- `fx_spk_cfg`: "Speaker Config" starts at 2.1, as Windows; the control
  still changes it.
- `dma_trace`: `debug=1` now logs each stream's buffer and BDL addresses
  when it's prepared.

Test builds only now (`make AE9_TEST=1`, above): `dsp_fail_inject`,
`hung_inject`, `scp_full_inject`, `rirb_inject`, `rirb_inject_us`. In a
release build a line naming one is ignored the same way.

## Removed in 0.12.14

Always on now, as they were by default. Sound Blaster Command v0.21.3 and
later tells the rate features by the driver version; `router_poll` and
`rt_dsp_walk` were fallbacks to the behaviour before 0.12.11's fixes. An old line naming one is
ignored, with an "unknown parameter" line in the kernel log.

- `surround_96k`, `surround_192k`: 5.1 at 96 and 192 kHz in Direct Mode.
- `dsp_96k`: 96 kHz with Direct Mode off, through the DSP.
- `pcm_176k4`: 176.4 kHz with `pcm_rates=2`.
- `dac_441`: Windows' DAC commands for 44.1/88.2 kHz (48/01 = 0xc1,
  48/0b = 0x20).
- `hp_live_upload`: headphone profiles load while audio plays, as Windows
  (0.12.10); the old wait for every stream to stop is gone.
- `router_poll`: the router chip's busy status is waited for before each of
  its setup commands, and a refused one is sent again, as Windows (0.12.11).
- `wuh_96k`: What U Hear records at 48 or 96 kHz (0.9.5). Sound Blaster
  Command v0.21.4 and later tells it by the driver version.
- `rt_dsp_walk`: DSP mode routes all twelve 96 kHz lanes in the router
  chip's own slots for them (stream 0x18), as Windows (0.12.11); the fixed
  slots stay only as the fallback when that list can't be used.

## Removed in 0.12.12

These options are gone; an old line in `/etc/modprobe.d` naming one is
ignored, with an "unknown parameter" line in the kernel log. Remove it at
your convenience.

- Bring-up diagnostics: `stream_rings_off`, `fw_rings_off`, `fw_posbuf`,
  `fw_verify`, `corb_rings`, `ring_irq`, `enable_pcm`, `load_fw`,
  `bit9_test`, `debug_block_log`, `pcm_posbuf`, `pcm_nowakeup`, `hu_debug`
  (`debug=2` logs the raw head-unit button state), `vc_quiet_ms`,
  `bridge_log`.
- "As before" fallbacks, now always as their defaults were: `win_reset2`,
  `warm_reset`, `board_d0`, `codec1_failfast`, `pcicfg40_fix`, `overlay`,
  `scp_event`, `scp_full_resend`, `unmute_check`, `pcm_unmute`,
  `pcm_stop_block`, `rt_chain_walk`, `rt_dac_clean`, `stream_drain`,
  `fmt_check`, `src_reclock`, `hu_sbx_mode`, `hu_reattach`,
  `dsp_soft_tries` (always 3), `sentinel_clear` (never), `in05_host`
  (never).
- Timing tunables, now fixed at their old defaults: `hu_fast_ms` (40 ms knob
  poll), `hu_cmd_settle_us` (4 ms DAC settle for volume changes),
  `codec_probe_delay_ms` (200 ms), `resume_tries` (3 bring-up attempts),
  `resume_delay_ms` (2 s between them), `dsp_switch_ms` (a fresh stream
  plays direct for 3 s before it moves to the DSP).
