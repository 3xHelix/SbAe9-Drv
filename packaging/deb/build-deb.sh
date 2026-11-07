#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
# Build creative-ae9-dkms_<version>_all.deb (Debian, Ubuntu, Mint, Pop!_OS ...):
# the driver source for DKMS plus the boot setup, PipeWire profiles and tools,
# laid out like the Fedora and Arch packages. No firmware: ae9-get-firmware
# installs it from Creative's installer after the package.
#
#   packaging/deb/build-deb.sh [source dir or SbAe9-Drv-<ver>.tar.gz] [out dir]
#
# Needs dpkg-deb (Debian/Ubuntu, or a debian container). Default source: this
# repo; default out dir: the current directory.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${1:-$HERE/../..}
OUT=$(mkdir -p "${2:-.}" && cd "${2:-.}" && pwd)
command -v dpkg-deb >/dev/null || { echo "needs dpkg-deb" >&2; exit 1; }

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
if [ -f "$SRC" ]; then
	tar xzf "$SRC" -C "$T"
	SRC=$(echo "$T"/SbAe9-Drv-*)
fi
SRC=$(cd "$SRC" && pwd)
VER=$(sed -n 's/^#define CREATIVE_AE9_DRV_VERSION[[:space:]]*"\(.*\)"/\1/p' "$SRC/creative_ae9.h")
[ -n "$VER" ] || { echo "no version in $SRC/creative_ae9.h" >&2; exit 1; }
MOD=snd-hda-codec-creative-ae9
PKG=creative-ae9-dkms
R=$T/root

