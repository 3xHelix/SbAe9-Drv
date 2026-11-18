# Using the AE-9 on Linux

Everything the driver does, feature by feature. Back to the [README](../README.md).

Prefer a window to `alsamixer`? [SbAe9-SBC](https://github.com/3xHelix/SbAe9-SBC)
gives every control below a Sound Blaster Command-style app (Python 3 +
PySide6), and follows the knob and alsamixer live. The controls named here
are also in `alsamixer -c AE9` and `amixer -c AE9`.

- [The head unit](#the-head-unit)
- [Playback](#playback)
- [Direct Mode, SBX and the EQ](#direct-mode-sbx-and-the-eq)
- [Speakers and headphones](#speakers-and-headphones)
- [5.1 surround](#51-surround)
- [Recording](#recording)
- [Optical (S/PDIF)](#optical-spdif)
- [DAC filter](#dac-filter)
- [Low latency](#low-latency)
- [Sleep, reboots and reloads](#sleep-reboots-and-reloads)

## The head unit

- **Output:** the card's RCA jacks (speaker mode, "-SP-") or the head unit's
  headphone jacks, 3.5 mm and 1/4" ("-HP-"). It starts on speakers; switch
  with a 1.5-3 s knob hold or the "Head Unit Output" control. Each output
  remembers its own volume (both start at -36 dB).
- **Knob:** turn = volume (0.5 dB per step, about 1 dB per click), press
  (< 1 s) = mute, hold 1.5-3 s = speakers / headphones, hold 3 s or more =
  knob light on/off.
- **Volume:** the knob's setting is also the ALSA "Master Playback Volume" /
  "Master Playback Switch" (`hu_master=0` names them "Head Unit Playback ..."
  instead), so PipeWire, the desktop slider and the knob all stay in sync.
  Range -127 dB to 0 dB; the desktop slider at 100% is 0 dB, the card's full
  output level. Keep app volumes at 100% in PipeWire and use the knob or the
  system volume (the card's hardware volume); app volumes above 100% are
  software gain and can clip.
- **SBX button:** turns SBX on/off, the same as "SBX Switch", only with Direct
  Mode off (with Direct Mode on it does nothing). Its light is on while the
  effects are running: Direct Mode off and SBX on. It acts when you let go,
  as on Windows. Hold it 5 s or more to switch the head unit's display off or
  back on (the knob light stays); the "Head Unit Display" control does the
  same.
- **Display at start-up:** "AE-9" for about 5 s, then "-SP-" or "-HP-", then
  the volume, as on Windows. If the head unit loses power or its setup while
  running, the driver sets it up again and the same sequence shows.
- **Plugs:** "Head Unit 3.5mm Phones Plugged" (the head unit's 3.5 mm
  headphone jack) and "Head Unit 3.5mm Mic Plugged" (its 3.5 mm mic jack)
  show what's plugged in, read-only, for apps to show. The 1/4" headphone
  jack and the XLR/TRS jack don't report plugs.
- **Headphone impedance switch** (IEM / N / High, on the head unit): works
  on its own, as on Windows; set it for your headphones (IEM 16-31 Ω, N
  32-149 Ω, High 150-600 Ω). The driver can read its position but doesn't
  need to: Windows only fine-tunes a DAC distortion setting from it, which
  made no measurable difference here.
- **Auto switch:** "Head Unit Auto Switch" (off by default) makes the head
  unit's 3.5 mm headphone jack switch outputs: plug in = headphones, unplug =
  speakers, about 1.5 s after the plug settles. Off, plugs change nothing.
  The 1/4" headphone jack isn't reported, so it can't auto switch. To start
  with it on: `options snd-hda-codec-creative-ae9 hu_auto_switch=1`.
- **Phantom power:** "Head Unit Phantom Power" shows and switches the head
  unit's 48 V phantom power for the XLR/TRS jack (its own 48V button works
  too). It always starts off, and switching it on is refused for 30 s after
  the card starts or wakes, so a restored mixer state can't turn 48 V on by
  itself. Only turn it on for mics that need it: 48 V can damage some gear
  (ribbon mics, some line-level sources).

## Playback

- **Desktop profile:** in PipeWire / KDE use the "Stereo Output + Stereo
  Input" profile: the desktop volume slider and mute drive the card (hardware
  volume), and the knob moves the slider and OSD. "Pro Audio" also plays, but
  uses software volume only.
- **Format:** the PCM takes 2 or 6 channels, S32_LE. "PCM Bit Depth"
  (24-bit, the default as Windows, or 32-bit) sets how the card takes it; it
  applies to the next stream (24-bit sources lose nothing at 32-bit).
- **Rates:** 44.1, 48, 88.2, 96, 176.4, 192 and 384 kHz in Direct Mode
  (44.1/88.2/176.4 kHz stereo only); 48 or 96 kHz with Direct Mode off,
  through the DSP (PipeWire resamples other rates). Recording is 48 or 96 kHz.
- **Choosing a rate:** PipeWire runs at 48 kHz unless told otherwise.
  `pw-metadata -n settings 0 clock.allowed-rates '[ 44100 48000 88200 96000 176400 192000 384000 ]'`
  follows each file's rate, `clock.force-rate 192000` forces one (0 = back to
  automatic). SbAe9-SBC does this from its rate picker. 44.1/88.2 kHz play
  through the card's sample-rate converter; leave 44100/88200 out and PipeWire
  resamples those files to 48 kHz instead. `pcm_rates=1` drops 44.1/88.2 kHz
  from the card's list, `pcm_rates=0` limits it to 48 kHz.
- **DSP mode needs 48 or 96 kHz:** with Direct Mode off, a stream at another
  rate (44.1 kHz, say) keeps playing direct, and DSP mode and the effects
  start with the next 48 kHz stream. `pw-metadata -n settings 0
  clock.force-rate 48000` gets you there.
- [Helvum](https://gitlab.freedesktop.org/pipewire/helvum) (on Flathub)
  draws PipeWire's patchbay: a great way to see the AE-9's ports (FL FR on
  stereo, all six on the 5.1 profile) and what's feeding them.

## Direct Mode, SBX and the EQ

Two stages, as in Windows.

- **"Direct Mode Switch"** (default on) plays straight to the DAC at the
  stream's rate, DSP bypassed. Turn it off and the audio goes through the
  DSP (streams at 48 or 96 kHz there, PipeWire resamples the rest; stereo or
  5.1).
- **"SBX Switch"** then turns the effects on or off, and "Speaker Config"
  (2.0 or 2.1, default 2.1 as Windows) tells the DSP your speaker setup.
- With both, the "FX Surround / Dialog Plus / Smart Volume / Crystalizer /
  Bass" switches and volumes (0-100), "FX Smart Volume Mode", "EQ Switch",
  "EQ Preset" (Windows' 20 presets, Flat to Stadium), "EQ Pre-gain" and ten
  bands (31 Hz-16 kHz, -12..+12 dB) and "Scout Mode Switch" act; otherwise
  they are greyed out and keep their values.
- **DSP mode plays at the same level as Direct Mode** (since 0.12.11;
  before, it was 6 dB quieter and only half the samples reached the DACs).
  **If you're coming from an older version, turn the volume down before you
  first switch Direct Mode off, on the headphones too.** Bass Redirect
  sends the bass to the sub on the Center/Sub jack (right channel). SBX's
  Smart Volume ("Loud" especially) lifts quiet audio a lot: by design.
- Works on speakers and headphones, across sleep and reboots. With Direct
  Mode off, a new stream after boot, wake or a switch made while stopped
  plays direct for 3 s, then moves to the DSP. Turning
  Direct Mode on restarts a stream that was going through the DSP by itself;
  one still waiting to move there keeps playing.

## Speakers and headphones

- **Speaker setup** (Direct Mode off, SBX on or off): "Bass Redirect Switch"
  sends the bass to the sub (x.1 configs), "Bass Redirect Crossover"
  (50-200 Hz, default 80; Windows' Tower is 80, Desktop 120) and "Bass
  Redirect Sub Boost Switch" (subwoofer boost, on/off); per speaker (Front
  L/R, Center, Sub, Rear L/R) "Speaker Level ... Volume" (-12..+12 dB) and
  "Speaker Distance ..." (0-500 cm of delay, as Windows' calibration).
  Measured on the card's C/Sub jack: the sub's -6 dB point lands on the
  crossover setting (82 Hz at 80, 205 Hz at 200), with Bass Redirect off
  the sub is silent, and Sub Boost adds about 15 dB.
- **Headphone Profile:** Sound Blaster Command's 32 headphone curves
  (ATH-M50, HD 280 Pro, ...), active with Direct Mode off, headphones out
  and a 48 kHz stream; off on speakers. The curves are Creative's:
  `ae9-get-firmware` installs them from Creative's installer.
- **Your own headphone profiles:** up to four, made from a published
  measurement of your headphones by `ae9-hp-profile` (AutoEQ's corrections,
  thousands of models), listed under their own names:
  `sudo ae9-hp-profile add "Beyerdynamic DT 990 Pro" DT990` fetches the
  correction, saves `/lib/firmware/creative-ae9/hp/DT990.bin`
  (`/etc/firmware/...` on Fedora Atomic) and lists it as "Beyerdynamic DT 990
  Pro" (in SbAe9-SBC too); empty slots show as "Custom 1" to "Custom 4". Pick
  it there or with
  `amixer -c AE9 cset name='Headphone Profile' 'Beyerdynamic DT 990 Pro'`.
  `ae9-hp-profile list` shows the slots, `sudo ae9-hp-profile remove DT990`
  frees one; `fetch`, `make` and `install` do the steps one by one
  (`ae9-hp-profile --help` explains them; your own CSV of frequency and gain
  works too). The slots live in `/etc/modprobe.d/creative-ae9-hp.conf`
  (`hp_custom`) and change in the running driver at once (names: up to 40
  bytes, so fewer letters with accents). A profile loads once nothing has
  played for a few seconds; a new file for a profile already picked loads by
  itself the same way. They never boost (the DSP stage clips), so they play a
  few dB quieter.
- **Headphone virtualization:** "Headphone Virtualization Switch", on by
  default as in Windows. With headphones selected and Direct Mode off, the
  DSP runs its headphone mode, Creative's "Virtual 7.1": SBX Surround ("SBX
  Switch" and "FX Surround") is the virtualizer, as in Windows, and 5.1
  (6-channel) sources are placed around you. The speaker setup (Speaker
  Config, Bass Redirect, per-speaker level and distance) doesn't apply in
  this mode. Off, headphones get the older speaker-style DSP path. It does
  nothing on speakers or in Direct Mode
  (`amixer -c AE9 cset name='Headphone Virtualization Switch' off`).
- **Line out virtualization:** "Line Out Virtualization Switch", Creative's
  "Apply Headphones Virtualization to Line Out", off by default. With
  Headphones selected and Direct Mode off, the virtualized headphone mix
  plays on the card's RCA out instead, and the head unit's headphone jacks
  are silent (for example, for headphones on an external amp). The volume is
  the headphone volume, so turn the knob down before you switch it on. "Head
  Unit Output" still reads Headphones. On:
  `amixer -c AE9 cset name='Line Out Virtualization Switch' on`.

## 5.1 surround

Pick "Analog Surround 5.1 Output" for the AE-9 in the desktop's sound
settings (needs the PipeWire profiles, which the packages install). Front L/R
come out of the card's RCA jacks, center and LFE out of its 3.5 mm C/Sub jack,
rear L/R out of its 3.5 mm Rear jack.

- Works in Direct Mode (straight to the DACs, 48, 96 or 192 kHz; 384 kHz
  stays stereo) and with Direct Mode off (through the DSP, 48 or 96 kHz).
- With Direct Mode off, "Speaker Config" tells the DSP your layout: 2.0/2.1
  fold the six channels down to the front (and sub), 4.0/4.1 drop the center,
  5.1 uses all six. It follows the stream by itself: a 5.1 stream turns
  2.0/2.1 into 5.1, a stereo stream turns 5.1 back into 2.1 (4.0/4.1 stay as
  you set them).
- [SbAe9-SBC](https://github.com/3xHelix/SbAe9-SBC) switches the desktop
  profile from its Speaker Config and has a speaker test.
  `scripts/ae9-surround-test.sh` speaks each channel's name (stops PipeWire
  while it runs).

## Recording

The card's recording devices:

| Device | What | Rates |
|---|---|---|
| `hw:AE9,0` | the head unit's mic input (XLR/TRS jack and 3.5 mm mic jack, shared) | 48, 96 kHz |
| `hw:AE9,1` | optical in | 44.1, 48, 88.2, 96, 192 kHz |
| `hw:AE9,2` | What U Hear (what the card plays) | 48, 96 kHz |
| `hw:AE9,3` | the head unit's rear RCA input | 44.1, 48, 88.2, 96, 192 kHz |

In PipeWire each is its own source of the AE-9's card profile.

- **The mic:** `arecord -D hw:AE9 -c 2 -r 96000 -f S32_LE`, or the AE-9 input
  in PipeWire/Audacity. The head unit's XLR/TRS jack and its 3.5 mm mic jack
  share it (Windows lists them as Microphone and Aux; the mic's own mute
  switch affects both). "Capture Bit Depth" picks 24-bit (default) or 32-bit
  samples, as Windows' recording format choice
  (`amixer -c AE9 cset name='Capture Bit Depth' 32-bit`; it takes effect at
  the next recording start).
- **Mic levels:** "Mic Gain" sets the mic preamp to 0/+10/+20 dB (default
  +10, like Windows): `amixer -c AE9 sset 'Mic Gain' 0|1|2`. With
  `mic_boost_30=1` it also offers 3 = +30 dB: the preamp has it, Windows
  doesn't use it; for quiet mics, normal speech clips there. "Capture" is the
  mic level, -89 to +9 dB in 1 dB steps with mute at 0 (default +9 dB, what
  Windows' recording setup uses), set the way Windows' Recording -> Levels
  slider does it; PipeWire uses it as the mic's hardware volume, so the
  desktop microphone slider moves it (100% = +9 dB). Mic Gain stays separate.
- **Clearer desktop names:** copy `config/61-ae9-names.conf` to
  `~/.config/wireplumber/wireplumber.conf.d/` and run
  `systemctl --user restart wireplumber`: the sources show as "AE-9 Head Unit
  Mic / Aux", "AE-9 Rear RCA In" and so on (node names stay the same).
- **Voice Clarity:** Sound Blaster Command's Recording page, run on the
  card's DSP as Windows does, for the head unit's mic input. "Voice Clarity
  Capture Switch" is the master; under it "Acoustic Echo Cancellation Capture
  Switch" (takes the speakers' sound out of the mic, about 43 dB in our
  test), "Noise Reduction Capture Switch", "Mic Smart Volume Capture Switch"
  and "Mic EQ Capture Enum" (Off, or Creative's 11 presets, "Preset 1 -
  Reduce Bass, harshness, improve clarity" to "Preset 10 - Improve clarity"
  and "Preset DM-1 - Improve vocal and clarity"). All off by default. The mic
  goes through the DSP's voice processor only while the master and at least
  one effect are on; otherwise it records as before. It sounds like Windows:
  wideband voice with nothing above 8 kHz (the voice processor runs at
  16 kHz), and Noise Reduction thins the low end. For voice chat, not music.
  While an effect is on, the mic records at 48 kHz only (Windows does the
  same). Changes act at the next recording start, or at once on a recording
  already running at 48 kHz. Example:
  `amixer -c AE9 cset name='Voice Clarity Capture Switch' on`, then
  `amixer -c AE9 cset name='Noise Reduction Capture Switch' on` and
  `amixer -c AE9 cset name='Mic EQ Capture Enum' 'Preset 10 - Improve clarity'`.
- **Voice Morphing:** "Voice Morphing Capture Enum", Sound Blaster Command's
  18 voices: Neutral (off, the default), Male, Female, Kid, Elderly, Deep
  Voice, From Up North, Unstable, Emo, Elf, Dwarf, Demon, Infiltrator, Brute,
  Orc, Marine, Chipmunk, Robot. It's part of Voice Clarity: it acts with
  "Voice Clarity Capture Switch" on, at 48 kHz
  (`amixer -c AE9 cset name='Voice Morphing Capture Enum' Robot`).
- **Mic monitoring:** "Mic Monitor Playback Switch" (off by default) plays
  the head unit's mic input on the output with zero delay, mixed inside the
  DSP, like Windows' mic monitor. Direct Mode off only. The driver holds the
  mic input for it, so no app has to record. It plays once DSP mode has been
  set up by some playback since boot: with Direct Mode off, play something
  for a few seconds first. Use headphones: on speakers the mic picks itself
  up and feeds back. On:
  `amixer -c AE9 cset name='Mic Monitor Playback Switch' on`. Software
  monitoring (the desktop's "Listen", a pw-loopback) still works, with a
  short delay; if you set a PipeWire headroom, use the per-rate one (see
  [Low latency](#low-latency)). It mixes freely with Voice Clarity since
  0.12.7 (before, Voice Clarity recordings made after the monitor had been
  on came out wrong until a power-off). "Mic Monitor Used Since Power-up"
  (read-only) always reads off now; it stays for older SbAe9-SBC versions.
- **Clean mic (optional example):** Windows' cleaner-sounding mic is
  Microsoft's software noise suppression, not the card. The same idea on
  Linux: `config/ae9-clean-mic.conf` (packages:
  `/usr/share/creative-ae9/examples/`) adds an input "AE-9 Mic (clean)", the
  AE-9 mic through PipeWire's built-in WebRTC noise suppression and low-cut,
  mono, no automatic gain. On the card: room hiss and hum 20-30 dB lower,
  speech level unchanged, about 35 ms extra delay (fine for calls and
  recording, not for monitoring yourself). Put your mic's name in it
  (`pactl list short sources | grep 'pci-.*analog-stereo$'`), copy it to
  `~/.config/pipewire/pipewire.conf.d/`, run
  `systemctl --user restart pipewire pipewire-pulse wireplumber` and pick
  "AE-9 Mic (clean)" in the app. Don't use it together with the card's
  Voice Clarity (two noise suppressors fight).
- **Rear RCA input:** the RCA L/R jacks on the back of the head unit,
  `hw:AE9,3` ("AE-9 Rear RCA In"), 24-bit, recording alongside the mic
  (`arecord -D hw:AE9,3 -c 2 -r 48000 -f S32_LE rca.wav`). Not 176.4 kHz: its
  converter doesn't offer it. No level control yet: line-level sources
  record around -25 dB below full scale.
- **What U Hear:** records what the card plays, `hw:AE9,2` ("AE-9 What U
  Hear"), next to the mic and optical in
  (`arecord -D hw:AE9,2 -c 2 -r 96000 -f S32_LE out.wav`). It works in Direct
  Mode and through the DSP; with SBX off it's the stream exactly as sent (a
  test tone comes back level-exact on both channels). In PipeWire it runs at
  PipeWire's rate (48 or 96 kHz; SbAe9-SBC can pin it). With the 5.1 output
  profile ("Analog Surround 5.1 Output + Analog Stereo Input") it records the
  six channels your speakers get, instead of a stereo copy ("What U Hear"
  source with node name `...what_u_hear_51`, FL FR FC LFE RL RR;
  `arecord -D hw:AE9,2 -c 6 ...`), e.g. for an AC-3 encoder; the stereo
  profiles keep the stereo source.
- **Upgrading from 0.10.x:** if you copied `ae9-what-u-hear.conf`,
  `ae9-rear-rca-in.conf` or `ae9-optical-in.conf` into
  `~/.config/pipewire/pipewire.conf.d/`, delete them, or each source shows
  up twice.

## Optical (S/PDIF)

Both ways, stereo, on the card's optical jacks.

- **Optical out** is its own output: PCM device 4, `hw:AE9,4` ("AE-9 Optical
  Out"; the "Optical Out" output in PipeWire), its own stream at 44.1-192 kHz,
  16-bit or 24-bit in 32. It takes the stream as the app sends it, before the
  DSP: no SBX on it, and app volume shows on it but the knob and system volume
  don't (those set the card's analog output). Your receiver sets the level.
  "Optical Out Playback Switch" turns it off
  (`amixer -c AE9 cset name='Optical Out Playback Switch' off`).
- **Mirror:** "Optical Out Mirror Switch" (off by default) makes the optical
  jack also play what the main output plays while nothing is sent to it,
  whenever that's stereo at 24-bit (the default "PCM Bit Depth") up to
  192 kHz. It's silent at 32-bit, at 384 kHz and in 5.1 (S/PDIF carries
  stereo up to 24-bit/192 kHz). PipeWire closes the optical output a few
  seconds after its last sound; with the mirror off, optical stays silent
  then.
- **Dolby Digital / DTS passthrough** to a receiver:
  `ae9-optical-passthrough movie.mkv` (needs ffmpeg; it sets the S/PDIF
  "non-audio" bit while it plays: `iecset -c <card> audio off`, the "IEC958
  Playback Default" control on device 4). While something holds `hw:AE9,4`
  directly (the passthrough script, an encoder), don't switch the card
  profile: PipeWire then builds it without the "Optical Out" output, which
  comes back at the next profile change.
- **Optical in** is its own capture device, `hw:AE9,1` ("AE-9 Optical"),
  44.1, 48, 88.2, 96 or 192 kHz (not 176.4: its converter doesn't offer it),
  so it records alongside the mic
  (`arecord -D hw:AE9,1 -c 2 -r 48000 -f S32_LE in.wav`). In PipeWire it's
  the "Optical In" source. S/PDIF input can't resample, so record at the
  sender's rate (PipeWire runs at 48 kHz unless told otherwise). With nothing
  sending it records silence; plugging the cable in or pulling it while
  recording restarts the recording once, on the signal or on silence.

## DAC filter

"DAC Filter": the ES9038's seven digital filters with the Creative app's
names (thanks to DarkNikGr): Apodizing Fast Roll Off (Windows' startup
filter, the default), Fast Roll Off, Slow Roll Off, Fast Roll Off - Minimum
Phase, Slow Roll Off - Minimum Phase, Hybrid Fast Roll Off, Brick Wall
(`amixer -c AE9 sset 'DAC Filter' 'Brick Wall'`). Set the way the Creative
app sets them, both DACs included; they apply in Direct Mode too. Kept across
sleep/resume.

## Low latency

PipeWire's defaults work as they are; nothing here is needed for normal use.
If you lower the quantum yourself, know this about the card: its DMA fetches
audio about 0.5-0.7 ms ahead of the playback position the driver reports,
the same on any machine. Below that (roughly 200 frames at 384 kHz, 100 at
192 kHz) PipeWire writes into audio the card has already fetched, and you get
a tinny, metallic static with no error logged. PipeWire's
`api.alsa.headroom` keeps extra frames queued past the card's read point and
cures it. Values tested clean down to quantum 32-64:

| Rate | Headroom | Measured read-ahead |
|---|---|---|
| 48 kHz | 64 | up to 33 frames |
| 96 kHz | 96 | up to 49 frames |
| 192 kHz | 144 | up to 97 frames |
| 384 kHz | 256 | up to 195 frames |

PipeWire takes one headroom per output. Two ways to set it:

- **Per rate (recommended):** `scripts/ae9-headroom.sh` runs as a user
  service and sets the value from the table for the rate the card is running
  at. When the rate changes on its own it waits 2 s, sets the new value and
  reopens the output once (a short gap, like a pause).

  ```sh
  cp scripts/ae9-headroom.sh ~/.local/bin/
  cp config/ae9-headroom.service ~/.config/systemd/user/
  systemctl --user enable --now ae9-headroom
  ```

- **Fixed:** `config/60-ae9-headroom.conf` sets 256, which covers every rate
  (edit the number for the rate you use most, e.g. 64 for 48 kHz).

  ```sh
  cp config/60-ae9-headroom.conf ~/.config/wireplumber/wireplumber.conf.d/
  systemctl --user restart wireplumber
  ```

A fixed 256 breaks live software mic monitoring ("Listen", a pw-loopback from
the mic to the AE-9 output): with the mic driving the clock, PipeWire resyncs
the output dozens of times a second and the monitor sounds chopped up (64 and
128 are fine). If you monitor the mic, use the per-rate service or a lower
fixed value, or the card's own "Mic Monitor Playback Switch", which doesn't
go through PipeWire.

A very small quantum (64) can drop out under heavy CPU load (`pw-top` ERR
column); 128 or more is fine.

## Sleep, reboots and reloads

- **Suspend/resume:** supported. Resume runs the whole bring-up again in the
  background (~3-5 s after wake) and restores output, per-output volume,
  mute, knob light and mic boost; paused audio carries on.
- **Unload, reboot and power-off** run Windows' own shutdown sequence, so the
  card restarts cleanly (reloads and warm reboots tested).
- **If the DSP misses its boot handshake** (at load or on resume), the driver
  runs that shutdown and tries again: 3 attempts, 2 s apart.
- **"Driver Health"** (read-only, any user): `amixer -c AE9 cget
  name='Driver Health'` gives 8 numbers: command timeouts, command-ring
  answers that never landed, answers from the wrong codec, unreadable ring
  pointer reads, DMA buffers kept because a stream didn't stop (all 0 on a
  healthy card), then late ring answers, the longest wait for one (µs) and
  the router's free slots (information). Apps can follow it with `amixer
  events` (SbAe9-AudioMon does). Since 0.12.11.
- **Reloading the driver:** `ae9-reload` stops what holds the card,
  reloads the driver and starts everything again; Sound Blaster Command
  and AudioMon let go of the card while it runs (see
  [Troubleshooting](TROUBLESHOOTING.md)).
