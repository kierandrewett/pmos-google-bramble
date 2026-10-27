# Hardware notes

Technical notes on how each part of the Pixel 4a (5G) works on this port,
and why it is done this way. Read the page for a part before you change a
package that touches it. What has been tested, and how, is in
[status.md](../status.md).

The port runs the downstream vendor kernel: Linux 4.19.325 from LineageOS
[`android_kernel_google_redbull`](https://github.com/LineageOS/android_kernel_google_redbull)
(commit `2af5306961aebce4cc30098c9f12a09f8d906906`, with CIP 4.19 SLTS
fixes), built with clang 16. Mainline Linux has no SM7250 support yet (the
first base SoC series boots only Qualcomm's reference board to a serial
console), so every driver here is the Qualcomm/Google downstream one.

| Page | Contents |
|---|---|
| [boot.md](boot.md) | Partitions, boot chain, device tree. |
| [display-gpu.md](display-gpu.md) | Display panel, Adreno GPU through KGSL, Mesa turnip and zink, animation speed. |
| [wifi.md](wifi.md) | Wi-Fi (WCN3991 and the modem's WLAN firmware). |
| [modem.md](modem.md) | Mobile data, SMS, voice calls (VoLTE) and GPS through ModemManager. |
| [audio.md](audio.md) | Speakers, microphone, headphones, PipeWire and PulseAudio. |
| [bluetooth.md](bluetooth.md) | Bluetooth controller and audio. |
| [input-usb.md](input-usb.md) | Touch, tap to wake, haptics, torch, USB gadget modes, adb and scrcpy. |
| [android-hal.md](android-hal.md) | The Android HAL container that runs the camera and fingerprint HALs. |
| [camera.md](camera.md) | Cameras: vendor HAL, PipeWire bridge, Snapshot. |
| [sensors.md](sensors.md) | Accelerometer, light and proximity sensors. |
| [fingerprint.md](fingerprint.md) | Fingerprint reader: enrol, verify and unlock. |
| [kernel-fixes.md](kernel-fixes.md) | Small kernel fixes that other software needed (polkit, Image Viewer). |
| [gnome-shell.md](gnome-shell.md) | Experiment: stock GNOME Shell instead of Phosh. |
