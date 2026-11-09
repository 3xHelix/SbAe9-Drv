# Creative Sound Blaster AE-9 — Linux Driver (WIP)

An out-of-tree Linux kernel driver for the **Creative Sound Blaster AE-9**
(CA0132 "Sound Core3D" DSP + detachable ACM head unit), reverse-engineered from
a Windows VFIO MMIO capture. The goal is functionality the in-tree
`snd_hda_intel` + `snd_hda_codec_ca0132` stack does not provide for this board:
card LEDs, the ACM head unit (OLED display + volume knob + buttons), and PCM
playback.

> **Status (2026-11-08, v0.12.1):** stereo playback works on the card's RCA outputs
> (speaker mode), clean on both channels, through ALSA and PipeWire.
> Recording from the head unit's XLR or 3.5 mm input works, at 48 or 96 kHz, 24- or 32-bit (arecord, Audacity).
> Optical S/PDIF works both ways: out is its own output (stereo, up
> to 24-bit/192 kHz; mirrors the main output if you switch that on), in is its own recording device. What U Hear records
> what the card plays. The head unit's rear RCA input, What U Hear and
> optical in show up in PipeWire as their own sources.
> The ACM head unit shows the volume; its knob sets the volume, a press
> mutes, a medium hold switches between the card's RCA outputs and the
> head unit's headphone jacks, and a long hold toggles the knob light. The SBX button
> turns SBX on and off (with Direct Mode off) and its light shows when the
> effects are running.
> The driver gets there by doing what the Windows driver does in a cold-boot
> VFIO capture; that setup is the built-in default, no module options needed. Playback at
> 44.1, 48, 88.2, 96, 176.4, 192 and 384 kHz, 24- or 32-bit ("PCM Bit Depth"), works out of
> the box in Direct Mode. SBX effects and the EQ work through the card's
> DSP, with Direct Mode off. 5.1 works on the card's RCA, Rear and
> C/Sub jacks, at 48, 96 or 192 kHz in Direct Mode and 48 or 96 kHz through
> the DSP. With Direct Mode off there's also Sound Blaster Command's speaker
> setup (bass redirection, per-speaker level and distance) and its 32
> headphone profiles.
> This repository holds only what you need to build and run the driver.
> Changes per release: **[CHANGELOG.md](CHANGELOG.md)**.

