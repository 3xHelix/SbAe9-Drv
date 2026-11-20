# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
# The AE-9 driver as one DKMS package for RPM distributions without akmods:
# openSUSE (Tumbleweed, Slowroll), RHEL/AlmaLinux/Rocky 10 with EPEL's dkms.
# Fedora uses the COPR akmod packages (creative-ae9, akmod-creative-ae9)
# instead; this conflicts with them. Plain paths, no distro-specific macros,
# so the same .noarch.rpm installs everywhere.
%global srcname SbAe9-Drv
%global modname snd-hda-codec-creative-ae9
# WirePlumber 0.4 reads Lua (main.lua.d), 0.5 only the .conf and warns
# "Old configuration file detected" for a .lua there: the .lua only where
# the distribution's WirePlumber is 0.4 (openSUSE Leap 15); Tumbleweed,
# Leap 16 and EL10 have 0.5
%if 0%{?sle_version} && 0%{?sle_version} < 160000
%global wp04 1
%else
%global wp04 0
%endif

Name:           creative-ae9-dkms
Version:        0.12.15
Release:        1
Summary:        Creative Sound Blaster AE-9 driver (DKMS)
License:        GPL-2.0-or-later
URL:            https://github.com/3xHelix/%{srcname}
Source0:        %{url}/archive/v%{version}/%{srcname}-%{version}.tar.gz
BuildArch:      noarch

Requires:       dkms
Requires:       python3
Requires:       make
Requires:       gcc
Recommends:     innoextract
Conflicts:      creative-ae9
Conflicts:      akmod-creative-ae9

%description
Out-of-tree Linux driver for the Creative Sound Blaster AE-9: playback up to
384 kHz, 5.1, SBX/DSP effects and EQ, speaker setup, headphone profiles,
recording, optical in and out, What U Hear and the ACM head unit. Both card
versions (IDT and ASMedia bridge).

DKMS builds the module for every installed kernel that has its headers
(openSUSE: kernel-default-devel; RHEL family: kernel-devel). Also installs
the boot setup that hands the card to this driver, the PipeWire profile set,
ae9-get-firmware, ae9-full-dump, ae9-dsp-snapshot and ae9-read-stress. The DSP firmware is Creative's and not
included: run ae9-get-firmware with Creative's AE-9 installer afterwards.

%prep
%setup -q -n %{srcname}-%{version}

%build

%install
d=%{buildroot}/usr/src/%{modname}-%{version}
install -d $d
install -m 644 Makefile *.c *.h $d/
sed 's/^PACKAGE_VERSION=.*/PACKAGE_VERSION="%{version}"/' dkms.conf > $d/dkms.conf
install -D -m 644 config/00-ae9-override.rules %{buildroot}/usr/lib/udev/rules.d/00-ae9-override.rules
install -D -m 644 config/89-ae9-pipewire.rules %{buildroot}/usr/lib/udev/rules.d/89-ae9-pipewire.rules
install -D -m 644 config/ae9-override.conf %{buildroot}/usr/lib/modprobe.d/ae9-override.conf
install -D -m 644 config/ae9-omit.conf %{buildroot}/usr/lib/dracut/dracut.conf.d/ae9-omit.conf
install -D -m 644 config/ae9-modules-load.conf %{buildroot}/usr/lib/modules-load.d/creative-ae9.conf
install -D -m 644 config/creative-ae9.conf %{buildroot}/usr/share/alsa-card-profile/mixer/profile-sets/creative-ae9.conf
install -D -m 644 config/50-ae9-safe-volume.conf %{buildroot}/usr/share/wireplumber/wireplumber.conf.d/50-ae9-safe-volume.conf
%if %{wp04}
install -D -m 644 config/51-ae9-safe-volume.lua %{buildroot}/usr/share/wireplumber/main.lua.d/51-ae9-safe-volume.lua
%endif
install -D -m 755 scripts/ae9-get-firmware.sh %{buildroot}/usr/bin/ae9-get-firmware
install -D -m 755 scripts/ae9-full-dump.sh %{buildroot}/usr/bin/ae9-full-dump
install -D -m 755 scripts/ae9-dsp-snapshot.sh %{buildroot}/usr/bin/ae9-dsp-snapshot
install -D -m 755 scripts/ae9-read-stress.sh %{buildroot}/usr/bin/ae9-read-stress
install -D -m 755 scripts/ae9-optical-passthrough %{buildroot}/usr/bin/ae9-optical-passthrough
install -D -m 755 scripts/ae9-reload.sh %{buildroot}/usr/bin/ae9-reload
install -D -m 755 scripts/ae9-hp-profile %{buildroot}/usr/bin/ae9-hp-profile
install -d %{buildroot}/usr/share/creative-ae9/examples
install -m 644 extract_firmware.py extract_overlay.py extract_hpeq.py %{buildroot}/usr/share/creative-ae9/
for f in ae9-options.conf 60-ae9-headroom.conf 61-ae9-names.conf ae9-optical-in.conf ae9-what-u-hear.conf ae9-rear-rca-in.conf ae9-clean-mic.conf; do
	install -m 644 config/$f %{buildroot}/usr/share/creative-ae9/examples/
