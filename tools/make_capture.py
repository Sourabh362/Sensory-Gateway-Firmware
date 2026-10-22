#!/usr/bin/env python3
"""Generate UART capture files (+ JSON manifest) for testing the gateway.

Our own stand-in for the brief's gen_frames.py (standard library only).
Frame: AA | LEN | TYPE | PAYLOAD | CRC16/CCITT-FALSE LE over LEN, TYPE, PAYLOAD.
"""
import argparse
import json
import random
import struct

PROFILES = {
    #         readings, heartbeat every N, noise probability, baud
    "clean": dict(readings=2000, hb_every=25, p_noise=0.0, baud=115200),
    "noisy": dict(readings=2714, hb_every=25, p_noise=0.35, baud=115200),
    "burst": dict(readings=19784, hb_every=100, p_noise=0.03, baud=921600),
}


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(ftype, payload):
    body = bytes([len(payload), ftype]) + payload
    return b"\xaa" + body + struct.pack("<H", crc16(body))


def reading(temp_centi, ts_ms):
    return frame(0x01, struct.pack("<hI", temp_centi, ts_ms))


def heartbeat():
    return frame(0x02, b"")


def noise(rng, temp, ts):
    """One damaged chunk. None of these may contain an intact frame."""
    kind = rng.choice(["junk", "truncated", "corrupted", "bad_len", "unknown"])
    if kind == "junk":  # random bytes, often 0xAA
        return bytes(0xAA if rng.random() < 0.3 else rng.randrange(256)
                     for _ in range(rng.randrange(1, 8)))
    if kind == "truncated":
        f = reading(temp, ts)
        return f[:rng.randrange(1, len(f) - 1)]  # keep at least 2 bytes missing
    if kind == "corrupted":
        f = bytearray(reading(temp, ts))
        i = rng.randrange(1, len(f))
        f[i] ^= 1 << rng.randrange(8)
        return bytes(f)
    if kind == "bad_len":
        return bytes([0xAA, rng.randrange(65, 256)]) + bytes(rng.randrange(256) for _ in range(3))
    return frame(rng.choice([0x03, 0x10, 0x7F]), bytes(rng.randrange(256) for _ in range(rng.randrange(0, 8))))


def generate(profile, seed):
    p = PROFILES[profile]
    rng = random.Random(seed)
    out = bytearray()
    temp, ts = 2200, 1000
    hb = 0
    for i in range(p["readings"]):
        if rng.random() < p["p_noise"]:
            out += noise(rng, temp, ts)
        temp = max(-4000, min(8500, temp + rng.randrange(-20, 21)))
        ts += rng.randrange(80, 120)
        out += reading(temp, ts)
        if (i + 1) % p["hb_every"] == 0:
            out += heartbeat()
            hb += 1
    manifest = {
        "profile": profile,
        "seed": seed,
        "baud": p["baud"],
        "bytes": len(out),
        "intact_readings": p["readings"],
        "intact_heartbeats": hb,
        "generator": "tools/make_capture.py",
    }
    return bytes(out), manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--profile", choices=PROFILES, required=True)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", required=True)
    ap.add_argument("--manifest", required=True)
    args = ap.parse_args()

    data, manifest = generate(args.profile, args.seed)
    with open(args.out, "wb") as f:
        f.write(data)
    with open(args.manifest, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"{args.out}: {manifest['bytes']} bytes, {manifest['intact_readings']} readings, "
          f"{manifest['intact_heartbeats']} heartbeats @ {manifest['baud']} baud")


if __name__ == "__main__":
    main()
