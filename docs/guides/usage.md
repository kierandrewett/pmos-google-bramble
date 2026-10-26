# Using the phone

Short how-tos for the parts of this port that need a command or a setting.
What the phone is like in general is in [experience.md](../experience.md).

## Log in and connect

- The first boot takes about a minute. Phosh's login screen appears. Log in
  with the password from the install step, then connect to Wi-Fi from the top
  bar.
- SSH over USB: the phone is `172.16.42.1` while it is in developer mode.
  `ssh <username>@172.16.42.1`, with the username you gave `pmbootstrap init`
  (the examples in these docs write `user`, the pmbootstrap default).
- File transfer: switch the USB mode to MTP (below), then the phone appears
  in your file manager.

### USB modes

`usb-signaller` owns the USB gadget. It takes its modes over D-Bus. As root
on the phone:

```sh
gdbus call --system --dest com.meego.usb_moded --object-path /com/meego/usb_moded \
    --method com.meego.usb_moded.get_modes     # developer_mode, tethering_mode, charging_only, mtp_ffs_mode
gdbus call --system --dest com.meego.usb_moded --object-path /com/meego/usb_moded \
    --method com.meego.usb_moded.set_mode mtp_ffs_mode
```

- The USB network goes away in MTP mode, so an SSH session over USB drops.
  Switch from a shell on Wi-Fi, or from the phone.
- Go back with `set_mode developer_mode`, then run `sudo rc-service adbd
  restart`. The switch replaces the USB gadget, and the new one has no adb
  function until `adbd` adds it again.

## adb and screen mirroring

`adb` and SSH work over the same USB cable. From a Linux PC with the stock
`adb` and `scrcpy`:

```sh
adb devices                  # lists "postmarketOS" over USB
adb shell                    # a shell as the default user
scrcpy -s postmarketOS --no-audio --max-size=1170
```

Things to know:

- scrcpy shows the screen only while the phone's screen is on.
- The phone's adbd has no key authentication. It listens on TCP only on the
  USB network address (`172.16.42.1:5555`). Anyone who can reach that port
  gets a shell, so do not bind it to a Wi-Fi address on a network you do not
  trust. `/etc/conf.d/adbd` explains the setting.
- The scrcpy server encodes in software and uses most of one CPU core while
  it runs.

## Camera

Open "Camera" (GNOME Snapshot). The camera button next to the shutter lists
the back, ultrawide and front cameras. Photos are saved at full resolution
and open in Image Viewer. [camera.md](../hardware/camera.md) has the limits.

## Fingerprint

Enrol a finger once, with the phone unlocked. The unlock service uses the
reader only while the lock screen shows.

```sh
sudo bramble-fingerprint enroll    # touch the reader on the back about 8 times
sudo bramble-fingerprint verify    # touch an enrolled finger: "verify OK"
sudo bramble-fingerprint list
sudo bramble-fingerprint remove --fid <id from list>
```

- The reader is the round sensor on the back, below the camera.
- Lift the finger fully between touches and move it a little each time.
- Each command takes over the reader. Do not start a second one while
  `enroll` waits for touches: that cancels the enrol.
- The PIN keeps working. After 5 wrong fingers the reader stops for 30
  seconds. After 20 in one lock, only the PIN unlocks.
- Back up `persist` before the first enrol, because enrolment writes to it
  ([recovery.md](recovery.md)).

## Services you can turn off

All of these are OpenRC services, on by default.

| What | Stop it now | Keep it off |
|---|---|---|
| Fingerprint unlock | `sudo rc-service bramble-fingerprint-unlock stop` | `sudo rc-update del bramble-fingerprint-unlock default` |
| UI boost (smoother animations, uses more power) | `sudo rc-service bramble-ui-boost stop` | `sudo rc-update del bramble-ui-boost default` |
| adb | `sudo rc-service adbd stop` | `sudo rc-update del adbd default` |

Tap to wake is a kernel setting. In `/sys/bus/spi/devices/spi0.0/`, write 0
to `double_tap_wake` to turn it off, or 2 to `tap_wake_taps` to need a double
tap.

## Settings that matter

- Audio: PipeWire is recommended. It moves playback to headphones when you
  plug them in. PulseAudio does not.
- Location: apps get a position only when Location is on in Settings. It is
  off by default.
- Mobile data: with a roaming SIM it can take about 2.5 minutes to come up
  after boot.

## Updating

These packages are not in the postmarketOS binary repository. Rebuild them
with pmbootstrap and install them with `pmbootstrap sideload <package>`. A
new kernel also has to be flashed again. See
[development.md](development.md).
