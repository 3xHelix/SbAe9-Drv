#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
# Everything we can read about the AE-9's state without changing anything:
# ALSA stream and controls, PipeWire, driver parameters, HDA stream
# registers, card (BAR2) registers, codec converter state, ChipIO (router,
# I2S clocks, DSP DMA, DBGCNTL) and the driver's recent log.
#
#   ae9-full-dump              -> ~/ae9-dumps/ae9-dump-<time>.txt (asks for sudo)
#   AE9_DUMP_DIR=dir ae9-full-dump   -> dir/ae9-dump-<time>.txt
#   ae9-full-dump --report     -> ~/ae9-dumps/ae9-report-<time>/ (the dump,
#       system.txt: versions and install, kernel-log.txt: the driver's lines
#       from this boot) and ae9-report-<time>.tar.gz of it, to attach to an
#       issue
# From a development checkout (scripts/ae9-full-dump.sh with a .tmp folder
# next to it) the default is .tmp/logs instead. ~ is the home of the user who
# ran sudo, and the folder and file belong to that user.
#
# Only reads (the same registers the driver itself reads, plus sysfs, lspci,
# pactl and udevadm for the system and PCI part). Take one in a bad
# state and one in a good state, then compare. For a bug report, attach the
# file. It holds no personal data beyond which apps are playing (PipeWire
# stream names), your kernel, sound hardware and the names of your audio
# config files: look it over before posting.
# Needs the driver loaded (debugfs mounted, as on most distributions).
# Each run also leaves an "ae9-full-dump: saved <file>" line in the kernel log.
set -u
REPORT=0
case "${1:-}" in
"") ;;
--report) REPORT=1 ;;
*) echo "usage: ae9-full-dump [--report]" >&2; exit 2 ;;
esac
[ "$(id -u)" = 0 ] || exec sudo ${AE9_DUMP_DIR:+AE9_DUMP_DIR="$AE9_DUMP_DIR"} "$0" "$@"
U=${SUDO_USER:-$USER}
H=$(getent passwd "$U" | cut -d: -f6)
REPO=$(cd "$(dirname "$0")/.." && pwd)
if [ -n "${AE9_DUMP_DIR:-}" ]; then LOGS=$AE9_DUMP_DIR
elif [ -d "$REPO/.tmp" ]; then LOGS=$REPO/.tmp/logs	# a development checkout
else LOGS=$H/ae9-dumps; fi
mkdir -p "$LOGS" && chown "$U": "$LOGS"
STAMP=$(date +%Y%m%d-%H%M%S)
if [ $REPORT = 1 ]; then
	RDIR=$LOGS/ae9-report-$STAMP
	mkdir -p "$RDIR" || exit 1
	OUT=$RDIR/ae9-dump-$STAMP.txt
else
	OUT=$LOGS/ae9-dump-$STAMP.txt
fi
CMD=/sys/kernel/debug/creative_ae9/cmd
P=/sys/module/snd_hda_codec_creative_ae9/parameters
[ -w "$CMD" ] || { echo "driver not loaded or no debugfs ($CMD)" >&2; exit 1; }

dbg() { echo "$1" > $CMD; cat $CMD; }
asuser() { sudo -u "$U" XDG_RUNTIME_DIR=/run/user/$(id -u "$U") "$@"; }
sec() { printf '\n===== %s =====\n' "$1"; }
# the card's sysfs: sound card (cardN), its PCI device, and the PCI name
# PipeWire uses (alsa_output.pci-0000_06_00.0.*)
SCARD=$(readlink /proc/asound/AE9 2>/dev/null)
DEV=$(readlink -f /sys/class/sound/$SCARD/device 2>/dev/null)
ae9pci=$(basename "$DEV" | tr : _)

