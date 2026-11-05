#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# 5.1 jack mapping test: plays a 6-channel stream in DSP mode with
# speaker-test speaking each channel's name ("Front Left", "Center", ...),
# so you can hear which jack each one comes out of. Run it once per speaker
# position (front RCA, rear jack, center/sub jack) and note what you hear.
#
#   scripts/ae9-surround-test.sh [--direct] [loops]   (default 4 loops)
#
# --direct tests 5.1 in Direct Mode (no DSP; Speaker Config doesn't apply).
#
# Pauses PipeWire for the test (desktop audio stops) and starts it again
# at the end. Sets surround=1, Direct Mode off, SBX off, Speaker Config 5.1.
# The first ~3 s play direct (front only) before the switch to the DSP,
# so listen from the second loop on. Saves a router dump to
# ~/ae9-surround-router.txt while the stream plays.
set -u
DIRECT=off
if [ "${1:-}" = "--direct" ]; then DIRECT=on; shift; fi
LOOPS=${1:-4}
P=/sys/module/snd_hda_codec_creative_ae9/parameters
CMD=/sys/kernel/debug/creative_ae9/cmd

restart_pw() { systemctl --user start pipewire.socket pipewire-pulse.socket wireplumber 2>/dev/null; }
trap restart_pw EXIT

echo 1 | sudo tee $P/surround >/dev/null || exit 1
amixer -q -c AE9 cset name='Direct Mode Switch' $DIRECT
amixer -q -c AE9 cset name='SBX Switch' off
amixer -q -c AE9 cset name='Speaker Config' '5.1' || echo "couldn't set Speaker Config 5.1" >&2

systemctl --user stop pipewire.socket pipewire-pulse.socket pipewire pipewire-pulse wireplumber
sleep 1

# plughw: speaker-test's voice WAVs are 16-bit, the card takes 32-bit only
speaker-test -D plughw:AE9 -c 6 -r 48000 -t wav -l "$LOOPS" &
ST=$!
sleep 12	# past the direct-to-DSP switch, mid-stream
{
	echo "# router while playing 6 ch, $(date -Is)"
	for i in $(seq 0 127); do
		a=$(printf '0x%x' $((0x190000 + i * 4)))
		echo "chipior $a" | sudo tee $CMD >/dev/null
		sudo cat $CMD
	done | grep -v '=0x0 '
} > ~/ae9-surround-router.txt
wait $ST
echo
echo "router dump saved to ~/ae9-surround-router.txt"
journalctl -k -b --since '-2min' | grep creative-ae9 | grep -E 'stream 0x05|6 ch|FAILED|error' | tail -5
