#!/bin/sh
# Take photos with the Pixel 4a (5G) cameras through the vendor camera HAL
# in the Android HAL container (service bramble-android). Needs root.
#
#   bramble-camera list
#   bramble-camera photo [back|wide|front] [FILE]
#
# back is the main rear camera, wide the rear ultrawide (the rear logical
# camera at its widest zoom), front the selfie camera. FILE defaults to
# IMG_<date>.jpg in the current directory.
set -eu
export PATH=/usr/sbin:/usr/bin:/sbin:/bin

ctl=/usr/libexec/bramble-android
[ "$(id -u)" = 0 ] || { echo "bramble-camera: run as root (sudo/doas)" >&2; exit 1; }

case "${1:-}" in
list)
	exec $ctl exec /system/bin/bramble-camera-ndk list
	;;
photo)
	which=${2:-back}
	out=${3:-IMG_$(date +%Y%m%d_%H%M%S).jpg}
	case "$which" in
	back) id=0 zoom=1.0 ;;
	wide) id=0 zoom=-1 ;;
	front) id=1 zoom=1.0 ;;
	*) echo "bramble-camera: unknown camera '$which' (back, wide, front)" >&2; exit 2 ;;
	esac
	tmp=bramble-camera-$$.jpg
	mkdir -p /var/lib/bramble-android/data/local/tmp
	$ctl exec /system/bin/bramble-camera-ndk still $id /data/local/tmp/$tmp $zoom
	mv /var/lib/bramble-android/data/local/tmp/$tmp "$out"
	# Hand the photo to the user who called sudo/doas.
	owner=${SUDO_USER:-${DOAS_USER:-}}
	[ -n "$owner" ] && chown "$owner:" "$out"
	echo "$out"
	;;
*)
	echo "usage: bramble-camera list | photo [back|wide|front] [FILE]" >&2
	exit 2
	;;
esac
