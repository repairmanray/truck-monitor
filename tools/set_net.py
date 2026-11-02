#!/usr/bin/env python3
"""Sets the node's WiFi and collector settings over USB without printing them.

Usage: set_net.py <ssid> <collector_host> [port] [--dev=/dev/ttyUSB1]
       set_net.py <ssid> <collector_host> [port] --psk-empty

The PSK comes from NetworkManager's saved connection named after the SSID, and
the token from ~/truck-log/token on CT100. Run with the esptool venv's python.

--hotspot=<connection> also loads the update settings onto the truck node: that
hotspot's name and PSK, and the signing key from ~/.secrets/truck-ota.key.
"""

import os
import subprocess
import sys
import time

import serial

PORT = "/dev/ttyUSB0"
CT100 = "ray@192.168.1.104"


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    ssid, host = args[0], args[1]
    port = args[2] if len(args) > 2 else "8101"
    psk = ""
    if "--psk-empty" not in sys.argv:
        psk = subprocess.run(
            ["nmcli", "-s", "-g", "802-11-wireless-security.psk", "connection", "show", ssid],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
    token = subprocess.run(
        ["ssh", CT100, "cat truck-log/token"],
        capture_output=True, text=True, check=True,
    ).stdout.strip()

    fields = ["cfg", ssid, psk, host, port, token]
    hotspot = next((a.split("=", 1)[1] for a in sys.argv if a.startswith("--hotspot=")), None)
    if hotspot:
        out = subprocess.run(
            ["nmcli", "-s", "-g", "802-11-wireless.ssid,802-11-wireless-security.psk",
             "connection", "show", hotspot],
            capture_output=True, text=True, check=True,
        ).stdout.splitlines()
        with open(os.path.expanduser("~/.secrets/truck-ota.key")) as f:
            fields += [f.read().strip(), out[0].strip(), out[1].strip()]

    link = serial.Serial()
    link.port = next((a.split("=", 1)[1] for a in sys.argv if a.startswith("--dev=")), PORT)
    link.baudrate = 115200
    link.timeout = 0.2
    link.dtr = False
    link.rts = False
    link.open()
    link.rts = True
    time.sleep(0.1)
    link.rts = False

    buffer = b""
    deadline = time.time() + 8
    while b"# cfg window" not in buffer and time.time() < deadline:
        buffer += link.read(4096)
    if b"# cfg window" not in buffer:
        sys.exit("no cfg window seen")

    link.write(("\t".join(fields) + "\n").encode())
    buffer = b""
    deadline = time.time() + 6
    while b"# cfg " not in buffer and time.time() < deadline:
        buffer += link.read(4096)
    print("saved" if b"# cfg saved" in buffer else "NOT saved")


if __name__ == "__main__":
    main()
