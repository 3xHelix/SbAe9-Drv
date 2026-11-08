#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
# DSP-side snapshot for telling a stuck AE-9 from a healthy one: samples DSP
# control and DMA registers a few times and shows which ones move (a DSP
# that's processing audio has moving DMA pointers/counters, a stuck one may
# not). Packaged as ae9-dsp-snapshot.
#
#   sudo ae9-dsp-snapshot > snap.txt              nothing playing
#   sudo ae9-dsp-snapshot --playing > snap.txt    while audio plays (the
#                                                 useful case, see below)
#
# For a stuck-DSP report: music playing in DSP mode (Direct Mode off), one
# snapshot on a good boot as the baseline and one while it's stuck, BEFORE
# the PSU power-off.
#
# Reading the card's chip while audio plays can freeze the audio until a
# full power-off (2026-11-07: a router scan during playback killed codec 1).
# So with a stream running it needs --playing, and even then it skips the
# router table and reads only the DSP registers the driver itself reads.
# ROUTER=1 adds the router table anyway; EXTRA="0x... 0x..." adds addresses.
CMD=/sys/kernel/debug/creative_ae9/cmd
SAMPLES=${SAMPLES:-4}
GAP=${GAP:-0.25}

[ -w "$CMD" ] || { echo "need root and the driver loaded ($CMD): sudo ae9-dsp-snapshot" >&2; exit 1; }

playing=$(grep -ls "^state: RUNNING" /proc/asound/AE9/pcm*/sub*/status 2>/dev/null | tr '\n' ' ')
router=${ROUTER:-1}
if [ -n "$playing" ]; then
	if [ "${1:-}" != --playing ]; then
		cat >&2 <<'MSG'
Audio is playing on the AE-9. Reading the card's chip during playback can
freeze the audio until you fully power off the PC (PSU switch; a reboot
isn't enough). If that's OK, run it again with --playing:
  sudo ae9-dsp-snapshot --playing > snap.txt
or stop all audio first.
MSG
		exit 2
	fi
	router=${ROUTER:-0}
fi

rd() { echo "chipior $1" > $CMD; sed -n "s/.*=\(0x[0-9a-f]*\) ret.*/\1/p" $CMD; }

# Only addresses the driver already reads (some hardware registers clear
# when read, so no blind ranges)
addrs=()
add_range() { local a=$(($1)); while [ $a -le $(($2)) ]; do addrs+=($(printf '0x%x' $a)); a=$((a + 4)); done; }
addrs+=(0x100e30 0x801c 0x40004 0x3f3b4)	# DBGCNTL, fw flags, overlay check
addrs+=(0x110f00 0x110f04 0x110f08)		# DSP DMA registers (overlay log)
addrs+=(0x110ff0 0x110ff8)			# DMA status (channels running)
[ "$router" = 1 ] && add_range 0x190030 0x1900fc	# router table
addrs+=($EXTRA)

declare -A v
for s in $(seq 1 $SAMPLES); do
	for a in "${addrs[@]}"; do v[$a,$s]=$(rd $a); done
	[ $s -lt $SAMPLES ] && sleep $GAP
done

echo "# AE-9 DSP snapshot $(date -Is), kernel $(uname -r), driver $(cat /sys/module/snd_hda_codec_creative_ae9/version 2>/dev/null)"
echo "# $(dmesg | grep -oE 'Direct Mode (on|off), SBX (on|off)' | tail -1)"
echo "# streams running: ${playing:-none}; router table: $([ "$router" = 1 ] && echo included || echo skipped)"
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
