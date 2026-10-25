# Back up, recover and return to stock

## Before you install: back up the device-unique partitions

The factory image restores every Google-signed partition, but not the
partitions that are unique to your phone: modem calibration and IMEI
(`modemst1`, `modemst2`, `fsg`, `fsc`), `persist` (Wi-Fi, sensor and
display calibration) and a few others. postmarketOS does not write to them,
with one exception: the Android container writes the fingerprint reader's
secure storage to `persist` once you enrol a finger (see
[porting-notes.md](porting-notes.md), "Fingerprint"). Keep a copy of all of
them anyway.

You need root on the phone to read them. The simplest way is a temporary
root shell from a custom recovery (for example LineageOS recovery booted
with `fastboot boot`) with root `adb`:

```sh
adb root
mkdir -p bramble-backup && cd bramble-backup
for p in persist modemst1 modemst2 fsg fsc devinfo frp keystore secdata; do
	adb exec-out "dd if=/dev/block/by-name/$p 2>/dev/null" > "$p.img"
done
sha256sum *.img > SHA256SUMS
```

Keep these files somewhere safe. They are specific to one phone.

## Return to stock Android

1. Download the final factory image for bramble from
   <https://developers.google.com/android/images#bramble>
   (`bramble-up1a.231105.001.b2`, Android 14), and check its SHA-256 against
   the value on that page.
2. Boot the phone to fastboot: power off, then hold Power + Volume Down.
3. Unzip the image and run its script:

   ```sh
   unzip bramble-up1a.231105.001.b2-factory-*.zip
   cd bramble-up1a.231105.001.b2
   ./flash-all.sh
   ```

   `flash-all.sh` flashes the bootloader, radio and the full system image,
   and wipes userdata.
4. Let Android boot once and finish setup.
5. Optional, only after Android boots fine: relock the bootloader with
   `fastboot flashing lock`. Never relock while any non-stock image is on
   the phone; the phone will refuse to boot.

Google's web flasher (<https://flash.android.com>) does the same in a
browser.

## If postmarketOS does not boot

- **Stuck at the Google logo, no USB device on the PC:** hold Power +
  Volume Down for about 10 seconds to force the bootloader, then flash a
  known-good `boot` and `vendor_boot` (or reinstall, see
  [install.md](install.md)).
- **Bootloader shows "error boot prepare":** the dtbo partition holds
  something other than the stock overlay. Flash the stock one back from the
  factory image: unzip `image-bramble-*.zip` from inside the factory zip
  and run `fastboot flash dtbo dtbo.img`.
- **Reach a shell over USB:** postmarketOS starts USB networking at boot.
  `ssh user@172.16.42.1` (the password you set at install).

## Restore device-unique partitions (only if damaged)

Symptoms: no IMEI, no Wi-Fi MAC or calibration, "persist" errors in
Android. From a root shell (custom recovery or postmarketOS):

```sh
sha256sum -c SHA256SUMS
dd if=modemst1.img of=/dev/disk/by-partlabel/modemst1 bs=1M
dd if=modemst2.img of=/dev/disk/by-partlabel/modemst2 bs=1M
dd if=fsg.img      of=/dev/disk/by-partlabel/fsg      bs=1M
dd if=fsc.img      of=/dev/disk/by-partlabel/fsc      bs=1M
dd if=persist.img  of=/dev/disk/by-partlabel/persist  bs=1M
```

Restore only the partitions that are damaged.
