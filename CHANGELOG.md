# Changelog

Driver for the Creative Sound Blaster AE-9 (out of tree). Pre-releases,
tested on one card (IDT bridge, `10e3:8111`).

## 0.6.0 (2026-11-04)

**SBX works on Linux.** Surround, Crystalizer, Dialog Plus, Smart Volume,
Bass, the EQ: through the card's own DSP, toggled from alsamixer, and you
can *hear* it.

I'm not going to pretend this one was clean. It took one long night of
"turn Direct Mode off... silence. Turn it back on... sound. Off again...
crunchy garbage that sounds like a USB card dying under load." The logs
said everything was fine. Every command: 0 errors. The card was lying to
us, politely.

So we stopped trusting the logs and started dumping the card's audio
router, all 64 entries of it, after every single click. Something like
thirty dumps later, it confessed:

- We were turning on the DSP's input stream and **never turning it off**.
  The card's little 8051 brain kept that path reserved forever, and the
  next time we asked for DSP mode it built a second path on spare
  hardware that never started. Silence. Toggle more, it gets worse.
  Reboot? Still broken, because the card doesn't forget on a warm reboot.
  Only pulling power fixed it. (I got very good at flipping the PSU
  switch.)
- Windows **waits** after it stops a stream: 48 ms here, 144 ms there.
  We didn't. Sometimes the chip kept up, sometimes it didn't, which is why
  the exact same steps worked on one boot and not the next.

Both fixed. Then we broke it again on purpose: round trips, sleep and wake,
headphones, booting straight into DSP mode, a warm reboot in the middle of
a Discord call with the mic open. It held.

### What you get

- **Direct Mode** ("Direct Mode Switch", on by default): same as before,
  straight to the DAC at the stream's rate, up to 384 kHz, DSP bypassed.
  Nothing changes unless you turn it off.
- Turn it **off** and the audio goes through the DSP at 96 kHz (stereo
  for now). Then the **SBX** button ("SBX Switch") turns the effects on
  and off.
- With both: Surround, Dialog Plus, Smart Volume (and its mode),
  Crystalizer, Bass (switch + level 0-100 each), EQ on/off with Flat,
  Rock and Gaming presets, pre-gain and ten bands, and Scout Mode.
  Otherwise those are greyed out and keep their values. Two-stage, like
  Windows.
- Switch Direct Mode with music playing, both ways. After boot, wake, or a
  switch made while nothing plays, the first stream plays direct for 3 s
  and then moves to the DSP (`dsp_switch_ms`). Turning Direct Mode back on
  goes quiet until you pause/play, same as Windows does.
- Speakers and headphones, sleep and wake, reboots. Before a reboot,
  power-off or sleep the driver now stops the DSP streams and any
  recording it started, so the card comes back clean.
- The six startup messages the DSP always rejected aren't sent any more
  (`tail_drop_rejected`), so those error lines are gone from the log.

### Known limits

- DSP mode is stereo only; 5.1 isn't wired up yet.
- Direct Mode on is silent until the stream restarts (pause/play).
- Three EQ presets so far; Scout Mode and 96 kHz streams in DSP mode
  haven't had a proper listen yet.
- If DSP mode ever goes silent or crunchy and toggling doesn't fix it,
  power off at the PSU. Haven't seen it since these fixes, but now you
  know.

Tested on kernel 7.2.14, Fedora 44 KDE, one card: everything above, plus
Direct Mode as in 0.5.0 (48/96/192 kHz, recording, sleep).

Upgrading from 0.5.0: replace the module, `sudo depmod -a`, reboot.

If you've got an AE-9 and Linux, try it and tell me what breaks. -- Joe

## 0.5.0 (2026-11-03)

Two new controls, both done the way the Windows driver does them.

- **Mic level** ("Capture" in alsamixer): -89 to +9 dB in 1 dB steps, mute
  at the bottom; default +9 dB, the level the mic had before. PipeWire uses
  it as the mic's hardware volume, so the desktop microphone slider now
  moves the card's own level. "XLR Gain" (0/+10/+20 dB preamp) stays a
  separate control.
- **DAC filter** ("DAC Filter"): Default, Fast or Slow roll-off for the
  headphone/line-out DAC, as in the Creative app. Default leaves the
  startup filter. Kept across sleep and wake.
- Tested on kernel 7.2.14: recording at different mic levels, filter
  switching during playback, sleep and wake.

Upgrading from 0.4.0: replace the module, `sudo depmod -a`, reboot. Note
that your desktop mic slider now sets the card's hardware level (100% =
+9 dB, as before).

## 0.4.0 (2026-11-03)

Higher rates and 32-bit, set up exactly as the Windows driver does it.

- **384 kHz playback** (with `pcm_rates=1`). Windows sends it over the card's
  link as 192 kHz with two samples per frame; the driver does the same.
- **True 32-bit playback** with `pcm_bits=32` (default stays 24-bit, as
  Windows). All 32 sample bits reach the card.
- **Fixed: 192 kHz was slightly quieter** than 96 kHz. The card needs more
  internal routing set up at higher rates (4 routes at 96, 8 at 192, 16 at
  384); 0.3.0 only set the 48 kHz pair.
- Rate changes now follow a new Windows capture step for step; steps that
  came from guesswork (an extra clock register, a DSP rate and a PLL
  reprogram) are gone.
