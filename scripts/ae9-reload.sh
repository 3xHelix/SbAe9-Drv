#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 3xHelix
# Reload the AE-9 driver in one go (a Sound Blaster AE-7 on it too: the
# card is found by its driver, ALSA id AE9 or AE7): stop what holds the card
# (PipeWire, WirePlumber, the AE-9 helper services, alsa-state), unload and
# load the driver, wait for the card, start everything again. For a card that
# "stopped answering", or to pick up new driver options. Packaged as
# ae9-reload.
#
#   ae9-reload            as your desktop user (asks for sudo for the driver)
#   ae9-reload --check    only show what holds the card
#   ae9-reload --force    reload even if something else still holds it
#
# SbAe9-SBC and SbAe9-AudioMon let go of the card by themselves once the
# driver says it stopped answering (driver 0.12.9+), and, in versions that
# know it, while the reload notice below exists; anything else that holds
# it is listed and the reload stops, unless --force.
#
# The reload notice (0.12.13): /run/user/<uid>/ae9-reload, the desktop
# user's, holding this script's PID, from just before the unload until the
# end (removed on any exit). An app that holds the card closes it while the
# file exists and that PID is alive, and opens it again once the file is
# gone (or its PID is: a reload killed half-way).
# PipeWire's live clock.force-rate and clock.force-quantum (pw-metadata -n
# settings) don't survive its restart: when set (not 0) they're saved before
# the stop and set again after.
# A reload doesn't fix everything: when the card's DSP won't start again,
# only a full power-off does (PSU switch or unplug for 15-30 s).
set -u
MOD=snd-hda-codec-creative-ae9
SYSMOD=/sys/module/snd_hda_codec_creative_ae9
OURS=(ae9-headroom.service sbae9-sbc-encoder.service)
PW=(pipewire.socket pipewire-pulse.socket pipewire.service pipewire-pulse.service wireplumber.service)
SYS=(alsa-state.service)
WAIT=90		# seconds for the card to come back (a bring-up with retries can take ~70)

CHECK=0 FORCE=0
for a in "$@"; do
	case $a in
	--check) CHECK=1 ;;
	--force) FORCE=1 ;;
	-h|--help) sed -n '4,27p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "unknown option: $a (see --help)" >&2; exit 2 ;;
	esac
done

# who runs it: as the desktop user, or with sudo (then the user is SUDO_USER)
if [ "$(id -u)" = 0 ]; then
	SU=""; DUSER=${SUDO_USER:-}
else
	SU=sudo; DUSER=$(id -un)
fi
ucmd() {	# systemctl --user for the desktop user
	[ -n "$DUSER" ] && [ "$DUSER" != root ] || return 1
	if [ "$(id -un)" = "$DUSER" ]; then
		systemctl --user "$@"
	else
		local uid; uid=$(id -u "$DUSER")
		sudo -u "$DUSER" XDG_RUNTIME_DIR=/run/user/$uid \
			DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$uid/bus systemctl --user "$@"
	fi
}

