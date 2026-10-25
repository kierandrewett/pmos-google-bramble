# postmarketOS for the Google Pixel 4a (5G) (bramble)

A [postmarketOS](https://postmarketos.org) port for the Pixel 4a (5G),
codename `google-bramble` (Snapdragon 765G, SM7250). It runs Phosh on the
downstream Linux 4.19 vendor kernel, with GPU acceleration, Wi-Fi, mobile
data, audio and the cameras working.

This is a community port, not part of the official postmarketOS
repositories. Expect rough edges. It is not the Pixel 4a (`sunfish`).

## Status

| Works | Partly | Does not work |
|---|---|---|
| Display, touch, Phosh | Suspend (not verified unplugged) | Voice calls (not tested) |
| GPU acceleration (60 fps) | Sensors (auto-rotate not confirmed) | |
| Cameras: back, ultrawide, front; photos and video | Fingerprint reader (enrol and verify work; unlock with a finger not yet seen on the phone) | |
| Wi-Fi, Bluetooth | | |
| Mobile data (LTE), SMS, GPS | | |
| Speakers, microphone, headphones | | |
| Battery and charging | | |
| USB networking (SSH), MTP file transfer, adb, scrcpy | | |
| Vibration, torch, tap to wake | | |

Details, known problems and test notes: [docs/status.md](docs/status.md).

## Install

Follow [docs/install.md](docs/install.md). In short:

```sh
git clone https://github.com/kierandrewett/pmos-google-bramble.git
cd pmos-google-bramble
scripts/setup-pmaports.sh
pmbootstrap --aports "$PWD/build/pmaports" init   # answers: docs/install.md
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

To go back to Android: [docs/recovery.md](docs/recovery.md).

## Documentation

| Document | Contents |
|---|---|
| [install.md](docs/install.md) | Prepare the phone, build, flash. |
| [status.md](docs/status.md) | What works, what does not, known problems. |
| [recovery.md](docs/recovery.md) | Backups, unbricking, return to stock. |
| [porting-notes.md](docs/porting-notes.md) | How each part works and why (boot chain, GPU, Wi-Fi through the modem, audio, input). |
| [development.md](docs/development.md) | Change, build and test the packages. |
| [camera.md](docs/camera.md) | How the cameras work (vendor HAL, PipeWire, Snapshot) and their limits. |

## Repository layout

This is an overlay for pmaports: `pmaports/` holds only the bramble
packages, in pmaports layout, with their full history.
`scripts/setup-pmaports.sh` puts them into a pmaports checkout (pinned to
the tested commit) in `build/pmaports`. See
[development.md](docs/development.md).

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