- 44.1 and 88.2 kHz are still not offered: Windows routes them through a
  sample-rate converter in the card, which doesn't play right on Linux yet
  (`pcm_rates=2` turns them on for testing only). PipeWire resamples those
  files, so they play fine.
- In PipeWire: `pw-metadata -n settings 0 clock.allowed-rates '[ 48000 96000 192000 384000 ]'`.
- Tested on kernel 7.2.14: same pitch and level at 48/96/192/384 kHz in 24-
  and 32-bit, PipeWire following file rates, playback through VLC.

Upgrading from 0.3.0: replace the module, `sudo depmod -a`, reboot. Your
`pcm_rates=1` keeps working and now includes 384 kHz.

## 0.3.0 (2026-11-03)

High-resolution playback: 96 and 192 kHz.

- **New, opt-in:** playback at 96 kHz and 192 kHz (24-bit), alongside 48 kHz.
  Turn it on with `pcm_rates=1` (in `config/ae9-options.conf`, or at
  runtime through `/sys/module/snd_hda_codec_creative_ae9/parameters/pcm_rates`),
  then let PipeWire use the rates, e.g.
  `pw-metadata -n settings 0 clock.allowed-rates '[ 48000 96000 192000 ]'`
  or `clock.force-rate 96000`. See README -> Using it.
- The card is re-clocked for each stream the way Windows does it (rate
  codes, I2S clock settings, full PLL program); going back to 48 kHz
  restores the startup clock setup.
- 44.1 and 88.2 kHz are not offered yet: they play sharp on this card, so
  Windows does something more for that clock family. Leave them out of
  PipeWire's allowed rates and it resamples those files (they play fine).
- Recording stays at 48 kHz; it works at the same time as 96 kHz playback.
- With the option off (the default) nothing changes from 0.2.1.
- Tested on kernel 7.2.14: pitch at 48/96/192 kHz, PipeWire at 96 kHz,
  mic during 96 kHz playback, output switching, sleep and wake.

Upgrading from 0.2.x: replace the module, `sudo depmod -a`, reboot, then
turn on `pcm_rates` if you want the higher rates.

## 0.2.1 (2026-11-03)

Small update: better logging, and fewer slow starts.

- Fixed: a needless firmware retry on about half of startups and resumes.
  The DSP briefly reports a firmware chunk as finished while it is still
  loading; the driver now double-checks before moving on, so startup no
  longer stalls 2-4 s for a retry.
- New log line after every startup: `DSP health: ok`, or a warning when the
  DSP did not come up right (with a hint to power off at the PSU if
  recording is dead).
- New log lines when a plug goes in or out of the head unit's 3.5 mm
  headphone or 3.5 mm mic jack. Logging only: the driver never switches
  between speakers and headphones by itself. (The 6.3 mm headphone jack and
  the XLR/TRS combo jack are not reported by the head unit.)
- Groundwork for sample rates other than 48 kHz (the stream format is now
  built for 8-192 kHz). Playback and recording still run at 48 kHz only.
- README: `iommu=pt` is not needed (tested on AMD with the IOMMU on).
- Tested on kernel 7.2.14 (Fedora 44): boot, sleep and wake on speakers and
  headphones, playback, recording, output switching.

Upgrading from 0.2.0: replace the module, `sudo depmod -a`, reboot.

## 0.2.0 (2026-11-03)

Stability release: every item from a full code review, plus fixes found
while testing them.

- Fixed: noisy, clipped recording after resume. A firmware chunk that never
  finished loading went unnoticed and left the DSP half-loaded; the load now
  fails and is retried.
- Fixed: CA0113 (head-unit DAC) command race.
- CORB timeout recovery follows the HD-Audio spec; CORB sends refuse while
  the rings are parked.
- The driver claims only the AE-9 and guards the bus reset.
- Firmware images are validated before use; extraction is MD5-checked.
- Probe/remove clean up on errors and never register a card with a dead DSP.
- Stream control writes no longer clear status bits by accident; the
  interrupt-enable update is locked.
- Head-unit polling sleeps instead of spinning, on the driver's own work queue
  (also silences a kernel 7.x warning).
- Fixed: the codec count grew on every resume.
- **XLR Gain** (was "Mic Boost"): the 0/+10/+20 dB mic preamp is separate from
  the desktop microphone slider, which is now software gain only. Set it with
  `alsamixer -c AE9` (F4) or `amixer -c AE9 sset 'XLR Gain' 0|1|2`.
- `restore-stock-audio.sh` removes everything the README installs.
- Tested on kernels 7.2.13 and 7.2.14 (Fedora 44).

Upgrading from 0.1.0: replace the module, `sudo depmod -a`, reboot. If you
used the desktop mic slider for boost, set it back to 100% and pick a gain
with `XLR Gain`.

## 0.1.0 (2026-11-02)

First pre-release.

- Stereo playback on the RCA outputs (speakers) and the head unit's headphone
  jack; 2 ch, 48 kHz, 24-bit (S32_LE), through ALSA and PipeWire.
- Recording from the head unit's XLR mic input, with 0/+10/+20 dB boost.
- Head unit: display, volume knob, mute, speaker/headphone switch, knob light.
- Suspend/resume, reboot and power-off follow Windows' sequences.
