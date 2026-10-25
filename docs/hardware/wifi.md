# Wi-Fi

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
