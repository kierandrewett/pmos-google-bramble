#!/bin/sh
# Pixel 4a (5G): bind the USB gadget once the DWC3 controller exists.
#
# a600000.dwc3 only probes after the PMIC charger and Type-C drivers, which
# is about 0.1-1 s after stage 1 of the initramfs tries to write the UDC. The
# initramfs only sets up USB networking once, so the gadget stays unbound.
# This hook runs in stage 2: wait for the UDC, bind the gadget that stage 1
# already created, then start the DHCP server for USB networking.

. /init_functions.sh
. /usr/share/misc/source_deviceinfo

udc="${deviceinfo_usb_network_udc:-a600000.dwc3}"

i=0
while [ ! -e "/sys/class/udc/$udc" ] && [ "$i" -lt 100 ]; do
	sleep 0.1
	i=$((i + 1))
done

if [ ! -e "/sys/class/udc/$udc" ]; then
	echo "[bramble] UDC $udc not found after 10 s"
	exit 0
fi

if [ -d "$CONFIGFS/g1" ] && [ -z "$(cat "$CONFIGFS/g1/UDC" 2>/dev/null)" ]; then
	echo "[bramble] binding USB gadget to $udc after $((i * 100)) ms"
	setup_usb_configfs_udc
	start_unudhcpd
fi
