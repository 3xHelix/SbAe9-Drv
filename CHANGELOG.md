# Changelog

Driver for the Creative Sound Blaster AE-9 (out of tree). Pre-releases,
tested on one card (IDT bridge, `10e3:8111`).

## Unreleased

- License made plain: a COPYING file with the GPL-2.0 text, a license tag
  on every file, and a License section in the README. Nothing changed in
  the driver itself.

## 0.7.0 (2026-11-05)

Spent a day chasing crackles at tiny quantums and came out the other side
with a card that runs 384 kHz at well under a millisecond, 5.1 at 96 kHz,
and starts every song clean. Turns out most of it was the driver getting
in its own way.

### Fixed

- **Stutter at 384 kHz with PipeWire on auto.** The playback buffer was
  stuck at 64 KB, which at 384 kHz is one PipeWire quantum. One cycle
  filled the whole thing: stutter at the start, odd clicks after. It's
  256 KB now, so 384 kHz on auto just plays. You no longer need a big
  quantum to keep 192/384 kHz clean.
- **5.1 at 96 kHz, the "here be dragons" one.** The driver was giving a
  96 kHz 5.1 stream the stereo setup: fronts mapped, the rest of the
  channels landing wherever the card's router pointed last. Tinny, then
  stuttering more and more. All six speakers now come out right at 96 kHz,
  clean for minutes on end, and it's **on by default** (Direct Mode; DSP
  mode stays 48 kHz). `surround_96k=0` puts 5.1 back to 48 kHz only.
- **The little hiccup half a second into every song.** Every start
  reprogrammed the DAC (Windows does it too), and the DAC re-locks: a
  quick glitch. Now it only gets those commands when the rate or output
  actually changed. You'll still hear it once after a rate change; every
  other start is clean. `dac_start_skip=0` goes back to Windows' way.
- **Apps asking for a big quantum** (pw-play wants 4096 at 96 kHz) could
  get one as big as the whole buffer and throw errors. The buffer now
  holds 8 periods by default (`pcm_periods`, 2-32): more room, same
  interrupt rate, same latency.

### Added

- **Low latency, if you want it.** The card's DMA reads about half a
  millisecond ahead of where the driver says it is. At tiny quantums
  PipeWire writes into audio the card already grabbed, and you get a
  tinny, metallic static with no error anywhere. A bit of PipeWire
  headroom fixes it: the README has a "Low latency" section with the
  numbers per rate, and `config/60-ae9-headroom.conf` (256, covers every
  rate) if you want it. Nothing changes PipeWire's defaults. Here it runs
  384 kHz clean down to quantum 64, about 0.8 ms.
- New module options: `pcm_periods` (8), `pcm_nowakeup` (0, lets PipeWire
  turn period interrupts off), `pcm_posbuf` (0; 1 logs the DMA read-ahead
  every 2 s while playing), `dac_start_skip` (1).
- **`hutext` debug command:** `echo "hutext ABCD" >
  /sys/kernel/debug/creative_ae9/cmd` puts your own text on the head
  unit's display (4 characters; a `.` lights the dot of the one before).
  Bare `hutext` gives the volume back.

### Heads up

- **Reloading the driver is for quick checks.** Even a clean reload can
  leave recording silent (all zeros). Reboot for good measure.
- 44.1/88.2 kHz still go through PipeWire's resampler.

Tested on kernel 7.2.15, freshly power-cycled: stereo at 48/96/192/384 kHz
on auto and small quantums (native-rate tones, no resampling), 5.1 at 48
and 96 kHz including a rate change mid-song, suspend/resume, speakers <->
headphones from the knob and the app, Direct Mode off/on with SBX, and a
day of Spotify, Discord and YouTube.

Upgrading from 0.6.8: rebuild, replace the module (`sudo depmod -a`),
reboot (PSU off is the safest start). Drop `surround_96k=1` from your
options file if you added it; it's the default now.

-- Joe

