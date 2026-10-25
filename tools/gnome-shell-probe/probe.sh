#!/bin/sh
# Run stock GNOME Shell headless on the phone and save a screenshot of its
# virtual monitor. Nothing is installed or changed: the display, Phosh and
# greetd are not touched. See docs/hardware/gnome-shell.md.
#
# Needs on the phone: gnome-shell, gnome-session (apk add gnome-shell
# gnome-session) and python3 with gi. Run it as the normal user, inside the
# Phosh session (over SSH, set XDG_RUNTIME_DIR first: see docs/guides/development.md).
#
# usage: probe.sh [OUT.png] [WIDTHxHEIGHT]
set -eu
export PATH=/usr/sbin:/usr/bin:/sbin:/bin

out=${1:-/tmp/gnome-shell-probe.png}
size=${2:-360x780}
here=$(dirname "$(readlink -f "$0")")
work=$(mktemp -d /tmp/gnome-shell-probe.XXXXXX)
trap 'rm -rf "$work"' EXIT INT TERM

# gnome-shell imports the Gdm typelib and the org.gnome.login-screen schema.
# Both come from the gdm package, which would also pull in more services, so
# unpack the package here and point GNOME Shell at the files.
(cd "$work" && apk fetch -q gdm && tar xzf gdm-*.apk usr/lib usr/share/glib-2.0/schemas 2> /dev/null)
glib-compile-schemas "$work/usr/share/glib-2.0/schemas"

export MESA_LOADER_DRIVER_OVERRIDE=zink
export XDG_SESSION_TYPE=wayland
export XDG_CURRENT_DESKTOP=GNOME
export GI_TYPELIB_PATH=$work/usr/lib/girepository-1.0
export LD_LIBRARY_PATH=$work/usr/lib
export GSETTINGS_SCHEMA_DIR=$work/usr/share/glib-2.0/schemas

# Own session bus, so the shell does not meet the bus that Phosh uses.
dbus-run-session -- sh -c '
	gnome-shell --headless --wayland --no-x11 --virtual-monitor "$2" > "$3/shell.log" 2>&1 &
	shell=$!
	sleep 15
	python3 "$1/shot.py" "$4"
	kill "$shell"
	wait "$shell" 2> /dev/null || true
' sh "$here" "$size" "$work" "$out"
echo "screenshot: $out"
echo "shell log (errors only):"
grep -E "CRITICAL|ERROR|WARNING" "$work/shell.log" || true
