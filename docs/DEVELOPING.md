# Developing

How the driver is built and where its knowledge comes from. Back to the
[README](../README.md). Building and testing a change:
[INSTALL-FROM-SOURCE.md](INSTALL-FROM-SOURCE.md#testing-a-new-build).

## Where this comes from

The driver replays what Creative's Windows driver does, recorded from a
Windows VM with the card passed through (VFIO). The capture data, analysis
and decode tools live in the development repository; the
`creative_ae9_seq.c` and `creative_ae9_seq_run.c` here hold everything the
driver uses from them, as code. Creative's Windows driver files are
not included, and none of its code is. Each source file names what it
adapted, with the original copyright notices.

## Source layout

| File | Purpose |
|---|---|
| `creative_ae9_core.c` | PCI probe, bus reset, HDA reset, codec detection, the bring-up (probe, retries, resume) and the start-up tail's replay, router and DSP-mode routing, head-unit poll/display/knob and ALSA volume controls, debugfs, module options |
| `creative_ae9_dsp.c` | DSP firmware download, MASTERCONTROL boot handshake, overlay transfer, SCP messaging, CA0113 (head-unit DAC) commands, Voice Clarity / Voice Morphing / Mic EQ and mic monitoring |
| `creative_ae9_i2c.c` | Head-unit frames over the card's UART (the SPI/I2C names in it are historical) |
| `creative_ae9_corb.c` | CORB/RIRB DMA ring + verb I/O, the "card stopped answering" stop |
| `creative_ae9_pcm.c` | ALSA PCM playback, mic capture, optical out (device 4, or a mirror of playback), optical in (device 1), What U Hear (device 2), rear RCA in (device 3) |
| `creative_ae9_fx.c` | SBX effects, EQ, Direct Mode and SBX controls, speaker setup, headphone profiles and headphone virtualization |
| `creative_ae9.h`, `ca0132_regs.h` | Registers, chip struct, MMIO accessors; definitions ported from the in-tree ca0132 driver |
| `creative_ae9_seq.c` | Sequences recorded from the Windows driver, written as code, the start-up's (sent verb by verb): the start-up before the firmware download (with the boot checks) and the verbs at the top of every download, the warm-DSP reset (recovery cycle), the start-up tail (Windows' set-up after the DSP's boot handshake) |
| `creative_ae9_seq_run.c` | The rest of them, sent as blocks or to the head unit: recording start, level and stop, playback start and stop, DSP mode's blocks (Direct Mode off and on, a stream's start in DSP mode, a stream's release), the speaker/headphone switch, Windows' shutdown, the head unit's and DACs' set-up after the DSP's start, Windows' head-unit poll (shared with the heartbeat) and its set-up resend, and Windows' BAR2 init at every start-up |
| `creative_ae9_seq.h` | The names those two files share |
| `creative_ae9_board.h` | The board layer (since 0.13.0): what a board has (head unit, inputs, SBX, Direct Mode, ...), the AE-7's state, and the hooks where the AE-7 takes over from the AE-9 code |
| `creative_ae7.h`, `creative_ae7_*.c` | The Sound Blaster AE-7 (experimental, [AE7.md](AE7.md)), compiled into every build: each `.c` is `#include`d at the end of the AE-9 file whose code it uses. Its start-up as Windows sends it (`_seq.c`), output select, volume, the DAC's mute gate and Direct Mode (`_run.c`), playback, recording and What U Hear (`_pcm.c`), SBX's output settings and Speaker Config (`_fx.c`), the board, the `ae7` option, controls and lifecycle (`_core.c`) |
| `extract_firmware.py`, `extract_overlay.py` | Extract the DSP firmware and overlay from Creative's `CtxHda.sys` |
| `extract_hpeq.py` | Extracts the headphone profiles from Creative's installer (`AE9HPEQ/*.reg`) |
| `dkms.conf`, `scripts/dkms-install.sh` | DKMS build and the source install |
| `config/` | Boot binding (udev rule, modprobe hook, dracut omit, early load), PipeWire profiles and optional extras |
| `config/60-ae9-headroom.conf` | Optional fixed PipeWire headroom for small quantums (see [Low latency](USING.md#low-latency)) |
| `scripts/ae9-headroom.sh`, `config/ae9-headroom.service` | Optional per-rate PipeWire headroom, as a user service |
| `config/61-ae9-names.conf` | Optional desktop names: "AE-9 Head Unit Mic / Aux", "AE-9 Rear RCA In", "AE-9 What U Hear" (stereo and 5.1), "AE-9 Optical In", "AE-9 Optical Out", "AE-9 Analog Out"; on an AE-7 "AE-7 Mic-in", "AE-7 What U Hear", "AE-7 Analog Out" |
| `config/ae9-clean-mic.conf` | Optional "AE-9 Mic (clean)" input: the mic through PipeWire's WebRTC noise suppression |
| `config/50-ae9-safe-volume.conf`, `config/51-ae9-safe-volume.lua` | Installed by the packages: the AE-9's outputs (and the AE-7's) start at -36 dB, the driver's boot volume, when WirePlumber has no saved volume for them (`.lua` for WirePlumber 0.4, where it applies to every output with no saved volume: quieter, never louder; WirePlumber 0.5 warns about a `.lua`, so the .deb links it into `main.lua.d` only when the installed WirePlumber is 0.4, and the RPMs ship it only for openSUSE Leap 15) |
| `config/ae9-optical-in.conf`, `config/ae9-what-u-hear.conf`, `config/ae9-rear-rca-in.conf` | 0.10.x only: hand-copied PipeWire sources. Since 0.11.0 the card profile opens them; don't copy these any more |
| `scripts/ae9-reload.sh` | `ae9-reload`: reloads the driver in one go |
| `scripts/ae9-hp-profile` | Makes and installs your own headphone profiles |
| `scripts/ae9-get-firmware.sh` | `ae9-get-firmware`: installs the firmware and headphone profiles from Creative's installer |
| `scripts/ae9-full-dump.sh`, `scripts/ae9-dsp-snapshot.sh`, `scripts/ae9-read-stress.sh` | Read-only reports (see [Troubleshooting](TROUBLESHOOTING.md#reporting-a-problem)) |
| `scripts/ae9-optical-passthrough` | Dolby Digital / DTS passthrough to a receiver |
| `scripts/ae9-surround-test.sh` | Speaks each 5.1 channel's name |
| `scripts/restore-stock-audio.sh` | Removes the driver and restores stock `snd_hda_intel` (an AE-7 not behind a Tsi381 is parked; `--stock` hands it over) |
| `scripts/ae9-ae7-enable`, `config/ae7/00-ae7-override.rules`, `config/ae7/ae7-override.conf` | `ae9-ae7-enable`: turns the AE-7 support on or off: the two files hand an AE-7 to this driver and set `ae7=1`; shipped inactive. Off parks an AE-7 not behind a Tsi381 bridge (no driver: `snd_hda_intel` hangs ASMedia AE-7s at boot); the script writes the park pair, `00-ae7-park.rules` and `ae7-park.conf`, itself |
| `config/creative-ae7.conf`, `config/89-ae7-pipewire.rules` | The AE-7's PipeWire profile set and its udev rule (act only on an AE-7 this driver runs) |
| `packaging/` | Fedora/COPR specs (akmod, DKMS), the Debian package builder |

## Test builds

`make AE9_TEST=1` builds a test version of the driver with five
fault-injection options for exercising its error paths: `dsp_fail_inject`,
`hung_inject`, `scp_full_inject`, `rirb_inject` and `rirb_inject_us`
(described in [OPTIONS.md](OPTIONS.md#test-hooks-test-builds-only)). Each
acts only with `debug=1` or more. Since 0.13.0 a test build also has two
AE-7 test options, `ae7_test` (the test kits' narrower levels) and
`ae7_lanes` (the Rear and C/Sub channel map), described in the same
section. A release build (a plain `make`, DKMS, the packages) has none of
these options nor their code (since 0.12.15; the AE-7's since 0.13.0):
`modinfo -p` lists 22 options there (with 0.13.0's `ae7`), 29 in a test
build.
