# Mobile data and SMS

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

# GPS

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
