# Boot chain and partitions

| Partition | Content | Notes |
|---|---|---|
| `boot` | kernel (`Image.lz4`) | Boot image header v3. |
| `vendor_boot` | initramfs + base DTB | The base DTB is `lito-v2.dtb` + `lito.dtb` from our kernel. |
| `dtbo` | **stock** Google overlay | Keep the stock one. See "Device tree" below. |
| `vbmeta` | AVB disabled | `pmbootstrap flasher flash_vbmeta`. |
| `userdata` | postmarketOS root file system | `pmbootstrap flasher flash_rootfs`. |
| `vendor_a/b` (in `super`) | stock firmware | Read at boot by `msm-firmware-loader`. |

The firmware for the modem, WLAN, ADSP (audio), GPU zap shader and touch
comes from the stock vendor partition, so the phone must run the final
stock build (Android 14, `UP1A.231105.001.B2`) before installing.

## Device tree

The bootloader applies the dtbo partition (board overlays for every
bramble revision) on top of the base DTB in vendor_boot.

- The dtbo built from this kernel tree does not work. Its overlays
  reference `&clock_rpmh`, which the LineageOS base DT labels `rpmhcc` only,
  so the bootloader stops with "error boot prepare". With that label added
  the bootloader accepts it, but the kernel then hangs at the Google logo.
  The kernel package therefore does not install a dtbo, and `deviceinfo`
  has no dtbo flash target.
- DT changes for this port go into the base DT (`lito.dtsi`) when the
  stock overlay does not touch the node (Bluetooth), or into the driver
  when it does (the flash LED names).
