#!/bin/sh
# ==============================================================================
# /etc/profile.d/bramble-display.sh
# System-wide display environment for Google Pixel 4a (5G) (bramble)
# ==============================================================================

# Preload DRM format & presentation clock fix + faccessat emulation
export LD_PRELOAD=/usr/lib/libfix_drm_formats.so

# Force DRM device and avoid hardware cursor crash on MSM
export WLR_DRM_DEVICES=/dev/dri/card0
export WLR_NO_HARDWARE_CURSORS=1

# Use Pixman CPU software rasterizer (MSM KMS + Pixman at 60 FPS)
export WLR_RENDERER=pixman

# Seat & Input management
export SEATD_VTBOUND=0
export WLR_LIBINPUT_NO_DEVICES=1

# Standard temp directories
export TMPDIR=/tmp
export TEMP=/tmp
export TMP=/tmp

# Wayland defaults
export GDK_BACKEND=wayland
export NO_AT_BRIDGE=1
export MOZ_ENABLE_WAYLAND=1
export QT_QPA_PLATFORM=wayland

# Auto-mount POSIX shared memory if missing
if [ ! -d /dev/shm ] || ! mountpoint -q /dev/shm; then
    mkdir -p /dev/shm
    mount -t tmpfs tmpfs /dev/shm -o rw,nosuid,nodev,mode=1777 2>/dev/null || true
fi
