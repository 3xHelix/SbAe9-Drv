#!/bin/bash
# Per-rate PipeWire headroom for the AE-9 (optional, runs as a user service).
#
# The card's DMA fetches audio up to ~0.5-0.7 ms ahead of the position the
# driver reports. At small PipeWire quantums PipeWire then writes into audio
# the card has already fetched: tinny, metallic static with no error shown.
# PipeWire's api.alsa.headroom keeps extra frames queued to cover it, but it
# is one fixed number of frames, and the read-ahead is a fixed time. This
# sets the headroom to suit the rate the card is running at:
#
#   up to 48 kHz: 64    up to 96 kHz: 96    up to 192 kHz: 144    above: 256
#
# (tested clean at quantum 32-64 on 2026-11-05). Override with
# AE9_HEADROOM="48000:64 96000:96 192000:144 384000:256".
#
# PipeWire only applies a new headroom when the output opens. So while the
# output is idle, this presets the value for the forced rate
# (clock.force-rate), and playback starts with it. When the card opens at a
# rate whose value isn't in place (an automatic rate switch), it waits 2 s,
# sets it, and reopens the output once: a short gap, like a pause.
#
#   scripts/ae9-headroom.sh          run in the foreground (Ctrl-C stops)
#   config/ae9-headroom.service      the user service; see the README
set -u
HW=/proc/asound/AE9/pcm0p/sub0/hw_params
TABLE=${AE9_HEADROOM:-48000:64 96000:96 192000:144 384000:256}
SETTLE_S=2

say() { echo "ae9-headroom: $*"; }

want_for() {	# headroom for a rate: the first tier at or above it
	local t last=""
	for t in $TABLE; do
		last=${t##*:}
		[ "$1" -le "${t%%:*}" ] && { echo "$last"; return; }
	done
	echo "$last"
}

# the AE-9 output node (stereo or 5.1 profile): "id name headroom", where
# headroom is what PipeWire uses now (while open)
node_info() {
	pw-dump 2>/dev/null | python3 -I -c '
import json, sys
try:
    for o in json.load(sys.stdin):
        p = (o.get("info") or {}).get("props") or {}
        if (p.get("alsa.driver_name") == "snd_hda_codec_creative_ae9"
                and p.get("media.class") == "Audio/Sink"):
            print(o["id"], p.get("node.name"), p.get("api.alsa.headroom", "-"))
            break
except Exception:
    pass'
}

set_headroom() {
	pw-cli set-param "$1" Props "{ params = [ \"api.alsa.headroom\" $2 ] }" >/dev/null 2>&1
}

card_rate() { awk '/^rate:/ {print $2}' $HW 2>/dev/null; }

force_rate() {
	pw-metadata -n settings 0 clock.force-rate 2>/dev/null |
		grep -o "value:'[0-9]*'" | tr -dc 0-9
}

say "table: $TABLE"
preset=""	# "node-id value" we preset for the forced rate while idle
seen=""		# rate of the open we last checked
tick=0
while :; do
	rate=$(card_rate)
	if [ -n "$rate" ]; then
		if [ "$rate" != "$seen" ]; then
			seen=$rate
			want=$(want_for "$rate")
			sleep 0.5	# let PipeWire publish the open's settings
			read -r id name have <<< "$(node_info)"
			if [ -n "${id:-}" ] && [ "$have" != "$want" ]; then
				sleep $SETTLE_S	# let the rate switch settle first
				if [ "$(card_rate)" = "$rate" ]; then
					set_headroom "$id" "$want"
					pactl suspend-sink "$name" 1 && pactl suspend-sink "$name" 0
					say "$rate Hz: headroom $have -> $want (output reopened)"
					preset="$id $want"
				else
					seen=""	# rate moved on; check again
				fi
			fi
		fi
	else
		seen=""
		fr=$(force_rate)
		if [ -n "$fr" ] && [ "$fr" -gt 0 ]; then
			want=$(want_for "$fr")
			# look up the node when the value changes, and every 15 s
			# in case WirePlumber restarted (new node id)
			if [ "$want" != "${preset#* }" ] || [ $((tick % 15)) -eq 0 ]; then
				read -r id name have <<< "$(node_info)"
				if [ -n "${id:-}" ] && [ "$id $want" != "$preset" ]; then
					set_headroom "$id" "$want" && preset="$id $want" &&
						say "forced $fr Hz: headroom $want for the next playback"
				fi
			fi
		fi
	fi
	tick=$((tick + 1))
	sleep 1
done
