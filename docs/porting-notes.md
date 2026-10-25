# Porting notes

Technical notes on how each part of the Pixel 4a (5G) works on this port,
and why it is done this way. Read this before changing a package.

The port runs the downstream vendor kernel: Linux 4.19.325 from LineageOS
[`android_kernel_google_redbull`](https://github.com/LineageOS/android_kernel_google_redbull)
(commit `2af5306961aebce4cc30098c9f12a09f8d906906`, with CIP 4.19 SLTS
fixes), built with clang 16. Mainline Linux has no SM7250 support yet (the
first base SoC series boots only Qualcomm's reference board to a serial
console), so every driver here is the Qualcomm/Google downstream one.

## Boot chain and partitions

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

## Display and GPU

- Display: Qualcomm SDE (`msm_drm`), DSI command-mode OLED, 1080x2340.
  The panel's `autorefresh` connector property must be 0. With it on,
  every atomic commit waits for the running transfer and for the
  autorefresh disable sequence, so each page flip took three TE periods:
  50 ms, 20 fps. `libfix_drm_formats.so` (an `LD_PRELOAD` shim in
  device-google-bramble) sets it and works around a few other quirks of
  the downstream DRM driver.
- GPU: Adreno 620 through the downstream KGSL driver (`/dev/kgsl-3d0`),
  not the upstream msm DRM driver. Alpine's Mesa only builds freedreno for
  msm, so `mesa-vulkan-freedreno-kgsl` builds Mesa's Vulkan driver (turnip)
  with the KGSL backend. OpenGL comes from Alpine's zink on top of turnip
  (`MESA_LOADER_DRIVER_OVERRIDE=zink`), and phoc uses its GLES2 renderer.
  - turnip on KGSL reports no DRM nodes, so zink and wlroots cannot match
    it to the display. A patch makes it report the display controller's
    nodes (`/dev/dri/card0`, `renderD128`).
  - The msm backend is built as well. With KGSL alone, meson drops libdrm,
    which compiles out the DRM image paths of the Vulkan WSI, and
    swapchain creation crashes.
  - `/dev/kgsl-3d0` and `/dev/ion` must be readable by the `video` group
    (udev rule); the greeter fails with `EGL_NOT_INITIALIZED` otherwise.
- Top bar: gmobile (Phosh's panel database) has an entry for
  `google,bramble-sm7250` (temp/gmobile), so Phosh keeps the status icons
  clear of the punch-hole camera and the rounded corners.
- Screen fade: Phosh and phoc turn the output off and on in one step, so
  the screen went black or lit up at once on the power button. This is
  upstream behaviour, not specific to this phone. temp/phoc carries a
  patch that reuses phoc's output shield to fade over 250 ms in both
  directions; it is written to be sent upstream.
- GPU clock: kgsl accepts only the vendor `msm-adreno-tz` governor, which
  does not raise the clock here, so on its own the GPU stays at 275 MHz.
- CPU: the kernel config makes schedutil the default cpufreq governor,
  as Android uses on this SoC (the imported config ran every cluster at
  its highest clock).
- Animation speed: a lock screen or greeter frame runs in sequence. phosh
  or phrog repaints the whole 1176x2550 window in software (GTK3,
  `hdy_carousel_box_draw`, 17-21 ms on a little core), phoc copies the
  12 MB SHM buffer into a texture, composites every layer on the GPU, and
  waits for the GPU before the next frame: about 46 ms, 20-30 fps. Android
  boosts its UI through schedtune; here no schedtune group was mounted, so
  everything ran on the little cores at 275 MHz GPU. `bramble-ui-boost`
  (OpenRC) mounts schedtune, keeps only phoc, phosh and phrog in a group
  with boost 50 (apps started from the shell are moved back out) and sets
  the GPU floor to 625 MHz (`min_pwrlevel` 0). Measured with injected
  swipes and the CRTC `total_framecount` in DRM debugfs: lock screen
  47-55 fps (21-23 without), greeter 55-60, app grid while dragging 57-60.
  The rest is upstream: phoc still draws layers hidden under the opaque
  lock screen and waits for the GPU twice per frame, and GTK3/libhandy
  repaint the whole window for every carousel frame.

## Wi-Fi

On SM7250 the WLAN firmware runs on the modem DSP (`msm/modem/wlan_pd`),
so Wi-Fi needs the modem booted, even without mobile data.
device-google-bramble repeats the stock Android start sequence:

1. `qrtr-ns` (the kernel has no in-kernel QRTR name service) and
   `pd-mapper` start in the boot runlevel. The kernel service locator gives
   up for the whole boot if `pd-mapper` is missing for 300 s.
2. `tqftpserv` serves files to the modem. It is patched for kernels without
   the remoteproc class (temp/tqftpserv).
3. `bramble-modem` sets the subsystem restart level to `related`, loads the
   IPA firmware (`/dev/ipa`), runs `rmtfs -P`, holds `/dev/subsys_modem`
   open and runs `bramble-wlfw-bdf`. That helper sends the board data file
   to the WLFW QMI service, the job of the vendor `cnss-daemon`.
4. The firmware then reports `WLAN FW is ready` and qcacld creates
   `wlan0`. NetworkManager ignores the extra `wlan1`, `p2p0` and
   `wifi-aware0` interfaces.

The kernel needs patch 0003: NetworkManager 1.58 sends on-link routes with
`RTA_GATEWAY = 0.0.0.0`, which this kernel rejected, so connections had an
address but no routes.

NetworkManager arms WoWLAN by default (conf.d `wifi.wake-on-wlan=28`).
Without it, cfg80211 disconnects on suspend, the firmware raises a wake
interrupt, and every suspend is aborted.

WLAN IPA offload is off: `bramble-firmware` gives qcacld a copy of the
vendor `WCNSS_qcom_cfg.ini` with `gIPAConfig=0`. With the offload on, each
WLAN connect or disconnect reconfigures the shared IPA hardware (on Android
IPACM manages that) and the mobile data uplink stops until the next data
connection.

## Mobile data and SMS

The stock pmOS stack works: ModemManager (qcom-soc plugin) talks QMI to the
modem over QRTR, NetworkManager brings up the connection, and the kernel
rmnet driver muxes QMAP on `rmnet_ipa0`, the IPA data path to the modem.
The connection interface is `qmapmux0.0`. Android does this with netmgrd
and vendor ioctls instead, so the kernel needs these patches:

- 0011: parent `rmnet_ipa0` to the IPA device, set the QMAP formats
  netmgrd would set (QMAPv5 with checksum offload downlink, plain QMAP
  uplink), expose the mainline IPA sysfs files ModemManager reads
  (`feature/{rx,tx}_offload`, `modem/{tx,rx}_endpoint_id`), and add a
  catch-all uplink filter rule behind the modem's rules.
- 0012: drop two IPA DT properties that assume the Android data stack
  (`qcom,modem-cfg-emb-pipe-flt`, `qcom,wan-use-skb-page`).
- 0013: link rmnet devices to their real device, so ModemManager matches
  `qmapmux0.0` with the modem.
- 0014: size QMAPv5 frames on the WAN consumer pipe from the IPA status
  length. The vendor code assumed QMAPv4 framing, so downlink packets
  were dropped.
- 0015: use the mainline netlink values for the rmnet MAPv5 flags; the
  vendor copy numbers them differently.
- 0016: fix a NULL dereference in `rmnet_changelink()` (`ip link set ...
  type rmnet`).

Set up the connection once with your APN, for example
`nmcli c add type gsm ifname '*' con-name mobile apn <apn>`; it then
connects at boot.

Checks: `mmcli -m any` shows the modem registered, `nmcli d` shows
`qrtr0` connected, and `ping -I qmapmux0.0 1.1.1.1` answers. The IPA
state is in `/sys/kernel/debug/ipa/` (`status_stats`, `ip4_flt_hw`,
`stats`).

SMS works through ModemManager (`mmcli -m any --messaging-list-sms`).
Chatty uses the same ModemManager interface but was not tested.

## GPS

The GNSS engine is in the modem. ModemManager drives it through the QMI
LOC service, and geoclue reads positions from ModemManager.

While it loads the location capabilities, ModemManager sends LOC "Set
Engine Lock" with lock type MT (`unlock_location_engine()` in
`mm-shared-qmi.c`), meant to lock only network-initiated sessions. This
firmware (MPSS.HI.2.0) keeps the lock as a client mask: none = 0x0,
mi = 0x1, mt = 0x2, all = 0x3. With mt or all, LOC Start fails with
`GeneralError` and the session status is ENGINE_LOCKED (7), so
ModemManager's own GPS sessions never start ("Couldn't start GPS
engine"). With none or mi they start.

`bramble-gnss` (OpenRC, device-google-bramble) watches ModemManager on
D-Bus. ModemManager exports a modem object only after that step, so each
time one appears (boot, ModemManager restart, modem re-probe) the service
runs `qmicli -p -d qrtr://0 --loc-set-engine-lock=none`.

The engine also needs the time. After a modem boot it has no valid time,
does not search (ModemManager gets empty NMEA messages) and does not ask
for time, although the modem knows it from the network. On Android the
location stack injects NTP time (`NTP_SERVER` in the vendor `gps.conf`).
So `bramble-gnss` waits until chrony has synced the system clock
(`chronyc waitsync`; the clock reads 1970 early in boot) and then runs
`qmicli -p -d qrtr://0 --loc-inject-time`. The time lasts until the next
modem boot.

libqmi 1.39 hides the lock problem: it defines ENGINE_LOCKED as 6 instead
of 7, and `--loc-get-engine-lock` reads only TLV 0x10, while this firmware
answers with TLV 0x12 ("missing" in qmicli).

Checks: `mmcli -m any --location-enable-gps-nmea`, then
`mmcli -m any --location-get` lists `$GPGSV` and `$GPGGA` sentences. Turn
it off again with `--location-disable-gps-nmea`.

## Audio

Card `sm7250-bolero-snd-card` (ADSP, q6 ASM/AFE). The ALSA UCM profile is
in `device-google-bramble/ucm/`, built from the stock
`mixer_paths_bolero_snd.xml`. alsa-lib finds it by driver name, which the
kernel cuts to `sm7250-bolero-s`.

- Speakers: two Cirrus CS35L41 amps on QUIN TDM. Stock Android runs the
  Cirrus speaker-protection DSP firmware, which needs per-device
  calibration. This port bypasses the amp DSP (`PCM Source` = `ASP`) and
  uses the stock amp gain and volume (0 dB).
- Main microphone: Realtek RT5514 on PRI TDM. Capture must be two
  channels; mono capture returns only zeros.
- Headphones and headset mic: WCD938x. Plug detection works through MBHC.
  Patch 0004 adds ALSA jack controls ("Headphone Jack", "Headset Mic Jack")
  so PulseAudio can switch outputs on plug-in. PulseAudio reads them
  through alsa-lib's simple mixer, which reads every boolean, integer and
  enumerated control while it loads and gives up on the whole card at the
  first failed read. 57 q6 controls fail (write-only, or readable only
  while a DSP session runs), so patch 0005 adds a per-card flag that makes
  failed control reads return zeros, set by the machine driver (`kona.c`).
  Check: `amixer -c0 scontrols` lists controls instead of printing
  "Mixer load error".
- MultiMedia1 must stay routed to a backend while PulseAudio probes the
  card, or the DSP rejects `hw_params` and PulseAudio drops the card. The
  UCM verb routes it.
- PipeWire backend: `device-google-bramble-pipewire` (installed together
  with PipeWire) ships a WirePlumber config (`51-bramble.conf`) that
  disables PipeWire's extra compress-offload device for the card and the
  camera monitors. WirePlumber stopped handling events after probing the
  downstream V4L2 nodes, so the audio nodes were never linked and nothing
  played.
  Speaker and headphones are separate ACP profiles (they share PCM 0);
  WirePlumber picks the best available profile on plug and unplug. A
  profile chosen by hand is saved in `~/.local/state/wireplumber/
  default-profile` and then wins over the automatic choice; delete that
  file to get automatic switching back.
- Speaker loudness (PipeWire): on Android the bypassed protection DSP
  also made the speaker louder. `device-google-bramble-pipewire` adds a
  filter-chain that WirePlumber puts in front of the speaker sink only
  (smart filter): +6 dB, then the LSP look-ahead limiter holds the peaks
  at -1 dBFS. LSP's "Bypass" control must be 0 (0 means processing) and
  its "Automatic level regulation" off, or the output stays near -9 dBFS.

## Bluetooth

The WCN3991 (Bluetooth and Wi-Fi in one chip) talks HCI over a QUP UART.
Android drives it from userspace (the vendor `bt_power` driver and the
Bluetooth HAL). This port uses the in-kernel `hci_qca` serdev driver, so
BlueZ gets `hci0`:

- 0008 adds a `qcom,wcn3990-bt` node to the UART with the supplies
  `hci_qca` expects. The vendor `bt_power` node stays enabled, because
  the sound card needs the BT FM SLIMbus codec, which needs `bt_power`.
  Both drivers hold the same regulators.
- 0010 adds the WCN3991 to btqca.
- 0017 parses the NVM file (`qca/crnv32u.bin`) from its inner tag set.
  A type 4 header encloses the type 2 tags; without the fix btqca never
  found the HCI tag, the NVM's 3.2 Mbaud default broke the link after
  the download, and `HCI_RESET` timed out.
- 0020 runs the controller without in-band sleep and deep sleep. With
  them, the controller stopped answering after its first sleep: on
  Android the HAL wakes it with UART clock votes that `hci_qca` does not
  make. The cost is some idle power while Bluetooth is on.
- 0019 lets the UART suspend. The downstream `msm_geni_serial` driver
  waits for the HAL's clock vote ioctls before it suspends a port; a port
  held by `hci_qca` never got them and blocked suspend in the noirq
  phase. Not yet verified with USB unplugged.

The NVM file sets a fixed address, `39:98:00:00:5A:AD`, the same on every
phone (Android sets the real one from the HAL). 0022 makes the controller
start unconfigured (`HCI_QUIRK_INVALID_BDADDR`), and `bootmac` (a
dependency of device-google-bramble) sets an address made from the serial
number, for example `02:00:5C:6C:B8:48`, before first use. Without 0022,
bootmac's request raced with the start-up and the controller dropped out
of mgmt until the next boot. The device udev rule gives bootmac 30 s
(`BT_TIMEOUT`) instead of 5 s, in case the firmware download is slow.

Switching Bluetooth off (rfkill) closes `hci0`: `hci_qca` sends the
WCN3990 power-off pulse and drops its regulator votes. The chip stays
powered for Wi-Fi and keeps its state, so switching on again needs no
firmware download.

A2DP works through WirePlumber's BlueZ monitor in a Phosh session. The
monitor only runs for an active login seat; to test without a session,
start WirePlumber with `monitor.bluez.seat-monitoring = disabled`.

## Input

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

## USB

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

scrcpy: the PC client pushes its Java server and runs it with
`app_process`. `temp/scrcpy-server` installs `/usr/bin/app_process` as a
shim that starts a native server instead. It speaks the scrcpy 4.x
protocol, captures phoc's output with wlr-screencopy (so it only sends
frames while the screen is on), encodes H.264 with x264 and injects touch
and keys through a uinput device.

