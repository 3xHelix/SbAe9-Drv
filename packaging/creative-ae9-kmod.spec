# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
# akmod for the Creative Sound Blaster AE-9 driver: akmods rebuilds the module
# for every new kernel on the user's machine (also on Fedora Atomic, through
# rpm-ostree). The firmware is Creative's and not included: see
# ae9-get-firmware in the creative-ae9 package.
%global buildforkernels akmod
%global debug_package %{nil}
%global kmod_name creative-ae9
%global srcname SbAe9-Drv

Name:           %{kmod_name}-kmod
Version:        0.13.0
Release:        1%{?dist}
Summary:        Kernel module for the Creative Sound Blaster AE-9
License:        GPL-2.0-or-later
URL:            https://github.com/3xHelix/%{srcname}
Source0:        %{url}/archive/v%{version}/%{srcname}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  elfutils-libelf-devel
BuildRequires:  kmodtool

# kmodtool writes the akmod/kmod sub-packages
%{expand:%(kmodtool --target %{_target_cpu} --kmodname %{kmod_name} %{?buildforkernels:--%{buildforkernels}} %{?kernels:--for-kernels "%{?kernels}"} 2>/dev/null) }

%description
Out-of-tree driver for the Creative Sound Blaster AE-9 (CA0132 Sound Core3D
DSP and the ACM head unit): playback up to 384 kHz, 5.1, SBX effects through
the DSP, recording, optical in/out, What U Hear, and the head unit's display,
knob and buttons. Works behind both bridges the card ships with (IDT and
ASMedia).
Sound Blaster AE-7 support (experimental, opt-in: ae9-ae7-enable in the
creative-ae9 package).

This package contains the kmod module (snd-hda-codec-creative-ae9).

%prep
%{?kmodtool_check}
kmodtool --target %{_target_cpu} --kmodname %{kmod_name} %{?buildforkernels:--%{buildforkernels}} %{?kernels:--for-kernels "%{?kernels}"} 2>/dev/null
%setup -q -c
for kernel_version in %{?kernel_versions}; do
	cp -a %{srcname}-%{version} _kmod_build_${kernel_version%%___*}
done

