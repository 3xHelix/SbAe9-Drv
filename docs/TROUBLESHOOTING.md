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
`ae9-report-<time>.tar.gz` of them to attach.

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
`amixer -c AE9 cget name='Driver Health'`. The first five of its eight
numbers (command timeouts, answers that never landed, answers from the wrong
codec, unreadable ring pointer, DMA buffers kept) should be 0; if one isn't,
include that line in your report.

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
  driver brings it back up at resume.
- **A player that opens the card directly** (`hw:AE9,0`, e.g. MPD with
  `buffer_time` set) can be refused some exact buffer times ("Invalid
  argument" from `snd_pcm_hw_params_set_buffer_time_near`): leave the
  player's buffer at its default or pick another value. Planned fix: 0.12.15.
  (PipeWire isn't affected.)
- **Suspend blocked** by other programs stuck on network file I/O (seen with
  Plex reading from a CIFS share); not driver-related.

## Fixed in earlier versions

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