{
echo "# AE-9 full dump $(date -Is), kernel $(uname -r)"
echo "# driver $(cat /sys/module/snd_hda_codec_creative_ae9/version) $(cat /sys/module/snd_hda_codec_creative_ae9/srcversion)"

sec "ALSA playback stream"
for f in hw_params sw_params status; do echo "--- $f"; cat /proc/asound/AE9/pcm0p/sub0/$f; done
sec "ALSA capture streams and optical out"
# every recording device and the optical out (the driver's device numbers),
# "closed" when nothing has it open
for d in "0c head unit mic (XLR/TRS jack + 3.5 mm mic jack)" "1c optical in" "2c What U Hear" \
	 "3c head unit rear RCA in" "4p optical out"; do
	n=${d%% *}
	f=/proc/asound/AE9/pcm$n/sub0
	echo "--- hw:AE9,${n%?} ${d#* }"
	if [ "$(cat $f/status 2>/dev/null)" = closed ] || [ ! -r $f/status ]; then echo closed
	else cat $f/hw_params $f/status 2>/dev/null; fi
done

sec "ALSA controls"
# one line per control: name + values (enum item lists dropped; pairing lines two at a time broke on them)
amixer -c AE9 contents 2>/dev/null | awk '/^numid=/{if(l)print l; sub(/numid=[0-9]+,iface=MIXER,/,""); l=$0; next}
	/: values=/{sub(/^ *: /,""); l=l"  "$0} END{if(l)print l}'

sec "Driver parameters"
for f in $P/*; do printf '%-22s %s\n' "$(basename $f)" "$(cat $f 2>/dev/null)"; done

sec "Driver state (debugfs state: modes, counters, router streams, chip flags)"
cat /sys/kernel/debug/creative_ae9/state 2>/dev/null || echo "(no state file: driver older than 0.12.6)"

sec "PipeWire"
asuser pactl list sinks short
# LC_ALL=C: pactl translates its labels ("Volume :" in French), which the
# grep would miss. Volume and Channel Map show a stream with one channel
# turned down or mapped away.
asuser LC_ALL=C pactl list sink-inputs 2>/dev/null |
	grep -E "Sink Input|Sink:|Corked|application.name|format\.|Volume:|Channel Map:"
# what feeds each AE-9 output port: a channel with no link plays nothing
echo "--- links into the AE-9 outputs (pw-link)"
[ -n "$ae9pci" ] && asuser pw-link -l 2>/dev/null |
	awk -v t="alsa_output.pci-$ae9pci." 'index($0, t) == 1 && /:playback_/ {p = 1; print; next}
		/^ / && p {print; next} {p = 0}'
echo "--- PipeWire clock settings (pw-metadata settings: rate, quantum)"
asuser pw-metadata -n settings 0 2>/dev/null | grep -E "rate|quantum"
# every running node from pw-top's second sample: a driver (a clock) first,
# its followers under it ("+", "="); ERR = that node's xruns since it started
echo "--- pw-top (running nodes; ERR = PipeWire xruns per node)"
timeout 4 sudo -u "$U" XDG_RUNTIME_DIR=/run/user/$(id -u "$U") pw-top -b -n 2 2>/dev/null |
	awk '/^S +ID +QUANT/ {n++; if (n == 2) print; next} n == 2 && $1 == "R"'
# without our profile set (89-ae9-pipewire.rules -> ACP_PROFILE_SET)
# PipeWire offers only "stereo-fallback": no optical in, What U Hear, rear RCA
echo "--- card profile"
udevadm info "/sys/class/sound/$SCARD" 2>/dev/null | grep -E "ACP_|PULSE_" | sed 's/^E: //'
asuser LC_ALL=C pactl list cards 2>/dev/null |
	awk -v t="alsa_card.pci-$ae9pci" '/^Card #/ {p = 0} $1 == "Name:" {p = ($2 == t)} p && /Active Profile:/ {sub(/^\t/, ""); print}'
echo "--- PipeWire / WirePlumber config files (names only)"
find "$H/.config/pipewire" "$H/.config/wireplumber" /etc/pipewire /etc/wireplumber -type f 2>/dev/null |
	sed "s|^$H|~|"

sec "System and PCI (read only; the card isn't touched)"
echo "up $(cut -d. -f1 /proc/uptime) s, load $(cut -d' ' -f1-3 /proc/loadavg), suspends since boot $(cat /sys/power/suspend_stats/success 2>/dev/null)"
# without root=, resume= and the like (disk ids)
echo "cmdline: $(tr ' ' '\n' < /proc/cmdline | grep -vE '^(BOOT_IMAGE|root|resume|rd\.luks|rd\.lvm|cryptdevice|initrd)=' |
	grep -vi uuid | tr '\n' ' ')"
echo "--- driver options set in modprobe.d"
grep -rsH -iE "ae9|creative" /etc/modprobe.d /usr/lib/modprobe.d /run/modprobe.d | grep -v ':#' | cut -c1-200 ||
	echo "(none)"
echo "--- sound cards"
cat /proc/asound/cards
echo "--- card interrupt (count per CPU, type)"
grep -E "creative-ae9" /proc/interrupts | tr -s ' '
g=$(readlink "$DEV/iommu_group" 2>/dev/null)
if [ -n "$g" ]; then echo "--- IOMMU: on, card in group ${g##*/}"
else echo "--- IOMMU: off (no group for the card)"; fi
# the card and every bridge up to the root port: the PCIe link (speed,
# width, ASPM) lives on the bridges; AER counts link errors
echo "--- PCI path (card first, then each bridge up to the root port)"
for d in $(echo "$DEV" | tr / '\n' | grep -E '^[0-9a-f]{4}:[0-9a-f]{2}:' | tac); do
	s=/sys/bus/pci/devices/$d
	lspci -vvv -s "$d" 2>/dev/null | grep -E "^[0-9a-f]|LnkCap:|LnkCtl:|LnkSta:|DevSta:|UESta:|CESta:|MSI: Enable|Status: D[0-3]|Kernel driver"
	echo "	power: runtime $(cat $s/power/runtime_status 2>/dev/null), control $(cat $s/power/control 2>/dev/null)"
	for a in correctable nonfatal fatal; do
		[ -r "$s/aer_dev_$a" ] && printf '\tAER %s: %s\n' $a "$(grep -E '^TOTAL' "$s/aer_dev_$a" | cut -d' ' -f2)"
	done
	# the port services the kernel bound (pcieNNN: ...01 PME, ...02 AER,
	# ...04 hotplug, ...08 DPC, ...10 link bandwidth); only a root port's
	# own AER service counts errors, for every device below it
	svc=$(ls -d /sys/bus/pci_express/devices/"$d":pcie* 2>/dev/null | sed 's/.*:pcie//' | tr '\n' ' ')
	[ -n "$svc" ] && printf '\tport services: %s\n' "$svc"
