# Creative Sound Blaster AE-9 — Linux Driver (WIP)

An out-of-tree Linux kernel driver for the **Creative Sound Blaster AE-9**
(CA0132 "Sound Core3D" DSP + detachable ACM head unit), reverse-engineered from
a Windows VFIO MMIO capture. The goal is functionality the in-tree
`snd_hda_intel` + `snd_hda_codec_ca0132` stack does not provide for this board:
card LEDs, the ACM head unit (OLED display + volume knob + buttons), and PCM
playback.

> **Status (2026-11-06, v0.9.6):** stereo playback works on the card's RCA outputs
> (speaker mode), clean on both channels, through ALSA and PipeWire.
> Recording from the head unit's XLR or 3.5 mm input works, at 48 or 96 kHz, 24- or 32-bit (arecord, Audacity).
> Optical S/PDIF works both ways: out mirrors the main output (stereo, up
> to 24-bit/192 kHz), in is its own recording device. What U Hear records
> what the card plays.
> The ACM head unit shows the volume; its knob sets the volume, a press
> mutes, a medium hold switches between the card's RCA outputs and the
> head unit's headphone jacks, and a long hold toggles the knob light. The SBX button
> turns SBX on and off (with Direct Mode off) and its light shows when the
> effects are running.
> The driver gets there by doing what the Windows driver does in a cold-boot
> VFIO capture; that setup is the built-in default, no module options needed. Playback at
> 44.1, 48, 88.2, 96, 192 and 384 kHz, 24- or 32-bit ("PCM Bit Depth"), works out of
> the box in Direct Mode. SBX effects and the EQ work through the card's
> DSP, with Direct Mode off. 5.1 works on the card's RCA, Rear and
> C/Sub jacks, at 48, 96 or 192 kHz in Direct Mode and 48 or 96 kHz through
> the DSP.
> This repository holds only what you need to build and run the driver.
> Changes per release: **[CHANGELOG.md](CHANGELOG.md)**.

