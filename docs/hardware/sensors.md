# Sensors

The accelerometer, ambient light and proximity sensors sit behind the
Qualcomm sensor core on the ADSP. `bramble-sensors` runs the patched
`hexagonrpcd`, and `iio-sensor-proxy` (patch 0001 in `temp/iio-sensor-proxy`,
built with libssc) serves them on `net.hadess.SensorProxy`. Its OpenRC
service starts after `bramble-sensors`, which must have created
the `adsprpc-smd-secure` node. The udev rule in `bramble-sensors`
(`71-bramble-sensors.rules`) tags that node as `ssc-accel`, `ssc-light`
and `ssc-proximity`.

Checked with `monitor-sensor` inside the Phosh session (it must run in the
session cgroup for the polkit claim): light in lux, proximity near/far,
tilt. Phosh's auto-brightness lowers the backlight at low light. Auto-rotate
and the proximity blank during calls: see [status.md](../status.md).

libssc does not know how the accelerometer is mounted: it reads the
all-zero `placement` entry of the SSC registry and falls back to the
identity matrix. The registry's `lsm6dsr_0_platform.orient` entry (in
`persist`, `sensors/registry/registry/`) says `-x`, `-y`, `+z`, a half
turn about the screen normal. Without it Phosh showed portrait upside
down. `71-bramble-sensors.rules` sets `ACCEL_MOUNT_MATRIX` to the same turn.