urun() {	# a command as the desktop user, in their session (for pw-metadata)
	[ -n "$DUSER" ] && [ "$DUSER" != root ] || return 1
	if [ "$(id -un)" = "$DUSER" ]; then
		"$@"
	else
		sudo -u "$DUSER" XDG_RUNTIME_DIR=/run/user/"$(id -u "$DUSER")" "$@"
	fi
}
pw_saved=()
pw_save() {	# "key value" for clock.force-rate / clock.force-quantum when not 0
	local k v line
	command -v pw-metadata >/dev/null || return 0
	line=$(urun pw-metadata -n settings 0 2>/dev/null) || return 0
	for k in clock.force-rate clock.force-quantum; do
		v=$(printf '%s\n' "$line" | sed -n "s/.*key:'$k' value:'\([0-9]*\)'.*/\1/p" | head -1)
		[ -n "$v" ] && [ "$v" != 0 ] && pw_saved+=("$k $v")
	done
	return 0
}
pw_restore() {	# once PipeWire answers again (up to 10 s)
	local kv i
	[ ${#pw_saved[@]} = 0 ] && return 0
	for ((i = 0; i < 20; i++)); do
		urun pw-metadata -n settings 0 >/dev/null 2>&1 && break
		sleep 0.5
	done
	for kv in "${pw_saved[@]}"; do
		# shellcheck disable=SC2086
		urun pw-metadata -n settings 0 $kv >/dev/null 2>&1
	done
	echo "PipeWire settings set again: ${pw_saved[*]}"
}

card_sys() {	# the sound card our driver registered (/sys/class/sound/cardN)
	local c
	for c in /sys/class/sound/card[0-9]*; do
		[ "$(basename "$(readlink -f "$c/device/driver" 2>/dev/null)")" = "$MOD" ] || continue
		echo "$c"; return 0
	done
	return 1
}
card_id() {	# its ALSA id: AE9, or AE7 for a Sound Blaster AE-7
	local c; c=$(card_sys) || return 1
	cat "$c/id"
}
card_nodes() {	# the card's device files, while it exists
	local c; c=$(card_sys) || return
	c=${c##*/card}
	ls /dev/snd/controlC"$c" /dev/snd/pcmC"$c"D* /dev/snd/hwC"$c"D* 2>/dev/null
}
holders() {	# processes holding the card (fuser -v lines), none = empty
	local n; n=$(card_nodes); [ -n "$n" ] || return 0
	local f=fuser	# without sudo, only your own processes show
	[ -z "$SU" ] || ! sudo -n true 2>/dev/null || f="sudo fuser"
	# shellcheck disable=SC2086
	$f -v $n 2>&1 | grep -v '^ *USER' | grep -vE '^\s*$' || true
}

if [ $CHECK = 1 ]; then
	[ -d "$SYSMOD" ] && echo "driver: $(cat $SYSMOD/version 2>/dev/null) loaded" || echo "driver: not loaded"
	id=$(card_id) && echo "card: registered ($id)" || echo "card: not registered"
	h=$(holders); echo "holding the card:"; echo "${h:-  nothing}"
	exit 0
fi

[ -z "$SU" ] || sudo -v || exit 1
modinfo $MOD >/dev/null 2>&1 || { echo "$MOD isn't installed for $(uname -r)" >&2; exit 1; }
[ -n "$DUSER" ] && [ "$DUSER" != root ] || \
	echo "note: no desktop user (run it as yourself, or with sudo from your session): PipeWire left alone" >&2

# stop what holds the card, remembering what ran
ran_user=() ran_sys=()
for s in "${OURS[@]}" "${PW[@]}"; do
	ucmd is-active --quiet "$s" 2>/dev/null && ran_user+=("$s")
done
for s in "${SYS[@]}"; do
	systemctl is-active --quiet "$s" 2>/dev/null && ran_sys+=("$s")
done
restart() {
	local s
	[ ${#ran_sys[@]} = 0 ] || $SU systemctl start "${ran_sys[@]}"
	# sockets and PipeWire first, then WirePlumber, then the AE-9 services
	for s in "${PW[@]}" "${OURS[@]}"; do
		[[ " ${ran_user[*]} " == *" $s "* ]] && ucmd start "$s"
	done
	[[ " ${ran_user[*]} " == *" pipewire.service "* ]] && pw_restore
	return 0
}
pw_save
echo "stopping: ${ran_user[*]:-} ${ran_sys[*]:-}"
[ ${#ran_user[@]} = 0 ] || ucmd stop "${ran_user[@]}"
[ ${#ran_sys[@]} = 0 ] || $SU systemctl stop "${ran_sys[@]}"
sleep 2

# the reload notice (see the top): the apps that know it let go now
NOTICE=""
notice_off() { [ -z "$NOTICE" ] || rm -f "$NOTICE"; }
if [ -n "$DUSER" ] && [ "$DUSER" != root ]; then
	ndir=/run/user/$(id -u "$DUSER")
	if [ -d "$ndir" ]; then
		NOTICE=$ndir/ae9-reload
		trap notice_off EXIT
		if [ "$(id -un)" = "$DUSER" ]; then
			echo $$ > "$NOTICE"
		else
			echo $$ | sudo -u "$DUSER" tee "$NOTICE" >/dev/null
		fi
		# up to 5 s for them to close the card
		for ((i = 0; i < 10; i++)); do
			[ -z "$(holders)" ] && break
			sleep 0.5
		done
	fi
fi

h=$(holders)
if [ -n "$h" ] && [ $FORCE = 0 ]; then
	echo "Still holding the card (close these, or use --force):"; echo "$h"
	restart; exit 1
fi

t=$(date +%s)
echo "unloading the driver..."
if [ -d "$SYSMOD" ] && ! $SU modprobe -r $MOD; then
	echo "The driver wouldn't unload. Holding the card:"; holders
	restart; exit 1
fi
echo "loading the driver (the card's start-up takes 10-25 s)..."
$SU modprobe $MOD || { echo "modprobe $MOD failed" >&2; restart; exit 1; }

# An AE-7 with its support off (parked, or never turned on) gets no card:
# the driver leaves it alone on purpose. Without an AE-9 there's nothing
# to wait for, and nothing for a power-off to fix.
ae7_off=0
if [ "$(cat "$SYSMOD/parameters/ae7" 2>/dev/null)" = 0 ]; then
	n9=0; n7=0
	for d in /sys/bus/pci/devices/*; do
		[ "$(cat "$d/vendor" 2>/dev/null):$(cat "$d/device" 2>/dev/null)" = 0x1102:0x0010 ] || continue
		case $(cat "$d/subsystem_device" 2>/dev/null) in
		0x0071) n9=$((n9 + 1)) ;;
		0x0081) n7=$((n7 + 1)) ;;
		esac
	done
	[ $n9 = 0 ] && [ $n7 -gt 0 ] && ae7_off=1
fi
if [ $ae7_off = 1 ]; then
	restart
	cat >&2 <<'MSG'
The driver is loaded, but its AE-7 support is off, so it leaves the AE-7
alone (no card). That's on purpose, not a fault: see
  ae9-ae7-enable --status
and docs/AE7.md to turn the support on (sudo ae9-ae7-enable, then reboot).
MSG
	exit 1
fi

ok=0
for ((i = 0; i < WAIT; i++)); do
	if card_id >/dev/null; then ok=1; break; fi
	if $SU journalctl -k --since "@$t" --no-pager -o cat 2>/dev/null | \
	   grep -q 'creative-ae9.*\(not registering the card\|probe with driver .* failed\)'; then
		break
	fi
	sleep 1
done
restart

if [ $ok = 1 ]; then
	id=$(card_id)
	ans=$(amixer -c "$id" cget name='Card Answering' 2>/dev/null | sed -n 's/.*: values=//p')
	echo "done: driver $(cat $SYSMOD/version 2>/dev/null), card $id back${ans:+ (Card Answering: $ans)}"
else
	cat >&2 <<'MSG'
The driver loaded, but the card didn't start (its DSP stayed busy).
A reload can't fix that: shut down, switch the PSU off (or unplug) for
15-30 s, and boot again. The kernel log has the details:
  journalctl -k -b | grep creative-ae9 | tail -30
MSG
	exit 1
fi
