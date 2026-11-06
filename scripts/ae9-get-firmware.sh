#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
# Installs the AE-9's DSP firmware from Creative's Windows driver. The firmware
# is Creative's and can't be shipped with this driver, so it comes from your
# own copy of their driver package ("Sound Blaster AE-Series Driver"): unpack
# it and point this at AESeriesDriver/Driver/AMD64/CtxHda.sys.
#
#   sudo ae9-get-firmware /path/to/CtxHda.sys    extract, check and install
#   ae9-get-firmware --check                     show what's installed (no root)
#
# Both images are checked against the ones this driver is tested with and
# refused if they differ. Regular systems get them in /lib/firmware.
# Read-only systems (Fedora Atomic: Silverblue, Kinoite, Bazzite, ...) get
# them in /etc/firmware, plus the kernel option firmware_class.path=/etc/firmware
# so the kernel looks there (takes effect after the next reboot).
set -u
FILES="ctefx-desktop.bin ctefx-ae9-overlay.bin"
MD5_DESKTOP=85c8f2b47180cd6623a2b0e8b8911fe0
MD5_OVERLAY=0e222c941f708a2caf93d50d2aba5ef8

# The extractors: next to this script in a git checkout, or where the
# creative-ae9 package puts them.
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
for d in "$HERE/.." "$HERE" /usr/share/creative-ae9; do
	[ -f "$d/extract_firmware.py" ] && [ -f "$d/extract_overlay.py" ] && { EX=$(cd "$d" && pwd); break; }
done

if [ -e /run/ostree-booted ]; then DEST=/etc/firmware; ATOMIC=1; else DEST=/lib/firmware; ATOMIC=0; fi

want_md5() { case $1 in ctefx-desktop.bin) echo $MD5_DESKTOP ;; *) echo $MD5_OVERLAY ;; esac; }

check() {
	local f p m ok=0
	for f in $FILES; do
		p=
		for d in /etc/firmware /lib/firmware /usr/lib/firmware; do [ -f "$d/$f" ] && { p=$d/$f; break; }; done
		if [ -z "$p" ]; then echo "  $f: missing"; ok=1; continue; fi
		m=$(md5sum "$p" | cut -d' ' -f1)
		if [ "$m" = "$(want_md5 $f)" ]; then echo "  $f: ok ($p)"
		else echo "  $f: $p is not the tested image (md5 $m)"; ok=1; fi
	done
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
	sed -n '4,16p' "$0" | sed 's/^# \{0,1\}//'; exit 2
fi
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"
[ -n "${EX:-}" ] || { echo "can't find extract_firmware.py / extract_overlay.py" >&2; exit 1; }

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
python3 -I "$EX/extract_firmware.py" "$1" "$T/ctefx-desktop.bin" || { echo "main DSP image: extraction refused" >&2; exit 1; }
python3 -I "$EX/extract_overlay.py" "$1" "$T/ctefx-ae9-overlay.bin" || { echo "overlay: extraction refused" >&2; exit 1; }
mkdir -p "$DEST"
for f in $FILES; do
	[ "$(md5sum "$T/$f" | cut -d' ' -f1)" = "$(want_md5 $f)" ] || { echo "$f: unexpected image, not installed" >&2; exit 1; }
	install -m 644 "$T/$f" "$DEST/$f"
done
command -v restorecon >/dev/null && restorecon -F "$DEST"/ctefx-desktop.bin "$DEST"/ctefx-ae9-overlay.bin 2>/dev/null
echo "installed to $DEST: $FILES"
if [ $ATOMIC = 1 ] && ! rpm-ostree kargs 2>/dev/null | grep -qw 'firmware_class.path=/etc/firmware'; then
	rpm-ostree kargs --append-if-missing=firmware_class.path=/etc/firmware &&
		echo "added the kernel option firmware_class.path=/etc/firmware: reboot to use it"
fi
echo; echo "AE-9 firmware:"; check
