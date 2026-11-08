#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
# How often does a read of the AE-9's chip come back wrong on this card?
# Reads one address N times (default: router entry 0x190030, constant
# within a boot) and counts the values: one value = clean; several = some
# reads came back wrong. Packaged as ae9-read-stress.
#
#   sudo ae9-read-stress > stress-idle.txt               nothing playing
#   sudo ae9-read-stress --playing > stress-playing.txt  while audio plays
#   sudo ae9-read-stress [--playing] [addr] [count]      (default 0x190030 300)
#
# Reading the chip while audio plays can freeze the audio until a full
# power-off (2026-11-07: router reads during playback killed codec 1), so
# with a stream running it needs --playing.
CMD=/sys/kernel/debug/creative_ae9/cmd
PLAYING_OK=0
[ "${1:-}" = --playing ] && { PLAYING_OK=1; shift; }
ADDR=${1:-0x190030}
N=${2:-300}

[ -w "$CMD" ] || { echo "need root and the driver loaded ($CMD): sudo ae9-read-stress" >&2; exit 1; }

playing=$(grep -ls "^state: RUNNING" /proc/asound/AE9/pcm*/sub*/status 2>/dev/null | tr '\n' ' ')
if [ -n "$playing" ] && [ $PLAYING_OK = 0 ]; then
	cat >&2 <<'MSG'
Audio is playing on the AE-9. Reading the card's chip during playback can
freeze the audio until you fully power off the PC (PSU switch; a reboot
isn't enough). If that's OK, run it again with --playing:
  sudo ae9-read-stress --playing > stress-playing.txt
or stop all audio first.
MSG
	exit 2
fi

for i in $(seq 1 $N); do
	echo "chipior $ADDR" > $CMD
	sed -n "s/.*=\(0x[0-9a-f]*\) ret=\(.*\)/\1 ret=\2/p" $CMD
done | sort | uniq -c | sort -rn | {
	echo "# $N reads of $ADDR, $(date -Is), kernel $(uname -r), driver $(cat /sys/module/snd_hda_codec_creative_ae9/version 2>/dev/null)"
	echo "# streams running: ${playing:-none}"
	cat
}
