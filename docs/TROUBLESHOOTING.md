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
   runs and come back by themselves.
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

**SBX/EQ do nothing:** they work with Direct Mode off, at 48 or 96 kHz. A
44.1 kHz stream keeps playing direct; `pw-metadata -n settings 0
clock.force-rate 48000` and play again (see [USING.md](USING.md#playback)).

## Reporting a problem

`sudo ae9-full-dump` (from a source checkout: `sudo scripts/ae9-full-dump.sh`)
saves everything the driver can read about the card (streams, controls,
PipeWire, registers, the DSP's router, the driver's recent log, a "Driver
state" section) to `ae9-dump-<time>.txt` in the current directory, without
changing anything.

- Stop all audio first: while something plays it skips the chip readings,
  which can freeze the audio during playback.
- Take one while the problem is happening (and one when it's fine, if you
  can) and attach them to your
  [issue](https://github.com/3xHelix/SbAe9-Drv/issues).
- It only names the apps that are playing; look it over before posting.
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
