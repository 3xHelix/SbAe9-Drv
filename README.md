# Creative Sound Blaster AE-9 — Linux Driver (WIP)

An out-of-tree Linux kernel driver for the **Creative Sound Blaster AE-9**
(CA0132 "Sound Core3D" DSP + detachable ACM head unit), reverse-engineered from
a Windows VFIO MMIO capture. The goal is functionality the in-tree
`snd_hda_intel` + `snd_hda_codec_ca0132` stack does not provide for this board:
card LEDs, the ACM head unit (OLED display + volume knob + buttons), and PCM
playback.

> **Status (2026-11-01):** stereo playback works on the card's RCA outputs
> (speaker mode), clean on both channels, through ALSA and PipeWire.
> Recording from the head unit's XLR mic input works (arecord, Audacity).
> The ACM head unit shows the volume; its knob sets the volume, a press
> mutes, a medium hold switches between the RCA outputs and the head unit's
> headphone jack, and a long hold toggles the knob light.
> The driver gets there by doing what the Windows driver does in a cold-boot
> VFIO capture; that setup is the built-in default, no module options needed. Not done yet: headphone
> sample rates other than 48 kHz.
> This repository holds only what you need to build and run the driver.
> Changes per release: **[CHANGELOG.md](CHANGELOG.md)**.

## Hardware / Environment

- Creative Sound Blaster AE-9 — PCI `1102:0010`, subsystem `0x0071`
  (CA0132 Sound Core3D, behind an IDT Tsi381 PCIe-to-PCI bridge), with the
  ACM head unit connected.
- Developed/tested on Fedora 44 KDE, kernels `7.2.13` and `7.2.14`
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

```sh
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
sudo cp snd-hda-codec-creative-ae9.ko /lib/modules/$(uname -r)/extra/ && sudo depmod -a
```

Redo both after every code change and kernel update.

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

`scripts/restore-stock-audio.sh` removes the driver and restores stock
`snd_hda_intel`.

Reboot after setup. A full power-off (PSU off) is the safest first start.

## Using it

- Output: the RCA jacks on the card (speaker mode, "-SP-") or the head
  unit's headphone jack ("-HP-"). Starts on speakers; switch with a 1.5-3.5 s
  knob hold or the "Head Unit Output" control. Each output remembers its own
  volume (both start at -36 dB).
- Knob: turn = volume, press (< 1 s) = mute, hold 1.5-3.5 s = speakers /
  headphones, hold > 3.5 s = knob light on/off.
- In PipeWire / KDE use the "Stereo Output + Stereo Input" profile: the
  desktop volume slider and mute drive the card (hardware volume), and the
  knob moves the slider and OSD. "Pro Audio" also plays, but uses software
  volume only.
- The PCM takes 2 channels, S32_LE (24 significant bits), 48 kHz only, for
  both playback and recording. PipeWire converts other rates.
