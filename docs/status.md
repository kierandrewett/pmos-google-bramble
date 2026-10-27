# Status

Tested on a Pixel 4a (5G) on the final stock firmware (Android 14,
`UP1A.231105.001.B2`), with Phosh from postmarketOS edge. "How" says what
was checked; the technical background is in
the [hardware notes](hardware/README.md).

## Works

| Feature | How it was checked | Notes |
|---|---|---|
| Display, touch, Phosh | Daily use. Scale 3.0 checked on the phone: the right edge matches the wallpaper, the app grid has 4 columns, and the first status icon starts 30 px after the punch-hole | 60 Hz panel. phoc scale 3.0 (a 360x780 logical screen): 2.75 left two columns on the right edge unpainted, which showed as a thin line. The status bar keeps clear of the punch-hole through `temp/gmobile`. |
| GPU acceleration | Phosh renders with Mesa turnip and zink on KGSL | See [hardware/display-gpu.md](hardware/display-gpu.md). |
| Wi-Fi | Daily use, SSH over Wi-Fi; switched off and on from Settings; after four modem restarts (a carrier configuration change each time) Wi-Fi was back in about 9 s and NetworkManager reconnected without help | Wakes the phone on incoming traffic (WoWLAN). Kernel patch 0018 makes polkit work, which the Wi-Fi switch in Settings needs. `bramble-wlfw` sends the board data to the firmware each time it starts. |
| Mobile data (LTE) | Two SIMs: a data-only roaming IoT SIM on a UK network, and a Smarty SIM (Three UK, home network). IoT SIM: NetworkManager connects at boot; with Wi-Fi off, ping, DNS and an HTTPS download over LTE; Wi-Fi off and on keeps it working; the NetworkManager WWAN switch (the Phosh mobile data toggle) off and on reconnects. Smarty SIM: ping (36 ms) and an HTTPS request (58 KB in 1 s) over the LTE interface, with Wi-Fi on | ModemManager with the kernel rmnet driver. See [hardware/modem.md](hardware/modem.md). |
| SMS | Sent over VoLTE (SMS over IMS) from the phone to two UK mobile numbers with `mmcli`; the Smarty SIM is on Three, which has no circuit-switched fallback, so this needs the whole VoLTE stack (see [hardware/modem.md](hardware/modem.md)) | The owner also confirmed by hand that SMS works. Not tested separately: receiving, Chatty. MMS is not tested (`mmsd-tng` and Chatty are installed and run). RCS is not supported. |
| Voice calls (VoLTE) | Outgoing calls to two UK mobile numbers, started with `mmcli` and from the Calls app: the call rings and is answered, and the person called and the person holding the phone hear each other on the earpiece route | `81voltd`, kernel patch 0026, the modem's carrier configuration (`bramble-mcfg`) and the voice audio path (`bramble-voiced`, `bramble-acdb`), all started by OpenRC. The owner also confirmed by hand that calls work. Not tested separately: the speaker route, the headset, the call volume and Bluetooth audio. |
| Speakers, internal microphone | Playback and recording through PulseAudio and PipeWire; a test tone heard in a Phosh session; speaker level measured with a microphone | Speaker protection DSP is bypassed; amp gain and volume are the stock values. With PipeWire, a speaker-only limiter makes the speaker 4-6 dB louder from 40 % volume up (device-google-bramble-pipewire). |
| Headphones, with automatic switching (PipeWire) | Plugging in and unplugging headphones moves playback between headphones and speaker without user action; tone heard in each | With PulseAudio the ports are detected but playback does not switch, see "Audio server" below. |
| Battery and charging | Charge level and charging state in Phosh | |
| USB networking (SSH) and MTP | `ssh user@172.16.42.1`; files copied over MTP. With `adbd` running, `set_mode mtp_ffs_mode` made the PC list the phone's storage ("Home") and `set_mode developer_mode` brought SSH over USB back | Mode is chosen with `usb-signaller` over D-Bus, see [usage.md](guides/usage.md). A mode switch drops adb: see Known problems. |
| Vibration | `fbcli -E phone-incoming-call` vibrates | feedbackd uses the DRV2624 force-feedback device. |
| Torch | Phosh quick setting turns the LED on and off | Shows "off" at boot. |
| Tap to wake | With the screen off, a tap turns it on, and a quick second tap does not turn it off again (checked by hand on kernel r35: the screen stays on, and the kernel log has one "waking the screen" per wake) | The touch controller keeps scanning while the screen is off. One tap is enough since patch 0021; `tap_wake_taps` selects 1 or 2 taps. Patch 0025 sends the power key once per screen off, because Phosh treats it as a toggle. |
| Screen fade | Power button: the screen fades out and in (filmed with a camera, checked by hand) | Patched phoc in `temp/phoc`; upstream Phosh switches instantly. |
| Bluetooth | Scans LE and BR/EDR devices. In Settings, with a Linux PC as the other device: paired (the codes on both sides matched), connected, switched off and on (the phone reconnected), forgot the device. A2DP audio from a Phosh session recorded on the PC (660 Hz at -23 dBFS received at -23.0 dBFS). The PC sees the phone under its own address | WCN3991 with the in-kernel hci_qca driver (patches 0008, 0010, 0017, 0020, 0022). Each phone gets its own address from its serial number (bootmac). The controller does not use in-band sleep, so it uses some power while Bluetooth is on. Playback moves to a Bluetooth device by itself only after it was chosen once as the output, for example in Settings, Sound: WirePlumber keeps a chosen output and then switches to it on every connect. |
| GPS | Indoors at night: ModemManager gets NMEA with 11-13 satellites in view, and a position fix after about 11 s in one test; geoclue reports "GPS GGA" positions | The modem's GNSS engine through ModemManager's QMI location support. `bramble-gnss` undoes an engine lock that ModemManager sets and that blocks it on this firmware, and gives the engine the time once the clock is synced. Apps get a position only when Location is switched on in Settings (it is off by default). |
| Cameras (back, ultrawide, front) | In Camera (Snapshot): live viewfinder from all three, switched seven times in a row; a photo from each, saved at 4032x3024 (back, ultrawide) and 3264x2448 (front), upright, front not mirrored; a 17 s video at 1600x1200, 29 fps, with sound; a photo opened in Image Viewer | Vendor camera HAL in the Android HAL container, `bramble-camera-pipewire` for apps, and a Snapshot patch (`temp/snapshot`) that takes photos through the HAL. See [hardware/camera.md](hardware/camera.md). No flash, no HDR+ or night mode. |
| adb and screen mirroring | From a Linux PC: `adb devices` lists the phone over USB, `adb connect 172.16.42.1:5555` works over the USB network, `adb shell` gives the default user; SSH over USB keeps working beside adb. Stock `scrcpy` 4.1 mirrors the screen (540x1170 H.264, about 24 fps over USB) and a mouse click in its window taps the phone | `temp/adbd` (AOSP adbd, standalone Linux build) and `temp/scrcpy-server` (native server behind an `app_process` shim). adbd has no key authentication, so TCP listens on the USB network address only. The server encodes in software and uses most of one CPU core while mirroring. |

