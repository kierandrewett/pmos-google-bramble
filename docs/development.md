# Development

How to change, build and test the bramble packages. Read
[porting-notes.md](porting-notes.md) first.

## Repository layout

This repository is an overlay: it holds only the packages that the port
adds to or changes in [pmaports](https://gitlab.postmarketos.org/postmarketOS/pmaports),
in the same directory layout, plus docs and scripts. The git history of
`pmaports/` is the history of the port.

| Path | What |
|---|---|
| `pmaports/` | The packages, in pmaports layout. This is the source of truth: commit package changes here. |
| `pmaports/device/downstream/device-google-bramble/` | Device package: deviceinfo, udev rules, ALSA UCM, OpenRC services for the modem, firmware, GNSS (`bramble-gnss`) and UI boost (`bramble-ui-boost`), environment for phoc, `libfix_drm_formats.so`; the `-pipewire` subpackage has the WirePlumber config and the speaker filter. |
| `pmaports/device/downstream/linux-google-bramble/` | Downstream 4.19 kernel: config and patches `0001`-`0022`. |
| `pmaports/main/qrtr-ns/`, `pd-mapper/` | QRTR name service and protection domain mapper for kernels without in-kernel QRTR NS / remoteproc. |
| `pmaports/main/mesa-vulkan-freedreno-kgsl/` | Mesa turnip (Vulkan) for the downstream KGSL GPU driver. |
| `pmaports/temp/tqftpserv/`, `temp/gmobile/`, `temp/phoc/` | Temporary forks of Alpine packages with bramble patches. `temp/phoc` adds the screen fade and a crash fix; phoc and Phosh are released together, so rebase it when Alpine edge moves Phosh to a new version. |
| `scripts/setup-pmaports.sh` | Puts these packages into a pmaports checkout at the pinned commit. |
| `docs/` | Documentation. |

pmbootstrap builds from one pmaports checkout, so the packages are copied
into one (`build/pmaports` by default). After changing a package here, run
`scripts/setup-pmaports.sh` again. It replaces the bramble package
directories in the checkout with the ones from this repository, so files
deleted here are deleted there too. It does not overwrite anything else:
it stops if the checkout has other local changes, if a copied file was
edited in the checkout since the last run, or if the `google-bramble`
branch there has commits of its own. Edit files here, not in the checkout.

To move the port to a newer pmaports, set `PMAPORTS_COMMIT` (or edit the
default in the script), run the script, build and test, then commit the
new default.

## Build

```sh
pmbootstrap checksum device-google-bramble     # after changing source files
pmbootstrap build device-google-bramble
```

Bump `pkgrel` for every change you install on a phone, so `apk` sees a new
version. A kernel build takes about 10-15 minutes, Mesa about 15-25.

## Test on a running phone

Install a rebuilt package over USB networking:

```sh
pmbootstrap sideload device-google-bramble
```

Or copy and install it by hand. The phone has no SFTP server, so use
`scp -O` (legacy SCP protocol):

```sh
scp -O ~/.local/var/pmbootstrap/packages/edge/aarch64/device-google-bramble-*.apk user@172.16.42.1:/tmp/
ssh user@172.16.42.1 'sudo apk add --allow-untrusted /tmp/device-google-bramble-*.apk'
```

### Kernel changes

Installing a new `linux-google-bramble` on the phone regenerates
`/boot/boot.img` and `/boot/vendor_boot.img`, but the bootloader reads the
`boot` and `vendor_boot` partitions, so flash them:

```sh
pmbootstrap flasher flash_kernel
pmbootstrap flasher flash_vendorboot
```

Or copy `/boot/boot.img` and `/boot/vendor_boot.img` from the phone and
flash them with `fastboot flash boot` / `fastboot flash vendor_boot`.

To get from a running phone to fastboot without the buttons: BusyBox
`reboot` ignores the `bootloader` argument, so call the syscall directly:

```sh
ssh user@172.16.42.1 'sudo python3 -c "
import ctypes, os; os.sync()
ctypes.CDLL(None).syscall(142, 0xfee1dead, 672274793, 0xA1B2C3D4, b\"bootloader\")"'
```

Device tree changes go into the base DT (patched `lito.dtsi`, built into
vendor_boot) or into drivers. Do not flash a dtbo built from this tree;
see porting-notes.md.

## Debugging

- Kernel log: `dmesg`. Persistent log after a crash: `/sys/fs/pstore/`.
- System log: `logread` (busybox syslog).
- Display timing: the SDE driver has ftrace events
  (`/sys/kernel/tracing/events/sde/`); `tracing_mark_write` shows the
  commit and kickoff stages.
- Audio: `alsaucm -c hw:0 list _verbs`, `amixer -c0 controls`. `wpctl`,
  `pw-cli` and `pactl` over SSH need `XDG_RUNTIME_DIR=/run/user/10000`
  (only exists after logging in).
- If the phone hangs at the Google logo: hold Power + Volume Down to reach
  fastboot, then flash known-good images.

## Code style and commits

Follow pmaports conventions: `maintainer=` in APKBUILDs, no executable
files in the tree (install with `install -Dm755`), kernel patches with a
`From:`/`Subject:` header and an explanation of why. Run the pmaports
checks from the pmaports checkout:

```sh
cd build/pmaports && PYTHONPATH=/path/to/pmbootstrap:.ci/lib pytest .ci/testcases
```
