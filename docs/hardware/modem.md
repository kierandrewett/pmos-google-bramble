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

# Voice calls and SMS over VoLTE

Networks without 2G or 3G (Three UK and its MVNOs, for example) carry voice
and SMS only over IMS (VoLTE). The modem then attaches to LTE for data only
("CS: detached" in `qmicli --nas-get-serving-system`), and without IMS an
SMS fails with `WmsMessageDeliveryFailure` and a call stays in "dialing".
Four things were missing, and each has a package or patch here:

1. **IMS Data service (`81voltd`).** The modem asks the application
   processor for the IMS data bearer (QMI service 770). Alpine's `81voltd`
   answers and brings up the `ims` bearer through ModemManager.
   `libqrtr`'s `qrtr_new_server()` sends the registration with node and
   port 0 and relies on the name service to fill them in, as the mainline
   kernel does. This kernel dropped such messages in `qrtr_sendmsg()`
   (node 0 is the modem), so 81voltd never appeared in `qrtr-lookup`.
   Kernel patch 0026 fills in the sender. `qrtr-lookup 770` should list
   "IMS data service".
2. **Carrier configuration files.** The modem reads its MCFG list from
   `/readonly/vendor/mbn/` over TFTP. `tqftpserv` (`temp/tqftpserv`, patch
   0002) serves it from `mbn/` in the firmware path, and `bramble-firmware`
   links the vendor partition's `rfs/msm/mpss/readonly/vendor/mbn` there.
3. **Carrier configuration selection (`bramble-mcfg`).** The modem holds
   configurations (about 100 `.mbn` files on the vendor partition, loaded
   over QMI PDC) and uses the active one for the band set, the IMS profile
   and the VoLTE and SMS-over-IMS settings. Android's `qcrild` picks and
   activates one for the SIM. Nothing does that here, so the modem stays on
   a generic configuration. `bramble-mcfg` (OpenRC, device-google-bramble)
   reads the SIM's MCC and MNC from ModemManager, finds the `.mbn` files
   whose trailer lists them (the PLMN list in the `MCFG_TRL` item 6, and
   item 2), prefers the one named like the network ("Smarty" for a Smarty
   SIM, `H3G_UK_Commercial` for a Three one), loads it if the modem does not
   have it, then selects and activates it. The modem keeps the choice across
   reboots, so a boot normally only compares the active configuration. The
   daemon talks QMI PDC over QRTR itself, because `qmicli --pdc-load-config`
   frees a mapped file and aborts. After activation the IMS services
   (`qrtr-lookup` services 18 and 33) appear and `81voltd` gets the bearer.
   `qmicli` cannot show this: `--pdc-list-configs` returns nothing useful
   on this modem. The modem stores few configurations; if a load fails,
   `bramble-mcfg` deletes other carriers' ones (never `SW_DEFAULT`,
   `WildCard`, `WildCard_IMS`, `Global`). Stock Android loads a missing one
   again when it needs it. Activating a configuration restarts the modem on
   purpose (dmesg: "Intentionally crash with err fatal for MCFG
   activation"). This happens once, at the first boot with a SIM, or when
   the SIM changes. Mobile data, calls and Wi-Fi drop, and ModemManager
   needs about a minute to find the modem again. `bramble-wlfw` sends the
   board data to the WLAN firmware again, so Wi-Fi is back after about
   10 s (see [wifi.md](wifi.md)).
4. **Voice audio (`bramble-voiced`).** See below.

SMS sends with `mmcli -m any --messaging-create-sms="text='hello',number='+44...'"`
and `mmcli -s N --send`. Calls start with `mmcli -m any
--voice-create-call="number=+44..."` or from Calls.

## Voice call audio

ModemManager only connects the call. The voice audio runs in the audio DSP
(Q6 voice, PCM `VoiceMMode1`, `hw:0,2`) and the modem attaches to it by
session id. Android's audio HAL starts it, and `bramble-voiced` (package
`bramble-voice`) does the same steps while a call is active:

- **Calibration.** The DSP voice path needs calibration blocks
  (`voice_get_cal: No cal data for cal 2/3/4/7`), which the HAL loads from
  `/vendor/etc/acdbdata` with `libacdbloader` through `/dev/msm_audio_cal`.
  Without them a call connects and nothing is heard either way.
  `bramble-acdb` (tools/android-helpers, in the Android HAL container)
  does this. It must run from `/data/local/tmp`, because a program in
  `/system/bin` may not load vendor libraries. The container needs
  `/dev/msm_audio_cal`. The kernel frees the blocks when the last user of
  the device closes it, so the helper stays up for the call. The ACDB device
  ids come from `audio_platform_info_bolero_snd.xml`: earpiece 7 and 4,
  speaker 14 and 19, headphones 45 and 4, headset 45 and 44.
- **Routing.** Mixer controls connect the voice session to the amplifier
  (`QUIN_TDM_RX_0_Voice Mixer VoiceMMode1`) or the headset, and the
  microphone (`VoiceMMode1_Tx Mixer PRI_TDM_TX_0_MMode1`). Stock `voice-handset`
  and `voice-speaker` choose the left and the right amplifier.
  The internal microphones need `Sto1/Sto2 ADC MIXL DMIC Switch`, which the
  UCM "Mic" device only sets while something records, so the daemon sets
  them.
- **HD voice and gain.** `HD Voice Enable`, `Voice Tx Mute`, `Voice Rx Gain`
  take a session id (`0x11C05000` for VoiceMMode1). Their range is declared
  as max -1, so `amixer` turns every value into -1; the daemon writes them
  through alsa-lib's control API.
- **The PCM.** The voice session starts when the playback and the capture
  stream of `hw:0,2` are both prepared. The PCM has no data buffer: reading
  or writing it (`aplay`, `arecord`, PipeWire) oopses the kernel in
  `__arch_copy_from_user`. Open it, prepare it, start it, do no I/O.

The output follows callaudiod's `SpeakerState` (the speaker button in
Calls), and the headset jack. The microphone mute follows the default
PipeWire source. In-call music and recording (`hw:0,23`, mixers
`Incall_Music Audio Mixer MultiMedia9`, `MultiMedia9 Mixer VOC_REC_UL`)
show what the DSP sends: a recording of the uplink is the quickest check
that the microphone reaches the voice path.

Checks: `rc-status | grep -E 'bramble-(voiced|mcfg)|81voltd'`; `logread |
grep bramble-voiced` shows "route earpiece" and "voice session started" at
answer; `dmesg | grep voice_get_cal` must stay empty.

RCS is not supported. Carriers in the UK use Google's Jibe, which needs
Google Play services (a DroidGuard check) to register. It cannot run on this
stack; see [status.md](../status.md).

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
