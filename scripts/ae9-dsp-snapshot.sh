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
# A register where one sample in the middle differs and all the others
# agree (A A B A), or whose only other values are 0x0/0x1, is most likely a
# bad read (chip reads glitch while audio plays, 0.3-2.7% on two cards), not
# movement: marked "one-off". A change at the first or last sample (A A A B)
# can be a slow counter stepping, so that counts as moving.
moving=0; oneoff=0
for a in "${addrs[@]}"; do
	line="$a"
	declare -A cnt=()
	for s in $(seq 1 $SAMPLES); do
		line="$line ${v[$a,$s]}"
		cnt[${v[$a,$s]}]=$(( ${cnt[${v[$a,$s]}]:-0} + 1 ))
	done
	top=0; mode=
	for k in "${!cnt[@]}"; do
		# on a tie, the steady value is the one that isn't 0x0/0x1
		if [ ${cnt[$k]} -gt $top ] || { [ ${cnt[$k]} -eq $top ] && { [ "$mode" = 0x0 ] || [ "$mode" = 0x1 ]; }; }; then
			top=${cnt[$k]}; mode=$k
		fi
	done
	# the bad reads are mostly 0 or 1: deviations that are only 0x0/0x1
	# from a steady value count as bad reads too (they come in bursts)
	only01=1; for k in "${!cnt[@]}"; do [ "$k" = "$mode" ] || [ "$k" = 0x0 ] || [ "$k" = 0x1 ] || only01=0; done
	if [ ${#cnt[@]} -eq 1 ]; then
		echo "$line"
	elif [ $SAMPLES -ge 3 ] && [ $top -ge 2 ] && { { [ ${#cnt[@]} -eq 2 ] && [ $top -eq $((SAMPLES - 1)) ] && \
	     [ "${v[$a,1]}" = "$mode" ] && [ "${v[$a,$SAMPLES]}" = "$mode" ]; } || [ $only01 = 1 ]; }; then
		echo "$line  <- one-off (likely a bad read)"; oneoff=$((oneoff + 1))
	else
		echo "$line  <- moving"; moving=$((moving + 1))
	fi
	unset cnt
done
echo "# $moving of ${#addrs[@]} registers moved over $SAMPLES samples; $oneoff one-off (likely bad reads)"
