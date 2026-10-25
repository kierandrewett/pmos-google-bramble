# postmarketOS for the Google Pixel 4a (5G) (bramble)

A [postmarketOS](https://postmarketos.org) port for the Pixel 4a (5G),
codename `google-bramble` (Snapdragon 765G, SM7250). It runs Phosh on the
downstream Linux 4.19 vendor kernel, with GPU acceleration, Wi-Fi, mobile
data, audio, all three cameras and the fingerprint reader.

This is a community port, not part of the official postmarketOS
repositories. Expect rough edges. It is not the Pixel 4a (`sunfish`).

## What it is like to use

You get a Linux phone with the Phosh shell. The lock screen takes a PIN, or
a finger on the reader. A tap wakes the screen. The home screen has a
4-column app grid, and the screen turns with the phone. Camera (GNOME
Snapshot) uses the main, ultrawide and front cameras for photos at full
resolution and for video, and the photos open in Image Viewer. Mobile data
(LTE), SMS, GPS, Wi-Fi and Bluetooth work. A PC sees the phone over USB as
an SSH host, an MTP drive and an adb device, and the stock `scrcpy` mirrors
its screen.

The full walk-through, with the numbers and the limits, is in
[docs/experience.md](docs/experience.md).

## Status

| Works | Partly | Not tested |
|---|---|---|
| Display, touch, Phosh, auto-rotate, auto-brightness | Suspend: works in principle, not verified with USB unplugged | Voice calls (the test SIM is data only) |
| GPU acceleration (60 fps) | Fingerprint: enrol and verify work; unlock with a real finger is not yet seen | Proximity blank during calls |
| Cameras: main, ultrawide, front; photos and video | | |
| Wi-Fi, Bluetooth | | |
| Mobile data (LTE), SMS, GPS | | |
| Speakers, microphone, headphones | | |
| Battery and charging | | |
| USB networking (SSH), MTP, adb, scrcpy | | |
| Vibration, torch, tap to wake | | |

How each item was checked, and the known problems: [docs/status.md](docs/status.md).

## Install

Follow [docs/guides/install.md](docs/guides/install.md). In short:

```sh
git clone https://github.com/kierandrewett/pmos-google-bramble.git
cd pmos-google-bramble
scripts/setup-pmaports.sh
pmbootstrap --aports "$PWD/build/pmaports" init   # answers: docs/guides/install.md
pmbootstrap install
# phone in fastboot (Power + Volume Down):
pmbootstrap flasher flash_vbmeta
pmbootstrap flasher flash_kernel
pmbootstrap flasher flash_vendorboot
pmbootstrap flasher flash_rootfs
```

The phone must be on the final stock firmware (Android 14,
`UP1A.231105.001.B2`) with the stock `dtbo`, and its bootloader must be
unlocked. Installing wipes the phone.

To go back to Android: [docs/guides/recovery.md](docs/guides/recovery.md).

## Repository layout

```
pmaports/     the packages, in pmaports layout (the source of truth)
  device/downstream/   device, kernel, Android HAL container, fingerprint, sensors
  main/                packages that fit pmaports main
  temp/                forks of Alpine packages, with bramble patches
docs/         documentation (index: docs/README.md)
  guides/              install, recovery, development
  hardware/            how each part works; experiments
scripts/      setup-pmaports.sh, CI checks (ci.sh)
tools/        helpers that are not packages: Android NDK camera helpers, GNOME Shell probe
build/        generated (git-ignored): the pmaports checkout and CI work
```

This is an overlay for pmaports: `pmaports/` holds only the bramble
packages, with their full history. `scripts/setup-pmaports.sh` puts them
into a pmaports checkout (pinned to the tested commit) in `build/pmaports`.
The package list is in [pmaports/README.md](pmaports/README.md); how to
change and test them is in [docs/guides/development.md](docs/guides/development.md).

## Documentation

Start at [docs/README.md](docs/README.md).

| Document | Contents |
|---|---|
| [experience.md](docs/experience.md) | What daily use is like. |
| [status.md](docs/status.md) | What works, what does not, known problems. |
| [guides/install.md](docs/guides/install.md) | Prepare the phone, build, flash. |
| [guides/recovery.md](docs/guides/recovery.md) | Backups, unbricking, return to stock. |
| [guides/development.md](docs/guides/development.md) | Change, build and test the packages. |
| [hardware/](docs/hardware/README.md) | How each part works and why: boot, GPU, Wi-Fi, modem, audio, input, camera, sensors, fingerprint. |

## Credits

- Yurii Bishchuk ([YuriiBishchuk/pmaports](https://github.com/YuriiBishchuk/pmaports))
  started the google-bramble port; this repository continues it.
- The kernel is LineageOS
  [`android_kernel_google_redbull`](https://github.com/LineageOS/android_kernel_google_redbull).
- [postmarketOS](https://postmarketos.org) and pmbootstrap, Alpine Linux,
  Mesa (turnip), Phosh and gmobile.

## License

GPL-3.0, like pmaports (see [LICENSE](LICENSE)). Each package states its
own licence in its APKBUILD; the kernel patches are GPL-2.0, the
license of the kernel.