done
# the AER counts above only move when the root port has the AER service
root=$(echo "$DEV" | tr / '\n' | grep -E '^[0-9a-f]{4}:[0-9a-f]{2}:' | head -1)
if ls -d /sys/bus/pci_express/devices/"$root":pcie?02 >/dev/null 2>&1; then
	echo "--- AER: on (root port $root has the AER service; the counts above are live)"
else
	echo "--- AER: off for this card (no AER service on root port $root: no AER capability there, the firmware kept AER, or pci=noaer): the AER counts above can't move; UESta/CESta (lspci) still show error bits"
fi

sec "HDA controller and playback stream SD4 (BAR0)"
for o in 0x08 0x0c 0x0e 0x20 0x24 0x40 0x4c 0x58 0x5c 0x100 0x104 0x108 0x10c 0x110 0x118 0x11c; do
	printf '%-6s %s\n' "$o" "$(dbg "r0 $o" | grep -o '= 0x[0-9a-f]*')"
done
echo "--- SD4 position x3 (should change while playing)"
for i in 1 2 3; do dbg "r0 0x104" | grep -o '0x[0-9a-f]*$'; sleep 0.2; done | tr '\n' ' '; echo
echo "--- capture streams: SD0 (hw:AE9,0 head unit mic: XLR/TRS + 3.5 mm mic jack), SD1 (hw:AE9,1 optical in), SD2 (hw:AE9,2 What U Hear), SD3 (hw:AE9,3 head unit rear RCA in)"
for o in 0x80 0x84 0x88 0x90 0xa0 0xa4 0xb0 0xc0 0xc4 0xd0 0xe0 0xe4 0xe8 0xf0; do printf '%-6s %s\n' "$o" "$(dbg "r0 $o" | grep -o '= 0x[0-9a-f]*')"; done
echo "--- optical out stream SD6 (hw:AE9,4)"
for o in 0x140 0x144 0x148 0x14c 0x150; do printf '%-6s %s\n' "$o" "$(dbg "r0 $o" | grep -o '= 0x[0-9a-f]*')"; done

sec "Card registers (BAR2, the ones the driver logs at bring-up)"
for o in 0x4 0x1c 0x20c 0x304 0x320 0xc0c 0xc14; do printf '%-6s %s\n' "$o" "$(dbg "r2 $o" | grep -o '= 0x[0-9a-f]*')"; done