## Partly works

| Feature | State |
|---|---|
| Audio server | PipeWire (answer `pipewire` for `postmarketos-base-ui-audio-backend` in `pmbootstrap init`) is the recommended backend: it plays, records and switches to headphones. PulseAudio (the postmarketOS default) plays and records, but does not move audio to headphones on plug-in: callaudiod unloads `module-switch-on-port-available` and only switches ports inside one sink, while the UCM profile puts speaker and headphones in separate profiles. |
| Suspend | Not verified with USB unplugged yet. While USB power is connected, suspend cannot work: the USB controller blocks it while the gadget is connected ("USB is outside LPM"; it passes with the controller forced off through `/sys/bus/platform/devices/a600000.ssusb/mode`), and the Google charger driver re-arms an alarm every 2 s while charging, so `alarmtimer` refuses suspend. With Bluetooth on, the Bluetooth UART used to block suspend (`qup_uart` failed to suspend noirq); patch 0019 is meant to fix that and is not verified yet either. |
| Sensors (accelerometer, light, proximity) | Through iio-sensor-proxy over libssc (SSC backend, patch 0001 in `temp/iio-sensor-proxy`; udev tags from `bramble-sensors`). `monitor-sensor` reports light in lux, proximity near/far and the tilt. Phosh auto-brightness, when its setting is on, lowers the backlight at about 187 lux (513 to 127 in the test). Auto-rotate (orientation lock off) was checked by hand after `bramble-sensors` r2 turned the accelerometer matrix by 180 degrees: the screen follows the phone. The proximity blank during calls is not verified, because it needs a call (voice calls are untested). Tap to wake does not use proximity. |
| Fingerprint reader | Through the fingerprint HAL in the Android container, with the `bramble-fingerprint` client: enrol completes (8 touches), verify matches the enrolled finger and rejects other touches, and the finger is still enrolled after the container restarts. `bramble-fingerprint-unlock` unlocks the lock screen on a match and turns the screen on; it was run on the real Phosh session against a stand-in for the HAL (screen on and off, 5 and 20 wrong fingers, unlock by PIN). A real finger on the locked phone is not yet seen. Whether a touch wakes the phone from suspend is not verified. See [hardware/fingerprint.md](hardware/fingerprint.md). |

