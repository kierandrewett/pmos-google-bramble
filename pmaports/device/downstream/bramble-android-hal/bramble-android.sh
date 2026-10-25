#!/bin/sh
# Android HAL container for google-bramble.
#
# Runs Android 14 init (Halium GSI, package halium-gsi-34) with the phone's
# own vendor partition in private mount/PID/IPC/UTS namespaces, so the
# vendor camera (and fingerprint) HALs work on postmarketOS. Only the vendor
# services listed in /usr/share/bramble-android/vendor-init.list are started:
# the modem, Wi-Fi, audio and the rest stay with the native Linux stack.
#
# Usage: bramble-android start | stop | status | exec CMD [ARG...]
set -eu
export PATH=/usr/sbin:/usr/bin:/sbin:/bin

SHARE=/usr/share/bramble-android
STATE=/var/lib/bramble-android
RUN=/run/bramble-android
ROOT=$RUN/root
GSI=/usr/share/halium/android-rootfs-34.img
VENDOR=/run/msm-firmware-loader/mnt/vendor_a
PERSIST=/run/msm-firmware-loader/mnt/persist
LOG=/var/log/bramble-android.log

log() { echo "[bramble-android] $*" >&2; }

init_pid() {
	[ -s $RUN/init.pid ] || return 1
	p=$(cat $RUN/init.pid)
	kill -0 "$p" 2> /dev/null || return 1
	echo "$p"
}

# A process in the container's default mount namespace (where all APEXes
# are mounted); init itself stays in the bootstrap namespace.
service_pid() {
	p=$(init_pid) || return 1
	ps -o pid,ppid,comm | awk -v p="$p" '$2 == p && $3 == "camera_service" {print $1; exit}'
}