> **Control panel:** [SbAe9-SBC](https://github.com/3xHelix/SbAe9-SBC) is a
> Sound Blaster Command-style desktop app for this driver: SBX, the EQ and
> its presets, Scout Mode, Direct Mode, outputs, recording and the mixer,
> all in one window. It's a solid stand-in for the Windows software that
> ships with the card.
> Driver v0.12.1 pairs with SbAe9-SBC v0.16.0 (the Optical Out mirror
> toggle on Playback > Digital, the Encoder page: AC-3, or experimental
> DTS, to optical) and SbAe9-AudioMon v0.4.3 (Optical Out meter); v0.12.0
> paired with SbAe9-SBC v0.15.0 and AudioMon v0.4.2. Older
> SbC versions still work, with fewer controls.

## Hardware / Environment

- Creative Sound Blaster AE-9 — PCI `1102:0010`, subsystem `0x0071`
  (CA0132 Sound Core3D, behind a PCIe-to-PCI bridge), with the ACM head unit
  connected.
- Developed/tested on Fedora 44 KDE, kernels `7.2.13`, `7.2.14` and `7.2.15`
  (`-200.fc44.x86_64`).
- No `iommu=pt` needed: tested on an AMD system with the IOMMU in its
  default translated mode, with no IOMMU page faults from the card. (Drivers
  built on the kernel's ca0132 code report needing it; this one doesn't.)
- Works with both versions of the card: AE-9s exist with an IDT/Tundra
  Tsi381 (`10e3:8111`, the developer's card) or an ASMedia ASM1083/1085
  (`1b21:1080`) PCIe-to-PCI bridge. The ASMedia version works since 0.9.8
  (tested by Kaiba on a rev 03 bridge); the driver spots that bridge and
  switches its workarounds on by itself. To see which one sits above your card:
  ```sh
  lspci -nn -s $(basename $(dirname $(readlink -f /sys/bus/pci/devices/$(lspci -D -d 1102:0010 | cut -d' ' -f1))))
  ```
  (On Windows: Device Manager, View, Devices by connection; the PCI bridge
  above the AE-9 shows `VEN_10E3&DEV_8111` or `VEN_1B21&DEV_1080`.)

## Setup

### Fedora: install from COPR (recommended)

Packages for every release, with the module rebuilt for each new kernel by
akmods (Fedora's usual way for out-of-tree drivers). They also do steps 3
and 4 below for you.

```sh
sudo dnf copr enable 3xhelix/SbAe9-Drv
sudo dnf install creative-ae9
```

akmods builds the module right after the install, which takes about a
minute (`modinfo -n snd-hda-codec-creative-ae9` shows it once it's there).
Then install the firmware (next section) and power the PC fully off and on.

**Fedora Atomic** (Silverblue, Kinoite and the other rpm-ostree desktops):

```sh
sudo curl -o /etc/yum.repos.d/SbAe9-Drv.repo https://copr.fedorainfracloud.org/coprs/3xhelix/SbAe9-Drv/repo/fedora-$(rpm -E %fedora)/3xhelix-SbAe9-Drv-fedora-$(rpm -E %fedora).repo
rpm-ostree install creative-ae9
```

Reboot into the new deployment, install the firmware, reboot once more
(the firmware location becomes a kernel option there; see below).

**Secure Boot:** akmods signs the module with a key of its own. Enroll it
once with `sudo mokutil --import /etc/pki/akmods/certs/public_key.der`,
reboot, and confirm the enrollment in the blue MOK screen. Without that,
a Secure Boot kernel refuses the module.

**Moving over from DKMS or a hand install:** remove that first (see "One
install method per machine" below), then install the package.

Packages: `creative-ae9` (boot setup, PipeWire profiles, `ae9-get-firmware`,
`ae9-full-dump`) pulls in `akmod-creative-ae9` (the module).
`sudo dnf remove creative-ae9 akmod-creative-ae9` takes it all out.

### Ubuntu, Linux Mint, Pop!_OS: from the PPA (since 0.10.0)

The Launchpad PPA `ppa:3xhelix/sbae9` has the DKMS package for Ubuntu
24.04 LTS, 26.04 LTS and 26.10, and so for Linux Mint 22, Pop!_OS 24.04
and the other Ubuntu-based desktops. New versions arrive with your normal
updates. Tested by installing from the PPA on all three (kernels 6.8, 7.0
and 7.3).

```sh
sudo add-apt-repository ppa:3xhelix/sbae9
sudo apt install creative-ae9-dkms innoextract
```

Then the firmware (step 1) and a full power-off. `dkms status` shows the
module per kernel.

### Debian (and Ubuntu without the PPA): the .deb (since 0.10.0)

Each release has `creative-ae9-dkms_<version>-1_all.deb` on its
[GitHub release page](https://github.com/3xHelix/SbAe9-Drv/releases):
the driver source for DKMS (rebuilt for every kernel you install, headers
needed) plus the boot setup, PipeWire profiles, `ae9-get-firmware` and
`ae9-full-dump`. Tested on Debian 13 (kernel 6.12) and Ubuntu 24.04
(kernel 6.8) in containers.

```sh
sudo apt install ./creative-ae9-dkms_0.12.1-1_all.deb
sudo apt install innoextract     # for ae9-get-firmware with Creative's installer
```

Then the firmware (step 1) and a full power-off. `dkms status` shows the
module per kernel; `sudo apt remove creative-ae9-dkms` takes it all out.
Secure Boot: DKMS signs the module with a machine key. Ubuntu asks you to
enroll it during the install (a password, then the blue MOK screen at the
next boot); on Debian enroll it once with
`sudo mokutil --import /var/lib/dkms/mok.pub` and reboot.
`packaging/deb/build-deb.sh` builds the package from a source tree or
release tarball.

### openSUSE, RHEL / AlmaLinux / Rocky 10: from COPR (since 0.10.0)

The same COPR repository as Fedora also builds the DKMS package for
openSUSE Tumbleweed, openSUSE Leap 16 and RHEL / AlmaLinux / Rocky 10
(with EPEL), so new versions arrive with your normal system updates. DKMS
rebuilds the module for every kernel with headers. Tested on Tumbleweed
(kernel 7.3) and AlmaLinux 10 (kernel 6.12).

```sh
# openSUSE Tumbleweed (Leap 16: write opensuse-leap-16.0 in both places of the URL)
sudo zypper addrepo https://copr.fedorainfracloud.org/coprs/3xhelix/SbAe9-Drv/repo/opensuse-tumbleweed/3xhelix-SbAe9-Drv-opensuse-tumbleweed.repo
sudo zypper --gpg-auto-import-keys install kernel-default-devel innoextract creative-ae9-dkms

# RHEL / AlmaLinux / Rocky 10
sudo dnf install epel-release dnf-plugins-core
sudo dnf copr enable 3xhelix/SbAe9-Drv
sudo dnf install kernel-devel innoextract creative-ae9-dkms
```

Then the firmware (step 1) and a full power-off. `dkms status` shows the
module per kernel; removing `creative-ae9-dkms` takes it all out. Without
the repository: each release also has
`creative-ae9-dkms-<version>-1.noarch.rpm` on its
[release page](https://github.com/3xHelix/SbAe9-Drv/releases)
(`sudo zypper install --allow-unsigned-rpm ./creative-ae9-dkms-*.rpm` or
`sudo dnf install ./creative-ae9-dkms-*.rpm`; no updates then).
`packaging/creative-ae9-dkms.spec` builds it. (Fedora: use the packages
above; this one conflicts with them.)

### 1. Firmware (and headphone profiles)

The DSP firmware isn't in the repo or the packages: it's Creative's. The
easiest source is Creative's own installer for the AE-9, **Sound Blaster
Command + driver** (`AECMDMasterInstaller_3.4.92.00.exe`, from Creative's AE-9
support page). `ae9-get-firmware` reads it directly (it needs
`innoextract`, in Fedora's, Arch's and Debian's repos; the installer is only
unpacked, never run):

```sh
sudo dnf install innoextract                            # once (or pacman / apt)
sudo ae9-get-firmware ~/Downloads/AECMDMasterInstaller_3.4.92.00.exe
sudo scripts/ae9-get-firmware.sh ~/Downloads/AECMDMasterInstaller_3.4.92.00.exe   # from this repo
```

That installs the main DSP image and the AE-9 overlay, checked against the
MD5 of the firmware this driver is tested with (`85c8f2b4…` main image,
`0e222c94…` overlay), plus Sound Blaster Command's 32 **headphone profiles**
(also checked) for the "Headphone Profile" control. It also takes an unpacked
installer folder, the AE-Series driver installer on its own (firmware only),
or just `CtxHda.sys` (`AESeriesDriver/Driver/AMD64/CtxHda.sys`; a Windows
install has it as `C:\Windows\System32\drivers\CtxHda.sys`). Files go to
`/lib/firmware`; on Fedora Atomic to `/etc/firmware`, with the kernel option
`firmware_class.path=/etc/firmware` added (active after the next reboot).
`ae9-get-firmware --check` shows what's installed.

By hand, from `CtxHda.sys`:

```sh
python3 extract_firmware.py /path/to/CtxHda.sys ctefx-desktop.bin   # main DSP image
python3 extract_overlay.py  /path/to/CtxHda.sys ctefx-ae9-overlay.bin
sudo install -Dm644 -t /lib/firmware/creative-ae9 ctefx-desktop.bin ctefx-ae9-overlay.bin
```

and the headphone profiles from an unpacked installer (`innoextract
AECMDMasterInstaller_3.4.92.00.exe`):

```sh
python3 extract_hpeq.py tmp/AE9HPEQ hp/
sudo mkdir -p /lib/firmware/creative-ae9/hp && sudo cp hp/*.bin /lib/firmware/creative-ae9/hp/
```

The scripts refuse a different image unless you pass `--force`.

### 2. Build and install (other distributions, or from source)

**With DKMS:** new kernels get the driver rebuilt automatically, nothing to
redo after a kernel update.

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

Both find the card by its IDs, in any slot (since 0.9.9; before, the
modprobe file had the PCI address written in). The module must load from the
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
  88.2, 96, 176.4, 192 and 384 kHz in Direct Mode (44.1/88.2/176.4 kHz stereo only),
  48 or 96 kHz with Direct Mode off, through the DSP (v0.9.4; PipeWire
  resamples other rates). Recording is 48 or 96 kHz.
- Choosing a rate: PipeWire runs at 48 kHz unless told otherwise.
  `pw-metadata -n settings 0 clock.allowed-rates '[ 44100 48000 88200 96000 176400 192000 384000 ]'`
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
- DAC filter (seven since 0.11.0, thanks to DarkNikGr): the ES9038's
  digital filters with the Creative app's names: Apodizing Fast Roll Off
  (Windows' startup filter, the default), Fast Roll Off, Slow Roll Off,
  Fast Roll Off - Minimum Phase, Slow Roll Off - Minimum Phase, Hybrid Fast
  Roll Off, Brick Wall (`amixer -c AE9 sset 'DAC Filter' 'Brick Wall'`).
  Set the way the Creative app sets them, both DACs included; they apply in
  Direct Mode too. Kept across sleep/resume. (Before 0.11.0 the choices
  were Default/Fast/Slow; a saved old choice comes back as the default
  once.)
- Optical (S/PDIF, new): both ways, stereo, on the card's optical jacks.
  - Out plays what you send to it: the "Optical Out" output (PCM device 4,
    below). "Optical Out Mirror Switch" (off by default since 0.12.1; on
    before) makes it also play what the main output plays while nothing
    is sent to it, whenever that's stereo at 24-bit (the default "PCM Bit
    Depth") up to 192 kHz. It's silent at 32-bit,
    at 384 kHz and in 5.1 (S/PDIF carries stereo up to 24-bit/192 kHz).
    It takes the stream as the app sends it, before the DSP: no SBX on
    it, and app volume shows on it but the knob and system volume don't
    (those set the card's analog output). Your receiver sets the level.
    "Optical Out Playback Switch" turns it off
    (`amixer -c AE9 cset name='Optical Out Playback Switch' off`).
  - Separate optical output (new in 0.12.0, thanks DarkNikGr): PCM device
    4, `hw:AE9,4` ("AE-9 Optical Out"; the "Optical Out" output in
    PipeWire), its own stream at 44.1-192 kHz, 16-bit or 24-bit in 32.
    With the mirror switched on, the optical jack carries the main output
    whenever nothing plays to it (PipeWire closes it a few seconds after
    its last sound); with it off (default), optical stays silent then. Dolby Digital / DTS passthrough to a receiver:
    `ae9-optical-passthrough movie.mkv` (needs ffmpeg; it sets the S/PDIF
    "non-audio" bit while it plays: `iecset -c <card> audio off`, the
    "IEC958 Playback Default" control on device 4).
  - In is its own capture device, `hw:AE9,1` ("AE-9 Optical"), 44.1 to
    192 kHz, so it records alongside the XLR mic
    (`arecord -D hw:AE9,1 -c 2 -r 48000 -f S32_LE in.wav`). In PipeWire
    it's the "Optical In" source of the AE-9's card profile (since 0.11.0;
    nothing to copy). S/PDIF input can't resample, so record at the
    sender's rate (PipeWire runs at 48 kHz unless told otherwise). With
    nothing sending it records silence (since 0.11.0; it used to fail);
    plugging the cable in or pulling it while recording restarts the
    recording once, on the signal or on silence.
- Head unit rear RCA input (new in 0.10.1): the RCA L/R jacks on the back
  of the head unit are their own capture device, `hw:AE9,3` ("AE-9 Rear RCA
  In"), 24-bit, 44.1 to 192 kHz, recording alongside the XLR mic
  (`arecord -D hw:AE9,3 -c 2 -r 48000 -f S32_LE rca.wav`). In PipeWire
  it's the "Rear RCA In" source of the AE-9's card profile (since 0.11.0;
  nothing to copy). No level control yet: line-level sources record
  around -25 dB below full scale.
- What U Hear (new): records what the card plays, as its own capture
  device, `hw:AE9,2` ("AE-9 What U Hear"), 48 or 96 kHz (96 kHz since
  0.9.5, `wuh_96k`), next to the XLR mic and optical in
  (`arecord -D hw:AE9,2 -c 2 -r 96000 -f S32_LE out.wav`). It
  works in Direct Mode and through the DSP; with SBX off it's the stream
  exactly as sent (a test tone comes back level-exact on both channels).
  In PipeWire it's the "What U Hear" source of the AE-9's card profile
  (since 0.11.0; nothing to copy), for recording or streaming the card's
  own output; it runs at PipeWire's rate (48 or 96 kHz; SbAe9-SBC can pin
  it). With the 5.1 output profile ("Analog Surround 5.1 Output + Analog
  Stereo Input") it records all six channels instead (since 0.12.1:
  "What U Hear" source with node name `...what_u_hear_51`, FL FR FC LFE RL
  RR; `arecord -D hw:AE9,2 -c 6 ...`), e.g. for an AC-3 encoder; the
  stereo profiles keep the stereo source. Upgrading from 0.10.x: if you copied `ae9-what-u-hear.conf`,
  `ae9-rear-rca-in.conf` or `ae9-optical-in.conf` into
  `~/.config/pipewire/pipewire.conf.d/`, delete them, or each source shows
  up twice.
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
  Speaker setup (0.10.0; Direct Mode off, SBX on or off): "Bass Redirect
  Switch" sends the bass to the sub (x.1 configs), "Bass Redirect
  Crossover" (50-200 Hz, default 80; Windows' Tower is 80, Desktop 120) and
  "Bass Redirect Sub Boost Switch" (subwoofer boost, on/off); per speaker (Front L/R,
  Center, Sub, Rear L/R) "Speaker Level ... Volume" (-12..+12 dB) and
  "Speaker Distance ..." (0-500 cm of delay, as Windows' calibration).
  "Headphone Profile" (0.10.0): Sound Blaster Command's 32 headphone curves
  (ATH-M50, HD 280 Pro, ...), active with Direct Mode off, headphones out
  and a 48 kHz stream; off on speakers. The curves are Creative's:
  `ae9-get-firmware` installs them from Creative's installer (step 1).
  Works on speakers and headphones, across sleep and reboots. With Direct
  Mode off, a new stream after boot, wake or a switch made while stopped
  plays direct for 3 s (`dsp_switch_ms`), then moves to the DSP. Turning
  Direct Mode on restarts a stream that was going through the DSP by itself
  (0.9.4); one still waiting to move there (a 44.1 kHz stream stays direct)
  keeps playing (0.10.0).
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

`sudo ae9-full-dump` (from a source checkout: `sudo
scripts/ae9-full-dump.sh`) saves everything the driver can read about the
card (streams, controls, PipeWire, registers, the DSP's router, the
driver's recent log) to `ae9-dump-<time>.txt` in the current directory,
without changing anything. Stop all audio first: while something plays it
skips the chip readings, which can freeze the audio during playback. Take
one while the problem is happening (and one when it's fine, if you can)
and attach them to your report. It only names the apps that are playing;
look it over before posting.

Two more tools for volunteers (packaged since 0.11.2, read-only):

- `sudo ae9-dsp-snapshot > snap.txt` samples the DSP's control and DMA
  registers a few times and marks the ones that move: a working DSP's do,
  a stuck one's may not. Most useful with music playing in DSP mode
  (Direct Mode off): one on a good boot, one while the audio is broken,
  before you power off.
- `sudo ae9-read-stress > stress.txt` reads one chip register 300 times
  and counts the answers: one value means clean reads, several mean some
  came back wrong on your card.

Both refuse to run while audio plays unless you add `--playing`: reading
the card's chip during playback can freeze the audio until a full power-off
(PSU switch; a reboot isn't enough). Only add it if that's OK.

### Known issues

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
  brings it back. Cause inside the card not found yet; if it happens, run
  `scripts/ae9-full-dump.sh` while it's silent: its I2S and power register
  section shows whether the card's output setup has moved.
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
| `creative_ae9_pcm.c` | ALSA PCM playback, XLR capture, optical out (device 4, or a mirror of playback), optical in (device 1) and What U Hear (device 2) |
| `creative_ae9_fx.c` | SBX effects, EQ, Direct Mode and SBX controls |
| `creative_ae9.h`, `ca0132_regs.h` | Registers, chip struct, MMIO accessors; definitions ported from the in-tree ca0132 driver |
| `creative_ae9_*_verbs.h`, `creative_ae9_switch_ops.h` | Sequences recorded from the Windows driver (init, cold boot, warm reset, recording, output switch, shutdown), auto-generated |
| `extract_firmware.py`, `extract_overlay.py` | Extract the DSP firmware and overlay from Creative's `CtxHda.sys` |
| `extract_hpeq.py` | Extracts the headphone profiles from Creative's installer (`AE9HPEQ/*.reg`) |
| `config/` | Boot binding (udev rule, modprobe hook, dracut omit) and optional module options |
| `scripts/restore-stock-audio.sh` | Remove the driver and restore stock `snd_hda_intel` |
| `config/60-ae9-headroom.conf` | Optional PipeWire headroom for small quantums (see "Low latency") |
| `config/ae9-optical-in.conf`, `config/ae9-what-u-hear.conf`, `config/ae9-rear-rca-in.conf` | 0.10.x only: hand-copied PipeWire sources for optical in, What U Hear and the rear RCA input. Since 0.11.0 the card profile opens them; don't copy these any more |
| `config/61-ae9-names.conf` | Optional desktop names: "AE-9 Head Unit Mic / Aux", "AE-9 Rear RCA In", "AE-9 What U Hear" (stereo and 5.1), "AE-9 Optical In", "AE-9 Optical Out", "AE-9 Analog Out" (the main output) |
| `config/50-ae9-safe-volume.conf` | Installed by the packages (WirePlumber): the AE-9's outputs start at -36 dB, the driver's boot volume, when WirePlumber has no saved volume for them (100% is the head unit at 0.0 dB) |

## Module parameters

The bring-up follows Windows' cold boot with no options needed. Since 0.9.7
the old "go back to our earlier code" switches are gone (see the changelog);
what's left is tuning and diagnostics:

- `enable_pcm=1`, `load_fw=1` — PCM device and DSP firmware.
- `bus_reset` — secondary bus reset at probe (codec SSIDs only read
  0x11020071/72 after one; a VM start does the same).
- `win_reset2`, `scp_event` (both 1) — Windows' second controller reset and
  announced DSP replies; kept switchable for cards behind other bridges.
- `sentinel_clear` (0) — clear the DSP's 0x0bad0add sentinel area.
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
  `pcm_176k4` (1) offers 176.4 kHz with them (0.9.7).
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
  `hu_fast_ms` (40) knob poll interval; `hu_cmd_settle_us` (4000, writable at
  runtime) CA0113 settle time for volume changes;
  `acm_heartbeat_ms` (500) head-unit heartbeat (0 = off).
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
  Playback Switch") optical out on; 0 keeps it silent.
  `optical_mirror` (0, writable at runtime; also the "Optical Out Mirror
  Switch") 1 = optical plays the main output while nothing plays to
  optical out (device 4), as before 0.12.1.

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
- **Kaiba**: tested the ASMedia-bridge AE-9, round after round, until it
  played (0.9.8).
- **DarkNikGr** also found that the ASMedia bridge locks up after a burst
  of register writes with no read in between: the key to the ASMedia fix.

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
