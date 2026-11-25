#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
# restore-stock-audio.sh — remove the out-of-tree AE-9 driver and its boot
# setup, so the stock kernel drivers own the card again (an AE-9, or a
# Sound Blaster AE-7 handed to this driver, unless it is parked: below).
#
# Removes what docs/INSTALL-FROM-SOURCE.md installs:
#   /lib/modules/<kernel>/extra/snd-hda-codec-creative-ae9.ko (and updates/)
#   /etc/udev/rules.d/00-ae9-override.rules
#   /etc/modprobe.d/ae9-override.conf, /etc/modprobe.d/ae9-options.conf
#   /etc/dracut.conf.d/ae9-omit.conf
#   /etc/udev/rules.d/89-ae9-pipewire.rules and the PipeWire profile set
#   /usr/share/alsa-card-profile/mixer/profile-sets/creative-ae9.conf
#   the AE-7's opt-in (ae9-ae7-enable): /etc/udev/rules.d/00-ae7-override.rules,
#   /etc/modprobe.d/ae7-override.conf, its PipeWire rule and profile set,
#   and the AE-7 test kits' files
# plus older file names from earlier versions of this project, and the
# "*.off-vfio" copies scripts/toggle-ae9-mode.sh sets aside. Then clears an
# AE-7's driver_override (not a parked one's), runs depmod, rebuilds the
# initramfs and checks nothing of ours is left.
#
# Note: the stock snd_hda_intel + snd_hda_codec_ca0132 don't support the
# AE-9; after this the card is simply back to having no working driver.
# An AE-7 behind a Tsi381 bridge goes back to the stock drivers (their
# ca0132 AE-7 support). Any other AE-7 is parked instead: snd_hda_intel
# hangs the PC at boot on an AE-7 with an ASMedia bridge. That's
# "ae9-ae7-enable --disable", run from the copy next to this script, which
# writes /etc/udev/rules.d/00-ae7-park.rules and /etc/modprobe.d/ae7-park.conf
# (they stay: the card gets no driver). --stock hands it to the stock
# drivers anyway (and removes park files left from before).
# The firmware in /lib/firmware and this source tree are left alone.
#
# Usage: sudo ./restore-stock-audio.sh [--stock]

set -u

STOCK=0
for a in "$@"; do
    case $a in
    --stock) STOCK=1 ;;
    -h|--help) awk 'NR < 4 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"; exit 0 ;;
    *) echo "ERROR: unknown option: $a (only --stock)"; exit 2 ;;
    esac
done

if [ "$(id -u)" != "0" ]; then
    echo "ERROR: run as root (sudo $0)"
    exit 1
fi

# the AE-7's on/off/park logic lives in ae9-ae7-enable: this tree's copy
# (an installed one may be older, or gone with the driver)
AE7_ENABLE=$(cd "$(dirname "$0")" && pwd)/ae9-ae7-enable
if [ ! -f "$AE7_ENABLE" ] || ! grep -q 'ae7-park' "$AE7_ENABLE"; then
    echo "ERROR: $AE7_ENABLE is missing or too old to park an AE-7: run this from the driver's source tree"
    exit 1
fi
unset AE7_ENABLE_ROOT

KERNEL=$(uname -r)
MOD=snd-hda-codec-creative-ae9
echo "=== AE-9 driver rollback (kernel $KERNEL) ==="

FILES=(
    "/lib/modules/${KERNEL}/extra/${MOD}.ko"
    "/lib/modules/${KERNEL}/updates/${MOD}.ko"
    /etc/udev/rules.d/00-ae9-override.rules
    /etc/modprobe.d/ae9-override.conf
    /etc/modprobe.d/ae9-options.conf
    /etc/dracut.conf.d/ae9-omit.conf
    /etc/modules-load.d/creative-ae9.conf
    /etc/udev/rules.d/89-ae9-pipewire.rules
    /usr/share/alsa-card-profile/mixer/profile-sets/creative-ae9.conf
    /etc/modprobe.d/ae9-surround.conf
    # the Sound Blaster AE-7 (experimental): the opt-in ae9-ae7-enable makes
    # and the AE-7's PipeWire files (a source install's copies)
    /etc/udev/rules.d/00-ae7-override.rules
    /etc/modprobe.d/ae7-override.conf
    /etc/udev/rules.d/89-ae7-pipewire.rules
    /usr/share/alsa-card-profile/mixer/profile-sets/creative-ae7.conf
    /usr/local/share/creative-ae9/ae7/00-ae7-override.rules
    /usr/local/share/creative-ae9/ae7/ae7-override.conf
    # the AE-7 test kits' files
    /etc/modprobe.d/ae7-kit.conf
    /etc/modules-load.d/ae7-kit.conf
    /etc/modules-load.d/ae7-kit-modules-load.conf
    /etc/udev/rules.d/00-ae7-kit-override.rules
    /etc/udev/rules.d/89-ae7-kit-pipewire.rules
    /etc/alsa/conf.d/ae7-stereo.conf
    /etc/wireplumber/wireplumber.conf.d/50-ae7-kit-safe-volume.conf
    /etc/wireplumber/main.lua.d/51-ae7-kit-safe-volume.lua
    # older names from earlier versions of this project
    /etc/udev/rules.d/10-creative-ae9.rules
    /etc/modprobe.d/creative_ae9.conf
    /etc/modprobe.d/creative-ae9-opts.conf
    /etc/modprobe.d/snd-hda-intel-ae9-exclude.conf
    /etc/dracut.conf.d/creative-ae9.conf
)

