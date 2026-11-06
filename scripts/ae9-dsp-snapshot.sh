#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
# DSP-side snapshot for telling a stuck AE-9 from a healthy one.
# The router layout can't (2026-11-04), so this samples DSP control and DMA
# registers a few times and shows which ones move: a DSP that's processing
# audio should have moving DMA pointers/counters, a stuck one maybe not.
#
# Run as root with the driver loaded and audio PLAYING in DSP mode
# (Direct Mode off). Take one on a good boot as the baseline, and one on a
# stuck boot BEFORE the PSU power-off.
#   sudo scripts/ae9-dsp-snapshot.sh > snap-good.txt
CMD=/sys/kernel/debug/creative_ae9/cmd
SAMPLES=${SAMPLES:-4}
GAP=${GAP:-0.25}

[ -w "$CMD" ] || { echo "need root and the driver loaded ($CMD)" >&2; exit 1; }

rd() { echo "chipior $1" > $CMD; cat $CMD | sed -n "s/.*=\(0x[0-9a-f]*\) ret.*/\1/p"; }

# address list: single words plus small ranges
addrs=()
add_range() { local a=$(($1)); while [ $a -le $(($2)) ]; do addrs+=($(printf '0x%x' $a)); a=$((a + 4)); done; }
# Only addresses the driver already reads (some hardware registers clear
# when read, so no blind ranges while audio plays). EXTRA="0x... 0x..." adds more.
addrs+=(0x100e30 0x801c 0x40004 0x3f3b4)	# DBGCNTL, fw flags, overlay check
addrs+=(0x110f00 0x110f04 0x110f08)		# DSP DMA registers (overlay log)
addrs+=(0x110ff0 0x110ff8)			# DMA status (overlay log)
add_range 0x190030 0x1900fc			# router, for the record
addrs+=($EXTRA)

declare -A v
for s in $(seq 1 $SAMPLES); do
	for a in "${addrs[@]}"; do v[$a,$s]=$(rd $a); done
	[ $s -lt $SAMPLES ] && sleep $GAP
done

echo "# AE-9 DSP snapshot $(date -Is), kernel $(uname -r)"
echo "# $(dmesg | grep -oE 'Direct Mode (on|off), SBX (on|off)' | tail -1)"
moving=0
for a in "${addrs[@]}"; do
	line="$a"; first=${v[$a,1]}; changed=0
	for s in $(seq 1 $SAMPLES); do
		line="$line ${v[$a,$s]}"
		[ "${v[$a,$s]}" != "$first" ] && changed=1
	done
	if [ $changed = 1 ]; then echo "$line  <- moving"; moving=$((moving + 1)); else echo "$line"; fi
done
echo "# $moving of ${#addrs[@]} registers moved over $SAMPLES samples"
