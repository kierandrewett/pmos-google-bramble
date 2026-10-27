# Packages

These are the packages that the port adds to, or changes in,
[pmaports](https://gitlab.postmarketos.org/postmarketOS/pmaports). The
directory layout is the same as pmaports, so `scripts/setup-pmaports.sh`
can copy them into a checkout. Build and test them as described in
[docs/guides/development.md](../docs/guides/development.md).

## Device (`device/downstream/`)

| Package | What it does | Docs |
|---|---|---|
| `device-google-bramble` | Device package: deviceinfo, udev rules, ALSA UCM, OpenRC services (modem, WLAN firmware data, carrier configuration, firmware, GNSS, UI boost), phoc scale, the DRM format shim. `-pipewire` has the WirePlumber config and the speaker limiter. | [display-gpu](../docs/hardware/display-gpu.md), [audio](../docs/hardware/audio.md), [modem](../docs/hardware/modem.md), [wifi](../docs/hardware/wifi.md) |
| `linux-google-bramble` | The 4.19 downstream kernel: config and patches `0001`-`0026`. | [boot](../docs/hardware/boot.md), [input-usb](../docs/hardware/input-usb.md), [kernel-fixes](../docs/hardware/kernel-fixes.md), [modem](../docs/hardware/modem.md) |
| `bramble-android-hal` | The Android HAL container (camera and fingerprint HALs, and `bramble-acdb`, which loads the voice call calibration) and the PipeWire camera bridge. | [android-hal](../docs/hardware/android-hal.md), [camera](../docs/hardware/camera.md), [modem](../docs/hardware/modem.md) |
| `bramble-fingerprint` | Fingerprint enrol, verify, list, remove, and the service that unlocks the lock screen. | [fingerprint](../docs/hardware/fingerprint.md) |
| `bramble-sensors` | Sensor DSP service, udev tags for iio-sensor-proxy, `bramble-sensors-demo`. | [sensors](../docs/hardware/sensors.md) |
| `bramble-voice` | `bramble-voiced`: sets up the audio DSP voice path while ModemManager has a call. | [modem](../docs/hardware/modem.md) |

## Main (`main/`)

Packages that fit pmaports `main`, with no change to an existing Alpine
package.

| Package | What it does |
|---|---|
| `halium-gsi-34` | Halium 14 Android system image for the HAL container. |
| `mesa-vulkan-freedreno-kgsl` | Mesa turnip (Vulkan) for the downstream KGSL GPU driver. |
| `pd-mapper`, `qrtr-ns` | Qualcomm protection domain mapper and QRTR name service, for kernels without the in-kernel versions. |

## Temporary forks (`temp/`)

Copies of Alpine packages with bramble patches, or packages that Alpine
does not have yet. Drop each one when its change is upstream.

| Package | Change |
|---|---|
| `adbd`, `scrcpy-server` | adb daemon (USB and TCP) and a native scrcpy server, so the stock `adb` and `scrcpy` work from a PC. |
| `snapshot` | GNOME Snapshot with full-resolution photos taken through the camera HAL. |
| `phoc` | Screen fade on power-save, and a crash fix. Phoc and Phosh are released together: rebase it when Alpine moves Phosh. |
| `gmobile` | Panel data for the Pixel 4a (5G): rounded corners and punch-hole, so Phosh keeps the status bar clear. |
| `iio-sensor-proxy` | SSC (Qualcomm sensor core) backend through libssc. |
| `hexagonrpcd` | Downstream adsprpc support (sensors). |
| `tqftpserv` | Falls back to the firmware path on kernels without remoteproc (modem firmware over QRTR). |
| `bubblewrap` | Alpine's build drops the fallbacks for kernels older than 6.18. This build keeps them, so Image Viewer's sandbox works on 4.19. |
