#!/usr/bin/env bash
# restore-stock-audio.sh — remove the out-of-tree AE-9 driver and its boot
# setup, so the stock kernel drivers own the card again.
#
# Removes what README.md -> Setup installs:
#   /lib/modules/<kernel>/extra/snd-hda-codec-creative-ae9.ko (and updates/)
#   /etc/udev/rules.d/00-ae9-override.rules
#   /etc/modprobe.d/ae9-override.conf, /etc/modprobe.d/ae9-options.conf
#   /etc/dracut.conf.d/ae9-omit.conf
# plus older file names from earlier versions of this project, and the
# "*.off-vfio" copies scripts/toggle-ae9-mode.sh parks. Then runs depmod,
# rebuilds the initramfs and checks nothing of ours is left.
#
# Note: the stock snd_hda_intel + snd_hda_codec_ca0132 don't support the
# AE-9; after this the card is simply back to having no working driver.
# The firmware in /lib/firmware and this source tree are left alone.
#
# Usage: sudo ./restore-stock-audio.sh

set -u

if [ "$(id -u)" != "0" ]; then
    echo "ERROR: run as root (sudo $0)"
    exit 1
fi

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
    # older names from earlier versions of this project
    /etc/udev/rules.d/10-creative-ae9.rules
    /etc/modprobe.d/creative_ae9.conf
    /etc/modprobe.d/creative-ae9-opts.conf
    /etc/modprobe.d/snd-hda-intel-ae9-exclude.conf
    /etc/dracut.conf.d/creative-ae9.conf
)

echo "[1/4] Removing files..."
for f in "${FILES[@]}" ; do
    for g in "$f" "$f.off-vfio"; do
        [ -e "$g" ] && rm -v "$g"
    done
done

echo "[2/4] Unloading the module if loaded..."
if lsmod | grep -q "^snd_hda_codec_creative_ae9 "; then
    modprobe -r "$MOD" 2>/dev/null || echo "  could not unload (in use); it will be gone after reboot"
fi

echo "[3/4] depmod + initramfs..."
depmod -a "$KERNEL"
dracut --force "/boot/initramfs-${KERNEL}.img" "$KERNEL"

echo "[4/4] Checking..."
LEFT=0
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
if [ "$LEFT" = 0 ]; then
    echo "=== Rollback complete. Reboot to finish (the stock drivers don't support the AE-9). ==="
else
    echo "=== Rollback INCOMPLETE: see the items above. ==="
    exit 1
fi
