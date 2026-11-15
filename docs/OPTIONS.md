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
`modinfo snd-hda-codec-creative-ae9` lists every option with a one-line
description.

## Logging and diagnostics

- `debug` (0, writable at runtime): log detail. 0: startup, your actions
  (Direct Mode/SBX, output switches, mutes) and problems. 1: also the routine
  per-stream and per-step lines (command blocks, router, volume steps). 2:
  plus the raw head-unit button state.
- `codec1_failfast` (1, writable at runtime): stop a bring-up right after
  codec detection when codec 1 is silent (the dark-card case). 0 runs the
  whole bring-up anyway: 2-3 min of timeouts, for experiments only.
- `fw_verify` (0): read the firmware back after download and count
  mismatches.
- `dma_trace` (0, writable at runtime): log each stream's buffer and
  BDL addresses when it starts, for matching an `IO_PAGE_FAULT` address
  to a stream.
- `bit9_test` (0, load-time): at bring-up, check which codecs answer with
  the card register `0x1c` bit 9 set, then clear (it switches the second
  codec off; the AE-9 needs it clear).

## Start-up

- `enable_pcm` (1), `load_fw` (1): the PCM devices and the DSP firmware.
- `bus_reset` (1): secondary bus reset at load (codec subsystem IDs only read
  0x11020071/72 after one; a VM start does the same). On by default on every
  card, also behind an ASMedia bridge. 0 turns it off.
- `win_reset2`, `scp_event` (both 1): Windows' second controller reset and
  announced DSP replies; kept switchable for cards behind other bridges.
- `sentinel_clear` (0): clear the DSP's 0x0bad0add sentinel area.
- `overlay`: transfer the DSP overlay image during the handshake.
- `warm_reset` (1): Windows' codec recovery cycle before the DSP download
  when a ramp status byte reads failed, as Windows (0 = never, 2 = always).
- `dsp_soft_tries` (3): DSP downloads per bring-up when one fails, with
  Windows' DSP_INIT + 0.5 s between them (1 = no soft retry).
  `dsp_fail_inject` (test only, needs `debug=1`, clears itself) fails the
  next DSP load once (1 = download, 2 = handshake) to exercise that retry.
- `resume_tries` (3) / `resume_delay_ms` (2000): bring-up retries.

## Playback

- `pcm_rates` (2, writable at runtime): playback rates: 2 = 44.1-384 kHz,
  1 = without 44.1/88.2 kHz, 0 = 48 kHz only. `dac_441` (1) sends Windows'
  DAC commands for 44.1/88.2 kHz (without them those rates play garbled or
  silent). `src_reclock` (1) sets the I2S to its 48 kHz clocks before
  44.1/88.2 kHz (without it they went silent after a 384 kHz stream).
  `pcm_176k4` (1) offers 176.4 kHz with them.
- `surround` (1, writable at runtime): 6-channel (5.1) playback and the
  4.0/4.1/5.1 speaker configs; 0 = stereo only. `surround_ports`
  (0x50,0x51,0x60,0x61) the router targets for the DSP's center, LFE, rear
  L/R outputs. `surround_96k` (1, writable at runtime): 5.1 at 96 kHz in
  Direct Mode; 0 limits 5.1 to 48 kHz. `surround_192k` (1, writable at
  runtime): 5.1 at 192 kHz in Direct Mode; 0 limits 5.1 to 96 kHz.
