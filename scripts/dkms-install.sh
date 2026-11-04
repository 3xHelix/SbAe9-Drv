#!/bin/bash
# Install the AE-9 driver as a DKMS module, so every new kernel gets it
# rebuilt automatically (Fedora's kernel-install hook runs dkms autoinstall;
# the matching kernel-devel package must be installed, as dnf does).
#
#   sudo scripts/dkms-install.sh            install this source tree's version
#                                            for the running kernel (and any
#                                            kernels given as arguments)
#   sudo scripts/dkms-install.sh --remove   take the driver out of DKMS
#
# Older registered versions are removed, and a hand-copied module in
# /lib/modules/<kernel>/extra/ is deleted first so only one copy exists.
set -euo pipefail

NAME=snd-hda-codec-creative-ae9
SRC=$(cd "$(dirname "$0")/.." && pwd)
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"
command -v dkms >/dev/null || { echo "dkms isn't installed (sudo dnf install dkms)" >&2; exit 1; }

registered() { dkms status -m "$NAME" 2>/dev/null | sed -n "s#^$NAME/\([^,: ]*\).*#\1#p" | sort -u; }

# Take a version out of DKMS and /usr/src. dkms remove can return non-zero
# even after removing it (2026-11-04: the module was gone but the script
# stopped there, leaving no driver), so don't let that stop the install.
drop() {
	dkms remove -m "$NAME" -v "$1" --all || echo "note: dkms remove $1 returned $?, carrying on" >&2
	rm -rf "/usr/src/$NAME-$1" "/var/lib/dkms/$NAME/$1"
}

if [ "${1:-}" = "--remove" ]; then
	for v in $(registered); do drop "$v"; done
	for d in /usr/src/$NAME-*; do if [ -d "$d" ]; then rm -rf "$d"; fi; done
	echo "removed from DKMS; build and copy the module by hand again if you still want it"
	exit 0
fi

VER=$(sed -n 's/^#define CREATIVE_AE9_DRV_VERSION[[:space:]]*"\(.*\)"/\1/p' "$SRC/creative_ae9.h")
[ -n "$VER" ] || { echo "no CREATIVE_AE9_DRV_VERSION in creative_ae9.h" >&2; exit 1; }

# Drop every registered version (including this one, so a re-run picks up
# changed source), then copy the source to /usr/src.
for v in $(registered); do drop "$v"; done
for d in /usr/src/$NAME-*; do if [ -d "$d" ]; then rm -rf "$d"; fi; done	# leftovers
DEST=/usr/src/$NAME-$VER
mkdir -p "$DEST"
cp "$SRC"/Makefile "$SRC"/creative_ae9*.c "$SRC"/creative_ae9*.h "$SRC"/ca0132_regs.h "$DEST"/
sed "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"$VER\"/" "$SRC/dkms.conf" > "$DEST/dkms.conf"
dkms add -m "$NAME" -v "$VER"

for k in "$(uname -r)" "$@"; do
	if [ ! -d "/lib/modules/$k/build" ]; then
		echo "skipping $k: no kernel headers (sudo dnf install kernel-devel-$k)" >&2
		continue
	fi
	rm -f "/lib/modules/$k/extra/$NAME.ko"	# the hand-copied one
	dkms build -m "$NAME" -v "$VER" -k "$k"
	dkms install -m "$NAME" -v "$VER" -k "$k" --force
done
dkms status -m "$NAME"
# make sure the running kernel really has it now
if ! modinfo -k "$(uname -r)" -n "$NAME" >/dev/null 2>&1; then
	echo "ERROR: $NAME isn't installed for $(uname -r); don't reboot until this is fixed" >&2
	exit 1
fi
echo "done: $NAME $VER is in DKMS ($(modinfo -k "$(uname -r)" -n "$NAME"));"
echo "new kernels get it built automatically. Reboot to load it."