%build
for kernel_version in %{?kernel_versions}; do
	make V=1 %{?_smp_mflags} -C ${kernel_version##*___} M=${PWD}/_kmod_build_${kernel_version%%___*} modules
done

%install
for kernel_version in %{?kernel_versions}; do
	install -D -m 755 _kmod_build_${kernel_version%%___*}/snd-hda-codec-creative-ae9.ko \
		%{buildroot}%{kmodinstdir_prefix}/${kernel_version%%___*}/%{kmodinstdir_postfix}/snd-hda-codec-creative-ae9.ko
done
%{?akmod_install}

%changelog
* Wed Nov 25 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.13.0-1
- Driver 0.13.0 "AE-7 experimental": Sound Blaster AE-7 support
  (experimental, opt-in: ae9-ae7-enable). With it on, the driver runs an
  AE-7 (ae7=1): stereo and 5.1 playback, DSP mode at 48 kHz, Direct Mode
  at 44.1-384 kHz, mic, What U Hear, SBX and CrystalVoice. Off by default:
  an AE-7 stays with the in-tree driver until ae9-ae7-enable is run. Off
  again (ae9-ae7-enable --disable, which removing the creative-ae9 package
  also runs with it on), an AE-7 with an ASMedia bridge, or one the script
  doesn't recognise, is left with no driver (parked): the in-tree driver
  hangs ASMedia AE-7s at boot. New read-only "Card Model" control on every
  card. The AE-7's PipeWire
  profile set and udev rule are installed for everyone (they match only
  an AE-7 this driver runs); its opt-in files go to
  /usr/share/creative-ae9/ae7/. ae9-reload and ae9-full-dump work on
  either card; the AE-9-only tools leave an AE-7 alone.

* Sat Nov 21 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.12.17-1
- Driver 0.12.17: DAC register reads are single (0.12.16's status checks
  made the triple read unnecessary); Driver Health has a 9th value, late
  DAC I2C NACKs (0 on a healthy card); stream and command-ring DMA stopped
  before a bus reset on retry and resume; the bridge log shows the
  discard-timer and AER secondary status, decoded, once and on change; a
  "card alive" check of the controller's ID registers after the bus
  reset refuses a card that didn't come back; debugfs "DMA FIFO" line and
  "readahead" sampler. A read-ahead pointer was tried and dropped. 21
  module options, no default changes.

* Sat Nov 21 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.12.16-1
- Driver 0.12.16: every card but the AE-9 is refused before it is touched,
  even through a driver_override or new_id, with one log line naming it.
  DAC I2C NACK detection: a 1-2 ms wait after the DACs leave reset (the
  start-up mute lost on every load now gets through); a refused DAC
  command is sent once more, a refused volume write again, the playback
  setup redone, DSD stays muted if its DAC switch failed; checked DAC
  reads (the one-behind read lag is gone); the shutdown's expected NACKs
  counted, not retried; debugfs "DAC I2C" line and dacabrt. 21 module
  options, no default changes.

* Fri Nov 20 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.12.15-1
- Driver 0.12.15: native DSD64 on by default (pcm_dsd=0 turns it off; a
  file with pcm_dsd=1 keeps it on). MPD on hw:AE9,0 no longer crackles at
  44.1 kHz (4 periods with its default buffer) and buffer times a player
  asks for are no longer refused; the head-unit volume no longer crosses
  outputs after a reload; DSD stop/abort/unload hardened, DSD reports 32
  significant bits; no "unlocked secondary bus reset" warning and no
  phantom-power alsactl error; debugfs and ae9-full-dump clearer; the
  WirePlumber 0.4 .lua only where WirePlumber is 0.4; 21 module options
  (4 removed, 5 test hooks only in AE9_TEST=1 builds).

* Fri Nov 20 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.12.14-1
- Driver 0.12.14: native DSD64 (experimental, opt-in with pcm_dsd=1): DSD
  straight to the DAC's DSD decoder, as Creative's Windows DSD driver does
  it. Optical in no longer restarts falsely; optical-in and head-unit
  counters in debugfs; the head unit logs "not answering" once; 8 options
  removed (always on); firmware checked by SHA-256; ae9-reload keeps a
  forced PipeWire rate/quantum; ae9-full-dump --report packs a .tar.gz with
  versions, the driver log and the PCI path.

* Wed Nov 18 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.12.13-1
- Driver 0.12.13: robustness release. Head-unit UART checks before each byte
  and before a line-control change (as Windows); a headphone-amp safety net
  that resends the jack frame if the head unit's amp disagrees; the Direct
  Mode slot walk waits for the router chip; ae9-reload leaves a notice so
  Sound Blaster Command and AudioMon let go of the card; 32-bit DMA (as
  Windows).

* Tue Nov 17 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.12.12-1
- Driver 0.12.12: every command list the driver sends the card is now
  readable C instead of replayed tables (the card gets exactly the same
  commands as 0.12.11, proven offline and on the card); a late card answer
  is caught when it lands instead of timing out; router-chip values only
  from the task that selected the setting; 42 module options removed
  (diagnostics, always-on fallbacks, fixed timings), 38 left.

* Sun Nov 15 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.12.11-1
- Driver 0.12.11: DSP mode plays at full level (it was 6 dB quiet and only
  half the samples reached the DACs; DSP mode is now 6 dB louder at the
  same volume, headphones too), the router chip is no longer left half set
  up after a power-on, the mic no longer sounds like a chipmunk after a
  96/192 kHz stream, DSP mode no longer goes silent after rate changes,
  unloading with a hung card no longer crashes, safer DMA and a hardened
  command ring, new "Driver Health" control.

* Sun Nov 15 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.12.10-1
- Driver 0.12.10: no more card hangs after effect changes in DSP mode
  (answers were read before they reached memory, the Creative controller
  quirk Linux works around too), headphone profiles load while audio
  plays, a stopped card stops its streams cleanly, source installs load
  the driver early at boot

* Sat Nov 14 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.12.9-1
- Driver 0.12.9: "AE-9" at start-up again (0.12.8 blanked it), new
  ae9-reload command (reload the driver in one go), "Card Answering"
  control so apps let go of a stopped card, unload no longer wedges a
  card that answers again, BAR2 0x1c logged (bit 9 = second codec off)

* Sat Nov 14 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.12.8-1
- Driver 0.12.8: head unit catches its start-up handshake (board D0
  step), sees 3.5 mm unplugs (reply length), shows "AE-9" at start-up and
  is set up again after a power dip or stop frame, SBX held 5 s switches
  its display; rear RCA input and optical work after a reload (codec 2
  woken); the driver stops when the card stops answering

* Thu Nov 12 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.12.7-1
- Driver 0.12.7: the card's mic monitor no longer breaks Voice Clarity
  until a power-off (monitor off restores the DSP's mic route), optional
  +30 dB Mic Gain (mic_boost_30=1), optional clean-mic PipeWire example,
  source install puts the tools in /usr/local/bin and refuses next to a
  packaged build, DAC register read/write in debugfs
* Wed Nov 11 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.12.6-1
- Driver 0.12.6: What U Hear no longer stalls after a Direct Mode switch
  (Direct starts follow the router chip's slot list), router stream counts
  released at shutdown/start-up, leftover DSP-mode routes switched off
  (gated/metallic analog out until a power-off), DSP output unmute read
  back and resent, debugfs state readout in ae9-full-dump
* Tue Nov 10 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.12.5-1
- Driver 0.12.5: restarts fixed for real (router stream count kept as
  Windows), bus reset on ASMedia-bridge cards too, a DSP word refused as
  full is resent until it goes in, WirePlumber 0.4 safe start volume
  (51-ae9-safe-volume.lua), optical without 176.4 kHz
* Mon Nov 09 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.12.4-1
- Driver 0.12.4: warm restarts as Windows (wake + recovery cycle), Voice
  Clarity starts in Windows' order (no DSP stall), Voice Clarity off goes
  back to full band, mic monitor crunch fixed, Windows' DSP load retry,
  rear RCA without 176.4 kHz
* Mon Nov 09 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.12.3-1
- Driver 0.12.3: code-review fixes (custom headphone profiles, speaker setup
  on headphones, mic monitor and Voice Clarity lifecycle, scripts); mic
  monitor / Voice Clarity known issue flagged ("Mic Monitor Used Since
  Power-up")
* Mon Nov 09 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.12.2-1
- Driver 0.12.2: Voice Clarity, Voice Morphing and Mic EQ on the DSP, mic
  monitoring, headphone and line-out virtualization, custom headphone
  profiles (ae9-hp-profile)
* Sun Nov 08 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.12.1-1
- Driver 0.12.1: optical out is its own output by default (mirror is a
  switch), What U Hear in 5.1, clearer headphone-profile log
* Sun Nov 08 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.12.0-1
- Driver 0.12.0: separate optical output with passthrough (DarkNikGr),
  headphone-profile uploads no longer cut short, quieter login, DKMS for
  every installed kernel
* Sun Nov 08 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.11.3-1
- Driver 0.11.3: no DSP control write from a bad read; snapshot marks bad reads
* Sun Nov 08 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.11.2-1
- Driver 0.11.2: ae9-dsp-snapshot and ae9-read-stress commands for reports
* Sun Nov 08 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.11.1-1
- Driver 0.11.1: builds on kernels before 6.13 again (Ubuntu 24.04, EPEL 10)
* Sun Nov 08 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.11.0-1
- Driver 0.11.0: safe headphone-profile uploads, EQ headroom, output switch
  fix in DSP mode, 7 DAC filters, optical in without a signal, What U Hear /
  Rear RCA In / Optical In as card-profile sources, -36 dB start volume
* Sat Nov 07 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.10.1-1
- Driver 0.10.1: early module load (modules-load.d), Rear RCA In, reliable
  headphone profiles, firmware in creative-ae9/, Sub Boost switch

* Sat Nov 07 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.10.0-1
- Driver 0.10.0: speaker setup, headphone profiles, Direct Mode fix

* Fri Nov 06 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.9.9-1
- First package: akmod for Fedora and Fedora Atomic (driver 0.9.9 = 0.9.8 code)
