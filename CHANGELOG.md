# Changelog

Driver for the Creative Sound Blaster AE-9 (out of tree). Pre-releases,
tested on two cards (IDT bridge `10e3:8111`; ASMedia bridge `1b21:1080` since 0.9.8).

## 0.12.5 (2026-11-10)
Restarts, for real this time. 🔁

Pairs with [SbAe9-SBC v0.18.0](https://github.com/3xHelix/SbAe9-SBC) (a hint when headphones are plugged in but the card plays on speakers; fixes for PipeWire 1.0 / WirePlumber 0.4 on Ubuntu 24.04 and Mint 22) and [SbAe9-AudioMon v0.4.6](https://github.com/3xHelix/SbAe9-AudioMon) (meters work on PipeWire 1.0.x).

Thanks to Kaiba (round after round of ASMedia restart tests: the logs that
found both restart bugs below), DarkNikGr (Dolby Digital and DTS confirmed on
a real receiver through the optical out, plus his reports) and s3boun3t (for
hanging out and cheering us on, plus the PipeWire 1.0 reports from
Linux Mint).

- **Fixed: 0.12.4's restart fix was incomplete. After a warm reboot or a
  driver reload, DSP mode and What U Hear could stay silent**, and after a
  reload even Direct Mode. The card's router chip counts how often each
  stream was switched on and keeps that count until the power goes off; the
  driver switched the DSP stream on at every DSP-mode switch but off only
  once, so the leftovers kept the next start from connecting it. The driver
  now keeps the count the way Windows does: it switches the stream on only
  when it's off, and hands every leftover back at shutdown, at start-up and
  when you leave DSP mode.
- **Fixed: ASMedia-bridge cards were silent after a driver reload.** The
  driver skipped the card reset at start-up behind that bridge (since 0.9.8,
  when the reset seemed to take the bridge down; the real cause was fixed
  later), so a reload started on the card's leftover state. The reset now
  runs on every card. Tested by Kaiba: hot reloads, a cold and a warm boot,
  and Voice Clarity now works after a warm reboot too. `bus_reset=0` turns
  it off.
- **Fixed: a DSP message could lose a word when the DSP's command queue was
  busy** (seen a few times per boot on some cards with Voice Clarity on).
  The card's router chip refuses a word while the DSP's queue is full and
  keeps saying "full" until the next word. The driver used to wait 2 s and
  then send the next word, losing the refused one. Now it waits out "busy",
  sends a refused word again after a short pause (up to 2 s) until it goes
  in, and logs how many tries it took. `scp_full_resend=0` is the old
  behaviour, for diagnosis only. Tested by Kaiba on an ASMedia card: the
  word that used to jam goes in after one resend (4 ms).
- **Fixed: on Ubuntu 24.04 / Mint 22, Debian 12 and openSUSE Leap the AE-9
  started 12 dB louder than intended** (-24 dB instead of -36 dB). Their
  WirePlumber 0.4 ignores the packages' safe-volume rule, which is written
  for WirePlumber 0.5. The packages now also install a WirePlumber 0.4
  version (`51-ae9-safe-volume.lua`). 0.4 can only set the start volume for
  every output at once, so there any output without a saved volume starts
  at -36 dB. Volumes you set are still kept.
- Optical out and optical in no longer offer 176.4 kHz (their converters
  don't support it), and a 176.4 kHz main stream isn't mirrored to optical.
- The DSP's start-up reset is repeated while the DSP answers busy, as
  Windows does.
- `dsp_fail_inject` (a test option) only acts with `debug=1`.
- Confirmed by DarkNikGr: Dolby Digital and DTS through the optical out to a
  receiver, both with `ae9-optical-passthrough` and SbAe9-SBC's encoder.
- Known issue, not new: switching Direct Mode while the optical out plays
  can leave What U Hear recordings stalling until the driver reloads (see
  README, Known issues).

## 0.12.4 (2026-11-09)
Restarts and Voice Clarity, the Windows way. 🔁
Pairs with [SbAe9-SBC v0.17.2](https://github.com/3xHelix/SbAe9-SBC) (adds the rtcwake tip) and [SbAe9-AudioMon v0.4.5](https://github.com/3xHelix/SbAe9-AudioMon) (warns when a Voice Clarity recording is hit by the mic monitor quirk). Thanks to Kaiba for the Voice Clarity report that found two of these.

- **Fixed: restarting the driver without a power cut could break the
  card.** Reloading the driver (or a warm reboot) got riskier each time:
  the 5th or 6th restart since the last power-off could leave the DSP
  stalled or the card dark until a PSU power-off. Our unload left the
  card's DSP codec in a deep sleep and the next start reset it without
  waking it. Now the driver restarts the card the way Windows does:
  light sleep at unload, wake + 2 s before the reset, Windows' recovery
  cycle when the analog outputs report a failed ramp, a 1.5 s settle, and
  the DSP firmware always downloaded. 6 restarts in a row on one power-up
  came up clean.
- **Fixed: Voice Morphing and Noise Reduction recordings stalled the DSP
  at their start** (every Voice Morphing start, about a third with Noise
  Reduction), and a DSP message could get lost there: once a loud
  distorted take (Kaiba). Recordings now start in Windows' order, and the
  mic level settings go to the DSP once per start-up instead of at every
  recording.
- **Fixed: switching Voice Clarity off during a recording** left it
  limited to 8 kHz until the recording was reopened (Kaiba). It now goes
  back to the full-band path at once, as Windows does.
- **Fixed: a short crunch when the card's mic monitor switches on.** The
  mic is now bound before the DSP mixes it in.
- **New: a failed DSP start retries the way Windows does** (reset the DSP,
  0.5 s, download again, up to 3 times) before the heavier full restart;
  tested with a forced download failure and a forced handshake failure,
  both recovered on the second download.
- **New: a DSP that halts itself is never set running again.** The driver
  stops talking to it and logs what to do (a short sleep or a power-off),
  instead of nudging a wiped DSP back into a state that left the card
  dark at the next start.
- **New: a short sleep resets a stuck card.** `sudo rtcwake -m mem -s 25`
  (suspend to RAM for 25 s) cuts the card's power like a PSU power-off
  does; try it before reaching for the PSU switch.
- Fixed: the head unit's rear RCA input offered 176.4 kHz, which its
  converter doesn't support; it now offers 44.1, 48, 88.2, 96 and 192 kHz.
- The log now says when a DSP message had to be written into a busy
  queue (it used to happen silently).

## 0.12.3 (2026-11-09)
Fixes from a full code review of 0.12.1 and 0.12.2, all tested on a card. 🔧
Pairs with [SbAe9-SBC v0.17.1](https://github.com/3xHelix/SbAe9-SBC) (Voice Clarity never uses the card's mic monitor) and [SbAe9-AudioMon v0.4.4](https://github.com/3xHelix/SbAe9-AudioMon).

- **Fixed: a bad custom headphone profile name could stop the driver
  loading.** `ae9-hp-profile` now checks names the way the driver does (up
  to 40 bytes, so fewer letters with accents), sets the running driver
  first and saves the setting only if the driver took it. The driver skips
  a bad entry with a warning instead of refusing to load.
- **Fixed: a re-installed or removed custom profile kept playing the old
  filter.** The new file now loads by itself once nothing plays (a removed
  one switches off); "pick Off, then it again" is no longer needed.
- **Fixed: on headphones, speaker settings reached the headphones.** Speaker
  levels, distances, bass redirection, crossover and sub gain changed while
  on headphones (Virtual 7.1) are now stored and sent when you switch back
  to speakers. Speaker Config follows the stream on headphones again, so a
  5.1 stream plays discrete 5.1 when you switch to speakers mid-stream.
- **Fixed: the mic monitor played at Windows' default gain** (+9 dB) after
  turning it on or after a recording, instead of your Capture Volume.
- **Fixed: starting a recording while the mic monitor is on** moved the mic
  route under a bound input (scrambled or wrong-rate recordings possible).
- **Fixed: sleep, reboot and unload** now release the monitor's hold on the
  mic and take Voice Clarity out of the mic path; after a wake, Voice
  Clarity and the monitor come back by themselves (they stayed off).
- **Fixed: the hardware mic monitor sounded crunchy after Voice Clarity had
  been used** (the DSP's mic mode stayed on the voice processor's).
- **Fixed: `ae9-hp-profile` on Fedora Atomic** (installs under
  `/etc/firmware`, no Python crash), and the per-rate headroom script and
  `60-ae9-headroom.conf` now only touch the analog output (they could pick
  the optical output and cut a passthrough stream).
- **New: "Mic Monitor Used Since Power-up"** (read-only). Known issue,
  found in this review: once the card's hardware mic monitor has been on
  since the PC started, Voice Clarity recordings made with it off come out
  wrong (left channel quiet, right channel empty) until the PC is powered
  off. It's in the card's DSP firmware; Windows never turns this monitor on,
  so Creative never met it. SbAe9-SBC v0.17.1 uses its software Listen
  while Voice Clarity is on and warns before mixing the two; the driver
  logs a warning when it happens. Plain recordings are not affected.
- README: the What U Hear 5.1 profile wording is clearer.

## 0.12.2 (2026-11-09)
Everything on Sound Blaster Command's list. 🎙️🎧
Pairs with [SbAe9-SBC v0.17.0](https://github.com/3xHelix/SbAe9-SBC) (Voice Clarity, Voice Morphing, mic monitor and virtualization pages, the optical out selector) and [SbAe9-AudioMon v0.4.4](https://github.com/3xHelix/SbAe9-AudioMon).

- **New: Voice Clarity on the card's DSP**, as Windows does it:
  "Acoustic Echo Cancellation" (takes the speakers' sound out of the mic,
  about 43 dB on our card), "Noise Reduction", "Mic Smart Volume" and
  "Mic EQ" with Creative's 11 presets (Preset 1-10, Preset DM-1). It sounds
  like Windows: a clean wideband voice with an 8 kHz top (the voice
  processor runs at 16 kHz). Controls: "Voice Clarity Capture Switch" and
  one switch per effect; recordings run at 48 kHz while an effect is on.
  The DSP route had been parked since 0.9 because it sounded metallic; the
  cause was found (our recording stop left the voice processor half
  routed) and Voice Clarity now matches Windows step for step.
- **New: Voice Morphing**, all 18 of Creative's voices ("Voice Morphing
  Capture Enum": Male, Female, Kid, Elderly, Deep Voice, From Up North,
  Unstable, Emo, Elf, Dwarf, Demon, Infiltrator, Brute, Orc, Marine,
  Chipmunk, Robot; Neutral = off).
- **New: mic monitoring with no delay** ("Mic Monitor Playback Switch"):
  the card's DSP mixes the mic into what you hear, as Windows' mic monitor
  does. Direct Mode off; it plays once anything has played in DSP mode
  since boot. Use headphones.
- **New: headphone virtualization** ("Virtual 7.1" in Creative's guide):
  on headphones with Direct Mode off the DSP now runs in its headphone
  mode, as Windows does, and SBX Surround places 5.1 sources around you.
  We used to send the speaker layout on headphones. "Headphone
  Virtualization Switch", on by default.
- **New: "Apply Headphones Virtualization to Line Out"** ("Line Out
  Virtualization Switch", off by default): with Headphones selected, the
  virtualized headphone mix plays on the card's RCA out and the headphone
  jack is silent. Turn the knob down first: it uses the headphone volume.
- **New: your own headphone profiles** (DarkNikGr's idea). The new
  `ae9-hp-profile` command makes one from a published measurement: it
  takes AutoEQ's correction for your model (thousands of headphones,
  measured by oratory1990, crinacle, Rtings and others) and turns it into
  the same kind of filter as Creative's profiles (two 512-tap
  linear-phase FIRs at 48 kHz). `sudo ae9-hp-profile add "Beyerdynamic DT
  990 Pro" DT990` saves it as `DT990.bin` and lists it in "Headphone
  Profile" (and SbAe9-SBC) as "Beyerdynamic DT 990 Pro"; up to four,
  `list` and `remove` manage them. Like Creative's, they work on
  headphones with Direct Mode off; they never boost, so they play a few
  dB quieter.
- **Checked: the speaker setup** with a measurement on the card's C/Sub
  jack: the crossover lands on its setting (50-200 Hz), bass redirection
  off leaves the sub silent, Sub Boost adds about 15 dB.
- **Fix: live mic monitoring ("Listen") sounded chopped up** with the
  optional fixed 256-frame PipeWire headroom: PipeWire resynced the output
  dozens of times a second while the mic drove the clock. The README now
  points to the per-rate headroom service (64 at 48 kHz) instead.
- Driver v0.12.1 now pairs with SbAe9-SBC v0.16.1 (its encoder fix).

**Thanks** 🙏 to **DarkNikGr** (custom headphone profiles, the reason the
optical out has two lives) and to everyone sending dumps. The Voice
Clarity, Virtual 7.1 and Line Out work came from Windows captures read
alongside Creative's own app and driver code.

## 0.12.1 (2026-11-08)
Two outputs, two streams. 🔀
Pairs with [SbAe9-SBC v0.16.0](https://github.com/3xHelix/SbAe9-SBC) (Optical Out mirror toggle on Playback > Digital, Encoder page: AC-3, or experimental DTS, to optical) and [SbAe9-AudioMon v0.4.3](https://github.com/3xHelix/SbAe9-AudioMon) (Optical Out meter, bits/kHz per stream; thanks DarkNikGr).

- **Changed: optical out no longer mirrors the main output by default**
  (thanks DarkNikGr). The analog outputs and optical out are now two
  separate outputs: optical plays only what's sent to it ("Optical Out" in
  PipeWire, `hw:AE9,4`). **Upgrading:** if you used the optical jack as a
  copy of the main output, switch "Optical Out Mirror Switch" on
  (SbAe9-SBC: Playback > Digital, or
  `amixer -c AE9 cset name='Optical Out Mirror Switch' on`; module
  option `optical_mirror=1`). A stream sent to optical out always takes
  priority over the mirror.
- **New: What U Hear in 5.1.** With the 5.1 output profile ("Analog
  Surround 5.1 Output + Analog Stereo Input") the What U Hear source
  records all six channels (FL FR FC LFE RL RR, each level-exact), e.g. for
  a software AC-3 encoder; `arecord -D hw:AE9,2 -c 6` works in any
  profile. The stereo profiles keep the stereo source.
- **Clearer headphone-profile log**: the upload result line says what
  happened (done, read-back differs, still running, end not confirmed,
  never started), with the stream position, run time and any verb
  timeouts. Profiles load once playback and recording have been stopped
  for a moment (about 10 s of real silence); a profile picked during
  playback waits for that.

**Thanks** 🙏 to **Kaiba** (the dump generator) and **DarkNikGr** (Mr. Needs
Optical), whose reports, dumps, tests and code shaped this release and the
ones before it, and a hat tip 🎩 to **HandsomeHans** for helping decode our
boot tables a bit further.

## 0.12.0 (2026-11-08)
A second output: optical gets its own stream. 💿
Pairs with [SbAe9-SBC v0.15.0](https://github.com/3xHelix/SbAe9-SBC) (Optical Out on Playback > Digital, passthrough status) and [SbAe9-AudioMon v0.4.2](https://github.com/3xHelix/SbAe9-AudioMon) (kit 0.3.8).

- **New: separate optical output** (thanks DarkNikGr!). PCM device 4,
  `hw:AE9,4`, "Optical Out" in PipeWire: its own stream at 44.1-192 kHz,
  16-bit or 24-bit. While nothing plays to it, the optical jack mirrors the
  main output as before, so nothing changes for you unless you use it.
  Dolby Digital / DTS passthrough to a receiver with
  `ae9-optical-passthrough movie.mkv` (sets the S/PDIF "non-audio" bit
  while it plays; `iecset` controls it by hand).
- **Fix: headphone-profile uploads could be cut short**. A glitched status
  read ended the transfer after ~4 ms of the ~11 it needs, leaving part of
  the old profile in place. 0.11 caught it (the profile stayed off and
  further uploads were blocked; thanks Kaiba for the report), and now it
  doesn't happen: the transfer has to really finish. The read-back now
  checks 9 words.
- **A profile picked during playback no longer switches the current one
  off** while it waits for a pause.
- **Quieter login**: Speaker Config doesn't flip between 5.1 and 2.1 while
  PipeWire probes the card any more.
- **`scripts/dkms-install.sh` builds for every installed kernel with
  headers**, not only the running one (a kernel update without a reboot
  could leave no driver for the next boot).

## 0.11.3 (2026-11-08)
One less way for the DSP to stop. 🛡️
Pairs with [SbAe9-SBC v0.14.0](https://github.com/3xHelix/SbAe9-SBC) and [SbAe9-AudioMon v0.4.1](https://github.com/3xHelix/SbAe9-AudioMon), as 0.11.0.

- **Fix: a bad chip read could be written into the DSP's control
  register**. When the DSP's command queue is full (and once at every
  start), the driver reads the DSP's control register and, if the DSP
  looks halted, writes it back to restart it. Reads of the card's chip
  come back wrong now and then while audio plays (0.3-2.7% on two cards,
  thanks DarkNik for the data), and one bad read was enough to write
  garbage into the DSP's control: a likely cause of a DSP that stops until
  a power-off. Now three reads must agree before anything is written; if
  they don't, the register is left alone and the log says so.
- `ae9-dsp-snapshot` marks registers that change only because of a bad
  read as "one-off (likely a bad read)" instead of "moving".

## 0.11.2 (2026-11-08)
More tools for your reports. 🧰
Pairs with [SbAe9-SBC v0.14.0](https://github.com/3xHelix/SbAe9-SBC) and [SbAe9-AudioMon v0.4.1](https://github.com/3xHelix/SbAe9-AudioMon), as 0.11.0.

- **New commands: `ae9-dsp-snapshot` and `ae9-read-stress`**, installed by
  every package next to `ae9-full-dump`. The snapshot shows which of the
  DSP's registers move (a working DSP's do, a stuck one's may not); the
  stress test reads one chip register 300 times and counts wrong answers.
  Both only read, and both refuse to run while audio plays unless you add
  `--playing`: reading the chip during playback can freeze the audio until
  a full power-off. With `--playing` the snapshot also skips the router
  table. See "Reporting a problem" in the README.
- `ae9-full-dump` is the command to use for dumps (the README said
  `scripts/ae9-full-dump.sh`); stop audio first for the full picture.
- No driver changes since 0.11.1.

## 0.11.1 (2026-11-08)
Builds everywhere again. 🔧
Pairs with [SbAe9-SBC v0.14.0](https://github.com/3xHelix/SbAe9-SBC) and [SbAe9-AudioMon v0.4.1](https://github.com/3xHelix/SbAe9-AudioMon), as 0.11.0.

- **Fix: 0.11.0 didn't build on kernels before 6.13** (Ubuntu 24.04's 6.8;
  DKMS installs failed): optical in's timer used a kernel call that's new
  in 6.13. The build now checks the kernel's own headers and uses the call
  it has (RHEL 10's 6.12 has the new one backported). Tested against 6.8,
  6.12 and 7.2. No other changes; Fedora and Arch users on 0.11.0 don't
  need it.

## 0.11.0 (2026-11-08)
Safer headphone profiles, a DSP that stays in step, seven DAC filters and
inputs that just show up. 🎛️
Pairs with [SbAe9-SBC v0.14.0](https://github.com/3xHelix/SbAe9-SBC) and [SbAe9-AudioMon v0.4.1](https://github.com/3xHelix/SbAe9-AudioMon).

- **Fix: picking a headphone profile could still break DSP mode** until a
  power-off (chopped "8-bit" sound or silence; Kaiba's report and ours).
  Most bad uploads happened while audio played, so a profile now loads only
  when nothing is playing or recording: picked during a song, it loads a
  few seconds after you pause. Every upload is also checked from end to
  end: the DSP's DMA port must be a proper pair, 8 words are read back from
  DSP memory and must match the profile (a mismatch keeps the profile off),
  a half-armed transfer is stopped before its channel is freed, and a
  failure after the DSP was set up stops further uploads until the next
  start, instead of retrying blindly. `hp_live_upload=1` brings back
  uploads during playback.
- **Fix: the "metallic" sound with a headphone profile and the EQ both
  on**. On a hot signal the two boosts overload the DSP (Windows does the
  same); while both are on, the DSP now gets the EQ pre-gain 12 dB lower,
  but not below -12 dB. The slider keeps your value. `eq_headroom=0` turns
  this off.
- **Fix: half-speed, buzzing audio after a speakers/headphones switch in
  DSP mode**. Windows' switch sequence left the DSP in its own state; the
  driver now sends the whole effects setup again after every switch.
- **New: seven DAC filters** (thanks DarkNikGr!), with the Creative app's
  names: Apodizing Fast Roll Off (Windows' startup filter, the default),
  Fast Roll Off, Slow Roll Off, Fast Roll Off - Minimum Phase, Slow Roll
  Off - Minimum Phase, Hybrid Fast Roll Off, Brick Wall. Set the way the
  Creative app sets them, second DAC included. A saved old
  Default/Fast/Slow comes back as the default once.
- **Fix: the DAC got stray writes of 0 at every boot, every shutdown and
  every 44.1/88.2 kHz start**: Windows' register *reads* had been taken
  for writes of 0 when its startup was decoded. They're gone (25 Windows
  captures agree), and the filter you picked now survives every DAC setup
  the driver sends (an output switch used to put it on Fast Roll Off).
- **New: optical in records without a signal**. With nothing plugged in it
  records silence instead of failing; plugging the cable in or pulling it
  while recording restarts the recording once, on the signal or on
  silence.
- **New: Rear RCA In, What U Hear and Optical In are sources out of the
  box**: the card profile opens them, nothing to copy. Upgrading: delete
  `ae9-what-u-hear.conf`, `ae9-rear-rca-in.conf` and `ae9-optical-in.conf`
  from `~/.config/pipewire/pipewire.conf.d/` if you copied them, or each
  source shows up twice. `config/61-ae9-names.conf` (optional) names them.
- **New: a safe start volume**. PipeWire's output volume is the head
  unit's volume (100% = 0.0 dB); the packages now make WirePlumber start
  the AE-9 at -36 dB, the driver's boot volume, whenever it has no saved
  volume for the output.
- More in the log for bug reports: the first failed DSP message after each
  start in full, DSP queue-full recoveries, and Windows' two startup checks
  (codec power state, router recovery bytes).

## 0.10.1 (2026-11-07)
Boots right every time, headphone profiles that hold up, and the head unit's rear RCA input. 🔌
Pairs with [SbAe9-SBC v0.13.0](https://github.com/3xHelix/SbAe9-SBC) and [SbAe9-AudioMon v0.4.0](https://github.com/3xHelix/SbAe9-AudioMon).

- **Fix: the DSP failed its first start on about half of cold boots**
  (packaged installs). Loaded only by udev, the driver's firmware download
  overlapped the rest of startup (network, VMs) and the DSP never answered;
  the retry usually recovered, sometimes with no audio. Every package now
  also installs `/usr/lib/modules-load.d/creative-ae9.conf`, so the driver
  loads early again: 0 failures in 150 boots that way, against 11 in 26
  without it. The DSP also gets 250 ms after it starts before the first
  message, as Windows.
- **New: "AE-9 Rear RCA In"** (`hw:AE9,3`): the head unit's rear RCA L/R
  input as its own recording device, 24-bit, 44.1-192 kHz, recording
  alongside the XLR mic. `config/ae9-rear-rca-in.conf` adds it as a
  PipeWire source. No level control yet.
- **Fix: headphone profiles could kill DSP mode** (silence, one dead
  channel or static until a PSU power-off; Kaiba's report). The DSP's
  replies are now read exactly as Windows reads them, and every upload is
  checked before its EQ goes on: the address is asked twice, the DMA
  channel must be free and armed, the transfer must really finish, and a
  stalled command anywhere in it keeps the EQ off. A failed upload costs
  that pick, never the audio. Picks are debounced: scrolling the list
  uploads only the one you stop on.
- **Fix: right-channel static after a bit-depth change in DSP mode**: the
  router chip sometimes kept the old stream format; it's now checked and
  repaired at every stream start.
- **"Bass Redirect Sub Boost Switch"** replaces "Bass Redirect Sub Gain
  Volume": the DSP setting is an on/off subwoofer boost (Creative's
  SubwooferBoost), not a gain.
- **DAC Filter "Default"** now also puts the Center/Sub and Rear DAC back
  to fast rolloff (it stayed on slow after picking Slow).
- **Firmware moves to `/lib/firmware/creative-ae9/`** (`/etc/firmware/...`
  on Fedora Atomic): `ctefx-desktop.bin` in the firmware root is also the
  AE-5/AE-7 file from linux-firmware (clashed on Arch). Run `sudo
  ae9-get-firmware ...` again to install there; until then the old location
  still works, and a root file that isn't the AE-9 image is refused with a
  clear message instead of loaded.

## 0.10.0 (2026-11-07)
Sound Blaster Command's speaker setup and headphone profiles. 🎧
Pairs with [SbAe9-SBC v0.12.0](https://github.com/3xHelix/SbAe9-SBC) (Speaker Setup tab, Headphone Profile picker).

- **Speaker setup** (Direct Mode off, SBX on or off), decoded from a new
  Windows capture: "Bass Redirect Switch", "Bass Redirect Crossover"
  (50-200 Hz; Windows sets it from the speaker type: Tower 80, Desktop 120),
  "Bass Redirect Sub Gain Volume" (dB), and per speaker (Front L/R, Center,
  Sub, Rear L/R) "Speaker Level ... Volume" (dB) and "Speaker Distance ..."
  (cm, sent as a delay like Windows' calibration). Levels and distance
  checked by ear; crossover and sub gain want a subwoofer to hear.
- **Headphone profiles**: "Headphone Profile" picks one of Sound Blaster
  Command's 32 headphone curves (Audio-Technica ATH-M50, Sennheiser HD 280
  Pro, Sony MDR-1R, Creative's own ...). The curve is uploaded into the DSP
  as Windows does and is active with Direct Mode off, on headphones, for
  48 kHz streams; off on speakers, back on when you switch to headphones.
  Tested with real ATH-M50s.
- **`ae9-get-firmware` reads Creative's installer**: `sudo ae9-get-firmware
  AECMDMasterInstaller_3.4.92.00.exe` installs the DSP firmware and the 32
  headphone curves in one go (needs `innoextract`, in Fedora's, Arch's and
  Debian's repos; the installer is only unpacked, never run). No Windows
  install needed. `CtxHda.sys` still works for the firmware alone;
  `--check` also counts the headphone curves.
- **Fix:** turning Direct Mode back on while a stream was still waiting to
  move into the DSP (e.g. 44.1 kHz, which stays direct) silenced it until
  the player reopened. It keeps playing now.
- Decoder (`docs/DECODED-SEQUENCES.md`): speaker levels, delays,
  crossover, sub gain and the headphone EQ named with their units.

## 0.9.9 (2026-11-06)
Fedora packages: install with dnf, updates with your kernel. 📦
Pairs with [SbAe9-SBC v0.11.2](https://github.com/3xHelix/SbAe9-SBC), as 0.9.8.

- **Packages for Fedora and Fedora Atomic** in the COPR repository
  `3xhelix/SbAe9-Drv`: `sudo dnf copr enable 3xhelix/SbAe9-Drv && sudo dnf
  install creative-ae9`. akmods rebuilds the module for every new kernel, so
  nothing to redo after a kernel update. On Silverblue, Kinoite and the other
  rpm-ostree desktops: `rpm-ostree install creative-ae9` (README). The
  package also sets up the boot binding and the PipeWire profiles.
- **`ae9-get-firmware`**: one command extracts the DSP firmware from your
  copy of Creative's driver, checks it and installs it
  (`sudo ae9-get-firmware /path/to/CtxHda.sys`; `--check` shows what's
  there). On Fedora Atomic it uses `/etc/firmware` and sets the kernel
  option that points there.
- **The boot binding finds the card by its IDs**: `config/ae9-override.conf`
  no longer has the PCI address written in, so it works in any slot with
  nothing to edit. Hand installs: copy the new file over the old one.
- No driver code changes: the module is the same as 0.9.8.

-- 3xHelix

## 0.9.8 (2026-11-06)
AE-9s behind an ASMedia bridge work. 🎉
Pairs with [SbAe9-SBC v0.11.2](https://github.com/3xHelix/SbAe9-SBC).

- **ASMedia-bridge cards play.** AE-9s come with one of two PCIe-to-PCI
  bridges: IDT Tsi381 (`10e3:8111`) or ASMedia ASM1083/1085 (`1b21:1080`).
  The ASMedia version used to drop off the bus during start-up. The bridge
  locks up after a burst of register writes with no read in between, and
  the driver wrote in such bursts. Behind an ASMedia bridge it now reads
  every register back right after writing it, as Windows' driver does
  (`posted_flush`, -1 = auto, the default). Also automatic behind ASMedia:
  the command rings start in Windows' order (`ring_order`), every DMA
  buffer gets 64 KB of spare room for the bridge's read-ahead (`dma_pad`),
  and the bus reset at start-up is skipped (`bus_reset`). Tested on an
  ASM1083 rev 03 card from a cold boot: start-up, playback, recording,
  5.1 and optical. Thanks to Kaiba for testing round after round, and to
  DarkNikGr for finding the write-burst lock-up.
- If the card drops off the bus anyway, the driver now stops cleanly and
  logs where, instead of hanging the machine.
- IDT-bridge cards are unchanged: everything above stays off there.
- Debug options for bridge trouble reports: `bridge_log=1` (bridge status
  at each start-up step), `corb_rings`, `fw_rings_off`, `stream_rings_off`,
  `ring_irq`, `fw_posbuf`. Not needed on any card tested so far.

-- 3xHelix

## 0.9.7 (2026-11-06)

Every rate Windows plays, and a lighter driver. 🧹
Pairs with [SbAe9-SBC v0.11.0](https://github.com/3xHelix/SbAe9-SBC), as 0.9.5.

- **176.4 kHz playback** (stereo, Direct Mode), through the card's
  sample-rate converter like 44.1/88.2 kHz, with the values from Windows'
  own driver. The AE-9 now plays 44.1, 48, 88.2, 96, 176.4, 192 and
  384 kHz. Add 176400 to PipeWire's `clock.allowed-rates` to use it;
  `pcm_176k4=0` leaves it out.
- **Cleanup:** 23 old module options are gone. Each only switched back to
  an earlier, replaced start-up path (`bar2_first`, `cold_prefw`,
  `codec2_first`, `prep_windows`, `full_init`, `init_mask`, `vfio_mask`,
  `fw_split`, `dl_windows`, `dl_reset_chunks`, `dl_bdl64`, `hs_immediate`,
  `tail_drop_rejected`, `scp_win_read`, `chipio_nocache`,
  `ca0113_windows`, `pll_windows`, `hu_dsp_keepalive`,
  `acm_first_delay_ms`, `acm_pass_gap_ms`, `acm_passes`,
  `acm_tx_settle_us`, `acm_i2c_debug`). Nothing changes with the defaults;
  a leftover one in modprobe.d only logs "unknown parameter".

-- 3xHelix

## 0.9.6 (2026-11-06)

A fix for 44.1/88.2 kHz, found with the dump script's new check. 🩹
Pairs with [SbAe9-SBC v0.11.0](https://github.com/3xHelix/SbAe9-SBC), as 0.9.5.

- **Fixed: 44.1 and 88.2 kHz went silent after a 384 kHz stream**, and stayed
  silent (also after idle) until something played at 48, 96 or 192 kHz. The
  384 kHz stream left the card's I2S on its 384 kHz clocks, where the
  sample-rate converter these rates use won't start. A 44.1/88.2 kHz start
  now sets the I2S to its 48 kHz clocks first, as Windows' driver does.
  `src_reclock=0` goes back to the old behaviour.
- `scripts/ae9-full-dump.sh` reads a few more card registers (the I2S check
  registers Windows' driver uses, and some power registers), to compare
  against the AE-9's usual values when something goes quiet.

-- 3xHelix

## 0.9.5 (2026-11-06)

Phantom power from the desktop, What U Hear at 96 kHz, and a big tidy-up. 🎙️
Pairs with [SbAe9-SBC v0.11.0](https://github.com/3xHelix/SbAe9-SBC), released
alongside: its phantom power button (red when on, with a warning first) and
a What U Hear rate picker (48/96 kHz).

- **Phantom power can be switched from the PC.** "Head Unit Phantom Power"
  shows and switches the head unit's 48 V phantom power for its XLR/TRS
  jack; the head unit's own button works too. Windows' software can only
  read it; the switch command was found on the card. It always starts off,
  after boot and after waking, and "on" is refused for the first 30 s so a
  restored mixer state can't turn 48 V on by itself. 48 V can damage some
  gear (ribbon mics, some line-level sources): only turn it on for mics
  that need it.
- **What U Hear records at 48 or 96 kHz** (`hw:AE9,2`, `wuh_96k`, default
  on), level-exact with the card playing at 48 or 96 kHz.
- **Cleanup:** the old pre-Windows start-up path is gone (about 5,700
  lines), with the module options only it used: `post_windows`,
  `enable_audio_path`, `hu_boot_ms`, `loop_start`, `lineout_prerun`. A
  leftover one in modprobe.d only logs "unknown parameter". Nothing changes
  with the defaults.
- Debug: `huframe <hex bytes>` in the debugfs `cmd` file sends one raw
  head-unit frame and shows the reply.

-- 3xHelix

## 0.9.4 (2026-11-06)

More rates, more surround, and Direct Mode that switches cleanly. 🎚️
All found by reading a fresh Windows capture overnight, then tested on the
card. Pairs with [SbAe9-SBC v0.10.0](https://github.com/3xHelix/SbAe9-SBC),
released alongside: its rate picker offers each mode's rates.

- **44.1 and 88.2 kHz playback works, on by default** (stereo). The DAC
  needed three commands Windows sends only for that family; without them
  these rates played garbled or silent. `pcm_rates=2` is the new default
  (1 = without 44.1/88.2, 0 = 48 kHz only); `dac_441` (1) sends the
  commands. Add 44100 and 88200 to PipeWire's `clock.allowed-rates` and
  44.1 kHz music plays at its own rate.
- **5.1 at 192 kHz** in Direct Mode, all six jacks right. The router
  layout is Windows' own (our 96 kHz one, extended). `surround_192k` (1);
  384 kHz stays stereo, as on Windows. 5.1 opens at 48, 96 or 192 kHz.
- **96 kHz through the DSP** (Direct Mode off), stereo and 5.1, with SBX.
  `dsp_96k` (1; 0 = 48 kHz only, as before).
- **Direct Mode on while playing:** the stream now restarts by itself and
  plays on, direct. Before, it went silent until you paused and played.
- **Fixed: a left/right echo in DSP mode** after a 5.1 (or 96 kHz+) stream
  in Direct Mode. Leftover direct routes played one channel twice; DSP mode
  now switches them off. The same leftovers caused the old 96 kHz echo.
- `scripts/ae9-surround-test.sh` takes `RATE=96000` or `RATE=192000` with
  `--direct`.

-- 3xHelix

## 0.9.3 (2026-11-06)

The head unit learns what's plugged into it. 🎧

- Plug controls: "Head Unit 3.5mm Phones Plugged" (the head unit's 3.5 mm
  headphone jack) and "Head Unit 3.5mm Mic Plugged" (its 3.5 mm mic jack),
  read-only, with change events so apps can follow them live. The 1/4"
  headphone jack and the XLR/TRS jack don't report plugs.
- Auto headphone switch, opt-in: "Head Unit Auto Switch" (off by default,
  `hu_auto_switch=1` to start on). Plug into the head unit's 3.5 mm
  headphone jack = headphones, unplug = speakers, once the plug has held
  still about 1.5 s, so a wiggly plug can't flap. It acts once per plug
  change: a knob switch afterwards stays. Off, plugs only log, as before.
- **Renamed: "XLR Gain" is now "Mic Gain"** (`Mic Gain Capture Volume`). It
  boosts both head unit jacks, XLR/TRS and 3.5 mm mic (checked: +20 dB on
  each). Scripts using `sset 'XLR Gain'` need the new name; a saved setting
  starts back at the default +10 dB.
- Names say where each jack lives (head unit vs card): the optional desktop
  name in `config/61-ae9-names.conf` is now "AE-9 Head Unit Mic / Aux",
  the log says "3.5 mm mic plug", and the docs name the card's RCA, Rear and
  C/Sub jacks and the head unit's 3.5 mm and 1/4" headphone jacks.
- `scripts/ae9-full-dump.sh` leaves an "ae9-full-dump: saved <file>" line
  in the kernel log, so a later log shows a dump was taken and where it went.

## 0.9.2 (2026-11-06)

Tidy-ups, and a tool for anyone who hits a problem. 🧰

- `scripts/ae9-full-dump.sh` ships with the driver: one command that saves
  everything we can read about the card, for bug reports. It now also covers
  the optical, What U Hear and extra capture streams.
- Debug interface tidy-ups: `chipiow` is serialised like `chipior`, the
  4-argument immediate-command `verb` is now `icwverb` (it could never run),
  and `w0`/`w2` print the value written.
- Waking from sleep with the knob light off: the light is now switched off
  again by the head unit's regular poll, instead of a separate write that
  could collide with it.

## 0.9.1 (2026-11-06)

The mic gets the same treatment as the speakers. 🎙️

- Recording at 96 kHz too (head unit's XLR and 3.5 mm input; Creative's
  limit). The trick was the order: the codec takes its rate when it's
  bound to the stream, so the recording's format now goes in before the
  bind, as Windows does. Before that a 96 kHz recording got 48 kHz-rate
  data and played back twice as fast.
- 24- or 32-bit recording, as Windows offers ("Capture Bit Depth"). At
  32-bit the card sends full 32-bit samples (the mic path runs through the
  DSP); it sounds the same, the extra bits sit below the mic's noise floor.
- "Aux" solved: Windows' Aux device is the head unit's 3.5 mm line-in,
  which shares the XLR/TRS input, so it already worked.
  `config/61-ae9-names.conf` names that input "AE-9 Mic / Line In" in the
  desktop.

## 0.9.0 (2026-11-06)

Now it can hear itself. 👂

- What U Hear: a new recording device, `hw:AE9,2`, that records what the
  card plays, in Direct Mode and through the DSP. A test tone comes back
  level-exact, both channels identical. `config/ae9-what-u-hear.conf`
  adds it to PipeWire as "AE-9 What U Hear". 48 kHz for now. Another
  pointer from DarkNikGr's notes: it's the loopback input the kernel's
  ca0132 driver already knows, on a node Windows switches on at boot.
- Under the hood: optical in and What U Hear share one capture setup.

## 0.8.0 (2026-11-05)

The optical jacks wake up. 🔦

- Optical S/PDIF, both ways. Turns out the optical port was already lit
  and locked the whole time, just sending silence: it lives on the card's
  second codec, the boot sequence switches it on, and nothing ever gave
  it any audio. Now:
  - Optical out plays what the main output plays (stereo, 24-bit, up to
    192 kHz; silent at 32-bit, 384 kHz and in 5.1). "Optical Out Playback
    Switch" turns it off. No SBX on it, and the knob doesn't change its
    level: your receiver does.
  - Optical in is a new recording device, `hw:AE9,1`, 44.1 to 192 kHz,
    next to the XLR mic. `config/ae9-optical-in.conf` adds it to
    PipeWire as "AE-9 Optical In". Record at the sender's rate.
  Tested with a USB codec looped both ways, thanks to a tip from
  DarkNikGr's notes on where the optical port sits.
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

-- 3xHelix

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

-- 3xHelix

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

-- 3xHelix

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

-- 3xHelix

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

-- 3xHelix

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

-- 3xHelix

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

-- 3xHelix

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

-- 3xHelix

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

If you've got an AE-9 and Linux, try it and tell me what breaks. -- 3xHelix

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
