# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
# The AE-9 driver as one DKMS package for RPM distributions without akmods:
# openSUSE (Tumbleweed, Slowroll), RHEL/AlmaLinux/Rocky 10 with EPEL's dkms.
# Fedora uses the COPR akmod packages (creative-ae9, akmod-creative-ae9)
# instead; this conflicts with them. Plain paths, no distro-specific macros,
# so the same .noarch.rpm installs everywhere.
%global srcname SbAe9-Drv
%global modname snd-hda-codec-creative-ae9

Name:           creative-ae9-dkms
Version:        0.12.8
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
install -D -m 644 config/51-ae9-safe-volume.lua %{buildroot}/usr/share/wireplumber/main.lua.d/51-ae9-safe-volume.lua
install -D -m 755 scripts/ae9-get-firmware.sh %{buildroot}/usr/bin/ae9-get-firmware
install -D -m 755 scripts/ae9-full-dump.sh %{buildroot}/usr/bin/ae9-full-dump
install -D -m 755 scripts/ae9-dsp-snapshot.sh %{buildroot}/usr/bin/ae9-dsp-snapshot
install -D -m 755 scripts/ae9-read-stress.sh %{buildroot}/usr/bin/ae9-read-stress
install -D -m 755 scripts/ae9-optical-passthrough %{buildroot}/usr/bin/ae9-optical-passthrough
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
%doc README.md CHANGELOG.md
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
%dir /usr/share/wireplumber/main.lua.d
/usr/share/wireplumber/main.lua.d/51-ae9-safe-volume.lua
/usr/bin/ae9-get-firmware
/usr/bin/ae9-full-dump
/usr/bin/ae9-dsp-snapshot
/usr/bin/ae9-read-stress
/usr/bin/ae9-optical-passthrough
/usr/bin/ae9-hp-profile
/usr/share/creative-ae9/

%changelog
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