# module source for DKMS, its version filled in
D=$R/usr/src/$MOD-$VER
install -d "$D"
install -m 644 "$SRC"/Makefile "$SRC"/*.c "$SRC"/*.h "$D/"
sed "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"$VER\"/" "$SRC/dkms.conf" > "$D/dkms.conf"

# boot setup: hand the card to this driver instead of snd_hda_intel; keep it
# out of a dracut initramfs (initramfs-tools adds no sound modules anyway)
install -Dm644 "$SRC/config/00-ae9-override.rules" "$R/usr/lib/udev/rules.d/00-ae9-override.rules"
install -Dm644 "$SRC/config/ae9-override.conf" "$R/usr/lib/modprobe.d/ae9-override.conf"
install -Dm644 "$SRC/config/ae9-omit.conf" "$R/usr/lib/dracut/dracut.conf.d/ae9-omit.conf"
install -Dm644 "$SRC/config/ae9-modules-load.conf" "$R/usr/lib/modules-load.d/creative-ae9.conf"
# PipeWire profile set (stereo duplex + 5.1)
install -Dm644 "$SRC/config/89-ae9-pipewire.rules" "$R/usr/lib/udev/rules.d/89-ae9-pipewire.rules"
install -Dm644 "$SRC/config/creative-ae9.conf" "$R/usr/share/alsa-card-profile/mixer/profile-sets/creative-ae9.conf"
# tools
install -Dm755 "$SRC/scripts/ae9-get-firmware.sh" "$R/usr/bin/ae9-get-firmware"
install -Dm755 "$SRC/scripts/ae9-full-dump.sh" "$R/usr/bin/ae9-full-dump"
install -d "$R/usr/share/creative-ae9/examples"
install -m 644 "$SRC"/extract_firmware.py "$SRC"/extract_overlay.py "$SRC"/extract_hpeq.py "$R/usr/share/creative-ae9/"
for f in ae9-options.conf 60-ae9-headroom.conf 61-ae9-names.conf ae9-optical-in.conf ae9-what-u-hear.conf ae9-rear-rca-in.conf; do
	install -m 644 "$SRC/config/$f" "$R/usr/share/creative-ae9/examples/"
done
install -d "$R/usr/share/doc/$PKG"
install -m 644 "$SRC/README.md" "$R/usr/share/doc/$PKG/"
install -m 644 "$SRC/CHANGELOG.md" "$R/usr/share/doc/$PKG/changelog"
gzip -9n "$R/usr/share/doc/$PKG/changelog"
# Debian's own changelog for the package (one entry per build)
{
	echo "$PKG ($VER-1) unstable; urgency=medium"
	echo
	echo "  * Driver $VER; upstream changes in changelog.gz."
	echo
	echo " -- 3xHelix <86426476+3xHelix@users.noreply.github.com>  $(LC_ALL=C date -R)"
} | gzip -9n > "$R/usr/share/doc/$PKG/changelog.Debian.gz"
cat > "$R/usr/share/doc/$PKG/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: SbAe9-Drv
Source: https://github.com/3xHelix/SbAe9-Drv

Files: *
Copyright: 2026 3xHelix
 Parts adapted from the Linux kernel's sound/pci/hda/patch_ca0132.c,
 Copyright (c) 2011 Creative Technology Ltd.
License: GPL-2.0-or-later
 On Debian systems the full text is in /usr/share/common-licenses/GPL-2.

Comment: The DSP firmware and the headphone profiles are Creative's and are
 not part of this package; ae9-get-firmware extracts them from Creative's
 installer on the user's machine.
EOF

# control and maintainer scripts
install -d "$R/DEBIAN"
SIZE=$(du -sk --exclude=DEBIAN "$R" | cut -f1)
cat > "$R/DEBIAN/control" <<EOF
Package: $PKG
Version: $VER-1
Architecture: all
Maintainer: 3xHelix <86426476+3xHelix@users.noreply.github.com>
Installed-Size: $SIZE
Depends: dkms, python3, udev
Recommends: innoextract, linux-headers-amd64 | linux-headers-generic | linux-headers
Section: kernel
Priority: optional
Homepage: https://github.com/3xHelix/SbAe9-Drv
Description: Creative Sound Blaster AE-9 driver (DKMS)
 Out-of-tree Linux driver for the Creative Sound Blaster AE-9: playback up
 to 384 kHz, 5.1, SBX/DSP effects and EQ, speaker setup, headphone profiles,
 recording, optical in and out, What U Hear and the ACM head unit. Both card
 versions (IDT and ASMedia bridge).
 .
 DKMS builds the module for every installed kernel with headers. The DSP
 firmware is Creative's and not included: run ae9-get-firmware with
 Creative's AE-9 installer after installing, then power the PC fully off.
EOF
cat > "$R/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e
if [ "\$1" = configure ]; then
	dkms add -m $MOD -v $VER >/dev/null 2>&1 || true
	# build for every installed kernel that has headers
	for k in /lib/modules/*; do
		k=\${k##*/}
		[ -e /lib/modules/\$k/build ] || continue
		dkms install -m $MOD -v $VER -k "\$k" || echo "$PKG: DKMS build for \$k failed (see above)" >&2
	done
	udevadm control --reload >/dev/null 2>&1 || true
	if ! ae9-get-firmware --check >/dev/null 2>&1; then
		cat <<'MSG'
creative-ae9: the DSP firmware isn't included (it's Creative's). Install it, and
the headphone profiles, from Creative's AE-9 installer (needs innoextract):
  sudo ae9-get-firmware AECMDMasterInstaller_3.4.92.00.exe
(or point it at CtxHda.sys), then power the PC fully off (PSU off) and on.
MSG
	fi
fi
exit 0
EOF
cat > "$R/DEBIAN/prerm" <<EOF
#!/bin/sh
set -e
case "\$1" in
remove|upgrade|deconfigure)
	dkms remove -m $MOD -v $VER --all >/dev/null 2>&1 || true ;;
esac
exit 0
EOF
cat > "$R/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
udevadm control --reload >/dev/null 2>&1 || true
exit 0
EOF
chmod 755 "$R/DEBIAN/postinst" "$R/DEBIAN/prerm" "$R/DEBIAN/postrm"

dpkg-deb --root-owner-group -Zxz --build "$R" "$OUT/${PKG}_${VER}-1_all.deb" >/dev/null
echo "$OUT/${PKG}_${VER}-1_all.deb"
