#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
# Installs the AE-9's DSP firmware and headphone profiles from Creative's
# Windows software. They're Creative's and can't be shipped with this driver,
# so they come from your own download of Creative's installer.
#
#   sudo ae9-get-firmware AECMDMasterInstaller_3.4.92.00.exe   everything
#   sudo ae9-get-firmware /path/to/unpacked/folder              same, unpacked
#   sudo ae9-get-firmware /path/to/CtxHda.sys                   firmware only
#   ae9-get-firmware --check                    show what's installed (no root)
#
# Installers need innoextract (dnf/pacman/apt install innoextract); it only
# reads them, nothing is run. Every image is checked against the ones this
# driver is tested with and refused if it differs. Regular systems get them
# in /lib/firmware; read-only ones (Fedora Atomic: Silverblue, Kinoite,
# Bazzite, ...) in /etc/firmware, plus the kernel option
# firmware_class.path=/etc/firmware (takes effect after the next reboot).
set -u
FILES="ctefx-desktop.bin ctefx-ae9-overlay.bin"
MD5_DESKTOP=85c8f2b47180cd6623a2b0e8b8911fe0
MD5_OVERLAY=0e222c941f708a2caf93d50d2aba5ef8

# The extractors: next to this script in a git checkout, or where the
# creative-ae9 package puts them.
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
for d in "$HERE/.." "$HERE" /usr/share/creative-ae9; do
	[ -f "$d/extract_firmware.py" ] && [ -f "$d/extract_overlay.py" ] && [ -f "$d/extract_hpeq.py" ] &&
		{ EX=$(cd "$d" && pwd); break; }
done

if [ -e /run/ostree-booted ]; then DEST=/etc/firmware; ATOMIC=1; else DEST=/lib/firmware; ATOMIC=0; fi
# AE9_FW_DEST=<dir>: install there instead, no root needed (tests, image builds)
[ -n "${AE9_FW_DEST:-}" ] && { DEST=$AE9_FW_DEST; ATOMIC=0; }

want_md5() { case $1 in ctefx-desktop.bin) echo $MD5_DESKTOP ;; *) echo $MD5_OVERLAY ;; esac; }

