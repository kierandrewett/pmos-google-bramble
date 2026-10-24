#!/bin/sh
# Pixel 4a (5G): let ModemManager use the modem's GNSS engine.
#
# 1. Engine lock. While it loads the location capabilities, ModemManager
#    sends LOC "Set Engine Lock" with lock type MT, meant to lock only
#    network-initiated sessions. This modem firmware (MPSS.HI.2.0) maps MT
#    to its client mask 0x2, which also locks ModemManager's own sessions:
#    LOC Start fails with GeneralError and the session status is
#    ENGINE_LOCKED. Lock types NONE (mask 0x0) and MI (0x1) let it start.
# 2. Time. After a modem boot the GNSS engine has no valid time and does
#    not search (empty NMEA), and it does not ask for time. On Android the
#    location stack injects NTP time. Inject the system time once chrony
#    has synced the clock; the clock reads 1970 early in boot, so injecting
#    earlier would give the engine a wrong time.
#
# ModemManager exports the modem object only after it set the lock, so do
# both each time a modem object appears on D-Bus: at boot, after a
# ModemManager restart and after a modem re-probe.

monitor_rule="type='signal',sender='org.freedesktop.ModemManager1',\
path='/org/freedesktop/ModemManager1',\
interface='org.freedesktop.DBus.ObjectManager',member='InterfacesAdded'"

setup_gnss() {
	if qmicli -p -d qrtr://0 --loc-set-engine-lock=none > /dev/null 2>&1; then
		logger -t bramble-gnss "GNSS engine lock set to none ($1)"
	else
		logger -t bramble-gnss "could not set the GNSS engine lock ($1)"
	fi
	# chronyc waitsync: up to 60 tries, 10 s apart, until the offset is
	# below 0.5 s. In the background, so the next event is not delayed.
	(
		if chronyc waitsync 60 0.5 > /dev/null 2>&1 &&
			qmicli -p -d qrtr://0 --loc-inject-time > /dev/null 2>&1; then
			logger -t bramble-gnss "GNSS time injected ($1)"
		else
			logger -t bramble-gnss "could not inject the GNSS time ($1)"
		fi
	) &
}

# ash runs a trap only after a foreground pipeline ends, and dbus-monitor
# does not end, so run the pipeline in the background and wait for it.
trap 'trap - TERM INT; pkill -P $$; exit 0' TERM INT

while :; do
	dbus-monitor --system "$monitor_rule" 2> /dev/null | while read -r line; do
		case "$line" in
		*member=NameAcquired*)
			# The monitor is listening now: handle a modem that
			# ModemManager exported before it started.
			if mmcli -L 2> /dev/null | grep -q /Modem/; then
				setup_gnss "modem already present"
			fi
			;;
		*'object path "/org/freedesktop/ModemManager1/Modem/'*)
			setup_gnss "modem added"
			;;
		esac
	done &
	wait $!
	sleep 5
done
