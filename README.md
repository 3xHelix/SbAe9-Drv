# Creative Sound Blaster AE-9 — Linux Driver (WIP)

Sound Blaster AE-9 Linux support: 100% of the features, 100 % of the Audio Control Module's, and some things Linux has that Windows doesn't. :)

An out-of-tree Linux kernel driver for the **Creative Sound Blaster AE-9**
(CA0132 "Sound Core3D" DSP + its detachable ACM head unit). The kernel's own
`snd_hda_intel` + `snd_hda_codec_ca0132` drivers don't support this card;
this one does, by doing what Creative's Windows driver does, step by step.

**Latest: v0.12.12** (2026-11-17) · [Changelog](CHANGELOG.md) ·
[Releases](https://github.com/3xHelix/SbAe9-Drv/releases) ·
Pre-releases, tested on two cards.

## What works

- **Playback** on the card's RCA outputs (speakers) and the head unit's
  headphone jacks: 44.1 to 384 kHz, 24- or 32-bit, hardware volume synced
  with the knob and the desktop slider.
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
  of the card (IDT or ASMedia bridge), no module options needed.

## Apps

- **[SbAe9-SBC](https://github.com/3xHelix/SbAe9-SBC)**: a Sound Blaster
  Command-style control panel for this driver: SBX, the EQ and its presets,
  Scout Mode, Direct Mode, outputs, recording and the mixer, all in one
  window.
- **[SbAe9-AudioMon](https://github.com/3xHelix/SbAe9-AudioMon)**: level
  meters for every AE-9 input and output, with a debug log.

Driver v0.12.12 works with SbAe9-SBC v0.21.3 and SbAe9-AudioMon v0.4.8. Older
app versions still work, with fewer controls. Earlier pairings are in the
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
sudo apt install ./creative-ae9-dkms_0.12.12-1_all.deb innoextract
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

It installs the DSP image and the AE-9 overlay (checked against the MD5 of
the firmware this driver is tested with: `85c8f2b4…` and `0e222c94…`) plus Sound Blaster Command's 32
headphone profiles. It also takes an unpacked installer folder, the AE-Series
driver installer on its own, or just `CtxHda.sys` from a Windows install.
`ae9-get-firmware --check` shows what's installed. (The `ctefx-desktop.bin`
in linux-firmware is a different image, for the AE-5/AE-7.)

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
- **Report it:** `sudo ae9-full-dump` saves a read-only report to attach to
  an [issue](https://github.com/3xHelix/SbAe9-Drv/issues). Stop the audio
  first.

Known issues and more help: **[docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)**.

## More docs

- [docs/USING.md](docs/USING.md): every feature and control.
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
- **[s3boun3t/ae9_build](https://github.com/s3boun3t/ae9_build)**: the AE-9
  MMIO init sequence and the head unit (ACM) init, adapted here.
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
