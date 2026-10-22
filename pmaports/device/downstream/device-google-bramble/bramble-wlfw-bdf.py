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
# Usage: bramble-wlfw-bdf.py [BDF_PATH]
import os
import socket
import struct
import subprocess
import sys
import time

WLFW_SERVICE = 69
BDF_DOWNLOAD_REQ = 0x0025
CAL_REPORT_REQ = 0x0026
MAX_DATA = 6144
QMI_REQUEST = 0x00
DEFAULT_BDF = "/run/msm-firmware-loader/target/bdwlan-bramble.bin"


def log(msg):
    print(f"[bramble-wlfw] {msg}", flush=True)


def find_wlfw(timeout=60):
    """Return (node, port) of the WLFW service, using qrtr-lookup."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        out = subprocess.run(["qrtr-lookup", str(WLFW_SERVICE)],
                             capture_output=True, text=True, timeout=10).stdout
        for line in out.splitlines():
            f = line.split()
            if len(f) >= 5 and f[0] == str(WLFW_SERVICE):
                return int(f[3]), int(f[4])
        time.sleep(1)
    return None


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


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BDF
    data = open(path, "rb").read()
    svc = find_wlfw()
    if svc is None:
        log("WLFW service not found")
        return 1
    addr = svc
    log(f"WLFW at node {addr[0]} port {addr[1]}, BDF {path} ({len(data)} bytes)")

    sock = socket.socket(socket.AF_QIPCRTR, socket.SOCK_DGRAM)
    sock.settimeout(10)
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
            return 1
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
        return 1
    log("cal report sent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
