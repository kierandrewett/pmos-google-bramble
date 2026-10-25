#!/bin/sh
# Fingerprint enrol/verify/list/remove against the fps_hal running in the
# Android container (bramble-android-hal). Runs in a private mount namespace
# with a tmpfs /dev, so libgbinder sees the literal /dev/hwbinder and talks
# to the container's hwbinder node. The host /dev is untouched.
#
# usage: bramble-fingerprint [--gid N] [--fid N] [--timeout S]
#                            status|enroll|verify|list|remove|hmac-share|hat-check
set -eu

if [ "$(id -u)" != 0 ]; then
	exec sudo -- "$0" "$@"
fi

PIDFILE=/run/bramble-android/init.pid
if [ ! -r "$PIDFILE" ]; then
	echo "bramble-fingerprint: the Android container is not running ($PIDFILE missing)" >&2
	exit 2
fi
P=$(cat "$PIDFILE")
S=$(ps -o pid,ppid,comm | awk -v p="$P" '$2==p && $3=="logd" {print $1}')
if [ -z "$S" ]; then
	echo "bramble-fingerprint: no logd under container init $P" >&2
	exit 2
fi

export FP_NODE=/proc/$S/root/dev/binderfs/hwbinder
export FP_STATE=${FP_STATE:-/var/lib/bramble-fingerprint}

exec unshare -m /bin/sh -c '
mount --make-rprivate /
mount -t tmpfs none /dev
mknod -m 666 /dev/null c 1 3
mknod -m 666 /dev/zero c 1 5
mknod -m 666 /dev/random c 1 8
mknod -m 666 /dev/urandom c 1 9
ln -s "$FP_NODE" /dev/hwbinder
exec /usr/libexec/bramble-fingerprint --state "$FP_STATE" "$@"
' sh "$@"
