# Creative Sound Blaster AE-9 — Linux Driver (WIP)

Sound Blaster AE-9 Linux support: 100% of the features, 100 % of the Audio Control Module's, and some things Linux has that Windows doesn't. :)

An out-of-tree Linux kernel driver for the **Creative Sound Blaster AE-9**
(CA0132 "Sound Core3D" DSP + its detachable ACM head unit). The kernel's own
`snd_hda_intel` + `snd_hda_codec_ca0132` drivers don't support this card;
this one does, by doing what Creative's Windows driver does, step by step.
Since v0.13.0 it also runs the **Sound Blaster AE-7**, experimental and
opt-in.

**Latest: v0.13.0** (2026-11-25) · [Changelog](CHANGELOG.md) ·
[Releases](https://github.com/3xHelix/SbAe9-Drv/releases) ·
Pre-releases, tested on two AE-9s and one AE-7.

## New in v0.13.0: Sound Blaster AE-7 (experimental)

The AE-7 runs on this driver now: **experimental, opt-in, and tested on one
card so far** ([s3boun3t](https://github.com/s3boun3t)'s, through eleven
rounds of test kits). On that card:

- **Playback** on the headphones and the Front jack, DSP mode at 48 kHz, and
  **Direct Mode** straight to the DAC at 44.1, 48, 88.2, 96, 192 and
  384 kHz.
- **A 5.1 stream and Speaker Config.** The front pair is proven and the
  subwoofer channel was felt through the C/Sub jack; the Rear jack and the
  center channel haven't been confirmed by ear yet.
- **SBX, the EQ, Scout Mode, headphone virtualization and CrystalVoice**,
  with the AE-9's controls (on the AE-7 the headphone virtualization switch
  stays on; SBX Surround off or Direct Mode give headphones without it, as in
  Creative's software).
- **The mic** (44.1/48/96 kHz), **What U Hear**, jack detection, a DAC that
  unmutes only after the volume reads back, and clean reloads, reboots and
  suspend.

**A huge thank-you to [s3boun3t](https://github.com/s3boun3t)**, who did the
heavy lifting for the AE-7: his own early AE-7 work in his fork, the Windows
captures of his card, and eleven rounds of test kits and three release
candidates run on it, each sent back with careful logs and reports (he
diagnosed the DKMS bug himself, and made his own subwoofer test file to
prove the C/Sub jack). This release is his as much as ours. 🙌

Not yet: Line-In mode, mic monitor, optical out, the LED and DSD64. The
driver leaves an AE-7 alone until you turn it on:

```sh
sudo ae9-ae7-enable      # then reboot
```

**Careful:** the kernel's own driver has hung the PC at boot on every AE-7
with an ASMedia bridge reported so far (most cards from 2022 on).
[docs/AE7.md](docs/AE7.md#the-asmedia-bridge-and-the-kernels-driver) says
how to check yours and how to boot a PC that hangs. Turning the AE-7
support off leaves such a card (or one with a bridge the script doesn't
know) with no driver instead of handing it back.

Everything else (firmware, checking it works, going back, what to send
when it doesn't): **[docs/AE7.md](docs/AE7.md)**. AE-7 reports are very
welcome.

**For AE-9 owners:** nothing to do. Every card now has a read-only "Card
Model" control (the card's name, for apps), and the driver has a new `ae7`
option the AE-9 ignores. One Creative card per PC is the supported setup.

## 💣 Native DSD64, on by default

**Native DSD. On Linux. On a PCIe sound card.** As far as we can tell,
nobody's done that before, and since v0.12.15 it's simply on.

Your `.dsf` files go bit-for-bit, a 1-bit stream at 2.8224 MHz, straight
into the AE-9's ES9038 DSD decoder. No PCM conversion, no DoP, no
resampling. It's the same lane Creative's own Windows DSD driver uses: we
watched what their driver does to the card, decoded it, found the hidden
path past the DSP, and lit it up. The DAC itself signs off on it: DSD decoder on, locked, 2.8224 MHz.

Point MPD (Cantata, ncmpcpp, ...) at `hw:AE9,0` with `dop "no"` and hit
play. How-to: [docs/USING.md](docs/USING.md#native-dsd64). Don't want it?
`pcm_dsd=0`, or the switch in
[SbAe9-SBC](https://github.com/3xHelix/SbAe9-SBC) (v0.22.1 or later). Turned
it off with SBC v0.22.0? Turn it off once more: with the 0.12.15 default
it's back on.

DSD128? The card can't clock it, we tried every setting there is. That's why
Creative stops at DSD64 too. Those files still play, as PCM.

## What works

- **Playback** on the card's RCA outputs (speakers) and the head unit's
  headphone jacks: 44.1 to 384 kHz, 24- or 32-bit, hardware volume synced
  with the knob and the desktop slider.
- **Native DSD64** (on by default): `.dsf`/`.dff` straight to the DAC's
  DSD decoder through MPD, the way Creative's Windows DSD driver does it
  ([USING.md](docs/USING.md#native-dsd64)).
- **5.1 surround** on the card's RCA, Rear and C/Sub jacks.
- **The DSP:** SBX (Surround, Dialog Plus, Smart Volume, Crystalizer, Bass),
  the EQ with Windows' 20 presets, Scout Mode, speaker setup (bass redirect,
  per-speaker level and distance), Sound Blaster Command's 32 headphone
  profiles plus your own, and headphone virtualization ("Virtual 7.1").
- **Recording:** the head unit's XLR/TRS and 3.5 mm mic input (48 or 96 kHz),
  with Voice Clarity, Voice Morphing and zero-delay mic monitoring; the head
  unit's rear RCA input; optical in; What U Hear (what the card plays).
- **Optical (S/PDIF) out** as its own output, up to 24-bit/192 kHz, with
  Dolby Digital / DTS passthrough to a receiver.
- **The head unit:** display (with the "AE-9" start-up screen), volume knob,
  mute, speakers/headphones switch, knob light, SBX button, 48 V phantom
  power, headphone/mic plug detection.
- **The rest:** sleep and resume, reboots and driver reloads, both versions
  of the card (Tundra Tsi381 or ASMedia bridge), no module options needed.

## Apps

- **[SbAe9-SBC](https://github.com/3xHelix/SbAe9-SBC)**: a Sound Blaster
  Command-style control panel for this driver: SBX, the EQ and its presets,
  Scout Mode, Direct Mode, outputs, recording and the mixer, all in one
  window.
- **[SbAe9-AudioMon](https://github.com/3xHelix/SbAe9-AudioMon)**: level
  meters for every AE-9 input and output, with a debug log.

Driver v0.13.0 works with **SbAe9-SBC v0.24.0** and
**SbAe9-AudioMon v0.5.0**. Older app versions still
work on the AE-9, with fewer controls. Earlier pairings are in the
[changelog](CHANGELOG.md).

## Hardware

- Creative Sound Blaster AE-9: PCI `1102:0010`, subsystem `0x0071`, with the
  ACM head unit connected.
- Both versions of the card work: with an IDT/Tundra Tsi381 (`10e3:8111`) or
  an ASMedia ASM1083/1085 (`1b21:1080`) PCIe-to-PCI bridge. The driver spots
  the ASMedia bridge and switches its workarounds on by itself.
- Tested on Fedora 44 (kernels 7.2 and 7.3) on an AMD system, and the
  packages on Ubuntu 24.04 to 26.10, Debian 13, openSUSE and AlmaLinux 10
  (kernels 6.8 to 7.3). No `iommu=pt` needed.
- **Sound Blaster AE-7** (experimental, opt-in): PCI `1102:0010`, subsystem
  `0x0081`. Tested on one card (ASMedia bridge) on Linux Mint 22 with a
  XanMod 7.3 kernel. See [docs/AE7.md](docs/AE7.md).

To see which bridge your card has:

```sh
lspci -nn -s $(basename $(dirname $(readlink -f /sys/bus/pci/devices/$(lspci -D -d 1102:0010 | cut -d' ' -f1))))
```

## Install

Pick your distribution, then install the [firmware](#firmware) and **power
the PC fully off and on** (PSU switch or unplug, 15-30 s; a reboot isn't
enough for the first start).

### Fedora: COPR (recommended)

```sh
sudo dnf copr enable 3xhelix/SbAe9-Drv
sudo dnf install creative-ae9
```

akmods builds the module for every new kernel; the first build takes about a
minute (`modinfo -n snd-hda-codec-creative-ae9` shows it once it's there).

- **Fedora Atomic** (Silverblue, Kinoite, ...):
  ```sh
  sudo curl -o /etc/yum.repos.d/SbAe9-Drv.repo https://copr.fedorainfracloud.org/coprs/3xhelix/SbAe9-Drv/repo/fedora-$(rpm -E %fedora)/3xhelix-SbAe9-Drv-fedora-$(rpm -E %fedora).repo
  rpm-ostree install creative-ae9
  ```
  Reboot into the new deployment, install the firmware, reboot once more.
- **Secure Boot:** enroll akmods' key once with
  `sudo mokutil --import /etc/pki/akmods/certs/public_key.der`, reboot and
  confirm in the blue MOK screen.
- **Remove:** `sudo dnf remove creative-ae9 akmod-creative-ae9`.

### Ubuntu, Linux Mint, Pop!_OS: PPA

For Ubuntu 24.04, 26.04 and 26.10 and the desktops built on them:

```sh
sudo add-apt-repository ppa:3xhelix/sbae9
sudo apt install creative-ae9-dkms innoextract
```

Ubuntu asks you to set up Secure Boot signing during the install (a password,
then the blue MOK screen at the next boot).

### Debian: the .deb

Download `creative-ae9-dkms_<version>-1_all.deb` from the
[latest release](https://github.com/3xHelix/SbAe9-Drv/releases), then:

```sh
sudo apt install ./creative-ae9-dkms_0.13.0-1_all.deb innoextract
```

Secure Boot: `sudo mokutil --import /var/lib/dkms/mok.pub` once, then
reboot. Remove: `sudo apt remove creative-ae9-dkms`.

### openSUSE, RHEL / AlmaLinux / Rocky 10: COPR

```sh
# openSUSE Tumbleweed (Leap 16: write opensuse-leap-16.0 in both places of the URL)
sudo zypper addrepo https://copr.fedorainfracloud.org/coprs/3xhelix/SbAe9-Drv/repo/opensuse-tumbleweed/3xhelix-SbAe9-Drv-opensuse-tumbleweed.repo
sudo zypper --gpg-auto-import-keys install kernel-default-devel innoextract creative-ae9-dkms

# RHEL / AlmaLinux / Rocky 10
sudo dnf install epel-release dnf-plugins-core
sudo dnf copr enable 3xhelix/SbAe9-Drv
sudo dnf install kernel-devel innoextract creative-ae9-dkms
```

Each release also has `creative-ae9-dkms-<version>-1.noarch.rpm` on its
[release page](https://github.com/3xHelix/SbAe9-Drv/releases) (no automatic
updates then). On Fedora use the COPR packages above instead.

### Other distributions, or from source

DKMS and a few config files: see
**[docs/INSTALL-FROM-SOURCE.md](docs/INSTALL-FROM-SOURCE.md)**. Use one
install method per machine: remove a package before a source install, and the
other way round.

## Firmware

The DSP firmware is Creative's, so it isn't in this repository or the
packages. `ae9-get-firmware` takes it from Creative's own installer for the
AE-9, **Sound Blaster Command + driver** (`AECMDMasterInstaller_3.4.92.00.exe`,
from Creative's AE-9 support page). The installer is only unpacked, never
run; it needs `innoextract`.

```sh
sudo ae9-get-firmware ~/Downloads/AECMDMasterInstaller_3.4.92.00.exe
```

It installs the DSP image and the AE-9 overlay (checked against the size and
SHA-256 of the firmware this driver is tested with: `90ba1de2…` and `ecc0740b…`) plus Sound Blaster Command's 32
headphone profiles. It also takes an unpacked installer folder, the AE-Series
driver installer on its own, or just `CtxHda.sys` from a Windows install.
`ae9-get-firmware --check` shows what's installed. The AE-7 uses the same
files. (The `ctefx-desktop.bin` in linux-firmware is a different image, for
the kernel's own driver.)

## Using it

The card starts in **Direct Mode** on **speakers** (the card's RCA jacks),
playing straight to the DACs at your music's rate. Then:

| To... | Do this |
|---|---|
| Change the volume / mute | Turn / press the head unit's knob, or use the desktop volume |
| Switch speakers ↔ headphones | Hold the knob 1.5-3 s, or the "Head Unit Output" control |
| Turn the knob light on/off | Hold the knob 3 s or more |
| Use SBX, the EQ, speaker setup, headphone profiles | Turn **Direct Mode** off (needs a 48 or 96 kHz stream), then SBX on: the SBX button or SbAe9-SBC |
| Turn the head unit's display off/on | Hold the SBX button 5 s |
| Record | The AE-9 inputs in PipeWire: mic, Rear RCA In, Optical In, What U Hear |
| Use 5.1 | "Analog Surround 5.1 Output" in the desktop's sound settings |
| Pick a sample rate | PipeWire runs at 48 kHz unless told otherwise; SbAe9-SBC's rate picker, or `pw-metadata -n settings 0 clock.force-rate 96000` |

In the desktop, use the AE-9's "Stereo Output + Stereo Input" profile: the
desktop volume and the knob stay in sync. Every control is also in
`alsamixer -c AE9`.

**All features in detail** (each control, rates, recording devices, Voice
Clarity, optical, low latency): **[docs/USING.md](docs/USING.md)**.

## If something goes wrong

- **Reload the driver:** `ae9-reload`. It stops what holds the card, reloads
  the driver and starts everything again.
- **Power off fully** if it says the card didn't start, or the card is dark:
  PSU off (or unplug) for 15-30 s. A warm reboot isn't enough.
- **Report it:** `ae9-full-dump --report` saves a read-only report
  (in `~/ae9-dumps/`) and one `.tar.gz` of it to attach to
  an [issue](https://github.com/3xHelix/SbAe9-Drv/issues). Stop the audio
  first.

Known issues and more help: **[docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)**.

## More docs

- [docs/USING.md](docs/USING.md): every feature and control.
- [docs/AE7.md](docs/AE7.md): the Sound Blaster AE-7 (experimental).
- [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md): quick fixes, reporting a problem, known issues.
- [docs/INSTALL-FROM-SOURCE.md](docs/INSTALL-FROM-SOURCE.md): DKMS/source install, manual firmware, testing a build.
- [docs/OPTIONS.md](docs/OPTIONS.md): module options.
- [docs/DEVELOPING.md](docs/DEVELOPING.md): source layout and where the driver comes from.
- [CHANGELOG.md](CHANGELOG.md): every release.

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
  S/PDIF routing, the stream 0x05 setup, the reverse-engineering notes on
  the head unit's serial link, the DAC filters, and the ASMedia bridge's
  register-write lockup (the key to the ASMedia fix).
- **[s3boun3t](https://github.com/s3boun3t)**
  ([linux-ae9](https://github.com/s3boun3t/linux-ae9), formerly
  [ae9_build](https://github.com/s3boun3t/ae9_build)): the AE-9 MMIO init
  sequence and the head unit (ACM) init, adapted here. Also fixes and
  suggestions adopted into this driver, the routing questions behind the
  +30 dB option, side-by-side comparisons and the PipeWire 1.6.8 find. And
  the AE-7 support: his early AE-7 work in his fork, the Windows captures of
  his AE-7, and the testing of every AE-7 test kit on his card. Without him
  there'd be no AE-7 support.
- **Creative's own CA0132 Linux build** ("0.73_kernel18", GPL), shared with
  the project: the AE-9 output, mic and speaker setup sequences.
- **Kaiba**: tested the ASMedia-bridge AE-9, round after round, until it
  played.
- **arkhodakov**: the head unit's start-up race (PR #3) and the source
  install's early load.

Everything Windows-side (timings, verb order, the head unit protocol) comes
from our own VFIO captures of Creative's Windows driver; none of its code is
included. Each source file names what it adapted, with the original copyright
notices. More in [docs/DEVELOPING.md](docs/DEVELOPING.md).

## License

GPL-2.0-or-later: the GNU GPL version 2, or (at your option) any later
version. It fits in the Linux kernel (GPLv2) and can also be used under
GPLv3. Full text: [COPYING](COPYING). Use it, change it and share it freely;
if you share it, share the source too, under the same terms.
`ca0132_regs.h` comes from the kernel's CA0132 driver unchanged; code
adapted from it and from the projects under Credits keeps their copyright
notices. The DSP firmware is Creative's and is not part of this repository:
`ae9-get-firmware` takes it from your own copy of Creative's installer.

## Disclaimer

Experimental and reverse-engineered. Use at your own risk. Not affiliated
with Creative.