echo "[1/4] Removing files..."
# the AE-7 first: its opt-in pair out, and an AE-7 the stock driver would
# hang parked. If that fails, stop before anything else is removed (the
# opt-in files then still keep the AE-7 from snd_hda_intel).
AE7_ARGS=(--disable)
[ "$STOCK" = 0 ] || AE7_ARGS+=(--stock)
AE7_OUT=$(bash "$AE7_ENABLE" "${AE7_ARGS[@]}" 2>&1); AE7_RC=$?
printf '%s\n' "$AE7_OUT" | sed 's/^/  /'
if [ "$AE7_RC" != 0 ]; then
    echo "ERROR: ae9-ae7-enable ${AE7_ARGS[*]} failed (exit $AE7_RC); nothing else removed"
    exit 1
fi
PARKED=0
if [ -e /etc/udev/rules.d/00-ae7-park.rules ] || [ -e /etc/modprobe.d/ae7-park.conf ]; then PARKED=1; fi
for f in "${FILES[@]}" ; do
    for g in "$f" "$f.off-vfio"; do
        [ -e "$g" ] && rm -v "$g"
    done
done

echo "[2/4] Unloading the module if loaded..."
if lsmod | grep -q "^snd_hda_codec_creative_ae9 "; then
    modprobe -r "$MOD" 2>/dev/null || echo "  could not unload (in use); it will be gone after reboot"
fi
# an AE-7 (PCI 1102:0010, subsystem 0x0081) marked for this driver: clear the
# mark, so the stock driver may take it (fully at the next boot); a parked
# AE-7 keeps it, so nothing takes it now either
for d in /sys/bus/pci/devices/*; do
    if [ "$(cat "$d/vendor" 2>/dev/null)" = 0x1102 ] && [ "$(cat "$d/device" 2>/dev/null)" = 0x0010 ] &&
       [ "$(cat "$d/subsystem_device" 2>/dev/null)" = 0x0081 ] &&
       [ "$(cat "$d/driver_override" 2>/dev/null)" = "$MOD" ]; then
        if [ "$PARKED" = 1 ]; then
            echo "  AE-7 at ${d##*/}: parked, driver_override left as it is"
        else
            echo > "$d/driver_override" && echo "  AE-7 at ${d##*/}: driver_override cleared"
        fi
    fi
done

echo "[3/4] depmod + initramfs..."
depmod -a "$KERNEL"
# the distribution's own tool (Debian, Ubuntu, Mint: update-initramfs; Arch:
# mkinitcpio; Fedora and others: dracut, at Fedora's path when that's there)
INITRD=ok
if command -v update-initramfs >/dev/null 2>&1; then
    update-initramfs -u -k "$KERNEL" || INITRD="update-initramfs -u -k $KERNEL failed"
elif command -v mkinitcpio >/dev/null 2>&1; then
    mkinitcpio -P || INITRD="mkinitcpio -P failed"
elif command -v dracut >/dev/null 2>&1; then
    if [ -e "/boot/initramfs-${KERNEL}.img" ]; then
        dracut --force "/boot/initramfs-${KERNEL}.img" "$KERNEL" || INITRD="dracut failed"
    else
        dracut --force --kver "$KERNEL" || INITRD="dracut failed"
    fi
else
    INITRD="no update-initramfs, mkinitcpio or dracut found"
fi

echo "[4/4] Checking..."
LEFT=0
if [ "$INITRD" != ok ]; then
    echo "  initramfs not rebuilt ($INITRD): rebuild it by hand before rebooting"
    LEFT=1
fi
for f in "${FILES[@]}" ; do
    for g in "$f" "$f.off-vfio"; do
        [ -e "$g" ] && { echo "  still present: $g"; LEFT=1; }
    done
done
if modinfo -n "$MOD" >/dev/null 2>&1; then
    echo "  module still found by modprobe: $(modinfo -n "$MOD")"
    LEFT=1
fi

echo ""
if [ "$LEFT" = 0 ] && [ "$PARKED" = 1 ]; then
    echo "=== Rollback complete. Reboot to finish (the stock drivers don't support the AE-9; an AE-7 stays parked, with no driver: docs/AE7.md, or run this again with --stock). ==="
elif [ "$LEFT" = 0 ]; then
    echo "=== Rollback complete. Reboot to finish (the stock drivers don't support the AE-9; an AE-7 is theirs again). ==="
else
    echo "=== Rollback INCOMPLETE: see the items above. ==="
    exit 1
fi
