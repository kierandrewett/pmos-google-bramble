# Input

- Touch: STMicro FTS (`fts_touch`), on SPI. Double tap to wake: on Android
  the touch bus goes to the ADSP while the screen is off, and the ADSP
  detects taps. The controller's own gesture mode does not report a double
  tap with this firmware (no interrupt, panel on or off), but normal touch
  reporting keeps working with the panel powered off. Patch 0009 adds a
  `double_tap_wake` sysfs switch (set by the device udev rule): while the
  screen is off the controller keeps scanning (mutual sense only), its
  interrupt stays armed as a wakeup source (GPIO 9 has a PDC wake line),
  and the driver finds a double tap in the touch events and reports
  `KEY_POWER`, which Phosh uses to turn the screen on. Patch 0021 adds
  `tap_wake_taps` (1 or 2) next to it, and the udev rule sets 1, so a
  single tap wakes the screen. A tap is one finger, at most 400 ms and
  60 px of movement, so swipes and long touches do not wake it.
- Haptics: TI DRV2624. Patch 0006 registers it as a force-feedback input
  device (`drv2624-haptics`, `FF_RUMBLE`), which feedbackd uses.
- Torch: Qualcomm flash LED (PM8150L). Phosh picks the first LED named
  `*:torch*` or `*:flash*` and only writes its brightness. Patch 0007
  renames the camera flash nodes to `led:strobe_N`, so `led:torch_0` is
  found first, and makes a torch brightness write enable the switch
  dedicated to that LED.

# USB

`usb-signaller` (pmOS) switches the USB gadget mode: developer mode (NCM
networking, SSH on 172.16.42.1), tethering, MTP (`umtprd`) or charging only.
MTP needs `CONFIG_POSIX_MQUEUE` (umtprd uses POSIX message queues).

adb: `temp/adbd` builds AOSP adbd as a standalone Linux daemon
(happyme531/standalone-linux-adbd on the android-tools 36.0.1 sources).
Its OpenRC service mounts a FunctionFS instance, lets adbd write the
descriptors, then links the `ffs.adb` function into the configfs gadget
next to `ncm.usb0` and rebinds the UDC, which removes `usb0` for a moment;
the service puts the 172.16.42.1 address back and restarts the DHCP
server. `adb shell` runs as the default user with the `input` group.
There is no key authentication, so TCP 5555 is bound to the USB network
address only.

The gadget is called `g1` at boot. `usb-signaller` replaces it with a new
one (`usb-signaller-developer`, and so on) at every mode switch, which has
no adb function, so the init script finds the gadget by looking in
configfs instead of using a fixed name, and a mode switch needs
`rc-service adbd restart` to bring adb back. `usb-signaller` does not know
the adb function, so it reports `unknown_mode` at boot.

scrcpy: the PC client pushes its Java server and runs it with
`app_process`. `temp/scrcpy-server` installs `/usr/bin/app_process` as a
shim that starts a native server instead. It speaks the scrcpy 4.x
protocol, captures phoc's output with wlr-screencopy (so it only sends
frames while the screen is on), encodes H.264 with x264 and injects touch
and keys through a uinput device.