## 0.6.8 (2026-11-04)

Turned this thing up to 384 kHz and lived to tell the tale. Also learned
the hard way that this card is not a sports car: it does not do a 180 on
a dime. Give it a second and it's solid as hell.

### Added

- **All the rates, out of the box:** 48, 96, 192 and 384 kHz in Direct
  Mode, no module option needed anymore. Pick one in SbAe9-SBC, or with
  `pw-metadata -n settings 0 clock.force-rate 192000` (0 = automatic).
  At 192/384 kHz PipeWire wants a bigger quantum or it crackles (2048 at
  384); the app sets that for you.
- **"PCM Bit Depth" control, 24-bit or 32-bit.** 24 is the default like
  Windows. Takes effect on the next stream; the app reopens it for you.
- **EQ presets A to Z**, with the game presets (Project CARS, Racing,
  Rocket League, RPG) at the bottom like the Windows app. Your saved
  preset stays the same preset, it's stored by name.
- [Helvum](https://gitlab.freedesktop.org/pipewire/helvum) gets a shout
  in the README. Beautiful tool for watching the six 5.1 ports light up.

### Fixed

- **The echo.** 96 kHz with Direct Mode off played the song twice: once
  straight through and once through the DSP a hair later. DSP mode now
  runs at 48 kHz and PipeWire resamples into it. Direct Mode keeps every
  rate.
- **5.1 at 96 kHz stutters** more and more as it plays. Caught it four
  times, the last on a fresh card with nothing else going on. 5.1 now plays
  at 48 kHz, where it's rock solid at 24 and 32-bit. If you want to wrestle
  it anyway: `surround_96k=1`. Here be dragons. Needs a Windows capture to
  sort out.

### Heads up

- **Don't speedrun the settings.** Dozens of rate/mode/5.1/bit-depth
  flips in a row can wear the card into a stuttering or silent state that
  only a PSU power-off clears. Normal use never gets there. SbAe9-SBC now
  shows an "Applying changes" notice and paces itself, just like Creative's
  own app does on Windows.
- 44.1/88.2 kHz still go through PipeWire's resampler.

Tested on kernel 7.2.15, freshly power-cycled: stereo at 48/96/192/384 at
24 and 32-bit, rate switching back and forth, 5.1 at 48 kHz at both
depths, DSP mode, and the new SbAe9-SBC picker and speedbump end to end.

Upgrading from 0.6.7: rebuild, replace the module (`sudo depmod -a`),
reboot (PSU off is the safest start). You can drop `pcm_rates=1` and
`pcm_bits=32` from your options file; use the control instead.

-- Joe

## 0.6.7 (2026-11-04)

**5.1 works.** Front, rear, center and sub, every one coming out of the
right jack. In Direct Mode and through the DSP. On a card that was a
stereo-only science project this morning. I moved one pair of speakers
from jack to jack about forty times today and I'd do it again.

### Added

- **5.1 surround.** Pick "Analog Surround 5.1 Output" for the AE-9 in your
  sound settings and you get FL/FR on the RCA jacks, center + LFE on the
  Center/Sub jack, rear L/R on the Rear jack. Works with Direct Mode on
  (straight to the DACs, like Windows) and off (through the DSP, effects
  and all). Played a real 5.1 FLAC in VLC: all six where they belong.
- **PipeWire profiles for the card** (`config/creative-ae9.conf` +
  `config/89-ae9-pipewire.rules`, setup step 4). Before this PipeWire only
  ever gave the AE-9 its "fallback" stereo profile, so 5.1 couldn't even
  show up. Stereo is still the default.
- **Speaker Config 4.0 / 4.1 / 5.1**, and it follows you: switch the
  desktop to 5.1 and a 2.0/2.1 setting jumps to 5.1 by itself; go back to
  stereo and it drops to 2.1. 4.0/4.1 stay put. Set 2.0 on a 5.1 stream and
  the DSP folds everything down to your two speakers, same as Windows.
- [SbAe9-SBC](https://github.com/3xHelix/SbAe9-SBC) now switches the
  desktop profile from its Speaker Config (and has a speaker test that
  says "Front Left", "Rear Right"... on the right speaker). Sync goes both
  ways and neither one fights the other. Tested every layout.
- **A stuck-DSP warning.** If the DSP isn't running when DSP mode turns on,
  the log says so plainly: DSP mode will be silent, Direct Mode works,
  power off at the PSU. That's the sign I was hunting all week.
- `scripts/ae9-surround-test.sh [--direct]`: speaks each channel's name
  so you can check your jacks.

### Fixed

- **The things the chip leaves lying around in 5.1.** Old stereo feeds
  that dragged the front into the rear (quiet, tinny, wrong voices), and
  an extra set of outputs that put the front on the sub. Both get switched
  off now. Switching between a stereo and a 5.1 stream rebuilds the DSP
  path properly instead of giving you static.
- **DKMS updates can't leave you with no driver** anymore: if
  `dkms remove` grumbles, the install carries on, and the script refuses
  to say "done" unless the module is really there.
- **One install method per machine:** the DKMS script won't run next to a
  packaged build and warns about leftover copies; `--check` shows what's
  installed and which copy loads.

### Correction to 0.6.5

The dark card after a warm reboot does **not** come back with the
driver's retry, I was wrong. Caught one live and threw everything at it:
bus resets, reloads 16 minutes later, the whole old slow bring-up. Dead
every time. A PSU power-off fixed it in one go. The fast fail stays (8
seconds and "power off at the PSU" beats a 3-minute boot stall), and
`codec1_failfast=0` is there if you ever want to experiment.

### Heads up

- After a long session of flipping Direct Mode and switching streams,
  the DSP can get stuck (silent or front-only DSP mode). Warm reboots
  don't clear it, a PSU power-off does. Normal use doesn't get there.
- 6 channels go up to 96 kHz; above that it's stereo.

Tested on kernel 7.2.15: the spoken channel test in both modes, a 5.1
FLAC, all five speaker configs on a 5.1 stream, Direct Mode on/off with
5.1 playing, stereo/5.1 profile switching from KDE and from SbAe9-SBC.

Upgrading from 0.6.6: rebuild, replace the module (`sudo depmod -a`),
install the two PipeWire files (setup step 4), reboot. First boot: a PSU
power-off is the safest start.

-- Joe

## 0.6.6 (2026-11-04)

Remember this morning? New kernel lands, I reboot, and before I can hear
a thing I'm rebuilding the driver by hand. Never again.

### Added

- **DKMS.** `sudo scripts/dkms-install.sh` hands the driver to DKMS, and
  from then on every new kernel gets it rebuilt automatically when dnf
  installs it. Update the kernel, reboot, music. Run the script again
  when you update the driver; `--remove` takes it back out. It cleans up
  a hand-copied module so you never end up with two, and the driver stays
  out of the initramfs like before (checked).

Tested on kernel 7.2.15: install, reboot, the DKMS-built module loaded
(same srcversion as the source). The real test is the next kernel update,
and I'm kind of looking forward to it.

Upgrading from 0.6.5: `sudo dnf install dkms kernel-devel` if you don't
have them, then `sudo scripts/dkms-install.sh` and reboot. That's the last
time you copy a .ko by hand.

-- Joe

## 0.6.5 (2026-11-04)

Spent the evening throwing music on and rebooting for fun, and the card
finally showed its hand. Two fixes that only matter on a bad day, which
is exactly when you want them, plus a log you can actually read again.

### Fixed

- **The dark card.** Every so often a warm reboot comes up with the card
  dead: lights off, no head unit, no sound. Happens in Windows too, so
  it's the card being moody, not us. Pulled the logs: the card's main
  chip just doesn't answer after the reboot. The old driver didn't take
  the hint and kept talking to it for 2-3 minutes straight (that's the
  "systemd-modules-load ... 2min 58s" crawl on boot), and THEN retried,
  and the retry brought it right back. Now the driver notices in the
  first second, skips the pointless part, and retries right away. A bad
  reboot should cost you seconds now, not minutes and a PSU flip. If it
  really won't come back, the log tells you plainly to power off at the
  PSU.
- **The overlay retry from 0.6.3 got its first real test**: a reboot at
  17:32 hit the exact failure, the first transfer fell over, the second
  one landed, boot carried on like nothing happened. Love to see it.
- **"SBX Switch" is locked with Direct Mode on**, same as the SBX button
  on the head unit. Mixers and the control panel snap back if you try.
- Zero compiler warnings. Thirteen little leftovers from old experiments,
  gone.

### Changed

- **The log is quiet now.** By default you get startup, the stuff you do
  (Direct Mode, SBX, output switches, mutes) and anything that goes
  wrong. All the per-stream chatter, router details and every single
  volume step went behind a switch:
  `echo 1 | sudo tee /sys/module/snd_hda_codec_creative_ae9/parameters/debug`
  brings it all back (2 adds the raw knob/button state), `echo 0` puts it
  away. No reboot needed.

### Learned (for the 5.1 crowd)

Went back through the Windows capture where I stepped through every
speaker layout: Windows always feeds the DSP 6 channels in DSP mode, even
on 2.0, and the layout is one DSP setting. A lot of the 5.1 plumbing is
already in place on the card's side. 5.1 is next on the list.

Tested on kernel 7.2.15: a pile of reboots with music going, the overlay
retry live, quiet logs with debug on and off, the SBX lock with the
control panel.

Upgrading from 0.6.4: rebuild, replace the module, `sudo depmod -a`,
reboot.

-- Joe

## 0.6.4 (2026-11-04)

Quality-of-life round, weeeee. The SBX button got its light, and the
knob immediately got jealous. Turns out the head unit has a little quirk,
and we caught it in the act.

### Fixed

- **The knob froze on headphones with SBX on.** Turning still worked, but
  mute, output switching and the knob light just... stopped, until you
  poked something in the control panel. Turned on the new debug log and
  there it was: on headphones with the SBX light on, the head unit leaves
  its "knob is being held" flag stuck on after the first press. Forever.
  The driver sat there waiting for you to let go of a knob you'd let go
  of ages ago. It still sends a separate "this press lasted X ms" report
  for every press, so now the driver notices the flag is lying and uses
  those reports instead, until the flag comes back to its senses (it
  does once you're on speakers or the light goes off). Even the first
  press counts. No more freezing, no more surprise knob-light toggles.
- **SBX light went dark after a speaker/headphone switch** while SBX was
  still running. Windows' switch sequence flashes that same light and
  leaves it off. Now the driver puts it right back.

### Changed

- **Knob light is now a 3 second hold** (was 3.5 s). Speakers/headphones
  is 1.5 to 3 s, mute is still a quick press. Why: that "press lasted"
  report from the head unit stops counting at 3 seconds, so 3 s is where
  the knob light has to live for it to work in every mode. On headphones
  with SBX on it toggles right at the 3 second mark, while you're still
  holding. Feels kind of nice actually.

### Added

- `hu_debug=1` (can be flipped while running, under
  `/sys/module/snd_hda_codec_creative_ae9/parameters/`): logs the head
  unit's raw button state. It's how this one got caught.
- The driver now sends the same "output mode" frame Windows sends right
  before each SBX light change (`hu_sbx_mode`, on by default).

Tested on kernel 7.2.15: mutes, output switches and the knob light on
speakers and headphones, with SBX on and off, SBX button mashing in
between. 99% on the money, and now that last 1% too.

Upgrading from 0.6.3: rebuild, replace the module, `sudo depmod -a`,
reboot. Muscle memory heads up: knob light is 3 s now.

-- Joe

## 0.6.3 (2026-11-04)

This one was hellish fun. New kernel drops (7.2.15), I reboot, and DSP
mode is dead silent. Cue an afternoon of dumps, scripts and "wait, that
can't be right." It wasn't the kernel. It was the card being the card.
But we came out the other side with a pile of fixes and the SBX button
on the head unit finally doing its job. Awesome quality of life.

### Added

- **The SBX button on the ACM works.** Press it and SBX flips on or off,
  same as the "SBX Switch" control, and your mixer or control panel
  follows along. Only with Direct Mode off; with Direct Mode on it does
  nothing, on purpose. Found it live: the button sits on bit 3 of the
  head unit's status byte.
- **The SBX light works too.** Pulled it out of the Windows capture
  (Windows flips it right as SBX toggles). It's lit while the effects
  are actually running: Direct Mode off and SBX on. The head unit even
  reports the light back to us. Polite little thing.
- "DSP alive" lines in the log at boot and when DSP mode turns on, so
  the next time the card gets stuck it tells on itself.
- Two helper scripts for chasing card weirdness:
  `scripts/ae9-dsp-snapshot.sh` and `scripts/ae9-read-stress.sh`.

### Fixed

- **The DSP firmware overlay was missing on two of three boots.** One
  setup read came back as 0 (not just the 0xff we already caught), so the
  overlay got sent to the wrong place and never landed; another boot hit
  an I/O error halfway. The driver now treats 0 as a bad answer too,
  checks that the overlay actually arrived, and sends it again if it
  didn't. Recording depends on it.
- **The card lies a little while audio plays.** Read the same router entry
  300 times with music stopped: 300 right. With music playing: 6 wrong.
  The card moves its own address pointer under us mid-read, and no lock
  on our side can stop that. Router reads now have to agree twice in a
  row, and router writes get read back and resent if they didn't stick.
  (So far every write has stuck. Good card.)
- The router's "outputs to 0xff" line no longer jumps the gun before the
  DSP path is built, and it stopped pretending to know what it means. It
  doesn't mean a stuck card; we checked, twice.

### A word on SbAe9-SBC

If you haven't tried [SbAe9-SBC](https://github.com/3xHelix/SbAe9-SBC)
yet, do. It's the desktop control panel for this driver (SBX, EQ and
presets, Scout Mode, Direct Mode, outputs, recording, mixer), and I've
been using it for a while now. It's a solid step in for the Windows
software that ships with the card, and it follows the knob and now the
SBX button live.

Tested on kernel 7.2.15: SBX button and light, Direct Mode off/on, a
handful of reboots (overlay landing every time), the read checks.

Upgrading from 0.6.2: rebuild for your kernel, replace the module,
`sudo depmod -a`, reboot. New kernel? Rebuild first, every time. And if
DSP mode ever comes up silent after a reboot, power off at the PSU for
20 seconds. A warm reboot keeps the card's bad mood.

-- Joe

## 0.6.2 (2026-11-04)

Quality-of-life round, plus one mystery solved and one ghost hunted.

### Added

- **Speaker Config** control (2.0 / 2.1), same idea as the speaker
  setting in the Creative app. Works with Direct Mode off; defaults to
  2.1, which is what Windows uses. Checked it in the log: the DSP gets
  exactly the values Windows sends. 4.0, 4.1 and 5.1 show up when 5.1
  does; until then they'd just throw half your mix at jacks we don't
  drive yet.

### Fixed

- **The "27 + 24 firmware mismatches" are gone.** These sat in the
  "known issues" list since the start, bugging me. Turns out the
  firmware was fine the whole time: we were reading the DSP's spare RAM
  back from the wrong address. Read it where it actually lives and it's
  a perfect match. 0 mismatches. Case closed.
- **Ghost boots.** Every so often a boot came up where DSP mode was
  silent or did something weird (every output run through a stack of
  sample-rate converters, like the card was having a moment). Both of
  those boots had one thing in common: a setup read during startup came
  back empty and the driver just guessed. Now it waits a beat and asks
  again. If you ever see "after a retry" in the log, that's it doing its
  job.

Tested on kernel 7.2.14: Speaker Config both ways, the firmware
read-back, Direct Mode off/on, SBX on speakers.

Upgrading from 0.6.1: replace the module, `sudo depmod -a`, reboot. If
DSP mode ever sounds wrong after an update, power off at the PSU once;
a warm reboot keeps the card's old state.

-- Joe

## 0.6.1 (2026-11-04)

- **All 20 of Windows' EQ presets**, straight from the capture, in the
  Creative app's order: Flat, Rock, Concert, Night, Acoustic, Classical,
  Country, Dance, Hip Hop, Jazz, Pop, R&B, Vocal, Gaming, Cinema, Project
  CARS, Racing, Rocket League, RPG, Stadium. Fractional values come
  through (0.1 dB); the band sliders still show whole dB. Heads up: on
  this card Night is the same as Concert, and the four game presets are
  flat. That's what Windows sends too.
- Tested by ear on kernel 7.2.14: Stadium, Concert, Hip Hop and Flat,
  clearly different. Bang on.

Upgrading from 0.6.0: replace the module, `sudo depmod -a`, reboot. A
saved "EQ Preset" setting keeps its name (Rock stays Rock).

## 0.6.0 (2026-11-04)

**SBX works on Linux.** Surround, Crystalizer, Dialog Plus, Smart Volume,
Bass, the EQ: through the card's own DSP, toggled from alsamixer, and you
can *hear* it.

I'm not going to pretend this one was clean. It took one long night of
"turn Direct Mode off... silence. Turn it back on... sound. Off again...
crunchy garbage that sounds like a USB card dying under load." The logs
said everything was fine. Every command: 0 errors. The card was lying to
us, politely.

So we stopped trusting the logs and started dumping the card's audio
router, all 64 entries of it, after every single click. Something like
thirty dumps later, it confessed:

- We were turning on the DSP's input stream and **never turning it off**.
  The card's little 8051 brain kept that path reserved forever, and the
  next time we asked for DSP mode it built a second path on spare
  hardware that never started. Silence. Toggle more, it gets worse.
  Reboot? Still broken, because the card doesn't forget on a warm reboot.
  Only pulling power fixed it. (I got very good at flipping the PSU
  switch.)
- Windows **waits** after it stops a stream: 48 ms here, 144 ms there.
  We didn't. Sometimes the chip kept up, sometimes it didn't, which is why
  the exact same steps worked on one boot and not the next.

Both fixed. Then we broke it again on purpose: round trips, sleep and wake,
headphones, booting straight into DSP mode, a warm reboot in the middle of
a Discord call with the mic open. It held.

### What you get

- **Direct Mode** ("Direct Mode Switch", on by default): same as before,
  straight to the DAC at the stream's rate, up to 384 kHz, DSP bypassed.
  Nothing changes unless you turn it off.
- Turn it **off** and the audio goes through the DSP at 96 kHz (stereo
  for now). Then the **SBX** button ("SBX Switch") turns the effects on
  and off.
- With both: Surround, Dialog Plus, Smart Volume (and its mode),
  Crystalizer, Bass (switch + level 0-100 each), EQ on/off with Flat,
  Rock and Gaming presets, pre-gain and ten bands, and Scout Mode.
  Otherwise those are greyed out and keep their values. Two-stage, like
  Windows.
- Switch Direct Mode with music playing, both ways. After boot, wake, or a
  switch made while nothing plays, the first stream plays direct for 3 s
  and then moves to the DSP (`dsp_switch_ms`). Turning Direct Mode back on
  goes quiet until you pause/play, same as Windows does.
- Speakers and headphones, sleep and wake, reboots. Before a reboot,
  power-off or sleep the driver now stops the DSP streams and any
  recording it started, so the card comes back clean.
- The six startup messages the DSP always rejected aren't sent any more
  (`tail_drop_rejected`), so those error lines are gone from the log.

### Known limits

- DSP mode is stereo only; 5.1 isn't wired up yet.
- Direct Mode on is silent until the stream restarts (pause/play).
- Three EQ presets so far; Scout Mode and 96 kHz streams in DSP mode
  haven't had a proper listen yet.
- If DSP mode ever goes silent or crunchy and toggling doesn't fix it,
  power off at the PSU. Haven't seen it since these fixes, but now you
  know.

Tested on kernel 7.2.14, Fedora 44 KDE, one card: everything above, plus
Direct Mode as in 0.5.0 (48/96/192 kHz, recording, sleep).

Upgrading from 0.5.0: replace the module, `sudo depmod -a`, reboot.

If you've got an AE-9 and Linux, try it and tell me what breaks. -- Joe

## 0.5.0 (2026-11-03)

Two new controls, both done the way the Windows driver does them.

- **Mic level** ("Capture" in alsamixer): -89 to +9 dB in 1 dB steps, mute
  at the bottom; default +9 dB, the level the mic had before. PipeWire uses
  it as the mic's hardware volume, so the desktop microphone slider now
  moves the card's own level. "XLR Gain" (0/+10/+20 dB preamp) stays a
  separate control.
- **DAC filter** ("DAC Filter"): Default, Fast or Slow roll-off for the
  headphone/line-out DAC, as in the Creative app. Default leaves the
  startup filter. Kept across sleep and wake.
- Tested on kernel 7.2.14: recording at different mic levels, filter
  switching during playback, sleep and wake.

Upgrading from 0.4.0: replace the module, `sudo depmod -a`, reboot. Note
that your desktop mic slider now sets the card's hardware level (100% =
+9 dB, as before).

## 0.4.0 (2026-11-03)

Higher rates and 32-bit, set up exactly as the Windows driver does it.

- **384 kHz playback** (with `pcm_rates=1`). Windows sends it over the card's
  link as 192 kHz with two samples per frame; the driver does the same.
- **True 32-bit playback** with `pcm_bits=32` (default stays 24-bit, as
  Windows). All 32 sample bits reach the card.
- **Fixed: 192 kHz was slightly quieter** than 96 kHz. The card needs more
  internal routing set up at higher rates (4 routes at 96, 8 at 192, 16 at
  384); 0.3.0 only set the 48 kHz pair.
- Rate changes now follow a new Windows capture step for step; steps that
  came from guesswork (an extra clock register, a DSP rate and a PLL
  reprogram) are gone.
- 44.1 and 88.2 kHz are still not offered: Windows routes them through a
  sample-rate converter in the card, which doesn't play right on Linux yet
  (`pcm_rates=2` turns them on for testing only). PipeWire resamples those
  files, so they play fine.
- In PipeWire: `pw-metadata -n settings 0 clock.allowed-rates '[ 48000 96000 192000 384000 ]'`.
- Tested on kernel 7.2.14: same pitch and level at 48/96/192/384 kHz in 24-
  and 32-bit, PipeWire following file rates, playback through VLC.

Upgrading from 0.3.0: replace the module, `sudo depmod -a`, reboot. Your
`pcm_rates=1` keeps working and now includes 384 kHz.

## 0.3.0 (2026-11-03)

High-resolution playback: 96 and 192 kHz.

- **New, opt-in:** playback at 96 kHz and 192 kHz (24-bit), alongside 48 kHz.
  Turn it on with `pcm_rates=1` (in `config/ae9-options.conf`, or at
  runtime through `/sys/module/snd_hda_codec_creative_ae9/parameters/pcm_rates`),
  then let PipeWire use the rates, e.g.
  `pw-metadata -n settings 0 clock.allowed-rates '[ 48000 96000 192000 ]'`
  or `clock.force-rate 96000`. See README -> Using it.
- The card is re-clocked for each stream the way Windows does it (rate
  codes, I2S clock settings, full PLL program); going back to 48 kHz
  restores the startup clock setup.
- 44.1 and 88.2 kHz are not offered yet: they play sharp on this card, so
  Windows does something more for that clock family. Leave them out of
  PipeWire's allowed rates and it resamples those files (they play fine).
- Recording stays at 48 kHz; it works at the same time as 96 kHz playback.
- With the option off (the default) nothing changes from 0.2.1.
- Tested on kernel 7.2.14: pitch at 48/96/192 kHz, PipeWire at 96 kHz,
  mic during 96 kHz playback, output switching, sleep and wake.

Upgrading from 0.2.x: replace the module, `sudo depmod -a`, reboot, then
turn on `pcm_rates` if you want the higher rates.

## 0.2.1 (2026-11-03)

Small update: better logging, and fewer slow starts.

- Fixed: a needless firmware retry on about half of startups and resumes.
  The DSP briefly reports a firmware chunk as finished while it is still
  loading; the driver now double-checks before moving on, so startup no
  longer stalls 2-4 s for a retry.
- New log line after every startup: `DSP health: ok`, or a warning when the
  DSP did not come up right (with a hint to power off at the PSU if
  recording is dead).
- New log lines when a plug goes in or out of the head unit's 3.5 mm
  headphone or 3.5 mm mic jack. Logging only: the driver never switches
  between speakers and headphones by itself. (The 6.3 mm headphone jack and
  the XLR/TRS combo jack are not reported by the head unit.)
- Groundwork for sample rates other than 48 kHz (the stream format is now
  built for 8-192 kHz). Playback and recording still run at 48 kHz only.
- README: `iommu=pt` is not needed (tested on AMD with the IOMMU on).
- Tested on kernel 7.2.14 (Fedora 44): boot, sleep and wake on speakers and
  headphones, playback, recording, output switching.

Upgrading from 0.2.0: replace the module, `sudo depmod -a`, reboot.

## 0.2.0 (2026-11-03)

Stability release: every item from a full code review, plus fixes found
while testing them.

- Fixed: noisy, clipped recording after resume. A firmware chunk that never
  finished loading went unnoticed and left the DSP half-loaded; the load now
  fails and is retried.
- Fixed: CA0113 (head-unit DAC) command race.
- CORB timeout recovery follows the HD-Audio spec; CORB sends refuse while
  the rings are parked.
- The driver claims only the AE-9 and guards the bus reset.
- Firmware images are validated before use; extraction is MD5-checked.
- Probe/remove clean up on errors and never register a card with a dead DSP.
- Stream control writes no longer clear status bits by accident; the
  interrupt-enable update is locked.
- Head-unit polling sleeps instead of spinning, on the driver's own work queue
  (also silences a kernel 7.x warning).
- Fixed: the codec count grew on every resume.
- **XLR Gain** (was "Mic Boost"): the 0/+10/+20 dB mic preamp is separate from
  the desktop microphone slider, which is now software gain only. Set it with
  `alsamixer -c AE9` (F4) or `amixer -c AE9 sset 'XLR Gain' 0|1|2`.
- `restore-stock-audio.sh` removes everything the README installs.
- Tested on kernels 7.2.13 and 7.2.14 (Fedora 44).

Upgrading from 0.1.0: replace the module, `sudo depmod -a`, reboot. If you
used the desktop mic slider for boost, set it back to 100% and pick a gain
with `XLR Gain`.

## 0.1.0 (2026-11-02)

First pre-release.

- Stereo playback on the RCA outputs (speakers) and the head unit's headphone
  jack; 2 ch, 48 kHz, 24-bit (S32_LE), through ALSA and PipeWire.
- Recording from the head unit's XLR mic input, with 0/+10/+20 dB boost.
- Head unit: display, volume knob, mute, speaker/headphone switch, knob light.
- Suspend/resume, reboot and power-off follow Windows' sequences.
