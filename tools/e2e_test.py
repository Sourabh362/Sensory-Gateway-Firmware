#!/usr/bin/env python3
"""End-to-end check: replay a capture through the gateway and compare stats with its manifest.

usage: e2e_test.py <gateway> <capture.bin> <manifest.json>
"""
import json
import os
import subprocess
import sys
import tempfile


def main():
    gateway, capture, manifest_path = sys.argv[1:4]
    with open(manifest_path) as f:
        m = json.load(f)
    baud = m.get("baud", 115200)

    with tempfile.TemporaryDirectory() as tmp:
        flash = os.path.join(tmp, "flash.img")
        out = subprocess.run([gateway, "--capture", capture, "--baud", str(baud),
                              "--flash", flash, "--exit-when-done"],
                             capture_output=True, text=True, timeout=60, check=True).stdout
    print(out)

    stats = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].isdigit():
            stats[parts[0]] = int(parts[1])

    checks = [
        ("bytes_received + bytes_lost == file size",
         stats["bytes_received"] + stats["bytes_lost"] == m["bytes"]),
        ("bytes_lost == 0", stats["bytes_lost"] == 0),
        ("every intact reading found", stats["readings"] == m["intact_readings"]),
        ("every intact heartbeat found", stats["heartbeats"] == m["intact_heartbeats"]),
        ("readings_dropped == 0", stats["readings_dropped"] == 0),
        ("every reading stored", stats["readings_stored"] == stats["readings"]),
    ]
    ok = True
    for name, passed in checks:
        print(f"  {'ok  ' if passed else 'FAIL'} {name}")
        ok &= passed
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