check() {
	local f p m ok=0
	for f in $FILES; do
		p=
		for d in ${AE9_FW_DEST:-} /etc/firmware /lib/firmware /usr/lib/firmware; do [ -f "$d/$f" ] && { p=$d/$f; break; }; done
		if [ -z "$p" ]; then echo "  $f: missing"; ok=1; continue; fi
		m=$(md5sum "$p" | cut -d' ' -f1)
		if [ "$m" = "$(want_md5 $f)" ]; then echo "  $f: ok ($p)"
		else echo "  $f: $p is not the tested image (md5 $m)"; ok=1; fi
	done
	n=0
	for d in ${AE9_FW_DEST:-} /etc/firmware /lib/firmware /usr/lib/firmware; do
		[ -d "$d/creative-ae9/hp" ] && { n=$(ls "$d/creative-ae9/hp"/*.bin 2>/dev/null | wc -l); hp=$d/creative-ae9/hp; break; }
	done
	if [ "$n" -gt 0 ]; then echo "  headphone profiles: $n ($hp)"
	else echo "  headphone profiles: none (optional; from Creative's AECMDMasterInstaller)"; fi
	if [ $ATOMIC = 1 ]; then
		if grep -qw 'firmware_class.path=/etc/firmware' /proc/cmdline; then echo "  kernel option firmware_class.path=/etc/firmware: active"
		else echo "  kernel option firmware_class.path=/etc/firmware: not active yet (reboot after installing)"; ok=1; fi
	fi
	return $ok
}

if [ "${1:-}" = --check ]; then
	echo "AE-9 firmware:"; check; exit $?
fi
if [ $# -ne 1 ] || [ ! -f "$1" ]; then
	sed -n '4,18p' "$0" | sed 's/^# \{0,1\}//'; exit 2
fi
[ "$(id -u)" = 0 ] || [ -n "${AE9_FW_DEST:-}" ] || exec sudo "$0" "$@"
[ -n "${EX:-}" ] || { echo "can't find extract_firmware.py / extract_overlay.py / extract_hpeq.py" >&2; exit 1; }

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
IN=$(readlink -f "$1")

# Creative's installers are Inno Setup packages: unpack them (read only).
unpack() {
	command -v innoextract >/dev/null || {
		echo "unpacking Creative's installer needs innoextract: sudo dnf install innoextract (Fedora),"
		echo "pacman -S innoextract (Arch), apt install innoextract (Debian/Ubuntu); or rpm-ostree install innoextract"
		exit 1; } >&2
	innoextract -s -d "$2" "$1" >/dev/null 2>"$T/inno.err" ||
		{ echo "innoextract couldn't unpack $1:" >&2; tail -3 "$T/inno.err" >&2; exit 1; }
}
case $IN in
*.[Ss][Yy][Ss]) SRC= SYS=$IN ;;
*.[Ee][Xx][Ee])
	echo "unpacking $(basename "$IN") ..."
	unpack "$IN" "$T/inst"
	# the master installer carries the driver installer inside
	for e in "$T"/inst/tmp/AE9Driver/*.exe "$T"/inst/tmp/*/AESeriesDriverInstaller.exe; do
		[ -f "$e" ] && { unpack "$e" "$T/drv"; break; }
	done
	SRC=$T ;;
*) [ -d "$IN" ] || { echo "$1: not a CtxHda.sys, installer or folder" >&2; exit 2; }; SRC=$IN ;;
esac
if [ -n "$SRC" ]; then
	SYS=$(find "$SRC" -ipath '*amd64/ctxhda.sys' -type f 2>/dev/null | head -1)
	HPEQ=$(find "$SRC" -type d -iname AE9HPEQ 2>/dev/null | head -1)
	[ -n "$SYS" ] || { echo "no AMD64/CtxHda.sys in there (is it Creative's AE-Series driver or AECMDMasterInstaller?)" >&2; exit 1; }
fi

python3 -I "$EX/extract_firmware.py" "$SYS" "$T/ctefx-desktop.bin" || { echo "main DSP image: extraction refused" >&2; exit 1; }
python3 -I "$EX/extract_overlay.py" "$SYS" "$T/ctefx-ae9-overlay.bin" || { echo "overlay: extraction refused" >&2; exit 1; }
mkdir -p "$DEST"
for f in $FILES; do
	[ "$(md5sum "$T/$f" | cut -d' ' -f1)" = "$(want_md5 $f)" ] || { echo "$f: unexpected image, not installed" >&2; exit 1; }
	install -m 644 "$T/$f" "$DEST/$f"
done
command -v restorecon >/dev/null && restorecon -F "$DEST"/ctefx-desktop.bin "$DEST"/ctefx-ae9-overlay.bin 2>/dev/null
echo "installed to $DEST: $FILES"

# Headphone profiles (optional): only Creative's AECMDMasterInstaller has them.
if [ -n "${HPEQ:-}" ]; then
	if python3 -I "$EX/extract_hpeq.py" "$HPEQ" "$T/hp"; then
		mkdir -p "$DEST/creative-ae9/hp"
		install -m 644 "$T"/hp/*.bin "$DEST/creative-ae9/hp/"
		command -v restorecon >/dev/null && restorecon -RF "$DEST/creative-ae9" 2>/dev/null
		echo "installed the headphone profiles to $DEST/creative-ae9/hp"
	else
		echo "headphone profiles: not installed (see above)" >&2
	fi
else
	echo "headphone profiles: not in this source; they come with Creative's AECMDMasterInstaller"
fi
if [ $ATOMIC = 1 ] && ! rpm-ostree kargs 2>/dev/null | grep -qw 'firmware_class.path=/etc/firmware'; then
	rpm-ostree kargs --append-if-missing=firmware_class.path=/etc/firmware &&
		echo "added the kernel option firmware_class.path=/etc/firmware: reboot to use it"
fi
echo; echo "AE-9 firmware:"; check
