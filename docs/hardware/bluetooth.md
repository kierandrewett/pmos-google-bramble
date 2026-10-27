# Bluetooth

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
  phase. With the patch, suspend completes with Bluetooth on (`hci0` up) and
  the cable out.

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