## polkit

polkit 127 resolves each caller through a pidfd and reads the PID from
`/proc/self/fdinfo/<fd>`. On this kernel that directory was mode 0500 and
owned by root while the task is not dumpable, so polkitd (running as its
own user) got `EACCES` and every check failed with "Process not found":
Settings and Phosh could not switch Wi-Fi off, for example. 0018 backports
the mainline fdinfo access rule (7bc3fa0172a4).

## Shared memory

GNOME's image loader (glycin) passes decoded images in memfds sealed
with `F_SEAL_WRITE` and maps them read-only and shared. 4.19 refuses any
shared mapping of a write-sealed memfd with `EPERM`, so Image Viewer
(Loupe) showed "Could not load image" for every file. 0024 backports the
mainline 6.7 rule: a read-only shared mapping of a write-sealed memfd is
allowed, and `VM_MAYWRITE` is cleared so it cannot be made writable.

## Android HAL container

The cameras and the fingerprint reader only work through Google's closed
vendor HALs (camera: CAMX kernel drivers plus the vendor camera provider;
fingerprint: the FPC HAL with its trusted app in QSEE). `bramble-android-hal`
runs them without libhybris: Android 14 init from the Halium GSI
(`halium-gsi-34`) starts in private mount, PID, IPC and UTS namespaces
with the phone's own vendor partition, and only the vendor services
listed in `vendor-init.list` start. The modem, Wi-Fi, audio and the rest
stay with the native Linux stack. `bramble-android.sh` and `bramble.rc`
explain each workaround (no SELinux policy, APEX loop devices, stubbed
`vdc`, a stand-in for `system_server`'s camera proxy).

Kernel: `CONFIG_PID_NS` is on for the container's own PID 1, and 0023
lets the container mount its own procfs (the superblock is looked up in
the user namespace that owns the PID namespace, as in mainline).

Camera apps: `bramble-camera-ndk serve` runs inside the container and
streams NV12 frames from the NDK camera API over a host socket
(`/run/bramble-android/camera/camera.sock`). `bramble-camera-pipewire`
(started in each Phosh session) turns each camera into a PipeWire video
source node, which GNOME Snapshot uses through the camera portal. Photos
are HAL stills from a JPEG output in the same capture session, which the
patched Snapshot (`temp/snapshot`) asks the bridge for. See
[camera.md](camera.md) for the design and the problems found on the way
(a blocked HAL callback aborts the camera provider; a malformed size
choice stalls Snapshot).

## Sensors

The accelerometer, ambient light and proximity sensors sit behind the Qualcomm sensor core on the ADSP. `bramble-sensors` runs the patched `hexagonrpcd`, and `iio-sensor-proxy` (patch 0001 in `temp/iio-sensor-proxy`, built with libssc) serves them on `net.hadess.SensorProxy`. Its OpenRC service starts after `bramble-sensors`, which must have created the `adsprpc-smd-secure` node. The udev rule in `bramble-sensors` (`71-bramble-sensors.rules`) tags that node as `ssc-accel`, `ssc-light` and `ssc-proximity`.

Checked with `monitor-sensor` inside the Phosh session (it must run in the session cgroup for the polkit claim): light in lux, proximity near/far, tilt. Phosh's auto-brightness lowers the backlight at low light. Auto-rotate and the proximity blank during calls: see status.md.

## Fingerprint

The reader is driven by Android's fingerprint HAL (`fps_hal`), which runs in the `bramble-android-hal` container. The phone's own libfprint has only the elan driver, and it does not drive this HAL, so stock fprintd cannot be used. The `bramble-fingerprint` package (`pmaports/device/downstream/bramble-fingerprint`) talks to the HAL over hwbinder. `bramble-fingerprint.sh` finds the container's logd, then runs the client in a private mount namespace where `/dev/hwbinder` is the container's binder node, so the host `/dev` is untouched. The templates are stored by the HAL in the container's `/data`; the client keeps its own state in `/var/lib/bramble-fingerprint` (root only).

Commands: `bramble-fingerprint status|enroll|verify|list|remove`. It re-runs itself with sudo. Enrol waits up to 180 s for the touches, and the other commands up to 60 s.

Enrolment needs an auth token from gatekeeper, signed with the key that
keymaster agrees on at boot (`computeSharedHmac`). On Android, keystore2
does that agreement; the container has no keystore2, so the
`bramble-fingerprint` OpenRC service runs it after the container starts,
then restarts `fps_hal`. The restart also matters on its own: an
`fps_hal` that opened its trusted app before `qseecomd` registered its
listeners rejects every enrol with `FPC_ERROR_INPUT`.

Enrolment also writes to the `persist` partition. The fingerprint trusted app keeps its secure storage there (`/mnt/vendor/persist`, writable on Android). `msm-firmware-loader` mounts `persist` read-only on the host, and the container used to get a read-only bind of that mount. With it read-only, the last touch of every enrol failed with `FPC_ERROR_INPUT`. `bramble-android.sh` now remounts the filesystem read-write, binds it read-write into the container, and sets the host mount back to read-only (mount flags are per mount, so the host stays read-only and the container is the only writer). `persist` also holds calibration data that is unique to the phone, so keep a copy of it ([recovery.md](recovery.md)).

Unlock. This Phosh build has no fingerprint code: the lock screen only asks PAM for the password, and `fprintd` cannot reach the HAL. So `bramble-fingerprint-unlock` (an OpenRC service, enabled by the package) watches logind. While the user session on seat0 is locked (`LockedHint`), it runs `bramble-fingerprint verify`. On a match it runs `loginctl unlock-session`, which Phosh follows, and it turns the screen on if the screen is off. The PIN is unchanged and always works. The service does not use PAM, so `sudo` and polkit prompts still ask for the password.

To turn the screen on, the service sets the `PowerSaveMode` property of `org.gnome.Mutter.DisplayConfig` on the user's session bus (through `setpriv`, because only that user can connect). Setting it to 0 does nothing once Phosh has blanked the screen itself, so the service sets 3 first and then 0. It does this only when the display reports `dpms Off`.

The HAL does not count wrong fingers; on Android the framework does. The service copies Android's limits: 5 wrong fingers stop the reader for 30 s, and after 20 wrong fingers in one lock only the PIN unlocks, until the phone is unlocked once. A watchdog stops `verify` as soon as the session is unlocked another way. The service writes to `/var/log/bramble-fingerprint-unlock.log` (root only). Turn it off with `sudo rc-service bramble-fingerprint-unlock stop` and `sudo rc-update del bramble-fingerprint-unlock default`.

Not verified: whether a touch wakes the phone from suspend. While the phone is awake with the screen off, the reader is active.

## Not working yet

- Voice calls: not tested; the test SIM is data only.
- Suspend: a connected USB cable always blocks it (the USB controller
  stays out of low power mode). With USB unplugged it is not verified
  yet; see the status page.
