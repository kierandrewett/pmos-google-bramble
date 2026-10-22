#!/bin/sh
# Hook for Google Pixel 4a (5G) (bramble) early boot initialization

echo "=========================================" > /dev/kmsg 2>/dev/null || true
echo "[pmOS-bramble-hook] Starting hardware bring-up hook..." > /dev/kmsg 2>/dev/null || true

# Source postmarketOS functions
[ -f /init_functions.sh ] && . /init_functions.sh
[ -f /init_functions_2nd.sh ] && . /init_functions_2nd.sh

# 1. Load TrustZone, SMEM, and Hardware Watchdog
for mod in \
    msm_ipc_logging qtee_shm_bridge qcom_hwspinlock smem msm_minidump \
    early_random watchdog_v2 cmd-db qcom-pdc qcom_rpmh pinctrl-msm \
    pinctrl-spmi-gpio pinctrl-lito clk-rpmh gcc-lito dispcc-lito \
    spmi-pmic-arb qpnp-revid refgen rpmh-regulator qpnp-amoled-regulator \
    tps-regulator slg51000-regulator regmap-spmi qcom-spmi-pmic \
    qpnp-power-on phy-generic phy-msm-ssusb-qmp phy-msm-snps-hs \
    tcpm qpnp_pdphy dwc3 usb-dwc3-msm dwc3-qcom msm_drm; do
    modprobe "$mod" 2>/dev/null || true
done

# 2. SCM TrustZone Watchdog Disarm & petting daemon
for wdt in /sys/bus/platform/devices/17c10000.qcom,wdt/disable /sys/devices/platform/soc/17c10000.qcom,wdt/disable; do
    if [ -f "$wdt" ]; then
        echo 1 > "$wdt" 2>/dev/null || true
        echo "[pmOS-bramble-hook] Hardware TrustZone Watchdog disabled via $wdt" > /dev/kmsg 2>/dev/null || true
    fi
done

(
    while true; do
        for dev in /dev/watchdog /dev/watchdog0; do
            if [ -c "$dev" ]; then
                echo "V" > "$dev" 2>/dev/null || true
            fi
        done
        sleep 2
    done
) &
echo "[pmOS-bramble-hook] Watchdog petting daemon started" > /dev/kmsg 2>/dev/null || true

# 2.1 Force DWC3-MSM into Peripheral Mode & Enable USB Data
for p in /sys/devices/platform/soc/a600000.ssusb/mode /sys/bus/platform/devices/a600000.ssusb/mode; do
    [ -f "$p" ] && echo peripheral > "$p" 2>/dev/null || true
done
for e in /sys/devices/platform/soc/a600000.ssusb/usb_data_enabled /sys/bus/platform/devices/a600000.ssusb/usb_data_enabled; do
    [ -f "$e" ] && echo 1 > "$e" 2>/dev/null || true
done

# 3. Wait up to 10 seconds for UDC controller registration
UDC=""
for i in $(seq 1 10); do
    UDC=$(ls /sys/class/udc 2>/dev/null | head -n 1)
    if [ -n "$UDC" ]; then
        echo "[pmOS-bramble-hook] Detected UDC: $UDC on attempt $i" > /dev/kmsg 2>/dev/null || true
        break
    fi
    sleep 1
done

if [ -z "$UDC" ]; then
    echo "[pmOS-bramble-hook] WARNING: UDC not found in sysfs after 10s, fallback to a600000.dwc3" > /dev/kmsg 2>/dev/null || true
    UDC="a600000.dwc3"
fi

# 4. Setup USB Gadget Network (NCM) directly
mkdir -p /config/usb_gadget/g1
cd /config/usb_gadget/g1 2>/dev/null || true
echo "0x18d1" > idVendor 2>/dev/null || true
echo "0x4eea" > idProduct 2>/dev/null || true
mkdir -p strings/0x409
echo "13011JECB06894" > strings/0x409/serialnumber 2>/dev/null || true
echo "Google" > strings/0x409/manufacturer 2>/dev/null || true
echo "Pixel 4a 5G Linux" > strings/0x409/product 2>/dev/null || true

mkdir -p configs/b.1/strings/0x409
echo "ncm" > configs/b.1/strings/0x409/configuration 2>/dev/null || true
echo 500 > configs/b.1/MaxPower 2>/dev/null || true

mkdir -p functions/ncm.usb0
ln -s functions/ncm.usb0 configs/b.1/f1 2>/dev/null || true

echo "" > UDC 2>/dev/null || true
echo "$UDC" > UDC 2>/dev/null || echo "[pmOS-bramble-hook] Error writing $UDC to UDC" > /dev/kmsg 2>/dev/null || true

ip addr add 172.16.42.1/24 dev usb0 2>/dev/null || true
ip link set usb0 up 2>/dev/null || true

# Start DHCP server
start_unudhcpd 2>/dev/null || true

# 5. Start telnet daemon on ports 23 & 24
if command -v telnetd > /dev/null; then
    telnetd -p 23 -l /bin/sh &
    telnetd -p 24 -l /bin/sh &
    echo "[pmOS-bramble-hook] Telnet daemon started on port 23 & 24" > /dev/kmsg 2>/dev/null || true
fi

echo "=========================================" > /dev/kmsg 2>/dev/null || true