- Recording: the head unit's XLR mic input (`arecord -D hw:AE9 -c 2 -r 48000
  -f S32_LE`, or the AE-9 input in PipeWire/Audacity). "XLR Gain" sets
  the mic preamp to 0/+10/+20 dB (default +10, like Windows). It is separate
  from the desktop microphone slider, which is software gain only; set it with
  `alsamixer -c AE9` (F4) or `amixer -c AE9 sset 'XLR Gain' 0|1|2`. Phantom power is the 48V button on
  the head unit's front panel; the driver doesn't need to do anything.
- Volume: turn the head-unit knob (0.5 dB per step, ~1 dB per click); press
  it to mute or unmute. The same setting is the ALSA "Master Playback
  Volume" / "Master Playback Switch" (`alsamixer -c AE9`; `hu_master=0`
  names them "Head Unit Playback ..." instead), so PipeWire, the desktop
  slider and the knob all stay in sync. Range -127 dB to 0 dB; the desktop
  slider at 100% is 0 dB, the card's full output level.

### Testing after a rebuild

`bus_reset=1` makes a module reload behave like a cold start, so a reboot
isn't needed:

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

### Known issues

- Unloading and reloading the driver several times in a row can stall the
  DSP ("tail replay done: ... N failed", DBGCNTL 0x00003400 instead of
  0x004d0000): playback works but recording is dead (arecord EIO), and a
  warm reboot doesn't clear it. Power off at the PSU for ~30 s to recover.
  Reboot instead of reloading where you can. The log line `DSP health:`
  after each startup warns when this has happened.
- A very small PipeWire quantum (64) can drop out under heavy CPU load
  (`pw-top` ERR column); 128 or more is fine.
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
- Firmware read-back (`fw_verify=1`) reports a few dozen mismatches in the DSP's
  auxiliary RAM; audio works regardless (likely a read-back artifact, open).

## Source layout

| File | Purpose |
|---|---|
| `creative_ae9_core.c` | PCI probe, bus reset, HDA reset, codec detection, Windows replays (pre-download, post-boot), head-unit poll/display/knob and ALSA volume controls, debugfs, module params |
| `creative_ae9_dsp.c` | DSP firmware download, MASTERCONTROL boot handshake, overlay transfer, SCP messaging, CA0113 (head-unit DAC) commands |
| `creative_ae9_i2c.c` | Head-unit frames over the card's UART (the SPI/I2C names in it are historical) |
| `creative_ae9_corb.c` | CORB/RIRB DMA ring + verb I/O |
| `creative_ae9_pcm.c` | ALSA PCM playback and capture (2 ch S32_LE 48 kHz) |
| `creative_ae9.h`, `ca0132_regs.h` | Registers, chip struct, MMIO accessors; definitions ported from the in-tree ca0132 driver |
| `creative_ae9_*_verbs.h`, `creative_ae9_switch_ops.h` | Sequences recorded from the Windows driver (init, cold boot, warm reset, recording, output switch, shutdown), auto-generated |
| `extract_firmware.py`, `extract_overlay.py` | Extract the DSP firmware and overlay from Creative's `CtxHda.sys` |
| `config/` | Boot binding (udev rule, modprobe hook, dracut omit) and optional module options |
| `scripts/restore-stock-audio.sh` | Remove the driver and restore stock `snd_hda_intel` |

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
  `sentinel_clear=0`, `lineout_prerun=0`, `loop_start=0` —
  firmware download, DSP start and the MASTERCONTROL boot handshake.
- `overlay` — transfer the DSP overlay image during the handshake.
- `fw_verify` (default 0) — read the firmware back after download and count
  mismatches.
- `ca0113_windows` — head-unit DAC commands with Windows' protocol/values.
- `post_windows` — replay Windows' post-boot BAR2 activity (speaker mode),
  Windows' head-unit poll, volume display, knob, and the ALSA controls
  "Head Unit Playback Volume" / "Head Unit Playback Switch".
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
  runtime) CA0113 settle time for volume changes.

Older parameters:

- `init_mask` (default `0x1f`) — gates BAR2 init stages.
- `vfio_mask` (default `0x1b`) — VFIO BAR2 sub-phases; **P3 disabled** (it turns
  off the card LEDs on this board).
- `full_init` (default `0`) — replay captured codec init verbs (bitmask:
  1=cad1, 2=cad2, 3=both).
- `enable_audio_path` (default `1`) — run DAC route + amp enable after ACM init.
- `acm_first_delay_ms` / `acm_pass_gap_ms` / `acm_passes` / `acm_heartbeat_ms`
  (12 s / 4 s / 2 / 500 ms) — head-unit init and keepalive timing.

## Where this comes from

The driver replays what Creative's Windows driver does, recorded from a
Windows VM with the card passed through. The capture data, analysis and
decode tools live in the development repository; the `creative_ae9_*_verbs.h`
tables here hold everything the driver uses from them. Creative's Windows
driver files are not included.

## Disclaimer

Experimental and reverse-engineered. Use at your own risk. Not affiliated
with Creative.
