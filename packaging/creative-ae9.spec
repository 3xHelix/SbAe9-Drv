# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
# The non-module half of the AE-9 driver: hands the card to the driver at
# boot (instead of snd_hda_intel), PipeWire profiles, the firmware helper and
# the dump script. Pulls in the akmod.
%global srcname SbAe9-Drv

Name:           creative-ae9
Version:        0.12.1
Release:        1%{?dist}
Summary:        Creative Sound Blaster AE-9 driver: boot setup, firmware helper, tools
License:        GPL-2.0-or-later
URL:            https://github.com/3xHelix/%{srcname}
Source0:        %{url}/archive/v%{version}/%{srcname}-%{version}.tar.gz
BuildArch:      noarch

BuildRequires:  systemd-rpm-macros
# the akmod/kmod packages want this
Provides:       %{name}-kmod-common = %{version}
Requires:       %{name}-kmod >= %{version}
# firmware extraction
Requires:       python3
# unpacks Creative's installer for ae9-get-firmware (firmware + headphone profiles)
Recommends:     innoextract

%description
Everything around the AE-9 kernel module except the module itself:
- a udev rule and a modprobe fallback that hand the AE-9 to this driver at
  boot instead of snd_hda_intel (found by its PCI IDs, any slot)
- keeps the module out of the initramfs (it needs the root filesystem's
  firmware)
- the PipeWire profile set (stereo duplex and 5.1)
- ae9-get-firmware: extracts the DSP firmware and the headphone profiles
  from your download of Creative's installer and installs them (also on
  read-only Fedora Atomic systems)
- ae9-full-dump: the state dump for bug reports
- ae9-dsp-snapshot, ae9-read-stress: DSP and chip-read checks for reports

The firmware itself is Creative's and not included.

%prep
%setup -q -n %{srcname}-%{version}

%build

%install
install -D -m 644 config/00-ae9-override.rules %{buildroot}%{_udevrulesdir}/00-ae9-override.rules
install -D -m 644 config/89-ae9-pipewire.rules %{buildroot}%{_udevrulesdir}/89-ae9-pipewire.rules
install -D -m 644 config/ae9-override.conf %{buildroot}%{_modprobedir}/ae9-override.conf
install -D -m 644 config/ae9-omit.conf %{buildroot}%{_prefix}/lib/dracut/dracut.conf.d/ae9-omit.conf
install -D -m 644 config/ae9-modules-load.conf %{buildroot}%{_prefix}/lib/modules-load.d/creative-ae9.conf
install -D -m 644 config/creative-ae9.conf %{buildroot}%{_datadir}/alsa-card-profile/mixer/profile-sets/creative-ae9.conf
install -D -m 644 config/50-ae9-safe-volume.conf %{buildroot}%{_datadir}/wireplumber/wireplumber.conf.d/50-ae9-safe-volume.conf
install -D -m 755 scripts/ae9-get-firmware.sh %{buildroot}%{_bindir}/ae9-get-firmware
install -D -m 755 scripts/ae9-full-dump.sh %{buildroot}%{_bindir}/ae9-full-dump
install -D -m 755 scripts/ae9-dsp-snapshot.sh %{buildroot}%{_bindir}/ae9-dsp-snapshot
install -D -m 755 scripts/ae9-read-stress.sh %{buildroot}%{_bindir}/ae9-read-stress
install -D -m 755 scripts/ae9-optical-passthrough %{buildroot}%{_bindir}/ae9-optical-passthrough
install -D -m 644 extract_firmware.py %{buildroot}%{_datadir}/%{name}/extract_firmware.py
install -D -m 644 extract_overlay.py %{buildroot}%{_datadir}/%{name}/extract_overlay.py
install -D -m 644 extract_hpeq.py %{buildroot}%{_datadir}/%{name}/extract_hpeq.py
# optional extras, copied by hand if wanted (see README)
for f in ae9-options.conf 60-ae9-headroom.conf 61-ae9-names.conf ae9-optical-in.conf ae9-what-u-hear.conf ae9-rear-rca-in.conf; do
	install -D -m 644 config/$f %{buildroot}%{_datadir}/%{name}/examples/$f
done

%post
udevadm control --reload >/dev/null 2>&1 || :
if [ "$1" -eq 1 ]; then
	echo "creative-ae9: the DSP firmware isn't included. Install it (and the headphone"
	echo "profiles) from Creative's AE-9 installer with"
	echo "  sudo ae9-get-firmware AECMDMasterInstaller_3.4.92.00.exe"
	echo "(needs innoextract; or point it at CtxHda.sys), then power the PC fully off and on."
fi

%postun
udevadm control --reload >/dev/null 2>&1 || :

%files
%license COPYING
%doc README.md CHANGELOG.md
%{_udevrulesdir}/00-ae9-override.rules
%{_udevrulesdir}/89-ae9-pipewire.rules
%{_modprobedir}/ae9-override.conf
%{_prefix}/lib/dracut/dracut.conf.d/ae9-omit.conf
%{_prefix}/lib/modules-load.d/creative-ae9.conf
%{_datadir}/alsa-card-profile/mixer/profile-sets/creative-ae9.conf
%dir %{_datadir}/wireplumber
%dir %{_datadir}/wireplumber/wireplumber.conf.d
%{_datadir}/wireplumber/wireplumber.conf.d/50-ae9-safe-volume.conf
%{_bindir}/ae9-get-firmware
%{_bindir}/ae9-full-dump
%{_bindir}/ae9-dsp-snapshot
%{_bindir}/ae9-read-stress
%{_bindir}/ae9-optical-passthrough
%{_datadir}/%{name}/

%changelog
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
- ae9-get-firmware reads Creative's installer (firmware + headphone profiles);
  extract_hpeq.py; Recommends: innoextract

* Fri Nov 06 2026 3xHelix <86426476+3xHelix@users.noreply.github.com> - 0.9.9-1
- First package: boot setup, PipeWire profiles, firmware helper, dump script
