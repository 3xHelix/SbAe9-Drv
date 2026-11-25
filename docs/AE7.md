# Sound Blaster AE-7 (experimental)

Since 0.13.0 the driver also runs the **Creative Sound Blaster AE-7** (PCI
`1102:0010`, subsystem `1102:0081`). Back to the [README](../README.md).

It's **experimental and opt-in**: the driver leaves an AE-7 alone until you
turn it on. So far it has been tested on **one card**, by
[s3boun3t](https://github.com/s3boun3t), through eleven rounds of test kits.
If you have an AE-7, reports are very welcome, good or bad
([issues](https://github.com/3xHelix/SbAe9-Drv/issues)).

The AE-7 has the same DSP chip as the AE-9 and uses the same firmware. Its
DAC and its Audio Control Module are different, so it has its own start-up,
output and volume code, written from what Creative's Windows driver does on
an AE-7.

- [What works](#what-works)
- [Not yet](#not-yet)
- [The ASMedia bridge and the kernel's driver](#the-asmedia-bridge-and-the-kernels-driver)
- [Before the first start](#before-the-first-start)
- [Turning it on](#turning-it-on)
- [Checking that it works](#checking-that-it-works)
- [Coming from a test kit](#coming-from-a-test-kit)
- [Going back](#going-back)
- [Reporting a problem](#reporting-a-problem)
- [Known limits](#known-limits)

## What works

On the tester's card, in 0.13.0:

- **Playback** on the headphones (the Audio Control Module) and the Front
  jack (line out). "Head Unit Output" picks one: Headphones or Speakers
  (the Front jack). Each output keeps its own volume.
- **DSP mode** (the start-up default, "Direct Mode Switch" off) at 48 kHz,
  stereo or 6 channels.
- **Direct Mode** ("Direct Mode Switch" on): straight to the DAC at 44.1,
  48, 88.2, 96, 192 and 384 kHz, stereo. 176.4 kHz isn't offered (Windows
  refuses it on the AE-7 too).
- **A 6-channel (5.1) stream and "Speaker Config"** (2.0, 2.1, 4.0, 4.1,
  5.1), in DSP mode at 48 kHz. The front pair is proven, and the
  subwoofer (LFE) channel was felt on a 5.1 speaker set through the C/Sub
  jack; **the Rear jack and the C/Sub jack's center channel haven't been
  confirmed by ear yet**.
- **Volume:** "Master Playback Volume", 0 to 180 = -90 to 0 dB in 0.5 dB
  steps, is the DAC's own volume, and the desktop slider drives it.
  "Headphone Gain" (Low, Normal, High) and "DAC Filter" (Fast Roll Off,
  Slow Roll Off, Minimum Phase).
- **A safety gate:** the DAC is unmuted only after the volume it was sent
  reads back on both channels.
- **Recording** from the Line-in / Mic-in jack (where the Audio Control
  Module's mic lead goes) at 44.1, 48 and 96 kHz, with "Capture Volume" and
  "Mic Gain Capture Volume" (0, +10, +20 dB).
- **What U Hear** (what the card plays) at 44.1 and 48 kHz.
- **SBX** (Surround, Crystalizer, Bass, Dialog Plus, Smart Volume), the EQ,
  Scout Mode and headphone virtualization, with Direct Mode off, with the
  same controls as on the AE-9. **On the AE-7, "Headphone Virtualization
  Switch" stays on** (setting it off changes nothing; a mixer snaps back to
  on). To listen on headphones without virtualization, turn **SBX Surround**
  off, or use **Direct Mode**, which skips the DSP: the ways Creative's
  software offers on the AE-7. The separate switch put the card in a state
  no Windows capture showed.
- **CrystalVoice:** Voice Clarity, echo cancellation, noise reduction, mic
  Smart Volume, Mic EQ and Voice Morphing, with the AE-9's controls.
- **Jack detection** for the Front, Rear, Headphone / Headset-out and
  Line-in / Mic-in jacks: read-only "Card ... Plugged" controls and one
  kernel log line per plug. A plug changes nothing else (no output switch,
  no volume change).
- **Reloads, warm reboots and suspend**, including Windows' recovery cycle
  when the card wakes up in a bad state.
- "Card Answering", "Driver Health" and "Card Model", as on the AE-9.

The AE-7's PipeWire profile (installed with the packages) gives the desktop
the stereo output, the 5.1 output (with Direct Mode off), the mic and What U
Hear.

## Not yet

- **Line-In mode:** the Line-in / Mic-in jack records as a mic only.
- **Mic monitor**, **optical out**, **the LED**, **native DSD64**.
- **The Rear jack and the C/Sub jack's center channel confirmed by ear**
  (see above).
- **Bass Redirect and the speakers' levels and distances** (the AE-9's
  speaker setup).
- **DSP mode at rates other than 48 kHz.** PipeWire plays at 48 kHz unless
  told otherwise, so this only matters with a forced rate: use Direct Mode
  for 44.1 to 384 kHz.

## The ASMedia bridge and the kernel's driver

**The kernel's own driver (`snd_hda_intel` with its ca0132 driver) has
hung the PC at boot on every AE-7 with an ASMedia PCIe bridge reported so
far** (ASM1083/1085, `1b21:1080`): most AE-7s from 2022 on. Not just the
sound: the whole PC freezes, and the log can show
`Watchdog detected hard LOCKUP` with `snd_hda_intel` in it. AE-7s with the
older IDT/Tundra Tsi381 bridge (`10e3:8111`) are reported to start with
that driver (not tried with this project). The script's messages call it
"the in-tree driver".

To see which bridge your AE-7 has:

- `ae9-ae7-enable --status` names it, for example
  `bridge: 0000:06:00.0, 1b21:1080 (ASMedia)`.
- Or, without the driver, this prints the bridge right above each AE-7:
  `[1b21:1080]` is ASMedia, `[10e3:8111]` is Tsi381 (bash, sh or dash;
  no output means no AE-7 was found).
  ```sh
  for d in /sys/bus/pci/devices/*; do [ "$(cat $d/vendor):$(cat $d/device):$(cat $d/subsystem_device)" = 0x1102:0x0010:0x0081 ] && lspci -nn -s $(basename $(dirname $(readlink -f $d))); done
  ```
  By hand: `lspci -PP -nn -d 1102:0010` shows the path to the card (an
  AE-9 shows there too), for example `00:1c.0/06:00.0/07:00.0`. The address
  before the card's own is its bridge: `lspci -nn -s 06:00.0` shows its
  IDs.

So turning the AE-7 support off never hands an AE-7 with an ASMedia
bridge, or a bridge the script doesn't know, back to the kernel's driver:
it **parks** the card instead, and the card gets no driver at all
([Going back](#going-back)). Installing the driver alone changes nothing:
until you run `ae9-ae7-enable`, the AE-7 stays with the kernel's driver.

**If the PC already hangs at boot:**

1. At the GRUB menu (if it doesn't show, hold Shift or press Esc while the
   PC starts), press `e`, add `module_blacklist=snd_hda_intel` at the end of
   the line that starts with `linux`, and boot with Ctrl+X or F10. With
   systemd-boot (Pop!_OS), press `e` on the entry, add the same word at the
   end, and press Enter.
2. That lasts for that one boot, and it also turns off every other HD-Audio
   device for it (the onboard sound, a graphics card's HDMI sound).
3. Then install the driver and turn the AE-7 on (`sudo ae9-ae7-enable`,
   below), or park it (`sudo ae9-ae7-enable --disable`), and reboot
   normally. Parking needs no driver: from a copy of the source,
   `sudo bash scripts/ae9-ae7-enable --disable` writes the park files too.

To set up before fitting a new AE-7: install the driver and the firmware,
run `sudo ae9-ae7-enable --force` (it installs the files without an AE-7 in
the PC), shut down, and fit the card. Otherwise `--disable` parks only a
card it can see, or one set up here with `--force`.

## Before the first start

The AE-7's outputs can drive headphones loudly. Until you've heard it play
once:

- headphones off your head, and the speakers or amp on the Front jack off
  or at their lowest;
- **nothing in the Rear or C/Sub jacks**, and never headphones or earbuds
  there: which channel they play hasn't been confirmed yet.

With the packages, PipeWire starts the AE-7's outputs at -36 dB when it has
no saved volume for them. Turn up slowly from there.

## Turning it on

1. **Install the driver** as in the [README](../README.md#install) (a
   package, or [from source](INSTALL-FROM-SOURCE.md)).
2. **Install the firmware.** The AE-7 uses the AE-9's DSP firmware, so the
   steps are the same as in the [README](../README.md#firmware):
   ```sh
   sudo ae9-get-firmware ~/Downloads/AECMDMasterInstaller_3.4.92.00.exe
   ```
   Creative's installer for the AE-9 is a free download from Creative's AE-9
   support page. If you have Windows with the AE-7's driver installed,
   `ae9-get-firmware` also takes its `CtxHda.sys`
   (`C:\Windows\System32\drivers\CtxHda.sys`). Either way, the image is
   checked against the one this driver is tested with; if it says your
   `CtxHda.sys` is a different one, use the AE-9 installer instead.
   `ae9-get-firmware --check` must say `ok` for both files. (Don't use the
   `ctefx-desktop.bin` from linux-firmware: it's a different image, for the
   kernel's own driver.)
3. **Turn the AE-7 on:**
   ```sh
   sudo ae9-ae7-enable
   ```
   It copies two files into place, `/etc/udev/rules.d/00-ae7-override.rules`
   (hands the AE-7 to this driver) and `/etc/modprobe.d/ae7-override.conf`
   (sets the driver's `ae7=1` option, and hands the card over if
   `snd_hda_intel` loads first), removes the park files if the card was
   parked, and reloads udev. Running it again changes nothing. It refuses
   to run if there's no AE-7 in the PC (`--force` installs anyway).
4. **Reboot.** A full power-off (PSU off or unplugged for 15-30 s) is the
   safest first start, as for the AE-9.

Without `ae7=1` the driver refuses an AE-7 that's handed to it, with one line
in the kernel log (see
[TROUBLESHOOTING.md](TROUBLESHOOTING.md#quick-fixes)).

## Checking that it works

- `aplay -l` lists a card `AE7` ("Sound Blaster AE-7").
- `sudo dmesg | grep AE-7` has, within a few seconds of the start, a line
  like
  ```
  AE-7: DAC unmuted (volume 0x48 read back on both channels)
  ```
  (the number is the volume: 0x48 is -36 dB).
- `ae9-ae7-enable --status` shows what's installed, whether an AE-7 is in
  the PC, its bridge and whether this driver has it.
- `amixer -c AE7 cget iface=CARD,name='Card Model'` says
  `Sound Blaster AE-7`.
- `amixer -c AE7 cget name='Driver Health'`: the first five numbers and the
  ninth should be 0 (see [USING.md](USING.md#sleep-reboots-and-reloads)).
- Every control is in `alsamixer -c AE7`.

**SbAe9-SBC v0.24.0** shows the AE-7's controls in its window; older
versions are made for the AE-9.

## Coming from a test kit

If you ran one of the AE-7 test kits:

1. Install the driver and the firmware, as above (steps 1 and 2).
2. Remove the kit's files **before** turning the AE-7 on (`ae9-ae7-enable`
   refuses while they're still there):
   ```sh
   sudo ae9-ae7-enable --remove-kit-files
   ```
   It finds the kit's files (`/etc/modprobe.d/ae7-kit.conf`, the
   `ae7-kit` udev rules, the boot-load file and the kit's safe-volume files)
   and removes them. It also offers to remove the kit's hand-copied module
   in `/lib/modules/$(uname -r)/updates/` and runs `depmod`. Afterwards
   `modinfo -n snd-hda-codec-creative-ae9` should point at the package's or
   DKMS's copy (one install method per machine:
   [INSTALL-FROM-SOURCE.md](INSTALL-FROM-SOURCE.md#one-install-method-per-machine)).
3. Turn the AE-7 on: `sudo ae9-ae7-enable` (step 3 above).
4. Reboot.

A kit's `ae7=2` to `ae7=5` left somewhere still works: 0.13.0 treats it as
`ae7=1`, with one line in the kernel log.

## Going back

- **Turn the AE-7 support off:** `sudo ae9-ae7-enable --disable`, then
  reboot. What happens next depends on the card's bridge
  ([above](#the-asmedia-bridge-and-the-kernels-driver)):
  - **ASMedia, or a bridge the script doesn't know:** the AE-7 is
    **parked**. No driver takes it, so it plays nothing, and the kernel's
    driver can't hang the PC. Two files keep it that way,
    `/etc/udev/rules.d/00-ae7-park.rules` and
    `/etc/modprobe.d/ae7-park.conf`: they hand the card to this driver,
    which leaves it alone without `ae7=1` (the "experimental support is
    off" line in the kernel log, if it shows up, is expected). With the
    driver removed, nothing takes the card at all.
  - **Tsi381:** the AE-7 goes back to `snd_hda_intel` with the kernel's
    ca0132 driver, which has AE-7 support of its own.
  - **No AE-7 to be seen, but the support is on** (set up with `--force`,
    or the card is out of the PC for now): parked too, to be safe.
- **To the kernel's own driver anyway:** `sudo ae9-ae7-enable --disable
  --stock`, then reboot. Use it for a Tsi381 card the script didn't
  recognise, or a kernel known to run your card. On an AE-7 with an ASMedia
  bridge this hangs the PC at boot.
- **Remove the whole driver:** remove the package (see the
  [README](../README.md#install)). With the AE-7 support on, the removal
  turns it off first, as `--disable` does, so an ASMedia AE-7 is parked.
  From a source install, `sudo scripts/dkms-install.sh --remove` does the
  same for the AE-7, and `scripts/restore-stock-audio.sh` removes the
  driver and restores stock `snd_hda_intel`, and also removes the AE-7's
  opt-in files and any test kit leftovers; an ASMedia AE-7 is parked
  (`restore-stock-audio.sh --stock` hands it to the kernel's driver).
- **Un-parking:** the park files are yours, not the package's, so they stay
  when the driver is removed (also with `apt purge`). `sudo ae9-ae7-enable`
  replaces them (this driver runs the AE-7);
  `sudo ae9-ae7-enable --disable --stock` removes them (the kernel's driver
  takes it). Without the driver installed, run
  `sudo bash scripts/ae9-ae7-enable --disable --stock` from a copy of the
  source, or delete them by hand and reboot:
  ```sh
  sudo rm /etc/udev/rules.d/00-ae7-park.rules /etc/modprobe.d/ae7-park.conf
  ```
  Either way the kernel's own driver then takes the AE-7, so do this only
  with a Tsi381 bridge: on an ASMedia card it hangs the PC at boot.

## Reporting a problem

`ae9-full-dump --report` works on the AE-7 too: it saves a read-only report
in `~/ae9-dumps/` and one `.tar.gz` of it to attach to an
[issue](https://github.com/3xHelix/SbAe9-Drv/issues). Stop the audio first.
Put "AE-7" in the issue's title, and say what you did right before it went
wrong, and whether a reload (`ae9-reload`) or a power-off fixed it. More in
[TROUBLESHOOTING.md](TROUBLESHOOTING.md#reporting-a-problem).

## Known limits

- **One Creative card per PC** is the supported setup in 0.13.0. An AE-9 and
  an AE-7 in the same PC haven't been tested together, and some of the
  driver's debug and global state is shared between cards; that's for a
  later release.
- **The Rear jack and the C/Sub jack's center channel** haven't been
  confirmed by ear yet (the subwoofer channel has).
- **No Line-In mode** yet: the Line-in / Mic-in jack is a mic input.
- **The volume jumps for a few seconds at login:** the login screen's own
  PipeWire sets the card's volume to its level, then your desktop restores
  yours. Harmless, and not AE-7 specific
  ([TROUBLESHOOTING.md](TROUBLESHOOTING.md#quick-fixes)).
- **"waiting for the DAC to lock"** in the kernel log at 44.1 or 88.2 kHz in
  Direct Mode is normal: at those rates the DAC takes a moment to lock
  before the driver unmutes it.
- **"Refused to change power state from D0 to D3hot"** at every suspend is
  harmless, as on the AE-9.
- **Tested on:** one AE-7 (board revision 0x09, ASMedia ASM1083/1085 PCIe
  bridge), Linux Mint 22 with a XanMod 7.3 kernel, WirePlumber 0.4.17, with
  the IOMMU on and off. Other distributions, kernels and AE-7s with another
  bridge haven't been tried yet.
- **Parking covers the start-up only.** A parked AE-7 that appears while
  the PC runs (plugged in or re-scanned, for example
  `echo 1 > /sys/bus/pci/rescan`, or in a Thunderbolt PCIe box) is taken by
  `snd_hda_intel` straight away if it's loaded for other sound, before the
  park rule can act. Don't hot-plug or re-scan an ASMedia AE-7;
  `module_blacklist=snd_hda_intel` is the only full guard.
- **Switching between the packages turns the AE-7 support off.** Replacing
  one package with another (for example `dnf swap creative-ae9
  creative-ae9-dkms`) removes the old one, which turns the support off and
  parks an ASMedia AE-7. Run `sudo ae9-ae7-enable` again afterwards.
- **Your own `install snd_hda_intel` line** in `/etc/modprobe.d/` runs only
  from a file whose name sorts before `ae7-park.conf` (for example
  `00-local.conf`); a later one is skipped while the AE-7 is parked. An
  earlier one replaces the park's own `install` line, which leaves only the
  udev rule to keep the AE-7 from `snd_hda_intel`.