# ChipIO reads go through codec 1 one verb at a time; hundreds of them
# (the router scan) while audio plays killed codec 1 on 2026-11-07 22:30
# (CORB timeouts, IOMMU faults, PSU cycle needed). Skip them while any AE-9
# stream runs (DRAINING too: its DMA still runs); ALLOW_PLAYING_CHIPIO=1
# forces them.
# Not consistent with the debugfs state above, which reads always (~300
# ChipIO and ~80 router XRAM reads, through the driver's locks): the
# 2026-11-07 death predates the RIRB fix (0.12.10) that turned out to be
# behind the reads' "wear", and since then about 20 dumps read the state
# while audio played without a problem. Kept until a card test shows the
# scan below is safe while playing too.
playing=$(grep -lsE "^state: (RUNNING|DRAINING)" /proc/asound/AE9/pcm*/sub*/status 2>/dev/null | tr '\n' ' ')
if [ -n "$playing" ] && [ "${ALLOW_PLAYING_CHIPIO:-0}" != 1 ]; then
sec "ChipIO sections skipped"
echo "skipped: AE-9 stream(s) running ($playing). Stop playback and dump again for them."
else
sec "ChipIO: I2S clocks, stream setup, SRC, router apply"
for a in 0x189000 0x189004 0x189008 0x189024 0x189028 0x18b008 0x18a020 0x18a024 0x18a028 0x19042c; do
	printf '%-9s %s\n' "$a" "$(dbg "chipior $a" | grep -o '=0x[0-9a-f]*')"
done

sec "ChipIO: I2S output check and power registers"
# Windows' I2S DAC check (for its I2S-DAC cards) wants 0x189030 = 0x82 and
# 0x18902c = 3. A healthy AE-9 reads 0x81 / 0xf instead (2026-11-06, idle,
# audio fine; 0x81 = the rate class the 0x725 request sets for 44.1-192 kHz),
# so the notes compare against the AE-9's own values. 0x18b030 = PMR1 (bit 4
# PDSNS, bit 3 SNSP); 0x18b0a4 bit 1 is set at codec start-up; 0x18b008 bit 3
# is cleared at every stream start.
for a in 0x189030 0x18902c 0x18b030 0x18b0a4 0x18b008; do
	v=$(dbg "chipior $a" | grep -o '=0x[0-9a-f]*')
	note=""
	case $a in
	0x189030) [ "$v" = "=0x81" ] && note="as usual (AE-9 0x81)" || note="<-- differs from the AE-9's usual 0x81" ;;
	0x18902c) [ "$v" = "=0xf" ] && note="as usual (AE-9 0xf)" || note="<-- differs from the AE-9's usual 0xf" ;;
	esac
	printf '%-9s %-12s %s\n' "$a" "$v" "$note"
done
# BAR2 0x800 is the DACs' I2C controller (a DW_apb_i2c). Never add 0x840,
# 0x854 or 0x860 here: they clear on read, and r2 doesn't take the driver's
# DAC lock, so such a read races the driver and steals a DAC command's NACK
# (0.12.16: the driver state's "DAC I2C" line counts them).
echo "--- BAR2 0x800 block (codec 2): 0x86c bit 0 enable, 0x89c bit 0 ack, 0x800 mode"
for o in 0x800 0x804 0x86c 0x89c; do printf '%-6s %s\n' "$o" "$(dbg "r2 $o" | grep -o '= 0x[0-9a-f]*')"; done

sec "ChipIO: DSP (DBGCNTL x4, fw flags, overlay check, DMA)"
for a in 0x100e30 0x100e30 0x100e30 0x100e30 0x801c 0x40004 0x3f3b4 0x110f00 0x110f04 0x110f08 0x110ff0 0x110ff8; do
	printf '%-9s %s\n' "$a" "$(dbg "chipior $a" | grep -o '=0x[0-9a-f]*')"
done

sec "ChipIO: audio router 0x190000-0x1901fc (non-zero; src -> dst, state)"
# Single ChipIO reads glitch while audio plays (an entry can read as 0):
# read until two reads in a row agree, as the driver does; "unsteady" marks
# an entry that never did.
rt_read() {
	local p= v t
	for t in 1 2 3 4 5 6; do
		v=$(dbg "chipior $1" | grep -o '=0x[0-9a-f]*' | tr -d =)
		[ -n "$v" ] && [ "$v" = "$p" ] && { echo "$v"; return; }
		p=$v
	done
	echo "$v unsteady"
}
for i in $(seq 0 127); do
	a=$(printf '0x%x' $((0x190000 + i * 4)))
	set -- $(rt_read "$a"); v=${1:-}; note=${2:+  (reads unsteady)}
	[ -n "$v" ] && [ "$v" != 0x0 ] && printf '%s %-8s %02x -> %02x  state %d%s\n' "$a" "$v" \
		$((v & 0xff)) $(((v >> 8) & 0xff)) $(((v >> 16) & 3)) "$note"
