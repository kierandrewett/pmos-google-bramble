#!/usr/bin/env python3
# Pixel 4a (5G): download the WLAN board data file (BDF) to the WLAN firmware.
#
# On this downstream kernel the icnss driver does the first part of the WLFW
# handshake (indication register, MSA, capabilities), but it never sends the
# BDF: on Android, vendor cnss-daemon does that from userspace. Without the
# BDF the firmware never sends FW_READY, the qcacld driver never probes and
# there is no wlan0. This does the same as upstream ath10k
# (ath10k_qmi_bdf_dnld_send_sync and ath10k_qmi_send_cal_report_req).
#
# The WLAN firmware runs on the modem, and it starts again with the modem:
# after a modem crash, and after a carrier configuration change (the modem
# restarts on purpose to apply it, see bramble-mcfg). It needs the BDF again
# each time. So this runs until it is stopped. It asks the QRTR name service
# to tell it about the WLFW service (service 69), and it sends the BDF each
# time that service appears.
#
# Usage: bramble-wlfw-bdf.py [BDF_PATH]
import select
import socket
import struct
import sys
import time

WLFW_SERVICE = 69
BDF_DOWNLOAD_REQ = 0x0025
CAL_REPORT_REQ = 0x0026
MAX_DATA = 6144
QMI_REQUEST = 0x00
DEFAULT_BDF = "/run/msm-firmware-loader/target/bdwlan-bramble.bin"

# Name service control messages (include/uapi/linux/qrtr.h): command, service,
# instance, node, port. A lookup for a service gets one NEW_SERVER message for
# each server that exists, and later a NEW_SERVER or DEL_SERVER message each
# time one comes or goes. NEW_SERVER with port 0 ends the list of existing ones.
QRTR_PORT_CTRL = 0xFFFFFFFE
QRTR_TYPE_NEW_SERVER = 4
QRTR_TYPE_DEL_SERVER = 5
QRTR_TYPE_NEW_LOOKUP = 10
CTRL_PKT = struct.Struct("<5I")

# The kernel does its own handshake with the firmware when the service
# appears. Wait this long without news before the BDF: it goes after that.
SETTLE = 1.0
ATTEMPTS = 5


def log(msg):
    print(f"[bramble-wlfw] {msg}", flush=True)


def watch_service():
    """Return a QRTR socket that the name service sends WLFW server changes to."""
    sock = socket.socket(socket.AF_QIPCRTR, socket.SOCK_DGRAM)
    node, _ = sock.getsockname()
    pkt = CTRL_PKT.pack(QRTR_TYPE_NEW_LOOKUP, WLFW_SERVICE, 0, 0, 0)
    sock.sendto(pkt, (node, QRTR_PORT_CTRL))
    return sock


def read_change(watch):
    """Return (command, (node, port)) for a WLFW server message, else None."""
    data, addr = watch.recvfrom(4096)
    if addr[1] != QRTR_PORT_CTRL or len(data) < CTRL_PKT.size:
        return None
    cmd, service, _, node, port = CTRL_PKT.unpack_from(data)
    if service != WLFW_SERVICE or port == 0:
        return None
    if cmd not in (QRTR_TYPE_NEW_SERVER, QRTR_TYPE_DEL_SERVER):
        return None
    return cmd, (node, port)


def tlv(t, value):
    return struct.pack("<BH", t, len(value)) + value


def qmi_msg(txn, msg_id, tlvs):
    body = b"".join(tlvs)
    return struct.pack("<BHHH", QMI_REQUEST, txn, msg_id, len(body)) + body


def qmi_result(resp):
    """Return (result, error) from TLV 0x02 of a QMI response."""
    body = resp[7:]
    i = 0
    while i + 3 <= len(body):
        t, ln = struct.unpack_from("<BH", body, i)
        if t == 0x02 and ln >= 4:
            return struct.unpack_from("<HH", body, i + 3)
        i += 3 + ln
    return None


def transact(sock, addr, txn, msg_id, tlvs):
    sock.sendto(qmi_msg(txn, msg_id, tlvs), addr)
    while True:
        data, _ = sock.recvfrom(65536)
        if len(data) < 7:
            continue
        mtype, rtxn, rmsg = struct.unpack_from("<BHH", data, 0)
        if rtxn == txn and rmsg == msg_id:
            return qmi_result(data)


def send_bdf(path, addr):
    """Send the BDF and the calibration report to the WLFW server at addr."""
    with open(path, "rb") as f:
        data = f.read()
    log(f"WLFW at node {addr[0]} port {addr[1]}, BDF {path} ({len(data)} bytes)")

    sock = socket.socket(socket.AF_QIPCRTR, socket.SOCK_DGRAM)
    sock.settimeout(10)
    try:
        txn = 1
        offset = 0
        seg = 0
        while True:
            chunk = data[offset:offset + MAX_DATA]
            end = 1 if offset + len(chunk) >= len(data) else 0
            tlvs = [
                tlv(0x01, struct.pack("<B", 1)),                 # valid
                tlv(0x10, struct.pack("<I", 0)),                 # file_id
                tlv(0x11, struct.pack("<I", len(data))),         # total_size
                tlv(0x12, struct.pack("<I", seg)),               # seg_id
                tlv(0x13, struct.pack("<H", len(chunk)) + chunk),  # data
                tlv(0x14, struct.pack("<B", end)),               # end
            ]
            res = transact(sock, addr, txn, BDF_DOWNLOAD_REQ, tlvs)
            if res is None or res[0] != 0:
                log(f"BDF segment {seg} failed: {res}")
                return False
            txn += 1
            seg += 1
            offset += len(chunk)
            if end:
                break
        log(f"BDF sent in {seg} segments")

        # Calibration report with no meta data, as ath10k does.
        res = transact(sock, addr, txn, CAL_REPORT_REQ, [tlv(0x01, struct.pack("<B", 0))])
        if res is None or res[0] != 0:
            log(f"cal report failed: {res}")
            return False
        log("cal report sent")
        return True
    finally:
        sock.close()


def serve(watch, path, addr):
    """Send the BDF, and try again if that fails, unless the server changes."""
    for attempt in range(1, ATTEMPTS + 1):
        try:
            if send_bdf(path, addr):
                return
        except OSError as e:
            log(f"BDF download failed: {e}")
        # A message from the name service means the server went away or came
        # back: the main loop deals with that.
        if select.select([watch], [], [], 2)[0]:
            return
        log(f"retrying ({attempt}/{ATTEMPTS})")
    log("giving up until the WLFW service appears again")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BDF
    watch = watch_service()
    log("waiting for the WLFW service")
    pending = None
    while True:
        watch.settimeout(SETTLE if pending else None)
        try:
            change = read_change(watch)
        except socket.timeout:
            addr, pending = pending, None
            serve(watch, path, addr)
            continue
        if change is None:
            continue
        cmd, addr = change
        if cmd == QRTR_TYPE_NEW_SERVER:
            log(f"WLFW service appeared at node {addr[0]} port {addr[1]}")
            pending = addr
        else:
            log(f"WLFW service at node {addr[0]} port {addr[1]} is gone")
            pending = None


if __name__ == "__main__":
    sys.exit(main())
