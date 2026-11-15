# Installing from source

For distributions without a package, or to run a build of your own. The
packages (COPR, PPA, .deb) do all of this for you; see the
[README](../README.md#install). One install method per machine (see
[below](#one-install-method-per-machine)).

1. [Firmware](#1-firmware)
2. [Build and install the driver](#2-build-and-install-the-driver)
3. [Hand the card to this driver](#3-hand-the-card-to-this-driver)
4. [PipeWire profiles](#4-pipewire-profiles-stereo-and-51)
5. [Testing a new build](#testing-a-new-build)

Reboot after setup. A full power-off (PSU off) is the safest first start.

## 1. Firmware

Same as with the packages: `sudo scripts/ae9-get-firmware.sh
~/Downloads/AECMDMasterInstaller_3.4.92.00.exe` from this repository (see the
[README](../README.md#firmware)).

By hand, from `CtxHda.sys` (`AESeriesDriver/Driver/AMD64/CtxHda.sys` in
Creative's installer; a Windows install has it as
`C:\Windows\System32\drivers\CtxHda.sys`):

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

## 2. Build and install the driver

**With DKMS** (recommended): new kernels get the driver rebuilt
automatically, nothing to redo after a kernel update.

```sh
sudo dnf install dkms kernel-devel     # once (Fedora; Debian/Ubuntu: sudo apt install dkms linux-headers-$(uname -r); Arch: sudo pacman -S dkms linux-headers)
sudo scripts/dkms-install.sh
```

Run the script again after updating the driver source (it replaces the old
version). It also:

- puts the command-line tools (`ae9-reload`, `ae9-hp-profile`,
  `ae9-full-dump`, `ae9-get-firmware` and the others the packages install) in
  `/usr/local/bin`; SbAe9-SBC's "Make custom profile…" needs `ae9-hp-profile`;
- installs `/etc/modules-load.d/creative-ae9.conf` (since 0.12.10), so the
  driver loads early at boot as with the packages (an existing file is left
  alone).

`sudo scripts/dkms-install.sh --remove` takes it all out of DKMS.
`dkms status` shows what's built for which kernel, and
`scripts/dkms-install.sh --check` (no root needed) shows what's installed for
the running kernel, every copy of the module, which one loads and whether it
loads early.

**By hand:**

```sh
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
sudo cp snd-hda-codec-creative-ae9.ko /lib/modules/$(uname -r)/extra/ && sudo depmod -a
```

Redo both after every code change and kernel update. For the tools:
`sudo install -m755 scripts/ae9-hp-profile /usr/local/bin/` (and the same for
`scripts/ae9-reload.sh` as `ae9-reload`).

### One install method per machine

By hand, DKMS and a packaged build (akmod/kmod) each put their own copy of
the module under `/lib/modules`; with two of them it's unclear which one
loads, and updating one can leave the other's old copy in charge. To switch:

- by hand to DKMS: just run `sudo scripts/dkms-install.sh` (it deletes the
  hand-copied module first);
- DKMS to by hand: `sudo scripts/dkms-install.sh --remove`, then copy;
- to or from a package: remove the other method first (the DKMS script
  refuses to run next to an installed package: the Fedora `akmod-`/`kmod-`
  ones or `creative-ae9-dkms` from the .deb, PPA, AUR or COPR).

## 3. Hand the card to this driver

`snd_hda_intel` would grab the card first, and it can't be blacklisted on
most systems because it also drives GPU HDMI audio. The files in `config/`
hand this one card to our driver instead, and load it early:

```sh
sudo cp config/00-ae9-override.rules /etc/udev/rules.d/   # driver_override at device add
sudo cp config/ae9-override.conf /etc/modprobe.d/         # same, if snd_hda_intel loads first
sudo cp config/ae9-omit.conf /etc/dracut.conf.d/          # keep our module out of the initramfs
sudo cp config/ae9-options.conf /etc/modprobe.d/          # optional: debug/tuning options (all commented out)
sudo cp config/ae9-modules-load.conf /etc/modules-load.d/creative-ae9.conf  # load early at boot (dkms-install.sh does this one)
sudo dracut -f
```

- The early load matters: loaded by udev alone, the DSP's first start failed
  on 11 of 26 cold boots (0 of 150 loaded early).
- The override files find the card by its IDs, in any slot.
- The module must load from the root filesystem (after `/lib/firmware` is
  available), which is what the dracut omit is for.

## 4. PipeWire profiles (stereo and 5.1)

Without this PipeWire only gives the card its stereo fallback profiles. The
profile set adds "Analog Stereo Duplex" (the default) and "Analog Surround
5.1 Output":

```sh
sudo cp config/creative-ae9.conf /usr/share/alsa-card-profile/mixer/profile-sets/
sudo cp config/89-ae9-pipewire.rules /etc/udev/rules.d/
```

The packages also start the AE-9's outputs at -36 dB when WirePlumber has no
saved volume for them: `config/50-ae9-safe-volume.conf` (WirePlumber 0.5,
into `/usr/share/wireplumber/wireplumber.conf.d/`) or
`config/51-ae9-safe-volume.lua` (WirePlumber 0.4's `main.lua.d`).

`scripts/restore-stock-audio.sh` removes the driver and restores stock
`snd_hda_intel` (and removes these files too).

## Testing a new build

The bus reset at every load (`bus_reset`, on by default) makes a module
reload behave close to a cold start, which is good for a quick check of a new
build. It can disrupt streams, though, so reboot for good measure before
trusting the results.

`ae9-reload` reloads in one go: it stops PipeWire, WirePlumber, the AE-9
helper services and alsa-state, reloads the driver, waits for the card and
starts them again; anything else still holding the card is listed
(`ae9-reload --check` only shows it). If the card's DSP won't start again, it
says so: only a power-off helps then. By hand:

```sh
systemctl --user stop pipewire.socket pipewire-pulse.socket pipewire pipewire-pulse wireplumber
sudo systemctl stop alsa-state          # alsactl holds the control device
sudo modprobe -r snd-hda-codec-creative-ae9 && sudo modprobe snd-hda-codec-creative-ae9
sleep 25; speaker-test -D hw:AE9 -c 2 -r 48000 -F S32_LE -t sine -l 2
sudo systemctl start alsa-state; systemctl --user start pipewire.socket pipewire-pulse.socket wireplumber
```

Building against the oldest supported kernel matters: the code is tested on
Ubuntu 24.04's 6.8 up to 7.3. The development notes are in
[DEVELOPING.md](DEVELOPING.md).