## Does not work

| Feature | Why |
|---|---|
| RCS | Not supported. UK carriers use Google's Jibe, and registering needs Google Play services (a DroidGuard check) that this stack cannot give. |

## Known problems

- Tap to wake keeps the touch controller scanning while the screen is
  off. Every touch wakes the CPU briefly, so a phone in a pocket uses a
  little more power, and with no proximity sensor a single tap in a
  pocket can turn the screen on. For the double tap, write 2 to
  `/sys/bus/spi/devices/spi0.0/tap_wake_taps`; to turn tap to wake off,
  write 0 to `double_tap_wake` in the same directory.
- With the roaming test SIM, mobile data can take about 2.5 minutes to
  come up after boot or after the mobile data toggle: the modem first
  tries networks that reject the SIM (2G and another operator) before it
  finds LTE.
  NetworkManager then connects without help.
- The flash LED's camera nodes are renamed to `led:strobe_N`; nothing in
  userspace uses them.
- GPS assistance data (XTRA) in the modem is valid until about
  2026-10-29. It is not yet known whether the modem refreshes it by
  itself; without it, the first fix after a long time off takes longer.
- `bramble-gnss` sets the GNSS engine lock to none. ModemManager's own
  setting (`mt`) was meant to lock only network-initiated location
  sessions, but on this firmware it locks ModemManager out too, so
  network-initiated sessions are not locked either.
- Switching Bluetooth off in Settings stops the radio without first
  disconnecting (rfkill closes the controller at once), so a connected
  device notices only after its link timeout: 20 s for the PC used in
  testing. After switching on again, the kernel logs a few "ACL packet
  for unknown connection handle" lines; the phone then reconnects.
- Animation: with `bramble-ui-boost` running (default), the lock screen
  animates at 47-55 fps, the greeter at 55-60 fps and the app grid at
  57-60 fps while dragging. Without it the lock screen and greeter run at
  20-30 fps. The service boosts phoc, phosh and phrog on the CPU and keeps
  the GPU at 625 MHz or more; it costs some power while the UI draws (not
  measured yet). Turn it off with `sudo rc-service bramble-ui-boost stop`
  and `sudo rc-update del bramble-ui-boost default`. The rest of the gap to
  60 fps on the lock screen is in phoc and GTK3 (full repaint per frame).
- GNOME Calls' dial pad drops digits when typing fast. The touch stack
  delivers every tap (checked against the kernel events, and a plain GTK
  keypad registers all of them), so this is a Calls/libcall-ui issue.
- The data-only IoT test SIM drops bursts of full-size packets,
  so some sites load slowly or not at all (DuckDuckGo). Captures on the
  phone show that the large packets never arrive; the cause is most
  likely the SIM's network, not the port.
- `usb-signaller` replaces the USB gadget at every mode switch, and the new
  gadget has no adb function, so after a switch adb is gone until
  `sudo rc-service adbd restart` (checked: after developer mode, `adb
  devices` was empty; after the restart it listed the phone, and SSH over
  USB kept working). `usb-signaller` also does not know the adb function:
  at boot it reports `unknown_mode`, and started by hand it prints "Cannot
  determine mode for gadget function ffs.adb". Switching modes still works.
