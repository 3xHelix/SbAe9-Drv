#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
# How often does a ChipIO read come back wrong? Reads one address N times
# (default: router entry 0x190030, constant within a boot) and counts the
# values. 2026-11-04 snapshot: ~4% of router reads were 0, 0x1, or the
# value 0x38 bytes further on. Run once with audio stopped and once with
# it playing to see whether playback disturbs the reads.
#   sudo scripts/ae9-read-stress.sh [addr] [count]
CMD=/sys/kernel/debug/creative_ae9/cmd
ADDR=${1:-0x190030}
N=${2:-300}

[ -w "$CMD" ] || { echo "need root and the driver loaded ($CMD)" >&2; exit 1; }

for i in $(seq 1 $N); do
	echo "chipior $ADDR" > $CMD
	sed -n "s/.*=\(0x[0-9a-f]*\) ret=\(.*\)/\1 ret=\2/p" $CMD
done | sort | uniq -c | sort -rn | {
	echo "# $N reads of $ADDR, $(date -Is)"
	cat
}
