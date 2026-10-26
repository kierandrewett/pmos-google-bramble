# Install postmarketOS on the Pixel 4a (5G)

This guide builds postmarketOS with Phosh for the Pixel 4a (5G), codename
**bramble** (not the Pixel 4a, `sunfish`), and flashes it with fastboot.
Installing wipes all data on the phone.

## What you need

- A Pixel 4a (5G) whose bootloader can be unlocked. Carrier-locked phones
  (for example Verizon) usually cannot be unlocked.
- A Linux PC with `git`, `fastboot` (android-tools) and
  [pmbootstrap](https://wiki.postmarketos.org/wiki/Pmbootstrap) 3.11 or
  newer, and about 15 GB of free disk space. The first build compiles the
  kernel and Mesa, which takes 30-60 minutes.
- A USB cable that carries data.

## 1. Back up and prepare the phone

1. Back up anything you want to keep, and read
   [recovery.md](recovery.md). Back up the device-unique partitions if you
   can.
2. In Android: Settings > About phone > tap "Build number" seven times.
   Then Settings > System > Developer options > enable "OEM unlocking".
3. Boot to fastboot (power off, hold Power + Volume Down) and unlock:

   ```sh
   fastboot flashing unlock
   ```

   Confirm on the phone. This wipes the phone.
4. Flash the final stock firmware, Android 14 `UP1A.231105.001.B2`, with
   `flash-all.sh` from the factory image (see [recovery.md](recovery.md)),
   then let Android boot once. postmarketOS reads the modem, Wi-Fi, audio
   DSP and GPU firmware from the stock vendor partition, and boots with the
   stock dtbo, so this exact build must be on the phone.

## 2. Get the packages

```sh
git clone https://github.com/kierandrewett/pmos-google-bramble.git
cd pmos-google-bramble
scripts/setup-pmaports.sh
```

The script clones pmaports, checks out the commit this port was tested
against, and copies the bramble packages into it (`build/pmaports`, which
git ignores).

## 3. Configure pmbootstrap

```sh
pmbootstrap --aports "$PWD/build/pmaports" init
```

Answer the questions like this:

| Question | Answer |
|---|---|
| Channel | `edge` |
| Vendor | `google` |
| Device codename | `bramble` |
| User interface | `phosh` |
| Which init system | `openrc` (the port uses OpenRC services; the Phosh default would be systemd) |
| Provider for `postmarketos-base-ui-audio-backend` | `pipewire`. The default, PulseAudio, plays but does not switch to headphones; see [status.md](../status.md). |
| Provider for `postmarketos-base-ui-wifi` | the default, `wpa_supplicant` |
| Username, hostname, locale, timezone | your choice |

pmbootstrap warns that bramble is a downstream device; that is expected.

## 4. Build the image

```sh
pmbootstrap install --password <your PIN>
```

Use your own password. Phosh's lock screen has a number pad, so a
numeric password (a PIN) is easiest. The build compiles the kernel, Mesa's turnip
driver and the other bramble packages.

## 5. Flash

Boot the phone to fastboot (power off, then hold Power + Volume Down),
connect it, and run:

```sh
pmbootstrap flasher flash_vbmeta
pmbootstrap flasher flash_kernel
pmbootstrap flasher flash_vendorboot
pmbootstrap flasher flash_rootfs
fastboot reboot
```

Leave the `dtbo` partition alone; the stock one is required.

The first boot takes about a minute. Phosh's login screen appears; log in
with the password from step 4, then connect to Wi-Fi from the top bar.

## After installing

[usage.md](usage.md) covers SSH over USB, adb and scrcpy, the camera, enrolling
a finger, and the services you can turn off. [status.md](../status.md) lists
what has been tested and the known problems.
