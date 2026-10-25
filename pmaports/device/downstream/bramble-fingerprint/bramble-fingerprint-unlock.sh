#!/bin/sh
# Unlock the Phosh lock screen with an enrolled fingerprint.
#
# While the graphical session on seat0 is locked (logind LockedHint), wait
# for a finger on the reader through the fingerprint HAL. On a match, unlock
# the session through logind, which Phosh follows, and turn the screen on if
# it is off. The PIN keeps working as before.
#
# The HAL does not limit wrong fingers (on Android the framework does), so
# this copies Android's limits: after 5 wrong fingers the reader stops for
# 30 seconds, and after 20 in one lock only the PIN can unlock.
#
# Runs as root (OpenRC service bramble-fingerprint-unlock).
set -u
export PATH=/usr/sbin:/usr/bin:/sbin:/bin

OUT=/run/bramble-fingerprint-unlock.verify
pid= watchdog=

log() { echo "$(date '+%F %T') $*"; }

# Sleep so that a stop signal is handled at once: the shell runs a trap only
# after a foreground command ends.
nap() {
	sleep "$1" &
	wait $!
}

# On stop, cancel the verify that is running, or it keeps the reader busy.
stop() {
	[ -n "$watchdog" ] && kill "$watchdog" 2> /dev/null
	[ -n "$pid" ] && kill -TERM "$pid" 2> /dev/null
	exit 0
}
trap stop TERM INT

# Print "<session> <user>" when the session that is active on seat0 is a
# locked user session.
locked_session() {
	id=$(loginctl show-seat seat0 -p ActiveSession --value 2> /dev/null)
	[ -n "$id" ] || return 0
	class= locked= name=
	while IFS== read -r key value; do
		case $key in
		Class) class=$value ;;
		LockedHint) locked=$value ;;
		Name) name=$value ;;
		esac
	done << EOF
$(loginctl show-session "$id" -p Class -p LockedHint -p Name 2> /dev/null)
EOF
	[ "$class" = user ] && [ "$locked" = yes ] && echo "$id $name"
	return 0
}

still_locked() {
	[ "$(loginctl show-session "$1" -p LockedHint --value 2> /dev/null)" = yes ]
}

enrolled() {
	bramble-fingerprint list 2> /dev/null | grep -q '^enrolled:'
}

rejected() {
	n=$(grep -c REJECTED "$OUT" 2> /dev/null)
	echo "${n:-0}"
}

# Turn the screen on through Phosh's DisplayConfig, on the user's session
# bus (only that user can connect to it). Setting "on" alone does nothing
# once Phosh has blanked the screen itself, so go through "off" first.
# Nothing to do when the screen is on.
screen_on() {
	grep -qx Off /sys/class/drm/card*-DSI-*/dpms 2> /dev/null || return 0
	for pid in $(pgrep -u "$1" -x phosh); do
		bus=$(tr '\0' '\n' < "/proc/$pid/environ" | sed -n 's/^DBUS_SESSION_BUS_ADDRESS=//p')
		[ -n "$bus" ] || continue
		for mode in 3 0; do
			setpriv --reuid="$(id -u "$1")" --regid="$(id -g "$1")" --init-groups \
				env DBUS_SESSION_BUS_ADDRESS="$bus" gdbus call --session \
				-d org.gnome.Mutter.DisplayConfig -o /org/gnome/Mutter/DisplayConfig \
				-m org.freedesktop.DBus.Properties.Set org.gnome.Mutter.DisplayConfig \
				PowerSaveMode "<$mode>" > /dev/null
		done
		return 0
	done
}

failed=0 # wrong fingers since the session was last unlocked
while :; do
	set -- $(locked_session)
	if [ $# -lt 2 ]; then
		failed=0
		nap 2
		continue
	fi
	session=$1 user=$2

	if [ "$failed" -ge 20 ] || ! enrolled; then
		[ "$failed" -ge 20 ] && log "20 wrong fingers: PIN only until the next unlock"
		while still_locked "$session"; do
			nap 2
		done
		continue
	fi

	# verify exits with 0 on a match and keeps waiting after a wrong finger.
	: > "$OUT"
	bramble-fingerprint --timeout 3600 verify > "$OUT" 2>&1 &
	pid=$!
	# Stop it when the session is unlocked another way, or after 5 wrong fingers.
	(
		while kill -0 "$pid" 2> /dev/null && still_locked "$session" && [ "$(rejected)" -lt 5 ]; do
			sleep 1
		done
		kill -TERM "$pid" 2> /dev/null
	) &
	watchdog=$!
	wait "$pid" 2> /dev/null
	rc=$?
	kill "$watchdog" 2> /dev/null
	wait "$watchdog" 2> /dev/null
	pid= watchdog=

	n=$(rejected)
	if [ "$rc" -eq 0 ]; then
		if still_locked "$session"; then
			log "fingerprint matched: unlocking session $session"
			loginctl unlock-session "$session"
			screen_on "$user"
		fi
		failed=0
		continue
	fi
	failed=$((failed + n))
	if [ "$n" -ge 5 ]; then
		log "$n wrong fingers ($failed since the last unlock): reader paused for 30 seconds"
		nap 30
	elif still_locked "$session"; then
		# The HAL is not there (container stopped) or timed out: do not spin.
		log "verify stopped (exit $rc): $(grep -v -e '^acquired' -e CRITICAL "$OUT" | tail -n 1)"
		nap 30
	fi
done
