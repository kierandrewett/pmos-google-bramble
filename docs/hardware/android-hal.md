# Android HAL container

The cameras and the fingerprint reader only work through Google's closed
vendor HALs (camera: CAMX kernel drivers plus the vendor camera provider;
fingerprint: the FPC HAL with its trusted app in QSEE). `bramble-android-hal`
runs them without libhybris: Android 14 init from the Halium GSI
(`halium-gsi-34`) starts in private mount, PID, IPC and UTS namespaces
with the phone's own vendor partition, and only the vendor services
listed in `vendor-init.list` start. The modem, Wi-Fi, audio and the rest
stay with the native Linux stack. `bramble-android.sh` and `bramble.rc`
explain each workaround (no SELinux policy, APEX loop devices, stubbed
`vdc`, a stand-in for `system_server`'s camera proxy).

Kernel: `CONFIG_PID_NS` is on for the container's own PID 1, and 0023
lets the container mount its own procfs (the superblock is looked up in
the user namespace that owns the PID namespace, as in mainline).

Camera apps: `bramble-camera-ndk serve` runs inside the container and
streams NV12 frames from the NDK camera API over a host socket
(`/run/bramble-android/camera/camera.sock`). `bramble-camera-pipewire`
(started in each Phosh session) turns each camera into a PipeWire video
source node, which GNOME Snapshot uses through the camera portal. Photos
are HAL stills from a JPEG output in the same capture session, which the
patched Snapshot (`temp/snapshot`) asks the bridge for. See
[camera.md](camera.md) for the design and the problems found on the way
(a blocked HAL callback aborts the camera provider; a malformed size
choice stalls Snapshot).
