#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Everything we can read about the AE-9's state without changing anything:
# ALSA stream and controls, PipeWire, driver parameters, HDA stream
# registers, card (BAR2) registers, codec converter state, ChipIO (router,
# I2S clocks, DSP DMA, DBGCNTL) and the driver's recent log.
#
#   sudo scripts/ae9-full-dump.sh     -> ./ae9-dump-<time>.txt
#   AE9_DUMP_DIR=dir sudo -E scripts/ae9-full-dump.sh   -> dir/ae9-dump-<time>.txt
#
# Only reads (the same registers the driver itself reads). Take one in a bad
# state and one in a good state, then compare. For a bug report, attach the
# file. It holds no personal data beyond which apps are playing (PipeWire
# stream names) and your kernel version: look it over before posting.
# Needs the driver loaded (debugfs mounted, as on most distributions).
set -u
[ "$(id -u)" = 0 ] || exec sudo "$0" "$@"
U=${SUDO_USER:-$USER}
H=$(getent passwd "$U" | cut -d: -f6)
REPO=$(cd "$(dirname "$0")/.." && pwd)
if [ -n "${AE9_DUMP_DIR:-}" ]; then LOGS=$AE9_DUMP_DIR
elif [ -d "$REPO/.tmp" ]; then LOGS=$REPO/.tmp/logs	# a development checkout
else LOGS=$PWD; fi
mkdir -p "$LOGS" && chown "$U": "$LOGS"
OUT=$LOGS/ae9-dump-$(date +%Y%m%d-%H%M%S).txt
CMD=/sys/kernel/debug/creative_ae9/cmd
P=/sys/module/snd_hda_codec_creative_ae9/parameters
[ -w "$CMD" ] || { echo "driver not loaded or no debugfs ($CMD)" >&2; exit 1; }

dbg() { echo "$1" > $CMD; cat $CMD; }
asuser() { sudo -u "$U" XDG_RUNTIME_DIR=/run/user/$(id -u "$U") "$@"; }
sec() { printf '\n===== %s =====\n' "$1"; }

{
echo "# AE-9 full dump $(date -Is), kernel $(uname -r)"
echo "# driver $(cat /sys/module/snd_hda_codec_creative_ae9/version) $(cat /sys/module/snd_hda_codec_creative_ae9/srcversion)"

sec "ALSA playback stream"
for f in hw_params sw_params status; do echo "--- $f"; cat /proc/asound/AE9/pcm0p/sub0/$f; done
sec "ALSA capture stream"
cat /proc/asound/AE9/pcm0c/sub0/hw_params /proc/asound/AE9/pcm0c/sub0/status 2>/dev/null

sec "ALSA controls"
amixer -c AE9 contents 2>/dev/null | grep -E "^numid|: values=|Item #" | paste - - | sed -E "s/numid=[0-9]+,iface=MIXER,//"

sec "Driver parameters"
for f in $P/*; do printf '%-22s %s\n' "$(basename $f)" "$(cat $f 2>/dev/null)"; done

sec "PipeWire"
asuser pactl list sinks short
asuser pactl list sink-inputs 2>/dev/null | grep -E "Sink Input|Sink:|Corked|application.name|format\."
asuser pw-metadata -n settings 0 2>/dev/null | grep -E "rate|quantum"
timeout 4 sudo -u "$U" XDG_RUNTIME_DIR=/run/user/$(id -u "$U") pw-top -b -n 2 2>/dev/null | tail -8

sec "HDA controller and playback stream SD4 (BAR0)"
for o in 0x08 0x0c 0x0e 0x20 0x24 0x40 0x4c 0x58 0x5c 0x100 0x104 0x108 0x10c 0x110 0x118 0x11c; do
	printf '%-6s %s\n' "$o" "$(dbg "r0 $o" | grep -o '= 0x[0-9a-f]*')"
done
echo "--- SD4 position x3 (should change while playing)"
for i in 1 2 3; do dbg "r0 0x104" | grep -o '0x[0-9a-f]*$'; sleep 0.2; done | tr '\n' ' '; echo
echo "--- capture streams: SD0 (mic / line-in), SD1 (optical in), SD2 (What U Hear)"
for o in 0x80 0x84 0x88 0x90 0xa0 0xa4 0xb0 0xc0 0xc4 0xd0; do printf '%-6s %s\n' "$o" "$(dbg "r0 $o" | grep -o '= 0x[0-9a-f]*')"; done

sec "Card registers (BAR2, the ones the driver logs at bring-up)"
for o in 0x4 0x20c 0x304 0x320 0xc0c 0xc14; do printf '%-6s %s\n' "$o" "$(dbg "r2 $o" | grep -o '= 0x[0-9a-f]*')"; done

sec "ChipIO: I2S clocks, stream setup, SRC, router apply"
for a in 0x189000 0x189004 0x189008 0x189024 0x189028 0x18b008 0x18a020 0x18a024 0x18a028 0x19042c; do
	printf '%-9s %s\n' "$a" "$(dbg "chipior $a" | grep -o '=0x[0-9a-f]*')"
done

sec "ChipIO: DSP (DBGCNTL x4, fw flags, overlay check, DMA)"
for a in 0x100e30 0x100e30 0x100e30 0x100e30 0x801c 0x40004 0x3f3b4 0x110f00 0x110f04 0x110f08 0x110ff0 0x110ff8; do
	printf '%-9s %s\n' "$a" "$(dbg "chipior $a" | grep -o '=0x[0-9a-f]*')"
done

sec "ChipIO: audio router 0x190000-0x1901fc (non-zero; src -> dst, state)"
for i in $(seq 0 127); do
	a=$(printf '0x%x' $((0x190000 + i * 4)))
	v=$(dbg "chipior $a" | grep -o '=0x[0-9a-f]*' | tr -d =)
	[ -n "$v" ] && [ "$v" != 0x0 ] && printf '%s %-8s %02x -> %02x  state %d\n' "$a" "$v" \
		$((v & 0xff)) $(((v >> 8) & 0xff)) $(((v >> 16) & 3))
done

sec "Codec converter state (GET verbs; replies in the log section below)"
SINCE=$(date '+%Y-%m-%d %H:%M:%S')
# codec 1: DACs 0x02-0x04, 0x05, mic ADC 0x07, What U Hear ADC 0x0a;
# codec 2: optical out 0x05, optical in 0x09
for cn in 1:0x02 1:0x03 1:0x04 1:0x05 1:0x07 1:0x0a 2:0x05 2:0x09; do
	for v in 0xa0000 0xf0600 0xf0500 0xf0d00; do	# format, stream/channel, power, digital conv
		printf -v w '0x%x%02x%05x' ${cn%%:*} $((${cn#*:})) $((v))
		echo "verb $w" > $CMD
	done
done
sleep 0.5
journalctl -k --since "$SINCE" --no-pager | grep -o 'dbg verb.*' | awk '{
	v=strtonum($3); cad=rshift(v,28); n=and(rshift(v,20),0xff); c=and(rshift(v,8),0xfff)
	if (and(rshift(v,16),0xf)==0xa) c=0xa00
	printf "codec %d node 0x%02x verb 0x%03x -> %s %s\n", cad, n, c, $5, $6 }'

sec "Driver log (last 60 lines, no head-unit chatter)"
journalctl -k -b --no-pager | grep creative-ae9 | grep -vE "hu_debug|head unit: volume|dbg verb" | tail -60 | cut -c60-
} > "$OUT" 2>&1
chown "$U": "$OUT"
echo "saved $OUT"
