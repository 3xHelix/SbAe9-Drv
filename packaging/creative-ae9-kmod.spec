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
Version:        0.11.3
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
