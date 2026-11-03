# Changelog

Driver for the Creative Sound Blaster AE-9 (out of tree). Pre-releases,
tested on one card (IDT bridge, `10e3:8111`).

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
