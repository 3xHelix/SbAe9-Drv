# Troubleshooting

What to try when the card misbehaves, how to report a problem, and the known
issues. Back to the [README](../README.md).

- [Quick fixes](#quick-fixes)
- [Reporting a problem](#reporting-a-problem)
- [Known issues](#known-issues)
- [Fixed in earlier versions](#fixed-in-earlier-versions)

## Quick fixes

1. **Reload the driver:** `ae9-reload` (as yourself; it asks for sudo). It
   stops PipeWire, WirePlumber, the AE-9 helper services and alsa-state,
   reloads the driver, waits for the card and starts everything again. It
   lists anything else still holding the card (`ae9-reload --check` only
   shows that), and it says when the card didn't start. Since 0.12.13 it
   also leaves a notice (`/run/user/<your uid>/ae9-reload`) so Sound Blaster
   Command (v0.21.4+) and AudioMon (v0.4.9+) let go of the card while it
   runs and come back by themselves. Since 0.12.14 it keeps a forced
   PipeWire rate or quantum (`clock.force-rate`, `clock.force-quantum` set
   with `pw-metadata`), which PipeWire's restart used to clear.
2. **Power off fully:** when a reload can't bring the card back ("the card
   didn't start"), or the card is dark (lights off, no head unit, no sound):
   shut down, switch the PSU off (or unplug) for 15-30 s, then boot. A warm
   reboot isn't enough: the card keeps its state while the PC has power.
3. **A short sleep** cuts the card's power too and is worth a try before a
   power-off: `sudo rtcwake -m mem -s 25` (suspend to RAM for 25 s). If it
   doesn't help, check that the sleep happened:
   `journalctl -b | grep -i "aborting suspend"`. Another driver can refuse it
   (seen with NVIDIA's while a model filled the VRAM), and then the card never
   loses power.

**"The card stopped answering":** the driver noticed the card stopped taking
commands (before 0.12.10: seen after toggling SBX, mostly while recording;
0.12.10 fixes the cause found). It stops sending and stops every audio stream; playback,
recording and control changes then fail with "No such device", and SbAe9-SBC
and SbAe9-AudioMon let go of the card and show a notice. Run `ae9-reload`; if that says the card
didn't start, power off.

**No sound from one app while others play:** check whether it has a stream
at all with `pactl list sink-inputs | grep -E "Sink:|application.name"`. Seen
with Chrome Canary after output switches or a reboot (no stream; VLC played
fine); restarting the app fixes it.

**The desktop volume slider doesn't move the head unit's volume** (the
slider changes the level, but the knob, the display and `amixer -c AE9 get
Master` stay put): look for an old WirePlumber rule that sets
`api.alsa.soft-mixer = true`, often left behind from a guide for another card:
`grep -rs soft-mixer ~/.config/wireplumber /etc/wireplumber`. It makes
PipeWire turn the volume down in software instead of using the card's
volume. Remove that rule (or limit it to the other card) and restart
WirePlumber: `systemctl --user restart wireplumber`. The "Pro Audio" profile
also uses software volume only; use "Stereo Output + Stereo Input" (see
[USING.md](USING.md#playback)).

**Listening to an input on the speakers tears all audio now and then**
(a console on optical in, a turntable on the rear RCA input, linked to the
output in Helvum, qpwgraph or `pw-link`): PipeWire made the input the clock
for the speakers, so every hiccup on the input tears everything that plays.
Make the inputs follow the output instead: put this in
`~/.config/wireplumber/wireplumber.conf.d/51-ae9-inputs.conf`

```
monitor.alsa.rules = [
  {
    matches = [
      { node.name = "~alsa_input.pci-.*optical_in" }
      { node.name = "~alsa_input.pci-.*rear_rca" }
      { node.name = "~alsa_input.pci-.*what_u_hear" }
    ]
    actions = { update-props = { priority.driver = 500 } }
  }
]
```

and `systemctl --user restart wireplumber`. Optical in only takes PCM: a
Dolby Digital or DTS bitstream (a console set to "Bitstream") comes in as
noise, so set the source to Linear PCM.

**"... Leaving it alone; if a driver_override or new_id sent it here, remove
that" in the kernel log** (0.12.16+): the driver was pointed at a card that
isn't an AE-9 or an AE-7, usually by a copy of `00-ae9-override.rules` or
`ae9-override.conf` edited for another card, or by hand through sysfs. The
line names the card. The driver refuses it before touching it (nothing
enabled, nothing written), but the override still keeps the card's own
driver away. The AE-5, AE-5 Plus, Z, ZxR and Recon3D belong to the kernel's
`snd_hda_intel` with its ca0132 codec driver (the AE-7 has its own line,
below, and that driver hangs an AE-7 with an ASMedia bridge:
[AE7.md](AE7.md#the-asmedia-bridge-and-the-kernels-driver)). To give
the card back, remove the edited rule from
`/etc/udev/rules.d/` or `/etc/modprobe.d/`, then (the address is the one in
the log line):

```sh
echo | sudo tee /sys/bus/pci/devices/0000:05:00.0/driver_override
echo 0000:05:00.0 | sudo tee /sys/bus/pci/drivers_probe
```

A `new_id` entry goes away when the driver is unloaded
(`sudo modprobe -r snd-hda-codec-creative-ae9`). Before 0.12.16 the driver
didn't check and ran its AE-9 start-up on such a card, writing to the wrong
registers: if that happened, power off fully once (quick fix 2).

**"Sound Blaster AE-7 found: experimental support is off; see docs/AE7.md
(run ae9-ae7-enable)" in the kernel log** (0.13.0+): an AE-7 was handed to
this driver, but its AE-7 support isn't turned on (the `ae7` option is 0, the
default). The driver leaves the card alone. On a **parked** AE-7 (support
turned off on a card with an ASMedia bridge, or a bridge the script doesn't
know: `ae9-ae7-enable --status` says "parked") this line is expected: the
card is kept without a driver on purpose. To use the AE-7 with this driver,
run `sudo ae9-ae7-enable` and reboot ([AE7.md](AE7.md)). To give it to the
kernel's own driver, `sudo ae9-ae7-enable --disable --stock` and reboot, but
only with a Tsi381 bridge: that driver hangs the PC at boot on an AE-7 with
an ASMedia bridge (next entry). If you never ran the script, look for a
copied or edited override rule as in the entry above.

**The PC hangs at boot with an AE-7** (the start stops or freezes; the log,
if you get one, shows `Watchdog detected hard LOCKUP` with `snd_hda_intel`,
`azx_probe_work` or `snd_hdac_bus_init_cmd_io`). The kernel's own
`snd_hda_intel` took an AE-7 with an ASMedia PCIe bridge (most AE-7s from
2022 on). It has hung the whole PC, not just the sound, on every such card
reported so far. To boot:

1. At the GRUB menu (hold Shift or press Esc while the PC starts if it
   doesn't show), press `e`, add `module_blacklist=snd_hda_intel` at the end
   of the line that starts with `linux`, and boot with Ctrl+X or F10. With
   systemd-boot (Pop!_OS), press `e` on the entry, add the same word at the
   end, and press Enter.
2. That lasts for that one boot and also turns off every other HD-Audio
   device for it (the onboard sound, a graphics card's HDMI sound).
3. Then, with this driver installed, either turn the AE-7 on
   (`sudo ae9-ae7-enable`) or park it with no driver
   (`sudo ae9-ae7-enable --disable`), and reboot normally. Without the
   driver, `sudo bash scripts/ae9-ae7-enable --disable` from a copy of its
   source parks it too
   ([AE7.md](AE7.md#the-asmedia-bridge-and-the-kernels-driver)).

**The volume jumps for a few seconds at login** (any card): the login
screen (LightDM, GDM, ...) runs its own PipeWire and WirePlumber, which set
the card's hardware volume ("Master") to their own level; when your desktop
session starts, its WirePlumber restores yours. So for a few seconds at login
the card can play at the login screen's level (on the AE-7 test card: -36
dB). It's harmless: an ordinary volume change, and the driver checks each
level it sends to the DAC as it does for yours.

**DKMS fails on a clang-built kernel** (XanMod, CachyOS...; the build log
`/var/lib/dkms/snd-hda-codec-creative-ae9/<version>/build/make.log` says
`clang: not found` or `clang: command not found`): DKMS builds the driver for
such a kernel with `LLVM=1`, which runs a plain `clang`. Since 0.13.0, when
there is none, the build uses the clang version the kernel was built with:
`clang-21` and `ld.lld-21` on the PATH, or else `/usr/lib/llvm-21/bin/` (for a
kernel built with clang 21; the log's first line says which). If neither is
there, the build stops with "is a clang-built kernel, but no clang ... was
found" and the packages to install: on Debian, Ubuntu and Mint `sudo apt
install clang-21 lld-21` (the version from that message), elsewhere the
distribution's `clang` and `lld`. Then rebuild: `sudo dkms autoinstall -k
<kernel>` or `sudo scripts/dkms-install.sh` again. Before 0.13.0 the build
needed an unversioned `clang` (and `lld`) on the PATH.

**SBX/EQ do nothing:** they work with Direct Mode off, at 48 or 96 kHz. A
44.1 kHz stream keeps playing direct; `pw-metadata -n settings 0
clock.force-rate 48000` and play again (see [USING.md](USING.md#playback)).

## Reporting a problem

`sudo ae9-full-dump` (from a source checkout: `sudo scripts/ae9-full-dump.sh`)
saves everything the driver can read about the card (streams, controls,
PipeWire and the card's profile, registers, the DSP's router, the
driver's recent log, a "Driver state" section, and the PCI path: link,
power-saving and error counters of the card's bridges) to `~/ae9-dumps/ae9-dump-<time>.txt`, without changing
anything (before 0.12.14: the current directory). To save them elsewhere:
`AE9_DUMP_DIR=/some/folder ae9-full-dump`.

For an issue, `ae9-full-dump --report` (0.12.14+) does the same and adds
what we ask for every time: `system.txt` (kernel, distribution, driver
version, how it's installed, `ae9-get-firmware --check`, PipeWire and
WirePlumber versions, Driver Health) and `kernel-log.txt` (the driver's
lines from this boot and the end of the last one, and IOMMU faults that
point at the card). You get the
files to read in `~/ae9-dumps/ae9-report-<time>/` and one
`ae9-report-<time>.tar.gz` of them to attach. Both work on an AE-7 too
(0.13.0+).

- Stop all audio first: while something plays it skips the chip readings,
  which can freeze the audio during playback.
- Take one while the problem is happening (and one when it's fine, if you
  can) and attach them to your
  [issue](https://github.com/3xHelix/SbAe9-Drv/issues).
- It only names the apps that are playing; look it over before posting.
- Say what you did right before it went wrong, and whether a reload or a
  power-off fixed it.
- Without `--report` (an older version), also include, if you can (each is
  one command):
  - `uname -r` and your distribution;
  - `modinfo -F version snd-hda-codec-creative-ae9` and `dkms status`
    (or which package you installed);
  - `ae9-get-firmware --check`;
  - `journalctl -k -b | grep -iE "ae9|AMD-Vi|DMAR|IO_PAGE_FAULT"` (the
    driver's lines from this boot, and IOMMU faults that point at the
    card, if any).
- In "Driver state", "STALE" means a leftover route; the driver switches
  those off at the next Direct Mode start.

A quick health check any user can run, any time (0.12.11+):
`amixer -c AE9 cget name='Driver Health'` (`-c AE7` on an AE-7). The first five of its nine
numbers (command timeouts, answers that never landed, answers from the wrong
codec, unreadable ring pointer, DMA buffers kept) should be 0, and so should
the ninth (late DAC I2C NACKs: DAC commands the DAC didn't acknowledge after
start-up, 0.12.17+); if one isn't, include that line in your report.

Two more tools for volunteers (read-only):

- `sudo ae9-dsp-snapshot > snap.txt` samples the DSP's control and DMA
  registers a few times and marks the ones that move: a working DSP's do, a
  stuck one's may not. Most useful with music playing in DSP mode (Direct
  Mode off): one on a good boot, one while the audio is broken, before you
  power off.
- `sudo ae9-read-stress > stress.txt` reads one chip register 300 times and
  counts the answers: one value means clean reads, several mean some came
  back wrong on your card.

Both refuse to run while audio plays unless you add `--playing`: reading the
card's chip during playback can freeze the audio until a full power-off (PSU
switch; a reboot isn't enough). Only add it if that's OK.

For more detail in the kernel log:
`echo 1 | sudo tee /sys/module/snd_hda_codec_creative_ae9/parameters/debug`
(see [OPTIONS.md](OPTIONS.md)).

## Known issues

- **A warm reboot can leave the card dark** now and then (lights off, no
  head unit, no sound); Windows sees the same at times, so it's the card. The
  driver notices within a second (`codec 1 (DSP) not answering after the bus
  reset`), tries a few times, and if it says `never answered`, power off at
  the PSU for ~20 s; only that clears it (a warm reboot sometimes does, a
  reload doesn't).
- **Rapid switching:** before 0.12.10, lots of fast setting changes in a
  row could wear the card into a bad state. That was the early-read bug
  0.12.10 fixed: 640 random changes, many with no gap, now come back clean.
  If anything still goes odd after a burst of changes, `ae9-reload`, and
  please send a full dump.
- **SBX: silence or echoey, crunched audio** with Direct Mode off that
  toggling doesn't fix means the card's audio router is in a bad state; only
  a PSU power-off clears it (a warm reboot doesn't). Seen often while SBX was
  being developed, rarely since; 0.12.11 routes DSP mode in the router
  chip's own slots, which removes the known causes. If you still see it,
  send `ae9-full-dump` (its "router stream 0x18" line shows the routes).
- **Silent restart after a pause** (rare): playback restarting after a pause
  comes up silent even though the stream runs and the audio reaches the card.
  Closing and reopening the player, or `ae9-reload`, brings it back. Cause
  inside the card not found yet; if it happens, run `ae9-full-dump` while
  it's silent.
- **A brief hiccup** about half a second into the first stream after a
  sample-rate or output change: the DAC re-locking to its new settings
  (Windows sends them at every start; we only when something changed,
  `dac_start_skip`).
- **One channel silent after changing the bit depth or a forced rate
  during playback** (PipeWire older than 1.6.8, e.g. Ubuntu 24.04's 1.0.5):
  PipeWire can leave a playing stream's port silent when the output is
  suspended and reopened under it (PipeWire issue #3547, fixed in 1.6.7 and
  1.6.8). The right channel stays silent until the next track or the next
  reopen; the driver gets the silence already mixed in (`pipewire
  --version` shows yours). Update PipeWire, or change PCM Bit Depth and the
  rate while nothing plays.
- **"Refused to change power state from D0 to D3hot"** in the kernel log at
  every suspend: the PCI core asking the card (behind its bridge) to sleep,
  which it declines; harmless, a suspend cuts the card's power anyway and the
  driver brings it back up at resume. The AE-7 logs it too.
- **AE-7: "waiting for the DAC to lock to its S/PDIF input"** at the start
  of a 44.1 or 88.2 kHz stream in Direct Mode is normal: at those rates the
  DAC takes a moment to lock before the driver unmutes it (within about
  1.5 s). The rest of the AE-7's known limits: [AE7.md](AE7.md#known-limits).
- **Suspend blocked** by other programs stuck on network file I/O (seen with
  Plex reading from a CIFS share); not driver-related.

## Fixed in earlier versions

- 0.12.15: MPD (or another player) straight on `hw:AE9,0` crackled at
  44.1 kHz with its default buffer (500 ms): it got only 2 periods
  (2 x 11024 frames), and the card's DMA reads about 0.7 ms ahead of the
  position the driver reports, so a refill that came a little late played
  old audio, with no xrun logged; now 4 (4 x 5520). A player asking for a
  buffer time or size of its own (MPD's `buffer_time`, `aplay
  --buffer-time`, `speaker-test -b`) could be refused with "Invalid
  argument" (0.12.14 refused about 1 request in 6 in our tests; 0.12.15
  none). On 0.12.14, `buffer_time "481760"` in the AE-9's `audio_output`
  in `mpd.conf` works around both (4 or more periods at every rate). A
  native DSD stream that was set up but not yet playing when Direct Mode
  went off (or an output switch began) could then start silent, with no
  error (now the player gets an error and sets it up again). The
  head-unit volume could cross outputs after a reload or `alsactl
  restore` (the speakers got the headphones' level).
- 0.12.11: DSP mode (Direct Mode off) played 6 dB quieter than Direct Mode
  and only half of the audio; the mic could sound like a chipmunk, or DSP
  mode go silent, after playing something at 96/192 kHz and switching
  Direct Mode off; unloading the driver could crash the kernel if the card
  stopped answering during the unload; at the first load after a power-on
  the card's router chip could be left half set up (a DAC allocation stuck
  until the next power-off).
- 0.12.10: the card could stop answering after effect changes in DSP mode
  (SBX on/off and the like), mostly while recording; the occasional lost
  DSP reply behind it is gone too.
- 0.12.9: the head unit showed no "AE-9" at start-up in 0.12.8.
- 0.12.7: after the card's mic monitor had been on, Voice Clarity
  recordings made with it off were wrong (left quiet, right empty) until a
  power-off.
- 0.12.6: switching Direct Mode on or off while the optical out played
  could leave What U Hear recordings stopping about a second after the next
  playback started.
- 0.12.4 / 0.12.5: reloading the driver (or a warm reboot) used to get
  riskier with each restart until the DSP stalled ("tail replay done: ... N
  failed", recording dead), and a clean reload could leave recording silent
  (all-zero samples). Since 0.12.4 the driver restarts the card the way
  Windows does (light sleep at unload, wake + 2 s, Windows' recovery cycle
  when needed); 0.12.5 fixed DSP mode and What U Hear staying silent after a
  restart, and ASMedia-bridge cards being silent after a reload.

Every change, release by release: [CHANGELOG.md](../CHANGELOG.md).