done

%post
dkms add -m %{modname} -v %{version} >/dev/null 2>&1 || :
for k in /lib/modules/*; do
	k=${k##*/}
	[ -e /lib/modules/$k/build ] || continue
	dkms install -m %{modname} -v %{version} -k "$k" || echo "creative-ae9-dkms: DKMS build for $k failed (see above)" >&2
done
udevadm control --reload >/dev/null 2>&1 || :
if ! ae9-get-firmware --check >/dev/null 2>&1; then
	echo "creative-ae9: the DSP firmware isn't included (it's Creative's). Install it, and"
	echo "the headphone profiles, from Creative's AE-9 installer (needs innoextract):"
	echo "  sudo ae9-get-firmware AECMDMasterInstaller_3.4.92.00.exe"
	echo "(or point it at CtxHda.sys), then power the PC fully off (PSU off) and on."
fi

%preun
# this version leaves DKMS on removal and on upgrade (the new one adds itself)
dkms remove -m %{modname} -v %{version} --all >/dev/null 2>&1 || :

%postun
udevadm control --reload >/dev/null 2>&1 || :

%files
%license COPYING
%doc README.md CHANGELOG.md docs
/usr/src/%{modname}-%{version}/
/usr/lib/udev/rules.d/00-ae9-override.rules
/usr/lib/udev/rules.d/89-ae9-pipewire.rules
/usr/lib/modprobe.d/ae9-override.conf
%dir /usr/lib/dracut
%dir /usr/lib/dracut/dracut.conf.d
/usr/lib/dracut/dracut.conf.d/ae9-omit.conf
/usr/lib/modules-load.d/creative-ae9.conf
%dir /usr/share/alsa-card-profile
%dir /usr/share/alsa-card-profile/mixer
%dir /usr/share/alsa-card-profile/mixer/profile-sets
/usr/share/alsa-card-profile/mixer/profile-sets/creative-ae9.conf
%dir /usr/share/wireplumber
%dir /usr/share/wireplumber/wireplumber.conf.d
/usr/share/wireplumber/wireplumber.conf.d/50-ae9-safe-volume.conf
%if %{wp04}
%dir /usr/share/wireplumber/main.lua.d
/usr/share/wireplumber/main.lua.d/51-ae9-safe-volume.lua
%endif
/usr/bin/ae9-get-firmware
/usr/bin/ae9-full-dump
/usr/bin/ae9-dsp-snapshot
/usr/bin/ae9-read-stress
/usr/bin/ae9-optical-passthrough
/usr/bin/ae9-reload
/usr/bin/ae9-hp-profile
/usr/share/creative-ae9/

%changelog
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
- First DKMS package: openSUSE and RHEL-family (EPEL dkms)
