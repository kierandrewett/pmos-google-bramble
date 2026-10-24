#!/bin/sh
# Pixel 4a (5G): keep the compositor and the shell responsive.
#
# A lock screen or greeter frame runs in sequence: phosh/phrog repaints the
# whole window in software (GTK3), phoc copies the SHM buffer into a texture
# and composites on the GPU, then waits for the GPU before the next frame.
# Without help all of it runs on the little CPU cores (no schedtune group is
# mounted, so nothing boosts the UI as on Android) and the GPU stays at its
# lowest clock (275 MHz; msm-adreno-tz never raises it here), which gives
# 20-30 fps. A schedtune boost for phoc, phosh and phrog and a GPU floor of
# 625 MHz gave 48-59 fps on the lock screen and greeter.
#
# Only these three processes are boosted. Apps started from the shell inherit
# its group, so move every other process back to the root group.

stune=/dev/stune
boost=${BOOST:-50}
# kgsl power levels: 0 = 625 MHz, 1 = 500 MHz, 3 = 275 MHz (kernel default)
gpu_min_pwrlevel=${GPU_MIN_PWRLEVEL:-0}

if ! grep -q " $stune cgroup .*schedtune" /proc/mounts; then
	mkdir -p "$stune" && mount -t cgroup -o schedtune schedtune "$stune" || exit 1
fi
mkdir -p "$stune/ui"
echo "$boost" > "$stune/ui/schedtune.boost"
echo "$gpu_min_pwrlevel" > /sys/class/kgsl/kgsl-3d0/min_pwrlevel

# A process moves with all its threads, and new threads stay in the group,
# so each process has to be moved once.
while :; do
	for pid in $(pgrep -x 'phoc|phosh|phrog'); do
		grep -qx "$pid" "$stune/ui/cgroup.procs" ||
			echo "$pid" > "$stune/ui/cgroup.procs" 2> /dev/null
	done
	for pid in $(cat "$stune/ui/cgroup.procs"); do
		case $(cat "/proc/$pid/comm" 2> /dev/null) in
		phoc | phosh | phrog) ;;
		*) echo "$pid" > "$stune/cgroup.procs" 2> /dev/null ;;
		esac
	done
	sleep 2
done