done

fi

sec "Codec converter state (GET verbs; replies in the log section below)"
SINCE=$(date '+%Y-%m-%d %H:%M:%S')
# codec 1: DACs 0x02-0x04, 0x05, mic ADC 0x07, What U Hear ADC 0x0a;
# codec 2: optical out 0x05, head unit rear RCA in ADC 0x08, optical in 0x09
for cn in 1:0x02 1:0x03 1:0x04 1:0x05 1:0x07 1:0x0a 2:0x05 2:0x08 2:0x09; do
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

sec "Driver log (last 60 lines, no head-unit chatter or debugfs reads)"
# local time to the millisecond, then the message without the device prefix
journalctl -k -b -o short-iso-precise --no-hostname --no-pager | grep -E "creative.ae9" |
	grep -vE "hu_debug|head unit: volume|dbg (verb|dacr)" | tail -60 |
	sed -E 's/^([0-9T:-]+\.[0-9]{3})[0-9]*[+-][0-9:]+ kernel: /\1 /
		s/^([^ ]+ )snd-hda-codec-creative-ae9 [0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9]: /\1/'
} > "$OUT" 2>&1
chown "$U": "$OUT"
# a line in the kernel log (dmesg / journalctl -k), so a later log shows
# that a dump was taken, when, and where it went
echo "<5>ae9-full-dump: saved $OUT (driver $(cat /sys/module/snd_hda_codec_creative_ae9/version 2>/dev/null) $(cat /sys/module/snd_hda_codec_creative_ae9/srcversion 2>/dev/null))" > /dev/kmsg 2>/dev/null
[ $REPORT = 1 ] || { echo "saved $OUT"; exit 0; }

# --report: what the issue checklist asks for, beside the dump
{
echo "# AE-9 report $(date -Is)"
echo "--- system"
uname -r
grep -E "^(PRETTY_NAME|VERSION_ID)=" /etc/os-release 2>/dev/null
echo "--- driver"
for f in version srcversion; do
	printf '%-11s %s\n' $f "$(cat /sys/module/snd_hda_codec_creative_ae9/$f 2>/dev/null)"
done
printf '%-11s %s\n' file "$(modinfo -n snd-hda-codec-creative-ae9 2>/dev/null)"
echo "--- install"
command -v dkms >/dev/null && dkms status 2>/dev/null | grep -i ae9
command -v rpm >/dev/null && rpm -qa 2>/dev/null | grep -i creative-ae9
command -v dpkg-query >/dev/null && dpkg-query -W 2>/dev/null | grep -i creative-ae9
command -v pacman >/dev/null && pacman -Q 2>/dev/null | grep -i creative-ae9
echo "--- firmware"
fw=$(command -v ae9-get-firmware || echo "$REPO/scripts/ae9-get-firmware.sh")
[ -x "$fw" ] && "$fw" --check 2>&1 || echo "(ae9-get-firmware not found)"
echo "--- audio stack"
asuser pipewire --version 2>/dev/null | tail -1
asuser wireplumber --version 2>/dev/null | tail -1
amixer -c AE9 cget name='Driver Health' 2>/dev/null | tail -1 | sed 's/^ *: /Driver Health /'
echo "--- card (lspci)"
lspci -nnk -d 1102: 2>/dev/null
} > "$RDIR/system.txt" 2>&1
# this boot's driver lines and IOMMU faults (no host name)
{
journalctl -k -b -o short-monotonic --no-hostname --no-pager 2>/dev/null |
	grep -iE "creative.ae9|ae9-full-dump|AMD-Vi|DMAR|IO_PAGE_FAULT" | grep -vE "dbg verb|hu_debug"
# how the last boot left the card (a dark card after a warm reboot)
echo "--- previous boot: the driver's last 40 lines"
journalctl -k -b -1 -o short-monotonic --no-hostname --no-pager 2>/dev/null |
	grep -iE "creative.ae9" | grep -vE "dbg verb|hu_debug" | tail -40
} > "$RDIR/kernel-log.txt"
TGZ=$LOGS/ae9-report-$STAMP.tar.gz
chown -R "$U": "$RDIR"
tar -C "$LOGS" -czf "$TGZ" "ae9-report-$STAMP"
chown "$U": "$TGZ"
echo "saved $RDIR/ (to read)"
echo "and   $TGZ (attach this one to your issue)"