> **Control panel:** [SbAe9-SBC](https://github.com/3xHelix/SbAe9-SBC) is a
> Sound Blaster Command-style desktop app for this driver: SBX, the EQ and
> its presets, Scout Mode, Direct Mode, outputs, recording and the mixer,
> all in one window. It's a solid stand-in for the Windows software that
> ships with the card.
> Driver v0.9.6 pairs with SbAe9-SBC v0.11.0 (phantom power button, What U
> Hear rate picker, rates per mode); older SbC versions still work, with
> fewer controls.

## Hardware / Environment

- Creative Sound Blaster AE-9 — PCI `1102:0010`, subsystem `0x0071`
  (CA0132 Sound Core3D, behind an IDT Tsi381 PCIe-to-PCI bridge), with the
  ACM head unit connected.
- Developed/tested on Fedora 44 KDE, kernels `7.2.13`, `7.2.14` and `7.2.15`
  (`-200.fc44.x86_64`).
- No `iommu=pt` needed: tested on an AMD system with the IOMMU in its
  default translated mode, with no IOMMU page faults from the card. (Drivers
  built on the kernel's ca0132 code report needing it; this one doesn't.)
- Only tested on the IDT-bridge version of the card. AE-9s exist with an
  IDT/Tundra Tsi381 (`10e3:8111`) or an ASMedia ASM1083/1085 (`1b21:1080`)
  PCIe-to-PCI bridge. To see which one sits above your card:
  ```sh
  lspci -nn -s $(basename $(dirname $(readlink -f /sys/bus/pci/devices/$(lspci -D -d 1102:0010 | cut -d' ' -f1))))
  ```
  (On Windows: Device Manager, View, Devices by connection; the PCI bridge
  above the AE-9 shows `VEN_10E3&DEV_8111` or `VEN_1B21&DEV_1080`.)

## Setup

### 1. Firmware

The DSP firmware isn't in the repo; extract it from Creative's Windows driver.
Get the "Sound Blaster AE-Series Driver" package from Creative, unpack it, and
find `AESeriesDriver/Driver/AMD64/CtxHda.sys`. Then:

```sh
python3 extract_firmware.py /path/to/CtxHda.sys ctefx-desktop.bin   # main DSP image
python3 extract_overlay.py  /path/to/CtxHda.sys ctefx-ae9-overlay.bin
sudo cp ctefx-desktop.bin ctefx-ae9-overlay.bin /lib/firmware/
```

Both scripts check the extracted image against the MD5 of the firmware this
driver is tested with (`85c8f2b4…` main image, `0e222c94…` overlay) and
refuse a different one unless you pass `--force`.

### 2. Build and install

**With DKMS (recommended):** new kernels get the driver rebuilt
automatically, nothing to redo after a kernel update.

```sh
sudo dnf install dkms kernel-devel     # once
sudo scripts/dkms-install.sh
```

Run the script again after updating the driver source (it replaces the
old version). `sudo scripts/dkms-install.sh --remove` takes it out of
DKMS. `dkms status` shows what's built for which kernel.

**By hand:**

```sh
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
sudo cp snd-hda-codec-creative-ae9.ko /lib/modules/$(uname -r)/extra/ && sudo depmod -a
```

Redo both after every code change and kernel update.

**One install method per machine.** By hand, DKMS and a packaged build
(akmod/kmod) each put their own copy of the module under `/lib/modules`;
with two of them it's unclear which one loads, and updating one can leave
the other's old copy in charge. To switch:

- by hand to DKMS: just run `sudo scripts/dkms-install.sh` (it deletes the
  hand-copied module first);
- DKMS to by hand: `sudo scripts/dkms-install.sh --remove`, then copy;
- to or from a package: remove the other method first (the DKMS script
  refuses to run next to an installed `akmod-`/`kmod-` package).

`scripts/dkms-install.sh --check` (no root needed) shows what's installed
for the running kernel, every copy of the module, and which one loads.

### 3. Keep `snd_hda_intel` off the card

`snd_hda_intel` would grab the card first, and it can't be blacklisted on
most systems because it also drives GPU HDMI audio. The files in `config/`
hand this one card to our driver instead:

```sh
sudo cp config/00-ae9-override.rules /etc/udev/rules.d/   # driver_override at device add
sudo cp config/ae9-override.conf /etc/modprobe.d/         # same, if snd_hda_intel loads first
sudo cp config/ae9-omit.conf /etc/dracut.conf.d/          # keep our module out of the initramfs
sudo cp config/ae9-options.conf /etc/modprobe.d/          # optional: debug/tuning options (all commented out)
sudo dracut -f
```

`ae9-override.conf` hardcodes the card's PCI address (`0000:06:00.0`); check
yours with `lspci -d 1102:0010` and edit it. The module must load from the
root filesystem (after `/lib/firmware` is available), which is what the
dracut omit is for.

### 4. PipeWire profiles (stereo and 5.1)

Without this PipeWire only gives the card its stereo fallback profiles.
The profile set adds "Analog Stereo Duplex" (the default) and "Analog
Surround 5.1 Output":

```sh
sudo cp config/creative-ae9.conf /usr/share/alsa-card-profile/mixer/profile-sets/
sudo cp config/89-ae9-pipewire.rules /etc/udev/rules.d/
```

`scripts/restore-stock-audio.sh` removes the driver and restores stock
`snd_hda_intel` (and removes these too).

Reboot after setup. A full power-off (PSU off) is the safest first start.

## Using it

- Prefer a GUI? [SbAe9-SBC](https://github.com/3xHelix/SbAe9-SBC) gives
  every control below a Sound Blaster Command-style window (Python 3 +
  PySide6), and follows the knob and alsamixer live.
- Output: the card's RCA jacks (speaker mode, "-SP-") or the head unit's
  headphone jacks, 3.5 mm and 1/4" ("-HP-"). Starts on speakers; switch with a 1.5-3 s
  knob hold or the "Head Unit Output" control. Each output remembers its own
  volume (both start at -36 dB).
- Knob: turn = volume, press (< 1 s) = mute, hold 1.5-3 s = speakers /
  headphones, hold 3 s or more = knob light on/off.
- Plugs: "Head Unit 3.5mm Phones Plugged" (the head unit's 3.5 mm
  headphone jack) and "Head Unit 3.5mm Mic Plugged" (its 3.5 mm mic jack)
  show what's plugged in, read-only, for apps to show. The 1/4" headphone
  jack and the XLR/TRS jack don't report plugs.
- Phantom power: "Head Unit Phantom Power" shows and switches the head
  unit's 48 V phantom power for the XLR/TRS jack (its own button works too).
  It always starts off, and switching it on is refused for 30 s after the
  card starts or wakes, so a restored mixer state can't turn 48 V on by
  itself. Only turn it on for mics that need it: 48 V can damage some gear
  (ribbon mics, some line-level sources).
- Auto switch: "Head Unit Auto Switch" (off by default) makes the head unit's
  3.5 mm headphone jack switch outputs: plug in = headphones, unplug = speakers,
  about 1.5 s after the plug settles. Off, plugs change nothing. The
  1/4" headphone jack isn't reported, so it can't auto switch. To start with it
  on: options snd-hda-codec-creative-ae9 hu_auto_switch=1
- SBX button (head unit): turns SBX on/off, same as "SBX Switch", only
  with Direct Mode off (with Direct Mode on it does nothing). Its light is
  on while the effects are running: Direct Mode off and SBX on.
- In PipeWire / KDE use the "Stereo Output + Stereo Input" profile: the
  desktop volume slider and mute drive the card (hardware volume), and the
  knob moves the slider and OSD. "Pro Audio" also plays, but uses software
  volume only.
- The PCM takes 2 or 6 channels, S32_LE. "PCM Bit Depth" (24-bit, the
  default as Windows, or 32-bit) sets how the card takes it; it applies to
  the next stream (24-bit sources lose nothing at 32-bit). Rates: 44.1, 48,
  88.2, 96, 192 and 384 kHz in Direct Mode (44.1/88.2 kHz stereo only),
  48 or 96 kHz with Direct Mode off, through the DSP (v0.9.4; PipeWire
  resamples other rates). Recording is 48 or 96 kHz.
- Choosing a rate: PipeWire runs at 48 kHz unless told otherwise.
  `pw-metadata -n settings 0 clock.allowed-rates '[ 44100 48000 88200 96000 192000 384000 ]'`
  follows each file's rate, `clock.force-rate 192000` forces one (0 =
  back to automatic). SbAe9-SBC does this from its rate picker. For
  small quantums, see "Low latency" below. 44.1/88.2 kHz (v0.9.4) play
  through the card's sample-rate converter; leave 44100/88200 out and
  PipeWire resamples those files to 48 kHz instead. `pcm_rates=1` drops
  44.1/88.2 kHz from the card's list, `pcm_rates=0` limits it to 48 kHz.
- Recording: the head unit's input, at 48 or 96 kHz (`arecord -D hw:AE9 -c 2
  -r 96000 -f S32_LE`, or the AE-9 input in PipeWire/Audacity). The head unit's
  XLR/TRS jack and its 3.5 mm mic jack share it (Windows lists them as
  Microphone and Aux; the mic's own mute switch affects both). "Capture Bit
  Depth" picks 24-bit (default) or 32-bit samples, as Windows' recording
  format choice (`amixer -c AE9 cset name='Capture Bit Depth' 32-bit`; it
  takes effect at the next recording start). For a clearer
  desktop name, copy `config/61-ae9-names.conf` to
  `~/.config/wireplumber/wireplumber.conf.d/` and run
  `systemctl --user restart wireplumber`: the source shows as "AE-9 Head Unit
  Mic / Aux" (node names stay the same). "Mic Gain" (was "XLR Gain") sets
  the mic preamp to 0/+10/+20 dB (default +10, like Windows); set it with
  `alsamixer -c AE9` (F4) or `amixer -c AE9 sset 'Mic Gain' 0|1|2`. "Capture"
  is the mic level, -89 to +9 dB in 1 dB steps with mute at 0 (default +9 dB,
  what Windows' recording setup uses), set the way Windows' Recording ->
  Levels slider does it; PipeWire uses it as the mic's hardware volume, so
  the desktop microphone slider moves it (100% = +9 dB). Mic Gain stays
  separate. Phantom power is the 48V button on the head unit's front panel;
  the driver doesn't need to do anything.
- DAC filter: `amixer -c AE9 sset 'DAC Filter' Default|Fast|Slow` (the
  ES9038's fast or slow roll-off, as the Creative app sets it; Default keeps
  Windows' startup filter). Kept across sleep/resume.
- Optical (S/PDIF, new): both ways, stereo, on the card's optical jacks.
  - Out plays what the main output plays, whenever it's stereo at 24-bit
    (the default "PCM Bit Depth") up to 192 kHz. It's silent at 32-bit,
    at 384 kHz and in 5.1 (S/PDIF carries stereo up to 24-bit/192 kHz).
    It takes the stream as the app sends it, before the DSP: no SBX on
    it, and app volume shows on it but the knob and system volume don't
    (those set the card's analog output). Your receiver sets the level.
    "Optical Out Playback Switch" turns it off
    (`amixer -c AE9 cset name='Optical Out Playback Switch' off`).
  - In is its own capture device, `hw:AE9,1` ("AE-9 Optical"), 44.1 to
    192 kHz, so it records alongside the XLR mic
    (`arecord -D hw:AE9,1 -c 2 -r 48000 -f S32_LE in.wav`). For PipeWire,
    copy `config/ae9-optical-in.conf` to
    `~/.config/pipewire/pipewire.conf.d/` and run
    `systemctl --user restart pipewire`: an "AE-9 Optical In" source
    appears (never the default input). S/PDIF input can't resample, so
    record at the sender's rate (PipeWire runs at 48 kHz unless told
    otherwise). With nothing sending, recording fails at once and the
    kernel log says "no signal on the optical input".
- What U Hear (new): records what the card plays, as its own capture
  device, `hw:AE9,2` ("AE-9 What U Hear"), 48 or 96 kHz (96 kHz since
  0.9.5, `wuh_96k`), next to the XLR mic and optical in
  (`arecord -D hw:AE9,2 -c 2 -r 96000 -f S32_LE out.wav`). The PipeWire
  config below records at 48 kHz; change its `audio.rate` to 96000 for
  96 kHz. It
  works in Direct Mode and through the DSP; with SBX off it's the stream
  exactly as sent (a test tone comes back level-exact on both channels).
  For PipeWire, copy `config/ae9-what-u-hear.conf` to
  `~/.config/pipewire/pipewire.conf.d/` and run
  `systemctl --user restart pipewire`: an "AE-9 What U Hear" source
  appears (never the default input), for recording or streaming the
  card's own output.
- 5.1 (new in 0.6.7): pick "Analog Surround 5.1 Output" for the AE-9 in
  the desktop's sound settings (needs step 4). Front L/R come out of the
  card's RCA jacks, center and LFE out of its 3.5 mm C/Sub jack, rear L/R
  out of its 3.5 mm Rear jack. Works in Direct Mode (straight to the DACs, 48,
  96 or 192 kHz; 384 kHz stays stereo) and with Direct Mode off (through the
  DSP, 48 or 96 kHz). With Direct Mode off, "Speaker
  Config" tells the DSP your layout: 2.0/2.1 fold the six channels down
  to the front (and sub), 4.0/4.1 drop the center, 5.1 uses all six. It
  follows the stream by itself: a 5.1 stream turns 2.0/2.1 into 5.1, a
  stereo stream turns 5.1 back into 2.1 (4.0/4.1 stay as you set them).
  [SbAe9-SBC](https://github.com/3xHelix/SbAe9-SBC) switches the desktop
  profile from its Speaker Config and has a speaker test.
  `scripts/ae9-surround-test.sh` speaks each channel's name (stops PipeWire
  while it runs). 5.1 plays at 48 kHz (above that it's stereo only).
- [Helvum](https://gitlab.freedesktop.org/pipewire/helvum) (on Flathub)
  draws PipeWire's patchbay: a great way to see the AE-9's ports (FL FR on
  stereo, all six on the 5.1 profile) and what's feeding them.
- SBX effects and EQ (new in 0.6.0): two stages, as in Windows.
  "Direct Mode Switch" (default on) plays straight to the DAC at the
  stream's rate, DSP bypassed. Turn it off and the audio goes through the
  DSP (streams at 48 kHz there, PipeWire resamples the rest; stereo or
  5.1); then "SBX Switch" turns the effects
  on or off, and "Speaker Config" (2.0 or 2.1, default 2.1 as Windows) tells
  the DSP your speaker setup. With both, the "FX Surround / Dialog Plus / Smart Volume /
  Crystalizer / Bass" switches and volumes (0-100), "FX Smart Volume Mode",
  "EQ Switch", "EQ Preset" (Windows' 20 presets, Flat to Stadium), "EQ Pre-gain" and ten
  bands (31 Hz-16 kHz, -12..+12 dB) and "Scout Mode Switch" act; otherwise
  they are greyed out and keep their values. Set them in `alsamixer -c AE9`.
  Works on speakers and headphones, across sleep and reboots. With Direct
  Mode off, a new stream after boot, wake or a switch made while stopped
  plays direct for 3 s (`dsp_switch_ms`), then moves to the DSP. Turning
  Direct Mode on is silent until the stream restarts (pause/play), as in
  Windows.
- Volume: turn the head-unit knob (0.5 dB per step, ~1 dB per click); press
  it to mute or unmute. The same setting is the ALSA "Master Playback
  Volume" / "Master Playback Switch" (`alsamixer -c AE9`; `hu_master=0`
  names them "Head Unit Playback ..." instead), so PipeWire, the desktop
  slider and the knob all stay in sync. Range -127 dB to 0 dB; the desktop
  slider at 100% is 0 dB, the card's full output level.

### Low latency

PipeWire's defaults work as they are; nothing here is needed for normal
use. If you lower the quantum yourself, know this about the card: its DMA
fetches audio about 0.5-0.7 ms ahead of the playback position the driver
reports, the same on any machine. Below that (roughly 200 frames at
384 kHz, 100 at 192 kHz) PipeWire writes into audio the card has already
fetched, and you get a tinny, metallic static with no error logged.
PipeWire's `api.alsa.headroom` keeps extra frames queued past the card's
read point and cures it. Values tested clean down to quantum 32-64
(2026-11-05):

| Rate | Headroom | Measured read-ahead |
|---|---|---|
| 48 kHz | 64 | up to 33 frames |
| 96 kHz | 96 | up to 49 frames |
| 192 kHz | 144 | up to 97 frames |
| 384 kHz | 256 | up to 195 frames |

PipeWire takes one headroom per output, so pick the one for the rate you
use most. `config/60-ae9-headroom.conf` sets 256, which covers every rate
(edit the number for a lower one):

```sh
cp config/60-ae9-headroom.conf ~/.config/wireplumber/wireplumber.conf.d/
systemctl --user restart wireplumber
```

`pcm_posbuf=1` logs the read-ahead every 2 s while playing, to check it on
your card.

### Testing after a rebuild

`bus_reset=1` makes a module reload behave close to a cold start, which is
good for a quick check of a new build. It can disrupt streams, though
(recording especially, see Known issues), so reboot for good measure
before trusting the results:

```sh
systemctl --user stop pipewire.socket pipewire-pulse.socket pipewire pipewire-pulse wireplumber
sudo systemctl stop alsa-state          # alsactl holds the control device
sudo modprobe -r snd-hda-codec-creative-ae9 && sudo modprobe snd-hda-codec-creative-ae9
sleep 25; speaker-test -D hw:AE9 -c 2 -r 48000 -F S32_LE -t sine -l 2
sudo systemctl start alsa-state; systemctl --user start pipewire.socket pipewire-pulse.socket wireplumber
```

- Suspend/resume: supported. Resume runs the whole bring-up again in the
  background (~3-5 s after wake) and restores output, per-output volume,
  mute, knob light and mic boost; paused audio carries on.
- Unload, reboot and power-off run Windows' own shutdown sequence, so the
  card restarts cleanly (reloads and warm reboots tested).
- If the DSP misses its boot handshake (at load or on resume), the driver
  runs that shutdown and tries again: `resume_tries` (3) attempts,
  `resume_delay_ms` (2000) apart.
- Keep app volumes at 100% in PipeWire and use the knob/system volume (the
  card's hardware volume); app volumes above 100% are software gain and
  can clip.

### Reporting a problem

`sudo scripts/ae9-full-dump.sh` saves everything the driver can read about
the card (streams, controls, PipeWire, registers, the DSP's router, the
driver's recent log) to `ae9-dump-<time>.txt` in the current directory,
without changing anything. Take one while the problem is happening (and one
when it's fine, if you can) and attach them to your report. It only names
the apps that are playing; look it over before posting.

### Known issues

- Optical in: if the S/PDIF signal stops in the middle of a recording,
  the recording stalls (the card sends nothing without a signal) until
  the app gives up; start it again once the source is back.
- Unloading and reloading the driver several times in a row can stall the
  DSP ("tail replay done: ... N failed", DBGCNTL 0x00003400 instead of
  0x004d0000): playback works but recording is dead (arecord EIO), and a
  warm reboot doesn't clear it. Power off at the PSU for ~30 s to recover.
  Reboot instead of reloading where you can. The log line `DSP health:`
  after each startup warns when this has happened.
- Even a clean reload (no failures, `DSP health: ok`) can leave recording
  silent: the stream runs but every sample is zero. A reboot fixes it.
- A very small PipeWire quantum (64) can drop out under heavy CPU load
  (`pw-top` ERR column); 128 or more is fine.
- The first stream after a sample-rate or output change can hiccup briefly
  about half a second in: the DAC re-locking to its new settings (Windows
  sends them at every start; we only when something changed,
  `dac_start_skip`).
- After one warm reboot the first playback was silent until a new stream
  started (the card itself was working). Not reproduced yet.
- Suspend can be blocked by other programs stuck on network file I/O (seen
  with Plex reading from a CIFS share); not driver-related.
- Rarely, playback restarting after a pause comes up silent even though
  the stream runs and the audio reaches the card (every start block
  succeeds). Closing and reopening the player, or reloading the driver,
  brings it back. Cause inside the card not found yet; `hu_dsp_keepalive=2`
  logs the DSP's run state to help catch it.
- No sound from one app while others play: check whether it has a stream at
  all with `pactl list sink-inputs | grep -E "Sink:|application.name"`. Seen
  with Chrome Canary after output switches or a reboot (no stream; VLC
  played fine); restarting the app fixes it. The earlier "silent first
  playback after a warm reboot" was most likely this.
- Now and then a warm reboot leaves the card dark (lights off, no head
  unit, no sound); Windows sees the same at times, so it's the card. The
  driver notices within a second (`codec 1 (DSP) not answering after the
  bus reset`), tries a few times, and if it says `never answered`, power
  off at the PSU for ~20 s; only that clears it (a warm reboot sometimes
  does, a reload doesn't).
- Lots of rapid switching in a row (sample rates, Direct Mode, stereo/5.1,
  bit depth, dozens of times) can wear the card into a bad state: a stream
  that stutters more and more, or plays nothing while the player runs.
  A warm reboot doesn't clear it, a PSU power-off does. Normal use doesn't
  get there; it showed up during heavy testing.
- SBX: if Direct Mode off ever gives silence or echoey, crunched audio
  that toggling doesn't fix, the card's audio router is in a bad state;
  only a PSU power-off clears it (a warm reboot doesn't). Seen often while
  SBX was being developed, and once since (0.6.2, a warm reboot onto a new
  kernel; the power-off fixed it).

## Source layout

| File | Purpose |
|---|---|
| `creative_ae9_core.c` | PCI probe, bus reset, HDA reset, codec detection, Windows replays (pre-download, post-boot), head-unit poll/display/knob and ALSA volume controls, debugfs, module params |
| `creative_ae9_dsp.c` | DSP firmware download, MASTERCONTROL boot handshake, overlay transfer, SCP messaging, CA0113 (head-unit DAC) commands |
| `creative_ae9_i2c.c` | Head-unit frames over the card's UART (the SPI/I2C names in it are historical) |
| `creative_ae9_corb.c` | CORB/RIRB DMA ring + verb I/O |
| `creative_ae9_pcm.c` | ALSA PCM playback, XLR capture, optical out (follows playback), optical in (device 1) and What U Hear (device 2) |
| `creative_ae9_fx.c` | SBX effects, EQ, Direct Mode and SBX controls |
| `creative_ae9.h`, `ca0132_regs.h` | Registers, chip struct, MMIO accessors; definitions ported from the in-tree ca0132 driver |
| `creative_ae9_*_verbs.h`, `creative_ae9_switch_ops.h` | Sequences recorded from the Windows driver (init, cold boot, warm reset, recording, output switch, shutdown), auto-generated |
| `extract_firmware.py`, `extract_overlay.py` | Extract the DSP firmware and overlay from Creative's `CtxHda.sys` |
| `config/` | Boot binding (udev rule, modprobe hook, dracut omit) and optional module options |
| `scripts/restore-stock-audio.sh` | Remove the driver and restore stock `snd_hda_intel` |
| `config/60-ae9-headroom.conf` | Optional PipeWire headroom for small quantums (see "Low latency") |
| `config/ae9-optical-in.conf` | Optional PipeWire source for the optical input (see "Optical") |
| `config/ae9-what-u-hear.conf` | Optional PipeWire source for What U Hear (see "What U Hear") |
| `config/61-ae9-names.conf` | Optional desktop name for the recording input, "AE-9 Head Unit Mic / Aux" |

## Module parameters

Windows-aligned set (each replaces one of our earlier guesses with what the
cold-boot Windows capture does). Since 2026-11-02 the values below are the
built-in defaults; set one to the other value only to go back to the older
code path for comparison:

- `enable_pcm=1`, `load_fw=1` — PCM device and DSP firmware.
- `bus_reset` — secondary bus reset at probe (codec SSIDs only read
  0x11020071/72 after one; a VM start does the same).
- `bar2_first`, `win_reset2`, `cold_prefw`, `codec2_first`, `prep_windows` —
  pre-download init as Windows does it (815-verb cold table).
- `fw_split`, `dl_windows`, `dl_reset_chunks` (default 1), `dl_bdl64`
  (default 1), `hs_immediate`, `scp_event`, `scp_win_read`, `chipio_nocache`,
  `sentinel_clear=0` —
  firmware download, DSP start and the MASTERCONTROL boot handshake.
- `overlay` — transfer the DSP overlay image during the handshake.
- `surround` (default 1, writable at runtime) — 6-channel (5.1) playback and
  the 4.0/4.1/5.1 speaker configs; 0 = stereo only. `surround_ports`
  (0x50,0x51,0x60,0x61) the router targets for the DSP's center, LFE, rear
  L/R outputs.
  `surround_96k` (1, writable at runtime) — 5.1 at 96 kHz in Direct Mode
  (fixed 2026-11-05: all six speakers right, clean over minutes); 0 limits
  5.1 to 48 kHz. Pick the 5.1 profile and `clock.force-rate 96000`.
  `surround_192k` (1, writable at runtime) — 5.1 at 192 kHz in Direct Mode
  (v0.9.4, Windows' router layout); 0 limits 5.1 to 96 kHz.
- `pcm_rates` (2, writable at runtime) — playback rates: 2 = 44.1-384
  kHz, 1 = without 44.1/88.2 kHz, 0 = 48 kHz only. `dac_441` (1) sends
  Windows' DAC commands for 44.1/88.2 kHz (without them those rates play
  garbled or silent). `src_reclock` (1) sets the I2S to its 48 kHz clocks before
  44.1/88.2 kHz (0.9.6; without it they went silent after a 384 kHz stream).
- `wuh_96k` (1, writable at runtime) — What U Hear at 48 or 96 kHz
  (0.9.5); 0 = 48 kHz only.
- `dsp_96k` (1, writable at runtime) — 96 kHz with Direct Mode off, through
  the DSP (v0.9.4); 0 limits DSP mode to 48 kHz.
- `debug` (default 0, writable at runtime) — log detail. 0: startup, your
  actions (Direct Mode/SBX, output switches, mutes) and problems. 1: also
  the routine per-stream and per-step lines (command blocks, router, volume
  steps). 2: plus the raw head-unit button state. Turn it on to dig in:
  `echo 1 | sudo tee /sys/module/snd_hda_codec_creative_ae9/parameters/debug`.
- `codec1_failfast` (default 1, writable at runtime) — stop a bring-up right
  after codec detection when codec 1 is silent (the dark-card case). 0 runs
  the whole bring-up anyway: 2-3 min of timeouts, for experiments only.
- `fw_verify` (default 0) — read the firmware back after download and count
  mismatches.
- `ca0113_windows` — head-unit DAC commands with Windows' protocol/values.
- Head unit: the driver replays Windows' post-boot BAR2 activity (speaker
  mode) and runs Windows' head-unit poll, volume display and knob, with the
  ALSA controls "Head Unit Playback Volume" / "Head Unit Playback Switch"
  (always on since 0.9.5; the old `post_windows=0` path is gone).
  `hu_master` (1) names the volume/mute "Master" for desktop sync;
  `warm_reset` (0) replays Windows' warm-DSP reset block before the
  download (1 = when the DSP reads dead, 2 = always; experimental);
  `resume_tries` (3) / `resume_delay_ms` (2000) bring-up retries;
  `pcm_stop_block` (1, writable at runtime) send Windows' playback stop
  block at stream release; 0 skips it (experiment for the rare silent
  restart after a pause);
  `hu_dsp_keepalive` (0) heartbeat DSP keepalive (1 = old forced
  run state, which left playback silent after the output idled; 2 = log only);
  `hu_fast_ms` (40) knob poll interval; `hu_cmd_settle_us` (4000, writable at
  runtime) CA0113 settle time for volume changes;
  `tail_drop_rejected` (1) leave out the six startup SCP messages the DSP
  rejects.
- SBX: `fx_spk_cfg` (2) the "Speaker Config" setting at load (1 = 2.0,
  2 = 2.1 as Windows); `dsp_switch_ms` (3000, writable at runtime) how long a fresh
  stream plays direct before the switch to the DSP.
- Playback buffer (all writable at runtime, applied at the next stream):
  `pcm_periods` (8) most periods per buffer, 2-32 (2 is Windows' 48 kHz
  layout; with 2 an app asking for a big quantum could fill the whole
  buffer); `pcm_nowakeup` (0) 1 lets PipeWire turn period interrupts off;
  `pcm_posbuf` (0) 1 logs how far the DMA reads ahead of the reported
  position (see "Low latency"); `dac_start_skip` (1) sends the DAC's start
  commands only when the rate or output changed (each send makes it
  re-lock, a brief hiccup); 0 sends them at every start, as Windows.

- Recording: `cap_bits` (24, writable at runtime; also the "Capture Bit
  Depth" control) 24- or 32-bit recording samples.
- Optical: `spdif_out` (1, writable at runtime; also the "Optical Out
  Playback Switch") optical out follows the main output; 0 keeps it
  silent.

Older parameters:

- `init_mask` (default `0x1f`) — gates BAR2 init stages.
- `vfio_mask` (default `0x1b`) — VFIO BAR2 sub-phases; **P3 disabled** (it turns
  off the card LEDs on this board).
- `full_init` (default `0`) — replay captured codec init verbs (bitmask:
  1=cad1, 2=cad2, 3=both).
- `acm_first_delay_ms` / `acm_pass_gap_ms` / `acm_passes` / `acm_heartbeat_ms`
  (12 s / 4 s / 2 / 500 ms) — head-unit init and keepalive timing.

## Where this comes from

The driver replays what Creative's Windows driver does, recorded from a
Windows VM with the card passed through. The capture data, analysis and
decode tools live in the development repository; the `creative_ae9_*_verbs.h`
tables here hold everything the driver uses from them. Creative's Windows
driver files are not included.

## Credits

This driver stands on other people's work:

- **The Linux kernel's CA0132 driver** (`sound/pci/hda/patch_ca0132.c`,
  Copyright (c) 2011 Creative Technology Ltd., based on `patch_ca0110.c`,
  Copyright (c) 2008 Takashi Iwai): the DSP firmware download (ChipIO,
  DSPIO, SCP), the AE-5/AE-7 init tables, the audio router and the effect
  defaults are adapted from it.
- **[verzic/sb-ae9](https://github.com/verzic/sb-ae9)**: the first
  out-of-tree AE-9 driver, which showed the card could work on Linux.
- **[DarkNikGr/sb-ae9](https://github.com/DarkNikGr/sb-ae9)**: optical
  S/PDIF routing, the stream 0x05 setup, and the reverse-engineering notes
  on the head unit's serial link.
- **[s3boun3t/ae9_build](https://github.com/s3boun3t/ae9_build)**: the
  AE-9 MMIO init sequence and the head unit (ACM) init, adapted here.
- **Creative's own CA0132 Linux build** ("0.73_kernel18", GPL), shared with
  the project: the AE-9 output, mic and speaker setup sequences.
- **Kaiba**: testing on an ASMedia-bridge AE-9, round after round.

Everything Windows-side (timings, verb order, the head unit protocol) comes
from our own VFIO captures of Creative's Windows driver; none of its code
is included. Each source file names what it adapted, with the original
copyright notices.

## License

GPL-2.0-or-later: the GNU GPL version 2, or (at your option) any later version. It fits in the Linux kernel (GPLv2) and can also be used under GPLv3. Full text: [COPYING](COPYING).
Use it, change it and share it freely; if you share it, share the source
too, under the same terms. `ca0132_regs.h` comes from the kernel's CA0132
driver unchanged; code adapted from it and from the projects under
Credits keeps their copyright notices. The DSP firmware is
Creative's and is not part of this repository: `extract_firmware.py` pulls
it from your own copy of the Windows driver.

## Disclaimer

Experimental and reverse-engineered. Use at your own risk. Not affiliated
with Creative.