# mknod_like PATH MODE UID GID: recreate host device node PATH in $ROOT/dev.
mknod_like() {
	[ -e "$1" ] || return 0
	t=c
	[ -b "$1" ] && t=b
	dst=$ROOT/dev/${1#/dev/}
	mkdir -p "$(dirname "$dst")"
	mknod -m "$2" "$dst" $t $((0x$(stat -c %t "$1"))) $((0x$(stat -c %T "$1")))
	chown "$3:$4" "$dst"
}

assemble_root() {
	[ -f $VENDOR/build.prop ] || { log "vendor partition not mounted at $VENDOR"; exit 1; }
	mkdir -p $RUN/gsi $RUN/upper $ROOT $STATE/data
	mountpoint -q $RUN/gsi || mount -o loop,ro $GSI $RUN/gsi
	mountpoint -q $RUN/upper || mount -t tmpfs -o mode=0755 tmpfs $RUN/upper
	mkdir -p $RUN/upper/up $RUN/upper/work
	mountpoint -q $ROOT || mount -t overlay overlay \
		-o lowerdir=$RUN/gsi,upperdir=$RUN/upper/up,workdir=$RUN/upper/work $ROOT

	# Vendor partition, read-only. The host mount is noexec,nosuid; this
	# bind must allow both.
	mount --bind $VENDOR $ROOT/vendor
	mount -o remount,bind,ro,exec,suid $ROOT/vendor

	# Curated vendor init directory: the allow-listed stock rc files plus our
	# replacement for init.bramble.rc (the stock one starts the modem stack).
	mount -t tmpfs -o mode=0755 tmpfs $ROOT/vendor/etc/init
	grep -v '^#' $SHARE/vendor-init.list | while read -r rc; do
		[ -n "$rc" ] && cp $VENDOR/etc/init/"$rc" $ROOT/vendor/etc/init/
	done
	install -Dm644 $SHARE/init.bramble.rc $ROOT/vendor/etc/init/hw/init.bramble.rc
	install -m644 $SHARE/bramble.rc $ROOT/system/etc/init/zz-bramble.rc

	# Helpers that run inside the container.
	install -m755 $SHARE/bin/bramble-camera-ndk $SHARE/bin/bramble-camera-proxy $ROOT/system/bin/

	# vold is off here; its client vdc would wait for it forever. Halium's
	# post-data hook waits for a boot HAL (the host runs qbootctl instead).
	for f in vdc on-post-data.sh; do
		printf '#!/system/bin/sh\nexit 0\n' > $ROOT/system/bin/$f
		chmod 0755 $ROOT/system/bin/$f
	done
}

release_root() {
	for m in $ROOT/vendor/etc/init $ROOT/vendor $ROOT $RUN/upper $RUN/gsi; do
		if mountpoint -q $m; then
			umount -l $m
		fi
	done
}

# Release what apexd set up on the host: dm devices and loop devices.
release_apex() {
	dmsetup ls 2> /dev/null | awk '$1 ~ /^com\.android\./ {print $1}' | while read -r d; do
		dmsetup remove --retry "$d" 2> /dev/null || log "could not remove dm device $d"
	done
	losetup -a 2> /dev/null | awk -F: '/\.apex\)/ {print $1}' | while read -r l; do
		losetup -d "$l" 2> /dev/null || log "could not detach $l"
	done
}

# Runs inside the new mount namespace, before Android init: everything
# first stage init would have done.
setup_namespace() {
	mount -t proc -o nosuid,nodev,noexec proc $ROOT/proc
	mount --bind $ROOT/proc/sys $ROOT/proc/sys
	mount -o remount,bind,ro $ROOT/proc/sys
	# init insists on writing these sysctls; give it private copies.
	mkdir -p $RUN/sysctl
	for f in kernel/kptr_restrict vm/mmap_rnd_bits vm/mmap_rnd_compat_bits; do
		c=$RUN/sysctl/$(echo $f | tr / _)
		cat /proc/sys/$f > "$c"
		mount --bind "$c" $ROOT/proc/sys/$f
	done

	# /sys read-only, except what the HALs must write: apexd tunes its
	# loop/dm devices, the FPC HAL drives the sensor's irq/wakeup nodes.
	mount -t sysfs -o ro,nosuid,nodev,noexec sysfs $ROOT/sys
	mkdir -p $RUN/sysrw
	mount -t sysfs -o nosuid,nodev,noexec sysfs $RUN/sysrw
	for d in devices/virtual/block devices/platform/soc/soc:fp_fpc1020; do
		[ -d $RUN/sysrw/$d ] && mount --bind $RUN/sysrw/$d $ROOT/sys/$d
	done
	umount -l $RUN/sysrw

	mount -t tmpfs -o nosuid,mode=0755 tmpfs $ROOT/dev
	for n in null zero full random urandom tty; do mknod_like /dev/$n 0666 0 0; done
	mknod_like /dev/kmsg 0620 0 0
	mknod_like /dev/ashmem 0666 0 0
	mknod_like /dev/ion 0664 1000 1000
	mknod_like /dev/kgsl-3d0 0666 1000 1000
	mknod_like /dev/loop-control 0660 0 6
	# Camera: CAMX nodes, and the rear spectral (flicker) sensor read by
	# rlsservice for auto exposure.
	for n in /dev/video* /dev/media* /dev/v4l-subdev*; do mknod_like "$n" 0660 1000 1006; done
	mknod_like /dev/vd6281 0660 1000 1000
	# Fingerprint: the FPC trusted app is reached through QSEECOM, whose
	# daemon needs the RPMB (UFS SCSI generic) nodes; the HAL reports
	# finger events through uinput.
	mknod_like /dev/qseecom 0660 1000 1026
	mknod_like /dev/smcinvoke 0666 1000 1000
	mknod_like /dev/uinput 0660 1000 1004
	for n in /dev/sg[0-9]*; do mknod_like "$n" 0660 1000 1000; done
	mkdir -p $ROOT/dev/block
	ln -s /sys/devices/platform/soc/1d84000.ufshc $ROOT/dev/block/bootdevice
	# qseecomd keeps the trusted apps' secure storage on the "ssd"
	# partition (libssd looks it up by partition label).
	if ssd=$(readlink -f /dev/disk/by-partlabel/ssd) && [ -b "$ssd" ]; then
		mkdir -p $ROOT/dev/disk/by-partlabel
		mknod -m 0660 $ROOT/dev/disk/by-partlabel/ssd b \
			$((0x$(stat -c %t "$ssd"))) $((0x$(stat -c %T "$ssd")))
		chown 1000:1000 $ROOT/dev/disk/by-partlabel/ssd
	fi
	# apexd: device-mapper and the loop devices (ueventd cannot coldboot
	# them with /sys read-only).
	if [ -e /dev/mapper/control ]; then
		mknod -m 0600 $ROOT/dev/device-mapper c \
			$((0x$(stat -c %t /dev/mapper/control))) $((0x$(stat -c %T /dev/mapper/control)))
	fi
	for l in /dev/loop[0-9]*; do
		case "$l" in *p[0-9]*) continue ;; esac
		[ -b "$l" ] && mknod -m 0600 $ROOT/dev/block/${l#/dev/} b 7 $((0x$(stat -c %T "$l")))
	done
	mkdir -p $ROOT/dev/pts $ROOT/dev/socket $ROOT/dev/binderfs
	mount -t devpts -o newinstance,ptmxmode=0666,mode=0620 devpts $ROOT/dev/pts
	ln -sf pts/ptmx $ROOT/dev/ptmx
	mount -t binder binder $ROOT/dev/binderfs
	chmod 0666 $ROOT/dev/binderfs/binder $ROOT/dev/binderfs/hwbinder $ROOT/dev/binderfs/vndbinder
	for b in binder hwbinder vndbinder; do ln -sf binderfs/$b $ROOT/dev/$b; done

	for d in apex bootstrap-apex linkerconfig debug_ramdisk second_stage_resources mnt; do
		mount -t tmpfs -o nosuid,nodev,mode=0755 tmpfs $ROOT/$d
	done
	# persist is writable on Android: Qualcomm's secure storage keeps files
	# there for the trusted apps, and the fingerprint app needs it to store a
	# new fingerprint (with persist read-only the last enrol step fails with
	# FPC_ERROR_INPUT). msm-firmware-loader mounts it read-only on the host;
	# make the filesystem writable, give the container a writable bind and
	# set the host's mount back to read-only (that is per mount).
	mkdir -p $ROOT/mnt/vendor/persist
	mount -o remount,rw $PERSIST
	mount --bind $PERSIST $ROOT/mnt/vendor/persist
	mount -o remount,bind,ro $PERSIST
	mount --bind $STATE/data $ROOT/data
	# Camera frame socket (bramble-camera-ndk serve), reachable from the
	# host for members of the video group only.
	mkdir -p $RUN/camera $ROOT/mnt/bramble-camera
	chown root:video $RUN/camera
	chmod 0750 $RUN/camera
	mount --bind $RUN/camera $ROOT/mnt/bramble-camera
	# The kernel loads firmware (CAMERA_ICP.elf, the FPC trusted app)
	# relative to the root of the requesting process.
	mkdir -p $ROOT/run/msm-firmware-loader
	mount --rbind /run/msm-firmware-loader $ROOT/run/msm-firmware-loader

	# libselinux treats a selinuxfs in /proc/mounts as "SELinux enabled";
	# with no policy loaded every access check would fail. Drop it in this
	# namespace only.
	umount -l /sys/fs/selinux 2> /dev/null || true
}

start() {
	if init_pid > /dev/null; then
		log "already running"
		return 0
	fi
	stop_quiet
	# This kernel has no LOOP_CONFIGURE: apexd needs free loop devices that
	# already exist.
	for i in $(seq 0 31); do
		[ -b /dev/loop$i ] || mknod -m 0660 /dev/loop$i b 7 $i
		: < /dev/loop$i 2> /dev/null || true
	done
	assemble_root
	unshare --mount --pid --ipc --uts --fork --propagation private \
		"$0" __init > $LOG 2>&1 < /dev/null &
	u=$!
	for _ in $(seq 1 20); do
		p=$(pgrep -P $u || true)
		[ -n "$p" ] && break
		sleep 0.2
	done
	[ -n "$p" ] || { log "init did not start, see $LOG"; exit 1; }
	echo "$p" > $RUN/init.pid
	log "started, init pid $p"
}

# Second half of start, inside the namespaces: becomes Android init.
init_child() {
	setup_namespace
	hostname bramble-android
	# pivot_root, not chroot: init switches between mount namespaces, which
	# resets the root to the namespace root.
	mkdir -p $ROOT/.oldroot
	cd $ROOT
	pivot_root . .oldroot
	exec /.oldroot/lib/ld-musl-aarch64.so.1 --library-path /.oldroot/lib:/.oldroot/usr/lib \
		/.oldroot/usr/bin/setpriv --bounding-set -sys_module,-mac_admin,-mac_override,-sys_time,-sys_boot \
		/system/bin/init second_stage
}

stop_quiet() {
	if p=$(init_pid); then
		kill -KILL "$p" 2> /dev/null || true
		sleep 1
	fi
	rm -f $RUN/init.pid
	release_apex
	release_root
}

case "${1:-}" in
start) start ;;
stop) stop_quiet ;;
status)
	if p=$(init_pid); then
		echo "running (init pid $p)"
	else
		echo "stopped"
		exit 3
	fi
	;;
exec)
	shift
	s=$(service_pid) || true
	[ -n "${s:-}" ] || { log "not running (or cameraserver not up yet)"; exit 1; }
	exec nsenter -t "$s" -m -p /system/bin/env -i \
		PATH=/system/bin:/vendor/bin ANDROID_ROOT=/system ANDROID_DATA=/data "$@"
	;;
__init) init_child ;;
*)
	echo "usage: $0 start | stop | status | exec CMD [ARG...]" >&2
	exit 2
	;;
esac
