#!/bin/sh
# Build the helpers that run inside the Android HAL container (bionic,
# Android 14 / API 34) with the Android NDK, and copy them into the
# bramble-android-hal package, which ships them prebuilt (pmbootstrap has
# no NDK).
#
# Usage: tools/android-helpers/build.sh [NDK_DIR]
#   NDK_DIR defaults to $ANDROID_NDK_HOME. Tested with NDK r26c
#   (26.2.11394342).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
ndk=${1:-${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME or pass the NDK directory}}
cxx=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang++
strip=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
out=$here/../../pmaports/device/downstream/bramble-android-hal

flags="-O2 -Wall -Wextra -std=c++17 -static-libstdc++"
$cxx $flags -o "$out/bramble-camera-ndk" "$here/bramble-camera-ndk.cpp" \
	-lcamera2ndk -lmediandk -landroid -llog -ldl
$cxx $flags -o "$out/bramble-camera-proxy" "$here/bramble-camera-proxy.cpp" -lbinder_ndk -ldl
$strip "$out/bramble-camera-ndk" "$out/bramble-camera-proxy"
chmod 644 "$out/bramble-camera-ndk" "$out/bramble-camera-proxy"
echo "[android-helpers] built into $out; run pmbootstrap checksum bramble-android-hal"
