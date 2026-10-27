# What it is like to use

This page describes day-to-day use of the phone with this port, as found
on the test phone. Each statement comes from a check listed in
[status.md](status.md). Where something was not checked, the page says so.

The shell is Phosh from postmarketOS edge on Linux 4.19. It is a normal
Linux phone: apps are Linux apps, and the Android vendor parts run only in
a private container, for the cameras and the fingerprint reader.

## Start and unlock

- The phone boots to the Phosh greeter (phrog). Enter the user's password
  or PIN on the keypad.
- The lock screen has the usual Phosh clock and notifications. The PIN
  always works.
- A finger on the round reader below the camera unlocks it and turns the
  screen on. The service for this is built and was tested against a
  stand-in for the reader. Unlock with a real finger on the locked phone has
  not been seen yet ([fingerprint notes](hardware/fingerprint.md)).
- A tap on the screen wakes it. A quick second tap does not turn it off
  again. Swipes and long touches do not wake it.
- The power button fades the screen out and in (250 ms).

## Screen and home screen

- 1080x2340 OLED at 60 Hz. The shell runs at scale 3.0, a 360x780 logical
  screen, so the app grid has 4 columns.
- The status bar keeps clear of the punch-hole camera and the rounded
  corners: the first icon starts about 10 logical pixels after the hole.
- Rotation follows the phone (accelerometer through iio-sensor-proxy).
  Auto-brightness, when switched on, lowers the backlight in dim light.
- Animations: the lock screen runs at 47-55 fps, the greeter at 55-60 fps
  and the app grid at 57-60 fps while you drag. That needs the
  `bramble-ui-boost` service, which is on by default. Without it the lock
  screen runs at 20-30 fps.
- Home favourites: Calls, Chatty, Contacts and Firefox. The app grid also
  has Audio Player, Calculator, Calendar, Camera, Clocks, Console,
  Document Viewer, eSIM Manager, Files, Gapless, Image Viewer, Maps,
  Settings, Software, Text Editor, Video Player and Weather.

## Camera

- Camera is GNOME Snapshot. The viewfinder works on all three cameras (main,
  ultrawide and front). Switch between them from the menu next to the
  shutter.
- Photos are 4032x3024 from the main and ultrawide cameras and 3264x2448
  from the front camera. They are upright, and the front camera is not
  mirrored in the saved photo.
- Video: 1600x1200 at about 29 fps with sound.
- Photos open in Image Viewer.
- Not there: flash, HDR+ and night mode. Photo quality is that of the
  vendor camera HAL, with the vendor's still processing, but without the
  Google Camera app's extra software. See [camera notes](hardware/camera.md).

## Calls, messages and data

- Mobile data over LTE works, and reconnects after Wi-Fi is switched off and
  on. With a roaming SIM, it can take about 2.5 minutes to come up after boot.
- SMS is sent over VoLTE (checked with `mmcli`), and the owner confirmed
  by hand that SMS works. Chatty has not been tested on its own.
- Voice calls over VoLTE connect and carry audio both ways on the earpiece
  (outgoing, tested to two mobile numbers), and the owner confirmed by hand
  that calls work. The speaker button has not been tested on its own. RCS
  chat features are not supported.
- GPS gets a fix and geoclue reports it. Apps get a position only when
  Location is on in Settings (off by default).

## Connectivity

- Wi-Fi works, and wakes the phone on incoming traffic.
- Bluetooth pairs, connects and plays audio (A2DP).
- USB: SSH over USB (`ssh user@172.16.42.1`) and MTP file transfer.
- `adb` over USB works beside SSH. Stock `scrcpy` from a PC shows the
  screen (about 24 fps over USB) and takes mouse clicks as touches. The
  screen shows only while the phone's screen is on.

## Sound

- Speakers, microphone and headphones work. With PipeWire, playback moves to
  headphones when you plug them in and back to the speaker when you unplug
  them. With PulseAudio, it does not.
- A speaker limiter makes the speaker 4-6 dB louder from 40 % volume.
- Vibration works (feedbackd) and the torch is a quick setting.

## Power

- Suspend is not verified with the USB cable unplugged. With a cable in, it
  cannot work: the USB controller blocks it.
- The touch controller keeps scanning while the screen is off, so that tap
  to wake works. A phone in a pocket uses a little more power, and a tap in a
  pocket can turn the screen on.
- Battery life has not been measured.

## Rough edges

The full list is in [status.md](status.md). The ones you notice most:

- The speaker button in Calls is not tested on its own.
- No Google Camera features: flash, HDR+, night mode.
- Some sites load slowly or not at all on the data-only test SIM. The large
  packets never arrive; the cause is most likely the SIM's network.
- GNOME Calls' dial pad drops digits when typing fast.
- The port is not in the postmarketOS repositories. Updates come from this
  repository, built with pmbootstrap ([install guide](guides/install.md)).
