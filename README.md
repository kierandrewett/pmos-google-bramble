# Google Pixel 4a (5G) (google-bramble)

A [postmarketOS](https://postmarketos.org) port for the Google Pixel 4a
(5G). It runs Phosh on the downstream Linux 4.19 vendor kernel.

This is a community port. It is not in the official postmarketOS
repositories, so it is a *downstream* device (see the
[postmarketOS device categories](https://wiki.postmarketos.org/wiki/Device_categorization)).
It is not the Pixel 4a (`sunfish`).

## Device

| | |
|---|---|
| Manufacturer | Google |
| Name | Pixel 4a (5G) |
| Codename | `google-bramble` (Android codename `bramble`) |
| Released | 2020 |
| Type | Handset |
| SoC | Qualcomm Snapdragon 765G (SM7250), `aarch64` |
| RAM and storage | 6 GB, 128 GB |
| Display | 6.2 inch OLED, 1080x2340, 60 Hz |
| Cameras | Main, ultrawide, front |
| Kernel | Downstream Linux 4.19 (LineageOS `android_kernel_google_redbull`) |
| User interface | Phosh |
| Init system | OpenRC |
| Category | Downstream |

## Features

"Works" means it was tested on a phone. [docs/status.md](docs/status.md)
says how each item was checked and lists the known problems.

| Feature | Status | Notes |
|---|---|---|
| Display, touch, Phosh | Works | GPU acceleration with Mesa turnip on KGSL. |
| Auto-rotate, auto-brightness | Works | Accelerometer and light sensor through iio-sensor-proxy. |
| Wi-Fi | Works | |
| Bluetooth | Works | Pairing, audio. Each phone gets its own address. |
| Mobile data (LTE) | Works | |
| SMS, voice calls | Partial | Outgoing SMS and calls over VoLTE work on a Three UK SIM. Incoming, MMS and the speaker button are not tested. |
| RCS | Not supported | Needs Google Play services. |
| GPS | Works | Switch Location on in Settings. |
| Audio: speakers, microphone, headphones | Works | Use PipeWire. PulseAudio does not switch to headphones. |
| Cameras: main, ultrawide, front | Works | Full-resolution photos and video in Snapshot. No flash, no HDR+. |
| Battery and charging | Works | |
| USB networking (SSH), MTP | Works | |
| adb and screen mirroring | Works | Packaged `adbd`; the stock `scrcpy` client works. |
| Vibration, torch, tap to wake | Works | |
| Fingerprint reader | Partial | Enrol and verify work. Unlock with a real finger is not yet seen. |
| Suspend | Partial | Not verified with USB unplugged. |
| Proximity blank during calls | Untested | Needs a call. |

The full walk-through of daily use is in
[docs/experience.md](docs/experience.md).

## Installation

Installing wipes the phone. You need:

- A Pixel 4a (5G) with an unlockable bootloader, on the final stock
  firmware (Android 14, `UP1A.231105.001.B2`) with the stock `dtbo`. The
  port reads the modem, Wi-Fi, audio DSP and GPU firmware from the stock
  vendor partition.
- A Linux PC with `git`, `fastboot` and
  [pmbootstrap](https://wiki.postmarketos.org/wiki/Pmbootstrap) 3.11 or
  newer, and about 15 GB of free disk space. The first build compiles the
  kernel and Mesa, which takes 30 to 60 minutes or more.

```sh
git clone https://github.com/kierandrewett/pmos-google-bramble.git
cd pmos-google-bramble
scripts/setup-pmaports.sh
pmbootstrap --aports "$PWD/build/pmaports" init   # answers: docs/guides/install.md
pmbootstrap install
```

Then put the phone in fastboot (Power + Volume Down) and flash:

```sh
pmbootstrap flasher flash_vbmeta
pmbootstrap flasher flash_kernel
pmbootstrap flasher flash_vendorboot
pmbootstrap flasher flash_rootfs
fastboot reboot
```

Do not flash `dtbo`. The full steps, including how to prepare the phone,
are in [docs/guides/install.md](docs/guides/install.md). To go back to
Android, read [docs/guides/recovery.md](docs/guides/recovery.md).

## Repository layout

This repository is an overlay for pmaports. `pmaports/` holds only the
packages for this phone, in pmaports layout. `scripts/setup-pmaports.sh`
clones pmaports at the tested commit and copies these packages into it.

```
pmaports/
  device/downstream/   device package, kernel, Android HAL container,
                       fingerprint, sensors
  main/                packages that fit pmaports main
  temp/                forks of Alpine packages with patches for this phone
docs/                  documentation (start at docs/README.md)
scripts/               setup-pmaports.sh and the CI checks (ci.sh)
tools/                 helpers that are not packages
build/                 generated, ignored by git: the pmaports checkout
```

The package list is in [pmaports/README.md](pmaports/README.md). To change
and test the packages, read
[docs/guides/development.md](docs/guides/development.md).

## Documentation

| Document | Contents |
|---|---|
| [docs/status.md](docs/status.md) | What works, what does not, known problems. |
| [docs/experience.md](docs/experience.md) | What daily use is like. |
| [docs/guides/install.md](docs/guides/install.md) | Prepare the phone, build, flash. |
| [docs/guides/usage.md](docs/guides/usage.md) | adb and scrcpy, camera, fingerprint, services you can turn off. |
| [docs/guides/recovery.md](docs/guides/recovery.md) | Backups, unbricking, return to stock. |
| [docs/guides/development.md](docs/guides/development.md) | Change, build and test the packages. |
| [docs/hardware/](docs/hardware/README.md) | How each part works and why. |

## Contributors

- Yurii Bishchuk ([YuriiBishchuk/pmaports](https://github.com/YuriiBishchuk/pmaports))
  started the google-bramble port. This repository continues it.
- The kernel comes from LineageOS
  [`android_kernel_google_redbull`](https://github.com/LineageOS/android_kernel_google_redbull).
- This port builds on [postmarketOS](https://postmarketos.org),
  pmbootstrap, Alpine Linux, Mesa (turnip), Phosh and gmobile.

## License

GPL-3.0, like pmaports (see [LICENSE](LICENSE)). Each package states its
license in its APKBUILD. The kernel patches are GPL-2.0, the license of
the kernel.