- Playback buffer (all writable at runtime, applied at the next stream):
  `pcm_periods` (8) most periods per buffer, 2-32 (2 is Windows' 48 kHz
  layout; with 2 an app asking for a big quantum could fill the whole
  buffer); `pcm_nowakeup` (0) 1 lets PipeWire turn period interrupts off;
  `pcm_posbuf` (0) 1 logs how far the DMA reads ahead of the reported
  position (see [Low latency](USING.md#low-latency)); `dac_start_skip` (1)
  sends the DAC's start commands only when the rate or output changed (each
  send makes it re-lock, a brief hiccup); 0 sends them at every start, as
  Windows.
- `pcm_stop_block` (1, writable at runtime): send Windows' playback stop
  block at stream release; 0 skips it (experiment for the rare silent restart
  after a pause).
- DSP output unmute (writable at runtime): `unmute_check` (1) read the
  unmute back after effect changes and resend it (up to 10 times), as
  Windows; `pcm_unmute` (1) unmute at every DSP-mode playback start and stop,
  as Windows.

## Direct Mode, SBX and the DSP

- `dsp_96k` (1, writable at runtime): 96 kHz with Direct Mode off, through
  the DSP; 0 limits DSP mode to 48 kHz.
- `fx_spk_cfg` (2): the "Speaker Config" setting at load (1 = 2.0, 2 = 2.1 as
  Windows).
- `dsp_switch_ms` (3000, writable at runtime): how long a fresh stream plays
  direct before the switch to the DSP.
- `hp_live_upload` (1, writable at runtime): load a newly picked headphone
  profile while audio plays, as Windows does; 0 = wait until every
  playback and recording stops (before 0.12.10).
- `hp_custom` (empty, writable at runtime): the four custom slots of
  "Headphone Profile", `KEY=Name;KEY=Name;...` (slot N loads
  `creative-ae9/hp/<KEY>.bin` and shows Name; an empty slot shows "Custom
  N"). `ae9-hp-profile` writes it, in `/etc/modprobe.d/creative-ae9-hp.conf`
  and in the running driver.
- `vc_quiet_ms` (0, writable at runtime): holds DSP messages this long after
  Voice Clarity routes the mic in (not needed since the start follows
  Windows' order).
- Router (kept switchable for diagnosis): `rt_chain_walk` (1, writable at
  runtime) Direct Mode starts use the router slots the router chip gave the
  stream, as Windows (0 = the old fixed slots, which stalled What U Hear);
  `stream_drain` (1, writable at runtime) release the router chip's stream
  counts at shutdown and start-up; `in05_host` (0, load-time) 1 = the old
  host enable of stream 0x05; `rt_dac_clean` (1, writable at runtime) switch
  off leftover DSP-mode routes at Direct Mode on, the first Direct start and
  shutdown (left on, they made the analog out gated/metallic until a
  power-off).

## Recording and optical

- `cap_bits` (24, writable at runtime; also the "Capture Bit Depth"
  control): 24- or 32-bit recording samples.
- `mic_boost_30` (0, load-time): 1 = Mic Gain also goes to +30 dB, the mic
  preamp's top step (Windows stops at +20 dB). It adds a clean 10 dB, but
  normal speech into a typical mic clips there: it's for quiet mics.
- `wuh_96k` (1, writable at runtime): What U Hear at 48 or 96 kHz; 0 = 48 kHz
  only.
- `spdif_out` (1, writable at runtime; also the "Optical Out Playback
  Switch"): optical out on; 0 keeps it silent.
- `optical_mirror` (0, writable at runtime; also the "Optical Out Mirror
  Switch"): 1 = optical plays the main output while nothing plays to optical
  out (device 4).

## Head unit

The driver replays Windows' post-boot head-unit setup and runs Windows'
head-unit poll, volume display and knob.

- `hu_master` (1): names the volume/mute "Master" for desktop sync (0 =
  "Head Unit Playback Volume" / "Head Unit Playback Switch").
- `hu_auto_switch` (0): start with "Head Unit Auto Switch" on.
- `hu_reattach` (1, writable at runtime): set the head unit up again when it
  loses its setup while running (its "set up" marker reads as freshly
  powered), as Windows; 0 = leave it.
- `hu_fast_ms` (40): knob poll interval. `hu_cmd_settle_us` (4000, writable
  at runtime): DAC settle time for volume changes. `acm_heartbeat_ms` (500):
  head-unit heartbeat (0 = off).
